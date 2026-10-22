// uitest — headless widget + service verification for the zapret GUI layer.
//
// Runs under the offscreen QPA platform (QT_QPA_PLATFORM is also forced in-process, so the binary is
// runnable straight from a shell). Like coretest it is a plain console program: readable PASS/FAIL
// lines, non-zero exit on any failure, no QTest data-driven scaffolding.
//
// Deliberately NOT covered here: anything that would spawn pkexec or winws, and real network
// reachability (the ping test only asserts that a verdict arrives). DownloadDialog is constructed but
// never started: startFetch() is the only thing in it that touches GitHub, and a test suite must not
// depend on that answer.
//
// A constructed MainWindow is covered in exactly one place — the watchdog section, which needs the real
// catalog and journal to prove that a dead bypass turns into an offer. It is built without a window
// manager (offscreen), never shown, and its three seams that would need a desktop, a network or a root
// prompt are overridden; the store it reads is the redirected test identity, and that section skips
// itself when the harness has not redirected it.

#include "BlockcheckEngine.h"
#include "IZapretBackend.h"
#include "DownloadDialog.h"
#include "LanguageManager.h"
#include "LogPanel.h"
#include "MainWindow.h"
#include "PingIndicator.h"
#include "PingService.h"
#include "PowerButton.h"
#include "ProfileCard.h"
#include "Settings.h"
#include "SettingsPage.h"
#include "Strategy.h"
#include "Theme.h"
#include "ZapretDownloader.h"
#include "ZapretPaths.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QToolButton>
#include <QVariant>
#include <QWidget>

#include <QtTest/QTest>

#include <cstdio>

using namespace zapret;

static int g_failures = 0;

// True when `w` sits in a QHBoxLayout that has a spacer after it — the shape the settings page uses to
// keep a short field beside its label instead of across the whole form. Walking the layouts is the only
// way to see it: a stretch is an item, not a widget, so nothing about the field itself records that it
// has one, and a maximum width alone would still leave the control floating in its column.
static bool fieldEndsInStretch(QWidget *w)
{
    if (!w || !w->parentWidget())
        return false;
    const auto layouts = w->parentWidget()->findChildren<QLayout *>();
    for (QLayout *l : layouts) {
        auto *row = qobject_cast<QHBoxLayout *>(l);
        if (!row)
            continue;
        const int at = row->indexOf(w);
        if (at < 0)
            continue;
        for (int i = at + 1; i < row->count(); ++i)
            if (row->itemAt(i) && row->itemAt(i)->spacerItem())
                return true;
    }
    return false;
}

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
        check(s.watchdogIntervalMin() == 5, "default watchdog interval is 5 min");
        check(Settings::kWatchdogDefaultIntervalMin == 5,
              "and the default the window reads is the same number");
        check(s.watchdogIntervalMin() >= Settings::kWatchdogMinIntervalMin
                  && s.watchdogIntervalMin() <= Settings::kWatchdogMaxIntervalMin,
              "the default sits inside the range the page offers");
        check(s.theme() == QLatin1String("dark"), "default theme is dark");

        QSignalSpy changed(&s, &Settings::changed);
        s.setZapretPath(QStringLiteral("/opt/zapret/linux"));
        s.setLastStrategyId(QStringLiteral("general"));
        s.setStartLastOnLaunch(true);
        s.setMinimizeToTray(false);
        s.setWatchdogIntervalMin(15);
        s.setTheme(QStringLiteral("system"));
        check(changed.count() == 6, "each setter emits changed()");

        // The spinbox range is also enforced in the model, so a bad value cannot be persisted.
        s.setPingIntervalSec(900);
        check(s.pingIntervalSec() == 60, "ping interval clamped to 60");
        s.setPingIntervalSec(1);
        check(s.pingIntervalSec() == 3, "ping interval clamped to 3");
        s.setPingIntervalSec(12);
        check(s.pingIntervalSec() == 12, "ping interval stored as given");
        // Same for the watchdog, against its own published bounds: a stored 0 would arm a timer that
        // fires continuously, and a stored 900 one that never fires in a session.
        s.setWatchdogIntervalMin(0);
        check(s.watchdogIntervalMin() == Settings::kWatchdogMinIntervalMin,
              "watchdog interval clamped to the floor");
        s.setWatchdogIntervalMin(900);
        check(s.watchdogIntervalMin() == Settings::kWatchdogMaxIntervalMin,
              "watchdog interval clamped to the ceiling");
        s.setWatchdogIntervalMin(15);
        check(s.watchdogIntervalMin() == 15, "watchdog interval stored as given");
    }

    {
        Settings reopened(file);
        check(reopened.zapretPath() == QLatin1String("/opt/zapret/linux"), "zapretPath round-trip");
        check(reopened.lastStrategyId() == QLatin1String("general"), "lastStrategyId round-trip");
        check(reopened.startLastOnLaunch() == true, "startLastOnLaunch round-trip");
        check(reopened.minimizeToTray() == false, "minimizeToTray round-trip");
        check(reopened.theme() == QLatin1String("system"), "theme round-trip");
        check(reopened.pingIntervalSec() == 12, "ping interval round-trip");
        check(reopened.watchdogIntervalMin() == 15, "watchdog interval round-trip");
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
    // The badge is a column now, not a status-bar chip: a fixed width that fits a name and a 50 px ring,
    // and no minimum that would push the dashboard's row apart.
    check(chip.width() >= 64, "badge has a width a 50px ring fits");
    check(chip.minimumWidth() < 120, "badge no longer reserves status-bar chip width");
    check(chip.status() == PingIndicator::PingStatus::Unknown, "starts Unknown");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Unknown, -1) == QStringLiteral("—"),
          "Unknown shows an em dash");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Ok, 12) == QStringLiteral("12 мс"),
          "Ok shows milliseconds");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Fail, -1) == QStringLiteral("таймаут"),
          "Fail shows timeout");
    check(PingIndicator::latencyText(PingIndicator::PingStatus::Waiting, -1) == QStringLiteral("…"),
          "Waiting shows ellipsis");

    // The figure above the ring is the same state without the unit: a bare number when there is one, and
    // something that is not a number when there is not.
    check(PingIndicator::latencyNumber(PingIndicator::PingStatus::Ok, 24) == QStringLiteral("24"),
          "Ok shows the bare number");
    check(PingIndicator::latencyNumber(PingIndicator::PingStatus::Fail, -1) == QStringLiteral("!"),
          "Fail shows a mark, not a zero that could read as a fast reply");
    check(PingIndicator::latencyNumber(PingIndicator::PingStatus::Waiting, -1) == QStringLiteral("…"),
          "Waiting shows ellipsis");
    check(PingIndicator::latencyNumber(PingIndicator::PingStatus::Unknown, -1) == QStringLiteral("—"),
          "Unknown shows an em dash");

    check(PingIndicator::statusFromServiceInt(0) == PingIndicator::PingStatus::Waiting, "0 -> Waiting");
    check(PingIndicator::statusFromServiceInt(1) == PingIndicator::PingStatus::Ok, "1 -> Ok");
    check(PingIndicator::statusFromServiceInt(2) == PingIndicator::PingStatus::Fail, "2 -> Fail");
    check(PingIndicator::statusFromServiceInt(42) == PingIndicator::PingStatus::Unknown, "other -> Unknown");

    check(PingIndicator::dotColor(PingIndicator::PingStatus::Ok) == Theme::pingOk(), "Ok ring is green");
    check(PingIndicator::dotColor(PingIndicator::PingStatus::Fail) == Theme::pingFail(), "Fail ring is red");
    check(PingIndicator::dotColor(PingIndicator::PingStatus::Waiting) == Theme::pingWaiting(),
          "Waiting ring is amber");
    check(PingIndicator::dotColor(PingIndicator::PingStatus::Unknown) == Theme::pingUnknown(),
          "Unknown ring is grey");
    // The dashboard's one green is a spec colour, not the general success token: the connected chip, the
    // live rings and a running power button all paint with this one value.
    check(Theme::pingOk() == QColor(0x10, 0xB9, 0x81), "the live green is #10B981");

    // Which mark a service gets, and that an unknown host still gets one rather than an empty ring.
    check(PingIndicator::glyphForService(QStringLiteral("Youtube")) == QStringLiteral("youtube"),
          "youtube gets its brand mark");
    check(PingIndicator::glyphForService(QStringLiteral("DISCORD")) == QStringLiteral("discord"),
          "the match ignores case, the names come from hostnames");
    check(PingIndicator::glyphForService(QStringLiteral("Google")) == QStringLiteral("globe"),
          "anything else gets the globe");

    check(!chip.isPulsing(), "Unknown does not pulse");
    chip.setStatus(PingIndicator::PingStatus::Ok, 34);
    check(chip.isPulsing(), "Ok pulses");
    check(chip.summary().startsWith(QStringLiteral("Youtube")), "summary carries the service name");
    check(chip.toolTip().contains(QStringLiteral("34 мс")), "tooltip carries the latency");
    chip.setStatus(PingIndicator::PingStatus::Fail, -1);
    check(!chip.isPulsing(), "Fail stops the pulse");

    // Renaming a badge re-picks its mark: a chip that kept youtube's glyph after being pointed at
    // discord would be the one thing on screen lying about what it measured.
    chip.setServiceName(QStringLiteral("Discord"));
    check(chip.glyph() == QStringLiteral("discord"), "setServiceName follows the name to the mark");
    check(chip.serviceName() == QStringLiteral("Discord"), "and keeps the name");
}

// ------------------------------------------------------------- PowerButton ----
static void testPowerButton()
{
    std::printf("\n[PowerButton]\n");

    PowerButton button;
    check(button.diameter() == 200, "the default circle is 200 px");
    // The widget is the circle plus the halo's margin on both sides — a button exactly as wide as its own
    // ring clips its glow into a square.
    check(button.width() == 200 + 2 * PowerButton::kHaloMargin, "the widget reserves halo room");
    check(button.height() == button.width(), "and the same on both axes");
    check(button.mode() == PowerButton::Mode::Idle, "starts idle");
    check(PowerButton::modeColor(PowerButton::Mode::Idle) == Theme::accent(),
          "idle is the accent purple");
    check(PowerButton::modeColor(PowerButton::Mode::Busy) == Theme::accent(),
          "a transition in flight stays purple and only breathes");
    check(PowerButton::modeColor(PowerButton::Mode::Active) == Theme::pingOk(),
          "a live run wears the same green as the connected chip");
    check(button.glyph() == QStringLiteral("power"), "the default glyph is the power mark");

    button.setMode(PowerButton::Mode::Busy);
    check(button.mode() == PowerButton::Mode::Busy, "mode is readable back");
    check(button.glow() > 0.0 && button.glow() <= 1.0, "the pulse holds the glow in range");
    button.setMode(PowerButton::Mode::Idle);
    check(qFuzzyCompare(button.glow(), 1.0), "leaving Busy parks the glow at full");

    // A diameter change has to move the glyph's rasterised size with it, or a 120 px button would carry
    // a 64 px power mark; and absurd sizes are clamped rather than honoured, because a 0 px circle is a
    // widget nobody can click.
    button.setDiameter(120);
    check(button.diameter() == 120, "setDiameter is readable back");
    check(button.width() == 120 + 2 * PowerButton::kHaloMargin, "and resizes the widget");
    button.setDiameter(0);
    check(button.diameter() == 48, "a zero diameter is clamped, not obeyed");
}

// --------------------------------------------------------------- ProfileCard ----
//
// A profile row is a selection item, not a control: no start button, only the name, the mini ping chips
// for the services it unblocks, and a menu (copy / delete). These checks pin that shape - the properties
// the sheet styles off, the signals the window wires, and the tooltip that carries a failure's reason
// now that a 52 px row has no room for a badge row of its own.
static void testProfileCard()
{
    std::printf("\n[ProfileCard]\n");

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

    ProfileCard card(s);
    check(card.objectName() == QLatin1String("profileCard"), "objectName is profileCard");
    check(card.id() == QLatin1String("gen-1"), "id comes from the strategy");
    check(card.property("selected").toBool() == false, "not selected initially");
    check(card.property("error").toBool() == false, "not failed initially");
    check(card.strategy().name == QLatin1String("general"), "row keeps its strategy");
    check(card.nameLabel() && card.nameLabel()->text() == QLatin1String("general"),
          "the name label shows the strategy's name");

    const QString cmd = card.commandLine();
    check(cmd.contains(QStringLiteral("winws.exe")), "command line names the program");
    check(cmd.contains(QStringLiteral("--wf-tcp=80,443")), "command line carries argv");

    // ---- the row menu ----
    // Two entries about the file on disk, and deliberately no start/stop: the power button owns running.
    QToolButton *menuButton = card.menuButton();
    check(menuButton != nullptr, "the row has a menu button");
    check(menuButton && menuButton->menu() && menuButton->menu()->actions().size() == 2,
          "the menu holds exactly copy and delete");

    // ---- selection ----
    QSignalSpy clickedSpy(&card, &ProfileCard::clicked);
    QTest::mousePress(&card, Qt::LeftButton);
    check(clickedSpy.count() == 1 && clickedSpy.at(0).at(0).toString() == QLatin1String("gen-1"),
          "a left press emits clicked(id)");
    card.setSelected(true);
    check(card.isSelected(), "setSelected marks it selected");
    check(card.property("selected").toBool() == true, "and the QSS property follows");
    card.setSelected(true);   // idempotent
    check(card.isSelected(), "re-selecting an already-selected row is a no-op");
    card.setSelected(false);
    check(!card.isSelected() && card.property("selected").toBool() == false,
          "deselecting clears both");

    // ---- the menu's own signals ----
    QSignalSpy copySpy(&card, &ProfileCard::copyRequested);
    QSignalSpy deleteSpy(&card, &ProfileCard::deleteRequested);
    if (menuButton && menuButton->menu() && menuButton->menu()->actions().size() == 2) {
        menuButton->menu()->actions().at(0)->trigger();   // copy
        menuButton->menu()->actions().at(1)->trigger();   // delete
        check(copySpy.count() == 1 && deleteSpy.count() == 1, "both menu entries fire once");
        check(copySpy.at(0).at(0).toString() == QLatin1String("gen-1"), "copy carries the id");
        check(QApplication::clipboard()->text() == cmd,
              "copying already put the command on the clipboard");
    } else {
        check(false, "menu actions were reachable to trigger");
    }

    // ---- the mini ping chips ----
    // One chip per probed host, in order. A chip for a service nobody measures would be the only dishonest
    // number on the row, so the count must track setHosts exactly.
    const QStringList hosts = {QStringLiteral("youtube.com"), QStringLiteral("discord.com")};
    card.setHosts(hosts);
    check(card.pingChipCount() == 2, "two hosts, two chips");
    card.setHosts({QStringLiteral("youtube.com")});
    check(card.pingChipCount() == 1, "a shorter host list rebuilds, not reconciles");
    card.setHosts(hosts);
    // An unknown host is ignored rather than guessed at; a known one updates without disturbing the other.
    card.setPing(QStringLiteral("youtube.com"), PingIndicator::PingStatus::Ok, 42);
    card.setPing(QStringLiteral("nobody.test"), PingIndicator::PingStatus::Ok, 1);
    check(card.pingChipCount() == 2, "an unlisted host added no phantom chip");

    // ---- the ring's own shape ----
    // A chip is a mark inside a ring, and the ring is the part that had been drawn too small to read:
    // the mark at 16 px inside a 7 px radius, with its number touching the border. The three things that
    // fixed it are all invisible to a screenshot diff and visible to a layout edit, so they are pinned.
    // setHosts() retires the chips it replaces through deleteLater, and deferred deletion does not run
    // just because events were processed — so the tree still holds last rebuild's ghosts until they are
    // flushed. Counting before this would count chips the row no longer shows.
    QTest::qWait(10);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto chips = card.findChildren<QWidget *>(QStringLiteral("miniPing"));
    check(chips.size() == 2, "one ring per service the row reports");
    if (chips.size() == 2) {
        QWidget *chip = chips.first();
        check(chip->property("pingStatus").toString() == QLatin1String("ok"),
              "the ring carries the verdict the sheet colours it from");
        check(chip->graphicsEffect() != nullptr,
              "the ring's glow is an effect, because QSS has no box-shadow");
        QLayout *pad = chip->layout();
        check(pad && pad->contentsMargins().left() > 0 && pad->contentsMargins().top() > 0,
              "mark and figure are padded off the ring's edge");
        const auto marks = chip->findChildren<QLabel *>(QStringLiteral("miniPingIcon"));
        check(!marks.isEmpty() && marks.first()->width() >= 20,
              "the mark is enlarged, not merely re-painted");
    }
    // An effect paints outside the widget it is set on, and a child is clipped to its parent's rect — so
    // the row that holds the chips has to reserve the halo's room or every glow is cut flat at the edge.
    QWidget *host = card.findChild<QWidget *>(QStringLiteral("profilePings"));
    check(host && host->layout() && host->layout()->contentsMargins().top() >= 6,
          "the chip row leaves room so the glow is not clipped");

    // ---- the failure surface ----
    // No badge row any more: the edge turns red and the reason moves to the tooltip, so the tooltip is the
    // assertion here - the whole reason, in order, after the path.
    const QString reason = QStringLiteral("LinuxBackend: start command failed (exit 1)");
    card.setError(reason);
    check(card.hasError(), "setError marks the row as failed");
    check(card.errorMessage() == reason, "the full reason is stored verbatim");
    check(card.property("error").toBool() == true, "the error property drives the QSS border");
    check(card.toolTip().contains(reason), "the tooltip carries the whole reason");
    check(card.toolTip().contains(QStringLiteral("/general.bat")),
          "and still names the file the row stands for");

    // Empty (and whitespace-only) clear it, and re-reporting is idempotent.
    card.setError(QStringLiteral("   "));
    check(!card.hasError(), "a whitespace-only message means no failure");
    check(card.property("error").toBool() == false, "and clears the error property");
    card.setError(reason);
    card.setError(reason);
    check(card.errorMessage() == reason, "re-reporting the same failure is idempotent");

    // A long backend message is never truncated in the model, and the tooltip can show all of it.
    const QString longReason = QStringLiteral(
        "WindowsBackend: not elevated - relaunch the application as administrator to load the "
        "WinDivert driver and then start this strategy again");
    card.setError(longReason);
    check(card.errorMessage() == longReason, "the model keeps the whole message");
    check(card.toolTip().contains(longReason), "the tooltip can show all of it");
    card.setError(QString());
    check(!card.hasError(), "an empty message clears the failure");

    // Retranslating and re-theming must not crash or drop state - both walk the chips and the menu.
    card.retranslateText();
    card.retheme();
    check(card.nameLabel()->text() == QLatin1String("general"), "retranslate kept the name");
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

    // The settings page explains a tray this desktop cannot provide instead of leaving a ticked box that
    // silently does nothing — and, unlike the old modal, it commits from its own Save button and says so.
    QTemporaryDir dir;
    check(dir.isValid(), "temporary settings store created");
    Settings settings(dir.filePath(QStringLiteral("muffin-test.ini")));
    settings.setMinimizeToTray(true);

    SettingsPage page(settings);
    QLabel *note = page.trayNote();
    check(note != nullptr, "the page carries a tray note");
    check(note && note->objectName() == QLatin1String("trayNote"), "the note is styled by objectName");
    // Hidden exactly when there is a tray to speak of — isHidden() is the widget's own flag, and the
    // page is not shown here, so it is the right thing to compare against.
    check(note && note->isHidden() == MainWindow::traySupported(),
          "the note is hidden exactly when this desktop has a tray host");
    check(note && !note->text().isEmpty(), "and the note says something when it is shown");
    check(!page.trayNote()->text().contains(QStringLiteral("%1")),
          "the note has no unsubstituted placeholder");

    // ---- the page is a commit, not a modal ----
    // A page has no frame to dismiss and no OK/Cancel pair, so the two ways it could quietly regress are
    // "Save does nothing" and "Save never says it saved". Both are pinned here, against the model the
    // page is meant to write through.
    QPushButton *save = page.saveButton();
    check(save != nullptr, "the page carries its own save button");
    check(save && save->objectName() == QLatin1String("primaryButton"), "and it is the primary action");

    const QString path = dir.filePath(QStringLiteral("zapret/linux"));
    page.setZapretPath(path);
    QSignalSpy savedSpy(&page, &SettingsPage::saved);
    save->click();
    check(savedSpy.count() == 1, "Save fires saved() once");
    check(settings.zapretPath() == path, "Save wrote the path through the model");
    // The tray box was loaded checked (the model said minimize-to-tray) and the commit wrote every field
    // back, so a round-trip through the page must leave the flag set — not silently drop it.
    check(settings.minimizeToTray(), "the page's commit preserved the tray setting it was loaded with");

    // The acknowledgement appears and is worded; the reload guard means committing did not drop it by
    // re-reading the form underneath the write.
    QLabel *ack = page.savedNote();
    check(ack != nullptr && !ack->text().isEmpty(), "Save put an acknowledgement beside the button");
    check(ack && ack->text() == LanguageManager::tr(QStringLiteral("settings.saved")),
          "and it is the catalog's 'saved' wording, not the key");

    // ---- the two short fields ----
    // A page that lets a form column stretch every field gives a three-word choice the same presence as
    // a path, which is how the settings sheet starts reading as a form to fill rather than as switches to
    // flip. The cap is one half of the fix; the row that ends in a stretch is the other, and both are
    // things a later layout edit can undo without a visible clue in the code.
    QComboBox *theme = page.themeField();
    QSpinBox *interval = page.pingIntervalField();
    check(theme && theme->maximumWidth() == 200, "the theme picker is one short value wide");
    check(interval && interval->maximumWidth() == 200, "and so is the ping interval");
    check(interval && interval->buttonSymbols() == QAbstractSpinBox::NoButtons,
          "the interval box shows no stepper arrows");
    check(theme && fieldEndsInStretch(theme), "the theme picker is held to the left of its row");
    check(interval && fieldEndsInStretch(interval), "and so is the interval box");

    // ---- the watchdog's cadence ----
    // The page is the only place the number is chosen, so the range it offers has to be the range the
    // model stores and the window arms its timer from — one spelling, read from Settings rather than
    // repeated as a second pair of literals here. Shaped like the box above on purpose.
    QSpinBox *watchdog = page.watchdogIntervalField();
    check(watchdog != nullptr, "the page carries a watchdog interval field");
    check(watchdog && watchdog->maximumWidth() == 200, "it is one short value wide like the others");
    check(watchdog && watchdog->buttonSymbols() == QAbstractSpinBox::NoButtons,
          "and it shows no stepper arrows either");
    check(watchdog && watchdog->minimum() == Settings::kWatchdogMinIntervalMin
                  && watchdog->maximum() == Settings::kWatchdogMaxIntervalMin,
          "its range is the model's range, 1 to 60 minutes");
    check(watchdog && fieldEndsInStretch(watchdog), "and it is held to the left of its row");
    check(watchdog && !watchdog->suffix().isEmpty(), "the unit is spelled on the field");
    check(watchdog && watchdog->property("i18nSuffix").toString()
                          == QLatin1String("settings.watchdog_suffix"),
          "tagged for retranslation, so the unit moves with the language");
    // Loaded from the model, not left on the widget's own default: the store said 5 minutes.
    check(watchdog && watchdog->value() == settings.watchdogIntervalMin(),
          "the field opens on the stored interval");
    // And committed back through Save, the only way this page writes anything.
    if (watchdog)
        watchdog->setValue(30);
    save->click();
    check(settings.watchdogIntervalMin() == 30, "Save wrote the new interval through the model");

    // A write that came from somewhere else (a download, the startup auto-location) re-reads the page
    // rather than leaving it showing a stale path.
    const QString other = dir.filePath(QStringLiteral("zapret/windows"));
    settings.setZapretPath(other);   // fires changed(); the page is not mid-commit, so it reloads
    check(page.zapretPath() == other, "an external change re-reads into the field");
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

    // The header's chip is the same mapping in one word. It has to distinguish all five states on its
    // own now: with the toolbar's pill gone it is the only place the state is written, and a chip that
    // said "OFFLINE" during a teardown would be the window lying about a live firewall.
    check(MainWindow::connectionText(S::Running) == QStringLiteral("CONNECTED"),
          "a live run is the connected state the mock draws");
    check(MainWindow::connectionText(S::Stopped) != MainWindow::connectionText(S::Stopping),
          "idle and tearing-down are different words");
    check(MainWindow::connectionText(S::Starting) != MainWindow::connectionText(S::Running),
          "coming up is not yet up");
    check(!MainWindow::connectionText(S::Error).isEmpty(), "a failure has wording");
    // The chip's word and its dot share one property value, so the pair cannot disagree.
    check(MainWindow::connectionText(S::Running) != MainWindow::connectionText(S::Error)
              && MainWindow::statusKind(S::Running) != MainWindow::statusKind(S::Error),
          "the two states that look most alike at 12 px differ in both word and colour");

    // ---- the action matrix ----
    // Every start/stop control in the window is derived from these two predicates, so the whole table IS
    // the feature. What was missing was one row: a Stopping state that left both the toolbar's
    // "Остановить" and every card's "Запустить" live for as long as the iptables teardown took, so the
    // second click ten seconds into a stop either re-issued terminate() or drew the backend's
    // "an operation is already in progress (stop first)" across a card that had done nothing wrong.
    check(MainWindow::canStartFrom(S::Stopped), "an idle backend accepts a start");
    check(MainWindow::canStartFrom(S::Error), "a failure can be retried");
    check(!MainWindow::canStartFrom(S::Starting), "a launch in flight accepts no other launch");
    check(!MainWindow::canStartFrom(S::Running), "one strategy at a time");
    check(!MainWindow::canStartFrom(S::Stopping),
          "a teardown accepts no start — the reported case");

    check(MainWindow::canStopFrom(S::Running), "a live run can be stopped");
    check(MainWindow::canStopFrom(S::Starting),
          "a stuck launch stays cancellable: the backend queues this stop instead of racing it");
    check(!MainWindow::canStopFrom(S::Stopping), "a stop already under way is not re-issued");
    check(!MainWindow::canStopFrom(S::Stopped), "nothing to stop when nothing ran");
    check(!MainWindow::canStopFrom(S::Error), "nor after a failure");

    for (const S s : {S::Stopped, S::Starting, S::Running, S::Stopping, S::Error}) {
        // A card holds a single button, so a state offering both actions could not be expressed at all.
        check(!MainWindow::canStartFrom(s) || !MainWindow::canStopFrom(s),
              "no state offers a start and a stop at the same time");
        // The sentence a gated control shows must exist exactly when the control is gated, or a greyed
        // button arrives with an empty tooltip and looks broken rather than busy.
        check(MainWindow::blockedReason(s).isEmpty() == MainWindow::canStartFrom(s),
              "a state that accepts a start has nothing to wait for, and one that refuses says why");
        // The chip is the only part of the window a user reads as "busy" — it must go amber over exactly
        // the states the buttons are gated by, or the wording and the controls disagree.
        check((MainWindow::statusKind(s) == QStringLiteral("starting"))
                  == (s == S::Starting || s == S::Stopping),
              "the busy chip covers precisely the two in-flight states");
    }

    check(MainWindow::statusText(S::Stopping, QStringLiteral("nfqws (portable)"))
              .contains(QStringLiteral("Остановка")),
          "the chip and the tray tooltip read Остановка… while the teardown runs");
    check(MainWindow::blockedReason(S::Stopping).contains(QStringLiteral("дождитесь")),
          "and the reason offered in its place tells the user to wait");
    check(MainWindow::blockedReason(S::Running).contains(QStringLiteral("остановите")),
          "while a run is up, the missing action is named as the stop");

    // ---- what a Stop press has to look like, before the backend can say so ----
    //
    // The teardown runs on the GUI thread and waits inside it, so the repaint stateChanged(Stopping)
    // would normally cause cannot land until the wait is over. The window therefore draws the stopping
    // look itself, ahead of the signal — and the look it draws has to be the same one the signal draws,
    // or a press shows a button the run then contradicts. These are the two halves of that: a look that
    // exists for a teardown, and one that differs from the live run it replaced.
    check(MainWindow::powerMode(S::Running) == PowerButton::Mode::Active,
          "a live run wears the green button whose press means stop");
    check(MainWindow::powerMode(S::Stopping) == PowerButton::Mode::Busy,
          "a teardown in flight is the busy look — the breathing ring the press can paint early");
    check(MainWindow::powerMode(S::Stopping) == MainWindow::powerMode(S::Starting),
          "both in-flight states draw one look, so the press and the signal cannot disagree");
    check(MainWindow::powerMode(S::Stopping) != MainWindow::powerMode(S::Running),
          "and pressing Stop visibly changes the button rather than leaving it as it was");
    for (const S s : {S::Stopped, S::Starting, S::Running, S::Stopping, S::Error}) {
        // One table for both controls: the chip goes busy over precisely the states the button does.
        check((MainWindow::powerMode(s) == PowerButton::Mode::Busy)
                  == (MainWindow::statusKind(s) == QStringLiteral("starting")),
              "the button's busy look and the chip's busy colour cover the same states");
    }
    check(MainWindow::powerMode(S::Stopped) == PowerButton::Mode::Idle
              && MainWindow::powerMode(S::Error) == PowerButton::Mode::Idle,
          "nothing running and nothing in flight is the plain idle button, failure included");

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

    // ---- which rows a ping verdict may reach ----
    // The rows are not a second copy of the dashboard: a cycle measures the path the running profile
    // opened, so an idle row must keep its last verdict rather than borrow somebody else's. The manual
    // round is the one press that measures every row at once, and the predicate is the whole rule.
    check(MainWindow::profilePingHosts().size() == 2,
          "a row carries a chip for the services it exists to unblock");
    check(MainWindow::profilePingHosts().contains(QLatin1String("youtube.com"))
              && MainWindow::profilePingHosts().contains(QLatin1String("discord.com")),
          "and those chips are the two the profile is used for");
    check(!MainWindow::profilePingHosts().contains(QLatin1String("google.com")),
          "the dashboard's baseline host is not a claim about a profile");
    check(MainWindow::profilePingHosts().size() < PingService::defaultHosts().size(),
          "the rows probe a subset of what the badges do, never more");

    check(!MainWindow::rowFollowsCycle(false, false),
          "an idle row does not follow the cycle of a run that is not its own");
    check(MainWindow::rowFollowsCycle(false, true),
          "the running profile's row follows every cycle");
    check(MainWindow::rowFollowsCycle(true, false),
          "a manual round reaches the rows that are not running");
    check(MainWindow::rowFollowsCycle(true, true),
          "and the one that is");

    // ---- the bypass check's verdict sheet ----
    //
    // The engine measures and the window words. What sits between them is this pure formatter, so the
    // two halves are run through each other here: a verdict goes out as the engine's own JSON (its real
    // resultToJson, not a hand-copy of the shape) and comes back as the line the user reads. That is
    // what catches a key renamed on one side only — a sheet that silently prints "FAIL (0 ms)" for
    // every host that actually passed.
    auto sheetFor = [](const QString &host, const HostCheckResult &r) {
        QJsonObject map;
        map.insert(host, BlockcheckEngine::resultToJson(r));
        return MainWindow::blockcheckResultText(map);
    };

    HostCheckResult pass;
    pass.success = true;
    pass.avgLatency = 45;
    pass.attemptsOk = 3;
    pass.attempts = 3;
    check(sheetFor(QStringLiteral("youtube.com"), pass)
              == QStringLiteral("Youtube: OK (45 ms)"),
          "a host that got through reads as OK with its latency");

    HostCheckResult dead;
    dead.success = false;
    dead.avgLatency = -1;
    dead.attempts = 3;
    dead.lastError = QStringLiteral("connect timed out after 3000 ms");
    check(sheetFor(QStringLiteral("discord.com"), dead) == QStringLiteral("Discord: FAIL"),
          "a host that did not is a bare FAIL with no latency invented for it");

    // The one line that means "intercepted", not "unreachable" — and the only case the sheet decorates.
    HostCheckResult intercepted = pass;
    intercepted.isCertificateValid = false;
    check(sheetFor(QStringLiteral("youtube.com"), intercepted)
              == QStringLiteral("Youtube: OK (45 ms) — %1")
                     .arg(LanguageManager::tr(QStringLiteral("blockcheck.cert_invalid"))),
          "an OK whose certificate was rejected says so on the same line");
    HostCheckResult deadUntrusted = dead;
    deadUntrusted.isCertificateValid = false;
    check(sheetFor(QStringLiteral("discord.com"), deadUntrusted)
              == QStringLiteral("Discord: FAIL"),
          "a host that never got through is not also blamed for its certificate");

    // An absent flag must read as the ordinary pass: the sheet is also handed maps written by older
    // builds and by muffin-cli, where the field simply is not there.
    QJsonObject legacy;
    legacy.insert(QStringLiteral("youtube.com"),
                  QJsonObject{{QStringLiteral("success"), true},
                              {QStringLiteral("latency"), 12}});
    check(MainWindow::blockcheckResultText(legacy) == QStringLiteral("Youtube: OK (12 ms)"),
          "a map without a certificate_valid key defaults to trusted, not to a warning");

    check(MainWindow::blockcheckResultText(QJsonObject()).isEmpty(),
          "no results is no lines — the empty map is the caller's no_results dialog instead");

    // Two hosts, inserted both ways round: QJsonObject owns the ordering, so the sheet a user reads
    // cannot depend on the order the probes happened to finish in.
    QJsonObject forward, backward;
    forward.insert(QStringLiteral("discord.com"), BlockcheckEngine::resultToJson(dead));
    forward.insert(QStringLiteral("youtube.com"), BlockcheckEngine::resultToJson(pass));
    backward.insert(QStringLiteral("youtube.com"), BlockcheckEngine::resultToJson(pass));
    backward.insert(QStringLiteral("discord.com"), BlockcheckEngine::resultToJson(dead));
    const QString two = MainWindow::blockcheckResultText(forward);
    check(two == MainWindow::blockcheckResultText(backward),
          "the sheet is the same text whatever order the results arrived in");
    check(two == QStringLiteral("Discord: FAIL\nYoutube: OK (45 ms)"),
          "one line per host, in the map's own sorted order");
}

// ------------------------------------------------------------ LanguageManager ----
//
// The catalog is compiled in rather than loaded, so these are the checks that keep it honest: that the
// seven languages exist in the order the picker shows them, that no row has a hole, that the fallback
// rules are the documented ones, that a language switch actually changes what new widgets say, and
// that the flag artwork the rail button is built from is really in the resources.
static void testLanguage()
{
    std::printf("\n[LanguageManager]\n");

    const QStringList codes = LanguageManager::codes();
    check(codes == QStringList({QStringLiteral("ru"), QStringLiteral("en"), QStringLiteral("es"),
                                QStringLiteral("de"), QStringLiteral("zh"), QStringLiteral("fr"),
                                QStringLiteral("hi")}),
          "seven languages, in menu order");
    check(LanguageManager::instance().currentCode() == QLatin1String("ru"),
          "the UI opens in Russian, the language every sheet was drawn in");

    const QStringList keys = LanguageManager::keys();
    check(keys.size() > 100, QStringLiteral("the catalog holds %1 keys").arg(keys.size()));
    check(keys.size() == QSet<QString>(keys.cbegin(), keys.cend()).size(),
          "no key is defined twice");

    int holes = 0;
    for (const QString &key : keys) {
        for (const QString &code : codes) {
            const QString text = LanguageManager::translate(code, key);
            if (text.isEmpty() || text == key)
                ++holes;
        }
    }
    check(holes == 0,
          QStringLiteral("every key has text in every language (%1 cells)").arg(keys.size() * codes.size()));

    check(LanguageManager::translate(QStringLiteral("de"), QStringLiteral("nav.settings"))
              == QStringLiteral("Einstellungen"),
          "German is German");
    check(LanguageManager::translate(QStringLiteral("zh"), QStringLiteral("nav.logs"))
              == QStringLiteral("日志"), "Chinese is Chinese");
    check(LanguageManager::nativeName(QStringLiteral("hi")).contains(QStringLiteral("हि")),
          "a language names itself in its own script");

    // The documented fallback: an unknown key says the key, never nothing.
    check(LanguageManager::tr(QStringLiteral("no.such.key.at.all"))
              == QStringLiteral("no.such.key.at.all"),
          "a missing key falls back to its own name rather than to blank");
    // A format string with its argument in one call.
    check(LanguageManager::tr(QStringLiteral("status.running"), QStringLiteral("nfqws"))
              .contains(QStringLiteral("nfqws")),
          "tr(key, arg) fills %1");

    for (const QString &code : codes) {
        QIcon flag = LanguageManager::flagIcon(code);
        check(!flag.isNull(), QStringLiteral("flag icon present for %1").arg(code));
        check(!flag.pixmap(22, 16).isNull(), QStringLiteral("flag renders for %1").arg(code));
    }

    // Switching language: one signal, and nothing for a no-op or an unsupported code.
    QSignalSpy changed(&LanguageManager::instance(), &LanguageManager::languageChanged);
    LanguageManager::instance().setCode(QStringLiteral("qq"));
    check(LanguageManager::instance().currentCode() == QLatin1String("ru"),
          "an unknown code is refused, not applied");
    check(changed.count() == 0, "and refusing one emits nothing");
    LanguageManager::instance().setCode(QStringLiteral("en"));
    check(LanguageManager::instance().currentCode() == QLatin1String("en"), "en applied");
    check(changed.count() == 1, "one signal for one real change");
    LanguageManager::instance().setCode(QStringLiteral("en"));
    check(changed.count() == 1, "and none when nothing moved");

    // A widget built after the switch speaks the new language straight out of its keys, and the keys it
    // carries are the ones retranslateUi() walks — the property is the whole contract between the two.
    {
        LogPanel panel;
        check(panel.view() != nullptr, "the log panel still builds under English");
        const auto buttons = panel.findChildren<QPushButton *>();
        int tagged = 0;
        for (QPushButton *b : buttons) {
            const QString key = b->property("i18nKey").toString();
            if (key.isEmpty())
                continue;
            ++tagged;
            check(LanguageManager::keys().contains(key),
                  QStringLiteral("log button key '%1' is in the catalog").arg(key));
            check(b->text() == LanguageManager::tr(key),
                  QStringLiteral("log button '%1' says what its key says").arg(key));
        }
        check(tagged == 3, "copy, clear and save are all tagged for retranslation");
    }

    // The walk itself, on a widget that already exists: tag-and-rewrite is what makes a language switch
    // take effect without a restart, so it is checked on the log panel's own toolbar — buttons, the
    // filter combo's entries, and the tooltips — rather than on the window, whose tree is the watchdog
    // section's business and needs a store of its own.
    {
        LogPanel panel;
        LanguageManager::instance().setCode(QStringLiteral("de"));
        const int touched = retranslateTree(&panel);
        QPushButton *copy = nullptr;
        for (QPushButton *b : panel.findChildren<QPushButton *>())
            if (b->property("i18nKey").toString() == QLatin1String("log.copy"))
                copy = b;
        check(copy && copy->text() == QStringLiteral("Kopieren"),
              "retranslateTree re-says a button in the new language");
        check(copy && copy->toolTip() == QStringLiteral("Das gesamte Protokoll in die Zwischenablage kopieren"),
              "and its tooltip");
        QComboBox *filter = panel.findChild<QComboBox *>();
        check(filter && filter->itemText(0) == QStringLiteral("Alle")
                  && filter->itemText(3) == QStringLiteral("Fehler"),
              "combo entries follow, in index order");
        check(filter && filter->itemData(3).toInt() == LogPanel::Error,
              "and the value behind an entry is untouched by its label");
        check(touched >= 8, QStringLiteral("the walk rewrote %1 tagged strings").arg(touched));
        LanguageManager::instance().setCode(QStringLiteral("ru"));
        retranslateTree(&panel);
        check(copy->text() == QStringLiteral("Копировать"),
              "switching back re-says them again");
    }

    // The persisted choice: the default, and a value that survives the object that wrote it.
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("language-test.ini"));
    {
        Settings s(file);
        check(s.language() == QLatin1String("ru"), "an unset language is Russian");
        s.setLanguage(QStringLiteral("fr"));
    }
    {
        Settings s(file);
        check(s.language() == QLatin1String("fr"), "the chosen language is stored");
    }

    // Back to the language the rest of this suite reads the chrome in.
    LanguageManager::instance().setCode(QStringLiteral("ru"));
    check(LanguageManager::tr(QStringLiteral("nav.logs")) == QStringLiteral("Логи"),
          "restoring ru restores the Russian chrome");
}

// ------------------------------------------------------------------- Theme ----
static void testTheme(QApplication &app)
{
    std::printf("\n[Theme]\n");

    Theme::apply(app, true);
    check(Theme::isDark(), "dark mode is active");
    check(!app.styleSheet().isEmpty(), "dark stylesheet loaded from resources");
    check(app.styleSheet().contains(QStringLiteral("#C084FC")), "accent colour present in the sheet");
    check(app.styleSheet().contains(QStringLiteral("#0B0C10")), "dashboard background present in the sheet");
    check(app.styleSheet().contains(QStringLiteral("profileCard")), "profile row rules present");
    check(app.styleSheet().contains(QStringLiteral("settingsPage")), "settings page rules present");
    check(app.styleSheet().contains(QStringLiteral("windowButton")), "window chrome rules present");
    // The rail and the page stack are the window's skeleton: if their rules go missing, the nav buttons
    // render as default raised buttons and the pages composite onto black.
    check(app.styleSheet().contains(QStringLiteral("navButton")), "rail button rules present");
    check(app.styleSheet().contains(QStringLiteral("pageStack")), "page stack rules present");
    // The dashboard redesign's own names. Each is a widget MainWindow paints or lays out by objectName,
    // and an unstyled one is a visible regression: #mainArea/#footerBar composite onto black, #appTitle
    // loses its 24 px weight, #statusDot loses the border-radius that makes it a dot at all.
    for (const QString &name : {QStringLiteral("mainArea"), QStringLiteral("appTitle"),
                                QStringLiteral("statusDot"), QStringLiteral("statusText"),
                                QStringLiteral("langButton"), QStringLiteral("footerBar"),
                                QStringLiteral("pingNumber"), QStringLiteral("pingLabel")}) {
        check(app.styleSheet().contains(name),
              QStringLiteral("dashboard rule present: %1").arg(name));
    }
    // The two neon values the C++ painters use must also be in the sheet, or the stylesheet and the
    // hand-painted circles drift apart into two greens and two greys.
    check(app.styleSheet().contains(QStringLiteral("#10B981")), "the sheet's live green is the painted one");
    check(app.styleSheet().contains(QStringLiteral("#9CA3AF")), "the sheet's badge grey is the painted one");
    check(app.palette().color(QPalette::Window) == Theme::windowBackground(), "palette matches tokens");

    Theme::apply(app, false);
    check(!Theme::isDark(), "light mode is active");
    check(app.styleSheet().contains(QStringLiteral("#F3F4F6")), "light background in the sheet");

    // Read one declaration out of one selector's rule. Needed because "does the sheet contain this hex"
    // is a much weaker question than it looks: #1F2937 is the light sheet's general text colour as well
    // as its old log colour, so a whole-sheet search stays green after the log rule alone stops
    // matching the token the code paints with. Scoping to the selector is what makes the check a check.
    auto ruleProperty = [](const QString &sheet, const QString &selector, const QString &prop) {
        const int at = sheet.indexOf(selector);
        const int open = at < 0 ? -1 : sheet.indexOf(QLatin1Char('{'), at);
        const int close = open < 0 ? -1 : sheet.indexOf(QLatin1Char('}'), open);
        if (close < 0)
            return QString();
        const QStringList decls = sheet.mid(open + 1, close - open - 1).split(QLatin1Char(';'));
        for (const QString &d : decls) {
            const int colon = d.indexOf(QLatin1Char(':'));
            if (colon > 0 && d.left(colon).trimmed().compare(prop, Qt::CaseInsensitive) == 0)
                return d.mid(colon + 1).trimmed();
        }
        return QString();
    };

    // Both sheets must carry the rules the dynamic properties switch on, or a badge/notice would render
    // as unstyled default text.
    for (bool dark : {true, false}) {
        Theme::apply(app, dark);
        const QString sheet = app.styleSheet();
        check(sheet.contains(QStringLiteral("[error=\"true\"]")),
              QStringLiteral("%1 sheet styles a failed profile row").arg(dark ? QStringLiteral("dark")
                                                                              : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("selected=\"true\"")),
              QStringLiteral("%1 sheet styles the selected row").arg(dark ? QStringLiteral("dark")
                                                                         : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("miniPing")),
              QStringLiteral("%1 sheet styles the row's ping chips").arg(dark ? QStringLiteral("dark")
                                                                              : QStringLiteral("light")));
        // The ring's colour is the verdict, and the verdict only reaches QSS through the token the row
        // stamps — a sheet without these rules draws every ring the same grey, whatever the service said.
        check(sheet.contains(QStringLiteral("miniPing[pingStatus=\"ok\"]")),
              QStringLiteral("%1 sheet colours a live ring").arg(dark ? QStringLiteral("dark")
                                                                     : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("miniPing[pingStatus=\"fail\"]")),
              QStringLiteral("%1 sheet colours a failed ring").arg(dark ? QStringLiteral("dark")
                                                                       : QStringLiteral("light")));
        // The row's menu opens on press, so the style's own arrow is a second affordance for one action.
        check(sheet.contains(QStringLiteral("#cardMenu::menu-indicator")),
              QStringLiteral("%1 sheet hides the row menu's arrow").arg(dark ? QStringLiteral("dark")
                                                                            : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("savedNote")),
              QStringLiteral("%1 sheet colours the save acknowledgement").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("trayNote")),
              QStringLiteral("%1 sheet colours the tray notice").arg(dark ? QStringLiteral("dark")
                                                                         : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("navButton")),
              QStringLiteral("%1 sheet styles the rail").arg(dark ? QStringLiteral("dark")
                                                                 : QStringLiteral("light")));
        // The dashboard's painted circles read their colours from C++ tokens, but everything framing them
        // is sheet styling — and both sheets must carry it, or the light theme shows an unstyled rail.
        for (const QString &name : {QStringLiteral("mainArea"), QStringLiteral("statusDot"),
                                    QStringLiteral("langButton"), QStringLiteral("pingLabel")}) {
            check(sheet.contains(name),
                  QStringLiteral("%1 sheet styles %2")
                      .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"), name));
        }
        // The unread-journal alert is a dynamic property like [error], and its colour has the same two
        // sources as the accent: the rail label reads it off the sheet, while the badge glyph on a failed
        // card is tinted with Theme::error() in C++. One red, or the tab and the card disagree.
        check(sheet.contains(QStringLiteral("[alert=\"true\"]")),
              QStringLiteral("%1 sheet styles the unread-journal alert").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        check(sheet.contains(Theme::error().name(QColor::HexRgb).toUpper()),
              QStringLiteral("%1 sheet's alert red is the red the code paints with").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // The accent is painted by C++ code (icons, the window icon, QPalette::Highlight) *and* by the
        // sheet (buttons, borders, the checked tab). Those are one design, so the sheet must contain
        // the very hex the accessor returns — otherwise a code-drawn glyph drifts from the button
        // beside it, which is the quietest possible way for a theme to break.
        check(sheet.contains(Theme::accent().name(QColor::HexRgb).toUpper()),
              QStringLiteral("%1 sheet carries the accent the code paints with").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // The journal's text colour has the accent's two sources too: the sheet paints the
        // QPlainTextEdit, and every line the log appends is formatted with Theme::logColor(0). A sheet
        // left on an older hex gives the frame one colour and its contents another, which reads as a
        // rendering glitch rather than a broken token. Scoped to the rule, not searched sheet-wide —
        // see ruleProperty above.
        const QString logHex =
            ruleProperty(sheet, QStringLiteral("QPlainTextEdit#logView"), QStringLiteral("color"));
        check(logHex.compare(Theme::logColor(0).name(QColor::HexRgb), Qt::CaseInsensitive) == 0,
              QStringLiteral("%1 sheet's logView colour (%2) is the colour the code writes log lines "
                             "in (%3)")
                  .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"),
                       logHex.isEmpty() ? QStringLiteral("<none>") : logHex,
                       Theme::logColor(0).name(QColor::HexRgb)));
        // The dashboard's service mark is baked into a pixmap by PingIndicator::refreshGlyph(), so no
        // sheet rule can rescue it once the theme changes — it has to be the right colour at bake time.
        // The failure this guards is not "slightly low contrast" but the mark vanishing: white on the
        // light sheet's #FFFFFF page is nothing at all. Distance from the surface it lands on, rather
        // than mere inequality, is what a glyph actually needs to be seen.
        const QColor mark = Theme::markOnRing();
        const int contrast = qAbs(qGray(mark.rgb()) - qGray(Theme::surface().rgb()));
        check(contrast > 100,
              QStringLiteral("%1 sheet's ring mark stands off its page by %2 gray levels")
                  .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                  .arg(contrast));
        check(dark ? mark == QColor(Qt::white) : mark.lightness() < 128,
              QStringLiteral("%1 sheet's ring mark is the white-on-dark / dark-on-white the page "
                             "behind it requires").arg(dark ? QStringLiteral("dark")
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
                                QStringLiteral("ping-wait"), QStringLiteral("error"),
                                // The rail's own set: brand, the two pages and the foot action.
                                QStringLiteral("home"), QStringLiteral("logs"),
                                QStringLiteral("power"),
                                // The dashboard's set: a ping badge's clock and the marks inside its ring.
                                QStringLiteral("clock"), QStringLiteral("globe"),
                                QStringLiteral("youtube"), QStringLiteral("discord"),
                                // The profile row's set: its menu handle, and the row menu's two glyphs.
                                QStringLiteral("more"), QStringLiteral("cards"),
                                // The frameless window's own chrome.
                                QStringLiteral("window-close"), QStringLiteral("window-minimize"),
                                QStringLiteral("ping-test")};
    for (const QString &g : glyphs)
        check(QFile::exists(QStringLiteral(":/icons/%1.svg").arg(g)),
              QStringLiteral("icon file exists: %1").arg(g));

    // The chrome and menu glyphs are thin strokes drawn for 16 px; a glyph that only "exists" but fails to
    // rasterise (a malformed path, a size that never got its currentColor) shows up as a hole in the header
    // or the row, so they are loaded through Theme::icon() exactly as the window draws them.
    Theme::apply(app, true);
    for (const QString &g : {QStringLiteral("more"), QStringLiteral("window-close"),
                             QStringLiteral("window-minimize")}) {
        const QPixmap pm = Theme::icon(g, Theme::iconColor(), 16).pixmap(16, 16);
        check(!pm.isNull(), QStringLiteral("chrome glyph rasterises: %1").arg(g));
        bool inked = false;
        if (!pm.isNull()) {
            const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < img.height() && !inked; ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qAlpha(img.pixel(x, y)) > 40) { inked = true; break; }
        }
        check(inked, QStringLiteral("chrome glyph has ink: %1").arg(g));
    }

    // The rail draws each glyph at 20 px through Theme::icon(), which rewrites the SVG's declared
    // width/height and its currentColor. A glyph that breaks that contract — a hardcoded fill, a
    // missing size — still "exists" as a file and shows up as a grey hole in the sidebar, so existence
    // alone is not enough for the ones the redesign added.
    Theme::apply(app, true);
    for (const QString &g : {QStringLiteral("home"), QStringLiteral("logs"), QStringLiteral("power"),
                             QStringLiteral("app"), QStringLiteral("clock"), QStringLiteral("globe"),
                             QStringLiteral("youtube"), QStringLiteral("discord")}) {
        const QPixmap pm = Theme::icon(g, Theme::accent(), 20).pixmap(20, 20);
        check(!pm.isNull(), QStringLiteral("rail glyph rasterises: %1").arg(g));
        bool inked = false;
        if (!pm.isNull()) {
            const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < img.height() && !inked; ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qAlpha(img.pixel(x, y)) > 40) { inked = true; break; }
        }
        check(inked, QStringLiteral("rail glyph has ink: %1").arg(g));
    }

    // The brand marks are filled paths whose play triangle / eyes are cut out by fill-rule, so a ring's
    // 22 px white render must have holes in it as well as ink — a solid blob is the same size as the
    // mark and reads as an unrelated circle at a glance.
    {
        const QPixmap pm = Theme::icon(QStringLiteral("youtube"), QColor(Qt::white), 22).pixmap(22, 22);
        check(!pm.isNull(), "the youtube mark rasterises at ring size");
        int inked = 0, empty = 0;
        if (!pm.isNull()) {
            const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    if (qAlpha(img.pixel(x, y)) > 40)
                        ++inked;
                    else
                        ++empty;
                }
            }
        }
        check(inked > 0 && empty > 0, "the mark is drawn, and its play triangle is cut out");
    }

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

// ------------------------------------------------------------------- Watchdog ----
//
// The quiet bypass check: a timer asks one host whether the live profile still gets through, and a FAIL
// turns into a red journal line, a balloon and an offer to switch. The deciding half is pure and is
// checked as such; the acting half is checked on a real window, because what it does depends on the
// catalog, on the row the power button points at, and on the journal that has to say it out loud.
//
// Three things in that path need something a test suite must not have — a network (the engine's probe),
// a desktop (the modal), a root prompt (the backend's stop/start) — and all three are MainWindow's
// virtual seams, overridden below. Everything between them is the production code.

namespace {

// A verdict map in the engine's own JSON shape, so the watchdog is handed exactly what testFinished
// would hand it: one entry per host, `success` the only field the decision reads.
QJsonObject watchdogVerdicts(const QStringList &hosts, const QStringList &through)
{
    QJsonObject out;
    for (const QString &host : hosts) {
        HostCheckResult r;
        r.success = through.contains(host);
        r.attempts = 3;
        r.attemptsOk = r.success ? 3 : 0;
        r.avgLatency = r.success ? 40 : -1;
        r.ttfbMs = r.success ? 12 : -1;
        r.lastError = r.success ? QString() : QStringLiteral("connection timed out");
        out.insert(host, BlockcheckEngine::resultToJson(r));
    }
    return out;
}

// The row id the window uses for a strategy: the parser's name when it has one, the file when it does
// not. The same expression populateCards() and watchdogNextStrategyId() use.
QString watchdogRowId(const Strategy &s)
{
    return s.id.isEmpty() ? s.filePath : s.id;
}

// A linux tree the parser names two profiles in: the sysv/openrc init script — which survives the scan
// only because <root>/config exists, since an unconfigured tree loses its InitScript card — and the
// portable nfqws payload synthesised from binaries/<arch>/nfqws. Two rows is the point: with one profile
// in the list there is nothing to rotate to, and the whole prompt path would be unreachable.
bool makeWatchdogTree(const QString &root)
{
    const QString initBody = QStringLiteral(
        "#!/bin/sh\ncase \"$1\" in\n  start) echo starting ;;\n  stop) echo stopping ;;\nesac\n");
    bool ok = true;
    ok &= writeTextFile(root + QLatin1String("/init.d/sysv/zapret"), initBody);
    ok &= writeTextFile(root + QLatin1String("/init.d/openrc/zapret"), initBody);
    ok &= writeTextFile(root + QLatin1String("/config.default"),
                        QStringLiteral("#!/bin/sh\n# options\n"));
    ok &= writeTextFile(root + QLatin1String("/config"), QStringLiteral("MODE_FILTER=none\n"));
    const QString arch = ZapretPaths::nativeArchDirName();
    ok &= writeTextFile(root + QLatin1String("/binaries/")
                            + (arch.isEmpty() ? QStringLiteral("linux-x86_64") : arch)
                            + QLatin1String("/nfqws"),
                        QStringLiteral("fixture payload\n"));
    return ok;
}

// The window under test: the same class, with its three answers replaced.
class WatchdogProbe : public MainWindow
{
public:
    using MainWindow::MainWindow;

    int probes = 0;
    int confirms = 0;
    bool answerYes = true;
    QStringList confirmPairs;   // "from|next", one per question asked
    QStringList performed;      // "from|next", one per rotation actually carried out

protected:
    void startWatchdogProbe(const Strategy &strategy) override
    {
        ++probes;
        // Not the base: the base drives the shared engine, which would bring a profile up through
        // pkexec and probe the real internet from a test binary. The verdict is fed to
        // onWatchdogResult() by hand instead, which is where the base's cycle would have landed.
    }

    bool confirmWatchdogRotation(const QString &fromName, const QString &nextName) override
    {
        ++confirms;
        confirmPairs << (fromName + QLatin1Char('|') + nextName);
        return answerYes;   // no modal anywhere in this suite — nothing out here could answer one
    }

    void performWatchdogRotation(const QString &fromId, const QString &nextId) override
    {
        performed << (fromId + QLatin1Char('|') + nextId);
        // Again not the base: the real act stops a live payload and starts another, both through the
        // backend. The intent it replaces is what watchdogRotationRequested has already recorded.
    }
};

} // namespace

static void testWatchdog()
{
    std::printf("\n[Watchdog]\n");

    using S = IZapretBackend::State;

    // ---- the one host, and the cadence ----
    const QString quick = MainWindow::watchdogQuickHost();
    check(quick == QLatin1String("discord.com"), "the quiet check probes one quick host");
    check(MainWindow::profilePingHosts().contains(quick),
          "and it is a service the profile rows promise to unblock");
    check(quick != QLatin1String("google.com"),
          "…not the baseline, which answers happily with no bypass at all");

    check(MainWindow::watchdogIntervalMs(5) == 300000, "the default five minutes is five minutes");
    check(MainWindow::watchdogIntervalMs(Settings::kWatchdogDefaultIntervalMin) == 300000,
          "and the window's default is the model's default, spelled once");
    check(MainWindow::watchdogIntervalMs(1) == 60000, "the floor is a minute");
    check(MainWindow::watchdogIntervalMs(60) == 3600000, "the ceiling is an hour");
    check(MainWindow::watchdogIntervalMs(0) == 60000, "a stored zero is not a timer that never rests");
    check(MainWindow::watchdogIntervalMs(-9) == 60000, "nor is a negative one");
    check(MainWindow::watchdogIntervalMs(100000) == 3600000,
          "and a hand-edited huge value is an hour, not a check that never happens");

    // ---- when a tick may measure ----
    check(MainWindow::watchdogShouldProbe(S::Running, false, false), "a live run is re-checked");
    check(!MainWindow::watchdogShouldProbe(S::Running, true, false),
          "a tick does not stack on the quiet cycle it started");
    check(!MainWindow::watchdogShouldProbe(S::Running, false, true),
          "…nor on the round the user pressed");
    check(!MainWindow::watchdogShouldProbe(S::Stopped, false, false),
          "a stopped profile has nothing for the answer to describe");
    check(!MainWindow::watchdogShouldProbe(S::Starting, false, false),
          "a launch in flight has opened nothing yet");
    check(!MainWindow::watchdogShouldProbe(S::Stopping, false, false),
          "and a teardown is closing the path it would measure");
    check(!MainWindow::watchdogShouldProbe(S::Error, false, false),
          "a failed run is not a live one");

    // ---- what counts as dead ----
    const QStringList one{quick};
    check(MainWindow::watchdogCycleFailed(watchdogVerdicts(one, {})),
          "no attempt through on any host is the FAIL the watchdog exists for");
    check(!MainWindow::watchdogCycleFailed(watchdogVerdicts(one, one)),
          "the same host getting through is not");
    check(!MainWindow::watchdogCycleFailed(QJsonObject()),
          "an empty map is a broken measurement, not a dead bypass");
    const QStringList two{quick, QStringLiteral("youtube.com")};
    check(!MainWindow::watchdogCycleFailed(watchdogVerdicts(two, {QStringLiteral("youtube.com")})),
          "one living host out of two is the bypass working");
    QJsonObject bare;
    bare.insert(quick, QJsonObject{{QStringLiteral("latency"), 30}});
    check(MainWindow::watchdogCycleFailed(bare), "a verdict with no success key is not a pass");

    // ---- which profile comes next ----
    QList<Strategy> list;
    for (const char *name : {"A", "B", "C"}) {
        Strategy s;
        s.id = QLatin1String(name);
        s.name = QLatin1String(name);
        list.append(s);
    }
    Strategy unnamed;
    unnamed.filePath = QStringLiteral("/opt/zapret/linux/no-name.sh");
    QList<Strategy> withUnnamed = list;
    withUnnamed << unnamed;

    check(MainWindow::watchdogNextStrategyId(list, QStringLiteral("A")) == QLatin1String("B"),
          "the next row in the list is the one offered");
    check(MainWindow::watchdogNextStrategyId(list, QStringLiteral("B")) == QLatin1String("C"),
          "…and the one after that, the time after");
    check(MainWindow::watchdogNextStrategyId(list, QStringLiteral("C")) == QLatin1String("A"),
          "the list wraps instead of running out");
    check(MainWindow::watchdogNextStrategyId(list, QStringLiteral("Z")) == QLatin1String("A"),
          "a run that names nothing on the list gets the first row");
    check(MainWindow::watchdogNextStrategyId(list, QString()) == QLatin1String("A"),
          "…and so does a run with no name at all");
    check(MainWindow::watchdogNextStrategyId(withUnnamed, QStringLiteral("C")) == unnamed.filePath,
          "a row the parser could not name is addressed by its path");
    check(MainWindow::watchdogNextStrategyId(QList<Strategy>{list.first()}, QStringLiteral("A"))
              .isEmpty(),
          "one profile in the list is not a rotation");
    check(MainWindow::watchdogNextStrategyId(QList<Strategy>{}, QStringLiteral("A")).isEmpty(),
          "nor is an empty list");

    // ---- the acting half, on a real window ----
    //
    // A window builds its own Settings from the process identity, so this section runs only where the
    // harness has redirected the config location into the build tree (CMake sets XDG_CONFIG_HOME to
    // <build>/test-home/config for uitest). Requiring the path to sit under the binary's own directory
    // is what makes that safe rather than merely likely: a developer running the suite from a shell
    // where XDG_CONFIG_HOME is just ~/.config has that variable set, and a check against it alone would
    // let the test write into their real config home. Anywhere else it skips.
    const QString buildTree = QCoreApplication::applicationDirPath();
    const QString xdgConfig = qEnvironmentVariable("XDG_CONFIG_HOME");
    const QString configBase =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QDir().mkpath(xdgConfig);   // the store's directory is created on its first write anyway
    const QString canonicalXdg = QDir(xdgConfig).canonicalPath();
    const QString canonicalBuild = QDir(buildTree).canonicalPath();
    const bool redirected = !xdgConfig.isEmpty() && !canonicalXdg.isEmpty()
                            && QDir::cleanPath(configBase) == QDir::cleanPath(xdgConfig)
                            && canonicalXdg.startsWith(canonicalBuild + QLatin1Char('/'));
    if (!redirected) {
        std::printf("  SKIP  the window section needs XDG_CONFIG_HOME redirected into the build tree "
                    "(run it through ctest)\n");
        return;
    }

    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory for the fixture tree");
    const QString root = dir.filePath(QStringLiteral("zapret/linux"));
    check(makeWatchdogTree(root), "a two-profile linux tree written");

    Settings store;   // the same store a MainWindow constructed here resolves to
    const QString savedPath = store.zapretPath();
    const QString savedLast = store.lastStrategyId();
    const bool savedStart = store.startLastOnLaunch();
    const int savedInterval = store.watchdogIntervalMin();

    store.setStartLastOnLaunch(false);   // nothing may start itself while the window is being built
    store.setLastStrategyId(QString());
    store.setWatchdogIntervalMin(7);     // a cadence only this suite sees, so the timer is checkable
    store.setZapretPath(root);

    {
        WatchdogProbe window;
        const QList<Strategy> catalog = window.manager()->strategies();
        check(catalog.size() >= 2,
              QStringLiteral("the fixture tree gives the window %1 profiles").arg(catalog.size()));
        check(window.cardCount() >= 2, "and draws a row for each of them");

        check(window.watchdogTimerActive(), "the watchdog is armed the moment the window exists");
        check(window.watchdogTimerInterval() == 7 * 60 * 1000,
              "and it beats on the stored cadence, not on a literal in the window");

        // The other half of startWatchdog()'s rule: Settings::changed() answers every write, and the
        // one a start makes must not push the next check further out. Written through the window's own
        // model — which is the same object SettingsPage holds a reference to — because that is the only
        // way the application ever changes a setting.
        window.settingsModel()->setLastStrategyId(QStringLiteral("an-unrelated-write"));
        check(window.watchdogTimerInterval() == 7 * 60 * 1000,
              "a write that did not move the interval leaves the countdown alone");
        window.settingsModel()->setWatchdogIntervalMin(20);
        check(window.watchdogTimerInterval() == 20 * 60 * 1000,
              "a new interval re-arms the timer with the new one");

        // A tick on a stopped window must not reach the engine at all.
        window.onWatchdogTick();
        check(window.probes == 0, "a stopped window asks the engine for nothing");

        // ---- the dead bypass, answered yes ----
        QSignalSpy rotation(&window, &MainWindow::watchdogRotationRequested);
        const QString fromId = watchdogRowId(catalog.first());   // what the window points at idle
        const QString nextId = MainWindow::watchdogNextStrategyId(catalog, fromId);
        check(!nextId.isEmpty(), "the fixture list has something to rotate to");
        QString nextName;
        for (const Strategy &s : catalog)
            if (watchdogRowId(s) == nextId)
                nextName = s.name;

        const int before = window.logPanel()->storedLineCount();
        window.onWatchdogResult(watchdogVerdicts(one, {}));
        const QString alert = LanguageManager::tr(QStringLiteral("watchdog.alert"));
        const QString rotating = LanguageManager::tr(QStringLiteral("watchdog.rotating")).arg(nextName);

        check(window.logPanel()->storedLineCount() == before + 2,
              "one dead check costs the journal two lines — the alert, and the switch");
        check(!alert.contains(QStringLiteral("%1")), "the alert sentence needs no arguments");
        QTextBlock hit, moved;
        QTextDocument *doc = window.logPanel()->view()->document();
        for (QTextBlock b = doc->firstBlock(); b.isValid(); b = b.next()) {
            if (b.text().contains(alert))
                hit = b;
            if (b.text().contains(rotating))
                moved = b;
        }
        check(hit.isValid(), "the alert is in the journal, in the current language");
        check(hit.isValid() && firstForegroundColor(hit) == Theme::error().name(QColor::HexRgb),
              "and it is the red line, not an info row");
        check(moved.isValid(), "and the switch names the profile it is making");

        check(window.confirms == 1, "the user is asked once, not twice for one verdict");
        check(window.confirmPairs.size() == 1
                  && window.confirmPairs.first()
                         == catalog.first().name + QLatin1Char('|') + nextName,
              "the question names the profile that died and the one being offered");
        check(rotation.count() == 1, "a yes is recorded as the intent to switch profiles");
        if (rotation.count() == 1) {
            check(rotation.at(0).size() == 2, "…carried as the pair of ids");
            check(rotation.at(0).at(0).toString() == fromId, "from the profile whose check failed");
            check(rotation.at(0).at(1).toString() == nextId, "to the next row in the list");
        }
        check(window.performed.size() == 1 && window.performed.first()
                                        == fromId + QLatin1Char('|') + nextId,
              "and the act is asked for exactly the pair the intent named");

        // ---- the same verdict, answered no ----
        window.answerYes = false;
        const int askedBefore = window.logPanel()->storedLineCount();
        window.onWatchdogResult(watchdogVerdicts(one, {}));
        check(window.confirms == 2, "a second dead check asks again");
        check(rotation.count() == 1, "a no is not a switch: nothing more was requested");
        check(window.performed.size() == 1, "…and nothing was performed");
        check(window.logPanel()->storedLineCount() == askedBefore + 1,
              "the alert is still given when the answer is no");

        // ---- a pass, and a cycle that produced nothing: silence ----
        const int silentBefore = window.logPanel()->storedLineCount();
        window.onWatchdogResult(watchdogVerdicts(one, one));
        window.onWatchdogResult(QJsonObject());
        check(window.logPanel()->storedLineCount() == silentBefore,
              "a passing check and a broken one leave the journal untouched");
        check(window.confirms == 2, "neither asks the user anything");
        check(rotation.count() == 1, "…and neither switches a profile");

        store.setLastStrategyId(QString());   // do not leave the fixture's route to a later run
    }

    store.setZapretPath(savedPath);
    store.setLastStrategyId(savedLast);
    store.setStartLastOnLaunch(savedStart);
    store.setWatchdogIntervalMin(savedInterval);
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
    testPowerButton();
    testProfileCard();
    testTrayPolicy();
    testWindowPolicy();
    testLanguage();
    testTheme(app);
    testPingService();
    testDownloadDialog();
    testWatchdog();

    std::printf("\n=== %s (%d failures) ===\n", g_failures ? "FAILED" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
