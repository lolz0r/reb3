# PROPS — the destructible track props (cones, barrier boards, bulb posts)

> User request, with `REFERENCE IMAGES/xemu-2026-08-12-13-53-46.png` (an orange
> cone mid-tumble beside the car at the dirt-shortcut barrier): *the
> destructible track props should be there as in retail, and react the same way
> when you hit them.*

Two agents worked this wave. **PROPS-1** (host process killed mid-run, transcript
lost) found the prop tables in `static.dat`, transcribed the world-object
registration loop, wrote the first `tools/extract_props.py`, the whole of
`src/burnout3_props.c/.h` (loader, knock integrator, renderer, the car sweep)
and a worktree testbed in `burnout3_full.c`. **PROPS-2** (this pass) re-derived
every claim from `build/burnout3.elf` with the Ghidra bridge down (a capstone
sweep, `scratchpad/props/b3dis.py`), corrected the cited addresses, settled the
class-vs-model-index question from the data, **found and fixed the index-base
bug that was drawing every model past the first as a heap of shards**, wired the
recovered object-crash verdict, rebased onto `be1e68a`, and did the
verification. Attribution is marked per section.

Addresses are in the corrected map (`build/burnout3.elf`; `.text` = old flat
address + `0x10000`). `[C]` = read out of the image, `[S]` = strongly supported,
`[?]` = open, **GLUE** = invented and marked as such.

---

## 1. Where the props live in the data [C] — PROPS-1, re-verified PROPS-2

Every shipped `static.dat` carries a **second** 0x70-record model table, next to
the backdrop-LOD table `tools/extract_track.py` documents at +0x34/+0x38:

| header | meaning |
|---|---|
| `+0x36` u16 | prop MODEL count |
| `+0x3C` i32 | prop MODEL table (0x70 per record) |
| `+0x40` u16 | prop INSTANCE count |
| `+0x44` i32 | `u8[model_count]` — the per-model **prop class** |
| `+0x48` i32 | instance TRANSFORMS, 0x40 bytes each (one 4x4) |
| `+0x4C` i32 | `ptr[unit]` → `u8[model]` per-streamed-unit instance counts |
| `+0x50` i32 | `ptr[unit]` → `ptr[model]` per-(unit,model) instance lists |
| `+0x54` u16 | streamed-unit count (shared with the world loader) |

Neither the 0x40 transform stride nor the length of the +0x44 array is assumed:
`(hdr+0x4C − hdr+0x48) / count` is exactly 64.000 on all 37 files, and
`hdr+0x44 + model_count` rounded up to 16 lands exactly on `hdr+0x48` on all 37.

The reader is the world-object registration loop
`FUN_00110420 @0x001109CB..0x00110A46`, transcribed instruction by instruction:

```
001109cb  MOV  ECX,[0x737688]        ; the +0x4C counts table
001109d5  MOV  AL,[0x737686]         ; model count, the loop bound
001109e8  MOV  EDX,[ECX + unit*4]    ; counts[unit]
001109eb  MOV  AL,[EDI + EDX]        ; counts[unit][model]         EDI = model
001109f4  MOV  EAX,[0x73768c]        ; the +0x50 lists table
001109fd  MOV  EDX,[EAX + unit*4]    ; lists[unit]
00110a00  MOV  EAX,[EDX + EDI*4]     ; lists[unit][model]
00110a03  LEA  ECX,[EAX + ESI*4]     ; 4 BYTES PER LIST ENTRY
00110a0c  LEA  EDX,[EAX+EAX*2]; SHL EDX,4   ; world slot stride 0x30
00110a19  MOV  byte [slot],0x5       ; world-slot TYPE 5 = STATIC PROP
00110a1c  MOVZX EDX,word [ECX]       ; entry+0x00 u16 = instance index
00110a1f  SHL  EDX,0x6               ;   * 0x40
00110a22  ADD  EDX,[0x737678]        ;   + the +0x48 transform table
00110a28  MOV  [slot+0x04],EDX       ; slot -> the LIVE transform
00110a2b  MOVZX ECX,byte [ECX+0x2]   ; entry+0x02 u8
00110a35  IMUL ECX,ECX,0x70          ; (see below)
00110a38  ADD  ECX,[hdr + 0x3C]
00110a3f  MOV  [slot+0x08],ECX
00110a46  CALL FUN_00114270          ; world AABB from model bbox x transform
```

**The list-entry byte at +0x02 is a CLASS, not a model index** [C, PROPS-2].
PROPS-1 asserted it from the byte being 0..7 on every track; that argument is
weak on its own (a 32-model track could still use only 8). The decisive test is
`AS_C2_V1`: 3 models, `+0x44 = [1, 5, 2]`, and the per-unit lists carry exactly
those three values. Dumping the +0x3C table past the third record shows records
3, 4, 5 are **garbage** (`mat=50629`, a bbox extent of 3e16), so byte 5 cannot
be a model index — it would index off the end of a shipped file. The model an
instance draws is the list it lives in (`EDI`), which is what the extractor
emits. Retail nevertheless scales the class by 0x70 into the model table at
`0x00110A35`; on tracks whose classes exceed the model count that slot+0x08
pointer is out of range. Recorded, not reproduced. **[?]**

Prop classes by content, stable across all 37 files:

| class | what it is | instances (all tracks) |
|---|---|---|
| 1 | **the cone family** — every track's cone, plus life rings | 4838 |
| 2 | benches, bins, litter | 1445 |
| 3 | roadwork lights, low bins | 978 |
| 4 | roadwork barrier boards, road-closed signs | 2141 |
| 5 | market/temporary barriers | 1073 |
| 6 | tall signposts, warning markers | 2056 |
| 7 | heavy blocks, tables, trunks | 824 |
| 0 | 4 stray instances on EU_M1 | 4 |

## 2. The model record and its meshes [C] — PROPS-1

0x70 bytes, relocated by `FUN_0019B4E0`: bbox MAX at +0x00, bbox MIN at +0x10,
three 0x14-byte mesh blocks at +0x20/+0x34/+0x48 (LOD1/LOD2 present per the
`+0x62` flags), material indices at +0x5C..+0x60, LOD near/far floats at
+0x64/+0x68. The vertex stride comes from the material's shader class, the two
declarations `extract_track.py` already pins as the foliage/prop/cone families:
class 8 = `pos + uv` (20 B), class 9 = `pos + NORMPACKED3 + uv` (24 B). Over all
436 prop models in all 37 tracks `(index_ptr − vertex_ptr)` is an exact multiple
of the class-derived stride **and** the vertex count exceeds the largest index,
which neither stride achieves alone. Indices are one u16 triangle strip stitched
with duplicated indices; the extractor de-strips to a triangle list.

**The bug that made this wave's first capture useless** [PROPS-2]: `build()`
wrote the concatenated index blob with **global** vertex indices while the loader
added the model's `first_vertex` on top, so every model after the first drew
another model's vertices. A `GL_DangBarr` came out as a pile of shards and a
cone as a flat yellow sheet. `props.bin` now stores **model-local** indices (the
format note says so) and the loader is unchanged. `scratchpad/props/props_zoom.png`
is the before, `props_zoom2.png` the after — striped trestle barriers and orange
cones.

## 3. Prop mass and radius [C] — PROPS-1, addresses corrected PROPS-2

`FUN_0011A020` is the prop rigid-body constructor. With `EBX` = the model record:

```
0011a0a5  body+0x1D0 = bbox MAX          0011a0af  body+0x1E0 = bbox MIN
0011a0de  SQRTSS -> body+0x1CC           radius = |bbox max|
0011a137  XMM0 = [0x3A2928] = 100.0
0011a13f  XMM1 = (max.x − min.x)          (body+0x1D0 − body+0x1E0)
0011a155  XMM0 = (max.z − min.z)          (body+0x1D8 − body+0x1E8)
0011a169  * [0x3A292C] = 200.0
0011a17d  MAXSS with 100.0
0011a191  -> body+0x1F0                   THE MASS
0011a19e  body+0x224 = DAT_0060EA20 + [0x3A7F34] (= 10.0)   the despawn clock
0011a062  body+0x204 = THE INSTANCE MATRIX ITSELF
0011a032..0011a05d save / 0011a06d..0011a094 restore  m[3] m[7] m[11] m[15]
```

So a prop's mass is its **footprint area × 200, floored at 100**, and the four
`w` slots of the instance matrix are not transform at all — they are the baked
half-range instance colour (~0.41, 0.40, 0.37 on the reference cluster), which
is why they survive the body reset. Both are emitted per model / per instance.
US_C3_V1: a `WF_Cone` floors at 100 kg, a `GL_Cone` is 156, a `GL_DangBarr`
(3.96 × 0.90 footprint) is 710.

## 4. What retail does when you hit one [C] — PROPS-1, addresses corrected PROPS-2

`FUN_00111CD0`, the collision-pair dispatcher, has four arms:

| arm | test | goes to |
|---|---|---|
| bail `@0x00111D0B` | either handle type == **8** | nothing |
| 1 `@0x00111D14..0x00111D8B` | one handle type **3**, other in {0,1,2,4,6,7} | `FUN_00112E70` (A in {0,1,2,4}) else `FUN_001135E0` |
| 2 `@0x00111D90` | both in {0,1,2} | car-vs-car |
| 3 `@0x00111DEB..0x00111E1B` | `FUN_0010FB20(other)` **and** other handle type **5** | **`FUN_00113890`** — the prop |
| 4 `@0x00111E22..0x00111E4E` | `FUN_0010FB20` both | `FUN_00113960`, the generic solver |

`FUN_00113890` then:

```
00113901  CALL FUN_001084E0   the "may I knock it" gate -- purely GEOMETRIC
                              (@0x001084EF reads both bbox rows and halves
                              their sum; no mass or size veto exists)
0011392e  CALL FUN_00197A20   prop-hit score/audio, and only when the other
                              handle's type is 0/1/2, i.e. a car
0011393b  CALL FUN_00114730   PROMOTE
0011394e  CALL FUN_00113960   resolve with the generic rigid solver
                              (guarded by `type < 8` @0x00113947)
```

and `FUN_00114730` hands the prop one of exactly **16** rigid bodies: allocation
bitmask at `gameworld+0xE9CA0/+0xE9CA4` (`@0x00114737`, `@0x00114750`), 16-slot
scan `@0x0011476D`, pool base `gameworld+0xC4380` `@0x001147E4`, stride `0x780`
`@0x001147BD`, and the handle's type byte becomes **6** `@0x0011478B`. When the
pool is full the body with the smallest `+0x224` clock is recycled and **the
recycled prop's world slot is retyped to 8** `@0x0011480C` — which the
dispatcher drops outright. A prop that has lost its body stops colliding; that
is exactly the module's `B3P_SETTLED` state [S].

## 5. Why a cone can never wreck you, and the object-crash interface [C] — PROPS-2

This is the section the crash wave asked for.

*Retail's answer.* `FUN_00112E70` is reachable **only** through dispatcher arm 1,
which requires a handle of **type 3**. A static prop is type 5 and a knocked one
is type 6, so a car-vs-prop pair takes arm 3 (knock) or, once promoted, arm 4
(generic solver; `FUN_0010FB20` accepts 6) — **it never reaches the crash
trigger**. The type→class map says the same from the other side: `FUN_0010FBC0`'s
jump table `@0x0010FC04` sends types 5, 6 and 7 to the class-6 arm `@0x0010FBFC`,
and `DAT_0039AE50` row 6, read out of the image, is all zeros:

```
row 0 [1 1 1 1 1 1 0]   B is a racecar
row 2 [1 0 0 1 0 0 0]   B is a type-3 OBJECT  -> crashes class 0 and 3
row 6 [0 0 0 0 0 0 0]   B is a prop (type 5/6/7)
```

*What the harness does.* `b3_props_collide_car()` fills `B3PropHit` with the
recovered `obj_class` **and** the kinematics `FUN_00112E70` wants — `vrel` is the
**prop's** point velocity minus the **car's** (`@0x00113311`), the same quantity
retail takes the normal component of `@0x00113331`. The `game_update` hunk hands
every contact to `b3_td_object_contact()` with

```
obj_class = b3_props_object_class(inst)   /* 6 for every shipped prop  [C] */
car_class = b3_td_object_class(0, 0)      /* racecar -> 0              [C] */
car_mass  = pv->fsim.mass                 /* the CAR's mass, per RE_TD_RULES */
immune    = g_race_time < pv->immune_until
vrel, n   in GAME space (z negated, RE_NOTES 12)
```

so the **retail crashability table returns the verdict**, not a harness rule:
`b3_td_object_contact` refuses at its `b3_td_object_crashable()` line and never
touches the per-car accumulator, so props cannot pre-empt a real wall or object
crash either. Over a full autodrive lap with 88 logged prop contacts (up to
158 mph closing) the object trigger fired **zero** times.

*The escape hatch, GLUE, default OFF.* `B3_PROP_CRASH_KG=<kg>` makes props at or
above that recovered mass present class **2** (a type-3 prop ENTITY), the row
that does crash a racecar. Retail ships no such promotion for `static.dat`
props, and the failure mode is exactly the one `be1e68a` had just fixed for the
soup mapping (a 75 mph *normal-component* bar with no head-on gate wrecks you out
of nowhere), so it stays off until something measures it. When a real type-3
entity family is recovered, it plugs in here: give it `obj_class = 2` and the
rest of this path is already the retail one.

## 6. `props.bin` [PROPS-1, index semantics corrected PROPS-2]

`tools/extract_props.py` (`--all`, or one track id; `B3_TRACK` honoured) writes
`build/tracks/<ID>/props.bin`. Header `'B3PP'`, version 1, then model records
(0x60: bbox, first_vertex/count, first_index/count, class, mass, radius, LOD
near/far, material flags, 32-char texture basename), instance records (0x50: the
4x4, model, class, unit, flags), vertices (0x20: pos, normal, uv) and u16
indices. Positions are RAW GAME SPACE — the loader does the Z reflection, as
`extract_track.py` and `burnout3_track_paths.h` do. **Index values are
model-local** (add `first_vertex`). Nothing per-track is compiled in anywhere.

```
37 tracks, 13359 prop instances, 436 models, no structural NOTEs
biggest EU_M2_V1 741   smallest US_C5_V1 0 (that track has no prop data at all)
US_C3_V1 (the reference track) 7 models, 123 instances: 53 WF_Cone, 18 GL_Cone,
   4 Chgo_LifeRing (75 class-1), 24 Chgo_RdWorkLight, 18 GL_DangBarr,
   5 GL_RdClosed, 1 Chgo_LitterBin
```

## 7. The knock law — GLUE, marked [PROPS-1, capped by PROPS-2]

`FUN_00113960`, the generic solver, is not ported (it is the car-vs-car impulse
chain), so the reaction is a ballistic tumble. Everything recovered that feeds it
is `[C]`: the 16-body pool, the 10 s body life, the mass and radius laws, the
"body taken away → stops colliding" rule. Everything else is GLUE and is named
in one block at the top of `burnout3_props.c`: restitution 0.35, ground bounce
0.30 / friction 0.70, spin gain 2.2 capped at 22 rad/s, air drag 0.25/s, lift
0.45, rest threshold 0.35 m/s, and **`B3P_KNOCK_MAX_MS = 30`** — the launch cap
PROPS-2 added: the impulse is mass-cancelling ((1+e)·vn, which is the limit of
the retail solver as m_car ≫ m_prop), so a 150 mph clip flung a cone at 90 m/s
and it left the frame in three frames. The user's reference frame is a cone
*mid-tumble beside the car*, so the launch speed is capped. Nothing about
*whether* a prop is knocked depends on it.

## 8. The harness patch [PROPS-2, from PROPS-1's testbed]

`scratchpad/props/apply_props_patch.py` — exact-match, idempotent, refuses to
write anything if any anchor is missing or ambiguous, and re-runs as a no-op.
Six hunks, all additive, anchored against `be1e68a`:

| hunk | anchor | what |
|---|---|---|
| include | `#include "burnout3_ai.h"` | `burnout3_props.h` |
| draw | `trackmesh_draw_scroll` / `trackmesh_fog_end` | `b3_props_draw()` inside the world's fog bracket |
| restart | `b3_tdfx_event_reset(); … g_state = RACING;` | `b3_props_reset()` |
| update | `traffic_update(g_delta_time);` | integrate + collide every racer + the object-trigger call |
| init | `traffic_init(); … b3_hud_init(…)` | `b3_props_load("build/tracks/<B3_TRACK>")` |
| Makefile | `src/burnout3_panels.c` | `src/burnout3_props.c` |

`B3_PROP_TRACE=1` logs every contact (`car, instance, model, prop class, object
class, mass, closing mph, impulse`).

## 9. Verification — headless only (`SDL_VIDEODRIVER=offscreen`, one instance)

Captures in `scratchpad/props/`:

| file | what it shows |
|---|---|
| `props_off.png` | the reference barrier with props disabled — nothing there |
| `props_before.png` | the same view with props: barrier boards, ROAD CLOSED signs, cones |
| `props_zoom2.png` | close-up: striped trestle barriers + orange cones (`props_zoom.png` is the same shot before the index fix) |
| `top_before.png` | top-down, the barrier line across the dirt shortcut, untouched |
| `top_1205.png`, `lap_top_1150.png` | mid-pass: the pack through the line, props tumbling |
| `lap_top_1260.png` | after: the middle of the line and the right-hand cluster swept away |
| `props_after.png`, `side_1206.png` | the player at 152 mph through the barrier line, HUD reading NEAR MISS / DRIFT — no crash |

Runtime, autodrive, deterministic `B3_FIXED_DT`, one instance at a time:

| run | props | wall clock |
|---|---|---|
| 6000 frames, US_C3_V1 (123 instances) | on | 30.04 s |
| 6000 frames, no props | off | 29.14 s (**+3.1 %**) |
| 3000 frames, EU_M2_V1 props (741 instances, the biggest table) | on | 15.72 s |
| 3000 frames, no props | off | 15.22 s (**+3.3 %**) |

A **full lap completes** (`Lap 1/3 completed!`, the run then ends on the race
clock — "Time's up", not a stall): **88 prop contacts** logged, 35 of them
class-1 cones and 52 class-4 barrier boards, closing speeds up to **118.3 mph**,
and **0 object crashes** — no prop wrecked any car, which is the retail law of
section 5.

Suites, all green on this tree:

```
tools/validate_port.py        95/95
tools/validate_carcol.py     752/752
tools/validate_td_rules.py   532/532
```

## 10. Open items

* **[?]** `0x00110A35`'s `IMUL class,0x70` into the model table — retail's
  slot+0x08 model pointer for a type-5 prop is derived from the CLASS, which on
  several tracks points past the end of the +0x3C table. The harness uses the
  list's model, which is unambiguous. Whether retail has a latent bug here or
  the pointer is only ever used for something that tolerates it (the
  `FUN_00114270` AABB) is unresolved.
* **[?]** Prop-hit **audio/score** (`FUN_00197A20 @0x0011392E`) is not ported —
  retail plays a per-class prop-hit sample and scores it. That is an SFX-wave
  item; the contact reports already carry everything it needs.
* **[?]** LOD1/LOD2 meshes and the `+0x64/+0x68` fade distances are extracted
  but unused; every prop draws LOD0 at any range.
* **[?]** The generic solver `FUN_00113960` is still unported, so the knock is
  GLUE (section 7) and the car takes no reaction force from a prop at all.
