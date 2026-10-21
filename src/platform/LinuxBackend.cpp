#include "LinuxBackend.h"

#include "PrivilegeHelper.h"

#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QTimer>

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
//   iptables  -t mangle -I OUTPUT 1 -p tcp --dport <80|443> \
//             -m mark ! --mark <MARK>/<MARK> \
//             -j NFQUEUE --queue-num <QNUM> --queue-bypass
//   iptables  -t mangle -I OUTPUT 1 -p udp --dport 443 \
//             -m mark ! --mark <MARK>/<MARK> \
//             -j NFQUEUE --queue-num <QNUM> --queue-bypass
//   ip6tables -t mangle -I OUTPUT 1 -p <tcp|udp> --dport <...> \
//             -j NFQUEUE --queue-num <QNUM> --queue-bypass        (the same three rules, v6 twin)
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

// ---------------------------------------------------------------------------
// How long each half of a stop is allowed to take
// ---------------------------------------------------------------------------
//
// Both numbers exist because one thing is true of every launch shape in this backend: the process
// QProcess controls is often pkexec, and pkexec does NOT forward SIGTERM to the nfqws it spawns. So
// terminate() is a request the payload never hears; kill() removes only the wrapper and leaves the
// payload running as an orphan that still holds the write end of the stdout/stderr pipes QProcess
// reads. Qt deliberately withholds finished() until those pipes reach EOF (otherwise it could not
// promise the child's output had all been delivered), so the exit notification a stop is sequenced on
// may never arrive at all — and state() stays Stopping while MainWindow greys the stop button out,
// leaving the user with no way out. Hence: no wait without a deadline.
//
//   kTerminateGraceMs        — how long the exit that a SIGTERM may produce is waited for before the
//                             run is taken down by force. It is NOT a courtesy period in which the
//                             payload is invited to shut itself down: on the elevated path the payload
//                             never hears the signal at all, so every millisecond of this wait is
//                             dead time spent sitting in the GUI thread with the Stop button pressed
//                             and nothing on screen moving. Three seconds of it bought nothing but a
//                             slow button. What the deadline is for is the opposite shape — a run this
//                             backend genuinely owns (an already-root GUI, or a plain direct payload),
//                             where SIGTERM lands and the exit arrives in milliseconds; waitForFinished
//                             returns the moment it does, so the polite path costs what it costs and
//                             is never padded. The number only bounds the case where no exit is coming.
//   kPrivilegedStepTimeoutMs — how long a privileged cleanup command may take before the GUI is
//                             released anyway. It is a deadline for WAITING, not for the command: an
//                             in-flight iptables script is left running (see goDeaf) rather than
//                             aborted halfway, because a half-run rule set is the one state the GUI
//                             genuinely cannot describe.
constexpr int kTerminateGraceMs = 500;
constexpr int kPrivilegedStepTimeoutMs = 5000;

// A payload name reduced to characters that are safe to interpolate into a privileged shell script.
//
// The name comes from a path on disk the user chose, and it ends up inside `sh -c`. Everything outside
// a plain file name is discarded, and an empty result falls back to the payload portable mode names
// itself: the same rule that keeps --qnum= provably numeric (see portableQueueNum).
QString portableProcessName(const QString &raw)
{
    QString clean;
    for (const QChar c : raw)
    {
        if (c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('_')
            || c == QLatin1Char('-') || c == QLatin1Char('+'))
            clean.append(c);
    }
    if (clean.isEmpty())
        clean = QStringLiteral("nfqws");
    return clean;
}

// The `pkill -f` pattern for a payload name: the first character in brackets.
//
// The bracket is the whole trick. The pattern still matches the payload's own argv, but it can no
// longer match the command line that CARRIES it — `pkexec /bin/sh -c pkill -9 -f '[n]fqws'` contains
// no literal "nfqws" — so the sweep cannot kill the very shell that is running it, and cannot match
// itself either.
QString portableProcessPattern(const QString &name)
{
    return QStringLiteral("[%1]%2").arg(name.left(1), name.mid(1));
}

// The whole force-stop command: sweep the payload by name, then take its nfqueue rules back out.
//
// Two properties decide the layout.
//
//   * The payload dies FIRST and unconditionally, because nothing else here can reach it.
//     terminate()/kill() only ever acts on the process QProcess owns, which is the pkexec wrapper;
//     the payload is that wrapper's child, orphaned the moment the wrapper dies. `-9` because a
//     process stuck in an nfqueue read loop is not going to honour a polite request. Best-effort by
//     design: nothing matched is the NORMAL case (the payload did exit, the pipes were just slow),
//     and a host without pkill is reported rather than guessed at.
//   * The rule cleanup runs in a SUBSHELL whose exit code is collected rather than inherited.
//     portableFirewallScript() opens with `set -e` and can legitimately fail (the rule is already
//     gone, xtables locked past -w5, no root); that must not abort the sweep that already happened —
//     and it must not be papered over with a blanket `exit 0` either. So the status this command ends
//     with means exactly one thing: "were the rules removable". Whether the payload died is said in
//     the script's own output, which the GUI logs.
QString forceStopScript(const QString &rawName, bool removeRules, int qnum)
{
    const QString name = portableProcessName(rawName);
    const QString pat = portableProcessPattern(name);

    QString s;
    s += QStringLiteral("# zapret GUI: force-stop the payload '%1' by name\n").arg(name);
    s += QStringLiteral("if command -v pkill >/dev/null 2>&1; then\n");
    s += QStringLiteral("  if pkill -9 -f '%1'; then echo \"LinuxBackend: killed '%2'\"\n"
                        "  else echo \"LinuxBackend: no surviving '%2' process found\"; fi\n")
               .arg(pat, name);
    s += QStringLiteral("else\n");
    s += QStringLiteral("  echo \"LinuxBackend: pkill is unavailable on this host; '%1' may still be "
                        "running\"\n").arg(name);
    s += QStringLiteral("fi\n");

    s += QStringLiteral("FW_RC=0\n");
    if (removeRules)
    {
        s += QStringLiteral("# Then the NFQUEUE rules, in a subshell: their failure cannot undo the "
                            "sweep above.\n");
        s += QStringLiteral("(\n");
        s += portableFirewallScript(qnum, false);
        s += QStringLiteral("\n) || FW_RC=$?\n");
    }
    s += QStringLiteral("if [ \"$FW_RC\" -ne 0 ]; then echo \"LinuxBackend: the NFQUEUE rules may "
                        "still be installed (cleanup exit $FW_RC); check 'iptables -t mangle "
                        "-L OUTPUT' and 'ip6tables -t mangle -L OUTPUT'\"; fi\n");
    s += QStringLiteral("exit $FW_RC\n");
    return s;
}

// Wrap a shell script in whatever this host needs to run it as root.
//
// Precedence, shared by the firewall command and the rescue command so the two can never disagree
// about how to get root — a stop that suddenly needs a different helper than the start did is a stop
// that fails in front of the user:
//
//   $ZAPRET_GUI_FIREWALL_PROG  — an explicit wrapper handed the full command ("/bin/sh" "-c"
//          <script>): `sudo`, `doas`, a site wrapper that already holds the polkit rule, or a test
//          double. Nothing is spawned as root unless this or pkexec says to, and the variable is the
//          only way to change that without editing the tree.
//   already root               — /bin/sh directly: no helper needed.
//   otherwise                  — pkexec /bin/sh -c <script>, one prompt for the whole script.
bool wrapPrivilegedScript(const QString &script, bool needsRoot, QString *program, QStringList *args,
                          QString *error)
{
    const QString sh = QStringLiteral("/bin/sh");

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

    // Nothing here needs root: a plain user command then, because raising an authorisation prompt to
    // signal a process the user already owns is a prompt nobody can explain — and one that can hang
    // the very stop it is being asked to perform.
    if (!needsRoot)
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

    // The deadline that makes a stop unconditional. Single-shot and parented: it is armed by whichever
    // privileged cleanup step is in flight and disarmed by its completion, so at most one stop step is
    // ever being waited on — exactly the invariant the rest of this class is built on.
    m_stopWatchdog = new QTimer(this);
    m_stopWatchdog->setSingleShot(true);
    m_stopWatchdog->setInterval(kPrivilegedStepTimeoutMs);
    connect(m_stopWatchdog, &QTimer::timeout, this, &LinuxBackend::onStopWatchdogFired);
}

LinuxBackend::~LinuxBackend()
{
    // Nothing here waits, but an in-flight privileged cleanup command is not ours to interrupt:
    // ~QProcess kills a running child, and killing iptables halfway through a rule delete leaves
    // precisely the undescribable rule set this class goes out of its way to avoid. Hand any command
    // that is doing cleanup its own lifetime and let it finish unobserved.
    disarmStopWatchdog();
    if (m_process && m_process->state() != QProcess::NotRunning &&
        (m_forcedStop || m_cmdIsStop || m_phase == PortablePhase::FirewallDown ||
         m_phase == PortablePhase::Rescue))
        goDeaf(m_process);
}

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
        // Recorded for the rescue path: anything reached through pkexec runs as root, and a sweep
        // issued by this unprivileged GUI could not signal it.
        m_payloadElevated = (prog == QLatin1String("pkexec"));
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

    // A payload that starts as root must not be allowed to drop it. nfqws does so on its own
    // initiative — main() sees CAP_SETUID, sets uid/gid to 0x7FFFFFFF and drops — and by the time it
    // reaches nfq_create_queue() it no longer holds CAP_NET_ADMIN, so the queue bind fails with
    // "Operation not permitted" after the firewall rules are already in. `--user=root` is the fix, and
    // kPortableNfUser in PortableMode.h records both why it works and why it is only ever correct here:
    // this is the one place that knows the child WILL be root (already elevated, or handed to pkexec
    // just below), and passing --user=root to an unprivileged child would break a run that otherwise
    // works. Windows-side entries are left untouched — there is no posix uid to keep.
    if (s.platform == Strategy::Platform::Linux &&
        (PrivilegeHelper::isElevated() || s.requiresRoot))
        a = portableArgvWithUser(a);

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
    // Recorded for the rescue path: a payload this backend handed to pkexec runs as root, and an
    // unprivileged sweep could not signal it. One launched as the user's own child needs no prompt.
    m_payloadElevated = (*program == QLatin1String("pkexec"));
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

    // A rescue from the previous run is still editing the firewall. It cannot be detected through
    // m_process — the whole point of releasing a hung stop was to stop depending on that command —
    // so the claim is kept explicitly. The rescue is bounded by the watchdog, which is what makes
    // "wait a moment" an honest answer rather than a door that never opens: starting now would race
    // this run's insert against a delete that is still in flight.
    if (m_forcedStop)
        return fail(QStringLiteral("LinuxBackend: the previous run is still being cleaned up "
                                   "(wait for the cleanup command to finish, then start again)"));

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
    m_forcedStop = false;   // ditto its rescue: a released run's abandoned command is not this one's
    disarmStopWatchdog();

    // The name a forced stop sweeps by, recorded while the strategy is still in hand. A controller
    // records nothing: its daemon belongs to the init script, and a stop that goes silent there is
    // released with a message rather than a name-based kill the script was asked to perform.
    m_payloadName = m_controller ? QString() : QFileInfo(launchProgram(s)).fileName();

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
    // Bounded like every other privileged stop step: pkexec reached the init script is the optimistic
    // case, and an unattended authorisation prompt is the one that leaves a GUI stuck in Stopping.
    armStopWatchdog();
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

    // One `sh -c` for the whole phase, wrapped by the same helper the rescue command uses: a single
    // authorisation prompt, and no way for the two privileged paths to drift apart. Editing the
    // firewall is always a root operation, so needsRoot is unconditional here.
    return wrapPrivilegedScript(portableFirewallScript(m_queueNum, apply), true, program, args, error);
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

    // A teardown is a step the stop waits on, so it gets a deadline: an insert during a start does
    // not, because a prompt the user is still typing into is normal there — but a stop that never
    // comes back is not a stop. See onStopWatchdogFired().
    if (!apply)
        armStopWatchdog();
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
    // Say what the backend added to the user's flags, and why, before the child's own output starts:
    // a "--user=root" nobody asked for is exactly the sort of thing a user is entitled to object to,
    // and pkexec's prompt does not explain it. Only when we supplied it ourselves — an argv that
    // already names --user or --uid is the user's own privilege choice and needs no commentary.
    if (!portableArgvSelectsPrivileges(baseArgs(m_active)) &&
        (PrivilegeHelper::isElevated() || m_active.requiresRoot))
    {
        emit logMessage(QStringLiteral("LinuxBackend: adding --user=%1 to the payload: nfqws drops "
                                       "root by itself and would then be unable to create NFQUEUE %2")
                            .arg(QLatin1String(kPortableNfUser))
                            .arg(m_queueNum), 0);
    }

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
    // Recorded now, not when the payload launches: a stop that arrives during the firewall insert
    // still has to know what a later sweep should look for, and by then the payload has no name yet.
    m_payloadName = QFileInfo(launchProgram(s)).fileName();
    // One read of argv decides the number both halves use: the rules enqueue into it and nfqws binds
    // it, because portableQueueNum() recovers exactly what the parser wrote into --qnum=.
    m_queueNum = portableQueueNum(s.argv);
    // Rules a previous run could not confirm away are reported, not silently overwritten: m_fwApplied
    // is the only thing that survives a forced stop, and it means "these may still be in the kernel".
    if (m_fwApplied)
    {
        emit logMessage(QStringLiteral("LinuxBackend: a previous run could not confirm removing its "
                                       "rules from NFQUEUE %1; check 'iptables -t mangle -L OUTPUT' "
                                       "before assuming this queue is clean only")
                            .arg(m_queueNum),
                        1);
    }
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
        //
        // "Let it finish" cannot mean "for as long as it likes", though: this command was reached by
        // way of a pkexec prompt, and a prompt nobody answers is the hang the whole stop path exists
        // to avoid. The deadline abandons it rather than killing it.
        setState(State::Stopping);
        m_portableStopQueued = true;
        armStopWatchdog();
        emit logMessage(QStringLiteral("LinuxBackend: stop queued until the firewall command returns"), 1);
        return;
    }

    if (m_phase == PortablePhase::FirewallDown)
    {
        setState(State::Stopping);
        // The teardown the user is asking for is already in flight and already watched; say so
        // instead of re-arming a deadline that is already running against the same command.
        emit logMessage(QStringLiteral("LinuxBackend: the firewall teardown is already running"), 1);
        return;
    }

    if (m_phase == PortablePhase::Rescue)
    {
        setState(State::Stopping);
        emit logMessage(QStringLiteral("LinuxBackend: the forced cleanup is already running"), 1);
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
        // The handler moves the phase off Payload — to FirewallDown, or to None if there was nothing
        // left to remove. Staying on Payload means the child may be dead and Qt still cannot say so,
        // which is the notification this path used to wait on forever.
        if (child->waitForFinished(kTerminateGraceMs) && m_phase != PortablePhase::Payload)
            return;      // finished() already ran, and the teardown with it

        // Still silent after the shortest wait that a run this backend truly owns needs. terminate()
        // reached the pkexec wrapper and nobody passed the word on; the surviving payload is also what
        // keeps the pipes open, so the exit notification a normal stop is sequenced on may never arrive
        // at all. Waiting longer cannot change that — it only holds this thread, and the button, still.
        forceStop(QStringLiteral("LinuxBackend: nfqws did not report an exit within %1 ms of SIGTERM; "
                                 "forcing it down and cleaning up behind it")
                      .arg(kTerminateGraceMs));
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

    case PortablePhase::Rescue:
        // Unreachable by construction: onProcessFinished() intercepts every notification while a
        // rescue is in flight, and a rescued child is disconnected before it can report at all. The
        // branch is here so the phase machine says what it would do if that ever stopped being true —
        // release the run, because a stop must never depend on being heard.
        finishForcedStop(QStringLiteral("LinuxBackend: a rescue command reported an exit through the "
                                        "phase machine; the run is released anyway"));
        return;

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
    // A rescue is already running: it is bounded by the watchdog, and pressing Stop again would only
    // abandon the command that is cleaning up after the payload.
    if (m_forcedStop)
    {
        emit logMessage(QStringLiteral("LinuxBackend: the forced cleanup is already running"), 1);
        return;
    }

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
        QProcess *child = m_process;
        child->terminate();
        // m_directStop is cleared by the finished handler, so asking it whether the handler ran is how
        // this path tells "the child exited" from "the child exited AND Qt could say so". Qt withholds
        // finished() until the pipes reach EOF, and a payload that outlives its wrapper is precisely
        // what keeps them open — hence the second condition, not the exit status alone.
        if (child->waitForFinished(kTerminateGraceMs) && !m_directStop)
            return;
        // The wrapper is still alive — or it is dead and its payload inherited the pipes, so no
        // notification will ever come. Either way this is the shape pkexec produces, and waiting
        // longer buys nothing: take the rescue path instead of the kill() that used to be the end.
        forceStop(QStringLiteral("LinuxBackend: the payload wrapper did not report an exit within "
                                 "%1 ms of SIGTERM; forcing the run down")
                      .arg(kTerminateGraceMs));
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


// ---------------------------------------------------------------------------
// The forced stop: the path that does not depend on being heard
// ---------------------------------------------------------------------------

QString LinuxBackend::payloadProcessName() const
{
    return m_payloadName;
}

void LinuxBackend::goDeaf(QProcess *p)
{
    if (!p)
        return;

    if (m_process == p)
        m_process = nullptr;   // resetProcess() must not destroy — and so must not kill — this one

    p->disconnect(this);

    // Ownership leaves the backend deliberately: ~QProcess kills a running child, and an iptables
    // script stopped halfway through a delete is the one outcome this backend refuses to produce —
    // it leaves a rule set no later run can describe. The object reaps itself once the child really
    // reports; a child that never reports costs one orphaned QProcess, which is bounded, per
    // abandonment, and cheaper than a damaged firewall.
    p->setParent(nullptr);
    QObject::connect(p, &QProcess::finished, p, &QObject::deleteLater);
    QObject::connect(p, &QProcess::errorOccurred, p, [p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            p->deleteLater();
    });
}

bool LinuxBackend::composeRescueCommand(QString *program, QStringList *args, QString *error) const
{
    if (!program || !args)
    {
        if (error)
            *error = QStringLiteral("LinuxBackend: internal error: nowhere to put the rescue command");
        return false;
    }

    // The sweep inherits the payload's own privileges and the rule delete needs root, so root is
    // asked for when either is on the list — and never otherwise. The wrapper precedence is the
    // firewall phase's own (see wrapPrivilegedScript), because a rescue that reaches root a different
    // way than the rules were installed with is a rescue that prompts twice or not at all.
    return wrapPrivilegedScript(forceStopScript(payloadProcessName(), m_fwApplied, m_queueNum),
                                m_fwApplied || m_payloadElevated, program, args, error);
}

void LinuxBackend::armStopWatchdog()
{
    if (m_stopWatchdog)
        m_stopWatchdog->start();
}

void LinuxBackend::disarmStopWatchdog()
{
    if (m_stopWatchdog)
        m_stopWatchdog->stop();
}

void LinuxBackend::forceStop(const QString &reason)
{
    // One rescue per run. A second call would abandon the rescue command itself, leaving the GUI with
    // nothing waiting on it and a sweep it cannot describe.
    if (m_forcedStop)
        return;

    // Is the child that has gone quiet the payload wrapper, or one of this backend's own short
    // cleanup commands? Decided HERE, before the bookkeeping below rewrites m_phase, because m_phase
    // is what says: a payload is abandoned with SIGKILL, an iptables script is abandoned untouched —
    // interrupting a delete halfway is how a rule set gets damaged into a state no later run can
    // describe.
    QProcess *stalled = m_process;
    const bool stalledIsPayload =
        stalled && stalled->state() != QProcess::NotRunning &&
        ((m_portable && m_phase == PortablePhase::Payload) ||
         (!m_portable && !m_controller && !m_cmdIsStop));

    m_forcedStop = true;
    // The abandoned child's exit is not a report of a stop on request: it is disconnected below so
    // that a death this backend cannot confirm is never shown as one it caused.
    m_directStop = false;
    m_portableStopQueued = false;
    m_stopRequested = false;
    m_cmdIsStop = false;
    if (m_portable)
        m_phase = PortablePhase::Rescue;

    setState(State::Stopping);
    emit logMessage(reason, 1);

    if (stalledIsPayload)
    {
        emit logMessage(QStringLiteral("LinuxBackend: sending SIGKILL to the stalled payload "
                                       "wrapper (pid %1)").arg(stalled->processId()), 1);
        stalled->kill();
    }
    goDeaf(stalled);

    QString program;
    QStringList args;
    QString err;
    if (!composeRescueCommand(&program, &args, &err))
    {
        // No way to reach root at all. The GUI still moves on — that is the whole point — but it says
        // plainly that the payload was left alone.
        finishForcedStop(err.isEmpty()
                             ? QStringLiteral("LinuxBackend: could not compose the cleanup command; "
                                              "the payload may still be running")
                             : err);
        return;
    }

    resetProcess();
    m_process->setProgram(program);
    m_process->setArguments(args);
    if (!m_active.workingDirectory.isEmpty())
        m_process->setWorkingDirectory(m_active.workingDirectory);

    emit logMessage(QStringLiteral("LinuxBackend: sweeping '%1' by name%2")
                        .arg(payloadProcessName().isEmpty() ? QStringLiteral("nfqws")
                                                            : payloadProcessName(),
                             m_fwApplied ? QStringLiteral(", then removing the NFQUEUE rules")
                                         : QString()),
                    0);
    armStopWatchdog();
    m_process->start(program, args);
}

void LinuxBackend::finishForcedStop(const QString &warning)
{
    disarmStopWatchdog();

    if (!warning.isEmpty())
        emit logMessage(warning, 1);

    // A command still in flight keeps its own lifetime: it is deaf now, and whatever it eventually
    // reports cannot change anything about a run the GUI has already let go of.
    if (m_process && m_process->state() != QProcess::NotRunning)
        goDeaf(m_process);

    m_forcedStop = false;
    m_directStop = false;
    m_portableStopQueued = false;
    m_stopRequested = false;
    m_cmdIsStop = false;
    m_payloadName.clear();
    m_portable = false;
    m_phase = PortablePhase::None;

    if (!m_portablePendingError.isEmpty())
    {
        // The run already failed for a worse reason than a teardown that would not answer; say that
        // rather than pretend it ended the way the user asked.
        const QString msg = m_portablePendingError;
        m_portablePendingError.clear();
        m_lastError = msg;
        setState(State::Error);
        emit errorOccurred(msg);
        return;
    }
    setState(State::Stopped);
}

void LinuxBackend::onStopWatchdogFired()
{
    // Silence is not failure: an iptables script may still be running, and interrupting one is how a
    // rule set gets damaged. Nothing is killed on the deadline — the run is released, and the user is
    // told exactly where to look.
    const QString waited = QStringLiteral("did not answer within %1 s; the run is released without "
                                          "waiting for it")
                               .arg(kPrivilegedStepTimeoutMs / 1000);

    if (m_forcedStop)
    {
        finishForcedStop(QStringLiteral("LinuxBackend: the cleanup command %1. If a payload is still "
                                        "running, stop it by hand and check 'iptables -t mangle "
                                        "-L OUTPUT' for NFQUEUE rules on queue %2; give the abandoned "
                                        "command a moment before starting again")
                             .arg(waited)
                             .arg(m_queueNum));
        return;
    }

    if (m_controller)
    {
        // A controller stop command that will not answer — usually an authorisation prompt nobody is
        // there to approve — ends the run here. Its daemon belongs to the init script, so this
        // backend does not sweep by name a process it was told to leave to the service.
        finishForcedStop(QStringLiteral("LinuxBackend: the stop command %1; check 'service zapret "
                                        "status' for the daemon it was meant to stop")
                             .arg(waited));
        return;
    }

    // A normal stop stalled on one of its privileged commands. Fall through to the rescue, which the
    // same deadline then watches: the sweep runs, the rules are attempted again (idempotently — every
    // delete is guarded by a -C probe), and the GUI comes back either way.
    //
    // A queued stop whose INSERT went silent is the one case where walking away leaves the machine in
    // a state this backend cannot describe: the script may complete a moment later, with nobody left
    // to take the rules back out. So the rules are claimed installed, and the rescue deletes them —
    // removing a rule that never landed is a no-op, not an error.
    const bool stalledInsert = m_phase == PortablePhase::FirewallUp || m_portableStopQueued;
    if (stalledInsert)
        m_fwApplied = true;

    forceStop(QStringLiteral("LinuxBackend: the %1 command %2")
                  .arg(stalledInsert ? QStringLiteral("firewall setup")
                                     : QStringLiteral("firewall teardown"),
                       waited));
}

void LinuxBackend::onProcessStarted()
{
    // The rescue command starting is bookkeeping with a process attached, not a service coming up:
    // reporting Running here would badge a card for a payload that this stop is on its way to kill.
    if (m_forcedStop)
        return;

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

    // A notification this handler acts on means the privileged step a stop was waiting on has
    // answered, in whichever direction it answered: the deadline exists for silence, not for a bad
    // exit code. Re-armed by the next step that needs waiting (see armStopWatchdog).
    disarmStopWatchdog();

    // The rescue command ends a forced stop; it is not a step in the phase machine, and nothing may
    // read its sweep as a payload exit or its delete as an ordinary teardown. Its status says one
    // thing only — whether the rules came out — and then the run is released whatever it says.
    if (m_forcedStop)
    {
        const bool rulesClean = !crashed && code == 0;
        if (m_fwApplied)
        {
            if (rulesClean)
            {
                m_fwApplied = false;
                emit logMessage(QStringLiteral("LinuxBackend: nfqueue rules removed"), 0);
            }
            else
            {
                // Left installed on purpose: m_fwApplied stays true so the next start reports the
                // leftovers this one could not confirm away.
                emit logMessage(QStringLiteral("LinuxBackend: the NFQUEUE rule cleanup reported exit "
                                               "%1; check 'iptables -t mangle -L OUTPUT' for leftovers "
                                               "on queue %2")
                                    .arg(code)
                                    .arg(m_queueNum),
                                1);
            }
        }
        finishForcedStop();
        return;
    }

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
            // false "crash" an ordinary Stop click must not turn into.
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
        if (m_forcedStop)
        {
            // The sweep could not even be composed into a running command. The payload wrapper was
            // SIGKILLed before it was tried, so what is unverified here is the firewall, not the
            // run: release the GUI and leave m_fwApplied set so the next start says the rest.
            disarmStopWatchdog();
            finishForcedStop(msg);
            return;
        }
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

