#pragma once

#include "Strategy.h"

#include <QFrame>

class QGraphicsDropShadowEffect;
class QLabel;
class QPushButton;
class QPropertyAnimation;

namespace zapret {

// One strategy in the list: title, what it does, and the single action the user needs.
//
// The card owns a copy of the Strategy it represents, so the GUI never has to keep an index alive:
// start/stop requests carry the strategy id, and the window looks it up in the manager's catalog.
//
// Visual state is expressed as Qt dynamic properties (`active`, `error`) that the stylesheet keys off —
// [active="true"] draws the accent border, [error="true"] draws the failure border. Because QSS does
// not re-evaluate properties automatically, both setters unpolish/polish the widget so the rule
// re-applies.
//
// A backend failure used to live only in the log and a modal dialog, which left the card looking
// healthy after a failed start. setError() pins the reason onto the card itself: a red badge row with
// the (elided) message, the full text in its tooltip, and the action button relabelled "Повторить".
class StrategyCard : public QFrame
{
    Q_OBJECT

public:
    explicit StrategyCard(QWidget *parent = nullptr);
    explicit StrategyCard(const Strategy &strategy, QWidget *parent = nullptr);
    ~StrategyCard() override;

    void setStrategy(const Strategy &strategy);
    const Strategy &strategy() const { return m_strategy; }

    // The id the start/stop signals carry. Defaults to the strategy's own id.
    void setId(const QString &id);
    QString id() const;

    // Running <-> stopped: swaps the button label/style and the `active` border. Starting a strategy
    // successfully also clears any earlier failure badge.
    void setRunning(bool running);
    bool isRunning() const { return m_running; }

    // ---- failure surface -----------------------------------------------------
    // Show `message` in the card's error badge (an empty message clears it). The badge elides to the
    // available width; errorMessage() always returns the full text.
    void setError(const QString &message);
    void clearError();
    bool hasError() const { return !m_errorMessage.isEmpty(); }
    QString errorMessage() const { return m_errorMessage; }

    // The command line shown by "Copy command": "<program> <argv...>" as the backend would exec it.
    QString commandLine() const;

signals:
    void startRequested(const QString &id);
    void stopRequested();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void build();
    void applyActiveProperty(bool active);
    void applyErrorProperty(bool error);
    void setActive(bool active);
    void refreshButton();
    // Elide the stored message into whatever width the badge label currently has.
    void refreshErrorBadge();

    // Human summary for strategies whose description is still empty (most linux/ps1 entries).
    static QString describeStrategy(const Strategy &strategy);
    static QString kindLabel(Strategy::Kind kind);

    Strategy m_strategy;
    QLabel *m_title = nullptr;
    QLabel *m_description = nullptr;
    QLabel *m_hint = nullptr;
    QPushButton *m_button = nullptr;

    QWidget *m_errorRow = nullptr;        // hidden unless the card carries a failure
    QLabel *m_errorIcon = nullptr;
    QLabel *m_errorText = nullptr;
    QString m_errorMessage;

    QGraphicsDropShadowEffect *m_shadow = nullptr;
    QPropertyAnimation *m_shadowAnim = nullptr;

    bool m_running = false;
};

} // namespace zapret
