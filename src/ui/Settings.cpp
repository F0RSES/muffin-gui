#include "Settings.h"

#include <QCoreApplication>
#include <QSettings>

namespace zapret {

namespace {
// Key names live here once. Anything that spells them elsewhere would drift on the first rename.
const QString kZapretPath        = QStringLiteral("zapret/path");
const QString kLastStrategyId    = QStringLiteral("strategy/last");
const QString kStartLastOnLaunch = QStringLiteral("startup/startLast");
const QString kMinimizeToTray    = QStringLiteral("ui/minimizeToTray");
const QString kPingIntervalSec   = QStringLiteral("ping/intervalSec");
const QString kTheme             = QStringLiteral("ui/theme");

constexpr int kPingIntervalDefault = 5;
constexpr int kPingIntervalMin     = 3;
constexpr int kPingIntervalMax     = 60;
} // namespace

Settings::Settings(QObject *parent)
    : QObject(parent)
{
    const QString org = QCoreApplication::organizationName().isEmpty()
                            ? QStringLiteral("ZapretGui")
                            : QCoreApplication::organizationName();
    const QString app = QCoreApplication::applicationName().isEmpty()
                            ? QStringLiteral("ZapretGui")
                            : QCoreApplication::applicationName();
    m_settings = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, org, app);
}

Settings::Settings(const QString &iniFilePath, QObject *parent)
    : QObject(parent)
{
    m_settings = std::make_unique<QSettings>(iniFilePath, QSettings::IniFormat);
}

Settings::~Settings() = default;

template <typename T>
T Settings::read(const QString &key, const T &fallback) const
{
    return m_settings->value(key, fallback).template value<T>();
}

template <typename T>
void Settings::write(const QString &key, const T &value)
{
    m_settings->setValue(key, value);
    m_settings->sync();
    emit changed();
}

QString Settings::zapretPath() const { return read<QString>(kZapretPath, QString()); }
void Settings::setZapretPath(const QString &path) { write(kZapretPath, path); }

QString Settings::lastStrategyId() const { return read<QString>(kLastStrategyId, QString()); }
void Settings::setLastStrategyId(const QString &id) { write(kLastStrategyId, id); }

bool Settings::startLastOnLaunch() const { return read<bool>(kStartLastOnLaunch, false); }
void Settings::setStartLastOnLaunch(bool on) { write(kStartLastOnLaunch, on); }

// Default on: zapret is a background service, so closing the window should keep it running.
// MainWindow refuses to hide when the platform has no system tray, so this can never strand the app.
bool Settings::minimizeToTray() const { return read<bool>(kMinimizeToTray, true); }
void Settings::setMinimizeToTray(bool on) { write(kMinimizeToTray, on); }

int Settings::pingIntervalSec() const
{
    const int value = read<int>(kPingIntervalSec, kPingIntervalDefault);
    return qBound(kPingIntervalMin, value, kPingIntervalMax);
}

void Settings::setPingIntervalSec(int seconds)
{
    write(kPingIntervalSec, qBound(kPingIntervalMin, seconds, kPingIntervalMax));
}

QString Settings::theme() const { return read<QString>(kTheme, QStringLiteral("dark")); }
void Settings::setTheme(const QString &name) { write(kTheme, name); }

QString Settings::filePath() const { return m_settings->fileName(); }
void Settings::sync() { m_settings->sync(); }

} // namespace zapret
