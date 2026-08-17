# Burnout 3 opponent AI — drivers, parameters, rubber-banding

Recovered 2026-08-10 from the analyzed `burnout3.elf` (VAs as elsewhere;
`.text` = flat + 0x10000). Markers as in the other docs: **[C]** =
execution-verified (green differential case in `tools/validate_gameplay.py`,
AI driver section — **6/6**) or reproduced by two independent derivations;
**[C-disasm]** = read directly from disassembly of straight-line code, no
execution case; **[S]** = decompile-supported, self-consistent, no green
case; **[?]** = open.

Ground-truth method: the AI parameter registrar was **executed under Unicorn**
(the `extract_car_vdb.py` scaffolding: hooks on the registration entry
FUN_001AEE20 and the hash core 0x001AF250), and the driver formulas were
verified by seeding FUN_00105340's environment and asserting its input writes
— same pattern as every other verified rule in this repo.

---

## 1. The AI parameter registrar — FUN_0016AFD0 [C]

`FUN_0016AFD0` is the AI config constructor + registrar. It is called from
the boot state machine `FUN_00015F10` with **EAX = 0x0047A140** (the static
AI config instance; `mov eax, 0x47a140; call FUN_0016afd0` at 0x16533,
immediately before the Score registrar FUN_00190430 and the Boost Bar
registrar FUN_0017A0F0). It writes the compiled defaults into the struct and
registers **73 parameters** through the same machinery as the physics/score
registrars, cfg path **`../Export/ValueDB/AI/defaults.cfg`** (string
0x003B0240; the cfg hash is present in `Data/vdb.xml`'s fileDef section).

Key pipeline verified: all 73 captured key strings
(`"<param><group>/../Export/ValueDB/AI/defaults.cfg"`) hash-mirror 73/73
against the table-CRC at 0x001AF250, and every key resolves in the retail
`Data/vdb.xml`.

Groups: `AI/Car`, `AI/Arbitrator`, `AI/Avoidance`, `AI/Target`, `AI/Driver`,
`AI/Aggressive Driving`, `AI/Aggressive Driving/Slam`,
`AI/Aggressive Driving/Reactions` (strings at 0x003B0264..0x003B030C).

### Complete parameter table (offset from 0x0047A140; default → VDB tune)

| off | VA | group | param | default | vdb |
|---|---|---|---|---|---|
| +000 | 47A140 | AI/Car | Out of range speed decrease rate | 60 | **10** |
| +004 | 47A144 | AI/Car | Max desDir angle change OOR | 1 | 1 |
| +008 | 47A148 | AI/Car | Angle you want min spd at | 38.4 | **90** |
| +00C | 47A14C | AI/Car | Top speed mps | 80 | **88** |
| +010 | 47A150 | AI/Car | Min speed mps | 10 | **20** |
| +014 | 47A154 | AI/Car | How much carAt affects steering | 0.2 | **0.9** |
| +018 | 47A158 | AI/Car | Angle at which drift is started | 15 | **20** |
| +01C | 47A15C | AI/Car | Max lock at 180 x degrees | 6 | **10** |
| +020 | 47A160 | AI/Car | Drift Max lock at 180 x degrees | 4 | **50** |
| +024 | 47A164 | AI/Arbitrator | Risk threshold | 4 | 1 |
| +028 | 47A168 | AI/Arbitrator | Total risk threshold | 4 | 2 |
| +02C | 47A16C | AI/Arbitrator | Current risk threshold | 4 | 5 |
| +030 | 47A170 | AI/Arbitrator | Corner Risk threshold | 4 | 0.5 |
| +034 | 47A174 | AI/Arbitrator | Total corner risk threshold | 4 | 1 |
| +038 | 47A178 | AI/Arbitrator | Current corner risk threshold | 4 | 4 |
| +03C | 47A17C | AI/Avoidance | AVOID: LookAhead dist racecars | 10 | 20 |
| +040 | 47A180 | AI/Avoidance | AVOID: Soft No Go offset time | 1.5 | 2.5 |
| +044 | 47A184 | AI/Avoidance | AVOID: Soft No Go offset distance | 75 | 101 |
| +048 | 47A188 | AI/Avoidance | Hard No Go offset time | 0 | 0 |
| +04C | 47A18C | AI/Avoidance | Hard No Go offset distance | 200 | 200 |
| +050 | 47A190 | AI/Avoidance | Distance to discard fatally colliding racecar | 40 | 100 |
| +054 | 47A194 | AI/Avoidance | Vert dist to discard traffic and racecars | 5 | 5 |
| +058 | 47A198 | AI/Avoidance | dt between start and end vehicle | 0.5 | 0.2 |
| +05C | 47A19C | AI/Avoidance | Speed when car is <10m away | 10 | **26.2** |
| +060 | 47A1A0 | AI/Avoidance | Speed when car is <20m away | 25 | **40** |
| +064 | 47A1A4 | AI/Avoidance | Speed when car is <30m away | 40 | **60** |
| +068 | 47A1A8 | AI/Avoidance | Speed when risk is >0.95 | 10 | 16 |
| +06C | 47A1AC | AI/Avoidance | Speed when risk is >0.9 | 30 | 30 |
| +070 | 47A1B0 | AI/Avoidance | Steering factor big=>extreme | 4 | 5.1 |
| +074 | 47A1B4 | AI/Avoidance | Extra softNoGo offset dist for future | 20 | 5 |
| +078 | 47A1B8 | AI/Avoidance | Extra softNoGo offset time for future | 0.5 | 0.08 |
| +07C | 47A1BC | AI/Avoidance | Aggression time variation factor | 1 | 3.08 |
| +080 | 47A1C0 | AI/Avoidance | Aggression dist variation factor | 10 | 10 |
| +084 | 47A1C4 | AI/Avoidance | Aggression time variation offset | 2 | 0 |
| +088 | 47A1C8 | AI/Avoidance | Aggression dist variation offset | 10 | 10 |
| +08C | 47A1CC | AI/Target | Dist to brake speed factor big=>fast | 0.44 | 0.6 |
| +090 | 47A1D0 | AI/Target | Dist to update target pos | 0.4 | 0.4 |
| +094 | 47A1D4 | AI/Target | Perp dist to update target pos | 0.1 | 0.1 |
| +098 | 47A1D8 | AI/Target | Drift speed threshold | 30 | 41 |
| +09C | 47A1DC | AI/Target | Corner radius to give max offset pos | 300 | 300 |
| +0A0 | 47A1E0 | AI/Target | (five corner-extension params — see note) | 0 | 0/1/5/nan |
| +0B4 | 47A1F4 | AI/Target | How far away from the true apex to move the apex point of a corner when drifting (meters) | 0 | 1 |
| +0B8 | 47A1F8 | AI/Driver | How close the target car must be to start proper speed matching (meters) | 10 | 10 |
| +0BC | 47A1FC | AI/Driver | Maximum distance from player to start doing sticky speed matching (metres) | 10 | 10 |
| +0C0 | 47A200 | AI/Driver | Maximum diffence in speeds for sticky speed matching (MPH) | 40 | 40 |
| +0C4 | 47A204 | AI/Aggressive Driving | Min. aggression before we start attacking | 0 | 0.002 |
| +0C8 | 47A208 | AI/Aggressive Driving | Min. time to wait between attacks (seconds) | 0.5 | 0 |
| +0CC | 47A20C | AI/Aggressive Driving | Max. time to wait between attacks (seconds) | 2 | 3 |
| +0D0 | 47A210 | AI/Aggressive Driving | Max. distance apart to begin attacking when ahead of target (meters) | 25 | **40** |
| +0D4 | 47A214 | AI/Aggressive Driving | Max. distance apart to begin attacking when behind of target (meters) | 50 | **150** |
| +0D8 | 47A218 | AI/Aggressive Driving | Min. target speed to consider attacking (mph) | 60 | **75** |
| +0DC | 47A21C | AI/Aggressive Driving | How much slower than the player to drive while speed matching to attack position | 0.95 | **0.9** |
| +0E0 | 47A220 | AI/Aggressive Driving | How far in front to boost when speed matching the player | 30 | **15** |
| +0E4 | 47A224 | AI/Aggressive Driving | Extra distance in front to boost relative to aggression (meters) | -22.5 | -22.5 |
| +0E8 | 47A228 | AI/Aggressive Driving | How long to wait at the start of the race before doing aggressive driving (seconds) | 7 | **3** |
| +0EC | 47A22C | AI/Aggressive Driving | How long after hitting something to disable immunity (seconds) | 3 | 3 |
| +0F0 | 47A230 | AI/Aggressive Driving/Slam | Min time to try and block you for (seconds) | 3 | 3 |
| +0F4 | 47A234 | AI/Aggressive Driving/Slam | Max time to try and block you for (seconds) | 15 | 15 |
| +0F8 | 47A238 | AI/Aggressive Driving/Slam | Max distance ahead to start blocking you (meters) | 15 | 15 |
| +0FC | 47A23C | AI/Aggressive Driving/Slam | Preferred car separation when getting into slam position (meters) | 4 | **3** |
| +100 | 47A240 | AI/Aggressive Driving/Slam | Max. time to try and get into slamming position (seconds) | 30 | 30 |
| +104 | 47A244 | AI/Aggressive Driving/Slam | Max. distance between cars, when ahead (meters) | 5 | **3.5** |
| +108 | 47A248 | AI/Aggressive Driving/Slam | Max. difference in speeds (MPH) | 10 | **50** |
| +10C | 47A24C | AI/Aggressive Driving/Slam | Steer out distance (meters) | 1 | **5** |
| +110 | 47A250 | AI/Aggressive Driving/Slam | Steer out time (seconds) | 0.25 | **0.5** |
| +114 | 47A254 | AI/Aggressive Driving/Slam | Slam time (seconds) | 3 | **0.75** |
| +118 | 47A258 | AI/Aggressive Driving/Slam | Max. cos angle off lane to stop attack | 0.8 | 0.8 |
| +11C | 47A25C | AI/Aggressive Driving/Slam | How soon after starting to slam is the AI car committed and can't stop | 0.1 | 0.075 |
| +120 | 47A260 | AI/Aggressive Driving/Reactions | How long a rubbed AI car goes blind for (seconds) | 1 | 1 |

Note on **+0A0**: the registrar registers FIVE `AI/Target` corner-extension
params ("Max/Number of segments to extend..." drift and non-drift variants,
"Time until we hit the apex...") all bound to the SAME storage offset +0xA0 —
last binding wins in the live struct; their VDB entries differ (two are
int-typed: raw 0 and 5; one is float nan). Recorded as-is; likely a shipped
registrar bug or leftovers [S].

## 2. Driver dispatch — FUN_00104D30 [C structure]

Per car, per frame (ECX = physics vehicle):
* gate: racecar(+0x13F4)+0x19A8 must be set (AI enabled), with a crash-mode
  override via FUN_00017310;
* route: `racecar+0x134C == 0` → **FUN_00105150** (the reduced driver —
  traffic-class cars); `racecar+0x179C == 1` → **FUN_00105340** (the AI racer
  driver); else → **FUN_00104E20** (per-grid-slot attract/fallback driver
  with its own LCG dither — not needed for racing [S]);
* post: below 0.1 m/s the steer input v+0x1408 is zeroed; for AI cars
  the live throttle is `v+0x1400 = v+0x1414 × v+0x13BC` clamped to 1
  (v+0x13BC = per-car throttle multiplier);
* then FUN_0011ECF0 (the verified steer-away/steering-response stage).

## 3. The AI racer driver — FUN_00105340

Inputs it consumes (written upstream by the targeting stage, writer **not
located** — see §6): `racecar+0x23C0` = target steering angle (degrees),
`racecar+0x23C4` = target speed (m/s), `racecar+0x2419` = wants-boost flag,
plus its own state timers on the vehicle (+0x156C..+0x157C).

All of the following is **[C]** (validate_gameplay AI driver section, 6/6):

* **Steering**: `v+0x1408 = clamp(-(racecar+0x23C0) × MaxLock / 180, -1, 1)`
  where MaxLock = `AI/Car/Max lock at 180 x degrees` (DAT_0047A15C; the
  -1/180 is the compiled constant -0.0055555557). In the drift/attack states
  (+0x2188 with +0x2413/+0x2414) the factor switches to DAT_0047A160 =
  "Drift Max lock at 180 x degrees" [C-disasm for the switch].
* **Throttle/brake** vs the target speed: deficit > 1.0 m/s → throttle 1;
  excess > **13.4112 m/s = exactly 30 mph** (DAT_005A39EC / DAT_005A3A10,
  both initialized from .data 0x3B1A5C by copy snippets at 0x2B8D1C/0x2B8D5C)
  → full brake via FUN_00104CA0(1.0); otherwise coast (all inputs zero).
* **FUN_00104CA0** (brake helper): with v+0x14C8 = -1 (reverse gear) the
  "brake" amount goes to the THROTTLE input (+0x1400) — reversing; forward
  it goes to +0x1404 and throttle is zeroed.
* **Boost**: while `racecar+0x11EE` (the boost record's boosting flag,
  record+0x52) the driver writes the boost input bits `v+0x13FC = 4` — the
  transmission anchor — and zeroes the brake; the engage request happens when
  `racecar+0x2419` is set and not already boosting, through the verified gate
  FUN_0017A5B0. While boosting un-ramped (+0x11F1 clear) it sets +0x11EF —
  the committed minimum-burn flag (record+0x53).

Also recovered from the same function:

* **Stuck-reverse** [C values]: below **5 mph** (the mph conversion here is
  the TRUE 2.2369363) with the crash timer clear, a 1.0 s arm timer
  (v+0x1578) counts down; on expiry the driver reverses for **2.0 s**
  (v+0x157C = 2.0, gear -1, full steer by error sign); target speed below
  8.9408 m/s (= 20 mph) additionally sets the stop flag v+0x1552.
* **Throttle dither** [S]: at full throttle below 13.4112 m/s (30 mph) the
  driver pulses the throttle on a 2×-frame-time cycle (v+0x1574/+0x156C) —
  launch traction control.
* **Out-of-control authority** [C, pre-existing cases]: inside the slam +
  aggressor windows the steering authority v+0x1534 is crushed to 0.1
  (mode +0x23F8 == 2) / 0.05.

## 4. Rubber-banding — the speed governor [C-disasm]

The unanalyzed mover code ending at 0x1710FA (Ghidra has no function there;
straight-line disassembly) advances the car and then commands the live speed
directly through FUN_001204C0 (which recomputes wheel spins from the new
v+0xBC — i.e. a hard speed write, not a force):

```
d = v.speed - racecar.target_speed(+0x23C4)
up_tol = v.speed_mph(+0x13D4) × DAT_003B1E04 (0.0010309278)
if d >  DAT_0047A140:  set_speed(v.speed - DAT_0047A140)   # too fast: pull down
elif -up_tol > d:      set_speed(v.speed + up_tol)          # too slow: pull up
else:                  set_speed(target)                    # in band: snap
```

DAT_0047A140 is `AI/Car/Out of range speed decrease rate` (VDB 10) — the
same registered parameter serves as both the band edge and the down-step.
The catch-up step grows with speed (0.103% of the mph figure per mover
step). **Which AI mode runs this mover (vs. the full physics) is not
identified** — its per-step vs per-second scaling is therefore [S]. The
down-pull is integrated in the harness scaled by dt; the catch-up branch is
documented only.

The classic per-opponent pacing knobs are elsewhere: the .bgd event param
records carry per-opponent factor tables (0.78–0.99, RE_BGD §3 [?]), and
`AI/Avoidance/Aggression {dist,time} variation {factor,offset}` randomize
per-car aggression [C params, consumer not traced].

## 5. Aggression (ramming/blocking) — parameters [C], logic [S]

The attack state machine lives behind the `racecar+0x2188/+0x2413/+0x2414/
+0x2415` flags read by FUN_00105340 (steer-out overrides write full lock ±1
and gear selects 1/2 during the slam approach). Its tuning is fully known
(§1): an AI starts attacking when its aggression exceeds 0.002, target above
75 mph, within 40 m ahead / 150 m behind; it speed-matches at 0.9× the
player's speed, prefers 3 m separation, slams for 0.75 s with a 5 m steer-out
over 0.5 s, is committed 0.075 s in, and blocks from up to 15 m ahead for
3–15 s. It boosts when 15 m in front while speed-matching. The state-machine
code itself (writer of the +0x24xx flags) was not decompiled [S/?].

## 6. Open [?]

* The writer of `racecar+0x23C0/+0x23C4/+0x2419` — the targeting stage. The
  only .text references to those displacements are the three drivers plus
  the unanalyzed mover; the writer addresses the block through a derived
  pointer. Next step: break on writes under a fuller-system emulation, or
  walk the unanalyzed region around 0x16F000-0x171100 (it consumes the
  loaded track image via DAT_0073A174 and the arbitrator tables).
* The mover's activation mode (§4) and its exact set_speed hand-off
  (xmm0 → FUN_000FFC80's vector write).
* The `AI/Arbitrator` risk model and `AI/Target` corner
  apex-offset/drift-extension consumers.
* FUN_00104E20's per-slot tables at DAT_00478934.. (attract driver).

## 7. Harness integration (src/burnout3_full.c AI block)

Integrated with the [C] markers above: the steer conversion (MaxLock/180
clamp), the throttle/coast/brake thresholds (1.0 m/s / 30 mph), stuck-reverse
(5 mph, 1.0 s arm, 2.0 s reverse), the boost input semantics through the
verified engage gate, the OOC authority 0.1 (pre-existing), and the
governor's down-pull (dt-scaled [S]). Real parameter values drive the
corner target speed (Top/Min speed mps + "Angle you want min spd at",
interpolation shape GLUE [S]) and the avoidance speed caps (26.2/40/60 m/s
under 10/20/30 m). Marked GLUE: the look-ahead point selection (stands in
for the unlocated targeting stage), the linear angle→speed shape, the
proportional brake blend above corner targets, per-AI pace variation, and
the boost trigger condition. The harness-side navigation fixes that keep
the AI moving (barrier-aware aim rays, position-based wall-grind escape,
kinematic reverse, route lane fixup) are documented in RE_NOTES section
13.1 -- all GLUE, none of it recovered logic.

---

## 8. THE AI OBJECT — §6's open item, closed (2026-08-11)

§6 recorded the writers of `racecar+0x23C0/+0x23C4/+0x2419` as unlocated and
guessed at "derived-pointer writes" in an unanalyzed region. **That premise was
wrong.** There are no hidden writes; the search was against the wrong base.

`FUN_001705F0` (navigator ctor) plants aggregate back-pointers:

```
*(rc+0x1A00) = rc+0x1A00   *(rc+0x1A04) = rc         *(rc+0x1A08) = rc+0x1A00
*(rc+0x2150) = rc+0x1A00   *(rc+0x2154) = rc         *(rc+0x2158) = rc+0x1A00
*(rc+0x2160) = rc          *(rc+0x2164) = rc+0x1A00  *(rc+0x21A0) = rc
```

Those are precisely the `this+0x004` / `this+0x754` / `this+0x760` /
`this+0x7A0` slots the AI code dereferences to reach its racecar. So **the AI
object is `racecar + 0x1A00`**, roughly 0xA40 bytes, ending at `racecar+0x2440`
(which holds the physics-vehicle pointer). Consequently

* `racecar+0x23C0` = `AI+0x9C0` — written by **`FUN_00171E30`**
* `racecar+0x23C4` = `AI+0x9C4` — written by **`FUN_001724F0`**
* `racecar+0x23F8` = `AI+0x9F8` — written by **`FUN_00172870`**
* `racecar+0x2419` = `AI+0xA19` — written by **`FUN_00172FA0`** (aggression)

All by plain `movss [reg+disp32]`. `[C]`.

### Field map (AI offset / racecar offset)

| AI | racecar | meaning |
|---|---|---|
| +0x004 | +0x1A04 | -> racecar |
| +0x180 | +0x1B80 | arbitrated target POINT (world) |
| +0x1D0 | +0x1BD0 | target-follower speed ceiling (corner brake speed) |
| +0x1D4/+0x1D8 | +0x1BD4/8 | current road section ptr / node index |
| +0x1F8 | +0x1BF8 | target mode 0/1/2/4 |
| +0x200 | +0x1C00 | tracked aim position (nav-graph output) |
| +0x280 | +0x1C80 | aim - car position |
| +0x298 | +0x1C98 | upcoming corner speed |
| +0x720..+0x734 | +0x2120.. | target-follower candidate dir + scalar |
| +0x738 | +0x2138 | avoidance speed |
| +0x75C | +0x215C | arbitrator state |
| **+0x770..+0x77C** | +0x2170 | **arbitrated desired direction** |
| **+0x780** | +0x2180 | **arbitrated speed ceiling** |
| **+0x784** | +0x2184 | **time-to-target (s)** |
| +0x790/+0x794 | +0x2190/4 | aggression state / target racecar |
| +0x7B0..+0x7BC | +0x21B0 | normalized snapshot of +0x770 (this frame) |
| +0x7C0 / +0x8C0 | +0x21C0/+0x22C0 | per-section min/max speed factor tables |
| **+0x9C0** | **+0x23C0** | **target steering angle (DEGREES)** |
| **+0x9C4** | **+0x23C4** | **target speed (m/s)** |
| +0x9C8 | +0x23C8 | time-to-target snapshot |
| +0x9CC | +0x23CC | slew-limiter memory (previous angle) |
| +0x9D0 | +0x23D0 | corner-law speed |
| +0x9D4 | +0x23D4 | yaw-rate error |
| +0x9DC | +0x23DC | catch-up bonus |
| +0x9E0..+0x9F4 | +0x23E0.. | per-opponent difficulty factors |
| +0x9F8 | +0x23F8 | OOC / difficulty mode |
| +0xA08 | +0x2408 | hard speed cap |
| +0xA19 | +0x2419 | wants-boost |

### Per-frame chain

`FUN_00170820` (the racecar's AI update, the same function the takedown
cinematic hands the wheel to via `racecar+0x27D8`) → `FUN_00171A10`:

```
FUN_00173690    AI+0xA08 = Top speed mps (racing)
FUN_0016AAC0    arbitrate: FUN_00175B10 (target follower, nav graph)
                         + FUN_0016C450 (avoidance)
                -> AI+0x780 = min(AI+0x738 avoidance, AI+0x1D0 corner brake)
                -> FUN_0016ADF0 / FUN_0016AE20 commit AI+0x770, AI+0x784
AI+0x7B0 = normalize(AI+0x770);  AI+0x9C8 = AI+0x784
FUN_00171E30    -> AI+0x9C0 target steering angle
FUN_00171BE0    drift decision ("Angle at which drift is started")
FUN_001724F0    -> AI+0x9C4 target speed
FUN_00171D90
```
then the physics side: `FUN_0011BE50` → vtable +0x24 (`0x003B1240+0x24` =
`FUN_00104D30`) → `FUN_00105340`.

## 9. The laws, recovered — all [C] (tools/validate_ai.py, 79/79)

### 9.1 `FUN_0016AE20` — direction and time-to-target
```
d          = AI+0x180 (target point) - racecar+0x40 (position)      [4-wide]
len        = |d.xyz| ;  AI+0x770 = d * (1/len)      (FUN_0002C0D0)
AI+0x784   = (v+0xBC > 1.0) ? len / v+0xBC : len
```

### 9.2 `FUN_00171E30` — target steering angle
Racing path (byte `v+0x1550` clear, or above the LSDM limit and not drifting):
```
k       = "How much carAt affects steering"                (VDB 0.9)
blend   = normalize(v+0xC0 * (1-k) + racecar+0x30 * k)
ang_deg = acos(clamp(dot(AI+0x7B0, blend), -1, 1)) * 57.29578
if dot(racecar+0x10 /*right*/, AI+0x7B0) < 0:  ang_deg = -ang_deg
```
followed by an **asymmetric per-frame slew limiter** on statics
`DAT_00754B28 = 2.4` / `DAT_00754B24 = 8.1` (lazily initialised from
`0x003895AC` / `0x003A2AB0`):
```
prev > 0 : up-limit 2.4, down-limit 8.1
prev <= 0: up-limit 8.1, down-limit 2.4
```
i.e. **winding away from centre is limited to 2.4 deg/frame, unwinding toward
centre to 8.1** — the wheel comes off lock three times faster than it goes on.
`AI+0x9CC` latches the result.

LSDM/drift path (`v+0x1550` set AND (speed_mph <= `v+0x13AC` OR drift state
1/2)) — a yaw-RATE demand instead of an angle:
```
a       = v+0xC0 (+ frame fwd unless AI+0x1F8 == 1), normalized
ang_rad = signed acos(dot(AI+0x7B0, a))          (sign via (a x right) x a)
rate    = ang_rad / AI+0x9C8                     (demanded yaw rate)
cur     = v+0xD4                                 (actual yaw rate)
AI+0x9D4 = cur - rate
gain    = 0.04 (0x0041A50C) when the error UNWINDS the yaw,
          0.01 (0x0041A510) when it winds it up
AI+0x9C0 = clamp((rate - cur) * speed^2 * gain, -90, 90);  AI+0x9CC = 0
```
The "opposite sign" multiplier `DAT_0041A508` is **1.0** in retail — a shipped
no-op.

### 9.3 `FUN_00172E80` — the corner-speed law (§7's "[S] interpolation shape")
```
t   = max(0, 1 - |AI+0x9C0| / "Angle you want min spd at")     (VDB 90)
s   = (drift state 1/2) ? S : t * S                            (S = AI+0x780)
spd = "Min speed mps" + s                                      (VDB 20)
if byte AI+0x213: spd *= 1.45   (0x003A2AB4)
return min(spd, S, AI+0xA08)
```
Note this **adds** Min speed to the scaled ceiling — it is not a lerp between
Min and Top, which is what the harness had assumed.

### 9.4 `FUN_001724F0` — the target speed
```
spd = FUN_00172E80(AI+0x780);  AI+0x9D0 = AI+0x9C4 = spd
if racecar+0x134C == 0:              # traffic class
    AI+0x9C4 = min(spd, DAT_005A9770)            # 22.352 m/s = 50 mph
elif racecar+0x1920 == 1:            # normal racing
    AI+0x9C4 = FUN_00172FA0(spd)                 # aggression speed match
    if AI+0xA31 == 0 and racecar+0x10DC > AI+0xA0C:
        AI+0x9C4 = FUN_001734C0(AI, AI+0x9C4)    # catch-up
    AI+0x9C4 = max(AI+0x9C4, racecar+0x2450 == 1 ? 0 : "Min speed mps")
# steering-error speed cut (drift recovery)
if |AI+0x9D4| > 1.0:
    AI+0x9C4 *= 1 - clamp(|AI+0x9D4| * 0.125, 0, 1)
```
`FUN_00172FA0` returns its argument unchanged while the aggression state
machine is idle (`AI+0x790 == 0`) — verified by disassembly (`ret 4`, early
exit at 0x00172FB6 loads the stack argument into XMM0).

`FUN_001734C0` (catch-up) writes `AI+0x9DC = v+0x155C * 17.8816 + 17.8816`
(17.8816 m/s = exactly 40 mph) when the car is behind and a player car is
within the previous `iVar5` checkpoints. `[S]` — its gating reads the
race-position tables at `DAT_0073A1A8`.

### 9.5 `FUN_00105340` — two corrections to §3
* The coast band is **not** pure coast. `byte [esp+7]`, set at `0x00105380`
  from `v+0x149C * 9.549296 >= v+0x1470` (engine rpm at/above the change-up
  point), forces **full throttle** inside the band. Ghidra reports this as an
  uninitialised stack read; it is a real local. `[C]`
* The throttle dither of §3 is no longer `[S]`: cases "dither at launch" /
  "dither mid-cycle" in validate_ai assert it against the real code.

### 9.6 `FUN_00170B30` — §4's mover, identified
The "unanalyzed mover code ending at 0x1710FA" is a whole function Ghidra
missed, `FUN_00170B30` (gap `0x00170B29..0x001711B0`), reached through the
racecar vtable slot at `0x003B1228`. It is the **out-of-range (off-camera)
kinematic mover**:
* rate-limits `racecar+0x19C0` (current direction) toward `racecar+0x21B0`
  (desired) by `AI/Car/Max desDir angle change OOR` degrees per step, rotating
  about `DAT_0040A8C0 = (0,1,0)` via `FUN_00011900`;
* advances `racecar+0x40` along the road-network node line (`DAT_0073A174`
  point pool, node link table via `racecar+0x18C4/+0x18C8`), applying lateral
  no-go offsets from `FUN_00173E40`/`FUN_00174610`/`FUN_00174680`;
* pins pos.y to the interpolated road height (`racecar+0x244C` + `v+0x870`);
* then runs §4's governor at `0x00171078`.

**§4's "which AI mode runs this mover is not identified" is answered: the
out-of-range one.** It is a hard `FUN_001204C0` speed write on cars that are
NOT running the force pipeline; applying it to a physics-driven car (as the
harness does) has no retail equivalent.

### 9.7 Per-opponent pace — `FUN_00172870`
Reads a **0x98-byte per-grid-slot record** at `DAT_0073A170 + slot*0x98`:
bytes at +0x90..+0x97 x 0.01 → `AI+0x9EC/+0x9F0/+0x9E8/+0x9F4/+0x9E4`, mode
byte +0x95 → `AI+0x9F8`, flag +0x96 bit0 → `AI+0xA12`; and two 16-float lerp
weight tables at +0x10 and +0x50 that interpolate each route section's
`{+0x28, +0x2C}` speed pair from `DAT_0073A164` into the per-section factor
tables at `AI+0x7C0` / `AI+0x8C0`. When no record exists the factors fall back
to `section_speed / Top speed mps` and `section_speed / Min speed mps`.
`[S]` — the record's .bgd source is not traced.

## 10. Reset / rescue (previously harness GLUE)

* **Off-world**: `FUN_001712E0` interpolates the road height at the current
  node from its four edge points, compares `racecar+0x44 - v+0x870`; more than
  **5 m below the road for 61 consecutive frames** (`0x3D`) → `FUN_001714F0`.
* **Stuck rescue**: `FUN_00170820 @0x001708EE` — `racecar+0x1904 > 200` frames
  → `FUN_001714F0(racecar+0x245A - 8, 0, 0)` then
  `FUN_001204C0(v, "Min speed mps" * 0.44704)` = respawn **8 nodes back at
  8.9408 m/s (20 mph)**, clearing `+0x1904` and `+0x2460`.
* **`FUN_001714F0`** itself: set node `+0x18D0` (wrapped by `DAT_0073A188`),
  re-walk the section links, **zero the physics accumulators `v+0xF0..+0x13C`**
  (force / torque / impulse / push-out) and the direction vectors
  `racecar+0x19C0..+0x19DC`, then `FUN_00179760` + `FUN_001709F0` (place the
  car ~3 nodes back, camera-occlusion tested through `FUN_0018E2F0`) +
  `FUN_0010A960`. `[S]` read in full, not ported.

## 11. Traffic constants (correcting the harness)

* Traffic-class target-speed cap `DAT_005A9770` = **22.352 m/s = exactly
  50 mph**, installed by the static-init snippet at `0x002C5E80` from .data
  `0x003B2110`. The harness's `B3_TRAFFIC_SPEED_MS = 13.0` (29 mph) is GLUE
  and wrong. `[C-disasm]`
* `FUN_00105150`'s brake excess is `DAT_005A3A20` = **2.2352 m/s = exactly
  5 mph** (.data `0x003B2330`, snippet `0x002B8D40`). `[C-disasm]`

## 12. The nav graph — format and walker recovered

The **aim point** `AI+0x200` / `AI+0x180` and its corner speed `AI+0x298` →
`AI+0x1D0`. `FUN_00175B10` is decompiled and readable; the harness loads its
.bgd road network in an equivalent form:

* a road SECTION object at `racecar+0x18C4` whose `+4 -> +0` is a per-node
  `{u16 pointA, u16 pointB}` table and whose `+4 -> +8` is a 10-byte-per-node
  link table `{u16 ?, u16 ?, u8 nextSection@+4, u8 prevSection@+5,
  u16 nextNode@+6, u16 prevNode@+8}`;
* a global 16-byte point pool at `DAT_0073A174` indexed by those u16s;
* a section directory at `&DAT_0060EA2C` (stride 8, `[i*8+4]` = section data);
* `FUN_00174CF0` seeds missing/stale navigator state by scanning every
  section/node and choosing the closest centroid of `pair[n]` and
  `pair[n+1]` (four point-pool entries, averaged equally), then clearing its
  route-selection byte. `FUN_00173CB0` is a separate selector helper with
  the type-5 section gate; it does not seed the `+0x27D8` driver;
* `FUN_00174050` classifies the car against that node's four-point XZ ribbon
  using the fixed world-up vector `{0,1,0}`: bits `4`/`8` take a local
  forward/reverse step through `FUN_00174EE0`; at a non-loop terminal that
  helper tries the forward link, then its reverse-link fallback. Bits `1`/`2`
  instead directly select the current node's forward/reverse link, with no
  alternate fallback. `FUN_00175570` applies them in priority `4,8,1,2` and
  uses a 128-step
  guard. `FUN_00174EE0` honors local ±1 moves and wraps flagged sections; on
  an unwrapped boundary it follows the current forward link, falling back to
  its reverse link only when no forward link exists. [C-disasm]
* `FUN_00174740` builds the forward direction from the two-point pairs at
  nodes `n..n+3`: `sum(pair[n+2], pair[n+3]) - sum(pair[n], pair[n+1])`,
  with explicit end-of-section wrap cases;
* `FUN_00176150` then produces `AI+0x1D0` as
  `"Dist to brake speed factor big=>fast" x (dist - corner_speed) + base`
  when target mode is 0, else the corner speed directly, capped by `AI+0xA08`.

`FUN_00158DE0` proves that every index row is relocated from three
row-relative offsets; `FUN_0018B250` publishes those rows directly in the
section directory. The BGD reader now exposes the graph, per-node links, and
the retail nearest-node seed, ribbon-direction rules, bounded graph walk, and
four-node forward difference. `FUN_001772A0`'s separate planning records are
also located at network `+0x1C` and exposed by the decoder. The harness now
loads the graph, maintains the section/node cursor with the exact
`FUN_00174960` → `FUN_00175570` ribbon walk, and uses the selected A/B/C
window for the normal racing aim. Ghidra MCP
confirms that `FUN_00175B10` does **not** read the index row's `edge_rel`
table. `FUN_00173C60` scans its caller-supplied node count and rejects nodes
with `(link+3 & 7) == 5`; `FUN_00176290` always passes `DX=1`, while its
separate `FUN_00178310` call spans the target node through `+8`. Its register
inputs are `EDI=section`, `DX=count`, and `CX=start_node`. It belongs to the
separate `FUN_00176290` selector rather
than the `+0x27D8` route cursor. `FUN_00176290` retains navigator `+0x1D4/+0x1D8`
and carries the route-selection byte `racecar+0x18CA` at navigator `+0x290`.
That byte starts at `0xFF`, while `FUN_00174960`'s nearest-route fallback
resets it to zero. The reset-state non-fallback selector is now ported: when
both type-5 successor gates pass, `FUN_001746F0`'s `up × nav_forward` result
chooses forward exactly when raw-game forward-Z is positive. The
planner corner speed is now available directly in native m/s, and the harness
feeds it through the recovered brake-distance law; the exact
`FUN_00174AF0` approach projection and no-go outputs remain. `FUN_00178310`
is now fully decoded as a traversal-span admissibility
mask. Node type 5 at the starting node returns rejection bit 4 immediately;
an overrun end contributes bit 2 on an open row or wraps on a loop. With the
racing-mode gate `racecar+0x1920 != 0`, type 4 contributes bit 1 while `AI+0x1F1 == 0`,
and type 1/3 does so only when `AI+0x1F0 == 0 && AI+0x1FC != 0`;
`AI+0x291/+0x292` select bit 1 versus rejection bit 4. Its wrapped mode-zero
tail is actually the `racecar+0x1920 == 0` mode and unusually contributes bit
1 unconditionally; its non-wrapped path only accepts type 4. `FUN_001705F0`
initializes this mode to 1. `tools/validate_nav_selector.py` executes the
retail code under Unicorn across this matrix. `FUN_00178520` is its one-node form. The
initializer `FUN_00175A10` starts `+0x1F0=1`, `+0x1F1=0`, `+0x1FC=2`, and
`+0x291/+0x292=1`, leaving only type 4 active at reset. The higher-level state
transitions are not mapped to the harness. Its reset-state branch policy is
now live on a separate target cursor: type-5 entry uses the recovered
two-successor vector tie-break, and a type-4 lookahead seeks the first clear
successor span. The mutable higher-level state ownership remains unported, so
`FUN_00175B10` remains partial.

`FUN_00174050` reports four ribbon half-space bits. `FUN_00175570` consumes
them in priority **4, 8, 1, 2**, with independent opposing-state guards
(4/8 and 1/2). Bits 4/8 call `FUN_00174EE0` with the signed local step; at a
non-loop terminal it always tries the forward link first, then the reverse
link fallback. Bits 1/2 directly select the forward/reverse link and do not
use `FUN_00174EE0` or its fallback. The runtime and `BGD.nav_walk()` now
follow those rules [C, Ghidra MCP].

The `+0x27D8` route-driver dispatch is now separated from that target follower:
`FUN_0018C510` calls `FUN_00170820`, which unconditionally calls
`FUN_0018D790` before its countdown/crash guards. `FUN_0018D790` invokes
`FUN_00174960`; that routine seeds missing state with `FUN_00174CF0`, then
runs `FUN_00175570`. Consequently the runtime graph cursor deliberately
matches `FUN_00174960` → `FUN_00175570`, while the still-unported
`FUN_00175B10` branch selector controls the aim/speed target separately.
`FUN_00170820` also runs `FUN_001711B0`, `FUN_001712E0`, and, after more than
200 stuck frames, `FUN_001714F0(last_valid_node - 8)` followed by the
20-mph relaunch helper.

`FUN_001712E0` is now mapped: it bilinearly interpolates the active nav
node's four point heights, subtracts the vehicle's wheel/body offset, and
only invokes `FUN_001714F0` after the body stays more than 5 m below that
surface for 61 frames. The harness mirrors the recovered threshold and
cadence with a projected bilinear sample of that node's four nav points; the
former 15 m immediate route reset is retired. A 60 m fall remains only as
collision-world containment glue.

`FUN_001772A0` scans the sorted 12-byte planner table for the closest
strictly-upcoming `node_b` in the active section (wrapping only loop rows),
then publishes that record's A/B/C node window. `FUN_00176AF0` tests its
local 12-node look-ahead against that window to move between the three
targets. The runtime now uses that selection for ordinary AI steering.

`FUN_00178100` obtains the car's lateral projection across the selected
node pair, clamps its factor to `0.4..0.6`, and `FUN_001781C0` returns
`pointA + (pointB-pointA) * factor`; disassembly shows the apparently
indirect path normalizes the edge and multiplies it by its length before the
final add. The runtime ports this final pair interpolation as well. [C-disasm]

The stuck-recovery handoff now walks eight nodes backward over the same
retail graph, uses `FUN_00174740`'s four-pair forward difference for heading,
and respawns on the recovered pair aim rather than the harness route line.

Also still open: the aggression state machine that writes `racecar+0x2413..
+0x2419` (`FUN_00169540` family, params fully known); `FUN_0016C450` avoidance;
`FUN_0016AAC0`'s risk arbitration numerics; the source of `racecar+0x1904`.

## 13. Port + validation

`src/burnout3_ai.c` / `.h` implement sections 9.1-9.6 plus the brake helper
(`FUN_00104CA0`), the traffic driver band (`FUN_00105150`) and the avoidance
speed caps. `tools/emulate_ai.py` runs the real functions under Unicorn;
`tools/validate_ai.py` compiles `src/burnout3_ai.c` itself into a probe and
diffs the shipped C against the retail instructions — **79/79 green** (4
commit, 8 angle, 4 drift-angle, 9 corner, 9 target-speed, 28 driver, 5
governor, 3 brake, 1 sixty-frame trajectory with the slew state carried on
both sides [worst |delta| 4.8e-08], 8 parameter provenance).

Provenance of the whole control path: `docs/RE_CONTROL_AUDIT.md`.

---

## 14. THE AGGRESSION (ATTACK / SLAM) STATE MACHINE — §5's [S], closed (2026-08-11)

§5 recorded "the attack state machine lives behind the `racecar+0x2188/+0x2413/
+0x2414/+0x2415` flags … the state-machine code itself was not decompiled
[S/?]". **Both halves of that sentence were wrong.**

* `racecar+0x2188/+0x2413/+0x2414/+0x2415` are **not** attack flags. They are
  the **drift-lock** flags: `+0x2188` (= `AI+0x788`) is the arbitrator's
  drift-enable byte (`FUN_0016AE20 @0x0016AE20` copies `AI+0x212` into it),
  and `+0x2413/+0x2414/+0x2415` are drift-left / drift-right / drift-commit,
  written by **`FUN_00171BE0`** (§14.7). `src/burnout3_ai.h` carries the
  correction; the driver-side consumption in `FUN_00105340` is unchanged and
  still `[C]`.
* The aggression machine is a **separate sub-object** and a separate family of
  functions, `FUN_00169490`/`FUN_00169540`, and it is now fully recovered.

### 14.0 Where it lives — `AI+0x170` == `racecar+0x1B70` [C]

`FUN_00175A10 @0x00175A1F` — the target-follower constructor, `ESI` = the AI
base — does `LEA ECX,[ESI+0x170]; CALL 0x00169490`. Cross-checked from the
other side: `FUN_0016AF10` reads `AI+0x191`, `AI+0x192`, `AI+0x1B8`, which are
exactly the `+0x21`, `+0x22`, `+0x48` fields `FUN_00169490` initialises.

| aggro | AI | racecar | meaning |
|---|---|---|---|
| +0x00 | +0x170 | +0x1B70 | **state** 0..7 |
| +0x10..0x1C | +0x180 | +0x1B80 | **attack aim point** (world) |
| +0x20 | +0x190 | +0x1B90 | **aim valid** — the steering override |
| +0x21 | +0x191 | +0x1B91 | attacking (arbitrator speed gate) |
| +0x22 | +0x192 | +0x1B92 | slam-speed (selects `AI+0x790` 5 vs 4) |
| +0x23 | +0x193 | +0x1B93 | blocked / immune this frame |
| +0x24 | +0x194 | +0x1B94 | contact byte (set by the collision side) |
| +0x2C | +0x19C | +0x1B9C | state deadline (clock, −1 = never) |
| +0x30 | +0x1A0 | +0x1BA0 | clock when the state was entered |
| +0x34 | +0x1A4 | +0x1BA4 | committed side, ±1 |
| +0x38 | +0x1A8 | +0x1BA8 | \|lateral offset\| to the target (m) |
| +0x3C | +0x1AC | +0x1BAC | signed longitudinal gap, + = target ahead |
| +0x40/+0x41/+0x42 | +0x1B0.. | +0x1BB0.. | can-slam / block-range / slam-speed |
| +0x44 | +0x1B4 | +0x1BB4 | own racecar |
| +0x48 | +0x1B8 | +0x1BB8 | **target racecar** |
| +0x4C/+0x50/+0x54/+0x58 | +0x1BC.. | +0x1BBC.. | "rubbed AI goes blind" timer |

Note this **overwrites** §8's field-map row "AI+0x180 = arbitrated target
POINT": `AI+0x180` is the AGGRESSION aim point, and the arbitrator only reads
it when `AI+0x190` is set.

### 14.1 How it reaches the wheels — three channels [C]

**1. Steering (this is the one that makes slams happen).** `FUN_0016AE20`:

```
0016AE2C  MOV AL,byte ptr [ESI + 0x190]     ; aggro+0x20, aim valid?
0016AE34  MOV byte ptr [ESI + 0x789],1
0016AE41  JZ  0x0016AE85                    ; no -> the target follower's dir
0016AE43  MOV EDX,[ESI+4]                   ; racecar
0016AE46  MOVAPS XMM0,[EDX+0x40]            ; racecar position
0016AE4A  MOVAPS XMM1,[ESI+0x180]           ; THE AGGRESSION AIM POINT
0016AE51  SUBPS  XMM1,XMM0
0016AE56  MOVAPS [ECX],XMM1                 ; -> AI+0x770 desired direction
0016AE59  CALL   0x0002C0D0                 ; normalize, returns |d|
0016AE78  DIVSS  XMM0,XMM1                  ; AI+0x784 = |d| / speed
```

So the attack does **not** add a steering bias: it **replaces the aim point**.
Everything downstream — the carAt blend, the asymmetric 2.4/8.1 deg-per-frame
slew limiter, `clamp(angle × MaxLock × −1/180)` — is the ordinary driver. The
35 mph sideways closing speed comes from *where* the aim point is put:

* state 3 (**steer out**) puts it `Steer out distance` = **5 m to the outside**
  of the car's own lane for `Steer out time` = 0.5 s — the AI deliberately
  opens a gap;
* state 4 (**slam**) puts it **dead on the victim**, `own_speed × 0.1` seconds
  ahead of it, for `Slam time` = 0.75 s.

Five metres of lateral offset closed in ~0.75 s is ~6.7 m/s ≈ 15 mph of pure
lateral closing rate on top of whatever convergence the two racing lines
already have — which is exactly the missing ingredient.

**2. Speed.** `FUN_0016AF10` (called by the arbitrator `FUN_0016AAC0`
@0x0016AB42) turns the aggro flags into the two fields `FUN_00172FA0` reads:

```
AI+0x790 = 0
if !DAT_0073A1C0            : return                     ; no racecars
if !aggro+0x21 (attacking)  : return
if !aggro+0x48 (target)     : return
if own+0x2450 == 0 && target boosting (+0x11EE): return
if !(target is our rival (own+0x1650) || we are its rival || own+0x2450 != 0):
    if 100.0 (0x003A2928) > target_mph : return
AI+0x794 = target
AI+0x790 = 4 + (aggro+0x22 != 0)          ; 4 = close in, 5 = lock speeds
```

**3. Boost.** `FUN_00172FA0` sets `AI+0xA17` and arms `AI+0xA2C` (a 2 s window
with a further 2 s re-arm lockout); `FUN_00171D90` latches `AI+0xA19` — the
`racecar+0x2419` the driver `FUN_00105340` turns into the engage request
through the verified `FUN_0017A5B0` gate.

### 14.2 The state machine — `FUN_00169540` [C]

Jump table at `0x00169BB0` (8 entries: `169643 16966D 1696EE 169825 169939
169B9B 1699C9 169AEF`).

Entry gate: `if ((rc+0x1920 == 0 && rc+0x134C != 3) || rc+0x134C == 0)` →
clear `+0x20`/`+0x24` and return. Player cars (unless class 3) and traffic
never attack.

Then `FUN_0016A8C0` (blind timer), the victim-wrecked abort, and
`FUN_0016A7D0` (measure) run every frame before the switch.

| state | name | behaviour |
|---|---|---|
| 0 | idle | `FUN_00169BD0` picks a target → `+0x21 = 1`, state 1 for `Max. time to try and get into slamming position` = **30 s** |
| 1 | approach | `FUN_0016A950` retaliation first; `+0x22 = +0x42`; if `+0x40` (can slam) → the fork; else `FUN_0016A0A0` positioning aim, and if `+0x41` → state 7 for `(BlockMax−BlockMin)×aggression + BlockMin` |
| 2 | retry | same, plus on timer expiry the abandon fork |
| 3 | steer out | latch `+0x34 = sign(dot(own.pos − targ.pos, own.right))`, aim `own.pos + own.fwd×(speed×0.15) + own.right×5×side`; on expiry → state 4 for `Slam time` |
| 4 | slam | `+0x22 = 1`, `rc+0x1BF1 = 1`, `FUN_00169E80` each frame; on expiry the abandon fork |
| 5 | cooldown | on expiry `FUN_0016A310` → idle |
| 6 | recoil | aim `own.pos + own.fwd×(speed×0.15) + own.right×5×sign(−side)` for `Steer out time`; then the abandon fork |
| 7 | block | `+0x22 = 0`, `+0x21 = 1`; `FUN_0016A4E0` block aim; if it fails or the timer expired → state 1; **every exit of state 7 lands on `0x00169B78`, so it always reports `blocked`** |

**The fork** (`LAB_00169687`), reached from states 1/2 when `+0x40` is set:
`aggro+0x38 > 2.5` (`0x003A2D50`) → state 4 directly (**already far enough
out to swing in**); otherwise state 3 first (**too close, open a gap**).

**The abandon fork**: `f = aggression × Max. time to wait between attacks`;
if `(f < 0.1 || own+0x2450 != 0)` and the target is alive → state 2 for 1.0 s;
otherwise state 5 for `f` seconds and a full reset (`+0x21`, `+0x22`, target,
`+0x38`, `+0x3C`, `+0x40..+0x42` cleared, `rc+0x1BF1 = 0`).

**Tail** (`0x00169715`): sweep every racecar; if
`own+0x15E0[slot] + How long after hitting something to disable immunity(3 s)
> own+0x10DC` for any of them, the frame ends `blocked`. That is the
"just hit someone, stand down" rule.

### 14.3 Target selection — `FUN_00169BD0` [C]

```
aggro+0x48 = 0
if Min. aggression before we start attacking (0.002) > rc+0x23E0 : return 0
if DAT_0073A1C0 == 1: target = &DAT_0073A1D0
else:
    best = +FLT_MAX (0x003B172C)
    for each racecar cand != own:
        lat = |own+0x18A4[cand+0x19BC]|         ; per-slot lateral offset
        if best < lat            : continue
        if cand+0x18FA (wrecked) : continue
        target = cand ; best = lat
    if best > 50.0 (0x003B16B8):
        target = &DAT_0073A1D0 + own_vehicle+0x1554 * 0x27E0   ; the player
if target == 0 || target == own                    : return 0
if target+0x1920 == 0 && target+0x27D8 (in a takedown cinematic) : return 0
if own+0x2450 == 0:
    gap = trackdist(target) - trackdist(own)                   ; FUN_00194380
    if gap >  Max. distance apart to begin attacking when behind (150) : return 0
    if -Max. distance apart to begin attacking when ahead (40) > gap   : return 0
    if Min. target speed to consider attacking (75 mph) > target_mph   : return 0
aggro+0x48 = target ; FUN_0016A7D0() ; return 1
```

So the entry condition is **nearest by lateral offset**, then a longitudinal
window of −40 m … +150 m, then the target must be doing at least 75 mph.
Nothing is proximity-in-3D and nothing is rubber-band position.

### 14.4 The per-frame measurements — `FUN_0016A7D0` [C]

* `+0x38` = `FUN_001716D0` = `|dot(own.pos − targ.pos, normalize(flatten(own.right)))|`
  (both vectors have their Y lane zeroed) — the **lateral** offset.
* `+0x3C` = `FUN_001717B0` = `−dot(own.pos − targ.pos, normalize(flatten(own+0x18E0)))`
  — the signed **longitudinal** gap along the road direction, positive when the
  target is ahead.
* `+0x40` = `FUN_00169D70` — **can we slam right now**:
  `target_mph >= 75` and `target_mph − own_mph <= Max. difference in speeds
  (50 mph)`; if the target is behind, `|gap| < own+0x2448 × 0.5` (half the car
  length), else `gap < Max. distance between cars, when ahead (3.5 m)`;
  `dot(own+0x18E0, own.fwd) >= Max. cos angle off lane to stop attack (0.8)`;
  and `+0x38 <= 10.0`. **This is the "side by side" test.**
* `+0x41` = `FUN_0016A3E0` — block range: `|Δmph| <= 20`, and we are ahead by
  between `car_length + 1e-4` and `car_length + Max. distance ahead to start
  blocking you (15 m)`, and the current route node has no junction.
* `+0x42` = `FUN_0016A620` — hold the target's exact speed: not
  `rc+0x2431`, out of control more than 3 s ago, `|Δtrackdist|` within
  `(X − 3) × aggression + 3` where `X` is 40 (chase mode) or `Maximum distance
  from player to start doing sticky speed matching (10 m)`; unless already
  slam-speed, `|Δmph| <= Maximum diffence in speeds for sticky speed matching
  (40)`; `target_mph >= 100` (60 in chase mode); the target is not boosting;
  and `own+0x2444 × 1.1 <= +0x38`.

### 14.5 The three aims [C]

```
state 3 / 6 (steer out / recoil), FUN_00169540 @0x0016988B / @0x00169A33:
    aim = own.pos + own.fwd  * (own_speed * 0.15 [0x00384A80])
                  + own.right* (Steer out distance [0x0047A24C] * side)

state 4 (SLAM), FUN_00169E80 @0x0016A057:
    lead = own_speed * 0.1 [0x003A69C4]
    aim  = targ.pos + targ.fwd * lead

state 1/2 (positioning), FUN_0016A0A0 @0x0016A170:
    d = (targ_dist + targ_speed*0.25) - (own_dist + own_speed*0.25)
    d = (d <= 0) ? 50.0 - d : d + 50.0                      [0x003B16B8]
    aim = targ.pos + own.road_dir * d
                   + cross((0,1,0), targ.road_dir) * sign(+0x34) * 5.0
    +0x34 = dot(normalize(own.pos - targ.pos), own.right)

state 7 (block), FUN_0016A4E0 @0x0016A4FC:
    lead = (-(targ_dist - own_dist) / (own+0x2448 + BlockDist)) * 15 + 15
    aim  = targ.pos + targ.fwd * lead + targ.right * (-sign(dot(...)))
```

Every one of them gates its `+0x20` through `FUN_0016A360`: the physics
vehicle's byte `+0x1550` must be set, the car must not have been out of control
within the last 2 s, and either the current node's link byte `+2` is `0xFF`
(no junction) or `rc+0x1C12` is clear.

### 14.6 The speed leg — `FUN_00172FA0` [C]

Called from `FUN_001724F0` with the corner-law speed on the stack. §9.4's
"returns its argument unchanged while idle" is right but was the whole of what
was known. In full:

```
if AI+0x790 == 0 : AI+0xA1C = 0 ; return spd
rel  = FUN_001717B0(own, target)                    ; + = target ahead
base = max(Min speed mps, target_speed * (1 + clamp(rel * 0.01, -1, 1)))

state 5:  v = clamp(base, own_speed - 1, own_speed + 1)
          AI+0x9D8 = v ; AI+0xA1C = 0
          AI+0xA17 = AI+0xA18 = (target boosting && target+0x10DC - target+0x11C0 > 1.5)
          return v
state 3:  return target_speed
state 1:  return (rel > 0) ? target_speed : spd
state 2:  return (rel > 0) ? spd : target_speed
state 4:
  ok = !(a PLAYER hit us within 1 s: rc+0x16C0 stamp, rc+0x16BC aggressor)
       && !(target has no race progress: FUN_00194430 == 0 && own+0x1394 > 0)
  if own+0x2450 == 1:                                   ; chase-the-player
      out = base
      if same target as last frame:
          if AI+0xA30 && ok && rel > aggression*ExtraBoostDist(-22.5) + BoostDist(15):
              out = Top speed mps ; AI+0xA17 = 1
      else: AI+0xA30 = (rel > 0 && rel > 10) ; AI+0xA1C = target
  else:
      gap = trackdist(target) - trackdist(own)
      if new target: AI+0xA30 = (rel > 0)
      else:
          demand = target_speed + 5 + gap - own_speed
          if ok && gap > 0: out = Top speed mps ; AI+0xA17 = 1
          else:
              demand += own_speed                       ; = target_speed + 5 + gap
              out = max(target_speed, demand)
              if |gap| > How close the target car must be … (10 m):
                  out = (gap > 0) ? out * 2 : max(out * SlowFactor(0.9), spd - 20)
      AI+0xA1C = target ; AI+0xA20 = own_speed ; AI+0xA24 = target_speed
  if AI+0xA17 && AI+0xA2C == -1 && clock > AI+0xA28:
      AI+0xA2C = clock + 2 ; AI+0xA28 = AI+0xA2C + 2
  if AI+0xA19 && !AI+0xA17: own_vehicle+0x1570 = clock + 1
  return out
```

`FUN_00171D90` then latches: window still armed → `AI+0xA18 = 1`; `AI+0xA18`
set → `FUN_0017A530(rc+0x11D0)` and `AI+0xA19 = 1`; otherwise
`AI+0xA19 = (AI+0xA17 != 0)`. `AI+0xA17`/`AI+0xA18` are cleared here, which is
why `FUN_00172FA0` never clears them itself.

**A shipped dead branch [C-disasm].** `0x00173287` sets `AI+0xA17` when
`10 > gap` **and** `target_speed + 5 + gap − own_speed > 10` **and**
`own_speed − target_speed > 13.4112`. The second requires `gap > 18.4`, the
first requires `gap < 10`: unreachable. Ported verbatim and marked.

### 14.7 The drift-lock flags — `FUN_00171BE0` [C]

```
AI+0xA13 = AI+0xA14 = 0
if !AI+0x788 (drift enable): return
mode = AI+0x1F4 ; ang = AI+0x9C0 ; d = AI+0x200 (tracked aim) - rc+0x40
if mode == 4 && ang < 0: AI+0xA13 = 1 ; c = cross(d, (0,1,0))
elif mode == 2 && ang > 0: AI+0xA14 = 1 ; c = cross((0,1,0), d)
else: return                       ; note: AI+0xA15 is NOT touched here
if dot(c, rc+0x30) > 0: AI+0xA15 = 0 ; return
t = |d| / own_speed
AI+0xA15 = (AI/Target +0xA0 [0x0047A1E0] > t) || (ang < -Angle at which drift
                                                  is started [0x0047A158])
```

### 14.8 The per-opponent aggression value `racecar+0x23E0` [C-disasm]

`FUN_00172870` (§9.7's per-slot record at `DAT_0073A170 + slot*0x98`):
`AI+0x9E0 = AI+0x9EC = record[+0x90] × 0.01` (so 0…2.55). Two special cases:
`racecar+0x2450 == 1` (chase mode) → **1.0** (`0x003B168C`); no record at all →
**0.0**, i.e. that car never attacks.

`FUN_001989A0 @0x00198D95` — the slam handler — **raises it on every hit**:
```
rc+0x23E0 = min(rc+0x23E0 + rc+0x23F0 × ctx[+0x0C], rc+0x23F4)
```
so an AI you keep hitting becomes progressively more willing to attack. The
`.bgd` source of the `0x98`-byte record is still `[?]` (§9.7).

### 14.9 What the parameters actually do (correcting §5)

§5 read the parameter names as a description of the machine. Two of its claims
are wrong and one parameter is dead:

* "prefers 3 m separation" — `AI/Aggressive Driving/Slam/Preferred car
  separation when getting into slam position` (`0x0047A23C`) has **no reader
  anywhere in `.text`** (verified by a full disassembly sweep of every absolute
  operand in `0x0047A204..0x0047A25C`). Dead parameter.
* `Min. time to wait between attacks` (`0x0047A208`) likewise has **no reader**.
  Only the *max* is used, and it is used as `aggression × max`, not as a range.
* `How soon after starting to slam is the AI car committed and can't stop`
  (`0x0047A25C`) has no reader either; the actual commitment is structural —
  state 4 runs for `Slam time` and only aborts if the car crosses to the other
  side of the victim or `+0x40` drops.

### 14.10 Port + validation

`src/burnout3_ai.c` / `.h` implement sections 14.0-14.8: `b3_aggro_init`,
`b3_aggro_update` (all eight states plus every predicate and aim),
`b3_aggro_arbitrate`, `b3_ai_aggro_speed`, `b3_ai_boost_latch`,
`b3_ai_drift_flags`. `tools/emulate_ai.py`'s new `AggroSession` lays the
racecars out at their **real** global addresses (`DAT_0073A1D0 + i*0x27E0`,
vehicle table `DAT_0064B38C`, route-length table `DAT_0073A184/88`) because the
machine walks those arrays. `tools/validate_ai.py` is **163/163** green — the
79 inherited cases plus 32 state-machine cases, 31 channel cases (arbitrate /
speed / boost latch / drift flags), 20 parameter-provenance checks and one
240-frame attack cycle carried on both sides (worst |delta| 4.3e-08).

### 14.11 Still open after §14 `[?]`

* The `.bgd` source of the `0x98`-byte per-slot record at `DAT_0073A170` — so
  the *value* of `racecar+0x23E0` (aggression) per opponent, per event, is
  still supplied by the caller. Everything the machine *does* with it is `[C]`.
* `racecar+0x1650` (the designated rival `FUN_0016A950` retaliates against) —
  read here, writer not traced.
* `racecar+0x15E0[slot]` (per-slot last-hit stamps) and `racecar+0x18A4[slot]`
  (per-slot lateral offsets) — read here, writers not traced. Both are
  natural products of the collision/nav stages.
* `racecar+0x2431`, `racecar+0x1BF1`, `racecar+0x18FD` — written by the
  machine (or gating it), consumers not traced.
* `FUN_0016A830`'s arming of the rubbed-blind timer (it calls the RNG
  `FUN_00048760` against 0.5 / 0.1 thresholds) is read but not ported; only
  `FUN_0016A8C0`'s countdown half is.
* The lateral spread that makes the gate `FUN_00169D70` open often — retail
  gets it from the nav-graph lane offsets and `FUN_0016C450` avoidance
  (§12 `[S]`). Until one of those is ported the harness pack runs single file
  and the machine spends most of its time in state 7 (block), which is the
  correct behaviour for a car that is already ahead. Integration note:
  `scratchpad/aggro/integration_aggro.md` §7.

---

## 15. THE AVOIDANCE STAGE — §12's `FUN_0016C450`, recovered (2026-08-13)

§12 listed `FUN_0016C450` avoidance among the things "still open". It is the
one missing input in the whole rival chain: **our rivals had no traffic,
racecar-proximity or obstacle avoidance of any kind**, which is exactly what
"opponent cars are still randomly crashing" and "opponents driving into
oncoming traffic" describe now that traffic actually populates the road.

Recovered from `burnout3.elf` (Ghidra bridge + capstone over the PT_LOAD
image). Markers as elsewhere.

### 15.1 Where it lives — `AI+0x2B0` == `racecar+0x1CB0` [C]

`FUN_0016AAC0 @0x0016AADD`: `LEA ESI,[EDI+0x2B0]; MOV EAX,ESI; CALL 0x16C450`
— EDI is the AI object, so the avoidance sub-object is `AI+0x2B0`. Its own
`+0x4A4` / `+0x454` are the racecar and `+0x4A0` the AI object (planted by the
same navigator ctor family as §8's back-pointers). The fields the rest of the
AI reads are aliases of it:

| avoid | AI | racecar | meaning |
|---|---|---|---|
| +0x040[256] | +0x2F0 | +0x1CF0 | per-strip **type** (2..4 = a vehicle, 6 = road no-go) |
| +0x140[256] | +0x3F0 | +0x1DF0 | per-strip **time to occupancy**, `u8 x 8/255` → 0..8 s |
| +0x240[256] | +0x4F0 | +0x1EF0 | per-strip **range**, `s16 x 1000/65536` → ±500 m |
| +0x440/+0x441/+0x442 | +0x6F0.. | | window width / high index / low index |
| +0x444 / +0x44C | | | road half-width / offset at the node (world→strip map) |
| +0x460 | +0x710 | +0x1E10... | the avoidance **aim point** (world) |
| **+0x470** | **+0x720** | +0x2120 | the avoidance **direction** — what `FUN_0016ADF0` commits |
| **+0x484** | **+0x734** | +0x2134 | time-to-target for that direction |
| **+0x488** | **+0x738** | +0x2138 | the **avoidance speed** |
| +0x490/+0x494 | +0x740/+0x744 | | per-side risk (mean) |
| +0x498/+0x49C | +0x748/+0x74C | | per-side risk (total) |
| +0x4AC | +0x75C | +0x215C | arbitrator state: `0x10` none, 1 / 2 = side |
| +0x4AF | | | 3-frame phase counter |

(§8's field-map rows "+0x720.. target-follower candidate dir + scalar" and
"+0x738 avoidance speed" were right about the addresses and wrong about the
producer of the first: `AI+0x720` is the AVOIDANCE direction.)

### 15.2 The profile — a 256-strip LATERAL histogram of the road [C]

`FUN_0016D2F0 @0x0016D32F` resets three parallel 256-entry arrays every
rebuild: `time[i] = 0xFF` (= 8.0 s, clear), `type[i] = 0`, `dist[i] = 32767`
(= 500 m). The index axis is **lateral**, pitch
`1/DAT_005A96EC = 1/5.0 = 0.2 m` (`DAT_005A96EC` is loaded from
`DAT_003B1694 = 5.0` by the static-init snippet at `0x002C5AC0`), with **index
128 = the road node line** — the `add esi,0x80` / `add edi,0x80` bias at
`0x0016E36C` / `0x0016E384`. 256 strips × 0.2 m = 51.2 m of road.

`FUN_0016FB50` (the arbitrator's "risk at this world position" query) is the
proof of the mapping: it computes
`idx = clamp((int)(-(lateral/len - (w-h)/w) * scale * DAT_005A96EC) + 128, 0, 255)`
from `avoid+0x444`/`+0x44C` and returns `type[idx]` plus three risk outputs
from `FUN_0016FCD0`. `[C]`

The whole stage runs **one frame in three** — `FUN_0016C450 @0x0016C453`
increments `avoid+0x4AF` and only rebuilds when it reaches 3. `[C]`

### 15.3 The four stampers [C]

Called in order at `0x0016D3E9`..`0x0016D407`, all gated by
`racecar+0x1654 + "How long a rubbed AI car goes blind for" <= racecar+0x10DC`
(the blind timer) and by the takedown/wreck check at `0x0016D398`:

| fn | what it stamps | notes |
|---|---|---|
| `FUN_0016F6C0` | **road / no-go edges** | walks route nodes ahead; node 0 uses `Hard No Go offset {distance,time}` (200 m / 0 s), node k uses `Soft No Go offset {distance,time}` + k × `Extra softNoGo offset {dist,time} for future` (101 + 5k m, 2.5 + 0.08k s) |
| `FUN_0016EA40` | **the other RACECARS** | the `DAT_0073A1A8[]` table, `DAT_0073A19C` entries; gated on `racecar+0x134C != 0`; skips wrecked (`+0x18FA`), the aggression target (`+0x1BB8`), and cars in a takedown cinematic (`+0x27D8`) |
| `FUN_0016EB60` | **TRAFFIC** | per-slot proximity list: counts `byte[0x649B36 + slot]`, indices `byte[0x6499F8 + slot*0x19 + i]`, records `0x625FB0 + idx*0x180` |
| `FUN_0016EC70` | the physics-vehicle list `DAT_00731E90[]` (`DAT_00731F9C` entries) | uses `v+0xBC` speed and `v+0x204` frame — a swept forecast |

The write itself is `FUN_0016E3D0`'s loop (`0x0016E3D3`..`0x0016E41C`): the
obstacle's two lateral extents become strip indices `edi`..`esi`, and over
that span

```
if (time[i] > eta) { time[i] = (u8)(eta * 255.0 * 0.125); type[i] = kind; }
if (dist[i] > range) dist[i] = (s16)(range * 65.536);
```

i.e. **keep the earliest arrival and the shortest range per strip**. The two
extents come from the obstacle's position now and its position at the
forecast time, so a crossing car stamps the whole band it sweeps.

### 15.4 The chooser — `FUN_0016C4B0` [C]

```
tmin = min(time[i]) over [lo,hi]
if tmin >= 4.0 (DAT_003B1690):            # nothing to dodge
    aim  = FUN_0016F000() + road_fwd*...  # straight on
    +0x488 = AI+0x1D0                     # corner-brake ceiling, no cut
else:
    # walk out from each edge while time stays within 0.2 s (DAT_003A69B4)
    # of the running best; take the run's MIDPOINT:
    idxA = (a+b)>>1  clamped [1,255]      @0x0016C6C4
    idxB = (a+b)>>1  clamped [0,254]      @0x0016C816
    +0x490 = mean over `win` strips of (8.0 - time)      @0x0016C8AC
    +0x494 = the same on the other side
    +0x498/+0x49C = sums over the band of (8.0 - time) where time < 2.0
    # side select (a five-term predicate on the two means, the target mode
    # AI+0x1F4, and the two totals) -> state 1 or 2      @0x0016CB33
    offset = (idx - edge) * 0.2                          @0x0016CB84
    aim    = car_pos + road_fwd * max(5.0, 2.0*dmin/"Steering factor
             big=>extreme") + road_lat * offset          @0x0016CBED
+0x470 = normalize(aim - car_pos);  +0x484 = |aim-car_pos| / speed
```

`dmin` is the minimum `dist[i]` over the **middle half** of the band
(`q = (hi-lo)/4`, `0x0016CC79`). The strip filter at `0x0016CCB0` is
`if (50.0 > avoid+0x48C || type[i] != 6) include` — i.e. every strip counts
while `avoid+0x48C < 50`, and only once it reaches 50 do the road no-go
strips drop out. The speed then falls out of the registered ladder:

```
dmin < 10 -> "Speed when car is <10m away"  = 26.2 m/s   @0x0016CCFC
dmin < 20 -> "Speed when car is <20m away"  = 40         @0x0016CD1E
dmin < 30 -> "Speed when car is <30m away"  = 60         @0x0016CD40
else, with frac = sum(8-time over type 2..4) / (n*8):
  frac > 0.95 -> "Speed when risk is >0.95" = 16
  frac > 0.9  -> "Speed when risk is >0.9"  = 30
  else        -> AI+0x1D0 (the corner-brake ceiling)
```

### 15.5 How it reaches the wheels — `FUN_0016AAC0` [C]

```
FUN_00175B10(AI)                 # target follower -> AI+0x200 aim, AI+0x1D0
FUN_0016C450(AI+0x2B0)           # THIS stage
AI+0x780 = AI+0x798 ? AI+0x1D0             # avoidance suppressed
                    : min(AI+0x1D0, AI+0x738)          @0x0016AB16
FUN_0016AEA0 ; FUN_0016AF10                # aggression channels (section 14)
corner = (AI+0x1FC == 0 && AI+0x1F8 in {1,2})
r_here = FUN_0016FB50(car position)        # risk at our own strip
if r_here > (corner ? "Current corner risk threshold" (4)
                    : "Current risk threshold" (5)):
    AI+0x770 = AI+0x720 ; AI+0x784 = AI+0x734      # avoidance dir  @0x0016AC4F
    return
r_aim = FUN_0016FB50(AI+0x200 tracked aim)                        # or the
        # takedown-cam variant via FUN_00178AA0 when racecar+0x1B61, x0.5
if AI+0x75C == 1: r_aim -= AI+0x740 ; tot -= AI+0x748
if AI+0x75C == 2: r_aim -= AI+0x744 ; tot -= AI+0x74C
if r_aim > (corner ? "Corner Risk threshold" (0.5) : "Risk threshold" (1)):
    FUN_0016ADF0()                                   # avoidance dir
elif tot > (corner ? "Total corner risk" (1) : "Total risk threshold" (2)):
    ... (0x0016ADCE / 0x0016ADDB tail)
else:
    FUN_0016AE20()                     # racing line / aggression aim
```

So the avoidance never adds a steering bias either — like the aggression
machine (§14.1) it **replaces the aim point**, and every downstream stage
(carAt blend, the 2.4/8.1 deg/frame slew limiter, `MaxLock/180`) is unchanged.

### 15.6 The complete rival input/aim chain — enumeration and port status

`FUN_00170820` → `FUN_00171A10`, per AI car, per frame:

| # | stage | address | what it produces | port status |
|---|---|---|---|---|
| 1 | hard speed cap | `FUN_00173690` | `AI+0xA08` = Top speed mps | ported (`b3_ai_params.top_speed_mps`) [C] |
| 2 | per-slot pace record | `FUN_00172870` | `AI+0x9E0` aggression, `+0x9F8` mode, `+0x7C0`/`+0x8C0` per-section factor tables | partial — record source `[?]` (§9.7/§14.11); harness supplies aggression |
| 3 | **target follower** | `FUN_00175B10` | `AI+0x200` tracked aim, `AI+0x180` arbitrated point, `AI+0x1D0` corner brake speed, `AI+0x298` corner speed | **PARTIAL** — graph cursor, reset-state type-5/type-4 selector, four-flag ribbon walk, A/B/C aim window, and planner-speed ceiling are live; mutable lane state remains |
| 3a | corner brake law | `FUN_00176150` | `AI+0x1D0` from `Dist to brake speed factor` | planner `u16 +6` is direct native m/s; the recovered interpolation is live, with `FUN_00174AF0`'s exact approach projection still GLUE |
| 3b | lateral no-go offsets | `FUN_00173E40` / `FUN_00174610` / `FUN_00174680` | node-relative no-go offsets used by 3 and by the OOR mover | not ported (nav graph) |
| 4 | **AVOIDANCE** | **`FUN_0016C450`** → `FUN_0016D2F0` + `FUN_0016C4B0` | `AI+0x720` dir, `AI+0x734` tt, `AI+0x738` speed, `AI+0x740..0x74C` risks, `AI+0x75C` state | **PORTED this pass** — §15.2-15.4 law, real parameters; strip source is harness route + collision-mesh rays (GLUE) |
| 4a | road no-go stamper | `FUN_0016F6C0` | type-6 strips from the Soft/Hard No Go params | ported in shape: HARD no-go = barrier rays on the real collision mesh; SOFT no-go = the other carriageway at `AVOID: Soft No Go offset time` (2.5 s), its boundary derived from the recovered traffic lane table [S] |
| 4b | racecar stamper | `FUN_0016EA40` | type-2 strips | ported [C law] |
| 4c | traffic stamper | `FUN_0016EB60` | type-3 strips | ported [C law] |
| 4d | vehicle-list stamper | `FUN_0016EC70` | swept strips | folded into 4b/4c |
| 4e | world→strip query | `FUN_0016FB50` / `FUN_0016FCD0` | risk at a position | ported (risk at the strip) [C] |
| 5 | **aggression** | `FUN_00169540` family | `AI+0x180` attack aim, `AI+0x190` valid, `AI+0x790` speed mode | ported §14 [C], validate_ai 163/163 |
| 6 | **arbitrator** | `FUN_0016AAC0` | `AI+0x770` desired dir, `AI+0x780` ceiling, `AI+0x784` tt | **PORTED this pass** — the `min(corner, avoidance)` ceiling plus both risk tests with the six registered thresholds, the second evaluated at the TRACKED AIM (`@0x0016AC81`) and both running every frame while only the profile rebuild is 1-in-3 |
| 6a | commit racing aim | `FUN_0016AE20` | dir from `AI+0x180` / target follower | ported §14.1 [C] |
| 6b | commit avoidance dir | `FUN_0016ADF0` | dir from `AI+0x720` | **ported this pass** [C] |
| 7 | target steering angle | `FUN_00171E30` | `AI+0x9C0` (deg) + the 2.4/8.1 slew limiter | ported §9.2 [C] |
| 8 | drift decision | `FUN_00171BE0` | `AI+0xA13..0xA15` | ported §14.7 [C] |
| 9 | corner speed law | `FUN_00172E80` | the `Min speed + t*S` shape | ported §9.3 [C] |
| 10 | target speed | `FUN_001724F0` | `AI+0x9C4` (m/s) | ported §9.4 [C] |
| 10a | aggression speed | `FUN_00172FA0` | speed match / boost arm | ported §14.6 [C] |
| 10b | catch-up | `FUN_001734C0` | `AI+0x9DC` | documented `[S]`, not ported |
| 11 | boost latch | `FUN_00171D90` | `AI+0xA19` | ported §14.1 [C] |
| 12 | **the driver** | `FUN_00105340` (vtable `0x003B1240+0x24` ← `FUN_00104D30`) | throttle / brake / steer / boost inputs | ported §3/§9.5 [C], 28 driver cases |
| 12a | traffic-class driver | `FUN_00105150` | the reduced band | ported §11 [C] |
| 13 | OOR kinematic mover | `FUN_00170B30` | off-camera advance + the speed governor | governor ported dt-scaled `[S]`; mover not (nav graph) |
| 14 | reset / rescue | `FUN_001712E0` / `FUN_001714F0` | off-world + 200-frame stuck respawn | ported in shape §10 |

**Note on the driver identity.** The task brief named `FUN_00105150`
(vtable slot `+0x64`) as retail's rival driver. It is not: `FUN_00105150` is
the reduced **traffic-class** driver (`racecar+0x134C == 0`), and the rival
driver is `FUN_00105340`, reached from the same dispatcher `FUN_00104D30`
when `racecar+0x179C == 1` (§2, unchanged since the first pass — and
`FUN_00105150`'s own 5 mph brake band, §11, is the tell). The brief's other
open point — "no writer found for `racecar+0x23C0/+0x23C4/+0x134C`" — was
closed in §8: the writers are `FUN_00171E30` / `FUN_001724F0`, addressing the
same bytes as `AI+0x9C0` / `AI+0x9C4`.

### 15.7 Still open after §15 `[?]`

* The target follower's mutable branch policy (`FUN_00176290` /
  `FUN_00173C60` / `FUN_00178310`) — its reset-state graph walk, A/B/C target
  selection, type-5 entry gate, successor tie-break, and span predicate are
  live; `racecar+0x1920` is now decoded as the mode gate (initialized to 1 by
  `FUN_001705F0`), but higher-level AI-state ownership is not yet modelled.
* `FUN_0016C4B0`'s five-term side-select predicate at `0x0016CB33` is read but
  the port uses the simpler "steer to the lower-risk side"; the two agree
  whenever one side is clear.
* `avoid+0x48C` (the `50.0` gate that drops type-6 strips out of `dmin`) and
  `avoid+0x450` (the range lerp factor at `0x0016E329`) — read, not modelled.
  The port takes the `>= 50` branch unconditionally (road no-go strips never
  enter `dmin`).
* `FUN_0016EB60`'s per-slot traffic list (`0x649B36` / `0x6499F8` /
  `0x625FB0`) is the retail proximity cache; the port rescans `g_traffic`.
