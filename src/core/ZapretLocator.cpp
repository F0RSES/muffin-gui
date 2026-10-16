#include "ZapretLocator.h"

#include "ZapretPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

namespace zapret {

namespace {

QString canonical(const QString &path)
{
    return QFileInfo(path).absoluteFilePath();
}

void appendUnique(QList<ZapretLocator::Found> &out, const QString &path,
                  Strategy::Platform platform)
{
    const QString canon = canonical(path);
    for (const auto &f : out)
        if (f.path == canon)
            return;   // keep the highest-priority occurrence
    out.append({canon, platform});
}

// Consider one directory: itself, plus a windows/ and a linux/ child, validating each.
void probeOne(const QString &candidate, QList<ZapretLocator::Found> &out)
{
    const Strategy::Platform p = ZapretPaths::detectPlatform(candidate);
    if (p != Strategy::Platform::Unknown)
        appendUnique(out, candidate, p);
}

} // namespace

QStringList ZapretLocator::defaultBases()
{
    QStringList bases;
    bases << QDir::homePath() + QStringLiteral("/Documents/zapret");
    bases << QDir::homePath() + QStringLiteral("/Downloads/zapret");
    bases << QDir::currentPath() + QStringLiteral("/zapret");
    bases << QDir::currentPath();
    bases << QStringLiteral("/opt/zapret");
    bases << QStringLiteral("/usr/local/zapret");
    return bases;
}

void ZapretLocator::probeBase(const QString &base, QList<Found> &out)
{
    if (base.isEmpty())
        return;
    // The base itself may be a windows/ or linux/ tree (or a single-platform unpack).
    probeOne(base, out);
    // Common convention: a parent holding both windows/ and linux/ siblings.
    probeOne(base + QStringLiteral("/windows"), out);
    probeOne(base + QStringLiteral("/linux"), out);
}

QList<ZapretLocator::Found> ZapretLocator::locate(const QStringList &extraRoots)
{
    QList<Found> found;

    // 1. Explicit override wins over everything.
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString envRoot = env.value(QStringLiteral("ZAPRET_ROOT"));
    if (!envRoot.isEmpty())
        probeBase(envRoot, found);

    // 2. Caller-supplied hints.
    for (const QString &r : extraRoots)
        probeBase(r, found);

    // 3. Well-known bases.
    for (const QString &b : defaultBases())
        probeBase(b, found);

    return found;
}

QList<ZapretLocator::Found> ZapretLocator::filter(const QList<Found> &found,
                                                  Strategy::Platform platform)
{
    QList<Found> out;
    for (const Found &f : found)
        if (f.platform == platform)
            out.append(f);
    return out;
}

QString ZapretLocator::firstRoot(const QList<Found> &found, Strategy::Platform platform)
{
    for (const Found &f : found)
        if (f.platform == platform)
            return f.path;
    return QString();
}

} // namespace zapret
