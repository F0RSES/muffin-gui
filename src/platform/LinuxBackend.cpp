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

    m_controller = (s.kind == Strategy::Kind::ShellScript);

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

void LinuxBackend::stop()
{
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
    // Only the direct-payload mode has a long-lived child; the controller command merely began to
    // run. For controller we set Running in the finished handler based on its exit code.
    if (!m_controller)
        setState(State::Running);
}

void LinuxBackend::onProcessFinished(int exitCode, bool crashed)
{
    const int code = crashed ? -1 : exitCode;

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

