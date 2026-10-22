#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace zapret {

class Settings;

// Where zapret lives, what happens at startup, how the window behaves, which theme, how often the
// reachability probes run and how often a live profile is re-asked whether it still bypasses — as a
// page of the main window's own stack rather than a modal sheet.
//
// The move from dialog to page is the whole point: the rail already carries a settings glyph, and a
// glyph that opens a modal is two ways of navigating where the design wants one. It also changes what
// "commit" means. A dialog has a frame and an OK button; a page has neither, so the commit is a button
// of its own at the foot of the form, and leaving the page without pressing it is the equivalent of
// Cancel — except that it says so, with the note beside the button staying blank.
//
// Path validation stays live (on every keystroke) and cheap: ZapretPaths only stats marker files
// (bin/winws.exe, init.d/sysv/zapret, binaries/<arch>/nfqws). describePath()/pathLooksValid() are
// static so the verdict can be reasoned about without a widget hierarchy.
class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(Settings &settings, QWidget *parent = nullptr);
    ~SettingsPage() override;

    // Read the stored values into the widgets / write the widgets back into Settings.
    void loadFromSettings();
    void apply();

    QString zapretPath() const;
    void setZapretPath(const QString &path);

    // The warning shown when QSystemTrayIcon reports no tray host, or null. Exposed so a test can check
    // the page actually explains the degraded behaviour instead of leaving the box ticked in vain.
    QLabel *trayNote() const { return m_trayNote; }
    // The commit control and the acknowledgement it writes. Both exposed for the same reason: "Save
    // does nothing" and "Save never says it saved" are the two ways this page could quietly regress.
    QPushButton *saveButton() const { return m_save; }
    QLabel *savedNote() const { return m_savedNote; }
    // The two short fields. Exposed because their shape is the page's promise: a field that holds one
    // value stays beside its label instead of spanning the form, and the interval box carries no
    // stepper arrows. Both are things a later layout change can silently undo.
    QComboBox *themeField() const { return m_themeBox; }
    QSpinBox *pingIntervalField() const { return m_pingSpin; }
    // The watchdog's cadence, in the same shape as the ping interval above — and exposed for the same
    // reason: the range is a promise too, and it is the one this page has to keep with the model.
    QSpinBox *watchdogIntervalField() const { return m_watchdogSpin; }

    // True when `dir` is a zapret root, or a base that contains a windows/ or linux/ root.
    static bool pathLooksValid(const QString &dir);
    // Human verdict shown next to the field (platform + how many entries were found).
    static QString describePath(const QString &dir);

signals:
    void saved();

private slots:
    void browse();
    void autoDetect();
    // Fetch a release into the managed install and put the resulting path in the field. A third action
    // next to "Обзор…" and "Автопоиск" because both of those assume zapret is already on this disk.
    void downloadZapret();
    void revalidatePath();

private:
    void build();
    void setPathState(bool ok, const QString &message);

    Settings &m_settings;
    // Guards the Settings::changed() reload: apply() writes through the model, and the model answers by
    // telling every view to re-read itself — including this one, mid-commit.
    bool m_writing = false;

    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_pathState = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_detectButton = nullptr;
    QPushButton *m_downloadButton = nullptr;

    QCheckBox *m_startLastCheck = nullptr;
    QCheckBox *m_trayCheck = nullptr;
    QLabel *m_trayNote = nullptr;    // shown only when this desktop has no tray host at all
    QComboBox *m_themeBox = nullptr;
    QSpinBox *m_pingSpin = nullptr;
    QSpinBox *m_watchdogSpin = nullptr;

    QPushButton *m_save = nullptr;
    QLabel *m_savedNote = nullptr;
    QTimer *m_savedTimer = nullptr;  // for how long "Сохранено ✓" stays up
};

} // namespace zapret
