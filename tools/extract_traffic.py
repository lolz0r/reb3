#!/usr/bin/env python3
'''Extract a track's race-mode TRAFFIC data + traffic vehicle assets.

Track-agnostic: `--track US_C3_V1` / `B3_TRACK=...` (ids from
tools/extract_tlist.py).  Outputs:

    src/burnout3_traffic_data.h        (compiled-in, legacy shape)
    build/tracks/<ID>/traffic.bin      (runtime-loadable, format below)
    build/tracks/<ID>/cars/*.obj,*.png (+ mirrored into build/cars/)

Where the data comes from (the .bgd walk lives in extract_bgd_paths.py):

  * MODE BLOCK ("TDESC"): the event's own `{size = param+0x3C4,
    offset = param+0x3C8}` (`mov eax,[eax+0x3c8]` @0x0018B569, `+0x3c4`
    @0x0018B5BB), i.e. per EVENT, not "block 0 of a size table at 0x660" --
    that table tiles on 1 of the 36 shipped files.                       [C]
  * TRAFFIC SET: six independent `{ptr, count}` lists at TDESC +0x54/+0x58,
    +0x60/+0x64, +0x6C/+0x70, +0x78/+0x7C, +0x84/+0x88, +0x90/+0x94, each
    of 0x18-byte records with a base-40 packed vehicle id at +0x00.  The
    six pointers are exactly the ones the block relocator FUN_00158B70
    fixes up (`*(ESI+0x54) += base;` ...), and the counts sit next to them.
    221/221 mode blocks across the 36 files parse cleanly.               [C]
    Runtime consumer: FUN_001A13F0 walks these records and hands each to
    FUN_001A4260, which appends ".btv" and loads the vehicle through the
    same relinker (FUN_000310F0) as .bgv.                                [C]
  * SPECIAL TRAFFIC: five inline u64 ids at TDESC +0x00/+0x08/+0x10/+0x18/
    +0x20, gated bit-by-bit by the flag byte at TDESC+0xB5.              [C]
    (The old "TSPCCARn at block+0x18" was the AS/C1_V1 bit of that gate.)
  * SPAWN/ENTRY TABLE: `{ptr = TDESC+0xAC, count = TDESC+0xB0}`, stride
    0x20, `{pos[3], w, dir[3], w}`.  Location [C] (it reproduces AS/C1_V1's
    71 records @0x1C450 exactly); the "spawn point" READING is still [S] --
    no consumer of the table has been located.
  * ONCOMING LINE: the drive line that hugs the road corridor, chosen and
    oriented by extract_bgd_paths.analyse() (see its docstring).  Ascending
    index runs AGAINST the race direction.                               [S]

Z IS NEGATED on every emitted point/direction (harness GL space,
RE_NOTES 12).

traffic.bin (little-endian):
  +0x00 char[4] 'B3TR'  +0x04 u32 version = 4
  +0x08 u32 car_count   +0x0C u32 spawn_count  +0x10 u32 oncoming_count
  +0x14 u32 special_count (cars[car_count-special_count:] are the specials)
  +0x18 u32 lane_count
  +0x1C car_count x { char id[16]; char cls[8]; char car[16];
                      i32 cat; i32 kingpin_spring; f32 tow_anchor[3];
                      f32 king_anchor[3] }                    (NUL padded)
        spawn_count x f32[6]   (pos xyz, dir xyz)
        oncoming_count x f32[3]
        lane_count x { f32 lat; i32 dir }

traffic_paths.bin (little-endian):
  +0x00 char[4] 'B3TP'  +0x04 u32 version = 3
  +0x08 u32 point_count +0x0C u32 path_count
  +0x10 u32 pool_window_count +0x14 u32 pool_request_count
        point_count x f32[3] (GL space)
        path_count x { u32 row_count; row_count x {u16 point_a, u16 point_b};
                       row_count x {f32 distance, f32 width};
                       row_count x u8[0x12] branch rows }
        pool_window_count x { u32 first_progress, u32 last_progress,
                               u32 request_base, u8 request_count,
                               u8 refresh_count, u16 pad }
        pool_request_count x { u16 first_row, u16 last_row,
                               u8 path_id, u8 direction }
'''
import argparse
import importlib.util
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_tlist as tl                                   # noqa: E402
import extract_bgd_paths as bp                               # noqa: E402

CARS_OUT = os.path.join(os.path.dirname(__file__), "..", "build", "cars")
_BGV_TOOL = None


# --------------------------------------------------------------------------
def load_tool(name):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(os.path.dirname(__file__), name + ".py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def extract_btv(xbgv, path, max_len=60.0):
    """extract_bgv.extract() with the player-car length gate widened for the
    long TSPC specials.  Same section choice + vertex layout otherwise."""
    data = open(path, 'rb').read()

    def u(o):
        return struct.unpack_from('<I', data, o)[0]

    ver = u(0)
    if not (0x14 <= ver <= 0x25):
        return None, "version 0x%X unsupported" % ver
    best = None
    for li in range(5):
        sec = xbgv.parse_section(data, u(0x4C + li * 4))
        if sec is None:
            continue
        ntris = sum(len(t) for _, _, t in sec['body'])
        if best is None or ntris > best[0]:
            best = (ntris, sec)
    if best is None:
        return None, "no valid LOD section"
    sec = best[1]
    verts = xbgv.read_verts(data, sec['pool'], sec['maxidx'] + 1)
    zs = [v[2] for v in verts]
    if not (0.5 < max(zs) - min(zs) < max_len):
        return None, "implausible length %.1f" % (max(zs) - min(zs))
    # [C deep-traced, tools/trace_panels.py --deep on HEVY/Car24.btv -- a
    # .btv traces identically to .bgv]: the intact car on screen is ONE
    # draw of the embedded part object (S+0x60, mask 0x3FF), and with
    # rec+0x10 decoded as the index COUNT (not size/2) that object is
    # panel-complete.  Glass records are dropped (traffic is drawn opaque
    # by the harness).
    intact = [("body_m%X" % m, t) for m, tx, t in sec['body']
              if (m & 0x300) == 0]
    if not intact:
        intact = [("m%X" % m, t) for m, tx, t in sec['body']]
    return (verts, intact, sec, data), None


def export_assets(cars, outdir=None):
    """cars: [(id, cls, carbase)] -> OBJ + paint PNGs per car.

    Written to build/tracks/<ID>/cars/ and mirrored into build/cars/ (the
    path the harness's load_car_meshes expects)."""
    dirs = [d for d in (outdir, CARS_OUT) if d]
    xbgv = load_tool("extract_bgv")
    xtex = load_tool("extract_bgv_textures")
    for d in dirs:
        os.makedirs(d, exist_ok=True)
    got_mesh = got_tex = 0
    for cid, cls, car in cars:
        src = os.path.join(xbgv.PVEH, cls, car + ".btv")
        name = "%s_%s" % (cls, car)
        res, err = extract_btv(xbgv, src)
        if err:
            print("  mesh FAIL %-12s %s" % (name, err))
        else:
            verts, groups, sec, data = res
            for dd in dirs:
                xbgv.write_obj(os.path.join(dd, name + ".obj"), verts,
                               groups, "traffic vehicle, intact record set")
            # Wheels: the .btv loads through the SAME relinker as .bgv
            # (FUN_001A4260 -> FUN_000310F0 [C]), so the wheel part slots
            # (7 slow / 8,9 blur, FUN_000303D0) and the attach matrices at
            # +0xB80 / radius at +0x18 are the same records extract_bgv
            # reads.  Their omission left traffic wheel-less and resting
            # on the body skirt (debug dump 023).
            best, fb, fb_size = None, None, -1
            for slot in (7, 8, 9):
                recs = sec['slots'].get(slot)
                if not recs:
                    continue
                size = sum(len(t) for _, _, t in recs)
                if slot == 7 and best is None:
                    best = recs
                if size > fb_size:
                    fb, fb_size = recs, size
            if best is None:
                best = fb
            if best:
                wgroups = [("m%X" % m, t) for m, tx, t in best]
                for dd in dirs:
                    xbgv.write_obj(os.path.join(dd, name + "_wheel.obj"),
                                   verts, wgroups,
                                   "traffic wheel (slot 7 slow, 8/9 blur "
                                   "fallback), same records as .bgv")
            radius, wheels = xbgv.read_wheels(data)
            for dd in dirs:
                with open(os.path.join(dd, name + ".wheels"), 'w') as f:
                    f.write("# wheel radius (file+0x18) + attach matrices"
                            " (file+0xB80) [C],\n# same layout as .bgv"
                            " (shared relinker FUN_000310F0)\n")
                    f.write("radius %.4f\n" % radius)
                    for pos, mirror in wheels:
                        f.write("wheel %.4f %.4f %.4f %d\n"
                                % (pos[0], pos[1], pos[2], mirror))
            got_mesh += 1
        tex, terr = xtex.extract_textures(src)
        if terr:
            print("  tex  FAIL %-12s %s" % (name, terr))
        else:
            _, images = tex
            for k, img in enumerate(images):
                for dd in dirs:
                    img.save(os.path.join(dd, "%s_p%d.png" % (name, k)))
            got_tex += 1
    print("traffic assets: %d/%d meshes, %d/%d paint sets -> %s"
          % (got_mesh, len(cars), got_tex, len(cars),
             ', '.join(os.path.normpath(d) for d in dirs)))


# --------------------------------------------------------------------------
def car_files(cid):
    """`COMPCAR14` -> ("COMP", "Car14"); pveh/<cls>/<car>.btv."""
    assert cid[4:7] == 'CAR', cid
    return cid[:4], "Car%s" % cid[7:]


# ------------------------------------------------------------ vehicle classes
# The six {ptr,count} lists are six vehicle CATEGORY POOLS, not one flat set.
# Evidence (all 36 shipped Gamedata.bgd, 221 mode blocks, 4130 records):
#   slot 0 (+0x54) COMPCAR11..19          small cars
#   slot 1 (+0x60) HEVYCAR11..16,31,33..35 vans/pickups/SUVs + rigid coach
#   slot 2 (+0x6C) HEVYCAR19,20,21        buses
#   slot 3 (+0x78) HEVYCAR17,18,32        medium trucks
#   slot 4 (+0x84) HEVYCAR22,23           SEMI TRACTORS -- EVERY event that
#                                         carries trailers has EXACTLY ONE
#                                         entry here (241x{1,2} 96x{1,3}
#                                         22x{1,4} 18x{1,1}, never 0 or 2)
#   slot 5 (+0x90) HEVYCAR24,25,26,27,29,30  TRAILERS -- and these six ids are
#                                         precisely the .btv models with NO
#                                         FRONT AXLE (attach matrices at
#                                         +0xB80 hold only a rear bogie at
#                                         z=-2.92/-4.22), i.e. bodies that
#                                         cannot stand up on their own.
# So the tractor<->trailer pairing is NOT a field inside the 0x18 record: it
# is the LIST SLOT the record lives in.  An articulated unit = the event's
# single slot-4 tractor + one slot-5 trailer.                            [C]
CAT_TRACTOR = 4
CAT_TRAILER = 5
CAT_SPECIAL = 6
CAT_NAME = ("compact", "light", "bus", "truck", "tractor", "trailer",
            "special")


def axle_zs(cls, car):
    """Rear/front attach-matrix Z of a .btv, from the exported .wheels."""
    path = os.path.join(CARS_OUT, "%s_%s.wheels" % (cls, car))
    zs = []
    try:
        for ln in open(path):
            f = ln.split()
            if len(f) == 5 and f[0] == 'wheel':
                zs.append(float(f[3]))
    except OSError:
        return []
    return sorted(set(round(z, 3) for z in zs))


def mesh_z_range(cls, car):
    path = os.path.join(CARS_OUT, "%s_%s.obj" % (cls, car))
    lo, hi = 1e9, -1e9
    try:
        for ln in open(path):
            if ln.startswith('v '):
                z = float(ln.split()[3])
                lo = min(lo, z)
                hi = max(hi, z)
    except OSError:
        return None
    return None if lo > hi else (lo, hi)


def hitch_anchor(cls, car, ptr_off, count_off):
    """First relinked model attach point at {ptr_off,count_off}. [C]"""
    global _BGV_TOOL
    if _BGV_TOOL is None:
        _BGV_TOOL = load_tool("extract_bgv")
    path = os.path.join(_BGV_TOOL.PVEH, cls, car + ".btv")
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    if count_off >= len(data) or data[count_off] == 0:
        return None
    ptr = struct.unpack_from("<I", data, ptr_off)[0]
    if ptr == 0 or ptr + 16 > len(data):
        return None
    return struct.unpack_from("<3f", data, ptr)


def hitch_count(cls, car, count_off):
    global _BGV_TOOL
    if _BGV_TOOL is None:
        _BGV_TOOL = load_tool("extract_bgv")
    path = os.path.join(_BGV_TOOL.PVEH, cls, car + ".btv")
    try:
        data = open(path, "rb").read()
    except OSError:
        return 0
    return data[count_off] if count_off < len(data) else 0


def hitch_geometry(cls, car, cat):
    """(tow_anchor, king_anchor, kingpin_spring) from retail groups. [C]

    The trailer is the articulated master: when its `model+0x16BC` count is
    one, it supplies its kingpin from `+0x16A4`; the tractor partner supplies
    its fifth-wheel anchor from `+0x16A8` (count `+0x16BD`).  These are raw
    mesh-space coordinates, matching the wheel/axle values used below.
    """
    if cat == CAT_TRACTOR:
        anchor = hitch_anchor(cls, car, 0x16A8, 0x16BD)
        if anchor is None:
            raise ValueError("%s/%s has no tractor fifth-wheel anchor" %
                             (cls, car))
        return anchor, (0.0, 0.0, 0.0), 0
    if cat == CAT_TRAILER:
        count = hitch_count(cls, car, 0x16BC)
        ptr_off = 0x16A4 if count == 1 else 0x16A8
        count_off = 0x16BC if count == 1 else 0x16BD
        anchor = hitch_anchor(cls, car, ptr_off, count_off)
        if anchor is None:
            raise ValueError("%s/%s has no selected trailer anchor" %
                             (cls, car))
        return (0.0, 0.0, 0.0), anchor, int(count == 1)
    return (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), 0


# ------------------------------------------------------------------- lanes
def lane_table(spawns, line):
    """Traffic LANES recovered from the spawn/entry table. [S]

    Each spawn record is projected onto the traffic route polyline: the
    signed lateral offset says which lane the entry sits in and the record's
    own `dir` says which way that lane runs.  On US_C3_V1/OFFSGRCF the 32
    entries collapse onto exactly four lanes -- +2.6 / +9.1 (running AGAINST
    the polyline's ascending index) and +14.8 / +21.3 (running WITH it) --
    plus 4 slip-road outliers, i.e. a four-lane divided road.

    The polyline itself sits at lateral 0, so it is the road EDGE / median
    line and NOT a driving lane: every lane is 2.6 .. 21.4 m to one side of
    it.  Driving traffic along the bare polyline (what the harness used to
    do) puts every car outside the outermost lane.

    Returns [(lat, dir)] with dir = +1 driven in ascending index order,
    -1 in descending order.
    """
    n = len(line)
    if n < 8 or not spawns:
        return []

    def proj(x, z):
        best, bt = 1e30, None
        for i in range(n):
            p, q = line[i], line[(i + 1) % n]
            ax, az = q[0] - p[0], q[2] - p[2]
            l2 = ax * ax + az * az
            t = ((x - p[0]) * ax + (z - p[2]) * az) / l2 if l2 > 1e-9 else 0.0
            t = max(0.0, min(1.0, t))
            cx, cz = p[0] + ax * t, p[2] + az * t
            d2 = (x - cx) ** 2 + (z - cz) ** 2
            if d2 < best:
                best, bt = d2, (cx, cz, ax, az)
        cx, cz, tx, tz = bt
        L = math.hypot(tx, tz) or 1.0
        tx, tz = tx / L, tz / L
        s = tx * (z - cz) - tz * (x - cx)
        return (math.sqrt(best) if s >= 0 else -math.sqrt(best)), tx, tz

    hits = []
    for s in spawns:
        lat, tx, tz = proj(s[0], s[2])
        dot = s[3] * tx + s[5] * tz
        if abs(dot) < 0.85:          # slip roads / junction mouths: not lanes
            continue
        # The spawn record's dir[3] is the instance matrix's Z COLUMN = the
        # car's BACKWARD axis [C, AI-DRIVE wave]: the sign was inverted,
        # which labelled the racers' own lanes "oncoming" (rivals met 50 mph
        # head-on traffic in their lanes -- the user's wrong-side report).
        hits.append((lat, -1 if dot > 0 else 1))
    # cluster on lateral offset (lanes are ~6 m apart, entries scatter ~1 m)
    lanes = []
    for lat, d in sorted(hits):
        if lanes and abs(lanes[-1][0][0] - lat) < 3.0 and lanes[-1][1] == d:
            lanes[-1][0].append(lat)
        else:
            lanes.append(([lat], d))
    out = [(sum(v) / len(v), d) for v, d in lanes if len(v) >= 2]
    return out


def pad(s, n):
    b = s.encode('ascii')[:n - 1]
    return b + b'\0' * (n - len(b))


def write_traffic_bin(path, cars, nspecial, spawns, line, cats, lanes):
    with open(path, 'wb') as f:
        f.write(b'B3TR')
        f.write(struct.pack('<IIIIII', 4, len(cars), len(spawns), len(line),
                            nspecial, len(lanes)))
        for (cid, cls, car), cat in zip(cars, cats):
            tow, king, spring = hitch_geometry(cls, car, cat)
            f.write(pad(cid, 16) + pad(cls, 8) + pad(car, 16)
                    + struct.pack('<ii6f', cat, spring, *tow, *king))
        for s in spawns:
            f.write(struct.pack('<6f', *s))
        for p in line:
            f.write(struct.pack('<3f', *p))
        for l, d in lanes:
            f.write(struct.pack('<fi', l, d))
    return path


def traffic_mix(td):
    """The retail spawn policy's own tables, straight out of the TDESC. [C]

    Three parallel pieces, all previously unread by this extractor:

    * per-class MODEL lists -- FUN_001A5E30's jump table (@0x001A5F10) maps the
      class code onto the six TDESC list offsets, each `{ptr, count, total}`;
      inside a list the model is drawn with `rng %% total` against the running
      sum of each 0x18-byte record's u32 weight at +0x10, and its paint with
      `rng %% 100` against the eight percentage bytes at +0x08..+0x0F
      (FUN_001A5F90).
    * (path,row) -> (manager record, slot) BINDINGS -- TDESC+0x3C/+0x40 rows,
      inverted exactly as FUN_001A13F0 @0x001A1BF3 does, and searched by
      FUN_0019E5B0 ("largest start_row <= row").
    * per-(record,slot) SPEED and per-class RATE tables -- schedule row 0's
      stage-2 (4-byte mph) and stage-3 (0x1C-byte, seven floats) tables.
      Row 0 is the one whose trigger (+0x50) is 0, i.e. the state the manager
      installs before the race starts; rows 1..n are progress-keyed updates
      that stay [?].
    """
    entries, classes = [], []
    for lst in td['list_records']:
        base = len(entries)
        for rec in lst['records']:
            entries.append((rec['id'], rec['weight'], rec['colours']))
        classes.append((lst['cls'], base, len(lst['records']), lst['total']))
    bindings = []
    for index, rec in enumerate(td['records']):
        for slot, (path_id, start_row) in enumerate(rec['slots']):
            bindings.append((path_id, index, slot, start_row))
    bindings.sort()
    speeds, rates = {}, {}
    for row in td['schedules']:
        if row['trigger'] or row['trigger_hi']:
            break                      # progress-keyed update, not the seed
        for record, slot, mph in row['speeds']:
            speeds[(record, slot)] = mph
        for record, slot, rate in row['rates']:
            rates[(record, slot)] = rate
        break                          # only row 0 is the initial state
    table = []
    for key in sorted(set(list(speeds) + list(rates))):
        table.append((key[0], key[1], speeds.get(key, 0.0),
                      rates.get(key, [0.0] * 7)[:6]))
    return classes, entries, bindings, table


def write_traffic_paths_bin(path, bgd, paths, pool_windows, mix=None):
    """Write RIDX paths plus FUN_001A28B0's TDESC pool-window requests. [C]"""
    request_count = sum(len(window['requests']) for window in pool_windows)
    with open(path, 'wb') as f:
        f.write(struct.pack('<4sIIIII', b'B3TP', 4, paths['point_count'],
                            len(paths['paths']), len(pool_windows),
                            request_count))
        for point_id in range(paths['point_count']):
            f.write(struct.pack('<3f', *bp.gl(bp.f3(bgd.d,
                                                   paths['points'] + point_id * 16))))
        for route in paths['paths']:
            f.write(struct.pack('<I', len(route['pairs'])))
            for pair in route['pairs']:
                f.write(struct.pack('<HH', *pair))
            for distance in route['distances']:
                f.write(struct.pack('<ff', *distance))
            f.write(route['aux'])
        request_base = 0
        for window in pool_windows:
            requests = window['requests']
            f.write(struct.pack('<IIIBBH', window['first_progress'],
                                window['last_progress'], request_base,
                                len(requests), window['refresh_count'], 0))
            request_base += len(requests)
        for window in pool_windows:
            for request in window['requests']:
                f.write(struct.pack('<HHBB', request['first_row'],
                                    request['last_row'], request['path_id'],
                                    request['direction']))
        classes, entries, bindings, table = mix or ([], [], [], [])
        f.write(struct.pack('<IIII', len(classes), len(entries),
                            len(bindings), len(table)))
        for cls, base, count, total in classes:
            f.write(struct.pack('<IIII', cls, base, count, total))
        for cid, weight, colours in entries:
            f.write(pad(cid, 16) + struct.pack('<I', weight)
                    + bytes(bytearray(colours[:8]))
                    + b'\0' * (8 - len(colours[:8])) + struct.pack('<I', 0))
        for path_id, record, slot, start_row in bindings:
            f.write(struct.pack('<BBBBI', path_id, record, slot, 0, start_row))
        for record, slot, mph, rate in table:
            f.write(struct.pack('<BBHf6f', record, slot, 0, mph, *rate))
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    tl.add_track_arg(ap)
    ap.add_argument("--event", default=bp.DEFAULT_EVENT)
    ap.add_argument("--header", default=None)
    ap.add_argument("--no-assets", action="store_true",
                    help="skip the .btv mesh/texture export")
    args = ap.parse_args()

    track = tl.resolve(args.track)
    a = bp.analyse(track, args.event)
    bgd, ev = a['bgd'], a['ev']
    td = bgd.tdesc(ev)
    traffic_paths = bgd.traffic_paths(ev)

    ids, cats, seen = [], [], set()
    for li, lst in enumerate(td['lists']):
        for cid in lst:
            if cid not in seen and cid[4:7] == 'CAR':
                seen.add(cid)
                ids.append(cid)
                cats.append(li)
    nspecial = 0
    for cid in td['specials']:
        if cid not in seen and cid[4:7] == 'CAR':
            seen.add(cid)
            ids.append(cid)
            cats.append(CAT_SPECIAL)
            nspecial += 1
    assert ids, "no traffic vehicle ids in the event's mode block"
    cars = [(cid,) + car_files(cid) for cid in ids]

    spawns = [(p[0], p[1], -p[2], p[3], p[4], -p[5]) for p in td['spawns']]
    onc = a['oncoming']
    line = [bp.gl(p) for p in onc['pts']] if onc else []
    lanes = lane_table(spawns, line)

    print("  TDESC @%#x+%#x: lists %s, specials %s, spawn table @%#x x%d"
          % (td['base'], td['size'], [len(x) for x in td['lists']],
             td['specials'] or '-', td['spawn_off'], len(spawns)))
    print("  traffic set: %d vehicles (%d special); oncoming line @%#x x%d"
          % (len(cars), nspecial, onc['off'] if onc else 0, len(line)))
    print("  categories: %s"
          % ", ".join("%s=%s" % (CAT_NAME[c], i)
                      for i, c in zip(ids, cats)))
    print("  lanes recovered from the spawn table: %s"
          % ", ".join("%+.2f m %s" % (l, "asc" if d > 0 else "desc")
                      for l, d in lanes))

    hdr = args.header or os.path.join(os.path.dirname(__file__), "..", "src",
                                      "burnout3_traffic_data.h")
    with open(hdr, 'w') as fh:
        w = fh.write
        w("// GENERATED by tools/extract_traffic.py -- DO NOT EDIT\n")
        w("// %s = %s, event %s.  Located through the game's own .bgd\n"
          % (track['id'], track['name'], ev['id']))
        w("// parser: mode block (TDESC) from param+0x3C4/+0x3C8 [C]; the six\n")
        w("//   {ptr,count} traffic lists at TDESC +0x54/+0x60/+0x6C/+0x78/\n")
        w("//   +0x84/+0x90 (the pointers FUN_00158B70 relocates) [C];\n")
        w("//   specials at TDESC+0x00.. gated by the byte at TDESC+0xB5 [C];\n")
        w("//   spawn/entry table {ptr TDESC+0xAC, count TDESC+0xB0} --\n")
        w("//   location [C], 'spawn point' semantics [S] (no consumer found);\n")
        w("//   oncoming line: the corridor-hugging drive line, oriented so\n")
        w("//   ascending index runs AGAINST the race direction [S].\n")
        w("// Runtime chain FUN_001A13F0 -> FUN_001A4260 -> \".btv\" [C].\n")
        w("// Z NEGATED on all points/directions (RE_NOTES 12).\n\n")
        w("#ifndef BURNOUT3_TRAFFIC_DATA_H\n#define BURNOUT3_TRAFFIC_DATA_H\n\n")
        w("// The six lists are six vehicle CATEGORY pools, not one flat\n")
        w("// set (all 36 .bgd, 221 mode blocks, 4130 records):\n")
        w("//   0 compact  1 light  2 bus  3 truck  4 TRACTOR  5 TRAILER\n")
        w("// Slot 5 holds exactly HEVYCAR24/25/26/27/29/30 -- precisely the\n")
        w("// .btv models with NO FRONT AXLE (rear bogie only) -- and every\n")
        w("// event carrying trailers has EXACTLY ONE slot-4 tractor.  The\n")
        w("// tractor<->trailer pairing is the LIST SLOT, not a record\n")
        w("// field: an articulated unit = the slot-4 tractor + a slot-5\n")
        w("// trailer.                                                  [C]\n")
        w("#define B3_TRAFFIC_CAT_COMPACT 0\n")
        w("#define B3_TRAFFIC_CAT_LIGHT   1\n")
        w("#define B3_TRAFFIC_CAT_BUS     2\n")
        w("#define B3_TRAFFIC_CAT_TRUCK   3\n")
        w("#define B3_TRAFFIC_CAT_TRACTOR 4\n")
        w("#define B3_TRAFFIC_CAT_TRAILER 5\n")
        w("#define B3_TRAFFIC_CAT_SPECIAL 6\n\n")
        w("typedef struct {\n")
        w("    const char* id;    // vlist/base-40 id (matches B3_CAR_PHYSICS)\n")
        w("    const char* cls;   // pveh/<class>/\n")
        w("    const char* car;   // file base, <car>.btv; mesh "
          "build/cars/<cls>_<car>.obj\n")
        w("    int   cat;         // B3_TRAFFIC_CAT_*, = the source list slot\n")
        w("    int   kingpin_spring; // trailer +0x16BC == 1 branch [C]\n")
        w("    float tow_anchor[3];  // tractor model+0x16A8 fifth wheel [C]\n")
        w("    float king_anchor[3]; // trailer model+0x16A4 kingpin    [C]\n")
        w("} B3TrafficCarId;\n\n")
        w("// Race-mode traffic set (last %d entries are the gated specials).\n"
          % nspecial)
        w("#define B3_TRAFFIC_CAR_COUNT %d\n" % len(cars))
        w("static const B3TrafficCarId B3_TRAFFIC_CARS[B3_TRAFFIC_CAR_COUNT]"
          " = {\n")
        for (cid, cls, car), cat in zip(cars, cats):
            tow, king, spring = hitch_geometry(cls, car, cat)
            w('    { "%s", "%s", "%s", %d, %d, {%.4ff, %.4ff, %.4ff}, '
              '{%.4ff, %.4ff, %.4ff} },  // %s\n'
              % (cid, cls, car, cat, spring, *tow, *king, CAT_NAME[cat]))
        w("};\n\n")
        w("// TRAFFIC LANES, recovered by projecting the spawn/entry table\n")
        w("// onto the route polyline: `lat` = signed lateral offset (m),\n")
        w("// `dir` = +1 driven in ascending index order, -1 descending.\n")
        w("// The polyline itself is at lat 0 and is the road EDGE/median\n")
        w("// line, NOT a lane -- every lane sits 2.6..21.4 m to one side\n")
        w("// of it, which is why driving cars ON the polyline put them off\n")
        w("// the road.                                                 [S]\n")
        w("#define B3_TRAFFIC_LANE_COUNT %d\n" % len(lanes))
        w("static const struct { float lat; int dir; } "
          "B3_TRAFFIC_LANES[B3_TRAFFIC_LANE_COUNT + 1] = {\n")
        for l, dd in lanes:
            w("    {%.3ff, %d},\n" % (l, dd))
        w("    {0.0f, 0},\n")
        w("};\n\n")
        w("// Spawn/entry points with headings (x,y,z, dx,dy,dz).\n")
        w("#define B3_TRAFFIC_SPAWN_COUNT %d\n" % len(spawns))
        w("static const float B3_TRAFFIC_SPAWN[B3_TRAFFIC_SPAWN_COUNT][6]"
          " = {\n")
        for s in spawns:
            w("    {%.3ff, %.3ff, %.3ff, %.5ff, %.5ff, %.5ff},\n" % s)
        w("};\n\n")
        w("// Oncoming drive line, closed loop, AGAINST the race direction.\n")
        w("#define B3_ONCOMING_COUNT %d\n" % len(line))
        w("static const float B3_ONCOMING[B3_ONCOMING_COUNT][3] = {\n")
        for p in line:
            w("    {%.3ff, %.3ff, %.3ff},\n" % p)
        w("};\n\n")
        w("#endif // BURNOUT3_TRAFFIC_DATA_H\n")
    print("wrote %s: %d cars, %d spawns, %d line points"
          % (os.path.normpath(hdr), len(cars), len(spawns), len(line)))

    write_traffic_bin(os.path.join(tl.out_root(track), "traffic.bin"),
                      cars, nspecial, spawns, line, cats, lanes)
    print("wrote %s" % os.path.join(tl.out_root(track), "traffic.bin"))
    mix = traffic_mix(td)
    write_traffic_paths_bin(os.path.join(tl.out_root(track), "traffic_paths.bin"),
                            bgd, traffic_paths, td['pool_windows'], mix)
    print("wrote %s: %d paths, %d points, %d pool windows"
          % (os.path.join(tl.out_root(track), "traffic_paths.bin"),
             len(traffic_paths['paths']), traffic_paths['point_count'],
             len(td['pool_windows'])))
    print("  spawn policy: %d class lists / %d model entries, %d path->record "
          "bindings, %d (record,slot) speed+rate rows"
          % (len(mix[0]), len(mix[1]), len(mix[2]), len(mix[3])))
    for cls, base, count, total in mix[0]:
        if not count:
            continue
        print("    class %2d: total %d  %s" % (cls, total,
              ", ".join("%s w%d" % (mix[1][base + k][0], mix[1][base + k][1])
                        for k in range(count))))

    if not args.no_assets:
        export_assets(cars, tl.out_root(track, "cars"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
