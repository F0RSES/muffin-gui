# zapret distribution analysis & execution model

Source of truth: the real trees at `~/Documents/zapret/{windows,linux}`. Everything the core
library hardcodes is an *exclusion* (helper names / payload dirs); strategy selection itself is
always derived from what is on disk. This document records the findings that shaped
`StrategyParser`, `ZapretPaths`, and `ZapretLocator`.

## 1. Layout differences

### Windows (`windows/`) — flat, wrapper-driven
```
windows/
  general.bat              <- STRATEGY  (the curated default)
  general (ALT).bat        <- STRATEGY  (13 ALT tuning variants)
  general (ALT2..13).bat
  general (EXP).bat
  general (FAKE TLS AUTO...).bat, general (SIMPLE FAKE...).bat   <- STRATEGY
  service.bat              <- HELPER   (called by the others: status_zapret/check_updates/...)
  bin/winws.exe            <- PAYLOAD  (the actual DPI engine, + WinDivert.sys/*.dll + *.bin templates)
  lists/                    <- DATA      (hostlist/ipset files referenced by the .bat wrappers)
  utils/                    <- HELPERS   (incl. "test zapret.ps1"; not a selectable strategy)
```
- **23** root `*.bat`, of which **22** are strategies and `service.bat` is excluded.
- Each `.bat` is self-contained: it `cd`s to `%~dp0bin`, then launches `winws.exe` with a long,
  pre-baked argument line (see `general.bat`). The GUI never needs to know the args — the wrapper
  carries them. Display name = file stem: `general (ALT5).bat` → `general (ALT5)`.

### Linux (`linux/`) — deep, script-driven
```
linux/
  init.d/sysv/zapret       <- STRATEGY  (primary: the SysV init script)
  init.d/sysv/functions    <- HELPER    (sourced, not executed → excluded)
  init.d/openrc/zapret     <- STRATEGY  (alternative init system)
  init.d/{systemd,macos,openwrt,pfsense,runit,s6}/   <- OTHER-PLATFORM/INIT TREES (excluded)
  init.d/custom.d*, windivert.filter.examples/       <- FILTER FRAGMENTS (excluded)
  binaries/<arch>/nfqws    <- PAYLOAD   (per-arch DPI engine; 18 arch dirs)
  nfq/, tpws/, ip2net/, mdig/, ipset/  <- SOURCE subtrees (excluded)
  config.default           <- DATA      (the profile every init script sources)
  install_easy.sh, blockcheck.sh, ...  <- INSTALLER/DEBUG helpers (excluded)
  Makefile, docs/, files/, common/, tmp/          <- not strategies (excluded)
```
- There is **no** per-profile file analogous to the Windows `.bat` set. A Linux zapret install
  exposes essentially **one** selectable launcher (`init.d/sysv/zapret`, plus `openrc` alt); the
  "which strategy" choice lives inside `config.default` (NFQWS_*/TPWS_* toggles), not in filenames.
- Direct `binaries/<arch>/nfqws` invocation is what **portable mode** is for. Upstream treats it as a
  build input rather than a run path — `binaries/` holds precompiled payloads, and the init script
  execs `nfq/<nfqws>` (the locally built one) after composing flags from `config.default`. An unpacked
  tree therefore has a payload that runs and no launcher that will start it. The GUI closes that gap:
  `StrategyParser::applyLinuxRunModes()` synthesizes a `Kind::Binary` entry for
  `binaries/<arch>/nfqws` marked `RunMode::PortableNfqws`, seeds its argv from `PortableMode.h`, and
  `LinuxBackend` owns the nfqueue rules around the child for its whole lifetime (see §2). Because that
  path genuinely works without an install, it is weighted **above** an init script that cannot start
  (§4), not below every wrapper.

## 2. Execution model chosen

| Platform | Strategy kind | Command the backend runs | Elevation |
|----------|---------------|---------------------------|-----------|
| Windows  | `Batch`       | `cmd.exe /c "<abs path to .bat>"` (cwd = `windows/`) | admin (WinDivert driver) |
| Windows  | `PowerShell`  | `powershell.exe -File "<.ps1>"` | admin |
| Linux    | `ShellScript` (`RunMode::InitScript`) | `pkexec "<abs path to init script>" start|stop|restart` (cwd = its dir) | root |
| Linux    | `SystemdUnit` | installed/enabled via `systemctl` (not self-exec'd) | root |
| Linux    | `Binary` (`RunMode::PortableNfqws`) | three phases: `pkexec /bin/sh -c "<iptables + ip6tables inserts>"` → `pkexec "<binaries/<arch>/nfqws>" <argv…>` → `pkexec /bin/sh -c "<iptables + ip6tables deletes>"` | root |
| both     | `Binary` (`RunMode::Default`) | `"<abs path to nfqws/winws.exe>" <GUI-supplied args>` — the GUI owns nothing outside the child | root/admin |

`StrategyParser::applyDefaults` fills `executablePath` + `primaryArg` (the payload path) +
`workingDirectory` + `requiresRoot`. The full argv (e.g. the `/c` flag, or the `start` verb for the
init script) is assembled by the execution backend from `kind` + `primaryArg`; that backend is out
of scope for this step (parsing/locating only).

Why `pkexec` and not `sudo` for Linux: the GUI is graphical and has no TTY; `pkexec` presents a
polkit dialog, which is the standard for a desktop wrapper. Windows batch wrappers self-elevate
through their existing `service.bat`/UAC handling, so the launcher just needs admin-token presence.

### Portable mode: what the GUI runs, and why it is safe

`LinuxBackend` (see its class comment) drives a `RunMode::PortableNfqws` strategy as three child
processes in sequence, one at a time, tracked by `m_phase`:

1. **FirewallUp** — one `sh -c` batch inserting, at the top of `mangle OUTPUT`, one rule per
   `(tcp|udp) × (80|443)`: `-m mark ! --mark M/M -j NFQUEUE --queue-num Q --queue-bypass`, applied
   through `iptables` and mirrored through `ip6tables` (behind a `[ -n "$IP6T" ]` guard so a host
   without `ip6tables` simply loses the v6 steering, not the v4, and the run still succeeds).
2. **Payload** — `pkexec <binaries/<arch>/nfqws> <argv…>`, the long-lived child.
3. **FirewallDown** — the same batch, deleting (`stop()`, a payload that exits by itself, a payload
   that fails to exec, and a `stop()` queued during phase 1 all reach it).

Four properties are load-bearing, and `coretest` asserts each one on the generated text:

* `--queue-bypass` — if `nfqws` is not bound to the queue the packet continues through the stack.
  Without it a crashed or never-started payload black-holes the user's HTTP(S); with it the worst
  case is "no desync", which is what they had before pressing Start.
* `-m mark ! --mark M/M` — excludes nfqws' own re-injected packets so it cannot feed itself. `M`
  must equal `--dpi-desync-fwmark` in argv, which is why both read `kPortableNfDesyncMark`. The mark
  match is family-agnostic (`xt_mark` lives in the raw SKB), so the same `M` guards the v4 and the v6
  anti-loop rule identically.
* `-C` before `-I`/`-D` — iptables accepts duplicate rules, and one `-D` removes only one match, so
  the probes make a start idempotent across crashes and a stop safe to run twice.
* `-w5` — the xtables lock is held by firewalld and friends; bare `-w` waits forever and would hang
  the phase, no `-w` fails spuriously. Five seconds then a loud, reported failure.

Queue number, mark and ports live in `PortableMode.h` (header-only, no process spawning, so the core
stays testable). The parser writes `--qnum=`/`--dpi-desync-fwmark=` into `Strategy::argv`; the backend
reads the queue back out of that same argv with `portableQueueNum()` — last occurrence wins, exactly
like `getopt_long`, and anything non-numeric is rejected rather than interpolated into a root shell
script. A user who edits the flags in the GUI therefore moves the firewall with them, and the two
halves cannot drift.

`$ZAPRET_GUI_FIREWALL_PROG` replaces the `pkexec /bin/sh` prefix (it receives `"/bin/sh" "-c" <script>`),
which is both the escape hatch for a site that elevates with `sudo`/`doas` or a custom polkit wrapper
and the seam `coretest` uses to drive the whole state machine without touching this machine's kernel.

**One consequence for the view.** Because a portable failure is announced only after the teardown
returns, the state it arrives with is already `Error` — and `ZapretManager` clears the active strategy
inside that same `stateChanged(Error)`, one signal *before* `errorOccurred` is emitted. A view that asks
"which run failed?" at notification time therefore gets `""`, and the card keeps no badge. `MainWindow`
holds the id of the last live run (`m_lastRunId`, dropped when a run comes down cleanly) and
`MainWindow::errorTargetId()` resolves pending-start → live-run → last-run in that order. `coretest`
(8i) pins the manager's signal ordering; `uitest` pins the precedence — the two halves of the rule are
asserted where each is actually observable.

**What portable mode deliberately leaves out.** `common/ipt.sh` — the file upstream's `config.default`
drives — builds a much wider rule set than the four above. The GUI's version is the smallest one that
makes `nfqws` work without an installer. It *does* cover both address families — mirroring upstream's
`_fw_nfqws_pre` (`iptables`) and `_fw_nfqws_pre6` (`ip6tables`) with the one shared queue number, since
a single `nfqws` serves both — and each remaining omission is a decision, not an oversight:

* **`OUTPUT` only, no `PREROUTING`/`FORWARD`.** Upstream's reverse rules exist for desync modes that act
  on traffic the box *receives* (relay, bridge and `--dpi-desync=tlsx2` style operation, and for
  gateway setups). Portable mode desyncs this machine's own outbound traffic, which is what `OUTPUT`
  covers; a gateway would have to be configured with zapret's own scripts.
* **No `-m multiport`, `--connbytes` first-packet filters or ipset/`nozapret` excludes.** One rule per
  `(proto, port)` is fewer moving parts and needs no ipset infrastructure, which an unpacked tree has
  none of (`MODE_FILTER=none` is also upstream's default for a quick run). The cost is that every
  packet on those ports is queued, not only the first ones, and that a user cannot carve out an
  exception host with `nozapret`.
* **No flow-offload exemption.** Upstream calls `flow_offloading_exempt` because hardware/NAT offload
  (common on routers) makes rules silently miss packets. Portable mode runs on the workstation itself,
  where conntrack offload is not in play; on an offloading host the rules may simply not fire.

## 3. Detection & search rules (`ZapretPaths` / `ZapretLocator`)

- **Windows root** iff `bin/winws.exe` exists, *or* (`service.bat` exists *and* any `*.bat`/`*.cmd`
  is present). Payload-absent-but-launcher-present trees still match so the GUI can say "extract
  the zip / install the binaries".
- **Linux root** iff `init.d/sysv/zapret` exists, *or* a native `nfqws` is found, *or* an `init.d/`
  dir is present.
- **Native nfqws** preference: `binaries/<host-arch>/nfqws` → `binaries/linux-x86_64/nfqws` →
  `binaries/linux-x86/nfqws` → `./nfqws` (installed by `install_bin.sh`). Host arch from
  `QSysInfo::currentCpuArchitecture()` mapped to the `binaries/` naming.
- **Locator priority**: `$ZAPRET_ROOT` → caller hints → `~/Documents/zapret`, `~/Downloads/zapret`,
  `<cwd>/zapret`, `<cwd>`, `/opt/zapret`, `/usr/local/zapret`; each base probed as itself plus a
  `windows/` and `linux/` child. De-duplicated by canonical path, highest priority kept.

## 4. Weighting (presentation order only, never correctness)

| Entry | weight | rationale |
|-------|--------|-----------|
| `general.bat` | 100 | upstream's recommended default |
| `init.d/sysv/zapret`, tree **installed** | 100 | primary documented Linux launcher, and it can start |
| `init.d/openrc/zapret`, **installed** | 99 | kept; deduped against sysv by `(name,kind)` — sysv wins |
| `binaries/<arch>/nfqws` (portable) | 50 | works on a tree that was only unpacked; firewall owned by the GUI |
| other `general (...).bat` | 60 | valid tunings, not the default |
| systemd unit | 50 | install target, not one-click run |
| PowerShell | 45 | |
| other `*.sh` shell scripts | 55 | not the zapret daemon: `weightForInitScript` only re-ranks entries named `zapret` |
| `init.d/sysv/zapret`, tree **not installed** | 30 | ranked *below* portable: `pkexec <script> start` execs `nfq/nfqws`, which an unpacked tree does not have |
| `init.d/openrc/zapret`, **not installed** | 29 | same verdict, sysv still wins the dedup |
| direct `winws.exe` (unresolved wrapper) | 40 | advanced; no list loading |

The Linux pair straddles the portable weight on purpose: the moment `install_easy.sh` /
`install_bin.sh` puts `nfq/nfqws` in place the daemon jumps from 30 to 100 and swaps places with it,
and `ZapretPaths::isLinuxInstalled()` is the single predicate that decides it. Both entries are always
*offered* whatever the state — the weights only order the list, they never hide the working option.
`StrategyParser::applyLinuxRunModes()` is the pass that assigns `Strategy::RunMode` and these weights;
the per-file `weightFor()` values above are only its PRE-pass guess for the tree walk to dedup on.

`deduplicated()` collapses duplicate `(name, kind)` pairs keeping the higher weight, then sorts by
descending weight and ascending name.

## 5. Build & verify (this dev box, no sudo)

System `qt6-base-dev`/`cmake` were unavailable and `sudo` needs a password, so a user-local prefix
`~/.local/zapretdev` was assembled by `apt-get download` + `dpkg-deb -x` of: `cmake` (+ `librhash1`),
`qt6-base-dev`, `qt6-base-dev-tools`, `qmake6`, `qmake6-bin`, `qtpaths6`. Configure/build:

```sh
export LD_LIBRARY_PATH=~/.local/zapretdev/usr/lib/x86_64-linux-gnu
export P=~/.local/zapretdev/usr
$P/bin/cmake -S . -B build -DCMAKE_PREFIX_PATH=$P -DCMAKE_BUILD_TYPE=Debug
$P/bin/cmake --build build
./build/coretest          # console verifier, exit 0 on success
```

`coretest` runs the parser/locator against the real trees and asserts: 22 Windows strategies with
`service` excluded, `general` ranked first, sysv `zapret` selected & pkexec-defaulted over openrc,
installer/payload dirs skipped, and both roots located. On Linux it then drives the backends with
throwaway `/bin/sh` children through a real event loop, including the whole portable-mode sequence
(§2) with `$ZAPRET_GUI_FIREWALL_PROG` pointed at a stand-in, so the generated rule text, the phase
ordering, the queued stop, the failed-insert path and the teardown-after-crash are all asserted without
touching this machine's firewall, and the installed/unpacked weight tiers are pinned on two scratch
trees that differ by exactly `nfq/nfqws`. Current status: **ALL PASS (0 failures)** — `ctest` runs
`coretest` plus the headless `uitest`.

