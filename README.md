# reB3
If Claude and Ghirda had a baby. The finest slop in all the lands. 

A reverse engineering of *Burnout 3: Takedown* (Criterion Games / EA, 2004,
Xbox) — a faithful recreation driven by data and logic recovered from the
retail executable.

The physics, collision, scoring and AI are not a re-imagining. They are the
game's own equations, recovered from `default.xbe`, each one cited to the
address it came from and checked against the real x86 running under emulation.

---

## You need your own copy of the game

**This repository contains no game content.** No executable, no tracks, no car
meshes, no textures, no audio, and no data tables extracted from any of them.
It is code and documentation only.

To get a program that runs, you supply a dump of a disc you own and the tools
here rebuild everything locally. Nothing has a path baked in; every tool
resolves `B3_GAME_ROOT` through `tools/b3_paths.py`.

---

## Setup, start to finish

### 1. What you need

| | |
|---|---|
| **The game** | A dump of a disc you own. Developed against the NTSC-U (USA) release. |
| **Python** | 3.8+, plus `pip install pillow capstone unicorn` — Pillow for texture PNGs, capstone and unicorn for the RE tools and the differential suites. |
| **A C toolchain** | On Debian/Ubuntu: `sudo apt install build-essential libsdl2-dev libsdl2-image-dev libgl1-mesa-dev` |
| **Disk** | ~8 GB free. Fully extracted, `build/` is about 7 GB, most of it audio. |
| **Ghidra** | Only for one extractor — see step 3. Everything else needs just Python. |

The game directory is whatever folder holds `default.xbe`:

```
Burnout 3 Takedown/
├── default.xbe
├── Data/            Globalus.bin, vdb.xml
├── GLOBAL/
├── pveh/            player + traffic vehicles (.bgv/.btv, .hwd/.lwd engine audio)
├── sound/           .awd audio dictionaries
└── Tracks/          per-track static.dat, streamed.dat, .bgd, .xwb, .rws
```

```bash
export B3_GAME_ROOT="/path/to/Burnout 3 Takedown"
```

Every tool reads that variable and fails with an explicit message if it is
unset. It also matches path case insensitively, so `Tracks/` and `tracks/`
both work — dumps differ.

### 2. The corrected ELF

```bash
make elf        # -> build/burnout3.elf
```

Rebuilds `default.xbe` as a correctly mapped ELF32 (one `PT_LOAD` per section
at its true VA, BSS materialised, `e_entry = 0x001D2807`). Every RE tool and
every test suite reads this, not the XBE.

It matters because loading the XBE as a flat binary is **silently wrong** —
each section has a different `VA − file_offset` delta, so absolute data
references land on the wrong bytes. `0x003A2D50` is `2.5` but reads as `0.0`;
one float constant reads as the string `"Score/Burnout Points"`. See
[docs/ASSETS.md](docs/ASSETS.md) for the full before/after.

`build/burnout3.elf` is a derived copy of the retail executable. It is
gitignored. Do not redistribute it.

### 3. The data tables — `make assets`

```bash
make assets
```

Writes seven headers into `src/` that get compiled in: the physics parameter
model, the 107-vehicle roster, 4,685 per-car overrides from `vdb.xml`, glyph
metrics, the track's nav lines, the traffic tables, and the start grid. They
are game data in C form, so they are gitignored, not shipped.

> **This one step needs Ghidra.** `extract_physics_params.py` walks the ValueDB
> registrar `FUN_00132D10` through a bridge on `http://127.0.0.1:8089`, so
> Ghidra must be open with `build/burnout3.elf` loaded and the bridge running.
> Import it with the **ELF loader and no explicit language ID** — passing one
> forces the Binary loader and reproduces the flat-binary bug above. The other
> six extractors need nothing but Python. This is the one rough edge in an
> otherwise self-contained pipeline; porting it to a capstone sweep over the
> ELF (pattern in `tools/field_usage_19be.py`) would remove it.

`make` refuses to build without these and names what is missing — it will never
quietly produce a program with invented numbers in it.

### 4. The world — `make content`

```bash
make content
```

Fourteen extractors: track geometry and its textures, the collision world, the
sky, light probes, props, car meshes and their paint, the traffic light tables,
HUD art, and the effects rasters. `B3_TRACK` picks the circuit (default
`US_C3_V1`, matching the runtime default).

**`make assets` alone is not enough to play.** It gets you a program that
compiles; without `make content` you build cleanly, launch, and drive through
an empty void with no track and no skybox.

### 5. Audio — `make audio`

```bash
make audio
```

~5 GB and by far the slowest step, which is why it is separate. The game runs
without it — you get silence, not a crash.

### 6. Build and run

```bash
make            # -> ./burnout3
./burnout3
```

`make everything` runs steps 3–5 in order if you would rather do it in one go.

A correct launch prints all of these:

```
[Burnout3] REAL track geometry: 97826 verts, 90246 tris from build/track.obj
[Burnout3] GAME collision world: 60373 triangles (build/collision.bin)
[Burnout3] REAL audio: 4 engine loops, EA TRAX 44/44 tracks
[carfx]    env map build/tracks/US_C3_V1/envmap.png
[Burnout3] REAL textures: 122 loaded, 0 groups unresolved (of 990)
props:     7 models, 123 instances
[Burnout3 HUD] 99 textures from build/frontend (edge 41/41, core 30/30, over 20/20)
```

### If something is wrong

| symptom | cause |
|---|---|
| `make` stops, "This tree has no retail data in it" | step 3 not run |
| No track, no sky, `Generated road mesh` and no `REAL track geometry` line | step 4 not run |
| Silence; `no playable tracks`, `missing wave` | step 5 not run |
| `B3_GAME_ROOT is not set` | step 1 |
| `make assets` hangs or fails on connection refused | the Ghidra bridge, step 3 |
| `Error 1 (ignored)` during `make content` / `make audio` | expected — see below |

Two extractors exit non-zero after succeeding, and the Makefile deliberately
ignores those two exit codes while still letting make print them:
`extract_collision.py` writes a correct 60373-triangle world and then fails its
own route-containment self-check (unexplained, pre-existing, tracked in
[TODO.md](TODO.md)), and the audio extractors report 6 of 1569 waves failing to
decode, which is the shipped data — a correct run still ends at 174
dictionaries / 1569 waves and `EA TRAX: 44 tracks`.

The reference detail behind all of this — why the ELF mapping matters, what
each generated header holds and where it came from, format credits, and what
must never be committed — is in **[docs/ASSETS.md](docs/ASSETS.md)**.

---

## State of it

You race a six-car grid of real liveried Burnout 3 cars — closed bodies,
wheels spinning and steering at their real attach points, translucent glass,
wreck-shell damage states — from the game's real start grid around a real
textured circuit, against real oncoming traffic on the shipped traffic lines.

What is running underneath is the game's own per-frame vehicle pipeline
(`b3_vehicle_step_full`: `FUN_0011ECF0` + `FUN_0011BE50`'s stage order over a
vehicle struct with four independent wheels), its scoring, boost, takedown,
out-of-control and damage rules, its crash decision surface, the recovered AI
driver law with its 73 parameters, the XBE's own fonts and HUD art, real engine
loops and streamed music, and mesh-based collision.

It is a harness, not a shipped game: there is no career, no frontend menu flow,
and the presentation is deliberately not pixel-matched. Handling, collision,
triggers and control flow are.

---

## How claims are made here

This is the part worth copying if you do your own RE project.

Every recovered fact carries provenance:

* **`[C]`** — confirmed from the image, **with the address**.
* **`[S]`** — strong inference, with the evidence stated.
* **`[?]`** — open.
* **GLUE** — an invention of this harness, not the game. Marked as such at the
  site, so it can never be mistaken for a recovered fact.

"Unrecoverable" requires proof, not a shrug.

The test suites are **differential**: they execute the retail function out of
`build/burnout3.elf` under Unicorn and compare it against this port, case by
case. When `tools/validate_carcol.py` says the car-vs-car crash gate matches,
it means the real `FUN_001121F0` was run and agreed.

```bash
python3 tools/validate_port.py         # the physics pipeline vs real x86
python3 tools/validate_carcol.py       # car-vs-car collision
python3 tools/validate_gameplay.py     # scoring / boost / takedown rules
make test-soup-ray test-traffic-pool test-traffic-reservations
```

Two constants that reading alone would have missed, and that emulation caught:
the game's gravity is **10.0**, not 9.81, and its m/s→mph factor is
**2.2374146**, not 2.2369363.

---

## Layout

```
src/burnout3_full.c              the harness: window, render loop, game state
src/burnout3_vehicle_sim.c       the recovered per-frame vehicle pipeline
src/burnout3_carcol.c            car-vs-car collision and the wreck path
src/burnout3_crash.c             crash state, cinematics, recovery
src/burnout3_td_rules.c          takedown / near-miss / score event rules
src/burnout3_ai.c                the AI driver law
src/burnout3_collision.c         mesh collision against the real track
src/burnout3_trackmesh.c         loads the extracted track geometry
src/burnout3_hud.c               HUD, using the XBE's own fonts

tools/b3_paths.py                resolves B3_GAME_ROOT -- start here
tools/xbe2elf.py                 XBE -> correctly mapped ELF32 (do this first)
tools/extract_*.py               26 extractors: geometry, textures, audio, tables
tools/emulate_*.py               run real retail functions under Unicorn
tools/validate_*.py              differential suites: this port vs the real code

docs/ASSETS.md                   how to extract from your own copy
docs/RE_NOTES.md                 the master findings document
docs/HANDOFF.md                  orientation, and which approaches are dead ends
```

The Android port lives in `android/` and builds the same sources through
Gradle + NDK with GL4ES; divergence is behind `#ifdef __ANDROID__`.

### Documentation

| | |
|---|---|
| [ASSETS.md](docs/ASSETS.md) | extracting from your own copy — **start here** |
| [HANDOFF.md](docs/HANDOFF.md) | orientation and war stories |
| [RE_NOTES.md](docs/RE_NOTES.md) | the master findings document |
| [RE_CRASH_PARITY.md](docs/RE_CRASH_PARITY.md) | the complete crash decision surface |
| [BGV_EXTRACTION.md](docs/BGV_EXTRACTION.md) | how the `.bgv` car mesh format was cracked |
| [PHYSICS_GLUE_LEDGER.md](docs/PHYSICS_GLUE_LEDGER.md) | every GLUE invention, and what would replace it |
| [TODO.md](TODO.md) | what is still open, each with its blocker |

---

## Legal

The code and documentation in this repository are original work, released
under the MIT licence — see [LICENSE](LICENSE).

*Burnout 3: Takedown* is the property of its rights holders (Criterion Games /
Electronic Arts). This project is not affiliated with or endorsed by them. No
copyrighted game content is distributed here; the extraction tools operate on
a copy you provide and must legally own.

Third-party components and their licences are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
