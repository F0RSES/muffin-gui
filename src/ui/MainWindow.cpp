#include "MainWindow.h"

#include "BlockcheckEngine.h"
#include "DownloadDialog.h"
#include "HistoryPage.h"
#include "HistoryStore.h"
#include "LanguageManager.h"
#include "LogPanel.h"
#include "Motion.h"
#include "PingIndicator.h"
#include "PingService.h"
#include "PowerButton.h"
#include "ProfileCard.h"
#include "Settings.h"
#include "SettingsPage.h"
#include "Theme.h"
#include "ZapretManager.h"
#include "ZapretPaths.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSize>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace zapret {

namespace {

// Two identical failures inside this window produce one dialog, not two. The log and the card badge
// still record every occurrence — only the modal interruption is rate-limited.
constexpr qint64 kErrorDialogWindowMs = 5000;

// A DE whose tray host starts after us (panel/indicator extension still loading) must not cost the
// session its tray: re-ask a few times before settling for "no tray here".
constexpr int kTrayRetryIntervalMs = 1500;
constexpr int kTrayRetryMaxAttempts = 6;

// "youtube.com" -> "Youtube": the chip shows a friendly name and keeps the real host in its tooltip.
QString friendlyHostName(const QString &host)
{
    const QString base = host.section(QLatin1Char('.'), 0, 0);
    if (base.isEmpty())
        return host;
    return base.left(1).toUpper() + base.mid(1);
}

// The toolbar stop's standing tooltip. updateStatusUi() replaces it with the reason the action is
// unavailable while a teardown runs, and has to restore this exact text afterwards — spelling it twice
// is how the two drift. Functions, not constants: the text is the current language's, and a static
// QString would freeze whichever language happened to be loaded when the binary first touched it.
QString stopActionHint()
{
    return LanguageManager::tr(QStringLiteral("hint.stop"));
}

// The power button's other standing tooltip — what it says when there is nothing to stop.
QString startActionHint()
{
    return LanguageManager::tr(QStringLiteral("hint.start"));
}

// The window is fixed, not resizable and never maximised (see the constructor): 900x700 is the canvas
// the mock is drawn on, and every gap below is measured against it.
constexpr int kWindowWidth = 900;
constexpr int kWindowHeight = 700;

// The rail is fixed, not resizable: it carries glyphs only, and 70 px around a 24 px icon is the width
// the mock specifies and the one that never needs to change.
constexpr int kSidebarWidth = 70;
constexpr int kRailGlyphPx = 24;
constexpr int kBrandGlyphPx = 28;
// The language picker's flag. 22x16 is the aspect the SVGs are drawn at; the button gives it a 46x30
// hit area, the same width as the glyphs above it so the rail reads as one column.
constexpr int kFlagGlyphPx = 22;

// The dashboard's circle. 200 px of rim, and PowerButton adds its own halo margin on every side, so the
// widget is taller than this — which is why the spacing below is measured from the widget, not the ring.
constexpr int kPowerDiameterPx = 200;
// Button to ping badges, as specified.
constexpr int kPowerToPingsGap = 60;
// Badge to badge.
constexpr int kPingSpacing = 50;
// The header's connected dot: 6 px across, so a 3 px radius.
constexpr int kStatusDotPx = 6;

// How far down from the top the frameless window can be grabbed by (see mousePressEvent). The header band
// is 18 px top margin + the tallest of its children (the 30 px chrome buttons / the 24 px title) + 16 px
// bottom margin + the 1 px rule, so ~64 px covers the whole header and nothing below it. The two chrome
// buttons and the rail glyphs are children that consume their own presses, so a press that reaches
// MainWindow inside this band is on bare header space and safe to drag from.
constexpr int kHeaderDragHeight = 64;

// How long a manual ping round may hold the button. The service's own budgets bound a cycle at three
// hosts × (3 s TCP + 5 s HTTP) = 24 s, and it leaves a stopped cycle without a final verdict — so the
// round needs a ceiling of its own, or one unlucky press leaves the button saying "Проверка…" for the
// rest of the session. One full budget plus room for the queued call to reach the worker.
constexpr int kPingTestWatchdogMs = 26000;

// The footer's standing line when nothing is being announced. The mock's "Updated every 5s".
QString pingCadenceText(int seconds)
{
    return QStringLiteral("Updated every %1s").arg(seconds);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_errorClock.start();
    m_settings = new Settings(this);
    // The motion policy is read before a single widget exists. A page that fades in on a machine that
    // was told not to move is the exact thing the settings box exists to prevent, and the only way to be
    // sure nothing moved first is to settle that before anything can.
    Motion::setEnabled(m_settings->animationsEnabled());
    m_manager = new ZapretManager(this);
    // Parentless by design: PingService lives on its own thread, and parenting it here would move it
    // back to the GUI thread. It is deleted explicitly in the destructor.
    m_ping = new PingService();
    // The bypass-check engine shares the manager — and through it the backend, and through that the
    // one-operation-at-a-time rule — with the power button. An ordinary child: the engine itself
    // lives on this thread, and only its probe worker does not.
    m_blockcheck = new BlockcheckEngine(this);
    m_blockcheck->setManager(m_manager);

    // The record of every check this window measures, kept in the per-user data directory next to — and
    // deliberately not inside — the payload tree a re-download replaces. load() is the one synchronous
    // read at startup: the file is bounded by the store's own cap, so the cost is a few tens of
    // milliseconds before the first paint, and every write after that is debounced and off-thread.
    //
    // A store that cannot read its file is not a window that cannot start: the checks still work, the
    // history simply begins again, and what was wrong goes to the journal once buildCentral() has made
    // one. Nothing is written over the unreadable file until a verdict actually arrives.
    m_history = new HistoryStore(ZapretPaths::historyFilePath(), this);
    QString historyError;
    const bool historyLoaded = m_history->load(&historyError);
    // A history that stopped saving is worth one journal line and no dialog: the user asked for a
    // check, and the check is what the sheet and the verdict answer.
    connect(m_history, &HistoryStore::saved, this, [this](bool ok, const QString &error) {
        if (!ok && m_log)
            m_log->appendLine(error, LogPanel::Warn);
    });

    // A fixed 900x700 canvas with no system frame at all. The dashboard is laid out around a 200 px ring
    // and a footer that must stay centred, so a maximised or resized window is a different design that
    // nobody drew — hence setFixedSize. And because the mock draws its own header, the platform title bar
    // is the one thing that cannot be there: FramelessWindowHint takes the whole frame away, which is why
    // the header carries the only two window controls (see buildHeader) and why a press on it drags the
    // window (see mousePressEvent). setWindowFlag alone would leave the WM's maximise control present but
    // useless; frameless removes every one of them at once.
    setWindowTitle(QStringLiteral("Muffin"));
    setWindowIcon(QIcon());
    setWindowFlag(Qt::FramelessWindowHint, true);
    setFixedSize(kWindowWidth, kWindowHeight);

    buildCentral();
    // Only now can the history's own trouble be said: m_log is built by buildCentral(), and a store that
    // could not read its file deserves a journal line rather than a silent restart of the record.
    if (!historyLoaded)
        m_log->appendLine(historyError, LogPanel::Warn);
    updateLanguageButton();
    const bool trayUp = ensureTray();
    if (!trayUp)
        startTrayRetry();
    buildShortcuts();
    wire();

    m_ping->setHosts(PingService::defaultHosts());
    m_ping->setIntervalSec(m_settings->pingIntervalSec());
    m_ping->start();

    // The watchdog runs from the moment the window exists, because the whole point is the hours nobody
    // is looking: a profile that dies at two in the morning should be known about before breakfast. The
    // tick is a no-op until something is actually running, so arming it before the first start costs
    // nothing, and a coarse timer is the honest type for a cadence measured in minutes.
    m_watchdogTimer = new QTimer(this);
    m_watchdogTimer->setTimerType(Qt::VeryCoarseTimer);
    connect(m_watchdogTimer, &QTimer::timeout, this, &MainWindow::onWatchdogTick);
    startWatchdog();

    // Seed the route from the persisted choice before the first scan, so the very first populateCards()
    // lights the row the last session left running rather than the first one alphabetically. If that id is
    // stale (the folder changed, the entry was deleted) populateCards falls it back to a real row.
    m_selectedId = m_settings->lastStrategyId();

    m_manager->setZapretDir(m_settings->zapretPath());
    updateStatusUi();

    // A short startup banner: it proves the manager -> log wiring end to end and leaves the panel
    // explaining itself instead of showing an empty box.
    m_log->appendLine(LanguageManager::tr(QStringLiteral("log.startup"))
                          .arg(QApplication::applicationVersion(), m_manager->backendName()),
                      LogPanel::Info);
    if (m_manager->strategyCount() > 0) {
        m_log->appendLine(LanguageManager::tr(QStringLiteral("log.found_in"))
                              .arg(m_manager->strategyCount())
                              .arg(QDir::toNativeSeparators(m_manager->resolvedRoot())),
                          LogPanel::Info);
    } else {
        m_log->appendLine(LanguageManager::tr(QStringLiteral("log.no_strategies")),
                          LogPanel::Warn);
    }

    // Closing the window can only mean "hide" if there is a tray to hide into. Say so up front instead
    // of letting the user discover it by pressing X with a strategy running.
    if (!trayUp && m_settings->minimizeToTray()) {
        m_trayWarned = true;
        m_log->appendLine(LanguageManager::tr(QStringLiteral("log.tray_no_env")), LogPanel::Warn);
        showTransientMessage(LanguageManager::tr(QStringLiteral("msg.tray_unavailable")), 8000);
    }

    if (m_manager->strategyCount() == 0) {
        m_needsSettingsPrompt = true;
    } else if (m_settings->zapretPath().isEmpty() && !m_manager->resolvedRoot().isEmpty()) {
        // Auto-location succeeded with no configured path: remember it so the next launch is instant.
        m_applyingSettings = true;
        m_settings->setZapretPath(m_manager->resolvedRoot());
        m_applyingSettings = false;
    }

    if (m_needsSettingsPrompt)
        QTimer::singleShot(0, this, &MainWindow::openSettings);

    if (m_settings->startLastOnLaunch()) {
        const QString last = m_settings->lastStrategyId();
        if (!last.isEmpty() && m_manager->findStrategy(last))
            QTimer::singleShot(500, this, [this, last] { startStrategy(last, true); });
    }
}

MainWindow::~MainWindow()
{
    // No tick, and no rotation waiting on a stop that will never be reported: both are statements about
    // a run this window is no longer here to follow.
    if (m_watchdogTimer)
        m_watchdogTimer->stop();
    m_watchdogRotationId.clear();
    if (m_blockcheck) {
        // Disconnect before cancelling: cancel() winds a mid-flight cycle down through failTest, and
        // its testFinished must not re-enter UI slots (a modal verdict sheet) from inside a teardown.
        m_blockcheck->disconnect(this);
        m_blockcheck->cancel();
    }
    if (m_history) {
        // The same rule as the engine's, and the reason flush() is called here rather than left to the
        // child's own destructor: a save that lands during teardown would emit saved() into a window
        // that is no longer listening, and the checks of the last seconds are the rows most worth
        // keeping. flush() writes what is pending on this thread and returns once the file matches.
        m_history->disconnect(this);
        m_history->flush();
    }
    if (m_ping) {
        m_ping->stop();
        delete m_ping;      // joins the worker thread (quit + wait)
        m_ping = nullptr;
    }
}

void MainWindow::buildCentral()
{
    // Rail + main area, edge to edge. The design has no window chrome of its own, so the two columns are
    // the whole window: any margin around them would show as a band of the wrong colour.
    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("rootCanvas"));

    auto *rootBox = new QHBoxLayout(root);
    rootBox->setContentsMargins(0, 0, 0, 0);
    rootBox->setSpacing(0);
    rootBox->addWidget(buildSidebar());
    rootBox->addWidget(buildMainArea(), 1);

    setCentralWidget(root);
}

// The rail: 70 px of glyphs. Brand at the head, the pages and the two commit controls pinned to the
// foot under the version. No labels anywhere — the whole argument of the mock is that icons need none,
// and the tooltip says the words when a pointer arrives.
QWidget *MainWindow::buildSidebar()
{
    auto *bar = new QFrame(this);
    m_sidebar = bar;
    bar->setObjectName(QStringLiteral("sidebar"));
    bar->setFixedWidth(kSidebarWidth);

    auto *box = new QVBoxLayout(bar);
    box->setContentsMargins(0, 18, 0, 12);
    box->setSpacing(10);

    auto *logo = new QLabel(bar);
    logo->setObjectName(QStringLiteral("brandLogo"));
    logo->setAlignment(Qt::AlignHCenter);
    logo->setPixmap(Theme::icon(QStringLiteral("app"), Theme::accent(), kBrandGlyphPx)
                        .pixmap(kBrandGlyphPx, kBrandGlyphPx));
    box->addWidget(logo);

    box->addStretch(1);

    // The bottom group, in the order the design asks for: the catalog, then the journal and the history
    // directly above settings, then settings, then the language. Every page glyph is checkable so a
    // second press puts the dashboard back — with one icon per page it has to work both ways.
    m_navStrategies = addRailButton(box, QStringLiteral("cards"), QStringLiteral("nav.profiles"));
    m_navStrategies->setCheckable(true);
    connect(m_navStrategies, &QPushButton::clicked, this, [this] {
        setCurrentPage(currentPage() == PageProfiles ? PageDashboard : PageProfiles);
    });

    m_navLogs = addRailButton(box, QStringLiteral("logs"), QStringLiteral("nav.logs"));
    m_navLogs->setCheckable(true);
    connect(m_navLogs, &QPushButton::clicked, this, [this] {
        setCurrentPage(currentPage() == PageLogs ? PageDashboard : PageLogs);
    });

    // The record beside the journal: one glyph for the lines as they arrived, one for what they add up
    // to. Checkable like its neighbours so a second press puts the dashboard back and the glyphs stay
    // mutually exclusive.
    m_navHistory = addRailButton(box, QStringLiteral("chart"), QStringLiteral("nav.history"));
    m_navHistory->setCheckable(true);
    i18nTip(m_navHistory, QStringLiteral("nav.history_tip"));
    connect(m_navHistory, &QPushButton::clicked, this, [this] {
        setCurrentPage(currentPage() == PageHistory ? PageDashboard : PageHistory);
    });

    // Settings is a page of this stack, not a modal sheet: a rail glyph that popped a dialog would be two
    // ways of navigating where the design wants one. Checkable like the other three, so a second press
    // puts the dashboard back and the four glyphs stay mutually exclusive.
    m_navSettings = addRailButton(box, QStringLiteral("settings"), QStringLiteral("nav.settings"));
    m_navSettings->setCheckable(true);
    i18nTip(m_navSettings, QStringLiteral("nav.settings_tip"));
    connect(m_navSettings, &QPushButton::clicked, this, [this] {
        setCurrentPage(currentPage() == PageSettings ? PageDashboard : PageSettings);
    });

    // The language picker: the current language's flag, opening a menu of the seven. It sits under
    // settings so the two "change how the app behaves" controls read as one group.
    m_langButton = new QToolButton(bar);
    m_langButton->setObjectName(QStringLiteral("langButton"));
    m_langButton->setCursor(Qt::PointingHandCursor);
    m_langButton->setAutoRaise(true);
    m_langButton->setFocusPolicy(Qt::TabFocus);
    m_langButton->setPopupMode(QToolButton::InstantPopup);
    m_langButton->setIconSize(QSize(kFlagGlyphPx, kFlagGlyphPx));
    m_langButton->setFixedSize(46, 30);
    m_langButton->setToolTip(LanguageManager::tr(QStringLiteral("nav.language")));
    m_langButton->setAccessibleName(LanguageManager::tr(QStringLiteral("nav.language")));
    m_langButton->setMenu(buildLanguageMenu());
    box->addWidget(m_langButton, 0, Qt::AlignHCenter);

    auto *version = new QLabel(QStringLiteral("v%1").arg(QApplication::applicationVersion()), bar);
    version->setObjectName(QStringLiteral("railVersion"));
    version->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    box->addWidget(version);

    return bar;
}

// One entry per language the catalog defines, each with its own flag, the native spelling of its name,
// and a tick on the one currently shown. Picking one writes it through Settings — so the next launch
// opens in it — and re-languages the running UI in place.
QMenu *MainWindow::buildLanguageMenu()
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("languageMenu"));

    for (const QString &code : LanguageManager::codes()) {
        QAction *act = menu->addAction(LanguageManager::flagIcon(code),
                                       LanguageManager::nativeName(code));
        act->setData(code);
        act->setCheckable(true);
        act->setChecked(code == LanguageManager::instance().currentCode());
        connect(act, &QAction::triggered, this, [this, code] {
            LanguageManager::instance().setCode(code);
            m_settings->setLanguage(code);
        });
    }
    return menu;
}

void MainWindow::updateLanguageButton()
{
    if (!m_langButton)
        return;
    const QString code = LanguageManager::instance().currentCode();
    m_langButton->setIcon(LanguageManager::flagIcon(code));
    const QString name = LanguageManager::nativeName(code);
    m_langButton->setToolTip(LanguageManager::tr(QStringLiteral("nav.language"))
                             + QStringLiteral(" — ") + name);
    m_langButton->setAccessibleName(name);
    // Re-tick the menu: the language can also change from QSettings, not only from this menu.
    if (QMenu *menu = m_langButton->menu()) {
        const auto actions = menu->actions();
        for (QAction *act : actions)
            act->setChecked(act->data().toString() == code);
    }
}

// The rail's icon-only button. `textKey` names a catalog entry rather than a sentence: the button shows
// a glyph and nothing else, so the key is what the accessible name, the tooltip and a later retranslate
// all read off the same widget.
QPushButton *MainWindow::addRailButton(QVBoxLayout *box, const QString &glyph, const QString &textKey)
{
    auto *b = new QPushButton(m_sidebar);
    b->setObjectName(QStringLiteral("navButton"));
    b->setProperty("glyph", glyph);
    b->setProperty("glyphPx", kRailGlyphPx);
    i18nAcc(b, textKey);
    i18nTip(b, textKey);
    b->setCursor(Qt::PointingHandCursor);
    b->setAutoDefault(false);
    b->setDefault(false);
    b->setFocusPolicy(Qt::TabFocus);
    b->setIconSize(QSize(kRailGlyphPx, kRailGlyphPx));
    b->setIcon(Theme::themedIcon(glyph, kRailGlyphPx));
    b->setFixedSize(46, 42);
    box->addWidget(b, 0, Qt::AlignHCenter);
    return b;
}

// A frameless-window control. It is a rail glyph turned sideways: same icon-only QToolButton, same
// "glyph"/"glyphPx" properties so applyTheme() re-bakes it with the rest of the window, but a square
// 30x30 plate that reads as chrome rather than navigation. `role` distinguishes the two for the sheet —
// "close" is tinted as the destructive one on hover, "min" is not — without giving them separate object
// names that would each need their own block in both themes.
QToolButton *MainWindow::addWindowButton(QHBoxLayout *row, const QString &glyph, const QString &textKey,
                                         const char *role)
{
    auto *b = new QToolButton(this);
    b->setObjectName(QStringLiteral("windowButton"));
    b->setProperty("glyph", glyph);
    b->setProperty("glyphPx", 16);
    b->setProperty("role", QString::fromLatin1(role));
    i18nTip(b, textKey);
    i18nAcc(b, textKey);
    b->setCursor(Qt::PointingHandCursor);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::TabFocus);
    b->setIconSize(QSize(16, 16));
    b->setIcon(Theme::themedIcon(glyph, 16));
    b->setFixedSize(30, 30);
    row->addWidget(b, 0, Qt::AlignVCenter);
    return b;
}

// The column beside the rail: header, page stack, footer. The rail's own 1 px right border is the only
// line between the two, which is how the mock separates them.
QWidget *MainWindow::buildMainArea()
{
    auto *area = new QFrame(this);
    m_mainArea = area;
    area->setObjectName(QStringLiteral("mainArea"));

    auto *box = new QVBoxLayout(area);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(0);
    box->addWidget(buildHeader());

    auto *rule = new QFrame(area);
    rule->setObjectName(QStringLiteral("headerRule"));
    rule->setFrameShape(QFrame::HLine);
    rule->setFixedHeight(1);
    box->addWidget(rule);

    m_pages = new QStackedWidget(area);
    m_pages->setObjectName(QStringLiteral("pageStack"));
    m_pages->addWidget(buildDashboardPage());
    m_pages->addWidget(buildProfilesPage());
    m_pages->addWidget(buildLogsPage());
    m_pages->addWidget(buildHistoryPage());
    m_pages->addWidget(buildSettingsPage());
    m_pages->setCurrentIndex(PageDashboard);
    box->addWidget(m_pages, 1);

    box->addWidget(buildFooter());
    return area;
}

QWidget *MainWindow::buildHeader()
{
    auto *header = new QWidget(m_mainArea);
    header->setObjectName(QStringLiteral("headerBar"));

    auto *row = new QHBoxLayout(header);
    row->setContentsMargins(28, 18, 28, 16);
    row->setSpacing(10);

    // The whole header is the drag handle for a frameless window (see mousePressEvent), so everything
    // that can eat a press — the buttons — is a child that stops the event, and the bare space between
    // and around them propagates down to MainWindow where the drag begins. The title says so to a screen
    // reader; the two controls on the right are the only ways off this window the frame no longer gives.
    auto *title = new QLabel(QStringLiteral("Muffin"), header);
    title->setObjectName(QStringLiteral("appTitle"));

    m_statusDot = new QWidget(header);
    m_statusDot->setObjectName(QStringLiteral("statusDot"));
    m_statusDot->setFixedSize(kStatusDotPx, kStatusDotPx);
    // A bare QWidget paints no stylesheet background of its own; this attribute is what lets #statusDot's
    // border-radius round anything at all.
    m_statusDot->setAttribute(Qt::WA_StyledBackground, true);
    i18nTip(m_statusDot, QStringLiteral("header.status_tip"));

    m_statusLabel = new QLabel(header);
    m_statusLabel->setObjectName(QStringLiteral("statusText"));
    m_statusLabel->setProperty("statusKind", statusKind(IZapretBackend::State::Stopped));

    row->addWidget(title);
    row->addStretch(1);
    row->addWidget(m_statusDot, 0, Qt::AlignVCenter);
    row->addWidget(m_statusLabel, 0, Qt::AlignVCenter);

    // The two controls a framed window would have given us for free. Minimise is showMinimized(); close
    // goes through close() and therefore closeEvent(), so the tray policy — hide when there is a tray to
    // hide into, quit when there is not — is the same one the X on a normal frame and Ctrl+Q follow. A
    // separate "hard quit" stays on Ctrl+Q / the tray menu; this button is the ordinary close.
    row->addSpacing(6);
    m_minButton = addWindowButton(row, QStringLiteral("window-minimize"),
                                  QStringLiteral("window.minimize"), "min");
    connect(m_minButton, &QToolButton::clicked, this, [this] { showMinimized(); });
    m_closeButton = addWindowButton(row, QStringLiteral("window-close"),
                                    QStringLiteral("window.close"), "close");
    connect(m_closeButton, &QToolButton::clicked, this, [this] { close(); });
    return header;
}

// The footer carries the ping cadence and, when something needs saying, a transient message. The zapret
// path used to sit to its left; the resolved root is a fact the settings dialog owns, and showing it
// here as well only made the one centred line sit off-centre.
QWidget *MainWindow::buildFooter()
{
    auto *foot = new QFrame(m_mainArea);
    foot->setObjectName(QStringLiteral("footerBar"));

    auto *grid = new QGridLayout(foot);
    grid->setContentsMargins(28, 8, 28, 8);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(0);

    m_footerLabel = new QLabel(foot);
    m_footerLabel->setObjectName(QStringLiteral("footerHint"));
    m_footerLabel->setAlignment(Qt::AlignCenter);

    // Two equal spacers either side of one stretch-free cell: whatever the message is, the line stays
    // on the window's axis.
    grid->addWidget(m_footerLabel, 0, 1, Qt::AlignCenter);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 0);
    grid->setColumnStretch(2, 1);

    m_footerTimer = new QTimer(this);
    m_footerTimer->setSingleShot(true);
    connect(m_footerTimer, &QTimer::timeout, this, [this] {
        if (m_footerLabel)
            m_footerLabel->setText(m_footerIdleText);
    });
    m_footerIdleText = pingCadenceText(m_settings->pingIntervalSec());
    m_footerLabel->setText(m_footerIdleText);
    return foot;
}

// The dashboard: one enormous button in the middle and the reachability badges under it. Nothing else —
// the point of the design is that the common case (run the thing you ran last time) never leaves this
// screen. The route pill that used to sit in the top-left named the strategy and opened the catalog;
// both jobs now belong to the rail's cards glyph and to the power button itself.
QWidget *MainWindow::buildDashboardPage()
{
    auto *page = new QWidget(m_pages);
    m_dashboardPage = page;
    page->setObjectName(QStringLiteral("dashboardPage"));

    auto *box = new QVBoxLayout(page);
    box->setContentsMargins(28, 22, 28, 22);
    box->setSpacing(0);

    // Unequal stretches either side of the button: with the badges and the footer in the same column,
    // equal stretches would push the circle visibly above the middle of the canvas.
    box->addStretch(2);

    m_power = new PowerButton(page);
    m_power->setDiameter(kPowerDiameterPx);
    connect(m_power, &QAbstractButton::clicked, this, &MainWindow::onPowerClicked);
    box->addWidget(m_power, 0, Qt::AlignHCenter);

    box->addSpacing(kPowerToPingsGap);

    // One badge per host the ping service actually checks, rather than a hardcoded pair: a badge that
    // showed a service nobody probes would be the only dishonest number on the screen.
    auto *pings = new QHBoxLayout;
    pings->setContentsMargins(0, 0, 0, 0);
    pings->setSpacing(kPingSpacing);
    pings->addStretch(1);
    const QStringList hosts = PingService::defaultHosts();
    for (const QString &host : hosts) {
        auto *chip = new PingIndicator(friendlyHostName(host), page);
        chip->setToolTip(host);
        m_indicators.insert(host, chip);
        pings->addWidget(chip, 0, Qt::AlignVCenter);
    }
    pings->addStretch(1);
    box->addLayout(pings);

    box->addStretch(3);
    return page;
}

// The profiles page — one horizontal row per strategy, each carrying the services it exists to unblock
// and a menu for what can be done to the entry itself. Reached from the dashboard's route pill, with its
// own way back, and holding the two actions this page is for: rescan the folder, and measure the
// services now. Fetching zapret is not one of them — a page that lists profiles has nothing to say about
// where the folder came from, and the settings page and the empty-list placeholder both already offer it.
QWidget *MainWindow::buildProfilesPage()
{
    auto *page = new QWidget(m_pages);
    m_profilesPage = page;
    page->setObjectName(QStringLiteral("profilesPage"));

    auto *box = new QVBoxLayout(page);
    box->setContentsMargins(28, 22, 28, 22);
    box->setSpacing(12);

    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(10);

    auto *back = new QPushButton(page);
    back->setObjectName(QStringLiteral("ghostButton"));
    back->setCursor(Qt::PointingHandCursor);
    back->setAutoDefault(false);
    i18nText(back, QStringLiteral("page.back"));
    i18nTip(back, QStringLiteral("page.back_tip"));
    connect(back, &QPushButton::clicked, this, &MainWindow::showDashboard);

    auto *title = new QLabel(page);
    title->setObjectName(QStringLiteral("pageTitle"));
    i18nText(title, QStringLiteral("nav.profiles"));

    // The manual measurement, beside the name of the page it acts on. The rows below it carry numbers
    // that only move while their own profile is running, so the page has to offer the one press that
    // asks for all of them at once — and say, in its own label, that a round is already in flight.
    m_pingTestWatchdog = new QTimer(this);
    m_pingTestWatchdog->setSingleShot(true);
    connect(m_pingTestWatchdog, &QTimer::timeout, this, &MainWindow::endPingTest);

    m_pingTestButton = new QPushButton(QString(), page);
    m_pingTestButton->setObjectName(QStringLiteral("ghostButton"));
    m_pingTestButton->setProperty("glyph", QStringLiteral("ping-test"));
    m_pingTestButton->setProperty("glyphPx", 16);
    m_pingTestButton->setIcon(Theme::themedIcon(QStringLiteral("ping-test"), 16));
    i18nText(m_pingTestButton, QStringLiteral("action.ping_test"));
    i18nTip(m_pingTestButton, QStringLiteral("action.ping_test_tip"));
    m_pingTestButton->setCursor(Qt::PointingHandCursor);
    m_pingTestButton->setAutoDefault(false);
    connect(m_pingTestButton, &QPushButton::clicked, this, &MainWindow::onPingTestRequested);

    // The page's second measurement, and the harder question: not "do the services answer" but
    // "does the path the SELECTED profile opens actually work" — connect + TLS handshake + the
    // certificate verdict, three tries per host. The engine brings the preset up itself and takes
    // it back down, so the verdict this button reports is about the profile, not about whatever
    // happens to be running right now.
    m_blockcheckButton = new QPushButton(QString(), page);
    m_blockcheckButton->setObjectName(QStringLiteral("ghostButton"));
    m_blockcheckButton->setProperty("glyph", QStringLiteral("shield-check"));
    m_blockcheckButton->setProperty("glyphPx", 16);
    m_blockcheckButton->setIcon(Theme::themedIcon(QStringLiteral("shield-check"), 16));
    i18nText(m_blockcheckButton, QStringLiteral("action.blockcheck"));
    i18nTip(m_blockcheckButton, QStringLiteral("action.blockcheck_tip"));
    m_blockcheckButton->setCursor(Qt::PointingHandCursor);
    m_blockcheckButton->setAutoDefault(false);
    connect(m_blockcheckButton, &QPushButton::clicked, this, &MainWindow::onBlockcheckRequested);

    auto *refresh = new QPushButton(QString(), page);
    refresh->setObjectName(QStringLiteral("ghostButton"));
    refresh->setProperty("glyph", QStringLiteral("refresh"));
    refresh->setProperty("glyphPx", 16);
    refresh->setIcon(Theme::themedIcon(QStringLiteral("refresh"), 16));
    i18nText(refresh, QStringLiteral("action.refresh"));
    i18nTip(refresh, QStringLiteral("action.refresh_tip"));
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshStrategies);

    head->addWidget(back);
    head->addSpacing(6);
    head->addWidget(title);
    head->addSpacing(12);
    head->addWidget(m_pingTestButton);
    head->addWidget(m_blockcheckButton);
    // The corner is the list's own utility: read the folder again. Everything else this page can do has
    // a place to say it in — the measurement in its label, the download on the settings page.
    head->addStretch(1);
    head->addWidget(refresh);
    box->addLayout(head);

    // The slim meter that exists only while a bypass-check cycle is in flight: busy (indeterminate)
    // while the engine starts, settles and stops the preset, determinate while the hosts are being
    // probed, hidden the rest of the time. A bar of its own rather than the footer's text because
    // "how far along" is the one thing a label cannot say.
    m_blockcheckProgress = new QProgressBar(page);
    m_blockcheckProgress->setObjectName(QStringLiteral("blockcheckProgress"));
    m_blockcheckProgress->setRange(0, 0);
    m_blockcheckProgress->setTextVisible(false);
    m_blockcheckProgress->setFixedHeight(6);
    m_blockcheckProgress->hide();
    box->addWidget(m_blockcheckProgress);

    // m_stack is still what it always was — the profile rows or the "nothing found" page.
    m_stack = new QStackedWidget(page);

    m_scroll = new QScrollArea(m_stack);
    m_scroll->setObjectName(QStringLiteral("cardScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_cardHost = new QWidget(m_scroll);
    m_cardLayout = new QVBoxLayout(m_cardHost);
    m_cardLayout->setContentsMargins(12, 12, 12, 12);
    // Rows are one fixed height each, so the list is read as a column: a small gap keeps them apart
    // without the airier spacing the old variable-height card grid needed between notes of different
    // lengths.
    m_cardLayout->setSpacing(8);
    m_cardLayout->addStretch(1);
    m_scroll->setWidget(m_cardHost);

    m_placeholder = new QWidget(m_stack);
    m_placeholder->setObjectName(QStringLiteral("placeholderPage"));

    // The placeholder used to be one label ending in "укажите путь в настройках" — instructions for
    // someone who has no zapret and no way to get it from here. Both ways out are now buttons.
    auto *placeholderText = new QLabel(m_placeholder);
    placeholderText->setObjectName(QStringLiteral("placeholder"));
    placeholderText->setAlignment(Qt::AlignCenter);
    placeholderText->setWordWrap(true);
    i18nText(placeholderText, QStringLiteral("placeholder.no_strategies"));

    auto *placeholderDownload = new QPushButton(m_placeholder);
    placeholderDownload->setObjectName(QStringLiteral("primaryButton"));
    i18nText(placeholderDownload, QStringLiteral("action.download_zapret"));
    auto *placeholderSettings = new QPushButton(m_placeholder);
    i18nText(placeholderSettings, QStringLiteral("nav.settings_tip"));

    auto *placeholderBox = new QVBoxLayout(m_placeholder);
    placeholderBox->setContentsMargins(24, 24, 24, 24);
    placeholderBox->setSpacing(14);
    placeholderBox->addStretch(1);
    placeholderBox->addWidget(placeholderText);
    placeholderBox->addWidget(placeholderDownload, 0, Qt::AlignHCenter);
    placeholderBox->addWidget(placeholderSettings, 0, Qt::AlignHCenter);
    placeholderBox->addStretch(2);

    connect(placeholderDownload, &QPushButton::clicked, this, &MainWindow::downloadZapret);
    connect(placeholderSettings, &QPushButton::clicked, this, &MainWindow::openSettings);

    m_stack->addWidget(m_scroll);
    m_stack->addWidget(m_placeholder);
    m_stack->setCurrentWidget(m_scroll);
    box->addWidget(m_stack, 1);
    return page;
}

// The settings page. Unlike the other three this owns its whole form: it reads the stored values on
// construction and writes them back through Settings on Save, which fires changed() and re-points the
// manager and the theme from onSettingsChanged(). The page only has to exist in the stack, be re-tinted
// with everything else, and offer the way back — what buildSettingsPage adds around the widget is the
// header row the other full-screen pages carry and nothing more.
QWidget *MainWindow::buildSettingsPage()
{
    auto *page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("settingsPageHost"));

    auto *box = new QVBoxLayout(page);
    box->setContentsMargins(0, 14, 0, 0);
    box->setSpacing(10);

    // The way back lives here rather than inside SettingsPage so the page widget stays purely the form and
    // a test can build it without the navigation chrome. Esc and the rail's settings glyph dismiss it too;
    // this is just the visible one, matching the journal and the profiles list.
    auto *head = new QHBoxLayout;
    head->setContentsMargins(28, 0, 28, 0);
    head->setSpacing(10);

    auto *back = new QPushButton(page);
    back->setObjectName(QStringLiteral("ghostButton"));
    back->setCursor(Qt::PointingHandCursor);
    back->setAutoDefault(false);
    i18nText(back, QStringLiteral("page.back"));
    i18nTip(back, QStringLiteral("page.back_tip"));
    connect(back, &QPushButton::clicked, this, &MainWindow::showDashboard);

    auto *title = new QLabel(page);
    title->setObjectName(QStringLiteral("pageTitle"));
    i18nText(title, QStringLiteral("nav.settings"));
    head->addWidget(back);
    head->addSpacing(6);
    head->addWidget(title);
    head->addStretch(1);
    box->addLayout(head);

    m_settingsPage = new SettingsPage(*m_settings, page);
    box->addWidget(m_settingsPage, 1);
    return page;
}

// The journal gets the whole canvas, which is what a 5000-line ring buffer of aligned timestamps
// actually wants. Filtering, copying and saving are LogPanel's own; the page adds nothing above them but
// the way back, because a header naming a box of logs says what the box already shows.
QWidget *MainWindow::buildLogsPage()
{
    auto *page = new QWidget(m_pages);
    m_logsPage = page;
    page->setObjectName(QStringLiteral("logsPage"));

    auto *box = new QVBoxLayout(page);
    box->setContentsMargins(28, 14, 28, 22);
    box->setSpacing(10);

    auto *back = new QPushButton(page);
    back->setObjectName(QStringLiteral("ghostButton"));
    back->setCursor(Qt::PointingHandCursor);
    back->setAutoDefault(false);
    i18nText(back, QStringLiteral("page.back"));
    i18nTip(back, QStringLiteral("page.back_tip"));
    connect(back, &QPushButton::clicked, this, &MainWindow::showDashboard);
    box->addWidget(back, 0, Qt::AlignLeft);

    m_log = new LogPanel(page);
    box->addWidget(m_log, 1);
    return page;
}

// The record of every check, with the arithmetic above it. The window contributes the way back and the
// page's place in the stack; everything else — the summary, the rows, the placeholder, the destructive
// button and the confirmation in front of it — belongs to HistoryPage, which is why the page can be
// built and filled by a test without a window at all.
//
// The store is handed over rather than copied: it is the one owner of the rows, it says when they move,
// and a page holding a second copy of a bounded log would be a page that can be wrong.
QWidget *MainWindow::buildHistoryPage()
{
    auto *page = new QWidget(m_pages);
    m_historyPage = new HistoryPage(m_history, page);

    auto *box = new QVBoxLayout(page);
    box->setContentsMargins(28, 14, 28, 22);
    box->setSpacing(10);

    auto *back = new QPushButton(page);
    back->setObjectName(QStringLiteral("ghostButton"));
    back->setCursor(Qt::PointingHandCursor);
    back->setAutoDefault(false);
    i18nText(back, QStringLiteral("page.back"));
    i18nTip(back, QStringLiteral("page.back_tip"));
    connect(back, &QPushButton::clicked, this, &MainWindow::showDashboard);
    box->addWidget(back, 0, Qt::AlignLeft);

    box->addWidget(m_historyPage, 1);
    return page;
}

int MainWindow::currentPage() const
{
    return m_pages ? m_pages->currentIndex() : PageDashboard;
}

QWidget *MainWindow::currentPageWidget() const
{
    return m_pages ? m_pages->currentWidget() : nullptr;
}

void MainWindow::setCurrentPage(int page)
{
    if (!m_pages)
        return;
    const int target = (page == PageLogs)     ? PageLogs
                       : (page == PageHistory) ? PageHistory
                       : (page == PageProfiles) ? PageProfiles
                       : (page == PageSettings) ? PageSettings
                                                : PageDashboard;
    const int previous = m_pages->currentIndex();
    m_pages->setCurrentIndex(target);

    // The new page appears rather than arriving. One hook covers every way a page is chosen — the four
    // rail glyphs, the way back, Ctrl+S, and the startup "no strategies yet" prompt — because they all
    // come through here, and navigation that animates from the rail but snaps from the keyboard is
    // exactly the inconsistency a second call site would create.
    //
    // Nothing is animated when the stack did not move: re-selecting the page that is already up is a
    // no-op for the user, and a fade would make it one.
    if (target != previous) {
        Motion::fadeIn(m_pages->currentWidget());
        if (target == PageProfiles)
            animateCardEntrance();
    }

    // The rail's glyphs are the only page indicator left, so they have to follow the stack even when the
    // page was chosen from code — a lit journal icon over the dashboard is worse than no icon at all. The
    // four are mutually exclusive by construction: exactly one is checked for the four pages, none for
    // the dashboard, which is why the settings glyph can now stand in for what used to be a modal.
    if (m_navLogs)
        m_navLogs->setChecked(target == PageLogs);
    if (m_navStrategies)
        m_navStrategies->setChecked(target == PageProfiles);
    if (m_navHistory)
        m_navHistory->setChecked(target == PageHistory);
    if (m_navSettings)
        m_navSettings->setChecked(target == PageSettings);
    // Opening the journal is what acknowledges whatever made it light up.
    if (target == PageLogs)
        setLogAlert(false);
    // Coming back to the dashboard is also how you dismiss a route you did not mean to change.
    if (target == PageDashboard)
        updateStatusUi();
}

void MainWindow::showDashboard()
{
    setCurrentPage(PageDashboard);
}

bool MainWindow::logAlertActive() const
{
    return m_navLogs && m_navLogs->property("alert").toBool();
}

void MainWindow::setLogAlert(bool on)
{
    if (!m_navLogs || m_navLogs->property("alert").toBool() == on)
        return;
    m_navLogs->setProperty("alert", on);
    m_navLogs->style()->unpolish(m_navLogs);
    m_navLogs->style()->polish(m_navLogs);
    m_navLogs->update();
}

// The header's chip: one glyph-sized dot and one word, both driven off the same statusKind property the
// old pill used. Split out because updateStatusUi() has to write the text and the property together, and
// a chip whose dot and word disagree is worse than a chip with neither.
void MainWindow::setConnectionChip(const QString &text, const QString &kind)
{
    if (m_statusLabel) {
        m_statusLabel->setText(text);
        m_statusLabel->setProperty("statusKind", kind);
        m_statusLabel->style()->unpolish(m_statusLabel);
        m_statusLabel->style()->polish(m_statusLabel);
        m_statusLabel->update();
    }
    if (m_statusDot) {
        m_statusDot->setProperty("statusKind", kind);
        m_statusDot->style()->unpolish(m_statusDot);
        m_statusDot->style()->polish(m_statusDot);
        m_statusDot->update();
    }
}

void MainWindow::showTransientMessage(const QString &text, int timeoutMs)
{
    if (!m_footerLabel)
        return;
    m_footerLabel->setText(text);
    m_footerTimer->start(timeoutMs > 0 ? timeoutMs : 5000);
}

// The one press this window is built around.
void MainWindow::onPowerClicked()
{
    const IZapretBackend::State state = m_manager->state();
    // A run is up, or coming up: the button's other meaning is the one that is wanted, and canStopFrom()
    // is the same gate the toolbar action and Ctrl+Shift+X used.
    if (canStopFrom(state)) {
        onStopRequested();
        return;
    }
    if (!canStartFrom(state))
        return;   // a teardown is in flight; blockedReason() already says so in the tooltip

    // The profile the power button runs is the one selected in the list, not a separately-stored "last":
    // selecting is choosing, and choosing is the whole job of the profiles page. m_selectedId is seeded
    // from the persisted lastStrategyId at startup, and every start writes it back there, so the two agree
    // for a restart while one live value — the selection — drives the button.
    if (m_selectedId.isEmpty() || !m_manager->findStrategy(m_selectedId)) {
        // Nothing to run yet. Saying "pick a profile" where the user can see the list they have not
        // opened is more useful than a sentence in a footer nobody reads.
        setCurrentPage(PageProfiles);
        showTransientMessage(LanguageManager::tr(QStringLiteral("msg.pick_profile")), 5000);
        return;
    }
    startStrategy(m_selectedId, true);
}


void MainWindow::bringToFront()
{
    // showNormal (not show): the window can be hidden — closed to the tray, or launched minimized —
    // and it can be a normal visible window that happens to sit behind another application. show()
    // would only be correct for the hidden case, and doing the two separately is a race against
    // whoever minimized the window between the visibility check and the call. showNormal un-hides
    // and un-minimizes in one step, and raises.
    showNormal();
    raise();
    // activateWindow is the one that actually pulls keyboard focus back from whatever the user was
    // typing into before they clicked the tray or launched a second instance. raise alone reorders
    // the window stack without stealing focus, which reads as the app being unresponsive to the
    // click — the exact symptom that keeps this trio together.
    activateWindow();
}

bool MainWindow::traySupported()
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

bool MainWindow::hideOnClose(bool minimizeToTraySetting, bool trayAvailable)
{
    // The setting only means "hide" when there is somewhere to hide: without a tray host the user would
    // lose the window with no way back, so the same close has to quit instead.
    return minimizeToTraySetting && trayAvailable;
}

QString MainWindow::errorTargetId(const QString &pendingStartId, const QString &activeId,
                                  const QString &lastRunId)
{
    if (!pendingStartId.isEmpty())
        return pendingStartId;
    if (!activeId.isEmpty())
        return activeId;
    return lastRunId;
}

// Idempotent: the first call can land before the DE publishes a tray host, in which case the retry
// timer calls this again. Only the first successful call builds the menu.
bool MainWindow::ensureTray()
{
    if (m_tray)
        return true;
    if (!traySupported())
        return false;

    m_tray = new QSystemTrayIcon(this);

    auto *menu = new QMenu(this);
    QAction *show = i18nText(menu->addAction(QString()), QStringLiteral("tray.show"));
    connect(show, &QAction::triggered, this, [this] { bringToFront(); });
    QAction *startLast = i18nText(menu->addAction(QString()), QStringLiteral("tray.start_last"));
    connect(startLast, &QAction::triggered, this, [this] {
        const QString last = m_settings->lastStrategyId();
        if (!last.isEmpty())
            startStrategy(last, true);
    });
    QAction *stop = i18nText(menu->addAction(QString()), QStringLiteral("tray.stop"));
    connect(stop, &QAction::triggered, this, &MainWindow::onStopRequested);
    menu->addSeparator();
    QAction *quit = i18nText(menu->addAction(QString()), QStringLiteral("tray.quit"));
    connect(quit, &QAction::triggered, this, &MainWindow::requestQuit);

    m_tray->setContextMenu(menu);
    // Single click and double click both mean "toggle": the icon is the only handle when the window
    // is hidden, so a click has to bring it back; when it is already the front window the same click
    // hides it again, which is what an ordinary dock-style icon does. Right-click opens the menu,
    // and Context (not Trigger) is the reason for it on the platforms that distinguish them, so we
    // do not have to handle the menu ourselves here.
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                    if (isVisible() && !isMinimized()) {
                        hide();
                    } else {
                        bringToFront();
                    }
                }
            });

    updateTrayIcon();
    m_tray->show();
    return true;
}

void MainWindow::startTrayRetry()
{
    if (m_trayRetry)
        return;
    m_trayRetry = new QTimer(this);
    m_trayRetry->setInterval(kTrayRetryIntervalMs);
    connect(m_trayRetry, &QTimer::timeout, this, [this] {
        if (ensureTray()) {
            m_trayRetry->stop();
            if (m_trayWarned) {
                m_trayWarned = false;
                m_log->appendLine(LanguageManager::tr(QStringLiteral("log.tray_available")),
                                  LogPanel::Info);
            }
            return;
        }
        if (++m_trayAttempts >= kTrayRetryMaxAttempts)
            m_trayRetry->stop();   // give up; closeEvent already falls back to quitting
    });
    m_trayRetry->start();
}

void MainWindow::buildShortcuts()
{
    auto *refresh = new QShortcut(QKeySequence(QStringLiteral("Ctrl+R")), this);
    connect(refresh, &QShortcut::activated, this, &MainWindow::refreshStrategies);

    auto *settings = new QShortcut(QKeySequence(QStringLiteral("Ctrl+S")), this);
    connect(settings, &QShortcut::activated, this, &MainWindow::openSettings);

    auto *startLast = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")), this);
    connect(startLast, &QShortcut::activated, this, [this] {
        const QString last = m_settings->lastStrategyId();
        if (!last.isEmpty())
            startStrategy(last, true);
        else
            showTransientMessage(LanguageManager::tr(QStringLiteral("msg.no_saved_strategy")), 3000);
    });

    auto *stop = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+X")), this);
    connect(stop, &QShortcut::activated, this, &MainWindow::onStopRequested);

    // The rail lost its "Свернуть/Выйти" glyph to the mock's three icons, so quitting needs a key: the
    // tray menu still has Выход, but a desktop with no tray host had the button as its only explicit way
    // out and now has this instead.
    auto *quit = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Q")), this);
    connect(quit, &QShortcut::activated, this, &MainWindow::requestQuit);

    // Esc backs out of whichever full-screen view is up, because the way in was a click and the way out
    // should not be a hunt.
    auto *back = new QShortcut(QKeySequence(QStringLiteral("Escape")), this);
    connect(back, &QShortcut::activated, this, [this] {
        if (currentPage() != PageDashboard)
            showDashboard();
    });
}

void MainWindow::wire()
{
    connect(m_manager, &ZapretManager::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_manager, &ZapretManager::strategiesLoaded, this, &MainWindow::onStrategiesLoaded);
    connect(m_manager, &ZapretManager::strategyStarted, this, &MainWindow::onStrategyStarted);
    connect(m_manager, &ZapretManager::strategyStopped, this, &MainWindow::onStrategyStopped);
    connect(m_manager, &ZapretManager::logMessage, this, &MainWindow::onLogMessage);
    connect(m_manager, &ZapretManager::errorOccurred, this, &MainWindow::onErrorOccurred);

    connect(m_settings, &Settings::changed, this, &MainWindow::onSettingsChanged);
    connect(m_ping, &PingService::pingUpdated, this, &MainWindow::onPingUpdated);

    // The history page owns its own store connection (it re-reads on changed() by itself), so the window
    // listens only for the one thing it has to hear: that the record was thrown away. That is a fact
    // about the whole tool, and it belongs in the journal that outlives the page.
    if (m_historyPage)
        connect(m_historyPage, &HistoryPage::cleared, this, [this](int removedRows) {
            m_log->appendLine(LanguageManager::tr(QStringLiteral("history.cleared"))
                                  .arg(removedRows),
                              LogPanel::Info);
        });

    // The bypass-check cycle reports through five channels, and each has exactly one job here: the
    // log lines into the journal, the stage into the meter's mode, progress into its value, per-host
    // verdicts into a compact log line, and the final map into the verdict sheet. errorOccurred
    // deliberately gets no modal of its own — a backend-side failure already dialogs through the
    // manager's own channel, and every failed run still ends in testFinished, so the sheet below is
    // the one place a press is answered, once.
    //
    // Four of the five then hold their tongue while m_watchdogRun is set. A quiet cycle is the same
    // engine answering a question nobody asked, and the window is most likely in the tray: journal
    // lines, a moving meter and a footer message from a background check would be exactly the noise the
    // "quiet" in the name rules out. testFinished is not muted — it is the verdict, and it routes to
    // onWatchdogResult() instead of to the sheet.
    connect(m_blockcheck, &BlockcheckEngine::logMessage, this,
            [this](const QString &line, int level) {
        if (m_watchdogRun)
            return;
        onLogMessage(line, level);
    });
    connect(m_blockcheck, &BlockcheckEngine::stageChanged, this, [this](const QString &stage) {
        if (m_watchdogRun)
            return;
        if (!m_blockcheckProgress || stage == QLatin1String("probing"))
            return;   // the first progress() moves the meter to determinate on its own
        m_blockcheckProgress->setRange(0, 0);   // busy while starting / settling / stopping
    });
    connect(m_blockcheck, &BlockcheckEngine::progress, this,
            [this](int done, int total, const QString &) {
        if (m_watchdogRun)
            return;
        if (!m_blockcheckProgress)
            return;
        if (m_blockcheckProgress->maximum() != qMax(1, total))
            m_blockcheckProgress->setRange(0, qMax(1, total));
        m_blockcheckProgress->setValue(done);
    });
    connect(m_blockcheck, &BlockcheckEngine::hostChecked, this,
            [this](const QString &host, const HostCheckResult &r) {
        // Recorded before the watchdog's silence, because history is data rather than noise: a quiet
        // cycle is the one that answers "did it still work at 3 a.m.", and dropping it would leave the
        // record full of the checks the user happened to ask for. What the mute covers is the journal,
        // the meter and the toast — not the file.
        //
        // The certificate verdict travels with the row, because it is the half of the answer that a
        // TCP-level pass hides: a substituted cert is the interception the user is trying to detect, and
        // a history that recorded it as an OK would read as "it worked yesterday" precisely when it
        // did not.
        if (m_history)
            m_history->appendResult(m_blockcheckPreset, host, r.success, r.avgLatency,
                                    r.isCertificateValid);

        if (m_watchdogRun)
            return;
        // A journal line per host, in the engine's own vocabulary rather than a translated sentence:
        // the sheet at the end is the user-facing wording, and the journal is where the numbers live.
        QString verdict;
        if (r.success) {
            verdict = QStringLiteral("OK %1 ms (%2/%3 tries)").arg(r.avgLatency)
                          .arg(r.attemptsOk).arg(r.attempts);
            if (r.ttfbMs >= 0)
                verdict += QStringLiteral(", ttfb %1 ms").arg(r.ttfbMs);
            if (!r.isCertificateValid)
                verdict += QStringLiteral(", certificate REJECTED");
        } else {
            verdict = QStringLiteral("FAIL (%1)").arg(r.lastError);
        }
        m_log->appendLine(QStringLiteral("blockcheck: %1 — %2").arg(host, verdict),
                          r.success ? LogPanel::Info : LogPanel::Warn);
    });
    connect(m_blockcheck, &BlockcheckEngine::errorOccurred, this,
            [this](const QString &message) {
        if (m_watchdogRun)
            return;
        showTransientMessage(message, 6000);
    });
    connect(m_blockcheck, &BlockcheckEngine::testFinished, this,
            &MainWindow::onBlockcheckFinished);

    // The language can move while this window is open (the rail's flag button), and nothing else tells
    // the widgets: one signal, one walk over the tree. updateStatusUi() is part of retranslateUi(), so
    // the state-derived strings move with it.
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged, this,
            [this](const QString &) { retranslateUi(); });

    if (QStyleHints *hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this] {
            if (Theme::currentMode() == Theme::Mode::System)
                applyTheme();
        });
    }
}

void MainWindow::populateCards()
{
    while (QLayoutItem *item = m_cardLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    m_cards.clear();
    m_errorCardId.clear();   // the new set of rows has not failed at anything yet

    const QStringList hosts = profilePingHosts();
    const QList<Strategy> list = m_manager->strategies();
    for (const Strategy &s : list) {
        auto *card = new ProfileCard(s, m_cardHost);
        const QString id = s.id.isEmpty() ? s.filePath : s.id;
        // The chips are the services this row exists to unblock — the same hosts the dashboard's rings
        // name, minus the baseline it probes and a profile has nothing to do with. Seeded from the last
        // verdict we hold, so a row built mid-cycle does not start every chip on grey and wait for the
        // next tick to be useful.
        card->setHosts(hosts);
        for (const QString &host : hosts) {
            auto it = m_lastPing.constFind(host);
            card->setPing(host, it == m_lastPing.constEnd() ? PingIndicator::PingStatus::Unknown : it->status,
                          it == m_lastPing.constEnd() ? -1 : it->latencyMs);
        }
        card->setSelected(id == m_selectedId);
        connect(card, &ProfileCard::clicked, this, &MainWindow::onProfileSelected);
        connect(card, &ProfileCard::copyRequested, this, &MainWindow::onProfileCopied);
        connect(card, &ProfileCard::deleteRequested, this, &MainWindow::onProfileDeleteRequested);
        m_cardLayout->addWidget(card);
        m_cards.insert(id, card);
    }
    m_cardLayout->addStretch(1);

    m_stack->setCurrentWidget(list.isEmpty() ? m_placeholder : m_scroll);
    // A reload can drop the profile that was selected — the folder changed, or the last run's entry is
    // gone. Fall back to the persisted route (still valid if it survived the scan), then to the first
    // row, so the power button always names something real and the neon edge is never left floating on a
    // row that no longer exists.
    if (!m_selectedId.isEmpty() && !m_cards.contains(m_selectedId))
        m_selectedId.clear();
    if (m_selectedId.isEmpty()) {
        const QString persisted = m_settings->lastStrategyId();
        if (!persisted.isEmpty() && m_cards.contains(persisted))
            m_selectedId = persisted;
        else if (!list.isEmpty())
            m_selectedId = list.first().id.isEmpty() ? list.first().filePath : list.first().id;
    }
    updateStatusUi();

    // The other moment a row appears in front of somebody: the list was rebuilt while its page was up —
    // a rescan, a delete, a folder that moved. Rows the user is not looking at are not worth animating,
    // which is what the page check is for; opening the page is the other half, in setCurrentPage().
    if (currentPage() == PageProfiles)
        animateCardEntrance();
}

QList<ProfileCard *> MainWindow::cardRows() const
{
    QList<ProfileCard *> rows;
    if (!m_cardLayout)
        return rows;
    // The layout, not the hash: m_cards is keyed by strategy id and iterates in whatever order its
    // buckets give, and a cascade that rose in hash order would arrive in a different order every run.
    for (int i = 0; i < m_cardLayout->count(); ++i) {
        if (ProfileCard *card = qobject_cast<ProfileCard *>(m_cardLayout->itemAt(i)->widget()))
            rows.append(card);
    }
    return rows;
}

void MainWindow::animateCardEntrance()
{
    if (!Motion::enabled() || !m_cardLayout)
        return;
    // Nothing to cascade while the list itself is not what the page is showing: a folder that yielded no
    // presets draws the placeholder, and animating the rows behind it would be motion on a widget the
    // user has already been told is not there.
    if (!m_stack || m_stack->currentWidget() != m_scroll)
        return;

    // The layout has to have said where each row goes before any of them is moved off that place: the
    // entrance both starts and ends on the layout's own answer, and read too early it would be (0, 0) —
    // every row rising from and back to the top-left corner of the host.
    m_cardLayout->activate();

    const QList<ProfileCard *> rows = cardRows();
    const int limit = qMin(rows.size(), Motion::kCardRiseRows);
    for (int i = 0; i < limit; ++i)
        Motion::riseIn(rows.at(i), i * Motion::kCardStepMs);
}

QString MainWindow::statusText(IZapretBackend::State state, const QString &strategyName)
{
    switch (state) {
    case IZapretBackend::State::Starting:
        return LanguageManager::tr(QStringLiteral("status.starting"))
            .arg(strategyName.isEmpty() ? QStringLiteral("…") : strategyName);
    case IZapretBackend::State::Running:
        return LanguageManager::tr(QStringLiteral("status.running"))
            .arg(strategyName.isEmpty() ? QStringLiteral("—") : strategyName);
    case IZapretBackend::State::Stopping:
        return LanguageManager::tr(QStringLiteral("status.stopping"));
    case IZapretBackend::State::Error:
        return LanguageManager::tr(QStringLiteral("status.error"));
    case IZapretBackend::State::Stopped:
        break;
    }
    return LanguageManager::tr(QStringLiteral("status.stopped"));
}

QString MainWindow::connectionText(IZapretBackend::State state)
{
    switch (state) {
    case IZapretBackend::State::Starting:
        return QStringLiteral("CONNECTING");
    case IZapretBackend::State::Running:
        return QStringLiteral("CONNECTED");
    case IZapretBackend::State::Stopping:
        return QStringLiteral("STOPPING");
    case IZapretBackend::State::Error:
        return QStringLiteral("ERROR");
    case IZapretBackend::State::Stopped:
        break;
    }
    return QStringLiteral("OFFLINE");
}

QString MainWindow::statusKind(IZapretBackend::State state)
{
    switch (state) {
    case IZapretBackend::State::Running:  return QStringLiteral("running");
    case IZapretBackend::State::Error:    return QStringLiteral("error");
    case IZapretBackend::State::Starting:
    case IZapretBackend::State::Stopping: return QStringLiteral("starting");
    case IZapretBackend::State::Stopped:
        break;
    }
    return QStringLiteral("stopped");
}

PowerButton::Mode MainWindow::powerMode(IZapretBackend::State state)
{
    switch (state) {
    case IZapretBackend::State::Running:
        return PowerButton::Mode::Active;
    // Both in-flight states wear the breathing ring. A teardown is a transition like a launch, and the
    // one look the window has for "something is happening, press will not help" has to cover both —
    // otherwise the Stop press and the stateChanged that follows it draw two different buttons for one
    // state, which is exactly the drift the statics in this class exist to prevent.
    case IZapretBackend::State::Starting:
    case IZapretBackend::State::Stopping:
        return PowerButton::Mode::Busy;
    case IZapretBackend::State::Stopped:
    case IZapretBackend::State::Error:
        break;
    }
    return PowerButton::Mode::Idle;
}

bool MainWindow::canStartFrom(IZapretBackend::State state)
{
    switch (state) {
    case IZapretBackend::State::Stopped:
    case IZapretBackend::State::Error:
        // Nothing is up and nothing is in flight — including after a failure, which is precisely when
        // the user needs the retry to stay reachable.
        return true;
    case IZapretBackend::State::Starting:
    case IZapretBackend::State::Running:
    case IZapretBackend::State::Stopping:
        break;
    }
    // All three in-flight/busy states. Stopping is the one that used to be missed: the payload is gone
    // or already dying, but the teardown (iptables through pkexec, up to ten seconds in the reported
    // log) still owns the backend, and start() answers "an operation is already in progress".
    return false;
}

bool MainWindow::canStopFrom(IZapretBackend::State state)
{
    switch (state) {
    case IZapretBackend::State::Running:
        return true;
    case IZapretBackend::State::Starting:
        // A launch in flight stays cancellable on purpose: a controller start can hang, and
        // LinuxBackend queues a stop asked for in this phase ("stop queued until the start command
        // returns") instead of racing it. Greying this out would leave no way out of a stuck launch.
        return true;
    case IZapretBackend::State::Stopped:
    case IZapretBackend::State::Error:
    case IZapretBackend::State::Stopping:
        break;
    }
    // Stopping is not a second stop waiting to happen: re-issuing one ran "terminating nfqws" a second
    // time while the first was still in flight, and in the direct-payload path it blocks the GUI inside
    // waitForFinished() for a death that is already under way.
    return false;
}

QString MainWindow::blockedReason(IZapretBackend::State state)
{
    switch (state) {
    case IZapretBackend::State::Starting:
        return LanguageManager::tr(QStringLiteral("gate.starting"));
    case IZapretBackend::State::Stopping:
        return LanguageManager::tr(QStringLiteral("gate.stopping"));
    case IZapretBackend::State::Running:
        // The missing action here is not another strategy's start but this run's stop.
        return LanguageManager::tr(QStringLiteral("gate.running"));
    case IZapretBackend::State::Stopped:
    case IZapretBackend::State::Error:
        break;
    }
    return QString();
}

// The services a profile row carries a chip for. The dashboard probes one more — a baseline that
// answers whether or not zapret is running, which is exactly what makes its badge a statement about the
// path rather than about a strategy. On a row that number would be a claim the profile never made, so
// the rows keep the services an entry exists to unblock. Filtering the probed list rather than naming a
// pair is the point: a chip for a host nobody measures is the one dishonest number the page could show.
QStringList MainWindow::profilePingHosts()
{
    QStringList hosts = PingService::defaultHosts();
    hosts.removeAll(QStringLiteral("google.com"));
    return hosts;
}

bool MainWindow::rowFollowsCycle(bool manualRound, bool rowIsActive)
{
    // The running profile's row is being measured by this very cycle. An idle row is not: the path it
    // would have opened is closed, so any number arriving for it belongs to somebody else's run — and a
    // row whose figures move while it does nothing is how a list of profiles starts lying. The manual
    // round is the exception the page's own button asks for: every row is then being measured at once.
    if (manualRound)
        return true;
    return rowIsActive;
}

void MainWindow::updateStatusUi()
{
    renderStatus(m_manager->state());
    updateTrayIcon();
}

// Draw the two status controls for one backend state.
//
// Split out of updateStatusUi() because the Stop press has to draw a state the backend has not reported
// yet — see showStoppingFeedback() — and a second copy of this mapping is how a chip and a button start
// telling two stories about one run. The tray icon is deliberately NOT drawn here: it reads the
// manager's live state and has no part in what the window itself shows.
void MainWindow::renderStatus(IZapretBackend::State state)
{
    if (m_statusLabel) {
        // The chip's word is the plain state, not the sentence the old pill carried: "CONNECTED" beside a
        // dot, with the strategy's name reserved for the log and the tray tooltip, which both say it more
        // precisely. The window title does not carry it either — the design keeps the title bar short.
        setConnectionChip(connectionText(state), statusKind(state));
    }

    // The selected row is the profile the power button will run, so the edge follows the user's choice and
    // nothing else. It is deliberately NOT snapped to the manager's active strategy here: while a run is up
    // the button means "stop", and letting the live run hijack the highlight would make it impossible to
    // pick a different profile to run next. The route is moved to the profile that actually started by
    // startStrategy (on an accepted launch), so "what's running" and "what I'll run next" stay the same row
    // through a normal start, and a mid-run re-pick is honoured.
    const bool canStart = canStartFrom(state);
    const bool canStop = canStopFrom(state);
    const QString busy = blockedReason(state);

    // The power button is the window's only start/stop control, so all the gating the toolbar action and
    // the rail's power glyph used to share lands here: which look it wears, and what it says it is waiting
    // for when a press would be refused.
    if (m_power) {
        m_power->setMode(powerMode(state));
        // The breath says "this run is live, or is being taken down" — the two states in which something
        // is actually running in the path. A launch in flight is not one of them: it has no run to be
        // alive yet, and the ring already breathes for it (see powerMode). Driven off the state rather
        // than off the mode because the mode cannot tell those two apart.
        m_power->setPulsing(state == IZapretBackend::State::Running
                            || state == IZapretBackend::State::Stopping);
        m_power->setEnabled(canStart || canStop);
        m_power->setToolTip(canStop ? stopActionHint()
                                    : (busy.isEmpty() ? startActionHint() : busy));
    }

    for (auto it = m_cards.cbegin(); it != m_cards.cend(); ++it)
        it.value()->setSelected(it.key() == m_selectedId);
}

// Answer a Stop press on screen before the teardown it asked for has begun.
//
// This is not a courtesy. The stop runs in this thread, and the backend waits inside it — a bounded
// wait, but a wait: waitForFinished() on the payload, then the privileged cleanup commands, each with
// its own deadline. While a slot is executing the event loop is not, so the repaint that
// stateChanged(Stopping) would normally cause cannot happen until the teardown is over. That is what
// the report was: a button pressed, and nothing at all moving on screen for seconds.
//
// So the stopping look is applied here, from the state the teardown is about to enter, and painted
// synchronously — repaint(), not update(), because update() only queues the paint for a loop that is
// about to be busy. Once the backend does report, updateStatusUi() redraws from the real state, which
// is why the look drawn here has to come from the same table the signal draws from, and not a second
// one invented for the press.
void MainWindow::showStoppingFeedback()
{
    renderStatus(IZapretBackend::State::Stopping);

    // A window that is hidden — stopped from the tray or from the shortcut — has nothing to paint, and
    // repainting a hidden widget is work thrown away.
    if (m_power && m_power->isVisible())
        m_power->repaint();
    if (m_statusLabel && m_statusLabel->isVisible())
        m_statusLabel->repaint();
    if (m_statusDot && m_statusDot->isVisible())
        m_statusDot->repaint();
}

void MainWindow::updateTrayIcon()
{
    if (!m_tray)
        return;
    const bool running = m_manager->isRunning() || m_manager->state() == IZapretBackend::State::Starting;
    const QColor tint = running ? Theme::success() : Theme::mutedText();
    m_tray->setIcon(Theme::icon(QStringLiteral("tray"), tint, 22));
    m_tray->setToolTip(QStringLiteral("Muffin — %1")
                           .arg(statusText(m_manager->state(),
                                           m_manager->activeStrategy()
                                               ? m_manager->activeStrategy()->name
                                               : QString())));
}

void MainWindow::applyTheme()
{
    Theme::apply(*qApp, Theme::modeFromName(m_settings->theme()));

    // Every baked-pixmap icon in the window is re-tinted here: a theme switch cannot repaint a pixmap the
    // sheet never owned. Each button carries the glyph name and the size it was drawn at, so one pass
    // covers the rail's 24 px icons, the pages' 16 px ones and the header's window controls alike. Both
    // QPushButtons and QToolButtons carry the property, so the glyph pass runs over each class.
    const auto buttons = findChildren<QPushButton *>();
    for (QPushButton *b : buttons) {
        const QString glyph = b->property("glyph").toString();
        if (glyph.isEmpty())
            continue;
        const int px = b->property("glyphPx").toInt();
        b->setIcon(Theme::themedIcon(glyph, px > 0 ? px : 16));
    }
    const auto tools = findChildren<QToolButton *>();
    for (QToolButton *b : tools) {
        const QString glyph = b->property("glyph").toString();
        if (glyph.isEmpty())
            continue;   // the language button is a flag, not a glyph, and keeps its own tint
        const int px = b->property("glyphPx").toInt();
        b->setIcon(Theme::themedIcon(glyph, px > 0 ? px : 16));
    }
    if (QLabel *logo = findChild<QLabel *>(QStringLiteral("brandLogo")))
        logo->setPixmap(Theme::icon(QStringLiteral("app"), Theme::accent(), kBrandGlyphPx)
                            .pixmap(kBrandGlyphPx, kBrandGlyphPx));

    // The power ring is painted from Theme::accent()/pingOk() rather than from the sheet, so it needs
    // telling that the palette moved. The profile rows bake their menu glyph and their ping chips the
    // same way, so each re-bakes on the same signal.
    if (m_power)
        m_power->retheme();
    for (ProfileCard *card : findChildren<ProfileCard *>())
        card->retheme();
    // The dashboard's rings bake their mark into a pixmap as well, and that mark is the one glyph in
    // the window whose colour is decided by the page behind it rather than by the sheet: white on the
    // dark canvas, near-black on the light one. Left alone it stays white, and on the light sheet's
    // #FFFFFF page the three service marks are simply invisible.
    for (PingIndicator *chip : findChildren<PingIndicator *>())
        chip->retheme();
    // The history page paints its row tints, its status words and its placeholder glyph from Theme
    // tokens rather than from the sheet, so it has to be told the palette moved like any other
    // code-coloured widget.
    if (m_historyPage)
        m_historyPage->retheme();

    // No setWindowIcon() here: the title bar is deliberately iconless, and a theme switch that
    // re-attached the muffin would put it back the first time anyone changed the colour scheme.
    updateLanguageButton();
    updateTrayIcon();
}

// Re-write every string this window owns in the language LanguageManager now holds.
//
// Nothing here knows what a widget *is*: it walks the object tree for the i18n* properties the builders
// left behind (see retranslateUi's declaration), so a new label is translated by setting its property at
// construction and needs no second list. Cards, the log panel and any dialog parented to this window are
// descendants and come along in the same walk.
void MainWindow::retranslateUi()
{
    retranslateTree(this);

    // Rows derive their name from the strategy rather than from a key, so they re-derive it; the menu
    // actions and the chip wording carry their own keys and move with the walk above.
    for (ProfileCard *card : findChildren<ProfileCard *>())
        card->retranslateText();

    // The rail's flag, the menu tick, and any text the status machine writes rather than the builders.
    updateLanguageButton();
    updateStatusUi();

    // The history page's headers, verdict words and summary sentence are derived rather than tagged, so
    // the tree walk above cannot reach them and the page has to re-read them itself.
    if (m_historyPage)
        m_historyPage->retranslateUi();
}

void MainWindow::refreshStrategies()
{
    const int count = m_manager->reload();
    populateCards();
    showTransientMessage(LanguageManager::tr(QStringLiteral("msg.found_count")).arg(count), 4000);
    m_log->appendLine(LanguageManager::tr(QStringLiteral("log.rescanned"))
                          .arg(count)
                          .arg(m_manager->resolvedRoot().isEmpty()
                                   ? LanguageManager::tr(QStringLiteral("log.no_dir"))
                                   : m_manager->resolvedRoot()),
                      LogPanel::Info);
    if (count == 0)
        m_log->appendLine(LanguageManager::tr(QStringLiteral("log.empty_dir")), LogPanel::Warn);
}

void MainWindow::openSettings()
{
    // Settings is a page of the stack now, not a modal. Everything the page can change fires through
    // Settings::changed() into onSettingsChanged(), which re-points the manager and re-themes on Save —
    // there is nothing to do here but go there. Kept as a named slot because three places reach it (the
    // rail glyph, Ctrl+S, and the startup "no strategies yet" prompt) and one of them defers it.
    setCurrentPage(PageSettings);
}

void MainWindow::downloadZapret()
{
    DownloadDialog dialog(this);
    connect(&dialog, &DownloadDialog::installed, this, &MainWindow::zapretInstalled);
    // A desktop window is the one place a password prompt makes sense: the user is here, the polkit
    // agent is reachable, and the answer decides whether the payload they just downloaded will ask
    // again on every start. Off in every other caller — a headless test must never be able to reach
    // pkexec by constructing a downloader.
    dialog.setPromoteAfterInstall(true);
    // The constructor only builds widgets; the request starts here. A dialog that is constructed but
    // never started — in a test, or by a code path that closes it immediately — touches no network.
    dialog.startFetch();
    dialog.exec();
}

void MainWindow::zapretInstalled(const QString &path)
{
    m_log->appendLine(LanguageManager::tr(QStringLiteral("log.installed"))
                          .arg(QDir::toNativeSeparators(path)),
                      LogPanel::Info);

    // Exactly one rescan, whichever way the path moves. Writing Settings fires changed(), and
    // onSettingsChanged re-points the manager whenever the value actually differs; when it does not
    // differ, the directory that was scanned a moment ago was empty, so the catalog still has to be
    // built again by hand.
    if (m_settings->zapretPath() != path)
        m_settings->setZapretPath(path);
    else
        refreshStrategies();
}

void MainWindow::startStrategy(const QString &id, bool interactive)
{
    // Asked before the backend sees the request. The power button is greyed in exactly these states, but
    // the tray menu, Ctrl+Shift+S and the auto-start on launch are not — and unguarded they reached
    // LinuxBackend::start() mid-teardown, which reported "an operation is already in progress (stop
    // first)" as a failure and badged the row that was merely started too early. Waiting is not an
    // error, so it gets a warning line and a footer message instead of a dialog.
    const QString busy = blockedReason(m_manager->state());
    if (!busy.isEmpty()) {
        m_log->appendLine(busy, LogPanel::Warn);
        showTransientMessage(busy, 5000);
        return;
    }

    const Strategy *strategy = m_manager->findStrategy(id);
    if (!strategy) {
        const QString msg = LanguageManager::tr(QStringLiteral("error.strategy_not_found")).arg(id);
        m_log->appendLine(msg, LogPanel::Error);
        notifyError(msg, interactive);
        return;
    }

    m_pendingStartId = id;
    QString error;
    if (!m_manager->start(*strategy, &error)) {
        const QString msg = error.isEmpty()
                            ? LanguageManager::tr(QStringLiteral("error.backend_refused"))
                            : error;
        m_pendingStartId.clear();
        // Every backend rejection path already emitted errorOccurred, and this window logs that — so
        // logging here as well duplicated the line (visible as two identical ERR rows for one click).
        // Only a rejection that arrived with no message at all needs one from us.
        if (error.isEmpty())
            m_log->appendLine(msg, LogPanel::Error);
        attachErrorBadge(id, msg);
        notifyError(msg, interactive);
        updateStatusUi();
        return;
    }

    // Accepted: drop a badge an earlier attempt may have left on this very card.
    attachErrorBadge(id, QString());

    // The route the power button will run next is now the profile that just started, so "what's running"
    // and "what I'll run next" agree — and after this run comes down, the highlight is already on the
    // right row. Written to Settings too (under lastStrategyId) so a restart resumes the same choice.
    const QString acceptedId = strategy->id.isEmpty() ? strategy->filePath : strategy->id;
    m_selectedId = acceptedId;
    for (auto it = m_cards.cbegin(); it != m_cards.cend(); ++it)
        it.value()->setSelected(it.key() == acceptedId);

    m_applyingSettings = true;
    m_settings->setLastStrategyId(acceptedId);
    m_applyingSettings = false;
}

void MainWindow::onStopRequested()
{
    // Reached from the toolbar action, the tray menu and Ctrl+Shift+X. The tray and shortcut paths are not
    // gated in updateStatusUi(), so this guard is what keeps them from re-issuing a stop that is already
    // running — the second "terminating nfqws" ten seconds after the first.
    if (!canStopFrom(m_manager->state()))
        return;

    // Before the stop, not after it: the teardown blocks this thread, so a look applied afterwards is a
    // look the user sees only when the waiting is already over.
    showStoppingFeedback();

    m_manager->stop();

    // Reconcile with what the backend actually did. A stop it refused, or one that had nothing to do,
    // left the window claiming a teardown that never started; a stop that ran to completion in the call
    // above has already reported Stopping or Stopped through onStateChanged, and this redraws the same
    // thing it drawn there. One mapping, two callers, no window that can lie about a run.
    updateStatusUi();
}

// A row was pressed. The window decides what selection means: this is now the profile the power button
// will run. The row's own clicked() fired for a left-press anywhere on it, and only one row is ever lit,
// so the edge moves here rather than in the card. Persisted through Settings under lastStrategyId so the
// choice survives a restart and the auto-start-on-launch path still has a name to start.
void MainWindow::onProfileSelected(const QString &id)
{
    ProfileCard *card = m_cards.value(id, nullptr);
    if (!card || id == m_selectedId)
        return;
    m_selectedId = id;
    for (auto it = m_cards.cbegin(); it != m_cards.cend(); ++it)
        it.value()->setSelected(it.key() == id);

    m_applyingSettings = true;
    m_settings->setLastStrategyId(id);
    m_applyingSettings = false;

    // While a run is up the button means "stop", and pressing it would tear down the live run rather than
    // switch to this profile — so say what selecting actually did, and let the user stop first if that is
    // what they meant. The power button's own tooltip already carries the refusal reason.
    const QString name = card->strategy().name.isEmpty() ? card->nameLabel()->text()
                                                          : card->strategy().name;
    showTransientMessage(name, 3000);
    updateStatusUi();
}

// The row already put the command on the clipboard before emitting this. The window's only job is to say
// so in the footer — a clipboard write with no feedback is indistinguishable from a dead button.
void MainWindow::onProfileCopied(const QString &id)
{
    ProfileCard *card = m_cards.value(id, nullptr);
    if (!card)
        return;
    showTransientMessage(LanguageManager::tr(QStringLiteral("profile.copied"))
                             .arg(card->commandLine()), 4000);
}

// The row asked to delete itself. A profile is a real file in the zapret folder, so this is the one menu
// entry that destroys something: it confirms first, then removes, then rescans so the list and the
// manager agree about what is gone. If the deleted profile is the selected one, selection falls back to
// the next row (populateCards does that), and the power button will not keep naming a deleted file.
void MainWindow::onProfileDeleteRequested(const QString &id)
{
    ProfileCard *card = m_cards.value(id, nullptr);
    if (!card)
        return;
    const Strategy strategy = card->strategy();
    const QString path = strategy.filePath;
    if (path.isEmpty()) {
        m_log->appendLine(LanguageManager::tr(QStringLiteral("profile.delete_failed")).arg(id),
                          LogPanel::Error);
        return;
    }

    const QString name = strategy.name.isEmpty() ? card->nameLabel()->text() : strategy.name;
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, LanguageManager::tr(QStringLiteral("profile.delete_title")),
        LanguageManager::tr(QStringLiteral("profile.delete_confirm")).arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    QFile file(path);
    if (file.remove()) {
        m_log->appendLine(LanguageManager::tr(QStringLiteral("profile.deleted")).arg(name),
                          LogPanel::Info);
        // A running profile whose file was just removed stays running (the payload is already in memory);
        // deleting the entry only takes it off the list. Rescan rebuilds the rows and repoint the route.
        refreshStrategies();
    } else {
        m_log->appendLine(LanguageManager::tr(QStringLiteral("profile.delete_failed"))
                              .arg(QDir::toNativeSeparators(path)),
                          LogPanel::Error);
    }
}

void MainWindow::onStateChanged(IZapretBackend::State state)
{
    // Once the attempt is over — up or down — later errors belong to whatever is active, not to a
    // pending start that already resolved.
    if (state == IZapretBackend::State::Running || state == IZapretBackend::State::Stopped)
        m_pendingStartId.clear();
    // A quiet cycle measuring a run that has just come down is measuring the teardown: the probes that
    // follow it fail against a closed path, and calling that "the provider changed its rules" would be
    // the window arguing with a decision the user just made. The verdict is dropped at onWatchdogResult().
    if (m_watchdogRun && state != IZapretBackend::State::Running)
        m_watchdogVerdictVoid = true;
    // Capture the run's card while the manager still names it. It forwards stateChanged before its own
    // bookkeeping slot clears m_activeId, so this is the last moment the two agree — and a portable
    // failure that deferred its report to after the nfqueue teardown arrives with nothing to attribute
    // to otherwise (see errorTargetId). A run that came down cleanly leaves no grievance behind.
    if (state == IZapretBackend::State::Stopped)
        m_lastRunId.clear();
    else if (const QString id = m_manager->activeStrategyId(); !id.isEmpty())
        m_lastRunId = id;
    // The deferred half of a watchdog rotation: the stop it waited on has now been reported (Stopped by
    // the teardown, Error by a run that died instead — either way the backend is free, which is what
    // canStartFrom() answers for). Called after the bookkeeping above so the start, which writes the
    // pending-start id, is not overwritten by it.
    if (state == IZapretBackend::State::Stopped || state == IZapretBackend::State::Error)
        continueWatchdogRotation();
    updateStatusUi();
    if (state == IZapretBackend::State::Stopped && m_tray && !isVisible())
        updateTrayIcon();
}

void MainWindow::onStrategyStarted(const QString &id, const QString &name)
{
    m_log->appendLine(LanguageManager::tr(QStringLiteral("log.start_requested")).arg(name),
                      LogPanel::Info);
    attachErrorBadge(id, QString());   // a fresh attempt supersedes the previous failure badge
    updateStatusUi();
}

void MainWindow::onStrategyStopped()
{
    m_log->appendLine(LanguageManager::tr(QStringLiteral("log.strategy_stopped")), LogPanel::Info);
    updateStatusUi();
}

void MainWindow::onLogMessage(const QString &line, int level)
{
    m_log->appendLine(line, level);
    // The journal no longer shares the screen with the cards, so a failure that lands while the user is
    // reading strategy names has to announce itself from the rail instead of scrolling past unseen.
    if (level >= LogPanel::Warn && currentPage() != PageLogs)
        setLogAlert(true);
}

void MainWindow::onErrorOccurred(const QString &message)
{
    // Deliberately NOT logged here: every emitter (both backends, and ZapretManager's own rejections)
    // writes the same text through logMessage first, so logging again put two identical ERR rows in the
    // panel for one click. errorOccurred is the notification channel — badge, dialog, balloon.
    // See the contract on IZapretBackend::errorOccurred.

    // Pin the reason where it stays visible after the dialog is dismissed: on the card that asked for
    // this start, or — when that attempt already resolved — on the card that is currently active. A
    // third case exists only on the deferred paths: portable mode reports its failure after the queue
    // teardown, and by then the manager has forgotten which strategy was running, so the window's own
    // memory of the last live run is what still identifies the card.
    const QString targetId = errorTargetId(m_pendingStartId, m_manager->activeStrategyId(),
                                           m_lastRunId);
    if (!targetId.isEmpty())
        attachErrorBadge(targetId, message);

    notifyError(message, true);
}

QString MainWindow::errorCardId() const
{
    return m_errorCardId;
}

void MainWindow::attachErrorBadge(const QString &id, const QString &message)
{
    ProfileCard *card = m_cards.value(id, nullptr);
    if (!card)
        return;
    // ProfileCard has no separate clearError(): an empty message is the clear, and the red edge plus the
    // reason in the tooltip are what the row does with it. The row is only here to say which profile failed
    // and why, not to offer a retry — that is the power button's again.
    card->setError(message);
    if (card->hasError())
        m_errorCardId = id;
    else if (m_errorCardId == id)
        m_errorCardId.clear();
}

void MainWindow::notifyError(const QString &message, bool interactive)
{
    const qint64 now = m_errorClock.elapsed();
    const bool repeat = (message == m_lastErrorText)
                        && (now - m_lastErrorAt < kErrorDialogWindowMs);
    m_lastErrorText = message;
    m_lastErrorAt = now;
    if (repeat)
        return;   // already announced; the log and the card badge still hold every occurrence

    if (!interactive) {
        // An automatic start must not steal focus with a modal: a balloon, or the status bar.
        if (m_tray)
            m_tray->showMessage(QStringLiteral("Muffin"), message,
                                Theme::icon(QStringLiteral("tray"), Theme::error(), 22), 5000);
        else
            showTransientMessage(message, 6000);
        return;
    }

    if (isVisible() && !QApplication::activeModalWidget())
        QMessageBox::warning(this, QStringLiteral("Muffin"), message);
    else if (m_tray)
        m_tray->showMessage(QStringLiteral("Muffin"), message,
                            Theme::icon(QStringLiteral("tray"), Theme::error(), 22), 5000);
}

void MainWindow::onPingUpdated(const QString &host, int status, int latencyMs)
{
    const PingIndicator::PingStatus ps = PingIndicator::statusFromServiceInt(status);

    // Remember the verdict so a profile row built later in this cycle can be seeded with the live number
    // instead of grey (populateCards reads m_lastPing), then push it to the dashboard ring.
    PingMemo memo;
    memo.status = ps;
    memo.latencyMs = latencyMs;
    m_lastPing.insert(host, memo);

    if (auto it = m_indicators.constFind(host); it != m_indicators.constEnd())
        it.value()->setStatus(ps, latencyMs);

    // The rows are the part that has to be chosen rather than broadcast. This cycle measures the path
    // the running profile opened, so only that row may take the number; every other row keeps the last
    // verdict it was given, which is what its chips are for when nothing is running at all. During a
    // manual round the window has asked for everybody, so everybody is served.
    const QString runningId = m_manager->activeStrategyId();
    for (auto it = m_cards.constBegin(); it != m_cards.constEnd(); ++it)
        if (rowFollowsCycle(m_pingTestActive, it.key() == runningId))
            it.value()->setPing(host, ps, latencyMs);

    // The round is over when every probed host has answered once. Only a verdict counts: the service
    // announces "measuring" before each probe, and treating that as an answer would end the round the
    // moment the first host started rather than after the last one finished.
    if (!m_pingTestActive || ps == PingIndicator::PingStatus::Waiting)
        return;
    m_pingTestPending.remove(host);
    if (m_pingTestPending.isEmpty())
        endPingTest();
}

// One cycle, asked for by hand, handed to every row. The periodic cycle is untouched — the service runs
// this one on the same worker between beats — so pressing the button measures now without changing how
// often the dashboard moves.
void MainWindow::onPingTestRequested()
{
    if (m_pingTestActive)
        return;   // already measuring; the button says so
    beginPingTest();
    m_ping->pingNow();
}

void MainWindow::beginPingTest()
{
    m_pingTestActive = true;
    const QStringList probed = m_ping->hosts();
    m_pingTestPending = QSet<QString>(probed.cbegin(), probed.cend());
    m_pingTestWatchdog->start(kPingTestWatchdogMs);

    // Say "measuring" on every row immediately, not only on the one that is running: the press has to
    // answer in the same frame, and a chip that keeps an old figure until its probe completes is
    // indistinguishable from a button that did nothing.
    const QStringList hosts = profilePingHosts();
    for (ProfileCard *card : std::as_const(m_cards))
        for (const QString &host : hosts)
            card->setPing(host, PingIndicator::PingStatus::Waiting, -1);

    // The button carries the round's own wording, and it is the wording that says the press worked — so
    // it is swapped through the i18n key rather than through a literal, and a language change mid-round
    // re-says the same state instead of falling back to the idle label. It also stops answering presses:
    // a second one would be refused outright, and a control that refuses silently looks broken.
    if (m_pingTestButton) {
        m_pingTestButton->setEnabled(false);
        i18nText(m_pingTestButton, QStringLiteral("action.ping_checking"));
    }
}

void MainWindow::endPingTest()
{
    m_pingTestActive = false;
    m_pingTestPending.clear();
    if (m_pingTestWatchdog)
        m_pingTestWatchdog->stop();
    if (m_pingTestButton) {
        m_pingTestButton->setEnabled(true);
        i18nText(m_pingTestButton, QStringLiteral("action.ping_test"));
    }
}

// ---- the real-bypass check ----

// The profiles page's second measuring press. Unlike the ping round this one has teeth: the engine
// starts the selected profile (on Linux through pkexec, so the polkit prompt can appear, exactly as
// for the power button), measures the services through it, and stops it again. The window's job is
// the gate, the meter and the sheet — the measurement itself is the engine's, start to verdict.
void MainWindow::onBlockcheckRequested()
{
    if (m_blockcheckActive)
        return;   // a cycle is in flight; the button says so and is not answering presses

    // A quiet watchdog cycle holds the same engine. The press is refused rather than queued, and said
    // out loud: unlike the manual round this one leaves the button looking perfectly idle, so a silent
    // return here would read as a button that stopped working.
    if (m_watchdogRun) {
        showTransientMessage(LanguageManager::tr(QStringLiteral("watchdog.busy")), 4000);
        return;
    }

    // The same gate the power button answers to, widened by exactly one state: a backend owns one
    // operation at a time, so Starting/Stopping refuse — but a run that is already up is fine, the
    // engine measures it as-is and leaves its lifecycle alone (it only stops what it started).
    const IZapretBackend::State state = m_manager->state();
    if (state != IZapretBackend::State::Running && !canStartFrom(state)) {
        showTransientMessage(blockedReason(state), 4000);
        return;
    }
    const Strategy *s = m_selectedId.isEmpty() ? nullptr : m_manager->findStrategy(m_selectedId);
    if (!s) {
        showTransientMessage(LanguageManager::tr(QStringLiteral("msg.pick_profile")), 5000);
        return;
    }

    beginBlockcheck();
    // The name the history rows get, kept for the whole cycle: the engine reports a host and a verdict,
    // and the preset that produced them is this window's to know.
    m_blockcheckPreset = s->name;
    m_log->appendLine(LanguageManager::tr(QStringLiteral("log.blockcheck_started")).arg(s->name),
                      LogPanel::Info);
    // The row's two hosts, not the dashboard's three: this check is about the path the profile
    // exists to open, and google.com is the baseline no profile ever promised.
    m_blockcheck->startTest(*s, profilePingHosts());
}

void MainWindow::beginBlockcheck()
{
    m_blockcheckActive = true;
    if (m_blockcheckButton) {
        m_blockcheckButton->setEnabled(false);
        i18nText(m_blockcheckButton, QStringLiteral("action.blockcheck_running"));
    }
    if (m_blockcheckProgress) {
        m_blockcheckProgress->setRange(0, 0);   // busy until the first host lands
        m_blockcheckProgress->show();
    }
}

void MainWindow::endBlockcheck()
{
    m_blockcheckActive = false;
    // The cycle is over, so no later verdict may claim this preset. Cleared rather than left: an
    // unsolicited hostChecked after this point would otherwise be recorded against the wrong profile.
    m_blockcheckPreset.clear();
    if (m_blockcheckButton) {
        m_blockcheckButton->setEnabled(true);
        i18nText(m_blockcheckButton, QStringLiteral("action.blockcheck"));
    }
    if (m_blockcheckProgress) {
        m_blockcheckProgress->hide();
        m_blockcheckProgress->setRange(0, 0);
    }
}

// Every run ends here — complete, cancelled (a partial map) or failed (an empty one) — so this is
// the one verdict sheet per press, and the only modal the cycle ever opens.
void MainWindow::onBlockcheckFinished(const QJsonObject &results)
{
    // One exception: a cycle the watchdog started is not a press and gets no sheet. Its own handler
    // reads the map for a dead bypass and stays silent otherwise, which is the whole difference between
    // a check the user asked for and one that is merely watching.
    if (m_watchdogRun) {
        onWatchdogResult(results);
        return;
    }

    endBlockcheck();
    if (results.isEmpty()) {
        const QString why = m_blockcheck ? m_blockcheck->lastError() : QString();
        QMessageBox::warning(this, LanguageManager::tr(QStringLiteral("blockcheck.title")),
                             why.isEmpty()
                                 ? LanguageManager::tr(QStringLiteral("blockcheck.no_results"))
                                 : why);
        return;
    }
    QMessageBox::information(this, LanguageManager::tr(QStringLiteral("blockcheck.title")),
                             blockcheckResultText(results));
}

QString MainWindow::blockcheckResultText(const QJsonObject &results)
{
    QStringList lines;
    for (auto it = results.constBegin(); it != results.constEnd(); ++it) {
        const QJsonObject one = it.value().toObject();
        const bool ok = one.value(QLatin1String("success")).toBool();
        // OK / FAIL / ms stay in the source's own words like the other design tokens (the header
        // chip's "CONNECTED" is the precedent): a verdict stamp, not a sentence.
        QString line = ok
            ? QStringLiteral("%1: OK (%2 ms)")
                  .arg(friendlyHostName(it.key()))
                  .arg(one.value(QLatin1String("latency")).toInt())
            : QStringLiteral("%1: FAIL").arg(friendlyHostName(it.key()));
        // The one result that is worse than unreachable: the path is open but somebody in the
        // middle owns its certificate. It gets the catalog's warning appended, in the UI language.
        if (ok && !one.value(QLatin1String("certificate_valid")).toBool(true))
            line += QStringLiteral(" — %1")
                        .arg(LanguageManager::tr(QStringLiteral("blockcheck.cert_invalid")));
        lines << line;
    }
    return lines.join(QLatin1Char('\n'));
}

// ---- the watchdog ----
//
// The same measurement the profiles page's button asks for, asked on a timer instead of on a press, of
// one host instead of two, and answered only when it comes back wrong. What follows is the whole policy:
// five pure predicates that say when to measure, what counts as a dead bypass and which profile is next;
// then the thin impulsive half that asks the engine, speaks up, and — if the user says yes — hands the
// running profile to the next row in the list.

QString MainWindow::watchdogQuickHost()
{
    // Discord rather than Youtube because the question has to be cheap: one host, one port, three tries.
    // It is also the service the profile lists are drawn around, so a FAIL here is a FAIL the user would
    // have noticed within the hour anyway.
    return QStringLiteral("discord.com");
}

int MainWindow::watchdogIntervalMs(int minutes)
{
    // Clamped here too, not only in Settings, because the caller is not always Settings: a test, a
    // hand-edited ini or a future default all arrive as a plain int, and a 0 or a negative value would
    // arm a QTimer that fires on every tick of the event loop.
    const int bounded = qBound(Settings::kWatchdogMinIntervalMin, minutes,
                               Settings::kWatchdogMaxIntervalMin);
    return bounded * 60 * 1000;
}

bool MainWindow::watchdogShouldProbe(IZapretBackend::State state, bool quietRunInFlight,
                                     bool manualCycleInFlight)
{
    // Only a live run. A stopped profile has no path to measure, so a probe would report what the
    // provider does without zapret and the window would call it the profile's verdict — the one answer
    // more misleading than the question. Transitions are excluded by the same reasoning: Starting has
    // not opened anything yet, and Stopping is taking it back down.
    //
    // And one engine serves one cycle, whoever asked: a tick landing during the manual round would be
    // refused outright by the shared engine, which is a wasted measurement and a confusing silence.
    return state == IZapretBackend::State::Running
           && !quietRunInFlight && !manualCycleInFlight;
}

bool MainWindow::watchdogCycleFailed(const QJsonObject &results)
{
    // An empty map is not a failure of the bypass; it is a failure of the measurement (no manager, a
    // start the backend refused, a preset that never came up). Asking "want to switch profiles?" because
    // the probe broke would blame the provider for the window's own bad day.
    if (results.isEmpty())
        return false;
    for (auto it = results.constBegin(); it != results.constEnd(); ++it) {
        if (it.value().toObject().value(QLatin1String("success")).toBool())
            return false;   // one host through is the bypass working
    }
    return true;
}

QString MainWindow::watchdogNextStrategyId(const QList<Strategy> &strategies,
                                           const QString &currentId)
{
    // Rows are addressed the way the window addresses them everywhere else — id, or the file path when
    // the parser could not name the entry (populateCards uses the same expression).
    const auto rowId = [](const Strategy &s) { return s.id.isEmpty() ? s.filePath : s.id; };
    const int size = static_cast<int>(strategies.size());
    if (size < 2)
        return QString();   // one profile is not a list to rotate through

    int at = -1;
    for (int i = 0; i < size; ++i) {
        if (rowId(strategies.at(i)) == currentId) {
            at = i;
            break;
        }
    }

    if (at < 0) {
        // The run names nothing on the list (an entry deleted mid-flight, or an id inherited from a
        // previous session): the first row is the only sensible "next".
        for (const Strategy &s : strategies) {
            const QString candidate = rowId(s);
            if (!candidate.isEmpty() && candidate != currentId)
                return candidate;
        }
        return QString();
    }

    // Forward from the failed row, wrapping at the end — and never back onto the row itself, which is
    // what a list of two would otherwise do every other time.
    for (int step = 1; step <= size; ++step) {
        const QString candidate = rowId(strategies.at((at + step) % size));
        if (!candidate.isEmpty() && candidate != currentId)
            return candidate;
    }
    return QString();
}

void MainWindow::startWatchdog()
{
    if (!m_watchdogTimer)
        return;
    const int intervalMs = watchdogIntervalMs(m_settings->watchdogIntervalMin());
    // Re-arm only on a real change. Settings::changed() fires for every write, including the
    // last-strategy id startStrategy() persists on each start, and restarting the countdown for that
    // would push a five-minute check five minutes further out every time the user pressed the button.
    if (m_watchdogTimer->isActive() && m_watchdogTimer->interval() == intervalMs)
        return;
    m_watchdogTimer->stop();
    m_watchdogTimer->setInterval(intervalMs);
    m_watchdogTimer->start();
}

int MainWindow::watchdogTimerInterval() const
{
    return m_watchdogTimer ? m_watchdogTimer->interval() : -1;
}

bool MainWindow::watchdogTimerActive() const
{
    return m_watchdogTimer && m_watchdogTimer->isActive();
}

void MainWindow::onWatchdogTick()
{
    // Not while the window has a question open. A modal runs its own event loop, and this timer is
    // armed inside it: starting a measurement the user cannot see while they are answering something
    // else is a surprise waiting behind the dialog — and the rotation prompt is exactly such a modal.
    if (QApplication::activeModalWidget())
        return;

    if (!watchdogShouldProbe(m_manager->state(), m_watchdogRun, m_blockcheckActive))
        return;

    // The run to measure, not the row the pointer happens to be over: a watchdog that checked a profile
    // nobody is running would be reporting a promise rather than the path in use.
    const Strategy *s = m_manager->activeStrategy();
    if (!s) {
        const QString id = watchdogRunId();
        s = id.isEmpty() ? nullptr : m_manager->findStrategy(id);
    }
    if (!s)
        return;   // a Running the window cannot name: there is nothing to ask the engine about

    m_watchdogRun = true;
    m_watchdogVerdictVoid = false;   // a fresh cycle starts with a live run by definition
    startWatchdogProbe(*s);
}

void MainWindow::startWatchdogProbe(const Strategy &strategy)
{
    if (!m_blockcheck) {
        // Nothing to ask — and the flag has to come back down with the question, because a quiet cycle
        // that was never started would otherwise mute the journal and refuse the manual press forever.
        m_watchdogRun = false;
        return;
    }
    // The engine measures a run that is already up as-is and leaves its lifecycle alone (it only ever
    // stops what it started), so a quiet cycle costs a settle and a probe — not a teardown of the very
    // profile the user is relying on. m_watchdogRun is cleared by onWatchdogResult(), and testFinished
    // is contractually the last word of every run, including the ones the engine refuses.
    m_blockcheckPreset = strategy.name;
    m_blockcheck->startTest(strategy, QStringList{watchdogQuickHost()});
}

void MainWindow::onWatchdogResult(const QJsonObject &results)
{
    // The cycle is over whatever the map says, and this is the only place that ends it: the engine is
    // shared with the manual press, so the mute in wire() and the refusal in onBlockcheckRequested()
    // have to expire with the run — a refused one included.
    const bool voided = m_watchdogVerdictVoid;
    m_watchdogRun = false;
    m_watchdogVerdictVoid = false;
    m_blockcheckPreset.clear();   // the same reason endBlockcheck() clears it: this cycle is finished
    if (voided)
        return;   // the run this measured came down on the way; see m_watchdogVerdictVoid

    // Silence on a pass — that is the deal — and silence on a cycle that produced no verdict at all.
    if (!watchdogCycleFailed(results))
        return;

    const QString fromId = watchdogRunId();
    const QString fromName = watchdogProfileName(fromId);

    // Three voices for one event, and not one of them the verdict sheet. The journal keeps the sentence
    // at error level, which is the red line the alert exists to be (and which lights the rail's unread
    // badge through onLogMessage); the balloon is the only part that reaches a window sitting in the
    // tray; the footer stands in for the balloon on a desktop that publishes no tray host at all.
    onLogMessage(LanguageManager::tr(QStringLiteral("watchdog.alert")), LogPanel::Error);
    const QString notify = LanguageManager::tr(QStringLiteral("watchdog.notify")).arg(fromName);
    if (m_tray && m_tray->isVisible())
        m_tray->showMessage(QStringLiteral("Muffin"), notify,
                            Theme::icon(QStringLiteral("tray"), Theme::error(), 22), 8000);
    else
        showTransientMessage(notify, 8000);

    const QString nextId = watchdogNextStrategyId(m_manager->strategies(), fromId);
    if (nextId.isEmpty()) {
        // Nothing to offer, so nothing to ask: with one profile in the list, "switch to the next" is a
        // question with no answer in it. The journal says the same thing a dialog would have.
        m_log->appendLine(LanguageManager::tr(QStringLiteral("watchdog.no_next")), LogPanel::Warn);
        return;
    }

    if (!confirmWatchdogRotation(fromName, watchdogProfileName(nextId)))
        return;   // the user's own call: the alert has been given, the run stays exactly as it was

    // Said here rather than inside the act, because it belongs to what the user agreed to: the journal
    // records the decision, and the act itself is two backend calls with no message of their own.
    m_log->appendLine(LanguageManager::tr(QStringLiteral("watchdog.rotating"))
                          .arg(watchdogProfileName(nextId)),
                      LogPanel::Info);
    emit watchdogRotationRequested(fromId, nextId);
    performWatchdogRotation(fromId, nextId);
}

QString MainWindow::watchdogRunId() const
{
    // The same precedence a failure badge uses: the start this window asked for that has not resolved,
    // else the run the manager still names, else the run it has already forgotten. The selected row is
    // the last resort, for a verdict that arrives after everything has come down.
    const QString id = errorTargetId(m_pendingStartId, m_manager->activeStrategyId(), m_lastRunId);
    return id.isEmpty() ? m_selectedId : id;
}

QString MainWindow::watchdogProfileName(const QString &id) const
{
    if (const Strategy *s = m_manager->findStrategy(id))
        return s->name;
    // A row that has left the catalog still has to be named in the sentence about it, and its id is the
    // only honest thing left: "unknown" would lose which profile the user is being asked to give up.
    return id;
}

bool MainWindow::confirmWatchdogRotation(const QString &fromName, const QString &nextName)
{
    // Yes or No, defaulting to No. Saying yes costs a teardown through pkexec and a new payload, so a
    // dialog dismissed by the keyboard must not be read as consent on the user's behalf.
    //
    // Built as an object rather than through the static because the two profile names belong under the
    // question rather than inside it: the sentence is the catalog's wording in seven languages, and
    // which rows it means is data that must not be threaded through every one of them.
    QMessageBox box(QMessageBox::Question,
                    LanguageManager::tr(QStringLiteral("watchdog.title")),
                    LanguageManager::tr(QStringLiteral("watchdog.question")),
                    QMessageBox::Yes | QMessageBox::No, this);
    box.setInformativeText(QStringLiteral("%1 \u2192 %2").arg(fromName, nextName));
    box.setDefaultButton(QMessageBox::No);
    return box.exec() == QMessageBox::Yes;
}

void MainWindow::performWatchdogRotation(const QString &fromId, const QString &nextId)
{
    Q_UNUSED(fromId);
    // Queued before the stop, because the stop is the part that takes time: a portable teardown runs a
    // pkexec iptables flush and costs seconds, during which the backend accepts no start at all (see the
    // gating note on canStartFrom). onStateChanged() picks the id up when the run is reported down.
    m_watchdogRotationId = nextId;

    if (canStopFrom(m_manager->state()))
        onStopRequested();   // the toolbar's own path, so the button and the journal both say "stopping"
    // A teardown that finished inside that call, and a run that was already down when the answer came,
    // both leave the backend free now. Otherwise this is the no-op that waits for the state change.
    continueWatchdogRotation();
}

void MainWindow::continueWatchdogRotation()
{
    if (m_watchdogRotationId.isEmpty())
        return;
    if (!canStartFrom(m_manager->state()))
        return;   // still coming down; the next Stopped/Error reports it and calls this again

    const QString id = m_watchdogRotationId;
    m_watchdogRotationId.clear();
    // The next turn of the loop, never this one. continueWatchdogRotation() is reached from inside
    // stateChanged, and on the direct-payload path that signal is emitted from within the backend's own
    // stop(): a start() run under that frame would let the rest of the teardown write over the state the
    // new launch just set. Queued costs nothing (the backend is already idle) and cannot re-enter.
    //
    // interactive: the user answered a dialog to get here, so a refusal that landed in silence would be
    // the window taking back the choice it just offered. startStrategy() dialogs, badges the row and
    // writes the selection through Settings, exactly as a press of the power button would.
    QTimer::singleShot(0, this, [this, id] { startStrategy(id, true); });
}

void MainWindow::onStrategiesLoaded(int count)
{
    populateCards();
    Q_UNUSED(count);
}

void MainWindow::onSettingsChanged()
{
    if (m_applyingSettings)
        return;

    const QString path = m_settings->zapretPath();
    if (path != m_manager->zapretDir())
        m_manager->setZapretDir(path);   // reload() -> onStrategiesLoaded -> populateCards()

    applyTheme();
    // The motion switch, and the one channel that carries it. Setting it before the widgets below are
    // re-read is what makes an untick take effect on a pulse that is running right now rather than on
    // the next state change: Motion::setEnabled lands everything in flight, and updateStatusUi() at the
    // end of this function re-decides what should be running.
    Motion::setEnabled(m_settings->animationsEnabled());
    m_ping->setIntervalSec(m_settings->pingIntervalSec());
    // The watchdog's cadence is the other number the page can move, and this is the only channel that
    // tells the window about it. startWatchdog() ignores a write that did not change the interval, so
    // the last-strategy id a start persists cannot push the next check five minutes further out.
    startWatchdog();
    // The footer's standing line states the cadence, so it is the settings dialog's business too.
    m_footerIdleText = pingCadenceText(m_settings->pingIntervalSec());
    if (m_footerLabel && !m_footerTimer->isActive())
        m_footerLabel->setText(m_footerIdleText);
    updateStatusUi();
}

void MainWindow::requestQuit()
{
    m_quitting = true;
    if (m_manager->isRunning() && canStopFrom(m_manager->state()))
        m_manager->stop();
    hide();
    qApp->quit();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    const bool trayUp = m_tray && m_tray->isVisible();
    if (!m_quitting && hideOnClose(m_settings->minimizeToTray(), trayUp)) {
        hide();
        event->ignore();
        m_tray->showMessage(QStringLiteral("Muffin"),
                            LanguageManager::tr(QStringLiteral("tray.minimized")),
                            Theme::icon(QStringLiteral("tray"), Theme::success(), 22),
                            4000);
        return;
    }

    // The user asked for hide-on-close but this desktop publishes no tray. Say what is about to happen
    // instead of silently quitting — the difference matters when a strategy is running.
    if (!m_quitting && m_settings->minimizeToTray() && !trayUp && m_manager->isRunning()) {
        m_log->appendLine(LanguageManager::tr(QStringLiteral("log.close_stops")), LogPanel::Warn);
    }

    m_quitting = true;
    // Same gate as the explicit quit: a teardown already in flight must not be asked for a second time
    // on the way out, where the direct-payload path would block the close in waitForFinished().
    if (m_manager->isRunning() && canStopFrom(m_manager->state()))
        m_manager->stop();
    event->accept();
}

// ---- frameless-window drag ----------------------------------------------------
//
// The window carries no system frame, so the only thing it has to be grabbed by is its own header. These
// three handlers turn a left-press on the header band into a move of the whole window. Everything else —
// a press on a page, a card, a button — is not in the header band and falls through to the ordinary
// widget behaviour, so the drag never steals a click from a control that lives below the title line.

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    // The header bar occupies the top kHeaderDragHeight px of the window, spanning the full width minus
    // nothing: the two chrome buttons are children that consume their own presses, so a press that reaches
    // MainWindow here is on bare header space. Anywhere else in that band, start dragging.
    if (event->button() == Qt::LeftButton && event->position().y() <= kHeaderDragHeight) {
        m_dragging = true;
        m_dragOffset = event->position().toPoint();
        event->accept();
        return;
    }
    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        // The window is frameless and fixed-size, so this is the only thing that moves it. mapToGlobal on
        // the central widget would fight the rail offset; frameGeometry() is the window's own top-left in
        // global space, and subtracting the press offset from the pointer's global position keeps the grab
        // point under the cursor exactly.
        move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }
    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging) {
        m_dragging = false;
        event->accept();
        return;
    }
    QMainWindow::mouseReleaseEvent(event);
}


} // namespace zapret
