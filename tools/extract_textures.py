#!/usr/bin/env python3
"""
Extract Burnout 3 Xbox textures from static.dat (or any file with the same
texture table) and write them as PNGs.

Format credit: Burnout Modding community (burnout.wiki) via EdnessP's Noesis
plugin. Texture record layout, all little-endian:

  +0x04  i32  bitmap data offset, RELATIVE to the record
  +0x34  u32  format: 0xB paletted, 0xC DXT1, 0xE DXT3, 0xF DXT5, 0x3A RGBA
  +0x38  u32  width
  +0x3C  u32  height
  +0x40  u32  bit depth -- if 4/8/32 this is the "old" revision and the name
              lives at +0x48; otherwise it is the newer revision, name at +0x44
              and the real bit depth at +0x64

static.dat holds the texture count at +0x16 (u16) and a pointer array at +0x18
for version > 0x25.

Only DXT1 and DXT5 are implemented, which covers every texture in the shipped
track files (96 DXT1 + 84 DXT5 in C1_V1). Unhandled formats are reported rather
than silently skipped.

============================================ THE V ORIGIN RULE (per format) ==

  For every texture format the game ships in a track, v = 0 is texel ROW 0 of
  the surface, i.e. the TOP of the picture, and this tool writes PNG row 0 =
  surface row 0. The rule is NOT format-dependent, and no consumer may flip.

Why that is the whole rule -- the record is a prebaked Xbox D3DPixelContainer
------------------------------------------------------------------------
The first 0x14 bytes of the texture record are not engine bookkeeping: they
are the Xbox D3D texture object itself, baked by the build tool and handed
to the GPU untouched.

  +0x00  u32  D3DResource.Common   (refcount/type; 0x00040001 for a 2D tex)
  +0x04  u32  D3DPixelContainer.Data   = the bitmap offset, relocated to a
              pointer at load -- the SAME field this tool reads at +0x04
  +0x08  u32  D3DPixelContainer.Lock
  +0x0C  u32  D3DPixelContainer.Format = the NV2A TX_FORMAT register word
  +0x10  u32  D3DPixelContainer.Size   = 0 for every track texture

Proof that it is that object [C]: the deferred render-state flusher
FUN_001D7040 ends with the four texture stages,

    001D7100  MOV  EAX,[ESI*4 + 0x0075db70]   ; the slot the material apply
    001D7107  CMP  [ESI*4 + 0x0041aa34],EAX   ;   writes (FUN_0003A3C0 /
    001D7112  MOV  [ESI*4 + 0x0041aa34],EAX   ;   FUN_000393C0, mat+0x0C)
    001D7119  CALL 0x0034dca0                 ; = D3DDevice_SetTexture

and FUN_0034dca0(stage, tex) is D3DDevice_SetTexture verbatim: it bumps
tex[0] (Common) by 0x80000, then pushes NV2A method (stage + 0x206C)*0x40 =
0x1B00 + stage*0x40 (TX_OFFSET) with tex[1] (+0x04 Data) and tex[3] (+0x0C
Format), and only if tex[4] (+0x10 Size) is non-zero does it also push
0x41B1C (TX_IMAGE_RECT) -- the linear-surface pitch path.

TX_FORMAT decodes exactly as the record's own descriptive fields [C]:

    bits 0-1   context DMA          bits 16-19  mipmap levels   = +0x68
    bit  2     cubemap              bits 20-23  log2 width      = +0x38
    bit  3     border source        bits 24-27  log2 height     = +0x3C
    bits 4-7   dimensionality (2)   bits 28-31  log2 depth (0)
    bits 8-15  COLOR = the D3DFMT code, identical to +0x34

e.g. GL_Droad_grass (US_C3_V1 record 0): Format 0x08890C29 -> DMA 1, 2D,
COLOR 0x0C = X_D3DFMT_DXT1, 9 mips, 2^8 x 2^8 = 256x256, depth 1 -- matching
+0x34 = 0x0C, +0x38/+0x3C = 256, +0x68 = 9. Checked over EVERY texture of ALL
36 shipped tracks: 9,224 records, 0 mismatches (scratchpad pixelcontainer.py).

Two consequences:

  1. The shipped bytes at +0x04 ARE the GPU surface. Nothing repacks, re-rows
     or flips them at load; the loader only turns the relative Data offset
     into a pointer. So "row 0 of the file" == "row 0 of the surface".
  2. NV2A TX_FORMAT has no origin, flip or mirror field -- the only per-stage
     addressing controls are TX_WRAP/TX_CONTROL, set from the material's
     texture-state blocks (0x4D6564 / 0x4D6568 / 0x4D65A0). D3D/NV2A sampling
     is fixed: v=0 is the first texel row. There is nothing per-format left
     to differ.

Which formats each track actually uses                                   [C]
------------------------------------------------------------------------
All 36 shipped tracks, 9,224 texture records: 4,045 DXT1 (0x0C) + 5,179 DXT5
(0x0F) and NOTHING else -- no paletted, no 32-bit, and in particular ZERO
LIN_* surfaces, so the swizzled-vs-linear distinction never arises on the
track path at all. US_C3_V1 (SILVER LAKE) = 85 DXT1 + 92 DXT5; AS_C1_V1
(GOLDEN CITY) = 96 DXT1 + 84 DXT5. The two tracks' texture sets are the same
formats in the same orientation; there was never a per-track or per-format
difference to find. (The one genuinely different family in the game is the
CAR paint texture: 0x0B = X_D3DFMT_P8, Morton/Z-order swizzled, decoded by
tools/extract_bgv_textures.py. Unswizzling restores the same row-0-first
linear surface, so it obeys the same rule -- swizzle changes the byte
ADDRESSING inside a surface, never which row is v=0.)

Corroboration from the geometry alone, without the binary               [S]
------------------------------------------------------------------------
Over near-vertical faces (|n.y| <= 0.3), v DECREASES as world height rises --
v=0 sits at the TOP of a wall -- in every track measured. Area-weighted share
of "v=0 at the top": US_C3_V1 95.7%, AS_C1_V1 71.9%, US_C1_V1 88.4%,
EU_C3_V1 81.3%, AS_C2_V1 71.8%. (The remainder is tiling clutter and rotated
atlas panels, where dv/dY carries no orientation meaning.) Since these PNGs
read right-side up on disk, PNG-top == v=0 == wall-top is the authored intent.

What this rule cost before it was recovered
------------------------------------------------------------------------
tools/extract_track.py used to write `vt u (1-v)` -- the reflex D3D->GL flip,
which is wrong precisely because glTexImage2D puts t=0 on the FIRST row of
the data it is given, the same row D3D calls v=0. Every sign in the world
rendered upside down (build/dump_sl2.png). It was never track-specific:
AS_C1_V1's bk_newfwysigns1 and bk_warnsigna render "Main Centre" / "Dockside"
/ "SLOW DOWN" flipped through the old OBJ as well -- GOLDEN CITY had simply
never been checked against legible signage. Fixed in extract_track.py by
emitting v verbatim; this tool's PNGs are unchanged and stay human-readable.
"""
import os
import struct
import sys

FMT_NAMES = {0xB: "Paletted", 0xC: "DXT1", 0xE: "DXT3", 0xF: "DXT5", 0x3A: "RGBA"}


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
               ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255),
               ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255)]
    else:
        # DXT1 with c0 <= c1: third colour is a 50% blend, fourth is transparent
        pal = [(r0, g0, b0, 255), (r1, g1, b1, 255),
               ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255),
               (0, 0, 0, 0)]
    for py in range(4):
        for px in range(4):
            x, y = ox + px, oy + py
            if x >= w or y >= h:
                continue
            out[(y * w + x) * 4: (y * w + x) * 4 + 4] = bytes(
                pal[(bits >> (2 * (py * 4 + px))) & 3])


def dxt5_alpha_block(data, o, out, ox, oy, w, h):
    a0, a1 = data[o], data[o + 1]
    # Standard DXT5 alpha interpolation. The weights are (6-i)/(i+1) over 7 and
    # (4-i)/(i+1) over 5 -- using 7-i / 5-i overflows past 255.
    if a0 > a1:
        al = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
    else:
        al = [a0, a1] + [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255]
    bits = int.from_bytes(data[o + 2:o + 8], 'little')
    for py in range(4):
        for px in range(4):
            x, y = ox + px, oy + py
            if x >= w or y >= h:
                continue
            out[(y * w + x) * 4 + 3] = al[(bits >> (3 * (py * 4 + px))) & 7]


def decode_dxt(data, off, w, h, fmt):
    out = bytearray(w * h * 4)
    bw, bh = (w + 3) // 4, (h + 3) // 4
    step = 8 if fmt == 0xC else 16
    need = bw * bh * step
    if off + need > len(data):
        return None
    for by in range(bh):
        for bx in range(bw):
            o = off + (by * bw + bx) * step
            if fmt == 0xC:
                dxt_colour_block(data, o, out, bx * 4, by * 4, w, h, True)
            else:  # DXT5: 8 bytes alpha then 8 bytes colour
                dxt_colour_block(data, o + 8, out, bx * 4, by * 4, w, h, False)
                dxt5_alpha_block(data, o, out, bx * 4, by * 4, w, h)
    return bytes(out)


def read_name(data, off):
    end = data.find(b'\0', off)
    if end < 0 or end - off > 64:
        return None
    try:
        return data[off:end].decode('ascii')
    except UnicodeDecodeError:
        return None


def extract(path, outdir, limit=None):
    from PIL import Image
    data = open(path, 'rb').read()
    ver = struct.unpack_from('<I', data, 0)[0]
    if ver > 0x25:
        count = struct.unpack_from('<H', data, 0x16)[0]
        table = struct.unpack_from('<I', data, 0x18)[0]
    else:
        count = struct.unpack_from('<I', data, 0x14)[0]
        table = struct.unpack_from('<I', data, 0x18)[0]

    os.makedirs(outdir, exist_ok=True)
    written, skipped = 0, {}
    for i in range(count if limit is None else min(count, limit)):
        rec = struct.unpack_from('<I', data, table + i * 4)[0]
        if not (0 < rec < len(data) - 0x70):
            skipped["bad record ptr"] = skipped.get("bad record ptr", 0) + 1
            continue
        bmp = struct.unpack_from('<i', data, rec + 4)[0]
        bmp = bmp + rec if bmp else 0
        fmt, w, h, bd = struct.unpack_from('<4I', data, rec + 0x34)
        name = read_name(data, rec + (0x48 if bd in (4, 8, 32) else 0x44))
        if not name:
            name = "tex_%03d" % i
        name = os.path.basename(name.replace('\\', '/')) or ("tex_%03d" % i)

        if fmt not in (0xC, 0xF):
            key = "unhandled fmt 0x%X (%s)" % (fmt, FMT_NAMES.get(fmt, "?"))
            skipped[key] = skipped.get(key, 0) + 1
            continue
        if not (0 < w <= 4096 and 0 < h <= 4096) or not (0 < bmp < len(data)):
            skipped["bad dims/offset"] = skipped.get("bad dims/offset", 0) + 1
            continue

        rgba = decode_dxt(data, bmp, w, h, fmt)
        if rgba is None:
            skipped["data past EOF"] = skipped.get("data past EOF", 0) + 1
            continue
        Image.frombytes("RGBA", (w, h), rgba).save(
            os.path.join(outdir, "%s.png" % name))
        written += 1
    return written, skipped, count


def main():
    # --track <id|dir|name> / B3_TRACK (tools/extract_tlist.py), or the
    # legacy positional <static.dat> [outdir].
    spec = None
    for i, a in enumerate(sys.argv):
        if a == "--track" and i + 1 < len(sys.argv):
            spec = sys.argv[i + 1]
        elif a.startswith("--track="):
            spec = a.split("=", 1)[1]
    pos = [a for a in sys.argv[1:] if not a.startswith("--") and a != spec]
    if pos:
        src, out = pos[0], (pos[1] if len(pos) > 1 else "build/textures")
        track = None
    else:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import extract_tlist as tl
        track = tl.resolve(spec)
        src = os.path.join(track['dir'], "static.dat")
        out = tl.out_root(track, "textures")
        print("track %s = %s" % (track['id'], track['name']))

    written, skipped, count = extract(src, out)
    if track is not None:
        import shutil
        legacy = os.path.join(os.path.dirname(__file__), "..", "build",
                              "textures")
        if os.path.isdir(legacy):
            shutil.rmtree(legacy)
        shutil.copytree(out, legacy)
        print("installed %s" % os.path.normpath(legacy))
    print("texture entries : %d" % count)
    print("written         : %d PNGs -> %s" % (written, out))
    if skipped:
        for k, v in sorted(skipped.items()):
            print("  skipped %-32s %d" % (k, v))
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
