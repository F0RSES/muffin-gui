#pragma once

#include "Strategy.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <QtGlobal>

namespace zapret {

// One downloadable zapret distribution, as advertised by a GitHub release.
//
// Everything here is metadata the GUI shows or validates against; no bytes are read from the network
// by this struct, and no path is touched. An release whose tagName is empty is the "nothing suitable
// found" answer, not a release with an empty tag.
struct ZapretRelease
{
    QString tagName;          // release tag, e.g. "v72.13" (bol-van) or "1.10.3" (Flowseal)
    QString version;          // display version: the tag, or the release name when tag_name is absent
    QString downloadUrl;      // assets[].browser_download_url of the chosen asset
    QString assetName;        // assets[].name of the chosen asset
    qint64 sizeBytes = 0;     // assets[].size of the chosen asset (0 when the API omitted it)
    QString sha256;           // lowercase hex WITHOUT the "sha256:" prefix; "" when not published
};

// Reads a GitHub releases/latest JSON document and picks THE asset this platform should install.
//
// The two feeds this application knows about are structurally different, and the picker has to know
// it rather than guess:
//
//   Flowseal/zapret-discord-youtube  -> .rar / .tar.gz / .zip, payload at bin/winws.exe
//   bol-van/zapret                  -> sha256sum.txt / *-openwrt-embedded.tar.gz / .tar.gz / .zip
//
// So extension priority alone is not enough on the linux feed: the very first tar.gz in assets[] is
// the OpenWrt/embedded firmware build, which ships no binaries/linux-x86_64/ at all. pickRelease()
// therefore also drops variant-suffixed assets, and drops .rar outright because no archiver shipped
// with Windows or a Linux distro can unpack it. Pure parsing — QtCore only, no network, testable
// against a captured fixture.
class ZapretSource
{
public:
    // releases/latest endpoints for the two supported distributions.
    static QString windowsApiUrl();
    static QString linuxApiUrl();
    // The feed for a platform (Windows -> Flowseal, everything else -> bol-van).
    static QString apiUrl(Strategy::Platform platform);

    // The platform whose archive we can actually run here. macOS answers Linux: the bol-van tarball is
    // the POSIX tree, and it is the only feed carrying non-Windows binaries.
    static Strategy::Platform hostPlatform();

    // Archive suffixes worth taking, best first, WITHOUT a leading dot:
    //   Windows -> {"zip", "tar.gz"}   Linux -> {"tar.gz", "zip"}
    static QStringList preferredExtensions(Strategy::Platform platform);
    static QStringList preferredExtensions();   // for hostPlatform()

    // "sha256:AB12..." -> "ab12...". Only a sha256-prefixed (or bare) 64-hex value qualifies: a digest
    // published under another algorithm returns "", so it is reported as unpublished rather than checked
    // with the wrong function and failing for the wrong reason.
    static QString normalizeDigest(const QString &digest);

    // Pick the asset to install. Empty release (tagName.isEmpty()) when the document has no usable
    // asset — including when it is an error body ({"message": "Not Found"}), which has no assets[].
    static ZapretRelease pickRelease(const QJsonObject &apiResponse, Strategy::Platform platform);
    static ZapretRelease pickRelease(const QJsonObject &apiResponse);   // for hostPlatform()

private:
    ZapretSource() = delete;

    // True when an asset name says "a different build for different hardware", not the main tree.
    static bool isVariantAsset(const QString &assetName);
};

} // namespace zapret

Q_DECLARE_METATYPE(zapret::ZapretRelease)
