#!/usr/bin/env python3
"""Extract Burnout 3 (Xbox) .bgv vehicle meshes to OBJ.

The layout below comes from the game's own pointer-relocation pass
(FUN_000310f0 + per-LOD FUN_00031010, reached from the .bgv load completion
FUN_0018d0e0) -- see BGV_EXTRACTION.md. All offsets were verified against the
relinker's field list, then empirically on every player vehicle (car-scale
bounds, zero out-of-range indices, UVs in [0,1], ~0.1 m median edge length).

  header   +0x00 u32 version 0x17, +0x08 u32 file size,
           +0x0C u8 numBodyParts, +0x0D u8 numWheels  [C: read by the damage
             distributor FUN_00023DE0 / draw path FUN_000303D0]
           +0x18 f32 wheel radius
           +0x4C u32[5] LOD section offsets, +0x60 u32 material dir offset
           +0x70 4x4 f32 x8, stride 0x40: aux/deformation matrix set [C:
             FUN_0012FEE0 copies all 8 to ctx+0x700; NOT the panel placement
             -- nothing in the draw path places panels with these]
           +0xAC4 i32[numBodyParts] panel kind ids [C: FUN_00023DE0 matches
             them against the priority table DAT_00385224 = 3,6,0,1,5,4,2;
             kinds {0,1,3,5,6} are never detached by the health path]
           +0xB80 4x4 f32 x6 WHEEL matrices, stride 0x40, rows
             Right/Up/At/Pos [C: FUN_0012FEE0 copies exactly six (0xB80..
             0xCFF) to ctx+0x000; the draw composes wheel w at
             (ctx + w*0x40) x frame.  The community PS2 layout (Matrix3x4,
             stride 0x30) does NOT apply to the Xbox files.]
           +0xD00 4x4 f32 x numBodyParts PANEL PLACEMENT matrices, stride
             0x40 [C execution-traced, tools/trace_panels.py: FUN_0012FEE0
             copies them to ctx+0x180; FUN_000303D0's panel loop (damage
             path, state<3) composes slot k+1 at (ctx+0x180 + k*0x40) x
             frame via FUN_000116e0, row-vector v' = v.A.F, position in
             row 3.  In practice rotation = identity, position = attach.]
  section  (= one LOD; an 18-slot part table FUN_00031010 relinks)
           +0x00 u32[18] part-object offsets rel section:
                 slot 0     deformable aperture body: the car MINUS panels,
                            with interior/driver.  Drawn only in the damage
                            path (FUN_00031E70, mask 0xFF, 8 matrices at
                            ctx+0x500) plus its glass in a mask-0x300 pass.
                 slot 1..numBodyParts (<=6)  damage panels, pivot-local
                 slot 7/8/9 wheel mesh: slow / blur>25rad/s / blur>50rad/s
                            (draw picks S+0x1C/0x20/0x24 by |wheel spin|)
                 slot 10..17 debris pieces (aux model records index them)
           +0x48 D3D vertex-buffer resource header {u32 Common, u32 Data}:
                 Data (+0x4C) = vertex-pool offset rel section.  [C deep-
                 traced, trace_panels.py --deep: FUN_000303D0 passes the
                 section to FUN_000315C0, which binds this header as vertex
                 STREAM 0, stride 0x18, via FUN_0034EDB0(0, S+0x48, 0x18)]
           +0x54 second resource header: Data (+0x58) = second pool, bound
                 as STREAM 1, stride 8 [C: FUN_0034EDB0(1, S+0x54, 8)].
                 A parallel per-vertex ATTRIBUTE stream (same index space);
                 position/normal/uv all live in stream 0, so geometry
                 never comes from pool 2.
           +0x60 s8  record count, +0x64 u32 record-array offset rel S+0x60
                 (the embedded ONE-PIECE INTACT car: this whole object, ALL
                 records, is what FUN_000303D0 draws for an undamaged car --
                 single FUN_00031E10 call, mask 0x3FF, at the frame matrix.
                 No panel placement happens in the intact state.  With the
                 full index count (below) it IS panel-complete. [C traced])
  part     s8 record count at +0, u32 record-array offset rel part at +4
  record   0x1C bytes: +0x0C u32 index offset rel record,
           +0x10 u16 INDEX COUNT [C deep-traced: FUN_00031AB0 pushes
             MOVZX word ptr [rec+0x10] indices at rec+0x0C to the indexed
             draw FUN_001D7D10 -- NOT a byte size; the old size/2 reading
             halved every strip, which was the source of every "missing
             panel half" hole and the door-completeness asymmetry],
           +0x18 u16 mask, +0x1A u8 texture slot.  The mask picks the
             SHADER branch in FUN_00031AB0, not a damage variant:
             bit1 = shader from rec+0x14; bits2..7 = light elements (lit
             when the model's light byte has the bit); bit8|bit9 = glass
             (two draw calls per record inside FUN_00031AB0; FUN_000300A0
             retargets texture slot 2 intact / 3 cracked / 4 shattered,
             tint 1.0 / 0.5 / 0.6 -- execution-verified); else plain body.
             Every record matching the pass mask IS drawn.

             THE TEXTURE SLOT IS NOT AN INDEX INTO THE .bgv.  Slot 0 is
             the file's own paint page; slots 1..4 are GLOBAL textures out
             of Data/Global.txd, shared by every car.  [C], end to end:

               FUN_00031AB0 opens with
                   MOV EAX, [ESI + 0x334 + movzx(byte [rec+0x1A])*4]
                 (0x00031AC5) -- rec+0x1A indexes a five-entry texture
                 pointer array on the DRAW CONTEXT at ctx+0x334, and a
                 non-zero entry becomes the bound texture DAT_0075DB70.

               FUN_000303D0 fills entry 0 from the model itself:
                   ctx+0x334 = *(model + 0x60)         (0x00030546)
                 i.e. the .bgv's own "compact1" paint record, then picks
                 its palette by the car's colour byte (model+0x59).

               FUN_000315C0 fills entries 1..4 from four BSS globals:
                   ctx+0x338 = DAT_004D61B4            (0x000317C0)
                   ctx+0x33C = DAT_004D61A8            (0x000317CB)
                   ctx+0x340 = DAT_004D61AC            (0x000317D7)
                   ctx+0x344 = DAT_004D61B0            (0x000317E3)

               and those four are written, in order, by the car-system
               init at 0x0002F260, each from a lookup-by-name
               (FUN_0002DDF0 = __stricmp against the texture record's
               name field at +0x48) in the global bank DAT_004D1FE0:

                   +0x38 = 0x4D61A8 <- "UnbrokenGlass"  -> slot 2
                   +0x3C = 0x4D61AC <- "CrackedGlass"   -> slot 3
                   +0x40 = 0x4D61B0 <- "SmashedGlass"   -> slot 4
                   +0x44 = 0x4D61B4 <- "VehicleUnderside" -> SLOT 1

             Slot 2/3/4 are exactly the intact/cracked/shattered tiers
             FUN_000300A0 retargets, which confirms the base is right.

             So SLOT 1 == "VehicleUnderside", a shared 512x256 chassis
             page (exhausts, transmission tunnel, diff, subframes) that
             lives in Data/Global.txd, not in the car file.  Every
             shipped vehicle's slot-1 records are its UNDERSIDE: 81 of
             the 107 .bgv/.btv files carry one, and on all 81 the
             area-weighted geometric normal points DOWN (mean n.y -0.70
             to -0.94).  tools/extract_txd.py already writes the page to
             build/frontend/VehicleUnderside.png.

             This is why every emitted group carries `usemtl b3tex<slot>`
             below: flattening the underside onto the paint page makes it
             sample the livery -- mirrored sponsor decals, tail-light and
             wheel-spoke art smeared across the car's belly, visible the
             moment the car flips.
  vertex   0x18 bytes: f32[3] position, D3DVSDT_NORMPACKED3 normal at
           +0x0C, f32[2] uv at +0x10.  ONE positions pool per section: every
           slot's records index the same stream-0 pool (COMP Car1: maxidx
           7169 == pool capacity 7170 exactly).

           THE NORMAL IS NOT s8[4].  It is the Xbox packed-normal vertex
           type: x = bits 0..10 signed / 1023, y = bits 11..21 signed /
           1023, z = bits 22..31 signed / 511.  [C] from the CAR shader
           factory FUN_0003c8a0, which creates the car vertex shader at
           0x0003cb78 --

               0003cb6e  PUSH 0x3e7d58     ; vertex program
               0003cb73  PUSH 0x387558     ; vertex DECLARATION
               0003cb78  CALL 0x0034f440   ; CreateVertexShader

           and the declaration at 0x00387558 is

               20000000 40320000 40160002 40220009 FFFFFFFF
               stream0  v0=FLOAT3 v2=NORMPACKED3 v9=FLOAT2

           = stride 0x18, normal at +0x0C, uv at +0x10, which is exactly the
           stride FUN_000315c0 binds for stream 0.  (That this is the CAR's
           factory and not the world's is [C] too: FUN_0003c8a0 CALLs
           0x0002ef90 at 0x0003cb60, the function that writes the car
           body/glass Fresnel pair into the draw context -- RE_CARFX.md 2.5.
           tools/extract_track.py already decodes the identical type for the
           world vertex, from declaration 0x0038758c.)

           EMPIRICALLY, on COMP/Car1's 7170-vertex pool, against the
           area-averaged geometric normal of the body triangles:

               s8[4]/127          mean dot -0.036, 55% NEGATIVE, |n| ~ 120
               NORMPACKED3 11/11/10   mean dot +0.9916, median 1.0000,
                                      98.9% above 0.7, none negative,
                                      |n| in [0.998, 1.000]

           The s8 reading is noise.  It is what produced the marbled
           bright/dark streaking over the car body in the harness, because
           burnout3_full.c now feeds these `vn` to the SH specular's
           reflect(-V, N).  tools/validate_carfx.py section 11 re-runs both
           decodes and asserts the split.
  indices  u16 triangle strip, degenerate restarts, alternate winding

Highest-detail LOD = the section producing the most triangles (LOD3 in
practice). Meshes are full-width -- no mirroring required.

Every emitted OBJ tags each record span with `usemtl b3tex<slot>`, the
record's own +0x1A texture slot (see `record` above).  No file is added or
removed and no geometry changes -- the `usemtl` line is the only difference
from the pre-fix output.  src/burnout3_trackmesh.c already turns `usemtl`
runs into TrackMeshGroups with first_triangle/triangle_count, so the harness
binds the shared VehicleUnderside page for the slot-1 (underside) span
instead of flattening the whole body onto the car's paint page.

Outputs per car (build/cars/):
  <NAME>.obj            whole-car, all records
  <NAME>_intact.obj     the pristine car: the embedded one-piece part
                        (S+0x60), non-glass records -- exactly the single
                        mask-0x3FF draw of the intact state.  Panel-
                        complete with the correct index count (both doors,
                        bonnet, hatch; the old "embedded ships without
                        bonnet/hatch" was the halved-strip artifact)
  <NAME>_shell.obj      slot 0 aperture body (car minus panels, interior/
                        driver): the game's damage-path body, and the wreck
                        end state once every panel has detached
  <NAME>_glass.obj      embedded-part glass records (mask bits 8/9, drawn
                        in the intact pass; tint separately)
  <NAME>_wheel.obj      wheel mesh (slot 7 -- the slow/stopped wheel the
                        game draws below 25 rad/s), origin-centred
  <NAME>.wheels         wheel radius + per-wheel attach pos + mirror flag
  <NAME>.panels         per-panel placement matrices (file+0xD00, [C])
  parts/<NAME>/panel<K>_kind<D>.obj   per-panel meshes (pivot-local space)
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import os
import struct
import sys

PVEH = (game_path('pveh'))


def strip_to_triangles(indices):
    tris = []
    for i in range(len(indices) - 2):
        a, b, c = indices[i], indices[i + 1], indices[i + 2]
        if a == b or b == c or a == c:
            continue
        tris.append((a, b, c) if i % 2 == 0 else (a, c, b))
    return tris


def parse_part(data, P):
    """Part object at P: s8 record count, u32 record-array offset rel P.
    Returns [(mask, tex_slot, tris)] or None."""
    def u32(o): return struct.unpack_from('<I', data, o)[0]
    if not (0 < P < len(data) - 8):
        return None
    cnt = struct.unpack_from('<b', data, P)[0]
    if not (0 < cnt < 64):
        return None
    recs = P + u32(P + 4)
    out = []
    for i in range(cnt):
        r = recs + i * 0x1C
        if r + 0x1C > len(data):
            return None
        ioff, isize = u32(r + 0xC), u32(r + 0x10)
        mask = struct.unpack_from('<H', data, r + 0x18)[0]
        tex = data[r + 0x1A]
        payload = r + ioff
        # [C deep-traced, tools/trace_panels.py --deep]: the draw pushes
        # count = *(u16*)(rec+0x10) INDICES (MOVZX at 0x31df3) -- +0x10 is
        # the index COUNT, not a byte size (size/2 halved every strip and
        # was the source of all the "missing panel half" holes).
        n = isize & 0xFFFF
        if n < 3 or payload + 2 * n > len(data):
            continue
        idx = struct.unpack_from('<%dH' % n, data, payload)
        if max(idx) > 0x8000:
            return None
        out.append((mask, tex, strip_to_triangles(idx)))
    return out


def parse_section(data, S):
    """One LOD section. Returns dict with pool offset, slot part-record
    lists, the embedded whole-car records, and max index -- or None."""
    def u32(o): return struct.unpack_from('<I', data, o)[0]
    if not (0 < S < len(data) - 0x70):
        return None
    # Relinker's pool rule: offset += S, masked unless (flags&0x70000)==0x20000
    pool = u32(S + 0x4C)
    if (u32(S + 0x48) & 0x70000) != 0x20000:
        pool &= 0xFFFFFFF
    pool += S
    body = parse_part(data, S + 0x60)
    if body is None or not (0 < pool < len(data)):
        return None
    slots = {}
    for slot in range(0, 10):       # 0 = aperture body, 1..6 panels, 7-9 wheels
        off = u32(S + slot * 4)
        if off:
            slots[slot] = parse_part(data, S + off)
    maxidx = -1
    for recs in [body] + [r for r in slots.values() if r]:
        for _, _, tris in recs:
            for t in tris:
                m = max(t)
                if m > maxidx:
                    maxidx = m
    if maxidx < 2 or pool + (maxidx + 1) * 0x18 > len(data):
        return None
    return dict(pool=pool, body=body, slots=slots, maxidx=maxidx)


def unpack_normal(w):
    """D3DVSDT_NORMPACKED3 -> (x, y, z).  [C] from the car vertex declaration
    at 0x00387558 (see the module docstring); identical to the world decode in
    tools/extract_track.py, whose citation is declaration 0x0038758c."""
    x = w & 0x7FF
    y = (w >> 11) & 0x7FF
    z = (w >> 22) & 0x3FF
    if x & 0x400:
        x -= 0x800
    if y & 0x400:
        y -= 0x800
    if z & 0x200:
        z -= 0x400
    return (x / 1023.0, y / 1023.0, z / 511.0)


def read_verts(data, pool, count):
    verts = []
    for i in range(count):
        o = pool + i * 0x18
        x, y, z = struct.unpack_from('<3f', data, o)
        w, = struct.unpack_from('<I', data, o + 0xC)
        u, v = struct.unpack_from('<2f', data, o + 0x10)
        nx, ny, nz = unpack_normal(w)
        # The packed value is already unit to ~4e-4; renormalise anyway so a
        # degenerate word can never emit a zero-length `vn`.
        l = (nx * nx + ny * ny + nz * nz) ** 0.5
        if l < 1e-6:
            nx, ny, nz, l = 0.0, 1.0, 0.0, 1.0
        verts.append((x, y, z, u, v, nx / l, ny / l, nz / l))
    return verts


def read_wheels(data):
    """Wheel matrices at +0xB80, stride 0x40 (4x4, rows Right/Up/At/Pos) [C],
    radius at +0x18. Returns (radius, [(pos3, mirror)])."""
    nw = data[0xD]
    radius = struct.unpack_from('<f', data, 0x18)[0]
    wheels = []
    for w in range(min(nw, 6)):
        b = 0xB80 + w * 0x40
        if b + 0x40 > len(data):
            break
        right = struct.unpack_from('<3f', data, b)
        pos = struct.unpack_from('<3f', data, b + 0x30)
        wheels.append((pos, 1 if right[0] >= 0 else -1))
    return radius, wheels


def write_obj(path, verts, groups, header_note):
    """Write an OBJ. `groups` entries index `verts` and are either

        (name, tris)            -- legacy, no material emitted
        (name, tex_slot, tris)  -- also emits `usemtl b3tex<tex_slot>`

    where tex_slot is the record's own +0x1A texture slot (see the module
    docstring: 0 = the car's paint page, 1 = the shared "VehicleUnderside"
    chassis page, 2..4 = the shared glass tiers).  The `usemtl` span is how
    a consumer learns which page a triangle belongs to -- src/
    burnout3_trackmesh.c already turns `usemtl` runs into TrackMeshGroups
    with first_triangle/triangle_count, so the renderer can bind per group
    instead of flattening the whole body onto one page.

    Legacy 2-tuple callers emit byte-identical output to before.
    Only used verts are written."""
    groups = [(g[0], None, g[1]) if len(g) == 2 else (g[0], g[1], g[2])
              for g in groups]
    used = sorted({i for _, _, tris in groups for t in tris for i in t})
    if not used:
        return False
    remap = {vi: n + 1 for n, vi in enumerate(used)}
    with open(path, 'w') as f:
        f.write("# Burnout 3 vehicle mesh, tools/extract_bgv.py\n")
        f.write("# layout from the game's relinker FUN_000310f0/FUN_00031010"
                " (BGV_EXTRACTION.md)\n")
        f.write("# %s\n" % header_note)
        for vi in used:
            x, y, z = verts[vi][0:3]
            f.write("v %.5f %.5f %.5f\n" % (x, y, z))
        for vi in used:
            u, v = verts[vi][3:5]
            # THE V ORIGIN RULE (see tools/extract_textures.py): v=0 is texel
            # row 0 for every game surface, and glTexImage2D puts t=0 on the
            # first row of the data -- NO flip. The old reflex "1-v" was the
            # same bug the track extractor had; it put every car livery/decal
            # in the wrong place once the paint PNGs were emitted upright.
            f.write("vt %.5f %.5f\n" % (u, v))
        for vi in used:
            nx, ny, nz = verts[vi][5:8]
            f.write("vn %.4f %.4f %.4f\n" % (nx, ny, nz))
        for name, tex, tris in groups:
            f.write("o %s\n" % name)
            if tex is not None:
                f.write("usemtl b3tex%d\n" % tex)
            for a, b, c in tris:
                a, b, c = remap[a], remap[b], remap[c]
                f.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n"
                        % (a, a, a, b, b, b, c, c, c))
    return True


LIGHT_BITS = 0x0FC   # mask bits 2..7: light elements


def extract(path, outdir, partsdir, name):
    data = open(path, 'rb').read()
    def u32(o): return struct.unpack_from('<I', data, o)[0]
    ver = u32(0)
    if not (0x14 <= ver <= 0x25):
        return None, "version 0x%X unsupported" % ver

    nb, nw = data[0xC], data[0xD]
    kinds = [struct.unpack_from('<i', data, 0xAC4 + 4 * i)[0]
             for i in range(nb)] if nb else []

    best = None
    for li in range(5):
        sec = parse_section(data, u32(0x4C + li * 4))
        if sec is None:
            continue
        ntris = sum(len(t) for _, _, t in sec['body'])
        if best is None or ntris > best[0]:
            best = (ntris, sec)
    if best is None:
        return None, "no valid LOD section"
    sec = best[1]
    verts = read_verts(data, sec['pool'], sec['maxidx'] + 1)

    zs = [v[2] for v in verts]
    if not (0.5 < max(zs) - min(zs) < 20):
        return None, "implausible length %.1f" % (max(zs) - min(zs))

    # 1. whole-car OBJ (all body records).  Group names unchanged; each now
    #    also carries its record's texture slot as `usemtl b3tex<slot>`.
    groups = [("part_%02d_tag_%X" % (i, m | (tx << 16)), tx, t)
              for i, (m, tx, t) in enumerate(sec['body'])]
    write_obj(os.path.join(outdir, name + ".obj"), verts, groups,
              "whole car: every record of the embedded part object")

    # 2. damage-state variants of the body.
    # [C deep-traced, tools/trace_panels.py --deep, executing the real
    # FUN_00031E10/FUN_00031AB0/FUN_000315C0]: the intact car on screen is
    # ONE draw of the embedded part object (S+0x60, mask 0x3FF, frame
    # matrix) -- and with rec+0x10 decoded as the index COUNT the embedded
    # object IS panel-complete (both doors, bonnet, hatch; the earlier
    # "ships without bonnet/hatch" claim was the halved-strip artifact of
    # the size/2 reading).  _intact.obj is therefore exactly that object's
    # non-glass records; its mask-0x300 records are the intact glass (drawn
    # in the same pass via the glass two-draw in FUN_00031AB0).  The slot 0
    # aperture body + panels at the file+0xD00 matrices remain the DAMAGE
    # path assembly (FUN_00031E70 + per-panel e10) -- kept as _shell.obj +
    # parts/<car>/panel*.obj + the .panels sidecar.
    slot0 = sec['slots'].get(0)
    panel_mats = [struct.unpack_from('<16f', data, 0xD00 + k * 0x40)
                  for k in range(nb)]
    intact = [("body_m%X" % m, tx, t) for m, tx, t in sec['body']
              if (m & 0x300) == 0]
    glass = [("glass_m%X" % m, tx, t) for m, tx, t in sec['body']
             if m & 0x300]
    # [C traced] slot 0 alone = the wreck end state (every panel detached).
    shell = [("shell_s0_m%X" % m, tx, t) for m, tx, t in slot0
             if (m & 0x300) == 0] if slot0 else list(intact)
    write_obj(os.path.join(outdir, name + "_intact.obj"), verts, intact,
              "intact car = the embedded one-piece part (S+0x60), non-glass"
              " records: the single mask-0x3FF draw of the intact state"
              " (trace_panels.py --deep [C])")
    write_obj(os.path.join(outdir, name + "_shell.obj"), verts, shell,
              "slot 0 aperture body (car minus panels, interior/driver):"
              " the damage-path body / wreck end state [C]")
    write_obj(os.path.join(outdir, name + "_glass.obj"), verts, glass,
              "embedded-part glass records, mask bits 8/9 (drawn in the"
              " intact pass; FUN_000300A0 retints by damage tier)")

    # 3. per-panel meshes (slots 1..numBodyParts, pivot-local space) + the
    # placement sidecar.  [C] tools/trace_panels.py executed the draw path:
    # panel k = slot k+1 placed at (file+0xD00 + k*0x40) x carframe,
    # row-vector (v' = v.A.F, position in row 3).
    pdir = os.path.join(partsdir, name)
    os.makedirs(pdir, exist_ok=True)
    npanels = 0
    with open(os.path.join(outdir, name + ".panels"), 'w') as pf:
        pf.write("# per-panel placement matrices, file+0xD00 + k*0x40 [C:\n"
                 "# execution-traced, tools/trace_panels.py -- FUN_0012FEE0\n"
                 "# copies them to ctx+0x180; FUN_000303D0 composes panel\n"
                 "# slot k+1 at (ctx+0x180+k*0x40) x frame, row-vector].\n"
                 "# panel <k> <kind> <16 floats: rows Right,Up,At,Pos>\n"
                 "# panelbb <k> <axis> <max.xyzw> <min.xyzw>  -- the\n"
                 "#   PIVOT-LOCAL AABB the flying-part activation ctor\n"
                 "#   FUN_001069C0 seeds a detached panel from [C]:\n"
                 "#     piece+0x1D0 (bbMAX) = file+0xEA0 + k*0x20 + 0x00\n"
                 "#     piece+0x1E0 (bbMIN) = file+0xEA0 + k*0x20 + 0x10\n"
                 "#   (the ctor reads them as (idx+0x75)*0x20 off the .bgv\n"
                 "#   base -- the same base as the file+0xADC byte below).\n"
                 "#   <axis> = file+0xADC+k, the hinge axis that decides\n"
                 "#   which component of the box centre is zeroed before\n"
                 "#   the recentring (0 -> y, 1 -> x, 2 -> y and z).\n")
        for k in range(nb):
            kind = kinds[k] if k < len(kinds) else -1
            mat = struct.unpack_from('<16f', data, 0xD00 + k * 0x40)
            pf.write("panel %d %d %s\n"
                     % (k, kind, " ".join("%.6f" % v for v in mat)))
            bb = struct.unpack_from('<8f', data, 0xEA0 + k * 0x20)
            axis = data[0xADC + k]
            pf.write("panelbb %d %d %s\n"
                     % (k, axis, " ".join("%.6f" % v for v in bb)))
            recs = sec['slots'].get(1 + k)
            if not recs:
                continue
            groups = [("m%X_t%d" % (m, tx), tx, t) for m, tx, t in recs]
            if write_obj(os.path.join(
                    pdir, "panel%d_kind%d.obj" % (k, kind)), verts, groups,
                    "panel slot %d, kind %d, PIVOT-LOCAL space; placement = "
                    "row-vector matrix file+0x%X (see %s.panels) [C]"
                    % (1 + k, kind, 0xD00 + k * 0x40, name)):
                npanels += 1

    # 4. wheel mesh.  [C traced] FUN_000303D0 selects the wheel part object
    # by |wheel spin|: slot 7 (S+0x1C) below 25 rad/s, slot 8 above, slot 9
    # above 50 rad/s (motion-blur variants).  The stationary/slow wheel the
    # player sees up close is slot 7 -- use it, falling back to the largest
    # of 8/9 if a file ships without one.
    nwheel_mesh = 0
    best_slot, best_recs, fb_slot, fb_size = None, None, None, -1
    for slot in (7, 8, 9):
        recs = sec['slots'].get(slot)
        if not recs:
            continue
        size = sum(len(t) for _, _, t in recs)
        if slot == 7 and best_slot is None:
            best_slot, best_recs = slot, recs
        if size > fb_size:
            fb_slot, fb_size = slot, size
    if best_slot is None and fb_slot is not None:
        best_slot, best_recs = fb_slot, sec['slots'][fb_slot]
    if best_recs:
        groups = [("m%X" % m, tx, t) for m, tx, t in best_recs]
        if write_obj(os.path.join(outdir, name + "_wheel.obj"), verts, groups,
                     "wheel slot %d (draw path: 7 slow / 8 / 9 blur), all"
                     " records" % best_slot):
            nwheel_mesh += 1
    for slot in (7, 8, 9):
        recs = sec['slots'].get(slot)
        if not recs:
            continue
        groups = [("m%X" % m, tx, t) for m, tx, t in recs]
        if write_obj(os.path.join(
                outdir, "parts", name, "wheel_slot%d.obj" % slot),
                verts, groups, "wheel slot %d, all records" % slot):
            nwheel_mesh += 1

    # 5. wheel placement (+0xB80 matrices [C] + +0x18 radius)
    radius, wheels = read_wheels(data)
    with open(os.path.join(outdir, name + ".wheels"), 'w') as f:
        f.write("# wheel radius (file+0x18) + attach matrices (file+0xB80,\n"
                "# 4x4 stride 0x40, rows Right/Up/At/Pos) [C: FUN_0012FEE0\n"
                "# copies them into the damage ctx the draw path reads]\n")
        f.write("radius %.4f\n" % radius)
        for pos, mirror in wheels:
            f.write("wheel %.4f %.4f %.4f %d\n"
                    % (pos[0], pos[1], pos[2], mirror))
        # physics body extents: file+0xE80 = half extents, file+0xE90 =
        # min-corner/center offset [C: FUN_00122830 copies them to the
        # live vehicle +0x1D0/+0x1E0; the airborne damper stations and the
        # centre-of-mass height (+0x1F4 = (ext.y - center.y)*0.1,
        # FUN_0011A8F0) come from these]
        ext = struct.unpack_from('<4f', data, 0xE80)
        cen = struct.unpack_from('<4f', data, 0xE90)
        f.write("ext %.4f %.4f %.4f %.4f\n" % ext)
        f.write("center %.4f %.4f %.4f %.4f\n" % cen)

    ntris = sum(len(t) for _, _, t in sec['body'])
    return (len(verts), ntris, nb, npanels, nwheel_mesh, len(wheels)), None


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "build/cars"
    partsdir = os.path.join(outdir, "parts")
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(partsdir, exist_ok=True)
    ok = fail = 0
    for cls in sorted(os.listdir(PVEH)):
        d = os.path.join(PVEH, cls)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.lower().endswith('.bgv'):
                continue
            name = "%s_%s" % (cls, os.path.splitext(fn)[0])
            res, err = extract(os.path.join(d, fn), outdir, partsdir, name)
            if err:
                print("FAIL %-24s %s" % (name, err))
                fail += 1
                continue
            nv, nt, nb, np_, nwm, nwp = res
            print("ok   %-24s %5d verts %5d tris  %d panels->%d objs  "
                  "%d wheel meshes  %d wheel pos"
                  % (name, nv, nt, nb, np_, nwm, nwp))
            ok += 1
    print("\n%d extracted, %d failed -> %s" % (ok, fail, outdir))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
