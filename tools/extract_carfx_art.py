#!/usr/bin/env python3
"""Extract the art the car-FX module needs: the shadow blob, the light corona,
and the per-car light/shadow geometry tables out of the .bgv files.

Everything here is named by the retail executable -- no asset was picked by
eye. See docs/RE_CARFX.md for the full evidence chain; the short version:

  "blobbyshadow"  the car shadow texture.  FUN_00043350 (0x00043350) looks it
                  up by name through FUN_0002DDF0; the string lives at VA
                  0x003AAFF8 and is that function's only xref.            [C]
  "coronaglow"    the light corona sprite.  FUN_0017EE00 loads the three
                  corona textures from the name table at VA 0x003A3E7C
                  ("coronaglow", "coronaboost", "coronaboostred"); the car
                  light emitter FUN_00187BE0 hard-codes pool 0 = the first
                  entry = coronaglow (ADD ECX,0xC at 0x00187C28).         [C]

Both live in Data/Global.txd, decoded with the repo's existing (unmodified)
container/format readers -- tools/extract_txd.py's layout, tools/
extract_textures.py's DXT decoder, tools/extract_bgv_textures.py's
unswizzler.  This tool only selects, decodes and re-emits them under
build/carfx/, so the car-FX module has a stable, small art directory that
does not depend on another agent's output naming.

It also emits, per player vehicle, `build/cars/<CLASS>_<CarN>.lights`:

  * the corona light table read by FUN_001879E0/FUN_00187AC0 from
    `*(u32*)(model + 0x1664 + type*4)` (offsets) and
    `*(u8*)(model + 0x16AC + type)` (counts), records of 0x30 bytes:
        +0x00 float4  position, model space, w = 1
        +0x10 float4  outward normal,       w = 1
        +0x20 float4  (0, a, b, 0)                              [? meaning]
    Verified on all 67 player .bgv files: offset != 0 <=> count != 0, every
    block in range, and on COMP/Car1 the front types face +Z at z = +1.77
    while every rear type faces -Z at z = -1.8.                          [C]

  * the shadow quad's geometry sources, read by FUN_0019A7C0.  The quad is a
    four-row triangle strip; the two OUTER rows come from the bounding box
    and the two INNER rows from the axles, which is what gives the blob its
    soft nose/tail caps and its stretchable middle:
        half width   = *(float*)(model + 0xE80)          (+/- x extent)
        outer front Z= *(float*)(model + 0xE88)   bbox front   (V = 0)
        inner front Z= *(float*)(model + 0xBF8)   == wheel[1].pos.z  (V=.1875)
        inner rear Z = *(float*)(model + 0xB38 + numWheels*0x40)
                                                 == wheel[nw-2].pos.z(V=.8125)
        outer rear Z = *(float*)(model + 0xE98)   bbox rear    (V = 1)
    (0xB38 = 0xB80 - 0x48, i.e. the wheel-matrix array's Pos-row Z field;
    numWheels is the .bgv header byte at +0x0D.  The +0xBF8 / +0xB38 values
    reach FUN_00043570 through the two float4s at [esp+0x24]/[esp+0x34], the
    bbox Z's through the register copies made BEFORE those two stores --
    0x0019A8EE / 0x0019A8E3 load xmm2/xmm1, 0x0019A917 / 0x0019A904 then
    overwrite only the memory.)                                          [C]

Usage:
  python3 tools/extract_carfx_art.py            # textures + all 67 cars
  python3 tools/extract_carfx_art.py --textures-only
Exits non-zero if any required asset is missing or fails to decode.
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_textures import decode_dxt                      # noqa: E402
from extract_bgv_textures import unswizzle8                  # noqa: E402

GAME = (game_root())
GLOBAL_TXD = os.path.join(GAME, "Data", "Global.txd")
PVEH = os.path.join(GAME, "pveh")
TRACKS = os.path.join(GAME, "Tracks")
OUTDIR = "build/carfx"
CARDIR = "build/cars"

# names taken from the executable, not chosen here (see the docstring)
WANTED = ("blobbyshadow", "coronaglow")

TXD_MAGIC = (0x543C0000, 0xBCDEED81)
PAL_MAGIC = (b'\x01\x00\x03\x00', b'\x01\x00\x03\xc0')

# The 12 corona light types.  Names come from the emitter FUN_00187C70's
# bit->type dispatch plus the geometry itself (front/rear, +Z/-Z normals).
LIGHT_TYPES = ["headlight", "tail", "brake", "unused3", "reverse",
               "indicator_r", "indicator_l", "unused7",
               "aux8", "aux9", "aux10", "aux11"]


def _read_name(data, off):
    end = data.find(b'\0', off)
    if end < 0 or end - off > 64:
        return None
    try:
        s = data[off:end].decode('ascii')
    except UnicodeDecodeError:
        return None
    return s if s and all(32 <= ord(c) < 127 for c in s) else None


def decode_record(data, rec):
    """One Criterion texture record -> (name, w, h, RGBA bytes)."""
    bmp = rec + struct.unpack_from('<i', data, rec + 0x04)[0]
    fmt, w, h = struct.unpack_from('<3I', data, rec + 0x34)
    bd = struct.unpack_from('<I', data, rec + 0x40)[0]
    name = _read_name(data, rec + (0x48 if bd in (4, 8, 32) else 0x44))
    if name is None:
        return None
    if fmt in (0xC, 0xE, 0xF):
        rgba = decode_dxt(data, bmp, w, h, fmt)
    elif fmt == 0xB:
        idx = unswizzle8(data[bmp:bmp + w * h], w, h)
        prec = rec + struct.unpack_from('<I', data, rec + 0x14)[0]
        if data[prec:prec + 4] not in PAL_MAGIC:
            return None
        pdata = rec + struct.unpack_from('<I', data, prec + 4)[0]
        ncol = 16 if bd == 4 else 256
        pal = data[pdata:pdata + ncol * 4]
        out = bytearray(w * h * 4)
        for i, pi in enumerate(idx):
            p, o = pi * 4, i * 4
            out[o], out[o + 1], out[o + 2], out[o + 3] = \
                pal[p + 2], pal[p + 1], pal[p], pal[p + 3]
        rgba = bytes(out)
    else:
        return None
    return name, w, h, rgba


def extract_textures():
    from PIL import Image
    data = open(GLOBAL_TXD, 'rb').read()
    m1, m2, count, esz = struct.unpack_from('<4I', data, 0)
    if (m1, m2) != TXD_MAGIC or esz != 16:
        raise SystemExit("%s: not a Burnout 3 txd container" % GLOBAL_TXD)
    os.makedirs(OUTDIR, exist_ok=True)
    got = {}
    for i in range(count):
        _, _, off, _ = struct.unpack_from('<4I', data, 0x10 + i * 16)
        if not (0x10 < off < len(data) - 0x80):
            continue
        r = decode_record(data, off)
        if r is None:
            continue
        name, w, h, rgba = r
        if name in WANTED:
            im = Image.frombytes('RGBA', (w, h), rgba)
            im.save(os.path.join(OUTDIR, name + ".png"))
            a = im.getchannel('A')
            got[name] = (w, h, a.getextrema())
    for n in WANTED:
        if n not in got:
            raise SystemExit("FAIL: %s not found in %s" % (n, GLOBAL_TXD))
        w, h, ext = got[n]
        print("  %-14s %dx%d  alpha range %s -> %s/%s.png"
              % (n, w, h, ext, OUTDIR, n))
    return got


def extract_lights():
    """Per-car corona table + shadow quad sources -> build/cars/*.lights."""
    if not os.path.isdir(PVEH):
        raise SystemExit("FAIL: %s missing" % PVEH)
    os.makedirs(CARDIR, exist_ok=True)
    n_cars = n_lights = 0
    for cls in sorted(os.listdir(PVEH)):
        cdir = os.path.join(PVEH, cls)
        if not os.path.isdir(cdir):
            continue
        for fn in sorted(os.listdir(cdir)):
            if not (fn.startswith("Car") and fn.endswith(".bgv")):
                continue
            d = open(os.path.join(cdir, fn), 'rb').read()
            u32 = lambda o: struct.unpack_from('<I', d, o)[0]   # noqa: E731
            f32 = lambda o: struct.unpack_from('<f', d, o)[0]   # noqa: E731
            nw = d[0x0D]
            lines = [
                "# Car-FX tables, extracted by tools/extract_carfx_art.py.",
                "# corona light table: FUN_001879E0 reads records at",
                "#   *(u32*)(model+0x1664+type*4), count *(u8*)(model+0x16AC+type),",
                "#   stride 0x30 = {float4 pos, float4 normal, float4 aux} [C]",
                "# shadow quad sources: FUN_0019A7C0 [C]  (4 strip rows)",
                "#   halfwidth = model+0xE80.x",
                "#   outerfrontz = model+0xE88   (V 0)",
                "#   frontz      = model+0xBF8   (V 0.1875)",
                "#   rearz       = model+0xB38+numWheels*0x40 (V 0.8125)",
                "#   outerrearz  = model+0xE98   (V 1)",
                "numwheels %d" % nw,
                "halfwidth %.6f" % f32(0xE80),
                "outerfrontz %.6f" % f32(0xE88),
                "frontz %.6f" % f32(0xBF8),
                "rearz %.6f" % f32(0xB38 + nw * 0x40),
                "outerrearz %.6f" % f32(0xE98),
            ]
            ok = True
            for t in range(12):
                off, cnt = u32(0x1664 + t * 4), d[0x16AC + t]
                if cnt == 0:
                    if off != 0:
                        ok = False
                    continue
                if off == 0 or off + cnt * 0x30 > len(d):
                    ok = False
                    continue
                for k in range(cnt):
                    b = off + k * 0x30
                    px, py, pz = struct.unpack_from('<3f', d, b)
                    nx, ny, nz = struct.unpack_from('<3f', d, b + 0x10)
                    lines.append("light %d %s %.6f %.6f %.6f %.6f %.6f %.6f"
                                 % (t, LIGHT_TYPES[t], px, py, pz,
                                    nx, ny, nz))
                    n_lights += 1
            if not ok:
                raise SystemExit("FAIL: %s/%s inconsistent light table"
                                 % (cls, fn))
            base = os.path.splitext(fn)[0]
            out = os.path.join(CARDIR, "%s_%s.lights" % (cls, base))
            open(out, 'w').write("\n".join(lines) + "\n")
            n_cars += 1
    print("  %d cars, %d corona lights -> %s/*.lights"
          % (n_cars, n_lights, CARDIR))
    return n_cars, n_lights


# ---------------------------------------------------------------- enviro sun
# Pixel-shader constant c14.xyz (DAT_0060E0A0) is the environment object's
# +0x60, and FUN_001888F0 (0x001888F0) overwrites that object with the first
# 0xB0 bytes of the track's "enviro.dat" (the literal at 0x003B0444) via
# FUN_00188C00, called at 0x00188A40.  So the car's light colour is a byte
# field in a shipped file; this dumps it for every track so the value in
# src/burnout3_carfx.c can be checked without a hex editor.             [C]
ENV_LIGHT_OFF = 0x60


def extract_env_light():
    if not os.path.isdir(TRACKS):
        print("[carfx-art] Tracks/ not found, skipping enviro sun table")
        return 0
    rows = []
    for reg in sorted(os.listdir(TRACKS)):
        rd = os.path.join(TRACKS, reg)
        if not os.path.isdir(rd):
            continue
        for trk in sorted(os.listdir(rd)):
            fp = os.path.join(rd, trk, "enviro.dat")
            if not os.path.isfile(fp):
                continue
            with open(fp, "rb") as f:
                f.seek(ENV_LIGHT_OFF)
                rgb = struct.unpack("<3f", f.read(12))
            rows.append((reg + "_" + trk, rgb))
    if not rows:
        return 0
    os.makedirs(OUTDIR, exist_ok=True)
    out = os.path.join(OUTDIR, "env_light.txt")
    with open(out, "w") as f:
        f.write("# ps c14.xyz per track = enviro.dat +0x%02X (float3)\n"
                "# track        r        g        b        (8-bit)\n"
                % ENV_LIGHT_OFF)
        for name, rgb in rows:
            f.write("%-10s %.9g %.9g %.9g   %d,%d,%d\n"
                    % (name, rgb[0], rgb[1], rgb[2],
                       round(rgb[0] * 255), round(rgb[1] * 255),
                       round(rgb[2] * 255)))
    print("[carfx-art] %s  (%d tracks)" % (out, len(rows)))
    return len(rows)


def main():
    print("[carfx-art] textures from %s" % GLOBAL_TXD)
    extract_textures()
    if "--textures-only" not in sys.argv:
        print("[carfx-art] per-car tables from %s" % PVEH)
        extract_lights()
        print("[carfx-art] enviro sun colours from %s" % TRACKS)
        extract_env_light()
    print("[carfx-art] OK")


if __name__ == "__main__":
    main()
