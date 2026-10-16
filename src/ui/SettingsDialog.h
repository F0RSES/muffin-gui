#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QSpinBox;

namespace zapret {

class Settings;

// Configuration sheet: where zapret lives, what happens at startup, how the window behaves, how the
// theme is chosen and how often the reachability probes run.
//
// Path validation is live (on every keystroke) and cheap: ZapretPaths only stats marker files
// (bin/winws.exe, init.d/sysv/zapret, binaries/<arch>/nfqws). describePath()/pathLooksValid() are
// static so the verdict can be reasoned about without a widget hierarchy.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(Settings &settings, QWidget *parent = nullptr);
    ~SettingsDialog() override;

    // Read the stored values into the widgets / write the widgets back into Settings.
    void loadFromSettings();
    void apply();

    QString zapretPath() const;
    void setZapretPath(const QString &path);

    // The warning shown when QSystemTrayIcon reports no tray host, or null. Exposed so a test can check
    // the dialog actually explains the degraded behaviour instead of leaving the box ticked in vain.
    QLabel *trayNote() const { return m_trayNote; }

    // True when `dir` is a zapret root, or a base that contains a windows/ or linux/ root.
    static bool pathLooksValid(const QString &dir);
    // Human verdict shown next to the field (platform + how many entries were found).
    static QString describePath(const QString &dir);

signals:
    void applied();

private slots:
    void browse();
    void autoDetect();
    void revalidatePath();

private:
    void build();
    void setPathState(bool ok, const QString &message);

    Settings &m_settings;

    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_pathState = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_detectButton = nullptr;

    QCheckBox *m_startLastCheck = nullptr;
    QCheckBox *m_trayCheck = nullptr;
    QLabel *m_trayNote = nullptr;    // shown only when this desktop has no tray host at all
    QComboBox *m_themeBox = nullptr;
    QSpinBox *m_pingSpin = nullptr;
};

} // namespace zapret
