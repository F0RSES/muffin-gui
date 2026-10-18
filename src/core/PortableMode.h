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
// A third thing the payload needs and the firewall does not: `--user=` set to kPortableNfUser, so the
// child keeps the privileges creating the queue requires. See kPortableNfUser for why that is not
// optional, and for the one situation where adding it would break a run that otherwise works.
//
// All of this is header-only: nothing here spawns a process, so the core stays testable headlessly.

// The NFQUEUE number the portable backend binds and the firewall rules enqueue into.
inline constexpr int kPortableNfQueueNum = 200;

// The firewall mark written on desync packets, formatted exactly the way both iptables (`--mark`)
// and nfqws (`--dpi-desync-fwmark`) accept it. zapret's upstream default.
inline constexpr const char *kPortableNfDesyncMark = "0x40000000";

// The account the payload must stay as when it is started with root privileges.
//
// nfqws drops root by itself, and the drop is not optional: main() asks can_drop_root() — which on
// Linux is "do I hold CAP_SETUID|CAP_SETGID", and a pkexec'd child always does — and then sets
// `params.uid = params.gid[0] = 0x7FFFFFFF` with droproot = true (nfq/nfqws.c:2348). 0x7FFFFFFF is not
// a user at all; it is zapret's sentinel for "nobody-ish", and it is what the log line
// "Running as UID=2147483647 GID=2147483647" reports.
//
// That drop happens before the queue is created (nfq/nfqws.c:302 runs droproot() then dropcaps(), and
// only then nfq_init()), so by the time nfqws asks the kernel for a queue it is no longer root. The
// resulting log pair is the signature of the bug: "Running as UID=2147483647 GID=2147483647" straight
// into "nfq_create_queue(): Operation not permitted", exit 1 — portable mode inserted its rules and
// started nothing.
//
// `--user=root` is what upstream itself does with the same option (init.d/sysv/functions:64 builds
// `USEROPT="--user=$WS_USER"`): IDX_USER resolves the name with getpwnam() and reuses it for uid/gid,
// so droproot() ends up calling initgroups("root", 0), setgid(0), setuid(0) on a process that is
// already uid 0. All three succeed and change nothing, the capability set is untouched, and
// nfq_create_queue() still holds CAP_NET_ADMIN. The log then reads "Running as UID=0 GID=0": the drop
// ran, and it dropped to root.
//
// It is therefore ONLY correct for a child that really is root. For an unprivileged nfqws the same
// flag sets droproot = true with uid 0 as the target, initgroups("root") / setuid(0) fail with EPERM,
// and nfqws exits 1 — turning a working unprivileged run into a broken one. That is why
// portableArgvWithUser() is applied behind the backend's elevation gate rather than baked into the
// default argv every strategy inherits.
inline constexpr const char *kPortableNfUser = "root";

// The argv-level switches that choose a privilege endpoint: `--user=<name>` (also in its separated
// `--user <name>` form, which nfqws' getopt_long accepts because the argument is required), and
// `--uid=uid[:gid,...]` / `--uid <n>`. Both set params.droproot themselves.
inline bool portableArgvSelectsPrivileges(const QStringList &argv)
{
    for (const QString &a : argv)
    {
        if (a.startsWith(QLatin1String("--user")) || a.startsWith(QLatin1String("--uid")))
            return true;
    }
    return false;
}

// The argv with `--user=root` appended, unless it already picks its own privilege endpoint.
//
// A user who edits the flags in the GUI may deliberately drop privileges (zapret's own service does,
// and it is the safer shape); that choice is theirs and is never overridden — an existing --user or
// --uid wins, exactly as it would if we appended anyway. An argv that says nothing about privileges
// gets kPortableNfUser appended LAST, after the DPI policy, so it reads as what it is: the launch
// wrapper's concern, not part of the desync profile.
inline QStringList portableArgvWithUser(const QStringList &argv)
{
    if (portableArgvSelectsPrivileges(argv))
        return argv;
    QStringList out = argv;
    out << QStringLiteral("--user=%1").arg(QLatin1String(kPortableNfUser));
    return out;
}

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
