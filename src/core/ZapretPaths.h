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

    // ---- the system-wide tree a download is promoted into (POSIX only) ----
    // "/opt/muffin/zapret", or empty on Windows, where there is no polkit and no root-owned tree to
    // promote into. Empty means "there is no such place", exactly like managedInstallDir() empty means
    // "this host offers no app-data directory" — callers must not read it as "the root directory".
    //
    // Why a directory of its own instead of /opt/zapret: upstream's own scripts default to that
    // (init.d/sysv/defaults.sh: ZAPRET_BASE=${ZAPRET_BASE:-/opt/zapret}), so a user who set zapret up
    // by hand already has a tree there that Muffin must never overwrite. /opt/muffin is this
    // application's namespace: nothing else claims it, and the packaging scripts may clean it up.
    //
    // Why it has to be root-owned: packaging/postinst installs a polkit rule that lets a payload under
    // this tree be exec'd through pkexec WITHOUT a password. That is only sound while an unprivileged
    // user cannot replace the bytes there. A directory in the user's own home fails that test — he owns
    // it, so whoever wrote the file owns the root shell that follows — which is why a download is
    // staged in managedInstallDir() and then promoted here by the root helper (MuffinInstallHelper)
    // instead of being exec'd in place.
    //
    // Fixed at compile time on purpose: no environment variable, no Settings key and no argument
    // reaches this value, because the helper reads it while running as root and a redirectable
    // destination would let an unprivileged user aim that root write at any directory on the machine.
    // packaging/check-polkit-rule.js reads this one literal back out of the source and requires the
    // polkit rule to carry the same prefix, so the two cannot drift apart silently.
    static QString systemInstallDir();

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
