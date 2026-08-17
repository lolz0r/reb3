# RE_BOOSTFX — the boost exhaust flame, and the boost sound

Recovered from `build/burnout3.elf` (the correctly-mapped ELF of the retail
Xbox `default.xbe`). Implemented in `src/burnout3_boostfx.{c,h}` (visual) and
`src/burnout3_sfx.{c,h}` (audio); asserted by `tools/validate_boostfx.py`
(**77/77**) and `tools/validate_sfx.py` §7 (part of **285/285**); art by
`tools/extract_boostfx_art.py`.

Evidence marks, as elsewhere in this repo:

* **[C]** execution- or byte-verified — the bytes were read out of the ELF or
  the shipped data, or the retail code was run and produced the value.
* **[S]** read from the disassembly, self-consistent, no differential case.
* **[?]** open.
* **GLUE** this port's own bridging; not the game's.

---

## 0. Headline results

1. **The flame is not a particle system and not a flipbook.** It is the *same*
   billboard sprite-pool system the light coronas use (`docs/RE_CARFX.md` §4),
   with two extra pools — `coronaboost` and `coronaboostred` — and a different
   emitter, `FUN_001871E0`.
2. **`docs/RE_CARFX.md` §4.2's `[?]` "types 8..11 exhaust / roof / misc" is
   closed for type 8: type 8 IS the exhaust.** `FUN_001871E0` reads exactly
   `model+0x1684` / `model+0x16B4`, i.e. `+0x1664 + 8*4` and `+0x16AC + 8` —
   corona light type 8 — and on `COMP/Car1` the two type-8 records are
   `(±0.453, 0.074, −1.972)` with normal `(0,0,−1)`: the tailpipes.
3. **Per emitter the flame is three additive billboards** along the record's
   outward normal at 0.08 / 0.14 / 0.18 units, each **half the size and half
   the brightness of the one before**, with each sprite's brightness
   multiplied by a fresh uniform random over its own band. **That random is
   the heat flicker** — there is no animation clock and no UV animation.
4. **`carObj+0x1901`, the pool selector, is a per-car-MODEL constant, not a
   gameplay state.** The car loader `FUN_0018D0E0` clears it and sets it for
   exactly five packed base-40 car ids — `COMPCAR10`, `MSCLCAR10`,
   `CUPECAR10`, `SPRTCAR10`, `SUPRCAR10`. So the **orange** flame is the badge
   of the five `Car10` specials; every other car burns blue-white.
5. **The boost sound is a three-part chain, and its wave names are ASCII
   literals, not the packed base-40 constants every other sound uses**:
   `BoostIn` → `BoostLoop` (looping) → `BoostOut`, plus a `fire` layer, all
   loaded by `FUN_00136F80` out of `sound\generic.awd`.

---

## 1. The dispatcher — `FUN_0017F730` [C]

```
0017f738  MOV  AL,byte ptr [EBP + 0x68]      ; the corona gate
0017f73b  TEST AL,AL
0017f73d  JZ   0x0017f751
0017f74a  CALL 0x00187c70                   ; the light coronas
0017f751  MOV  AL,byte ptr [EBP + 0x18fa]    ; the CRASHED flag
0017f757  TEST AL,AL
0017f759  JNZ  0x0017f763
0017f75e  CALL 0x001871e0                   ; THE FLAME
```

`EBP` is the car object. `carObj+0x18FA` is the crashed byte
(`docs/RE_GAMEPLAY.md` §3), so **a wrecked car has no flame** — which is also
the reason the port cuts the boost loop on a crash. Every instruction above is
read back byte for byte by `validate_boostfx.py` §4.

---

## 2. The emitter — `FUN_001871E0` [C]

### 2.1 Where the flames come from: corona light type 8

```
0018725b  MOV EBX,dword ptr [EAX + 0x1684]   ; records = model + 0x1664 + 8*4
00187261  MOV AL, byte ptr [EAX + 0x16b4]    ; count   = model + 0x16AC + 8
00187480  ADD EBX,0x28                       ; walk from record + 0x28
001879c8  ADD EBX,0x30                       ; stride 0x30 -- the corona table
```

with `EAX = carObj+0x50`, the relinked `.bgv`. This is *literally the corona
light table* `docs/RE_CARFX.md` §4.2 documents (`0x30`-byte records of
`{float4 pos, float4 outward normal, float4 aux}`), read at type 8.
`tools/extract_carfx_art.py` already writes those records into
`build/cars/<CLS>_<Car>.lights` as `light 8 aux8 …`, so **no new extractor was
needed for the emitter positions**: 213 records over 84 shipped cars, 205 of
them at negative Z (the rear), every one but a single outlier unit-normal.

### 2.2 The per-record transform

The record's `+0x20` vec4 — `docs/RE_CARFX.md` §4.2's other `[?]` — is a
**skinning attachment**: `+0x24`/`+0x28` are two barycentric weights and
`+0x2C`/`+0x2D` two indices into the car's part-matrix array at
`carObj+0x590 + idx*0x40`, the third slot of the blend being the literal
identity matrix at `0x003F8110..0x004A1F70`:

```
M = M[rec+0x2C] * rec[+0x24] + M[rec+0x2D] * rec[+0x28]
                             + I * (1 - rec[+0x24] - rec[+0x28])
worldPos = xform(rec.pos, M) * carObj+0x10          (the car world matrix)
worldNrm = xform3(rec.nrm, M) * carObj+0x10
```

so the flame follows body deformation. `FUN_0018D0E0` seeds `carObj+0x10` from
those same identity rows, so on an **undamaged** car the blend collapses to
the identity and the chain is exactly "transform the record by the car
matrix" — which is what the port does, since this harness has no per-part
deformation matrices. `[C]` for the blend, `[S]` for the collapse.

### 2.3 The three-sprite cascade [C]

Per record, three calls to the pool push `FUN_00042B00`:

| k | offset along normal | VA | arg3 (size) | VA | arg4 (pull) | colour band | far cut |
|---|---|---|---|---|---|---|---|
| 0 | 0.08 | `0x0039C16C` | `S` | — | `0.5·S` (`0x003B1684`) | `C · U(0.5, 1.0)` | 800 |
| 1 | 0.14 | `0x003B1B28` | `0.5·S` | `0x003B1684` | `0.25·S` (`0x003B1730`) | `C · U(0.28, 0.56)` | 400 |
| 2 | 0.18 | `0x003B1B24` | `0.25·S` | `0x003B1730` | `0.125·S` (`0x003B1728`) | `C · U(0.14, 0.28)` | 200 |

with the colour-band constants `0.5` (`0x003B1684`), `0.28` (`0x003A5A58`),
`0.56` (`0x003B1B2C`), `0.14` (`0x003B1B28`), and the far cuts as
`PUSH 0x44480000 / 0x43C80000 / 0x43480000` at `0x00187869` / `0x001878FD` /
`0x00187995`.

`FUN_00042B00` stores `arg3 * 0.5` (`MULSS 0x003B1684` @`0x00042B64`) as the
record's half-size, so the three sprites' **half-sizes are `0.5·S`, `0.25·S`,
`0.125·S`** and their pulls are numerically the same three values.

**The flicker.** Each sprite's colour is `base + span · u` with `u` from the
function's own inline generator, re-rolled per sprite per record per frame:

```
x = (x >> 16) + (x << 16) + carry;   carry += x
u = (unsigned)x * DAT_0054F46C
```

seeded from a global counter at `[renderCtx + 0x64550]` and its complement
(`MOV EAX,[EDI+0x64550]; NOT EAX` @`0x00187240`). `DAT_0054F46C` is BSS, so
its value is `[S]` — 2⁻³², the scale the identical `(unsigned)LCG * scale`
idiom in `FUN_0014A6B0` uses (`docs/RE_SFX.md` §2). **There is no other
animation**: no flipbook, no time input, no UV scroll.

### 2.4 The gate [C]

```
0018720f  MOVSS  XMM0,dword ptr [0x003a7ed8]   ; 0.01
00187220  COMISS XMM0,XMM6                     ; XMM6 = the size S
00187223  JNC    <return>
```

nothing is emitted at all unless `S > 0.01`.

---

## 3. Size, colour and pool — `FUN_00179F30` [C]

`__regparm3(EAX = out float[6], ECX = carObj+0x119C, [esp+4] = carObj+0x1901)`:

```
t = *(float*)(ECX + 0x14)            ; = carObj+0x11B0
if (t <= 0.0)  { out[4] = 0; return; }
if (typeByte)  { C = [0x00415CD0]; k = [0x003A2D7C] = 0.72; out[5] = 2; }
else           { C = [0x00415CC0]; k = [0x003B16EC] = 0.60; out[5] = 1; }
if (t >= 1.0)  { out[4] = k * t;  out[0..3] = C;     }
else           { out[4] = k;      out[0..3] = C * t; }
```

| constant | VA | value |
|---|---|---|
| blue colour | `0x00415CC0` | `(0.7, 0.72, 0.75, 0)` |
| orange colour | `0x00415CD0` | `(0.8, 0.8, 0.8, 0)` |
| k, pool 1 | `0x003B16EC` | 0.6 |
| k, pool 2 | `0x003A2D7C` | 0.72 |

So **below level 1 the flame fades in colour at constant size; above level 1
it grows at constant colour** — the "scales with boost" behaviour.

`out[5]` is the sprite POOL index, written as `MOV [EAX+0x14],1` @`0x00179F74`
and `…,2` @`0x00179F5C`. `FUN_001871E0` turns it into the pool pointer with
`LEA ECX,[EAX + EAX*2 + 3]` / `LEA ECX,[EDI + ECX*4]` (`0x0018723C` /
`0x00187382`), i.e. `renderCtx + 0xC + pool*0xC` — the same 0xC stride the
corona emitter hard-codes pool 0 into (`ADD ECX,0xC` @`0x00187C28`).

### 3.1 The pools, and their textures [C]

`FUN_0017EE00` walks a three-entry table at `0x003A3E7C`:

```
pool 0  0x003B0428  "coronaglow"        the light coronas (RE_CARFX 4.3)
pool 1  0x003B041C  "coronaboost"       blue halo, white-hot core
pool 2  0x003B040C  "coronaboostred"    orange halo, yellow-white core
```

Both flame rasters are 64×64 in `Data/Global.txd`, fully opaque with the glow
in RGB — the same convention as `coronaglow`, which is what justifies the
additive blend. `tools/extract_boostfx_art.py` writes them to
`build/boostfx/`.

### 3.2 Which cars burn orange — `FUN_0018D0E0` [C]

`carObj+0x1901` has no per-frame writer because it is **set once, at car
load**, by the car-object loader (the same function that builds `"<car>.bgv"`
and seeds `carObj+0x10` with the identity). It compares the car's 64-bit
packed base-40 name as an `(EAX, ECX)` dword pair:

```
0018d4cb  MOV byte ptr [EBX + 0x1901],0x0                  ; the default
0018d4c6/d4d4/d4dc  EAX 0x95EE2E00 ECX 0x5B55839B -> "COMPCAR10"
0018d4e3/d4ea/d4f2  EAX 0x81EE2E00 ECX 0x961647D1 -> "MSCLCAR10"
0018d4f9/d500/d508  EAX 0xE2EE2E00 ECX 0x5C3791C1 -> "CUPECAR10"
0018d50f/d516/d51e  EAX 0x21EE2E00 ECX 0xB8A15FE9 -> "SPRTCAR10"
0018d525/d52c/d534  EAX 0x4FEE2E00 ECX 0xB959BADE -> "SUPRCAR10"
```

`validate_boostfx.py` §5 re-decodes the ten immediates out of the instruction
stream with the repo's own base-40 alphabet on every run, so the five names
are produced, not remembered. (The neighbouring `carObj+0x1900`, set for two
further ids and handed to the *corona* emitter as `BL` @`0x0017F740`, is the
corona system's business and is not touched here.)

---

## 4. The level — `FUN_0017A480`'s tail [C]

`docs/RE_GAMEPLAY.md` §4 describes this function's tail as "camera/FOV +
backfire FX (speed thresholds 100→165 mph) — mapped, not core". The backfire
FX **is** the flame, and its level is the boost record's `+0x14`, i.e.
`carObj+0x11B0` — exactly what `FUN_00179F30` reads:

```
if (fxByte == 0) {
    if (!boosting) { lvl -= dt*2.0;  if (lvl <= 0) lvl = 0;   store; return; }
} else if (lvl == 0.0f) {
    lvl = 2.0f;                                  ; MOV 0x40000000 -- the flare
}
lvl -= dt*2.5;  if (lvl <= 1.0) lvl = 1.0;  store
```

so:

* **boosting → instantly 1.0** and held there (the floor clamps up as well as
  down);
* **from cold → a 2.0 flare** that falls to the 1.0 sustain in 0.4 s — an
  ignition backfire twice the normal size;
* **not boosting → fade at 2.0/s**, i.e. 0.5 s from the sustain to nothing.

`fxByte` is `(*(veh+0xCC4)) + 0x1033`, a byte on the damage/visual context
whose writer was **not** chased — so the port substitutes `fxByte := boosting`
and marks that substitution `[S]`. With the substitution the branch structure
above reproduces exactly, and `validate_boostfx.py` §6 runs the module's state
machine head-to-head against a transcription of it over four boost/release/
crash patterns.

The other floats in the same block, for the record: `+0x04` and `+0x08` are
the camera/blur ramps `FUN_00179C90` / `FUN_00179FD0` consume (`+0x08` becomes
`*0.4 + 0.5` into the post-FX), and it is `+0x08` that carries the recovered
100→165 mph curve — not the flame.

---

## 5. The quad — `FUN_00042B00` / `FUN_00042BC0` [C]

The pool record is 0x30 bytes (`LEA EAX,[EAX+EAX*2]; SHL EAX,4` @`0x00042B2B`):

```
+0x00  float3  position
+0x0C  float   far cut          (arg2)
+0x10  float   half size        (arg3 * 0.5,  MULSS 0x003B1684 @0x00042B64)
+0x14  float   pull-to-eye      (arg4)
+0x20  float3  colour * 64.0    (DAT_0035BF1C @0x00042B1A)
+0x2C  u32     0
```

`FUN_00042B00` is `RET 0x10` — four callee-popped stack args, which is what
pins the argument order; the corona emitter's own call
`FUN_00042b00(&colour, 400.0f, size, 0.5f)` (`FUN_00187BE0`) is the reference.

`FUN_00042BC0` culls each sprite on camera distance against `+0x0C`, then
builds one `D3DPT_QUADLIST` quad per record:

```
corner = pos ± right*size ± up*size - (pos - eye) * pull
uv     = (0,0) (1,0) (1,1) (0,1)      four 24-byte vertices, stride 0x18
```

with the eye at `DAT_004D67D0..` (the same `c108` the car vertex program
uses, `docs/RE_CARFX.md` §13.1). The corona pass's constant pull of 0.5 puts
its sprite at the midpoint to the eye; the flame's pull equals its own
half-size, so the brightest sprite is pulled ~30 % of the way to the camera at
the 1.0 sustain and correspondingly less as it shrinks.

The `×64.0` is a 0..4 HDR range packed into a D3DCOLOR byte and unpacked by
the combiner's ×4 output scale (`64/255 × 4 = 1.0039`), so the effective
vertex colour is the raw value — which is why the port simply passes it to
`glColor4f`, exactly as `burnout3_carfx.c` does for the coronas.

**The literal Xbox blend factors for the sprite pool were not decoded `[?]`.**
Additive is the port's GLUE choice, on the same three grounds the corona pass
uses it: the ×64 overbright, colour constants that only make sense
saturating, and rasters that ship fully opaque with the glow in RGB.

---

## 6. The boost SOUND [C]

### 6.1 Four waves, named by ASCII literals

Every other sound in `docs/RE_SFX.md` is named by a packed base-40 constant.
The racecar boost chain is not: `FUN_00136F80` loads ASCII literals and packs
them at runtime with `FUN_001AEAA0` before the usual `FUN_001C9E50` bank
lookup against `0x0040B7F4`:

| audio-object slot | literal | VA | site | file |
|---|---|---|---|---|
| `+0xA4` | `BoostLoop` | `0x003AD404` | `0x00136F8F` | `awd_generic/boostloop.wav` |
| `+0xA8` | `fire` | `0x003AD3FC` | `0x00136FBF` | `awd_crashmod/fire.wav` |
| `+0xAC` | `BoostIn` | `0x003AD3F4` | `0x00136FE7` | `awd_generic/boostin.wav` |
| `+0xB0` | `BoostOut` | `0x003AD3E8` | `0x0013700F` | `awd_generic/boostout.wav` |

Two more ASCII names, `BoostBig` (`0x003AD3D0`) and `BoostSml`
(`0x003AD3DC`), are the **chained-boost stings** `FUN_00137600` picks between
on a piecewise curve of the meter fraction `veh+0x11D4 / DAT_003F72F0`
(breaks at 0.25 / 0.5 / 0.75, levels 0.24→0.42→0.70). They are not part of the
sustain and are **not ported** — the harness has no chain state to drive them
and guessing one would be an invention.

### 6.2 The second path, and why the loop is not positioned [C]

The mode singleton `0x00411560` starts the same sustain through
`FUN_00141D20`, this time with the *packed* names `BOOSTLOOP` (`0x0039C338`)
and `FIRE` (`0x0039C340`) — `docs/RE_SFX.md` §1.1's table — at

* gain **1.0** (`0x003B168C` @`0x00141DD9` / `0x00141F07`),
* the wave's native sample rate,
* `OR byte ptr [EAX+0x37],0x10` @`0x00141E4D` / `0x00141F7B` — the LOOP flag,
* and a position taken from `[0x0073C610]+0x204 +0x30..0x38`, i.e. **the
  camera**.

Head-locked, not 3D. That is why `b3_sfx_boost_tick()` plays the chain
unpositioned.

### 6.3 The levels are `[?]`, and here is the whole search

A nine-parameter ValueDB group `"Sound/Boost"` (`0x003AD4C0`) exists —
`Boost Volume`, `Boost Ready/In/Out/Loop/Chain Volume` and three `AI …`
variants — registered by `FUN_00136DA0` (`0x00136E1E`..`0x00136F6C`) against
BSS fields `0x00479EC0`/`EC4` and `0x0047A018`..`0x0047A030`, with cfg path
`"../export/ValueDB/Sound/Boost.cfg"` (`0x003AE778`). Two independent misses:

* the shipped `Data/vdb.xml` has **no override** for any of the nine (the
  cfg-path hash *is* in its filedef list, so the composition is right and the
  answer is genuinely "no override" — same chain as
  `tools/extract_car_vdb.py`);
* the BSS fields have no findable writer, so the compiled-in defaults are not
  in the image.

The module therefore runs the one level that *is* `[C]` — `FUN_00141D20`'s
1.0 — for all three parts, and says so.

### 6.4 What the port does

`b3_sfx_boost_tick(boosting, crashed)`, called once per frame with the
player's state:

```
rising edge   -> BoostIn (one-shot) + BoostLoop (looping voice)
falling edge  -> stop the loop + BoostOut
crashed       -> stop the loop, no release          GLUE (see §1: FUN_0017F730
                                                    gates all boost FX on
                                                    carObj+0x18FA, and the
                                                    crash bed owns the mix)
```

`B3_SFX_BOOST_IN` / `B3_SFX_BOOST_OUT` join the existing `B3_SFX_BOOST_LOOP`
in the module's table; `validate_sfx.py` §1 grew an ASCII-literal branch (the
base-40 alphabet has no lower case, so a mixed-case wave name is by
construction an ASCII literal) and proves each of the four the same three-step
way as the packed names: the literal is at the recorded VA, a `.text`
immediate inside the emitter points at it, and it reads back.

---

## 7. What the port does and does not claim

Recovered and reproduced exactly: the emitter source (corona type 8), the
three-sprite cascade with all twelve of its constants, the flicker generator's
shape, the size/colour/pool law, the five orange-flame car ids, the level
state machine, the quad construction, and the four wave names with their bank.

`GLUE` / `[S]`, all marked at their sites:

| item | mark | why |
|---|---|---|
| additive blending for the sprite pool | GLUE | the Xbox blend factors are `[?]`, as for the coronas |
| `fxByte := boosting` in the level law | `[S]` | `(*(veh+0xCC4))+0x1033`'s writer not chased |
| part-matrix blend collapses to identity | `[S]` | true for an undamaged car; the harness has no part matrices |
| the flicker seed counter | GLUE | `[renderCtx+0x64550]` is a runtime global; the generator itself is `[C]` |
| `DAT_0054F46C = 2^-32` | `[S]` | BSS; the same idiom in `FUN_0014A6B0` fixes the scale |
| model-space Z flip of the emitter table | GLUE | this harness's world is the game world's Z mirror (`RE_NOTES` §12), identical to `b3_carfx_load_car` |
| boost gains = 1.0 | `[C]` for the loop, `[?]` for the levels | §6.3 |
| no BoostOut on a crash | GLUE | §6.4 |
| `BoostBig` / `BoostSml` chained stings | not ported | §6.1 |

No reference capture in `REFERENCE IMAGES/` shows a boosting car, so the
flame's on-screen magnitude was **not** eyeball-tuned against retail — every
number in it is the recovered constant.

---

## 8. Reproduce

```bash
python3 tools/extract_boostfx_art.py   # coronaboost / coronaboostred
python3 tools/validate_boostfx.py      # 77/77
python3 tools/validate_sfx.py          # 285/285 (243 + 42 boost-audio)
```
