// uitest — headless widget + service verification for the zapret GUI layer.
//
// Runs under the offscreen QPA platform (QT_QPA_PLATFORM is also forced in-process, so the binary is
// runnable straight from a shell). Like coretest it is a plain console program: readable PASS/FAIL
// lines, non-zero exit on any failure, no QTest data-driven scaffolding.
//
// Deliberately NOT covered here: anything that would spawn pkexec or winws, and real network
// reachability (the ping test only asserts that a verdict arrives). DownloadDialog is constructed but
// never started: startFetch() is the only thing in it that touches GitHub, and a test suite must not
// depend on that answer. UpdateChecker::check() is the release feed's one socket and is likewise never
// called — the badge section hands the window the answer it would have carried instead.
//
// A constructed MainWindow is covered in three places: the watchdog section, which needs the real catalog
// and journal to prove that a dead bypass turns into an offer; the release-badge section, which needs the
// header the badge lives in; and the rounded-shell section, which needs the window's own pixels. All are
// built without a window manager (offscreen) — the watchdog's three seams that would need a desktop, a
// network or a root prompt are overridden — and all read the redirected test identity, skipping themselves
// when the harness has not redirected it. Neither the watchdog nor the badge section asks the release feed
// anything: the GitHub lookup is armed by main.cpp, which is not part of this binary, so a window here
// cannot reach the network however the suite drives it. Motion is the one thing in the suite that has to
// put a window on screen: a page behind another page has nothing to appear on, so a fade can only be
// watched on a visible window — and so can a badge that breathes, and so can the corners of the window
// itself, which the shell section reads out of a rendered grab.

#include "BlockcheckEngine.h"
#include "IZapretBackend.h"
#include "DownloadDialog.h"
#include "HistoryPage.h"
#include "HistoryStore.h"
#include "LanguageManager.h"
#include "LogPanel.h"
#include "MainWindow.h"
#include "Motion.h"
#include "PingIndicator.h"
#include "PingService.h"
#include "PowerButton.h"
#include "ProfileCard.h"
#include "Settings.h"
#include "SettingsPage.h"
#include "Strategy.h"
#include "Theme.h"
#include "UpdateChecker.h"
#include "ZapretDownloader.h"
#include "ZapretPaths.h"

#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QAbstractItemModel>
#include <QColor>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTableView>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
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

// The sheet with every /* … */ block removed.
//
// Both files name selectors inside their comments to explain what a rule is for ("a plain
// `QMainWindow { … }` rule would match this window too", "the surfaces that reach a corner are
// QFrame#sidebar …"). A scan that looks for a selector's first match would then read that prose as the
// declaration and return the body of whatever rule happens to follow the sentence — so every structural
// read of a sheet goes through this first. Comment-free text is where a declaration can only ever live.
static QString sheetWithoutComments(const QString &sheet)
{
    QString code;
    code.reserve(sheet.size());
    bool inComment = false;
    for (int i = 0; i < sheet.size(); ++i) {
        if (!inComment && sheet.at(i) == QLatin1Char('/') && i + 1 < sheet.size()
            && sheet.at(i + 1) == QLatin1Char('*')) {
            inComment = true;
            ++i;
            continue;
        }
        if (inComment && sheet.at(i) == QLatin1Char('*') && i + 1 < sheet.size()
            && sheet.at(i + 1) == QLatin1Char('/')) {
            inComment = false;
            ++i;
            continue;
        }
        if (!inComment)
            code.append(sheet.at(i));
    }
    return code;
}

// True when some rule in `code` names QMainWindow as a bare TYPE (`QMainWindow {`, `QMainWindow, QDialog`).
// That is the shape of the bug the rounded shell dies of: a background on the window is an opaque
// rectangle over every pixel of a WA_TranslucentBackground surface, corners included, and the rounded card
// under it is a rounding nobody can see. Takes comment-free text, so a comment may name the type freely.
static bool hasUnqualifiedQMainWindowRule(const QString &code)
{
    static const QLatin1String type("QMainWindow");
    int at = code.indexOf(type);
    while (at >= 0) {
        const int after = at + type.size();
        const QChar next = after < code.size() ? code.at(after) : QLatin1Char(' ');
        if (!next.isLetterOrNumber() && next != QLatin1Char('_') && next != QLatin1Char('#'))
            return true;                       // not `QMainWindow#mainWindow` — the type itself
        at = code.indexOf(type, after);
    }
    return false;
}

// The colour one rule of one sheet declares, out of comment-free text (see sheetWithoutComments), or an
// invalid colour when nothing matches. Kept next to the helpers that read a sheet rather than a pixel
// because every check that wants to know what the SHEET says goes through it: hardcoding the same hexes
// here would be a test of this file, not of a theme.
static QColor sheetRuleColor(const QString &sheet, const QString &selector, const QString &prop)
{
    const QString code = sheetWithoutComments(sheet);
    const int at = code.indexOf(selector);
    const int open = at < 0 ? -1 : code.indexOf(QLatin1Char('{'), at);
    const int close = open < 0 ? -1 : code.indexOf(QLatin1Char('}'), open);
    if (close < 0)
        return QColor();
    const QStringList decls = code.mid(open + 1, close - open - 1).split(QLatin1Char(';'));
    for (const QString &d : decls) {
        const int colon = d.indexOf(QLatin1Char(':'));
        if (colon > 0 && d.left(colon).trimmed().compare(prop, Qt::CaseInsensitive) == 0)
            return QColor(d.mid(colon + 1).trimmed());
    }
    return QColor();
}

// The first hex inside one declaration of one rule — the form a border takes ("1px solid #262A36"), where
// the colour is not the whole value and sheetRuleColor would hand back an unparseable string.
static QColor sheetRuleHex(const QString &sheet, const QString &selector, const QString &prop)
{
    const QString code = sheetWithoutComments(sheet);
    const int at = code.indexOf(selector);
    const int open = at < 0 ? -1 : code.indexOf(QLatin1Char('{'), at);
    const int close = open < 0 ? -1 : code.indexOf(QLatin1Char('}'), open);
    if (close < 0)
        return QColor();
    const QString body = code.mid(open + 1, close - open - 1);
    for (const QString &d : body.split(QLatin1Char(';'))) {
        const int colon = d.indexOf(QLatin1Char(':'));
        if (colon <= 0 || d.left(colon).trimmed().compare(prop, Qt::CaseInsensitive) != 0)
            continue;
        const QRegularExpression hex(QStringLiteral("#[0-9a-fA-F]{6}"));
        const QRegularExpressionMatch m = hex.match(d.mid(colon + 1));
        if (m.hasMatch())
            return QColor(m.captured(0));
    }
    return QColor();
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
        check(s.animationsEnabled() == true, "default animationsEnabled is true");
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
        s.setAnimationsEnabled(false);
        s.setWatchdogIntervalMin(15);
        s.setTheme(QStringLiteral("system"));
        check(changed.count() == 7, "each setter emits changed()");

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
        check(reopened.animationsEnabled() == false, "animationsEnabled round-trip");
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
// True if `w`'s polished font is actually fixed-pitch — an 'i' advancing the same as a 'W'. This tests the
// rendering property (tabular figures that do not shift) rather than a family NAME, so it holds whatever
// QFontDatabase resolves the system's fixed face to, and it catches the real bug: a monospaced face silently
// replaced by the proportional UI family via the generic `QWidget { font-family }` sheet rule.
static bool resolvesMonospace(QWidget *w)
{
    if (!w)
        return false;
    w->ensurePolished();
    const QFontMetrics fm(w->font());
    return fm.horizontalAdvance(QLatin1Char('i')) == fm.horizontalAdvance(QLatin1Char('W'))
        && fm.horizontalAdvance(QLatin1Char('l')) == fm.horizontalAdvance(QLatin1Char('M'));
}

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

    // The latency figure is monospace WHERE IT RENDERS, not merely requested. The generic
    // `QWidget { font-family: <UI family> }` rule in the app sheet outranks a C++ setFamily(), so a bare
    // setFont() left the figure proportional and its digits shifted as they changed — this is the regression
    // Theme::applyFontViaSheet() exists to stop. It runs against the real applied sheet (main() did
    // Theme::apply first), so it is exactly what the widget shows on screen.
    QLabel *number = chip.findChild<QLabel *>(QStringLiteral("pingNumber"));
    check(number != nullptr, "the badge carries a #pingNumber figure");
    check(resolvesMonospace(number), "and the figure resolves to a fixed-pitch face (no digit drift)");
    // Guard that the check above means something: the app's own UI family must be proportional, or the
    // monospace test would pass on a font that never got overridden in the first place.
    QLabel uiSample;
    uiSample.ensurePolished();
    const QFontMetrics fmUi(uiSample.font());
    check(fmUi.horizontalAdvance(QLatin1Char('i')) != fmUi.horizontalAdvance(QLatin1Char('W')),
          "the UI text family really is proportional, so the monospace check discriminates");
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

    button.show();
    QTest::qWait(10);        // a control nobody has put on screen is hidden, and nothing animates on it

    button.setMode(PowerButton::Mode::Busy);
    check(button.mode() == PowerButton::Mode::Busy, "mode is readable back");
    QTest::qWait(300);
    check(button.glow() > 0.0 && button.glow() < 1.0,
          "the busy pulse is actually moving the halo, not merely started");
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

    // ---- the chip text inherits the theme's proportional face ----
    // A chip can spell a word ("таймаут"), not only digits, so unlike the dashboard's number it is
    // deliberately NOT pinned to a fixed-pitch face: the generic `QWidget { font-family }` rule must
    // win here. The pinning machinery stays proven by the dashboard check above; this one proves the
    // chip opts out of it and inherits the proportional UI face.
    QLabel *chipVal = card.findChild<QLabel *>(QStringLiteral("miniPingValue"));
    check(chipVal != nullptr, "the chip carries a #miniPingValue figure");
    check(!resolvesMonospace(chipVal), "and the chip text inherits the proportional UI face");

    // ---- the row height FOLLOWS the font instead of clipping it ----
    // It used to be a literal 52 px. Now it is max(52 px floor, layout size hint, name line + padding), so
    // a larger theme/HiDPI text size grows the row rather than cutting the name's descenders or the ring.
    // The shipped look is preserved exactly at the base font; growth is what the change buys.
    check(card.height() >= 52, "the row keeps the design's 52 px floor at the base font");
    check(card.height() >= card.nameLabel()->sizeHint().height(),
          "the row never clips the name's own height");

    card.nameLabel()->setStyleSheet(QStringLiteral("font-size: 42pt;"));   // a big-font stand-in
    card.nameLabel()->ensurePolished();
    card.setHosts(hosts);                                                   // re-runs the height maths
    check(card.height() > 52, "a larger name font grows the row past the floor");
    check(card.height() >= card.nameLabel()->sizeHint().height(),
          "...and still leaves the larger name fully inside the row");

    card.nameLabel()->setStyleSheet(QString());                            // back to the sheet's 11pt
    card.nameLabel()->ensurePolished();
    card.setHosts(hosts);
    check(card.height() == 52, "clearing the override returns the shipped 52 px");
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
    // The history page is sheet-styled apart from its row tints: the panel, the headline, the table and
    // the rate bar all need rules, or the page renders as loose labels on a black canvas.
    check(app.styleSheet().contains(QStringLiteral("historySummary")), "history summary rules present");
    check(app.styleSheet().contains(QStringLiteral("historyTable")), "history table rules present");
    // The rounded panel the table sits in, and the three count chips: both are widgets the page builds by
    // objectName, and an unstyled one is either a square table on a rounded card or plain loose text.
    check(app.styleSheet().contains(QStringLiteral("historyTableCard")),
          "history table panel rules present");
    check(app.styleSheet().contains(QStringLiteral("historyBadge")),
          "history count chip rules present");
    check(app.styleSheet().contains(QStringLiteral("historyRateBar")), "history rate bar rules present");
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
    // matching the token the code paints with. Scoping to the selector is what makes the check a check —
    // and stripping the comments first is what makes the scope mean the DECLARATION, since these files
    // name selectors in prose to explain them (see sheetWithoutComments).
    auto ruleProperty = [](const QString &sheetIn, const QString &selector, const QString &prop) {
        const QString sheet = sheetWithoutComments(sheetIn);
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
        // The history page's rate bar is coloured by a dynamic property the code stamps and the sheet
        // reads; a band missing from one theme leaves that theme's summary a colourless thread.
        check(sheet.contains(QStringLiteral("historyRateBar[rateLevel=\"good\"]")),
              QStringLiteral("%1 sheet colours a healthy bypass").arg(dark ? QStringLiteral("dark")
                                                                          : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("historyRateBar[rateLevel=\"poor\"]")),
              QStringLiteral("%1 sheet colours a failing one").arg(dark ? QStringLiteral("dark")
                                                                       : QStringLiteral("light")));
        // The row tints are model data, and only the absence of an item background rule lets them
        // through: this is the sheet promising not to override them.
        check(!sheet.contains(QStringLiteral("historyTable::item { background")),
              QStringLiteral("%1 sheet leaves the row colours to the model").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // The rounded box belongs to the panel the table sits in, not to the table: a QTableView's radius
        // rounds only its FRAME, while the header band and the rows are children that keep square corners
        // and paint over the arc — the bug. So the panel declares the border and the radius, the table
        // declares no fill and no border of its own, and HistoryPage keeps the table 4 px clear of the
        // panel's edge (checked where the corners are read, below).
        check(ruleProperty(sheet, QStringLiteral("QFrame#historyTableCard"),
                           QStringLiteral("border-radius"))
                  == QStringLiteral("12px"),
              QStringLiteral("%1 sheet rounds the table's panel").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // The table takes the panel's own fill and no border of its own: the panel is the only thing in
        // there that draws a box. (The fill cannot be `transparent` either — a scroll viewport that paints
        // nothing composites onto black, which is why this sheet's last block exists.)
        check(ruleProperty(sheet, QStringLiteral("QTableView#historyTable"),
                           QStringLiteral("background-color"))
                  == ruleProperty(sheet, QStringLiteral("QFrame#historyTableCard"),
                                  QStringLiteral("background-color")),
              QStringLiteral("%1 sheet makes the table and its panel one surface").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        check(ruleProperty(sheet, QStringLiteral("QTableView#historyTable"),
                           QStringLiteral("border"))
                  == QStringLiteral("none"),
              QStringLiteral("%1 sheet gives the panel the only border in there").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // The chips' two shouting bands are the sheet's own state colours, read off the property the code
        // stamps. A band missing from one theme leaves that theme's failures grey.
        check(sheet.contains(QStringLiteral("historyBadge[badgeKind=\"danger\"]")),
              QStringLiteral("%1 sheet colours a chip that counted failures").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        check(sheet.contains(QStringLiteral("historyBadge[badgeKind=\"warning\"]")),
              QStringLiteral("%1 sheet colours a chip that counted substitutions").arg(
                  dark ? QStringLiteral("dark") : QStringLiteral("light")));
        // ---- the window's own shell ----
        //
        // The rounded window is a shape, and a shape is only ever visible as pixels — those are read off a
        // shown window in testWindowFrame(). What belongs here, in the sheet, is the OTHER half: that both
        // themes declare the shape at all, in the numbers MainWindow names, and that nothing in either
        // theme paints the rectangle that would hide it.
        const QString theme = dark ? QStringLiteral("dark") : QStringLiteral("light");
        const QString code = sheetWithoutComments(sheet);
        // The bug this file's whole cascade exists around, in one line: a `QMainWindow { … }` type rule puts
        // an opaque square over every pixel of a translucent surface, corners included, and the rounded
        // card below it becomes a rounding nobody can see. Structural, because "the sheet contains #0B0C10"
        // is true whether or not the window paints it.
        check(!hasUnqualifiedQMainWindowRule(code),
              QStringLiteral("%1 sheet gives the main window no rectangle of its own").arg(theme));
        check(ruleProperty(sheet, QStringLiteral("QMainWindow#mainWindow"),
                           QStringLiteral("background-color"))
                  == QStringLiteral("transparent"),
              QStringLiteral("%1 sheet's window rule is the transparent one").arg(theme));
        // The canvas between the window and the card is the transparent band, and is named here because a
        // reader who "completed" the window-surfaces block with it would undo the corners.
        check(ruleProperty(sheet, QStringLiteral("QWidget#rootCanvas"),
                           QStringLiteral("background-color"))
                  == QStringLiteral("transparent"),
              QStringLiteral("%1 sheet leaves the band around the shell unpainted").arg(theme));
        // The card itself: the arc, the stroke, the fill — all three read out of the constants the C++
        // side and the pixel test both use, so the sheets, the code and the pixels are one design.
        check(ruleProperty(sheet, QStringLiteral("QFrame#mainWindowFrame"),
                           QStringLiteral("border-radius"))
                  == QStringLiteral("%1px").arg(MainWindow::kWindowFrameRadiusPx),
              QStringLiteral("%1 sheet rounds the window shell by %2px")
                  .arg(theme).arg(MainWindow::kWindowFrameRadiusPx));
        check(sheetRuleHex(sheet, QStringLiteral("QFrame#mainWindowFrame"), QStringLiteral("border"))
                  == Theme::border(),
              QStringLiteral("%1 sheet strokes the shell in the sheet's own border token")
                  .arg(theme));
        check(sheetRuleColor(sheet, QStringLiteral("QFrame#mainWindowFrame"),
                             QStringLiteral("background-color"))
                  == Theme::windowBackground(),
              QStringLiteral("%1 sheet fills the shell with the window background").arg(theme));
        check(sheetRuleColor(sheet, QStringLiteral("QFrame#mainWindowFrame"),
                             QStringLiteral("background-color"))
                  != sheetRuleColor(sheet, QStringLiteral("QFrame#sidebar"),
                                    QStringLiteral("background-color"))
                      || dark,
              QStringLiteral("%1 sheet's shell and rail are distinguishable, so a corner that leaks the "
                             "rail is a pixel that disagrees").arg(theme));
        // Qt does not clip a child to its parent's radius, so every surface that reaches a window corner
        // carries the arc's INNER edge — one pixel less than the radius, because the pixel it would
        // otherwise cover is the stroke. An unrounded one is a square of the app outside the silhouette.
        const QString inner = QStringLiteral("%1px").arg(MainWindow::kWindowFrameRadiusPx
                                                         - MainWindow::kWindowFrameBorderPx);
        struct CornerRule {
            const char *selector;
            const char *property;
        };
        const CornerRule kCornerRules[] = {
            {"QFrame#sidebar", "border-top-left-radius"},
            {"QFrame#sidebar", "border-bottom-left-radius"},
            {"QFrame#mainArea", "border-top-right-radius"},
            {"QFrame#mainArea", "border-bottom-right-radius"},
            {"QWidget#headerBar", "border-top-right-radius"},
            {"QFrame#footerBar", "border-bottom-right-radius"},
        };
        for (const CornerRule &c : kCornerRules) {
            check(ruleProperty(sheet, QLatin1String(c.selector), QLatin1String(c.property))
                      == inner,
                  QStringLiteral("%1 sheet rounds %2's %3 to the shell's inner arc (%4)")
                      .arg(theme, QLatin1String(c.selector), QLatin1String(c.property), inner));
        }

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
                                // The rail's own set: brand, the page glyphs and the foot action.
                                QStringLiteral("home"), QStringLiteral("logs"),
                                QStringLiteral("chart"),
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
                             QStringLiteral("youtube"), QStringLiteral("discord"),
                             QStringLiteral("chart")}) {
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

// ---------------------------------------------------------------- History page ----
//
// The page is the record's face: a summary of everything the store holds, and the rows themselves with
// their verdict painted on. Half of that is arithmetic and mapping — what colour is an intercepted row,
// what percent is two of three — which is checked on the class without a widget at all; the other half
// is "does the table follow the store", checked on a live page over a real temporary file.

namespace {

// A history page that answers its own confirmation. The default one opens a modal, and a suite has no
// pointer to click and nobody to press Escape; what is worth checking is the decision on either side of
// the answer, so the answer is data here.
class ClearProbe : public HistoryPage
{
public:
    using HistoryPage::HistoryPage;

    bool answerYes = true;
    int asked = 0;
    int lastTotal = -1;

protected:
    bool confirmClear(int totalRows) override
    {
        ++asked;
        lastTotal = totalRows;
        return answerYes;
    }
};

// The colour one sheet declares for one selector property. The corner checks below compare pixels
// with colours the SHEET owns - the panel fill and the header band - so reading them out of the
// sheet is what keeps those assertions true in either theme; hardcoding them here would test this
// file, not a theme.
QColor sheetColor(const QString &sheet, const QString &selector, const QString &prop)
{
    const int at = sheet.indexOf(selector);
    const int open = at < 0 ? -1 : sheet.indexOf(QLatin1Char('{'), at);
    const int close = open < 0 ? -1 : sheet.indexOf(QLatin1Char('}'), open);
    if (close < 0)
        return QColor();
    const QStringList decls = sheet.mid(open + 1, close - open - 1).split(QLatin1Char(';'));
    for (const QString &d : decls) {
        const int colon = d.indexOf(QLatin1Char(':'));
        if (colon > 0 && d.left(colon).trimmed().compare(prop, Qt::CaseInsensitive) == 0)
            return QColor(d.mid(colon + 1).trimmed());
    }
    return QColor();
}

// What a rendered row strip is mostly made of: its background. Averaged over the strip rather than
// sampled, because the text in it is a few hundred pixels against twenty thousand of tint.
QColor rowAverage(const QImage &shot, int row, int rowHeight)
{
    const int top = row * rowHeight + 4;
    const int bottom = qMin(shot.height(), top + qMax(1, rowHeight - 10));
    qint64 r = 0, g = 0, b = 0, n = 0;
    for (int y = top; y < bottom; ++y) {
        for (int x = 4; x + 4 < shot.width(); ++x) {
            const QColor c = shot.pixelColor(x, y);
            r += c.red(); g += c.green(); b += c.blue(); ++n;
        }
    }
    if (n == 0)
        return QColor();
    return QColor(int(r / n), int(g / n), int(b / n));
}

} // namespace

static void testHistoryPage()
{
    std::printf("\n[HistoryPage]\n");

    // ---- the three verdicts, from the two booleans a row carries ----
    HistoryEntry passed;   passed.host = QStringLiteral("a.example");   passed.success = true;
    HistoryEntry intercepted; intercepted.host = QStringLiteral("b.example");
    intercepted.success = true; intercepted.certValid = false;
    HistoryEntry failed;   failed.host = QStringLiteral("c.example");   failed.success = false;
    HistoryEntry failedWithBadCert = failed;
    failedWithBadCert.certValid = false;

    check(HistoryPage::verdictFor(passed) == HistoryVerdict::Passed,
          "a clean pass is the boring green");
    check(HistoryPage::verdictFor(intercepted) == HistoryVerdict::Intercepted,
          "a pass whose certificate was rejected is the amber one — that is why the field exists");
    check(HistoryPage::verdictFor(failed) == HistoryVerdict::Failed, "no connection is a failure");
    check(HistoryPage::verdictFor(failedWithBadCert) == HistoryVerdict::Failed,
          "…and stays one when the TLS half failed too: a check that never connected has no "
          "interception to accuse anybody of");

    check(HistoryPage::verdictColor(HistoryVerdict::Passed) == Theme::success(),
          "the passed word is the theme's green, not a second green");
    check(HistoryPage::verdictColor(HistoryVerdict::Failed) == Theme::error(),
          "the failed word is the theme's red");
    check(HistoryPage::verdictColor(HistoryVerdict::Intercepted) == Theme::warning(),
          "the intercepted word is the theme's amber");

    const QColor tint = HistoryPage::verdictTint(HistoryVerdict::Intercepted);
    check(tint.red() == Theme::warning().red() && tint.green() == Theme::warning().green()
              && tint.blue() == Theme::warning().blue(),
          "a row's background is its verdict's colour, not an unrelated one");
    check(tint.alpha() > 0 && tint.alpha() < 90,
          QStringLiteral("…and kept a wash rather than a fill (alpha %1)").arg(tint.alpha()));

    // The words themselves are catalog entries; a missing row falls back to the key, which is exactly
    // what a status column must never show.
    for (const QString &key : {QStringLiteral("history.verdict.passed"),
                               QStringLiteral("history.verdict.failed"),
                               QStringLiteral("history.verdict.intercepted")})
        check(LanguageManager::tr(key) != key,
              QStringLiteral("the status column's word '%1' is translated").arg(key));
    check(HistoryPage::verdictText(HistoryVerdict::Intercepted)
              == QStringLiteral("Подмена сертификата"),
          "the amber row says what happened to the certificate, in Russian");

    // ---- the arithmetic behind the headline ----
    check(HistoryPage::Tally().successRate() == -1,
          "an empty record has no percentage, and 0% is a different claim");

    QVector<HistoryEntry> mixed;
    mixed << passed << passed << intercepted << failed;
    const HistoryPage::Tally t = HistoryPage::tallyOf(mixed);
    check(t.total == 4 && t.passed == 2 && t.intercepted == 1 && t.failed == 1,
          "one pass over the rows counts each verdict once");
    check(t.successRate() == 50, "two of four is 50%");
    check(HistoryPage::tallyOf(QVector<HistoryEntry>{failed}).successRate() == 0,
          "none of one is 0%, and that is not the same sentence as -1");

    QVector<HistoryEntry> twoOfThree;
    twoOfThree << passed << passed << failed;
    check(HistoryPage::tallyOf(twoOfThree).successRate() == 67,
          "two of three rounds to 67% — a truncated 66% undersells the record it summarises");
    // ---- the live page over a real store ----
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory for the history file");
    HistoryStore store(dir.filePath(QStringLiteral("history.json")));
    store.setSaveDelayMs(0);

    ClearProbe page(&store);
    check(page.rowCount() == 0, "a fresh record shows an empty table");
    check(page.showsEmptyNotice(), "…and says so, instead of drawing a bare grid of headings");
    check(page.successRate() == -1, "with no number to show");
    check(!page.clearButton()->isEnabled(), "and nothing to clear, so the button says so");

    // Timestamps spelled out rather than left to the store's "stamp it now": the ordering checks below
    // are about which row the page picks first, and two appends inside the same millisecond would make
    // that a race instead of a fact.
    const QDateTime t0(QDate(2026, 10, 22), QTime(12, 0, 0));
    store.appendResult(QStringLiteral("nfqws (portable)"), QStringLiteral("first.example"),
                       true, 42, true, t0);
    check(page.rowCount() == 1,
          "the store's changed() moves the table by itself — nobody had to tell the page");
    check(!page.showsEmptyNotice(), "and the notice steps aside for it");
    check(page.clearButton()->isEnabled(), "…while clearing becomes a thing you can do");

    store.appendResult(QStringLiteral("nfqws (portable)"), QStringLiteral("second.example"),
                       false, -1, true, t0.addSecs(60));
    store.appendResult(QStringLiteral("str192-domain"), QStringLiteral("third.example"),
                       true, 10, false, t0.addSecs(120));
    check(page.rowCount() == 3, "one refresh per recorded check");

    QAbstractItemModel *model = page.table()->model();
    const int colHost = HistoryTableModel::ColumnHost;
    const int colVerdict = HistoryTableModel::ColumnVerdict;
    const int colLatency = HistoryTableModel::ColumnLatency;
    const int colTime = HistoryTableModel::ColumnTime;
    check(model->columnCount(QModelIndex()) == HistoryTableModel::ColumnCount,
          "the table has the five columns it was drawn with");
    check(model->data(model->index(0, colHost), Qt::DisplayRole).toString()
              == QStringLiteral("third.example"),
          "newest first: the row the user just watched happen is at the top");
    check(model->data(model->index(2, colHost), Qt::DisplayRole).toString()
              == QStringLiteral("first.example"), "…and the oldest is at the bottom");

    check(model->data(model->index(0, colVerdict), Qt::DisplayRole).toString()
              == HistoryPage::verdictText(HistoryVerdict::Intercepted),
          "the newest row is the amber one, and says so in words as well as in colour");
    check(model->data(model->index(1, colVerdict), Qt::DisplayRole).toString()
              == HistoryPage::verdictText(HistoryVerdict::Failed), "the middle row is the failure");
    check(model->data(model->index(2, colVerdict), Qt::DisplayRole).toString()
              == HistoryPage::verdictText(HistoryVerdict::Passed), "the oldest row passed");

    check(model->data(model->index(1, colLatency), Qt::DisplayRole).toString()
              == QStringLiteral("—"),
          "a check that never connected shows a dash, not the 0 it was measured as");
    check(model->data(model->index(2, colLatency), Qt::DisplayRole).toString()
              == QStringLiteral("42"), "a measured latency shows as whole milliseconds");
    check(model->data(model->index(2, colTime), Qt::DisplayRole).toString()
              .contains(QStringLiteral("22.10.2026")), "the time column carries the date too");

    // The colour coding, as the view is told it: every cell of the row wears the verdict, and the status
    // word wears it at full strength.
    check(model->data(model->index(0, colHost), Qt::BackgroundRole).value<QBrush>().color()
              == HistoryPage::verdictTint(HistoryVerdict::Intercepted),
          "an intercepted row's background is the amber wash");
    check(model->data(model->index(1, colTime), Qt::BackgroundRole).value<QBrush>().color()
              == HistoryPage::verdictTint(HistoryVerdict::Failed),
          "…a failed row's is the red one, on every column of the row");
    check(model->data(model->index(0, colVerdict), Qt::ForegroundRole).value<QBrush>().color()
              == HistoryPage::verdictColor(HistoryVerdict::Intercepted),
          "and the status word is its verdict's colour at full strength");
    check(!model->data(model->index(0, colHost), Qt::ForegroundRole).isValid(),
          "…which the rest of the row deliberately does not take, or the page would be a palette");

    // ---- the summary ----
    check(page.successRate() == 33, "one clean check in three is 33%");
    check(page.headline()->text() == LanguageManager::tr(QStringLiteral("history.headline"),
                                                        QStringLiteral("33")),
          "and the headline says exactly that");
    // ---- the three count chips ----
    // One sentence cannot be coloured per number, so the summary is three chips and the colour is the
    // decision worth testing: red and amber are claims that something happened, which means a zero must
    // not wear one and a seven must not wear grey.
    check(page.summaryBadgesVisible(), "a record with rows in it shows its chips");
    check(page.totalBadge()->text()
              == LanguageManager::tr(QStringLiteral("history.badge.total")).arg(3),
          "the first chip names what the percentage was computed from");
    check(page.failedBadge()->text()
              == LanguageManager::tr(QStringLiteral("history.badge.failed")).arg(1),
          "…the second the checks that never connected…");
    check(page.certBadge()->text()
              == LanguageManager::tr(QStringLiteral("history.badge.cert")).arg(1),
          "…and the third the certificates that came back somebody else's");
    check(page.badgeKind(page.totalBadge()) == QStringLiteral("neutral"),
          "a total is a number, never an alarm");
    check(page.badgeKind(page.failedBadge()) == QStringLiteral("danger"),
          "one failure does wear the sheet's red");
    check(page.badgeKind(page.certBadge()) == QStringLiteral("warning"),
          "…and one substitution its amber");
    // …and the band reaches the screen. Each chip carries its figure in an inline stylesheet (the mono
    // font needs one to beat the sheet's generic `QWidget` rule), which is exactly the situation where a
    // widget's own colours could silently stop coming from the app sheet — so a chip is rendered and the
    // colour it claimed is counted inside it.
    auto pixelsOf = [](QLabel *badge, const QColor &ink) {
        const QImage img = badge->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        int hits = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const QColor c = img.pixelColor(x, y);
                if (qAbs(c.red() - ink.red()) < 24 && qAbs(c.green() - ink.green()) < 24
                    && qAbs(c.blue() - ink.blue()) < 24)
                    ++hits;
            }
        return hits;
    };
    check(pixelsOf(page.failedBadge(), Theme::error()) > 40,
          QStringLiteral("the failure chip is rendered in the sheet's red (%1 px of %2x%3)")
              .arg(pixelsOf(page.failedBadge(), Theme::error()))
              .arg(page.failedBadge()->width()).arg(page.failedBadge()->height()));
    check(pixelsOf(page.certBadge(), Theme::warning()) > 40,
          QStringLiteral("…and the substitution chip in its amber (%1 px of %2x%3)")
              .arg(pixelsOf(page.certBadge(), Theme::warning()))
              .arg(page.certBadge()->width()).arg(page.certBadge()->height()));
    // Read across as well as straight: each chip wears one colour, not either. Without these the two
    // counts above would be satisfied by a chip that painted the wrong state colour, and the two state
    // colours are close enough for a loose tolerance to let that through.
    check(pixelsOf(page.failedBadge(), Theme::warning()) == 0,
          "…and the failure chip is not the amber one");
    check(pixelsOf(page.certBadge(), Theme::error()) == 0, "…and the substitution chip is not the red one");
    check(page.certBadge()->text() != page.failedBadge()->text(),
          "the two chips are different labels, not one widget checked twice");
    check(pixelsOf(page.totalBadge(), Theme::error()) == 0,
          "…while the total chip wears no alarm colour at all");

    check(page.table()->model()->rowCount() == page.rowCount(),
          "the hook the suite reads is the table's own row count, not a second number");

    // ---- do the row colours survive the stylesheet? ----
    //
    // Everything above is the model's side of the promise. The sheet is the other half, and it is the
    // half that breaks silently: the tint is a brush on the model, and a styled item view only puts it on
    // screen because the page installs a delegate that fills it (see VerdictDelegate). Drop that delegate,
    // or let the sheet declare a background of its own for ::item, and the table keeps its words, its
    // columns and its order while losing the one thing that makes a failed night findable at a glance.
    // So the table is rendered, and its pixels are read.
    page.resize(840, 460);
    if (QLayout *l = page.layout())
        l->activate();
    const QImage shot = page.table()->viewport()->grab().toImage()
                           .convertToFormat(QImage::Format_ARGB32);
    check(shot.width() > 300 && shot.height() > 80,
          QStringLiteral("the table renders at %1x%2").arg(shot.width()).arg(shot.height()));
    if (shot.width() > 300 && shot.height() > 80) {
        // Redness is where the red and the amber rows part company from the green one, greenness is
        // where the amber parts from the red: amber is a green that ran out of blue, and red is not.
        auto redness = [&](int row) {
            const QColor c = rowAverage(shot, row, 30);
            return c.red() - c.green();
        };
        auto greenness = [&](int row) {
            const QColor c = rowAverage(shot, row, 30);
            return c.green() - c.blue();
        };
        check(redness(2) < redness(0) - 4 && redness(0) < redness(1) - 4,
              QStringLiteral("rendered: the passed row (%1) is less red than the intercepted (%2), "
                             "which is less red than the failed one (%3)")
                  .arg(redness(2)).arg(redness(0)).arg(redness(1)));
        check(greenness(1) < greenness(2) - 4 && greenness(2) < greenness(0) - 4,
              QStringLiteral("rendered: the failed row (%1) is less green than the passed (%2), "
                             "which is less green than the amber one (%3)")
                  .arg(greenness(1)).arg(greenness(2)).arg(greenness(0)));

        // The word, not only the row. An inherited `color` from the sheet would paint every status word
        // in the page's text colour and leave the verdict in the background alone — a page that reads as
        // three grey words on three coloured stripes is one edit away from reading as nothing.
        auto inkCount = [&](int row, const QColor &want) {
            int n = 0;
            const int top = row * 30 + 4;
            const int bottom = qMin(shot.height(), top + 20);
            for (int y = top; y < bottom; ++y)
                for (int x = 4; x + 4 < shot.width(); ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    const int d = qAbs(c.red() - want.red()) + qAbs(c.green() - want.green())
                                  + qAbs(c.blue() - want.blue());
                    if (d < 90)
                        ++n;
                }
            return n;
        };
        check(inkCount(0, Theme::warning()) > 20, "the amber row's status word is amber");
        check(inkCount(1, Theme::error()) > 20, "the failed row's status word is red");
        check(inkCount(2, Theme::success()) > 20, "the passed row's status word is green");
    }

    // The three chips have to fit beside the headline rather than push it out: the window is a fixed
    // 900 px, and a summary that overflowed would clip the one number the page exists to show.
    if (QLayout *fit = page.layout())
        fit->activate();
    const QRect chips(page.totalBadge()->mapTo(&page, QPoint(0, 0)),
                      page.certBadge()->mapTo(&page, QPoint(page.certBadge()->width(), 0)));
    const QRect head(page.headline()->mapTo(&page, QPoint(0, 0)),
                     page.headline()->mapTo(&page, QPoint(page.headline()->width(), 0)));
    check(chips.right() < page.width() && chips.left() > head.right()
              && page.totalBadge()->height() > 8 && page.certBadge()->width() > 20,
          QStringLiteral("the chips fit on the summary line, after the headline (head ends %1, "
                         "chips run %2..%3 of %4)")
              .arg(head.right()).arg(chips.left()).arg(chips.right()).arg(page.width()));

    // ---- does the panel's shape actually hold? ----
    //
    // Everything above is the model's side of the page. Its shape is the other half, and it is the half
    // that broke silently: the radius used to sit on the QTableView, which rounds its own frame while the
    // header band paints straight over the arc, square-cornered and outside the panel. No assertion on the
    // rows and no contains("border-radius") on the sheet can see that, so the pixels are read - against the
    // colours the sheet itself declares, which is what keeps each of them true in either theme.
    {
        QWidget *card = page.tableCard();
        QHeaderView *hdr = page.table()->horizontalHeader();
        check(card != nullptr && hdr != nullptr && card->width() > 120 && card->height() > 60,
              QStringLiteral("the table's panel is on screen at %1x%2")
                  .arg(card ? card->width() : -1).arg(card ? card->height() : -1));
        if (card && hdr && card->width() > 120 && card->height() > 60) {
            auto within6 = [](const QColor &a, const QColor &b) {
                return qAbs(a.red() - b.red()) <= 6 && qAbs(a.green() - b.green()) <= 6
                       && qAbs(a.blue() - b.blue()) <= 6;
            };
            // Run it in BOTH themes, and each theme proves something the other cannot. The three
            // greys in play are: light page #F3F4F6, card #FFFFFF, band #F3F4F6; dark page #0B0C10,
            // card #12131A, band #15161C — and within6() is the yardstick.
            //
            //  * A corner left square because the table sits flush to the panel reads the CARD FILL
            //    there: 12 levels off the page grey in light, 14 in dark. Both runs catch it.
            //  * A corner covered by the HEADER BAND reads the band colour: 14 off the page in dark,
            //    so dark catches it — but #F3F4F6 in light is the page's own grey, so a band leak in
            //    light is invisible to this particular sample. It is dark's job, not light's.
            //  * Which rule owns the band is light's job. Dark declares that grey twice — #15161C on
            //    QHeaderView#historyHeader::section and the same #15161C on the generic
            //    QHeaderView::section fallback at the foot of the sheet — so a band painted by either
            //    rule looks identical there. Light declares #F3F4F6 against that fallback's #E5E7EB,
            //    fourteen apart: only the light run can see the ID-qualified rule beating the
            //    fallback, which is what the inset relies on (a band the fallback re-tinted would
            //    change colour under the arc without changing its shape).
            //
            // Reading the wanted colours out of the live sheet instead of hardcoding them is what
            // lets one loop cover both.
            for (const bool dark : {true, false}) {
                const QString theme = dark ? QStringLiteral("dark") : QStringLiteral("light");
                Theme::apply(*static_cast<QApplication *>(qApp), dark);
                if (QLayout *ls = page.layout())
                    ls->activate();
                const QImage chrome = page.grab().toImage().convertToFormat(QImage::Format_ARGB32);
                const QColor pageInk = Theme::windowBackground();
                const QColor bandInk = sheetColor(static_cast<QApplication *>(qApp)->styleSheet(),
                                                  QStringLiteral("QHeaderView#historyHeader::section"),
                                                  QStringLiteral("background-color"));
                check(bandInk.isValid(),
                      QStringLiteral("the %1 sheet declares the header band's colour").arg(theme));
                // One pixel inside the panel's frame, at the corner. Outside a 12 px arc there is
                // nothing but the page behind the panel, and a band that leaked covers this point.
                const QPoint tl = card->mapTo(&page, QPoint(1, 1));
                const QPoint tr = card->mapTo(&page, QPoint(card->width() - 2, 1));
                // And the band itself, read in the strip where its text does not paint: proof the clean
                // corners are a shape rather than a header that forgot to draw at all. In the left
                // padding of the first section, vertically centred — band colour there, and no glyph,
                // whichever way the header's own padding resolves.
                const QPoint band = hdr->mapTo(&page, QPoint(3, hdr->height() / 2));
                check(chrome.rect().contains(tl) && within6(chrome.pixelColor(tl), pageInk),
                      QStringLiteral("%1: the panel's top-left corner shows the page behind it, not "
                                     "the header band (%2,%3,%4)")
                          .arg(theme).arg(chrome.pixelColor(tl).red())
                          .arg(chrome.pixelColor(tl).green()).arg(chrome.pixelColor(tl).blue()));
                check(chrome.rect().contains(tr) && within6(chrome.pixelColor(tr), pageInk),
                      QStringLiteral("…%1 and so does its top-right one (%2,%3,%4)")
                          .arg(theme).arg(chrome.pixelColor(tr).red())
                          .arg(chrome.pixelColor(tr).green()).arg(chrome.pixelColor(tr).blue()));
                check(chrome.rect().contains(band) && within6(chrome.pixelColor(band), bandInk),
                      QStringLiteral("%1: the header band is painted where the sheet puts it "
                                     "(%2,%3,%4), so the clean corners are a shape and not a blank")
                          .arg(theme).arg(chrome.pixelColor(band).red())
                          .arg(chrome.pixelColor(band).green())
                          .arg(chrome.pixelColor(band).blue()));
                chrome.save(dark ? QStringLiteral("/tmp/stats-dark.png")
                                 : QStringLiteral("/tmp/stats-light.png"));
            }
            // Leave the page on dark, the default the rest of the binary and the app itself expect.
            Theme::apply(*static_cast<QApplication *>(qApp), true);
            if (QLayout *ls = page.layout())
                ls->activate();
        }
    }

    // ---- sorting ----
    // The unmeasured row has to land at the far end of an ascending latency sort, not at the front: a
    // reader clicking the latency header is asking what is slowest to answer, and "it never answered" is
    // not the fastest possible answer.
    page.table()->sortByColumn(HistoryTableModel::ColumnLatency, Qt::AscendingOrder);
    check(model->data(model->index(0, colLatency), Qt::DisplayRole).toString()
              == QStringLiteral("10"),
          "sorting by latency puts the quickest answer first");
    check(model->data(model->index(2, colHost), Qt::DisplayRole).toString()
              == QStringLiteral("second.example"),
          "…and the check that never connected last, not first");
    page.table()->sortByColumn(colTime, Qt::DescendingOrder);
    check(model->data(model->index(0, colHost), Qt::DisplayRole).toString()
              == QStringLiteral("third.example"),
          "the newest-first reading order comes back on the click that asks for it");

    // ---- the clear, both answers ----
    QSignalSpy clearedSpy(&page, &HistoryPage::cleared);
    page.answerYes = false;
    page.clearButton()->click();
    check(page.asked == 1, "the press asks before it destroys");
    check(page.lastTotal == 3, "and says how much is at stake while asking");
    check(store.count() == 3 && page.rowCount() == 3,
          "a no leaves every row where it was, in the file and on the screen alike");
    check(clearedSpy.count() == 0, "…and is not reported as a clearing");

    page.answerYes = true;
    page.clearButton()->click();
    check(page.asked == 2, "a second press asks again rather than remembering the last answer");
    check(store.count() == 0, "a yes empties the record");
    check(page.rowCount() == 0, "…and the table follows the record, not a copy of it");
    check(page.showsEmptyNotice(), "the notice comes back into its own");
    check(page.successRate() == -1, "with the percentage taken away rather than set to zero");
    check(!page.clearButton()->isEnabled(), "…and nothing left to clear");
    check(clearedSpy.count() == 1 && clearedSpy.at(0).at(0).toInt() == 3,
          "the page says how many rows went, so the window can put the number in the journal");
    check(page.headline()->text() == LanguageManager::tr(QStringLiteral("history.headline.empty")),
          "the headline switches to the sentence an empty record deserves");
    check(!page.summaryBadgesVisible(),
          "…and the three chips go quiet rather than reciting three zeroes at it");
    // The bar's band is a dynamic property the sheet colours. "none" is the point: an empty record must
    // not be painted as a verdict, and a grey thread is the only honest thing to show for it.
    check(page.rateLevel() == QStringLiteral("none"),
          "…and the indicator shows no verdict at all rather than a zero percent one");

    // ---- language ----
    // Headers and verdict words are derived from the catalog rather than tagged on a widget, so they are
    // the part a language switch can leave behind. The page is asked to re-read them, and does.
    LanguageManager::instance().setCode(QStringLiteral("en"));
    page.retranslateUi();
    check(model->headerData(colVerdict, Qt::Horizontal, Qt::DisplayRole).toString()
              == QStringLiteral("Status"),
          "the header row follows the language");
    store.appendResult(QStringLiteral("nfqws"), QStringLiteral("en.example"), true, 5, true, t0);
    check(model->data(model->index(0, colVerdict), Qt::DisplayRole).toString()
              == QStringLiteral("Passed"),
          "…and so does a row's verdict word, on a row the page was not rebuilt for");
    check(!page.showsEmptyNotice(), "the one new row is enough to put the table back");
    check(page.summaryBadgesVisible(), "…and the chips came back with it");
    check(page.badgeKind(page.failedBadge()) == QStringLiteral("neutral"),
          "a record with nothing failed takes the red off the failure chip");
    check(page.badgeKind(page.certBadge()) == QStringLiteral("neutral"),
          "…and the amber off the certificate one");
    LanguageManager::instance().setCode(QStringLiteral("ru"));
    page.retranslateUi();
    check(model->headerData(colVerdict, Qt::Horizontal, Qt::DisplayRole).toString()
              == QStringLiteral("Статус"), "switching back re-says them again");
    check(page.headline()->text() == LanguageManager::tr(QStringLiteral("history.headline"),
                                                        QStringLiteral("100")),
          "the summary re-says itself in the new language too");

    // Every text-bearing control on the page carries a catalog key — the contract the window's
    // retranslate walk reads. A button with a hardcoded sentence on it is invisible to a language switch
    // and to this check.
    const auto buttons = page.findChildren<QPushButton *>();
    int tagged = 0;
    for (QPushButton *b : buttons) {
        const QString key = b->property("i18nKey").toString();
        if (key.isEmpty())
            continue;
        ++tagged;
        check(LanguageManager::keys().contains(key),
              QStringLiteral("history page button key '%1' is in the catalog").arg(key));
        check(b->text() == LanguageManager::tr(key),
              QStringLiteral("history page button '%1' says what its key says").arg(key));
    }
    check(tagged >= 1, "the page's own controls are tagged for retranslation");

    // ---- the indicator's bands ----
    // Where the headline's colour turns is a policy of its own, and it is the policy the whole summary
    // exists to state: a bypass that passes one check in two is not working, and painting it green
    // because the bar moved at all is the page lying about the one number it was built to show.
    check(page.rateLevel() == QStringLiteral("good"), "one clean check in one is the healthy band");
    store.appendResult(QStringLiteral("nfqws"), QStringLiteral("bad.example"), false, -1, true,
                       t0.addSecs(600));
    check(page.rateLevel() == QStringLiteral("poor"),
          "…and it falls to the failing band at half, not at zero");
    store.appendResult(QStringLiteral("nfqws"), QStringLiteral("ok.example"), true, 7, true,
                       t0.addSecs(660));
    check(page.rateLevel() == QStringLiteral("fair"),
          "two of three is its own amber band, because it is neither working nor broken");
}

// --------------------------------------------------------------------- Motion ----
//
// The three kinds of movement and the one switch over them, each checked the same way round: the motion
// is running when it should be, it has STOPPED and cleaned up when it should not, and the widget is left
// where its layout wanted it. That last part is the difference between an animation and an animation
// that broke the screen, and it is invisible to any check that only asks whether one started.
//
// The window's own paths — setCurrentPage, populateCards, renderStatus — are exercised in the watchdog
// section below, which is the one place a real window is built.
static void testMotion()
{
    std::printf("\n[motion]\n");

    const bool wasEnabled = Motion::enabled();
    Motion::setEnabled(true);

    check(Motion::enabled(), "motion is on by default: the design was drawn moving");
    Motion::setEnabled(false);
    check(!Motion::enabled(), "and the switch is readable back");
    Motion::setEnabled(true);

    // ---- a page appearing ----
    {
        QWidget page;
        page.resize(320, 200);
        page.show();
        QTest::qWait(10);
        check(page.graphicsEffect() == nullptr, "a widget nobody touched carries no effect");

        Motion::fadeIn(&page, 120);
        check(Motion::isMoving(&page), "fadeIn starts a run on the widget it was given");
        auto *fx = qobject_cast<QGraphicsOpacityEffect *>(page.graphicsEffect());
        check(fx != nullptr,
              "…driven by an opacity effect: QWidget::windowOpacity is a window property and would fade "
              "the whole application rather than the page inside it");
        QTest::qWait(60);
        check(fx && fx->opacity() > 0.0 && fx->opacity() <= 1.0,
              QStringLiteral("…whose opacity is actually moving (%1)").arg(fx ? fx->opacity() : -1.0));
        QTest::qWait(150);
        check(!Motion::isMoving(&page), "…which ends by itself");
        check(page.graphicsEffect() == nullptr,
              "…and detaches: an effect left attached would tax every repaint the page goes on doing");
        check(!Motion::isMoving(nullptr), "a null widget is simply not moving");
        page.hide();
    }

    // ---- a row rising into place ----
    {
        QWidget host;
        auto *lay = new QVBoxLayout(&host);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);
        auto *one = new QWidget(&host);
        auto *two = new QWidget(&host);
        one->setFixedSize(300, 52);
        two->setFixedSize(300, 52);
        lay->addWidget(one);
        lay->addWidget(two);
        host.show();
        QTest::qWait(10);
        const QPoint oneRest = one->pos();
        const QPoint twoRest = two->pos();
        check(twoRest.y() > oneRest.y(), "the fixture layout stacked its two rows");

        Motion::riseIn(one);
        Motion::riseIn(two, Motion::kCardStepMs);
        check(one->pos() == oneRest + QPoint(0, Motion::kCardLiftPx),
              "a rising row starts below the slot the layout gave it");
        check(Motion::isMoving(one) && Motion::isMoving(two),
              "…and a staggered row is staged from the same moment rather than starting late");
        QTest::qWait(Motion::kCardRiseMs + Motion::kCardStepMs + 150);
        check(one->pos() == oneRest && two->pos() == twoRest,
              "…and both land exactly where the layout said, not 14 px short of it");
        check(one->graphicsEffect() == nullptr && two->graphicsEffect() == nullptr,
              "…with no effect left behind on either row");
        host.hide();
    }

    // ---- off means nothing moves, and nothing is installed in order not to ----
    {
        QWidget page;
        auto *lay = new QVBoxLayout(&page);
        auto *row = new QWidget(&page);
        row->setFixedSize(200, 40);
        lay->addWidget(row);
        page.show();
        QTest::qWait(10);
        const QPoint rest = row->pos();

        Motion::setEnabled(false);
        Motion::fadeIn(&page);
        Motion::riseIn(row);
        check(page.graphicsEffect() == nullptr && row->graphicsEffect() == nullptr,
              "with motion off, neither entrance installs an effect at all");
        check(!Motion::isMoving(&page) && !Motion::isMoving(row), "…and neither is left running");
        check(row->pos() == rest, "…and the row was never moved, so there is nothing to put back");

        // The mid-flight case: unchecking the box lands what was already in the air.
        Motion::setEnabled(true);
        Motion::riseIn(row, 0, 600);
        check(Motion::isMoving(row), "a long rise is in flight");
        Motion::setEnabled(false);
        check(!Motion::isMoving(row), "unchecking motion stops it where it stands");
        check(row->pos() == rest && row->graphicsEffect() == nullptr,
              "…and puts the row back with the effect off, rather than parking it mid-slide");
        Motion::setEnabled(true);
        page.hide();
    }

    // ---- the power button's breath ----
    {
        PowerButton button;
        button.show();
        QTest::qWait(10);     // the same rule as above: hidden means still, so show it first
        check(!button.isPulsing(), "an idle button does not breathe");
        check(button.graphicsEffect() == nullptr, "…and carries no effect at all");

        button.setPulsing(true);
        check(button.isPulsing(), "a live run breathes the whole control");
        check(qobject_cast<QGraphicsOpacityEffect *>(button.graphicsEffect()) != nullptr,
              "…as an opacity effect, the only thing that dims ring, halo and glyph by one factor");
        QTest::qWait(300);
        const qreal level = Motion::breathLevel(&button);
        check(level > 0.0 && level <= 1.0, QStringLiteral("the breath stays in range (%1)").arg(level));
        check(level < 1.0, "…and is actually below full, which is what makes it a pulse rather than a poke");

        button.setPulsing(false);
        check(!button.isPulsing(), "stopping the run stops the breath");
        check(button.graphicsEffect() == nullptr,
              "…taking the effect off, so the ring goes back through the fast paint path");
        check(qFuzzyCompare(button.glow(), 1.0), "…and leaves no layer parked at a dimmed value");

        // One rhythm at a time: while the button breathes, the halo holds still.
        button.setPulsing(true);
        check(qFuzzyCompare(button.glow(), 1.0),
              "a breathing button's halo does not breathe alongside it on a second period");
        Motion::setEnabled(false);
        button.setPulsing(true);        // what the next renderStatus() does under the new policy
        check(!button.isPulsing(), "with motion off, the same state leaves the button still");
        check(button.graphicsEffect() == nullptr, "…and no effect was installed in order to be skipped");
        Motion::setEnabled(true);
        button.setPulsing(true);
        check(button.isPulsing(), "…and switching motion back on brings the pulse back");
        button.setPulsing(false);
    }

    // ---- the halo breath belongs to a transition, and to the same switch ----
    {
        PowerButton button;
        Motion::setEnabled(true);
        button.show();
        QTest::qWait(10);     // the same rule as above: a widget never put on screen stays still
        button.setMode(PowerButton::Mode::Busy);
        QTest::qWait(300);
        check(button.glow() < 1.0 && button.glow() > 0.0,
              "a transition in flight breathes the halo");
        Motion::setEnabled(false);
        button.setPulsing(false);       // the window reconciles on every settings change
        check(qFuzzyCompare(button.glow(), 1.0),
              "…and the motion switch parks it at full rather than leaving it half-dimmed");
        Motion::setEnabled(true);
        button.setMode(PowerButton::Mode::Idle);
        check(qFuzzyCompare(button.glow(), 1.0), "an idle button is a still one");
        button.hide();
    }

    // ---- the settings page owns the switch ----
    QTemporaryDir dir;
    check(dir.isValid(), "temporary store for the motion checkbox");
    Settings store(dir.filePath(QStringLiteral("muffin-motion.ini")));
    check(store.animationsEnabled(), "an unset store means motion on");
    {
        SettingsPage settingsPage(store);
        QCheckBox *box = settingsPage.animationsField();
        check(box != nullptr, "the settings page carries the motion checkbox");
        check(box && box->isChecked(), "…ticked, because the model says so");
        check(box && box->text() == LanguageManager::tr(QStringLiteral("settings.animations")),
              "…labelled from the catalog rather than from a literal written into the page");
        check(box && box->toolTip() == LanguageManager::tr(QStringLiteral("settings.animations_tip")),
              "…and its tooltip came from the catalog too");
        if (box)
            box->setChecked(false);
        settingsPage.saveButton()->click();
        check(!store.animationsEnabled(), "Save writes an untick through the model");
        check(box && !box->isChecked(), "…and the commit did not re-read the form over the tick");
        store.setAnimationsEnabled(true);        // a write from anywhere else
        check(box && box->isChecked(), "an external write re-reads into the box");
    }

    Motion::setEnabled(wasEnabled);
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

// True when the harness has redirected the config location into the build tree. A window builds its own
// Settings from the process identity and writes the path it locates into that store, so a section that
// constructs one runs only where the redirect took effect — requiring the path to sit under the binary's
// own directory is what makes that safe rather than merely likely: a developer running the suite from a
// shell where XDG_CONFIG_HOME is just ~/.config has that variable set, and a check against it alone would
// let the test write into their real config home. Anywhere else the section skips.
//
// Spelled once because two sections need it: the watchdog's acting half and the release badge.
static bool configRedirectedIntoBuildTree()
{
    const QString buildTree = QCoreApplication::applicationDirPath();
    const QString xdgConfig = qEnvironmentVariable("XDG_CONFIG_HOME");
    const QString configBase =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QDir().mkpath(xdgConfig);   // the store's directory is created on its first write anyway
    const QString canonicalXdg = QDir(xdgConfig).canonicalPath();
    const QString canonicalBuild = QDir(buildTree).canonicalPath();
    return !xdgConfig.isEmpty() && !canonicalXdg.isEmpty()
           && QDir::cleanPath(configBase) == QDir::cleanPath(xdgConfig)
           && canonicalXdg.startsWith(canonicalBuild + QLatin1Char('/'));
}

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
    // See configRedirectedIntoBuildTree() for why this section, and any other that constructs a
    // MainWindow, refuses to run outside a redirected config location.
    if (!configRedirectedIntoBuildTree()) {
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

        // ---- the history page, on the real window ----
        // The page follows its store by itself, so what the window has to get right is narrower and
        // worth pinning: that the page exists in the stack, that it was handed the store the window
        // records into rather than a second one of its own, and that its rail glyph reaches it.
        check(window.historyPage() != nullptr, "the window builds a history page");
        check(window.historyPage()->store() == window.historyStore(),
              "…and shows the record the window writes, not a copy of it");
        const int rowsBefore = window.historyPage()->rowCount();
        window.historyStore()->appendResult(QStringLiteral("fixture"),
                                            QStringLiteral("watch.example"), true, 11, true,
                                            QDateTime::currentDateTime());
        check(window.historyPage()->rowCount() == rowsBefore + 1,
              "a verdict recorded by the window reaches the page without anybody refreshing it");
        window.setCurrentPage(MainWindow::PageHistory);
        check(window.currentPage() == MainWindow::PageHistory,
              "the rail's history glyph has a page to switch to");
        window.showDashboard();
        check(window.currentPage() == MainWindow::PageDashboard,
              "…and the way back is the one every other full-screen page uses");

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

        // ---- motion, on a window that is actually on screen ----
        //
        // Everything above drives a window nobody ever showed, which is the only honest way to test a
        // journal and a watchdog without a desktop. Motion refuses a hidden widget — a page behind
        // another page has nothing to appear on — so the fade, the cascade and the pulse can only be
        // watched on a window that is on screen, and under the offscreen platform that is a call rather
        // than a wish. This is also the only place the window's own three paths (setCurrentPage,
        // populateCards, renderStatus) are checked rather than the primitives they call.
        const bool motionWasOn = Motion::enabled();
        Motion::setEnabled(true);
        window.show();
        QTest::qWait(40);

        // The rows' resting places, taken while nothing animates them, so the entrance is measured
        // against the layout's own answer rather than against a position it had not decided yet.
        Motion::setEnabled(false);
        window.setCurrentPage(MainWindow::PageDashboard);
        window.setCurrentPage(MainWindow::PageProfiles);
        QTest::qWait(40);
        const QList<ProfileCard *> rows = window.cardRows();
        QVector<QPoint> resting;
        for (ProfileCard *card : rows)
            resting.append(card->pos());
        check(rows.size() >= 2,
              QStringLiteral("the fixture list gives the entrance %1 rows to move").arg(rows.size()));
        check(resting.size() >= 2 && resting.at(1).y() > resting.at(0).y(),
              "the list laid its rows out one under the other before anything was asked to move");
        bool installed = false;
        for (ProfileCard *card : rows)
            installed = installed || card->graphicsEffect() != nullptr;
        check(!installed && !Motion::isMoving(window.currentPageWidget()),
              "opening a page with motion off installs nothing and leaves nothing running");

        Motion::setEnabled(true);
        window.setCurrentPage(MainWindow::PageDashboard);
        check(Motion::isMoving(window.currentPageWidget()),
              "opening a page fades the widget the stack is now showing, not the index it was given");
        QTest::qWait(Motion::kPageFadeMs + 120);
        check(!Motion::isMoving(window.currentPageWidget())
                  && window.currentPageWidget()->graphicsEffect() == nullptr,
              "…which ends by itself and takes the effect off, so a page that appeared once stops "
              "paying for it on every later repaint");

        window.setCurrentPage(MainWindow::PageProfiles);
        int rising = 0;
        for (ProfileCard *card : std::as_const(rows))
            rising += Motion::isMoving(card) ? 1 : 0;
        check(rising == qMin(rows.size(), Motion::kCardRiseRows),
              QStringLiteral("%1 of the list's %2 rows rise into place (the cap is %3)")
                  .arg(rising)
                  .arg(rows.size())
                  .arg(Motion::kCardRiseRows));
        QTest::qWait(Motion::kCardRiseMs + Motion::kCardStepMs * Motion::kCardRiseRows + 250);
        int landed = 0;
        for (int i = 0; i < rows.size(); ++i) {
            if (!Motion::isMoving(rows.at(i)) && rows.at(i)->pos() == resting.value(i)
                    && rows.at(i)->graphicsEffect() == nullptr)
                ++landed;
        }
        check(landed == rows.size(),
              "every row finished exactly where the layout had put it with motion off, effect gone — "
              "an entrance that parked a row 14 px low would break the list long after it looked right");

        // The pulse is the window's claim about a backend state, so it is checked through the window.
        window.setCurrentPage(MainWindow::PageDashboard);
        QTest::qWait(Motion::kPageFadeMs + 60);
        PowerButton *power = window.powerButton();
        check(power != nullptr, "the window exposes the button the pulse belongs to");
        window.renderStatusFor(IZapretBackend::State::Running);
        check(power->mode() == PowerButton::Mode::Active && power->isPulsing(),
              "a run that is up breathes the whole control");
        window.renderStatusFor(IZapretBackend::State::Stopping);
        check(power->mode() == PowerButton::Mode::Busy && power->isPulsing(),
              "a teardown breathes too — it is a run coming down, not an absence of one");
        window.renderStatusFor(IZapretBackend::State::Starting);
        check(power->mode() == PowerButton::Mode::Busy && !power->isPulsing(),
              "a launch in flight has no run to be alive yet, so the ring breathes instead of the button");
        QTest::qWait(300);
        check(power->glow() < 1.0, "…and that ring is measurably mid-breath, not merely started");
        window.renderStatusFor(IZapretBackend::State::Stopped);
        check(!power->isPulsing() && power->graphicsEffect() == nullptr
                  && qFuzzyCompare(power->glow(), 1.0),
              "an idle machine is a still button at full brightness: the pulse must not contradict the "
              "word the chip is showing");
        window.hide();
        Motion::setEnabled(motionWasOn);

        store.setLastStrategyId(QString());   // do not leave the fixture's route to a later run
    }

    store.setZapretPath(savedPath);
    store.setLastStrategyId(savedLast);
    store.setStartLastOnLaunch(savedStart);
    store.setWatchdogIntervalMin(savedInterval);
}

// A release answer has to become something a person can see, and the seeing is the window's claim
// rather than the checker's: UpdateChecker decides whether a build is ahead, MainWindow decides what an
// offer looks like. This section hands the window an answer by hand — the same way the watchdog section
// hands it a verdict — because the one method that would produce an answer on its own opens a socket to
// GitHub, and no suite can afford to be waiting on that.
static void testUpdateNotice()
{
    std::printf("\n[Update notice]\n");

    // The delay is a number the feature was specified with, and one a test can hold the code to.
    check(MainWindow::kUpdateCheckDelayMs == 5000,
          "the first lookup waits the five seconds the startup was specified for");

    if (!configRedirectedIntoBuildTree()) {
        std::printf("  SKIP  the notice belongs to a window, and constructing one needs XDG_CONFIG_HOME "
                    "redirected into the build tree (run it through ctest)\n");
        return;
    }

    // A two-profile tree, for the same reason the watchdog section wants one: pointed at nothing, the
    // window opens its settings page instead of settling into the chrome this section is checking.
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory for the fixture tree");
    const QString root = dir.filePath(QStringLiteral("zapret/linux"));
    check(makeWatchdogTree(root), "a two-profile linux tree written");

    Settings store;
    const QString savedPath = store.zapretPath();
    store.setZapretPath(root);

    {
        // Never shown at first, and never asking: scheduleUpdateCheck() is main.cpp's call, which is
        // exactly why a window constructed here cannot reach the network.
        MainWindow window;
        QToolButton *badge = window.updateButton();
        check(badge != nullptr, "the header carries the release badge");
        if (!badge) {
            store.setZapretPath(savedPath);
            return;
        }

        check(badge->objectName() == QLatin1String("windowButton"),
              "…as a control of the window chrome, which is where the design puts it");
        check(badge->property("role").toString() == QLatin1String("update"),
              "…told apart from minimise and close by a role, so both sheets can style it alone");
        check(badge->property("glyph").toString() == QLatin1String("update-available"),
              "…and carrying the glyph property applyTheme() re-bakes, so a theme change keeps it lit");

        // ---- before any answer, nothing is offered ----
        check(badge->isHidden(), "a window nobody has answered yet shows nothing");
        check(!window.updateNoticeShown(), "…and says so through the window's own predicate");
        check(window.updateVersion().isEmpty(), "…with no version on offer to name");

        // ---- the answer arrives ----
        const int lines = window.logPanel()->storedLineCount();
        window.onUpdateAvailable(QStringLiteral("v0.5.0"),
                                 QStringLiteral("https://example.invalid/releases/tag/v0.5.0"));
        check(window.updateNoticeShown(),
              "an answer naming a newer build raises the badge — the visible half of the feature");
        check(!badge->isHidden(), "…which is on its way to being seen, window on screen or not");
        check(window.updateVersion() == QLatin1String("v0.5.0"),
              "…quoting the tag the feed published rather than a re-spelling of it");
        check(window.logPanel()->storedLineCount() == lines + 1,
              "…and recorded in the journal as well as shown, so the offer outlives the glance");

        // The words the press will ask with, read out of the catalog rather than written here.
        const QString question = LanguageManager::tr(QStringLiteral("update.question"),
                                                     QStringLiteral("v0.5.0"));
        check(question.contains(QStringLiteral("v0.5.0")),
              "the question names the version it is offering");
        check(!LanguageManager::tr(QStringLiteral("update.title")).isEmpty(),
              "…and the dialog has a title to ask it under");
        check(!LanguageManager::tr(QStringLiteral("update.badge_tip")).isEmpty(),
              "…and the icon-only control has an accessible name, which is what a glyph with no label "
              "leaves a screen reader to read");

        // A second answer replaces the offer rather than stacking a second badge on it or losing the
        // first: the feed can only be ahead by one release, and pressing it must open the newest page.
        window.onUpdateAvailable(QStringLiteral("v0.6.0"),
                                 QStringLiteral("https://example.invalid/releases/tag/v0.6.0"));
        check(window.updateVersion() == QLatin1String("v0.6.0"),
              "a newer answer replaces the version the badge is offering");
        check(window.updateNoticeShown(), "…without taking the offer away");

        // ---- the pulse, on a badge that can be seen ----
        // Breathing is a claim about a widget on screen, so it is checked the way the motion section
        // checks every other one: with the window actually shown, under the offscreen platform.
        const bool motionWasOn = Motion::enabled();
        Motion::setEnabled(true);
        window.show();
        QTest::qWait(40);
        check(badge->isVisible(), "the badge is on screen once its window is");
        check(Motion::isBreathing(badge),
              "…and breathing, which is what tells a person an icon has just arrived rather than always "
              "lived in that corner");
        Motion::setEnabled(false);
        window.onUpdateAvailable(QStringLiteral("v0.7.0"), QStringLiteral("https://example.invalid/r"));
        check(!Motion::isBreathing(badge) && window.updateNoticeShown(),
              "with motion off the offer still arrives: the switch governs the movement, never the news");
        Motion::setEnabled(motionWasOn);
        window.hide();
    }

    store.setZapretPath(savedPath);
}

// ------------------------------------------------------------- rounded shell ----
//
// The window's own corners. Everything above asks what the window contains; this asks what the window IS —
// a rounded rectangle with no system frame and, since the shell, no background of its own. A shape cannot
// be checked by reading a property, so the window is rendered and the rendering is read: the shell paints
// an arc, and what lies outside it is not a colour at all. That last part is the whole trick —
// QWidget::grab() fills its target with Qt::transparent before rendering, so an alpha of 0 in the image is
// the desktop showing through, which is exactly the fact this section turns on.
//
// Four ways it can be wrong, each with its own reading:
//   * no band — a card flush with the surface edge has its 1 px stroke cut in half by that edge, so the
//     window's own outline is a smudge instead of a line;
//   * a painted window or canvas — an opaque rectangle again, corners and all, with the rounding hidden
//     behind it: the black squares this feature exists to remove, in whatever colour the theme uses;
//   * a square corner inside the arc — the rail, the main column, the header band and the footer are
//     children, and Qt does not clip a child to its parent's radius, so an unrounded one paints a nub of
//     the app outside the silhouette and over the stroke;
//   * the hierarchy — the shell sits between the window and everything the suite finds by name, so a page,
//     a rail glyph or a chrome control that stopped being reachable, or a header press that stopped
//     reaching the window that drags on it, is the same feature failing quietly;
//   * the shadow — the band is the shadow's room, and the two readings that matter are that it fades instead
//     of stepping, and that by the time it reaches the surface's own edge only a tail is left there. A band
//     narrowed for any reason puts the body of the shadow against the window border instead: a hard line, and
//     the cheapest of these to miss.
//
// The shape and the shadow cannot be read out of one render, because a drop shadow paints into the very band
// the corner tests measure: "nothing is painted here" is only a claim about a shadow-free window. So the
// window is drawn twice, once with the effect switched off and once with it on, and every reading below is
// made from the picture it can be trusted in.
// As with the stats panel, the light theme proves what the dark one cannot: there the rail is #E5E7EB
// against a #F3F4F6 shell, so a leaked corner is a grey pixel outside the arc, while the dark sheet paints
// both #0B0C10 and hides the mistake in a matching colour. Every reading below therefore runs in both
// themes, against the colours each sheet declares rather than colours written here.
static void testWindowFrame()
{
    std::printf("\n[Rounded shell]\n");

    QApplication &app = *static_cast<QApplication *>(qApp);
    const int r = MainWindow::kWindowFrameRadiusPx;
    const int m = MainWindow::kWindowFrameMarginPx;
    const int b = MainWindow::kWindowFrameBorderPx;

    // The numbers the shell and its shadow are made of, spelled once more here so a change to one of them
    // has to pass two places that would otherwise disagree quietly.
    check(r == 12 && b == 1, "the shell is 12 px of arc over a 1 px stroke");
    // The band is not a free design value any more: it is the shadow's reach, and the shadow is the reason
    // the surface's own edge no longer falls inside it. QGraphicsDropShadowEffect blurs with a radius of half
    // its blurRadius and the offset pushes the whole curve down, so the band is that reach plus a pixel. The
    // tail does keep going past the band — as a tail; the ring reading below is what says so.
    const int reach = MainWindow::kWindowShadowBlurPx / 2;
    check(m == reach + MainWindow::kWindowShadowOffsetYpx + 1,
          QStringLiteral("…inside a band of exactly the reach the shadow needs (%1 px of blur radius, %2 px "
                         "of offset, %3 px over the end of it = %4 px)")
              .arg(reach).arg(MainWindow::kWindowShadowOffsetYpx).arg(1).arg(m));
    check(m > reach, QStringLiteral("…which does mean the band is wider than the stroke it protects (%1 px "
                                    "of it is only there for the shadow)").arg(m));

    if (!configRedirectedIntoBuildTree()) {
        std::printf("  SKIP  the shell belongs to a window, and constructing one needs XDG_CONFIG_HOME "
                    "redirected into the build tree (run it through ctest)\n");
        return;
    }

    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory for the fixture tree");
    const QString root = dir.filePath(QStringLiteral("zapret/linux"));
    check(makeWatchdogTree(root), "a two-profile linux tree written");

    Settings store;
    const QString savedPath = store.zapretPath();
    const QString savedLast = store.lastStrategyId();
    const bool savedStart = store.startLastOnLaunch();
    store.setStartLastOnLaunch(false);   // nothing may start itself while its corners are being read
    store.setLastStrategyId(QString());
    store.setZapretPath(root);

    {
        MainWindow window;
        // ---- the hierarchy the rest of the suite reads through ----
        QWidget *canvas = window.findChild<QWidget *>(QStringLiteral("rootCanvas"));
        QFrame *shell = window.windowFrame();
        check(canvas != nullptr && canvas->parentWidget() == &window,
              "the central widget is still the window's own canvas");
        check(shell != nullptr, "the window builds a rounded shell");
        if (!canvas || !shell) {
            store.setZapretPath(savedPath);
            store.setLastStrategyId(savedLast);
            store.setStartLastOnLaunch(savedStart);
            return;
        }
        check(shell->objectName() == QLatin1String("mainWindowFrame"),
              "…under the name both sheets style it by");
        check(shell->parentWidget() == canvas,
              "…between that canvas and everything else, which is the one thing the new layer changes");

        // ---- the shadow, as an object ----
        //
        // Attached to the canvas, with the parameters the constants name. The pixels below can show that a
        // shadow happened; only this can show that it is the shadow this file declares, and that it hangs off
        // the one widget whose rect includes the band it has to fall through.
        auto *shadow = qobject_cast<QGraphicsDropShadowEffect *>(canvas->graphicsEffect());
        check(shadow != nullptr, "the window's canvas carries a drop shadow");
        check(shadow && shadow->blurRadius() == MainWindow::kWindowShadowBlurPx,
              QStringLiteral("…blurred by %1 px").arg(MainWindow::kWindowShadowBlurPx));
        check(shadow && shadow->offset() == QPointF(0, MainWindow::kWindowShadowOffsetYpx),
              QStringLiteral("…pushed %1 px down, so the window reads as lit from above")
                  .arg(MainWindow::kWindowShadowOffsetYpx));
        check(shadow && shadow->color().alpha() == MainWindow::kWindowShadowAlpha
                      && shadow->color().red() == 0 && shadow->color().green() == 0
                      && shadow->color().blue() == 0,
              QStringLiteral("…in %1 of plain black rather than a colour of its own")
                  .arg(MainWindow::kWindowShadowAlpha));
        // Not on the card, and not on the window: a drop shadow paints outside the shape it came from, and
        // the card's rect stops at the card. An effect moved there would be a shadow with nowhere to go.
        check(shell->graphicsEffect() == nullptr, "…and the card itself owns no effect to clip it");
        check(window.graphicsEffect() == nullptr, "…nor does the window, which is not the band's owner");
        // The claim Task 3 makes is that findChild() still reaches everything, because it walks the tree
        // rather than naming a parent. Four kinds of thing the suite leans on: the page stack, a rail
        // glyph, the footer at the bottom of the column, and a page built several levels below the shell.
        check(window.findChild<QStackedWidget *>(QStringLiteral("pageStack")) != nullptr,
              "the page stack is reachable through the shell");
        check(window.findChild<QPushButton *>(QStringLiteral("navButton")) != nullptr,
              "…and so is a rail glyph");
        check(window.findChild<QFrame *>(QStringLiteral("footerBar")) != nullptr,
              "…and the footer at the bottom of the column");
        check(window.findChild<QWidget *>(QStringLiteral("historyPage")) != nullptr,
              "…and a page several levels below the shell");

        // A claim about a shape needs the window on screen and rendered. The offscreen platform is enough:
        // grab() draws the widget tree into its own image and asks no compositor for anything. Showing it
        // is also what puts the layout to work, so every geometry read below comes after it.
        window.show();
        QTest::qWait(60);

        // ---- the band, as geometry ----
        check(shell->pos() == QPoint(m, m),
              QStringLiteral("the shell starts %1 px in from the surface on both axes (%2,%3)")
                  .arg(m).arg(shell->pos().x()).arg(shell->pos().y()));
        check(shell->width() == window.width() - 2 * m
                  && shell->height() == window.height() - 2 * m,
              QStringLiteral("…and stops %1 px before it again (%2x%3 inside a %4x%5 window)")
                  .arg(m).arg(shell->width()).arg(shell->height())
                  .arg(window.width()).arg(window.height()));

        // ---- the chrome controls, where the design puts them ----
        QToolButton *closeBtn = nullptr;
        QToolButton *minBtn = nullptr;
        const auto chromeButtons = window.findChildren<QToolButton *>();
        for (QToolButton *btn : chromeButtons) {
            const QString role = btn->property("role").toString();
            if (role == QLatin1String("close"))
                closeBtn = btn;
            else if (role == QLatin1String("min"))
                minBtn = btn;
        }
        check(closeBtn != nullptr && minBtn != nullptr,
              "the minimise and close controls are still built into the header");
        // Inside the card, clear of its stroke: a control that overlapped the 1 px border would be the
        // design's top-right corner cut away by its own button.
        const QRect shellInner = QRect(shell->pos(), shell->size())
                                     .adjusted(b, b, -b - 1, -b - 1);
        for (QToolButton *btn : {closeBtn, minBtn}) {
            if (!btn)
                continue;
            const QRect g(btn->mapTo(&window, QPoint(0, 0)), btn->size());
            check(shellInner.contains(g),
                  QStringLiteral("the %1 control sits inside the shell, clear of its stroke")
                      .arg(btn->property("role").toString()));
            check(g.center().x() > window.width() / 2 && g.center().y() < window.height() / 4,
                  QStringLiteral("…and still in the top-right corner, where window furniture goes (%1,%2)")
                      .arg(g.x()).arg(g.y()));
        }
        const int w = window.width();
        const int h = window.height();
        // One mirrored coordinate for all four corners: (lx, ly) counts away from the corner in question,
        // which puts every corner's arc centre at (m + r, m + r) and lets one arithmetic read all of them.
        struct Corner {
            const char *name;
            int x0;   // the window x of the corner's own pixel
            int dx;   // …and the direction that goes into the window
            int y0;
            int dy;
            const char *surface;   // the selector whose colour must be found inside the arc
        };
        const Corner corners[] = {
            {"top-left",      0, 1,      0,  1, "QFrame#sidebar"},
            {"top-right",  w - 1, -1,     0,  1, "QFrame#mainArea"},
            {"bottom-left",   0, 1,   h - 1, -1, "QFrame#sidebar"},
            {"bottom-right", w - 1, -1, h - 1, -1, "QFrame#mainArea"},
        };
        auto at = [](const Corner &c, int lx, int ly) {
            return QPoint(c.x0 + c.dx * lx, c.y0 + c.dy * ly);
        };
        // A pixel of the corner box is painted by the shell exactly when its centre falls inside the outer
        // arc. A square corner paints every one of them, which is what the count below tells apart.
        auto arcPaints = [&](int lx, int ly) {
            const double dx = lx + 0.5 - (m + r);
            const double dy = ly + 0.5 - (m + r);
            return dx * dx + dy * dy <= double(r * r);
        };
        // How far in from the corner the 45° diagonal crosses the arc, in that mirrored coordinate: the
        // pixel that must be nothing if the corner is round, and something if any child left it square.
        const int diagOut = m + r - int((r + 1) / 1.4142135623730951) - 1;
        auto rgbWithin = [](const QColor &x, const QColor &y, int tol) {
            return qAbs(x.red() - y.red()) <= tol && qAbs(x.green() - y.green()) <= tol
                   && qAbs(x.blue() - y.blue()) <= tol;
        };
        // Two renders of the same window, told apart only by whether the shadow is switched on. Disabling a
        // QGraphicsEffect makes Qt draw the source as though nothing were attached, which is the whole of
        // what the shape tests need — and re-enabling it afterwards leaves the window as the user would see
        // it, so no reading below can be the one that forgot to put the shadow back.
        auto grabWindow = [&](bool withShadow) {
            QGraphicsEffect *fx = canvas->graphicsEffect();
            if (fx)
                fx->setEnabled(withShadow);
            const QImage img = window.grab().toImage().convertToFormat(QImage::Format_ARGB32);
            if (fx)
                fx->setEnabled(true);
            return img;
        };

        for (const bool dark : {true, false}) {
            const QString theme = dark ? QStringLiteral("dark") : QStringLiteral("light");
            Theme::apply(app, dark);
            QTest::qWait(30);
            const QImage shot = grabWindow(false);    // the silhouette, uncast
            const QImage cast = grabWindow(true);     // …and the same window wearing its shadow
            check(shot.hasAlphaChannel() && shot.width() == w && shot.height() == h
                          && cast.width() == w && cast.height() == h,
                  QStringLiteral("%1: the window renders %2x%3 with an alpha channel for corners to hide in")
                      .arg(theme).arg(shot.width()).arg(shot.height()));
            if (!shot.hasAlphaChannel() || shot.width() != w || shot.height() != h
                || cast.width() != w || cast.height() != h)
                continue;

            const QString sheet = app.styleSheet();
            const QColor stroke = sheetRuleHex(sheet, QStringLiteral("QFrame#mainWindowFrame"),
                                               QStringLiteral("border"));
            check(stroke.isValid(),
                  QStringLiteral("%1: the sheet declares the shell's stroke colour").arg(theme));

            // ---- the four corners ----
            for (const Corner &c : corners) {
                const QColor wantFill = sheetRuleColor(sheet, QLatin1String(c.surface),
                                                       QStringLiteral("background-color"));
                check(wantFill.isValid(),
                      QStringLiteral("%1: the sheet says what %2 paints at a corner")
                          .arg(theme, QLatin1String(c.surface)));

                // Nothing of the app may be at the window's very corner pixel…
                check(qAlpha(shot.pixel(at(c, 0, 0))) == 0,
                      QStringLiteral("%1: the window's %2 corner pixel is empty, not %3")
                          .arg(theme, QLatin1String(c.name))
                          .arg(shot.pixelColor(at(c, 0, 0)).name(QColor::HexRgb)));
                // …nor one step further in along the diagonal, which is exactly where a square child
                // corner — an unrounded rail, band or footer — would still be reaching.
                const QPoint out = at(c, diagOut, diagOut);
                check(qAlpha(shot.pixel(out)) == 0,
                      QStringLiteral("%1: nothing is painted at (%3,%4) of the %2 corner, so the corner is "
                                     "an arc and not a child that forgot to round")
                          .arg(theme, QLatin1String(c.name)).arg(out.x()).arg(out.y()));
                // And the corner is filled with its own surface, not a hole in the shell: proof the clean
                // diagonal is a shape rather than a window that stopped drawing. Four pixels short of the
                // arc's own centre along each axis puts the sample well inside the arc, clear of the stroke
                // and of the antialiased edge alike — counted from the corner of the CARD, since the band
                // between the card and the surface is a window's worth of pixels that must not be measured.
                const QPoint inside = at(c, m + r - 4, m + r - 4);
                check(rgbWithin(shot.pixelColor(inside), wantFill, 12),
                      QStringLiteral("%1: the %2 corner is painted by %3 (%4 at (%5,%6), wanted %7)")
                          .arg(theme, QLatin1String(c.name), QLatin1String(c.surface))
                          .arg(shot.pixelColor(inside).name(QColor::HexRgb))
                          .arg(inside.x()).arg(inside.y())
                          .arg(wantFill.name(QColor::HexRgb)));

                int painted = 0;
                int expected = 0;
                // The r x r box between the arc's centre and the corner it rounds — the one box whose
                // painted count separates an arc (about three quarters of it) from a square child (all of
                // it). It starts at the band rather than at the surface, so widening the band moves the box
                // with it instead of leaving it counting an empty margin twice over.
                for (int ly = m; ly < m + r; ++ly) {
                    for (int lx = m; lx < m + r; ++lx) {
                        if (qAlpha(shot.pixel(at(c, lx, ly))) > 128)
                            ++painted;
                        if (arcPaints(lx, ly))
                            ++expected;
                    }
                }
                check(qAbs(painted - expected) <= 14,
                      QStringLiteral("%1: the %2 corner is the arc, not a square — %3 of the %4x%4 box "
                                     "painted, the arc says %5 and a square would be %6")
                          .arg(theme, QLatin1String(c.name)).arg(painted).arg(r)
                          .arg(expected).arg(r * r));
            }
            // ---- the four straight edges: the band, as pixels ----
            // Nothing antialiased to argue with here, and this is the reading that proves the margin: the
            // first pixel the window paints on an edge has to be the shell's stroke, one band in from the
            // surface's own edge. Flush against that edge, the same stroke is half a pixel of smudge.
            struct Edge {
                const char *name;
                int dx;   // which axis is the surface normal, and in which direction
                int dy;
            };
            const Edge edges[] = {{"top", 0, -1}, {"bottom", 0, 1}, {"left", -1, 0}, {"right", 1, 0}};
            for (const Edge &e : edges) {
                // The stroke pixel, and the one before it on the way out of the window.
                const QPoint on(e.dx < 0 ? m : (e.dx > 0 ? w - 1 - m : w / 2),
                                e.dy < 0 ? m : (e.dy > 0 ? h - 1 - m : h / 2));
                const QPoint out(on.x() + e.dx, on.y() + e.dy);
                check(shot.rect().contains(out) && qAlpha(shot.pixel(out)) == 0,
                      QStringLiteral("%1: the %2 edge is transparent for the whole %3 px band, so the "
                                     "window really is inset from its own surface")
                          .arg(theme, QLatin1String(e.name)).arg(m));
                check(qAlpha(shot.pixel(on)) > 200 && rgbWithin(shot.pixelColor(on), stroke, 16),
                      QStringLiteral("%1: the %2 edge draws the shell's stroke at (%3,%4) — %5, wanted %6")
                          .arg(theme, QLatin1String(e.name)).arg(on.x()).arg(on.y())
                          .arg(shot.pixelColor(on).name(QColor::HexRgb))
                          .arg(stroke.isValid() ? stroke.name(QColor::HexRgb)
                                                : QStringLiteral("<none>")));
            }

            // ---- the shadow in that band ----
            //
            // The band is empty in `shot` by construction, and the claim of this block is that it stops being
            // empty in `cast`. Three readings, in the order anyone notices them: that there is shadow at all,
            // that it falls off instead of ending in a step, and that it is spent before it reaches the
            // surface's own edge. The last is what the band's width exists for, and the first to fail if the
            // blur grows or the margin shrinks without the other being asked.
            auto bandAlpha = [&](const QPoint &p) { return qAlpha(cast.pixel(p)); };
            int maxNear = 0;
            for (const Edge &e : edges) {
                // The same mid-edge column the band is measured on, walked outward from the stroke. Half a
                // blur radius out is the far sample: still inside the fade on every edge, because the offset
                // only ever lengthens the shadow's reach downward and never shortens it below the radius.
                const QPoint on(e.dx < 0 ? m : (e.dx > 0 ? w - 1 - m : w / 2),
                                e.dy < 0 ? m : (e.dy > 0 ? h - 1 - m : h / 2));
                const QPoint near(on.x() + e.dx, on.y() + e.dy);
                const QPoint far(on.x() + e.dx * (reach / 2), on.y() + e.dy * (reach / 2));
                maxNear = qMax(maxNear, bandAlpha(near));
                check(bandAlpha(near) >= 16,
                      QStringLiteral("%1: the %2 edge casts into the band — %3 of alpha one pixel outside "
                                     "the stroke").arg(theme, QLatin1String(e.name)).arg(bandAlpha(near)));
                check(bandAlpha(near) <= MainWindow::kWindowShadowAlpha,
                      QStringLiteral("%1: …and never more opaque than the %2 of black the shadow was given "
                                     "(%3 here on the %4 edge)")
                          .arg(theme).arg(MainWindow::kWindowShadowAlpha).arg(bandAlpha(near))
                          .arg(QLatin1String(e.name)));
                check(bandAlpha(near) > bandAlpha(far),
                      QStringLiteral("%1: the %2 shadow fades rather than steps (%3 at the stroke, %4 half a "
                                     "radius out)")
                          .arg(theme, QLatin1String(e.name)).arg(bandAlpha(near)).arg(bandAlpha(far)));
                const QColor ink = cast.pixelColor(near);
                check(ink.red() <= 8 && ink.green() <= 8 && ink.blue() <= 8,
                      QStringLiteral("%1: the %2 shadow is black, not a colour of the app's own (%3)")
                          .arg(theme, QLatin1String(e.name)).arg(ink.name(QColor::HexArgb)));
            }

            // Every pixel of the surface's own outer ring, no exceptions, and no claim of exact zero: a
            // Gaussian tail is still falling when it arrives, so what the band has to guarantee is that the
            // shadow is *nothing there is a chance of seeing* by then — under a tenth of the black it was made
            // of, and under an eighth of its own strength one pixel off the stroke. Measured, the band ends at
            // four of alpha here and seventy-five at the stroke. Narrow it to six pixels and the ring stops
            // being a tail and starts being the shadow's body — thirty-seven is a step you can see on a light
            // desktop, and that is exactly the cut this reading is here to hear about.
            int ringMax = 0;
            for (int x = 0; x < w; ++x) {
                ringMax = qMax(ringMax, bandAlpha(QPoint(x, 0)));
                ringMax = qMax(ringMax, bandAlpha(QPoint(x, h - 1)));
            }
            for (int y = 0; y < h; ++y) {
                ringMax = qMax(ringMax, bandAlpha(QPoint(0, y)));
                ringMax = qMax(ringMax, bandAlpha(QPoint(w - 1, y)));
            }
            check(ringMax <= MainWindow::kWindowShadowAlpha / 10,
                  QStringLiteral("%1: the shadow is spent at the surface edge — its strongest ring pixel is "
                                 "%2 of the %3 it was given, and a cut would read as a step")
                      .arg(theme).arg(ringMax).arg(MainWindow::kWindowShadowAlpha));
            check(ringMax * 8 <= maxNear,
                  QStringLiteral("%1: …and that ring pixel is a tail, not the shadow itself (%2 against the "
                                 "%3 it carries one pixel off the stroke)")
                      .arg(theme).arg(ringMax).arg(maxNear));

            // The four corners of the cast window in particular: a shadow that pooled in the surface's corner
            // would square the window again in the one place the whole shell exists to round it.
            int cornerMax = 0;
            for (const QPoint &p : {QPoint(0, 0), QPoint(w - 1, 0), QPoint(0, h - 1), QPoint(w - 1, h - 1)})
                cornerMax = qMax(cornerMax, bandAlpha(p));
            check(cornerMax <= MainWindow::kWindowShadowAlpha / 20,
                  QStringLiteral("%1: the shadow leaves the window's corners empty, so the silhouette is "
                                 "still the arc (%2 at the strongest)")
                      .arg(theme).arg(cornerMax));

            // And the shadow falls outside the card and nowhere else. A drop shadow that also laid over the
            // app's own pixels would read as a dirty window, and it would be the cheapest way for this
            // feature to get the theme's colours wrong while every corner reading above still passed.
            QFrame *rail = window.findChild<QFrame *>(QStringLiteral("sidebar"));
            if (rail) {
                const QPoint railCentre =
                    rail->mapTo(&window, QPoint(rail->width() / 2, rail->height() / 2));
                check(cast.rect().contains(railCentre) && qAlpha(cast.pixel(railCentre)) == 255,
                      QStringLiteral("%1: the middle of the rail is still fully opaque under the shadow")
                          .arg(theme));
                check(rgbWithin(shot.pixelColor(railCentre), cast.pixelColor(railCentre), 2),
                      QStringLiteral("%1: …and the shadow has not tinted it (%2 with, %3 without)")
                          .arg(theme).arg(cast.pixelColor(railCentre).name(QColor::HexRgb))
                          .arg(shot.pixelColor(railCentre).name(QColor::HexRgb)));
            }

            // ---- the chrome controls, inside the silhouette ----
            // Geometry proved they are clear of the stroke; this proves it in the same pixels the corners
            // are judged by: every corner of every control lands on a painted pixel, so none of them is
            // sticking out past the arc the window is made of.
            for (QToolButton *btn : {closeBtn, minBtn}) {
                if (!btn)
                    continue;
                const QRect g(btn->mapTo(&window, QPoint(0, 0)), btn->size());
                bool allPainted = true;
                for (const QPoint &p : {g.topLeft(), g.topRight(), g.bottomLeft(), g.bottomRight()}) {
                    if (!shot.rect().contains(p) || qAlpha(shot.pixel(p)) == 0)
                        allPainted = false;
                }
                check(allPainted,
                      QStringLiteral("%1: every corner of the %2 control lands on painted pixels, so it is "
                                     "inside the rounded area")
                          .arg(theme, btn->property("role").toString()));
            }

            // The shell as the user sees it, kept for the eye to check the arc it cannot measure — and the
            // same window without its shadow beside it, because the difference between the two is the one
            // part of this feature a screenshot cannot be argued about.
            shot.save(dark ? QStringLiteral("/tmp/shell-dark.png")
                           : QStringLiteral("/tmp/shell-light.png"));
            cast.save(dark ? QStringLiteral("/tmp/shadow-dark.png")
                           : QStringLiteral("/tmp/shadow-light.png"));
        }

        // ---- the drag, across the new layer ----
        //
        // A press on bare header space is not a press on the window: it lands on the band, is ignored
        // there, and travels up through the shell to the window that starts a drag from it. The shell put
        // a layer into that journey, and a frameless window that cannot be dragged is a window that cannot
        // be moved at all — a much worse failure than a wrong colour. So the press is delivered to a child
        // and the window's own position is what is measured.
        Theme::apply(app, true);
        QTest::qWait(30);
        QWidget *band = window.findChild<QWidget *>(QStringLiteral("headerBar"));
        check(band != nullptr, "the header band is reachable by name for the drag to start on");
        if (band) {
            check(band->width() > 200 && band->height() > 24,
                  QStringLiteral("…and is big enough to grab by (%1x%2)").arg(band->width())
                      .arg(band->height()));
            const QPoint grabPoint(band->width() / 2, 20);   // between the title and the status chip
            const QRect closeInBand = closeBtn
                                          ? QRect(closeBtn->mapTo(band, QPoint(0, 0)), closeBtn->size())
                                          : QRect();
            check(!closeInBand.contains(grabPoint),
                  "…the grab point is bare header space, not one of the controls sitting on it");

            const QPoint before = window.pos();
            const QPoint p1 = band->mapTo(&window, grabPoint);
            QTest::mousePress(band, Qt::LeftButton, Qt::NoModifier, grabPoint);
            const QPoint p2 = p1 + QPoint(27, 7);
            QTest::mouseMove(&window, p2);
            const QPoint moved = window.pos();
            check(moved != before,
                  QStringLiteral("a press on the header and a drag move the frameless window ((%1,%2) -> "
                                 "(%3,%4))").arg(before.x()).arg(before.y())
                      .arg(moved.x()).arg(moved.y()));
            check(moved - before == p2 - p1,
                  QStringLiteral("…by exactly as far as the pointer went, so the extra layer did not shift "
                                 "the grab point (window moved (%1,%2), pointer (%3,%4))")
                      .arg(moved.x() - before.x()).arg(moved.y() - before.y())
                      .arg(p2.x() - p1.x()).arg(p2.y() - p1.y()));
            QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, p2);

            // A second drag, measured from a second press. If the release had left the window mid-drag, or
            // the offset had carried over from the first press point, this jump would not be the
            // pointer's own.
            const QPoint againFrom = window.pos();
            const QPoint q1 = band->mapTo(&window, grabPoint);
            QTest::mousePress(band, Qt::LeftButton, Qt::NoModifier, grabPoint);
            QTest::mouseMove(&window, q1 + QPoint(-19, 11));
            const QPoint againTo = window.pos();
            check(againTo - againFrom == QPoint(-19, 11),
                  QStringLiteral("…and the next drag is its own, not a carry-over from the last (moved "
                                 "(%1,%2), expected (-19,11))")
                      .arg(againTo.x() - againFrom.x()).arg(againTo.y() - againFrom.y()));
            QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, q1 + QPoint(-19, 11));

            // And below the band a press belongs to the page rather than to the window: the same journey
            // up ends with the drag handler refusing it, so clicking anything never drags the app about.
            const QPoint still = window.pos();
            QWidget *page = window.findChild<QWidget *>(QStringLiteral("dashboardPage"));
            if (page && page->height() > 60) {
                const QPoint onPage(page->width() / 2, page->height() - 30);
                const QPoint onWindow = page->mapTo(&window, onPage);
                check(onWindow.y() > 64 + m + b,
                      QStringLiteral("…that press is below the drag band (%1 px into a %2 px window)")
                          .arg(onWindow.y()).arg(window.height()));
                QTest::mousePress(page, Qt::LeftButton, Qt::NoModifier, onPage);
                QTest::mouseMove(&window, onWindow + QPoint(30, 30));
                check(window.pos() == still,
                      "…and dragging from the page leaves the window where it was");
                QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier,
                                    onWindow + QPoint(30, 30));
            }
        }

        // ---- the drag, from the band the shadow falls through ----
        //
        // The band is a window's worth of pixels wide now, because that is what the shadow needs, and every
        // one of them is a place the pointer can land without touching a widget. They have to stay grabbable:
        // a shadow is paint, and paint does not take the mouse. So the press goes to the canvas itself, in the
        // middle of the top band and on nothing but fall-off, and the window's own position is what proves
        // the drag reached the handler that moves it.
        {
            const QPoint inBand(w / 2, m / 2);
            check(inBand.y() > 0 && inBand.y() < m,
                  QStringLiteral("…the press point is in the band and not on the card (%1 px of %2)")
                      .arg(inBand.y()).arg(m));
            const QPoint before = window.pos();
            QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, inBand);
            const QPoint after = inBand + QPoint(15, -9);
            QTest::mouseMove(&window, after);
            const QPoint moved = window.pos();
            QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, after);
            check(moved - before == QPoint(15, -9),
                  QStringLiteral("a press on the shadowed band still drags the frameless window (moved "
                                 "(%1,%2), expected (15,-9))").arg(moved.x() - before.x())
                      .arg(moved.y() - before.y()));
        }

        // Back on the theme the rest of the binary — and the application itself — expects.
        Theme::apply(app, true);
        window.hide();
    }

    store.setZapretPath(savedPath);
    store.setLastStrategyId(savedLast);
    store.setStartLastOnLaunch(savedStart);
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
    testHistoryPage();
    testMotion();
    testWatchdog();
    testUpdateNotice();
    testWindowFrame();

    std::printf("\n=== %s (%d failures) ===\n", g_failures ? "FAILED" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
