#include "SettingsPage.h"

#include "DownloadDialog.h"
#include "LanguageManager.h"
#include "Settings.h"
#include "StrategyParser.h"
#include "Theme.h"
#include "ZapretLocator.h"
#include "ZapretPaths.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace zapret {

namespace {
// How long the acknowledgement next to the Save button holds. Long enough to read, short enough that it
// cannot be mistaken for the page's opinion of the values currently in the fields.
constexpr int kSavedNoteMs = 2000;

// The width of a field that holds one short value. A form column stretches whatever is put in it, and a
// theme picker that spans the page reads as a sentence rather than as a choice of three words — the same
// reason the interval box does not need a line long enough to type a novel into.
constexpr int kNarrowFieldPx = 200;
} // namespace

SettingsPage::SettingsPage(Settings &settings, QWidget *parent)
    : QWidget(parent)
    , m_settings(settings)
{
    build();
    loadFromSettings();

    // The application can move the path without anyone touching this form — a download finishing, the
    // startup auto-location, the placeholder's button. Re-read on every write that was not ours.
    connect(&m_settings, &Settings::changed, this, [this] {
        if (!m_writing)
            loadFromSettings();
    });
}

SettingsPage::~SettingsPage() = default;

void SettingsPage::build()
{
    setObjectName(QStringLiteral("settingsPage"));
    setAttribute(Qt::WA_StyledBackground, true);

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(QStringLiteral("/home/…/zapret/linux ")
                                   + LanguageManager::tr(QStringLiteral("common.or"))
                                   + QStringLiteral(" C:\\...\\zapret\\windows"));
    m_pathEdit->setClearButtonEnabled(true);

    // Three ways to fill one field, because three situations call for them: the tree is on this disk and
    // the user knows where, the tree is on this disk and nobody knows where, or there is no tree yet.
    i18nText(m_browseButton = new QPushButton(this), QStringLiteral("settings.browse"));
    i18nText(m_detectButton = new QPushButton(this), QStringLiteral("settings.autodetect"));
    i18nTip(m_detectButton, QStringLiteral("settings.autodetect_tip"));
    i18nText(m_downloadButton = new QPushButton(this), QStringLiteral("action.download_zapret"));
    i18nTip(m_downloadButton, QStringLiteral("action.download_zapret_tip"));

    auto *pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->setSpacing(6);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(m_browseButton);
    pathRow->addWidget(m_detectButton);
    pathRow->addWidget(m_downloadButton);

    m_pathState = new QLabel(this);
    m_pathState->setWordWrap(true);

    i18nText(m_startLastCheck = new QCheckBox(this), QStringLiteral("settings.start_last"));
    i18nText(m_trayCheck = new QCheckBox(this), QStringLiteral("settings.tray"));

    // A ticked box that cannot be honoured is worse than an explanation. Sessions without a StatusNotifier
    // host (bare GNOME, a plain Wayland compositor) report no tray at all, so spell out what closing the
    // window will actually do — MainWindow still retries, so the tray may appear later by itself.
    m_trayNote = new QLabel(this);
    m_trayNote->setObjectName(QStringLiteral("trayNote"));
    i18nText(m_trayNote, QStringLiteral("settings.tray_note"));
    m_trayNote->setWordWrap(true);
    m_trayNote->setVisible(!QSystemTrayIcon::isSystemTrayAvailable());

    m_themeBox = new QComboBox(this);
    m_themeBox->setMaximumWidth(kNarrowFieldPx);
    m_themeBox->addItem(LanguageManager::tr(QStringLiteral("theme.dark")), Theme::nameForMode(Theme::Mode::Dark));
    m_themeBox->addItem(LanguageManager::tr(QStringLiteral("theme.light")), Theme::nameForMode(Theme::Mode::Light));
    m_themeBox->addItem(LanguageManager::tr(QStringLiteral("theme.system")), Theme::nameForMode(Theme::Mode::System));
    i18nItems(m_themeBox, { QStringLiteral("theme.dark"), QStringLiteral("theme.light"),
                            QStringLiteral("theme.system") });

    m_pingSpin = new QSpinBox(this);
    m_pingSpin->setMaximumWidth(kNarrowFieldPx);
    // No stepper arrows. They are the platform's own control, drawn by the style rather than by the
    // sheet, and the value they set is a number the user types or nudges once a year — the arrows cost
    // more attention than the field they belong to. Typing and the wheel still work.
    m_pingSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_pingSpin->setRange(3, 60);
    m_pingSpin->setSuffix(LanguageManager::tr(QStringLiteral("settings.ping_suffix")));
    i18nSuffix(m_pingSpin, QStringLiteral("settings.ping_suffix"));
    m_pingSpin->setSingleStep(1);

    // The watchdog's cadence, built like the box above because it is the same kind of answer: one short
    // number with a unit. Its range comes from Settings, not from a second pair of literals here — the
    // model clamps what it stores and the window arms a QTimer from that value, so three places would
    // otherwise have to agree on "1..60" by coincidence.
    m_watchdogSpin = new QSpinBox(this);
    m_watchdogSpin->setMaximumWidth(kNarrowFieldPx);
    m_watchdogSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_watchdogSpin->setRange(Settings::kWatchdogMinIntervalMin, Settings::kWatchdogMaxIntervalMin);
    m_watchdogSpin->setSuffix(LanguageManager::tr(QStringLiteral("settings.watchdog_suffix")));
    i18nSuffix(m_watchdogSpin, QStringLiteral("settings.watchdog_suffix"));
    m_watchdogSpin->setSingleStep(1);
    i18nTip(m_watchdogSpin, QStringLiteral("settings.watchdog_tip"));

    // Labels are built here rather than passed as strings to addRow so each one can carry the key it
    // came from; a QFormLayout's own label has no handle to tag.
    auto *pathRowLabel = i18nText(new QLabel(this), QStringLiteral("settings.path"));
    auto *themeRowLabel = i18nText(new QLabel(this), QStringLiteral("settings.theme"));
    auto *pingRowLabel = i18nText(new QLabel(this), QStringLiteral("settings.ping"));
    auto *watchdogRowLabel = i18nText(new QLabel(this), QStringLiteral("settings.watchdog"));

    // The two short fields, held to the left. A QFormLayout gives a field the whole width of its column,
    // which is right for a path and wrong for a three-word choice: the page then reads as two long
    // controls and a lot of empty form. The stretch beside each one is what keeps it beside its label
    // rather than across the screen.
    auto *themeField = new QHBoxLayout;
    themeField->setContentsMargins(0, 0, 0, 0);
    themeField->addWidget(m_themeBox);
    themeField->addStretch(1);

    auto *pingField = new QHBoxLayout;
    pingField->setContentsMargins(0, 0, 0, 0);
    pingField->addWidget(m_pingSpin);
    pingField->addStretch(1);

    auto *watchdogField = new QHBoxLayout;
    watchdogField->setContentsMargins(0, 0, 0, 0);
    watchdogField->addWidget(m_watchdogSpin);
    watchdogField->addStretch(1);

    auto *form = new QFormLayout;
    form->setSpacing(10);
    form->addRow(pathRowLabel, pathRow);
    form->addRow(QString(), m_pathState);
    form->addRow(QString(), m_startLastCheck);
    form->addRow(QString(), m_trayCheck);
    form->addRow(QString(), m_trayNote);
    form->addRow(themeRowLabel, themeField);
    form->addRow(pingRowLabel, pingField);
    // Right under the ping interval, because that is what it is: a second cadence, measured in minutes
    // rather than seconds, for the same question about the same services.
    form->addRow(watchdogRowLabel, watchdogField);

    // The commit row. The dialog's OK/Cancel pair is gone: a page has no frame to dismiss, so leaving it
    // is the cancel, and the only thing that has to be explicit is the write. The acknowledgement sits to
    // the left of the button it belongs to rather than in a footer, because a note the button did not
    // cause would be indistinguishable from a note about the values.
    m_savedNote = new QLabel(this);
    m_savedNote->setObjectName(QStringLiteral("savedNote"));
    m_savedNote->setTextFormat(Qt::PlainText);

    m_save = new QPushButton(this);
    m_save->setObjectName(QStringLiteral("primaryButton"));
    m_save->setCursor(Qt::PointingHandCursor);
    m_save->setAutoDefault(false);
    i18nText(m_save, QStringLiteral("settings.save"));

    auto *commit = new QHBoxLayout;
    commit->setContentsMargins(0, 0, 0, 0);
    commit->setSpacing(12);
    commit->addWidget(m_savedNote);
    commit->addStretch(1);
    commit->addWidget(m_save);

    m_savedTimer = new QTimer(this);
    m_savedTimer->setSingleShot(true);
    connect(m_savedTimer, &QTimer::timeout, this, [this] { m_savedNote->clear(); });

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(28, 22, 28, 22);
    column->setSpacing(16);
    column->addLayout(form);
    column->addStretch(1);
    column->addLayout(commit);

    connect(m_save, &QPushButton::clicked, this, &SettingsPage::apply);
    connect(m_browseButton, &QPushButton::clicked, this, &SettingsPage::browse);
    connect(m_detectButton, &QPushButton::clicked, this, &SettingsPage::autoDetect);
    connect(m_downloadButton, &QPushButton::clicked, this, &SettingsPage::downloadZapret);
    connect(m_pathEdit, &QLineEdit::textChanged, this, &SettingsPage::revalidatePath);
}

void SettingsPage::loadFromSettings()
{
    const QString path = m_settings.zapretPath();
    m_pathEdit->setText(path);
    m_startLastCheck->setChecked(m_settings.startLastOnLaunch());
    m_trayCheck->setChecked(m_settings.minimizeToTray());
    m_pingSpin->setValue(m_settings.pingIntervalSec());
    m_watchdogSpin->setValue(m_settings.watchdogIntervalMin());
    m_trayNote->setVisible(!QSystemTrayIcon::isSystemTrayAvailable());

    const QString theme = m_settings.theme();
    const int index = m_themeBox->findData(theme);
    m_themeBox->setCurrentIndex(index >= 0 ? index : 0);

    revalidatePath();
}

void SettingsPage::apply()
{
    // One guard around six writes: Settings answers every write with changed(), and the reload slot
    // connected in the constructor would otherwise re-read the form underneath the commit — harmless
    // for these six values, but it would drop the note this call is about to put up.
    m_writing = true;
    m_settings.setZapretPath(m_pathEdit->text().trimmed());
    m_settings.setStartLastOnLaunch(m_startLastCheck->isChecked());
    m_settings.setMinimizeToTray(m_trayCheck->isChecked());
    m_settings.setPingIntervalSec(m_pingSpin->value());
    m_settings.setWatchdogIntervalMin(m_watchdogSpin->value());
    m_settings.setTheme(m_themeBox->currentData().toString());
    m_writing = false;

    m_savedNote->setText(LanguageManager::tr(QStringLiteral("settings.saved")));
    m_savedTimer->start(kSavedNoteMs);
    emit saved();
}

QString SettingsPage::zapretPath() const
{
    return m_pathEdit->text().trimmed();
}

void SettingsPage::setZapretPath(const QString &path)
{
    m_pathEdit->setText(path);
}


bool SettingsPage::pathLooksValid(const QString &dir)
{
    if (dir.trimmed().isEmpty())
        return false;
    if (ZapretPaths::detectPlatform(dir) != Strategy::Platform::Unknown)
        return true;
    return !ZapretLocator::locate({dir}).isEmpty();
}

QString SettingsPage::describePath(const QString &dir)
{
    if (dir.trimmed().isEmpty())
        return LanguageManager::tr(QStringLiteral("path.none"));

    const Strategy::Platform direct = ZapretPaths::detectPlatform(dir);
    if (direct != Strategy::Platform::Unknown) {
        const QList<Strategy> found = StrategyParser::parseTree(dir, direct, 2);
        return LanguageManager::tr(QStringLiteral("path.found"))
            .arg(direct == Strategy::Platform::Windows ? QStringLiteral("windows")
                                                       : QStringLiteral("linux"),
                 QString::number(found.size()));
    }

    const QList<ZapretLocator::Found> roots = ZapretLocator::locate({dir});
    if (!roots.isEmpty()) {
        QStringList names;
        for (const ZapretLocator::Found &f : roots)
            names << (f.platform == Strategy::Platform::Windows ? QStringLiteral("windows")
                                                                : QStringLiteral("linux"));
        return LanguageManager::tr(QStringLiteral("path.base")).arg(names.join(QStringLiteral(", ")));
    }

    return LanguageManager::tr(QStringLiteral("path.not_zapret"));
}

void SettingsPage::revalidatePath()
{
    const QString dir = m_pathEdit->text().trimmed();
    const bool ok = pathLooksValid(dir);
    setPathState(ok, describePath(dir));
}

void SettingsPage::setPathState(bool ok, const QString &message)
{
    m_pathState->setText(ok ? QStringLiteral("✓ %1").arg(message)
                            : QStringLiteral("✕ %1").arg(message));
    m_pathState->setObjectName(ok ? QStringLiteral("validOk") : QStringLiteral("validBad"));
    m_pathState->style()->unpolish(m_pathState);
    m_pathState->style()->polish(m_pathState);
    m_pathState->update();

    m_pathEdit->setObjectName(ok ? QStringLiteral("pathOk") : QStringLiteral("pathBad"));
    m_pathEdit->style()->unpolish(m_pathEdit);
    m_pathEdit->style()->polish(m_pathEdit);
    m_pathEdit->update();
}

void SettingsPage::browse()
{
    const QString start = m_pathEdit->text().trimmed();
    const QString dir = QFileDialog::getExistingDirectory(
        this, LanguageManager::tr(QStringLiteral("settings.folder_title")),
        start.isEmpty() ? QDir::homePath() : start,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty())
        m_pathEdit->setText(QDir::toNativeSeparators(dir));
}

void SettingsPage::autoDetect()
{
    const QList<ZapretLocator::Found> found = ZapretLocator::locate();
    if (found.isEmpty()) {
        setPathState(false, LanguageManager::tr(QStringLiteral("path.not_found_manual")));
        return;
    }
    m_pathEdit->setText(QDir::toNativeSeparators(found.first().path));
}

void SettingsPage::downloadZapret()
{
    DownloadDialog dialog(this);
    connect(&dialog, &DownloadDialog::installed, this, [this](const QString &path) {
        m_pathEdit->setText(QDir::toNativeSeparators(path));
        // Said out loud rather than left to the textChanged handler: setText is a no-op when the field
        // already reads exactly this path, and the fresh tree still has to be described.
        revalidatePath();
    });
    dialog.startFetch();
    dialog.exec();
    // Nothing reaches Settings until Save is pressed: a download that lands in the field and a path the
    // application will actually use are two decisions, and this page owns the second.
}

} // namespace zapret

