#!/usr/bin/env python3
"""
Art for the BOOST EXHAUST FLAME (src/burnout3_boostfx.c).

Two textures, both named BY THE EXECUTABLE, not chosen here:

  FUN_0017EE00 (0x0017EE00) walks a three-entry {const char* name, u32}
  table at VA 0x003A3E7C and creates one sprite POOL per entry:

      pool 0 -> "coronaglow"        (VA 0x003B0428)   the light coronas
      pool 1 -> "coronaboost"       (VA 0x003B041C)
      pool 2 -> "coronaboostred"    (VA 0x003B040C)

  FUN_00187BE0 (the corona emitter) hard-codes pool 0 (`ADD ECX,0xC`
  @0x00187C28), so the car lights are always `coronaglow` and pools 1/2 are
  used by nothing else in the image except the exhaust-flame emitter
  FUN_001871E0, which picks its pool from FUN_00179F30's output word:
  1 when the flame-type byte carObj+0x1901 is 0, 2 when it is not
  (0x00179F30 stores int 1 / int 2 into out[5]; FUN_001871E0 reads it at
  ESP+0x174 and forms `EDI + 0xC + pool*0xC` @0x0018723C/0x00187382).   [C]

  tools/extract_carfx_art.py already pulls `coronaglow`; this tool pulls the
  other two, into a directory of its own so the two modules stay separable.

The per-emitter POSITIONS are not extracted here: they are corona light-table
type 8 records, which tools/extract_carfx_art.py already writes into
build/cars/<CLS>_<Car>.lights as `light 8 aux8 ...`.  FUN_001871E0 reads
    records = *(u32*)(model + 0x1664 + 8*4)  = model + 0x1684   @0x0018725B
    count   = *(u8*) (model + 0x16AC + 8)    = model + 0x16B4   @0x00187261
i.e. exactly type 8 of the same table.                                  [C]

Container decoding is delegated to the repo's existing, unmodified readers.

Usage:
  python3 tools/extract_boostfx_art.py
Exits non-zero if either texture is missing or fails to decode.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_carfx_art import GLOBAL_TXD, TXD_MAGIC, decode_record  # noqa: E402

OUTDIR = "build/boostfx"

# names taken from the executable's pool table at 0x003A3E7C, not chosen here
WANTED = ("coronaboost", "coronaboostred")


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
            raise SystemExit("FAIL: %s not found in %s" % (n, GLOBAL_TXD))
        w, h, aext, rgbext = got[n]
        print("  %-16s %dx%d  alpha %s  rgb %s -> %s/%s.png"
              % (n, w, h, aext, rgbext, OUTDIR, n))
    return got


def report_emitters():
    """Sanity-report the type-8 (exhaust) records already in build/cars."""
    d = "build/cars"
    if not os.path.isdir(d):
        print("  (build/cars absent -- run tools/extract_carfx_art.py first)")
        return 0
    tot = ncars = 0
    sample = None
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".lights"):
            continue
        n = 0
        for line in open(os.path.join(d, fn)):
            if line.startswith("light 8 "):
                n += 1
                if sample is None:
                    sample = (fn, line.strip())
        if n:
            ncars += 1
        tot += n
    print("  type-8 exhaust emitters: %d records over %d cars" % (tot, ncars))
    if sample:
        print("    e.g. %s: %s" % sample)
    return tot


def main():
    print("boost-flame art (names read out of burnout3.elf):")
    extract_textures()
    report_emitters()
    return 0


if __name__ == "__main__":
    sys.exit(main())
