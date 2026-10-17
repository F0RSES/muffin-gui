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
    static QString nfqwsBuiltPath(const QString &root);       // nfq/nfqws  (what the init scripts exec)
    // True when the linux tree is INSTALLED, i.e. its sysv/openrc init script can actually bring the
    // daemon up. See the definition for the exact on-disk condition.
    static bool isLinuxInstalled(const QString &root);
    static QString binariesDir(const QString &root);         // binaries/

    // The arch directory name that matches the host, e.g. "linux-x86_64". Empty if we can't map
    // QSysInfo::currentCpuArchitecture() to a binaries/ subdir.
    static QString nativeArchDirName();

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
