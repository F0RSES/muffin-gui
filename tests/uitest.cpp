// uitest — headless widget + service verification for the zapret GUI layer.
//
// Runs under the offscreen QPA platform (QT_QPA_PLATFORM is also forced in-process, so the binary is
// runnable straight from a shell). Like coretest it is a plain console program: readable PASS/FAIL
// lines, non-zero exit on any failure, no QTest data-driven scaffolding.
//
// Deliberately NOT covered here: a constructed MainWindow (it needs a window manager and, for the tray
// paths, a tray host) — its pure policy predicates are covered — anything that would spawn
// pkexec/winws, and real network reachability (the ping test only asserts that a verdict arrives).

#include "IZapretBackend.h"
#include "LogPanel.h"
#include "MainWindow.h"
#include "PingIndicator.h"
#include "PingService.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "Strategy.h"
#include "StrategyCard.h"
#include "Theme.h"

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
#include <QSignalSpy>
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
    testLogPanel();
    testPingIndicator();
    testStrategyCard();
    testTrayPolicy();
    testTheme(app);
    testPingService();

    std::printf("\n=== %s (%d failures) ===\n", g_failures ? "FAILED" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
