#include "StrategyCard.h"
#include "StrategyParser.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QEnterEvent>
#include <QFileInfo>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

namespace zapret {

namespace {
constexpr int kShadowBlurHover = 24;
constexpr int kShadowBlurIdle = 0;

// The hover glow is the accent, not black: on a #0B0C10 canvas a dark shadow is invisible, while a
// purple one reads as the card lifting into the theme's own light. The alpha has to stay low:
// QGraphicsDropShadowEffect blurs a *filled* rounded rect, so a saturated tint at this radius lands
// as a purple slab along the card's bottom edge instead of a halo (seen in a screenshot of the
// rendered window). The border in the QSS carries the neon edge; the glow only softens it.
QColor glowColor()
{
    QColor glow = Theme::accent();
    glow.setAlpha(56);
    return glow;
}

// Quote a token the way a shell would need it, so a copied command line can be pasted and run.
QString quoteArg(const QString &token)
{
    if (!token.isEmpty() && !token.contains(QLatin1Char(' ')) && !token.contains(QLatin1Char('"')))
        return token;
    QString quoted = token;
    quoted.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}
} // namespace

StrategyCard::StrategyCard(QWidget *parent)
    : QFrame(parent)
{
    build();
}

StrategyCard::StrategyCard(const Strategy &strategy, QWidget *parent)
    : QFrame(parent)
    , m_strategy(strategy)
{
    build();
    setStrategy(strategy);
}

StrategyCard::~StrategyCard()
{
    if (m_shadowAnim)
        m_shadowAnim->stop();
}

void StrategyCard::build()
{
    setObjectName(QStringLiteral("strategyCard"));
    setProperty("active", false);
    setProperty("error", false);
    setAttribute(Qt::WA_StyledBackground, true);
    setAccessibleName(QStringLiteral("strategy card"));

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("cardTitle"));
    m_title->setTextFormat(Qt::PlainText);

    m_description = new QLabel(this);
    m_description->setObjectName(QStringLiteral("cardDesc"));
    m_description->setWordWrap(true);
    m_description->setTextFormat(Qt::PlainText);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("cardHint"));
    m_hint->setTextFormat(Qt::PlainText);

    m_button = new QPushButton(this);
    m_button->setCursor(Qt::PointingHandCursor);
    m_button->setAutoDefault(false);
    m_button->setDefault(false);
    // The three labels this button takes — "Запустить", "Остановить", "Повторить" — differ in width, and
    // a button that resizes with them moves the card's right edge from row to row. Pinning the width
    // keeps every card in the column on the same grid.
    m_button->setMinimumWidth(124);
    // The idle appearance has to be applied here: setRunning() ignores a no-op transition, so the
    // label/objectName a fresh card starts with would otherwise stay empty.
    m_button->setText(QStringLiteral("Запустить"));
    m_button->setObjectName(QStringLiteral("primaryButton"));

    // Failure badge: an alert glyph plus the reason the last start failed. It lives between the
    // description and the action row and is hidden until setError() puts text in it, so a healthy card
    // has exactly the same geometry as before.
    m_errorRow = new QWidget(this);
    m_errorRow->setObjectName(QStringLiteral("cardErrorRow"));
    m_errorRow->setVisible(false);
    m_errorIcon = new QLabel(m_errorRow);
    m_errorIcon->setObjectName(QStringLiteral("cardErrorIcon"));
    m_errorIcon->setFixedSize(16, 16);
    m_errorIcon->setScaledContents(true);
    m_errorText = new QLabel(m_errorRow);
    m_errorText->setObjectName(QStringLiteral("cardError"));
    m_errorText->setTextFormat(Qt::PlainText);
    // Ignored horizontal policy: the label must yield to the card width instead of pushing it wider,
    // which is what makes the elision in refreshErrorBadge() the thing that keeps lines on one row.
    m_errorText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_errorText->setMinimumWidth(0);

    auto *errorLayout = new QHBoxLayout(m_errorRow);
    errorLayout->setContentsMargins(0, 0, 0, 0);
    errorLayout->setSpacing(6);
    errorLayout->addWidget(m_errorIcon, 0);
    errorLayout->addWidget(m_errorText, 1);

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(10);
    actions->addWidget(m_hint);
    actions->addStretch(1);
    actions->addWidget(m_button);

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(14, 12, 14, 12);
    column->setSpacing(6);
    column->addWidget(m_title);
    column->addWidget(m_description);
    column->addWidget(m_errorRow);
    column->addSpacing(2);
    column->addLayout(actions);

    connect(m_button, &QPushButton::clicked, this, [this] {
        if (m_running)
            emit stopRequested();
        else
            emit startRequested(id());
    });

    // Hover elevation. The effect is installed once and only its blur radius animates, so the card
    // costs nothing while idle — but "nothing" has to be explicit: QGraphicsDropShadowEffect with
    // blurRadius 0 does not disable itself, it draws an unblurred copy of the widget's rect at the
    // offset, so a coloured shadow at rest is a hard purple slab under every card. Idle therefore
    // paints with a transparent ink, and enterEvent() puts the accent on before the blur grows.
    m_shadow = new QGraphicsDropShadowEffect(this);
    m_shadow->setBlurRadius(kShadowBlurIdle);
    m_shadow->setColor(Qt::transparent);
    m_shadow->setOffset(0, 4);
    setGraphicsEffect(m_shadow);

    m_shadowAnim = new QPropertyAnimation(m_shadow, QByteArrayLiteral("blurRadius"), this);
    m_shadowAnim->setDuration(140);
    m_shadowAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_shadowAnim, &QPropertyAnimation::finished, this, [this] {
        if (m_shadow->blurRadius() <= kShadowBlurIdle)
            m_shadow->setColor(Qt::transparent);
    });

    setStrategy(m_strategy);
}

QString StrategyCard::kindLabel(Strategy::Kind kind)
{
    switch (kind)
    {
    case Strategy::Kind::Batch:       return QStringLiteral("BAT");
    case Strategy::Kind::PowerShell:  return QStringLiteral("PowerShell");
    case Strategy::Kind::ShellScript: return QStringLiteral("init-скрипт");
    case Strategy::Kind::SystemdUnit: return QStringLiteral("systemd");
    case Strategy::Kind::Binary:      return QStringLiteral("бинарник");
    case Strategy::Kind::Unknown:     break;
    }
    return QStringLiteral("—");
}

QString StrategyCard::describeStrategy(const Strategy &strategy)
{
    if (!strategy.description.isEmpty())
        return strategy.description;

    switch (strategy.kind)
    {
    case Strategy::Kind::Binary:
    {
        const QString exe = QFileInfo(strategy.executablePath.isEmpty()
                                          ? strategy.filePath
                                          : strategy.executablePath).fileName();
        QString text = QStringLiteral("Прямой запуск %1").arg(exe);
        if (!strategy.argv.isEmpty())
            text += QStringLiteral(", %1 арг.").arg(strategy.argv.size());
        return text;
    }
    case Strategy::Kind::Batch:
        return QStringLiteral("Обёртка cmd.exe /c %1").arg(strategy.fileName);
    case Strategy::Kind::PowerShell:
        return QStringLiteral("Запуск через PowerShell: %1").arg(strategy.fileName);
    case Strategy::Kind::ShellScript:
        return QStringLiteral("Init-скрипт zapret (%1 start/stop)").arg(strategy.fileName);
    case Strategy::Kind::SystemdUnit:
        return QStringLiteral("Unit-файл systemd: %1").arg(strategy.fileName);
    case Strategy::Kind::Unknown:
        break;
    }
    return strategy.fileName;
}

void StrategyCard::setStrategy(const Strategy &strategy)
{
    m_strategy = strategy;

    const QString name = strategy.name.isEmpty()
                             ? StrategyParser::displayName(strategy.fileName)
                             : strategy.name;
    m_title->setText(name);
    m_description->setText(describeStrategy(strategy));

    QString hint = kindLabel(strategy.kind);
    if (strategy.requiresRoot)
        hint += QStringLiteral(" · %1").arg(
            strategy.platform == Strategy::Platform::Windows
                ? QStringLiteral("нужен администратор")
                : QStringLiteral("нужен root"));
    m_hint->setText(hint);

    if (!strategy.filePath.isEmpty()) {
        m_hint->setToolTip(strategy.filePath);
        m_title->setToolTip(strategy.filePath);
    }

    if (m_strategy.id.isEmpty())
        m_strategy.id = m_strategy.filePath;

    refreshErrorBadge();
}

void StrategyCard::setId(const QString &id)
{
    m_strategy.id = id;
}

QString StrategyCard::id() const
{
    return m_strategy.id.isEmpty() ? m_strategy.filePath : m_strategy.id;
}

QString StrategyCard::commandLine() const
{
    const QString program = m_strategy.executablePath.isEmpty()
                                ? m_strategy.filePath
                                : m_strategy.executablePath;

    QStringList parts;
    parts << quoteArg(program);
    if (!m_strategy.argv.isEmpty()) {
        for (const QString &a : m_strategy.argv)
            parts << quoteArg(a);
    } else if (!m_strategy.primaryArg.isEmpty()) {
        parts << quoteArg(m_strategy.primaryArg);
    }
    return parts.join(QLatin1Char(' '));
}

void StrategyCard::setRunning(bool running)
{
    if (running == m_running)
        return;
    m_running = running;

    // A confirmed start supersedes whatever failed here before: the badge must not outlive the failure.
    if (running && !m_errorMessage.isEmpty()) {
        m_errorMessage.clear();
        applyErrorProperty(false);
        refreshErrorBadge();
    }

    refreshButton();
    setActive(running);
}

void StrategyCard::setActionEnabled(bool enabled, const QString &reason)
{
    m_actionEnabled = enabled;
    if (!m_button)
        return;
    if (m_button->isEnabled() != enabled)
        m_button->setEnabled(enabled);
    // The reason belongs on the button, not only in the log: this is the control the user just reached
    // for, and an empty tooltip on a live one would suggest the card is broken.
    const QString wanted = enabled ? QString() : reason;
    if (m_button->toolTip() != wanted)
        m_button->setToolTip(wanted);
}

void StrategyCard::refreshButton()
{
    QString text;
    QString styleName;
    if (m_running) {
        text = QStringLiteral("Остановить");
        styleName = QStringLiteral("dangerButton");
    } else if (hasError()) {
        // The action after a failure is the same action that failed — say so instead of "Запустить".
        text = QStringLiteral("Повторить");
        styleName = QStringLiteral("primaryButton");
    } else {
        text = QStringLiteral("Запустить");
        styleName = QStringLiteral("primaryButton");
    }

    if (m_button->text() == text && m_button->objectName() == styleName)
        return;
    m_button->setText(text);
    m_button->setObjectName(styleName);
    // Object name changed, so QSS must be re-evaluated for the button.
    m_button->style()->unpolish(m_button);
    m_button->style()->polish(m_button);
    m_button->update();
}

void StrategyCard::setError(const QString &message)
{
    const QString trimmed = message.trimmed();
    if (trimmed == m_errorMessage) {
        // Same reason reported twice (a retry that fails identically): keep the badge, skip the polish.
        refreshErrorBadge();
        return;
    }
    m_errorMessage = trimmed;
    applyErrorProperty(!trimmed.isEmpty());
    refreshButton();
    refreshErrorBadge();
}

void StrategyCard::clearError()
{
    setError(QString());
}

void StrategyCard::refreshErrorBadge()
{
    if (!m_errorRow)
        return;
    m_errorRow->setVisible(hasError());
    if (!hasError()) {
        m_errorText->clear();
        m_errorIcon->clear();
        m_errorText->setToolTip(QString());
        return;
    }
    m_errorIcon->setPixmap(Theme::icon(QStringLiteral("error"), Theme::error(), 16).pixmap(16, 16));
    // Elide into the width the layout actually gave us. Before the first layout pass width() is 0, so a
    // not-yet-shown card uses a conservative guess; resizeEvent() refreshes it with the real number.
    const int width = m_errorText->width() > 40 ? m_errorText->width() : 220;
    m_errorText->setText(m_errorText->fontMetrics().elidedText(
        m_errorMessage, Qt::ElideRight, width));
    // The full, un-elided reason belongs in the tooltip — that is what a user hovers a red badge for.
    m_errorText->setToolTip(m_errorMessage);
    m_errorRow->setToolTip(m_errorMessage);
}

void StrategyCard::setActive(bool active)
{
    applyActiveProperty(active);
}

void StrategyCard::applyActiveProperty(bool active)
{
    if (property("active").toBool() == active)
        return;
    setProperty("active", active);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void StrategyCard::applyErrorProperty(bool error)
{
    if (property("error").toBool() == error)
        return;
    setProperty("error", error);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void StrategyCard::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);

    QAction *copy = menu.addAction(Theme::themedIcon(QStringLiteral("copy"), 16),
                                   QStringLiteral("Скопировать команду"));
    copy->setEnabled(!m_strategy.filePath.isEmpty());
    connect(copy, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(commandLine());
    });

    QAction *open = menu.addAction(Theme::themedIcon(QStringLiteral("settings"), 16),
                                   QStringLiteral("Открыть папку со стратегией"));
    const QString dir = m_strategy.dirPath.isEmpty()
                            ? QFileInfo(m_strategy.filePath).absolutePath()
                            : m_strategy.dirPath;
    open->setEnabled(!dir.isEmpty() && QFileInfo::exists(dir));
    connect(open, &QAction::triggered, this, [dir] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    menu.exec(event->globalPos());
}

void StrategyCard::enterEvent(QEnterEvent *event)
{
    QFrame::enterEvent(event);
    if (!m_shadowAnim)
        return;
    m_shadowAnim->stop();
    // Put the accent on before the blur grows: leaveEvent() hands the ink back to transparent once the
    // radius is home, so this is the only place the glow colour is ever set.
    m_shadow->setColor(glowColor());
    m_shadowAnim->setStartValue(m_shadow->blurRadius());
    m_shadowAnim->setEndValue(kShadowBlurHover);
    m_shadowAnim->start();
}

void StrategyCard::leaveEvent(QEvent *event)
{
    QFrame::leaveEvent(event);
    if (!m_shadowAnim)
        return;
    m_shadowAnim->stop();
    m_shadowAnim->setStartValue(m_shadow->blurRadius());
    m_shadowAnim->setEndValue(kShadowBlurIdle);
    m_shadowAnim->start();
}

void StrategyCard::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    // The badge elides against the width the layout gave it, so every resize re-derives the text.
    refreshErrorBadge();
}

} // namespace zapret
