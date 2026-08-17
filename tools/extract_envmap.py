#!/usr/bin/env python3
"""
Extract the per-track CAR ENVIRONMENT MAP -- the reflection sheet the car body
shader samples on texture stage 1 -- out of every track's `enviro.dat`, into
`build/tracks/<REGION>_<TRACK>/envmap.png`.

WHY THIS TOOL EXISTS
--------------------
The car body's NV2A programs are recovered (docs/RE_CARFX.md 13):

  * the vertex program at 0x003E7D58 instruction 21 emits
        oT1.xyz = 2N(N.V) - V          -- the WORLD-SPACE REFLECTION VECTOR
    and never writes oT1.w;                                             [C]
  * the body pixel-shader def at 0x003E8468 has
        PSTextureModes = 0x21 -> stage 0 PROJECT2D, stage 1 PROJECT2D
    and combiner stage 3 does
        r0.rgb = r0.a*t1.rgb + r0.rgb*(1 - r0.a)
    i.e. it LERPS the shaded paint towards `t1` by the Fresnel-weighted
    gloss mask -- `t1` is the environment map;                          [C]
  * `FUN_00031690` binds that stage from the single global `DAT_004D6C00`
    (`MOV ECX,[0x004D6C00]; MOV [0x0075DB74],ECX` @0x00031740) and sets the
    stage's ADDRESSU/ADDRESSV to 3 = CLAMP (0x0075D744 / 0x0075D754).   [C]

WHAT `DAT_004D6C00` HOLDS IS `[?]` AND STAYS `[?]`.  Its writer is not in the
image; see `scratchpad/carenv/INTEGRATION_NOTE.md` for the four independent
exhaustive searches (literal scan, encoding-exhaustive disp32 scan,
constant-propagated effective-address scan over all 7434 functions, and two
whole-image dynamic sweeps under Unicorn).

THE SUBSTITUTE THIS TOOL EXTRACTS IS `[S]`, and it is the only reflection-
shaped per-track image that exists in the shipped data.  `enviro.dat`'s header
carries four texture-record offsets that `FUN_00188880` relocates and
registers -- +0x98, +0x9C, +0xA0, +0xA4 (0x00188893 / 0x001888A9 / 0x001888BF /
0x001888D5)                                                            [C].
Across the 37 shipped track directories the +0xA0 slot is named, by the
artists, `envmapclouds` / `Envmapclouds` / `WC_SkyEnv` / `ATB_SkyEnv` /
`R_Sky_env` / `MRTHN1_SkyEnv` / `P2P1_SkyEnv` / `AS_m_envap` /
`Clouds_prestorm_envmap` -- and US/P2_V1's 512x512 `Envmapclouds` is a
textbook SPHERE MAP: a circular disc of cloud inscribed in the square.  It is
an environment map, it is per track, and it is bright sky in every weather.

The other three slots are ruled out by shape and by use: +0x98 is a 32x32
gradient LUT on every track, +0x9C is the 1024-wide sky-dome cloud PANORAMA,
+0xA4 is the paletted sun sprite.  Only +0xA0 is a reflection sheet.

Four tracks ship no +0xA0 record at all (AS/C1_V1, AS/C1_V2, AS/C2_V1,
AS/C2_V2 -- their sky is `bk_sky`/`bk_skyclouds` only).  This tool writes no
`envmap.png` for them and the runtime keeps its existing probe-based fallback;
nothing is invented to fill the hole.

FORMAT (identical to static.dat's texture records, which
tools/extract_textures.py documents)
    +0x04  i32  bitmap data offset, RELATIVE to the record
    +0x34  u32  format  (0xB paletted, 0xC DXT1, 0xE DXT3, 0xF DXT5, 0x3A RGBA)
    +0x38  u32  width
    +0x3C  u32  height
    +0x40  u32  bit depth -- 4/8/32 => name at +0x48, else name at +0x44

V-ORIGIN: block row 0 is written as PNG row 0 and no flip is applied anywhere,
so v=0 addresses texel row 0 -- the repo-wide rule.

USAGE
    python3 tools/extract_envmap.py                 # every track
    python3 tools/extract_envmap.py --track US/C3_V1
    python3 tools/extract_envmap.py --list          # report, write nothing
Output: build/tracks/<REGION>_<TRACK>/envmap.png plus build/tracks/envmap.txt
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import os
import struct
import sys

GAME = os.environ.get(
    "B3_GAME_DIR",
    game_root())
TRACKS = os.path.join(GAME, "Tracks")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTROOT = os.path.join(ROOT, "build", "tracks")

# [C] FUN_00188880 relocates + registers exactly these four; +0xA0 is the
# reflection sheet (0x001888BF).
ENV_TEX_FIELD_ENVMAP = 0xA0

FMT_NAMES = {0xB: "Paletted", 0xC: "DXT1", 0xE: "DXT3", 0xF: "DXT5",
             0x3A: "RGBA"}


# ---------------------------------------------------------------- decoding
def rgb565(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def _colour_block(data, o, out, ox, oy, w, h, dxt1):
    c0, c1 = struct.unpack_from('<2H', data, o)
    bits, = struct.unpack_from('<I', data, o + 4)
    r0, g0, b0 = rgb565(c0)
    r1, g1, b1 = rgb565(c1)
    if c0 > c1 or not dxt1:
        pal = [(r0, g0, b0, 255), (r1, g1, b1, 255),
               ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255),
               ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255)]
    else:
        pal = [(r0, g0, b0, 255), (r1, g1, b1, 255),
               ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255),
               (0, 0, 0, 0)]
    for py in range(4):
        for px in range(4):
            x, y = ox + px, oy + py
            if x >= w or y >= h:
                continue
            k = (y * w + x) * 4
            out[k:k + 4] = bytes(pal[(bits >> (2 * (py * 4 + px))) & 3])


def _dxt5_alpha_block(data, o, out, ox, oy, w, h):
    a0, a1 = data[o], data[o + 1]
    if a0 > a1:
        al = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
    else:
        al = ([a0, a1] + [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)]
              + [0, 255])
    bits = int.from_bytes(data[o + 2:o + 8], 'little')
    for py in range(4):
        for px in range(4):
            x, y = ox + px, oy + py
            if x >= w or y >= h:
                continue
            out[(y * w + x) * 4 + 3] = al[(bits >> (3 * (py * 4 + px))) & 7]


def _dxt3_alpha_block(data, o, out, ox, oy, w, h):
    bits = int.from_bytes(data[o:o + 8], 'little')
    for py in range(4):
        for px in range(4):
            x, y = ox + px, oy + py
            if x >= w or y >= h:
                continue
            a = (bits >> (4 * (py * 4 + px))) & 0xF
            out[(y * w + x) * 4 + 3] = a * 17


def decode_dxt(data, off, w, h, fmt):
    out = bytearray(w * h * 4)
    bw, bh = (w + 3) // 4, (h + 3) // 4
    step = 8 if fmt == 0xC else 16
    if off + bw * bh * step > len(data):
        return None
    for by in range(bh):
        for bx in range(bw):
            o = off + (by * bw + bx) * step
            if fmt == 0xC:
                _colour_block(data, o, out, bx * 4, by * 4, w, h, True)
            else:
                _colour_block(data, o + 8, out, bx * 4, by * 4, w, h, False)
                if fmt == 0xF:
                    _dxt5_alpha_block(data, o, out, bx * 4, by * 4, w, h)
                else:
                    _dxt3_alpha_block(data, o, out, bx * 4, by * 4, w, h)
    return bytes(out)


def decode_rgba(data, off, w, h):
    n = w * h * 4
    if off + n > len(data):
        return None
    src = data[off:off + n]
    out = bytearray(n)
    for i in range(0, n, 4):                    # BGRA -> RGBA
        out[i] = src[i + 2]
        out[i + 1] = src[i + 1]
        out[i + 2] = src[i]
        out[i + 3] = src[i + 3]
    return bytes(out)


# ---------------------------------------------------------------- records
def read_name(data, off, limit=64):
    end = data.find(b'\0', off)
    if end < 0 or end - off > limit:
        return None
    try:
        return data[off:end].decode('ascii')
    except UnicodeDecodeError:
        return None


def parse_record(d, rec):
    """Validate + parse one Burnout texture record at file offset `rec`."""
    if rec < 0 or rec + 0x70 > len(d):
        return None
    fmt, = struct.unpack_from('<I', d, rec + 0x34)
    w, = struct.unpack_from('<I', d, rec + 0x38)
    h, = struct.unpack_from('<I', d, rec + 0x3C)
    depth, = struct.unpack_from('<I', d, rec + 0x40)
    if fmt not in FMT_NAMES:
        return None
    if not (0 < w <= 4096 and 0 < h <= 4096):
        return None
    if (w & (w - 1)) or (h & (h - 1)):
        return None
    name = read_name(d, rec + (0x48 if depth in (4, 8, 32) else 0x44))
    if not name or not name.replace('_', '').replace('.', '').isalnum():
        return None
    rel, = struct.unpack_from('<i', d, rec + 0x04)
    data_off = rec + rel
    if not (0 < data_off < len(d)):
        return None
    return {"rec": rec, "fmt": fmt, "w": w, "h": h, "depth": depth,
            "name": name, "data_off": data_off}


def envmap_record(d):
    """The +0xA0 texture record. [C] FUN_00188880 @0x001888BF."""
    off, = struct.unpack_from('<i', d, ENV_TEX_FIELD_ENVMAP)
    return parse_record(d, off) if off else None


# ---------------------------------------------------------------- driver
def track_ids():
    """Every <REGION>/<TRACK> that ships an enviro.dat.  Data driven."""
    out = []
    if not os.path.isdir(TRACKS):
        return out
    for region in sorted(os.listdir(TRACKS)):
        rd = os.path.join(TRACKS, region)
        if not os.path.isdir(rd):
            continue
        for t in sorted(os.listdir(rd)):
            if os.path.exists(os.path.join(rd, t, "enviro.dat")):
                out.append("%s/%s" % (region, t))
    return out


def extract(track, write=True):
    """-> (line, wrote_path_or_None)"""
    path = os.path.join(TRACKS, track, "enviro.dat")
    if not os.path.exists(path):
        return ("%-12s no enviro.dat" % track, None)
    d = open(path, 'rb').read()
    r = envmap_record(d)
    if r is None:
        return ("%-12s enviro.dat +0xA0 EMPTY -- no env map ships for this "
                "track; runtime falls back" % track, None)
    tag = track.replace('/', '_')
    line = ("%-12s +0xA0 %-24s %4dx%-4d %-9s data@0x%06X"
            % (track, r["name"], r["w"], r["h"],
               FMT_NAMES.get(r["fmt"], "0x%X" % r["fmt"]), r["data_off"]))
    if r["fmt"] in (0xC, 0xE, 0xF):
        px = decode_dxt(d, r["data_off"], r["w"], r["h"], r["fmt"])
    elif r["fmt"] == 0x3A:
        px = decode_rgba(d, r["data_off"], r["w"], r["h"])
    else:
        return (line + "   [!] format not decodable here", None)
    if px is None:
        return (line + "   [!] bitmap runs past end of file", None)
    # mean luminance/alpha, so the manifest carries a sanity signal
    n = r["w"] * r["h"]
    lum = sum(px[i] + px[i + 1] + px[i + 2] for i in range(0, n * 4, 4))
    al = sum(px[i + 3] for i in range(0, n * 4, 4))
    line += "  meanRGB=%.1f meanA=%.1f" % (lum / (3.0 * n), al / float(n))
    if not write:
        return (line, None)
    try:
        from PIL import Image
    except ImportError:
        return (line + "   [!] PIL missing, not written", None)
    outdir = os.path.join(OUTROOT, tag)
    os.makedirs(outdir, exist_ok=True)
    out = os.path.join(outdir, "envmap.png")
    # row 0 of the block grid is row 0 of the PNG -- no flip.  V-origin rule.
    Image.frombytes('RGBA', (r["w"], r["h"]), px).save(out)
    return (line + "  -> %s" % os.path.relpath(out, ROOT), out)


def main():
    args = sys.argv[1:]
    write = "--list" not in args
    tracks = ([args[args.index("--track") + 1]] if "--track" in args
              else track_ids())
    lines, n = [], 0
    for t in tracks:
        line, wrote = extract(t, write)
        print(line)
        lines.append(line)
        n += 1 if wrote else 0
    if write and tracks == track_ids():
        os.makedirs(OUTROOT, exist_ok=True)
        with open(os.path.join(OUTROOT, "envmap.txt"), 'w') as f:
            f.write("# car body texture stage 1 substitute: enviro.dat +0xA0\n"
                    "# [C] FUN_00188880 @0x001888BF relocates this slot;\n"
                    "# [S] that it is what DAT_004D6C00 holds -- see\n"
                    "# scratchpad/carenv/INTEGRATION_NOTE.md\n")
            f.write("\n".join(lines) + "\n")
    print("\n%d/%d track(s) have an env map" % (n, len(tracks)))


if __name__ == "__main__":
    main()
