#!/usr/bin/env python3
"""Preflight check for the Windows application resources in packaging/windows.

Validates the files that decide how Muffin.exe behaves before any of its own code runs --
packaging/windows/app.rc, packaging/windows/app.manifest, packaging/windows/muffin.ico --
and the WIN32 block of CMakeLists.txt that feeds them to the resource compiler.

It exists because none of the six failure modes it catches is reported by a build. A
resource compiler embeds bytes; it does not ask what the shell or the loader will make of
them, and link.exe does not look inside a manifest either:

  1. app.rc naming a resource by a symbol the toolchain was never told about. rc.exe stops
     with RC2104, but windres compiles it into a *string* resource name and reports success.
     CreateProcess reads the execution level out of ordinal 1 and nothing else, so such an
     image carries a manifest nobody consults: the process starts asInvoker,
     WindowsBackend::start() then refuses every elevated-required strategy, and what the
     user sees reads like a WinDivert problem. A green MinGW build shipping a broken app is
     the case worth designing out -- no CI job runs windres.
  2. The same trap for a resource TYPE: RT_MANIFEST as an unresolved identifier becomes the
     string type "RT_MANIFEST", which is not 24, so the loader never sees a manifest.
  3. A manifest that is not well-formed XML. Both compilers take it as opaque bytes and the
     link succeeds; the loader's own manifest parser then rejects it and the process runs
     asInvoker. Identical symptom, again with a green build.
  4. An .ico that the compilers embed without complaint but the shell cannot decode, so the
     file icon silently falls back to the generic application glyph.
  5. A payload named in app.rc that CMakeLists.txt does not stage into the build directory.
     windres resolves the relative path against the directory holding the .rc and finds it;
     rc.exe resolves it against the working directory, which in a CMake build is the binary
     dir. The same script therefore builds on MinGW and fails on MSVC -- the reverse of the
     usual cross-platform order, and late: after a Windows runner has installed Qt and
     compiled everything else.
  6. The installer's own assets. packaging/windows_installer.iss asks Inno for a dark wizard
     and for two bitmaps, and ISCC resolves all three at compile time: a missing or renamed
     banner is a failed build, and a palettised, truncated or wrong-shaped one is a wizard
     quietly drawing its own art instead. Both land on the `installer` job, which runs only on
     a tag -- the last place anyone wants to discover that a picture was deleted from a
     repository.

Everything here is checked in about 1.5 s on any host with Python, with no Windows, no Qt and
no compiler, which is what makes it worth running before those runners are booked. Nearly all
of that is the wizard bitmaps being re-rendered and compared byte for byte; the text and byte
inspection of everything else takes about 0.1 s.

Usage: python3 packaging/check-win-resources.py [repo-root]
       (default root: the parent of this script's directory)
Exit status: 0 when every check passes, 1 when at least one problem is printed.
"""
import importlib.util
import os
import re
import struct
import sys
import tempfile
import xml.etree.ElementTree as ET

# winuser.h: RT_MANIFEST is 24, and CREATEPROCESS_MANIFEST_RESOURCE_ID -- the only id
# CreateProcess consults for an application's execution level -- is 1. The names are spelled
# out as numbers rather than read from a header because the point of this checker is that an
# unresolved name of either kind silently produces a resource the loader ignores.
RT_GROUP_ICON = 14
RT_MANIFEST = 24
MANIFEST_ID = 1

# The payload names are read out of app.rc rather than hard-coded, so renaming a file (and
# staging the new name in CMakeLists.txt) is not a reason this checker would fail a valid
# build. What has to hold is the (type, ordinal) pair below, and that whatever is named also
# exists and is staged.
WANTED = {
    "icon": (RT_GROUP_ICON, 1),
    "manifest": (RT_MANIFEST, MANIFEST_ID),
}

# rc.exe's memory-usage keywords. The payload is read as the last quoted token of a statement,
# so `1 ICON DISCARDABLE "x.ico"` and `1 ICON "x.ico"` parse the same way and no keyword list
# is needed; DISCARDABLE on the manifest is a documented choice of app.rc's own, not a fact a
# checker should police.

# Type keywords that are part of the .rc language itself, so both compilers know them without
# any header: this is the distinction that matters. An identifier NOT in this list (RT_MANIFEST,
# IDI_ICON1, CREATEPROCESS_MANIFEST_RESOURCE_ID ...) can only come from a header, and app.rc
# includes none -- so rc.exe stops with RC2104 while windres quietly compiles it into a string
# name or type. The values are the resource types each keyword stands for.
RC_TYPE_KEYWORDS = {
    "BITMAP": 2, "ICON": RT_GROUP_ICON, "DIALOG": 5, "STRINGTABLE": 6, "ACCEL": 9,
    "RCDATA": 10, "MENU": 11, "CURSOR": 12, "GROUP_ICON": RT_GROUP_ICON,
    "GROUP_CURSOR": 12, "VERSIONINFO": 16, "DIALOGEX": 23,
}

# Token classes. rc.exe accepts a hex integer for a name/type as easily as a decimal one, so
# both are ordinals here; anything else that is not quoted is an identifier the compiler may
# or may not know, which is exactly the distinction this file is about.
DEC_RE = re.compile(r"\d+")
HEX_RE = re.compile(r"0[xX][0-9a-fA-F]+")

# Frame set app.rc's header documents for muffin.ico: cupcake at 256 px (PNG-compressed, as
# Explorer asks for it) plus the sizes the shell, the taskbar and a HiDPI desktop actually
# sample, as 32-bit DIBs. A file that has only one frame still builds and still ships -- with
# a scaled or blocky icon everywhere the 16 px glyph is not the one being drawn -- so the
# manual "check the five entries before committing" step is the thing encoded here. Update
# this constant together with that comment, not together with the artwork alone.
ICON_FRAMES = {256: "png", 64: 32, 48: 32, 32: 32, 16: 32}

# The supportedOS values are deliberately NOT enumerated. Two different GUIDs circulate for
# "Windows 8" (the MSDN page and the manifest every vendor ships disagree), and a checker
# with the wrong entry in it would fail a valid manifest -- which is worse than not checking.
# What is unambiguous is the shape, and that a process is only treated as current-OS if the
# Windows 10 id is present (Windows 11 shares it), so those are what is asserted below.
WINDOWS_10_OS_ID = "8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a"
GUID_RE = re.compile(r"^\{?[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}"
                     r"-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}?$")

EXEC_LEVELS = {"asInvoker", "highestAvailable", "requireAdministrator"}

# What Muffin has to ask for, and why the other two are not acceptable here: winws.exe loads
# the WinDivert driver and writes firewall/DNS state, so WindowsBackend::start() refuses an
# elevated-required strategy unless PrivilegeHelper::isElevated() already holds. asInvoker
# means the user has to remember the right-click; highestAvailable means it silently depends
# on whether the account that launched it happens to be an administrator's.
WANTED_LEVEL = "requireAdministrator"


def strip_rc_comments(text):
    """Blank out ``//`` and ``/* */`` comments, keeping every newline and every string.

    app.rc's header names the very identifiers this checker rejects -- IDI_ICON1,
    RT_MANIFEST, CREATEPROCESS_MANIFEST_RESOURCE_ID -- to explain why they are rejected.
    Reading comments as statements would report that documentation as a regression, so this
    runs first. Quoted text is never a comment (a path may legitimately contain "//"), and
    ';' does not start a comment in a .rc, so unlike the Inno checker only quotes matter.
    """
    out, i, n, in_q = [], 0, len(text), False
    while i < n:
        ch = text[i]
        if in_q:
            out.append(ch)
            if ch == '"':
                in_q = False
            i += 1
        elif ch == '"':
            in_q = True
            out.append(ch)
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                out.append("\n" * text.count("\n", i))
                i = n
            else:
                out.append("\n" * text.count("\n", i, end + 2))
                i = end + 2
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def tokenize(line):
    """Split a statement into tokens, keeping a quoted path as one token."""
    return re.findall(r'"[^"]*"|\S+', line)


def token_kind(token):
    """'ordinal' | 'string' | 'symbol', and the ordinal's value when there is one."""
    if len(token) >= 2 and token.startswith('"') and token.endswith('"'):
        return "string", token[1:-1]
    if DEC_RE.fullmatch(token) or HEX_RE.fullmatch(token):
        try:
            return "ordinal", int(token, 0)
        except ValueError:  # a leading zero, which int(base 0) refuses but rc.exe takes
            return "ordinal", int(token, 16) if HEX_RE.fullmatch(token) else int(token)
    return "symbol", token


def describe(kind, value):
    if kind == "ordinal":
        return f"ordinal {value}"
    if kind == "string":
        return f'string name "{value}"'
    return f'identifier "{value}"'


def parse_rc_statements(path):
    """Yield (line_no, name, type, payload) for every line that defines a resource.

    A definition is read as ``name type [memory-options...] data``: name and type are the
    first two tokens and the payload is the last quoted token on the line. Preprocessor
    lines, directives like LANGUAGE and multi-line blocks (STRINGTABLE, VERSIONINFO) do not
    match that shape and are skipped rather than guessed at.
    """
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    for line_no, line in enumerate(strip_rc_comments(text).splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        tokens = tokenize(line)
        if len(tokens) < 3:
            continue
        if not tokens[-1].startswith('"'):
            continue
        payload = tokens[-1][1:-1]
        yield line_no, token_kind(tokens[0]), token_kind(tokens[1]), payload


def check_rc(path):
    """Assert the two statements app.rc owes the loader, and how each of them is named.

    Returns (problems, found); `found` maps 'icon'/'manifest' to the statement's line and the
    payload it names, which the existence and CMake-staging checks then work from. A name
    cannot be what identifies a statement (it is exactly the part that may be wrong), so the
    role is decided from the TYPE token alone.
    """
    if not os.path.isfile(path):
        return [f"{path}: missing; CMakeLists.txt fails at configure without it"], {}
    problems, found, seen = [], {}, set()
    # `seen` records that a statement for a role exists at all, even when it is named wrong, so
    # one broken line produces one accurate complaint rather than also claiming the resource is
    # absent from the file.
    for line_no, (nkind, nval), (tkind, tval), payload in parse_rc_statements(path):
        if tkind == "ordinal":
            effective = tval
        elif tkind == "symbol" and tval.upper() in RC_TYPE_KEYWORDS:
            effective = RC_TYPE_KEYWORDS[tval.upper()]
        elif tkind in ("symbol", "string"):
            # Not a keyword of the .rc language, so only a header could give it a value -- and
            # app.rc includes none. Report it as the resource it was meant to be.
            role = ("manifest" if "MANIFEST" in str(tval).upper()
                    else "icon" if "ICON" in str(tval).upper() else None)
            if role is None:
                continue  # some other resource kind; not this checker's business
            problems.append(
                f'{path}:{line_no}: the {role} resource type is "{tval}"'
                + (", an identifier no header in scope defines, "
                   if tkind == "symbol" else ", a string, ")
                + f"which rc.exe rejects with RC2104 while windres compiles it into the STRING "
                  f"type \"{tval}\". The loader looks for type {WANTED[role][0]} by number, so "
                  f"the MinGW build that passes produces an image that ignores {payload}. Use "
                  f"{WANTED[role][0]}"
                  + (", or the ICON keyword." if role == "icon" else "."))
            seen.add(role)
            continue
        else:
            continue
        if effective == RT_GROUP_ICON:
            role = "icon"
        elif effective == RT_MANIFEST:
            role = "manifest"
        elif effective == 3:
            role = "icon"
            problems.append(
                f"{path}:{line_no}: type 3 is RT_ICON, a single frame, not the "
                f"{RT_GROUP_ICON} group the shell resolves a file's icon from. Name the "
                f"whole .ico with the ICON keyword or type {RT_GROUP_ICON}.")
        else:
            continue

        want_id = WANTED[role][1]
        if nkind != "ordinal":
            problems.append(
                f"{path}:{line_no}: the {role} resource is named {describe(nkind, nval)} "
                f"instead of ordinal {want_id}. "
                + ("CreateProcess reads the execution level from ordinal 1 and nothing else, "
                   "so the process starts asInvoker, WindowsBackend::start() refuses every "
                   "elevated-required strategy, and the build is green the whole way."
                   if role == "manifest" else
                   "Explorer resolves a file's icon from the icon group with the lowest "
                   "ordinal, so this is the icon the taskbar and Inno's IconIndex: 0 fail "
                   "to find."))
            seen.add(role)
            continue
        if nval != want_id:
            problems.append(
                f"{path}:{line_no}: the {role} resource is ordinal {nval}, expected "
                f"{want_id}. "
                + ("An ordinal-2 manifest sits in the image and the loader never consults it."
                   if role == "manifest" else
                   "The shell and Inno's IconIndex: 0 both resolve the lowest ordinal."))
            seen.add(role)
            continue
        if role in found:
            problems.append(
                f"{path}:{line_no}: a second {role} resource ({payload}); {path}:"
                f"{found[role]['line']} already defines ordinal {want_id}")
            continue
        found[role] = {"line": line_no, "payload": payload}

    for role, kind_name in (("icon", "icon group"), ("manifest", "RT_MANIFEST")):
        if role not in found and role not in seen:

            problems.append(
                f"{path}: no {kind_name} statement (expected "
                f"`{WANTED[role][1]} {'ICON' if role == 'icon' else WANTED[role][0]}` "
                f'"payload path"). Without it the exe ships '
                + ("no manifest, so it starts asInvoker and WindowsBackend::start() refuses "
                   "every elevated-required strategy that needs the WinDivert driver."
                   if role == "manifest" else
                   "brandless: no icon for Explorer, the Start menu or the taskbar."))
    return problems, found


def local(tag):
    """Strip the namespace URI. These elements are looked for by name, because the schema
    spreads them over four different namespaces and a typo in one of them is its own bug."""
    return tag.rsplit("}", 1)[-1]


def path_of(el, parents):
    """Local-name chain from the root down to `el`, so the parent element can be asserted.

    A <requestedExecutionLevel> outside trustInfo/security/requestedPrivileges is well-formed
    XML that no reader consults -- the same shape of silent failure as the ordinal trap above,
    so it is checked rather than assumed.
    """
    chain = []
    while el is not None:
        chain.append(local(el.tag))
        el = parents.get(id(el))
    return list(reversed(chain))


def check_manifest(path):
    """Parse app.manifest and assert the declarations that change how the process starts.

    Returns (problems, level); `level` is what was found, so the caller can print it instead
    of making anyone open the file to confirm it.
    """
    if not os.path.isfile(path):
        return [f"{path}: missing; app.rc names it and CMakeLists.txt stages it"], None
    raw = open(path, encoding="utf-8", errors="replace").read()
    try:
        root = ET.fromstring(raw)
    except ET.ParseError as exc:
        line = getattr(exc, "position", (0, 0))[0]
        return [
            f"{path}:{line}: not well-formed XML ({exc}). rc.exe and windres embed a manifest "
            "as opaque bytes and the link succeeds; the loader's own parser then rejects the "
            "resource and the process starts asInvoker. Two hyphens in a row inside an XML "
            "comment is the usual cause -- an XML comment may not contain a double hyphen, "
            "which is why the prose in this file avoids it."], None

    problems = []
    parents, els = {}, {}
    for el in root.iter():
        els.setdefault(local(el.tag), []).append(el)
        for child in el:
            parents[id(child)] = el

    if local(root.tag) != "assembly":
        problems.append(f"{path}: root element is <{local(root.tag)}>, expected <assembly>")
    if root.get("manifestVersion") != "1.0":
        problems.append(
            f"{path}: manifestVersion is {root.get('manifestVersion')!r}; 1.0 is the assembly "
            "schema version the loader parses")

    # 1. the execution level, and where in the document it sits
    levels = els.get("requestedExecutionLevel", [])
    if not levels:
        problems.append(
            f"{path}: no <requestedExecutionLevel>, so the process gets the default asInvoker "
            "and WindowsBackend::start() refuses every elevated-required strategy")
    else:
        if len(levels) > 1:
            problems.append(
                f"{path}: {len(levels)} <requestedExecutionLevel> elements; the loader applies "
                "the first one and the rest only invite disagreement about the level")
        el = levels[0]
        chain = path_of(el, parents)
        if chain[-4:] != ["trustInfo", "security", "requestedPrivileges",
                          "requestedExecutionLevel"]:
            problems.append(
                f"{path}: <requestedExecutionLevel> sits at {'/'.join(chain)}; it is only read "
                "under trustInfo/security/requestedPrivileges, and anywhere else it is "
                "well-formed XML that changes nothing")
        level = el.get("level")
        if level not in EXEC_LEVELS:
            problems.append(
                f"{path}: requestedExecutionLevel level={level!r} is none of the values the "
                f"schema allows: {', '.join(sorted(EXEC_LEVELS))}")
        elif level != WANTED_LEVEL:
            problems.append(
                f"{path}: level is {level!r}, expected {WANTED_LEVEL!r}. winws.exe loads the "
                "WinDivert driver and writes firewall/DNS state, so an app started lower down "
                "refuses every elevated strategy at runtime -- with a green build either way. "
                f"(level={level!r} is also what the exe had before this manifest existed.)")
        if el.get("uiAccess", "false").lower() != "false":
            problems.append(
                f"{path}: uiAccess is true. That needs an Authenticode-signed binary under "
                "Program Files and a Windows SDK entitlement; without all three the loader "
                "falls back to asInvoker instead of honouring the request -- and it is an "
                "accessibility-API grant this application has no business asking for.")

    # 2. the two restatements of what link.exe used to generate on its own, each of them
    #    silently load-bearing -- they are here because /MANIFEST:NO switched that step off
    dpi = els.get("dpiAwareness", [])
    if not dpi:
        problems.append(
            f"{path}: no <dpiAwareness>. The process then starts DPI-unaware, Qt 6's "
            "per-monitor-v2 handling never engages, and on a scaled monitor the window just "
            "renders blurry -- nothing about the build says so")
    else:
        el = dpi[0]
        if not el.tag.startswith("{http://schemas.microsoft.com/SMI/"):
            problems.append(
                f"{path}: <dpiAwareness> is in namespace {el.tag.split('}', 1)[0][1:]!r}; the "
                "DPI settings are read from the SMI WindowsSettings namespaces only, so in "
                "any other one they are inert")
        if "permonitorv2" not in (el.text or "").lower():
            problems.append(
                f"{path}: dpiAwareness is {(el.text or '').strip()!r}, which omits "
                "'permonitorv2' -- the value Qt 6's own Windows backend asks for")
    if not els.get("dpiAware"):
        problems.append(
            f"{path}: no <dpiAware>, the pre-8.1 spelling the setting falls back to when "
            "dpiAwareness itself is not understood")

    commons = [e for e in els.get("assemblyIdentity", [])
               if e.get("name") == "Microsoft.Windows.Common-Controls"]
    if not commons:
        problems.append(
            f"{path}: no Common-Controls dependentAssembly. The native file dialogs Qt "
            "borrows from comdlg32 then fall back to the classic Win32 look, which is a visual "
            "regression no compiler complains about")
    else:
        el = commons[0]
        if el.get("publicKeyToken") != "6595b64144ccf1df":
            problems.append(
                f"{path}: Common-Controls publicKeyToken is {el.get('publicKeyToken')!r}; the "
                "SxS resolve of the themed comctl32 depends on that exact token")
        if el.get("version") != "6.0.0.0":
            problems.append(
                f"{path}: Common-Controls version is {el.get('version')!r}, expected 6.0.0.0 "
                "-- the v6 line, which is the themed one")

    # 3. the compatibility list: shape and the one id that matters, not an enumeration of
    #    every OS GUID (see WINDOWS_10_OS_ID above for why)
    os_ids = []
    for el in els.get("supportedOS", []):
        ident = el.get("Id") or ""
        if not GUID_RE.match(ident):
            problems.append(
                f"{path}: supportedOS Id={ident!r} is not a GUID. A version the loader cannot "
                "match makes Windows apply a compatibility version lie to the process, which "
                "changes what APIs report rather than what fails")
            continue
        plain = ident.strip("{}").lower()
        if plain in os_ids:
            problems.append(f"{path}: duplicate supportedOS Id={ident}")
        os_ids.append(plain)
    if WINDOWS_10_OS_ID not in os_ids:
        problems.append(
            f"{path}: no supportedOS Id for Windows 10/11 ({{{WINDOWS_10_OS_ID}}}); that is the "
            "id under which the process is treated as running on a current OS")

    level = levels[0].get("level") if levels else None
    return problems, level


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def check_ico(path):
    """Parse muffin.ico as a Windows icon file, the way the shell will read it back.

    rc.exe and windres both embed this file's bytes without looking at them, so a file that
    is truncated, is actually a cursor, or has an entry directory pointing outside the image
    costs nothing at build time and shows up as Explorer falling back to the generic glyph.
    The header documents the frame set this file is supposed to carry; that is asserted here
    rather than by eye before a commit.
    """
    if not os.path.isfile(path):
        return [f"{path}: missing; app.rc names it and CMakeLists.txt stages it"], None
    data = open(path, "rb").read()
    problems = []
    if len(data) < 6:
        return [f"{path}: only {len(data)} byte(s); no ICONDIR header"], None
    reserved, itype, count = struct.unpack("<HHH", data[:6])
    if reserved:
        problems.append(f"{path}: ICONDIR reserved word is {reserved}, must be 0")
    if itype != 1:
        problems.append(
            f"{path}: icon type is {itype}, not 1. 2 is a CURSOR and the shell will not use "
            "it for a file icon; anything else the resource compilers still embed and ignore")
    if count == 0:
        problems.append(f"{path}: directory says 0 images, so the group has nothing to draw")
        return problems, None
    if len(data) < 6 + 16 * count:
        problems.append(
            f"{path}: directory declares {count} entries but the file is {len(data)} bytes, "
            "so it is truncated -- the compilers embed it anyway")
        return problems, None

    frames = {}
    for i in range(count):
        off = 6 + 16 * i
        w, h, ncol, _, planes, bits, size, doff = struct.unpack("<BBBBHHII", data[off:off + 16])
        # 0 in these fields means 256, which is why a 256 px frame reads as 0x0.
        edge = 256 if w == 0 else w
        edge_h = 256 if h == 0 else h
        where = f"{path}: entry {i} ({edge}x{edge_h})"
        if size == 0:
            problems.append(f"{where} declares 0 bytes of image data")
            continue
        if doff + size > len(data):
            problems.append(
                f"{where} points at offset {doff}+{size} bytes, past the end of a "
                f"{len(data)}-byte file")
            continue
        body = data[doff:doff + size]
        png = body.startswith(PNG_SIGNATURE)
        if png:
            if edge not in frames:
                frames[edge] = "png"
            continue
        # Below 40 bytes there is no BITMAPINFOHEADER, so the entry is not image data at all.
        if len(body) < 40:
            problems.append(f"{where} is neither PNG nor a DIB: {len(body)} bytes of data")
            continue

        bi_size, bi_w, bi_h = struct.unpack("<Iii", body[:12])
        if bi_size != 40:
            problems.append(
                f"{where} starts with a {bi_size}-byte bitmap header, not the 40-byte "
                "BITMAPINFOHEADER an icon directory entry points at")
        if bi_w != edge:
            problems.append(f"{where} bitmap width {bi_w} disagrees with the directory's {edge}")
        # A DIB icon frame carries the AND mask too, so biHeight is the image height twice;
        # 32-bit frames may omit it and give the height once. Anything else draws half a glyph.
        if bi_h not in (edge_h, 2 * edge_h):
            problems.append(
                f"{where} bitmap height {bi_h} is neither {edge_h} nor the doubled {2 * edge_h} "
                "that includes the AND mask, so the frame is decoded at the wrong size")
        if planes != 1:
            problems.append(f"{where} has {planes} colour planes; a DIB icon frame has one")
        if bits not in (1, 4, 8, 16, 24, 32):
            problems.append(f"{where} declares {bits} bits per pixel, which is not a DIB depth")
        if edge == edge_h and edge not in frames:
            frames[edge] = bits

    for edge, want in sorted(ICON_FRAMES.items(), reverse=True):
        got = frames.get(edge)
        if got is None:
            problems.append(
                f"{path}: no {edge}x{edge} frame (expected "
                f"{'PNG-compressed' if want == 'png' else f'{want}-bit'}); the shell picks the "
                "closest available size and scales it, which is where a blurry icon comes from")
        elif want != got:
            problems.append(
                f"{path}: the {edge}x{edge} frame is "
                f"{'PNG-compressed' if got == 'png' else f'{got}-bit'}, expected "
                f"{'PNG-compressed' if want == 'png' else f'{want}-bit'}")
    extra = sorted(set(frames) - set(ICON_FRAMES), reverse=True)
    if extra:
        problems.append(
            f"{path}: unexpected frame sizes {extra}; muffin.ico's header lists the set this "
            "file is meant to carry, so update that comment and ICON_FRAMES together")
    return problems, ", ".join(f"{e}{'px PNG' if f == 'png' else f'/{f}bp'}"
                               for e, f in sorted(frames.items(), reverse=True)) or None



def check_payload_names(rc_path, found):
    """Every payload app.rc names has to be a plain file in the same directory.

    A subdirectory would break the staging that makes the script compile under both path
    rules: CMakeLists.txt copies `${MUFFIN_RC_DIR}/<name>` to `${CMAKE_CURRENT_BINARY_DIR}/<name>`
    and configure_file() does not create intermediate directories, so the MSVC build would
    fail on a file windres found happily beside the .rc. An absolute path is worse still -- it
    resolves on the machine that wrote it and nowhere else.
    """
    problems = []
    directory = os.path.dirname(rc_path)
    for role in ("icon", "manifest"):
        if role not in found:
            continue
        payload = found[role]["payload"]
        if payload != os.path.basename(payload) or payload in ("", ".", ".."):
            problems.append(
                f"{rc_path}:{found[role]['line']}: {role} payload {payload!r} is not a plain "
                f"file name in packaging/windows/. Keep it relative and unsegmented so the "
                f"staged copy in the build directory is the same file both compilers read.")
            continue
        if not os.path.isfile(os.path.join(directory, payload)):
            problems.append(
                f"{rc_path}:{found[role]['line']}: {payload!r} is not beside the .rc; windres "
                f"resolves it against that directory and rc.exe against the build directory, "
                f"so this fails on one toolchain or the other at compile time")
    return problems


def strip_cmake_comments(text):
    """Drop every CMake comment: an unquoted ``#`` and the rest of its line.

    A CMake comment does not have to start the line, so both `# enable_language(RC)` and
    `set_target_properties(Muffin PROPERTIES LINK_FLAGS "/MANIFEST:NO")  # ...` remove a command
    that the build depends on. Every assertion in check_cmake is about a command that is
    *called*, so commenting one out -- the exact edit that reintroduces the failure it was added
    to prevent -- has to stop matching. A '#' inside a quoted argument is literal and is kept.
    Bracket comments (#[[ ... ]]) are not handled; none is used in this file.
    """
    lines = []
    for line in text.splitlines():
        buf, in_q, esc = [], False, False
        for ch in line:
            if esc:
                buf.append(ch)
                esc = False
            elif ch == "\\" and in_q:
                buf.append(ch)
                esc = True
            elif ch == '"':
                in_q = not in_q
                buf.append(ch)
            elif ch == "#" and not in_q:
                break
            else:
                buf.append(ch)
        lines.append("".join(buf))
    return "\n".join(lines)


def check_cmake(path, found):

    """Assert the WIN32 block of CMakeLists.txt actually wires these files up.

    The point is the pairings a text edit breaks quietly: a payload named in app.rc and not
    staged, a resource added to the target without enable_language(RC), the linker manifest
    left enabled so mt.exe overwrites resource 1/RT_MANIFEST with asInvoker again.
    """
    problems = []
    if not os.path.isfile(path):
        return [f"{path}: missing; nothing builds the resources at all"], []
    text = open(path, encoding="utf-8", errors="replace").read()
    # Comments carry the reasoning for every line below, so they are stripped rather than
    # trusted: an identifier mentioned in a comment is not a command that gets called.
    text = strip_cmake_comments(text)
    done = []


    def need(pattern, what, label=None):
        m = re.search(pattern, text)
        if not m:
            problems.append(f"{path}: {what}")
        else:
            done.append(label or what)
        return m

    need(r"enable_language\s*\(\s*RC\s*\)",
         "does not enable_language(RC); app.rc in target_sources() is not a known source "
         "language, so CMake passes it to the compiler driver and the resource is never built")
    need(r"target_sources\s*\(\s*Muffin\b[^\n]*app\.rc",
         "does not add packaging/windows/app.rc to the Muffin target; the exe links without "
         "either resource and nothing complains")

    staged = []
    for role in ("icon", "manifest"):
        if role not in found:
            continue
        name = re.escape(found[role]["payload"])
        m = re.search(r"configure_file\s*\([^\n]*?" + name + r"[^\n]*\)", text)
        if not m:
            problems.append(
                f"{path}: no configure_file() staging {found[role]['payload']} into "
                f"${{CMAKE_CURRENT_BINARY_DIR}}. windres finds it beside app.rc; rc.exe "
                "resolves relative paths against the working directory, which in a CMake "
                "build is the binary dir, so the same .rc compiles on MinGW and fails on "
                "MSVC -- late, on a Windows runner, after Qt is installed and everything "
                "else is built")
        else:
            staged.append(found[role]["payload"])
            done.append(f"stages {found[role]['payload']}")

    guard = re.search(r"foreach\s*\(\s*\w+\s+IN\s+ITEMS([^\n]*)\)", text)
    if not guard:
        problems.append(
            f"{path}: no foreach(IN ITEMS ...) existence guard for the resource files; a "
            "missing payload should fail at configure rather than at compile")
    else:
        listed = guard.group(1)
        for name in ["app.rc"] + staged:
            if name not in listed:
                problems.append(
                    f"{path}: the configure-time guard lists {listed.strip()!r} and not "
                    f"{name!r}, so a missing {name} would only surface in the resource compiler")
        done.append("guard list covers app.rc and each staged payload")

    # Non-greedy to the first ')' rather than to the end of the file: there is no nested
    # parenthesis in the statement, and a blob that swallowed the rest of CMakeLists.txt
    # would pass this check for anything mentioned anywhere below it.
    depends = re.search(r"set_source_files_properties\s*\((.*?)\)", text, re.S)
    blob = depends.group(1) if depends else ""
    if not depends:
        problems.append(
            f"{path}: no set_source_files_properties() with OBJECT_DEPENDS; app.rc has no "
            "declared inputs, so a change to the artwork or the manifest does not rebuild "
            "it and the stale .res is what ships")
    for name in staged:
        if name not in blob:
            problems.append(
                f"{path}: OBJECT_DEPENDS does not cover {name}; editing the artwork or the "
                "manifest in the checkout would not rebuild the resource, and the stale .res "
                "is what ships")
            break
    else:
        done.append("OBJECT_DEPENDS covers every staged payload")

    # Match where the flag is actually *used*, not where it is first mentioned: the string
    # also appears in the comment above the block, and a comment is not a linker flag.
    linkflag = re.search(r'LINK_FLAGS\s+"(/MANIFEST:NO)"', text)
    if not linkflag:
        problems.append(
            f"{path}: no LINK_FLAGS \"/MANIFEST:NO\". link.exe generates its own asInvoker "
            "manifest and mt.exe embeds it over resource 1/RT_MANIFEST, replacing the elevation "
            "level this file exists to set -- on the toolchain CI actually builds with")
    else:
        head = text[:linkflag.start()]
        opens, closes = len(re.findall(r"\bif\s*\(", head)), len(re.findall(r"\bendif\s*\(", head))
        enclosing = re.findall(r"\bif\s*\(\s*([^)]*?)\s*\)", head)
        if opens <= closes:
            problems.append(
                f"{path}: /MANIFEST:NO is set outside any if() block, so it reaches MinGW too, "
                "whose driver does not understand the flag")
        elif "MSVC" not in enclosing[-1]:
            problems.append(
                f"{path}: /MANIFEST:NO is inside if ({enclosing[-1]}) rather than if (MSVC), so "
                "the block that fixes MSVC is applied to a toolchain that cannot take it")
        else:
            done.append("/MANIFEST:NO confined to if (MSVC)")
    return problems, done




def strip_iss_comments(text):
    """The script's lines with whole-line comments dropped.

    The reason is the one that makes CMakeLists.txt get read comment-free too: a pattern matches
    text whether or not the compiler will act on it, and a `;WizardStyle=modern dark` left in the
    file as a note is not a setting.
    """
    return [line for line in text.splitlines() if not line.lstrip().startswith(";")]


def setup_directives(text):
    """{lowercased name: value} for the [Setup] section; a later repeat of a name wins, as in Inno."""
    vals = {}
    in_setup = False
    for line in strip_iss_comments(text):
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            in_setup = s.lower() == "[setup]"
            continue
        if not in_setup or "=" not in s:
            continue
        key, value = s.split("=", 1)
        vals[key.strip().lower()] = value.strip()
    return vals


def check_bmp(path, want_w, want_h, what):
    """A plain 24-bit bottom-up BMP of exactly `want_w` x `want_h`, and self-consistent.

    ISCC embeds whatever bytes it is handed, and only the running wizard discovers that the
    "bitmap" is palettised, truncated or the wrong shape. The header is therefore checked against
    the file rather than trusted -- the same stance muffin.ico's directory is read with.
    """
    problems = []
    with open(path, "rb") as fh:
        blob = fh.read()
    if len(blob) < 54:
        return [f"{path}: {len(blob)} bytes is too short to hold a BMP header ({what})"]
    if blob[:2] != b"BM":
        problems.append(f"{path}: starts with {blob[:2]!r}, not b'BM' ({what})")
    declared = struct.unpack("<I", blob[2:6])[0]
    if declared != len(blob):
        problems.append(f"{path}: header declares {declared} bytes, the file is {len(blob)} "
                        f"({what})")
    off_bits = struct.unpack("<I", blob[10:14])[0]
    if off_bits != 54:
        problems.append(f"{path}: pixel data starts at {off_bits}, not at the 54-byte header-only "
                        f"layout ({what})")
    hdr = struct.unpack("<I", blob[14:18])[0]
    if hdr != 40:
        problems.append(f"{path}: BITMAPINFOHEADER is {hdr} bytes; a plain DIB uses 40 ({what})")
    w, h, planes, bpp, comp = struct.unpack("<iiHHI", blob[18:34])
    if (w, h) != (want_w, want_h):
        problems.append(f"{path}: is {w}x{h}; Inno's image area for {what} at 100% DPI is "
                        f"{want_w}x{want_h}")
    if h <= 0:
        problems.append(f"{path}: height {h} means a top-down BMP; the generator writes bottom-up")
    if planes != 1:
        problems.append(f"{path}: {planes} colour planes, expected 1")
    if bpp != 24:
        problems.append(f"{path}: {bpp} bits per pixel; 24 is what Inno is handed here")
    if comp != 0:
        problems.append(f"{path}: compression {comp} is set; the checked-in bitmaps are uncompressed")
    row = ((w * 3) + 3) // 4 * 4
    need = off_bits + row * abs(h)
    if len(blob) < need:
        problems.append(f"{path}: {len(blob)} bytes cannot hold {abs(h)} padded rows of {row} "
                        f"bytes from offset {off_bits} ({need} needed)")
    return problems


def load_wizard_generator():
    """Import packaging/make-wizard-images.py despite the dashes in its file name.

    The expected names and sizes come out of the generator rather than from constants repeated here,
    for the same reason the payload names are read out of app.rc: a rename on one side is only a
    valid build if the other side moved with it, and a checker carrying its own copy of the answer
    would fail the pair that moved together and pass the pair that did not.
    """
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "make-wizard-images.py")
    spec = importlib.util.spec_from_file_location("muffin_make_wizard_images", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_installer(root):
    """The wizard look the [Setup] section asks for, and the two bitmaps it names.

    Worth an automated answer for the same reason the rest of this file does: ISCC resolves
    WizardImageFile at compile time and stops when the file is not there, and the `installer` job
    only runs on a tag -- so a banner that goes missing, is renamed on one side only, or is edited
    into something that is no longer a plain BMP fails a release after everything else has gone
    green. Neither the .iss nor the .bmp is inspected by any other step.
    """
    problems, done = [], []
    iss_path = os.path.join(root, "packaging", "windows_installer.iss")
    if not os.path.isfile(iss_path):
        return [f"{iss_path}: not found"], done

    with open(iss_path, encoding="utf-8") as fh:
        vals = setup_directives(fh.read())

    # A base style and an appearance mode live in one WizardStyle value, space separated. Losing the
    # second word is silent -- the wizard simply comes out light -- so both halves are asked for.
    style = vals.get("wizardstyle", "").lower().split()
    if not style:
        problems.append(f"{iss_path}: no WizardStyle, so Inno draws its default wizard next to an "
                        "application whose own sheet is dark")
    else:
        if "modern" not in style:
            problems.append(f"{iss_path}: WizardStyle={vals.get('wizardstyle')} does not name the "
                            "modern base style")
        if "dark" not in style:
            problems.append(f"{iss_path}: WizardStyle={vals.get('wizardstyle')} names no appearance "
                            "mode, so the wizard renders light over a dark application")
        if "light" in style:
            problems.append(f"{iss_path}: WizardStyle={vals.get('wizardstyle')} names both light "
                            "and dark; which one wins is not worth leaving to the compiler")
        else:
            done.append(f"WizardStyle={' '.join(style)}")

    try:
        gen = load_wizard_generator()
    except Exception as exc:                        # any import failure is a report, not a traceback
        problems.append(f"packaging/make-wizard-images.py: cannot be loaded, so the wizard bitmaps "
                        f"cannot be checked against it: {exc}")
        return problems, done

    # The image area each directive fills, at 100% DPI, per the [Setup] reference -- and the reference
    # adds for both that an image *larger* than that is recommended, because a smaller one is enlarged
    # and reads as a blur on any display. This is the one property of the two assets that no other
    # check here can see: the files are valid BMPs and byte-identical to their generator either way, and
    # a resize of BANNER_SIZE back down to the old 164x314 would pass every other assertion in this
    # function while making the installer look soft on the machine of anyone not at 100% scaling.
    area = {"wizardimagefile": (202, 386), "wizardsmallimagefile": (58, 58)}
    wanted = [("wizardimagefile", gen.BANNER_NAME, gen.BANNER_SIZE, "the tall side panel"),
              ("wizardsmallimagefile", gen.SMALL_NAME, gen.SMALL_SIZE, "the corner thumbnail")]
    for directive, name, size, what in wanted:
        value = vals.get(directive, "")
        if not value:
            problems.append(f"{iss_path}: no {directive}, so Inno's own built-in art is shown where "
                            f"{what} should carry the application's mark")
            continue
        # The script anchors both paths to {#SourcePath} -- the directory holding the .iss, which is
        # this one. Anything after the last '}' is the file name either way, and Inno resolves a
        # relative path against that same directory, so this is the file it will open.
        tail = value.rsplit("}", 1)[-1].strip()
        path = os.path.join(root, "packaging", os.path.basename(tail))
        if not os.path.isfile(path):
            problems.append(f"{iss_path}: {directive}={value} names {path}, which does not exist "
                            "-- ISCC treats that as an error, on a tag build, with nothing else "
                            "having run first")
            continue
        found = check_bmp(path, size[0], size[1], what)
        problems += found
        if not found:
            done.append(f"{os.path.basename(path)}: {size[0]}x{size[1]} 24-bit BMP, {what}")
        min_w, min_h = area[directive]
        if size[0] < min_w or size[1] < min_h:
            problems.append(f"packaging/{os.path.basename(path)}: {size[0]}x{size[1]} is smaller than "
                            f"the {min_w}x{min_h} image area Inno measures for {what} at 100% DPI, so "
                            "Setup enlarges it and the banner is blurry on every display above that "
                            "scaling (the reference recommends an image larger than the area)")
        else:
            done.append(f"{os.path.basename(path)}: {size[0]}x{size[1]} is at least the "
                        f"{min_w}x{min_h} of {what}'s area at 100% DPI")

    # That the committed bytes are what the generator renders. An asset edited by hand is invisible
    # in a diff of the .py and unreadable in a diff of the .bmp, so regenerating into a temporary
    # directory and comparing bytes is the only review that survives contact with either file.
    with tempfile.TemporaryDirectory(prefix="muffin-wizard-") as tmp:
        for name, job in ((gen.BANNER_NAME, gen.JOB_BANNER), (gen.SMALL_NAME, gen.JOB_SMALL)):
            committed = os.path.join(root, "packaging", name)
            if not os.path.isfile(committed):
                continue                            # the existence check above already said so
            w, h, pix = gen.build(job)
            fresh = os.path.join(tmp, name)
            gen.write_bmp(fresh, w, h, pix)
            with open(fresh, "rb") as a, open(committed, "rb") as b:
                same = a.read() == b.read()
            if not same:
                problems.append(f"packaging/{name}: is not what packaging/make-wizard-images.py "
                                "renders now -- regenerate it with "
                                "`python3 packaging/make-wizard-images.py` and commit the result")
            else:
                done.append(f"{name}: byte-identical to its generator")
    return problems, done


def main(argv):
    root = argv[0] if argv else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    windows_dir = os.path.join(root, "packaging", "windows")
    rc_path = os.path.join(windows_dir, "app.rc")
    manifest_path = os.path.join(windows_dir, "app.manifest")
    ico_path = os.path.join(windows_dir, "muffin.ico")
    cmake_path = os.path.join(root, "CMakeLists.txt")

    problems = []

    rc_problems, found = check_rc(rc_path)
    problems += rc_problems
    problems += check_payload_names(rc_path, found)

    # Check the files app.rc actually names, not the names this script assumes: a payload
    # renamed on both sides (the .rc and the staging) is a valid build, and a checker that
    # still looked for "app.manifest" would report the file it no longer uses as missing.
    # With no statement to read a name from, fall back to the conventional file so its
    # contents are still reported; with a name that resolves to nothing on disk,
    # check_payload_names has already said so and there is nothing to parse.
    def target(role, default):
        payload = found[role]["payload"] if role in found else default
        resolved = os.path.join(windows_dir, payload)
        return resolved if os.path.isfile(resolved) else None

    manifest_target = target("manifest", "app.manifest")
    ico_target = target("icon", "muffin.ico")

    manifest_problems, level = check_manifest(manifest_target or manifest_path)
    problems += manifest_problems
    ico_problems, frames = check_ico(ico_target or ico_path)
    problems += ico_problems

    cmake_problems, wired = check_cmake(cmake_path, found)
    problems += cmake_problems

    iss_problems, wizard = check_installer(root)
    problems += iss_problems

    for problem in problems:
        print(f"FAIL  {problem}")
    if problems:
        print(f"\n{len(problems)} PROBLEM(S) in the Windows application resources")
        return 1

    for role in ("icon", "manifest"):
        if role in found:
            print(f"OK    {found[role]['payload']}: embedded from "
                  f"{os.path.relpath(rc_path, root)}:{found[role]['line']} as "
                  f"ordinal {WANTED[role][1]}, type "
                  f"{'ICON' if role == 'icon' else WANTED[role][0]}")
    print(f"OK    {os.path.relpath(manifest_target or manifest_path, root)}: "
          f"well-formed XML, level={level}")
    print(f"OK    {os.path.relpath(ico_target or ico_path, root)}: {frames}")

    print(f"OK    {os.path.relpath(cmake_path, root)}: "
          f"{len(wired)} resource-wiring assertions met")
    for line in wizard:
        print(f"OK    installer: {line}")

    print("\nWINDOWS RESOURCES OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
