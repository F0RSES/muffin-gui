#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace zapret {

// ---------------------------------------------------------------------------
// Portable linux mode: the shared vocabulary
// ---------------------------------------------------------------------------
//
// Portable mode (Strategy::RunMode::PortableNfqws) covers the case where the zapret tree was merely
// unpacked: install_easy.sh / install_bin.sh never ran, so <root>/nfq/nfqws does not exist and the
// sysv/openrc init script refuses to start. Instead of the daemon we exec the precompiled
// <root>/binaries/<arch>/nfqws ourselves as a foreground child, and we own the nfqueue rules for the
// lifetime of that child.
//
// Two halves of that must agree, and they live in different translation units:
//
//   LinuxBackend    inserts `-t mangle OUTPUT ... -j NFQUEUE --queue-num Q --queue-bypass` into both
//                   iptables and ip6tables, and excludes packets carrying mark M so our injected
//                   packets cannot loop.
//   StrategyParser  seeds Strategy::argv with `--qnum=Q --dpi-desync-fwmark=M`.
//
// The queue number and the mark are therefore declared once, here. The values are zapret's own
// upstream defaults (init.d/sysv/functions: `QNUM=200`, `DESYNC_MARK=0x40000000`), so a portable run
// and an installed zapret daemon agree on the numbers, and a user who later switches from one to the
// other does not have to reconcile two configurations.
//
// All of this is header-only: nothing here spawns a process, so the core stays testable headlessly.

// The NFQUEUE number the portable backend binds and the firewall rules enqueue into.
inline constexpr int kPortableNfQueueNum = 200;

// The firewall mark written on desync packets, formatted exactly the way both iptables (`--mark`)
// and nfqws (`--dpi-desync-fwmark`) accept it. zapret's upstream default.
inline constexpr const char *kPortableNfDesyncMark = "0x40000000";

// The transport/destination-port pairs portable mode steers into the queue. One source of truth for
// both the iptables rules and the nfqws filters, so the two can never drift apart.
inline const QList<QPair<QString, QString>> &portablePorts()
{
    static const QList<QPair<QString, QString>> ports = {
        {QStringLiteral("tcp"), QStringLiteral("80")},
        {QStringLiteral("tcp"), QStringLiteral("443")},
        {QStringLiteral("udp"), QStringLiteral("80")},
        {QStringLiteral("udp"), QStringLiteral("443")},
    };
    return ports;
}

// The default nfqws argument vector for a portable run, seeded into Strategy::argv by the parser.
//
// Deliberately hostlist-free: an unpacked tree has no lists loaded and MODE_FILTER=none is upstream's
// own default for a quick run, so this desyncs every packet on the ports above. It is a starting
// point a user can edit, not a policy — which is why it is a plain QStringList on the Strategy rather
// than a hidden field the backend would invent at launch time.
inline QStringList portableNfqwsDefaultArgs()
{
    QString tcp, udp;
    for (const auto &p : portablePorts())
    {
        QString &into = (p.first == QLatin1String("tcp")) ? tcp : udp;
        if (!into.isEmpty())
            into += QLatin1Char(',');
        into += p.second;
    }

    return {
        QStringLiteral("--qnum=%1").arg(kPortableNfQueueNum),
        QStringLiteral("--dpi-desync-fwmark=%1").arg(QLatin1String(kPortableNfDesyncMark)),
        QStringLiteral("--filter-tcp=%1").arg(tcp),
        QStringLiteral("--filter-udp=%1").arg(udp),
        QStringLiteral("--dpi-desync=fake,split2"),
        QStringLiteral("--dpi-desync-fooling=md5sig"),
    };
}

// The queue number a strategy's argv actually binds to: the last `--qnum=<digits>` wins (matching
// getopt_long's own last-occurrence behaviour), otherwise kPortableNfQueueNum.
//
// Anything non-numeric is rejected rather than passed through: the value is interpolated into a
// `pkexec sh -c` script that edits the firewall, so it must be provably a number.
inline int portableQueueNum(const QStringList &argv)
{
    int qnum = kPortableNfQueueNum;
    for (const QString &a : argv)
    {
        if (!a.startsWith(QLatin1String("--qnum=")))
            continue;
        const QString v = a.mid(int(QByteArrayLiteral("--qnum=").size()));
        if (v.isEmpty())
            continue;
        bool ok = false;
        const int parsed = v.toInt(&ok);
        if (ok && parsed >= 0 && parsed <= 65535)   // nfqws itself rejects anything outside 0..65535
            qnum = parsed;
    }
    return qnum;
}

} // namespace zapret
