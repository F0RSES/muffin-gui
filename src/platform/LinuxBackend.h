#pragma once

#include "IZapretBackend.h"
#include "PortableMode.h"

class QProcess;
class QTimer;

namespace zapret {

// Linux execution backend.
//
// Three launch shapes are supported, mirroring what StrategyParser produces:
//
//   1. Service controller (Strategy::Kind::ShellScript / RunMode::InitScript == the spec's InitScript).
//      The zapret sysv / openrc init script is NOT a long-lived foreground process — `pkexec <script>
//      start` brings up a background daemon and returns. So this backend runs the short controller
//      command and derives state from its exit status: exit 0 => Running (the daemon is up), non-zero
//      => Error. stop() runs `pkexec <script> stop` and, on success, => Stopped.
//
//   2. Direct payload (Strategy::Kind::Binary with RunMode::Default). This is a live foreground
//      process tracked by QProcess exactly like WindowsBackend does for winws.exe. The GUI supplies the
//      firewall setup itself — the backend touches nothing outside the child process.
//
//   3. Portable payload (RunMode::PortableNfqws). Same live child as (2), but the backend also owns the
//      nfqueue rules around it, because an unpacked zapret tree has no installer to do it and nfqws
//      without a queue does nothing. start() installs the rules, then execs nfqws; stop() terminates
//      nfqws and removes the rules. See startPortable()/stopPortable() for the sequencing and
//      portableFirewallScript() for exactly what is written into the kernel.
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

    // A privileged cleanup step the stop is waiting on has not answered within its deadline. Releasing
    // the run here is what keeps "Stop" from being a one-way door into a frozen GUI — see forceStop().
    void onStopWatchdogFired();

private:
    void setState(State s);
    void resetProcess();

    // Build the controller command ("pkexec", {script, action}) or the direct command. Returns false
    // and fills *error on any synchronous validation failure (missing script, missing pkexec, ...).
    bool composeCommand(const Strategy &s, const QString &action, QString *program,
                        QStringList *args, QString *error);

    // ---- RunMode::PortableNfqws ----
    //
    // Three commands on a run that behaves, a fourth when a stop goes unanswered — always one QProcess
    // at a time, sequenced by m_phase:
    //
    //   FirewallUp   pkexec /bin/sh -c '<idempotent iptables insert>'    (short-lived)
    //   Payload      pkexec <nfqws> <argv...>                             (long-lived child)
    //   FirewallDown pkexec /bin/sh -c '<idempotent iptables delete>'    (short-lived)
    //   Rescue       pkexec /bin/sh -c '<pkill sweep + the same delete>'  (see forceStop)
    //
    // The payload only ever starts once its queue exists: nfqws binding a queue nobody enqueues into
    // is useless, and an nfqws that dies with the rules already installed is safe precisely because
    // every rule carries --queue-bypass.

    // Assemble the batched, idempotent firewall command (apply==true inserts, false deletes).
    //
    // Wrapping is the shared `wrapPrivilegedScript()` precedence the rescue command uses too —
    // $ZAPRET_GUI_FIREWALL_PROG, else /bin/sh when already root, else pkexec /bin/sh -c <script> — so
    // editing the firewall and cleaning it up after a stuck stop can never disagree about how root is
    // reached, and one prompt covers a whole phase.
    bool composeFirewallCommand(bool apply, QString *program, QStringList *args, QString *error) const;

    void launchPortableFirewall(bool apply);
    void launchPortablePayload();
    void startPortable(const Strategy &s);        // firewall-up, then payload on success
    void stopPortable();
    void onPortableFinished(int code, bool crashed);

    // Surface a portable-mode failure. When the rules are installed the report is DEFERRED: the
    // message is parked in m_portablePendingError and emitted once the teardown command finishes,
    // because a queue that nothing services must not be left behind merely because something failed.
    // m_fwApplied is false in every failure path that cannot reach teardown, so this never loops.
    void portableFail(const QString &msg);

    // ---- forced stop (the anti-hang path) ----
    //
    // Every launch shape here can put the payload one level deeper than the process we own: the child
    // QProcess controls is pkexec, and pkexec does not forward signals to the nfqws it spawned. The
    // surviving grandchild also keeps the write end of the stdout/stderr pipes QProcess reads, and Qt
    // withholds finished() until those pipes reach EOF — so the notification a stop is built on may
    // never arrive and state() sits in Stopping with the GUI's stop button disabled. A stop must not
    // depend on being heard, so forceStop() takes charge: SIGKILL the child we own, drop it from the
    // state machine, and run ONE bounded privileged command that sweeps the payload by name and takes
    // the firewall rules with it. Whatever that command answers — or fails to answer — the run ends
    // Stopped, because a released GUI with a command to check beats a frozen one.

    // The payload process name to sweep by, recorded at launch (the strategy is not kept for a plain
    // direct run, so it cannot be recovered afterwards).
    QString payloadProcessName() const;

    // Stop listening to a command without destroying it: a destructor would kill an iptables script
    // halfway through, which is the one outcome this backend refuses to produce. The object cleans
    // itself up once the child is really gone.
    void goDeaf(QProcess *p);

    // Compose the rescue command (sweep + firewall delete when the rules are still installed) using
    // the same privilege precedence as the firewall command: $ZAPRET_GUI_FIREWALL_PROG, else /bin/sh
    // when already root, else pkexec.
    bool composeRescueCommand(QString *program, QStringList *args, QString *error) const;

    // Abandon the stalled child and launch the rescue command, watched by the stop watchdog. `reason`
    // is what the user is told about the payload that would not die.
    void forceStop(const QString &reason);

    // Release the run: watchdog off, bookkeeping clear, state Stopped (or the parked failure as Error).
    void finishForcedStop(const QString &warning = QString());

    // Deadline for whichever privileged step a stop is currently waiting on. Armed only while the run
    // is heading down, never for a plain start: a slow polkit prompt during start is normal.
    void armStopWatchdog();
    void disarmStopWatchdog();

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

    // ---- portable-mode state ----
    enum class PortablePhase
    {
        None,          // not a portable run (controller or plain direct payload)
        FirewallUp,    // the insert command is running; nfqws not started yet
        Payload,       // nfqws is the live child
        FirewallDown,  // the delete command is running; nothing else to tear down afterwards
        Rescue         // the payload would not die: sweep it by name, then remove the rules
    };

    bool m_portable = false;
    PortablePhase m_phase = PortablePhase::None;
    // True once the rules are known to be installed. Teardown is skipped when the insert command
    // itself failed, so a failed start never issues a delete for rules that do not exist.
    bool m_fwApplied = false;
    bool m_portableStopQueued = false;   // stop() pressed while FirewallUp was in flight
    // The failure to emit once an in-flight teardown command returns (see portableFail). Non-empty
    // only while the run is winding down after an error, and cleared when it is reported.
    QString m_portablePendingError;
    int m_queueNum = kPortableNfQueueNum;
    Strategy m_active;                   // the portable strategy, replayed after each short command

    // The command now running is a rescue command, not part of the ordinary phase machine: its exit
    // (or its silence) releases the GUI, and nothing else may read it as a payload or firewall result.
    bool m_forcedStop = false;
    // The payload's own binary name, recorded when it is launched — what `pkill -f` sweeps on stop.
    QString m_payloadName;
    // Whether that payload was handed to pkexec. The rescue inherits exactly this: an ordinary user's
    // pkill cannot signal a root payload, and asking polkit to authorise killing a process the user
    // already owns is a prompt nobody can explain — and one that can hang the stop it is for.
    bool m_payloadElevated = false;
    // One-shot deadline for whichever privileged step a stopping run is waiting on.
    QTimer *m_stopWatchdog = nullptr;
};


} // namespace zapret
