#include "MainWindow.h"

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

    m_placeholder = new QLabel(m_stack);
    m_placeholder->setObjectName(QStringLiteral("placeholder"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setText(QStringLiteral(
        "Стратегии не найдены.\n\n"
        "Укажите папку с zapret в настройках (Ctrl+S) —\n"
        "нужен каталог windows/ с *.bat или linux/ с init.d/zapret."));

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

    QAction *settings = m_toolBar->addAction(Theme::themedIcon(QStringLiteral("settings"), 16),
                                             QStringLiteral("Настройки"));
    settings->setToolTip(QStringLiteral("Настройки (Ctrl+S)"));
    connect(settings, &QAction::triggered, this, &MainWindow::openSettings);

    m_stopAction = m_toolBar->addAction(Theme::themedIcon(QStringLiteral("stop"), 16),
                                        QStringLiteral("Остановить"));
    m_stopAction->setToolTip(QStringLiteral("Остановить запущенную стратегию (Ctrl+Shift+X)"));
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

    m_stack->setCurrentWidget(list.isEmpty()
                                  ? static_cast<QWidget *>(m_placeholder)
                                  : static_cast<QWidget *>(m_scroll));
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

    const QString titleName = (!name.isEmpty() && state != IZapretBackend::State::Stopped
                               && state != IZapretBackend::State::Error)
                                  ? name
                                  : QStringLiteral("остановлено");
    setWindowTitle(QStringLiteral("Muffin — %1").arg(titleName));

    if (m_dirLabel) {
        const QString dir = m_manager->resolvedRoot().isEmpty()
                                ? QStringLiteral("zapret не найден")
                                : QDir::toNativeSeparators(m_manager->resolvedRoot());
        m_dirLabel->setText(dir);
        m_dirLabel->setToolTip(dir);
    }

    const bool activeIsUp = m_manager->isRunning() || state == IZapretBackend::State::Starting
                            || state == IZapretBackend::State::Stopping;
    if (m_stopAction)
        m_stopAction->setEnabled(activeIsUp);
    const QString activeId = m_manager->activeStrategyId();
    for (auto it = m_cards.cbegin(); it != m_cards.cend(); ++it)
        it.value()->setRunning(activeIsUp && it.key() == activeId);

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

void MainWindow::startStrategy(const QString &id, bool interactive)
{
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
    const IZapretBackend::State state = m_manager->state();
    if (state == IZapretBackend::State::Stopped || state == IZapretBackend::State::Error)
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
    if (m_manager->isRunning())
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
    if (m_manager->isRunning())
        m_manager->stop();
    event->accept();
}


} // namespace zapret
