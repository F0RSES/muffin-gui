#include "BlockcheckEngine.h"

#include "Strategy.h"
#include "ZapretManager.h"

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSslError>
#include <QSslSocket>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

namespace zapret {

namespace {

// Every blocking wait is spent in slices of this size, polling the cancel flag between them — the
// latency granularity PingService established, and the reason cancel() answers within ~100 ms even
// when a 3 s budget is still on the clock.
constexpr int kSliceMs = 100;

// The TTFB phase gets its own, shorter ceiling: connect+TLS already is the verdict, so a server
// that accepts and then says nothing must not add a full attempt budget to every try.
constexpr int kTtfbBudgetMs = 2000;

} // namespace

BlockcheckEngine::BlockcheckEngine(QObject *parent)
    : QObject(parent)
{
    // The per-host verdict crosses from the probe worker into this thread inside a queued signal;
    // registering it here keeps the first cross-thread emit from failing on an unknown type.
    qRegisterMetaType<HostCheckResult>("zapret::HostCheckResult");

    // The async accumulator: on the worker path hostChecked is emitted from the probe thread, so
    // this delivery is queued and lands here, on the engine's thread, strictly before the thread's
    // finished() that triggers onProbesDone() — event order is what makes m_collected complete
    // by the time the cycle ends. On the synchronous path it is a same-thread direct call, and
    // runTestForPreset builds its own map regardless.
    connect(this, &BlockcheckEngine::hostChecked,
            this, &BlockcheckEngine::onHostCheckedInternal);

    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this] {
        if (m_phase == Phase::WaitRunning) {
            QString why = QStringLiteral("Blockcheck: the preset did not come up within %1 s")
                              .arg(m_startTimeoutMs / 1000);
            if (!m_asyncError.isEmpty())
                why += QStringLiteral(" (%1)").arg(m_asyncError);
            failTest(why);
        } else if (m_phase == Phase::WaitStopped) {
            // The backend has its own rescue deadline; outlasting it means something is wedged in
            // a way this engine cannot fix. Report and release rather than hold the UI forever.
            emit logMessage(QStringLiteral("Blockcheck: the teardown did not confirm within %1 s; "
                                           "releasing the test")
                                .arg(m_stopTimeoutMs / 1000), 1);
            finishTest();
        }
    });

    m_settleTimer = new QTimer(this);
    m_settleTimer->setSingleShot(true);
    connect(m_settleTimer, &QTimer::timeout, this, [this] {
        if (m_busy && m_phase == Phase::Settle)
            startProbes();
    });
}

BlockcheckEngine::~BlockcheckEngine()
{
    m_cancel = true;
    cleanupProbeThread();
    detachManager();
}

void BlockcheckEngine::setManager(ZapretManager *manager)
{
    if (m_manager == manager)
        return;
    detachManager();
    m_manager = manager;
}

// The tuning setters share one guard: the probe worker reads these plain ints without a lock, which
// is only sound while nothing can move them mid-run.
#define BLOCKCHECK_SETTER_GUARD if (m_busy) return;

void BlockcheckEngine::setAttempts(int n)
{
    BLOCKCHECK_SETTER_GUARD
    m_attempts = qMax(1, n);
}

void BlockcheckEngine::setAttemptTimeoutMs(int ms)
{
    BLOCKCHECK_SETTER_GUARD
    m_attemptTimeoutMs = qMax(200, ms);
}

void BlockcheckEngine::setSettleDelayMs(int ms)
{
    BLOCKCHECK_SETTER_GUARD
    m_settleDelayMs = qMax(0, ms);
}

void BlockcheckEngine::setStartTimeoutMs(int ms)
{
    BLOCKCHECK_SETTER_GUARD
    m_startTimeoutMs = qMax(1000, ms);
}

void BlockcheckEngine::setStopTimeoutMs(int ms)
{
    BLOCKCHECK_SETTER_GUARD
    m_stopTimeoutMs = qMax(1000, ms);
}

void BlockcheckEngine::setPort(int port)
{
    BLOCKCHECK_SETTER_GUARD
    if (port > 0 && port <= 65535)
        m_port = port;
}

#undef BLOCKCHECK_SETTER_GUARD

QJsonObject BlockcheckEngine::resultToJson(const HostCheckResult &r)
{
    QJsonObject o;
    o.insert(QLatin1String("success"), r.success);
    o.insert(QLatin1String("latency"), r.avgLatency);
    o.insert(QLatin1String("ttfb_ms"), r.ttfbMs);
    o.insert(QLatin1String("certificate_valid"), r.isCertificateValid);
    o.insert(QLatin1String("attempts_ok"), r.attemptsOk);
    o.insert(QLatin1String("attempts"), r.attempts);
    if (!r.lastError.isEmpty())
        o.insert(QLatin1String("error"), r.lastError);
    return o;
}

// ---- the measurement ----

HostCheckResult BlockcheckEngine::checkHost(const QString &host, int port, bool forceTls)
{
    HostCheckResult res;
    const int n = qMax(1, m_attempts);
    res.attempts = n;

    const QString target = host.trimmed();
    if (target.isEmpty()) {
        res.lastError = QStringLiteral("empty host");
        return res;
    }
    if (port <= 0 || port > 65535) {
        res.lastError = QStringLiteral("invalid port %1").arg(port);
        return res;
    }

    // Port 443 is where the DPI filters live, so that is where the handshake (and with it the
    // certificate) is part of the verdict; anywhere else a bare TCP answer is the whole claim.
    const bool useTls = forceTls || port == 443;

    qint64 latencySum = 0;
    qint64 ttfbSum = 0;
    int ttfbCount = 0;
    for (int i = 0; i < n; ++i) {
        if (m_cancel) {
            if (res.lastError.isEmpty())
                res.lastError = QStringLiteral("cancelled");
            break;
        }
        int latency = -1;
        int ttfb = -1;
        bool certValid = true;
        QString err;
        const bool ok = probeOnce(target, quint16(port), useTls, &latency, &ttfb, &certValid, &err);
        if (!certValid)
            res.isCertificateValid = false;
        if (ok) {
            ++res.attemptsOk;
            latencySum += latency;
            if (ttfb >= 0) {
                ttfbSum += ttfb;
                ++ttfbCount;
            }
        } else if (!err.isEmpty()) {
            res.lastError = err;
        }
    }

    // Majority, not "any": one lucky try out of three is an intermittent hole, which is exactly the
    // failure mode this check exists to call out.
    res.success = res.attemptsOk >= (n + 1) / 2;
    if (res.attemptsOk > 0)
        res.avgLatency = static_cast<int>(latencySum / res.attemptsOk);
    if (ttfbCount > 0)
        res.ttfbMs = static_cast<int>(ttfbSum / ttfbCount);
    return res;
}

// One try of one host: connect (TLS + certificate verdict when asked), then a best-effort HTTP
// exchange for the TTFB number. Every wait is a slice loop over the cancel flag; the sockets are
// stack objects of the calling thread, so this function is safe on the probe worker and in a
// console alike, and re-entrant by construction.
bool BlockcheckEngine::probeOnce(const QString &host, quint16 port, bool useTls,
                                 int *latencyMs, int *ttfbMs, bool *certValid, QString *error)
{
    const int budget = qMax(200, m_attemptTimeoutMs);
    const int ttfbBudget = qMin(budget, kTtfbBudgetMs);

    QSslSocket sslSock;
    QTcpSocket tcpSock;
    QAbstractSocket *sock = useTls ? static_cast<QAbstractSocket *>(&sslSock)
                                   : static_cast<QAbstractSocket *>(&tcpSock);

    QList<QSslError> sslErrors;
    if (useTls) {
        // QueryPeer, and the veto is applied here rather than by the TLS backend.
        //
        // VerifyPeer makes Qt ask the backend to verify inside the handshake, so the connection dies
        // in SSL_do_handshake and QSslSocket::sslErrors is never emitted at all on some platforms —
        // observed: it fires on Linux, it does not on Windows/MSVC. A substituted certificate then
        // reads as an opaque "TLS handshake failed: <words that vary per OS and per OpenSSL build>",
        // which is the one verdict this engine must never be vague about: the whole point of the probe
        // is that a DPI handing back its own certificate is named as such.
        //
        // QueryPeer lets the handshake complete and leaves Qt to verify the chain afterwards, which
        // emits sslErrors with structured QSslError objects on every backend. The probe then fails the
        // try itself, so the certificate case is reported identically everywhere and no longer depends
        // on whether a given backend chose to abort.
        sslSock.setPeerVerifyMode(QSslSocket::QueryPeer);
        QObject::connect(&sslSock, &QSslSocket::sslErrors, &sslSock,
                         [&sslErrors](const QList<QSslError> &errors) { sslErrors.append(errors); });
    }

    QElapsedTimer clock;
    clock.start();
    if (useTls)
        sslSock.connectToHostEncrypted(host, port);
    else
        sock->connectToHost(host, port);

    while (!sock->waitForConnected(kSliceMs)) {
        if (m_cancel) {
            sock->abort();
            if (error) *error = QStringLiteral("cancelled");
            return false;
        }
        if (sock->error() != QAbstractSocket::UnknownSocketError) {
            if (error) *error = sock->errorString();
            sock->abort();
            return false;
        }
        if (clock.elapsed() >= budget) {
            if (error) *error = QStringLiteral("connect timed out after %1 ms").arg(budget);
            sock->abort();
            return false;
        }
    }

    if (useTls) {
        QElapsedTimer hsClock;
        hsClock.start();
        while (!sslSock.isEncrypted()) {
            if (m_cancel)
                break;
            if (sslSock.waitForEncrypted(kSliceMs))
                break;
            if (sslSock.isEncrypted())
                break;
            // A set error is the handshake having failed for good (no certificate arrived, the peer
            // went away, the cipher suites disagree); a bare false with no error is only this slice's
            // timeout, so loop on.
            if (sslSock.error() != QAbstractSocket::UnknownSocketError)
                break;
            if (hsClock.elapsed() >= budget)
                break;
        }
        if (!sslSock.isEncrypted()) {
            // abort() resets the socket's recorded error, so both halves of the diagnosis are
            // captured before it runs.
            const QAbstractSocket::SocketError sockErr = sslSock.error();
            const QString errText = sslSock.errorString();
            sslSock.abort();
            if (m_cancel) {
                if (error) *error = QStringLiteral("cancelled");
                return false;
            }
            if (!sslErrors.isEmpty()) {
                // Belt and braces: QueryPeer leaves the certificate verdict to the veto below, but a
                // backend that aborts anyway still leaves the errors on record, and a certificate is
                // the reason that outranks every other — keep naming it here too.
                if (certValid) *certValid = false;
                if (error)
                    *error = QStringLiteral("TLS certificate rejected: %1")
                                 .arg(sslErrors.first().errorString());
            } else if (sockErr != QAbstractSocket::UnknownSocketError) {
                if (error)
                    *error = QStringLiteral("TLS handshake failed: %1").arg(errText);
            } else {
                if (error)
                    *error = QStringLiteral("TLS handshake timed out after %1 ms").arg(budget);
            }
            return false;
        }
        // The certificate veto (see QueryPeer above). A handshake that completed with verification
        // errors on record is a failed probe, not a passed one: an untrusted or substituted chain is
        // the event this engine exists to report. This returns before latency is recorded, so a
        // rejected certificate claims no transport — the same shape as a refused handshake.
        if (!sslErrors.isEmpty()) {
            if (certValid)
                *certValid = false;
            if (error)
                *error = QStringLiteral("TLS certificate rejected: %1")
                             .arg(sslErrors.first().errorString());
            sslSock.abort();
            return false;
        }
    }

    // "Transport up" — the number the latency claim is about. DNS, SYN/ACK and, when asked, the
    // whole handshake are inside it; the optional HTTP exchange below deliberately is not.
    if (latencyMs)
        *latencyMs = static_cast<int>(clock.elapsed());

    // TTFB, opportunistically: one small request, then wait for the first byte. A server that
    // never answers costs at most ttfbBudget and does not fail the try — connect+TLS already is
    // the verdict this engine claims.
    const QByteArray request = QByteArrayLiteral("HEAD / HTTP/1.1\r\nHost: ") + host.toUtf8()
        + QByteArrayLiteral("\r\nUser-Agent: Muffin-blockcheck/1.0\r\nConnection: close\r\n\r\n");
    sock->write(request);
    if (sock->waitForBytesWritten(kSliceMs)) {
        QElapsedTimer ttfbClock;
        ttfbClock.start();
        while (sock->bytesAvailable() == 0) {
            if (m_cancel)
                break;
            if (!sock->waitForReadyRead(kSliceMs)) {
                if (sock->bytesAvailable() > 0)
                    break;
                if (sock->state() != QAbstractSocket::ConnectedState)
                    break;   // the peer closed without saying anything: no TTFB, not a failure
                if (ttfbClock.elapsed() >= ttfbBudget)
                    break;
            }
        }
        if (sock->bytesAvailable() > 0 && ttfbMs)
            *ttfbMs = static_cast<int>(ttfbClock.elapsed());
    }

    sock->abort();
    return true;
}

// ---- the synchronous cycle (consoles, tests) ----

bool BlockcheckEngine::waitForStates(const QSet<IZapretBackend::State> &ok,
                                     const QSet<IZapretBackend::State> &bad, int timeoutMs)
{
    if (!m_manager)
        return false;
    if (ok.contains(m_manager->state()))
        return true;

    QEventLoop loop;
    bool result = false;
    QMetaObject::Connection conn = connect(
        m_manager.data(), &ZapretManager::stateChanged, &loop,
        [&](IZapretBackend::State s) {
            if (ok.contains(s)) {
                result = true;
                loop.quit();
            } else if (bad.contains(s)) {
                result = false;
                loop.quit();
            }
        });

    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, &loop, [&] {
        result = m_manager && ok.contains(m_manager->state());
        loop.quit();
    });
    // The cancel poll: waitForStates is the synchronous path's only long wait besides the sockets
    // themselves, and cancel() has to cut it just like it cuts a probe slice.
    QTimer poll;
    poll.setInterval(kSliceMs);
    connect(&poll, &QTimer::timeout, &loop, [&] {
        if (m_cancel) {
            result = false;
            loop.quit();
        }
    });
    deadline.start(timeoutMs);
    poll.start();
    loop.exec();
    disconnect(conn);
    return result;
}

void BlockcheckEngine::waitMs(int ms)
{
    if (ms <= 0)
        return;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QTimer poll;
    poll.setInterval(kSliceMs);
    connect(&poll, &QTimer::timeout, &loop, [&] {
        if (m_cancel)
            loop.quit();
    });
    deadline.start(ms);
    poll.start();
    loop.exec();
}

QJsonObject BlockcheckEngine::runTestForPreset(const Strategy &strategy, const QStringList &targets)
{
    QJsonObject out;
    m_lastError.clear();

    auto refuse = [this, &out](const QString &why) -> QJsonObject {
        m_lastError = why;
        emit logMessage(why, 2);
        emit stageChanged(QStringLiteral("done"));
        emit testFinished(out);
        return out;
    };

    if (!m_manager)
        return refuse(QStringLiteral("Blockcheck: no ZapretManager attached"));
    if (targets.isEmpty())
        return refuse(QStringLiteral("Blockcheck: no target hosts given"));
    if (m_busy)
        return refuse(QStringLiteral("Blockcheck: an asynchronous test owns the engine"));

    m_cancel = false;

    // Only a run this call started may be stopped by it: measuring a session the user brought up
    // by hand and then tearing it down would be the check deciding things about the user's network.
    bool startedByUs = false;
    if (m_manager->isRunning()) {
        emit logMessage(QStringLiteral("Blockcheck: measuring the run that is already up"), 0);
    } else {
        emit stageChanged(QStringLiteral("starting"));
        QString err;
        if (!m_manager->start(strategy, &err)) {
            return refuse(err.isEmpty() ? QStringLiteral("Blockcheck: the backend refused the start")
                                        : err);
        }
        startedByUs = true;
        if (!waitForStates({IZapretBackend::State::Running},
                           {IZapretBackend::State::Error, IZapretBackend::State::Stopped},
                           m_startTimeoutMs)) {
            // Leave nothing behind: a half-started preset is exactly what the user did not ask for.
            if (m_manager->state() == IZapretBackend::State::Running
                || m_manager->state() == IZapretBackend::State::Starting) {
                m_manager->stop();
                waitForStates({IZapretBackend::State::Stopped, IZapretBackend::State::Error}, {},
                              m_stopTimeoutMs);
            }
            return refuse(m_cancel
                              ? QStringLiteral("Blockcheck: cancelled")
                              : QStringLiteral("Blockcheck: the preset did not reach the running state"));
        }
        emit stageChanged(QStringLiteral("settling"));
        emit logMessage(QStringLiteral("Blockcheck: letting the rules settle (%1 ms)")
                            .arg(m_settleDelayMs), 0);
        waitMs(m_settleDelayMs);
        if (m_cancel) {
            m_manager->stop();
            waitForStates({IZapretBackend::State::Stopped, IZapretBackend::State::Error}, {},
                          m_stopTimeoutMs);
            return refuse(QStringLiteral("Blockcheck: cancelled"));
        }
    }

    emit stageChanged(QStringLiteral("probing"));
    int done = 0;
    for (const QString &host : targets) {
        if (m_cancel)
            break;
        const HostCheckResult r = checkHost(host, m_port);
        out.insert(host, resultToJson(r));
        emit hostChecked(host, r);
        emit progress(++done, targets.size(), host);
    }

    if (startedByUs) {
        emit stageChanged(QStringLiteral("stopping"));
        m_manager->stop();
        if (!waitForStates({IZapretBackend::State::Stopped, IZapretBackend::State::Error}, {},
                           m_stopTimeoutMs))
            emit logMessage(QStringLiteral("Blockcheck: the teardown did not confirm within %1 s")
                                .arg(m_stopTimeoutMs / 1000), 1);
    }

    if (m_cancel && m_lastError.isEmpty())
        m_lastError = QStringLiteral("cancelled");
    emit stageChanged(QStringLiteral("done"));
    emit testFinished(out);
    return out;
}

// ---- the asynchronous cycle (GUI) ----

void BlockcheckEngine::attachManager()
{
    if (!m_manager || m_stateConn)
        return;
    m_stateConn = connect(m_manager.data(), &ZapretManager::stateChanged,
                          this, [this](IZapretBackend::State s) { onManagerState(s); });
    m_errorConn = connect(m_manager.data(), &ZapretManager::errorOccurred,
                          this, [this](const QString &message) { m_asyncError = message; });
}

void BlockcheckEngine::detachManager()
{
    if (m_stateConn)
        disconnect(m_stateConn);
    if (m_errorConn)
        disconnect(m_errorConn);
    m_stateConn = QMetaObject::Connection();
    m_errorConn = QMetaObject::Connection();
}

void BlockcheckEngine::armDeadline(int ms)
{
    m_deadline->start(ms);
}

void BlockcheckEngine::startTest(const Strategy &strategy, const QStringList &targets)
{
    auto refuse = [this](const QString &why) {
        m_lastError = why;
        emit logMessage(why, 2);
        emit errorOccurred(why);
        emit stageChanged(QStringLiteral("done"));
        emit testFinished(QJsonObject());
    };

    if (m_busy) {
        refuse(QStringLiteral("Blockcheck: a test is already running"));
        return;
    }
    if (!m_manager) {
        refuse(QStringLiteral("Blockcheck: no ZapretManager attached"));
        return;
    }
    if (targets.isEmpty()) {
        refuse(QStringLiteral("Blockcheck: no target hosts given"));
        return;
    }
    const IZapretBackend::State st = m_manager->state();
    if (st == IZapretBackend::State::Starting || st == IZapretBackend::State::Stopping) {
        // The backend owns one operation at a time; starting a preset under a transition in flight
        // is the same refusal the power button gets.
        refuse(QStringLiteral("Blockcheck: another operation is already in progress"));
        return;
    }

    m_cancel = false;
    m_busy = true;
    m_collected = QJsonObject();
    m_targets = targets;
    m_lastError.clear();
    m_asyncError.clear();
    m_startedByUs = false;

    attachManager();

    if (st == IZapretBackend::State::Running) {
        // Measure what is already up and leave its lifecycle alone — the user started it, the user
        // stops it. The log line below is how the GUI says so.
        emit logMessage(QStringLiteral("Blockcheck: measuring the run that is already up"), 0);
        startProbes();
        return;
    }

    emit stageChanged(QStringLiteral("starting"));
    m_startedByUs = true;
    m_phase = Phase::WaitRunning;   // set before start(): a backend may emit synchronously
    QString err;
    if (!m_manager->start(strategy, &err)) {
        failTest(err.isEmpty() ? QStringLiteral("Blockcheck: the backend refused the start") : err);
        return;
    }
    if (m_manager->state() == IZapretBackend::State::Running) {
        onManagerState(IZapretBackend::State::Running);   // it beat its own signal
        return;
    }
    armDeadline(m_startTimeoutMs);
}

void BlockcheckEngine::cancel()
{
    m_cancel = true;
    if (!m_busy)
        return;
    // The probing worker polls the flag between hosts and inside every wait slice, so that phase
    // winds itself down. The waits around it need a push: they are timers and signals on this
    // thread, and the honest end of a cancelled cycle is the same teardown as a finished one.
    if (m_phase == Phase::WaitRunning || m_phase == Phase::Settle)
        failTest(QStringLiteral("Blockcheck: cancelled"));
}

void BlockcheckEngine::onManagerState(IZapretBackend::State s)
{
    using S = IZapretBackend::State;
    switch (m_phase) {
    case Phase::WaitRunning:
        if (s == S::Running) {
            m_deadline->stop();
            m_phase = Phase::Settle;
            emit stageChanged(QStringLiteral("settling"));
            emit logMessage(QStringLiteral("Blockcheck: letting the rules settle (%1 ms)")
                                .arg(m_settleDelayMs), 0);
            m_settleTimer->start(m_settleDelayMs);
        } else if (s == S::Error) {
            failTest(m_asyncError.isEmpty()
                         ? QStringLiteral("Blockcheck: the preset failed to start")
                         : m_asyncError);
        } else if (s == S::Stopped) {
            failTest(QStringLiteral("Blockcheck: the start was aborted"));
        }
        break;
    case Phase::WaitStopped:
        if (s == S::Stopped || s == S::Error) {
            m_deadline->stop();
            finishTest();
        }
        break;
    default:
        // Probing: a user's Stop press mid-test is theirs to make; the probes then simply fail and
        // the cycle ends with the partial map it has. Settle/Idle: nothing to drive.
        break;
    }
}

void BlockcheckEngine::onHostCheckedInternal(const QString &host, const HostCheckResult &r)
{
    m_collected.insert(host, resultToJson(r));
}

void BlockcheckEngine::startProbes()
{
    m_phase = Phase::Probing;
    emit stageChanged(QStringLiteral("probing"));

    cleanupProbeThread();
    const QStringList targets = m_targets;
    const int total = targets.size();
    // QThread::create, not a moved worker object: the whole job is one loop over hosts calling the
    // re-entrant checkHost(), and every verdict leaves as a queued signal on this engine — there is
    // no worker state to own. The thread object stays owned by the engine (cleanupProbeThread joins
    // and deletes it), so nothing here outlives the run.
    m_probeThread = QThread::create([this, targets, total] {
        int done = 0;
        for (const QString &host : targets) {
            if (m_cancel)
                break;
            const HostCheckResult r = checkHost(host, m_port);
            emit hostChecked(host, r);
            emit progress(++done, total, host);
        }
    });
    // finished() is emitted from the worker after run() returns; the queued hostChecked deliveries
    // were posted earlier, so by the time onProbesDone runs on this thread m_collected is complete.
    connect(m_probeThread, &QThread::finished, this, &BlockcheckEngine::onProbesDone);
    m_probeThread->start();
}

void BlockcheckEngine::cleanupProbeThread()
{
    if (!m_probeThread)
        return;
    if (!m_probeThread->wait(15000)) {
        // Every wait inside is a 100 ms slice over m_cancel, which cancel() and the destructor set;
        // reaching this branch means the OS itself is holding the thread — a wedged DNS call is the
        // known shape, and PingService documents the same last resort.
        m_probeThread->terminate();
        m_probeThread->wait(1000);
    }
    delete m_probeThread;
    m_probeThread = nullptr;
}

void BlockcheckEngine::onProbesDone()
{
    if (!m_busy || m_phase != Phase::Probing)
        return;   // a late finished() from a thread this run already reaped

    cleanupProbeThread();

    if (m_startedByUs && m_manager
        && (m_manager->state() == IZapretBackend::State::Running
            || m_manager->state() == IZapretBackend::State::Starting)) {
        emit stageChanged(QStringLiteral("stopping"));
        m_phase = Phase::WaitStopped;   // set before stop(): a backend may confirm synchronously
        m_manager->stop();
        if (m_manager->state() == IZapretBackend::State::Stopped
            || m_manager->state() == IZapretBackend::State::Error) {
            finishTest();
            return;
        }
        armDeadline(m_stopTimeoutMs);
        return;
    }
    finishTest();
}

void BlockcheckEngine::failTest(const QString &message)
{
    if (!m_busy)
        return;
    m_lastError = message;
    m_busy = false;
    m_phase = Phase::Idle;
    m_deadline->stop();
    m_settleTimer->stop();
    detachManager();

    // Leave the machine as the user had it: a preset this test brought up does not survive the test.
    if (m_startedByUs && m_manager && m_manager->isRunning()) {
        emit logMessage(QStringLiteral("Blockcheck: stopping the preset the failed test started"), 1);
        m_manager->stop();
    }
    m_startedByUs = false;

    // Wind a probe worker down if one is out there: the flag is what its slices poll, and the join
    // is bounded by those same slices. Reset afterwards so the next run starts with a clean flag.
    m_cancel = true;
    cleanupProbeThread();
    m_cancel = false;

    emit logMessage(message, 2);
    emit errorOccurred(message);
    emit stageChanged(QStringLiteral("done"));
    emit testFinished(m_collected);
}

void BlockcheckEngine::finishTest()
{
    if (!m_busy)
        return;
    m_busy = false;
    m_phase = Phase::Idle;
    m_startedByUs = false;
    m_deadline->stop();
    m_settleTimer->stop();
    detachManager();
    cleanupProbeThread();

    if (m_cancel && m_lastError.isEmpty())
        m_lastError = QStringLiteral("cancelled");
    emit stageChanged(QStringLiteral("done"));
    emit testFinished(m_collected);
}

} // namespace zapret
