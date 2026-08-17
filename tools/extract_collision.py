#!/usr/bin/env python3
"""Extract Burnout 3's own collision world for C1_V1 into build/collision.bin.

Where the collision lives [C]:
  Each streamed.dat unit's LOD block (static.dat unit table at +0x54/+0x58,
  entry {sub_off, lod_off, sub_size, lod_size}) is a self-contained record:
    +0x00 u32 state (1 on disk; the streamer stamps 2 when resident)
    +0x04 u32 unit index
    +0x08 u32 block size
    +0x50 {u32 1, u32 offA, u32 0, u32 offB}   (render far-LOD sections)
    +0x70 f32[12]  four XZ half-planes bounding the unit (FUN_0019d7f0 tests)
    +0xA0 u32 offset of the COLLISION HEADER   (relinked by FUN_0019d7a0)
    +0xA4 u32 offset of a second section       (relinked, not collision)

  Collision header (relinked by FUN_001b02b0), offsets header-relative:
    +0x00 f32[3] bbox max        +0x10 f32[3] bbox min
    +0x20 u32 kd-node array offset (0x10-stride nodes:
          {f32 split, u16 index, u8 axis, u8 leaf_flag(0xFF=internal)} x2)
    +0x24 u32 leaf array offset  +0x28 u16 leaf count  +0x2A u16 node count
    +0x2C u32 format flag (1 = quantized-vertex leaves on every retail unit)

  Leaf record (0x10 bytes; FUN_001b02b0 relinks +0x00/+0x04 self-relative):
    +0x00 i32 prim data offset (relative to this record)
    +0x04 i32 vertex data offset (relative to this record)
    +0x08 u16 ?                  +0x0A s8[3] cell offset (x,y,z)
    +0x0D u8 prim stride (0x0E)  +0x0E u8 prim count  +0x0F u8 vertex count

  Vertex: u16[3], stride 6.  world = u16/65536*1000 + cell*500  (FUN_001b0f00)

  Prim record (stride 0x0E):
    +0x00 u8 i0,i1,i2,i3 (i3==0xFF -> triangle, else quad)
    +0x04 u16 surface type
    +0x06 u16[4] extra (unused by the wheel/body query chain)
  Quad decomposition (FUN_001b2940, execution-verified): tri1=(i0,i1,i2),
  tri2=(i2,i1,i3); normal = normalize(cross(v1-v0, v2-v0)).

  The per-frame gather callback 0x00109ce0 SKIPS prims whose surface-type low
  byte is 0x20/0x22/0x23/0x24 -- those never reach the vehicle soup at
  veh+0x200 (they are flagged here instead of dropped).

Verification: tools/validate_gameplay.py 'collision' section replays the
game's own walker FUN_001aff70 under Unicorn and diffs its enumerated
triangles against this parser (exact-set match), then diffs the game's ray
query FUN_00123790 against the reimplementation.

Output build/collision.bin (little-endian, raw GAME coordinates -- the
harness loader applies the same Z-negation as trackmesh_load):
  +0x00 magic 'B3CL'  +0x04 u32 version=1  +0x08 u32 tri_count
  +0x0C u32 unit_count  +0x10 f32 min[3]  +0x1C f32 max[3]
  +0x28 tri[tri_count]: {f32 v0[3],v1[3],v2[3]; u16 type; u8 unit; u8 flags}
        flags bit0 = excluded from the vehicle soup by the gather callback
"""
import os
import struct
import sys

# Track selection: --track <id|dir|name> / B3_TRACK (ids from
# tools/extract_tlist.py), or the legacy B3_TRACK_DIR override.  Nothing in
# the parser below is track-specific -- every offset comes out of the files.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_tlist as tl                                    # noqa: E402

_spec = None
for _i, _a in enumerate(sys.argv):
    if _a == "--track" and _i + 1 < len(sys.argv):
        _spec = sys.argv[_i + 1]
    elif _a.startswith("--track="):
        _spec = _a.split("=", 1)[1]
TRACK = tl.resolve(_spec)
TRACK_DIR = os.environ.get("B3_TRACK_DIR", TRACK['dir'])
OUT = os.path.join(tl.out_root(TRACK), "collision.bin")
LEGACY_OUT = os.path.join(os.path.dirname(__file__), "..", "build",
                          "collision.bin")
OVERLAY = os.path.join(os.path.dirname(__file__), "..", "build",
                       "collision_overlay.png")

# Surface-type low bytes the gather callback 0x00109ce0 refuses to append.
EXCLUDED_LOW = {0x20, 0x22, 0x23, 0x24}


def parse_unit_collision(blk, unit):
    """Triangles {v0,v1,v2,type,flags} from one unit's LOD block. [C]"""
    hdr = struct.unpack_from("<I", blk, 0xA0)[0]
    if hdr == 0:
        return None
    bmax = struct.unpack_from("<3f", blk, hdr)
    bmin = struct.unpack_from("<3f", blk, hdr + 0x10)
    nodes_off, leafs_off = struct.unpack_from("<II", blk, hdr + 0x20)
    nleaf, nnode = struct.unpack_from("<HH", blk, hdr + 0x28)
    flag = struct.unpack_from("<I", blk, hdr + 0x2C)[0]
    assert flag == 1, "unit %d: unexpected leaf format flag %d" % (unit, flag)
    tris = []
    for i in range(nleaf):
        rec = hdr + leafs_off + i * 0x10
        p_rel, v_rel = struct.unpack_from("<ii", blk, rec)
        prim_base = rec + p_rel
        vert_base = rec + v_rel
        cellx, celly, cellz = struct.unpack_from("<3b", blk, rec + 0xA)
        stride = blk[rec + 0xD]
        pcount = blk[rec + 0xE]
        assert stride == 0x0E, "unit %d leaf %d: prim stride %#x" % (
            unit, i, stride)

        def vert(idx):
            vx, vy, vz = struct.unpack_from("<3H", blk, vert_base + idx * 6)
            return (vx / 65536.0 * 1000.0 + cellx * 500.0,
                    vy / 65536.0 * 1000.0 + celly * 500.0,
                    vz / 65536.0 * 1000.0 + cellz * 500.0)

        for k in range(pcount):
            p = prim_base + k * stride
            i0, i1, i2, i3 = blk[p], blk[p + 1], blk[p + 2], blk[p + 3]
            styp = struct.unpack_from("<H", blk, p + 4)[0]
            flags = 1 if (styp & 0xFF) in EXCLUDED_LOW else 0
            v0, v1, v2 = vert(i0), vert(i1), vert(i2)
            tris.append((v0, v1, v2, styp, flags))
            if i3 != 0xFF:
                tris.append((v2, v1, vert(i3), styp, flags))
    return dict(bmax=bmax, bmin=bmin, nleaf=nleaf, nnode=nnode, tris=tris)


def normal_of(t):
    v0, v1, v2 = t[0], t[1], t[2]
    ux, uy, uz = (v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2])
    vx, vy, vz = (v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2])
    n = (uy*vz - uz*vy, uz*vx - ux*vz, ux*vy - uy*vx)
    l = (n[0]*n[0] + n[1]*n[1] + n[2]*n[2]) ** 0.5
    return (0.0, 0.0, 0.0) if l < 1e-12 else (n[0]/l, n[1]/l, n[2]/l)


def route_bounds():
    """XZ bounds of the recovered route (GL space, z negated) -> game space."""
    path = os.path.join(os.path.dirname(__file__), "..", "src",
                        "burnout3_track_paths.h")
    if not os.path.exists(path):
        return None
    import re
    pts = re.findall(r"\{\s*(-?[\d.]+)f?\s*,\s*(-?[\d.]+)f?\s*,\s*(-?[\d.]+)f?",
                     open(path).read())
    if not pts:
        return None
    # the header stores raw game coordinates (init_paths negates z at load)
    xs = [float(a) for a, b, c in pts]
    zs = [float(c) for a, b, c in pts]
    return (min(xs), max(xs), min(zs), max(zs))


def main():
    print("[extract_collision] track %s = %s (%s)"
          % (TRACK['id'], TRACK['name'], TRACK_DIR))
    sd = open(os.path.join(TRACK_DIR, "static.dat"), "rb").read()
    st = open(os.path.join(TRACK_DIR, "streamed.dat"), "rb").read()
    unit_count = struct.unpack_from("<H", sd, 0x54)[0]
    unit_table = struct.unpack_from("<i", sd, 0x58)[0]

    all_tris = []            # (v0,v1,v2,type,unit,flags)
    per_unit = []
    gmin = [1e30] * 3
    gmax = [-1e30] * 3
    for u in range(unit_count):
        so, lo, ss, ls = struct.unpack_from("<iiii", sd, unit_table + u * 0x10)
        if not lo or not ls:
            per_unit.append(0)
            continue
        m = parse_unit_collision(st[lo:lo + ls], u)
        if m is None:
            per_unit.append(0)
            continue
        per_unit.append(len(m["tris"]))
        for v0, v1, v2, styp, fl in m["tris"]:
            all_tris.append((v0, v1, v2, styp, u, fl))
            for v in (v0, v1, v2):
                for i in range(3):
                    gmin[i] = min(gmin[i], v[i])
                    gmax[i] = max(gmax[i], v[i])

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(b"B3CL")
        f.write(struct.pack("<III", 1, len(all_tris), unit_count))
        f.write(struct.pack("<6f", gmin[0], gmin[1], gmin[2],
                            gmax[0], gmax[1], gmax[2]))
        for v0, v1, v2, styp, u, fl in all_tris:
            f.write(struct.pack("<9f", *v0, *v1, *v2))
            f.write(struct.pack("<HBB", styp, u, fl))

    # ---- validation report ----------------------------------------------
    n_units = sum(1 for c in per_unit if c)
    road = sum(1 for t in all_tris if normal_of(t)[1] >= 0.45 and not t[5])
    wall = sum(1 for t in all_tris if abs(normal_of(t)[1]) < 0.45 and not t[5])
    excl = sum(1 for t in all_tris if t[5])
    print("[extract_collision] units with collision: %d/%d" %
          (n_units, unit_count))
    print("[extract_collision] triangles: %d  (road-like %d, wall-like %d, "
          "gather-excluded %d)" % (len(all_tris), road, wall, excl))
    print("[extract_collision] bounds  min (%.1f, %.1f, %.1f)  "
          "max (%.1f, %.1f, %.1f)" % (*gmin, *gmax))
    ok = n_units == unit_count and len(all_tris) > 0
    rb = route_bounds()
    if rb:
        contained = (gmin[0] <= rb[0] and gmax[0] >= rb[1] and
                     gmin[2] <= rb[2] and gmax[2] >= rb[3])
        print("[extract_collision] route XZ bounds x[%.0f..%.0f] z[%.0f..%.0f]"
              " %s collision bounds" %
              (rb[0], rb[1], rb[2], rb[3],
               "CONTAINED IN" if contained else "NOT CONTAINED IN (FAIL)"))
        ok = ok and contained
    print("[extract_collision] wrote %s (%d bytes)" %
          (OUT, os.path.getsize(OUT)))
    import shutil
    shutil.copyfile(OUT, LEGACY_OUT)
    print("[extract_collision] installed %s" % os.path.normpath(LEGACY_OUT))

    # ---- overlay PNG: collision vs render mesh --------------------------
    try:
        overlay(all_tris, gmin, gmax)
    except Exception as e:                                  # noqa: BLE001
        print("[extract_collision] overlay skipped: %s" % e)

    if not ok:
        sys.exit(1)


def overlay(all_tris, gmin, gmax):
    from PIL import Image, ImageDraw
    W = 1600
    x0, x1 = gmin[0] - 30, gmax[0] + 30
    z0, z1 = gmin[2] - 30, gmax[2] + 30
    s = W / max(x1 - x0, z1 - z0)
    H = int((z1 - z0) * s)

    def pt(x, z):
        return (x - x0) * s, H - (z - z0) * s

    img = Image.new("RGB", (W, H), (16, 16, 20))
    dr = ImageDraw.Draw(img)

    # render mesh (game space: track.obj is raw game coords) in grey
    objp = os.path.join(os.path.dirname(__file__), "..", "build", "track.obj")
    if os.path.exists(objp):
        vs = []
        for line in open(objp):
            if line.startswith("v "):
                _, a, b, c = line.split()[:4]
                vs.append((float(a), float(c)))
            elif line.startswith("f "):
                idx = [int(w.split("/")[0]) - 1 for w in line.split()[1:4]]
                p = [pt(*vs[i]) for i in idx]
                dr.polygon(p, outline=(70, 70, 78))
    # collision: road green, wall red, excluded orange
    for v0, v1, v2, styp, u, fl in all_tris:
        ny = normal_of((v0, v1, v2))[1]
        col = ((255, 150, 40) if fl else
               (60, 200, 90) if ny >= 0.45 else (230, 70, 70))
        dr.polygon([pt(v0[0], v0[2]), pt(v1[0], v1[2]), pt(v2[0], v2[2])],
                   outline=col)
    img.save(OVERLAY)
    print("[extract_collision] overlay -> %s" % OVERLAY)


if __name__ == "__main__":
    main()
