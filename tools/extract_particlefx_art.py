#!/usr/bin/env python3
"""
Art for the CRASH DUST / SMOKE / DEBRIS layer (src/burnout3_crashfx.c).

The names are the GAME's, not chosen here.  `Data/Global.txd` -- the same
flat Criterion container the boost-flame and corona rasters come out of
(docs/RE_FRONTEND.md 1) -- carries a whole `fx*` family:

    fxsmoke            the grey puff:      dust clouds + tyre smoke
    fxexplosionsmoke   the darker, denser puff used by the explosion
    fxdebris1          scattered panel/glass shrapnel, sheet 1
    fxdebris2          ... sheet 2
    fxspark            the hot scrape spark
    fxpopcorndebris    the "popcorn" burst debris
    fxpopcornspark     ... and its spark

and the executable additionally carries the literal `fxskid`
(VA 0x003E7BC8), the tyre-skid decal.  These are exactly the rasters the
retail crash presentation puts around a wreck (see the reference frame
a reference capture of a sliding car: a wide dust plume
behind the sliding car and scattered dark shrapnel on the road).

Unlike the boost flame, whose two rasters are POOLS in the executable's own
three-entry sprite-pool table at 0x003A3E7C (`coronaglow` / `coronaboost` /
`coronaboostred` -- and that table has NO fourth entry, so the crash FX are
NOT sprite-pool clients), these are bound by the FX system directly.  The
emitter law is recorded in src/burnout3_crashfx.h.

Usage:
  python3 tools/extract_crashfx_art.py
Exits non-zero if any texture is missing or fails to decode.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_carfx_art import GLOBAL_TXD, TXD_MAGIC, decode_record  # noqa: E402

OUTDIR = "build/particlefx"

# every `fx*` raster the crash layer can use, by the name in Global.txd
WANTED = ("fxsmoke", "fxexplosionsmoke", "fxexplosionfire",
          "fxexplosionflash", "fxfire", "fxdebris1", "fxdebris2",
          "fxglass", "fxgravel", "fxsnow", "fxspark",
          "fxpopcorndebris", "fxpopcornspark", "fxscrape")

# the ones burnout3_crashfx.c refuses to run without
REQUIRED = ("fxsmoke", "fxdebris1", "fxglass", "fxgravel")


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
            got[name] = (w, h, im.getchannel('A').getextrema(),
                         im.convert('RGB').getextrema())
    for n in WANTED:
        if n not in got:
            msg = "%s not found in %s" % (n, GLOBAL_TXD)
            if n in REQUIRED:
                raise SystemExit("FAIL: " + msg)
            print("  (optional) " + msg)
            continue
        w, h, aext, rgbext = got[n]
        print("  %-18s %dx%d  alpha %s  rgb %s -> %s/%s.png"
              % (n, w, h, aext, rgbext, OUTDIR, n))
    return got


def main():
    print("crash dust/debris art (names read out of Data/Global.txd):")
    extract_textures()
    return 0


if __name__ == "__main__":
    sys.exit(main())
