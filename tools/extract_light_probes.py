#!/usr/bin/env python3
"""Extract Burnout 3's per-position car light probes into
build/tracks/<ID>/light_probes.bin.

WHAT THE PROBES ARE  [C]
------------------------
The nine SH coefficients the car body shader consumes (modelInstance+0x5C ->
vertex constants c0..c3, docs/RE_CARFX.md) are rewritten EVERY FRAME, PER CAR,
by `FUN_0019D400` (0x0019D400).  That function

  * takes the car's own world position (carObj+0x40) and casts a segment
    straight DOWN by 20.0 (`PUSH 0x41A00000` @0x001AB136)                 [C]
  * hands it to `FUN_0019D360` -> `FUN_001AF980`, i.e. **the ordinary
    collision BSP of the streamed unit** (the same tree
    tools/extract_collision.py already parses)                            [C]
  * takes the prim record it hit (hit+0x60) plus the sub-triangle flag
    (hit+0x64) and reads THREE u16 probe indices out of the prim:
        sub-triangle 0 -> prim+0x06, prim+0x08, prim+0x0A
        sub-triangle 1 -> prim+0x0A, prim+0x08, prim+0x0C
    (0x0019D4D6..0x0019D525)                                              [C]
  * decodes each with `FUN_0019C640(out float[9], in s8[9])`:
        out[0]    = s8[0] * 1.6 * (1/128)   = s8[0] * 0.0125     (L00)
        out[1..3] = s8[i] * 0.6 * (1/128)   = s8[i] * 0.0046875  (L1)
        out[4..8] = s8[i] * 0.4 * (1/128)   = s8[i] * 0.003125   (L2)
    from `unit+0xA4 + idx*9` (`LEA EDX,[ESI+EAX*8]; ADD EDX,EAX`
    @0x0019D4DA -> stride 9)                                              [C]
  * blends them with the hit's barycentrics (hit+0x54 = u, hit+0x58 = v,
    written by `FUN_001B24A0` @0x001B26C3/0x001B26D7 from `FUN_001B2230`'s
    Moller-Trumbore u/v):
        L[i] = p0[i] + (p1[i]-p0[i])*u + (p2[i]-p0[i])*v                  [C]
  * on a miss the call is skipped entirely (JL @0x001AB128 / the `TEST AL,AL`
    at 0x0019D4B2), so the car KEEPS the previous frame's nine.           [C]

So the "probe volume" is not a grid: it is the collision mesh itself, with a
9-byte quantised SH probe per collision vertex.  The prim record's u16[4] at
+0x06 (which tools/extract_collision.py logs as "extra (unused)") are the four
per-corner probe indices, exactly parallel to its u8 corner indices at +0x00.

CONTAINER  [C, byte-verified on every shipped track]
----------------------------------------------------
Unit LOD block (static.dat unit table at +0x54/+0x58; the block layout is the
one extract_collision.py documents):

    +0xA0  u32 offset of the collision header  (relinked by FUN_0019D7A0)
    +0xA4  u32 offset of the PROBE ARRAY       (relinked at 0x0019D7A8-BB)

The probe array is `probe_count * 9` signed bytes; index space is per unit and
starts at 0.  On every shipped file it sits at block+0xB0, immediately after
the block header, and every decoded L00 byte is positive -- which is what an
irradiance L00 must be, and is the check that the offset is right.

OUTPUT  build/tracks/<ID>/light_probes.bin  (little-endian, raw GAME
coordinates -- the loader in src/burnout3_carfx.c negates Z at query time the
same way trackmesh_load does for geometry):

    +0x00  'B3LP'
    +0x04  u32 version = 1
    +0x08  u32 probe_count          total, units concatenated in unit order
    +0x0C  u32 tri_count
    +0x10  f32 min[3]               probe-mesh bounds, game space
    +0x1C  f32 max[3]
    +0x28  probe_count * 9 s8       raw quantised probes (undecoded)
           pad to 4
           tri_count * { f32 v0[3], v1[3], v2[3]; u32 i0, i1, i2 }

Usage:  python3 tools/extract_light_probes.py [--track <id>] [--all] [--png]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_tlist as tl                                    # noqa: E402

# FUN_0019C640's three band scales, byte-read from .rdata at the VAs below. [C]
DEC_L0 = 1.6 * 0.0078125          # 0x003B16F0 * 0x003B16F4  = 0.0125
DEC_L1 = 0.6 * 0.0078125          # 0x003B16EC * 0x003B16F4  = 0.0046875
DEC_L2 = 0.4 * 0.0078125          # 0x003B16E8 * 0x003B16F4  = 0.003125
PROBE_SCALE = (DEC_L0,) + (DEC_L1,) * 3 + (DEC_L2,) * 5


def parse_unit(blk, unit):
    """(probes, tris) for one unit's LOD block.  tris index probes locally."""
    coll, probe_off = struct.unpack_from("<II", blk, 0xA0)
    if coll == 0 or probe_off == 0:
        return None
    nodes_off, leafs_off = struct.unpack_from("<II", blk, coll + 0x20)
    nleaf, nnode = struct.unpack_from("<HH", blk, coll + 0x28)
    flag = struct.unpack_from("<I", blk, coll + 0x2C)[0]
    if flag != 1:
        raise AssertionError("unit %d: leaf format flag %d" % (unit, flag))

    tris = []
    maxidx = -1
    for i in range(nleaf):
        rec = coll + leafs_off + i * 0x10
        p_rel, v_rel = struct.unpack_from("<ii", blk, rec)
        prim_base = rec + p_rel
        vert_base = rec + v_rel
        cellx, celly, cellz = struct.unpack_from("<3b", blk, rec + 0xA)
        stride = blk[rec + 0xD]
        pcount = blk[rec + 0xE]
        if stride != 0x0E:
            raise AssertionError("unit %d leaf %d: prim stride %#x"
                                 % (unit, i, stride))

        def vert(idx):
            vx, vy, vz = struct.unpack_from("<3H", blk, vert_base + idx * 6)
            return (vx / 65536.0 * 1000.0 + cellx * 500.0,
                    vy / 65536.0 * 1000.0 + celly * 500.0,
                    vz / 65536.0 * 1000.0 + cellz * 500.0)

        for k in range(pcount):
            p = prim_base + k * stride
            i0, i1, i2, i3 = blk[p], blk[p + 1], blk[p + 2], blk[p + 3]
            q = struct.unpack_from("<4H", blk, p + 6)
            maxidx = max(maxidx, q[0], q[1], q[2],
                         q[3] if i3 != 0xFF else 0)
            v0, v1, v2 = vert(i0), vert(i1), vert(i2)
            tris.append((v0, v1, v2, q[0], q[1], q[2]))
            if i3 != 0xFF:
                # FUN_001B2940's quad split, and FUN_0019D400's matching
                # index pick for the second sub-triangle.                [C]
                tris.append((v2, v1, vert(i3), q[2], q[1], q[3]))
    n = maxidx + 1
    probes = blk[probe_off:probe_off + n * 9]
    if len(probes) != n * 9:
        raise AssertionError("unit %d: probe array truncated" % unit)
    return probes, tris, n


def extract(track, want_png=False):
    tdir = track["dir"]
    sd = open(os.path.join(tdir, "static.dat"), "rb").read()
    st = open(os.path.join(tdir, "streamed.dat"), "rb").read()
    unit_count = struct.unpack_from("<H", sd, 0x54)[0]
    unit_table = struct.unpack_from("<i", sd, 0x58)[0]

    all_probes = bytearray()
    all_tris = []
    gmin = [1e30] * 3
    gmax = [-1e30] * 3
    units_with = 0
    for u in range(unit_count):
        so, lo, ss, ls = struct.unpack_from("<iiii", sd, unit_table + u * 0x10)
        if not lo or not ls:
            continue
        got = parse_unit(st[lo:lo + ls], u)
        if got is None:
            continue
        probes, tris, n = got
        base = len(all_probes) // 9
        all_probes += probes
        units_with += 1
        for v0, v1, v2, a, b, c in tris:
            all_tris.append((v0, v1, v2, base + a, base + b, base + c))
            for v in (v0, v1, v2):
                for i in range(3):
                    gmin[i] = min(gmin[i], v[i])
                    gmax[i] = max(gmax[i], v[i])

    nprobe = len(all_probes) // 9
    out = os.path.join(tl.out_root(track), "light_probes.bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(b"B3LP")
        f.write(struct.pack("<III", 1, nprobe, len(all_tris)))
        f.write(struct.pack("<6f", gmin[0], gmin[1], gmin[2],
                            gmax[0], gmax[1], gmax[2]))
        f.write(bytes(all_probes))
        f.write(b"\0" * ((-len(all_probes)) & 3))
        for v0, v1, v2, a, b, c in all_tris:
            f.write(struct.pack("<9f", *v0, *v1, *v2))
            f.write(struct.pack("<3I", a, b, c))

    l00 = [struct.unpack_from("<b", all_probes, i * 9)[0] for i in range(nprobe)]
    neg = sum(1 for v in l00 if v < 0)
    print("[probes] %-9s units %2d/%2d  probes %6d  tris %6d  "
          "L00 %d..%d (neg %d)  -> %s"
          % (track["id"], units_with, unit_count, nprobe, len(all_tris),
             min(l00) if l00 else 0, max(l00) if l00 else 0, neg,
             os.path.relpath(out)))
    if neg:
        print("[probes]   WARNING: %d negative L00 bytes -- offset suspect" % neg)
    if want_png:
        try:
            overlay(track, all_tris, all_probes, gmin, gmax)
        except Exception as e:                                  # noqa: BLE001
            print("[probes]   overlay skipped: %s" % e)
    return nprobe, len(all_tris), neg


def overlay(track, tris, probes, gmin, gmax):
    """Top-down map of the probe L00 -- the visual check that shade darkens."""
    from PIL import Image, ImageDraw
    W = 1400
    x0, x1 = gmin[0] - 20, gmax[0] + 20
    z0, z1 = gmin[2] - 20, gmax[2] + 20
    s = W / max(x1 - x0, z1 - z0)
    H = max(1, int((z1 - z0) * s))
    img = Image.new("RGB", (W, H), (8, 8, 12))
    dr = ImageDraw.Draw(img)
    l00 = [struct.unpack_from("<b", probes, i * 9)[0] * DEC_L0
           for i in range(len(probes) // 9)]
    lo, hi = min(l00), max(l00)
    rng = max(hi - lo, 1e-6)
    for v0, v1, v2, a, b, c in tris:
        e = (l00[a] + l00[b] + l00[c]) / 3.0
        t = (e - lo) / rng
        col = (int(30 + 225 * t), int(30 + 200 * t), int(60 + 120 * t))
        pts = [((v[0] - x0) * s, H - (v[2] - z0) * s) for v in (v0, v1, v2)]
        dr.polygon(pts, fill=col)
    p = os.path.join(tl.out_root(track), "light_probes.png")
    img.save(p)
    print("[probes]   overlay %s  (L00 %.4f .. %.4f)" % (os.path.relpath(p),
                                                         lo, hi))


def main():
    want_png = "--png" in sys.argv
    if "--all" in sys.argv:
        tracks = tl.track_table()
    else:
        spec = None
        for i, a in enumerate(sys.argv):
            if a == "--track" and i + 1 < len(sys.argv):
                spec = sys.argv[i + 1]
            elif a.startswith("--track="):
                spec = a.split("=", 1)[1]
        tracks = [tl.resolve(spec)]
    bad = 0
    for t in tracks:
        if not os.path.isdir(t["dir"]):
            print("[probes] %-9s MISSING %s" % (t["id"], t["dir"]))
            continue
        try:
            _, _, neg = extract(t, want_png)
            bad += 1 if neg else 0
        except Exception as e:                                  # noqa: BLE001
            print("[probes] %-9s FAILED: %s" % (t["id"], e))
            bad += 1
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
