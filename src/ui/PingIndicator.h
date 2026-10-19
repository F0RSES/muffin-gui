#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

class QLabel;
class QVariantAnimation;

namespace zapret {

class PingRing;

// One service's reachability, drawn as the dashboard's small neon badge: the latency figure with a clock
// glyph above it, the service's mark inside a glowing ring, and its name underneath.
//
// The ring is a tiny dedicated widget so the "alive" breath can animate only the halo, and so the ring's
// colour — which carries the status — stays separate from the mark inside it, which is always white.
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

    // True while the "alive" breath is driving the ring's halo (only ever for PingStatus::Ok).
    bool isPulsing() const;

    void setServiceName(const QString &name);
    QString serviceName() const { return m_name; }

    // The mark inside the ring: a name under :/icons/<glyph>.svg. Defaults to whatever glyphForService()
    // picks for the service name, and is re-picked by setServiceName().
    void setGlyph(const QString &glyph);
    QString glyph() const { return m_glyph; }

    // Map a PingService status int onto the UI enum (0 Waiting, 1 Ok, 2 Fail; anything else Unknown).
    static PingStatus statusFromServiceInt(int status);

    // "12 мс" / "…" / "таймаут" / "—". The long form, for tooltips and anything that reads as a sentence.
    static QString latencyText(PingStatus status, int latencyMs);
    // "12" / "…" / "!" / "—". The short form: the figure above the ring, which has no room for a unit and
    // reads better as a number than as a phrase.
    static QString latencyNumber(PingStatus status, int latencyMs);
    // "<name> <latencyText>" — used as the widget's tooltip.
    QString summary() const;

    // The ring's colour for a status (spec-fixed neon, theme-independent). Named for what it replaced:
    // this badge used to be a status-bar chip with a 12 px dot.
    static QColor dotColor(PingStatus status);

    // Which mark a service name gets: the brand glyph for the hosts that have one, a globe for the rest.
    // Case-insensitive on purpose — the names come from a hostname, not from a catalogue.
    static QString glyphForService(const QString &serviceName);

private:
    void applyStatus(PingStatus status, int latencyMs);
    void refreshGlyph();

    PingRing *m_ring = nullptr;
    QLabel *m_numberLabel = nullptr;
    QLabel *m_clockLabel = nullptr;
    QLabel *m_nameLabel = nullptr;

    PingStatus m_status = PingStatus::Unknown;
    int m_latencyMs = -1;
    QString m_name;
    QString m_glyph;
};

} // namespace zapret
