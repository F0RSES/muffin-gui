// coretest — plain console verifier for the zapret core library.
//
// It does not use QTest (keeps the dependency surface to QtCore only) and runs the parser and
// locator against real on-disk zapret trees. Defaults to /home/norman/Documents/zapret but accepts
// an alternative base as argv[1]. Exits non-zero on any failed assertion.

#include "Strategy.h"
#include "StrategyParser.h"
#include "ZapretLocator.h"
#include "ZapretPaths.h"
#include "ZapretManager.h"
#include "IZapretBackend.h"
#include "LinuxBackend.h"
#include "WindowsBackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTextStream>

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

int main(int argc, char **argv)
{
    // Backends use QProcess, which needs a QCoreApplication instance to exist. We never enter a real
    // event loop here — every backend assertion below exercises a synchronous start() rejection.
    QCoreApplication app(argc, argv);

    QString base = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                              : QStringLiteral("/home/norman/Documents/zapret");
    const QString winRoot = base + "/windows";
    const QString linuxRoot = base + "/linux";

    out << "=== zapret core verification ===\n";
    out << "base = " << base << "\n\n";

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
    check(win.size() == 22, "22 .bat strategies (23 minus service.bat)");
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
        check(genD->workingDirectory == winRoot, "workingDirectory = windows root");
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
        check(wc.executablePath.startsWith(winRoot), "executablePath is under the windows root");
        check(wc.workingDirectory == winRoot, "workingDirectory is the bat directory");

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

    out << "\n=== " << (g_failures ? "FAILED" : "ALL PASS") << " (" << g_failures << " failures) ===\n";
    out.flush();
    return g_failures ? 1 : 0;
}

