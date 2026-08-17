#!/usr/bin/env python3
"""Scan Gamedata.bgd for world-space polylines (race route / AI line / grid).

Method: the .bgd is a baked memory image (base 0x00320000 -- internal pointers
all fall in [base, base+filesize)). Rather than reverse the full layout, find
float-triple arrays whose points sit inside the known track bounds
(from build/track.obj: X[-857,2384] Y[-15,233] Z[-1474,2281]) and whose
consecutive spacing looks like a path. Every candidate is then plotted over the
track mesh top-down for visual ground truth -- a run that traces the roads is a
real path; scattered noise is not.
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import struct, sys, os

BGD = (game_path('Tracks/AS/C1_V1/Gamedata.bgd'))
BASE = 0x00320000

XR = (-900.0, 2450.0)
YR = (-60.0, 300.0)
ZR = (-1550.0, 2350.0)

def in_world(x, y, z):
    return XR[0] <= x <= XR[1] and YR[0] <= y <= YR[1] and ZR[0] <= z <= ZR[1]

def main():
    data = open(BGD, 'rb').read()
    n = len(data)
    print("file size 0x%X, baked range 0x%08X..0x%08X" % (n, BASE, BASE + n))

    # For every plausible stride, walk the file and find runs of world points
    # whose consecutive XZ distance stays in [0.5, 120] units.
    for stride in (12, 16, 20, 24, 28, 32, 48, 64):
        runs = []
        off = 0
        while off + 12 <= n:
            # try to grow a run starting at off
            cnt = 0
            px = py = pz = None
            o = off
            while o + 12 <= n:
                x, y, z = struct.unpack_from('<fff', data, o)
                if not in_world(x, y, z):
                    break
                if px is not None:
                    d = ((x-px)**2 + (z-pz)**2) ** 0.5
                    if not (0.25 <= d <= 120.0):
                        break
                px, py, pz = x, y, z
                cnt += 1
                o += stride
            if cnt >= 16:
                runs.append((off, cnt))
                off = o
            else:
                off += 4
        if runs:
            runs.sort(key=lambda r: -r[1])
            print("stride %2d: %3d runs; top: %s" % (
                stride, len(runs),
                ", ".join("0x%X x%d" % r for r in runs[:8])))

if __name__ == '__main__':
    main()
