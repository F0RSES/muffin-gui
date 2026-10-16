#pragma once

#include <QString>
#include <QStringList>

namespace zapret {

// Elevation helpers isolated from the backends so the (platform-specific) privilege dance lives in
// exactly one place. Every function is a no-op-safe, host-portable static; the OS branches are hidden
// inside PrivilegeHelper.cpp via #ifdef so callers - and the headless test harness - never compile
// Windows headers on Linux or vice versa.
class PrivilegeHelper
{
public:
    PrivilegeHelper() = delete;

    // True when the current process already runs with the privileges zapret needs
    // (Windows: elevated token; Linux/macOS: euid == 0).
    static bool isElevated();

    // Launch `program` with `args` requesting elevation. Returns as soon as the launch request is
    // handed off (fire-and-forget): true means "started", NOT "succeeded". On Windows this shows the
    // UAC prompt via ShellExecuteEx "runas"; on Linux it goes through pkexec. Fills *error on a
    // failure to even start.
    static bool runElevated(const QString &program, const QStringList &args, QString *error);

    // Whether a polkit authentication agent is reachable (Linux). When false, pkexec cannot prompt
    // and elevation will fail with no UI - the backend uses this to produce a clear message.
    // Always true on Windows (UAC is not mediated by polkit).
    static bool hasPolkitAgent();
};

} // namespace zapret
