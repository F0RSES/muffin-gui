#include "PowerButton.h"

#include "Motion.h"
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
    // Both rhythms die with this widget: the halo animation is parented where it is created, and
    // Motion's breath run is parented to the widget it moves and unregisters itself in its own
    // destructor — so the breath needs no call here at all. The stop below is the older half of the
    // contract: it keeps a frame that was already queued from writing a glow level into a button that
    // is partway through being destroyed.
    if (m_pulse)
        m_pulse->stop();
}

QColor PowerButton::modeColor(Mode mode)
{
    return modeColor(mode, Palette::Zapret);
}

QColor PowerButton::modeColor(Mode mode, Palette palette)
{
    // Warp's colours are Cloudflare's own registered palette rather than a re-tint of the app's
    // accents: a user who has ever seen the WARP tray icon should recognise that the green
    // they know from zapret means something different in this mode. The values below match the
    // brand assets as published on 2024-11 — "Cloudflare Orange" #F6821F and a lighter
    // "Cloudflare Blue" #22A7F0 for the "up and passing traffic" state.
    const QColor warpIdle(0xF6, 0x82, 0x1F);
    const QColor warpActive(0x22, 0xA7, 0xF0);
    switch (mode) {
    case Mode::Active:
        if (palette == Palette::Warp)
            return warpActive;
        // Green: the same token the "CONNECTED" chip and the live ping rings are drawn with, so
        // one colour means one thing on this screen. Purple-to-green is the difference, at a
        // glance, between "press me" and "press me and your traffic goes down".
        return Theme::pingOk();
    case Mode::Idle:
    case Mode::Busy:
        break;
    }
    if (palette == Palette::Warp)
        return warpIdle;
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
    reconcileMotion();
    update();
}

void PowerButton::setPalette(Palette palette)
{
    if (m_palette == palette)
        return;
    m_palette = palette;
    // The glyph is baked in the mode's colour, so a palette flip cannot just repaint the ring
    // — the tint has to be re-rasterised or the icon keeps the previous palette's hue.
    refreshGlyph();
    update();
}

void PowerButton::setPulsing(bool on)
{
    m_pulsing = on;
    // Reconciled unconditionally rather than returned from early when the request did not change: this
    // is also how the button re-reads the motion policy after the settings box was moved, and the two
    // calls have to look the same from here.
    reconcileMotion();
    update();
}

bool PowerButton::isPulsing()
{
    return m_pulsing && Motion::isBreathing(this);
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

// Which of the two rhythms runs.
//
// Only one of them at a time, because two cycles of different lengths on the same circle beat against
// each other and read as a flicker rather than as a pulse: the halo breathes through a transition that
// has no run to report yet, and the whole button breathes while there is one — the request the window
// makes, and the policy the settings box decides.
void PowerButton::reconcileMotion()
{
    // isHidden(), not isVisible(): the widget's own flag, which stays true until something has actually
    // been put on screen. A window hidden to the tray, a dashboard behind another page and a button
    // constructed but never shown all answer "hidden", and none of them is worth animating; showEvent()
    // and hideEvent() below are what re-run this on each of those transitions.
    const bool motion = Motion::enabled() && !isHidden();
    if (motion && m_pulsing)
        startBreath();
    else
        stopBreath();
    if (motion && !m_pulsing && m_mode == Mode::Busy)
        startPulse();
    else
        stopPulse();
}

// The breath is the one place this widget lets an effect own a piece of its appearance, and it is the
// piece that has to be an effect: fading the ring, its bloom and its glyph by the same factor is exactly
// what "the whole button is dimming" means, and multiplying one paintEvent's alphas would have to be
// written into every layer of that paint separately. Motion detaches the effect again when the breath is
// stopped, so a button that is not pulsing keeps the fast paint path the rest of this class is drawn for.
void PowerButton::startBreath()
{
    Motion::startBreath(this, Motion::kBreathFloor, Motion::kBreathMs);
}

void PowerButton::stopBreath()
{
    Motion::stopBreath(this);
}

QPixmap PowerButton::glyphPixmap(const QColor &tint, int px) const
{
    if (m_glyph.isEmpty() || px <= 0)
        return QPixmap();
    return Theme::icon(m_glyph, tint, px).pixmap(px, px);
}

void PowerButton::refreshGlyph()
{
    // Rasterised once per (glyph, mode, palette, size) rather than per paint: Theme::icon()
    // rewrites and reloads an SVG document, which is far too much work to repeat on every
    // frame of the pulse.
    m_glyphPx = qRound(m_diameter * kGlyphRatio);
    m_glyphPm = glyphPixmap(modeColor(m_mode, m_palette), m_glyphPx);
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

// Coming back on screen restarts whatever the state asked for; going off screen stops it. The button
// lives on the dashboard, so opening the journal or the history hides it, and an endless pulse over a
// hidden widget is frames spent on a paint nobody will look at — the same argument the settings switch
// makes about the whole interface, applied to one control.
void PowerButton::showEvent(QShowEvent *event)
{
    QAbstractButton::showEvent(event);
    reconcileMotion();
}

void PowerButton::hideEvent(QHideEvent *event)
{
    // The request survives: what is parked is the animation, not the state that asked for it.
    stopBreath();
    stopPulse();
    QAbstractButton::hideEvent(event);
}

void PowerButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor tint = modeColor(m_mode, m_palette);
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
