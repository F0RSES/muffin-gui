#include "DownloadDialog.h"
#include "LanguageManager.h"

#include "ZapretPaths.h"

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
    setWindowTitle(LanguageManager::tr(QStringLiteral("action.download_zapret")));
    setModal(true);
    setMinimumWidth(480);

    m_info = new QLabel(LanguageManager::tr(QStringLiteral("dl.info")), this);
    m_info->setObjectName(QStringLiteral("muted"));
    m_info->setWordWrap(true);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);

    m_status = new QLabel(LanguageManager::tr(QStringLiteral("dl.waiting")), this);
    m_status->setWordWrap(true);

    m_cancelButton = new QPushButton(LanguageManager::tr(QStringLiteral("common.cancel")), this);

    m_downloadButton = new QPushButton(LanguageManager::tr(QStringLiteral("dl.download_button")), this);
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
    connect(m_downloader, &ZapretDownloader::promotionFailed, this, &DownloadDialog::onPromotionFailed);
    connect(m_downloader, &ZapretDownloader::cancelled, this, &DownloadDialog::onCancelled);
}

QString DownloadDialog::humanSize(qint64 bytes)
{
    if (bytes < 0)
        return LanguageManager::tr(QStringLiteral("size.unknown"));
    if (bytes < 1024)
        return LanguageManager::tr(QStringLiteral("size.bytes")).arg(bytes);
    const double ki = bytes / 1024.0;
    if (ki < 1024.0)
        return LanguageManager::tr(QStringLiteral("size.kib")).arg(ki, 0, 'f', 0);
    const double mi = ki / 1024.0;
    if (mi < 1024.0)
        return LanguageManager::tr(QStringLiteral("size.mib")).arg(mi, 0, 'f', 1);
    return LanguageManager::tr(QStringLiteral("size.gib")).arg(mi / 1024.0, 0, 'f', 1);
}

void DownloadDialog::startFetch()
{
    if (m_downloader->isBusy())
        return;
    setStatus(LanguageManager::tr(QStringLiteral("dl.checking")));
    m_downloader->fetchLatestRelease();
}

void DownloadDialog::setTargetDir(const QString &dir)
{
    m_downloader->setTargetDir(dir);
}

void DownloadDialog::setPromoteAfterInstall(bool on)
{
    m_downloader->setPromoteAfterInstall(on);
}

bool DownloadDialog::promoteAfterInstall() const
{
    return m_downloader->promoteAfterInstall();
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
    QString text = LanguageManager::tr(QStringLiteral("dl.release"))
                       .arg(release.version.isEmpty()
                                ? LanguageManager::tr(QStringLiteral("dl.unnamed"))
                                : release.version,
                            release.assetName,
                            humanSize(release.sizeBytes),
                            QDir::toNativeSeparators(targetDir()));
    // GitHub has published assets[].digest since 2022 but not on every asset, and an older or mirrored
    // release has none at all. Refusing to download then would be worse than saying so plainly.
    text += release.sha256.isEmpty()
                ? LanguageManager::tr(QStringLiteral("dl.no_sha"))
                : LanguageManager::tr(QStringLiteral("dl.sha_will_check"));

    m_info->setText(text);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    setStatus(LanguageManager::tr(QStringLiteral("dl.ready")));
    m_downloadButton->setEnabled(true);
}

void DownloadDialog::onFetchFailed(QString message)
{
    setStatus(message.section(QLatin1Char('\n'), 0, 0));
    const auto answer = QMessageBox::critical(
        this, LanguageManager::tr(QStringLiteral("action.download_zapret")),
        LanguageManager::tr(QStringLiteral("dl.version_failed")).arg(message),
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
        setStatus(LanguageManager::tr(QStringLiteral("dl.downloading_of")).arg(humanSize(receivedBytes), humanSize(totalBytes)));
    }
    else
    {
        // No Content-Length: a bar that cannot be filled must not pretend to be sitting at 0%.
        if (m_progress->maximum() != 0)
            m_progress->setRange(0, 0);
        setStatus(LanguageManager::tr(QStringLiteral("dl.downloading_short")).arg(humanSize(receivedBytes)));
    }
}

void DownloadDialog::onExtractionStarted()
{
    // The digest matched and unpacking began; the rest of the work has no byte count to show.
    m_progress->setRange(0, 0);
    setStatus(LanguageManager::tr(QStringLiteral("dl.sha_ok")));
}

void DownloadDialog::onPromotionFailed(QString message)
{
    // Kept, not shown here: downloadFinished follows in the same signal sequence, and one dialog that
    // says "installed here, and here is why it is not installed system-wide" is worth more than two
    // that a user has to click through in order to learn the same thing.
    m_promotionNote = message.section(QLatin1Char('\n'), 0, 0);
    setStatus(m_promotionNote);
}

void DownloadDialog::onDownloadFinished(QString path)
{
    m_installedPath = path;
    m_progress->setRange(0, 100);
    m_progress->setValue(100);
    setStatus(LanguageManager::tr(QStringLiteral("dl.installed_in")).arg(QDir::toNativeSeparators(path)));
    setBusy(false);

    // The path decides the message. A promoted install lands in the root-owned tree whose payload the
    // polkit rule exempts, so starts are passwordless; anything else is the managed folder, which
    // works exactly the same but asks for the password each time.
    const bool promoted =
        !ZapretPaths::systemInstallDir().isEmpty() && path == ZapretPaths::systemInstallDir();
    QString text = LanguageManager::tr(QStringLiteral("dl.installed_msg"))
                       .arg(QDir::toNativeSeparators(path));
    if (promoted)
    {
        text += LanguageManager::tr(QStringLiteral("dl.root_note"));
    }
    else if (!m_promotionNote.isEmpty())
    {
        text += QStringLiteral("\n\n%1").arg(m_promotionNote);
    }

    QMessageBox::information(this, LanguageManager::tr(QStringLiteral("action.download_zapret")), text);

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
    QMessageBox::critical(this, LanguageManager::tr(QStringLiteral("action.download_zapret")),
                          LanguageManager::tr(QStringLiteral("dl.failed")).arg(message));
    m_downloadButton->setEnabled(!m_release.downloadUrl.isEmpty());
}

void DownloadDialog::onCancelled()
{
    setBusy(false);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    setStatus(LanguageManager::tr(QStringLiteral("dl.cancelled")));
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
        this, LanguageManager::tr(QStringLiteral("action.download_zapret")),
        LanguageManager::tr(QStringLiteral("dl.replace_question"))
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
        QMessageBox::critical(this, LanguageManager::tr(QStringLiteral("action.download_zapret")),
                              LanguageManager::tr(QStringLiteral("dl.no_data_dir")));
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
    setStatus(LanguageManager::tr(QStringLiteral("dl.downloading")));
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
    setStatus(LanguageManager::tr(QStringLiteral("dl.cancelling")));
    m_downloader->cancel();
}

} // namespace zapret


