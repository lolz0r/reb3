# Control-path audit — input to wheel forces

Scope: every stage that converts a control intent (key, pad, AI decision,
cinematic handover, rescue) into the forces the vehicle pipeline applies, as
the harness runs it today (`src/burnout3_full.c` + `src/burnout3_vehicle_sim.c`
+ `src/burnout3_ai.c`).

Markers, as elsewhere in this repo:
**[C]** execution-verified — a green differential case runs the real x86 and
asserts the port; **[C-disasm]** read directly from disassembly of
straight-line code, no execution case; **[S]** decompile-supported,
self-consistent, no green case; **[?]** open; **GLUE** original harness code
that stands in for something not recovered.

Suites backing this table: `tools/validate_port.py` 76/76,
`tools/validate_gameplay.py` 91/91, `tools/validate_ai.py` **79/79**.

All addresses are the analyzed `build/burnout3.elf` (`.text` = flat + 0x10000).
Ghidra bridge queries in this audit all carried `&program=burnout3.elf`; the
default program on that bridge is a flat-loaded `default.elf` whose addresses
are wrong.

---

## 0. Verdict

**What claimed to be real code and was not:** nothing in the harness is
mislabelled *upward*. Every `[C]` marker on the control path that this audit
spot-checked against the binary held (8 spot-checks, section 3). The problems
run the other way, in three shapes:

1. **A wrong open item.** `docs/RE_AI.md` §6 recorded the AI target writers
   (`racecar+0x23C0/+0x23C4`) as unlocated, "derived-pointer writes" somewhere
   in an unanalyzed region. That premise was false. The AI object is
   **embedded in the racecar at `racecar+0x1A00`**, so those two fields are
   `AI+0x9C0` / `AI+0x9C4`, and both are written by plain named-offset stores
   in `FUN_00171E30` and `FUN_001724F0`. The scan had been looking for the
   wrong base. Everything downstream of the aim point is now recovered and
   ported (section 2, `src/burnout3_ai.c`).

2. **A law applied in the wrong place.** `vehicle_update`'s "AI speed governor"
   (burnout3_full.c:1573) applies the `0x00171078` down-pull to cars running
   the *full physics*. That governor is the tail of `FUN_00170B30`, which this
   audit identified as the **out-of-range (simplified, off-camera) mover** — it
   is a hard `FUN_001204C0` speed write on cars that are NOT being simulated.
   Retail never runs it against the physics pipeline. It is a plausible-looking
   rubber band bolted onto the wrong code path.

3. **GLUE that is now recoverable.** Six stages marked GLUE turn out to have
   located, readable retail code: the aim/steering demand, the corner-speed
   shape, the traffic cruise speed, the off-world reset, the "beach" rescue,
   and the per-opponent pace tables. Details in the table.

---

## 1. Stage-by-stage table

### 1.1 Player control

| # | Stage | Where (harness) | Retail | Status |
|---|---|---|---|---|
| 1 | Key/pad mapping | `process_input`, `vehicle_update` 1110-1119 | Xbox pad → input manager, not located | **GLUE**. Digital W/S/A/D → {0,1}; retail has analogue triggers/stick. *Recoverable:* likely (the input manager feeds `v+0x1400/0x1404/0x1408/0x13FC`), but no function located; nothing downstream depends on it because the pipeline consumes the same four fields. |
| 2 | Steering input shaping | `b3_input_stage` (burnout3_vehicle_sim.c) | `FUN_0011ECF0` steering schedule + slew, angle `0x1164 = -(steer × angle)` | **[C]** validate_port "steering" section (5 cases: `b3_steer_schedule`, `b3_steer_slew`) |
| 3 | Throttle shaping | `b3_input_stage` + `FUN_00104D30` glue | live `0x1400 = raw 0x1414 × accel-mult 0x13BC`, capped 1.0 | **[C-disasm]**, mirrored in the pipeline; the AI half is **[C]** (validate_ai driver cases) |
| 4 | Brake shaping / drift gate | `b3_input_stage` | `FUN_0011ECF0` brake-pressure drift gate `max(0.1, 1 + (maxpress-1)/(130-mindrift) × (mph - mindrift))` | **[C]** validate_port |
| 5 | Reverse control swap | pipeline gear engage | `FUN_0011ECF0`: gear < 0 swaps `0x1400`/`0x1404` | **[C]** validate_port "engage" (6 cases) |
| 6 | Boost engage | `b3_boost_engage` | `FUN_0017A5B0` min-units + recovery-time gate | **[C]** validate_gameplay (5 engage cases) |
| 7 | Boost input bit | `v->bar.boosting` → `b3_vehicle_step_full` | driver writes `v+0x13FC = 4` while the record burns | **[C]** validate_ai "boost bit set" / validate_gameplay |
| 8 | Drift entry | `b3_input_stage` auto-drift block | `FUN_0011ECF0` state-0 entry: `|steer|>0.9 && throttle>30/speed` accumulates `0x1438` vs `(1.5-throttle) - (speed-43)×0.04 + delay`, needs speed > 43 | **[C]** validate_port (trajectory: gear+drift state equal every frame) |

### 1.2 The vehicle pipeline (input → wheel forces)

| # | Stage | Retail | Status |
|---|---|---|---|
| 9 | Frame tick | `DAT_0060EA1C`, hard 60 Hz | **[C-disasm]**; harness accumulator is GLUE around it |
| 10 | Driver dispatch | vtable **+0x24 → `FUN_00104D30`** (see spot-check 5) | **[C]** structure; glue mirrored |
| 11 | Input stage | `FUN_0011ECF0` incl. engine `FUN_00121560` | **[C]** validate_port (19 engine/transmission cases) |
| 12 | Substep split | `racecar+0x1920 == 0` → dt/2 ×2 | **[C]** |
| 13 | Force pass | `FUN_0011D460` (+ LSDM `FUN_0011C7C0`) | **[C]** validate_port (10 tyre/airborne + 1 LSDM) |
| 14 | Suspension pre-pass | `FUN_001239C0` | **[C]** (7 suspension + 1 pre-pass) |
| 15 | Suspension force pass | `FUN_00123FD0` | **[C]** racing path; bottom-out impulse solver `FUN_001066A0/720` **[S]** unported |
| 16 | Integrator | `FUN_00109560` | **[C]** (8 integrator cases) |
| 17 | Ground query | `FUN_0011BC60` soup | **[S]** interface-stubbed by `b3_ground_probe` (real kd-tree world) |
| 18 | Wall push-out | `FUN_0011AEF0` | crash agent's; the harness's `mesh_collide` write-back is **GLUE** |

### 1.3 AI driver (this session's work)

| # | Stage | Retail | Status |
|---|---|---|---|
| 19 | Aim point selection | `FUN_00175B10` nav-graph walk over the .bgd road network (node link tables + the 16-byte point pool at `DAT_0073A174`), producing `AI+0x200`; `FUN_0016AAC0` arbitrates it against avoidance | **[?] THE WALL.** Located and readable, but it walks a graph the harness does not load. Caller supplies the point. |
| 20 | Aim direction + time-to-target | `FUN_0016AE20`: `AI+0x770 = normalize(AI+0x180 - racecar+0x40)`, `AI+0x784 = len/speed` (or `len` when speed ≤ 1) | **[C]** validate_ai "commit" (4 cases) → `b3_ai_commit_target` |
| 21 | Frame snapshot | `FUN_00171A10`: `AI+0x7B0 = normalize(AI+0x770)`, `AI+0x9C8 = AI+0x784` | **[C]** → `b3_ai_frame_snapshot` |
| 22 | **Target steering angle** (`racecar+0x23C0`) | `FUN_00171E30` racing path: angle between `normalize(carAt×(1-k) + racecarFwd×k)` and the aim direction, signed by `dot(racecarRight, aim)`, then an **asymmetric per-frame slew limiter** (2.4°/frame winding away from centre, 8.1°/frame unwinding) | **[C]** validate_ai (8 cases + 60-frame trajectory, worst Δ 4.8e-08) → `b3_ai_target_angle` |
| 23 | Target angle, LSDM/drift path | same function: yaw-RATE demand `acos/t2t` vs `v+0xD4`, `(rate-cur)×speed²×gain`, gain 0.04 unwinding / 0.01 winding, clamp ±90° | **[C]** validate_ai (4 cases) |
| 24 | Corner-speed law | `FUN_00172E80`: `min( MinSpeedMps + max(0, 1 - \|ang\|/AngleYouWantMinSpdAt) × S , S , AI+0xA08 )`, ×1.45 when `AI+0x213`; drifting bypasses the angle scale | **[C]** validate_ai (9 cases) → `b3_ai_corner_speed` |
| 25 | **Target speed** (`racecar+0x23C4`) | `FUN_001724F0`: corner law → aggression match (`FUN_00172FA0`) → catch-up (`FUN_001734C0`) → `Min speed mps` floor (or 0 when `racecar+0x2450==1`) → steering-error cut `×(1 - clamp(\|AI+0x9D4\|×0.125, 0, 1))` when `\|err\| > 1` | **[C]** validate_ai (9 cases) → `b3_ai_target_speed` |
| 26 | Speed cap | `FUN_00173690` → `AI+0xA08` = `Top speed mps` while racing | **[C-disasm]** |
| 27 | Steering conversion | `clamp(racecar+0x23C0 × MaxLock × -1/180, -1, 1)`; MaxLock switches to `Drift Max lock` in the attack states | **[C]** validate_ai + validate_gameplay |
| 28 | Throttle/brake band | deficit > 1.0 m/s → throttle 1; excess > 13.4112 m/s (30 mph) → full brake; **else: throttle 1 if engine rpm ≥ change-up rpm, otherwise coast** | **[C]** validate_ai (the rpm override is NEW — Ghidra reported it as an uninitialised-stack read; it is `byte [esp+7]`, set at 0x00105380) |
| 29 | Brake helper | `FUN_00104CA0`: gear −1 routes the brake amount to the THROTTLE input | **[C]** validate_ai (3 cases) |
| 30 | Stuck-reverse | below 5 mph with the crash timer clear → 1.0 s arm → 2.0 s reverse burst, gear −1, shift kick 0.35 s; steer ±1 by `clamp(dot(racecar+0x18E0, carRight), ±1)`; target below 8.9408 m/s also sets the stop flag | **[C]** validate_ai (8 cases) |
| 31 | Launch throttle dither | 2×-frame-time cycle on `v+0x1574`/`v+0x156C` below 13.4112 m/s | **[C]** validate_ai (2 cases) — was **[S]** |
| 32 | OOC envelope | authority `v+0x1534` = 0.1 (mode 2) / 0.05; mode 0 throws full opposite lock unless `FUN_00198190`; mode 1 holds the previous input | **[C]** validate_ai (3 cases) + validate_gameplay (3 cases) |
| 33 | Out-of-range governor | `0x00171078` (tail of `FUN_00170B30`): down-step by `Out of range speed decrease rate`, up-step by `mph × 0.0010309278`, snap inside the band, through `FUN_001204C0` | **[C]** validate_ai (5 cases). **Belongs to the simplified mover, not the physics path** — see verdict item 2. |
| 34 | Traffic driver | `FUN_00105150`: same MaxLock/180 conversion; throttle above 1 m/s deficit; brake above **2.2352 m/s = exactly 5 mph** excess (`0x003B2330` → `DAT_005A3A20`); target speed capped at **`DAT_005A9770` = 22.352 m/s = exactly 50 mph** (`0x003B2110` → `0x005A9770`, static-init snippet at `0x002C5E80`) | **[C-disasm]**; the harness's `B3_TRAFFIC_SPEED_MS = 13.0` (29 mph) is **GLUE and wrong** |
| 35 | Per-opponent pace | `FUN_00172870` reads a **0x98-byte per-grid-slot record** at `DAT_0073A170` (bytes ×0.01 at +0x90..+0x97 → `AI+0x9E0..0x9F4`, mode byte at +0x95 → `AI+0x9F8`) and two 16-entry lerp weight tables at +0x10/+0x50, producing per-section min/max speed factors into `AI+0x7C0` / `AI+0x8C0` | **[S]** located; data binding to the .bgd not traced. Harness's `×(0.88 + 0.04×(id%4))` is **GLUE** |
| 36 | Aggression / slam | `racecar+0x2188/+0x2413..0x2415` state machine (`FUN_00172FA0` + `FUN_00169540` family) | **[S]** params fully known (RE_AI §1), writer not decompiled; the driver-side *consumption* of the flags is **[C]** |
| 37 | Avoidance | `FUN_0016C450` / `FUN_0016F6C0` family, "Soft/Hard No Go" offsets, risk model `FUN_0016AAC0` | **[S]** params **[C]**, logic located but not ported |

### 1.4 Recovery / handover paths

| # | Stage | Harness | Retail | Status |
|---|---|---|---|---|
| 38 | Crash-state control | `crashed_until > 0` → throttle 0, brake 1, boost 0 (1391-1408) | crashed path is byte `v+0x210` → `FUN_00118410` input shaper (534 lines) | **GLUE**; the real shaper is located, unported (crash agent's domain) |
| 39 | Takedown-cinematic AI handover | `td_ai_wheel` → the harness line-follower (1095-1108) | `byte racecar+0x27D8` tested at **`0x0018C53A`**, then `mov ecx,esi; call FUN_00170820` — the racecar's own AI update | flag semantics **[C]** (spot-check 6); the driver behind it is now ported for stages 20-32, so the handover can use `b3_ai_update` |
| 40 | Off-world reset | 15 m below the corridor → immediate route reset (1639-1652) | **`FUN_001712E0`**: interpolate the road height at the current node from the four edge points, compare against `racecar+0x44 - v+0x870`; more than **5 m below for 61 consecutive frames** (`0x3D`) → `FUN_001714F0` | **GLUE**; real trigger + reset now located **[C-disasm]** |
| 41 | "Beach" rescue | 18 s without 25 m of progress → route reset (1350-1373) | **`FUN_00170820` @0x001708EE**: `racecar+0x1904 > 200` frames → `FUN_001714F0(last_valid_node − 8, 0, 0)` then `FUN_001204C0(v, MinSpeedMps × 0.44704)` = respawn 8 nodes back at **8.9408 m/s (20 mph)**, and clear the counter | **GLUE**; real rule now located **[C-disasm]**. What increments `racecar+0x1904` is **[?]** |
| 42 | The reset itself | teleport to the nearest route point | `FUN_001714F0`: set node `+0x18D0` (wrapped by `DAT_0073A188`), re-walk the section links, **zero the physics accumulators `v+0xF0..+0x13C`** (force/torque/impulse/push-out) and the direction vectors `racecar+0x19C0..0x19DC`, then `FUN_00179760` + `FUN_001709F0` (place the car ~3 nodes back, camera-occlusion tested via `FUN_0018E2F0`) + `FUN_0010A960` | **[S]** fully read, unported |

---

## 2. The AI object — the finding that closed §6

`FUN_001705F0` (the navigator constructor) writes a set of aggregate
back-pointers into the racecar:

```
*(rc + 0x1A00) = rc + 0x1A00      *(rc + 0x1A04) = rc      *(rc + 0x1A08) = rc + 0x1A00
*(rc + 0x2150) = rc + 0x1A00      *(rc + 0x2154) = rc      *(rc + 0x2158) = rc + 0x1A00
*(rc + 0x2160) = rc               *(rc + 0x2164) = rc + 0x1A00
*(rc + 0x21A0) = rc
```

Those are exactly the `this+0x004`, `this+0x750..0x764`, `this+0x7A0` slots the
AI code dereferences to reach the racecar. Therefore the AI object base is
`racecar + 0x1A00`, and:

| racecar offset | AI offset | meaning |
|---|---|---|
| +0x1B80 | +0x180 | arbitrated target POINT (world) |
| +0x1BD0 | +0x1D0 | target-follower speed ceiling |
| +0x1BF8 | +0x1F8 | target mode (0/1/2/4) |
| +0x1C00 | +0x200 | tracked aim position (nav-graph output) |
| +0x2120 | +0x720 | target-follower candidate direction |
| +0x2170 | +0x770 | **arbitrated desired direction** |
| +0x2180 | +0x780 | arbitrated speed ceiling |
| +0x2184 | +0x784 | time-to-target (s) |
| +0x21B0 | +0x7B0 | normalized snapshot of +0x770 |
| **+0x23C0** | **+0x9C0** | **target steering angle (degrees)** |
| **+0x23C4** | **+0x9C4** | **target speed (m/s)** |
| +0x23C8 | +0x9C8 | time-to-target snapshot |
| +0x23CC | +0x9CC | slew-limiter memory |
| +0x23D4 | +0x9D4 | yaw-rate error |
| +0x23F8 | +0x9F8 | OOC / difficulty mode |
| +0x2408 | +0xA08 | hard speed cap |
| +0x2419 | +0xA19 | wants-boost flag |

Per-frame order (from `FUN_00170820` → `FUN_00171A10`):
`FUN_00173690` (cap) → `FUN_0016AAC0` (arbitrate: `FUN_00175B10` target
follower + `FUN_0016C450` avoidance, committed by `FUN_0016ADF0`/`FUN_0016AE20`)
→ snapshot/normalize → **`FUN_00171E30`** → `FUN_00171BE0` (drift decision) →
**`FUN_001724F0`** → `FUN_00171D90`; then the physics side runs
`FUN_00104D30` → `FUN_00105340`.

---

## 3. Spot-checks performed against the binary

Every one of these was read out of `build/burnout3.elf` in this session, not
taken from the notes. All Ghidra queries carried `&program=burnout3.elf`.

1. **Steering conversion** (`docs/RE_AI.md` §3, harness 1246-1251). At
   `0x001055BB`: `movss xmm0,[ecx+0x23C0]` → `mulss` by `[0x0047A160]`
   (drift) or `[0x0047A15C]` (normal) → `mulss` by `[0x003B1A64]`.
   `0x003B1A64 = -0.0055555557` = −1/180. **Confirmed**, and it confirms the
   *input* is the target angle, not a heading error.
2. **30 mph brake threshold.** `0x003B1A5C = 13.411199569702148`; copy snippets
   at `0x002B8D20` / `0x002B8D60` install it into `DAT_005A39EC` /
   `DAT_005A3A10`; both are read at `0x001058CD` / `0x0010583A`. **Confirmed.**
3. **Stuck-reverse constants.** `0x003B1694 = 5.0` (mph gate),
   `0x0038994C = 2.236936` (the TRUE mps→mph here, not the game's 2.2374146),
   `0x003B168C = 1.0` (arm), `0x003B1688 = 2.0` (reverse),
   `0x0039B2B0 = 0.35` (shift kick), `0x003A69C4 = 0.1` (stop). **Confirmed.**
4. **OOC authority.** `0x001056C6` writes `0x3D4CCCCD` (0.05) and the mode-2
   leg writes `0x3DCCCCCD` (0.1) into `v+0x1534`. **Confirmed.**
5. **Driver dispatch vtable.** Vtable at `0x003B1240`; slot **+0x24** holds
   `0x00104D30` (the table's last entry — `+0x28` is the string "Ghost car").
   `FUN_0011BE50` executes `call dword ptr [eax+0x24]` at `0x0011BF4C`.
   **Confirmed twice, independently.**
6. **Takedown handover.** `0x0018C53A`: `mov al, byte ptr [esi+0x27D8]` /
   `test al,al` / `mov ecx,esi` / `call 0x170820`. **Confirmed.**
7. **Traffic driver band.** `FUN_00105150` at `0x00105159` uses the same
   `+0x23C0 × [0x47A15C] × −1/180`; `0x0010520F` brakes when
   `−DAT_005A3A20 > deficit`, and `0x003B2330 = 2.2352` = exactly 5 mph.
   **Confirmed.**
8. **Traffic speed cap (new).** `0x002C5E80`: `movss xmm0,[0x003B2110]` /
   `movss [0x005A9770], xmm0`; `0x003B2110 = 22.352` = exactly 50 mph, read at
   `0x00172585` in `FUN_001724F0`'s traffic branch. **Confirmed.**

Two further reads that corrected prior notes:

* `RE_AI.md` §4 says "the unanalyzed mover code ending at 0x1710FA (Ghidra has
  no function there)". Ghidra indeed has no function there — but there *is*
  one: `FUN_00170B30`, in the gap `0x00170B29..0x001711B0`, a virtual method in
  the racecar vtable at `0x003B1228`. It rate-limits `racecar+0x19C0` toward
  `racecar+0x21B0` by `AI/Car/Max desDir angle change OOR`, moves the car
  kinematically along the road-network node line, then runs the governor. It is
  the **out-of-range** mover — §4's "which AI mode runs this mover is not
  identified" is answered.
* The `[esp+0xb]` byte the decompiler reports as an uninitialised stack read in
  `FUN_00105340` is a real local: `0x00105380` sets `byte [esp+7]` from
  `v+0x149C × 9.549296 >= v+0x1470` (engine rpm at/above the change-up point).
  It gates a full-throttle override inside the coast band.

---

## 4. What the harness should change (integration)

Exact hunks in `scratchpad/ai/integration_ai.md`. In priority order:

1. Replace the line-follower's steering with `b3_ai_target_angle` +
   `b3_ai_drive` (adds the carAt blend and the 2.4/8.1 slew limiter — the two
   things that make retail AI look like it has hands on a wheel).
2. Replace the linear `top + (min-top)×k` corner speed with
   `b3_ai_target_speed` (real shape + the steering-error cut + the min floor).
3. Delete the governor block at burnout3_full.c:1573 for physics-driven cars
   (verdict item 2), or gate it on an explicit out-of-range mode.
4. Raise traffic cruise from 13.0 to 22.352 m/s and use `b3_ai_traffic_drive`.
5. Route the takedown-cinematic handover (`racecar+0x27D8`) through
   `b3_ai_update` instead of the line-follower.
6. Off-world reset / rescue: adopt the retail triggers (5 m below for 61
   frames; 200 stuck frames → 8 nodes back at 20 mph).
