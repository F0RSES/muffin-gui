#include "DownloadDialog.h"

#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

namespace zapret {

DownloadDialog::DownloadDialog(QWidget *parent)
    : QDialog(parent)
{
    // Parented to the dialog on purpose: closing the window destroys the downloader, whose destructor
    // aborts the in-flight request and kills the archiver. A dialog the user walked away from cannot
    // keep writing into their AppData afterwards.
    m_downloader = new ZapretDownloader(this);
    build();
}

DownloadDialog::~DownloadDialog() = default;

void DownloadDialog::build()
{
    setWindowTitle(QStringLiteral("Скачать zapret"));
    setModal(true);
    setMinimumWidth(480);

    m_info = new QLabel(QStringLiteral("Сведения о релизе появятся после проверки GitHub."), this);
    m_info->setObjectName(QStringLiteral("muted"));
    m_info->setWordWrap(true);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);

    m_status = new QLabel(QStringLiteral("Ожидание."), this);
    m_status->setWordWrap(true);

    m_cancelButton = new QPushButton(QStringLiteral("Отмена"), this);

    m_downloadButton = new QPushButton(QStringLiteral("Скачать"), this);
    m_downloadButton->setObjectName(QStringLiteral("primaryButton"));
    m_downloadButton->setDefault(true);
    // Nothing to act on yet: no url, no size, no digest. Enabling it here would be a button whose only
    // answer is silence.
    m_downloadButton->setEnabled(false);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_cancelButton);
    buttons->addWidget(m_downloadButton);

    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(18, 18, 16, 16);
    column->setSpacing(12);
    column->addWidget(m_info);
    column->addWidget(m_progress);
    column->addWidget(m_status);
    column->addStretch(1);
    column->addLayout(buttons);

    connect(m_downloadButton, &QPushButton::clicked, this, &DownloadDialog::chooseDownload);
    connect(m_cancelButton, &QPushButton::clicked, this, &DownloadDialog::chooseCancel);

    connect(m_downloader, &ZapretDownloader::releaseFetched, this, &DownloadDialog::onReleaseFetched);
    connect(m_downloader, &ZapretDownloader::fetchFailed, this, &DownloadDialog::onFetchFailed);
    connect(m_downloader, &ZapretDownloader::downloadProgress, this, &DownloadDialog::onProgress);
    connect(m_downloader, &ZapretDownloader::extractionStarted, this, &DownloadDialog::onExtractionStarted);
    connect(m_downloader, &ZapretDownloader::downloadFinished, this, &DownloadDialog::onDownloadFinished);
    connect(m_downloader, &ZapretDownloader::downloadFailed, this, &DownloadDialog::onDownloadFailed);
    connect(m_downloader, &ZapretDownloader::cancelled, this, &DownloadDialog::onCancelled);
}

QString DownloadDialog::humanSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("неизвестно");
    if (bytes < 1024)
        return QStringLiteral("%1 Б").arg(bytes);
    const double ki = bytes / 1024.0;
    if (ki < 1024.0)
        return QStringLiteral("%1 КиБ").arg(ki, 0, 'f', 0);
    const double mi = ki / 1024.0;
    if (mi < 1024.0)
        return QStringLiteral("%1 МиБ").arg(mi, 0, 'f', 1);
    return QStringLiteral("%1 ГиБ").arg(mi / 1024.0, 0, 'f', 1);
}

void DownloadDialog::startFetch()
{
    if (m_downloader->isBusy())
        return;
    setStatus(QStringLiteral("Проверяю последнюю версию на GitHub…"));
    m_downloader->fetchLatestRelease();
}

void DownloadDialog::setTargetDir(const QString &dir)
{
    m_downloader->setTargetDir(dir);
}

QString DownloadDialog::targetDir() const
{
    return m_downloader->targetDir();
}

QString DownloadDialog::installedPath() const
{
    return m_installedPath;
}

QString DownloadDialog::statusText() const
{
    return m_status->text();
}

QString DownloadDialog::infoText() const
{
    return m_info->text();
}

bool DownloadDialog::isDownloadEnabled() const
{
    return m_downloadButton->isEnabled();
}

bool DownloadDialog::isBusy() const
{
    return m_downloader->isBusy();
}

void DownloadDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}

void DownloadDialog::setBusy(bool busy)
{
    m_downloadButton->setEnabled(!busy);
    m_cancelButton->setEnabled(true);
}

void DownloadDialog::onReleaseFetched(ZapretRelease release)
{
    m_release = release;

    // Say what will be fetched AND where it will go: this is the one moment the user can still notice
    // they were about to write into an unexpected folder.
    QString text = QStringLiteral("Релиз %1\nФайл: %2, %3\nУстановить в: %4")
                       .arg(release.version.isEmpty() ? QStringLiteral("(без имени)") : release.version,
                            release.assetName,
                            humanSize(release.sizeBytes),
                            QDir::toNativeSeparators(targetDir()));
    // GitHub has published assets[].digest since 2022 but not on every asset, and an older or mirrored
    // release has none at all. Refusing to download then would be worse than saying so plainly.
    text += release.sha256.isEmpty()
                ? QStringLiteral("\nSHA-256 для этого файла не опубликован — сверка невозможна.")
                : QStringLiteral("\nSHA-256 будет сверен с опубликованным.");

    m_info->setText(text);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    setStatus(QStringLiteral("Готово к скачиванию."));
    m_downloadButton->setEnabled(true);
}

void DownloadDialog::onFetchFailed(QString message)
{
    setStatus(message.section(QLatin1Char('\n'), 0, 0));
    const auto answer = QMessageBox::critical(
        this, QStringLiteral("Скачать zapret"),
        QStringLiteral("Не удалось узнать последнюю версию:\n\n%1").arg(message),
        QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Retry)
        startFetch();
}

void DownloadDialog::onProgress(qint64 receivedBytes, qint64 totalBytes)
{
    if (totalBytes > 0)
    {
        if (m_progress->maximum() != 100)
            m_progress->setRange(0, 100);
        const qint64 percent = receivedBytes * 100 / totalBytes;
        m_progress->setValue(static_cast<int>(percent > 100 ? 100 : percent));
        setStatus(QStringLiteral("Скачано %1 из %2…").arg(humanSize(receivedBytes), humanSize(totalBytes)));
    }
    else
    {
        // No Content-Length: a bar that cannot be filled must not pretend to be sitting at 0%.
        if (m_progress->maximum() != 0)
            m_progress->setRange(0, 0);
        setStatus(QStringLiteral("Скачано %1…").arg(humanSize(receivedBytes)));
    }
}

void DownloadDialog::onExtractionStarted()
{
    // The digest matched and unpacking began; the rest of the work has no byte count to show.
    m_progress->setRange(0, 0);
    setStatus(QStringLiteral("SHA-256 совпал. Распаковываю…"));
}

void DownloadDialog::onDownloadFinished(QString path)
{
    m_installedPath = path;
    m_progress->setRange(0, 100);
    m_progress->setValue(100);
    setStatus(QStringLiteral("Установлено в %1").arg(QDir::toNativeSeparators(path)));
    setBusy(false);

    QMessageBox::information(
        this, QStringLiteral("Скачать zapret"),
        QStringLiteral("zapret установлен в:\n%1").arg(QDir::toNativeSeparators(path)));

    // Announced before accept() so the owner holds the path while this dialog is still the window any
    // follow-up message belongs to.
    emit installed(path);
    accept();
}

void DownloadDialog::onDownloadFailed(QString message)
{
    setBusy(false);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    setStatus(message.section(QLatin1Char('\n'), 0, 0));

    // The full text, not its first line: an exit code or an expected/actual digest pair is the whole
    // value of the message. Retry stays possible, so a transient loss costs one click instead of a new
    // round trip to the release API.
    QMessageBox::critical(this, QStringLiteral("Скачать zapret"),
                          QStringLiteral("Скачивание не удалось:\n\n%1").arg(message));
    m_downloadButton->setEnabled(!m_release.downloadUrl.isEmpty());
}

void DownloadDialog::onCancelled()
{
    setBusy(false);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    setStatus(QStringLiteral("Отменено."));
    reject();
}

bool DownloadDialog::confirmReplace(const QString &target)
{
    QDir dir(target);
    if (!dir.exists())
        return true;
    // Hidden entries count: a leftover ".muffin-previous-<pid>" is a directory the swap needs, and
    // calling the folder empty while it is not would misdescribe what is about to be deleted.
    const QStringList entries = dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden);
    if (entries.isEmpty())
        return true;

    const auto answer = QMessageBox::question(
        this, QStringLiteral("Скачать zapret"),
        QStringLiteral("В %1 уже есть файлы (%2).\n\n"
                       "Скачивание установит туда новую копию zapret; старые файлы будут удалены "
                       "после того, как проверка подтвердит новое дерево.\n\nПродолжить?")
            .arg(QDir::toNativeSeparators(target), QString::number(entries.size())),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer == QMessageBox::Yes;
}

void DownloadDialog::chooseDownload()
{
    if (m_downloader->isBusy() || m_release.downloadUrl.isEmpty())
        return;

    const QString target = targetDir();
    if (target.isEmpty())
    {
        QMessageBox::critical(this, QStringLiteral("Скачать zapret"),
                              QStringLiteral("Эта система не даёт доступный каталог данных приложения, "
                                             "куда можно установить zapret."));
        return;
    }

    // Asked once for the life of the dialog: retrying after a network hiccup is not a second decision
    // about the same folder the user already agreed to replace.
    if (!m_replaceConfirmed && !confirmReplace(target))
        return;
    m_replaceConfirmed = true;

    setBusy(true);
    m_progress->setRange(0, m_release.sizeBytes > 0 ? 100 : 0);
    m_progress->setValue(0);
    setStatus(QStringLiteral("Скачиваю…"));
    m_downloader->download(m_release);
}

void DownloadDialog::chooseCancel()
{
    if (!m_downloader->isBusy())
    {
        reject();
        return;
    }
    // Stop offering both actions while the abort unwinds; cancelled() closes the dialog when it lands.
    m_cancelButton->setEnabled(false);
    m_downloadButton->setEnabled(false);
    setStatus(QStringLiteral("Отменяю…"));
    m_downloader->cancel();
}

} // namespace zapret


