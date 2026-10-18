// muffin-install-helper — the root half of installing a downloaded zapret tree.
//
// This is the program src/core/MuffinInstallHelper.h describes: a stand-alone executable the .deb
// installs under /usr/lib/muffin/, reached through pkexec, that performs the one step an
// unprivileged process cannot — write into /opt/muffin and hand the result to root:root.
//
// It is deliberately tiny and it has exactly one job. Everything both sides must agree on (the
// destination, the staging names, the source checks, the root-side commands) lives in
// MuffinInstallHelper, so this file is only the sequence: validate, build the staging tree, swap it
// into place, clean up. The GUI composes the command with composePromote(), tests assert
// promotionSteps(), and packaging/check-polkit-rule.js reads the paths back out of that source —
// which is what makes the polkit rule's prefix a checked fact instead of a claim.
//
// Exit codes: 0 installed, 1 the promotion was refused or failed, 2 the invocation itself was
// unusable (not root, wrong arguments, this build has no system tree).

#include "MuffinInstallHelper.h"
#include "PrivilegeHelper.h"
#include "ZapretPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTextStream>

// The helper shares every path and every step with the library it links, so it works in the same
// namespace the rest of the code does rather than spelling zapret:: on each of the twenty-odd uses.
using namespace zapret;

namespace {

void note(const QString &line)
{
    QTextStream o(stdout);
    o << line << '\n';
}

void warn(const QString &line)
{
    QTextStream o(stderr);
    o << line << '\n';
}

// How long one root-side command may take. `cp -a` of a several-megabyte tree is a fraction of a
// second on any real disk; the ceiling exists so a filesystem that has stopped answering produces a
// failure and a clean staging directory rather than a process sitting in pkexec's foreground forever
// with a modal authorisation already spent.
constexpr int kStepStartMs = 10000;
constexpr int kStepRunMs = 120000;

// The environment the coreutils commands run with: one variable we chose and nothing inherited.
// Reached through pkexec the environment is already scrubbed (pkexec(1) unsets the loader hooks and
// resets IFS before exec'ing the target), but a root session and a `cmake --install` prefix take the
// direct branch in composePromote() and carry whatever the caller had. A root `cp` honouring an
// inherited LD_PRELOAD is a way for the calling user to choose what root executes — the precise thing
// this whole class exists to prevent — and since the steps name absolute programs, PATH is here only
// so a message like "no such file" stays understandable.
QProcessEnvironment stepEnvironment()
{
    QProcessEnvironment env;
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    return env;
}
bool runStep(const MuffinInstallHelper::Step &step)
{
    if (step.program.isEmpty())
    {
        warn(QStringLiteral("%1: cannot run '%2' because that program is not installed under "
                            "/usr/bin or /bin")
                 .arg(MuffinInstallHelper::programName(), step.label));
        return false;
    }

    QProcess proc;
    proc.setProgram(step.program);
    proc.setArguments(step.arguments);
    proc.setProcessEnvironment(stepEnvironment());
    proc.setWorkingDirectory(QStringLiteral("/"));
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();

    if (!proc.waitForStarted(kStepStartMs))
    {
        warn(QStringLiteral("%1: '%2' could not be started (%3)")
                 .arg(MuffinInstallHelper::programName(), step.label, proc.errorString()));
        proc.kill();
        proc.waitForFinished(1000);
        return false;
    }
    if (!proc.waitForFinished(kStepRunMs))
    {
        warn(QStringLiteral("%1: '%2' did not finish within %3 s; the staging directory is removed "
                            "and nothing was installed over the current tree")
                 .arg(MuffinInstallHelper::programName(), step.label,
                      QString::number(kStepRunMs / 1000)));
        proc.kill();
        proc.waitForFinished(5000);
        return false;
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
    {
        const QString output = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
        warn(QStringLiteral("%1: '%2' failed (exit %3)%4")
                 .arg(MuffinInstallHelper::programName(), step.label, QString::number(proc.exitCode()),
                      output.isEmpty() ? QString() : QStringLiteral("\n%1").arg(output.left(500))));
        return false;
    }
    return true;
}

// Is `path` safe to delete on this run's behalf? Only a real directory that is not a symlink: the
// `.new` and `.old` names are siblings of the tree being installed, and a leftover from a killed run
// is normally ours to sweep away — but if some other account put a link there, following it as root
// would delete somebody else's files.
bool removableDir(const QString &path)
{
    const QFileInfo fi(path);
    return fi.exists() && fi.isDir() && !fi.isSymLink();
}

bool clearDir(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return true;
    if (!removableDir(path))
    {
        warn(QStringLiteral("%1: refusing to remove %2: it is not an ordinary directory")
                 .arg(MuffinInstallHelper::programName(), path));
        return false;
    }
    if (!QDir(path).removeRecursively())
    {
        warn(QStringLiteral("%1: could not remove %2").arg(MuffinInstallHelper::programName(), path));
        return false;
    }
    return true;
}

// The version CMake stamps in with MUFFIN_VERSION. QCoreApplication::applicationVersion() is where
// main() puts it, and reading it back through Qt keeps --help and --version reporting the same string
// the .deb's Version field does. A build configured without the define gets "unknown" rather than an
// invented number.
QString versionText()
{
    const QString v = QCoreApplication::applicationVersion();
    return v.isEmpty() ? QStringLiteral("unknown") : v;
}

void usage()
{
    note(QStringLiteral("Usage: %1 <directory>\n"
                        "Install a downloaded linux zapret tree into %2 as root, taking the copy in\n"
                        "through a staging directory so a running tree is never left half-written.\n"
                        "The argument must already be an unpacked linux payload. No environment\n"
                        "variable names the destination, and the result is owned by root:root and\n"
                        "not writable by any other account.\n"
                        "Version: %3")
             .arg(MuffinInstallHelper::programName(), ZapretPaths::systemInstallDir(),
                  versionText()));
}

} // namespace


int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(MuffinInstallHelper::programName());
    QCoreApplication::setOrganizationName(QStringLiteral("F0RSES"));
#ifdef MUFFIN_VERSION
    QCoreApplication::setApplicationVersion(QStringLiteral(MUFFIN_VERSION));
#endif

    const QStringList args = app.arguments();   // [0] is this program
    if (args.size() == 2 &&
        (args.at(1) == QLatin1String("-h") || args.at(1) == QLatin1String("--help")))
    {
        usage();
        return 0;
    }
    // Answered before the privilege gate and before any path is looked at: knowing which build of a
    // root-owned binary is installed is a question an ordinary user has to be able to ask.
    if (args.size() == 2 && args.at(1) == QLatin1String("--version"))
    {
        note(QStringLiteral("%1 %2").arg(MuffinInstallHelper::programName(), versionText()));
        return 0;
    }
    if (args.size() != 2)
    {
        warn(QStringLiteral("%1: exactly one argument is required, the directory to install")
                 .arg(MuffinInstallHelper::programName()));
        usage();
        return 2;
    }

    // The privilege gate is checked before any path is even looked at. Everything below assumes root:
    // creating /opt/muffin, chown'ing to root, renaming over a running tree. Reaching this binary
    // without root means somebody ran it by hand, and the honest answer is "this is what pkexec is
    // for", not a partial install that quietly did nothing.
    if (!PrivilegeHelper::isElevated())
    {
        warn(QStringLiteral("%1: must run as root (it is started through pkexec by the Muffin "
                            "application, which asks for a password first)")
                 .arg(MuffinInstallHelper::programName()));
        return 2;
    }

    const QString dest = ZapretPaths::systemInstallDir();
    const QString ns = MuffinInstallHelper::namespaceDir();
    const QString staging = MuffinInstallHelper::stagingDir();
    const QString backup = MuffinInstallHelper::backupDir();
    if (dest.isEmpty() || ns.isEmpty() || staging.isEmpty() || backup.isEmpty())
    {
        warn(QStringLiteral("%1: this build has no system install directory, so there is nothing to "
                            "promote into")
                 .arg(MuffinInstallHelper::programName()));
        return 2;
    }

    const QString source = args.at(1);
    QString why;
    if (!MuffinInstallHelper::validateSource(source, &why))
    {
        warn(why);
        return 1;
    }

    if (!clearDir(staging) || !clearDir(backup))
        return 1;   // leftovers we cannot account for are a reason to stop, not to plough through

    // The namespace directory is created here rather than left to the steps: `cp -a` into a staging
    // path whose parent does not exist fails, and promotionSteps() owns the ownership and the mode of
    // the tree, not its existence.
    if (!QDir().mkpath(ns) || !QDir().mkpath(staging))
    {
        warn(QStringLiteral("%1: cannot create %2").arg(MuffinInstallHelper::programName(), staging));
        return 1;
    }

    // Build the whole tree in the staging directory first. Every command here is data from
    // promotionSteps(): an absolute program and its argv, no shell, so a path with a space in it stays
    // one argument. If any step fails the destination is still the previous tree.
    const QList<MuffinInstallHelper::Step> steps =
        MuffinInstallHelper::promotionSteps(source, staging);
    for (const MuffinInstallHelper::Step &step : steps)
    {
        if (!runStep(step))
        {
            clearDir(staging);
            return 1;
        }
    }

    // Verify what was actually staged, not what was promised. validateSource checked the source; this
    // checks the copy that is about to be renamed into the passwordless tree, so a change of copy
    // semantics, a filesystem that materialises links differently, or a source that changed while it
    // was being read all end up refused here rather than exec'd later.
    QString offender;
    if (MuffinInstallHelper::hasEscapingSymlink(staging, &offender))
    {
        warn(QStringLiteral("%1: the staged tree contains the symbolic link %2, which leaves the tree; "
                            "the staged copy was removed and nothing was installed")
                 .arg(MuffinInstallHelper::programName(), offender));
        clearDir(staging);
        return 1;
    }

    // The staging tree is complete, already root:root with no group/world write bit. From here on
    // there are exactly two operations that change what is installed, and both are renames inside one
    // directory — atomic, and never crossing a filesystem boundary.
    const QFileInfo destInfo(dest);
    if (destInfo.exists() && !(destInfo.isDir() && !destInfo.isSymLink()))
    {
        warn(QStringLiteral("%1: %2 exists and is not an ordinary directory; nothing was changed")
                 .arg(MuffinInstallHelper::programName(), dest));
        clearDir(staging);
        return 1;
    }

    const bool replacing = destInfo.exists();
    if (replacing && !QFile::rename(dest, backup))
    {
        warn(QStringLiteral("%1: the current tree could not be moved aside; nothing was changed (%2 → "
                            "%3)")
                 .arg(MuffinInstallHelper::programName(), dest, backup));
        clearDir(staging);
        return 1;
    }

    if (!QFile::rename(staging, dest))
    {
        const QString reason =
            QStringLiteral("%1: the new tree could not be put in place at %2")
                .arg(MuffinInstallHelper::programName(), dest);
        if (replacing && QFile::rename(backup, dest))
        {
            warn(reason + QStringLiteral("; the previous installation was restored."));
        }
        else
        {
            // The one state that has to be said out loud rather than papered over: the incumbent sits
            // under .old and the destination is empty. Whatever stopped the second rename (disk full, a
            // mount appearing between the two) leaves the tree recoverable by hand, and the message has
            // to say where it is.
            warn(reason + QStringLiteral("; the previous installation is still under %1 and can be "
                                          "moved back by root.")
                             .arg(backup));
        }
        clearDir(staging);
        return 1;
    }

    // The replacement is in place; the incumbent has spent its purpose. A failure here is reported and
    // then ignored: leaving .old beside a good installation is untidy, not wrong, and being able to
    // leave it is exactly why the swap happens before this line rather than after.
    if (replacing && !clearDir(backup))
        warn(QStringLiteral("%1: installed, but the previous tree is still under %2")
                 .arg(MuffinInstallHelper::programName(), backup));

    note(QStringLiteral("%1: installed %2 into %3 as root:root, not writable by any other account")
             .arg(MuffinInstallHelper::programName(), source, dest));
    return 0;
}

