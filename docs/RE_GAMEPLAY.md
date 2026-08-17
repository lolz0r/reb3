# Burnout 3 gameplay rules — scoring, boost, takedowns, out-of-control

Recovered 2026-08-10 from the analyzed `burnout3.elf` (addresses are the
corrected VAs; `.text` = flat + 0x10000). Everything marked **[C]** is either
execution-verified by a green differential case in `tools/validate_gameplay.py`
(**44/44**) or is a registrar/data fact reproduced by two independent
derivations. **[S]** = read from the decompile, self-consistent, but with no
green case — documented here and deliberately **not** integrated into the
harness. **[?]** = open.

Verification pattern is `tools/validate_port.py`'s: seed the function's real
environment under Unicorn (`tools/emulate_vehicle.py`), execute the actual
instructions, assert a pure-Python mirror matches the post-state.

---

## 1. The parameter registrars [C]

`"Score/Burnout Points"` string at **0x003B09F8** (the 0x003A1238 figure in
HANDOFF §2 is the flat-load artifact; the true value at 0x003A1238 is 90.0).
All 31 xrefs land in **FUN_00190430** — the Score registrar. It uses the same
param-registry machinery as the physics registrars (`FUN_001AEDB0` allocates a
param record; virtual registry calls at `DAT_004A1E94+0x10`), but unlike
`FUN_00132D10` it binds each parameter to a **global**, not a struct offset:

```
record+0x00 = &storage global      record+0x1E = type (2=float, 1=int/u32)
record+0x1C = element count (0 = scalar, N = array)
cfg path    = "../Export/ValueDB/Score.cfg"
```

* **FUN_00190430** registers **79** `Score/*` params (storage 0x3F73A0..0x3F75DC,
  plus one BSS straggler at 0x4A1E2C for "No Mercy BP").
* **FUN_0017A0F0** registers the **5** `Boost Bar` params (0x3F72DC..0x3F7310).
* **FUN_00192320 / FUN_0017A320** are the matching unregister walkers.

**VDB resolution [C]:** the registration key is the same pipeline recovered for
the car physics (RE_NOTES §10): `gt_hash("<param name><group>/../Export/
ValueDB/Score.cfg")` with the table-CRC at 0x001AF250 (SAR shift, no final
inversion). Every key resolves in the retail `Data/vdb.xml`; the cfg-path hash
appears in its fileDef section. **Array params store a file OFFSET as their
VDB rawValue** — the elements live in the pool between the default records and
the fileDefs (this matches the bdtool `VDBParser.cs` comment, and is
cross-confirmed by the tier-consistency check below).

## 2. The recovered parameter tables

Values: `default` = compiled-in (.data image; what pre-init emulation sees),
`vdb` = the tuned retail value the shipped game plays with. Types: f=float,
i=int (raw u32). Full extraction script: the registrar walk in this session's
notes; spot re-derivation: `tools/validate_gameplay.py` §9 (8 data checks).

### "Boost Bar" (FUN_0017A0F0) [C]

| param | addr | default | vdb |
|---|---|---|---|
| Minimum Boost Time (seconds) | 0x3F72DC | 1.0 | **0.5** |
| Minimum Boost Recovery Time (seconds) | 0x3F72E0 | 0.5 | **0.1** |
| Boost Bar sizes (Boost Units) [4] | 0x3F72E4 | 240,360,600,720 | **240,360,540,720** |
| Boost Earning Multiplier [4] | 0x3F72F4 | 1,1.5,2.5,3 | **0.667,1,1.5,2** |
| Boost Rate (Boost Units per Second) [4] | 0x3F7304 | 36,36,36,36 | **36,36,36,36** |

### "Score/Boost/*" — boost-unit awards (FUN_00190430) [C]

| group | param | addr | default | vdb |
|---|---|---|---|---|
| Air | Minimum Air Distance (Metres) | 0x3F73A0 | 5 | 5 |
| Air | Boost value per Air metre | 0x3F73A4 | 2 | 0.3 |
| Oncoming | Minimum Oncoming Speed (MPH) | 0x3F73A8 | 30 | 50 |
| Oncoming | Minimum Oncoming Distance (Metres) | 0x3F73AC | 10 | 40 |
| Oncoming | Boost value per Oncoming metre | 0x3F73B0 | 0.3 | 0.15 |
| Drift | Minimum Drift Speed (MPH) | 0x3F73B4 | 30 | 90 |
| Drift | Minimum Drift Distance (Metres) | 0x3F73B8 | 10 | 20 |
| Drift | Boost value per Drift metre | 0x3F73BC | 1 | 0.1 |
| Near Miss | Near Miss Distance (Metres) | 0x3F73C0 | 2 | 1.8 |
| Near Miss | Minimum Near Miss Speed (MPH) | 0x3F73C4 | 30 | 60 |
| Near Miss | Minimum Near Miss Chain Speed (MPH) | 0x3F73C8 | 30 | 60 |
| Near Miss | Boost value per Near Miss | 0x3F73CC | 60 | 15 |
| Near Miss | Near Miss Chain Time (Seconds) | 0x3F73D0 | 5 | 2.5 |
| Crash Escape | Crash Escape Radius | 0x3F73D4 | 8 | 30 |
| Crash Escape | Time Between Crashing Near Misses (s) | 0x3F73D8 | 1 | 1 |
| Crash Escape | Minimum Time Clear After Crash Escape | 0x3F73DC | 3 | 4 |
| Crash Escape | Minimum Crash Escape Speed (MPH) | 0x3F73E0 | 70 | 10 |
| Crash Escape | Boost value per Crash Escape | 0x3F73E4 | 120 | 80 |
| Crashing | Ratio of Boost Lost After Crash | 0x3F73E8 | 0.5 | 0 |
| Aggressive | Boost value lost or gained per Slam | 0x3F73EC | 360 | 180 |
| Aggressive | Min Contact Time For Rubbing (Seconds) | 0x3F73F0 | 0.3 | 0.3 |
| Aggressive | Boost value for rubbing (per second) | 0x3F73F4 | 15 | 45 |
| Aggressive | Trading Paint Boost value | 0x3F742C | 3 | 90 |
| Boost Start | Boost Start Boost value | 0x3F73F8 | 50 | 50 |
| Takedowns | Takedown Boost value | 0x3F73FC | 720 | **360** |
| Takedowns | Race Car Clear Wait Time (Seconds) | 0x3F7400 | 1 | **0.5** |
| Takedowns | Maximum Crash Wait Time (Seconds) | 0x3F7404 | 1 | **2** |
| Takedowns | Maximum Crash Wait Time - No Slam (s) | 0x3F7408 | 0.5 | 1 |
| Takedowns | Min Collide Time to enable TD - No Slam | 0x3F740C | 0.5 | 0.1 |
| Takedowns | Double takedown window (Seconds) | 0x3F7410 | 1 | 1 |
| Takedowns | Takedown spree window (Seconds) | 0x3F7414 | 30 | 30 |
| Takedowns | Double revenge window (Seconds) | 0x3F7418 | 60 | 60 |
| Props | Prop Hit Chain time (Seconds) | 0x3F741C | 2 | 2 |
| Props | Prop Hit Boost value | 0x3F7420 | 3 | 0 |
| Lap | Perfect Lap Boost value | 0x3F7424 | 120 | 120 |
| Lap | Burnout Lap Boost value | 0x3F7428 | 240 | 240 |

### "Score/Burnout Points" — BP awards, all ints (FUN_00190430) [C]

| param | addr | default | vdb |
|---|---|---|---|
| Prop Hit BP | 0x3F7430 | 10 | 0 |
| Rubbing BP | 0x3F7434 | 5 | 5 |
| Tradin' Paint BP | 0x3F7438 | 25 | 15 |
| Boost Start BP | 0x3F743C | 50 | 50 |
| Crash Escape BP | 0x3F7440 | 100 | 25 |
| Burning Slam Extra BP | 0x3F7444 | 15 | 15 |
| Slam Type BP [4] | 0x3F7448 | 20,20,20,20 | 50,20,30,15 |
| Super Slam Type BP [4] | 0x3F7458 | 30,30,30,30 | 80,30,30,30 |
| Perfect Lap BP | 0x3F7468 | 500 | 200 |
| **Takedown BP** | 0x3F746C | 1000 | **150** |
| Revenge Takedown Bonus BP | 0x3F7470 | 350 | 350 |
| Psyche Out BP | 0x3F7474 | 150 | 150 |
| Aftertouch Takedown BP | 0x3F7478 | 250 | **1250** |
| Burnout Lap BP | 0x3F747C | 1000 | 500 |
| Near Miss BP (per chain link) | 0x3F7480 | 20 | 0 |
| Leading Race At End Of Lap BP | 0x3F7484 | 250 | 100 |
| Denied (You Were Lucky) BP | 0x3F7488 | 15 | 15 |
| Denied (Takedown Denied) BP | 0x3F748C | 10 | 10 |
| Air Category BP [4] | 0x3F7490 | 100,250,500,1000 | 0,10,50,100 |
| Oncoming Category BP [4] | 0x3F74A0 | 100,250,500,1000 | 0,20,75,150 |
| Drift Category BP [4] | 0x3F74B0 | 100,250,500,1000 | 0,20,50,100 |
| Tailgate Category BP [3] | 0x3F74C0 | 5,10,15 | 5,10,15 |
| Rubbing Category BP [3] | 0x3F74CC | 5,10,15 | 5,10,15 |
| Grinding Category BP [3] | 0x3F74D8 | 5,10,15 | 5,10,15 |
| Near Miss Category BP [4] | 0x3F74E4 | 5,10,15,25 | (no VDB entry) |
| Take Race Position BP [5] | 0x3F74F4 | 500,400,300,200,100 | 300,200,100,50,25 |
| Double Takedown BP [4] | 0x3F7508 | 300,500,750,1000 | 300,500,750,1000 |
| Takedown Spree BP [4] | 0x3F7518 | 300,500,750,1000 | 300,500,750,1000 |
| Double Revenge BP [2] | 0x3F7528 | 300,500 | 300,500 |
| Road Rage Takedown BP [8] | 0x3F7530 | 100,250,…,2000 | 100,250,500,1000,1050,1150,1300,1500 |
| No Mercy BP | 0x4A1E2C (BSS) | 0 | 30 |

### "Score/BP Categories" minima + the rest [C]

| param | addr | default | vdb |
|---|---|---|---|
| Air Category Minima (Metres) [4] | 0x3F7550 | 15,30,60,90 | 1115,10,20,35 |
| Oncoming Category Minima (Metres) [4] | 0x3F7560 | 50,100,200,300 | 9150,500,900,1400 |
| Drift Category Minima (Metres) [4] | 0x3F7570 | 45,90,180,270 | 9980,100,150,250 |
| Tailgate Category Minima (Seconds) [3] | 0x3F7580 | 1,5,10 | 1,3,5.5 |
| Near Miss Category Minima (Counts) [4] | 0x3F75A4 | 999,2,5,9 | (no VDB entry) |
| Tailgating: Max distance behind (Metres) | 0x3F75B4 | 15 | 18 |
| Tailgating: psyche-out min distance | 0x3F75B8 | 7 | 7 |
| Tailgating: Cone angle (Degrees) | 0x3F75BC | 15 | 15 |
| Tailgating: Extra boost per second | 0x3F75C0 | 15 | 30 |
| Tailgating: Minimum time (Seconds) | 0x3F75C4 | 0.5 | 0.5 |
| Grinding: Minimum time before scoring | 0x3F75C8 | 3 | 0.1 |
| Grinding: Extra boost per second | 0x3F75CC | 15 | 30 |
| Distance Travelled Medals (Miles) [3] | 0x3F75D0 | 4,7,12 | 4,7,12 |

Alignment cross-check for the array pool: the tuned tier-1 minima are huge
sentinels (1115/9150/9980) exactly where the tuned tier-1 category BP is 0 —
i.e. Criterion disabled the lowest medal tier for air/oncoming/drift in the
retail tune. Three independent groups agree, so the pool decode is not an
off-by-one.

---

## 3. Object model [C]

The per-frame scoring update is **FUN_001935F0**, called per car from
FUN_00026D30 with `EAX = racecar + 0x10D0` (`MOV EAX,[ESI*4+0x73A1A8]; ADD
EAX,0x10D0` at 0x27130) — so the **score object is embedded in the racecar at
+0x10D0**, and `DAT_0073A1A8[slot]` is the racecar table (`DAT_0073A19C` =
count). `DAT_0064B38C` (stride 0x30) maps slot → physics vehicle. The physics
vehicle's `v+0x13F4` points at its racecar; `racecar+0x1198` and `+0x133C` are
self/score back-pointers (score+0xC8 / score+0x26C both hold the racecar).

Key absolute offsets (score-relative in parens):

| racecar offset | meaning |
|---|---|
| +0x10DC (sc+0x0C) | race clock — the known "boost clock" anchor |
| +0x111C (sc+0x4C) | **Burnout Points total** |
| +0x117C (sc+0xAC) | BP subtotal (takedown side) |
| +0x1188 (sc+0xB8) | BP subtotal (event side) |
| +0x1174/0x1588/0x158C | slams made count / slam BP total / last slam time |
| +0x1178 (sc+0xA8) | signature-takedown bitmask |
| +0x118C/0x1190/0x1194 | aftertouch / takedown counters |
| **+0x119C (sc+0xCC)** | **boost record** (layout below) |
| +0x1598 | "slammed at" clock — the OOC trigger timestamp |
| +0x1590/0x1594/0x159C | times-slammed count / BP / last slam type |
| +0x15A8[6]/+0x15C0[6]/+0x15C6[6] | per-slot crash-attribution stamps (time / aftertouch flag / slam flag) |
| +0x15D6/+0x15D7 | takedown credited flags (dedup) |
| +0x15DC / +0x15D8 | credited attacker ptr / victim back-ptr |
| +0x1654 | last-rubbed time (AI reaction) |
| +0x1684 | slot of the car that last slammed this one |
| +0x1689[6] / +0x168F | "was taken down by slot i" flags / revenge marker |
| +0x1690 | second OOC timestamp (wall/spin event; 0.6x windows) |
| +0x16BC / +0x16C0 | last aggressor ptr / time (drives OOC + rubbing gates) |
| +0x18FA / +0x18FB | crashed / crashed-alt flags |
| +0x1920 | car class: 0=player-ish, 1=AI racer, 2=other |
| +0x19BC | grid slot byte |
| +0x2440 | ptr to physics vehicle |
| +0x240C | AI recovery: set to clock+5.0 on being taken down |

**Boost record** (racecar+0x119C; constructor **FUN_0017A3C0** proves every
field): `+0x00` self-ptr, `+0x24` boost start clock (**= racecar+0x11C0**, the
transmission's elapsed-boost anchor), `+0x28` boost end clock, `+0x30` bar
tier, `+0x34` bar size, `+0x38` meter, `+0x3C` lifetime earned, `+0x40` drain
rate, `+0x44` min units, `+0x48/+0x4C` earning multipliers (base/bonus),
`+0x50/+0x51` peg-full flags, `+0x52` boosting, `+0x53` fixed-duration burn,
`+0x55` ramp-done (**= racecar+0x11F1**, read by the ported transmission),
`+0x56` set when +0x55 while boosting. This closes the loop on the transmission
port's three anchors: elapsed boost = `[+0x10DC] - [+0x11C0]`, ramp byte =
`[+0x11F1]`.

Score-object records: category events use a common 0x1C-byte record handled by
**FUN_00192D20** — `{+0 value, +4 clock, +8 prev value, +C thresholds ptr,
+0x11 tier s8, +0x12 prev tier, +0x13 count, +0x18 clock-source}`. Instances:
air +0x358, oncoming +0x374, drift +0x390, boost-time +0x3AC, near-miss chain
+0x418, rubbing +0x564, tailgate +0x5A4 region, grinding +0x5C4.

---

## 4. Boost bar rules [C — validate_gameplay 4+7+5+2 cases]

* **Award** (`FUN_0017A530`, ESI=record, arg=units; inlined at ~10 sites):
  `amount = units × (mult_base + mult_bonus) [× global scale, vtable +0xAC]`,
  `earned += amount`, `meter = min(meter + amount, bar size)`. Skipped in
  crash-party modes (FUN_00017310: game-context mode 6 / sub-mode 3/4/5).
* **Engage** (`FUN_0017A5B0`, EAX=physics vehicle, ESI=record): starts iff not
  boosting AND `meter >= min_units` (= MinBoostTime × rate at reset) AND
  `end_time < 0 || end_time + MinRecovery <= clock`. Sets boosting=1,
  fixed=0, start=clock (+ FX). Callers: AI driver FUN_00105340, player input
  stage 0x00118114.
* **Drain/stop** (`FUN_0017A480` head, EAX=racecar, ECX=record): while
  boosting `meter -= rate × dt` (dt = DAT_0060EA1C); flags +0x50/+0x51 peg
  meter to bar size instead (crash modes). Stop when meter ≤ 0 (meter := 0),
  or — with +0x53 set — when `clock >= start + MinBoostTime` (the AI's
  committed short burn; FUN_00105340 sets +0x53 while boosting with ramp not
  done). Stop clears +0x52/+0x53 and writes end=clock. The function's tail is
  camera/FOV + backfire FX (speed thresholds 100→165 mph) — mapped, not core.
* **Takedown upgrade** (`FUN_000273F0`, args event/racecar): if tier < 3:
  `tier += 1`, size/rate/mult reloaded from the Boost Bar arrays, min units
  recomputed; then award TakedownBoostValue units through the standard path.
  Verified end-to-end including the tier-3 saturation case.
* **Full-bar special [S]** (`FUN_00029DF0` variant): at tier 3 it sets record
  +0x50 (meter pegged full — temporary infinite boost) once, announces via
  FUN_00199D60, and awards a whole bar. Not integrated (no green case; +0x50
  lifetime unknown — cleared at least by the reset).

Tuned numbers: full 720-unit bar = 20 s of boost; one 240-unit base bar =
6.67 s; a takedown = +360 units × multiplier and a bar-size step
240→360→540→720 with earning multiplier 0.667→1→1.5→2.

## 5. Scoring events

Per-frame update FUN_001935F0 calls, in order: FUN_00199080 (window expiry
[C]), FUN_00194A80 (rubbing), FUN_00194EE0 (near miss), FUN_00195490 (crash
escape), FUN_001959A0 (tailgating), FUN_00195CE0 (denied), FUN_00197260
(grinding), FUN_00197180 (position BP), FUN_00196940/BE0/E10 (air/oncoming/
drift), FUN_00197040 (takedown claim scan), then FUN_0017A480 (boost update).
A crash (racecar+0x18FA) resets every chain/record.

* **Air/oncoming/drift** (FUN_00196940/FUN_00196BE0/FUN_00196E10) share one
  template [C, air landing path has a 14-assert green case]: while the state
  flag holds (airborne racecar+0x10C0 / oncoming +0x18FC + speed ≥ min /
  drift +0x10C2 + speed ≥ min), FUN_00192D20 accumulates distance and tracks
  the category tier against the minima table; past the minimum distance,
  boost accrues per metre. On event end: `CategoryBP[tier]` → BP totals
  (+0x4C/+0xB8), best-event record (ids 0x71..0x76), stats (total/best/count),
  record reset (tier := -1). Speed source: `racecar+0x64 × 2.2369363` (the
  TRUE mph constant, not the 2.2374146 physics one).
* **Near miss** (FUN_00194EE0) [S struct, C params]: 8 tracked slots
  (score+0x3E8 ids, +0x3F0 timestamps, +0x410 armed flags) filled from the
  proximity table `DAT_00649A8E`/count `DAT_00649B3C` via FUN_00195DD0
  (distance < Near Miss Distance etc.) at speed ≥ min; a pass ages out ⇒
  award: chain count +0x3D0 and total +0x3CC increment, boost += Boost value
  per Near Miss, **BP += chain_count × NearMissBP**; chain expires after
  Chain Time or below chain speed ⇒ NearMissCategoryBP[tier by count].
* **Rubbing** (FUN_00194A80) [S logic, C params]: per-opponent contact timers
  (+0x528) accumulate while touching (flags +0x55E set by the collision
  side); zeroed if either car is in a recent slam/crash interaction (the
  takedown paths own those); at ≥ Min Contact Time: rubbing boost = dt × rate
  every frame + Rubbing Category BP on end; marks the opponent's +0x1654.
* **Tailgating** (FUN_001959A0) [S logic, C params]: behind a car within 18 m
  inside a 15° cone for ≥ 0.5 s ⇒ dt × 30 units/s and the tailgate category
  record; the psyche-out distance (7 m) feeds Psyche Out BP via the takedown
  module.
* **Grinding** (FUN_00197260) [S logic, C params]: wall-scrape state ⇒
  dt × 30 units/s + Grinding Category BP via FUN_0019A050.
* **Position** (FUN_00197180) [S]: each place gained ⇒
  `TakeRacePositionBP[new position - 1]` (pos 1..5) + HUD event.
* **Crash escape / denied** (FUN_00195490 / FUN_00195CE0) [S logic, C params]:
  escaping crashing cars within the radius at ≥ min speed ⇒ 80 units + Crash
  Escape BP; surviving a slam for Maximum Crash Wait Time ⇒ Denied BPs
  (15 + 10) into +0x4C/+0xB0.
* **Category tiers** (FUN_00192D20) [C — 5 cases]: store value/prev/clock;
  if `tier < count-1`, scan minima top-down, tier := highest index with
  `minima[i] <= value`; never downgrades within an event.

## 6. Takedowns [C core — 3 commit cases + 1 expiry + 2 upgrade]

Chain of custody for a takedown:

1. **Slam** (`FUN_001989A0(victim_pv, attacker_pv, impact, type)`) [S formula,
   C fields]: attacker gets Slam/SuperSlam Type BP (+ Burning Slam Extra BP
   15 if boosting at impact, read from attacker's record +0x52 via +0x133C)
   and gains boost `BarSizes[0]/(tier+1) × multiplier`; the victim **loses**
   the same base quantity (unless peg flags), gets `+0x1598 = clock` (slam
   timestamp), `+0x16BC/+0x16C0` = attacker/time, slammed counters, and — if
   its meter hits 0 while boosting un-ramped — the forced min-burn stop flag.
   The attacker/victim both record each other as last aggressive contact.
2. **Attribution stamps** (`FUN_00197430`, run when a car crashes): every
   opponent within 160 m whose contact timer exceeds Min Collide Time (or the
   one that slammed me within **Maximum Crash Wait Time**, +0x5EC/+0x5F0)
   gets `their +0x15A8[my_slot] = clock` (+0x15C0 aftertouch flag, +0x15C6
   slam flag) — "car my_slot went down near/because of you".
3. **Claim scan** (`FUN_00197040`, per frame): a pending claim at
   score+0x4D8[slot] is committed only after **Race Car Clear Wait Time**
   passes without the attacker crashing (+0x18FA gate; a force flag +0x4F0
   commits immediately).
4. **Commit** (`FUN_00198E60`, ESI=score, EDI=victim racecar) [C]: dedup on
   victim+0x15D6; victim+0x15DC := attacker; takedown count +0x68 += 1;
   +0x500 := clock; +0x4D4 := victim; **revenge bookkeeping**: if
   score+0x5B9[victim_slot] (they had taken me down) → clear it + victim
   +0x168F := 1, else victim+0x1689[my_slot] := 1; AI victims get
   +0x240C = clock + 5.0 (recovery). Then the BP chain:
5. **BP award** (`FUN_001994D0`) [C for the executed path]: base Takedown BP;
   **double takedown** — takedowns within a rolling 1 s window (+0x110/+0x114)
   escalate through DoubleTakedownBP; **spree** — same over 30 s
   (+0x118/+0x11C) through TakedownSpreeBP; **revenge** +350 (flag from
   racecar+0x1689[victim_slot]); aftertouch variant pays Aftertouch BP and
   its own counters; signature-takedown table at DAT_003A4FF4/0x3A5000 (per
   track+victim, single-player only) [S]. BP lands in racecar+0x111C/+0x117C.
   Emulated end-to-end: 1000 BP (compiled default) + windows set to
   clock+1/clock+30 — exactly the recovered constants.
6. **Window expiry** (`FUN_00199080`, per frame, ECX=score+0x124) [C]: an
   expired double/spree window resets its count to 0 and the window to -1.

## 7. Out-of-control + steer-away [C — 5 + 3 cases]

Config params (registrar FUN_00132D10, config +0x1BC/+0x1C0/+0x1C4 → live
v+0x13E0/+0x13E4/+0x13E8 via FUN_00134710): **Steer Away Time 0.3 s**,
**Total Out-Of-Control Time 1.0 s**, **Steering Max Angle 24°**.

* **Steer-away envelope** (`FUN_0011ECF0` head) [C]: with the racecar slammed
  at `t` (+0x1598) and clock `c`: while `c <= t + SteerAway` the live steering
  response (v+0x137C) is forced to the max angle (v+0x13E8); until
  `t + TotalOOC` it lerps back to the config base
  (`base + (target-base) × (1 - (c-t-T_sa)/(T_ooc-T_sa))`); after that the
  normal config value returns. The second event type (+0x1690, wall/spin)
  uses 0.6× windows and a 0.8× decay target (full max angle still applies in
  phase 1). Verified in 5 cases through the full real function.
* **AI authority** (`FUN_00105340`) [C]: while BOTH `+0x1598` and the
  aggressor-contact time `+0x16C0` are within Total-OOC-Time and the recorded
  aggressor (+0x16BC) is a class-0 car, the AI's steering authority v+0x1534
  is written to **0.1** (mode +0x23F8==2) or 0.05 (other modes) — the victim
  cannot steer away. The same function is the AI boost driver: it calls the
  engage gate, sets the committed-burn flag while ramping, and writes the
  boost input bit `v+0x13FC := 4` from the record's boosting flag —
  confirming the transmission-input anchor.

## 8. Open items — [S]/[?]

* **Slam boost transfer**: exact executed quantities of FUN_001989A0
  (`BarSizes[0]/(tier+1)` vs the stored 180-unit param; super-slam table
  selection reads the VICTIM's crash/speed state) — needs a differential case
  with the FX call environment; NOT integrated.
* **+0x11F1 writer** (boost ramp-done): consumed by the transmission and by
  FUN_0017A480 (+0x56 echo), set somewhere in the input stage (0x118xxx)
  — [?].
* **Burnout chain BP table** at DAT_003A4A00 (boost-time record +0x3AC,
  active > 120 mph while `score+0x11E` flag) — compiled-in, not
  VDB-registered; tiers by continuous boost time [S].
* **FUN_0019A050** combo/chain helper (grinding/rubbing/tailgate BP with its
  own escalation records at +0xD4..+0xF4) [S].
* **Denied awards, crash escape, prop hits, distance medals**: parameters [C],
  logic read but not execution-verified [S].
* **Signature/aftertouch takedown tables** (DAT_003A4FF4, DAT_003A4BC8) [S].
* **Score/BP Categories "No Mercy BP"** lives in BSS (0x4A1E2C), so its
  compiled default is 0 and only the VDB gives it a value (30) [C oddity].

## 9. Verification inventory (tools/validate_gameplay.py — 44/44)

| section | function | cases | asserts |
|---|---|---|---|
| boost award | FUN_0017A530 | 4 | meter/earned incl. clamp + multiplier |
| meter update | FUN_0017A480 | 7 | drain, empty-stop, peg, fixed burn ×2, idle ×2 |
| engage | FUN_0017A5B0 | 5 | gate combinations + start-time write |
| tier tracker | FUN_00192D20 | 5 | tier scan, no-downgrade, value/prev/clock |
| air landing | FUN_00196940 | 1 | 14 fields: BP, stats, event record, resets |
| takedown commit | FUN_00198E60(+FUN_001994D0) | 3 | dedup, counters, revenge flags, BP=1000, windows |
| window expiry | FUN_00199080 | 1 | expired resets, open kept |
| bar upgrade | FUN_000273F0 | 2 | tier/size/rate/mult/min units + award |
| steer-away | FUN_0011ECF0 | 5 | +0x137C envelope, both event types |
| out-of-control | FUN_00105340 | 3 | +0x1534 authority in/out of window |
| parameters | registrar+vdb.xml | 8 | tuned values incl. arrays via pool offsets |

Emulation notes: the game-context virtuals (`DAT_004D5370 → +0x1B8 → vtable`)
are faked with 3-instruction stubs; the +0x5C hook takes two stack args so its
stub must `RET 8` — a plain RET leaks the args and the caller's final RET then
"returns" into a data pointer (this exact failure produced a wild-execution
artifact before it was fixed; the same class of bug as validate_port's
convention gotchas). `FUN_00017310` resolves its context via
`MOV EAX,0x4A71A0; MOV EAX,[EAX+0x2E1D0]` = DAT_004D5370.

## 10. Harness integration (src/burnout3_full.c + src/burnout3_gameplay.h)

Integrated (all with green cases): the boost bar record and its
engage/drain/award/upgrade rules driving the ported transmission's boost
input; takedown commit bookkeeping, revenge flags, double/spree BP windows +
expiry; the steer-away response envelope and the AI OOC authority factor 0.1;
race Car Clear Wait deferral and Maximum Crash Wait attribution windows; HUD
boost bar (grows with tier, 240-unit notches) + BP + takedown readouts.
Marked GLUE: what counts as a slam/crash in the harness's sphere-contact
world, the 8 s post-recovery immunity, and AI holding boost on straights.
NOT integrated (no green case): slam boost transfer, crash escape, denied,
prop/lap/position BP, the tier-3 peg-full special.
