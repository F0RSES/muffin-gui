#include "Motion.h"

#include <QAbstractAnimation>
#include <QGraphicsOpacityEffect>
#include <QList>
#include <QParallelAnimationGroup>
#include <QPointer>
#include <QPropertyAnimation>
#include <QSequentialAnimationGroup>
#include <QWidget>

namespace zapret {

namespace {

// Whether the interface may move. One writer — Motion::setEnabled, fed from Settings by the window — and
// many readers, so the checkbox in the settings page is the whole policy rather than a hint each widget
// interprets for itself.
bool g_enabled = true;

// What a run is doing. The kind matters in two places: only one Page may be fading at a time (a
// QStackedWidget shows one page, so a fade left running on the page the user just abandoned is invisible
// work holding a compositor layer open), and only a Row has been moved, so only a Row has a position to
// put back.
enum class Kind
{
    Page,     // a page of the main stack, fading in
    Row,      // one row of the profile list, rising into place
    Breath    // an endless opacity pulse on a control that is still doing something
};

// One widget's worth of motion: the effect it installed and the animation — or group of them — driving it.
//
// One lead rather than a list, because Qt 6 removed QAbstractAnimation::setStartDelay: the stagger a
// cascade needs is a QPauseAnimation in front of a QParallelAnimationGroup, and the thing whose
// finished() ends the run is that outer group. It is also the honest shape — a run is over when the one
// animation it started is over, not when N of M have reported in.
//
// Parented to the widget it moves, so it cannot outlive it. That is load-bearing rather than tidy:
// populateCards() deletes and rebuilds every row, and an animation that survived its widget would be a
// crash whose stack names nothing.
//
// No Q_OBJECT: this declares no signal and no slot. It only listens to the animation it owns, and the
// functor form of connect() works on a plain QObject subclass.
class MotionRun : public QObject
{
public:
    // `rest` is where the widget's layout wants it. A fade and a breath never move anything, so they
    // pass the position the widget is already at and it is simply never used.
    MotionRun(QWidget *widget, Kind kind, QGraphicsOpacityEffect *effect, const QPoint &rest,
              QAbstractAnimation *lead)
        : QObject(widget)
        , m_kind(kind)
        , m_widget(widget)
        , m_effect(effect)
        , m_rest(rest)
        , m_lead(lead)
    {
        m_lead->setParent(this);
        // A looping lead never reports finished(), so a breath is landed only by stopBreath() or by the
        // motion switch — never by the clock running out, because it does not run out.
        if (m_lead->loopCount() >= 0)
            connect(m_lead, &QAbstractAnimation::finished, this, [this] { land(); });
        registry().append(QPointer<MotionRun>(this));
    }

    ~MotionRun() override;

    void start() { m_lead->start(); }

    Kind kind() const { return m_kind; }
    QWidget *widget() const { return m_widget; }
    bool isLanded() const { return m_landed; }
    QGraphicsOpacityEffect *effect() const { return m_effect; }

    bool running() const
    {
        // Staged counts as running: a row waiting behind its pause is already invisible and already
        // committed to rising, so calling it still would be an answer a test then contradicts by
        // watching it move.
        return !m_landed && m_lead && m_lead->state() != QAbstractAnimation::Stopped;
    }

    // The opacity the run is holding its widget at. 1.0 once the effect is gone, so a landed or deleted
    // run can never report an invisible widget.
    qreal level() const { return m_effect ? m_effect->opacity() : 1.0; }

    // Stop, leave the widget still and fully opaque with its position back where the layout put it, and
    // take the effect off. Idempotent, and reached both when the animation runs to its end and when it
    // is cut short — by the motion switch, by a newer entrance of the same kind, or by the list being
    // rebuilt under a row that was still rising.
    void land()
    {
        if (m_landed)
            return;
        m_landed = true;
        // The signals go first: QAbstractAnimation::stop() emits finished(), and this is the very
        // handler being run down. m_landed above already makes the re-entry harmless; blocking keeps it
        // from happening at all.
        if (m_lead) {
            m_lead->blockSignals(true);
            m_lead->stop();
            m_lead->blockSignals(false);
        }
        if (m_widget) {
            // A row that rose has been moved by hand, and a widget in a layout that was moved by hand
            // and never moved back stays in the wrong place until the next resize silently corrects it.
            if (m_kind == Kind::Row)
                m_widget->move(m_rest);
            // Only detach the effect this run installed: setGraphicsEffect() deletes whatever was there,
            // so a finishing fade must not be able to take off a breath that started after it.
            if (m_effect && m_widget->graphicsEffect() == m_effect)
                m_widget->setGraphicsEffect(nullptr);
        }
        m_effect = nullptr;
        deleteLater();
    }

    // The runs in flight. QPointer because a run is a child of the widget it moves and can therefore
    // vanish at any moment, and the registry must never hand back a pointer the widget took away. A
    // member rather than a file-static so the type it names is the class's own.
    static QList<QPointer<MotionRun>> &registry()
    {
        static QList<QPointer<MotionRun>> list;
        return list;
    }

    static MotionRun *forWidget(QWidget *widget)
    {
        if (!widget)
            return nullptr;
        for (const QPointer<MotionRun> &run : registry()) {
            if (run && !run->isLanded() && run->widget() == widget)
                return run.data();
        }
        return nullptr;
    }

    static void landAll()
    {
        // A copy is walked because landing schedules a deleteLater(); the list itself only changes when
        // those deletions run, but a walk that could see entries drop out is a walk that has to be
        // re-reasoned about the next time this file changes.
        const QList<QPointer<MotionRun>> all = registry();
        for (const QPointer<MotionRun> &run : all) {
            if (run && !run->isLanded())
                run->land();
        }
    }

    static void landKind(Kind kind)
    {
        const QList<QPointer<MotionRun>> all = registry();
        for (const QPointer<MotionRun> &run : all) {
            if (run && !run->isLanded() && run->kind() == kind)
                run->land();
        }
    }

private:
    Kind m_kind;
    QPointer<QWidget> m_widget;               // null once the widget is gone
    QPointer<QGraphicsOpacityEffect> m_effect;
    QPoint m_rest;                            // the position the widget's layout gave it
    QPointer<QAbstractAnimation> m_lead;
    bool m_landed = false;
};

MotionRun::~MotionRun()
{
    QList<QPointer<MotionRun>> &all = registry();
    for (int i = 0; i < all.size(); ++i) {
        if (all.at(i) == this) {
            all.removeAt(i);
            break;
        }
    }
}

// The effect every run starts from, installed now rather than at the animation's first frame: the widget
// stops being drawn (or goes fully opaque, for a breath) the moment the motion is asked for, which is the
// difference between appearing and flickering.
QGraphicsOpacityEffect *attachFaded(QWidget *widget, qreal opacity)
{
    auto *effect = new QGraphicsOpacityEffect(widget);
    effect->setOpacity(opacity);
    widget->setGraphicsEffect(effect);
    return effect;
}

// An animation over an effect's opacity. Unparented: the run adopts its lead, and anything the lead owns
// (a parallel group, a pause in front of it) is deleted with it.
QPropertyAnimation *opacityAnim(QGraphicsOpacityEffect *effect, qreal from, qreal to, int durationMs)
{
    auto *anim = new QPropertyAnimation(effect, "opacity");
    anim->setStartValue(from);
    anim->setEndValue(to);
    anim->setDuration(qMax(1, durationMs));
    return anim;
}

// A widget its container has hidden has nothing to appear on, so it is not worth a compositor layer.
//
// isHidden() is the narrowest true statement of that, and it is measured rather than assumed: a widget
// that has never been shown reports hidden as well, which is the behaviour wanted here — nothing animates
// on screen until something is on screen — and the reason the suite's fixtures call show() before asking
// for an entrance. It is false for a widget that has been shown and not hidden since, including the
// current page of a QStackedWidget, which the stack un-hides on arrival.
bool canBeSeen(const QWidget *widget)
{
    return widget && !widget->isHidden();
}

// The tests no caller may skip, and the effect they all install: refuse a hidden widget, refuse
// anything while motion is off, and land whatever was already moving this widget before stacking a second
// effect on it. A null effect is the entire settings switch for the entry points below.
QGraphicsOpacityEffect *prepareRun(QWidget *widget, qreal fromOpacity)
{
    if (!canBeSeen(widget) || !g_enabled)
        return nullptr;
    // An effect is a per-widget resource: installing a second one deletes the first, and a run whose
    // effect has been replaced no longer describes anything. Land it before installing over it.
    if (MotionRun *previous = MotionRun::forWidget(widget))
        previous->land();
    return attachFaded(widget, fromOpacity);
}

// The same refusal for a widget that is already breathing. Without it a breath would restart from the
// top of its cycle every time a state change re-drew the button, and renderStatus() runs for far more
// than the state — the pulse would stutter on screen instead of breathing.
bool alreadyBreathing(QWidget *widget)
{
    MotionRun *run = MotionRun::forWidget(widget);
    return run && run->kind() == Kind::Breath && run->running();
}

} // namespace

// ---- the switch ------------------------------------------------------------

bool Motion::enabled()
{
    return g_enabled;
}

// Turning motion off has to be visible at once rather than when whatever is mid-flight happens to land:
// the box was unticked because this machine is too slow for the movement, so the frames it has left are
// the last thing the user wants to spend.
void Motion::setEnabled(bool on)
{
    if (g_enabled == on)
        return;
    g_enabled = on;
    if (!on)
        MotionRun::landAll();
}

// ---- a page appearing ------------------------------------------------------

void Motion::fadeIn(QWidget *widget, int durationMs)
{
    QGraphicsOpacityEffect *effect = prepareRun(widget, 0.0);
    if (!effect)
        return;
    // One page at a time: a QStackedWidget shows exactly one, so a fade still running on the page the
    // user just left is invisible work holding a compositor layer open.
    MotionRun::landKind(Kind::Page);

    QPropertyAnimation *fade = opacityAnim(effect, 0.0, 1.0, durationMs);
    // Out-cubic: the first third of the change carries most of the movement, which is what lets a short
    // fade read as "appearing" rather than as a lag in front of the content.
    fade->setEasingCurve(QEasingCurve::OutCubic);

    auto *run = new MotionRun(widget, Kind::Page, effect, widget->pos(), fade);
    run->start();
}

// ---- a row rising into place -----------------------------------------------

void Motion::riseIn(QWidget *widget, int delayMs, int durationMs)
{
    // Where the layout wants the row, read before anything is moved. The animation ends on this value
    // and the row is left exactly here: one that had risen and stayed parked 14 px low until the next
    // resize corrected it would be the entrance quietly breaking the list.
    const QPoint rest = widget ? widget->pos() : QPoint();
    QGraphicsOpacityEffect *effect = prepareRun(widget, 0.0);
    if (!effect)
        return;

    // Hidden before its first pixel and dropped at the same moment, so a row is never drawn at its
    // resting place only to fall out of it.
    const QPoint below = rest + QPoint(0, kCardLiftPx);
    widget->move(below);

    QPropertyAnimation *fade = opacityAnim(effect, 0.0, 1.0, durationMs);
    fade->setEasingCurve(QEasingCurve::OutCubic);

    auto *rise = new QPropertyAnimation(widget, "pos");
    rise->setStartValue(below);
    rise->setEndValue(rest);
    rise->setDuration(qMax(1, durationMs));
    rise->setEasingCurve(QEasingCurve::OutCubic);

    auto *together = new QParallelAnimationGroup;
    together->addAnimation(fade);
    together->addAnimation(rise);

    // The stagger is a pause in front of the pair rather than a delay on one of them — Qt 6 removed the
    // per-animation start delay, and a slide that arrived before its own fade would be a row dragging a
    // visible tail across the list. Every row still takes the same time to rise, so a list arrives as a
    // column instead of all at once.
    QAbstractAnimation *lead = together;
    if (delayMs > 0) {
        auto *cascade = new QSequentialAnimationGroup;
        cascade->addPause(delayMs);
        cascade->addAnimation(together);
        lead = cascade;
    }

    auto *run = new MotionRun(widget, Kind::Row, effect, rest, lead);
    run->start();
}

// ---- the breath of a control that is still doing something ------------------

void Motion::startBreath(QWidget *widget, qreal floor, int periodMs)
{
    if (alreadyBreathing(widget))
        return;
    // From fully opaque rather than from nothing: a breath sits over a widget the user is already
    // looking at, and blanking it to bring an effect up would be the pulse stuttering once before it
    // settled into its rhythm.
    QGraphicsOpacityEffect *effect = prepareRun(widget, 1.0);
    if (!effect)
        return;

    QPropertyAnimation *breath = opacityAnim(effect, 1.0, 1.0, periodMs);
    breath->setKeyValueAt(0.5, qBound(0.0, floor, 1.0));
    breath->setEasingCurve(QEasingCurve::InOutSine);
    // Endless on purpose. A run that outlives the user's attention — a teardown longer than expected, a
    // window hidden in the tray — has to keep saying "still working" rather than settle into what would
    // look like an idle control, so stopping it is the caller's decision and never the clock's.
    breath->setLoopCount(-1);

    auto *run = new MotionRun(widget, Kind::Breath, effect, widget->pos(), breath);
    run->start();
}

void Motion::stopBreath(QWidget *widget)
{
    MotionRun *run = MotionRun::forWidget(widget);
    if (run && run->kind() == Kind::Breath)
        run->land();      // taking the effect off is also what restores full opacity
}

bool Motion::isBreathing(QWidget *widget)
{
    return alreadyBreathing(widget);
}

qreal Motion::breathLevel(QWidget *widget)
{
    MotionRun *run = MotionRun::forWidget(widget);
    if (!run || run->kind() != Kind::Breath)
        return 1.0;
    return run->level();
}

// ---- the question a test asks ----------------------------------------------

bool Motion::isMoving(QWidget *widget)
{
    MotionRun *run = MotionRun::forWidget(widget);
    return run && run->running();
}

} // namespace zapret
