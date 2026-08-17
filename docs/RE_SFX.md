# RE: the crash / impact / takedown SOUND EVENT system

Scope: how Burnout 3 decides **which wave plays on which gameplay event, at
what gain and what pitch**. The wave *data* extraction is `docs/AUDIO_NOTES.md`;
this document is the executable side.

Confidence legend as in `docs/RE_NOTES.md`:
`[C]` confirmed by two independent derivations or by executing the real code,
`[S]` structurally derived but not executed, `[?]` unresolved, `GLUE` original
harness code.

Ported to `src/burnout3_sfx.c` / `.h`.
Differential suite `tools/validate_sfx.py` — **243/243 green**.

---

## 1. Wave names are base-40, and the last two characters are a variant pair

The audio module never uses strings for wave names. Every emitter does

```
0014f585  MOV  EAX, 0x0039CB78          ; -> packed u64 "IMPACTNUDG  "
0014f584  PUSH EDX                      ; the wave bank
0014f582  PUSH 0x1                      ; randomise-variant flag
0014f590  CALL 0x001C99D0               ; wave-dictionary lookup
```

`0x0039CB78` holds a **12-character base-40 value** in exactly the packing
`FUN_001AECC0` decodes for vlist car ids (`tools/extract_traffic.py b40()`,
alphabet `" -/0123456789A..Z_"`, left justified, space padded). `[C]`

`FUN_001C99D0` is a lower-bound binary search over the bank's sorted
`0x10`-byte records, and the key is taken **modulo `0x640` = 40x40** via
`__aullrem` (0x001C9A59 and the entry at 0x001C99F0):

```c
key      = *(u64*)EAX;                    /* "IMPACTNUDG  " */
rec_key  = record.name - (record.name % 1600);   /* mask 2 chars off */
```

so the last two base-40 characters are **excluded from the match**. When the
randomise flag is set the code then decodes those two characters out of the
found record (`FUN_001AED30` at 0x001C9ABA) and offsets the record index by a
value from the module LCG at `DAT_004A1BE0`:

```
iVar5 = iVar5 + 1 + ((rand + count) % count - index)
```

i.e. **char 11 is the variant index (1-based) and char 12 is the variant
count**. `[C]`

That is the rule behind every shipped wave filename: `impactnudg13`,
`impactnudg23`, `impactnudg33` are variants 1..3 of 3; `impactfata16..66` are
1..6 of 6; `slam______12`/`slam______22` are 1..2 of 2 with the base name
padded to 10 characters by underscores. The XBE stores only the 10-character
stem (`"SLAM______  "` at `0x0039C360`) and the bank supplies the family.

Consequence for anyone hunting more of these: **searching the image for the
literal shipped filename mostly fails** — you have to search for the
space-padded stem.

### 1.1 The name tables

Two contiguous packed-name arrays carry everything in this document:

| VA range | contents |
|---|---|
| `0x0039C310`..`0x0039C367` | `BOOSTLOSS BOOSTGAIN SFXTESTL SFXTESTR STATICLP BOOSTLOOP FIRE P_INTRO_L P_INTRO_R SHUNT_____ SLAM______` |
| `0x0039CB28`..`0x0039CBEF` | `AIRRAM P1HORN STEAMRADAT ATMOS CREAK FIRE KURBBOUNCE HEART IMPACTWORL IMPACTFATA IMPACTNUDG GLASSDEBRI GLASSFRONT GLASSSIDES GLASSWINDS EXP EXP CARPARTLDE CARPARTMDE CARPARTLRE CARPARTMRE CARPARTLPR CARPARTMPR CARPARTWPR CARALARMLP` |

plus `CARSCRAPLO`/`CARSCRAPHI` at `0x0039D158`/`0x0039D160`, `STATICPASS` at
`0x0039D670`, the road-surface names `TAR GVL WOOD METAL SNOW` at
`0x0039BC00`, and the per-track prop-hit block at `0x0039CC10`..`0x0039D048`
(`WOODENBOXH13 METALBOXHT13 MTLBARRIER13 BARRIERHIT13 CONTAINERH OILDRUMHIT
BOATPAYHIT WOODLOGHIT HAYBALEHIT CABLEDRUMH`). All `[C]` — each decodes to a
name that exists in the extracted banks and each is pointed at by a `.text`
immediate.

---

## 2. The event law

Every impulse-driven emitter is the same seventeen instructions. Taking
`FUN_0014F3E0` (IMPACTNUDG) as the reference, with `ESI` = the per-racecar
audio object:

```
0014f3e6  MOVSS XMM1,[ESI+0x6D0]  ; min impulse
0014f3ee  COMISS XMM1,XMM0
0014f3f4  JA    <return>          ; below min impulse -> SILENT
0014f448  SUBSS XMM0,[ESI+0x6D0]
0014f440  MOVSS XMM3,[ESI+0x6D4]  ; max impulse
0014f450  SUBSS XMM3,[ESI+0x6D0]
0014f463  DIVSS XMM0,XMM3         ; t
0014f487  MAXSS XMM0, 0.0
0014f48d  MINSS XMM0, 1.0         ; clamp01
0014f49f  MOVSS XMM0,[ESI+0x6DC]  ; max gain
0014f4a7  SUBSS XMM0,[ESI+0x6D8]  ; - min gain
0014f4fd  MULSS XMM0,XMM3
0014f501  ADDSS XMM0,[ESI+0x6D8]  ; gain  = lerp(minG,maxG,t)
0014f4af  MOVSS XMM4,[ESI+0x6E4]  ; max pitch
0014f4b7  SUBSS XMM4,[ESI+0x6E0]
0014f509  MULSS XMM4,XMM3
0014f515  ADDSS XMM4,[ESI+0x6E0]  ; pitch = lerp(minP,maxP,t)
```

then

```
0014f571  JZ  +8
0014f573  MULSS XMM0,[0x003B1688]      ; gain *= 2.0 when obj+0x8DF is set
0014f59e  CALL 0x0014A6B0              ; pitch variance, argument 0.1
0014f5a5  FMUL [pitch]
0014f5b3  FILD [waverec->desc+0x10]    ; the wave's sample rate
0014f5c0  FMUL ST1
0014f5c7  FSTP [params+0x24]           ; PLAYBACK RATE in Hz
```

So, in one line: `[C]`

```
t     = clamp01((impulse - minImpulse) / (maxImpulse - minImpulse))
gain  = minGain  + t*(maxGain  - minGain)
rate  = wave.sampleRate * (minPitch + t*(maxPitch - minPitch)) * v
v     = 1 + U(-0.1, +0.1)                        [FUN_0014A6B0]
        impulse < minImpulse  =>  no voice at all
```

`FUN_0014A6B0` is exactly `1 + r*(2u-1)` where `u = (unsigned)LCG * 2^-32`
from the object's own LCG at `+0x520`/`+0x524`; the emitters pass `r = 0.1`.
`[C]`

**This single law is the whole "small tap vs huge crash" behaviour** — a
harder hit is louder *and* lower/higher pitched along the same `t`, and taps
below the window make no sound rather than a quiet one.

The play-parameter block handed to `PlaySound3D` (`0x001CD8D0`, `EDI` =
block, `ESI` = `0x0040B844`) is:

| offset | meaning |
|---|---|
| `+0x00` | position `float[3]` |
| `+0x0C` | velocity `float[3]` (always 0 for these events) |
| `+0x18` | the wave record from `0x001C99D0` |
| `+0x1C` | **gain** |
| `+0x24` | **playback rate, Hz** (`-1` = the wave's native rate) |
| `+0x38` | max distance — `50.0` (`0x003B16B8`), `40.0` when off-screen |
| `+0x3C` | min distance — `15.0` (`0x003B16B4`) |

`[C]` — read out of a live emitter run.

### 2.1 Surface dependence

There is none on the *car* impact path. Surface only selects the road/skid
loops: `FUN_00136610` reads a 20-entry table at `0x0039BC00` of
`TAR/GVL/WOOD/METAL/SNOW`, one per surface id. The per-track object hits
(crates, oil drums, barriers…) are a separate family with their own tuples at
`+0x1B0`..`+0x24C` and `+0x730`..`+0x7AC` — see §5. `[C]`

---

## 3. The parameter block, and where its numbers come from

`FUN_0014A710` is the racecar-audio initialiser. It writes every tuple as a
compiled-in default and then registers the per-track prop tuples with the
**same ValueDB registrar the physics config uses** (`0x001AEE20`, hash core
`0x001AF250` — see `tools/extract_car_vdb.py`). Group/param strings are real:
`"Sound/Car/Nudge"`, `"Sound/World/Crash"`, `"Sound/Scrape/High"`,
`"Min Impulse"`, `"Max Volume"`, … at `0x003AE674`..`0x003AF17C`, with cfg
paths `../export/ValueDB/Sound/Crash.cfg` / `Payload.cfg` /
`Tracks/<TRACK>.cfg`. `[C]`

`tools/emulate_sfx_params.py` executes `FUN_0014A710` (and
`FUN_0014B600` message 1) under Unicorn, hooks the registrar and the hash
core, and reads the object back. It recovers **60 registered prop parameters
with their ValueDB keys and retail values** and the whole compiled-in tuple
block. The car/world/glass/panel tuples below are compiled-in defaults with
no ValueDB override, so the defaults are what ships.

`tools/emulate_sfx.py` then executes each **emitter** with a controlled
impulse and captures its `PlaySound3D` call, which is what
`tools/validate_sfx.py` compares the C port against.

---

## 4. THE TABLE — event → wave → gain/pitch

All rows `[C]` unless marked. `off` is the tuple's offset in the racecar
audio object; impulses are in the emitter's own units.

| emitter | wave (bank) | off | min imp | max imp | gain | pitch | cooldown |
|---|---|---|---|---|---|---|---|
| `FUN_0014F3E0` car-vs-car, victim alive | `IMPACTNUDG` (crashmod) | `+0x6D0` | 700 | 4000 | 0.25 → 0.80 | 0.70 → 1.10 | 20 + rand%20 |
| `FUN_0014F690` car-vs-car, victim wrecked | `IMPACTFATA` (crashmod) | `+0x700` | 100 | 5000 | 0.30 → 1.00 | 0.75 → 1.05 | 20 |
| `FUN_0014F130` crash-mode fatal impact | `IMPACTFATA` (crashmod) | `+0x6E8` | 2 | 10 | 0.80 → 1.00 | 0.90 → 1.30 | 20 |
| `FUN_0014EEA0` car-vs-world | `IMPACTWORL` (crashmod) | `+0x6A0` | 6 | 22 | 0.20 → 0.60 | 0.80 → 1.10 | 20 + rand%20 |
| `FUN_0014D5F0` windscreen | `GLASSFRONT` (crashmod) | `+0x598` | 0.010 | 0.015 | 0.50 → 0.80 | 0.80 → 1.20 | 30 |
| `FUN_0014D8A0` side glass | `GLASSSIDES` (crashmod) | `+0x5A0` | 0.020 | 0.400 | 0.80 → 1.00 | 0.80 → 1.20 | 30 |
| `FUN_0014DB50` rear glass | `GLASSWINDS` (crashmod) | `+0x59C` | 0.040 | 0.600 | 0.80 → 1.00 | 0.80 → 1.30 | 30 |
| `FUN_0014EB00` large panel deform | `CARPARTLDE` (crashmod) | `+0x5E0` | 0.035 | 0.400 | 0.80 → 1.00 | 0.70 → 1.20 | 4 |
| `FUN_0014ECB0` medium panel deform | `CARPARTMDE` (crashmod) | `+0x5F8` | 0.050 | 0.300 | 0.60 → 1.00 | 0.80 → 1.20 | 7 |
| `FUN_0014FC80` large panel detach | `CARPARTLRE` (crashmod) | `+0x610` | 99 | 100 | 0.12 | 0.80 → 1.20 | 4 |
| `FUN_0014FE60` medium panel detach | `CARPARTMRE` (crashmod) | `+0x628` | 99 | 100 | 0.50 | 0.80 → 1.20 | 7 |
| `FUN_00150040` loose large panel vs world | `CARPARTLPR` (crashmod) | `+0x640` | 800 | 1500 | 0.10 → 0.50 (x0.5) | 0.80 → 1.10 | 15 |
| `FUN_00150260` loose medium panel vs world | `CARPARTMPR` (crashmod) | `+0x660` | 800 | 5000 | 0.10 → 0.50 (x0.5) | 0.80 → 1.10 | 10 |
| `FUN_00150480` loose wheel vs world | `CARPARTWPR` (crashmod) | `+0x680` | 1000 | 2500 | 0.70 → 1.00 | 0.80 → 1.20 | 5 |

The glass emitters are the only ones with **no silence gate** (`t` is clamped
but the `COMISS`/`JA` guard is absent) and the only ones that do **not** call
the pitch-variance helper — their pitch is the bare lerp. `[C]`
`CARPARTLPR`/`CARPARTMPR` multiply the gain lerp by the extra `0.5` at
`+0x65C`/`+0x67C`. `[C]`

Fixed-gain events (no tuple):

| emitter | wave (bank) | gain | notes |
|---|---|---|---|
| `FUN_00140610` takedown **slam** | `SLAM______` (mode bank) | mode-object `+0x5C`, halved on the non-racecar branch at `0x0014075E` | native rate, no pitch shift. Cooldown `+0x50 = 30`. Also stamps `0x0040E12C = clock + 0.06`. `[S]` on the level itself |
| `FUN_00140480` **shunt** | `SHUNT_____` (mode bank) | same field | Cooldown `+0x54 = 30`. `[S]` |
| `FUN_00140DF0` boost ignition | `BOOSTGAIN` (mode bank) | **1.0** (`0x003EC26C`) | `[C]` |
| `FUN_00140EF0` boost lost | `BOOSTLOSS` (mode bank) | **0.8** (`0x003EC268`) | `[C]` |
| `FUN_00141D20` boost loop | `BOOSTLOOP` (generic) + `FIRE` | — | looping `[S]` |
| `FUN_001521C0` body scrape | `CARSCRAPLO` + `CARSCRAPHI` (crashmod) | crossfaded by the caller | both loops start together `[S]` |
| `FUN_00150B90` kerb strike | `KURBBOUNCE` (crashmod) | — | cooldown `+0x60 = 5`; needs a live viewport list, not captured `[S]` |
| `FUN_00151490` air ram | `AIRRAM` (crashmod) | — | `[S]` |
| `FUN_001516C0` explosion | `EXP` (crashmod) x2 | — | also fires `FUN_0014D5F0` and `FUN_0014F130` at impulse `100000.0` `[C]` |
| `FUN_00151B70` radiator steam | `STEAMRADAT` (crashmod) | — | looping `[S]` |
| `0x0015204D` car alarm | `CARALARMLP` (crashmod) | — | looping, no owning function in the DB `[S]` |
| `FUN_00156300` near-miss whoosh | `STATICPASS` (generic) | — | `[S]` |

### 4.1 Which bank

* **crashmod** `[C]`: the emitters' bank pointer is racecar-audio `+0x880`,
  filled at `0x0014B6DD` by loading the string at `0x003AEDDC`,
  `"sound\crashmod.awd"` → `build/audio/awd_crashmod`.
* **mode bank** `[S]`: `SLAM`/`SHUNT`/`BOOSTGAIN`/`BOOSTLOSS` take theirs from
  the `0x00411560` mode singleton's `+0x78`. The four candidates are the
  strings at `0x003AE17E`..`0x003AE1AE` — `sound\elim.awd`,
  `sound\crash.awd`, `sound\roadrage.awd`, `sound\single.awd` — and a single
  race is `single.awd`. The pointer was **not** traced to that string, hence
  `[S]`; all four banks carry the same slam/shunt/boost waves, so the choice
  is inaudible.
* **generic** `[S]`: `BOOSTLOOP`/`STATICPASS` exist only in
  `sound\generic.awd`.

---

## 5. Trigger call sites

```
FUN_00183BD0  collision manager
  -> FUN_0014E960   collision-EVENT sound dispatch: switches on the two
                    colliding entities' type bytes ([ESI+0x24], [ESI+0x28],
                    jump tables at 0x0014EA70/0x0014EA80/0x0014EAA0)
     -> FUN_0014E7D0  per-vehicle impact accumulator + emitter choice:
                        victim +0x210 == 0 -> FUN_0014F3E0 (IMPACTNUDG)
                        victim +0x210 != 0 -> FUN_0014F690 (IMPACTFATA)
                      writes the accumulator at param_1 + idx*0x40:
                        +0x00 contact position (vec4)
                        +0x2C vehicle speed (+0xBC)
                        +0x30 magnitude scale (1.0, or 10.0 when the other
                              party's type is 0/1/2; x2.5 for the second car)
                        +0x38 pending-impact counter, += 2, capped at 25
                        +0x3C active flag
  -> FUN_001839E0 / FUN_00183BD0 -> FUN_0014DDF0   per-track prop hits
                                 -> FUN_00150480   loose wheel vs world
FUN_00185360 -> FUN_0014D0F0 -> FUN_0014EEA0 (IMPACTWORL) / FUN_0014F130
FUN_00186030 -> FUN_0014D2C0                 heavy world impact pair
FUN_001121F0 (carcol racer-vs-racer)  -> FUN_00141700 -> FUN_00140480 SHUNT
FUN_00197680 / FUN_00197BE0 -> FUN_001989A0 (the slam scorer)
     param_4 == 0 -> FUN_00140610 SLAM______      (full slam)
     param_4 != 0 -> FUN_00140480 SHUNT_____      (light shunt)
   and it plays the sound TWICE, once through each car's audio context
   (0x00198AAD and 0x00198ABF / 0x00198A78 and 0x00198A8A)
FUN_0004C390 -> FUN_00140DF0 / FUN_00140EF0   boost gain / loss
FUN_000828F0 -> FUN_00141D20                  boost loop
FUN_00155FD0 -> FUN_00156300                  near-miss whoosh
FUN_0018A170 -> FUN_001516C0                  explosion
FUN_0014C880 -> FUN_00150B90 / FUN_00151B70   kerb / steam
FUN_0013EA20 -> FUN_0014B600 -> FUN_001521C0  scrape loops
```

All `[C]` (Ghidra call graph over `burnout3.elf`).

**Not resolved `[?]`:** the impulse the emitters receive arrives in the
collision-event record (`[ESI+0x20]` chain out of `FUN_00183BD0`); that field
was not traced back to a named physics quantity, so the harness's choice of
`B3CarContact.impact` for it is GLUE, not recovery. The magnitudes are at
least in the right family — the game's IMPACTNUDG window is 700..4000 and the
harness's independently-recovered traffic-wreck threshold is 4000.

**Not resolved `[?]`:** the HUD/announcer stings — `tdown`, `tdownsp`,
`swoosh`, `medal1/2`, `trophy`, `vertical`, `take` in
`sound\{elim,crash,roadrage,single}.awd`. None of them appears as a packed
base-40 constant anywhere in the image (a full 4-byte-aligned scan of every
segment finds no `TDOWN`/`SWOOSH`/… slot) and none appears as an ASCII
string, so the front-end/HUD sound path builds its ids some other way. Per
the evidence rule they are **left out of the module** rather than guessed at.
The only ASCII wave name in the whole image is `"crashescap"` at
`0x003AD240`.

---

## 6. What a CRASH sounds like

The playtest report was "a high engine REV plays during a crash; it should be
slow-motion and crunching". Everything below is what retail actually does.
`tools/validate_sfx.py` §6 re-reads every instruction quoted here out of
`build/burnout3.elf` and runs the module's engine law head-to-head against the
real ported `FUN_00121560`.

### 6.1 The engine is STARVED, not muted — `[C]`

There is no fade, no duck and no stop on the crashing car's engine voice.
What happens is upstream of the audio module entirely: the vehicle main path
`FUN_0011BE50` has a **crashed branch**, and it does not run the engine the
way the racing path does.

```
0011BE75  MOV  AL,[EBX+0x210]          ; the crashed byte
0011BE7B  TEST AL,AL
0011BE7D  JZ   0x0011BF0C              ; not crashed -> the racing path
0011BE83  MOVSS XMM0,[EBX+0x1530] ; += dt        the crash clock
0011BE8B  MOV  EAX,[EBX+0x14C8]         ; gear
0011BEA0  JZ   0x0011BEC6               ; already neutral?
0011BEA2  MOVSS XMM0,[0x0039B2B0]       ; 0.35
0011BEAA  MOV  [EBX+0x14C8],0           ; gear    -> NEUTRAL
0011BEB4  MOV  [EBX+0x14A4],1           ; shifting = 1
0011BEBE  MOVSS [EBX+0x14A0],XMM0       ; shift timer = 0.35 s
0011BEC6  PUSH 0 / PUSH 0 / PUSH 0      ; throttle, wheel omega, kick
0011BECC  LEA  ESI,[EBX+0x1448]         ; the transmission block
0011BED2  XOR  EDI,EDI                  ; boost input = 0
0011BED4  CALL FUN_00121560             ; the engine/transmission update
```

So for the whole crash the engine update runs **in neutral at zero throttle,
every frame**, and the racing path's engine/AI block (`FUN_00104A90`
@`0x00104B6A`, gated on the same `veh+0x210`) and the drive-torque path never
touch it. In `FUN_00121560`'s neutral branch the target is
`rev_limit * throttle = 0` and the down-slew is **16.0 rad/s per call** (9.6
for an engine whose max rpm is below 6000), with the idle floor catching it at
`idle + U(0, 16 rad/s)`. From 5800 rpm that is 31 frames — **about half a
second** — to idle, and it stays there.

Two consequences worth stating explicitly:

* the rates are per CALL, and the frame rate does not change under time
  dilation (only `DAT_0060EA1C` does), so the coast-down takes the same half
  second of WALL time whatever the crash divisor is;
* the in-shift flag `veh+0x14A4` is also what silences the rev emitter:
  `FUN_0013E640` @`0x0013E663` zeroes its rpm term when
  `veh+0x1400 (throttle) <= 0 || veh+0x14A4 != 0`.

The audio mix state machine confirms the absence of a duck. `FUN_0013F610`
ORs every car's crashed byte into **bit 3** of the audio state word
(`0x0013F700`, stride `0x4AD0` over the vehicle array) and, on any change,
`FUN_0013F840` re-solves 15 group volumes from the 12 x 15 x 12-byte table at
`0x004191B0` (per entry: value, priority float, override byte; the winner is
the *lowest* value unless some active state marks an override, then the
*highest*), fading at 0.1. The crash row raises group 2 to 1.1 as an override
and leaves everything else at 1.0 — i.e. **nothing is ducked when a car
crashes**. `[C]`

### 6.2 The crunches — `[C]`

`FUN_00185360` (the chassis contact handler) hands every world contact to the
router `FUN_0014D0F0`, which switches on the same crashed byte:

```
0014D17B  MOV   AL,[ESI+0x210]                 ; crashed?
0014D223  DIVSS XMM0,[ESI+0x1F0]               ; impulse / MASS  -> m/s
0014D248  MULSS XMM0,[0x003B1684]              ; x0.5, crash MODE only
0014D23C / 0014D25C  CALL FUN_0014F130         ; crashed  -> IMPACTFATA
0014D293 / 0014D2A7  CALL FUN_0014EEA0         ; otherwise-> IMPACTWORL
```

The division by the mass (`veh+0x1F0`) is the piece that makes the numbers
line up: what the emitter receives is a **velocity change in m/s**, and
`FUN_0014F130`'s recovered window is exactly 2..10 (§4), gain 0.8..1.0, pitch
0.9..1.3 — a much heavier crunch than `IMPACTWORL`'s 6..22 window at gain
0.2..0.6. A crashing car's contacts are therefore all big crunches, and the
`x0.5` is applied only in the crash-mode game modes (`ctx+0x1920 != 0`), not
in a race.

### 6.3 Slow motion — `[C]` for the mechanism

Nothing pitches the effects down. The audio's own gates read the game clock
`DAT_0060EA20`, which is the **dilated** clock (RE_TAKEDOWN_FX 1.1), so every
crash-side timer — e.g. the 1.0 s crash-stream re-trigger gate at
`0x0014D1D8` — stretches with the divisor for free, while the voices keep
playing at their own sample rate.

### 6.4 The crash STREAM — recovered, deliberately not ported `[C]`

`FUN_0014B600` builds `"tracks\crash%d.rws"` (`0x0014C27B`) with
`(counter % 0x14) + 1`, i.e. it cycles the 20 files `Tracks/crash1.rws` ..
`crash20.rws`, and `FUN_00150D40` starts it from the router's crashed branch
(`0x0014D20B`) when the crash is within 50 units and `DAT_0073A1C0 <= 1`,
latching `+0x8E0` so it fires once per crash. Each of those files holds **two
30 s stereo 32 kHz streams** — `aGenCrashNN` and `zSloCrashNN` (extracted to
`build/audio/rws_crashNN/`): the normal and the SLOW-MOTION crash bed. That
pair is retail's actual "slow-mo crash sound".

It is not ported here because it is a music-domain stream (a second streaming
voice and ~7.7 MB per crash pair) and `src/burnout3_music.c` belongs to
another module. Which of the two sub-streams plays when, and how they are
crossfaded, is **`[?]`** — `FUN_001CBCA0` only sets 5.1 mix-bin volumes
(0.71 / 0.71 / 0.6 / 0.6 / 0 / 0.35) on the stream's voices.

---

## 7. What the port does and does not claim

`src/burnout3_sfx.c` carries the table above verbatim, implements §2's law
exactly (including the silence gate, the `x0.5` panel-prop scale, the `+/-10%`
pitch variance and the per-event frame cooldowns), and picks a variant from
the `<base><index><count>` family.

Its **mixer is GLUE**: 24 voices, linear-interpolated resample to 44100 Hz,
and the emitters' own 15/50-unit distance roll-off. The game's real voice
manager (`0x001CD8D0`, `RwaVoice`, `0x001CD0D0`'s seven-parameter tail) is a
3D DirectSound path with no meaning off the Xbox and is not reproduced.

Everything marked `[S]` in §4 is a **level**, never a wave choice or a
threshold: those are all `[C]`.

---

## 8. Reproduce

```bash
python3 tools/emulate_sfx_params.py     # FUN_0014A710 under Unicorn: the
                                        # tuple block + 60 ValueDB keys
python3 tools/emulate_sfx.py            # each emitter executed, its
                                        # PlaySound3D call captured
python3 tools/validate_sfx.py           # the differential suite: 243/243
```

`tools/validate_sfx.py` asserts, without trusting the C table:

1. every event's base-40 name is referenced by a `.text` immediate **inside
   that event's own emitter**, and decodes back to the same name;
2. the file list is exactly the `<base><index><count>` family the modulo-1600
   lookup implies, and every file exists under `build/audio`;
3. the six tuple floats equal, bit-for-bit, what the executed
   `FUN_0014A710` leaves in the object;
4. the gain the executed emitter hands to `PlaySound3D` equals
   `b3_sfx_resolve()`'s at five impulses per event, and both are silent below
   the minimum;
5. the emitter's playback rate divided by the C lerp lands inside
   `FUN_0014A6B0`'s `+/-10%` band with the real LCG scale seeded, and is
   exactly 1.0 for the emitters that never call it;
6. §6's crash laws: the nine crashed-branch instructions of `FUN_0011BE50`
   (including `0x0039B2B0 == 0.35` and the `CALL FUN_00121560` target) and
   the eight router instructions of `FUN_0014D0F0` (including
   `DIVSS [ESI+0x1F0]` and `0x003B1684 == 0.5`) are read back out of the
   image byte for byte; the module's crash-engine coast-down is run
   head-to-head against the REAL ported `FUN_00121560` driven exactly as the
   crashed branch drives it (`tools/crash_engine_drv.c`); and the router is
   fired through the module itself, which must pick `IMPACTWORL` when the car
   is not crashed and `IMPACTFATA` when it is.
