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
- Direct `binaries/<arch>/nfqws` invocation is possible but is *secondary*: it needs iptables/nft
  rule setup that the init script performs for you. The core models it as `Kind::Binary`
  (weight 40, ranked below the wrappers) rather than as a first-class pick.

## 2. Execution model chosen

| Platform | Strategy kind | Command the backend runs | Elevation |
|----------|---------------|---------------------------|-----------|
| Windows  | `Batch`       | `cmd.exe /c "<abs path to .bat>"` (cwd = `windows/`) | admin (WinDivert driver) |
| Windows  | `PowerShell`  | `powershell.exe -File "<.ps1>"` | admin |
| Linux    | `ShellScript` | `pkexec "<abs path to init script>" start|stop|restart` (cwd = its dir) | root |
| Linux    | `SystemdUnit` | installed/enabled via `systemctl` (not self-exec'd) | root |
| both     | `Binary`      | `"<abs path to nfqws/winws.exe>" <GUI-supplied args>` | root/admin |

`StrategyParser::applyDefaults` fills `executablePath` + `primaryArg` (the payload path) +
`workingDirectory` + `requiresRoot`. The full argv (e.g. the `/c` flag, or the `start` verb for the
init script) is assembled by the execution backend from `kind` + `primaryArg`; that backend is out
of scope for this step (parsing/locating only).

Why `pkexec` and not `sudo` for Linux: the GUI is graphical and has no TTY; `pkexec` presents a
polkit dialog, which is the standard for a desktop wrapper. Windows batch wrappers self-elevate
through their existing `service.bat`/UAC handling, so the launcher just needs admin-token presence.

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
| `init.d/sysv/zapret` | 100 | primary documented Linux launcher |
| `init.d/openrc/zapret` | 90 | kept; deduped against sysv by `(name,kind)` — sysv wins |
| other `general (...).bat` | 60 | valid tunings, not the default |
| other shell scripts | 55 | |
| systemd unit | 50 | install target, not one-click run |
| PowerShell | 45 | |
| direct `nfqws`/`winws.exe` | 40 | advanced; needs manual firewall |

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
installer/payload dirs skipped, and both roots located. Current status: **ALL PASS (0 failures)**.

