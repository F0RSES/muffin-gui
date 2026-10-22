#pragma once

#include "IZapretBackend.h"
// Not a forward declaration: the member below is a QPointer<ZapretManager> and manager() converts it
// to ZapretManager* inline, which needs the complete type at every include site. Forward-declaring it
// left this header includable only in translation units that happened to pull ZapretManager.h first.
#include "ZapretManager.h"

#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <atomic>

class QThread;
class QTimer;

namespace zapret {

struct Strategy;

// The verdict of one host check — see BlockcheckEngine::checkHost().
//
// A check is `attempts` independent tries. One try counts as OK when the transport came up:
// the TCP connect landed and, when TLS was used, the handshake completed without a single ssl
// error. That last part is the reason this engine exists next to PingService: a DPI filter that
// lets SYN/ACK through but feeds the client a substituted certificate is "reachable" to a TCP
// probe and only caught by refusing the handshake — so an untrusted cert is a failed try, and
// isCertificateValid says whether any try saw one.
struct HostCheckResult
{
    // Majority of the attempts reached "transport up" (connect + TLS when used). A 1-of-3 pass is
    // deliberately not a pass: an intermittent hole is what "the bypass does not really work" looks
    // like from the user's chair.
    bool success = false;
    // Average ms from the first SYN to "transport up" over the OK attempts; -1 when none was OK.
    int avgLatency = -1;
    // TLS verdict. False the moment any attempt saw ssl errors (an untrusted/self-signed cert, the
    // shape of a substitution). Stays true when no TLS was attempted — there was nothing to falsify.
    bool isCertificateValid = true;
    // Average time-to-first-byte of the HTTP answer over the OK attempts; -1 when no server answered.
    // Measured opportunistically: a silent server does not fail a try, because connect+TLS already
    // is the verdict, and the number is a bonus the JSON carries when it exists.
    int ttfbMs = -1;
    int attemptsOk = 0;
    int attempts = 0;
    // Why the last failed attempt failed ("" when all passed, or nothing failed with a reason).
    QString lastError;
};

// "Does the bypass actually work?" as a reusable, GUI-free engine.
//
// PingService answers "is the host reachable at all"; this engine answers the harder question the
// profile page's «Проверить реальный обход» button and the console ask: bring a preset up through
// the same ZapretManager the power button uses, let the rules settle, then probe the services with
// a certificate-checking TLS probe and tear the preset back down. Living in zapretcore (QtCore +
// QtNetwork only) is the point — a router, a CI job or the future watchdog can run the identical
// measurement through muffin-cli without dragging QtWidgets along.
//
// Two front doors, one measurement:
//
//   * runTestForPreset() is the synchronous one for consoles and tests: it pumps nested event loops
//     on the calling thread (QCoreApplication is enough) and returns the finished JSON.
//   * startTest() is the asynchronous one for the GUI: identical phases, but the waits are timers
//     and signals and the blocking socket work runs on a private worker thread, so the caller's
//     event loop — the GUI's — is never held. Results arrive through hostChecked/progress and the
//     whole map through testFinished.
//
// checkHost() itself is blocking by nature (sliced waitFor* loops, like PingService's probes) and
// is public for consoles and tests; on the GUI path it only ever runs on the worker thread.
//
// Threading: one engine belongs to one thread's event loop. The tuning setters are refused while a
// test is busy precisely so the worker's reads of them stay race-free.
class BlockcheckEngine : public QObject
{
    Q_OBJECT

public:
    explicit BlockcheckEngine(QObject *parent = nullptr);
    ~BlockcheckEngine() override;

    // The manager the preset lifecycle goes through. Not owned; may stay null for checkHost-only
    // use (runTestForPreset/startTest then refuse with a lastError instead of guessing).
    void setManager(ZapretManager *manager);
    ZapretManager *manager() const { return m_manager; }

    // ---- tuning (refused while isBusy(), see the threading note) ----
    void setAttempts(int n);              // tries per host; default 3, floor 1
    int attempts() const { return m_attempts; }
    void setAttemptTimeoutMs(int ms);     // budget of each phase of one try; default 3000, floor 200
    int attemptTimeoutMs() const { return m_attemptTimeoutMs; }
    void setSettleDelayMs(int ms);        // pause after Running before probing; default 2500
    int settleDelayMs() const { return m_settleDelayMs; }
    void setStartTimeoutMs(int ms);       // how long the preset may take to come up; default 30000
    int startTimeoutMs() const { return m_startTimeoutMs; }
    void setStopTimeoutMs(int ms);        // how long the teardown may take; default 20000
    int stopTimeoutMs() const { return m_stopTimeoutMs; }
    void setPort(int port);               // what runTestForPreset/startTest probe; default 443
    int port() const { return m_port; }

    // ---- the measurement ----

    // One host, blocking. `attempts` tries; TLS is used when port is 443 (the port the DPI filters
    // watch) or when forceTls asks for it on another port — the flag exists for tests, which cannot
    // own 443 on a CI runner. Safe to call from any thread; never call it on the GUI thread.
    HostCheckResult checkHost(const QString &host, int port = 443, bool forceTls = false);

    // The synchronous full cycle for consoles/tests: start the preset (unless the manager already
    // runs one — that run is measured as-is and left alone), settle, probe every target, stop what
    // this call started. Returns {host -> result object}; an empty object plus lastError() means the
    // cycle itself failed (no manager, refused start, preset never came up). Pumps nested event
    // loops on the calling thread.
    QJsonObject runTestForPreset(const Strategy &strategy, const QStringList &targets);

    // The asynchronous full cycle for the GUI: same contract, delivered through signals. Returns
    // at once; refusal (busy, no manager, a start/stop transition already in flight) is reported
    // through errorOccurred + an empty testFinished rather than silently ignored.
    void startTest(const Strategy &strategy, const QStringList &targets);

    // Ask a running test to wind down: the in-flight try bails within one 100 ms slice, the cycle
    // then finishes normally (stopping what it started) with whatever results exist. Idempotent.
    void cancel();
    bool isBusy() const { return m_busy; }

    // Why the last synchronous cycle returned nothing, or the async one failed. Cleared per run.
    QString lastError() const { return m_lastError; }

    // The per-host JSON shape both front doors emit: success, latency, ttfb_ms, certificate_valid,
    // attempts_ok, attempts, and error when one is known. Static so a console can format a bare
    // checkHost() the same way without an engine of its own.
    static QJsonObject resultToJson(const HostCheckResult &r);

signals:
    // Coarse phase token for progress UIs: "starting", "settling", "probing", "stopping", "done".
    void stageChanged(const QString &stage);
    // After each host: how many are through, of how many, and which one just finished.
    void progress(int done, int total, const QString &host);
    // Per-host verdict, emitted from whichever thread did the work (queued into the GUI thread on
    // the async path — HostCheckResult is a declared metatype for exactly that crossing).
    void hostChecked(const QString &host, const zapret::HostCheckResult &result);
    // Same level contract as IZapretBackend: 0 info, 1 warn, 2 error.
    void logMessage(const QString &line, int level);
    // The cycle is over, however it ended: the full map, a partial one after cancel(), or {} when
    // the orchestration itself failed (lastError says why). Always the last word of a run.
    void testFinished(const QJsonObject &results);
    // The notification channel for a failed cycle; like the backend's, the text has already gone
    // through logMessage, so a listener wired to both must not show it twice.
    void errorOccurred(const QString &message);

private:
    enum class Phase
    {
        Idle,
        WaitRunning,   // async: start() accepted, Running not seen yet
        Settle,        // async: Running, letting the firewall rules land
        Probing,       // worker thread owns the sockets
        WaitStopped    // async: teardown asked for, Stopped not seen yet
    };

    // One try of one host. Blocking; sliced waitFor* waits polling m_cancel, the PingService way.
    bool probeOnce(const QString &host, quint16 port, bool useTls,
                   int *latencyMs, int *ttfbMs, bool *certValid, QString *error);

    // Synchronous-path waits: a nested QEventLoop on the calling thread, cut short by the wanted
    // states, the rejected ones, the deadline or cancel().
    bool waitForStates(const QSet<IZapretBackend::State> &ok,
                       const QSet<IZapretBackend::State> &bad, int timeoutMs);
    void waitMs(int ms);

    // Asynchronous-path machinery (all on the engine's own thread).
    void attachManager();
    void detachManager();
    void armDeadline(int ms);
    void startProbes();
    void cleanupProbeThread();
    void onManagerState(IZapretBackend::State s);
    void onHostCheckedInternal(const QString &host, const HostCheckResult &r);
    void onProbesDone();
    void failTest(const QString &message);
    void finishTest();

    QPointer<ZapretManager> m_manager;   // not owned; a QPointer because the window can die first

    int m_attempts = 3;
    int m_attemptTimeoutMs = 3000;
    int m_settleDelayMs = 2500;
    int m_startTimeoutMs = 30000;   // a pkexec prompt is answered by a human; be generous
    int m_stopTimeoutMs = 20000;
    int m_port = 443;

    bool m_busy = false;            // an async test owns the engine
    Phase m_phase = Phase::Idle;
    bool m_startedByUs = false;     // only a run this engine started may be stopped by it
    QStringList m_targets;
    QJsonObject m_collected;        // async accumulation, fed by onHostCheckedInternal
    QString m_lastError;
    QString m_asyncError;           // the manager's last errorOccurred while busy, for attribution

    QMetaObject::Connection m_stateConn;
    QMetaObject::Connection m_errorConn;
    QTimer *m_deadline = nullptr;   // single-shot, re-armed per phase
    QTimer *m_settleTimer = nullptr;
    QThread *m_probeThread = nullptr;   // owned; created per test, joined and deleted after

    std::atomic_bool m_cancel{false};
};

} // namespace zapret

Q_DECLARE_METATYPE(zapret::HostCheckResult)

