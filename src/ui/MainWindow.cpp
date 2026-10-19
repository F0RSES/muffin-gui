#include "MainWindow.h"

#include "DownloadDialog.h"
#include "LogPanel.h"
#include "PingIndicator.h"
#include "PingService.h"
#include "PowerButton.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "StrategyCard.h"
#include "Theme.h"
#include "ZapretManager.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTimer>
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
// is how the two drift.
const QString kStopActionHint = QStringLiteral("Остановить запущенную стратегию (Ctrl+Shift+X)");

// The power button's other standing tooltip — what it says when there is nothing to stop.
const QString startActionHint = QStringLiteral("Запустить выбранную стратегию (Ctrl+Shift+S)");

// The rail is fixed, not resizable: it carries glyphs only, and 70 px around a 24 px icon is the width
// the mock specifies and the one that never needs to change.
constexpr int kSidebarWidth = 70;
constexpr int kRailGlyphPx = 24;
constexpr int kBrandGlyphPx = 28;

// The dashboard's circle. 200 px of rim, and PowerButton adds its own halo margin on every side, so the
// widget is taller than this — which is why the spacing below is measured from the widget, not the ring.
constexpr int kPowerDiameterPx = 200;
// Button to ping badges, as specified.
constexpr int kPowerToPingsGap = 60;
// Badge to badge.
constexpr int kPingSpacing = 50;
// The header's connected dot: 6 px across, so a 3 px radius.
constexpr int kStatusDotPx = 6;

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
    m_manager = new ZapretManager(this);
    // Parentless by design: PingService lives on its own thread, and parenting it here would move it
    // back to the GUI thread. It is deleted explicitly in the destructor.
    m_ping = new PingService();

    setWindowTitle(QStringLiteral("Muffin"));
    setMinimumSize(900, 640);
    setWindowIcon(Theme::icon(QStringLiteral("app"), Theme::accent(), 32));

    buildCentral();
    const bool trayUp = ensureTray();
    if (!trayUp)
        startTrayRetry();
    buildShortcuts();
    wire();

    m_ping->setHosts(PingService::defaultHosts());
    m_ping->setIntervalSec(m_settings->pingIntervalSec());
    m_ping->start();

    m_manager->setZapretDir(m_settings->zapretPath());
    updateStatusUi();

    // A short startup banner: it proves the manager -> log wiring end to end and leaves the panel
    // explaining itself instead of showing an empty box.
    m_log->appendLine(QStringLiteral("Muffin %1 запущен (бэкенд: %2)")
                          .arg(QApplication::applicationVersion(), m_manager->backendName()),
                      LogPanel::Info);
    if (m_manager->strategyCount() > 0) {
        m_log->appendLine(QStringLiteral("Найдено стратегий: %1 в %2")
                              .arg(m_manager->strategyCount())
                              .arg(QDir::toNativeSeparators(m_manager->resolvedRoot())),
                          LogPanel::Info);
    } else {
        m_log->appendLine(QStringLiteral("Стратегии не найдены — укажите путь к zapret в настройках."),
                          LogPanel::Warn);
    }

    // Closing the window can only mean "hide" if there is a tray to hide into. Say so up front instead
    // of letting the user discover it by pressing X with a strategy running.
    if (!trayUp && m_settings->minimizeToTray()) {
        m_trayWarned = true;
        m_log->appendLine(QStringLiteral(
                              "Трей недоступен в этой среде: закрытие окна остановит zapret "
                              "и выйдет из приложения."),
                          LogPanel::Warn);
        showTransientMessage(QStringLiteral("Трей недоступен — закрытие окна завершит работу"), 8000);
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

// The rail: 70 px of glyphs. Brand at the head, the journal above the middle, settings and the version
// pinned to the foot. No labels anywhere — the whole argument of the mock is that three icons need none.
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

    box->addStretch(3);

    // The journal. Checkable so it can say "you are looking at it", and a second press puts the
    // dashboard back — with one glyph on the rail it has to work both ways.
    m_navLogs = addRailButton(box, QStringLiteral("logs"), QStringLiteral("Логи"));
    m_navLogs->setCheckable(true);
    connect(m_navLogs, &QPushButton::clicked, this, [this] {
        setCurrentPage(currentPage() == PageLogs ? PageDashboard : PageLogs);
    });

    box->addStretch(6);

    // Below the fold: settings, then the version. Настройки stays a modal sheet rather than a page — it
    // is a commit, not a view you glance at, and SettingsDialog::apply() is written as a dialog accept.
    QPushButton *navSettings = addRailButton(box, QStringLiteral("settings"),
                                             QStringLiteral("Настройки"));
    navSettings->setToolTip(QStringLiteral("Настройки (Ctrl+S)"));
    connect(navSettings, &QPushButton::clicked, this, &MainWindow::openSettings);

    auto *version = new QLabel(QStringLiteral("v%1").arg(QApplication::applicationVersion()), bar);
    version->setObjectName(QStringLiteral("railVersion"));
    version->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    box->addWidget(version);

    return bar;
}

QPushButton *MainWindow::addRailButton(QVBoxLayout *box, const QString &glyph, const QString &text)
{
    auto *b = new QPushButton(m_sidebar);
    b->setObjectName(QStringLiteral("navButton"));
    b->setProperty("glyph", glyph);
    b->setProperty("glyphPx", kRailGlyphPx);
    // The button shows a glyph and nothing else, so the name it needs for assistive tech — and the one
    // the tooltip repeats — has to be said out loud rather than read off a label.
    b->setAccessibleName(text);
    b->setToolTip(text);
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
    m_pages->addWidget(buildStrategiesPage());
    m_pages->addWidget(buildLogsPage());
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

    auto *title = new QLabel(QStringLiteral("Muffin"), header);
    title->setObjectName(QStringLiteral("appTitle"));

    // The mock's "/ network utility": quiet, monospace, and the only thing that has to say what kind of
    // program this is.
    auto *subtitle = new QLabel(QStringLiteral("/ network utility"), header);
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPixelSize(11);
    subtitle->setFont(mono);

    m_statusDot = new QWidget(header);
    m_statusDot->setObjectName(QStringLiteral("statusDot"));
    m_statusDot->setFixedSize(kStatusDotPx, kStatusDotPx);
    // A bare QWidget paints no stylesheet background of its own; this attribute is what lets #statusDot's
    // border-radius round anything at all.
    m_statusDot->setAttribute(Qt::WA_StyledBackground, true);
    m_statusDot->setToolTip(QStringLiteral("Состояние подключения"));

    m_statusLabel = new QLabel(header);
    m_statusLabel->setObjectName(QStringLiteral("statusText"));
    m_statusLabel->setProperty("statusKind", statusKind(IZapretBackend::State::Stopped));

    row->addWidget(title);
    row->addWidget(subtitle);
    row->addStretch(1);
    row->addWidget(m_statusDot, 0, Qt::AlignVCenter);
    row->addWidget(m_statusLabel, 0, Qt::AlignVCenter);
    return header;
}

QWidget *MainWindow::buildFooter()
{
    auto *foot = new QFrame(m_mainArea);
    foot->setObjectName(QStringLiteral("footerBar"));

    auto *grid = new QGridLayout(foot);
    grid->setContentsMargins(28, 8, 28, 8);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(0);

    m_dirLabel = new QLabel(foot);
    m_dirLabel->setObjectName(QStringLiteral("dirLabel"));
    m_dirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_footerLabel = new QLabel(foot);
    m_footerLabel->setObjectName(QStringLiteral("footerHint"));
    m_footerLabel->setAlignment(Qt::AlignCenter);

    // Three columns with the outer two weighted equally, so the centred line stays centred however long
    // the path beside it grows — which a two-column row cannot promise.
    grid->addWidget(m_dirLabel, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
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

// The dashboard: a route pill top-left, one enormous button in the middle, and the reachability badges
// under it. Nothing else — the point of the design is that the common case (run the thing you ran last
// time) never leaves this screen.
QWidget *MainWindow::buildDashboardPage()
{
    auto *page = new QWidget(m_pages);
    m_dashboardPage = page;
    page->setObjectName(QStringLiteral("dashboardPage"));

    auto *box = new QVBoxLayout(page);
    box->setContentsMargins(28, 22, 28, 22);
    box->setSpacing(0);

    // Where the mock puts its "Smart route" line. The pill names the strategy the power button will run
    // and opens the list to change it, so "what am I about to run" and "let me pick another" are one
    // control — which is also what lets the cards keep existing without taking a rail glyph.
    m_routePill = new QPushButton(page);
    m_routePill->setObjectName(QStringLiteral("routePill"));
    m_routePill->setCursor(Qt::PointingHandCursor);
    m_routePill->setFocusPolicy(Qt::TabFocus);
    m_routePill->setAutoDefault(false);
    connect(m_routePill, &QPushButton::clicked, this, [this] { setCurrentPage(PageStrategies); });
    box->addWidget(m_routePill, 0, Qt::AlignLeft | Qt::AlignTop);

    // Unequal stretches either side of the button: with the badges, the footer and the pill all in the
    // same column, equal stretches would push the circle visibly above the middle of the canvas.
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

// The catalog. Reached from the route pill, with its own way back, and carrying the two actions the
// retired toolbar used to hold — rescan the folder, and fetch zapret when the folder is empty.
QWidget *MainWindow::buildStrategiesPage()
{
    auto *page = new QWidget(m_pages);
    m_strategiesPage = page;
    page->setObjectName(QStringLiteral("strategiesPage"));

    auto *box = new QVBoxLayout(page);
    box->setContentsMargins(28, 22, 28, 22);
    box->setSpacing(12);

    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(10);

    auto *back = new QPushButton(QStringLiteral("←  Muffin"), page);
    back->setObjectName(QStringLiteral("ghostButton"));
    back->setCursor(Qt::PointingHandCursor);
    back->setAutoDefault(false);
    back->setToolTip(QStringLiteral("Вернуться на главный экран (Esc)"));
    connect(back, &QPushButton::clicked, this, &MainWindow::showDashboard);

    auto *title = new QLabel(QStringLiteral("Стратегии"), page);
    title->setObjectName(QStringLiteral("pageTitle"));

    auto *refresh = new QPushButton(Theme::themedIcon(QStringLiteral("refresh"), 16),
                                    QStringLiteral("Обновить"), page);
    refresh->setObjectName(QStringLiteral("ghostButton"));
    refresh->setProperty("glyph", QStringLiteral("refresh"));
    refresh->setProperty("glyphPx", 16);
    refresh->setToolTip(QStringLiteral("Пересканировать папку zapret (Ctrl+R)"));
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshStrategies);

    auto *download = new QPushButton(Theme::themedIcon(QStringLiteral("arrow-down"), 16),
                                     QStringLiteral("Скачать zapret"), page);
    download->setObjectName(QStringLiteral("ghostButton"));
    download->setProperty("glyph", QStringLiteral("arrow-down"));
    download->setProperty("glyphPx", 16);
    download->setToolTip(QStringLiteral("Загрузить последнюю версию zapret с GitHub"));
    connect(download, &QPushButton::clicked, this, &MainWindow::downloadZapret);

    head->addWidget(back);
    head->addSpacing(6);
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(refresh);
    head->addWidget(download);
    box->addLayout(head);

    auto *subtitle = new QLabel(QStringLiteral(
        "Одна стратегия за раз. Правый клик по карточке — скопировать команду."), page);
    subtitle->setObjectName(QStringLiteral("pageSubtitle"));
    box->addWidget(subtitle);

    // m_stack is still what it always was — the cards list or the "nothing found" page.
    m_stack = new QStackedWidget(page);

    m_scroll = new QScrollArea(m_stack);
    m_scroll->setObjectName(QStringLiteral("cardScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_cardHost = new QWidget(m_scroll);
    m_cardLayout = new QVBoxLayout(m_cardHost);
    m_cardLayout->setContentsMargins(12, 12, 12, 12);
    m_cardLayout->setSpacing(10);
    m_cardLayout->addStretch(1);
    m_scroll->setWidget(m_cardHost);

    m_placeholder = new QWidget(m_stack);
    m_placeholder->setObjectName(QStringLiteral("placeholderPage"));

    // The placeholder used to be one label ending in "укажите путь в настройках" — instructions for
    // someone who has no zapret and no way to get it from here. Both ways out are now buttons.
    auto *placeholderText = new QLabel(QStringLiteral(
        "Стратегии не найдены.\n\n"
        "Скачайте zapret сами — или укажите папку, которая уже есть на диске."), m_placeholder);
    placeholderText->setObjectName(QStringLiteral("placeholder"));
    placeholderText->setAlignment(Qt::AlignCenter);
    placeholderText->setWordWrap(true);

    auto *placeholderDownload = new QPushButton(QStringLiteral("Скачать zapret"), m_placeholder);
    placeholderDownload->setObjectName(QStringLiteral("primaryButton"));
    auto *placeholderSettings = new QPushButton(QStringLiteral("Настройки (Ctrl+S)"), m_placeholder);

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

// The journal gets the whole canvas, which is what a 5000-line ring buffer of aligned timestamps
// actually wants. Filtering, copying and saving are LogPanel's own.
QWidget *MainWindow::buildLogsPage()
{
    auto *page = new QWidget(m_pages);
    m_logsPage = page;
    page->setObjectName(QStringLiteral("logsPage"));

    auto *box = new QVBoxLayout(page);
    box->setContentsMargins(28, 22, 28, 22);
    box->setSpacing(12);

    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(10);

    auto *back = new QPushButton(QStringLiteral("←  Muffin"), page);
    back->setObjectName(QStringLiteral("ghostButton"));
    back->setCursor(Qt::PointingHandCursor);
    back->setAutoDefault(false);
    back->setToolTip(QStringLiteral("Вернуться на главный экран (Esc)"));
    connect(back, &QPushButton::clicked, this, &MainWindow::showDashboard);

    auto *title = new QLabel(QStringLiteral("Журнал"), page);
    title->setObjectName(QStringLiteral("pageTitle"));
    head->addWidget(back);
    head->addSpacing(6);
    head->addWidget(title);
    head->addStretch(1);
    box->addLayout(head);

    auto *subtitle = new QLabel(QStringLiteral(
        "Всё, что сообщает бэкенд: уровень, время, строка."), page);
    subtitle->setObjectName(QStringLiteral("pageSubtitle"));
    box->addWidget(subtitle);

    m_log = new LogPanel(page);
    box->addWidget(m_log, 1);
    return page;
}

int MainWindow::currentPage() const
{
    return m_pages ? m_pages->currentIndex() : PageDashboard;
}

void MainWindow::setCurrentPage(int page)
{
    if (!m_pages)
        return;
    const int target = (page == PageLogs)   ? PageLogs
                       : (page == PageStrategies) ? PageStrategies
                                                  : PageDashboard;
    m_pages->setCurrentIndex(target);

    // The rail's glyph is the only page indicator left, so it has to follow the stack even when the page
    // was chosen from code — a lit journal icon over the dashboard is worse than no icon at all.
    if (m_navLogs)
        m_navLogs->setChecked(target == PageLogs);
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

    const QString last = m_settings->lastStrategyId();
    if (last.isEmpty() || !m_manager->findStrategy(last)) {
        // Nothing to run yet. Saying "no saved strategy" where the user can see the list they have not
        // opened is more useful than a sentence in a footer nobody reads.
        setCurrentPage(PageStrategies);
        showTransientMessage(QStringLiteral("Выберите стратегию, затем нажмите кнопку"), 5000);
        return;
    }
    startStrategy(last, true);
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
    QAction *show = menu->addAction(QStringLiteral("Показать"));
    connect(show, &QAction::triggered, this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    QAction *startLast = menu->addAction(QStringLiteral("Запустить последнюю"));
    connect(startLast, &QAction::triggered, this, [this] {
        const QString last = m_settings->lastStrategyId();
        if (!last.isEmpty())
            startStrategy(last, true);
    });
    QAction *stop = menu->addAction(QStringLiteral("Остановить"));
    connect(stop, &QAction::triggered, this, &MainWindow::onStopRequested);
    menu->addSeparator();
    QAction *quit = menu->addAction(QStringLiteral("Выход"));
    connect(quit, &QAction::triggered, this, &MainWindow::requestQuit);

    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                    if (isVisible() && !isMinimized()) {
                        hide();
                    } else {
                        showNormal();
                        raise();
                        activateWindow();
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
                m_log->appendLine(QStringLiteral("Трей доступен — сворачивание при закрытии включено."),
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
            showTransientMessage(QStringLiteral("Нет сохранённой стратегии для запуска"), 3000);
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
    m_errorCardId.clear();   // the new set of cards has not failed at anything yet

    const QList<Strategy> list = m_manager->strategies();
    for (const Strategy &s : list) {
        auto *card = new StrategyCard(s, m_cardHost);
        const QString id = s.id.isEmpty() ? s.filePath : s.id;
        connect(card, &StrategyCard::startRequested, this, &MainWindow::onStartRequested);
        connect(card, &StrategyCard::stopRequested, this, &MainWindow::onStopRequested);
        m_cardLayout->addWidget(card);
        m_cards.insert(id, card);
    }
    m_cardLayout->addStretch(1);

    m_stack->setCurrentWidget(list.isEmpty() ? m_placeholder : m_scroll);
    updateStatusUi();
}

QString MainWindow::statusText(IZapretBackend::State state, const QString &strategyName)
{
    switch (state) {
    case IZapretBackend::State::Starting:
        return QStringLiteral("Запуск: %1").arg(strategyName.isEmpty() ? QStringLiteral("…") : strategyName);
    case IZapretBackend::State::Running:
        return QStringLiteral("Работает: %1").arg(strategyName.isEmpty() ? QStringLiteral("—") : strategyName);
    case IZapretBackend::State::Stopping:
        return QStringLiteral("Остановка…");
    case IZapretBackend::State::Error:
        return QStringLiteral("Ошибка");
    case IZapretBackend::State::Stopped:
        break;
    }
    return QStringLiteral("Остановлено");
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
    // Stopping is not a second stop waiting to happen: re-issuing one re-ran "terminating nfqws" ten
    // seconds after the first, and in the direct-payload path it blocks the GUI in
    // waitForFinished(3000) for a death that is already under way.
    return false;
}

QString MainWindow::blockedReason(IZapretBackend::State state)
{
    switch (state) {
    case IZapretBackend::State::Starting:
        return QStringLiteral("Идёт запуск — дождитесь его завершения.");
    case IZapretBackend::State::Stopping:
        return QStringLiteral("Идёт остановка — дождитесь её завершения.");
    case IZapretBackend::State::Running:
        // The missing action here is not another strategy's start but this run's stop.
        return QStringLiteral("Стратегия уже запущена — сначала остановите её.");
    case IZapretBackend::State::Stopped:
    case IZapretBackend::State::Error:
        break;
    }
    return QString();
}

void MainWindow::updateStatusUi()
{
    const IZapretBackend::State state = m_manager->state();
    const Strategy *active = m_manager->activeStrategy();
    const QString name = active ? active->name : QString();

    if (m_statusLabel) {
        // The chip's word is the plain state, not the sentence the old pill carried: "CONNECTED" beside a
        // dot, with the strategy's name reserved for the route pill and the window title, which both say
        // it more precisely.
        setConnectionChip(connectionText(state), statusKind(state));
    }

    // While the teardown runs the title has to say so rather than keep naming the strategy: alt-tabbing
    // back to "Muffin — nfqws (portable)" is indistinguishable from a live run, and that ten-second gap
    // is exactly when the second click used to land.
    QString titleName = QStringLiteral("остановлено");
    if (state == IZapretBackend::State::Stopping)
        titleName = QStringLiteral("остановка…");
    else if (!name.isEmpty() && state != IZapretBackend::State::Stopped
             && state != IZapretBackend::State::Error)
        titleName = name;
    setWindowTitle(QStringLiteral("Muffin — %1").arg(titleName));

    if (m_dirLabel) {
        const QString dir = m_manager->resolvedRoot().isEmpty()
                                ? QStringLiteral("zapret не найден")
                                : QDir::toNativeSeparators(m_manager->resolvedRoot());
        m_dirLabel->setText(dir);
        m_dirLabel->setToolTip(dir);
    }

    // "The run this card belongs to" is shown from the moment it is requested until the teardown
    // finishes: the manager still names the strategy through both in-flight phases, and a card that
    // silently fell back to "Запустить" mid-teardown would offer the very action the backend refuses.
    const bool activeIsUp = m_manager->isRunning() || state == IZapretBackend::State::Starting
                            || state == IZapretBackend::State::Stopping;
    const bool canStart = canStartFrom(state);
    const bool canStop = canStopFrom(state);
    const QString busy = blockedReason(state);

    // The power button is the window's only start/stop control now, so all the gating the toolbar action
    // and the rail's power glyph used to share lands here: which look it wears, and what it says it is
    // waiting for when a press would be refused.
    if (m_power) {
        PowerButton::Mode mode = PowerButton::Mode::Idle;
        if (state == IZapretBackend::State::Running)
            mode = PowerButton::Mode::Active;
        else if (state == IZapretBackend::State::Starting || state == IZapretBackend::State::Stopping)
            mode = PowerButton::Mode::Busy;
        m_power->setMode(mode);
        m_power->setEnabled(canStart || canStop);
        m_power->setToolTip(canStop ? kStopActionHint
                                    : (busy.isEmpty() ? startActionHint : busy));
    }

    if (m_routePill) {
        // The pill names what the button will do: the live strategy while one is up, otherwise the one
        // saved from the last run, otherwise the invitation to go pick one.
        const QString last = m_settings->lastStrategyId();
        const Strategy *chosen = activeIsUp ? active : m_manager->findStrategy(last);
        const QString chosenName = chosen ? chosen->name
                                          : (last.isEmpty() ? QString() : last);
        m_routePill->setText(chosenName.isEmpty()
                                 ? QStringLiteral("Smart route — выбрать стратегию")
                                 : QStringLiteral("Smart route · %1").arg(chosenName));
        m_routePill->setToolTip(QStringLiteral("Изменить стратегию"));
    }

    const QString activeId = m_manager->activeStrategyId();
    for (auto it = m_cards.cbegin(); it != m_cards.cend(); ++it) {
        const bool isActive = activeIsUp && it.key() == activeId;
        it.value()->setRunning(isActive);
        // One card offers exactly one action — stop when it owns the run, start otherwise — so the
        // gating is per card, not per window: the active card is greyed only when a stop would be a
        // duplicate, the others whenever a start would be refused.
        it.value()->setActionEnabled(isActive ? canStop : canStart, busy);
    }

    updateTrayIcon();
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
    // covers the rail's 24 px icons and the strategies page's 16 px ones alike.
    const auto buttons = findChildren<QPushButton *>();
    for (QPushButton *b : buttons) {
        const QString glyph = b->property("glyph").toString();
        if (glyph.isEmpty())
            continue;
        const int px = b->property("glyphPx").toInt();
        b->setIcon(Theme::themedIcon(glyph, px > 0 ? px : 16));
    }
    if (QLabel *logo = findChild<QLabel *>(QStringLiteral("brandLogo")))
        logo->setPixmap(Theme::icon(QStringLiteral("app"), Theme::accent(), kBrandGlyphPx)
                            .pixmap(kBrandGlyphPx, kBrandGlyphPx));

    // The power ring is painted from Theme::accent()/pingOk() rather than from the sheet, so it needs
    // telling that the palette moved.
    if (m_power)
        m_power->retheme();

    setWindowIcon(Theme::icon(QStringLiteral("app"), Theme::accent(), 32));
    updateTrayIcon();
}

void MainWindow::refreshStrategies()
{
    const int count = m_manager->reload();
    populateCards();
    showTransientMessage(QStringLiteral("Найдено стратегий: %1").arg(count), 4000);
    m_log->appendLine(QStringLiteral("Пересканировано: %1 стратегий в %2")
                          .arg(count)
                          .arg(m_manager->resolvedRoot().isEmpty() ? QStringLiteral("(не найдено)")
                                                                   : m_manager->resolvedRoot()),
                      LogPanel::Info);
    if (count == 0)
        m_log->appendLine(QStringLiteral("В этой папке нет стратегий — проверьте путь в настройках."),
                          LogPanel::Warn);
}

void MainWindow::openSettings()
{
    SettingsDialog dialog(*m_settings, this);
    dialog.exec();
    // SettingsDialog::apply() already wrote through Settings, which fired changed() and reloaded.
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
    m_log->appendLine(QStringLiteral("Установлен zapret: %1").arg(QDir::toNativeSeparators(path)),
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
    // Asked before the backend sees the request. Cards and toolbar are greyed in exactly these states,
    // but the tray menu, Ctrl+Shift+S and the auto-start on launch are not — and unguarded they reached
    // LinuxBackend::start() mid-teardown, which reported "an operation is already in progress (stop
    // first)" as a failure and badged the card that was merely clicked too early. Waiting is not an
    // error, so it gets a warning line and a status-bar message instead of a dialog.
    const QString busy = blockedReason(m_manager->state());
    if (!busy.isEmpty()) {
        m_log->appendLine(busy, LogPanel::Warn);
        showTransientMessage(busy, 5000);
        return;
    }

    const Strategy *strategy = m_manager->findStrategy(id);
    if (!strategy) {
        const QString msg = QStringLiteral("Стратегия не найдена: %1").arg(id);
        m_log->appendLine(msg, LogPanel::Error);
        notifyError(msg, interactive);
        return;
    }

    m_pendingStartId = id;
    QString error;
    if (!m_manager->start(*strategy, &error)) {
        const QString msg = error.isEmpty() ? QStringLiteral("Бэкенд отклонил запуск.") : error;
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

    m_applyingSettings = true;
    m_settings->setLastStrategyId(strategy->id.isEmpty() ? strategy->filePath : strategy->id);
    m_applyingSettings = false;
}

void MainWindow::onStartRequested(const QString &id)
{
    startStrategy(id, true);
}

void MainWindow::onStopRequested()
{
    // Reached from the toolbar action, the card button, the tray menu and Ctrl+Shift+X. The first two
    // are gated in updateStatusUi(), so this guard is what keeps the two that are not from re-issuing a
    // stop that is already running — the second "terminating nfqws" ten seconds after the first.
    if (!canStopFrom(m_manager->state()))
        return;
    m_manager->stop();
}

void MainWindow::onStateChanged(IZapretBackend::State state)
{
    // Once the attempt is over — up or down — later errors belong to whatever is active, not to a
    // pending start that already resolved.
    if (state == IZapretBackend::State::Running || state == IZapretBackend::State::Stopped)
        m_pendingStartId.clear();
    // Capture the run's card while the manager still names it. It forwards stateChanged before its own
    // bookkeeping slot clears m_activeId, so this is the last moment the two agree — and a portable
    // failure that deferred its report to after the nfqueue teardown arrives with nothing to attribute
    // to otherwise (see errorTargetId). A run that came down cleanly leaves no grievance behind.
    if (state == IZapretBackend::State::Stopped)
        m_lastRunId.clear();
    else if (const QString id = m_manager->activeStrategyId(); !id.isEmpty())
        m_lastRunId = id;
    updateStatusUi();
    if (state == IZapretBackend::State::Stopped && m_tray && !isVisible())
        updateTrayIcon();
}

void MainWindow::onStrategyStarted(const QString &id, const QString &name)
{
    m_log->appendLine(QStringLiteral("Запрошен запуск: %1").arg(name), LogPanel::Info);
    attachErrorBadge(id, QString());   // a fresh attempt supersedes the previous failure badge
    updateStatusUi();
}

void MainWindow::onStrategyStopped()
{
    m_log->appendLine(QStringLiteral("Стратегия остановлена"), LogPanel::Info);
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
    StrategyCard *card = m_cards.value(id, nullptr);
    if (!card)
        return;
    card->setError(message);   // an empty message clears the badge
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
    auto it = m_indicators.constFind(host);
    if (it == m_indicators.constEnd())
        return;
    it.value()->setStatus(PingIndicator::statusFromServiceInt(status), latencyMs);
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
    m_ping->setIntervalSec(m_settings->pingIntervalSec());
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
                            QStringLiteral("Окно свёрнуто в трей — zapret продолжает работать.\n"
                                           "Двойной клик по иконке открывает окно."),
                            Theme::icon(QStringLiteral("tray"), Theme::success(), 22),
                            4000);
        return;
    }

    // The user asked for hide-on-close but this desktop publishes no tray. Say what is about to happen
    // instead of silently quitting — the difference matters when a strategy is running.
    if (!m_quitting && m_settings->minimizeToTray() && !trayUp && m_manager->isRunning()) {
        m_log->appendLine(QStringLiteral(
                              "Трей недоступен: закрытие окна останавливает zapret и завершает приложение."),
                          LogPanel::Warn);
    }

    m_quitting = true;
    // Same gate as the explicit quit: a teardown already in flight must not be asked for a second time
    // on the way out, where the direct-payload path would block the close in waitForFinished().
    if (m_manager->isRunning() && canStopFrom(m_manager->state()))
        m_manager->stop();
    event->accept();
}


} // namespace zapret
