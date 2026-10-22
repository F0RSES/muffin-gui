#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QSettings;

namespace zapret {

// Typed view over QSettings for everything the GUI persists.
//
// The wrapper exists so no widget ever spells a key name or a default twice: readers get sane
// defaults, writers emit changed() so open views (MainWindow, SettingsPage, tray) can react in
// one place.
//
// The application-wide store uses Qt's NATIVE format for the platform — QSettings::setDefaultFormat()
// is never called, so a default-constructed QSettings resolves organizationName()/applicationName() to
// wherever this platform keeps them: the registry under HKCU\Software\F0RSES\Muffin on Windows, a plist
// under ~/Library/Preferences on macOS, and an ini file in the organization's config directory on Linux
// (~/.config/F0RSES/Muffin.conf, honouring XDG_CONFIG_HOME) — Qt's native format on XDG platforms
// already being IniFormat, which is why the file looks like the one an ini store would write.
//
// That is a deliberate choice, not an oversight: it is the format the 0.1.0 store was written in, so
// importLegacySettings() can read the old identity with QSettings::defaultFormat() and be sure it is
// naming the same kind of store. It also carries the one cost worth knowing — on Windows the values live
// in the registry rather than in a text file, so nothing here is a portable file the user can copy, and
// renaming the organization relocates the whole store (again: that is what the import repairs).
//
// Only the explicit-path constructor below forces IniFormat, and only so that a test or an isolated
// store is handed a plain text file at a known location.
class Settings : public QObject
{
    Q_OBJECT

public:
    // ---- the watchdog's cadence, spelled exactly once ----
    //
    // The clamp below is the policy; the spin box's range, the model's own clamp and the QTimer's
    // interval are three places that would otherwise each get their own copy of "1..60". A range that
    // lives in one of them only is a range that silently disagrees with the other two the first time
    // somebody widens it, so they are published here and read from everywhere else.
    static constexpr int kWatchdogDefaultIntervalMin = 5;
    static constexpr int kWatchdogMinIntervalMin = 1;
    static constexpr int kWatchdogMaxIntervalMin = 60;

    // Application-wide store (organizationName/applicationName from QApplication).
    explicit Settings(QObject *parent = nullptr);
    // Explicit ini file — used by tests and by anyone who wants an isolated store.
    explicit Settings(const QString &iniFilePath, QObject *parent = nullptr);
    ~Settings() override;

    // ---- zapret location ----
    QString zapretPath() const;
    void setZapretPath(const QString &path);

    // ---- strategy bookkeeping ----
    QString lastStrategyId() const;
    void setLastStrategyId(const QString &id);

    // ---- behaviour toggles ----
    bool startLastOnLaunch() const;      // auto-start the previous strategy at startup
    void setStartLastOnLaunch(bool on);

    bool minimizeToTray() const;         // hide to tray instead of quitting on close
    void setMinimizeToTray(bool on);

    // ---- motion ----
    // Whether the interface may move at all: page fades, card entrances, the power button's breath. On
    // by default, because the design was drawn moving; the switch exists for the machines the movement
    // costs. Read once by MainWindow at startup and again on every write, and it is the whole policy —
    // no widget consults this directly, they all ask Motion, which the window keeps in step with it.
    bool animationsEnabled() const;
    void setAnimationsEnabled(bool on);

    // ---- ping ----
    int pingIntervalSec() const;         // clamped to 3..60
    void setPingIntervalSec(int seconds);

    // ---- watchdog ----
    // How often a live profile is re-asked "do you still bypass?", in minutes, clamped to the range
    // above. The window re-arms its timer from this value, so the setting is the whole policy.
    int watchdogIntervalMin() const;
    void setWatchdogIntervalMin(int minutes);

    // ---- appearance ----
    QString theme() const;               // "dark" | "light" | "system"
    void setTheme(const QString &name);

    // The UI language: a LanguageManager code ("ru", "en", "es", "de", "zh", "fr", "hi"). Unset
    // resolves to "ru", the language every screen was drawn and tested in, so a first run and a
    // headless test agree on what the chrome says.
    QString language() const;
    void setLanguage(const QString &code);

    QString filePath() const;
    void sync();

signals:
    // Emitted after any successful write. Views reconnect to it to re-read what they care about.
    void changed();

private:
    template <typename T>
    T read(const QString &key, const T &fallback) const;
    template <typename T>
    void write(const QString &key, const T &value);

    std::unique_ptr<QSettings> m_settings;
};

// One-time carry-over of everything the 0.1.0 identity persisted, into the store this build uses.
//
// QSettings resolves its file/registry location from organizationName()+applicationName(), so renaming
// the organization moves the settings too — a user who upgrades would lose their theme, ping interval,
// last strategy and the stored zapret path with no warning at all. This imports them.
//
// It must run before the first Settings instance is constructed, i.e. from the entry point, and it
// never overwrites: if the current identity already holds any value the upgrade has been imported (or
// the user started fresh), so the legacy store is left alone. Two stored values can name a path inside
// the folder the migration moved — the zapret root, and a strategy id, which StrategyParser stamps as
// an absolute path into that tree for entries it cannot name — and both are rebased onto the new
// location, but only when the old one has really gone away. See ZapretPaths::migrateManagedInstallDir(),
// whose settings half this is.
void importLegacySettings();

} // namespace zapret
