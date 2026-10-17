#include "SettingsDialog.h"
#include "DownloadDialog.h"
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
    setWindowTitle(QStringLiteral("Настройки Muffin"));
    setModal(true);

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(QStringLiteral("/home/…/zapret/linux или C:\\…\\zapret\\windows"));
    m_pathEdit->setClearButtonEnabled(true);

    m_browseButton = new QPushButton(QStringLiteral("Обзор…"), this);
    m_detectButton = new QPushButton(QStringLiteral("Автопоиск"), this);
    m_detectButton->setToolTip(QStringLiteral("Найти дерево zapret в $ZAPRET_ROOT, ~/Documents/zapret, /opt/zapret"));
    m_downloadButton = new QPushButton(QStringLiteral("Скачать…"), this);
    m_downloadButton->setToolTip(QStringLiteral("Загрузить последнюю версию zapret с GitHub в папку приложения"));

    auto *pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->setSpacing(6);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(m_browseButton);
    pathRow->addWidget(m_detectButton);
    pathRow->addWidget(m_downloadButton);

    m_pathState = new QLabel(this);
    m_pathState->setWordWrap(true);

    m_startLastCheck = new QCheckBox(QStringLiteral("Запускать последнюю стратегию при старте"), this);
    m_trayCheck = new QCheckBox(QStringLiteral("Сворачивать в трей при закрытии"), this);

    // A ticked box that cannot be honoured is worse than an explanation. Sessions without a StatusNotifier
    // host (bare GNOME, a plain Wayland compositor) report no tray at all, so spell out what closing the
    // window will actually do — MainWindow still retries, so the tray may appear later by itself.
    m_trayNote = new QLabel(QStringLiteral(
        "В этой среде нет области уведомлений: закрытие окна остановит zapret и выйдет из приложения.\n"
        "Настройка сохранена — если панель запустится позже, трей появится сам."), this);
    m_trayNote->setObjectName(QStringLiteral("trayNote"));
    m_trayNote->setWordWrap(true);
    m_trayNote->setVisible(!QSystemTrayIcon::isSystemTrayAvailable());

    m_themeBox = new QComboBox(this);
    m_themeBox->addItem(QStringLiteral("Тёмная"), Theme::nameForMode(Theme::Mode::Dark));
    m_themeBox->addItem(QStringLiteral("Светлая"), Theme::nameForMode(Theme::Mode::Light));
    m_themeBox->addItem(QStringLiteral("Системная"), Theme::nameForMode(Theme::Mode::System));

    m_pingSpin = new QSpinBox(this);
    m_pingSpin->setRange(3, 60);
    m_pingSpin->setSuffix(QStringLiteral(" сек"));
    m_pingSpin->setSingleStep(1);

    auto *form = new QFormLayout;
    form->setSpacing(10);
    form->addRow(QStringLiteral("Путь к zapret"), pathRow);
    form->addRow(QString(), m_pathState);
    form->addRow(QString(), m_startLastCheck);
    form->addRow(QString(), m_trayCheck);
    form->addRow(QString(), m_trayNote);
    form->addRow(QStringLiteral("Тема"), m_themeBox);
    form->addRow(QStringLiteral("Интервал пинга"), m_pingSpin);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));

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
        return QStringLiteral("Путь не выбран.");

    const Strategy::Platform direct = ZapretPaths::detectPlatform(dir);
    if (direct != Strategy::Platform::Unknown) {
        const QList<Strategy> found = StrategyParser::parseTree(dir, direct, 2);
        return QStringLiteral("Найдено: %1, стратегий: %2")
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
        return QStringLiteral("Базовая папка: найдены %1").arg(names.join(QStringLiteral(", ")));
    }

    return QStringLiteral("Это не дерево zapret: нет ни bin/winws.exe + *.bat, ни init.d/nfqws.");
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
        this, QStringLiteral("Папка с zapret"),
        start.isEmpty() ? QDir::homePath() : start,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty())
        m_pathEdit->setText(QDir::toNativeSeparators(dir));
}

void SettingsDialog::autoDetect()
{
    const QList<ZapretLocator::Found> found = ZapretLocator::locate();
    if (found.isEmpty()) {
        setPathState(false, QStringLiteral("Zapret не найден — укажите путь вручную."));
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
