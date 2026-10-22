// muffin-cli — the blockcheck console.
//
// One measurement, two heads: the GUI's «Проверить реальный обход» button and this program drive the
// same zapret::BlockcheckEngine. What the console adds is a machine-readable contract the window
// cannot offer — JSON on stdout, logs on stderr, and exit codes a script can branch on:
//
//   0  every target host passed the check
//   1  the check ran, at least one host failed (this is the "the bypass does not work" answer)
//   2  the check could not run at all (no zapret tree, unknown preset, refused start, bad usage)
//
// Typical runs:
//   muffin-cli --preset general.bat --test discord.com,youtube.com
//   muffin-cli --preset general --zapret-dir ~/Documents/zapret | jq '.["discord.com"].latency'
//   muffin-cli --test discord.com          # baseline: probe without touching zapret
//   sudo muffin-cli --preset general       # portable-mode trees edit the firewall as root
//
// Everything here is QtCore + QtNetwork through zapretcore; no widget, no window, no GUI thread to
// protect — runTestForPreset() pumps its nested event loops right on this thread and returns the
// finished map, which is exactly the synchronous contract the engine keeps for consoles.

#include "BlockcheckEngine.h"
#include "PingService.h"
#include "Strategy.h"
#include "ZapretManager.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

using namespace zapret;

namespace {

// How many of the probed hosts failed. The exit code is the one number a watchdog actually reads.
int hostFailures(const QJsonObject &results)
{
    int failed = 0;
    for (auto it = results.constBegin(); it != results.constEnd(); ++it)
        if (!it.value().toObject().value(QLatin1String("success")).toBool())
            ++failed;
    return failed;
}

// Resolve what the user typed to a catalog entry. People spell presets three ways — the file name
// (general.bat), the display name (general (ALT5)) and a bare stem (general) — and the catalog
// already carries all three per entry, so the match walks them in that order, case-insensitively.
// An empty result means "not found", which the caller turns into a listing rather than a guess:
// starting a different preset than the one named would be the console lying about what it measured.
const Strategy *matchPreset(const QList<Strategy> &list, const QString &name)
{
    const QString want = name.trimmed();
    if (want.isEmpty())
        return nullptr;

    for (const Strategy &s : list)
        if (s.fileName.compare(want, Qt::CaseInsensitive) == 0)
            return &s;
    for (const Strategy &s : list)
        if (s.name.compare(want, Qt::CaseInsensitive) == 0)
            return &s;
    for (const Strategy &s : list) {
        const QString id = s.id.isEmpty() ? s.filePath : s.id;
        if (id.compare(want, Qt::CaseInsensitive) == 0)
            return &s;
        // "general" for "general.bat": the stem, when the tree has exactly one file with it.
        QString stem = s.fileName;
        const int dot = stem.lastIndexOf(QLatin1Char('.'));
        if (dot > 0)
            stem.truncate(dot);
        if (stem.compare(want, Qt::CaseInsensitive) == 0)
            return &s;
    }
    return nullptr;
}

// One log line to stderr, level-tagged. stdout stays JSON-only by contract.
void logToStderr(QTextStream &err, const QString &line, int level)
{
    const char *tag = level >= 2 ? "[E] " : (level == 1 ? "[W] " : "[i] ");
    err << tag << line << '\n';
    err.flush();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("muffin-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(MUFFIN_VERSION));

    QTextStream out(stdout);
    QTextStream err(stderr);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Console blockcheck for zapret: starts a preset, probes the target hosts through it and\n"
        "prints the verdicts as JSON on stdout. All logs go to stderr.\n"
        "\n"
        "Exit codes: 0 = every host passed, 1 = at least one host failed, 2 = the check could not\n"
        "run (no zapret tree, unknown preset, refused start, bad usage).\n"
        "\n"
        "Portable-mode trees edit the firewall as root: on Linux run this under sudo, or from a\n"
        "desktop session with a polkit agent, or the start will be refused with a clear message."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption presetOpt(
        {QStringLiteral("p"), QStringLiteral("preset")},
        QStringLiteral("Preset to run, e.g. general.bat. Omit to probe WITHOUT touching zapret "
                       "(a baseline measurement)."),
        QStringLiteral("name"));
    const QCommandLineOption testOpt(
        {QStringLiteral("t"), QStringLiteral("test")},
        QStringLiteral("Comma-separated hosts to check (default: youtube.com,discord.com,google.com)."),
        QStringLiteral("hosts"));
    const QCommandLineOption dirOpt(
        QStringLiteral("zapret-dir"),
        QStringLiteral("Where the zapret tree lives (default: auto-locate, like the GUI)."),
        QStringLiteral("path"));
    const QCommandLineOption portOpt(
        QStringLiteral("port"),
        QStringLiteral("Port to check; 443 (the default) adds the TLS + certificate verdict."),
        QStringLiteral("port"), QStringLiteral("443"));
    parser.addOption(presetOpt);
    parser.addOption(testOpt);
    parser.addOption(dirOpt);
    parser.addOption(portOpt);
    parser.process(app);

    bool portOk = false;
    const int port = parser.value(portOpt).toInt(&portOk);
    if (!portOk || port <= 0 || port > 65535) {
        err << "invalid --port: " << parser.value(portOpt) << '\n';
        err.flush();
        return 2;
    }

    QStringList targets;
    if (parser.isSet(testOpt)) {
        const QStringList parts = parser.value(testOpt).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            const QString host = part.trimmed();
            if (!host.isEmpty())
                targets << host;
        }
    }
    if (targets.isEmpty())
        targets = PingService::defaultHosts();

    BlockcheckEngine engine;
    engine.setPort(port);
    QObject::connect(&engine, &BlockcheckEngine::logMessage,
                     [&err](const QString &line, int level) { logToStderr(err, line, level); });
    QObject::connect(&engine, &BlockcheckEngine::progress,
                     [&err](int done, int total, const QString &host) {
                         err << QStringLiteral("    [%1/%2] %3").arg(done).arg(total).arg(host) << '\n';
                         err.flush();
                     });

    QJsonObject results;

    if (!parser.isSet(presetOpt)) {
        // Baseline mode: no preset named, so nothing is started, stopped or touched. Useful as the
        // "before" half of a before/after comparison and on a box where zapret runs as a service.
        err << "no --preset given: probing without starting zapret\n";
        err.flush();
        int done = 0;
        for (const QString &host : targets) {
            const HostCheckResult r = engine.checkHost(host, port);
            results.insert(host, BlockcheckEngine::resultToJson(r));
            err << QStringLiteral("    [%1/%2] %3").arg(++done).arg(targets.size()).arg(host) << '\n';
            err.flush();
        }
    } else {
        ZapretManager manager;
        QObject::connect(&manager, &ZapretManager::logMessage,
                         [&err](const QString &line, int level) { logToStderr(err, line, level); });
        manager.setZapretDir(parser.value(dirOpt));   // "" = auto-locate

        if (manager.strategyCount() == 0) {
            err << "no zapret strategies found"
                << (manager.resolvedRoot().isEmpty()
                        ? QStringLiteral(" (searched the well-known locations; try --zapret-dir)")
                        : QStringLiteral(" under %1").arg(QDir::toNativeSeparators(manager.resolvedRoot())))
                << '\n';
            err.flush();
            return 2;
        }

        const Strategy *preset = matchPreset(manager.strategies(), parser.value(presetOpt));
        if (!preset) {
            err << "preset not found: " << parser.value(presetOpt) << "\navailable:\n";
            const QList<Strategy> all = manager.strategies();
            for (const Strategy &s : all)
                err << "  - " << s.fileName
                    << (s.name == s.fileName ? QString() : QStringLiteral("  (%1)").arg(s.name)) << '\n';
            err.flush();
            return 2;
        }
        err << QStringLiteral("preset: %1 [%2]").arg(preset->fileName, preset->filePath) << '\n';
        err.flush();

        engine.setManager(&manager);
        results = engine.runTestForPreset(*preset, targets);
        if (results.isEmpty()) {
            err << "blockcheck failed: " << engine.lastError() << '\n';
            err.flush();
            return 2;
        }
    }

    out << QJsonDocument(results).toJson(QJsonDocument::Indented);
    out.flush();
    return hostFailures(results) == 0 ? 0 : 1;
}
