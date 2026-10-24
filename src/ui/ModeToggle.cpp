#include "ModeToggle.h"

#include "Motion.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QVariantAnimation>

namespace zapret {

namespace {

// Duration of the thumb slide. Matches Motion's kCardRiseMs so a segment move reads as the
// same family of motion as a row rising into place; anything slower and the pill feels sticky,
// anything faster and the eye cannot tell which half the thumb just came from.
constexpr int kSlideMs = 220;

// The rounded corner radius the pill is drawn with. Half the height is the fully round case; a
// 40 px pill at 20 px corner is a stadium, which is the shape the design asked for.
constexpr int kPillRadiusPx = ModeToggle::kPillHeightPx / 2;

qreal lerp(qreal a, qreal b, qreal t) { return a + (b - a) * t; }

QColor mixQ(const QColor &a, const QColor &b, qreal t)
{
    QColor out;
    out.setRedF(lerp(a.redF(), b.redF(), t));
    out.setGreenF(lerp(a.greenF(), b.greenF(), t));
    out.setBlueF(lerp(a.blueF(), b.blueF(), t));
    out.setAlphaF(lerp(a.alphaF(), b.alphaF(), t));
    return out;
}

QRectF lerpRect(const QRectF &a, const QRectF &b, qreal t)
{
    return QRectF(lerp(a.x(), b.x(), t), lerp(a.y(), b.y(), t),
                  lerp(a.width(), b.width(), t), lerp(a.height(), b.height(), t));
}

} // namespace

QRect ModeToggle::thumbRectFor(int index, const QRect &bounds)
{
    // The two halves inside the pill's own border. Subtract kThumbPaddingPx on every side of
    // the interior so the thumb reads as an inset element rather than a full-width highlight.
    const QRect interior = bounds.adjusted(kThumbPaddingPx, kThumbPaddingPx,
                                           -kThumbPaddingPx, -kThumbPaddingPx);
    if (interior.width() <= 0 || interior.height() <= 0)
        return QRect();
    const int halfWidth = interior.width() / 2;
    const int x = (index == 0) ? interior.left() : interior.left() + halfWidth;
    const int right = interior.right();
    const int w = (index == 0) ? halfWidth : (right - (interior.left() + halfWidth) + 1);
    return QRect(x, interior.top(), w, interior.height());
}

QColor ModeToggle::colorFor(Mode mode)
{
    // Zapret stays on the dashboard's own accent (purple in the dark sheet) so the mode the app
    // was designed around is the mode whose colour is already everywhere else on the screen.
    // WARP borrows Cloudflare's registered orange so a user who has ever seen the WARP client's
    // own window recognises what they are about to turn on. Deliberately not theme-dependent:
    // "which mode am I in" is a fact about the tunnel, not about the palette.
    if (mode == Mode::Warp)
        return QColor(0xF6, 0x82, 0x1F);   // Cloudflare orange
    return Theme::accent();
}

QColor ModeToggle::colorForIndex(int index)
{
    return colorFor(index == 0 ? Mode::Zapret : Mode::Warp);
}

QString ModeToggle::labelFor(Mode mode)
{
    return (mode == Mode::Warp) ? QStringLiteral("WARP") : QStringLiteral("Zapret");
}

QString ModeToggle::labelForIndex(int index)
{
    return labelFor(index == 0 ? Mode::Zapret : Mode::Warp);
}

ModeToggle::ModeToggle(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(kPillWidthPx, kPillHeightPx);

    m_slide = new QVariantAnimation(this);
    m_slide->setDuration(kSlideMs);
    m_slide->setEasingCurve(QEasingCurve::InOutSine);
    connect(m_slide, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        const qreal next = v.toReal();
        if (qFuzzyCompare(next, m_thumbOffset))
            return;
        m_thumbOffset = next;
        update();
    });
}

ModeToggle::~ModeToggle()
{
    if (m_slide)
        m_slide->stop();
}

QSize ModeToggle::sizeHint() const
{
    return QSize(kPillWidthPx, kPillHeightPx);
}

void ModeToggle::setMode(Mode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    setModeAnimated(mode);
    update();
    emit modeChanged(mode);
}

void ModeToggle::setModeAnimated(Mode mode)
{
    const qreal target = (mode == Mode::Zapret) ? 0.0 : 1.0;
    if (!Motion::enabled()) {
        m_thumbOffset = target;
        return;
    }
    if (m_slide->state() == QAbstractAnimation::Running)
        m_slide->stop();
    m_slide->setStartValue(m_thumbOffset);
    m_slide->setEndValue(target);
    m_slide->start();
}

void ModeToggle::retheme()
{
    update();
}

int ModeToggle::indexFromX(int x) const
{
    const QRect interior = rect().adjusted(kThumbPaddingPx, kThumbPaddingPx,
                                           -kThumbPaddingPx, -kThumbPaddingPx);
    if (interior.width() <= 0)
        return 0;
    return (x < interior.left() + interior.width() / 2) ? 0 : 1;
}

void ModeToggle::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    setMode(indexFromX(event->pos().x()) == 0 ? Mode::Zapret : Mode::Warp);
}

void ModeToggle::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Left) {
        setMode(Mode::Zapret);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Right) {
        setMode(Mode::Warp);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return) {
        setMode(m_mode == Mode::Zapret ? Mode::Warp : Mode::Zapret);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ModeToggle::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRect r = rect();
    // The pill's track: a bordered stadium slightly darker than the surface so the thumb has
    // something to sit on. Reuses the sheet's hover token as the recessed fill for the same
    // reason the log well's dark band matches the code it holds — a control that reads as inset
    // is one that shares its colour with what the surface itself becomes on press.
    const QColor track = Theme::hover();
    const QColor border = Theme::border();
    p.setPen(QPen(border, 1));
    p.setBrush(track);
    p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), kPillRadiusPx, kPillRadiusPx);

    // Interpolate the thumb's rectangle and colour across the slide so the pill actually looks
    // like one control sliding between two halves rather than a highlight that swaps at the
    // halfway mark.
    const QRectF zapretThumb = QRectF(thumbRectFor(0, r));
    const QRectF warpThumb = QRectF(thumbRectFor(1, r));
    const QRectF thumbF = lerpRect(zapretThumb, warpThumb, m_thumbOffset);
    const QColor zapretColor = colorFor(Mode::Zapret);
    const QColor warpColor = colorFor(Mode::Warp);
    const QColor mixed = mixQ(zapretColor, warpColor, m_thumbOffset);

    // The thumb's own fill is a low-alpha wash of that colour rather than the pure tone: on the
    // dark sheet a full-saturation orange under "WARP" white text pushes the label out of
    // legibility, and the mode's colour already carries through to the power button's halo —
    // the pill's job is to say which segment is picked, not to compete with the ring for
    // attention.
    QColor wash = mixed;
    wash.setAlphaF(0.28);
    p.setPen(Qt::NoPen);
    p.setBrush(wash);
    const qreal thumbRadius = qreal(thumbF.height()) / 2.0;
    p.drawRoundedRect(thumbF, thumbRadius, thumbRadius);

    // Segment labels — the one that owns the thumb is drawn in the mode's full colour, the
    // other in muted text so the choice reads before the user even registers the pill's shape.
    QFont f = p.font();
    f.setBold(true);
    p.setFont(f);
    const int halfInterior = (r.width() - 2 * kThumbPaddingPx) / 2;
    QRect zapretTextRect(r.left() + kThumbPaddingPx, r.top(), halfInterior, r.height());
    QRect warpTextRect(r.left() + kThumbPaddingPx + halfInterior, r.top(),
                       r.width() - kThumbPaddingPx - halfInterior, r.height());
    const bool zapretActive = (m_thumbOffset < 0.5);
    p.setPen(zapretActive ? zapretColor : Theme::mutedText());
    p.drawText(zapretTextRect, Qt::AlignCenter, labelFor(Mode::Zapret));
    p.setPen(!zapretActive ? warpColor : Theme::mutedText());
    p.drawText(warpTextRect, Qt::AlignCenter, labelFor(Mode::Warp));
}

} // namespace zapret

