# Burnout 3 RE — handoff

Written for whoever picks this up next. Read this before touching anything; it
will save you days, mostly by telling you which approaches are already dead.

**Goal:** a faithful recreation of Burnout 3: Takedown driven by data and logic
recovered from the retail Xbox executable.

**Honest state (updated 2026-08-10, session 2 close):** a playable, faithful
recreation. You race a 6-car grid of real liveried Burnout 3 cars (closed
bodies, spinning/steering wheels at real attach points, translucent glass,
wreck-shell damage states) from the game's REAL start grid around the real
textured Bangkok circuit, against real oncoming traffic (12 .btv cars on the
926-pt oncoming line), with the game's engine/transmission/suspension/tyre
grip/steering physics run as THE GAME'S OWN per-frame pipeline
(b3_vehicle_step_full: FUN_0011ECF0 + FUN_0011BE50's stage order over a real
vehicle struct with 4 independent wheels, multi-frame trajectory-verified —
validate_port.py 76/76 incl. 3 full-pipeline trajectory cases), the game's
scoring/boost/takedown/out-of-control/damage rules (validate_gameplay.py
61/61), the real HUD (XBE-embedded fonts + Global.txd art), real engine
loops + music, mesh-based collision, and AI running the recovered AI-driver
law + 73 recovered AI params. Every recovered equation cites its address and
has a differential case; glue is marked GLUE; unrecovered items are [S]/[?]
in the docs (panel attach matrices, AI target writers, catch-up integration,
in-race HUD layout initializers, crash-mode aftermath).

Detailed, evidence-marked findings live in `docs/RE_NOTES.md`. This file is the
orientation and the war stories.

---

## 1. Setup you need

**Target file** (not in repo — user-supplied game dump):
```
$B3_GAME_ROOT/default.xbe
```

**Ghidra + MCP bridge.** A Ghidra instance runs with the `ghidra-mcp` plugin
exposing an HTTP API on `127.0.0.1:8089`. Nearly every tool here talks to it.
Check it is alive:
```bash
curl -s http://127.0.0.1:8089/analysis_status
```
Useful endpoints: `/decompile_function`, `/disassemble_function`,
`/get_function_by_address`, `/get_xrefs_to`, `/list_functions`,
`/get_function_callers`, `/get_function_callees`, `/create_struct`,
`/recreate_struct`, `/set_function_prototype`, `/get_struct_layout`.

**Python deps:** `unicorn` (CPU emulation), `Pillow` (texture export). Both
already installed.

**Build and run:**
```bash
make                                  # -> ./burnout3
python3 tools/extract_track.py        # -> build/track.obj + track.mtl
python3 tools/extract_textures.py     # -> build/textures/*.png
./burnout3                            # renders the real track
python3 tools/validate_port.py        # physics port vs real x86; must stay green
```

---

## 2. The single most important thing

**The XBE must not be loaded as a flat binary.** Doing so is silently wrong and
it invalidates everything downstream. Each section has a different
`VA − file_offset` delta, so every absolute data reference reads the wrong bytes:

| Reference | True value | Flat-load value |
|---|---|---|
| `0x003A2D50` | `2.5` | `0.0` |
| `0x00384A80` | `0.15` | `22.0` |
| `0x003A1238` | `90.0` | the string `"Score/Burnout Points"` |

`tools/xbe2elf.py` rebuilds the real address space as an ELF32 (one `PT_LOAD`
per section at its true VA, BSS materialised, `e_entry = 0x001D2807`).

**Import it with Ghidra's ELF loader and do NOT pass an explicit language ID** —
doing so forces the Binary loader and silently recreates the bug. If a fresh
project is needed:
```bash
python3 tools/xbe2elf.py "<path>/default.xbe" build/burnout3.elf
# import build/burnout3.elf with auto-detect, then:
python3 tools/apply_ghidra_types.py   # structs + prototypes, self-verifying
```

Old flat-load addresses translate as `new = old + 0x10000`, **`.text` only**.

---

## 3. What is verified

Everything here is either confirmed by two independent derivations or checked
against executing the real code. `docs/RE_NOTES.md` marks each with `[C]`/`[S]`/`[?]`.

### Executable
Image base `0x00010000`, entry `0x001D2807`, 17 sections, RenderWare RW36,
7,434 functions. 147 Xbox kernel imports named from the thunk table at
`0x0036B7C0` (`tools/xboxkrnl_ordinals.py`).

### Physics parameters — 73 across two config structs
- `FUN_00132D10` registers 64 params (`+0xB8`..`+0x1CC`, struct stride `0x1D0`)
- `FUN_00134AC0` registers 9 more (`+0x88`..`+0xA8`) — mass + suspension only,
  i.e. the reduced traffic-vehicle config
- `FUN_00132950` holds all 64 compiled-in defaults
- `FUN_00134710` copies config → live vehicle, which gave the live layout free

**Independently corroborated:** 14 of 16 spot-checked defaults appear as exact
floats in the community VDB dump (`Sokka06/burnout-data-tool`,
`data/vdb/VDB_ps2_bo3_release.XML`). The VDB is 8-byte records of
`float value` + `uint32 hashed key`; the hashing is why no parameter names
appear in any shipped data file.

Regenerate: `python3 tools/extract_physics_params.py`

### Physics equations — 4 ported and verified against real execution
`tools/emulate_vehicle.py` runs the actual x86 under Unicorn.
`tools/validate_port.py` diffs the ported C against it — **49/49 green**.

Recovered so far: longitudinal resistance (incl. its 4th accumulator component),
vertical force with downforce, gear change-up test, the full
engine/transmission update, drive-torque-to-wheels, gear engagement, the
per-wheel suspension spring/damper (with droop, bump flag, wheel spin and the
pre-pass airborne path — RE_NOTES section 10).

**Two constants no amount of reading would have found:**
- gravity is **10.0**, not 9.81
- m/s→mph is **2.2374146**, not the true 2.2369363 (Criterion's is 0.02% high),
  and the downforce term converts `speed_ms` with it rather than reading the
  stored mph field

### Assets — track geometry, textures, materials
- `static.dat` Xbox track format decoded: **14,551 verts / 9,847 tris, 67
  models, 0 skipped** (`tools/extract_track.py`)
- **180/180 textures** decoded, DXT1/DXT5 (`tools/extract_textures.py`), with
  production names (`bk_marketbuilding1b`, `bk_freewaysigns3`, …)
- Materials wired: **71/71 submeshes textured**, 12/12 references resolve
- It's the **Bangkok** circuit (Thai signage; `Tracks/AS/` = Asia)
- `src/burnout3_trackmesh.c` loads the OBJ; the harness renders it

### Vehicle roster
107 vehicles extracted from `pveh/`, cross-validating exactly against
`vlist.bin`'s declared count (`tools/extract_vehicles.py`).

---

## 4. Dead ends — do not repeat these

### `.bgv` vehicle meshes: five failed approaches

The mesh format is **unsolved, including by the Burnout modding community** —
EdnessP's Noesis plugin loads `.bgv` textures then calls `boSetDummyMdl`, and
"All vehicle model support" is in its own TODO. This is open research, not a
lookup.

1. **Plain float3 scan** — all 576 KB of `Car1.bgv` yields exactly one 38-triple
   region. Not float vertices.
2. **Spatial coherence metric** — ranked the header's sub-offset tables as
   "mesh-like". Monotonically ascending tables are maximally coherent; any
   smoothness-only heuristic locks onto them.
3. **Coherence + oscillation** — found regions with aspect ratio 1.00 on all
   three axes. No car is a cube. The tell was recurring `16256` = `0x3F80`, the
   high half of float `1.0` read misaligned. Those regions are float
   transforms/matrices.
4. **Version-immediate search** (`0x403`/`0x405`/`0x406`) — the "dispatch table"
   at `0x001DAEF1` is C++ static-initialiser registration. Also, the `0x406`
   hit at `0x00169855` is bytes `C7 06 04 00 00 00` = `MOV dword ptr [ESI],4`;
   the immediate does not exist, it spans two operands.
5. **Brute-force emulation** (`tools/find_bgv_parser.py`) — scored functions on
   reading/following header pointers. Top hits `FUN_00190330`/`FUN_00190380`
   are 33-line flag-setting loops. **The detector was invalid by construction:**
   it put the buffer pointer in all four registers and four argument slots, so
   any function reading small offsets scored; and "followed" counted a read
   anywhere within `0x400` bytes of a target, nearly free over a `0x90000` file.

**Also retracted:** `0x1C80` in `Car1.bgv` is **texture data** (per the Noesis
plugin), not NV2A push-buffers as recorded earlier. And `+0x00` is the **version**
(valid `0x14`..`0x25`), not a magic number.

**Falsified:** vehicle meshes do **not** reuse the track layout. Applying the
track submodel structure at every header offset and sub-offset entry, with and
without the `0x40` bounds prefix, validates nothing.

### Other dead ends
- `PrgData.bin` is not a VDB (its header fields don't validate)
- Full-game emulation via xemu is impractical here: not installed, not packaged,
  and needs a BIOS/MCPX ROM. Function-level emulation via Unicorn works instead
  and needs none of that.

---

## 5. Methodology — read this, it is the real lesson

Across this work, **every single error was caught by a second independent
derivation disagreeing, never by re-reading more carefully.** Errors made and
later caught: wheel stride (`0x30`→`0xC0`), wheel array base (`+0x894`→`+0x820`),
a missed second parameter registrar, a silent struct-packing failure, a wrong
calling convention, a mislabelled velocity vector as a force vector, a missing
4th force component, a broken coherence metric, an over-optimistic feasibility
estimate, and two premature "found it" announcements on the `.bgv` parser.

Practical rules that follow:

1. **Ground truth beats reading.** The physics work is trustworthy because
   Unicorn executes the real code and says yes or no. The geometry work produced
   five confident wrong answers because it had no equivalent. Get ground truth
   before believing anything.
2. **A green test suite means "what I checked matches", not "correct".** The
   4-component force bug passed 7/7 because nothing compared the 4th component.
   Widen what you assert before trusting a pass.
3. **Verify every immediate-value hit by disassembling around it.** Small
   immediates (`0x403`, `0x4C`, `0x17`) occur constantly as unrelated data.
4. **Any heuristic loose enough to rank a big binary will rank noise.** If a
   metric produces a tidy top-10, assume it's measuring something structural and
   irrelevant until proven otherwise.
5. **API success responses lie.** `/create_struct` reported success while
   packing fields wrong (440 bytes instead of 464, last five fields 24 bytes
   low). `/set_function_prototype` reported success with a wrong calling
   convention that produced zero typed fields. Always assert on the *result*.
6. **When you announce a finding, you've already verified it.** Two of the worst
   errors here were reported as breakthroughs and retracted the next step.

---

## 6. What's left

(A–D from the original list are DONE — textures, drivetrain port, .bgv
geometry+textures, audio. What remains, by value-per-effort:)

### A. Per-car physics — DONE (2026-08-10, with a correction)
The per-car VDB is **NOT inside each .bgv** (that assumption is falsified —
full-dump scan). It is the single retail file **`Data/vdb.xml`**, and the
registrar emulation recovered the whole key pipeline: key =
`"<param><group>/../Export/ValueDB/VehiclePhysics/<VLIST-ID>.cfg"` (param
name FIRST — the reason the earlier cfgpath-first CRC guesses matched
nothing), hashed by the table-CRC at 0x001AF250 with **SAR** (arithmetic)
shift and no final inversion; car IDs are base-40 decodes of vlist.bin's
packed 8-byte IDs (`COMPCAR1`...). `tools/extract_car_vdb.py` (probe /
scan / generate) emulates `FUN_00132D10` + `FUN_00134AC0`, verifies its
Python hash mirror against the real code, and emits
`src/burnout3_car_physics.h`: 100/107 cars (67 player cars x 64 params,
33 traffic x 9). init_vehicles now gives every grid car its own
B3PhysicsConfig with its real values; the marked fallback ratios only remain
for the 7 cars with no VDB overrides. See RE_NOTES section 10.

### A2. The full vehicle pipeline — DONE (2026-08-11)
The harness no longer runs a scalar speed+drift reconstruction: vehicle_update
drives `b3_vehicle_step_full()`, the 1:1 C composition of the real per-frame
pipeline (input stage FUN_0011ECF0 incl. engine, then FUN_0011BE50's main
path: 2 substeps at dt/2 of force pass FUN_0011D460 [with the LSDM bicycle
model FUN_0011C7C0 now ported] / crash stub / suspension pre-pass
FUN_001239C0 [ground-hit path now ported] / force pass FUN_00123FD0 /
stop-check / integrator FUN_00109560), on a fixed 60 Hz tick accumulator,
in GAME coordinate space with boundary conversion (RE_NOTES 14). The
acceptance oracle is `tools/emulate_pipeline.py`: the REAL functions running
multi-frame under one persistent Unicorn session; `tools/dump_traj.c` runs
the C from identical state; the differential windows match to 1e-6..1e-3
(tolerance 1e-2/1e-1) and full 300-390-frame runs keep gear/drift state
equal on every frame. Ground contact goes through `b3_ground_probe`
(the collision agent's real kd-tree world; route-height fallback documented).

### B. Suspension/tyre solver (FUN_00123FD0) — suspension half DONE
The per-wheel spring/damper is ported and verified (validate_port.py
suspension section, 7 cases + 1 pre-pass case, 49/49 total): clamped
`F = -(comp-len)k + vel*c` along the contact normal into +0xF0/+0x100,
droop relax, bump flag, wheel spin decay/wrap, and FUN_001239C0's airborne
path. The wheel record (+0x820 stride 0xC0) field map is in RE_NOTES 10.
Still open there: the pre-pass ground-hit path (needs the track poly soup at
+0x200 — record format documented), the bottom-out impulse solver
(FUN_001066A0/FUN_00106720), and the body-scrape branch. IMPORTANT: the
lateral/longitudinal **tyre grip forces are NOT in FUN_00123FD0** — that
function is suspension + wheel visuals only; grip lives in FUN_0011D460's
unexplored branches / FUN_00123000, which is now the last big physics block.

### C. Damage panels + wheels in .bgv
`tools/extract_bgv.py` gets the whole drivable mesh via the relinker layout
(BGV_EXTRACTION.md). The per-panel sub-structures (the 10-slot table at each
LOD base, glass records, damage-state masks at record+0x18, wheel matrices at
+0xB80) are partially mapped in RE_NOTES; rendering panels/wheels separately
(for damage + spinning wheels) is open. The draw path to study is
`FUN_000303d0` → `FUN_00031e10` → `FUN_00031ab0`.

### D. Remaining polish
Real audio IS wired now: the player's own rpm-labelled engine loops
(pitch-tracked) + front-end music play from build/audio/ (see
load_real_audio / audio_callback in burnout3_full.c). Still open: crash/DJ
samples, track collision against actual mesh (currently corridor walls),
opponent AI beyond line-following, HUD/menus, other tracks (extractors are
track-agnostic — only C1_V1 is wired up).

(The old note here about matching hashes against .bgv bytes is superseded:
the registrar emulation was indeed the way, but the VDB the keys match is
Data/vdb.xml, not anything in the .bgv files — section A above.)

---

## 7. File inventory

```
tools/xbe2elf.py                XBE -> correctly mapped ELF32   [essential first step]
tools/xboxkrnl_ordinals.py      kernel ordinal -> name table
tools/apply_ghidra_types.py     applies structs + prototypes to the DB, self-verifying
tools/extract_physics_params.py Ghidra -> src/burnout3_physics_params.h
tools/extract_vehicles.py       pveh/ -> src/burnout3_vehicle_data.h (107 vehicles)
tools/extract_track.py          static.dat -> OBJ + MTL (real track geometry)
tools/extract_textures.py       static.dat -> PNGs (DXT1/DXT5, 180/180)
tools/emulate_vehicle.py        runs real x86 physics under Unicorn  [ground truth]
tools/validate_port.py          differential test: ported C vs real code (49/49)
tools/extract_car_vdb.py        registrar emulation -> per-car physics from Data/vdb.xml
tools/probe_fields.py           identifies struct fields by perturbation
tools/field_usage.py            static read/write map of struct accesses
tools/find_bgv_parser.py        INVALID SCORING -- plumbing is reusable, metric is not

src/burnout3_full.c             test harness (original code, NOT decompiled)
src/burnout3_vehicle_sim.c      physics; bottom section is ported 1:1 and verified
src/burnout3_vehicle_struct.h   live vehicle layout, per-field evidence + confidence
src/burnout3_trackmesh.c        loads the extracted track OBJ
src/burnout3_physics_params.h   generated: 64 params + defaults
src/burnout3_car_physics.h      generated: per-car real values from Data/vdb.xml (100 cars)
src/burnout3_vehicle_data.h     generated: 107-vehicle roster

docs/RE_NOTES.md                all findings, with evidence, dead ends, retractions
```

**`src/burnout3_full.c` is not decompiled code.** It is an original harness
written to have something runnable. Only the clearly-marked ported section of
`burnout3_vehicle_sim.c`, the generated headers, and the extracted assets are
real game data. The repo previously claimed to be a "full decompile" that was
"FULLY PLAYABLE"; that was false and the claim has been removed. Please keep it
that way — mark provenance on everything.

---

## 8. Credits

Track/texture formats and the VDB layout come from the Burnout Modding community:
- EdnessP's Noesis plugin `fmt_Burnout3LRD.py` — burnout.wiki, discord.gg/8zxbb4x
- `Sokka06/burnout-data-tool` — VDB/VList reader, and the reference VDB dump

The extractors here are independent Python reimplementations so the data can be
used without Noesis, but the format knowledge is theirs.
