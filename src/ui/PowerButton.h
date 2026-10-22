#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QPixmap>
#include <QString>
#include <QtGui/qevent.h>

class QVariantAnimation;

namespace zapret {

// The dashboard's one and only control: a large neon circle that starts the strategy, or stops the one
// that is running.
//
// Everything about it is painted rather than styled. A QGraphicsDropShadowEffect would give the halo but
// cannot be clipped to a circle (Qt renders the effect over the widget's bounding rectangle, so a square
// shadow bleeds out of a round button), it forces the widget through the raster paint engine, and it
// cannot animate — the "busy" pulse has to breathe with the button, not redraw a static blur. So the
// glow is drawn here instead: one radial gradient for the bloom, a few widening translucent strokes for
// the falloff, and a crisp ring on top.
//
// The widget is larger than the circle it shows: the halo needs the margin around it to have somewhere
// to fade into, and a clipped widget would cut the bloom off as a hard square edge.
class PowerButton : public QAbstractButton
{
    Q_OBJECT

public:
    // Which of the three looks the button wears. These are presentation states, not backend states:
    // MainWindow maps IZapretBackend::State onto them, so the button never has to know what a backend
    // can be in the middle of doing.
    enum class Mode
    {
        Idle,     // nothing running — the press will start
        Active,   // a live run — the press will stop
        Busy      // a transition is in flight — the halo breathes, the press does nothing
    };
    Q_ENUM(Mode)

    explicit PowerButton(QWidget *parent = nullptr);
    ~PowerButton() override;

    // The circle's diameter in px; the widget reserves kHaloMargin of its own space on every side for
    // the bloom, so the total size is diameter + 2 * kHaloMargin.
    void setDiameter(int px);
    int diameter() const { return m_diameter; }

    // The glyph drawn in the middle, named after :/icons/<name>.svg. Tinted with the mode colour, so it
    // is re-rasterised whenever the mode changes rather than recoloured at paint time.
    void setGlyph(const QString &name);
    QString glyph() const { return m_glyph; }

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    // The "this run is alive" breath: the whole button fading between fully opaque and Motion's floor
    // and back, on Motion's period. MainWindow turns it on for exactly the two states in which the
    // firewall is doing something — a run that is up, and a teardown that is under way — and off for
    // the third, which is the "Отключено" the pulse must not contradict.
    //
    // It is asked for rather than derived from Mode on purpose: Mode::Busy covers a launch as well as a
    // teardown, and "coming up" and "coming down" are different claims about the run.
    void setPulsing(bool on);

    // Whether the breath is actually running: the request AND the policy, so an idle button, a stopped
    // machine or a hidden widget all answer false. Public because a test can only prove the pulse moves
    // if it can ask whether the pulse is there.
    bool isPulsing();

    // Re-rasterise the glyph in the current theme's colours. Theme::icon() bakes its tint into a pixmap,
    // so a theme switch cannot repaint one — the window has to ask for a fresh draw.
    void retheme();

    // The halo's brightness, 0.0–1.0. Only the busy pulse writes this; it is public so a test can
    // assert the pulse is actually moving rather than merely started.
    void setGlow(qreal level);
    qreal glow() const { return m_glow; }

    // The ring/halo/glyph colour for a mode. Theme-independent neon: purple to act, green to stop.
    static QColor modeColor(Mode mode);

    // How much room the bloom gets on each side of the circle.
    static constexpr int kHaloMargin = 38;

protected:
    void paintEvent(QPaintEvent *event) override;
    // Hover has to repaint: the ring brightens under the cursor, and unlike :pressed — which
    // QAbstractButton::setDown() already announces with update() — entering and leaving are state this
    // class tracks itself.
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    // The button breathes only while it can be seen. It lives on the dashboard, so every other page the
    // rail opens hides it — and an endless animation over a hidden widget is work spent on frames
    // nobody will look at.
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void startPulse();
    void stopPulse();
    void startBreath();
    void stopBreath();
    // One place decides which of the two rhythms runs, so a mode change, a state change and the motion
    // switch cannot each leave their own idea of "is something moving right now" behind.
    void reconcileMotion();
    void refreshGlyph();
    QPixmap glyphPixmap(const QColor &tint, int px) const;

    QVariantAnimation *m_pulse = nullptr;
    bool m_pulsing = false;     // the caller's request; reconcileMotion() turns it into a running breath
    QString m_glyph;
    QPixmap m_glyphPm;          // the glyph rasterised at m_glyphPx in the current mode's tint
    int m_glyphPx = 0;
    qreal m_glow = 1.0;
    int m_diameter = 200;
    Mode m_mode = Mode::Idle;
    bool m_hovered = false;
};

} // namespace zapret
