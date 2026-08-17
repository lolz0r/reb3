# Burnout 3: Takedown (Xbox) — Master RE Ledger

Consolidated record of everything recovered from the retail XBE
(`build/burnout3.elf`) across the recreation project. Per-subsystem detail
lives in the dedicated `docs/RE_*.md` files; this document is the master
index of addresses, methods, data structures and verdicts.

Provenance marks used throughout the project:
- **[C]** — confirmed: byte/instruction-verified at the stated VA, or executed
  under Unicorn against the retail code.
- **[S]** — strong inference from disassembly/data, not fully executed.
- **[?]** — open unknown.
- **GLUE** — harness-side invention bridging a gap (tracked in
  `docs/PHYSICS_GLUE_LEDGER.md` for physics).
- **TUNED** — user-authorized look deviation (rendering only; physics is
  strict 1:1 per standing directive).

Tooling: Ghidra MCP HTTP bridge at `127.0.0.1:8089` — every request MUST
carry `&program=burnout3.elf` (the default served program is a flat-loaded
duplicate; `/switch_program` lies). Unicorn differential validation uses
persistent sessions (`tools/validate_*.py`, ~4,000 checks total executing the
retail functions). Capstone/objdump over the raw ELF is the fallback when the
bridge is down.

---

## 1. Clocks and timing

- `DAT_0060EA00` — frame timer block. `+0x18` current divisor, `+0x1C`
  per-frame dt = period/divisor (`DAT_0060EA1C`), `+0x24` requested divisor
  (`DAT_0060EA24`). Time dilation = integer divisor on this timer.
- `DAT_0060EA20` — the dilated game clock (audio gates and gameplay stamps
  read it).
- `DAT_004AE1FC` — raw (undilated) frame period; `FUN_00027AD0 @0x00027ADA`
  accumulates it for the takedown-cam thresholds (2.8 s camera return /
  4.8 s presentation end are real seconds even under divisor 5) [C].
- Retail runs 1:1 frame-locked: one pipeline step per rendered frame at the
  nominal period (never wall clock).

## 2. Vehicle physics pipeline (validate_port 95/95)

Per-frame chain [C]:
- `FUN_0011ECF0` — input stage: steering schedule/slew, gear engage, engine.
  Contains the aggressive-driving reaction head gated on class byte
  `+0x215 != 3`: the steer-away envelope — 0.3 s forced full lock away from
  the hit side, authority raised 11.6°/10.6° → flat 24°, lerp back over
  0.7 s; vetoed by chassis-contact byte `+0x212`.
- `FUN_0011BE50` — main path: two substeps at dt/2 of `FUN_0011D460`
  (tyres/drift/LSDM/resistance) + `FUN_001239C0`/`FUN_00123FD0` (per-wheel
  ground rays + suspension) + `FUN_00109560` (rigid-body integration).
  Crashed branch @`0x0011BE75` (gated `veh+0x210`): forces neutral
  (`[EBX+0x14C8]=0`, `[EBX+0x14A4]=1`, shift timer 0.35 from `0x0039B2B0`)
  and calls `FUN_00121560` with throttle/omega/kick/boost = 0 (the engine
  starve — audio coasts to idle in ~0.5 s; down-slew 16.0 rad/s per frame).
- `FUN_00121560` — engine + gearbox update (ported verbatim; drivetrain
  omega at `+0x149C` rad/s).
- `FUN_00123FD0` — suspension; bottom-out contact impulse @`0x001243BF` →
  `FUN_00106720`/`FUN_00106500`: kills closing velocity at the wheel
  positions along the FRAME-UP axis and cancels the pending vertical
  impulse (2.4 m/s in one frame on the landing case; its absence was the
  "kicked and rotated" bug).
- `FUN_00109560` — shared integrator. Gravity `DAT_0040A8A0 = (0,-20,0,0)`
  scaled by mass; for state-6 bodies gravity applies at
  `pos + up*com_height` (i.e. as a torque). Omega clamp: if |ω|² > 10000,
  normalize ω·100 and angmom ·0.95.
- `FUN_000FF270 @0x000FF27D..0x000FF544` — frame re-orthonormaliser, THREE
  branches keeping the most-orthogonal row pair (a=|r2·r1| b=|r0·r2|
  c=|r1·r0|; branch table at 0x000FF332/0x000FF37C/0x000FF3D3/0x000FF3D5,
  degenerate arms 0x000FF41B/0x000FF47D/0x000FF4E1). The one-branch port
  was the wreck-spin energy pump.
- Airborne damper table in `FUN_0011D460` (closed the historical
  `-20·dir.x` known gap).
- Sideways air landing @`0x0011D4B7`: drift entry must also set
  `+0x142C`/`+0x1430` (drift clock) or the 1 s lateral ramp / 0.25 s yaw-cap
  reduction / 0.3 s brake hold are skipped.
- Class inverse inertia (`FUN_001203A0` [C]): default 0.0008/0.0011/0.0013;
  HEVY 0.0004/0.0006/0.0007; HEVYCAR5/6 0.00075/0.0008/0.0011.
- Steering constants `c[96..99]` upload near `FUN_00038D10`; spec power =
  `mat+0x08` (roads 7.4) via `FUN_000393C0 @0x000394A8..D1` slot 0x62
  @0x00039570.

### 2.1 Control-state bytes (racecar/veh struct)
- `+0x210` crashed; `+0x212` chassis-contact-this-substep (set by
  `FUN_0011AEF0`, cleared per force pass and by `FUN_00104840`);
  `+0x215` VEHICLE CLASS — 1 player (`FUN_00117730`), 2/3 the two AI racer
  pools, 4 traffic, 6/7 non-car physics bodies (`FUN_00110280` stamps;
  only 3 skips the steer-away/rollover/countdown blocks; {1,2,3} is the
  "is a car" set `FUN_00123FD0` tests).
- `+0x1168` airborne classification; `+0x14C8` gear (0 = neutral);
  `+0x14B8` manual box; `+0x1444/+0x1446` boost latch (3 s clear from
  `+0x1350` stamp); `+0x1524` drift; `+0x153C` slam side (classifier
  `FUN_00112AC0 @0x112B06`: side_dot<0 on victim, complement on attacker);
  `+0x1528..+0x152B` countdown; `+0x190`/`+0x1160` surfaces.
- `+0x1598` OOC slam stamp; `+0x1690` OOC wall stamp (0.6×/0.8× windows);
  `+0x1534` steering authority / crash authority (see §5).
- `+0x18FA` crashed flag (aborts cinematics, gates flames);
  `+0x18FB` respawn-frame flag (score events destroyed unpaid);
  `+0x1920` 0 = human; `+0x16C8` crash-presentation credit;
  `+0x16C4` damage-machine health; `+0x245D`/`+0x1B93` crash-entry veto [S].
- `+0x27D8` — autopilot fork: routes player inputs to AI dispatcher
  `FUN_00104D30` (@0x00117FCA) and runs the racecar's own AI update
  `FUN_00170820` (@0x0018C53A/54A). Set by `FUN_0018CB60 @0x0018CB6A` for
  the takedown cam t∈[0,2.8); its falling edge restores `+0x1534 = 1.0`
  and clears drift-state 4. Flagged cars untargetable (0x00169CD7),
  rumble-suppressed (0x00017C93).
- `+0x4AC2` — DEAD CODE gate (only writer stores 0) on the racing-stage
  aftertouch block; the LIVE aftertouch is `FUN_00118410` (see §6.4).
- `+0x4AC5` — "steered the wreck this crash" byte (written @0x00118CD3,
  cleared @0x00119C87) — the aftertouch-takedown qualifier.

## 3. Collision world

- Racing gather `FUN_0011BBE0` [C]: excludes only low-byte 0x22/0x23 +
  type bit 0x1000; runtime filters: skip if
  `dot(normal, veh+0xB0 own velocity) > 0.5` for low bytes 0x15..0x20, and
  skip `normal.y < -0.7` for ALL types (ported in `b3_sweep_sphere_ex`).
- Wreck gather `FUN_00109CE0`: excludes 0x20/22/23/24.
- Chevron boards (type 0x0020) ARE collidable in retail.
- Chassis contact resolution `FUN_0011AEF0`: per-contact-event wall scrub
  `vel *= 0.99` @0x0011B5E5 (NOT per frame); crashed-path solver
  `FUN_00123000` has no linear decay while moving (decays at
  0x001232C3/0x00123372 gate on speed < 1). Wall-scrape straightening =
  `FUN_001206D0` at-point impulse, gated BRAKE `veh+0x1404 <= 0.1`;
  the `-1000·mass·dir` stop is car-vs-car only (second poly set count
  @0x0011B09F, writes veh+0xF0). Contact impulse `FUN_00106720` (e=0).
- 24-bit depth needed for road decals (~0.011 unit lift).

## 4. Crash triggers (validate_td_rules 532, carcol 752)

- Wall crash decision (in `FUN_0011AEF0`): `dv` measured AT THE CONTACT
  POINT (ω×r included), fire iff `surface_lo != 0x20 && !(flags1353 & 8)
  && dv > authority*27.5 && headon > authority*0.707 && class != 2`.
  The −0.7071 constant (0x003B1DE0) only skips the two `FUN_00125100`
  launch kicks (JBE 0x0010E42A), never the crash.
- AUTHORITY `veh+0x1534`: view-distance ladder `FUN_00105BD0`/`FUN_00105FC0`
  — player 1.0; others by squared distance to the local player's racecar
  (`DAT_0073A1C0` = local-player count, `DAT_0073A1D0` = racecar array,
  stride 0x27E0 [C executed]); base 15625 (125 m), bands {0,.1,.4,.5,1} at
  0x0039A858; radii seeded by reset thunks 0x002B8D80/0x002B8DA0; past
  ~79 m `veh+0x1353 |= 0x18` disables crash entries. Slam override
  `FUN_00105340`: six conditions, 0.05 race / 0.1 crash party; only human
  attackers (`+0x1920 == 0`) collapse victims' thresholds
  (dv 27.5→1.375 m/s, cone 45°→88°).
- Object/prop crash `FUN_00112E70` (sole caller `FUN_00111CD0 @0x00111D77`):
  car-ish handle vs TYPE-3 prop handle (type-3 swapped into slot [10]);
  crashability = `DAT_0039AE50[classB*7+classA]` (`FUN_0010FBC0` classes;
  object row = racecar or the designated big-hit traffic vehicle,
  `entity+0x242B == DAT_0073BB8C`); `closing_mph > authority*75`
  (0x003EBE44; ×20 crash party 0x003EBE48); crash party compares |v_rel|,
  race compares normal-direction closing; `veh+0x152C` = 0.5 s post-spawn
  immunity. STATIC WORLD NEVER ROUTES HERE (only the wall test).
- Hard-landing roll `FUN_0010ED30`: `rand01 < (impact-8000)/17000` —
  recovered, deliberately unported (user chose to skip).
- Traffic big-hit: `+0x174 & 8` designated vehicle arms the 0.35 s
  divisor-6 impact window (`FUN_00026A70`); cancels the ordinary wreck
  (0x00113077).

## 5. Crash flight, recovery, presentation

- Launch: crash-entry impulse pair (corner torque 0.40 @0x00024F94-family +
  linear up 0.65 rollover @0x0011C421, dv = 10·mag); crash-director
  magnitudes 0x0064ACE8+0x57C..0x594 are all 0.0.
- Flight oracle: `tools/validate_crash_traj.py` 134/134 (retail whole
  flights; travel 0.967 m per m/s on seq_wall35; port within 2.3%).
- Recovery: retail stamps 5 GAME seconds (`FUN_00198E60 @0x00198F65`,
  `[0x003B1694]` = 5.0); early releaser `racecar+0x19BE` (written by
  0x0018D740) still [?]; harness uses a wall-clock cap + grounded-and-slow
  release (deliberate deviation) and relaunch at speed (retail:
  `FUN_00025500` → `FUN_001714F0` place ~3 nodes back + `FUN_001204C0`
  speed; constants 0x41569446 = 13.4112 = 30 mph, 0x4232D0E5 = 44.704).
- Presentation: ONE divisor-5 credit per car per event (`+0x16C8`, set by
  event reset `FUN_00025AB0 @0x00025AE5`; gate `FUN_00025850 @0x00025890`
  → `FUN_00025CC0` decrements, requests divisor 5 @0x00025D5C).
  Takedown credit spend: OnTakedown `FUN_00025A30 @0x00025A7E/@0x00025AA3`
  → `FUN_00025CC0` charged to the ATTACKER; divisor-5 only for human
  attackers (0x00025D29 needs `+0x1920 == 0`); gate:
  (AI attacker && human victim) → spend no FX; (human attacker &&
  0.3 [0x003B1750] > `+0x16C4`) → spend + FX.
- Impact Time: `FUN_00118410 @0x001188A4` — holding BOOST while crashed
  requests divisor 5, unlimited.

### 5.1 Panels (damage machine)
- `FUN_001253C0(arg)`: crash entry `FUN_00115130 @0x0011560B` passes 1 =
  CRUMPLE only; EXPLODE `FUN_00120800 @0x0012085E` passes 0 = detach all.
- States (ctx+0x4B2): 0 pristine / 1 loose-flapping (`FUN_0012C860
  @0x0012C9F9`, animated matrix over ctx+0x180) / 2 crumpled / 3
  detached-flying / 4 gone; draw gate state < 3.
- Accumulators: snapshot → `FUN_0012C670` adds `min(impact*0.002, 0.1)` to
  panels whose pivot dots negative with the body-space contact axis →
  one-frame rip test `acc − snapshot > rip_band*0.3` → release
  (`FUN_00123000` + `FUN_00125A50`). Thresholds randomized per car at
  `FUN_0012FEE0`: loose 0.05–0.10, crumple 0.085–0.17, rip 0.14–0.20.
  Impact arms: collision `FUN_0012FA40` (×1/3200, gate 5.0) and contact
  `FUN_00123FD0` (raw gate 2000, halved).
- Crash entry fires FIVE 1e6 damage events from the bbox faces
  (`FUN_00115130 @0x00115265` → `FUN_00127180`).
- Panel kinds (`.bgv+0xAC4`): 0/1 doors, 2/3 front/rear wings, 4 bonnet
  (pivot x = 0; 50/50 skip-loose + flip-up pose), 5 boot/hatch, 6 extra.
  Hinge axes at `.bgv+0xADC`/`+0xAFC` (loose pose, not yet in sidecar [?]).
- Panel/debris flight = class-7 bodies: ctor `FUN_001068A0`, pool
  `gameworld+0xD3380` stride 0x4E0, update `FUN_00106D00` via vtable
  0x003B1108 (ledger PH-05 spec).

### 5.2 Aftertouch (all [C])
- Producer: `FUN_00118410` (crashed-path input shaper, called from
  `FUN_0011BE50`'s crashed branch) — publishes axes `veh+0x1408/140C`
  (dpad + left stick) and @0x001189A3..0x00118CD3 rotates the wreck's
  VELOCITY VECTOR one clamped yaw step per tick toward a camera-relative
  direction (packed camera quaternion `veh+0x1410`); authority
  `0.4/(crash_clock+1)` degrees; gates: boost held, speed > 1,
  crash clock < 5 s, |h|+|v| > 0.5. (The racing-stage 0x4AC2 block is dead.)
- Aftertouch takedown: `FUN_00113960` stores the wreck into the victim's
  cause record +0x0C; `FUN_00197430` tail arms the claim iff wreck
  `+0x4AC5`; `FUN_00198E60 @0x00198EF0` → `FUN_001994D0` → message 0xAA,
  +1250 BP. Retail byte `score+0x4F0` (racecar+0x15C0[]) does BOTH
  claim-force and aftertouch-qualifier jobs (port had split arrays — merge
  specced).

## 6. Takedown rules and scoring

- Slam kinds (carcol event = game-context vtable +0x64 kind): 5/6 full slam
  (stamps OOC + aggressor), 3/4 light (BP only), 2 stamps aggressor, 1 rub.
  A slam NEVER wrecks directly.
- Attribution: victim crashes within Maximum Crash Wait → claim; commit
  after Race Car Clear Wait 0.5 s (score +0x4D8 scan `FUN_00197040`);
  DENIED/LUCKY via callout priority (`FUN_00199350`); revenge bookkeeping
  score+0x5B9 (`FUN_00198E60`).
- Boost bar (validate_gameplay executed): award `FUN_0017A530`; engage
  `FUN_0017A5B0` (min units, recovery gate); drain `FUN_0017A480` (stop at
  empty; AI fixed burns B3_MIN_BOOST_TIME); human release = input bit
  `v+0x13FC & 4` held; tier upgrade `FUN_000273F0` (max 3).

## 7. AI

- Rival driver: `FUN_00105340` (vtable 0x3B1240 slot +0x64 consumer chain;
  dispatcher `FUN_00104D30` when `racecar+0x134C != 0 && +0x179C == 1`).
  `FUN_00105150` is the TRAFFIC-class driver (earlier mislabel corrected).
  Speed governor tail at 0x171078. No writer found for
  `racer+0x23C0/0x23C4/0x134C` in this image [?].
- Avoidance `FUN_0016C450` (docs/RE_AI.md §15, 22 chain stages): object at
  AI+0x2B0 — 256-strip lateral histogram, 0.2 m pitch (`DAT_005A96EC`=5.0),
  index 128 = node line; stores time-to-occupancy/range/type; four stampers
  (road no-go, racecars, traffic, physics vehicles); chooser `FUN_0016C4B0`
  (4.0 s threat gate, 0.2 s band slack, midpoint pick, mean(8−time) risk,
  speed ladder 10/20/30 → 26.2/40/60, aim `max(5, 2·dmin/5.1)`); arbiter
  `FUN_0016AAC0` (two thresholds).
- Takedown-cam autopilot: §2.1 `+0x27D8`. Cam clock: §1.
- Traffic driver: road agents (`S` 0x50 @0x0063DCB0) under `FUN_001A20F0`;
  speed law `FUN_0019F560` [C]: K = 1/6.5, ramp clamp −0.1/+0.08 m/s
  per frame, 35 m avoid radius, car-following gap −2.5 m, releasing brake
  latch; speed cap `DAT_005A9770` = 22.352 m/s.

## 8. Traffic data

- Mode block "TDESC": per-event `{size=param+0x3C4, offset=param+0x3C8}`
  (@0x0018B569/@0x0018B5BB). Six {ptr,count} CLASS-POOL lists at
  +0x54/+0x60/+0x6C/+0x78/+0x84/+0x90 (relocator `FUN_00158B70`):
  `FUN_001A5E30` maps class 1→+0x54, 2→+0x60, 3→+0x78, 4→+0x84 (semi
  tractors), 5→+0x6C, 11→+0x90 (trailers — models with no front axle).
  Record = {u64 base-40 id, u8[8] weight vector (sums 100), u32 selection
  weight, u32 zero} — NO pairing field; the spawner hard-assigns
  `if (primaryType == 4) trailerType = 0x0B` @0x001A63A6. Trailer towing =
  mode-1 kinematic trailing caster `FUN_001A8640`.
- Special traffic: five u64 ids at TDESC+0x00..+0x20 gated by flag byte
  +0xB5. Spawn table {ptr=+0xAC, count=+0xB0} stride 0x20 {pos[3],w,
  dir[3],w} — dir[3] is the instance matrix Z column = the BACKWARD axis
  (the lane-direction sign bug). Loader chain `FUN_001A13F0` →
  `FUN_001A4260` (".btv") → relinker `FUN_000310F0` (same as .bgv).
- Lanes (recovered from spawn projections, US_C3_V1): with-race at laterals
  +14.56/+20.83, oncoming +2.66/+9.07 relative to the oncoming polyline;
  race direction = DESCENDING polyline index; the game's start grid
  (SPATIAL @0xA000) sits at +14.29..+21.72 facing descending; race line
  (Gamedata.bgd 0xC0930) spans +11.4..+24.1.

## 9. Props and particles

- Prop placement: every `static.dat` carries a second 0x70-record model
  table: hdr+0x36/+0x3C models, +0x40/+0x48 instance 4x4s (w slots = baked
  instance colour), +0x44 per-model CLASS byte, +0x4C/+0x50 per-unit
  instance lists; reader `FUN_00110420 @0x001109CB..0x00110A46` (world slot
  type 5, u16 index<<6); `IMUL class,0x70` @0x00110A35 out-of-range quirk
  on some tracks [?]. 37 tracks, 13,359 instances, 436 models.
- Prop bodies: promotion `FUN_0011A020` — mass `max(100, footprint*200)`
  (@0x0011A137, consts 0x3A2928/2C), inv inertia `1/((e_j²+e_k²)·m·0.5)`
  per axis (`FUN_00109BB0`→`FUN_00109190`), com `(bbmax.y+bbmin.y)·0.5`,
  launch = four PRNG draws @0x0011A1DD:
  `v = (u0−0.5, u1·0.5, u2−0.5)·((u3+1)·5)` (y never negative). Contact via
  generic solver `FUN_00113960` (un-crashed car forced role 2 = immovable
  @0x00113B57; normal bent −0.9 toward relative velocity @0x00113F16;
  `FUN_0010F8D0` two-body impulse, restitution 0). Update `FUN_0011A330`
  (class-6 vtable 0x003B1120 slot 0): `F += dir·−speed²`,
  `T += ω·(−2|ω|)`, then `FUN_00109560` (state-6 gravity-as-torque).
  `+0x224` = LRU key (not a timer); 16-body pool `gameworld+0xC4380` stride
  0x780; recycled slot retyped 8 = stops colliding. Cones can't wreck you:
  `DAT_0039AE50` row 6 all zeros [C]. Prop-hit "audio" `FUN_00197A20` is
  actually the prop score accumulator.
- Particle engine (NOT the sprite pools): 24 descriptors at 0x004182A0
  (1249-instruction branchless initialiser), 26 emitters 0x003A3648, 40
  surface rows 0x003A3BF8; update `p + v·t + g·t²` (no ½); distance-based
  emission with dither carry; literal D3D8 blend tokens — crash-trail smoke
  SUBTRACTIVE; D3DCOLOR here is 0xAABBGGRR (red = LOW byte,
  `FUN_00035740 @0x000357BE`, red lane @0x0003547E); screen clamps apply to
  the HALF-extent (0x000346B9→0x00034814); rotated kinds build a diamond
  (`FUN_00011570` negate), kinds 3/4 scale by √2 (0x003A34B8), kind 4
  (smoke) draws at HALF alpha (MULSS 0.5 @0x000352FD); pool wraps its ring
  (`FUN_00035740`/`FUN_00035C00`); particle vertex path `FUN_00034130`
  (pre-transformed screen quads, `half_px = size·(1/w)·{W/4A, H/4B}`
  @0x00034678, A/B = tan(fov/2) via cam+0x58 published @0x001AE769).
  Wheel dust gates ARE the surface row: wheel bytes +0xA0/A1/A2 slewed by
  `FUN_001805B0` (×255, 510/s) toward `surfrow[0]·skid/[4]/[8]`, emitters
  0/3/4, rear wheels only (@0x00180924); per-system distance fade at
  system+0xC0..0xCC [?].
- Sprite pools (coronas/flames): table 0x003A3E7C = exactly three corona
  records; pool draw `FUN_00042BC0` (corner = pos ± right·size ± up·size −
  (pos−eye)·pull; per-sprite far cut +0x0C); push `FUN_00042B00`
  (stores arg3·0.5 @0x00042B64).

## 10. Rendering

### 10.1 World (validate_postfx 171, port world sections)
- Material dispatch `FUN_000393C0`/`FUN_00038D10`. Flags: 0x001 alpha test
  (GREATER 64/255), 0x010 blend, 0x020 two-sided, 0x100 ping-pong (unused
  by shipped data), 0x200 UV scroll, 0x400 decal ZWRITE off. Six world
  `D3DPIXELSHADERDEF`s: class 0/6/1/7/10/2 at 0x003E8D08/0x003E8DF8/
  0x003E8EE8/0x003E8FD8/0x003E90C8/0x003E91B8 — all
  `PSRGBOutputs[0]=0x000100C0` (SHIFTLEFT_1 = ×2, proved against
  `FUN_0034E790 @0x0034E90F` copying to NV097_SET_COMBINER_COLOR_OCW);
  one sampler (PSTextureModes 0x00000001); final combiner 0x130C0300
  (fog lerp; E/F ZERO). Equations: base `2·T0·V0`; class 1/2/7 add
  `(T0.a·V0.a)·C0.rgb` (C0 = sceneLight × mat+0x04); class 10 adds
  `T0.a·C0.rgb`; class 6 alpha `T0.a·C0.a` (C0.a = mat+0x20).
- Class-1 VP 0x003E88C0 (31/32 instr): `V = pos − c[0x60]` (eye + 5.0 y,
  literal 0x003B1694, add @0x00038F71), `R = 2N(N·L) − c[0x61]`
  (= −(light+0x00), XORPS 0x00038D4C..65); LIT specular; `MIN oFog, z,
  c[120].z` (fog coordinate FLOORED at fog_far — upload chain 0x00038D8D →
  0x00038DD8 → 0x00038F59). The world light direction is enviro.dat+0x80
  (normalised in place by `FUN_001888F0`/`FUN_00011570`), y = −sin(round
  elevations) on all 36 tracks → the specular lobe is ZERO on roads.
- Vertex decls 0x0038758C..0x003875E8: one 28-byte vertex (pos FLOAT3 +
  NORMPACKED3 + DIFFUSE D3DCOLOR + TEX FLOAT2); no second colour stream.
  Vertex colours half-range (128 = white).
- Animated materials: `FUN_0019B1E0` (sole caller `FUN_001AA720
  @0x001aa7c3`) — two arms: frame_count < 2 → UV scroll (flag 0x200; U-only,
  writes `1−frac(phase)` to X lane 0x00039CE6..0x00039D0C; Arrows rate 1.2
  step 0.125); frame_count ≥ 2 → TEXTURE-FRAME CYCLING (no flag; durations
  = `atol` of the next frame's name past frame 0's stem — keyframe times in
  periods). 19 animated materials across 38 tracks (9 scroll, 10 frame).
- Unit/LOD: `FUN_0019D100 @0x0019D23B..49` |unit offset| vs 4 → blockA/B
  geometry swap; submesh frustum test `FUN_001B23F0` vs `DAT_004D67F0`.
  Decal opacity constant per material (`FUN_000393C0` class-6 arm
  0x000396B8..D9 → `FUN_0034E9A0` packs ×255); no distance fade.
- Gamma: `FUN_0003C8A0` installs `ramp[i]=round((i/255)^0.95·255)` once
  (@0x0003D3F0..0x0003D484) — SetGammaRamp path; the harness applies it as
  a GL LUT present pass (SDL ramps are dead on modern Linux).
- Present composite [C]: whole scene renders to a 640×480 LIN_A8R8G8B8 RT
  at renderer+0x890 (`FUN_0003FA20 @0x0003FA43`), reduced 640→320→160×120
  (`FUN_0003E520`, 2×2 box), presented by `FUN_0003DA90` via
  `FUN_000402C0`, PS def 0x003E9EA8:
  `out.rgb = 2·(scene + C0.a·blur)`, `C0.a = min(s,2)·0.5` — NOT energy
  conserving; the ×2 was the long-hunted world brightness deficit. Blur
  strength producer `s` (blurState+0x54) has no writer found [?].
- Sky: dome `FUN_00032020` (264 verts/1344 idx); PS 0x003E9B08:
  `out = C0·T1·(C0.a·T1.a) + T0·(1−C0.a·T1.a)`, C0 = (0.5,...) set
  @0x000327A7 (no shift — the old "MODULATE2X" was wrong). Sky LUT:
  `FUN_001891F0` rebuilds a 64×32 RT per frame (CreateTexture @0x001A9FFD →
  `DAT_004A1D04`) from the 32×32 gradients sheet via three blits
  (`FUN_001C82E0` blend tables): mode 3 RGB `0.5·sheet[col(progress)]`;
  mode 7 ALPHA 13 quads — 2-D sun-halo mask in (azimuth, elevation); mode 4
  DST_ALPHA lerp toward `0.5·sheet[col+0.5]` (glow colour). U_SPAN 15/32.
- Renderer object base 0x004D6170 (pinned @0x00015C47); stage-state array
  state-major 0x0075D740 + k*0x10 + stage*4 (`FUN_001D7180` seeds
  1,1,1,2,2,0).

### 10.2 Car paint (validate_carfx 223)
- Body VP 0x003E7D58 (32 instr, uploaded by FUN_0003C8A0 @0x0003CB78):
  `oD0.rgb = E(N)` (L2 SH irradiance, Ramamoorthi–Hanrahan constants in
  .rdata), `oD0.a` = Schlick F vs c111 (R0 0.18357/0.03994),
  `oT1.xyz = 2N(N·V) − V̂` (reflection vector); c108 = eye
  (`FUN_00031690` uploads 0x004D67D0 to hw slot 0x6C).
- Body PS def 0x003E8468 (two stages, 8 combiners):
  `base = 2·paint·E(N)`; `refl = paint.a·(P + (1−P)·F)·fade`, P = c14.w =
  0.1 (Fresnel floor); `col = mix(base, env, refl)`;
  `spec = clamp(paint.a·env.a·fade − (1−K))·M/(1−K)`, `out += light·spec`.
  paint.a = per-texel gloss mask.
- Stage-1 env texture binding: `MOV ECX,[0x004D6C00]; MOV [0x0075DB74],ECX`
  @0x00031740 = renderer+0xA90 — writer PROVEN ABSENT from the image (four
  exhaustive scans + two whole-image Unicorn sweeps); substitute [S] =
  enviro.dat+0xA0 sphere map (relocated @0x001888BF; artist names
  envmapclouds/SkyEnv on 33/37 tracks).
- Light probes: THE COLLISION MESH — each collision prim's u16[4] at +0x06
  = per-corner probe indices into 9-signed-bytes-per-probe array at unit
  block +0xB0 (byte-verified all 36 tracks); `FUN_0019D400` rewrites SH per
  frame per car from position.
- Texture byte rec+0x1A indexes a five-entry DRAW-CONTEXT array at
  ctx+0x334 (`FUN_00031AB0 @0x00031AC5`): 0 = own paint (compact1,
  `FUN_000303D0 @0x00030546`); 1 = "VehicleUnderside" (Global.txd 512×256);
  2/3/4 = Unbroken/Cracked/SmashedGlass (globals 0x4D61A8..B4 filled by
  name at 0x0002F260 via `FUN_0002DDF0`).
- Coronas: per-car light tables at model+0x1664 (types) / +0x16AC
  (positions/normals), stride 0x30; types 0 head / 1 tail / 2 brake /
  4 reverse / 5,6 indicators / 8 exhaust (boost flames, model+0x1684/
  +0x16B4) / 9 impact glass / 10 wreck smoke / 11 wreck fire. Emitter
  `FUN_00187C70`; cosine gate `FUN_00187BE0` (dot(eye, lampNormal) > 0,
  colour × dot, no 1/r²). .btv files carry the SAME table (40/40 parse,
  673 lights).
- Boost flames `FUN_001871E0` (dispatcher `FUN_0017F730 @0x0017F73C`):
  three additive billboards per pipe at 0.08/0.14/0.18 along the record
  normal, half-sizes {0.5,0.25,0.125}·S, colours C·U(0.5,1)/U(0.28,0.56)/
  U(0.14,0.28) (the random IS the flicker); level = boost record +0x14 via
  `FUN_0017A480` tail (flare 2.0, decay 2.0/2.5, floor 1.0);
  carObj+0x1901 per-model constant (`FUN_0018D0E0 @0x0018D4CB..D534`): the
  five Car10 specials burn orange (coronaboostred), all else blue.
- Wheels: radius .bgv+0x18, attach matrices +0xB80 stride 0x40; draw picks
  the wheel part object by |spin| — slot 7 < 25 rad/s, 8 above, 9 above 50
  (`FUN_000303D0`); blobbyshadow `FUN_0019A7C0`/`FUN_00043570` (airFade =
  1 − min(h·0.8, 1); alpha ramp ×0.7; scale airFade·0.4 + 1; two bbox +
  two axle Z rows @0x0019A8E3..0x0019A917).

## 11. HUD / frontend (validate_hud 760)

- Virtual space 640×480 (B3HUD_VIRT_W @0x003B1F00). Element01 three-slice
  plates `FUN_00048430`; fonts XBE-embedded; text styles `FUN_0004DA90`.
- HUD STATE MACHINE: `FUN_00053A10` masks each element vs
  `DAT_00388F78[state] = {0,1,2,4,8,0xF}`; racing elements carry mask 1;
  the crash letterbox "blades" element 0x003FF2C8 (ctor `FUN_000509B0`,
  builder `FUN_00050A70(elem,2)`) carries mask 6 and appears in state 2 —
  requested by the enter-crashed handler @0x0018C7EB; ALL racing HUD
  vanishes during a crash; transitions snap (timer 0 @0x00053D5F).
  Blades: 640×60 bars at y 0/420 with 2 px inner gradient to
  (0.47,0.72,1.0).
- IMPACT TIME element: ctor `FUN_000511C0` (loads "A_Button" glyph by
  name), update at 0x00051230 (undefined-run function); text Globalus 2191,
  124 px right-aligned; glyph 30×30 at text_left−4−15; swaps to the
  aftertouch cursor (54×36, callback `FUN_0004FCA0`) when boost held:
  four wedges around (0.5,0.45) from 7-vertex table 0x003FCF38 +
  4-primitive table 0x00388928; pulse `1−frac(t·2)²`; alpha ease
  `2a−a²`; gloss pass mirrors U; art = Global.txd "Aftertouch" (right half
  of the badge; UVs mirror about the vertical centreline; ValueDB endpoints
  not in image [S]).
- Crash ticker: `FUN_0004ED40` joins descriptors with "+ "; `FUN_0017A720`
  formats from a 30-row table at 0x003A2F70 (flags: &4 metres×3.28084+"ft";
  &8 %.1f+"s" — AIR is SECONDS; &1|&2 count×180°; &1 Double/Triple names);
  "Into A Vehicle" via a 40-entry base-40 key table whose alphabet is the
  SFX charmap ROTATED BY TWO.
- Takedown cam: presentation state 3 on entry, state 1 (racing view)
  broadcast at t=2.5 via `FUN_00053D20` (0.3 s before the wheel at 2.8);
  crash immunity across [0, 4.8]: `racecar+0x245D` → `FUN_00105340
  @0x00105F95` + 0x00027C9D directly for [2.8, 4.8).
- Chase camera = director mode 2 (`FUN_0015E550`): offset (0, 0.95, −6.8),
  focus 2 m ahead, pitch = asin(fwd.y), FOV 90→110 on boost.
- Opponent tags: ordinal < 35 depth, yellow triangle beyond; 4.5%
  title-safe inset.

## 12. Audio (validate_sfx 399, music 100)

- Racecar audio per-frame update `FUN_00136120` (no Ghidra record; found
  via xrefs): children = tyre skid loops, surface loops, gear shot, rev.
- Tyre skid/squeal `FUN_0013DE10` ×2 (per side): three CONST01/02/03 loops
  on a slip/spin integrator with lag limiting; ValueDB two-point curves
  (recovered by EXECUTING registrar `FUN_00137F50`: 96 Sound/Surface + 48
  Sound/Skids points); ±1000 Hz side detune; the SPIN branch writes |total|
  back into the slot the 3-sample loop re-reads (only sample 1 sees Spin
  curves) — reproduced verbatim. Squeal = cornering AND brake lock-up.
- Surface beds `FUN_00136610`: four looping voices TAR/GVL/WOOD/METAL/SNOW,
  gain lerped to 44 m/s with `1+4·slip` lift; slip lift muted on tarmac
  (jump table 0x00136C2C); skid samples weigh 0 on gravel.
- Traffic pass whoosh `FUN_00146530`: bstpsl0N/bstpss0N fired by a
  zero-crossing of a lead plane pushed so the sample's 0.25 s peak lands on
  the pass; volume `max(masterVol, 0.09)·2.0` with masterVol ramped to 1.0
  by `FUN_00145F60`; per-frame voice re-positioning `FUN_001CC3E0
  @0x0014663C`; 15/50 roll-off `FUN_001CD180`; Big/Little Pass Scalar
  1.0/0.8 under Sound/Boost; the 40 "m/s" gate (@0x003B1884→0x0040FBF0)
  reads like an mph-vs-m/s unit slip [S].
- Crash audio: engine STARVED not muted (§2 crashed branch); crashed
  contacts route IMPACTFATA (`FUN_0014D0F0`; impulse ÷ mass DIVSS
  [ESI+0x1F0] @0x0014D223; window 2..10 m/s, gain 0.8..1.0); slow-mo
  pitches nothing (audio gates read `DAT_0060EA20`); mix state machine
  `FUN_0013F610`/`FUN_0013F840` (table 0x004191B0) ducks nothing in crash.
- Crash bed: `sprintf(mgr+0x4B0, "tracks\\crash%d.rws", (mgr[0x890]%20)+1)`
  @0x0014C269 (race init, DOUBLE bump 0x0014C282+0x0014C2D6) / 0x00151239
  (rotate); buffered pre-crash (`FUN_001CB6C0 @0x001512A0`); unpaused by
  `FUN_00150D40` within 50.0 units (0x003B16B8); LAYER GATE = the divisor:
  `CMP [0x0060EA18],1` @0x00150F4F — 1 → aGenCrashNN (sub-stream 0,
  bed+0x15C), else zSloCrashNN (+0x160; `FUN_001CB9E0` MOVSS indexed);
  level 0.70 (0x003EC418) vs song 0.30 (0x003EC424); release 0.5 s linear
  (0x003B1684 × 0x003B1688=2.0) on the frozen layer (mgr+0x8C0).
- Boost audio: BoostIn/BoostLoop/BoostOut + fire layer, ASCII literals in
  `FUN_00136F80` (unique in the module); gain 1.0; head-locked.
- SLAM/SHUNT split `FUN_001989A0` → `FUN_00140610`/`FUN_00140480`.
- EA TRAX: 44-song table at 0x003EC458 (24-byte stride; Globalus ids).

## 13. File formats and containers

- `.bgv`/`.btv` (shared relinker `FUN_000310F0`/`FUN_00031010`): sections
  at +0x4C+li*4; body records (m, tx, tris; rec+0x10 = index COUNT);
  wheels radius +0x18, matrices +0xB80; panels pivots +0xAC4 kinds,
  placement matrices +0xD00 stride 0x40, hinge +0xADC/+0xAFC; light table
  +0x1664/+0x16AC stride 0x30 (+0x1684/+0x16B4 = type-8 view); physics
  extents +0xE80/center +0xE90 (live veh +0x1D0/+0x1E0); tex byte rec+0x1A
  → ctx+0x334 five-slot table.
- `enviro.dat`: +0x00 fog colour (×127.5), light RGB 0x60..0x6B, light dir
  +0x80, car env sphere map +0xA0, sky gradients/clouds/envmapsun sheets.
- `static.dat`: backdrop LOD table + the prop model/instance tables (§9).
- `Gamedata.bgd`: TDESC mode blocks (§8), SPATIAL records (start grid
  @0xA000 for US_C3_V1, race line @0xC0930), nav/route lines
  (extract_bgd_paths).
- `Global.txd`: HUD art, VehicleUnderside, Aftertouch, glass tiers.
- `tlist.bin`: track table (US_C3 = Silver Lake, AS_C1 = Golden City,
  US_C2 = Chicago).
- `Tracks/crash1..20.rws`: aGenCrashNN + zSloCrashNN beds (30 s stereo
  32 kHz).
- vdb.xml: per-car physics overrides (extract_car_vdb).
- Base-40 packed ids throughout (vehicle ids, ticker keys — the ticker's
  alphabet is the SFX charmap rotated by two).

## 14. Coordinate conventions (harness)

- Harness world = game world with Z negated (RE_NOTES 12); physics runs in
  GAME space (the cross-product algebra is chirality-bound); display
  un-mirrors via projection `glScalef(-1,1,1)` + CCW scene winding.
- Pseudo-vectors (ω, angmom) map (x,y,z) → (−x,−y,z) between spaces.
- V origin rule: v = 0 is texel row 0 for ALL textures, no flips anywhere.
- Traffic spawn dir[3] = instance matrix Z column = BACKWARD axis.

## 15. Open [?] ledger (highlights)

- Blur strength producer `s` (blurState+0x54) — no writer found.
- Sun-glow magnitude (no toward-sun reference capture) — B3_SKY_NOGLOW A/B.
- DAT_004D6C00 (car env stage-1 binding) — writer proven absent from image.
- ~~Retail crash early-releaser `racecar+0x19BE`~~ **CLOSED 2026-08-14:
  there is none.** `+0x19BE` is the racecar ASSET-LOAD-COMPLETE latch --
  one write, immediate `1`, @0x0018D740 at the tail of the spawn state
  machine `FUN_0018D0E0`, never cleared anywhere, read only @0x00119C09
  and @0x0011FEC1 to gate the post-load place-on-track virtual.  The
  believed 5-second deadline `racecar+0x240C` (`clock + 5.0 [0x003B1694]`
  @0x00198F75) has ZERO readers in any of the image's 14 executable
  PT_LOAD segments, and crash EXIT (`racecar+0x18FA = 0`, vtable slot
  +0x10) is dispatched from only two non-timer sites (@0x0011FEDE,
  @0x0018D91E).  No timed crash releaser exists in this image; the
  harness bound stays GLUE for a proven reason.  See
  `docs/PHYSICS_GLUE_LEDGER.md` PH-11.
- `racer+0x23C0/0x23C4/0x134C` — no writer in this image (AI plan inputs).
- Particle per-system distance fade (system+0xC0..0xCC).
- Panel loose-pose hinge sidecar; mask-bit1 record shader (DAT_004D6558).
- Prop table IMUL class,0x70 out-of-range quirk on some tracks.
- Aftertouch cursor ValueDB UV endpoints; wheel+0x78 mode index; kerb +
  scenery-whoosh (STATICPASS) trigger conditions.
- Trailer/tow attach points: RECOVERED 2026-08-14 -- model+0x169C/0x16A0/
  0x16A4/0x16A8 (`ptr -> vec4[]`, u8 counts at +0x16BA..+0x16BD), read by
  `FUN_00120F30` @0x00121075/0x00121082/0x0012109E (the two-rigid-body tow
  constraint) and `FUN_001A7210`.  No writer in `.text`: read-only model
  payload.  `FUN_00120BA0` @0x00120C08 also copies a second, unrecorded
  0x40-stride matrix table `model+0xD00` (count `model+0x0C`) into
  `vehicle+0x1560`.  See `docs/PHYSICS_GLUE_LEDGER.md` PH-15.
- `docs/PHYSICS_GLUE_LEDGER.md` tracks the physics stand-ins
  (recover-or-prove-unrecoverable; wave 3 closed the substep relocation,
  PH-06/08/11/15/22/27 and PH-09's wall arm).
