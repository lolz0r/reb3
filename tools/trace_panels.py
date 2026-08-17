#!/usr/bin/env python3
"""Trace Burnout 3's per-panel/wheel draw transforms by EXECUTING the real code.

Chain traced (all addresses = build/burnout3.elf VAs):

  FUN_000310f0  .bgv relinker (ESI = buffer; called from FUN_0018d0e0 with
                ESI = loadobj+0x50).  Run for real over the actual file bytes.
  FUN_0012fee0  damage-ctx init (EAX = state obj, [esp+4] = model obj whose
                +0x40 = buffer, [esp+8] = ctx).  Run for real.  Copies:
                  file+0xB80 + w*0x40 (w<6)      -> ctx+0x000  wheel matrices
                  file+0xD00 + k*0x40 (k<nParts) -> ctx+0x180  PANEL matrices
                  file+0x70  + k*0x40 (k<8)      -> ctx+0x700  (aux/deform set)
  FUN_000303d0  per-model draw (EAX = LOD idx, args render/modelobj/ctx).
                Run for real with the D3D-level record walkers stubbed and
                their arguments captured at entry:
                  FUN_00031e10 (rigid part draw)   ESI=part obj, EAX=matrix
                  FUN_00031e70 (deformable draw)   EAX=matrix, ECX=n matrices
                FUN_000116e0 (matrix multiply, out = A x B row-vector,
                A in ECX, B = [esp+8], out = [esp+4]) runs for real; entry and
                exit are hooked so every compose is recorded with its inputs.

Facts this script asserts by execution (not by reading):
  * relinked section/slot pointers match the extractor's layout
  * ctx+0x180 after init == file+0xD00 bytes (panel placement source)
  * intact state (ctx+0x101b == 0): ONE body draw = embedded object at
    section+0x60, mask 0x3FF (ALL records: the one-piece car), matrix = frame;
    glass pass = slot 0 object, mask 0x300; wheels = slot 7 object at
    (file+0xB80 matrix) x frame
  * damaged state (ctx+0x101b != 0): body = slot 0 object (aperture body) via
    FUN_00031e70 w/ 8 ctx+0x500 matrices; panel k (state<3) = slot k+1 object,
    mask 0x3FF, matrix = (ctx+0x180 + k*0x40) x frame  [= file+0xD00 x frame]
  * the compose convention is row-vector: v' = [x y z 1] . A . F  (proved by
    running with a non-trivial frame F and checking the captured product)

Usage:
  python3 tools/trace_panels.py                       # trace COMP/Car1.bgv
  python3 tools/trace_panels.py HEVY/Car2.bgv         # trace another car
  python3 tools/trace_panels.py COMP/Car1.bgv --render out.png
      # additionally render (PIL): intact one-piece body / slot0 aperture
      # body alone / slot0 + panels at the traced matrices, 3 views each
  python3 tools/trace_panels.py COMP/Car1.bgv --deep
      # DEEP mode: FUN_00031e10 + FUN_00031ab0 + FUN_000315c0 run FOR REAL
      # (only the push-buffer leaves are stubbed); captures every
      # FUN_0034edb0 stream bind and FUN_001d7d10 indexed-draw call.
      # Proves [C]:
      #   * FUN_000315c0(EAX=section) binds TWO vertex streams:
      #     stream 0 = D3D resource header at S+0x48 (Data ptr S+0x4C),
      #     stride 0x18 (pos+normal+uv); stream 1 = header at S+0x54
      #     (Data ptr S+0x58), stride 8 (parallel per-vertex attributes).
      #     Geometry always comes from pool 1 -- pool 2 is NOT an
      #     alternate position pool.
      #   * the draw pushes count = *(u16*)(record+0x10) INDICES at
      #     record+0x0C: record+0x10 is the INDEX COUNT, not a byte size
      #     (the old size/2 reading halved every strip).
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
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

PVEH = (game_path('pveh'))

FUN_RELINK = 0x000310f0
FUN_CTXINIT = 0x0012fee0
FUN_DRAW = 0x000303d0
FUN_MATMUL = 0x000116e0
FUN_MATMUL_RET = 0x000117d2       # its single RET 0x8
FUN_E10 = 0x00031e10              # rigid part-object draw     (stub RET 0x14)
FUN_E70 = 0x00031e70              # deformable part-object draw(stub RET 0x1C)
FUN_FADE = 0x000315c0             # fade/alpha setup           (stub RET 0x4)
FUN_TEXSEL = 0x0034de60           # palette/texture select     (stub RET 0x8)
FUN_TEXREG = 0x001c8e20           # texture-dir registration   (stub RET)
FUN_AUXRELINK = 0x00159470        # +0x16dc sub-object relink  (stub RET)

# deep mode: the record walk runs for real, only the push-buffer leaves are
# stubbed (RET imm read from each function's real tail):
FUN_DRAWIDX = 0x001d7d10          # indexed draw (6, count, idxptr)  captured
FUN_STREAM = 0x0034edb0           # stream-source bind (RET 0xC)     captured
DEEP_STUBS = ((0x0034f8f0, None),   # vshader const upload (RET)
              (0x0034f840, None),   # vshader const upload (RET)
              (0x0034e9a0, 0x4),    # vshader const upload (RET 4)
              (0x0034f9a0, 0x4),    # vshader const upload (RET 4)
              (0x0034f6d0, 0x4),    # SetVertexShader      (RET 4)
              (0x0034e790, 0x4))    # pixel-shader select  (RET 4)

STACK_BASE = 0x20000000
STACK_SIZE = 0x100000
BUF = 0x08000000        # the .bgv file image (relinked in place, as in-game)
CTX = 0x30000000        # damage/visual ctx (*(vehicle+0xCC4)); spans +0x1030
CTX_SZ = 0x2000
OBJ = 0x30010000        # FUN_0012fee0 param_1 state obj (PRNG at +0/+4)
MODEL = 0x30020000      # model obj: frame matrix @0, buffer ptr @+0x40
REND = 0x30030000       # render obj (param_2): +0x334/+0x370/+0x374 written
MAGIC_RET = 0x50000000

IDENT = [1.0, 0.0, 0.0, 0.0,
         0.0, 1.0, 0.0, 0.0,
         0.0, 0.0, 1.0, 0.0,
         0.0, 0.0, 0.0, 0.0]   # game's default: rows 0x3f8110.., pos 0x4a1f70


def matmul_rowvec(a, b):
    """FUN_000116e0's exact math: out = A x B, rows 0..2 rotated by B's 3x3,
    row 3 (position) additionally translated by B's row 3."""
    out = [0.0] * 16
    for r in range(4):
        for c in range(4):
            s = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] + a[r*4+2]*b[2*4+c]
            if r == 3:
                s += b[3*4+c]
            out[r*4+c] = s
    return out


def fmt_mat(m):
    return " / ".join(
        "[" + " ".join("%8.4f" % v for v in m[r*4:r*4+4]) + "]"
        for r in range(4))


class Tracer:
    def __init__(self, bgv_path, deep=False):
        self.data = open(bgv_path, 'rb').read()
        self.deep = deep
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        load_elf(self.uc, os.path.join(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))), "build/burnout3.elf")
            if not os.path.exists("build/burnout3.elf") else "build/burnout3.elf")
        uc = self.uc
        uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
        bufsz = (len(self.data) + PAGE) & ~(PAGE - 1)
        uc.mem_map(BUF, bufsz, UC_PROT_ALL)
        uc.mem_write(BUF, self.data)
        uc.mem_map(CTX, CTX_SZ, UC_PROT_ALL)
        for a in (OBJ, MODEL, REND):
            uc.mem_map(a, 0x1000, UC_PROT_ALL)
        uc.mem_map(MAGIC_RET & ~(PAGE - 1), PAGE, UC_PROT_ALL)

        # Stubs: the record walkers + texture calls.  RET sizes read from the
        # real code (RET imm at each function's tail; e10/e70 take 5/7 args).
        # Deep mode leaves FUN_E10 + FUN_FADE REAL (the record walk executes)
        # and instead stubs the push-buffer leaves, capturing the stream
        # binds (FUN_STREAM) and indexed draws (FUN_DRAWIDX) at entry.
        stubs = [(FUN_E70, 0x1C), (FUN_TEXSEL, 0x8)]
        if not deep:
            stubs += [(FUN_E10, 0x14), (FUN_FADE, 0x4)]
        for addr, imm in stubs:
            uc.mem_write(addr, b"\xc2" + struct.pack('<H', imm))
        for addr in (FUN_TEXREG, FUN_AUXRELINK):
            uc.mem_write(addr, b"\xc3")
        if deep:
            for addr, imm in DEEP_STUBS + ((FUN_STREAM, 0xC),):
                uc.mem_write(addr, b"\xc3" if imm is None
                             else b"\xc2" + struct.pack('<H', imm))
            uc.mem_write(FUN_DRAWIDX, b"\xc3")   # caller cleans (cdecl)

        self.calls = []          # captured draw/mul events
        self._mul_out = None
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)
        hooks = [FUN_E10, FUN_E70, FUN_MATMUL, FUN_MATMUL_RET]
        if deep:
            hooks += [FUN_DRAWIDX, FUN_STREAM]
        for a in hooks:
            uc.hook_add(UC_HOOK_CODE, self._on_code, begin=a, end=a)

    # -- unicorn plumbing ---------------------------------------------------
    def _on_unmapped(self, uc, access, address, size, value, user):
        try:
            uc.mem_map(address & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        except UcError:
            return False
        return True

    def _u32(self, addr):
        return struct.unpack('<I', bytes(self.uc.mem_read(addr, 4)))[0]

    def _mat(self, addr):
        return list(struct.unpack('<16f', bytes(self.uc.mem_read(addr, 0x40))))

    def _on_code(self, uc, addr, size, user):
        esp = uc.reg_read(UC_X86_REG_ESP)
        if addr == FUN_E10:
            self.calls.append(dict(
                fn="e10", part=uc.reg_read(UC_X86_REG_ESI),
                mat=self._mat(uc.reg_read(UC_X86_REG_EAX)),
                mask=self._u32(esp + 0x10), light=self._u32(esp + 0x14)))
        elif addr == FUN_E70:
            # 7 args: +4 render, +8 flags, +0xC part obj, +0x10 1.0f,
            # +0x14 mask, +0x18 light word, +0x1C deform-matrix base
            self.calls.append(dict(
                fn="e70", part=self._u32(esp + 0xC),
                mat=self._mat(uc.reg_read(UC_X86_REG_EAX)),
                nmat=uc.reg_read(UC_X86_REG_ECX),
                mask=self._u32(esp + 0x14),
                defbase=self._u32(esp + 0x1C)))
        elif addr == FUN_DRAWIDX:
            self.calls.append(dict(
                fn="draw", prim=self._u32(esp + 4),
                count=self._u32(esp + 8), ptr=self._u32(esp + 0xC)))
        elif addr == FUN_STREAM:
            self.calls.append(dict(
                fn="bind", stream=self._u32(esp + 4),
                hdr=self._u32(esp + 8), stride=self._u32(esp + 0xC)))
        elif addr == FUN_MATMUL:
            self._mul_out = self._u32(esp + 4)
            self.calls.append(dict(
                fn="mul", a_ptr=uc.reg_read(UC_X86_REG_ECX),
                a=self._mat(uc.reg_read(UC_X86_REG_ECX)),
                b=self._mat(self._u32(esp + 8)), out=None))
        elif addr == FUN_MATMUL_RET and self._mul_out is not None:
            for c in reversed(self.calls):
                if c["fn"] == "mul" and c["out"] is None:
                    c["out"] = self._mat(self._mul_out)
                    break
            self._mul_out = None

    def _call(self, addr, stack_args=(), regs=None, max_steps=2_000_000):
        uc = self.uc
        sp = STACK_BASE + STACK_SIZE - 0x2000
        uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
        for i, a in enumerate(stack_args):
            uc.mem_write(sp + 4 + i * 4, struct.pack('<I', a & 0xFFFFFFFF))
        uc.reg_write(UC_X86_REG_ESP, sp)
        for r, v in (regs or {}).items():
            uc.reg_write(r, v)
        uc.emu_start(addr, MAGIC_RET, count=max_steps)

    # -- the traced pipeline ------------------------------------------------
    def relink(self):
        """FUN_000310f0 exactly as FUN_0018d0e0 calls it: ESI = buffer."""
        self._call(FUN_RELINK, regs={UC_X86_REG_ESI: BUF})
        # assert the relink did what the extractor's layout says
        d = self.data
        for li in range(4):
            raw = struct.unpack_from('<I', d, 0x4C + li * 4)[0]
            if raw:
                got = self._u32(BUF + 0x4C + li * 4)
                assert got == BUF + raw, (li, hex(got), hex(raw))
        return self

    def init_ctx(self):
        """FUN_0012fee0: EAX = state obj, args (modelobj, ctx)."""
        self.uc.mem_write(MODEL + 0x40, struct.pack('<I', BUF))
        self._call(FUN_CTXINIT, stack_args=(MODEL, CTX),
                   regs={UC_X86_REG_EAX: OBJ})
        d, uc = self.data, self.uc
        nb = d[0xC]
        # execution-level proof of the three copies
        assert bytes(uc.mem_read(CTX, 0x180)) == d[0xB80:0xD00], "wheel copy"
        assert bytes(uc.mem_read(CTX + 0x180, nb * 0x40)) == \
            d[0xD00:0xD00 + nb * 0x40], "panel copy (file+0xD00)"
        assert bytes(uc.mem_read(CTX + 0x700, 8 * 0x40)) == \
            d[0x70:0x70 + 8 * 0x40], "aux copy (file+0x70)"
        assert bytes(uc.mem_read(CTX + 0x4B2, nb)) == b"\0" * nb, "states"
        return self

    def draw(self, lod, frame=None, damaged=False, visible=1):
        """FUN_000303d0: EAX = LOD index, args (render, modelobj, ctx)."""
        uc = self.uc
        uc.mem_write(MODEL, struct.pack('<16f', *(frame or IDENT)))
        uc.mem_write(MODEL + 0x40, struct.pack('<I', BUF))
        # rest-state effect params exactly as FUN_0018d0e0 writes them
        # (loadobj +0x54/0x58/0x5c/0x60 = 0,1,0,1 at 0x18d5f5..0x18d604)
        uc.mem_write(MODEL + 0x44, struct.pack('<4f', 0.0, 1.0, 0.0, 1.0))
        uc.mem_write(MODEL + 0x58, bytes([0xFF, 0, 0, visible]))
        uc.mem_write(CTX + 0x101B, bytes([1 if damaged else 0]))
        self.calls = []
        self._call(FUN_DRAW, stack_args=(REND, MODEL, CTX),
                   regs={UC_X86_REG_EAX: lod})
        return list(self.calls)

    def slot_name(self, part_ptr, lod):
        """Map a captured part-object pointer to its section slot."""
        S = self._u32(BUF + 0x4C + lod * 4)
        if part_ptr == S + 0x60:
            return "embedded(S+0x60)"
        for slot in range(18):
            if self._u32(S + slot * 4) == part_ptr:
                return "slot%d" % slot
        return "0x%08X?" % part_ptr


def render_fit(data, panel_mats, out_png):
    """Offline placement check: flat-shade (PIL) the embedded one-piece body,
    the slot0 aperture body alone, and slot0 + panels at the traced matrices.
    Panels must close the apertures for the middle/bottom rows to match."""
    from PIL import Image, ImageDraw
    import extract_bgv as EB

    u32 = lambda o: struct.unpack_from('<I', data, o)[0]
    best = None
    for li in range(5):
        sec = EB.parse_section(data, u32(0x4C + li * 4))
        if sec is None:
            continue
        nt = sum(len(t) for _, _, t in sec['body'])
        if best is None or nt > best[0]:
            best = (nt, sec, u32(0x4C + li * 4))
    _, sec, S = best
    verts = EB.read_verts(data, sec['pool'], sec['maxidx'] + 1)
    slot0 = EB.parse_part(data, S + u32(S))
    nb = data[0xC]

    def tris_of(recs, mask_filter):
        out = []
        for m, tx, tris in recs or []:
            if mask_filter(m):
                out.extend(tris)
        return out

    def xform(tri_idx, mat):
        out = []
        for t in tri_idx:
            pts = []
            for i in t:
                x, y, z = verts[i][0:3]
                if mat is None:
                    pts.append((x, y, z))
                else:
                    pts.append((
                        x*mat[0] + y*mat[4] + z*mat[8] + mat[12],
                        x*mat[1] + y*mat[5] + z*mat[9] + mat[13],
                        x*mat[2] + y*mat[6] + z*mat[10] + mat[14]))
            out.append(pts)
        return out

    body_all = xform(tris_of(sec['body'], lambda m: (m & 0xFF) != 0), None)
    s0_body = xform(tris_of(slot0, lambda m: (m & 0xFF) != 0), None)
    panels = []
    for k in range(nb):
        recs = sec['slots'].get(1 + k)
        if recs and k in panel_mats:
            panels.append(xform(tris_of(recs, lambda m: (m & 0xFF) != 0),
                                panel_mats[k]))

    W = H = 340
    views = [("side +X", lambda p: (p[2], p[1])),
             ("front +Z", lambda p: (p[0], p[1])),
             ("top +Y", lambda p: (p[0], p[2]))]
    rows = [("intact: embedded obj, all body records", [(body_all, None)]),
            ("slot0 aperture body ALONE (holes)", [(s0_body, None)]),
            ("slot0 + panels at traced file+0xD00 matrices",
             [(s0_body, None)] + [(p, k) for k, p in enumerate(panels)])]
    img = Image.new("RGB", (W * 3, H * 3 + 20), (12, 12, 16))
    dr = ImageDraw.Draw(img)
    allpts = [p for t in body_all for p in t]
    for vi, (vn, proj) in enumerate(views):
        ps = [proj(p) for p in allpts]
        x0 = min(p[0] for p in ps); x1 = max(p[0] for p in ps)
        y0 = min(p[1] for p in ps); y1 = max(p[1] for p in ps)
        sc = (W - 40) / max(x1 - x0, y1 - y0, 1e-3)
        for ri, (rn, layers) in enumerate(rows):
            ox, oy = vi * W, ri * H
            def mp(p):
                x, y = proj(p)
                return (ox + 20 + (x - x0) * sc,
                        oy + H - 20 - (y - y0) * sc)
            pal = [(230, 90, 90), (90, 200, 90), (90, 140, 230),
                   (230, 200, 80), (200, 100, 220), (90, 210, 210)]
            for tris, key in layers:
                col = (150, 150, 155) if key is None else pal[key % 6]
                for t in tris:
                    dr.polygon([mp(p) for p in t], outline=col)
            dr.text((ox + 8, oy + 4), "%s | %s" % (rn, vn), fill=(255, 255, 0))
    img.save(out_png)
    print("wrote %s" % out_png)


def deep_main(path):
    """Run the intact draw with the REAL record walk (FUN_00031e10 /
    FUN_00031ab0 / FUN_000315c0 executing) and prove the stream binds and
    per-record draw counts against the file bytes."""
    t = Tracer(path, deep=True).relink().init_ctx()
    d = t.data
    u32 = lambda o: struct.unpack_from('<I', d, o)[0]
    lods = [u32(0x4C + i * 4) for i in range(4)]
    lod = max(i for i in range(4) if lods[i])
    S = lods[lod]
    print("deep-tracing LOD %d (S=0x%X)" % (lod, S))

    # every record of every part in this section, by absolute payload offset
    recmap = {}
    def add_part(pname, P):
        cnt = struct.unpack_from('<b', d, P)[0]
        if not (0 < cnt < 64):
            return
        recs = P + u32(P + 4)
        for i in range(cnt):
            r = recs + i * 0x1C
            recmap[r + u32(r + 0xC)] = (
                pname, i, u32(r + 0x10),
                struct.unpack_from('<H', d, r + 0x18)[0])
    add_part("embedded", S + 0x60)
    for slot in range(18):
        off = u32(S + slot * 4)
        if off:
            add_part("slot%d" % slot, S + off)

    calls = t.draw(lod, damaged=False)
    binds = [c for c in calls if c["fn"] == "bind"]
    draws = [c for c in calls if c["fn"] == "draw"]
    print("\n-- stream binds (FUN_000315c0 -> FUN_0034edb0) --")
    for b in binds:
        print("stream %d  header=file+0x%06X  stride=0x%02X"
              % (b["stream"], b["hdr"] - BUF, b["stride"]))
    assert any(b["stream"] == 0 and b["hdr"] == BUF + S + 0x48
               and b["stride"] == 0x18 for b in binds), "stream0 != S+0x48"
    assert any(b["stream"] == 1 and b["hdr"] == BUF + S + 0x54
               and b["stride"] == 8 for b in binds), "stream1 != S+0x54"
    print("[C] stream 0 = S+0x48 hdr (pool S+0x4C) stride 0x18; "
          "stream 1 = S+0x54 hdr (pool S+0x58) stride 8")

    print("\n-- indexed draws (FUN_001d7d10) vs record+0x10 --")
    nok = 0
    for c in draws:
        rel = c["ptr"] - BUF
        info = recmap.get(rel)
        assert info, "draw ptr 0x%X not a record payload" % rel
        pname, i, isize, mask = info
        assert c["prim"] == 6, c["prim"]
        assert c["count"] == isize & 0xFFFF, (pname, i, c["count"], isize)
        print("%-9s rec%-2d mask=0x%03X  drawn count=%5d  (u32@+0x10=0x%X,"
              " old size/2 reading = %d)"
              % (pname, i, mask, c["count"], isize, isize // 2))
        nok += 1
    assert nok > 0
    print("\n[C] %d draws: count pushed = *(u16*)(rec+0x10) INDICES at "
          "rec+0x0C -- rec+0x10 is the index COUNT (the size/2 reading "
          "halved every strip)" % nok)
    return 0


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    render_out = None
    if "--render" in sys.argv:
        i = sys.argv.index("--render")
        render_out = sys.argv[i + 1] if i + 1 < len(sys.argv) else "panels_fit.png"
        if render_out in args:
            args.remove(render_out)
    rel = args[0] if args else "COMP/Car1.bgv"
    path = rel if os.path.isabs(rel) else os.path.join(PVEH, rel)
    if not os.path.exists(path):
        path = rel
    if "--deep" in sys.argv:
        return deep_main(path)
    print("== tracing %s ==" % path)
    t = Tracer(path).relink().init_ctx()
    d = t.data
    nb, nw = d[0xC], d[0xD]
    # highest-detail LOD = the extractor's rule (most body triangles); the
    # draw's own walk starts at the requested index, so request it directly.
    lods = [struct.unpack_from('<I', d, 0x4C + i * 4)[0] for i in range(4)]
    lod = max(range(4), key=lambda i: lods[i] or -1)  # sections ascend; last
    lod = max(i for i in range(4) if lods[i])
    print("numBodyParts=%d numWheels=%d, using LOD idx %d (S=0x%X)"
          % (nb, nw, lod, lods[lod]))

    print("\n-- intact draw (ctx+0x101b = 0) --")
    for c in t.draw(lod, damaged=False):
        if c["fn"] == "mul":
            continue
        print("%s part=%-18s mask=0x%03X  mat= %s"
              % (c["fn"], t.slot_name(c["part"], lod), c["mask"],
                 fmt_mat(c["mat"])))

    print("\n-- damaged-intact draw (ctx+0x101b = 1, all panel states 0) --")
    panel_mats = {}
    calls = t.draw(lod, damaged=True)
    for c in calls:
        if c["fn"] == "mul":
            continue
        name = t.slot_name(c["part"], lod)
        print("%s part=%-18s mask=0x%03X  mat= %s"
              % (c["fn"], name, c["mask"], fmt_mat(c["mat"])))
        if name.startswith("slot") and c["fn"] == "e10":
            slot = int(name[4:])
            if 1 <= slot <= nb:
                panel_mats[slot - 1] = c["mat"]

    # prove panel matrix source: composed(k) == file+0xD00[k] x frame(identity)
    for k, m in sorted(panel_mats.items()):
        src = list(struct.unpack_from('<16f', d, 0xD00 + k * 0x40))
        assert max(abs(a - b) for a, b in zip(m, src)) < 1e-6, k
    print("\n[C] all %d attached-panel draw matrices == file+0xD00 array "
          "(identity frame)" % len(panel_mats))

    # prove the compose convention with a non-trivial frame
    import math
    th = math.radians(30.0)
    F = [math.cos(th), 0.0, -math.sin(th), 0.0,
         0.0, 1.0, 0.0, 0.0,
         math.sin(th), 0.0, math.cos(th), 0.0,
         1.0, 2.0, 3.0, 0.0]
    calls = t.draw(lod, frame=F, damaged=True)
    checked = 0
    for c in calls:
        if c["fn"] != "e10":
            continue
        name = t.slot_name(c["part"], lod)
        if not name.startswith("slot"):
            continue
        slot = int(name[4:])
        if not (1 <= slot <= nb):
            continue
        src = list(struct.unpack_from('<16f', d, 0xD00 + (slot - 1) * 0x40))
        want = matmul_rowvec(src, F)
        err = max(abs(a - b) for a, b in zip(c["mat"], want))
        assert err < 1e-5, (slot, err)
        checked += 1
    print("[C] %d panel matrices under a 30deg+translate frame match "
          "row-vector A x F (max err < 1e-5)" % checked)

    print("\n-- per-panel placement (file+0xD00 + k*0x40, row-vector) --")
    kinds = [struct.unpack_from('<i', d, 0xAC4 + 4 * i)[0] for i in range(nb)]
    for k in range(nb):
        m = list(struct.unpack_from('<16f', d, 0xD00 + k * 0x40))
        print("panel%d kind%d  pos=(%7.4f %7.4f %7.4f)  rows= %s"
              % (k, kinds[k], m[12], m[13], m[14], fmt_mat(m[:12] + [0]*4)))

    if render_out:
        render_fit(d, panel_mats, render_out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
