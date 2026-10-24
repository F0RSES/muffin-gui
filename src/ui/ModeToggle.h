#pragma once

#include <QColor>
#include <QRect>
#include <QWidget>

class QVariantAnimation;

namespace zapret {

// The dashboard's Zapret/WARP segmented pill.
//
// Two segments, one thumb. Clicking a segment (or pressing left/right on the widget) moves the
// thumb to that half and emits modeChanged(). The thumb itself slides on a Motion-paced
// QVariantAnimation rather than snapping: the user needs to see the two halves as the two ends
// of one choice, and the way Qt draws a QButtonGroup of two checkable QPushButtons does not
// make that obvious.
//
// The whole control is painted in paintEvent() for the same reason PowerButton is — a
// QGraphicsDropShadowEffect cannot be clipped to a rounded pill, and the thumb has to breathe
// with the animation rather than have every segment redraw around it.
//
// Design rules the tests hold this to:
//   * two segments, and only two — the "index" a test can reach for is either 0 (Zapret) or
//     1 (Warp), and the geometry helper takes exactly that;
//   * the thumb's target rectangle is derivable from the widget's own bounds, so
//     thumbRectFor(0, bounds) and thumbRectFor(1, bounds) are the whole geometry contract;
//   * colour choice is per-mode, not per-state, so a theme flip cannot make Zapret purple
//     on one run and orange on the next.
class ModeToggle : public QWidget
{
    Q_OBJECT

public:
    enum class Mode
    {
        Zapret,
        Warp
    };
    Q_ENUM(Mode)

    explicit ModeToggle(QWidget *parent = nullptr);
    ~ModeToggle() override;

    Mode mode() const { return m_mode; }
    // Move the thumb. Setting the mode it is already in is a no-op; a change emits modeChanged.
    void setMode(Mode mode);

    // The pill's preferred size — wide enough for two legible segments — exposed so a layout
    // does not have to hardcode the width it wants. Height is fixed to kPillHeightPx.
    static constexpr int kPillWidthPx = 240;
    static constexpr int kPillHeightPx = 40;
    static constexpr int kThumbPaddingPx = 4;

    // The rectangle the thumb should occupy for `index` inside `bounds`. Public and static
    // because the geometry is the whole promise the widget makes: a test can check that the
    // thumb lands where it says without a window, a paint or a Qt platform.
    static QRect thumbRectFor(int index, const QRect &bounds);

    // The colour the thumb wears for each mode. Theme-independent neon so a mode's meaning
    // survives a dark/light flip.
    static QColor colorFor(Mode mode);
    // Same, indexed by segment so paintEvent and tests can share one lookup table.
    static QColor colorForIndex(int index);
    // Segment labels; language-neutral English to match the design of the power button's chip
    // ("CONNECTED"). A translated form would need every test to reach into a catalog.
    static QString labelFor(Mode mode);
    // Whether the label of the segment `index` is the one currently lit.
    static QString labelForIndex(int index);

    // Re-rasterise the pieces whose colour comes from the theme (the pill's border and the
    // unselected segment's text). MainWindow calls this on a theme switch, as it does for
    // every baked-pixmap widget.
    void retheme();

signals:
    void modeChanged(Mode mode);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    QSize sizeHint() const override;

private:
    int indexFromX(int x) const;
    void setModeAnimated(Mode mode);

    Mode m_mode = Mode::Zapret;
    // The thumb's current x offset inside the pill's interior, animated between the two
    // segment positions. 0.0 = Zapret end, 1.0 = Warp end; the animation carries a real pixel
    // offset so paintEvent can read it directly.
    qreal m_thumbOffset = 0.0;
    QVariantAnimation *m_slide = nullptr;
};

} // namespace zapret
