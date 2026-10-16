#pragma once

#include <QColor>
#include <QWidget>

class QGraphicsOpacityEffect;
class QLabel;
class QPropertyAnimation;

namespace zapret {

class PingDot;

// One status-bar reachability chip: a coloured dot, the service name and its latency.
//
// The dot is a tiny dedicated widget so the "alive" pulse animation can fade just the circle through
// its own QGraphicsOpacityEffect instead of fading the whole chip (and its text) with it.
class PingIndicator : public QWidget
{
    Q_OBJECT

public:
    // Mirrors the int status PingService reports: Unknown is the pre-first-probe state, Waiting is
    // "probe in flight".
    enum class PingStatus
    {
        Unknown,
        Waiting,
        Ok,
        Fail
    };
    Q_ENUM(PingStatus)

    explicit PingIndicator(const QString &serviceName, QWidget *parent = nullptr);
    ~PingIndicator() override;

    void setStatus(PingStatus status, int latencyMs = -1);
    PingStatus status() const { return m_status; }

    // True while the "alive" pulse animation is driving the dot's opacity (only for PingStatus::Ok).
    bool isPulsing() const;

    void setServiceName(const QString &name);
    QString serviceName() const { return m_name; }

    // Map a PingService status int onto the UI enum (0 Waiting, 1 Ok, 2 Fail; anything else Unknown).
    static PingStatus statusFromServiceInt(int status);

    // "12 мс" / "…" / "таймаут" / "—". Exposed for tests and for reuse by tooltips.
    static QString latencyText(PingStatus status, int latencyMs);
    // "<name> <latencyText>" — used as the widget's tooltip.
    QString summary() const;

    // The dot's colour for a status (spec-fixed palette, theme-independent).
    static QColor dotColor(PingStatus status);

private:
    void applyStatus(PingStatus status, int latencyMs);

    PingDot *m_dot = nullptr;
    QLabel *m_nameLabel = nullptr;
    QLabel *m_valueLabel = nullptr;
    QGraphicsOpacityEffect *m_pulseEffect = nullptr;
    QPropertyAnimation *m_pulse = nullptr;

    PingStatus m_status = PingStatus::Unknown;
    int m_latencyMs = -1;
    QString m_name;
};

} // namespace zapret
