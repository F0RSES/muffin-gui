#pragma once

#include <QString>
#include <QStringList>

namespace zapret {

// One user-selectable way to bring zapret up.
//
// A Strategy is pure data describing WHAT is on disk and HOW it should be launched. It is
// produced by StrategyParser and consumed later by the (not-yet-written) execution backend,
// so nothing here spawns processes or touches the network.
//
// The shape mirrors the two real distribution layouts:
//
//   windows/                     linux/
//     general.bat       <- S       init.d/sysv/zapret      <- S (primary)
//     general (ALT5).bat <- S      binaries/linux-x86_64/nfqws <- S (secondary)
//     service.bat       (helper)   config.default          (data)
//     bin/winws.exe     (payload)  install_easy.sh         (helper)
struct Strategy
{
    // Which distribution tree the entry was found in. Drives path + launch assumptions.
    enum class Platform
    {
        Unknown,
        Windows,
        Linux
    };

    // How the payload must be started. The launcher (cmd / pkexec / systemd / the binary itself)
    // is implied by the kind; see StrategyParser::applyDefaults for the exact command it yields.
    //
    // Launch-vocabulary mapping to the backend spec (which speaks in terms of BatScript /
    // InitScript / DirectBinary). The enum names below are kept for backward compatibility with
    // the parser and existing tests, and are the single source of truth:
    //   Batch        == BatScript     (a windows wrapper script)
    //   ShellScript  == InitScript    (a linux init.d launcher driven via pkexec)
    //   Binary       == DirectBinary  (exec the payload binary itself: winws.exe / nfqws)
    // Once StrategyParser::resolveWindowsCommands() turns a *.bat into its underlying winws.exe
    // invocation the entry's kind becomes Binary (DirectBinary): the GUI then launches winws.exe
    // directly with argv instead of shelling out to cmd.exe /c "<bat>".
    enum class Kind
    {
        Unknown,
        Batch,        // windows *.bat / *.cmd  ->  cmd.exe /c "<file>"  (BatScript)
        PowerShell,   // *.ps1                  ->  powershell -File "<file>"
        ShellScript,  // linux zapret init script -> pkexec "<file>" start  (InitScript)
        SystemdUnit,  // *.service              ->  systemd unit install target (not self-launched)
        Binary        // direct nfqws / winws payload invocation (DirectBinary)
    };

    QString id;                // stable unique id (defaults to filePath)
    QString name;              // display name, e.g. "general", "general (ALT5)"
    QString description;       // one-line human summary for the list view ("" until populated)
    QString fileName;          // name as it appears on disk, e.g. "general (ALT5).bat"
    QString dirPath;           // absolute path of the containing directory
    QString filePath;          // absolute path of the payload file itself

    Kind kind = Kind::Unknown;
    Platform platform = Platform::Unknown;

    // Filled by StrategyParser::applyDefaults() (the "wrapper" launch form):
    QString executablePath;    // program to exec ("cmd.exe", "pkexec", the binary, ...)
    QString primaryArg;        // the payload path passed to executablePath ("" if unused)
    QString workingDirectory;  // cwd to launch in
    QString profileTemplate;   // associated config/profile hint ("" when none)
    bool requiresRoot = false; // needs elevation (admin / root) to run

    // The fully-resolved command line for the backend, WITHOUT the program itself.
    // For a Windows *.bat resolved via resolveWindowsCommands() this holds the winws.exe
    // argument vector (e.g. "--wf-tcp=80,443,...", "--dpi-desync=fake", "--new", ...) and
    // executablePath points straight at bin/winws.exe. For other kinds it stays empty and the
    // backend falls back to executablePath + primaryArg.
    QStringList argv;

    // Relative desirability used purely for ordering the presented list. Higher runs first.
    // Not a correctness signal; a heuristic so the recommended entry floats to the top.
    int weight = 0;

    bool isValid() const { return kind != Kind::Unknown && !filePath.isEmpty(); }
};

} // namespace zapret
