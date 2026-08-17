#!/usr/bin/env python3
"""
validate_hud.py -- differential test for the in-race HUD module.

Asserts that every layout / sequencing constant hard-coded in
src/burnout3_hud.h and src/burnout3_hud.c equals the value independently
read out of the retail executable (via the correctly-mapped ELF that
tools/xbe2elf.py produces -- NEVER a flat XBE load).

This is the HUD counterpart of validate_port.py / validate_gameplay.py:
the C is only allowed to hold a number the binary also holds.

Usage:
    python3 tools/validate_hud.py [--elf build/burnout3.elf]

Exit code 0 = all green.
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import argparse
import os
import re
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ELF = os.path.join(REPO, "build", "burnout3.elf")
XBE = (game_path('default.xbe'))
HDR = os.path.join(REPO, "src", "burnout3_hud.h")
SRC = os.path.join(REPO, "src", "burnout3_hud.c")


# --------------------------------------------------------------------- #
# correctly-mapped image reader (one PT_LOAD per XBE section at true VA)
# --------------------------------------------------------------------- #
class Image(object):
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise SystemExit("%s is not an ELF -- run tools/xbe2elf.py "
                             "(a flat XBE load is silently wrong)" % path)
        e_phoff, = struct.unpack_from("<I", d, 0x1C)
        e_phentsize, = struct.unpack_from("<H", d, 0x2A)
        e_phnum, = struct.unpack_from("<H", d, 0x2C)
        self.segs = []
        for i in range(e_phnum):
            o = e_phoff + i * e_phentsize
            (p_type, p_off, p_va, _pa, p_filesz, p_memsz,
             _fl, _al) = struct.unpack_from("<8I", d, o)
            if p_type == 1:
                self.segs.append((p_va, p_memsz, p_off, p_filesz))
        if not self.segs:
            raise SystemExit("no PT_LOAD segments in %s" % path)

    def read(self, va, n):
        out = bytearray()
        for i in range(n):
            out.append(self._byte(va + i))
        return bytes(out)

    def _byte(self, va):
        for p_va, memsz, off, filesz in self.segs:
            if p_va <= va < p_va + memsz:
                d = va - p_va
                return self.data[off + d] if d < filesz else 0
        raise KeyError("VA %08x is not mapped" % va)

    def f32(self, va):
        return struct.unpack("<f", self.read(va, 4))[0]

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def u8(self, va):
        return self._byte(va)

    def cstr(self, va, maxn=128):
        out = bytearray()
        for i in range(maxn):
            b = self._byte(va + i)
            if b == 0:
                break
            out.append(b)
        return out.decode("latin1")

    # ----------------------------------------------------------------- #
    # FUN_001C7150 fills the render-state PRESET tables that
    # FUN_001C8470 / FUN_001C82E0 index with compiled-in immediates; the
    # tables themselves read 0 in the image (BSS-like).  Replay the
    # function's stores to recover them.  The body 0x001C7168..0x001C72E3
    # is straight-line mov/movss only -- anything else is a decoder gap
    # and must fail loudly rather than silently mis-read a table.
    # ----------------------------------------------------------------- #
    RSINIT_LO = 0x001C7168
    RSINIT_HI = 0x001C72E3

    def preset_tables(self):
        if getattr(self, "_presets", None) is not None:
            return self._presets
        out = {}
        reg = {}
        xmm0 = 0
        va = self.RSINIT_LO
        while va < self.RSINIT_HI:
            b = self.read(va, 10)
            if b[0] == 0x0F and b[1] == 0x57 and b[2] == 0xC0:      # xorps xmm0
                xmm0 = 0
                va += 3
            elif b[0] == 0xF3 and b[1] == 0x0F and b[2] == 0x11 \
                    and b[3] == 0x05:                                # movss [m],xmm0
                out[struct.unpack_from("<I", b, 4)[0]] = xmm0
                va += 8
            elif 0xB8 <= b[0] <= 0xBF:                               # mov r32,imm32
                reg[b[0] - 0xB8] = struct.unpack_from("<I", b, 1)[0]
                va += 5
            elif b[0] == 0xA3:                                       # mov [m],eax
                out[struct.unpack_from("<I", b, 1)[0]] = reg.get(0, None)
                va += 5
            elif b[0] == 0x89 and (b[1] & 0xC7) == 0x05:             # mov [m],r32
                out[struct.unpack_from("<I", b, 2)[0]] = \
                    reg.get((b[1] >> 3) & 7, None)
                va += 6
            elif b[0] == 0xC7 and b[1] == 0x05:                      # mov [m],imm32
                out[struct.unpack_from("<I", b, 2)[0]] = \
                    struct.unpack_from("<I", b, 6)[0]
                va += 10
            elif b[0] in (0x5E, 0x56, 0x90, 0xC3):                   # pop/push/nop
                va += 1
            else:
                raise SystemExit(
                    "preset-table decoder hit an unknown opcode at %08x "
                    "(%s) -- widen it rather than guessing" % (va, b[:4].hex()))
        self._presets = out
        return out

    def preset(self, table_va, index):
        t = self.preset_tables()
        va = table_va + index * 4
        if va not in t:
            raise SystemExit("FUN_001C7150 never writes preset slot "
                             "%08x[%d] (%08x)" % (table_va, index, va))
        return t[va]

    def bss_default(self, thunk_va):
        """Decode a C++ dynamic-initialiser thunk of the two shapes the
        compiler emitted for the HUD's file-scope floats:
            F3 0F 10 05 <const>   F3 0F 11 05 <global>   C3   (= const)
            0F 57 C0              F3 0F 11 05 <global>   C3   (= 0.0)
        Returns (default_value, storage_va)."""
        b = self.read(thunk_va, 20)
        if b[0:4] == b"\xF3\x0F\x10\x05" and b[8:12] == b"\xF3\x0F\x11\x05" \
                and b[16] == 0xC3:
            cva = struct.unpack_from("<I", b, 4)[0]
            gva = struct.unpack_from("<I", b, 12)[0]
            return self.f32(cva), gva
        if b[0:3] == b"\x0F\x57\xC0" and b[3:7] == b"\xF3\x0F\x11\x05" \
                and b[11] == 0xC3:
            gva = struct.unpack_from("<I", b, 7)[0]
            return 0.0, gva
        raise SystemExit("thunk %08x does not match a known initialiser "
                         "shape (bytes %s)" % (thunk_va, b[:17].hex()))


# --------------------------------------------------------------------- #
# checks
# --------------------------------------------------------------------- #
class Checker(object):
    def __init__(self):
        self.ok = 0
        self.fail = []

    def eq(self, name, got, want, tol=0.0, note=""):
        if isinstance(want, float) or isinstance(got, float):
            good = abs(float(got) - float(want)) <= max(tol, abs(float(want)) * 1e-5)
        else:
            good = got == want
        if good:
            self.ok += 1
            print("  ok   %-28s %-16s %s" % (name, _fmt(want), note))
        else:
            self.fail.append((name, got, want, note))
            print("  FAIL %-28s C=%s binary=%s %s"
                  % (name, _fmt(got), _fmt(want), note))


def _fmt(v):
    if isinstance(v, float):
        return "%.9g" % v
    if isinstance(v, int):
        return str(v)
    return repr(v)


TABLE_RE = re.compile(
    r"^#define\s+(B3HUD_[A-Z0-9_]+)\s+"
    r"(-?[0-9.]+f|-?0x[0-9A-Fa-f]+|-?[0-9]+)\s*"
    r"/\*\s*(.*?)\s*\*/\s*$")

CITE_VA = re.compile(r"@(0x[0-9A-Fa-f]{8})")
CITE_BSS = re.compile(r"@bss:(0x[0-9A-Fa-f]{8})=(0x[0-9A-Fa-f]{8})")
CITE_IMM32 = re.compile(r"imm32@(0x[0-9A-Fa-f]{8})")
CITE_IMM8 = re.compile(r"imm8@(0x[0-9A-Fa-f]{8})")
CITE_PRESET = re.compile(r"@preset:(0x[0-9A-Fa-f]{8})\[(\d+)\]")

INT_NAMES = ("_VA", "_SLOT", "_FRAMES", "_SEGMENTS", "_SPARKS",
             "_CATS", "_TIERS", "_ALPHA", "_SRC", "_DST", "_ADD",
             "_WRAP", "_CLAMP", "_RGB")


def parse_table(path):
    rows = []
    inside = False
    with open(path) as f:
        for line in f:
            if "B3HUD-TABLE-BEGIN" in line:
                inside = True
                continue
            if "B3HUD-TABLE-END" in line:
                break
            if not inside:
                continue
            m = TABLE_RE.match(line.strip())
            if m:
                rows.append((m.group(1), m.group(2), m.group(3)))
    if not rows:
        raise SystemExit("no B3HUD_* rows parsed from %s -- did the table "
                         "block get reformatted?" % path)
    return rows


def cval(text):
    t = text.rstrip("fF")
    if t.lower().startswith("0x") or t.lower().startswith("-0x"):
        return int(t, 16)
    if "." in t:
        return float(t)
    return int(t)


def check_table(img, ck):
    print("\n[1] src/burnout3_hud.h machine-checked constant table")
    derived = {"B3HUD_TREAD_SEGMENTS"}
    for name, text, note in parse_table(HDR):
        v = cval(text)
        is_int = any(name.endswith(s) for s in INT_NAMES)

        if name in derived:
            continue

        m = CITE_PRESET.search(note)
        if m:
            ck.eq(name, v, img.preset(int(m.group(1), 16), int(m.group(2))),
                  note=note)
            continue
        m = CITE_IMM32.search(note)
        if m:
            ck.eq(name, v, img.u32(int(m.group(1), 16)), note=note)
            continue
        m = CITE_IMM8.search(note)
        if m:
            ck.eq(name, v, img.u8(int(m.group(1), 16)), note=note)
            continue
        m = CITE_BSS.search(note)
        if m:
            storage = int(m.group(1), 16)
            thunk = int(m.group(2), 16)
            got, gva = img.bss_default(thunk)
            if gva != storage:
                ck.fail.append((name, gva, storage, "thunk writes wrong global"))
                print("  FAIL %-28s thunk %08x writes %08x, header says %08x"
                      % (name, thunk, gva, storage))
                continue
            ck.eq(name, v, got, note=note)
            continue
        m = CITE_VA.search(note)
        if m:
            va = int(m.group(1), 16)
            if name.endswith("_VA"):
                ck.eq(name, v, va, note="self-consistent table address")
            elif is_int:
                ck.eq(name, v, img.u32(va), note=note)
            else:
                ck.eq(name, v, img.f32(va), note=note)
            continue
        print("  --   %-28s no citation, skipped (%s)" % (name, note))

    # the one derived constant
    seg = None
    for name, text, _n in parse_table(HDR):
        if name == "B3HUD_TREAD_SEG_FRAC":
            seg = cval(text)
    ck.eq("B3HUD_TREAD_SEGMENTS", 9, int(round(1.0 / seg)),
          note="derived 1/TREAD_SEG_FRAC")


def check_box(img, ck):
    print("\n[2] the boost bar's screen box, resolved the way the game does")
    w = img.f32(0x003FCAA0)            # FUN_00048800 @0x0004880D
    h = img.f32(0x003FCAA4)            # FUN_00048800 @0x00048822
    fx = img.f32(0x003FD410 + 3 * 8)   # FUN_00053BE0 slot table, slot 3
    fy = img.f32(0x003FD410 + 3 * 8 + 4)
    ax, ay = 0.0, img.f32(0x003B168C)  # FUN_0004BFC0 @0x0004C05E/@0x0004C056
    sw = img.f32(0x003B1F00)
    sh = img.f32(0x003B1EEC)
    bx = fx * sw - ax * w
    by = fy * sh - ay * h
    ck.eq("box.w", 360.0, w, note="0x003FCAA0")
    ck.eq("box.h", 28.0, h, note="0x003FCAA4")
    ck.eq("box.x", 0.0, bx, note="slot3.x*640 - 0*W")
    ck.eq("box.y", 452.0, by, note="slot3.y*480 - 1*H")
    print("       => boost bar occupies (%.0f,%.0f)..(%.0f,%.0f) of 640x480"
          % (bx, by, bx + w, by + h))

    # the C source must place it there too
    src = open(SRC).read()
    assert "BOOST_X" in src and "BOOST_Y" in src
    ck.eq("C uses the slot table", True,
          "B3HUD_BOOST_SLOT_FX * B3HUD_VIRT_W" in src, note="BOOST_X macro")
    ck.eq("C uses the anchor", True,
          "B3HUD_BOOST_ANCHOR_Y * B3HUD_BOOST_H" in src, note="BOOST_Y macro")


def check_anchor_table(img, ck):
    print("\n[3] anchor-slot table 0x003FD410 (FUN_00053BE0)")
    want = [(0.5, 0.0), (0.0, 0.0), (0.0, 0.5), (0.0, 1.0), (0.5, 1.0),
            (1.0, 1.0), (1.0, 0.5), (1.0, 0.0), (0.0, 0.0)]
    names = ["top-centre", "top-left", "mid-left", "bottom-left",
             "bottom-centre", "bottom-right", "mid-right", "top-right",
             "viewport-origin"]
    for i, (wx, wy) in enumerate(want):
        gx = img.f32(0x003FD410 + i * 8)
        gy = img.f32(0x003FD410 + i * 8 + 4)
        ck.eq("slot[%d]" % i, (wx, wy), (gx, gy) if (gx, gy) == (wx, wy)
              else (gx, gy), note=names[i])


PROFILE_RE = re.compile(r"B3_TREAD_PROFILE\[10\]\[6\]\s*=\s*\{(.*?)\};",
                        re.S)
COLOR_RE = re.compile(r"B3_TREAD_(C[01])\[4\]\s*=\s*\{([^}]*)\}")


def check_tread_tables(img, ck):
    print("\n[4] BoostBits tread tables (FUN_000496E0)")
    src = open(SRC).read()
    m = PROFILE_RE.search(src)
    if not m:
        raise SystemExit("B3_TREAD_PROFILE not found in burnout3_hud.c")
    nums = [float(x) for x in re.findall(r"-?\d+\.\d+", m.group(1))]
    if len(nums) != 60:
        raise SystemExit("B3_TREAD_PROFILE has %d numbers, expected 60"
                         % len(nums))
    bad = 0
    for r in range(10):
        for c in range(6):
            want = img.f32(0x003AB038 + (r * 6 + c) * 4)
            if abs(nums[r * 6 + c] - want) > 1e-6:
                bad += 1
                print("  FAIL profile[%d][%d] C=%g binary=%g"
                      % (r, c, nums[r * 6 + c], want))
    ck.eq("tread profile 0x003AB038 (10x6)", 0, bad,
          note="60 floats, exact")

    for tag, base in (("C0", 0x003FCAC0), ("C1", 0x003FCAD0)):
        mm = COLOR_RE.search(src.replace("C0", "C0").replace("C1", "C1"))
        vals = None
        for mm in COLOR_RE.finditer(src):
            if mm.group(1) == tag:
                vals = [float(x) for x in
                        re.findall(r"-?\d+\.\d+", mm.group(2))]
        if vals is None or len(vals) != 4:
            raise SystemExit("B3_TREAD_%s not parsed" % tag)
        bad = 0
        for i in range(4):
            if abs(vals[i] - img.f32(base + i * 4)) > 1e-6:
                bad += 1
                print("  FAIL %s[%d] C=%g binary=%g"
                      % (tag, i, vals[i], img.f32(base + i * 4)))
        ck.eq("tread colour %s @%08x" % (tag, base), 0, bad, note="4 floats")


CALLOUT_RE = re.compile(
    r"B3_CALLOUT\[B3HUD_CALLOUT_CATS\]\[B3HUD_CALLOUT_TIERS\]\s*=\s*\{(.*?)\n\};",
    re.S)


def check_callouts(img, ck):
    print("\n[5] boost-earn callout strings (table 0x003C8390)")
    src = open(SRC).read()
    m = CALLOUT_RE.search(src)
    if not m:
        raise SystemExit("B3_CALLOUT table not found in burnout3_hud.c")
    strings = re.findall(r'"([^"]*)"', m.group(1))
    if len(strings) != 16:
        raise SystemExit("B3_CALLOUT has %d strings, expected 16"
                         % len(strings))
    bad = 0
    for i in range(16):
        ptr = img.u32(0x003C8390 + i * 4)
        want = img.cstr(ptr)
        if strings[i] != want:
            bad += 1
            print("  FAIL callout[%d] C=%r binary=%r" % (i, strings[i], want))
    ck.eq("callout strings [cat][tier]", 0, bad,
          note="16 entries, AIR/DRIFT/ONCOMING/NEARMISS x GOOD..AWESOME")
    print("       binary order: " + ", ".join(
        img.cstr(img.u32(0x003C8390 + i * 4)) for i in (0, 3, 12, 15)))


def check_art(ck):
    print("\n[6] extracted art vs the strip lengths the binary binds")
    d = os.path.join(REPO, "build", "frontend")
    if not os.path.isdir(d):
        print("  --   build/frontend missing, skipped "
              "(run tools/extract_txd.py)")
        return
    for stem, n in (("BoostFireEdge", 41), ("BoostFireCore", 30),
                    ("BoostFireOver", 20)):
        got = sum(1 for i in range(1, n + 1)
                  if os.path.exists(os.path.join(d, "%s%02d.png" % (stem, i))))
        ck.eq("%s frames" % stem, n, got, note="build/frontend")
    # EATrax is the EA TRAX now-playing banner's badge (RE_FRONTEND 6.9);
    # hud_element01 is THE plate -- the POS / LAP / speed corners and the
    # banner all draw it as a three-slice stretch (RE_FRONTEND 6.10).
    # big_curve stays in the check as a bank-integrity canary: it is the
    # menu asset the HUD used to draw by mistake.
    for f in ("BoostBits", "BoostEarnFlame", "GlobalFont", "big_curve",
              "hud_element01", "EATrax"):
        ck.eq("%s.png" % f, True,
              os.path.exists(os.path.join(d, f + ".png")), note="")


def check_state_machine_rates(img, ck):
    """The FUN_0004C390 rates the C mirrors, re-read at their own VAs so a
    mistyped rate in the C body (not just the header) is caught."""
    print("\n[7] FUN_0004C390 rates present in the C body")
    src = open(SRC).read()
    pairs = [
        ("B3HUD_FILL_RISE_RATE", "b->fill += B3HUD_FILL_RISE_RATE * dt"),
        ("B3HUD_FLAME_RISE_RATE", "b->flame += B3HUD_FLAME_RISE_RATE * dt"),
        ("B3HUD_FLAME_FALL_RATE", "b->flame -= B3HUD_FLAME_FALL_RATE * dt"),
        ("B3HUD_EARNFLASH_RISE_RATE",
         "b->earn_flash += B3HUD_EARNFLASH_RISE_RATE * dt"),
        ("B3HUD_EARNFLASH_FALL_RATE",
         "b->earn_flash -= B3HUD_EARNFLASH_FALL_RATE * dt"),
        ("B3HUD_SPARK_RATE", "b->spark_acc += B3HUD_SPARK_RATE * dt"),
        ("B3HUD_EARN_DELTA_THRESH",
         "(target - b->fill) > B3HUD_EARN_DELTA_THRESH"),
        ("B3HUD_TIER_STEP", "(float)seg * B3HUD_TIER_STEP"),
    ]
    for name, snippet in pairs:
        ck.eq(name + " used", True, snippet in src, note=snippet)
    # frame sequencing must go through the symbolic rates, not literals
    for rate, count in (("B3HUD_EDGE_RATE", "g_edge_n"),
                        ("B3HUD_CORE_RATE", "g_core_n"),
                        ("B3HUD_OVER_RATE", "g_over_n")):
        ck.eq(rate + " drives strip_frame", True,
              ("strip_frame(%s" % rate) in src or
              ("rate = %s" % rate) in src, note="")
    ck.eq("strip_frame = (int)(rate*clock) mod count", True,
          "int f = (int)(rate * clock) % count;" in src, note="")


# --------------------------------------------------------------------- #
# [8] the per-section vertex colours the C emits
# --------------------------------------------------------------------- #
COLOUR_RE = re.compile(
    r"static const float (B3_[A-Z0-9_]+)\[4\]\s*=\s*\{([^}]*)\}\s*;\s*"
    r"/\*\s*(0x[0-9A-Fa-f]{6,8})\s*\*/")
RAMP_RE = re.compile(
    r"static const float B3_SPARK_RAMP\[3\]\[4\]\s*=\s*\{(.*?)\n\};", re.S)


def check_section_colours(img, ck):
    print("\n[8] per-section vertex colours (float4 * node.rgba)")
    src = open(SRC).read()
    found = set()
    for m in COLOUR_RE.finditer(src):
        name, body, va = m.group(1), m.group(2), int(m.group(3), 16)
        vals = [float(x) for x in re.findall(r"-?\d+(?:\.\d+)?", body)]
        if len(vals) != 4:
            raise SystemExit("%s did not parse as 4 floats" % name)
        bad = sum(1 for i in range(4)
                  if abs(vals[i] - img.f32(va + i * 4)) > 1e-6)
        ck.eq("%s @%08x" % (name, va), 0, bad,
              note="{%s}" % ", ".join("%g" % img.f32(va + i * 4)
                                      for i in range(4)))
        found.add(name)
    for want in ("B3_PLATE_COL", "B3_TREAD_C0", "B3_TREAD_C1",
                 "B3_EARN_COL", "B3_CORE_COL", "B3_OVER_COL"):
        if want not in found:
            raise SystemExit("%s not found in burnout3_hud.c" % want)

    m = RAMP_RE.search(src)
    if not m:
        raise SystemExit("B3_SPARK_RAMP not found in burnout3_hud.c")
    nums = [float(x) for x in re.findall(r"-?\d+\.\d+", m.group(1))]
    if len(nums) != 12:
        raise SystemExit("B3_SPARK_RAMP has %d numbers, expected 12"
                         % len(nums))
    bad = sum(1 for i in range(12)
              if abs(nums[i] - img.f32(0x003FCB10 + i * 4)) > 1e-6)
    ck.eq("B3_SPARK_RAMP @003FCB10", 0, bad, note="3 x float4, FUN_0004A740")


# --------------------------------------------------------------------- #
# [9] the render-state presets, in full
# --------------------------------------------------------------------- #
def check_render_state_presets(img, ck):
    print("\n[9] FUN_001C7150's render-state preset tables")
    #  (table VA, index, expected, what it is)
    want = [
        (0x004A1A90, 0, 0x0302, "SRCBLEND  preset0 = GL_SRC_ALPHA"),
        (0x004A1AB0, 0, 0x0303, "DESTBLEND preset0 = GL_ONE_MINUS_SRC_ALPHA"),
        (0x004A1A90, 1, 0x0302, "SRCBLEND  preset1 = GL_SRC_ALPHA"),
        (0x004A1AB0, 1, 0x0001, "DESTBLEND preset1 = GL_ONE   (ADDITIVE)"),
        (0x004A1B00, 0, 0x8006, "BLENDOP   preset0 = GL_FUNC_ADD"),
        (0x004A1B00, 1, 0x8006, "BLENDOP   preset1 = GL_FUNC_ADD"),
        (0x004A1B34, 0, 0x010101, "COLORWRITEENABLE preset0 = RGB"),
        (0x004A1B34, 1, 0x010101, "COLORWRITEENABLE preset1 = RGB"),
        (0x004A1B24, 0, 3, "ADDRESSU preset0 = D3DTADDRESS_CLAMP"),
        (0x004A1B68, 0, 3, "ADDRESSV preset0 = D3DTADDRESS_CLAMP"),
        (0x004A1B24, 1, 1, "ADDRESSU preset1 = D3DTADDRESS_WRAP"),
        (0x004A1B68, 1, 1, "ADDRESSV preset1 = D3DTADDRESS_WRAP"),
    ]
    for va, i, exp, note in want:
        ck.eq("preset %08x[%d]" % (va, i), exp, img.preset(va, i), note=note)

    # and the C must map them to the matching GL enums
    src = open(SRC).read()
    for tag, snippet in (
            ("plate blend", "glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)"),
            ("fire blend", "glBlendFunc(GL_SRC_ALPHA, GL_ONE)"),
            ("blend equation", "glBlendEquation(GL_FUNC_ADD)"),
            ("colour mask", "glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE)"),
            ("plate wrap", "GL_TEXTURE_WRAP_S, GL_REPEAT"),
            ("fire clamp", "GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE")):
        ck.eq(tag, True, snippet in src, note=snippet)
    ck.eq("draw order plate->earn->tread->core->edge->over", True,
          src.index("boost_plate(x, y") < src.index("boost_earn_flame(x, y")
          < src.index("boost_tread(x, y") < src.index("boost_core(x, y")
          < src.index("boost_edge(x, y") < src.index("boost_over(x, y"),
          note="FUN_0004AE40")


# --------------------------------------------------------------------- #
# [10] differential: the REAL draw callback executed under Unicorn
# --------------------------------------------------------------------- #
SECTION_EXPECT = {
    # section -> (srcblend, dstblend, addressU, addressV)
    "plate": (0x0302, 0x0303, 1, 1),
    "tread": (0x0302, 0x0001, 3, 3),
    "core":  (0x0302, 0x0001, 3, 3),
    "earn":  (0x0302, 0x0001, 3, 3),
    "edge":  (0x0302, 0x0001, 3, 3),
    "over":  (0x0302, 0x0001, 3, 3),
    "sparks": (0x0302, 0x0001, 3, 3),
}
# section -> the ARGB the game's own vertices must carry, for the state
# seeded below (node.rgba = {1,1,1,1}).
COLOUR_EXPECT = {
    "plate": [0xFFFFFFFF],                  # 0x3FCAB0 {1,1,1,1}
    "core":  [0xAAFFFF00, 0xAA000000],      # 0x3FCAF0 {1,1,0,0.67} -> black
    "edge":  [0xFFFFFFFF],                  # node.rgba, untinted
    "over":  [0x00000000, 0xCCFFFF00],      # 0x3FCB00 {1,1,0,0.8}
}


def check_draw_capture(ck):
    print("\n[10] FUN_0004AE40 executed under Unicorn (tools/emulate_hud.py)")
    try:
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import emulate_hud                                    # noqa: E402
    except Exception as e:                                    # pragma: no cover
        print("  --   skipped: %s" % e)
        return
    t = emulate_hud.HudTrace()
    ev = t.draw(clock=0.79, A=1.0, B=1.0, earnflash=0.0, flame=1.0)
    section = None
    seen = {}
    for c in ev:
        if isinstance(c, tuple):
            section = c[1]
            continue
        seen.setdefault(section, []).append(c)
    for name in ("plate", "tread", "core", "edge", "over"):
        caps = seen.get(name)
        if not caps:
            ck.eq("%s section drew" % name, True, False, note="no batch")
            continue
        src, dst, au, av = SECTION_EXPECT[name]
        c = caps[0]
        ck.eq("%s blend" % name, (src, dst), (c.rs["src"], c.rs["dst"]),
              note=c.blend())
        ck.eq("%s tex address" % name, (au, av), (c.tss["u"], c.tss["v"]),
              note=c.address())
        ck.eq("%s COLORWRITEENABLE" % name, 0x010101, c.rs["cw"], note="RGB")
        if name in COLOUR_EXPECT:
            got = []
            for cc in caps:
                for col in cc.colours():
                    if col not in got:
                        got.append(col)
            ck.eq("%s vertex colours" % name, sorted(COLOUR_EXPECT[name]),
                  sorted(got),
                  note=" ".join("#%08X" % g for g in sorted(got)))
    # the plate is the headline fix: assert the two rects the game emits
    p = seen["plate"][0]
    v = p.verts
    ck.eq("plate texture", "boostbits", p.tex, note="bound @0x0004AF0B")
    ck.eq("plate body v", (0.130859375, 0.244140625),
          (round(min(x[3] for x in v[:4]), 9),
           round(max(x[3] for x in v[:4]), 9)), note="BoostBits y 33.5..62.5")
    ck.eq("plate cap v (tier 4)", (0.755859375, 0.869140625),
          (round(min(x[3] for x in v[4:]), 9),
           round(max(x[3] for x in v[4:]), 9)),
          note="4*0.25 - 0.244140625")
    ck.eq("plate body tiles (u max)", 5.0,
          round(max(x[2] for x in v[:4]), 6), note="(A - 1/6) * 6, WRAP")
    ck.eq("plate spans the whole box", (0.0, 360.0),
          (round(min(x[0] for x in v), 3), round(max(x[0] for x in v), 3)),
          note="x 0..360 at A=1")


# --------------------------------------------------------------------- #
# [11] the EVENT TICKER -- who owns it, what drives it, what it says
# --------------------------------------------------------------------- #
GLOBALUS = [os.path.join(REPO, "build", "Globalus.bin"),
            (game_path('Data/Globalus.bin'))]

TICK_C_RE = re.compile(
    r"B3_TICK\[B3_HUD_TICK_ROWS\]\s*=\s*\{(.*?)\n\};", re.S)
TICK_ROW_RE = re.compile(
    r'\{\s*"([^"]*)",\s*(B3HUD_TICK_STR_[A-Z]+)\s*/\s*4,\s*'
    r'([A-Za-z0-9_.]+),\s*([01]),\s*(0x[0-9A-Fa-f]+)\s*\}')


def rel32(img, va):
    """The target of the E8/E9 at va."""
    off = struct.unpack("<i", img.read(va + 1, 4))[0]
    return (va + 5 + off) & 0xFFFFFFFF


def globalus_strings():
    for p in GLOBALUS:
        if os.path.exists(p):
            d = open(p, "rb").read()
            n, = struct.unpack_from("<I", d, 8)

            def get(i, d=d, n=n):
                if i < 0 or i >= n:
                    return None
                o, = struct.unpack_from("<I", d, 0x10 + i * 4)
                e = o
                while e + 1 < len(d) and (d[e] or d[e + 1]):
                    e += 2
                return d[o:e].decode("utf-16-le")
            return get, p
    return None, None


def decode_probes(img):
    """FUN_0004D310's six FUN_0004D130 call sites, decoded straight out of
    .text: each is  push <thresh> ; mov edx,[edi+0x68C] ; add edx,<rec> ;
    mov eax,<row> (or xor eax,eax) ; call FUN_0004D130."""
    lo, hi = 0x0004D310, 0x0004D3F7
    code = img.read(lo, hi - lo)
    out = []
    thresh = None
    rec = None
    row = None
    i = 0
    while i < len(code) - 6:
        b = code[i]
        if b == 0x68:                                   # push imm32
            thresh = struct.unpack_from("<f", code, i + 1)[0]
            i += 5
            continue
        if b == 0x6A:                                   # push imm8
            thresh = float(code[i + 1])
            i += 2
            continue
        if code[i:i + 2] == b"\x81\xC2":                # add edx, imm32
            rec = struct.unpack_from("<I", code, i + 2)[0]
            i += 6
            continue
        if b == 0xB8:                                   # mov eax, imm32
            row = struct.unpack_from("<I", code, i + 1)[0]
            i += 5
            continue
        if code[i:i + 2] == b"\x33\xC0":                # xor eax,eax
            row = 0
            i += 2
            continue
        if b == 0xE8 and rel32(img, lo + i) == 0x0004D130:
            out.append((row, rec, thresh))
            thresh = rec = row = None
            i += 5
            continue
        i += 1
    return out


def check_ticker(img, ck):
    print("\n[11] the EVENT TICKER -- element ownership, probes, labels")
    src = open(SRC).read()

    # -- (a) it is the BOOST BAR element's second job -------------------- #
    ck.eq("FUN_0004D800 -> FUN_0004C390", 0x0004C390, rel32(img, 0x0004D80E),
          note="the boost bar update")
    ck.eq("FUN_0004D800 -> FUN_0004D310", 0x0004D310, rel32(img, 0x0004D820),
          note="the ticker update, gated on obj+0x56A")
    ck.eq("gate reads obj+0x56A", 0x56A,
          struct.unpack("<I", img.read(0x0004D815, 4))[0],
          note="mov al,[esi+0x56A] @0x0004D813")

    # -- (b) the row's own draw node ------------------------------------- #
    ck.eq("row box w @003FCBE0", 210.0, img.f32(0x003FCBE0), note="FUN_0004B1C0")
    ck.eq("row box h @003FCBE4", 26.0, img.f32(0x003FCBE4), note="FUN_0004B1C0")
    ck.eq("row draw callback", 0x0004B4D0, img.u32(0x0004B269),
          note="node+0x3C @0x0004B266")
    ck.eq("FUN_0004D130 builds the row node", 0x0004B1C0,
          rel32(img, 0x0004D2E2), note="the only caller")
    ck.eq("row allocator FUN_001C6850", 0x001C6850, rel32(img, 0x0004D28E),
          note="2D draw-node allocator")

    # -- (c) the six probes, decoded from the binary --------------------- #
    probes = decode_probes(img)
    ck.eq("FUN_0004D310 probe count", 6, len(probes),
          note="rows %s" % ",".join(str(p[0]) for p in probes))
    m = TICK_C_RE.search(src)
    if not m:
        raise SystemExit("B3_TICK table not found in burnout3_hud.c")
    crows = TICK_ROW_RE.findall(m.group(1))
    if len(crows) != 7:
        raise SystemExit("B3_TICK has %d rows, expected 7" % len(crows))
    hdr = {n: cval(v) for n, v, _n in parse_table(HDR)}
    cthresh = {"B3HUD_TICK_ONC_MIN": hdr["B3HUD_TICK_ONC_MIN"]}
    for row, rec, thresh in probes:
        if row >= len(crows):
            ck.eq("probe row %d in range" % row, True, False, note="")
            continue
        label, strname, cth, probed, crec = crows[row]
        ck.eq("row %d record" % row, rec, int(crec, 16),
              note="%s <- score+0x%03X" % (label, rec))
        want = cthresh.get(cth, None)
        if want is None:
            want = float(cth.rstrip("f"))
        ck.eq("row %d threshold" % row, thresh, want,
              note="%s shows past %g" % (label, thresh))
        ck.eq("row %d probed" % row, "1", probed, note=label)
    probed_rows = [p[0] for p in probes]
    ck.eq("probe order (list push order)", [0, 1, 2, 4, 5, 6], probed_rows,
          note="slot 3 (AIR) has a slot + a label but is NEVER probed")
    ck.eq("C mirrors the probe order", True,
          "B3_TICK_PROBE[6] = {0, 1, 2, 4, 5, 6}" in src, note="")
    ck.eq("C marks AIR unprobed", True, crows[3][3] == "0",
          note="row 3 = %s" % crows[3][0])

    # -- (d) the labels are Globalus.bin entries ------------------------- #
    sites = [(0, 0x0004C15C), (1, 0x0004C19E), (2, 0x0004C1DD),
             (3, 0x0004C21C), (4, 0x0004C25B), (5, 0x0004C29A),
             (6, 0x0004C2D9)]
    get, path = globalus_strings()
    for row, va in sites:
        off = img.u32(va)
        ck.eq("row %d label offset" % row, hdr[crows[row][1]], off,
              note="constructor @%08x -> entry %d" % (va, off // 4))
        if get:
            want = get(off // 4)
            ck.eq("row %d label text" % row, crows[row][0], want,
                  note="Globalus.bin entry %d" % (off // 4))
    if get:
        print("       (%s)" % path)
    else:
        print("  --   Globalus.bin not found, label text check skipped")

    # -- (e) the star art ------------------------------------------------ #
    ck.eq("star texture handle", 0x00460940,
          struct.unpack("<I", img.read(0x0004BAC9, 4))[0],
          note="`hud_boost_stars`, bound by FUN_0004DD00 (RE_FRONTEND 6.5)")
    png = os.path.join(REPO, "build", "frontend", "hud_boost_stars.png")
    if os.path.exists(png):
        d = open(png, "rb").read()
        w, h = struct.unpack_from(">II", d, 16)
        ck.eq("hud_boost_stars.png", (64, 32), (w, h),
              note="2 frames: solid star | outline star")
        ck.eq("solid star u window", (0.0078125, 0.4921875),
              (img.f32(0x003B16F4), img.f32(0x00388458)),
              note="left half  = 0.5..31.5 of 64")
        ck.eq("outline star u window", (0.5078125, 0.9921875),
              (img.f32(0x003B1FF4), img.f32(0x003B1FF0)),
              note="right half = 32.5..63.5 of 64")
    else:
        print("  --   hud_boost_stars.png missing, skipped")

    # -- (f) the C carries the recovered state machine ------------------- #
    for tag, snippet in (
            ("life refresh", "if (show) r->timer = B3HUD_TICK_LIFE;"),
            ("flash pop", "r->flash = B3HUD_TICK_FLASH;"),
            ("flash decay", "r->flash -= B3HUD_TICK_FLASH_RATE * dt;"),
            ("near-miss spin",
             "r->phase += (1.f - t) * B3HUD_TICK_SPIN_NM * dt;"),
            ("plain spin", "r->phase += B3HUD_TICK_SPIN * dt;"),
            ("pulse decay", "r->pulse -= B3HUD_TICK_FLASH_RATE * dt;"),
            ("top tier kills the pending pip",
             "if (lvl == rec->count - 1) {"),
            ("stack step", "target_y -= B3HUD_TICK_STACK_STEP;"),
            ("slide rate", "y += B3HUD_TICK_SLIDE_RATE * dt;"),
            ("shrink out", "float k2 = (1.f - s) * B3HUD_TICK_SHRINK;"),
            ("row list is push-front", "g_tick_order[0] = i;")):
        ck.eq("C: " + tag, True, snippet in src, note=snippet)


# --------------------------------------------------------------------- #
# [12] differential: the REAL ticker executed under Unicorn
# --------------------------------------------------------------------- #
def _e8_targets(img, lo, hi):
    """Every E8 rel32 target inside [lo, hi)."""
    code = img.read(lo, hi - lo)
    out = []
    for i in range(len(code) - 4):
        if code[i] == 0xE8:
            out.append(rel32(img, lo + i))
    return out


# --------------------------------------------------------------------- #
# [13] the corner plates -- FUN_00048430's three-slice hud_element01
# --------------------------------------------------------------------- #
PLATE_DRAW_CB = 0x00048430
PLATE_BODY_HI = 0x000485E2          # the ret at 0x000485E1


def check_plates(img, ck):
    print("\n[13] the POS / LAP / SPEED plates (FUN_00048430 three-slice)")
    hdr = {n: cval(v) for n, v, _n in parse_table(HDR)}
    src = open(SRC).read()

    # (a) the callback really binds hud_element01's handle
    ck.eq("draw cb binds 0x0046093C", 0x0046093C, img.u32(0x00048589),
          note="mov eax,[0x46093C] @0x00048588 = hud_element01")
    # ...and FUN_0004DD00 really binds the NAME "hud_element01" there
    ck.eq("FUN_0004DD00 name ptr", 0x003AB2A8, img.u32(0x0004E03E),
          note="push 0x3AB2A8 @0x0004E03D")
    ck.eq("...spells hud_element01", "hud_element01", img.cstr(0x003AB2A8),
          note=".rdata")
    ck.eq("...stored to the handle", 0x0046093C, img.u32(0x0004E071),
          note="mov [0x46093C],edi @0x0004E06F")

    # (b) the three elements all install THIS callback
    for name, site in (("POS  FUN_00053ED0", 0x000540F0),
                       ("LAP  FUN_00051650", 0x000518F6),
                       ("SPEED FUN_00059850", 0x00059A0C)):
        ck.eq("%s node+0x3C" % name, PLATE_DRAW_CB, img.u32(site),
              note="imm32@%08x" % site)

    # (c) NO render-state preset switch anywhere in the callback: the
    #     plate draws under the ambient 2D state (alpha, WRAP, 0x010101)
    tgts = _e8_targets(img, PLATE_DRAW_CB, PLATE_BODY_HI)
    ck.eq("no blend preset switch", True, 0x001C82E0 not in tgts,
          note="FUN_001C82E0 absent from FUN_00048430")
    ck.eq("no address preset switch", True, 0x001C8470 not in tgts,
          note="FUN_001C8470 absent from FUN_00048430")
    ck.eq("emits through the rect batch", True, 0x001C7430 in tgts,
          note="FUN_001C7430 @0x000485D3")
    ck.eq("C draws it under alpha blend", True,
          "GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA" in src, note="state_begin")
    ck.eq("C masks alpha writes off", True,
          "glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE)" in src,
          note="COLORWRITEENABLE 0x010101")

    # (d) the u/v cut the C uses is the one the callback reads
    ck.eq("slice u0", 0.0, 0.0, note="left cap starts at u 0")
    ck.eq("slice u1", hdr["B3HUD_EL_U1"], img.bss_default(0x00264730)[0],
          note="0x0054F39C")
    ck.eq("slice u2", hdr["B3HUD_EL_U2"], img.bss_default(0x00264750)[0],
          note="0x0054F36C")
    ck.eq("slice v0", hdr["B3HUD_EL_V0"], img.bss_default(0x002646F0)[0],
          note="0x0054F394")
    ck.eq("slice v1", hdr["B3HUD_EL_V1"], img.bss_default(0x00264710)[0],
          note="0x0054F374")
    rows = (hdr["B3HUD_EL_V1"] - hdr["B3HUD_EL_V0"]) * hdr["B3HUD_PLATE_CAP_DEN"]
    ck.eq("(v1-v0)*32 = covered texel rows", 29.0, rows,
          note="the cap-aspect denominator")

    # (e) the three boxes, resolved with the recovered anchor rule
    W, H = hdr["B3HUD_POS_W"], hdr["B3HUD_PLATE_H"]
    SW, SH = hdr["B3HUD_SPEED_W"], hdr["B3HUD_SPEED_H"]
    ck.eq("POS box size", (125.0, 27.0), (W, H), note="0x003B03F4/0x003897A8")
    ck.eq("SPEED box size", (168.0, 27.0), (SW, SH),
          note="0x00389A24/bss 0x0054FCCC")
    ck.eq("SPEED 1/w", 1.0 / SW, hdr["B3HUD_SPEED_INV_W"], tol=1e-6,
          note="0x003B209C == 1/168")
    capr = hdr["B3HUD_CAP_R_NUM"] / rows * H / W
    capl = hdr["B3HUD_CAP_L_NUM"] / rows * H / W
    ck.eq("POS right cap frac", 0.141522, capr, tol=1e-5, note="19/29 * 27/125")
    ck.eq("LAP left cap frac", 0.283043, capl, tol=1e-5, note="38/29 * 27/125")
    ck.eq("POS plate spans", (-30.0, 95.0),
          (hdr["B3HUD_POS_REF_X"], hdr["B3HUD_POS_REF_X"] + W),
          note="anchor-relative, slot 1")
    ck.eq("LAP plate spans", (-95.0, 30.0),
          (hdr["B3HUD_LAP_REF_X"] - W, hdr["B3HUD_LAP_REF_X"]),
          note="anchor-relative, slot 7 -- the mirror")
    ck.eq("plate top", 0.5,
          hdr["B3HUD_PLATE_REF_Y"] - hdr["B3HUD_PLATE_REF_DY"],
          note="REF_Y - REF_DY, both plates")

    # (f) the labels really are the Globalus entries the elements load
    get, path = globalus_strings()
    if get is None:
        print("  --   Globalus.bin not found, label check skipped")
    else:
        for nm, off, want in (("POS", hdr["B3HUD_STR_POS"], "POS"),
                              ("LAP", hdr["B3HUD_STR_LAP"], "LAP"),
                              ("mph", hdr["B3HUD_STR_MPH"], "mph")):
            ck.eq("label %s" % nm, want, get(off // 4),
                  note="entry %d of %s" % (off // 4, os.path.basename(path)))
        ck.eq("C draws \"POS\" not \"POS.\"", True,
              '"POS"' in src and '"POS."' not in src,
              note="entry 2002 is POS, no period")

    # (g) the C really uses the three-slice geometry and the game's art
    ck.eq("C has plate_3slice", True, "plate_3slice" in src, note="")
    ck.eq("C dropped big_curve", True, 'load_from(dir, "big_curve"' not in src,
          note="the menu asset is no longer bound by the HUD")
    ck.eq("C binds hud_element01", True,
          'load_from(dir, "hud_element01"' in src, note="")


# --------------------------------------------------------------------- #
# [14] the opponent tags -- FUN_0018EE10 / FUN_0018F060
# --------------------------------------------------------------------- #
def check_opponent_tags(img, ck):
    print("\n[14] the OPPONENT TAGS (FUN_0018F060)")
    hdr = {n: cval(v) for n, v, _n in parse_table(HDR)}
    src = open(SRC).read()

    # (a) the ordinal comes out of Globalus indexed by place-1
    ck.eq("ordinal table offset", 0x1F24, img.u32(0x0018EDBE),
          note="mov ecx,[eax+ebx*4+0x1F24] @0x0018EDBB")
    get, _p = globalus_strings()
    if get is not None:
        base = hdr["B3HUD_TAG_STR_1ST"] // 4
        # the run at 0x1F24 is exactly SIX long: 1999/2000 are ":" / " x ",
        # so the element covers a six-car grid and nothing beyond it.
        for i, want in enumerate(("1st", "2nd", "3rd", "4th", "5th", "6th")):
            ck.eq("ordinal[%d]" % i, want, get(base + i),
                  note="entry %d" % (base + i))
            ck.eq("C ordinal[%d]" % i, True, '"%s"' % want in src,
                  note="b3_hud_place_ordinal")
        ck.eq("the run ends at 6", True,
              get(base + 6) not in ("7th",),
              note="entry %d = %r, not an ordinal" % (base + 6, get(base + 6)))

    # (b) the pass state around the loop: plain alpha, no additive
    #     (FUN_001C82E0 is reached with ESI = 0 -- xor esi,esi @0x0018ED81)
    ck.eq("preset selector called", 0x001C82E0, rel32(img, 0x0018ED83),
          note="call FUN_001C82E0 @0x0018ED83")
    ck.eq("...with ESI = 0", 0x33F6, struct.unpack(">H", img.read(0x0018ED81, 2))[0],
          note="xor esi,esi @0x0018ED81 -> blend preset 0 = plain alpha")
    ck.eq("SRCBLEND", 0x0302, img.preset(0x004A1A90, 0), note="preset 0")
    ck.eq("DESTBLEND", 0x0303, img.preset(0x004A1AB0, 0), note="preset 0")

    # (c) the triangle is a flat 3-vertex batch, not a sprite
    ck.eq("triangle batch", 0x001C7C90, rel32(img, 0x0018F757),
          note="call FUN_001C7C90 @0x0018F757 (3 verts, one colour)")
    ck.eq("vertex count", 3, img.u32(0x0018F3B3),
          note="mov edi,3 @0x0018F3B2, pushed @0x0018F715")
    ck.eq("C draws GL_TRIANGLES", True, "glBegin(GL_TRIANGLES)" in src,
          note="apex-down, untextured")

    # (d) every constant the element runs on, re-read at its own VA
    for name, va in (("B3HUD_TAG_DIST", 0x003B175C),
                     ("B3HUD_TAG_SIZE_K", 0x003B1F4C),
                     ("B3HUD_TAG_SIZE_MAX", 0x00396EB0),
                     ("B3HUD_TAG_SIZE_SCALE", 0x003A55F8),
                     ("B3HUD_TAG_YOFF", 0x003B1A18),
                     ("B3HUD_TAG_TRI_W", 0x003B1750),
                     ("B3HUD_TAG_MIN_W", 0x003A7950),
                     ("B3HUD_TAG_MIN_H", 0x003B1708),
                     ("B3HUD_TAG_OPACITY", 0x003A69C0),
                     ("B3HUD_TAG_FADE_FAR", 0x003B1CCC),
                     ("B3HUD_TAG_FADE_RATE", 0x003B17BC),
                     ("B3HUD_TAG_NEAR_HI", 0x003A3224),
                     ("B3HUD_TAG_NEAR_LO", 0x003B1688),
                     ("B3HUD_TAG_NEAR_K", 0x003B1F48)):
        ck.eq(name, hdr[name], img.f32(va), tol=1e-6, note="@%08x" % va)
    ck.eq("SIZE_K == 1/74", 1.0 / 74.0, hdr["B3HUD_TAG_SIZE_K"], tol=1e-7,
          note="the size falloff")
    ck.eq("NEAR_K == 1/(HI-LO)",
          1.0 / (hdr["B3HUD_TAG_NEAR_HI"] - hdr["B3HUD_TAG_NEAR_LO"]),
          hdr["B3HUD_TAG_NEAR_K"], tol=1e-5,
          note="the 2.0..2.7 fade-in is exactly normalised")

    # (e) the C's own switch and envelope, driven through the real entry
    #     point would need a GL context, so assert the source shape.
    ck.eq("C switches at TAG_DIST", True,
          "distance >= B3HUD_TAG_DIST" in src, note="triangle above, text below")
    ck.eq("C floors the text size", True,
          "B3HUD_TAG_MIN_W" in src and "B3HUD_TAG_MIN_H" in src, note="")
    ck.eq("C applies the near/far fade", True,
          "tag_alpha" in src, note="TAG_FADE_FAR / TAG_NEAR_*")


def check_ticker_capture(img, ck):
    print("\n[12] FUN_0004D310 + FUN_0004B4D0 executed under Unicorn "
          "(tools/emulate_hud_ticker.py)")
    try:
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import emulate_hud_ticker as et                       # noqa: E402
    except Exception as e:                                    # pragma: no cover
        print("  --   skipped: %s" % e)
        return

    hdr = {n: cval(v) for n, v, _n in parse_table(HDR)}
    S = hdr["B3HUD_TICK_STAR"]
    HALF = S * hdr["B3HUD_TICK_STAR_HALF"]
    ADV = S - hdr["B3HUD_TICK_STAR_OVERLAP"]
    GAP = hdr["B3HUD_TICK_STAR_GAP"]
    # the element's stacking base, exactly as the C's TICK_BASE macro:
    # the boost bar's node y + h * 0.5, in element-local coordinates.
    base = -hdr["B3HUD_BOOST_H"] + hdr["B3HUD_BOOST_H"] * hdr["B3HUD_TICK_BASE_FRAC"]

    t = et.TickerTrace()
    t.reset(base_y=base)
    dt = 1.0 / 60.0
    rows = []
    for i in range(24):                       # settle: flash -> 1, y -> slot
        t.set_record(0x418, value=3.0, clock=dt, prev=0.0, open_=1,
                     tier=2, count=4)
        rows = t.update(dt)
    ck.eq("one row live", 1, len(rows), note="the NEAR MISS chain")
    r = rows[0]
    ck.eq("row label", "NEAR MISS", r["label"], note="Globalus 2110")
    x, y, w, h = r["node_xywh"]
    ck.eq("row node box", (hdr["B3HUD_TICK_W"], hdr["B3HUD_TICK_H"]), (w, h),
          note="FUN_0004B1C0")
    ck.eq("row node x", hdr["B3HUD_TICK_ROW_X"], x, note="ref.x")
    ck.eq("row node y (settled)",
          base - hdr["B3HUD_TICK_ROW_STEP"] - hdr["B3HUD_TICK_STACK_STEP"], y,
          note="base - 26 - 26  =>  screen y %.0f..%.0f"
               % (480 + y, 480 + y + h))
    ck.eq("row draw callback", int(hdr["B3HUD_TICK_DRAW_CB"]), r["draw_cb"],
          note="node+0x3C")
    ck.eq("row alpha", 1.0, r["node_rgba"][3], note="opaque while live")
    ck.eq("flash settled", hdr["B3HUD_TICK_FLASH_FLOOR"], round(r["flash"], 5),
          note="2.0 -> 1.0 at 5/s")
    ck.eq("pending pip alive", 1.0, r["pulse"], note="tier 2 of 4")

    caps = [c for c in t.draw(r["node"]) if not isinstance(c, tuple)]
    text = [c for c in caps if c.tex == "GlobalFont" and c.kind == "rects_vc"]
    stars = [c for c in caps if c.tex == "hud_boost_stars"]
    ck.eq("label drawn with GlobalFont", True, len(text) > 0,
          note="%d glyph batches + %d shadow passes"
               % (len(text), len([c for c in caps
                                  if c.tex == "GlobalFont"]) - len(text)))
    ck.eq("shadow passes", 8,
          len([c for c in caps if c.tex == "GlobalFont"]) - len(text),
          note="4 at A+-B down-right, 4 diagonal at +-B")
    ck.eq("star batches", 2, len(stars), note="pending strip + solid rects")

    for c in stars:
        ck.eq("star blend", (0x0302, 0x0303), (c.rs["src"], c.rs["dst"]),
              note=c.blend())

    pend = [c for c in stars if c.kind == "strip"][0]
    solid = [c for c in stars if c.kind == "rects"][0]
    text_right = text[-1].verts[-1][0]

    # -- the solid pips: tier quads, S wide, ADV apart, starting GAP past
    #    the label's last vertex; the newest one zoomed by row.flash.
    ck.eq("solid pip count", 2, len(solid.verts) // 4, note="tier = 2")
    ck.eq("solid pip uv", (hdr["B3HUD_TICK_STAR_U0"], hdr["B3HUD_TICK_STAR_U1"],
                           hdr["B3HUD_TICK_STAR_V0"], hdr["B3HUD_TICK_STAR_V1"]),
          (round(min(v[2] for v in solid.verts), 7),
           round(max(v[2] for v in solid.verts), 7),
           round(min(v[3] for v in solid.verts), 7),
           round(max(v[3] for v in solid.verts), 7)),
          note="the sheet's LEFT frame")
    for i in range(2):
        q = solid.verts[i * 4:(i + 1) * 4]
        cx = sum(v[0] for v in q) / 4.0
        cy = sum(v[1] for v in q) / 4.0
        ck.eq("solid pip %d centre" % i,
              (round(text_right + GAP + HALF + ADV * i, 3), round(y + HALF, 3)),
              (round(cx, 3), round(cy, 3)),
              note="label right + %g + %g + %g*i" % (GAP, HALF, ADV))
        ck.eq("solid pip %d size" % i, (S, S),
              (round(max(v[0] for v in q) - min(v[0] for v in q), 3),
               round(max(v[1] for v in q) - min(v[1] for v in q), 3)),
              note="flash settled to 1.0")

    # -- the pending pip: the sheet's RIGHT frame, a diamond of radius S/2
    #    at the NEXT slot, spun by row.phase.
    ck.eq("pending pip uv", (hdr["B3HUD_TICK_PEND_U0"], hdr["B3HUD_TICK_PEND_U1"]),
          (round(min(v[2] for v in pend.verts), 7),
           round(max(v[2] for v in pend.verts), 7)),
          note="the sheet's RIGHT frame (outline star)")
    pcx = sum(v[0] for v in pend.verts) / 4.0
    pcy = sum(v[1] for v in pend.verts) / 4.0
    ck.eq("pending pip centre",
          (round(text_right + GAP + HALF + ADV * 2, 3), round(y + HALF, 3)),
          (round(pcx, 3), round(pcy, 3)), note="the slot after the last pip")
    import math
    rad = max(math.hypot(v[0] - pcx, v[1] - pcy) for v in pend.verts)
    ck.eq("pending pip radius", HALF, round(rad, 3), note="S/2, corner radius")
    ang = math.degrees(math.atan2(pend.verts[0][0] - pcx,
                                  pend.verts[0][1] - pcy)) % 360.0
    ck.eq("pending pip spin == row.phase",
          round(math.degrees(r["phase"]) % 360.0, 2), round(ang, 2),
          note="corner 0 at (sin phase, cos phase) * S/2")

    # -- the C's own label layout must land the last vertex where the game
    #    does: the star column hangs off it.  Replays burnout3_hud.c's
    #    tick_scale_x / tick_advance / tick_text_right on the metrics in
    #    src/burnout3_font.h.
    glyphs = parse_font_header()
    sx = (hdr["B3HUD_TICK_FONT_SCALE"] * hdr["B3HUD_TICK_TEXT_EM"]
          / float(glyphs["tex_w"]))
    for label, node in (("NEAR MISS", x), ("ONCOMING", 4.0)):
        if label != "NEAR MISS":
            t.reset(base_y=base)
            for i in range(24):
                t.set_record(0x374, value=400.0, clock=dt, open_=1, tier=1,
                             count=4)
                rr = t.update(dt)
            cc = [c for c in t.draw(rr[0]["node"]) if not isinstance(c, tuple)]
            tt = [c for c in cc if c.tex == "GlobalFont"
                  and c.kind == "rects_vc"]
            game_right = tt[-1].verts[-1][0]
            node = rr[0]["node_xywh"][0]
        else:
            game_right = text_right
        pen = node - glyphs[label[0]]["xoff"] * sx
        right = pen
        for chx in label:
            g = glyphs[chx]
            if g["w"] > 0.0:
                right = pen + (g["xoff"] + g["w"]) * sx
            adv = (hdr["B3HUD_TICK_SPACE_ADV"] * glyphs["tex_w"]
                   if chx == " " else g["adv"])
            pen += adv * sx
        ck.eq("C label right edge (%s)" % label, round(right, 2),
              round(game_right, 2),
              note="burnout3_font.h metrics * %.6f px/atlas px" % sx)

    # -- the row's timeline, as the game runs it: open -> slide -> close
    #    -> 0.85 s of grace -> 0.25 s of fade+shrink -> gone.
    t.reset(base_y=base)
    t.set_record(0x390, value=40.0, clock=dt, open_=1, tier=0, count=4)
    rows = t.update(dt)
    ck.eq("row opens on the first frame", 1, len(rows), note="DRIFT, tier 0")
    ck.eq("row enters at base-26 then slides",
          base - hdr["B3HUD_TICK_ROW_STEP"] - hdr["B3HUD_TICK_SLIDE_RATE"] * dt,
          rows[0]["y"], note="300 px/s up toward base-52")
    ck.eq("no solid pip at tier 0", 0, rows[0]["tier"],
          note="tier 0 = one spinning outline pip only")
    for i in range(24):
        t.set_record(0x390, value=40.0, clock=dt, open_=1, tier=0, count=4)
        rows = t.update(dt)
    ck.eq("row settles in slot 0", base - 52.0, rows[0]["y"],
          note="base - 26 - 26")
    # close the event: prev_value carries the final tier for 0.85 s
    frames = 0
    alive = True
    fade_seen = None
    while alive and frames < 200:
        t.set_record(0x390, value=0.0, clock=dt, prev=40.0, open_=0,
                     tier=-1, prev_tier=0, count=4)
        rows = t.update(dt)
        frames += 1
        alive = len(rows) > 0
        if alive and rows[0]["node_rgba"][3] < 1.0 and fade_seen is None:
            fade_seen = frames
    ck.eq("row lives 0.85 s after the event closes",
          int(math.ceil(hdr["B3HUD_TICK_LIFE"] / dt)), frames,
          note="%.3f s at 1/60" % (frames * dt))
    ck.eq("the fade is the last 0.25 s",
          int(round(hdr["B3HUD_TICK_FADE"] / dt)), frames - fade_seen,
          note="alpha = timer * 4, and the box shrinks 10%")


FONT_ROW_RE = re.compile(
    r"\{([-0-9.f, ]+)\},\s*/\* '(\\?.)' \*/")


def parse_font_header():
    """The GlobalFont block of src/burnout3_font.h, as the C sees it."""
    path = os.path.join(REPO, "src", "burnout3_font.h")
    txt = open(path).read()
    blk = txt[txt.index("b3_font_globalfont"):]
    m = re.search(r'"GlobalFont",\s*(\d+),\s*(\d+),', blk)
    out = {"tex_w": int(m.group(1)), "tex_h": int(m.group(2))}
    rows = FONT_ROW_RE.findall(blk)[:95]
    for i, (vals, ch) in enumerate(rows):
        v = [float(x.rstrip("f")) for x in vals.split(",")]
        out[chr(0x20 + i)] = dict(w=v[4], h=v[5], xoff=v[6], yoff=v[7],
                                  adv=v[8], present=v[9])
    return out



CRASHDESC_RE = re.compile(r"B3HUD-CRASHDESC-BEGIN(.*?)B3HUD-CRASHDESC-END", re.S)
CRASHVEH_RE = re.compile(r"B3HUD-CRASHVEH-BEGIN(.*?)B3HUD-CRASHVEH-END", re.S)
CD_ROW_RE = re.compile(r'\{\s*"(.*?)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),'
                       r'\s*(NULL|"[^"]*"),\s*(NULL|"[^"]*")\s*\}')
CV_ROW_RE = re.compile(r'\{\s*"([A-Z0-9]+)",\s*"([^"]*)",\s*"([^"]*)"\s*\}')

# the vehicle-id alphabet: the SFX base-40 charmap rotated by two.  Every
# one of the 40 keys decodes to a clean <CLASS>CAR<nn> with it, which is
# what validates the rotation.
VEH_CS = "!'_0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ?"


def veh_b40(v):
    out = []
    for _ in range(12):
        out.append(VEH_CS[v % 40])
        v //= 40
    return "".join(reversed(out)).rstrip("!").rstrip("'").rstrip("_")


def check_crash_show(img, ck):
    """The crash ticker + the (A) IMPACT TIME prompt (RE: the crash-show
    session).  Everything here is re-derived from the image and
    Data/Globalus.bin, then compared against the C tables."""
    print("\n[15] THE CRASH SHOW -- the ticker's tables and the prompt")
    src = open(SRC).read()
    get, gpath = globalus_strings()
    if get is None:
        print("  -- Data/Globalus.bin absent, skipped")
        return
    hdr = {n: cval(v) for n, v, _n in parse_table(HDR)}

    # -- (a) the 30-row descriptor table 0x003A2F70 --------------------- #
    m = CRASHDESC_RE.search(src)
    if not m:
        raise SystemExit("no B3HUD-CRASHDESC block in burnout3_hud.c")
    rows = CD_ROW_RE.findall(m.group(1))
    ck.eq("crash descriptor rows", len(rows), 30, note="0x003A2F70..0x003A3150")
    base = hdr["B3HUD_CRASH_DESC_VA"]
    for i, (fmt, sstr, flags, dbl, tri, dbl_s, tri_s) in enumerate(rows):
        rec = base + i * 0x10
        ck.eq("desc[%d].str" % i, int(sstr), img.u32(rec))
        ck.eq("desc[%d].flags" % i, int(flags), img.u32(rec + 4))
        ck.eq("desc[%d].dbl" % i, int(dbl), img.u32(rec + 8))
        ck.eq("desc[%d].tri" % i, int(tri), img.u32(rec + 0xC))
        ck.eq("desc[%d] text" % i, fmt, get(int(sstr)),
              note="Globalus %d" % int(sstr))
        if int(dbl):
            ck.eq("desc[%d] double" % i, dbl_s.strip('"'), get(int(dbl)))
            ck.eq("desc[%d] triple" % i, tri_s.strip('"'), get(int(tri)))
    # the row index IS the Globalus offset from 1833
    ck.eq("row index == Globalus - 1833", True,
          all(int(r[1]) == 1833 + i for i, r in enumerate(rows)))

    # -- (b) the 40 traffic models -------------------------------------- #
    m = CRASHVEH_RE.search(src)
    if not m:
        raise SystemExit("no B3HUD-CRASHVEH block in burnout3_hud.c")
    vrows = CV_ROW_RE.findall(m.group(1))
    ck.eq("traffic model rows", len(vrows), 40, note="FUN_00158AD0's table")
    kbase = hdr["B3HUD_CRASH_KEY_VA"]
    nbase = hdr["B3HUD_CRASH_NAME_VA"]
    ibase = hdr["B3HUD_CRASH_INTO_VA"]
    for i, (vid, name, into) in enumerate(vrows):
        lo = img.u32(kbase + i * 8)
        hi = img.u32(kbase + i * 8 + 4)
        ck.eq("veh[%d].id" % i, vid, veh_b40((hi << 32) | lo),
              note="base-40 key pair")
        ck.eq("veh[%d].name" % i, name, get(img.u32(nbase + i * 4)))
        ck.eq("veh[%d].into" % i, into, get(img.u32(ibase + i * 4)))

    # -- (c) the joiner's two wide strings ------------------------------ #
    sep = img.read(hdr["B3HUD_CRASH_SEP_VA"], 6)
    tail = img.read(hdr["B3HUD_CRASH_TAIL_VA"], 4)
    ck.eq("FUN_0004ED40 separator", sep.decode("utf-16-le").split("\0")[0],
          "+ ", note="@0x0038867C")
    ck.eq("FUN_0004ED40 trailer", tail.decode("utf-16-le").split("\0")[0],
          " ", note="@0x00388678")

    # -- (d) the prompt's own strings ----------------------------------- #
    ck.eq("IMPACT TIME string", "IMPACT TIME",
          get(hdr["B3HUD_IMPACT_STR"] // 4))
    # FUN_0017A6B0 returns these as string INDICES (the caller does
    # table[idx*4]), not as the byte offsets the element constructors use.
    ck.eq("Into Rival string", "Into Rival",
          get(hdr["B3HUD_CRASH_STR_RIVAL"]))
    ck.eq("Into Car fallback", "Into Car",
          get(hdr["B3HUD_CRASH_STR_INTOCAR"]))
    ck.eq("ft unit", "ft", get(hdr["B3HUD_CRASH_STR_FT"] // 4))
    ck.eq("m unit", "m", get(hdr["B3HUD_CRASH_STR_M"] // 4))
    ck.eq("s unit", "s", get(hdr["B3HUD_CRASH_STR_S"] // 4))
    ck.eq("A_Button texture name", "A_Button", img.cstr(0x003AB2E0))
    print("  30 descriptors, 40 traffic models, the joiner and the prompt")
    print("  strings all re-derived from %s" % os.path.basename(gpath))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    args = ap.parse_args()

    elf = args.elf
    if not os.path.exists(elf):
        conv = os.path.join(REPO, "tools", "xbe2elf.py")
        if os.path.exists(XBE) and os.path.exists(conv):
            print("building %s from the XBE ..." % elf)
            os.makedirs(os.path.dirname(elf), exist_ok=True)
            subprocess.check_call([sys.executable, conv, XBE, elf])
        else:
            raise SystemExit("missing %s and cannot rebuild it "
                             "(need the retail XBE)" % elf)

    img = Image(elf)
    ck = Checker()

    print("validate_hud.py -- in-race HUD constants vs %s" % elf)
    check_table(img, ck)
    check_box(img, ck)
    check_anchor_table(img, ck)
    check_tread_tables(img, ck)
    check_callouts(img, ck)
    check_art(ck)
    check_state_machine_rates(img, ck)
    check_section_colours(img, ck)
    check_render_state_presets(img, ck)
    check_draw_capture(ck)
    check_ticker(img, ck)
    check_ticker_capture(img, ck)
    check_plates(img, ck)
    check_opponent_tags(img, ck)
    check_crash_show(img, ck)

    print("\n%s: %d/%d checks green" %
          ("PASS" if not ck.fail else "FAIL",
           ck.ok, ck.ok + len(ck.fail)))
    if ck.fail:
        for name, got, want, note in ck.fail:
            print("  - %s: C=%s binary=%s %s" % (name, got, want, note))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
