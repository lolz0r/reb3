#!/usr/bin/env python3
"""
Extract Burnout 3 Xbox track geometry from static.dat to Wavefront OBJ.

Format credit: the Burnout Modding community, via EdnessP's Noesis plugin
`fmt_Burnout3LRD.py` (burnout.wiki, discord.gg/8zxbb4x). This is an independent
Python reimplementation of the Xbox track path so the geometry can be loaded
without Noesis.

Layout (all little-endian):

  header
    +0x00  u32   version (0x27 here; <=0x25 is the demo/prealpha layout)
    +0x04  u32   file size
    +0x08  i32   material table offset       (see "Material record")
    +0x0C  u16   material count
    +0x0E  u16   animated-material count     (see "Animated materials")
    +0x10  i32   animated-material index table (u16 material ids)
    +0x14  u8    static pass split A         (see "Static group draw order")
    +0x15  u8    static pass split B
    +0x1C  u16[4] group counts: backdrop, chevron, water, reflection
    +0x24  i32[4] group table offsets (absolute)
    +0x34  u16   instanced-prop count  / +0x38 i32 table  (not extracted, [S])
    +0x54  u16   streamed unit count
    +0x58  i32   streamed unit table

  group entry (8 bytes, at groupTable + i*8)
    +0x00  u16   submodel count
    +0x02  u16   pad
    +0x04  i32   model offset, RELATIVE to this entry

  submodel (stride 0x60: 0x40 bytes of OBB floats, then 0x20 of info)
    +0x00  f32[16] oriented bounding box: 3 edge vectors then the origin
                   corner (rows 0/1/2 = extents along X/Y/Z, row 3 = origin).
                   Vertices are already in WORLD space -- this is a bound, not
                   a placement transform (checked against the raw vertex bbox
                   of all 7 chevron and 14 backdrop groups).
    +0x40  u32   must be 1
    +0x44  i32   vertex offset, relative to +0x40
    +0x4C  i32   submesh offset, relative to +0x40
    +0x50  u32   submesh count
    +0x54  i32   material index array (u16 per submesh), relative to the group
                 entry -- static groups only; streamed units use the PVS block

  submesh entry (stride 0x90)
    +0x80  u32   triangle format: 6 = strip, 2 = line
    +0x84  u32   index count
    +0x88  i32   index offset, relative to the submesh entry
    +0x8C  i8    NEXT submesh index in this material slot's chain, <0 = end.
                 [C] FUN_001AD510 @0x001AD691 / FUN_001AD5E0 @0x001AD691:
                 the per-slot draw loop reads the head index out of the PVS
                 chunk and then walks `movsx eax, byte [submesh+0x8C]` until
                 it goes negative. In C1_V1 every chain has length 1 (all
                 1436 streamed submeshes carry -1), but the walk is
                 implemented here because the format allows longer chains.

  vertex (stride 0x1C)                                                     [C]
    +0x00  f32[3] position
    +0x0C  u32    D3DVSDT_NORMPACKED3 normal: x = bits 0..10 signed / 1023,
                  y = bits 11..21 signed / 1023, z = bits 22..31 signed / 511
    +0x10  u8[4]  D3DCOLOR diffuse, BGRA. RGB is HALF-RANGE (max 128 = white,
                  the pixel shader doubles it); A is the specular gate, 0 or 255
    +0x14  f32[2] uv   -- v is a D3D texture coordinate: v=0 is texel ROW 0
                          of the surface, i.e. the TOP of the picture. The OBJ
                          `vt` is therefore written UNCHANGED; see below.

  The whole vertex layout is pinned by the D3D vertex DECLARATION the game
  hands to CreateVertexShader for the world shaders, at 0x003875A4 (classes
  0/6/10, no normal) and 0x0038758C (classes 1/2/7, with normal):

      0x003875A4: 20000000 40320000 50010000 40400003 40220009 FFFFFFFF
                  stream0  v0=FLOAT3 skip1dw  v3=D3DCOLOR v9=FLOAT2  end
      0x0038758C: 20000000 40320000 40160002 40400003 40220009 FFFFFFFF
                  stream0  v0=FLOAT3 v2=NORMPACKED3 v3=D3DCOLOR v9=FLOAT2 end

  and two more that read NEITHER the normal nor the colour:

      0x003875D4: 20000000 40320000 40160002 40220009 FFFFFFFF   class 9
      0x003875E8: 20000000 40320000 40220009 FFFFFFFF            class 8

  (D3DVSD_STREAM = 0x20000000|n, D3DVSD_REG = 0x40000000|type<<16|reg,
  D3DVSDT_FLOAT3 = 0x32, D3DVSDT_NORMPACKED3 = 0x16, D3DVSDT_D3DCOLOR = 0x40,
  D3DVSDT_FLOAT2 = 0x22, D3DVSD_END = 0xFFFFFFFF.)  The class-0 declaration
  SKIPS the four bytes at +0x0C -- only the class-1/2/7 shaders read the
  normal.  Decoding the packed normal with the 11/11/10 split above gives
  |n| = 1.0000 +/- 0.0004 over every vertex of C3_V1 unit 12 and +Y for the
  road surfaces; the 10/11/11 split gives |n| = 0.42 +/- 0.47, so the split is
  not a guess.                                                             [S]

  This tool emits all four channels: `v x y z r g b` (the raw half-range
  colour, NOT pre-doubled), `vn nx ny nz`, and `vt u v a` where the third
  texture-coordinate component carries the vertex ALPHA.  Readers that want
  only geometry can ignore the extra fields -- `v x y z ...` and `vt u v ...`
  are both standard-tolerant.

Vertex data for a model runs from vtxOffset up to the first submesh's index
data, so the vertex count is derived from that span.

The V origin: `vt` is emitted verbatim, NOT 1-v                          [C]
------------------------------------------------------------------------
tools/extract_textures.py's docstring carries the full recovery (the texture
record IS a prebaked Xbox D3DPixelContainer handed to D3DDevice_SetTexture,
NV2A TX_FORMAT has no origin bit, all 9,224 textures in all 36 shipped tracks
are DXT1/DXT5 with zero LIN_* surfaces). The consequence for this tool:

  game v  ==  D3D v  ==  GL t  ==  PNG row v*height, row 0 = top

because tools/extract_textures.py writes each PNG in surface-row order and
the harness uploads it row-0-first with glTexImage2D, which is exactly where
GL puts t=0. So no conversion is needed in either direction and this tool
emits `vt u v`.

This file used to write `vt u (1-v)` -- the reflex D3D->GL flip -- and every
sign in the world rendered UPSIDE DOWN in the harness (build/dump_sl2.png:
US_C3_V1's "GIFT SHOP" / "SOFT DRINKS" mirrored vertically while
build/textures/GL_SHOP1.png reads correctly on disk). It was not a Silver
Lake problem: rendering AS_C1_V1's bk_newfwysigns1 and bk_warnsigna through
the old OBJ shows "Main Centre"/"Dockside"/"SLOW DOWN" flipped too -- Golden
City had simply never been checked against legible signage.

The data agrees with the code, per track and without reference to it: over
near-vertical faces (|n.y| <= 0.3), v DECREASES as world height increases,
i.e. v=0 sits at the top of a wall. Area-weighted share of "v=0 at the top":
US_C3_V1 95.7%, AS_C1_V1 71.9%, US_C1_V1 88.4%, EU_C3_V1 81.3%, AS_C2_V1
71.8% (the remainder is tiling clutter and rotated atlas panels, where the
sign of dv/dY carries no orientation meaning).                            [S]

Reading the verification shots: the harness is ALSO mirrored left-right, and
that is a different bug, not this one. build/texorigin_us_c3_v1.png and
build/texorigin_as_c1_v1.png show every sign standing the right way up after
this fix but still reading right-to-left; mirroring the crop makes it read
perfectly. build/texorigin_world_mirror.png pins the cause: the harness start
line flipped horizontally matches the retail xemu reference frame
(REFERENCE IMAGES/xemu-2026-08-12-13-52-39.png) building for building --
billboard-on-a-pole LEFT, brick shop row RIGHT -- while the unflipped harness
frame has them swapped. So the whole WORLD is mirrored, not the texture: the
uniform Z-negation in src/burnout3_trackmesh.c (`p[2] = -p[2]`, and its
companions in burnout3_full.c for paths/collision/cars) reflects the world,
and a reflection of both the world and the camera is only invisible when the
world data was left-handed to begin with. The data behaves as if it is not,
so the negation introduces the mirror rather than removing it. That belongs
to the renderer, not to this extractor -- emitting -z here would only break
the mesh's alignment with the arrays burnout3_full.c negates itself.  [C for
the A/B; the LH/RH call itself is [S]]

Material record (stride 0x28, table ptr at header +0x08, u16 count at +0x0C)
------------------------------------------------------------------------
Recovered from the two material-apply functions: FUN_0003A3C0 (called by the
static-group draws) and FUN_000393C0 (called by the streamed-unit draws).

    +0x00  u32   shader class; switch at 0x0003A400 (static, classes 3..10)
                 and 0x000393D4 (streamed, classes 1..10). Class 2 is the
                 mirror-only material -- FUN_001AD350 SKIPS submeshes whose
                 material class == 2 (test at 0x001AD40D). That skip is
                 applied to the STATIC groups only: C1_V1's 15 class-2
                 submeshes (Chgo_Glass_Reflec) all live in streamed units and
                 the streamed path has no such test.  The full per-class draw
                 is recovered below ("Shader classes").                  [C]
    +0x04  f32   SPECULAR STRENGTH. Classes 1/7 scale the scene light colour
                 DAT_0060E0A0..AC by it and hand the product to the pixel
                 shader as combiner constant C0 (0x000394D7..0x00039518 ->
                 FUN_0034E9A0, which packs the float4 to a D3DCOLOR and
                 queues NV097_SET_COMBINER_FACTOR0). 0.2..1.1 on the class-1
                 materials, 0.0 on almost every class-0 one.             [C]
    +0x08  f32   SPECULAR POWER. Classes 1/7 push (1, 0, 0, this) into vertex
                 shader constant 0x62 (0x00039567 -> FUN_0034F840) and the
                 class-1 vertex program feeds it to the NV2A LIT instruction
                 as the exponent (see "Shader classes"). Ranges 2.28..65 on
                 class 1; hard 1.0 on class 0/6.                         [C]
    +0x0C  ptr   -> ptr -> texture record (name at rec+0x48 / +0x44)
    +0x10  u8    animation frame index into the texture pointer array;
                 zeroed at load (FUN_0019AE10 case 0x17). Advanced by
                 FUN_0019B1E0 for the materials listed at header +0x10.  [C]
    +0x11  u8    frame count of that animation                           [C]
    +0x12  u8    current glyph/frame for the string-driven signs         [S]
    +0x13  i8    frame step, +1 / -1 (ping-pong at the ends)             [C]
    +0x14  f32   animation period / scroll rate                          [C]
    +0x18  f32   accumulator limit                                       [S]
    +0x1C  f32   UV-scroll phase, zeroed at load; consumed as
                 `1.0 - frac(phase)` into vertex-shader constant 0x63
                 (0x0003A6C8 static / 0x00039CE0 streamed). A pure scroll
                 OFFSET -- it cannot mirror a texture.                   [C]
    +0x20  f32   alpha/fade scalar pushed into every shader path         [C]
    +0x24  u32   FLAGS -- see the table below.

FLAG BITS at +0x24 -- the complete set tested by the two apply functions
------------------------------------------------------------------------
Deferred render states are shadowed at 0x0075D4A0 + id*4 and queued for the
flusher by `shadow_dirty[n++] = id` at 0x0075DE20 (RE_FRONTEND 6.7.1).

THE ALPHA BITS WERE THE WRONG WAY ROUND HERE UNTIL 2026-08-12.  Render state
0x3B is D3DRS_ALPHABLENDENABLE (59) and 0x3C is D3DRS_ALPHATESTENABLE (60), so
bit 0x001 means BLEND and bit 0x010 means TEST.  The proof is the coherent
triple the world setup FUN_00038D10 writes around them: RS 0x3A := 0x204
(@0x0003901B), RS 0x3C := 0 (@0x00038FCA) and RS 0x3D := 0x40 (@0x00038FEE).
0x204 is D3DCMP_GREATER on the 0x200 base that RS 57 = ZFUNC already
established (it only ever receives 0x201/0x203/0x207 = LESS/LESSEQUAL/ALWAYS),
0x40 = 64 is an alpha reference, and "ALPHAFUNC GREATER / ALPHATESTENABLE off /
ALPHAREF 64" is the only reading in which all three are meaningful -- 58, 59,
60, 61 = ALPHAFUNC, ALPHABLENDENABLE, ALPHATESTENABLE, ALPHAREF, the same order
the already-pinned 57 ZFUNC, 62 SRCBLEND, 63 DESTBLEND, 64 ZWRITEENABLE and
67 COLORWRITEENABLE run in.  The data agrees material by material: US_C3_V1
gives bit 0x001 to Tree_shadow1, GL_Pine_Treeshadow, GL_Tree_Shadowbirch,
GL_rock_shadow, GL_bridgeshadow, GL_Road_Decals, Chgo_RdDecals, Arrows and
Barrier -- soft-edged alpha ramps that must blend -- and bit 0x010 to
GL_Fence, GL_Wfence, GL_armcoT, GL_armco_RW, GL_Railings, GL_treeflat,
GL_treeline1c, GL_OverheadSigns, GL_road_signs and the foliage cards, which are
hard cut-outs.                                                            [C]

So the cut-out threshold is GREATER 64/255, not the 0.5 a renderer reaches for
by reflex, and neither material apply ever changes ALPHAFUNC or ALPHAREF.

 bit     where tested                     effect                          conf
 0x0001  0x0003A5E7 (static)              RS 0x3B := 1, else 0             [C]
         0x00039B21 (streamed)            RS 0x3B (D3DRS_ALPHABLENDENABLE)
                                          := 1 and RS 0x43
                                          (COLORWRITEENABLE) := 0x00010101
                                          (RGB, no alpha); clear => RS 0x3B
                                          := 0 and RS 0x43 := 0x01010101.
                                          The blend factors are global:
                                          RS 62/63/74 SRCBLEND/DESTBLEND/
                                          BLENDOP := 0x302/0x303/0x8006 at
                                          0x00038B06/0x00038B2F/0x00038B58,
                                          and the NV2A blend tokens ARE the
                                          GL enums -- GL_SRC_ALPHA,
                                          GL_ONE_MINUS_SRC_ALPHA,
                                          GL_FUNC_ADD.                     [C]
 0x0002  0x0003A63E / 0x00039C52          (flags & 2) << 1 -> 0x0075D7F0,
                                          queued under deferred token 0x0B
                                          (a texture-stage slot, not an RS
                                          shadow). 70/181 materials.       [C
                                          for the write, purpose [?])
 0x0004  not tested by either apply fn    -- (5 materials carry it)        [?]
 0x0008  0x0003976F, 0x0003983E (streamed,
         tested as `test al,0x18`)        selects an alternate texture-state
                                          block for shader classes 8 and 9  [C]
 0x0010  0x00039BD4 (streamed)            RS 0x3C (D3DRS_ALPHATESTENABLE)
                                          := 1 (+ RS 0x43 := 0x00010101),
                                          else RS 0x3C := 0.  The test is
                                          GREATER 64/255, set once by the
                                          world setup (see above) and never
                                          changed.  Carried by the fences,
                                          armco, railings, foliage cards and
                                          overhead signs => cut-outs.      [C]
                                          NOT tested by the static apply.
 0x0020  0x0003A674 (static)              D3DRS_CULLMODE (RS 0x93, shadow
         0x00039C86 (streamed)            0x0075D6EC) := 0 = D3DCULL_NONE,
                                          i.e. THE MATERIAL IS TWO-SIDED;
                                          otherwise the slot gets the default
                                          from 0x004D6B34. The slot is
                                          identified by the values written to
                                          it elsewhere: 0x900 / 0x901 =
                                          D3DCULL_CW / D3DCULL_CCW
                                          (0x00031800, 0x0003C7CF,
                                          0x000303D0).                    [C]
 0x0040  0x000393F6 (class 7), 0x000394EA  SPECULAR GATE. Picks vertex shader
         (class 1), both streamed        0x004D6568 instead of 0x004D6564 for
                                          shader classes 1 and 7. Those two
                                          programs are byte-identical except
                                          for instruction 14:
                                            0x004D6564 (bit clear):
                                              MOV oD0.w, r1.z
                                            0x004D6568 (bit set):
                                              MUL oD0.w, v3.w, r1.z
                                          r1.z is the LIT specular term, v3.w
                                          the vertex alpha. So the bit means
                                          "MODULATE THE SPECULAR BY THE VERTEX
                                          ALPHA", i.e. let the artist paint
                                          which vertices shine. Every class-1
                                          material in US_C3_V1 sets it; a few
                                          on AS_C1_V1 do not (bk_building8,
                                          bk_ip_huts, bk_ShopTop4Glass,
                                          bk_downtown_bd) and shine
                                          everywhere.                      [C]
 0x0080  0x0003977E (streamed, `test al,al;
         jns`, i.e. the sign bit of the
         low flag byte)                   third texture-state block
                                          (0x4D65A0) for shader class 8.   [C]
 0x0100  not tested by either apply fn    -- (unused in C1_V1)             [?]
 0x0200  0x0003A6C8 (static, byte +0x25
         bit 0x02) / 0x00039CE0 (streamed) enable the UV scroll: vertex
                                          shader constant 0x63 :=
                                          (1 - frac(mat+0x1C), 0, 0),
                                          else (0,0,0). C1_V1: Arrows,
                                          bk_billboards2, bk_skytrainads x2.[C]
 0x0400  0x00039AF5 (streamed only)       DECAL: D3DRS_ZWRITEENABLE
                                          (RS 0x40, shadow 0x0075D5A0)
                                          := !(flags>>10 & 1). The static
                                          apply hard-codes 1 at 0x0003A5C2.
                                          C1_V1: bk_roaddecals (0x417),
                                          bk_roaddecals2 (0x412),
                                          bk_decalshadows (0x40F).         [C]
 >0x0400 no material in any shipped C1_V1 table sets a higher bit.

  Everything in the world is therefore SINGLE-SIDED unless bit 0x20 is set,
  and the data is wound consistently for it: cross(e1,e2) points +Y for all
  9,832 road-surface triangles, 0 exceptions. Materials carrying bit 0x20 are
  exactly the fences, railings, foliage, cones, benches, banners and signs.

  The harness renderer has no per-material cull state, so two-sided
  submeshes are emitted here with each triangle duplicated in reverse
  winding -- the standard way to express D3DCULL_NONE in a format that has
  no such flag. src/burnout3_trackmesh.c turns on backface culling for
  everything else. Before this, the whole world drew double-sided and the
  BACK of single-sided geometry showed through: the Bangkok underpass at
  route progress ~0.48 was walled off by the back of a bk_downtown_bd
  facade quad (static group 2, model 1) standing across the road.

Shader classes: what each one actually DRAWS                             [C]
------------------------------------------------------------------------
FUN_0003C8A0 (the renderer ctor, `this` = 0x004D6170, call at 0x00015C47)
creates every world shader and stores the handles in `this`; the two material
apply functions then select by class from that table:

  vertex programs, D3DDevice_CreateVertexShader(decl, prog, &slot, 0)
    0x004D6560  decl 0x003875A4  prog 0x003E8828   classes 0/3/6/10/default
    0x004D6564  decl 0x0038758C  prog 0x003E88C0   classes 1/7, flag 0x40 CLEAR
    0x004D6568  decl 0x0038758C  prog 0x003E89E8   classes 1/7, flag 0x40 SET
    0x004D656C  decl 0x0038758C  prog 0x003E8B10   class 2 (mirror)
  pixel shaders, 0xF0-byte D3DPIXELSHADERDEFs copied into a heap block
    0x004D6570  def 0x003E8D08   class 0 / 9(no 0x18) / default
    0x004D6574  def 0x003E8DF8   class 6 / 9(with 0x18)
    0x004D6578  def 0x003E8EE8   class 1
    0x004D657C  def 0x003E8FD8   class 7
    0x004D6580  def 0x003E90C8   class 10
    0x004D6584  def 0x003E91B8   class 2

The pixel-shader defs decode as register-combiner programs (PSTextureModes = 1
in ALL of them: ONE texture stage, stage 0. FUN_000393C0 confirms it from the
other side -- it writes the four SetTexture shadow slots 0x0075DB70/74/78/7C,
which FUN_001D7040 flushes in a 4-iteration loop at 0x001D7100, and for class 1
it sets stage 0 = the material's texture and stages 1..2 = NULL. Only class 3
ever binds a second texture, the fixed &DAT_004D6A00.  There is NO
multitexturing in the world draw):

  T0 = the material texture, V0 = the interpolated vertex diffuse (oD0),
  C0 = combiner factor 0 (set per material by FUN_0034E9A0), fog = the fixed
  final combiner `rgb = fog.a*R0 + (1-fog.a)*fog.rgb` that every class shares.

    class 0/default   R0.rgb = 2 * (T0.rgb * V0.rgb)
                      out.a  = C0.a            = material +0x20
    class 6           R0.rgb = 2 * (T0.rgb * V0.rgb)
                      out.a  = T0.a * C0.a     = texture alpha * material +0x20
    class 1           R0.rgb = 2 * (T0.rgb * V0.rgb)  +  (T0.a * V0.a) * C0.rgb
                      out.a  = T0.a * V0.a
    class 7           same rgb as class 1; out.a = C0.a
    class 10          R0.rgb = 2 * (T0.rgb * V0.rgb)  +  T0.a * C0.rgb
                      out.a  = T0.a          (emissive: C3_V1's Chgo_TunLght)

  and for classes 1/7/10, C0.rgb = sceneLight(DAT_0060E0A0..AC) * material+0x04.

  The `2 *` is PS_COMBINEROUTPUT_SHIFTLEFT_1 in the stage-0 RGB output word
  (0x000100C0, identical in all six defs). It is corroborated by the data:
  the maximum of every colour byte over all 95,624 C3_V1 streamed vertices is
  exactly 128, i.e. 128 = white.                                          [C]

CLASS 1 IS PER-VERTEX SPECULAR, NOT A DUAL-TEXTURE "DISTANCE BLEND".  The
class-1 vertex program (18 instructions, disassembled from the NV2A microcode
at 0x003E88C0 -- the four DP4s against c[16..19] writing oPos and the oT0 =
v9.xy + c[3].xy scroll validate the decode against the class-0 program).

The block is an Xbox `xvs` binary: a 4-BYTE header {u16 version 0x2078, u16
instruction count} followed by `count` 16-byte instructions in the natural
DWORD0..DWORD3 order, DWORD0 always zero (0x003E88C0 reads ver 0x2078
count 18, 0x003E7D58 -- the car program -- reads count 32, 0x003E8828 reads
count 9; each count lands the FINAL bit on the last instruction).  Field
layout is the canonical Cxbx/nouveau map; note in particular that the input
register index for a V-bank source comes from FLD_V at DWORD1 bits 9..12
(NOT from the per-source A_R/B_R/C_R fields), that FLD_C_R_LOW is DWORD3
bits 30..31, and that the MAC ADD reads banks A and C (not A and B).  An
earlier scratch disassembler (trackdark/vp2.py) missed all three and
therefore printed `v0` for every attribute and `<mux0:N>` for the LIT
source; the register names below are from the corrected decode.        [C]

     0  ADD  r2.xyz = v0 - c[0]              ; c[0] = eye position -> view vec
     1  DP3  r5.w   = v2 . c[1]              ; c[1] = light direction, N.L
     2  DP3  r3.w   = r2 . r2   | MOV oD0.xyz = v3.xyz     ; |V|^2 ; colour
     3  MOV  r9.xw  = c[2]                   ; c[2] = vs const 0x62 =
                                             ;        (1, 0, 0, spec power)
     4  MUL  r6.xyz = v2 * r5.w  | RSQ r1.w = 1/|V|   (the ILU is forced to
                                             ;  r1 when the MAC also writes a
                                             ;  temp, which is why 6 reads r1.w)
     5  ADD  oT0.xy = v9.xy + c[3].xy        ; the UV scroll (flag 0x200)
     6  MUL  r4.xyz = r2 * r1.w              ; normalised view direction
     7  ADD  r7.xyz = r6 + r6                ; 2*N*(N.L)
     8  DP4  oPos.x = v0 . c[16]
     9  ADD  r8.xyz = r7 - c[1]              ; R = 2N(N.L) - L
    10  DP4  oPos.y = v0 . c[17]
    11  DP3  r9.y   = r4 . r8                ; R.V
    12  DP4  oPos.w = v0 . c[19]
    13  DP4  oPos.z = v0 . c[18] | LIT r1.z = LIT(r9).z = pow(R.V, r9.w)
    14  MOV  oD0.w  = r1.z                   ; 0x004D6564 variant
        MUL  oD0.w  = v3.w * r1.z            ; 0x004D6568 variant (flag 0x40)
    15  MIN  oFog   = ...
    16  MUL  oPos.xyz, r12, c[-38]  | RSQ ...   ; NV2A viewport epilogue
    17  MAD  oPos.xyz, r12, r1.x, c[-37]

  so the complete class-1 surface is

      colour = 2 * tex.rgb * vcol.rgb
             + tex.a * (vcol.a) * pow(max(R.V,0), mat+0x08) * light.rgb * mat+0x04

  -- a Phong specular whose per-TEXEL mask is the texture's alpha channel,
  whose per-VERTEX gate is the vertex alpha, whose exponent is mat+0x08 and
  whose strength is mat+0x04.

  WHICH WAY THE LOBE POINTS -- the [?] that used to sit here is CLOSED.
  Because reflect2(A) := 2N(N.A) - A obeys reflect2(-A) = -reflect2(A), and
  the program dots reflect2(c[0x61]) against unit(P - eye) rather than
  unit(eye - P), the term is algebraically

      ( reflect2( -c[0x61] ) . unit(eye - P) )^power
    = ( reflect2( enviro.dat +0x80 ) . unit(eye - P) )^power

  i.e. TEXTBOOK PHONG WITH L = enviro.dat +0x80.  And enviro.dat +0x80 is the
  sun's DOWNWARD TRAVEL direction on all 36 shipped tracks -- its y component
  is -sin(elevation) for round elevations (-0.2588 = 15 deg, -0.3420 = 20,
  -0.4226 = 25, -0.5000 = 30, -0.4695, -0.4848, -0.5299, -0.5592, -0.6428 =
  40, -0.7071 = 45), never positive, and the float4 is already unit with
  w = 0.  A "light vector" that points below the horizon puts the Phong lobe
  BELOW the surface, so the term is identically zero on anything facing the
  sky: on US_C3_V1 frame 1100 every GL_road5/6/7 group measures exactly
  0.0000, and over a lap the level road only lights where the surface rises
  above the eye (uphill crests, peak 0.21).  Where it does fire strongly is
  near-VERTICAL geometry -- road signs, shopfronts, the class-2 glass and
  building reflections -- which is consistent with the class roster.
  The port reproduces this exactly; it is the shipped behaviour, not a bug in
  the port, and it means this additive CANNOT supply a broad brightness lift
  on the near road.  The opposite convention is used a few functions away:
  the CAR vertex program at 0x003E7D58 builds its view vector the other way
  round (`ADD r5.xyz, c[108], -r4` = eye - P, instruction 12) and so gets the
  ordinary outgoing reflection at instruction 21.                       [C]

  Class 0 is the same family with the specular
  term removed and no normal in the vertex stream.  The class-1 roster is
  exactly the shiny things: every road surface, the shop fronts, the garage
  door, the cabin window, the wet dirt track (US_C3_V1); the freeway decks,
  tower blocks and HK_Road* (AS_C1_V1, where bk_hk_fwnblend is one of 28).

NOTE ON CONSTANT NUMBERING.  The `c[n]` above are the microcode's own indices,
which ARE the D3D vertex-shader register numbers -- the class-0 program's UV
scroll reads c[99] = 0x63, its matrix c[112..115] = 0x70..0x73 and its fog
c[120] = 0x78, matching FUN_0034F840(ECX = register) / FUN_0034F8F0 exactly.
An earlier revision of this docstring wrote them biased by 96 (c[0], c[1],
c[3], c[16..19]); read c[k] there as register k + 0x60.                   [C]

WHAT EACH WORLD SHADER CONSTANT HOLDS -- the whole world pass in one table.
FUN_00038D10 fills them and every world draw (FUN_001AD350, FUN_001AD7A0,
FUN_001ADA40, FUN_001ADD60, FUN_001AE200) calls it first; FUN_00039140 is the
matching teardown.
    0x60  eye position -- the float4 at 0x004D67D0 with .y += 5.0 (the +5 is
          the literal at 0x003B1694), pushed at 0x0003911F              [C]
    0x61  -(light record +0x00), the vector the class-1/2/7 programs dot the
          normal against; negated by the XORPS at 0x00038D62, pushed at
          0x00039111                                                    [C]
    0x62  (1, 0, 0, material +0x08) -- the LIT exponent, per material    [C]
    0x63  (1 - frac(material +0x1C), 0, 0) -- the UV scroll, per material[C]
    0x70..0x73  the view-projection matrix, from 0x004D6730 via
          FUN_0034F8F0(0x70) at 0x00039102                               [C]
    0x78  (0, 0, light record +0x20, 0) -- the fog distance cap, pushed at
          0x00038F55                                                     [C]

THE TEXTURE ALPHA OF A CLASS-0/1/7/10 MATERIAL IS NOT OPACITY.  Opacity is
decided by two render states and nothing else: FUN_000393C0 sets
D3DRS_ALPHABLENDENABLE (RS 0x3B) := flags & 0x001 at 0x00039B21 and
D3DRS_ALPHATESTENABLE (RS 0x3C) := flags & 0x010 at 0x00039BD4.  Every
class-1 material in US_C3_V1 carries flags 0x40 or 0x60 -- both bits clear --
so it draws FULLY OPAQUE and the alpha channel is consumed only as the
specular mask above.  This is why the extractor now writes the flags into the
MTL: a renderer that decides "is this a cut-out?" by counting transparent
texels gets GL_Droad1b (55.7% of texels alpha==0, only 4.3% above 0.5) and
alpha-tests away 95% of the Silver Lake back-country road surface, which is
the whole of the "the road renders flat slate blue" bug -- the blue was the
sky dome showing through the discarded road.                              [C]

AND "DO WE HAVE A MATERIAL RECORD?" IS NOT `flags || cls`.  US_C3_V1's tunnel
interior walls are GL_tunnel_lightout and GL_tunnel_light, both class 0 with
flag word 0x0000 -- a renderer that uses `(flags || cls)` as the presence test
falls back to the texel heuristic for exactly them, and their textures are
97.7% alpha == 0 with 0.9% above 0.5, so 99% of the wall is discarded and you
drive through the tunnel looking out at the mountains (build/dump012.png; 470
triangles / 3821 m2 of interior wall in units 19/20/21 and 27/28, plus 56
triangles of GL_SHOPTOP2b).  The MTL writes a `# class` line for every
material, so its presence is the test; src/burnout3_trackmesh.c carries the
flag as TrackMeshGroup.have_material.  Everything else was ruled out first:
the walls are front-facing (107 of 109 near triangles pass the renderer's own
facing rule), they are not two-sided, they are opaque slots 9 and 31 of unit
20's 80 (well below the alpha cut at 75), there are no class-2 materials in
this track at all, the backdrop local-cell filter drops only GL_BD1/GL_BD6/
BD_bridge distant cards, and unit 20's blockB is a lower-poly LOD with the
same bounding box rather than extra geometry.                             [C]

THE OUTPUT ALPHA, AND WHY THE SHADOWS WENT BLACK.  Material +0x20 is the ALPHA
of combiner factor C0 (FUN_000393C0 loads it on every class branch --
0x0003945D, 0x0003951E, 0x00039602, 0x0003968A, 0x000397F5, 0x00039918 -- and
FUN_0034E9A0 packs the float4 to a D3DCOLOR with a plain clamp-and-*255,
constants at 0x003F7BE0/0x004A1F00/0x003F7BF0, so 0.6 in the file is 0.6 on
screen).  Each class then spends it differently, from the D3DPIXELSHADERDEF
final-combiner G input and the stage alpha words:
    class 0 / 7   out.a = C0.a              (G = C0.a)
    class 6       out.a = tex.a * C0.a      (stage-0 alpha = T0.a * C0.a)
    class 1       out.a = tex.a * vertex.a  (C0.a unused)
    class 10      out.a = tex.a
US_C3_V1's blended sheets are ALL class 6 and their +0x20 is 0.6 (Tree_shadow1,
GL_Pine_Treeshadow, GL_Tree_Shadowbirch, GL_rock_shadow, Chgo_RdDecals) or 0.5
(GL_Road_Decals), so retail lays the tree shadows on at 60% and their texture
-- which is pure black RGB with 55% of its texels at alpha > 0.9 -- darkens the
road to 40%, not to nothing.  Blending them at full texture alpha paints the
junction solid black (build/dump013.png).                                 [C]

THE SCENE LIGHT AND THE FOG -- how the world pass composes final brightness
------------------------------------------------------------------------
See parse_enviro() for the byte-level chain from enviro.dat to the light
records and the render states.  The whole composition, per fragment, is

    R0.rgb  = 2 * tex.rgb * vcol.rgb                       (all classes)
            + tex.a * (vcol.a if flag 0x40) * pow(max(R.V,0), mat+0x08)
                    * sceneLight.rgb * mat+0x04            (classes 1, 7)
            + tex.a * sceneLight.rgb                       (class 10)
    out.rgb = fog.a * R0.rgb + (1 - fog.a) * fogColour     (final combiner,
                                                            identical in all
                                                            six defs)

and that is ALL of it -- there is no ambient add, no second modulate and no
post-process grade anywhere in the world path.  The pieces:

  * sceneLight.rgb = DAT_0060E0A0 = enviro.dat +0x60. US_C3_V1: (0.992, 0.894,
    0.675), a warm sun. It reaches the pixel shader only through combiner
    factor C0 on classes 1/7/10, i.e. it tints the specular and the emissive
    and touches nothing else.                                            [C]
  * fogColour = D3DRS_FOGCOLOR = enviro.dat +0x00 times 127.5 (the constant at
    0x003B1E68 -- NOT 255). US_C3_V1 authors (222, 183, 139)/255 and the
    register receives (111, 91, 69). The halving is in the code; why the
    artists author at double is not recovered.                     [C], [?]
  * the fog factor is LINEAR (D3DRS_FOGTABLEMODE := 3 at 0x00038F23) over
    [0.05*far, (far - 0.05*far)/div + 0.05*far] -- 50..3850 on US_C3_V1 --
    BUT the fog coordinate is `min(clip z, far)`: every world vertex program
    ends with `min oFog, r12.z, c[120].z` and c[120].z is the light record's
    +0x20. So the factor is FLOORED at (end-far)/(end-start) and the fog can
    never take more than that share of the picture: 25% on US_C3_V1, 10% on
    EU_C3_V1, 30% on AS_C2_V1.                                           [C]
  * fog is scoped to the world: FUN_00038D10 sets D3DRS_FOGENABLE := 1
    (0x00038F47) and the world teardown FUN_00039140 sets it back to 0
    (0x000391C5). Cars, traffic and HUD are not fogged.                  [C]

Two consequences worth stating plainly, because both were guessed wrong before:
the baked vertex colour is HALF RANGE and the doubling in the pixel shader
restores it, so the road's 0.33 mean becomes 0.66 and there is no missing gain
to find; and the light record is NOT a per-zone selector -- DAT_0060E170 is
written with the render-target index by FUN_001AE340 (@0x001AE37F) and
FUN_001AE6F0 (@0x001AE703), and FUN_001888F0's loop initialises both records
identically, so a track has one light and one fog for its whole world.    [C]

Animated materials (header +0x0E count, +0x10 u16 index table)          [C]
------------------------------------------------------------------------
FUN_0019B1E0 ticks them once a frame (its only caller is FUN_001AA720 at
0x001AA7C3) off the global clock DAT_0060EA20 -- the same seconds counter the
vehicle step FUN_0011BE50 reads, i.e. the DILATED sim clock, so the boards slow
down during a takedown replay.  For each listed material, if frame_count
(+0x11) > 1 it advances the frame index +0x10 by the signed step at +0x13 and
ping-pongs at the ends; otherwise, if flag bit 0x200 is set (tested as byte
+0x25 & 2), it runs the UV SCROLL, which is a QUANTISED STEPPER and not a
continuous accumulation:

    T      = DAT_0060EA20                        (seconds)
    target = rate(+0x14) * T
    next   = step(+0x18) + phase(+0x1C)
    if (next <= target)  phase = (step == 0) ? target : next

-- at most one step per tick, so the phase advances in whole multiples of
+0x18 while keeping up with rate*T.  The material apply then pushes
(1 - frac(phase), 0, 0) into vertex-shader constant 0x63 (0x00039CE0 streamed
via the CRT floor at 0x00243743, 0x0003A6C8 static) and every world vertex
program starts with `add oT0.xy, v9.xy, c[99].xy`, so U scrolls and V never
moves.                                                                    [C]

THE COMPLETE ANIMATED-MATERIAL TABLE (all 38 shipped static.dat, 2026-08-13)
------------------------------------------------------------------------
The scroll arm above is only HALF the ticker, and the smaller half.  Across
every shipped track there are 19 distinct animated material names and only 9
of them scroll; the other 10 TEXTURE-FRAME CYCLE (see Material.frame_cycle),
and those are every bulb-matrix message board in the game.  Tagging scrolls
alone is why the port's boards stood still: `Arrows` is the ONLY scrolling
material in US_C3_V1, so the chevrons moved and nothing else did.

  UV SCROLL  (frame count < 2 AND flag 0x200)      rate      step   tracks
    Arrows            wrong-way chevron board      1.2/1.25  0.125    36
    ATB_River                                      0.1/0.5   0        24
    ATB_Waterfall                                  1.0       0         6
    spray                                          1.6       0         4
    bk_skytrainads                                +0.1/-0.1  0         8
    bk_billboards2                                 0.04      0         4
    Water / WC_Water / WintercityWater             0.05      0         6

  step 0 takes the `phase = target` branch, i.e. a SMOOTH scroll; only Arrows
  has a non-zero step and is therefore the quantised one -- 0.125 of the 128 px
  texture, exactly one bulb column, every 1/9.6 s.  The two AS_C1_V1
  bk_skytrainads records are a matched +0.1 / -0.1 pair and THE NEGATIVE ONE
  NEVER MOVES: the assignment is gated on `step + phase <= rate * T`, which for
  step 0, phase 0 and a negative rate is `0 <= negative` -- false forever.  A
  port reproduces that by leaving rate <= 0 materials baked.              [C]

  FRAME CYCLE  (frame count >= 2, NO flag needed)  n   period  tracks
    water                                          17  0.05..0.5   46
    Surf                                           16  0.05         8
    Chgo_Flag                                      10  0.0667      14
    bk_warnsignb   bulb-matrix message board     6/12  1.0          8
    ATB_Sign_Info_Text_A  bulb-matrix text         8  1.0          10
    Freeway_info_slow     the "SLOW" gantry        4  0.3333        4
    bk_warnsigna   bulb-matrix message board       2  1.0           6
    Chgo_AccidentSign_Words  SLOW/DOWN board       2  1.0           8
    Chgo_words                                     2  1.0           6
    ATB_Sign_Info_Flash                            2  0.5          10

  bk_warnsignb, bk_warnsigna and ATB_Sign_Info_Text_A carry NON-UNIFORM frame
  labels (1,4,5,8,9,12 / 1,3 / 1,3,4,7,8,11,12,15), so they hold 3 s and blink
  1 s rather than running at a flat rate -- the keyframe timing the frame arm
  reads out of the texture names.  Every other set is uniformly numbered and
  degenerates to a flat flipbook.  NO material in ANY shipped track sets the
  ping-pong bit 0x100, so retail always wraps; the earlier claim here that
  `water` ping-pongs was wrong (it carries 0x020/0x024/0x064).

  Barrier, the red X that shares the chevron group with Arrows, is in NO
  track's animated list -- the X boards genuinely do not animate in retail.

This is why the extractor writes `# uv_scroll <rate> <step>` AND
`# anim_frames`/`# anim_frame` into the MTL: both kinds change every frame --
one its texture coordinates, the other its texture BINDING -- so a renderer
that bakes the world into a display list has to draw those groups separately
(src/burnout3_trackmesh.c: trackmesh_tick / trackmesh_draw_scroll).

Streamed units: the PVS block, the material-slot split, and the draw passes
------------------------------------------------------------------------
The streamed unit table (static.dat +0x54 count, +0x58 ptr) has 0x10-byte
entries `{i32 blockA_off, i32 blockB_off, u32 blockA_size, u32 blockB_size}`
into streamed.dat (+0x10 for the block header). NOTE: unit 0's blockA offset
is 0 and IS valid -- an earlier `if not sub_off: continue` silently dropped
the whole first unit.

Each block starts with one Xbox submodel at blockOff+0x10+0x40 (call that
`base`); the PVS block follows at `X = base+0x20`. At runtime the current
cell's PVS block is `trackObj+0x1C4` and it sits 0x70 BELOW X, i.e.
`pvsRT = X - 0x70` -- pinned by the material table, which the game reads at
`pvsRT+0x84 + slot*2` (0x001AD57E / 0x001AD651) and which is X+0x14 in the
file. With that anchor every other field lands on real data:

  pvsRT+0x71  u8    material-slot count for the blockB model            [C] 0x001ADB7E
  pvsRT+0x72  u8    (same value in all 49 C1_V1 units)                  [?]
  pvsRT+0x73  u8    FIRST ALPHA SLOT / opaque-slot count for blockA     [C] 0x001AD8AF
  pvsRT+0x74  i8[8] backdrop group indices this cell draws, <0 = none   [C] 0x001AD976
  pvsRT+0x7D  i8    chevron group index, <0 = none                      [C] 0x001AE211
  pvsRT+0x7E  i8    water group index                                   [C] 0x001AD9DD
  pvsRT+0x7F  i8    reflection group index                              [C] 0x001AD9E4
  pvsRT+0x84  u16[] material id per slot                                [C]
  pvsRT+0x12A + c*0xA9   3-byte chunk header: [0] = signed unit offset
                         relative to this cell (0 = self), [1] = valid   [C] 0x0019D1F7
  pvsRT+0x12D + c*0xA9 + slot   i8 head submesh index, blockA model      [C] 0x001AD52C
  pvsRT+0x180 + c*0xA9 + slot   i8 head submesh index, blockB model      [C] 0x001AD5FC
There are 17 chunks (c = 0 is self, 1..8 are units -1..-8, 9..16 are +1..+8),
0xA9 = 3 + 0x53 + 0x53 bytes each.

The world draw is FUN_001AE340, and it issues the passes in this order:

  0x001AE375  FUN_0019D100   build the 17 visible-slot model/vis arrays
  0x001AE460  FUN_001AD7A0   STREAMED OPAQUE: for unit slot i = 0..16, for
                             material slot j = 0..pvs[0x73)-1, draw the
                             blockA chain at pvs[0x12D + i*0xA9 + j]
              (inside it, 0x001AD9AA) the up to 8 BACKDROP groups named by
                             pvs2[0x74..0x7B]
              (inside it, 0x001ADA27) WATER + REFLECTION groups
  0x001AE4AE  FUN_001ADD60   STREAMED ALPHA: for unit slot i = 16..0
                             (descending, i.e. far-to-near), for material
                             slot j = pvs[0x73]..pvs[0x71]-1, same blockA
                             chain array
  0x001AE4E0  FUN_001AE200   CHEVRON group (last of all world geometry)

Which model a visible slot points at is an LOD choice, and it is a pure
distance-in-units rule.  FUN_0019D100 reads the chunk's signed unit offset
(0x0019D1F7), takes its absolute value with the usual CDQ/XOR/SUB idiom and
compares it with 4:

  0019d23b  CDQ / XOR EAX,EDX / SUB EAX,EDX    ; EAX = |unit offset|
  0019d244  CMP dword ptr [ESP + 0x1c],0x4
  0019d249  JLE 0x0019d24f                     ; <= 4  -> blockA (full detail)
  0019d24b  MOV BL,0x1                         ; 5..8  -> blockB (low LOD)

so the eight nearest cells (self +/- 4) draw the blockA model this tool
extracts and the outer ring draws the lower-poly blockB one.  The near road,
and everything lying on it, is always blockA.                            [C]

Inside a slot, FUN_001AD510 walks the chain and the ONLY per-submesh gate is
`FUN_001B23F0(&DAT_004D67F0, submesh)` -- a bounding-box-vs-plane-set test
over the submesh's eight leading corner float4s (+0x00..0x7F, immediately
before the fmt/count/ptr triple at +0x80/+0x84/+0x88), returning 0 outside /
1 straddling / 2 inside -- and it is skipped entirely when the caller's
"whole unit inside the frustum" flag is set.  There is no distance fade, no
range cutoff and no per-batch constant anywhere in that path.            [C]

So a streamed unit's material table is SORTED, and pvs+0x73 is the cut: slots
below it are the opaque pass, slots at/above it are a separate transparent
pass that runs after ALL opaque geometry of ALL units and after the static
backdrop and water. In C1_V1 unit 1 the cut is at slot 65 and the six slots
above it are bk_shoptop1, bk_shoptop2, bk_roaddecals, bk_ShopTop4Glass,
bk_meshfence and one unnamed -- exactly the coplanar facade/decal detail
layer. Emitting each unit's alpha slots immediately after its own opaque
slots (what this tool used to do) leaves those overlays fighting the walls
and roads of every LATER unit, which overlap them: C1_V1's 49 unit bounding
boxes overlap heavily. They are now emitted as one global pass.

The backdrop groups drawn at 0x001AD9AA are NOT all of them: the cell names up
to 8 of the 14 in pvsRT+0x74..0x7B. Baking the union into one display list puts
distant-only backdrop cards in front of the streamed geometry they stand in
for -- in C1_V1, backdrop group 2 model 1 (bk_downtown_bd) spans
X[921..1573] Z[1479..1807], three metres in front of the shop fronts of units
5..8, and groups 2 and 3 are named only by units 43..48 on the far side of the
loop. parse_static_group() applies a local-cell filter for that; see its
docstring.

FUN_001ADA40 (called only from 0x0003FCB4, not from FUN_001AE340) is the
blockB / +0xC4 model pass plus the header+0x34 instanced-prop pass; it is a
separate render target (it forces COLORWRITEENABLE 0x10101 and uses the
static-style material apply). Neither blockB models nor the instanced props
are extracted here.  For the IN-RACE draw that is not a gap in coverage but a
deliberate LOD choice: FUN_0019D100's |offset| <= 4 test above means blockB
only ever stands in for cells 5..8 away, where this tool's blockA geometry is
the higher-detail version of the same place.  It does mean the harness draws
more triangles at distance than retail does.                             [C]

Static group draw order (FUN_001AD350 / FUN_001AE6F0)                    [C]
------------------------------------------------------------------------
FUN_001AE6F0 calls FUN_001AD350(track, pass) with pass = 0, 1, 2, and the
BACKDROP group table (header +0x24) alone is split into three ranges by two
header bytes: pass 0 = groups [0, hdr+0x14), pass 1 = [hdr+0x14, hdr+0x15),
pass 2 = [hdr+0x15, u16 at hdr+0x1C). In C1_V1 both bytes are 0, so all 14
backdrop groups draw in pass 2. FUN_001AE6F0 is NOT the in-race world draw
(that is FUN_001AE340, above); it is reached from 0x00014D75 and renders the
same static groups for a second target.

Decals: material flag bit 0x400, and the streamed DRAW ORDER          [C]
------------------------------------------------------------------------
Road markings, arrows, patches and blob shadows are ordinary coplanar
overlay geometry sitting a hair above the road (median +0.0114 world
units over the surface below, p95 +0.135). In C1_V1 exactly three
materials carry flag bit 0x400 and they are, by name, exactly that
layer: bk_roaddecals (0x417), bk_roaddecals2 (0x412), bk_decalshadows
(0x40F). Nothing else in the 181-entry table has the bit.

The bit is consumed by the STREAMED-world material apply FUN_000393C0
(the sibling of the static-group FUN_0003A3C0), which turns DEPTH
WRITES OFF for it:

    00039AF5  MOV  CX,word ptr [ESI + 0x24]   ; material flags
    00039AF9  SHR  ECX,0xa                    ; -> bit 0x400
    00039AFC  NOT  ECX
    00039AFE  AND  ECX,EBX                    ; EBX = 1
    00039B04  MOV  [EAX*4 + 0x0075DE20],0x40  ; mark render state 64 dirty
    00039B1B  MOV  [0x0075D5A0],ECX           ; shadow[64] := !decal

0x0075D5A0 is 0x0075D4A0 + 64*4, the shadow slot for render state 64 =
D3DRS_ZWRITEENABLE (RE_FRONTEND 6.7.1 documents the shadow scheme and
the flusher FUN_001D7040).  The neighbouring ids identify themselves by
the values written to them: state 57 only ever receives 0x201 / 0x203 /
0x207 = D3DCMP_LESS / LESSEQUAL / ALWAYS (0x0003C360, 0x0003616D,
0x001DA2E6), i.e. 57 = D3DRS_ZFUNC, and 62/63/67/74 were already pinned
as SRCBLEND / DESTBLEND / COLORWRITEENABLE / BLENDOP.  State 64 only
ever receives 0 or 1.  So: decal => ZWRITEENABLE 0, everything else 1
(FUN_0003A3C0 @0x0003A5C2 hard-codes 1 for the static groups, which
carry no decals).  The ambient depth test is LESSEQUAL (0x001BFAEC).

The game does NOT use a depth bias: render states 76..81
(SWATHWIDTH, POLYGONOFFSETZSLOPESCALE, POLYGONOFFSETZOFFSET,
POINTOFFSETENABLE, WIREFRAMEOFFSETENABLE, SOLIDOFFSETENABLE) are
written once, to 4/0/0/0/0/0, at 0x001BFC07..0x001BFCC4 and never
touched again.  There is no ZBIAS anywhere.

What keeps the decals from z-fighting is DRAW ORDER: a submesh's place in
the frame is its material SLOT, and the tables are sorted opaque-first.
Emitting submeshes in index order (what this tool used to do) put the
decals BEFORE the road that they sit on top of; 2392 of C1_V1's 2465 decal
triangles were affected. Submeshes are now emitted in material-slot order
within their pass, which is the game's own order.

src/burnout3_trackmesh.c completes it: the harness bakes the whole
track into ONE display list with no per-cell visibility, so the
material-major sort's *global* consequence -- decals after all of the
world, not just after their own unit's road -- is applied there, using
the `# decal 1` MTL annotation this tool writes.

Draw order and ZWRITE are the WHOLE law; there is no distance fade, no
per-batch alpha ramp and no range cutoff on the layer.  The opacity is the
per-material constant C0.a = material+0x20, built by FUN_000393C0's class-6
arm at 0x000396B8..0x000396D9 and pushed to NV097_SET_COMBINER_FACTOR0 by
FUN_0034E9A0, which packs the float4 to a D3DCOLOR with *255 on every lane
(0x003F7BF0..FC); the classes-0/3/6/10 vertex program 0x003E8828 has nine
instructions and no view-dependent term at all; and the per-submesh gate in
FUN_001AD510 is only the frustum test FUN_001B23F0.  See
src/burnout3_trackmesh.c above trackmesh_decals_last() for the full
citation set and for why the layer is nevertheless the largest single
darkener the harness applies to the road.                             [C]

The wrong-way chevrons: there is no direction gating to recover      [C]
------------------------------------------------------------------------
The yellow bulb-chain chevron boards over C1_V1's junctions are the
"chevron" group (header +0x1E count, +0x28 table), materials Arrows (the
bulb chevron) and Barrier (the red X). They are drawn by FUN_001AE200 ->
FUN_0019B1B0 -> FUN_001AD050, last in the frame. Everything that can gate
them was checked:

  * the only per-cell selector is the single signed byte pvsRT+0x7D
    (0x001AE211), a chevron GROUP index. C1_V1 has exactly ONE chevron
    group, so it is on/off, not a choice of direction. Across all 40
    shipped tracks the count is 1 everywhere except AS/M1_V1 and AS/M1_V2
    (2) and US/C5_V1 (0); on M1_V1 the two groups are selected by unit
    range (group 0 for units 75..114, group 1 elsewhere) and their
    submodels sit in disjoint parts of the map -- the selector is SPATIAL.
  * the mode gate at 0x001AE24E is a virtual call on the .bgd mode-handler
    singleton `*(DAT_004D5370+0x1B8)`, vtable slot +0xA4. That slot is the
    constant-false stub FUN_00023C10 in 7 of the 9 mode handlers, and
    FUN_00015610 (`this->[0x40] == 1 || == 4`) in FUN_0005F7B0 modes 2
    and 3. It suppresses ALL chevrons for those modes; it is not a
    forward/reverse test, and there is no second (reverse) chevron set to
    switch to anywhere in the shipped data.
  * there is no per-submodel or per-submesh flag in the chevron path:
    FUN_0019B1B0 walks every submodel of the selected group entry
    unconditionally.

So retail draws the same chevron boards for a Forward event and a Reverse
one. What they show is baked into the art: fitting `u = f(x,z)` per
connected board and comparing the world direction of increasing U with the
forward race line (Gamedata.bgd 0x1ABF20, 1029 pts) gives dot <= 0 for all
24 C1_V1 Arrows boards, mean -0.95. The pieces that could have made that an
extraction artefact were each ruled out:
  - the race-line array order IS the race direction: all six start-grid
    slots of the C1_V1 event spatial record have their basis row 2 within
    0.005 of the line direction at the nearest node (dot +1.00 x6);
  - the texture pipeline does not mirror: bk_warnsigna/bk_warnsignb read
    "SLOW DOWN"/"SPEED LIMIT" and bk_newfwysigns1 reads "Tourist
    Information" the right way round in the extracted PNGs;
  - the harness's uniform Z-negation is an orthogonal transform, so it
    preserves dot(dU, forward);
  - the only per-material UV manipulation in the whole apply path is the
    scroll OFFSET at shader constant 0x63, which cannot mirror anything.
The boards are therefore drawn exactly as retail draws them and are NOT
hidden or reoriented here. The one real defect the chevron investigation
did turn up is draw order, fixed above: they belong LAST, not second.

The chevron boards ARE collidable in retail                              [C]
------------------------------------------------------------------------
US_C3_V1's chevron group is one group, six submodels, seven submeshes, 102
triangles, materials `Arrows` (58 tris) and `Barrier` (44).  The static groups
are never handed to the collision builder -- the collision soups are the
per-unit kd-trees in streamed.dat -- but the level compiler baked a quantised
copy of all 102 chevron triangles into the surrounding units' soups under
SURFACE TYPE 0x0020: build/collision.bin holds exactly 114 type-0x0020
triangles (102 distinct, duplicated across the overlapping units), 126 unique
vertices against the render group's 126, of which 104 agree to 0.1 m and the
other 22 to within one u16 quantisation step (1000/65536 = 0.01526 m).

Whether a car collides with them is then decided by the per-frame gather
callback, and there are two:
  * FUN_00109CE0, used by FUN_00109D20 <- FUN_00122D00, skips surface low
    bytes 0x20, 0x22, 0x23 and 0x24 (compares at 0x00109CFF/0x00109CF0/
    0x00109CF5/0x00109CFA).  FUN_00122D00 is reached through the stub at
    0x0011BE40, which runs only when the vehicle byte +0x210 is set -- the
    CRASHED path.  This is the WRECK gather.
  * FUN_0011BBE0, used by FUN_0011BC60, which the per-frame vehicle update
    FUN_0011BE50 calls directly at 0x0011BF48.  It skips low bytes 0x23 and
    0x22 only (0x0011BBFE/0x0011BC03), skips anything with type bit 0x1000
    (`test ch,0x10` at 0x0011BC08), and for low bytes 0x15..0x20 additionally
    skips a face whose normal dotted with the vehicle velocity at veh+0xB0
    exceeds 0.5 (0x0011BC1A..0x0011BC39) or whose normal.y is below -0.7.
    This is the RACING gather, and it does NOT exclude 0x20.
So retail collides the racing car with the chevron boards.  A harness that
copies the 0x20/0x22/0x23/0x24 exclusion set into its driving sweep is using
the wreck gather's filter and will drive straight through them.
"""
import math
import os
import struct
import sys

GROUP_NAMES = ("backdrop", "chevron", "water", "reflection")
VTX_STRIDE = 0x1C
SUB_STRIDE = 0x90
MDL_STRIDE = 0x60
MAT_STRIDE = 0x28
TRI_STRIP, TRI_LINE = 0x6, 0x2

# material record fields (see the header comment for the evidence)
MAT_FLAGS_OFF = 0x24
# CORRECTED 2026-08-12: bit 0x001 is ALPHA BLEND and bit 0x010 is ALPHA TEST,
# not the other way round.  Render state 0x3B is D3DRS_ALPHABLENDENABLE (59)
# and 0x3C is D3DRS_ALPHATESTENABLE (60) -- pinned by their neighbours, which
# the world setup FUN_00038D10 writes as a coherent triple: RS 0x3A := 0x204
# (= D3DCMP_GREATER on the 0x200 base that RS 57/ZFUNC already established),
# RS 0x3C := 0, RS 0x3D := 0x40.  That is ALPHAFUNC/ALPHATESTENABLE/ALPHAREF
# = "GREATER, off, 64" -- the classic cut-out setup -- and it only parses
# that way with 58/59/60/61 = ALPHAFUNC/ALPHABLENDENABLE/ALPHATESTENABLE/
# ALPHAREF, which is also the order the already-pinned 57 = ZFUNC,
# 62/63 = SRCBLEND/DESTBLEND, 64 = ZWRITEENABLE, 67 = COLORWRITEENABLE run in.
# The data agrees: bit 0x001 is carried by the tree/rock/bridge shadow blobs,
# the road-decal sheets and the chevron boards (soft-edged alpha ramps that
# must blend), bit 0x010 by the mesh fences, foliage cards, armco, railings
# and overhead signs (hard cut-outs).                                     [C]
MAT_ALPHA_BLEND = 0x001     # RS 0x3B := 1, COLORWRITEENABLE RGB   [C] 0x00039B21
MAT_TWO_SIDED = 0x20        # D3DRS_CULLMODE := D3DCULL_NONE       [C] 0x0003A674
MAT_ALPHA_TEST = 0x010      # RS 0x3C := 1                         [C] 0x00039BD4
MAT_SPEC_GATE = 0x040       # class 1/7 VS variant: oD0.w *= v3.w  [C] 0x000394EA
MAT_UV_SCROLL = 0x200       # vertex shader constant 0x63          [C] 0x00039CE0
# Neither material apply reads bit 0x100 -- the ANIMATED-MATERIAL TICKER does.
# FUN_0019B1E0 tests it as `*(byte*)(mat+0x25) & 1` (i.e. the flag word's bit
# 0x100) at the top of the frame-cycle arm and takes the +0x13-stepped
# PING-PONG path when it is set, the plain `(frame+1) % n` wrap when it is
# clear.  NO material in ANY of the 38 shipped track files sets it, so retail
# only ever wraps; the ping-pong path is implemented for fidelity and is dead
# on the shipped data.  (RE_NOTES called `water` a ping-pong -- it is not; it
# carries flags 0x020/0x024/0x064 and wraps its 17 frames.)              [C]
MAT_FRAME_PINGPONG = 0x100
MAT_DECAL = 0x400           # D3DRS_ZWRITEENABLE := 0              [C] 0x00039AF5
MAT_CLASS_MIRROR = 2        # skipped by FUN_001AD350              [C] 0x001AD40D
# D3DRS_ALPHAFUNC / D3DRS_ALPHAREF, set once by the world setup FUN_00038D10
# (@0x0003901B RS 0x3A := 0x204 = D3DCMP_GREATER, @0x00038FEE RS 0x3D := 0x40)
# and never changed by either material apply.  The cut-out threshold is
# therefore 64/255, not the 0.5 a renderer would reach for.               [C]
MAT_ALPHA_REF = 64.0 / 255.0
# shader classes whose pixel shader adds the specular/emissive term
# tex.a * C0.rgb (C0.rgb = sceneLight * mat+0x04). See "Shader classes".
#
# CLASS 2 BELONGS HERE AND WAS MISSING UNTIL 2026-08-12.  Its pixel shader
# (D3DPIXELSHADERDEF 0x003E91B8) carries the SAME two-stage structure as the
# class-1 one, just routed through R1:
#     stage 0 alpha  PSAlphaInputs[0]  = 0xD4D81010  -> A=V0.a  B=T0.a
#                    PSAlphaOutputs[0] = 0x000000D0  -> AB into R1  (not R0)
#     stage 1 rgb    PSRGBInputs[1]    = 0xC1DD20CC  -> A=C0.rgb B=R1.a
#                                                       C=1      D=R0.rgb
#                    PSRGBOutputs[1]   = 0x00000C00  -> AB+CD into R0
# i.e. R0.rgb = 2*(T0*V0) + (T0.a * V0.a) * C0.rgb, which is the class-1
# equation verbatim -- and the class-2 material apply loads combiner factor 0
# with sceneLight * material+0x04 exactly as the class-1/7 branches do.  So a
# class-2 surface takes the additive scene-light term; omitting it drew every
# reflective pane in the game with its base pass only.  The vertex-alpha gate
# is UNCONDITIONAL for class 2 (the flag 0x40 variant selection is a class-1/7
# thing -- class 2 has a single vertex program, 0x003E8B10), so the gate is
# reported as True regardless of the flag word.                            [C]
#
# What class 2 additionally does and this extractor does NOT reproduce: its
# vertex program computes oT0 analytically as a reflection coordinate (it
# builds the view vector, reflects it, and scales the result by c[0x65].x =
# 0.5) instead of passing v9 through, so the surface samples its own texture
# as a fake environment map.  The UVs in the file are therefore NOT the ones
# the game draws with.  Class 3 (EU_C3_V1's single `Chgo_Glass_Reflec`-style
# record) goes further and samples a 640x480 render target through a
# screen-space projection -- a real planar reflection.  Neither is expressible
# in a baked OBJ.                                                     [C], [?]
MAT_CLASSES_SPECULAR = (1, 2, 7)
MAT_CLASS_EMISSIVE = 10
# Shader classes whose vertex DECLARATION has no D3DCOLOR register, i.e. the
# ones that never read the vertex colour at all: class 8 (0x003875E8 =
# position + texcoord) and class 9 (0x003875D4 = position + normal +
# texcoord). Those are the foliage/prop/cone families; their oD0 comes out of
# the vertex program, not the file, so a renderer that modulates them by the
# stored colour buries the trees in shadow that the game never applies.  [C]
#
# WHAT THE GAME PUTS THERE INSTEAD -- neither is "white", so the flat-white
# fallback in src/burnout3_trackmesh.c is a stand-in, not a reproduction:
#   class 8  the three class-8 pixel shaders (0x003E93E8/0x003E94D8/
#            0x003E95C8, selected by flag bits 0x08/0x80) read C0.rgb where
#            every other class reads V0 -- PSRGBInputs[0] = 0xC8C10000, and
#            PSC0Mapping = 0xFFFFFFF2 routes stage 0's C0 to pixel-shader
#            constant register 2.  Their stage-0 output word is 0x000000C0,
#            i.e. NO SHIFTLEFT_1: class 8 is the one world class that is not
#            doubled.  The vertex program 0x003E9360 never writes oD0.
#   class 9  vertex program 0x003E92A8 LIGHTS THE VERTEX AT RUNTIME:
#              dp3 r2, v2, c[102]      ; N . L
#              mul r3, r2, c[101]      ; * light colour
#              add oD0, r3, c[100]     ; + ambient
#            the only per-vertex N.L in the whole world renderer.  Its three
#            constants are hardware 100/101/102 = D3D c[4]/c[5]/c[6], and
#            c[102] is never written by any call site in the executable, so on
#            retail hardware class 9 collapses to oD0 = c[100].            [C]
# No track shipped in this game uses class 8 or class 9 -- the six extracted
# tracks contain only classes 0, 1, 2, 3, 5, 6 and 10 -- so this is dead data
# in practice and recorded here so the next reader does not re-derive it.
MAT_CLASSES_NO_VCOLOR = (8, 9)

# PVS block field offsets, expressed relative to X = submodel+0x40+0x20 (the
# file position); the runtime base is X-0x70. See the module docstring.
PVS_NSLOT_B = 0x01          # pvsRT+0x71
PVS_NSLOT_OPAQUE = 0x03     # pvsRT+0x73 -- first alpha slot of the A model
PVS_MATTAB = 0x14           # pvsRT+0x84
PVS_CHUNK0 = 0xBA           # pvsRT+0x12A, stride PVS_CHUNK_STRIDE
PVS_CHUNK_STRIDE = 0xA9
PVS_CHUNK_HEADS_A = 3       # +3 within the chunk -> pvsRT+0x12D
PVS_CHUNKS = 17
SUB_NEXT_OFF = 0x8C         # i8 next-in-chain


class Reader:
    def __init__(self, data):
        self.d = data

    def u16(self, o):
        return struct.unpack_from('<H', self.d, o)[0]

    def u32(self, o):
        return struct.unpack_from('<I', self.d, o)[0]

    def i32(self, o):
        return struct.unpack_from('<i', self.d, o)[0]

    def i8(self, o):
        return struct.unpack_from('<b', self.d, o)[0]

    def ptr(self, o, rel=0):
        """Relative pointer: zero stays zero, otherwise add the base."""
        v = self.i32(o)
        return v + rel if v else 0


def strip_to_triangles(indices):
    """Triangle strip -> triangle list, dropping degenerates."""
    tris = []
    for i in range(len(indices) - 2):
        a, b, c = indices[i], indices[i + 1], indices[i + 2]
        if a == b or b == c or a == c:
            continue
        tris.append((a, b, c) if i % 2 == 0 else (a, c, b))
    return tris


def read_cstr(d, o, limit=64):
    e = d.find(b'\0', o)
    if e < 0 or e - o > limit:
        return None
    try:
        return d[o:e].decode('ascii')
    except UnicodeDecodeError:
        return None


class Material:
    """One 0x28-byte static.dat material record (the fields we use)."""

    __slots__ = ("texture", "flags", "cls", "spec_strength", "spec_power",
                 "alpha_scalar", "anim_rate", "anim_limit", "anim_frames",
                 "animated", "anim_index", "anim_label", "anim_step",
                 "frame_textures", "frame_labels")

    def __init__(self, texture, flags, cls, spec_strength=0.0, spec_power=1.0,
                 alpha_scalar=1.0, anim_rate=0.0, anim_limit=0.0,
                 anim_frames=1, animated=False, anim_index=0, anim_label=1,
                 anim_step=1, frame_textures=None, frame_labels=None):
        self.texture = texture
        self.flags = flags
        self.cls = cls
        self.spec_strength = spec_strength   # +0x04
        self.spec_power = spec_power         # +0x08
        self.alpha_scalar = alpha_scalar     # +0x20
        self.anim_rate = anim_rate           # +0x14
        self.anim_limit = anim_limit         # +0x18
        self.anim_frames = anim_frames       # +0x11
        # listed in the animated-material index table at header +0x10, i.e.
        # FUN_0019B1E0 ticks it once a frame.
        self.animated = animated
        self.anim_index = anim_index         # +0x10  current frame, 0 at load
        self.anim_label = anim_label         # +0x12  current numeric label
        self.anim_step = anim_step           # +0x13  frame step, +1 / -1
        # One texture name per animation frame, read from the pointer ARRAY at
        # +0x0C (the game indexes it as `(*(int**)(mat+0xC))[frame]`), and the
        # integer FUN_0019B1E0 parses out of each name -- see frame_cycle.
        self.frame_textures = frame_textures or [texture]
        self.frame_labels = frame_labels or [0]

    @property
    def uv_scroll(self):
        """(rate, step) for a material the animated-material ticker scrolls,
        else None.

        FUN_0019B1E0's `frame_count < 2` branch is the scroll branch: it only
        runs for materials in the header+0x10 index table whose frame count
        (+0x11) is below 2 and whose flag bit 0x200 is set, and its whole body
        is (T = the global clock DAT_0060EA20, in seconds):

            target = rate(+0x14) * T
            next   = step(+0x18) + phase(+0x1C)
            if next <= target:  phase = target if step == 0 else next

        -- a QUANTISED stepper, at most one step per tick.  The phase advances
        in whole multiples of +0x18 and keeps up with rate*T, so the mean
        speed is +0x14 texture widths per second taken in +0x18 jumps.  On
        US_C3_V1 the only scrolling material is `Arrows`, the wrong-way
        chevron board: rate 1.2, step 0.125 = one bulb column of the 128 px
        texture every 1/9.6 s.                                            [C]
        """
        if not (self.animated and (self.flags & MAT_UV_SCROLL)
                and self.anim_frames < 2):
            return None
        return (self.anim_rate, self.anim_limit)

    @property
    def frame_cycle(self):
        """The OTHER half of FUN_0019B1E0: TEXTURE-FRAME cycling, or None.

        `if (frame_count(+0x11) < 2) { ...scroll... } else if (...)` -- the two
        mechanisms are the two arms of one `if`, so a material NEVER does both,
        and the frame arm needs NO flag bit at all: two or more frames is the
        whole entry condition (EU_C4_V1's `Freeway_info_slow` animates on flags
        0x000).  This is why an extractor that only emitted `uv_scroll` left
        every bulb-matrix message board in the game standing still -- the
        scrolling chevron `Arrows` is the ONE animated material in US_C3_V1
        that takes the scroll arm.

        The frame arm, verbatim from the decompile (T = DAT_0060EA20):

            if (hold(+0x18) + last(+0x1C) < T) {
                last = T
                if (flags & 0x100) { ...ping-pong on step(+0x13)... }
                else               f = (frame(+0x10) + 1) % n; frame = f
                nxt = (frame + 1) % n
                if (nxt == 0) { hold = period(+0x14); label(+0x12) = 1 }
                else {
                    L    = atol(name[nxt] + strlen(name[0]))
                    hold = (L - label) * period
                    label = L
                }
            }

        SO THE FRAME DURATIONS LIVE IN THE TEXTURE NAMES.  The pointer array at
        +0x0C holds one texture record per frame; the game skips the first
        frame's whole name and runs the C library's atol over the tail of the
        NEXT frame's name, which for `bk_warnsignb` / `bk_warnsignb4` /
        `bk_warnsignb5` / `bk_warnsignb8` ... yields 4, 5, 8 ... -- KEYFRAME
        TIMES in units of the period, not frame ordinals.  bk_warnsignb's
        labels 1,4,5,8,9,12 at period 1.0 s are therefore a message that holds
        for 3 s and blinks for 1 s, three times, per 12 s cycle; the wrap arm
        supplies the last frame's own 1-period hold.  A uniformly numbered set
        (`water` .. `water17`, `Freeway_info_slow` .. `_slow4`) degenerates to
        a flat one-period-per-frame flipbook, which is why the naming looked
        like plain numbering.

        Returned as (n, period, hold0, label0, frame0, step, pingpong, labels)
        with `labels` one integer per frame (index 0's is never read -- the
        wrap arm special-cases it).  Everything is straight out of the record;
        no material is named anywhere in this tool.
        """
        if not (self.animated and self.anim_frames >= 2):
            return None
        return (self.anim_frames, self.anim_rate, self.anim_limit,
                self.anim_label, self.anim_index, self.anim_step,
                1 if (self.flags & MAT_FRAME_PINGPONG) else 0,
                list(self.frame_labels))

    @property
    def two_sided(self):
        """D3DCULL_NONE -- draw this material from both sides. [C] 0x0003A674"""
        return bool(self.flags & MAT_TWO_SIDED)

    @property
    def decal(self):
        """Road-decal layer: D3DRS_ZWRITEENABLE := 0, drawn after the
        surface it lies on. [C] 0x00039AF5..0x00039B1B in FUN_000393C0."""
        return bool(self.flags & MAT_DECAL)

    @property
    def alpha_test(self):
        """D3DRS_ALPHATESTENABLE (RS 0x3C) := (flags & 0x010) != 0.
        [C] FUN_000393C0 @0x00039BD4.  Threshold GREATER 64/255, see
        MAT_ALPHA_REF.

        THE ONLY thing that makes a world texture's alpha channel act as a
        cut-out. Counting transparent texels is not a substitute: the class-1
        road materials are 40-77% alpha==0 and are drawn fully opaque.
        """
        return bool(self.flags & MAT_ALPHA_TEST)

    @property
    def alpha_blend(self):
        """D3DRS_ALPHABLENDENABLE (RS 0x3B) := (flags & 0x001) != 0.
        [C] FUN_000393C0 @0x00039B21.  SRC_ALPHA / ONE_MINUS_SRC_ALPHA,
        FUNC_ADD (RS 62/63/74 := 0x302/0x303/0x8006, @0x00038B06..0x00038B58;
        the NV2A blend tokens are the GL enums verbatim)."""
        return bool(self.flags & MAT_ALPHA_BLEND)

    @property
    def specular(self):
        """(strength, power, gated_by_vertex_alpha) for the classes whose
        pixel shader adds `tex.a * C0.rgb`, else None. See "Shader classes"."""
        if self.cls in MAT_CLASSES_SPECULAR:
            # Class 2 has ONE vertex program (0x003E8B10), so the flag-0x40
            # program swap that gates classes 1/7 does not apply to it; its
            # stage-0 alpha word 0xD4D81010 multiplies V0.a in unconditionally.
            gate = (True if self.cls == MAT_CLASS_MIRROR
                    else bool(self.flags & MAT_SPEC_GATE))
            return (self.spec_strength, self.spec_power, gate)
        if self.cls == MAT_CLASS_EMISSIVE:
            # class 10 adds tex.a * C0.rgb with C0.rgb = the scene light
            # UNSCALED (0x0003963F..0x0003965A leaves out the +0x04 multiply)
            # and with no vertex-alpha gate and no specular factor.
            return (1.0, 0.0, False)
        return None

    @property
    def uses_vertex_color(self):
        """Does this material's vertex declaration carry the D3DCOLOR diffuse?
        False for classes 8 and 9. See MAT_CLASSES_NO_VCOLOR."""
        return self.cls not in MAT_CLASSES_NO_VCOLOR

    @property
    def key(self):
        """MTL identity. Two materials that share a texture but differ in
        class or flags are DIFFERENT surfaces (US_C3_V1 ships GL_road5,
        GL_road7 and GL_road7grass as both a class-0 and a class-1 record),
        so they cannot share one `newmtl`."""
        return (self.texture, self.cls, self.flags)


def parse_materials(r, data):
    """material index -> Material.

    Material table pointer at +0x08, count at +0x0C. Each material is 0x28
    bytes with a pointer-to-pointer to its texture record at +0x0C; the texture
    record's name sits at +0x48 (old revision) or +0x44 (new). Class at +0x00
    and the flag word at +0x24 are read straight from the record -- see the
    module docstring for where each is consumed in the game.
    """
    mat_off = r.i32(0x08)
    mat_count = r.u16(0x0C)
    out = {}
    if not (0 < mat_off < len(data)) or not (0 < mat_count < 4096):
        return out
    # The animated-material index table (u16 material ids) at header +0x10,
    # count at +0x0E: exactly the materials FUN_0019B1E0 ticks each frame.
    animated = set()
    anim_count = r.u16(0x0E)
    anim_tbl = r.i32(0x10)
    if 0 < anim_tbl < len(data) - anim_count * 2:
        animated = {r.u16(anim_tbl + k * 2) for k in range(anim_count)}
    for i in range(mat_count):
        m = mat_off + i * MAT_STRIDE
        if m + MAT_STRIDE > len(data):
            break
        tex_ptr = r.ptr(m + 0x0C, m)
        if not (0 < tex_ptr < len(data) - 4):
            continue
        tex_rec = r.ptr(tex_ptr, m)
        if not (0 < tex_rec < len(data) - 0x70):
            continue
        bd = r.u32(tex_rec + 0x40)
        name = read_cstr(data, tex_rec + (0x48 if bd in (4, 8, 32) else 0x44))
        if name:
            s, p = struct.unpack_from('<2f', data, m + 0x04)
            rate, lim = struct.unpack_from('<2f', data, m + 0x14)
            a, = struct.unpack_from('<f', data, m + 0x20)
            nframes = data[m + 0x11]
            names, labels = read_anim_frames(r, data, m, tex_ptr, name, nframes)
            out[i] = Material(os.path.basename(name.replace('\\', '/')),
                              r.u32(m + MAT_FLAGS_OFF), r.u32(m), s, p, a,
                              rate, lim, nframes, i in animated,
                              data[m + 0x10], data[m + 0x12],
                              struct.unpack_from('<b', data, m + 0x13)[0],
                              names, labels)
    return out


def c_atol(s):
    """The C library's atol, which is what FUN_0019B1E0 calls on a texture
    name's tail: skip leading blanks, take an optional sign and then digits,
    stop at the first character that is not one. Anything else is 0."""
    i = 0
    while i < len(s) and s[i] in " \t\n\r\f\v":
        i += 1
    j = i
    if j < len(s) and s[j] in "+-":
        j += 1
    k = j
    while k < len(s) and s[k].isdigit():
        k += 1
    if k == j:
        return 0
    return int(s[i:k])


def read_anim_frames(r, data, m, tex_ptr, name0, nframes):
    """The per-frame texture names and their FUN_0019B1E0 keyframe labels.

    +0x0C does not point at one texture -- it points at an ARRAY of texture
    record pointers, one per animation frame, which the ticker indexes as
    `(*(int**)(mat+0xC))[frame]`.  The stride is 4 and the relocation is the
    same one the frame-0 pointer uses.

    The label is what the ticker's `atol` yields.  Its argument, unwound from
    the decompile, is
        pcVar4 + ((tex[nxt] + 0x48) - (rec0 + 0x49))
    where pcVar4 has been walked to one past frame 0's NUL, i.e. it is
        (rec0 + 0x48) + strlen(name0) + 1 + tex[nxt] + 0x48 - rec0 - 0x49
      =  name_of(tex[nxt]) + strlen(name0)
    -- the NEXT frame's name with as many characters skipped as frame 0's whole
    name is long.  `bk_warnsignb` is 12 long, so `bk_warnsignb12` + 12 = "12".
    The skip uses the STORED strings, so it is computed here on those, not on
    the basenames the OBJ writer emits.                                   [C]
    """
    names, labels = [], []
    skip = len(name0)
    for fi in range(max(1, nframes)):
        raw = None
        po = tex_ptr + fi * 4
        if 0 < po < len(data) - 4:
            rec = r.ptr(po, m)
            if 0 < rec < len(data) - 0x70:
                bd = r.u32(rec + 0x40)
                raw = read_cstr(data, rec + (0x48 if bd in (4, 8, 32) else 0x44))
        if raw is None:
            raw = name0
        names.append(os.path.basename(raw.replace('\\', '/')))
        # Frame 0's own label is never read: the ticker reaches frame 0 only
        # through the `nxt == 0` wrap arm, which hard-codes label 1.
        labels.append(0 if fi == 0 else c_atol(raw[skip:]))
    return names, labels


def unpack_normal(w):
    """D3DVSDT_NORMPACKED3 -> (x, y, z). [C] from the vertex declaration at
    0x0038758C; the 11/11/10 split is the one that yields |n| = 1."""
    x = (w & 0x7FF)
    y = (w >> 11) & 0x7FF
    z = (w >> 22) & 0x3FF
    if x & 0x400:
        x -= 0x800
    if y & 0x400:
        y -= 0x800
    if z & 0x200:
        z -= 0x400
    return (x / 1023.0, y / 1023.0, z / 511.0)


def read_vertices(buf, off, count):
    """The 0x1C-byte world vertex -> (px, py, pz, u, v, nx, ny, nz, r, g, b, a).

    Colour is the raw half-range D3DCOLOR scaled to 0..1 (so 128 -> 0.502);
    the renderer applies the pixel shader's SHIFTLEFT_1 doubling itself.
    """
    out = []
    for i in range(count):
        o = off + i * VTX_STRIDE
        px, py, pz = struct.unpack_from('<3f', buf, o)
        nrm, = struct.unpack_from('<I', buf, o + 0x0C)
        b, g, rr, a = buf[o + 0x10], buf[o + 0x11], buf[o + 0x12], buf[o + 0x13]
        uu, vv = struct.unpack_from('<2f', buf, o + 0x14)
        nx, ny, nz = unpack_normal(nrm)
        out.append((px, py, pz, uu, vv, nx, ny, nz,
                    rr / 255.0, g / 255.0, b / 255.0, a / 255.0))
    return out


def emit_tris(tris, two_sided):
    """Bake D3DCULL_NONE into the geometry.

    The harness renderer has one global cull state, so a two-sided material's
    triangles are emitted twice, the copy reverse-wound. Single-sided
    materials keep the game's own winding and are culled normally.
    """
    if not two_sided:
        return tris
    return tris + [(a, c, b) for a, b, c in tris]


def parse_streamed(r, data, sdata, materials, stats, cells=None):
    """Streamed road units. Returns (opaque_meshes, alpha_meshes).

    Unit table lives in static.dat at +0x54: u16 count, ptr at +0x58. Each
    0x10-byte entry: blockA offset, blockB offset, blockA size, blockB size
    (offsets into streamed.dat, +0x10 for the block header). A block's model is
    one Xbox track submodel at offset + 0x10 + 0x40; the PVS block follows it.

    The unit's material table and per-slot submesh-chain heads come out of the
    PVS block; the slot range splits into an OPAQUE pass and a later ALPHA pass
    at pvs[0x73]. See the module docstring for the byte-level citations.
    """
    unit_count = r.u16(0x54)
    unit_table = r.i32(0x58)
    sr = Reader(sdata)
    opaque, alpha = [], []
    units = 0
    for u in range(unit_count):
        e = unit_table + u * 0x10
        if e + 0x10 > len(data):
            break
        sub_off = r.i32(e)
        sub_size = r.u32(e + 8)
        # offset 0 is a legitimate block position (unit 0 lives there)
        if sub_off < 0 or not sub_size:
            continue
        base = sub_off + 0x10 + 0x40
        if base + 0x20 > len(sdata) or sr.u32(base) != 1:
            stats["skipped"] += 1
            continue
        vtx_off = sr.ptr(base + 0x04, base)
        sub2 = sr.ptr(base + 0x0C, base)
        nsub = sr.u32(base + 0x10)
        if not (0 < vtx_off < len(sdata)) or not (0 < sub2 < len(sdata)) \
           or not (0 < nsub < 4096):
            stats["skipped"] += 1
            continue

        # --- PVS block: material table, slot split, per-slot chain heads
        X = base + 0x20
        n_slots = sdata[X + PVS_NSLOT_B]
        n_opaque = sdata[X + PVS_NSLOT_OPAQUE]
        if n_opaque > n_slots:                    # never seen; stay safe
            n_opaque = n_slots
        mat_tbl = [sr.u16(X + PVS_MATTAB + k * 2) for k in range(n_slots)]
        # the chunk describing THIS unit is the one whose relative unit index
        # is 0 (FUN_0019D100 @0x0019D1F7 reads that byte); chunk 0 in every
        # C1_V1 unit, but search for it rather than assume.
        heads = None
        for chnk in range(PVS_CHUNKS):
            o = X + PVS_CHUNK0 + PVS_CHUNK_STRIDE * chnk
            if o + PVS_CHUNK_STRIDE > len(sdata):
                break
            if sr.i8(o) == 0:
                h = o + PVS_CHUNK_HEADS_A
                heads = [sr.i8(h + k) for k in range(n_slots)]
                break

        subs = []
        for k in range(nsub):
            se = sub2 + k * SUB_STRIDE
            if se + SUB_STRIDE > len(sdata):
                break
            subs.append((sr.u32(se + 0x80), sr.u32(se + 0x84),
                         sr.ptr(se + 0x88, se), sr.i8(se + SUB_NEXT_OFF)))
        if not subs or not subs[0][2]:
            stats["skipped"] += 1
            continue
        vtx_bytes = subs[0][2] - vtx_off
        if vtx_bytes <= 0 or vtx_bytes % VTX_STRIDE:
            stats["skipped"] += 1
            continue
        nverts = vtx_bytes // VTX_STRIDE
        verts = read_vertices(sdata, vtx_off, nverts)

        if cells is not None:
            # This cell's own footprint plus the static groups it names, for
            # the backdrop filter in parse_static_group().
            cells.append(dict(
                xmin=min(v[0] for v in verts), xmax=max(v[0] for v in verts),
                zmin=min(v[2] for v in verts), zmax=max(v[2] for v in verts),
                backdrop={b for b in (sr.i8(X + 0x04 + k) for k in range(8))
                          if b >= 0},
                chevron=sr.i8(X + 0x0D), water=sr.i8(X + 0x0E),
                reflection=sr.i8(X + 0x0F)))

        units += 1
        stats["models"] += 1

        # --- walk the slots in the game's own order, splitting the passes
        seen = set()
        order = []          # (submesh index, material id, is_alpha)
        if heads is not None:
            for slot, head in enumerate(heads):
                g = head
                guard = 0
                while 0 <= g < len(subs) and g not in seen and guard <= len(subs):
                    seen.add(g)
                    order.append((g, mat_tbl[slot], slot >= n_opaque))
                    if slot >= n_opaque:
                        stats["alpha_sub"] += 1
                    g = subs[g][3]
                    guard += 1
                    if guard > 1:
                        stats["chained"] += 1
        # anything the table does not reach keeps index order, ahead of the
        # mapped passes (none in C1_V1)
        for k in range(len(subs)):
            if k not in seen:
                order.insert(0, (k, None, False))
                stats["unmapped"] += 1

        for k, mat_id, is_alpha in order:
            fmt, count, off, _next = subs[k]
            if fmt != TRI_STRIP or not off or not count:
                continue
            if off + count * 2 > len(sdata):
                continue
            idx = struct.unpack_from('<%dH' % count, sdata, off)
            if max(idx) >= nverts:
                continue
            tris = strip_to_triangles(idx)
            if not tris:
                continue
            mat = materials.get(mat_id) if mat_id is not None else None
            # NOTE: the mirror-class (material class 2) skip is NOT applied
            # here. It is confirmed only for the static-group draw
            # (FUN_001AD350 @0x001AD40D); the streamed draw path has no such
            # test, and the 15 class-2 submeshes in C1_V1 are all streamed
            # (Chgo_Glass_Reflec).
            two_sided = bool(mat and mat.two_sided)
            decal = bool(mat and mat.decal)
            if mat:
                stats["textured"] += 1
            if two_sided:
                stats["two_sided"] += 1
                stats["dup_tris"] += len(tris)
            if decal:
                stats["decal"] += 1
                stats["decal_tris"] += len(tris)
            (alpha if is_alpha else opaque).append(
                ("unit_%03d_s%03d" % (u, k), verts, emit_tris(tris, two_sided),
                 mat, two_sided, decal, is_alpha))
            stats["submeshes"] += 1
    stats["units"] = units
    return opaque, alpha


def parse_static_group(r, data, materials, gi, name, stats, limit_models=None,
                       cells=None, loose=False):
    """One of the four static group tables -> list of mesh tuples.

    `cells`, when given, is the per-streamed-unit record built by
    parse_streamed(); it enables the LOCAL-CELL FILTER on the backdrop groups.

    The game does not draw all 14 backdrop groups: FUN_001AD7A0 @0x001AD976
    draws only the (up to 8) groups named by the current cell's PVS bytes
    pvsRT+0x74..0x7B. The harness bakes one display list with no per-cell
    visibility, so drawing the union puts distant-only backdrop cards in front
    of the detailed streamed geometry they stand in for. In C1_V1 the union is
    all 14 groups, and one of them -- backdrop group 2, model 1, material
    bk_downtown_bd -- is a low-resolution facade card that spans
    X[921..1573] Z[1479..1807], i.e. straight across the shop fronts of
    streamed units 5..8, 3 m in front of them. Groups 2 and 3 are named only by
    units 43..48, on the far side of the loop.

    STRICT (default): a backdrop submodel is dropped as soon as ANY streamed
    unit it overlaps omits its group -- i.e. as soon as there is a cell you
    can stand in where the game would NOT draw this card but the baked list
    does.  Submodels that overlap no unit at all are always kept (pure
    distant backdrop, nothing to occlude).

    LOOSE (--loose-backdrops, the previous behaviour): drop only when EVERY
    overlapping unit omits the group.  That is enough on AS/C1_V1 (it drops
    exactly g02 s0/s1, g03 s0, g12 s0 of the 47 backdrop submodels) but not
    on US/C3_V1, where the ground card GL_BD6 -- 17 triangles, 72,000 m2, at
    road height y=148.5..149 across the start straight -- is named by some
    overlapping units and so survives, and the baked list then paints it over
    the road you are driving on.  Pass --all-backdrops to disable filtering.
    """
    out = []
    n = r.u16(0x1C + gi * 2)
    table = r.i32(0x24 + gi * 4)
    if not n or not (0 < table < len(data)):
        return out
    for g in range(n):
        entry = table + g * 8
        if entry + 8 > len(data):
            break
        sub_count = r.u16(entry)
        mdl = r.ptr(entry + 4, entry)
        if not mdl or not (0 < mdl < len(data)) or not sub_count:
            continue
        stats["groups"] += 1
        for s in range(sub_count):
            base = mdl + s * MDL_STRIDE + 0x40
            if base + 0x20 > len(data):
                break
            if r.u32(base) != 1:
                stats["skipped"] += 1
                continue
            vtx_off = r.ptr(base + 0x04, base)
            sub_off = r.ptr(base + 0x0C, base)
            nsub = r.u32(base + 0x10)
            if not (0 < vtx_off < len(data)) or not (0 < sub_off < len(data)):
                stats["skipped"] += 1
                continue
            if not (0 < nsub < 4096):
                stats["skipped"] += 1
                continue

            # Material indices: pointer at +0x14, relative to the group entry
            # for version <= 0x30. One u16 per submesh.
            mat_idx = []
            mi_ptr = r.ptr(base + 0x14, entry)
            if 0 < mi_ptr < len(data) - nsub * 2:
                mat_idx = [r.u16(mi_ptr + k * 2) for k in range(nsub)]

            subs = []
            for k in range(nsub):
                se = sub_off + k * SUB_STRIDE
                if se + SUB_STRIDE > len(data):
                    break
                subs.append((r.u32(se + 0x80), r.u32(se + 0x84),
                             r.ptr(se + 0x88, se)))
            if not subs or not subs[0][2]:
                stats["skipped"] += 1
                continue

            # Vertices run up to the first index block.
            vtx_bytes = subs[0][2] - vtx_off
            if vtx_bytes <= 0 or vtx_bytes % VTX_STRIDE:
                stats["skipped"] += 1
                continue
            nverts = vtx_bytes // VTX_STRIDE
            verts = read_vertices(data, vtx_off, nverts)

            if cells:
                xmn = min(v[0] for v in verts)
                xmx = max(v[0] for v in verts)
                zmn = min(v[2] for v in verts)
                zmx = max(v[2] for v in verts)
                over = [c for c in cells
                        if c["xmin"] <= xmx and c["xmax"] >= xmn
                        and c["zmin"] <= zmx and c["zmax"] >= zmn]
                named = [g in c["backdrop"] for c in over]
                drop = (not any(named)) if loose else (not all(named))
                if over and drop:
                    stats["backdrop_local_skipped"] += 1
                    stats["backdrop_local_names"].append("g%02d s%d" % (g, s))
                    continue

            stats["models"] += 1
            for k, (fmt, count, off) in enumerate(subs):
                if fmt != TRI_STRIP or not off or not count:
                    continue
                if off + count * 2 > len(data):
                    continue
                idx = struct.unpack_from('<%dH' % count, data, off)
                if max(idx) >= nverts:
                    continue
                tris = strip_to_triangles(idx)
                if not tris:
                    continue
                mat = materials.get(mat_idx[k]) if k < len(mat_idx) else None
                if mat and mat.cls == MAT_CLASS_MIRROR:
                    stats["mirror_skipped"] += 1
                    continue
                two_sided = bool(mat and mat.two_sided)
                decal = bool(mat and mat.decal)
                if mat:
                    stats["textured"] += 1
                if two_sided:
                    stats["two_sided"] += 1
                    stats["dup_tris"] += len(tris)
                if decal:
                    stats["decal"] += 1
                    stats["decal_tris"] += len(tris)
                out.append(("%s_g%03d_m%03d_s%03d" % (name, g, s, k),
                            verts, emit_tris(tris, two_sided),
                            mat, two_sided, decal, False))
                stats["submeshes"] += 1
            if limit_models and stats["models"] >= limit_models:
                return out
    return out


def parse_enviro(track_dir):
    """The track's scene lighting/fog, out of `enviro.dat`.

    FUN_001888F0 loads "<track dir>/enviro.dat" (the literal at 0x003B0444,
    path built @0x001889D5) and FUN_00188C00 (called @0x00188A40) copies its
    FIRST 0xB0 BYTES verbatim over the environment object at 0x0060E040 --
    that object IS the head of the file.  Then, still in FUN_001888F0:

      0x00188A47  env+0x80 is normalised in place (FUN_00011570)
      0x00188A63  env+0x90 *= (float)(int)DAT_004D6170
      0x00188B40  a 2-iteration loop copies env fields into the two 0x40-byte
                  LIGHT RECORDS at env+0xB0 and env+0xF0 (= DAT_0060E0F0 and
                  DAT_0060E130), BOTH from the same source:
                      rec+0x00 (float4) = env+0x80    light direction
                      rec+0x10 (float4) = env+0x00    fog colour
                      rec+0x20..+0x3C   = env+0x10..+0x2C
    DAT_0060E170 (= env+0x130, immediately after the second record) is the
    record INDEX, and it is not a zone selector: FUN_001AE340 @0x001AE37F and
    FUN_001AE6F0 @0x001AE703 both write it with their own render-target index,
    and both records are initialised identically.  So a track has ONE scene
    light and ONE fog setting, per track, for the whole world.             [C]

    The world setup FUN_00038D10, which FUN_001AD350 / FUN_001AD7A0 /
    FUN_001ADA40 / FUN_001ADD60 / FUN_001AE200 all call before drawing, then
    turns those numbers into render state and vertex-shader constants:

      c[0x61] := -(rec+0x00)            @0x00038D62 (XORPS with 0x80000000
                                        on all four lanes), the light
                                        direction the class-1/2/7 vertex
                                        programs dot the normal against
      c[0x60] := (float4 at 0x004D67D0) with .y += 5.0   -- the eye position
      c[0x78] := (DAT_00549E08, 0, rec+0x20, 0)  -- the fog distance cap.
                                        The .z lane is loaded at 0x00038D8D
                                        (`MOVSS XMM1,[ESI+0x10]`, ESI =
                                        0x0060E100 + env*0x40, i.e. rec+0x20)
                                        and stored at 0x00038DD8; the float4
                                        is uploaded at 0x00038F59 by the ONLY
                                        `MOV ECX,0x78` feeding either constant
                                        uploader in the whole image.  It is
                                        the same field the next block turns
                                        into FOGSTART/FOGEND, so the cap and
                                        the ramp are in the same units.  [C]
      RS 0x8A FOGCOLOR   := D3DCOLOR(rec+0x10..0x18 * 127.5)  @0x00038E0E
      RS 0x5E FOGSTART   := rec+0x20 * 0.05                   @0x00038E10
      RS 0x5F FOGEND     := (rec+0x20 - FOGSTART) / rec+0x24 + FOGSTART
      RS 0x5D FOGTABLEMODE := 3 (D3DFOG_LINEAR)               @0x00038F23
      RS 0x5C FOGENABLE    := 1                               @0x00038F47
    and the world TEARDOWN FUN_00039140 sets FOGENABLE := 0 again
    (@0x000391C5), so the fog is scoped to the world draw: cars, traffic and
    HUD are not fogged.                                                    [C]

    Note the 127.5 (0x003B1E68), not 255: the D3DRS_FOGCOLOR the hardware
    lerps toward is HALF the colour authored in the file.  US_C3_V1 stores
    (222, 183, 139)/255 and the register gets (111, 91, 69).               [C]

    Returns None when there is no enviro.dat next to static.dat.
    """
    path = os.path.join(track_dir, "enviro.dat")
    if not os.path.isfile(path):
        return None
    with open(path, 'rb') as f:
        d = f.read(0xB0)
    if len(d) < 0xB0:
        return None
    fog = struct.unpack_from('<3f', d, 0x00)
    fog_far, fog_div = struct.unpack_from('<2f', d, 0x10)
    light_rgb = struct.unpack_from('<3f', d, 0x60)
    ldir = list(struct.unpack_from('<3f', d, 0x80))
    n = math.sqrt(sum(c * c for c in ldir))            # FUN_00011570
    if n > 1e-6:
        ldir = [c / n for c in ldir]
    fog_start = fog_far * 0.05
    fog_end = ((fog_far - fog_start) / fog_div + fog_start) if fog_div > 0.0 \
        else 0.0
    return dict(
        # the vertex-shader constant c[0x61], i.e. ALREADY NEGATED, in the
        # game's own space -- exactly the vector the class-1 program uses.
        light_dir=[-c for c in ldir],
        light_rgb=list(light_rgb),
        # the value the hardware lerps toward = authored * 0.5
        fog_rgb=[c * 0.5 for c in fog],
        fog_authored=list(fog),
        fog_far=fog_far, fog_div=fog_div,
        fog_start=fog_start, fog_end=fog_end,
        fog_enabled=fog_div > 0.0)


def parse(path, want_groups=None, limit_models=None, local_backdrops=True,
          loose_backdrops=False):
    data = open(path, 'rb').read()
    r = Reader(data)
    ver, size = r.u32(0), r.u32(4)
    if size != len(data):
        print("warning: header size 0x%X != file size 0x%X" % (size, len(data)),
              file=sys.stderr)
    if ver > 0x25:
        counts = [r.u16(0x1C + i * 2) for i in range(4)]
    else:
        raise SystemExit("version 0x%X uses the older layout; not implemented" % ver)

    materials = parse_materials(r, data)
    stats = {"groups": 0, "models": 0, "submeshes": 0, "skipped": 0,
             "materials": len(materials), "textured": 0,
             "two_sided": 0, "dup_tris": 0, "mirror_skipped": 0,
             "decal": 0, "decal_tris": 0, "alpha_sub": 0, "chained": 0,
             "unmapped": 0, "units": 0, "backdrop_local_skipped": 0,
             "backdrop_local_names": [],
             "scene": parse_enviro(os.path.dirname(path) or ".")}

    # The reflection group duplicates nearby geometry for the game's mirror
    # pass; drawn directly it z-fights the real surfaces, so skip it.
    if want_groups is None:
        want_groups = {"backdrop", "chevron", "water"}

    # The drivable road surface streams from streamed.dat next to static.dat.
    stream_opaque, stream_alpha = [], []
    cells = []
    streamed = os.path.join(os.path.dirname(path), "streamed.dat")
    if os.path.exists(streamed) and not limit_models:
        sdata = open(streamed, 'rb').read()
        stream_opaque, stream_alpha = parse_streamed(r, data, sdata, materials,
                                                     stats, cells)

    static = {}
    for gi, name in enumerate(GROUP_NAMES):
        if name in want_groups:
            static[name] = parse_static_group(
                r, data, materials, gi, name, stats, limit_models,
                cells if (name == "backdrop" and local_backdrops) else None,
                loose=loose_backdrops)

    # FRAME ORDER, from FUN_001AE340 (see the module docstring):
    #   streamed opaque -> per-cell backdrop groups -> water/reflection ->
    #   streamed alpha -> chevron group.
    # The harness bakes one display list, so the per-cell selection collapses
    # to "all of them", but the pass order is reproduced exactly.
    meshes = []
    meshes += stream_opaque
    meshes += static.get("backdrop", [])
    meshes += static.get("water", [])
    meshes += stream_alpha
    meshes += static.get("chevron", [])
    for name in GROUP_NAMES:
        if name in static and name not in ("backdrop", "water", "chevron"):
            meshes += static[name]
    return meshes, stats, ver, counts


def material_names(meshes):
    """mesh index -> `newmtl` name, one per distinct (texture, class, flags).

    A texture is NOT a material: US_C3_V1 ships GL_road5 / GL_road7 /
    GL_road7grass twice, once unshaded (class 0) and once specular (class 1),
    and several backdrop cards appear with and without D3DCULL_NONE. Keying
    the MTL on the texture name alone silently merged those, so a renderer
    reading the per-material state got whichever record came last. Distinct
    records now get `<texture>.mNN` suffixes (the first keeps the bare name,
    so nothing that only cares about the texture changes).
    """
    order, seen = [], {}
    per_tex = {}
    for m in meshes:
        mat = m[3]
        if mat is None:
            continue
        k = mat.key
        if k in seen:
            continue
        n = per_tex.get(mat.texture, 0)
        per_tex[mat.texture] = n + 1
        seen[k] = mat.texture if n == 0 else "%s.m%d" % (mat.texture, n)
        order.append((seen[k], mat))
    return seen, order


def write_obj(meshes, path, texdir="textures", scene=None):
    base = 1
    mtl_path = os.path.splitext(path)[0] + ".mtl"
    names, used = material_names(meshes)
    two_sided, decal, alpha = set(), set(), set()
    for m in meshes:
        if m[3] is None:
            continue
        n = names[m[3].key]
        if m[4]:
            two_sided.add(n)
        if m[5]:
            decal.add(n)
        if m[6]:
            alpha.add(n)
    with open(mtl_path, 'w') as f:
        f.write("# Burnout 3 materials from static.dat.\n"
                "# 'two_sided' records the game material flag +0x24 bit 0x20\n"
                "# (D3DRS_CULLMODE := D3DCULL_NONE, FUN_0003A3C0 @0x0003A674).\n"
                "# It is documentation only: those submeshes are already\n"
                "# emitted with reverse-wound duplicates in the OBJ.\n"
                "# 'decal' records material flag bit 0x400 -- the road\n"
                "# markings/overlay layer, which the game draws with\n"
                "# D3DRS_ZWRITEENABLE (render state 64) := 0\n"
                "# (FUN_000393C0 @0x00039AF5..0x00039B1B).\n"
                "# 'alpha' records that the material sat at or above the\n"
                "# streamed unit's alpha cut (PVS byte pvsRT+0x73), i.e. the\n"
                "# game draws it in the separate transparent pass\n"
                "# FUN_001ADD60 that runs after ALL opaque world geometry.\n"
                "# Submeshes are already emitted here in the game's own pass\n"
                "# and material-slot order; src/burnout3_trackmesh.c reads\n"
                "# the decal line to hoist the decal groups behind the whole\n"
                "# world, because the harness bakes one display list with no\n"
                "# per-cell visibility.\n"
                "#\n"
                "# 'class' is the material's shader class (+0x00) -- see the\n"
                "# 'Shader classes' section of tools/extract_track.py for the\n"
                "# recovered per-class colour equation.\n"
                "# 'alpha_blend' / 'alpha_test' are D3DRS_ALPHABLENDENABLE\n"
                "# (RS 0x3B) and D3DRS_ALPHATESTENABLE (RS 0x3C), which\n"
                "# FUN_000393C0 computes as flags&0x001 (@0x00039B21) and\n"
                "# flags&0x010 (@0x00039BD4).  NOTE THE ORDER: the two were\n"
                "# swapped in this file until 2026-08-12 -- see MAT_ALPHA_BLEND\n"
                "# for the render-state identification.\n"
                "# THEY ARE THE ONLY THING THAT MAKES A TEXTURE'S ALPHA MEAN\n"
                "# TRANSPARENCY. On a class 0/1/7/10 material the alpha\n"
                "# channel is a specular/emissive MASK and the surface is\n"
                "# opaque; guessing from texel statistics alpha-tested away\n"
                "# 95% of Silver Lake's dirt road (the 'blue road' bug).\n"
                "# The cut-out test itself is GREATER, 64/255 -- the world\n"
                "# setup FUN_00038D10 writes D3DRS_ALPHAFUNC := D3DCMP_GREATER\n"
                "# (@0x0003901B) and D3DRS_ALPHAREF := 0x40 (@0x00038FEE) and\n"
                "# nothing in either material apply changes them.\n"
                "# 'alpha_scalar' is material +0x20.  It is the ALPHA of\n"
                "# combiner factor C0, and every class puts it in the output:\n"
                "#   class 6 -> out.a = tex.a * alpha_scalar\n"
                "#   class 0/7 -> out.a = alpha_scalar\n"
                "#   class 1 -> out.a = tex.a * vertex.a  (C0.a unused)\n"
                "#   class 10 -> out.a = tex.a\n"
                "# The shadow and road-decal sheets are all class 6 and all\n"
                "# carry alpha_scalar 0.5..0.6, so retail draws them at half\n"
                "# strength; a renderer that blends them at full texture alpha\n"
                "# paints solid black tree shadows over the road.\n"
                "# 'vcolor 0' marks a material whose vertex declaration has no\n"
                "# D3DCOLOR register (classes 8 and 9 -- foliage, props,\n"
                "# cones): the game never reads the stored vertex colour for\n"
                "# them, so a renderer must draw them at full white.\n"
                "# 'shine <strength> <power> <gate>' is the class-1/7/10\n"
                "# additive term tex.a*(vertex.a if gate)*pow(R.V,power)*\n"
                "# light*strength; strength = material +0x04, power = +0x08,\n"
                "# gate = flag bit 0x40.\n"
                "# 'uv_scroll <rate> <step>' is the animated-material ticker\n"
                "# FUN_0019B1E0's scroll branch: phase(+0x1C) advances in\n"
                "# whole steps of +0x18 while phase+step <= rate(+0x14)*T, and\n"
                "# the material apply pushes (1-frac(phase), 0, 0) into vertex\n"
                "# shader constant 0x63, which the world vertex programs add\n"
                "# to v9.xy -- so U scrolls and V never moves.\n"
                "# 'anim_frames <n> <period> <hold0> <label0> <frame0>\n"
                "# <step> <pingpong>' plus one 'anim_frame <k> <label> <tex>'\n"
                "# per frame is the ticker's OTHER arm -- TEXTURE-FRAME\n"
                "# CYCLING, which needs no flag bit, only frame count\n"
                "# (+0x11) >= 2, and which swaps the bound texture instead of\n"
                "# offsetting the UVs. n = +0x11, period = +0x14, hold0 =\n"
                "# +0x18, label0 = +0x12, frame0 = +0x10, step = +0x13,\n"
                "# pingpong = flag bit 0x100 (unset on every shipped\n"
                "# material). <label> is the integer FUN_0019B1E0 atol()s out\n"
                "# of the frame's own texture NAME after skipping frame 0's\n"
                "# name length, and it is a KEYFRAME TIME in periods, not an\n"
                "# ordinal: frame k is held (label[k+1] - label[k]) * period\n"
                "# seconds, and the last frame one period. bk_warnsignb's\n"
                "# 1,4,5,8,9,12 is a board that reads for 3 s and blinks for\n"
                "# 1 s. Frame 0's label field is unused (the wrap arm forces\n"
                "# 1) and is written as 0.\n")
        if scene:
            f.write("#\n"
                    "# Scene lighting and fog, from this track's enviro.dat --\n"
                    "# see parse_enviro() for the byte-level citations. The\n"
                    "# light direction is ALREADY NEGATED, i.e. it is exactly\n"
                    "# vertex shader constant c[0x61], in the game's own space\n"
                    "# (a reader that mirrors the world must mirror it too).\n"
                    "# fog_rgb is the value the hardware lerps toward, i.e.\n"
                    "# the authored colour halved (FOGCOLOR := f * 127.5).\n"
                    "# The fog is linear over [fog_start, fog_end] on a fog\n"
                    "# coordinate of min(clip z, fog_far) -- the MIN comes from\n"
                    "# the world vertex programs' `min oFog, r12.z, c[120].z`\n"
                    "# -- so the fog factor never falls below\n"
                    "# (fog_end-fog_far)/(fog_end-fog_start).\n")
            f.write("# scene_light_dir %.6f %.6f %.6f\n" % tuple(scene["light_dir"]))
            f.write("# scene_light_rgb %.6f %.6f %.6f\n" % tuple(scene["light_rgb"]))
            f.write("# scene_fog_rgb %.6f %.6f %.6f\n" % tuple(scene["fog_rgb"]))
            f.write("# scene_fog_range %.4f %.4f %.4f\n"
                    % (scene["fog_start"], scene["fog_end"], scene["fog_far"]))
        f.write("\n")
        for n, mat in used:
            f.write("newmtl %s\nKd 1 1 1\nmap_Kd %s/%s.png\n"
                    % (n, texdir, mat.texture))
            f.write("# class %d\n# flags 0x%04X\n" % (mat.cls, mat.flags))
            if mat.alpha_test:
                f.write("# alpha_test 1\n")
            if mat.alpha_blend:
                f.write("# alpha_blend 1\n")
            f.write("# alpha_scalar %.4f\n" % mat.alpha_scalar)
            scroll = mat.uv_scroll
            if scroll:
                f.write("# uv_scroll %.6f %.6f\n" % scroll)
            cyc = mat.frame_cycle
            if cyc:
                n, period, hold0, label0, frame0, fstep, pp, labels = cyc
                f.write("# anim_frames %d %.6f %.6f %d %d %d %d\n"
                        % (n, period, hold0, label0, frame0, fstep, pp))
                for k in range(n):
                    f.write("# anim_frame %d %d %s/%s.png\n"
                            % (k, labels[k] if k < len(labels) else 0, texdir,
                               mat.frame_textures[k]
                               if k < len(mat.frame_textures) else mat.texture))
            if not mat.uses_vertex_color:
                f.write("# vcolor 0\n")
            spec = mat.specular
            if spec and spec[0] > 0.0:
                f.write("# shine %.4f %.4f %d\n"
                        % (spec[0], spec[1], 1 if spec[2] else 0))
            if n in two_sided:
                f.write("# two_sided 1\n")
            if n in decal:
                f.write("# decal 1\n")
            if n in alpha:
                f.write("# alpha 1\n")
            f.write("\n")

    with open(path, 'w') as f:
        f.write("# Burnout 3 track geometry\n")
        f.write("# format credit: Burnout Modding community (burnout.wiki)\n")
        f.write("# Single-sided: the game culls backfaces (see the material\n")
        f.write("# flags in tools/extract_track.py). Submeshes whose material\n")
        f.write("# is D3DCULL_NONE carry a reverse-wound duplicate of every\n")
        f.write("# triangle instead. src/burnout3_trackmesh.c enables culling.\n")
        f.write("# Face order is the game's frame order (FUN_001AE340):\n")
        f.write("#   streamed opaque, backdrop groups, water, streamed alpha,\n")
        f.write("#   chevron group.\n")
        f.write("# vt carries the game's own v with NO flip: v=0 is texel row\n")
        f.write("# 0 of the D3D surface = the top row of the extracted PNG,\n")
        f.write("# which is where GL's t=0 lands when the PNG is uploaded\n")
        f.write("# row-0-first. Do not apply a 1-v anywhere downstream.\n")
        f.write("# Extra per-vertex channels, all straight from the game's\n")
        f.write("# 0x1C-byte world vertex (see the module docstring):\n")
        f.write("#   v  x y z r g b   -- the D3DCOLOR diffuse at +0x10, scaled\n")
        f.write("#                       0..1. It is HALF RANGE: 128/255 is\n")
        f.write("#                       white, because the world pixel\n")
        f.write("#                       shaders all carry SHIFTLEFT_1 on\n")
        f.write("#                       stage 0 (rgb = 2*tex*colour).\n")
        f.write("#   vt u v a         -- the vertex ALPHA, 0 or 1: the gate\n")
        f.write("#                       for the class-1 specular term.\n")
        f.write("#   vn nx ny nz      -- the NORMPACKED3 normal at +0x0C.\n")
        f.write("mtllib %s\n" % os.path.basename(mtl_path))
        # One vertex block per source model, but a model's submeshes are no
        # longer contiguous in the emission order (the alpha pass is hoisted),
        # so remember where each model's block landed and emit an `o` line per
        # submesh so the object names stay meaningful.
        emitted = {}
        for name, verts, tris, mat, _two, _dec, _alpha in meshes:
            key = id(verts)
            off = emitted.get(key)
            f.write("o %s\n" % name)
            if off is None:
                off = base
                emitted[key] = off
                for v in verts:
                    f.write("v %.6f %.6f %.6f %.5f %.5f %.5f\n"
                            % (v[0], v[1], v[2], v[8], v[9], v[10]))
                for v in verts:
                    # v is written UNCHANGED. The game's v and GL's t are the
                    # same number: both index texel row v*height with row 0 =
                    # the first row of the surface = the top of the PNG that
                    # tools/extract_textures.py writes. A 1-v here is what put
                    # every sign in the world upside down; see the module
                    # docstring ("The V origin").
                    f.write("vt %.6f %.6f %.3f\n" % (v[3], v[4], v[11]))
                for v in verts:
                    f.write("vn %.4f %.4f %.4f\n" % (v[5], v[6], v[7]))
                base += len(verts)
            if mat is not None:
                f.write("usemtl %s\n" % names[mat.key])
            for a, b, c in tris:
                f.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n"
                        % (off + a, off + a, off + a, off + b, off + b, off + b,
                           off + c, off + c, off + c))


def main():
    # Track selection is track-agnostic: --track <id|dir|name> or B3_TRACK
    # (ids from tools/extract_tlist.py).  Positional args still work:
    #   extract_track.py <static.dat> [out.obj]
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    local_backdrops = "--all-backdrops" not in sys.argv
    loose_backdrops = "--loose-backdrops" in sys.argv
    spec = None
    for i, a in enumerate(sys.argv):
        if a == "--track" and i + 1 < len(sys.argv):
            spec = sys.argv[i + 1]
        elif a.startswith("--track="):
            spec = a.split("=", 1)[1]
    args = [a for a in args if a != spec]
    if args:
        src = args[0]
        out = args[1] if len(args) > 1 else "build/track.obj"
        track = None
    else:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import extract_tlist as tl
        track = tl.resolve(spec)
        src = os.path.join(track['dir'], "static.dat")
        out = os.path.join(tl.out_root(track), "track.obj")
        print("track %s = %s (%s)" % (track['id'], track['name'],
                                      os.path.relpath(track['dir'],
                                                      tl.GAME_DIR)))

    meshes, stats, ver, counts = parse(src, local_backdrops=local_backdrops,
                                       loose_backdrops=loose_backdrops)
    print("static.dat version 0x%X" % ver)
    print("group counts: " + ", ".join("%s=%d" % (n, c)
                                       for n, c in zip(GROUP_NAMES, counts)))
    print("parsed: %d groups, %d models, %d submeshes (%d skipped), %d units"
          % (stats["groups"], stats["models"], stats["submeshes"],
             stats["skipped"], stats["units"]))
    if not meshes:
        print("no geometry recovered", file=sys.stderr)
        return 1

    nv = sum(len(m[1]) for m in meshes)
    nt = sum(len(m[2]) for m in meshes)
    print("materials: %d in table, %d/%d submeshes textured"
          % (stats["materials"], stats["textured"], stats["submeshes"]))
    print("two-sided: %d submeshes (material flag +0x24 bit 0x20), "
          "%d duplicate tris; %d mirror-class submeshes skipped"
          % (stats["two_sided"], stats["dup_tris"], stats["mirror_skipped"]))
    print("decals   : %d submeshes / %d tris (material flag bit 0x400, "
          "ZWRITEENABLE 0)" % (stats["decal"], stats["decal_tris"]))
    print("passes   : %d streamed submeshes in the alpha pass (material slot "
          ">= pvs+0x73), %d chained, %d unmapped"
          % (stats["alpha_sub"], stats["chained"], stats["unmapped"]))
    sc = stats.get("scene")
    if sc:
        print("scene    : light dir (%.3f %.3f %.3f) colour (%.3f %.3f %.3f); "
              "fog %s rgb (%.3f %.3f %.3f) linear %.0f..%.0f cap %.0f"
              % (sc["light_dir"][0], sc["light_dir"][1], sc["light_dir"][2],
                 sc["light_rgb"][0], sc["light_rgb"][1], sc["light_rgb"][2],
                 "on" if sc["fog_enabled"] else "OFF",
                 sc["fog_rgb"][0], sc["fog_rgb"][1], sc["fog_rgb"][2],
                 sc["fog_start"], sc["fog_end"], sc["fog_far"]))
    else:
        print("scene    : no enviro.dat beside static.dat")
    print("backdrop : %d submodels dropped by the local-cell filter%s"
          % (stats["backdrop_local_skipped"],
             (" (" + ", ".join(stats["backdrop_local_names"]) + ")")
             if stats["backdrop_local_names"] else ""))
    xs = [v[0] for m in meshes for v in m[1]]
    ys = [v[1] for m in meshes for v in m[1]]
    zs = [v[2] for m in meshes for v in m[1]]
    print("geometry: %d vertices, %d triangles" % (nv, nt))
    print("bounds  : X[%.1f %.1f]  Y[%.1f %.1f]  Z[%.1f %.1f]"
          % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    write_obj(meshes, out, scene=stats.get("scene"))
    print("wrote %s (%.1f MB)" % (out, os.path.getsize(out) / 1e6))
    if track is not None:
        # mirror to the legacy paths the harness loads today
        import shutil
        for ext in (".obj", ".mtl"):
            srcf = out[:-4] + ext
            if os.path.exists(srcf):
                shutil.copyfile(srcf, os.path.join("build", "track" + ext))
        print("installed build/track.obj + build/track.mtl")
    return 0


if __name__ == "__main__":
    sys.exit(main())
