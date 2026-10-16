#pragma once

#include "IZapretBackend.h"

class QProcess;

namespace zapret {

// Linux execution backend.
//
// Two launch shapes are supported, mirroring what StrategyParser produces:
//
//   1. Service controller (Strategy::Kind::ShellScript == the spec's InitScript). The zapret sysv /
//      openrc init script is NOT a long-lived foreground process — `pkexec <script> start` brings up
//      a background daemon and returns. So this backend runs the short controller command and derives
//      state from its exit status: exit 0 => Running (the daemon is up), non-zero => Error. stop()
//      runs `pkexec <script> stop` and, on success, => Stopped.
//
//   2. Direct payload (Strategy::Kind::Binary == DirectBinary, e.g. a raw nfqws). This is a live
//      foreground process tracked by QProcess exactly like WindowsBackend does for winws.exe.
//
// Privileges: nfqws/init scripts need root. Elevation goes through pkexec, which authenticates via a
// polkit agent. If we are not already root and no agent is reachable, start() fails up front with a
// clear message rather than hanging on an unattended pkexec.
//
// Threading: single-threaded (GUI thread), no internal locking.
class LinuxBackend : public IZapretBackend
{
    Q_OBJECT

public:
    explicit LinuxBackend(QObject *parent = nullptr);
    ~LinuxBackend() override;

    bool start(const Strategy &s, QString *error) override;
    void stop() override;

    State state() const override { return m_state; }
    bool isRunning() const override;

    QString platformName() const override { return QStringLiteral("Linux"); }

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, bool crashed);
    void onProcessError();
    void onReadyReadStdout();
    void onReadyReadStderr();

private:
    void setState(State s);
    void resetProcess();

    // Build the controller command ("pkexec", {script, action}) or the direct command. Returns false
    // and fills *error on any synchronous validation failure (missing script, missing pkexec, ...).
    bool composeCommand(const Strategy &s, const QString &action, QString *program,
                        QStringList *args, QString *error);

    QProcess *m_process = nullptr;
    State m_state = State::Stopped;
    QString m_lastError;

    // The pending stop command for controller mode, run after the controller "start" completes so the
    // two short-lived invocations never overlap.
    bool m_controller = false;
    bool m_stopRequested = false;
    bool m_cmdIsStop = false;      // the currently running controller command is a "stop"
    // The direct payload was killed by stop(). A SIGTERM'd child reports CrashExit on Unix, and without
    // this marker a routine user-initiated stop would be shown as a crash.
    bool m_directStop = false;
    QString m_scriptPath;      // controller: the init script path (for the later "stop")
    QString m_controllerProg;  // controller: "pkexec" (reused for stop)

    void launchControllerStop();
};

} // namespace zapret
