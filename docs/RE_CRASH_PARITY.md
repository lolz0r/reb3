# Crash handling — the complete decision surface, and its parity status

Every way a car can be put into the crashed state in retail, what gates it,
and where the harness stands on each. Provenance is `[C]` confirmed from the
image (address given), `[S]` strong inference with the evidence stated, `[?]`
open. Anything marked **Unicorn** was proved by *executing the retail function*
out of `build/burnout3.elf` via `tools/emulate_carcol.py`, not by reading the
port.

---

## 0. The convergence point

There is exactly **one** way a car's crashed byte `+0x210` is set:
`FUN_00125B30` @0x00125b4b, and its only caller is **`FUN_0011FC60`**
(retail's enter-crashed-state, which guards on `+0x210 == 0` and then stamps
`+0x218`). `[C]`

`FUN_0011FC60` is reached two ways:

* the wrapper **@0x00118E30** — saves `+0xCC4` into `+0x3A80`, then calls it.
  Its *only* reference is the racecar vtable slot 0x003B1168. `[C]`
* the single indirect `call [reg+0x5c]` in the whole image, **@0x00198F25** in
  `FUN_00198E60`, dispatching on the global `DAT_004d5370+0x1b8` — the crash
  presentation path (it also stamps recovery, `+0x240C = +0x10DC + 5.0`). `[C]`

**Vtable map.** The racecar and traffic vtables carry an identical interface
block `{FUN_0011FC60, FUN_0011FE90, FUN_0011BE40, FUN_001A98D0}` — at
0x003B1140 in the racecar table, and at **0x003B1248 in the traffic table**
(base 0x003B11EC, i.e. slot +0x5C). So a traffic body is crashable through the
same entry a racecar uses. `[C]`

So every crash decision below ends in the same place; what differs is the gate.

---

## 1. Chassis vs world (the wall crash) — `FUN_0011AEF0`

| gate | address | status |
|---|---|---|
| bit-0 bail | `TEST byte [EDI+0x1353], 5` @0x0011AF0C | ported, tested |
| **bit-3 crash veto** | `TEST byte [EDI+0x1353], 8` @0x0011B94D | ported, tested |
| dv / head-on thresholds | the gate body | ported, tested |

The veto is written by `FUN_00105BD0`: `OR byte [ESI+0x1353], 0x18`
@0x00105F95 when the authority ladder's `crash_ok` byte is 0, i.e. when the
car is outside `0.4 * base` = 79.06 m of the viewed car. Missing this was a
real bug — it made AI cars 33x easier to wreck than the player (fixed in
be471b4). `validate_port` executes the retail stream
0x0011C0A0..0x0011C16C with the byte set and clear.

**Contact source caveat (GLUE).** Retail's gate is fed by retail's contact
collection; this harness feeds it from a substepped sphere sweep
(`B3_WALL_SWEEP=0` disables). Deleting that sweep as "invented" once made the
game uncrashable — `crash_fired` stayed 0 across 580 wall contacts — because
nothing else *detects* the contact. See fcff57f.

---

## 2. Car vs car, both alive — `FUN_001121F0`

```
XMM2 = |dot(vrel, n)| * [0x0038994c]        ; 2.236936 = m/s -> mph
       COMISS XMM2, [0x003ebe4c]            ; 150.0        @0x0011281e
       JBE  <skip the crash>                ;              @0x0011283e
       byte [ECX + EAX + 0x39ae50]          ; class pair   @0x0011285f
```

**Unicorn, nose-to-nose:** 120+30 = 150 mph closing does **not** crash;
130+30 = 160 mph **does** — and the result is *identical* for `B.type` 0, 1, 2
and 3. Retail gives a traffic body **no lower bar** than a racer. `[C]`

`DAT_0039AE50` (7x7, dumped from the image):

```
        clsB: 0 1 2 3 4 5 6
  clsA 0      1 1 1 1 1 1 0
  clsA 1      0 0 0 0 0 0 0
  clsA 2      1 0 0 1 0 0 0
  clsA 3..6   all 0
```

`FUN_0010FBC0` maps type 0 (racer) **and** type 2 (traffic) to interaction
class 0, so racer-vs-traffic is `table[0][0] = 1`. The port's hardcoded
"both directions are 1" is therefore correct for every pair this game puts on
the road. `[C]`

Locked by `validate_carcol`: `headon-under-150` (vn 146.5 -> no crash),
`headon-over-150` (vn 156.3 -> crash), `headon-traffic-type2` (same bar).

---

## 3. Car vs car, one already wrecked — `FUN_00113960`

The cheap path, and the one the real game runs on.

| gate | value | status |
|---|---|---|
| impact threshold | `> 5000` `[0x003EBE50]` | ported, tested |
| **traffic threshold** | `> 2500` `[0x003EBE54]` when an un-crashed non-traffic car hits a **crashed traffic** car | ported, tested |
| recent-slam window | `< 1.5 s` `[0x003EBE7C]` suppresses | **ported ae7d346** |
| type-4 bit-4 veto | `cmp cl,4` + `TEST [+0x1353], 0x10` @0x001141C2 | audited — type 4 only, absent from this game |
| type-3 `+0x174 & 8` | @0x001141AC suppresses | audited — see §5 |

**Unicorn discriminator** (impact band 2578..4727, i.e. 12–22 mph closing):

```
closing 15 mph, impact 3223   B.type 0 -> crash_a = 0
                              B.type 2 -> crash_a = 1
```

So once a traffic car is wrecked, retail wrecks **you** from ~15 mph of
closing — 190 mph cheaper than §2. That cascade is what makes oncoming
traffic lethal in the real game.

The 1.5 s window carries retail's own guard: it is **skipped** when a
non-traffic car hits a traffic car (`A->type == 2 || B->type != 2`), so it
never gates the cascade — only racer-vs-racer wreck hits.

---

## 4. Car vs object — `FUN_00112E70`

`closing_mph > authority * scale`, with the same ladder veto as §1: the
`|= 0x18` store sets bit 4 alongside bit 3, so `crash_ok` covers the
`TEST byte [EBX+0x1353], 0x10` @0x0011329A. `b3_td_object_contact` already
refuses on `!a.crash_ok` (`td_rules.c:934`). Ported. `[C]`

---

## 5. Known non-gaps (audited, deliberately not modelled)

* **type-3 `+0x174 & 8`.** The only writer in the image is
  `OR byte [ECX+0x174], 8` @0x001A7584 in `FUN_001A7210`, gated by a byte
  argument *and* a global, and it records the body in `DAT_00739C68` — i.e. it
  designates **one** special "big-hit" vehicle whose collisions are suppressed.
  All 3,005 static records in the shipped event data have that designation
  clear, so the path is inactive in normal play. `[S]`
* **type-4 bodies** (§3) do not exist on these tracks.

## 6. Still open `[?]`

* What invokes the traffic crash entry *directly* in retail. Traffic is
  crashable through the shared vtable slot (§0) and through §2/§3 like any
  car, and the harness now reproduces both, but no traffic-specific invoker
  has been located.
* The 21 progress-keyed rows of the TDESC traffic schedule (row 0 alone is
  applied). These change traffic SPEED mid-race, which moves closing speeds
  and therefore crash frequency — a traffic-fidelity gap, not a crash-logic
  one.

---

## 7. What this cost to get wrong

Two of these gates were missing at once and the game became uncrashable: §1's
contact source had been deleted as "invented GLUE", and the traffic-vs-traffic
arm of §3 never ran (the pair-outcome branch keys on a racer pointer that is
NULL when both bodies are traffic), so no traffic car could ever wreck and the
cheap cascade could never seed. Measured over 120 s, restoring both took
wreck-path calls against a crashed traffic body from **0 to 2718** and crashes
per run from **3 to 7**, while leaving only 0.7% of traffic in the crashed
state at any moment.

---

## 8. Debug knobs

Every one is env-gated and off by default.

| knob | what it does |
|---|---|
| `B3_DRIVE_LOG=1` | append the WHOLE game state to `build/drive_log.txt` every frame (or `B3_DRIVE_LOG=<path>`). Same block the T-key dump writes, so a frame-by-frame trace and a one-shot report read identically. ~3.3 KB/frame — about 22 MB per minute at 60 fps, so it is opt-in. Flushed every 120 frames, so a kill still leaves a usable tail. |
| `B3_MAX_BOOST=1` | start the PLAYER with a full boost bar. Reaching the 150 mph closing gate by hand is slow from a cold bar. Player only. |
| `B3_CFIRE_TRACE=1` | the in-substep `crash_fired` decision beside the `td_rules` one, plus the frozen-soup poly count |
| `B3_WRECK_TRACE=1` | every `FUN_00113960` wreck-path decision (types, crashed flags, j, threshold, outcome) and the attacker-spared guard |
| `B3_TRAFFIC_HIT=1` | every racer-vs-traffic contact with vn, true closing speed, both speeds and the crash flags |
| `B3_POOL_TRACE=1` | traffic pool acquire/release/seed-fail per pass |
| `B3_CRASH_TIMING=1` | wall-clock and game-clock duration of each player crash |
| `B3_ROLL_TRACE=1` | each barrel-roll filing with the accumulated angle |
| `B3_WALL_SWEEP=0` | disable the substepped wall-contact sweep (see §1) |
