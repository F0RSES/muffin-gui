#include "MainWindow.h"

#include "DownloadDialog.h"
#include "LogPanel.h"
#include "PingIndicator.h"
#include "PingService.h"
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
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolBar>
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
    setMinimumSize(1100, 700);
    setWindowIcon(Theme::icon(QStringLiteral("app"), Theme::accent(), 32));

    buildCentral();
    buildToolBar();
    buildStatusBar();
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
        statusBar()->showMessage(QStringLiteral("Трей недоступен — закрытие окна завершит работу"), 8000);
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
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("mainSplit"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(3);

    m_stack = new QStackedWidget(m_splitter);

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

    // The placeholder used to be one label that ended in "укажите путь в настройках" — instructions for
    // someone who has no zapret yet and no way to get it from here. Both ways out are now buttons:
    // pointing at a folder that does not exist is not a fix, downloading it is.
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
    m_stack->setMinimumWidth(400);

    m_log = new LogPanel(m_splitter);
    m_log->setMinimumWidth(400);

    m_splitter->addWidget(m_stack);
    m_splitter->addWidget(m_log);
    m_splitter->setStretchFactor(0, 2);
    m_splitter->setStretchFactor(1, 3);
    m_splitter->setSizes({520, 640});

    setCentralWidget(m_splitter);
}

void MainWindow::buildToolBar()
{
    m_toolBar = addToolBar(QStringLiteral("Главная"));
    m_toolBar->setObjectName(QStringLiteral("mainToolBar"));
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    QAction *refresh = m_toolBar->addAction(Theme::themedIcon(QStringLiteral("refresh"), 16),
                                            QStringLiteral("Обновить"));
    refresh->setToolTip(QStringLiteral("Пересканировать папку zapret (Ctrl+R)"));
    connect(refresh, &QAction::triggered, this, &MainWindow::refreshStrategies);

    // Between "Обновить" (look at what is here) and "Настройки" (change where to look): when there is
    // nothing here yet, downloading is the action that actually helps, and it has to be one click away
    // rather than buried in the settings sheet.
    QAction *download = m_toolBar->addAction(Theme::themedIcon(QStringLiteral("arrow-down"), 16),
                                             QStringLiteral("Скачать zapret"));
    download->setToolTip(QStringLiteral("Загрузить последнюю версию zapret с GitHub"));
    connect(download, &QAction::triggered, this, &MainWindow::downloadZapret);

    QAction *settings = m_toolBar->addAction(Theme::themedIcon(QStringLiteral("settings"), 16),
                                             QStringLiteral("Настройки"));
    settings->setToolTip(QStringLiteral("Настройки (Ctrl+S)"));
    connect(settings, &QAction::triggered, this, &MainWindow::openSettings);

    m_stopAction = m_toolBar->addAction(Theme::themedIcon(QStringLiteral("stop"), 16),
                                        QStringLiteral("Остановить"));
    m_stopAction->setToolTip(kStopActionHint);
    connect(m_stopAction, &QAction::triggered, this, &MainWindow::onStopRequested);

    auto *spacer = new QWidget(m_toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBar->addWidget(spacer);

    m_statusLabel = new QLabel(m_toolBar);
    m_statusLabel->setObjectName(QStringLiteral("statusLabel"));
    m_toolBar->addWidget(m_statusLabel);
}

void MainWindow::buildStatusBar()
{
    // Three chips + the resolved zapret directory, exactly as laid out in the spec.
    const QStringList hosts = PingService::defaultHosts();
    for (const QString &host : hosts) {
        auto *chip = new PingIndicator(friendlyHostName(host), this);
        chip->setToolTip(host);
        m_indicators.insert(host, chip);
        statusBar()->addWidget(chip, 0);
    }

    m_dirLabel = new QLabel(this);
    m_dirLabel->setObjectName(QStringLiteral("dirLabel"));
    statusBar()->addPermanentWidget(m_dirLabel);
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
            statusBar()->showMessage(QStringLiteral("Нет сохранённой стратегии для запуска"), 3000);
    });

    auto *stop = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+X")), this);
    connect(stop, &QShortcut::activated, this, &MainWindow::onStopRequested);
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
        m_statusLabel->setText(statusText(state, name));
        m_statusLabel->setProperty("statusKind", statusKind(state));
        m_statusLabel->style()->unpolish(m_statusLabel);
        m_statusLabel->style()->polish(m_statusLabel);
        m_statusLabel->update();
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

    if (m_stopAction) {
        m_stopAction->setEnabled(canStop);
        // Idle states get the ordinary hint back; only a busy one gets a reason, so a disabled action
        // never claims to be waiting for something that already finished.
        m_stopAction->setToolTip(canStop || busy.isEmpty() ? kStopActionHint : busy);
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

    // Toolbar icons are baked at load time, so they must be re-tinted after a theme switch.
    if (m_toolBar) {
        const QList<QAction *> actions = m_toolBar->actions();
        for (QAction *a : actions) {
            const QString text = a->text();
            if (text == QLatin1String("Обновить"))
                a->setIcon(Theme::themedIcon(QStringLiteral("refresh"), 16));
            else if (text == QLatin1String("Скачать zapret"))
                a->setIcon(Theme::themedIcon(QStringLiteral("arrow-down"), 16));
            else if (text == QLatin1String("Настройки"))
                a->setIcon(Theme::themedIcon(QStringLiteral("settings"), 16));
            else if (text == QLatin1String("Остановить"))
                a->setIcon(Theme::themedIcon(QStringLiteral("stop"), 16));
        }
    }
    setWindowIcon(Theme::icon(QStringLiteral("app"), Theme::accent(), 32));
    updateTrayIcon();
}

void MainWindow::refreshStrategies()
{
    const int count = m_manager->reload();
    populateCards();
    statusBar()->showMessage(QStringLiteral("Найдено стратегий: %1").arg(count), 4000);
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
        statusBar()->showMessage(busy, 5000);
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
            statusBar()->showMessage(message, 6000);
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
