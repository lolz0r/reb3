# World post-FX — the sky and the speed blur

Two world-level features the harness lacked, recovered from the retail Xbox
executable and checked against the xemu reference captures in
`REFERENCE IMAGES/` (real Silver Lake gameplay, `US/C3_V1`).

Evidence marks are the project's: **[C]** confirmed against the image at the
cited address, **[S]** strongly supported but not proven, **[?]** unknown,
**GLUE** = harness-side invention, always labelled.

Port: `src/burnout3_postfx.c` / `.h`.
Art extractor: `tools/extract_postfx_art.py`.
Test: `tools/validate_postfx.py` — **122/122**.

---

## 0. Summary of the verdict

| Claim | Mark |
|---|---|
| The sky is **not** track geometry — no material in any of the 40 shipped `static.dat` files names a sky/cloud texture | [C] (validator B4, 40 tracks) |
| The sky is a **procedurally generated two-hemisphere dome**, built once at race init by `FUN_00032020` | [C] |
| 33 rings × 8 rows = 264 vertices per hemisphere, stride `0x20`, 1344 (`0x540`) indices, `D3DPT_TRIANGLELIST`, 448 triangles | [C] |
| Every generator constant (azimuth step, dome parameter step, the two v-ranges, the skirt drop, the tc1 scale) | [C], addresses below |
| The dome is centred on the camera and uniformly scaled by `farClip − 1000` | [C] |
| Its stage-0 texture is a **runtime 64×32 gradient LUT**, its stage-1 texture the per-track cloud panorama | [C] |
| The per-track art lives in `Tracks/<r>/<t>/enviro.dat`, four role-fixed slots at `env+0x98..+0xA4` | [C] (validator B2/B2b, 40 tracks) |
| The LUT is regenerated per frame by `FUN_001891F0` from the 32×32 `gradients` source (pass 1 = a stretched column, pass 2 = a sun-glow band, pass 3 = a second column) | [C] for the draw calls, [S] for the pass-2/3 semantics |
| The sky/cloud **combiner** is a `Graphics/*.bum` pixel shader | [?] — not decoded |
| The speed blur is a **per-screen radial zoom** object built by `FUN_0002EBE0`, holding the `radialblurmask` texture, centred at (0.5, 0.5), with zoom layers 0.99 and 0.9999 | [C] |
| The whole in-race scene renders **off screen**, into a 640×480 `LIN_A8R8G8B8` surface at renderer+0x890 | [C] |
| It is reduced 640×480 → 320×240 → 160×120 → 160×120 (`LIN_R5G6B5`) by `FUN_0003E520`, the radial zoom applied in the last pass | [C] |
| The present pass `FUN_0003DA90` composites **`out = 2 × (scene + C0.a × blur)`** into the back buffer, one full-screen quad, blending OFF | **[C] — the present is OVER-UNITY** |
| `C0.a = min(s, 2) × 0.5`, `s` = the float at blurState+0x54 | [C] |
| The **producer** of `s` (the blur's strength-vs-speed law) | **[?] — still NOT recovered.** The port's ramp is GLUE, fitted to the captures |
| The output gamma ramp `round((i/255)^0.95×255)` is installed once at renderer construction | [C] |

---

## 1. The sky is not in the track data

The obvious first hypothesis — a sky backdrop group filtered out by
`extract_track.py`'s local-cell filter — is **false**.

`tools/extract_track.py`'s `parse_materials()` was run over every shipped
`static.dat`. US/C3_V1 has 167 resolvable material records and AS/C1_V1 has
180; **none** of them, on **any** of the 40 tracks, names a texture beginning
`sky`, ending `sky`, or containing `cloud`. The backdrop groups are what their
names say they are — `GL_BD1`, `GL_BD2b`, `GL_BD6`, `GL_BD9_panb`, `BD_bridge`
on Silver Lake, all shader class 6 — distant *building/terrain* cards, not sky.

`tools/validate_postfx.py` section B4 re-runs that scan on every track so the
negative cannot silently rot.

The sky comes from a file no existing tool touched: **`enviro.dat`**, one per
track directory.

---

## 2. `enviro.dat` — the per-track sky art [C]

`FUN_001888F0` (`0x001888F0`) is the loader. At `0x001889D5` it appends the
literal `"enviro.dat"` (string at `0x003B0444`) to the track directory and
loads the file. Like `Gamedata.bgd`, the file is a **baked memory image**: the
environment record starts at offset 0 and its internal pointers are
file-relative offsets that get relocated on load.

`FUN_00188880` (`0x00188880`) does the relocation, for exactly four fields,
handing each to the texture registrar `FUN_001C8E20`:

| field | relocate + register | published to |
|---|---|---|
| `env+0x98` | `0x00188893` | `DAT_0045BC10` @ `0x00188A9B` — the **gradient LUT source** |
| `env+0x9C` | `0x001888A9` | `DAT_0045D11C` @ `0x00188AA7` — sky-dome stage 1 |
| `env+0xA0` | `0x001888BF` | `DAT_0045D118` @ `0x00188AAD` — ground/env-map stage 1 |
| `env+0xA4` | `0x001888D5` | gates the `"sunblob"` sprite registration @ `0x00188BBA` |

Each points at an ordinary Burnout texture record (the layout
`tools/extract_textures.py` documents). `tools/extract_postfx_art.py` decodes
them; a blind full-file scan finds **exactly** the same records the header
names, on all 40 tracks — the independent confirmation that `+0x98..+0xA4` is
the right place.

**The slots are role-fixed; the artists' names are not.** [C]

| slot | role | invariant across all 40 tracks | example names |
|---|---|---|---|
| `+0x98` | sky-gradient LUT source | always **32×32 DXT5** | `gradients`, `bk_sky`, `skygrad1`, `P2P1_SkyGradient` |
| `+0x9C` | sky cloud panorama | always **1024 wide DXT5** (128 or 256 tall) | `clouds`, `bk_skyclouds`, `Clouds_prestorm_01` |
| `+0xA0` | env-map cloud sheet | present on 36/40; absent on AS/C1_V1, C1_V2, C2_V1, C2_V2 | `envmapclouds`, `WC_SkyEnv` |
| `+0xA4` | sun sprite | always **128×256 paletted (fmt 0xB)** | `suncorona`, `bk_suncorona`, `P2P1_Sun` |

Hence the extractor writes files named by **role**, not by the artist's name,
so the runtime finds the right texture on any track.

Decoded Silver Lake art (`build/postfx/US_C3_V1_*.png`):

* `gradients` 32×32 — the sky **colour ramp**. Rows 0..22 run deep blue
  `(24,34,109)` at the zenith through `(231,235,247)` haze; rows 23..31 are the
  darker below-horizon colours. Columns are a **condition/time ramp** (see §3.4)
  and the alpha channel carries the sun-glow mask.
* `clouds` 1024×128 — a greyscale cloud panorama (luma 74..255, mean 150) with
  the coverage in the alpha channel (0..220), fading to alpha≈0 at both the top
  and bottom rows.
* `envmapclouds` 512×128 — the reflection sheet (not used by the sky pass).
* `suncorona` 128×256 paletted — palette lives outside the record; **[?]** not
  decoded here.

---

## 3. The sky dome

### 3.1 Where it happens in the frame [C]

The in-race world draw is `FUN_001AE340` (`extract_track.py` documents its pass
order). The sky bookends the world geometry:

```
0x001AE375  FUN_0019D100   build the visible-cell arrays
0x001AE3EA  FUN_00189040   SKY PASS  -> FUN_00032580 with mode 6      <-- before the world
0x001AE460  FUN_001AD7A0   streamed opaque + backdrop + water
0x001AE46A  FUN_00188E10   SKY PASS  -> FUN_00032580 with modes 0, 1  <-- after opaque
0x001AE4AE  FUN_001ADD60   streamed alpha
0x001AE4E0  FUN_001AE200   chevrons
```

`FUN_00189040` (`0x00189040`) issues `EBX = 6` then `EBX = 5`;
`FUN_00188E10` (`0x00188E10`) issues `EBX = 0` then `EBX = 1`. Inside
`FUN_00032580` only modes **1, 4 and 6** reach the draw — `0`, `2` and `5`
return at `0x000327FE` after the stream-source setup. Both bracketing calls are
gated on the environment object's `+0x14C` byte (enviro loaded).

Mode selects the shader and the stage-1 texture:

| mode | vertex shader | stage-1 texture |
|---|---|---|
| 1 | `DAT_004D65B4` @`0x00032689` | `DAT_0045D11C` = `clouds` |
| 4 | `DAT_004D65B8` @`0x000326E5` | `DAT_0045D118` = `envmapclouds` |
| 6 | `DAT_004D65B4` @`0x0003270D` | `DAT_0045D11C` = `clouds` |

Stage 0 is always the runtime LUT: `0x0003259A..0x00032613` computes
`DAT_0075DB70 := (DAT_0045BC10 != 0) ? DAT_004A1D04 : 0`, i.e. bind the LUT
only when the enviro art loaded.

### 3.2 The draw [C]

```
00032633  PUSH 0x20                 vertex stride
00032651  PUSH DAT_004A1CF8         the vertex buffer
0003267B  CALL FUN_0034EDB0         SetStreamSource(0, vb, 0x20)
00032742  CALL FUN_001CF153         worldMatrix . DAT_004D66F0 (view-proj)
0003275D..0003279D                  transpose, then set 4 shader constants at reg 0x70
000327A7  diffuse constant (0.5, 0.5, 0.5, 1.0)          <-- MODULATE2X pairing
000327E9  PUSH 5                    D3DPT_TRIANGLELIST
000327E4  PUSH 0x540                1344 indices
000327DD  PUSH DAT_004A1CFC         the index buffer (= &DAT_0045BC18)
000327EB  CALL FUN_001D7D10         DrawIndexedPrimitive
```

The world matrix is a pure **uniform scale + translate to the camera**
(`0x000325C7..0x00032664`): rows 0/1/2 diagonal = `S`, row 3 = the camera
position `DAT_004D67D0/D4/D8`, row 3 w = 1.0, with

```
S = DAT_004D67E0 - 1000.0            0x000325AB..0x000325BB, literal at 0x003B16CC
```

`DAT_004D67E0` is the view far clip **[S]** — `FUN_0002ECC0` stamps `10000.0`
as the far plane at `0x0002ED??`, so the dome sits 1000 units inside it.

Render state around the draw, from `FUN_000323D0` / `FUN_000324A0`
(the deferred shadow scheme of RE_FRONTEND 6.7.1):

| state | before the draw | restored to |
|---|---|---|
| `0x3B` ALPHATESTENABLE | 0 | — |
| `0x3C` ALPHABLENDENABLE | 0 | — |
| `0x40` ZWRITEENABLE | **0** | 1 (via `0x0003DA90`'s epilogue / `0x000324A0`) |
| `0x93` CULLMODE | **0 = D3DCULL_NONE** | `0x900` |
| `0x8F` | 1 | 1 |

So: opaque, two-sided, **no depth writes** — the world draws over it. The port
does exactly this.

### 3.3 The generator, `FUN_00032020` — transcribed 1:1 [C]

Called twice from `FUN_001A9C50` (`0x001A9FEF` with arg 1, `0x001A9FF5` with
arg 0) into one `0x41C0`-byte buffer (`PUSH 0x41C0` @ `0x001A9FA8`).
`0x41C0 = 2 × 263 × 0x20`: two hemispheres, one buffer.

```
outer: 33 rings           local_aa0 = 0x21     @0x0003203D
inner:  7 dome rows       local_ab0 = 7        (+1 skirt vertex per ring = 8 rows)

theta = ring * 0.19634954                    DAT_004D916C  (= 2*pi/32)
u0    = theta * 0.15915494                   (= 1/(2*pi))
skirt vertex:
   pos = (cos t, s * -0.25, sin t)           literal -0.25 @0x0003206A
   tc0 = (u0, -0.25*(vHigh - vLow) + vLow)
dome vertex k = 0..6:
   a    = (k * 0.2617994) * 0.63661975       DAT_004D9174 (= pi/12), 2/pi
        = k/6                                exactly, so a runs 0 -> 1
   yhat = 2a - a*a                           (0 at the horizon, 1 at the pole)
   rad  = 1 - a*a                            (1 at the horizon, 0 at the pole)
   pos  = (cos t * rad, yhat * s, sin t * rad)
   tc0  = (u0, yhat*(vHigh - vLow) + vLow)
   tc1  = (u0 * 2, 1 - yhat * 1.1764705)     sky half only; 1.1764705 = 1/0.85
colour = 0x0000FF00 for every vertex         (the decompiler shows it as the
                                              denormal 9.14768e-41)
```

with the sign and v-range chosen by `param_1`:

| `param_1` | `s` | `vLow` | `vHigh` | source | LUT rows it owns |
|---|---|---|---|---|---|
| 1 (sky) | +1 | `DAT_004D7058` = 0.765625 | `DAT_004D9154` = 0.015625 | `0x0025EA40` / `0x0025EA60` | 24.5 → 0.5 |
| 0 (ground) | −1 | `DAT_004D9164` = 0.765625 | `DAT_004D9168` = 1.015625 | `0x0025EA80` / `0x0025EAA0` | 24.5 → 32.5 |

Those v values are **exact texel centres of a 32-row texture**:
`0.015625 = 0.5/32`, `0.765625 = 24.5/32`, `1.015625 = 32.5/32`. The sky half
owns rows 0..24 of the LUT and the ground half rows 24..32, meeting on a shared
horizon ring at texel 24.5. The skirt of each half reaches 0.25 *past* the
horizon into the other half's rows, which is why there is no seam.

**Indices** (`0x000322FD..0x00032397`): 16 columns, each emitting the quad
strip for rings `c`/`c+1` and then rings `c+16`/`c+17` (the `+0x80 = 16*8`
vertex offset), 7 quads of 6 indices —
`16 × 2 × 7 × 6 = 1344 = 0x540`, matching the draw count exactly.

**One honest gap.** Retail's inner loop writes `tc1` (`[0xd]`/`[0xe]`) only for
the 7 dome vertices, so each ring's *skirt* vertex keeps whatever was in that
memory — an uninitialised read. The port gives the skirt the value the formula
would produce at `yhat = -0.25` (the continuous extension). **GLUE**, and it
only affects the cloud layer 0.25 units below the horizon, which world geometry
covers.

### 3.4 The 64×32 gradient LUT

`FUN_001A9C50` @ `0x001A9FFD` creates it:
`CreateTexture(w = 0x40, h = 0x20, levels = 1, format = 6)` → `DAT_004A1D04`.
Width 64, height 32 — the height matching the dome's v texel space exactly.

`FUN_001891F0` (`0x001891F0`) rebuilds it every frame, before the world draw,
by drawing the 32×32 `gradients` source (`DAT_0045BC10`, bound as stage 0 at
`0x00189273`; it is a **paletted** texture and `FUN_0034DE60` sets its palette
from `+0x14` when `+0x69` is non-zero) into the 64×32 target through
`FUN_001C7430` three times: [C for the calls]

1. **one quad**, dest `(0,0)-(64,32)`, source u **degenerate** — the same
   column on both edges — v `0.015625 .. 1.015625`. i.e. **one column of
   `gradients`, stretched over the whole LUT.** The column is
   ```
   u = progress * 0.46875 + 0.015625        0x001892E1..0x001892FB
     = texel 0.5 + 15*progress   of 32
   progress = (DAT_0073B5EC + DAT_0073B5E8) / *(DAT_0073A164 + 8)      [S]
   ```
   and `u = 0.015625` (the leftmost column) when `DAT_0073A164` is null.
   The identity of that ratio is **[S]** — it is a race/time fraction; the port
   takes it from the caller.
2. **13 quads**, dest y `0..24` in 2-row bands, x `0..64`, source u spanning a
   full wrap `(15.5 − k) .. (16.5 − k)` and v warped by
   `clamp((0.5 − ((1 − sqrt(y/24)) − X)) * 24, ≤24) / 32`. This is the
   azimuthal **sun-glow** band (`k` comes from `FUN_00189660`). **[S]** for the
   semantics; **not implemented** in the port.
3. **one quad**, dest `(0,0)-(64,32)`, another degenerate column at `X + 0.5`.
   **[?]** not identified; **not implemented**.

Because pass 1 leaves the LUT **horizontally uniform**, the dome's
`tc0.u = theta/(2*pi)` is irrelevant to the result of pass 1 alone. The port
therefore binds the 32×32 `gradients` texture directly and feeds the dome
`tc0 = (u_column, v)` — which reproduces retail's pass 1 **exactly**, with no
render-to-texture needed. Passes 2 and 3 are the difference between the port
and retail, and they are the reason retail's sky brightens toward the sun.

### 3.5 What the port draws

```
sky pass A  gradients LUT, both hemispheres, opaque, ZWRITE off, cull off
sky pass B  clouds sheet, sky hemisphere only, alpha-over, RGB doubled
```

The doubling is the recovered `(0.5, 0.5, 0.5, 1.0)` diffuse constant
(`0x000327A7`) read as the classic **MODULATE2X** pairing: the cloud sheet is
authored mid-grey (mean luma 150) and doubling is what turns it into the white
plates the captures show. The **exact combiner is a `Graphics/*.bum` pixel
shader and is [?]** — the alpha-over + `GL_RGB_SCALE 2` in the port is the
closest fixed-function equivalent and is GLUE.

Cloud addressing: S repeats (`tc1.u = theta/pi` wraps the panorama twice around
the dome), T clamps so the zenith's `tc1.v = -0.176` does not wrap the cloud
band over the top. Retail's addressing mode is a texture-stage state that was
**not** recovered **[?]**.

The dome's Z is negated on emit, the same single reflection
`RE_NOTES §12` applies to all world data, so the cloud panorama runs the same
way round as the world.

---

## 4. The speed blur

### 4.1 What was recovered [C]

The global texture dictionary contains a texture named **`radialblurmask`**
(string at `0x003AAFE8`, one reference, from `FUN_0002EBE0` @ `0x0002EC3C`).

`FUN_0002EBE0` (`0x0002EBE0`) is a constructor for a **0xA0-byte per-screen
radial-blur state**:

| offset | value | source |
|---|---|---|
| `+0x00..+0x0C` | 0.5, 0.5, 0.5, 0.5 | `0x003B1684` |
| `+0x10, +0x14` | **0.99, 0.99** | `0x003B1758` |
| `+0x18` | 1.0 | `0x003B168C` |
| `+0x1C..+0x28` | 0 | |
| `+0x30` | byte 0 | |
| `+0x40` | **`radialblurmask` texture handle** | `FUN_0002DDF0("radialblurmask")` |
| `+0x60..+0x6F` | 0 | |
| `+0x70, +0x74` | 0.5, 0.5 | `0x003B1684` |
| `+0x78, +0x7C` | **0.9998999834** | `0x003B18B4` |
| `+0x80` | 0 | |
| `+0x84` | `DAT_004AE200` (a time base) | |
| `+0x90, +0x94` | 0 | |
| `+0x98` | 1.0 | |

Two instances are constructed, at `screenMgr+0x1D0` (`0x0002F2BF`) and
`screenMgr+0x270` (`0x0002F2CA`), by `FUN_0002F260` — the screen-manager
constructor, which also loads `UnbrokenGlass` / `CrackedGlass` /
`SmashedGlass` / `VehicleUnderside`.

`FUN_0002F330` (`0x0002F330`), reached from the world draw via
`FUN_0003D9E0` (`0x001AE3A9` → `0x0003D9EC`), publishes the active pair for the
screen being drawn:

```
mode 0 (full screen)   viewport = mgr+0x310, blur = mgr+0x1D0
mode 1 (split, top)    viewport = mgr+0x90,  blur = mgr+0x1D0
mode 2 (split, bottom) viewport = mgr+0x130, blur = mgr+0x270
mgr+0x3B0 := viewport ; mgr+0x3B4 := blur
```

So: **one radial-blur state per player screen**, holding a mask texture, a
centre at screen centre `(0.5, 0.5)`, and two zoom layers — a coarse one
stepping the frame in by 1% and a near-unity one at 0.9999.

### 4.2 What was NOT recovered [?]

**The strength-vs-speed law.** No consumer of `mgr+0x3B4` was found in this
session. A `disp32 == 0x3B4` scan of `.text` produced 16 hits, of which only
`FUN_0002EF90` (the field's zero-init) and `FUN_0002F330` (the write) touch
this structure at all; the rest are stack offsets or unrelated objects. The
draw that consumes the state — and therefore the mapping from speed/boost to
blur amount, and the way the mask is sampled — is still open.

**The pixel-side mechanism.** The mask lookup and the accumulation combiner are
in the `Graphics/*.bum` shader bundles (17 files, 43008 bytes each, referenced
through the format string `"Graphics/%d.bum"` at `0x0039E4??`). Not decoded.

**Whether it is single-shot or feedback.** The pairing of a 0.99 layer with a
0.9999 layer is *suggestive* of a per-frame feedback accumulation (0.9999 per
frame is meaningless as a one-shot tap but sensible as a feedback step), but
that is a hypothesis and is recorded as such — it is **not** evidence.

### 4.3 What the port does

Recovered structure, GLUE parameters, everything labelled in the header:

* grab the finished frame to a texture (`glCopyTexSubImage2D`);
* draw `B3_BLUR_TAPS` screen quads, tap *k* sampling the frame **zoomed by
  `0.99^k` about (0.5, 0.5)** — the recovered zoom step and centre;
* alpha per tap `strength * mask / (k+1)`, a running average biased toward the
  sharp frame;
* the mask is evaluated per vertex of a 12×12 screen grid — a GLUE stand-in for
  the `radialblurmask` texture lookup, which keeps the screen centre sharp and
  smears the edges, exactly the character the captures show. If a
  `build/textures/radialblurmask.png` ever appears, that is the thing to
  replace it with.

The strength ramp (`b3_postfx_blur_strength`) is **GLUE fitted to the reference
captures**, not recovered:

```
t        = clamp((mph - 30) / (120 - 30), 0, 1) ^ 2
boost    = clamp(1 - (racecar+0x11AC - 1)^2, 0, 1)     <-- the recovered FOV ramp's shape
strength = 0.85 * t * (1 + 0.45 * boost)
```

`racecar+0x11AC` is the same boost ramp the **recovered** FOV 90→110 law
consumes (RE_TAKEDOWN_FX §9.2). Reusing it here is a **modelling choice**, not a
recovered coupling — the port would be equally correct with `boost_ramp = 0`.

Fit targets from the captures: 0 mph sharp; 37/42/52 mph faint edge streaks;
60 mph moderate; 85/103 mph strong, with the car at screen centre still sharp.
`B3_BLUR_TAPS = 16` gives `0.99^16 = 0.851`, a 15% edge displacement, which is
what the 85/103 mph captures show.

---

## 4b. The present composite — where the scene actually goes [C]

The blur is only half of a larger mechanism, and the other half is the missing
global brightness step the world-render waves had been hunting.

### 4b.1 The scene is rendered off screen

`FUN_0003D890` (`0x0003D890`), called from the renderer constructor, creates
four surfaces on the renderer object (base `0x004D6170`):

| slot | size | format | role |
|---|---|---|---|
| `+0x890` | 640×480 | `0x12` = `LIN_A8R8G8B8` | **the scene render target** |
| `+0x8A8` | 320×240 | `0x11` = `LIN_R5G6B5` | reduction 1 |
| `+0x8C0` | 160×120 | `0x11` | reduction 2 |
| `+0x8D8` | 160×120 | `0x11` | the blur the present pass samples |

```
0003d89b  6A 12 68 E0 01 00 00 68 80 02 00 00   push 0x12 / push 480 / push 640
0003d8a7  LEA EDI,[EBX + 0x890]                 -> the scene surface
0003d8e2  6A 11 6A 78 68 A0 00 00 00            push 0x11 / push 120 / push 160
0003d8eb  LEA EDI,[EBX + 0x8d8]                 -> the blur surface
```

and `FUN_0003FA20` (`0x0003FA20`), the first thing the in-race screen render
`FUN_001AE340` calls, points the device at it:

```
0003fa3c  MOV ECX,[EAX + 0x868]     ; the depth surface
0003fa43  ADD EAX,0x890             ; the scene surface
0003fa48  PUSH EAX / CALL 0x0034cbf0  ; D3DDevice_SetRenderTarget
```

(The same two-line idiom appears at `0x000402AB` and `0x0003FDED`, the restore
paths.) `+0x890` is created with the common-type tag `0x00050001`
(`D3DCOMMON_TYPE_SURFACE`), which is why it can be both a render target and a
texture — it is also the surface class 3 samples for its planar reflection
(`DAT_004D6A00` = `0x004D6170 + 0x890`, `FUN_000393C0` @`0x00039A7D`).

### 4b.2 The reduction chain

`FUN_0003E520` (`0x0003E520`) runs three passes, each `SetRenderTarget` +
full-screen quad:

1. **640×480 → 320×240**, all four samplers bound to `+0x890`
   (`0x0003E960..0x0003E97F`), tap offsets ±0.5 texels (`0x003B1684`,
   `0x003B16A4`) — a 2×2 box.
2. **320×240 → 160×120** (`0x0003ED93`), four taps at ±0.9285714 and
   ±2.8333333 texels (`0x003B1F0C/0x003B1F08/0x003B1F10/0x003B1F04`) — a wider
   separable kernel. The exact weights are a signed-combiner construction that
   was **not** fully decoded [?].
3. **the radial pass** into `+0x8D8` (`0x0003F3A7`), vertex shader
   renderer+0x490, pixel shader renderer+0x4A0 (def `0x003EA248`), which is
   where the `radialblurmask` taps live.

The whole chain is energy conserving by construction — it is a blur. **It is
not where the brightness comes from.**

### 4b.3 The present pass, and the ×2

`FUN_0003DA90` (`0x0003DA90`) is called last, from both screen renderers
(`0x001AE615`, `0x001AEA70`). Its first act is
`FUN_000402C0` (`0x000402C0`), which does
`SetRenderTarget(device+0x1A14, this+0x868)` — **the back buffer**. Then:

```
0003dc2b  MOV ECX,[ESI + 0x4a4] / PUSH / CALL SetPixelShader
0003dc37  MOV EAX,[EBP + 8]           ; the blur state (blurObj + 0x50)
0003dc3a  MOVSS XMM0,[EAX + 0x4]      ; s
0003dc42  COMISS XMM0,[0x003b1688]    ; 2.0
0003dc4b  MOVSS XMM0,[0x003b168c]     ;   -> 1.0 when s > 2
0003dc5a  MULSS XMM0,[0x003b1684]     ;   -> s * 0.5 otherwise
0003dc89  CALL 0x0034e9a0             ; SetPixelShaderConstant(0, (0,0,0,a))
0003dc96  MOV [0x0075db70],EDI        ; stage 0 := renderer+0x890  (the scene)
0003dc9c  MOV [0x0075db74],EAX        ; stage 1 := renderer+0x8D8  (the blur)
0003dc19  MOV [0x0075d590],EDX        ; RS 0x3C ALPHABLENDENABLE := 0
0003dd13  C7 00 FC 17 04 00           ; NV097_SET_BEGIN_END ...
0003dd19  C7 40 04 08 00 00 00        ;   ... = 8 (QUADS): ONE quad
```

The quad's texcoords span (0,0)–(640,480) on attribute 9 and (0,0)–(160,120)
on attribute 10 (`0x003B1F00`, `0x003B1EEC`, `0x003A49FC`, `0x003A1A00`), i.e.
both surfaces 1:1 over the screen.

The pixel shader is renderer+0x4A4, loaded from the `D3DPIXELSHADERDEF` at
**`0x003E9EA8`** (`0x0003D175`: `MOV ESI,0x3E9EA8 / MOVSD.REP / MOV
[EBX+0x4A4]`). It decodes to:

```
PSTextureModes = 0x00000021   two PROJECT2D samplers, T0 and T1
PSCombinerCount= 0x00011101   one stage
stage0 RGB in  = 0xC9D120C8   A = T1.rgb   B = C0.a
                              C = ZERO|UNSIGNED_INVERT = 1
                              D = T0.rgb
stage0 RGB out = 0x00010C00   SUM -> R0
                              OP field (bits 15..17, mask 0x38000) = 2
                              = NV097 ..._OP_SHIFTLEFTBY1 = x2
stage0 A   in  = 0xD8301010   R0.a = T0.a          out = 0x000000C0 (no shift)
final ABCD     = 0x00000C00   out.rgb = R0.rgb
final EFG      = 0x00001C80   out.a = R0.a, CLAMP_SUM
```

```
   out.rgb = 2 * ( scene.rgb  +  C0.a * blur.rgb )
```

**The presented frame is twice the render target, even when the blur is
idle.** The OP field is the same field, read the same way, that TRACK-LIGHT
and WORLD-SPEC independently settled as ×2 on the six world defs
(`PSRGBOutputs[0] = 0x000100C0`); the two words differ only in the destination
nibbles. `tools/validate_postfx.py` section A5 asserts both readings and the
cross-check.

### 4b.4 What that means for the port

Retail's *scene* is therefore authored to live in the bottom half of the
range, and the present pass expands it. Every recovered per-object equation in
this port — the world's `2·T·V` with `V ≤ 0.5`, the sky dome's `T0`, the car
combiners — is a render-target equation, so the port is **missing a global
×2**, which is exactly the 1.45×–2.0× world deficit DECAL-FOG measured.

It is implemented (`b3_postfx_blur`, `B3_PRESENT_SHIFT`) and it is **off by
default**, because the port is not yet uniformly in render-target space:
measured percentile-for-percentile against the xemu captures, our frame sits
at 1.2×–1.7× of retail's render target rather than 1.0×, so switching the ×2
on today clips 21% of the frame at 255 where the retail captures clip 0.11%.
The excess is concentrated in the sky (57% of the sky band clips; the near-road
band only 4.7%). `B3_POSTFX_PRESENT=1` enables it for A/B work.

---

## 4c. The output gamma ramp [C]

`FUN_0003C8A0` builds a 256-entry ramp inline and installs it with
`D3DDevice_SetGammaRamp`:

```
0003d3f0  FILD  dword [esp+0x10]        ; i = 0..255
0003d3f4  FMUL  float [0x003b16ac]      ; * 1/255
0003d3fa  FLD   double [0x003b20f8]     ; 0.949999988079071
0003d400  CALL  pow
0003d405  FMUL  float [0x003b16c4]      ; * 255.0
0003d40b  CALL  round
0003d410  MOV   byte [esi+0x45d1a8], al ; red
0003d416  MOV   byte [esi+0x45d2a8], al ; green
0003d41c  MOV   byte [esi+0x45d3a8], al ; blue
```

`ramp[i] = round((i/255)^0.95 * 255)`, one table for all three channels,
written once at construction. Exponent < 1 ⇒ a midtone/shadow lift.

**In the harness this is a GL pass, not an SDL call.** `SDL_SetWindowGammaRamp`
is not a viable delivery mechanism: measured on this machine,

```
video driver = wayland    SDL_SetWindowGammaRamp -> -1  "That operation is not supported"
video driver = offscreen  SDL_SetWindowGammaRamp -> -1  "That operation is not supported"
video driver = x11        SDL_SetWindowGammaRamp ->  0   (an XWayland virtual CRTC LUT)
```

and even where it succeeds it is a *display* transform, so `glReadPixels`
still returns pre-ramp pixels and every capture path has to reproduce it
separately — which is how the harness ended up with a live window and a
screenshot that were not the same image. `b3_postfx_gamma()` uploads the table
as a 256×1 `GL_NEAREST` texture and runs one full-screen quad through a
three-lookup fragment program at the end of `render_frame()`, after the HUD.
For an 8-bit frame `floor((i/255)·256) == i` for every `i`, so the dependent
lookup reproduces retail's *quantised* byte table exactly rather than a
continuous `pow`. Verified: on the live Wayland window and under the offscreen
driver, the GL result equals the software table on 99.3% of pixels, the
remainder being one wall-clock-animated HUD element that differs between runs.

---

## 5. Acceptance

`tools/validate_postfx.py` — **122/122**, three sections:

* **A. image bytes** — 9 float constants read back at their literal addresses
  and cross-checked against the header's `#define`s; 8 instruction-byte
  patterns (`PUSH 0x540`, `PUSH 5`, `PUSH 0x20`, `DAT_0045D120 := 0x540`,
  `PUSH 0x41C0`, `CreateTexture(0x40, 0x20, 1, 6)`, `PUSH 0x3AAFE8`, the
  `farClip − 1000` sequence); the `radialblurmask` string itself; and the
  arithmetic identities `32 × 0.19634954 = 2π` and `6 × 0.2617994 × 2/π = 1`.
* **B. shipped data** — all 40 tracks: `env+0x98..+0xA4` resolve, a blind
  full-file scan agrees with the header slots, the slot **roles** are invariant
  while the names are not, and no `static.dat` material anywhere is a sky.
* **C. executed port** — `burnout3_postfx.c` is compiled with
  `-DB3_POSTFX_NO_GL -Werror` into a shared object and **run**; its 264
  vertices and 1344 indices are compared element-by-element against an
  independent Python transcription of `FUN_00032020` written from the
  disassembly (not from the C), plus geometric invariants (radius falls 1→0,
  the horizon ring exists at texel 24.5 in both halves, y ranges, index bounds)
  and the blur law's endpoint/monotonicity behaviour.

`sizeof(B3SkyVertex) == 32` is asserted, so the port cannot silently drift off
the game's stride.

**Visual acceptance** (scratchpad `postfx/`):

| sheet | what it shows |
|---|---|
| `sidebyside_sky.png` | retail 0 mph vs harness 0 mph — blue gradient, white cloud plates, hazy horizon |
| `sidebyside_blur.png` | retail 103 mph vs harness 96 mph |
| `sidebyside_blur_ab.png` | the same harness frame at 96 mph with `B3_POSTFX_BLUR=0` and `=1` |

Before this module the harness cleared to flat `(0.3, 0.5, 0.8)`.

---

## 6. Open items

0. **Getting the port into render-target space so the ×2 can be switched on.**
   This is now the top item and it is not in this module: with
   `screen = 2 × RT` proven, our frame is 1.2×–1.7× of retail's RT. The
   leading suspects, in order: the texture decode (`tools/extract_textures.py`
   — TRACK-LIGHT's §7 lead 1 already flags our road textures as suspiciously
   bright), then this module's own unimplemented LUT passes (item 2 below),
   which is where the excess is worst (the sky band is ~1.7× hot and clips
   57% under the ×2 while the near-road band clips 4.7%).
1. **The blur's strength law** — find the *producer* of `blurState+0x54`. The
   consumer is now recovered (`min(s,2)*0.5`), so only the writer is missing.
   A whole-image scan for a float store at that offset, both through the
   published pointer `DAT_004D6524` and through the renderer base as
   `[reg+0x224]`, finds nothing, so it is written through a pointer to the
   `+0x50` sub-struct whose owner was not identified. Suggested attack: the
   `Graphics/*.bum` bundles, or a Unicorn trace with the renderer
   instrumented.
1b. **The mask sampling** — `radialblurmask` lives in the global texture
   dictionary (`FUN_0002DDF0`), which no extractor touches yet. Its shape is
   what decides whether the additive blur brightens the whole screen or only
   the edges; the port assumes the latter (a radial ramp, GLUE).
1c. **`FUN_0003C810`** saves the current render target and re-points it at
   `renderer+0x87C + i*4`, and the in-race renderer calls it with `i = 1`.
   Those four surfaces are 256/128/64/32 square `R5G6B5` textures created by
   the same `FUN_0003D890` loop, and the sky call `FUN_00189040` sits between
   the matching `FUN_00040500` / `FUN_000405F0` pair — so this is almost
   certainly the car's reflection-environment chain rather than anything on
   the scene path [S]. It was not chased further because the scene target is
   pinned independently by `FUN_0003FA20` and the present target by
   `FUN_000402C0`.
2. **LUT passes 2 and 3** — the sun-glow band (`FUN_001891F0`'s 13-quad strip)
   and the second column pass. Needs `FUN_00189660` (which produces the sun
   azimuth `k`) and `FUN_001C7430`'s exact 2D quad semantics.
3. **`suncorona`** — a paletted texture whose palette is outside the record;
   `FUN_00188BBA` registers it as `"sunblob"` and `FUN_00032D80` consumes it.
   The sun sprite is not drawn by the port.
4. **`envmapclouds` (`env+0xA0`)** — mode 4 of `FUN_00032580` draws the dome
   with it and a different vertex shader; that is the car reflection
   environment, not the sky, and is the car-FX side of the boundary.
5. **`FUN_00189040`'s middle block** — between the two sky draws it optionally
   runs `FUN_00032DC0(12.0, env+0x90)` + `FUN_00033F20`, gated on `env+0x13C`.
   Weather particles **[?]**.
