#include "MuffinInstallHelper.h"

#include "PrivilegeHelper.h"
#include "ZapretPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace zapret {

namespace {

// Resolve a coreutils program WITHOUT consulting PATH. The caller is root, PATH is the one environment
// value an invoking context can still influence (pkexec hands its environment on, and a build tree, a
// sudo shell and a desktop session do not agree on what is in it), and a helper that exec'd "$PATH/cp"
// could be given somebody else's `cp` by a directory it never chose. /usr/bin is the answer on any
// merged-usr system, /bin on the rest, and both are root-owned.
QString systemTool(const QString &name)
{
    const QStringList candidates = {
        QStringLiteral("/usr/bin/") + name,
        QStringLiteral("/bin/") + name,
    };
    for (const QString &c : candidates)
        if (QFileInfo::exists(c))
            return c;
    return QString();
}

// The parent of a clean absolute path, read off the name. QFileInfo answers for a path that does not
// exist yet — and the namespace directory frequently does not, on the first promote of a fresh install.
QString parentOf(const QString &path)
{
    return QFileInfo(QDir::cleanPath(path)).dir().absolutePath();
}

// Is `path` inside `prefix`, treating `prefix` as a directory rather than as a string? The trailing
// separator is what keeps "/opt/muffin/zapret" from matching "/opt/muffin/zapret.new" — the two sibling
// names a promotion creates beside the tree itself.
bool isInside(const QString &path, const QString &prefix)
{
    if (prefix.isEmpty() || path.isEmpty())
        return false;
    const QString clean = QDir::cleanPath(path);
    const QString base = QDir::cleanPath(prefix);
    return clean == base || clean.startsWith(base + QLatin1Char('/'));
}

} // namespace

QString MuffinInstallHelper::programName()
{
    return QStringLiteral("muffin-install-helper");
}

QString MuffinInstallHelper::installedProgram()
{
    // CMakeLists.txt installs the target to `lib/muffin` under the /usr prefix, and
    // packaging/check-polkit-rule.js reads that DESTINATION back out of the CMake file and asserts the
    // polkit rule refuses the resulting path — which is what makes this a fact rather than a habit.
    return QStringLiteral("/usr/lib/muffin/") + programName();
}

QStringList MuffinInstallHelper::programCandidates(const QString &applicationDirPath)
{
    QStringList out;
    out << installedProgram();

    const QString base = applicationDirPath.isEmpty()
                             ? QCoreApplication::applicationDirPath()
                             : applicationDirPath;
    if (!base.isEmpty())
    {
        // A build tree puts every executable in the same directory, so a download started straight from
        // `build/` finds the helper that was just compiled rather than one from an older install.
        out << base + QLatin1Char('/') + programName();
        // And a `cmake --install` prefix keeps bin/ and lib/muffin/ two levels apart.
        out << base + QStringLiteral("/../lib/muffin/") + programName();
    }
    return out;
}

QString MuffinInstallHelper::findProgram(const QString &applicationDirPath)
{
    for (const QString &c : programCandidates(applicationDirPath))
    {
        const QFileInfo fi(c);
        if (!fi.isFile() || !fi.isExecutable())
            continue;
        // Not group- or world-writable. This is the last point at which the program that is about to be
        // run as root can still be vetoed, and a binary any other account could rewrite is not a
        // trusted one no matter whose password unlocks it. (The polkit rule never matches this path, so
        // a human authorisation is always required as well — see packaging/postinst.)
        if (fi.permissions() & (QFileDevice::WriteGroup | QFileDevice::WriteOther))
            continue;
        const QString canonical = fi.canonicalFilePath();
        return canonical.isEmpty() ? c : canonical;
    }
    return QString();
}

QString MuffinInstallHelper::namespaceDir()
{
    const QString dest = ZapretPaths::systemInstallDir();
    return dest.isEmpty() ? QString() : parentOf(dest);
}

QString MuffinInstallHelper::stagingDir()
{
    const QString dest = ZapretPaths::systemInstallDir();
    return dest.isEmpty() ? QString() : dest + QStringLiteral(".new");
}

QString MuffinInstallHelper::backupDir()
{
    const QString dest = ZapretPaths::systemInstallDir();
    return dest.isEmpty() ? QString() : dest + QStringLiteral(".old");
}
bool MuffinInstallHelper::looksLikeLinuxPayload(const QString &dir)
{
    if (dir.isEmpty())
        return false;

    // nfqwsPath() returns the first candidate that EXISTS, preferring the host architecture, so this is
    // the packaged binary the portable card will exec. nfqwsBuiltPath() is the other shape: what
    // install_bin.sh or a hand-built tree leaves at nfq/nfqws for the init script to exec. Neither is
    // required — one of them is enough, and neither being there means the directory is a source tree
    // that cannot start anything and must not be dressed up as a root-owned, passwordless payload.
    if (!ZapretPaths::nfqwsPath(dir).isEmpty())
        return true;
    return QFileInfo(ZapretPaths::nfqwsBuiltPath(dir)).isFile();
}

bool MuffinInstallHelper::hasEscapingSymlink(const QString &dir, QString *offender)
{
    if (dir.isEmpty())
        return false;

    // The tree root as an existing, link-free path: a link anywhere below is compared against it, so
    // resolving a link's target relative to the link's own directory is the same computation the kernel
    // will do when something tries to exec that path.
    const QString root = QFileInfo(dir).canonicalFilePath();
    if (root.isEmpty())
        return false;   // validateSource has already refused a directory that cannot be opened

    QDirIterator it(root, QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    // QDirIterator does not follow symbolic links when it descends (FollowSymlinks is off), so a link
    // loop cannot spin this walk forever, and every link — to a file or to a directory — surfaces here
    // as its own entry.
    while (it.hasNext())
    {
        it.next();
        const QFileInfo fi = it.fileInfo();
        if (!fi.isSymLink())
            continue;

        // Read the link the way the kernel will resolve it: take the string actually stored in it, and
        // resolve it against the link's own directory. QFile::symLinkTarget(path) is the static,
        // readlink-based accessor that gives that raw string; QFileInfo::symLinkTarget() is not used
        // because it canonicalises and answers empty for a dangling link, and "we cannot tell where
        // this ends up" is a state to refuse, not one to hand a passwordless tree.
        const QString raw = QFile::symLinkTarget(fi.absoluteFilePath());
        if (raw.isEmpty())
        {
            if (offender)
                *offender = fi.absoluteFilePath();
            return true;
        }
        const QString resolved =
            QDir::isAbsolutePath(raw)
                ? QDir::cleanPath(raw)
                : QDir::cleanPath(fi.dir().absolutePath() + QLatin1Char('/') + raw);
        // Inside the tree, including the tree root itself: the target is copied alongside the link, and
        // after chown -R root:root both belong to root.
        if (isInside(resolved, root))
            continue;
        if (offender)
            *offender = fi.absoluteFilePath();
        return true;
    }
    return false;
}

bool MuffinInstallHelper::validateSource(const QString &sourceDir, QString *error)
{
    auto refuse = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    const QString label = QStringLiteral("MuffinInstallHelper: ");

    if (sourceDir.isEmpty())
        return refuse(label + QStringLiteral("no directory was given to promote"));
    if (!QDir::isAbsolutePath(sourceDir))
        return refuse(label + QStringLiteral("%1 is not an absolute path").arg(sourceDir));

    // A ".." component is refused outright rather than cleaned away. QDir::cleanPath() would resolve it
    // and produce a path that is fine to open, but this string is also what the polkit rule, the log
    // line and the user's approval all describe, and a promotion whose source has two spellings is a
    // promotion nobody can audit afterwards.
    const QStringList parts = sourceDir.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return refuse(label + QStringLiteral("%1 is the root directory").arg(sourceDir));
    if (parts.contains(QLatin1String("..")))
        return refuse(label + QStringLiteral("%1 walks out of its own directory").arg(sourceDir));

    const QFileInfo fi(sourceDir);
    if (fi.isSymLink())
        return refuse(label + QStringLiteral("%1 is a link, not a directory").arg(sourceDir));
    if (!fi.isDir())
        return refuse(label + QStringLiteral("%1 does not exist or is not a directory").arg(sourceDir));

    if (!looksLikeLinuxPayload(sourceDir))
        return refuse(label + QStringLiteral("%1 holds no linux zapret payload (binaries/<arch>/nfqws "
                                             "or nfq/nfqws), so there is nothing to promote")
                            .arg(sourceDir));

    // A link out of the tree survives `cp -a` as a link, and the copy's mode and ownership are then
    // protecting a path that resolves to bytes outside it — which the polkit rule would exec without a
    // password. Refusing the whole tree is the only sound answer, and it costs a real release nothing:
    // none of the zapret archives ship links of this kind.
    QString offender;
    if (hasEscapingSymlink(sourceDir, &offender))
        return refuse(label + QStringLiteral("%1 contains the symbolic link %2, which points outside "
                                             "the tree; a promoted payload must not resolve through a "
                                             "link the promotion does not own")
                            .arg(sourceDir, offender));

    // Never copy the destination, or a sibling of it, into itself: `cp -a` of a directory into a
    // descendant of itself grows until the disk fills, and the staging directory is exactly one such
    // sibling. Refusing the whole namespace is the cheap version of that guard and costs nothing, since
    // a promotion always reads from the user's own staging area outside it.
    const QString dest = ZapretPaths::systemInstallDir();
    const QStringList offLimits = { dest, stagingDir(), backupDir(), namespaceDir() };
    for (const QString &prefix : offLimits)
    {
        if (!prefix.isEmpty() && isInside(sourceDir, prefix))
            return refuse(label + QStringLiteral("%1 is inside the system directory %2, which this "
                                                 "promotion writes to")
                                .arg(sourceDir, prefix));
    }

    return true;
}
QList<MuffinInstallHelper::Step> MuffinInstallHelper::promotionSteps(const QString &sourceDir,
                                                                    const QString &staging)
{
    const QString ns = namespaceDir();
    QList<Step> steps;

    // Take the namespace directory in hand first. It is usually created moments earlier by this same
    // helper running as root, but "usually" is not the property the polkit rule is buying: if anything
    // ever left /opt/muffin group- or world-writable, an unprivileged user could rename the whole tree
    // beside it and put his own in — permissions on the payload would then be protecting a directory he
    // controls. These two steps make that state impossible rather than unlikely.
    steps.append({ QStringLiteral("chown root:root %1").arg(ns),
                   systemTool(QStringLiteral("chown")),
                   { QStringLiteral("root:root"), QStringLiteral("--"), ns } });
    steps.append({ QStringLiteral("chmod 755 %1").arg(ns),
                   systemTool(QStringLiteral("chmod")),
                   { QStringLiteral("755"), QStringLiteral("--"), ns } });

    // cp -a == -dR --preserve=all: recurse, keep symlinks as symlinks instead of copying what they
    // point at, and keep the mode bits and timestamps. The "/." suffix copies the CONTENTS of the
    // source directory into the staging directory, which is what makes the tree land at
    // <system>/binaries/... rather than at <system>/zapret/binaries/... — the difference between a
    // located payload and a directory the locator cannot read.
    steps.append({ QStringLiteral("cp -a %1").arg(sourceDir),
                   systemTool(QStringLiteral("cp")),
                   { QStringLiteral("-a"), QStringLiteral("--"),
                     sourceDir + QStringLiteral("/."), staging } });

    // The ownership step is not decoration and is the reason this helper exists: `cp -a` preserves the
    // source's uid and gid when it runs as root, so without this the passwordless tree would still be
    // owned by the unprivileged user who downloaded it, and he could rewrite the binary polkit has been
    // told is safe to exec. Order matters — it has to follow the copy and precede the chmods, because
    // chmod on a file the user still owns is a permission he can undo later.
    steps.append({ QStringLiteral("chown -R root:root %1").arg(staging),
                   systemTool(QStringLiteral("chown")),
                   { QStringLiteral("-R"), QStringLiteral("root:root"), QStringLiteral("--"), staging } });

    // Readable by the account that has to scan it: the GUI runs as the user, and StrategyParser,
    // ZapretPaths::isLinuxInstalled() and the locator all read this tree. a+rX adds read for everyone
    // and the execute bit only where some execute bit is already set (or on directories), so it cannot
    // turn a data file into a program.
    steps.append({ QStringLiteral("chmod -R a+rX %1").arg(staging),
                   systemTool(QStringLiteral("chmod")),
                   { QStringLiteral("-R"), QStringLiteral("a+rX"), QStringLiteral("--"), staging } });

    // And never writable by anyone but root — the exact property the polkit rule's path prefix is
    // asserting. Without it "/opt/muffin/zapret/**/nfqws is passwordless" would mean "whatever the last
    // person to write that file wants root to run".
    steps.append({ QStringLiteral("chmod -R go-w %1").arg(staging),
                   systemTool(QStringLiteral("chmod")),
                   { QStringLiteral("-R"), QStringLiteral("go-w"), QStringLiteral("--"), staging } });

    return steps;
}

bool MuffinInstallHelper::composePromote(const QString &sourceDir, QString *program,
                                         QStringList *args, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!program || !args)
        return fail(QStringLiteral("MuffinInstallHelper: composePromote() needs somewhere to write"));
    program->clear();
    args->clear();

    QString why;
    if (!validateSource(sourceDir, &why))
        return fail(why);

    const QString helper = findProgram();
    if (helper.isEmpty())
        return fail(QStringLiteral(
                        "MuffinInstallHelper: the root helper is not installed (%1), so the download "
                        "stays in the managed directory and keeps asking for a password when it starts.")
                        .arg(installedProgram()));

    // Already root: run it directly. Same guard LinuxBackend::composeCommand() applies, for the same
    // reason — pkexec from a root session is a prompt nobody can answer, not an extra permission.
    if (PrivilegeHelper::isElevated())
    {
        *program = helper;
        *args = { sourceDir };
        return true;
    }

    if (!QFileInfo::exists(QStringLiteral("/usr/bin/pkexec")))
        return fail(QStringLiteral(
                        "MuffinInstallHelper: pkexec is not installed, so the payload cannot be "
                        "promoted into %1.").arg(ZapretPaths::systemInstallDir()));

    *program = QStringLiteral("pkexec");
    *args = { helper, sourceDir };
    return true;
}

} // namespace zapret
