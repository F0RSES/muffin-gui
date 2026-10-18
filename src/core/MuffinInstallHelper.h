#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace zapret {

// The root helper that moves a downloaded zapret tree into ZapretPaths::systemInstallDir().
//
// What it is: a small stand-alone program (src/tools/muffin-install-helper.cpp) that the .deb installs
// under /usr/lib/muffin/, that refuses to do anything unless it is already root, and that performs the
// one step an unprivileged process cannot — write into /opt/muffin and hand the result to root:root.
// The GUI reaches it the way it reaches everything privileged: through pkexec, so polkit decides and a
// human types a password.
//
// Why a compiled helper and not a shell script: `pkexec /bin/sh install_bin.sh` puts a script
// interpreter on the authorised path — the one thing polkit's own documentation warns about, because
// the interpreter execs whatever the script says and the script sits in a directory the caller owns. A
// binary with a fixed destination, one argument, and its own validation has no such degrees of freedom.
// It also means the steps are functions with names, so tests can assert them: coretest reads
// promotionSteps() and packaging/check-polkit-rule.js reads the paths back out of this source, instead
// of anyone having to trust a heredoc or a comment that claims what the code does.
//
// Everything both sides must agree on lives here: the caller composes the command with composePromote(),
// the helper runs the commands promotionSteps() returns, and neither keeps a private copy of a path.
class MuffinInstallHelper
{
public:
    MuffinInstallHelper() = delete;

    // ---- where the helper lives ----
    static QString programName();        // "muffin-install-helper"
    // The path the package installs it to. The GUI execs exactly this through pkexec, and the polkit
    // rule deliberately does NOT authorise it: promoting user-written bytes into a passwordless tree is
    // the moment a human approves, so this call costs a password like `apt install` did.
    static QString installedProgram();   // "/usr/lib/muffin/muffin-install-helper"

    // Where the caller looks for it, in order: the installed path, then the two spellings a build tree
    // (both executables in build/) and a `cmake --install` prefix (bin/ beside lib/muffin/) produce, so
    // someone testing a download straight out of the build directory walks the same code path as an apt
    // user. `applicationDirPath` is a parameter rather than QCoreApplication::applicationDirPath() so a
    // test can ask about a tree it is not running from.
    static QStringList programCandidates(const QString &applicationDirPath = QString());
    // The first candidate that is a real, executable file that no other account could rewrite (see the
    // definition). Empty when no helper is available — a normal state for a checkout that never ran
    // `cmake --install`, and it means "stay with the managed copy", not "fail the download".
    // `applicationDirPath` is a parameter for the same reason programCandidates() has one: the veto on
    // a group- or world-writable binary is a security property, and a test has to be able to present
    // one to be refused rather than only trusting that it would be.
    static QString findProgram(const QString &applicationDirPath = QString());

    // ---- the three directories a promotion touches, all of them fixed ----
    // The namespace that holds them ("/opt/muffin"). It has to be root-owned and not group/world
    // writable too: a directory the user could write into would let him rename the tree inside it out of
    // place, which is the same exploit the payload's own permissions exist to prevent.
    static QString namespaceDir();
    static QString stagingDir();         // systemInstallDir() + ".new" — copied into, then renamed
    static QString backupDir();          // systemInstallDir() + ".old" — the incumbent, moved aside

    // ---- the checks made before anything is written as root ----
    // Is `dir` a linux zapret payload worth promoting? The same question
    // ZapretDownloader::installLooksComplete() asks, phrased without the downloader so a root process
    // can ask it too: a tree that cannot start anything must not become a root-owned, passwordless
    // directory just because someone pointed the helper at it.
    static bool looksLikeLinuxPayload(const QString &dir);

    // Does `dir` hold a symbolic link that leaves `dir`? Returns false when nothing escapes, and true
    // with *offender set to the link when one does — a link whose stored target resolves outside the
    // tree, an absolute link to anything outside it, a relative link that walks out with "..", or a
    // link whose target cannot be read at all.
    //
    // Why this is checked at all: the promotion copies with `cp -a`, whose preserving semantics mean a
    // symlink stays a symlink rather than becoming the bytes it points at. The polkit rule exempts a
    // payload under the promoted tree WITHOUT a password, and pkexec execs the path it is given — which
    // the kernel resolves through any link found there. A tree whose binaries/linux-x86_64/nfqws is a
    // link to a script its owner can still edit would therefore be a passwordless root shell reached by
    // a path nobody could rewrite. Links that stay inside the tree are harmless (the target is copied
    // alongside them, and the whole tree ends up root-owned); links that leave it are not.
    static bool hasEscapingSymlink(const QString &dir, QString *offender = nullptr);

    // Rejects a relative path, a path with a ".." component, a symlink, anything that is not a
    // directory, anything that is not a payload, anything holding a link out of itself, and anything
    // inside the system tree the helper is about to overwrite. Fills *error with which of those it was.
    static bool validateSource(const QString &sourceDir, QString *error);

    // ---- the root-side steps, as data ----
    // One external command: an absolute program and its argv. No shell is involved anywhere, so a path
    // with a space or a glob character in it stays one argument rather than becoming a command.
    struct Step
    {
        QString label;                   // what to call it in a message ("cp -a")
        QString program;                 // absolute, resolved without PATH; "" when not installed
        QStringList arguments;
    };
    // The commands in the order they must run: take the namespace directory in hand, copy with `cp -a`,
    // then `chown -R root:root`, then make the tree readable but not writable (`chmod -R a+rX`,
    // `chmod -R go-w`). Those last three are the entire point of the promotion, so they are asserted by
    // name in coretest rather than left to a comment: `cp -a` preserves the source's ownership when it
    // runs as root, which is why the chown is load-bearing and not decoration, and a group- or
    // world-writable payload under a passwordless prefix would be a local root.
    static QList<Step> promotionSteps(const QString &sourceDir, const QString &staging);

    // ---- the caller's side ----
    // The program and argv that ASK for the promotion: `pkexec <helper> <sourceDir>`, or the helper
    // directly when this process is already root (a build running under sudo, or a root session). False
    // with an explanation when the helper is missing, the source fails validation, or there is no
    // pkexec to ask.
    static bool composePromote(const QString &sourceDir, QString *program, QStringList *args,
                               QString *error);
};

} // namespace zapret
