#include "SettingsDialog.h"
#include "DownloadDialog.h"
#include "LanguageManager.h"
#include "Settings.h"
#include "StrategyParser.h"
#include "Theme.h"
#include "ZapretLocator.h"
#include "ZapretPaths.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QUrl>
#include <QVBoxLayout>

namespace zapret {

SettingsDialog::SettingsDialog(Settings &settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
{
    build();
    loadFromSettings();
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::build()
{
    // Same rule as the main window: a configuration sheet is a form with a known size, and stretching it
    // to the screen only spreads six rows over a thousand pixels of background.
    setFixedSize(620, 400);
    setWindowFlag(Qt::WindowMaximizeButtonHint, false);
    setModal(true);
    setWindowTitle(LanguageManager::tr(QStringLiteral("settings.title")));

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(QStringLiteral("/home/…/zapret/linux ")
                                   + LanguageManager::tr(QStringLiteral("common.or"))
                                   + QStringLiteral(" C:\\...\\zapret\\windows"));
    m_pathEdit->setClearButtonEnabled(true);

    i18nText(m_browseButton = new QPushButton(this), QStringLiteral("settings.browse"));
    i18nText(m_detectButton = new QPushButton(this), QStringLiteral("settings.autodetect"));
    i18nTip(m_detectButton, QStringLiteral("settings.autodetect_tip"));
    i18nText(m_downloadButton = new QPushButton(this), QStringLiteral("settings.download"));
    i18nTip(m_downloadButton, QStringLiteral("settings.download_tip"));

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
    m_themeBox->addItem(LanguageManager::tr(QStringLiteral("theme.dark")), Theme::nameForMode(Theme::Mode::Dark));
    m_themeBox->addItem(LanguageManager::tr(QStringLiteral("theme.light")), Theme::nameForMode(Theme::Mode::Light));
    m_themeBox->addItem(LanguageManager::tr(QStringLiteral("theme.system")), Theme::nameForMode(Theme::Mode::System));
    i18nItems(m_themeBox, { QStringLiteral("theme.dark"), QStringLiteral("theme.light"),
                            QStringLiteral("theme.system") });

    m_pingSpin = new QSpinBox(this);
    m_pingSpin->setRange(3, 60);
    m_pingSpin->setSuffix(LanguageManager::tr(QStringLiteral("settings.ping_suffix")));
    i18nSuffix(m_pingSpin, QStringLiteral("settings.ping_suffix"));
    m_pingSpin->setSingleStep(1);

    // Labels are built here rather than passed as strings to addRow so each one can carry the key it
    // came from; a QFormLayout's own label has no handle to tag.
    auto *pathRowLabel = i18nText(new QLabel(this), QStringLiteral("settings.path"));
    auto *themeRowLabel = i18nText(new QLabel(this), QStringLiteral("settings.theme"));
    auto *pingRowLabel = i18nText(new QLabel(this), QStringLiteral("settings.ping"));

    auto *form = new QFormLayout;
    form->setSpacing(10);
    form->addRow(pathRowLabel, pathRow);
    form->addRow(QString(), m_pathState);
    form->addRow(QString(), m_startLastCheck);
    form->addRow(QString(), m_trayCheck);
    form->addRow(QString(), m_trayNote);
    form->addRow(themeRowLabel, m_themeBox);
    form->addRow(pingRowLabel, m_pingSpin);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    i18nText(buttons->button(QDialogButtonBox::Ok), QStringLiteral("common.ok"));
    i18nText(buttons->button(QDialogButtonBox::Cancel), QStringLiteral("common.cancel"));

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(18, 18, 18, 16);
    column->setSpacing(14);
    column->addLayout(form);
    column->addStretch(1);
    column->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        apply();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_browseButton, &QPushButton::clicked, this, &SettingsDialog::browse);
    connect(m_detectButton, &QPushButton::clicked, this, &SettingsDialog::autoDetect);
    connect(m_downloadButton, &QPushButton::clicked, this, &SettingsDialog::downloadZapret);
    connect(m_pathEdit, &QLineEdit::textChanged, this, &SettingsDialog::revalidatePath);
}

void SettingsDialog::loadFromSettings()
{
    const QString path = m_settings.zapretPath();
    m_pathEdit->setText(path);
    m_startLastCheck->setChecked(m_settings.startLastOnLaunch());
    m_trayCheck->setChecked(m_settings.minimizeToTray());
    m_pingSpin->setValue(m_settings.pingIntervalSec());

    const QString theme = m_settings.theme();
    const int index = m_themeBox->findData(theme);
    m_themeBox->setCurrentIndex(index >= 0 ? index : 0);

    revalidatePath();
}

void SettingsDialog::apply()
{
    m_settings.setZapretPath(m_pathEdit->text().trimmed());
    m_settings.setStartLastOnLaunch(m_startLastCheck->isChecked());
    m_settings.setMinimizeToTray(m_trayCheck->isChecked());
    m_settings.setPingIntervalSec(m_pingSpin->value());
    m_settings.setTheme(m_themeBox->currentData().toString());
    emit applied();
}

QString SettingsDialog::zapretPath() const
{
    return m_pathEdit->text().trimmed();
}

void SettingsDialog::setZapretPath(const QString &path)
{
    m_pathEdit->setText(path);
}

bool SettingsDialog::pathLooksValid(const QString &dir)
{
    if (dir.trimmed().isEmpty())
        return false;
    if (ZapretPaths::detectPlatform(dir) != Strategy::Platform::Unknown)
        return true;
    return !ZapretLocator::locate({dir}).isEmpty();
}

QString SettingsDialog::describePath(const QString &dir)
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

void SettingsDialog::revalidatePath()
{
    const QString dir = m_pathEdit->text().trimmed();
    const bool ok = pathLooksValid(dir);
    setPathState(ok, describePath(dir));
}

void SettingsDialog::setPathState(bool ok, const QString &message)
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

void SettingsDialog::browse()
{
    const QString start = m_pathEdit->text().trimmed();
    const QString dir = QFileDialog::getExistingDirectory(
        this, LanguageManager::tr(QStringLiteral("settings.folder_title")),
        start.isEmpty() ? QDir::homePath() : start,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty())
        m_pathEdit->setText(QDir::toNativeSeparators(dir));
}

void SettingsDialog::autoDetect()
{
    const QList<ZapretLocator::Found> found = ZapretLocator::locate();
    if (found.isEmpty()) {
        setPathState(false, LanguageManager::tr(QStringLiteral("path.not_found_manual")));
        return;
    }
    m_pathEdit->setText(QDir::toNativeSeparators(found.first().path));
}

void SettingsDialog::downloadZapret()
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
    // Nothing is written to Settings until the user presses OK: a download that lands in the field and
    // a path the application will use are two decisions, and this dialog has always owned the second.
}

} // namespace zapret
