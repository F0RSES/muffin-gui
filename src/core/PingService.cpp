#include "PingService.h"

#include <QMutexLocker>

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace zapret {

namespace {
constexpr quint16 kProbePort = 443;
constexpr int kTcpBudgetMs = 3000;
constexpr int kHttpTimeoutMs = 5000;
constexpr int kSliceMs = 100;   // stop-latency granularity for every blocking wait
} // namespace

PingService::PingService(QObject *parent)
    : QObject(parent)
{
    // moveToThread() refuses a object with a parent, and a parented service would be destroyed on
    // the GUI thread while its timer still belonged to the worker.
    Q_ASSERT(parent == nullptr);

    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("PingService"));
    moveToThread(m_thread);

    m_hosts = defaultHosts();
    connect(m_thread, &QThread::finished, this, &PingService::shutdownWorker);
}

PingService::~PingService()
{
    m_stopRequested = true;

    if (m_thread && m_thread->isRunning()) {
        // Queued, so it runs before the Quit event that quit() posts — no deadlock if a probe is
        // mid-flight, and the timer is destroyed on the thread that owns it.
        QMetaObject::invokeMethod(this, "shutdownWorker", Qt::QueuedConnection);
        m_thread->quit();
        if (!m_thread->wait(3000)) {
            // Pathological case (a probe wedged in a uninterruptible DNS call): give up on grace.
            m_thread->terminate();
            m_thread->wait(1000);
        }
    }

    delete m_thread;
    m_thread = nullptr;
}

QStringList PingService::defaultHosts()
{
    return {QStringLiteral("youtube.com"), QStringLiteral("discord.com"), QStringLiteral("google.com")};
}

void PingService::setHosts(const QStringList &hosts)
{
    QMutexLocker lock(&m_mutex);
    m_hosts = hosts;
}

QStringList PingService::hosts() const
{
    QMutexLocker lock(&m_mutex);
    return m_hosts;
}

void PingService::setIntervalSec(int seconds)
{
    setIntervalMs(seconds * 1000);
}

void PingService::setIntervalMs(int ms)
{
    {
        QMutexLocker lock(&m_mutex);
        m_intervalMs = qMax(100, ms);
    }
    if (m_running) {
        // Re-arm the live timer on its own thread rather than from here.
        QMetaObject::invokeMethod(this, "startWorker", Qt::QueuedConnection);
    }
}

int PingService::intervalMs() const
{
    QMutexLocker lock(&m_mutex);
    return m_intervalMs;
}

void PingService::start()
{
    m_stopRequested = false;
    if (m_thread && !m_thread->isRunning())
        m_thread->start();
    QMetaObject::invokeMethod(this, "startWorker", Qt::QueuedConnection);
}

void PingService::stop()
{
    m_stopRequested = true;
    QMetaObject::invokeMethod(this, "stopWorker", Qt::QueuedConnection);
}

bool PingService::isRunning() const
{
    return m_running;
}

void PingService::pingNow()
{
    m_stopRequested = false;
    QMetaObject::invokeMethod(this, "tick", Qt::QueuedConnection);
}

// ---- worker-thread slots ----

void PingService::startWorker()
{
    if (!m_timer) {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &PingService::tick);
    }
    m_timer->stop();
    m_timer->start(intervalMs());
    m_running = true;
    m_stopRequested = false;
}

void PingService::stopWorker()
{
    m_stopRequested = true;
    if (m_timer)
        m_timer->stop();
    m_running = false;
}

void PingService::shutdownWorker()
{
    stopWorker();
    // Direct delete, not deleteLater(): this slot may run from QThread::finished, when the event
    // loop has already stopped and deferred deletion would never be processed. Both objects are
    // owned by this (worker) thread, so deleting here is legal.
    if (m_timer) {
        m_timer->stop();
        delete m_timer;
        m_timer = nullptr;
    }
    if (m_nam) {
        delete m_nam;
        m_nam = nullptr;
    }
}

void PingService::tick()
{
    const QStringList list = hosts();
    for (const QString &host : list) {
        if (m_stopRequested)
            return;

        emit pingUpdated(host, Waiting, -1);

        int latency = -1;
        bool up = probeTcp(host, &latency);
        if (!up && !m_stopRequested)
            up = probeHttp(host, &latency);

        if (m_stopRequested)
            return;
        emit pingUpdated(host, up ? Ok : Fail, up ? latency : -1);
    }
}

bool PingService::probeTcp(const QString &host, int *latencyMs)
{
    QTcpSocket socket;
    QElapsedTimer clock;
    clock.start();

    socket.connectToHost(host, kProbePort);

    // waitForConnected() in slices: the whole budget is spent waiting, but the stop flag is polled
    // every kSliceMs so shutdown stays prompt.
    while (!socket.waitForConnected(kSliceMs)) {
        if (m_stopRequested) {
            socket.abort();
            return false;
        }
        if (socket.error() != QAbstractSocket::UnknownSocketError) {
            socket.abort();
            return false;
        }
        if (clock.elapsed() >= kTcpBudgetMs) {
            socket.abort();
            return false;
        }
    }

    if (latencyMs)
        *latencyMs = static_cast<int>(clock.elapsed());
    socket.abort();
    return true;
}

bool PingService::probeHttp(const QString &host, int *latencyMs)
{
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);

    QNetworkRequest req{QUrl(QStringLiteral("https://%1/").arg(host))};
    req.setTransferTimeout(kHttpTimeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Mozilla/5.0 (compatible; Muffin/0.1; reachability probe)"));

    QElapsedTimer clock;
    clock.start();
    // QPointer, not a raw one: the nested loop below can destroy this reply out from under us —
    // see the check after exec().
    QPointer<QNetworkReply> reply{m_nam->head(req)};
    if (!reply)
        return false;

    // Nested loop on the worker thread only — the GUI thread never sees it. A 100 ms poller lets a
    // stop request cut the wait short even when the transfer timeout is still far away.
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer poller;
    poller.setInterval(kSliceMs);
    QObject::connect(&poller, &QTimer::timeout, &loop, [&loop, this] {
        if (m_stopRequested)
            loop.quit();
    });
    poller.start();
    loop.exec();
    poller.stop();

    // loop.exec() drains posted events, and one of them may be shutdownWorker(): it deletes m_nam,
    // which owns this reply, so by the time the nested loop returns the reply can already be gone.
    // Nothing below may touch it unchecked.
    if (!reply)
        return false;

    // Any HTTP status line at all means TCP+TLS succeeded, which is what the indicator claims; a
    // 403/404 is as good a sign of reachability as 200.
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool reachable = status > 0;

    if (reachable && latencyMs)
        *latencyMs = static_cast<int>(clock.elapsed());

    // The finished emission has fully unwound by the time exec() returns, so the reply can go now
    // (the worker thread may be about to stop, and deleteLater would then never run).
    reply->abort();
    delete reply.data();
    return reachable;
}

} // namespace zapret
