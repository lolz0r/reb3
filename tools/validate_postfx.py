#!/usr/bin/env python3
"""
Differential/assertion test for the world post-FX port (src/burnout3_postfx.*).

Three kinds of check, in the project's usual order of trustworthiness:

  A. IMAGE BYTES.  Every constant the port hard-codes is read back out of
     build/burnout3.elf at its literal address and compared.  Every structural
     claim (draw count, primitive type, vertex stride, buffer size, texture
     size, the `radialblurmask` string reference) is asserted as the actual
     INSTRUCTION BYTES at the cited address, so a wrong address fails loudly
     instead of silently agreeing.

  B. SHIPPED DATA.  The per-track enviro.dat records are parsed and their
     names/sizes/formats asserted, and the negative that motivated the whole
     sky investigation -- "no material in any track's static.dat names a sky"
     -- is re-checked on every shipped track rather than taken on trust.

  C. EXECUTED PORT.  src/burnout3_postfx.c is compiled with -DB3_POSTFX_NO_GL
     and its dome generator is run for real; the emitted vertices and indices
     are compared element-by-element against an independent Python
     transcription of FUN_00032020 written from the disassembly.  Two
     independent derivations disagreeing is what catches errors here
     (HANDOFF section 5), so the Python mirror is deliberately NOT a
     translation of the C -- it is written from the decompiler output.

Run:  python3 tools/validate_postfx.py            (add -v for the full list)
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import ctypes
import math
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "burnout3.elf")
HDR = os.path.join(ROOT, "src", "burnout3_postfx.h")
SRC = os.path.join(ROOT, "src", "burnout3_postfx.c")
GAME = (game_root())
TRACKS = os.path.join(GAME, "Tracks")

PASS, FAIL = [], []
VERBOSE = "-v" in sys.argv


def check(name, ok, detail=""):
    (PASS if ok else FAIL).append((name, detail))
    if VERBOSE or not ok:
        print("%-4s %-58s %s" % ("ok" if ok else "FAIL", name, detail))


# --------------------------------------------------------------- ELF reader
class Image:
    def __init__(self, path):
        self.f = open(path, 'rb').read()
        e_phoff = struct.unpack_from('<I', self.f, 0x1C)[0]
        e_phnum = struct.unpack_from('<H', self.f, 0x2C)[0]
        self.segs = []
        for i in range(e_phnum):
            o = e_phoff + i * 32
            _, off, va, _, fsz, _, _, _ = struct.unpack_from('<8I', self.f, o)
            self.segs.append((off, va, fsz))

    def read(self, va, n):
        for off, base, fsz in self.segs:
            if base <= va < base + fsz:
                o = off + (va - base)
                return self.f[o:o + n]
        return None

    def f32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<f', b)[0] if b else None

    def f64(self, va):
        b = self.read(va, 8)
        return struct.unpack('<d', b)[0] if b and len(b) == 8 else None

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<I', b)[0] if b and len(b) == 4 else None

    def cstr(self, va, limit=64):
        b = self.read(va, limit)
        if b is None:
            return None
        e = b.find(b'\0')
        return b[:e].decode('ascii', 'replace') if e >= 0 else None


# ------------------------------------------------------- header constants
def header_defines():
    out = {}
    for line in open(HDR):
        line = line.strip()
        if not line.startswith("#define "):
            continue
        parts = line[8:].split(None, 1)
        if len(parts) != 2:
            continue
        name, rest = parts
        val = rest.split("/*")[0].strip()
        out[name] = val
    return out


def as_float(tok):
    tok = tok.strip().strip("()")
    if tok.endswith('f'):
        tok = tok[:-1]
    try:
        return float(tok)
    except ValueError:
        return None


# ============================================================ A. IMAGE BYTES
def section_a(img, defs):
    # A1 -- every float constant the port hard-codes, at its literal address.
    #      (address, expected, what consumes it)
    floats = [
        (0x003B2144, 0.19634954,   "ring azimuth step -> DAT_004D916C @0x0025EAE0",
         "B3_SKY_THETA_STEP"),
        (0x003B18F0, 0.2617994,    "dome parameter step -> DAT_004D9174 @0x0025EAC0",
         "B3_SKY_T_STEP"),
        (0x003B2140, 0.765625,     "vLow -> DAT_004D7058 @0x0025EA40 / DAT_004D9164 @0x0025EA80",
         "B3_SKY_V_LOW"),
        (0x003B1A90, 0.015625,     "sky vHigh -> DAT_004D9154 @0x0025EA60",
         "B3_SKY_V_HIGH"),
        (0x003B1F50, 1.015625,     "ground vHigh -> DAT_004D9168 @0x0025EAA0",
         "B3_GND_V_HIGH"),
        (0x003B16CC, 1000.0,       "dome scale margin, FUN_00032580 @0x000325B3",
         "B3_SKY_FAR_MARGIN"),
        (0x003B1684, 0.5,          "blur centre / diffuse constant",
         "B3_BLUR_CENTER_X"),
        (0x003B1758, 0.99,         "blur layer-A zoom, FUN_0002EBE0 @0x0002EBFB",
         "B3_BLUR_ZOOM_A"),
        (0x003B18B4, 0.9998999834, "blur layer-B zoom, FUN_0002EBE0 @0x0002EC62",
         "B3_BLUR_ZOOM_B"),
    ]
    for va, want, why, macro in floats:
        got = img.f32(va)
        ok = got is not None and abs(got - want) <= max(1e-7, abs(want) * 1e-6)
        check("A1 float 0x%08X == %-12g (%s)" % (va, want, macro), ok,
              "got %r  [%s]" % (got, why))
        if macro in defs:
            hv = as_float(defs[macro])
            check("A1 header %s matches the image" % macro,
                  hv is not None and abs(hv - want) <= abs(want) * 1e-6 + 1e-7,
                  "header %r vs image %r" % (hv, got))

    # A1b -- the two literals the generator uses inline (2/pi, 1/(2*pi), 1/0.85,
    # -0.25) are folded into the code stream as .rdata reads; assert the values
    # the port uses agree with exact maths rather than with a guessed address.
    exact = [("B3_SKY_TWO_OVER_PI", 2.0 / math.pi),
             ("B3_SKY_INV_TWO_PI", 1.0 / (2.0 * math.pi)),
             ("B3_SKY_TC1_VSCALE", 1.0 / 0.85),
             ("B3_SKY_SKIRT", -0.25)]
    for macro, want in exact:
        hv = as_float(defs.get(macro, ""))
        check("A1b header %s == %.8f" % (macro, want),
              hv is not None and abs(hv - want) < 2e-7, "header %r" % hv)

    # A1c -- the two step constants must reproduce the intended sampling.
    check("A1c 33 rings x 0.19634954 spans exactly 2*pi",
          abs(32 * 0.19634954 - 2 * math.pi) < 1e-5,
          "%.8f vs %.8f" % (32 * 0.19634954, 2 * math.pi))
    check("A1c 7 dome rows x 0.2617994 x 2/pi spans a = 0..1",
          abs(6 * 0.2617994 * (2 / math.pi) - 1.0) < 1e-6,
          "%.8f" % (6 * 0.2617994 * (2 / math.pi)))

    # A2 -- instruction bytes for every structural claim.
    code = [
        (0x000327E4, "68 40 05 00 00",
         "FUN_00032580: PUSH 0x540 = the dome's 1344 indices", "B3_SKY_INDICES", 1344),
        (0x000327E9, "6A 05",
         "FUN_00032580: PUSH 5 = D3DPT_TRIANGLELIST", None, None),
        (0x00032633, "6A 20",
         "FUN_00032580: PUSH 0x20 = the 32-byte vertex stride", None, None),
        (0x001A9F88, "C7 05 20 D1 45 00 40 05 00 00",
         "FUN_001A9C50: DAT_0045D120 := 0x540 (index count)", None, None),
        (0x001A9FA8, "68 C0 41 00 00",
         "FUN_001A9C50: PUSH 0x41C0 = the two-dome vertex buffer size", None, None),
        (0x001A9FFD, "6A 06 6A 01 6A 20 6A 40",
         "FUN_001A9C50: CreateTexture(w=0x40, h=0x20, 1, 6) = the 64x32 LUT",
         "B3_SKY_LUT_W", 64),
        (0x0002EC3C, "68 E8 AF 3A 00",
         "FUN_0002EBE0: PUSH 0x3AAFE8 = the `radialblurmask` name", None, None),
        (0x000325AB, "F3 0F 10 0D E0 67 4D 00 F3 0F 5C 0D CC 16 3B 00",
         "FUN_00032580: scale = DAT_004D67E0 - [0x003B16CC]", None, None),
    ]
    for va, hexpat, why, macro, mval in code:
        want = bytes.fromhex(hexpat.replace(" ", ""))
        got = img.read(va, len(want))
        check("A2 bytes @0x%08X (%s)" % (va, why.split(':')[0]),
              got == want, "%s | want %s got %s" % (why, want.hex(), got.hex() if got else None))
        if macro:
            hv = as_float(defs.get(macro, ""))
            if hv is None:
                try:
                    hv = float(int(defs.get(macro, "x"), 0))
                except ValueError:
                    hv = None
            check("A2 header %s == %d" % (macro, mval), hv == mval, "header %r" % hv)

    # A3 -- the texture name the blur mask points at.
    s = img.cstr(0x003AAFE8)
    check("A3 string at 0x003AAFE8 == 'radialblurmask'", s == "radialblurmask",
          "got %r" % s)

    # A4 -- 0x41C0 is exactly two domes of 263 vertices at stride 0x20, and the
    # sky/ground pair the port builds has the same vertex count per dome.
    check("A4 0x41C0 == 2 * 263 * 0x20 (two dome blocks at the 32-byte stride)",
          0x41C0 == 2 * 263 * 0x20, "%d" % 0x41C0)
    check("A4 index count 0x540 == 16 columns * 2 halves * 7 quads * 6",
          0x540 == 16 * 2 * 7 * 6, "%d" % 0x540)

    # ------------------------------------------------------------------ A5
    # THE PRESENT COMPOSITE (FUN_0003DA90 / the def at 0x003E9EA8).
    #
    # This is the section that carries the wave's headline claim -- that the
    # present pass is over-unity -- so every word of the D3DPIXELSHADERDEF is
    # asserted as raw image bytes, and the x2 is asserted twice: once as the
    # D3D flag nibble (value >> 12) and once as the raw NV2A
    # NV097_SET_COMBINER_COLOR_OCW OP field (bits 15..17, mask 0x00038000),
    # because FUN_0034E790 copies the def words verbatim into the push buffer.
    PSDEF = 0x003E9EA8
    F = {"PSAlphaInputs": 0x00, "PSFinalCombinerInputsABCD": 0x20,
         "PSFinalCombinerInputsEFG": 0x24, "PSAlphaOutputs": 0x68,
         "PSRGBInputs": 0x88, "PSRGBOutputs": 0xB4, "PSCombinerCount": 0xD4,
         "PSTextureModes": 0xD8, "PSC0Mapping": 0xE4}
    words = [("PSCombinerCount", 0x00011101, "1 combiner stage"),
             ("PSTextureModes", 0x00000021, "two PROJECT2D samplers, T0 and T1"),
             ("PSRGBInputs", 0xC9D120C8,
              "A=T1.rgb B=C0.a C=ZERO|inv(=1) D=T0.rgb"),
             ("PSRGBOutputs", 0x00010C00, "SUM->R0, OP field = SHIFTLEFTBY1"),
             ("PSAlphaInputs", 0xD8301010, "A=T0.a B=ZERO|inv(=1)"),
             ("PSAlphaOutputs", 0x000000C0, "AB->R0, no shift on alpha"),
             ("PSFinalCombinerInputsABCD", 0x00000C00, "out.rgb = R0.rgb"),
             ("PSFinalCombinerInputsEFG", 0x00001C80, "out.a = R0.a, CLAMP_SUM"),
             ("PSC0Mapping", 0xFFFFFFF0, "stage 0 takes PSConstant0[0]")]
    for name, want, why in words:
        got = img.u32(PSDEF + F[name])
        check("A5 present PS def %s == 0x%08X" % (name, want), got == want,
              "%s | got %s" % (why, "0x%08X" % got if got is not None else None))

    ocw = img.u32(PSDEF + F["PSRGBOutputs"]) or 0
    check("A5 present x2: NV2A OCW OP field (bits 15..17) == 2 (SHIFTLEFTBY1)",
          (ocw & 0x00038000) >> 15 == 2,
          "OCW 0x%08X -> OP %d" % (ocw, (ocw & 0x00038000) >> 15))
    check("A5 present x2: D3D flag nibble (word >> 12) == 0x10 "
          "(PS_COMBINEROUTPUT_SHIFTLEFT_1)", (ocw >> 12) == 0x10,
          "0x%03X" % (ocw >> 12))
    # ... and the SAME field, read the SAME way, on a world def, which the two
    # previous waves independently settled as x2. If the decode were wrong this
    # would disagree with their conclusion.
    world = img.u32(0x003E8D08 + F["PSRGBOutputs"]) or 0
    check("A5 the world def 0x003E8D08 uses the same OP value (cross-check)",
          (world & 0x00038000) >> 15 == 2 and world == 0x000100C0,
          "world OCW 0x%08X" % world)
    check("A5 the two defs differ ONLY in the destination field",
          (world & ~0x00000FF0) == (ocw & ~0x00000FF0),
          "0x%08X vs 0x%08X" % (world, ocw))

    # A6 -- the instruction bytes behind the composite's plumbing.
    code2 = [
        (0x0003D89B, "6A 12 68 E0 01 00 00 68 80 02 00 00",
         "FUN_0003D890: the scene surface is (640, 480, fmt 0x12 = "
         "LIN_A8R8G8B8) at renderer+0x890"),
        (0x0003D8E2, "6A 11 6A 78 68 A0 00 00 00",
         "FUN_0003D890: the blur surface is (160, 120, fmt 0x11 = LIN_R5G6B5) "
         "at renderer+0x8D8"),
        (0x0003FA43, "05 90 08 00 00 50 E8",
         "FUN_0003FA20: ADD EAX,0x890 / PUSH EAX / CALL SetRenderTarget -- the "
         "whole scene renders into the 640x480 surface"),
        (0x000402AB, "81 C3 90 08 00 00 53 E8",
         "the RT restore also targets renderer+0x890"),
        (0x0003DC96, "89 3D 70 DB 75 00",
         "FUN_0003DA90: texture stage 0 := renderer+0x890 (the scene)"),
        (0x0003DC9C, "A3 74 DB 75 00",
         "FUN_0003DA90: texture stage 1 := renderer+0x8D8 (the blur)"),
        (0x0003DC2B, "8B 8E A4 04 00 00 51 E8",
         "FUN_0003DA90: SetPixelShader(renderer+0x4A4) -- the composite shader"),
        (0x0003D175, "BE A8 9E 3E 00 F3 A5 89 83 A4 04 00 00",
         "FUN_0003C8A0: MOV ESI,0x3E9EA8 / MOVSD.REP / MOV [EBX+0x4A4] -- "
         "renderer+0x4A4 IS the def at 0x003E9EA8"),
        (0x0003DD13, "C7 00 FC 17 04 00",
         "FUN_0003DA90: NV097_SET_BEGIN_END method 0x417FC ... "),
        (0x0003DD19, "C7 40 04 08 00 00 00",
         "... with value 8 = QUADS: ONE full-screen quad"),
        (0x0003DC19, "89 15 90 D5 75 00",
         "FUN_0003DA90: render state 0x3C (ALPHABLENDENABLE) := 0 -- the "
         "composite is an opaque blit, not a blend"),
        (0x0003DA9F, "E8",
         "FUN_0003DA90 @+0xF: CALL FUN_000402C0, which does "
         "SetRenderTarget(device+0x1A14 = the back buffer)"),
        (0x000402F0, "8B 4C 24 08 8B 91 68 08 00 00 52 56 E8",
         "FUN_000402C0: SetRenderTarget(device+0x1A14, this+0x868)"),
    ]
    for va, hexpat, why in code2:
        want = bytes.fromhex(hexpat.replace(" ", ""))
        got = img.read(va, len(want))
        check("A6 bytes @0x%08X (%s)" % (va, why.split(':')[0]), got == want,
              "%s | want %s got %s"
              % (why, want.hex(), got.hex() if got else None))

    # A7 -- the alpha law's three literals, and the resolution relationship
    # that makes the blur surface exactly mip level 2 of the scene surface.
    for va, want, why, macro in [
            (0x003B1688, 2.0, "FUN_0003DA90 @0x0003DC42 COMISS bound",
             "B3_PRESENT_S_MAX"),
            (0x003B1684, 0.5, "FUN_0003DA90 @0x0003DC5A MULSS scale",
             "B3_PRESENT_S_SCALE"),
            (0x003B168C, 1.0, "FUN_0003DA90 @0x0003DC4B saturated value", None),
            (0x003B1F00, 640.0, "quad texcoord0 u span = the scene width", None),
            (0x003B1EEC, 480.0, "quad texcoord0 v span = the scene height", None),
            (0x003A49FC, 160.0, "quad texcoord1 u span = the blur width", None),
            (0x003A1A00, 120.0, "quad texcoord1 v span = the blur height", None)]:
        got = img.f32(va)
        ok = got is not None and abs(got - want) <= max(1e-6, abs(want) * 1e-6)
        check("A7 float 0x%08X == %-8g (%s)" % (va, want, why.split(',')[0]), ok,
              "got %r" % got)
        if macro and macro in defs:
            hv = as_float(defs[macro])
            check("A7 header %s matches the image" % macro,
                  hv is not None and abs(hv - want) < 1e-6, "header %r" % hv)
    check("A7 640/160 == 480/120 == 4, so the blur surface is mip level 2",
          640 // 160 == 4 and 480 // 120 == 4
          and int(defs.get("B3_PRESENT_BLUR_LOD", "0")) == 2,
          "header LOD %s" % defs.get("B3_PRESENT_BLUR_LOD"))

    # A8 -- the gamma ramp: the exponent as the raw double at its literal
    # address, the loop that consumes it, and the three identical byte stores
    # that prove R, G and B get the same table.
    gam = img.f64(0x003B20F8)
    check("A8 gamma exponent double @0x003B20F8 == 0.95 "
          "(FUN_0003C8A0 @0x0003D3FA)",
          gam is not None and abs(gam - 0.949999988079071) < 1e-12,
          "got %r" % gam)
    hv = as_float(defs.get("B3_GAMMA_EXPONENT", ""))
    check("A8 header B3_GAMMA_EXPONENT matches the image",
          hv is not None and gam is not None and abs(hv - gam) < 1e-12,
          "header %r" % hv)
    check("A8 float 0x003B16AC == 1/255 (the ramp loop's FMUL)",
          abs((img.f32(0x003B16AC) or 0) - 1.0 / 255.0) < 1e-9,
          "got %r" % img.f32(0x003B16AC))
    check("A8 float 0x003B16C4 == 255 (the ramp loop's rescale)",
          abs((img.f32(0x003B16C4) or 0) - 255.0) < 1e-6,
          "got %r" % img.f32(0x003B16C4))
    check("A8 bytes @0x0003D3F0 (FILD i / FMUL 1-255 / FLD 0.95)",
          img.read(0x0003D3F0, 16) ==
          bytes.fromhex("db442410d80dac163b00dd05f8203b00"),
          "got %s" % (img.read(0x0003D3F0, 16) or b"").hex())
    check("A8 bytes @0x0003D410 (three identical stores to 0x0045D1A8 / "
          "0x0045D2A8 / 0x0045D3A8 -- one table for R, G and B)",
          img.read(0x0003D410, 18) ==
          bytes.fromhex("8886a8d145008886a8d245008886a8d34500"),
          "got %s" % (img.read(0x0003D410, 18) or b"").hex())


# ========================================================== B. SHIPPED DATA
def section_b():
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    try:
        import extract_postfx_art as art
    except ImportError as e:
        check("B0 tools/extract_postfx_art.py imports", False, str(e))
        return
    check("B0 tools/extract_postfx_art.py imports", True)

    if not os.path.isdir(TRACKS):
        check("B1 game Tracks/ present", False, TRACKS)
        return

    tracks = []
    for region in sorted(os.listdir(TRACKS)):
        rd = os.path.join(TRACKS, region)
        if os.path.isdir(rd):
            for t in sorted(os.listdir(rd)):
                if os.path.exists(os.path.join(rd, t, "enviro.dat")):
                    tracks.append("%s/%s" % (region, t))
    check("B1 every shipped track has an enviro.dat", len(tracks) >= 30,
          "%d tracks" % len(tracks))

    # B2 -- the header slots resolve to real texture records on EVERY track,
    # and a blind full-file scan finds exactly the same records.
    bad_hdr, bad_scan, counts = [], [], {}
    roles = {0x98: [], 0x9C: [], 0xA0: [], 0xA4: []}
    for t in tracks:
        d = open(os.path.join(TRACKS, t, "enviro.dat"), 'rb').read()
        hdr = art.records_from_header(d)
        counts[len(hdr)] = counts.get(len(hdr), 0) + 1
        if len(hdr) < 3:
            bad_hdr.append("%s:%d" % (t, len(hdr)))
        sc = art.records_by_scan(d)
        if {r.rec for _, r in hdr} != {r.rec for r in sc}:
            bad_scan.append(t)
        for f, r in hdr:
            roles[f].append((t, r))
    check("B2 env+0x98..+0xA4 resolve to >=3 texture records on every track",
          not bad_hdr, "counts %s  bad %s" % (counts, bad_hdr[:6]))
    check("B2 blind scan agrees with the header slots on every track",
          not bad_scan, "disagree: %s" % bad_scan[:6])

    # B2b -- the slots are ROLE-fixed even though the artists' names are not.
    # This is what lets the runtime find the right texture on any track.
    off = [(t, r.name) for t, r in roles[0x98]
           if not (r.w == 32 and r.h == 32 and r.fmt == 0xF)]
    check("B2b env+0x98 is a 32x32 DXT5 gradient LUT on every track (%d)"
          % len(roles[0x98]), not off, "%s" % off[:5])
    off = [(t, r.name, r.w) for t, r in roles[0x9C] if not (r.w == 1024 and r.fmt == 0xF)]
    check("B2b env+0x9C is a 1024-wide DXT5 cloud sheet on every track (%d)"
          % len(roles[0x9C]), not off, "%s" % off[:5])
    off = [(t, r.name) for t, r in roles[0xA4]
           if not (r.w == 128 and r.h == 256 and r.fmt == 0xB)]
    check("B2b env+0xA4 is a 128x256 paletted sun sprite on every track (%d)"
          % len(roles[0xA4]), not off, "%s" % off[:5])
    check("B2b names vary but roles do not (>=6 distinct gradient names)",
          len({r.name for _, r in roles[0x98]}) >= 6,
          "%d distinct" % len({r.name for _, r in roles[0x98]}))

    # B3 -- the gradient LUT source is 32x32, matching the 64x32 runtime LUT's
    # height, and the sky/ground v split lands on whole texel rows.
    d = open(os.path.join(TRACKS, "US/C3_V1", "enviro.dat"), 'rb').read()
    recs = {art.SLOT_ROLE[f]: r for f, r in art.records_from_header(d)}
    check("B3 US/C3_V1 gradients is 32x32",
          recs["gradients"].w == 32 and recs["gradients"].h == 32,
          "%dx%d" % (recs["gradients"].w, recs["gradients"].h))
    check("B3 US/C3_V1 clouds is 1024x128 DXT5",
          recs["clouds"].w == 1024 and recs["clouds"].h == 128
          and recs["clouds"].fmt == 0xF,
          "%dx%d fmt 0x%X" % (recs["clouds"].w, recs["clouds"].h, recs["clouds"].fmt))
    for v, texel in ((0.765625, 24.5), (0.015625, 0.5), (1.015625, 32.5)):
        check("B3 v %g is texel %g of 32" % (v, texel), abs(v * 32 - texel) < 1e-6,
              "%.4f" % (v * 32))

    # B4 -- the negative: no track's static.dat names a sky/cloud material.
    try:
        import extract_track as ET
    except ImportError as e:
        check("B4 extract_track imports", False, str(e))
        return
    hits, scanned = [], 0
    for t in tracks:
        p = os.path.join(TRACKS, t, "static.dat")
        if not os.path.exists(p):
            continue
        data = open(p, 'rb').read()
        mats = ET.parse_materials(ET.Reader(data), data)
        scanned += 1
        for i, m in mats.items():
            low = m.texture.lower()
            if low.startswith("sky") or "cloud" in low or low.endswith("sky"):
                hits.append("%s:%s" % (t, m.texture))
    check("B4 no static.dat material is a sky/cloud texture (%d tracks scanned)"
          % scanned, not hits and scanned >= 30, "hits: %s" % hits[:8])


# ========================================================= C. EXECUTED PORT
# Independent Python transcription of FUN_00032020, written from the
# disassembly -- NOT from burnout3_postfx.c.
def dome_reference(sky):
    THETA = 0.19634954          # DAT_004D916C
    TSTEP = 0.2617994           # DAT_004D9174
    TWO_PI_INV = 0.15915494
    TWO_OVER_PI = 0.63661975
    s = 1.0 if sky else -1.0
    v_low = 0.765625
    v_hi = 0.015625 if sky else 1.015625
    rng = v_hi - v_low
    verts = []
    for ring in range(33):
        th = ring * THETA
        ct, st = math.cos(th), math.sin(th)
        u0 = th * TWO_PI_INV
        verts.append(((ct, s * -0.25, st), 0x0000FF00,
                      (u0, -0.25 * rng + v_low)))
        for k in range(7):
            a = (k * TSTEP) * TWO_OVER_PI
            yhat = 2.0 * a - a * a
            rad = 1.0 - a * a
            verts.append(((ct * rad, yhat * s, st * rad), 0x0000FF00,
                          (u0, yhat * rng + v_low)))
    idx = []
    for c in range(16):
        for half in range(2):
            b0 = half * 0x80 + c * 8
            b1 = b0 + 8
            for j in range(7):
                idx += [b0 + j, b1 + j, b0 + 1 + j,
                        b1 + j, b0 + 1 + j, b1 + 1 + j]
    return verts, idx


class SkyVertex(ctypes.Structure):
    _fields_ = [("pos", ctypes.c_float * 3), ("color", ctypes.c_uint32),
                ("tc0", ctypes.c_float * 2), ("tc1", ctypes.c_float * 2)]


def section_c(defs):
    tmp = tempfile.mkdtemp(prefix="b3postfx_")
    so = os.path.join(tmp, "postfx_probe.so")
    cmd = ["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
           "-DB3_POSTFX_NO_GL", "-shared", "-fPIC",
           "-I" + os.path.join(ROOT, "src"), SRC, "-o", so, "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    check("C0 burnout3_postfx.c compiles clean with -DB3_POSTFX_NO_GL -Werror",
          r.returncode == 0, r.stderr.strip()[:400])
    if r.returncode != 0:
        return

    lib = ctypes.CDLL(so)
    check("C0b sizeof(B3SkyVertex) == 32 (the game's vertex stride)",
          ctypes.sizeof(SkyVertex) == 32, "%d" % ctypes.sizeof(SkyVertex))

    lib.b3_sky_build.restype = ctypes.c_int
    lib.b3_sky_build.argtypes = [ctypes.c_int, ctypes.POINTER(SkyVertex),
                                 ctypes.POINTER(ctypes.c_uint16)]
    lib.b3_postfx_blur_strength.restype = ctypes.c_float
    lib.b3_postfx_blur_strength.argtypes = [ctypes.c_float, ctypes.c_float]

    for sky in (1, 0):
        tag = "sky" if sky else "ground"
        V = (SkyVertex * 264)()
        I = (ctypes.c_uint16 * 1344)()
        n = lib.b3_sky_build(sky, V, I)
        check("C1 %s dome emits 264 vertices" % tag, n == 264, "%d" % n)

        ref_v, ref_i = dome_reference(sky)
        worst, worst_i = 0.0, -1
        for i in range(264):
            rp, rc, rt = ref_v[i]
            for j in range(3):
                d = abs(V[i].pos[j] - rp[j])
                if d > worst:
                    worst, worst_i = d, i
            for j in range(2):
                d = abs(V[i].tc0[j] - rt[j])
                if d > worst:
                    worst, worst_i = d, i
            if V[i].color != rc:
                worst, worst_i = 9.9, i
        check("C2 %s dome matches the independent transcription" % tag,
              worst < 1e-6, "worst |delta| %.3g at vertex %d" % (worst, worst_i))

        if sky:
            bad = [i for i in range(1344) if I[i] != ref_i[i]]
            check("C3 index buffer matches (1344 entries)", not bad,
                  "%d mismatches, first at %s" % (len(bad), bad[:3]))
            check("C3 every index is inside the 264-vertex block",
                  all(0 <= I[i] < 264 for i in range(1344)),
                  "max %d" % max(I))

        # C4 -- geometric properties the dome must have.
        ys = [V[i].pos[1] for i in range(264)]
        vs = [V[i].tc0[1] for i in range(264)]
        if sky:
            check("C4 sky dome spans y = -0.25 (skirt) .. +1.0 (zenith)",
                  abs(min(ys) + 0.25) < 1e-6 and abs(max(ys) - 1.0) < 1e-6,
                  "%.4f .. %.4f" % (min(ys), max(ys)))
            check("C4 sky v spans LUT texel 0.5 (zenith) .. 30.5 (skirt)",
                  abs(min(vs) * 32 - 0.5) < 1e-4 and abs(max(vs) * 32 - 30.5) < 1e-4,
                  "texels %.3f .. %.3f" % (min(vs) * 32, max(vs) * 32))
        else:
            check("C4 ground dome spans y = -1.0 (nadir) .. +0.25 (skirt)",
                  abs(min(ys) + 1.0) < 1e-6 and abs(max(ys) - 0.25) < 1e-6,
                  "%.4f .. %.4f" % (min(ys), max(ys)))
            check("C4 ground v spans LUT texel 22.5 (skirt) .. 32.5 (nadir)",
                  abs(min(vs) * 32 - 22.5) < 1e-4 and abs(max(vs) * 32 - 32.5) < 1e-4,
                  "texels %.3f .. %.3f" % (min(vs) * 32, max(vs) * 32))
        # both halves must meet exactly at the horizon row, texel 24.5
        horizon = [v for v in vs if abs(v - 0.765625) < 1e-6]
        check("C4 %s dome has a horizon ring at v texel 24.5" % tag,
              len(horizon) == 33, "%d vertices" % len(horizon))

    # C5 -- the radius must fall monotonically from the horizon to the pole and
    # the dome must close (radius 0 at the pole), or it is not a dome.
    V = (SkyVertex * 264)()
    lib.b3_sky_build(1, V, None)
    row_r = [math.hypot(V[k].pos[0], V[k].pos[2]) for k in range(8)]
    check("C5 dome radius falls 1.0 -> 0.0 from horizon row to zenith",
          abs(row_r[1] - 1.0) < 1e-6 and abs(row_r[7]) < 1e-6
          and all(row_r[i] > row_r[i + 1] - 1e-9 for i in range(1, 7)),
          "%s" % ["%.4f" % r for r in row_r])

    # C6 -- the blur strength law's boundary behaviour (GLUE curve, but its
    # endpoints are what the reference captures pin down).
    s0 = lib.b3_postfx_blur_strength(0.0, 0.0)
    s30 = lib.b3_postfx_blur_strength(30.0, 0.0)
    s120 = lib.b3_postfx_blur_strength(120.0, 0.0)
    s200 = lib.b3_postfx_blur_strength(200.0, 0.0)
    sb = lib.b3_postfx_blur_strength(120.0, 1.0)
    check("C6 blur is exactly zero at rest (the 0 mph capture is sharp)",
          s0 == 0.0, "%.4f" % s0)
    check("C6 blur is zero at the onset speed and rises above it",
          s30 == 0.0 and s120 > 0.3, "s30 %.4f s120 %.4f" % (s30, s120))
    check("C6 blur saturates (no runaway above the top of the ramp)",
          abs(s200 - s120) < 1e-6, "s120 %.4f s200 %.4f" % (s120, s200))
    check("C6 full boost ramp adds strength on top of speed", sb > s120,
          "no-boost %.4f boost %.4f" % (s120, sb))
    mono = all(lib.b3_postfx_blur_strength(float(v), 0.0)
               <= lib.b3_postfx_blur_strength(float(v + 1), 0.0) + 1e-7
               for v in range(0, 200))
    check("C6 blur strength is monotone in speed", mono)
    check("C6 blur strength stays inside the game's 0..2 range for `s`",
          0.0 <= lib.b3_postfx_blur_strength(400.0, 2.0) <= 2.0,
          "%.4f" % lib.b3_postfx_blur_strength(400.0, 2.0))

    # C7 -- the RECOVERED alpha law, run for real against a Python mirror of
    # FUN_0003DA90 @0x0003DC37..0x0003DC89 written from the disassembly.
    lib.b3_postfx_present_alpha.restype = ctypes.c_float
    lib.b3_postfx_present_alpha.argtypes = [ctypes.c_float]

    def ref_alpha(s):                    # 0003dc42 COMISS 2.0 / JBE
        a = (s * 0.5) if s <= 2.0 else 1.0
        return min(max(a, 0.0), 1.0)     # FUN_0034E9A0's MINPS/MAXPS pack

    worst, worst_s = 0.0, None
    for i in range(-40, 641):
        s = i / 100.0
        d = abs(lib.b3_postfx_present_alpha(s) - ref_alpha(s))
        if d > worst:
            worst, worst_s = d, s
    check("C7 present alpha matches the independent transcription over "
          "s = -0.4 .. 6.4", worst < 1e-6,
          "worst |delta| %.3g at s=%r" % (worst, worst_s))
    check("C7 alpha is 0 when the blur is idle (the composite is then a "
          "pure x2 of the render target)",
          lib.b3_postfx_present_alpha(0.0) == 0.0)
    check("C7 alpha saturates at 1 above s = 2 (the recovered branch)",
          lib.b3_postfx_present_alpha(2.0) == 1.0
          and lib.b3_postfx_present_alpha(50.0) == 1.0,
          "a(2)=%.3f a(50)=%.3f" % (lib.b3_postfx_present_alpha(2.0),
                                    lib.b3_postfx_present_alpha(50.0)))
    check("C7 header B3_PRESENT_SHIFT == 2 (the recovered SHIFTLEFTBY1)",
          abs(as_float(defs.get("B3_PRESENT_SHIFT", "0")) - 2.0) < 1e-9,
          "header %s" % defs.get("B3_PRESENT_SHIFT"))

    # C8 -- the gamma table the GL pass uploads, element by element against an
    # independent transcription of the loop at 0x0003D3F0.
    lib.b3_postfx_gamma_table.restype = None
    lib.b3_postfx_gamma_table.argtypes = [ctypes.POINTER(ctypes.c_ubyte)]
    T = (ctypes.c_ubyte * 256)()
    lib.b3_postfx_gamma_table(T)
    ref = [int(math.floor(((i / 255.0) ** 0.949999988079071) * 255.0 + 0.5))
           for i in range(256)]
    bad = [i for i in range(256) if T[i] != min(255, max(0, ref[i]))]
    check("C8 gamma table matches round((i/255)^0.95*255) for all 256 entries",
          not bad, "%d mismatches, first %s" % (len(bad), bad[:4]))
    check("C8 gamma table is a midtone LIFT and pins both endpoints",
          T[0] == 0 and T[255] == 255 and T[128] > 128 and T[40] > 40,
          "0->%d 40->%d 128->%d 255->%d" % (T[0], T[40], T[128], T[255]))
    check("C8 gamma table is monotone non-decreasing",
          all(T[i] <= T[i + 1] for i in range(255)))
    # the 256x1 GL_NEAREST lookup the pass uses must index the table exactly:
    # floor((i/255)*256) == i for every 8-bit input.
    check("C8 the 256x1 NEAREST LUT indexes exactly (floor(i/255*256) == i)",
          all(min(255, int((i / 255.0) * 256.0)) == i for i in range(256)))

    try:
        os.remove(so)
        os.rmdir(tmp)
    except OSError:
        pass


# ===================================================== D. THE TUNED DEFAULTS
# The LOOK wave (user-authorized deviation 2026-08-13) turned the recovered
# present composite ON by default and put the sky dome into render-target
# space to meet it.  These checks pin BOTH halves of that: that the recovered
# equations are still exactly what they were, and that the two magnitudes are
# marked, overridable, and switchable back off.
def section_d(defs):
    src = open(SRC).read()
    hdr = open(HDR).read()

    check("D1 the recovered present composite is ON by default "
          "(B3_POSTFX_PRESENT=0 is the off-switch)",
          "static int   present_on = 1;" in src
          and 'getenv("B3_POSTFX_PRESENT")' in src)

    # D2/D3 -- THE TUNED SKY GAIN IS RETIRED (SKY-LUT wave).
    #
    # B3_SKY_RT_GAIN used to be a fitted 0.57, chosen between the measured
    # per-channel ratios 75/138 .. 43/72.  The sky LUT recovery found what it
    # was standing in for: the dome does not sample the `gradients` sheet, it
    # samples the 64x32 render target FUN_001891F0 blits that sheet into
    # through a 0xFF808080 vertex colour -- i.e. at HALF.  The recovered 0.5
    # replaces the fit and the gain now ships at identity.
    #
    # Pinned as a MEASUREMENT, not a preference: with pass 1 alone the open-sky
    # band reads (81,95,146) against the retail 0 mph reference's (84,97,146)
    # -- blue exact, R and G inside 4% -- where the old fit left blue ~20% hot.
    # See INTEGRATION_NOTE.md for the full table.
    g = as_float(defs.get("B3_SKY_RT_GAIN", ""))
    check("D2 B3_SKY_RT_GAIN ships at IDENTITY -- the recovered blit colour "
          "carries the dome's render-target scale, not a fit",
          g is not None and abs(g - 1.0) < 1e-9, "%r" % g)
    blit = as_float(defs.get("B3_SKY_LUT_BLIT_RGB", ""))
    check("D3 the recovered blit colour that replaced it is 0.5 -- the 0x80 of "
          "FUN_001891F0's 0xFF808080 vertex colour -- and it lands inside the "
          "band the retired fit was drawn from",
          blit is not None and abs(blit - 0.5) < 1e-9
          and blit <= (43.0 / 72.0), "%r" % blit)
    check("D3 the retired fit is still reachable for A/B, and the blit colour "
          "is overridable on its own",
          'getenv("B3_POSTFX_SKYGAIN")' in src
          and 'getenv("B3_POSTFX_SKYBLIT")' in src)
    check("D4 the recovered cloud constant C0 = (0.5,0.5,0.5,1.0) is unchanged",
          as_float(defs.get("B3_SKY_CLOUD_C0_RGB", "")) == 0.5
          and as_float(defs.get("B3_SKY_CLOUD_C0_A", "")) == 1.0)
    check("D5 the gain multiplies the recovered cloud constant rather than "
          "replacing it",
          "B3_SKY_CLOUD_C0_RGB * postfx_sky_gain()" in src)
    check("D6 both dome passes take the same one gain (no per-channel fit)",
          src.count("postfx_sky_gain()") == 2
          and 'getenv("B3_POSTFX_SKYGAIN")' in src)
    # D7 -- the sky's two TUNED magnitudes are GONE (the dome gain and the
    # cloud gain both derive from the recovered blit colour now), so the only
    # marks left are the present composite's flipped default.  Both halves are
    # asserted: at least one mark survives AND no mark still sits on the sky
    # gain, which is what would mean the fit had crept back in.
    marks = (src + hdr).count("TUNED (user-authorized deviation 2026-08-13)")
    check("D7 every tuned magnitude still in this module carries the TUNED "
          "mark (the sky's two were retired by the recovered LUT)",
          marks >= 2, "%d marks" % marks)
    check("D7 no TUNED mark remains on the sky gain -- it is recovered now",
          "TUNED (user-authorized deviation 2026-08-13) -- B3_SKY_RT_GAIN"
          not in src)
    check("D8 the recovered composite itself is untouched: x2 via the "
          "DST_COLOR/ONE white quad, taps at the recovered LOD",
          "GL_DST_COLOR, GL_ONE" in src and "B3_PRESENT_BLUR_LOD" in src)


# ================================== E. THE SKY LUT, THE ARC FIX, THE FLIP FIX
# The SKY-LUT wave. Three things land here:
#
#   * FUN_001891F0's THREE blit passes, which rebuild the 64x32 gradient LUT
#     the dome actually samples. The port used to bind the 32x32 `gradients`
#     sheet directly at a constant u, which is exact for pass 1 alone and drops
#     the sun-glow passes entirely. Executed for real against an independent
#     Python transcription, same discipline as section C.
#   * the ARC fix: the present composite's 16 taps summed in ONE draw instead
#     of 16 additive 8-bit quads.
#   * the RESIZE fix: b3_postfx_flip_rows, exercised at a width ABOVE the 2048
#     the old fixed 8192-byte scratch row silently clamped to.
def section_e(img, defs):
    src = open(SRC).read()

    # ---- E1: the header's LUT constants, read back out of the image at the
    # addresses burnout3_postfx.h cites for them.
    for va, want, name, macro in [
            (0x003B1A90, 0.015625,  "pass 1/3 source v0", "B3_SKY_LUT_SRC_V0"),
            (0x003B1F50, 1.015625,  "pass 1/3 source v1", "B3_SKY_LUT_SRC_V1"),
            (0x003B0438, 15.5,      "pass 2 azimuth base", "B3_SKY_GLOW_U_BASE"),
            (0x003B168C, 1.0,       "pass 2 azimuth span", "B3_SKY_GLOW_U_SPAN"),
            (0x003B183C, 1.0 / 24.0, "pass 2 elevation 1/24", None),
            (0x003B1C30, 1.0 / 32.0, "pass 2 source 1/32", None)]:
        got = img.f32(va)
        ok = got is not None and abs(got - want) < 1e-6
        detail = "" if ok else "@0x%08X got %r want %r" % (va, got, want)
        if ok and macro:
            h = as_float(defs.get(macro, ""))
            ok = h is not None and abs(h - want) < 1e-6
            detail = "" if ok else "header %s = %r, image %r" % (macro, h, want)
        check("E1 %s @0x%08X%s" % (name, va, " -> " + macro if macro else ""),
              ok, detail)

    # the LUT is the 64x32 texture FUN_001A9C50 creates at 0x001A9FFD:
    # CreateTexture(0x40, 0x20, 1, format 6).
    check("E1 the LUT is 64x32 (CreateTexture 0x40 x 0x20 @0x001A9FFD)",
          as_float(defs.get("B3_SKY_LUT_W", "")) == 64.0
          and as_float(defs.get("B3_SKY_LUT_H", "")) == 32.0)
    # pass 1's column span is the sheet's LEFT half and pass 3's the RIGHT:
    # U_BASE = 0.5/32 (texel 0.5) and U_SPAN = 15/32 put pass 1 on texels
    # 0.5..15.5, and GLOW_U_OFFSET = 0.5 normalised is +16 texels.
    check("E1 the sheet is split base|glow: pass 1 spans texels 0.5..15.5 and "
          "pass 3 is +16 texels from it",
          abs(as_float(defs.get("B3_SKY_LUT_U_BASE", "")) - 0.5 / 32) < 1e-9
          and abs(as_float(defs.get("B3_SKY_LUT_U_SPAN", "")) - 15.0 / 32) < 1e-9
          and abs(as_float(defs.get("B3_SKY_GLOW_U_OFFSET", "")) - 0.5) < 1e-9)

    tmp = tempfile.mkdtemp(prefix="b3skylut_")
    so = os.path.join(tmp, "skylut_probe.so")
    cmd = ["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
           "-DB3_POSTFX_NO_GL", "-shared", "-fPIC",
           "-I" + os.path.join(ROOT, "src"), SRC, "-o", so, "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    check("E0 the LUT builder compiles GL-free (probeable)",
          r.returncode == 0, r.stderr.strip()[:400])
    if r.returncode != 0:
        return
    lib = ctypes.CDLL(so)

    # ---- E2: FUN_00189660, direction -> (azimuth turns, elevation parameter),
    # against an independent transcription.
    lib.b3_sky_sun_angles.restype = None
    lib.b3_sky_sun_angles.argtypes = [ctypes.POINTER(ctypes.c_float),
                                      ctypes.POINTER(ctypes.c_float),
                                      ctypes.POINTER(ctypes.c_float)]

    def sun(x, y, z):
        d = (ctypes.c_float * 3)(x, y, z)
        a, e = ctypes.c_float(), ctypes.c_float()
        lib.b3_sky_sun_angles(d, ctypes.byref(a), ctypes.byref(e))
        return a.value, e.value

    def ref_sun(x, y, z):                      # written from the decompilation
        n = math.sqrt(x * x + y * y + z * z)
        x, y, z = x / n, y / n, z / n
        a = (math.atan2(z, x) / (2.0 * math.pi)) % 1.0   # 0 at +X toward +Z
        e = (1.0 - math.sqrt(1.0 - y)) if y >= 0.0 else (math.sqrt(1.0 + y) - 1.0)
        return a, e

    worst_a = worst_e = 0.0
    n_az = 0
    for xi in range(-4, 5):
        for yi in range(-4, 5):
            for zi in range(-4, 5):
                if xi == yi == zi == 0:
                    continue
                ga, ge = sun(float(xi), float(yi), float(zi))
                ra, re = ref_sun(float(xi), float(yi), float(zi))
                worst_e = max(worst_e, abs(ge - re))
                if xi == 0 and zi == 0:
                    continue        # straight up/down: azimuth is undefined,
                                    # and both sides just fall out of atan2(0,0)
                da = min(abs(ga - ra), 1.0 - abs(ga - ra))   # wrap at the turn
                worst_a = max(worst_a, da)
                n_az += 1
    check("E2 FUN_00189660 azimuth matches the independent transcription over "
          "%d directions" % n_az, worst_a < 1e-5, "worst %.3g turns" % worst_a)
    check("E2 FUN_00189660 elevation parameter matches it too",
          worst_e < 1e-5, "worst %.3g" % worst_e)
    check("E2 the azimuth convention agrees with the dome's own tc0.u "
          "(0 at +X, 0.25 at +Z) so LUT column and dome azimuth line up",
          abs(sun(1.0, 0.0, 0.0)[0]) < 1e-5
          and abs(sun(0.0, 0.0, 1.0)[0] - 0.25) < 1e-5,
          "+X %.4f  +Z %.4f" % (sun(1, 0, 0)[0], sun(0, 0, 1)[0]))
    check("E2 straight up / straight down are the parameter's endpoints",
          abs(sun(0.0, 1.0, 0.0)[1] - 1.0) < 1e-5
          and abs(sun(0.0, -1.0, 0.0)[1] + 1.0) < 1e-5)

    # ---- E3: pass 2's elevation warp, 0x001893BC..0x001893F7 + the loop.
    lib.b3_sky_glow_v_texels.restype = ctypes.c_float
    lib.b3_sky_glow_v_texels.argtypes = [ctypes.c_float, ctypes.c_float]

    def ref_v(y, e):
        a_row = 1.0 - math.sqrt(max(y, 0.0) / 24.0)
        return min((0.5 - (a_row - e)) * 24.0, 24.0)

    worst = max(abs(lib.b3_sky_glow_v_texels(float(y) / 4.0, e / 100.0)
                    - ref_v(float(y) / 4.0, e / 100.0))
                for y in range(0, 129) for e in range(-100, 101, 7))
    check("E3 the elevation warp matches its independent transcription",
          worst < 1e-4, "worst %.3g texels" % worst)
    check("E3 the warp is CLAMPED at 24 texels (0x001893F5)",
          lib.b3_sky_glow_v_texels(24.0, 1.0) == 24.0
          and lib.b3_sky_glow_v_texels(30.0, 1.0) == 24.0)
    # v == 12 (the blob's centre row) exactly at the sun's own elevation:
    # a_row == sun_elev  =>  v = 0.5 * 24.
    for e in (0.0, 0.15, 0.2929, 0.5, 0.8):
        y = 24.0 * (1.0 - e) ** 2            # the row whose a_row == e
        check("E3 the glow centres on the sun's own elevation (elev %.4f "
              "-> v 12)" % e,
              abs(lib.b3_sky_glow_v_texels(y, e) - 12.0) < 1e-3,
              "v = %.4f" % lib.b3_sky_glow_v_texels(y, e))

    # ---- E4: the three passes, executed on a synthetic sheet whose left half,
    # right half and alpha are each identifiable, so every pass is separable.
    W = H = 32
    sheet = (ctypes.c_ubyte * (W * H * 4))()
    for y in range(H):
        for x in range(W):
            o = (y * W + x) * 4
            base = (x < 16)
            sheet[o + 0] = 40 if base else 200      # left = dark, right = light
            sheet[o + 1] = 40 if base else 200
            sheet[o + 2] = 80 if base else 200
            # alpha: a single spike at (texel 15.5, row 11.5), zero elsewhere
            sheet[o + 3] = 255 if (x in (15, 16) and y in (11, 12)) else 0
    out = (ctypes.c_ubyte * (64 * 32 * 4))()
    lib.b3_sky_build_lut.restype = None
    lib.b3_sky_build_lut.argtypes = [ctypes.POINTER(ctypes.c_ubyte),
                                     ctypes.c_int, ctypes.c_int,
                                     ctypes.c_float, ctypes.c_float,
                                     ctypes.c_float, ctypes.c_int,
                                     ctypes.POINTER(ctypes.c_ubyte)]

    def build(progress, az, elev, have_sun):
        lib.b3_sky_build_lut(sheet, W, H, ctypes.c_float(progress),
                             ctypes.c_float(az), ctypes.c_float(elev),
                             have_sun, out)
        return [[tuple(out[(y * 64 + x) * 4 + c] for c in range(4))
                 for x in range(64)] for y in range(32)]

    # pass 1 alone: horizontally uniform (this is exactly the old behaviour,
    # which is why binding the sheet at a constant u used to be exact), alpha 0,
    # and RGB at the recovered HALF of the sheet's left half.
    L = build(0.5, 0.0, 0.0, 0)
    uniform = all(L[y][x] == L[y][0] for y in range(32) for x in range(64))
    check("E4 pass 1 alone leaves the LUT horizontally uniform "
          "(the pre-wave constant-u binding was exact for it)", uniform)
    check("E4 pass 1 alone writes NO alpha (mode 3 is an RGB-only write mask)",
          all(L[y][0][3] == 0 for y in range(32)))
    check("E4 pass 1 lands on HALF the sheet -- the 0xFF808080 blit colour",
          L[8][0][0] == 20 and L[8][0][2] == 40,
          "got %r, want (20, 20, 40, 0)" % (L[8][0],))

    # all three passes, sun at azimuth 0.25 turns and 30 degrees up.
    az, elev = 0.25, 1.0 - math.sqrt(0.5)
    G = build(0.5, az, elev, 1)
    acol = [max(G[y][x][3] for y in range(24)) for x in range(64)]
    peak = acol.index(max(acol))
    want = int(az * 64 - 0.5)
    check("E4 pass 2 centres the halo on the SUN'S azimuth column",
          abs(peak - want) <= 1, "peak col %d, sun col %d" % (peak, want))
    check("E4 pass 2 leaves the far side of the dome unlit",
          acol[(peak + 32) % 64] == 0,
          "opposite col alpha %d" % acol[(peak + 32) % 64])
    check("E4 pass 2 writes NOTHING below the horizon -- the 13th quad's "
          "source v is env+0x180/0x184 = 0",
          all(G[y][x][3] == 0 for y in range(24, 32) for x in range(64)))
    check("E4 pass 2 is ALPHA-only: where its alpha is 0 the RGB is still "
          "exactly pass 1's",
          all(G[y][x][:3] == L[y][0][:3]
              for y in range(24) for x in range(64) if G[y][x][3] == 0))
    check("E4 pass 3 lerps toward the sheet's RIGHT half where the alpha is "
          "high (dst = src*dst.a + dst*(1-dst.a))",
          G[[y for y in range(24)
             if G[y][peak][3] == max(G[y2][peak][3]
                                     for y2 in range(24))][0]][peak][0] > 90)
    check("E4 have_sun == 0 reproduces the pre-wave sky exactly",
          build(0.5, az, elev, 0) == L)

    # ---- E5: THE RESIZE FIX. The old open-coded flip clamped its copy to a
    # fixed 8192-byte scratch row, so anything past pixel 2048 kept
    # glReadPixels' bottom-up order -- build/debug_dump_035.png.
    lib.b3_postfx_flip_rows.restype = None
    lib.b3_postfx_flip_rows.argtypes = [ctypes.POINTER(ctypes.c_ubyte),
                                        ctypes.c_int, ctypes.c_int]

    def flip_ok(w, h):
        buf = (ctypes.c_ubyte * (w * h * 4))()
        for y in range(h):
            for x in range(w):
                buf[(y * w + x) * 4] = (y * 7 + x * 3) & 0xFF
                buf[(y * w + x) * 4 + 1] = y & 0xFF
                buf[(y * w + x) * 4 + 2] = x & 0xFF
                buf[(y * w + x) * 4 + 3] = 0xFF
        want = [[(((h - 1 - y) * 7 + x * 3) & 0xFF, (h - 1 - y) & 0xFF,
                  x & 0xFF, 0xFF) for x in range(w)] for y in range(h)]
        lib.b3_postfx_flip_rows(buf, w, h)
        got = [[tuple(buf[(y * w + x) * 4 + c] for c in range(4))
                for x in range(w)] for y in range(h)]
        return got == want

    check("E5 flip is exact at the old safe width (2048, one whole 8192-byte "
          "scratch row)", flip_ok(2048, 24))
    check("E5 flip is exact at 3706 -- the width of build/debug_dump_035.png, "
          "where the old clamp gave up at column 2048", flip_ok(3706, 18))
    check("E5 flip is exact at an odd height (the middle row stays put)",
          flip_ok(2600, 17))
    check("E5 flip is exact at both parities of height",
          flip_ok(300, 8) and flip_ok(300, 9))

    def untouched(w, h):
        buf = (ctypes.c_ubyte * max(w * h * 4, 4))()
        for i in range(len(buf)):
            buf[i] = (i * 5) & 0xFF
        before = list(buf)
        lib.b3_postfx_flip_rows(buf, w, h)
        return list(buf) == before

    check("E5 degenerate sizes are left alone, not corrupted",
          untouched(64, 1) and untouched(64, 0) and untouched(0, 64))
    check("E5 no capture path open-codes a fixed-size scratch row any more",
          "row[8192]" not in open(
              os.path.join(ROOT, "src", "burnout3_full.c")).read(),
          "src/burnout3_full.c still has row[8192] -- apply "
          "scratchpad/skylut/patch_full_flip.py")

    # ---- E6: THE ARC FIX. The taps must be summed in one draw, and the shader
    # must be built from the very macros the fixed-function path uses.
    check("E6 the present taps are summed in ONE draw (the 16 additive 8-bit "
          "quads are what banded)",
          "postfx_taps_init()" in src and "g_taps_prog" in src)
    check("E6 the tap shader is generated from the same recovered/GLUE macros "
          "as the fallback, not from re-typed literals",
          "(double)B3_BLUR_CENTER_X" in src and "(double)B3_BLUR_ZOOM_A" in src
          and "B3_BLUR_TAPS, (double)B3_BLUR_ZOOM_A" in src
          and "(double)B3_BLUR_MASK_POW" in src)
    check("E6 the 16-quad path survives as the fallback and as the A/B switch",
          "postfx_quad_masked(powf(B3_BLUR_ZOOM_A" in src
          and 'getenv("B3_POSTFX_TAPSHADER")' in src)
    check("E6 both paths sample the SAME recovered reduction level",
          src.count("GL_TEXTURE_BASE_LEVEL,\n                            "
                    "B3_PRESENT_BLUR_LOD") == 1)

    try:
        os.remove(so)
        os.rmdir(tmp)
    except OSError:
        pass


# =================================================================== driver
def main():
    if not os.path.exists(ELF):
        print("missing %s -- run tools/xbe2elf.py first" % ELF)
        return 2
    img = Image(ELF)
    defs = header_defines()
    section_a(img, defs)
    section_b()
    section_c(defs)
    section_d(defs)
    section_e(img, defs)

    n = len(PASS) + len(FAIL)
    print("\nvalidate_postfx: %d/%d" % (len(PASS), n))
    if FAIL:
        print("\nFAILURES:")
        for name, detail in FAIL:
            print("  %s   %s" % (name, detail))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
