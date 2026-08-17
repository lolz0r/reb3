#!/usr/bin/env python3
"""
Recover Burnout 3's DESTRUCTIBLE TRACK PROPS (traffic cones, barrier boards,
signposts, boxes, benches ...) out of each track's `static.dat`, and emit
`build/tracks/<ID>/props.bin` for the harness.

Nothing here is a per-track constant: every address is read out of the file's
own header, and every semantic below is pinned to a disassembled site in
`build/burnout3.elf` (the corrected ELF; `.text` = old flat VA + 0x10000).

================================================================ WHERE THEY ARE
`tools/extract_track.py` documents `static.dat`'s header +0x34/+0x38 as the
"instanced-prop" table.  That is NOT this.  +0x34/+0x38 is the 0x70-record
BACKDROP-LOD table (bounding boxes up to 96 m -- buildings and tree clusters)
drawn by `FUN_001ADA40`.  The DESTRUCTIBLE props are a SECOND table of the same
0x70 record shape, and they come with their own instance/placement machinery:

    hdr +0x36  u16   MODEL count          (the 0x70-record relocation loop is
    hdr +0x3C  i32   MODEL table           FUN_0019B4E0 @0x0019B634..0x0019B6F7)
    hdr +0x40  u16   INSTANCE count
    hdr +0x44  i32   u8[model_count]      per-model PROP CLASS  (see below)
    hdr +0x48  i32   instance TRANSFORMS  -- 0x40 bytes each, a 4x4 matrix
    hdr +0x4C  i32   ptr[unit_count]      -> u8[model_count] per-unit counts
    hdr +0x50  i32   ptr[unit_count]      -> ptr[model_count] per-unit lists
    hdr +0x54  u16   streamed unit count  (shared with the world loader)

The +0x48 stride is not assumed: `(hdr+0x4C - hdr+0x48) / hdr+0x40` is exactly
64.000 on all 37 shipped `static.dat` files.  The +0x44 length is not assumed
either: `hdr+0x44 + model_count` rounded up to 16 lands exactly on `hdr+0x48` on
every one of them (and `model_count+1` .. does not disambiguate, so the byte
array is confirmed instead by its CONTENT -- see "prop class").

The reader of all of it is the world-object registration loop
`FUN_00110420 @0x001109CB..0x00110A46`  [C-disasm]:

    0x001109D5  MOV  AL,[0x00737686]          ; model_count
    0x001109E8  MOV  EDX,[ECX + unit*4]       ; 0x00737688 = +0x4C table
    0x001109EB  MOV  AL,[EDI + EDX]           ; count[unit][model]
    0x001109FD  MOV  EDX,[EAX + unit*4]       ; 0x0073768C = +0x50 table
    0x00110A00  MOV  EAX,[EDX + model*4]      ; the instance list
    0x00110A03  LEA  ECX,[EAX + j*4]          ; 4 BYTES PER LIST ENTRY
    0x00110A19  MOV  byte [slot],0x5          ; world-slot TYPE 5 = STATIC PROP
    0x00110A1C  MOVZX EDX,word [ECX]          ; entry+0x00 u16 = instance index
    0x00110A1F  SHL  EDX,0x6                  ;   * 0x40
    0x00110A22  ADD  EDX,[0x00737678]         ;   + the +0x48 transform table
    0x00110A28  MOV  [slot+0x04],EDX          ; slot -> live transform
    0x00110A2B  MOVZX ECX,byte [ECX + 0x2]    ; entry+0x02 u8 = the class byte
    0x00110A38  ADD  ECX,[hdr + 0x3C]         ; (see the note below)
    0x00110A3F  MOV  [slot+0x08],ECX          ; slot -> model record
    0x00110A46  CALL FUN_00114270             ; world AABB from model bbox x transform

The list-entry byte at +0x02 is CONSTANT within one (unit, model) list and is
byte-for-byte equal to `hdr+0x44[model]` on all 37 files, so the two are the
same datum.  It is a CLASS, not a model index, and the proof is AS/C2_V1: three
models, `hdr+0x44 = [1, 5, 2]`, and dumping the +0x3C table PAST the third
record shows records 3/4/5 are garbage (material index 50629, a bbox extent of
3e16).  Byte 5 would therefore index off the end of a shipped file, which the
retail loader cannot be doing.  The model an instance draws and collides with
is the LIST it lives in (EDI above), and that is what this tool emits.  Retail
nevertheless scales the class by 0x70 into the +0x3C table at 0x00110A35, so
its slot+0x08 model pointer is out of range on those tracks; recorded and
marked [?], not reproduced.

  PROP CLASS -- the same value across all 37 files, by content:
     class 1 is the CONE on every single track that has one
       (`bk_cone` AS/C1+M1, `R_cone` EU/C1+C3+M2, `WF_Cone` US/C1+P1,
        `Chgo_Cone` US/C2, `Chgo_Cone_02` AS/C3, the 0.37x1.54 GL cone US/C3),
     class 4/5 the roadwork barrier boards and market barriers,
     class 6 the tall signposts / warning markers,
     class 2/3 benches, bins, litter, low walls,
     class 7 the heavy tables, tree trunks, concrete blocks.
  It is the same 0..7 space the object-crash and prop-hit-audio paths use.

============================================================ THE MODEL RECORD
0x70 bytes.  Fixed up by `FUN_0019B4E0 @0x0019B634..0x0019B6F7`, which is what
pins the three mesh blocks and their gating flag:

    +0x00  f32[4]  bbox MAX  (w unused)         [C] FUN_0011A020 @0x0011A0A5:
    +0x10  f32[4]  bbox MIN  (w unused)             copied to body+0x1D0/+0x1E0
    +0x20  mesh block, LOD 0                    always present
    +0x34  mesh block, LOD 1                    present iff (u16 +0x62 & 1)
    +0x48  mesh block, LOD 2                    present iff (u16 +0x62 & 2)
    +0x5C  u16     LOD0 material index (into the header +0x08 material table)
    +0x5E  u16     LOD1 material index      -- +0x5C is the one this tool uses;
    +0x60  u16     LOD2 material index         the three agree on every prop
                                               model in every shipped track
    +0x62  u16     LOD-present flags
    +0x64  f32     LOD/fade NEAR distance
    +0x68  f32     LOD/fade FAR distance
    +0x6C  i32     extra (0 on every prop model seen)

  mesh block (0x14 bytes), relocation transcribed from FUN_0019B4E0:
    +0x00  u32  Xbox D3DResource Common; the relocated data pointer is masked
                with 0x0FFFFFFF unless (Common & 0x70000) == 0x20000
    +0x04  i32  vertex data, RELATIVE to the block
    +0x08  u32  (D3DResource Lock, 0 in the file)
    +0x0C  u32  INDEX count
    +0x10  i32  index data, RELATIVE to the block

  The vertex STRIDE comes from the material's shader class, which is exactly
  the two declarations `extract_track.py` already pins as "the foliage/prop/
  cone families" (`src/burnout3_trackmesh.h:207`):

      class 8  0x003875E8: stream0 v0=FLOAT3           v9=FLOAT2  = 20 bytes
      class 9  0x003875D4: stream0 v0=FLOAT3 v2=NPK3   v9=FLOAT2  = 24 bytes

  i.e. `f32 pos[3], [u32 D3DVSDT_NORMPACKED3 normal,] f32 uv[2]`, no D3DCOLOR
  either way.  That is not an assumption: over all 436 prop models in all 37
  shipped tracks, `(index_ptr - vertex_ptr)` is an exact multiple of the
  class-derived stride AND the resulting vertex count exceeds the largest index
  in the strip, in every single case, which neither stride achieves alone
  (US/C3_V1 model 1 is class 8 and needs 20; US/C1_V1 model 3 is class 9 and
  needs 24).  Indices are u16 and form ONE triangle strip (degenerate doubles
  included); this tool de-strips them into a triangle list.

============================================================== THE PHYSICS
`FUN_0011A020` is the prop rigid-body constructor (reached from the car-vs-prop
contact handler `FUN_00113890 @0x0011393B -> FUN_00114730 -> FUN_0011A020`).
It reads the model record's two bbox rows and writes [C]:

    body+0x1CC  radius = |bbox_max|                       SQRTSS @0x0011A0DE
    body+0x1D0/+0x1E0  bbox max / min                     @0x0011A0A5/0x0011A0AF
    body+0x1F0  MASS   = max(100.0, (max.z - min.z) * (max.x - min.x) * 200.0)
                @0x0011A137..0x0011A191; the constants are the image floats
                [0x003A2928] = 100.0 and [0x003A292C] = 200.0
    body+0x224  despawn clock = DAT_0060EA20 + [0x003A7F34] (= 10.0)
                @0x0011A19E..0x0011A1B4

so a prop's mass is its FOOTPRINT AREA x 200, floored at 100.  Every cone in
the game lands on the floor (a 0.59 m cone is 0.35 m^2 -> 70 -> 100); the big
US/C1_V1 wooden box is 1.65 x 1.65 -> 545.  Both numbers are emitted per model.

================================================================== props.bin
Little-endian.  Positions/normals/transforms are in RAW GAME SPACE -- the
Z-negation to harness/GL space is the loader's job, exactly as
`tools/extract_track.py` and `src/burnout3_track_paths.h` do it (RE_NOTES 12).

    +0x00  char[4] 'B3PP'
    +0x04  u32 version = 1
    +0x08  u32 model_count
    +0x0C  u32 instance_count
    +0x10  u32 vertex_count           (all models concatenated)
    +0x14  u32 index_count            (all models concatenated, triangle LIST)
    +0x18  u32 off_models
    +0x1C  u32 off_instances
    +0x20  u32 off_vertices
    +0x24  u32 off_indices
    +0x28  u32 unit_count
    +0x2C  u32 reserved

    model record, 0x60 bytes, at off_models + i*0x60
      +0x00  f32 bb_min[3]
      +0x0C  f32 bb_max[3]
      +0x18  u32 first_vertex        (in vertices)
      +0x1C  u32 vertex_count
      +0x20  u32 first_index         (in u16s)
      +0x24  u32 index_count         (triangle list, multiple of 3; the values
                                      are MODEL-LOCAL -- add first_vertex)
      +0x28  u32 prop_class          (hdr+0x44, 0..7; 1 == cone)
      +0x2C  f32 mass                (FUN_0011A020's law)
      +0x30  f32 radius              (|bb_max|)
      +0x34  f32 lod_near
      +0x38  f32 lod_far
      +0x3C  u32 material_flags      (static.dat material +0x24)
      +0x40  char[32] texture basename, NUL padded

    instance record, 0x50 bytes, at off_instances + i*0x50
      +0x00  f32 m[16]   row0 right, row1 up, row2 at, row3 position
                         (rows 0..2 carry a per-instance tint in .w; row3.w = 0)
      +0x40  u32 model
      +0x44  u32 prop_class
      +0x48  u32 unit                (streamed unit that owns it)
      +0x4C  u32 flags               (list entry byte +0x03; 0 on every track)

    vertex, 0x20 bytes: f32 pos[3], f32 normal[3], f32 uv[2]
    index : u16
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_track as et                                   # noqa: E402
import extract_tlist as tl                                   # noqa: E402

MAGIC = b'B3PP'
VERSION = 1
VSTRIDE_C9 = 0x18       # shader class 9: pos + NORMPACKED3 + uv
VSTRIDE_C8 = 0x14       # shader class 8: pos + uv (no normal)
MODEL_REC = 0x60
INST_REC = 0x50


def u16(d, o):
    return struct.unpack_from('<H', d, o)[0]


def i16(d, o):
    return struct.unpack_from('<h', d, o)[0]


def i32(d, o):
    return struct.unpack_from('<i', d, o)[0]


def f32(d, o):
    return struct.unpack_from('<f', d, o)[0]


def unpack_normal(v):
    """D3DVSDT_NORMPACKED3: x = bits 0..10 /1023, y = 11..21 /1023, z = 22..31
    /511, all signed.  Same decode extract_track.py pins for the world."""
    x = v & 0x7FF
    y = (v >> 11) & 0x7FF
    z = (v >> 22) & 0x3FF
    if x > 1023:
        x -= 2048
    if y > 1023:
        y -= 2048
    if z > 511:
        z -= 1024
    return x / 1023.0, y / 1023.0, z / 511.0


def mesh_block(d, base):
    """(vertex_off, index_off, index_count) for the 0x14-byte block at `base`,
    relocated the way FUN_0019B4E0 does it."""
    common = struct.unpack_from('<I', d, base)[0]
    vrel = i32(d, base + 0x04)
    ncount = struct.unpack_from('<I', d, base + 0x0C)[0]
    irel = i32(d, base + 0x10)
    voff = vrel + base
    if (common & 0x70000) != 0x20000:
        voff &= 0x0FFFFFFF
    ioff = irel + base
    return voff, ioff, ncount


def destrip(d, ioff, n):
    """One triangle strip of u16 indices -> a triangle list, degenerates out."""
    out = []
    if n < 3:
        return out
    idx = [u16(d, ioff + k * 2) for k in range(n)]
    for k in range(n - 2):
        a, b, c = idx[k], idx[k + 1], idx[k + 2]
        if a == b or b == c or a == c:
            continue
        # keep a consistent winding across the strip
        if k & 1:
            out += [a, c, b]
        else:
            out += [a, b, c]
    return out


class PropTrack(object):
    """Everything static.dat says about one track's destructible props."""

    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        d = self.data
        self.reader = et.Reader(d)
        self.materials = et.parse_materials(self.reader, d)
        self.n_models = u16(d, 0x36)
        self.t_models = i32(d, 0x3C)
        self.n_inst = u16(d, 0x40)
        self.t_class = i32(d, 0x44)
        self.t_xform = i32(d, 0x48)
        self.t_counts = i32(d, 0x4C)
        self.t_lists = i32(d, 0x50)
        self.n_units = u16(d, 0x54)
        self.classes = list(d[self.t_class:self.t_class + self.n_models])
        self.notes = []
        self._check()

    def _check(self):
        """Every structural claim re-derived from this file, not assumed."""
        d = self.data
        if self.n_models == 0 or self.n_inst == 0:
            return
        stride = (self.t_counts - self.t_xform) / float(self.n_inst)
        if abs(stride - 0x40) > 1e-6:
            self.notes.append('transform stride %.3f != 64' % stride)
        end = self.t_class + self.n_models
        if not (self.t_xform - 16 < end <= self.t_xform):
            self.notes.append('class array does not abut the transform table')

    def models(self):
        d = self.data
        out = []
        for i in range(self.n_models):
            r = self.t_models + i * 0x70
            bmax = [f32(d, r + k * 4) for k in range(3)]
            bmin = [f32(d, r + 0x10 + k * 4) for k in range(3)]
            voff, ioff, n = mesh_block(d, r + 0x20)
            mat = u16(d, r + 0x5C)
            m = self.materials.get(mat)
            has_n = (m.cls if m else 9) != 8
            stride = VSTRIDE_C9 if has_n else VSTRIDE_C8
            span = ioff - voff
            nv = span // stride
            if span < 0 or nv <= 0 or n <= 0 or span % stride:
                self.notes.append('model %d: LOD0 span %d not a multiple of the '
                                  'class-%s stride %d'
                                  % (i, span, m.cls if m else '?', stride))
                nv, n = max(nv, 0), n
            verts = []
            for k in range(nv):
                o = voff + k * stride
                px, py, pz = struct.unpack_from('<3f', d, o)
                if has_n:
                    nx, ny, nz = unpack_normal(
                        struct.unpack_from('<I', d, o + 12)[0])
                    u, v = struct.unpack_from('<2f', d, o + 16)
                else:
                    nx, ny, nz = 0.0, 1.0, 0.0
                    u, v = struct.unpack_from('<2f', d, o + 12)
                verts.append((px, py, pz, nx, ny, nz, u, v))
            tris = destrip(d, ioff, n) if n else []
            # FUN_0011A020 @0x0011A137..0x0011A191 [C]
            mass = (bmax[2] - bmin[2]) * (bmax[0] - bmin[0]) * 200.0
            if mass <= 100.0:
                mass = 100.0
            radius = (bmax[0] ** 2 + bmax[1] ** 2 + bmax[2] ** 2) ** 0.5
            out.append(dict(
                index=i, bb_min=bmin, bb_max=bmax, verts=verts, tris=tris,
                cls=self.classes[i] if i < len(self.classes) else 0,
                mass=mass, radius=radius,
                lod_near=f32(d, r + 0x64), lod_far=f32(d, r + 0x68),
                mat=mat, mat_flags=(m.flags if m else 0),
                texture=(m.texture if m else ''),
            ))
        return out

    def instances(self):
        """Walk the per-unit lists -- the only unambiguous model<->instance
        binding in the file (FUN_00110420 @0x001109E8..0x00110A03)."""
        d = self.data
        out = []
        seen = set()
        for unit in range(self.n_units):
            cbase = i32(d, self.t_counts + unit * 4)
            lbase = i32(d, self.t_lists + unit * 4)
            if cbase <= 0 or lbase <= 0:
                continue
            counts = d[cbase:cbase + self.n_models]
            for m in range(self.n_models):
                n = counts[m] if m < len(counts) else 0
                if not n:
                    continue
                s = i32(d, lbase + m * 4)
                if s <= 0:
                    self.notes.append('unit %d model %d: %d entries, null list'
                                      % (unit, m, n))
                    continue
                for j in range(n):
                    idx, cls, flags = struct.unpack_from('<HBB', d, s + j * 4)
                    if idx >= self.n_inst:
                        self.notes.append('instance index %d out of range' % idx)
                        continue
                    if idx in seen:
                        self.notes.append('instance %d listed twice' % idx)
                    seen.add(idx)
                    mm = struct.unpack_from('<16f', d, self.t_xform + idx * 0x40)
                    out.append(dict(m=mm, model=m, cls=cls, unit=unit,
                                    flags=flags, index=idx))
        if len(seen) != self.n_inst:
            self.notes.append('%d of %d instances reached by the unit lists'
                              % (len(seen), self.n_inst))
        return out


def build(track):
    models = track.models()
    insts = track.instances()

    vblob, iblob = [], []
    for m in models:
        m['first_vertex'] = len(vblob)
        m['first_index'] = len(iblob)
        vblob += m['verts']
        # Index values are MODEL-LOCAL: the reader adds the model's
        # first_vertex.  (They were global once; the loader added first_vertex
        # on top and every model past the first drew another model's
        # vertices -- a barrier board came out as a heap of shards.)
        iblob += m['tris']
        m['n_vertex'] = len(m['verts'])
        m['n_index'] = len(m['tris'])
        if m['tris'] and max(m['tris']) >= m['n_vertex']:
            track.notes.append('model %d index out of range' % m['index'])

    off_models = 0x30
    off_inst = off_models + len(models) * MODEL_REC
    off_vtx = off_inst + len(insts) * INST_REC
    off_idx = off_vtx + len(vblob) * 0x20

    out = bytearray()
    out += MAGIC
    out += struct.pack('<10I', VERSION, len(models), len(insts), len(vblob),
                       len(iblob), off_models, off_inst, off_vtx, off_idx,
                       track.n_units)
    out += b'\0' * (off_models - len(out))
    for m in models:
        rec = struct.pack('<3f3f4I I f f f f I',
                          m['bb_min'][0], m['bb_min'][1], m['bb_min'][2],
                          m['bb_max'][0], m['bb_max'][1], m['bb_max'][2],
                          m['first_vertex'], m['n_vertex'],
                          m['first_index'], m['n_index'],
                          m['cls'], m['mass'], m['radius'],
                          m['lod_near'], m['lod_far'], m['mat_flags'])
        name = os.path.basename(m['texture'].replace('\\', '/'))[:31]
        rec += name.encode('ascii', 'replace') + b'\0' * (32 - len(name))
        assert len(rec) == MODEL_REC, len(rec)
        out += rec
    for it in insts:
        out += struct.pack('<16f4I', *it['m'], it['model'], it['cls'],
                           it['unit'], it['flags'])
    for v in vblob:
        out += struct.pack('<8f', *v)
    for i in iblob:
        out += struct.pack('<H', i)
    return bytes(out), models, insts


CONE_CLASS = 1          # the recovered constant: class 1 is the cone


def report(tid, track, models, insts, size):
    from collections import Counter
    per = Counter(i['model'] for i in insts)
    print('%-9s %3d models  %4d instances  %4d units  props.bin %7d B'
          % (tid, len(models), len(insts), track.n_units, size))
    for m in models:
        tag = '  <-- CONE' if m['cls'] == CONE_CLASS else ''
        print('    m%-2d cls=%d %-20s %6.1f kg  %5.2f x %5.2f x %5.2f  '
              '%4d v %4d tri  lod %.0f/%.0f%s'
              % (m['index'], m['cls'], m['texture'][:20], m['mass'],
                 m['bb_max'][0] - m['bb_min'][0],
                 m['bb_max'][1] - m['bb_min'][1],
                 m['bb_max'][2] - m['bb_min'][2],
                 m['n_vertex'], m['n_index'] // 3,
                 m['lod_near'], m['lod_far'], tag))
        print('        %d placed' % per.get(m['index'], 0))
    for n in track.notes:
        print('    NOTE: %s' % n)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    want = args[0] if args else os.environ.get('B3_TRACK')
    all_tracks = '--all' in sys.argv[1:]

    tracks = tl.load_tracks() if hasattr(tl, 'load_tracks') else None
    ids = []
    if tracks:
        ids = [(t.id, t.dir) for t in tracks]
    else:
        root = tl.TRACKS_DIR
        for reg in sorted(os.listdir(root)):
            rd = os.path.join(root, reg)
            if not os.path.isdir(rd):
                continue
            for sub in sorted(os.listdir(rd)):
                p = os.path.join(rd, sub)
                if os.path.isfile(os.path.join(p, 'static.dat')):
                    ids.append(('%s_%s' % (reg, sub), p))

    if want and not all_tracks:
        ids = [x for x in ids if x[0] == want]
        if not ids:
            print('no such track: %s' % want)
            return 1
    elif not all_tracks and not want:
        ids = [x for x in ids if x[0] == 'US_C3_V1'] or ids[:1]

    total = 0
    for tid, tdir in ids:
        sd = os.path.join(tdir, 'static.dat')
        if not os.path.exists(sd):
            continue
        track = PropTrack(sd)
        blob, models, insts = build(track)
        out_dir = os.path.join('build', 'tracks', tid)
        os.makedirs(out_dir, exist_ok=True)
        out = os.path.join(out_dir, 'props.bin')
        with open(out, 'wb') as f:
            f.write(blob)
        report(tid, track, models, insts, len(blob))
        total += len(insts)
    print('-- %d tracks, %d prop instances' % (len(ids), total))
    return 0


if __name__ == '__main__':
    sys.exit(main())
