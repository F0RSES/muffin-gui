#pragma once

#include <QObject>
#include <QString>

class QJsonObject;
class QNetworkAccessManager;
class QNetworkReply;

namespace zapret {

// Asks this application's own GitHub release feed whether a newer build exists.
//
// The object is split the way ZapretSource is split, and for the same reason: everything that DECIDES
// (what a tag means, whether it is newer than the version running, what the release JSON names as the
// page to open) is static and pure, so a test can hand it a captured API body and check the answer
// without a socket, a window or a rate limit. Only check() touches the network.
//
// The subject here is the GUI itself, not the zapret payload — ZapretDownloader::fetchLatestRelease()
// already owns the download of bol-van/Flowseal. This answers the different question "is the thing I
// am running the thing I shipped?", and its whole output is one URL: the release page. An automatic
// replace of a running application needs root (the payload tree is root-owned on Linux) and a
// transaction across three platforms, so the deliberate endpoint of this feature is a browser tab on
// the release page, which works on every platform and installs nothing behind the user's back.
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    // ---- the feed ----
    // The repository whose releases this build belongs to. One spelling, so the API endpoint and the
    // page the browser opens cannot drift apart into two different projects.
    static QString repository();
    // releases/latest for that repository — the endpoint check() GETs.
    static QString apiUrl();
    // The human-facing releases page, used when the API body names no html_url of its own.
    static QString releasesPageUrl();

    // ---- the pure decisions, all testable without a socket ----
    // "v0.4.10" / " V0.4.10 " -> "0.4.10". Tags carry a leading v and the application version does not,
    // so the two sides of a comparison are spelled alike before anything is compared.
    static QString normalizeTag(const QString &tag);
    // Component-wise numeric order of two version strings: -1 when `left` is older, 0 when the same,
    // 1 when newer. Short and long forms agree ("0.4" == "0.4.0"), a non-numeric tail is dropped
    // ("0.4.10-rc1" orders as "0.4.10"), and anything with no digits at all is treated as no version —
    // which is what makes an unparseable tag come back "not newer" instead of waking the user with a
    // nag about a build that is not actually ahead.
    static int compareVersions(const QString &left, const QString &right);
    // True when the release feed is ahead of the version this process is running.
    static bool isNewer(const QString &tag, const QString &currentVersion);
    // release/tag_name, falling back to release/name — the same order ZapretSource::pickRelease uses,
    // because a release published without a tag still says what version it is. "" for an error body
    // ({"message": "Not Found"}), which has neither.
    static QString tagFromRelease(const QJsonObject &apiResponse);
    // Where to take the user: the release's own html_url when the API gave one, the releases page
    // otherwise. Never empty, so the dialog always has something real to open.
    static QString downloadUrlFromRelease(const QJsonObject &apiResponse);

    // ---- the request ----
    // The version compared against. Defaults to QCoreApplication::applicationVersion(); set it to put
    // the decision under test rather than whatever the host's stamp says.
    void setCurrentVersion(const QString &version);
    QString currentVersion() const;

    // Ask the feed. A no-op while a request is in flight — one answer at a time, and two overlapping
    // lookups could only report the same fact twice.
    void check();
    bool isChecking() const;
    // The tag the last successful answer named, "" when nothing has come back yet.
    QString latestVersion() const;

signals:
    // The feed is ahead of this build. `version` is the tag as published (leading "v" and all, the way
    // the release page shows it) so the UI quotes the release rather than a re-spelling of it;
    // `downloadUrl` is the page to open.
    void updateAvailable(QString version, QString downloadUrl);
    // This build is the newest thing the feed has. Reported rather than silent: a check that answered
    // nothing and a check that answered "current" are different facts, and only one of them deserves a
    // line in the journal.
    void upToDate(QString version);
    // No answer at all — offline, refused, rate-limited, or a body that is not a release. The text
    // explains which, and nothing about the UI changes: a failed check is not a missing update.
    void checkFailed(QString message);

private:
    // `currentVersion` arrives with the request rather than being read when the answer lands: a check
    // that is already in flight must be judged against the version that was running when it was asked,
    // not against one a caller may have moved since.
    void finishRequest(QNetworkReply *reply, const QString &currentVersion);

    QNetworkAccessManager *m_nam = nullptr;   // created on first use, child of this
    QNetworkReply *m_reply = nullptr;         // non-null only during an in-flight request
    QString m_currentVersion;                 // empty -> read from QCoreApplication at check() time
    QString m_latestVersion;
};

} // namespace zapret
