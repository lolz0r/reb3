#!/usr/bin/env python3
"""
emulate_hud.py -- run Burnout 3's REAL in-race boost-bar draw callback
(FUN_0004AE40) under Unicorn and capture, for every draw section, the
render state that is live and the vertex buffer it emits (positions, UVs
and COLOURS).

Why this exists: the boost bar's geometry and timing were recovered by
reading, but the per-section RENDER STATE (blend mode, texture addressing,
vertex colour) was left [S] -- and the harness's guessed layer intensities
made the bar render as a faint purple wash instead of the retail dark plate
+ orange fire.  Reading harder was not going to fix that.  This executes the
actual x86 and reports what the game hands the GPU.

What is emulated for real
-------------------------
  FUN_0004AE40    the draw callback itself
  FUN_000488A0    the bar plate
  FUN_00048C00    the tier-change flash overlay
  FUN_000496E0    the BoostBits tread band
  FUN_00049AD0    the BoostFireCore blobs
  FUN_00049E40    the BoostEarnFlame comet
  FUN_00049FD0    the BoostFireEdge plume
  FUN_0004A470    the BoostFireOver streak band
  FUN_0004A740    the spark particles
  FUN_001C8470    texture-addressing preset  (writes the TSS shadow array)
  FUN_001C82E0    blend-mode preset          (writes the RS shadow array)
  FUN_001C7150    the render-state preset TABLES these two index

  FUN_001C7430(EAX=xy[], node, n, uv[])            rect batch, ONE colour
                                                   = pack(ECX -> float4)
  FUN_001C7710(EAX=xy[], node, n, uv[])            strip batch, ONE colour
  FUN_001C7960(EAX=xy[], node, n, uv[], rgba[])    batch, PER-VERTEX colour

The emitters RUN FOR REAL and their output is read straight out of the 2D
vertex pool at 0x00752F78 (stride 0x18: f32 x, f32 y, f32 u, f32 v,
u32 ARGB colour, u32 pad), which is what actually reaches the GPU.  The
ARGB is produced by FUN_001C6920: (int)(rgba*255) packed A<<24|R<<16|G<<8|B.

What is stubbed (captured at entry, then returned from)
-------------------------------------------------------
  FUN_0034DE60(stage, texobj)                      D3D SetTexture
  FUN_001C69C0 / FUN_001D7D50 / FUN_0034F9A0       pushbuffer flush
  FUN_00017310                                     crash-party mode test -> 0

Shadow arrays the presets write (found by following FUN_001D7040, the
state flusher, and FUN_001D7130/FUN_001D7150, the raw setters):

  render state  id  -> 0x0075D4A0 + id*4          then D3DDevice_SetRenderState
  texture stage      -> 0x0075D740 + (stage + type*4)*4  then SetTextureStageState

Usage:
    python3 tools/emulate_hud.py                 # the four canonical states
    python3 tools/emulate_hud.py --json out.json
"""
import argparse
import json
import os
import struct
import sys

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_CODE, UcError, UC_PROT_ALL)
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX,
                               UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emulate_vehicle import load_elf, PAGE  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(REPO, "build", "burnout3.elf")

# ---- addresses ------------------------------------------------------- #
FUN_DRAW        = 0x0004AE40    # the boost bar's draw callback
FUN_PLATE       = 0x000488A0
FUN_TIERFX      = 0x00048C00
FUN_TREAD       = 0x000496E0
FUN_CORE        = 0x00049AD0
FUN_EARN        = 0x00049E40
FUN_EDGE        = 0x00049FD0
FUN_OVER        = 0x0004A470
FUN_SPARKS      = 0x0004A740

FUN_QUADS       = 0x001C7430    # EAX=xy[], (node, n, uv[]) ; colour=pack(ECX)
FUN_STRIP       = 0x001C7710    # EAX=xy[], (node, n, uv[]) ; colour=pack(ECX)
FUN_QUADS_VC    = 0x001C7960    # EAX=xy[], (node, n, uv[], rgba[]) per-vertex
FUN_SETTEX      = 0x0034DE60
FUN_FLUSH       = 0x001C69C0
FUN_PUSHBUF     = 0x001D7D50
FUN_PUSHBUF2    = 0x0034F9A0
FUN_CRASHMODE   = 0x00017310
FUN_RSINIT      = 0x001C7150    # +0x18: past its one call, pure table fill
FUN_RSINIT_ENTRY = 0x001C7168
FUN_RSINIT_RET  = 0x001C72E3

# render-state / texture-stage shadow arrays (FUN_001D7040 flusher)
RS_SHADOW  = 0x0075D4A0         # rs[id]  at +id*4
TSS_SHADOW = 0x0075D740         # tss[stage][type] at +(stage + type*4)*4

RS_SRCBLEND        = 62         # value is a GL/NV2A blend-factor token
RS_DESTBLEND       = 63
RS_COLORWRITEENABLE = 67
RS_BLENDOP         = 74         # value is a GL/NV2A blend-equation token

TSS_ADDRESSU = 0
TSS_ADDRESSV = 1

# texture-handle globals bound by FUN_0004DD00 (RE_FRONTEND 6.5)
TEX_GLOBALS = {
    "boostbits":      (0x00460938, 1),
    "boostearnflame": (0x004607C4, 1),
    "boostfirecore":  (0x004607C8, 30),
    "boostfireedge":  (0x00460848, 41),
    "boostfireover":  (0x00460770, 20),
}

# ---- scratch layout --------------------------------------------------- #
STACK_BASE = 0x20000000
STACK_SIZE = 0x00100000
NODE       = 0x30000000         # the draw node (FUN_00048800 fills it)
STATE      = 0x30010000         # the boost state block (element obj +0x20)
TEXOBJS    = 0x30100000         # fake texture handle objects
MAGIC_RET  = 0x50000000

BLEND_TOKEN = {
    0x0000: "ZERO", 0x0001: "ONE",
    0x0300: "SRC_COLOR", 0x0301: "ONE_MINUS_SRC_COLOR",
    0x0302: "SRC_ALPHA", 0x0303: "ONE_MINUS_SRC_ALPHA",
    0x0304: "DST_ALPHA", 0x0305: "ONE_MINUS_DST_ALPHA",
    0x0306: "DST_COLOR", 0x0307: "ONE_MINUS_DST_COLOR",
    0x0308: "SRC_ALPHA_SATURATE",
    0x8001: "CONSTANT_COLOR", 0x8002: "ONE_MINUS_CONSTANT_COLOR",
    0x8003: "CONSTANT_ALPHA", 0x8004: "ONE_MINUS_CONSTANT_ALPHA",
}
BLENDOP_TOKEN = {0x8006: "FUNC_ADD", 0x8007: "MIN", 0x8008: "MAX",
                 0x800A: "FUNC_SUBTRACT", 0x800B: "FUNC_REVERSE_SUBTRACT"}
ADDRESS_MODE = {1: "WRAP", 2: "MIRROR", 3: "CLAMP", 4: "BORDER",
                5: "CLAMPTOEDGE"}


# --------------------------------------------------------------------- #
def apply_float_initialisers(uc, lo, hi):
    """The boost bar's geometry ratios live in BSS; their compiled-in
    defaults come from one-instruction C++ dynamic initialisers of the shape
        F3 0F 10 05 <const>  F3 0F 11 05 <global>  C3      (= *const)
        0F 57 C0             F3 0F 11 05 <global>  C3      (= 0.0)
    Executing the whole registry is unnecessary -- decode and apply them.
    Returns the number applied."""
    data = uc.mem_read(0x00260000, 0x00020000)
    n = 0
    i = 0
    while i < len(data) - 20:
        if data[i:i + 4] == b"\xF3\x0F\x10\x05" and \
           data[i + 8:i + 12] == b"\xF3\x0F\x11\x05" and data[i + 16] == 0xC3:
            cva = struct.unpack_from("<I", data, i + 4)[0]
            gva = struct.unpack_from("<I", data, i + 12)[0]
            if lo <= cva < hi and lo <= gva < hi:
                uc.mem_write(gva, bytes(uc.mem_read(cva, 4)))
                n += 1
            i += 17
            continue
        if data[i:i + 3] == b"\x0F\x57\xC0" and \
           data[i + 3:i + 7] == b"\xF3\x0F\x11\x05" and data[i + 11] == 0xC3:
            gva = struct.unpack_from("<I", data, i + 7)[0]
            if lo <= gva < hi:
                uc.mem_write(gva, b"\x00\x00\x00\x00")
                n += 1
            i += 12
            continue
        i += 1
    return n


def f32(uc, va):
    return struct.unpack("<f", uc.mem_read(va, 4))[0]


def u32(uc, va):
    return struct.unpack("<I", uc.mem_read(va, 4))[0]


def wf(uc, va, v):
    uc.mem_write(va, struct.pack("<f", v))


def wu(uc, va, v):
    uc.mem_write(va, struct.pack("<I", v & 0xFFFFFFFF))


def vec4(uc, va):
    return list(struct.unpack("<4f", uc.mem_read(va, 16)))


# the 2D vertex pool FUN_001C7430/7710/7960 fill, and its stride
VPOOL      = 0x00752F78
VPOOL_STRIDE = 0x18
VPOOL_COUNT  = 0x004A1B9C


class Capture(object):
    """One emitter call: what state was live and what vertices went out."""

    def __init__(self, kind, tex, rs, tss, verts, node_rgba, site=0):
        self.site = site
        self.kind = kind
        self.tex = tex
        self.rs = rs
        self.tss = tss
        self.verts = verts              # [(x, y, u, v, argb)]
        self.node_rgba = node_rgba

    def colours(self):
        out = []
        for v in self.verts:
            if v[4] not in out:
                out.append(v[4])
        return out

    def blend(self):
        return "%s,%s %s" % (
            BLEND_TOKEN.get(self.rs["src"], hex(self.rs["src"])),
            BLEND_TOKEN.get(self.rs["dst"], hex(self.rs["dst"])),
            BLENDOP_TOKEN.get(self.rs["op"], hex(self.rs["op"])))

    def address(self):
        return "%s/%s" % (ADDRESS_MODE.get(self.tss["u"], self.tss["u"]),
                          ADDRESS_MODE.get(self.tss["v"], self.tss["v"]))


class HudTrace(object):
    def __init__(self, verbose=False):
        self.verbose = verbose
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.lo, self.hi = load_elf(self.uc, ELF)
        self.uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
        self.uc.mem_map(NODE, PAGE, UC_PROT_ALL)
        self.uc.mem_map(STATE, 0x4000, UC_PROT_ALL)
        self.uc.mem_map(TEXOBJS, 0x10000, UC_PROT_ALL)
        self.uc.mem_map(MAGIC_RET & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        self.ninit = apply_float_initialisers(self.uc, self.lo, self.hi)
        self.tex_names = {}
        self._bind_textures()
        self.events = []
        self.cur_tex = None
        self.pending = None
        self.flushed = None
        self.faults = 0
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._fault)
        self.uc.hook_add(UC_HOOK_CODE, self._code)
        self._run_rs_init()

    # -- setup ---------------------------------------------------------- #
    def _bind_textures(self):
        """FUN_0004DD00 fills these handle globals at load; give each a
        distinguishable fake handle so SetTexture tells us the strip frame."""
        p = TEXOBJS
        for name, (base, count) in TEX_GLOBALS.items():
            for i in range(count):
                handle = p
                p += 0x40
                wu(self.uc, handle + 0x14, handle + 0x1000)   # "texture obj"
                wu(self.uc, base + i * 4, handle)
                label = name if count == 1 else "%s%02d" % (name, i + 1)
                self.tex_names[handle + 0x1000] = label

    def _run_rs_init(self):
        """FUN_001C7150 (from just past its single call) fills the render
        state preset tables that FUN_001C8470 / FUN_001C82E0 index."""
        uc = self.uc
        uc.reg_write(UC_X86_REG_ESP, STACK_BASE + STACK_SIZE - 0x1000)
        uc.reg_write(UC_X86_REG_ESI, 0)
        uc.emu_start(FUN_RSINIT_ENTRY, FUN_RSINIT_RET)
        # 2D pass entry state (FUN_001C72F0): address preset 1, blend preset 0
        wu(uc, 0x004A1B20, 1)
        wu(uc, 0x004A1B5C, 0)
        wu(uc, 0x004A1B64, 0)
        wu(uc, 0x004A1B60, 0)
        wu(uc, 0x004A1B9C, 0)
        # and mirror those presets into the shadow arrays, the way the
        # engine's own initial state programming does
        self._apply_addr_preset(1)
        self._apply_blend_preset(0)

    def _apply_addr_preset(self, i):
        uc = self.uc
        wu(uc, TSS_SHADOW + (0 + TSS_ADDRESSU * 4) * 4, u32(uc, 0x004A1B24 + i * 4))
        wu(uc, TSS_SHADOW + (0 + TSS_ADDRESSV * 4) * 4, u32(uc, 0x004A1B68 + i * 4))

    def _apply_blend_preset(self, i):
        uc = self.uc
        wu(uc, RS_SHADOW + RS_SRCBLEND * 4, u32(uc, 0x004A1A90 + i * 4))
        wu(uc, RS_SHADOW + RS_DESTBLEND * 4, u32(uc, 0x004A1AB0 + i * 4))
        wu(uc, RS_SHADOW + RS_BLENDOP * 4, u32(uc, 0x004A1B00 + i * 4))
        wu(uc, RS_SHADOW + RS_COLORWRITEENABLE * 4, u32(uc, 0x004A1B34 + i * 4))

    # -- hooks ---------------------------------------------------------- #
    def _fault(self, uc, access, address, size, value, user):
        base = address & ~(PAGE - 1)
        try:
            uc.mem_map(base, PAGE, UC_PROT_ALL)
        except UcError:
            pass
        self.faults += 1
        return True

    def _stub_return(self, uc, pop=0):
        """Return from a stubbed callee.  `pop` is the callee-cleaned
        argument size: FUN_0034DE60 (SetTexture) and FUN_0034F9A0 are
        __stdcall -- their call sites have no `add esp` -- so a stub that
        forgets to pop leaks stack and silently shifts every later
        esp-relative slot in the CALLER's own frame."""
        esp = uc.reg_read(UC_X86_REG_ESP)
        ret = u32(uc, esp)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + pop)
        uc.reg_write(UC_X86_REG_EIP, ret)

    def _state(self):
        uc = self.uc
        return (dict(src=u32(uc, RS_SHADOW + RS_SRCBLEND * 4),
                     dst=u32(uc, RS_SHADOW + RS_DESTBLEND * 4),
                     op=u32(uc, RS_SHADOW + RS_BLENDOP * 4),
                     cw=u32(uc, RS_SHADOW + RS_COLORWRITEENABLE * 4)),
                dict(u=u32(uc, TSS_SHADOW + (0 + TSS_ADDRESSU * 4) * 4),
                     v=u32(uc, TSS_SHADOW + (0 + TSS_ADDRESSV * 4) * 4)))

    def _pool(self, uc, first, last):
        out = []
        for i in range(first, min(last, first + 4096)):
            b = uc.mem_read(VPOOL + i * VPOOL_STRIDE, VPOOL_STRIDE)
            x, y, u, v = struct.unpack_from("<4f", b, 0)
            argb, = struct.unpack_from("<I", b, 16)
            out.append((x, y, u, v, argb))
        return out

    def _code(self, uc, address, size, user):
        if self.pending and address == self.pending[0]:
            ret_at, kind, n0, node = self.pending
            self.pending = None
            n1 = u32(uc, VPOOL_COUNT)
            verts = self._pool(uc, n0, n1) if n1 > n0 else []
            if self.flushed:               # the batch spilled mid-call
                verts = self.flushed + self._pool(uc, 0, n1)
                self.flushed = None
            rs, tss = self._state()
            self.events.append(Capture(kind, self.cur_tex, rs, tss, verts,
                                       vec4(uc, node + 0x10), ret_at))
            return
        if address == FUN_CRASHMODE:
            uc.reg_write(UC_X86_REG_EAX, 0)
            self._stub_return(uc)
        elif address == FUN_SETTEX:
            esp = uc.reg_read(UC_X86_REG_ESP)
            self.cur_tex = self.tex_names.get(u32(uc, esp + 8), "?")
            self._stub_return(uc, 8)          # __stdcall(stage, texobj)
        elif address == FUN_FLUSH:
            n = u32(uc, VPOOL_COUNT)
            if self.pending and n > self.pending[2]:
                self.flushed = self._pool(uc, self.pending[2], n)
            wu(uc, VPOOL_COUNT, 0)
            if self.pending:
                self.pending = (self.pending[0], self.pending[1], 0,
                                self.pending[3])
            self._stub_return(uc)
        elif address == FUN_PUSHBUF:          # __cdecl, caller cleans
            self._stub_return(uc)
        elif address == FUN_PUSHBUF2:         # __stdcall(1 arg)
            self._stub_return(uc, 4)
        elif address in (FUN_QUADS, FUN_STRIP, FUN_QUADS_VC):
            esp = uc.reg_read(UC_X86_REG_ESP)
            kind = {FUN_QUADS: "rects", FUN_STRIP: "strip",
                    FUN_QUADS_VC: "rects_vc"}[address]
            self.pending = (u32(uc, esp), kind, u32(uc, VPOOL_COUNT),
                            u32(uc, esp + 4))
            self.flushed = None
        elif address in (FUN_PLATE, FUN_TIERFX, FUN_TREAD, FUN_CORE,
                         FUN_EARN, FUN_EDGE, FUN_OVER, FUN_SPARKS):
            self.events.append(("section", {
                FUN_PLATE: "plate", FUN_TIERFX: "tierfx", FUN_TREAD: "tread",
                FUN_CORE: "core", FUN_EARN: "earn", FUN_EDGE: "edge",
                FUN_OVER: "over", FUN_SPARKS: "sparks"}[address]))

    # -- the run --------------------------------------------------------- #
    def draw(self, clock=0.5, A=1.0, B=1.0, earnflash=0.0, flame=1.0,
             tieranim=9.0, segments=4, mode=0, half=0):
        uc = self.uc
        self.events = []
        self.cur_tex = None
        self.pending = None
        self.flushed = None
        # the draw node FUN_00048800 built: box (0,452) 360x28, rgba {1,1,1,1}
        for off in range(0, PAGE, 4):
            wu(uc, NODE + off, 0)
        wf(uc, NODE + 0x00, 0.0)          # node.x
        wf(uc, NODE + 0x04, 452.0)        # node.y
        wf(uc, NODE + 0x08, 360.0)        # node.w
        wf(uc, NODE + 0x0C, 28.0)         # node.h
        for i in range(4):
            wf(uc, NODE + 0x10 + i * 4, 1.0)   # node.rgba = {1,1,1,1}
        wu(uc, NODE + 0x20, STATE)
        wu(uc, NODE + 0x3C, FUN_DRAW)

        for off in range(0, 0x600, 4):
            wu(uc, STATE + off, 0)
        wf(uc, STATE + 0x500, clock)
        wf(uc, STATE + 0x504, A)
        wf(uc, STATE + 0x508, B)
        wf(uc, STATE + 0x50C, earnflash)
        wf(uc, STATE + 0x510, flame)
        wf(uc, STATE + 0x514, tieranim)
        uc.mem_write(STATE + 0x518, bytes([segments & 0xFF]))
        uc.mem_write(STATE + 0x519, bytes([0]))
        uc.mem_write(STATE + 0x51A, bytes([mode & 0xFF]))
        uc.mem_write(STATE + 0x51B, bytes([half & 0xFF]))

        # the 2D pass's ambient state, re-established for every run
        wu(uc, 0x004A1B20, 1)
        wu(uc, 0x004A1B5C, 0)
        wu(uc, 0x004A1B78, 0)
        wu(uc, 0x004A1B9C, 0)
        self._apply_addr_preset(1)
        self._apply_blend_preset(0)

        esp = STACK_BASE + STACK_SIZE - 0x2000
        argp = NODE + 0x20
        wu(uc, esp - 4, argp)             # [ebp+0xC] -> &statePtr
        wu(uc, esp - 8, NODE)             # [ebp+8]   = node
        wu(uc, esp - 12, MAGIC_RET)
        uc.reg_write(UC_X86_REG_ESP, esp - 12)
        uc.reg_write(UC_X86_REG_EAX, 0)
        uc.reg_write(UC_X86_REG_ECX, 0)
        uc.reg_write(UC_X86_REG_EDX, 0)
        uc.reg_write(UC_X86_REG_EBX, 0)
        uc.reg_write(UC_X86_REG_ESI, 0)
        uc.reg_write(UC_X86_REG_EDI, 0)
        try:
            uc.emu_start(FUN_DRAW, MAGIC_RET, count=8000000)
        except UcError as e:
            print("  ! emulation error: %s at eip=%08x"
                  % (e, uc.reg_read(UC_X86_REG_EIP)))
        return self.events


def argb_str(c):
    return "#%08X  A=%.3f R=%.3f G=%.3f B=%.3f" % (
        c, ((c >> 24) & 255) / 255.0, ((c >> 16) & 255) / 255.0,
        ((c >> 8) & 255) / 255.0, (c & 255) / 255.0)


def report(events, title, full=False):
    print("\n=== %s ===" % title)
    for ev in events:
        if isinstance(ev, tuple):
            print("  -- section %s" % ev[1])
            continue
        c = ev
        print("     %-8s @%08x tex=%-16s blend=%-34s addr=%-12s cw=0x%06X nv=%d"
              % (c.kind, c.site, c.tex, c.blend(), c.address(), c.rs["cw"],
                 len(c.verts)))
        cols = c.colours()
        if len(cols) <= 4:
            for v in cols:
                print("              vertex colour %s" % argb_str(v))
        else:
            print("              %d distinct vertex colours, "
                  "first %s" % (len(cols), argb_str(cols[0])))
            print("              %*s last  %s" % (24, "", argb_str(cols[-1])))
        if c.verts:
            xs = [v[0] for v in c.verts]
            ys = [v[1] for v in c.verts]
            us = [v[2] for v in c.verts]
            vs = [v[3] for v in c.verts]
            print("              x %8.2f..%8.2f   y %8.2f..%8.2f"
                  % (min(xs), max(xs), min(ys), max(ys)))
            print("              u %8.5f..%8.5f   v %8.5f..%8.5f"
                  % (min(us), max(us), min(vs), max(vs)))
        if full:
            for i, v in enumerate(c.verts):
                print("                [%2d] xy=(%9.3f,%9.3f) uv=(%9.6f,"
                      "%9.6f) %s" % (i, v[0], v[1], v[2], v[3], argb_str(v[4])))


CASES = [
    ("empty bar, no flame",
     dict(clock=0.0, A=1.0, B=0.0, earnflash=0.0, flame=0.0)),
    ("filling, earn comet live",
     dict(clock=0.37, A=1.0, B=0.45, earnflash=0.8, flame=0.55)),
    ("full bar, full flame",
     dict(clock=0.79, A=1.0, B=1.0, earnflash=0.0, flame=1.0)),
    ("draining mid-boost",
     dict(clock=1.31, A=0.75, B=0.62, earnflash=0.0, flame=1.0)),
    ("tier grew (mode 2, shake window)",
     dict(clock=2.10, A=1.0, B=0.30, earnflash=0.0, flame=0.2,
          tieranim=0.45, mode=2)),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=None)
    ap.add_argument("--case", type=int, default=None)
    ap.add_argument("--full", action="store_true",
                    help="print every emitted vertex")
    args = ap.parse_args()

    t = HudTrace()
    print("emulate_hud.py -- FUN_0004AE40 executed under Unicorn")
    print("  %d C++ float dynamic initialisers applied" % t.ninit)

    dump = {}
    cases = CASES if args.case is None else [CASES[args.case]]
    for title, kw in cases:
        ev = t.draw(**kw)
        report(ev, title, args.full)
        dump[title] = [
            dict(kind=c.kind, tex=c.tex, rs=c.rs, tss=c.tss,
                 verts=c.verts, node_rgba=c.node_rgba)
            for c in ev if not isinstance(c, tuple)]
    print("\n  (%d lazily-mapped faults)" % t.faults)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(dump, f, indent=1)
        print("  wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
