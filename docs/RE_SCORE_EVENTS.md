# Burnout 3 earn events — ONCOMING, NEAR MISS, DRIFT, AIR

How the game decides that you are earning boost, what it measures, what it
pays, and which HUD callout it lights. Recovered 2026-08-11 from the analyzed
`build/burnout3.elf` (addresses are the corrected VAs; `.text` = flat +
0x10000).

Marks: **[C]** execution-verified — a green differential case in
`tools/validate_score_events.py` runs the ORIGINAL x86 under Unicorn from a
seeded score object and asserts the port reproduces the memory transition; or
an exact decompilation/immediate read at the cited address. **[S]** static
only. **[?]** open. **GLUE** = harness-side representation bridge.

Ported to `src/burnout3_score_events.c/.h`. **tools/validate_score_events.py
— 116/116.**  §5–8 are the original recovery; **§9 is the CRASH GATE**
(2026-08-11) — why a crashing car earns nothing, which is the fix for the
"NEAR MISS while wrecked" report.

> **Bridge hazard (cost me the first hour).** The Ghidra project has TWO
> programs open: `burnout3.elf` (correct — image base 0x00010000, 17 segments,
> 7434 functions) and `default.elf`, which is the **same bytes flat-loaded as a
> Raw Binary** (image base 0, one `ram` segment) — exactly the failure HANDOFF
> §2 warns about. The bridge served `default.elf` as current, where
> `"Score/Burnout Points"` reads at 0x0039E9F8 instead of 0x003B09F8 and every
> function boundary is wrong. `/switch_program` reports success without
> changing the served program; the reliable fix is to pass
> **`&program=burnout3.elf` on every request**. Anything derived without it is
> void.

---

## 1. Where the detectors live

The per-frame score update **FUN_001935F0** runs them, per car, with the score
object in EDI (`racecar+0x10D0`, docs/RE_GAMEPLAY.md §3):

| address | call | takes |
|---|---|---|
| 0x00193A04 | FUN_00194A80 rubbing | ESI = score |
| **0x00193A09** | **FUN_00194EE0 near miss** | **ESI = score** |
| 0x00193A32 | **FUN_00196940 air** | EDI = score, **XMM1 = distance step** |
| 0x00193A3D | **FUN_00196BE0 oncoming** | EDI = score, XMM1 = step |
| 0x00193A48 | **FUN_00196E10 drift** | EDI = score, XMM1 = step |

**The distance step [C].** All three category detectors are handed the *same*
scalar, computed once at 0x001939B5:

```
XMM1 = [score+0x2A0..0x2AC]      ; previous frame's position (16 B)
XMM0 = [score+0x2B0..0x2BC]      ; this frame's position
SUBPS XMM0, XMM1                 ; packed delta
[ESP+0x10] = XMM0
EAX = ESP+0x10 ; CALL FUN_00013C10
[score+0x288] += XMM0            ; total distance travelled
[ESP+0xC] = XMM0                 ; -> XMM1 for each detector
```

`FUN_00013C10` is `sqrt(v.x² + v.y² + v.z²)` — MULPS + two SHUFPS(0x39)
rotations summing lanes 0..2, then SQRTSS **[C, exact disassembly]**. So the
accumulator every category adds is **the 3-D distance the car moved this
frame**, in metres. Not speed × dt, not a projected arc length.

## 2. The shared template [C]

`FUN_00196940` / `FUN_00196BE0` / `FUN_00196E10` are the same function three
times over. Reading FUN_00196BE0 (oncoming):

```
active = state_flag && (speed_mph >= min_speed)      ; AIR HAS NO SPEED GATE
score[active_off] = active
if (active) {
    FUN_00192D20(record, record.value + step)        ; accumulate + retier
    if (record.value > min_distance) {               ; STRICTLY greater
        units = first_payment ? record.value : step
        FUN_0017A530(&score[0xCC], per_metre * units)
        score[scored_off] = 1
    }
} else {
    if (score[scored_off]) {                         ; this event ever paid
        if (record.tier >= 0) {
            BP = CategoryBP[tier]
            score[0x4C] += BP ; score[0xB8] += BP    ; racecar+0x111C / +0x1188
            claim the HUD callout (§5)
        }
        total_stat += record.value
        best_stat = max(best_stat, record.value)
    }
    record.prev = record.value ; record.prev_tier = record.tier
    record.clock = *(clocksrc+0xC) ; record.value = 0 ; record.tier = -1
    score[active_off] = 0 ; score[scored_off] = 0
}
```

Two details that are easy to get wrong and are pinned by green cases:

* **The payment gate is strictly `>`** (`COMISS`/`JBE`). Landing exactly on the
  minimum pays nothing. Three boundary cases assert this.
* **The first payment covers the whole accumulation**, including the metres
  below the minimum; every later frame pays only its own step. So crossing the
  40 m oncoming threshold pays for 40 m at once, then trickles.

Speed source: `racecar+0x64 × 2.2369363` — the **TRUE** m/s→mph constant at
0x0038994C, not the 2.2374146 the physics uses.

### The per-category table [C]

| | AIR | ONCOMING | DRIFT |
|---|---|---|---|
| function | FUN_00196940 | FUN_00196BE0 | FUN_00196E10 |
| state flag | racecar+0x10C0 | **racecar+0x18FC** | racecar+0x10C2 |
| speed gate | **none** | 0x3F73A8 (50 mph) | 0x3F73B4 (90 mph) |
| record | score+0x358 | score+0x374 | score+0x390 |
| active byte | score+0x368 | score+0x384 | score+0x3A0 |
| paid flag | score+0x3C8 | score+0x3C9 | score+0x3CA |
| min distance | 0x3F73A0 (5 m) | 0x3F73AC (40 m) | 0x3F73B8 (20 m) |
| boost/metre | 0x3F73A4 (0.3) | 0x3F73B0 (0.15) | 0x3F73BC (0.1) |
| category BP | 0x3F7490 | 0x3F74A0 | 0x3F74B0 |
| tier minima | 0x3F7550 | 0x3F7560 | 0x3F7570 |
| total / best | +0x50 / +0x54 | +0x58 / +0x5C | +0x60 / +0x64 |
| count stat | +0x354 (air only) | — | — |
| callout id | 0x72 | 0x73 | 0x71 |

(Values are the retail VDB tune; compiled-in defaults and the full parameter
tables are docs/RE_GAMEPLAY.md §2, which this work re-derived byte-for-byte
from the .data image as a cross-check.)

Air additionally fires a sound at 0x00196A9F via `FUN_00017EC0`, gated on
`racecar+0x1920 == 0` (player class), `tier >= 0` and physics-vehicle speed
`*(pv+0xBC) × 2.2369363 > 20`. FX only — no scoring effect **[C]**.

The 0x1C-byte category record and its tier tracker `FUN_00192D20` are
documented in docs/RE_GAMEPLAY.md §3/§5; the port re-asserts the tracker
through every category case here.

## 3. ONCOMING — what "the wrong side of the road" actually means [C]

**This is the answer to the question that started this work.** The game does
**not** compare your heading against traffic, and does not test which side of
the centreline you are on. It reads a flag the road-network tracker set:

`racecar+0x18FC` has exactly **one writer**, `FUN_0018D790` at **0x0018D89F**
(`MOV byte ptr [EBX + 0x18fc], DL`), plus two clears (0x0018D55C in
FUN_0018D0E0, 0x0018D9E5). An operand-substring sweep of all 793,970
instructions finds no other reference. The writer:

```c
/* FUN_0018D790, in_ECX = racecar, guarded by `if (DAT_0073A178 >= 1)` */
FUN_00174960(&path_obj, &flags);        /* locate the car on the road graph */
...
racecar[0x18C4] = path_obj;             /* in_ECX[0x631] */
racecar[0x18C8] = (u16)segment_index;   /* in_ECX[0x632] */
iVar3 = segment_index * 10;
bVar4 = *(byte*)(*(*(path_obj + 4) + 8) + iVar3 + 3) & 7;
racecar[0x18FC] = (bVar4 == 1 || bVar4 == 3);          /* <-- THE RULE */
racecar[0x18D0] = *(s16*)(*(*(path_obj + 4) + 8) + iVar3);  /* section id */
```

So:

* the road network carries a table of **10-byte per-segment records** at
  `*(*(path_obj + 4) + 8)`;
* record `+0x00` is an `s16` route-section id (the game compares it against
  `DAT_0073A188 - 0x10` for checkpoint logic);
* record `+0x03` holds a **3-bit lane-type field**, and lane types **1 and 3**
  are the wrong side;
* with no road network loaded (`DAT_0073A178 < 1`) the flag is forced to 0 and
  the section id to −1.

In other words *oncoming is baked into the track data per road segment*, which
is why the game can score it consistently on divided highways, tunnels and
roundabouts where a heading test would misfire. The locator `FUN_00174960`
takes the previous `(path object, segment index)` as a hint, re-acquires with
`FUN_00174CF0` when the hint is stale, and advances with `FUN_00175570` **[S:
read, not executed]**.

**Open [?]:** the byte layout of the remaining 7 bytes of the segment record,
and where that table sits in `Gamedata.bgd`. docs/RE_BGD.md §5 places per-node
lane/link data in the road-network section's index directory (u16/u8 packed
rows) but has not decoded it; this finding says exactly what to look for
there — a 10-byte stride with a 3-bit lane type at +3.

**GLUE in the harness.** The harness's road representation is the two
recovered drive lines (forward race line, and the 926-point reverse-direction
line at 0x1A8540), not the per-segment lane table. The same decision is
mirrored as *"closer to the reverse-direction line than to the forward race
line"*. Everything downstream of the flag is the game's own rule; only the
flag's *source* is bridged, and it is marked GLUE at the call site.

## 4. NEAR MISS [C]

`FUN_00194EE0`, score object in **ESI**. Three passes per frame.

### 4.1 The proximity test — `FUN_00195DD0` [C]

`__regparm3(EAX = score, ECX = candidate object)`. It is **not** a centre
distance. It is a **box-to-box gap in the ground plane** with a height gate:

1. `|other.pos.y − my.pos.y| > 4.0` → reject (imm at 0x00195DE9/0x00195DF8).
2. Each car is reduced to a centre and two half-axis vectors from its `.bgv`
   corners (`+0xE80` max, `+0xE90` min — `B3VehicleFull.half_ext` /
   `center_off`): `halfX = (max.x−min.x)/2`, `halfZ = (max.z−min.z)/2`,
   axis A = matrix row0 × halfX, axis B = matrix row2 × halfZ. My matrix is at
   `racecar+0x10`, the candidate's at `object+0x70`.
3. Bounding-circle reject: `|halfA₂| + |halfA₁| + NearMissDistance < |Δcentre|`.
4. Each half-axis is flipped to point at the other box, giving the two nearest
   corners; the gap is the smallest of the corner-to-corner distance and four
   point-to-edge projections.
5. Hit iff `gap² < NearMissDistance²` (0x3F73C0 — **1.8 m** tuned, 2.0 default).

So "Near Miss Distance (Metres)" is **clearance between the car bodies**, not
separation between origins. A bus and a hatchback trigger at different centre
distances, which is the behaviour you feel in game.

Candidates come from an 8-entry-per-car broadphase: count
`DAT_00649B3C[grid_slot]`, ids `DAT_00649A8E[grid_slot*8 + j]`, each indexing
an object table at **0x625FB0, stride 0x180** (`EDI = id*3<<7 + 0x625FB0` at
0x00194F2F). If `object+0x10C` is non-null the tracked id is remapped to
`*(byte*)(owner + 0x177)`, so every part of one car maps to one id **[C]**.
The broadphase *producer* is **[?]** — the harness passes its traffic set
directly (GLUE at that boundary only).

### 4.2 Slot tracking, chain, award [C]

```
1. for each candidate passing the OBB test, at speed >= 0x3F73C4 (60 mph):
     refresh score+0x3E8[i] if already tracked  (timestamp score+0x3F0[i])
     else claim a free slot (id 0xFF) and ARM it (score+0x410[i] = 1)
     else evict the least-recently-seen slot whose timestamp predates the
          previous frame's clock, and arm that
2. chain expiry: if chain != 0 and (last_link + ChainTime(0x3F73D0) < clock
                                    or speed < ChainSpeed(0x3F73C8)):
     chain = 0 ; if tier >= 0 -> NearMissCategoryBP[tier] + callout id 0x74
     reset the chain record (score+0x418)
3. award scan, per slot:
     if timestamp < PREVIOUS frame's clock (score+0x3DC) and armed:
        disarm; chain += 1 (score+0x3D0); total += 1 (score+0x3CC)
        award "Boost value per Near Miss" (0x3F73CC) through FUN_0017A530
        BP += chain * NearMissBP (0x3F7480)
        retier the chain record on the CHAIN LENGTH against 0x3F75A4
        last_link = clock (score+0x3E0)
     if timestamp + 1.0 < previous clock -> release the slot (id = 0xFF)
   score+0x3DC = clock
```

The rule that makes it once-per-vehicle-per-pass: **the award fires when the
car stops being close, not when it gets close.** A slot is armed on first
proximity, pays when its last-seen timestamp falls behind the previous frame,
and is only released a second later — so sitting alongside a bus for three
seconds is one near miss, and the boost lands as you clear it.

The chain record's *value is the chain length*, so the near-miss tier minima
(0x3F75A4 = 999/2/5/9) are **counts**, not metres — a 2-car chain is "GREAT
NEAR MISS!". The boost award here is `FUN_0017A530` inlined (score+0x114 +
score+0x118 multipliers, clamp at score+0x100), which independently confirms
the boost-record offsets in `burnout3_gameplay.h`.

## 5. The HUD earn callout [C]

Each closing event tries to claim the score object's announcement slot:

```
CMP dword ptr [score+0x254], <own id> ; JLE  -> claim
score+0x254 = id ; score+0x25C = racecar+0x10DC ; score+0x134 = 1
score+0x260 = tier ; score+0x258 = FUN_00011510(BP, score+0x258)
```

so **the immediate in each compare is that event's own id, and a claim
succeeds only when the pending id is `<=` it** — the ids *are* the priority
order:

| id | event | compare @ | HUD category (table 0x003C8390) |
|---|---|---|---|
| 0x71 | DRIFT | 0x00196F21 | 1 |
| 0x72 | AIR | 0x00196A2E | 0 |
| 0x73 | ONCOMING | 0x00196CF1 | 2 |
| 0x74 | NEAR MISS | 0x00195167 | 3 |

The claim is additionally gated on `racecar+0x18FA == 0` (not crashed) and
`FUN_00017310() == 0` (not a crash-party mode). The 16 strings at 0x003C8390
read back as GOOD/GREAT/FANTASTIC/AWESOME × AIR/DRIFT/ONCOMING/NEAR MISS,
confirming `burnout3_hud.h`'s category order.

Note the retail tune disables the lowest medal tier for air/oncoming/drift
(tier-0 minima are the sentinels 1115/9150/9980 with tier-0 BP = 0), so
"GOOD ONCOMING!" is unreachable in a shipped race — the first oncoming callout
you can earn is "GREAT ONCOMING!" at 500 m.

## 6. The two flags this work did NOT locate — [?]

`racecar+0x10C0` (airborne) and `racecar+0x10C2` (drift) have **no writer
anywhere in the image**: an operand-substring sweep and a raw byte-pattern
search for the `c0 10 00 00` displacement both return only the two readers
(0x00196946, 0x00196E16) — so they are written through a base pointer that is
not the racecar. `FUN_001372F0` reads a sibling byte at `+0x10C3`. Their
*semantics* are unambiguous from the readers (non-zero = state active) and the
harness already owns both signals — per-wheel contact from the ported
suspension, and `drift_state` at `veh+0x1524` from the ported vehicle sim — so
the port reads those instead and marks it. **The SCORING of drift and air is
fully recovered; the provenance of the two state bytes is not.**

## 7. Verification inventory — tools/validate_score_events.py, 92/92

Every case seeds a game-shaped score object, executes the real functions under
Unicorn through a trampoline that reproduces FUN_001935F0's own call sequence
(EDI and ESI both set; XMM1 re-loaded per detector), then runs the port from
the byte-identical seed via `build/dump_score_events` and diffs the two
post-images field by field. Near-miss sequences run frame by frame and carry
the **real** post-state forward, so a divergence cannot hide behind a reset.

| section | function(s) | cases | asserted |
|---|---|---|---|
| air/oncoming/drift | FUN_00196940/BE0/E10 + FUN_00192D20 | 22 | record value/clock/prev, active, tier/prev_tier, paid flag, stats, BP totals, boost meter/earned, callout id+tier |
| proximity test | FUN_00195DD0 (through FUN_00194EE0) | 24 | slot claim per geometry, + the real function's verdict vs the expected one |
| near miss | FUN_00194EE0 | 46 | 8 slot ids/timestamps/armed flags, chain, total, last/chain-end clocks, tier, BP, boost |

Parameters are seeded explicitly on both sides, and every case is run against
both the compiled-in defaults and the retail VDB tune where they differ, so a
pass is not an artifact of one parameter set.

**Mutation-tested** (the suite must fail when the port is wrong): OBB height
gate 4→40 → 91/92; first-payment rule removed → 87/92; strict `>` → `>=` →
89/92; near-miss award `<` → `<=` → 79/92; disarm removed → 79/92; OBB axis
flip inverted → 72/92.

Emulation environment: the game-context virtuals are the same 3-instruction
stubs `tools/validate_gameplay.py` uses (`+0x40` → 1, `+0x90`/`+0x94` → 0,
`+0xAC` → FLD1, `+0x5C` → `RET 8`); `racecar+0x1920` is set non-zero so the
air FX call is skipped; `FUN_00017310`, `FUN_00011510` and `FUN_0017A530` all
execute for real.

## 8. Harness integration

`src/burnout3_score_events.c/.h`. Per car: `B3ScoreEvents` (the score-object
subset these events own) driven by `b3_score_events_frame()` and
`b3_score_events_near_miss()`, awarding through `b3_boost_award` into the
existing `B3BoostBar`, and `b3_score_events_take_callout()` feeding
`b3_hud_boost_event(cat, tier)`.

GLUE, all marked at the call sites: the oncoming flag's source (§3), the
near-miss broadphase (§4.1), and the airborne/drift state bytes (§6). The
distance step, every gate, every award, the tier scan, the slot/chain rules
and the callout priority are the game's own.

In-race evidence (autodrive on the wrong carriageway, 150 s):
**246.3 boost units earned from oncoming driving**, 6 near misses including a
3-link chain, and on-screen "FANTASTIC ONCOMING!", "GREAT NEAR MISS!",
"AWESOME AIR!" and "GREAT DRIFT!" callouts.

---

## 9. THE CRASH GATE — why a wreck must not earn [C]

*Added 2026-08-11 (score-gating pass). Bug report: the player crashed into
traffic and got a **"NEAR MISS"** callout as the wreck flew.*

The hypothesis was right — the near-miss award fires on OBB *separation*
(§4.2), so a car that stops being close because it just wrecked into it looks
exactly like a car that stopped being close because it drove past. Retail
gates this, and not in the detector: **the detectors never run at all.**

### 9.1 The gate — FUN_001935F0 @0x001939AD [C]

```
001939a5 TEST EAX,EAX ; JZ 0x00193e35        ; racecar+0x179C (car is racing)
001939ad MOV  AL,byte ptr [ESI + 0x18fa]     ; ESI = racecar   CRASHED
001939b3 TEST AL,AL
001939b5 MOVAPS XMM1,[EDI + 0x2a0]           ; (the step is computed either way)
001939bc MOVAPS XMM0,[EDI + 0x2b0]
001939c3 SUBPS  XMM0,XMM1
001939c6 MOVAPS [ESP + 0x10],XMM0
001939cb JNZ  0x00193a80                     ; -> the reset block
001939d1 MOV  AL,byte ptr [ESI + 0x18fb]     ; RESPAWNING (being re-placed)
001939d7 TEST AL,AL
001939d9 JNZ  0x00193a80
001939df LEA  EAX,[ESP + 0x10] ; CALL 0x00013c10   ; the detectors from here
```

Everything §1 lists — FUN_00013C10 (the step and `score+0x288`), FUN_00194A80
rubbing, **FUN_00194EE0 near miss**, FUN_00195490/9A0/CE0, FUN_00197260/180,
**FUN_00196940/BE0/E10 air/oncoming/drift** — is inside the branch the gate
jumps over. So while the racecar is crashed **not one of the four detectors
executes**, and the question "do the state-byte writers gate on the driving
state?" never arises: the *readers* are unreachable. (The callout claim's own
`racecar+0x18FA == 0` test, §5, is therefore belt-and-braces — it can only be
reached with the byte already clear.)

**`racecar+0x18FB` is the second half of the gate [C].** Every writer pairs it
with +0x18FA: crash entry sets `+0x18FA = 1` (0x0018C80B, the block
RE_TAKEDOWN_FX §515 already ties to the crash), and every crash *exit* clears
+0x18FA and sets +0x18FB in the next instruction — 0x0018C884/0x0018C88B,
0x0018C8A7/0x0018C8AE, 0x001709C4/0x001709CB, 0x0018E040/0x0018E047 (a
3-instruction setter pair at 0x0018E036 / 0x0018E040). FUN_0018D790 sets it
around its re-placement path (0x0018D938) and clears it at its tail
(0x0018D9D0). So +0x18FB means *"this car is being teleported back onto the
road this frame"*, and the gate keeps the detectors off for that frame too —
which is what stops the respawn jump from being scored as 60 m of AIR.

### 9.2 What the crash branch does instead — 0x00193A80..0x00193CE4 [C]

```
00193a80 PUSH EDI ; CALL 0x00197040          ; takedown claim scan still runs
00193a86 OR   EAX,0xffffffff
00193a89 MOV  [EDI + 0x3e8],EAX              ; slot ids 0..3  := 0xFF
00193a8f MOV  [EDI + 0x3ec],EAX              ; slot ids 4..7  := 0xFF
00193aa0 MOV  [EDI + 0x3d0],ECX(0)           ; near-miss CHAIN := 0
00193aa6 MOV  [EDI + 0x590],ECX
00193aac MOV  [EDI + 0x3c8],CL               ; air   `scored` := 0
00193ab2 MOV  [EDI + 0x3c9],CL               ; onc   `scored` := 0
00193ab8 MOV  [EDI + 0x3ca],CL               ; drift `scored` := 0
00193abe MOV  [EDI + 0x5b8],CL
00193ac6 ...  release the tracked opponent at score+0x5B4 (byte +0x1688)
00193add per-racecar loop over DAT_0073A19C: score+0x438/+0x440/+0x528/
         +0x558/+0x55E (rubbing/tailgate bookkeeping — other modules)
00193b36 AIR record      0x358 : prev:=value, prev_tier:=tier, clock:=now,
00193b76 ONCOMING record 0x374   value:=0, tier:=0xFF, active byte := 0
00193bb3 DRIFT record    0x390
00193bf0 RUBBING  0x564 / 00193c2d TAILGATE 0x598 / 00193c6a GRINDING 0x5C4
00193ca7 NEAR-MISS CHAIN record 0x418 (same reset, active +0x428 := 0)
```

Two things this block is **not**:

* it is **not** the event-end path. There is no `CategoryBP[tier]` payment,
  no total/best stat, no callout claim. A 400 m oncoming run that ends in a
  crash forfeits its BP, its stats **and** its "FANTASTIC ONCOMING!". Only the
  per-metre boost already banked in the meter survives.
* it is **not** a full wipe. Deliberately untouched: the eight ARMED flags
  (score+0x410), the eight last-seen clocks (+0x3F0), the lifetime near-miss
  count (+0x3CC), the previous-frame clock (+0x3DC) — FUN_00194EE0 is the
  only writer of that and it never runs — the BP totals, the boost meter and
  the pending callout. The armed flags are left dangling harmlessly because
  every slot id is 0xFF, and a slot is re-armed when it is next claimed.

The pending near miss is therefore **destroyed, not paid** — which is exactly
the missing rule. Answering the report directly: **there is no crash
situation in which retail pays the crashing car a near miss.**

### 9.3 The candidate side is NOT gated [C]

The other half of the hypothesis does not hold, and the honest answer is the
uncomfortable one:

* `FUN_00195DD0` reads no state at all. Its 450 instructions contain no
  `byte ptr` operand, no `TEST` and no `CALL` — it is pure geometry.
* `FUN_00194EE0`'s scan (0x00194F21..0x00194F69) goes broadphase byte →
  `EDI = id*0x180 + 0x625FB0` → OBB test → owner remap (`obj+0x10C`) → speed
  gate. There is no crashed/wrecked test on the candidate.
* The broadphase producers — the only writers of `DAT_00649A8E`/`DAT_00649B3C`
  — are FUN_001A6B40 (@0x001A6D2B), FUN_001A8220 (@0x001A85BC) and
  FUN_001A8640 (@0x001A8B4B). All three insert on **distance < 16 m** and the
  8-slot capacity alone; two have no state gate whatsoever, the third
  additionally requires `obj+0x174 & 2 == 0` and a live driver object at
  `obj+0x114` (meaning **[?]**). **[S: read, not executed]**

So a wrecked vehicle stays a near-miss candidate. **Retail does pay a near
miss against a car you have just wrecked, as long as *you* are not crashed** —
take a rival down, stay clean, thread the tumbling wreck and it counts. That
is a real retail behaviour, not a bug, and it is not what the report hit: in
the report *the player* was the crashed car, which §9.1 covers.

> **Harness deviation (not fixed here — burnout3_full.c is not this module's
> file).** `score_near_miss_update()` skips traffic with
> `t->crashed_until > g_race_time`, i.e. the harness is *stricter* than retail
> on the candidate side. Faithful would be to keep wrecked traffic in the
> candidate list. It does not affect the reported bug either way.

### 9.4 Port + verification

`b3_score_events_set_crash()` mirrors the two racecar bytes into the state,
`b3_score_events_suspended()` is the gate, and `b3_score_events_crash_reset()`
is the 0x00193A80 block. `b3_score_events_frame()` runs the gate and the reset
in place of the detectors; `b3_score_events_near_miss()` refuses outright (it
is one of the calls the gate jumps over, so it must not even advance
score+0x3DC). The two bytes are deliberately **not** members of `B3ScoreFrame`
— callers fill that struct field by field on the stack and new members would
be read uninitialised.

**tools/validate_score_events.py — 116/116** (was 92/92; +24). The new cases
execute the **real slice 0x001939AD..0x00193CE4** under Unicorn with EDI/ESI
holding what FUN_001935F0 itself holds there, so the *game's own* branch
decision and its *own* reset block produce the reference image:

| section | cases | asserted |
|---|---|---|
| gate verdict | 4 | which branch the real code takes for each (0x18FA, 0x18FB) combination, vs `b3_score_events_suspended()` |
| crash reset | 6 | the whole score image (FIELDS + NM_FIELDS) after the real reset block, over seeds with live records, 8 occupied slots, an idle object and an untiered event |
| crashed vs not | 7 (+7 guards) | same seed and same traffic geometry run both ways through the real slice; each is also guarded by "did the real code actually pay?" so neither side can pass by doing nothing |

Mutation-tested (the suite must fail when the port is wrong): near-miss gate
removed → 112/116; air/oncoming/drift gate removed → 112/116; gate ignores
+0x18FB → 113/116; reset also clears the armed flags → 107/116; reset also
clears the lifetime count → 107/116; reset keeps the slots → 107/116; reset
skips the record resets → 106/116; reset advances prev_clock → 106/116.

**In-race evidence** (scratchpad `scoregate/evidence_ingame.txt`, one binary,
one scripted encounter — two traffic cars alongside at 130 mph, the player
wrecked the frame both slots arm, then the cars separate):

```
pre-fix   t=20.003 slots=2 armed=2      ... t=21.314 HUD CALLOUT: GREAT NEAR
          t=20.019 *** CRASH ***            MISS!  (player crashed=1)
          final: nm_total=2  bp=10  meter=25.9      <- the reported bug
with fix  t=20.019 *** CRASH *** -> slots=0 immediately (the reset)
          final: nm_total=0  bp=0   meter=4.2   0 callouts
```
