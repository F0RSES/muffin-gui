#include "Settings.h"

#include "ZapretPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>

namespace zapret {

namespace {
// Key names live here once. Anything that spells them elsewhere would drift on the first rename.
const QString kZapretPath        = QStringLiteral("zapret/path");
const QString kLastStrategyId    = QStringLiteral("strategy/last");
const QString kStartLastOnLaunch = QStringLiteral("startup/startLast");
const QString kMinimizeToTray    = QStringLiteral("ui/minimizeToTray");
const QString kAnimations        = QStringLiteral("ui/animations");
const QString kPingIntervalSec   = QStringLiteral("ping/intervalSec");
const QString kWatchdogInterval  = QStringLiteral("watchdog/intervalMin");
const QString kTheme             = QStringLiteral("ui/theme");
const QString kLanguage          = QStringLiteral("ui/language");
const QString kWarpMode          = QStringLiteral("ui/warpMode");

constexpr int kPingIntervalDefault = 5;
constexpr int kPingIntervalMin     = 3;
constexpr int kPingIntervalMax     = 60;

// Everything the 0.1.0 identity could have written, spelled with the constants above instead of
// repeated as literals. The migration changed the organization, not the schema, so a legacy key and
// this build's key are the same string; a key present only in the legacy file is one this build
// knows nothing about, and dropping it is the right outcome — importing never invents a value.
const QStringList &legacyKeys()
{
    static const QStringList keys = { kZapretPath, kLastStrategyId, kStartLastOnLaunch,
                                     kMinimizeToTray, kPingIntervalSec, kTheme };
    return keys;
}

// The subset of those values that can hold a path into the managed install, and therefore need
// rebasing when the migration moves that folder:
//
//   * zapret/path — the root the GUI was pointed at;
//   * strategy/last — the id of the last used strategy, which MainWindow stores as
//     strategy->id.isEmpty() ? strategy->filePath : strategy->id, and StrategyParser stamps with
//     absoluteFilePath() for entries it cannot name (a direct nfqws binary). A portable-mode user's
//     real 0.1.0 store therefore holds ".../zapret/binaries/<arch>/nfqws" here: the same folder the
//     folder move renamed.
//
// Named separately from legacyKeys() because the two jobs are different: everything is carried over,
// only path-bearing values may be rewritten.
const QStringList &managedPathKeys()
{
    static const QStringList keys = { kZapretPath, kLastStrategyId };
    return keys;
}

// One spelling for a path: '/' separators with the structure folded — no duplicate or trailing
// separators, no '.' or '..' segments. cleanPath() does the folding; fromNativeSeparators() then
// pins the separator style, because a stored value is whatever QStandardPaths handed back and that is
// native (backslashes) on Windows while the replacement built here always uses '/'.
QString normalizedPath(const QString &path)
{
    return QDir::fromNativeSeparators(QDir::cleanPath(path));
}

bool samePath(const QString &a, const QString &b)
{
    const QString left = normalizedPath(a);
    const QString right = normalizedPath(b);
#if defined(Q_OS_WIN)
    return left.compare(right, Qt::CaseInsensitive) == 0;
#else
    return left == right;
#endif
}

bool pathUnder(const QString &path, const QString &prefix)
{
    const QString cleanPrefix = normalizedPath(prefix);
    if (cleanPrefix.isEmpty())
        return false;
#if defined(Q_OS_WIN)
    return normalizedPath(path).startsWith(cleanPrefix + QLatin1Char('/'), Qt::CaseInsensitive);
#else
    return normalizedPath(path).startsWith(cleanPrefix + QLatin1Char('/'));
#endif
}

// Rebase `stored` from the managed directory the legacy identity resolved to onto the one this
// identity resolves to, keeping any child segment the path had.
//
// Anything that is not under the legacy directory comes back byte-for-byte unchanged: the stored root
// is also whatever the user picked on the settings page (~/Documents/zapret, /opt/zapret, a linux/
// child of some other unpack), and those folders were never moved by this migration. Rewriting one of them
// would send the GUI looking for a tree that is not there, which is worse than the stale path it
// replaced.
QString rebaseManagedPath(const QString &stored, const QString &legacyDir, const QString &managedDir)
{
    if (stored.isEmpty() || legacyDir.isEmpty() || managedDir.isEmpty())
        return stored;
    const QString storedPath = normalizedPath(stored);
    const QString legacyPath = normalizedPath(legacyDir);
    if (samePath(stored, legacyDir))
        return normalizedPath(managedDir);
    if (pathUnder(stored, legacyDir))
    {
        // mid() of the normalized legacy length: the remainder starts at the first separator after the
        // legacy directory, so it can be hung off the new one as-is. 0.1.0 stores the resolved subtree
        // (linux/ or windows/) far more often than the parent it was unpacked into.
        return normalizedPath(managedDir + storedPath.mid(legacyPath.size()));
    }
    return stored;
}
} // namespace

Settings::Settings(QObject *parent)
    : QObject(parent)
{
    // QSettings() resolves organizationName()/applicationName() on its own; the entry point (main.cpp)
    // owns those identities, so hardcoding a second copy here could only ever drift from it.
    m_settings = std::make_unique<QSettings>();
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

// Default off: Zapret is the mode the app was designed around, and picking WARP is an explicit
// switch to a different tool — a first run that lit the tunnel without the user asking would be
// the interface changing the shape of their traffic on its own.
bool Settings::warpMode() const { return read<bool>(kWarpMode, false); }
void Settings::setWarpMode(bool on) { write(kWarpMode, on); }

// Default on: zapret is a background service, so closing the window should keep it running.
// MainWindow refuses to hide when the platform has no system tray, so this can never strand the app.
bool Settings::minimizeToTray() const { return read<bool>(kMinimizeToTray, true); }
void Settings::setMinimizeToTray(bool on) { write(kMinimizeToTray, on); }

// Default on too, and for the opposite reason: the interface was drawn moving, so a first run that did
// not would be a different application. Unlike the tray this has no platform half — the box is the whole
// answer, and Motion::setEnabled is the only thing that reads it.
bool Settings::animationsEnabled() const { return read<bool>(kAnimations, true); }
void Settings::setAnimationsEnabled(bool on) { write(kAnimations, on); }

int Settings::pingIntervalSec() const
{
    const int value = read<int>(kPingIntervalSec, kPingIntervalDefault);
    return qBound(kPingIntervalMin, value, kPingIntervalMax);
}

void Settings::setPingIntervalSec(int seconds)
{
    write(kPingIntervalSec, qBound(kPingIntervalMin, seconds, kPingIntervalMax));
}

// Five minutes: long enough that nobody pays for the check in traffic or in a pkexec prompt, short
// enough that a provider that flips its rules during the evening is caught before the user wonders
// why Discord is acting up — rather than the next morning.
int Settings::watchdogIntervalMin() const
{
    const int value = read<int>(kWatchdogInterval, kWatchdogDefaultIntervalMin);
    return qBound(kWatchdogMinIntervalMin, value, kWatchdogMaxIntervalMin);
}

void Settings::setWatchdogIntervalMin(int minutes)
{
    write(kWatchdogInterval, qBound(kWatchdogMinIntervalMin, minutes, kWatchdogMaxIntervalMin));
}

QString Settings::theme() const { return read<QString>(kTheme, QStringLiteral("dark")); }
void Settings::setTheme(const QString &name) { write(kTheme, name); }

QString Settings::language() const { return read<QString>(kLanguage, QStringLiteral("ru")); }
void Settings::setLanguage(const QString &code) { write(kLanguage, code); }

QString Settings::filePath() const { return m_settings->fileName(); }
void Settings::sync() { m_settings->sync(); }

void importLegacySettings()
{
    // The store this build's identity resolves to — same scope, same default format, different
    // organization. QSettings() with no arguments is exactly how 0.1.0 spelled it, so the pair
    // (defaultFormat(), UserScope) is what makes the two halves comparable: the legacy store has to
    // be read with the format 0.1.0 wrote it in, which is the platform default this process resolves
    // too (ini under XDG_CONFIG_HOME on Linux, registry under HKCU on Windows, plist on macOS).
    // Changing QSettings::setDefaultFormat() would change what this line means: the legacy store was
    // written under the format that was the default then.
    QSettings current;

    // Carry-over, once. Any value already living under the new identity means the import ran — or the
    // user deleted the file and started over — and re-importing would quietly undo every change they
    // made since the upgrade. allKeys() rather than a per-key probe because the legacy file can also
    // hold keys this build no longer reads: an empty store is the only state that proves "nothing
    // imported yet".
    if (current.allKeys().isEmpty())
    {
        const QSettings legacy(QSettings::defaultFormat(), QSettings::UserScope,
                               ZapretPaths::legacyOrganizationName(),
                               QCoreApplication::applicationName());
        for (const QString &key : legacyKeys())
        {
            if (legacy.contains(key))
                current.setValue(key, legacy.value(key));
        }
    }

    // The repair half of ZapretPaths::migrateManagedInstallDir(): that call moved the downloaded tree,
    // and the carried-over values still name where it used to be. Only the two values that can hold a
    // path inside it are candidates, and only when the old location really is gone and the new one
    // really exists — so a refused or failed move, and every value that never pointed at the managed
    // install, survive exactly as the user last set them.
    const QString legacyDir = ZapretPaths::legacyManagedInstallDir();
    const QString managedDir = ZapretPaths::managedInstallDir();
    if (!legacyDir.isEmpty() && !managedDir.isEmpty()
        && !QFileInfo::exists(legacyDir) && QFileInfo::exists(managedDir))
    {
        for (const QString &key : managedPathKeys())
        {
            const QString stored = current.value(key).toString();
            const QString repaired = rebaseManagedPath(stored, legacyDir, managedDir);
            if (repaired != stored)
                current.setValue(key, repaired);
        }
    }

    current.sync();
}

} // namespace zapret
