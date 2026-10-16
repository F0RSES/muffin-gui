#include "WindowsBackend.h"

#include "PrivilegeHelper.h"

#include <QFileInfo>
#include <QProcess>
#include <QStringList>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace zapret {

WindowsBackend::WindowsBackend(QObject *parent)
    : IZapretBackend(parent)
{
    m_process = new QProcess(this);
    // A finished() connection that mirrors (int, ExitStatus) keeps the slots portable across Qt
    // patch versions whose overload set differs; lambda form binds unambiguously.
    connect(m_process, &QProcess::started, this, &WindowsBackend::onProcessStarted);
    connect(m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                onProcessFinished(code, status == QProcess::CrashExit);
            });
    connect(m_process, &QProcess::errorOccurred, this, &WindowsBackend::onProcessError);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &WindowsBackend::onReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &WindowsBackend::onReadyReadStderr);
}

WindowsBackend::~WindowsBackend()
{
    // Do not wait for the child on destruction: killing a winws.exe mid-flight from the GUI teardown
    // path is the caller's decision (stop() first). We just ensure the QProcess object goes away.
}

void WindowsBackend::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(s);
}

void WindowsBackend::resetProcess()
{
    // Replace the managed process so a fresh start never inherits buffered output or a stale error
    // from the previous run.
    if (m_process)
    {
        m_process->disconnect(this);
        m_process->deleteLater();
    }
    m_process = new QProcess(this);
    connect(m_process, &QProcess::started, this, &WindowsBackend::onProcessStarted);
    connect(m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                onProcessFinished(code, status == QProcess::CrashExit);
            });
    connect(m_process, &QProcess::errorOccurred, this, &WindowsBackend::onProcessError);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &WindowsBackend::onReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &WindowsBackend::onReadyReadStderr);
}

bool WindowsBackend::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

bool WindowsBackend::start(const Strategy &s, QString *error)
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

    if (isRunning() || m_state == State::Starting)
        return fail(QStringLiteral("WindowsBackend: a process is already managed (stop first)"));

    if (!s.isValid())
        return fail(QStringLiteral("WindowsBackend: strategy is not valid"));

    // winws.exe loads the WinDivert driver; that only succeeds with an elevated token. QProcess
    // inherits this process's token, so we cannot silently elevate a child — the GUI must already be
    // running elevated (PrivilegeHelper::runElevated). Detect it up front for a clear message.
    if (s.requiresRoot && !PrivilegeHelper::isElevated())
        return fail(QStringLiteral("WindowsBackend: not elevated — relaunch the application as "
                                   "administrator to load the WinDivert driver"));

    const QString program = launchProgram(s);
    if (program.isEmpty())
        return fail(QStringLiteral("WindowsBackend: strategy has no executable"));

    QStringList args = baseArgs(s);

    // When the strategy was NOT resolved to a direct winws.exe invocation it is still the wrapper
    // form applyDefaults produced (executablePath = cmd.exe / powershell.exe with the payload path
    // as primaryArg). Reconstruct the launcher's own argument conventions.
    if (s.argv.isEmpty())
    {
        const QString p = program.toLower();
        if (p.endsWith(QLatin1String("cmd.exe")) && !s.primaryArg.isEmpty())
            args = {QStringLiteral("/c"), s.primaryArg};
        else if (p.endsWith(QLatin1String("powershell.exe")) && !s.primaryArg.isEmpty())
            args = {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                    QStringLiteral("Bypass"), QStringLiteral("-File"), s.primaryArg};
    }

    // Synchronous existence check for an explicit path (winws.exe, a full nfqws path). Bare names
    // like "cmd.exe" resolve through PATH and are validated asynchronously by QProcess instead.
    const QFileInfo fi(program);
    const bool explicitPath =
        fi.isAbsolute() || program.contains(QLatin1Char('/')) || program.contains(QLatin1Char('\\'));
    if (explicitPath && !fi.exists())
        return fail(QStringLiteral("WindowsBackend: executable not found: %1").arg(program));

    resetProcess();
    m_process->setProgram(program);
    m_process->setArguments(args);
    if (!s.workingDirectory.isEmpty())
        m_process->setWorkingDirectory(s.workingDirectory);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_stopByRequest = false;   // a new run starts clean: its exits are never "on request"

#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    // Hide the console window winws.exe would otherwise flash when launched from a GUI process. The
    // only supported way to reach CreateProcess' flags is QProcess' create-process arguments modifier
    // (Qt 6.6+): a void lambda handed the struct Qt is about to pass to CreateProcessW. On an older
    // Qt the flag is unreachable, so the console may blink — cosmetic only, and not worth a second
    // launch path. Compiled only for Windows targets; guarded by Q_OS_WIN above.
    m_process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *cpa) {
        cpa->flags |= CREATE_NO_WINDOW;
    });
#endif

    m_lastError.clear();
    setState(State::Starting);
    emit logMessage(QStringLiteral("WindowsBackend: launching %1 %2")
                        .arg(program, args.join(QLatin1Char(' ')))
                        .trimmed(), 0);

    // QProcess::start() is non-blocking; a FailedToStart (bad PATH name, etc.) arrives via
    // errorOccurred and moves us to Error asynchronously. start() itself only reports the launch
    // request was accepted, unless we already flipped to Error synchronously above.
    m_process->start(program, args);
    if (m_state == State::Error)
    {
        if (error)
            *error = m_lastError;
        return false;
    }
    return true;
}

void WindowsBackend::stop()
{
    if (!m_process || m_process->state() == QProcess::NotRunning)
    {
        m_stopByRequest = false;
        setState(State::Stopped);
        return;
    }
    // Set before terminate(): waitForFinished() below can deliver finished() synchronously, and the
    // handler has to already know that this death is ours.
    m_stopByRequest = true;
    setState(State::Stopping);
    emit logMessage(QStringLiteral("WindowsBackend: terminating managed process"), 0);
    m_process->terminate();
    // Escalate to kill if it ignores terminate. waitForFinished blocks briefly (<=3s) but only in the
    // explicit user-initiated stop path, which is acceptable for a GUI already expecting a shutdown.
    if (!m_process->waitForFinished(3000))
        m_process->kill();
}


void WindowsBackend::onProcessStarted()
{
    setState(State::Running);
    emit logMessage(QStringLiteral("WindowsBackend: process started (pid %1)")
                        .arg(m_process->processId()), 0);
}

void WindowsBackend::onProcessFinished(int exitCode, bool crashed)
{
    if (m_stopByRequest)
    {
        // We asked for this shutdown. Whichever signal took the child, or whatever code it returned on
        // the way out, the run ended as intended: reporting "crashed" here would put a red badge on a
        // card the user simply stopped.
        m_stopByRequest = false;
        emit logMessage(QStringLiteral("WindowsBackend: process stopped on request"), 0);
        setState(State::Stopped);
        return;
    }

    if (crashed)
    {
        m_lastError = QStringLiteral("WindowsBackend: process crashed (exit %1)").arg(exitCode);
        emit logMessage(m_lastError, 2);
        setState(State::Error);
        emit errorOccurred(m_lastError);
    }
    else
    {
        emit logMessage(QStringLiteral("WindowsBackend: process exited with code %1").arg(exitCode),
                        exitCode == 0 ? 0 : 1);
        setState(State::Stopped);
    }
}

void WindowsBackend::onProcessError()
{
    const QProcess::ProcessError e = m_process ? m_process->error() : QProcess::UnknownError;

    // Qt emits errorOccurred(Crashed) *before* finished() for every signalled exit — including the
    // SIGTERM our own stop() sends. finished() is handled right after this and already knows the death
    // was requested, so reporting it here too would put a crash badge and a dialog on a clean stop.
    if (e == QProcess::Crashed && m_stopByRequest)
        return;

    QString msg;
    switch (e)
    {
    case QProcess::FailedToStart:
        msg = QStringLiteral("WindowsBackend: failed to start: %1")
                  .arg(m_process ? m_process->errorString() : QString());
        break;
    case QProcess::Crashed:
        msg = QStringLiteral("WindowsBackend: process crashed");
        break;
    default:
        msg = QStringLiteral("WindowsBackend: process error (%1)").arg(int(e));
        break;
    }
    m_lastError = msg;
    emit logMessage(msg, 2);
    setState(State::Error);
    emit errorOccurred(msg);
}

void WindowsBackend::onReadyReadStdout()
{
    if (!m_process)
        return;
    const QByteArray data = m_process->readAllStandardOutput();
    if (!data.isEmpty())
        emit logMessage(QString::fromLocal8Bit(data).trimmed(), 0);
}

void WindowsBackend::onReadyReadStderr()
{
    if (!m_process)
        return;
    const QByteArray data = m_process->readAllStandardError();
    if (!data.isEmpty())
        emit logMessage(QString::fromLocal8Bit(data).trimmed(), 1);
}

} // namespace zapret

