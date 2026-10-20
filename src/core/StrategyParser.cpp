#include "StrategyParser.h"

#include "PortableMode.h"
#include "WindowsArgs.h"
#include "ZapretPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfoList>
#include <QHash>
#include <QStringList>
#include <algorithm>

namespace zapret {

namespace {

QString lower(const QString &s) { return s.toLower(); }

bool hasSuffix(const QString &name, std::initializer_list<const char *> suffixes)
{
    const QString n = lower(name);
    for (const char *s : suffixes)
        if (n.endsWith(QLatin1String(s)))
            return true;
    return false;
}

// Resolve which platform a single file belongs to, given an already-guessed directory platform.
// The directory guess wins; otherwise infer from what kind of launcher the file is.
Strategy::Platform resolvePlatform(const Strategy::Kind kind, const QString &fileName,
                                   Strategy::Platform dirGuess)
{
    if (dirGuess != Strategy::Platform::Unknown)
        return dirGuess;
    switch (kind)
    {
    case Strategy::Kind::Batch:
    case Strategy::Kind::PowerShell:
        return Strategy::Platform::Windows;
    case Strategy::Kind::SystemdUnit:
        return Strategy::Platform::Linux;
    case Strategy::Kind::ShellScript:
        return Strategy::Platform::Linux;
    case Strategy::Kind::Binary:
        return hasSuffix(fileName, {".exe", ".sys", ".dll"}) ? Strategy::Platform::Windows
                                                             : Strategy::Platform::Linux;
    case Strategy::Kind::Unknown:
        break;
    }
    return Strategy::Platform::Unknown;
}

// Desirability heuristic so the recommended entry floats to the top. Purely for list ordering;
// it never affects how something is launched. Higher runs first.
//
// The linux ShellScript numbers here are only a PRE-pass guess: they encode sysv-beats-openrc so the
// tree walk has something to deduplicate on. StrategyParser::applyLinuxRunModes is what decides the
// final weight, because ranking the init script correctly requires knowing the tree's install state,
// which a per-file heuristic cannot see.
int weightFor(const Strategy &s)
{
    switch (s.kind)
    {
    case Strategy::Kind::Batch:
        // "general.bat" is the curated default; the (ALT*)/(FAKE*) variants are tunings.
        return s.name == QLatin1String("general") ? 100 : 60;
    case Strategy::Kind::ShellScript:
        // sysv "zapret" is the primary documented linux launcher; openrc is the close alternative.
        if (s.name == QLatin1String("zapret"))
            return lower(s.filePath).contains(QLatin1String("/sysv/")) ? 100 : 90;
        return 55;
    case Strategy::Kind::SystemdUnit:
        return 50;
    case Strategy::Kind::PowerShell:
        return 45;
    case Strategy::Kind::Binary:
        // Direct nfqws/winws runs are advanced (no list loading) — always below the wrappers.
        return 40;
    case Strategy::Kind::Unknown:
        break;
    }
    return 0;
}

// Portable-mode presentation weights (docs/zapret_analysis.md section 4). They are deliberately
// spread apart: the init script's 100/30 pair straddles the portable payload's 50, so the two swap
// places the moment the tree gets installed, and neither ties with anything else in the list.
constexpr int kInitScriptInstalledWeight = 100;
constexpr int kInitScriptUninstalledWeight = 30;
constexpr int kPortableNfqwsWeight = 50;

// The final weight of an InitScript strategy. `installed` decides the tier; inside each tier sysv
// keeps a one-point lead over openrc so deduplicated() — which collapses every entry named "zapret"
// of kind ShellScript down to the highest-weighted one — still keeps the sysv copy either way.
int weightForInitScript(const Strategy &s, bool installed)
{
    const bool primary = lower(s.filePath).contains(QLatin1String("/sysv/"));
    const int tier = installed ? kInitScriptInstalledWeight : kInitScriptUninstalledWeight;
    return primary ? tier : tier - 1;
}

// True when both paths name the same on-disk file. QFileInfo::operator== compares canonical paths,
// which is what lets a synthesized strategy and a tree-walked one be recognised as the same payload
// even when one path came from QDir (native separators) and the other from ZapretPaths ('/').
bool sameFile(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty())
        return false;
    return QFileInfo(a) == QFileInfo(b);
}

} // namespace

QStringList StrategyParser::excludedNames(Strategy::Platform platform)
{
    // Names that look like strategies but are helpers or data. Matched case-insensitively.
    QStringList out;
    auto add = [&out](const char *n) { out << QString::fromLatin1(n); };

    if (platform == Strategy::Platform::Windows || platform == Strategy::Platform::Unknown)
    {
        add("service.bat");            // called by the general*.bat wrappers, not standalone
        add("readme.bat");
    }
    if (platform == Strategy::Platform::Linux || platform == Strategy::Platform::Unknown)
    {
        // Root helper / installer / data files that are not launchers the user picks.
        add("blockcheck.sh");
        add("install_easy.sh");
        add("install_bin.sh");
        add("install_prereq.sh");
        add("uninstall_easy.sh");
        add("update.sh");
        add("functions");             // sourced by the init scripts, not executed directly
        add("config.default");
        add("Makefile");
        add("zapret.sh");             // pfsense helper naming
    }
    return out;
}

QStringList StrategyParser::excludedDirs(Strategy::Platform platform)
{
    QStringList out;
    auto add = [&out](const char *n) { out << QString::fromLatin1(n); };

    // Universal noise.
    add(".git");
    add(".svn");
    add("__MACOSX");

    if (platform == Strategy::Platform::Windows || platform == Strategy::Platform::Unknown)
    {
        add("bin");
        add("lists");
        add("utils");
    }
    if (platform == Strategy::Platform::Linux || platform == Strategy::Platform::Unknown)
    {
        // Payload trees and source dirs that never contain selectable launchers.
        add("binaries");
        add("docs");
        add("files");
        add("common");
        add("ip2net");
        add("ipset");
        add("mdig");
        add("nfq");
        add("tpws");
        add("tmp");
        // init.d subdirectories for other OSes / init systems we do not offer on a generic linux
        // host. sysv + openrc stay selectable; systemd units are installed, not launched here.
        add("macos");
        add("openwrt");
        add("openwrt-minimal");
        add("pfsense");
        add("runit");
        add("s6");
        add("systemd");
        add("custom.d");
        add("custom.d.examples");
        add("custom.d.examples.linux");
        add("windivert.filter.examples");
    }
    return out;
}

Strategy::Platform StrategyParser::guessPlatform(const QString &dirPath)
{
    return ZapretPaths::detectPlatform(dirPath);
}

QString StrategyParser::displayName(const QString &fileName)
{
    QString stem = QFileInfo(fileName).completeBaseName();  // "general (ALT5).bat" -> "general (ALT5)"
    static const int kTestPrefixLen = int(QByteArrayLiteral("test_").size());  // 5, not 6
    if (stem.startsWith(QLatin1String("test_"), Qt::CaseInsensitive))
        stem.remove(0, kTestPrefixLen);
    return stem;
}

Strategy::Kind StrategyParser::kindOf(const QFileInfo &fi, Strategy::Platform platform)
{
    // Classify by name. parse() already restricts the listing to regular files, so here we only
    // need to reject directories (and, via the explicit checks below, anything that isn't a
    // recognized launcher name). Avoid requiring on-disk existence so kindOf stays a pure mapping.
    if (fi.isDir())
        return Strategy::Kind::Unknown;

    const QString name = fi.fileName();
    const QString ext = fi.suffix().toLower();

    if (ext == QLatin1String("bat") || ext == QLatin1String("cmd"))
        return Strategy::Kind::Batch;
    if (ext == QLatin1String("ps1"))
        return Strategy::Kind::PowerShell;
    if (ext == QLatin1String("service"))
        return Strategy::Kind::SystemdUnit;
    if (ext == QLatin1String("exe"))
        return Strategy::Kind::Binary;
    if (ext == QLatin1String("sh"))
        return Strategy::Kind::ShellScript;

    // Extensionless launchers: the sysv/openrc "zapret" init script is a shell script; a bare
    // nfqws/tpws is a direct payload binary. Everything else (functions, Makefile, ...) is Unknown.
    if (name == QLatin1String("zapret"))
        return Strategy::Kind::ShellScript;
    if (name == QLatin1String("nfqws") || name == QLatin1String("tpws"))
        return Strategy::Kind::Binary;

    Q_UNUSED(platform);
    return Strategy::Kind::Unknown;
}

bool StrategyParser::isExcludedName(const QString &fileName, Strategy::Platform platform)
{
    const QString n = lower(fileName);
    const QStringList ex = excludedNames(platform);
    return std::any_of(ex.begin(), ex.end(), [&n](const QString &e) { return lower(e) == n; });
}

bool StrategyParser::isExcludedDir(const QString &dirName, Strategy::Platform platform)
{
    const QString n = lower(dirName);
    const QStringList ex = excludedDirs(platform);
    return std::any_of(ex.begin(), ex.end(), [&n](const QString &e) { return lower(e) == n; });
}

QList<Strategy> StrategyParser::parse(const QString &dirPath, Strategy::Platform platform)
{
    QList<Strategy> out;
    QDir dir(dirPath);
    if (!dir.exists())
        return out;

    const Strategy::Platform dirGuess =
        (platform != Strategy::Platform::Unknown) ? platform : guessPlatform(dirPath);

    const QFileInfoList entries =
        dir.entryInfoList({QStringLiteral("*")}, QDir::Files, QDir::Name);
    for (const QFileInfo &fi : entries)
    {
        const QString fileName = fi.fileName();
        if (isExcludedName(fileName, platform))
            continue;

        const Strategy::Kind kind = kindOf(fi, dirGuess);
        if (kind == Strategy::Kind::Unknown)
            continue;

        Strategy s;
        s.fileName = fileName;
        s.filePath = fi.absoluteFilePath();
        s.dirPath = dir.absolutePath();
        s.name = displayName(fileName);
        s.id = s.filePath;
        s.kind = kind;
        s.platform = resolvePlatform(kind, fileName, dirGuess);
        s.weight = weightFor(s);
        out.append(s);
    }
    return out;
}

void StrategyParser::collect(const QString &dirPath, Strategy::Platform platform, int depth,
                             int maxDepth, QList<Strategy> &out)
{
    out += parse(dirPath, platform);
    if (depth >= maxDepth)
        return;

    QDir dir(dirPath);
    const QFileInfoList subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &d : subdirs)
    {
        if (isExcludedDir(d.fileName(), platform))
            continue;
        collect(d.absoluteFilePath(), platform, depth + 1, maxDepth, out);
    }
}

QList<Strategy> StrategyParser::parseTree(const QString &dirPath, Strategy::Platform platform,
                                          int maxDepth)
{
    QList<Strategy> raw;
    collect(dirPath, platform, 0, std::max(0, maxDepth), raw);

    // Run modes go first: this is the only pass that sees the whole tree (so it can judge install
    // state), and it may APPEND a strategy that applyDefaults still has to fill in.
    applyLinuxRunModes(raw, dirPath, platform);

    // applyDefaults needs the root; when the caller handed us the root, pass it through, then
    // re-dedup + order the whole presented list.
    applyDefaults(raw, dirPath, platform);
    return deduplicated(raw);
}

void StrategyParser::applyLinuxRunModes(QList<Strategy> &strategies, const QString &rootPath,
                                        Strategy::Platform platform)
{
    Strategy::Platform plat = platform;
    if (plat == Strategy::Platform::Unknown)
        plat = guessPlatform(rootPath);
    if (plat != Strategy::Platform::Linux)
        return;   // windows trees have no bring-up choice to make

    const bool installed = ZapretPaths::isLinuxInstalled(rootPath);
    const bool configured = ZapretPaths::isLinuxConfigured(rootPath);

    for (Strategy &s : strategies)
    {
        if (s.platform != Strategy::Platform::Linux)
            continue;

        if (s.kind == Strategy::Kind::ShellScript)
        {
            // Label every linux controller script explicitly rather than leaving the backend to
            // infer it from kind, and re-rank the zapret daemon by whether it can actually start.
            s.runMode = Strategy::RunMode::InitScript;
            if (s.name == QLatin1String("zapret"))
            {
                s.weight = weightForInitScript(s, installed);
                if (!installed && s.description.isEmpty())
                {
                    // Say why the documented one-click launcher is ranked below a raw binary: it will
                    // fail with "nfqws not found" until install_easy.sh / install_bin.sh put the
                    // daemon binary at <root>/nfq/nfqws.
                    s.description = QStringLiteral(
                        "init script (not installed: %1 is missing, start will fail)")
                        .arg(QFileInfo(ZapretPaths::nfqwsBuiltPath(rootPath)).fileName());
                }
            }
        }
    }

    // ---- an unpacked tree gets no init-script card at all ----
    //
    // Ranking a launcher below the payload says "this one is less good"; hiding it says "this one is
    // not a choice". A tree without <root>/config is the second case, and it is the case the GUI's own
    // download path puts every user in: init.d/sysv/functions sources $ZAPRET_RW/config as its first
    // act, so `pkexec <script> start` dies on "cannot open config" before it can read a single option.
    // A card that reproduces a guaranteed failure is worse than no card — the user has one working
    // bring-up (portable nfqws) and one that only looks like the documented zapret install.
    //
    // The entries are removed rather than left in and disabled: the parser is what both the card list
    // and the tray menu read, so one decision here cannot drift into a place that forgot to check it.
    // Running install_easy.sh creates the file and brings the card back on the next rescan — this is a
    // statement about the tree's state, not about a platform being unsupported.
    if (!configured)
    {
        for (int i = strategies.size() - 1; i >= 0; --i)
        {
            if (strategies.at(i).runMode == Strategy::RunMode::InitScript)
                strategies.removeAt(i);
        }
    }

    // ---- the portable payload ----
    //
    // `binaries` is an excluded dir, so the walk above never sees <root>/binaries/<arch>/nfqws and
    // this strategy has to be synthesized. nfqwsPath() already applies the native-arch preference and
    // returns "" when the tree ships no usable payload, in which case portable mode is not offered
    // at all (the GUI would only be able to start something that cannot run).
    const QString nfqws = ZapretPaths::nfqwsPath(rootPath);
    if (nfqws.isEmpty())
        return;

    // A tree that carries a bare nfqws where the walk DOES look (e.g. <root>/nfqws, the leftover
    // install_bin.sh leaves behind) already produced a Kind::Binary entry for it. Adopt that one
    // instead of appending a twin with a different id for the same file.
    for (Strategy &s : strategies)
    {
        if (s.kind == Strategy::Kind::Binary && s.platform == Strategy::Platform::Linux &&
            sameFile(s.filePath, nfqws))
        {
            s.runMode = Strategy::RunMode::PortableNfqws;
            s.weight = kPortableNfqwsWeight;
            if (s.argv.isEmpty())
                s.argv = portableNfqwsDefaultArgs();
            if (s.workingDirectory.isEmpty())
                s.workingDirectory = QDir(rootPath).absolutePath();
            if (s.description.isEmpty())
                s.description = QStringLiteral("nfqws (portable: firewall rules managed by the GUI)");
            return;
        }
    }

    Strategy p;
    const QFileInfo fi(nfqws);
    p.id = fi.absoluteFilePath();
    p.name = QStringLiteral("nfqws (portable)");
    p.description = QStringLiteral("nfqws (portable: firewall rules managed by the GUI)");
    p.fileName = fi.fileName();
    p.dirPath = fi.absolutePath();
    p.filePath = fi.absoluteFilePath();
    p.kind = Strategy::Kind::Binary;        // what is on disk: an executable payload
    p.platform = Strategy::Platform::Linux;
    p.runMode = Strategy::RunMode::PortableNfqws;   // how the backend owns its lifecycle
    p.weight = kPortableNfqwsWeight;
    // The default flags live in argv, the one field the backend already reads for a direct payload:
    // no parallel "portable args" concept for the UI and the tests to keep in sync. portable mode
    // needs them (unlike the init script, which composes its own from config.default) because nothing
    // else supplies them, and a raw nfqws with no --qnum would simply refuse to start.
    p.argv = portableNfqwsDefaultArgs();
    // Run from the tree root, not from binaries/<arch>/: every relative path a user may add to argv
    // (lists, ipset scripts, nfqws' own helper files) is rooted at the zapret tree.
    p.workingDirectory = QDir(rootPath).absolutePath();
    // executablePath / requiresRoot / profileTemplate are filled by applyDefaults() for Kind::Binary.
    strategies.append(p);
}

void StrategyParser::applyDefaults(QList<Strategy> &strategies, const QString &rootPath,
                                   Strategy::Platform platform)
{
    // config.default is the natural profile anchor for linux wrappers.
    const QString profile = (platform != Strategy::Platform::Windows)
                                ? ZapretPaths::configDefaultPath(rootPath)
                                : QString();
    const bool profileExists = !profile.isEmpty() && QFileInfo::exists(profile);

    for (Strategy &s : strategies)
    {
        // A working directory equal to the payload's own dir makes the relative "%~dp0bin\..." /
        // "init.d/../binaries" references inside the wrappers resolve correctly.
        if (s.workingDirectory.isEmpty())
            s.workingDirectory = s.dirPath;
        if (s.profileTemplate.isEmpty() && profileExists)
            s.profileTemplate = profile;

        switch (s.kind)
        {
        case Strategy::Kind::Batch:
            s.executablePath = QStringLiteral("cmd.exe");
            s.primaryArg = s.filePath;   // backend runs: cmd.exe /c "<filePath>"
            s.requiresRoot = true;       // WinDivert driver load needs admin
            break;
        case Strategy::Kind::PowerShell:
            s.executablePath = QStringLiteral("powershell.exe");
            s.primaryArg = s.filePath;   // powershell -ExecutionPolicy Bypass -File "<file>"
            s.requiresRoot = true;
            break;
        case Strategy::Kind::ShellScript:
            s.executablePath = QStringLiteral("pkexec");
            s.primaryArg = s.filePath;   // pkexec "<filePath>" start|stop|restart
            s.requiresRoot = true;
            break;
        case Strategy::Kind::SystemdUnit:
            s.executablePath = QStringLiteral("systemctl");
            s.primaryArg = s.filePath;   // unit is installed/enabled, not directly exec'd
            s.requiresRoot = true;
            break;
        case Strategy::Kind::Binary:
            s.executablePath = s.filePath;
            s.primaryArg.clear();        // advanced: the GUI supplies nfqws args itself
            s.requiresRoot = true;
            break;
        case Strategy::Kind::Unknown:
            break;
        }
    }
}

QList<Strategy> StrategyParser::deduplicated(const QList<Strategy> &strategies)
{
    // Keep the best-weighted entry for each (name, kind) identity.
    QList<Strategy> kept;
    for (const Strategy &s : strategies)
    {
        int idx = -1;
        for (int i = 0; i < kept.size(); ++i)
        {
            if (kept[i].name == s.name && kept[i].kind == s.kind)
            {
                idx = i;
                break;
            }
        }
        if (idx < 0)
            kept.append(s);
        else if (s.weight > kept[idx].weight)
            kept[idx] = s;   // replace with a higher-weighted duplicate (e.g. sysv beats openrc)
    }

    std::stable_sort(kept.begin(), kept.end(), [](const Strategy &a, const Strategy &b) {
        if (a.weight != b.weight)
            return a.weight > b.weight;      // descending weight
        return a.name.localeAwareCompare(b.name) < 0;   // then ascending name
    });
    return kept;
}

namespace {

// Split raw file bytes into text lines with the trailing '\r' removed.
QStringList physicalLines(const QString &text)
{
    const QStringList raw = text.split(QLatin1Char('\n'));
    QStringList out;
    out.reserve(raw.size());
    for (const QString &l : raw)
    {
        QString t = l;
        if (t.endsWith(QLatin1Char('\r')))
            t.chop(1);
        out.append(t);
    }
    return out;
}

// Read a *.bat as text. The batch selects a code page with `chcp NNNN`; Flowseal's wrappers use
// 65001 (UTF-8). We honour that marker and otherwise fall back to the locale codec. Exact CP866 /
// CP1251 decoding is left as a TODO (Qt6 moved QTextCodec behind Qt5Compat); the shipped trees are
// UTF-8 so this is enough for a faithful argv on both platforms.
QString readBatText(const QString &batPath, bool *ok, QString *err)
{
    QFile f(batPath);
    if (!f.open(QIODevice::ReadOnly))
    {
        if (ok) *ok = false;
        if (err) *err = QStringLiteral("cannot open '%1': %2").arg(batPath, f.errorString());
        return QString();
    }
    const QByteArray bytes = f.readAll();
    f.close();

    const QString probe = QString::fromLatin1(bytes.left(400));
    const int chcp = probe.indexOf(QLatin1String("chcp"), 0, Qt::CaseInsensitive);
    bool utf8 = true;   // default: the wrappers declare `chcp 65001`
    if (chcp >= 0)
    {
        QString digits;
        for (int i = chcp + 4; i < probe.size(); ++i)
        {
            const QChar c = probe.at(i);
            if (c.isDigit())
                digits.append(c);
            else if (!digits.isEmpty() || c != QLatin1Char(' '))
                break;
        }
        if (!digits.isEmpty())
            utf8 = (digits.toInt() == 65001 || digits.toInt() == 65000);
    }

    if (ok) *ok = true;
    return utf8 ? QString::fromUtf8(bytes) : QString::fromLocal8Bit(bytes);
}

// Join `^`-continued physical lines into logical command lines. A physical line whose last
// character is a caret is a continuation: the caret is dropped and the next line is appended with
// a single separating space.
QStringList logicalLines(const QStringList &physical)
{
    QStringList logical;
    QString cur;
    bool open = false;
    for (const QString &line : physical)
    {
        QString t = line;
        if (t.endsWith(QLatin1Char('^')))
        {
            t.chop(1);
            cur += t;
            if (!t.isEmpty())
                cur += QLatin1Char(' ');
            open = true;
            continue;
        }
        cur += t;
        if (open || !cur.trimmed().isEmpty())
            logical.append(cur);
        cur.clear();
        open = false;
    }
    if (!cur.trimmed().isEmpty())
        logical.append(cur);
    return logical;
}

// Split one logical command line into tokens, honouring "..." regions: quotes are removed and a
// quoted region is a single token even if it contains spaces.
QStringList tokenize(const QString &line)
{
    QStringList toks;
    QString cur;
    bool inQuote = false;
    bool started = false;
    for (const QChar c : line)
    {
        if (c == QLatin1Char('"'))
        {
            inQuote = !inQuote;
            started = true;
            continue;
        }
        if (!inQuote && (c == QLatin1Char(' ') || c == QLatin1Char('\t')))
        {
            if (started)
            {
                toks.append(cur);
                cur.clear();
                started = false;
            }
            continue;
        }
        cur.append(c);
        started = true;
    }
    if (started)
        toks.append(cur);
    return toks;
}

// Collapse repeated commas and drop a single trailing comma. Empty %VAR% expansions inside a
// comma-separated list (e.g. "--wf-tcp=80,443,%GameFilterTCP%") otherwise leave a dangling comma.
QString tidyCommas(QString t)
{
    while (t.contains(QLatin1String(",,")))
        t.replace(QLatin1String(",,"), QLatin1String(","));
    if (t.endsWith(QLatin1String(",")))
        t.chop(1);
    return t;
}

// True when an expanded token carries no value at all. Two shapes reach this from a *.bat, both born
// when a variable the wrapper expects somebody else to have set expands to nothing:
//
//   "--filter-tcp=%GameFilterTCP%"   ->  "--filter-tcp="   (the flag kept its name, lost its value)
//   "%GameFilterTCP%"                ->  ""                (the whole token was the variable)
//
// %GameFilterTCP%/UDP% are set by `call service.bat load_game_filter`, which this app does not run,
// so every wrapper that passes them as a flag value produces the first shape. Handing winws.exe an
// "--filter-tcp=" with nothing behind it is not ignored by the payload: it quits with
// "Invalid port filter :", which looks like a broken strategy rather than like an unset variable. An
// empty argument is equally wrong -- QProcess quotes it as "" and the child gets one more positional.
//
// The value is everything after the FIRST '=', so a token whose value merely ends in one ("--x=YWQ==")
// still has a value and is kept; only a value that is genuinely absent is dropped.
bool isValuelessArgument(const QString &t)
{
    if (t.isEmpty())
        return true;
    const qsizetype eq = t.indexOf(QLatin1Char('='));
    return eq >= 0 && eq == t.size() - 1;
}

bool isWinwsToken(const QString &t)
{
    const int sep = std::max(t.lastIndexOf(QLatin1Char('/')), t.lastIndexOf(QLatin1Char('\\')));
    const QString base = (sep >= 0) ? t.mid(sep + 1) : t;
    return base.compare(QLatin1String("winws.exe"), Qt::CaseInsensitive) == 0;
}

} // namespace

StrategyParser::WinCommand StrategyParser::parseWindowsCommand(const QString &batPath,
                                                               const QString &batDirPathIn)
{
    WinCommand r;

    QString dirPath = batDirPathIn;
    if (dirPath.isEmpty())
        dirPath = QFileInfo(batPath).absolutePath();

    bool readOk = false;
    QString readErr;
    const QString text = readBatText(batPath, &readOk, &readErr);
    if (!readOk)
    {
        r.error = readErr;
        return r;
    }

    // Locate the single logical line that invokes winws.exe.
    QString cmdLine;
    bool found = false;
    const QStringList logical = logicalLines(physicalLines(text));
    for (const QString &line : logical)
    {
        if (line.contains(QLatin1String("winws.exe"), Qt::CaseInsensitive))
        {
            cmdLine = line;
            found = true;
            break;
        }
    }
    if (!found)
    {
        r.error = QStringLiteral("no winws.exe command found in '%1'").arg(batPath);
        return r;
    }

    // Variable environment: the windows layout maps BIN/LISTS under the bat directory. QDir
    // separator keeps the resolved paths clean on whichever host parses them.
    const QChar sep = QDir::separator();
    const QString dp0 = dirPath + sep;
    const QString n0 = QFileInfo(batPath).completeBaseName();
    const QString bin = dp0 + QStringLiteral("bin") + sep;
    const QString lists = dp0 + QStringLiteral("lists") + sep;
    // %GameFilterTCP% / %GameFilterUDP% are set by `call service.bat load_game_filter`, which we do
    // not run; they expand to empty (TODO: seed from utils/ when such a file is present).
    QHash<QString, QString> vars;
    vars.insert(QStringLiteral("BIN"), bin);
    vars.insert(QStringLiteral("LISTS"), lists);
    vars.insert(QStringLiteral("GAMEFILTERTCP"), QString());
    vars.insert(QStringLiteral("GAMEFILTERUDP"), QString());

    auto subst = [&dp0, &n0, &vars](const QString &token) {
        QString out;
        int i = 0;
        const int n = token.size();
        while (i < n)
        {
            const QChar c = token.at(i);
            if (c != QLatin1Char('%'))
            {
                out.append(c);
                ++i;
                continue;
            }
            const int close = token.indexOf(QLatin1Char('%'), i + 1);
            if (close < 0)
            {
                out.append(c);   // lone trailing '%' kept literally
                ++i;
                continue;
            }
            const QString name = token.mid(i + 1, close - i - 1);
            if (name.compare(QLatin1String("~dp0"), Qt::CaseInsensitive) == 0)
                out.append(dp0);
            else if (name.compare(QLatin1String("~n0"), Qt::CaseInsensitive) == 0)
                out.append(n0);
            else
                out.append(vars.value(name.toUpper()));   // unknown -> empty
            i = close + 1;
        }
        return out;
    };

    // Tokenize first (so quoted regions survive any spaces an expansion may introduce), then expand
    // variables per token and tidy comma artifacts. The program token is every token strictly after
    // the winws.exe token; `start`, the title string and `/min` precede it and are dropped.
    const QStringList raw = tokenize(cmdLine);
    QStringList expanded;
    expanded.reserve(raw.size());
    for (const QString &t : raw)
        expanded.append(tidyCommas(subst(t)));

    int progIdx = -1;
    for (int i = 0; i < expanded.size(); ++i)
    {
        if (isWinwsToken(expanded.at(i)))
        {
            progIdx = i;
            break;
        }
    }
    if (progIdx < 0)
    {
        r.error = QStringLiteral("winws.exe token not resolvable in '%1'").arg(batPath);
        return r;
    }

    // Everything after the program token is the winws argv. Tokens an unexpanded %VAR% left without a
    // value are dropped here rather than passed on: see isValuelessArgument -- "--filter-tcp=" makes
    // winws.exe abort with "Invalid port filter :", and the *.bat that produced it is not the shape
    // the payload was ever meant to be handed. This is the one place a resolved wrapper's argv is
    // composed, so resolveWindowsCommands() and anything else built on it inherits the cleanup.
    r.executablePath = expanded.at(progIdx);
    QStringList argv;
    argv.reserve(expanded.size() - progIdx - 1);
    for (int i = progIdx + 1; i < expanded.size(); ++i)
    {
        if (isValuelessArgument(expanded.at(i)))
            continue;
        argv.append(expanded.at(i));
    }
    r.argv = argv;
    r.workingDirectory = dirPath;
    r.ok = true;
    return r;
}

void StrategyParser::resolveWindowsCommands(QList<Strategy> &strategies)
{
    for (Strategy &s : strategies)
    {
        if (s.kind != Strategy::Kind::Batch || s.platform != Strategy::Platform::Windows)
            continue;

        const WinCommand w = parseWindowsCommand(s.filePath, s.dirPath);
        if (!w.ok)
            continue;   // keep the cmd.exe wrapper form from applyDefaults

        s.executablePath = w.executablePath;
        s.argv = w.argv;
        s.primaryArg.clear();
        s.workingDirectory = w.workingDirectory;
        s.requiresRoot = true;
        s.kind = Strategy::Kind::Binary;   // == DirectBinary: exec winws.exe with argv directly
        if (s.description.isEmpty())
            s.description = QStringLiteral("winws.exe (%1 direct)").arg(s.name);
    }
}

namespace {

// The file path a winws argument names, or empty when it names none. "--flag=<path>" contributes
// its value; a bare path token contributes itself. Anything that is not a path -- "--new",
// "80,443", "fake,split" -- yields nothing, which is what keeps a flag value that legitimately
// holds a comma or a slash from being mistaken for a file.
QString namedFileOf(const QString &arg)
{
    QString value = arg;
    if (value.startsWith(QLatin1Char('-'))) {
        const qsizetype eq = value.indexOf(QLatin1Char('='));
        if (eq < 0)
            return QString();      // a switch with no value cannot name a file from this token
        value = value.mid(eq + 1);
    }
    return winLooksLikePath(value) ? value : QString();
}

} // namespace

QStringList StrategyParser::missingUserListFiles(const QStringList &argv, const QString &root)
{
    QStringList out;
    if (root.isEmpty())
        return out;

    // Compare on absolute, separator-normalized paths. The argv of a resolved wrapper carries the
    // mixed form Qt produces (a "%~dp0" expansion glued onto a QFileInfo path), and a root passed
    // in by a caller may be relative; Qt's own QFileInfo always speaks in '/', so both sides have
    // to be put in that form before anything is written.
    const QString absRoot = QDir(root).absolutePath();

    for (const QString &arg : argv) {
        const QString value = namedFileOf(arg);
        if (value.isEmpty())
            continue;

        QFileInfo fi(value);
        if (!fi.isAbsolute())
            fi = QFileInfo(QDir(absRoot).absoluteFilePath(value));
        const QString path = fi.absoluteFilePath();

        // Only the editable placeholders, matched on the file name so a directory that happens to
        // contain "-user." in its name cannot qualify, and only inside the tree the payload lives
        // in -- a path outside the root is somebody else's file and must never spring into
        // existence because a strategy happened to be started.
        if (!fi.fileName().contains(QLatin1String("-user."), Qt::CaseInsensitive))
            continue;
        if (!path.startsWith(absRoot + QLatin1Char('/'), Qt::CaseInsensitive))
            continue;
        if (fi.exists())
            continue;                       // shipped by the archive, or already made

        // The directory has to be part of the tree already. A command line gets to name a file the
        // launcher will create because the payload is about to be told to read it; it does not get to
        // invent new directories inside the install, which is what a --ipset-exclude= pointing at a
        // path nobody ever unpacked would otherwise do.
        if (!QFileInfo(fi.absolutePath()).isDir())
            continue;

        out << path;
    }
    out.removeDuplicates();
    return out;
}

} // namespace zapret


