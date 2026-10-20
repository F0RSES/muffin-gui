#!/usr/bin/env python3
"""Render the two Inno Setup wizard bitmaps from the application's own logo.

packaging/wizard_image.bmp   240x459  -- WizardImageFile, the tall panel on the Welcome and
                                          Setup Completed pages
packaging/wizard_small.bmp   147x147  -- WizardSmallImageFile, the square in the top-right corner

Why those two sizes, from the [Setup] reference (topic_setup_wizardimagefile and
topic_wizardsmallimagefile as of Inno Setup 6.6+):

  * The tall image's area keeps an aspect ratio of 164:314, and the area is 202x386 at 100% DPI --
    269x515 at 125%, 336x643 at 150%. The reference adds: "Specifying an image larger than 202x386
    is recommended to avoid the image looking blurry on higher-DPI systems", and gives the size of
    Inno's own built-in image as 240x459. So 240x459 is used: the same box as the built-in art, and
    headroom above the 100% area rather than a stretch at it. An earlier revision of this file
    shipped 164x314 -- the area as it was *before* 6.6.0 resized it, which is 66% of the pixels the
    100% area needs and so gets enlarged by 23% on every display, and far more at 125% and above.
  * The small image's area is square and 58x58 at 100% DPI, "larger than 58x58 is recommended", and
    Inno's built-in small image is 147x147. 147x147 is that same size, and the one case where a
    smaller file is not stretched but centered is under 58x58, so this lands as the built-in did.

Anything larger is shrunk and anything smaller is stretched, both preserving the ratio above; 24-bit
BMP because that is a format a bitmap reader cannot misinterpret and needs no decoder to agree.

Why this is code and not a painting
-----------------------------------
Inno Setup needs .bmp or .png here and resolves the path at compile time -- so a banner that drifts
from the logo, or a file that quietly stops existing, costs a release build on a tag where nothing
else has run yet. Drawing the bitmap from the same coordinates as
src/resources/icons/app.svg means there is one artwork definition, it is reviewable as text, and
check-win-resources.py can regenerate it and compare bytes to prove the committed file is exactly
what this script produces.

The output is deterministic: same input, same bytes. No timestamps, no randomness, and no float
accumulation whose order could differ between interpreters.

Usage: python3 packaging/make-wizard-images.py [--outdir DIR] [--png]
       (default outdir: this script's directory; --png also writes PNG previews, which is how the
        result gets looked at in a review without any image library in the repository)
"""

import argparse
import math
import os
import struct
import zlib

# The palette is the dark sheet's, not a new one: accent and accent2 are what the running
# application paints, so the installer shows the same object in the same colours.
PURPLE = (0xC0, 0x84, 0xFC)        # Theme::accent()  / theme_dark.qss
CYAN = (0x22, 0xD3, 0xEE)          # Theme::accentSecondary()
TEXT = (0xE7, 0xE9, 0xF2)          # Theme::text()
GROUND_TOP = (0x0B, 0x0C, 0x10)    # Theme::windowBackground()
GROUND_BOTTOM = (0x15, 0x16, 0x1C)  # Theme::surface()
CUP_FILL = (0x15, 0x16, 0x1C)

# The mark, in the 24x24 user space of src/resources/icons/app.svg. Kept as coordinates rather
# than as a path string because the rasterizer below works on signed distances, and because a
# commented-out number is easier to check against the SVG than a run-on "d=" attribute.
DOME_CENTER = (12.0, 12.3)
DOME_RADII = (8.3, 6.4)            # the cubic arc from (3.7,12.3) over (12,5.9) to (20.3,12.3)
CUP = [(5.2, 12.3), (18.8, 12.3), (17.3, 19.6), (6.7, 19.6)]   # the tapered wrapper
PLEATS = [(9.7, 13.6, 10.2, 19.7), (14.3, 13.6, 13.8, 19.7)]   # the two crease lines
STEM = (12.0, 5.9, 12.0, 4.4)      # the sprig holding the cherry
CHERRY = (12.0, 2.9, 1.5)          # cx, cy, r
STROKE = 1.7                       # stroke-width in the same units

# A 5x7 bitmap face, only ever the six letters of the wordmark. A real font would need a rasterizer
# and a file to ship; six glyphs of bitmaps need neither, and at these sizes a font would be hinted
# by hand anyway. "#" is an inked cell, "." is not.
FONT = {
    "M": ("#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"),
    "U": ("#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "F": ("#####", "#....", "#....", "####.", "#....", "#....", "#...."),
    "I": ("#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"),
    "N": ("#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"),
}
WORDMARK = "MUFFIN"


def smooth(dist, edge=1.0):
    """Coverage of a signed-distance value: 1 well inside, 0 well outside, linear across `edge`."""
    t = 0.5 - dist / edge
    if t <= 0.0:
        return 0.0
    if t >= 1.0:
        return 1.0
    return t


def seg_sdf(ax, ay, bx, by, px, py):
    """Unsigned distance from (px,py) to the segment a-b."""
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    ln = vx * vx + vy * vy
    t = 0.0 if ln == 0.0 else max(0.0, min(1.0, (wx * vx + wy * vy) / ln))
    return math.hypot(wx - t * vx, wy - t * vy)


def capsule(ax, ay, bx, by, width, px, py, scale):
    """Signed distance to the *outline* of a round-capped stroke, in output pixels."""
    centre = seg_sdf(ax, ay, bx, by, px, py) - width / 2.0
    return centre * scale


def arc_polyline(cx, cy, rx, ry, a0, a1, steps):
    """`steps` chords across an elliptic arc, in the arc's own coordinate space."""
    out = []
    for i in range(steps + 1):
        a = a0 + (a1 - a0) * i / steps
        out.append((cx + rx * math.cos(a), cy + ry * math.sin(a)))
    return out


def polyline_sdf(pts, px, py):
    """Unsigned distance to a polyline: the nearest of its segments."""
    best = float("inf")
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        d = seg_sdf(ax, ay, bx, by, px, py)
        if d < best:
            best = d
    return best


# The dome walked once, at import. Thirty-six chords across a half-ellipse of this radius put the
# sagitta error under a hundredth of a pixel at any size the wizard uses, so the polyline is exact
# for our purposes -- and recomputing it for every pixel would be pure waste, since these images
# get regenerated in a CI job that also diffs the bytes.
DOME_PTS = arc_polyline(DOME_CENTER[0], DOME_CENTER[1], DOME_RADII[0], DOME_RADII[1],
                        math.pi, 2.0 * math.pi, 36)


def convex_sdf(pts, px, py, scale):
    """Signed distance to a convex polygon's outline, in output pixels.

    Negative inside, positive outside: the max over edges of the distance *past* each edge's
    inward half-plane. The negation is the part that is easy to get backwards -- a dot product
    against an inward normal is positive for a point the polygon contains, so using it directly
    reports "outside" for the whole interior and the shape silently never draws at all.

    Exact outside anywhere a corner is not involved. The mark's cup has rounded corners in the
    original artwork, so the slight corner softening is closer to the SVG than a hard mitre.
    """
    n = len(pts)
    cx = sum(p[0] for p in pts) / n
    cy = sum(p[1] for p in pts) / n
    d = -1e18
    for i in range(n):
        ax, ay = pts[i]
        bx, by = pts[(i + 1) % n]
        nx, ny = -(by - ay), bx - ax
        ln = math.hypot(nx, ny) or 1.0
        nx, ny = nx / ln, ny / ln
        if (cx - ax) * nx + (cy - ay) * ny < 0.0:
            nx, ny = -nx, -ny                       # make it the inward normal
        d = max(d, -((px - ax) * nx + (py - ay) * ny))
    return d * scale


def mix(a, b, t):
    return tuple(int(round(ac + (bc - ac) * t)) for ac, bc in zip(a, b))


def over(dst, src, alpha):
    """Source-over onto an opaque background colour."""
    if alpha <= 0.0:
        return dst
    return tuple(int(round(d * (1.0 - alpha) + s * alpha)) for d, s in zip(dst, src))


def background(x, y, w, h, pools):
    """The vertical ground with two soft neon pools behind the mark.

    Squared ramp, so the panel stays near windowBackground at the top and only reaches surface at
    the bottom -- a linear ramp lifts the middle to a grey the dark sheet never shows.
    """
    t = (y / max(h - 1, 1)) ** 2
    col = mix(GROUND_TOP, GROUND_BOTTOM, t)
    sigma = min(w, h)
    for (px, py, colour, strength, spread) in pools:
        dx, dy = x - px, y - py
        f = math.exp(-(dx * dx + dy * dy) / (2.0 * (sigma * spread) ** 2))
        col = over(col, colour, strength * f)
    return col


def paint_mark(gx, gy, scale, stroke_px):
    """The logo composited for one glyph-space sample; returns (rgb, coverage).

    Layers go back to front: the wrapper's fill, then the neon outline of the same cup, the dome
    arc, the two pleats, the sprig and the cherry. Distances arrive in output pixels, which is what
    lets one smooth() give every edge the same one-pixel falloff at any size.
    """
    acc, cov = (0.0, 0.0, 0.0), 0.0

    def layer(c, a):
        nonlocal acc, cov
        if a <= 0.0:
            return
        na = cov + a * (1.0 - cov)
        if na <= 0.0:
            return
        acc = tuple((acc[i] * cov + c[i] * a * (1.0 - cov)) / na for i in range(3))
        cov = na

    cup = convex_sdf(CUP, gx, gy, scale)
    layer(CUP_FILL, smooth(cup))
    layer(PURPLE, smooth(abs(cup) - stroke_px / 2.0))
    dome = (polyline_sdf(DOME_PTS, gx, gy) - STROKE / 2.0) * scale
    layer(PURPLE, smooth(dome))
    for ax, ay, bx, by in PLEATS:
        layer(PURPLE, smooth(capsule(ax, ay, bx, by, STROKE, gx, gy, scale)))
    sx, sy, tx, ty = STEM
    layer(CYAN, smooth(capsule(sx, sy, tx, ty, STROKE, gx, gy, scale)))
    cx, cy, cr = CHERRY
    layer(CYAN, smooth(math.hypot(gx - cx, gy - cy) * scale - cr * scale))
    return acc, cov


def wordmark_box(text, cell):
    """Width and height in pixels of `text` set in the built-in face at `cell` pixels per cell."""
    return len(text) * 6 * cell - cell, 7 * cell


def wordmark_inked(text, cell, px, py, ox, oy):
    """True when (px,py) lands in an inked cell of the wordmark.

    Whole cells only, deliberately unsmoothed: a 5x7 face on an integer cell grid has no diagonal
    boundary to soften -- every edge is a whole pixel -- so antialiasing here would only blur it.
    """
    lx, ly = px - ox, py - oy
    if lx < 0 or ly < 0 or ly >= 7 * cell:
        return False
    li, inx = divmod(lx, 6 * cell)
    if li >= len(text) or inx >= 5 * cell:
        return False                                    # the gap between two letters
    rows = FONT.get(text[li])
    if rows is None:
        return False
    return rows[ly // cell][inx // cell] == "#"


# What the two files are. Named here rather than inline so check-win-resources.py can import them
# and assert that the .iss points at exactly these paths, at exactly these dimensions.
BANNER_NAME, BANNER_SIZE = "wizard_image.bmp", (240, 459)
SMALL_NAME, SMALL_SIZE = "wizard_small.bmp", (147, 147)


def render(w, h, mark_px, mark_center, pools, wordmark=None, rule=None):
    """Rasterize one wizard bitmap into a flat row-major RGB bytearray.

    `mark_px` is the width of the logo's 24x24 box in output pixels and `mark_center` where its
    centre lands; the stroke, the margins and every distance follow from those two, so a size is
    one number and not five that have to be kept in step. Samples are taken at pixel centres
    (+0.5), which is what puts the antialiased edge where the eye expects it instead of half a
    pixel high.
    """
    scale = mark_px / 24.0
    stroke_px = STROKE * scale
    ox = mark_center[0] - 12.0 * scale
    oy = mark_center[1] - 12.0 * scale

    wm = None
    if wordmark is not None:
        cell, wy, colour = wordmark
        width, _ = wordmark_box(WORDMARK, cell)
        wm = (cell, (w - width) // 2, wy, colour)

    pix = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            col = background(x, y, w, h, pools)

            rgb, cov = paint_mark((x + 0.5 - ox) / scale, (y + 0.5 - oy) / scale,
                                  scale, stroke_px)
            if cov > 0.0:
                col = over(col, tuple(int(round(c)) for c in rgb), cov)

            if wm is not None and wordmark_inked(WORDMARK, wm[0], x, y, wm[1], wm[2]):
                col = over(col, wm[3], 1.0)

            if rule is not None:
                rx0, rx1, ry0, ry1, rcol, ralpha = rule
                if rx0 <= x < rx1 and ry0 <= y < ry1:
                    col = over(col, rcol, ralpha)

            i = (y * w + x) * 3
            pix[i], pix[i + 1], pix[i + 2] = col

    return pix


def write_bmp(path, w, h, pix):
    """A 24-bit bottom-up device-independent BMP: the format Inno Setup has always taken.

    Written by hand because there is no image library in this repository's toolchain and an asset
    generator should not need one to be fetched. No palette, no compression, rows padded to four
    bytes -- the simplest thing a bitmap reader cannot misinterpret.
    """
    row_pad = (4 - (w * 3) % 4) % 4
    rows = []
    for y in range(h - 1, -1, -1):                     # bottom-up, as the format says
        start, end = y * w * 3, (y + 1) * w * 3
        bgr = bytearray(end - start)
        bgr[0::3], bgr[1::3], bgr[2::3] = pix[start + 2:end + 2:3], pix[start + 1:end + 1:3], \
            pix[start:end:3]
        rows.append(bytes(bgr) + b"\x00" * row_pad)
    data = b"".join(rows)
    with open(path, "wb") as fh:
        fh.write(b"BM" + struct.pack("<IHHI", 54 + len(data), 0, 0, 54))
        fh.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0))
        fh.write(data)
    return len(data) + 54


def write_png(path, w, h, pix):
    """A preview copy, for looking at the result in a review. Not an installer input."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += pix[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        fh.write(chunk(b"IEND", b""))


# The two compositions. Everything is in output pixels and every number is placed relative to the
# documented image box, so the banner's mark and the thumbnail's mark are the same object at
# different sizes and a resize is one edit.
#
#   mark_px ........ width of the logo's 24x24 box; the stroke follows it
#   mark_center .... where that box is centred
#   pools .......... [(x, y, colour, strength, spread)] of the neon behind the mark
#   wordmark ....... (pixels per face cell, top y, colour) or None
#   rule ........... (x0, x1, y0, y1, colour, alpha) or None
#
# The banner is laid out on the 164x314 box and scaled from it, which is what makes the paragraph
# above true of the size as well: BANNER_SIZE is the only number that changes when the wizard's
# image area changes.
BANNER_REF = (164, 314)


def banner_job(size):
    """The banner as drawn on BANNER_REF, enlarged to `size`.

    Scaling the reference numbers rather than restating them is what keeps the two files the same
    drawing: rendering this at exactly BANNER_SIZE reproduces the reference layout pixel for pixel,
    which check-win-resources.py's byte comparison against the committed file would notice if it
    did not.
    """
    rw, rh = BANNER_REF
    w, h = size
    sx, sy = w / rw, h / rh
    return dict(
        name=BANNER_NAME, size=size,
        mark_px=112 * sx, mark_center=(82.0 * sx, 124.0 * sy),
        pools=[(58.0 * sx, 106.0 * sy, PURPLE, 0.17, 0.46),
               (110.0 * sx, 152.0 * sy, CYAN, 0.10, 0.34)],
        wordmark=(max(1, round(3 * sx)), round(206 * sy), TEXT),
        rule=(round(62 * sx), round(102 * sx), round(234 * sy), round(236 * sy), CYAN, 0.55),
    )


JOB_BANNER = banner_job(BANNER_SIZE)
JOB_SMALL = dict(
    name=SMALL_NAME, size=SMALL_SIZE,
    mark_px=108, mark_center=(73.5, 73.5),
    pools=[(52.0, 56.0, PURPLE, 0.19, 0.50), (98.0, 94.0, CYAN, 0.11, 0.40)],
    wordmark=None, rule=None,
)


def build(job):
    """Rasterize one job spec into (width, height, pixel buffer)."""
    w, h = job["size"]
    return w, h, render(w, h, job["mark_px"], job["mark_center"], job["pools"],
                        job.get("wordmark"), job.get("rule"))


def main(argv):
    ap = argparse.ArgumentParser(description="Render the Inno Setup wizard bitmaps.")
    ap.add_argument("--outdir", default=os.path.dirname(os.path.abspath(__file__)),
                    help="where to write them (default: this script's directory)")
    ap.add_argument("--png", action="store_true",
                    help="also write .png previews, for looking at the result in a review")
    args = ap.parse_args(argv)
    os.makedirs(args.outdir, exist_ok=True)   # a generator should not need the directory to pre-exist

    for job in (JOB_BANNER, JOB_SMALL):
        w, h, pix = build(job)
        path = os.path.join(args.outdir, job["name"])
        size = write_bmp(path, w, h, pix)
        print(f"wrote {os.path.relpath(path, os.getcwd())}: {w}x{h}, {size} bytes")
        if args.png:
            write_png(os.path.splitext(path)[0] + ".png", w, h, pix)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(None))
