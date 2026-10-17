#include "LinuxBackend.h"

#include "PrivilegeHelper.h"

#include <QFileInfo>
#include <QProcess>
#include <QStringList>

namespace zapret {

namespace {
QString pkexecPath()
{
    // QProcess resolves "pkexec" via PATH at start time, but for a synchronous, clear failure we
    // stat the standard location. Debian/edora ship it here; a distro that keeps it elsewhere still
    // works because we only hard-fail when the file is absent everywhere sensible.
    return QStringLiteral("/usr/bin/pkexec");
}

// ---------------------------------------------------------------------------
// The portable-mode firewall command
// ---------------------------------------------------------------------------
//
// What nfqws needs to see traffic is: every packet we care about steered into an nfqueue. The
// upstream way to get that is the init script (which does it via $ZAPRET_BASE/common/ipt.sh) — but
// portable mode exists precisely because the init script cannot run. So the backend writes the rules
// itself, mirroring what upstream writes:
//
//   iptables  -t mangle -I OUTPUT 1 -p <tcp|udp> --dport <80|443> \
//             -m mark ! --mark <MARK>/<MARK> \
//             -j NFQUEUE --queue-num <QNUM> --queue-bypass
//   ip6tables -t mangle -I OUTPUT 1 -p <tcp|udp> --dport <80|443> \
//             -j NFQUEUE --queue-num <QNUM> --queue-bypass        (the same four rules, v6 twin)
//
// Both families share one queue because one `nfqws` serves both (upstream's _fw_nfqws_pre /
// _fw_nfqws_pre6 use the identical QNUM). The rule body is byte-for-byte identical across the two —
// only the binary differs — so the ports, the queue and the mark stay single-sourced and cannot drift.
//
// The IPv6 block is emitted behind a `[ -n "$IP6T" ]` guard: ip6tables is a genuine optional
// dependency (a host can have iptables without the v6 tool or without the v6 xtables match), and a
// missing ip6tables must skip the v6 rules rather than fail the whole insert and strand the user with
// no v4 steering either. iptables itself stays unconditional — it is the one thing portable mode
// cannot work without, so its absence must surface as a reported error.
//
// Four properties are load-bearing, and each answers a way this could hurt the user:
//
//   * --queue-bypass  (upstream uses it too, common/ipt.sh:209): if nfqws is not bound to the queue,
//     the packet continues through the stack instead of being dropped. Without it, a crashed or
//     never-started nfqws blackholes all the user's HTTP(S) traffic. With it, the worst case is
//     "no desync", which is what the user had before pressing Start.
//   * `-m mark ! --mark MARK/MARK` excludes our own re-injected packets, so nfqws does not feed
//     itself an endless loop of the same flow. The mark MUST equal the `--dpi-desync-fwmark` in argv,
//     which is why both come from kPortableNfDesyncMark. The mark match is family-agnostic (xt_mark
//     lives in the raw SKB), so the same MARK guards both the v4 and the v6 anti-loop rule.
//   * `-C` before `-I`: iptables happily accepts duplicate identical rules, and a duplicate would
//     outlive a single `-D` (which removes one match). The check makes start() idempotent across
//     crashes and repeated starts. `-D` is likewise guarded so stop() is safe to run twice.
//   * `-w5` on every call: without it iptables exits immediately when another program (firewalld,
//     a desktop network manager) holds the xtables lock, and a spurious "another app is holding the
//     lock" failure would look like a broken GUI. Bare `-w` waits FOREVER, which would hang the
//     phase; the bounded form gives the lock 5 s and then fails loudly, which the backend reports.
//
// Everything runs inside ONE `sh -c` per phase, so polkit prompts once per start/stop instead of once
// per rule. `set -e` aborts at the first rule that genuinely fails, leaving a non-zero exit code the
// backend reports; the `if` guards keep the -C probes from tripping it.
QString portableFirewallScript(int qnum, bool apply)
{
    const QString mark = QLatin1String(kPortableNfDesyncMark);

    QString s;
    s += QStringLiteral("# zapret GUI: %1 nfqueue rules for portable mode (queue %2) [IPv4 + IPv6]\n")
              .arg(apply ? QStringLiteral("insert") : QStringLiteral("remove"))
              .arg(qnum);
    s += QStringLiteral("set -e\n");
    // GUI processes started from a desktop launcher do not always carry /usr/sbin in PATH, and
    // iptables lives there on every mainstream distro. Probe the known absolute homes first.
    s += QStringLiteral("IPT=iptables\n");
    s += QStringLiteral("for p in /usr/sbin/iptables /sbin/iptables; do "
                        "if [ -x \"$p\" ]; then IPT=\"$p\"; break; fi; done\n");
    // IPv6 is best-effort: resolve ip6tables the same way, but fall back to the empty string when the
    // tool is absent so the [ -n "$IP6T" ] guard below can skip the whole v6 block without failing.
    s += QStringLiteral("IP6T=\n");
    s += QStringLiteral("for p in /usr/sbin/ip6tables /sbin/ip6tables; do "
                        "if [ -x \"$p\" ]; then IP6T=\"$p\"; break; fi; done\n");
    s += QStringLiteral("if [ -z \"$IP6T\" ] && command -v ip6tables >/dev/null 2>&1; then "
                        "IP6T=ip6tables; fi\n");
    // The firewall binary is the first argument so one `op` serves both families; the rule body that
    // follows is identical for iptables and ip6tables.
    if (apply)
    {
        s += QStringLiteral("op() { B=\"$1\"; shift; if \"$B\" -w5 -t mangle -C OUTPUT \"$@\" 2>/dev/null; "
                            "then return 0; fi; \"$B\" -w5 -t mangle -I OUTPUT 1 \"$@\"; }\n");
    }
    else
    {
        s += QStringLiteral("op() { B=\"$1\"; shift; if ! \"$B\" -w5 -t mangle -C OUTPUT \"$@\" 2>/dev/null; "
                            "then return 0; fi; \"$B\" -w5 -t mangle -D OUTPUT \"$@\"; }\n");
    }
    s += QStringLiteral("M=%1\nQ=%2\n").arg(mark).arg(qnum);

    // One line per (proto, port) pair from the single source of truth the parser also uses to build
    // --filter-tcp/--filter-udp, so the queue can never be fed traffic nfqws was told to ignore. The
    // rule body is captured once and applied to iptables unconditionally and to ip6tables when present.
    QStringList bodies;
    for (const auto &pp : portablePorts())
    {
        bodies << QStringLiteral("-p %1 --dport %2 -m mark ! --mark $M/$M "
                                 "-j NFQUEUE --queue-num $Q --queue-bypass")
                      .arg(pp.first, pp.second);
    }

    for (const QString &body : bodies)
        s += QStringLiteral("op \"$IPT\" %1\n").arg(body);

    // Mirror the exact same rule set into ip6tables, but only when the tool exists: a host without
    // ip6tables loses v6 steering, not v4, and the run still succeeds.
    s += QStringLiteral("if [ -n \"$IP6T\" ]; then\n");
    for (const QString &body : bodies)
        s += QStringLiteral("  op \"$IP6T\" %1\n").arg(body);
    s += QStringLiteral("fi\n");
    return s;
}

// The one-line description of what the firewall command is about to do, so the user is never
// surprised by a password dialog they cannot explain. pkexec takes its prompt text from polkit's
// action annotation, which only root can edit, so the GUI's own log is where the user learns what
// they are authorising.
QString portableFirewallBlurb(int qnum, bool apply)
{
    QStringList protos;
    for (const auto &pp : portablePorts())
        protos << QStringLiteral("%1/%2").arg(pp.second, pp.first);

    return QStringLiteral("portable mode: %1 iptables rules in mangle OUTPUT steering %2 into "
                          "NFQUEUE %3, plus the same ip6tables rules when it is available "
                          "(anti-loop mark %4, --queue-bypass so traffic is never black-holed if "
                          "nfqws dies)")
        .arg(apply ? QStringLiteral("insert") : QStringLiteral("remove"))
        .arg(protos.join(QStringLiteral(", ")))
        .arg(qnum)
        .arg(QLatin1String(kPortableNfDesyncMark));
}

} // namespace

LinuxBackend::LinuxBackend(QObject *parent)
    : IZapretBackend(parent)
{
    m_process = new QProcess(this);
    connect(m_process, &QProcess::started, this, &LinuxBackend::onProcessStarted);
    connect(m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                onProcessFinished(code, status == QProcess::CrashExit);
            });
    connect(m_process, &QProcess::errorOccurred, this, &LinuxBackend::onProcessError);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &LinuxBackend::onReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &LinuxBackend::onReadyReadStderr);
}

LinuxBackend::~LinuxBackend() = default;

void LinuxBackend::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(s);
}

void LinuxBackend::resetProcess()
{
    if (m_process)
    {
        m_process->disconnect(this);
        m_process->deleteLater();
    }
    m_process = new QProcess(this);
    connect(m_process, &QProcess::started, this, &LinuxBackend::onProcessStarted);
    connect(m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                onProcessFinished(code, status == QProcess::CrashExit);
            });
    connect(m_process, &QProcess::errorOccurred, this, &LinuxBackend::onProcessError);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &LinuxBackend::onReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &LinuxBackend::onReadyReadStderr);
}

bool LinuxBackend::isRunning() const
{
    if (m_portable)
        // The firewall commands are bookkeeping, not the service: portable mode is only "running"
        // while nfqws itself is the live child. A caller that needs to know whether the queue exists
        // can read state() instead, which distinguishes Starting/Stopping from Running.
        return m_phase == PortablePhase::Payload && m_process &&
               m_process->state() != QProcess::NotRunning;
    if (m_controller)
        // For a controller we are "running" when the daemon is up, signalled by a prior successful
        // start; the short pkexec command itself is not running most of the time.
        return m_state == State::Running;
    return m_process && m_process->state() != QProcess::NotRunning;
}

bool LinuxBackend::composeCommand(const Strategy &s, const QString &action, QString *program,
                                  QStringList *args, QString *error)
{
    const bool controller = (s.kind == Strategy::Kind::ShellScript);

    if (controller)
    {
        // The init script path: primaryArg holds it after applyDefaults; filePath is the fallback.
        const QString script = s.primaryArg.isEmpty() ? s.filePath : s.primaryArg;
        if (script.isEmpty() || !QFileInfo::exists(script))
        {
            if (error)
                *error = QStringLiteral("LinuxBackend: init script not found: %1").arg(script);
            return false;
        }
        // Drive through pkexec unless we are already root.
        QString prog = s.executablePath.isEmpty() ? QStringLiteral("pkexec") : s.executablePath;
        if (prog == QLatin1String("pkexec") && PrivilegeHelper::isElevated())
            prog = script;   // already root: run the script directly, no pkexec round-trip

        if (prog == QLatin1String("pkexec") && !QFileInfo::exists(pkexecPath()))
        {
            if (error)
                *error = QStringLiteral("LinuxBackend: pkexec is not installed (%1)").arg(pkexecPath());
            return false;
        }

        if (prog == QLatin1String("pkexec"))
            *args = {script, action};       // pkexec <script> start|stop
        else
            *args = {action};               // <script> start|stop  (we are root)
        *program = prog;
        return true;
    }

    // Direct payload (nfqws / winws-equivalent).
    const QString binary = launchProgram(s);
    if (binary.isEmpty())
    {
        if (error)
            *error = QStringLiteral("LinuxBackend: strategy has no executable");
        return false;
    }
    const QFileInfo fi(binary);
    const bool explicitPath =
        fi.isAbsolute() || binary.contains(QLatin1Char('/')) || binary.contains(QLatin1Char('\\'));
    if (explicitPath && !fi.exists())
    {
        if (error)
            *error = QStringLiteral("LinuxBackend: executable not found: %1").arg(binary);
        return false;
    }

    QStringList a = baseArgs(s);
    if (s.requiresRoot && !PrivilegeHelper::isElevated())
    {
        if (!QFileInfo::exists(pkexecPath()))
        {
            if (error)
                *error = QStringLiteral("LinuxBackend: pkexec is not installed and root is required");
            return false;
        }
        a.prepend(binary);                    // pkexec <binary> <args...>
        *program = QStringLiteral("pkexec");
    }
    else
    {
        *program = binary;
    }
    *args = a;
    return true;
}

bool LinuxBackend::start(const Strategy &s, QString *error)
{
    auto fail = [&](const QString &msg) -> bool {
        m_lastError = msg;
        emit logMessage(msg, 2);
        setState(State::Error);
        emit errorOccurred(msg);
        if (error)
            *error = msg;
        return false;
    };

    if (m_state == State::Starting || m_state == State::Running ||
        (m_process && m_process->state() != QProcess::NotRunning))
        return fail(QStringLiteral("LinuxBackend: an operation is already in progress (stop first)"));

    if (!s.isValid())
        return fail(QStringLiteral("LinuxBackend: strategy is not valid"));

    // Elevation gate. Not root + no reachable polkit agent => pkexec cannot prompt and would hang or
    // fail silently, so surface a precise message instead of attempting it.
    if (s.requiresRoot && !PrivilegeHelper::isElevated() && !PrivilegeHelper::hasPolkitAgent())
        return fail(QStringLiteral("LinuxBackend: root is required but no polkit agent is available "
                                   "(run the application as root, or launch it from a desktop session)"));

    // Run mode, not kind, decides who owns the firewall. A portable entry is Kind::Binary like any
    // direct payload, but the backend has to wrap its lifetime in nfqueue rules; a controller is a
    // script that does all of that itself. The two flags are exclusive by construction.
    m_portable = (s.runMode == Strategy::RunMode::PortableNfqws);
    m_controller = !m_portable && (s.kind == Strategy::Kind::ShellScript);

    if (m_portable)
    {
        startPortable(s);
        // startPortable is asynchronous: true means "the firewall command is out there", and the
        // payload follows when it succeeds. Only a synchronous rejection reports false.
        if (m_state == State::Error)
        {
            if (error)
                *error = m_lastError;
            return false;
        }
        return true;
    }

    // Not portable: drop the portable bookkeeping so a stale phase can never misroute a later
    // notification. Rules a previous portable run failed to remove are reported, not silently kept:
    // every such rule carries --queue-bypass, so the worst case is traffic that bypasses the queue.
    m_phase = PortablePhase::None;
    m_portableStopQueued = false;
    m_portablePendingError.clear();
    m_active = Strategy();
    if (m_fwApplied)
        emit logMessage(QStringLiteral("LinuxBackend: a previous portable run could not remove its "
                                       "rules from NFQUEUE %1; clean them up with 'iptables -t "
                                       "mangle -L OUTPUT'")
                            .arg(m_queueNum),
                        1);
    m_fwApplied = false;

    QString program;
    QStringList args;
    if (!composeCommand(s, QStringLiteral("start"), &program, &args, error))
    {
        const QString msg = error && !error->isEmpty()
                                ? *error
                                : QStringLiteral("LinuxBackend: could not compose command");
        m_lastError = msg;
        emit logMessage(msg, 2);
        setState(State::Error);
        emit errorOccurred(msg);
        return false;
    }

    // Remember the controller specifics so stop() can issue the matching "stop" later.
    m_scriptPath = s.primaryArg.isEmpty() ? s.filePath : s.primaryArg;
    m_controllerProg = program;
    m_stopRequested = false;
    m_cmdIsStop = false;
    m_directStop = false;   // a new run starts clean: its exits are never "on request"

    resetProcess();
    m_process->setProgram(program);
    m_process->setArguments(args);
    if (!s.workingDirectory.isEmpty())
        m_process->setWorkingDirectory(s.workingDirectory);

    m_lastError.clear();
    setState(State::Starting);
    emit logMessage(QStringLiteral("LinuxBackend: launching %1 %2")
                        .arg(program, args.join(QLatin1Char(' '))), 0);

    m_process->start(program, args);
    if (m_state == State::Error)
    {
        if (error)
            *error = m_lastError;
        return false;
    }
    return true;
}

void LinuxBackend::launchControllerStop()
{
    Strategy tmp;
    tmp.kind = Strategy::Kind::ShellScript;
    tmp.primaryArg = m_scriptPath;
    tmp.filePath = m_scriptPath;
    tmp.executablePath = m_controllerProg;   // reuse the same pkexec (or direct script) path

    QString program;
    QStringList args;
    QString err;
    if (!composeCommand(tmp, QStringLiteral("stop"), &program, &args, &err))
    {
        m_lastError = err;
        emit logMessage(err, 2);
        setState(State::Error);
        emit errorOccurred(err);
        return;
    }

    m_cmdIsStop = true;
    resetProcess();
    m_process->setProgram(program);
    m_process->setArguments(args);
    setState(State::Stopping);
    emit logMessage(QStringLiteral("LinuxBackend: stopping via %1 %2").arg(program, args.join(QLatin1Char(' '))), 0);
    m_process->start(program, args);
}

// ---------------------------------------------------------------------------
// RunMode::PortableNfqws
// ---------------------------------------------------------------------------
//
// The whole of portable mode is a three-step sequence driven by m_phase, one QProcess at a time:
//
//     FirewallUp  ->  Payload  ->  (stop / payload exit)  ->  FirewallDown
//
// Each step is a plain child process, so nothing here needs a timer or a thread: every transition
// happens in onPortableFinished(). The invariant that matters is that nfqws only ever runs with its
// queue installed, and the queue never outlives a run the GUI knows about.

bool LinuxBackend::composeFirewallCommand(bool apply, QString *program, QStringList *args,
                                          QString *error) const
{
    if (!program || !args)
    {
        if (error)
            *error = QStringLiteral("LinuxBackend: internal error: nowhere to put the firewall command");
        return false;
    }

    const QString script = portableFirewallScript(m_queueNum, apply);
    const QString sh = QStringLiteral("/bin/sh");

    // An explicit wrapper is handed the full command line, exactly as pkexec would be, so `sudo`,
    // `doas`, a site wrapper that already owns the polkit rule and a test double all behave the same.
    const QString wrapper = qEnvironmentVariable("ZAPRET_GUI_FIREWALL_PROG").trimmed();
    if (!wrapper.isEmpty())
    {
        *program = wrapper;
        *args = {sh, QStringLiteral("-c"), script};
        return true;
    }

    if (PrivilegeHelper::isElevated())
    {
        *program = sh;
        *args = {QStringLiteral("-c"), script};
        return true;
    }

    if (!QFileInfo::exists(pkexecPath()))
    {
        if (error)
            *error = QStringLiteral("LinuxBackend: pkexec is not installed (%1) and root is needed to "
                                    "edit the firewall; run the GUI as root or set "
                                    "ZAPRET_GUI_FIREWALL_PROG to a wrapper such as sudo")
                         .arg(pkexecPath());
        return false;
    }

    *program = QStringLiteral("pkexec");
    *args = {sh, QStringLiteral("-c"), script};
    return true;
}

void LinuxBackend::launchPortableFirewall(bool apply)
{
    QString program;
    QStringList args;
    QString err;
    if (!composeFirewallCommand(apply, &program, &args, &err))
    {
        // Nothing was spawned, so there is nothing to unwind and no second attempt worth making.
        m_lastError = err.isEmpty()
                          ? QStringLiteral("LinuxBackend: could not compose the firewall command")
                          : err;
        m_phase = PortablePhase::None;
        m_portableStopQueued = false;
        if (apply)
            m_fwApplied = false;   // an insert that never ran installed nothing
        emit logMessage(m_lastError, 2);
        setState(State::Error);
        emit errorOccurred(m_lastError);
        return;
    }

    m_phase = apply ? PortablePhase::FirewallUp : PortablePhase::FirewallDown;
    setState(apply ? State::Starting : State::Stopping);

    resetProcess();
    m_process->setProgram(program);
    m_process->setArguments(args);
    if (!m_active.workingDirectory.isEmpty())
        m_process->setWorkingDirectory(m_active.workingDirectory);

    // Tell the user what they are about to authorise BEFORE the prompt appears, not after: pkexec's
    // own description comes from a polkit action annotation only root can edit, so the log is the
    // only place this can be said at all. The script itself is not dumped — it is long, and the blurb
    // already names every number in it.
    emit logMessage(QStringLiteral("LinuxBackend: %1").arg(portableFirewallBlurb(m_queueNum, apply)), 0);
    m_process->start(program, args);
}

void LinuxBackend::launchPortablePayload()
{
    QString program;
    QStringList args;
    QString err;
    if (!composeCommand(m_active, QStringLiteral("start"), &program, &args, &err))
    {
        // The queue exists and nfqws does not: portableFail() removes the rules first, then reports.
        portableFail(err.isEmpty() ? QStringLiteral("LinuxBackend: could not compose the nfqws command")
                                   : err);
        return;
    }

    m_phase = PortablePhase::Payload;
    resetProcess();
    m_process->setProgram(program);
    m_process->setArguments(args);
    // Relative paths a user adds to argv (list files, ipset helpers, nfqws' own support files) are
    // tree-rooted, exactly as the init script runs them from $ZAPRET_BASE.
    if (!m_active.workingDirectory.isEmpty())
        m_process->setWorkingDirectory(m_active.workingDirectory);

    emit logMessage(QStringLiteral("LinuxBackend: portable mode: starting %1 %2")
                        .arg(program, args.join(QLatin1Char(' '))), 0);
    // State reaches Running only through onProcessStarted(), which fires for this phase alone: the
    // firewall commands above must never be mistaken for the service being up.
    m_process->start(program, args);
}

void LinuxBackend::startPortable(const Strategy &s)
{
    m_active = s;
    // One read of argv decides the number both halves use: the rules enqueue into it and nfqws binds
    // it, because portableQueueNum() recovers exactly what the parser wrote into --qnum=.
    m_queueNum = portableQueueNum(s.argv);
    m_fwApplied = false;
    m_portableStopQueued = false;
    m_portablePendingError.clear();
    m_directStop = false;
    m_cmdIsStop = false;
    m_stopRequested = false;
    m_phase = PortablePhase::FirewallUp;
    setState(State::Starting);

    emit logMessage(QStringLiteral("LinuxBackend: portable mode: payload %1, NFQUEUE %2")
                        .arg(QFileInfo(launchProgram(s)).fileName())
                        .arg(m_queueNum),
                    0);
    launchPortableFirewall(true);
}

void LinuxBackend::portableFail(const QString &msg)
{
    m_lastError = msg;
    emit logMessage(msg, 2);

    // Tear the queue down first whenever the rules are installed — unless we are already doing it,
    // because a failing teardown that re-arms itself would loop. The message is parked and emitted as
    // an error by the FirewallDown branch of onPortableFinished() once the machine is clean.
    if (m_fwApplied && m_phase != PortablePhase::FirewallDown)
    {
        m_portablePendingError = msg;
        launchPortableFirewall(false);
        return;
    }

    m_phase = PortablePhase::None;
    m_portableStopQueued = false;
    setState(State::Error);
    emit errorOccurred(msg);
}

void LinuxBackend::stopPortable()
{
    if (m_phase == PortablePhase::None && !m_fwApplied &&
        (!m_process || m_process->state() == QProcess::NotRunning))
    {
        // Already entirely down: don't flap Stopping->Stopped for the sake of a duplicate click.
        setState(State::Stopped);
        return;
    }

    if (m_phase == PortablePhase::FirewallUp)
    {
        // The insert command is short-lived, and killing a shell mid-script is the one way to leave
        // the rule set in a state the GUI cannot describe. Let it finish: the finished handler sees
        // this flag and tears the rules down instead of starting nfqws.
        setState(State::Stopping);
        m_portableStopQueued = true;
        emit logMessage(QStringLiteral("LinuxBackend: stop queued until the firewall command returns"), 1);
        return;
    }

    if (m_phase == PortablePhase::FirewallDown)
    {
        setState(State::Stopping);
        emit logMessage(QStringLiteral("LinuxBackend: the firewall teardown is already running"), 1);
        return;
    }

    setState(State::Stopping);

    if (m_phase == PortablePhase::Payload && m_process &&
        m_process->state() != QProcess::NotRunning)
    {
        // The rules stay installed until nfqws is really gone: --queue-bypass makes them inert in the
        // meantime, and deleting the queue first would only widen the window in which nothing steers
        // traffic at all. Marking the kill as requested is what keeps a routine SIGTERM — which
        // arrives as CrashExit on Unix — from being reported as a crash.
        //
        // The child is captured first: waitForFinished() delivers finished() from inside this call,
        // and that handler replaces m_process with the teardown command. Touching the member
        // afterwards would SIGKILL the teardown.
        QProcess *child = m_process;
        m_directStop = true;
        emit logMessage(QStringLiteral("LinuxBackend: terminating nfqws (portable mode)"), 0);
        child->terminate();
        if (child->waitForFinished(3000))
            return;      // finished() already ran, and the teardown with it
        child->kill();   // its finished() lands in the event loop and tears down from there
        return;
    }

    // Nothing is live but the rules are (a payload that exited while the backend was busy): remove
    // them so the run leaves no trace behind.
    if (m_fwApplied)
        launchPortableFirewall(false);
    else
        setState(State::Stopped);
}

void LinuxBackend::onPortableFinished(int exitCode, bool crashed)
{
    const int code = crashed ? -1 : exitCode;

    switch (m_phase)
    {
    case PortablePhase::FirewallUp:
        if (crashed || code != 0)
        {
            // The insert command itself failed. A partially applied rule set is possible but not
            // dangerous: every rule carries --queue-bypass, so at worst traffic is steered into an
            // unbound queue and passed straight through. No teardown is attempted — whatever made the
            // insert fail (no root, no iptables, no NFQUEUE target) makes the delete fail too, and a
            // second authorisation prompt directly after a failure is noise the user cannot act on.
            const QString msg = QStringLiteral(
                "LinuxBackend: firewall setup failed (exit %1); nfqws was not started. Any rule that "
                "did get applied carries --queue-bypass and cannot block traffic; check 'iptables -t "
                "mangle -L OUTPUT' and 'ip6tables -t mangle -L OUTPUT' for leftovers on queue %2")
                .arg(code)
                .arg(m_queueNum);
            m_fwApplied = false;
            m_phase = PortablePhase::None;
            m_portableStopQueued = false;
            m_lastError = msg;
            emit logMessage(msg, 2);
            setState(State::Error);
            emit errorOccurred(msg);
            return;
        }
        m_fwApplied = true;
        emit logMessage(QStringLiteral("LinuxBackend: nfqueue rules installed (queue %1)")
                            .arg(m_queueNum), 0);
        if (m_portableStopQueued)
        {
            m_portableStopQueued = false;
            emit logMessage(QStringLiteral("LinuxBackend: stop was requested — removing the rules "
                                           "without starting nfqws"), 0);
            launchPortableFirewall(false);
            return;
        }
        launchPortablePayload();
        return;

    case PortablePhase::Payload:
    {
        // The live child is gone. Whether it exited on its own, crashed or was asked to, the queue
        // must not outlive it, so any failure text is parked and the teardown runs first.
        const bool intentional = m_directStop;
        m_directStop = false;
        m_phase = PortablePhase::None;

        QString failure;
        if (intentional)
            emit logMessage(QStringLiteral("LinuxBackend: nfqws stopped on request"), 0);
        else if (crashed)
            failure = QStringLiteral("LinuxBackend: nfqws process crashed");
        else if (code != 0)
            failure = QStringLiteral("LinuxBackend: nfqws exited with code %1").arg(code);
        else
            emit logMessage(QStringLiteral("LinuxBackend: nfqws exited normally"), 0);

        if (!failure.isEmpty())
        {
            m_lastError = failure;
            emit logMessage(failure, 2);   // logged here; errorOccurred follows the teardown
        }

        if (m_fwApplied)
        {
            m_portablePendingError = failure;
            launchPortableFirewall(false);
            return;
        }
        if (!failure.isEmpty())
        {
            setState(State::Error);
            emit errorOccurred(failure);
            return;
        }
        setState(State::Stopped);
        return;
    }

    case PortablePhase::FirewallDown:
    {
        m_fwApplied = false;
        m_phase = PortablePhase::None;
        m_portableStopQueued = false;
        if (crashed || code != 0)
        {
            // A leftover --queue-bypass rule cannot black-hole traffic, so this is a warning naming
            // the exact place to look rather than an error that would bury the real failure under it.
            emit logMessage(QStringLiteral("LinuxBackend: firewall teardown failed (exit %1); check "
                                           "'iptables -t mangle -L OUTPUT' for leftover NFQUEUE rules "
                                           "on queue %2")
                                .arg(code)
                                .arg(m_queueNum),
                            1);
        }
        else
        {
            emit logMessage(QStringLiteral("LinuxBackend: nfqueue rules removed"), 0);
        }

        if (!m_portablePendingError.isEmpty())
        {
            // The run already failed for a worse reason; now that the machine is clean, say that. The
            // line was logged where it originated, so only the notification is emitted here.
            const QString msg = m_portablePendingError;
            m_portablePendingError.clear();
            m_lastError = msg;
            setState(State::Error);
            emit errorOccurred(msg);
            return;
        }
        setState(State::Stopped);
        return;
    }

    case PortablePhase::None:
        // A notification no phase owns — a late exit from a child the teardown already disconnected.
        // Settle quietly instead of inventing a state the GUI would show as a phantom failure.
        m_directStop = false;
        m_portablePendingError.clear();
        setState(State::Stopped);
        return;
    }
}

void LinuxBackend::stop()
{
    // Portable teardown is its own phase machine: the payload kill must be followed by the rule
    // removal, which no other shape of run has to do.
    if (m_portable)
    {
        stopPortable();
        return;
    }

    if (!m_controller)
    {
        // Direct payload: terminate the live process exactly like the Windows backend.
        if (!m_process || m_process->state() == QProcess::NotRunning)
        {
            m_directStop = false;
            setState(State::Stopped);
            return;
        }
        // Set before terminate(): waitForFinished() delivers finished() from inside this call, and the
        // handler must already know the death was requested.
        m_directStop = true;
        setState(State::Stopping);
        emit logMessage(QStringLiteral("LinuxBackend: terminating managed process"), 0);
        m_process->terminate();
        if (!m_process->waitForFinished(3000))
            m_process->kill();
        return;
    }

    // Controller: a start command may still be in flight.
    if (m_process && m_process->state() != QProcess::NotRunning && !m_cmdIsStop)
    {
        m_stopRequested = true;
        setState(State::Stopping);
        emit logMessage(QStringLiteral("LinuxBackend: stop queued until the start command returns"), 1);
        return;
    }
    setState(State::Stopping);
    launchControllerStop();
}


void LinuxBackend::onProcessStarted()
{
    if (m_portable)
    {
        // Only nfqws itself means the service is up. The firewall commands are bookkeeping that
        // happens to be a process too, and reporting Running while they execute would badge a card
        // for a daemon that does not exist yet.
        if (m_phase == PortablePhase::Payload)
            setState(State::Running);
        return;
    }

    // Only the direct-payload mode has a long-lived child; the controller command merely began to
    // run. For controller we set Running in the finished handler based on its exit code.
    if (!m_controller)
        setState(State::Running);
}

void LinuxBackend::onProcessFinished(int exitCode, bool crashed)
{
    const int code = crashed ? -1 : exitCode;

    if (m_portable)
    {
        onPortableFinished(code, crashed);
        return;
    }

    if (!m_controller)
    {
        if (m_directStop)
        {
            // stop() killed it. A signalled exit arrives here as CrashExit on Unix, which is exactly the
            // false "crash" an ordinary Остановить click must not turn into.
            m_directStop = false;
            emit logMessage(QStringLiteral("LinuxBackend: payload stopped on request"), 0);
            setState(State::Stopped);
            return;
        }
        // Direct payload terminated.
        if (crashed)
        {
            m_lastError = QStringLiteral("LinuxBackend: payload process crashed");
            emit logMessage(m_lastError, 2);
            setState(State::Error);
            emit errorOccurred(m_lastError);
        }
        else
        {
            emit logMessage(QStringLiteral("LinuxBackend: payload exited with code %1").arg(code),
                            code == 0 ? 0 : 1);
            setState(code == 0 ? State::Stopped : State::Error);
            if (code != 0)
            {
                m_lastError = QStringLiteral("LinuxBackend: payload exited with code %1").arg(code);
                emit errorOccurred(m_lastError);
            }
        }
        return;
    }

    // Controller: the short pkexec/script invocation just completed.
    if (m_cmdIsStop)
    {
        m_cmdIsStop = false;
        if (!crashed && code == 0)
        {
            emit logMessage(QStringLiteral("LinuxBackend: stop command succeeded"), 0);
            setState(State::Stopped);
        }
        else
        {
            m_lastError = QStringLiteral("LinuxBackend: stop command failed (exit %1)").arg(code);
            emit logMessage(m_lastError, crashed ? 2 : 1);
            setState(State::Error);
            emit errorOccurred(m_lastError);
        }
        return;
    }

    // A "start" controller command completed.
    if (m_stopRequested)
    {
        // User pressed stop while start was still running: run the matching stop now.
        m_stopRequested = false;
        if (crashed || code != 0)
        {
            // The start itself failed; nothing to stop, report the start failure.
            m_lastError = QStringLiteral("LinuxBackend: start command failed (exit %1)").arg(code);
            emit logMessage(m_lastError, 2);
            setState(State::Error);
            emit errorOccurred(m_lastError);
        }
        else
        {
            launchControllerStop();
        }
        return;
    }

    if (!crashed && code == 0)
    {
        emit logMessage(QStringLiteral("LinuxBackend: start command succeeded — daemon is up"), 0);
        setState(State::Running);
    }
    else
    {
        m_lastError = QStringLiteral("LinuxBackend: start command failed (exit %1)").arg(code);
        emit logMessage(m_lastError, 2);
        setState(State::Error);
        emit errorOccurred(m_lastError);
    }
}

void LinuxBackend::onProcessError()
{
    const QProcess::ProcessError e = m_process ? m_process->error() : QProcess::UnknownError;
    if (e == QProcess::FailedToStart)
    {
        const QString msg = QStringLiteral("LinuxBackend: failed to start '%1': %2")
                                .arg(m_process ? m_process->program() : QString(),
                                     m_process ? m_process->errorString() : QString());
        if (m_portable)
        {
            // A firewall command that never ran leaves nothing behind. A payload that never exec'd
            // leaves the queue rules behind, so portableFail() removes them before surfacing this.
            portableFail(msg);
            return;
        }
        m_lastError = msg;
        emit logMessage(msg, 2);
        setState(State::Error);
        emit errorOccurred(msg);
    }
}

void LinuxBackend::onReadyReadStdout()
{
    if (!m_process)
        return;
    const QByteArray data = m_process->readAllStandardOutput();
    if (!data.isEmpty())
        emit logMessage(QString::fromLocal8Bit(data).trimmed(), 0);
}

void LinuxBackend::onReadyReadStderr()
{
    if (!m_process)
        return;
    const QByteArray data = m_process->readAllStandardError();
    if (!data.isEmpty())
        emit logMessage(QString::fromLocal8Bit(data).trimmed(), 1);
}

} // namespace zapret

