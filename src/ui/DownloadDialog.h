#pragma once

#include "ZapretDownloader.h"
#include "ZapretSource.h"

#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;

namespace zapret {

// The download sheet: asks the release feed, shows what it found, then fetches/verifies/unpacks it.
//
// The constructor builds widgets and nothing else. The network request is startFetch(), called by
// whoever opens the dialog, and that split is load-bearing: it keeps the widget constructible in a
// headless test (and in a constructor chain that may never show it) instead of leaving a dialog whose
// first action is an un-cancellable request the caller cannot observe.
//
// The release answer is what enables the "Скачать" button — until it arrives there is no size, no
// digest and no url to act on, so an enabled button would be a button that does nothing.
class DownloadDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DownloadDialog(QWidget *parent = nullptr);
    ~DownloadDialog() override;

    // Ask the release feed for this platform. Safe to call again after a failure (that is what its
    // "Повторить" does).
    void startFetch();

    // Where the finished tree lands. Defaults to the managed install (<AppDataLocation>/zapret); tests
    // point it at a temporary directory.
    void setTargetDir(const QString &dir);
    QString targetDir() const;

    // Also hand a finished linux download to the root helper, so the payload ends up root-owned under
    // the system directory the polkit rule authorises. Forwarded to the downloader; off unless the
    // caller turns it on, because it is the one step in this dialog that costs a password.
    void setPromoteAfterInstall(bool on);
    bool promoteAfterInstall() const;

    // ---- read-only views used by tests ----
    QString installedPath() const;
    QString statusText() const;
    QString infoText() const;
    bool isDownloadEnabled() const;
    bool isBusy() const;
    ZapretDownloader *downloader() const { return m_downloader; }

    // "7.5 MiB" style size for the info line. Static: the wording is a decision, not a widget.
    static QString humanSize(qint64 bytes);

    // Did `path` arrive from the promotion into the system tree, or from the managed directory the
    // download unpacked into? The two need different wording — one can promise that starts will be
    // passwordless, the other has to explain why they will not — and reading the answer off the path
    // is the whole of that decision.
    //
    // Static, for humanSize()'s reason: it is a judgement about a report and not a widget. The
    // alternative is a test that cannot reach it at all, because the code that asks the question ends
    // in a modal QMessageBox — and a headless suite has nobody to click OK, so calling it hangs.
    //
    // An empty systemInstallDir() means this host has no promoted destination to reach (Windows); it
    // is read as "no such place", never as a path a report might match, so nothing is ever promoted
    // there and an unset `path` can never read as one.
    static bool isPromotedPath(const QString &path);

    // The completion message for a finished install: where it landed, followed by either the note that
    // starts will be passwordless (promoted) or the reason that promotion did not happen.
    //
    // `promotionNote` is the first line of the promotionFailed message, or empty when the promotion
    // succeeded or was never attempted. Passing it in rather than reading a member keeps the answer a
    // function of its arguments, which is what makes it checkable without driving a downloader.
    static QString installedMessageText(const QString &path, const QString &promotionNote);

signals:
    // A validated tree now sits at `path`. Named installed() instead of accepted(): QDialog already has
    // an argument-less accepted() that accept() fires, and overloading it would make every connect site
    // ambiguous while letting a plain "user closed the dialog" read as a successful install.
    void installed(QString path);

private slots:
    void onReleaseFetched(ZapretRelease release);
    void onFetchFailed(QString message);
    void onProgress(qint64 receivedBytes, qint64 totalBytes);
    void onExtractionStarted();
    void onDownloadFinished(QString path);
    void onDownloadFailed(QString message);
    void onPromotionFailed(QString message);
    void onCancelled();
    void chooseDownload();
    void chooseCancel();

private:
    void build();
    void setStatus(const QString &text);
    void setBusy(bool busy);
    bool confirmReplace(const QString &target);

    ZapretDownloader *m_downloader = nullptr;

    QLabel *m_info = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_downloadButton = nullptr;

    ZapretRelease m_release;
    QString m_installedPath;
    QString m_promotionNote;     // why the system-wide copy did not happen, if it did not
    bool m_replaceConfirmed = false;   // asked once per dialog; a retry does not ask again
};

} // namespace zapret
