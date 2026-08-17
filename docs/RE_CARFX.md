# RE_CARFX — car appearance FX: body shine, shadows, light glow

Recovered from `build/burnout3.elf` (the correctly-mapped ELF of the retail
Xbox `default.xbe`) against the xemu reference captures in
`REFERENCE IMAGES/`. Implemented in `src/burnout3_carfx.{c,h}`; asserted by
`tools/validate_carfx.py` (**153/153**); art + per-car tables by
`tools/extract_carfx_art.py`.

Evidence marks, as elsewhere in this repo:

* **[C]** execution- or byte-verified — the retail code was run under Unicorn
  and produced the value, or the bytes were read out of the ELF / the retail
  `.bgv` files.
* **[S]** read from the disassembly, self-consistent, no differential case.
* **[?]** open.
* **GLUE** this port's own bridging; not the game's.

The execution oracle is
`scratchpad/carfx/trace_carfx.py`, which extends `tools/trace_panels.py`'s
`--deep` harness (imported read-only) with hooks on the D3D constant setters,
so every shader constant the car draw uploads is captured with its real
value. `tools/validate_carfx.py` re-runs the same emulation on every
invocation — sections 2 and 4 are not tables of remembered numbers, they are
produced by the retail code each time the validator runs. Sections 9 and 10
do the same for the environment feed: §9 decodes the nine SH coefficients out
of the three code sites that write them, §10 re-reads the sun colour out of
every shipped `enviro.dat`.

---

## 0. Headline results

1. **The car body has no environment map.** The "shine" is an **L2 spherical
   harmonic irradiance environment**, evaluated per vertex/pixel as
   `E(n) = nᵀ M n` from the Ramamoorthi–Hanrahan irradiance matrix. Only
   texture stage 0 (the paint raster) is ever bound in the car path. This is
   the answer to the "sphere-map / cube-map equivalence" question: there is
   no map to equate — the environment *is* the nine SH coefficients.
2. **Both environment inputs are now recovered** (they were `[?]`). The nine
   SH coefficients are a **per-position light probe**: `FUN_0019D400` samples
   a probe volume in the streamed track data at the car's own world position
   every frame and writes the nine floats into the model instance. The nine
   `.rdata` literals of §2.7 are the **default** it starts from and keeps on a
   miss. **That is the mechanism behind cars darkening under bridges and in
   tunnels** — measured at ~2× on the reference captures. See §12; the probe
   *container* layout is still `[?]`. The light RGB (`ps c14.xyz`) is
   **`enviro.dat` bytes 0x60..0x6B** — the track's sun colour, from the same
   file `docs/RE_POSTFX.md` decodes for the sky. See §2.8.
3. **The shadow is a blob quad**, not a projection or a stencil: an
   8-vertex triangle strip textured with `blobbyshadow`, four rows deep, the
   outer rows on the bounding box and the inner rows on the axles.
4. **The glow is a corona billboard system** fed by a real per-car light
   table inside the `.bgv` (12 types, 0x30-byte records of position +
   outward normal), coloured by seven constants in `.data`.

---

## 1. The car draw path (context)

```
FUN_0018DE00 (0x0018DE00)      per-car draw
  ├─ FUN_000303D0 (0x000303D0) per-model draw   EAX = LOD index
  │    ├─ FUN_000315C0 (0x000315C0) SH upload + stream binds
  │    │     └─ FUN_000313E0 (0x000313E0)  SH coefficients -> irradiance matrix
  │    ├─ FUN_0034DE60 (0x0034DE60) SetTexture(0, paint[paintIdx])
  │    ├─ FUN_000300A0 (0x000300A0) glass tier dressing (RE_NOTES §13)
  │    └─ FUN_00031E10 (0x00031E10) part draw -> per-record
  │          └─ FUN_00031AB0 (0x00031AB0) SHADER BRANCH + constant uploads
  └─ FUN_0017F730 (0x0017F730) -> FUN_00187C70  light coronas
FUN_001AE340 (0x001AE340) scene dispatcher
  └─ FUN_0019A580 (0x0019A580) @0x001AE4BE   ALL car shadows, one later pass
```

`FUN_000303D0`'s arguments, from the disassembly at `0x000303DC`–`0x000303E8`
and the call site `0x0018DF6F` **[C]**:

| reg / arg | meaning |
|---|---|
| `EAX` | LOD index |
| `[ebp+8]` | the **global draw context** `0x004D6C90` |
| `[ebp+0xC]` | the **model instance** (`carObj + 0x10`) |
| `[ebp+0x10]` | the damage/visual ctx (`*(vehicle+0xCC4)`, RE_NOTES §13) |

Model instance layout used here **[C]**:

| offset | meaning |
|---|---|
| `+0x00` | 4×4 frame matrix |
| `+0x40` | the relinked `.bgv` buffer |
| `+0x44/+0x48` | rest-state effect params (0, 1) |
| `+0x58` | **light byte** — `AND 0xFC` at `0x000303EE` becomes the record pass mask |
| `+0x59` | **paint / colour index** — indexes the shine table and the material dir |
| `+0x5B` | draw-enable |
| `+0x5C .. +0x7C` | **nine SH coefficients**, the source of the environment |

`FUN_00031E10` is `RET 0x14` (five stack args); arg3 = the record-mask filter
(`0x3FF` for the intact car), arg4 = the light byte, which becomes
`FUN_00031AB0`'s third parameter **[C, disasm 0x00031E11 / 0x00031E48]**.

### D3D wrappers identified

| VA | signature | identification |
|---|---|---|
| `0x0034F840` | `(ECX = hw const slot, EDX = float4*)` | `SetVertexShaderConstant` ×1 — emits NV097_SET_TRANSFORM_CONSTANT_LOAD (`0x1EA4`, via `&LAB_00041EA4`) then `0x100B80` = count 4, method `0x0B80` = NV097_SET_TRANSFORM_CONSTANT **[C by encoding]** |
| `0x0034F8F0` | same, 16 floats | `SetVertexShaderConstant` ×4 — same LOAD then `&DAT_00400B80` = count 16 **[C]** |
| `0x0034E9A0` | `(ECX = register, EAX = count, [ESP+4] = data)` | `SetPixelShaderConstant` — packs the float4 to D3DCOLOR and patches every combiner stage whose C0/C1 nibble (shader def `+0xE4/+0xE8/+0xEC`) selects that register, emitting NV097_SET_COMBINER_FACTOR0/1 (`0x0A60..0x0A9C`) **[C by encoding]** |
| `0x0034EDB0` | `(stream, vb hdr, stride)` | `SetStreamSource` (already [C] in `extract_bgv.py`) |
| `0x001DABD0` | `(handle)` | shader select — caches to `DAT_0041AABC`, calls `FUN_0034E790` |
| `0x001D7D10` | `(prim, count, idxptr)` cdecl | indexed draw; prim `6` = `D3DPT_TRIANGLESTRIP` |

Xbox `SetVertexShaderConstant` biases the D3D register by 96, so hardware
slot 96 = `c0` and 111 = `c15`.

---

## 2. Body shine

### 2.1 The environment is spherical harmonics [C]

`FUN_000313E0(EAX = out float[16], ECX = in float[9])` builds exactly the
irradiance matrix of Ramamoorthi & Hanrahan, *An Efficient Representation for
Irradiance Environment Maps* (SIGGRAPH 2001):

```
M = [ c1·L22   c1·L2-2  c1·L21   c2·L11          ]
    [ c1·L2-2 -c1·L22   c1·L2-1  c2·L1-1         ]
    [ c1·L21   c1·L2-1  c3·L20   c2·L10          ]
    [ c2·L11   c2·L1-1  c2·L10   c4·L00 - c5·L20 ]
```

with the paper's own constants, read out of the ELF:

| constant | VA | value |
|---|---|---|
| c1 | `0x003868F0` | 0.4290429949760437 |
| −c1 | `0x003B1AE0` | −0.4290429949760437 |
| c2 | `0x003868F4` | 0.5116639733314514 |
| c3 | `0x003868F8` | 0.7431250214576721 |
| c4 | `0x003868FC` | 0.886227011680603 |
| c5 | inline at `0x00031474` | 0.247708 |

**Verified by execution**: driving the real `FUN_000313E0` with L00 = 1 and
the other eight coefficients zero reproduces the analytic matrix to < 1e-6,
including `M[3][3] = c4·L00 = 0.886227012`
(`validate_carfx.py` section 4, last check).

`FUN_000315C0` then multiplies **all four rows** by a scalar and uploads them
to hardware constant slots `0x60..0x63`, i.e. vertex-shader `c0..c3`
**[C, `MOV ECX,0x60` @0x0003164C]**. The nine input coefficients come from
`ECX + 0x5C` where `ECX` is the model instance
(`ADD ECX,0x5C` @`0x000315CC`, `MOV ECX,EDI` @`0x00030531`) **[C]**.

**Who writes those nine coefficients: recovered — see §2.7.**

### 2.2 The SH row scale and the fade [C]

`FUN_000303D0` computes two scalars before calling `FUN_000315C0`
(disasm `0x0003049E`–`0x00030533`):

```
shScale = 1.0                              -> stack arg, multiplies M's rows
psFade  = 1.0                              -> XMM0 -> DAT_004A1CF4 -> ps c3.w
if (dmgCtx+0x101B != 0)  psFade = 1.0 - dmgCtx[0x370] * 0.6      (0x003B16EC)
if (dmgCtx+0x1015 & 0x10) {                                       despawn fade
    t = now(0x0060EA20) - dmgCtx[0x100C]
    if (t > 0.4)  { shScale = 0.25;              psFade *= 0.1 }  (0x003B16E8 /
    else          { shScale = 1 - t*1.875;                          0x003B1730 /
                    psFade *= 1 - t*2.25 }                          0x003A69C4 /
}                                                                  0x003B1F30 /
                                                                   0x003B1F34
```

The two branches agree at t = 0.4 (1 − 0.4·1.875 = 0.25, 1 − 0.4·2.25 = 0.1),
which is what confirms the reading.

### 2.3 Per-record shader branch — FUN_00031AB0 [C]

```
tex   = drawCtx[0x334 + rec[0x1A]*4]            texture slot -> DAT_0075DB70
mask  = *(u16*)(rec + 0x18)
if      (mask & 0x0002)              -> shader DAT_004D6558, RS 0x3B = rec[0x14]
else if (mask & lightByte & 0xFC)    -> shader DAT_004D655C, RS 0x3B = 0
                                        (the LIGHT/emissive branch: no constants)
else if (mask & 0x0300)              -> GLASS: two draws
else                                 -> BODY: DAT_004D6550 (rec[0x14]==0)
                                             DAT_004D6554 (rec[0x14]!=0)
```

The light test is an **if/else**, not a skip: a light-element record is always
drawn, emissively when its bit is lit and with the plain body shader when it
is not.

Constants uploaded, **captured live** (`validate_carfx.py` §4):

| register | body | glass pass 2 | alt material |
|---|---|---|---|
| ps `c14` | `(lightRGB, 0.1)` | `(lightRGB, 0.8)` | `(lightRGB, 0.5)` |
| ps `c15` | `(1, 1, 0.650000006, 0.269230762)` | `(1, 1, 0.775000006, 0.298387098)` | as body |
| ps `c13` | — | `(0,0,0, tint)` on pass 1 | — |
| ps `c3` | `(0,0,0, psFade)` | same | same |
| vs `c15` (hw 111) | `(0.1835709810256958, 0.8164290189743042, 1, 1)` | `(0.039943765848875046, 0.9600562453269958, 1, 1)` | as body |
| vs `c0..c3` (hw 96..99) | SH matrix × shScale | same | same |

`lightRGB` = `DAT_0060E0A0..A8`, a runtime RGB global (BSS; also read by
`FUN_0017EE00`, which normalises it as a colour). It lives at
`environment object (0x0060E040) + 0x60` — **recovered, see §2.8**.

### 2.4 The shine table 0x0045BB20 [C]

`FUN_00030150(EAX = draw ctx)` writes it. **Executed** under Unicorn; the
resulting 0x70 bytes are:

| field | offset | value | becomes |
|---|---|---|---|
| P[0..7] | `+0x08 + i*4` | 0.1 (`0x3DCCCCCD`) | ps `c14.w` |
| K[0..7] | `+0x28 + i*4` | 0.35 (`0x3EB33333`) | ps `c15.z` = 1 − K |
| M[0..7] | `+0x48 + i*4` | 0.7 (`0x3F333333`) | ps `c15.w` = 0.25·M/(1−K) |
| glass K | `+0x68` | 0.225 (`0x3E666666`) | ps `c15.z` (glass) |
| glass M | `+0x6C` | 0.925 (`0x3F6CCCCD`) | ps `c15.w` (glass) |

The eight entries are one per paint/colour variant (`modelInstance+0x59`),
and the retail defaults ship all eight identical. `FUN_00030150` is called
from `FUN_0002F260` (`MOV EAX,0x4D6C90` @`0x0002F2CF`), the vehicle-renderer
init that also loads the glass textures `"UnbrokenGlass"`, `"CrackedGlass"`,
`"SmashedGlass"` and `"VehicleUnderside"` — the slots `FUN_000300A0` retargets
(RE_NOTES §13).

The `0.25` at `0x003B1730` is the reciprocal of the NV2A register combiner's
4× output scale, so `M/(1−K)` is the effective specular gain in hardware
(1.0769 body, 1.1935 glass) **[S]**.

### 2.5 The Fresnel pair [C values, S interpretation]

`FUN_0002EF90` writes two float4s into the global draw context
(`0x0002F01E`–`0x0002F07C`):

| ctx offset | absolute | value |
|---|---|---|
| `+0x350` (glass) | `0x004D6FE0` | `(0.039943765848875046, 0.9600562453269958, 1, 1)` |
| `+0x360` (body) | `0x004D6FF0` | `(0.1835709810256958, 0.8164290189743042, 1, 1)` |

Both are `(x, 1−x, 1, 1)` to the last bit. Solving Schlick's
`R0 = ((n−1)/(n+1))²` gives **n = 1.4996 for glass** and **n = 2.4993 for the
body** — window glass and a car clearcoat. That, plus the absence of any
second texture stage, is the whole basis for reading the shine as
Fresnel-weighted SH reflection.

### 2.6 What is NOT recovered [?]

The eight car shader handles `DAT_004D6540..0x004D655C` are **runtime-created
handles in BSS with zero WRITE xrefs**; no `vs.1.1`/`ps.1.1` token stream
(`0xFFFE0101` / `0xFFFF0101`) exists anywhere in the image. **The NV2A
register-combiner / vertex program the constants feed could not be
recovered.** Everything above is *what is uploaded*, not *what the shader
does with it*.

### 2.7 Where the nine SH coefficients come from [C]

> **SUPERSEDED IN PART — see §12.** The three sites below are real and are
> still `[C]`, but the conclusion drawn from them ("not computed, not per
> track") is **wrong**. A fourth writer, `FUN_0019D400`, rewrites the same
> nine floats **every frame, per car**, from a per-position light-probe
> volume in the streamed track data; it was missed here because it copies
> them with `REP MOVSD` rather than the `MOVSS` literal-store pattern this
> section scanned for. The three sites below are the **initial default**,
> which is also what a car keeps whenever the probe lookup misses. §12 has
> the recovered chain.

They are **not** computed and **not** per track. Nine literal floats in
`.rdata` are stored into the model instance immediately before the draw, by
**three separate sites that write exactly the same nine** — which is what
makes the reading safe:

| site | destination | which car |
|---|---|---|
| `0x000C047C .. 0x000C04F5` | `[ebx+0x6C .. +0x8C]` | in-race car object |
| `0x001A74C5 .. 0x001A755A` | `[ecx+0xCC .. +0xEC]` | the `FUN_001A4710` instance (`obj+0x70`) |
| `0x001AE92D .. 0x001AE9AD` | `[esi+0x6C .. +0x8C]` | front-end showcase |

(the model instance is at `carObj+0x10`, so `+0x6C` **is** `instance+0x5C`;
`FUN_001A4710` passes `obj+0x70`, so `+0xCC` is too.)

`validate_carfx.py` §9 decodes those three runs straight out of the
instruction stream (`F3 0F 10 05 imm32` / `F3 0F 11 modrm disp`) and asserts
they agree and that the module's table equals them.

| i | VA | value | R-H basis | matrix slot (mapped by executing `FUN_000313E0` on each `e_i`) |
|---|---|---|---|---|
| 0 | `0x003B1900` | 0.3125 | L00 | M33 = c4·L00 − c5·L20 |
| 1 | `0x003B193C` | 0.247825757 | L1−1 (y) | M13 / M31 |
| 2 | `0x003B1938` | −0.0931382477 | L10 (z) | M23 / M32 |
| 3 | `0x00384A80` | 0.150000006 | L11 (x) | M03 / M30 |
| 4 | `0x003B1934` | 0.146874994 | L2−2 (xy) | M01 / M10 |
| 5 | `0x003B1930` | −0.0851906464 | L2−1 (yz) | M12 / M21 |
| 6 | `0x003B192C` | −0.0785328001 | L20 | M22, and −c5 into M33 |
| 7 | `0x003B1928` | −0.118749999 | L21 (xz) | M02 / M20 |
| 8 | `0x003B1924` | 0.065624997 | L22 | M00 / −M11 |

`E(n) = nᵀMn` on the resulting matrix is a **top-lit studio probe**:

```
E(+Y) 0.52185   E(−Y) 0.01464     (bright above, black below)
E(+X) 0.47805   E(−X) 0.17106     (a strong left/right key)
E(+Z) 0.14273   E(−Z) 0.33335
```

The maximum over the whole sphere is higher than any single axis —
**0.68824 at n = (0.650, 0.672, −0.354)** — which is why the specular
threshold K = 0.35 opens at all. The peak body highlight is therefore
`(0.68824 − 0.35)/0.65 × 0.7 = 0.364` of the sun colour, and the peak glass
highlight `0.553`. That is the recovered source of the gloss, not a tuned
gain.

The **front-end showcase** (`FUN_001AE6F0`) draws the car twice: the second
draw is the upright car with the set above; the first is its floor
reflection, which negates `m11` and `ty` (`0x001AE909`–`0x001AE91C`) and uses
the same nine with **i = 1, 5 and 7 negated** (`0x003B204C` / `0x003B2048` /
`0x003B2044`). An exact y-mirror of the R-H form negates i = 1, 4 and 5, so
that second set is hand-authored rather than derived — **[C] for the values,
[?] for the interpretation**. Only the upright set is used in a race.

That the showcase had to change the coefficients at all is the evidence that
the SH is consumed in a space the **model matrix affects** — i.e. world (or
view) space, not object space: mirroring the model matrix does not change an
object-space normal, so an object-space probe would have needed no new
coefficients **[S]**.

### 2.8 Where the light RGB comes from — `enviro.dat` [C]

`DAT_0060E0A0` has no writer of its own because it is a **field, not a
variable**. The environment object lives at `0x0060E040` and `0x0060E0A0` is
its `+0x60`; an exhaustive scan of every executable byte finds the address
`0x0060E0A0` in **9 places, all reads**, and no instruction anywhere writes
it absolutely.

```
FUN_001888F0 (0x001888F0)                       the environment loader
  builds "<track dir>/enviro.dat"               literal at 0x003B0444
  FUN_00011240(...)                             loads the file whole
  FUN_00188880 (0x00188880)                     relocates +0x98..+0xA4
                                                (the four sky textures
                                                 RE_POSTFX.md documents)
  FUN_00188C00  @0x00188A40  (EAX = 0x0060E040, ECX = the loaded buffer)
                                                copies 0xB0 bytes over the
                                                environment object
```

so **`ps c14.xyz` is literally `enviro.dat` bytes 0x60..0x6B**. The same
record also carries `+0x00` (a warm ambient/fog colour), `+0x10` (far plane,
fog density), `+0x70` (a second, lighter colour) and `+0x80` (a unit light
direction — written but never read by any mapped code, `[?]`).

Every shipped file authors it as an exact 8-bit colour. Silver Lake, the
track in the reference captures:

```
US/C3_V1   enviro.dat +0x60 = 0.992157, 0.894118, 0.674510   = 253,228,172
```

`tools/extract_carfx_art.py` dumps all 37 tracks to
`build/carfx/env_light.txt`, `src/burnout3_carfx.c` carries them as
`B3FX_ENV_LIGHT[]`, and `validate_carfx.py` §10 re-reads every shipped file
to prove the table. `b3_carfx_set_track("US_C3_V1")` installs both inputs.

### 2.9 The GL translation — GLUE, and marked as such

`src/burnout3_carfx.c` implements:

```
irr  = max(0, nᵀ M n)                                  SH irradiance   [C]
env  = max(0, rᵀ M r),  r = reflect(-V, N)             the same basis sampled
                                                       along the mirror ray
F    = R0 + (1-R0)·(1 - max(0, N·V))^5                 Schlick from vs c15.xy
spec = clamp((env - K)/(1-K), 0, 1) · M                from ps c15.zw
base = albedo·irr·(1-F) + lightRGB·env·F
out  = base                     + lightRGB·spec        opaque body record
out  = lerp(base, lightRGB·spec, P)                    a BLENDED record
                                                       (glass pass 2 P=0.8,
                                                        alt material P=0.5)
```

which is the NV2A **final combiner**'s fixed equation `A·B + (1−A)·C + D`
with `A = F`, `B = lightRGB·env`, `C = albedo·irr` and `D = lightRGB·spec` —
`D` being the input the hardware has in order to receive a specular sum.

**Correction, and the fix for "no glossiness".** The first cut multiplied the
specular by `P` (ps `c14.w` = 0.1 on the body), which put the highlight at
≈0.003 — invisible. `P` is **not** an RGB scale. `c14` is a combiner
*factor*: its `.xyz` feeds the RGB combiner (the light colour) and its `.w`
feeds the **separate alpha combiner**. Its three values track the pass's
blend state exactly, not any gloss strength **[S]**:

| branch | ALPHABLENDENABLE (RS `0x3B`) | P |
|---|---|---|
| plain body record | 0 — `0x00031BF1` with `rec+0x14 == 0` | 0.1 (unused) |
| alt-material record | `rec+0x14` — `0x00031BF1`, non-zero | 0.5 |
| glass pass 2 | 1 — `0x00031CD0` | 0.8 |

So `P` is the pass **alpha**, and with it out of the RGB path the recovered
constants alone put the peak highlight at `(E−K)/(1−K)·M = 0.364` of the sun
colour. Two more consequences of the same reading, both `[C]`-anchored:

* glass **pass 1** uploads `ps c13 = (0,0,0, tint)` — its colour is literally
  black — so the glass draw contributes only a tint plus the pass-2
  reflection (`base` is multiplied by 0 on that path);
* the glass reflection is a **textured** draw (`FUN_000300A0` retargets stage
  0 to `UnbrokenGlass` / `CrackedGlass` / `SmashedGlass`), so its reflection
  is modulated by the bound raster; the body's is not.

Every scalar (R0, K, M, P, lightRGB, and now the nine SH coefficients) is a
recovered constant; the **algebra that combines them is GLUE**, because the
combiner program is `[?]`. Nothing is invented — if you delete the
`spec`/`F` term you are left with the pure, fully-recovered SH irradiance
layer, which is what the module does when `uR0` is set to 0.

One further GLUE line: the reflection is gated on `gl_FrontFacing`, because
the game draws the body with back-face culling and the harness does not —
without it the probe's bright lobe lights the *inside* of the roof through
the glass.

**Sphere-map / cube-map equivalence, documented as requested:** there is
none to draw, and that is the finding. `FUN_000303D0` makes exactly two
`SetTexture` calls (`0x0003055B` and `0x00030568`), both to **stage 0**, both
with the paint raster from the material directory (`file+0x60`, entry
`+0x14 + paintIdx*4`, count `+0x69`). No cube map, no sphere map, no second
UV set used for reflection (stream 1 is a stride-8 attribute stream, and
`extract_bgv.py` already establishes that position/normal/uv all live in
stream 0). The GL equivalent of the Xbox environment term is therefore
evaluating the same `nᵀMn` quadratic form along the reflection vector — which
is what a prefiltered irradiance cube map would return anyway for an L2
environment.

---

## 3. Car shadow

`FUN_0019A580` (`0x0019A580`) runs **after** all car meshes — the scene
dispatcher `FUN_001AE340` calls the car draw `FUN_001A4710` first and reaches
`FUN_0019A580` at return address `0x001AE4BE` **[C]**. It walks three object
lists (vehicles `DAT_0073A1A8`, props, a third entity list) and calls
`FUN_0019A7C0` per object.

### 3.1 Render state — FUN_00043350 [C]

`FUN_0002DDF0("blobbyshadow")` (string at VA `0x003AAFF8`, its only xref is
`0x00043371`) binds the texture. Shadowed render states written into
`0x0075D4A0 + id*4`:

| id | value | Xbox D3DRS [S] |
|---|---|---|
| 59 (`0x3B`) | 1 | ALPHABLENDENABLE |
| 60 | 1 | ALPHATESTENABLE |
| 62 | `0x302` | SRCBLEND = D3DBLEND_SRCALPHA |
| 63 | `0x303` | DESTBLEND = D3DBLEND_INVSRCALPHA |
| 64 | 0 | ZWRITEENABLE = off |

Texture-stage addressing is set to 2/2 (Xbox MIRROR); the module uses
`GL_CLAMP_TO_EDGE`, which is equivalent for a single non-tiling blob and
avoids bleeding the soft border — **GLUE, noted in the source**.

### 3.2 Geometry — FUN_00043570 [C]

Eight vertices, stride `0x14` (`float3 pos + float2 uv`), static buffer
`0x0054F058`, drawn with `FUN_001D7D50(6, 8, 0x0054F058, 0x14)` — prim type
6 = `D3DPT_TRIANGLESTRIP`. All eight share one Y (the caller's arg). Four
rows of two:

| row | X | Z | V |
|---|---|---|---|
| 0 | ±halfWidth·s | **bbox front** (`model+0xE88`) | 0.0 |
| 1 | ±halfWidth·s | **front axle** (`model+0xBF8`) | 0.1875 (`0x003B1AC0`) |
| 2 | ±halfWidth·s | **rear axle** (`model+0xB38+nw*0x40`) | 0.8125 (`0x003B1ABC`) |
| 3 | ±halfWidth·s | **bbox rear** (`model+0xE98`) | 1.0 (`0x003B168C`) |

so the blob's two soft V-cap bands land on the nose/tail overhangs and its
middle band stretches across the wheelbase.

**Correction worth recording.** Reading `FUN_0019A7C0` naively makes rows 0/3
look identical to rows 1/2, because `[esp+0x24]`/`[esp+0x34]` have their `.z`
overwritten with the axle Z's at `0x0019A917` / `0x0019A904`. But `xmm2` and
`xmm1` were loaded from those slots **earlier** (`0x0019A8EE` / `0x0019A8E3`)
and still hold the *bounding-box* Z's; those registers are what become the
`ECX` pair passed to `FUN_00043570`, while the overwritten memory becomes the
`EAX` pair. That is what makes the strip a real four-row 3-slice. Both the
degenerate and the correct reading were rendered in the harness; only the
correct one produces the soft blob the reference shows.

`halfWidth` = `model+0xE80.x`; `model+0xB38 + numWheels*0x40` is the
wheel-matrix array's Pos-row Z (`0xB80 − 0x48`), i.e. a rear wheel's Z.
`numWheels` = `.bgv` header byte `+0x0D`.

### 3.3 Placement + fade [C]

```
frame       = FUN_0011A720(groundPlane(veh+0x520), veh+0x30)   ground-aligned basis
translation = veh+0x40  with  .y -= *(veh+0x52C)               dropped to ground
vertexY     = 0.06                        PUSH 0x3D75C28F @0x0019A6A0
h           = *(veh+0x52C) - *(model+0x18)     model+0x18 = the .bgv wheel radius
airFade     = 1 - min(h * 0.8, 1)              0x003A5600 ; skip if <= 0
scale       = airFade * 0.4 + 1.0              0x003B16E8
alpha       = min(1, ramp * 2 * 3.3333333) * 0.7 * airFade
              (0x003B1688 · 0x003B1DF8 ; master darkness 0.7 @0x003B17D8,
               MULSS at 0x0004359C)
colour      = (1, 1, 1, alpha)  -> SetPixelShaderConstant register 0
```

`ramp` is `vehicle+0x70`; **its semantics are [?]**, so the module takes it as
a parameter and callers pass 1.0 (fully faded in), leaving
`alpha = 0.7·airFade` — all recovered.

Per-object gating: `vehicle+0x18F8` is the shadow-enable byte.

---

## 4. Light coronas

### 4.1 The light byte [S, addresses C]

`FUN_0011BE50` (the per-frame vehicle update) clears `modelInstance+0x58` at
`0x0011BF55` and then ORs:

| bit | meaning | site |
|---|---|---|
| `0x10` | brake | `OR byte ptr [eax+0x58], 0x10` @`0x0011BFC3` |
| `0x80` | reverse | `OR byte ptr [eax+0x58], 0x80` @`0x0011BFD6` |
| rest | ORed wholesale from `carObj+0x18FF` | `0x0011BFE6`/`0x0011BFEC` |

**The writer of `carObj+0x18FF` does not exist in the mapped code** — an
exhaustive function-aligned scan plus a raw displacement scan over every
executable segment finds only the read above and two clears (`0x0018D671`,
`0x0018D98D`). So the origin of the headlight / tail / indicator bits (night?
track lighting? AI?) is **[?]**, and the module takes them from the caller.

### 4.2 Position source: a real table in the .bgv [C]

`FUN_001879E0` / `FUN_00187AC0` read

```
records = *(u32*)(model + 0x1664 + type*4)
count   = *(u8*) (model + 0x16AC + type)
```

with 0x30-byte records:

```
+0x00  float4  position, model space, w = 1
+0x10  float4  outward normal,        w = 1
+0x20  float4  (0, a, b, 0)                                [? meaning]
```

Validated on **all 67** player `.bgv` files: `offset != 0 ⟺ count != 0`, every
block in range, every normal unit length, every position car-scale, `w = 1`
throughout (`validate_carfx.py` §7 — 1515 records). On `COMP/Car1` the
semantics are unambiguous:

```
type 0 headlight   (±0.602, 0.305,  +1.772)  n = (0,0,+1)
type 1 tail        (±0.622, 0.452,  -1.856)  n = (0,0,-1)
type 2 brake       (±0.729, 0.454,  -1.774)  n = (0,0,-1)
type 4 reverse     (±0.623, 0.564,  -1.838)  n = (0,0,-1)
type 5 indicator R (+0.764, 0.332,  +1.743) + (+0.724, 0.563, -1.751)
type 6 indicator L (-0.764, 0.332,  +1.743) + (-0.725, 0.563, -1.751)
types 8..11        exhaust / roof / misc                   [?]  (FUN_001871E0)
```

### 4.3 Colour, size, falloff [C constants]

| bit | type | colour VA | colour | size × |
|---|---|---|---|---|
| `0x02` | 0 | `0x004161A0` | (1.5, 1.5, 1.5) | 1.25 (`0x003895BC`) |
| `0x04` | 0 | `0x004161B0` | (1.0, 1.0, 1.0) | 1.0 |
| `0x10` | 2 | `0x004161C0` | (1.4, 0, 0) | 0.75 (`0x003A55F8`) |
| `0x08` | 1 | `0x004161D0` | (1.1, 0, 0) | 0.5 |
| `0x20` | 5 | `0x004161E0` | (1.0, 0.9, 0) | 0.5 |
| `0x40` | 6 | `0x004161E0` | (1.0, 0.9, 0) | 0.5 |
| `0x80` | 4 | `0x004161F0` | (0.9, 0.9, 0.9) | 0.5 |

`FUN_00187C70` tests `0x10` before `0x08`, so a braking car draws the brake
corona **instead of** the tail corona **[C, 0x00187D2B/0x00187D59]**.

```
size = (dist * 0.02 + 1.0) * typeMult              0x003B1A08
d    = dot(eyeVector, lightWorldNormal);  reject if d <= 0      (no 1/r²)
rgba = colourConst * d
pool record: {pos, 400.0 far cut, size*0.5, 0.5 pull, rgb*64.0}
quad corner = pos ± right·size ± up·size - 0.5·(pos - camPos)
```

i.e. the sprite is emitted at the **midpoint between the light and the eye**,
which is the depth bias that keeps it in front of the bodywork.
`FUN_0034F6D0(0x142)` sets FVF `D3DFVF_XYZ|DIFFUSE|TEX1` = 24 bytes, matching
the `0x18` stride of the `FUN_001D7D50(8, 4, …)` **`D3DPT_QUADLIST`** draw.

Texture: the name table at VA `0x003A3E7C` is
`{"coronaglow", "coronaboost", "coronaboostred"}`; `FUN_00187BE0` hard-codes
pool 0 (`ADD ECX,0xC` @`0x00187C28`), so **car lights always use
`coronaglow`** **[C]**.

**The literal Xbox blend factors for the corona pass are [?]** — the
`id → D3DRENDERSTATETYPE` map was not decoded. Additive is the module's GLUE
choice, justified by the ×64.0 overbright (`DAT_0035BF1C`), the >1.0 colour
constants, and `coronaglow` shipping fully opaque with the glow in RGB.

---

## 5. Art

`tools/extract_carfx_art.py` writes:

* `build/carfx/blobbyshadow.png` — 128×128, blob in the alpha channel
* `build/carfx/coronaglow.png` — 128×128, radial glow in RGB, alpha 255
* `build/cars/<CLASS>_<CarN>.lights` — 67 files, 1515 corona records plus the
  five shadow-quad scalars
* `build/carfx/env_light.txt` — the per-track sun colour (`ps c14.xyz`),
  byte-read from all 37 shipped `enviro.dat` files

Both textures are selected **by name from the executable**, not by eye, and
decoded with the repo's existing (unmodified) container/format readers.

---

## 6. Open items

| item | status |
|---|---|
| Writer of the nine SH coefficients at `modelInstance+0x5C` | **[C] CLOSED, and the earlier answer was incomplete** — the three `.rdata` literal sites (§2.7) are only the DEFAULT; `FUN_0019D400` rewrites all nine per frame per car from a light-probe volume, §12 |
| The probe volume's byte layout inside `streamed.dat` | **[?]** — the consumer is `[C]`, the container is not; §12.4 |
| Writer of `DAT_0060E0A0` (ps `c14.xyz` light RGB) | **[C] CLOSED** — `enviro.dat` +0x60, §2.8 |
| Which space the SH normal is in (world vs object) | **[S]** — world, from the showcase-mirror argument (§2.7); not directly provable |
| The showcase reflection's i = 1,5,7 sign flips | **[?]** — values `[C]`, but they are not an exact y-mirror |
| `enviro.dat` `+0x80` unit light direction | **[?]** — written by the loader, read by no mapped code |
| The NV2A vertex/pixel programs `DAT_004D6540..655C` | **[?]** — BSS handles, no token stream in the image; the GL composition is GLUE |
| Writer of `carObj+0x18FF` (head/tail/indicator bits) | **[?]** — exhaustively scanned; only the read and two clears exist |
| Semantics of `vehicle+0x70` (shadow opacity ramp) | **[?]** — parameter, callers pass 1.0 |
| Meaning of the light record's third vec4 (`+0x20`) and light types 8..11 | **[?]** — `FUN_001871E0`'s domain (boost flame / exhaust) |
| Literal D3D blend factors of the corona pass | **[?]** — raw shadowed ids reported instead |
| `coronastar` (in `Global.txd`) | unreferenced by the ELF **[C]** |

---

## 11. The marbled shine — the `.bgv` vertex normal is `NORMPACKED3` [C]

The car body was covered in chaotic bright/dark streaking once the harness
started feeding the `.bgv` `vn` to the shine (the specular is evaluated along
`reflect(-V, N)`, so a wrong normal is not a soft shading error — it is noise).
**The cause was the extractor, not the shine.** `tools/extract_bgv.py` read the
four bytes at vertex `+0x0C` as `s8[4] / 127`. They are Xbox
**`D3DVSDT_NORMPACKED3`**: `x` = bits 0..10 signed / 1023, `y` = bits 11..21
signed / 1023, `z` = bits 22..31 signed / 511.

### 11.1 The static proof — the car's own vertex declaration

`FUN_0003C8A0` (`0x0003C8A0`) is the **car** shader factory. That it is the
car's and not the world's is `[C]`: it calls `FUN_0002EF90` at `0x0003CB60` —
the function §2.5 identifies as the writer of the car body/glass Fresnel pair.
Immediately after, it creates the vertex shader:

```
0003cb60  CALL 0x0002ef90          ; the car Fresnel pair
0003cb6e  PUSH 0x3e7d58            ; vertex program
0003cb73  PUSH 0x387558            ; vertex DECLARATION
0003cb78  CALL 0x0034f440          ; CreateVertexShader
```

and `0x00387558` is

```
20000000 40320000 40160002 40220009 FFFFFFFF
stream0  v0=FLOAT3 v2=NORMPACKED3 v9=FLOAT2  END
```

= 12 + 4 + 8 = **stride 0x18**, which is exactly the stream-0 stride
`FUN_000315C0` binds (`PUSH 0x18` @`0x0003166A`). So position is at `+0x00`,
the packed normal at `+0x0C` and the UV at `+0x10`.
`tools/extract_track.py` already decoded the identical type for the world
vertex from declaration `0x0038758C`; only the car path had been left on the
`s8` reading.

### 11.2 The empirical proof

Decoding both ways on `COMP/Car1`'s 7170-vertex pool and scoring against the
area-averaged geometric normal of the body triangles:

| reading | mean dot | median | > 0.7 | negative | \|n\| |
|---|---|---|---|---|---|
| `s8[4]/127` | **−0.036** | −0.073 | 13.7% | 54.7% | 120 (!) |
| `NORMPACKED3` | **+0.9916** | 1.0000 | 98.9% | 0% | 0.998 – 1.000 |

The old reading was noise. `validate_carfx.py` §11 re-runs both decodes, the
declaration read and the OBJ round-trip on every invocation.

### 11.3 The other two contributors, both in the GL translation

* **The fragment normal was flipped toward the viewer.** `B3FX_FS` ran
  `if (dot(N,V) < 0.0) N = -N;` on *every* fragment. With real vertex normals
  that snaps the normal to its opposite wherever `N·V` crosses zero — a hard
  seam across the body, and the reflection lobe jumps with it. The flip is
  only legitimate on the face-normal fallback (`cross(dFdx, dFdy)`, whose sign
  genuinely is arbitrary), and it is now confined there.
* **The interpolated normal must be renormalised per fragment**, which it now
  is; feeding the shortened interpolant to a quadratic form bulges and bands
  the highlight between the corners.

### 11.4 The world mirror — GLUE, but forced

§2.7 establishes `[S]` that the probe is consumed in **world** space. This
harness's GL world is the **Z-mirror** of the game world: `trackmesh_load()`
negates the Z of every position *and* every normal, `burnout3_full.c` mirrors
the path/collision arrays to match and negates the car yaw, then flips X in
the **projection** so the final image reads the right way round.

A normal the game would evaluate as `Ng` therefore arrives as
`Ngl = (Ng.x, Ng.y, −Ng.z)`, and a quadratic form cannot absorb that. So the
module mirrors the **probe** instead: in the R-H basis the coefficients whose
basis function is odd in z are `i = 2` (`L10`), `5` (`L2-1`) and `7` (`L21`),
and negating exactly those three gives `E'(x,y,z) ≡ E(x,y,−z)` — verified to
**0.0e+00** over 4000 random directions (`validate_carfx.py` §12). It leaves
`E(±X)` and `E(±Y)` untouched and swaps `E(+Z) = 0.14273` with
`E(−Z) = 0.33335`, i.e. it moves retail's 2.3× front/back asymmetry onto the
right end of the car. This is a mirror of the **port's** world, not of the
game's — the game does not do it — so the transform is **GLUE** even though
every number it moves is `[C]`. `B3_CARFX_NOZMIRROR=1` restores the old
behaviour; it is the A/B that justified the change (mean absolute difference
6–8 levels over the car, peak 94–101).

The consistency of the mirror with the yaw negation is exact:
`mirror_Z ∘ R_y(θ) ∘ mirror_Z = R_y(−θ)`, and `burnout3_full.c` already draws
the car with `glRotatef(−yaw)` for that reason.

### 11.5 The loader

`trackmesh_load()` stores positions, UVs, colours and normals as four
**parallel** arrays indexed by a single number, which is only correct while a
face's `vt`/`vn` indices equal its `v` index. Both extractors do emit
`f a/a/a b/b/b c/c/c`, so the flat model was exact — but by an invariant of
the *writers*, not of the format. The face parser now reads all three indices,
and any OBJ that indexes the channels separately triggers a de-duplicating
expansion into one vertex per distinct `(v, vt, vn)` triple. On the shipped
data the expansion never runs and the loaded mesh is bit-identical;
`validate_carfx.py` §11 asserts the 1:1 form on both a car OBJ and the track
OBJ so the no-op guarantee is checked rather than assumed.

**Nothing but the normal channel changed.** Regenerating all 352 car OBJs left
the `v`, `vt` and `f`/`o` lines byte-identical (0 files differing) and all 540
paint/decal rasters byte-identical; only `vn` moved. The sponsor decals
(`RUGERIERO`, `MASTROIANNI`, `xenier`, `LUALDI Racing`, …) are the same
rasters, on the same UVs — they had simply been buried under the marbling.
The traffic extractor `tools/extract_traffic.py` shares `read_verts`, so its
11 `.btv` meshes were regenerated too.

---

## 12. Environmental adaptation — the car SH is a per-position light probe [C consumer, ? container]

Retail cars visibly darken under bridges and in tunnels. Measured on the nine
`REFERENCE IMAGES/` frames (same car, same paint, "red paint" mask over a
fixed chase-cam box): mean red **113.1** on the open sunlit road
(`xemu-2026-08-12-13-52-39`) against **57.8** in the tunnel
(`xemu-2026-08-12-13-54-21`), p90 166 → 78. A **~2×** swing on the paint, and
it is a darkening, not a fog tint.

### 12.1 Why it has to be the SH — proof by elimination [S]

The car's colour is the NV2A final combiner `A·B + (1−A)·C + D` with
`C = albedo·irr` (§2.9). `ps c14.xyz` multiplies only `B` and `D`, so changing
the sun colour **cannot** dim the paint. The only inputs that can are
`FUN_000315C0`'s row scale and the nine coefficients themselves. The row scale
is `1.0` except the damage/despawn branches (§2.2, re-verified). So the nine
coefficients must vary — which contradicts §2.7, and they do.

### 12.2 The hidden writer — `FUN_0019D400` [C]

§2.7 scanned for the literal-store pattern `F3 0F 10 05 imm32` /
`F3 0F 11 modrm disp`. `FUN_0019D400` does not match it, because it copies:

```
0019d6a9  MOV EDI,dword ptr [EBP + 0xc]      ; arg2 = destination
...
0019d6f0  MOV ECX,0x9
0019d6f5  LEA ESI,[ESP + 0x14]
0019d6ff  MOVSD.REP ES:EDI,ESI               ; NINE dwords
0019d706  RET 0xc
```

and the block immediately above it is a **barycentric blend** of three probes,
`p0 + (p1−p0)·w0 + (p2−p0)·w1`, done component by component
(`SUBSS/SUBSS/MULSS/MULSS/ADDSS/ADDSS` ×9).

### 12.3 The per-frame chain [C]

```
FUN_001AE340 @0x001AE49A -> FUN_001AAF00                   (car pre-draw)
  @0x001AAFAE  CALL FUN_001AD4A0(streamCtx, carObj+0x40)   which streamed unit?
  @0x001AAFB3  MOV byte ptr [carObj+0x6A], AL              cache the unit index
  @0x001AB123  MOV DL, byte ptr [EAX+0x6A]
  @0x001AB128  JL  0x001AB14E                              index < 0 -> SKIP
  @0x001AB133  LEA EBX,[EAX+0x6C]                          = modelInstance+0x5C
  @0x001AB136  PUSH 0x41A00000                             = 20.0f
  @0x001AB13B  ADD EAX,0x40                                = car world position
  @0x001AB143  ADD ECX,0x7397C8                            stream ctx (0x1CC/viewport)
  @0x001AB149  CALL FUN_0019D400
```

Three things fall out of this and all three matter:

* the destination `carObj+0x6C` **is** `modelInstance+0x5C` (the instance is at
  `carObj+0x10`), i.e. exactly the nine floats `FUN_000315C0` reads;
* the sample point is the **car's own world position** (`carObj+0x40`) with a
  **20.0** downward cast — nothing else; no camera, no PVS cell;
* on a miss (`unit index < 0`, `JL` at `0x001AB128`) the call is **skipped**,
  so the previous frame's value persists. The `.rdata` literals of §2.7 are
  the value a car starts with and keeps until a probe is found.

Traffic cars go through the same system from `FUN_001A4710` @`0x001A4EF5`
(index byte `obj+0xCA`, destination `obj+0xCC`) **[S]**.

### 12.4 The probe record — decode [C], container [?]

`FUN_0019C640(ECX = out float[9], EDX = in s8[9])` is fully recovered:

```
out[0]    = s8[0] * 1.6 * 0.0078125   = s8[0] * 0.0125      (L00)
out[1..3] = s8[i] * 0.6 * 0.0078125   = s8[i] * 0.0046875   (L1)
out[4..8] = s8[i] * 0.4 * 0.0078125   = s8[i] * 0.003125    (L2)
```

with `1.6` @`0x003B16F0`, `0.6` @`0x003B16EC`, `0.4` @`0x003B16E8` and
`0.0078125` (= 1/128) @`0x003B16F4`. So **a probe is nine signed bytes**, and
the record stride is 9 (`LEA EDX,[ESI+EAX*8]; ADD EDX,EAX` @`0x0019D4DA`).

The quantisation grid is the strongest corroboration that §2.7's literals are
themselves a probe: `0.3125 = 25 × 0.0125` **exactly**, and so are
`L[3] = 0.15 = 32 × 0.0046875`, `L[4] = 0.146875 = 47 × 0.003125`,
`L[7] = −0.11875 = −38 × 0.003125` and `L[8] = 0.065625 = 21 × 0.003125` —
five of the nine land exactly on the probe grid. (The other four do not, so
the shipped default is **not** simply a decoded probe; `[?]`.)

The unit object carries two relocated pointers, `[unit+0xA0]` (a BSP the
vertical cast walks) and `[unit+0xA4]` (the probe array), fixed up at load by
adding the object base:

```
0019d7a8  MOV EAX,dword ptr [ESI + 0xa4]
0019d7b9  ADD EAX,ESI
0019d7bb  MOV dword ptr [ESI + 0xa4],EAX
0019d7c1  MOV EAX,dword ptr [ESI + 0xa0]
0019d7cb  LEA ECX,[EAX + ESI*0x1]
0019d7ce  MOV dword ptr [ESI + 0xa0],ECX
```

**What is NOT established `[?]`:** where those two blobs live in
`streamed.dat`. Reading `[blockB + 0x10 + 0xA4]` on the shipped files does not
produce a plausible probe array — the L00 byte spans the full −128..127 range
rather than the non-negative range an irradiance L00 must have. So either the
runtime unit object is not the raw block image, or the block header is not
`0x10` here. **An extractor must not be written against this offset until
that is pinned down**, and none has been.

### 12.5 What a port needs

The consumer is small and fully recovered; the producer is not. A renderer
supplies **only the car's world position** per frame, per car:

```
idx = unitContaining(carPos)                  -> -1 = keep previous nine
(p0,p1,p2,w0,w1) = bspCast(unit[idx], carPos, down 20.0)
L[i] = decode9(p0) + (decode9(p1)-decode9(p0))*w0
                   + (decode9(p2)-decode9(p0))*w1
```

then the existing pipeline is unchanged: `FUN_000313E0` builds the matrix,
`FUN_000315C0` scales the four rows and uploads `c0..c3`. Since the port's
`b3_carfx_set_sh()` already takes exactly these nine floats, **no new module
API is required** — only the probe volume, which is the `[?]` above.

---

## 13. The shader itself — RECOVERED. §0.1, §2.6 and §2.9 are superseded

§2.6 concluded "the NV2A register-combiner / vertex program the constants feed
could not be recovered", and §0.1 concluded from that "the car body has no
environment map". **Both are wrong**, and the reason is a wrong premise: the
search was for a DirectX 8 *token stream* (`0xFFFE0101` / `0xFFFF0101`), which
an Xbox title does not ship. It ships the NV2A binaries.

### 13.1 The vertex program is at `0x003E7D58` [C]

`FUN_0003C8A0`, immediately after the Fresnel writer it is identified by,
does

```
0003cb60  CALL 0x0002ef90          ; the car Fresnel pair (§2.5)
0003cb6e  PUSH 0x3e7d58            ; pFunction
0003cb73  PUSH 0x387558            ; pDeclaration  (§11.1)
0003cb78  CALL 0x0034f440          ; CreateVertexShader
```

and `0x0034F440` reads `u16[0]` (version) and `u16[1]` (instruction count) of
`pFunction`, then sizes a push buffer at `count*16` plus one method header per
128 bytes — i.e. `pFunction` is `{u16 0x2078, u16 32}` followed by **32
16-byte NV2A vertex instructions**. Decoded (`scratchpad/carshine/vsh.py`;
`validate_carfx.py` §13 re-decodes it and asserts each line):

| # | instruction | meaning |
|---|---|---|
| 0-3 | `dp3 r2.xyz, v2, c[116..118]` | the **world-space normal** |
| 4-7 | `dp4 r3, r2, c[96..99]` | `M · (N,1)` — the SH matrix `FUN_000315C0` uploads to hw slot `0x60` |
| 4 | `mov oT0, v9` | the paint UV |
| 9 | `dp4 oD0.xyz, r3, r2` | **`E(N) = (N,1)ᵀ M (N,1)`** → the diffuse colour |
| 8,10,11 | `dp4 r4.xyz, v0, c[116..118]` | the world-space position |
| 12 | `add r5.xyz, c[108], -r4` | **`c108` is the eye position** — `FUN_00031690` uploads `0x004D67D0..` to hw slot `0x6C` |
| 13,14,16,18,19 | | `r7 = normalize(r5)`, `r8.w = N·V` |
| 21 | `mad oT1.xyz, 2N, N·V, -V̂` | **the reflection vector**, into texcoord 1 |
| 20,22,23,25-28 | `mad r5.x, (1-\|N·V\|)^5, c111.y, c111.x` | **Schlick**, `c111 = (R0, 1-R0, 1, 1)`, hw slot `0x6F` |
| 30 | `min oD0.w, r5.x, c111.z` | `F` → the diffuse **alpha** |
| 15,17,24,29,31 | | the viewport-scaled `oPos` |

So the vertex stage produces exactly `oT0 = uv`, `oT1 = R`, `oD0 = (E,E,E,F)`.

### 13.2 The pixel shaders are six `D3DPIXELSHADERDEF`s in `.rdata` [C]

`DAT_004D6550/54/58/5C` are **pixel**-shader objects, not vertex handles:
`FUN_001DABD0 → FUN_0034E790` reads `[obj+8]`'s `+0xD8` (`PSTextureModes`),
`+0xE4/+0xE8/+0xEC` (`PSC0Mapping` / `PSC1Mapping` /
`PSFinalCombinerConstants`) — the Xbox `D3DPIXELSHADERDEF` layout — and
`FUN_0003C8A0` builds each object by allocating `0xFC` bytes and copying
`0x3C` dwords = **240 bytes = sizeof(D3DPIXELSHADERDEF)** from `.rdata`.
The renderer object is `0x004D6170` (`FUN_0002F260`'s argument, whose
`+0x38..+0x44` are the glass rasters `FUN_00031690` republishes into the draw
context), so:

| slot | global | def VA | shader |
|---|---|---|---|
| `+0x3D8` | `0x004D6548` | `0x003E8288` | glass pass 1 |
| `+0x3DC` | `0x004D654C` | `0x003E8378` | glass pass 2 |
| `+0x3E0` | `0x004D6550` | `0x003E8468` | **body** |
| `+0x3E4` | `0x004D6554` | `0x003E8558` | alt material |
| `+0x3E8` | `0x004D6558` | `0x003E8648` | the `mask&2` branch |
| `+0x3EC` | `0x004D655C` | `0x003E8738` | light / emissive |

The body def (`scratchpad/carshine/psh.py`; `validate_carfx.py` §14 re-decodes
every word):

```
PSTextureModes 0x21  -> t0 PROJECT2D, t1 PROJECT2D     <-- TWO texture stages
PSC0Mapping ff3ff3fe -> stage0 = ps c14, stages 2,5 = ps c3, others = c15
0 rgb  r0.rgb = t0.rgb * v0.rgb            *2      albedo x E(N), DOUBLED
0 a    r0.a   = (1-c14.a)*v0.a + c14.a            P + (1-P)*F   (P = c14.w)
1 a    r0.a  *= t0.a                              the paint ALPHA is the gloss mask
2 a    r0.a  *= c3.a                              the fade
3 rgb  r0.rgb = r0.a*t1.rgb + r0.rgb*(1-r0.a)     LERP TO THE ENVIRONMENT MAP
4 a    r0.a   = t0.a * t1.a
5 a    r1.a   = r0.a * c3.a
6 a    r1.a   = r1.a - c15.z                      minus (1-K)
7 a    r1.a   = r1.a * c15.w             *4       times M/(1-K)
final  rgb = c14.rgb*r1.a + (1-c14.rgb)*0 + r0.rgb   a = 1   CLAMP_SUM
```

i.e.

```
base = 2 * paint.rgb * E(N)
refl = paint.a * (P + (1-P)*F) * fade
col  = mix(base, env.rgb, refl)
spec = clamp(paint.a*env.a*fade - (1-K)) * M/(1-K)
out  = col + lightRGB*spec ,  alpha 1
```

The `×4` on stage 7 is the independent confirmation of the whole decode: the C
code builds `c15.w` by multiplying by exactly `0.25` (`MULSS 0x003B1730`
@`0x00031C70`), which §2.4 had already guessed was "the reciprocal of the
combiner's 4× output scale". It is, and the scale is in the OCW.

**Three corrections to §2.9 fall out of it**, all of them things the port was
getting wrong:

* the diffuse term is **doubled** — the port was rendering the car half-lit;
* `P` (`c14.w`) is the **Fresnel floor of the reflection**, not "the pass
  alpha" (§2.9's correction) and not an RGB gain (the cut before that);
* the paint texture's **alpha channel is the per-texel gloss mask** and was
  being ignored entirely.

Glass pass 2 is the same shape with its own literal constant: its stage-0
`C0` nibble is 5, nothing ever uploads pixel register `c5` on that path, so the
value used is the def's own `PSConstant0[0] = 0x4DE5B2E5` → `(0.898, 0.698,
0.898, 0.302)`, giving `reflectance = 0.302 + 0.898·F`. Its colour is
`t1.rgb` — **the glass reflection is the environment map, not the glass
raster** (another §2.9 correction). The alt-material def has an all-zero final
combiner, which `FUN_0034E790` special-cases at `0x0034E81F`: the output is
`r0` directly.

### 13.3 What is bound to stage 1 — still `[?]`

`FUN_00031690` (the car-pass setup, called from all three car draws) binds
stage 1 once for the whole pass:

```
00031740  MOV ECX,[0x004D6C00]
00031746  MOV [0x0075DB74],ECX      ; the shadowed stage-1 texture slot
```

and sets both stages' addressing to `3` (`0x0075D744` / `0x0075D754`, against
the shadow pass's `2` at `0x0075D740`). `0x004D6C00` is
`vehicleRenderer+0xA90` and it has **exactly one reference in the entire
image — that read**. An exhaustive `disp32 == 0xA90` scan over every segment
and a full-image decompile sweep of all 7434 functions for `+ 0xa90` find only
unrelated objects. So the *shape* of the environment term is `[C]` and its
*content* is `[?]`. `src/burnout3_carfx.c` therefore substitutes the same L2
probe evaluated along the recovered reflection vector, marked GLUE, and takes
a real map from `b3_carfx_load_env_map()` / `B3_CARFX_ENVMAP` the moment one
is identified.

---

## 14. The probe container — RECOVERED. §12.4's `[?]` is closed

The probe volume is **the collision mesh**. `FUN_0019D400` reads three `u16`
indices out of the prim record it hit (`prim+0x06/+0x08/+0x0A` for the first
sub-triangle, `+0x0A/+0x08/+0x0C` for the second — exactly parallel to the
prim's `u8` corner indices at `+0x00..+0x03`, and matching `FUN_001B2940`'s
quad split), and indexes `unit+0xA4` with them at stride 9.

`tools/extract_collision.py` already logs `prim+0x06` as "`u16[4]` extra
(unused by the wheel/body query chain)" and `unit+0xA4` as "a second section,
relinked, not collision". They are the **probe indices** and the **probe
array**. §12.4's failed attempt used `blockB + 0x10 + 0xA4`; the right base is
the LOD block itself, the same one the collision parser uses.

Byte-verified on **all 36 shipped tracks**: the array sits at `block+0xB0` on
every unit, `max(index)+1` entries of 9 bytes each fit inside the block, and
**not one of the 1 383 016 decoded `L00` bytes is negative** — which an
irradiance `L00` cannot be, and which a wrong offset would violate about half
the time (`validate_carfx.py` §16).

The barycentric weights are the hit record's `+0x54` / `+0x58`, written by
`FUN_001B24A0` (`0x001B26C3` / `0x001B26D7`) from `FUN_001B2230`'s
Möller–Trumbore `u` / `v`; that test is **one-sided** (`det > 1e-8`) and bounds
`t` to `[-1e-5, 1.00001]`, i.e. the 20.0 cast is a segment.

`tools/extract_light_probes.py` emits `build/tracks/<ID>/light_probes.bin` for
every track (36/36, 10k–86k probes each); its triangle set and bounds are
bit-identical to `collision.bin`'s, which is the cross-check that it is
reading the same mesh the already-validated collision extractor does.
