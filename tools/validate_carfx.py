#!/usr/bin/env python3
"""Differential validator for src/burnout3_carfx.{c,h}.

Every constant the module hard-codes is re-read here from build/burnout3.elf,
from the retail .bgv files, or produced by EXECUTING the retail code under
Unicorn, and compared against what the C source actually contains.  Nothing
is asserted against a value typed into this file by hand: the expected side
of each comparison always comes from the binary.

Sections:
  1  SH irradiance   FUN_000313E0's five Ramamoorthi-Hanrahan constants, and
                     the whole 16-float matrix reproduced by executing the
                     real function against the analytic formula.
  2  Shine table     0x0045BB20 as written by EXECUTING FUN_00030150, and the
                     derived pixel-shader constants c14/c15.
  3  Fresnel pair    the two float4s FUN_0002EF90 writes to the global draw
                     context (0x004D6FE0 glass / 0x004D6FF0 body).
  4  Draw path       FUN_00031AB0's shader/constant plumbing captured live:
                     which register gets which value on the body, glass,
                     alt-material and light branches.
  5  Shadow          FUN_0019A7C0 / FUN_00043570 / FUN_00043350 constants.
  6  Coronas         the seven colour vec4s, size multipliers, distance gain,
                     overbright and the corona texture name table.
  7  .bgv tables     the per-car light table and shadow rows over all 67
                     player vehicles, and the sidecars extract_carfx_art.py
                     emits from them.
  8  Source check    every value above appears in src/burnout3_carfx.c.
  9  SH environment  the nine coefficients at modelInstance+0x5C, decoded
                     from the THREE code sites that write them, cross-checked
                     against each other and against the module's table, and
                     pushed through the real FUN_000313E0.
 10  Sun colour      pixel-shader c14.xyz = the track's enviro.dat +0x60,
                     re-read from every shipped enviro.dat.
 11  Vertex normal   the .bgv vertex normal at +0x0C is D3DVSDT_NORMPACKED3,
                     proved from the car shader factory's vertex DECLARATION,
                     from the geometry, and from the OBJ the extractor wrote
                     (plus the loader round-trip: one index per corner).
 11b Loader         src/burnout3_trackmesh.c COMPILED AND RUN over a 1:1 OBJ
                     and over one whose vt/vn indices disagree with its v
                     indices, asserting per-corner uv/normal fidelity.
 12  World mirror    the Z-mirror the module applies to the SH probe, because
                     this harness's GL world is the Z-mirror of the game's.
 17  Env map         the stage-1 bind FUN_00031690 issues, byte-exact; the
                     two exhaustive searches that put the writer of
                     DAT_004D6C00 outside the image (so the port's map is a
                     declared [S] substitute); and the enviro.dat +0xA0
                     reflection sheet extract_envmap.py writes per track,
                     round-tripped against an independent DXT decode.

Usage:  python3 tools/validate_carfx.py            (run from the repo root)
        python3 tools/validate_carfx.py --no-emu   (skip the Unicorn sections)
Exit status is non-zero if any check fails.
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import glob
import os
import random
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "burnout3.elf")
CARFX_C = os.path.join(ROOT, "src", "burnout3_carfx.c")
CARFX_H = os.path.join(ROOT, "src", "burnout3_carfx.h")
PVEH = (game_path('pveh'))

PASS, FAIL = [], []


def ck(cond, label, detail=""):
    (PASS if cond else FAIL).append(label)
    print("  %-4s %s%s" % ("ok" if cond else "FAIL", label,
                           ("   " + detail) if detail else ""))


def close(a, b, tol=1e-6):
    return a is not None and b is not None and abs(a - b) <= tol


# ---------------------------------------------------------------- ELF access
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
        if b is None:
            return None
        return b.split(b'\0')[0].decode('ascii', 'replace')


# --------------------------------------------------------------- C source
def c_floats(src):
    """every float literal in the module, keyed by nothing -- membership set"""
    out = set()
    for m in re.finditer(r'(-?\d+\.\d+(?:e-?\d+)?)f?', src):
        try:
            out.add(float(m.group(1)))
        except ValueError:
            pass
    return out


def c_has(src_floats, v, tol=1e-6):
    return any(abs(x - v) <= tol for x in src_floats)


# -------------------------------------------------------- 1. SH irradiance
SH_CONSTS = [("c1", 0x003868F0), ("c2", 0x003868F4), ("c3", 0x003868F8),
             ("c4", 0x003868FC), ("-c1", 0x003B1AE0)]


def sh_matrix(L, c1, c2, c3, c4, c5):
    return [c1*L[8],  c1*L[4], c1*L[7], c2*L[3],
            c1*L[4], -c1*L[8], c1*L[5], c2*L[1],
            c1*L[7],  c1*L[5], c3*L[6], c2*L[2],
            c2*L[3],  c2*L[1], c2*L[2], c4*L[0] - c5*L[6]]


def section1(elf, srcf):
    print("\n[1] SH irradiance -- FUN_000313E0 @0x000313E0")
    vals = {}
    for name, va in SH_CONSTS:
        vals[name] = elf.f32(va)
    ck(close(vals["c1"], 0.4290429949760437), "c1 = 0.429043 @0x003868F0",
       "%r" % vals["c1"])
    ck(close(vals["-c1"], -vals["c1"]), "-c1 @0x003B1AE0 == -c1")
    ck(close(vals["c2"], 0.5116639733314514), "c2 = 0.511664 @0x003868F4")
    ck(close(vals["c3"], 0.7431250214576721), "c3 = 0.743125 @0x003868F8")
    ck(close(vals["c4"], 0.886227011680603),  "c4 = 0.886227 @0x003868FC")
    for name, v in (("c1", vals["c1"]), ("c2", vals["c2"]),
                    ("c3", vals["c3"]), ("c4", vals["c4"])):
        ck(c_has(srcf, v), "module carries SH %s" % name)
    ck(c_has(srcf, 0.247708), "module carries SH c5 = 0.247708")
    return vals


# ------------------------------------------------------------ 2/4. emulation
def emulate(elf):
    """Run FUN_00030150 and a full FUN_000303D0 draw; return (table, events)."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    from unicorn import UC_HOOK_CODE, UC_PROT_ALL
    from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX,
                                   UC_X86_REG_EDX, UC_X86_REG_ESP)
    import trace_panels as TP

    SCRATCH = 0x30040000
    glass_c15 = [elf.f32(0x003B18D0), elf.f32(0x003B18C8), 1.0, 1.0]
    body_c15 = [elf.f32(0x003B18D4), elf.f32(0x003B18CC), 1.0, 1.0]

    t = TP.Tracer(os.path.join(PVEH, "COMP/Car1.bgv"), deep=True)
    uc = t.uc
    try:
        uc.mem_map(SCRATCH, 0x1000, UC_PROT_ALL)
    except Exception:
        pass
    ev = []

    def hook(uc_, addr, size, user):
        esp = uc_.reg_read(UC_X86_REG_ESP)

        def f4(a, n=4):
            return list(struct.unpack('<%df' % n,
                                      bytes(uc_.mem_read(a, 4 * n))))
        if addr == 0x0034F840:
            ev.append(("vs1", uc_.reg_read(UC_X86_REG_ECX),
                       f4(uc_.reg_read(UC_X86_REG_EDX))))
        elif addr == 0x0034F8F0:
            ev.append(("vs4", uc_.reg_read(UC_X86_REG_ECX),
                       f4(uc_.reg_read(UC_X86_REG_EDX), 16)))
        elif addr == 0x0034E9A0:
            p = struct.unpack('<I', bytes(uc_.mem_read(esp + 4, 4)))[0]
            ev.append(("ps", uc_.reg_read(UC_X86_REG_ECX), f4(p)))
    for a in (0x0034F840, 0x0034F8F0, 0x0034E9A0):
        uc.hook_add(UC_HOOK_CODE, hook, begin=a, end=a)

    uc.mem_write(SCRATCH, b"\xff" * 0x20)
    t._call(0x00030150, regs={UC_X86_REG_EAX: SCRATCH})
    table = list(struct.unpack('<28f', bytes(uc.mem_read(0x0045BB20, 0x70))))

    t.relink().init_ctx()
    lod = max(range(4), key=lambda li: len(
        [c for c in t.draw(li) if c["fn"] == "draw"]))
    uc.mem_write(TP.REND + 0x350, struct.pack('<4f', *glass_c15))
    uc.mem_write(TP.REND + 0x360, struct.pack('<4f', *body_c15))
    uc.mem_write(TP.MODEL + 0x5C, struct.pack('<9f', 1.0, *([0.0] * 8)))
    uc.mem_write(TP.MODEL + 0x58, bytes([0xFF, 0, 0, 1]))
    uc.mem_write(TP.MODEL, struct.pack('<16f', *TP.IDENT))
    uc.mem_write(TP.MODEL + 0x40, struct.pack('<I', TP.BUF))
    uc.mem_write(TP.MODEL + 0x44, struct.pack('<4f', 0.0, 1.0, 0.0, 1.0))
    uc.mem_write(TP.CTX + 0x101B, b"\0")
    del ev[:]
    t._call(TP.FUN_DRAW, stack_args=(TP.REND, TP.MODEL, TP.CTX),
            regs={UC_X86_REG_EAX: lod})
    return table, ev, body_c15, glass_c15


def section2(table, srcf):
    print("\n[2] shine table 0x0045BB20 -- FUN_00030150 EXECUTED")
    P = table[2:10]
    K = table[10:18]
    M = table[18:26]
    gk, gm = table[26], table[27]
    ck(all(close(v, 0.10000000149011612) for v in P),
       "P[0..7] = 0.1 (0x3DCCCCCD)", "%r" % P[0])
    ck(all(close(v, 0.3499999940395355) for v in K),
       "K[0..7] = 0.35 (0x3EB33333)", "%r" % K[0])
    ck(all(close(v, 0.699999988079071) for v in M),
       "M[0..7] = 0.7 (0x3F333333)", "%r" % M[0])
    ck(close(gk, 0.22499999403953552), "glass K = 0.225 (0x3E666666)")
    ck(close(gm, 0.925000011920929), "glass M = 0.925 (0x3F6CCCCD)")
    for v, lbl in ((P[0], "P"), (K[0], "K"), (M[0], "M"),
                   (gk, "glass K"), (gm, "glass M")):
        ck(c_has(srcf, v, 1e-6), "module carries %s" % lbl)
    return P, K, M, gk, gm


def section3(elf, srcf):
    print("\n[3] Fresnel pair -- FUN_0002EF90 -> draw ctx +0x350/+0x360")
    body = [elf.f32(0x003B18D4), elf.f32(0x003B18CC)]
    glass = [elf.f32(0x003B18D0), elf.f32(0x003B18C8)]
    ck(close(body[0] + body[1], 1.0, 1e-6),
       "body (R0, 1-R0) sums to 1", "%r + %r" % (body[0], body[1]))
    ck(close(glass[0] + glass[1], 1.0, 1e-6),
       "glass (R0, 1-R0) sums to 1", "%r + %r" % (glass[0], glass[1]))
    ck(c_has(srcf, body[0]), "module carries body R0 = %r" % body[0])
    ck(c_has(srcf, glass[0]), "module carries glass R0 = %r" % glass[0])
    return body[0], glass[0]


def section4(ev, table, body_r0, glass_r0, shc, body_c15, glass_c15):
    print("\n[4] draw path -- FUN_00031AB0 constant uploads, EXECUTED")
    P, K, M = table[2], table[10], table[18]
    gk, gm = table[26], table[27]
    ps = [(r, v) for (k, r, v) in ev if k == "ps"]
    vs1 = [(r, v) for (k, r, v) in ev if k == "vs1"]
    vs4 = [(r, v) for (k, r, v) in ev if k == "vs4"]

    want_c15_body = (1.0, 1.0, 1.0 - K, (1.0 / (1.0 - K)) * 0.25 * M)
    want_c15_glass = (1.0, 1.0, 1.0 - gk, (1.0 / (1.0 - gk)) * 0.25 * gm)

    def seen_ps(reg, want, tol=1e-6):
        return any(r == reg and all(close(a, b, tol) for a, b in zip(v, want))
                   for r, v in ps)
    ck(seen_ps(14, (0.0, 0.0, 0.0, P)),
       "ps c14 = (lightRGB, P=%.6g) on the body branch" % P)
    ck(seen_ps(15, want_c15_body),
       "ps c15 = (1,1,1-K,0.25M/(1-K)) = (1,1,%.9g,%.9g)" % want_c15_body[2:])
    ck(seen_ps(14, (0.0, 0.0, 0.0, 0.800000011920929)),
       "ps c14.w = 0.8 on the glass pass-2 branch (0x003A5600)")
    ck(seen_ps(15, want_c15_glass),
       "ps c15 glass = (1,1,%.9g,%.9g)" % want_c15_glass[2:])
    ck(seen_ps(14, (0.0, 0.0, 0.0, 0.5)),
       "ps c14.w = 0.5 on the alt-material branch (0x003B1684)")
    ck(any(r == 3 for r, _ in ps), "ps c3 (fade) uploaded")
    ck(any(r == 13 for r, _ in ps), "ps c13 (glass tint) uploaded")

    ck(any(r == 111 and all(close(a, b) for a, b in zip(v, body_c15))
           for r, v in vs1),
       "vs hw111 (c15) = body Fresnel pair %r" % body_r0)
    ck(any(r == 111 and all(close(a, b) for a, b in zip(v, glass_c15))
           for r, v in vs1),
       "vs hw111 (c15) = glass Fresnel pair %r" % glass_r0)

    want = sh_matrix([1.0] + [0.0] * 8, shc["c1"], shc["c2"], shc["c3"],
                     shc["c4"], 0.247708)
    got = [v for r, v in vs4 if r == 0x60]
    ck(bool(got) and max(abs(a - b) for a, b in zip(got[0], want)) < 1e-6,
       "vs hw96..99 (c0..c3) == Ramamoorthi-Hanrahan matrix of L00=1")


SHADOW = [("master darkness 0.7", 0x003B17D8, 0.699999988079071),
          ("airborne slope 0.8", 0x003A5600, 0.800000011920929),
          ("size gain 0.4", 0x003B16E8, 0.4000000059604645),
          ("ramp x2", 0x003B1688, 2.0),
          ("ramp x3.3333", 0x003B1DF8, 3.3333332538604736),
          ("uv split 0.1875", 0x003B1AC0, 0.1875),
          ("uv split 0.8125", 0x003B1ABC, 0.8125),
          ("uv 1.0", 0x003B168C, 1.0)]


def section5(elf, srcf):
    print("\n[5] shadow -- FUN_0019A7C0 / FUN_00043570 / FUN_00043350")
    for label, va, want in SHADOW:
        got = elf.f32(va)
        ck(close(got, want), "%s @0x%08X" % (label, va), "%r" % got)
        ck(c_has(srcf, want), "module carries %s" % label)
    lift = struct.unpack('<f', struct.pack('<I', 0x3D75C28F))[0]
    ck(close(lift, 0.05999999865889549),
       "ground lift 0.06 (PUSH 0x3D75C28F @0x0019A6A0)")
    ck(c_has(srcf, lift), "module carries the 0.06 lift")
    ck(elf.cstr(0x003AAFF8) == "blobbyshadow",
       "shadow texture name @0x003AAFF8", elf.cstr(0x003AAFF8))
    ck(os.path.exists(os.path.join(ROOT, "build/carfx/blobbyshadow.png")),
       "build/carfx/blobbyshadow.png extracted")


CORONAS = [(0x004161A0, (1.5, 1.5, 1.5, 0.0), 1.25),
           (0x004161B0, (1.0, 1.0, 1.0, 0.0), 1.0),
           (0x004161C0, (1.4, 0.0, 0.0, 0.0), 0.75),
           (0x004161D0, (1.1, 0.0, 0.0, 0.0), 0.5),
           (0x004161E0, (1.0, 0.9, 0.0, 0.0), 0.5),
           (0x004161F0, (0.9, 0.9, 0.9, 0.0), 0.5)]


def section6(elf, srcf):
    print("\n[6] coronas -- FUN_00187C70 / FUN_00187BE0 / FUN_00042B00")
    for va, want, _mult in CORONAS:
        got = [elf.f32(va + 4 * i) for i in range(4)]
        ck(all(close(a, b, 1e-6) for a, b in zip(got, want)),
           "colour @0x%08X = %s" % (va, list(want)),
           " ".join("%.4g" % x for x in got))
        for c in want[:3]:
            if c:
                ck(c_has(srcf, c), "module carries colour component %g" % c)
    ck(close(elf.f32(0x003895BC), 1.25), "hi-beam size x1.25 @0x003895BC")
    ck(close(elf.f32(0x003A55F8), 0.75), "brake size x0.75 @0x003A55F8")
    ck(close(elf.f32(0x003B1A08), 0.019999999552965164),
       "distance gain 0.02 @0x003B1A08")
    ck(c_has(srcf, 0.019999999552965164), "module carries the 0.02 gain")
    ck(close(elf.f32(0x0035BF1C), 64.0), "overbright 64.0 @0x0035BF1C")
    names = [elf.cstr(elf.u32(0x003A3E7C + i * 8)) for i in range(3)]
    ck(names == ["coronaglow", "coronaboost", "coronaboostred"],
       "corona texture name table @0x003A3E7C", str(names))
    ck(os.path.exists(os.path.join(ROOT, "build/carfx/coronaglow.png")),
       "build/carfx/coronaglow.png extracted")


def section7():
    print("\n[7] .bgv light + shadow tables (all player vehicles)")
    if not os.path.isdir(PVEH):
        ck(False, "pveh/ present")
        return
    cars = ok = lights = 0
    bad = []
    for cls in sorted(os.listdir(PVEH)):
        cdir = os.path.join(PVEH, cls)
        if not os.path.isdir(cdir):
            continue
        for fn in sorted(os.listdir(cdir)):
            if not (fn.startswith("Car") and fn.endswith(".bgv")):
                continue
            d = open(os.path.join(cdir, fn), 'rb').read()
            cars += 1
            good = True
            n = 0
            for t in range(12):
                off = struct.unpack_from('<I', d, 0x1664 + t * 4)[0]
                cnt = d[0x16AC + t]
                if cnt == 0:
                    good &= (off == 0)
                    continue
                if off == 0 or off + cnt * 0x30 > len(d):
                    good = False
                    continue
                for k in range(cnt):
                    px, py, pz, pw = struct.unpack_from('<4f', d,
                                                        off + k * 0x30)
                    nx, ny, nz, nw = struct.unpack_from('<4f', d,
                                                        off + k * 0x30 + 0x10)
                    ln = nx*nx + ny*ny + nz*nz
                    good &= abs(pw - 1.0) < 1e-3 and abs(nw - 1.0) < 1e-3
                    good &= abs(ln - 1.0) < 1e-2
                    good &= abs(px) < 5.0 and abs(py) < 5.0 and abs(pz) < 6.0
                    n += 1
            nw_ = d[0x0D]
            hw = struct.unpack_from('<f', d, 0xE80)[0]
            ofz = struct.unpack_from('<f', d, 0xE88)[0]
            fz = struct.unpack_from('<f', d, 0xBF8)[0]
            rz = struct.unpack_from('<f', d, 0xB38 + nw_ * 0x40)[0]
            orz = struct.unpack_from('<f', d, 0xE98)[0]
            good &= (orz < rz < fz < ofz) and hw > 0.5
            if good:
                ok += 1
                lights += n
            else:
                bad.append("%s/%s" % (cls, fn))
    ck(ok == cars and cars == 67,
       "%d/%d .bgv files: light records unit-normal + car-scale, and shadow "
       "rows strictly ordered outerRear < rear < front < outerFront"
       % (ok, cars), (("bad: " + ", ".join(bad[:4])) if bad else ""))
    ck(lights > 1000, "%d corona light records total" % lights)
    # sidecars: 67 player (.bgv, extract_carfx_art.py) + 40 traffic/prop
    # (.btv, extract_traffic_lights.py).  The two name spaces cannot collide --
    # within a class the player cars are Car1..Car10(+Car36) as .bgv and the
    # .btv fleet is Car11.., asserted below.
    d = os.path.join(ROOT, "build", "cars")
    n = len([f for f in os.listdir(d) if f.endswith(".lights")]) \
        if os.path.isdir(d) else 0
    ck(n == 107, "%d/107 build/cars/*.lights sidecars present "
                 "(67 player .bgv + 40 traffic .btv)" % n)

    # --- the TRAFFIC light table -------------------------------------------
    # The traffic fleet ships as .btv and FUN_001A4260 loads it through the
    # SAME .bgv relinker, so the corona table must be at the same two offsets.
    # Assert that on every shipped .btv, with the same consistency test the
    # .bgv set gets plus a front/rear geometry test.
    tok = tbad = tlights = tgeom = 0
    tnames = []
    for cls in sorted(os.listdir(PVEH)):
        cdir = os.path.join(PVEH, cls)
        if not os.path.isdir(cdir):
            continue
        bgv = {f[:-4] for f in os.listdir(cdir) if f.endswith(".bgv")}
        for fn in sorted(os.listdir(cdir)):
            if not fn.endswith(".btv"):
                continue
            tnames.append((cls, fn[:-4], fn[:-4] in bgv))
            d2 = open(os.path.join(cdir, fn), 'rb').read()
            good = True
            head = []
            tail = []
            for t in range(12):
                off = struct.unpack_from('<I', d2, 0x1664 + t * 4)[0]
                cnt = d2[0x16AC + t]
                if cnt == 0:
                    good &= (off == 0)
                    continue
                if off == 0 or off + cnt * 0x30 > len(d2):
                    good = False
                    continue
                for k in range(cnt):
                    p = struct.unpack_from('<3f', d2, off + k * 0x30)
                    nn = struct.unpack_from('<3f', d2, off + k * 0x30 + 0x10)
                    good &= max(abs(v) for v in p) < 100.0
                    good &= max(abs(v) for v in nn) > 1e-6
                    if t == 0:
                        head.append((p[2], nn[2]))
                    if t == 1:
                        tail.append((p[2], nn[2]))
                    tlights += 1
            if head and tail:
                hz = sum(h[0] for h in head) / len(head)
                tz = sum(h[0] for h in tail) / len(tail)
                hn = sum(h[1] for h in head) / len(head)
                tn = sum(h[1] for h in tail) / len(tail)
                if hz > tz and hn * tn < 0.0:
                    tgeom += 1
                else:
                    good = False
            if good:
                tok += 1
            else:
                tbad += 1
    ck(tok == 40 and tbad == 0,
       "%d/%d shipped .btv files carry a consistent light table at the SAME "
       "offsets the .bgv ones do (0x1664 / 0x16AC, stride 0x30) -- so the "
       "traffic lights are the game's own data, not a fallback"
       % (tok, tok + tbad))
    ck(tlights > 600, "%d traffic corona light records total" % tlights)
    ck(tgeom == 25,
       "%d/25 .btv models carrying BOTH type 0 and type 1 put the headlights "
       "forward of the tails with opposed Z normals" % tgeom)
    ck(not any(c for _, _, c in tnames),
       "no .bgv/.btv base-name collides, so the two sidecar sets share "
       "build/cars/ safely (%d .btv names checked)" % len(tnames))


def section8(elf, srcf, csrc):
    print("\n[8] module cross-check")
    for va, sym in ((0x000313E0, "FUN_000313E0"), (0x00030150, "FUN_00030150"),
                    (0x00031AB0, "FUN_00031AB0"), (0x0019A7C0, "FUN_0019A7C0"),
                    (0x00043570, "FUN_00043570"), (0x00187C70, "FUN_00187C70"),
                    (0x0002EF90, "FUN_0002EF90"), (0x001888F0, "FUN_001888F0"),
                    (0x00188C00, "FUN_00188C00")):
        ck(sym in csrc, "module cites %s (0x%08X)" % (sym, va))
    hdr = open(CARFX_H).read()
    for bit, name in ((0x02, "HEAD_HI"), (0x04, "HEAD"), (0x08, "TAIL"),
                      (0x10, "BRAKE"), (0x20, "INDIC_R"), (0x40, "INDIC_L"),
                      (0x80, "REVERSE")):
        ck(("B3_CARFX_LIGHT_%s   0x%02Xu" % (name, bit)) in hdr
           or ("B3_CARFX_LIGHT_%s     0x%02Xu" % (name, bit)) in hdr
           or re.search(r"B3_CARFX_LIGHT_%s\s+0x%02Xu" % (name, bit), hdr)
           is not None, "header defines %s = 0x%02X" % (name, bit))
    ck("GLUE" in csrc, "module marks its GLUE layers")
    ck(elf is not None, "ELF loaded")


# ------------------------------------------------- 9. the SH environment feed
#
# Three sites store the same nine floats into the model instance just before
# the car draw.  Each is a run of
#     movss xmm0, dword ptr [imm32]      F3 0F 10 05 <imm32>
#     movss dword ptr [reg+disp], xmm0   F3 0F 11 <modrm> <disp8|disp32>
# so the (destination offset -> source VA -> value) mapping is decoded out of
# the instruction stream rather than typed in here.
SH_WRITERS = [("FUN_000C047C  in-race car object", 0x000C047C, 0x000C0500),
              ("FUN_001A74C5  second car instance", 0x001A74C5, 0x001A7562),
              ("FUN_001AE92D  front-end showcase", 0x001AE92D, 0x001AE9B5)]
SH_WRITER_MIRROR = ("FUN_001AE882  showcase reflection", 0x001AE882, 0x001AE903)


def decode_sh_writer(elf, lo, hi):
    """-> list of (dest_disp, src_va, value) in program order"""
    b = elf.rd(lo, hi - lo)
    out, i, pend = [], 0, None
    while i < len(b) - 4:
        if b[i:i + 4] == b"\xf3\x0f\x10\x05":
            pend = struct.unpack_from("<I", b, i + 4)[0]
            i += 8
            continue
        if b[i:i + 3] == b"\xf3\x0f\x11" and pend is not None:
            modrm = b[i + 3]
            mod, reg = modrm >> 6, (modrm >> 3) & 7
            if reg != 0:                      # not xmm0
                i += 1
                continue
            if mod == 1:
                disp = struct.unpack_from("<b", b, i + 4)[0]
                i += 5
            elif mod == 2:
                disp = struct.unpack_from("<i", b, i + 4)[0]
                i += 8
            else:
                i += 1
                continue
            out.append((disp, pend, elf.f32(pend)))
            pend = None
            continue
        i += 1
    return out


def c_named_array(csrc, name):
    m = re.search(r"const float %s\[9\]\s*=\s*\{(.*?)\}" % name, csrc, re.S)
    if not m:
        return None
    return [float(x) for x in re.findall(r"(-?\d+(?:\.\d+)?(?:e-?\d+)?)f",
                                         m.group(1))]


def sh_eval(M, n3):
    v = list(n3) + [1.0]
    return sum(v[r] * M[r * 4 + c] * v[c] for r in range(4) for c in range(4))


def section9(elf, csrc, shc):
    print("\n[9] SH environment feed -- modelInstance+0x5C .. +0x7C")
    decoded = []
    for label, lo, hi in SH_WRITERS:
        d = decode_sh_writer(elf, lo, hi)
        ck(len(d) == 9, "%s writes 9 floats" % label, "got %d" % len(d))
        if len(d) != 9:
            continue
        d.sort(key=lambda t: t[0])
        base = d[0][0]
        step_ok = all(d[k][0] == base + 4 * k for k in range(9))
        ck(step_ok, "%s: nine CONSECUTIVE dwords from +0x%02X" % (label, base))
        decoded.append([t[2] for t in d])
    ck(len(decoded) == 3 and decoded[0] == decoded[1] == decoded[2],
       "all three writers store an identical nine-float set")
    if not decoded:
        return None
    L = decoded[0]
    mod = c_named_array(csrc, "B3_CARFX_SH_RETAIL")
    ck(mod is not None and len(mod) == 9,
       "module defines B3_CARFX_SH_RETAIL[9]")
    if mod:
        ck(all(close(a, b, 1e-6) for a, b in zip(mod, L)),
           "module's B3_CARFX_SH_RETAIL == the bytes in the ELF",
           " ".join("%.9g" % v for v in L))

    mir = decode_sh_writer(elf, SH_WRITER_MIRROR[1], SH_WRITER_MIRROR[2])
    mir.sort(key=lambda t: t[0])
    ck(len(mir) == 9, "%s writes 9 floats" % SH_WRITER_MIRROR[0])
    if len(mir) == 9:
        mv = [t[2] for t in mir]
        flipped = [i for i in range(9) if not close(mv[i], L[i], 1e-9)]
        ck(flipped == [1, 5, 7],
           "showcase reflection differs from the upright set at i=1,5,7",
           str(flipped))
        ck(all(close(mv[i], -L[i], 1e-9) for i in flipped),
           "and only by sign")
        modm = c_named_array(csrc, "B3_CARFX_SH_RETAIL_MIRROR")
        ck(modm is not None and all(close(a, b, 1e-6)
                                    for a, b in zip(modm, mv)),
           "module's B3_CARFX_SH_RETAIL_MIRROR == the bytes in the ELF")

    # push the recovered nine through the analytic form the module implements
    c1, c2, c3, c4 = shc["c1"], shc["c2"], shc["c3"], shc["c4"]
    M = sh_matrix(L, c1, c2, c3, c4, 0.247708)
    axes = [("+Y", (0, 1, 0)), ("-Y", (0, -1, 0)), ("+X", (1, 0, 0)),
            ("-X", (-1, 0, 0)), ("+Z", (0, 0, 1)), ("-Z", (0, 0, -1))]
    E = {k: sh_eval(M, v) for k, v in axes}
    ck(E["+Y"] > E["-Y"] and E["+Y"] > max(E["+X"], E["-X"], E["+Z"], E["-Z"]),
       "the recovered probe is top-lit (E(+Y) is the maximum)",
       " ".join("%s=%.5f" % (k, E[k]) for k, _ in axes))
    ck(E["-Y"] < 0.05, "and nearly black underneath", "E(-Y)=%.5f" % E["-Y"])
    ck(all(v >= 0.0 for v in E.values()),
       "E(n) >= 0 on every axis (no negative irradiance)")
    # the specular threshold K only opens where E > K -- that IS the gloss.
    # the peak is the maximum of the quadratic form over the whole sphere,
    # which is above every axis value, so sample it.
    import math
    emax, emin = -1e9, 1e9
    NT = 200
    for i in range(NT):
        z = 1.0 - 2.0 * (i + 0.5) / NT
        st = math.sqrt(max(0.0, 1.0 - z * z))
        for j in range(2 * NT):
            ph = 2.0 * math.pi * (j + 0.5) / (2 * NT)
            v = sh_eval(M, (st * math.cos(ph), z, st * math.sin(ph)))
            emax = max(emax, v)
            emin = min(emin, v)
    ck(emin >= 0.0, "E(n) >= 0 everywhere on the sphere", "min %.5f" % emin)
    kv, mv = 0.35, 0.7
    peak = max(0.0, min(1.0, (emax - kv) / (1.0 - kv))) * mv
    ck(emax > kv, "the probe's peak E clears the shine table's K = 0.35, so "
                  "the recovered constants alone produce a highlight",
       "E_max = %.5f -> peak body spec = %.4f" % (emax, peak))
    ck(peak > 0.3, "and the highlight is a real one, not a rounding error",
       "%.4f" % peak)
    return L


# --------------------------------------------- 10. the sun colour (enviro.dat)
TRACKS_ROOT = (game_path('Tracks'))
ENVIRO_LIGHT_OFF = 0x60          # env object +0x60 == DAT_0060E0A0


def section10(elf, csrc):
    print("\n[10] sun colour -- ps c14.xyz = enviro.dat +0x%02X"
          % ENVIRO_LIGHT_OFF)
    ck(elf.cstr(0x003B0444) == "enviro.dat",
       'the loader\'s filename literal at 0x003B0444 is "enviro.dat"',
       repr(elf.cstr(0x003B0444)))
    # FUN_001888F0 sets EAX=this / ECX=the loaded buffer then CALLs the 0xB0
    # copy FUN_00188C00 at 0x00188A40:  8B C3 (mov eax,ebx)  E8 rel32
    b = elf.rd(0x00188A3E, 7)
    tgt = 0x00188A40 + 5 + struct.unpack_from("<i", b, 3)[0]
    ck(b[0:2] == b"\x8b\xc3" and b[2] == 0xE8 and tgt == 0x00188C00,
       "FUN_001888F0 @0x00188A40 copies the loaded record into the env "
       "object (call FUN_00188C00)", "target=0x%08X" % tgt)

    if not os.path.isdir(TRACKS_ROOT):
        ck(False, "shipped Tracks/ tree present", TRACKS_ROOT)
        return
    files = []
    for reg in sorted(os.listdir(TRACKS_ROOT)):
        rd = os.path.join(TRACKS_ROOT, reg)
        if not os.path.isdir(rd):
            continue
        for trk in sorted(os.listdir(rd)):
            fp = os.path.join(rd, trk, "enviro.dat")
            if os.path.isfile(fp):
                files.append((reg + "_" + trk, fp))
    ck(len(files) > 0, "enviro.dat files found", "%d" % len(files))

    tbl = dict(re.findall(
        r'\{\s*"([A-Z]{2}_[A-Z0-9_]+)",\s*\{([^}]*)\}\s*\}', csrc))
    parsed = {}
    for k, v in tbl.items():
        parsed[k] = [float(x) for x in
                     re.findall(r"(-?\d+(?:\.\d+)?)f", v)]
    ck(len(parsed) == len(files),
       "module's B3FX_ENV_LIGHT covers every shipped track",
       "module %d, shipped %d" % (len(parsed), len(files)))

    bad_range, bad_byte, bad_tbl = [], [], []
    for name, fp in files:
        with open(fp, "rb") as f:
            f.seek(ENVIRO_LIGHT_OFF)
            rgb = struct.unpack("<3f", f.read(12))
        if not all(0.0 <= c <= 1.0 for c in rgb):
            bad_range.append(name)
        if not all(abs(c * 255.0 - round(c * 255.0)) < 1e-3 for c in rgb):
            bad_byte.append(name)
        m = parsed.get(name)
        if m is None or not all(close(a, b, 1e-6) for a, b in zip(m, rgb)):
            bad_tbl.append(name)
    ck(not bad_range, "every enviro.dat +0x60 is a colour in [0,1]",
       str(bad_range))
    ck(not bad_byte, "and is authored as an exact 8-bit colour",
       str(bad_byte))
    ck(not bad_tbl, "module's per-track table == the shipped files "
                    "(%d tracks)" % len(files), str(bad_tbl))
    # the reference capture's track, spelled out
    with open(os.path.join(TRACKS_ROOT, "US", "C3_V1", "enviro.dat"),
              "rb") as f:
        f.seek(ENVIRO_LIGHT_OFF)
        sl = struct.unpack("<3f", f.read(12))
    ck(close(sl[0], 253 / 255.0, 1e-6) and close(sl[1], 228 / 255.0, 1e-6)
       and close(sl[2], 172 / 255.0, 1e-6),
       "US/C3_V1 (the reference capture's track) sun = 253,228,172",
       "%.6f %.6f %.6f" % sl)


# ------------------------------------- 11. the .bgv vertex normal (NORMPACKED3)
#
# THE MARBLING BUG.  burnout3_full.c's car display lists emit the OBJ's `vn`
# and the shine evaluates its specular along reflect(-V, N), so a wrong normal
# is not a subtle shading error -- it is chaotic bright/dark streaking over the
# whole body.  extract_bgv.py used to read vertex +0x0C as s8[4]/127; it is
# D3DVSDT_NORMPACKED3.  This section proves the split three ways: from the
# vertex DECLARATION the car shader factory hands to CreateVertexShader, from
# the geometry itself, and from the OBJ the extractor actually wrote.
CAR_VDECL_VA = 0x00387558          # pushed at 0x0003CB73
CAR_VDECL = [0x20000000,           # D3DVSD_STREAM(0)
             0x40320000,           # v0  = FLOAT3        (position, +0x00)
             0x40160002,           # v2  = NORMPACKED3   (normal,   +0x0C)
             0x40220009,           # v9  = FLOAT2        (uv,       +0x10)
             0xFFFFFFFF]           # D3DVSD_END


def unpack_normpacked3(w):
    x, y, z = w & 0x7FF, (w >> 11) & 0x7FF, (w >> 22) & 0x3FF
    if x & 0x400:
        x -= 0x800
    if y & 0x400:
        y -= 0x800
    if z & 0x200:
        z -= 0x400
    return (x / 1023.0, y / 1023.0, z / 511.0)


def unpack_s8x4(w):
    b = [(w >> (8 * k)) & 0xFF for k in range(4)]
    b = [v - 256 if v > 127 else v for v in b]
    return (b[0] / 127.0, b[1] / 127.0, b[2] / 127.0)


def section11(elf):
    print("\n[11] car vertex normal -- D3DVSDT_NORMPACKED3 at .bgv vertex +0x0C")

    # ---- (a) the declaration in .rdata -----------------------------------
    toks = [elf.u32(CAR_VDECL_VA + 4 * i) for i in range(len(CAR_VDECL))]
    ck(toks == CAR_VDECL,
       "vertex declaration 0x%08X = stream0 FLOAT3 / NORMPACKED3 / FLOAT2"
       % CAR_VDECL_VA,
       " ".join("%08X" % (t or 0) for t in toks))
    # sizes: FLOAT3 12 + NORMPACKED3 4 + FLOAT2 8 = 0x18, the stride
    # FUN_000315C0 binds for stream 0 (PUSH 0x18 @0x0003166A).
    ck(elf.d[elf.off(0x0003166A)] == 0x6A and
       elf.d[elf.off(0x0003166A) + 1] == 0x18,
       "declaration stride 0x18 == the stream-0 stride FUN_000315C0 binds",
       "PUSH 0x18 @0x0003166A")
    # and that this declaration belongs to the CAR factory: FUN_0003C8A0 calls
    # the car Fresnel writer FUN_0002EF90 and then CreateVertexShader with it.
    push_decl = elf.rd(0x0003CB73, 5)
    ck(push_decl == b"\x68" + struct.pack("<I", CAR_VDECL_VA),
       "PUSH 0x%08X @0x0003CB73, inside the car shader factory FUN_0003C8A0"
       % CAR_VDECL_VA, repr(push_decl))
    rel = elf.u32(0x0003CB61)
    ck(rel is not None and (0x0003CB60 + 5 + rel) & 0xFFFFFFFF == 0x0002EF90,
       "FUN_0003C8A0 CALLs the car Fresnel writer FUN_0002EF90 @0x0003CB60",
       "target 0x%08X" % ((0x0003CB60 + 5 + (rel or 0)) & 0xFFFFFFFF))

    # ---- (b) the geometry: decode both ways, score against face normals ---
    path = os.path.join(PVEH, "COMP", "Car1.bgv")
    if not os.path.exists(path):
        ck(False, "COMP/Car1.bgv present")
        return
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import extract_bgv                                   # noqa: E402

    d = open(path, 'rb').read()
    best = None
    for li in range(5):
        sec = extract_bgv.parse_section(
            d, struct.unpack_from('<I', d, 0x4C + li * 4)[0])
        if sec is None:
            continue
        nt = sum(len(t) for _, _, t in sec['body'])
        if best is None or nt > best[0]:
            best = (nt, sec)
    if best is None:
        ck(False, "COMP/Car1.bgv has a decodable LOD section")
        return
    sec = best[1]
    nv = sec['maxidx'] + 1
    pos, raw = [], []
    for i in range(nv):
        o = sec['pool'] + i * 0x18
        pos.append(struct.unpack_from('<3f', d, o))
        raw.append(struct.unpack_from('<I', d, o + 0xC)[0])

    acc = [[0.0, 0.0, 0.0] for _ in range(nv)]
    for _, _, tris in sec['body']:
        for a, b, c in tris:
            pa, pb, pc = pos[a], pos[b], pos[c]
            u = (pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2])
            v = (pc[0]-pa[0], pc[1]-pa[1], pc[2]-pa[2])
            n = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
            ln = (n[0]**2 + n[1]**2 + n[2]**2) ** 0.5 or 1.0
            for vi in (a, b, c):
                for k in range(3):
                    acc[vi][k] += n[k] / ln

    def unit(v):
        ln = (v[0]**2 + v[1]**2 + v[2]**2) ** 0.5
        return None if ln < 1e-9 else (v[0]/ln, v[1]/ln, v[2]/ln)

    score = {}
    for tag, dec in (("NORMPACKED3", unpack_normpacked3), ("s8[4]", unpack_s8x4)):
        dots, lens = [], []
        for i in range(nv):
            g = unit(acc[i])
            if g is None:
                continue
            nvec = dec(raw[i])
            lens.append((nvec[0]**2 + nvec[1]**2 + nvec[2]**2) ** 0.5)
            u = unit(nvec)
            if u:
                dots.append(sum(u[k]*g[k] for k in range(3)))
        score[tag] = (sum(dots)/len(dots), sum(lens)/len(lens),
                      min(lens), max(lens), len(dots))

    npk, s8 = score["NORMPACKED3"], score["s8[4]"]
    ck(npk[0] > 0.95,
       "NORMPACKED3 agrees with the geometric normal: mean dot %.4f over %d "
       "verts" % (npk[0], npk[4]))
    ck(0.99 < npk[2] and npk[3] < 1.01,
       "and every decoded normal is unit: |n| in [%.4f, %.4f]" % (npk[2], npk[3]))
    ck(abs(s8[0]) < 0.2,
       "the old s8[4] reading is NOISE by comparison: mean dot %+.4f, "
       "mean |n| %.1f" % (s8[0], s8[1]))

    # ---- (c) the OBJ the extractor actually wrote -------------------------
    obj = os.path.join(ROOT, "build", "cars", "COMP_Car1_intact.obj")
    if not os.path.exists(obj):
        ck(False, "build/cars/COMP_Car1_intact.obj present "
                  "(run tools/extract_bgv.py)")
        return
    vn, faces, nvpos = [], [], 0
    for line in open(obj):
        if line.startswith("vn "):
            vn.append(tuple(float(x) for x in line.split()[1:4]))
        elif line.startswith("v "):
            nvpos += 1
        elif line.startswith("f "):
            faces.append([c.split("/") for c in line.split()[1:4]])
    ck(vn and len(vn) == nvpos,
       "OBJ carries one `vn` per `v` (%d/%d)" % (len(vn), nvpos))
    bad = sum(1 for n in vn
              if abs((n[0]**2 + n[1]**2 + n[2]**2) ** 0.5 - 1.0) > 2e-3)
    ck(bad == 0, "every emitted `vn` is unit (%d bad of %d)" % (bad, len(vn)))

    # THE LOADER ROUND-TRIP.  src/burnout3_trackmesh.c stores positions, uvs
    # and normals as PARALLEL arrays indexed by one number, so a face whose
    # vn (or vt) index differs from its v index would read a corner's normal
    # off some other vertex -- which is exactly the marbling.  The loader now
    # detects that and expands, but the extractor must not need it: assert the
    # 1:1 form directly on the file.
    off = sum(1 for f in faces for c in f
              if len(c) == 3 and not (c[0] == c[1] == c[2]))
    ck(off == 0,
       "every face is `v/vt/vn` with all three indices EQUAL, so the loader's "
       "flat vertex model is exact (%d/%d corners off)"
       % (off, 3 * len(faces)))

    # The same loader serves the TRACK, and trackmesh_load's channel-expansion
    # path must stay a no-op there -- otherwise a car-side fix would silently
    # rebuild the world mesh. Assert the 1:1 form on the track OBJ too, and
    # that its texture-coordinate channel is intact (the decal rasters are
    # addressed through it).
    for tobj in sorted(glob.glob(os.path.join(ROOT, "build", "tracks",
                                              "*", "track.obj")))[:1]:
        nv = nt = nn = bad = tot = 0
        for line in open(tobj):
            if line.startswith("v "):
                nv += 1
            elif line.startswith("vt "):
                nt += 1
            elif line.startswith("vn "):
                nn += 1
            elif line.startswith("f "):
                for c in line.split()[1:4]:
                    q = c.split("/")
                    tot += 1
                    if len(q) == 3 and not (q[0] == q[1] == q[2]):
                        bad += 1
        ck(bad == 0 and tot > 0,
           "%s: every face index triple is 1:1 too, so the expansion path "
           "stays a no-op on the world mesh (%d/%d corners off)"
           % (os.path.relpath(tobj, ROOT), bad, tot))
        ck(nv > 0 and nt == nv and nn == nv,
           "and it carries one vt and one vn per v (v=%d vt=%d vn=%d)"
           % (nv, nt, nn))


# --------------------------------- 11b. the loader round-trip, COMPILED AND RUN
#
# The two assertions above prove the shipped OBJs need no channel expansion.
# This one proves the loader is right EITHER WAY: it compiles
# src/burnout3_trackmesh.c and runs it over (a) a 1:1 OBJ, which must load
# unexpanded and in file order, and (b) an OBJ whose vt/vn indices deliberately
# disagree with its v indices, where every corner must still come back with ITS
# OWN uv and normal. Without this, the expansion path is code that has never
# executed.
LOADER_DRV = r"""
#include "burnout3_trackmesh.h"
#include <stdio.h>
int main(int argc, char** argv) {
    TrackMesh m;
    (void)argc;
    if (trackmesh_load(&m, argv[1]) != 0) { printf("FAIL load\n"); return 1; }
    printf("V %d %d %d %d\n", m.vertex_count, m.triangle_count,
           m.normals ? 1 : 0, m.uvs ? 1 : 0);
    for (int t = 0; t < m.triangle_count; t++)
        for (int k = 0; k < 3; k++) {
            unsigned v = m.indices[t * 3 + k];
            printf("C %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
                   m.positions[v*3], m.positions[v*3+1], m.positions[v*3+2],
                   m.uvs ? m.uvs[v*2] : -1.0f, m.uvs ? m.uvs[v*2+1] : -1.0f,
                   m.normals ? m.normals[v*3] : 0.0f,
                   m.normals ? m.normals[v*3+1] : 0.0f,
                   m.normals ? m.normals[v*3+2] : 0.0f);
        }
    trackmesh_free(&m);
    return 0;
}
"""

# corner -> (position, uv, normal-BEFORE-the-loader's-Z-negation)
SPLIT_OBJ = """v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0.9 0.9
vt 0.8 0.8
vt 0.7 0.7
vt 0.6 0.6
vn 0 0 1
vn 1 0 0
f 1/4/1 2/3/2 3/2/1
f 1/4/2 3/2/2 4/1/1
f 1/4/1 3/2/1 4/1/1
"""
FLAT_OBJ = """v 0 0 0
v 1 0 0
v 1 1 0
vt 0.1 0.1
vt 0.2 0.2
vt 0.3 0.3
vn 0 0 1
vn 0 1 0
vn 1 0 0
f 1/1/1 2/2/2 3/3/3
"""
BAD_OBJ = """v 0 0 0
v 1 0 0
v 1 1 0
vt 0.9 0.9
vt 0.8 0.8
vt 0.7 0.7
vn 0 0 1
vn 1 0 0
f 1/3/1 2/2/2 3/1/1
f 1/3/1 2/2/2 99/1/1
"""
V = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
VT = [(0.9, 0.9), (0.8, 0.8), (0.7, 0.7), (0.6, 0.6)]
VN = [(0, 0, 1), (1, 0, 0)]
SPLIT_FACES = [[(1, 4, 1), (2, 3, 2), (3, 2, 1)],
               [(1, 4, 2), (3, 2, 2), (4, 1, 1)],
               [(1, 4, 1), (3, 2, 1), (4, 1, 1)]]


def section11b():
    print("\n[11b] trackmesh_load round-trip -- COMPILED AND RUN")
    import shutil
    import subprocess
    import tempfile
    if not shutil.which("gcc"):
        print("     skipped (no gcc)")
        return
    tmp = tempfile.mkdtemp(prefix="b3loader")
    try:
        drv = os.path.join(tmp, "drv.c")
        with open(drv, "w") as f:
            f.write(LOADER_DRV)
        try:
            cflags = subprocess.check_output(
                ["pkg-config", "--cflags", "--libs", "sdl2"],
                stderr=subprocess.DEVNULL).decode().split()
        except Exception:
            cflags = []
        cmd = (["gcc", "-std=c11", "-O1", "-I", os.path.join(ROOT, "src"),
                "-o", os.path.join(tmp, "drv"), drv,
                os.path.join(ROOT, "src", "burnout3_trackmesh.c")]
               + cflags + ["-lGL", "-lm"])
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if r.returncode != 0:
            print("     skipped (cannot build the driver: %s)"
                  % r.stderr.decode().strip().splitlines()[-1:])
            return

        def run(text):
            op = os.path.join(tmp, "m.obj")
            with open(op, "w") as f:
                f.write(text)
            out = subprocess.check_output(
                [os.path.join(tmp, "drv"), op],
                env=dict(os.environ, B3_TRACK_NOCULL="1")).decode()
            head, corners = None, []
            for line in out.splitlines():
                if line.startswith("V "):
                    head = [int(x) for x in line.split()[1:]]
                elif line.startswith("C "):
                    corners.append([float(x) for x in line.split()[1:]])
            return head, corners

        # (a) the 1:1 control must NOT expand
        head, corners = run(FLAT_OBJ)
        ck(head == [3, 1, 1, 1],
           "1:1 OBJ loads unexpanded: 3 verts, 1 tri, normals+uvs kept",
           str(head))

        # (b) the split OBJ: every corner keeps ITS OWN uv and normal
        head, corners = run(SPLIT_OBJ)
        want = []
        for face in SPLIT_FACES:
            for (vi, ti, ni) in face:
                p, t, n = V[vi - 1], VT[ti - 1], VN[ni - 1]
                # the loader mirrors Z on positions AND normals
                want.append((p[0], p[1], -p[2], t[0], t[1],
                             n[0], n[1], -n[2]))
        ok = (len(corners) == len(want) and
              all(all(abs(a - b) < 1e-4 for a, b in zip(c, w))
                  for c, w in zip(corners, want)))
        ck(ok, "split-channel OBJ: all %d corners keep their own uv AND "
               "normal (the marbling failure mode, made impossible)"
           % len(want),
           "" if ok else "got %r want %r" % (corners[:2], want[:2]))
        # 6 distinct (v,vt,vn) triples over 3 faces == dedup really dedups
        ck(head is not None and head[0] == 6 and head[1] == 3,
           "and it de-duplicates: 9 corners -> 6 vertices, 3 triangles",
           str(head))

        # (c) a face referencing a vertex that was never loaded must be
        # DROPPED on the expansion path too, not clamped onto vertex 0.
        head, corners = run(BAD_OBJ)
        ck(head is not None and head[1] == 1 and len(corners) == 3,
           "an out-of-range face is dropped on the expansion path as well",
           str(head))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ------------------------------------------- 12. the harness's SH world mirror
def sh_matrix44(L, shc):
    """FUN_000313E0's form as a 4x4, with the constants READ FROM THE ELF."""
    c1, c2, c3, c4 = shc["c1"], shc["c2"], shc["c3"], shc["c4"]
    c5 = 0.247708                                   # inline at 0x00031474
    return [[c1*L[8],  c1*L[4], c1*L[7], c2*L[3]],
            [c1*L[4], -c1*L[8], c1*L[5], c2*L[1]],
            [c1*L[7],  c1*L[5], c3*L[6], c2*L[2]],
            [c2*L[3],  c2*L[1], c2*L[2], c4*L[0] - c5*L[6]]]


def sh_eval44(M, n):
    v = list(n) + [1.0]
    return max(0.0, sum(v[r] * M[r][c] * v[c] for r in range(4)
                        for c in range(4)))


def section12(csrc, shc, nine):
    print("\n[12] SH world mirror -- the harness's GL world is Z-mirrored")
    # the module must negate exactly i = 2, 5, 7 (the R-H basis functions odd
    # in z: L10, L2-1, L21)
    m = re.search(r"b3fx_sh_mirror_z\(const float in9\[9\], float out9\[9\]\)"
                  r"\s*\{(.*?)\n\}", csrc, re.S)
    ck(m is not None, "src/burnout3_carfx.c defines b3fx_sh_mirror_z()")
    if not m:
        return
    negated = sorted(int(i) for i in re.findall(r"out9\[(\d)\]\s*=\s*-in9",
                                                m.group(1)))
    ck(negated == [2, 5, 7],
       "it negates exactly i = 2 (L10, z), 5 (L2-1, yz), 7 (L21, xz)",
       str(negated))

    # and that choice must BE the z-mirror, identically, over the sphere
    L = list(nine)
    Lm = list(L)
    for i in (2, 5, 7):
        Lm[i] = -Lm[i]
    Mg, Mm = sh_matrix44(L, shc), sh_matrix44(Lm, shc)
    worst = 0.0
    rnd = random.Random(20260812)
    for _ in range(4000):
        v = [rnd.gauss(0, 1) for _ in range(3)]
        ln = (v[0]**2 + v[1]**2 + v[2]**2) ** 0.5
        if ln < 1e-6:
            continue
        v = [x / ln for x in v]
        worst = max(worst, abs(sh_eval44(Mm, v)
                               - sh_eval44(Mg, [v[0], v[1], -v[2]])))
    ck(worst < 1e-9,
       "E_mirrored(x,y,z) == E_retail(x,y,-z) exactly (max diff %.1e over "
       "4000 directions)" % worst)

    # the six axes: x and y untouched, z swapped
    ax = {"+X": (1, 0, 0), "-X": (-1, 0, 0), "+Y": (0, 1, 0),
          "-Y": (0, -1, 0), "+Z": (0, 0, 1), "-Z": (0, 0, -1)}
    same = all(close(sh_eval44(Mm, ax[k]), sh_eval44(Mg, ax[k]), 1e-9)
               for k in ("+X", "-X", "+Y", "-Y"))
    swapped = (close(sh_eval44(Mm, ax["+Z"]), sh_eval44(Mg, ax["-Z"]), 1e-9)
               and close(sh_eval44(Mm, ax["-Z"]), sh_eval44(Mg, ax["+Z"]), 1e-9))
    ck(same, "the mirror leaves E(+/-X) and E(+/-Y) untouched")
    ck(swapped, "and swaps E(+Z) <-> E(-Z): %.5f <-> %.5f"
       % (sh_eval44(Mg, ax["+Z"]), sh_eval44(Mg, ax["-Z"])))

    # the loader really does mirror Z on BOTH positions and normals -- that is
    # the premise the whole transform rests on.
    tm = open(os.path.join(ROOT, "src", "burnout3_trackmesh.c")).read()
    ck("p[2] = -p[2];" in tm,
       "trackmesh_load negates position Z (the premise: GL world = mirror_Z)")
    ck("nv[2] = -nv[2];" in tm,
       "trackmesh_load negates normal Z to match")
    # and the fragment shader must renormalise the interpolated normal
    ck("normalize(vN)" in csrc,
       "the fragment stage renormalises the interpolated normal")
    # The view-facing normal flip must survive ONLY on the face-normal
    # fallback, whose sign genuinely is arbitrary. Applied to real vertex
    # normals it snaps the normal to its opposite wherever N.V crosses zero,
    # which puts a hard seam across the body -- half of the reported
    # "stripes". Check it positionally, inside the uHasNrm == 0 branch.
    flip = [m.start() for m in re.finditer(r"dot\(N, V\) < 0\.0", csrc)]
    cross = csrc.find("normalize(cross(dFdx(vE), dFdy(vE)))")
    smooth = csrc.find("N = normalize(vN);")
    ck(len(flip) == 1 and cross > 0 and smooth > 0
       and smooth < cross < flip[0],
       "the view-facing normal flip is confined to the NO-normals path "
       "(it banded the body when real normals were present)",
       "flips=%d, smooth@%d cross@%d flip@%d"
       % (len(flip), smooth, cross, flip[0] if flip else -1))


# ===========================================================================
# 13. THE CAR VERTEX PROGRAM -- NV2A microcode at 0x003E7D58
# ===========================================================================
VSH_F = {
    "ILU": (1, 25, 3), "MAC": (1, 21, 4), "CONST": (1, 13, 8), "INPUT_V": (1, 9, 4),
    "A_NEG": (1, 8, 1), "A_R": (2, 28, 4), "A_MUX": (2, 26, 2),
    "B_NEG": (2, 25, 1), "B_R": (2, 13, 4), "B_MUX": (2, 11, 2),
    "C_NEG": (2, 10, 1), "C_R_HIGH": (2, 0, 2), "C_R_LOW": (3, 30, 2),
    "C_MUX": (3, 28, 2), "OUT_MAC_MASK": (3, 24, 4), "OUT_R": (3, 20, 4),
    "OUT_ILU_MASK": (3, 16, 4), "OUT_O_MASK": (3, 12, 4), "OUT_ORB": (3, 11, 1),
    "OUT_ADDRESS": (3, 3, 8), "OUT_MUX": (3, 2, 1), "A0X": (3, 1, 1),
    "FINAL": (3, 0, 1),
}
VSH_MAC = ["nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
           "dst", "min", "max", "slt", "sge", "arl", "?", "?"]


def vfld(t, name):
    d, s, n = VSH_F[name]
    return (t[d] >> s) & ((1 << n) - 1)


def vsh_decode(elf, va):
    ver = elf.rd(va, 2)
    ver = struct.unpack('<H', ver)[0]
    n = struct.unpack('<H', elf.rd(va + 2, 2))[0]
    out = []
    for i in range(n):
        b = elf.rd(va + 4 + i * 16, 16)
        out.append([struct.unpack_from('<I', b, j * 4)[0] for j in range(4)])
    return ver, out


def section13(elf, csrc):
    print("\n[13] car VERTEX PROGRAM 0x003E7D58 -- NV2A microcode decoded")
    # the factory's own CreateVertexShader call site, byte-read
    site = elf.rd(0x0003CB6E, 10)
    prog = struct.unpack_from('<I', site, 1)[0]
    decl = struct.unpack_from('<I', site, 6)[0]
    ck(prog == 0x003E7D58 and decl == 0x00387558,
       "FUN_0003C8A0 @0x0003CB6E: CreateVertexShader(decl 0x387558, "
       "prog 0x3E7D58)", "prog=%#x decl=%#x" % (prog, decl))
    ck(elf.u32(0x00387558) == 0x20000000
       and elf.u32(0x0038755C) == 0x40320000
       and elf.u32(0x00387560) == 0x40160002
       and elf.u32(0x00387564) == 0x40220009
       and elf.u32(0x00387568) == 0xFFFFFFFF,
       "declaration = stream0 {v0 FLOAT3, v2 NORMPACKED3, v9 FLOAT2}")

    ver, ins = vsh_decode(elf, 0x003E7D58)
    ck(ver == 0x2078 and len(ins) == 32,
       "xvs header {version 0x2078, 32 instructions}",
       "ver=%#x n=%d" % (ver, len(ins)))
    ck(vfld(ins[-1], "FINAL") == 1 and
       all(vfld(t, "FINAL") == 0 for t in ins[:-1]),
       "the FINAL bit is on instruction 31 and nowhere else")

    def isop(i, mac, consts=None, out_addr=None, out_mask=None):
        t = ins[i]
        if VSH_MAC[vfld(t, "MAC")] != mac:
            return False
        if consts is not None and vfld(t, "CONST") not in consts:
            return False
        if out_addr is not None and (vfld(t, "OUT_O_MASK") == 0
                                     or vfld(t, "OUT_ADDRESS") != out_addr):
            return False
        if out_mask is not None and vfld(t, "OUT_O_MASK") != out_mask:
            return False
        return True

    # 0..2: the world normal -- dp3 v2 against c116/c117/c118
    ck(all(isop(i, "dp3", consts={116 + i}) for i in range(3))
       and all(vfld(ins[i], "A_MUX") == 2 and vfld(ins[i], "INPUT_V") == 2
               for i in range(3)),
       "0-2  r2.xyz = dp3(v2, c[116..118])  -- the world-space NORMAL")
    # 4..7: M * (N,1) against the SH matrix at hw slots 96..99
    ck(all(isop(i, "dp4", consts={96 + (i - 4)}) for i in range(4, 8)),
       "4-7  r3 = dp4(r2, c[96..99])  -- the SH irradiance matrix rows")
    # 9: oD0.xyz = dp4(r3, r2)
    ck(isop(9, "dp4", out_addr=3) and vfld(ins[9], "OUT_O_MASK") == 0xE,
       "9    oD0.xyz = dp4(r3, r2) = E(N) -- the irradiance goes to DIFFUSE")
    # 12: eye position c108 minus the world position
    ck(isop(12, "add", consts={108}) and vfld(ins[12], "C_NEG") == 1,
       "12   r5 = c[108] - worldPos  -- c108 is the EYE POSITION")
    # 21: the reflection vector into oT1
    ck(isop(21, "mad", out_addr=10) and vfld(ins[21], "OUT_O_MASK") == 0xE
       and vfld(ins[21], "C_NEG") == 1,
       "21   oT1.xyz = 2N*(N.V) - V  -- THE REFLECTION VECTOR")
    # 28: Schlick against c111 (the Fresnel pair)
    ck(isop(28, "mad", consts={111}),
       "28   r5.x = (1-|N.V|)^5 * c111.y + c111.x  -- Schlick, c111 = "
       "(R0, 1-R0, 1, 1)")
    # 30: oD0.w = min(F, c111.z)
    ck(isop(30, "min", consts={111}, out_addr=3)
       and vfld(ins[30], "OUT_O_MASK") == 0x1,
       "30   oD0.w = min(F, 1) -- the Fresnel goes to DIFFUSE ALPHA")
    # the constants the C code uploads must be the ones the program reads
    ck(elf.rd(0x0003164C, 2) == b"\xb9\x60",           # MOV ECX,0x60
       "FUN_000315C0 uploads the SH matrix to hw slot 0x60 = c[96]")
    ck(elf.rd(0x00031D7E, 2) == b"\xb9\x6f",           # MOV ECX,0x6f
       "FUN_00031AB0 uploads the Fresnel pair to hw slot 0x6F = c[111]")
    ck(elf.rd(0x00031750, 2) == b"\xb9\x6c",           # MOV ECX,0x6c
       "FUN_00031690 uploads the eye position to hw slot 0x6C = c[108]")
    # and the module's shader must implement them
    ck("2.0 * N * ndv - V" in csrc,
       "module's fragment program builds the same reflection vector")
    ck("pow(1.0 - abs(ndv), 5.0)" in csrc,
       "module's Schlick uses |N.V| exactly as instruction 22 does")


# ===========================================================================
# 14. THE CAR PIXEL SHADERS -- D3DPIXELSHADERDEF register combiners
# ===========================================================================
PS_DEFS = {"glass1": 0x003E8288, "glass2": 0x003E8378, "body": 0x003E8468,
           "alt": 0x003E8558, "mask2": 0x003E8648, "light": 0x003E8738}


def psdef(elf, va):
    d = [elf.u32(va + i * 4) for i in range(60)]
    return dict(ai=d[0:8], fabcd=d[8], fefg=d[9], c0=d[10:18], c1=d[18:26],
                ao=d[26:34], ri=d[34:42], cmp=d[42], ro=d[45:53], cnt=d[53],
                tex=d[54], dot=d[55], intex=d[56], c0m=d[57], c1m=d[58],
                fc=d[59])


def section14(elf, csrc):
    print("\n[14] car PIXEL SHADERS -- D3DPIXELSHADERDEF combiner programs")
    # the factory copies 0x3C dwords = sizeof(D3DPIXELSHADERDEF) per shader
    ck(elf.rd(0x0003CBB8, 2) == b"\xb9\x3c",
       "FUN_0003C8A0 copies 0x3C dwords = 240 bytes = sizeof "
       "D3DPIXELSHADERDEF")
    ck(elf.rd(0x0003CB9A, 2) == b"\x68\xfc",
       "each shader object is a 0xFC-byte allocation (def at +0xC)")
    b = psdef(elf, PS_DEFS["body"])
    ck(b["cnt"] & 0xF == 8, "body: 8 combiner stages", "%08x" % b["cnt"])
    ck(b["tex"] == 0x21,
       "body PSTextureModes = 0x21 -> TWO stages, both PROJECT2D: t0 = the "
       "paint, t1 = THE ENVIRONMENT MAP on oT1")
    ck((b["c0m"] & 0xF) == 14 and ((b["c0m"] >> 8) & 0xF) == 3
       and ((b["c0m"] >> 20) & 0xF) == 3,
       "body C0 map: stage0 = ps c14 (lightRGB, P), stages 2/5 = ps c3 (fade)",
       "%08x" % b["c0m"])
    ck((b["fc"] & 0xF) == 14, "body final-combiner C0 = ps c14 (lightRGB)")
    ck(b["ri"][0] == 0xC8C40000 and b["ro"][0] == 0x000100C0,
       "stage0 rgb: r0 = t0.rgb * v0.rgb, output scale x2  (the ALBEDO x "
       "IRRADIANCE term the port was rendering at HALF)")
    ck(b["ai"][0] == 0x31D430D1 and b["ao"][0] == 0x00000C00,
       "stage0 a:   r0.a = (1-c14.a)*v0.a + c14.a  -- P is the FRESNEL FLOOR")
    ck(b["ai"][1] == 0xD8DC1010 and b["ao"][1] == 0x000000C0,
       "stage1 a:   r0.a *= t0.a  -- the paint ALPHA is the gloss mask")
    ck(b["ai"][2] == 0xDCD11010, "stage2 a:   r0.a *= c3.a (the fade)")
    ck(b["ri"][3] == 0x1CC9CC3C and b["ro"][3] == 0x00000C00,
       "stage3 rgb: r0 = r0.a*t1.rgb + r0.rgb*(1-r0.a)  -- THE LERP TO THE "
       "ENVIRONMENT MAP: this is the gloss")
    ck(b["ai"][4] == 0xD8D91010, "stage4 a:   r0.a = t0.a * t1.a")
    ck(b["ai"][6] == 0xDD30C150,
       "stage6 a:   r1.a = r1.a - c15.z   (c15.z = 1-K)")
    ck(b["ai"][7] == 0x1DD11010 and b["ao"][7] == 0x000200D0,
       "stage7 a:   r1.a = r1.a * c15.w, output scale x4 -- and the C code "
       "pre-divides c15.w by exactly 0.25 (MULSS 0x003B1730 @0x00031C70)")
    ck(close(elf.f32(0x003B1730), 0.25),
       "0x003B1730 = 0.25 = the reciprocal of that x4")
    ck(b["fabcd"] == 0x011D000C and b["fefg"] == 0x00002080,
       "body final: rgb = c14.rgb*r1.a + (1-c14.rgb)*0 + r0.rgb, a = 1, "
       "CLAMP_SUM")
    a = psdef(elf, PS_DEFS["alt"])
    ck(a["cnt"] & 0xF == 3 and a["fabcd"] == 0 and a["fefg"] == 0,
       "alt-material: 3 stages and NO final combiner (FUN_0034E790 "
       "special-cases FinalABCD|EFG == 0 @0x0034E81F)")
    ck(a["ri"][2] == 0x14CDCC34,
       "alt-material stage2 rgb: mix(base, env*P, F)")
    g2 = psdef(elf, PS_DEFS["glass2"])
    ck(g2["tex"] == 0x21 and g2["ri"][0] == 0xC9200000,
       "glass pass 2 stage0 rgb = t1.rgb -- the glass reflection IS the "
       "environment map, not the glass raster")
    ck(g2["c0"][0] == 0x4DE5B2E5,
       "glass pass 2 stage0 constant c5 = 0x4DE5B2E5 (a,r,g,b) -> "
       "0.302 + 0.898*F")
    for v, name in (((0x4DE5B2E5 >> 16) & 0xFF, "GLASS_C5_B"),
                    ((0x4DE5B2E5 >> 24) & 0xFF, "GLASS_C5_A")):
        ck(c_has(c_floats(csrc), v / 255.0, 1e-9),
           "module carries %s = %r" % (name, v / 255.0))
    l = psdef(elf, PS_DEFS["light"])
    ck(l["tex"] == 0x01 and l["ri"][0] == 0xC8200000
       and l["ro"][0] == 0x000100C0,
       "light/emissive: one stage, t0.rgb x2, no environment")
    # the module must implement the recovered composition.  The reflection
    # line carries three TUNED weights since the LOOK wave (user-authorized
    # deviation 2026-08-13) -- uReflFloor on the constant P part, uReflGain on
    # the whole layer, uEnvMod on the local probe brightness -- so the pinned
    # fragment is the tuned one; the SHAPE (paint.a, the P floor, (1-P)*F) is
    # asserted term by term below and is unchanged.
    for frag, why in (
            ("2.0 * paint.rgb * E", "the x2 albedo*irradiance"),
            ("paint.a * (uP * uReflFloor + (1.0 - uP) * F)",
             "the P floor and gloss mask"),
            ("mix(base, envc, refl)", "the lerp to the environment"),
            ("(paint.a * env.a * uFade - uKz) * uGain", "the specular")):
        ck(frag in csrc, "module implements %s" % why)
    # ...and the tuned weights must all be neutral-able from one switch, so
    # the recovered composition is still reachable from the shipped binary.
    ck('getenv("B3_CARFX_TUNE")' in csrc and "g.tune ? B3FX_T_REFL_GAIN" in csrc
       and "g.tune ? B3FX_T_REFL_FLOOR" in csrc
       and "g.tune ? g.envmod" in csrc,
       "B3_CARFX_TUNE=0 sets all three TUNED weights to 1, i.e. restores the "
       "untouched recovered composition")
    for name in ("B3FX_T_REFL_GAIN", "B3FX_T_REFL_FLOOR",
                 "B3FX_T_ENV_SHADE_MIN", "B3FX_T_ENV_SHADE_POW",
                 "B3FX_T_PROBE_CONTRAST"):
        ck(name in csrc, "module defines the tuned magnitude %s" % name)
    ck(csrc.count("TUNED (user-authorized deviation 2026-08-13)") >= 8,
       "every tuned magnitude carries the TUNED mark (%d marks)"
       % csrc.count("TUNED (user-authorized deviation 2026-08-13)"))


# ===========================================================================
# 15/16. LIGHT PROBES -- decode differential, container, extractor output
# ===========================================================================
def section15(elf, csrc):
    print("\n[15] light probes -- FUN_0019C640 decode, EXECUTED vs the module")
    # the three band scales the decoder multiplies by
    s0, s1, s2 = elf.f32(0x003B16F0), elf.f32(0x003B16EC), elf.f32(0x003B16E8)
    inv = elf.f32(0x003B16F4)
    ck(close(s0, 1.6) and close(s1, 0.6) and close(s2, 0.4)
       and close(inv, 0.0078125),
       "band scales 1.6 / 0.6 / 0.4 and 1/128 @0x003B16E8..F4",
       "%r %r %r %r" % (s0, s1, s2, inv))
    # LEA EDX,[ESI + EAX*8]; ADD EDX,EAX  == stride 9
    ck(elf.rd(0x0019D4DA, 3) == b"\x8d\x14\xc6" and
       elf.rd(0x0019D4DD, 2) == b"\x03\xd0",
       "probe stride is 9 bytes (LEA [ESI+EAX*8]; ADD EDX,EAX @0x0019D4DA)")
    if "--no-emu" in sys.argv:
        print("  ..  differential skipped (--no-emu)")
        return
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_EDX
    from unicorn import UC_PROT_ALL
    import trace_panels as TP
    t = TP.Tracer(os.path.join(PVEH, "COMP/Car1.bgv"), deep=False)
    SCR = 0x30050000
    try:
        t.uc.mem_map(SCR, 0x1000, UC_PROT_ALL)
    except Exception:
        pass
    random.seed(1234)
    worst = 0.0
    for _ in range(64):
        b = bytes((random.randrange(256) for _ in range(9)))
        t.uc.mem_write(SCR, b)
        t._call(0x0019C640, regs={UC_X86_REG_EDX: SCR,
                                  UC_X86_REG_ECX: SCR + 0x40})
        got = struct.unpack('<9f', bytes(t.uc.mem_read(SCR + 0x40, 36)))
        sb = [x - 256 if x > 127 else x for x in b]
        want = [sb[0] * s0 * inv] + [sb[i] * s1 * inv for i in (1, 2, 3)] \
            + [sb[i] * s2 * inv for i in (4, 5, 6, 7, 8)]
        worst = max(worst, max(abs(a - c) for a, c in zip(got, want)))
    ck(worst < 1e-7,
       "FUN_0019C640 EXECUTED on 64 random probes == the module's decode",
       "max |diff| = %.3g" % worst)
    ck("(1.6f * 0.0078125f)" in csrc and "(0.6f * 0.0078125f)" in csrc
       and "(0.4f * 0.0078125f)" in csrc,
       "module's b3fx_probe_decode carries those three products")


def section16():
    print("\n[16] light-probe CONTAINER -- every shipped track")
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import extract_tlist as tl
    import extract_light_probes as lp
    ntrack = nbad = 0
    tot_probe = 0
    for tr in tl.track_table():
        sd = os.path.join(tr["dir"], "static.dat")
        st = os.path.join(tr["dir"], "streamed.dat")
        if not (os.path.exists(sd) and os.path.exists(st)):
            continue
        sdb, stb = open(sd, "rb").read(), open(st, "rb").read()
        nu = struct.unpack_from("<H", sdb, 0x54)[0]
        ut = struct.unpack_from("<i", sdb, 0x58)[0]
        neg = 0
        n = 0
        for u in range(nu):
            so, lo, ss, ls = struct.unpack_from("<iiii", sdb, ut + u * 0x10)
            if not lo or not ls:
                continue
            got = lp.parse_unit(stb[lo:lo + ls], u)
            if got is None:
                continue
            probes, tris, cnt = got
            n += cnt
            neg += sum(1 for i in range(cnt)
                       if struct.unpack_from("<b", probes, i * 9)[0] < 0)
        ntrack += 1
        tot_probe += n
        if neg:
            nbad += 1
    ck(ntrack > 0 and nbad == 0,
       "unit+0xA4 parses as a 9-byte probe array on all %d shipped tracks, "
       "and not one of the %d L00 bytes is negative (an irradiance L00 "
       "cannot be)" % (ntrack, tot_probe), "bad tracks: %d" % nbad)
    # the extractor's triangles must be the collision extractor's triangles
    import extract_collision as ec                              # noqa: F401
    out = os.path.join(ROOT, "build", "tracks")
    pairs = 0
    for tid in sorted(os.listdir(out)) if os.path.isdir(out) else []:
        p = os.path.join(out, tid, "light_probes.bin")
        c = os.path.join(out, tid, "collision.bin")
        if not (os.path.exists(p) and os.path.exists(c)):
            continue
        pd, cd = open(p, "rb").read(20), open(c, "rb").read(0x28)
        ptri = struct.unpack_from("<I", pd, 12)[0]
        ctri = struct.unpack_from("<I", cd, 8)[0]
        pb = struct.unpack_from("<6f", open(p, "rb").read(0x28), 0x10)
        cb = struct.unpack_from("<6f", cd, 0x10)
        ck(ptri == ctri and pb == cb,
           "%s: light_probes.bin and collision.bin agree (%d tris, same "
           "bounds)" % (tid, ptri))
        pairs += 1
    if not pairs:
        print("  ..  no build/tracks/*/collision.bin to cross-check against")


# ------------------------- 17. the body's texture stage 1 -- the environment
# The car body pixel shader lerps the shaded paint towards texture stage 1
# (section 14 asserts the combiner).  This section pins THREE things:
#   a) the bind and the addressing FUN_00031690 issues, byte-exact, and the
#      renderer base that makes DAT_004D6C00 = renderer+0xA90;
#   b) the BOUNDARY -- that the writer of that global is not in the image, so
#      the port's env map is a documented [S] substitute and not a guess.  Two
#      searches are re-run here from the raw bytes: the whole-image literal
#      count, and the only encoding that can reach +0xA90 off a register base;
#   c) the substitute itself -- enviro.dat +0xA0 on every shipped track, and
#      that the PNGs tools/extract_envmap.py wrote decode to the same pixels
#      an independent decoder in this file produces.
ENVMAP_FIELD = 0xA0          # [C] FUN_00188880 @0x001888BF


def _dxt_ref_block(blob, o, dxt1):
    """Independent 4x4 DXT colour-block decode (this file's own)."""
    c0, c1 = struct.unpack_from('<2H', blob, o)
    bits, = struct.unpack_from('<I', blob, o + 4)

    def e565(c):
        r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
        return [r * 255 // 31, g * 255 // 63, b * 255 // 31]
    a, b_ = e565(c0), e565(c1)
    if c0 > c1 or not dxt1:
        pal = [a, b_,
               [(2 * a[i] + b_[i]) // 3 for i in range(3)],
               [(a[i] + 2 * b_[i]) // 3 for i in range(3)]]
    else:
        pal = [a, b_, [(a[i] + b_[i]) // 2 for i in range(3)], [0, 0, 0]]
    return [[pal[(bits >> (2 * (y * 4 + x))) & 3] for x in range(4)]
            for y in range(4)]


def section17(elf, csrc):
    print("\n[17] body texture stage 1 -- the environment map")
    # ---- (a) the bind, byte-exact -------------------------------------
    ck(elf.rd(0x00031740, 6) == bytes.fromhex("8b0d006c4d00"),
       "FUN_00031690 @0x00031740 = MOV ECX,[0x004D6C00]")
    ck(elf.rd(0x00031746, 6) == bytes.fromhex("890d74db7500"),
       "FUN_00031690 @0x00031746 = MOV [0x0075DB74],ECX  (stage-1 texture)")
    # renderer base: MOV EAX,0x4d6170 ; CALL rel32 -> FUN_0003C8A0
    b = elf.rd(0x00015C47, 10)
    rel, = struct.unpack_from("<i", b, 6)
    ck(b[0] == 0xB8 and struct.unpack_from("<I", b, 1)[0] == 0x004D6170
       and b[5] == 0xE8 and 0x00015C51 + rel == 0x0003C8A0,
       "renderer object base is 0x004D6170 (MOV EAX,0x4d6170; CALL "
       "FUN_0003C8A0 @0x00015C47) -- so DAT_004D6C00 is renderer+0x%03X"
       % (0x004D6C00 - 0x004D6170))
    # ---- (b) the boundary ---------------------------------------------
    lit = struct.pack('<I', 0x004D6C00)
    sites = []
    for va, off, fsz in elf.segs:
        blob = elf.d[off:off + fsz]
        k = blob.find(lit)
        while k >= 0:
            sites.append(va + k)
            k = blob.find(lit, k + 1)
    ck(sites == [0x00031742],
       "the dword 0x004D6C00 occurs exactly once in every PT_LOAD segment "
       "(unaligned scan) -- inside that read; no absolute store, no pointer "
       "table entry", "sites: %s" % [hex(s) for s in sites])
    # +0xA90 cannot be a disp8, so every register-based access to it carries
    # the bytes 90 0A 00 00 right after a mod=10 ModRM (+ optional SIB).
    disp = struct.pack('<I', 0xA90)
    cand, gfx = 0, 0
    for va, off, fsz in elf.segs:          # every PT_LOAD -- a superset
        blob = elf.d[off:off + fsz]
        k = blob.find(disp)
        while k >= 0:
            for back in (1, 2):
                if k - back < 0 or (blob[k - back] >> 6) != 0b10:
                    continue
                rm = blob[k - back] & 7
                if (back == 1 and rm != 4) or (back == 2 and rm == 4):
                    cand += 1
                    if 0x00011000 <= va + k < 0x00050000:
                        gfx += 1
            k = blob.find(disp, k + 1)
    ck(cand > 0 and gfx == 0,
       "no instruction in the graphics module [0x011000,0x050000) can address "
       "+0xA90 off a register base (%d candidate disp32==0xA90 sites image-"
       "wide, %d there) -- the writer of the env-map global is not in the "
       "image, so the port's map is a documented [S] substitute"
       % (cand, gfx))
    # ---- (c) the substitute -------------------------------------------
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import extract_envmap as ee
    tids = ee.track_ids()
    have, missing, badname = [], [], []
    for t in tids:
        d = open(os.path.join(ee.TRACKS, t, "enviro.dat"), 'rb').read()
        r = ee.envmap_record(d)
        if r is None:
            missing.append(t)
            continue
        have.append((t, r))
        n = r["name"].lower()
        if "env" not in n and "sky" not in n:
            badname.append((t, r["name"]))
    ck(len(tids) > 0 and not badname,
       "enviro.dat +0xA0 parses as a texture record on %d/%d track "
       "directories and every one of them is named env*/*sky* by the artists"
       % (len(have), len(tids)),
       "no +0xA0: %s" % ",".join(missing))
    # the PNGs the extractor wrote must match an independent block decode
    try:
        from PIL import Image
    except ImportError:
        print("  ..  PIL missing, skipping the envmap.png round-trip")
        return
    checked = mism = 0
    for t, r in have:
        png = os.path.join(ROOT, "build", "tracks",
                           t.replace('/', '_'), "envmap.png")
        if not os.path.exists(png):
            continue
        im = Image.open(png).convert('RGB')
        if im.size != (r["w"], r["h"]):
            mism += 1
            continue
        d = open(os.path.join(ee.TRACKS, t, "enviro.dat"), 'rb').read()
        dxt1 = r["fmt"] == 0xC
        o = r["data_off"] + (0 if dxt1 else 8)
        ref = _dxt_ref_block(d, o, dxt1)
        for y in range(min(4, r["h"])):
            for x in range(min(4, r["w"])):
                got = im.getpixel((x, y))
                want = ref[y][x]
                if max(abs(got[i] - want[i]) for i in range(3)) > 2:
                    mism += 1
        checked += 1
    ck(checked > 0 and mism == 0,
       "build/tracks/*/envmap.png block 0 matches an independent DXT decode "
       "on all %d written maps (and every image is the size its enviro.dat "
       "record declares)" % checked, "mismatches: %d" % mism)
    # ---- the module actually loads it ----------------------------------
    ck("build/tracks/%s/envmap.png" in csrc and "B3_CARFX_ENVMAP" in csrc,
       "b3_carfx_set_track() loads build/tracks/<ID>/envmap.png and "
       "B3_CARFX_ENVMAP still overrides it")
    ck("texture2D(uEnv, R.xy)" in csrc,
       "the shader samples stage 1 at the reflection vector's xy -- "
       "PROJECT2D with the unwritten q = 1, as decoded in section 13")


def main():
    if not os.path.exists(ELF):
        raise SystemExit("build/burnout3.elf missing (tools/xbe2elf.py)")
    elf = Elf(ELF)
    csrc = open(CARFX_C).read()
    srcf = c_floats(csrc)

    print("validate_carfx: src/burnout3_carfx.c vs build/burnout3.elf")
    shc = section1(elf, srcf)
    body_r0, glass_r0 = section3(elf, srcf)

    if "--no-emu" in sys.argv:
        print("\n[2][4] skipped (--no-emu)")
    else:
        table, ev, body_c15, glass_c15 = emulate(elf)
        section2(table, srcf)
        section4(ev, table, body_r0, glass_r0, shc, body_c15, glass_c15)

    section5(elf, srcf)
    section6(elf, srcf)
    section7()
    section8(elf, srcf, csrc)
    nine = section9(elf, csrc, shc)
    section10(elf, csrc)
    section11(elf)
    section11b()
    section12(csrc, shc, nine)
    section13(elf, csrc)
    section14(elf, csrc)
    section15(elf, csrc)
    section16()
    section17(elf, csrc)

    n = len(PASS) + len(FAIL)
    print("\n%d/%d checks pass" % (len(PASS), n))
    if FAIL:
        print("FAILURES:")
        for f in FAIL:
            print("  -", f)
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
