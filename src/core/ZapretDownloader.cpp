#include "ZapretDownloader.h"

#include "MuffinInstallHelper.h"
#include "PrivilegeHelper.h"
#include "ZapretPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace zapret {

namespace {

// A release API answer that stalls is worse than one that fails: the dialog would sit there showing
// "Checking for the newest release" with Cancel as the only way out. GitHub answers in well under a
// second; 20 s is generous and 90 s still keeps a stalled 8 MB transfer from hanging the UI forever.
constexpr int kFetchTimeoutMs = 20000;
constexpr int kTransferTimeoutMs = 90000;

// GitHub rejects requests that carry no User-Agent (403), and the version string is what shows up in
// the two repositories' traffic if anyone ever wonders who is pulling their releases.
const char kUserAgent[] = "Muffin/0.1";

// ".tar.gz" is one suffix, not two: QString::endsWith(".gz") would happily accept a plain gzip.
QString archiveSuffix(const QString &path)
{
    const QString name = QFileInfo(path).fileName().toLower();
    if (name.endsWith(QStringLiteral(".tar.gz")))
        return QStringLiteral(".tar.gz");
    if (name.endsWith(QStringLiteral(".tgz")))
        return QStringLiteral(".tgz");
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    return dot < 0 ? QString() : name.mid(dot);
}

// The first candidate that resolves to a real executable on PATH. QStandardPaths::findExecutable is
// used rather than a bare program name so "no unzip here" is knowable BEFORE QProcess reports it as
// an opaque FailedToStart.
QString firstAvailable(const QStringList &candidates)
{
    for (const QString &c : candidates)
    {
        const QString found = QStandardPaths::findExecutable(c);
        if (!found.isEmpty())
            return found;
    }
    return QString();
}

// The name the archive is staged under inside the throwaway directory. The asset's OWN suffix is kept:
// extractorProgram() decides what can unpack a file by looking at its name, so a staged file called
// "download.tmp" would report "no archiver for a .tmp archive" for a perfectly good tar.gz.
QString stagedArchiveName(const ZapretRelease &release)
{
    QString suffix = archiveSuffix(release.assetName);
    if (suffix.isEmpty())
        suffix = archiveSuffix(QUrl(release.downloadUrl).path());
    return QStringLiteral("download") + (suffix.isEmpty() ? QStringLiteral(".bin") : suffix);
}

// Recursive copy used only when renaming across filesystem boundaries is impossible. Not atomic, and
// it does not reproduce ownership or timestamps — hence the fallback, not the fast path.
bool copyTree(const QString &from, const QString &to)
{
    QDir source(from);
    if (!source.exists())
        return false;
    if (!QDir().mkpath(to))
        return false;

    const QFileInfoList entries =
        source.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &fi : entries)
    {
        const QString dest = to + QLatin1Char('/') + fi.fileName();
        if (fi.isDir())
        {
            if (!copyTree(fi.absoluteFilePath(), dest))
                return false;
        }
        else
        {
            QFile::remove(dest);   // QFile::copy refuses to overwrite
            if (!QFile::copy(fi.absoluteFilePath(), dest))
                return false;
        }
    }
    return true;
}

} // namespace

ZapretDownloader::ZapretDownloader(QObject *parent)
    : QObject(parent)
{
}

ZapretDownloader::~ZapretDownloader()
{
    // A destroy must not leave a child process or an open file behind: abort the request, kill the
    // archiver, then drop the staged bytes.
    if (m_reply)
    {
        m_reply->abort();
        m_reply = nullptr;
    }
    if (m_process)
    {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
    cleanup();
}

void ZapretDownloader::setPlatform(Strategy::Platform platform)
{
    m_platform = platform;
}

Strategy::Platform ZapretDownloader::platform() const
{
    return m_platform;
}

void ZapretDownloader::setTargetDir(const QString &dir)
{
    m_targetDir = dir.trimmed();
}

QString ZapretDownloader::targetDir() const
{
    if (!m_targetDir.isEmpty())
        return m_targetDir;
    return defaultInstallTarget(m_platform, m_promoteAfterInstall, PrivilegeHelper::isElevated());
}

QString ZapretDownloader::installRoot()
{
    return ZapretPaths::managedInstallDir();
}

QString ZapretDownloader::promotionRoot()
{
    return ZapretPaths::systemInstallDir();
}

QString ZapretDownloader::defaultInstallTarget(Strategy::Platform platform, bool promote,
                                              bool elevated)
{
    if (!promote)
        return installRoot();

#if defined(Q_OS_WIN)
    // See the header: on Windows the choice IS the promotion. The tree is unpacked where it will be
    // run from, by a process that already holds the elevation, and installTree()'s rename puts it there
    // in one step - so there is never a second, user-writable copy to delete or to defend.
    if (platform == Strategy::Platform::Windows && elevated && !promotionRoot().isEmpty())
        return promotionRoot();
#else
    Q_UNUSED(platform);
    Q_UNUSED(elevated);
    // POSIX cannot write here at all from an unprivileged process, so the download always lands in the
    // managed directory and MuffinInstallHelper copies it afterwards.
#endif
    return installRoot();
}

QString ZapretDownloader::promotionRefusal(Strategy::Platform platform, bool promote,
                                           bool elevated)
{
    if (!promote)
        return QString();

#if defined(Q_OS_WIN)
    // The only promotion Windows can make is the one made while writing the tree into place, and that
    // needs the elevated token the manifest asks for. Saying so is the whole value of this message: a
    // payload that stayed in AppData looks exactly like an installed one, and only this sentence tells
    // the user that the copy a non-admin program can rewrite is the copy that will be run.
    if (platform == Strategy::Platform::Windows && !elevated)
    {
        return QStringLiteral(
            "ZapretDownloader: Muffin is not running as administrator, so zapret could not be "
            "installed into %1 and stays in %2. Any program running as this user can replace the "
            "payload there, and Muffin would then start the replacement with administrative rights - "
            "start Muffin with \"Run as administrator\" and download again to install it where that "
            "is not possible.")
            .arg(QDir::toNativeSeparators(promotionRoot()),
                 QDir::toNativeSeparators(installRoot()));
    }
#else
    Q_UNUSED(platform);
    Q_UNUSED(elevated);
    // POSIX has no such refusal: the root helper and pkexec are the elevation, so whether this
    // download can be promoted is a question MuffinInstallHelper answers in detail.
#endif
    return QString();
}

void ZapretDownloader::setPromoteAfterInstall(bool on)
{
    m_promoteAfterInstall = on;
}

bool ZapretDownloader::promoteAfterInstall() const
{
    return m_promoteAfterInstall;
}

QString ZapretDownloader::sha256OfFile(const QString &path, bool *ok)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (ok)
            *ok = false;
        return QString();
    }

    // addData(QIODevice*) hashes in 4 KiB draws internally, so this stays flat in memory no matter how
    // large the archive is. The result is identical to hash(file.readAll(), Sha256).
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
    {
        if (ok)
            *ok = false;
        return QString();
    }
    if (ok)
        *ok = true;
    return QString::fromLatin1(hash.result().toHex());
}

bool ZapretDownloader::verifySha256(const QString &path, const QString &expectedHex,
                                    QString *actualOut, QString *errorOut)
{
    bool ok = false;
    const QString actual = sha256OfFile(path, &ok);
    if (actualOut)
        *actualOut = actual;
    if (!ok)
    {
        if (errorOut)
            *errorOut = QStringLiteral("ZapretDownloader: cannot read %1 to verify it").arg(path);
        return false;
    }

    // A missing published digest is a gap in the release, not a reason to refuse the download: GitHub
    // only started carrying assets[].digest in 2022 and an older or mirrored release simply has none.
    if (expectedHex.isEmpty())
        return true;

    if (actual.compare(expectedHex, Qt::CaseInsensitive) != 0)
    {
        if (errorOut)
        {
            *errorOut = QStringLiteral(
                "SHA-256 mismatch: possible MITM or corrupted download\n"
                "expected %1\nactual   %2").arg(expectedHex.toLower(), actual);
        }
        return false;
    }
    return true;
}

QString ZapretDownloader::extractorProgram(const QString &archivePath)
{
    const QString suffix = archiveSuffix(archivePath);

#if defined(Q_OS_WIN)
    // tar.exe (bsdtar) has shipped with Windows since 1803 and reads both zip and tar.gz, so one
    // program covers either feed with nothing third-party to install.
    Q_UNUSED(suffix)
    return firstAvailable({QStringLiteral("tar"), QStringLiteral("tar.exe")});
#else
    if (suffix == QLatin1String(".tar.gz") || suffix == QLatin1String(".tgz") ||
        suffix == QLatin1String(".tar"))
        return firstAvailable({QStringLiteral("tar")});
    // unzip is what people expect for a zip, but a minimal system can lack it while having bsdtar, so
    // the latter stays a fallback rather than a hard requirement.
    if (suffix == QLatin1String(".zip"))
        return firstAvailable({QStringLiteral("unzip"), QStringLiteral("bsdtar")});
    return QString();
#endif
}

QStringList ZapretDownloader::extractorArguments(const QString &program, const QString &archivePath,
                                                 const QString &destDir)
{
    const QString exe = QFileInfo(program).fileName().toLower();
    const QString suffix = archiveSuffix(archivePath);

    if (exe == QLatin1String("unzip"))
        return {QStringLiteral("-q"), archivePath, QStringLiteral("-d"), destDir};
    // tar/bsdtar detect the compression from the payload, but -z for a tarball states what we mean.
    if (suffix == QLatin1String(".tar.gz") || suffix == QLatin1String(".tgz"))
        return {QStringLiteral("-xzf"), archivePath, QStringLiteral("-C"), destDir};
    return {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"), destDir};
}

bool ZapretDownloader::installLooksComplete(const QString &root, Strategy::Platform platform,
                                            QString *missing)
{
    if (root.isEmpty())
    {
        if (missing)
            *missing = QStringLiteral("no directory to check");
        return false;
    }

    if (platform == Strategy::Platform::Windows)
    {
        const QString winws = ZapretPaths::winwsPath(root);
        if (QFileInfo::exists(winws))
            return true;
        if (missing)
            *missing = QStringLiteral("bin/winws.exe is missing (%1)").arg(winws);
        return false;
    }

    // The linux tree is usable when a packaged binary for some architecture is there. The host-native
    // one is asked for first because that is what the backend will exec.
    const QStringList wants = {
        ZapretPaths::nfqwsArchPath(root, ZapretPaths::nativeArchDirName()),
        ZapretPaths::nfqwsArchPath(root, QStringLiteral("linux-x86_64")),
        ZapretPaths::nfqwsPath(root),
    };
    for (const QString &c : wants)
    {
        if (c.isEmpty())
            continue;
        const QFileInfo fi(c);
#if defined(Q_OS_WIN)
        // Inspecting a linux tree from Windows: the exec bit cannot be expressed there, so the presence
        // of the regular file is the whole check — the same asymmetry ZapretPaths documents.
        if (fi.isFile())
            return true;
#else
        if (fi.isFile() && fi.isExecutable())
            return true;
#endif
    }

    if (missing)
    {
        const QString arch = ZapretPaths::nativeArchDirName().isEmpty()
                                 ? QStringLiteral("linux-x86_64")
                                 : ZapretPaths::nativeArchDirName();
        *missing = QStringLiteral("binaries/%1/nfqws is missing or not executable").arg(arch);
    }
    return false;
}

void ZapretDownloader::setStage(Stage stage)
{
    m_stage = stage;
}

void ZapretDownloader::cleanup()
{
    if (m_file)
    {
        if (m_file->isOpen())
            m_file->close();
        delete m_file;
        m_file = nullptr;
    }
    if (!m_stagingPath.isEmpty())
    {
        QDir(m_stagingPath).removeRecursively();
        m_stagingPath.clear();
    }
    m_writeError.clear();
    m_received = 0;
}

void ZapretDownloader::failAndCleanup(const QString &message)
{
    cleanup();
    setStage(Stage::Idle);
    emit downloadFailed(message);
}

void ZapretDownloader::cancel()
{
    // Nothing in flight: a cancel is a no-op, not a cancelled() emission the dialog would have to
    // explain to the user.
    if (m_stage == Stage::Idle && !m_reply && !m_process)
        return;

    m_cancelRequested = true;

    // abort()/kill() both make their object emit finished(), and those handlers are what turn the
    // cancellation into the cancelled() signal — so a cancel never reports success twice.
    if (m_reply)
        m_reply->abort();
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_process->kill();

    // Verifying and installing are short local steps with nothing to abort; a cancel landing there is
    // resolved here, once the operation had no chance to notice it any other way.
    //
    // Promoting is deliberately NOT in the "nothing to abort" branch. The download it is following has
    // already succeeded — the tree is installed and validated — so what a cancel can mean here is only
    // "stop waiting for the password", and its finished() handler reports that as a finished install
    // with a failed promotion rather than as cancelled(), which would tell the user the download itself
    // never happened.
    if (m_stage != Stage::Fetching && m_stage != Stage::Downloading &&
        m_stage != Stage::Extracting && m_stage != Stage::Promoting)
    {
        m_cancelRequested = false;
        cleanup();
        setStage(Stage::Idle);
        emit cancelled();
    }
}

void ZapretDownloader::fetchLatestRelease()
{
    if (isBusy() || m_reply)
        return;   // one operation at a time; a lookup answering after a cancel would be useless anyway

    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);

    QNetworkRequest req{QUrl(ZapretSource::apiUrl(m_platform))};
    req.setTransferTimeout(kFetchTimeoutMs);
    // GitHub rejects an unauthenticated request with no User-Agent, and the Accept value keeps the
    // response on the stable REST shape rather than whatever the default preview would return.
    req.setRawHeader("User-Agent", kUserAgent);
    req.setRawHeader("Accept", "application/vnd.github+json");

    m_cancelRequested = false;
    setStage(Stage::Fetching);

    QNetworkReply *reply = m_nam->get(req);
    m_reply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const bool stale = (reply != m_reply);
        if (!stale)
            m_reply = nullptr;
        reply->deleteLater();
        if (stale || m_stage != Stage::Fetching)
            return;

        if (m_cancelRequested)
        {
            m_cancelRequested = false;
            setStage(Stage::Idle);
            emit cancelled();
            return;
        }

        const QByteArray body = reply->readAll();
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);

        if (reply->error() != QNetworkReply::NoError)
        {
            // A 4xx arrives with error() set AND a JSON body that actually explains it: "Not Found" or
            // "API rate limit exceeded" is worth far more than "Remote host closed the connection".
            const QString apiMessage =
                doc.isObject() ? doc.object().value(QStringLiteral("message")).toString() : QString();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString detail = apiMessage.isEmpty() ? reply->errorString()
                                                 : QStringLiteral("%1 (HTTP %2)").arg(apiMessage).arg(status);
            if (status == 403 || status == 429)
                detail += QStringLiteral(" - unauthenticated GitHub API access is rate-limited; try again later");
            setStage(Stage::Idle);
            emit fetchFailed(QStringLiteral("ZapretDownloader: release lookup failed: %1").arg(detail));
            return;
        }

        if (!doc.isObject())
        {
            setStage(Stage::Idle);
            emit fetchFailed(QStringLiteral("ZapretDownloader: the release API did not return a JSON object (%1)")
                                 .arg(parseError.error == QJsonParseError::NoError
                                          ? QStringLiteral("empty or non-JSON body")
                                          : parseError.errorString()));
            return;
        }

        const ZapretRelease release = ZapretSource::pickRelease(doc.object(), m_platform);
        setStage(Stage::Idle);
        if (release.tagName.isEmpty())
        {
            emit fetchFailed(QStringLiteral(
                "ZapretDownloader: the latest release publishes no archive this platform can install "
                "(looked for %1)")
                                 .arg(ZapretSource::preferredExtensions(m_platform)
                                          .join(QStringLiteral(", "))));
            return;
        }
        emit releaseFetched(release);
    });
}

void ZapretDownloader::download(const ZapretRelease &release)
{
    if (isBusy() || m_reply)
        return;   // a second unpack into the same directory could only corrupt the first

    if (release.downloadUrl.isEmpty())
    {
        emit downloadFailed(QStringLiteral("ZapretDownloader: the release carries no download url"));
        return;
    }

    const QString target = targetDir();
    if (target.isEmpty())
    {
        emit downloadFailed(
            QStringLiteral("ZapretDownloader: this host has no writable application-data location "
                           "to install zapret into"));
        return;
    }

    const QString parent = QFileInfo(target).absolutePath();
    if (!QDir().mkpath(parent))
    {
        emit downloadFailed(QStringLiteral("ZapretDownloader: cannot create %1").arg(parent));
        return;
    }

    // Staging sits NEXT TO the target rather than in /tmp. The install step renames a whole directory
    // into place: rename() neither crosses filesystems nor works onto an existing destination, and a
    // half-copied tree is exactly the state the swap is designed to avoid.
    static int sequence = 0;
    m_stagingPath = parent + QLatin1String("/.muffin-download-") +
                    QString::number(QCoreApplication::applicationPid()) + QLatin1Char('-') +
                    QString::number(++sequence);
    if (!QDir().mkpath(m_stagingPath))
    {
        m_stagingPath.clear();
        emit downloadFailed(QStringLiteral("ZapretDownloader: cannot create a staging directory"));
        return;
    }

    // The name is set before anything else: the staged file must keep the asset's suffix, or the
    // archiver chosen from it is the wrong one (see stagedArchiveName()).
    m_release = release;
    const QString archivePath = m_stagingPath + QLatin1Char('/') + stagedArchiveName(release);
    m_file = new QFile(archivePath, this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        const QString err = m_file->errorString();
        cleanup();
        emit downloadFailed(QStringLiteral("ZapretDownloader: cannot write %1 (%2)")
                                .arg(archivePath, err));
        return;
    }

    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);

    QNetworkRequest req{QUrl(release.downloadUrl)};
    req.setTransferTimeout(kTransferTimeoutMs);
    req.setRawHeader("User-Agent", kUserAgent);
    // github.com hands out release files through a redirect to a signed object URL; Qt follows it by
    // default, and this policy says "follow it, but never downgrade to a less safe scheme".
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    m_received = 0;
    m_writeError.clear();
    m_cancelRequested = false;
    setStage(Stage::Downloading);

    QNetworkReply *reply = m_nam->get(req);
    m_reply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (reply != m_reply || !m_file || !m_file->isOpen())
            return;
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;
        if (m_file->write(chunk) != chunk.size())
        {
            // A full disk or a removed directory. Say so, but do not abort from inside the reply's own
            // readyRead: finished() would then be emitted re-entrantly. One loop turn is enough.
            m_writeError = QStringLiteral("ZapretDownloader: cannot write the download (%1)")
                               .arg(m_file->errorString());
            m_file->close();
            QTimer::singleShot(0, reply, [reply]() {
                if (reply->isRunning())
                    reply->abort();
            });
            return;
        }
        m_received += chunk.size();
    });

    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 done, qint64 total) {
        if (reply != m_reply)
            return;
        // -1 rather than 0 when the server did not send Content-Length: "working" instead of "0 of 0".
        emit downloadProgress(done, total > 0 ? total : -1);
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, archivePath]() {
        const bool stale = (reply != m_reply);
        if (!stale)
            m_reply = nullptr;
        reply->deleteLater();
        if (stale || m_stage != Stage::Downloading)
            return;

        if (m_file && m_file->isOpen())
        {
            m_file->flush();
            m_file->close();
        }

        if (m_cancelRequested)
        {
            m_cancelRequested = false;
            cleanup();
            setStage(Stage::Idle);
            emit cancelled();
            return;
        }

        if (!m_writeError.isEmpty())
        {
            failAndCleanup(m_writeError);
            return;
        }

        if (reply->error() != QNetworkReply::NoError)
        {
            failAndCleanup(QStringLiteral("ZapretDownloader: download failed: %1")
                               .arg(reply->errorString()));
            return;
        }

        const QFileInfo staged(archivePath);
        if (!staged.isFile() || staged.size() == 0)
        {
            failAndCleanup(QStringLiteral("ZapretDownloader: the server returned an empty file"));
            return;
        }

        // Verify BEFORE unpacking: an archive that does not match the published digest is not the file
        // the release author shipped, and unpacking it would hand its contents to `tar`/`unzip` and then
        // to Settings as a zapret tree.
        setStage(Stage::Verifying);
        QString actual, error;
        if (!verifySha256(archivePath, m_release.sha256, &actual, &error))
        {
            failAndCleanup(error);
            return;
        }
        beginExtraction();
    });
}

void ZapretDownloader::beginExtraction()
{
    const QString archivePath = m_stagingPath + QLatin1Char('/') + stagedArchiveName(m_release);
    const QString program = extractorProgram(archivePath);
    if (program.isEmpty())
    {
        const QString suffix = archiveSuffix(archivePath);
        failAndCleanup(QStringLiteral(
            "ZapretDownloader: no archiver is available to unpack a %1 archive "
            "(needs tar, or unzip/bsdtar for zip)")
            .arg(suffix.isEmpty() ? QStringLiteral("compressed") : suffix.mid(1)));
        return;
    }

    const QString dest = m_stagingPath + QStringLiteral("/extract");
    if (!QDir().mkpath(dest))
    {
        failAndCleanup(QStringLiteral("ZapretDownloader: cannot create the unpack directory %1").arg(dest));
        return;
    }

    setStage(Stage::Extracting);
    emit extractionStarted();

    auto *proc = new QProcess(this);
    m_process = proc;
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProgram(program);
    proc->setArguments(extractorArguments(program, archivePath, dest));

    connect(proc, &QProcess::finished, this, [this, proc, dest](int code, QProcess::ExitStatus status) {
        if (m_process == proc)
            m_process = nullptr;
        const QString output = QString::fromLocal8Bit(proc->readAll()).trimmed();
        proc->deleteLater();

        if (m_cancelRequested)
        {
            m_cancelRequested = false;
            cleanup();
            setStage(Stage::Idle);
            emit cancelled();
            return;
        }
        if (m_stage != Stage::Extracting)
            return;   // errorOccurred already reported this run

        if (status != QProcess::NormalExit || code != 0)
        {
            failAndCleanup(QStringLiteral("ZapretDownloader: unpacking failed (exit code %1)%2")
                               .arg(code)
                               .arg(output.isEmpty() ? QString()
                                                     : QStringLiteral("\n%1").arg(output.left(500))));
            return;
        }
        installAndFinish(dest);
    });

    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        // FailedToStart is the only failure that never produces finished(); everything else arrives there.
        if (e != QProcess::FailedToStart)
            return;
        if (m_process == proc)
            m_process = nullptr;
        const QString text = proc->errorString();
        proc->deleteLater();
        if (m_stage == Stage::Extracting)
            failAndCleanup(QStringLiteral("ZapretDownloader: the archiver could not be started (%1)").arg(text));
    });

    proc->start();
}

void ZapretDownloader::installAndFinish(const QString &extractDir)
{
    // Both feeds wrap the whole tree in one folder named after the release
    // (zapret-v72.13/, zapret-discord-youtube-1.10.3/). Unwrap it: the installed root must contain
    // bin/winws.exe or init.d/ directly, because that is the level ZapretPaths is written against.
    QString payload = extractDir;
    const QFileInfoList entries =
        QDir(extractDir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    if (entries.size() == 1 && entries.first().isDir())
        payload = entries.first().absoluteFilePath();

#if !defined(Q_OS_WIN)
    // tar carries the mode bits; a zip unpack can lose them. An nfqws that cannot be exec'd, or an init
    // script pkexec refuses, is a tree whose every strategy fails at start with a message the user
    // cannot trace back to an archive format — so restore the bit before validating.
    auto fixExec = [](const QString &path) {
        const QFileInfo fi(path);
        if (!fi.isFile() || fi.isExecutable())
            return;
        QFile::setPermissions(path, fi.permissions() | QFileDevice::ExeOwner |
                                        QFileDevice::ExeGroup | QFileDevice::ExeOther);
    };
    QDir binaries(ZapretPaths::binariesDir(payload));
    if (binaries.exists())
    {
        const QFileInfoList arches = binaries.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &arch : arches)
            fixExec(arch.absoluteFilePath() + QStringLiteral("/nfqws"));
    }
    fixExec(ZapretPaths::nfqwsPath(payload));
    fixExec(ZapretPaths::nfqwsBuiltPath(payload));
    fixExec(ZapretPaths::sysvScriptPath(payload));
    fixExec(ZapretPaths::openrcScriptPath(payload));
#endif

    // Validate the PAYLOAD before the incumbent is touched: a release that changes its layout must not
    // be able to cost the user the tree that currently works.
    QString missing;
    if (!installLooksComplete(payload, m_platform, &missing))
    {
        failAndCleanup(QStringLiteral("ZapretDownloader: the unpacked archive is not a usable %1 zapret tree: %2")
                           .arg(m_platform == Strategy::Platform::Windows ? QStringLiteral("windows")
                                                                          : QStringLiteral("linux"),
                                missing));
        return;
    }

    const QString target = targetDir();
    if (!installTree(payload, target, &missing))
    {
        failAndCleanup(missing);
        return;
    }

    cleanup();
    // Two ways a finished download becomes a system-wide one, and the choice of which is the
    // platform's, not the caller's.
    //
    // Windows decided during the install. defaultInstallTarget() aimed the whole unpack at
    // C:\Program Files\Muffin\zapret when promotion was asked for and this process holds the
    // administrator token, so `target` already IS the system tree and there is nobody left to hand it
    // to. What remains is saying why it could not, when the user asked and the token was not there.
    //
    // POSIX cannot write to /opt/muffin from an unprivileged process at all, so the tree landed in the
    // managed directory and the root helper is the only way it moves. That call owns the end of the
    // operation - it emits downloadFinished itself, because the path worth reporting depends on
    // whether the promotion landed.
    if (m_promoteAfterInstall)
    {
        const QString refusal = promotionRefusal(m_platform, true, PrivilegeHelper::isElevated());
        if (!refusal.isEmpty())
        {
            // The download is complete and usable either way, exactly as a declined pkexec prompt
            // leaves it: promotion is an addition, never a requirement.
            setStage(Stage::Idle);
            emit promotionFailed(refusal);
            emit downloadFinished(target);
            return;
        }
        if (m_platform == Strategy::Platform::Linux)
        {
            beginPromotion(target);
            return;
        }
    }

    setStage(Stage::Idle);
    emit downloadFinished(target);
}

void ZapretDownloader::beginPromotion(const QString &managedDir)
{
    QString program;
    QStringList args;
    QString why;
    if (!MuffinInstallHelper::composePromote(managedDir, &program, &args, &why))
    {
        // Nothing to attempt: no helper installed, no pkexec, or the tree failed the source checks.
        // The download is complete and usable either way, which is why this path reports and finishes
        // rather than failing — the managed copy is exactly what every earlier build used.
        setStage(Stage::Idle);
        emit promotionFailed(why);
        emit downloadFinished(managedDir);
        return;
    }

    m_promoteFrom = managedDir;
    setStage(Stage::Promoting);

    auto *proc = new QProcess(this);
    m_process = proc;
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProgram(program);
    proc->setArguments(args);

    connect(proc, &QProcess::finished, this,
            [this, proc, managedDir](int code, QProcess::ExitStatus status) {
        if (m_process == proc)
            m_process = nullptr;
        const QString output = QString::fromLocal8Bit(proc->readAll()).trimmed();
        proc->deleteLater();
        if (m_stage != Stage::Promoting)
            return;   // errorOccurred already reported this run, or a destroy took it

        // A pkexec the user walked away from exits non-zero (polkit's NOT_AUTHORIZED / an
        // authentication failure), and so does a helper that refused the tree. Both mean the same
        // thing to the caller: the managed install stands, the system-wide one does not.
        if (status != QProcess::NormalExit || code != 0)
        {
            setStage(Stage::Idle);
            emit promotionFailed(QStringLiteral(
                                     "ZapretDownloader: the system-wide install into %1 did not happen "
                                     "(exit code %2), so zapret keeps asking for a password when it "
                                     "starts from the managed folder.%3")
                                     .arg(ZapretPaths::systemInstallDir(), QString::number(code),
                                          output.isEmpty() ? QString()
                                                           : QStringLiteral("\n%1").arg(output.left(500))));
            emit downloadFinished(managedDir);
            return;
        }

        // Report the tree that was just made root-owned, so the caller stores the path whose payload
        // the polkit rule authorises without a password.
        setStage(Stage::Idle);
        m_promoteFrom.clear();
        emit downloadFinished(ZapretPaths::systemInstallDir());
    });

    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        // FailedToStart is the only failure that never produces finished(): here that means pkexec
        // itself vanished between the existence check and the exec.
        if (e != QProcess::FailedToStart)
            return;
        if (m_process == proc)
            m_process = nullptr;
        const QString text = proc->errorString();
        proc->deleteLater();
        if (m_stage != Stage::Promoting)
            return;
        const QString from = m_promoteFrom;
        m_promoteFrom.clear();
        setStage(Stage::Idle);
        emit promotionFailed(QStringLiteral(
                                 "ZapretDownloader: the root helper could not be started (%1); the "
                                 "download is installed and works, it just keeps asking for a "
                                 "password when it starts.").arg(text));
        emit downloadFinished(from);
    });

    proc->start();
}

bool ZapretDownloader::installTree(const QString &payloadDir, const QString &target, QString *error)
{
    const QFileInfo targetInfo(target);
    if (!targetInfo.absoluteDir().exists() && !QDir().mkpath(targetInfo.absolutePath()))
    {
        if (error)
            *error = QStringLiteral("ZapretDownloader: cannot create %1").arg(targetInfo.absolutePath());
        return false;
    }

    const bool replacing = QDir(target).exists();
    const QString backup =
        targetInfo.absolutePath() + QLatin1String("/.muffin-previous-") +
        QString::number(QCoreApplication::applicationPid());

    if (replacing)
    {
        QDir(backup).removeRecursively();   // debris from a run that died mid-swap, if any
        // Move the incumbent aside rather than delete it: everything that can still go wrong (a refused
        // rename, an interrupted copy) then has something to put back.
        if (!QFile::rename(target, backup))
        {
            if (error)
            {
                *error = QStringLiteral(
                    "ZapretDownloader: the current zapret folder could not be moved aside - is a "
                    "strategy started from it still running? (%1)").arg(target);
            }
            return false;
        }
    }

    bool installed = QFile::rename(payloadDir, target);
    if (!installed)
    {
        // Only reachable when something forbids the rename. Slower and not atomic, so it stays the
        // fallback instead of the default.
        installed = copyTree(payloadDir, target);
        if (installed)
            QDir(payloadDir).removeRecursively();
    }

    if (!installed)
    {
        QString note;
        if (replacing && !QFile::rename(backup, target))
            note = QStringLiteral(" - the previous installation is still under %1").arg(backup);
        if (error)
            *error = QStringLiteral("ZapretDownloader: cannot install the unpacked tree into %1%2")
                         .arg(target, note);
        return false;
    }

    if (replacing)
        QDir(backup).removeRecursively();   // the replacement is in place; the incumbent is spent
    return true;
}

} // namespace zapret







