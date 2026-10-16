#pragma once

#include <QMutex>
#include <QObject>
#include <QStringList>

#include <atomic>

class QNetworkAccessManager;
class QThread;
class QTimer;

namespace zapret {

// Reachability prober for the status bar, designed to run entirely off the GUI thread.
//
// A PingService moves itself onto a private QThread in its constructor and does all of its work in
// that thread's event loop: the GUI only ever posts requests (start/stop/pingNow/setHosts are
// thread-safe forwarders). Each cycle emits pingUpdated(host, Waiting) and then the outcome per
// host, so an indicator lights up amber immediately instead of waiting for every probe to finish.
//
// Probe strategy per host:
//   1. TCP connect to port 443 with a 3 s budget  -> latency = connect time
//   2. if that fails, HEAD https://<host> with a 5 s transfer timeout -> "reachable" if any HTTP
//      answer came back (a 403/404 still proves the path works, which is all the indicator claims)
// The blocking waits happen inside the worker thread and are polled in 100 ms slices against the
// stop flag, so stop()/destruction never waits longer than about one slice.
//
// Threading note: the object must be constructed parentless (moveToThread requirement).
class PingService : public QObject
{
    Q_OBJECT

public:
    // Values match the int status carried by pingUpdated and the UI's indicator mapping.
    enum Status
    {
        Waiting = 0,
        Ok      = 1,
        Fail    = 2
    };
    Q_ENUM(Status)

    explicit PingService(QObject *parent = nullptr);
    ~PingService() override;

    // The three hosts the GUI checks when the user has not configured anything: the services zapret
    // is most often used to unblock.
    static QStringList defaultHosts();

    void setHosts(const QStringList &hosts);
    QStringList hosts() const;

    void setIntervalSec(int seconds);
    void setIntervalMs(int ms);              // tests use a short cycle
    int intervalMs() const;

    // Starts the worker thread (if needed) and the periodic cycle. Idempotent.
    void start();
    // Stops the cycle; the in-flight probe bails out within one 100 ms slice. Idempotent.
    void stop();
    bool isRunning() const;

    // Run one cycle immediately without disturbing the periodic one.
    void pingNow();

signals:
    // status is a PingService::Status; latencyMs is -1 unless status == Ok.
    void pingUpdated(QString host, int status, int latencyMs);

private slots:
    void startWorker();
    void stopWorker();
    void shutdownWorker();
    void tick();

private:
    bool probeTcp(const QString &host, int *latencyMs);
    bool probeHttp(const QString &host, int *latencyMs);

    QThread *m_thread = nullptr;
    QTimer *m_timer = nullptr;                  // worker-thread only, deleted there too
    QNetworkAccessManager *m_nam = nullptr;      // worker-thread only

    mutable QMutex m_mutex;                      // guards m_hosts + m_intervalMs (cross-thread reads)
    QStringList m_hosts;
    int m_intervalMs = 5000;

    std::atomic_bool m_stopRequested{false};
    std::atomic_bool m_running{false};
};

} // namespace zapret
