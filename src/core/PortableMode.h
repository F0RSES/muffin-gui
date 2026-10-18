#pragma once

#include "ZapretPaths.h"

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
//   StrategyParser  seeds Strategy::argv with `--qnum=Q --dpi-desync-fwmark=M` plus the desync policy
//                   the payload should start with (see portableNfqwsDefaultArgs).
//
// The queue number and the mark are therefore declared once, here. The policy flags are declared once
// too, in the same place, because they travel in the very same argv the two halves reconcile over.
//
// The mark IS zapret's own upstream default (`DESYNC_MARK=0x40000000`, init.d/sysv/functions), so a
// portable run and an installed zapret daemon agree on it and a user who later switches from one to
// the other does not have to reconcile two configurations.
//
// The queue number deliberately is NOT upstream's `QNUM=200`. 200 is the number every other zapret
// wrapper, fork and preinstalled service reaches for first, so on a machine where one of them is
// already running nfqws fails with "cannot open nfqueue group 200: Operation not permitted" — and it
// fails AFTER the GUI has inserted its rules, which is the worst possible moment: the user sees a
// dead run plus firewall rules they did not ask for. Upstream can afford 200 because its init script
// owns the machine; a GUI launched next to whatever the desktop already runs cannot. 2000 sits far
// above that crowd. A user who wants the upstream number back does not need a code change: editing
// `--qnum=` on the card moves the firewall rules with it, because portableQueueNum() reads the queue
// out of the same argv the payload gets.
//
// A third thing the payload needs and the firewall does not: `--user=` set to kPortableNfUser, so the
// child keeps the privileges creating the queue requires. See kPortableNfUser for why that is not
// optional, and for the one situation where adding it would break a run that otherwise works.
//
// All of this is header-only: nothing here spawns a process, so the core stays testable headlessly.

// The NFQUEUE number the portable backend binds and the firewall rules enqueue into. Not upstream's
// 200 — see the note at the top of this header for why that number is the one that collides.
inline constexpr int kPortableNfQueueNum = 2000;

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
//
// udp/80 is deliberately NOT here. Everything this profile desyncs speaks TLS or QUIC, and both land
// on 443; UDP/80 carries no client hello for `fake,split2` to imitate, so steering it only moved
// packets that could not benefit into a root-owned queue — and every extra pair is one more iptables
// rule that can collide with something else on the machine. Dropping it costs no flow the policy
// desyncs, and the firewall narrows along with --filter-udp because both read this one list.
inline const QList<QPair<QString, QString>> &portablePorts()
{
    static const QList<QPair<QString, QString>> ports = {
        {QStringLiteral("tcp"), QStringLiteral("80")},
        {QStringLiteral("tcp"), QStringLiteral("443")},
        {QStringLiteral("udp"), QStringLiteral("443")},
    };
    return ports;
}

// ---------------------------------------------------------------------------
// The desync policy the default argv carries
// ---------------------------------------------------------------------------
//
// `fake,split2` with md5sig was not enough against the DPI Russian ISPs run: the injected fake
// ClientHello was still matched, so a fresh install did nothing a user could feel. These are the
// knobs that make the same fake,split2 pair penetrate it, taken from zapret's own hard-DPI recipe and
// from the profile users had already verified by hand in a shell script.
//
//   fooling=ts            sets an impossible IPv4 timestamp rather than corrupting the TCP checksum.
//                         md5sig — the previous default — asks the receiving stack to drop the packet
//                         on a bad checksum, and a stack that drops it before the DPI has read the
//                         payload turns the desync into a stall. The timestamp id is ignored far more
//                         often than a bad checksum is.
//   repeats=6             resend the desynced packet, so one loss on the path is not the end of the
//                         handshake the DPI is supposed to be confused during.
//   fake-tls-mod=none     send the fake ClientHello byte-for-byte as the template holds it. nfqws' own
//                         default mutates random bytes, and a mutated hello is exactly what a DPI that
//                         learned to compare against a real handshake has started rejecting.
//   fake-tls / fake-quic  explicit templates, so what goes out looks like a genuine Chrome hello to
//                         www.google.com instead of whatever nfqws was built with.
inline constexpr const char *kPortableDesyncFooling = "ts";
inline constexpr int kPortableDesyncRepeats = 6;
inline constexpr const char *kPortableFakeTlsMod = "none";

// The two templates, and where they live. Upstream ships them in the release tree itself, so this is
// not a guess about someone else's layout: the archive that ZapretDownloader verifies and unpacks
// carries files/fake/, and the root helper copies the whole tree with `cp -a` when it promotes it.
inline constexpr const char *kPortableFakeFilesSubdir = "files/fake";
inline constexpr const char *kPortableFakeTlsFile = "tls_clienthello_www_google_com.bin";
inline constexpr const char *kPortableFakeQuicFile = "quic_initial_www_google_com.bin";

// The absolute path to one fake template, or empty when this host has no system tree at all.
//
// Why absolute: nfqws resolves a relative path against its cwd, and the backend starts the payload
// from the tree root — a directory the run happened to be located in, not something this header
// controls. Anchoring to ZapretPaths::systemInstallDir() instead means the payload, the polkit rule
// and the packager all read the one literal, and a template can never be sought inside a directory
// the unprivileged user can rewrite. Empty on Windows: no such tree, and no portable mode there.
inline QString portableFakeFilePath(const char *fileName)
{
    const QString tree = ZapretPaths::systemInstallDir();
    if (tree.isEmpty())
        return QString();
    return tree + QLatin1Char('/') + QLatin1String(kPortableFakeFilesSubdir) +
           QLatin1Char('/') + QLatin1String(fileName);
}

// The default nfqws argument vector for a portable run, seeded into Strategy::argv by the parser.
//
// Deliberately hostlist-free: an unpacked tree has no lists loaded and MODE_FILTER=none is upstream's
// own default for a quick run, so this desyncs every packet on the ports above. It is a starting
// point a user can edit, not a policy — which is why it is a plain QStringList on the Strategy rather
// than a hidden field the backend would invent at launch time.
//
// The two fake-template flags are the only entries that depend on the host: they are appended when
// there is a system tree to read them from, and left out when there is none. That is not decoration —
// nfqws fails the whole start on a --dpi-desync-fake-tls= it cannot open, while a flag that is simply
// absent makes it fall back to its built-in template and still run.
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

    QStringList argv;
    argv << QStringLiteral("--qnum=%1").arg(kPortableNfQueueNum)
         << QStringLiteral("--dpi-desync-fwmark=%1").arg(QLatin1String(kPortableNfDesyncMark))
         << QStringLiteral("--dpi-desync=fake,split2")
         << QStringLiteral("--dpi-desync-repeats=%1").arg(kPortableDesyncRepeats)
         << QStringLiteral("--dpi-desync-fooling=%1").arg(QLatin1String(kPortableDesyncFooling))
         << QStringLiteral("--dpi-desync-fake-tls-mod=%1").arg(QLatin1String(kPortableFakeTlsMod));

    const QString fakeTls = portableFakeFilePath(kPortableFakeTlsFile);
    if (!fakeTls.isEmpty())
        argv << QStringLiteral("--dpi-desync-fake-tls=%1").arg(fakeTls);
    const QString fakeQuic = portableFakeFilePath(kPortableFakeQuicFile);
    if (!fakeQuic.isEmpty())
        argv << QStringLiteral("--dpi-desync-fake-quic=%1").arg(fakeQuic);

    argv << QStringLiteral("--filter-tcp=%1").arg(tcp)
         << QStringLiteral("--filter-udp=%1").arg(udp);

    return argv;
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
