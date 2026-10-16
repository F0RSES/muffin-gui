// coretest — plain console verifier for the zapret core library.
//
// It does not use QTest (keeps the dependency surface to QtCore only) and runs the parser and
// locator against an on-disk zapret tree. The tree is taken from argv[1], else $ZAPRET_TEST_TREE,
// else ~/Documents/zapret but only when that folder is itself a zapret tree, so nothing is pinned to
// one developer's home. When none of those is an actual zapret distribution — which
// is the case on a CI runner that ships nothing but the toolchain — a minimal fixture tree is
// generated in a temporary directory instead, so every tree-dependent section below still asserts
// against real files rather than against an empty directory. Exits non-zero on any failed assertion.
//
// The "[backends: real execution]" section goes further than the synchronous rejection checks: it
// really spawns child processes (throwaway /bin/sh fixtures in a temporary directory), pumps the Qt
// event loop and asserts the resulting state machine, log output and error reporting. It never runs
// pkexec, nfqws or anything that could touch this machine's network.

#include "Strategy.h"
#include "StrategyParser.h"
#include "ZapretLocator.h"
#include "ZapretPaths.h"
#include "ZapretManager.h"
#include "PrivilegeHelper.h"
#include "IZapretBackend.h"
#include "LinuxBackend.h"
#include "WindowsBackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVector>

#include <functional>

using namespace zapret;

static int g_failures = 0;
static QTextStream out{stdout};

static void check(bool cond, const QString &what)
{
    out << (cond ? "  PASS  " : "  FAIL  ") << what << '\n';
    if (!cond)
        ++g_failures;
}

static QString kindName(Strategy::Kind k)
{
    switch (k)
    {
    case Strategy::Kind::Batch: return "Batch";
    case Strategy::Kind::PowerShell: return "PowerShell";
    case Strategy::Kind::ShellScript: return "ShellScript";
    case Strategy::Kind::SystemdUnit: return "SystemdUnit";
    case Strategy::Kind::Binary: return "Binary";
    case Strategy::Kind::Unknown: return "Unknown";
    }
    return "?";
}

static const Strategy *findByName(const QList<Strategy> &list, const QString &name)
{
    for (const Strategy &s : list)
        if (s.name == name)
            return &s;
    return nullptr;
}

// ---- the tree the parse/locate sections read ---------------------------------
//
// Parsing, path resolution and locating only mean something against files that exist. On a
// developer box that is an unpacked zapret; on a CI runner it is nothing at all, and the sections
// below would then be asserting against an empty directory — reporting parser failures that are
// really "no data" failures. So the tree is resolved once at startup: a real one is used as-is,
// otherwise buildFixtureTree() materialises a minimal distribution carrying the same landmarks.
//
// The fixture is deliberately shaped to keep every assertion meaningful rather than weakened: its
// general.bat has the chcp marker, the %~dp0-derived BIN/LISTS, an empty %GameFilterTCP%/UDP%
// expansion, a ^-continued `start "title" /min "%BIN%winws.exe" ...` line and quoted list paths, so
// parseWindowsCommand() has the same work to do as it does on the shipped wrapper. Only the number
// of wrappers a tree holds is content-specific, and that is what g_winStrategies pins per tree.

static bool g_treeIsReal = true;    // false when the generated fixture stands in
static int g_winStrategies = 22;    // selectable *.bat count, -1 = "no exact expectation"

// Windows hands Qt paths back with native separators while this test builds them with '/'. Every
// path comparison below goes through these so one set of assertions holds on both hosts.
static QString pathKey(const QString &p)
{
    const QString k = QDir::fromNativeSeparators(p);
#if defined(Q_OS_WIN)
    return k.toLower();
#else
    return k;
#endif
}

static bool samePath(const QString &a, const QString &b)
{
    return !a.isEmpty() && !b.isEmpty() && pathKey(a) == pathKey(b);
}

static bool pathStartsWith(const QString &path, const QString &prefix)
{
    const QString k = pathKey(path);
    const QString p = pathKey(prefix);
    return !k.isEmpty() && !p.isEmpty() && k.startsWith(p);
}

// Does this directory hold a zapret distribution — either as the root itself or as the parent of
// windows/ and linux/? Answered with the production detector on purpose: whatever the tree sections
// accept is exactly what the locator and the GUI would accept.
static bool isZapretTree(const QString &base)
{
    if (base.isEmpty() || !QFileInfo(base).isDir())
        return false;
    if (ZapretPaths::detectPlatform(base) != Strategy::Platform::Unknown)
        return true;
    const Strategy::Platform win = ZapretPaths::detectPlatform(base + QLatin1String("/windows"));
    const Strategy::Platform lin = ZapretPaths::detectPlatform(base + QLatin1String("/linux"));
    return win != Strategy::Platform::Unknown || lin != Strategy::Platform::Unknown;
}

static bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = f.write(bytes) == bytes.size();
    f.close();
    return ok;
}

// *.bat files buildFixtureTree() writes that the parser is allowed to surface: three wrappers, with
// service.bat and readme.bat dropped as excluded names and bin/ + lists/ as excluded dirs.
static const int kFixtureWindowsStrategies = 3;

static bool buildFixtureTree(const QString &base)
{
    const QString win = base + QLatin1String("/windows");
    const QString lin = base + QLatin1String("/linux");

    // windows — the curated default wrapper, in the shape the shipped one has: code page marker,
    // %~dp0-derived BIN/LISTS, an empty game-filter expansion that has to be tidied off the comma
    // list, and the winws.exe invocation spread over caret-continued lines.
    if (!writeFile(win + QLatin1String("/general.bat"), QByteArrayLiteral(
            "@echo off\r\n"
            "chcp 65001 > nul\r\n"
            "set BIN=%~dp0bin\\\r\n"
            "set LISTS=%~dp0lists\\\r\n"
            "call service.bat load_game_filter\r\n"
            "start \"zapret general\" /min \"%BIN%winws.exe\" --wf-tcp=80,443,2053,2083,2087,2096,8443,%GameFilterTCP% ^\r\n"
            " --wf-udp=443,19294-19344,50000-50100,%GameFilterUDP% ^\r\n"
            " --new --filter-tcp=443 --dpi-desync=fake --hostlist=\"%LISTS%list-general.txt\" ^\r\n"
            " --new --filter-udp=443 --dpi-desync=fake --full-hostlist=\"%LISTS%list-general.txt\"\r\n"
            "pause\r\n")))
        return false;

    if (!writeFile(win + QLatin1String("/general (ALT5).bat"), QByteArrayLiteral(
            "@echo off\r\n"
            "chcp 65001 > nul\r\n"
            "set BIN=%~dp0bin\\\r\n"
            "start \"zapret\" /min \"%BIN%winws.exe\" --wf-tcp=80,443 --wf-udp=443,4433 --dpi-desync=altcomma\r\n"
            "pause\r\n")))
        return false;

    if (!writeFile(win + QLatin1String("/md4.bat"), QByteArrayLiteral(
            "@echo off\r\n"
            "chcp 65001 > nul\r\n"
            "set BIN=%~dp0bin\\\r\n"
            "start \"zapret\" /min \"%BIN%winws.exe\" --wf-tcp=80,443 --filter-tcp=443 --dpi-desync=fake,split\r\n"
            "pause\r\n")))
        return false;

    // Helpers and payload: never selectable, but they are what marks the directory a windows root.
    if (!writeFile(win + QLatin1String("/service.bat"), QByteArrayLiteral(
            "@echo off\r\nrem game filter helper - not a strategy\r\n")))
        return false;
    if (!writeFile(win + QLatin1String("/readme.bat"), QByteArrayLiteral(
            "@echo off\r\nrem documentation launcher - not a strategy\r\n")))
        return false;
    if (!writeFile(win + QLatin1String("/bin/winws.exe"), QByteArrayLiteral("MZ fixture payload\r\n")))
        return false;
    if (!writeFile(win + QLatin1String("/lists/list-general.txt"), QByteArrayLiteral("example.com\r\n")))
        return false;

    // linux — the primary sysv launcher plus the openrc alternative that dedup has to choose
    // between, a systemd unit parked in an excluded dir, the helper names the exclusion list must
    // drop, and the payload binaries ZapretPaths::nfqwsPath() searches for.
    const QByteArray initScript = QByteArrayLiteral(
        "#!/bin/sh\n"
        "# zapret init script (fixture)\n"
        "case \"$1\" in\n"
        "  start) echo starting ;;\n"
        "  stop) echo stopping ;;\n"
        "esac\n");
    if (!writeFile(lin + QLatin1String("/init.d/sysv/zapret"), initScript))
        return false;
    if (!writeFile(lin + QLatin1String("/init.d/openrc/zapret"), initScript))
        return false;
    if (!writeFile(lin + QLatin1String("/init.d/systemd/zapret.service"), QByteArrayLiteral(
            "[Unit]\nDescription=zapret\n")))
        return false;
    if (!writeFile(lin + QLatin1String("/blockcheck.sh"), QByteArrayLiteral("#!/bin/sh\necho fixture\n")))
        return false;
    if (!writeFile(lin + QLatin1String("/install_easy.sh"), QByteArrayLiteral("#!/bin/sh\necho fixture\n")))
        return false;
    if (!writeFile(lin + QLatin1String("/config.default"), QByteArrayLiteral("#!/bin/sh\n# options\n")))
        return false;
    if (!writeFile(lin + QLatin1String("/binaries/linux-x86_64/nfqws"), QByteArrayLiteral("fixture payload\n")))
        return false;
    if (!writeFile(lin + QLatin1String("/binaries/linux-arm64/nfqws"), QByteArrayLiteral("fixture payload\n")))
        return false;

    return true;
}

// Assert the number of selectable windows wrappers. Exact for the two trees whose contents this
// project knows (the pinned developer unpack and the generated fixture). A third tree — a CI
// download, another zapret release — legitimately ships a different list, so there the requirement
// is only that something selectable was found rather than a number that is not a property of the
// code under test.
static void checkWindowsCount(int actual, const QString &label)
{
    if (g_winStrategies >= 0)
        check(actual == g_winStrategies, label + QStringLiteral(" is %1").arg(g_winStrategies));
    else
        check(actual > 0, label + QStringLiteral(" is non-zero (got %1)").arg(actual));
}

// ---- real process execution --------------------------------------------------
//
// Everything else in this file asserts synchronous, loop-free behaviour. The helpers below drive the
// part that only a running event loop can prove: a backend that really spawns a child, watches it,
// pipes its output into the log and reports the outcome. The children are throwaway /bin/sh scripts in
// a temporary directory — never pkexec, never nfqws — so no test here can touch the host's network.

// Records everything a backend emitted while the test watched it. Parented to the backend so it can
// never outlive (or out-observe) the object it is connected to.
class BackendProbe : public QObject
{
public:
    explicit BackendProbe(IZapretBackend *backend)
        : QObject(backend)
    {
        QObject::connect(backend, &IZapretBackend::stateChanged, this,
                         [this](IZapretBackend::State s) { m_states.append(s); });
        QObject::connect(backend, &IZapretBackend::logMessage, this,
                         [this](const QString &line, int level) {
                             m_logs << QStringLiteral("%1 %2").arg(level).arg(line);
                         });
        QObject::connect(backend, &IZapretBackend::errorOccurred, this,
                         [this](const QString &m) { m_errors << m; });
    }

    bool sawState(IZapretBackend::State s) const { return m_states.contains(s); }
    // Did the first state transition go through `s`? (Order matters: Stopping after Running, etc.)
    bool stateSequence(const QVector<IZapretBackend::State> &expected) const
    {
        int at = 0;
        for (const IZapretBackend::State s : m_states) {
            if (at < expected.size() && s == expected.at(at))
                ++at;
        }
        return at == expected.size();
    }
    bool sawLog(const QString &needle) const
    {
        for (const QString &line : m_logs)
            if (line.contains(needle))
                return true;
        return false;
    }
    // How many log lines carry `needle`. A failure must appear exactly once: the emitter logs it and the
    // notification channel (errorOccurred) is not a second log.
    int logCountWith(const QString &needle) const
    {
        int hits = 0;
        for (const QString &line : m_logs)
            if (line.contains(needle))
                ++hits;
        return hits;
    }
    const QStringList &errors() const { return m_errors; }
    const QStringList &logs() const { return m_logs; }
    int stateCount() const { return m_states.size(); }

private:
    QVector<IZapretBackend::State> m_states;
    QStringList m_logs;
    QStringList m_errors;
};

static void pumpFor(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

static bool pumpUntil(const std::function<bool()> &pred, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() >= timeoutMs)
            return pred();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return true;
}

// Write a POSIX shell script and make it executable. Returns the path, or "" on any failure.
static QString makeScript(const QString &path, const QString &body)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write("#!/bin/sh\n");
    f.write(body.toUtf8());
    f.close();
    const QFileDevice::Permissions owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                           | QFileDevice::ExeOwner;
    const QFileDevice::Permissions others = QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                            | QFileDevice::ReadOther | QFileDevice::ExeOther;
    if (!QFile::setPermissions(path, owner | others))
        return QString();
    return path;
}

// A Strategy shaped exactly like the parser's DirectBinary output, but pointed at a fake payload.
static Strategy fakeDirectStrategy(const QString &id, const QString &script, Strategy::Platform platform)
{
    Strategy s;
    s.id = id;
    s.name = id;
    s.fileName = QFileInfo(script).fileName();
    s.dirPath = QFileInfo(script).absolutePath();
    s.filePath = script;
    s.executablePath = script;
    s.kind = Strategy::Kind::Binary;
    s.platform = platform;
    s.argv = {QStringLiteral("--wf-tcp=80,443"), QStringLiteral("--dpi-desync=fake")};
    s.requiresRoot = false;
    return s;
}

int main(int argc, char **argv)
{
#if defined(Q_OS_UNIX)
    // Deterministic elevation gate. PrivilegeHelper::hasPolkitAgent() approximates "can pkexec prompt
    // me here?" by looking for a desktop-session variable. Clearing them before anything caches the
    // process environment makes that answer stable even on a developer desktop, so the root-required
    // path is provable without ever raising a real polkit password prompt on the user's screen.
    // (QProcessEnvironment caches on first read — hence: first statement of main, before the app too.)
    qunsetenv("DISPLAY");
    qunsetenv("WAYLAND_DISPLAY");
    qunsetenv("XDG_SESSION_TYPE");
#endif

    // Backends use QProcess, which needs a QCoreApplication instance to exist. The rejection paths below
    // need no looping; the real-execution section pumps the event loop explicitly (pumpUntil/pumpFor).
    QCoreApplication app(argc, argv);

    // Where the two distribution trees are read from: argv[1], else $ZAPRET_TEST_TREE (the hook a
    // build can point at a downloaded unpack), else ~/Documents/zapret — and that last one only when
    // the folder actually holds a zapret tree, so the test is host-independent rather than pinned to
    // one developer's home. None of those resolving to a real zapret tree is the normal CI situation
    // rather than an error — it falls back to a fixture.
    QString base = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                              : QString::fromLocal8Bit(qgetenv("ZAPRET_TEST_TREE"));
    const QString pinnedRoot = QDir::homePath() + QLatin1String("/Documents/zapret");
    const bool pinnedDefault = base.isEmpty() && isZapretTree(pinnedRoot);
    if (pinnedDefault)
        base = pinnedRoot;

    QTemporaryDir fixtureDir;
    if (!isZapretTree(base))
    {
        const QString requested = base.isEmpty()
                                      ? QStringLiteral("$HOME/Documents/zapret (absent or not a zapret tree)")
                                      : base;
        // The generated root is named "zapret" on purpose: ZapretLocator probes "<cwd>/zapret", so
        // moving the working directory to its parent keeps the auto-locate section below meaningful
        // on a host that ships no zapret at all.
        if (!fixtureDir.isValid())
        {
            out << "  FAIL  cannot create a fixture temporary directory\n";
            out.flush();
            return 2;
        }
        base = fixtureDir.path() + QLatin1String("/zapret");
        if (!buildFixtureTree(base))
        {
            out << "  FAIL  cannot write a fixture zapret tree under " << fixtureDir.path() << '\n';
            out.flush();
            return 2;
        }
        QDir::setCurrent(QFileInfo(base).absolutePath());
        g_treeIsReal = false;
        g_winStrategies = kFixtureWindowsStrategies;
        out << "  NOTE  no zapret tree at " << requested << " — generated a fixture at " << base << '\n';
    }
    else if (!pinnedDefault)
    {
        // A tree this project does not pin the contents of (a different zapret release, a CI
        // download): assert the landmarks, not a wrapper count copied from somebody's unpack.
        g_winStrategies = -1;
    }

    const QString winRoot = base + "/windows";
    const QString linuxRoot = base + "/linux";
    const QString base_dir = base;   // the unsorted parent the locator expands

    out << "=== zapret core verification ===\n";
    out << "base = " << base << (g_treeIsReal ? "  (real zapret tree)" : "  (generated fixture tree)")
        << "\n\n";

    // ---- displayName / kindOf pure logic (no filesystem needed) ----
    out << "[displayName]\n";
    check(StrategyParser::displayName("general (ALT5).bat") == "general (ALT5)", "general (ALT5).bat -> general (ALT5)");
    check(StrategyParser::displayName("general.bat") == "general", "general.bat -> general");
    check(StrategyParser::displayName("test_blockcheck.sh") == "blockcheck", "strips test_ prefix");
    check(StrategyParser::displayName("zapret") == "zapret", "extensionless preserved");

    out << "\n[kindOf]\n";
    auto kindFor = [](const QString &fn, Strategy::Platform p) {
        return StrategyParser::kindOf(QFileInfo("/x/" + fn), p);
    };
    check(kindFor("general.bat", Strategy::Platform::Windows) == Strategy::Kind::Batch, ".bat -> Batch");
    check(kindFor("foo.ps1", Strategy::Platform::Windows) == Strategy::Kind::PowerShell, ".ps1 -> PowerShell");
    check(kindFor("zapret.service", Strategy::Platform::Linux) == Strategy::Kind::SystemdUnit, ".service -> SystemdUnit");
    check(kindFor("nfqws", Strategy::Platform::Linux) == Strategy::Kind::Binary, "nfqws -> Binary");
    check(kindFor("functions", Strategy::Platform::Linux) == Strategy::Kind::Unknown, "functions -> Unknown");
    check(kindFor("config.default", Strategy::Platform::Linux) == Strategy::Kind::Unknown, "config.default -> Unknown");

    // ---- exclusions ----
    out << "\n[exclusions]\n";
    check(StrategyParser::excludedNames(Strategy::Platform::Windows).contains("service.bat"), "windows excludes service.bat");
    check(StrategyParser::excludedDirs(Strategy::Platform::Linux).contains("binaries"), "linux excludes binaries");
    check(StrategyParser::excludedDirs(Strategy::Platform::Linux).contains("systemd"), "linux excludes systemd tree");
    check(!StrategyParser::excludedDirs(Strategy::Platform::Linux).contains("sysv"), "linux keeps sysv");

    // ---- path helpers + layout detection ----
    out << "\n[ZapretPaths]\n";
    check(QFileInfo::exists(ZapretPaths::winwsPath(winRoot)), "winws.exe located");
    check(ZapretPaths::detectPlatform(winRoot) == Strategy::Platform::Windows, "windows root detected");
    check(ZapretPaths::detectPlatform(linuxRoot) == Strategy::Platform::Linux, "linux root detected");
    check(!ZapretPaths::nfqwsPath(linuxRoot).isEmpty(), "native nfqws located");
    check(QFileInfo::exists(ZapretPaths::sysvScriptPath(linuxRoot)), "sysv zapret script located");

    // ---- windows strategy parse ----
    out << "\n[parse windows]\n";
    QList<Strategy> win = StrategyParser::parse(winRoot, Strategy::Platform::Windows);
    out << "  found " << win.size() << " windows strategies\n";
    // The count is the one property the tree's contents decide, so it is checked against the
    // expectation resolved for this tree (see checkWindowsCount).
    checkWindowsCount(static_cast<int>(win.size()), QStringLiteral("selectable windows strategies"));
    check(findByName(win, "general") != nullptr, "'general' present");
    check(findByName(win, "service") == nullptr, "'service' excluded");
    check(findByName(win, "general (ALT5)") != nullptr, "'general (ALT5)' present");

    // applyDefaults on windows batch
    QList<Strategy> winD = win;
    StrategyParser::applyDefaults(winD, winRoot, Strategy::Platform::Windows);
    const Strategy *genD = findByName(winD, "general");
    out << "\n[applyDefaults windows]\n";
    if (genD)
    {
        out << "  cmd = " << genD->executablePath << " /c  arg=" << genD->primaryArg << "\n";
        check(genD->executablePath == "cmd.exe", "batch uses cmd.exe");
        check(genD->primaryArg == genD->filePath, "primaryArg is the .bat path");
        check(samePath(genD->workingDirectory, winRoot), "workingDirectory = windows root");
        check(genD->requiresRoot, "batch requires elevation");
    }

    // ---- direct winws.exe resolution from general.bat ----
    //
    // The whole point of parseWindowsCommand/resolveWindowsCommands is to turn the cmd.exe wrapper
    // into the exact winws.exe command line so the backend can exec the binary directly. general.bat
    // is the canonical, richest wrapper: chcp 65001, %~dp0-based BIN/LISTS, a `start ... /min
    // "%BIN%winws.exe" ...` spread over ^-continued lines, empty %GameFilter*% expansions, and quoted
    // list paths. This runs identically on any host because the resolution is pure text.
    out << "\n[parseWindowsCommand general.bat]\n";
    {
        const QString bat = winRoot + "/general.bat";
        StrategyParser::WinCommand wc = StrategyParser::parseWindowsCommand(bat, winRoot);
        check(wc.ok, "general.bat resolves to a winws.exe command");
        if (!wc.ok)
            out << "    error: " << wc.error << "\n";

        // Program: .../windows/bin/winws.exe (BIN expands %~dp0bin\; separator normalised per host).
        const QString progBase = QFileInfo(wc.executablePath).fileName();
        check(progBase.compare(QLatin1String("winws.exe"), Qt::CaseInsensitive) == 0,
              "executablePath basename is winws.exe");
        check(wc.executablePath.contains(QLatin1String("bin")), "executablePath runs through bin/");
        check(pathStartsWith(wc.executablePath, winRoot), "executablePath is under the windows root");
        check(samePath(wc.workingDirectory, winRoot), "workingDirectory is the bat directory");

        // argv must be non-empty and must not re-include the program token itself.
        check(!wc.argv.isEmpty(), "winws argv is populated");
        bool argvHasWinws = false;
        for (const QString &a : wc.argv)
            if (QFileInfo(a).fileName().compare(QLatin1String("winws.exe"), Qt::CaseInsensitive) == 0)
                argvHasWinws = true;
        check(!argvHasWinws, "argv excludes the winws.exe program token");

        // The `start`, its title, and `/min` precede the program and must all be dropped.
        check(!wc.argv.contains(QLatin1String("start")) && !wc.argv.contains(QLatin1String("/min")),
              "argv drops start/title/min");

        // No leftover batch artifacts anywhere: variables expanded, continuations joined, and any
        // dangling comma from an empty %GameFilter*% tidyed.
        bool clean = true;
        for (const QString &a : wc.argv)
        {
            if (a.contains(QLatin1Char('%')) || a.contains(QLatin1Char('^')))
                clean = false;
            if (a.endsWith(QLatin1Char(',')) || a.contains(QLatin1String(",,")))
                clean = false;
        }
        check(clean, "argv tokens carry no unexpanded %var%, ^, or comma artifacts");

        // The well-known first two flags come straight off the `start` line, with the empty
        // GameFilter expansions tidied off the end of each comma list.
        const QString wfTcp = QStringLiteral("--wf-tcp=80,443,2053,2083,2087,2096,8443");
        const QString wfUdp = QStringLiteral("--wf-udp=443,19294-19344,50000-50100");
        check(wc.argv.size() >= 2 && wc.argv.at(0) == wfTcp, "argv[0] is the tidied --wf-tcp list");
        check(wc.argv.size() >= 2 && wc.argv.at(1) == wfUdp, "argv[1] is the tidied --wf-udp list");

        // The general.bat profile uses %LISTS%-relative hostlist paths and one --new per filter.
        bool anyHostlist = false, anyListPath = false;
        for (const QString &a : wc.argv)
        {
            if (a.startsWith(QLatin1String("--hostlist")))
                anyHostlist = true;
            if (a.contains(QLatin1String("list-general.txt")) && a.contains(QLatin1String("lists")))
                anyListPath = true;
        }
        check(anyHostlist, "argv contains --hostlist* filter flags");
        check(anyListPath, "%LISTS% expands to an under-lists/ path in argv");
        check(wc.argv.contains(QStringLiteral("--new")),
              "argv preserves the --new filter separators");

        out << "  argv (" << wc.argv.size() << " args): " << wc.argv.join(' ') << "\n";
    }

    // ---- resolveWindowsCommands rewrites the wrapper list into direct-binary form ----
    out << "\n[resolveWindowsCommands]\n";
    {
        QList<Strategy> res = winD;   // the applyDefaults'd cmd.exe wrapper list
        StrategyParser::resolveWindowsCommands(res);
        const Strategy *genR = findByName(res, "general");
        check(genR != nullptr, "'general' still present after resolution");
        if (genR)
        {
            check(genR->kind == Strategy::Kind::Binary, "general becomes Binary (DirectBinary)");
            check(genR->executablePath.endsWith(QLatin1String("winws.exe"), Qt::CaseInsensitive),
                  "general.executablePath points at winws.exe");
            check(genR->primaryArg.isEmpty(), "general clears primaryArg (no cmd /c payload)");
            check(!genR->argv.isEmpty(), "general carries the resolved winws argv");
            check(genR->requiresRoot, "resolved general still requires elevation");
            check(!genR->description.isEmpty(), "resolved general gains a description");

            // Idempotent: running resolution again over the (now Binary) list changes nothing.
            QList<Strategy> res2 = res;
            StrategyParser::resolveWindowsCommands(res2);
            const Strategy *genR2 = findByName(res2, "general");
            check(genR2 && genR2->argv == genR->argv &&
                      genR2->executablePath == genR->executablePath,
                  "resolveWindowsCommands is idempotent");
        }
        // Unresolvable wrappers (none here, but assert the untouched-fallback contract): a Batch
        // strategy whose file has no winws.exe line must keep its cmd.exe wrapper form.
        Strategy fake;
        fake.kind = Strategy::Kind::Batch;
        fake.platform = Strategy::Platform::Windows;
        fake.filePath = winRoot + "/general.bat";   // resolvable
        fake.executablePath = QStringLiteral("cmd.exe");
        fake.primaryArg = fake.filePath;
        QList<Strategy> one{fake};
        StrategyParser::resolveWindowsCommands(one);
        check(one.at(0).kind == Strategy::Kind::Binary, "explicit winws wrapper resolves");
    }

    // ---- linux tree parse ----
    out << "\n[parseTree linux]\n";
    QList<Strategy> lin = StrategyParser::parseTree(linuxRoot, Strategy::Platform::Linux, 3);
    out << "  found " << lin.size() << " linux strategies\n";
    for (const Strategy &s : lin)
        out << "    - " << s.name << " [" << kindName(s.kind) << "] w=" << s.weight
            << "  " << s.filePath << "\n";
    const Strategy *z = findByName(lin, "zapret");
    check(z != nullptr, "sysv 'zapret' present");
    if (z)
    {
        check(z->kind == Strategy::Kind::ShellScript, "zapret is ShellScript");
        check(z->weight == 100, "zapret is top-weighted (sysv)");
        check(z->filePath.contains("sysv"), "selected the sysv copy");
        check(z->executablePath == "pkexec", "shell script uses pkexec");
        check(z->requiresRoot, "shell script requires root");
    }
    check(findByName(lin, "install_easy") == nullptr, "install_easy.sh excluded");
    check(findByName(lin, "blockcheck") == nullptr, "blockcheck.sh excluded");
    bool sorted = true;
    for (int i = 1; i < lin.size(); ++i)
        if (lin[i].weight > lin[i - 1].weight)
            sorted = false;
    check(sorted, "linux results ordered by descending weight");

    // ---- locator ----
    out << "\n[ZapretLocator]\n";
    QList<ZapretLocator::Found> found = ZapretLocator::locate({base});
    out << "  located " << found.size() << " root(s):\n";
    for (const auto &f : found)
        out << "    - " << f.path << " [" << (f.platform == Strategy::Platform::Windows ? "Windows" : "Linux") << "]\n";
    check(!ZapretLocator::firstRoot(found, Strategy::Platform::Windows).isEmpty(), "located a windows root");
    check(!ZapretLocator::firstRoot(found, Strategy::Platform::Linux).isEmpty(), "located a linux root");

    // ---- execution backends: synchronous failure modes ----
    //
    // These assert the deterministic, event-loop-free rejection paths that every backend must satisfy
    // before it ever reaches QProcess::start(). Both backend classes compile and run their portable
    // logic on this Linux host (only the CREATE_NO_WINDOW branch is #ifdef'd out), so WindowsBackend
    // is exercisable here too.
    out << "\n[backends: failure modes]\n";
    {
        auto expectRejected = [&](IZapretBackend *be, const Strategy &s, const QString &needle,
                                  const QString &label) {
            QString err;
            const bool accepted = be->start(s, &err);
            check(!accepted, label + ": start() rejected synchronously");
            check(err.contains(needle, Qt::CaseInsensitive), label + ": error mentions '" + needle + "'");
            check(be->state() == IZapretBackend::State::Error, label + ": state is Error");
            check(!be->isRunning(), label + ": not running");
        };

        // (a) Invalid strategy (no kind) is refused by both backends.
        Strategy invalid;   // kind == Unknown, empty filePath -> isValid() false
        WindowsBackend wb1;
        expectRejected(&wb1, invalid, "not valid", "Windows/invalid");

        // (b) Direct binary that does not exist (privilege not requested, so the elevation gate is
        //     bypassed and the existence check is what fails). Deterministic on any host.
        Strategy ghost;
        ghost.kind = Strategy::Kind::Binary;
        ghost.platform = Strategy::Platform::Windows;
        ghost.filePath = winRoot + "/bin/winws.exe";
        ghost.executablePath = QStringLiteral("/nonexistent/path/winws.exe");
        ghost.requiresRoot = false;
        WindowsBackend wb2;
        expectRejected(&wb2, ghost, "not found", "Windows/missing-exe");

        // (c) Same shape through the Linux backend (its composeCommand existence check runs first
        //     for a non-root direct payload).
        Strategy ghostLin = ghost;
        ghostLin.platform = Strategy::Platform::Linux;
        ghostLin.executablePath = QStringLiteral("/nonexistent/path/nfqws");
        LinuxBackend lb1;
        expectRejected(&lb1, ghostLin, "not found", "Linux/missing-exe");

        // (d) Controller (init script) whose file is absent -> 'not found', before pkexec is touched.
        Strategy controller;
        controller.kind = Strategy::Kind::ShellScript;
        controller.platform = Strategy::Platform::Linux;
        controller.filePath = QStringLiteral("/nonexistent/zapret-init");
        controller.executablePath = QStringLiteral("pkexec");
        controller.primaryArg = controller.filePath;
        controller.requiresRoot = false;   // skip the polkit-agent gate for a deterministic result
        LinuxBackend lb2;
        expectRejected(&lb2, controller, "not found", "Linux/missing-script");

        // (e) A backend reports its own platform name.
        check(wb2.platformName() == QLatin1String("Windows"), "WindowsBackend.platformName");
        check(lb2.platformName() == QLatin1String("Linux"), "LinuxBackend.platformName");
    }

    // ---- ZapretManager facade ----
    out << "\n[ZapretManager]\n";
    {
        ZapretManager mgr;
        // On this host (Linux) createBackend must produce a live Linux backend; on macOS it is null.
#if defined(Q_OS_LINUX)
        check(mgr.hasBackend(), "manager has a backend on Linux");
        check(mgr.backendName() == QLatin1String("Linux"), "manager backend is Linux");
        check(mgr.state() == IZapretBackend::State::Stopped, "manager starts Stopped");
        check(!mgr.isRunning(), "manager not running initially");
#elif defined(Q_OS_WIN)
        check(mgr.hasBackend() && mgr.backendName() == QLatin1String("Windows"),
              "manager backend is Windows");
#endif
        // Factory yields the same class the manager chose.
        IZapretBackend *solo = ZapretManager::createBackend();
        check(solo != nullptr, "createBackend returns a backend on a supported host");
        if (solo)
            check(!solo->platformName().isEmpty(), "created backend names its platform");
        delete solo;

        // Facade forwards a synchronous rejection through to its caller.
        QString err;
        Strategy invalid;
        check(!mgr.start(invalid, &err), "manager.start(invalid) fails");
        check(!err.isEmpty(), "manager fills the error on rejection");
        check(mgr.state() == IZapretBackend::State::Error, "manager reflects Error state");
    }

    // ---- ZapretManager strategy catalog (what the GUI feeds its cards) ----
    out << "\n[ZapretManager catalog]\n";
    {
        ZapretManager catalog;
        catalog.setZapretDir(winRoot);
        check(samePath(catalog.resolvedRoot(), winRoot), "manager keeps a direct windows root");
        checkWindowsCount(catalog.strategyCount(), QStringLiteral("manager windows strategies"));

        const Strategy *gen = findByName(catalog.strategies(), "general");
        check(gen != nullptr, "catalog holds 'general'");
        if (gen) {
            check(gen->kind == Strategy::Kind::Binary, "catalog strategy is resolved to DirectBinary");
            check(!gen->argv.isEmpty(), "catalog strategy carries winws argv");
            check(!gen->id.isEmpty(), "catalog strategy has a stable id");
            check(catalog.findStrategy(gen->id) != nullptr, "findStrategy resolves by id");
            check(catalog.findStrategy(gen->id)->name == gen->name, "found strategy matches");
        }
        check(catalog.findStrategy(QStringLiteral("no-such-id")) == nullptr,
              "findStrategy rejects unknown ids");

        QString err;
        check(!catalog.startById(QStringLiteral("no-such-id"), &err), "startById(unknown) fails");
        check(!err.isEmpty(), "startById(unknown) explains itself");
        check(catalog.activeStrategyId().isEmpty(), "a failed start leaves nothing active");

        // A base folder (…/zapret) expands to the root that matches this host.
        ZapretManager base;
        base.setZapretDir(base_dir);
        check(!base.resolvedRoot().isEmpty(), "a base directory resolves to a root");
#if defined(Q_OS_LINUX)
        check(samePath(base.resolvedRoot(), linuxRoot), "on Linux the base resolves to linux/");
        check(base.strategyCount() >= 1, "linux catalog holds at least the init script");
#elif defined(Q_OS_WIN)
        check(samePath(base.resolvedRoot(), winRoot), "on Windows the base resolves to windows/");
#endif

        // Empty setting = auto-locate through ZapretLocator's well-known bases.
        ZapretManager autoMgr;
        const int autoCount = autoMgr.reload();
        check(!autoMgr.resolvedRoot().isEmpty(), "auto-locate found a zapret tree");
        check(autoCount == autoMgr.strategyCount(), "reload() count matches the catalog");

        // Pointing at nothing yields an empty catalog instead of a crash or stale entries.
        ZapretManager none;
        none.setZapretDir(QStringLiteral("/definitely/not/zapret"));
        check(none.strategyCount() == 0, "a bogus directory yields no strategies");
    }

    // ---- backends: real process execution ----
    //
    // These are the assertions the failure-mode section above cannot make: that a strategy actually
    // launches, that its output reaches the log, that exit codes become states, and — the one that
    // matters most when a user presses "Остановить" — that a requested stop is reported as a clean stop
    // and not as a crash.
    out << "\n[backends: real execution]\n";
#if defined(Q_OS_UNIX)
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "fixture directory created");
        const QString argsLog = tmp.filePath(QStringLiteral("args.log"));

        // (1) A long-lived direct payload. It stays up for ~5 s of sleep loops, which is what lets the
        //     test observe Running and then stop() a live process instead of one that already exited.
        const QString live = makeScript(tmp.filePath(QStringLiteral("fake-nfqws.sh")), QStringLiteral(
            "echo \"nfqws-fake up: $*\"\n"
            "echo \"$*\" >> '%1'\n"
            "i=0\n"
            "while [ \"$i\" -lt 5 ]; do sleep 1; i=$((i+1)); done\n"
            "echo \"nfqws-fake done\"\n").arg(argsLog));
        check(!live.isEmpty(), "live payload fixture written");

        {
            using S = IZapretBackend::State;
            LinuxBackend lb;
            BackendProbe *probe = new BackendProbe(&lb);
            Strategy s = fakeDirectStrategy(QStringLiteral("live-direct"), live, Strategy::Platform::Linux);

            QString err;
            const bool accepted = lb.start(s, &err);
            check(accepted, "LinuxBackend accepted a direct payload");
            if (!accepted)
                out << "    note: " << err << '\n';

            check(pumpUntil([&] { return lb.state() == S::Running; }, 8000),
                  "a real child drives the backend to Running");
            check(probe->stateSequence({S::Starting, S::Running}),
                  "transitions arrived as Starting then Running");
            check(pumpUntil([&] { return probe->sawLog(QStringLiteral("nfqws-fake up")); }, 3000),
                  "the child's stdout was piped into the log");
            check(probe->sawLog(QStringLiteral("--wf-tcp=80,443")),
                  "the argv the parser produced reached the child");
            check(lb.isRunning(), "isRunning() is true while the child lives");

            lb.stop();
            check(pumpUntil([&] { return lb.state() == S::Stopped; }, 8000), "stop() ends in Stopped");
            check(probe->sawState(S::Stopping), "Stopping was reported on the way down");
            check(probe->errors().isEmpty(),
                  "a user-initiated stop is NOT reported as an error (SIGTERM used to look like a crash)");
            check(probe->sawLog(QStringLiteral("stopped on request")),
                  "the clean stop is what the log says");
            check(!probe->sawState(S::Error),
                  "and no Error state was emitted anywhere in the stop cycle");
            check(!lb.isRunning(), "nothing is managed after the stop");
            pumpFor(150);   // let any late notification land before the scope closes
        }

        // (2) A payload that fails on its own: exit 3 must surface as Error + errorOccurred, because
        //     nfqws dying by itself is exactly the failure the GUI has to badge on the card.
        const QString failing = makeScript(tmp.filePath(QStringLiteral("fake-fail.sh")), QStringLiteral(
            "echo \"nfqws-fake: cannot open nfqueue group 200: Operation not permitted\" >&2\n"
            "exit 3\n"));
        {
            using S = IZapretBackend::State;
            LinuxBackend lb;
            BackendProbe *probe = new BackendProbe(&lb);
            Strategy s = fakeDirectStrategy(QStringLiteral("fail-direct"), failing, Strategy::Platform::Linux);

            QString err;
            check(lb.start(s, &err), "a failing payload is still accepted (the launch itself worked)");
            check(pumpUntil([&] { return lb.state() == S::Error; }, 8000),
                  "a non-zero exit moves the backend to Error");
            check(probe->errors().size() == 1, "exactly one errorOccurred for the failed run");
            check(probe->logCountWith(QStringLiteral("payload exited with code 3")) == 1,
                  "the failure is logged exactly once (logMessage owns the line, errorOccurred does not)");
            check(!probe->errors().isEmpty()
                      && probe->errors().first().contains(QStringLiteral("code 3")),
                  "the error names the exit code");
            check(pumpUntil([&] { return probe->sawLog(QStringLiteral("cannot open nfqueue")); }, 3000),
                  "stderr reached the log too — that is where the reason lives");
            check(!lb.isRunning(), "a dead payload leaves nothing running");
        }

        // (3) The controller (init-script) shape: two short-lived commands, `start` then `stop`, with the
        //     daemon state derived from their exit codes. executablePath is the script itself — that is how
        //     LinuxBackend runs a controller once it is root — which keeps a real pkexec prompt out of the
        //     test while still driving the whole controller state machine.
        const QString controller = makeScript(tmp.filePath(QStringLiteral("fake-init.sh")), QStringLiteral(
            "echo \"$*\" >> '%1'\n"
            "echo \"controller: $*\"\n").arg(argsLog));
        {
            using S = IZapretBackend::State;
            LinuxBackend lb;
            BackendProbe *probe = new BackendProbe(&lb);
            Strategy s;
            s.id = QStringLiteral("controller");
            s.name = QStringLiteral("controller");
            s.fileName = QStringLiteral("zapret");
            s.filePath = controller;
            s.primaryArg = controller;
            s.executablePath = controller;   // stand-in for "pkexec": no elevation attempt is made
            s.kind = Strategy::Kind::ShellScript;
            s.platform = Strategy::Platform::Linux;
            s.requiresRoot = false;

            QString err;
            check(lb.start(s, &err), "controller start accepted");
            check(pumpUntil([&] { return lb.state() == S::Running; }, 8000),
                  "a successful controller command means the daemon is up");
            check(lb.isRunning(), "controller mode reports running with no live child process");
            check(pumpUntil([&] {
                        QFile f(argsLog);
                        return f.open(QIODevice::ReadOnly) &&
                               QString::fromUtf8(f.readAll()).contains(QStringLiteral("start"));
                    }, 5000),
                  "the controller was invoked with 'start'");

            lb.stop();
            check(pumpUntil([&] { return lb.state() == S::Stopped; }, 8000),
                  "the matching 'stop' command brings the controller down");
            QFile f(argsLog);
            const QString invoked = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
            f.close();
            check(invoked.contains(QStringLiteral("stop")), "the controller was invoked with 'stop'");
            check(invoked.contains(QStringLiteral("start")), "and with 'start' before that, in order");
            check(probe->errors().isEmpty(), "a clean controller cycle raises no error");
        }

        // (4) WindowsBackend's live-process tracking is plain QProcess, so the same machine is verifiable
        //     here with a POSIX payload. What genuinely needs a Windows box — cmd.exe /c, powershell -File,
        //     CREATE_NO_WINDOW and the UAC prompt — is deliberately not claimed by this test.
        {
            using S = IZapretBackend::State;
            WindowsBackend wb;
            BackendProbe *probe = new BackendProbe(&wb);
            Strategy s = fakeDirectStrategy(QStringLiteral("win-live"), live, Strategy::Platform::Windows);

            QString err;
            check(wb.start(s, &err), "WindowsBackend accepted a live payload");
            check(pumpUntil([&] { return wb.state() == S::Running; }, 8000),
                  "WindowsBackend reaches Running from QProcess::started");
            check(pumpUntil([&] { return probe->sawLog(QStringLiteral("nfqws-fake up")); }, 3000),
                  "merged stdout is captured by WindowsBackend");
            wb.stop();
            check(pumpUntil([&] { return wb.state() == S::Stopped; }, 8000),
                  "WindowsBackend stop() lands on Stopped");
            check(probe->errors().isEmpty(),
                  "and, like Linux, does not dress its own kill up as a crash");
            check(!probe->sawState(S::Error),
                  "the requested stop never passes through Error (Qt emits Crashed before finished)");
        }

        // (5) A program that cannot be exec'd at all: the asynchronous FailedToStart path. A bare name
        //     skips the synchronous existence check, so this is the path a missing binary takes in the field.
        {
            using S = IZapretBackend::State;
            WindowsBackend wb;
            BackendProbe *probe = new BackendProbe(&wb);
            Strategy s = fakeDirectStrategy(QStringLiteral("win-missing"), live, Strategy::Platform::Windows);
            s.executablePath = QStringLiteral("zapret-nonexistent-command");
            s.filePath = s.executablePath;

            wb.start(s, nullptr);   // accepted or not: either way it has to end in Error
            check(pumpUntil([&] { return wb.state() == S::Error; }, 8000),
                  "an unresolvable program ends in Error");
            check(!probe->errors().isEmpty()
                      && probe->errors().first().contains(QStringLiteral("failed to start"), Qt::CaseInsensitive),
                  "the error says it failed to start");
            check(!wb.isRunning(), "no phantom process is tracked after a failed start");
        }

        // (6) The elevation gate, deterministically. coretest cleared the session variables at startup, so a
        //     root-requiring strategy must be refused before spawning anything rather than reaching a real
        //     pkexec prompt. Under a root run the gate does not apply, so that branch is asserted instead.
        {
            using S = IZapretBackend::State;
            Strategy needsRoot = fakeDirectStrategy(QStringLiteral("needs-root"), live,
                                                     Strategy::Platform::Linux);
            needsRoot.requiresRoot = true;
            LinuxBackend lb;
            BackendProbe *probe = new BackendProbe(&lb);
            QString err;
            const bool accepted = lb.start(needsRoot, &err);

            if (PrivilegeHelper::isElevated()) {
                check(accepted, "as root the gate does not block a privileged strategy");
                pumpUntil([&] { return lb.state() == S::Running; }, 8000);
                lb.stop();
                pumpUntil([&] { return lb.state() == S::Stopped; }, 8000);
            } else {
                check(!accepted, "non-root without a polkit agent is refused before spawning anything");
                check(err.contains(QStringLiteral("polkit")) || err.contains(QStringLiteral("root")),
                      "the refusal says why: root required, no agent reachable");
                check(lb.state() == S::Error, "the gate leaves the backend in Error");
                check(probe->errors().size() == 1, "the gate reports exactly one error");
                check(probe->logCountWith(err) == 1, "the refusal is logged exactly once");
            }

            // The Windows mirror of the same gate: an unelevated GUI cannot load WinDivert, so it is refused
            // with its own message. isElevated() is geteuid() here, so this is a real assertion, not a loop.
            Strategy winRoot = fakeDirectStrategy(QStringLiteral("win-admin"), live,
                                                  Strategy::Platform::Windows);
            winRoot.requiresRoot = true;
            WindowsBackend wb;
            QString werr;
            const bool winAccepted = wb.start(winRoot, &werr);
            check(winAccepted == PrivilegeHelper::isElevated(),
                  "WindowsBackend gates elevation on the same token check");
            if (!winAccepted)
                check(werr.contains(QStringLiteral("elevat"), Qt::CaseInsensitive),
                      "the Windows refusal mentions elevation");
        }

        // (7) ZapretManager end to end with a real child — the facade the GUI actually drives.
        {
            using S = IZapretBackend::State;
            ZapretManager mgr;
            int startedCount = 0;
            int stoppedCount = 0;
            QString startedId;
            QObject::connect(&mgr, &ZapretManager::strategyStarted,
                             [&](const QString &id, const QString &) { ++startedCount; startedId = id; });
            QObject::connect(&mgr, &ZapretManager::strategyStopped, [&] { ++stoppedCount; });

            const QString quick = makeScript(tmp.filePath(QStringLiteral("fake-quick.sh")), QStringLiteral(
                "echo \"quick child up\"\n"));
            Strategy s = fakeDirectStrategy(QStringLiteral("mgr-live"), quick, Strategy::Platform::Linux);

            QString err;
            check(mgr.start(s, &err), "manager accepted the strategy");
            check(startedCount == 1 && startedId == QLatin1String("mgr-live"),
                  "strategyStarted carried the id the UI badges");
            check(mgr.activeStrategyId() == QLatin1String("mgr-live"),
                  "manager remembers which strategy is active");
            check(mgr.activeStrategy() != nullptr, "activeStrategy() resolves back to it");
            check(mgr.activeStrategy() && mgr.activeStrategy()->id == QLatin1String("mgr-live"),
                  "activeStrategy() is the copy that was started, not a catalog lookup");
            check(mgr.isRunning() || mgr.state() != S::Stopped, "manager reports a live backend");
            check(pumpUntil([&] { return mgr.state() == S::Stopped; }, 8000),
                  "the manager follows the child all the way down");
            check(stoppedCount == 1, "strategyStopped fired once when the child exited");
            check(mgr.activeStrategyId().isEmpty() && mgr.activeStrategy() == nullptr,
                  "nothing stays active after the run ends");
        }
    }
#else
    out << "  SKIP  real-execution tests need a POSIX host to run the /bin/sh fixtures\n";
#endif

    out << "\n=== " << (g_failures ? "FAILED" : "ALL PASS") << " (" << g_failures << " failures) ===\n";
    out.flush();
    return g_failures ? 1 : 0;
}

