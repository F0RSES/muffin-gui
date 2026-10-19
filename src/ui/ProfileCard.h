#pragma once

#include "PingIndicator.h"
#include "Strategy.h"

#include <QFrame>
#include <QHash>
#include <QStringList>

class QHBoxLayout;
class QLabel;
class QMenu;
class QToolButton;

namespace zapret {

// One profile in the "Профили" list.
//
// The row is deliberately horizontal and deliberately sparse: the name at the left edge, how the two
// services it exists to unblock are answering right now, and a menu of the two things you can do to the
// entry itself. What is NOT on it is a start button. Running a profile is the dashboard's power
// button's job — one control, one meaning — and a second way to start the same thing from a second
// screen is how a GUI grows two widgets that disagree about the state.
//
// Selection is the card's only interactive state, and the window owns it: a click anywhere on the row
// makes this the profile the power button will run, and `[selected="true"]` draws the neon edge. QSS
// never re-reads a dynamic property on its own, so every setter here unpolishes and re-polishes.
//
// A backend failure pins itself to the card that caused it the same way the old one did — the edge turns
// red and the reason moves into the tooltip — but the reason no longer takes a row of its own, because a
// 52 px row has nowhere to put a sentence.
class ProfileCard : public QFrame
{
    Q_OBJECT

public:
    explicit ProfileCard(QWidget *parent = nullptr);
    explicit ProfileCard(const Strategy &strategy, QWidget *parent = nullptr);

    void setStrategy(const Strategy &strategy);
    // Re-derive the text the strategy implies (the name) and the wording the pings carry, in the
    // language the UI now uses.
    void retranslateText();
    const Strategy &strategy() const { return m_strategy; }
    // The id the window keys its cards by — the strategy's own id, falling back to its path.
    QString id() const;
    // "<program> <argv...>" as the backend would exec it. What the menu's "Скопировать" puts on the
    // clipboard.
    QString commandLine() const;

    // ---- the mini ping chips ------------------------------------------------------
    // Rebuild the row of chips, one per host, in the order given. The hosts are the ones the window
    // actually probes; a chip for a service nobody measures would be the only dishonest number on the
    // page.
    void setHosts(const QStringList &hosts);
    // Feed one chip. An unknown host is ignored rather than guessed at.
    void setPing(const QString &host, PingIndicator::PingStatus status, int latencyMs = -1);

    void setSelected(bool on);
    bool isSelected() const { return m_selected; }

    void setError(const QString &message);   // empty clears it
    bool hasError() const { return !m_errorMessage.isEmpty(); }
    QString errorMessage() const { return m_errorMessage; }

    // Re-bake every glyph this card paints. Theme::icon() tints into a pixmap, so a theme switch cannot
    // recolour one — the window calls this for every card when the palette moves.
    void retheme();

    // Handles for tests.
    QLabel *nameLabel() const { return m_name; }
    QToolButton *menuButton() const { return m_menu; }
    int pingChipCount() const { return static_cast<int>(m_pings.size()); }

signals:
    // The row was pressed (or activated from the keyboard). The window is the only thing that decides
    // what selection means.
    void clicked(const QString &id);
    // The menu's two entries. Copying has already happened by the time this fires — the signal is the
    // window's cue to say so in the footer.
    void copyRequested(const QString &id);
    void deleteRequested(const QString &id);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    // One chip: the service's mark, tinted with its status, and the latency beside it. The status is
    // kept here so retheme()/retranslateText() can re-derive both halves without a second lookup.
    struct Ping
    {
        QWidget *chip = nullptr;
        QLabel *icon = nullptr;
        QLabel *value = nullptr;
        PingIndicator::PingStatus status = PingIndicator::PingStatus::Unknown;
        int latencyMs = -1;
    };

    void build();
    QMenu *buildMenu();
    void refreshPing(const QString &host, Ping &ping);
    void applyStateProperty(const char *name, bool on);

    Strategy m_strategy;
    QLabel *m_name = nullptr;
    QWidget *m_pingHost = nullptr;
    QHBoxLayout *m_pingRow = nullptr;
    QHash<QString, Ping> m_pings;
    QToolButton *m_menu = nullptr;
    QString m_errorMessage;
    bool m_selected = false;
};

} // namespace zapret
