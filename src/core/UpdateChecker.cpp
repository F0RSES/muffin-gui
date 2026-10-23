#include "UpdateChecker.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QUrl>

namespace zapret {

namespace {

// A release lookup sits on the GUI's startup path, so it is the one network call in this application
// that has to be short by construction: a stalled GitHub answer must not sit there holding anything,
// and 10 s is well past how long the API actually takes while still being a bounded wait.
constexpr int kLookupTimeoutMs = 10000;

// GitHub rejects an unauthenticated request that carries no User-Agent (403), and this is the string
// that shows up in the repository's traffic should anyone wonder what pulls its releases. The same
// spelling as ZapretDownloader's, for the same reason.
const char kUserAgent[] = "Muffin/0.1";

// The repository whose releases this build belongs to — spelled once, in one direction: apiUrl() and
// releasesPageUrl() both derive from it, so the machine-facing endpoint and the human-facing page can
// only ever name the same project.
constexpr char kRepository[] = "F0RSES/zapret-muffin-gui";

// Leading digits of one dotted component, or -1 when the component begins with no digit at all.
// Separate from the split so that "10-rc1" answers 10 and "rc1" answers -1 rather than a misleading 0:
// "there is no number here" and "the number is zero" are different statements, and only the second one
// is allowed to order a version.
int componentNumber(const QString &part)
{
    int index = 0;
    while (index < part.size() && part.at(index).isDigit())
        ++index;
    if (index == 0)
        return -1;
    bool ok = false;
    const int value = part.left(index).toInt(&ok);
    // A run of digits too long for an int is not a version this comparison can lose to, so it answers
    // as the largest number it can name rather than as the 0 toInt() returns on overflow.
    return ok ? value : 2147483647;
}

// The numeric components of a version: "0.4.10-rc1" -> {0, 4, 10}. An empty answer means the string
// names no version at all, which is what keeps garbage out of the ordering instead of letting it be
// compared against a real number.
QList<int> components(const QString &normalized)
{
    QList<int> out;
    const QStringList parts = normalized.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const int value = componentNumber(part);
        if (value < 0)
            break;            // a letter where a number belongs ends the numeric part of a version
        out.append(value);
    }
    return out;
}

} // namespace

QString UpdateChecker::repository()
{
    return QString::fromLatin1(kRepository);
}

QString UpdateChecker::apiUrl()
{
    return QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(repository());
}

QString UpdateChecker::releasesPageUrl()
{
    return QStringLiteral("https://github.com/%1/releases").arg(repository());
}

QString UpdateChecker::normalizeTag(const QString &tag)
{
    const QString trimmed = tag.trimmed();
    // Only a leading v/V, and only when a digit follows it: "v0.4.9" is the tag for 0.4.9, while "VS2019"
    // is a name whose first letter is not a marker — stripping it would invent a different version out of
    // a real one.
    if ((trimmed.startsWith(QLatin1Char('v')) || trimmed.startsWith(QLatin1Char('V')))
        && trimmed.size() > 1 && trimmed.at(1).isDigit())
        return trimmed.mid(1);
    return trimmed;
}

int UpdateChecker::compareVersions(const QString &left, const QString &right)
{
    const QList<int> a = components(normalizeTag(left));
    const QList<int> b = components(normalizeTag(right));
    // Either side naming no version at all means there is no order to report, and "no order" has to be 0
    // — the answer that keeps isNewer() false. A malformed tag must never look like an upgrade.
    if (a.isEmpty() || b.isEmpty())
        return 0;

    const int count = qMax(a.size(), b.size());
    for (int i = 0; i < count; ++i) {
        // A shorter version is not a lesser one: 0.4 is 0.4.0, because the missing component was never
        // written down rather than written as something smaller.
        const int x = i < a.size() ? a.at(i) : 0;
        const int y = i < b.size() ? b.at(i) : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

bool UpdateChecker::isNewer(const QString &tag, const QString &currentVersion)
{
    return compareVersions(tag, currentVersion) > 0;
}

QString UpdateChecker::tagFromRelease(const QJsonObject &apiResponse)
{
    const QString tag = apiResponse.value(QStringLiteral("tag_name")).toString().trimmed();
    if (!tag.isEmpty())
        return tag;
    // The fallback ZapretSource::pickRelease uses, for the same reason: a release can be published with
    // only a name, and "Release 2024-05" still answers "which version is this?".
    return apiResponse.value(QStringLiteral("name")).toString().trimmed();
}

QString UpdateChecker::downloadUrlFromRelease(const QJsonObject &apiResponse)
{
    const QString page = apiResponse.value(QStringLiteral("html_url")).toString().trimmed();
    if (!page.isEmpty())
        return page;
    // No page of its own — an older API shape, or a body that is a release in name only. The
    // repository's releases list is the right place to land from there, and it is never empty, so a
    // dialog built on this answer can never offer to open nothing.
    return releasesPageUrl();
}

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
{
}

UpdateChecker::~UpdateChecker()
{
    // Silence first, then abort, then let Qt delete. Without the disconnect the reply's finished() would
    // fire into finishRequest() while this object is being torn down — which is exactly what quitting the
    // application inside the startup window does: a signal emitted out of a destructor. The reply is the
    // manager's child and dies with it, so nothing leaks by leaving it here.
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply = nullptr;
    }
}

void UpdateChecker::setCurrentVersion(const QString &version)
{
    m_currentVersion = version;
}

QString UpdateChecker::currentVersion() const
{
    return m_currentVersion.isEmpty() ? QCoreApplication::applicationVersion() : m_currentVersion;
}

bool UpdateChecker::isChecking() const
{
    return m_reply != nullptr;
}

QString UpdateChecker::latestVersion() const
{
    return m_latestVersion;
}

void UpdateChecker::check()
{
    if (m_reply)
        return;   // one answer at a time; a second lookup could only race the first to the same fact

    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);

    // Read now and carry the value through the request: the answer belongs to the build that was
    // running when the question was asked, not to one a later setCurrentVersion() might name.
    const QString running = currentVersion();

    QNetworkRequest req{QUrl(apiUrl())};
    req.setTransferTimeout(kLookupTimeoutMs);
    req.setRawHeader("User-Agent", kUserAgent);
    // The stable REST shape rather than whatever the default media type would return, same as the
    // release fetches ZapretDownloader makes.
    req.setRawHeader("Accept", "application/vnd.github+json");

    QNetworkReply *reply = m_nam->get(req);
    m_reply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, running] {
        finishRequest(reply, running);
    });
}

void UpdateChecker::finishRequest(QNetworkReply *reply, const QString &currentVersion)
{
    // The stale reply is dropped without a word. abort() in the destructor disconnects before it can
    // get here, but a reply that is no longer the one in flight can also arrive from any other teardown
    // order, and reporting it would be reporting an answer to a question nobody is asking anymore.
    const bool stale = (reply != m_reply);
    if (!stale)
        m_reply = nullptr;
    reply->deleteLater();
    if (stale)
        return;

    const QByteArray body = reply->readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    const QJsonObject release = doc.isObject() ? doc.object() : QJsonObject();

    if (reply->error() != QNetworkReply::NoError) {
        // A 4xx arrives with error() set AND a body that explains it. "Not Found" (the repository was
        // renamed, so the endpoint is dead) or "API rate limit exceeded" (60 unauthenticated calls an
        // hour per address) is worth far more than Qt's transport-shaped string, and the difference is
        // what tells a user whether to care: a rate limit says wait, a 404 says this build's feed is
        // gone and no later version of it will ever be found there.
        const QString apiMessage = release.value(QStringLiteral("message")).toString().trimmed();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString detail = apiMessage.isEmpty()
                                   ? reply->errorString()
                                   : QStringLiteral("%1 (HTTP %2)").arg(apiMessage).arg(status);
        emit checkFailed(detail);
        return;
    }

    if (!doc.isObject()) {
        // 200 and not an object: a proxy's login page, an empty body, a captive portal. All of them
        // mean the same thing to the caller — no answer — and none of them is a missing update.
        emit checkFailed(QStringLiteral("the release feed did not return a release"));
        return;
    }

    const QString tag = tagFromRelease(release);
    if (tag.isEmpty()) {
        // A JSON object with neither tag_name nor name is not a release record, whatever the status
        // code said. Same treatment as an error body: report that nothing was learned.
        emit checkFailed(QStringLiteral("the release feed named no version"));
        return;
    }

    m_latestVersion = tag;

    // Compared against the normalized forms, so "v0.4.10" ahead of "0.4.9" reads as an update and
    // "v0.4.9" against "0.4.9" reads as the same build.
    if (isNewer(tag, currentVersion))
        emit updateAvailable(tag, downloadUrlFromRelease(release));
    else
        emit upToDate(tag);
}

} // namespace zapret

