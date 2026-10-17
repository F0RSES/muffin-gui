#include "ZapretPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QStringList>
#include <QSysInfo>

namespace zapret {

namespace {

// Join `root` with a relative path using '/' regardless of host separator; QDir/QFileInfo accept
// '/' on Windows too, which keeps these helpers identical on both platforms.
QString rel(const QString &root, const QString &sub)
{
    if (root.isEmpty())
        return sub;
    if (root.endsWith(QLatin1Char('/')) || root.endsWith(QLatin1Char('\\')))
        return root + sub;
    return root + QLatin1Char('/') + sub;
}

bool exists(const QString &path)
{
    return !path.isEmpty() && QFileInfo::exists(path);
}

// The directory one level above `path`, read off the name alone. QFileInfo answers for a path that does
// not exist either, which is exactly the case here: the source folder has just been renamed away, and
// the shells that held it are what is left to look at. QDir::cdUp() would refuse to move out of a
// directory it cannot open.
QString parentOf(const QString &path)
{
    return QFileInfo(QDir::cleanPath(path)).dir().absolutePath();
}

// Remove `dir` if — and only if — it is an empty directory that the current install does not depend
// on. QDir::rmdir cannot remove a directory holding anything, and the check here says so plainly
// rather than trusting that call's failure; a legacy folder holding something the user or another
// program put there therefore survives untouched.
//
// The protection is by name, not by luck: an identity that resolves to the same folder as this build's
// (an organization change that changed nothing, an empty organization name) would otherwise have the
// live app-data directory deleted out from under it, which is a far worse bug than the empty folder
// this is meant to sweep up. Anything that is an ancestor or a descendant of a protected path is
// refused too, so no amount of path aliasing reaches the running install.
void removeEmptyDir(const QString &dir, const QStringList &protectedDirs)
{
    const QString cleaned = QDir::cleanPath(dir);
    if (cleaned.isEmpty() || cleaned == QStringLiteral("/"))
        return;

    for (const QString &raw : protectedDirs)
    {
        const QString p = QDir::cleanPath(raw);
        if (p.isEmpty())
            continue;
        if (cleaned == p
            || cleaned.startsWith(p + QLatin1Char('/'))
            || p.startsWith(cleaned + QLatin1Char('/')))
            return;
    }

    QDir candidate(cleaned);
    if (!candidate.exists() || !candidate.isEmpty())
        return;
    QDir().rmdir(cleaned);
}

// Can `fi` be executed? The honest answer is platform-dependent, and the difference is not cosmetic.
//
// On POSIX the exec bit is real, and install_bin.sh can leave a binary that exists but cannot be
// exec'd, so the bit has to be checked.
//
// On Windows it cannot be. Qt's ACL-based permission lookup is OFF by default
// (qt_ntfs_permission_lookup == 0 in qfilesystemengine_win.cpp), so the fallback derives the exec
// permissions from the file *extension* alone -- ".exe"/".com"/".bat"/".pif"/".cmd" -- and
// QFile::setPermissions() forwards only read/write to _wchmod, silently discarding
// ExeOwner/ExeGroup/ExeOther. An extension-less ELF payload like nfq/nfqws therefore reads as
// non-executable no matter what is on disk. Treating that as "not installed" would be a permanent
// false negative misranking every linux tree viewed from Windows, so there the criterion is the
// presence of a regular file: what install_bin.sh leaves behind and what an unpacked tree
// (nfq/ = sources only) does not have.
bool isRunnableNfqws(const QFileInfo &fi)
{
#if defined(Q_OS_WIN)
    return fi.isFile();
#else
    return fi.isFile() && fi.isExecutable();
#endif
}

// Adopt a different organization for the duration of one scope, then put the previous one back.
//
// QStandardPaths derives the app-specific locations from the process-wide organization/application
// pair and offers no way to ask it the same question about another organization, so adopting that
// identity is the only faithful way to compute a legacy path: it reuses Qt's own per-platform rule
// (~/.local/share/<org>/<app>, %APPDATA%\<org>\<app>, ~/Library/Application Support/<app>) instead of
// a second copy of that rule here, which a Qt release could contradict. Restored on every exit path,
// including an early return.
//
// Not thread-safe, by design: the only caller runs once, from the entry point, before any second
// thread exists.
class ScopedOrganization
{
public:
    explicit ScopedOrganization(const QString &organizationName)
        : m_previous(QCoreApplication::organizationName())
    {
        m_changed = (m_previous != organizationName);
        if (m_changed)
            QCoreApplication::setOrganizationName(organizationName);
    }
    ~ScopedOrganization()
    {
        if (m_changed)
            QCoreApplication::setOrganizationName(m_previous);
    }
    ScopedOrganization(const ScopedOrganization &) = delete;
    ScopedOrganization &operator=(const ScopedOrganization &) = delete;

private:
    QString m_previous;
    bool m_changed = false;
};

} // namespace

QString ZapretPaths::winwsPath(const QString &root) { return rel(root, QStringLiteral("bin/winws.exe")); }
QString ZapretPaths::listsDir(const QString &root) { return rel(root, QStringLiteral("lists")); }
QString ZapretPaths::binDir(const QString &root) { return rel(root, QStringLiteral("bin")); }
QString ZapretPaths::serviceBatPath(const QString &root) { return rel(root, QStringLiteral("service.bat")); }

QString ZapretPaths::nfqwsArchPath(const QString &root, const QString &archDirName)
{
    if (archDirName.isEmpty())
        return QString();
    return rel(root, QStringLiteral("binaries/") + archDirName + QStringLiteral("/nfqws"));
}

QString ZapretPaths::nativeArchDirName()
{
    // QSysInfo returns "x86_64", "arm64" (mac), "aarch64", "arm", "i386" ... Map to binaries/ names.
    const QString cpu = QSysInfo::currentCpuArchitecture().toLower();
    if (cpu == QLatin1String("x86_64") || cpu == QLatin1String("amd64"))
        return QStringLiteral("linux-x86_64");
    if (cpu == QLatin1String("arm64") || cpu == QLatin1String("aarch64"))
        return QStringLiteral("linux-arm64");
    if (cpu == QLatin1String("i386") || cpu == QLatin1String("x86"))
        return QStringLiteral("linux-x86");
    return QString();
}

QString ZapretPaths::managedInstallDirFor(const QString &organizationName)
{
    const ScopedOrganization org(organizationName);
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        return QString();
    return rel(base, QStringLiteral("zapret"));
}

QString ZapretPaths::managedInstallDir()
{
    // The identity the entry point set is the one this build uses; passing it through the same helper
    // the legacy lookup uses keeps the two answers computed by exactly the same rule.
    return managedInstallDirFor(QCoreApplication::organizationName());
}

QString ZapretPaths::legacyOrganizationName()
{
    return QStringLiteral("Muffin");
}

QString ZapretPaths::legacyManagedInstallDir()
{
    return managedInstallDirFor(legacyOrganizationName());
}

bool ZapretPaths::moveManagedInstallDir(const QString &from, const QString &to)
{
    if (from.isEmpty() || to.isEmpty())
        return false;

    const QFileInfo fromInfo(from);
    if (!fromInfo.isDir())
        return false;                        // nothing was ever installed under the old name
    if (QFileInfo::exists(to))
        return false;                        // a tree already sits there: never clobber it

    // Two names for one directory — an identity change that does not actually move anything, or a
    // symlink pointing at itself — is a no-op, not a move. canonicalFilePath() is empty for a path
    // that does not exist yet, so this can only match a real overlap.
    if (fromInfo.canonicalFilePath() == QFileInfo(to).canonicalFilePath())
        return false;

    // rename() needs the target's PARENT to exist, and creating the target itself first would trip
    // the clobber check above. mkpath walks the whole chain, so a first launch after the identity
    // change (~/.local/share/F0RSES/ does not exist yet) still succeeds.
    const QFileInfo toInfo(to);
    if (!toInfo.dir().exists() && !QDir().mkpath(toInfo.dir().absolutePath()))
        return false;                        // nowhere to put it: leave the source exactly as it was

    // One step, not a copy: this is rename(2) on POSIX and MoveFile on Windows, so the whole tree
    // moves at once and a failure (cross-device link, for instance) leaves the source untouched —
    // which is the property that makes this safe to run unattended at startup, on a folder holding a
    // download the user cannot get again offline.
    return QFile::rename(from, to);
}

bool ZapretPaths::migrateManagedInstallDir()
{
    const QString from = legacyManagedInstallDir();
    const QString to = managedInstallDir();
    if (from.isEmpty() || to.isEmpty())
        return false;

    const bool moved = moveManagedInstallDir(from, to);

    // What a rename leaves behind: the two folders that existed only to hold the tree —
    // "<data>/Muffin/Muffin" and "<data>/Muffin". Sweeping them up is the point of this migration (the
    // name read twice in the file manager is what the user noticed), and it also erases the empty
    // directory Qt's own writableLocation() creates while computing a legacy path on a machine that
    // never had one. Only genuinely empty directories go, never the shared data root, and never a
    // location this build resolves to — see removeEmptyDir().
    const QString legacyAppDir = parentOf(from);          // e.g. <data>/Muffin/Muffin
    const QString legacyOrgDir = parentOf(legacyAppDir);  // e.g. <data>/Muffin
    QStringList protectedDirs;
    protectedDirs << parentOf(to) << parentOf(parentOf(to));   // this build's own app and org folders
    const QString dataRoot =
        QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));

    for (const QString &leftover : { legacyAppDir, legacyOrgDir })
    {
        if (dataRoot.isEmpty() || QDir::cleanPath(leftover) == dataRoot)
            continue;   // never sweep a location shared with other applications
        removeEmptyDir(leftover, protectedDirs);
    }

    return moved;
}

QString ZapretPaths::nfqwsPath(const QString &root)
{
    // Preference order: native arch, then the common x86_64 / x86 packaged binaries, then a
    // root-level nfqws left behind by install_bin.sh. First existing candidate wins.
    const QStringList candidates = {
        nfqwsArchPath(root, nativeArchDirName()),
        nfqwsArchPath(root, QStringLiteral("linux-x86_64")),
        nfqwsArchPath(root, QStringLiteral("linux-x86")),
        rel(root, QStringLiteral("nfqws")),
    };
    for (const QString &c : candidates)
        if (exists(c))
            return c;
    return QString();
}

QString ZapretPaths::sysvScriptPath(const QString &root) { return rel(root, QStringLiteral("init.d/sysv/zapret")); }
QString ZapretPaths::openrcScriptPath(const QString &root) { return rel(root, QStringLiteral("init.d/openrc/zapret")); }
QString ZapretPaths::systemdUnitPath(const QString &root) { return rel(root, QStringLiteral("init.d/systemd/zapret.service")); }
QString ZapretPaths::configDefaultPath(const QString &root) { return rel(root, QStringLiteral("config.default")); }
QString ZapretPaths::nfqwsBuiltPath(const QString &root) { return rel(root, QStringLiteral("nfq/nfqws")); }

bool ZapretPaths::isLinuxInstalled(const QString &root)
{
    // "Installed" here means one specific, observable thing: the sysv/openrc init script, when asked
    // to start, will find the binary it execs. init.d/sysv/functions hardcodes
    //
    //   NFQWS="$ZAPRET_BASE/nfq/nfqws"
    //
    // and $ZAPRET_BASE resolves to the tree root (readlink of init.d/<form>/../..). A freshly
    // unpacked tree ships nfq/ as *sources only* (Makefile, *.c, *.h), so the script's start path
    // fails with "nfqws not found" even though everything else is in place. nfq/nfqws existing and
    // being executable is therefore exactly the condition that separates a working InitScript
    // strategy from one that is guaranteed to fail, and install_bin.sh / install_easy.sh are what
    // create it.
    //
    // Deliberately NOT consulted: /etc/init.d/zapret or systemctl state. Those say the service is
    // registered, which the GUI does not care about — it cares whether `pkexec <script> start` works.
    //
    // isRunnableNfqws() is why the executable half of that sentence means different things per
    // platform; see its comment.
    const QFileInfo fi(nfqwsBuiltPath(root));
    return fi.exists() && isRunnableNfqws(fi);
}

QString ZapretPaths::binariesDir(const QString &root) { return rel(root, QStringLiteral("binaries")); }

bool ZapretPaths::isWindowsRoot(const QString &root)
{
    if (root.isEmpty())
        return false;
    if (exists(winwsPath(root)))
        return true;
    // A windows tree without its payload still ships the launcher batch files + service.bat.
    QDir dir(root);
    if (!dir.exists())
        return false;
    const bool hasService = exists(serviceBatPath(root));
    const QStringList bats = dir.entryList({QStringLiteral("*.bat"), QStringLiteral("*.cmd")},
                                           QDir::Files, QDir::Name);
    return hasService && !bats.isEmpty();
}

bool ZapretPaths::isLinuxRoot(const QString &root)
{
    if (root.isEmpty())
        return false;
    if (exists(sysvScriptPath(root)))
        return true;
    if (!nfqwsPath(root).isEmpty())
        return true;
    QDir dir(root);
    return dir.exists() && QDir(rel(root, QStringLiteral("init.d"))).exists();
}

Strategy::Platform ZapretPaths::detectPlatform(const QString &root)
{
    if (isWindowsRoot(root))
        return Strategy::Platform::Windows;
    if (isLinuxRoot(root))
        return Strategy::Platform::Linux;
    return Strategy::Platform::Unknown;
}

} // namespace zapret
