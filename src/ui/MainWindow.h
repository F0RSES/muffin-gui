#pragma once

#include "IZapretBackend.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QString>

class QAction;
class QCloseEvent;
class QLabel;
class QScrollArea;
class QSplitter;
class QStackedWidget;
class QSystemTrayIcon;
class QTimer;
class QToolBar;
class QVBoxLayout;
class QWidget;

namespace zapret {

class LogPanel;
class PingIndicator;
class PingService;
class Settings;
class StrategyCard;
class ZapretManager;

// The application window: strategy cards on the left, live log on the right, reachability in the
// status bar, everything driven through ZapretManager + Settings.
//
// It owns no domain logic. Cards come from the manager's catalog (which is the parser's output), the
// start/stop path is the manager's, and persisted values are Settings'. What lives here is only
// presentation state: which card is flagged active, what the title says, whether closing hides to the
// tray.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Text/colour of the toolbar status chip for a backend state. Static so the mapping is testable.
    static QString statusText(IZapretBackend::State state, const QString &strategyName);
    // QSS dynamic-property value for [statusKind="..."].
    static QString statusKind(IZapretBackend::State state);

    // ---- tray policy, expressed as pure predicates so it is testable without a desktop ----
    // Whether the platform reports a tray host at all (GNOME without an AppIndicator extension, a bare
    // Wayland compositor and a headless session all answer false).
    static bool traySupported();
    // Closing the window may only hide it when there is a tray to hide it into; otherwise the same
    // click has to quit, or the user loses their only way back to the window.
    static bool hideOnClose(bool minimizeToTraySetting, bool trayAvailable);

    // Accessors used by tests and by nothing else in the app.
    ZapretManager *manager() const { return m_manager; }
    LogPanel *logPanel() const { return m_log; }
    Settings *settingsModel() const { return m_settings; }
    int cardCount() const { return static_cast<int>(m_cards.size()); }
    // True when a tray icon was actually created (a DE without a tray host leaves this false).
    bool hasTray() const { return m_tray != nullptr; }
    // The card a failure was pinned to ("" when none). Test hook for the inline error badge.
    QString errorCardId() const;

    void refreshStrategies();
    void openSettings();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onStartRequested(const QString &id);
    void onStopRequested();
    void onStateChanged(IZapretBackend::State state);
    void onStrategyStarted(const QString &id, const QString &name);
    void onStrategyStopped();
    void onLogMessage(const QString &line, int level);
    void onErrorOccurred(const QString &message);
    void onPingUpdated(const QString &host, int status, int latencyMs);
    void onSettingsChanged();
    void onStrategiesLoaded(int count);

private:
    void buildCentral();
    void buildToolBar();
    void buildStatusBar();
    // Create the tray icon if the platform has a tray host. Idempotent; returns whether one exists.
    bool ensureTray();
    void startTrayRetry();
    void buildShortcuts();
    void wire();

    void populateCards();
    void updateStatusUi();
    void updateTrayIcon();
    void applyTheme();
    void requestQuit();
    void startStrategy(const QString &id, bool interactive);

    // Pin a backend failure onto the card that caused it (an empty message clears it), and tell the
    // user once — repeated identical failures are logged and badged, but not re-dialoged.
    void attachErrorBadge(const QString &id, const QString &message);
    void notifyError(const QString &message, bool interactive);

    Settings *m_settings = nullptr;
    ZapretManager *m_manager = nullptr;
    PingService *m_ping = nullptr;
    LogPanel *m_log = nullptr;

    QToolBar *m_toolBar = nullptr;
    QSplitter *m_splitter = nullptr;
    QScrollArea *m_scroll = nullptr;
    QStackedWidget *m_stack = nullptr;      // cards list <-> "no strategies found" placeholder
    QWidget *m_cardHost = nullptr;
    QVBoxLayout *m_cardLayout = nullptr;
    QLabel *m_placeholder = nullptr;
    QAction *m_stopAction = nullptr;         // toolbar "Остановить", enabled only while managed
    QLabel *m_statusLabel = nullptr;
    QLabel *m_dirLabel = nullptr;

    QSystemTrayIcon *m_tray = nullptr;
    QTimer *m_trayRetry = nullptr;           // the tray host can appear after we start
    int m_trayAttempts = 0;

    QHash<QString, StrategyCard *> m_cards;                 // strategy id -> card
    QHash<QString, PingIndicator *> m_indicators;           // host -> chip

    bool m_quitting = false;          // explicit quit (tray menu / Ctrl+Q), bypasses hide-to-tray
    bool m_applyingSettings = false;  // re-entrancy guard for onSettingsChanged
    bool m_needsSettingsPrompt = false;
    bool m_trayWarned = false;        // "no tray here" has already been said once

    // Which strategy the current start attempt belongs to, and which card carries a failure badge.
    QString m_pendingStartId;
    QString m_errorCardId;
    QString m_lastErrorText;          // for dialog coalescing of repeated identical failures
    QElapsedTimer m_errorClock;       // running from construction
    qint64 m_lastErrorAt = 0;
};

} // namespace zapret
