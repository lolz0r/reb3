# Gamedata.bgd — track gameplay data (decoded 2026-08-10)

Confidence markers as elsewhere in this repo:
`[C]` confirmed (game code address given, or an invariant that holds on all
40 shipped Tracks/*/*/Gamedata.bgd files), `[S]` strongly supported
(structure/geometry/overlay evidence), `[?]` open.

Tools: `tools/decode_bgd.py` (full decoder, `--all` runs every track),
`tools/extract_bgd_paths.py` (harness arrays, now structure-located).
Overlays: `build/bgd_loops.png`, `build/bgd_events.png` (+ the original
`build/bgd_walls.png`).

## 0. Summary of what replaced the old seam-split heuristic

The C1_V1 arrays the harness uses are unchanged, but they are now located
from the file's own directory instead of a pool scan:
section table @0x3BB8 → road-network section @0x19D800 → boundary strip
@0x19D970, forward race line @0x1ABF20 x1029, and the loops below.
`src/burnout3_track_paths.h` regenerates byte-identically (data) with an
updated provenance comment; `make` and `validate_port.py` stay green.

## 1. Container: a baked memory image with tool garbage [C]

* Header: `{u32 version = 9, u32 rootVA = 0x00320178}`. Baked base VA is
  0x00320000; the root object is at file offset 0x178. All 40 files agree.
* The file is written by Criterion's PC track pipeline as a raw dump of live
  C++ objects. Non-serialized fields keep the *tool's* memory: values like
  `0012F9xx` (Win32 stack), `7C3Axxxx`/`77F5xxxx` (XP-era DLL code),
  `7FFDF000` (TEB) appear throughout the directory region. Only dwords in
  `[0x320000, 0x320000+filesize)` are real baked pointers, and real pointers
  exist **only** in the directory region (< 0x8000); the payload sections
  cross-reference by `{offset,size}` and index tables. (Aligned-pointer scan:
  1627 candidates, all in-header ones structural, mid-file ones are u16-pair
  aliasing.) [C]
* The game cannot map the file at 0x320000 (that VA range is occupied by the
  XBE's XONLINE/.data sections and the biggest file would reach 0x5C9000, into
  live .data), so consumers use the offset/index tables, not the baked
  pointers. [C for the address-clash; the load-time handling of the two
  header pointers is [?]]
* Every 0x800-byte sector of the early blocks ends with leaked writer state:
  `{u32 last_block_size, u32 last_block_offset, sizeR, offR, sizeN, offN, 0}`
  where `(sizeR,offR)` and `(sizeN,offN)` match the section table (see §4)
  — tool archaeology, not game data. [S]

## 2. Code chain — how the game loads and consumes it

* Path builders (one per mode family), selected by `FUN_0005e8c0` (itself
  called from `FUN_0006dac0`): `FUN_0005f0a0`, `FUN_0005f4a0`,
  `FUN_0005f7b0`, `FUN_0005fb20`, `FUN_0005fd30`. Each fills a static
  track-load record and appends `"Gamedata.bgd"` (string ptr `0x003eaf30`)
  to `"tracks/<REG>/<Cn>_<Vn>/"` built by `FUN_001574f0` from the packed
  track ID (table `DAT_004d3408`, 8-byte stride). [C]
* The records are C++ objects in BSS with vtables installed by the static
  initializer at `0x00257580` (`ESI = 0x004A71A0` manager singleton;
  installer `FUN_00015790`):
  - record `DAT_004d4008` (no-track) vtable `0x3a9e7c`
  - record `DAT_004d4798`            vtable `0x3a939c`
  - record `DAT_004d4968` (the Gamedata.bgd record built by FUN_0005fd30)
    vtable `0x3a9e5c`; methods `FUN_0001a910..FUN_0001af30`
    (+0x18 returns field +0x1CC, a small config value — not the image ptr)
  - mode-handler singletons stored at record+0x1B8:
    `4D4E54/4D4EA0/4D4CF4/4D4DFC` (FUN_0005f7b0 modes 0-3, vtables
    `3a9fb8/3a9b18/3a9c40/3aa130`) and `4D4F14/4D4F78/4D4FFC/4D5070/4D50E4`
    (FUN_0005fd30 modes 0-4, vtables `3a9528/3aa240/3a9ea0/3a9280/3a98a0`).
    Nearly all runtime access to track data goes through virtual calls on
    `*(DAT_004d5370 + 0x1b8)` — DAT_004d5370/DAT_004d5374 point at the
    current/pending track record. [C]
* `FUN_00017450` (record pump) publishes the request to a global block:
  path → `DAT_00735418` (0x100 bytes, flag `DAT_00735549`), current 64-bit
  event ID (record+0x70) → `DAT_00735518/1C` (flag `DAT_0073554A`). [C]
* After load, `DAT_00735524` points at the **matched event record inside the
  image** (see §3 for the fields read through it) and `DAT_0073552C` at a
  second in-image struct (+0x3EC count gates a per-player table). The
  writer of these globals was not located (indirect/streamer side). [?]

## 3. Event/mode directory — 18 slots [C]

IDs are 12-char strings packed base-40 (charset `" -/0-9A-Z_"`), the same
encoding as vlist.bin car IDs; encoder `FUN_001aeaa0`, decoder
`FUN_001aecc0`. [C]

* `+0x08`: 18 × u64 packed event/mode IDs.
* root object @0x178, its field @file 0x198: 18 × u32 file offsets of the
  0x800-byte **event param records** (always 0x3000 + slot*0x800).
* **event spatial records** pair with slots at 0xC000 + slot*0x800. [S]
* Unused slots keep stale tool memory in both tables (the repeated junk IDs
  like `' -AOQ510G7ST'` across files are leftovers, not data). [C]

Mode-default IDs come from the game's mode-name table at `.data 0x3E9CD8`
(used by `FUN_0003c8a0`): `[Off|On] + [SgRc|LpEl|BtRc|RRge|BrLp|Srvl|Crsh]
+ [F|R]` = Offline/Online, {Single Race, Lap Eliminator, BtRc [?], Road
Rage, Burning Lap, Survival [?], Crash}, Forward/Reverse route. C1_V1
carries `OffSgRcF, OnSgRcF, OffBtRcF, OffRRgeF, OffBrLpF` + 13 World-Tour
event IDs of the form `<code><REGION><ROUTE>` (e.g. `1G000JASC1V1`). [C]

### Event param record (0x800 bytes), fields with code evidence

| offset | type | meaning | evidence |
|---|---|---|---|
| +0x00 | 2×u64 | assigned car ID, split base-40 halves (e.g. `HEVYCAR4`, `MSCLCAR4`) | [S] decode across all events |
| +0x3A0 | f32 | gold threshold | [C] `FUN_001986a0` (score ≥ → medal 2), also `0x2455c/0x24692/0x24a4b/0x24d75` |
| +0x3A4 | f32 | silver threshold | [C] same readers |
| +0x3A8 | f32 | bronze threshold | [C] same readers (`-1 & 4` bronze flag) |
| +0x3AC | u32 | target count; `FUN_0017e030` computes `target - takedowns` remaining. Road Rage default = 15 takedowns | [C] |
| +0x3B8 | u32 | small int read via record method stub `0x1a8d0`; 3 on race events/modes, 1 on single-lap events, 0 on RRge/BrLp/BtRc → lap count | [S] |
| +0x3EC | u32 | count gating a per-player 0x10-stride table (`FUN_0018d0e0` via `DAT_0073552C`) | [S] |
| +0x10.. | f32[] | per-opponent/AI factor tables (0.78-0.99 ranges) | [?] |

Semantics check across C1_V1: crash-style `1G…` events carry $-scale medals
(550000/275000/75000), the burning-lap style `0E0503…` carries ascending
times (78/90/105), `OffRRgeF` carries 7/4/2 takedown medals. [C-data]

### Event spatial record (0x800 bytes at 0xC000 + slot*0x800)

* 6 × 0x50-byte **start-grid slots**: `{3 rows of [f32 x,y,z,pad] rotation,
  [f32 x,y,z,0] position, u32 node_index, pad}`. Rows are unit vectors; the
  six positions sit on the road in grid formation at the start line, all
  facing race direction (overlay `build/bgd_events.png`). 6 = the game's
  race field size. C1_V2's grid sits at a different location with its own
  heading (958,18,1707 fwd(-0.97,0.23) node 37 vs C1_V1's 1103,5.8,518
  fwd(-0.93,0.37) node 28) — route direction lives HERE, not in the shared
  geometry pools. [S, overlay-verified]
* +0x1E0.. : further `{pos, f32 ~100-106, dir}` records — finish/cameras/
  zone anchors. [?]

## 4. Section table and per-mode blocks

At 0x3BB8 (root-referenced): `{u32 3, then {size,offset} pairs}`:
`(0x800,0xC800)`, `(0x7800,0x9D000)`, `(size_R, off_R)` = **route-index
section**, `(size_N, off_N)` = **road-network section**. Invariants on all
40 files: `off_N + size_N == filesize` and `off_R + size_R == off_N`. [C]

* `0x18000..off_R`: **per-mode blocks**, dword size table at 0x660 (tiles
  exactly on many files; on others the table parse is incomplete → the
  decoder falls back to signature scans). Each block contains, among other
  things [S]:
  - a **traffic-vehicle set**: 0x18-byte records `{u64 packed car ID, …}` —
    e.g. C1_V1 block 0: COMPCAR14, COMPCAR12, COMPCAR17, HEVYCAR14/15/12/…
    (compacts + heavies), later blocks carry 12-car sets; a `TSPCCARn`
    special-traffic id sits at block+0x18 region on C1.
  - a 71 × 0x20 `{pos[3],0, dir[3],0}` point table (C1_V1 @0x1C450):
    positions with headings on the roads and clustered at side-street
    junctions → traffic spawn/entry points incl. respawn direction; both
    race directions appear. [S, overlay]
* `off_R..off_N`: **route-index section**: u16 pair/index tables plus
  sampled sub-path polylines (C1_V1: open segments x566 @0x17C3A0,
  x482 @0x17E700, x472 @0x189C40/0x18E8E0, x468 @0x177250 live here, NOT in
  the network section — they are per-route sections, e.g. Road-Rage/crash
  stretches). [S]

## 5. Road-network section (the pools) [C structure]

Header at `off_N` (C1_V1 0x19D800):
`{u32 a=3?, u32 0x14, u32 node_count, u32 0x30, u32 nsec, u32 idx_rel,
u32 relA, u32 relB}` — `node_count` 625..1830 per track, `nsec` 7-8,
`idx_rel` → index directory, `relA/relB` → tail tables (arclength LUT of
`{f32 distance, u32 1}` pairs at relA). [C: consistent on all 40 files]

* `+0x20`: nsec × 0x10 **route-section records**
  `{u32 start_node, f32 length_m, f32, f32}` + u32 terminator (825).
  C1_V1 starts {0,110,231,319,419,592,711,(825)} with lengths summing to
  ~5.3 km ≈ lap length → checkpoint/timing sections. The extra record with
  start 176 and the exact field order is unresolved. [S]
* `idx_rel` points to `N+0x14`'s index directory, relocated in-place by
  `FUN_00158DE0`. Each `0x10` row is
  `{u32 pair_rel, u32 edge_rel, u32 link_rel, u16 node_count, u16 flags}`;
  all three offsets are relative to the row itself. `FUN_00158DE0` adds the
  row address to each offset, and `FUN_0018B250` installs those rows directly
  in the `DAT_0060EA30` section directory. [C]
* `row+pair_rel` is `node_count` pairs of `{u16 point_a, u16 point_b}` into
  the 16-byte point pool at `N+0x20`. `row+link_rel` is `node_count` records
  of `{u16 ?, u16 ?, u8 forward_section, u8 reverse_section,
  u16 forward_node, u16 reverse_node}`. `FUN_00175B10` follows the forward
  `+4/+6` or reverse `+5/+8` pair; `0xFF/0xFFFF` terminates an end of graph.
  The low three bits of the high byte of the second u16 are a route gate:
  register-accurate `FUN_00173C60` scans its caller-supplied node count and
  rejects it when `(link+3 & 7) == 5`; `FUN_00176290` passes one node before
  applying `FUN_00178310`'s separate eight-node selector span. It uses that
  gate to choose an
  available forward/reverse successor, retaining navigator `+0x1D4/+0x1D8`
  and copying the separate `racecar+0x18CA` route-selection byte to navigator
  `+0x290`. The `edge_rel` table is
  not read by either function; it remains undecoded but is not the missing
  branch decision. [C, Ghidra MCP]
* `N+0x0C` counts 12-byte look-ahead planning records at `N+0x1C`
  (`DAT_0073A17C` / `DAT_0073A16C`). `FUN_001772A0` matches byte `+8` to the
  active graph section, orders candidates by `u16 +2` relative to the current
  node, and consumes `{u16 +0,+2,+4,+6, u8 +8,+9,+10,+11}`. The first three
  u16s are node indices. `+6` is promoted directly to float by
  `FUN_001772A0` and shares `AI+0xA08`'s native m/s ceiling domain in
  `FUN_00176150`; `+10` provides the observed type flags. The exact approach
  projection used before the brake interpolation remains pending. [C]
* `+0x170`: **boundary strip**, 1752 × 16-byte `{x,y,z,0}`: two interleaved
  wall strands (even/odd), mean corridor width 14.5 (unchanged finding,
  `build/bgd_walls.png`). [S]
* then misc small arrays, then four contiguous loops ending exactly at the
  index directory (`idx = off_N + idx_rel`):

| C1_V1 offset | count | classification | evidence |
|---|---|---|---|
| 0x1A8540 | 926 | **reverse-direction drive line** (closed lap, opposite winding to the race line, on the other side of/parallel to the road; oncoming-traffic line in Forward events, drive line for the `..R` reverse modes) | signed area +1.28e6 vs −1.10e6 for the race line; 21-51 u lateral offset; `build/bgd_loops.png` [S] |
| 0x1ABF20 | 1029 | **forward race line** (the harness centerline) | original overlay + this decode [C-visual] |
| 0x1AFF70 | 910 = node_count | **node-paired outer hull loop** | count == header node_count on ALL 40 files; brackets the corridor at ~+70 u outside [C-pairing, purpose S] |
| 0x1B3850 | 910 = node_count | **node-paired inner hull loop** | same, ~-70 u inside |

The two hull loops wind WITH the race direction and enclose the road ribbon
between them (A[i]↔B[i] horizontal span 92-207 u, mean 148). Purpose
candidates: minimap/track-ribbon outline, out-of-bounds/reset hull, camera
bounds. **Not** road edges (those are the 14.5-wide strip), **not**
AI-line variants (they leave the drivable surface), **not** reverse-route
specific. Purpose stays [?].

* `idx`: **index directory**: rows `{u32 off1, u32 off2, u32 off3,
  u32 flags<<16|count}` — offsets relative to `idx`. C1_V1: rows with
  count=node_count(910)|flag1 (four of them: per-node parallel tables),
  counts 197/381/198/104 and eleven rows with counts 9..40 (per-event point
  lists). Element data is u16/u8 packed (node links/lane info). [S]

### Falsified / corrected

* RE_NOTES §9 said the 926-pt loop is "2D pts, height in w". Wrong: records
  are plain `{x,y,z,0}` like every other pool array — the earlier read was
  8-byte misaligned (0x1A8538 instead of 0x1A8540). [C]
* "C1_V2 uses its own data for the reverse route" as a discriminator:
  falsified in the useful sense — C1_V2's five pools are content-identical
  to C1_V1's (same counts, bboxes, signed areas; only file offsets shift).
  Direction/mode selection happens via the mode records (`..F`/`..R`) and
  index tables, not via different geometry. [C]

## 6. Traffic [C code chain, S data binding]

* Traffic vehicles are `.btv` files (reduced config registered by
  `FUN_00134AC0`, 9 params at +0x88..+0xA8 — RE_NOTES §10).
* `FUN_001a13f0` is the traffic manager state machine:
  - state 1: grabs arena buffers (`FUN_00018bb0`) for slots iterated over
    `0x3fa024..0x3fa0e4`,
  - state 2: walks a list `{ptr = *(obj+0x54), count = *(obj+0x58)}` of
    **0x18-byte records with a packed base-40 car ID at +0x00** and stores
    the IDs into its own slot array (+0x34390 area, count +0x34450),
  - `FUN_001a4260` then builds `"<car name>.btv"` (`0x7674622e`) per entry,
    loads it via the async loader `FUN_00011240` into an arena buffer and
    relinks it with `FUN_000310f0` (same relinker as .bgv). [C]
* **Alternate type-3 schedule.** TDESC `+0x4C/+0x50` is a `{ptr,count}` table
  of `0x58`-byte schedule rows. `FUN_00158B70` relocates the table then calls
  `FUN_00158D10` once per row, which relocates its internal pointers.
  `FUN_001A13F0` sets traffic-manager mode `+0x363B8` when `TDESC+0xB4 & 4`;
  in that mode `FUN_001A5880` selects `FUN_001A5C70`. Stage 2 of
  `FUN_001A3AE0` maps a row's `+0x04` source-table addresses through parallel
  byte maps at `+0x08` (manager) and `+0x0C` (field), count `+0x40`, into the
  `0x118`-byte manager records. State 6, independently, reads row
  `+0x38/+0x3C` as 0x0C descriptors and stores each address at
  `manager+0x30+slot*4`; descriptor `+0` is a 0x20-record pointer, `+8` its
  u8 count, and `+0x0A/+0x0B` manager/slot. Record `+0x1B` is the designation
  flag passed to `FUN_001A7210`. `BGD.tdesc()` now exposes both stage maps and
  descriptors: 6,033 rows across 377 available event TDESCs have bounded
  pointers/maps. The 926 descriptors contain 3,005 static request records;
  every `+0x1B & 1` is clear, so designation is runtime-only or absent from
  the shipped modes. Payload semantics and recycle policy remain `[?]`.
* The 0x18-stride ID records match the **traffic sets found in every
  per-mode block** of the .bgd (§4) — layout identical (id64 at +0x00,
  stride 0x18). The runtime pointer `*(obj+0x54)` was not traced to its
  in-file source yet, so the binding .bgd-block → FUN_001a13f0 list is [S].
* Traffic routing: `FUN_001A20F0` drives 0x50-byte road agents through
  `FUN_0019F560` (speed), `FUN_0019F1C0` (persistent path-distance advance),
  `FUN_0019FFA0` (clamped rows `floor(cursor)-1..+2`, two u16 point IDs per
  row, 16-byte points). It first blends each row pair at agent `+0x34`, then
  evaluates the uniform cubic B-spline coefficients
  `{(-P0+3P1-3P2+P3)/6, (P0-2P1+P2)/2, (P2-P0)/2,
  (P0+4P1+P2)/6}` at `{t^3,t^2,t,1}`), and
  `FUN_0019F3B0` (occupancy), then invokes `FUN_001A6B40` for physical traffic
  bodies and `FUN_001A8640` for trailers. The route agent therefore persists
  independently of a coupled body's streaming sleep. The reverse-direction
  line (0x1A8540) remains the harness's extracted oncoming-lane stand-in;
  each `DAT_0060EC2C + path_id*0x4C` entry is `{pair_rows, distances, branch_rows,
  point_base, count}`. `FUN_00158CC0` relocates 0x14-byte source descriptors
  and `FUN_0018B250` seeds this table. The source is the active event's RIDX
  image at `param+0x3CC/+0x3D0`: its header is `{descriptor_rows, point_base,
  path_count}` and `BGD.traffic_paths()` validates all paths (21 on
  US_C3/OFFSGRCF). `FUN_0019F1C0` retires an agent at the descriptor end;
  `FUN_001A09F0` chooses a speed profile there rather than linking to a new
  path, so these are traffic-pool segments. `FUN_001A0750` reads `branch_rows`
  as `count x 0x12` records: it selects one of four `{u16 target_row at +0x00,
  u8 target_path at +0x0C}` columns according to agent direction/mode and
  hands the selected descriptor and cursor to `FUN_001A9040`.
  `FUN_001A20F0` initializes that branch-attempt mode (`agent+0x48`) from the
  selected racecar's `+0x1920` mode field, and `FUN_001A8EE0` defers the
  reassignment until the selected source row is reached. `FUN_001A28B0` also
  consumes TDESC `+0xA4/+0xA8` as 0x18-byte progress windows: `+0x04/+0x08`
  is an inclusive progress range and `+0x00` points to `+0x14` six-byte
  `{first_row,last_row,path_id,direction}` requests. It processes the current
  plus two preceding windows with circular wrap. `traffic_paths.bin` v3
  preserves those windows and requests alongside the raw link rows and relocated
  point/pair/distance rows; the harness validates and loads them, then uses distance rows
  for the persistent cursor, blends each pair at the centre and applies the
  verified four-row uniform cubic B-spline, then reseeds at a segment end.
  `FUN_001A2B20` seeds road-agent `+0x34` with `0.5 + (rng % 1000 - 500) *
  0.0001`; the harness preserves that 0.45..0.5499 lateral range, though not
  the retail RNG sequence, cross-descriptor occupancy branch reassignment, or
  physical-pool assignment policy. `FUN_001A03F0` / `FUN_001A0600`'s same-descriptor owner
  chain is mirrored from the cursor rows: each row names its nearest agent at
  or ahead of it, and the speed update follows that linked agent. Spawn/entry
  points with
  headings come from the
  71×0x20 tables in the mode blocks; per-node lane/link data lives in the index
  directory tables.
  [C]/[S]

### 6.1 The spawn policy — how many cars, which cars [C, 2026-08-16]

A pool request is **not one car**. `FUN_001A3470` walks the request's row range
one *manager-record section* at a time and hands each section to
`FUN_001A6070`, which decides the population, the placement, the vehicle and
the speed. All of it is driven by shipped TDESC tables.

| law | address | rule |
|---|---|---|
| the RNG | `FUN_00048760` @0x00048760 | `s' = (s<<16) + (int16)(s>>16) + c; c' = c + s'`; uniform = `(float)(uint32)s' * 1/2^32`. The traffic manager's own pair is `0x00649B28/0x00649B2C` (= manager `+0x36348/+0x3634C`, manager base `0x006137E0`) and `FUN_001A3EA0` @0x001A3EA7/0x001A3EB1 seeds it with `state = 0xFD462907`, `carry = 0x02B9D6F8`. The `1/2^32` scale at `_DAT_0054F46C` is installed by the ctor @0x00264880 from `0x003B191C`. |
| population | `FUN_001A6070` @0x001A6183 | `rate_sum = Σ rate[0..5]`; `v = speed_mph * 0.44704` (@0x003A5958); `n_f = (span_m / v) * (1/60) (@0x003B1838) * rate_sum`; skip if `n_f < 0.5` (@0x003B1684); `spacing = span_m / n_f`; `n = (int)(n_f + 0.5)`. The rate table is literally **vehicles per minute**. `span_m` is the difference of the path's cumulative-distance rows. |
| placement | `FUN_001A6070` @0x001A6240..0x001A6338 | `x = uniform()*spacing` (or exactly `spacing` when `0x00649B97`/`0x00649B98` are set); `row = low + i*x`; `row += row*(2*uniform()-1)*0.30` (@0x003B1750); clamp to `[low,high]`; then the occupancy veto. Note the mixed units are retail's own: metres are added to a row index and the clamp does the rest. |
| occupancy veto | `FUN_001A6610` @0x001A6610 → `FUN_001A4150` @0x001A4150 | `DAT_00498D80[row>>2] & (1 << ((row&3)*2 + (direction==0)))` — two bits per row, gated on manager `+0x363B9`. |
| vehicle CLASS | `FUN_001A6590` @0x001A6590 | `r = uniform() * rate_sum; acc = 0; for cls in 0..5 { acc += rate[cls]; if (acc > r) return cls; } return 1`. The 6-float table lives at `[record + slot*4 + 0x10]` and is indexed **by class code**; index 0 is unused and is 0.0 in every shipped row, which is what keeps `FUN_001A5E30`'s divide-by-zero class-0 path unreachable. |
| MODEL in class | `FUN_001A5E30` @0x001A5E30 | jump table @0x001A5F10, index `class-1`: `1→TDESC+0x54, 2→+0x60, 3→+0x78, 4→+0x84, 5→+0x6C, 0xB→+0x90` — i.e. the six lists this repo already extracted, **but the class order is not the list order**. Each list is `{ptr, count, weight_total}`; `count == 1` returns the only record with no draw, otherwise `k = rng % weight_total` walked against the running sum of each 0x18 record's u32 weight at **+0x10** with a `jle` (`k <= acc`) compare @0x001A5EF4. That `<=` is an off-by-one: the first entry gains one ticket and the last loses one, so on a two-entry list of equal weights (US_C3_V1's `COMPCAR11`/`COMPCAR12`) **the second entry can never be drawn**. Reproduced under Unicorn. |
| PAINT | `FUN_001A5F90` @0x001A5F90 | `k = rng % 100` against eight cumulative percentage bytes at record **+0x08..+0x0F** (same `<=` compare). US_C3_V1 ships `10,18,18,18,18,18,0,0` for most records. |
| cruise SPEED | `FUN_001A6070` @0x001A64B1..0x001A64E3 | `agent+0x00 = speed_mph * (1 + (2*uniform()-1) * 0.15)` (@0x00384A80), the mph coming from `*(float*)[record + slot*4]`. US_C3_V1 runs 30..60 mph per carriageway. |
| tractor pairing | `FUN_001A6070` @0x001A63A6 | class 4 always draws a class-0xB partner with its own model+paint draw, then both ids go to `FUN_001A2B20`. |
| the MODEL stamp | `FUN_001A2B20` @0x001A2BD8..0x001A2C08 | the drawn packed base-40 id is matched against the manager's loaded set (`manager+0x34390`, 8 bytes/entry, count `manager+0x34450`); on a hit the set index goes to `body+0x176` and the loaded `.btv` config pointer to `body+0xB0`. The model is therefore a property of the request, not of the recycled slot. |
| the pool CAP | `FUN_001A13F0` @0x001A1A37, `FUN_001A2B20` @0x001A2B60 | `manager+0x363AB = 0xFE` (254) is the physical-body capacity; `manager+0x363AC` is the live count (`inc` @0x001A3945, `dec` @0x001A41BF/0x001A41DF/0x001A21B7). The gate is `if (capacity < live + 2) refuse` (+3 for class 7), so **252 live bodies** is the ceiling — it is not what limits the on-screen population; the population is emergent from the law above. |

**The (path,row) → (manager record, slot) binding.** `FUN_0019E5B0` @0x0019E5B0
returns the binding with the largest `start_row <= row` (and `FUN_0019E640`
@0x0019E640 the smallest `start_row > row`, i.e. the section end), scanning the
path descriptor's `desc+0x06[]` (record) / `desc+0x1E[]` (slot) arrays with the
count at `desc+0x48`. Those arrays are *built* by `FUN_001A13F0`
@0x001A1BF3..0x001A1C40, which inverts the real source:

* **TDESC +0x3C / +0x40** — the manager-record table, `0x10`-byte rows
  `{u32 path_ids_rel, u32 start_rows_rel, u32 slot_count, u8 flags, ...}`.
  `FUN_001A5680` @0x001A5680 parks the row pointer at `record+0x40` and derives
  `record+0x114` from its `+0x0C` flags. US_C3_V1/OFFSGRCF: **34 records over
  21 paths**, 1-2 slots each (a slot is one carriageway).
* **TDESC +0x4C / +0x50** — the `0x58`-byte schedule rows `FUN_001A3AE0` walks.
  Row 0 (trigger `row+0x50` = 0) is the state installed before the race:
  - stage 2 `{ptr +0x04, mgrmap +0x08, slotmap +0x0C, count +0x40}`, 4-byte
    entries = **cruise speed in mph** per (record, slot), landing at
    `record + slot*4 + 0x00`;
  - stage 3 `{ptr +0x10, mgrmap +0x14, slotmap +0x18, count +0x44}`,
    **0x1C-byte entries = seven floats**, `[0..5]` = the per-class rates,
    landing at `record + slot*4 + 0x10`;
  - stage 4 `{ptr +0x1C, ..., count +0x48}`, 0x14-byte entries whose `+0x10` is
    a modulo cycle count (`idiv` @0x001A646D) driving agent `+0x47`;
  - rows 1..n are **progress-keyed updates** — the tail of `FUN_001A3AE0`
    copies the *next* row's `+0x50` (u16) / `+0x52` into
    `manager+0x363A4/+0x363A6` as its trigger. The trigger domain is `[?]` and
    the harness applies row 0 only.

`traffic_paths.bin` v4 carries all of it (`tools/extract_traffic.py`
`traffic_mix()`), and `tools/validate_traffic_mix.py` executes
`FUN_00048760`, `FUN_001A6590`, `FUN_001A5E30`, `FUN_001A5F90`,
`FUN_0019E5B0` and `FUN_0019E640` under Unicorn against the recovered model
(848 checks). Replaying the population law over the shipped windows gives
**9 / 43 / 23.4 cars (min/max/mean) per racer's current-plus-two-preceding
window group** on US_C3_V1 — the number the harness now reproduces.

  The allocator boundary is now also located: `FUN_001A3470` stamps each
  request's inclusive source-row range into `DAT_00498D80`, while
  `FUN_001A2B20` gets a physical traffic body from `FUN_001A38F0` and a road
  agent from `FUN_001A3A10`. Both pop their respective singly linked free-list
  heads (physical `manager+0x36364`, agent `manager+0x3636C`), but release
  differs: `FUN_001A41A0` appends physical bodies at the tail (FIFO reuse),
  while `FUN_001A3A80` pushes road agents at the head (LIFO reuse). The harness
  now has a tested representation of that split lifecycle but does not yet
  apply it to persistent bodies.

## 7. What stayed open [?]

* The exact reader of the network-section arrays (virtual dispatch through
  the mode handlers hides it statically; next step: Unicorn the handler
  vtable slots `+0x40/+0x94/+0x9C` with a loaded image).
* Purpose of the node-paired hull loops (minimap ribbon vs bounds).
* The `(0x800,0xC800)` and `(0x7800,0x9D000)` section pairs' semantics.
* Field order of the route-section records (the 176-start record).
* The writer of `DAT_00735520/24/2C` (streamer side).
* Mode-block internal layout beyond the traffic set and spawn table
  (the u16 tables at block tails, the per-mode 0x5800 blocks' role —
  online variants likely among them).
