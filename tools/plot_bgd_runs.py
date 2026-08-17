#!/usr/bin/env python3
"""Overlay candidate .bgd polyline runs on the track mesh (top-down PNG).

Visual ground truth for scan_bgd_paths.py candidates: a real route traces the
road network; a false positive scatters. Writes build/bgd_overlay.png.
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import struct, sys
from PIL import Image, ImageDraw

BGD = (game_path('Tracks/AS/C1_V1/Gamedata.bgd'))
OBJ = "build/track.obj"

# (file offset, count, stride) candidates to draw, one color each
RUNS = [
    (0x19D970, 1752, 16, (255, 60, 60)),
    (0x1ABF20, 1029, 16, (60, 255, 60)),
    (0x1A8538, 926, 16, (60, 120, 255)),
    (0x1AFF70, 910, 16, (255, 255, 60)),
    (0x1B3850, 910, 16, (255, 60, 255)),
    (0x17C3A0, 566, 16, (60, 255, 255)),
    (0x17E700, 482, 16, (255, 160, 60)),
    (0x189C40, 472, 16, (160, 60, 255)),
]

W = H = 1600

def main():
    data = open(BGD, 'rb').read()

    xs, zs = [], []
    tris = []
    verts = []
    for line in open(OBJ):
        if line.startswith('v '):
            _, x, y, z = line.split()[:4]
            verts.append((float(x), float(z)))
        elif line.startswith('f '):
            idx = [int(t.split('/')[0]) - 1 for t in line.split()[1:4]]
            tris.append(idx)
    xs = [v[0] for v in verts]; zs = [v[1] for v in verts]
    x0, x1 = min(xs), max(xs); z0, z1 = min(zs), max(zs)
    s = min((W - 40) / (x1 - x0), (H - 40) / (z1 - z0))
    def m(x, z):
        return (20 + (x - x0) * s, 20 + (z - z0) * s)

    im = Image.new('RGB', (W, H), (16, 16, 24))
    dr = ImageDraw.Draw(im)
    for t in tris:
        p = [m(*verts[i]) for i in t]
        dr.polygon(p, fill=(52, 52, 62))

    for off, cnt, stride, col in RUNS:
        pts = []
        for i in range(cnt):
            x, y, z = struct.unpack_from('<fff', data, off + i * stride)
            pts.append(m(x, z))
        dr.line(pts, fill=col, width=2)
        dr.ellipse([pts[0][0]-4, pts[0][1]-4, pts[0][0]+4, pts[0][1]+4], outline=col, width=2)
        x, y, z = struct.unpack_from('<fff', data, off)
        print("0x%06X x%-5d  start world (%.0f, %.0f, %.0f)" % (off, cnt, x, y, z))

    out = sys.argv[1] if len(sys.argv) > 1 else "build/bgd_overlay.png"
    im.save(out)
    print("wrote", out)

if __name__ == '__main__':
    main()
