#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QTimer;

namespace zapret {

// Drives the Cloudflare WARP client through its own `warp-cli` binary.
//
// WARP is deliberately NOT an IZapretBackend. A backend owns one child process from launch to
// teardown and reports its exit; WARP's tunnel lives in a system daemon the user installed
// separately, and `warp-cli` is a short-lived control client that exits seconds after every verb.
// Reusing the backend contract here would either have to lie about "the process died" every time
// warp-cli exits normally, or keep a phantom process object alive for a run that is really a
// daemon's — both are the sort of thing that quietly breaks the power button's state machine.
//
// The manager therefore owns only:
//   * availability — is warp-cli on PATH?
//   * the current state, learned from `warp-cli --accept-tos status`
//   * one in-flight verb (connect / disconnect / status), issued asynchronously
//
// Everything on the CLI itself is delegated through QProcess, and the whole class is designed to
// be testable without the daemon: the argv the verbs run, the way the status answer is parsed,
// and the fallback when warp-cli is absent are all static functions a headless suite can call
// directly.
//
// Threading: like every other QObject here, WarpManager belongs to the GUI thread's event loop
// and is not internally locked.
class WarpManager : public QObject
{
    Q_OBJECT

public:
    // Coarse lifecycle state, chosen to map 1:1 onto the three looks the power button wears plus
    // one "there is nothing to press" case a backend never needs.
    enum class State
    {
        Unavailable,   // warp-cli is not installed on this host — the toggle cannot be used
        Disconnected,  // warp-cli answers, and the tunnel is down
        Connecting,    // a connect() is in flight, or the daemon says it is coming up
        Connected,     // the tunnel is up
        Disconnecting, // a disconnect() is in flight
        Error          // the last verb failed; errorOccurred carries the reason
    };
    Q_ENUM(State)

    explicit WarpManager(QObject *parent = nullptr);
    ~WarpManager() override;

    // ---- availability ---------------------------------------------------------
    // The name of the control program on this host. Kept as one word ("warp-cli") rather than an
    // absolute path: warp-cli lives under Program Files on Windows and /usr/bin elsewhere, and
    // QStandardPaths::findExecutable resolves the same name on both.
    static QString warpCliProgram();

    // Whether warp-cli resolves on this host right now. Testable form: pass the PATH-resolved
    // path — non-empty means available. The instance method caches the last answer so an
    // unavailable state does not re-probe PATH on every repaint.
    bool isAvailable() const { return m_available; }
    static bool isAvailableFromPath(const QString &resolvedPath);

    // ---- the argv each verb runs ---------------------------------------------
    //
    // Every one carries `--accept-tos` because warp-cli refuses to do anything at all without it
    // (and, in the words of its own man page, "prompts interactively" — which is exactly the
    // wrong behaviour for a GUI with nobody watching a hidden console). Split out so a test can
    // assert "the connect call is what we think it is" without needing warp-cli at all.
    static QStringList statusArgs();
    static QStringList connectArgs();
    static QStringList disconnectArgs();


    // ---- the parser -----------------------------------------------------------
    //
    // warp-cli's status output varies by version and platform: a Linux 2023.09 answer reads
    // "2024-... warp-cli [INFO] Connected", a Windows one writes "Status: Connected", and an
    // unregistered client says either "Disconnected" or "Not registered". What every flavour
    // agrees on is that the state word appears in the text, so this scans for the recognised
    // markers in a fixed order and falls back to Disconnected when nothing matches — a status
    // the manager cannot read is a status it should not claim.
    //
    // `wasConnectAttempt` is what makes "WARP is not registered, or is otherwise unavailable"
    // read as Connecting rather than Error when it appears during a verb we ourselves just ran;
    // the same text on a bare refresh means the daemon has nothing to say, which is closer to
    // "Disconnected" than to a failure of ours.
    static State parseStatus(const QString &raw, bool wasConnectAttempt = false);

    // ---- current view ---------------------------------------------------------
    State state() const { return m_state; }

    // ---- verbs ----------------------------------------------------------------
    // Each returns false when the operation could not even be started (warp-cli absent, or a
    // verb already in flight) and fills *error then; a successful return only means the request
    // was accepted, not that the tunnel moved. Completion lands through stateChanged.
    //
    // Named start/stop rather than connect/disconnect for two reasons: `connect` is a QObject
    // member, so a subclass that spells it that way shadows the base and every unqualified
    // `connect(...)` inside this file resolves to the wrong overload; and the rest of the
    // application already speaks start/stop about a tunnel — the power button, ZapretManager,
    // the tray menu — so a second vocabulary here would be a translation table waiting to
    // drift.
    bool start(QString *error = nullptr);
    bool stop(QString *error = nullptr);
    bool refresh(QString *error = nullptr);

    // Re-probe availability. Called on construction; a caller who just installed WARP while the
    // window was open can ask for the same effect without a full restart.
    void rescanAvailability();

    // Cadence-driven polling. Off by default; MainWindow turns it on only while a mode is
    // active, because a status probe on a machine with no warp-cli is a wasted fork every tick.
    void setPollingEnabled(bool on);
    bool isPollingEnabled() const { return m_pollEnabled; }
    // How often a poll fires while enabled. Exposed so a test can pin the cadence without
    // reaching into the private timer.
    static constexpr int kPollIntervalMs = 5000;

signals:
    void stateChanged(State s);
    void availabilityChanged(bool available);
    // Mirror of IZapretBackend's channel — the log line format the window already understands.
    // level: 0=info, 1=warn, 2=error
    void logMessage(const QString &line, int level);
    void errorOccurred(const QString &message);

private:
    bool startVerb(const QStringList &args, State onAccepted, bool wasConnectAttempt,
                   QString *error);
    void onVerbFinished();
    void setState(State s);

    State m_state = State::Unavailable;
    bool m_available = false;
    bool m_pollEnabled = false;
    QProcess *m_process = nullptr;   // the in-flight verb, non-null only while it runs
    QTimer *m_pollTimer = nullptr;
    // Whether the verb currently in flight is the one that could legitimately read an unknown
    // status as Connecting rather than Disconnected; see parseStatus().
    bool m_lastVerbWasConnect = false;
    // The state we expect the running verb to leave the daemon in, so a parse that returns
    // "Disconnected" while a connect() is still settling can be reported as Connecting.
    State m_pendingState = State::Disconnected;
};

} // namespace zapret
