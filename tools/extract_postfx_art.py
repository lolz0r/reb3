#!/usr/bin/env python3
"""
Extract the world post-FX art: the per-track SKY textures out of
`Tracks/<region>/<track>/enviro.dat`, and (optionally) report on the
`radialblurmask` entry of the global texture dictionary.

WHY THIS TOOL EXISTS
--------------------
`tools/extract_track.py` decodes static.dat and `tools/extract_textures.py`
decodes its texture table.  Neither of them touches `enviro.dat`, and the sky
is not in static.dat at all: NO material in either US/C3_V1 (167 records) or
AS/C1_V1 (180 records) names a sky/cloud texture.  The sky is drawn from a
procedurally generated dome (`FUN_00032020`) textured with the images in
`enviro.dat` (`FUN_001888f0` @ 0x001889D5 builds the path
`"<track dir>/enviro.dat"` and loads it).  See docs/RE_POSTFX.md.

FORMAT
------
`enviro.dat` is a baked memory image, like Gamedata.bgd: it starts with the
environment record itself and its internal pointers are FILE-RELATIVE offsets
that the loader relocates by adding the base address.  [C] FUN_00188880
(0x00188880) does exactly that for four fields and hands each to the texture
registrar FUN_001C8E20:

    env+0x98  -> texture 0      relocated + registered   [C] 0x00188893
    env+0x9C  -> texture 1      relocated + registered   [C] 0x001888A9
    env+0xA0  -> texture 2      relocated + registered   [C] 0x001888BF
    env+0xA4  -> texture 3      relocated + registered   [C] 0x001888D5

and FUN_001888F0's tail publishes three of them to the sky draw:

    DAT_0045BC10 := env+0x98    the palette-carrying sky source   [C] 0x00188A9B
    DAT_0045D11C := env+0x9C    sky-dome texture stage 1          [C] 0x00188AA7
    DAT_0045D118 := env+0xA0    ground-dome texture stage 1       [C] 0x00188AAD

Each of those points at an ordinary Burnout texture record -- the same layout
`tools/extract_textures.py` documents for static.dat:

    +0x04  i32  bitmap data offset, RELATIVE to the record
    +0x34  u32  format  (0xB paletted, 0xC DXT1, 0xE DXT3, 0xF DXT5, 0x3A RGBA)
    +0x38  u32  width
    +0x3C  u32  height
    +0x40  u32  bit depth -- 4/8/32 => name at +0x48, else name at +0x44

There is no texture *table* in enviro.dat (no count/pointer array like
static.dat +0x16/+0x18), so this tool reads the four record offsets straight
out of the environment header at +0x98..+0xA4 and validates each one before
decoding it.  A conservative full-file scan (`--scan`) is available as a
cross-check; on US/C3_V1 the scan finds exactly the records the header names,
which is the independent confirmation that +0x98..+0xA4 is the right place.

USAGE
    python3 tools/extract_postfx_art.py                       # default track
    python3 tools/extract_postfx_art.py --track US/C3_V1
    python3 tools/extract_postfx_art.py --all                 # every track
    python3 tools/extract_postfx_art.py --scan                # verify by scan
Output: build/postfx/<REGION>_<TRACK>_<name>.png plus a manifest .txt.
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

GAME = (game_root())
TRACKS = os.path.join(GAME, "Tracks")
DEFAULT_TRACK = "US/C3_V1"
OUTDIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "build", "postfx")

# [C] FUN_00188880 -- the four relocated texture-pointer fields.
ENV_TEX_FIELDS = (0x98, 0x9C, 0xA0, 0xA4)

# The four slots are role-fixed across all 40 shipped tracks even though the
# artists' texture NAMES are not (US/C3_V1 "gradients"/"clouds", AS/C1_V1
# "bk_sky"/"bk_skyclouds", EU/C1_V1 "skygrad1"/"clouds", ...). Verified by
# tools/validate_postfx.py section B: +0x98 is 32x32 DXT5 everywhere, +0x9C is
# 1024 wide DXT5 everywhere, +0xA4 is 128x256 paletted everywhere, and +0xA0
# is absent on some tracks (AS/C1_V1, AS/C1_V2, AS/C2_V1, AS/C2_V2). Output
# files are therefore named by ROLE, not by the artist's name, so the runtime
# can find them on any track.
SLOT_ROLE = {0x98: "gradients",      # the 32x32 sky-gradient LUT source
             0x9C: "clouds",         # the sky-dome cloud panorama
             0xA0: "envmapclouds",   # the reflection/env-map cloud sheet
             0xA4: "suncorona"}      # the sun sprite (paletted)

FMT_NAMES = {0xB: "Paletted", 0xC: "DXT1", 0xE: "DXT3", 0xF: "DXT5",
             0x3A: "RGBA"}


# ---------------------------------------------------------------- decoding
# DXT decode is an independent reimplementation of the same block format
# tools/extract_textures.py uses (format credit: Burnout Modding community).

def rgb565(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def dxt_colour_block(data, o, out, ox, oy, w, h, dxt1):
    c0, c1 = struct.unpack_from('<2H', data, o)
    bits = struct.unpack_from('<I', data, o + 4)[0]
    r0, g0, b0 = rgb565(c0)
    r1, g1, b1 = rgb565(c1)
    if c0 > c1 or not dxt1:
        pal = [(r0, g0, b0, 255), (r1, g1, b1, 255),
               ((2*r0 + r1)//3, (2*g0 + g1)//3, (2*b0 + b1)//3, 255),
               ((r0 + 2*r1)//3, (g0 + 2*g1)//3, (b0 + 2*b1)//3, 255)]
    else:
        pal = [(r0, g0, b0, 255), (r1, g1, b1, 255),
               ((r0 + r1)//2, (g0 + g1)//2, (b0 + b1)//2, 255), (0, 0, 0, 0)]
    for py in range(4):
        for px in range(4):
            x, y = ox + px, oy + py
            if x >= w or y >= h:
                continue
            out[(y*w + x)*4:(y*w + x)*4 + 4] = bytes(
                pal[(bits >> (2*(py*4 + px))) & 3])


def dxt5_alpha_block(data, o, out, ox, oy, w, h):
    a0, a1 = data[o], data[o + 1]
    if a0 > a1:
        al = [a0, a1] + [((6 - i)*a0 + (i + 1)*a1)//7 for i in range(6)]
    else:
        al = [a0, a1] + [((4 - i)*a0 + (i + 1)*a1)//5 for i in range(4)] + [0, 255]
    bits = int.from_bytes(data[o + 2:o + 8], 'little')
    for py in range(4):
        for px in range(4):
            x, y = ox + px, oy + py
            if x >= w or y >= h:
                continue
            out[(y*w + x)*4 + 3] = al[(bits >> (3*(py*4 + px))) & 7]


def decode_dxt(data, off, w, h, fmt):
    out = bytearray(w*h*4)
    bw, bh = (w + 3)//4, (h + 3)//4
    step = 8 if fmt == 0xC else 16
    if off + bw*bh*step > len(data):
        return None
    for by in range(bh):
        for bx in range(bw):
            o = off + (by*bw + bx)*step
            if fmt == 0xC:
                dxt_colour_block(data, o, out, bx*4, by*4, w, h, True)
            else:
                dxt_colour_block(data, o + 8, out, bx*4, by*4, w, h, False)
                dxt5_alpha_block(data, o, out, bx*4, by*4, w, h)
    return bytes(out)


def read_name(data, off, limit=64):
    end = data.find(b'\0', off)
    if end < 0 or end - off > limit:
        return None
    try:
        return data[off:end].decode('ascii')
    except UnicodeDecodeError:
        return None


# ---------------------------------------------------------------- records
class TexRecord:
    __slots__ = ("rec", "fmt", "w", "h", "depth", "name", "data_off")

    def __repr__(self):
        return "0x%06X %-14s %4dx%-4d %-9s data@0x%06X" % (
            self.rec, self.name, self.w, self.h,
            FMT_NAMES.get(self.fmt, "0x%X" % self.fmt), self.data_off)


def parse_record(d, rec):
    """Validate + parse one texture record at file offset `rec`."""
    if rec < 0 or rec + 0x70 > len(d):
        return None
    fmt = struct.unpack_from('<I', d, rec + 0x34)[0]
    w = struct.unpack_from('<I', d, rec + 0x38)[0]
    h = struct.unpack_from('<I', d, rec + 0x3C)[0]
    depth = struct.unpack_from('<I', d, rec + 0x40)[0]
    if fmt not in FMT_NAMES:
        return None
    if not (0 < w <= 4096 and 0 < h <= 4096):
        return None
    if (w & (w - 1)) or (h & (h - 1)):
        return None
    name = read_name(d, rec + (0x48 if depth in (4, 8, 32) else 0x44))
    if not name or not name.replace('_', '').replace('.', '').isalnum():
        return None
    rel = struct.unpack_from('<i', d, rec + 0x04)[0]
    data_off = rec + rel
    if not (0 < data_off < len(d)):
        return None
    t = TexRecord()
    t.rec, t.fmt, t.w, t.h, t.depth = rec, fmt, w, h, depth
    t.name, t.data_off = name, data_off
    return t


# [C] The SUN VECTOR, env+0x80.
#
# The environment record is the global at 0x0060E040 (FUN_001AE340 @0x001AE37A
# loads EAX with it and calls the LUT builder FUN_001891F0; 0x0060E170 =
# env+0x130 is the screen index it stores at 0x001AE37F). The loader's tail
# FUN_001888F0 @0x00188B40..0x00188B95 copies env+0x80 into BOTH per-screen
# 64-byte blocks at env+0xB0 and env+0xF0, and FUN_001891F0 @0x00189243 reads
# `env + 0xB0 + 0x40*screen` and hands its first three floats to FUN_00189660,
# which turns them into the (azimuth, elevation) the sun-glow LUT pass is
# centred on.
#
# On all 40 shipped tracks env+0x80 is a UNIT vector with a NEGATIVE y, i.e.
# the direction the light TRAVELS, not the direction to the sun. It is written
# out here exactly as stored; src/burnout3_postfx.c negates it.
ENV_SUN_DIR = 0x80

def read_sun(d):
    """env+0x80 as (x, y, z) plus its length, for the sidecar and the tests."""
    x, y, z, _w = struct.unpack_from('<4f', d, ENV_SUN_DIR)
    n = (x * x + y * y + z * z) ** 0.5
    return x, y, z, n


def records_from_header(d):
    """The four texture pointers the loader relocates. [C] FUN_00188880."""
    out = []
    for f in ENV_TEX_FIELDS:
        off = struct.unpack_from('<i', d, f)[0]
        if off == 0:
            continue
        t = parse_record(d, off)
        if t:
            out.append((f, t))
    return out


def records_by_scan(d):
    """Independent cross-check: brute-force every 4-byte-aligned position."""
    out, seen = [], set()
    for rec in range(0, len(d) - 0x70, 4):
        t = parse_record(d, rec)
        if t and t.name not in seen:
            seen.add(t.name)
            out.append(t)
    return out


# ---------------------------------------------------------------- driver
def extract_track(track, do_scan=False):
    path = os.path.join(TRACKS, track, "enviro.dat")
    if not os.path.exists(path):
        return None
    d = open(path, 'rb').read()
    hdr = records_from_header(d)
    lines = ["%s  enviro.dat  %d bytes" % (track, len(d))]
    for f, t in hdr:
        lines.append("  env+0x%02X %-13s -> %s" % (f, SLOT_ROLE[f], t))
    if do_scan:
        sc = records_by_scan(d)
        lines.append("  --scan found %d record(s):" % len(sc))
        for t in sc:
            lines.append("      %s" % t)
        hdr_recs = {t.rec for _, t in hdr}
        scan_recs = {t.rec for t in sc}
        lines.append("  scan == header: %s" % (hdr_recs == scan_recs))
    os.makedirs(OUTDIR, exist_ok=True)
    tag = track.replace('/', '_')
    written = 0

    # the per-track environment sidecar the runtime reads (sun vector + the
    # ground-quad v span). Same role-named convention as the PNGs.
    sx, sy, sz, sn = read_sun(d)
    envp = os.path.join(OUTDIR, "%s_env.txt" % tag)
    with open(envp, 'w') as fh:
        fh.write("# enviro.dat %s -- see docs/RE_POSTFX.md 3.4\n" % track)
        fh.write("# env+0x80: the SUN vector as stored (direction of travel,\n")
        fh.write("#           y < 0); burnout3_postfx.c negates it.\n")
        fh.write("sun_dir %.8f %.8f %.8f\n" % (sx, sy, sz))
        fh.write("sun_len %.8f\n" % sn)
        fh.write("# The 13th glow quad's source v span is DAT_0060E1C0/C4 =\n")
        fh.write("# env+0x180/0x184 -- a .bss pair with no initialiser found,\n")
        fh.write("# i.e. 0.0; it is a runtime field, NOT a field of this file.\n")
    lines.append("  env+0x80 sun (%.4f, %.4f, %.4f) |v|=%.4f -> %s"
                 % (sx, sy, sz, sn, os.path.relpath(envp)))
    try:
        from PIL import Image
    except ImportError:
        Image = None
    for f, t in hdr:
        if t.fmt in (0xC, 0xE, 0xF) and Image is not None:
            px = decode_dxt(d, t.data_off, t.w, t.h, t.fmt)
            if px:
                img = Image.frombytes('RGBA', (t.w, t.h), px)
                out = os.path.join(OUTDIR, "%s_%s.png" % (tag, SLOT_ROLE[f]))
                img.save(out)
                written += 1
                lines.append("  wrote %s" % os.path.relpath(out))
        elif t.fmt == 0xB:
            lines.append("  %s is PALETTED (0xB) -- palette lives outside the "
                         "record; not decoded here [?]" % t.name)
        else:
            lines.append("  %s format 0x%X not decoded" % (t.name, t.fmt))
    return "\n".join(lines), written


def main():
    args = sys.argv[1:]
    do_scan = "--scan" in args
    do_all = "--all" in args
    track = DEFAULT_TRACK
    if "--track" in args:
        track = args[args.index("--track") + 1]
    tracks = []
    if do_all:
        for region in sorted(os.listdir(TRACKS)):
            rd = os.path.join(TRACKS, region)
            if os.path.isdir(rd):
                for t in sorted(os.listdir(rd)):
                    if os.path.exists(os.path.join(rd, t, "enviro.dat")):
                        tracks.append("%s/%s" % (region, t))
    else:
        tracks = [track]
    os.makedirs(OUTDIR, exist_ok=True)
    manifest, total = [], 0
    for t in tracks:
        r = extract_track(t, do_scan)
        if r is None:
            print("no enviro.dat for %s" % t)
            continue
        text, n = r
        total += n
        print(text)
        manifest.append(text)
    with open(os.path.join(OUTDIR, "enviro_manifest.txt"), 'w') as f:
        f.write("\n".join(manifest) + "\n")
    print("\n%d PNG(s) -> %s" % (total, OUTDIR))


if __name__ == "__main__":
    main()
