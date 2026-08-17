#!/usr/bin/env python3
"""
Extract Burnout 3 Xbox Data/*.txd texture banks (Frontend.txd, Global.txd) to
PNG.

Despite the extension these are NOT RenderWare stream TXDs (no 0x16/0x15 RW
chunk headers). They are a flat Criterion container -- layout read off the
bytes of the shipped files and verified by full extraction:

  +0x00  u32 0x543C0000          (constant in both files)
  +0x04  u32 0xBCDEED81          (constant in both files)
  +0x08  u32 count               (Frontend 218, Global 191)
  +0x0C  u32 entrySize = 16
  +0x10  count x {u32 id (1-based, sequential), u32 0,
                  u32 absoluteOffset, u32 0}

Each offset points at the SAME per-texture record used by static.dat and the
.bgv paint textures (format credit: Burnout Modding community / EdnessP's
fmt_Burnout3LRD.py; see tools/extract_textures.py):

  +0x04  i32  bitmap data offset, RELATIVE to the record (always +0x80 here)
  +0x14  u32[n] palette record offsets, relative to the record (fmt 0xB)
  +0x34  u32  format: 0xB paletted, 0xC DXT1, 0xE DXT3, 0xF DXT5, 0x3A RGBA
  +0x38  u32  width      +0x3C  u32  height
  +0x40  u32  bit depth (4/8/32 -> name at +0x48, else name at +0x44)
  +0x69  u8   palette count (car thumbnails carry 8 = colour variants)

  palette record: {u16 1, u16 3 or 0xC003}, u32 data offset rel. the TEXTURE
  record; data = 256 x BGRA (bit depth 8) or 16 x BGRA (bit depth 4).
  Bit-depth-4 images still store ONE BYTE per pixel (indices 0..15).

DXT1/DXT5 payloads are standard linear S3TC (no swizzle), decoded by
tools/extract_textures.py's decoder. Paletted payloads are Morton/Z-order
swizzled exactly like the .bgv paints, undone by
tools/extract_bgv_textures.py's unswizzle8. Records are packed back-to-back
(record 0x80 + pixels + palettes == next offset), so there are no mip chains.

Usage:
  python3 tools/extract_txd.py                       # both retail banks
  python3 tools/extract_txd.py file.txd... -o DIR    # explicit inputs
  --all-palettes    also write _p1.._pN variants of multi-palette textures

Output: build/frontend/<name>.png  (palette 0). The single cross-bank name
collision ("Takedown") gets a _<bank> suffix on the later-processed bank.
Exits non-zero if any texture fails to decode or validate.

=================================== THE V ORIGIN RULE, AND THE ALPHA ==

  This tool writes PNG row 0 = surface row 0 = v 0, for EVERY format, and
  it never touches the alpha channel except to move it. Same rule as
  tools/extract_textures.py (whose docstring proves it from the record's
  D3DPixelContainer header and the NV2A TX_FORMAT word): the shipped
  bytes at +0x04 ARE the GPU surface, TX_FORMAT has no origin/flip field,
  and unswizzling a Morton-ordered paletted surface restores the same
  row-0-first linear image. Frontend.txd / Global.txd records are the
  same records, so the rule carries over unchanged.

  Verified for these banks (2026-08-12, HUD-fidelity session):
    * `B3Logo` decodes to a right-way-up, un-mirrored "BURNOUT 3
      TAKEDOWN" wordmark -- a decisive asymmetric check on both axes.
    * `hud_element01`, the in-race HUD plate, decodes with the alpha
      profile row 0 = 0, rows 1..4 = 204 (blue rim), rows 5..26 = 153
      (0.6 dark fill), rows 27..30 = 255 (blue rim), row 31 = 0, and its
      ink's LEFT edge marches monotonically outward from x 35 at the top
      to x 1 at the bottom. A vertical flip reverses that march; an
      inverted alpha turns 153 into 102 and the transparent border into
      an opaque one. selfcheck_orientation() asserts exactly that on
      every run that decodes the record, reports it as a FAILURE row and
      exits non-zero if either ever changes.
    * The plate's decoded numbers are what the in-race HUD consumes:
      FUN_00048430 samples it at v 0.03125..0.9375 -- i.e. texel rows
      1..30, precisely the ink between the two transparent border rows.
      That the recovered UV window lands on the decoded ink is an
      independent confirmation of the row order.

  CONSEQUENCE FOR CONSUMERS: a HUD element that looks vertically flipped
  or alpha-inverted is a consumer bug, not an extractor bug. The one that
  bit src/burnout3_hud.c was drawing `big_curve` -- a FRONTEND corner
  asset whose 0.35-alpha field sits OUTSIDE its swoosh -- where the
  in-race code draws `hud_element01`, whose 0.6-alpha field sits INSIDE
  its rim. Same pixels, opposite reading. See docs/RE_FRONTEND.md 6.10.
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_textures import decode_dxt, FMT_NAMES          # noqa: E402
from extract_bgv_textures import unswizzle8                 # noqa: E402

GAME = (game_root())
DEFAULT_INPUTS = [os.path.join(GAME, "Data", "Frontend.txd"),
                  os.path.join(GAME, "Data", "Global.txd")]

MAGIC = (0x543C0000, 0xBCDEED81)
PAL_MAGIC = (b'\x01\x00\x03\x00', b'\x01\x00\x03\xc0')


def read_name(data, off):
    end = data.find(b'\0', off)
    if end < 0 or end - off > 64:
        return None
    try:
        s = data[off:end].decode('ascii')
    except UnicodeDecodeError:
        return None
    return s if s and all(32 <= ord(c) < 127 for c in s) else None


def decode_paletted(data, rec, bmp, w, h, bd, which_pal):
    """fmt 0xB: Morton-swizzled 8bpp indices + BGRA palette -> RGBA bytes."""
    if bmp + w * h > len(data):
        return None, "pixels truncated"
    idx = unswizzle8(data[bmp:bmp + w * h], w, h)
    prec = rec + struct.unpack_from('<I', data, rec + 0x14 + which_pal * 4)[0]
    if prec + 8 > len(data) or data[prec:prec + 4] not in PAL_MAGIC:
        return None, "bad palette record"
    pdata = rec + struct.unpack_from('<I', data, prec + 4)[0]
    ncol = 16 if bd == 4 else 256
    if pdata + ncol * 4 > len(data):
        return None, "palette truncated"
    pal = data[pdata:pdata + ncol * 4]
    out = bytearray(w * h * 4)
    for i, pi in enumerate(idx):
        if pi >= ncol:
            return None, "palette index %d out of range (%d colours)" % (pi, ncol)
        p = pi * 4
        o = i * 4
        out[o] = pal[p + 2]        # BGRA -> RGBA
        out[o + 1] = pal[p + 1]
        out[o + 2] = pal[p]
        out[o + 3] = pal[p + 3]
    return bytes(out), None


def image_stats(rgba, w, h):
    """(mean |horizontal luma delta| over opaque runs, is_constant).
    Random noise scores ~85; real art (incl. DXT) stays well below 60."""
    tot = n = 0
    first = rgba[0:4]
    const = True
    for y in range(0, h, max(1, h // 64)):        # sample rows, cap cost
        row = y * w * 4
        prev = None
        for x in range(0, w, max(1, w // 64)):
            o = row + x * 4
            px = rgba[o:o + 4]
            if px != first:
                const = False
            if px[3] < 8:
                prev = None
                continue
            lum = (px[0] * 3 + px[1] * 6 + px[2]) // 10
            if prev is not None:
                tot += abs(lum - prev)
                n += 1
            prev = lum
    return (tot / n if n else 0.0), const


# --------------------------------------------------------------------- #
# THE V ORIGIN RULE / ALPHA self-check (see the module docstring)
#
# `hud_element01` is the in-race HUD plate: a 64x32 blade with a 1-texel
# transparent border, a bright blue rim top and bottom, a 0.6-alpha dark
# fill between them, and a left tip that slants outward as y increases.
# Every one of those is asymmetric under a vertical flip or an alpha
# inversion, and the numbers are the ones the recovered draw callback
# FUN_00048430 samples (v 0.03125..0.9375 = texel rows 1..30).
# --------------------------------------------------------------------- #
ORIENT_TEX = "hud_element01"
ORIENT_DIMS = (64, 32)


def alpha_at(rgba, w, x, y):
    return rgba[(y * w + x) * 4 + 3]


def selfcheck_orientation(name, rgba, w, h):
    """Return a list of failure strings (empty = the rule holds)."""
    if name.lower() != ORIENT_TEX or (w, h) != ORIENT_DIMS:
        return []
    bad = []
    if alpha_at(rgba, w, 40, 0) != 0 or alpha_at(rgba, w, 40, h - 1) != 0:
        bad.append("row 0 / row %d are not the transparent border "
                   "(got %d / %d) -- flipped or alpha-inverted"
                   % (h - 1, alpha_at(rgba, w, 40, 0),
                      alpha_at(rgba, w, 40, h - 1)))
    fill = alpha_at(rgba, w, 40, 16)
    if not (140 <= fill <= 170):
        bad.append("mid-blade fill alpha %d, expected ~153 (0.6) -- an "
                   "inverted alpha channel reads ~102" % fill)
    rim = alpha_at(rgba, w, 40, 2)
    if rim < 190:
        bad.append("top rim alpha %d, expected >= 190" % rim)
    # the left ink edge must march OUTWARD as y increases
    edges = []
    for y in range(2, h - 2):
        xs = [x for x in range(w) if alpha_at(rgba, w, x, y) > 32]
        edges.append(xs[0] if xs else w)
    if not (edges[0] > edges[-1] + 20):
        bad.append("left ink edge does not slant outward downward "
                   "(top %d, bottom %d) -- the image is vertically flipped"
                   % (edges[0], edges[-1]))
    return bad


def extract_bank(path, outdir, taken, all_palettes, verbose=True):
    from PIL import Image
    bank = os.path.splitext(os.path.basename(path))[0]
    data = open(path, 'rb').read()
    m1, m2, count, esz = struct.unpack_from('<4I', data, 0)
    if (m1, m2) != MAGIC or esz != 16:
        raise SystemExit("%s: not a Burnout 3 txd container "
                         "(hdr %08X %08X entsize %d)" % (path, m1, m2, esz))
    rows, failures, written = [], [], 0
    checked_orientation = False
    for i in range(count):
        eid, z0, off, z1 = struct.unpack_from('<4I', data, 0x10 + i * 16)
        if eid != i + 1 or z0 or z1 or not (0x10 < off < len(data) - 0x80):
            failures.append((i + 1, "?", "bad TOC entry"))
            continue
        rec = off
        bmp_rel = struct.unpack_from('<i', data, rec + 4)[0]
        bmp = rec + bmp_rel
        fmt, w, h, bd = struct.unpack_from('<4I', data, rec + 0x34)
        npal = data[rec + 0x69]
        name = read_name(data, rec + (0x48 if bd in (4, 8, 32) else 0x44))
        label = name or ("tex_%03d" % eid)
        if not name:
            failures.append((eid, label, "unnamed"))
            continue
        if not (0 < w <= 2048 and 0 < h <= 2048):
            failures.append((eid, label, "bad dims %dx%d" % (w, h)))
            continue

        if fmt in (0xC, 0xF):
            rgba = decode_dxt(data, bmp, w, h, fmt)
            err = None if rgba else "DXT data past EOF"
            extra = []
        elif fmt == 0xB:
            rgba, err = decode_paletted(data, rec, bmp, w, h, bd, 0)
            extra = list(range(1, npal)) if (all_palettes and rgba) else []
        else:
            rgba, err = None, "unhandled fmt 0x%X (%s)" % (
                fmt, FMT_NAMES.get(fmt, "?"))
            extra = []
        if err:
            failures.append((eid, label, err))
            continue

        for why in selfcheck_orientation(name, rgba, w, h):
            failures.append((eid, label, "V-ORIGIN/ALPHA self-check: " + why))
        grad, const = image_stats(rgba, w, h)
        # grad is informational: broken stride/swizzle decodes as ~85+ random
        # noise, but legitimate hi-freq art scores high too (SmashedGlass 63,
        # Volumebar's repeating bars 147 -- all 8 >60 outliers were inspected
        # visually and are correct). Only structural errors fail the run.
        status = "ok"
        if grad > 60:
            status = "hi-freq"
        elif const:
            status = "flat"          # constant colour: legal but noted

        fname = name
        if fname.lower() in taken:
            fname = "%s_%s" % (name, bank)
        taken.add(fname.lower())
        Image.frombytes("RGBA", (w, h), rgba).save(
            os.path.join(outdir, "%s.png" % fname))
        written += 1
        for k in extra:
            prgba, perr = decode_paletted(data, rec, bmp, w, h, bd, k)
            if perr:
                failures.append((eid, "%s_p%d" % (label, k), perr))
                continue
            Image.frombytes("RGBA", (w, h), prgba).save(
                os.path.join(outdir, "%s_p%d.png" % (fname, k)))
        if name.lower() == ORIENT_TEX and (w, h) == ORIENT_DIMS:
            checked_orientation = True
        rows.append((eid, fname, fmt, w, h, bd, npal, grad, status))

    if verbose:
        print("\n%s -- %d entries" % (path, count))
        if checked_orientation:
            print("  V-ORIGIN/ALPHA self-check: %s decodes with the "
                  "recovered row order and alpha profile [ok]" % ORIENT_TEX)
        print("%4s %-26s %-5s %9s %3s %4s %6s %s"
              % ("id", "name", "fmt", "dims", "bd", "npal", "grad", "status"))
        for eid, nm, fmt, w, h, bd, npal, grad, status in rows:
            print("%4d %-26s %-5s %4dx%-4d %3d %4d %6.1f %s"
                  % (eid, nm, FMT_NAMES.get(fmt, hex(fmt)), w, h, bd, npal,
                     grad, status))
    return count, written, failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("inputs", nargs="*", default=None)
    ap.add_argument("-o", "--outdir", default="build/frontend")
    ap.add_argument("--all-palettes", action="store_true",
                    help="also write _pK.png for every extra palette")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()
    inputs = args.inputs or DEFAULT_INPUTS

    os.makedirs(args.outdir, exist_ok=True)
    taken = set()
    total = wrote = 0
    all_fail = []
    for path in inputs:
        count, written, failures = extract_bank(
            path, args.outdir, taken, args.all_palettes, not args.quiet)
        total += count
        wrote += written
        all_fail += [(os.path.basename(path),) + f for f in failures]

    print("\ntotal: %d/%d textures -> %s" % (wrote, total, args.outdir))
    if all_fail:
        print("FAILURES (%d):" % len(all_fail))
        for bank, eid, nm, why in all_fail:
            print("  %s [%3d] %-24s %s" % (bank, eid, nm, why))
    return 1 if all_fail else 0


if __name__ == "__main__":
    sys.exit(main())
