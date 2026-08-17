# Takedown presentation FX — time dilation, the takedown camera, the callout

Recovered 2026-08-11 from the analysed `burnout3.elf` (corrected VAs;
`.text` = old flat address + 0x10000).

Evidence marks:

* **[C]** — confirmed by executing the REAL x86 under Unicorn
  (`tools/emulate_tdfx.py` / `tools/emulate_tdfx_camera.py`) with a green
  differential case in `tools/validate_takedown.py` (**960/960**), or by
  reading bytes out of the retail image / retail data file.
* **[S]** — read from the instructions, self-consistent, no green case.
* **[?]** — open.
* **GLUE** — this harness's own, not from the binary.

Ported module: `src/burnout3_takedown.c/.h`.
Oracles: `tools/emulate_tdfx.py`, `tools/emulate_tdfx_camera.py`.
Acceptance: `tools/validate_takedown.py`.

---

## 1. Time dilation — there is no float "timescale" [C]

The game has **no** timescale multiplier. Time is produced by a frame-tick
timer object and dilation is an **integer divisor** on it.

### 1.1 The frame timer object at `DAT_0060EA00`

`FUN_001B5AC0` (thiscall, ECX = timer) is the tick; `FUN_001B5B60` (ECX =
timer) is the divisor-change handler it calls when the current and
requested divisors differ. Field map, proved by executing both functions:

| offset | global | meaning |
|---|---|---|
| +0x00 | — | last raw frame-counter value seen |
| +0x04 | — | previous `(counter − base)` |
| +0x08 | — | accumulator used while the running flag is 0 |
| +0x0C | — | accumulated whole ticks at the current rate |
| +0x10 | — | sub-tick remainder |
| +0x14 | — | counter origin |
| **+0x18** | `DAT_0060EA18` | **current time divisor** |
| **+0x1C** | `DAT_0060EA1C` | **per-frame dt** (the `dt` used everywhere) |
| **+0x20** | `DAT_0060EA20` | **game clock** (the race/score clock source) |
| **+0x24** | `DAT_0060EA24` | **requested divisor** |
| +0x28 | `DAT_0060EA28` | running flag |

Inputs:

* `DAT_004A1EB4` — a raw **frame counter**, incremented once per rendered
  frame in the main loop (`INC ECX; MOV ds:0x4A1EB4,ECX` at `0x000166D1`,
  `0x0001673E`, `0x00016880`, immediately followed by
  `MOV ECX,0x60EA00; CALL FUN_001B5AC0`). [C-disasm]
* `DAT_0049C120` — the nominal frame period, selected once at `0x00015B4F`:
  `0.0166667` (`0x003B1838`, NTSC 60 Hz) or `0.02` (`0x003B1A08`, PAL
  50 Hz); the companion int `DAT_0049C11C` gets 16 / 20 (ms). [C]

The whole of the dilation is one line inside `FUN_001B5B60`:

```
timer+0x1C = DAT_0049C120 / (float)timer+0x24      ; dt   = period / divisor
timer+0x18 = timer+0x24                            ; divisor := requested
```

and the clock accumulates at the same rate (`FUN_001B5AC0`'s tail:
`clock = (period/divisor)*rem + whole*period`, with `whole += delta/divisor`
and `rem += delta % divisor`).

**Executed proof** (`tools/emulate_tdfx.py timer`, asserted by
validate_takedown §1): with period 1/60 and divisor 5 the real code
produces `dt = 0.003333333` and advances the clock by exactly 1/300 s per
frame — five dilated frames advance the clock by one normal frame.

The rescale also rebases the remainder [C]:

```
if requested == 1:  if (divisor/2 < rem) whole += 1 ;  rem = 0
else:               rem = (rem*requested - 1 + divisor) / divisor
```

(6 differential cases, including the `rem 3` round-up vs `rem 2` no-op on
5 → 1.)

### 1.2 Every divisor writer in the retail code

`DAT_0060EA24` is written at exactly 14 sites. All of them, with what they
mean:

| address | value | in | meaning |
|---|---|---|---|
| `0x00027959` | **5** | `FUN_00027920` | takedown cinematic entry [C] |
| `0x00027BCD` | 5 / 1 | `FUN_00027AD0` | cinematic phase change at t = 2.8 s [C] |
| `0x000279FD` | 1 | `FUN_000279C0` | cinematic exit [C] |
| `0x00025D5C` | **5** | `FUN_00025CC0` | the local player's own car crashes (also latches `DAT_00649B9E`) [S] |
| `0x0002655B` | **6** | `FUN_00026050` | 0.35 s impact hit (below) [S] |
| `0x00026525` | 1 | `FUN_00026050` | impact hit expiry [S] |
| `0x00026695`, `0x0002678D`, `0x000269FD` | 1 | race manager | restores [S] |
| `0x001188A4` | **5** | `FUN_00118410` | **race aftertouch** — boost button held while crashed [C-disasm] |
| `0x0011888E` | **3** | `FUN_00118410` | crash-mode aftertouch [C-disasm] |
| `0x00118986` | **4** | `FUN_00118410` | crash-mode after a crashbreaker press [C-disasm] |
| `0x001188D6` | 1 | `FUN_00118410` | aftertouch release [C-disasm] |
| `0x00119C24` | 1 | crash-mode exit | restore [C-disasm] |

So the dilation ladder the retail game actually uses is
**1 (normal) / 3 / 4 / 5 / 6**, i.e. full, 1/3, 1/4, 1/5 and 1/6 speed.

### 1.3 The audio companion [C-disasm]

Every site that requests a divisor > 1 also writes the global rate scale
`DAT_003EBFD0` (default `1.0`, `0x003B168C`) to **0.75**
(`DAT_003A55F8`), and every restore writes it back to `1.0`. It is
consumed by the sound update at `0x00016E22` (pushed alongside the real
clock `DAT_004AE200` and `frames × real dt`). So the pitch does **not**
track 1/N — it is a fixed 0.75 while any dilation is active.

### 1.4 "Impact Time" / aftertouch — the producer of the request [C-disasm]

`FUN_00118410` is the crashed-path input shaper (only reachable on the
byte `v+0x210` crashed path, RE_NOTES §14). Its head classifies the
context and its tail requests the divisor:

```
A  = FUN_00017310()                     ; crash-party (Crash junction) mode
B  = in crash mode: (pad & 4) or DAT_004AE1DC, gated by 2.0 > v+0x1530,
     plus a forced-on window of 0.9 s [0x003A69C0] after v+0x3A74 is stamped
0x0011885F: DAT_0073A1C0 must be 1      ; single local player only
if (A)  { if (!B) release;  divisor = (v+0x3A74 >= 0) ? 4 : 3 }
else    { if (pad & 4) divisor = 5;  else release }
release: if (v+0x4AC7) { divisor = 1; pitch = 1.0; v+0x4AC7 = 0 }
engage : pitch = 0.75; v+0x4AC7 = 1
```

`pad & 4` is the same bit as the vehicle's boost input `v+0x13FC & 4`
(RE_NOTES §14). So **in a race, "Impact Time" is: your car is crashed and
you hold boost → the whole world runs at 1/5 speed** while you steer the
wreck. `v+0x3A74` is stamped with the game clock at `0x00118919` when the
crashbreaker button (`pad & 1`) is pressed, and expires back to −1.0 after
0.9 s.

The **pad-input → wreck-motion producer** (racecar `+0x1A10` angular
velocity, `+0x1A30` position delta, `+0x1A50` rotation delta — the
consumer side is RE_NOTES §16) is still **[?]**: it writes through derived
pointers in the crash-director object. What is now closed is that the
*slow-motion gate* is the boost bit, and that it lives here.

### 1.5 The 0.35 s impact hit [S]

`FUN_00026050` (the per-frame race-manager state machine; ECX = the race
director, calls `FUN_00027700`) carries a second, much shorter dilation on
`director+0x40..+0x52`:

```
arm  (0x00026B18):  director+0x44 = clock + 0.05 [0x003A69BC];  +0x52 = 1
tick (0x00026500):  el = clock - +0x44
                    if (el > 0.35 [0x0039B2B0]) { divisor=1; pitch=1.0;
                                                  +0x44 = -1; +0x52 = 0 }
                    else if (el > 0.0)          { divisor=6; pitch=0.75 }
```

so: a 0.05 s delay, then **0.35 s at 1/6 speed**. It is armed from the
big-hit handler at `0x00026AC5` (which also sets `v+0x1353 |= 2`,
`v+0x153F = 1`, `v+0x1538 = clock` and fires two `FUN_00125100` effects),
gated on the event flags `+0x174 & 8` and `!(+0x174 & 2)`. The exact
event that raises those flags is **[?]**.

---

## 2. The takedown cinematic [C]

### 2.1 Chain of custody

```
takedown commit  FUN_00198E60  (RE_GAMEPLAY §6)
  -> FUN_001994D0  BP award
  -> FUN_000273F0  boost-bar tier upgrade
       -> FUN_000278B0  (thiscall, ECX = race director,
                         [esp+8] = victim racecar, [esp+0xC] = attacker)
            gates: attacker+0x1920 == 0 (human)         0x000278C4
                   victim != 0                          0x000278CE
                   DAT_004AE1DB (global enable byte)    0x000278D9
                   director->vtbl[+0x104](attacker,victim)  0x000278E8
            record = director + 0x20 + (attacker+0x27D0)*0x10   0x000278F2
            -> FUN_00027A60  trigger gate
            -> FUN_00027920  entry
per frame: FUN_00026D30 -> FUN_00027700 -> FUN_00027AD0 x2 records
exit:      FUN_000279C0
```

`FUN_00027700` runs the update over **two** 0x10-byte records at
`director+0x20` and `director+0x30` — one per local player.

### 2.2 The camera record (0x10 bytes) [C]

| offset | meaning |
|---|---|
| +0x00 | **the VICTIM racecar** (copied from attacker `+0x15A4`) |
| +0x04 | elapsed seconds since entry (−1.0 when idle) |
| +0x08 | owner racecar slot / local-player index |
| +0x0C | armed flag, cleared at t ≥ 2.5 s |
| +0x0D | "callout already posted" dedup |
| +0x0E | cinematic active |
| +0x0F | copy of the attacker's burnout-chain flag `+0x11EE` |

`racecar+0x15A4` is "the car I most recently took down" — this is what
makes the shot the **victim's** crash, not the attacker's car.

### 2.3 The trigger gate — `FUN_00027A60` (EDX = record) [C, 8 cases]

```
me     = DAT_0073A1D0 + record[+0x08]*0x27E0
victim = me+0x15A4
return 1 iff  victim != 0
          &&  victim+0x18FA          (the victim is crashed)
          && !me+0x18FA              (I am not crashed)
          && !record+0x0E            (no cinematic already running)
          &&  victim != record+0x00  (not the same victim as last time)
          &&  0.0 <= victim+0x10DC - victim+0x140C <= 1.5   [0x003B1870]
```

i.e. the camera only takes over within **1.5 s of the victim's crash
starting**.

### 2.4 Entry — `FUN_00027920` (ESI = record) [C, 4 cases]

```
record+0x00 = me+0x15A4 ; +0x0E = 1 ; +0x0C = 1 ; +0x04 = 0.0 ; +0x0D = 0
record+0x0F = me+0x11EE
DAT_0060EA24 = 5                                    ; 0x00027959
hud[me] -> vtbl[+0x0C]( (me+0x1320 == -1) ? 9 : 11, &victim+0x19BC )
FUN_00053D20(state = 3)                             ; EDI = 0x54F900 + i*0x70
```

`me+0x1320 == -1` selects the plain vs **signature-takedown** callout id
(event 9 vs 11); the argument is the **victim's grid slot** (`+0x19BC`).
Re-entry with `+0x0E` already set is a no-op (verified: no events posted).

### 2.5 Per-frame update — `FUN_00027AD0` (ESI = record) [C, 5 scenarios × 288 frames]

```
t = record+0x04 + DAT_004AE1FC      ; REAL (undilated) dt -- 0x00027ADA
if (me+0x134C == 3 || me+0x18FA)  -> FUN_000279C0 (abort)
if (t >= 4.8  [0x00385A00])       -> FUN_000279C0 (normal end)
slow = (t < 2.8 [0x00395E88])
if (camera_on && !slow && victim_vehicle+0x1524 == 4) +0x1524 = 0
FUN_0018CB60(me, slow)             ; me+0x27D8 = slow (edge-triggered)
DAT_0060EA24 = slow ? 5 : 1        ; 0x00027BCD  (= (slow!=0)*4 + 1)
if (!record+0x0D && t >= 2.5 [0x003A2D50] && victim_slot != -1) {
      hud[me] -> vtbl[+0x0C](10, &victim_slot)
      FUN_00053D20(state = 1)
      record+0x0D = 1 }
if (record+0x0C && t >= 2.5 && victim_slot != -1) record+0x0C = 0
if (slow) { if (record+0x0F && !me+0x11F1) me+0x2417 = 1 ;  me+0x245D = 1 }
else      { attacker_vehicle+0x1353 |= 0x18 }        ; 0x00027C9D
```

`FUN_0018CB60(racecar, on)` is the camera hand-over itself: it stores the
flag in `racecar+0x27D8`, and on the rising edge zeroes `racecar+0x19D0..
+0x19DC` (a vec4 inside the crash handoff block of RE_NOTES §16) and calls
the camera-object reset `FUN_00179760`; on the falling edge it restores
the vehicle's steering authority `+0x1534 = 1.0` and clears the cinematic
drift state `+0x1524 == 4 -> 0`.

### 2.6 Exit — `FUN_000279C0` (EDX = record) [C, 3 cases]

```
me+0x245D = 0
if (me+0x27D8) { me+0x27D8 = 0 ; pv+0x1534 = 1.0 ; if (pv+0x1524==4) = 0 }
if (DAT_0064B38C[me+0x19BC]+0x1524 == 4) = 0
DAT_0060EA24 = 1
record+0x0C = 0 ; +0x0E = 0 ; +0x00 = 0 ; +0x04 = -1.0
```

### 2.7 The recovered timeline

Executed frame-by-frame at a real dt of 1/60 (validate_takedown §4):

| t (real seconds) | what happens | divisor | source |
|---|---|---|---|
| 0.000 | camera takes the view, targets the victim; HUD event 9/11; state 3 | **5** | `FUN_00027920` |
| 2.500 | HUD event 10 (the "TAKEDOWN!" beat); state 1 | 5 | `0x003A2D50` |
| 2.800 | slow-motion ends, camera released, attacker vehicle `+0x1353 |= 0x18` | **1** | `0x00395E88` |
| 4.800 | cinematic exits, record cleared | 1 | `0x00385A00` |

**288 frames at 60 Hz**, asserted. The clock is the *real* dt
`DAT_004AE1FC`, so the cinematic is 4.8 wall-clock seconds even though the
simulation inside it is running at 1/5 speed.

Early-outs (both verified): the attacker crashing (`+0x18FA`) and the race
reaching state 3 (`+0x134C`) both exit immediately and restore divisor 1.

---

## 3. Camera parameters [C]

`Camera.cfg` has two registrars. Both were executed under Unicorn with the
CRC core at `0x001AF250` hooked, and **every one of the 34 keys the shake
registrar produces resolves in the retail `Data/vdb.xml`** — which is the
acceptance proof that the keys are right (`tools/emulate_tdfx_camera.py`;
same pipeline as the per-car physics, RE_NOTES §10).

* **`FUN_00160840`** — `Camera/Follow/`, `Camera/Look Back/`,
  `Camera/Bumper/`: `Camera Offset`, `Focus Offset`, `Spring Coeff`,
  `Down Angle`. **None of these six has a VDB override**, so the shipped
  values are the compiled-in defaults; the per-parameter storage offsets
  are **[?]** (the vector params' storage pointer goes through the
  allocator's heap, which this run does not track).
* **`FUN_00160B90`** — the camera-shake spring system, cfg path
  `"../Export/ValueDB/Camera/Camera.cfg"` (string at `0x003AF460`,
  group prefix at `0x003A15DA`). Retail values:

| group | Spring Coeff | Damping | Force |
|---|---|---|---|
| **Crash/HandyCam** | 15, 15, 15 | 10, 10, 10 | — (`Safe FOV Horiz` 0.1, `Safe FOV Vert` 0.5) |
| InGame/Effect/Burnout/Position | 150, 150, 10 | 10, 10, 10 | 0, 0, −500 |
| InGame/Effect/Burnout/Angular | 150, 150, 300 | 1, 1, 10 | 0, 0, 10000 |
| InGame/Effect/Shunt/Position | 15, 100, 10 | 10, 15, 10 | 0, 400, 600 |
| InGame/Effect/Shunt/Angular | 15, 500, 15 | 10, 10, 10 | 2000, 0, 0 |
| InGame/Effect/Shunted/Position | 15, 15, 15 | 10, 10, 10 | 0, 0, −20 |
| InGame/Effect/Shunted/Angular | 15, 15, 200 | 10, 10, 10 | 0, 0, 10000 |
| **InGame/Effect/Slam/Position** | 15, 15, 15 | 10, 10, 10 | 0, 0, 0 |
| **InGame/Effect/Slam/Angular** | 15, 15, 200 | 10, 10, 10 | 0, 0, **10000** |
| InGame/Effect/Shake/Position | 300, 30, 50 | 20, 10, 40 | 0, 0, 0 |
| InGame/Effect/Shake/Angular | 500, 15, 150 | 50, 10, 10 | 2000, 0, 0 |

("Slam" = you slammed someone, "Shunted" = you were slammed. Both fire a
10000 angular impulse about the third axis against a 200 spring and 10
damping — the camera roll-kick on a hit.) The consumer of these springs is
`FUN_0018DA00`, the per-frame slam/aftertouch camera-wobble writer gated
by the slam clocks `racecar+0x1598`/`+0x1690` (RE_NOTES §14, item 12 —
ablation-verified to have zero effect on the trajectory, i.e. it is purely
presentational).

**RETRACTED (see section 8.6).** Two claims in the paragraph that used to
close this section are wrong:

* *"the consumer of these springs is `FUN_0018DA00`"* — no. **Nothing**
  consumes the Crash/HandyCam group. The block lives at
  `DAT_0047A134 + 0x2C30` and exactly two instructions in the whole image
  address it: the constructor `LEA` at `0x00167273` that hands it to the
  registrar, and the destructor `ADD` at `0x001AAA64` that hands it to the
  unregistrar. It is registered, VDB-overridable **dead config** in this
  build (validate_takedown §10d asserts the negative).
* *"`FUN_00179760` sets `racecar+0x1D8`…"* — the offsets are right but the
  object is not the racecar: `FUN_0018CB60` does `ADD EAX,0x1A00` before
  jumping to it (`0x0018CB7C`), so the target is **`racecar + 0x1A00`**,
  which the constructor `FUN_001705F0` proves is an embedded object
  (`0x00170660: MOV [ESI+0x1A04], ESI` — its owner back-pointer).

The real camera is a separate 20-mode director; it is section 8.

---

## 4. The callout chain [C]

**Correction to a first reading.** `FUN_00027920` / `FUN_00027AD0` do call
`DAT_004CFB20[i]->vtable[+0x0C]` with ids 9 / 11 (entry) and 10 (t ≥ 2.5 s),
and those instructions are real — but that object's listener pointer
`+0x04` is set to 0 by its Init (`FUN_001B4170` @ `0x001B4191`) and **no
writer of a non-zero value exists anywhere in the image**, so
`FUN_001B4230` (the method at `vtable+0x0C`, `0x001B4230`) returns
immediately. In retail those posts are **inert** [S]. The module still
surfaces them, because they are real and ordered and make good phase
markers, but they are not the callout.

The callout is a separate pipeline that starts at the **same commit**:

```
FUN_00198E60 commit -> FUN_001994D0 BP award  (also SELECTS the message)
   -> FUN_00199350 "PostHudCallout"  (ESI = callout slot, EDI = message id)
        -> FUN_00054700  (drawer/spawner)
             -> FUN_00055C90  (element build, phase functions)
                  -> FUN_00056120  (3-phase animation, per frame)
```

So the sign appears **immediately with the slow-motion**, and runs on its
own 4.25 s clock — it is not tied to the cinematic's 2.5 s beat.

### 4.1 `FUN_00199350` — post + priority [C-disasm]

```
if (crash-party mode)  accept only 0xC8 <= id <= 0xD0     0x00199368
else if (id < slot+0x130) reject                          0x0019937F
if (id in {0x7F,0x80,0x81}) require racecar+0x18FA         0x001993EF
else { slot+0x11 = flagbyte; if (racecar+0x18FA && !flagbyte) reject }
slot+0x130 = id            ; the message id IS the priority
slot+0x134 = PlaySound(soundId, prev)
slot+0x138 = racecar+0x10DC   ; post timestamp (the dilated race clock)
slot+0x10  = 1
```

### 4.2 The message table — 206 × 4 B at `0x00389160` [C]

Record = `{ u8 message id, u8 sign kind, u16 Data/Globalus.bin string index }`
(searched linearly by `FUN_00054BF0`: `movzx edx,[eax*4+0x0038915C]`).
All 206 ids are distinct. The takedown family, with the **retail strings
decoded from `Data/Globalus.bin`** (3985 entries, `u32` offset table at
`+0x10`, UTF-16LE payloads; TOC end == first offset == `0x3E54`, verified):

| msg | str | text | msg | str | text |
|---|---|---|---|---|---|
| **0x93** | **2074** | **`TAKEDOWN!`** | 0xA3 | 2075 | `DOUBLE TAKEDOWN!` |
| 0x94..0x9C | 2079..2087 | `CAR` / `VAN` / `TRUCK` / `BIG RIG` / `BUS` / `L-TRAIN` / `TRAM` / `MONORAIL` / `TRAILER` `TAKEDOWN!` | 0xA4 | 2076 | `TRIPLE TAKEDOWN!` |
| 0x9D | 2089 | `WALL TAKEDOWN!` | 0xA5 | 2077 | `4-WAY TAKEDOWN!` |
| 0x9E | 2088 | `BONUS TAKEDOWN!` | 0xA6 | 2078 | `TOTAL TAKEDOWN!` |
| 0x9F..0xA2 | 2096..2099 | `TAKEDOWN 2-IN-A-ROW` / `3-IN-A-ROW` / `HOT STREAK` / `RAMPAGE!` | **0xA7** | **2094** | **`REVENGE!`** |
| 0xA8 | 2095 | `GRUDGE!` | 0xA9 | 2093 | `PSYCHE OUT!` |
| 0xAA..0xAE | 2102, 2075..2078 | `AFTERTOUCH TAKEDOWN!` then the multi ladder | 0xAF..0xC2 | 2090 | `SIGNATURE TAKEDOWN` ×20 |
| 0x35 / 0x36 | 2100 / 2101 | `TAKEDOWN DENIED!` / `LUCKY ESCAPE!` | 0x7F / 0x80 / 0x81 | 2103 / 2104 / 2105 | `CRASHED!` / `TAKEN OUT!` / `TAKEDOWN AVENGED!` (victim side) |

Note there is **no "TAKEDOWN SPREE" string** — the spree family is
`2-IN-A-ROW / 3-IN-A-ROW / HOT STREAK / RAMPAGE!`.

### 4.3 Kind → descriptor → sign texture [C]

`FUN_00054700` @ `0x0005494C`: `ecx = kind - 3; if (ecx > 0x14) desc = 0;
else jmp [ecx*4 + 0x00054ACC]`, each handler being `MOV ESI,<descriptor>`.
Read out of the image:

| kind | 3 | 4 | 5-8,14 | 9 | 10 | 11 | 12 | 13 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | 22 | 23 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| desc | 7 | **6** | 0 | 1 | 5 | 4 | 3 | 2 | 9 | 10 | 11 | 12 | 13 | 8 | 14 | 15 | 16 |

Descriptors are the 16-byte records at `0x003895E8`:
`{ u32 text param, u32 anim style, u32 sign index (17 = none), u32 flags }`.

* **descriptor 6 = `{1, 1, 3, 0x0D}`** — anim style 1, **sign index 3 =
  `hud_signs_td`**, flags bit 3 set (long hold). This is what every
  takedown message (kind 4) resolves to.
* descriptor 7 = `{4, 2, 1, 0x0D}` — the victim-side skull
  (`hud_sign_skull`), anim style 2.

Sign index → texture is the 17-entry table `FUN_0004DD00` binds by name
into `DAT_004608F0[]` from the pointer table at `0x00388560`:

| idx | texture | idx | texture |
|---|---|---|---|
| 0 | `hud_sign_flame` | 9 | `HUD_crash_score02` |
| 1 | `hud_sign_skull` | 10 | `HUD_crash_score03` |
| 2 | `hud_award_star` | 11 | `HUD_crash_x2` |
| **3** | **`hud_signs_td`** | 12 | `HUD_crash_x4` |
| 4 | `MedalGold` | 13 | `HUD_crash_breaker` |
| 5 | `MedalSilver` | 14 | `HUD_crash_skull` |
| 6 | `MedalBronze` | 15 | `HUD_crash_stealer` |
| 7 | `HUD_crash_boost` | 16 | `Bad` |
| 8 | `HUD_crash_score01` | 17 | (sentinel: no sign) |

### 4.4 On-screen duration [C for the constants, S for the phase walk]

`FUN_00055C90` builds the element with three phase functions at
`[el+0x158/0x15C/0x160]`, a lifetime at `[el+0x154]` and an elapsed
accumulator at `[el+0x150]`; `FUN_00056120` walks them.

Anim style 1 (`0x00056059`..`0x000560C5`):

```
[el+0x158] = FUN_00056300   ; slam-in, ends at elapsed >= DAT_0054FB90
[el+0x15C] = FUN_00056550   ; hold,    ends at elapsed >= [el+0x154]
[el+0x160] = FUN_00056570   ; fly-out, ends at elapsed-[el+0x154] >= DAT_0054FB84
[el+0x154] = DAT_0054FB90 + (flags & 8 ? 2.0 : 0.125)      ; 0x00056089
```

| symbol | address | value | set by |
|---|---|---|---|
| `DAT_0054FB90` | 0x0054FB90 | **1.5** | `FUN_002675A0` ← `0x003B1870` |
| `DAT_0054FB84` | 0x0054FB84 | **0.75** | `FUN_00267600` = `0x003B1684`(0.5) + `0x003B1730`(0.25) |
| hold, bit 3 set | `0x003B1688` | **2.0** | literal |
| hold, bit 3 clear | `0x003B1728` | **0.125** | literal |
| `DAT_0054FB50` (anim 2 in) | 0x0054FB50 | **0.75** | `FUN_00267640` ← `0x003A55F8` |
| anim-2 hold, bit 3 set | `0x003B1F34` | **2.25** | literal |
| anim-2 hold/out | `0x003B168C` | **1.0** | literal |

**So the TAKEDOWN callout (descriptor 6, style 1, flags 0x0D):**

| phase | length | ends at |
|---|---|---|
| slam-in + spin | 1.5 s | 1.5 |
| hold | 2.0 s | **3.5** (`[el+0x154]`) |
| fly-out | 0.75 s | **4.25** |

**Total on screen ≈ 4.25 s.** The victim-side `TAKEDOWN AVENGED!`
(descriptor 7, style 2) is 0.75 + 2.25 + 1.0 ≈ 4.0 s.

*(This supersedes an earlier "2.3 s, inferred from the cinematic bounds"
reading in this document — the callout has its own lifetime and is not
bounded by the cinematic.)*

The dt the element's `+0x150` accumulator is fed with was not identified
**[?]**; the port uses the real frame delta, matching the cinematic's timer.

### 4.5 Revenge [C]

* Commit (`0x00198F28`..`0x00198F5B`): if
  `racecar + 0x1689 + victim_grid_slot` is set, clear it and set
  `racecar+0x168F = 1`; otherwise set the victim's own
  `0x1689 + attacker_slot`.
* Award (`FUN_001994D0` @ `0x00199529`) snapshots that byte **before** the
  commit clears it, and at `0x00199973`..`0x001999A5` selects
  `msg 0xA7 = REVENGE! (2094)`.
* The victim gets `msg 0x81 = TAKEDOWN AVENGED! (2105)`, which
  `FUN_00199350` only accepts while `racecar+0x18FA` is set.

### 4.6 The presentation-state broadcast `FUN_00053D20` [C]

`__usercall(EDI = state object at 0x0054F900 + player*0x70, EAX = new
state, XMM0 = transition time)`. Fields: `+0x00` transition timer, `+0x04`
current, `+0x08` previous, `+0x0C` pending (6 = none), `+0x10` racecar
index, `+0x68` listener list. Rules: same-state returns; a running
transition defers into `+0x0C`; **any state other than 4 is refused while
`racecar+0x134C == 3`**; the default transition time is `0.4`
(`0x003B16E8`); the broadcast mask is `DAT_00388F78[state] =
{0,1,2,4,8,0x0F}` (verified byte-for-byte).

| state | mask | posted at | meaning |
|---|---|---|---|
| 1 | 1 | `0x00027C1D` (cinematic exit), `0x0005363A` (global reset), `0x0018C927` (crash recovery) | normal / driving [C] |
| 2 | 2 | `0x0018C812`, in the same block that sets `racecar+0x18FA` | crash [C] |
| 3 | 4 | `0x000279B4`, immediately after the cinematic entry | takedown cinematic [C] |
| 4 | 8 | `0x0018C6B7`, guarded by `racecar+0x134C == 3` | race over [C gating, S name] |
| 0, 5 | 0, 0x0F | never posted directly | [?] |

---

## 5. What the port implements

`src/burnout3_takedown.c`:

* `b3_tdfx_timer_tick` / `b3_tdfx_timer_rescale` — `FUN_001B5AC0` /
  `FUN_001B5B60`, field-for-field [C].
* `b3_td_cam_gate` / `_enter` / `_update` / `_exit` — `FUN_00027A60` /
  `FUN_00027920` / `FUN_00027AD0` / `FUN_000279C0`, 1:1 [C].
* `b3_tdfx_post_callout` / `b3_tdfx_message_info` / `b3_tdfx_callout` —
  `FUN_00199350`'s accept test and the message/descriptor tables, with the
  3-phase schedule of §4.4 [C for the tables and constants].
  `b3_tdfx_select_takedown_message` is the message chooser [S].
* A harness-facing singleton: `b3_tdfx_update(real_dt)` returns the sim dt
  (real_dt / divisor), `b3_tdfx_on_takedown(...)` runs the gate + entry,
  `b3_tdfx_status()` publishes the slow-mo/callout state, and
  `b3_tdfx_camera(...)` frames the victim. Marked GLUE where it is not a
  port: the camera eye law, the game-mode virtual at vtable `+0x104`
  (treated as "allowed"), and the choice to scale a *variable* real dt by
  1/divisor (the console runs a fixed frame tick, so its dt *is*
  period/divisor).
* Aftertouch (`b3_tdfx_set_aftertouch`) and the impact hit
  (`b3_tdfx_impact_hit`) request divisors 5 and 6 with the recovered
  windows; the priority order between the three (cinematic > impact hit >
  aftertouch) is GLUE — the retail code has one global request slot and
  relies on the modes being mutually exclusive.

## 6. Verification inventory — `tools/validate_takedown.py` (960/960)

| section | function(s) executed | checks |
|---|---|---|
| 1 time dilation | `FUN_001B5AC0`, `FUN_001B5B60` | 18-frame divisor schedule (1/5/6/3/4), dt + clock per frame, 6 rescale cases, the "5 dilated frames == 1 normal frame" identity |
| 2 trigger gate | `FUN_00027A60` | 9 seedings (crashed / not, camera up, ±1.5 s window, dedup, no victim) |
| 3 entry | `FUN_00027920` | record fields, divisor request, HUD event ids 9/11, state 3, re-entry no-op |
| 4 update | `FUN_00027AD0` | 5 scenarios × every frame to 4.8 s: 11 fields + the posted events compared frame-by-frame; the 288-frame length and the 2.5/2.8 boundaries asserted |
| 5 exit | `FUN_000279C0` | 3 seedings × 9 fields |
| 6 constants | image bytes | 11 timeline constants + the header macros carrying them |
| 7 callout art | image bytes | all 17 sign names at `0x00388560`, takedown == index 3 |
| 8 camera params | registrar emulation + retail `Data/vdb.xml` | 8 spot values, all 34 keys resolve |
| 11 crash director | **the whole `FUN_0015E550` executed** + image bytes | the camera law over 7 seeded wreck attitudes (eye / quat / FOV / pitch / yaw per frame), the "messages 4 and 5 are the dispatcher's default" negative, `FUN_00018B90`'s and `FUN_0018E050`'s shapes, all 10 dilation writers at their literal addresses, and the three recovered timelines (impact-truncated, sustained-to-recovery, aftertouch) |
| 10 camera | `FUN_0015E550`'s smoothing span executed + image bytes | the 20-mode table and every mode's vtable/enter/update, the message dispatcher's two jump tables, the Camera/Follow geometry + FOV + law constants at their literal addresses and in the header, the Crash/HandyCam "no reader" negative, 30 executed (dt, speed) blend cases, and both ported cameras run finite |
| 9 callout | image bytes + retail `Data/Globalus.bin` | the 206-entry message table's distinctness, 31 message rows (kind + string index + the decoded retail text), 8 descriptors, all 21 kind→descriptor jump-table handlers, 7 animation constants, the derived 1.5/3.5/4.25 s schedule, and `FUN_00199350`'s priority + crashed gates |

---

## 8. The camera [C-disasm unless marked]

Recovered 2026-08-11. Every address below is in the corrected map
(`.text` = old flat address + 0x10000). Acceptance:
`tools/validate_takedown.py` section 10 (**624/624** overall).

### 8.1 What `racecar+0x27D8` actually is — a correction [C-disasm]

The inherited notes call `racecar+0x27D8` the "cam_active" flag. It is
not a camera flag. Its only per-frame consumer is `FUN_0018C510`
(`0x0018C53A`):

```
if (racecar+0x27D8) FUN_00170820(racecar)          ; 0x0018C54A
else              { FUN_0018D790(); FUN_001711B0(racecar); ... }
```

`FUN_00170820` is the **route-following auto-driver** (it advances the
racecar's route node `+0x18C4/+0x18C8`, calls the respawn-on-route
`FUN_001714F0` after 200 stuck frames, and ticks the navigator at
`racecar+0x1A00`). So `+0x27D8` means **"the AI has the wheel"** — which
is exactly consistent with `FUN_0018CB60` restoring the vehicle's
steering authority `+0x1534 = 1.0` when it clears the flag. The same
flag is raised on race-over at `0x0018C688` (guarded by
`racecar+0x134C == 3`, immediately before the state-4 broadcast at
`0x0018C6B7`).

`racecar+0x1A00` is therefore the **navigator**, not a camera:
`FUN_001705F0` builds it at `0x0017063F..0x0017067E` with
`[ESI+0x1A00]=ESI+0x1A00`, `[ESI+0x1A04]=ESI` (owner), `[ESI+0x1A08]`,
`[ESI+0x2150..0x2164]`, `[ESI+0x21A0]`. Its per-frame update is
`FUN_0016AAC0` -> `FUN_00175B10` -> `FUN_00176290` (route-choice
initialization and branch selector),
and `FUN_00179760` (what `FUN_0018CB60` calls) merely **resets** it:
`+0x1D8 = 0xFFFF` (force a route re-pick), `+0x296 = 1` (cut), then
`FUN_00176090` clears `+0x1F8..+0x25C`.

### 8.2 The camera director [C-disasm]

The camera is a separate C++ object, built by `FUN_001674B0` and
constructed by the vtable-store run at `0x00015900..0x00015A54`.

| field | meaning |
|---|---|
| `+0x250` | the shared **camera state** (vtable `0x003A9AEC`), size 0x50 |
| **`+0x260`** | **field of view, degrees — 90.0** (`0x003B1850`, stored at `0x001678B5`, re-stamped at `0x001679D8`) |
| `+0x2A0..+0x2EC` | the **20-entry camera-mode pointer table** |
| `+0x930/+0x980/+0x9D0/+0xA20` | four more camera states (blend slots), same vtable |
| `+0xA80..+0xAA4` | the mode-to-mode **blend** record (from-mode, to-mode, duration) |
| `+0xC20 / +0xC24` | the **64-bit active-mode bitmask** |
| `+0xC40` | current mode index |
| `+0xC44` | "modes are constructed" gate |
| `+0xC3C` | the player's selected view (2 or 5) |

Per-frame tick — `FUN_00167940(dt)`:

```
for (i = 0; i < 0x14; i++)
    if (mask & (1<<i))
        mode[i]->vtbl[+0x10]( director+0x250,
                              (i == 0 || i == 0x0E) ? DAT_004AE1FC : dt )
```

so **modes 0 and 14 are ticked with the REAL (undilated) dt** while every
other mode gets the dilated one — the same distinction the cinematic
timer makes (§2.5).

Switch — `FUN_00167CE0(index)`: deactivate every set bit (calling each
mode's `vtbl[+0x0C]`), call `mode[index]->vtbl[+0x08](director+0x250)`,
set the bit, `+0xC40 = index`, `+0xC46 = 1`.
Disable — `FUN_00167C90(index)`: `vtbl[+0x0C]`, clear the bit.

### 8.3 The 20 camera modes [C-disasm]

Read out of the constructor at `0x00015900`; `enter` = `vtbl+0x08`,
`update` = `vtbl+0x10`.

| idx | sub-object | vtable | enter | update | notes |
|---|---|---|---|---|---|
| 0 | `+0x070` | `0x003A9514` | `0x0015C070` | `0x0015C130` | free/debug fly cam; **real dt**; seeds itself from the vehicle matrix and stamps FOV 90 |
| 1 | `+0x2F0` | `0x003A94FC` | `0x0015F0A0` | `0x0015F270` | entered 1.5 s into replay state 2 (`0x00168462`) |
| **2** | `+0x370` | `0x003A9C28` | `0x0015E060` | **`0x0015E550`** | **the chase camera** — cfg = Camera/Follow (`+0x08` = cfg+0x60) |
| 3 | `+0x398` | `0x003A9C28` | `0x0015E060` | `0x0015E550` | chase, second local player |
| 4 | `+0x4C0` | `0x003A975C` | `0x0015DC60` | `0x0015DCB0` | entered by message 8 |
| 5 | `+0x3C0` | `0x003A975C` | `0x0015DC60` | `0x0015DCB0` | the other selectable view; cfg = cfg+0x30 (Camera/Bumper) |
| 6 | `+0x540` | `0x003A93D0` | `0x0015B270` | `0x0015B540` | |
| 7 | `+0x730` | `0x003A93D0` | `0x0015B270` | `0x0015B540` | replay takes the mask over exclusively (`[+0xC20] = 0x80`, `0x00165E12`) |
| 8 | `+0xB60` | `0x003A9AB8` | `0x0015F5E0` | `0x00160080` | |
| 9 | `+0xB90` | `0x003A975C` | `0x0015DC60` | `0x0015DCB0` | |
| 10 | `+0x920` | `0x003AA0C4` | `0x0015D170` | `0x0015D340` | the replay/highlight camera (`|= 0x400` at `0x001686B7`, `0x00165DE0`) |
| 11 | `+0xAB0` | `0x003A9384` | `0x0015DA10` | `0x0015DB50` | |
| 12 | `+0xABC` | `0x003A9384` | `0x0015DA10` | `0x0015DB50` | |
| 13 | `+0xBD0` | `0x003A93BC` | `0x00159750` | `0x00159A80` | the scripted camera-path camera (only mode with its own exit, `0x00159C00`); driven from the manager's loaded cut list at `mgr+0x990` |
| 14 | `+0xAD0` | `0x003A9514` | `0x0015C070` | `0x0015C130` | **real dt** |
| 15 | `+0x500` | `0x003AA118` | `0x0015B6D0` | `0x0015B750` | overlay, enabled by `FUN_0015B690` (`|= 0x8000`) |
| 16 | `+0x520` | `0x003AA118` | `0x0015B6D0` | `0x0015B750` | |
| 17 | `+0x400` | `0x003A975C` | `0x0015DC60` | `0x0015DCB0` | trackside, kind 5 (`[+0x42C]=5`) |
| 18 | `+0x440` | `0x003A975C` | `0x0015DC60` | `0x0015DCB0` | trackside, kind 6 |
| 19 | `+0x480` | `0x003A975C` | `0x0015DC60` | `0x0015DCB0` | trackside, kind 7 |

Modes 17/18/19 take a shared random index `0..11`
(`0x001693C6..0x001693E7`, then `FUN_00167CE0(0x11)`).

### 8.4 The camera-controller message dispatcher [C]

`0x001684AF`: `cmp eax,0x16; ja default; movzx eax,[eax+0x00168A20];
jmp [eax*4+0x001689EC]` — 23 message ids through a 13-entry handler
table. Both tables are asserted byte-for-byte in validate_takedown §10b.
Identified handlers:

| msg | handler | what it does |
|---|---|---|
| 7 | `0x00168516` | toggles the player view: `+0xC3C = 5` or `2` (with `DAT_004AE1DD`), then `FUN_00168360` |
| 8 | `0x00168591` | `FUN_00167CE0(4)` |
| 9 | `0x001685B5` | drops mode 15, sets a **0.5 s** blend (`0x003B1684`) from mode 2/4/5 and switches to **mode 10** |
| 10 | `0x001686CE` | same 0.5 s blend from `+0xC3C` into mode 10 |
| 11 | `0x00168788` | as 9, with a different blend style |

`FUN_00168360` is the view applier: `if (+0xC3C == 2) FUN_00167CE0(2)
else FUN_00167CE0(5)`.

**[?]** Which message the takedown cinematic and a player crash raise is
still open: the presentation-state broadcast `FUN_00053D20` (§4.6) has a
listener list, but the writer that installs the camera controller into it
has not been found.

### 8.5 The two ValueDB config blocks [C]

**Geometry — `FUN_00160840`**, per local player, at `mgr+0x28D0` and
`mgr+0x2A80` (`FUN_00160690` hands one out keyed on the owner pair
`racecar+0x1970/+0x1974`; released by `FUN_00160710`). Layout and the
compiled-in defaults, read directly out of the registrar:

| offset | group / param | default |
|---|---|---|
| `cfg+0x00` | `Camera/Look Back/` `Camera Offset` | (0, 0, 0) |
| `cfg+0x30` | `Camera/Bumper/` `Camera Offset` | (0, 0, 0) |
| `cfg+0x60` | `Camera/Follow/` `Camera Offset` | **(0, 0.95, −6.8)** |
| `cfg+0x70` | `Camera/Follow/` `Focus Offset` | **(0, 0.35, 2.0)** |
| `cfg+0x80` | `Camera/Follow/` `Spring Coeff` | **(0.06, 0.1, 5.0)** |
| `cfg+0x90` | `Camera/Follow/` `Down Angle` | **1.8** |

(strings: `Camera/Look Back/` `0x003AF4A8`, `Camera/Bumper/`
`0x003AF4CC`, `Camera/Follow/` `0x003AF498`, `Camera Offset`
`0x003AF4BC`, `Focus Offset` `0x003AF488`, `Spring Coeff` `0x003AF478`.)
Only Camera/Follow gets the Focus/Spring/Down triple; the other two
groups register `Camera Offset` alone, both zero. None of the six has a
`Data/vdb.xml` override (§3), so **these are the shipped retail values**.

**Shake — `FUN_00160B90`**, one instance at `mgr+0x2C30`, 11 groups of
`0x30`:

```
group i:  +0x00 Spring Coeff (vec3)   +0x10 Damping (vec3)   +0x20 Force (vec3)
group 0 (Crash/HandyCam) has no Force: +0x20 = Safe FOV Horiz, +0x24 = Safe FOV Vert
```

order: Crash/HandyCam, then InGame/Effect/{Burnout, Shunt, Shunted, Slam,
Shake}/{Position, Angular}. Compiled-in defaults are 15 / 10 for every
spring/damping; the retail values in §3 come from the VDB.

### 8.6 Crash/HandyCam is dead config [C, negative]

The mission premise was that the takedown camera consumes the
`Crash/HandyCam` spring/damping. It does not. Only two instructions in
the entire image address the block `mgr+0x2C30..+0x2E3F` off a base
register:

```
0x00167273   LEA EAX,[EDX + 0x2C30]     ; FUN_00167220 -> registrar FUN_00160B90
0x001AAA64   ADD EAX, 0x2C30            ; FUN_001AA990 -> unregistrar FUN_00161990
```

and **no instruction anywhere in the camera module
(`0x0015B000..0x0017B000`) addresses it at all**. validate_takedown §10d
asserts both. The values are still in `Data/vdb.xml` and still resolve
(§3) — they are simply never read by this build. They have accordingly
been removed from the port's camera law.

### 8.7 The recovered chase-camera law [C, executed]

`FUN_0015E550` (mode 2/3's per-frame update) is the only camera update
wired to Camera/Follow. Its smoothing core was **executed under Unicorn**
over the span `0x0015E5B6..0x0015E734`
(`tools/emulate_tdfx_camera.py:follow_blend`) and diffed frame-by-frame
against the C port over a 5 x 6 grid of (dt, speed) — validate_takedown
§10e, 120 checks:

```
n     = (int)(60.0f * dt + 0.5f)                 ; 0x0015E5DC
                                                 ;   60.0 @0x003F720C, 0.5 @0x003B1684
gate  = min(1.0f, speed_ms * 2.236936330795288f * 0.02f)   ; 0x0015E5B6
                                                 ;   2.23694 @0x0038994C, 0.02 @0x003B1A08,
                                                 ;   clamp vs 1.0 @0x003B168C
retain_pitch = ((1 - SpringCoeff.x) * gate)^n    ; 0x0015E630..0x0015E734 (unrolled x8)
retain_yaw   =  (1 - SpringCoeff.y)^n
blend_pitch  = 1 - retain_pitch                  ; 0x0015E745
blend_yaw    = 1 - retain_yaw                    ; 0x0015E73C / 0x0015E74B
```

Note the **pitch spring is gated by speed and the yaw spring is not**: at
a standstill `gate = 0`, so `blend_pitch = 1` and the pitch snaps; the
gate saturates at 50 mph.

The angle step itself **[S]** (`0x0015E885`..`0x0015E8A3`):

```
target_deg = atan2(sqrt(1 - c*c), c) * 57.29578      ; 57.29578 @0x00395D78
cam+0x1C  += (target_deg - cam+0x1C) * blend_pitch
```

and the eye is composed by rotating the Camera Offset about **X by
−`cam+0x1C`** (axis constant `0x00414AD0` = (1,0,0), `FUN_00011900` at
`0x0015E8AC`) and then about **Y by `cam+0x18`** (axis `0x00414AE0` =
(0,1,0), `0x0015E942`), each matrix multiplied in by `FUN_000116E0`.

**[?] still open:** where `c` (the pitch target's cosine) comes from in
the vehicle transform, the yaw target, and the Spring Coeff `.z` = 5.0
consumer.

### 8.8 What the port does with this

`src/burnout3_takedown.c`:

* `b3_cam_substeps` / `b3_cam_speed_gate` / `b3_cam_follow_blend` — the
  §8.7 core, 1:1, **[C]** with 120 executed differential checks.
* `b3_cam_smooth_angle` — the angle step, **[S]**.
* `B3_CAM_*` macros — §8.5's Camera/Follow geometry and the 90 degree
  FOV, **[C]** from image bytes, asserted both at their literal addresses
  and in the header text.
* `b3_tdfx_camera` — frames the **victim** (recovered) with the above.
  **GLUE, marked inline:** the choice to use the follow geometry (retail
  picks a director mode that is still `[?]`), the yaw target
  (broadside to the attacker->victim line), the pitch target (held at
  Down Angle), and the ease-in/ease-out weight.
* `b3_tdfx_crash_camera` / `_reset` — the player-crash view. **The retail
  crash-camera director is `[?]`**: no crash-specific mode was identified
  in the 20-mode table and the state-2 broadcast has no proven camera
  listener, so this reuses the same recovered law aimed along the wreck's
  travel. Composition GLUE, constants `[C]`.

---

## 7. Open items

* **RESOLVED for the player crash** (section 9.1): retail does not switch
  camera mode on a crash at all — the crash view is mode 2 still running
  over the wreck. The sender is `FUN_00018B90` (section 9.1b) and no site
  posts a crash message. Which mode the **takedown cinematic** selects is
  still **[?]** for the same reason: nothing posts a message for it either.
* **RESOLVED** (section 9.2): the pitch target is `asin(car.forward.y)`,
  the yaw target is the car's flattened heading tracked by a signed
  `acos` error, and Spring Coeff `.z` = 5.0 is the reach of the
  below-50-mph camera-collision probe at `0x0015EE8A`.
* **RESOLVED** (was: "the crash/handycam camera's actual eye placement"):
  the camera is the 20-mode director of section 8, not the object at
  `racecar+0x1A00`, and the Crash/HandyCam springs are dead config
  (section 8.6).
* **[?]** the aftertouch *motion* producer (racecar `+0x1A10`/`+0x1A30`/
  `+0x1A50`) — only its slow-motion gate is closed here.
* **[?]** whether anything ever installs a listener at `DAT_004CFB20+0x04`;
  with none in the image the 9/10/11 posts are inert in retail [S].
* **[?]** the dt source of the callout element's `+0x150` accumulator.
* **[?]** `FUN_001994D0`'s exact selection precedence between revenge /
  multi / streak / aftertouch / signature (the ids and their conditions are
  located; the port's `b3_tdfx_select_takedown_message` is [S]).
* **[?]** descriptor fields `+0x00` / `+0x04`'s text-layout meaning and the
  flag bits other than bit 3, and the sound ids behind `DAT_003F746C..`.
* **[?]** what raises the `+0x174 & 8` event flag that arms the 0.35 s
  impact hit.
* **[?]** the game-mode virtual `director->vtbl[+0x104]` that can veto the
  cinematic, and the global enable byte `DAT_004AE1DB`'s writer
  (`0x00085127` sets it from a comparison — likely an options toggle).
* **RESOLVED to [C-disasm]** (section 9.3/9.4): both windows, their
  writers, their gates and the fact that the impact window is measured on
  the *dilated* clock are now read out of the image at their literal
  addresses and asserted in validate_takedown section 11c; the ported
  machine reproduces all three timelines. The remaining **[?]** is what
  raises the `+0x174 & 8` arming flag.

---

## 9. The crash director — the player-crash camera and the dilation window

Recovered 2026-08-12. Acceptance: `tools/validate_takedown.py` section 11
(**960/960** overall). Oracle: `tools/emulate_tdfx_camera.py:follow_update`,
which executes **the whole of `FUN_0015E550`** under Unicorn over a seeded
car transform.

### 9.1 Three retractions [C, negative]

The mission this section answers started from three inherited claims. All
three are wrong, and the evidence is in the image:

**(a) `FUN_0018E050` is not a camera call, and 4/5 are not camera
messages.** `FUN_00025CC0` does `PUSH 4`/`PUSH 5; CALL 0x0018E050` at
`0x00025D51`..`0x00025D57`. `FUN_0018E050` is:

```
0018E068  CALL [gamemode->vtbl + 0x7C](racecar, kind)   ; gamemode =
                                                        ; *(DAT_004D5370+0x1B8)
0018E06B  racecar+0x18F0 = racecar+0x18D0
0018E077  racecar+0x18F4 = (s8)DAT_0064B38C[grid_slot]->+0x216
0018E09E  CALL FUN_001986A0(kind)
0018E0B5  if (!DAT_0064B30C) rep movsd 0x32 dwords : racecar+0x10D0 -> +0x16D0
0018E0C0  if (kind == 1) *(u16*)(racecar+0x16D0) = ++DAT_0073A18C
```

i.e. the **race-result recorder**: it notifies the game mode of a
finish/wreck/eliminate *kind* and snapshots the 200-byte score block
`racecar+0x10D0` into the result slot `racecar+0x16D0`. Other kinds seen
at other sites: 1 (finish, gets the sequence number), 2
(`FUN_00024F10` @ `0x00024F32`, eliminated), 5 (`0x000244DE`,
`0x0002460C`, `0x00024C61`), 6 (`0x000269F0`, recovered), 7/8
(`FUN_00025850` via `vtbl+0x100`).

And even if it *were* a camera post, ids 4 and 5 are dead there: the
23-message dispatcher's index table `0x00168A20` maps **both to handler
slot 12**, which the handler table `0x001689EC` resolves to `0x001689CC`
— the default `pop/pop/pop/pop; ret 0x10` arm. Asserted in §11a.

**(b) The camera-message poster is `FUN_00018B90`, and nothing posts a
crash message.**

```
00018B90  ECX = DAT_00567174          ; the CURRENT top-level state's
                                      ; listener interface
00018B96  if (!ECX) return
00018BA2  push EDX ; push msg ; push DAT_00567178 (its owner) ; push 5
00018BAB  call [ECX]                  ; slot 0 = the dispatcher
```

`(DAT_00567174, DAT_00567178)` is the running state's `(interface, owner)`
pair — written by every state entry (`FUN_00026D30` @ `0x00026FA7`,
`FUN_0002B030`, `FUN_0005E8C0`, `FUN_00067330`, …) and ticked from the
main loop with **verb 4** (`0x00016BD6`, `0x000170D1`, `0x000172B1`);
verb 5 is "message". Every message actually posted in the image is:
`0x30`..`0x3C` and `0x2E`/`0x2F` (the 22 `FUN_00018B90` call sites in
`0x00022xxx`), `0x34`, `0x4B` ×3, `0x1B`, `0x28`, and — the only two in
the camera's 0..0x16 range — `0x0A` at `0x00022291` and `0x0D` at
`0x00022629`, both to the replay owner `DAT_003F9BA8`. **No site posts
4, 5, or any other crash-related camera message.**

**(c) There is no crash camera mode.** No instruction in the camera
module (`0x0015B000..0x0017B000`) reads the crashed flag
`racecar+0x18FA` — the only readers in `0x00169xxx`/`0x0016Exxx`/
`0x0017xxxx` belong to the AI aggression machine `FUN_00169540` and the
route follower. Every `FUN_00167CE0(n)` call site is enumerated in §8.3
and none of them is reached from a crash. The presentation-state
broadcast `FUN_00053D20` (state 2 on crash, `0x0018C812`) has a listener
list, but the camera composite is not in it.

> **The player-crash camera is the ordinary chase camera, director mode 2
> (`FUN_0015E550`), still running over the wreck's own tumbling
> transform.** That is the truth; the recovered law below is what makes
> it read as a crash camera.

### 9.2 The whole chase-camera law — `FUN_0015E550` [C, executed]

Executed end to end (`tools/emulate_tdfx_camera.py:follow_update`; only
the low-speed camera-collision probe `FUN_00162A90` @ `0x0015EEEF` is
stubbed) and diffed frame-by-frame against the C port over 7 seeded
wreck attitudes — validate_takedown §11b.

Inputs, all read at the top of the function:

| where | what |
|---|---|
| `[mode+0x20]` | the slot; `DAT_0073A1A8[slot]` = racecar, `DAT_0064B38C[slot*0x30]` = **vehicle** |
| `*(veh+0x204)` | the car's own 4×4: rows **right / up / forward / position** |
| `veh+0x0BC` | speed m/s → the pitch spring's gate |
| `racecar+0x11AC` | the boost ramp → FOV |
| `[mode+0x08]` | the Camera/Follow cfg block (§8.5), `cfg+0x60`-based |
| `[mode+0x18]`, `[mode+0x1C]` | the persistent **yaw** and **pitch**, degrees |
| `[mode+0x24]` | yaw-tracking gate (`FUN_0015E060` @ `0x0015E072` sets it to 1) |
| `[mode+0x26]` | look-back: negates the forward and right rows (`0x0015E753`) |

The law:

```
n            = floor(60*dt + 0.5)                          0x0015E5DC
gate         = min(1, speed_ms * 2.236936 * 0.02)          0x0015E5B6
blend_pitch  = 1 - ((1 - Spring.x) * gate)^n               0x0015E630..0x0015E745
blend_yaw    = 1 - (1 - Spring.y)^n
                                    (a `pow(x,1.2)` branch at 0x0015E65E is
                                     gated by DAT_0045B9C0, which is 0)

focus        = car.pos + car.right*Fx + car.up*Fy + car.fwd*Fz   0x0015E7B7
                                    F = Focus Offset (0, 0.35, 2.0), selected
                                    by `ADD EDI,0x10` at 0x0015E7A5

pitch_target = asin(car.fwd.y) * 180/pi                    0x0015E869 fpatan
pitch       += (pitch_target - pitch) * blend_pitch        0x0015E885..0x0015E8A3

M            = translate(Camera Offset) . Rx(-pitch) . Ry(yaw)
                                    0x0015E7F6 / 0x0015E8AC / 0x0015E942
yaw_err      = acos( dot( flatten(car.fwd), flatten(M.fwd) ) )   0x0015EB6F
               negated when dot(car.fwd, M.right) < 0            0x0015EBB9
yaw         += yaw_err * blend_yaw   (only if [mode+0x24])       0x0015EBD9
M            = M . Ry(yaw_err * blend_yaw)                       0x0015EBEF

eye          = M.row3 + focus                              0x0015EC79
M            = Rx(Down Angle) . M      (orientation only; eye kept)  0x0015ECDE
fov          = 90 + (1 - (racecar+0x11AC - 1)^2) * (110 - 90)   0x0015ED5C
eye         += M.fwd * (2.3 - 2.3 / tan(fov/2))            0x0015ED61..0x0015EDC6

camstate+0x10 = fov ; camstate+0x20 = quat(M) ; camstate+0x30 = eye
```

with `FUN_00011900(axis, angle)` taking **degrees** (`0x0001190C` multiplies
by `0.0174533` before `fsin`/`fcos`) and producing, row-vector,
`Rx(a) = {{1,0,0},{0,cos a,sin a},{0,-sin a,cos a}}`,
`Ry(a) = {{cos a,0,-sin a},{0,1,0},{sin a,0,cos a}}`. `FUN_000116E0(A,out,B)`
is `out = A·B` row-vector, so a rotation `A` with a zero translation row
leaves `B`'s translation alone — which is why the Down Angle tilts the view
without moving the eye. `FUN_00011B10` is matrix → quaternion.

New constants: `0x003F7210` = **110** (boost FOV), `0x003F7214` = **90**
(base FOV), `0x003F7218` = **2.3** (the dolly that keeps the car the same
size as the FOV widens), `0x003F721C` = **50 mph** (below it the update
runs the camera-collision probe `FUN_00162A90`, fed a point
`Spring.z = 5.0` metres down the view ray — that is the missing consumer
of the third spring component, `0x0015EE8A`).

**Why this is the anti-floaty piece.** The old port anchored the eye on
`wreck.pos` with a constant pitch target, so the camera translated 1:1
with the wreck and nothing moved in frame. Retail anchors it on a point
**2 m ahead inside the car's own frame**, and drives the pitch from
`asin(car.forward.y)` — a tumbling wreck's forward sweeps the full ±90°,
so the camera pitches and swings with the tumble, and the speed gate means
the pitch spring *snaps* as the wreck slows (gate → 0 ⇒ blend → 1). The
in-game series below shows pitch 0° → 31° and yaw 89° → 176° over one
crash.

### 9.3 The player-crash dilation window [C-disasm]

`FUN_00025CC0` is not the crash handler; it is the **crash-presentation**
handler, and it is reached from three places in the race-rules object
(vtable `0x003A99F4` / `0x003A9B64`):

| site | function | when |
|---|---|---|
| `0x000258BF` | `FUN_00025850` "OnVehicleWrecked" (`vtbl+0x0A4`) | a car wrecks |
| `0x00025A7E` / `0x00025AA3` | `FUN_00025A30` "OnTakedown" (`vtbl+0x0B4`) | after `FUN_000273F0` (the cinematic chain, §2.1) |
| `0x00025C5B` | `FUN_00025C50` "ForceWreck" (`vtbl+0x0CC`) | scripted wreck (`FUN_0010DCA0` + `FUN_00125100(10, 0.4, up)`) |

`FUN_00025850`'s gate at `0x00025890`:

```
if (racecar+0x16C8 == 0)              return    ; no presentation credit
if (racecar+0x16C4 != 0.0)            return    ; health not floored
if (racecar+0x1920 != 0)              return    ; not the human
FUN_00025CC0(EAX=racecar, ECX=rules, 0)
```

and `FUN_00025CC0`'s head + body:

```
00025CC6  if (0 < credit != -1) { racecar+0x16C8 = --credit
                                  if (credit == 0) DAT_0073A1A4-- }
00025CE4  racecar+0x16C4 -= 1.0 ; clamp at 0
00025D10  if (racecar+0x16C8 != 0) { racecar+0x16C4 = 1.0 ; return }
00025D29  if (racecar+0x1920 != 0) goto score_pass          ; AI: no FX
00025D33  if (!rules+0x4C)          return                  ; mode disabled
00025D3E  if (racecar+0x10D8 == 0)  FUN_0018E050(racecar+0x1144==4 ? 4 : 5)
00025D5C  DAT_0060EA24 = 5                                  ; <-- THE REQUEST
00025D66  DAT_00649B9E = 1
          ... per-car crash-score pass over DAT_0073B34C[] ...
```

**The other two entries, recovered 2026-08-13 (CRASH-PARITY wave 2 —
closes the `[?]` on whether a takedown-caused crash spends the credit):**

`FUN_00025A30` "OnTakedown" (EBX = attacker racecar, ESI = victim,
ECX = rules) spends a credit at TWO sites, and in both the racecar handed to
`FUN_00025CC0` in EAX is the **ATTACKER**:

```
0x00025A67  attacker+0x1920 == 1 && victim+0x1920 == 0
            -> FUN_00025CC0(attacker, rules, victim)          0x00025A7E
0x00025A83  attacker+0x1920 == 0 && 0.3 [0x003B1750] > attacker+0x16C4
            -> FUN_00025CC0(attacker, rules, victim)          0x00025AA3
```

Since `FUN_00025CC0` @`0x00025D29` gates the divisor-5 request on
`racecar+0x1920 == 0`, an AI taking the human down spends the AI's credit and
requests NO slow-motion, while the human taking someone down spends the
PLAYER's credit and DOES request divisor 5.

`FUN_00025C50` "ForceWreck" spends the credit unconditionally at
`0x00025C5B` with a NULL victim, on the racecar being wrecked, before
`FUN_0010DCA0` + `FUN_00125100(10, 0.3, up)`.

So **takedown-caused crashes are not exempt: they run the same one-credit
machine**, charged to the attacker's racecar.

Counter semantics, from the head @`0x00025CC6`..`0x00025D29`: the decrement
only happens while the value is `> 0` and `!= -1`, and the body runs once it
READS 0. `+0x16C8 == 1` (what `FUN_00025AB0` writes) = "this call presents";
`== 2` = "the next one does"; `== 0` = "present every time"; `== -1` =
**presentation permanently disabled**. Every suppressed call sets
`+0x16C4 = 1.0`.

*Harness gap (open):* `b3_tdfx_crash_begin` spends the credit on the player's
OWN crash (`FUN_00025850`'s path, correct), but nothing spends it when the
PLAYER TAKES SOMEONE DOWN. Closing that needs a takedown-module export —
spec in the CRASH-PARITY-2 integration note.

* `racecar+0x16C8` — the **crash-presentation credit**. `FUN_00025AB0`
  (the event reset, `vtbl+0x074`) sets it to **1** for every car
  (`0x00025AE5`) and clears it again for class-1 cars (`0x00025B63`);
  `FUN_00024F10` (the eliminator mode) clears it (`0x00024F3F`). So a
  race gives the human **one** divisor-5 crash presentation per event.
  `DAT_0073A1A4` counts the cars that still hold a credit.
* `racecar+0x16C4` — health (the damage machine's distributor writes it at
  `0x000241BB`); the crash floors it. `FUN_00025A30` uses `0.3` there
  (`0x00025A95`, `0x003B1750`).
* `rules+0x4C` — the enable byte, set to 1 by the virtual at `vtbl+0x00C`
  (`FUN_00025040` / `FUN_000243D0`) at event start. Normally on.

**The request is made once — it is not re-asserted per frame.** What
takes it back, in the order the frame runs them:

| order | site | writes | condition |
|---|---|---|---|
| 1 | `0x001188A4` / `0x001188D6` | 5 / 1 | `FUN_00118410`, crashed + boost held / released — **but the release only fires if the engaged flag `veh+0x4AC7` was set** (`0x001188CC`), so a crash with no aftertouch never triggers it |
| 2 | `0x00025D5C` | **5** | the wreck instant, above |
| 3 | `0x0002655B` / `0x00026525` | 6 / **1** | `FUN_00026050`'s impact-hit window |
| 4 | `0x00119C24`, `0x000269FD` | 1 | `FUN_00119C00` (the vehicle leaving crash mode, gated `racecar+0x19BE` which `0x0018D740` sets to 1) and the mode's "car recovered" virtual |

The impact-hit window is armed by `FUN_00026A70` (the mode's "big hit"
virtual) at `0x00026B18`/`0x00026B2D`, gated on the collision record's
`+0x174 & 8` and `!(+0x174 & 2)` (what raises bit 3 is still **[?]**;
`0x00114B62` sets bit 1 in the collision module). **Its clock is the
DILATED `DAT_0060EA20`** (`0x00026507`), so the documented 0.05 s + 0.35 s
are *game* seconds:

```
0.05 s of clock at divisor 5  =  0.25 s of wall time
0.35 s of clock at divisor 6  =  2.10 s of wall time
```

### 9.4 The recovered timeline

| wall t | game t | what | divisor | source |
|---|---|---|---|---|
| 0.00 | 0.00 | the wreck: `FUN_00025850` → `FUN_00025CC0` requests 5, latches `DAT_00649B9E`; a big hit also arms the impact window at clock+0.05 | **5** | `0x00025D5C`, `0x00026B18` |
| 0.25 | 0.05 | impact window opens | **6** | `0x0002655B` |
| 2.35 | 0.40 | impact window expires → **full speed** | **1** | `0x00026525` |
| — | — | boost held while crashed → aftertouch | **5** | `0x001188A4` |
| — | — | boost released (only after it engaged) | 1 | `0x001188D6` |
| recovery | — | the vehicle leaves crash mode | 1 | `0x00119C24` |

So a violent player crash is **0.25 s at 1/5 then 2.1 s at 1/6, then the
wreck tumbles at full speed** — the impact window is what truncates the
crash's own divisor-5 request. A crash that does **not** arm the impact
window keeps divisor 5 all the way to crash-mode exit; that is the only
case in which retail sustains the slow-motion, and it is why the earlier
"whole window" reading felt weightless: it applied the un-truncated branch
to every crash.

### 9.5 What the port implements

`src/burnout3_takedown.c/.h`:

* `b3_cam_follow_update` / `b3_cam_follow_init` — `FUN_0015E550`, 1:1
  **[C]**, 7 scenarios × up to 12 frames diffed against the executed
  original (eye, quaternion, FOV, both angle states).
* `b3_tdfx_crash_camera_x(car_rows, speed, boost_ramp, dt, out)` — the
  crash view: the same law over the wreck's transform. **The only
  remaining harness choice is which transform is fed in**; there is no
  camera GLUE left in this path. `b3_tdfx_crash_camera` (the old
  position+velocity signature) is kept as a *degraded* wrapper that
  synthesises a levelled basis — it cannot reproduce the tumble pitch.
* `b3_tdfx_crash_begin` / `b3_tdfx_crash_end` / `b3_tdfx_event_reset` —
  the §9.3 credit and the two ends of the window.
* `b3_tdfx_update` now runs the four dilation writers in the retail
  per-frame order over one latched request slot, so the §9.4 timeline is
  emergent rather than scripted. The cinematic still pre-empts the lot
  (GLUE priority; retail relies on the modes being exclusive).

### 9.6 Still open

* **[?]** what raises the collision record's `+0x174 & 8` that arms the
  impact window (the consumers are located; `0x00114B62` sets bit 1).
* **[?]** `FUN_00162A90`, the below-50-mph camera-collision probe, and how
  its result moves the eye. `Spring.z = 5.0` is confirmed as its reach.
* **[?]** the game-mode virtual `vtbl+0x7C` that `FUN_0018E050` calls, and
  what the result kinds 4 / 5 / 6 mean to each mode.
* **[?]** `racecar+0x10D8` (gates the result post) and `racecar+0x1144`
  (selects kind 4 over 5).

## 9.7 CORRECTION — the crash presentation, composed (2026-08-12)

§9.2–9.5 recovered the three pieces. This section is what happens when they
run **together, per rendered frame** — the whole-sequence oracle
`tools/emulate_crash_traj.py:CrashSequence`, accepted by
`tools/validate_crash_traj.py` section 8 (**134/134**) and
`tools/validate_takedown.py` section 11c/11d (**973/973**). The full
write-up, with the main-loop disassembly and the measured curves, is
RE_NOTES §16.3; this is the takedown-FX half.

### (a) The camera runs on the DILATED dt, not the real one [C-disasm]

This retracts the assumption the port was built on. `FUN_000170B0`
@`0x00017147`..`0x0001717A` builds the camera delta as
`ticks * DAT_0060EA1C` — the **dilated** per-frame dt — and `FUN_00167940`'s
20-mode dispatch (`0x0016797E`) hands that to every mode **except 0 and 14**,
which alone read `DAT_004AE1FC`:

```
83ff0e  CMP EDI,0xE     7407 JZ +7
85ff    TEST EDI,EDI    7403 JZ +3
55      PUSH EBP                        ; the DILATED product
eb07    JMP +7
8b15fce14a00 MOV EDX,[0x004AE1FC]  52 PUSH EDX   ; modes 0 and 14 only
```

The player-crash camera is the chase camera, **mode 2** (§9.1), so it is
dilated. Because `FUN_0015E550`'s blend exponent is
`n = floor(60*dt + 0.5)` (`0x0015E5DC`), at divisor 5 (`dt = 1/300`) and
divisor 6 (`1/360`) **n = 0**, every blend is `1 - x^0 = 0`, and the camera's
yaw and pitch **hold for the whole dilated window**. Only the focus anchor —
`car.pos + right*0 + up*0.35 + fwd*2.0`, in the CAR's frame — moves, so the
shot sways gently with the tumble instead of chasing it.

§9.2's "why this is the anti-floaty piece" still stands for the **full-speed**
camera; it is simply not what runs during the crash. Measured against the
oracle: yaw holds at -0.01 deg and pitch at -0.81 deg for all 300 rendered
frames of a plain crash, and the moment the divisor returns to 1 the same law
swings 178.8 deg of yaw.

### (b) What arms the impact window — closes §9.6's first `[?]` [C]

`mode+0x52` / `mode+0x44` are written only at `0x00026B2D` / `0x00026B28`
inside `FUN_00026A70`, whose head requires the arg2 object's
`+0x174 & 8` set and `+0x174 & 2` clear. `FUN_00026A70` is reachable only
virtually, and `0x001134EF` in `FUN_00112E70` is its **only** call site (the
other two `CALL [reg+0x54]` in the image are 4-arg cdecl callbacks;
`FUN_00026A70` is `RET 8`). `FUN_00112E70`'s only caller is the broad phase
`FUN_00111CD0` @`0x00111D77`, on the arm that requires a participant of
**type 3** — a traffic vehicle. Bit 3 has one setter,
`0x001A7584 OR byte ptr [ECX+0x174],0x8` in the type-3 record initialiser
`FUN_001A7210`, gated on the spawn flag's bit 0, which also caches the record
at `DAT_00739C68`: there is **one designated big-hit traffic vehicle**.
Ghidra MCP traces that flag as `FUN_001A2B20`'s stack argument
`[EBP+0x3C]`, forwarded unchanged to `FUN_001A7210`; the alternate queue path
`FUN_001A5C70` supplies it from its 0x20-byte runtime request record at
`+0x1B`. `FUN_001A5880` takes that alternate path only when the manager's
`+0x363B8` mode is nonzero; `FUN_001A13F0` writes that mode from
`TDESC+0xB4 & 4`. `FUN_001A3AE0` state 6 reads TDESC schedule row
`+0x38/+0x3C` as 0x0C descriptors and writes each descriptor address to
`manager+0x30+slot*4`, using descriptor bytes `+0x0A/+0x0B` as manager and
slot. Descriptor `+0` points to its 0x20 records, byte `+8` is their count,
and record `+0x1B` is the designation flag. Normal-mode `FUN_001A5910`
supplies zero. This is **not** TDESC's
0x20
position/direction entry: TDESC `+0x1B` is the high byte of the direction-Z
float. The dynamic road-agent path `FUN_001A6070` also explicitly pushes zero
for this final argument; its nearby `0x0B` value is `FUN_001A2B20`'s
companion-type argument, not the flag. Thus ordinary pool traffic is not a
big-hit candidate. Across 377 shipped event TDESCs, all 3,005 static request
records have `+0x1B & 1 == 0`, so the designation writer is runtime-only or
absent from these modes. Payload semantics and lifetime policy remain open. [C]

So the window is the scripted "launched by something big" presentation — the
same block fires `FUN_00125100(0x10, 0.50, world-up)` and
`FUN_00125100(0x80, 1.60, frame.at)` and *cancels* the ordinary wreck
(`0x00113077`, `0x001141B5`).

> **§9.4's timeline is the BIG-HIT branch, not the default.** A plain wall or
> car-vs-car crash never arms the window and keeps divisor 5 to crash-mode
> exit. Arming it on every wreck — which the port did — truncated the crash's
> own request at 0.40 game-seconds and dropped the rest of the flight to full
> speed: apparent rotation 2.41 -> 11.89 deg per rendered frame.

### (c) The crash HUD sign

`b3_tdfx_crash_begin` now posts **0x7F "CRASHED!"** (string 2103, sign kind 3
-> descriptor 6 -> sign index 3). The accept test at `0x001993EF` makes 0x7F,
0x80 and 0x81 the three ids that *require* the racecar's crashed flag, which
is set at the wreck instant. It is posted **before** the credit gate, because
the callout chain (`FUN_001994D0` -> `FUN_00199350`) is independent of the
dilation credit `racecar+0x16C8` — the sign shows on every crash, the
divisor-5 presentation once per event. No ordering logic is needed against a
takedown: the message id **is** the priority (`0x0019937F`), so a 0x80
"TAKEN OUT" post overrides 0x7F by itself.

### (d) What the port implements now

* `b3_tdfx_crash_camera_x` — driven by `b3_tdfx_sim_dt()` (the dilated dt the
  last `b3_tdfx_update` produced), with `real_dt` kept only as the fallback
  for callers outside the dilation machine.
* `b3_tdfx_qualify_big_hit(flags174)` / `b3_tdfx_impact_hit_object(flags174)`
  — `FUN_00026A70`'s gate. `b3_tdfx_impact_hit()` now arms only when a real
  object contact qualified it, so an unqualified call is inert.
* `b3_tdfx_sim_dt()` — the port's `DAT_0060EA1C`.

### (e) Still open

* the `[?]` in §9.6 about `FUN_00162A90` is unchanged.
* **[?]** the un-truncated branch's length: `FUN_00198E60` @`0x00198F65`
  stamps recovery as `racecar+0x10DC (the DILATED clock) + 5.0`, so 5 game
  seconds = **25 s of wall time** at divisor 5. Either `FUN_00119C00`'s gate
  `racecar+0x19BE` (written by `0x0018D740`, unrecovered) releases sooner or
  retail leans on the boost release at `0x001188D6`. Top follow-up.
* **[?]** what sets the type-3 spawn flag bit 0 that designates the big-hit
  traffic vehicle.
