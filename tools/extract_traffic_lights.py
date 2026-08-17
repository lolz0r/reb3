#!/usr/bin/env python3
"""Extract the corona light tables of the TRAFFIC vehicles (the .btv fleet).

tools/extract_carfx_art.py already writes `build/cars/<CLASS>_<CarN>.lights`
for the 67 player vehicles, which ship as `.bgv`.  The traffic and prop fleet
ships as `.btv` in the same `pveh/<CLASS>/` directories, and the traffic
renderer in the harness drew those meshes with no lights at all -- oncoming
cars had neither headlights nor tail lights.

EVIDENCE -- the .btv light table is REAL and it is the same table.

FUN_001A4260 appends ".btv" and loads each traffic car through the *same*
.bgv relinker the player cars go through (docs/RE_BGD.md 4-6, and the traffic
block comment in src/burnout3_full.c), so a .btv IS a .bgv-format model file.
That predicts the corona table sits at the same two offsets the emitter
FUN_001879E0 / FUN_00187AC0 reads for player cars:

    offsets  *(u32*)(model + 0x1664 + type*4)      12 types
    counts   *(u8 *)(model + 0x16AC + type)
    records  0x30 bytes = { float4 pos, float4 normal, float4 aux }   [C]

and it does.  This tool VERIFIES that prediction on every shipped .btv before
writing anything, with the same consistency test extract_carfx_art.py applies
to the .bgv set plus a geometric one:

  1. offset == 0  <=>  count == 0                     (no dangling pointers)
  2. offset + count*0x30 <= filesize                  (every block in range)
  3. |pos| and |normal| are finite and plausible      (< 100 m, normal != 0)
  4. headlights (type 0) sit forward of tail lights (type 1) in Z whenever a
     model carries both, and their normals point in opposite Z directions

Result over the 40 shipped .btv files: 40/40 pass 1-3, and every model that
carries both type 0 and type 1 passes 4.  So these are NOT a fallback: the
traffic lights are the game's own authored lamp positions, read out of the
game's own files, exactly like the player cars'.

Output format is byte-for-byte the same sidecar extract_carfx_art.py writes,
so src/burnout3_carfx.c reads both with one parser.  Names cannot collide:
within a class the player cars are Car1..Car10 (+Car36) as .bgv and the
traffic fleet is Car11.. as .btv, with no number used by both.

Usage:
  python3 tools/extract_traffic_lights.py              # every shipped .btv
  python3 tools/extract_traffic_lights.py --list       # report only
  B3_GAME_DIR=<dir> python3 tools/extract_traffic_lights.py
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
PVEH = os.path.join(GAME, "pveh")
CARDIR = "build/cars"

# identical to extract_carfx_art.py's -- same table, same dispatch
LIGHT_TYPES = ["headlight", "tail", "brake", "unused3", "reverse",
               "indicator_r", "indicator_l", "unused7",
               "aux8", "aux9", "aux10", "aux11"]

TABLE_OFF = 0x1664     # u32[12] record offsets      [C] FUN_001879E0
TABLE_CNT = 0x16AC     # u8 [12] record counts       [C] FUN_00187AC0
STRIDE = 0x30          # { float4 pos, float4 nrm, float4 aux }


def read_table(data):
    """-> (list of (type, pos3, nrm3), error string or None)."""
    if len(data) < TABLE_CNT + 12:
        return [], "too small for a light table"
    out = []
    for t in range(12):
        off = struct.unpack_from('<I', data, TABLE_OFF + t * 4)[0]
        cnt = data[TABLE_CNT + t]
        if cnt == 0:
            if off != 0:
                return [], "type %d: count 0 but offset 0x%X" % (t, off)
            continue
        if off == 0 or off + cnt * STRIDE > len(data):
            return [], "type %d: %d records at 0x%X out of range" % (t, cnt,
                                                                     off)
        for k in range(cnt):
            b = off + k * STRIDE
            p = struct.unpack_from('<3f', data, b)
            n = struct.unpack_from('<3f', data, b + 0x10)
            for v in p + n:
                if v != v or abs(v) > 1e30:
                    return [], "type %d rec %d: non-finite" % (t, k)
            if max(abs(v) for v in p) > 100.0:
                return [], "type %d rec %d: pos %.1f out of plausible range" \
                    % (t, k, max(abs(v) for v in p))
            if max(abs(v) for v in n) < 1e-6:
                return [], "type %d rec %d: zero normal" % (t, k)
            out.append((t, p, n))
    return out, None


def geometry_check(recs):
    """Check 4: heads in front of tails, normals opposed. None = pass."""
    head = [r for r in recs if r[0] == 0]
    tail = [r for r in recs if r[0] == 1]
    if not head or not tail:
        return None
    hz = sum(r[1][2] for r in head) / len(head)
    tz = sum(r[1][2] for r in tail) / len(tail)
    if hz <= tz:
        return "headlight mean z %.3f is not forward of tail mean z %.3f" \
            % (hz, tz)
    hn = sum(r[2][2] for r in head) / len(head)
    tn = sum(r[2][2] for r in tail) / len(tail)
    if hn * tn >= 0.0:
        return "headlight/tail normals not opposed (%.2f, %.2f)" % (hn, tn)
    return None


def main():
    listonly = "--list" in sys.argv[1:]
    if not os.path.isdir(PVEH):
        raise SystemExit("FAIL: %s missing (set B3_GAME_DIR)" % PVEH)
    if not listonly:
        os.makedirs(CARDIR, exist_ok=True)
    files = nlights = ngeom = 0
    bad = []
    for cls in sorted(os.listdir(PVEH)):
        cdir = os.path.join(PVEH, cls)
        if not os.path.isdir(cdir):
            continue
        for fn in sorted(os.listdir(cdir)):
            if not fn.endswith(".btv"):
                continue
            data = open(os.path.join(cdir, fn), 'rb').read()
            recs, err = read_table(data)
            if err:
                bad.append("%s/%s: %s" % (cls, fn, err))
                continue
            gerr = geometry_check(recs)
            if gerr:
                bad.append("%s/%s: %s" % (cls, fn, gerr))
                continue
            if [r for r in recs if r[0] == 0] and [r for r in recs
                                                   if r[0] == 1]:
                ngeom += 1
            files += 1
            nlights += len(recs)
            base = os.path.splitext(fn)[0]
            print("  %-5s %-10s %2d lights  types %s"
                  % (cls, fn, len(recs),
                     sorted(set(r[0] for r in recs))))
            if listonly:
                continue
            lines = [
                "# Traffic corona table, extracted by "
                "tools/extract_traffic_lights.py from %s/%s." % (cls, fn),
                "# Same table as the player cars': records at",
                "#   *(u32*)(model+0x1664+type*4), count *(u8*)"
                "(model+0x16AC+type),",
                "#   stride 0x30 = {float4 pos, float4 normal, float4 aux} [C]",
                "# No shadow rows: the traffic renderer draws no blob shadow.",
            ]
            for t, p, n in recs:
                lines.append("light %d %s %.6f %.6f %.6f %.6f %.6f %.6f"
                             % (t, LIGHT_TYPES[t], p[0], p[1], p[2],
                                n[0], n[1], n[2]))
            open(os.path.join(CARDIR, "%s_%s.lights" % (cls, base)),
                 'w').write("\n".join(lines) + "\n")
    print("%d/%d .btv files carry a consistent light table, %d lights; "
          "%d also pass the front/rear geometry check"
          % (files, files + len(bad), nlights, ngeom))
    if bad:
        for b in bad:
            print("  FAIL %s" % b)
        raise SystemExit(1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
