#pragma once

#include "Strategy.h"

#include <QString>

namespace zapret {

// Central map of the well-known file/dir locations inside a zapret distribution tree.
//
// Keeping the layout in one place means StrategyParser, ZapretLocator and the (future) backend
// all agree on where things live, and the magic strings only appear once. Every function takes a
// `rootPath` (the windows/ or linux/ directory) and returns an absolute path derived from it.
//
// The "exists" helpers use QFileInfo, so they reflect on-disk truth rather than assumptions; the
// plain *Path helpers just join strings and do NOT stat the file.
class ZapretPaths
{
public:
    // ---- Windows (root = the .../windows directory) ----
    static QString winwsPath(const QString &root);        // bin/winws.exe
    static QString listsDir(const QString &root);         // lists/
    static QString binDir(const QString &root);           // bin/
    static QString serviceBatPath(const QString &root);   // service.bat

    // ---- Linux (root = the .../linux directory) ----
    // nfqws lives in binaries/<arch>/ for the packaged tree, or at the root after install_bin.sh.
    // The machine-native binary is preferred; see nfqwsPath().
    static QString nfqwsPath(const QString &root);            // best available nfqws, "" if none
    static QString nfqwsArchPath(const QString &root,
                                 const QString &archDirName);  // binaries/<archDirName>/nfqws
    static QString sysvScriptPath(const QString &root);      // init.d/sysv/zapret
    static QString openrcScriptPath(const QString &root);    // init.d/openrc/zapret
    static QString systemdUnitPath(const QString &root);     // init.d/systemd/zapret.service
    static QString configDefaultPath(const QString &root);   // config.default
    // The live config the init scripts source. install_easy.sh creates it (cp config.default config)
    // and it is the file named by init.d/sysv/functions: ZAPRET_CONFIG="${ZAPRET_CONFIG:-$ZAPRET_RW/config}".
    static QString configPath(const QString &root);          // config  (the sourced one, not the template)
    static QString nfqwsBuiltPath(const QString &root);       // nfq/nfqws  (what the init scripts exec)
    // True when the linux tree has the config file its init scripts source. An unpacked release ships
    // only config.default, so `. "$ZAPRET_CONFIG"` fails at the very top of init.d/sysv/functions and
    // the script cannot bring anything up; install_easy.sh is what creates the file.
    static bool isLinuxConfigured(const QString &root);
    // True when the linux tree is INSTALLED, i.e. its sysv/openrc init script can actually bring the
    // daemon up. See the definition for the exact on-disk condition.
    //
    // Platform note: "can be exec'd" is checked with the POSIX exec bit, and on Windows only the
    // presence of nfq/nfqws. Qt cannot report an extension-less ELF file as executable there, so
    // asking it would make every installed tree look unpacked forever.
    static bool isLinuxInstalled(const QString &root);
    static QString binariesDir(const QString &root);         // binaries/

    // The arch directory name that matches the host, e.g. "linux-x86_64". Empty if we can't map
    // QSysInfo::currentCpuArchitecture() to a binaries/ subdir.
    static QString nativeArchDirName();

    // ---- the tree this application installs for the user ----
    // "<AppDataLocation>/zapret": the one directory whose contents Muffin itself lays out, because
    // ZapretDownloader unpacks a GitHub release there and validates it before announcing success.
    // QStandardPaths already resolves the per-OS base (~/.local/share/<org>/<app> on Linux,
    // %APPDATA%\<org>\<app> on Windows, ~/Library/Application Support/<app> on macOS), so this needs
    // no #ifdef — the "zapret" folder is the only thing added here.
    //
    // Empty when the host has no writable app-data location at all (applicationName unset and the
    // OS gives us nothing): callers must treat that as "no managed install", not as "the root dir".
    static QString managedInstallDir();

    // The per-user directory this identity's data lives in — exactly the base managedInstallDir() hangs
    // "zapret" off, without that folder. Anything the application records about itself (the check
    // history) belongs here rather than inside the payload tree, because that tree is a download: a
    // re-download replaces it, and the root helper promotes a copy of it over a root-owned one.
    // Empty for the same reason managedInstallDir() is.
    static QString appDataDir();
    static QString appDataDirFor(const QString &organizationName);

    // "<AppDataLocation>/history.json": the past bypass checks, as HistoryStore reads and writes them.
    // Empty when there is no app-data directory to put it in; a HistoryStore with an empty path keeps
    // its rows in memory and says so through lastError().
    static QString historyFilePath();

    // managedInstallDir() as it resolves under an explicitly chosen organization, ignoring what the
    // application identity currently says. The one-off identity migration below needs to ask the same
    // question about a name the process no longer uses, and QStandardPaths has no other way to answer
    // it: it reads the process-wide organization, so this adopts it for the duration of the call.
    static QString managedInstallDirFor(const QString &organizationName);

    // The organization this application was built with up to and including 0.1.0 — the very same word
    // as the application name, which is what made the data folder read "Muffin/Muffin" twice. Named
    // here rather than in the callers so the folder move and the settings import agree on it instead
    // of each keeping their own copy of a historical fact.
    static QString legacyOrganizationName();

    // The managed install as 0.1.0 laid it out: "<legacy identity>/zapret". May be empty for the same
    // reason managedInstallDir() can be.
    static QString legacyManagedInstallDir();

    // Move one whole directory onto another name. True only when `from` is gone because it became
    // `to`. It refuses — without touching anything — when the target already exists, when there is
    // nothing to move, and when both names describe the same directory, so a refused move always
    // leaves a downloaded tree exactly where the previous build can still find it.
    static bool moveManagedInstallDir(const QString &from, const QString &to);

    // Carry the 0.1.0 tree into the folder this build's identity resolves to. True when a move
    // actually happened. Called once from the entry point, before anything reads a path.
    //
    // It then sweeps up the folders that only ever held that tree ("<data>/Muffin/Muffin" and
    // "<data>/Muffin"), because a renamed-away tree would otherwise leave the doubled name sitting in
    // the file manager as an empty directory — which is the thing the migration set out to remove. Only
    // directories that are genuinely empty are deleted, so a legacy folder holding anything else (or
    // one this build resolves to) survives untouched.
    static bool migrateManagedInstallDir();

    // ---- the system-wide tree a download is promoted into ----
    // "/opt/muffin/zapret" on POSIX, "<Program Files>\Muffin\zapret" on Windows. Empty only when the
    // host offers no such place at all; callers must not read an empty answer as "the root directory".
    //
    // Why a directory of its own instead of /opt/zapret (and on Windows instead of a folder loose in
    // C:\): upstream's own scripts default to that POSIX path
    // (init.d/sysv/defaults.sh: ZAPRET_BASE=${ZAPRET_BASE:-/opt/zapret}), so a user who set zapret up
    // by hand already has a tree there that Muffin must never overwrite. Muffin's namespace is a place
    // nothing else claims, and the packaging scripts may clean it up.
    //
    // Why it has to be a directory the user cannot write to: that IS the point of promoting. The
    // download is unpacked into managedInstallDir(), which the user owns, and the finished tree is
    // then moved here, where only root (POSIX) or only an elevated process (Windows) may write.
    //
    //   POSIX: packaging/postinst installs a polkit rule that lets a payload under this tree be exec'd
    //   through pkexec WITHOUT a password. That is only sound while an unprivileged user cannot replace
    //   the bytes there. A directory in the user's own home fails that test - he owns it, so whoever
    //   wrote the file owns the root shell that follows - which is why the promotion is done by the
    //   root helper (MuffinInstallHelper) rather than by exec'ing the staged copy in place.
    //
    //   Windows: Muffin.exe carries requireAdministrator in its manifest and WindowsBackend starts
    //   winws.exe from that elevated token. A tree left in %LOCALAPPDATA% is therefore a local
    //   privilege escalation: any program running as the user can rewrite winws.exe there, and the
    //   next start runs the replacement as administrator. %ProgramFiles% is the one ordinary location
    //   a standard token cannot write to, so that is where the payload belongs.
    //
    // The POSIX branch is a compile-time literal because the root helper reads it while running as
    // root, and a redirectable destination would let an unprivileged user aim that root write at any
    // directory on the machine. The Windows branch is read from the environment because Muffin reads
    // it while it is ALREADY elevated: the value comes from the elevated process's own environment
    // block, which a non-admin cannot influence, and hardcoding C:\ would break the machine where
    // Windows is installed elsewhere. systemInstallDirUnder() refuses anything that is not an
    // absolute, traversal-free path, so even a hostile value cannot steer the write outside
    // Program Files.
    //
    // packaging/check-polkit-rule.js reads the POSIX literal back out of the source and requires the
    // polkit rule to carry the same prefix, so the two cannot drift apart silently.
    static QString systemInstallDir();

    // The Windows destination composed from the Program Files root `programFilesRoot`, under the rules
    // systemInstallDir() applies to the value it reads from the environment: empty, not absolute, or
    // carrying a ".." component lands on the plain "C:/Program Files" default instead. Split out
    // because "where would a promotion write?" is the security answer, and an answer a test cannot
    // hand its inputs to is an answer nobody has checked.
    static QString systemInstallDirUnder(const QString &programFilesRoot);

    // ---- the list files a payload is told to read ----
    //
    // Make one empty list file: the "-user" placeholders (list-general-user.txt,
    // ipset-exclude-user.txt, ...) that zapret's windows wrappers point winws at but a release
    // archive does not ship, because upstream creates them with zz-make-user-files.bat. A launcher
    // that execs the payload directly has to make them itself or the child stops with
    // "cannot access ipset file" -- a failure that reads as a path problem and is a missing file.
    //
    // The file is created empty and, when its directory is absent, that directory is made first:
    // upstream's own batch does the same for the lists folder. An existing file is left exactly as
    // it is and reported as success, because the placeholder may already hold the user's entries.
    // Fills *error only when nothing could be created.
    static bool createEmptyListFile(const QString &path, QString *error = nullptr);

    // ---- Layout detection (no assumptions about which root you pass) ----
    // True if `root` looks like a usable windows distribution (bin/winws.exe + *.bat).
    static bool isWindowsRoot(const QString &root);
    // True if `root` looks like a usable linux distribution (init.d tree or an nfqws binary).
    static bool isLinuxRoot(const QString &root);
    // Convenience: Windows / Linux / Unknown based on the markers above (Windows checked first).
    static Strategy::Platform detectPlatform(const QString &root);

private:
    ZapretPaths() = delete;
};

} // namespace zapret
