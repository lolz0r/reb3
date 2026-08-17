# Extracting Burnout 3 (Xbox) car geometry from `.bgv` files

This document explains, step by step, how to extract the car body meshes from
the Xbox game `Burnout 3: Takedown` `.bgv` files, and how that format was
cracked (the game's own relinker is the ground truth — no guessing).

The working extractor is `tools/extract_bgv.py` in this repo. Run it:

```bash
python3 tools/extract_bgv.py build/cars
```

It writes every player vehicle class to `build/cars/<CLASS>_<CarN>.obj`
(max-detail LOD, with `v` / `vt` / `vn` and `f v/vt/vn` faces).

---

## 1. Where the files live

The extracted game dump has a `pveh/` directory with one sub-folder per car
class (`COMP`, `CUPE`, `HEVY`, `HSPC`, `MSCL`, `SPRT`, `SUPR`, `TSPC`), each
holding a set of `CarN.bgv` files. There are 67 player `.bgv` files.

Example:

```
pveh/COMP/Car1.bgv        <- body mesh (player car)
pveh/COMP/Car1.hwd        <- alternate high-detail mesh (not textures)
pveh/COMP/Car1.lwd        <- alternate low-detail mesh (not textures)
```

`.hwd` / `.lwd` files are alternate LOD meshes ("high"/"low" magic at `+0x5C`);
the `.bgv` already contains the geometry you want.

---

## 2. The one key fact: `.bgv` is a pointer-relocated container

`.bgv` is **not** a static vertex/index blob. It is a container whose internal
pointers are stored as **offsets relative to the start of the buffer**. At load
time the game adds the runtime buffer address to a fixed set of fields — that's
the "relocation pass". Because a file's first byte is at offset 0 == base,
you can treat the file **in place** and walk the exact same structure the game
builds in memory.

The function that does this relocation in the executable is
**`FUN_000310f0`** (the `.bgv`/`.btv` loader completions `FUN_0018d0e0` and
`FUN_001a4260` call it). It tells us precisely which fields are pointers and
their strides. `tools/replicate_bgv_relinker.py` transcribes it; the field
offsets in this document come straight from that code, not from inference.

---

## 3. Header

Little-endian.

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | u32 | version/magic `0x17` |
| `+0x08` | u32 | file size |
| `+0x0C` | u32 | version2 `0x406` |
| `+0x10` | u32 | `3` |
| `+0x4C` | u32 | LOD section table 1 offset |
| `+0x50` | u32 | LOD section table 2 offset |
| `+0x54` | u32 | LOD section table 3 offset |
| `+0x58` | u32 | LOD section table 4 offset |
| `+0x60` | u32 | material/texture directory offset |

In `COMP/Car1.bgv` the four section-table offsets are:
`0x16F0`, `0x2764C`, `0x35CFC`, `0x5229C`. These are **four LOD levels of the
same car body** — the one with the most geometry is the highest detail.

---

## 4. LOD section table

Let `S` be a section-table offset from the header.

| Offset | Type | Meaning |
|---|---|---|
| `+0x4C` | u32 | vertex pool offset, **relative to S** |
| `+0x60` | u8  | number of part records |
| `+0x64` | u32 | part-array offset, **relative to S + 0x60** |

The vertex pool is at `S + (u32 at S+0x4C)`.
The part array is at `S + 0x60 + (u32 at S+0x64)`.

---

## 5. Part records

There are `(u8 at S+0x60)` part records, each `0x1C` bytes, starting at the
part array address.

| Offset | Type | Meaning |
|---|---|---|
| `+0x0C` | u32 | index-stream offset, **relative to the part record** |
| `+0x10` | u32 | index-stream size in bytes (→ u16 count = size / 2) |
| `+0x18` | u32 | part tag (1, 2, 4, 8, … , `0x10002`, `0x20100`) |

Each part has its own index stream. The index stream is a **u16 triangle strip**.

---

## 6. Vertex records

The vertex pool at `S + (u32 at S+0x4C)` is an array of records, stride `0x18`
(24 bytes). The number of vertices is derived: it's `max_index + 1`, where
`max_index` is the largest index referenced by any of the section's parts.

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | f32[3] | position (X, Y, Z) |
| `+0x0C` | u8[4] | packed normal / per-vertex data |
| `+0x10` | f32[2] | UV (U, V) — cleanly in [0,1] |

The normal is stored as signed bytes; divide by 127 and normalize to recover a
unit vector. The 4th byte is padding/per-vertex flag.

---

## 7. Triangulation

For each part, read its `n = size/2` u16 values as a triangle strip. Emit one
triangle per consecutive index triple, skipping degenerate triples
(`a==b || b==c || a==c`), and **flip the winding of every other triangle** to
undo the strip alternation:

```
for i in 0 .. len(idx)-3:
    a, b, c = idx[i], idx[i+1], idx[i+2]
    if a==b or b==c or a==c: continue
    if i is even: emit (a, b, c)
    else:         emit (a, c, b)
```

---

## 8. Which LOD to emit

Each car file has 4 LOD sections. For a single export you only need the
highest-detail one: pick the section with the most triangles (its parts index
the widest vertex range).

---

## 9. Why this is trusted (verification, not vibes)

- **67/67** player `.bgv` files produce car-shaped bounds (e.g. COMP_Car1
  highest LOD ≈ 2.5 m wide, 1.7 m tall, 4 m long).
- **0** out-of-range index references across every LOD of every car.
- Vertex stride confirmed by edge-length coherence: at stride `0x18` the median
  index-connected edge is 0.158 m and 92% of edges are < 1 m — an order of
  magnitude tighter than any nearby stride (0x14/0x1C/0x20 median ≈ 0.9–1.1 m).

---

## 10. What about textures / materials?

The vertex data carries UVs and normals (exported as `vt`/`vn`), so UV mapping
is genuine. The actual paint/livery raster is a separate Criterion `compact1`
palette-indexed block — **not** the track DXT1/DXT5 path. It is referenced from
the material directory at `+0x60` → `0x1C80` (format 0x0B, 512×256, name
"compact1"). Decoding that raster is not yet implemented; it is the natural
next step (emulate the game's texture decoder under Unicorn).

---

## 11. Tools in this repo

| Tool | Purpose |
|---|---|
| `tools/extract_bgv.py` | working extractor → OBJ (positions, UVs, normals) |
| `tools/replicate_bgv_relinker.py` | transcribes the game's relinker `FUN_000310f0` |
| `tools/debug_bgv_structure.py` | prints one file's full relinked structure |

The Ghidra analysis used `build/burnout3.elf` (a correctly-mapped ELF of
`default.xbe`, entry `0x001D2807`) via the ghidra-mcp bridge.