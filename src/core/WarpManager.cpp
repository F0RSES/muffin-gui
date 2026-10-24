#include "WarpManager.h"

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

namespace zapret {

namespace {

// warp-cli's own timeouts are usually under a second, but a first "connect" has to talk to the
// daemon and register if the client was never enrolled — that round trip can take a while. 30 s
// is long enough for a slow first run and short enough that a wedged daemon shows up in the log
// rather than hanging the UI.
constexpr int kVerbTimeoutMs = 30000;

} // namespace

QString WarpManager::warpCliProgram()
{
    return QStringLiteral("warp-cli");
}

bool WarpManager::isAvailableFromPath(const QString &resolvedPath)
{
    return !resolvedPath.isEmpty();
}

QStringList WarpManager::statusArgs()
{
    return {QStringLiteral("--accept-tos"), QStringLiteral("status")};
}

QStringList WarpManager::connectArgs()
{
    return {QStringLiteral("--accept-tos"), QStringLiteral("connect")};
}

QStringList WarpManager::disconnectArgs()
{
    return {QStringLiteral("--accept-tos"), QStringLiteral("disconnect")};
}

WarpManager::State WarpManager::parseStatus(const QString &raw, bool wasConnectAttempt)
{
    // warp-cli's status answer is a single line ending in a state word, wrapped in an ISO
    // timestamp and the program's own log tag. Windows writes a shorter "Status: X" prefix;
    // macOS/Linux write "<date> warp-cli [INFO] X". Both spell the state the same way, so we
    // look for the words in order of confidence.
    //
    // Case-insensitive because the version on Windows capitalises "Connected" while the version
    // on Linux writes it the same way but sometimes prefixes "Already ". Any of these being a
    // substring of the whole answer is enough to name the state; the order below is what makes
    // "Disconnected" not match "Connected" first, since a "not connected" phrasing would
    // otherwise be read as connected.
    const QString t = raw.toLower();

    // Check the negatives first. "Disconnected" contains "connected", so if we scanned for
    // "connected" first every disconnected client would report as connected.
    if (t.contains(QStringLiteral("disconnected")))
        return State::Disconnected;
    if (t.contains(QStringLiteral("disconnecting")))
        return State::Disconnecting;

    // The daemon's "connecting" phase is often reported as the string "Connect pending" or
    // "Connecting" (Linux) or "Status: Connecting" (Windows).
    if (t.contains(QStringLiteral("connecting")) || t.contains(QStringLiteral("connect pending")))
        return State::Connecting;

    if (t.contains(QStringLiteral("connected")))
        return State::Connected;

    // A daemon that has never been enrolled says "Not registered" — from warp-cli's own
    // vocabulary. From the user's point of view that means "WARP is not set up on this
    // machine", and the honest answer depends on whether we just asked it to connect.
    if (t.contains(QStringLiteral("not registered")) || t.contains(QStringLiteral("unregistered")))
        return wasConnectAttempt ? State::Connecting : State::Disconnected;

    // No recognisable marker. The safest thing to say is "the tunnel is not up", because the
    // alternative — reporting Connected on an unparseable answer — is a claim that would keep
    // the button green when the daemon has actually gone away.
    return State::Disconnected;
}

WarpManager::WarpManager(QObject *parent)
    : QObject(parent)
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    m_pollTimer->setTimerType(Qt::CoarseTimer);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        // A poll is not a user request — if a verb is already in flight, drop this tick rather
        // than queueing a second warp-cli behind it and having the pair disagree about the state.
        if (m_process)
            return;
        QString err;
        refresh(&err);
    });

    rescanAvailability();
}

WarpManager::~WarpManager()
{
    // Kill any warp-cli still running: a status probe that outlives us would be a stray
    // subprocess with no owner. QProcess does the right thing when its parent is destroyed, but
    // the explicit kill makes the intent readable at a glance.
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

void WarpManager::rescanAvailability()
{
    const QString resolved = QStandardPaths::findExecutable(warpCliProgram());
    const bool now = isAvailableFromPath(resolved);
    if (now == m_available)
        return;
    m_available = now;
    emit availabilityChanged(now);
    // The state a moment ago ("Unavailable") is not true any more, so say so. If WARP became
    // available the next refresh will replace this with the daemon's real answer; if it went
    // away, nothing can.
    setState(now ? State::Disconnected : State::Unavailable);
    if (now) {
        QString err;
        refresh(&err);
    }
}

void WarpManager::setPollingEnabled(bool on)
{
    m_pollEnabled = on;
    if (!m_pollTimer)
        return;
    if (on && m_available)
        m_pollTimer->start();
    else
        m_pollTimer->stop();
}

bool WarpManager::start(QString *error)
{
    return startVerb(connectArgs(), State::Connecting, true, error);
}

bool WarpManager::stop(QString *error)
{
    return startVerb(disconnectArgs(), State::Disconnecting, false, error);
}

bool WarpManager::refresh(QString *error)
{
    return startVerb(statusArgs(), m_state, false, error);
}

bool WarpManager::startVerb(const QStringList &args, State onAccepted, bool wasConnectAttempt,
                            QString *error)
{
    if (!m_available) {
        if (error)
            *error = QStringLiteral("WarpManager: warp-cli is not installed on this machine.");
        return false;
    }
    if (m_process) {
        if (error)
            *error = QStringLiteral("WarpManager: another warp-cli request is still running.");
        return false;
    }

    m_lastVerbWasConnect = wasConnectAttempt;
    m_pendingState = onAccepted;

    auto *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProgram(warpCliProgram());
    proc->setArguments(args);

    connect(proc, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        onVerbFinished();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        // FailedToStart is the only case that never produces finished(); every other error is
        // reported after the exit signal arrives. If we do not clear the pointer here a
        // wedged QProcess keeps every subsequent verb from starting at all.
        if (e != QProcess::FailedToStart)
            return;
        if (m_process == proc)
            m_process = nullptr;
        const QString text = proc->errorString();
        proc->deleteLater();
        emit logMessage(QStringLiteral("WarpManager: warp-cli could not be started (%1)")
                            .arg(text), 2);
        emit errorOccurred(text);
        setState(State::Error);
    });

    m_process = proc;
    proc->start();
    // Report the intent immediately: warp-cli's connect verb returns in a fraction of a second
    // but the tunnel does not actually come up until the daemon finishes the handshake. Without
    // this transition the button would look like nothing had happened for the whole round trip.
    if (m_state != onAccepted)
        setState(onAccepted);

    // Bound the wait even though warp-cli is normally quick. A daemon that has hung would
    // otherwise strand us in Connecting forever.
    QTimer::singleShot(kVerbTimeoutMs, proc, [this, proc]() {
        if (m_process != proc)
            return;   // the finished signal already beat us here
        proc->kill();
    });
    return true;
}

void WarpManager::onVerbFinished()
{
    if (!m_process)
        return;
    QProcess *proc = m_process;
    m_process = nullptr;

    const QString out = QString::fromUtf8(proc->readAll()).trimmed();
    const int exitCode = proc->exitCode();
    const bool crashed = proc->exitStatus() == QProcess::CrashExit;
    proc->deleteLater();

    if (crashed || exitCode != 0) {
        const QString reason = out.isEmpty()
            ? QStringLiteral("warp-cli exited with code %1").arg(exitCode)
            : out;
        emit logMessage(QStringLiteral("WarpManager: %1").arg(reason), 2);
        emit errorOccurred(reason);
        setState(State::Error);
        return;
    }

    // A status probe's stdout is the only place the daemon's truth lives; connect/disconnect
    // produce a short acknowledgement that does not itself carry the state.
    const State parsed = parseStatus(out, m_lastVerbWasConnect);
    // If the parse said "Disconnected" but we just asked for a connect, that usually means the
    // daemon acknowledged the request and is still completing the handshake. Keep the pending
    // state so the button does not flip back to Idle for a run that is on its way up; the next
    // poll (five seconds later) reads the real answer.
    if (m_lastVerbWasConnect && parsed == State::Disconnected && m_pendingState == State::Connecting) {
        setState(State::Connecting);
    } else {
        setState(parsed);
    }
    m_lastVerbWasConnect = false;
}

void WarpManager::setState(State s)
{
    if (s == m_state)
        return;
    m_state = s;
    emit stateChanged(s);
}

} // namespace zapret

