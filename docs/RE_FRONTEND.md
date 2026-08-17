# Frontend / HUD assets and sound trigger logic (Burnout 3: Takedown, Xbox)

Session 2026-08-10. Provenance marks as in RE_NOTES.md: **[C]** confirmed
(two independent derivations, or read off the bytes and validated by full
extraction), **[S]** single-source, **[?]** open.

Tools: `tools/extract_txd.py` (new), `tools/extract_font.py` (added
2026-08-10 — section 6), consumers `src/burnout3_hud.c/.h` (new).
Output: `build/frontend/*.png` (409 txd textures + 3 XBE fonts),
`src/burnout3_font.h` (glyph metrics).

---

## 1. `Data/Frontend.txd` / `Data/Global.txd` container [C]

RE_NOTES old item 9.3 said these "do not match the RenderWare TXD layout;
format unidentified". Resolved: despite the extension they are **not**
RenderWare stream TXDs (no 0x16/0x15 chunk headers anywhere). They are a flat
Criterion container:

```
+0x00  u32 0x543C0000        constant in both files
+0x04  u32 0xBCDEED81        constant in both files
+0x08  u32 count             Frontend 218, Global 191
+0x0C  u32 entrySize = 16
+0x10  count x {u32 id (1-based, sequential), u32 0,
                u32 absoluteOffset, u32 0}
```

Each offset points at the **same texture record used by static.dat and the
.bgv paints** (tools/extract_textures.py header comment has the full layout):
`+0x04` bitmap offset rel. record (always `+0x80` here), `+0x34` format,
`+0x38/+0x3C` w/h, `+0x40` bit depth, `+0x48` name, `+0x14` palette pointer
array, `+0x69` palette count.

Verified by arithmetic [C]: records are packed back-to-back —
`record + 0x80 + pixelBytes (+ paletteBytes) == next TOC offset` for every
entry in both files, and the last record ends at EOF. Hence **no mip chains**.

Payload encodings [C]:
- fmt `0xC`/`0xF`: standard linear DXT1/DXT5 (same decoder as static.dat).
- fmt `0xB`: 8bpp indices, **Morton/Z-order swizzled** exactly like the .bgv
  paints (linear decode produces stripe garbage; unswizzle8 produces the
  intact "BURNOUT 3 TAKEDOWN" logo, globe, etc.).
- bit depth 4 records (`+0x40 == 4`) still store **one byte per pixel**
  (indices 0..15, verified max index) with a 16 x BGRA palette; depth 8 uses
  256 x BGRA. Palette record: `{01 00 03 00|C0}`, then u32 data offset rel.
  the texture record.
- Car thumbnail records carry `+0x69 == 8` palettes = the 8 paint colour
  variants, same scheme as the .bgv paints (`--all-palettes` emits them).

Extraction result: **409/409 textures decode and validate**
(Frontend 218: 59 DXT1, 63 DXT5, 87 pal-8bpp, 9 pal-4bpp;
Global 191: 101 DXT1, 85 DXT5, 3 pal-8bpp, 2 pal-4bpp).
Every record is named; dims 16x16 .. 1024x512, all powers of two. The
per-texture table (id, name, fmt, dims, depth, palettes, gradient statistic)
is printed by `python3 tools/extract_txd.py`. The 8 gradient outliers
(SmashedGlass, CrackedGlass, Volumebar, TButton, SATMAPU*, HEVYCAR36,
loadCAR1) were inspected visually and are correct hi-frequency art, not
decode failures. One cross-bank name collision: `Takedown` — the Global copy
is written as `Takedown_Global.png`.

Cross-check [C]: nearly all Global.txd texture names appear as string
literals in `.rdata` (file 0x39B960..0x39C4F0, VA = file+0xF7C0) — the HUD
binds its art **by name** at runtime, so the names in the container are the
names the code uses.

## 2. HUD element identification (by name + visual inspection) [C]

In-race HUD (all in **Global.txd**):

| Element | Texture(s) |
|---|---|
| speedo / cluster backing panel | `hud_element01` (dark swoosh, 128x64) |
| boost bar flame animation | `BoostFireEdge01..41` (strip), `BoostFireCore01..30`, `BoostFireOver01..20`, `BoostEarnFlame`, `boostflare`, `coronaboost`, `coronaboostred` |
| crash-mode boost/health bar | `health_bar` (vertical bar), `BoostBits` (tyre-tread x2/x3/x4 multiplier art) |
| panel corners / frames | `16_curve(b)`, `box_curve(b)`, `big_curve`, `small_curve`, `bg`, `grid`, `circle` |
| takedown / event signs | `hud_signs_td`, `hud_sign_flame`, `hud_sign_skull`, `hud_sign_crash`, `hud_sign_exclam`, `Takedown` (flame roundel), `sign_tip` |
| crash-mode pickups & score | `pickup_boost`, `pickup_gamebreaker`, `pickup_multiplierx2/x4`, `pickup_multiplier_skull`, `pickup_score01-03`, `pickup_stealer`, `HUD_crash_boost/breaker/score01-03/skull/stealer/x2/x4`, `HUD_Strikeout` |
| ratings / medals | `MedalGold/Silver/Bronze`, `hud_award_star`, `hud_boost_stars`, `Aftertouch`, `Bad`, `check`, `finish`, `Cup` |
| controller prompts | `Buttons` (B/A/X/Y/black/white/L/R sheet), `A_Button`, `B_Button`, `dpad`, `TButton` (trigger letter sheet) |
| misc sprite sheet | `HUD` (brackets + rev lamp), `Arrow`, `loading_bars`, `Lobby`, `Shutter` |
| particle/fx (world, not HUD) | `fx*` (14), `CrackedGlass`, `SmashedGlass`, `UnbrokenGlass`, `VehicleUnderside`, `envmapsun`, `WaterFresnel`, `radialblurmask`, `blobbyshadow`, `sunblob`, `coronaglow`, `coronastar` |

Frontend/menu art (all in **Frontend.txd**): car-class thumbnails
`COMPCAR1..SUPRCAR10` (128x64, 8 paint palettes each — matches the .bgv
paint variants), track selection stills `*-st1/st2`, postcard art `HD*`,
preview `PC*`, satellite maps `SATMAP{A,E,U}*`, world/progress
`World_Map/progmap/progdots/points_globe/globe`, medals/trophies, language
flags (`France/GB/Germany/…`), Xbox Live icons
(`Friendonline/Invitation/Mail/…`), `titlescreen_logo`, difficulty tiers
(`easy/advanced/expert/champion/driving-skills`).

**No font/digit texture exists in either txd** [C] — and section 6 (added
2026-08-10, later session) resolves where the glyphs actually live: the
three fonts are **compiled into the XBE .data section**. The old open
item is CLOSED. (`Globalus.bin`/`Headus.bin` remain string tables: same
16-byte-entry TOC idea but magic `0x69F60308/0xB93622FA`; entries are 4
offsets to NUL-terminated **UTF-16** strings — see 6.4.)

## 3. Sound trigger logic (XBE) — what is verified

**The XACT-cue-table premise is falsified** [C]: the executable contains no
`SDBK`/`XACT`/`WBND` magic. Runtime SFX use RenderWare Audio dictionaries
(.awd) with waves looked up **by name**, packed base-40; XWB music banks are
referenced by filename only (consistent with AUDIO_NOTES: ENTRYNAMES empty).

Mechanism [C] (from decompiles):
- `FUN_001c9c80(fmt, path, prio, 0x2000)` opens/loads a wave dictionary.
- `FUN_001aeaa0(name)` packs the wave name into a u64, base-40
  (`value = value*0x28 + charCode`, `'_'`->0x27, `'/'`->2, case-folded) —
  the same encoding family as the vlist.bin car IDs.
- `FUN_001c99d0(dict, packed64)` resolves the wave handle in the dictionary.

Static registration table in `.data` at **0x3EBFC8..0x3EC44C** [C]: pointers
pairing every AWD bank with its ValueDB config group(s):
`Sound/Voice`, `Sound/Misc`, `Sound/Surface` (surface names None/Tarmac/
Offline Tarmac/Gravel/Pavement/Snow/Offline Gravel/Metal/Wood/Concrete),
`Sound/Skids/Sample1-3/Variant1`, `Sound/Turbo`, `Sound/Hud/General`,
`Sound/Hud/Aggressive Driving`, `Sound/Traffic`,
`Sound/CrashingTraffic/Skids/{Small,Large}`, `Sound/Boost`, `Sound/Doppler`,
`Sound/LTrain`, `Sound/Esm`, `Sound/Crash`, `Sound/Scrape/{Low,High}`,
`Sound/Component/{Glass,Large,Small}/{Deform,Remove}`,
`Sound/{World,Car}/{Nudge,Crash}`, `Sound/Prop/<15 categories>`,
`Sound/Crash Stream`, radio pieces `_EATrax`+`0|1`+`.xwb`.

Verified event -> sample bindings (string xref + decompile, addresses are
burnout3.elf VAs):

| Function | Event | Sample(s), bank | Evidence |
|---|---|---|---|
| `FUN_00142550` | crash-escape jingle; crashing-traffic skids | `crashescap`; `smltrafskd12`, `smltrafskd22`, `lrgtrafskd` — all `sound\generic.awd` | opens generic.awd, 4x pack+lookup, handles at +0x48/+0x0C/+0x10/+0x14 |
| `FUN_00146530` | boost/doppler pass-by | `bstpss0%i` (small), `bstpsl0%i` (large), i=1..4; `cardry1` — generic.awd | sprintf'd names, lookup on trigger; speed-dependent vol/pitch from globals in the Doppler param block ("Big/Little Pass Scalar" [S]) |
| `FUN_00147800` | traffic vehicle engines | `cartar0%i`, i = `rand()%5+1` (cars); `trkeng01` (lorry); `buseng01` (bus) — generic.awd | literal `_sprintf(buf,"cartar0%i",rand%5+1)` in decompile; type switch on vehicle+0x173 |
| `FUN_0013f9c0` | game-mode SFX bank chooser | mode 0/4 -> `sound\single.awd`; 1 -> `elim.awd`; 3 (sub 1-2) -> `roadrage.awd`; 6 (sub 3-5) -> `crash.awd` | switch over two vtable getters (+0x90/+0x94) |
| `FUN_0013ea20` | per-track surface/prop bank | `tracks\<track>\sound.awd` via `"%ssound.awd"` | string xref |
| `FUN_00148ee0` | per-vehicle engine loops | `pveh/<class>/Car<N>.hwd` / `.lwd` via `_sprintf("%swd", base+"h"|"l")`, high/low flag at +0x5e | decompile; `.h`/`.l` strings registered next to Sound/Esm.cfg |
| `FUN_0014b600` | crash-mode impact bank | `sound\g_crsh##.awd` (base `g_crsh04` + variant) or `sound\crashmod.awd`; binds ~20 wave handles | decompile |
| `FUN_0014b600` | crash aftermath music rotation | `tracks\crash%d.rws` with `%d = (u8 counter % 20) + 1`, counter incremented per crash -> sequential loop through the disc's crash1..crash20.rws | literal in decompile (`% 0x14 + 1`) |
| `FUN_00140110` | frontend UI sounds | `sound\fe.awd` | string xref |
| `FUN_00135240`, `FUN_0013dbc0` | generic bank preload | `sound\generic.awd` | string xref |

Open [?]:
- **Prop-impact wave names** (`woodenboxh22`, `trafficone33`, `oildrumhit12`,
  … in each track's SOUND.AWD) do **not** appear in the XBE. The
  `Sound/Prop/<category>/` VDB groups carry `Min Impulse`/`Max Impulse` (and
  presumably wave-selection) parameters; how a category resolves to its wave
  name/index was not recovered. Do not assume name-similarity mapping.
- DJ speech selection: `.rdata` has `djmcr/djmbl/djmrr/…`, `DJAS/DJEU/DJUS`,
  `djrace/djgen/djwww/ident` and a `"%s%s%d.awd"` builder near
  `Sound/Radio.cfg`; the E_DJ*.xwb entry-selection logic was not decompiled.
- The `Sound/*.cfg` gain/pitch parameters live in hashed form in
  `Data/vdb.xml` (same pipeline as RE_NOTES section 10); individual sound
  keys were not extracted this session.

## 4. HUD harness module

`src/burnout3_hud.c/.h` — draws the in-race HUD from the recovered data
(rebuilt 2026-08-10, now wired into the Makefile + burnout3_full.c):
real `GlobalFont` glyphs (section 6) for every numeral/label with the
game's Globalus strings ("POS.", "LAP", "mph", "1st".."6th"), `big_curve`
POS/LAP chrome panels, `hud_element01` speed swoosh, the `BoostBits`
tread band backing the boost bar with the 41-frame BoostFireEdge flame
fill + BoostEarnFlame head glow, and a transient ordinal position
callout. Internally everything is drawn in the game's 640x480 virtual
space; element positions and the gold-gradient/outline/italic text
styling are calibrated against a reference gameplay frame and marked
[S-reference-calibrated] in the source (the game's own numeric layout
init was not recovered — section 6.5). `B3_HUD_TEST=1` forces the
callout + a 65% boost fill for screenshot verification.

Integration (3 lines + build):
```c
#include "burnout3_hud.h"                 // top of burnout3_full.c
b3_hud_init("build/frontend");            // once, after IMG_Init(IMG_INIT_PNG)
b3_hud_draw(mph, boost_frac, lap, total_laps, pos, n_cars, dt);
                                          // end of render_frame(), before swap
```
plus `src/burnout3_hud.c` on the Makefile compile line. Standalone check
(run this session, clean):
```
gcc -c -Wall -Wextra -std=c11 -O2 -Isrc $(pkg-config --cflags sdl2) src/burnout3_hud.c
```

## 5. Reproduce

```
python3 tools/extract_txd.py                 # both banks -> build/frontend, prints table
python3 tools/extract_txd.py --all-palettes  # + car-thumbnail colour variants
```
Exit code is non-zero if any texture fails structural validation.

## 6. The fonts — compiled into the XBE (2026-08-10, closes the section-2 [?])

### 6.1 Discovery chain [C]

The digit/glyph store hunted in section 2 is in the executable itself.
`FUN_0002ef90` (font-system init, called from `FUN_0003c8a0`) binds three
font objects to their textures **by name** through `FUN_0002ddf0` — a
`__stricmp` walk over a texture container's records comparing at
`record+0x48`, the same name field as every texture record in the game.
The names `"GlobalFont"/"HeadFont"/"SmallFont"` (.rdata
0x3aaed0/0x3aaec4/0x3aaeb8) appear in **no shipped data file** (full-dump
grep); the only records carrying them sit in `.data`, each font object
followed by its texture record and DXT5 bitmap:

| font | object VA | texture record VA | dims | coverage |
|---|---|---|---|---|
| GlobalFont | 0x3c84d8 | 0x3c9c38 | 256x256 | full ASCII 32..126 (94 glyphs) — **the HUD font** (digits 12x22 px) |
| HeadFont | 0x3d9cb8 | 0x3da338 | 128x256 | space, `-`, A–Z minus W (27) — headline caps |
| SmallFont | 0x3e23b8 | 0x3e3b18 | 128x128 | full ASCII 32..126 (94) — small text |

Pointer slots `[0x3e7b98]/[0x3e7bb0]/[0x3e7ba4]` hold the object VAs
(assignment order read off the decompile: renderer obj+0x10 ← [0x3e7b98]
gets "GlobalFont", +0x14 ← [0x3e7bb0] gets "HeadFont", +0x18 ← [0x3e7ba4]
gets "SmallFont" — note the crossed order, it cost one mis-pairing before
the per-font coverage/dims agreed). All three atlases are **white glyphs +
DXT5 alpha**; colour is per-vertex at draw time (text scene-graph nodes
carry an RGBA each — `FUN_001c15a0/FUN_001c17f0` init nodes as
`{pos.xy, scale.xy, rgba[2]}` with parent-multiplied colour in
`FUN_001c1670`). The gold, the white, the menu blue are all runtime tints
of the same white glyphs.

### 6.2 Font object + glyph record layout [C]

```
font object ("3rev" magic):
 +0x00 char[4] "3rev"     +0x04 u32 texture handle (runtime)
 +0x08 f32,f32 layout scale (FUN_001c1060 divides widths by +0x08)
 +0x18 u32 relocated flag +0x1C ptr default glyph record
 +0x20 ptr[128]           charmap for (char & 0x7F)
glyph record (0x20 bytes):
 +0x00 f32 u   +0x04 f32 v      top-left, / texture size (-1 = blank)
 +0x08 f32 w   +0x0C f32 h      extent
 +0x10 f32 xoffset +0x14 f32 yoffset
 +0x18 f32 advance +0x1C u32 charcode
```

Proven two ways: (a) `FUN_0002ef90`'s relocation loop adds the object base
to +0x1C and all 128 +0x20 slots exactly once (+0x18 guards); (b)
`FUN_001c1060`/`FUN_001c0f50` look up `charmap[c & 0x7F]`, compare the
record's +0x1C charcode, and on mismatch walk 0x20-stride sibling records
until the default record — the charmap holds chain HEADS shared by aliased
codes (e.g. `'\x60'` chains behind `'à'` = 224, 224&0x7F == 0x60). Decoded
metrics are self-consistent for every mapped char in all three fonts
(checked by tools/extract_font.py, which mimics the exact walk and exits
non-zero on any charcode/UV inconsistency).

### 6.3 Extraction

```
python3 tools/extract_font.py   # -> build/frontend/{Global,Head,Small}Font.png
                                #    + src/burnout3_font.h (pixel metrics)
```

### 6.4 HUD strings (Data/Globalus.bin) [C]

UTF-16 string table; each 16-byte TOC entry = 4 string offsets. In-race
HUD labels: `POS.` (entry 167), `Lap`/`LAP` (234/500), `mph`/`MPH`
(496/501), ordinals `1st..6th` (146..148, dupes 498..499).

### 6.5 In-race HUD draw code map [C locations, layout still open]

* `FUN_0004dd00` binds the in-race HUD art by name into handle globals:
  `boostfirecore%02d`→0x4607c8[30], `boostfireedge%02d`→0x460848[41],
  `boostfireover%02d`→0x460770[20], `boostbits`→0x460938,
  `boostearnflame`→0x4607c4, `hud_element01`→0x46093c,
  `health_bar`→0x460840, `hud_boost_stars`→0x460940, `Aftertouch`→0x4607c0,
  a 17-entry sign table (`hud_sign_flame`…)→0x4608f0.
* The HUD module is 0x461c0..0x4e360; elements are 2D scene-graph objects
  drawn via `FUN_000461c0`/`FUN_000474d0` (dispatcher `FUN_001ae340`),
  positions held per object as **normalized 0..1 screen fractions**
  (`FUN_000461c0` multiplies obj+0x10/+0x14 by the screen dims
  DAT_004d6668/DAT_004d666c).
* The 2D virtual space is **640x480** ([C]: the LOADING drawer
  `FUN_0002f650` centres text at x=320 and places the bar at y 420..435;
  element records like `FUN_0002de40`'s EATrax carry {26,400,330} in the
  same space).
* The numeric per-element init of the in-race layout was NOT recovered
  (values flow through per-element object constructors behind virtual
  dispatch) [?] — the harness layout in `src/burnout3_hud.c` is therefore
  calibrated from a reference gameplay frame and marked
  [S-reference-calibrated] per element.
* `BoostBits` sub-regions (64x256): y 0..31 cap/smoke, **y 33..63 plain
  tyre-tread band** (the race boost bar backing), then x2/x3/x4
  tread-emboss + glow rows at 64/96/128/160/192/224 [C visual].

## 6.6 The in-race HUD layout — CRACKED (2026-08-11, boost-HUD session)

Section 6.5 recorded the in-race element layout as **[?]** ("values flow
through per-element object constructors behind virtual dispatch"). That is
now closed for the boost bar and closed *at the anchor level* for every
in-race element. Chain of custody, all addresses are burnout3.elf VAs
(`.text` = flat + 0x10000):

### 6.6.1 The element graph is a table of static C++ objects [C]

The in-race HUD is built by **FUN_00052CF0 / FUN_00052FC0** (0x00052CF0..
0x00053459), a straight-line sequence of

```
mov <reg>, <static object VA>
mov ecx, <anchor slot>
call <element init>
```

Ghidra reports **no xrefs** to the element inits (FUN_0004BFC0 etc.) because
they are reached by relative `call` from code Ghidra left undefined; a
`E8 rel32` scan over `.text` finds them (`tools/validate_hud.py`'s method is
independent of Ghidra entirely — it reads the ELF).

Recovered elements (object VA, init, anchor slot):

| object | init | slot | anchor |
|---|---|---|---|
| **0x003FD550** | **FUN_0004BFC0** | **3** | **bottom-left — the BOOST BAR (player 1)** |
| 0x003FE6A0 | FUN_0004BFC0 | 3 | bottom-left — boost bar (player 2, split screen) |
| 0x003FDD48 | FUN_00059850 | 5 | bottom-right (speed cluster region) |
| 0x003FEE98 | FUN_00059850 | 5 | bottom-right (player 2) |
| 0x003FDFA8 | FUN_0005A3F0 | 7 | top-right |
| 0x003FDC00 | FUN_00056DB0 | 0 | top-centre |
| 0x003FED50 | FUN_00056DB0 | 0 | top-centre (player 2) |
| 0x003FE460 | FUN_0005A390 | 5 | bottom-right |
| 0x003FF2C8 | FUN_000509B0 | 0 | top-centre |
| 0x003FDF70 | FUN_0004F990 | 0 | top-centre |
| 0x003FE268 / 0x003FEFE8 | FUN_00054630 | 8 | viewport origin |
| — | FUN_0004F020 | 3 | bottom-left |
| — | FUN_000511C0 | 7 | top-right |
| — | FUN_0004EDC0 | 8 | viewport origin |

The objects are zero-initialised statics whose `+0x00` is a 3-entry method
table inside the shared block 0x003AB338..0x003AB3B4; the boost bar's is
`{FUN_0004D800 update, FUN_0004D830, FUN_0004E2F0}` and FUN_0004D800 calls
the real per-frame update FUN_0004C390.

### 6.6.2 The anchor table — 0x003FD410 [C]

`FUN_00053BE0(EAX=&out, EDX=slot, [esp+4]=viewport)`:

```
if (slot == 8) { out = {0,0}; return; }              // 0x00053BE9
vw = vp[0x50]-vp[0x48];  vh = vp[0x54]-vp[0x4C];     // viewport extent
out.x = vp[0x48] + TABLE[slot].x * vw;               // 0x00053C23
out.y = vp[0x4C] + TABLE[slot].y * vh;               // 0x00053C3C
```

`TABLE = 0x003FD410`, stride 8, the 3x3 screen-anchor grid:

| slot | value | meaning |
|---|---|---|
| 0 | (0.5, 0.0) | top-centre |
| 1 | (0.0, 0.0) | top-left |
| 2 | (0.0, 0.5) | mid-left |
| **3** | **(0.0, 1.0)** | **bottom-left** |
| 4 | (0.5, 1.0) | bottom-centre |
| 5 | (1.0, 1.0) | bottom-right |
| 6 | (1.0, 0.5) | mid-right |
| 7 | (1.0, 0.0) | top-right |
| 8 | (0.0, 0.0) | viewport origin (special-cased) |

This is the "normalized 0..1 screen fractions" 6.5 suspected, found.

### 6.6.3 The boost bar's box — 360 x 28 at (0,452) [C]

`FUN_00048800` (the boost bar's own layout builder, reached from
FUN_0004BFC0 @0x0004C0A9) fills the draw node:

```
node.x  = ref.x - anchor.x * 360.0     // 0x003FCAA0, mulss @0x0004880D
node.y  = ref.y - anchor.y *  28.0     // 0x003FCAA4, mulss @0x00048822
node.w  = 360.0 ; node.h = 28.0
node.rgba  = {1,1,1,1}                 // 0x003B168C x4
node.+0x20 = &obj[0x20]                // the boost state block
node.+0x3C = 0x0004AE40                // the draw callback
```

with `ref = {0,0}` (xorps @0x0004C05E) and `anchor = {0,1}` (@0x0004C056).
Slot 3 puts `ref` at the viewport's bottom-left, so in the 640x480 virtual
screen the boost bar occupies

> **x 0 .. 360, y 452 .. 480** — flush to the bottom-left corner.

This **supersedes** the old `[S-reference-calibrated]` "boost tread
x 28..117, y 429..449" in section 4: that was ~4x too narrow. The retail
bar spans 56% of the screen width.

The quad emitter `FUN_001C7430(EAX=verts, [esp+4]=node, count, uvs)` adds
`node.x/node.y` to each vertex, so everything the boost bar draws is in
box-local pixels — which is why the recovered ratios below can be applied
directly.

### 6.6.4 The boost HUD state machine — FUN_0004C390 [C]

`obj+0x68C` is the player's **score object** (racecar+0x10D0, stride 0x27E0
from 0x0073B2A0) — cross-confirming RE_GAMEPLAY §3, since the fields it
reads are the boost record at score+0xCC:

| read | score off | boost record | meaning |
|---|---|---|---|
| +0x0FC | +0xFC | rec+0x30 | bar tier |
| +0x100 | +0x100 | rec+0x34 | bar size |
| +0x104 | +0x104 | rec+0x38 | meter |
| +0x108 | +0x108 | rec+0x3C | lifetime earned |
| +0x110 | +0x110 | rec+0x44 | min units |
| +0x11E | +0x11E | rec+0x52 | boosting flag |

Per-frame (element-object offsets; the draw side sees them at -0x20):

```
+0x520 clock      += dt
+0x524 A          =  (tier+1) * 0.25                    @0x0004C49E
       target     =  meter * (1/bar_size) * A
+0x528 B          =  target                    if target <= B   (instant drop)
                     min(B + 0.625*dt, target) otherwise        @0x0054F4A0
       earning    =  (target - B) > 0.025                       @0x0004CDA5
+0x530 flame      =  min(flame + 5*dt, 1)  if boosting||earning @0x0004CE67
                     max(flame - 2*dt, 0)  otherwise            @0x0004CE2A
+0x52C earnflash  =  min(+3*dt, 1)  on the frame `earned` rose  @0x0004D0B8
                     max(-0.75*dt, 0) otherwise                 @0x0004D095
+0x540 sparks     += 16/s, one particle per whole unit          @0x0004CE8F
```

Bar-size change (takedown upgrade / slam loss): `+0x534 := -0.5`,
`+0x53A := 1` (shrank) or `2` (grew); SFX FUN_00140EF0 (shrink, as the timer
crosses 0) / FUN_00140DF0 (grow, crossing 0.3, @0x003B1750); the element's
node x is shaken by `(50 - (t-0.3)*125) * <sin table>` for
0.3 < t < 0.7 (@0x003B16B8 / 0x003B03F4 / 0x003B1CEC); the fill is not
allowed to climb again until `t > 1.2` (@0x003B1768). The element also
slides in 150 px on y over its first second (@0x003B16D8).

### 6.6.5 The flame layers — when and how fast [C]

`FUN_0004AE40` draws, in order: the bar plate (FUN_000488A0), the two
20-slot spark tables (FUN_0004A740), then — **only if the earn flash is
above zero** — the earn flame, and — **only if the fill B is above zero** —
the three fire strips:

| layer | function | texture handles | frame index | notes |
|---|---|---|---|---|
| earn | FUN_00049E40 | `boostearnflame` 0x004607C4 | single quad | blue comet at the fill head; the **earn indicator** |
| tread | FUN_000496E0 | `boostbits` 0x00460938 | — | 9 wobbling segments, amber->white |
| core | FUN_00049AD0 | `boostfirecore%02d` 0x004607C8[30] | `(int)(30.0*clock) % 30` | magenta blobs, rate *= 0.95 per pass |
| edge | FUN_00049FD0 | `boostfireedge%02d` 0x00460848[41] | `(int)(24.0*clock) % 41` | the orange plume, rate *= 0.95 per layer |
| over | FUN_0004A470 | `boostfireover%02d` 0x00460770[20] | `(int)(25.0*clock) % 20` | magenta streak band |

So the answer to "when does the bar render flames vs plain" is: **the fire
strips are gated on the fill B > 0 alone** — a non-empty bar always burns.
What the *boost/earn* state changes is the plume's height and brightness,
through the `flame` level (0..1) that feeds FUN_00049FD0's curve term.
`+0x51A == 2` additionally suppresses the fill climb during the grow
animation, and crash-party modes (FUN_00017310) skip the whole draw when
the fill is zero (@0x0004AE71).

`FUN_000496E0` tread geometry (box-local, x right / y down from the box
top-left):

```
seg_w   = (1 + 2*0.00555556) * 0.11111111 * w          = 40.44 px
x0      = -0.00555556 * w                              = -2.0 px
rails   = PROFILE[i][0..2] * 1.0 * h  (+ 0.0 * h)      0x003AB038, 10 x 6 f32
mix     = PROFILE[i][3..5] * 0.6 + tri(i*0.25 + t*1.8) * 0.4
colour  = lerp({1,0.8,0.1,0.8}@0x3FCAC0, {1,0.9,0.8,0}@0x3FCAD0, mix)
u       = 0.75 + (i / (9*B)) * 0.2421875   -> BoostBits x 48..63.5
v rails = 0.001953125 / 0.0625 / 0.123046875  -> BoostBits y 0.5..31.5
loop while i/(9*B) < 0.999                             -> 9*B segments
```

`FUN_00049FD0` plume geometry:

```
span     = (0.00138889 + B) * w
x(t)     = box.x - 0.0111111*w + span*t*{1.15+0.15*flame (top),
                                         1.10+0.05*flame (bottom)}
top rail = (0.214286-1.0)*h - t^2 * curve * B * h       = -22 - dip
mid rail = ((0.214286*h) + (0.214286-1.0)*h) * 0.5      = -8
curve    = (3.428571-2.142857)*flame + (2.142857-1.0)
t < 0.125: both rails lerp back to 0.214286*h           (flat tail)
v        = 0.015625 (top) .. 0.65 (bottom)
each further layer: rate *= 0.95, span quantised down to a multiple of
                    (0.277778-0.0444444)*w = 84 px
```

At full bar and full flame the plume reaches ~68 px above the box top —
i.e. it burns roughly 2.4 bar-heights into the screen.

**Where the runtime float globals come from:** the geometry ratios live in
BSS at 0x0054F3xx..0x0054F4xx (they read 0 in the image). Their compiled-in
defaults come from one-instruction C++ dynamic initialisers registered in
the table at **0x003B3748..**, each of the shape
`movss xmm0,[const]; movss [global],xmm0; ret` (or `xorps` for zero).
`tools/validate_hud.py` decodes those thunks, so the values are read out of
the binary rather than assumed.

### 6.6.6 The earn text callouts [C]

The tiered category callouts are **ASCII string literals in the XBE**, not
Globalus.bin entries. Pointer table at **0x003C8390**, 16 entries,
`[category][tier]`:

| | tier 0 | tier 1 | tier 2 | tier 3 |
|---|---|---|---|---|
| **AIR** (0x003C8390) | GOOD AIR! | GREAT AIR! | FANTASTIC AIR! | AWESOME AIR! |
| **DRIFT** (0x003C83A0) | GOOD DRIFT! | GREAT DRIFT! | FANTASTIC DRIFT! | AWESOME DRIFT! |
| **ONCOMING** (0x003C83B0) | GOOD ONCOMING! | GREAT ONCOMING! | FANTASTIC ONCOMING! | AWESOME ONCOMING! |
| **NEAR MISS** (0x003C83C0) | GOOD NEAR MISS! | GREAT NEAR MISS! | FANTASTIC NEAR MISS! | AWESOME NEAR MISS! |

The strings themselves are at 0x003AA50C..0x003AA61C. The tier index is the
category tier that **FUN_00192D20** maintains against the "Score/BP
Categories" minima tables (RE_GAMEPLAY §2/§5: air 0x3F7550, oncoming
0x3F7560, drift 0x3F7570, near-miss counts 0x3F75A4) — i.e. the same tier
that selects `<Category> BP`. So a scoring event's award tier and its
on-screen shout are the same number.

### 6.6.7 What is now [C] vs still calibrated

**[C] (binary, re-read by `tools/validate_hud.py`, 98/98 green):** the
boost bar box (360x28) and its screen placement (0,452)-(360,480); the
anchor slot table and every in-race element's slot; the fill/earn/flame/
spark rates and thresholds; the bar-size-change timings and shake; the
three flame strips' frame counts, frame rates and 0.95 decay; the tread
band's segment count, widths, UV window, 10x6 silhouette table and the two
gradient colours; the plume's span/lean/curve/rail constants; the 16 earn
callout strings and their table order.

**[S] :** the plate silhouette FUN_000488A0 emits (its two quad strips were
read but not reproduced vertex-for-vertex), and the per-section render
state (FUN_001C8470 / FUN_001C82E0 with 0/1) — so the **layer intensities**
the harness modulates each additive strip by are [S-ref], picked so the
composite reads like the retail bar. Every geometry and timing number under
them is [C].

**[S-ref] (unchanged, deliberately):** the speed cluster, POS and LAP boxes
and their inner text placement. Their element inits go through the generic
sprite builder (draw callback 0x00048430) whose size comes from the bound
texture at runtime rather than a static constant pair, so only their anchor
slots were recovered. They are still calibrated from the reference
gameplay frame and are marked as such in `src/burnout3_hud.c`.

**[?] :** the spark particle records' visual parameters (the 0x20-byte
records at state+0x10 and state+0x290, drawn by FUN_0004A740 with the
tables at 0x003FCB10 / 0x003FCB60 — the spawn rate and lifetimes are
recovered, the per-particle art is not); the split-screen viewport rects;
`FUN_0004B1C0`, a second custom-drawn HUD box (**210 x 26**, 0x003FCBE0/
0x003FCBE4, draw callback 0x0004B4D0) whose owning element was not
identified.

### 6.6.8 Reproduce

```
python3 tools/validate_hud.py        # 98/98, reads build/burnout3.elf
```
It parses the machine-checked `#define B3HUD_*` block in
`src/burnout3_hud.h` plus the tables in `src/burnout3_hud.c` and asserts
each value against the address it cites, decoding BSS initialiser thunks
where needed. It is deliberately independent of Ghidra.

## 6.7 The boost bar's per-section RENDER STATE — CRACKED (2026-08-11)

Section 6.6.7 left two things `[S]`: the per-section render state
(`FUN_001C8470` / `FUN_001C82E0` called with 0/1 around the draw sections)
and, consequently, the "layer intensities" the harness modulated each strip
by. That is what made the in-game bar render as a faint translucent purple
wash with no backdrop: the geometry and timing were the game's, the
**colours and blend modes were not**.

Both are now closed, and the plate — the piece that was pure glue — is the
game's own textured art.

**Provenance note.** Nothing in this section came from Ghidra. Every
address, byte and value below was read out of `build/burnout3.elf` (the
`xbe2elf.py` image) directly, or produced by *executing* that image under
Unicorn (`tools/emulate_hud.py`). Two Ghidra function-boundary lookups were
made with an explicit `&program=burnout3.elf` afterwards purely as a
cross-check; they agree (`FUN_001C82E0` body `0x001C82E0..0x001C83C3`,
matching the byte-level disassembly, and `0x0004AE40` correctly reports *no*
function, which is why 6.6.1's "Ghidra has no xrefs to these" note holds).

### 6.7.1 How the engine's render state actually works [C]

`FUN_001D7040` is the state flusher, and reading it gives the whole scheme:

```
for i in dirty_render_states:            # ids in 0x0075DE20, count 0x0075DB60
    if shadow[i] != last[i]: SetRenderState(i, shadow[i])   # FUN_00355390
    shadow  = 0x0075D4A0 + id*4
for stage in 0..3:
    for rec in dirty_tss[stage]:         # 0x0075D2A0 + stage*4 + n*0x10
        type = rec[0]
        SetTextureStageState(stage, type, shadow2[(stage + type*4)*4])
    shadow2 = 0x0075D740
```

Corroborated by the two raw setters: `FUN_001D7130(state, value)` writes
`0x0075D4A0 + state*4`; `FUN_001D7150(stage, type, value)` writes
`0x0075D740 + (stage + type*4)*4`.

`FUN_001C82E0(ESI=i)` therefore sets **render states 62, 63, 74, 67** and
`FUN_001C8470(ESI=i)` sets **texture stage 0, types 0 and 1**, in both cases
from preset tables indexed by `i`. The tables are BSS (they read 0 in the
image); `FUN_001C7150` fills them from compiled-in immediates, and
`tools/validate_hud.py` now replays that function's stores to recover them
(a small mov/movss decoder over `0x001C7168..0x001C72E3` that fails loudly
on an unknown opcode rather than guessing).

| preset table | i=0 | i=1 | i=2 | i=3 |
|---|---|---|---|---|
| 0x004A1A90 SRCBLEND (62) | 0x0302 | 0x0302 | 0x0302 | 0x0001 |
| 0x004A1AB0 DESTBLEND (63) | 0x0303 | 0x0001 | 0x0001 | 0x0000 |
| 0x004A1B00 BLENDOP (74) | 0x8006 | 0x8006 | 0x800B | 0x8006 |
| 0x004A1B34 COLORWRITEENABLE (67) | 0x010101 | 0x010101 | 0x010101 | 0x010101 |
| 0x004A1B24 ADDRESSU (stage0 type0) | 3 | 1 | 3 | 1 |
| 0x004A1B68 ADDRESSV (stage0 type1) | 3 | 1 | 1 | 3 |

**The values identify themselves**, which is what pins the render-state ids
independently of any enum numbering: on Xbox these fields carry the NV2A
tokens, which are the OpenGL enums verbatim — `0x0302 = GL_SRC_ALPHA =
D3DBLEND_SRCALPHA`, `0x0303 = GL_ONE_MINUS_SRC_ALPHA`, `0x0001 = GL_ONE`,
`0x8006 = GL_FUNC_ADD = D3DBLENDOP_ADD`, `0x800B = GL_FUNC_REVERSE_SUBTRACT`;
and `D3DTADDRESS_WRAP = 1`, `D3DTADDRESS_CLAMP = 3`. Nothing else produces
that pattern (preset 0 alpha / 1 additive / 2 subtractive / 3 opaque, and a
2x2 wrap/clamp matrix). So:

> **blend preset 0 = plain alpha, preset 1 = ADDITIVE;
> address preset 0 = CLAMP/CLAMP, preset 1 = WRAP/WRAP.**

`FUN_001C72F0` (2D pass begin) sets `0x004A1B20 = 1` and `0x004A1B5C = 0`,
so the ambient 2D state is **WRAP + alpha**.

### 6.7.2 What FUN_0004AE40 does with them [C]

```
0x0004AF0B  SetTexture(0, boostbits)              # 0x00460938
0x0004AF24  FUN_001C8470(1)   -> WRAP/WRAP  (already ambient, a no-op)
            ... tier-change flash FUN_00048C00 + sparks table 0x003FCB60 ...
0x0004B0D6  FUN_000488A0      -> THE PLATE, under alpha blend + WRAP
0x0004B0E0  FUN_001C8470(0)   -> CLAMP/CLAMP
0x0004B0EA  FUN_001C82E0(1)   -> ADDITIVE
0x0004B103  FUN_0004A740(0x003FCB10)  sparks
0x0004B12B  FUN_00049E40      earn comet
0x0004B170  FUN_000496E0      tread band
0x0004B180  FUN_00049AD0      core blobs
0x0004B199  FUN_00049FD0      edge plume
0x0004B1AA  FUN_0004A470      over streaks
0x0004B1B4  FUN_001C82E0(0)   -> back to alpha
```

### 6.7.3 Where the vertex colour comes from [C]

`FUN_001C7430` (rect batch) and `FUN_001C7710` (strip batch) call
`FUN_001C6920` **once per batch** with `ECX` pointing at a `float[4]`, and
give every emitted vertex that one colour; `FUN_001C7960` takes a
**per-vertex** `float[4]` array instead. `FUN_001C6920` packs it as
`(int)(c*255)` into `A<<24 | R<<16 | G<<8 | B`, i.e. D3DCOLOR ARGB8888
(read straight off its shuffle at `0x001C69A0..0x001C69B1`).

The `float[4]` each section hands over is a constant in the block right
after the bar's box size, multiplied by the draw node's own rgba
(`node+0x10`, `{1,1,1,1}` from `FUN_00048800`):

| VA | value | used by |
|---|---|---|
| 0x003FCAB0 | {1, 1, 1, 1} | plate (`movaps @0x00048962`) |
| 0x003FCAC0 | {1, 0.8, 0.1, 0.8} | tread gradient C0 |
| 0x003FCAD0 | {1, 0.9, 0.8, 0.0} | tread gradient C1 |
| 0x003FCAE0 | {1, 1, 1, 0.5} | earn comet (`@0x00049E5E`) |
| 0x003FCAF0 | {1, 1, 0, 0.67} | core blobs (`@0x00049B77`) |
| 0x003FCB00 | {1, 1, 0, 0.8} | over streaks (`@0x0004A48C`) |
| 0x003FCB10/20/30 | {1,.8,.4,0} {1,.15,.05,.4} {1,.1,0,0} | spark ramp |

**The fire is yellow, not magenta.** The old harness drew the core and over
strips as raw texture at ~0.3 grey; the game modulates them with
`{1,1,0,·}`. That, plus the missing plate, is the entire "purple wash".

### 6.7.4 The per-section table [C]

Captured by executing `FUN_0004AE40` under Unicorn with a seeded element +
score object and reading the 2D vertex pool at `0x00752F78` (stride 0x18:
`f32 x, f32 y, f32 u, f32 v, u32 ARGB, u32 pad`) — `tools/emulate_hud.py`.

| section | fn | texture | blend | tex address | vertex colour(s) |
|---|---|---|---|---|---|
| plate | FUN_000488A0 | **boostbits** | SRC_ALPHA, INV_SRC_ALPHA | **WRAP/WRAP** | `#FFFFFFFF` opaque white |
| sparks | FUN_0004A740 | boostbits (32x32 at 0,0) | SRC_ALPHA, ONE | CLAMP | ramp 0x003FCB10 |
| earn | FUN_00049E40 | boostearnflame | SRC_ALPHA, ONE | CLAMP | `(0.94k, 0.94k, 0.94k, 0.5)` |
| tread | FUN_000496E0 | boostbits | SRC_ALPHA, ONE | CLAMP | per-vertex lerp(C0, C1) |
| core | FUN_00049AD0 | boostfirecoreNN | SRC_ALPHA, ONE | CLAMP | `#AAFFFF00`, tail fades to `#AA000000` |
| edge | FUN_00049FD0 | boostfireedgeNN | SRC_ALPHA, ONE | CLAMP | `#FFFFFFFF` (untinted) |
| over | FUN_0004A470 | boostfireoverNN | SRC_ALPHA, ONE | CLAMP | `#CCFFFF00`, both ends `#00000000` |

BLENDOP is `GL_FUNC_ADD` and COLORWRITEENABLE is `0x010101` (RGB, no alpha
write) throughout.

### 6.7.5 The PLATE — why the backdrop was invisible [C]

It was invisible because the harness drew a hand-picked dark translucent
quad instead of the game's art. `FUN_000488A0` emits **two textured rects
out of BoostBits at opaque white, under alpha blend with WRAP addressing so
the body tiles**:

```
split   = A - 1/6                                   # 0x0054F47C
body:  x 0 .. split*w          u 0 .. split*6       # 6.0 @0x003B1824
       v 0.130859375 .. 0.244140625                 # 0x00388384, span 0x003B2008
cap :  x split*w .. A*w        u 0 .. 1
       v seg*0.25 - 0.244140625, + 0.11328125
if seg <= 1: ONE rect, x 0..A*w, u 0..A*6, body v
```

In the 64x256 BoostBits sheet that is `y 33.5..62.5` for the body and
`y 65.5 / 129.5 / 193.5` for the tier-2/3/4 cap — i.e. the four tread bands
section 6.5 spotted, with the cap carrying the **x2/x3/x4 multiplier art**.
Verified at A = 0.25 / 0.5 / 0.75 / 1.0 against the emitted vertices.

### 6.7.6 Geometry corrections that fell out of the capture [C]

* **core**: 64 x 36 px blobs (`0.177778*w` x `(1.28571+0.0357143)*h`)
  marching back from `head = (0.0222222 + B)*w` in `0.0888889*w` steps, the
  rect **mirrored about h/2 every blob** (the game recomputes `y := h - y`
  each pass — a zig-zag), each with its own strip frame as the rate decays
  0.95 per blob, and a gradient quad `0 .. 0.0222222*w` closing the run.
* **over**: `x -0.00833333*w .. (0.0166667 + B)*w`, `y 0 .. 0.857143*h`,
  with both ends faded to transparent black over `0.0333333*w`.
* **edge**: **three** rails, not two — leans `1.15+0.15*flame` (top),
  `1.10+0.05*flame` (mid), `1.0` (bottom, ending at the box bottom), drawn
  as two stacked 5-column quad strips per layer with v `0.015625..0.65` and
  `0.65..0.984375`. The harness only had the upper strip, so the plume had
  no body.
* **earn**: does **not** track the fill head (the function is never handed
  `B`). It flares at the bar's left end,
  `x -0.0111111*w .. w*(0.94*(0.5+0.5k)*(0.22+0.0111111) - 0.0111111)`,
  `y -0.428571*h .. h+0.428571*h`, checked at k = 0.25/0.5/0.8/1.0.

### 6.7.7 The callout sign + text [C blend, ? colour]

An `E8 rel32` scan of the entire 2D HUD module `0x000461C0..0x0005E360`
finds **2** texture-address preset switches and **9** blend preset switches;
none of them is in the callout path (the 17-entry sign table `0x004608F0`
is indexed at `0x00055D01`, inside a function containing no preset call).
So the **TAKEDOWN! sign and every text run draw under the ambient 2D state**
— `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`, `BLENDOP ADD`,
`COLORWRITEENABLE 0x010101`, address `WRAP/WRAP`. [C]

Their vertex colour is the shared 2D default node colour `float[4]` at
**0x0054FA00** (`movaps xmm2,[0x54FA00] ; movaps [node+0x10],xmm2`
@`0x00051E09`), read by 20 element constructors. It has **no** static
initialiser of either decodable shape — neither the single-float thunk form
nor a `movaps xmm0,[c]; movaps [g],xmm0; ret` float4 form (a scan of
`0x00260000..0x00280000` finds zero of the latter) — so its runtime RGBA is
**[?]**, and the harness's gold-gradient text style stays **GLUE**, now
marked as such in `src/burnout3_hud.c`.

### 6.7.8 What is still open

* the spark particle **records** (spawn/lifetime are recovered, the per-
  particle art is not) — the ramp table is now in the module and validated,
  but no sparks are drawn `[?]`
* `0x0054FA00`'s writer, i.e. the real callout text/sign colour `[?]`
* the two other additive elements at `0x00050130` / `0x00055C51` `[?]`

### 6.7.9 Reproduce

```
python3 tools/emulate_hud.py            # runs FUN_0004AE40 for real, 5 states
python3 tools/emulate_hud.py --full     # every emitted vertex
python3 tools/validate_hud.py           # 175/175 (was 98/98)
```
`validate_hud.py` gained: `[8]` the six section colour float4s + the spark
ramp against their VAs, `[9]` the full preset tables reconstructed from
`FUN_001C7150`'s stores plus the C's GL mapping and draw order, and `[10]` a
differential that executes the real draw callback and asserts the blend
mode, texture address, COLORWRITEENABLE, vertex colours and the plate's two
rects against what the C is built from.

## 6.8 The EVENT TICKER — CRACKED (2026-08-11, ticker session)

The retail in-race HUD stacks rows of **earn labels with star pips** at the
lower left, immediately above the boost bar (`DRIFT ★`, `NEAR MISS ★★★` in
the reference frame). Section 6.6.7 left the box they are drawn in as the
last in-race `[?]`:

> `FUN_0004B1C0`, a second custom-drawn HUD box (**210 x 26**, 0x003FCBE0/
> 0x003FCBE4, draw callback 0x0004B4D0) whose owning element was not
> identified.

That is the ticker row, and its owner is **the boost bar element itself**.
All addresses are `build/burnout3.elf` VAs.

### 6.8.1 It is the boost element's second job [C]

`FUN_0004D800` — the element's update slot in the shared method table
(6.6.1) — is two calls:

```
FUN_0004D800(obj, dt):
    FUN_0004C390(obj, dt)                        @0x0004D80E   the bar
    if (obj+0x56A) FUN_0004D310(obj=EAX, dt)     @0x0004D820   the ticker
```

`obj+0x56A` is a **constructor argument** (`mov [ebx+0x56A], dl` @0x0004C034,
`dl` = `[ebp+0xC]`), so a boost-bar element can be built with the ticker
off. The ticker therefore inherits the bar's anchor slot 3 (bottom-left)
and its score object `obj+0x68C` (racecar+0x10D0, stride 0x27E0 from
0x0073B2A0) — the same one FUN_0004C390 reads.

### 6.8.2 Seven row slots, seven Globalus labels [C]

The constructor `FUN_0004BFC0` lays out **7 row slots at obj+0x570, stride
0x28**, each initialised with a `Data/Globalus.bin` string pointer taken
from the loaded table object `[[0x004D532C]+0x0C] + <byte offset>`:

| slot | obj offset | ctor site | byte off | Globalus entry | label |
|---|---|---|---|---|---|
| 0 | 0x570 | 0x0004C15A | 0x20F0 | 2108 | **ONCOMING** |
| 1 | 0x598 | 0x0004C19C | 0x20F4 | 2109 | **DRIFT** |
| 2 | 0x5C0 | 0x0004C1DB | 0x20F8 | 2110 | **NEAR MISS** |
| 3 | 0x5E8 | 0x0004C21A | 0x20EC | 2107 | **AIR** |
| 4 | 0x610 | 0x0004C259 | 0x20FC | 2111 | **TAILGATING** |
| 5 | 0x638 | 0x0004C298 | 0x2100 | 2112 | **GRINDING** |
| 6 | 0x660 | 0x0004C2D7 | 0x2104 | 2113 | **RUBBING** |

Slot layout (offsets relative to the slot):

```
+0x00 draw node ptr (0 = row not on screen)   +0x04 wchar* label
+0x08 lifetime timer                          +0x0C previous frame's timer
+0x10 current y (element-local)               +0x14 next row in the live list
+0x18 s8 tier (stars), -1 = none              +0x1C newest star's flash zoom
+0x20 pending star's spin phase (rad)         +0x24 pending star's pulse 0..1
```

`obj+0x688` is the head of the live-row list (push-front), `obj+0x56C` the
stacking base = `barnode.y + barnode.h * 0.5` (@0x003B1684, `mulss`
@0x0004C13C) = the boost bar's centre line, **y 466** in the 640x480 space.

### 6.8.3 What opens a row — FUN_0004D310's six probes [C]

`FUN_0004D310(EAX=obj, [esp+4]=dt)` probes six score-object category
records, in this order, each through `FUN_0004D130(EAX=row, EDX=&rec,
EDI=obj, XMM3=dt, [esp+4]=minimum)`:

| row | record | minimum | pushed at |
|---|---|---|---|
| 0 ONCOMING | score+0x374 | **100.0** | 0x0004D32C |
| 1 DRIFT | score+0x390 | 0.0 | 0x0004D351 |
| 2 NEAR MISS | score+0x418 | 0.0 | 0x0004D370 |
| 4 TAILGATING | score+0x598 | 1.0 | 0x0004D391 |
| 5 GRINDING | score+0x5C4 | 1.0 | 0x0004D3B5 |
| 6 RUBBING | score+0x564 | 1.0 | 0x0004D3D9 |

**Slot 3 (AIR) is never probed** — it has a slot and a label and no driver
in the shipped build. (Negative result, but a firm one: FUN_0004D130 has
exactly six `E8` callers, all inside FUN_0004D310, and the row index only
ever reaches it in EAX.)

The record is the 0x1C-byte `B3CatRecord` FUN_00192D20 maintains
(RE_GAMEPLAY §2/§5; `src/burnout3_score_events.h`), read as:

```
+0x00 f32 value        the accumulated metres / chain links
+0x04 f32 clock        last update (near miss: the chain link's clock)
+0x08 f32 prev_value   the closed event's final value
+0x10 u8  active       "event open"  (= B3ScoreEvents' *_active byte)
+0x11 s8  tier         current tier, -1 = none
+0x12 s8  prev_tier    the closed event's tier
+0x13 s8  count        number of tiers (4)
```

FUN_0004D130, verbatim:

```
if (rec.active) {
    if (rec.value < minimum) { if (!row.node) return 0; }   @0x0004D167
    else { lvl = rec.tier; show = 1;
           if (row.tier > lvl) row.tier = -1; }             @0x0004D174
} else {
    if (rec.prev_value >= minimum) lvl = rec.prev_tier;     @0x0004D1E9
    if (!row.node) return 0;
}
if (lvl > -1 && lvl > row.tier) {                           @0x0004D189
    row.tier = lvl; row.flash = 2.0; row.phase = 0; }
row.flash = max(row.flash - 5*dt, 1.0)                      @0x0004D1B3
if (lvl == rec.count-1)  row.pulse = 0                      @0x0004D1D6
else if (show) { row.phase += SPIN*dt ; row.pulse = 1 }     @0x0004D248
else             row.pulse -= 5*dt                          @0x0004D26A
if (!row.node) { row.node = new node; row.y = base - 26;    @0x0004D273
                 push front of obj+0x688 ;
                 FUN_0004B1C0(node, anchor {0,0},
                              ref {4.0, row.y}) }           @0x0004D2E2
if (show) row.timer = 0.85                                  @0x0004D2F5
row.prev_timer = row.timer
return "created a row this frame"
```

`SPIN` is `2.5*pi` rad/s (`0x0054F428`, initialiser 0x002650A0) for every
row **except NEAR MISS**, which spins its pending pip at
`(1 - (score.clock - rec.clock)/5.0) * 5*pi` (`0x0054F400`, init 0x002650C0;
the 5.0 is the VDB's *Near Miss Chain Time* at 0x003F73D0) — i.e. the pip
whirls when a link lands and **slows to a stop as the chain window runs
out**. When any row was created this frame FUN_0004D310 fires an SFX
(`FUN_00141010(0x94566800, 0x79477E7C, …)` @0x0004D47D).

So a row appears while its category is open **and past the minimum**, and
lingers 0.85 s after the event closes, still showing the tier the event
reached (`prev_tier`).

### 6.8.4 The row list walk: life, exit, stacking [C]

Per row, newest first (`clamp` and `target` start at `base-26` and
`base-52` and both drop 26 per row):

```
row.timer -= dt
if (row.timer < 0) { unlink + free the node; reset the row }     @0x0004D4C9
if (row.timer < 0.25) {                                          @0x0004D579
    s  = row.timer * 4                    ; alpha = s
    k  = (1 - s) * -0.05
    node.w = 210 + 2*k*210 ; node.h = 26 + 2*k*26                 (shrinks 10%)
} else alpha = 1, no shrink
y = min(row.y, clamp)                                            @0x0004D70C
y = move y toward target at 300 px/s                             @0x0004D414
node.x = 4 - k*210 ; node.y = y - k*26 ; node.rgba = {1,1,1,alpha}
target -= 26 ; clamp -= 26
```

A new row is born at `base-26` and slides **up** 26 px into the first slot,
pushing older rows further up, one 26 px row at a time; each row fades and
shrinks out over its last 0.25 s. In the 640x480 space that puts the
newest row at **y 414..440** and the next at **388..414**, directly above
the boost bar's 452..480 — which is where the reference frame has them.

### 6.8.5 One row's draw — FUN_0004B4D0 [C]

`FUN_0004B1C0` builds the row's node exactly like the bar's builder:
`node.{w,h} = {210, 26}` (0x003FCBE0/4), `node.xy = ref - anchor*{w,h}`,
`node+0x20 = the row slot`, `node+0x3C = 0x0004B4D0`.

The callback scales everything by `W' = node.w/210`, `H' = node.h/26`
(`0x0054F3F8/C = 1/210, 1/26`, computed by the initialiser FUN_00265030),
then:

1. **The label**, laid out by `FUN_0004B280` over GlobalFont's charmap
   (6.2). Glyph fields are texture-normalised and get multiplied by
   `W' * GlobalFont+0x08 (6.773046) * 26.0` — **0.687888 px per atlas px**
   for a full-size row. The pen starts at `node.x - glyph[0].xoff*scale`
   (@0x0004B34D), so the first glyph's **ink** lands exactly on `node.x`;
   blank records (`u < 0`) emit nothing but still advance.
2. It is drawn **8 times as a shadow** before the fill: four copies at the
   combinations of `A±B` down-right and four at `±B` diagonally, where
   `A = 2.2` (0x003B18B0 → 0x0054F520/4) and `B = 1.2` (0x003B1768 →
   0x0054F528/C). The first four carry `{0,0,0, 0.25 * node.a²}`
   (0x0054F510..C, filled by the HUD init FUN_0004DA90). *The second
   four take their colour from `[esp+0x50]`, whose alpha slot the function
   never writes* — `{0,0,0, uninitialised}` `[?]`; all three style blocks
   the init fills carry the same `{0,0,0,0.25}`, so 0.25 is the sane read.
3. The fill pass is per-vertex `node.rgba * {0x0054FA10, 0x0054FA50}` — a
   two-colour vertical gradient whose float4s are BSS with no decodable
   initialiser, the same `[?]` as 0x0054FA00 (6.7.8).
4. **The star pips**, from `hud_boost_stars` (handle 0x00460940, bound by
   FUN_0004DD00, 6.5) — a 64x32 sheet, **solid star left / outline star
   right**:

```
S    = 23.4                     0x0054F3BC (init 0x002650E0)  pip size
half = S * 0.5   = 11.7         0x003B1684
adv  = S - 6.0   = 17.4         0x003B1824  (the pips overlap 6 px)
gap  = 12.0                     0x003B178C
cx   = <label's last vertex x> + (half + gap) * W'
cy   = node.y + half * H'
solid pip i (i < row.tier): axis-aligned quad, centre (cx + adv*i, cy),
     half-extent (half*W', half*H'), u 0.0078125..0.4921875,
     v 0.015625..0.984375 ; the LAST one is scaled by row.flash (2 -> 1)
pending pip (only if row.pulse > 0): the RIGHT frame,
     u 0.5078125..0.9921875, centre (cx + adv*row.tier, cy), corners at
     C + 11.7*{( sin p, cos p), ( cos p,-sin p), (-cos p, sin p),
               (-sin p,-cos p)}  with p = row.phase   -- it SPINS
     colour = node.rgba with alpha *= row.pulse
```

So the row shows **`tier` solid pips plus one spinning outline pip for the
tier being worked on**, and the outline pip disappears once the category
tops out (`tier == count-1`). `tier == 0` draws no solid pip at all — the
`GOOD` tier is "one spinning outline star", which is what the reference
frame's `DRIFT ★` is.

### 6.8.6 What is [C] vs open

**[C]** — everything above: the element ownership and its enable byte, the
seven slots and their Globalus labels, the six probe records and minima,
the AIR slot's absence of a driver, the whole FUN_0004D130 state machine
(thresholds, tier latch, flash, spin rates, pulse), the row lifetime/fade/
shrink/slide/stacking numbers, the row box and its screen placement, the
label scale and pen rule, the 8 shadow offsets and the first four's colour,
and every star-pip constant and UV window.

**[?]** — the label's fill gradient (0x0054FA10/0x0054FA50) and the second
shadow group's alpha, both BSS floats with no decodable writer; the harness
uses white→light-steel and 0.25 and marks them `[S-ref]`.

**Harness note**: `src/burnout3_font.h` stores `' '` advance = 8 atlas px;
the retail record (0x003C8758, +0x18 = 0.02734375) is **7**. The ticker
uses the recovered value (`B3HUD_TICK_SPACE_ADV`) so its label metrics
reproduce the game's last-vertex x to 0.01 px — the fix for the generated
table belongs in `tools/extract_font.py`.

### 6.8.7 Reproduce

```
python3 tools/emulate_hud_ticker.py          # FUN_0004D310 + FUN_0004B4D0 for real
python3 tools/emulate_hud_ticker.py --full   # every emitted vertex
python3 tools/validate_hud.py                # 308/308 (was 175/175)
```

`emulate_hud_ticker.py` runs the update and the draw callback under
Unicorn with the retail GlobalFont object relocated in place, and reports
the rows created, their node boxes, and every vertex/UV/colour the row
emits. `validate_hud.py` gained `[11]` (element ownership, the six probe
sites decoded straight out of `.text`, the seven label offsets checked
against `Data/Globalus.bin`, the star sheet) and `[12]` (a differential:
the row box, stacking, pip centres, sizes, UV windows, spin angle and the
C's own label metrics, all against the executed code).

---

## 6.9 The EA TRAX NOW-PLAYING BANNER — added 2026-08-12 (music session)

The white/blue "song title over band name" panel that comes up at the
bottom of the screen when a race soundtrack starts. Unlike §§6.6–6.8 this
element was **not** recovered: no element object, no anchor slot, no
constructor. It is marked **GLUE** in `src/burnout3_hud.h` and lives
deliberately *outside* the `B3HUD-TABLE-BEGIN/END` block, so
`tools/validate_hud.py` does not go looking for its numbers in the ELF.

### 6.9.1 What is the game's own [C]

The banner is glue only in its *layout*. Everything it is made of is
recovered material, which is the reason it reads as a sibling of the
POS / LAP plates rather than as an overlay bolted on top:

| part | source |
|---|---|
| the text | the 44-entry song table at `0x003EC458` → `Data/Globalus.bin` (docs/RE_MUSIC.md §1) |
| the plate | `big_curve` from `Data/Global.txd` — the same asset `elem_position` / `elem_lap` draw, in the POS orientation (u 0..1) so its swoosh runs the length of the banner |
| the glyphs | the XBE-recovered GlobalFont (§6.2), through the same `draw_text()` pen + 8-offset shadow every other HUD label uses |
| the badge | `EATrax` from `Global.txd` — a 256×128 sheet whose **right** 128×128 half is the EA TRAX mark (the left half is the EA GAMES roundel). It is stored rotated a quarter turn, so the quad's UVs are rotated to stand it up |
| the blue | `big_curve`'s own swoosh colour, sampled off the texture: RGB (99,142,214) at the cap, deepened to (60,105,190) at the baseline |

White primary line = the song title, HUD-blue secondary line = the band —
the same white-label / coloured-value pairing `POS.` and `LAP` use.

### 6.9.2 The layout — GLUE / [S-ref]

The reference captures in `REFERENCE IMAGES/` contain no frame with the
banner up, so there was nothing to calibrate against; the box was placed
against the *recovered* geometry of its neighbours instead:

```
box   x 176 .. 464,  y 388 .. 438      (640x480 virtual screen)
badge 36 x 36 at x+8, vertically centred
title  x+52, y+3,  scale 0.60, shrink-to-fit into a 228 px column
artist x+52, y+26, scale 0.48, same column
life 8.0 s: 0.35 s slide-in from -70 px, hold, 0.60 s slide-out
```

The three constraints that fixed it, all against numbers recovered
earlier in this document:

* it must clear the boost bar's box, `y 452..480` [C, §6.6.3] → bottom 438;
* it must clear the ticker's first row slot, top `y 440` [C, §6.8.4];
* it must clear the speed cluster, `x 469..626` [S-ref, §6.6] → right 464.

`tools/validate_music.py` §6 asserts all three by driving the real
`b3_hud_music_box()` over the whole 0..10 s range, so the banner cannot
be nudged into a collision without a validator failure.

### 6.9.3 Wiring

The element is driven by additive `B3HudState.music`
(`{const char *artist; const char *title; float elapsed;}`) — the same
pattern §6.8 used for the ticker. It holds **no state of its own**: the
whole animation is a pure function of `elapsed`, so when the music module
rolls over to the next song (which resets `elapsed` to 0) the banner
re-runs its slide-in/hold/slide-out by itself. `artist == NULL` is inert,
so the field is zero-safe and the pre-existing `b3_hud_draw()` shim keeps
working unchanged.

### 6.9.4 Open

* The retail banner's real element object, anchor slot, box, colours and
  on-screen life are all **[?]**. If a capture with the banner up ever
  turns up, everything in 6.9.2 should be re-derived from it.
* Whether retail draws the *album* as a third line (the song table
  carries one) is **[?]**; the port does not.

---

## 6.10 The CORNER PLATES and the OPPONENT TAGS — CRACKED (2026-08-12, HUD-fidelity session)

Three user reports against `Downloads/xemu-2026-08-12-16-23-19.png`:
**(A)** the POS/LAP plates render flipped with inverted alpha, **(B)** the
HUD does not alpha-blend like retail, **(C)** retail tags rivals with a
position ordinal / a triangle and the port has no such element. All three
are closed below. All addresses are `build/burnout3.elf` VAs.

### 6.10.1 (A) Nothing was flipped, and nothing was alpha-inverted [C]

`tools/extract_txd.py` obeys the V ORIGIN RULE (`tools/extract_textures.py`
docstring): PNG row 0 = surface row 0 = v 0, for every format, alpha
untouched. Re-verified two ways this session and now machine-checked by
the extractor itself (`selfcheck_orientation`, failing the run if it ever
regresses):

* `B3Logo` decodes to a right-way-up, un-mirrored "BURNOUT 3 TAKEDOWN".
* `hud_element01` decodes with alpha row 0 = 0, rows 1–4 = 204 (blue rim),
  rows 5–26 = **153** (the 0.6 dark fill), rows 27–30 = 255, row 31 = 0,
  and its left ink edge marching from x 35 at the top to x 1 at the
  bottom. A vertical flip reverses the march; an inverted alpha turns 153
  into 102 and the transparent border into an opaque one.

The bug was in the consumer. `src/burnout3_hud.c` drew **`big_curve`** —
a *Frontend* menu corner asset whose 0.35-alpha field sits **outside** its
swoosh — where the in-race code draws **`hud_element01`**, whose 0.6-alpha
field sits **inside** its rim. Same pixels, opposite reading: a plate that
looks inside-out and, mirrored by `u 1..0` for the LAP corner, flipped.

### 6.10.2 FUN_00048430 is not a generic sprite builder — it is a 3-slice plate [C]

6.6.7 recorded the POS / LAP / speed boxes as "[S-ref], only their anchor
slots recovered, sizes come from the bound texture at runtime". That is
now closed. The draw callback all three install is a **fixed three-slice
stretcher that always binds `hud_element01`** (handle `0x0046093C`, read
at `0x00048588`):

```
state[0] = s0  (left cap fraction, node+0x20)
state[1] = s1  (right cap fraction, node+0x24)

left cap   x 0 .. s0*w          u 0        .. 0.59375   (skipped if s0 == 0)
middle     x s0*w..(1-s1)*w     u 0.59375  .. 0.703125  (stretched)
right cap  x (1-s1)*w .. w      u 0.703125 .. 1.0       (skipped if s1 == 0)
v          0.03125 .. 0.9375                            (all three)
```

The four UVs are BSS floats with plain one-instruction initialisers:
`0x0054F394 = 0.03125` (`0x002646F0`), `0x0054F374 = 0.9375` (`0x00264710`),
`0x0054F39C = 0.59375` (`0x00264730`), `0x0054F36C = 0.703125` (`0x00264750`);
`0x0054F380`/`0x0054F37C` are copies made by `0x00264770`/`0x00264790`.
On the 64x32 sheet that is texels x 0–38 / 38–45 / 45–64 and rows 1–30 —
i.e. exactly the ink between the two transparent border rows, which is an
independent confirmation of the decoded row order (6.10.1).

### 6.10.3 The three elements [C]

| element | init | object | slot | box (anchor-relative) | caps |
|---|---|---|---|---|---|
| **POS** | FUN_00053ED0 | 0x003FD4A0 | **1 top-left** | x −30 .. +95, y 0.5 .. 27.5 | s0 = 0, s1 = 19/29·H/W |
| **LAP** | FUN_00051650 | 0x003FD4F8 | **7 top-right** | x −95 .. +30, same y | s0 = 38/29·H/W, s1 = 0 |
| **SPEED** | FUN_00059850 | 0x003FDD48 | 5 bottom-right | x −168 .. 0, y −27 .. 0 | both |

Slot 1 is a **new** row for 6.6.1's table — the POS element is built at
`0x00052DAE` (`mov ebx,0x3FD4A0 ; mov ecx,1 ; call 0x53ED0`), which the
old scan missed. Constants: W = 125.0 (`0x003B03F4`, 148.0 `0x00389094`
in split screen), H = 27.0 (`0x003897A8`; the speedo's own H is the BSS
`0x0054FCCC`, initialiser `0x00268100`, which loads the same 27.0), speed
W = 168.0 (`0x00389A24`, with 1/168 at `0x003B209C`), POS ref.x = −30.0
(`0x003A60AC`), LAP ref.x = +30.0 (`0x003A7964`), ref.y = 14.0 − 13.5
(`0x003B1790`/`0x003B1FE0`).

The cap fraction is `texels / ((v1−v0)·32) · H/W` — and `(v1−v0)·32 = 29`
is the number of texture rows the quad covers, so **a cap is its own texel
width scaled to the box height**: the end pieces keep `hud_element01`'s
aspect however wide the plate is stretched. `19` (`0x003A5594`) is the
right cap, `38` (`0x00399654`) the left.

**The LAP plate is not a mirrored texture.** It is the same art with the
caps swapped, exactly as FUN_00051650 mirrors FUN_00053ED0.

Labels, straight out of `Data/Globalus.bin` as the elements load them:
`POS` = entry 2002 (`imm32@0x00053FF6`) — **not** `POS.` (entry 670, which
is what the port was drawing), `LAP` = 2003 (`imm32@0x00051783`), `mph` =
1987 / `kph` = 1986 (`imm32@0x00059922`, selected on `[0x0045B9BC]`).

### 6.10.4 (B) The blend audit [C blend, S-ref colours]

An `E8` scan of FUN_00048430's whole body (`0x00048430..0x000485E1`) finds
only `FUN_001C69C0` (flush), `D3DDevice_SetTexture` and `FUN_001C7430`
(rect batch) — **no `FUN_001C82E0` / `FUN_001C8470` preset switch**. So the
plates, like the callout signs (6.7.7), draw under the ambient 2D state:
`SRC_ALPHA / ONE_MINUS_SRC_ALPHA`, `BLENDOP ADD`, **`COLORWRITEENABLE
0x010101`** (RGB only — the alpha channel is never written), address
`WRAP/WRAP`.

| element | blend | vertex colour | was |
|---|---|---|---|
| POS / LAP / speed plate | ambient alpha | `[0x0054FA00]`, solved to `{1,1,1,1}` | `big_curve` at a hand-picked 0.95 |
| all text runs | ambient alpha | 8 shadow passes `{0,0,0,0.25·a²}` + fill | 8 unit-offset opaque navy copies |
| ticker rows | ambient alpha | `[0x0054FA10]/[0x0054FA50]` | white → light steel |
| boost bar plate | ambient alpha, WRAP | `#FFFFFFFF` | unchanged, already [C] 6.7 |
| boost fire strips | SRC_ALPHA / ONE | per-section float4s | unchanged, already [C] 6.7 |
| opponent tags | ambient alpha | see 6.10.5 | did not exist |

Two real bugs fell out of the audit, both now fixed in
`src/burnout3_hud.c`:

* the 2D pass never masked the alpha channel, so every translucent HUD
  quad also stamped its own alpha into the framebuffer. `state_begin()`
  now sets `glColorMask(1,1,1,0)` for the whole pass;
* `draw_text()` drew its outline as eight **unit-offset, fully opaque**
  dark-navy copies. Retail draws **eight black copies at 0.25 alpha** —
  four at the combinations of `A±B` down-right and four at `±B`
  diagonally, the scheme 6.8.5 recovered from FUN_0004B4D0. `A` comes
  from the style block: **2.2 / 3.0 / 4.0** (`0x0054F520` / `0x0054F540` /
  `0x0054F560`, from `0x003B18B0` / `0x003B1698` / `0x003B1690`), `B` =
  1.2 in all three, colour `{0,0,0,0.25}` in all three — the three blocks
  `FUN_0004DA90` fills at `0x0004DBE2/E9/F0`.

**Colours solved off the reference frame** (the `[?]` BSS float4s of 6.7.8
have no writer in the image; the retail capture is the only source, so
these stay `[S-ref]` — but each is now *solved*, not guessed):

| what | value | how |
|---|---|---|
| plate vertex colour `0x0054FA00` | `{1,1,1,1}` | the 0.6-alpha fill over sky (95,115,164) composites to (41,50,70) = 0.4·bg to within a unit — so the fill draws at exactly its own texture alpha |
| HUD gold (numerals) | `#F7DB24` → `#F4AF14` | POS and LAP read *identically at identical screen y*, so it is the run's own vertical gradient |
| "mph" | `#F3AA12` → `#F18F07` | a deeper orange than the numerals |
| "POS"/"LAP" | flat `#91BBFF` | no gradient over the whole 14-row ink |
| ticker label `0x0054FA10`/`0x0054FA50` | flat `#F9F883` | the ONCOMING row is a **pale yellow**, not white, and flat over its ink — and the opponent tag, a second consumer of `0x0054FA10`, measures the same `#F8F884` |
| ticker star pip | `#FDD101` | a saturated gold, distinct from the label |

### 6.10.5 (C) The OPPONENT TAGS [C]

Not a 2D HUD element of 6.6.1 at all — it lives in the race module and is
projected per car.

`FUN_0018EE10`-family: depth-sorts the cars, then for each one that is
neither the player (`@0x0018EC7C`) nor hidden (`byte[car+0x18FA]`,
`@0x0018EC8D`):

```
place  = word[car+0x10D0] - 1                            @0x0018ED04
world  = car[+0x204][+0x30] , y += car[+0xCC0][+0x40][+0xE84]   @0x0018ED5B
string = globalus[0x1F24/4 + place]                      @0x0018EDBB
FUN_0018F060(state, string, slot, car, player, 0,0,0, 0.5)     @0x0018EDCA
```

The ordinal run at byte offset **`0x1F24`** is entries 1993..1998 =
`"1st".."6th"` and is exactly **six long** (1999/2000 are `":"` and
`" x "`), matching the six-car grid. (The `1st..8th` run at entry 587 is a
different table, used elsewhere.)

**Pass state** (`@0x0018ED7C..`): `FUN_001C72F0` (2D begin),
`FUN_001C82E0` with `ESI = 0` (`xor esi,esi @0x0018ED81`) = **blend preset
0 = plain alpha**, `FUN_001C83D0(0)` = the default texture,
`FUN_001C8690(1)`. Both the ordinal and the triangle therefore draw under
`SRC_ALPHA / ONE_MINUS_SRC_ALPHA`. [C]

`FUN_0018F060`, verbatim (z = camera-space depth, W/H = viewport px):

```
cull      z < [car+0x84] (near)  or  z >= [car+0x88] (far)   @0x0018F1F0
size      = clamp( ((z-1)*1/74 + 1)/z * W * 0.75 * s , <= 320 )   @0x0018F28E
sx        = px/z * W                                          @0x0018F2BF
sy        = py/z * H - size_y/3                               @0x0018F2F7
cull      sx < -size/2, sy < -size_y/2, sx > W + size/2,
          sy > H + size_y/2                                   @0x0018F338..
alpha     = 0.9                                               @0x0018F7B8
          z > 175 : -= (z-175)*0.04                           @0x0018F66E
          z < 2.0 : hidden ; z < 2.7 : *= (z-2)*1.428571      @0x0018F694
FAR   z >= 35.0 :  a flat 3-vertex triangle, apex DOWN,
                   (sx-w, sy-h) (sx+w, sy-h) (sx, sy+h)
                   w = size*0.3, h = size_y*0.5               @0x0018F6D7
                   emitted by FUN_001C7C90 (3 verts, one colour) @0x0018F757
NEAR  z <  35.0 :  the ordinal, size = max(size,20),
                   size_y = max(size_y,7), em = size_y*2,
                   box top-left (sx, sy - size_y)             @0x0018F761..
```

so **the switch is a plain camera-depth threshold at 35 units**
(`0x003B175C`), with a per-slot hysteresis latch in retail
(`state[slot]` + the `[ebx+0x18]` gate `@0x0018F218`) that only matters
for a car sitting exactly on it. The triangle is **untextured** — no
`Arrow`, no sprite, three vertices and a colour.

Colour comes from a per-game-mode tint table at **`0x00416830`**
(`(0.49,0.25,0,1)` the current rival via `FUN_0019A030(player+0x11F4)`,
`(0.5,0,0,1)` a car `FUN_001941C0` says you are level with,
`(0.5,0.5,0.5,1)` everyone else) multiplied by the `[?]` 2D text colour
`0x0054FA10`. Since that global has no writer in the image, the tag's
on-screen colour is taken from the reference: `#F8F884`, the same pale
yellow the ticker labels measure. `[S-ref]`

The `size`/`size_y` ratio is retail's `[esp+0x40]` term, built from camera
fields that are runtime data. Pinned off the reference instead: the far
triangles measure 7 × 5 px, and `size = 10.0` at the 35 m threshold gives
6.0 × 4.3 with a ratio of **0.428571** — the measured shape to within the
capture's antialiasing. With `size_y` floored at 7 the ordinal never draws
smaller than a **14 px cap**, which is exactly what `"4th"` measures in
the reference (14 rows / 15 virtual px). `[S-ref]`

### 6.10.6 The anchor viewport — the one number that is [S-ref]

6.6.2's rule is `anchor = vp.min + SLOT_TABLE[slot] * vp.size`, and `vp`
is runtime data (`elem[+4] + player*0x70`, +0x48..+0x54). The reference
frame gives it: the game renders **640x448 letterboxed into rows 16..463**,
so `screen_y = 16 + virt_y * 448/480`, and four independently recovered
element boxes then all land on the same inset:

| element | recovered edge | measured | ⇒ anchor |
|---|---|---|---|
| POS plate right | anchor.x + 95 | 123 | 28 |
| LAP plate left | anchor.x − 95 | 516 | 29 |
| SPEED plate bottom | anchor.y | 443 | 458 = 480 − 22 |
| BOOST bar left / bottom | anchor.x / anchor.y | 30 / ~443 | 28 / 458 |

i.e. a symmetric **4.5%-per-side title-safe frame** (0.045·640 = 28.8,
0.045·480 = 21.6) — the standard Xbox safe area. One number,
`B3HUD_SAFE_FRAC`, applied through the recovered rule so every in-race
element moves together; set it to 0 for the old flush-to-the-edge
behaviour. This is why the retail boost bar's tread starts at x = 30 and
not at x = 0 as 6.6.3's "flush to the bottom-left corner" reads.

### 6.10.7 What is still open

* `0x0054FA00` / `0x0054FA10` / `0x0054FA20` / `0x0054FA30` / `0x0054FA50`
  still have **no writer** in the image (a store scan over the whole
  `.text` finds none — they are filled through a pointer). Their values
  are now *solved* from the reference rather than guessed, but the writer
  is `[?]`.
* the inner text placement inside the three plates (`POS_NUM_DX` and
  friends in `burnout3_hud.c`) is measured off the reference; the retail
  text nodes were not decoded.
* the speedo's ~5.5 px of extra digit tracking (26 px measured per digit
  against 19 px of font advance) is `[S-ref]`.
* the opponent tag's per-slot hysteresis latch and its per-game-mode
  colour switch are recovered but not implemented — the harness entry
  point is stateless and single-mode.

### 6.10.8 Reproduce

```
python3 tools/extract_txd.py    # 409/409 + the V-ORIGIN/ALPHA self-check
python3 tools/validate_hud.py   # 425/425 (was 310/310)
```
`validate_hud.py` gained `[13]` (the plate: the handle it binds, the name
FUN_0004DD00 binds to it, all three elements' draw callbacks, the absence
of any preset switch inside the callback, the four slice UVs, the box
sizes and cap fractions, the Globalus labels, and that the C dropped
`big_curve`) and `[14]` (the opponent tag: the ordinal table offset and
the six strings behind it, the `xor esi,esi` that pins the blend preset,
the three-vertex triangle batch, and every one of the fourteen constants
re-read at its own VA).

The HUD's public API gained one entry point,
`b3_hud_opponent_tag(screen_x, screen_y, distance, place, visible)` —
see `src/burnout3_hud.h` for the integration note on what the caller must
project and which depth it must pass.
