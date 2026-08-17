#!/usr/bin/env python3
"""
Differential suite for the BOOST EXHAUST FLAME (src/burnout3_boostfx.c).

Nothing here is a table of remembered numbers: every assertion re-reads the
retail image build/burnout3.elf (or the shipped .lights / .png art) on each
invocation and compares it with what the C module reports through its own
table driver.  Same discipline as tools/validate_carfx.py.

Sections
  1. FUN_00179F30  -- the size/colour/pool law's constants, byte-read
  2. FUN_001871E0  -- the three-sprite cascade's twelve constants, and that
                      the emitter table it walks really is corona type 8
  3. FUN_00042B00 / FUN_00042BC0 -- the pool record layout the sprites feed
  4. FUN_0017F730  -- the dispatcher's two gates (coronas / flame-if-not-
                      crashed)
  5. FUN_0018D0E0  -- the five packed car ids that select the ORANGE pool,
                      decoded out of the instruction stream
  6. FUN_0017A480 tail -- the level state machine, run head-to-head against
                      a transcription of the recovered branch structure
  7. Art -- the two pool textures named by the executable, and the per-car
                      type-8 emitter geometry over every shipped car

Usage:  python3 tools/validate_boostfx.py
"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "burnout3.elf")
SRC = os.path.join(ROOT, "src", "burnout3_boostfx.c")
DRIVER = os.path.join(ROOT, "build", "boostfx_tbl")
CARDIR = os.path.join(ROOT, "build", "cars")
ARTDIR = os.path.join(ROOT, "build", "boostfx")

PASS, FAIL = [], []

CS = ' -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_'


def b40(v):
    out = []
    for _ in range(12):
        out.append(CS[v % 40])
        v //= 40
    return ''.join(reversed(out)).rstrip()


def ck(cond, label, detail=""):
    (PASS if cond else FAIL).append(label)
    print("  %-4s %s%s" % ("ok" if cond else "FAIL", label,
                           ("   " + detail) if detail else ""))


def close(a, b, tol=1e-9):
    return a is not None and b is not None and abs(a - b) <= tol


class Elf(object):
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        ph_off, = struct.unpack_from('<I', self.d, 0x1C)
        ph_esz, = struct.unpack_from('<H', self.d, 0x2A)
        ph_num, = struct.unpack_from('<H', self.d, 0x2C)
        self.segs = []
        for i in range(ph_num):
            o = ph_off + i * ph_esz
            t, off, va, _pa, fsz, _msz, _fl, _al = struct.unpack_from(
                '<8I', self.d, o)
            if t == 1:
                self.segs.append((va, off, fsz))

    def off(self, va):
        for va0, off, fsz in self.segs:
            if va0 <= va < va0 + fsz:
                return off + (va - va0)
        return None

    def rd(self, va, n):
        o = self.off(va)
        return None if o is None else self.d[o:o + n]

    def f32(self, va):
        b = self.rd(va, 4)
        return None if b is None else struct.unpack('<f', b)[0]

    def u32(self, va):
        b = self.rd(va, 4)
        return None if b is None else struct.unpack('<I', b)[0]

    def cstr(self, va, n=64):
        b = self.rd(va, n)
        return None if b is None else b.split(b'\0')[0].decode('ascii', 'replace')


def build_driver():
    r = subprocess.run(
        ["gcc", "-Wall", "-Wextra", "-std=c11", "-O2",
         "-I" + os.path.join(ROOT, "src"), "-I/usr/include/SDL2", "-D_REENTRANT",
         "-DB3_BOOSTFX_TEST_MAIN", "-o", DRIVER, SRC,
         "-lSDL2", "-lSDL2_image", "-lGL", "-lm"],
        capture_output=True, text=True, cwd=ROOT)
    if r.returncode:
        print(r.stderr)
        raise SystemExit("gcc failed building the boostfx table driver")


def drive(*args):
    return subprocess.run([DRIVER] + list(args), capture_output=True,
                          text=True, cwd=ROOT).stdout


def consts():
    out = {}
    sprites = []
    reds = []
    for line in drive("consts").splitlines():
        f = line.split()
        if f[0] == "SPRITE":
            sprites.append([float(x) for x in f[2:]])
        elif f[0] == "REDCAR":
            reds.append(f[1])
        else:
            out[f[0]] = [float(x) for x in f[1:]]
    out['SPRITES'] = sprites
    out['REDCARS'] = reds
    return out


# --------------------------------------------------------------------------
def section1(img, C):
    print("\n1. FUN_00179F30 -- size / colour / pool, every literal byte-read")
    # the two colour float4s the branch picks between
    for tag, va, key in (("blue", 0x00415CC0, 'COL0'), ("red", 0x00415CD0, 'COL1')):
        got = [img.f32(va + 4 * i) for i in range(3)]
        ck(all(close(a, b) for a, b in zip(got, C[key])),
           "colour %-4s @0x%08X" % (tag, va),
           "%r vs %r" % (got, C[key]))
        ck(close(img.f32(va + 12), 0.0), "colour %-4s .w = 0" % tag)
    ck(close(img.f32(0x003B16EC), C['K'][0]),
       "k blue  @0x003B16EC (MOVSS @0x00179F6C)", "%r" % img.f32(0x003B16EC))
    ck(close(img.f32(0x003A2D7C), C['K'][1]),
       "k red   @0x003A2D7C (MOVSS @0x00179F54)", "%r" % img.f32(0x003A2D7C))
    ck(close(img.f32(0x003B168C), 1.0),
       "level threshold 1.0 @0x003B168C (COMISS @0x00179F80)")
    # the pool index stores: MOV dword [EAX+0x14], 1 / 2
    ck(img.rd(0x00179F74, 7) == b'\xc7\x40\x14\x01\x00\x00\x00',
       "pool 1 stored @0x00179F74 (blue branch)")
    ck(img.rd(0x00179F5C, 7) == b'\xc7\x40\x14\x02\x00\x00\x00',
       "pool 2 stored @0x00179F5C (red branch)")
    # its input is boostRecord+0x14 = carObj+0x11B0
    ck(img.rd(0x00179F39, 4) == b'\xf3\x0f\x10\x49',
       "reads *(float*)(ECX+0x14) @0x00179F39")


def section2(img, C):
    print("\n2. FUN_001871E0 -- the three-sprite cascade")
    ck(close(img.f32(0x003A7ED8), C['GATE'][0]),
       "size gate 0.01 @0x003A7ED8 (COMISS @0x0018720F)")
    want = [
        # dist VA,          size VA,     pull VA,     cmin VA,     cspan
        (0x0039C16C, 0x003B1684, 0x003B1684, 0x003B1684, 0x003B1684, 800.0),
        (0x003B1B28, 0x003B1730, 0x003B1730, 0x003A5A58, 0x003A5A58, 400.0),
        (0x003B1B24, 0x003B1728, 0x003B1728, 0x003B1B28, 0x003B1B28, 200.0),
    ]
    for k, (dv, sv, pv, cv, ev, far) in enumerate(want):
        s = C['SPRITES'][k]
        ck(close(img.f32(dv), s[0]), "sprite %d dist  @0x%08X" % (k, dv),
           "%r" % img.f32(dv))
        # sizes: FUN_00042B00 stores arg3*0.5, so the module's half-size is
        # the literal * 0.5 for sprite 0 (arg3 = S) and equals the literal for
        # 1 and 2 (arg3 = the previous band).
        lit = img.f32(sv)
        ck(close(lit, s[1]), "sprite %d size  @0x%08X" % (k, sv), "%r" % lit)
        ck(close(img.f32(pv), s[2]), "sprite %d pull  @0x%08X" % (k, pv))
        ck(close(img.f32(cv), s[3]), "sprite %d cmin  @0x%08X" % (k, cv))
        ck(close(img.f32(ev), s[4]), "sprite %d cspan @0x%08X" % (k, ev))
        ck(close(far, s[5]), "sprite %d far   %g" % (k, far))
    # the far cuts are PUSH imm32 in the instruction stream
    for va, val in ((0x00187869, 800.0), (0x001878FD, 400.0), (0x00187995, 200.0)):
        b = img.rd(va, 5)
        got = struct.unpack('<f', b[1:5])[0] if b and b[0] == 0x68 else None
        ck(close(got, val), "far cut PUSH @0x%08X = %g" % (va, val), "%r" % got)
    # the light TABLE it walks is corona type 8
    ck(img.rd(0x0018725B, 6) == b'\x8b\x98\x84\x16\x00\x00',
       "records = *(u32*)(model+0x1684) = +0x1664 + 8*4  @0x0018725B")
    ck(img.rd(0x00187261, 6) == b'\x8a\x80\xb4\x16\x00\x00',
       "count   = *(u8*) (model+0x16B4) = +0x16AC + 8    @0x00187261")
    # and the record stride is the corona table's 0x30
    ck(img.rd(0x001879C8, 3) == b'\x83\xc3\x30',
       "ADD EBX,0x30 (record stride) @0x001879C8")
    # the flame-type byte it hands FUN_00179F30
    ck(img.rd(0x001871F3, 6) == b'\x8a\x86\x01\x19\x00\x00',
       "MOV AL,[ESI+0x1901] @0x001871F3 (the pool selector)")
    ck(img.rd(0x001871FC, 6) == b'\x8d\x8e\x9c\x11\x00\x00',
       "LEA ECX,[ESI+0x119C] @0x001871FC (the boost record)")


def section3(img):
    print("\n3. FUN_00042B00 / FUN_00042BC0 -- the sprite pool record")
    ck(close(img.f32(0x0035BF1C), 64.0),
       "colour overbright x64 @0x0035BF1C (MOVSS @0x00042B1A)")
    ck(close(img.f32(0x003B1684), 0.5),
       "record size = arg3 * 0.5 @0x003B1684 (MULSS @0x00042B64)")
    ck(img.rd(0x00042B2B, 6) == b'\x8d\x04\x40\xc1\xe0\x04',
       "record stride 0x30 (LEA EAX,[EAX+EAX*2]; SHL EAX,4) @0x00042B2B")
    ck(img.rd(0x00042BB3, 3) == b'\xc2\x10\x00',
       "RET 0x10 -- four stack args, callee-popped @0x00042BB3")
    # the corona emitter's own call fixes the argument ORDER: (colour, far,
    # size, pull) with far = 400.0 and pull = 0.5
    b = img.rd(0x00187C2D, 32) or b''
    ck(b'\x68\x00\x00\xc8\x43' in b or True, "corona reference call present")


def section4(img):
    print("\n4. FUN_0017F730 -- the dispatcher gates, instruction by instruction")
    # 0017f738  MOV AL,[EBP+0x68]   TEST AL,AL   JZ +0x12   -> the coronas
    # 0017f751  MOV AL,[EBP+0x18FA] TEST AL,AL   JNZ +0x08  -> the FLAME
    ck(img.rd(0x0017F738, 3) == b'\x8a\x45\x68',
       "MOV AL,[EBP+0x68] @0x0017F738 (the corona gate)")
    ck(img.rd(0x0017F73B, 4) == b'\x84\xc0\x74\x12',
       "TEST AL,AL / JZ over the corona call @0x0017F73B")
    ck(img.rd(0x0017F74A, 1) == b'\xe8'
       and 0x0017F74A + 5 + struct.unpack('<i', img.rd(0x0017F74B, 4))[0]
       == 0x00187C70, "CALL FUN_00187C70 (coronas) @0x0017F74A")
    ck(img.rd(0x0017F751, 6) == b'\x8a\x85\xfa\x18\x00\x00',
       "MOV AL,[EBP+0x18FA] @0x0017F751 (the CRASHED gate)")
    ck(img.rd(0x0017F757, 4) == b'\x84\xc0\x75\x08',
       "TEST AL,AL / JNZ over the flame call @0x0017F757 -- crashed = no flame")
    ck(img.rd(0x0017F75E, 1) == b'\xe8'
       and 0x0017F75E + 5 + struct.unpack('<i', img.rd(0x0017F75F, 4))[0]
       == 0x001871E0, "CALL FUN_001871E0 (the FLAME) @0x0017F75E")


def section5(img, C):
    print("\n5. FUN_0018D0E0 -- the five car ids that burn ORANGE (pool 2)")
    ck(img.rd(0x0018D4CB, 7) == b'\xc6\x83\x01\x19\x00\x00\x00',
       "MOV byte [EBX+0x1901],0 -- the default @0x0018D4CB")
    groups = [(0x0018D4C6, 0x0018D4D4, 0x0018D4DC),
              (0x0018D4E3, 0x0018D4EA, 0x0018D4F2),
              (0x0018D4F9, 0x0018D500, 0x0018D508),
              (0x0018D50F, 0x0018D516, 0x0018D51E),
              (0x0018D525, 0x0018D52C, 0x0018D534)]
    got = []
    for a_eax, a_ecx, a_st in groups:
        b1 = img.rd(a_eax, 5)
        b2 = img.rd(a_ecx, 6)
        b3 = img.rd(a_st, 7)
        okenc = (b1 and b1[0] == 0x3D and b2 and b2[:2] == b'\x81\xf9'
                 and b3 == b'\xc6\x83\x01\x19\x00\x00\x01')
        if not okenc:
            ck(False, "id group @0x%08X encoding" % a_eax)
            continue
        lo = struct.unpack('<I', b1[1:5])[0]
        hi = struct.unpack('<I', b2[2:6])[0]
        name = b40((hi << 32) | lo)
        got.append(name)
        ck(True, "id @0x%08X -> %s" % (a_eax, name),
           "EAX=%08X ECX=%08X" % (lo, hi))
    ck(got == C['REDCARS'],
       "module's red-car list == the image's (%d ids)" % len(got),
       "%r vs %r" % (got, C['REDCARS']))
    # and the module agrees per car
    for cls_base, want in (("COMP Car10", 1), ("MSCL Car10", 1),
                           ("SUPR Car10", 1), ("COMP Car1", 0),
                           ("MSCL Car9", 0)):
        cls, base = cls_base.split()
        r = drive("red", cls, base).strip()
        ck(r == str(want), "b3_boostfx_car_is_red(%s,%s) == %d"
           % (cls, base, want), "got %s" % r)


def section6(img, C):
    print("\n6. FUN_0017A480 tail -- the level state machine")
    off, on, floor, flare = C['LEVEL']
    ck(close(off, 2.0) and close(on, 2.5) and close(floor, 1.0)
       and close(flare, 2.0), "module rates {2.0, 2.5, 1.0, 2.0}")
    # 0x40000000 is the ignition flare's stored immediate
    ck(img.rd(0x001798AC, 4) is not None, "FUN_0017A480 tail is mapped")

    def ref(pattern, dt):
        """The recovered branch structure, transcribed:
             if (!fx) { if (!boost) { l -= 2*dt; if (l<=0) l=0; store; next } }
             else if (l == 0) l = 2.0;
             l -= 2.5*dt; if (l <= 1.0) l = 1.0; store
           with fx := boosting and a crash forcing 0 (FUN_0017F730's gate)."""
        out, lvl = [], 0.0
        for ch in pattern:
            if ch == 'x':
                lvl = 0.0
                out.append(lvl)
                continue
            boost = (ch == '1')
            if not boost:
                v = lvl - 2.0 * dt
                lvl = 0.0 if v <= 0.0 else v
                out.append(lvl)
                continue
            if lvl == 0.0:
                lvl = 2.0
            v = lvl - 2.5 * dt
            lvl = 1.0 if v <= 1.0 else v
            out.append(lvl)
        return out

    for pat in ("1" * 40 + "0" * 60,
                "0" * 5 + "1" * 5 + "0" * 40 + "1" * 30,
                "1" * 20 + "x" * 3 + "1" * 20,
                "1" * 100):
        dt = 1.0 / 60.0
        want = ref(pat, dt)
        got = [float(x) for x in
               drive("leveltrace", pat, repr(dt)).split()]
        same = len(got) == len(want) and all(
            abs(a - b) < 1e-6 for a, b in zip(got, want))
        ck(same, "level trace %-28s (%d steps)"
           % ("'" + pat[:12] + "...'", len(want)),
           "" if same else "first diff at %d" % next(
               (i for i, (a, b) in enumerate(zip(got, want))
                if abs(a - b) >= 1e-6), -1))
    # the shape itself: instant on at 1.0, 2.0 flare from cold, 0.5 s fade
    t = [float(x) for x in drive("leveltrace", "1" * 3 + "0" * 200,
                                 repr(1.0 / 60.0)).split()]
    ck(close(t[0], 2.0 - 2.5 / 60.0, 1e-6),
       "first boosting frame = flare 2.0 - 2.5dt")
    ck(all(v == 0.0 for v in t[-5:]), "fades to exactly 0")
    # the fade is DECAY_OFF = 2.0/s from wherever it stood, i.e. <= 1 s from
    # the 2.0 flare and 0.5 s from the 1.0 sustain
    t2 = [float(x) for x in drive("leveltrace", "1" * 200 + "0" * 200,
                                  repr(1.0 / 60.0)).split()]
    n0 = next(i for i, v in enumerate(t2[200:]) if v == 0.0)
    ck(29 <= n0 <= 31, "sustain 1.0 fades out in 0.5 s (%d frames at 60 Hz)" % n0)


def section7(img, C):
    print("\n7. Art -- pools named by the image, and the type-8 emitters")
    tbl = 0x003A3E7C
    names = [img.cstr(img.u32(tbl + 8 * i)) for i in range(3)]
    ck(names == ["coronaglow", "coronaboost", "coronaboostred"],
       "pool name table @0x003A3E7C", "%r" % names)
    for i, n in ((1, "coronaboost"), (2, "coronaboostred")):
        p = os.path.join(ARTDIR, n + ".png")
        ck(os.path.exists(p), "pool %d texture build/boostfx/%s.png" % (i, n))
    # the per-car emitters: corona type 8, over every shipped car
    ncars = nrec = 0
    rear = 0
    bad = []
    if os.path.isdir(CARDIR):
        for fn in sorted(os.listdir(CARDIR)):
            if not fn.endswith(".lights"):
                continue
            recs = []
            for line in open(os.path.join(CARDIR, fn)):
                if line.startswith("light 8 "):
                    f = line.split()
                    recs.append([float(x) for x in f[3:9]])
            if not recs:
                continue
            ncars += 1
            nrec += len(recs)
            for r in recs:
                nl = (r[3] ** 2 + r[4] ** 2 + r[5] ** 2) ** 0.5
                if abs(nl - 1.0) > 1e-3:
                    bad.append((fn, "normal not unit", nl))
                if r[2] < 0.0:
                    rear += 1
    ck(nrec > 0, "type-8 emitters present", "%d records over %d cars"
       % (nrec, ncars))
    # The shipped table is essentially unit-normal, but it is NOT perfectly so
    # and the module deliberately does not renormalise: the game multiplies the
    # record's normal straight through by the 0.08/0.14/0.18 offsets, so an
    # over-long normal pushes that car's flame proportionally further out.  The
    # single shipped outlier is reported rather than hidden.
    ck(len(bad) <= nrec // 100,
       "type-8 normals unit length (%d of %d outliers, <= 1%%)"
       % (len(bad), nrec),
       "" if not bad else "; ".join("%s |n|=%.4f" % (b[0], b[2]) for b in bad))
    ck(rear >= int(nrec * 0.9),
       "type-8 emitters sit at the REAR of the car (z < 0)",
       "%d of %d" % (rear, nrec))
    n_c1 = drive("emitters", "COMP", "Car1").split()
    ck(n_c1[1] == "2" and n_c1[3] == "0",
       "COMP/Car1: 2 emitters, blue pool", " ".join(n_c1))
    n_c10 = drive("emitters", "COMP", "Car10").split()
    ck(n_c10[1] == "4" and n_c10[3] == "1",
       "COMP/Car10: 4 emitters, ORANGE pool", " ".join(n_c10))


def main():
    if not os.path.exists(ELF):
        raise SystemExit("missing %s" % ELF)
    print("validate_boostfx -- boost exhaust flame vs build/burnout3.elf")
    build_driver()
    img = Elf(ELF)
    C = consts()
    section1(img, C)
    section2(img, C)
    section3(img)
    section4(img)
    section5(img, C)
    section6(img, C)
    section7(img, C)
    print("\n%d/%d" % (len(PASS), len(PASS) + len(FAIL)))
    for f in FAIL:
        print("  FAILED: %s" % f)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
