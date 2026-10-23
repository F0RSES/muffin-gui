#include "PingIndicator.h"

#include "LanguageManager.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace zapret {

namespace {

constexpr int kRingDiameter = 50;     // the badge's circle, as specified
constexpr int kRingGlowMargin = 12;   // the halo's room; the widget is larger than the ring it shows
constexpr int kRingStroke = 2;        // 2 px, the same weight as the power button's rim
constexpr int kMarkPx = 22;           // the service glyph inside the ring
constexpr int kClockPx = 12;          // the clock glyph beside the latency figure
constexpr int kBadgeWidth = 88;       // wide enough for "000" plus a name like "Discord"

// A colour with its alpha scaled by `factor` — the whole glow is one hue at several alphas.
QColor faded(const QColor &c, qreal factor)
{
    QColor out = c;
    out.setAlphaF(c.alphaF() * qBound(0.0, factor, 1.0));
    return out;
}

} // namespace

// ---- the ring ----

// The badge's circle: a 2 px rim in the status colour, a soft halo outside it, and the service's mark
// floating in the middle in the theme's mark colour (Theme::markOnRing()).
//
// Painted rather than styled for the same reason as PowerButton: QSS cannot draw a glow that is clipped
// to a circle, and the "alive" breath has to animate the halo alone — an opacity effect on the whole
// badge would fade the number and the name with it.
class PingRing : public QWidget
{
public:
    explicit PingRing(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedSize(kRingDiameter + 2 * kRingGlowMargin, kRingDiameter + 2 * kRingGlowMargin);

        m_pulse = new QVariantAnimation(this);
        m_pulse->setDuration(2000);
        m_pulse->setStartValue(1.0);
        m_pulse->setKeyValueAt(0.5, 0.40);
        m_pulse->setEndValue(1.0);
        m_pulse->setEasingCurve(QEasingCurve::InOutSine);
        m_pulse->setLoopCount(-1);
        connect(m_pulse, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            setGlow(v.toReal());
        });
    }

    void setColor(const QColor &color)
    {
        if (color == m_color)
            return;
        m_color = color;
        update();
    }

    QColor color() const { return m_color; }

    void setMark(const QPixmap &pm)
    {
        m_mark = pm;
        update();
    }

    void setGlow(qreal level)
    {
        const qreal clamped = qBound(0.0, level, 1.0);
        if (qFuzzyCompare(clamped, m_glow))
            return;
        m_glow = clamped;
        update();
    }

    qreal glow() const { return m_glow; }

    void setPulsing(bool on)
    {
        if (on) {
            if (m_pulse->state() != QAbstractAnimation::Running) {
                m_glow = 1.0;
                m_pulse->start();
            }
        } else {
            m_pulse->stop();
            m_glow = 1.0;
            update();
        }
    }

    bool isPulsing() const { return m_pulse->state() == QAbstractAnimation::Running; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QPointF center(width() / 2.0, height() / 2.0);
        const qreal radius = kRingDiameter / 2.0;
        const QRectF rim(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0);

        // Bloom, then two falloff passes, then the rim: the same three-layer recipe as the power button,
        // which is what makes a 50 px badge and a 200 px button look like one design.
        const qreal bloom = radius + kRingGlowMargin;
        const qreal edge = radius / bloom;
        QRadialGradient halo(center, bloom, center);
        halo.setColorAt(0.0, faded(m_color, 0.0));
        halo.setColorAt(qMax(0.0, edge - 0.10), faded(m_color, 0.0));
        halo.setColorAt(edge, faded(m_color, 0.38 * m_glow));
        halo.setColorAt(qMin(0.999, edge + 0.16), faded(m_color, 0.10 * m_glow));
        halo.setColorAt(1.0, faded(m_color, 0.0));
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(center, bloom, bloom);

        p.setBrush(Qt::NoBrush);
        for (int i = 2; i >= 1; --i) {
            const qreal t = (i - 0.5) / 2.0;
            p.setPen(QPen(faded(m_color, 0.18 * m_glow * (1.0 - t) * (1.0 - t)),
                          kRingStroke + i * 3.0));
            p.drawEllipse(rim);
        }

        p.setPen(Qt::NoPen);
        p.setBrush(faded(m_color, 0.07 + 0.04 * m_glow));
        p.drawEllipse(rim);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(faded(m_color, 0.95), kRingStroke));
        p.drawEllipse(rim);

        // The mark stays white whatever the status: the ring is the signal, the glyph is only identity.
        if (!m_mark.isNull()) {
            const QRectF target(center.x() - kMarkPx / 2.0, center.y() - kMarkPx / 2.0,
                                kMarkPx, kMarkPx);
            p.drawPixmap(target, m_mark, QRectF(m_mark.rect()));
        }
    }

private:
    QVariantAnimation *m_pulse = nullptr;
    QPixmap m_mark;
    QColor m_color{Theme::pingUnknown()};
    qreal m_glow = 1.0;
};

// ---- the badge ----

PingIndicator::PingIndicator(const QString &serviceName, QWidget *parent)
    : QWidget(parent)
    , m_name(serviceName)
{
    m_ring = new PingRing(this);

    // The latency figure is the one number on this screen that must not shift sideways as it changes
    // digits, so it is monospace by construction rather than by theme. It is sized in POINTS (matching the
    // sheet's base `QWidget` rule), not a frozen device-pixel size, so a HiDPI font setting scales it with
    // the rest of the UI. The family then has to be pinned with an inline sheet: the generic
    // `QWidget { font-family: … }` rule outranks setFont() and would otherwise drop the figure back to the
    // proportional UI face — the exact digit-drift this is here to prevent.
    QFont mono = Theme::monoFont(9);
    mono.setBold(true);

    m_numberLabel = new QLabel(this);
    m_numberLabel->setObjectName(QStringLiteral("pingNumber"));
    m_numberLabel->setFont(mono);
    Theme::applyFontViaSheet(m_numberLabel, mono);
    m_numberLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_clockLabel = new QLabel(this);
    m_clockLabel->setObjectName(QStringLiteral("pingClock"));
    m_clockLabel->setFixedSize(kClockPx, kClockPx);
    m_clockLabel->setScaledContents(true);
    m_clockLabel->setPixmap(Theme::icon(QStringLiteral("clock"), Theme::pingMuted(), kClockPx)
                                .pixmap(kClockPx, kClockPx));
    m_clockLabel->setAttribute(Qt::WA_TransparentForMouseEvents);

    m_nameLabel = new QLabel(m_name, this);
    m_nameLabel->setObjectName(QStringLiteral("pingLabel"));
    m_nameLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(4);
    top->addStretch(1);
    top->addWidget(m_numberLabel, 0, Qt::AlignBottom);
    top->addWidget(m_clockLabel, 0, Qt::AlignBottom);
    top->addStretch(1);

    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(8);
    box->addLayout(top);
    box->addWidget(m_ring, 0, Qt::AlignHCenter);
    box->addWidget(m_nameLabel, 0, Qt::AlignHCenter);

    setFixedWidth(kBadgeWidth);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    m_glyph = glyphForService(m_name);
    refreshGlyph();
    setToolTip(summary());
    applyStatus(PingStatus::Unknown, -1);
}

PingIndicator::~PingIndicator()
{
    if (m_ring)
        m_ring->setPulsing(false);
}

QColor PingIndicator::dotColor(PingStatus status)
{
    switch (status)
    {
    case PingStatus::Waiting: return Theme::pingWaiting();
    case PingStatus::Ok:      return Theme::pingOk();
    case PingStatus::Fail:    return Theme::pingFail();
    case PingStatus::Unknown: break;
    }
    return Theme::pingUnknown();
}

PingIndicator::PingStatus PingIndicator::statusFromServiceInt(int status)
{
    switch (status)
    {
    case 0: return PingStatus::Waiting;
    case 1: return PingStatus::Ok;
    case 2: return PingStatus::Fail;
    default: return PingStatus::Unknown;
    }
}

QString PingIndicator::latencyText(PingStatus status, int latencyMs)
{
    switch (status)
    {
    case PingStatus::Waiting:
        return QStringLiteral("…");
    case PingStatus::Ok:
        if (latencyMs >= 0)
            return LanguageManager::tr(QStringLiteral("ping.ms")).arg(latencyMs);
        return QStringLiteral("ok");
    case PingStatus::Fail:
        return LanguageManager::tr(QStringLiteral("ping.timeout"));
    case PingStatus::Unknown:
        break;
    }
    return QStringLiteral("—");
}

QString PingIndicator::latencyNumber(PingStatus status, int latencyMs)
{
    switch (status)
    {
    case PingStatus::Waiting:
        return QStringLiteral("…");
    case PingStatus::Ok:
        if (latencyMs >= 0)
            return QString::number(latencyMs);
        return QStringLiteral("ok");
    case PingStatus::Fail:
        // "!" rather than "0": a timeout has no latency to show, and a zero would read as an
        // implausibly fast reply.
        return QStringLiteral("!");
    case PingStatus::Unknown:
        break;
    }
    return QStringLiteral("—");
}

QString PingIndicator::summary() const
{
    return QStringLiteral("%1 %2").arg(m_name, latencyText(m_status, m_latencyMs));
}

QString PingIndicator::glyphForService(const QString &serviceName)
{
    const QString n = serviceName.trimmed().toLower();
    if (n.contains(QStringLiteral("youtube")))
        return QStringLiteral("youtube");
    if (n.contains(QStringLiteral("discord")))
        return QStringLiteral("discord");
    return QStringLiteral("globe");
}

void PingIndicator::setServiceName(const QString &name)
{
    m_name = name;
    m_nameLabel->setText(m_name);
    // A new service is a new identity, so the mark follows the name — unless setGlyph() pinned one.
    setGlyph(glyphForService(m_name));
    setToolTip(summary());
}

void PingIndicator::setGlyph(const QString &glyph)
{
    if (m_glyph == glyph)
        return;
    m_glyph = glyph;
    refreshGlyph();
    update();
}

void PingIndicator::refreshGlyph()
{
    // The mark takes the theme's mark colour rather than a fixed white: on the light sheet the ring
    // sits on a #FFFFFF page and a white glyph there is simply not there. See Theme::markOnRing().
    m_ring->setMark(Theme::icon(m_glyph, Theme::markOnRing(), kMarkPx).pixmap(kMarkPx, kMarkPx));
}

// A theme switch cannot repaint a baked pixmap the sheet never owned, so the ring's status colour, its
// mark and the figure beside it are all re-derived here. MainWindow::applyTheme() walks the badges on
// exactly this signal.
void PingIndicator::retheme()
{
    refreshGlyph();
    applyStatus(m_status, m_latencyMs);
}

bool PingIndicator::isPulsing() const
{
    return m_ring && m_ring->isPulsing();
}

void PingIndicator::setStatus(PingStatus status, int latencyMs)
{
    applyStatus(status, latencyMs);
}

void PingIndicator::applyStatus(PingStatus status, int latencyMs)
{
    m_status = status;
    m_latencyMs = latencyMs;

    m_ring->setColor(dotColor(status));
    m_ring->setPulsing(status == PingStatus::Ok);
    m_numberLabel->setText(latencyNumber(status, latencyMs));
    setToolTip(summary());

    update();
}

} // namespace zapret

