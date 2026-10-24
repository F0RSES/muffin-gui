#pragma once

#include "Strategy.h"
#include "ZapretSource.h"

#include <QObject>
#include <QString>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

namespace zapret {

// Fetches a release, downloads its archive, verifies it and installs the tree it contains.
//
// Everything is asynchronous and signal-driven so the modal DownloadDialog stays responsive and
// cancel() works at any point: fetch -> download -> verify -> extract -> install, each step advancing
// the stage and emitting. The object owns one operation at a time (isBusy()); a second call while one
// is running is ignored rather than queued, because two overlapping unpacks of the same directory
// could only corrupt each other.
//
// Where the bytes land, and why:
//   * the archive and the unpacked tree are staged in a fresh temporary directory created NEXT TO the
//     target (not in /tmp): the install step renames a whole directory into place, and a rename is
//     only atomic — and only possible at all — inside one filesystem.
//   * the existing install is renamed aside BEFORE the new one takes its place, and restored if that
//     second step fails. A download that dies halfway, a corrupt archive, a mismatched digest or an
//     unexpected tree layout therefore leave the previous working zapret exactly where it was, instead
//     of having deleted it first and handed the user an empty folder.
//   * success is only announced after the installed tree has been validated on disk, so the GUI never
//     writes a path into Settings that its own scanner would reject.
class ZapretDownloader : public QObject
{
    Q_OBJECT

public:
    // Which step is in flight. The values are also what the dialog's status line describes.
    enum class Stage
    {
        Idle,
        Fetching,     // asking the release API
        Downloading,  // streaming the archive to disk
        Verifying,    // hashing the archive
        Extracting,   // running the system archiver
        Promoting     // handing the installed tree to the root helper
    };
    Q_ENUM(Stage)

    explicit ZapretDownloader(QObject *parent = nullptr);
    ~ZapretDownloader() override;

    // The distribution feed and the layout validated after unpacking. Defaults to the host platform.
    void setPlatform(Strategy::Platform platform);
    Strategy::Platform platform() const;

    // Where the finished tree is installed. Defaults to installRoot().
    void setTargetDir(const QString &dir);
    QString targetDir() const;

    // ---- system-wide promotion ----
    //
    // Turn on, and a finished download ends up in ZapretPaths::systemInstallDir() - the one tree its
    // owner cannot rewrite - instead of staying in the managed directory the user owns.
    //
    // On POSIX that means handing the finished linux tree to the root helper (MuffinInstallHelper),
    // which copies it into the system directory as root:root with no group/world write bit. That tree
    // is the one packaging/postinst's polkit rule lets pkexec run WITHOUT a password; a copy in the
    // user's own app-data folder can never be authorised, because he owns the bytes there and could
    // swap in anything.
    //
    // On Windows it means unpacking the tree straight into C:\Program Files\Muffin\zapret. There is no
    // polkit to ask and no helper to exec: Muffin.exe is already elevated, because its manifest said
    // so before main() ran, and the installer's own QFile::rename is enough to put bytes where a
    // standard token may not write. Leaving them in %LOCALAPPDATA% instead is a local privilege
    // escalation - an unprivileged program rewrites winws.exe there, and the elevated window runs it.
    // See defaultInstallTarget().
    //
    // Off by default: installing system-wide is not something a library does behind a caller's back,
    // and a headless test that never opts in can therefore never reach pkexec. Promotion also never
    // fails a download - see promotionFailed.
    void setPromoteAfterInstall(bool on);
    bool promoteAfterInstall() const;

    Stage stage() const { return m_stage; }
    bool isBusy() const { return m_stage != Stage::Idle; }

    // ---- operations ----
    // Ask the release API for the newest usable archive. Emits releaseFetched() or fetchFailed().
    void fetchLatestRelease();
    // Download, verify, unpack and install `release`. Emits downloadProgress()/extractionStarted()
    // then downloadFinished() or downloadFailed().
    void download(const ZapretRelease &release);
    // Abort whatever is in flight, discard the staged bytes, emit cancelled(). Idempotent.
    void cancel();

    // ---- pure helpers: no socket, no subprocess, usable from tests ----
    // The directory this application installs into ("<AppDataLocation>/zapret"). The user owns every
    // byte of it, which is exactly why a promoted download does not stay there.
    static QString installRoot();
    // The tree a promotion writes into: ZapretPaths::systemInstallDir(). "" when this host has none.
    static QString promotionRoot();

    // Which of the two directories this download will be unpacked into, as a function of the answers
    // rather than of the environment, so a test can ask it about a host it is not standing on.
    //
    // POSIX always answers the managed directory. An unprivileged process cannot write to
    // /opt/muffin, so the tree is unpacked where it can be and then COPIED there by the root helper
    // through pkexec - the password is spent on one copy, and the payload is never exec'd from a
    // directory the user owns.
    //
    // Windows answers the system directory when promotion was asked for and this process is elevated,
    // because Muffin.exe carries requireAdministrator in its manifest: the elevation already happened,
    // before main() ran. There is no polkit to ask and no helper to exec, so the installer writes the
    // tree straight into Program Files with QFile::rename - which is the same transactional swap
    // installTree() already performs, only aimed somewhere a standard token cannot write. Nothing
    // user-writable is left behind, which is the point: an unprivileged process that can rewrite
    // winws.exe has a local privilege escalation the moment the elevated window starts it.
    static QString defaultInstallTarget(Strategy::Platform platform, bool promote, bool elevated);

    // The reason a requested promotion cannot happen on this host, or empty when it can. Split out
    // because "why did you not protect me?" is a decision the dialog reports, and a decision that only
    // exists inside a signal emission is a decision nobody can test.
    static QString promotionRefusal(Strategy::Platform platform, bool promote, bool elevated);

    // Lowercase hex sha256 of the file, or "" (with *ok == false) when it could not be read. Streamed,
    // so an 8 MB tarball is never held in memory.
    static QString sha256OfFile(const QString &path, bool *ok = nullptr);
    // True when the file's digest equals `expectedHex` (case-insensitive). An empty expectation means
    // "no digest was published", which is NOT a failure: it returns true and reports the real digest.
    static bool verifySha256(const QString &path, const QString &expectedHex,
                             QString *actualOut = nullptr, QString *errorOut = nullptr);

    // The archiver program that unpacks `archivePath` on this host, or "" when none exists. Probes the
    // candidates in the order the platform actually offers them.
    static QString extractorProgram(const QString &archivePath);
    // The arguments `program` needs to unpack archivePath into destDir.
    static QStringList extractorArguments(const QString &program, const QString &archivePath,
                                          const QString &destDir);

    // Does `root` hold the payload this platform can run? Windows wants bin/winws.exe; linux wants a
    // binaries/<arch>/nfqws. `missing` describes what was not there.
    static bool installLooksComplete(const QString &root, Strategy::Platform platform,
                                     QString *missing = nullptr);

signals:
    void releaseFetched(ZapretRelease release);
    void fetchFailed(QString message);
    // totalBytes is -1 when the server did not say, so the UI shows activity instead of a percentage.
    void downloadProgress(qint64 receivedBytes, qint64 totalBytes);
    void extractionStarted();
    // Emitted once, after the promotion attempt has resolved (or immediately when promotion is off),
    // and it names the tree the caller should use: the system directory when the root helper put one
    // there, the managed directory otherwise.
    void downloadFinished(QString installedPath);
    void downloadFailed(QString message);
    // The managed copy is installed and usable when this fires — promotion is an addition, never a
    // requirement, and a declined password prompt must not read as a failed download. The text says
    // which of the two it was (no helper installed, validation refused, pkexec exit status) and is the
    // reason the payload keeps asking for a password at start.
    void promotionFailed(QString message);
    void cancelled();

private:
    void setStage(Stage stage);
    void cleanup();                       // close/remove staging, drop reply + file + process
    void failAndCleanup(const QString &message);
    void beginExtraction();
    void installAndFinish(const QString &extractDir);
    // Move the unpacked tree into the target, keeping the previous install recoverable.
    bool installTree(const QString &payloadDir, const QString &target, QString *error);
    // The promotion phase. Resolves to downloadFinished itself, so installAndFinish returns to the
    // event loop instead of announcing an install that is not yet settled.
    void beginPromotion(const QString &managedDir);

    QNetworkAccessManager *m_nam = nullptr;   // created on first use, child of this
    QNetworkReply *m_reply = nullptr;         // non-null only during an in-flight request
    QFile *m_file = nullptr;                  // the staged archive, open while it is written
    QProcess *m_process = nullptr;            // the archiver, non-null only while it runs
    QString m_stagingPath;                    // the throwaway dir holding archive + unpack, "" when none

    ZapretRelease m_release;
    QString m_targetDir;
    Strategy::Platform m_platform = ZapretSource::hostPlatform();
    Stage m_stage = Stage::Idle;
    bool m_cancelRequested = false;
    qint64 m_received = 0;
    QString m_writeError;                   // a disk write that failed mid-download; reported at finish
    bool m_promoteAfterInstall = false;     // see setPromoteAfterInstall
    QString m_promoteFrom;                  // the managed dir the promotion was started from

};

} // namespace zapret

