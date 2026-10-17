#include "ZapretPaths.h"

#include <QDir>
#include <QFileInfo>
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
