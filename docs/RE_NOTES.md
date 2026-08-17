# Burnout 3: Takedown — reverse engineering notes

Target: `default.xbe`, Burnout 3: Takedown (USA), Criterion Games / EA, 2004.
Everything below is **verified against the binary or the data files**. Anything
not verified is marked as such. Nothing here is inferred from the game's
behaviour or from prior assumptions.

---

## 1. Executable

| Property | Value | How verified |
|---|---|---|
| Format | XBE (original Xbox), magic `XBEH` | header bytes |
| File size | 4,308,992 (`0x41C000`) | stat |
| Image base | `0x00010000` | header `+0x104` |
| Image size | `0x00768200` (7,766,528) | header `+0x10C` |
| Entry point | `0x001D2807` | `+0x128` XOR retail key `0xA8FC57AB`, lands in-image |
| Kernel thunk table | `0x0036B7C0` | `+0x158` XOR retail key `0x5B6D40B6` |
| Title | `Burnout 3`, title ID `0x4541005B` (`EA`) | certificate |
| Build timestamp | `0x4113B1C0` (Aug 2004) | certificate |
| Sections | 17 | `+0x11C` |
| Engine | RenderWare **RW36** | `//RenderWare/RW36Active/rwsdk/...` `$Id` strings |
| Original build path | `C:\work\B3\CG1Code\GameProjects\VisualStudio.NET\Burnout3\XboxExternal\Burnout3_External.exe` | embedded string |

### Section map

| # | Name | VA | VSize | Raw off | Raw size |
|---|---|---|---|---|---|
| 0 | `.text` | `0x00011000` | `0x2BB200` | `0x001000` | `0x2BB200` |
| 1 | `XMV` | `0x002CC200` | `0x027D34` | `0x2BD000` | `0x027D24` |
| 2 | `DSOUND` | `0x002F3F40` | `0x00CDBC` | `0x2E5000` | `0x00CB54` |
| 3 | `WMADEC` | `0x00300D00` | `0x019D64` | `0x2F2000` | `0x019D64` |
| 4 | `XONLINE` | `0x0031AA80` | `0x01E75C` | `0x30C000` | `0x01E75C` |
| 5 | `XNET` | `0x003391E0` | `0x0130E8` | `0x32B000` | `0x0130E8` |
| 6 | `D3D` | `0x0034C2E0` | `0x014774` | `0x33F000` | `0x010EA4` |
| 7 | `XGRPH` | `0x00360A60` | `0x00206C` | `0x350000` | `0x00206C` |
| 8 | `XPP` | `0x00362AE0` | `0x008CD4` | `0x353000` | `0x008CD4` |
| 9 | `.rdata` | `0x0036B7C0` | `0x046B94` | `0x35C000` | `0x046B94` |
| 10 | `.data` | `0x003B2360` | `0x3B95DC` | `0x3A3000` | `0x06844C` |
| 11 | `DOLBY` | `0x0076B940` | `0x007180` | `0x40C000` | `0x00716C` |
| 12 | `XON_RD` | `0x00772AC0` | `0x001528` | `0x414000` | `0x001528` |
| 13 | `.data1` | `0x00774000` | `0x0000E0` | `0x416000` | `0x0000B0` |
| 14 | `$$XTIMAGE` | `0x007740E0` | `0x002800` | `0x417000` | `0x002800` |
| 15 | `$$XSIMAGE` | `0x007768E0` | `0x001000` | `0x41A000` | `0x001000` |
| 16 | `.XTLID` | `0x007778E0` | `0x000920` | `0x41B000` | `0x000920` |

`.data` is mostly BSS: `0x3B95DC` virtual vs `0x06844C` on disk, so
`0x351190` bytes are zero-init. The vehicle entity array (below) lives there.

---

## 2. Loading it correctly

Ghidra has no XBE loader. Loading `default.xbe` as a **flat raw binary** — which
is what the first pass did — is silently wrong, because each section has a
different `VA - raw_offset` delta:

```
.text   delta 0x10000      .rdata  delta 0xF7C0      .data  delta 0xF360
XMV     delta 0xF200       D3D     delta 0xD2E0      ...
```

Code inside `.text` still disassembles (uniform shift) and intra-`.text` calls
still resolve, because they are relative. Everything else breaks:

- **Every absolute data reference reads the wrong bytes.** Measured examples:

  | Reference | True value | Raw-load value |
  |---|---|---|
  | `0x003A2D50` | `2.5` | `0.0` |
  | `0x00384A80` | `0.15` | `22.0` |
  | `0x003B16F0` | `1.6` | `0.0` |
  | `0x003A1238` | `90.0` | the string `"Score/Burnout Points"` |

- Cross-section calls resolve to wrong targets (differing deltas).
- The entry point is missed entirely; Ghidra puts `entry` at `0x0`, which is the
  `XBEH` magic. Five "functions" get created inside the RSA header signature.
- BSS is absent, so `.data` globals above `0x41C000` are unmapped.

### The fix: `tools/xbe2elf.py`

Rebuilds the real VA space as an ELF32/x86 executable — one `PT_LOAD` per XBE
section at its true VA, `p_memsz > p_filesz` to materialise BSS, and
`e_entry = 0x001D2807`. Ghidra's stock ELF loader then does the right thing.

```bash
python3 tools/xbe2elf.py <path to default.xbe> build/burnout3.elf
```

Import it with the **ELF loader** (auto-detect). Do *not* pass an explicit
language ID — that forces Ghidra's Binary loader and silently reproduces the
flat-at-zero bug.

### Result

| | Raw-binary load | Corrected ELF load |
|---|---|---|
| Functions | 7,267 | 7,434 |
| Entry point | `0x0` (bogus) | `0x1D2807` (real, decompiles as CRT startup) |
| Functions in header/`.data` garbage | 7 | 0 |
| Float constants | wrong | correct (verified, table above) |
| String/data xrefs | meaningless | resolve correctly |

To translate an address from the old (raw) database to the new one:
`new_VA = old_addr + 0x10000` — **valid only for `.text`** (old `0x1000`–`0x2BC200`).

---

## 3. Xbox kernel imports

147 imports, thunk table at `0x0036B7C0`, terminated by a zero DWORD. Each entry
is `0x80000000 | ordinal`.

Decoding verified independently: the thunk at `0x36B7C0` holds `0x80000150`, and
`FUN_001D7012` is `jmp [0x80000150]` — `0x150` = 336, matching the table entry.

Ordinal→name mapping is applied by `tools/xboxkrnl_ordinals.py` from the
published `xboxkrnl.exe` export table. Ordinals outside the documented 1–340
range are labelled `xboxkrnl_ord_NNN` rather than guessed.

Labels are applied in the Ghidra DB with a `krnl_` prefix.

Notable imports present: `NtCreateFile`, `NtOpenFile`, `NtReadFile`,
`NtQueryVolumeInformationFile`, `ExQueryNonVolatileSetting` (region/language),
`XboxHDKey` / `XboxSignatureKey` (save signing), `RtlSprintf` family.

---

## 4. Asset loaders located

Found by xref from path strings in `.rdata` — only possible once data references
resolve correctly.

| String | VA | Loader |
|---|---|---|
| `static.dat` | `0x003B1098` | `FUN_0019AE10` |
| `streamed.dat` | `0x003B10F0` | — |
| `enviro.dat` | `0x003B0444` | `FUN_001888F0` |
| `Gamedata.bgd` | `0x003AB414` | `FUN_0005F7B0`, `FUN_0005FD30` (17 refs) |
| `.bgv` | `0x003B04FC` | `FUN_0018D0E0` |
| `.btv` | `0x003B1100` | — |
| `pveh/vlist.bin` | `0x003AA390` | `FUN_00015F10` |
| `Tracks/tlist.bin` | `0x003AA37C` | — |
| `Data/Global.txd` | `0x003AA3A0` | — |
| `Data/Frontend.txd` | `0x003A840C` | — |

---

## 5. Data formats

### `pveh/vlist.bin` — vehicle roster (verified)

4096 bytes of `u32`.

| Offset | Value | Meaning |
|---|---|---|
| `+0x00` | `6` | version/kind (unconfirmed) |
| `+0x04` | `107` | **vehicle count** |

Cross-check: on disk there are 67 player vehicles (`.bgv`) and 40 traffic
vehicles (`.btv`), totalling exactly 107.

| Class | `.bgv` | `.btv` |
|---|---|---|
| COMP (compact) | 10 | 9 |
| CUPE (coupe) | 10 | 0 |
| HEVY (heavy) | 11 | 25 |
| HSPC | 6 | 0 |
| MSCL (muscle) | 10 | 0 |
| SPRT (sports) | 10 | 0 |
| SUPR (super) | 10 | 0 |
| TSPC | 0 | 6 |
| **total** | **67** | **40** |

### External references (community work)

Two community projects, supplied by the user, resolve several open questions and
correct one of my claims:

* **`EdnessP/scripts` `fmt_Burnout3LRD.py`** — Noesis plugin, Burnout Modding
  community (burnout.wiki).
* **`Sokka06/burnout-data-tool`** — C# reader/writer for VDB, VList, BGV/BTV data.

Confirmed by the Noesis plugin, cross-checked against our files:

| Field | Finding |
|---|---|
| `.bgv` `+0x00` | **version**, valid range `0x14`..`0x25` (ours is `0x17`) — *not* a magic number, as previously recorded |
| `.bgv` `+0x04` | size, or `0`; ours is 0 with the real size at `+0x08` (matches our finding) |
| `.bgv` `+0x60` | **texture data offset** — in `Car1.bgv` that is `0x1C80` |
| `static.dat` `+0x04` | file size (`0x006F8000`, exact match) |
| `static.dat` `+0x16`/`+0x18` | texture count / texture pointer array, for version > `0x25` |

**Correction:** `0x1C80` in `Car1.bgv` is **texture data**, not NV2A push-buffers.
The earlier push-buffer reading was wrong — those bytes are Xbox texture headers.

**Vehicle geometry is unsolved by the community as well.** `boArcMdlBxv` parses
only textures and then calls `boSetDummyMdl`; the plugin's own TODO lists "All
vehicle model support". So the `.bgv` mesh format is genuinely open, not merely
un-found here. **Track models are supported**, which makes `static.dat` the
realistic route to real 3D geometry. Verified: our `static.dat` declares
**180 textures** with a pointer array at `0x60`, every pointer in range.

### VDB (ValueDB) binary format — and independent corroboration

From `burnout-data-tool`, header is five int32:

| Offset | Field |
|---|---|
| `0x00` | Type |
| `0x04` | DefaultValueCount |
| `0x08` | Unk1 (count of vectors) |
| `0x0C` | FileDefCount |
| `0x10` | FileDefOffset |

Values follow at `0x14` as **8-byte records: `float value` + `uint32 key`**, where
the key is a **hashed parameter name with the high bit set** (`0x80xxxxxx`). That
explains why no parameter names appear in any shipped data file — the names exist
only in the executable, which is where we recovered them.

Parsing the reference `VDB_ps2_bo3_release.XML` (binary despite the extension):
Type 2, 7,525 values, FileDefOffset `0x10918`, all self-consistent.

**14 of 16 spot-checked defaults we recovered from `FUN_00132950` appear as exact
float values in that independent VDB** — mass/idle 1000, torque 630, max rpm
6800, peak torque revs 5000, falloff 6500, slide max 0.85, slide min 0.75, drag
1.9, max drift angle 45, spring force 60000, steer response 0.158, brake force
height −0.55, turn momentum 800/1100. This is third-party confirmation that the
parameter extraction is correct.

**Next step for per-vehicle values:** recover the name→hash function used by the
registrar so the VDB can be keyed by name. That yields real per-car physics
rather than the compiled-in defaults.

### Xbox track geometry — DECODED and rendering

`tools/extract_track.py` is an independent Python reimplementation of the Xbox
track path, and it works: **`Tracks/AS/C1_V1/static.dat` yields 14,551 vertices
and 9,847 triangles across 67 models, with 0 models skipped.** Bounds
X[-857, 2384] Y[-15, 233] Z[-1474, 2281] — height a small fraction of the
horizontal extents, as a track should be. Exported OBJ validates: no degenerate
faces, no out-of-range indices.

Layout, all little-endian:

```
header
  +0x00  u32    version (0x27; <=0x25 is the demo/prealpha layout)
  +0x04  u32    file size
  +0x1C  u16[4] group counts: backdrop, chevron, water, reflection
  +0x24  i32[4] group table offsets (absolute)

group entry (8 bytes)
  +0x00  u16    submodel count
  +0x04  i32    model offset, RELATIVE to this entry

submodel (stride 0x60 -- 0x40 bytes of bounds floats, then 0x20 of info)
  +0x40  u32    must be 1
  +0x44  i32    vertex offset, relative to +0x40
  +0x4C  i32    submesh offset, relative to +0x40
  +0x50  u32    submesh count

submesh entry (stride 0x90)
  +0x80  u32    tri format: 6 = strip, 2 = line
  +0x84  u32    index count
  +0x88  i32    index offset, relative to this entry

vertex (stride 0x1C)
  +0x00  f32[3] position
  +0x10  u8[4]  colour BGRA
  +0x14  f32[2] uv
```

Vertex count is derived from the span between `vtxOffset` and the first
submesh's index block. Indices are u16 triangle strips.

`src/burnout3_trackmesh.c` loads the exported OBJ and the harness renders it,
so **the running program now draws real Burnout 3 track geometry** rather than
the parametric placeholder. The placeholder circuit remains as a fallback when
`build/track.obj` is absent.

Format credit: the Burnout Modding community (burnout.wiki), via EdnessP's
Noesis plugin.

### Xbox textures — DECODED

`tools/extract_textures.py` decodes the texture table and writes PNGs.
**All 180 textures in `Tracks/AS/C1_V1/static.dat` extract successfully**
(96 DXT1, 84 DXT5 — standard S3TC, no Xbox swizzle on these).

Texture record, little-endian:

```
+0x04  i32  bitmap data offset, RELATIVE to the record
+0x34  u32  format: 0xB paletted, 0xC DXT1, 0xE DXT3, 0xF DXT5, 0x3A RGBA
+0x38  u32  width
+0x3C  u32  height
+0x40  u32  bit depth -- if 4/8/32 this is the old revision (name at +0x48),
            otherwise the newer revision (name at +0x44, depth at +0x64)
```

Names come out intact and are recognisably production assets:
`bk_conc5`, `bk_marketbuilding1b`, `bk_paving_central1`, `bk_skytrainconc`,
`bk_tollbooth2`, `bk_marketshopsigns1`, `bk_freewaysigns3`, `KS_stall`,
`bk_fruitbox`. The signage is in Thai ("Temple Drive", "Tourist Information",
"Province Road") — this is the **Bangkok** track, consistent with `Tracks/AS/`
being the Asia region.

**Implementation note:** DXT5 alpha interpolation weights are `(6-i)` over 7 and
`(4-i)` over 5. Using `(7-i)`/`(5-i)` produces values above 255 and throws.

Still to do for textured rendering: the per-submesh material index
(`boMdlTrackGetMatIdx` in the Noesis plugin) maps submeshes to materials, and
materials to textures via the material table at `+0x08`. `tools/extract_track.py`
does not yet emit material assignments, so the mesh renders flat-shaded.

### `.bgv` geometry — what it is NOT (negative results)

Recorded so these dead ends are not re-walked. **The vertex format is still
unknown.**

* **`0x1C80` is texture data** (per the Noesis plugin), so the "NV2A push-buffer"
  reading recorded earlier was wrong.
* **Not plain float3.** Scanning all 576 KB of `Car1.bgv` for runs of
  vehicle-scale float triples (|v| < 6 m) yields exactly one 38-triple region.
  A float-vertex parser will not work.
* **Header offsets `0x16F0 / 0x2764C / 0x35CFC / 0x5229C` are sub-offset
  tables**, not geometry. Read as int16 they look highly "coherent" — because
  monotonically ascending tables are maximally coherent. Any mesh-detection
  heuristic based on smoothness alone will lock onto these. Require *oscillation*
  (frequent delta sign changes) as well.
* **The regions those tables point at are float data, not int16 vertices.** The
  tell is the recurring value `16256` = `0x3F80`, the high half of float `1.0`
  read misaligned. Bounding boxes come out with aspect 1.00 across all three
  axes, which no car has. These look like transforms/matrices.

**Retracted lead — `0x001DAEF1`..`0x001DAF57` is NOT a `.bgv` dispatch table.**
Searching `.text` for the header version immediates (`0x403`/`0x405`/`0x406`)
turns up `PUSH` sequences pairing each with a pointer, which looks exactly like a
per-version parser table. It is not. Disassembling the three pointed-to
addresses gives:

```
MOV ECX,[0x3C0A40] ; MOV EAX,[ESP+8] ; MOV EDX,[0x3C0A3C]
PUSH 0x0004000E    ; PUSH 0x41AAE0   ; PUSH ECX ; PUSH 0x10 ...
```

Runtime globals and flag-like IDs — C++ static-initialiser / registration code.
The version-looking immediates are coincidental.

Two lessons, both cheap to repeat and expensive to act on:
* **Small immediates are terrible search keys.** `0x403`/`0x405`/`0x406` occur
  constantly as unrelated IDs and flags.
* **Verify every immediate hit by disassembling around it.** The `0x406` match at
  `0x00169855` is bytes `C7 06 04 00 00 00` = `MOV dword ptr [ESI],4` — the
  immediate does not exist at all, it spans two operands.

**Falsified: vehicle meshes do NOT reuse the track layout.** Applying the track
submodel structure (tag==1, vtx/sub pointers, stride 0x90 submeshes, 0x1C
vertices) at every `.bgv` header offset and every sub-offset table entry, with
and without the 0x40 bounds prefix, validates **nothing**. The vehicle path is a
genuinely different format, so track knowledge does not transfer.

**Using the game's own loader is the right approach** (and is what the emulation
harness exists for). Status of that search: the `.bgv` filename is built in
`FUN_0018D0E0`, but its callees are file I/O and generic helpers, not the mesh
parser. Scanning for code reading the header's consecutive table pointers
(`+0x4C/0x50/0x54/0x58`) yields 57 candidate windows; the most promising sit in
the asset-loading neighbourhood around `0x00188C00`-`0x0018D0E0`, but none has
been confirmed as the mesh parser yet. These are small `disp8` offsets, so the
search is noisy for the same reason the version-immediate search was.

**Failed attempt: brute-force emulation of candidate functions.**
`tools/find_bgv_parser.py` maps a real `Car1.bgv`, calls every function in
`0x185000..0x195000` with a pointer to it, and scores them on reading the header
pointers (`+0x4C..+0x68`) and then following them. Top hits `FUN_00190330` /
`FUN_00190380` scored 16/16 — and are **not** parsers. They are 33-line loops
that OR a flag bit into a byte at `+0x4D8D`.

The detector was invalid by construction:
* the buffer pointer was placed in **all four registers and four argument
  slots**, so any function reading small offsets from any input scores as
  "read the header pointers";
* "followed a pointer" counted a read anywhere within `0x400` bytes of a target,
  which over a `0x90000` file is close to free.

The tool is kept because the *approach* is right and the harness works; the
scoring is what needs replacing. A valid version must pass the buffer in exactly
one location (determined from the real call site), and require the follow-read to
land on the pointer target itself rather than a wide window.

**This is the fifth heuristic to produce a confident wrong answer on the `.bgv`
format** (float3 scan, coherence, coherence+oscillation, version immediates,
brute-force emulation). The pattern is consistent: any metric loose enough to
return a ranking over this file returns a ranking over noise. Ground truth is
what has worked everywhere else in this project, and the way to get it here is a
real call site -- find what actually invokes the parser and with what arguments,
rather than guessing the calling convention and spraying the pointer.

**Dataflow lead (valid method, unfinished).** Tracing `FUN_0018D0E0` rather than
guessing: it does `in_ECX[0x14] = FUN_00018BB0()` and later reads
`*(char *)(in_ECX[0x14] + 0xD)` as a count — so `in_ECX[0x14]` is the parsed
model handle. But `FUN_00018BB0` is only an 11-line accessor:

```c
if (((param_3[1] < 0) || (param_3[1] == param_2)) && (*param_3 == 0)) {
    *param_3 = -1;
    return param_3[3];          // cached handle
}
```

It hands back a pointer already stored in a resource slot. **The parse therefore
happens in the resource-loading path, upstream of `FUN_0018D0E0`, not inside
it.** Next step is to find who populates `param_3[3]` for `.bgv` resources —
that is the function to emulate, with the argument in the one place the real
call site puts it.

**The other principled step is not more pattern matching.** Emulate the loader
`FUN_0018D0E0` (found via the `.bgv` string xref, section 4) with
`tools/emulate_vehicle.py` and watch which file offsets it reads and in what
order — the same technique that cracked the physics. Guessing at strides has
produced three false positives so far; the loader knows the answer.

### `pveh/*/Car*.bgv` — vehicle model container (partially decoded)

Header, verified across 8 files:

| Offset | Type | Value | Meaning |
|---|---|---|---|
| `+0x00` | u32 | `0x17` | magic/kind, constant |
| `+0x04` | u32 | `0` | constant |
| `+0x08` | u32 | varies | **total file size** — exact match on all 8 files tested |
| `+0x0C` | u32 | `0x0406` | version, constant |
| `+0x10` | u32 | `3` | count (LOD or section count; unconfirmed) |
| `+0x14` | f32 | e.g. `2.6764` | per-car dimension (unconfirmed) |
| `+0x18` | f32 | e.g. `0.3117` | per-car dimension (unconfirmed) |
| `+0x1C`..`+0x28` | f32×4 | `1.0` | scale |
| `+0x4C`.. | u32[] | | section offsets into the file |

Section offsets at `+0x4C`/`+0x50` point to **tables of ascending u32 sub-offsets**
(`0x70, 0xF0, 0x138, 0x180, 0x1AC, ...`) — per-part directories.

Geometry is **not** plain vertex arrays and **not** RenderWare stream chunks. The
blocks contain NV2A (Xbox GPU) push-buffer command words — e.g. `0x00020080`,
`0x00020500`, `0x00020980` at `+0x1C80`. Extracting meshes therefore means
interpreting pushbuffer state, not just reading a vertex list. **This is the
main blocker for rendering real car models.**

Embedded class strings are present (`"compact1"` in `COMP/Car1.bgv`).

### `Tracks/<region>/<track>/` — track data

| File | Notes |
|---|---|
| `static.dat` | container; `+0x00` = `0x27` (39) entry count, `+0x04` = `0x006F8000` = exact file size |
| `streamed.dat` | streaming geometry |
| `enviro.dat` | starts with floats (`0.925, 1.0, 1.0, 0.0, 1500.0, ...`) — environment/lighting params |
| `Gamedata.bgd` | game/logic data |
| `CRASH*.RWS` | RenderWare stream, chunk type `0x080D`, libraryID `0x1C020009` |
| `MUSIC.RWS` | 124 bytes, playlist stub |
| `SOUND.AWD` | audio wave dictionary |
| `E_DJRACE.xwb` | XACT wave bank |

Track set present: regions under `Tracks/AS/` are `C1_V1`, `C1_V2`, `C2_V1`,
`C2_V2`, `C3_V1`, `C3_V2`, `M1_V1`, `M1_V2`.

---

## 6. Game state

### Vehicle entity array

`FUN_00012170` (`.text`) reads:

```c
int idx = DAT_004AED45;                                  // active/player index
p = (&DAT_004AE728)[idx * 0x62];                         // 0x62 dwords = 0x188 bytes stride
if (*(char *)(p + 8) != 0) f = *(float *)(p + 0x74);
if (fabsf(f) > 0.15f) (&DAT_004AE8A8)[idx * 0x62] = DAT_004AE200;
// hysteresis: set flag at +0x232 when |f| > 0.3, clear when |f| < 0.15
```

Established:

- `0x004AE728` — array of pointers to vehicle/entity records, **stride `0x188`
  bytes** (392). Lives in `.data` BSS.
- `0x004AED45` — active entity index.
- Entity record: `+0x08` = active/valid byte, `+0x74` = a signed float driving a
  0.15/0.3 hysteresis pair, `+0x232` = resulting state flag.

The `0.15`/`0.3` constants are exactly the ones the raw-binary load misread as
`22.0` — this function was uninterpretable before the reload.

---

## 7. Vehicle physics model (recovered)

This is the substantive result of the corrected load: the game's complete
vehicle physics parameter set, with real offsets and real values.

### How it was found

`.rdata` holds the path `../Export/ValueDB/VehiclePhysics/%s.cfg` (`0x003AC65C`)
and a block of parameter-name strings around `0x003AC600`–`0x003AD200`. Those
names are consumed by **`FUN_00132D10`**, which registers every tunable with the
game's ValueDB. Each registration compiles to:

```asm
LEA  <reg>,[ESI + 0xNNN]     ; address of the field in the physics struct
MOV  ECX, <group string VA>  ; "Physics/Transmission/Engine"
MOV  EDX, <name string VA>   ; "Peak Torque Revs"
CALL 0x001AEE20              ; register(name, group, &field, ...)
```

The final registration folds the displacement into the base register
(`ADD ESI,0x1CC` / `PUSH ESI`) instead of using `LEA`, since `ESI` is dead
afterwards — worth knowing, it is easy to miss and yields a duplicate offset.

Defaults come from the constructor **`FUN_00132950`**, which stores float
immediates into the same offsets before registration.

`tools/extract_physics_params.py` automates both and emits
`src/burnout3_physics_params.h`.

### Result

**64 parameters, 12 groups, spanning `+0x0B8`..`+0x1CC`. All 64 have a
compiled-in default.** Struct stride is `0x1D0`, confirmed independently by
`FUN_0011A810`, which indexes a config cache with `iVar3 * 0x1D0 + param_1`.

Self-validation: the groups land in contiguous ascending blocks matching C++
member declaration order — gear ratios occupy `+0x0E0`..`+0x100` in the exact
sequence Reverse, Neutral, 1st…6th, Final; both suspension groups are unbroken
four-field runs; body-roll is `+0x138`..`+0x144`.

| Group | Fields | Notable defaults |
|---|---|---|
| `Physics/Vehicle` | Mass (Kg) | **1000** |
| `Physics/Suspension/Front` | attach height, spring force/damping/length | 0.1 / **60000** / **5000** / 0.2 |
| `Physics/Suspension/Rear` | same four | identical defaults to front |
| `Physics/Transmission/Gear Ratios` | Reverse…Final | −1, 0, then 5.0 ×7 |
| `Physics/Transmission/Engine` | idle/up/down/max RPM, torque, peak, falloff | 1000 / 6800 / 4200 / 6800 / **630** / **5000** / **6500** |
| `Physics/Transmission/Boost Kick` | torque, time | 2.0 / 1.0 |
| `Physics/Race Car/Misc` | drag, downforce, braking, accel mult, engine braking, boost top speed | **1.9** / 0 / 2.0 / 4.0 / **−5500** / **141** |
| `Physics/Race Car/Body Roll` | brake/accel/steer/drift force heights | −0.55 / −0.35 / −0.22 / −0.23 |
| `Physics/Race Car/LSDM` | speed limit, steering angle, torque 1/2 | 35 / 50 / 1.0 / 0.9 |
| `Physics/Steering` | min/max angle, min/max velocity, response, drag | 10.5° / 14° / 23 / 12 / 0.158 / 2.0 |
| `Physics/Drift` | slide min/max, turn momentum slow/fast, auto-drift delay, turn rate, min drift speed, max drift angle | 0.75 / 0.85 / 800 / 1100 / 0.77 / 1.2 / 65 / **45°** |
| `Physics/Aggressive Driving Reaction` | steer-away time, out-of-control time, max angle/velocity, drag | 0.3 / 1.0 / 24 / 50 / 0.5 |

Note the shipped game contains **no** `.cfg` files and no parameter names appear
in any data file on disc — the values above are the ones the retail build runs
with unless a per-vehicle override exists elsewhere (not yet located).

### A second config type

`FUN_00132D10` is not the only registrar. **`FUN_00134AC0` registers a second,
reduced config**: 9 parameters at `+0x88`..`+0xA8`, with its identity key at
`+0x80`/`+0x84` instead of `+0xB0`/`+0xB4`.

| Offset | Group | Name |
|---|---|---|
| `+0x88` | `Physics/Vehicle` | Mass (Kg) |
| `+0x8C`..`+0x98` | `Physics/Suspension/Front` | attach height, spring force, damping, length |
| `+0x9C`..`+0xA8` | `Physics/Suspension/Rear` | spring force, damping, length, attach height |

Mass plus both suspension sets and nothing else — no gearbox, engine or drift
model — which fits traffic vehicles. That matches the roster: 40 of the 107
vehicles are `.btv` traffic cars.

**73 registered physics parameters in total.** This one was missed on the first
pass and only surfaced after the struct was applied in Ghidra (section 8.1),
which rendered the writes below `+0xB8` visible as real fields rather than
opaque header bytes.

### Config → live vehicle mapping

`FUN_00134710` copies the config into the live vehicle instance, which gives the
live struct's layout for free:

| Config | Live vehicle |
|---|---|
| `+0x0B8` Mass | `+0x1F0` |
| `+0x0BC/C0/C4/C8` front susp. | `+0xCA0 / 0xCA8 / 0xCA4 / 0xCAC` |
| `+0x0CC/D0/D4/D8` rear susp. | `+0xCB8 / 0xCB4 / 0xCBC / 0xCB0` |
| `+0x0E0`..`+0x100` gears | `+0x1448`..`+0x1468` |
| `+0x104`..`+0x118` engine | `+0x146C`..`+0x1480` |

Also recovered: the vehicle has a **wheel array with stride `0xC0`** (the loop
advances an `undefined4*` by `0x30` elements = 192 bytes) and a wheel count byte
at `+0x1169`. Wheels 0–1 take the front attach height, 2+ the rear.

**Correction:** `+0x894` is not the array base. `FUN_00123FD0` walks the same
array from `param_1 + 0x820` with the same `0xC0` stride, and
`0x894 − 0x820 = 0x74`, so `FUN_00134710` is writing **field `+0x74` of each
wheel**. Base is `+0x820`; the array spans `0x820`–`0xB20` for four wheels,
which sits cleanly below the suspension config copies at `0xCA0+`. An active
flag byte lives at wheel `+0xB4`. Typed map: `src/burnout3_vehicle_struct.h`.

### Vehicle class vtable

`FUN_001049A0` (physics setup) is a virtual method; its function pointer sits at
`0x003B1258` inside a vtable region spanning roughly `0x003B1108`–`0x003B1264`
(several adjacent vtables). A repeating six-slot interface is visible —
`[?, 0x00104C70, X, Y, 0x0011BE40, 0x001A98D0]` — recurring at entries 12–17,
22–27 and 78–83, i.e. one block per vehicle variant. The string `"Ghost car"`
follows the table at `0x003B1268`.

Call chain: `FUN_001049A0` → `FUN_0011A8F0` → `FUN_0011A810`
(config cache lookup, stride `0x1D0`) → `FUN_00132D10` (register) +
`FUN_00134710` (apply).

---

## 7.1 RenderWare-style math helpers (salvaged)

These were annotated in the now-deleted `src/burnout3_math.c`. Addresses there
were in the old flat-load space; **corrected below by +0x10000**. Three were
spot-checked against the decompiler and hold; the rest are that file's original
claims and are marked unverified.

| Corrected VA | Meaning | Status |
|---|---|---|
| `0x00011570` | 4-component XOR with `0x80000000` — sign-flip negate | verified |
| `0x000115C0` | scale 4-vector by scalar | unverified |
| `0x00011610` | scale 4-vector by scalar (variant) | unverified |
| `0x00011640` | normalise 4-vector | unverified |
| `0x000116E0` | 4x4 * 4x4 matrix multiply, row-major | verified |
| `0x000117E0` | 4x4 matrix multiply (variant) | unverified |
| `0x00011900` | quaternion -> rotation matrix (uses `fsin`/`fcos`) | verified |
| `0x00011D90` | rotation matrix from quaternion, X axis | unverified |
| `0x00011E50` | rotation matrix from quaternion, Y axis | unverified |
| `0x00013BD0` | add two 4-vectors | unverified |
| `0x00013CA0` | transform vector by 4x4 matrix | unverified |

Note the original README claimed `0x00001E50` was "audio processing" while the
math file called the same address a Y-axis rotation matrix. The math file was
right; that contradiction is what first suggested the old documentation was
unreliable.

## 8.1 Typing the structs in Ghidra

The recovered layouts are now applied as real Ghidra data types, which is the
prerequisite for porting the integrator — 1,900 lines of `*(float *)(param_1 +
0x1470)` is not reviewable, `v->flEngine_change_up_rpm` is.

| Struct | Size | Notes |
|---|---|---|
| `B3PhysicsConfig` | `0x1D0` (464) | 64 named float params + 6 explicit pads |
| `B3Wheel` | `0xC0` (192) | `attach_height` at `+0x74`, `active` at `+0xB4` |

**Gotcha:** `/create_struct` packs fields sequentially and ignores the `offset`
key when there are holes. The first attempt came out 440 bytes with the last
five fields 24 bytes low — silently wrong. Explicit `undefined4` padding must be
inserted at every gap (`0xDC`, `0x110`, `0x114`, `0x1A8`, `0x1AC`, `0x1B8`), then
`recreate_struct` with `force=true`. Always verify the resulting size and
offsets afterwards.

All of this is reproducible: **`tools/apply_ghidra_types.py`** rebuilds the three
structs against a fresh import, verifies each size and every parameter offset,
applies the prototypes, and checks the result. The Ghidra project is not in the
repo, so the typing has to be a script rather than manual GUI work.

| Struct | Size |
|---|---|
| `B3Wheel` | `0xC0` |
| `B3PhysicsConfig` | `0x1D0` |
| `B3Vehicle` | `0x14D0` |

Applied via `/set_function_prototype` (calling convention goes in the separate
`calling_convention` field, not inline in the prototype string; the available
conventions here are `__cdecl`, `__thiscall`, `__regparm1/2/3`, not `__fastcall`):

```
0x00132950  void b3_physics_config_defaults(B3PhysicsConfig *cfg)   __regparm1
0x0011D460  void b3_vehicle_drivetrain_update(B3Vehicle *v)         __cdecl
0x00123FD0  void b3_vehicle_suspension_update(B3Vehicle *v)         __cdecl
```

**Convention matters.** `FUN_0011D460` takes its vehicle pointer on the stack,
not in a register. Applying `__regparm1` "succeeds" but yields
`in_stack_00000004` and *zero* named-field references; `__cdecl` yields 234.
The script asserts on that count for exactly this reason.

which decompiles to, verbatim:

```c
cfg->flVehicle_mass_kg              = 1000.0;
cfg->flMisc_drag_coef               = 1.9;
cfg->flBody_roll_brake_force_height = -0.55;
cfg->flSteering_response            = 0.158;
```

This is an independent confirmation of the extracted defaults: the values were
pulled programmatically from raw offsets, and typing the struct reproduces them
as named fields with identical values.

It also paid for itself immediately — writes into the `+0x88`..`+0xA8` region
became visible as real fields, which is how the second registrar (section 7) was
found.

---

## 8. The vehicle integrator (located and mapped)

**`FUN_0011BE50` is the per-frame vehicle physics update.** It takes a single
`float` (delta time), operates on the vehicle through `ECX` (`__thiscall`), and
is reached from the vehicle vtable via the stub at `0x0011BE40` (vtable entries
16 / 26 / 82, section 7). It contains several loops over the wheel count byte at
`+0x1169`.

Found by scanning `.text` for `disp32` accesses to the distinctive live-struct
offsets recovered in section 7 — `+0x1169` (wheel count), `+0x894` (wheel array),
`+0x1448` (transmission). Note the sub-updates reach engine/gear fields through
sub-struct pointers, so a naive scan for large constant displacements finds
almost nothing; the wheel-count byte is the reliable handle.

### Pipeline

18 callees, in call order, profiled by size, float density, and which recovered
struct regions they touch:

| Stage | Lines | Floats | Touches | Likely role |
|---|---|---|---|---|
| `FUN_00121560` | 220 | 124 | — | pre-pass |
| `FUN_00118410` | 534 | 145 | — | |
| `FUN_00123000` | 323 | 141 | wheels | |
| `FUN_0011C720` | 22 | 2 | — | small helper (called twice) |
| `FUN_0011BC60` | 116 | 18 | — | |
| **`FUN_0011D460`** | **1046** | **709** | **engine, wheels, mass** | **drivetrain / main force integrator** |
| `FUN_0011AEF0` | 560 | 222 | mass | rigid-body integration |
| **`FUN_001239C0`** | 366 | 130 | wheels, suspension | wheel/suspension pre-pass |
| **`FUN_00123FD0`** | **852** | **310** | **wheels, suspension, mass** | **suspension / tyre force solver** |
| `FUN_00109560` | 456 | 214 | mass | |
| `FUN_0018DA00` | 126 | 65 | — | |
| `FUN_0011FFA0` | 132 | 53 | wheels | |
| `FUN_0010DCA0` | 19 | 0 | — | |
| `FUN_0010DD20` | 438 | 134 | — | |
| `FUN_00109BB0` | 22 | 14 | mass | |
| `FUN_00125100` | 119 | 40 | mass | |
| `FUN_00126520` | 543 | 111 | — | |
| `FUN_00126D40` | 228 | 45 | — | |

Handling feel lives principally in `FUN_0011D460` and `FUN_00123FD0`
(~1,900 decompiled lines between them).

### Confirmed live-struct fields

From `FUN_0011D460`:

```c
// gear change-up test
if (*(float *)(v + 0x1470) <= *(float *)(v + 0x149c) * 9.549296f) ...
```

`9.549296` is `60 / 2π`. So `+0x149C` is **engine angular velocity in rad/s**,
converted to RPM and compared against `+0x1470`. Section 7's config→live table
independently predicts `+0x1470` is **Change Up RPM** (config `+0x108`) — the two
derivations agree, which cross-validates the whole mapping.

Also identified:

| Live offset | Meaning | Evidence |
|---|---|---|
| `+0x149C` | engine angular velocity (rad/s) | `× 9.549296` → RPM |
| `+0x1470` | change-up RPM | config `+0x108`, and the shift test above |
| `+0x13D4` | speed in **mph** | `× 0.44704` → m/s |
| `+0x1444`, `+0x1446` | drivetrain state flags (bytes) | gating the branch above |
| `+0x14C8`, `+0x14CC` | gear indices (current vs target) | compared for inequality |
| `+0xB0`..`+0xBC` | force/direction vector + scalar | scaled together by one factor |
| `+0x1360` | damping/resistance coefficient | multiplies `+0xBC` |

Units are mixed: the game stores road speed in mph and converts with `0.44704`
where SI is needed. Any faithful port has to preserve that or the numbers drift.

---

## 8.2 Executing the real code (the verification loop)

The blocker on porting was never readability — it was that **a ported equation
producing plausible numbers is indistinguishable from a correct one**. Every
error in this project was caught only when a second derivation happened to
disagree.

That is now solved, and it does not need a console emulator. We do not need to
run the *game*; we need to run the *function*. `tools/emulate_vehicle.py` uses
Unicorn (`pip install unicorn`) to:

* map `build/burnout3.elf` at its real VAs — the corrected image is what makes
  this possible at all
* place a synthetic vehicle struct in scratch memory, with pointer fields aimed
  at a scratch page
* call the function `__cdecl` with a sentinel return address
* lazily map any faulting page so execution proceeds instead of dying on the
  first unresolved pointer
* diff the struct before/after for the true write set

`FUN_0011D460` **runs to return with zero faults**, touching 42 struct offsets
and writing 18, of which 5 change value.

### Result: the resistance port is verified exactly

`tools/validate_port.py` drives the emulator with varied inputs and compares
against the ported C. **7/7 cases match to 1e-3**, including a non-axis-aligned
direction vector and a reversed one:

| rc | v | k | q | direction | model | emulated |
|---|---|---|---|---|---|---|
| 0.50 | 40 | 0.01 | 1.0 | (0,0,1) | −20.40 | −20.40 |
| 0.25 | 40 | 0.01 | 1.0 | (0,0,1) | −10.40 | −10.40 |
| 0.50 | 40 | 0.02 | 3.0 | (0,0,1) | −27.20 | −27.20 |
| 0.50 | 40 | 0.01 | 1.0 | (0.6,0,0.8) | −16.32 | −16.32 |
| 0.75 | 60 | 0.00 | 0.0 | (0,0,−1) | +45.00 | +45.00 |

This also confirmed the force accumulator is at **`+0x0F0`/`+0x0F4`/`+0x0F8`**
(x/y/z), and identified the vertical term: `-(k*x*0.1 + 10.0) * mass`, which
with `k*x == 0` gives exactly `-10000` for a 1000 kg car — i.e. **gravity at
10.0 m/s², not 9.81**.

### Fields identified by differential probing

`tools/probe_fields.py` perturbs one unknown input at a time, re-runs the real
function, and reports which outputs moved. Of 28 unknown inputs probed, 26 are
inert on this code path; two drive the force accumulator, and both now have
exact formulas verified against the instructions:

| Offset | Meaning | Formula | Verified |
|---|---|---|---|
| `+0x1408` | quadratic resistance term | `-k * speed_ms * q^2` | 5/5 values |
| `+0x1364` | downforce coefficient | `-(c * mph * 0.1 + 10.0) * mass` | 16/16 |

**Two constants that only running the code could reveal:**

* **Gravity is `10.0`, not `9.81`.** A 1000 kg car gets exactly `-10000`.
* **The m/s→mph constant is `2.2374146`, not the true `2.2369363`** — Criterion's
  value is 0.02% high. The downforce term converts `speed_ms` with it rather
  than reading the stored mph field at `+0x13D4`. Getting this wrong produces a
  small, speed-dependent error that would be almost impossible to spot by eye
  but would compound over a lap.

Both are now in `src/burnout3_vehicle_struct.h` as `B3_GRAVITY` and
`B3_MS_TO_MPH_GAME`, and ported into `b3_vertical_force()`.

`tools/validate_port.py` ran **12/12 green** (7 resistance, 5 vertical) at
this point; later sessions grew it to 41/41 (drivetrain, section 9) and
49/49 (suspension, section 10).

### Limits of function-level emulation

Running `FUN_0011D460` in isolation reaches **one code path**. Perturbing wheel
active flags, drivetrain flags, gear mismatch, over-rev, steering angle, reverse
gear and a seeded identity transform all produce **identical** output; only
zeroing the speed changes anything (resistance goes to 0, as expected). The write
count stays at 18 across every variation.

Tracing reads outside the vehicle shows why. The function reads:

* **16 offsets from a pointed-to object** at `+0x00`..`+0x34`, in a `0/4/8/C`
  per-row pattern — a **4x4 transform matrix**, i.e. a RenderWare frame
* **beyond `vehicle+0x14D0`**: `+0x1520`, `+0x1524`, `+0x153D`, so the real
  struct is larger than the 0x14D0 currently declared

Seeding an identity matrix did not change the outcome, so the branch conditions
depend on further environment state — the objects behind `p_owner`/`p_cfg_obj`,
and state written by the 17 other pipeline stages before this one runs.

**What this means for the port.** Function-level emulation gives *exact*
validation of the paths it can reach, and that is genuinely valuable — it caught
the missing 4th force component and the two wrong constants. But it cannot by
itself drive the branches, because the inputs that select them are produced
elsewhere in the frame. Getting those requires either reconstructing each
upstream object (slow, and each reconstruction is itself an unverified guess) or
capturing real state from the game running under a whole-system emulator.

This is the specific, narrow thing a console emulator is still needed for: not
checking arithmetic, but supplying realistic inputs so the interesting paths
execute at all.

### An unexplained term (recorded, not guessed)

Seeding the frame matrix with identity opened a code path that adds a term to
the **X** accumulator: exactly `-20.0 * dir.x`. That `-20.0` is invariant to
resistance coefficient, speed, quadratic term, mass, steering angle, wheel
count, engine rpm and stored mph -- everything probed. With the matrix zeroed it
does not appear at all, which is why the first 7/7 pass did not see it.

It is **not modelled**, and `tools/validate_port.py` deliberately does not assert
on X rather than fitting a constant that is probably an artefact of the synthetic
environment. Z and W remain verified exactly.

This is the honest shape of the work: making the environment more realistic
exposed a term that a less realistic setup had hidden. Expect more of these, and
expect some to be artefacts rather than real physics -- which is exactly why the
term is recorded as unexplained instead of being absorbed into the model.

### The workflow from here

1. Port a block from the typed decompilation.
2. Add a case to `tools/validate_port.py`.
3. Run it. If it does not match the real instructions, the port is wrong.

Porting is now a checkable loop rather than careful reading. That does not make
the remaining ~1,900 lines quick, but it removes the failure mode where errors
accumulate silently.

---

## 9. What is not done

Ordered by what stands between here and "races like the real thing".

1. **The integrator port.** Located, typed, and **begun** — two blocks are now
   transcribed 1:1 (longitudinal resistance and the gear change-up test, both
   from `0x0011D460`, at the bottom of `src/burnout3_vehicle_sim.c`, with unit
   tests) **and verified against the real code by emulation** (section 8.2).
   That is roughly 20 lines of ~1,900. The rest is **not ported**.

   **How far off it really is.** Of `FUN_0011D460`'s 1,075 decompiled lines, 234
   reference the vehicle through the applied type — but only **55 of those hit
   identified fields**; the other **183 index into padding**, spanning 10
   distinct unidentified regions (`0x0000`, `0x00C0`, `0x0140`, `0x0B20`,
   `0x1168`, `0x1364`, `0x13D8`, `0x13FC`, `0x1445`, `0x14A0`). So the bulk of
   the drivetrain is still **exploratory RE, not transcription** — each of those
   regions needs identifying before the surrounding math can be ported at all.
   Typing the struct made the function readable; it did not make it mechanical.

   **But the validation surface is far smaller than the read surface.**
   `tools/field_usage.py` classifies every struct access in a function as read,
   written, or both. For `FUN_0011D460`: **69 distinct offsets touched, but only
   9 are ever written** —

   | Role | Offsets |
   |---|---|
   | written only (outputs) | `0x0B7C`, `0x0C3C`, `0x1430`, `0x14A8` |
   | read + written (integrated state) | `0x0008`, `0x09F4`, `0x0AB4`, `0x142C`, `0x143C` |
   | read only (inputs) | the other 60 |

   So the drivetrain is overwhelmingly a *consumer*. A port does not have to
   explain all 47 unknown inputs to be checkable — it has to reproduce **9
   output fields**. That is a tractable target, and it is the right unit for
   incremental validation. `0x1408` is read 15 times and `0x1440` 7 times, so
   those two inputs dominate and are the highest-value ones to identify next.
   `b3_vehicle_step()` in `src/burnout3_vehicle_sim.c` is still a conventional
   vehicle model wired to the real parameters — real data, inferred forces.
   Handling will not match until `FUN_0011D460` (drivetrain, 1046 lines) and
   `FUN_00123FD0` (suspension/tyres, 852 lines) are ported line-for-line, which
   in turn needs the wheel record at `+0x894` (stride `0xC0`) fully typed. This
   is now a bounded transcription job rather than a search problem, but it is
   ~1,900 lines of dense float math over a partially-typed struct.
2. **Geometry.** `.bgv` and `static.dat` store NV2A push-buffers, not vertex
   arrays — see the command words at `Car1.bgv+0x1C80`. Decoding means walking
   pushbuffer state to recover vertex/index buffers. Nothing renders from real
   geometry today; cars are boxes and the track is a parametric figure-8.
3. **Textures.** `Data/Global.txd` / `Frontend.txd` do not match the RenderWare
   TXD layout; format unidentified.
4. **Audio.** `.awd` dictionaries and `.xwb` XACT banks are untouched; engine
   audio is synthesised.
5. **Function naming.** 7,434 functions, 119 named — all Ghidra FID matches of
   CRT routines. No game function is named and no struct is defined in the DB;
   the layouts in this document live only here and in the generated headers.
6. **RenderWare separation.** Much of `.text` is RW36 SDK rather than Criterion
   code and has not been partitioned off.

Scale note: items 1–4 are each substantial independent efforts. A faithful
recreation is a long project, not a session's work; what exists now is a correct
foundation plus the physics parameter set, not a reproduction of the game.

---

## 9. Session addendum (2026-08-10) — streamed roads, paths, .bgv, drivetrain

Everything below is [C] confirmed unless marked.

### streamed.dat road units [C]
Unit table in static.dat at +0x54 (u16 count, ptr at +0x58); 0x10-byte entries
{sub_off, lod_off (both +0x10 into streamed.dat), sub_size, lod_size}. Each
unit = one Xbox track submodel (same layout as static groups). Streamed
submodels have a zero material-index pointer; indices come from the PVS block
at submodel+0x20 (count byte at +1, u16 table at +0x14, per-unit chunks at
+0xBA stride 0xA9, first byte 0 = current unit). Implemented in
tools/extract_track.py; 1486/1486 submeshes textured. The reflection group is
skipped (mirror-pass duplicate; z-fights if drawn directly). Road textures are
DXT5 whose alpha is a reflection mask, NOT transparency — alpha-test only
textures with >35% fully-transparent texels (fences/foliage).

### Gamedata.bgd paths [C]
Baked memory image, base 0x00320000 (internal pointers all fall in
[base, base+filesize)). Point pool of 16-byte records {f32 x,y,z, u32 w} in
file region ~0x17C000..0x1B8000. Recovered by scan + overlay verification
(build/bgd_walls.png): center/racing line at 0x1ABF20 x1029 (5.7 km lap),
road-boundary strip at 0x19D970 x1752 = two interleaved wall strands (mean
corridor width 14.5). tools/extract_bgd_paths.py emits
src/burnout3_track_paths.h. The .bgd's own directory structure is NOT decoded;
segmentation is tooling. [S] 0x1A8540 x926 is a different loop (2D pts,
height in w) — unidentified.

### .bgv vehicle container [C]
See BGV_EXTRACTION.md (relinker FUN_000310f0 is the authority). Geometry:
tools/extract_bgv.py, 67/67 player cars. Paint: record at header+0x60 —
fmt 0xB = 8bpp paletted, Morton-swizzled; +0x69 palette count (= color
variants), +0x14 palette ptr array; palette rec {01 00 03 00|C0} then data ptr,
256×BGRA. tools/extract_bgv_textures.py, 67/67. Draw path:
FUN_000303d0 (per-model; LOD fallback walk over +0x4C table, aux table at
+0x60 with count at +0x69) → FUN_00031e10 (per part: records at part+4,
stride 0x1C, damage mask u16 at rec+0x18 vs 0x3FF) → FUN_00031ab0 (per record:
DrawIndexedPrim(TRISTRIP, count=rec+0x10, ptr=rec+0xC); rec+0x1A = texture
slot; rec+0x14 selects shader; rec+4 = glass tint set by FUN_000300a0, part
header byte1 = glass record index). [?] The 10-slot per-LOD part table
(damage panels/wheels) vertex bases are still unresolved — the whole-car mesh
comes from the relinker walk instead.

### Engine/transmission [C] — ported & integrated
FUN_00121560 (engine+gearbox per frame), FUN_0011ECF0 (neutral/reverse
engagement + reverse throttle/brake swap), FUN_0011D460 torque split
(50/50 rear, omega += torque*0.04*dt). Live struct +0x1448..+0x14D8 typed in
burnout3_vehicle_sim.h (B3EngineTransmission). validate_port.py: 41/41.
Corrections found while porting: live +0x13D4 is config Max Speed In Boost
(mph), not a speed copy; +0x14CC is the forward gear COUNT, not target gear.
The true m/s→mph constant 2.2369363 appears in the idle-floor check (unlike
2.2374146 elsewhere). PRNG seeds 0xFD462907/0x02B9D6F8 (engine jitter).
CAVEAT [C]: compiled-in gear-ratio defaults are placeholders (all 5.0);
real ratios are per-car in each .bgv's VDB (hashed keys incl. per-car cfg
filename — see burnout-data-tool Hash.cs). b3_vehicle_step substitutes marked
reconstruction ratios until the VDB-apply path is emulated.

### Audio [C]
docs/AUDIO_NOTES.md: XWB v3 (876 waves, all WMA), AWD (1569 PCM waves incl.
per-vehicle .hwd/.lwd engine rpm loops), RWS (131 PCM streams, interleave
proven three ways). 4.3 GB extracted to build/audio/.

---

## 10. Session addendum (2026-08-10, later) — per-car VDB physics, suspension port

Everything below is [C] confirmed unless marked.

### Per-car physics recovered — the VDB is Data/vdb.xml, NOT inside .bgv [C]

**Correction to section 9's caveat and HANDOFF 6A:** the per-car tuning VDB is
not embedded in each `.bgv`. It is the single retail file **`Data/vdb.xml`**
(binary, despite the extension), byte-identical to the community dump
`burnout-data-tool/data/vdb/vdb_xbox_bo3_release.xml`. A scan of every shipped
file for the recovered hash dwords finds them only there (the occasional
1-2/64 hits in media files are noise). Format (per bdtool's VDBParser.cs,
verified against the file): header `{i32 type=2, i32 defaultCount=7525, i32,
i32 fileDefCount=348, i32 fileDefOffset}`, then 7525 x
`{u32 rawValue, i32 keyHash}` default records (all distinct), then
`{u32 active, i32 pathHash}` file definitions.

**The hash pipeline, recovered by executing the game's own registrar**
(tools/extract_car_vdb.py, stage 1):

* `FUN_00132D10` (EAX = 0x1D0 config struct, EBX = vehicle) registers all 64
  params; `FUN_00134AC0` (EAX = struct, stack: packed id lo/hi) registers the
  9-param traffic subset at +0x88..+0xA8 (mass + suspension).
* The car name is the **base-40 decode** (`FUN_001AECC0`) of the packed 8-byte
  vehicle ID in `vlist.bin` (+0x408, 107 entries): charset
  `0=' ' 1='-' 2='/' 3-12='0'-'9' 13-38='A'-'Z' 39='_'`, 12 chars,
  trailing-space trimmed. IDs decode to `COMPCAR1..TSPCCAR6`.
* The registrar sprintf-builds `../Export/ValueDB/VehiclePhysics/%s.cfg` into
  the config struct at +0x00 and stores the packed ID at +0xB0/+0xB4
  (traffic: +0x80/+0x84).
* Registration key composition (`FUN_001AEEB0`), **captured from the real
  code, not guessed**: `"<param name><group>/<cfg path>"` — param name FIRST,
  e.g. `Mass (Kg)Physics/Vehicle/../Export/ValueDB/VehiclePhysics/COMPCAR1.cfg`.
  This is why the earlier direct-CRC attempts (8 cfgpath-first variants x 64
  params) matched nothing.
* The hash core `FUN_001AF250` is table-CRC over the 256-dword table at
  0x003F7700 with **SAR (arithmetic) shift**, sign-extended input bytes, and
  no final inversion: `h = 0xFFFFFFFF; h = (h SAR 8) ^ table[(h&0xFF)^byte]`.
  Matches the community `CalcGtHash` (Hash.cs / EdnessP's GtHash.py).

Verification chain: registrar emulation (Unicorn, hooks at 0x001AEE20 entry
for the destination offset, 0x001AF250/0x001AF27F for string+hash) produced
exactly the 64 known offsets; a pure-Python mirror reproduces every emulated
hash (64/64 on three car names); the community `bo3_vdb_definitions.yaml`
independently lists the same hash for COMPCAR1 Mass (0x6B81AAA0); and
`Data/vdb.xml` contains 64/64 computed hashes for every drivable (.bgv) vlist
car and exactly the 9-param subset for .btv traffic — the coverage pattern
mirrors the two registrars perfectly. Per-car cfg-path hashes also appear in
the fileDef section.

**Extraction results** (tools/extract_car_vdb.py generate →
src/burnout3_car_physics.h): 100/107 cars have VDB overrides — all 67 player
cars with the full 64, 33 traffic cars with the 9-param subset. No overrides
(compiled defaults apply): COMPCAR18, HEVYCAR35, TSPCCAR1/2/4/5/6. Values are
sane throughout: masses 800-2600 kg (traffic trucks to 10000), descending
gear ratios, finals 3.0-4.0, torque 570-2930, redlines 2500 (trucks) to
19000 rpm (HSPC F1 specials). Real COMPCAR1: mass 800, torque 570, gears
3.80/2.30/1.70/1.37/1.16/1.11, final 3.20 — the placeholder-5.0 story from
section 9 is now closed. Wired into the harness: each grid car gets its own
B3PhysicsConfig (init_vehicles → b3_config_set_by_offset), fallback approx
ratios only for cars without overrides.

### Suspension solver ported blocks [C] — validate_port.py 49/49

`FUN_001239C0` (pre-pass) + `FUN_00123FD0` (force pass), verified by
executing both under Unicorn with a fully seeded environment (ground soup at
+0x200, aux object at +0xCC4, per-wheel frame matrices at +0xCC8..).

**Wheel record field map** (base +0x820, stride 0xC0), recovered by
execution: +0x00 world pos, +0x10 contact point, +0x20 contact normal, +0x30
prev-frame copy of +0x00..+0x1C, +0x50 pre-pass ray height, +0x58 spin angle,
+0x5C omega, +0x60 previous spring length, +0x64 current spring length
(pre-pass output), +0x74 attach height, +0xB0 surface type u16, +0xB2 bump
flag, +0xB3 contact flag, +0xB4 force-applied flag. Config copies live at
+0xCA0..0xCBC (front attach/damping/force/length, rear ditto — FUN_00134710).

**Ported and verified equations** (bottom of burnout3_vehicle_sim.c, 7
suspension cases + 1 pre-pass case, all green):

* spring/damper (contact): `comp = attach - cur`, clamped to
  `[0.25*len, in_race ? 0.75*len : inf)`; compression clamp rewrites cur;
  `F = -(comp - len)*k + ((cur' - prev)/dt)*c` (damper uses the CLAMPED cur);
  `prev = cur'`. In-race soft clip: `ratio = comp/(0.75 len)`, if
  `ratio > 0.5` then `F *= (1 - ratio)*2` — force fades to exactly 0 at full
  droop (constants at 0x3A55F8/0x3B1684/0x3B1688/0x3B168C).
* bump flag (+0xB2) = `cur - prev > 0.12`, tested before any clamp.
* force application: `v+0xF0 += normal*F`; torque `v+0x100 += r x (normal*F)`
  with r = wheel world pos - frame origin (FUN_00106590). Wheel world pos =
  frame transform of the wheel-frame local position with y := cur'
  (FUN_00013CA0). A second accumulator pair (+0x110/+0x120) belongs to the
  impulse path (FUN_00106500).
* no-contact droop: stored length relaxes to `attach - 0.75*len`; the
  decompiled rate term `t*k*dt^3` is dead code (t = 1 - 1/0.75 < 0 clamps
  to 0 for every len) — mirrored exactly anyway.
* wheel spin (tail): omega *= 0.99 per call, *0.8 more with ground contact;
  spin integrates the NEW omega, wraps at +/-628.31854 (200*pi); sin/cos of
  the wrapped angle build the visual wheel matrix in the +0xCC8 frames.
* pre-pass airborne path (empty ground soup / surface type 3): +0x30 gets the
  prev-frame snapshot, contact flag clears, contact point = prev world pos -
  frame_up * (wheel+0x50), normal = frame up axis; v+0x1352 = 1;
  *(v+0xCC4)+0x49C = 10000.0 sentinel.

**Located but NOT ported** (documented, out of validate coverage):
* the bottom-out block: when the compression clamp cuts > 0.001 it
  accumulates `normal*(0.25len - comp)` (averaged into a separate
  accumulator) and runs an impulse solver (FUN_001066A0 point velocity,
  FUN_00106720 impulse with the inertia rows at v+0x40..0x68) plus the
  takedown/crash bookkeeping spaghetti behind it (vehicle-ID compares etc.).
* the body-scrape branch (in_race && v+0x116B==0 && speed > 0.1).
* the pre-pass ground-hit path: needs the track poly soup at +0x200
  (0x40-stride records: two edge vectors +0x10/+0x20, normal +0x30; u16
  surface types via ptr[2]; ray/triangle test FUN_001B2230, grip scalar
  writes at *(v+0xCC4)+0x324: 0.2 for type 0x26, else 1.2).
* the DAT_004D617E dt*1.2 alternate timestep.

**Tyre grip note [S]:** FUN_00123FD0 turned out to be suspension + wheel
spin/visual only — the lateral/longitudinal tyre grip forces are NOT in it;
they live upstream (FUN_0011D460's unexplored branches / FUN_00123000).
"Suspension/tyre solver" in section 8's table overstated this function.

Integration: b3_vehicle_step's static-compression reconstruction was replaced
by the ported spring/damper law relaxing each wheel toward the static-load
equilibrium (marked GLUE — the harness has no ray-cast ground contact).
Autodrive smoke test: races the circuit, 131 mph in 6th on the straight with
COMPCAR1's real gearing.

**Gameplay rules (2026-08-10):** scoring/boost/takedown/out-of-control parameter tables and rules — registrars FUN_00190430 + FUN_0017A0F0, boost record at racecar+0x119C, takedown chain FUN_001989A0→FUN_00197430→FUN_00197040→FUN_00198E60 — recovered and execution-verified (tools/validate_gameplay.py 44/44); full evidence in docs/RE_GAMEPLAY.md.

## 12. Coordinate handedness + collision (2026-08-10, session close-out)

[C] The game world is D3D left-handed; rendering it through GL's right-handed
camera mirrors the image (billboard text read backwards, verified against the
decoded bk_billboards2 texture). Fix: one uniform reflection (negate Z),
applied in trackmesh_load (all mesh data: track + cars) and init_paths (all
path arrays). GL output now matches the original's chirality (billboard
re-verified readable in-app). Car meshes face their motion after the flip.

[C] Collision is now against the rendered track mesh itself: all steep
(|n.y|/|n| < 0.45, height > 0.25) triangles' edges in a 16m uniform grid
(~120k segments), push-out with margin 1.0. This replaced the .bgd road-edge
strip as "walls" -- on dual carriageways that strip runs mid-road (it bounds
the corridor, not the barriers), which had produced invisible mid-road walls.

[C reconciled] The X road-closure barriers (texture "Barrier") sit at
side-street junction mouths along the forward race line; an earlier session
note inferred from them that the 1029 loop was a different variant's route --
falsified by docs/RE_BGD.md's directory decode (start grid, mode records).
The real cause of AI pile-ups at those junctions was the harness AI's
corner-cutting; fixed with curvature-based braking + stuck-recovery reverse.

---

## 11. Session addendum (2026-08-10, physics) — tyre grip, rigid-body integrator, steering, the −20 term

(Appended after §12 chronologically; numbered 11 as the physics slot left for
this work stream.) Everything below is [C] confirmed by differential execution
unless marked. tools/validate_port.py grew 49/49 → **73/73**: +10 tyre/airborne
cases (+1 LSDM consumer), +8 integrator, +5 steering, and the resistance cases
now assert **all four** accumulator components (the old KNOWN GAP is closed).

### Where tyre grip actually lives [C]

**FUN_0011D460's per-wheel loop (0x0011ddf0..0x0011e675)** — not FUN_00123FD0
(suspension only, §10) and not FUN_00123000. FUN_00123000 is the *solver
dispatcher*: it applies `acc += dir_c0 * (-speed²)` drag, crash/rollover
damping, then runs FUN_00123FD0 + FUN_00109560 once (or twice at dt/2 when the
byte at its +0x1b stack flag is set), with FUN_00126520 per crashed-wheel.

Per wheel i (0..3 always; wheels 4/5 of 6-wheelers copy the rear omegas):

* wheel world point = frame ⋅ (wheel_frame_pos.x, **attach height** (+0x74),
  wheel_frame_pos.z) (FUN_00013CA0; wheel frames from v+0xCC8..)
* point velocity vp = ω(+0xD0) × (pt − pos) + **velocity** (+0xB0)
  (FUN_001066A0). NOTE: +0xB0 is the true velocity *vector*; +0xBC its
  magnitude; +0xC0 the unit travel direction (written by FUN_000FFC80).
* basis: front wheels use the frame rotated about local up by the steering
  angle v+0x1164 (deg) — FUN_00011900 axis-angle → FUN_000116E0 multiply,
  cached at v+0x14E0 (right) / v+0x1500 (at). Rears use the frame itself.
* vlat = lat·vp, vlong = fwd·vp, roll = ω_wheel(+0x5C) · radius(+0x50);
  front wheels then free-roll: ω_wheel := vlong / radius.
* slip = √(vlat² + slip_long²), slip_long = vlong − roll
  (ε: roll==0 → 1e-6, slip==0 → 1e-7)
* per axle: front stiff=150000, denom=60, curve=(1−throttle)·2.5+2.3;
  rear t=clamp(speed·0.025, 0.5, 1), stiff=t·500000, denom=12,
  curve=t·((1−throttle)·7+6)  ← throttle-controlled oversteer
* x = 0.4 − 0.8·min(1, |stiff·slip / (denom·roll·(mass·5))|)
* **F_scalar = (−1/slip)·( [ (x−0.4)·cos0.4 + sin0.4 − sin x ]·curve·(mass·5)
  ·23.8164  +  ((x+0.4)/roll)·stiff·slip·0.5 )**
  — a sine-curve friction law (gap between sin x and its tangent at 0.4),
  normalised to peak 1.0 by the BSS constant **0x005A8054 = 23.8164**
  (static-init stub 0x002BA3C0 ← float at 0x3B2338; must be seeded when
  emulating, it is BSS-zero otherwise and lateral grip vanishes).
* F_lat = vlat·F_scalar applied along lat at wheel_pt + up·SteerForceHeight
  (0x1370); F_long = slip_long·F_scalar applied along fwd at
  pos + up·AccelForceHeight (0x136C). Wheel reaction: torque −= F_long·radius,
  ω_wheel += torque·0.04·dt, torque := 0.
* drift state (+0x1524 = 1/2): lateral force blends to a scripted magnitude
  `(1−slide·⅔)·(dir·at·6400+4200)` by the slide amount (+0x1440), applied at
  DriftForceHeight (0x1374); afterwards the up-axis component of the loop's
  torque is REPLACED by `(1−slide·0.75)·TurnRate·steer·|steer|·(−10000|−5000)`
  (±1000·v+0x1414 fallbacks), capped by 800/TurnMomentum(fast→slow, 60–110
  mph, ×(t+0.75) in the first 0.25 s) against the up-axis angular momentum
  (over-cap ×0.1); plus an anti-slowdown block that cancels the backward
  component of the summed lateral force and re-aims it along up×dir.
* drift-state exits: (t>0.5 s ∧ |dir·right| inside ±0.01 ∧ |steer| ≤ 0.005),
  or (throttle<0.5 ∧ brake<0.5 ∧ steer==0 ∧ mph<40). Entry from a sideways
  landing: airtime>0.1 ∧ |dir·right| > cos(90−MinDriftAngleInAir) (0x13DC).

Also in D460: brake/engine-brake `((EngineBraking·offthrottle −
BrakingFactor·brake·20000)·(speed+1)/70)` along dir at
pos+up·BrakeForceHeight (0x1368); the resistance/vertical blocks already
ported in §8.2 (their gates confirmed against the disassembly).

### The −20.0·dir.x term — SOLVED [C]

It was the **airborne attitude dampers** (0x0011D75B..): four compiled-in
records, two stations z = ±(v+0x1D8·0.5) / (v+0x1E8·2.0) (z negated when
dot(at, vel) < 0, i.e. flying backwards), each with an up-axis damper gain
**1000** (0x3B16CC) and a right-axis damper gain **10** (0x3A7F34):
`F = −gain · dot(axis_w, vp) · axis_w` at the record's world point. In the
synthetic identity environment (offsets 0, ω 0) that is exactly
X += −2·10·vel.x and Y += −2·1000·vel.y — the "−20.0·dir.x" recorded in §8.2.
Both are now modelled and asserted in every resistance case. The airborne
branch also: corkscrew-damps +0xE0 by `E −= at·(at·E)·v+0x13D8`
(FUN_0011F800; 0x13D8 ← config +0x1B0 In Air Corkscrew Damping), applies the
verified vertical force, splits drive torque, and returns (no gravity pin).

### Gravity is 20, then pinned to the body [C]

`DAT_0040A8A0 = (0, −20, 0, 0)`. FUN_00109560 applies it world-vertical every
step (×mass; with a torque about pos+up·(v+0x1F4) while in-race). The tail of
FUN_0011D460 (0x0011EC55), when grounded ∧ speed>1 ∧ frame at.y>0, removes
20·mass of world-vertical and re-applies it along the body's −up — the car is
pressed into banked road surface instead of straight down. So: −20 world when
airborne-ish (§8.2's −10 vertical force is *additional*, airborne only), −20
along −up when grounded. Ported as b3_gravity_body_pin.

### Rigid-body integration — FUN_00109560 [C] (8 cases)

+0x10 = **body-space inverse inertia** rows; +0x40 = world inverse inertia
(rebuilt each step as Rᵀ·I₀·R via FUN_00109040 ×2); +0x70 = inverse frame
transform (FUN_00040AE0: transpose + pos = −pos·R). Steps: gravity as above →
impulses +0x110/+0x120 += acc·dt (acc cleared; the w-lane dt slot is an
*uninitialised stack dword*, 0 under emulation) → vel(+0xB0, 4-wide) +=
imp/mass, vel.y capped at **120** → L(+0xE0) += imp_T → ω(+0xD0) = L·M →
|ω|>100 squashes ω to 100/|ω| and L ×= 0.95 → pos += vel·dt (+ deflection
+0x130, cleared) → rows −= row × (ω·dt) → FUN_000FF270 re-orthonormalise →
FUN_000FFC80 refreshes speed=|vel| and dir=vel/|vel| (at-row when stopped).

**FUN_000FF270 quirk [C]:** its row-selection dot products call FUN_00013C60,
which is a **no-op stub** in the retail binary, so the branch reads stale
stack. Standalone it anchors one way; called from FUN_00109560 (traced) it
takes the at-anchored path: normalise rows, row1 = norm(at×right),
row0 = norm(row1×at). The port mirrors the in-context path.

### Steering consumers [C]

* **FUN_0011ECF0** (input stage): live 0x1378/0x137C/0x1380/0x1384/0x1388 ←
  config +0x148..+0x158. Angle = clamp(0x1384 − (speed−0x1380)·1.4,
  min 0x1378, max 0x137C) — the "velocity" params are really (speed offset,
  base angle). Input +0x1408 slews by ≤ SteerResponse (0x1388) per frame;
  **v+0x1164 = −(steer·angle)** degrees. 5 cases green.
* **Aggressive steering** (+0x1C4/+0x1C8/+0x1CC → live 0x13E8/0x13EC/0x13F0):
  during the steer-away reaction (clocks behind +0x13F4→+0x1198 within
  SteerAwayTime/TotalOutOfControlTime windows) they *replace* the live
  0x137C/0x1384/0x13C0, and steer is forced to ±1 by byte +0x153C; byte
  +0x215==3 restores the normal trio from the config behind +0x13F8. [S] only
  the mechanism; not separately emulated (needs the owner-object clocks).
* **LSDM — FUN_0011C7C0** (speed·2.2374 < LsdmSpeedLimit 0x13AC, or reverse):
  a 4-substep (dt/4) bicycle model. Steering angle = −LsdmSteeringAngle
  (0x13B0)·steer [C, asserted]; yaw damping −(LsdmTorque1−|steer|)·LsdmTorque2
  ·yaw_rate/(v+0x24·dt) (v+0x24 = yaw inertia); slip forces via fpatan curves
  capped at 3°/8°; engine braking (ω/maxrpm)·EngineBraking·0.5; front wheel
  omegas rewritten |vp|/radius (sign = reversing). Located + mapped, **not
  ported** (equations sketched here; the model is self-contained in c7c0).

### Ported this session (all in burnout3_vehicle_sim.c's marked section)

b3_tyre_grip, b3_tyre_wheel_reaction, b3_airborne_damper, b3_gravity_body_pin,
b3_brake_drag_scalar, b3_drift_lateral_blend, b3_drift_yaw_torque,
b3_yaw_torque_cap, b3_steer_schedule, b3_steer_slew, b3_rigid_body_integrate
(+ b3_mat_orthonormalize and rigid-matrix helpers). GLUE into b3_vehicle_step:
b3_steer_angle now delegates to the verified b3_steer_schedule, and the
brake/engine-brake reconstruction was replaced by b3_brake_drag_scalar.

### Still open here

* the flag_b (+0x1446) takedown/recoil block inside the wheel loop
  (HEVYCAR9/HEVYCAR36 ID compares, ×4/×8 F_long, −right·Δt recoil) — located,
  unported; every tyre case gates it off
* FUN_0011C7C0's bicycle model (above) — the last unported drive model
* FUN_0011AEF0 is NOT the pose integrator (that is FUN_00109560): it is the
  ground-collision/crash response — per-poly FUN_0011AC30 tests, a
  −1000·mass·dir wall stop, impulse via FUN_00106720, deflection into +0x130,
  velocity scrub (×0.99 / surface-grip ×(+0x13A8)), crash triggers. Mapped,
  unported.
* FUN_00118410, FUN_0010DD20, FUN_00126520 (crashed-wheel pass) untouched.

## 13. Session addendum (2026-08-10) — traffic runtime + opponent AI

Full write-ups: docs/RE_AI.md (AI drivers, 73-param registrar, governor) and
docs/RE_BGD.md sections 4-6 (the traffic data this session put on the road).
Suites after integration: validate_port.py green, validate_gameplay.py green
including the new 6-case "AI driver" section (FUN_00105340's driving path
executed under Unicorn, asserting the input writes).

Traffic (#15):
* tools/extract_traffic.py -> src/burnout3_traffic_data.h: C1_V1 mode block 0
  traffic set (11 ids: COMPCAR14/12/17, HEVYCAR14/15/12/18/22/21/26/28 +
  special TSPCCAR2 at block+0x18), the 71x0x20 {pos,dir} spawn table
  @0x1C450 (located structurally, longest valid-record run in the block),
  and the 926-pt reverse loop @0x1A8540 as the oncoming line. All points
  z-negated to the harness GL space (section 12).
* [C] The .btv files parse with the SAME relinker layout as .bgv
  (extract_bgv.py's section/part/vertex walk): 12/12 traffic meshes
  extracted with car-scale bounds and in-range indices, 12/12 paint sets via
  the .bgv texture record layout. Only the length plausibility gate needed
  relaxing (TSPC specials reach ~52 units).
* Harness: 12 traffic cars spawn from the real table and drive the oncoming
  loop (ascending index = against race direction, opposite winding [S]);
  speed band thresholds are the real traffic driver's (FUN_00105150: +1 m/s
  throttle gate, 5 mph brake excess [C]); the kinematic mover is GLUE. Hard
  racer contact wrecks the smaller car by the real masses (racer 64-param vs
  traffic 9-param VDB config); racer-side crashes reuse the verified slam
  fields + 5 s recovery. The TSPC special is extracted but not driven (its
  routing is its own system [?]). Capsule contact for the long heavies —
  a bounding sphere on a 10-unit bus shoved racers off the adjacent lane.

Opponent AI (#16), the short version (details in RE_AI.md):
* Registrar FUN_0016AFD0, static config 0x0047A140, 73 params under
  "../Export/ValueDB/AI/defaults.cfg", all hash-mirrored and resolved in
  Data/vdb.xml (emulated with the extract_car_vdb scaffolding).
* Driver dispatch FUN_00104D30: +0x134C==0 -> FUN_00105150 (traffic),
  +0x179C==1 -> FUN_00105340 (AI racer), else FUN_00104E20 (attract).
* FUN_00105340 verified [C]: steer = clamp(-tgt_deg * MaxLock/180); throttle
  above 1 m/s deficit; brake above exactly 30 mph excess; boost bit
  v+0x13FC=4; stuck-reverse 5 mph / 1.0 s / 2.0 s; plus the pre-existing
  OOC authority cases.
* Rubber-band governor [C-disasm]: unanalyzed mover tail 0x171078 pulls
  v+0xBC toward racecar+0x23C4 through FUN_001204C0 — down by "Out of range
  speed decrease rate" (VDB 10), up by speed_mph*0.0010309 per step, snap
  inside the band. Mode/step-scaling unresolved -> only the down-pull is in
  the harness (dt-scaled, marked [S]).
* Still open: the writer of the AI targets racecar+0x23C0/+0x23C4/+0x2419
  (no direct .text references outside the drivers — derived-pointer writes
  in the unanalyzed 0x16F000..0x171100 region).

## 13. Crash/damage state machine + damage visuals (2026-08-10, task 13)

Everything below is [C] execution-verified (tools/validate_gameplay.py, the
"damage scan" / "distributor" / "crash sequencer" / "glass" sections, 55/55
total) unless marked [S] (read from decompile/data, self-consistent, no green
case) or [?] (open).

### The damage/visual context object [C]

`ctx = *(vehicle + 0xCC4)` — the "aux object" from section 10's suspension
notes IS the per-car damage/visual context (proved by FUN_000303D0's
vehicle-array scan matching `*(veh+0xCC4) == param_4`). Recovered layout:

| ctx offset | meaning |
|---|---|
| +0x20  | per-wheel 4x4 matrices, stride 0x40 (draw + debris) |
| +0x180 | per-panel live matrices, stride 0x40 (detached panels) |
| +0x324 | grip scalar (pre-pass writes 0.2/1.2 — section 10) |
| +0x4AC | **wheel state bytes**, one per wheel (count = veh+0x1169) |
| +0x4B2 | **panel state bytes**, one per body part (count = file+0xC) |
| +0x4D0 | per-wheel f32 gate for detach (> 0.05 required) |
| +0x500 | per-panel deformation matrices (FUN_00031E70 path) [S] |
| +0x700 | per-panel attach matrices, copied from file+0x70 [S] |
| +0xF78 | per-wheel impact accumulators f32[6] |
| +0xF90 | per-panel impact accumulators f32 |
| +0xFC0 | per-panel crumple thresholds f32 (randomized at init) |
| +0x1014 | health tier byte: 1 (health<0.95) / 2 (health<0.7) |
| +0x1022 | "any part detached" draw gate (FUN_000314F0) |
| +0x1023/+0x1024 | burst flag / aux model ptr (FUN_00125AC0) [S] |

### Panel/wheel states and every located writer

States: **0** pristine, **1** (read everywhere as "like 0"; no writer located
[?]), **2** crumpled, **3** detached (a flying-part pool record animates it),
**4** gone.

* **FUN_00123000 (head)** [C] — per-frame accumulator scan (the same function
  whose tail is the tyre-grip block):
  - panel: `acc(+0xF90) > threshold(+0xFC0)` and state 0/1 → 2;
    `acc > DAT_005A80C8` → detach via FUN_00125A50.
    DAT_005A80C8 = **999.0** (static init 0x002BA780).
  - wheel: `acc(+0xF78) > 0.3` and state 0/1 → 2; `acc > 0.5` and state == 2
    and `ctx+0x4D0[i] > 0.05` → state 3 + pool alloc, **reverted to 2 if the
    allocation fails**. Constants 0x3B1750 / 0x3B1684 / 0x3A69BC.
  - also computes veh+0x1168 (all-wheels-airborne flag) and the quadratic
    drag add into veh+0xF0..0xFC (verified in the same cases).
* **FUN_00125A50** [C] — panel detach: state 2 → 3 stamped BEFORE the pool
  alloc `FUN_00111340(&DAT_0064B310, veh, idx(ESI), kind=1)` (stdcall ret
  0x10); alloc failure reverts to 2; success runs FUN_00106F20 (flying-part
  init) + FUN_0014FC30(rec+0x204+0x30, 100.0).
* **FUN_00023DE0** [C] — per-frame health-driven distributor over all cars
  (slot table 0x0064B38C stride 0x30, count 0x0073A19C): reads health
  `racecar+0x16C4`, writes ctx+0x1014 tier (2 if h<0.7 else 1 if h<0.95,
  constants 0x3B17D8/0x3A69B8), then dents/detaches panels walking the
  priority table **DAT_00385224 = [3,6,0,1,5,4,2]** (panel kind ids from
  file+0xAC4) until `intact = int(nparts * min(1, h+0.1))`. Kinds
  {0,1,3,5,6} are dented but never detached by this path (the kind switch
  at 0x23FFB; kinds 2, 4 and >6 detach).
* **FUN_001253C0** [C data, S caller-chain] — total-wreck stamp: all panel
  accumulators := 1000.0 (> the 999 threshold → everything detaches on the
  next FUN_00123000 pass), wheel accumulators := 2.0, all states floor 2.
* **FUN_00115FC0** [S] — detached→gone: when a flying-part (type-7) record
  in the 0x30-stride active list retires, every state-3 panel of that car
  → 4.
* **FUN_00111340** [S] — flying-part pool (64 slots, stride 0x4E0, bitmask
  +0xE9C98/9C): on slot steal the stolen part's state is force-written to 4
  (kind byte slot+0x2BA: 0 wheel / 1 panel).
* **FUN_0012FEE0** [S] — ctx init: states 0, panel crumple thresholds
  RANDOMIZED per car (PRNG × 0.17 band at +0xFC0, plus 0.1/0.2 bands at
  +0xFA8/+0xFD8), wheel matrices MOVAPS-copied from **file+0xB80**, panel
  attach matrices from **file+0x70** into ctx+0x700 (defaults 0x3F8110).

### Health (racecar+0x16C4) [C]

1.0 = pristine. Writers: **FUN_000241A0** reset to 1.0 (respawn);
**FUN_00023C20** graded damage `health -= table[type]` with
DAT_004D58D0[9] = {0.04, 0.04, 0, 0, 0, 0, 0.025·dt, 0.15, 0.15} (static
init 0x00259E40; types 4-6 dt-scaled; types 7/8 cannot take health below
~0.01 unless already < 0.1) — a racecar vtable method (15 vtables);
**FUN_00025CC0** crash sequencer: crash counter +0x16C8 ticks down, health
-= 1.0 (clamped 0), then **reset to 1.0 unless the counter hit 0**.
Net-sync writers FUN_00101120/FUN_001012B0 (byte/255 from packets) [S].

### Crash chain forward link [S chain, C endpoints]

Collision resolver FUN_00111CD0 → severity gate FUN_0010E690 (impact energy
param+0x20 vs 2400/10000/20000, 70 mph gate, probability rolls) →
FUN_0010EC10 → FUN_00115130 → FUN_001253C0(0) wreck stamp + debris burst;
FUN_00120800 is the explode variant (-Y impulse 100000 + same stamp). The
slam chain (RE_GAMEPLAY §3, victim+0x1598) feeds the same crash entry via
the crash state machine that calls FUN_00025CC0.

### Glass dressing FUN_000300A0 [C]

EAX = part header (byte+1 = glass record index into the 0x1C-stride record
array at +4), EDX = tier (= ctx+0x1014): tier 1 → record texture slot 3,
tint 0.5; tier 2 → slot 4, tint 0.6; else slot 2, tint restored from
record+0x8. I.e. the paint directory's slots 2/3/4 are
intact/cracked/shattered glass rasters.

### .bgv part/damage layout (extends BGV_EXTRACTION.md) [C]

* File header: +0xC u8 numBodyParts, +0xD u8 numWheels, +0x18 f32 wheel
  radius, +0xAC4 i32[] panel kind ids.
* **+0x70: panel attach matrices**, 4x4 stride 0x40 (copied to ctx+0x700 by
  FUN_0012FEE0). Which matrix belongs to which panel slot is **[?]** (NN
  matching against the intact hull was inconclusive).
* **+0xB80: wheel matrices**, 4x4 f32 stride 0x40, rows Right/Up/At/Pos
  [C twice: raw positions mirror L/R across four classes (e.g. COMP Car1
  ±0.760/±1.238 front, ±1.307 rear; right-side wheels have negated
  Right/At rows = 180° about Y), and FUN_0012FEE0 MOVAPS-copies exactly
  this array into the ctx wheel matrices the draw path consumes]. The
  community B3VehicleDataParser layout (+0xB80, Matrix3x4, stride 0x30) is
  the PS2 file layout — on Xbox files it reads garbage, which is what
  falsified the earlier attempt.
* Each LOD section base IS the 10-slot part table (relinked by
  FUN_00031010): slot 0 aux object, slots 1..numBodyParts damage panels
  (PIVOT-LOCAL vertices), remaining slots wheel meshes (origin-centred,
  diameter = 2×file+0x18), slot 9 far-LOD wheel. All slots' records index
  the ONE section vertex pool (S + *(S+0x4C)) — this closes section 9's
  open question about per-part vertex bases.
* Record mask semantics (u16 at record+0x18): bit0 intact variant, bit1
  damaged variant (body: shell-minus-panels + underbody, WITH interior/
  driver), bits2..7 lights, bit8 glass. [S] The mask-bit0 body is NOT
  panel-complete (e.g. COMP Car1 bakes the left door but not the right);
  the intact car on screen = body records + panel slots drawn at their
  attach matrices, which is why the whole-record union looks right.

### Extraction + harness (tools/extract_bgv.py, src/burnout3_full.c)

extract_bgv.py now also emits, 67/67 cars: `<car>_intact.obj` (bit0+lights),
`<car>_shell.obj` (bit1+lights — the wreck), `<car>_glass.obj`,
`<car>_wheel.obj` (origin-centred wheel mesh), `<car>.wheels` (radius +
attach positions + mirror flags), and `parts/<car>/panel<K>_kind<D>.obj`.
Wireframe verification: panels are recognizable doors/fenders/bumpers with
window cutouts, wheels are spoked discs, the shell exposes the modeled
interior + driver figure.

Harness (car mesh load/draw only): wheels render as separate meshes at the
real attach positions (loader Z-flip applied), spinning via the ported
FUN_00123FD0 spin law (b3_wheel_spin_update; omega = speed / real per-car
radius is GLUE, as is the front-axle steer pose); cars ground through
wheel radius instead of the OBJ min-y (which included pivot-local panel
verts). A wreck (crashed_until running — the GLUE crash trigger) swaps the
body to the bit1 shell + glass tinted with the verified 0.6, matching the
machine's end state (wreck stamp → every panel past the 999 threshold →
detached → gone). Respawn restores the intact body (FUN_000241A0 resets
health to 1.0).

### Open [?]

* Writer of panel state 1 (never observed; all logic treats 0/1 alike).
* Panel slot ↔ attach matrix assignment (file+0x70 order), hence no
  per-panel articulated rendering / detach animation in the harness yet.
* The per-panel deformation renderer FUN_00031E70 (ctx+0x500 matrices) and
  the flying-part simulation (FUN_00106F20 / pool record layout beyond
  +0x2B0 owner / +0x2B8 index / +0x2BA kind).
* Which impact events feed the accumulators at ctx+0xF78/+0xF90
  (FUN_0012A210 adds impact×0.25 to the kind-4 panel; FUN_00129D00 ±40×dt
  to door kinds 0/1; the general collision→panel mapping in
  FUN_00126D40/FUN_0012BEB0/FUN_0012C860 is unread).

### 13.1 Mid-route stall diagnosis + harness fixes (same session)

The known autodrive slowdown (mean sinking toward ~23 mph) traced to FOUR
separate causes, found with the new headless test bed
(`SDL_VIDEODRIVER=offscreen B3_FIXED_DT=0.0166 B3_TELEM=1` -- deterministic
60 Hz steps at >1000 fps wall):

1. [FIXED] prog 0.427 terminal stall: the junction there is a DECK over
   lower structure; mesh_collide's +1.2 upper y-margin treated geometry with
   its top up to 0.7 m BELOW the wheel contact as a barrier, walling the
   road off completely. Margin corrected to +0.45 (still catches kerbs);
   the collision segments now also carry the EDGE's own y-range instead of
   the whole triangle's (a 90 m facade's top edge no longer blocks at road
   level).
2. [FIXED] wall-grind blindness: cars grinding a barrier at ~6 m/s never
   trip the real 5 mph stuck rule. Added a position-based detector (< 6 m
   moved in 2.5 s), a kinematic reverse (the reconstruction's scalar speed
   clamps at 0, so gear -1 cannot move the car through b3_vehicle_step),
   and an alternating lateral escape aim. The 5 mph arm counter no longer
   cancels an ACTIVE reverse.
3. [FIXED] median-slot routing: on dual carriageways the corridor midline
   runs down the median (a 2 m rail slot under the prog ~0.49 flyover).
   route_lane_fixup() shifts each route point laterally toward the forward
   race line (cap 5 m, skip where it diverges >12 m -- the roundabout
   chord); 210/876 points move. Aim rays are additionally checked against
   the collision grid (aim_blocked) with race-line fallback.
4. [OPEN] prog 0.489 pillar gap: the flyover underpass is a single-file
   passage flanked by pillars; the real race line threads it (nearest point
   1.4 m). Two of six cars lap cleanly; the trailing pack contends there
   (repeated 5-10 s jams, no permanent freeze). Queuing (racers and traffic
   hold behind near-stopped cars ahead) and a stationary-racer rule
   (traffic wrecks itself on a pileup instead of re-wrecking the recovering
   car) reduce but do not eliminate the contention. Next lever: single-file
   merge ordering by race position, or per-slot lane offsets through the
   mouth.

Autodrive after the fixes (deterministic 180 s, 6 cars + 12 traffic): pack
mean ~16 m/s (~36 mph) including the contention zone, top speeds 155+ mph,
two cars complete ~full laps; the windowed 60 s acceptance run holds
58-62 fps with traffic visible.

### 12.1 Render-yaw sign (the recurring "crooked car" reports)
[C] The harness heading convention fwd=(sin h, -cos h) combined with
glRotatef's CCW-about-+Y rotation renders a -Z-nose mesh mirrored in X when
rotated by +h: the visual yaw error is 2x the heading angle, so cars looked
straight at some track headings and up to fully sideways at others (every
"crooked/facing wrong" report traced here; the old +180 offset partially
masked it at spawn headings near the axes). Fix: render rotation = -h for
bodies (players + traffic). Verified by camera-on-heading shots: dead-ahead
camera sees headlights/windshield. Also: wheel-slot record masks do NOT
follow body damage semantics -- bit0 = rim/spokes, bit1 = tire (full wheel
needs all records of the slot); traffic .btv meshes now use intact-record
selection like players.

## 15. The collision world: format + query chain (2026-08-11)

Everything below is [C] unless marked. [C] here means execution-verified:
tools/validate_gameplay.py's 'collision' section (15 cases) runs the game's
own walker/gather/ray functions under Unicorn over the real streamed.dat
blocks and asserts exact agreement with tools/extract_collision.py's parser,
a pure-Python mirror, AND the compiled src/burnout3_collision.c.

### Where collision lives

Vehicle collision (wheel rays + body) runs against per-unit **kd-tree poly
soups embedded in streamed.dat's unit LOD blocks** — NOT in static.dat, the
.bgd, or a separate file. Each unit table entry (static.dat +0x54 count /
+0x58 table, 0x10-byte entries {sub_off, lod_off, sub_size, lod_size}) names
a LOD block that is a self-contained record, relinked in place after load:

```
lod block (at lod_off, size lod_size)
  +0x00  u32   state: 1 on disk, streamer stamps 2 when resident
               (FUN_0019d3b0 requires *rec == 2)
  +0x04  u32   unit index          +0x08 u32 block size
  +0x50  {u32 1, u32 offA, u32 0, u32 offB}   far-LOD render sections
  +0x70  f32[12]  FOUR XZ HALF-PLANES bounding the unit:
                  inside iff a[i]*x + b[i]*z - c[i] >= 0 for i=0..3
                  (a=+0x70[0..3], b=+0x80[0..3], c=+0x90[0..3]);
                  FUN_0019d7f0 is exactly this test [C numeric + disasm]
  +0xA0  u32   COLLISION HEADER offset  (0 = none; all 49 C1_V1 units have one)
  +0xA4  u32   second relinked section offset (not collision)
```
`FUN_0019d7a0` is the block relinker: `+0xA0/+0xA4 += base`, then
`FUN_001b02b0` relinks the collision header (below). The per-frame unit
lookup `FUN_001ad4a0` scans current-unit-4 .. +4 with the half-plane test
and returns the unit index; `*(world+0x17c + unit*4)` (world objects at
0x007397c8, dword-stride 0x73, 2 instances selected by veh+0x217) yields the
resident block, and `[block+0xA0]` the collision header.

### Collision header + kd-tree (relinker FUN_001b02b0)

```
header (16-aligned)
  +0x00  f32[3] bbox max      +0x10  f32[3] bbox min
  +0x20  u32    kd-node array offset (header-relative -> pointer)
  +0x24  u32    leaf array offset    (relative -> pointer)
  +0x28  u16    leaf count           +0x2A  u16 node count
  +0x2C  u32    leaf format flag: 1 on every retail unit = quantized path
                (flag 0 would select FUN_001b1d40's float3-vertex leaves)

kd node (0x10 bytes, two planes per node -- traversal FUN_001aff70):
  {f32 split, u16 child/leaf index, u8 axis, u8 leaf_flag(0xFF=internal)} x2

leaf record (0x10 bytes; relinker adds &rec to +0x00/+0x04):
  +0x00  i32 prim data offset (self-relative)
  +0x04  i32 vertex data offset (self-relative)
  +0x08  u16 ?          +0x0A  s8[3] cell offset x,y,z
  +0x0D  u8  prim stride (0x0E)   +0x0E u8 prim count   +0x0F u8 vert count

vertex: u16[3], stride 6 (FUN_001b0f00):
  world = u16/65536*1000 + cell*500        (per-axis, cell from leaf +0xA..C)

prim (stride 0x0E):
  +0x00  u8 i0,i1,i2,i3   (i3 == 0xFF -> triangle, else quad)
  +0x04  u16 surface type
  +0x06  u16[4] extra (unread by the wheel/body chain)
```
Quad decomposition (FUN_001b2940, verified by hooked-callback enumeration):
tri1 = (i0,i1,i2), tri2 = (i2,i1,i3); normal = normalize(cross(v1-v0,v2-v0))
(FUN_001b0fe0 + FUN_00011640). C1_V1: 49/49 units, 67,547 triangles, bounds
x[427..2309] y[-33..618] z[104..1778] (enclosing the route; y 618 = the
~420 m tall out-of-bounds sky-walls, y -33 = riverbed). Exact triangle-set
equality parser-vs-game on units 0/24/40 (enum cases).

### Per-frame gather -> the vehicle soup at veh+0x200

`FUN_00122d00` (per vehicle, gated by +0x210 in-race and +0x1353):
points veh+0x200 at the global soup {u32 count @0x005a3aa0, recs
@0x005a3ab0 (0x40 stride, cap 0x60=96, overflow byte 0x00478a30), u16 types
@0x005a52b0}, calls `FUN_00109d20` (unit lookup + `FUN_0019d3b0` +
`FUN_001aff70` walk with callback **0x00109ce0**), then appends extra
records: 4 or 6 planes from veh+0x11f0 stamped **type 0x26** (crashed-car
surfaces — this is where the low-grip type comes from; the static track
never carries 0x26) and veh+0x15b0/+0x2030 records stamped type 0x21.
Then FUN_00109ea0 (body vs soup via FUN_00107950/FUN_0010aad0),
per-crumpled-panel FUN_00126d40, the wheel pre-pass FUN_001239c0, and
FUN_0010ed30.

Callback 0x00109ce0 **skips prims whose surface-type low byte is
0x20/0x22/0x23/0x24** (they never enter the soup; on C1_V1 those are 624
type-0x0020 prims + the 0x8124/0x0124 tall boundary walls — camera/netcode
geometry). Appender FUN_0010A8E0 copies the expanded prim into the soup:
rec = {v0 @+0x00, v1 @+0x10, v2 @+0x20, normal @+0x30}, type u16 from
*(prim+0x60)+4. (Section 10's "two edge vectors" reading of +0x10/+0x20 is
corrected: they are vertices.)

Soup record layout is one-sided by construction: the ray/pole tests only hit
front faces (det > 0), and the DATA relies on it — fences are PAIRS of
offset one-sided faces (~0.3 m apart, opposite windings), the tall course
walls single one-sided quads passable from outside.

### The ray core FUN_001b2230 [C]

Moller-Trumbore over a SEGMENT, one-sided, with compiled-in constants
(0x003a6860 / 0x003b16a0 / 0x003b1918 / 0x003b168c):
det = (v1-v0)·((B-A)×(v2-v0)) must be > 1e-8; u,v,u+v and t (all
det-scaled) must lie in (-1e-5*det, 1.00001*det]; outputs t/det, u/det,
v/det. Registers: EAX=segment{A[4],B[4]}, ECX=v0, stack v1, v2, &t, &u, &v.
FUN_00013c60 is the 3-dot helper.

### The wheel ray FUN_00123790 [C]

(rayA, rayB, &out_index, &out_dist; ESI=vehicle). Per soup record:
optional crash-mode (veh+0x215 in 1..3) surface filter (only low byte <0x15
or ==0x26 pass), optional slope gate when veh+0x210 (normal·frame_up >=
0.25), FUN_001b2230, winner = MINIMUM PARAMETRIC t; out_dist = t * |A-B|.
Side effect: ctx(veh+0xCC4)+0x324 grip = 0.2 on a type-0x26 record, reset
to 1.2 by a later non-0x26 record (order-dependent). FUN_001239C0
additionally runs an under-body ray (frame pos, 30 straight down, no
filter) writing ctx+0x49C = t*30, ctx+0x490 = normal, veh+0x1160 = type
(10000.0 sentinel when airborne).

### Extraction + reimplementation

* tools/extract_collision.py -> build/collision.bin (custom format, header
  'B3CL' + 40-byte tris {v0,v1,v2,u16 type,u8 unit,u8 excluded}), raw game
  coordinates; overlay build/collision_overlay.png (road/wall/excluded over
  the render mesh -- visibly aligned with the drivable circuit).
* src/burnout3_collision.c/.h: loads collision.bin (mirrors into harness GL
  space, v1/v2 swap preserves the one-sided orientation), 8 m XZ grid.
  b3_ground_probe(x,y,z) = the under-body down-ray (30 down from y+2) with
  the exact FUN_001b2230 constants, returns surface type / -1;
  b3_sweep_sphere = one-sided sphere-vs-wall-triangle contact (the game's
  own body path FUN_00107950/FUN_0010aad0 is unported; the sweep is marked
  GLUE over real data).
* Verification (validate_gameplay 'collision', 15 cases): 3 enum units
  (exact set match) + 12 probes (route points incl. the prog-0.43 deck, the
  prog-0.49 underpass BELOW it at the same x,z from deck height, kerb,
  riverbed, and two agreed-miss positions) — game gather+ray under Unicorn
  vs Python mirror vs compiled C: hit/miss, distance, winning normal +
  surface type, and the grip write all agree.
* Harness: build_collision loads collision.bin and rebuilds aim_blocked's
  2D barrier grid from the real wall polys (render-mesh steep-triangle
  fallback retained); mesh_collide = b3_sweep_sphere push-out; the full
  vehicle pipeline's wheel rays consume b3_ground_probe via
  harness_ground_probe (game-space -> GL boundary there). In-game:
  build/collision_deck.png (car ON the flyover deck), collision_underpass.png
  (cars on the lower road UNDER the deck at the same x,z -- the old
  single-height route follow could not represent this), collision_wall.png
  (car stopped in contact with a barrier island, no clipping).

### The RACING gather's two RUNTIME filters — FUN_0011BBE0 [C-disasm]

`FUN_00122D00`'s callback `0x00109CE0` above is the WRECK gather. The RACING
one is `FUN_0011BBE0`, pushed at `0x0011BD90` by `FUN_0011BE50` into the same
walker. Its static tests are `low == 0x23`, `low == 0x22` and `type & 0x1000`
(`0x0011BBFE` / `0x0011BC03` / `0x0011BC08`) — NOT the wreck gather's
0x20/0x22/0x23/0x24 — plus two RUNTIME tests:

```
0x0011BC0D  0x15 <= low <= 0x20       (the STRUCTURE band)
0x0011BC2D     j = dot(record normal at +0x10, *(vec3*)(veh + 0xB0))
0x0011BC32     j > 0.5   [0x003B1684]                 -> skip
0x0011BC43  -0.7 [0x0039B264] > normal.y              -> skip   (all types)
```

`veh+0xB0` is the car's OWN velocity, so (a) drops the structure faces the
car is already separating from — the anti-snag that keeps a paired one-sided
armco's back plate out of the soup. Ported into `b3_sweep_sphere_ex`
(`src/burnout3_collision.c`); both filters are invariant under the loader's
z mirror. The down-ray keeps the static set only (its oracle is the wreck
gather, it has no velocity input, and (b) is a no-op for a downward segment
under the one-sided `det > 0` test). Differential:
`validate_carcol.py gather`, 22 checks.

### Open [?]

* The world-object streamer fields beyond {+0xC ring size, +0x24/+0x2C ring
  indices, +0x44 loaded, +0x4C current unit, +0x17C resident table} and the
  dual-world selection byte veh+0x217 (second instance at 0x7397c8+0x1CC).
* Leaf +0x08 u16 and prim +0x06 u16[4] extras.
* Surface-type semantics beyond the verified filters (road low bytes
  0x01..0x14 pass the crash-mode filter; 0x16/0x18/0x19 etc. are
  barrier/building classes by observation [S]).

## 16. Crash response + crash-mode motion + aftertouch (2026-08-11)

Everything [C] below is verified by executing the REAL function whole under
Unicorn and asserting a 1:1 mirror reproduces its writes
(tools/validate_gameplay.py "crash response" section: 10 whole-function
FUN_0011AEF0 cases + 3 clipper + 2 impulse cases; suite 91/91). Ported code:
src/burnout3_crash.c/.h. [S-disasm] = read from the instructions, constants
confirmed in the image, no green case. [?] = open.

### FUN_0011AEF0 -- the chassis-vs-world collision response [C]

thiscall, ECX = vehicle. NOT the pose integrator (that is FUN_00109560).
Returns the wall-contact count. Bails (writes +0x198 = 0) when
`veh+0x1353 & 5`.

Environment: `veh+0x200` -> the per-frame poly set `{int count; poly*
records; u16* flags}` built by FUN_00122D00 into the GLOBAL scratch at
0x005A3AA0 (records 0x5A3AB0, stride 0x40, cap 0x60; flags 0x5A52B0):
track polys via FUN_00109D20, 4/6 extra barrier polys from veh+0x11D0
(flag '&', bytes +0x1350/+0x1351 gate), other-car hull polys copied from
veh+0x1590 (count veh+0x3A50) and veh+0x2030 (count veh+0x3A58) with flag
'!' when byte veh+0x215 == 1, then FUN_00109EA0. Poly record = world-space
{p0,p1,p2,n} vec4s. `veh+0x204` -> frame; `veh+0x70` = inverse frame;
`veh+0x1D0/+0x1E0` = collision bbox MAX/MIN, loaded from **.bgv +0xE80 /
+0xE90** by the collision-object init FUN_00122830 [C-disasm] (which also
proves numwheels = file+0xD, and resets +0x1353/+0x1354).

Flow (per-poly FUN_0011AC30 into a stack accumulator, then):

1. Class-0 cars only: run the SECOND set veh+0x1590 (count +0x3A50, flags
   0x20 = ' '); if it added wall contacts:
   `acc(+0xF0) += dir(+0xC0) * mass * -1000.0` [0x3B1744] -- the wall stop.
2. **Wall path** (wall count > 0, +0x198 := 1): normal = componentwise
   min+max of all wall normals (cancels opposing walls; if |sum|^2 <
   2.33e-10 pick nmax, or nmin when dot(right, nmax) < 0). Centroid = mean
   of per-poly clip centroids (body space). Writes veh+0x160 = frame *
   (centroid.x, bbmin.y + 0.33*(bbmax.y-bbmin.y), centroid.z) world point,
   veh+0x190 = surface u16, veh+0x194 = 0, +0x198 = 1. The normal's y is
   ZEROED, normalized, then projected into the car's right/at plane and
   re-normalized -> veh+0x170 (a pure horizontal push direction; |proj|^2 <
   2.33e-10 aborts the whole function returning 0).
   * deflection: `veh+0x130 += n_h * 1.5 * edge_dist` [0x3B1870], where
     edge_dist = min distance of the centroid to the bbox x/z faces.
   * head-on scrub: headon = |dot(n, at)|; if > 0.707 [0x3B1A20]:
     `vel *= 1 - (headon - 0.707)*0.1` [0x3A69C4] (xyz + speed +0xBC).
   * surface scrub: class-0 `vel *= veh+0x13A8`, and gear +0x14C8 == -1
     flips the drift direction +0x1434; other classes `vel *= 0.99`
     [0x3B1758] unless byte +0x153E.
   * impulse: j = FUN_00106720(n_h, world centroid, point velocity, e=0);
     dv = j/mass; if dv > 0:
     `impact = ((1 - min(speed,89.408)/89.408)*0.9
                + (1 - min(0.9, 1.75*|dot(dir,n)|))) * mass * dv * 0.175`
     -> veh+0x194  [0x3B1E18 89.408 = 200 mph in m/s, 0x3B1E14 = 1/89.408,
     0x3A69C0 0.9, 0x3B18B8 1.75, 0x3B1A68 0.175]. Impulse direction =
     n_h minus its up-row component, normalized, * impact; applied as pure
     linear impulse (+0x110) when veh+0x1404 > 0.1, else routed through
     FUN_001206D0 (below) -- the AIRBORNE path applies it AT THE CONTACT
     POINT with torque: this is what makes a wrecked car spin/tumble.
   * wall-crash trigger: normal race -- surface byte != 0x20 AND
     !(+0x1353 & 8) AND dv > authority(+0x1534)*27.5 [0x39B2FC], threshold
     = authority*0.707; crash party (FUN_00017310) -- dv > authority*10
     [0x3A7F34], threshold = authority*0.303 [0x39B308]. Fires when
     headon > threshold and racecar class (+0x1920) != 2:
     `racecar+0x15CC = surface>>15`, then FUN_0010DCA0(&DAT_0064ACE8, veh,
     slot) -> FUN_0010DD20, the wall-crash entry into the crash state
     machine. The byte veh+0x211 == 1 tail ("landed on a car": vtable
     +0x64 crush callback via racecar+0x1650) is mapped, NOT ported [S].
3. **Ground path** (no wall contacts, ground count > 0, +0x198 := 2):
   world point = frame * mean ground centroid -> veh+0x160 = +0x1A0 =
   +0x1B0; veh+0x170 = normalized ground-normal sum; veh+0x194 = RAW
   impulse j (recorded, NOT applied -- the keep-out is the suspension /
   crashed-wheel passes' job). Feeds FUN_0010ED30's probability roll:
   `crash if rand01 < (impact - 8000) / 17000` [C-disasm, 5.882353e-05 =
   1/17000] when byte veh+0x212 set state-2 contact.

### FUN_0011AC30 -- per-poly contact test [C]

stdcall(record, u16 flags, accum), EAX = vehicle. Crash-party head: '&'
polys skipped, and any wheel surface byte veh+0x8D0+i*0xC0 == '&' skips
everything [S]. Transform p0/p1/p2 world->body by accum+0x00 (= veh+0x70);
if |n.y| < 0.2 [0x3A69B4] reject slivers (edge v0-v1, then FUN_0011ABB0 on
(v2,v0) and (v2,v1): |dy| < 0.5 [0x3B1684] and dz^2+dx^2 < 0.2). Clip the
triangle against the bbox (FUN_001B0C00: Sutherland-Hodgman slabs in
x, z, y order via FUN_001B09C0, >= 3 verts at every stage). Centroid =
mean of clip verts (4-wide). n.y > 0.7 [0x3B17D8] -> ground record IF
centroid.y <= 0.35 [0x39B2B0]: accum +0x60 += n, +0x50 += centroid,
count2(+0xD4)++. Else wall: +0x70 minps n, +0x80 maxps n (both init 0!),
+0x40 += centroid, count1(+0xD0)++. Surface flags +0xDC: lowest nonzero
low byte wins (whole u16 stored).

### FUN_00106720 / FUN_00106500 / FUN_001206D0 [C]

* FUN_00106720 (EAX=veh, ECX=&n, stack pt/vp/e/out): standard contact
  impulse `j = |-(1+e)*dot(n,vp) / (1/m + dot(cross(Iinv*(r x n), r), n))|`
  with r = pt - frame pos, Iinv = veh+0x40 world rows. out = n*j, j in XMM0.
* FUN_00106500: `+0x110 += imp; +0x120 += (pt - pos) x imp` (w lane 0).
* FUN_001206D0 routes: drifting (+0x1524 in {1,2}) -> linear only; already
  yawing |omega.y| > 3.0 [0x3EBF64] with the impulse's yaw torque
  (r.z*imp.x - r.x*imp.z) in the SAME direction (sign-bit compare) ->
  linear only; else FUN_00106500 (linear + angular).

### Crash-mode motion -- there is NO separate crash integrator [S-disasm]

FUN_00123000 (the solver dispatcher) runs EVERY frame, crashed or not:
quadratic drag, damage scans ([C], section 13), then per substep the
suspension pre-pass + FUN_00109560 rigid-body integration. What changes in
a crash:

* airborne damping: all wheels off (+0x1168) -> `L *= (0.99, 0.99, 0.97)`.
* slow rollover (byte +0x211 == 0 path, speed < 1): `L.y *= 0.95,
  vel *= 0.95` (0x3F733333).
* settle pass (FUN_00125CF0 true or +0x20E): `L *= 0.9` (0x3F666666),
  `vel *= 0.95`, settle counter +0x1354++; SLEEP (+0x20E := 1, wheel
  omegas zeroed, FX unless +0x215 == 1) when speed < 0.5 AND |omega|^2 <
  DAT_005A80B8 AND ctx+0xFFC < DAT_0060EA20 AND upright-check (wheel
  contact normals dot up-row > 0.98, or +0x212 contact-normal variant vs
  0.99) AND +0x1354 > DAT_003EBF88. The integrator still runs while
  asleep. FUN_00126520 then runs per CRUMPLED wheel (state 2): a
  gravity-reaction + spring/angle-clamped joint sim at the wheel contact
  segment (base+0xEA0/+0xEB0 pairs) -- the flopping-wheel motion. Mapped,
  unported.
* veh+0x1353 crash/ghost flags, writers located [S-disasm]: bits 0+1
  ("retired/ghost", collision + response off via the &5 gates in
  FUN_00122D00/FUN_0011AEF0/FUN_00122D15's caller) set at 0x10F770 when
  racecar+0x19A8 == 0 or owner+0x1AA0 == 0, and by FUN_001213C0/
  FUN_0010F785 (OR 3); bit 1 alone from racecar+0x1171 (0x118E10);
  bit 3 (re-trigger block) + bit 4 by FUN_00027AD0 (OR 0x18) and
  FUN_0018D790; bit 4 tested by FUN_00112E70/FUN_00113960 (car-car crash
  paths); cleared to 0 by FUN_00110AF0 and the inits FUN_00122710/
  FUN_00122830.

### Aftertouch -- mechanism at the consumer end [C-disasm], producer [?]

The racecar carries a crash handoff block, initialized by the routine at
0x0018C2C0 (identity matrix +0x19C0, zeros through +0x1A98):

| racecar offset | meaning |
|---|---|
| +0x19C0 | 4x4 reference matrix (net-sync writes it from packets) |
| +0x1A00/+0x1A10 | vec4s; **+0x1A10 = angular velocity handed to the wreck** |
| +0x1A20..+0x1A22 | button bytes; +0x1A22 & 7 -> vehicle GEAR +0x14C8 (7 = -1 reverse) |
| +0x1A24 | crash kind (0 -> FUN_00114D10 slot path, else FUN_0010DD20) |
| +0x1A25/+0x1A26 | -> veh+0x1528 / veh+0x1446 (the wheel-loop recoil flag) |
| +0x1A28 | -> veh+0x1524 drift state |
| +0x1A30 | vec4 position delta; significant when any |component| > 5.0 [0x3B1694] |
| +0x1A40 | result byte of FUN_0010F100/0x10F200 |
| +0x1A50 | 4x4 rotation delta |
| +0x1A90 | frame counter (set to 10 by 0x10F100) |
| +0x1A94/+0x1A95 | crash-entry pending / secondary flag |
| +0x1A98 | -> director object; its +0x4D8C forces veh+0x20E (sleep) |
| +0x1AA0 | crash camera / crash mode active |

Vehicle-side consumption (unanalyzed region 0x10EE20..0x10F660,
disassembled raw): on +0x1A94 the crash-mode update (0x10F370) calls
0x10EE20 which writes **veh omega +0xD0 := racecar+0x1A10 directly**,
re-syncs the drivetrain (wheel omegas := speed/radius via FUN_00120470 +
FUN_000FFC80, engine omega +0x149C := gear_ratio[+0x14C8] * final(+0x1468)
* max(rear wheel omegas), gear := buttons & 7). While the crash camera runs
(+0x1AA0), 0x10F080 applies the stored pose delta each frame: frame
rotation *= +0x1A50 (FUN_000116E0), frame pos += +0x1A30, counter +0x1A90
decrements. So the wreck's trajectory during aftertouch is COMMANDED by
these deltas; the reverse-gear input path is visible in FUN_0011AEF0
(+0x14C8 == -1 flips +0x1434). **The producer that turns pad input into
+0x1A10/+0x1A30/+0x1A50 during the crash was NOT located** -- the only
direct .text writers are the reset (0x18C2C0) and the net-sync receivers
FUN_00101120/FUN_001012B0; the local producer writes through derived
pointers in the crash-director object (same unanalyzed hole as the AI
target writers, RE_NOTES 13) [?]. Aftertouch BP (1250) attribution flags
were already mapped in RE_GAMEPLAY (score +0x15C0 stamps).

### Ported + integrated

* src/burnout3_crash.c/.h: b3_crash_response (FUN_0011AEF0),
  b3_crash_poly_contact (FUN_0011AC30), b3_crash_box_clip/clip_axis
  (FUN_001B0C00/FUN_001B09C0), b3_crash_impulse (FUN_00106720),
  b3_crash_apply_impulse[_at_point] (FUN_001206D0/FUN_00106500),
  b3_crash_point_velocity (FUN_001066A0) -- all [C]; b3_crash_mode_damping
  (FUN_00123000 constants) [S-disasm]; b3_wreck_begin/update = GLUE
  assembly of the ported pieces around the verified
  b3_rigid_body_integrate.
* tools/validate_gameplay.py "crash response": 15 new cases (10
  whole-function response incl. airborne-tumble/yaw-lock/drift routing,
  class-0 grip + wall stop + drift-dir flip, wall-crash + party-mode
  trigger thresholds with the FUN_0010DCA0 stub and racecar+0x15CC write,
  ground-record path with the +0x1A0/+0x1B0 duplicates, bail path; 3
  clipper; 2 impulse). Suite 91/91; validate_port.py untouched, 73/73.
* Harness (src/burnout3_full.c, CRASH-EVENT blocks): on a wreck the crash
  sites hand the contact to wreck_begin_for -> the ported impulse response
  gives the car real spin/tumble momentum; while crashed_until runs the
  wreck coasts/tumbles/settles through b3_wreck_update (ported response +
  damping + verified integrator, ground via b3_ground_probe) and the draw
  uses the full rigid pose; recovery at the verified +5 s clock levels the
  car. Camera slow-look while the player's wreck tumbles (GLUE). Verified
  in-game: build/wreck_f300/330/420.png -- shell body visibly rolled,
  spinning and sliding ~35 m before settling; takedown/BP flow unchanged
  (tier-up prints in the same run).

## 16.1 CORRECTION — the crash-entry LAUNCH, and what the crashed path really runs (2026-08-11)

Trigger: "the takedowns / crashing should show the car tumbling through the
air; it just shows it upside down sliding". The wreck built in §16 could not
fly, and §16's account of the crashed-path motion was incomplete. Both are
corrected here. Oracle: `tools/emulate_crash_traj.py` (persistent Unicorn
session, real crashed-path frames). Acceptance:
`tools/validate_crash_traj.py` **44/44**, including 260-frame 6DOF
differentials. `validate_gameplay.py` 91/91 and `validate_port.py` 76/76
are unchanged.

### Root cause of the slide [C]

`FUN_0011AEF0`'s wall impulse is **horizontal by construction**: the wall
normal has its `y` zeroed at `0x0011B48x`, is projected into the car's
right/at plane, and the impulse direction then has its up-row component
removed at `0x0011B89x`. It can never give a wreck vertical velocity. §16
ported that impulse and stopped there, so the harness wreck could only ever
flip once (from the contact torque) and slide.

The retail launch is a **separate pair of impulses fired by the crash entry**
`FUN_0010DCA0 -> FUN_0010DD20`, which §16 recorded only as "the wall-crash
entry into the crash state machine".

### `FUN_00125100` — the crash-entry impulse kick [C]

`ESI` = vehicle; stack `(byte flags, float mag, vec4* axis)`; `ret 0xC`.

```
P  = frame.pos                                    (frame row3)
P += frame.right * (-veh+0x1D0.x)   flags & 0x08  hit on the LEFT
P += frame.right * (-veh+0x1E0.x)   flags & 0x04  hit on the RIGHT
P += frame.at    * ( veh+0x1D8  )   flags & 0x21  FRONT, mag *= 1.2  [0x0041A500]
P += frame.at    * ( veh+0x1E8  )   flags & 0x42  REAR,  mag *= 1.2
P += frame.up    * (veh+0x1D4 * 0.8)  when frame.up.y < 0  [0x003A5600]
                                       (an INVERTED car is kicked at the roof)
J  = axis * 10.0 * veh+0x1F0(mass) * mag           [0x0041A504 = 10.0]
(flags & 0x90) == 0 -> veh+0x120 += (P - pos) x J   TORQUE ONLY: the linear
                       part is saved at 0x001252D0 and written straight back
                       at 0x001252EC, so veh+0x110 is left untouched
else                -> veh+0x110 += J              LINEAR ONLY
veh+0x20E := 0                                     (the kick WAKES the body)
ctx1(+0xCC4)+0xFF0/+0xFF4 = fx pair, +0xFF8 = 0, +0xFFC = clock  [0x00125341]
ctx1+0x1015 |= 6 when (flags & 0xE0) == 0
tail FUN_00151490(&DAT_0040F270, mag) = the FX/sound spawn
```

`FUN_00125380` [C] (`EAX` = veh, `CL` = flags, stack `(mag, pad)`, `ret 8`)
just picks the axis: `frame.right` when `flags & 0x60`, `frame.at` when
`flags & 0x80`, else **`frame.up`**.

So a pure-linear up kick gives `dv = 10 * mag` **straight up along the car's
own up row** — 6.5 m/s at mag 0.65 — and the at-point kick is pure torque
about an offset corner. **Launch + tumble.** Every crash-entry site fires
both.

### The crash-entry sites and their magnitudes

| address | flags | mag | what |
|---|---|---|---|
| `0x0011C421` / `0x0011C439` | 0x10, 0x08 | **0.65 / 0.90** | `FUN_0011BE50` rollover entry (`[0x007353E8 + slot*4] == 2`); the `== 3` twin at `0x0011C466`/`0x0011C47C` uses 0x04 |
| `0x00024F94` | 0x0A | **0.40** | `FUN_00024F10`, the crash-record handler — a car wrecked mid-race |
| `0x00025C8D` | 0x0A | **0.30** | its sibling at `0x00025C60` |
| `0x001181B4/E5`, `0x00118213`, `0x00118241` | 0x09/0x05/0x0A/0x06 | **0.60** | `FUN_00118410` — see aftertouch below |
| `0x00197376` / `0x00197363` | 0x08 / 0x04 | **0.25** | `FUN_00197260` grind crash |
| `0x00026AEE` / `0x00026B09` | 0x10 / 0x80 | **0.50 / 1.60** | world-up (`DAT_0040ADF0` = (0,1,0,0)) and forward kicks |
| `0x0010ECA7` / `0x0010ECFD` | 0x01 / 0x02 | 0.50 | `FUN_0010EC10` |

`FUN_0010DD20`'s own kicks take their magnitude from the crash-director
object `0x0064ACE8` at **`+0x57C`..`+0x58C`** (indexed by `director+8`
through the jump table in `FUN_0010DAC0` @`0x0010DAE2`), **`+0x590`** (the
head-on wall entry, `0x0010E3FA`) and **`+0x594`** (a low-speed multiplier,
`0x0010E38D`). Those five floats are **BSS with no static writer anywhere in
`.text`** (scanned for every `mov`/`movss` store at those displacements and
for every absolute reference to `0x0064B264`..`0x0064B27C`) — **[?]**. The
mechanism is [C]; only the director's own tuning values are open.

`FUN_0010DD20` structure, for the record [C-disasm]: `EAX` = out record,
`EBX` = vehicle, `ECX` = slot, stack `(director, cause record)`. Bails on
`veh+0x210 != 0` or slot `== -1`. **`cause record == 0`** (what
`FUN_0011AEF0` passes — `XOR EDI,EDI` at `0x0011B9F1`, and what
`FUN_0011BE50`'s rollover and `FUN_00024F10` pass) takes the branch at
`0x0010E3C9`: read the contact block `veh+0x160`, and **if
`dot(frame.at, contact normal) < -0.7071` [`0x003B1DE0`]** fire
`FUN_00125100(0x02, director+0x590, frame.up)` (rear corner torque) then
`FUN_00125100(0x10, director+0x590, frame.up)` (the linear launch), record
kind 0xB. A non-zero cause record (`FUN_00113960` @`0x001141EB`) goes through
`FUN_0010DAC0`'s per-kind table instead.

### Aftertouch — the producer, FOUND [C-disasm] (closes §16's `[?]`)

§16 said "the producer that turns pad input into +0x1A10/+0x1A30/+0x1A50
during the crash was NOT located". It does not go through those fields at
all. `FUN_00118410` (the crashed-path input shaper) @`0x00118176`..
`0x0011824F` reads the direction bits of `byte ptr [EBX]` and, per direction,
calls `FUN_0010DCA0` and then one **corner torque kick at magnitude 0.6
along the car's own up row**:

| bit | flags | corner |
|---|---|---|
| 0x04 | 9 | front-left |
| 0x10 | 5 | front-right |
| 0x01 | 0xA | rear-left |
| 0x02 | 6 | rear-right |

Gated on `byte [EBP+0x4AC2]`. That is aftertouch: the player steers the
tumbling wreck by adding corner torque impulses. Ported as
`b3_wreck_aftertouch`.

### The crashed path is NOT the racing pipeline [C]

`FUN_0011BE50` @`0x0011BE75` branches on `byte veh+0x210` and the crashed
side (`0x0011BE83`..`0x0011BF09`) runs **only**:

```
veh+0x1530 += dt                                  crash clock
if (veh+0x14C8) { veh+0x14A0 = 0.35; veh+0x14C8 = 0; veh+0x14A4 = 1 }
FUN_00121560(&veh+0x1448, 0, 0, 0)                engine idles
if (racecar+0x1920 == 0) FUN_00118410(veh)        crashed input shaper
FUN_00123000(veh, dt)                             the crash-mode solver
FUN_0011C720(veh)                                 ctx export
```

It never reaches `FUN_0011BC60` (soup), the vtable `+0x24` collision update
(`FUN_00122D00`, vtable slots `0x003B1198` / `0x003B11FC`), `FUN_0011D460`,
`FUN_001239C0` **or `FUN_0011AEF0`**. A wreck therefore has **no
chassis-vs-world response at all** on this path; only the wheel/suspension
pass holds it up. (`FUN_0011C490`, the multi-substep runner called by
`FUN_0010F200`, does run the full set including `FUN_0011AEF0` — that is the
crash-camera/aftertouch resync path, not the per-frame one.)

### `FUN_00123000` — the crash-mode solver, exactly [C]

Called **only** by `FUN_0011BE50`'s crashed branch. Correcting §16's
"runs EVERY frame, crashed or not".

* **Wheel classification** @`0x0012303E`, the airborne test §16 left vague:
  for each wheel whose damage state `ctx1+0x4AC+i != 3`, read the contact
  surface byte **`veh+0x8D3 + i*0xC0`**.
  `veh+0x1168` (**all airborne**) = 1 iff **no** wheel has a surface;
  the local at `[ESP+0x1A]` (**all grounded**) = 1 iff **every** wheel does.
  It is a per-WHEEL test, not a body-box test.
* **Quadratic drag**, once per frame before the substep loop @`0x001230A6`:
  `veh+0xF0 += veh+0xC0(dir) * (speed^2 * -1.0)` [`0x003B16C0`].
* Substeps: 1, or 2 at dt/2 when `byte [0x005A3759]` and the mode check pass.
  **The speed all three damping gates test is captured ONCE** at
  `0x001230FA`, before the loop.
* Per substep, in this order:
  - rollover crawl @`0x001232C3`: `veh+0x211 == 0` AND all grounded AND
    speed < 1 -> `L.y *= 0.95`, `vel *= 0.95` [`0x003A69B8`];
  - airborne @`0x001235FF`: `veh+0x1168` -> `L *= (0.99, 0.99, 0.97)`
    [`0x003B1758`, `0x003B1A2C`];
  - settle @`0x00123372`: speed < 1 AND (`FUN_00125CF0(veh,1)` OR
    `veh+0x20E`) -> `L *= 0.9` [`0x003A69C0`], `vel *= 0.95`,
    `veh+0x1354++`; **else if speed >= 1** @`0x001236E0` ->
    `veh+0x20E = 0`, `veh+0x1354 = 0`;
  - sleep @`0x00123518`..`0x001235ED`: speed < 0.5 [`0x003B1684`],
    `|omega|^2 < DAT_005A80B8` (**BSS, [?]**), `clock > ctx1+0xFFC` (the
    timestamp `FUN_00125100` stamps), the contact/upright test, and
    **`veh+0x1354 > 5`** — `DAT_003EBF88` is a **byte 0x05**, not a float;
    then wheel omegas zeroed, `veh+0x20E = 1`, integrate WITHOUT the
    suspension pass;
  - `FUN_00123FD0` (skipped while asleep), then `FUN_00109560`.
* **The gravity torque.** `FUN_00109560` @`0x00109592`/`0x001095D8` branches
  on `veh+0x210`: crashed (or `veh+0x215 == 6`) takes `0x00109622`, applying
  gravity at `pos + up * veh+0x1F4(com height)` — i.e. **with a torque**.
  The racing path never sees it. A tumbling wreck keeps rotating partly
  because of this term; the §16 harness called the integrator with
  `crash_mode = 0` and `com_height = 0` and lost it.

Ablation, measured with the oracle: stubbing `FUN_00123FD0` for the whole
airborne arc changes the trajectory by **0** — while no wheel has a contact
the suspension pass contributes nothing, so the flight is drag + gravity
(with torque) + the airborne `L` law, and that is exactly what the C mirror
reproduces.

### Ported + verified

* `src/burnout3_crash.c/.h`: `b3_crash_kick` (`FUN_00125100`),
  `b3_crash_kick_axis` (`FUN_00125380`), `b3_crash_mode_frame`
  (`FUN_00123000`'s body maths + `FUN_00109560` in crash mode) — all [C];
  `b3_wreck_aftertouch` [C-disasm]. `b3_wreck_begin` now fires the retail
  entry pair (corner torque 0.40 = the `0x00024F94` literal, plus the
  0.65 linear launch), classifies airborne per wheel station instead of
  "any bbox corner touching", and uses the game's **compiled-in class
  inverse-inertia diagonal** (`FUN_001203A0`, §14) instead of a box formula
  that gave the wreck ~2x the retail inverse roll inertia.
* `tools/emulate_crash_traj.py` — the oracle: warm the car up on the real
  racing pipeline, fire the REAL `FUN_00125380`/`FUN_00125100`, then run the
  REAL crashed-path frame 150-300 times. Scenarios `wall` (44 m/s into a
  real wall plane resolved by the REAL `FUN_0011AEF0`), `slam`, `spin_only`.
* `tools/validate_crash_traj.py` **44/44**: 18 whole-function kick cases
  (every literal magnitude above, both wrappers, yawed and INVERTED poses
  for the roof arm); 260-frame 6DOF differentials for both crash scenarios
  (position **5e-7 m**, velocity **5e-8 m/s**, angular momentum and omega
  exact on the first crashed frame); the flight assertions (launch 6.30 m/s,
  1.8-2.2 m of air, 2 rotating axes for a corner entry, 1.16-2.55 turns in
  2 s); and the **launch ablation** — a torque-only entry produces
  **-0.20 m/s** of vertical velocity, i.e. no launch at all, which is the
  regression guard on the original bug.

### Cross-module finding — `b3_mat_orthonormalize` vs `FUN_000FF270` [C-disasm]

While matching the tumble, one residual would not close: with `L` and
`omega` reproduced exactly (3e-10 / 3e-9 on the first crashed frame) the
orientation rows still drifted **2.6e-3 per frame** under a fast multi-axis
tumble, and **4e-6 per frame** for a single-axis somersault.

`FUN_000FF270` is not the fixed "row2/at anchored" repair §11 records. It
normalises each row through `FUN_0002C0D0` (normalise in place, return the
length), and then:

* length(row0) <= 0 -> `row0 = norm(row1 x row2)`, `row2 = norm(row0 x row1)`
* else length(row1) <= 0 -> `row1 = norm(row2 x row0)`, `row0 = norm(row1 x row2)`  <- what §11 describes
* else length(row2) <= 0 -> `row2 = norm(row0 x row1)`, `row1 = norm(row2 x row0)`
* else three `FUN_00013C60` pairwise dots -> `FUN_000FF090` -> rebuild the
  **two rows of the least-orthogonal pair** (`FUN_000328F0` crosses)

i.e. **which row is anchored is data-dependent**. Verified numerically on a
real crashed frame: the retail result is `up = normalize(up')`,
`right = norm(up x at')`, `at = right x up` — the first branch — while
`b3_mat_orthonormalize` took the second. This lives in
`src/burnout3_vehicle_sim.c` (not this agent's file) and is recorded here as
a hand-off item; `validate_crash_traj.py` asserts the per-step residual
stays under 4e-3 so it cannot silently grow.

### Still open here

* crash-director `0x0064ACE8` `+0x57C`..`+0x594` launch magnitudes [?]
  (BSS, no static writer) — the harness uses the literal-magnitude sites.
* `DAT_005A80B8`, the `|omega|^2` sleep limit [?] (BSS).
* `FUN_00125CF0` (the settle predicate) and `FUN_00123FD0`'s crashed-path
  per-wheel scrape at `0x001248EA` (mapped: point velocity at
  `wheel+0x10`, `F = -vhat * (|dot(right,vhat)|+1) * mass * numwheels *
  0.75` [`0x003B17EC`] applied at that point via `FUN_001064B0`, halved
  unless `veh+0x215` in {1,2,3}) — unported [S]; both are inert on the
  airborne arc the oracle asserts.
* `b3_mat_orthonormalize` (above).

### 13.3 Second vertex pool decoded + the halved-strip bug (2026-08-11)

Investigation trigger: the intact car still showed a hole at the LEFT door's
lower half in-game, while panel slot 1 (right door) extracted as only a
beltline strip -- mirror-opposite completeness between the embedded body and
the panel meshes.

**The streams [C]** (tools/trace_panels.py --deep -- FUN_00031E10 /
FUN_00031AB0 / FUN_000315C0 executed for REAL, only the push-buffer leaves
stubbed; FUN_0034EDB0 / FUN_001D7D10 captured at entry):

* FUN_000303D0 passes the LOD section pointer S in EAX (0x3052F
  `MOV EAX,EBX`) to FUN_000315C0, which binds TWO vertex streams:
  `FUN_0034EDB0(0, S+0x48, 0x18)` and `FUN_0034EDB0(1, S+0x54, 8)`.
  S+0x48 and S+0x54 are D3D vertex-buffer resource headers {u32 Common,
  u32 Data}: Data = the relinked pool pointers at S+0x4C / S+0x58 (this is
  why FUN_000310F0 relinks both with the same masked += S rule -- the
  "flags" words at +0x48/+0x54 are the resource Common fields).
  FUN_00031800 unbinds streams 0 and 1 after the vehicle pass.
* The SECOND pool (S+0x58) is therefore a parallel per-vertex ATTRIBUTE
  stream, stride 8, same index space as pool 1.  Geometry (position/
  normal/uv, stride 0x18) always comes from pool 1.  The hypothesis that
  some records resolve positions against pool 2 is FALSIFIED.
* [S] pool-2 entry layout: 4 x u16 per vertex, observed {a, b, wa, wb}
  with wa + wb == 255 (COMP Car1: (236,19), (244,11), (207,48) ...) --
  per-vertex two-matrix blend indices+weights for the deformable renderer
  FUN_00031E70 (the 8 ctx+0x500 matrices).  Not needed for extraction;
  not decoded further.

**The actual bug [C]**: record+0x10 is the u16 INDEX COUNT, not a byte
size.  FUN_00031AB0 pushes `MOVZX EAX, word ptr [rec+0x10]` indices at
rec+0x0C to the indexed draw FUN_001D7D10(6, count, ptr) (call sites
0x31D14 glass / 0x31DFB tail -- glass records are drawn TWICE by design).
The extractor's old `size/2` reading decoded HALF of every triangle strip.
Static cross-check on COMP/Car1's embedded part: consecutive payload gaps
equal 2*count (+2 alignment), and rec 0's +0x10 value 0xE59 is ODD --
impossible as a byte size of u16 indices.  Deep-trace check: all 15
captured intact-state draws push exactly the u16 at rec+0x10 (player .bgv
and traffic .btv -- HEVY/Car24.btv -- identical).

**Corrected picture**:

* COMP/Car1 embedded part is now symmetric and complete: left-door-lower
  108 tris / right-door-lower 107, bonnet 581, hatch 162; maxidx 7169 ==
  pool-1 capacity 7170 exactly (pool2-pool1 = 0x2A030 = 7170*0x18).
  Door panel slots 1/2 both extract 155 tris (full height).
* Intact draw = ONE FUN_00031E10(embedded, mask 0x3FF) -> 11 indexed draws
  (10 records; the mask-0x100 glass record twice) + 4 x e10(slot 7 wheel).
  There is NO separate slot-0 glass pass in the intact state (that pass
  belongs to the damaged branch, ctx+0x101b != 0).
* REVERSAL of the 13/"part-damage layout" claim: with the correct index
  count the embedded one-piece object IS panel-complete.  The earlier
  "mask-bit0 body is NOT panel-complete / intact = slot0 + panels at
  matrices" reading was an artifact of the halved strips.  extract_bgv.py
  now emits `_intact.obj` = the embedded object's non-glass records (the
  traced intact draw), `_glass.obj` = its mask-0x300 records; the slot-0
  aperture body + panels + file+0xD00 matrices remain the DAMAGE-path
  assembly (`_shell.obj`, `parts/<car>/panel*.obj`, `.panels`).
  extract_traffic.py's .btv meshes likewise use the embedded object.
* No cross-pool indexing exists anywhere, so the historic "spaghetti"
  union renders were never a pool problem.

**Regen + acceptance**: 67/67 player cars + 12/12 traffic re-extracted;
validate_port.py 76/76, validate_gameplay.py 91/91 after rebuild.  12
in-game screenshots (COMP_Car1, SUPR_Car1, HEVY_Car1 x left side / right
side / front / rear; SDL_AUDIODRIVER=dummy B3_FIXED_DT=0.0166 B3_TRAFFIC=0,
B3_CAMSIDE + pinned B3_CAM views): no body holes anywhere -- both doors,
bonnet, hatch, bumpers, light clusters all present.  Harness addition:
`B3_PLAYER_CAR=CLASS_CarN` (e.g. SUPR_Car1) overrides the player's roster
entry for screenshot verification.  Note: headless runs without
B3_FIXED_DT produce NaN physics from frame 1 (pre-existing variable-dt
issue, unrelated to meshes) -- always set B3_FIXED_DT for shots.

## 14. The full per-frame vehicle pipeline, ported and trajectory-verified (2026-08-11)

The harness now runs vehicle physics THE GAME'S WAY: `b3_vehicle_step_full()`
(burnout3_vehicle_sim.c) is a 1:1 composition of the per-frame pipeline over a
real vehicle struct with 4 independent wheels, replacing the scalar
speed+drift scaffold `b3_vehicle_step` (retired from vehicle_update).
Everything below is [C] confirmed by differential execution unless marked.

### Ground truth: multi-frame differential trajectories

`tools/emulate_pipeline.py` runs the REAL functions under one persistent
Unicorn session, per frame in FUN_0011BE50's exact call order and glue, over
a fully seeded COMPCAR1 vehicle (real Data/vdb.xml config, real Car1.bgv
extents/wheel positions/radius, real class inertia) on a flat-plane ground
soup, for 300-390 consecutive frames x 3 input scenarios (accelerate from
rest through the gears; corner at speed; brake to a stop and into
hold-brake reverse). `tools/dump_traj.c` runs the C port from the same
state; validate_port.py's "full pipeline trajectory" section asserts:

* 15-frame windows re-seeded from emulation state checkpoints: worst
  deviations pos 3e-6 m, vel 4e-5 m/s, rpm 8e-3, wheel omega 8e-4 —
  3-6 orders of magnitude inside the 1e-2/1e-1 acceptance;
* full runs from t=0: gear and drift state EQUAL on every frame, pos within
  0.11 m and speed within 0.06 m/s over 6.5 s (bounded fp noise at the
  standstill fixed point; one borderline idle-floor branch in deep reverse
  desyncs the PRNG stream for a bounded ~2% rpm flutter divergence).

Two fp-fidelity findings the trajectory exposed (invisible to unit cases):
* FUN_00121560's torque chain runs on the x87 stack from the raw PRNG
  integer (FILD..FMUL..single FSTP): rounding the rand to f32 early loses
  bits that the tyre slip feedback amplifies ~2.5x per frame. b3_rand01 now
  returns double and the chain rounds once.
* 32-bit PRNG words must round-trip exactly through any state transfer
  (%.9g loses low bits and desyncs the flutter stream).

### FUN_0011BE50 (per-frame update, __thiscall + float dt) — mapped in full

Gate: byte racecar+0x19A8. Byte v+0x210 SET selects the crashed/simplified
path (engine-idle + FUN_00118410 when racecar mode +0x1920==0 +
FUN_00123000 dispatcher + FUN_0011C720) — the crash agent's domain (section
16); section 10's "in_race = +0x210" naming was BACKWARDS. The normal
racing path is byte 0x210 == 0:

| order | callee | role | status |
|---|---|---|---|
| 1 | FUN_0011BC60 | collects ground polys near the car into the soup at v+0x200 (world spatial index at DAT_007397C8, per-cell query FUN_001AD4A0/FUN_001AFF70; zeroes the count then refills; appends 6 crash-floor records when byte v+0x1351 set) | [P] `harness_soup_freeze` snapshots raw local collision triangles once per frame; wheel rays use it directly and chassis contact filters the same snapshot with FUN_0011BBE0's predicate. The crash-floor append remains absent |
| 2 | vcall *(vtbl+0x24) = FUN_00104D30 | driver dispatch (traffic/AI/attract input writers) then FUN_0011ECF0; also throttle glue: live 0x1400 = raw 0x1414 x accel-mult 0x13BC capped 1.0, steer zeroed below 0.1 m/s | glue mirrored; FUN_0011ECF0 PORTED (below) |
| 3 | flag dressing | ctx(0xCC0)+0x58 visual bits from gear/state | [S] visual-only, not modelled |
| 4 | substep decision | racecar+0x1920==0 (or ==2 in crash-mode) => dt *= 0.5, run stages 5-9 TWICE; wheels +0x00..0x1F snapshotted | PORTED (mode 0 = normal) |
| 5 | FUN_0011D460(v, dt') | force pass | PORTED (b3_d460_force_pass) |
| 6 | FUN_0011AEF0 | ground-collision/crash response; ret!=0 => v+0x1524=3, byte 0x212=1 | crash agent's (section 16); BE50's ret-0 glue mirrored |
| 7 | FUN_001239C0(v) | suspension pre-pass (below) | PORTED (b3_prepass) |
| 8 | FUN_00123FD0(v, dt') | suspension force pass + wheel visuals | PORTED (b3_suspension_pass) |
| 8b | stop-check glue | (speed<0.5 && throttle<=0.1) or racecar+0x179C in {0,2}: zero vel 0xB0..0xBC and accF.x/.z (0xF0/0xF8) | PORTED |
| 9 | FUN_00109560(v, dt') | rigid-body integration | PORTED (b3_rigid_body_integrate, section 11) |
| 10 | substep tail | wheel prev records +0x30..0x4F := the PRE-frame +0x00..0x1F (prev spans the whole frame); timer v+0x152C -= dt | PORTED |
| 11 | panel loop | per crumpled panel (ctx+0x4B2[i]==2): FUN_00126D40 + FUN_00126520 | [S] damage visuals, 0 panels seeded; crash agent's domain |
| 12 | FUN_0018DA00 | slam/aftertouch camera-wobble writer on the racecar object (+0x1930 matrix, +0x54..0x60), gated by slam clocks +0x1598/+0x1690 | [C] STUBBED: differential ablation over 360 frames = 0.0 trajectory delta |
| 13 | FUN_0011C720 | export: ctx0+0x54=speed, ctx1+0x1028/102C/1030-1033 = gear/rpm/airborne/boost/drifting/flag_b | PORTED as the harness accessors (struct fields) |
| 14 | FUN_0011FFA0 | skid/smoke classifier: writes wheel+0x78 state + v+0x1529, spawns effects (FUN_001202A0) | [C] STUBBED: same ablation, 0.0 delta |
| 15 | FUN_0010DCA0 | rollover handler, gate frame up.y < 0.5 | [S] stub (upright racing never reaches it) |
| 16 | crash-mode tail | DAT_004D5370-gated: FUN_00109BB0/FUN_0010DD20/FUN_00125100 + the 0x26-surface scan | [S] crash-mode only (global 0 in racing); crash agent's domain |

### FUN_0011ECF0 (input stage) — ported as b3_input_stage + engine

Racing path (byte v+0x215==3, racecar+0x179C==1): steering trio restore
from the config behind 0x13F8 (+0x14C/+0x154/+0x190 -> live
0x137C/0x1384/0x13C0); flag_b (0x1446) full-throttle override; gear engage
— RETAIL QUIRK [C]: the forward-velocity projection calls FUN_00013C60,
a no-op stub in the shipped binary, so the engage tests compare raw vel.x
(+0xB0); reverse control swap (gear<0 swaps 0x1400/0x1404); steering
schedule + slew (verified b3_steer_schedule/b3_steer_slew), angle 0x1164 =
-(steer x angle); engine update FUN_00121560 with wheel omega = max(rear
pair 0x9FC/0xABC), kick = (+0x179C==0), boost = input bit 0x13FC&4;
drive torque 0x1520 with boost shaping (engine-brake torque x -0.5 while
boosting in top gear under the boost top speed) and the top-speed cut;
brake-pressure drift gate = max(0.1, 1 + (maxpress-1)/(130-mindrift) x
(speed_mph - mindrift)) with trigger brake > gate or (throttle>0.2 &&
brake>0.2); slide target (drifting: (1-|dir.right|/cos_maxdrift) x
(smax-smin) + smin + brake - max(0, rawthrottle-0.5), clamped; else smax)
lerped into 0x1440 by dt; auto-drift entry (state 0: |steer|>0.9 &&
throttle>30/speed accumulates 0x1438 vs threshold (1.5-throttle) -
(speed-43)x0.04 + delay_13A0 min 0.2, trigger needs speed>43) -> state
2/1 by steer sign, drift clock 0x142C=0; latches 0x141C/0x1420/0x1424/0x1430.

### FUN_0011C7C0 (LSDM low-speed drive model) — PORTED (b3_lsdm_update)

Engages when flag_b==0 && (speed_mph < LsdmSpeedLimit 0x13AC or gear==-1)
— i.e. every standing start and stop. 4 substeps at dt/4 of a bicycle
model: rear drive/brake force (torque_1520 x throttle_1400, brakef x brake
x 5000/6000 by rear-omega sign, engine braking (rpm/maxrpm) x engbrake x
0.5 off-throttle above 1 m/s); rear slip via fpatan capped 3 deg, force
angle/3 x min(mass,2000) x 16; front lateral capped 8 deg, angle/8 x
min(mass,2000) x 12; rear omega integrates force x h x 0.04 and back-
torques -F_long x radius x h x 0.04, copied to wheel 2; body-space
delta-v -> force into 0xF0 with torque about pos - up x (steer_h x -0.5),
plus yaw-momentum-change and yaw-damping ((LsdmT1 - |steer|) x LsdmT2 x
yaw / (I_yaw_inv x dt)) torques along up; fronts free-roll |point vel| /
radius signed by reversing; below 0.2 m/s in neutral: wheel omegas zeroed,
vel and omega x0.9 (4-wide). Steering: 0x1164 = -(LsdmAngle x steer),
drift state cleared. Sets the no-upshift latch on rear slip.

### FUN_001239C0 (suspension pre-pass) — PORTED (b3_prepass)

Per wheel: prev snapshot (+0x30 <= +0x00, +0x40 <= +0x10); ray through the
wheel frame's row3 x/z, local y from radius+attach (start) down to
attach - 0.75len - radius (end, +radius x 0.25 when the wheel is crumpled),
world-transformed; the soup query FUN_00123790 (surface-type filter: in
states 1/2/3 with byte 0x210==0 only types <0x15 and 0x26 are testable;
grip scalar side effect ctx+0x324 = 0.2 on 0x26 else 1.2) runs the ray/
triangle core FUN_001B2230 — CORRECTION to section 10: the soup records
are three triangle POINTS +0x00/+0x10/+0x20 plus normal +0x30, not "two
edge vectors" — Moller-style with det tolerance band (-1e-5 .. 1.00001] x
det, outputs the ray parameter. Hit: contact flag, contact point = start +
ray x t, normal = poly normal if n.y >= 0.2 else frame up, surface u16 to
wheel+0xB0, spring length +0x64 = start_local_y - dist + radius. Miss:
the airborne path already ported in section 10. Tail: v+0x1160 = wheel 0
surface; 30 m under-body ray from the frame origin -> ctx+0x49C clearance
(10000.0 sentinel), ctx+0x490 normal; v+0x1352 = 1. The C port ray-tests a
per-frame frozen local triangle snapshot with the same one-sided Moller
winner rule; the standalone `b3_ground_probe` remains only its no-snapshot
fallback.

### FUN_00123FD0 (suspension force pass) — PORTED for the racing path

Byte 0x210==0 (normal racing): NO extension clamp, NO soft clip, NO body
scrape, NO spin decay — those all belong to the crashed path (0x210!=0,
via FUN_00123000; section 10's clamp/clip findings hold there). Racing:
droop relax (verified b3_wheel_droop) or contact spring/damper
F = -(comp-len)k + vel x c with the 0.25len bottom clamp (clamp cut >
0.001 accumulates normal x cut averaged into the deflection +0x130 —
ported; the bottom-out impulse solver FUN_001066A0/FUN_00106720 and the
HEVYCAR-ID crash bookkeeping behind it remain UNPORTED [S], the scenarios
never reach them); force along the contact normal at the wheel world pos
(frame x (x, cur, z)), torque about the frame origin; wheel spin integrates
omega x dt (wrap +/-200pi); the visual tail rewrites each wheel frame:
rows = spin/steer rotation, row3 = (attach_x, spring length, attach_z)
(DAT_004A1F70 row-3 offsets are zeros [C]) and exports per-wheel omega to
ctx+0x4E8.

### Init chain (mirrored by b3_vehicle_full_init + the harness)

FUN_00109270 zero/identity rigid body; FUN_00109190(x,y,z diag) body
INVERSE INERTIA — compiled-in per class [C, FUN_001203A0 + .data]:
default (0.0008, 0.0011, 0.0013), HEVY* trucks (0.0004, 0.0006, 0.0007),
HEVYCAR5/6 (0.00075, 0.0008, 0.0011); FUN_00122830: wheel count from
file+0xD, v+0x1D0 half extents = file+0xE80, v+0x1E0 = file+0xE90 (both
now emitted into the .wheels sidecars by extract_bgv.py), v+0x1CC =
file+0x14, wheel radius from ctx+0x4D0, frame object = ctx0xCC0 (v+0x204
== v+0xCC0, file ptr at +0x40); FUN_0011A8F0: com height +0x1F4 =
(ext.y - center.y) x 0.1, input/drift state zeroing, slide = slide_max;
FUN_00134710 config->live copy incl. cos precomputes 0x13D0 =
cos(MaxDriftAngle), 0x13DC = cos(90 - MinDriftAngleInAir);
FUN_001214A0 transmission reset (PRNG seeds, rev limit, gear count).

### Harness integration (burnout3_full.c)

vehicle_update drives `B3VehicleFull fsim` through a FIXED 60 Hz tick
accumulator (the Xbox game steps this pipeline at its hard frame tick;
raw variable dt destabilises it); physics state lives in GAME space and
converts at the boundary (pos/vel z-negated in/out, steering negated in —
the pipeline's cross-product algebra is chirality-bound: a mirrored basis
is flipped upside down by the FUN_000FF270 orthonormalizer, which is how
the first attempt failed); ground via `b3_ground_probe_hook` (mesh probe
of section 15 first, route-line height as the documented placeholder
fallback); wall push-out (mesh_collide) is substepped along the frame
motion and written back into the body (pos + velocity + dir, derived
matrices refreshed); HUD/audio/AI keep reading v->sim (speed/rpm/gear/
trans synced from fsim each frame); hold-brake reverse now comes from the
real gear engage (the kinematic AI reverse is retired); wheel visuals use
the real per-wheel omega and the pipeline steering angle. GLUE that
remains: off-world recovery (15 m below the corridor -> route reset; the
real suspension can drop a car off unfenced edges the old height-pinned
model masked), AI target selection/branch speeds, crash triggers (crash
agent). Autodrive acceptance (3-min window, 6 cars + traffic): pack mean
64 mph (old scalar model: ~36 mph), on-road top 161 mph, all 8 gear states
-1..6 exercised, rpm 1000-8250, lap completions observed, no NaN under
fixed or variable dt.

### Still open here

* the bottom-out impulse solver + takedown/recoil block in FUN_00123FD0
  and FUN_0011D460's flag_b block (located, gated off in racing);
* FUN_00118410 (crashed-path input shaper, 534 lines) — only reachable on
  the byte-0x210 path, crash agent's domain;
* the forced-steer stack flags in FUN_0011ECF0 read uninitialised stack in
  retail; the differential driver zeroes the stack and the port mirrors
  the flags-clear behavior;
* drift-state trajectories (state 1/2) are covered by the unit tyre cases
  and the corner scenario's slide schedule, but no long drift trajectory
  window is asserted yet (entry thresholds need >96 mph or heavy brake).

## 16.2 CORRECTION — the AT-REST phase: why a harness wreck bobbled, and what retail really does (2026-08-11)

Trigger: "after a crash the wreck bobbles into space" — a wreck floats/jitters
upward instead of coming to rest, and a live T-dump repro (frame 599,
race_time 6.23, divisor-5 crash slow-mo active) with the player wreck 10 m
over the road and **vy still +2.9 m/s at 0.89 s of air time**. §16.1's launch
and tumble are oracle-verified; everything below is the phase after them.
Oracle: `tools/emulate_crash_traj.py`; acceptance
`tools/validate_crash_traj.py` **108/108** (was 44/44).
`validate_gameplay.py` 91/91 and `validate_port.py` 76/76 unchanged.

### First, a retraction: §16.1's `FUN_00123FD0` ablation was a Unicorn artifact

§16.1 recorded "stubbing `FUN_00123FD0` for the whole airborne arc changes the
trajectory by **0**". That measurement patched the stub AFTER the warm-up
frames had already executed the function, so Unicorn served the **cached
translation** and the stub never ran. With `uc.ctl_flush_tb()` after the
patch the same ablation moves the car **0.4 m in seven frames** and then lets
it fall through the world (`build/…` scratch runs; the numbers are in the
integration note). The conclusion that matters:

* **`FUN_00123FD0` is what holds a wreck up and what brings it to rest.**
  The zero-difference claim survives only for the *fully airborne* window
  (no wheel has a contact, so the pass contributes nothing) — which is
  exactly the window the 44 existing differential cases cover, so those stay
  valid. Any future ablation under Unicorn must flush the TB cache.

### `DAT_005A80B8` = **0.25** [C] — closes §16.1's `[?]`

Not "BSS with no static writer": the writer is the CRT static-initialiser
thunk at **`0x002BA7A0`** (`movss xmm0,[0x003B1730]; movss [0x005A80B8],xmm0;
ret`), reached through entry **`0x003BDE84`** of the init-pointer table at
`0x003BDE60`.. (all entries are 0x20-spaced thunks of the same shape). The
constant `[0x003B1730]` is **0.25**, so the sleep gate wants **|omega|^2 <
0.25**, i.e. |omega| < 0.5 rad/s. Verified by executing the real thunk
(`validate_crash_traj` section 6). The emulator never runs CRT init, which is
why the gate looked dead in the oracle.

### `FUN_00125CF0` — the settle predicate, ported [C-disasm]

`ESI` = vehicle, one byte stack arg, `ret 4`:

```
0x00125CF0  veh+0x212 (a chassis contact this frame) AND
            veh+0x190 (its surface byte) <= 0x20            -> 1
0x00125D0E  arg == 0                                        -> 0
0x00125D16  walk wheels i = veh+0x1169-1 .. 0:
              contact byte veh+0x8D3 + i*0xC0 set, AND
              damage state ctx1+0x4AC + i != 3, AND
              surface byte veh+0x8D0 + i*0xC0 <= 0x20        -> 1
0x00125D66  otherwise                                       -> 0
```

(The `JL` after each `AND EAX,0xff` can never be taken — the byte is
zero-extended first — so the test is exactly `<= 0x20`.) Ported as
`b3_crash_settle_test`; 9 whole-function differential cases.

### The sleep gate, in full [C-disasm] (§16.1 had it partly wrong)

Reached only from the settle branch, AFTER the counter increment
(`0x001233E8`: `cl = veh+0x1354; inc cl; store` — a BYTE, it **wraps** at 255,
it does not saturate; the oracle shows 255 -> 0 -> …). Then:

| addr | gate |
|---|---|
| `0x00123414` | `veh+0x212 != 0` picks the CHASSIS arm, `== 0` the WHEEL arm — **it is not a bail-out** (§16.1 read it as one) |
| `0x0012341A`..`0x00123516` | chassis arm: `dl = 1` unless all three `abs(dot(veh+0x170, frame row))` <= **0.99** `[0x003B1758]` |
| `0x00123662`..`0x001236DB` | wheel arm: `dl = 0` if any wheel with a contact (`wheel+0xB3`) has `dot(wheel+0x20 normal, frame.up)` < **0.98** `[0x003B1DA0]`; no wheels in contact -> passes |
| `0x00123518` | `speed < 0.5` `[0x003B1684]` |
| `0x00123529` | `|omega|^2 < DAT_005A80B8` (= 0.25) |
| `0x00123564` | global clock `DAT_0060EA20` > `ctx1+0xFFC` (the stamp `FUN_00125100` writes at the crash entry) |
| `0x0012357F` | `dl != 0` OR the all-grounded local |
| `0x0012358F` | settle counter (post-increment) > **5** `[byte 0x003EBF88]` |
| `0x001235A1`..`0x001235ED` | zero every wheel omega (`+0x87C + i*0xC0`), `FUN_00151EC0(&DAT_0040F270, veh)` unless `veh+0x215 == 1`, `veh+0x20E := 1`, and **skip `FUN_00123FD0`** (`jmp 0x123701` — the integrator still runs) |

**And the flag does not stick by itself**: `FUN_00109560` @`0x00109592`
clears `veh+0x20E` on entry whenever **`veh+0x211 != 0`**, so a body in that
state can never stay asleep however quiet it is. (The shared
`b3_rigid_body_integrate` in `src/burnout3_vehicle_sim.c` does not model that
clear — hand-off item; the crash path mirrors it in `b3_crash_mode_frame`.)

All of it is now driven through the REAL `FUN_00123000` in
`validate_crash_traj` section 6: 11 cases, each gate violated one at a time,
plus the `0x002BA7A0` initialiser.

### `FUN_00123FD0` @`0x001248EA`..`0x001249E9` — the crashed-path SCRAPE [C-disasm]

The friction that actually stops a sliding wreck. Per contact point, while
`veh+0x210` is set, `veh+0x116B` is clear and `speed > 0.1` `[0x003A69C4]`:

```
V    = FUN_001066A0(veh, pt)                 velocity OF THE POINT
vhat = FUN_00011640(V)                       normalise in place
d    = FUN_00013C60(frame.right, vhat)       row 0
f    = (|d| + 1.0) * veh+0x1F0(mass) * veh+0x1169(wheel count) * -0.75
                                             [0x003B168C, 0x003B17EC = -0.75]
F    = vhat * f                              FUN_001115C0
if (veh+0x215 not in {1,2,3} && ctx1+0x101B == 0) F *= 0.5   [0x003B1684]
FUN_001064B0(veh, F, pt)                     force_acc += F; torque += r x F
```

`FUN_001064B0` = force-at-point (`veh+0xF0 += F`, `veh+0x100 += (pt-pos) x F`
via `FUN_00106590`). Ported as `b3_crash_scrape`, differentially tested
against the real callee chain (section 7). For a 1500 kg car with 4 wheels
that is 2.2–9 kN per contact point — 1.5–6 m/s^2 — which is why a retail
wreck's slide bleeds out in a second or two.

### What was actually wrong in the harness

Measured with a private build (scratch copy of `burnout3_full.c` +
`burnout3_crash.c`, per-frame wreck trace, deterministic autodrive race,
`B3_FIXED_DT=0.0166`):

1. **The wreck frame was LEFT-handed** [the flip]. `b3_wreck_begin` built the
   frame in HARNESS space (`fwd = (sin h, -cos h)`, `right = (cos h, 0,
   sin h)`), whose determinant is **-1**. `b3_mat_orthonormalize`
   (`FUN_000FF270`) rebuilds a row from a cross product, so on the FIRST
   integration step it **negates the up row**: the trace shows
   `up.y +1.000 -> -0.986` in one frame. Every ported law then ran on a
   mirrored body (gravity torque at `up*com_height`, the inverted-car roof
   arm in `FUN_00125100`, the contact geometry). `src/burnout3_full.c` already
   documents this hazard for the racing path and keeps its rigid body in GAME
   space; the wreck now does the same — mirror in at entry, mirror out at
   exit, vectors z-negated, the pseudovectors `L` and `omega` mapped
   `(x,y,z) -> (-x,-y,z)`. Both maps are exact sign flips, so the round trip
   is bit-exact.
2. **No friction at all.** Nothing in the wreck path opposed a slide, so a
   shell coasted at 1.7–3 m/s indefinitely — and every gate in
   `FUN_00123000` (rollover crawl, settle, and through it sleep) is
   `speed < 1.0`. The wreck could never settle. The scrape above is the
   missing term.
3. **A single-corner contact cannot hold a body up.** `FUN_00106720`'s
   denominator is dominated by its angular term at r ~ 2 m and the class
   inverse inertia, so ~90% of a corner impulse becomes SPIN; the shell kept
   ~0.8 m/s of residual sink plus a permanent slow rotation. Retail supports
   the body at four points (the wheel stations). The harness now resolves
   every bbox corner inside the contact band with a projected Gauss-Seidel
   sweep (each impulse seen by the next), which is what lets it find a flat
   equilibrium.
4. **Two position corrections per frame, one of them velocity-destroying.**
   The old code pushed the shell out through the deflection channel AND again
   after integration, and the second push zeroed any downward `vel.y`. That
   pair is what turned the harness's containment sweep into a per-frame
   bobble (below). There is now exactly one correction, through the
   deflection channel `FUN_00123FD0` itself uses, averaged over the
   contributing points the way the real bottom-out cut is.
5. **The aftertouch kick rate is multiplied by the crash slow-mo.**
   `FUN_00118410` fires at most ONE corner kick per vehicle TICK
   (`0x0011817E`: gated on `veh+0x4AC2`; `0x0011818C` is an else-if chain over
   the four direction bits, so never more than one). The harness calls
   `b3_wreck_aftertouch` once per RENDERED frame, and the takedown FX divides
   the sim dt by 5 during a player crash — five kicks per retail tick. A kick
   is an IMPULSE (dt-independent) while gravity integrates per sim-dt, so the
   asymmetry alone holds a wreck up: ablation, 15 s of held aftertouch at
   dt = 1/300, **peak +64.8 m without the cadence match vs +25.9 m with it**
   (and +12.4 m at 60 Hz, against the real machine's **+10.9 m** measured by
   firing the REAL `FUN_00125100` every crashed frame under Unicorn).
   `b3_wreck_aftertouch` now banks simulated time and spends it at the retail
   cadence (`B3_AFTERTOUCH_PERIOD` = 1/60 s of SIM time, a leaky bucket, so
   at the normal frame rate every frame still kicks).
6. **`b3_wreck_aftertouch` cleared the settle counter.** Retail does not:
   only `speed >= 1.0` resets `veh+0x1354` (`0x001236E0`). Held input could
   therefore keep a settled wreck awake for ever. Removed.

Not in this module, and the single biggest injector in the user's build —
the harness's containment sweep — is written up with an exact corrective hunk
in the hand-off note (see below): `b3_sweep_sphere`'s `hit_pos` is the
CONTACT POINT on the triangle, and `src/burnout3_full.c` assigned it straight
to `wk->frame[3]`, teleporting the wreck onto the wall. Traced over a 90 s
autodrive race that fired on **75–100% of wreck frames, up to 10.6 m per
frame**, against which the module's push-out fought — the visible bobble.
With the one-line push-out fix it fires on **<1%** (genuine wall contacts).

### The second repro: +105.7 m/s of vy on a kerb clip [measured]

A later dump (130 mph into a raised median kerb, divisor-5 slow-mo, input
held) had the wreck at **vel.y = +105.7 m/s with airborne = 0** -- just under
`FUN_00109560`'s vertical cap of 120 `[0x00109595 band]`. Reproduced in the
C driver (58 m/s entry, dt = 1/300, aftertouch held, ground height 0 / +0.5 /
+1.5 m under the shell): `|omega|` climbed 4 -> 26 -> 38 -> **95 rad/s** (the
integrator clamps at 100) and `vy` climbed to +31 m/s. The ground height made
**no difference at all** -- it is not a penetration/restitution runaway, and
the entry kick is fired exactly once.

The mechanism is the CONTACT converting angular momentum into com lift, frame
after frame, while the aftertouch producer feeds the shell a verified 0.6
corner kick (about 27 rad/s of `|omega|` each). A rigid-body corner impulse
has no force bound; retail's support is `FUN_00123FD0`'s spring, whose
vertical effect in one tick is bounded by `F*dt/m`. So the GLUE box contact
is now **zero-restitution in the LINEAR channel**: it may stop a descent, it
may not raise `vy` above what the body already had (the angular channel is
untouched, so wrecks still tumble over the nose and roll onto their roofs).
`FUN_00106720` is called here with restitution 0, so this is the same
statement the real call makes, applied to the com.

Two independent checks that this is the right direction, not a fudge:

* the plain launch's rise drops from **+3.68 m to +1.95 m**, and the REAL
  machine's slam rises **+1.81 m** -- the unclamped contact had been
  inflating every launch;
* peak `vy` over a whole run is now exactly the entry kick (+6.43 m/s) for
  every kerb height and both step rates.

Known deviation, recorded rather than tuned away: with aftertouch HELD the
real machine reaches **+10.87 m** (measured: the real `FUN_00125100` fired
every crashed frame under Unicorn) where the port reaches **+3.11 m**. Retail
gets there through the spring/bottom-out pass in `FUN_00123FD0` that is
unported [S]; the harness deliberately sits under it rather than over it.

### Ported + verified

* `src/burnout3_crash.c/.h`: `b3_crash_settle_test` (`FUN_00125CF0`),
  `b3_crash_scrape` (`FUN_00123FD0` @`0x001248EA`), the full sleep gate inside
  `b3_crash_mode_frame` (both arms, the byte-wrapping counter, the
  `veh+0x211` wake from `FUN_00109560`) — all [C-disasm]; the GAME-space
  mirror, the multi-point contact solve and the aftertouch cadence are GLUE
  with the reasoning above.
* `tools/validate_crash_traj.py` **108/108**: the 44 launch/tumble cases
  unchanged, plus 9 settle-predicate, 13 sleep-gate (through the real
  `FUN_00123000`), 3 scrape and 22 at-rest cases (including the two live
  repros: the divisor-5 slow-mo and the 130 mph kerb clip at three ground
  heights) — the real wreck stopping
  dead (|v| <= 3.8e-6 m/s, 1.2e-7 m of drift over 450 frames), the harness
  wreck launching / tumbling / landing / sleeping and STAYING put (0.02 m of
  jitter, 0.00 m of travel over 300 frames), never climbing after landing,
  the slow-mo cases (exact -20*dt per airborne step at dt = 1/300), the
  kerb-clip repro (peak vy never beats the entry kick at any ground height)
  and the cadence ablation (600 kicks in 10 s of sim time with the cadence
  match = the retail 60/s, 3000 without).
* In-game (autodrive, 90 s, 12 wrecks): every wreck now lands, settles and
  sleeps with its yaw frozen and its height stable to ~3 mm, upright or on
  its roof; `build/`-style frame series in the hand-off note.

### Still open here

* crash-director `0x0064ACE8` `+0x57C`..`+0x594` launch magnitudes [?]
  (BSS, and unlike `DAT_005A80B8` there is no static-init thunk for them).
* `veh+0x4AC2`, the aftertouch enable byte: the only static writer
  (`0x00117799`, the crash-input reset at `0x00117730`) sets it to **0**, so
  whatever turns it on writes through a derived pointer [?]. The harness
  treats aftertouch as always enabled while the wreck is down.
* `b3_rigid_body_integrate` does not clear the sleep byte on `veh+0x211`
  (above), and `b3_mat_orthonormalize` still takes the wrong branch under a
  fast multi-axis tumble (§16.1) — both live in
  `src/burnout3_vehicle_sim.c`.
* The per-tick damping constants (0.99/0.95/0.9) are applied per FRAME, so
  under the crash slow-mo they damp ~5x faster per simulated second than
  retail. This removes energy, never adds it, and the at-rest cases pass at
  both rates; a fixed-rate wreck tick would close it properly.

## 16.3 CORRECTION — the crash PRESENTATION composes wrong: the whole-sequence oracle (2026-08-12)

Trigger: "the physics of crashing still feel wrong; right now the car spins
very quickly in the air after a crash. We want the realistic tumbling slo-mo
of the retail version." The user's reports had oscillated ("floaty", then
"spins very quickly") — the classic sign that every piece passes its own test
and the ASSEMBLY is wrong. It was.

Every part was already oracle-verified in isolation: the launch pair and the
crash-mode solver (§16.1/§16.2, `validate_crash_traj` 108/108), the dilation
timeline (RE_TAKEDOWN_FX §9.3/9.4) and the mode-2 camera law (§9.2,
`validate_takedown` 960/960). What was never tested is the one thing the
player actually sees: **all three running together, per RENDERED frame.**

### The whole-sequence oracle — `tools/emulate_crash_traj.py:CrashSequence`

One continuous Unicorn session that, per rendered frame, executes the real
pieces in the real order, and captures the **apparent rotation** — the angle
the shell turns between two rendered frames, which is what the eye sees.

The order is taken from the retail main loop, `FUN_000165F0`
@`0x00016ABC`..`0x00016C37` [C-disasm]:

```
0x00016ABC  if ([EBP+0x2E20C] <= 0) skip the sim entirely
0x00016AD0  do {                                   ; the SIM-TICK loop
0x00016ADC     INC [0x004A1EB4]                    ; the tick counter
0x00016B06     CALL FUN_001B5AC0 (ECX=0x0060EA00)  ; THE GAME TIMER
               ... the whole game update at dt = DAT_0060EA1C ...
0x00016BFE  } while (++i < [EBP+0x2E20C])
0x00016C10  MOV EDI,EBP ; CALL FUN_000170B0        ; camera + render, ONCE
0x00016C31  CALL FUN_001B58E0(1,4)                 ; ticks for the NEXT frame
0x00016C37  MOV [EBP+0x2E20C],EAX
```

So **`DAT_004A1EB4` is a SIM-TICK counter, not a rendered-frame counter**
(RE_TAKEDOWN_FX §1.1 called it the latter; the increments are inside the tick
loop). At a solid frame rate the governor returns 1 — one sim tick per
rendered frame at `dt = period/divisor`, which is the arrangement the harness
already uses.

Acceptance: `tools/validate_crash_traj.py` **134/134** (was 108/108), section
8. Two scenarios (a 35 m/s wall crash resolved by the real `FUN_0011AEF0`,
and a car-vs-car fatal at 40 m/s) x both presentation branches. The port's
per-rendered-frame apparent-rotation curve matches the real machine's to
**8e-06 rad (0.0005 deg)** over 180 rendered frames, position to **5e-08 m**.
The crashed-path composition itself was never the problem.

### What the oracle actually shows

| | plain crash | impact window armed |
|---|---|---|
| divisor timeline | **5, held for the whole flight** | 1x1, 5x15, **6x126**, then 1 |
| apparent rotation, peak | **2.41 deg/frame** | **11.89 deg/frame** |
| apparent rotation, mean | 0.89 deg/frame | 3.32 deg/frame |
| total swept in 5 s wall | 266 deg | 995 deg |
| camera yaw over the window | **holds, 0.0000 deg of drift** | swings **178.8 deg** |

The right-hand column is the frantic spin the user was reporting, and the
harness was taking it on **every** crash.

### Root cause 1 — the camera runs on the DILATED dt [C-disasm]

`FUN_000170B0` @`0x00017147`..`0x0001717A` composes the camera's delta:

```
00017147  CVTSI2SS XMM0,dword ptr [EDI + 0x2E20C]   ; the sim-tick count
0001714F  MULSS    XMM0,dword ptr [0x0060EA1C]      ; * the DILATED dt
0001715C  MOVSS    dword ptr [EDI + 0x2E210],XMM0
0001716F  MOV EAX,dword ptr [ESP] ; PUSH EAX        ; -> param_1
0001717A  CALL FUN_00167940
```

and `FUN_00167940`'s 20-mode dispatch chooses per mode
(`0x0016797E`, bytes `83ff0e 7407 85ff 7403 55 eb07 8b15fce14a00 52`):

```
if (mode == 0xE || mode == 0) dt = DAT_004AE1FC   ; the REAL dt
else                          dt = param_1        ; the DILATED product
call [mode->vtbl + 0x10](dt, camstate)
```

The chase camera — the player-crash camera, §9.1 — is **mode 2**, so it gets
`ticks * DAT_0060EA1C`. Only modes 0 and 14 see real time.

That is decisive, because `FUN_0015E550`'s blend exponent is
`n = floor(60*dt + 0.5)` (`0x0015E5DC`). At divisor 5 (`dt = 1/300`) and
divisor 6 (`1/360`) **n = 0**, so every blend is `1 - x^0 = 0` and the
camera's yaw and pitch **hold** for the whole dilated window — only the focus
anchor tracks the shell. Fed the undilated 1/60 instead, `n = 1`, and the
speed gate has collapsed toward 0 by then, so `blend_pitch -> 1`: the camera
SNAPS its pitch to `asin(wreck.forward.y)` every rendered frame, and a
tumbling wreck sweeps that through the full +-90 deg. The view then
counter-rotates with the tumble and the shell reads as spinning far faster
than it is. The port was passing `g_tdfx_real_dt`.

### Root cause 2 — the impact window was armed on every wreck [C]

Closes RE_TAKEDOWN_FX §9.6's `[?]`. `mode+0x52`/`mode+0x44` are written in
exactly one place, `0x00026B2D`/`0x00026B28` inside `FUN_00026A70`, and its
head is:

```
00026a7e  MOV  ESI, dword ptr [EBP + 0xc]        ; arg2 -- owns +0x174
00026a97  MOV  AL, byte ptr [ESI + 0x174]
00026a9d  TEST AL, 0x8
00026a9f  JZ   0x00026b31                        ; bit 3 REQUIRED
00026aa5  TEST AL, 0x2
00026aa7  JNZ  0x00026b31                        ; bit 1 must be CLEAR
```

`FUN_00026A70` has **no direct-call xrefs** — 5 DATA refs, all vtable slots —
and of the three `CALL dword ptr [reg+0x54]` sites in the image, two push 4
args and clean with `ADD ESP,0x10` (cdecl callbacks; `FUN_00026A70` is
`RET 8`), so **`0x001134EF` in `FUN_00112E70` is the only call site**. Its
only caller is the broad phase `FUN_00111CD0` @`0x00111D77`, on the branch
that demands one participant be **type 3** (`0x00111D2E CMP CL,0x3`) — a
traffic vehicle. And bit 3 has exactly one setter in the image,
`0x001A7584 OR byte ptr [ECX+0x174],0x8`, in the type-3 record initialiser
`FUN_001A7210`, gated on the spawn request's flag bit 0; it also caches the
record at `DAT_00739C68`, i.e. there is **one designated big-hit traffic
vehicle**. Bit 3 additionally *cancels* the normal wreck at `0x00113077` and
`0x001141B5`, and the arm block fires the scripted launch pair
`FUN_00125100(0x10, 0.50, world-up)` + `FUN_00125100(0x80, 1.60, frame.at)`
(`0x00026AEE` `680000003f`, `0x00026B09` `68cdcccc3f`).

**A plain wall crash cannot reach any of it.** `FUN_0011AEF0`'s callers are
only the vehicle's own update (`0x0011C0B7`, `0x0011C571`); it enters the
crash machine directly with cause record 0 (`0x0011B9F1 XOR EDI,EDI`). There
is no collision-pair record and no `+0x174` byte anywhere on that path.

> **A plain, ordinary race wall or car-vs-car crash keeps divisor 5 all the
> way to crash-mode exit. The 2.1 s truncation in §9.4 does not happen to
> it.** [C]

### `0x0064ACE8 + 0x57C..+0x594` are **0.0** [C] — closes §16.1's `[?]`

Not "no static writer, value unknown": the value is recoverable and it is
zero. `/get_xrefs_to` on all seven absolute addresses
`0x0064B264..0x0064B27C` returns empty; `operand_pattern=0x64b26` /
`0x64b27` return zero matches against a control (`0x64b`) of 149; a byte
search for every little-endian pointer to those addresses returns nothing
(control `e8ac6400` = `&director`: 56 hits), so no registrar, VDB entry or
CRT thunk (`0x003BDE60` is a pointer table, not code) can reach them; and no
store at those displacements exists anywhere in `0x0010D000..0x00111000`,
the only module holding the director pointer. `read_memory 0x0064B260 +48`
is all zero, and the ELF is statically linked with no section header, so the
region is loader-zeroed BSS and stays 0.0.

Since `FUN_00125100` computes `J = axis * 10.0 * mass * mag`, **every
director-sourced kick is a null impulse**. In particular the head-on wall
entry `FUN_00125100(0x02, director+0x590, up)` +
`FUN_00125100(0x10, director+0x590, up)` at `0x0010E414`/`0x0010E42x`
contributes **nothing**: a retail wall crash's tumble comes entirely from
`FUN_0011AEF0`'s own contact impulse routed through `FUN_001206D0`'s
at-point (torque) path. §16.1's *mechanism* is right; its magnitudes are not
where a wall crash's motion comes from. The kicks that do fire are the
compiled-in literals (0.65/0.90 rollover, 0.40 crash-record, 0.30, 0.60
aftertouch, 0.25 grind, 0.50/1.60 big hit).

### The per-frame damping cadence is NOT a divergence [C]

§16.2's last open item worried that applying `0.99/0.99/0.97` per rendered
frame damps ~5x faster per simulated second than retail under the slow-mo.
The oracle settles it: the sim-tick loop runs **once per rendered frame**, and
`FUN_00123000` runs per tick, so **the real machine damps once per rendered
frame too**. The harness matches retail exactly; the whole-sequence
differential (8e-06 rad over 180 frames) is that statement measured.

### Ported + verified

* `src/burnout3_takedown.c/.h`: `b3_tdfx_crash_camera_x` now runs on the
  dilated dt the timer produced (`b3_tdfx_sim_dt()`); the impact window's arm
  carries `FUN_00026A70`'s gate (`b3_tdfx_qualify_big_hit` /
  `b3_tdfx_impact_hit_object`, `(f & 8) && !(f & 2)`), so a bare wreck no
  longer truncates its own presentation; and `b3_tdfx_crash_begin` posts the
  **CRASHED!** callout (0x7F) ahead of the credit gate, because the sign
  chain (`FUN_001994D0` -> `FUN_00199350`, accept test `0x001993EF`) is
  independent of the dilation credit and shows on every crash.
* `tools/emulate_crash_traj.py`: `CrashSequence` + `seq_wall35` / `seq_slam`.
* `tools/validate_crash_traj.py` **134/134**, `tools/validate_takedown.py`
  **973/973** (13 new: the camera-dt bytes, the mode dispatch, the arm gate
  and its four port cases, `b3_cam_substeps` at 1/300 and 1/360).
* In-game (private instrumented copy, forced wreck, autodrive): divisor 5
  held for the whole crash, camera yaw/pitch frozen at (-68.57, +0.39) for
  all 1500 wreck frames, apparent rotation <= 0.77 deg/frame, and the
  CRASHED! sign up.

### Still open here

* **[?]** The un-truncated branch runs the wreck's whole recovery clock at
  divisor 5. `FUN_00198E60` @`0x00198F65` stamps recovery as
  `racecar+0x10DC (the DILATED clock) + 5.0`, i.e. **5 GAME seconds = 25 s of
  wall time** at divisor 5, which is what the harness now shows. Either an
  unmodelled releaser fires sooner (`FUN_00119C00`'s gate `racecar+0x19BE`,
  set by `0x0018D740`, is the prime suspect and is not recovered), or retail
  relies on the player releasing boost. This is the one composition number
  still unpinned and it is the top follow-up.
* **[?]** Which crash-entry site fires for which crash kind. The harness's
  `b3_wreck_begin` pairs a 0.40 corner torque (`0x00024F94`, `FUN_00024F10`,
  TORQUE-ONLY since flags 0x0A) with a 0.65 linear launch (`0x0011C421`,
  which belongs to the **rollover** entry) — a mix of two different sites.
  With the director magnitudes now known to be 0.0, the wall crash's real
  answer is "neither, only `FUN_0011AEF0`'s impulse".
  `b3_wreck_begin_kick()` already takes both magnitudes explicitly, so the
  per-kind table can be wired from the caller without touching the module.
* **[C]** The type-3 request producer is TDESC's state-6 schedule table.
  Ghidra MCP proves `FUN_001A2B20` forwards `[EBP+0x3c]`
  directly to `FUN_001A7210`, and `FUN_001A5C70` sources that argument from
  byte `+0x1B` of a distinct 0x20-byte runtime request record. It is not
  TDESC's 0x20-byte transform table, whose byte `+0x1B` is direction-Z's float
  encoding. `FUN_001A13F0` writes the manager's `+0x363B8` queue-mode byte
  directly from `TDESC+0xB4 & 4`; `FUN_001A5880` selects `FUN_001A5C70` only
  in that mode. `FUN_001A3AE0` state 6 reads TDESC schedule row `+0x38/+0x3C`
  as 0x0C descriptors and writes each descriptor address to
  `manager+0x30+slot*4`, using descriptor bytes `+0x0A/+0x0B` as manager and
  slot. The descriptor's record pointer is `+0`, record count is `+8`, and
  its 0x20 records carry the designation flag at `+0x1B`. In normal mode
  `FUN_001A5910` supplies zero, and
  `FUN_001A6070` likewise pushes zero; its local `0x0B` is the preceding
  companion-type argument, not a designation. The decoder finds 926 static
  descriptors / 3,005 records across 377 event TDESCs and every `+0x1B & 1`
  is clear; the designation writer is therefore runtime-only or absent from
  the shipped modes. Payload semantics and lifetime policy remain open.

## 16.4 CORRECTION — the wall-crash TRIGGER, and what `-0.7071` actually gates (2026-08-12)

§16's walkthrough of `FUN_0011AEF0` records the wall-crash trigger correctly
(`dv > authority*27.5`, `headon > authority*0.707`). Two things were wrong
downstream of it and both are now fixed in the ported modules.

**1. The harness never ran it.** `b3_crash_response` had the whole test
inside it, but nothing in `src/burnout3_full.c` ever called
`b3_crash_response`: `mesh_collide` is a sphere push-out and it derived its
own crash decision from `vin = -(vel . n)`, a CENTRE-OF-MASS closing speed,
against a fixed `26 m/s` (`4` when out of control). Retail's `dv` is
`|FUN_00106720(...)| / mass` at the CONTACT POINT, so `omega x r` is in it —
a spun car whose tail swings into a barrier has a large `dv` and almost no
`vin`, and that is exactly the state a slam leaves its victim in. The tail of
`FUN_0011AEF0` is now factored into `crash_wall_core` and exposed as
`b3_crash_wall_eval`, which scores a caller-supplied contact with the real
solver and the real gate. Details, the authority scale that gives the gate
its whole dynamic range, and the full parity table: docs/RE_TD_RULES.md §12.

**2. `-0.7071` is a LAUNCH selector, not a crash gate.** The head-on gate the
harness applied (`dot(at, n) < -0.7071`, cited to `FUN_0010DD20`) reads a real
constant — `[0x003B1DE0]` = `-0.707106769` — but the branch it guards is only
the crash-entry launch:

```
0010e3d3  CALL FUN_0010E510          contact vector from veh+0x160
0010e3e5  CALL FUN_00013C60          XMM0 = dot(that, frame.at)
0010e3ea  MOVSS XMM1,[0x003B1DE0]    -0.70710677
0010e3f2  COMISS XMM1,XMM0
0010e3f5  JBE  0x0010E42A            ---> skips ONLY the FUN_00125100 pair
0010e42a  *(rec+0x114) = 0xB         the crash proceeds regardless
```

`FUN_0010DCA0` has no magnitude test whatsoever (it resolves the mode and the
racecar slot, then always calls `FUN_0010DD20`), and `FUN_0010DD20`'s only
refusal is `if (*(rec+0x130) > 0.0) return` — do not re-enter while the
previous crash timer runs. **The caller's decision is final**, and for walls
that caller is `FUN_0011AEF0`. Retail's head-on requirement lives there, as
`headon > authority*0.707`, and it is scaled rather than skipped when the car
is out of control.


---

## 16.5 CORRECTION — `FUN_0011AEF0` runs INSIDE the substep, and `FUN_00106720` returns a SIGNED impulse (2026-08-14)

Two findings, one of which is a bug that had been in the port since wave 1.

### 1. The substep's real shape

`FUN_0011BE50`'s loop body, read out of the instruction stream rather than the
decompiler:

```
0011c0a0  push esi; push ebx; call 0x11d460     FUN_0011D460  tyre force pass
0011c0a9  mov byte [ebx+0x212], 0
0011c0b0  mov byte [ebx+0x213], 0
0011c0b7  call 0x11aef0                         FUN_0011AEF0  chassis contact
0011c0bc  test eax,eax / je 0x11c0d3
0011c0c0    mov dword [ebx+0x1524], 3                         forced drift 3
0011c0ca    mov byte  [ebx+0x212], 1
0011c0d3  cmp dword [ebx+0x1524], 3 -> mov 0                  release the latch
0011c0e6  push ebx;           call 0x1239c0     FUN_001239C0  suspension pre-pass
0011c0ec  push esi; push ebx; call 0x123fd0     FUN_00123FD0  suspension forces
0011c0f3  0.5 [0x3b1684] > +0xBC && 0.1 [0x3a69c4] >= +0x1400,
          or [[+0x13F4]+0x179C] in {0,2}
          -> zero +0xB0/+0xB4/+0xB8/+0xBC/+0xF0/+0xF8          near-stop check
0011c15d  push esi; mov ecx,ebx; call 0x109560  FUN_00109560  INTEGRATE
0011c165  dec edi; jne 0x11c0a0
```

and the polygon soup `FUN_0011AEF0` walks (`veh+0x200`) is filled ONCE per
frame by `FUN_0011BC60` @0x0011BF43, **outside** the loop.  The record layout,
from the gather loop @0x0011B000..0x0011B033:

```
[veh+0x200] -> [+0x00] int    poly count            mov ecx,[eax]
               [+0x04] void*  polys, stride 0x40    mov edx,[eax+4] / add ebx,0x40
               [+0x08] u16*   surface flags         mov cx,[edx+esi*2]
```

Everything `FUN_0011AEF0` produces is an **accumulator** write — `+0x110`
impulse, `+0x120` angular impulse, `+0x130` deflection, `+0xF0` force — and
`FUN_00109560` @0x0011C160 consumes and clears all four at the end of the SAME
substep.  A wall response is therefore a force inside that solve, seen by that
substep's suspension pass and integrator; it is never a correction applied to
an already-integrated pose.  `b3_vehicle_step_full` now has both call sites
(the soup freeze and the resolve) at exactly those positions, verified by 14
trajectory differentials in which **the retail instruction stream
0x0011C0A0..0x0011C16C is executed verbatim under Unicorn with EDI = 2**
(`tools/validate_port.py`, `run_relocation_cases`).

Two smaller corrections fall out:

* `FUN_00104840` is **not** the per-frame contact-scratch clear.  It does zero
  `+0x160..+0x19F` and `+0x212` (@0x00104856..0x00104888), but it also sets
  `+0x1353 |= 4` @0x00104848 — the bit `FUN_0011AEF0` tests at 0x0011AF0C to
  refuse to run at all — and its callers are `FUN_00120F30`'s out-of-unit and
  parked arms (@0x00120F7B, @0x00120FB7).  It is a reset/park path.  Nothing on
  the per-frame path clears the contact record, so within a frame it persists
  from one substep to the next.
* `FUN_00120F30` (vtable slot 0x003B11EC, installed @0x001A9946/@0x001A9950)
  **tail-calls `FUN_00123000` @0x0012138E** — so a traffic/articulated body is
  an ordinary vehicle running the same integrator loop.  Its own body is the
  streaming/sleep protocol plus the tow constraint (see RE_NOTES on PH-15 in
  `docs/PHYSICS_GLUE_LEDGER.md`).

### 2. `FUN_00106720` returns the SIGNED impulse

The tail, literally:

```
00106863  divss xmm0, xmm2          j = -(1+e)*dot(n,vp) / denom
0010686a  movss [esp+0xc], xmm0
00106870  fld [esp+0xc] / fabs / fstp [esp+0xc]
0010687e  movss xmm2, [esp+0xc]     |j|
00106882  shufps xmm2, xmm2, 0
00106886  mulps xmm1, xmm2          out = n * |j|
00106889  movaps [eax], xmm1
0010688f  ret 0x10
```

XMM0 — the caller's float return — still holds the SIGNED quotient from
0x00106863.  The `fabs` at 0x00106874 goes through the x87 stack and a memory
slot into XMM2 and only ever scales the OUTPUT VECTOR.  The sign is load
bearing at the one caller that reads it as a scalar:

```
0011b754  call 0x106720
0011b759  movaps xmm3, xmm0
0011b75c  divss  xmm3, [edi+0x1f0]   dv = j / mass
0011b764  comiss xmm3, [0x3b16e0]    vs 0.0
0011b76b  movss  [esp+0x1c], xmm3
0011b771  jbe 0x11b909               dv <= 0 -> NO impact, NO impulse
```

so `FUN_0011AEF0` still records the contact and still pushes `+0x130` out for
a chassis that is **separating** from a wall, but it does not kick it back in.
`b3_crash_impulse` in `src/burnout3_crash.c` returned `fabsf(j)` and therefore
did; `b3_contact_impulse` in `src/burnout3_vehicle_sim.c`, the wave-1 port of
the same function, always returned the signed value.  The two transcriptions
disagreed with each other, and no case in the suite could see it until the
relocation put a real separating wall contact inside a substep (the
`wall mid-corner (separating)` trajectory: the emulator applied no impulse,
the port applied one worth `veh+0x194 = 934`, and the trajectories split by
0.22 m).  Fixed; the one non-retail call site (`b3_wreck_begin_*`, handed an
already-classified crash-entry contact rather than a point velocity) takes
`fabsf` explicitly and says so.
