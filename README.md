# Burnout 3: Takedown — reverse engineering project

A faithful recreation of *Burnout 3: Takedown* (Criterion Games / EA, 2004,
Xbox), driven by data and logic recovered from the retail executable.

The physics, collision, scoring and AI are not a re-imagining. They are the
game's own equations, recovered from `default.xbe`, each one cited to the
address it came from and checked against the real x86 running under emulation.

---

## You need your own copy of the game

**This repository contains no game content.** No executable, no tracks, no car
meshes, no textures, no audio, and no data tables extracted from any of them.
It is code and documentation only.

To get a program that runs, you supply a dump of a disc you own and the tools
here rebuild everything locally:

```bash
export B3_GAME_ROOT="/path/to/Burnout 3 Takedown"   # the folder with default.xbe
make assets                                          # extract the data tables
make                                                 # -> ./burnout3
```

`make` refuses to build without the extracted data and tells you exactly what
is missing — it will never quietly produce a program with invented numbers in
it. The full walkthrough, including the track/car/audio content, is
**[docs/ASSETS.md](docs/ASSETS.md)**.

Nothing has a path baked in; every tool resolves `B3_GAME_ROOT` through
`tools/b3_paths.py`.

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
