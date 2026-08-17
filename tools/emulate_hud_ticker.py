#!/usr/bin/env python3
"""
emulate_hud_ticker.py -- run Burnout 3's REAL in-race EVENT TICKER under
Unicorn: the per-frame row updater FUN_0004D310 / FUN_0004D130 and the row
draw callback FUN_0004B4D0, and capture the rows it creates, where it puts
them, and every vertex (position, UV, colour) it hands the GPU.

The ticker is the stack of "ONCOMING / DRIFT / NEAR MISS ..." labels with
star pips at the lower-left of the in-race HUD, just above the boost bar
(retail screenshot: rows "DRIFT *" and "NEAR MISS ***").

WHAT IT IS (recovered here, docs/RE_FRONTEND.md 6.8)
----------------------------------------------------
It is NOT a separate HUD element: it belongs to the BOOST BAR element
object 0x003FD550 (init FUN_0004BFC0, anchor slot 3 = bottom-left).  Its
per-frame update hangs off the same element update FUN_0004D800:

    FUN_0004D800(obj, dt):
        FUN_0004C390(obj, dt)                  # the boost bar
        if (obj+0x56A) FUN_0004D310(obj, dt)   # the ticker

Each row is its own 2D draw node built by FUN_0004B1C0 (box 210 x 26 from
0x003FCBE0/0x003FCBE4, draw callback 0x0004B4D0) -- the "second custom-drawn
HUD box whose owning element was not identified" that RE_FRONTEND 6.6.7
left [?].

What is emulated for real
-------------------------
  FUN_0004D310    the ticker's per-frame update (6 category probes + the
                  row list walk: lifetime, fade-out, stacking)
  FUN_0004D130    one category probe: open/tier/threshold -> row state,
                  row creation (calls FUN_0004B1C0)
  FUN_0004B1C0    the row's draw-node layout builder (210 x 26)
  FUN_0004B4D0    the row draw callback: label text + star pips
  FUN_0004B280    the label text layout (walks GlobalFont's charmap)
  FUN_0004DA90    the HUD module init (fills the 0x0054F5xx text styles)
  FUN_00265030    the C++ initialiser that computes 1/210 and 1/26
  FUN_001C7150    the render-state preset tables
  FUN_001C7430 / FUN_001C7710 / FUN_001C7960   the quad/strip emitters

The font is the retail GlobalFont object at 0x003C84D8 *inside the image*
(RE_FRONTEND 6.2); this tool performs the same +0x1C / charmap[128]
relocation FUN_0002EF90 does, so FUN_0004B280 lays out real glyphs.

Stubbed (captured at entry, then returned from)
-----------------------------------------------
  FUN_001C6850(...)          2D draw-node allocator -> scratch nodes
  FUN_0034DE60(stage,tex)    D3D SetTexture
  FUN_001C69C0 / FUN_001D7D50 / FUN_0034F9A0   pushbuffer flush
  FUN_00141010(...)          the "new row appeared" SFX trigger

Usage:
    python3 tools/emulate_hud_ticker.py              # the canonical cases
    python3 tools/emulate_hud_ticker.py --json out.json
    python3 tools/emulate_hud_ticker.py --full       # every vertex
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
                               UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_XMM3)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emulate_vehicle import load_elf, PAGE                      # noqa: E402
from emulate_hud import apply_float_initialisers, VPOOL, VPOOL_STRIDE, \
    VPOOL_COUNT, RS_SHADOW, TSS_SHADOW, RS_SRCBLEND, RS_DESTBLEND, \
    RS_BLENDOP, RS_COLORWRITEENABLE, TSS_ADDRESSU, TSS_ADDRESSV, \
    BLEND_TOKEN, BLENDOP_TOKEN, ADDRESS_MODE, argb_str               # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(REPO, "build", "burnout3.elf")

# ---- addresses -------------------------------------------------------- #
FUN_UPDATE      = 0x0004D310    # the ticker update (obj, dt)
FUN_ROW         = 0x0004D130    # one category probe
FUN_ROWLAYOUT   = 0x0004B1C0    # the row's node layout builder
FUN_ROWDRAW     = 0x0004B4D0    # the row draw callback
FUN_TEXT        = 0x0004B280    # the label layout
FUN_HUDINIT     = 0x0004DA90    # HUD module init (text styles)
FUN_RECIPINIT   = 0x00265030    # 1/210, 1/26
FUN_ELEMINIT    = 0x0004BFC0    # the boost/ticker element constructor

FUN_NEWNODE     = 0x001C6850
FUN_SFX         = 0x00141010
FUN_QUADS       = 0x001C7430
FUN_STRIP       = 0x001C7710
FUN_QUADS_VC    = 0x001C7960
FUN_SETTEX      = 0x0034DE60
FUN_FLUSH       = 0x001C69C0
FUN_PUSHBUF     = 0x001D7D50
FUN_PUSHBUF2    = 0x0034F9A0

FONT_GLOBAL     = 0x003C84D8    # GlobalFont object ("3rev"), RE_FRONTEND 6.2
FONT_SLOT       = 0x004D6180    # what FUN_0004B280 reads
TEX_STARS       = 0x00460940    # `hud_boost_stars` handle (FUN_0004DD00)

# element-object offsets (all [C], see the module docstring)
OBJ_ROWS        = 0x570         # 7 rows, stride 0x28
OBJ_ROW_STRIDE  = 0x28
OBJ_ENABLE      = 0x56A         # ticker enable byte (constructor arg)
OBJ_BASE_Y      = 0x56C         # stacking base = boostnode.y + h*0.5
OBJ_LIST        = 0x688         # head of the live-row list
OBJ_SCORE       = 0x68C         # the player's score object

# score-object category records FUN_0004D310 probes, in call order
ROWS = [
    (0, 0x374, 100.0, "ONCOMING"),
    (1, 0x390,   0.0, "DRIFT"),
    (2, 0x418,   0.0, "NEAR MISS"),
    (4, 0x598,   1.0, "TAILGATING"),
    (5, 0x5C4,   1.0, "GRINDING"),
    (6, 0x564,   1.0, "RUBBING"),
]
ROW_LABELS = {0: "ONCOMING", 1: "DRIFT", 2: "NEAR MISS", 3: "AIR",
              4: "TAILGATING", 5: "GRINDING", 6: "RUBBING"}

# ---- scratch layout ---------------------------------------------------- #
STACK_BASE = 0x20000000
STACK_SIZE = 0x00100000
OBJ        = 0x30000000         # the element object
SCORE      = 0x30020000         # the score object
NODEPOOL   = 0x30040000         # draw nodes handed out by the stub allocator
STRINGS    = 0x30060000         # UTF-16 labels
TEXOBJS    = 0x30100000
MAGIC_RET  = 0x50000000


def f32(uc, va):
    return struct.unpack("<f", uc.mem_read(va, 4))[0]


def u32(uc, va):
    return struct.unpack("<I", uc.mem_read(va, 4))[0]


def i8(uc, va):
    return struct.unpack("<b", uc.mem_read(va, 1))[0]


def wf(uc, va, v):
    uc.mem_write(va, struct.pack("<f", v))


def wu(uc, va, v):
    uc.mem_write(va, struct.pack("<I", v & 0xFFFFFFFF))


def wb(uc, va, v):
    uc.mem_write(va, bytes([v & 0xFF]))


def vec4(uc, va):
    return list(struct.unpack("<4f", uc.mem_read(va, 16)))


class Capture(object):
    """One emitter call: what state was live and what vertices went out."""

    def __init__(self, kind, tex, rs, tss, verts, site=0):
        self.site = site
        self.kind = kind
        self.tex = tex
        self.rs = rs
        self.tss = tss
        self.verts = verts

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

    def bbox(self):
        xs = [v[0] for v in self.verts]
        ys = [v[1] for v in self.verts]
        return (min(xs), min(ys), max(xs), max(ys)) if self.verts else None


class TickerTrace(object):
    def __init__(self):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.lo, self.hi = load_elf(self.uc, ELF)
        for base, size in ((STACK_BASE, STACK_SIZE), (OBJ, 0x4000),
                           (SCORE, 0x4000), (NODEPOOL, 0x4000),
                           (STRINGS, PAGE), (TEXOBJS, 0x10000),
                           (MAGIC_RET & ~(PAGE - 1), PAGE)):
            self.uc.mem_map(base, size, UC_PROT_ALL)
        self.ninit = apply_float_initialisers(self.uc, self.lo, self.hi)
        self.faults = 0
        self.events = []
        self.cur_tex = None
        self.pending = None
        self.flushed = None
        self.nodes = []
        self.sfx = 0
        self.tex_names = {}
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._fault)
        self.uc.hook_add(UC_HOOK_CODE, self._code)
        self._run(FUN_RECIPINIT)            # 1/210, 1/26
        self._run(FUN_HUDINIT)              # the 0x0054F5xx text styles
        self._run_rs_init()
        self._bind_textures()
        self._relocate_font()

    # -- setup ---------------------------------------------------------- #
    def _run(self, entry, eax=0, ecx=0, edx=0, esi=0, edi=0, args=()):
        uc = self.uc
        esp = STACK_BASE + STACK_SIZE - 0x8000
        for i, a in enumerate(reversed(args)):
            esp -= 4
            wu(uc, esp, a)
        esp -= 4
        wu(uc, esp, MAGIC_RET)
        uc.reg_write(UC_X86_REG_ESP, esp)
        for r, v in ((UC_X86_REG_EAX, eax), (UC_X86_REG_ECX, ecx),
                     (UC_X86_REG_EDX, edx), (UC_X86_REG_ESI, esi),
                     (UC_X86_REG_EDI, edi), (UC_X86_REG_EBX, 0)):
            uc.reg_write(r, v)
        try:
            uc.emu_start(entry, MAGIC_RET, count=20000000)
        except UcError as e:
            print("  ! emulation error: %s at eip=%08x"
                  % (e, uc.reg_read(UC_X86_REG_EIP)))

    def _bind_textures(self):
        p = TEXOBJS
        for name, base in (("hud_boost_stars", TEX_STARS),):
            wu(self.uc, p + 0x14, p + 0x1000)
            wu(self.uc, base, p)
            self.tex_names[p + 0x1000] = name
            p += 0x40
        # the font's runtime texture handle (font+0x04)
        wu(self.uc, p + 0x14, p + 0x1000)
        wu(self.uc, FONT_GLOBAL + 0x04, p)
        self.tex_names[p + 0x1000] = "GlobalFont"

    def _relocate_font(self):
        """FUN_0002EF90's relocation: the shipped font object holds file-
        relative offsets in +0x1C and its 128-entry charmap; the loader adds
        the object base exactly once (guarded by +0x18)."""
        uc = self.uc
        assert bytes(uc.mem_read(FONT_GLOBAL, 4)) == b"3rev"
        if u32(uc, FONT_GLOBAL + 0x18) == 0:
            wu(uc, FONT_GLOBAL + 0x1C, u32(uc, FONT_GLOBAL + 0x1C) + FONT_GLOBAL)
            for i in range(128):
                s = FONT_GLOBAL + 0x20 + i * 4
                wu(uc, s, u32(uc, s) + FONT_GLOBAL)
            wu(uc, FONT_GLOBAL + 0x18, 1)
        wu(uc, FONT_SLOT, FONT_GLOBAL)

    def _run_rs_init(self):
        uc = self.uc
        uc.reg_write(UC_X86_REG_ESP, STACK_BASE + STACK_SIZE - 0x1000)
        uc.reg_write(UC_X86_REG_ESI, 0)
        uc.emu_start(0x001C7168, 0x001C72E3)
        wu(uc, 0x004A1B20, 1)
        wu(uc, 0x004A1B5C, 0)
        wu(uc, 0x004A1B64, 0)
        wu(uc, 0x004A1B60, 0)
        wu(uc, 0x004A1B9C, 0)
        self._apply_addr_preset(1)
        self._apply_blend_preset(0)

    def _apply_addr_preset(self, i):
        uc = self.uc
        wu(uc, TSS_SHADOW + (0 + TSS_ADDRESSU * 4) * 4,
           u32(uc, 0x004A1B24 + i * 4))
        wu(uc, TSS_SHADOW + (0 + TSS_ADDRESSV * 4) * 4,
           u32(uc, 0x004A1B68 + i * 4))

    def _apply_blend_preset(self, i):
        uc = self.uc
        wu(uc, RS_SHADOW + RS_SRCBLEND * 4, u32(uc, 0x004A1A90 + i * 4))
        wu(uc, RS_SHADOW + RS_DESTBLEND * 4, u32(uc, 0x004A1AB0 + i * 4))
        wu(uc, RS_SHADOW + RS_BLENDOP * 4, u32(uc, 0x004A1B00 + i * 4))
        wu(uc, RS_SHADOW + RS_COLORWRITEENABLE * 4,
           u32(uc, 0x004A1B34 + i * 4))

    # -- hooks ---------------------------------------------------------- #
    def _fault(self, uc, access, address, size, value, user):
        try:
            uc.mem_map(address & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        except UcError:
            pass
        self.faults += 1
        return True

    def _stub_return(self, uc, pop=0, eax=None):
        esp = uc.reg_read(UC_X86_REG_ESP)
        ret = u32(uc, esp)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + pop)
        if eax is not None:
            uc.reg_write(UC_X86_REG_EAX, eax)
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
            ret_at, kind, n0 = self.pending
            self.pending = None
            n1 = u32(uc, VPOOL_COUNT)
            verts = self._pool(uc, n0, n1) if n1 > n0 else []
            if self.flushed:
                verts = self.flushed + self._pool(uc, 0, n1)
                self.flushed = None
            rs, tss = self._state()
            self.events.append(Capture(kind, self.cur_tex, rs, tss, verts,
                                       ret_at))
            return
        if address == FUN_NEWNODE:
            node = NODEPOOL + len(self.nodes) * 0x80
            for off in range(0, 0x80, 4):
                wu(uc, node + off, 0)
            self.nodes.append(node)
            self._stub_return(uc, 4, eax=node)          # __stdcall(1 arg)
        elif address == FUN_SFX:
            self.sfx += 1
            self._stub_return(uc, 24)                   # __stdcall(6 args)
        elif address == FUN_SETTEX:
            esp = uc.reg_read(UC_X86_REG_ESP)
            self.cur_tex = self.tex_names.get(u32(uc, esp + 8), "?")
            self._stub_return(uc, 8)
        elif address == FUN_FLUSH:
            n = u32(uc, VPOOL_COUNT)
            if self.pending and n > self.pending[2]:
                self.flushed = self._pool(uc, self.pending[2], n)
            wu(uc, VPOOL_COUNT, 0)
            if self.pending:
                self.pending = (self.pending[0], self.pending[1], 0)
            self._stub_return(uc)
        elif address == FUN_PUSHBUF:
            self._stub_return(uc)
        elif address == FUN_PUSHBUF2:
            self._stub_return(uc, 4)
        elif address in (FUN_QUADS, FUN_STRIP, FUN_QUADS_VC):
            esp = uc.reg_read(UC_X86_REG_ESP)
            kind = {FUN_QUADS: "rects", FUN_STRIP: "strip",
                    FUN_QUADS_VC: "rects_vc"}[address]
            self.pending = (u32(uc, esp), kind, u32(uc, VPOOL_COUNT))
            self.flushed = None
        elif address == FUN_TEXT:
            self.events.append(("section", "label"))
        elif address == FUN_ROWLAYOUT:
            self.events.append(("section", "row-node built"))

    # -- the element ----------------------------------------------------- #
    def reset(self, base_y=-14.0):
        """Zero the element + score objects and lay out the seven row slots
        exactly as the constructor FUN_0004BFC0 does."""
        uc = self.uc
        for off in range(0, 0x800, 4):
            wu(uc, OBJ + off, 0)
        for off in range(0, 0x1000, 4):
            wu(uc, SCORE + off, 0)
        wu(uc, OBJ + OBJ_SCORE, SCORE)
        wb(uc, OBJ + OBJ_ENABLE, 1)
        wf(uc, OBJ + OBJ_BASE_Y, base_y)
        p = STRINGS
        for i in range(7):
            r = OBJ + OBJ_ROWS + i * OBJ_ROW_STRIDE
            s = ROW_LABELS[i].encode("utf-16-le") + b"\0\0"
            uc.mem_write(p, s)
            wu(uc, r + 0x00, 0)             # no node yet
            wu(uc, r + 0x04, p)             # the label (Globalus wchar*)
            wf(uc, r + 0x08, 0.0)
            wf(uc, r + 0x0C, 0.0)
            wf(uc, r + 0x10, 0.0)
            wu(uc, r + 0x14, 0)
            wb(uc, r + 0x18, 0xFF)          # tier -1
            wf(uc, r + 0x1C, 1.0)
            wf(uc, r + 0x20, 0.0)
            wf(uc, r + 0x24, 0.0)
            p += 0x40
        wu(uc, OBJ + OBJ_LIST, 0)
        self.nodes = []
        self.sfx = 0

    def set_record(self, off, value=0.0, clock=0.0, prev=0.0, open_=1,
                   tier=0, prev_tier=-1, count=4):
        """One B3CatRecord (score+off) as FUN_00192D20 maintains it."""
        uc = self.uc
        wf(uc, SCORE + off + 0x00, value)
        wf(uc, SCORE + off + 0x04, clock)
        wf(uc, SCORE + off + 0x08, prev)
        wb(uc, SCORE + off + 0x10, open_)
        wb(uc, SCORE + off + 0x11, tier & 0xFF)
        wb(uc, SCORE + off + 0x12, prev_tier & 0xFF)
        wb(uc, SCORE + off + 0x13, count & 0xFF)

    def update(self, dt):
        self.events = []
        uc = self.uc
        esp = STACK_BASE + STACK_SIZE - 0x2000
        wu(uc, esp - 4, struct.unpack("<I", struct.pack("<f", dt))[0])
        wu(uc, esp - 8, MAGIC_RET)
        uc.reg_write(UC_X86_REG_ESP, esp - 8)
        uc.reg_write(UC_X86_REG_EAX, OBJ)
        for r in (UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                  UC_X86_REG_ESI, UC_X86_REG_EDI):
            uc.reg_write(r, 0)
        try:
            uc.emu_start(FUN_UPDATE, MAGIC_RET, count=20000000)
        except UcError as e:
            print("  ! update error: %s at eip=%08x"
                  % (e, uc.reg_read(UC_X86_REG_EIP)))
        return self.live_rows()

    def live_rows(self):
        """Walk the element's live-row list, newest first."""
        uc = self.uc
        out = []
        p = u32(uc, OBJ + OBJ_LIST)
        seen = 0
        while p and seen < 8:
            idx = (p - (OBJ + OBJ_ROWS)) // OBJ_ROW_STRIDE
            node = u32(uc, p + 0x00)
            out.append(dict(
                row=idx, label=ROW_LABELS.get(idx, "?"), slot=p, node=node,
                timer=f32(uc, p + 0x08), y=f32(uc, p + 0x10),
                tier=i8(uc, p + 0x18), flash=f32(uc, p + 0x1C),
                phase=f32(uc, p + 0x20), pulse=f32(uc, p + 0x24),
                node_xywh=[f32(uc, node + i * 4) for i in range(4)] if node
                else None,
                node_rgba=vec4(uc, node + 0x10) if node else None,
                draw_cb=u32(uc, node + 0x3C) if node else 0))
            p = u32(uc, p + 0x14)
            seen += 1
        return out

    def draw(self, node):
        """Run the row draw callback for one live row's node."""
        uc = self.uc
        self.events = []
        self.cur_tex = None
        self.pending = None
        self.flushed = None
        wu(uc, 0x004A1B20, 1)
        wu(uc, 0x004A1B5C, 0)
        wu(uc, 0x004A1B78, 0)
        wu(uc, 0x004A1B9C, 0)
        self._apply_addr_preset(1)
        self._apply_blend_preset(0)
        esp = STACK_BASE + STACK_SIZE - 0x4000
        wu(uc, esp - 4, node + 0x20)       # [ebp+0xC] -> &rowslot
        wu(uc, esp - 8, node)              # [ebp+8]   = node
        wu(uc, esp - 12, MAGIC_RET)
        uc.reg_write(UC_X86_REG_ESP, esp - 12)
        for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                  UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI):
            uc.reg_write(r, 0)
        try:
            uc.emu_start(FUN_ROWDRAW, MAGIC_RET, count=20000000)
        except UcError as e:
            print("  ! draw error: %s at eip=%08x"
                  % (e, uc.reg_read(UC_X86_REG_EIP)))
        return self.events


# --------------------------------------------------------------------- #
def report_draw(events, title, full=False):
    print("\n  --- draw %s ---" % title)
    for ev in events:
        if isinstance(ev, tuple):
            print("      -- %s" % ev[1])
            continue
        c = ev
        bb = c.bbox()
        print("      %-8s @%08x tex=%-14s blend=%-30s addr=%-11s nv=%3d"
              % (c.kind, c.site, c.tex, c.blend(), c.address(), len(c.verts)))
        if bb:
            print("               box x %7.2f..%7.2f  y %7.2f..%7.2f"
                  % (bb[0], bb[2], bb[1], bb[3]))
        cols = c.colours()
        for v in cols[:3]:
            print("               colour %s" % argb_str(v))
        if len(cols) > 3:
            print("               (%d distinct colours)" % len(cols))
        if full:
            for i, v in enumerate(c.verts):
                print("                 [%2d] xy=(%8.3f,%8.3f) uv=(%8.5f,"
                      "%8.5f) %s" % (i, v[0], v[1], v[2], v[3], argb_str(v[4])))


def dump_style(t):
    uc = t.uc
    print("\n  text style block 0x0054F510 (FUN_0004DA90):")
    print("     shadow rgba   = %r" % (vec4(uc, 0x0054F510),))
    print("     offsetA       = (%r, %r)  @0x0054F520/4"
          % (f32(uc, 0x0054F520), f32(uc, 0x0054F524)))
    print("     offsetB       = (%r, %r)  @0x0054F528/C"
          % (f32(uc, 0x0054F528), f32(uc, 0x0054F52C)))
    print("     1/box         = (%r, %r)  @0x0054F3F8/C"
          % (f32(uc, 0x0054F3F8), f32(uc, 0x0054F3FC)))
    print("     star gap/size = %r / %r    @0x0054F454 / 0x0054F3BC"
          % (f32(uc, 0x0054F454), f32(uc, 0x0054F3BC)))


def scenario_oncoming(t, args):
    """Driving oncoming: the ONCOMING record open, distance climbing."""
    print("\n=== oncoming run (record score+0x374, threshold 100 m) ===")
    t.reset()
    dt = 1.0 / 60.0
    clock = 0.0
    for i, (dist, tier) in enumerate([(40, -1), (90, -1), (120, 0), (160, 0),
                                      (260, 1), (400, 2)]):
        clock += dt
        t.set_record(0x374, value=float(dist), clock=clock, prev=0.0,
                     open_=1, tier=tier, prev_tier=-1, count=4)
        rows = t.update(dt)
        print("  frame %d  dist=%3d tier=%2d -> %d row(s)%s"
              % (i, dist, tier, len(rows),
                 "".join("  [%s node=%08x y=%.1f tier=%d flash=%.3f "
                         "pulse=%.2f]" % (r["label"], r["node"], r["y"],
                                          r["tier"], r["flash"], r["pulse"])
                         for r in rows)))
    rows = t.live_rows()
    for r in rows:
        print("     node box xywh=%s rgba=%s cb=%08x"
              % (["%.2f" % v for v in r["node_xywh"]],
                 ["%.3f" % v for v in r["node_rgba"]], r["draw_cb"]))
        report_draw(t.draw(r["node"]), "%s tier %d" % (r["label"], r["tier"]),
                    args.full)


def scenario_stack(t, args):
    """A near-miss chain while drifting: two rows stacked."""
    print("\n=== stacked rows: DRIFT then NEAR MISS chain ===")
    t.reset()
    dt = 1.0 / 60.0
    clock = 0.0
    for i in range(8):
        clock += dt
        t.set_record(0x390, value=30.0 + i * 4, clock=clock, open_=1,
                     tier=0 if i < 4 else 1, count=4)
        if i >= 2:
            t.set_record(0x418, value=float(min(i - 1, 3)), clock=clock,
                         open_=1, tier=min(i - 2, 3), count=4)
        rows = t.update(dt)
    print("  after %d frames: %d rows" % (i + 1, len(rows)))
    for r in rows:
        print("     %-10s node=%08x y=%7.2f tier=%d flash=%.3f pulse=%.2f "
              "timer=%.3f" % (r["label"], r["node"], r["y"], r["tier"],
                              r["flash"], r["phase"], r["timer"]))
        print("       box xywh=%s rgba=%s"
              % (["%.2f" % v for v in r["node_xywh"]],
                 ["%.3f" % v for v in r["node_rgba"]]))
        report_draw(t.draw(r["node"]), "%s tier %d" % (r["label"], r["tier"]),
                    args.full)
    # let them age out
    print("\n  --- ageing out (no record open) ---")
    for off in (0x390, 0x418):
        t.set_record(off, value=0.0, open_=0, tier=-1, prev_tier=1,
                     prev=999.0, count=4)
    for i in range(60):
        rows = t.update(dt)
        if i in (0, 12, 30, 50, 59):
            print("     +%2d frames: %d rows%s" % (
                i + 1, len(rows),
                "".join("  [%s y=%.1f a=%.3f w=%.1f]"
                        % (r["label"], r["y"], r["node_rgba"][3],
                           r["node_xywh"][2]) for r in rows)))


def scenario_all(t, args):
    """Every category open at once -- the full seven-slot stack."""
    print("\n=== all six driven categories open ===")
    t.reset()
    dt = 1.0 / 60.0
    for i in range(6):
        clock = (i + 1) * dt
        for k, (idx, off, thresh, name) in enumerate(ROWS):
            t.set_record(off, value=max(thresh, 1.0) + 50.0, clock=clock,
                         open_=1, tier=min(k, 3), count=4)
        rows = t.update(dt)
    print("  %d rows, list order (newest first):" % len(rows))
    for r in rows:
        print("     row %d %-10s y=%7.2f  ->  screen y %6.1f..%6.1f"
              % (r["row"], r["label"], r["y"], 480 + r["y"],
                 480 + r["y"] + 26))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=None)
    ap.add_argument("--full", action="store_true")
    args = ap.parse_args()

    t = TickerTrace()
    print("emulate_hud_ticker.py -- FUN_0004D310 / FUN_0004B4D0 under Unicorn")
    print("  %d C++ float dynamic initialisers applied" % t.ninit)
    dump_style(t)
    scenario_oncoming(t, args)
    scenario_stack(t, args)
    scenario_all(t, args)
    print("\n  (%d lazily-mapped faults, %d SFX triggers)" % (t.faults, t.sfx))
    if args.json:
        t.reset()
        dt = 1.0 / 60.0
        t.set_record(0x418, value=3.0, clock=dt, open_=1, tier=2, count=4)
        t.update(dt)
        rows = t.live_rows()
        ev = t.draw(rows[0]["node"])
        with open(args.json, "w") as f:
            json.dump([dict(kind=c.kind, tex=c.tex, rs=c.rs, tss=c.tss,
                            verts=c.verts)
                       for c in ev if not isinstance(c, tuple)], f, indent=1)
        print("  wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
