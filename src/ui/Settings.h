#pragma once

#include <QObject>
#include <QString>

#include <memory>

class QSettings;

namespace zapret {

// Typed view over QSettings for everything the GUI persists.
//
// The wrapper exists so no widget ever spells a key name or a default twice: readers get sane
// defaults, writers emit changed() so open windows (MainWindow, SettingsDialog, tray) can react in
// one place. IniFormat is forced on every platform, including Windows, so the file location is
// predictable (QStandardPaths::AppConfigLocation) and the settings stay a plain, portable text file.
class Settings : public QObject
{
    Q_OBJECT

public:
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

    // ---- ping ----
    int pingIntervalSec() const;         // clamped to 3..60
    void setPingIntervalSec(int seconds);

    // ---- appearance ----
    QString theme() const;               // "dark" | "light" | "system"
    void setTheme(const QString &name);

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

} // namespace zapret
