#include "ProfileCard.h"

#include "LanguageManager.h"
#include "PingIndicator.h"
#include "StrategyParser.h"
#include "Theme.h"
#include "WindowsArgs.h"

#include <QApplication>
#include <QClipboard>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QStyle>
#include <QToolButton>

namespace zapret {

namespace {

// The chip's mark. Two thirds of the dashboard's 22 px mark: on that screen the ring is the news, here
// the number is the news and the mark only says which service the number belongs to.
constexpr int kMiniGlyphPx = 18;
// The box the mark is set into — a hair larger than the glyph so the mark never touches the ring.
constexpr int kMiniIconPx = 20;
// Inside the ring: the mark and the figure are set off its edge, and apart from each other. A chip that
// packs its contents against the border reads as two labels with a line drawn around them, which is the
// "too tight" the row had.
constexpr int kMiniPadX = 8;
constexpr int kMiniPadY = 4;
constexpr int kMiniGapPx = 6;
// The halo. QSS has no box-shadow, so the glow is a graphics effect on the chip — and an effect paints
// outside the widget it is set on, which is why the row that holds the chips reserves this much room on
// every side (see m_pingRow's margins): a child is clipped to its parent's rect, and a ring cut off at
// the rim is the one thing this design cannot show.
constexpr int kMiniGlowBlurPx = 14;
constexpr int kMiniGlowRoomPx = 8;
constexpr int kMiniGlowAlpha = 130;

// The chip's ring is drawn by the sheet from a status token, because QSS cannot read a QColor and
// PingIndicator::dotColor() is C++'s to know. One spelling per status, both sheets, same words.
const char *statusToken(PingIndicator::PingStatus status)
{
    switch (status)
    {
    case PingIndicator::PingStatus::Waiting: return "waiting";
    case PingIndicator::PingStatus::Ok:      return "ok";
    case PingIndicator::PingStatus::Fail:    return "fail";
    case PingIndicator::PingStatus::Unknown: break;
    }
    return "unknown";
}

// QSS never re-reads a dynamic property on its own, so a chip whose verdict moved has to be taken apart
// and put back together — the same dance the row does for [selected] and [error]. Setting it when it
// already reads the same would re-polish every chip of every row on every cycle for nothing.
void applyStatusToken(QWidget *chip, PingIndicator::PingStatus status)
{
    if (!chip)
        return;
    const QString token = QString::fromLatin1(statusToken(status));
    if (chip->property("pingStatus").toString() == token)
        return;
    chip->setProperty("pingStatus", token);
    chip->style()->unpolish(chip);
    chip->style()->polish(chip);
    chip->update();
}

} // namespace

ProfileCard::ProfileCard(QWidget *parent)
    : QFrame(parent)
{
    build();
    setStrategy(m_strategy);
}

ProfileCard::ProfileCard(const Strategy &strategy, QWidget *parent)
    : QFrame(parent)
    , m_strategy(strategy)
{
    build();
    setStrategy(strategy);
}

void ProfileCard::build()
{
    setObjectName(QStringLiteral("profileCard"));
    setProperty("selected", false);
    setProperty("error", false);
    // A plain QFrame paints no stylesheet background of its own; this is what lets #profileCard's fill
    // and its 12 px radius appear at all.
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // One height for every row, so a column of cards reads as a list rather than as notes of different
    // length. The name's 16 px and the chips' 14 px both fit inside it with room to breathe.
    setFixedHeight(52);
    setAccessibleName(QStringLiteral("profile"));

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(16, 0, 8, 0);
    row->setSpacing(10);

    m_name = new QLabel(this);
    m_name->setObjectName(QStringLiteral("profileName"));
    m_name->setTextFormat(Qt::PlainText);
    // The label must not eat the press: the row is the click target, and a label that consumed mouse
    // events would leave the left half of the card dead.
    m_name->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    m_pingHost = new QWidget(this);
    m_pingHost->setObjectName(QStringLiteral("profilePings"));
    m_pingHost->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_pingRow = new QHBoxLayout(m_pingHost);
    // Not padding for the chips — room for their halos. A graphics effect paints beyond the widget it
    // is set on, and a child is clipped to its parent's rect, so without this the ring's glow would be
    // cut flat where the row ends. The chips themselves would fit with no margin at all.
    m_pingRow->setContentsMargins(kMiniGlowRoomPx, kMiniGlowRoomPx,
                                  kMiniGlowRoomPx, kMiniGlowRoomPx);
    m_pingRow->setSpacing(12);

    m_menu = new QToolButton(this);
    m_menu->setObjectName(QStringLiteral("cardMenu"));
    m_menu->setIcon(Theme::themedIcon(QStringLiteral("more"), 16));
    m_menu->setIconSize(QSize(16, 16));
    m_menu->setAutoRaise(true);
    m_menu->setCursor(Qt::PointingHandCursor);
    m_menu->setFocusPolicy(Qt::TabFocus);
    m_menu->setPopupMode(QToolButton::InstantPopup);
    m_menu->setFixedSize(30, 30);
    i18nTip(m_menu, QStringLiteral("profile.menu_tip"));
    i18nAcc(m_menu, QStringLiteral("profile.menu_tip"));
    m_menu->setMenu(buildMenu());

    row->addWidget(m_name, 1);
    row->addWidget(m_pingHost, 0, Qt::AlignVCenter);
    row->addWidget(m_menu, 0, Qt::AlignVCenter);
}

// The two things that can be done to the entry itself. Both are about the file on disk, not about the
// run: starting and stopping a profile is the dashboard's job, and a menu that offered it too would be a
// second power button.
QMenu *ProfileCard::buildMenu()
{
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("cardMenuPopup"));

    QAction *copy = i18nText(menu->addAction(QString()), QStringLiteral("profile.copy"));
    copy->setIcon(Theme::themedIcon(QStringLiteral("copy"), 16));
    connect(copy, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(commandLine());
        emit copyRequested(id());
    });

    QAction *remove = i18nText(menu->addAction(QString()), QStringLiteral("profile.delete"));
    remove->setIcon(Theme::icon(QStringLiteral("clear"), Theme::error(), 16));
    connect(remove, &QAction::triggered, this, [this] { emit deleteRequested(id()); });

    return menu;
}

void ProfileCard::setStrategy(const Strategy &strategy)
{
    m_strategy = strategy;
    if (m_strategy.id.isEmpty())
        m_strategy.id = m_strategy.filePath;

    const QString name = m_strategy.name.isEmpty()
                             ? StrategyParser::displayName(m_strategy.fileName)
                             : m_strategy.name;
    m_name->setText(name);
    // The path is the only unambiguous thing about a row once two folders hold a file of the same name,
    // so it is what the whole card's tooltip says.
    const QString tip = m_strategy.filePath.isEmpty() ? name : m_strategy.filePath;
    setToolTip(tip);
    m_name->setToolTip(tip);
}

void ProfileCard::retranslateText()
{
    setStrategy(m_strategy);
    for (auto it = m_pings.begin(); it != m_pings.end(); ++it)
        refreshPing(it.key(), it.value());
}

QString ProfileCard::id() const
{
    return m_strategy.id.isEmpty() ? m_strategy.filePath : m_strategy.id;
}

QString ProfileCard::commandLine() const
{
    const QString program = m_strategy.executablePath.isEmpty()
                                ? m_strategy.filePath
                                : m_strategy.executablePath;

    QStringList parts;
    // winQuoteArg is the rule QProcess applies to the arguments it is about to launch, so the line a
    // user copies from a profile row is the line the payload really receives. One implementation is
    // what keeps the two from ever disagreeing.
    parts << winQuoteArg(program);
    if (!m_strategy.argv.isEmpty()) {
        for (const QString &a : m_strategy.argv)
            parts << winQuoteArg(a);
    } else if (!m_strategy.primaryArg.isEmpty()) {
        parts << winQuoteArg(m_strategy.primaryArg);
    }
    return parts.join(QLatin1Char(' '));
}

void ProfileCard::setHosts(const QStringList &hosts)
{
    // Rebuild rather than reconcile: the host list is two entries long and set once at startup, and a
    // card that kept a stale chip for a service nobody probes any more is exactly the kind of number the
    // page would rather not show.
    for (const Ping &ping : std::as_const(m_pings))
        if (ping.chip)
            ping.chip->deleteLater();
    m_pings.clear();

    for (const QString &host : hosts) {
        auto *chip = new QWidget(m_pingHost);
        chip->setObjectName(QStringLiteral("miniPing"));
        chip->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        // The neon around the ring. QSS has no box-shadow, so this is the only way a chip can glow —
        // and refreshPing() moves its colour with the verdict, which is the whole point of the ring.
        auto *glow = new QGraphicsDropShadowEffect(chip);
        glow->setBlurRadius(kMiniGlowBlurPx);
        glow->setOffset(0.0, 0.0);
        chip->setGraphicsEffect(glow);

        auto *box = new QHBoxLayout(chip);
        box->setContentsMargins(kMiniPadX, kMiniPadY, kMiniPadX, kMiniPadY);
        box->setSpacing(kMiniGapPx);

        auto *icon = new QLabel(chip);
        icon->setObjectName(QStringLiteral("miniPingIcon"));
        icon->setFixedSize(kMiniIconPx, kMiniIconPx);
        icon->setScaledContents(true);

        auto *value = new QLabel(chip);
        value->setObjectName(QStringLiteral("miniPingValue"));
        value->setTextFormat(Qt::PlainText);

        box->addWidget(icon);
        box->addWidget(value);
        m_pingRow->addWidget(chip);

        Ping ping;
        ping.chip = chip;
        ping.icon = icon;
        ping.value = value;
        ping.glow = glow;
        m_pings.insert(host, ping);
        refreshPing(host, m_pings[host]);
    }
}

void ProfileCard::setPing(const QString &host, PingIndicator::PingStatus status, int latencyMs)
{
    auto it = m_pings.find(host);
    if (it == m_pings.end())
        return;   // not a service this card shows a chip for
    if (it->status == status && it->latencyMs == latencyMs)
        return;
    it->status = status;
    it->latencyMs = latencyMs;
    refreshPing(host, it.value());
}

void ProfileCard::refreshPing(const QString &host, Ping &ping)
{
    if (!ping.icon || !ping.value)
        return;
    const QString glyph = PingIndicator::glyphForService(host);
    const QColor status = PingIndicator::dotColor(ping.status);
    // The mark carries the status colour — the same mapping the dashboard's rings use, so one green
    // means "answering" on both screens.
    ping.icon->setPixmap(Theme::icon(glyph, status, kMiniGlyphPx)
                             .pixmap(kMiniGlyphPx, kMiniGlyphPx));

    // The ring and its halo answer to the same verdict: the sheet paints the rim from the token, the
    // effect paints the glow from the colour, and both move together with the mark so a chip can never
    // say "answering" in one part of itself and "timeout" in another.
    if (ping.glow) {
        QColor halo = status;
        halo.setAlpha(kMiniGlowAlpha);
        ping.glow->setColor(halo);
    }
    applyStatusToken(ping.chip, ping.status);

    const QString text = PingIndicator::latencyText(ping.status, ping.latencyMs);
    ping.value->setText(text);
    ping.value->setToolTip(QStringLiteral("%1 — %2").arg(host, text));
}

void ProfileCard::retheme()
{
    m_menu->setIcon(Theme::themedIcon(QStringLiteral("more"), 16));
    if (QMenu *menu = m_menu->menu()) {
        const auto actions = menu->actions();
        for (QAction *act : actions) {
            // The two glyphs differ in colour (the delete entry is tinted with the failure red), so the
            // re-bake has to go through the same choice the builder made rather than flatten both.
            const bool isDelete = act->text() == LanguageManager::tr(QStringLiteral("profile.delete"));
            act->setIcon(isDelete ? Theme::icon(QStringLiteral("clear"), Theme::error(), 16)
                                  : Theme::themedIcon(QStringLiteral("copy"), 16));
        }
    }
    for (auto it = m_pings.begin(); it != m_pings.end(); ++it)
        refreshPing(it.key(), it.value());
}

void ProfileCard::setSelected(bool on)
{
    if (m_selected == on)
        return;
    m_selected = on;
    applyStateProperty("selected", on);
}

void ProfileCard::setError(const QString &message)
{
    const QString trimmed = message.trimmed();
    if (trimmed == m_errorMessage)
        return;
    m_errorMessage = trimmed;
    applyStateProperty("error", !trimmed.isEmpty());

    // The row has no room for a sentence, so the reason lives in the tooltip — which is what a user
    // hovering a red edge is reaching for.
    const QString base = m_strategy.filePath.isEmpty() ? m_name->text() : m_strategy.filePath;
    setToolTip(trimmed.isEmpty() ? base : QStringLiteral("%1\n%2").arg(base, trimmed));
    m_name->setToolTip(toolTip());
}

void ProfileCard::applyStateProperty(const char *name, bool on)
{
    if (property(name).toBool() == on)
        return;
    setProperty(name, on);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void ProfileCard::mousePressEvent(QMouseEvent *event)
{
    // Left button anywhere on the row selects. The menu button is a child and consumes its own press,
    // so the one part of the card that is not "select" stays "menu".
    if (event->button() == Qt::LeftButton) {
        setFocus();
        emit clicked(id());
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void ProfileCard::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Space) {
        emit clicked(id());
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

} // namespace zapret

