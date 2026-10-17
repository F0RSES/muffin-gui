#!/usr/bin/env python3
"""Preflight check for Inno Setup .iss scripts: validate every `Flags:` token in
the [Run] / [UninstallRun] sections against the flags documented by jrsoftware.

Reproduces ISCC's `Parameter "Flags" includes an unknown flag.` locally, so a typo
is caught in ~2 s on the Linux job (the "Lint Inno Setup script" step in
.github/workflows/ci.yml) instead of after a Windows runner has installed Qt and
built the whole application.

Exit status: 0 when every flag checks out, 1 when at least one problem is printed.
"""
import re
import sys

# Every [Run] / [UninstallRun] flag, exactly as enumerated at
# https://jrsoftware.org/ishelp/topic_runsection.htm . Those 19 names are the complete
# definition list under that page's "Flags" parameter, so anything else -- a misspelling
# such as "skipifsunchecked", or an [Files]-only flag pasted onto a [Run] line -- is
# something ISCC itself would reject. Keep this set in sync with the docs: a missing name
# here fails a valid build, an extra name lets a real typo through.
RUN_FLAGS = {
    "32bit", "64bit", "dontlogparameters", "hidewizard", "logoutput", "nowait",
    "postinstall", "runascurrentuser", "runasoriginaluser", "runhidden", "runmaximized",
    "runminimized", "shellexec", "skipifdoesntexist", "skipifnotsilent", "skipifsilent",
    "unchecked", "waituntilidle", "waituntilterminated",
}

# Only meaningful together with postinstall: the docs state the flag "is ignored if the
# postinstall flag isn't also specified", i.e. on any other entry it is dead code.
NEEDS_POSTINSTALL = {"unchecked"}

# Pairs the docs describe as "cannot be combined".
CONFLICTS = [
    {"runascurrentuser", "runasoriginaluser"},
    {"nowait", "waituntilidle"},
    {"nowait", "waituntilterminated"},
    {"waituntilidle", "waituntilterminated"},
]

SECTION_RE = re.compile(r"^\s*\[([A-Za-z]+)\]\s*$")
ENTRY_RE = re.compile(r"^\s*([A-Za-z]+)\s*:\s*(.*)$")


def parse_entries(path):
    """Yield (line_no, section, entries) blocks; `entries` maps lowercase param -> value.

    Comment lines (first non-space char is ';') are skipped. Semicolons are parameter
    separators only when they fall outside double quotes, which is how ISCC reads a line
    too. A line with no separator is a single-parameter entry and is parsed the same way.
    """
    section = None
    block = []
    with open(path, encoding="utf-8") as fh:
        for line_no, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line.strip():
                continue
            if line.lstrip().startswith(";"):
                continue
            m = SECTION_RE.match(line)
            if m:
                if section and block:
                    yield line_no, section, block
                section, block = m.group(1).lower(), []
                continue
            if section is None:
                continue  # preamble ahead of the first section header
            parts, buf, in_q = [], "", False
            for ch in line:
                if ch == '"':
                    in_q = not in_q
                if ch == ";" and not in_q:
                    parts.append(buf)
                    buf = ""
                    continue
                buf += ch
            parts.append(buf)
            entry = {}
            for chunk in parts:
                em = ENTRY_RE.match(chunk)
                if em:
                    entry[em.group(1).lower()] = em.group(2).strip()
            if entry:
                block.append((line_no, entry))
    if section and block:
        yield sys.maxsize, section, block


def check(path):
    problems = []
    scanned = 0
    for _, section, block in parse_entries(path):
        if section not in ("run", "uninstallrun"):
            continue
        for line_no, entry in block:
            scanned += 1
            flags_raw = entry.get("flags")
            if not flags_raw:
                continue
            flags = [f.strip().lower() for f in flags_raw.split() if f.strip()]
            seen = set()
            for flag in flags:
                if flag not in RUN_FLAGS:
                    near = sorted(r for r in RUN_FLAGS
                                  if r.startswith(flag[:4]) or flag.startswith(r[:4]))
                    problems.append(
                        f'{path}:{line_no}: Flags includes unknown flag "{flag}"'
                        + (f"  (did you mean: {', '.join(near)})" if near else ""))
                if flag in seen:
                    problems.append(f"{path}:{line_no}: duplicate flag \"{flag}\"")
                seen.add(flag)
            if "postinstall" not in seen:
                for flag in sorted(NEEDS_POSTINSTALL & seen):
                    problems.append(
                        f'{path}:{line_no}: "{flag}" is ignored without "postinstall"')
            for group in CONFLICTS:
                if group <= seen:
                    problems.append(
                        f"{path}:{line_no}: flags cannot be combined: {sorted(group)}")
    return problems, scanned


def main(argv):
    status = 0
    for path in argv:
        problems, scanned = check(path)
        if problems:
            status = 1
            print(f"FAIL  {path}  ({scanned} [Run]/[UninstallRun] entr"
                  f"{'y' if scanned == 1 else 'ies'} checked)")
            for p in problems:
                print(f"      {p}")
        else:
            print(f"OK    {path}  ({scanned} [Run]/[UninstallRun] entr"
                  f"{'y' if scanned == 1 else 'ies'} checked, all flags valid)")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
