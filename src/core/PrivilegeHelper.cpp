#include "PrivilegeHelper.h"

#include "WindowsArgs.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace zapret {

bool PrivilegeHelper::isElevated()
{
#if defined(Q_OS_WIN)
    // TokenElevation yields a TOKEN_ELEVATION whose single DWORD is non-zero only for a process
    // launched "as administrator" (or a builtin Administrator account). QProcess inherits this token,
    // so winws.exe's WinDivert driver load fails silently unless we already hold an elevated token.
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    TOKEN_ELEVATION elev = {};
    DWORD len = 0;
    const BOOL got = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &len);
    CloseHandle(token);
    return got != FALSE && elev.TokenIsElevated != 0;
#else
    return geteuid() == 0;
#endif
}

bool PrivilegeHelper::runElevated(const QString &program, const QStringList &args, QString *error)
{
    if (program.isEmpty())
    {
        if (error)
            *error = QStringLiteral("runElevated: empty program");
        return false;
    }

#if defined(Q_OS_WIN)
    // ShellExecuteEx with the "runas" verb triggers the UAC consent prompt and, on consent, starts
    // the process elevated. lpParameters is a bare string that goes straight into the child's command
    // line with nothing in between: unlike QProcess, Qt is not involved and quotes nothing here, so
    // this is one of the places where quoting is ours to do. WindowsArgs::winQuoteArg applies the
    // same rule QProcess applies to its own arguments, which covers the cases the old
    // "wrap it if it contains a space" test did not: an argument carrying a quote of its own, one
    // holding a tab, and one ending in a backslash that would otherwise escape the closing quote and
    // swallow the next argument. A path with a space in it -- a profile folder like
    // "C:\Users\KDFX Modes" -- is exactly what arrives here when the app relaunches itself elevated.
    const QString params = winComposeCommandLine(QString(), args);

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    const QString nativeFile = QDir::toNativeSeparators(program);
    sei.lpFile = reinterpret_cast<LPCWSTR>(nativeFile.toStdWString().c_str());
    sei.lpParameters = reinterpret_cast<LPCWSTR>(params.toStdWString().c_str());
    sei.nShow = SW_HIDE;

    if (ShellExecuteExW(&sei))
        return true;

    const DWORD err = GetLastError();
    if (error)
    {
        *error = (err == ERROR_CANCELLED)
                     ? QStringLiteral("UAC prompt was declined")
                     : QStringLiteral("ShellExecuteEx failed (code %1)").arg(err);
    }
    return false;
#else
    // Delegate to pkexec: it authenticates via the polkit agent and re-executes the target as root.
    QStringList full;
    full << program;
    full.append(args);
    if (QProcess::startDetached(QStringLiteral("pkexec"), full))
        return true;
    if (error)
        *error = QStringLiteral("failed to start 'pkexec %1'").arg(program);
    return false;
#endif
}

bool PrivilegeHelper::hasPolkitAgent()
{
#if defined(Q_OS_WIN)
    // Windows elevation is handled by the SCM/UAC, not polkit - there is always an "agent".
    return true;
#else
    // A precise check needs a D-Bus call to org.freedesktop.PolicyKit1 (which would pull in QtDBus).
    // We stay QtCore-only and approximate: pkexec must exist and we must be in a graphical session,
    // because a polkit authentication agent only runs under a desktop. That is enough for the
    // backend to give a clear message instead of hanging on an unattended pkexec.
    if (!QFileInfo::exists(QStringLiteral("/usr/bin/pkexec")))
        return false;
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    return env.contains(QStringLiteral("DISPLAY")) || env.contains(QStringLiteral("WAYLAND_DISPLAY"))
           || env.contains(QStringLiteral("XDG_SESSION_TYPE"));
#endif
}

} // namespace zapret
