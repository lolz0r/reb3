# Getting the data out of your own copy

This repository contains **no game content**. Not the executable, not the
tracks, cars, textures, audio or music, and not the data tables extracted from
them. What it contains is the code that reads those files and the recovered
logic that runs on them.

To build a running program you supply your own copy of *Burnout 3: Takedown*
for the original Xbox, and the tools here rebuild everything locally.

---

## 1. What you need

| | |
|---|---|
| **The game** | A dump of a disc you own. The extractors were developed against the NTSC-U (USA) release. |
| **Python** | 3.8+. `pip install pillow capstone unicorn` — Pillow for texture PNGs, capstone/unicorn for the RE tools and test suites. |
| **A C toolchain** | `gcc`, `make`, SDL2 and SDL2_image dev packages, and OpenGL. On Debian/Ubuntu: `sudo apt install build-essential libsdl2-dev libsdl2-image-dev libgl1-mesa-dev` |
| **Disk** | About 8 GB free. Fully extracted, `build/` is ~7 GB, most of it audio. |

The game directory is whatever folder holds `default.xbe`:

```
Burnout 3 Takedown/
├── default.xbe
├── GLOBAL/
├── pveh/            player + traffic vehicles (.bgv / .btv)
├── Tracks/          per-event track data (static.dat, streamed.dat, .bgd)
└── ...
```

Point the tools at it once:

```bash
export B3_GAME_ROOT="/path/to/Burnout 3 Takedown"
```

Every tool reads that variable through `tools/b3_paths.py` and fails with an
explicit message if it is unset. Nothing here has a path baked in.

---

## 2. The corrected ELF

Everything else depends on this step, and it is the one place people usually
go wrong.

```bash
python3 tools/xbe2elf.py "$B3_GAME_ROOT/default.xbe" build/burnout3.elf
```

**Do not load `default.xbe` into a disassembler as a flat binary.** Each XBE
section has a different `VA − file_offset` delta, so every absolute data
reference lands on the wrong bytes — and it fails *silently*. Measured on this
image: `0x003A2D50` is `2.5` but reads as `0.0`; `0x00384A80` is `0.15` but
reads as `22.0`; one float constant reads as the string `"Score/Burnout
Points"`.

`xbe2elf.py` rebuilds the real address space as an ELF32 — one `PT_LOAD` per
section at its true VA, BSS materialised, `e_entry = 0x001D2807`.

|  | Flat binary | Corrected ELF |
|---|---|---|
| Functions found | 7,267 | 7,434 |
| Entry point | `0x0` (bogus) | `0x1D2807` (real) |
| Junk functions in header/`.data` | 7 | 0 |
| Float constants | wrong | correct |
| Data/string xrefs | meaningless | resolve |

If you import it into Ghidra, use the **ELF loader and do not pass an explicit
language ID** — doing so forces the Binary loader and reproduces the bug.

`build/burnout3.elf` is a derived copy of the retail executable. It is
gitignored. Do not redistribute it.

---

## 3. The data tables (`make assets`)

Seven headers are compiled into the program. They are game data in C form, so
they are gitignored and generated locally:

| header | extractor | what it holds |
|---|---|---|
| `burnout3_physics_params.h` | `extract_physics_params.py` | the 73-parameter physics model, offsets into the `0x1D0` config struct |
| `burnout3_vehicle_data.h` | `extract_vehicles.py` | the 107-vehicle roster, cross-checked against `vlist.bin` |
| `burnout3_car_physics.h` | `extract_car_vdb.py generate` | 4,685 per-car overrides out of `Data/vdb.xml` |
| `burnout3_font.h` | `extract_font.py` | glyph metrics for the three fonts in the XBE `.data` |
| `burnout3_track_paths.h` | `extract_bgd_paths.py` | the track's nav/racing lines from its `.bgd` |
| `burnout3_traffic_data.h` | `extract_traffic.py` | traffic population, class, model and paint tables |
| `burnout3_start_grid.h` | `extract_start_grid.py` | the real race start grid |

```bash
make assets      # runs all seven, in dependency order
make             # then builds ./burnout3
```

`make` refuses to build without them and tells you exactly what is missing —
it will never silently produce a program with invented numbers in it.

### The one manual step

`tools/extract_physics_params.py` walks the ValueDB registrar `FUN_00132D10`
and reads the compiled-in defaults out of `FUN_00132950`. It does that through
a **Ghidra bridge on `http://127.0.0.1:8089`** rather than from the ELF
directly, so that single extractor needs Ghidra open with `build/burnout3.elf`
loaded and the bridge running. The other six read the game files directly and
need nothing but Python.

This is the one rough edge in an otherwise self-contained pipeline. Porting it
to a capstone sweep over the ELF — the pattern is in
`tools/field_usage_19be.py` — would remove the dependency.

---

## 4. The runtime content

`make assets` gets you a program that compiles. To actually drive, extract the
geometry, textures and audio. These are independent of each other; run what you
need.

```bash
# track geometry, textures, collision, props, lighting
python3 tools/extract_track.py           # -> build/track.obj + build/tracks/
python3 tools/extract_textures.py        # -> build/textures/
python3 tools/extract_collision.py       # -> build/collision.bin
python3 tools/extract_props.py           # -> build/tracks/<ID>/props.bin
python3 tools/extract_envmap.py          # -> build/tracks/<ID>/
python3 tools/extract_light_probes.py    # -> build/tracks/<ID>/

# vehicles: meshes, their textures, and the traffic light/paint tables
python3 tools/extract_bgv.py build/cars  # -> build/cars/<CLASS>_<CarN>.obj
python3 tools/extract_bgv_textures.py    # -> build/cars/
python3 tools/extract_traffic_lights.py  # -> build/cars/

# effects art
python3 tools/extract_carfx_art.py
python3 tools/extract_boostfx_art.py
python3 tools/extract_particlefx_art.py
python3 tools/extract_postfx_art.py

# audio: engine/effect banks, then the streamed music
python3 tools/extract_awd.py             # -> build/audio/awd_*/
python3 tools/extract_xwb.py             # -> build/audio/
python3 tools/extract_eatrax.py          # -> build/music/   (slow; -j 8 helps)
```

Audio and music are the bulk of the 7 GB and the slowest steps. The game runs
without them — you get silence, not a crash.

Track-scoped extractors default to one event and take `--event <ID>`; several
accept `--all`. Nothing is hardcoded per track: the IDs come from the shipped
track list, so `B3_TRACK` selects at runtime.

---

## 5. Checking it worked

```bash
make test-soup-ray test-traffic-pool test-traffic-reservations
python3 tools/validate_port.py           # the physics differential vs retail
python3 tools/validate_carcol.py         # car-vs-car collision vs retail
```

`validate_carcol.py` compares against the real collision hulls, which live in
the `.bgv`/`.btv` files rather than in any extracted asset. Pull them out once:

```bash
python3 -c "import sys; sys.path.insert(0,'tools'); \
            import emulate_carcol as ec; ec.extract_hulls()"   # -> build/cars/*.hull
```

The `validate_*` suites are differential tests: they execute the *retail* code
out of `build/burnout3.elf` under Unicorn and compare it against this port,
function by function. They need the ELF from step 2. If a suite fails after you
change something, the port diverged from the game — that is the whole point of
them.

---

## 6. What must never be committed

`.gitignore` already covers all of it, but for the avoidance of doubt:

* `default.xbe`, any disc image, and `build/burnout3.elf`
* everything under `build/` — geometry, textures, audio, music, collision
* the seven generated headers in `src/`
* screenshots or video of the retail game

Cite a retail frame or an address in the documentation freely. Do not check the
bytes in.
