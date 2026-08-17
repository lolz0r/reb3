#!/usr/bin/env python3
"""validate_particlefx.py -- the PARTICLE FX engine's recovered tables,
re-derived from build/burnout3.elf and asserted against what
src/burnout3_particlefx.c has baked in.

What this checks, and why each one is worth checking:

  [1] THE NEGATIVE.  docs/RE_BOOSTFX.md 3.1's sprite-pool table at
      0x003A3E7C is exactly THREE records and stops -- the dword after
      the third is a float +3.0, not a name pointer.  The whole crash/dust FX
      story rests on that being true, so it is checked first.

  [2] THE EMITTER TABLE, 0x003A3648, 26 x 0x38.  Fully static rodata:
      every field of every row is compared against the C table.

  [3] THE SURFACE TABLE, 0x003A3BF8, 40 x 0x10.  Also static.  Its byte
      +0x0F equals the row index on all 40 rows, which is what pins the
      base and the stride; then the emitter id at +0x0C is compared.

  [4] THE DESCRIPTOR TABLE, 0x004182A0, 24 x 0x80.  Only RECORD 0 lives
      in the image -- records 1..23 are written at run time by the
      straight-line initialiser 0x0025EE20..0x002610FA -- so record 0 is
      checked field by field and the rest are reported as
      initialiser-derived (they were recovered by executing it).

  [5] THE POPCORN TABLE, 0x003EADE8, 4 x 0x50.

  [6] The literal constants the drivers use: the crash-trail emitter id
      array at 0x0041A514, FUN_00186D50's 5.0 / 2.5 / 0.4 / 0.9 window,
      FUN_00181A80's 0.875 / 0.25 / 1/256 / 0.75 emission constants, and
      FUN_00034130's 0.435 screen clamp.

  [7] The texture names, against the string blob at 0x003AAF38.

  [8] THE WHEEL-FX GATE LAW, THE SIZE LAW AND THE COLOUR BYTE ORDER --
      added 2026-08-13 with the "dust is too small / does not blend"
      fix.  The three per-wheel gate bytes are slewed towards the
      SURFACE ROW's own +0x00/+0x04/+0x08 (FUN_001807C0 @0x00180932..47)
      and feed emitters 0/3/4, with the surface row's own emitter only
      on wheels > 1; FUN_00034130's max_px/min_px limits are applied to
      the pixel HALF-extent; and the descriptor colours are 0xAABBGGRR,
      the record's LOW byte being RED (FUN_00035740 @0x000357BE unpacks
      low first, FUN_00034130 @0x0003547E puts component 0 in the
      D3DCOLOR's red lane).  All pinned as instruction bytes so a wrong
      re-reading of any of them fails here rather than on screen.

Usage:  python3 tools/validate_particlefx.py [--elf build/burnout3.elf]
"""
import argparse
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ELF = os.path.join(REPO, "build", "burnout3.elf")
SRC = os.path.join(REPO, "src", "burnout3_particlefx.c")

DESC_VA = 0x004182A0
EMIT_VA = 0x003A3648
SURF_VA = 0x003A3BF8
POOL_VA = 0x003A3E7C
POP_VA = 0x003EADE8
SLIDE_IDS_VA = 0x0041A514


class Image(object):
    def __init__(self, path):
        d = open(path, "rb").read()
        self.d = d
        phoff, = struct.unpack_from("<I", d, 0x1C)
        phes, = struct.unpack_from("<H", d, 0x2A)
        phn, = struct.unpack_from("<H", d, 0x2C)
        self.segs = []
        for i in range(phn):
            o = phoff + i * phes
            t, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<8I", d, o)
            if t == 1:
                self.segs.append((va, off, fsz))

    def off(self, va):
        for v, o, s in self.segs:
            if v <= va < v + s:
                return o + (va - v)
        return None

    def read(self, va, n):
        o = self.off(va)
        if o is None:
            raise SystemExit("VA %08x is not in any PT_LOAD segment" % va)
        return self.d[o:o + n]

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def u8(self, va):
        return self.read(va, 1)[0]

    def f32(self, va):
        return struct.unpack("<f", self.read(va, 4))[0]

    def cstr(self, va, cap=64):
        b = self.read(va, cap)
        return b.split(b"\0")[0].decode("ascii", "replace")


class Checker(object):
    def __init__(self):
        self.ok = 0
        self.fail = []

    def eq(self, name, got, want, note=""):
        if isinstance(want, float) or isinstance(got, float):
            good = abs(float(got) - float(want)) <= max(1e-6,
                                                        abs(float(want)) * 1e-5)
        else:
            good = got == want
        if good:
            self.ok += 1
        else:
            self.fail.append((name, got, want, note))
            print("  FAIL %-40s C=%s binary=%s %s" % (name, got, want, note))


# --------------------------------------------------------------------- #
# the C tables, parsed out of the module
# --------------------------------------------------------------------- #
def block(src, tag):
    m = re.search(r"B3PFX-%s-BEGIN(.*?)B3PFX-%s-END" % (tag, tag), src,
                  re.S)
    if not m:
        raise SystemExit("no B3PFX-%s block in %s" % (tag, SRC))
    return m.group(1)


def c_floats(text):
    return [float(x) for x in re.findall(r"-?\d+\.\d+(?=f)", text)]


def parse_emit(src):
    rows = []
    for line in block(src, "EMIT").splitlines():
        m = re.match(r"\s*\{\s*(\d+),\s*([-\d.]+)f,\s*\{(.*?)\},\s*(.*?)\}\s*,?\s*$",
                     line)
        if not m:
            continue
        sys_i = int(m.group(1))
        rate = float(m.group(2))
        v = [float(x) for x in re.findall(r"-?[\d.]+(?=f)", m.group(3))]
        rest = [float(x) for x in re.findall(r"-?[\d.]+(?=f)", m.group(4))]
        rows.append((sys_i, rate, v, rest))
    return rows


def parse_surf(src):
    rows = []
    for m in re.finditer(r"\{\s*([-\d.]+)f,\s*([-\d.]+)f,\s*([-\d.]+)f,\s*(N|\d+),\s*(\d+)\s*\}",
                         block(src, "SURF")):
        emit = 0x1A if m.group(4) == "N" else int(m.group(4))
        rows.append((float(m.group(1)), float(m.group(2)),
                     float(m.group(3)), emit, int(m.group(5))))
    return rows


def parse_desc(src):
    rows = []
    for line in block(src, "DESC").splitlines():
        m = re.match(r'\s*\{\s*"([a-z0-9]+)",\s*(\d+),\s*(\d+),\s*'
                     r'([-\d.]+)f,\s*([-\d.]+)f,\s*'
                     r'\{(0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+)\},\s*'
                     r'\{([-\d.]+)f,([-\d.]+)f,([-\d.]+)f\},\s*'
                     r'([-+\d.]+)f,\s*([-\d.]+)f,\s*(\d+),\s*(\d+),\s*'
                     r'([-\d.]+)f,\s*([-\d.]+)f\s*\}', line)
        if not m:
            continue
        g = m.groups()
        rows.append(dict(tex=g[0], kind=int(g[1]), blend=int(g[2]),
                         life=float(g[3]), mid=float(g[4]),
                         col=[int(g[5], 16), int(g[6], 16), int(g[7], 16)],
                         size=[float(g[8]), float(g[9]), float(g[10])],
                         grav=float(g[11]), cone=float(g[12]),
                         cap_a=int(g[13]), cap_b=int(g[14]),
                         max_px=float(g[15]), min_px=float(g[16])))
    return rows


# --------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    args = ap.parse_args()
    if not os.path.exists(args.elf):
        raise SystemExit("missing %s" % args.elf)
    img = Image(args.elf)
    src = open(SRC).read()
    ck = Checker()

    print("validate_particlefx.py -- vs %s" % args.elf)

    # -- [1] the negative: the sprite-pool table is exactly three ------- #
    print("\n[1] the sprite-pool table 0x003A3E7C is EXACTLY three records")
    names = []
    for i in range(3):
        p = img.u32(POOL_VA + i * 8)
        names.append(img.cstr(p))
    ck.eq("pool 0 name", names[0], "coronaglow")
    ck.eq("pool 1 name", names[1], "coronaboost")
    ck.eq("pool 2 name", names[2], "coronaboostred")
    ck.eq("no 4th pool (dword is a float, not a ptr)",
          "%08x" % img.u32(POOL_VA + 3 * 8), "40400000",
          note="= +3.0f, not a name pointer")
    ck.eq("4th slot as float", round(img.f32(POOL_VA + 3 * 8), 3), 3.0)
    print("  pools: %s -- and the table stops" % ", ".join(names))

    # -- [2] the emitter table ------------------------------------------ #
    print("\n[2] the emitter table 0x003A3648, 26 x 0x38")
    crows = parse_emit(src)
    ck.eq("emitter row count", len(crows), 26)
    for i, (sys_i, rate, v, rest) in enumerate(crows):
        base = EMIT_VA + i * 0x38
        ck.eq("emit[%d].sys" % i, sys_i, img.u32(base))
        ck.eq("emit[%d].rate" % i, rate, img.f32(base + 4))
        for k in range(6):
            ck.eq("emit[%d].v[%d]" % (i, k), v[k], img.f32(base + 8 + k * 4))
        # rest = size_lo, size_hi, alpha_lo, alpha_hi, strm_a, strm_b
        for k, off in enumerate((0x20, 0x24, 0x28, 0x2C, 0x30, 0x34)):
            ck.eq("emit[%d]+0x%02X" % (i, off), rest[k], img.f32(base + off))
    print("  26 rows x 12 fields checked")

    # -- [3] the surface table ------------------------------------------ #
    print("\n[3] the surface table 0x003A3BF8, 40 x 0x10")
    srows = parse_surf(src)
    ck.eq("surface row count", len(srows), 40)
    for i, (scale, skid, grav, emit, decal) in enumerate(srows):
        base = SURF_VA + i * 0x10
        # the self-check that pins base+stride
        ck.eq("surf[%d]+0x0F == index" % i, img.u8(base + 0x0F), i)
        ck.eq("surf[%d].scale" % i, scale, img.f32(base))
        ck.eq("surf[%d].emit" % i, emit, img.u8(base + 0x0C))
    live = [(i, r[3]) for i, r in enumerate(srows) if r[3] != 0x1A]
    print("  40 rows; %d carry a wheel emitter: %s"
          % (len(live), ", ".join("%d->%d" % t for t in live)))

    # -- [4] the descriptor table --------------------------------------- #
    print("\n[4] the descriptor table 0x004182A0 -- record 0 is in .data")
    drows = parse_desc(src)
    ck.eq("descriptor row count", len(drows), 24)
    d0 = drows[0]
    ck.eq("desc[0].tex", d0["tex"], img.cstr(img.u32(DESC_VA)))
    ck.eq("desc[0].kind", d0["kind"], img.u32(DESC_VA + 0x08))
    ck.eq("desc[0].blend", d0["blend"], img.u32(DESC_VA + 0x0C))
    ck.eq("desc[0].life", d0["life"], img.f32(DESC_VA + 0x10))
    ck.eq("desc[0].mid_t", d0["mid"], img.f32(DESC_VA + 0x14))
    for k in range(3):
        ck.eq("desc[0].col[%d]" % k, "%08x" % d0["col"][k],
              "%08x" % img.u32(DESC_VA + 0x18 + k * 4))
        ck.eq("desc[0].size[%d]" % k, d0["size"][k],
              img.f32(DESC_VA + 0x28 + k * 4))
    ck.eq("desc[0].grav", d0["grav"], img.f32(DESC_VA + 0x44))
    ck.eq("desc[0].cone", d0["cone"], img.f32(DESC_VA + 0x4C))
    ck.eq("desc[0].cap_a", d0["cap_a"], img.u32(DESC_VA + 0x60))
    ck.eq("desc[0].cap_b", d0["cap_b"], img.u32(DESC_VA + 0x64))
    ck.eq("desc[0].max_px", d0["max_px"], img.f32(DESC_VA + 0x6C))
    ck.eq("desc[0].min_px", d0["min_px"], img.f32(DESC_VA + 0x70))
    print("  record 0 (%s) checked field by field;" % d0["tex"])
    print("  records 1..23 are written at run time by the straight-line")
    print("  initialiser 0x0025EE20..0x002610FA and were recovered by")
    print("  EXECUTING it -- they are not in the image to compare against.")

    # -- [5] the popcorn table ------------------------------------------ #
    print("\n[5] the popcorn table 0x003EADE8, 4 x 0x50")
    for i, want in enumerate(("fxpopcornspark", "fxpopcorndebris",
                              "fxpopcorndebris", "fxpopcorndebris")):
        got = img.cstr(img.u32(POP_VA + i * 0x50))
        ck.eq("popcorn[%d].tex" % i, got, want)
    print("  4 texture bindings checked")

    # -- [6] the driver constants --------------------------------------- #
    print("\n[6] the driver constants")
    ids = [img.u32(SLIDE_IDS_VA + i * 4) for i in range(3)]
    ck.eq("crash-trail emitter ids @0x0041A514", ids, [20, 21, 22])
    csrc = re.search(r"B3_PFX_SLIDE_IDS\[3\] = \{ (\d+), (\d+), (\d+) \}", src)
    ck.eq("C slide ids", [int(x) for x in csrc.groups()], ids)
    for name, va, want in (
            ("SLIDE_WINDOW 5.0  @0x003B1694", 0x003B1694, 5.0),
            ("FADE_A 0.4        @0x003B16E8", 0x003B16E8, 0.4),
            ("FADE_B 0.9        @0x003A69C0", 0x003A69C0, 0.9),
            ("RATE_JITTER_LO    @0x0039922C", 0x0039922C, 0.875),
            ("RATE_JITTER_SP    @0x003B1730", 0x003B1730, 0.25),
            ("1/256             @0x003B1B40", 0x003B1B40, 1.0 / 256.0),
            ("STREAM_A_K 0.75   @0x003A55F8", 0x003A55F8, 0.75)):
        ck.eq(name, img.f32(va), want)
    cm = {m.group(1): float(m.group(2))
          for m in re.finditer(r"#define\s+(B3_PFX_[A-Z0-9_]+)\s+([-\d.]+)f", src)}
    ck.eq("C SLIDE_WINDOW", cm["B3_PFX_SLIDE_WINDOW"], 5.0)
    ck.eq("C SLIDE_HOLD", cm["B3_PFX_SLIDE_HOLD"], 2.5)
    ck.eq("C SLIDE_FADE_A", cm["B3_PFX_SLIDE_FADE_A"], 0.4)
    ck.eq("C SLIDE_FADE_B", cm["B3_PFX_SLIDE_FADE_B"], 0.9)
    ck.eq("C RATE_JITTER_LO", cm["B3_PFX_RATE_JITTER_LO"], 0.875)
    ck.eq("C RATE_JITTER_SP", cm["B3_PFX_RATE_JITTER_SP"], 0.25)
    ck.eq("C STREAM_A_K", cm["B3_PFX_STREAM_A_K"], 0.75)
    ck.eq("C MAXPX_K", cm["B3_PFX_MAXPX_K"], 0.435)
    ck.eq("C SIZE_K", cm["B3_PFX_SIZE_K"], 0.9)
    # the trail fades to exactly 0.1 at the end of the 5 s window
    tail = 1.0 - (5.0 - 2.5) * 0.4 * 0.9
    ck.eq("slide fade lands on 0.1 at t=5", round(tail, 6), 0.1)

    # -- [7] the texture names ------------------------------------------ #
    print("\n[7] the fx* texture names in the string blob 0x003AAF38")
    blob = img.read(0x003AAF38, 0x100)
    for n in ("fxdebris2", "fxgravel", "fxsnow", "fxglass", "fxsmoke",
              "fxexplosionsmoke", "fxfire", "fxexplosionfire",
              "fxexplosionflash", "fxdebris1", "fxspark",
              "fxpopcorndebris", "fxpopcornspark"):
        ck.eq("blob has %s" % n, n.encode() + b"\0" in blob, True)
    used = sorted(set(r["tex"] for r in drows))
    for n in used:
        ck.eq("C descriptor texture %s is the game's" % n,
              n.encode() + b"\0" in blob, True)
    print("  %d distinct textures used by the C descriptors: %s"
          % (len(used), ", ".join(used)))

    # -- [8] the wheel-FX gate law, the size law and the colour order -- #
    print("\n[8] FUN_001807C0's gate law, FUN_00034130's size law and the"
          "\n    0xAABBGGRR colour order")

    def code(name, va, want_hex, note=""):
        ck.eq(name, img.read(va, len(want_hex) // 2).hex(), want_hex,
              note=note)

    # the four emitter ids are register immediates at the call sites
    code("emitter 0 @0x00180CDF  XOR EAX,EAX", 0x00180CDF, "33c0")
    code("emitter 1 @0x00180D44  MOV EAX,1", 0x00180D44, "b801000000")
    code("emitter 3 @0x00180DDF  MOV EAX,3", 0x00180DDF, "b803000000")
    code("emitter 4 @0x00180E7A  MOV EAX,4", 0x00180E7A, "b804000000")
    gsrc = re.search(r"B3_PFX_GATE_IDS\[3\]\s*=\s*\{ (\d+), (\d+), (\d+) \}",
                     src)
    ck.eq("C gate->emitter ids", [int(x) for x in gsrc.groups()], [0, 3, 4])
    ck.eq("C companion emitter id",
          int(re.search(r"B3_PFX_WHEEL_EXTRA_ID\s+(\d+)", src).group(1)), 1)

    # the surface emitter is REAR-WHEELS-ONLY: `if (1 < wheel) ...`
    code("rear-only gate @0x00180924  CMP [EBP+0x10],1 / JBE",
         0x00180924, "837d10017608",
         note="the surface row's emitter id is only read for wheel > 1")
    code("surf emit id  @0x0018092A  MOVSX EDX,byte [ESI+0x0C]",
         0x0018092A, "0fbe560c")
    # the three gate targets are surface row +0x00, +0x04, +0x08
    code("gate0 target  @0x00180932  MOVSS XMM1,[ESI]", 0x00180932,
         "f30f100e", note="row+0x00 'scale', times the skid flag")
    code("gate1 target  @0x00180936  MOVSS XMM0,[ESI+0x04]", 0x00180936,
         "f30f104604", note="row+0x04 'skid'")
    code("gate2 target  @0x00180947  MOVSS XMM0,[ESI+0x08]", 0x00180947,
         "f30f104608", note="row+0x08 'gravelness'")
    # the three gate bytes and the first dither carry
    code("gate byte 0   @0x00180969  LEA ESI,[EBX+0xA0]", 0x00180969,
         "8db3a0000000")
    code("gate byte 1   @0x00180984  LEA ESI,[EBX+0xA1]", 0x00180984,
         "8db3a1000000")
    code("gate byte 2   @0x0018099C  LEA ESI,[EBX+0xA2]", 0x0018099C,
         "8db3a2000000")
    code("carry byte 0  @0x00180CBF  LEA ECX,[EBX+0xA4]", 0x00180CBF,
         "8d8ba4000000")
    # FUN_001805B0's slew
    code("slew x255     @0x001805B3", 0x001805B3, "f30f590dc4163b00")
    code("slew step*510 @0x001805D2", 0x001805D2, "f30f590d40183b00")
    code("slew round+.5 @0x00180639", 0x00180639, "d80584163b00")
    for name, va, want in (("gate FULL 255.0 @0x003B16C4", 0x003B16C4, 255.0),
                           ("gate SLEW 510.0 @0x003B1840", 0x003B1840, 510.0),
                           ("gate ROUND 0.5  @0x003B1684", 0x003B1684, 0.5),
                           ("gate INTEN 2.0  @0x003B1688", 0x003B1688, 2.0),
                           ("kind3/4 sqrt(2) @0x003A34B8", 0x003A34B8,
                            2.0 ** 0.5)):
        ck.eq(name, round(img.f32(va), 6), round(want, 6))
    ck.eq("C GATE_FULL", cm["B3_PFX_GATE_FULL"], 255.0)
    ck.eq("C GATE_SLEW", cm["B3_PFX_GATE_SLEW"], 510.0)
    ck.eq("C GATE_ROUND", cm["B3_PFX_GATE_ROUND"], 0.5)
    ck.eq("C GATE_RATE_K", cm["B3_PFX_GATE_RATE_K"], 0.25)
    ck.eq("C GATE_RATE_B", cm["B3_PFX_GATE_RATE_B"], 0.75)
    ck.eq("C GATE_INTEN_K", cm["B3_PFX_GATE_INTEN_K"], 2.0)
    ck.eq("C SQRT2", round(cm["B3_PFX_SQRT2"], 6), round(2.0 ** 0.5, 6))
    ck.eq("C HALF_TO_FULL", cm["B3_PFX_HALF_TO_FULL"], 2.0)
    ck.eq("C LEVEL is the recovered 1.0", cm["B3_PFX_LEVEL"], 1.0)

    # the two screen limits are applied to the pixel HALF-extent
    code("max_px  read  @0x000346B9  MOVSS XMM0,[EDI+0xD0]", 0x000346B9,
         "f30f1087d0000000")
    code("max_px *0.435 @0x000346C1", 0x000346C1, "f30f59056c1e3b00")
    code("clamp the HALF @0x000346DF  MOVSS [ESP+0x1D0],XMM0", 0x000346DF,
         "f30f118424d0010000", note="[ESP+0x1D0] is the half-extent that "
                                    "0x00034814 mirrors into +-corners")
    code("min_px  read  @0x000346F1  MOVSS XMM0,[EDI+0x9C]", 0x000346F1,
         "f30f10879c000000")
    code("kind3/4 sqrt2 @0x00035138", 0x00035138, "f30f1015b8343a00")

    # the colour byte order: low byte first into component 0, and
    # component 0 lands in the D3DCOLOR's RED lane
    code("unpack +0x1B->comp3 @0x000357BE", 0x000357BE, "0fb6481b")
    code("unpack +0x18->comp0 @0x000357D2", 0x000357D2, "0fb65018")
    code("comp0 -> system+0x40 @0x000357ED", 0x000357ED, "f30f116640")
    code("kind4 alpha x0.5   @0x000352FD", 0x000352FD, "f30f590584163b00")
    # and the sanity check the swap is really a swap: fxfire's middle key
    fire = [r for r in drows if r["tex"] == "fxfire"]
    ck.eq("fxfire descriptors", len(fire), 2)
    r0, g0, b0 = (fire[0]["col"][1] & 0xFF, (fire[0]["col"][1] >> 8) & 0xFF,
                  (fire[0]["col"][1] >> 16) & 0xFF)
    ck.eq("fxfire mid key reads ORANGE (R>G>B)", (r0 > g0 and g0 > b0), True,
          note="0x%08X -> (%d,%d,%d)" % (fire[0]["col"][1], r0, g0, b0))
    gv = [r for r in drows if r["tex"] == "fxgravel"][0]
    r0, g0, b0 = (gv["col"][1] & 0xFF, (gv["col"][1] >> 8) & 0xFF,
                  (gv["col"][1] >> 16) & 0xFF)
    ck.eq("fxgravel mid key reads SAND (R>G>B)", (r0 > g0 and g0 > b0), True,
          note="0x%08X -> (%d,%d,%d)" % (gv["col"][1], r0, g0, b0))
    # the C reader must take the LOW byte as red
    ck.eq("C key_lerp takes the low byte as red",
          "rgba[0] = ((a & 0xFF)" in src, True)
    print("  gate law (3 targets, 4 emitter ids, rear-only surface row),")
    print("  the slew, the half-extent screen limits, the kind 3/4 sqrt(2)")
    print("  and the 0xAABBGGRR colour order all pinned to the image")

    print("\n%s: %d/%d checks green" %
          ("PASS" if not ck.fail else "FAIL",
           ck.ok, ck.ok + len(ck.fail)))
    if ck.fail:
        for name, got, want, note in ck.fail[:40]:
            print("  - %s: C=%s binary=%s %s" % (name, got, want, note))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
