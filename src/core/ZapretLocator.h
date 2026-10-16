#pragma once

#include "Strategy.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace zapret {

// Finds zapret distribution directories on disk and reports which platform each one is.
//
// The GUI needs to answer "where is zapret?" without the user typing a path. ZapretPaths knows
// the *internal* layout; ZapretLocator knows where such trees *live*. It walks a priority-ordered
// list of candidate roots and validates each with ZapretPaths::detectPlatform, so a directory is
// only reported if it really looks like a zapret tree.
class ZapretLocator
{
public:
    struct Found
    {
        QString path;                      // absolute, normalized directory path
        Strategy::Platform platform = Strategy::Platform::Unknown;
    };

    // Locate candidate roots. Priority order (first match wins for a given platform):
    //   1. $ZAPRET_ROOT (and its windows/ + linux/ children)
    //   2. each entry of extraRoots (and their children)
    //   3. well-known bases: ~/Documents/zapret, <cwd>/zapret, <cwd>, /opt/zapret
    // Each base is probed as itself plus any windows/ or linux/ child. Results are de-duplicated
    // by canonical path and keep the highest-priority occurrence.
    static QList<Found> locate(const QStringList &extraRoots = {});

    // Only the entries for a specific platform, preserving order.
    static QList<Found> filter(const QList<Found> &found, Strategy::Platform platform);

    // The first located root of `platform`, or "" if none was found.
    static QString firstRoot(const QList<Found> &found, Strategy::Platform platform);

    // The base directories searched when locate() is called with no extra roots. Exposed for tests.
    static QStringList defaultBases();

private:
    ZapretLocator() = delete;

    // Probe one directory: itself + its windows/ + linux/ children, appending validated entries.
    static void probeBase(const QString &base, QList<Found> &out);
};

} // namespace zapret
