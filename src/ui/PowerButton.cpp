#include "PowerButton.h"

#include "Theme.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPixmap>
#include <QVariantAnimation>

namespace zapret {

namespace {

// Translucent strokes faked between the crisp rim and the outer edge of the bloom. Each pass is wider
// and dimmer than the last; five is where the steps stop being visible at 1x.
constexpr int kGlowPasses = 5;
// The rim's stroke width — 2 px, the same weight as the ping rings, so the dashboard reads as one
// family of circles rather than one big one and several unrelated small ones.
constexpr int kRingWidth = 2;
// The glyph occupies this fraction of the diameter: 0.32 * 200 = 64 px, as specified.
constexpr qreal kGlyphRatio = 0.32;

// The same colour with its alpha scaled by `factor` (0.0 = invisible). Every layer of the glow is one
// colour at a different alpha, so this is the only alpha arithmetic in the file.
QColor faded(const QColor &c, qreal factor)
{
    QColor out = c;
    out.setAlphaF(c.alphaF() * qBound(0.0, factor, 1.0));
    return out;
}

} // namespace

PowerButton::PowerButton(QWidget *parent)
    : QAbstractButton(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // The text is never painted (paintEvent draws only the circle and its glyph) — it is the accessible
    // name, which a bare icon button otherwise has none of.
    setText(QStringLiteral("Power"));
    setDiameter(m_diameter);

    // The busy breath: a slow 1.6 s in-out loop on the glow level. Infinite on purpose — a transition
    // that finishes while the user is elsewhere (a pkexec prompt they walked away from) must keep saying
    // "still working" rather than settle into what looks like an idle button.
    m_pulse = new QVariantAnimation(this);
    m_pulse->setDuration(1600);
    m_pulse->setStartValue(1.0);
    m_pulse->setKeyValueAt(0.5, 0.45);
    m_pulse->setEndValue(1.0);
    m_pulse->setEasingCurve(QEasingCurve::InOutSine);
    m_pulse->setLoopCount(-1);
    connect(m_pulse, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        setGlow(v.toReal());
    });

    setGlyph(QStringLiteral("power"));
}

PowerButton::~PowerButton()
{
    if (m_pulse)
        m_pulse->stop();
}

QColor PowerButton::modeColor(Mode mode)
{
    switch (mode) {
    case Mode::Active:
        // Green: the same token the "CONNECTED" chip and the live ping rings are drawn with, so one
        // colour means one thing on this screen. Purple-to-green is the difference, at a glance, between
        // "press me" and "press me and your traffic goes down".
        return Theme::pingOk();
    case Mode::Idle:
    case Mode::Busy:
        break;
    }
    return Theme::accent();
}

void PowerButton::setDiameter(int px)
{
    m_diameter = qMax(48, px);
    // The widget is the circle plus the room its bloom needs to fade out; leaving that margin off is how
    // a glowing button ends up with clipped corners.
    setFixedSize(m_diameter + 2 * kHaloMargin, m_diameter + 2 * kHaloMargin);
    refreshGlyph();
    update();
}

void PowerButton::setGlyph(const QString &name)
{
    if (m_glyph == name)
        return;
    m_glyph = name;
    refreshGlyph();
    update();
}

void PowerButton::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    refreshGlyph();
    if (mode == Mode::Busy)
        startPulse();
    else
        stopPulse();
    update();
}

void PowerButton::setGlow(qreal level)
{
    const qreal clamped = qBound(0.0, level, 1.0);
    if (qFuzzyCompare(clamped, m_glow))
        return;
    m_glow = clamped;
    update();
}

void PowerButton::retheme()
{
    refreshGlyph();
    update();
}

void PowerButton::startPulse()
{
    if (m_pulse->state() == QAbstractAnimation::Running)
        return;
    m_pulse->start();
}

void PowerButton::stopPulse()
{
    if (m_pulse->state() != QAbstractAnimation::Stopped)
        m_pulse->stop();
    m_glow = 1.0;
    update();
}

QPixmap PowerButton::glyphPixmap(const QColor &tint, int px) const
{
    if (m_glyph.isEmpty() || px <= 0)
        return QPixmap();
    return Theme::icon(m_glyph, tint, px).pixmap(px, px);
}

void PowerButton::refreshGlyph()
{
    // Rasterised once per (glyph, mode, size) rather than per paint: Theme::icon() rewrites and reloads
    // an SVG document, which is far too much work to repeat on every frame of the pulse.
    m_glyphPx = qRound(m_diameter * kGlyphRatio);
    m_glyphPm = glyphPixmap(modeColor(m_mode), m_glyphPx);
}

void PowerButton::enterEvent(QEnterEvent *event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void PowerButton::leaveEvent(QEvent *event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void PowerButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor tint = modeColor(m_mode);
    const QPointF center(width() / 2.0, height() / 2.0);
    const qreal radius = m_diameter / 2.0;
    const QRectF rim(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0);

    // The level the glow is drawn at: the pulse's live value, lifted by hover, dropped while held (a
    // press has to read as the button giving way) and flattened when disabled.
    qreal level = m_glow;
    if (!isEnabled())
        level *= 0.30;
    else if (isDown())
        level *= 0.65;
    else if (m_hovered)
        level *= 1.30;
    level = qBound(0.0, level, 1.5);

    // 1. Bloom. A ring-shaped radial gradient: transparent through the middle, peaking exactly at the
    //    circle's edge, gone kHaloMargin beyond it.
    const qreal bloom = radius + kHaloMargin;
    const qreal edge = radius / bloom;   // where the rim falls in gradient units, 0..1
    QRadialGradient halo(center, bloom, center);
    halo.setColorAt(0.0, faded(tint, 0.0));
    halo.setColorAt(qMax(0.0, edge - 0.08), faded(tint, 0.0));
    halo.setColorAt(edge, faded(tint, 0.40 * level));
    halo.setColorAt(qMin(0.999, edge + 0.12), faded(tint, 0.13 * level));
    halo.setColorAt(1.0, faded(tint, 0.0));
    p.setPen(Qt::NoPen);
    p.setBrush(halo);
    p.drawEllipse(center, bloom, bloom);

    // 2. Falloff. Concentric strokes of increasing width and decreasing alpha, all centred on the rim.
    //    The gradient alone reads as a soft disc; these are what make it read as light coming *from* the
    //    ring — and they are why the effect survives being animated: each pass is two float operations.
    p.setBrush(Qt::NoBrush);
    for (int i = kGlowPasses; i >= 1; --i) {
        const qreal t = (i - 0.5) / kGlowPasses;
        QPen pass(faded(tint, 0.14 * level * (1.0 - t) * (1.0 - t)), kRingWidth + i * 4.0);
        p.setPen(pass);
        p.drawEllipse(rim);
    }

    // 3. The disc: a whisper of the accent over the window background — enough to separate the interior
    //    from the surrounding black, not enough to turn the ring into a filled button.
    p.setPen(Qt::NoPen);
    p.setBrush(faded(tint, 0.05 + 0.03 * level));
    p.drawEllipse(rim);

    // 4. The rim, drawn last among the circles so it stays the crispest thing on screen.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(faded(tint, isDown() ? 0.70 : (m_hovered ? 1.0 : 0.88)), kRingWidth));
    p.drawEllipse(rim);

    // 5. Keyboard focus, one ring outside the rim: this dashboard has exactly one tab stop that matters,
    //    and it has to be findable without a mouse.
    if (hasFocus()) {
        QPen focus(faded(Theme::accent(), 0.55), 1, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(focus);
        p.drawEllipse(rim.adjusted(-7, -7, 7, 7));
    }

    // 6. The glyph, tinted with the mode colour and cached by refreshGlyph().
    if (m_glyphPx > 0 && !m_glyphPm.isNull()) {
        const QRectF target(center.x() - m_glyphPx / 2.0, center.y() - m_glyphPx / 2.0,
                            m_glyphPx, m_glyphPx);
        p.drawPixmap(target, m_glyphPm, QRectF(m_glyphPm.rect()));
    }
}

} // namespace zapret
