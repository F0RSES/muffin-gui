#pragma once

#include <QtGlobal>

class QWidget;

namespace zapret {

// Whether the interface is allowed to move, and the one spelling of how fast when it does.
//
// Three families of motion live in this application: a page appearing when the rail is pressed, a
// profile row rising into place when the list is rebuilt, and the power button breathing while a run is
// alive. All three answer to one switch, because the reason a user reaches for it is that the whole
// interface feels heavy — not that one particular widget moves.
//
// The durations are here for the same reason the watchdog's range is in Settings: a cadence spelled at
// each call site is a cadence that drifts. A page that fades in 180 ms, a row that rises over 220 ms and
// a button that breathes across 2 s is one design; let every call site pick its own numbers and the
// screen stops reading as though one hand had drawn it.
//
// Nothing here is stateful per widget: a fade, a rise and a breath are all "put a QGraphicsOpacityEffect
// on it and drive that effect's opacity", with the rise adding a `pos` animation beside it and the breath
// looping instead of ending. The consequence is that turning motion off lands everything at once,
// including a breath that has no natural end.
class Motion
{
public:
    // ---- the cadences, spelled once ----
    // A page appears rather than arrives: long enough to read as movement, short enough that nobody
    // waits on it. Somewhere around 180 ms is where a fade stops looking like a lag.
    static constexpr int kPageFadeMs = 180;
    // A profile row rises into its place. Longer than the page because it carries distance with it, and
    // a distance that crosses the screen faster than the eye can follow reads as a jump, not a slide.
    static constexpr int kCardRiseMs = 220;
    // How much later each successive row starts. Enough to read the list as arriving in order; the
    // cascade is capped by kCardRiseRows, so the total never multiplies out into seconds.
    static constexpr int kCardStepMs = 35;
    // How far below its resting place a row starts. A fraction of the row's own height — more than that
    // and the list visibly reflows instead of appearing.
    static constexpr int kCardLiftPx = 14;
    // The most rows one entrance will animate. Sixty presets in a folder is not a reason to hold sixty
    // effects and a hundred and twenty animations in memory, and a row below the fold cannot be seen
    // rising anyway.
    static constexpr int kCardRiseRows = 8;
    // The power button's "still alive" breath: one slow cycle from opaque down to this and back, run
    // while a profile is up or coming down.
    static constexpr int kBreathMs = 2000;
    static constexpr qreal kBreathFloor = 0.6;

    // Motion is on by default: a first launch that moved like a slideshow would be a different design
    // from the one that was drawn, and the setting exists to opt OUT of it, not into it.
    static bool enabled();
    // Turns motion off — and lands everything already in flight when it does, because an animation that
    // keeps running after the box was unticked is the opposite of what the box just said.
    static void setEnabled(bool on);

    // Fade `widget` in, from nothing to fully opaque: the way a page of the main stack arrives.
    //
    // The animated property is QGraphicsOpacityEffect::opacity rather than QWidget::windowOpacity, which
    // is what the design asks for in spirit and what actually works: windowOpacity is a WINDOW property,
    // so animating it on a widget inside a layout fades the whole application window instead of the page
    // inside it, and does nothing at all to a child whose window is opaque.
    //
    // The effect is detached again when the fade ends. An attached QGraphicsOpacityEffect routes every
    // later paint of that widget through the raster compositor, so a page that had faded in once would go
    // on paying for it for the rest of the session — precisely the cost the settings switch exists to
    // avoid.
    static void fadeIn(QWidget *widget, int durationMs = kPageFadeMs);

    // Rise a row into its place: starting `delayMs` from now, over `durationMs`, from kCardLiftPx below
    // the position its layout gave it, fading in as it travels.
    //
    // The row is made invisible before its first pixel rather than after it. Where a layout will put a
    // widget is not known until the layout has run, so a row drawn at its resting place that then dropped
    // and climbed back would read as a glitch rather than as an entrance.
    static void riseIn(QWidget *widget, int delayMs = 0, int durationMs = kCardRiseMs);

    // Put an endless opacity breath on `widget` — the "this process is alive" pulse. Installing it
    // replaces any effect the widget already had, so a widget that owns one of its own (a painted halo,
    // a drop shadow) should keep that elsewhere: PowerButton draws its glow inside paintEvent() for
    // exactly this reason, and only the breath lives here.
    static void startBreath(QWidget *widget, qreal floor = kBreathFloor, int periodMs = kBreathMs);
    static void stopBreath(QWidget *widget);
    static bool isBreathing(QWidget *widget);
    // The opacity the breath is holding the widget at right now: 1.0 when nothing is breathing it.
    static qreal breathLevel(QWidget *widget);

    // Is Motion still moving this widget? A test's question: "the animation was started" and "the
    // animation is running" are different claims, and only the second is something a user can see.
    static bool isMoving(QWidget *widget);

private:
    Motion() = delete;
};

} // namespace zapret
