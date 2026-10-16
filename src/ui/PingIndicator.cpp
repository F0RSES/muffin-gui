#include "PingIndicator.h"
#include "Theme.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPropertyAnimation>

namespace zapret {

namespace {
constexpr int kDotDiameter = 12;   // radius 6 px as specified
constexpr int kMinChipWidth = 120;
} // namespace

// ---- the dot ----

class PingDot : public QWidget
{
public:
    explicit PingDot(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedSize(kDotDiameter, kDotDiameter);
    }

    void setColor(const QColor &color)
    {
        if (color == m_color)
            return;
        m_color = color;
        update();
    }

    QColor color() const { return m_color; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(m_color);
        // Inset by half a pixel so the circle never clips at the widget border.
        p.drawEllipse(QRectF(0.5, 0.5, width() - 1.0, height() - 1.0));
    }

private:
    QColor m_color{QColor(0x6B, 0x72, 0x80)};
};

// ---- the chip ----

PingIndicator::PingIndicator(const QString &serviceName, QWidget *parent)
    : QWidget(parent)
    , m_name(serviceName)
{
    m_dot = new PingDot(this);

    m_nameLabel = new QLabel(m_name, this);
    m_nameLabel->setObjectName(QStringLiteral("pingName"));
    m_valueLabel = new QLabel(this);
    m_valueLabel->setObjectName(QStringLiteral("pingValue"));

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(4, 0, 4, 0);
    row->setSpacing(6);
    row->addWidget(m_dot);
    row->addWidget(m_nameLabel);
    row->addWidget(m_valueLabel);
    row->addStretch(1);

    setMinimumWidth(kMinChipWidth);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // Pulse: fade the dot only, on a 2 s infinite loop, and only while the service is Ok.
    m_pulseEffect = new QGraphicsOpacityEffect(m_dot);
    m_pulseEffect->setOpacity(1.0);
    m_dot->setGraphicsEffect(m_pulseEffect);

    m_pulse = new QPropertyAnimation(m_pulseEffect, QByteArrayLiteral("opacity"), this);
    m_pulse->setDuration(2000);
    m_pulse->setStartValue(1.0);
    m_pulse->setKeyValueAt(0.5, 0.35);
    m_pulse->setEndValue(1.0);
    m_pulse->setEasingCurve(QEasingCurve::InOutSine);
    m_pulse->setLoopCount(-1);

    setToolTip(summary());
    applyStatus(PingStatus::Unknown, -1);
}

PingIndicator::~PingIndicator()
{
    if (m_pulse)
        m_pulse->stop();
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
            return QStringLiteral("%1 мс").arg(latencyMs);
        return QStringLiteral("ok");
    case PingStatus::Fail:
        return QStringLiteral("таймаут");
    case PingStatus::Unknown:
        break;
    }
    return QStringLiteral("—");
}

QString PingIndicator::summary() const
{
    return QStringLiteral("%1 %2").arg(m_name, latencyText(m_status, m_latencyMs));
}

void PingIndicator::setServiceName(const QString &name)
{
    m_name = name;
    m_nameLabel->setText(m_name);
    setToolTip(summary());
}

bool PingIndicator::isPulsing() const
{
    return m_pulse && m_pulse->state() == QPropertyAnimation::Running;
}

void PingIndicator::setStatus(PingStatus status, int latencyMs)
{
    applyStatus(status, latencyMs);
}

void PingIndicator::applyStatus(PingStatus status, int latencyMs)
{
    m_status = status;
    m_latencyMs = latencyMs;

    m_dot->setColor(dotColor(status));
    m_valueLabel->setText(latencyText(status, latencyMs));
    setToolTip(summary());

    if (status == PingStatus::Ok) {
        if (m_pulse->state() != QPropertyAnimation::Running) {
            m_pulseEffect->setOpacity(1.0);
            m_pulse->start();
        }
    } else {
        m_pulse->stop();
        m_pulseEffect->setOpacity(1.0);
    }

    update();
}

} // namespace zapret
