#include "StrategyParser.h"

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

    // applyDefaults needs the root; when the caller handed us the root, pass it through, then
    // re-dedup + order the whole presented list.
    applyDefaults(raw, dirPath, platform);
    return deduplicated(raw);
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

    r.executablePath = expanded.at(progIdx);
    r.argv = QStringList(expanded.cbegin() + progIdx + 1, expanded.cend());
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

} // namespace zapret


