// uitest — headless widget + service verification for the zapret GUI layer.
//
// Runs under the offscreen QPA platform (QT_QPA_PLATFORM is also forced in-process, so the binary is
// runnable straight from a shell). Like coretest it is a plain console program: readable PASS/FAIL
// lines, non-zero exit on any failure, no QTest data-driven scaffolding.
//
// Deliberately NOT covered here: a constructed MainWindow (it needs a window manager and, for the tray
// paths, a tray host) — its pure policy predicates are covered — anything that would spawn
// pkexec/winws, and real network reachability (the ping test only asserts that a verdict arrives).
// DownloadDialog is constructed but never started: startFetch() is the only thing in it that touches
// GitHub, and a test suite must not depend on that answer.

#include "IZapretBackend.h"
#include "DownloadDialog.h"
#include "LogPanel.h"
#include "MainWindow.h"
#include "PingIndicator.h"
#include "PingService.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "Strategy.h"
#include "StrategyCard.h"
#include "Theme.h"
#include "ZapretDownloader.h"
#include "ZapretPaths.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QVariant>

#include <QtTest/QTest>

#include <cstdio>

using namespace zapret;

static int g_failures = 0;

static void check(bool cond, const QString &what)
{
    std::printf(cond ? "  PASS  %s\n" : "  FAIL  %s\n", qUtf8Printable(what));
    if (!cond)
        ++g_failures;
}

// ---------------------------------------------------------------- Settings ----
static void testSettings()
{
    std::printf("\n[Settings]\n");

    QTemporaryDir dir;
    check(dir.isValid(), "temporary dir created");
    const QString file = dir.filePath(QStringLiteral("muffin-test.ini"));

    {
        Settings s(file);
        check(s.zapretPath().isEmpty(), "default zapretPath is empty");
        check(s.lastStrategyId().isEmpty(), "default lastStrategyId is empty");
        check(s.startLastOnLaunch() == false, "default startLastOnLaunch is false");
        check(s.minimizeToTray() == true, "default minimizeToTray is true");
        check(s.pingIntervalSec() == 5, "default ping interval is 5s");
        check(s.theme() == QLatin1String("dark"), "default theme is dark");

        QSignalSpy changed(&s, &Settings::changed);
        s.setZapretPath(QStringLiteral("/opt/zapret/linux"));
        s.setLastStrategyId(QStringLiteral("general"));
        s.setStartLastOnLaunch(true);
        s.setMinimizeToTray(false);
        s.setTheme(QStringLiteral("system"));
        check(changed.count() == 5, "each setter emits changed()");

        // The spinbox range is also enforced in the model, so a bad value cannot be persisted.
        s.setPingIntervalSec(900);
        check(s.pingIntervalSec() == 60, "ping interval clamped to 60");
        s.setPingIntervalSec(1);
        check(s.pingIntervalSec() == 3, "ping interval clamped to 3");
        s.setPingIntervalSec(12);
        check(s.pingIntervalSec() == 12, "ping interval stored as given");
    }

    {
        Settings reopened(file);
        check(reopened.zapretPath() == QLatin1String("/opt/zapret/linux"), "zapretPath round-trip");
        check(reopened.lastStrategyId() == QLatin1String("general"), "lastStrategyId round-trip");
        check(reopened.startLastOnLaunch() == true, "startLastOnLaunch round-trip");
        check(reopened.minimizeToTray() == false, "minimizeToTray round-trip");
        check(reopened.theme() == QLatin1String("system"), "theme round-trip");
        check(reopened.pingIntervalSec() == 12, "ping interval round-trip");
    }
}

// Write `text` at `path`, creating parent directories on the way. The migration asserts read on-disk
// truth — the repair only fires when a folder really exists or really went away — so an empty promise
// of a path is not enough here.
static bool writeTextFile(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray bytes = text.toUtf8();
    const bool ok = f.write(bytes) == bytes.size();
    f.close();
    return ok;
}

// ------------------------------------------------- Settings identity migration ----
//
// The settings half of the 0.1.1 organization rename. Everything here is the user-level store, so the
// section only runs while the harness redirects the config and data locations into the build tree
// (CMake sets XDG_CONFIG_HOME/XDG_DATA_HOME for uitest): otherwise the test would be reading and
// writing a real desktop's settings file, which is the one thing a migration test must never do.
static void clearTree(const QString &path)
{
    QDir dir(path);
    if (dir.exists())
        dir.removeRecursively();
}

static void testSettingsMigration()
{
    std::printf("\n[Settings: 0.1.1 identity migration]\n");

    const QString configBase =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString dataBase =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString xdgConfig = qEnvironmentVariable("XDG_CONFIG_HOME");
    const QString xdgData = qEnvironmentVariable("XDG_DATA_HOME");
    const bool redirected =
        !xdgConfig.isEmpty() && !xdgData.isEmpty()
        && QDir::cleanPath(configBase) == QDir::cleanPath(xdgConfig)
        && QDir::cleanPath(dataBase) == QDir::cleanPath(xdgData);
    if (!redirected)
    {
        std::printf("  SKIP  the settings import needs XDG_CONFIG_HOME and XDG_DATA_HOME redirected "
                    "into the build tree (run it through ctest)\n");
        return;
    }

    const QString legacyOrg = ZapretPaths::legacyOrganizationName();
    const QString currentOrg = QCoreApplication::organizationName();
    const QString app = QCoreApplication::applicationName();
    // The two stores are two directories under the redirected config root, named after the two
    // organizations. Asserting that up front is what proves this test is sandboxed, not the paths it
    // later rewrites.
    const QString legacyStoreDir = QDir(configBase).filePath(legacyOrg);
    const QString currentStoreDir = QDir(configBase).filePath(currentOrg);
    check(legacyOrg != currentOrg && !currentOrg.isEmpty(),
          "the test identity differs from the legacy organization");
    check(QDir::cleanPath(legacyStoreDir).startsWith(QDir::cleanPath(configBase) + QLatin1Char('/'))
              && QDir::cleanPath(currentStoreDir).startsWith(
                     QDir::cleanPath(configBase) + QLatin1Char('/')),
          "both stores live under the redirected config location");

    const QString legacyManaged = ZapretPaths::legacyManagedInstallDir();
    const QString managed = ZapretPaths::managedInstallDir();
    check(!legacyManaged.isEmpty() && !managed.isEmpty() && legacyManaged != managed,
          "the migration's two directory names are distinct");
    check(QDir::cleanPath(legacyManaged).startsWith(QDir::cleanPath(dataBase) + QLatin1Char('/'))
              && QDir::cleanPath(managed).startsWith(QDir::cleanPath(dataBase) + QLatin1Char('/')),
          "and both live under the redirected data location");

    clearTree(legacyStoreDir);
    clearTree(currentStoreDir);
    clearTree(legacyManaged);
    clearTree(managed);

    // ---- the state an upgraded user is in: 0.1.0's store filled, this build's not ----
    const QString legacyLinuxRoot = legacyManaged + QStringLiteral("/linux");
    const QString managedLinuxRoot = managed + QStringLiteral("/linux");
    // What a portable-mode user actually has in `strategy/last`: not a name, but the absolute path of
    // the binary the parser surfaced — inside the very folder the migration renames.
    const QString legacyPortableId =
        legacyManaged + QStringLiteral("/binaries/linux-x86_64/nfqws");
    const QString managedPortableId =
        managed + QStringLiteral("/binaries/linux-x86_64/nfqws");

    // The file 0.1.0's QSettings() resolved to: this build's scope and format, the organization it
    // replaced, the same application name. Everything the import reads comes from here.
    const QString legacyFile =
        QSettings(QSettings::defaultFormat(), QSettings::UserScope, legacyOrg, app).fileName();
    check(!legacyFile.isEmpty(), "the legacy store has a resolvable file name");
    check(QDir::cleanPath(legacyFile).startsWith(QDir::cleanPath(legacyStoreDir) + QLatin1Char('/')),
          "and it is the file inside the legacy organization folder");

    {
        // The tree moved first (that is ZapretPaths::migrateManagedInstallDir(), covered in coretest),
        // so what is on disk here is the post-move world: the new folder holds it, the old one is gone.
        check(writeTextFile(managedLinuxRoot + QStringLiteral("/nfq/nfqws"), "payload\n"),
              "planted the migrated tree at the new name");
        check(writeTextFile(managedPortableId, "nfqws\n"),
              "including the portable binary a stored strategy id can name");
        check(!QFileInfo::exists(legacyManaged), "the legacy folder is vacated");

        // Written through Settings itself, on the legacy store's own file name, so the key spellings
        // the import looks for are produced by the code that reads them and cannot drift.
        Settings legacyStore(legacyFile);
        legacyStore.setTheme(QStringLiteral("light"));
        legacyStore.setPingIntervalSec(7);
        legacyStore.setLastStrategyId(legacyPortableId);
        legacyStore.setStartLastOnLaunch(true);
        legacyStore.setMinimizeToTray(false);
        legacyStore.setZapretPath(legacyLinuxRoot);
        check(QFileInfo::exists(legacyFile), "the legacy store file was written");
    }

    // ---- the import ----
    importLegacySettings();

    {
        Settings migrated;   // this build's own store, reached by identity rather than by path
        check(QDir::cleanPath(migrated.filePath()) != QDir::cleanPath(legacyFile),
              "and it is a different file than the one the import read");
        check(migrated.theme() == QLatin1String("light"), "theme carried over");
        check(migrated.pingIntervalSec() == 7, "ping interval carried over");
        check(migrated.lastStrategyId() == QDir::cleanPath(managedPortableId),
              "a stored strategy path followed the tree to its new folder too");
        check(migrated.startLastOnLaunch() == true, "start-last-on-launch carried over");
        check(migrated.minimizeToTray() == false, "minimize-to-tray carried over");
        check(QDir::cleanPath(migrated.zapretPath()) == QDir::cleanPath(managedLinuxRoot),
              "the stored tree path followed the tree to its new folder");
    }

    // ---- never overwrite ----
    //
    // This guard is what makes running the import on every startup safe: once this identity holds any
    // value at all, the legacy store is inert. So edit the new store, contradict it from the old one,
    // and re-run — the user's newer choice must win.
    {
        Settings migrated;
        migrated.setTheme(QStringLiteral("system"));

        Settings legacyStore(legacyFile);
        legacyStore.setTheme(QStringLiteral("dark"));

        importLegacySettings();

        Settings again;
        check(again.theme() == QLatin1String("system"), "an imported store is never imported over");
        check(QDir::cleanPath(again.zapretPath()) == QDir::cleanPath(managedLinuxRoot),
              "and re-running the repair leaves the repaired path alone");
    }

    // ---- which stored paths the repair may touch, and which it must not ----
    //
    // Empty the current store, then read the import's answer for one stored value. The managed install
    // is the only folder this migration moves, so a path that names it — itself or a child the locator
    // resolved inside it — is rebased, while anything the user pointed elsewhere survives byte for byte.
    {
        auto importedPathFor = [&](const QString &storedPath) {
            clearTree(currentStoreDir);
            Settings legacyStore(legacyFile);
            legacyStore.setZapretPath(storedPath);
            importLegacySettings();
            Settings migrated;
            return migrated.zapretPath();
        };

        check(QDir::cleanPath(importedPathFor(legacyLinuxRoot)) == QDir::cleanPath(managedLinuxRoot),
              "a child of the moved folder keeps its child segment");
        check(QDir::cleanPath(importedPathFor(legacyManaged)) == QDir::cleanPath(managed),
              "the moved folder itself is rebased");
        const QString foreign = QStringLiteral("/opt/zapret/linux");
        check(importedPathFor(foreign) == foreign, "a path this migration never moved is left alone");
        check(importedPathFor(legacyLinuxRoot + QStringLiteral("/../linux"))
                  == QDir::cleanPath(managedLinuxRoot),
              "and an untidy spelling of a moved path is still recognised");
    }

    // A strategy id the parser could name is a bare word, and the stored root may be a folder the user
    // picked in the dialog: neither may be edited by a repair that is looking for one directory.
    {
        clearTree(currentStoreDir);
        Settings legacyStore(legacyFile);
        legacyStore.setLastStrategyId(QStringLiteral("general"));
        legacyStore.setZapretPath(QStringLiteral("/opt/zapret/linux"));
        importLegacySettings();

        Settings migrated;
        check(migrated.lastStrategyId() == QLatin1String("general"),
              "a bare strategy id is carried over as it was");
        check(migrated.zapretPath() == QLatin1String("/opt/zapret/linux"),
              "and a root outside the managed folder keeps its own spelling");
    }

    clearTree(legacyStoreDir);
    clearTree(currentStoreDir);
    clearTree(managed);
    clearTree(legacyManaged);
}

// ---------------------------------------------------------------- LogPanel ----
static QString firstForegroundColor(const QTextBlock &block)
{
    const QList<QTextLayout::FormatRange> ranges = block.textFormats();
    if (ranges.isEmpty())
        return QString();
    return ranges.first().format.foreground().color().name(QColor::HexRgb);
}

static void testLogPanel()
{
    std::printf("\n[LogPanel]\n");

    LogPanel panel;
    check(panel.storedLineCount() == 0, "starts empty");
    check(panel.filterLevel() == LogPanel::All, "starts unfiltered");

    panel.appendLine(QStringLiteral("service up"), LogPanel::Info);
    panel.appendLine(QStringLiteral("list missing"), LogPanel::Warn);
    panel.appendLine(QStringLiteral("winws exited"), LogPanel::Error);
    panel.appendLine(QStringLiteral("bare line"));   // default level = info

    check(panel.storedLineCount() == 4, "four lines stored");
    check(panel.view()->document()->blockCount() == 4, "four blocks in the document");

    const QString all = panel.text();
    check(all.contains(QStringLiteral("[INFO] service up")), "info line formatted");
    check(all.contains(QStringLiteral("[WARN] list missing")), "warn line formatted");
    check(all.contains(QStringLiteral("[ERR] winws exited")), "error line tagged ERR");
    check(all.count(QLatin1Char('\n')) == 3, "lines joined with newlines");
    check(QRegularExpression(R"(\[\d{2}:\d{2}:\d{2}\])").match(all).hasMatch(), "timestamp prefix present");

    QTextDocument *doc = panel.view()->document();
    check(firstForegroundColor(doc->findBlockByNumber(0)) == Theme::logColor(0).name(QColor::HexRgb),
          "info uses the theme text colour");
    check(firstForegroundColor(doc->findBlockByNumber(1)) == Theme::warning().name(QColor::HexRgb),
          "warn is amber");
    check(firstForegroundColor(doc->findBlockByNumber(2)) == Theme::error().name(QColor::HexRgb),
          "error is red");

    const qreal heightAll = doc->size().height();

    panel.setFilterLevel(LogPanel::Error);
    check(panel.visibleLineCount() == 1, "error filter leaves one visible line");
    check(panel.storedLineCount() == 4, "filtering does not drop stored lines");
    QString visibleText;
    for (QTextBlock b = doc->firstBlock(); b.isValid(); b = b.next())
        if (b.isVisible())
            visibleText += b.text();
    check(visibleText.contains(QStringLiteral("winws exited")), "the surviving line is the error");
    check(doc->size().height() < heightAll, "hidden blocks shrink the laid-out document");

    panel.setFilterLevel(LogPanel::Warn);
    check(panel.visibleLineCount() == 2, "warn filter keeps warn + error");

    panel.setFilterLevel(LogPanel::All);
    check(panel.visibleLineCount() == 4, "all filter restores every line");
    check(qFuzzyCompare(doc->size().height(), heightAll), "document height restored");

    panel.clear();
    check(panel.storedLineCount() == 0 && panel.view()->toPlainText().isEmpty(), "clear() empties both");

    panel.appendLine(QStringLiteral("persisted"), LogPanel::Warn);
    const QString out = QDir::tempPath() + QStringLiteral("/uitest-log.txt");
    check(panel.saveToFile(out), "saveToFile writes");
    QFile saved(out);
    check(saved.open(QIODevice::ReadOnly), "saved file readable");
    const QString savedText = QString::fromUtf8(saved.readAll());
    check(savedText.contains(QStringLiteral("[WARN] persisted")), "saved file holds the line");
    saved.close();
    QFile::remove(out);

    // The ring buffer cap must be honoured: the widget drops, and the cache follows it.
    LogPanel big;
    for (int i = 0; i < LogPanel::kMaxLines + 250; ++i)
        big.appendLine(QStringLiteral("line %1").arg(i), LogPanel::Info);
    check(big.storedLineCount() == LogPanel::kMaxLines, "cache capped at kMaxLines");
    check(big.view()->document()->blockCount() <= LogPanel::kMaxLines + 1, "view capped too");
    check(big.text().contains(QStringLiteral("line 250")), "oldest lines dropped first");
}

// ---------------------------------------------------------- PingIndicator ----
static void testPingIndicator()
{
    std::printf("\n[PingIndicator]\n");

    PingIndicator chip(QStringLiteral("Youtube"));
    check(chip.minimumWidth() >= 120, "chip has a 120px minimum width");
    check(chip.status() == PingIndicator::PingStatus::Unknown, "starts Unknown");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Unknown, -1) == QStringLiteral("—"),
          "Unknown shows an em dash");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Ok, 12) == QStringLiteral("12 мс"),
          "Ok shows milliseconds");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Fail, -1) == QStringLiteral("таймаут"),
          "Fail shows timeout");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Waiting, -1) == QStringLiteral("…"),
          "Waiting shows ellipsis");

    check(PingIndicator::statusFromServiceInt(0) == PingIndicator::PingStatus::Waiting, "0 -> Waiting");
    check(PingIndicator::statusFromServiceInt(1) == PingIndicator::PingStatus::Ok, "1 -> Ok");
    check(PingIndicator::statusFromServiceInt(2) == PingIndicator::PingStatus::Fail, "2 -> Fail");
    check(PingIndicator::statusFromServiceInt(42) == PingIndicator::PingStatus::Unknown, "other -> Unknown");

    check(PingIndicator::dotColor(PingIndicator::PingStatus::Ok) == Theme::pingOk(), "Ok dot is green");
    check(PingIndicator::dotColor(PingIndicator::PingStatus::Fail) == Theme::pingFail(), "Fail dot is red");
    check(PingIndicator::dotColor(PingIndicator::PingStatus::Waiting) == Theme::pingWaiting(),
          "Waiting dot is amber");
    check(PingIndicator::dotColor(PingIndicator::PingStatus::Unknown) == Theme::pingUnknown(),
          "Unknown dot is grey");

    check(!chip.isPulsing(), "Unknown does not pulse");
    chip.setStatus(PingIndicator::PingStatus::Ok, 34);
    check(chip.isPulsing(), "Ok pulses");
    check(chip.summary().startsWith(QStringLiteral("Youtube")), "summary carries the service name");
    check(chip.toolTip().contains(QStringLiteral("34 мс")), "tooltip carries the latency");
    chip.setStatus(PingIndicator::PingStatus::Fail, -1);
    check(!chip.isPulsing(), "Fail stops the pulse");
}

// ------------------------------------------------------------ StrategyCard ----
static void testStrategyCard()
{
    std::printf("\n[StrategyCard]\n");

    Strategy s;
    s.id = QStringLiteral("gen-1");
    s.name = QStringLiteral("general");
    s.fileName = QStringLiteral("general.bat");
    s.dirPath = QDir::tempPath();
    s.filePath = QDir::tempPath() + QStringLiteral("/general.bat");
    s.kind = Strategy::Kind::Binary;
    s.platform = Strategy::Platform::Windows;
    s.executablePath = QDir::tempPath() + QStringLiteral("/bin/winws.exe");
    s.argv = {QStringLiteral("--wf-tcp=80,443"), QStringLiteral("--new"), QStringLiteral("--dpi-desync=fake")};
    s.requiresRoot = true;

    StrategyCard card(s);
    check(card.objectName() == QLatin1String("strategyCard"), "objectName is strategyCard");
    check(card.id() == QLatin1String("gen-1"), "id comes from the strategy");
    check(card.property("active").toBool() == false, "not active initially");
    check(card.strategy().name == QLatin1String("general"), "card keeps its strategy");

    const QString cmd = card.commandLine();
    check(cmd.contains(QStringLiteral("winws.exe")), "command line names the program");
    check(cmd.contains(QStringLiteral("--wf-tcp=80,443")), "command line carries argv");

    auto *button = card.findChild<QPushButton *>();
    check(button != nullptr, "card has its action button");
    if (!button)
        return;

    check(button->text() == QStringLiteral("Запустить"), "idle button says Запустить");
    check(button->objectName() == QLatin1String("primaryButton"), "idle button is primary");

    QSignalSpy startSpy(&card, &StrategyCard::startRequested);
    QSignalSpy stopSpy(&card, &StrategyCard::stopRequested);

    button->click();
    check(startSpy.count() == 1 && stopSpy.count() == 0, "idle click requests a start");
    check(startSpy.at(0).at(0).toString() == QLatin1String("gen-1"), "start carries the strategy id");

    card.setRunning(true);
    check(card.property("active").toBool() == true, "running sets the active property");
    check(button->text() == QStringLiteral("Остановить"), "running button says Остановить");
    check(button->objectName() != QLatin1String("primaryButton"), "running button is not primary");
    button->click();
    check(stopSpy.count() == 1 && startSpy.count() == 1, "running click requests a stop");

    card.setRunning(false);
    check(card.property("active").toBool() == false, "stopping clears the active property");

    // ---- the inline failure surface ----
    // A backend error used to live only in the log and a modal; the card now carries it too. The row
    // widget is what gets hidden, so visibility is asserted on it (a child of a hidden parent is not
    // itself "hidden" in Qt's sense).
    QWidget *badgeRow = card.findChild<QWidget *>(QStringLiteral("cardErrorRow"));
    QLabel *badge = card.findChild<QLabel *>(QStringLiteral("cardError"));
    check(badgeRow != nullptr && badge != nullptr, "the card owns an error badge row and label");
    check(!card.hasError(), "a fresh card carries no failure");
    check(card.property("error").toBool() == false, "the error property starts false");
    check(badgeRow->isHidden(), "the badge row is hidden while nothing failed");

    const QString reason = QStringLiteral("LinuxBackend: start command failed (exit 1)");
    card.setError(reason);
    check(card.hasError(), "setError marks the card as failed");
    check(card.errorMessage() == reason, "the full reason is stored verbatim");
    check(card.property("error").toBool() == true, "the error property drives the QSS border");
    check(!badgeRow->isHidden(), "the badge row becomes visible");
    check(!badge->text().isEmpty(), "and shows the reason");
    check(badge->toolTip() == reason, "the tooltip holds the un-elided reason");
    check(badge->text().length() <= reason.length(), "the shown text is the reason or an elision of it");
    check(button->text() == QStringLiteral("Повторить"), "the action after a failure is a retry");
    check(button->objectName() == QLatin1String("primaryButton"), "a retry is still a primary action");
    check(card.property("active").toBool() == false, "a failure is not marked active");

    card.setError(QStringLiteral("   "));
    check(!card.hasError(), "a whitespace-only message means no failure");
    check(button->text() == QStringLiteral("Запустить"), "and the button goes back to a plain start");

    card.setError(reason);
    card.setError(reason);
    check(card.errorMessage() == reason, "re-reporting the same failure is idempotent");

    card.setRunning(true);
    check(!card.hasError(), "a confirmed start clears the badge");
    check(card.property("error").toBool() == false, "and the error property with it");
    check(button->text() == QStringLiteral("Остановить"), "running still offers the stop action");
    card.setRunning(false);
    check(button->text() == QStringLiteral("Запустить"), "stopped again offers a start, not a stale retry");

    // The elision is display-only: a long backend message must never be truncated in the model.
    const QString longReason = QStringLiteral(
        "WindowsBackend: not elevated — relaunch the application as administrator to load the "
        "WinDivert driver and then start this strategy again");
    card.setError(longReason);
    check(card.errorMessage() == longReason, "the model keeps the whole message");
    check(badge->toolTip() == longReason, "the tooltip can show all of it");
    card.clearError();
    check(!card.hasError() && badgeRow->isHidden(), "clearError hides the badge row again");
}

// ------------------------------------------------------ tray + policy glue ----
static void testTrayPolicy()
{
    std::printf("\n[tray policy]\n");

    // Whether closing hides or quits is a pure predicate, so the whole truth table is checkable here
    // even though the window itself needs a real desktop to construct.
    check(MainWindow::hideOnClose(true, true), "minimize-to-tray with a tray host hides the window");
    check(!MainWindow::hideOnClose(true, false), "minimize-to-tray with no tray host quits instead");
    check(!MainWindow::hideOnClose(false, true), "an unticked setting never hides");
    check(!MainWindow::hideOnClose(false, false), "nor does it hide when both are off");

    // The predicate must follow the platform's own answer rather than a guessed list of desktops.
    check(MainWindow::traySupported() == QSystemTrayIcon::isSystemTrayAvailable(),
          "traySupported mirrors QSystemTrayIcon::isSystemTrayAvailable");

    // The settings sheet explains a tray this desktop cannot provide instead of leaving a ticked box
    // that silently does nothing.
    QTemporaryDir dir;
    check(dir.isValid(), "temporary settings store created");
    Settings settings(dir.filePath(QStringLiteral("muffin-test.ini")));
    settings.setMinimizeToTray(true);

    SettingsDialog dialog(settings);
    QLabel *note = dialog.trayNote();
    check(note != nullptr, "the dialog carries a tray note");
    check(note && note->objectName() == QLatin1String("trayNote"), "the note is styled by objectName");
    // Hidden exactly when there is a tray to speak of — isHidden() is the widget's own flag, and the
    // dialog is not shown here, so it is the right thing to compare against.
    check(note && note->isHidden() == MainWindow::traySupported(),
          "the note is hidden exactly when this desktop has a tray host");
    check(note && !note->text().isEmpty(), "and the note says something when it is shown");
    check(!dialog.trayNote()->text().contains(QStringLiteral("%1")), "the note has no unsubstituted placeholder");
}

// ---------------------------------------------------------- window policy ----
static void testWindowPolicy()
{
    std::printf("\n[window policy]\n");

    // The status chip's wording is a pure mapping, so the phases a portable run walks through — which
    // are the only ones a view can misread as "nothing is happening" — are checkable here.
    using S = IZapretBackend::State;
    check(MainWindow::statusText(S::Starting, QStringLiteral("nfqws (portable)"))
              .contains(QStringLiteral("nfqws (portable)")),
          "the firewall-up phase still names the strategy the user picked");
    check(MainWindow::statusText(S::Running, QStringLiteral("nfqws (portable)"))
              .contains(QStringLiteral("nfqws (portable)")),
          "and so does the live payload");
    check(!MainWindow::statusText(S::Stopping, QStringLiteral("nfqws (portable)")).isEmpty(),
          "the teardown phase has wording of its own");
    check(MainWindow::statusKind(S::Starting) == MainWindow::statusKind(S::Stopping),
          "in-flight phases share one chip colour: neither reads as idle");
    check(MainWindow::statusKind(S::Running) != MainWindow::statusKind(S::Stopped),
          "up and down are distinguishable");
    check(MainWindow::statusKind(S::Error) != MainWindow::statusKind(S::Stopped),
          "a failure is not shown as an ordinary stop");

    // Which card a backend failure belongs to. A pending start owns the answer while the attempt is
    // unresolved; otherwise the live run does; otherwise the window's memory of the last live run —
    // which is exactly what a deferred portable error needs, because the manager has forgotten the
    // strategy by the time that error is announced (see the manager-level test in coretest).
    const QString pend = QStringLiteral("A");
    const QString act = QStringLiteral("B");
    const QString last = QStringLiteral("C");
    check(MainWindow::errorTargetId(pend, act, last) == pend,
          "the card that asked for this start owns its failure");
    check(MainWindow::errorTargetId(QString(), act, last) == act,
          "once the attempt resolved, the live run's card does");
    check(MainWindow::errorTargetId(QString(), QString(), last) == last,
          "with the run already forgotten, the remembered one is badged");
    check(MainWindow::errorTargetId(QString(), QString(), QString()).isEmpty(),
          "and a failure that belongs to no run badges nothing rather than something stale");
    check(MainWindow::errorTargetId(pend, QString(), QString()) == pend,
          "an empty fallback chain never outranks the pending start");
}

// ------------------------------------------------------------------- Theme ----
static void testTheme(QApplication &app)
{
    std::printf("\n[Theme]\n");

    Theme::apply(app, true);
    check(Theme::isDark(), "dark mode is active");
    check(!app.styleSheet().isEmpty(), "dark stylesheet loaded from resources");
    check(app.styleSheet().contains(QStringLiteral("#7C3AED")), "accent colour present in the sheet");
    check(app.styleSheet().contains(QStringLiteral("strategyCard")), "card rules present");
    check(app.palette().color(QPalette::Window) == Theme::windowBackground(), "palette matches tokens");

    Theme::apply(app, false);
    check(!Theme::isDark(), "light mode is active");
    check(app.styleSheet().contains(QStringLiteral("#F4F5FA")), "light background in the sheet");

    // Both sheets must carry the rules the dynamic properties switch on, or a badge/notice would render
    // as unstyled default text.
    for (bool dark : {true, false}) {
        Theme::apply(app, dark);
        const QString sheet = app.styleSheet();
        check(sheet.contains(QStringLiteral("[error=\"true\"]")),
              QStringLiteral("%1 sheet styles a failed card").arg(dark ? QStringLiteral("dark")
                                                                      : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("cardError")),
              QStringLiteral("%1 sheet colours the badge text").arg(dark ? QStringLiteral("dark")
                                                                        : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("trayNote")),
              QStringLiteral("%1 sheet colours the tray notice").arg(dark ? QStringLiteral("dark")
                                                                         : QStringLiteral("light")));
    }

    check(Theme::modeFromName(QStringLiteral("light")) == Theme::Mode::Light, "name -> Light");
    check(Theme::modeFromName(QStringLiteral("System")) == Theme::Mode::System, "name -> System");
    check(Theme::modeFromName(QStringLiteral("nonsense")) == Theme::Mode::Dark, "unknown name -> Dark");
    check(Theme::nameForMode(Theme::Mode::System) == QLatin1String("system"), "mode -> name");

    const QIcon play = Theme::icon(QStringLiteral("play"), QColor(Qt::red), 16);
    check(!play.isNull(), "play glyph loads from the qrc");
    check(!play.pixmap(16, 16).isNull(), "play glyph rasterises at 16px");
    const QPixmap pm = play.pixmap(16, 16);
    bool sawInk = false;
    if (!pm.isNull()) {
        const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < img.height() && !sawInk; ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const QRgb px = img.pixel(x, y);
                if (qAlpha(px) > 200 && qRed(px) > 120 && qGreen(px) < 90 && qBlue(px) < 90) {
                    sawInk = true;
                    break;
                }
            }
        }
    }
    check(sawInk, "currentColor substitution tinted the glyph red");

    // Every shipped icon must resolve, or the QSS/toolbars would silently show holes.
    const QStringList glyphs = {QStringLiteral("app"), QStringLiteral("play"), QStringLiteral("stop"),
                                QStringLiteral("refresh"), QStringLiteral("settings"),
                                QStringLiteral("tray"), QStringLiteral("clear"), QStringLiteral("copy"),
                                QStringLiteral("ping-ok"), QStringLiteral("ping-fail"),
                                QStringLiteral("ping-wait"), QStringLiteral("error")};
    for (const QString &g : glyphs)
        check(QFile::exists(QStringLiteral(":/icons/%1.svg").arg(g)),
              QStringLiteral("icon file exists: %1").arg(g));

    // The badge glyph must actually rasterise in both themes, or a failed start shows a red hole.
    check(!Theme::icon(QStringLiteral("error"), Theme::error(), 16).pixmap(16, 16).isNull(),
          "the error glyph renders in the dark theme tint");
}

// -------------------------------------------------------------- PingService ----
static void testPingService()
{
    std::printf("\n[PingService]\n");

    check(PingService::defaultHosts().size() == 3, "three default hosts");
    check(PingService::defaultHosts().contains(QLatin1String("youtube.com")), "youtube is a default host");

    {
        PingService ping;
        // 127.0.0.1:443 answers instantly one way or the other on any host — we assert that a verdict
        // arrives, not which verdict it is.
        ping.setHosts({QStringLiteral("127.0.0.1")});
        ping.setIntervalMs(200);
        check(ping.hosts().size() == 1, "setHosts stored");
        check(ping.intervalMs() == 200, "setIntervalMs stored");

        QSignalSpy spy(&ping, &PingService::pingUpdated);
        ping.start();
        check(spy.wait(20000) || spy.count() > 0, "pingUpdated emitted");

        // A cycle emits Waiting first and the verdict afterwards: keep pumping events until the
        // verdict lands (bounded), rather than asserting on the first signal only.
        int waited = 0;
        while (waited < 20000) {
            bool verdictSeen = false;
            for (int i = 0; i < spy.count(); ++i) {
                const int status = spy.at(i).at(1).toInt();
                if (status == int(PingService::Ok) || status == int(PingService::Fail))
                    verdictSeen = true;
            }
            if (verdictSeen)
                break;
            QTest::qWait(50);
            waited += 50;
        }

        bool sawWaiting = false;
        bool sawVerdict = false;
        for (int i = 0; i < spy.count() && !(sawWaiting && sawVerdict); ++i) {
            const QList<QVariant> args = spy.at(i);
            if (args.size() != 3)
                continue;
            const int status = args.at(1).toInt();
            sawWaiting = sawWaiting || (status == int(PingService::Waiting));
            sawVerdict = sawVerdict || (status == int(PingService::Ok) || status == int(PingService::Fail));
            }
        for (int i = 0; i < spy.count(); ++i)
            check(spy.at(i).at(0).toString() == QLatin1String("127.0.0.1"), "signal names the host");
        check(sawWaiting, "a Waiting state was reported before the verdict");
        check(sawVerdict, "a terminal Ok/Fail was reported");

        ping.stop();
        // stop() posts into the worker thread, so the flag clears asynchronously.
        int cleared = 0;
        while (ping.isRunning() && cleared < 5000) {
            QTest::qWait(25);
            cleared += 25;
        }
        check(!ping.isRunning(), "stop() clears the running flag");
    }
    // Leaving the scope joins the worker thread (~PingService). If that hangs, the test times out
    // rather than leaking a thread.
    check(true, "PingService thread joined cleanly");
}

// ------------------------------------------------------------- DownloadDialog ----
//
// Nothing here talks to GitHub. The dialog's constructor only builds widgets and the request is
// startFetch(), which these tests never call — that separation is precisely what makes a download UI
// testable, and it is what the assertions below are written against.
static void testDownloadDialog()
{
    std::printf("\n[DownloadDialog]\n");

    DownloadDialog dialog;
    check(!dialog.isBusy(), "a fresh dialog has nothing in flight");
    check(!dialog.isDownloadEnabled(),
          "the download action is disabled until a release has actually been picked");
    check(dialog.installedPath().isEmpty(), "nothing is reported installed before anything ran");
    check(!dialog.statusText().isEmpty(), "and the status line is never left blank");
    check(dialog.downloader() != nullptr, "the dialog owns its downloader");

    // The default target is the directory the GUI manages — the same one ZapretLocator lists first, so
    // what is installed here is still found after a restart.
    const QString root = ZapretDownloader::installRoot();
    check(!root.isEmpty() && QDir::fromNativeSeparators(root).endsWith(QLatin1String("/zapret")),
          "the managed install directory ends in zapret");
    check(dialog.targetDir() == root, "and the dialog points at it without being told to");

    QTemporaryDir tmp;
    check(tmp.isValid(), "temporary directory for the target override");
    const QString overrideDir = tmp.filePath(QStringLiteral("zapret-target"));
    dialog.setTargetDir(overrideDir);
    check(dialog.targetDir() == overrideDir, "an override is reported back verbatim");
    check(!QDir(overrideDir).exists(),
          "naming a target does not create it: only a finished download may write there");

    // Wording the info line is built from, checked without a network answer.
    check(DownloadDialog::humanSize(0) == QStringLiteral("0 Б"), "zero bytes has wording of its own");
    check(DownloadDialog::humanSize(1536).contains(QStringLiteral("КиБ")), "kilobytes are spelled in КиБ");
    check(DownloadDialog::humanSize(7844856).contains(QStringLiteral("МиБ")), "and megabytes in МиБ");
    check(DownloadDialog::humanSize(-1) == QStringLiteral("неизвестно"),
          "an unannounced size is called unknown, not zero");
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("MuffinTest"));
    QApplication::setOrganizationName(QStringLiteral("MuffinTest"));

    std::printf("=== zapret GUI verification (offscreen) ===\n");
    std::printf("platform = %s\n", qPrintable(app.platformName()));

    Theme::apply(app, true);

    testSettings();
    testSettingsMigration();
    testLogPanel();
    testPingIndicator();
    testStrategyCard();
    testTrayPolicy();
    testWindowPolicy();
    testTheme(app);
    testPingService();
    testDownloadDialog();

    std::printf("\n=== %s (%d failures) ===\n", g_failures ? "FAILED" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
