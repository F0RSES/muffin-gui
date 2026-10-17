#include "ZapretSource.h"

#include <QJsonArray>
#include <QJsonValue>

namespace zapret {

namespace {

// "a b_c.tar.gz" -> {"a","b","c","tar","gz"}, all lowercase. Splitting into tokens (rather than
// looking for a substring) is what keeps the variant test honest: a hypothetical
// "winshowing-tools.zip" must not be disqualified because the letters of "ios" or "gpt" appear
// inside one of its words.
QStringList tokensOf(const QString &name)
{
    QStringList out;
    QString current;
    for (const QChar c : name)
    {
        if (c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.') ||
            c == QLatin1Char(' ') || c == QLatin1Char('(') || c == QLatin1Char(')'))
        {
            if (!current.isEmpty())
                out << current;
            current.clear();
        }
        else
        {
            current.append(c.toLower());
        }
    }
    if (!current.isEmpty())
        out << current;
    return out;
}

// Build flavours that live in the same release as the tree we want but are not it. Matched as whole
// tokens, so "zapret-v72.13-openwrt-embedded.tar.gz" is rejected while "zapret-v72.13.tar.gz" is not.
const QStringList &variantTokens()
{
    static const QStringList tokens = {
        QStringLiteral("openwrt"),
        QStringLiteral("embedded"),
        QStringLiteral("android"),
        QStringLiteral("gpt"),
        QStringLiteral("bsd"),
        QStringLiteral("macos"),
        QStringLiteral("ios"),
    };
    return tokens;
}

// The dotted suffixes (lowercase, leading dot) that count as one preferred extension. "tar.gz" also
// accepts ".tgz"; nothing else has aliases.
QStringList acceptedSuffixes(const QString &ext)
{
    if (ext == QLatin1String("tar.gz"))
        return {QStringLiteral(".tar.gz"), QStringLiteral(".tgz")};
    return {QLatin1Char('.') + ext};
}

bool isHex(const QString &s)
{
    for (const QChar c : s)
    {
        const bool digit = c >= QLatin1Char('0') && c <= QLatin1Char('9');
        const bool lower = c >= QLatin1Char('a') && c <= QLatin1Char('f');
        const bool upper = c >= QLatin1Char('A') && c <= QLatin1Char('F');
        if (!digit && !lower && !upper)
            return false;
    }
    return !s.isEmpty();
}

} // namespace

QString ZapretSource::windowsApiUrl()
{
    return QStringLiteral("https://api.github.com/repos/Flowseal/zapret-discord-youtube/releases/latest");
}

QString ZapretSource::linuxApiUrl()
{
    return QStringLiteral("https://api.github.com/repos/bol-van/zapret/releases/latest");
}

QString ZapretSource::apiUrl(Strategy::Platform platform)
{
    return platform == Strategy::Platform::Windows ? windowsApiUrl() : linuxApiUrl();
}

Strategy::Platform ZapretSource::hostPlatform()
{
#if defined(Q_OS_WIN)
    return Strategy::Platform::Windows;
#else
    return Strategy::Platform::Linux;
#endif
}

QStringList ZapretSource::preferredExtensions(Strategy::Platform platform)
{
    // Windows takes the zip first: bsdtar (tar.exe, present on every supported Windows) reads it
    // natively, and the zip is the asset Flowseal documents. .rar is deliberately absent — it is in
    // that release too, and no archiver shipping with an OS can unpack it.
    if (platform == Strategy::Platform::Windows)
        return {QStringLiteral("zip"), QStringLiteral("tar.gz")};
    // Linux takes the tarball first: it is the asset bol-van's own install scripts expect, and tar
    // preserves the permission bits that binaries/<arch>/nfqws needs.
    return {QStringLiteral("tar.gz"), QStringLiteral("zip")};
}

QStringList ZapretSource::preferredExtensions()
{
    return preferredExtensions(hostPlatform());
}

QString ZapretSource::normalizeDigest(const QString &digest)
{
    const QString trimmed = digest.trimmed();
    if (trimmed.isEmpty())
        return QString();

    // GitHub publishes "sha256:<hex>" today. A bare hex string is accepted as well, so a hand-written
    // manifest keeps working; anything else that carries a prefix is checked against the algorithm NAME,
    // because comparing a "sha512:..." value with a sha256 of the file would report a guaranteed mismatch
    // as tampering. With a prefix present and not sha256, or a remainder that is not 64 hex characters,
    // the answer is "" — no digest, and the downloader says so instead of guessing.
    QString hex = trimmed;
    if (trimmed.contains(QLatin1Char(':')))
    {
        const QString algo = trimmed.section(QLatin1Char(':'), 0, 0).trimmed().toLower();
        hex = trimmed.section(QLatin1Char(':'), -1).trimmed();
        if (algo != QLatin1String("sha256"))
            return QString();
    }

    if (hex.size() != 64 || !isHex(hex))
        return QString();
    return hex.toLower();
}

bool ZapretSource::isVariantAsset(const QString &assetName)
{
    const QStringList parts = tokensOf(assetName);
    for (const QString &token : variantTokens())
        if (parts.contains(token))
            return true;
    return false;
}

ZapretRelease ZapretSource::pickRelease(const QJsonObject &apiResponse, Strategy::Platform platform)
{
    ZapretRelease none;
    const QJsonArray assets = apiResponse.value(QStringLiteral("assets")).toArray();
    if (assets.isEmpty())
        return none;   // also the shape of an API error body: {"message": "Not Found"}

    for (const QString &ext : preferredExtensions(platform))
    {
        for (const QString &suffix : acceptedSuffixes(ext))
        {
            for (const QJsonValue &value : assets)
            {
                const QJsonObject asset = value.toObject();
                const QString name = asset.value(QStringLiteral("name")).toString();
                const QString url = asset.value(QStringLiteral("browser_download_url")).toString();
                if (name.isEmpty() || url.isEmpty())
                    continue;
                if (!name.endsWith(suffix, Qt::CaseInsensitive))
                    continue;
                if (isVariantAsset(name))
                    continue;

                ZapretRelease release;
                release.tagName = apiResponse.value(QStringLiteral("tag_name")).toString();
                if (release.tagName.isEmpty())
                    release.tagName = apiResponse.value(QStringLiteral("name")).toString();
                release.version = release.tagName;
                release.assetName = name;
                release.downloadUrl = url;
                release.sizeBytes = static_cast<qint64>(asset.value(QStringLiteral("size")).toDouble());
                release.sha256 = normalizeDigest(asset.value(QStringLiteral("digest")).toString());
                return release;
            }
        }
    }
    return none;
}

ZapretRelease ZapretSource::pickRelease(const QJsonObject &apiResponse)
{
    return pickRelease(apiResponse, hostPlatform());
}

} // namespace zapret

