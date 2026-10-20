#pragma once

#include "Strategy.h"

#include <QFileInfo>
#include <QFileInfoList>
#include <QList>
#include <QString>

namespace zapret {

// Turns a zapret distribution directory into a list of Strategy objects.
//
// Everything the GUI runs is derived from what is on disk; nothing is hardcoded except the
// exclusion lists below. Two passes happen:
//   * file names decide the display name and the launcher kind (see Strategy::Kind)
//   * the extension decides nothing about the arguments, because the strategies are wrappers
//     that carry their own parameters
class StrategyParser
{
public:
    // File names that are never offered to the user even though they look like strategies.
    static QStringList excludedNames(Strategy::Platform platform);

    // Directories that never contain strategies (binaries, lists, helper trees, other platforms).
    static QStringList excludedDirs(Strategy::Platform platform);

    // The platform whose layout dirPath matches, Unknown if nothing does.
    static Strategy::Platform guessPlatform(const QString &dirPath);

    // "general (ALT5).bat" -> "general (ALT5)". Strips the extension and any "test_" prefix.
    static QString displayName(const QString &fileName);

    // The kind a file would be launched as. Unknown for anything that isn't a strategy payload.
    static Strategy::Kind kindOf(const QFileInfo &fi, Strategy::Platform platform);

    // Shallow scan of one directory. Platform::Unknown means "accept whatever this directory holds".
    static QList<Strategy> parse(const QString &dirPath,
                                 Strategy::Platform platform = Strategy::Platform::Unknown);

    // Scan plus a bounded walk into strategy subdirectories (init.d/sysv, helpers, ...).
    // Results are unique by name and ordered by descending weight, then by name.
    static QList<Strategy> parseTree(const QString &dirPath,
                                     Strategy::Platform platform = Strategy::Platform::Unknown,
                                     int maxDepth = 3);

    // ---- Linux bring-up procedures (portable mode) ----
    //
    // A linux tree exposes two genuinely different ways to bring zapret up, and which one WORKS
    // depends on whether the tree was installed. This pass labels every linux strategy with its
    // Strategy::RunMode and re-weights it accordingly:
    //
    //   * the sysv/openrc "zapret" script   -> RunMode::InitScript
    //         weight 100 when the tree is installed (ZapretPaths::isLinuxInstalled), so the documented
    //         one-click daemon stays first; weight 30 when it is not, because `pkexec <script> start`
    //         is then guaranteed to fail (it execs <root>/nfq/nfqws, which an unpacked tree does not have).
    //   * <root>/binaries/<arch>/nfqws      -> RunMode::PortableNfqws, weight 50, created here.
    //         `binaries` is an excluded dir, so the tree walk never finds this payload; the strategy is
    //         synthesized from ZapretPaths::nfqwsPath() instead. It carries portableNfqwsDefaultArgs()
    //         in argv, which is what makes it runnable without the user typing flags.
    //
    // Both are always emitted when both are possible: the GUI then shows a working option whatever the
    // install state, and the weights merely decide which one floats to the top.
    //
    // Windows strategies are left untouched. Safe to call repeatedly (it matches on filePath).
    static void applyLinuxRunModes(QList<Strategy> &strategies, const QString &rootPath,
                                   Strategy::Platform platform = Strategy::Platform::Unknown);


    // Fills in executablePath / primaryArg / workingDirectory / profileTemplate / requiresRoot.
    static void applyDefaults(QList<Strategy> &strategies, const QString &rootPath,
                              Strategy::Platform platform);

    // Removes entries with a duplicate (name, kind) pair, then sorts by weight.
    static QList<Strategy> deduplicated(const QList<Strategy> &strategies);

    // ---- Direct winws.exe resolution (Windows wrapper -> payload command line) ----
    //
    // A windows *.bat is really a thin wrapper: it sets %BIN%/%LISTS%, calls service.bat to
    // prepare lists, then runs `start "title" /min "%BIN%winws.exe" <args...>` (with the arg
    // vector spread over several `^`-continued lines). parseWindowsCommand reads that file and
    // rebuilds the exact winws.exe command so the backend can exec the binary directly instead of
    // going through cmd.exe.
    struct WinCommand
    {
        bool ok = false;               // false when no winws.exe command line could be found
        QString executablePath;        // the resolved path to bin/winws.exe
        QStringList argv;              // winws argument vector (does NOT include the program)
        QString workingDirectory;      // the directory the *.bat lives in
        QString error;                 // human-readable reason when !ok
    };

    // Parse one *.bat into its winws.exe command. Reads the file as text (UTF-8 when the batch
    // selects chcp 65001, otherwise the locale codec), joins `^` continuations, expands %~dp0,
    // %~n0, %BIN%, %LISTS% and treats any other unresolved %VAR% as empty (dropping the comma it
    // joined). Quote-aware: a `"..."` region is one token and keeps any spaces inside it.
    static WinCommand parseWindowsCommand(const QString &batPath, const QString &batDirPath = QString());

    // For every Windows Batch strategy in the list, replace the cmd.exe wrapper form with the
    // resolved direct-winws form: executablePath/argv/workingDirectory are filled from
    // parseWindowsCommand, kind becomes Binary (DirectBinary), requiresRoot stays true. Strategies
    // whose parse fails are left untouched (the wrapper form still applies). Safe to call more
    // than once. Purely textual, so it works even when run on a non-Windows host.
    static void resolveWindowsCommands(QList<Strategy> &strategies);

    // ---- the list files an argv asks the payload to read ----
    //
    // zapret's windows wrappers point several flags at the "-user" files -- list-general-user.txt,
    // ipset-exclude-user.txt and their siblings -- and a release archive does not contain them:
    // upstream creates them with zz-make-user-files.bat. This backend execs winws.exe instead of
    // running the wrapper, so that batch never gets a chance to, and the payload reports that it
    // cannot access the file. Handing the paths back lets the launcher create exactly the
    // placeholders the child is about to be told to read, derived from the command line that names
    // them, so the two cannot drift apart.
    //
    // `argv` is the resolved argument vector (the --flag=<path> form, or a bare path) and `root` the
    // tree the payload lives in. A value is returned only when all of these hold: its file name is
    // a "-user" placeholder, it resolves under `root`, the file does not already exist, and the
    // directory it belongs to does. Anything else the command line names is deliberately left alone --
    // a main list or a hostlist the archive was supposed to ship is a real missing file, and papering
    // over it with an empty one would turn a loud failure into a silent no-op.
    //
    // Pure path arithmetic: nothing is created here, and nothing is read from disk but existence.
    static QStringList missingUserListFiles(const QStringList &argv, const QString &root);

private:
    StrategyParser() = delete;

    static bool isExcludedName(const QString &fileName, Strategy::Platform platform);
    static bool isExcludedDir(const QString &dirName, Strategy::Platform platform);
    static void collect(const QString &dirPath, Strategy::Platform platform, int depth, int maxDepth,
                        QList<Strategy> &out);
};

} // namespace zapret
