#pragma once

#include "IZapretBackend.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QString>

class QAction;
class QCloseEvent;
class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QSystemTrayIcon;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace zapret {

class LogPanel;
class PingIndicator;
class PingService;
class PowerButton;
class Settings;
class StrategyCard;
class ZapretManager;

// The application window: a narrow icon rail down the left, one full-width page at a time to its right,
// everything driven through ZapretManager + Settings.
//
// The rail is 70 px of glyphs only — brand at the top, the journal in the middle, settings and the
// version at the foot. The page beside it is the dashboard: a header naming the app and its connection
// state, one enormous power button that starts and stops the strategy, and a row of neon ping badges.
// Strategy cards are not a page you navigate to so much as a choice you make from the dashboard's
// "route" pill, because picking the route and running it are one decision, not two screens.
//
// It owns no domain logic. Cards come from the manager's catalog (which is the parser's output), the
// start/stop path is the manager's, and persisted values are Settings'. What lives here is only
// presentation state: which card is flagged active, what the title says, whether closing hides to the
// tray, and which page the rail is showing.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Text/colour of the toolbar status chip for a backend state. Static so the mapping is testable.
    static QString statusText(IZapretBackend::State state, const QString &strategyName);
    // The header's one-word chip for a state ("CONNECTED", "OFFLINE"). Uppercase and English on purpose:
    // it is a design element beside a 6 px dot, not a sentence, and the mock spells it that way. The
    // longer, strategy-naming form stays in statusText() for the tray tooltip and the log.
    static QString connectionText(IZapretBackend::State state);
    // QSS dynamic-property value for [statusKind="..."].
    static QString statusKind(IZapretBackend::State state);

    // ---- action gating: pure predicates again, so the whole matrix is checkable without a process ----
    //
    // A backend owns one operation at a time. LinuxBackend::start() refuses outright while anything is up
    // or coming up ("an operation is already in progress (stop first)"), and a portable stop takes as long
    // as the iptables teardown under pkexec needs — ten seconds in the log that exposed this. For all of
    // that time the window used to keep both the toolbar's "Остановить" and every card's "Запустить"
    // clickable, so the only honest answer was an error row and a failure badge on a card that had done
    // nothing wrong. Greyed out and worded is the fix; the guards in the slots below are the backstop.
    static bool canStartFrom(IZapretBackend::State state);
    static bool canStopFrom(IZapretBackend::State state);
    // Why a start cannot be issued in this state, empty when it can. Doubles as the tooltip of every
    // control the gating disables, so a greyed button says what it is waiting for.
    static QString blockedReason(IZapretBackend::State state);

    // ---- tray policy, expressed as pure predicates so it is testable without a desktop ----
    // Whether the platform reports a tray host at all (GNOME without an AppIndicator extension, a bare
    // Wayland compositor and a headless session all answer false).
    static bool traySupported();
    // Closing the window may only hide it when there is a tray to hide it into; otherwise the same
    // click has to quit, or the user loses their only way back to the window.
    static bool hideOnClose(bool minimizeToTraySetting, bool trayAvailable);

    // Which card a backend failure should be pinned to, from the three ids the window can name a run
    // by, in precedence order:
    //
    //   pendingStartId — the start this window asked for and that has not resolved yet;
    //   activeId       — the strategy the manager currently reports as running;
    //   lastRunId      — the last run the window saw as active, kept because a manager that has
    //                    already forgotten the run answers with "" (see m_lastRunId).
    //
    // The third case is not hypothetical: portable mode deliberately defers its error report until the
    // nfqueue teardown finished, and by then stateChanged(Error) has already made the manager drop the
    // active strategy, so the first two are both empty and the failure would otherwise badge nothing.
    static QString errorTargetId(const QString &pendingStartId, const QString &activeId,
                                 const QString &lastRunId);

    // Accessors used by tests and by nothing else in the app.
    ZapretManager *manager() const { return m_manager; }
    LogPanel *logPanel() const { return m_log; }
    Settings *settingsModel() const { return m_settings; }
    int cardCount() const { return static_cast<int>(m_cards.size()); }
    // True when a tray icon was actually created (a DE without a tray host leaves this false).
    bool hasTray() const { return m_tray != nullptr; }
    // The card a failure was pinned to ("" when none). Test hook for the inline error badge.
    QString errorCardId() const;

    // ---- rail navigation ------------------------------------------------------------
    // Pages are addressed by index so a test — and the unread-log alert below — can name the visible
    // one without walking the widget tree.
    //
    // PageStrategies is not reachable from the rail: the rail is three glyphs and the mock has no room
    // for a fourth. It opens from the dashboard's route pill, which is where the choice belongs — you
    // pick a strategy in order to run it.
    enum Page : int
    {
        PageDashboard  = 0,
        PageStrategies = 1,
        PageLogs       = 2
    };
    int currentPage() const;
    void setCurrentPage(int page);
    // The page to fall back to when a full-screen view is closed: always the dashboard, never whichever
    // page happened to be up before.
    Q_INVOKABLE void showDashboard();
    // Set while the journal is hidden and a warning or error arrives, cleared by opening it: the page
    // switch costs the user the passive view of the log, so something has to say "there is news here".
    bool logAlertActive() const;

    void refreshStrategies();
    void openSettings();
    // Fetch the newest release for this platform, install it into the managed directory, and point the
    // application at it. Reached from the toolbar, from SettingsDialog and from the "no strategies"
    // placeholder — three entrances, one implementation, because a download that behaves differently
    // depending on which button opened it is the kind of bug nobody notices until an install fails.
    void downloadZapret();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onStartRequested(const QString &id);
    void onStopRequested();
    // The power button: stop what is running, otherwise start what the route pill names. One slot for
    // both because the button is one control whose meaning the state decides.
    void onPowerClicked();
    void onStateChanged(IZapretBackend::State state);
    void onStrategyStarted(const QString &id, const QString &name);
    void onStrategyStopped();
    void onLogMessage(const QString &line, int level);
    void onErrorOccurred(const QString &message);
    void onPingUpdated(const QString &host, int status, int latencyMs);
    void onSettingsChanged();
    void onStrategiesLoaded(int count);
    // A download finished and validated: persist the path and rescan.
    void zapretInstalled(const QString &path);

private:
    void buildCentral();
    // The rail: brand at the head, the journal in the middle, settings and the version at the foot.
    QWidget *buildSidebar();
    // The column to the rail's right: header, page stack, footer.
    QWidget *buildMainArea();
    QWidget *buildHeader();
    QWidget *buildFooter();
    QWidget *buildDashboardPage();
    QWidget *buildStrategiesPage();
    QWidget *buildLogsPage();
    // One rail glyph. The name goes in a property so applyTheme() can re-tint it: Theme::icon() bakes the
    // colour into a pixmap, so a theme switch cannot repaint an already-loaded glyph.
    QPushButton *addRailButton(QVBoxLayout *box, const QString &glyph, const QString &text);
    // Turns the journal's tab red while it holds a warning or error the user has not opened. The
    // property is what the QSS [alert="true"] rule keys off, so it needs the same unpolish/polish
    // dance StrategyCard uses for [active] and [error].
    void setLogAlert(bool on);
    // The one line the header's chip is allowed to be built from: text plus colour from the state.
    void setConnectionChip(const QString &text, const QString &statusKind);
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
    // The footer's temporary line — what statusBar()->showMessage used to be. The redesign retired the
    // status bar, and a window that creates one just to flash a sentence in it for three seconds is how
    // the old chrome creeps back.
    void showTransientMessage(const QString &text, int timeoutMs);

    // Pin a backend failure onto the card that caused it (an empty message clears it), and tell the
    // user once — repeated identical failures are logged and badged, but not re-dialoged.
    void attachErrorBadge(const QString &id, const QString &message);
    void notifyError(const QString &message, bool interactive);

    Settings *m_settings = nullptr;
    ZapretManager *m_manager = nullptr;
    PingService *m_ping = nullptr;
    LogPanel *m_log = nullptr;

    QWidget *m_sidebar = nullptr;          // the rail: brand, the journal glyph, settings and version
    QFrame *m_mainArea = nullptr;          // header, page stack, footer
    QStackedWidget *m_pages = nullptr;     // PageDashboard <-> PageStrategies <-> PageLogs
    QPushButton *m_navLogs = nullptr;      // carries the [alert="true"] property
    PowerButton *m_power = nullptr;        // the dashboard's one control
    QPushButton *m_routePill = nullptr;    // "Smart route" — names the strategy the power button runs
    QWidget *m_statusDot = nullptr;        // the 6 px chip beside the connection text
    QLabel *m_statusLabel = nullptr;       // "CONNECTED" and friends
    QLabel *m_dirLabel = nullptr;          // the resolved zapret root, in the footer
    QLabel *m_footerLabel = nullptr;       // the ping cadence, or a transient message
    QTimer *m_footerTimer = nullptr;       // for how long a transient message holds the footer
    QString m_footerIdleText;              // what the footer says when nothing is being announced
    QWidget *m_dashboardPage = nullptr;
    QWidget *m_strategiesPage = nullptr;
    QWidget *m_logsPage = nullptr;
    QScrollArea *m_scroll = nullptr;
    QStackedWidget *m_stack = nullptr;      // cards list <-> "no strategies found" placeholder
    QWidget *m_cardHost = nullptr;
    QVBoxLayout *m_cardLayout = nullptr;
    QWidget *m_placeholder = nullptr;      // "no strategies found" page: the text plus its two ways out

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
    // The most recent run the manager still named as active. ZapretManager clears that name inside the
    // same stateChanged(Error) that precedes a backend's errorOccurred, so a deferred failure — every
    // portable-mode failure that had to tear the queue down first — arrives with nothing to attribute
    // it to unless the window remembered. Cleared when a run comes down cleanly, so a much later,
    // unrelated refusal cannot badge a card whose run ended without incident.
    QString m_lastRunId;
    QString m_lastErrorText;          // for dialog coalescing of repeated identical failures
    QElapsedTimer m_errorClock;       // running from construction
    qint64 m_lastErrorAt = 0;
};

} // namespace zapret
