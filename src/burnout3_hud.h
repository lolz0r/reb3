/*
 * burnout3_hud.h -- the real Burnout 3 in-race HUD.
 *
 * PROVENANCE
 * ----------
 * Art: the game's own Global.txd panels (tools/extract_txd.py) and the HUD
 * font recovered from the XBE .data section (tools/extract_font.py; glyph
 * metrics in src/burnout3_font.h).  See docs/RE_FRONTEND.md sections 1-6.
 *
 * The BOOST BAR in this module is no longer eyeballed: its box size, screen
 * anchor, fill/earn/flame state machine, flame-frame sequencing and segment
 * geometry are all read out of the retail XBE (docs/RE_FRONTEND.md 6.6).
 * Every constant below carries the address it came from.  Values still
 * calibrated from a reference gameplay frame are marked [S-ref] in
 * burnout3_hud.c and are NOT in the [C] table below.
 *
 * The drawing code itself is original harness code, NOT decompiled.
 *
 * Marks: [C] confirmed (read off the binary at the cited address, and
 * re-derived independently by tools/validate_hud.py), [S] strong static
 * inference, [S-ref] reference-frame calibrated, [?] open.
 */
#ifndef BURNOUT3_HUD_H
#define BURNOUT3_HUD_H

#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== *
 * B3HUD-TABLE-BEGIN
 *   Machine-checked block.  tools/validate_hud.py parses every line of
 *   the form
 *       #define B3HUD_NAME  <value>   /-* @VA  <note> *-/
 *   and asserts <value> equals the f32 (or u32, for _VA/_SLOT/_FRAMES/
 *   _SEGMENTS/_SPARKS/_CATS/_TIERS names) that it independently reads
 *   out of build/burnout3.elf at VA.  `@bss:VA=thunk` means the storage
 *   is a runtime global whose compiled-in default comes from the C++
 *   dynamic initialiser at `thunk` (registry 0x003B3748..); the
 *   validator decodes the thunk.  Do not reformat without updating the
 *   parser.
 * ===================================================================== */

/* -- the 2D virtual screen ------------------------------------------- */
#define B3HUD_VIRT_W                640.0f   /* @0x003B1F00  bss:0x0054F388=0x00264590 */
#define B3HUD_VIRT_H                480.0f   /* @0x003B1EEC  bss:0x0054F3C8=0x00264920 */

/* ===================================================================== *
 *  THE CORNER PLATES -- POS / LAP / SPEED  (2026-08-12, HUD-fidelity)
 *
 *  RE_FRONTEND 6.6.7 left these three as "[S-ref], only their anchor
 *  slots were recovered, their box sizes come from the bound texture at
 *  runtime".  That was wrong in one important way: the draw callback
 *  0x00048430 they all install is NOT a generic sprite builder -- it is
 *  a fixed THREE-SLICE stretcher that always binds `hud_element01`
 *  (handle 0x0046093C, read at 0x00048588) and cuts it into
 *
 *      left cap   x 0 .. s0*w        u 0        .. 0.59375
 *      middle     x s0*w..(1-s1)*w   u 0.59375  .. 0.703125   (stretched)
 *      right cap  x (1-s1)*w .. w    u 0.703125 .. 1.0
 *      v          EL_V0 .. EL_V1 throughout
 *
 *  with the two cap fractions taken from the draw node at +0x20 / +0x24.
 *  Each element writes its own node box and cap pair:
 *
 *    POS   FUN_00053ED0  obj 0x003FD4A0  slot 1 (top-left)
 *          box (REF_X, REF_Y-REF_DY) size (POS_W, PLATE_H)
 *          s0 = 0                     -- NO left cap, cut at the edge
 *          s1 = CAP_R_NUM/((V1-V0)*CAP_DEN) * H/W
 *    LAP   FUN_00051650  obj 0x003FD4F8  slot 7 (top-right)
 *          box (LAP_REF_X - W, ...)   -- mirrored: cap on the LEFT
 *          s0 = CAP_L_NUM/((V1-V0)*CAP_DEN) * H/W ,  s1 = 0
 *    SPEED FUN_00059850  obj 0x003FDD48  slot 5 (bottom-right)
 *          box (-SPEED_W, -SPEED_H) from the anchor, BOTH caps
 *
 *  (V1-V0)*CAP_DEN = 0.90625*32 = 29 = the texture rows the quad covers,
 *  so a cap's width is exactly its texel width scaled to the box height:
 *  the caps keep hud_element01's aspect however wide the plate gets.
 *
 *  The plate draws under the AMBIENT 2D state -- an E8 scan of
 *  FUN_00048430's whole body (0x00048430..0x000485E1) finds only
 *  FUN_001C69C0 (flush), D3DDevice_SetTexture and FUN_001C7430 (rect
 *  batch): NO FUN_001C82E0 / FUN_001C8470 preset switch.  So it is
 *  SRC_ALPHA / ONE_MINUS_SRC_ALPHA, BLENDOP ADD, COLORWRITEENABLE
 *  0x010101, address WRAP/WRAP, exactly like the callout signs (6.7.7).
 *  Its vertex colour is the shared 2D default node colour float4 at
 *  0x0054FA00 (movaps @0x000540E1 / @0x000518E7 / @0x000599F6), still
 *  [?] in the image -- solved to {1,1,1,1} off the retail frame, see
 *  B3_PLATE_NODE_RGBA in burnout3_hud.c.
 * ===================================================================== */
#define B3HUD_EL_V0             0.03125f     /* @bss:0x0054F394=0x002646F0 */
#define B3HUD_EL_V1              0.9375f     /* @bss:0x0054F374=0x00264710 */
#define B3HUD_EL_U1             0.59375f     /* @bss:0x0054F39C=0x00264730  (copied to 0x0054F380 by 0x00264770) */
#define B3HUD_EL_U2            0.703125f     /* @bss:0x0054F36C=0x00264750  (copied to 0x0054F37C by 0x00264790) */
#define B3HUD_PLATE_H              27.0f     /* @0x003897A8  node.h  @0x0005404C */
#define B3HUD_PLATE_CAP_DEN        32.0f     /* @0x003B17C8  mulss @0x00054036 */
#define B3HUD_CAP_R_NUM            19.0f     /* @0x003A5594  mulss @0x0005996B  right cap texels */
#define B3HUD_CAP_L_NUM            38.0f     /* @0x00399654  mulss @0x00059973  left cap texels  */

#define B3HUD_POS_SLOT                 1     /* imm32@0x00052DB5  (mov eax,1 -> ecx @0x00052DBB) */
#define B3HUD_POS_W               125.0f     /* @0x003B03F4  node.w @0x0005401E */
#define B3HUD_POS_W_SPLIT         148.0f     /* @0x00389094  split-screen variant @0x00054014 */
#define B3HUD_POS_REF_X           -30.0f     /* @0x003A60AC  node.x @0x00054082 */
#define B3HUD_PLATE_REF_Y          14.0f     /* @0x003B1790  node.y @0x0005409A */
#define B3HUD_PLATE_REF_DY         13.5f     /* @0x003B1FE0  node.y @0x0005406C */
#define B3HUD_LAP_SLOT                 7     /* imm32@0x00052E4C  (mov ecx,7) */
#define B3HUD_LAP_REF_X            30.0f     /* @0x003A7964  node.x @0x0005188D */
#define B3HUD_SPEED_SLOT               5     /* imm32@0x00052DA0  (mov ecx,5) */
#define B3HUD_SPEED_W             168.0f     /* @0x00389A24  node.w @0x00059951 */
#define B3HUD_SPEED_H              27.0f     /* @bss:0x0054FCCC=0x00268100  node.h @0x00059949 */
#define B3HUD_SPEED_INV_W  0.005952381f      /* @0x003B209C  = 1/168, @0x00059960 */

/* -- the plate labels: Data/Globalus.bin, as the elements load them --- */
#define B3HUD_STR_POS       0x00001F48       /* imm32@0x00053FF6 -> entry 2002 "POS" */
#define B3HUD_STR_LAP       0x00001F4C       /* imm32@0x00051783 -> entry 2003 "LAP" */
#define B3HUD_STR_MPH       0x00001F0C       /* imm32@0x00059922 -> entry 1987 "mph" */

/* ===================================================================== *
 *  THE OPPONENT TAGS  (2026-08-12, HUD-fidelity session)
 *
 *  The rival's position ordinal floating over its roof, and the small
 *  down-pointing triangle over rivals that are further away.  This is
 *  NOT one of the 2D HUD elements of 6.6.1: it lives in the race module
 *  and is projected per car.
 *
 *    FUN_0018EB40-ish   sorts the cars by depth, then for each one that
 *                       is not the player and not hidden:
 *                         ebx = word[car+0x10D0] - 1        (its place)
 *                         world = car[+0x204][+0x30] with y raised by
 *                                 car[+0xCC0][+0x40][+0xE84]
 *                         string = globalus[TAG_STR_1ST + place-1]
 *                       and calls FUN_0018F060 (@0x0018EDCA).
 *    FUN_0018F060       projects, culls, sizes, and draws either the
 *                       ordinal text or the triangle.
 *
 *  The pass state is set once around the whole loop (@0x0018ED7C..):
 *  FUN_001C72F0 (2D begin), FUN_001C82E0(0) = blend preset 0 = plain
 *  alpha, FUN_001C83D0(0) = the default texture, FUN_001C8690(1).  So
 *  BOTH the ordinal and the triangle draw under SRC_ALPHA /
 *  ONE_MINUS_SRC_ALPHA -- the triangle is a flat 3-vertex batch through
 *  FUN_001C7C90 (@0x0018F757), no art at all.
 *
 *  Screen maths, verbatim (z = camera-space depth, W/H = viewport px):
 *      sx   = px/z * W
 *      size = clamp(((z-1)*TAG_SIZE_K + 1)/z * W * TAG_SIZE_SCALE * s,
 *                   <= TAG_SIZE_MAX)                      @0x0018F28E..
 *      sy   = py/z * H - size_y * TAG_YOFF                @0x0018F2F7
 *    text  (z <  TAG_DIST):  size = max(size, TAG_MIN_W),
 *                            size_y = max(size_y, TAG_MIN_H)  @0x0018F761
 *    tri   (z >= TAG_DIST):  half-width  = size * TAG_TRI_W,
 *                            half-height = size_y * TAG_HALF,
 *                            verts (sx-w, sy-h) (sx+w, sy-h) (sx, sy+h)
 *                            -- apex DOWN                     @0x0018F6D7
 *  and the alpha envelope
 *      a = TAG_ALPHA                                       @0x0018F7B8
 *      z > TAG_FADE_FAR : a -= (z-TAG_FADE_FAR)*TAG_FADE_RATE
 *      z < TAG_NEAR_LO  : hidden;  z < TAG_NEAR_HI : a *= (z-LO)*NEAR_K
 * ===================================================================== */
#define B3HUD_TAG_DIST             35.0f     /* @0x003B175C  comiss @0x0018F226/@0x0018F244 */
#define B3HUD_TAG_SIZE_K    0.013513514f     /* @0x003B1F4C  mulss @0x0018F292  = 1/74 */
#define B3HUD_TAG_SIZE_MAX        320.0f     /* @0x00396EB0  comiss @0x0018F2BC */
#define B3HUD_TAG_SIZE_SCALE       0.75f     /* @0x003A55F8  mulss @0x0018F2B4 */
#define B3HUD_TAG_YOFF       0.33333334f     /* @0x003B1A18  mulss @0x0018F2EF */
#define B3HUD_TAG_HALF              0.5f     /* @0x003B1684  mulss @0x0018F373 */
#define B3HUD_TAG_TRI_W             0.3f     /* @0x003B1750  mulss @0x0018F6E3 */
#define B3HUD_TAG_MIN_W            20.0f     /* @0x003A7950  comiss @0x0018F769 */
#define B3HUD_TAG_MIN_H             7.0f     /* @0x003B1708  comiss @0x0018F784 */
#define B3HUD_TAG_OPACITY             0.9f     /* @0x003A69C0  movss @0x0018F7B8 */
#define B3HUD_TAG_FADE_FAR        175.0f     /* @0x003B1CCC  comiss @0x0018F66E */
#define B3HUD_TAG_FADE_RATE        0.04f     /* @0x003B17BC  mulss @0x0018F677 */
#define B3HUD_TAG_NEAR_HI           2.7f     /* @0x003A3224  comiss @0x0018F694 */
#define B3HUD_TAG_NEAR_LO           2.0f     /* @0x003B1688  comiss @0x0018F6A1 */
#define B3HUD_TAG_NEAR_K     1.42857134f     /* @0x003B1F48  mulss @0x0018F6B2 */
#define B3HUD_TAG_STR_1ST   0x00001F24       /* imm32@0x0018EDBE -> entry 1993 "1st" */

/* -- the three HUD text shadow style blocks: FUN_0004DA90 ------------- *
 * All three carry {0,0,0,0.25} and B = 1.2; they differ only in the
 * down-right offset A.  RE_FRONTEND 6.8.5 recovered the 8-pass scheme
 * (four copies at A+-B down-right, four at +-B diagonally) from the
 * ticker row; these are the other two blocks the same init fills.     */
#define B3HUD_SHADOW_A_MED          3.0f     /* @0x003B1698 -> 0x0054F540/4 @0x0004DC23 */
#define B3HUD_SHADOW_A_BIG          4.0f     /* @0x003B1690 -> 0x0054F560/4 @0x0004DC41 */

/* -- boost bar box (FUN_00048800 reads both) ------------------------- */
#define B3HUD_BOOST_W               360.0f   /* @0x003FCAA0  mulss @0x0004880D */
#define B3HUD_BOOST_H                28.0f   /* @0x003FCAA4  mulss @0x00048822 */

/* -- boost bar anchor within the box: FUN_0004BFC0 ------------------- *
 * box.x = ref.x - anchor.x*W ; box.y = ref.y - anchor.y*H ; ref={0,0}   */
#define B3HUD_BOOST_ANCHOR_X          0.0f   /* @0x003B16E0 (=0.0); materialised by xorps @0x0004C05E */
#define B3HUD_BOOST_ANCHOR_Y          1.0f   /* @0x003B168C  movss @0x0004C056 */

/* -- anchor-slot table: FUN_00053BE0 (@0x00053C23 / @0x00053C3C) ----- *
 * pos = viewport.min + TABLE[slot]*viewport.size ; boost bar = slot 3.  */
#define B3HUD_BOOST_SLOT                 3   /* imm32@0x00052DD8  (mov ecx,3 @0x00052DD7) */
#define B3HUD_SLOT_TABLE_VA     0x003FD410   /* @0x003FD410  9 x {f32,f32} */
#define B3HUD_BOOST_SLOT_FX           0.0f   /* @0x003FD428 */
#define B3HUD_BOOST_SLOT_FY           1.0f   /* @0x003FD42C */

/* -- fill / earn / flame state machine: FUN_0004C390 ----------------- */
#define B3HUD_TIER_STEP              0.25f   /* @0x003B1730  mulss @0x0004C49E */
#define B3HUD_FILL_RISE_RATE        0.625f   /* @bss:0x0054F4A0=0x00264AA0  @0x0004CDB3 */
#define B3HUD_EARN_DELTA_THRESH     0.025f   /* @0x003B1A78  comiss @0x0004CDA5 */
#define B3HUD_FLAME_RISE_RATE         5.0f   /* @0x003B1694  mulss @0x0004CE67 */
#define B3HUD_FLAME_FALL_RATE         2.0f   /* @0x003B1688  mulss @0x0004CE2A */
#define B3HUD_EARNFLASH_RISE_RATE     3.0f   /* @0x003B1698  mulss @0x0004D0B8 */
#define B3HUD_EARNFLASH_FALL_RATE    0.75f   /* @0x003A55F8  mulss @0x0004D095 */
#define B3HUD_SPARK_RATE             16.0f   /* @0x003980F8  mulss @0x0004CE8F */
#define B3HUD_SPARKS                    20   /* imm8@0x0004D03E  (cmp eax,0x14 @0x0004D03C) */

/* -- bar-size-change animation: FUN_0004C390 ------------------------- */
#define B3HUD_TIERANIM_GROW_SFX_T     0.3f   /* @0x003B1750  FUN_00140DF0 gate */
#define B3HUD_TIERANIM_SHAKE_END      0.7f   /* @0x003B1CEC  movss @0x0004CC97 */
#define B3HUD_TIERANIM_SHAKE_A       50.0f   /* @0x003B16B8  movss @0x0004CCE7 */
#define B3HUD_TIERANIM_SHAKE_B      125.0f   /* @0x003B03F4  mulss @0x0004CCDA */
#define B3HUD_TIERANIM_GROW_GATE      1.2f   /* @0x003B1768  comiss @0x0004CD91 */
#define B3HUD_SLIDEIN_PX            150.0f   /* @0x003B16D8  movss @0x0004C468 */

/* -- flame frame sequencing: frame = (int)(rate*clock) % count -------- */
#define B3HUD_EDGE_FRAMES               41   /* imm32@0x0004A150 (mov ecx,0x29) */
#define B3HUD_EDGE_RATE              24.0f   /* @0x003A35C4  movss @0x0004A071 */
#define B3HUD_CORE_FRAMES               30   /* imm32@0x00049C89 (mov ecx,0x1E) */
#define B3HUD_CORE_RATE              30.0f   /* @0x003A7964  movss @0x00049B3C */
#define B3HUD_OVER_FRAMES               20   /* imm32@0x0004A6CC (mov ecx,0x14) */
#define B3HUD_OVER_RATE              25.0f   /* @0x003A795C  mulss @0x0004A6BE */
#define B3HUD_FLAME_RATE_DECAY       0.95f   /* @0x003A69B8  mulss @0x00049DEA */

/* -- BoostBits tread band: FUN_000496E0 ------------------------------ */
#define B3HUD_TREAD_SEG_FRAC   0.11111111f   /* @0x003B1B4C  movss @0x000496FA */
#define B3HUD_TREAD_OVERLAP    0.00555556f   /* @bss:0x0054F424=0x00264BF0 */
#define B3HUD_TREAD_HSCALE            1.0f   /* @bss:0x0054F460=0x00264C10 */
#define B3HUD_TREAD_YBASE             0.0f   /* @bss:0x0054F3B0=0x00264C30 */
#define B3HUD_TREAD_WAVE_SPEED        1.8f   /* @0x003B03EC  mulss @0x0004978A */
#define B3HUD_TREAD_WAVE_PHASE       0.25f   /* @0x003B1730 */
#define B3HUD_TREAD_WAVE_AMP          0.4f   /* @0x003B16E8  mulss @0x0004986E */
#define B3HUD_TREAD_PROFILE_MIX       0.6f   /* @0x003B16EC */
#define B3HUD_TREAD_U0               0.75f   /* @0x003A55F8  addss @0x00049852 */
#define B3HUD_TREAD_DU          0.2421875f   /* @0x003B2000  mulss @0x0004984A */
#define B3HUD_TREAD_V0        0.001953125f   /* @0x003B1E94  movss @0x00049939 */
#define B3HUD_TREAD_V1             0.0625f   /* @0x003B1C34  movss @0x00049941 */
#define B3HUD_TREAD_V2       0.123046875f    /* @0x003883F8  movss @0x000499BD */
#define B3HUD_TREAD_PROFILE_VA  0x003AB038   /* @0x003AB038  10 rows x 6 f32 */
#define B3HUD_TREAD_SEGMENTS             9   /* derived: 1/B3HUD_TREAD_SEG_FRAC */

/* -- BoostFireEdge plume: FUN_00049FD0 ------------------------------- */
#define B3HUD_EDGE_XBIAS       0.00138889f   /* @bss:0x0054F40C=0x00264E50 */
#define B3HUD_EDGE_XSHIFT      0.01111111f   /* @bss:0x0054F43C=0x00264D40 */
#define B3HUD_EDGE_YTOP         0.2142857f   /* @bss:0x0054F478=0x00264E00 */
#define B3HUD_EDGE_YSCALE             1.0f   /* @bss:0x0054F4C0=0x00264DA0 */
#define B3HUD_EDGE_STEP        0.2777778f    /* @bss:0x0054F4CC=0x00264D80 */
#define B3HUD_EDGE_STEP2       0.0444444f    /* @bss:0x0054F3E0=0x00264E20 */
#define B3HUD_EDGE_CURVE_LO     2.142857f    /* @bss:0x0054F468=0x00264DC0 */
#define B3HUD_EDGE_CURVE_HI     3.428571f    /* @bss:0x0054F3DC=0x00264DE0 */
#define B3HUD_EDGE_LEAN_A            1.15f   /* @0x00372B18  addss @0x0004A0BF */
#define B3HUD_EDGE_LEAN_B            0.15f   /* @0x00384A80 */
#define B3HUD_EDGE_TAPER            0.125f   /* @0x003B1728 */
#define B3HUD_EDGE_V0            0.015625f   /* @0x003B1A90 */
#define B3HUD_EDGE_V1                0.65f   /* @0x003A2D54 */

/* -- earn callouts: 4 categories x 4 tiers --------------------------- */
#define B3HUD_CALLOUT_TABLE_VA  0x003C8390   /* @0x003C8390  16 char* */
#define B3HUD_CALLOUT_CATS               4   /* AIR, DRIFT, ONCOMING, NEAR MISS */
#define B3HUD_CALLOUT_TIERS              4   /* GOOD, GREAT, FANTASTIC, AWESOME */

/* ===================================================================== *
 * PER-SECTION RENDER STATE  (2026-08-11, render-state session)
 *
 * FUN_0004AE40 brackets its sections with two preset selectors:
 *   FUN_001C8470(ESI=i)  texture-address preset -> texture stage 0,
 *                        TSS types 0/1 (ADDRESSU/ADDRESSV)
 *   FUN_001C82E0(ESI=i)  blend preset -> render states 62/63/74/67
 *                        (SRCBLEND / DESTBLEND / BLENDOP / COLORWRITEENABLE)
 * Both write a shadow array that FUN_001D7040 flushes through
 * FUN_001D7130 (SetRenderState) / FUN_001D7150 (SetTextureStageState) --
 * that flusher is how the shadow bases 0x0075D4A0 / 0x0075D740 and the
 * (stage + type*4) indexing were identified.
 *
 * The preset TABLES are filled by FUN_001C7150 with compiled-in
 * immediates.  `@preset:TABLE[i]` below means "index i of the table at
 * TABLE, as reconstructed by replaying FUN_001C7150's stores"; that is
 * exactly what tools/validate_hud.py does (a linear mov/store decoder over
 * 0x001C7168..0x001C72E3, no Ghidra, no emulator).
 *
 * The values are NV2A / D3D-on-Xbox tokens, which are the OpenGL blend
 * enums verbatim -- D3DBLEND_SRCALPHA = 0x0302 = GL_SRC_ALPHA,
 * D3DBLEND_INVSRCALPHA = 0x0303 = GL_ONE_MINUS_SRC_ALPHA, D3DBLEND_ONE = 1
 * = GL_ONE, D3DBLENDOP_ADD = 0x8006 = GL_FUNC_ADD; and
 * D3DTADDRESS_WRAP = 1, D3DTADDRESS_CLAMP = 3.  That self-identification
 * is what pins the render-state ids, independent of the enum numbering.
 * ===================================================================== */

/* blend preset 0 = plain alpha (the 2D pass's ambient state, restored at
 * the end of the draw); preset 1 = additive.  Both write COLORWRITEENABLE
 * = 0x010101 (RGB, no alpha channel write).                             */
#define B3HUD_BLEND_SRC_ALPHA       0x0302   /* @preset:0x004A1A90[0] SRCBLEND  */
#define B3HUD_BLEND_INV_SRC_ALPHA   0x0303   /* @preset:0x004A1AB0[0] DESTBLEND */
#define B3HUD_BLEND_ADD_SRC         0x0302   /* @preset:0x004A1A90[1] SRCBLEND  */
#define B3HUD_BLEND_ADD_DST         0x0001   /* @preset:0x004A1AB0[1] DESTBLEND */
#define B3HUD_BLENDOP_ADD           0x8006   /* @preset:0x004A1B00[0] BLENDOP   */
#define B3HUD_COLORWRITE_RGB      0x010101   /* @preset:0x004A1B34[0]           */

/* texture-address preset 1 = WRAP/WRAP (live for the PLATE), preset 0 =
 * CLAMP/CLAMP (live for every fire section).                            */
#define B3HUD_ADDRESS_WRAP               1   /* @preset:0x004A1B24[1] ADDRESSU  */
#define B3HUD_ADDRESS_CLAMP              3   /* @preset:0x004A1B24[0] ADDRESSU  */

/* -- the bar PLATE: FUN_000488A0 ------------------------------------- *
 * Two textured rects out of BoostBits, opaque white, WRAP addressing so
 * the body TILES.  Body = the unlocked run, cap = the 60 px end piece
 * whose v row selects the tier.                                        */
#define B3HUD_PLATE_CAP_FRAC   0.16666667f   /* @bss:0x0054F47C=0x00264AC0 */
#define B3HUD_PLATE_U_SCALE           6.0f   /* @0x003B1824  mulss @0x00048941 */
#define B3HUD_PLATE_V1        0.244140625f   /* @0x00388384  movss @0x000488B1 */
#define B3HUD_PLATE_VH         0.11328125f   /* @0x003B2008  addss @0x0004890D */

/* -- the EARN comet: FUN_00049E40 ------------------------------------ */
#define B3HUD_EARN_BASE              0.94f   /* @0x003B1FFC  addss @0x00049E9B */
#define B3HUD_EARN_FLAG_SCALE        0.06f   /* @0x00387C04  mulss @0x00049E97 */
#define B3HUD_EARN_X0         -0.01111111f   /* @bss:0x0054F444=0x00264C40 */
#define B3HUD_EARN_X1                0.22f   /* @0x003883C4  subss @0x00049EFE */
#define B3HUD_EARN_YTOP       -0.42857143f   /* @bss:0x0054F45C=0x00264C60 */

/* -- the CORE blobs: FUN_00049AD0 ------------------------------------ */
#define B3HUD_CORE_XBIAS       0.02222222f   /* @bss:0x0054F3E4=0x00264D60 */
#define B3HUD_CORE_BLOB_W      0.17777778f   /* @bss:0x0054F44C=0x00264CA0 */
#define B3HUD_CORE_STEP        0.08888889f   /* @bss:0x0054F450=0x00264D00 */
#define B3HUD_CORE_YTOP       -0.03571429f   /* @bss:0x0054F3F0=0x00264CE0 */
#define B3HUD_CORE_YBOT         1.28571427f  /* @bss:0x0054F418=0x00264CC0 */

/* -- the OVER streak band: FUN_0004A470 ------------------------------ */
#define B3HUD_OVER_XSHIFT      0.00833333f   /* @bss:0x0054F458=0x00264EA0 */
#define B3HUD_OVER_XBIAS       0.01666667f   /* @bss:0x0054F470=0x00264EC0 */
#define B3HUD_OVER_FADE        0.03333334f   /* @bss:0x0054F4AC=0x00264EE0 */
#define B3HUD_OVER_H           0.85714287f   /* @bss:0x0054F4B8=0x00264E70 */

/* -- the plume's third rail (the lower half the harness was missing) -- */
#define B3HUD_EDGE_V2            0.984375f   /* @0x0038845C  movss @0x0004A399 */

/* ===================================================================== *
 *  THE EVENT TICKER  (2026-08-11, ticker session; RE_FRONTEND 6.8)
 *
 *  The stacked "ONCOMING / DRIFT / NEAR MISS ..." rows with star pips at
 *  the lower left, just above the boost bar.  It is NOT a HUD element of
 *  its own: it belongs to the BOOST BAR element object 0x003FD550, whose
 *  update FUN_0004D800 runs
 *        FUN_0004C390(obj, dt)                   the bar
 *        if (obj+0x56A) FUN_0004D310(obj, dt)    the ticker
 *  FUN_0004D310 probes six category records in the player's score object
 *  and walks the live-row list; FUN_0004D130 is one probe (and builds the
 *  row's draw node through FUN_0004B1C0); 0x0004B4D0 draws one row.
 *  That closes RE_FRONTEND 6.6.7's "[?] FUN_0004B1C0, a second custom-
 *  drawn HUD box (210 x 26) whose owning element was not identified".
 * ===================================================================== */

/* -- the row's draw node: FUN_0004B1C0 ------------------------------- */
#define B3HUD_TICK_W                210.0f   /* @0x003FCBE0  mulss @0x0004B1F0 */
#define B3HUD_TICK_H                 26.0f   /* @0x003FCBE4  mulss @0x0004B202 */
#define B3HUD_TICK_DRAW_CB     0x0004B4D0    /* imm32@0x0004B269  node+0x3C */
#define B3HUD_TICK_ROW_X              4.0f   /* @bss:0x0054F3D0=0x00264FF0  ref.x @0x0004D2B0 */
#define B3HUD_TICK_ROW_STEP          26.0f   /* @bss:0x0054F49C=0x00265010  first row = base - this */
#define B3HUD_TICK_STACK_STEP        26.0f   /* @0x00397540  subss @0x0004D7D1/@0x0004D7D5 */
#define B3HUD_TICK_BASE_FRAC          0.5f   /* @0x003B1684  mulss @0x0004C13C  base = barnode.y + h*this */

/* -- row lifetime / entry / exit: FUN_0004D310 ----------------------- */
#define B3HUD_TICK_LIFE              0.85f   /* @0x0039CC00  movss @0x0004D2F5 */
#define B3HUD_TICK_FADE              0.25f   /* @0x003B1730  comiss @0x0004D579 */
#define B3HUD_TICK_FADE_RATE          4.0f   /* @0x003B1690  mulss @0x0004D57E */
#define B3HUD_TICK_SHRINK           -0.05f   /* @0x003B1A84  mulss @0x0004D58D */
#define B3HUD_TICK_SLIDE_RATE       300.0f   /* @0x003B16DC  mulss @0x0004D414 */

/* -- per-row star state: FUN_0004D130 -------------------------------- */
#define B3HUD_TICK_FLASH              2.0f   /* @0x003B1688  movss @0x0004D18E */
#define B3HUD_TICK_FLASH_FLOOR        1.0f   /* @0x003B168C  movss @0x0004D1C9 */
#define B3HUD_TICK_FLASH_RATE         5.0f   /* @0x003B1694  mulss @0x0004D1B3 */
#define B3HUD_TICK_SPIN        7.85398197f   /* @bss:0x0054F428=0x002650A0  2.5*pi rad/s */
#define B3HUD_TICK_SPIN_NM    15.70796394f   /* @bss:0x0054F400=0x002650C0  5*pi rad/s */
#define B3HUD_TICK_NM_WINDOW          5.0f   /* @0x003F73D0  divss @0x0004D21C  "Near Miss Chain Time" */
#define B3HUD_TICK_ONC_MIN          100.0f   /* @0x0004D32D  (push 0x42C80000 @0x0004D32C) */

/* -- the star pips: FUN_0004B4D0 ------------------------------------- *
 * art = `hud_boost_stars` (handle 0x00460940, bound by FUN_0004DD00): a
 * 64x32 two-frame sheet, SOLID star left / OUTLINE star right.          */
#define B3HUD_TICK_STAR              23.4f   /* @bss:0x0054F3BC=0x002650E0  quad size */
#define B3HUD_TICK_STAR_HALF          0.5f   /* @0x003B1684  mulss @0x0004BADE */
#define B3HUD_TICK_STAR_OVERLAP       6.0f   /* @0x003B1824  subss @0x0004BAD6  advance = STAR - this */
#define B3HUD_TICK_STAR_GAP          12.0f   /* @0x003B178C  addss @0x0004BAE5  after the label */
#define B3HUD_TICK_STAR_U0      0.0078125f   /* @0x003B16F4  movss @0x0004BC5A */
#define B3HUD_TICK_STAR_U1      0.4921875f   /* @0x00388458  movss @0x0004BC17 */
#define B3HUD_TICK_STAR_V0      0.015625f    /* @0x003B1A90  movss @0x0004BC5E */
#define B3HUD_TICK_STAR_V1      0.984375f    /* @0x0038845C  movss @0x0004BC05 */
#define B3HUD_TICK_PEND_U0     0.5078125f    /* @0x003B1FF4  movss @0x0004BE39 */
#define B3HUD_TICK_PEND_U1     0.9921875f    /* @0x003B1FF0  movss @0x0004BE49 */

/* -- the label: FUN_0004B280 (GlobalFont, RE_FRONTEND 6.2) ----------- *
 * glyph px = record_field * (boxW/210) * FONT_SCALE * TEXT_EM / atlas_w  */
#define B3HUD_TICK_FONT_SCALE   6.7730465f   /* @0x003C84E0  GlobalFont+0x08 */
#define B3HUD_TICK_SPACE_ADV   0.02734375f   /* @0x003C8770  GlobalFont ' ' record +0x18 */
#define B3HUD_TICK_TEXT_EM           26.0f   /* @0x00397540  mulss @0x0004B312 */
#define B3HUD_TICK_SHADOW_A           2.2f   /* @0x003B18B0 -> 0x0054F520/4 @0x0004DC05 */
#define B3HUD_TICK_SHADOW_B           1.2f   /* @0x003B1768 -> 0x0054F528/C @0x0004DACF */
#define B3HUD_TICK_SHADOW_LEVEL      0.25f   /* @0x003B1730 -> 0x0054F51C @0x0004DAB7 */

/* -- the row labels: Data/Globalus.bin, indices the constructor loads - *
 * FUN_0004BFC0 reads [[0x004D532C]+0x0C] + <byte offset>; index = /4.    */
#define B3HUD_TICK_STR_ONCOMING  0x000020F0  /* imm32@0x0004C15C -> entry 2108 */
#define B3HUD_TICK_STR_DRIFT     0x000020F4  /* imm32@0x0004C19E -> entry 2109 */
#define B3HUD_TICK_STR_NEARMISS  0x000020F8  /* imm32@0x0004C1DD -> entry 2110 */
#define B3HUD_TICK_STR_AIR       0x000020EC  /* imm32@0x0004C21C -> entry 2107 */
#define B3HUD_TICK_STR_TAILGATE  0x000020FC  /* imm32@0x0004C25B -> entry 2111 */
#define B3HUD_TICK_STR_GRINDING  0x00002100  /* imm32@0x0004C29A -> entry 2112 */
#define B3HUD_TICK_STR_RUBBING   0x00002104  /* imm32@0x0004C2D9 -> entry 2113 */

/* ===================================================================== *
 *  THE CRASH SHOW -- the "(A) IMPACT TIME" prompt and the crash TICKER
 *  (2026-08-13, crash-show session)
 *
 *  (A) THE PROMPT is a HUD element of its own: constructor FUN_000511C0
 *  (loads the "A_Button" glyph by name, string @0x003AB2E0, into
 *  obj+0x2C) and update FUN_00051230 -- a function Ghidra never made:
 *  it sits in the undefined run 0x0005122D..0x00051650 and was
 *  disassembled by hand.  The update owns three draw nodes:
 *
 *    obj+0x20  the TEXT node: Globalus 2191 "IMPACT TIME", built by the
 *              shared text-node builder FUN_000F68E0 @0x000513B2 with
 *              box width 124, style id 11, scale 1.0, anchor {1.0,0.5}
 *              (right-aligned, vertically centred) and ref y = 20.
 *    obj+0x24  the BUTTON node: TWO shapes, chosen by the live
 *              aftertouch input `a` (car+0x84, |a| clamped to 1, the
 *              ucomiss/lahf/test ah,0x44/jp compare @0x00051412):
 *                a == 0  ->  a 30x30 A_Button SPRITE (FUN_001C19A0
 *                            @0x000514CF), anchor {0.5,0.5}, centred at
 *                            (text.x - 4 - 15, 20)        <- the PROMPT
 *                a != 0  ->  a 54x36 box at (text.x - 4 - 54, 19 - 18)
 *                            with the custom callback 0x0004FCA0
 *              i.e. the (A) glyph shows while the player is NOT steering
 *              the wreck and is replaced the moment aftertouch is used.
 *    obj+0x28  a split-screen hint line, Globalus 3041, size 22, gated
 *              on [0x0073A1A4] > 1 @0x00051595.
 *
 *  (B) THE TICKER is the bottom band that reads "Into Taxi + 81ft Side
 *  Panel Skid" in the reference frame.  It is a COMPOSED string:
 *
 *    FUN_0004ED40(append, cap, wbuf)          -- the joiner
 *        if (append) wbuf += L"+ "            (@0x0038867C)
 *        FUN_0017A720(wbuf)                   -- one descriptor
 *        wbuf += L" "                         (@0x00388678)
 *    so N events read  "<d0> + <d1> + <d2> ".                       [C]
 *
 *    FUN_0017A720 formats one crash-event record `ev`:
 *        ev+0x00  the entity that was hit (type 1 only)
 *        ev+0x04  descriptor type, row of the 30-row table 0x003A2F70
 *        ev+0x08  int   count  (rolls / flips / half-turns)
 *        ev+0x0C  float value  (metres, or SECONDS for AIR)
 *    Each 16-byte row is {u32 Globalus index, u32 flags, u32 "Double
 *    ..." string, u32 "Triple ..." string} and the flags pick the
 *    substitution:
 *        &4       %1 = (int)(value * 3.28084), %2 = "ft"  (metric x1,"m")
 *        &8       %1 = sprintf("%.1f", value), %2 = "s"
 *        &1 | &2  %1 = count * 180                        (degrees)
 *        &1       count 2 -> row[2], count 3 -> row[3], else %1 = count
 *        0        the string verbatim
 *
 *    Type 1 ("Into A Vehicle") is special-cased by FUN_0017A6B0: an
 *    entity-class byte at ent+0x215 of 1/2/3 gives Globalus 2245 "Into
 *    Rival"; 4/5 looks the traffic model's 64-bit base-40 id (the pair
 *    at 0x00647B70 + model*8) up in the 40-entry key table 0x003A06F0
 *    (FUN_00158AD0) and indexes 0x003A08D0 "Into <Vehicle>"; anything
 *    else falls back to Globalus 1943 "Into Car".                   [C]
 * ===================================================================== */
#define B3HUD_CRASH_DESC_VA     0x003A2F70   /* 30 x {str,flags,dbl,tri}    */
#define B3HUD_CRASH_KEY_VA      0x003A06F0   /* 40 x {u32,u32} base-40 id   */
#define B3HUD_CRASH_NAME_VA     0x003A0830   /* 40 x u32 "<Vehicle>"        */
#define B3HUD_CRASH_INTO_VA     0x003A08D0   /* 40 x u32 "Into <Vehicle>"   */
#define B3HUD_CRASH_SEP_VA      0x0038867C   /* L"+ "  the joiner           */
#define B3HUD_CRASH_TAIL_VA     0x00388678   /* L" "   the trailer          */
#define B3HUD_CRASH_MODEL_MAX           40   /* imm8@0x00158AF9 cmp eax,0x28 */
#define B3HUD_CRASH_FT_PER_M     3.28084f    /* @0x0038A760  mulss @0x0017A7A8 */
#define B3HUD_CRASH_M_PER_M          1.0f    /* @0x003B168C  mulss @0x0017A7A8 */
#define B3HUD_CRASH_SPIN_DEG           180   /* imm8@0x0017A834 imul eax,0xB4 */
#define B3HUD_CRASH_MULTI_LO             2   /* imm8@0x0017A856 cmp ecx,2   */
#define B3HUD_CRASH_MULTI_HI             3   /* imm8@0x0017A85E cmp ecx,3   */
#define B3HUD_CRASH_STR_FT      0x00001F14   /* imm32@0x0017A789 -> 1989 "ft" */
#define B3HUD_CRASH_STR_M       0x00001F10   /* imm32@0x0017A775 -> 1988 "m"  */
#define B3HUD_CRASH_STR_S       0x00001F18   /* imm32@0x0017A7FE -> 1990 "s"  */
#define B3HUD_CRASH_STR_RIVAL   0x000008C5   /* imm32@0x0017A6C9 -> 2245 */
#define B3HUD_CRASH_STR_INTOCAR 0x00000797   /* imm32@0x0017A6FB -> 1943 */

#define B3HUD_IMPACT_STR        0x0000223C   /* imm32@0x00051372 -> 2191 */
#define B3HUD_IMPACT_HINT_STR   0x00002F84   /* imm32@0x00051609 -> 3041 */
#define B3HUD_IMPACT_TEXT_W_BITS 0x42F80000 /* imm32@0x00051355 = 124.0f */
#define B3HUD_IMPACT_TEXT_STYLE       11     /* imm8@0x0005135A  push 0xB     */
#define B3HUD_IMPACT_ANCHOR_X       1.0f     /* @0x003B168C [esp+0x58] @0x00051384 */
#define B3HUD_IMPACT_ANCHOR_Y       0.5f     /* @0x003B1684 [esp+0x60] @0x00051393 */
#define B3HUD_IMPACT_REF_Y         20.0f     /* @0x003A7950 [esp+0x5C] @0x000513AC */
#define B3HUD_IMPACT_GLYPH_W       30.0f     /* @0x003FD228 FUN_001C19A0 box   */
#define B3HUD_IMPACT_GLYPH_H       30.0f     /* @0x003FD22C                    */
#define B3HUD_IMPACT_GLYPH_GAP      4.0f     /* @0x003B1690 subss @0x00051476  */
#define B3HUD_IMPACT_GLYPH_BACK    15.0f     /* @0x003B16B4 subss @0x0005147E  */
#define B3HUD_IMPACT_BOX_W         54.0f     /* @0x003FD220 node.w @0x0005154B */
#define B3HUD_IMPACT_BOX_H         36.0f     /* @0x003FD224 node.h @0x00051556 */
#define B3HUD_IMPACT_BOX_Y         19.0f     /* @0x003A5594 movss @0x0005152B  */
#define B3HUD_IMPACT_HINT_BITS   0x41B00000 /* imm32@0x000515E7 = 22.0f  */

/* -- THE AFTERTOUCH ARROW CURSOR, FUN_0004FCA0 (crash-cinema wave) ----- *
 * The element's second node (elem+0x24) swaps the (A) glyph for a 54x36
 * box whose custom draw callback is 0x0004FCA0 (stored @0x00051583).
 * That callback splits the box into FOUR wedges around a centre point
 * and fades each one by one of the two aftertouch axes.               [C] */
#define B3HUD_AT_CB_VA        0x0004FCA0 /* @0x0004FCA0 node+0x3C callback */
#define B3HUD_AT_PRIM_VA      0x00388928 /* @0x00388928 4 prims, stride 5  */
#define B3HUD_AT_TEX_GLOBAL_VA 0x004607C0 /* @0x004607C0 "Aftertouch" slot */
/* the 7 unit vertices, (x,y) fractions of the box  @0x003FCF38..0x003FCF6C */
#define B3HUD_AT_VX_TL          0.0f     /* @0x003FCF38 v0.x               */
#define B3HUD_AT_VY_TOP         0.0f     /* @0x003FCF3C v0.y               */
#define B3HUD_AT_VX_MID         0.5f     /* @0x003FCF40 v1.x               */
#define B3HUD_AT_VX_RIGHT       1.0f     /* @0x003FCF48 v2.x               */
#define B3HUD_AT_CENTRE_X       0.5f     /* @0x003FCF50 v3.x               */
#define B3HUD_AT_CENTRE_Y       0.45f    /* @0x003FCF54 v3.y               */
#define B3HUD_AT_VY_BOTTOM      1.0f     /* @0x003FCF5C v4.y               */
/* the pulse: 1.0 - frac(clock * 2.0)^2   @0x0004FCB4..0x0004FCFF          */
#define B3HUD_AT_PULSE_RATE     2.0f     /* @0x003B1688 mulss @0x0004FCBC  */
#define B3HUD_AT_PULSE_ONE      1.0f     /* @0x003B168C subss @0x0004FCFF  */
/* colour: lerp(DIM, GLOSS, a) where a = 2x - x^2  @0x0005007A..0x000500B2 */
#define B3HUD_AT_DIM_R          0.2764706f /* @0x003FCF70                  */
#define B3HUD_AT_DIM_G          0.3627451f /* @0x003FCF74                  */
#define B3HUD_AT_DIM_B          0.5f       /* @0x003FCF78                  */
#define B3HUD_AT_DIM_A          1.0f       /* @0x003FCF7C                  */
#define B3HUD_AT_GLOSS_R        0.8f       /* @0x003FCF80 pass 2 @0x0005013E */
#define B3HUD_AT_GLOSS_G        0.7f       /* @0x003FCF84                  */
#define B3HUD_AT_GLOSS_B        0.6f       /* @0x003FCF88                  */
#define B3HUD_AT_GLOSS_A        1.0f       /* @0x003FCF8C                  */
#define B3HUD_AT_ALPHA_MIN      0.01f      /* @0x003A7ED8 comiss @0x00050168 */

/* B3HUD-TABLE-END */

/* The two crash-prompt sizes the element pushes as raw float immediates
 * (the table above checks their BIT PATTERNS against the push sites). */
#define B3HUD_IMPACT_TEXT_W       124.0f   /* = B3HUD_IMPACT_TEXT_W_BITS */
#define B3HUD_IMPACT_HINT_SIZE     22.0f   /* = B3HUD_IMPACT_HINT_BITS   */

/* ===================================================================== *
 *  THE ANCHOR VIEWPORT -- the one number that is [S-ref]
 *
 *  FUN_00053BE0 resolves an anchor slot against a VIEWPORT rect held per
 *  player at `elem[+4] + player*0x70` (+0x48/+0x4C/+0x50/+0x54):
 *      anchor = vp.min + SLOT_TABLE[slot] * vp.size            [C, 6.6.2]
 *  That rect is runtime data (the render setup writes it), so its value
 *  is not in the image.  The retail frame gives it directly, though --
 *  four independently recovered element boxes all land on the same
 *  inset, in the 640x480 virtual space of the reference capture
 *  (xemu-2026-08-12-16-23-19.png; the game renders 640x448 letterboxed
 *  into rows 16..463, so screen_y = 16 + virt_y * 448/480):
 *
 *    POS   plate right edge  = anchor.x + 95  -> measured 123  => 28
 *    LAP   plate left  edge  = anchor.x - 95  -> measured 516  => 29
 *    SPEED plate right edge  = anchor.x       -> measured ~610 => 28..30
 *    SPEED plate bottom      = anchor.y       -> measured 443  => 458 = 480-22
 *    BOOST bar left edge     = anchor.x       -> measured 30   => 28
 *    BOOST bar bottom        = anchor.y       -> measured ~443 => 458
 *
 *  i.e. a symmetric title-safe inset of 4.5% per side (0.045*640 = 28.8,
 *  0.045*480 = 21.6) -- the standard Xbox safe frame.  ONE number,
 *  [S-ref], applied through the recovered rule so every element moves
 *  together.  Set B3HUD_SAFE_FRAC to 0 to get the old flush-to-the-edge
 *  behaviour.
 * ===================================================================== */
#define B3HUD_SAFE_FRAC           0.045f  /* [S-ref] title-safe inset    */
#define B3HUD_VP_X0   (B3HUD_VIRT_W * B3HUD_SAFE_FRAC)
#define B3HUD_VP_Y0   (B3HUD_VIRT_H * B3HUD_SAFE_FRAC)
#define B3HUD_VP_SX   (1.0f - 2.0f * B3HUD_SAFE_FRAC)
#define B3HUD_VP_SY   (1.0f - 2.0f * B3HUD_SAFE_FRAC)

/* ===================================================================== *
 *  THE EA TRAX NOW-PLAYING BANNER  (2026-08-12, music session)
 *
 *  GLUE.  Deliberately outside the machine-checked block above: the
 *  banner's element object was NOT recovered, so none of these numbers
 *  carries an address and tools/validate_hud.py does not try to find them
 *  in the ELF (tools/validate_music.py checks them for self-consistency
 *  instead -- that it fits the screen, clears the boost bar's recovered
 *  28 px strip, and clears the speed cluster).
 *
 *  What IS the game's own here is the CONTENT and the STYLING VOCABULARY:
 *  the artist/title text comes from the song table at 0x003EC458 via
 *  burnout3_music.h, the plate is the same `big_curve` art the POS / LAP
 *  elements use (drawn as a mirrored pair, the way those two corners
 *  mirror each other), the glyphs are the recovered GlobalFont drawn
 *  through the same pen/shadow path as every other label, the badge is
 *  the game's own `EATrax` sheet out of Global.txd, and the blue is
 *  big_curve's own swoosh colour (99,142,214) sampled off the texture.
 *  Layout and timing are [S-ref] / GLUE -- the reference captures in
 *  REFERENCE IMAGES/ do not contain a frame with the banner up.
 * ===================================================================== */
#define B3HUD_TRAX_X                176.0f   /* GLUE box left, virtual px  */
#define B3HUD_TRAX_Y                388.0f   /* GLUE box top               */
#define B3HUD_TRAX_W                288.0f   /* GLUE  (centred: 176+288/2) */
#define B3HUD_TRAX_H                 50.0f   /* GLUE                       */
#define B3HUD_TRAX_LIFE               8.0f   /* GLUE seconds on screen     */
#define B3HUD_TRAX_IN                 0.35f  /* GLUE slide-in seconds      */
#define B3HUD_TRAX_OUT                0.60f  /* GLUE slide-out seconds     */
#define B3HUD_TRAX_SLIDE             70.0f   /* GLUE slide distance, px    */
#define B3HUD_TRAX_ICON              36.0f   /* GLUE badge size, px        */
#define B3HUD_TRAX_PAD                8.0f   /* GLUE badge inset           */
#define B3HUD_TRAX_TEXT_X            52.0f   /* GLUE text column, box-local*/
#define B3HUD_TRAX_TITLE_Y            3.0f   /* GLUE                       */
#define B3HUD_TRAX_ARTIST_Y          26.0f   /* GLUE                       */
#define B3HUD_TRAX_TITLE_SCALE        0.60f  /* GLUE                       */
#define B3HUD_TRAX_ARTIST_SCALE       0.48f  /* GLUE                       */

/* -- the bit that stays honest --------------------------------------- *
 * The boost bar's screen box is fully recovered.  The speedo / lap /
 * position boxes are NOT: their element inits go through the generic
 * sprite builder (draw callback 0x00048430) whose size comes from the
 * bound texture rather than a static pair, so only their ANCHOR SLOTS
 * were recovered ([C], docs/RE_FRONTEND.md 6.6) -- their box sizes and
 * inner text placement remain [S-ref].                                  */

/* ---- public API ----------------------------------------------------- */

/* Earn-callout categories -- the game's own table order (0x003C8390). */
enum {
    B3_HUD_CAT_AIR = 0,
    B3_HUD_CAT_DRIFT = 1,
    B3_HUD_CAT_ONCOMING = 2,
    B3_HUD_CAT_NEARMISS = 3
};

/* A generic callout slot: the takedown-FX module and the boost-earn
 * tiers both feed this.  `label` is drawn with the game's GlobalFont;
 * `art` (optional) is a GL texture drawn behind it (e.g. the hud_sign_*
 * roundels).  `t` is seconds since the event fired; <0 or > life = idle. */
typedef struct B3HudCallout {
    const char *label;   /* NULL = nothing to show                      */
    GLuint      art;     /* 0 = text only                               */
    float       t;       /* seconds since fired                         */
    float       life;    /* total on-screen time (0 => default 1.5)     */
    int         id;      /* caller's tag; unused by the renderer        */
} B3HudCallout;

/* Mirror of the game's boost record (racecar+0x119C, RE_GAMEPLAY 3).
 * The HUD derives everything it draws from these five numbers, exactly
 * as FUN_0004C390 does from score+0xFC..+0x11E. */
typedef struct B3HudBoostIn {
    int   tier;         /* score+0x0FC  0..3                            */
    float bar_size;     /* score+0x100  units (240/360/540/720)         */
    float meter;        /* score+0x104  units                           */
    float earned;       /* score+0x108  lifetime units, monotone        */
    float min_units;    /* score+0x110  units                           */
    int   boosting;     /* score+0x11E                                  */
} B3HudBoostIn;

/* ---- the EVENT TICKER --------------------------------------------- *
 * Row slots in the element object's order (obj+0x570 + i*0x28); the
 * labels are the Globalus.bin entries the constructor loads.  Retail
 * FUN_0004D310 probes 0,1,2,4,5,6 -- slot 3 (AIR) has a slot and a
 * label but is NEVER probed in the shipped build.                  [C] */
enum {
    B3_HUD_TICK_ONCOMING = 0,   /* score+0x374, shows past 100 m       */
    B3_HUD_TICK_DRIFT    = 1,   /* score+0x390                         */
    B3_HUD_TICK_NEARMISS = 2,   /* score+0x418, chain length           */
    B3_HUD_TICK_AIR      = 3,   /* slot + label exist, never probed    */
    B3_HUD_TICK_TAILGATE = 4,   /* score+0x598                         */
    B3_HUD_TICK_GRINDING = 5,   /* score+0x5C4                         */
    B3_HUD_TICK_RUBBING  = 6,   /* score+0x564                         */
    B3_HUD_TICK_ROWS     = 7
};

/* One category record exactly as FUN_0004D130 reads it -- the 0x1C-byte
 * B3CatRecord of src/burnout3_score_events.h (score+0x358/+0x374/+0x390/
 * +0x418) plus its separate "event open" flag (rec+0x10, mirrored in
 * B3ScoreEvents as air_active / onc_active / drift_active / nm_active). */
typedef struct B3HudTickIn {
    int   open;         /* rec+0x10  the *_active flag                  */
    float value;        /* rec+0x00  metres (near miss: chain links)    */
    float prev_value;   /* rec+0x08  the closed event's final value     */
    float clock;        /* rec+0x04  last-update clock (near miss only) */
    int   tier;         /* rec+0x11  current tier, -1 = none            */
    int   prev_tier;    /* rec+0x12  the closed event's tier            */
    int   count;        /* rec+0x13  number of tiers (4)                */
} B3HudTickIn;

/* ---- the EA TRAX now-playing feed ---------------------------------- *
 * Fill this straight from b3_music_now_playing().  The element is a pure
 * function of `elapsed`, so a track change (which resets elapsed to 0)
 * re-runs the whole slide-in/hold/slide-out on its own and the HUD keeps
 * no music state of its own.  artist == NULL || title == NULL => inert. */
typedef struct B3HudMusicIn {
    const char *artist;     /* band name, drawn in the HUD blue          */
    const char *title;      /* song name, drawn in white                 */
    float       elapsed;    /* seconds since this track's first sample   */
} B3HudMusicIn;

/* ---- THE CRASH SHOW: the descriptor vocabulary ---------------------- *
 * Row order of the 30-entry table at 0x003A2F70; the row index IS the
 * game's `ev+0x04` and (Globalus entry) = 1833 + row.                [C] */
enum {
    B3_HUD_CD_INTO_WALL = 0,   /* 1833 Into The Wall                     */
    B3_HUD_CD_INTO_VEHICLE,    /* 1834 Into A Vehicle -> FUN_0017A6B0    */
    B3_HUD_CD_HIT_RAMP,        /* 1835 Hit A Ramp                        */
    B3_HUD_CD_ROLLOVER,        /* 1836 Rollover                          */
    B3_HUD_CD_RIGHTSIDED,      /* 1837 Rightsided                        */
    B3_HUD_CD_BARREL_ROLL,     /* 1838 Barrel Roll x%1   (2/3 -> 2246/2249) */
    B3_HUD_CD_FRONT_FLIP,      /* 1839 Front Flip x%1    (2247/2250)     */
    B3_HUD_CD_BACK_FLIP,       /* 1840 Back Flip x%1     (2248/2251)     */
    B3_HUD_CD_HELICOPTER,      /* 1841 Helicopter %1     (count*180 deg) */
    B3_HUD_CD_CARTWHEEL,       /* 1842 Cartwheel %1                      */
    B3_HUD_CD_RODEO,           /* 1843 Rodeo %1                          */
    B3_HUD_CD_ALLEY_OOP,       /* 1844 Alley Oop %1                      */
    B3_HUD_CD_CRAZY_STYLE,     /* 1845 Crazy Style %1                    */
    B3_HUD_CD_HEAD_SPIN,       /* 1846 Head Spin %1                      */
    B3_HUD_CD_SUNROOF_SKID,    /* 1847 %1%2 Sunroof Skid   (distance)    */
    B3_HUD_CD_SKID,            /* 1848 %1%2 Skid                         */
    B3_HUD_CD_SIDE_PANEL_SKID, /* 1849 %1%2 Side Panel Skid              */
    B3_HUD_CD_NOSE_GRIND,      /* 1850 %1%2 Nose Grind                   */
    B3_HUD_CD_TAIL_GRIND,      /* 1851 %1%2 Tail Grind                   */
    B3_HUD_CD_ROOF_WRECK,      /* 1852 Roof Wreck                        */
    B3_HUD_CD_NOSE_DIVE,       /* 1853 Nose Dive                         */
    B3_HUD_CD_EXHAUST_STAND,   /* 1854 Exhaust Stand                     */
    B3_HUD_CD_WING_MIRROR,     /* 1855 Wing Mirror Crush                 */
    B3_HUD_CD_SOFT_LANDING,    /* 1856 Soft Landing                      */
    B3_HUD_CD_PAYLOAD_SPILL,   /* 1857 Payload Spill                     */
    B3_HUD_CD_AIR,             /* 1858 %1%2 Air            (SECONDS)     */
    B3_HUD_CD_BURST_FLAMES,    /* 1859 Burst Into Flames                 */
    B3_HUD_CD_EXPLODED,        /* 1860 Exploded                          */
    B3_HUD_CD_OVER_VEHICLE,    /* 1861 Over Vehicle                      */
    B3_HUD_CD_COLLECTED_PICKUP,/* 1862 Collected Pickup                  */
    B3_HUD_CD_COUNT
};

/* What FUN_0017A6B0 reads off ent+0x215 to pick "Into <what>".        [C] */
enum {
    B3_HUD_HIT_NONE = 0,    /* -> "Into Car" (the 0x797 fallback)        */
    B3_HUD_HIT_RIVAL,       /* class 1/2/3 -> 2245 "Into Rival"          */
    B3_HUD_HIT_TRAFFIC      /* class 4/5   -> the 0x003A08D0 run         */
};

/* Compose ONE descriptor exactly as FUN_0017A720 does, into `out`.
 * `type` is a B3_HUD_CD_*; `hit_kind`/`hit_id` only matter for
 * B3_HUD_CD_INTO_VEHICLE (`hit_id` is the traffic model's base-40 id,
 * e.g. "HEVYCAR23", the same string B3_TRAFFIC_CARS[].id carries);
 * `count` feeds the &1/&2 flags and `value` the &4 (metres) / &8
 * (seconds) ones.  Returns the number of characters written.        [C] */
int b3_hud_crash_descriptor(char *out, int cap, int type,
                            int hit_kind, const char *hit_id,
                            int count, float value);

/* The retail traffic-model index (0..39) for a base-40 id, or -1 --
 * the port of FUN_00158AD0 over the key table 0x003A06F0.            [C] */
int b3_hud_crash_traffic_index(const char *id);

/* The game's "<Vehicle>" / "Into <Vehicle>" name for that index.     [C] */
const char *b3_hud_crash_vehicle_name(int index);
const char *b3_hud_crash_vehicle_into(int index);

/* ---- what the harness feeds the crash show -------------------------- *
 * All-zero = inert, so an untouched B3HudState keeps the old behaviour.
 * The module runs the whole descriptor state machine off these numbers
 * (skid distance, air time, roll counting), so the harness only has to
 * hand over the wreck's pose and a couple of flags.                     */
typedef struct B3HudCrashIn {
    int   active;        /* 1 while the crash presentation is running    */
    int   aftertouch;    /* 1 when aftertouch is available (the prompt)  */
    float input;         /* live |aftertouch steer| 0..1; 0 -> (A) shows */
    /* -- additive, crash-cinema wave: the AFTERTOUCH ARROW CURSOR ------ *
     * FUN_00051230 @0x00051412 picks between the (A) glyph and the arrow
     * on pad+0x84 -- the BOOST button -- not on the steer axes, and
     * FUN_0004FCA0 lights the four wedges from the two aftertouch axes
     * veh+0x1408 / veh+0x140C.                                     [C]  */
    int   impact_held;   /* the boost/(A) button is down -> the arrow    */
    float steer_h;       /* veh+0x1408: +1 lights the RIGHT wedge        */
    float steer_v;       /* veh+0x140C: +1 lights the UP    wedge        */
    /* -- the wreck, once per frame while `active` -- */
    int   hit_kind;      /* B3_HUD_HIT_* of the thing that started it    */
    const char *hit_id;  /* traffic base-40 id, or NULL                  */
    float up_y;          /* body up vector .y   ( 1 level, -1 on roof)   */
    float right_y;       /* body right vector .y (|.|~1 = on a side)     */
    float fwd_y;         /* body forward vector .y (1 = nose up)         */
    float speed_ms;      /* wreck speed, m/s                             */
    int   airborne;      /* 1 while the wreck is off the ground          */
    int   hit_wall;      /* 1 on the frame a wall contact happened       */
} B3HudCrashIn;

typedef struct B3HudState {
    float mph;
    int   lap, total_laps;
    int   position, n_cars;
    B3HudBoostIn boost;
    B3HudCallout callout;   /* driven by the caller (takedown FX, ...)  */
    /* -- additive, 2026-08-11: the event ticker.  All-zero = inert. -- */
    B3HudTickIn ticker[B3_HUD_TICK_ROWS];
    float race_clock;       /* score+0x0C, the near-miss spin needs it  */
    /* -- additive, 2026-08-12: the EA TRAX banner.  All-zero = inert. */
    B3HudMusicIn music;
    /* -- additive, 2026-08-13: the crash show.  All-zero = inert.    */
    B3HudCrashIn crash;
} B3HudState;

/* The crash ticker's composed line for the events seen so far, i.e.
 * FUN_0004ED40's buffer: "<d0> + <d1> + ... ".  NULL when no crash is
 * running.  `newest` (may be NULL) receives the last descriptor on its
 * own -- the retail band draws that one large and white to the right of
 * the accumulated blue run.  Exposed for tools/validate_hud.py.       */
const char *b3_hud_crash_ticker(const char **newest);

/* Forget the running crash (race restart / mode change). */
void b3_hud_crash_reset(void);

/* ---- THE OPPONENT TAG (RE_FRONTEND 6.10) --------------------------- *
 * One call per rival, made by the caller between b3_hud_draw_state()
 * and the buffer swap (or anywhere in the 2D pass -- the entry point
 * saves and restores its own GL state).
 *
 * INTEGRATION -- what the harness has to supply.  The retail code does
 * the projection itself out of the camera matrix in [0x004D6520]; the
 * harness already has the same matrices, so the tag takes the projected
 * result instead of re-deriving it:
 *
 *   screen_x, screen_y  the car's TAG ANCHOR projected into the 640x480
 *                       virtual HUD space, y down.  The anchor is the
 *                       car's origin raised by its body height, i.e.
 *                       project (pos.x, pos.y + car_height, pos.z);
 *                       retail reads that height from the car model's
 *                       bounds (car[+0xCC0][+0x40][+0xE84], @0x0018ED5B).
 *                       Pass the raw perspective-divided point -- the
 *                       element applies its own -size_y/3 lift.
 *   distance            the CAMERA-SPACE DEPTH z of that point (the
 *                       view-space z, not the 3D euclidean distance;
 *                       FUN_0018F060 uses the transformed .z at
 *                       @0x0018F1E7), in world units (metres).
 *   place               the car's race position, 1-based.  Retail reads
 *                       word[car+0x10D0] and subtracts 1 to index the
 *                       "1st".."6th" Globalus run (B3HUD_TAG_STR_1ST).
 *   visible             0 = skip (crashed / hidden / the player's own
 *                       car).  Retail's own tests are byte[car+0x18FA]
 *                       (@0x0018EC8D) and "car == player" (@0x0018EC7C).
 *
 * Returns 1 if anything was drawn.  Everything else -- text vs triangle,
 * the size curve, the near/far fades, the culls -- is decided in here
 * from the recovered constants, so the caller never has to know the
 * threshold.  Draw the cars far-to-near if you want retail's ordering
 * (the loop at 0x0018EB40 depth-sorts before drawing).                */
int b3_hud_opponent_tag(float screen_x, float screen_y, float distance,
                        int place, int visible);

/* The ordinal string retail would use for `place` ("1st".."6th"), or
 * NULL when the place is outside the 8-entry Globalus run.  Exposed so
 * the validator can check the table walk without drawing.             */
const char *b3_hud_place_ordinal(int place);

/* Load one PNG as a GL texture (RGBA, linear, clamped). 0 on failure. */
GLuint b3_hud_load_texture(const char *path);

/* Load the HUD texture set from <dir> (normally "build/frontend").
 * Returns the number of textures loaded (0 = nothing found / no HUD). */
int b3_hud_init(const char *dir);

/* Full overlay, binary-derived boost bar included. */
void b3_hud_draw_state(const B3HudState *st, float dt_s);

/* Fire a boost-earn callout ("GREAT NEAR MISS!" ...).  cat is one of
 * B3_HUD_CAT_*, tier 0..3 (the FUN_00192D20 category tier).  The strings
 * are the game's own (table 0x003C8390). */
void b3_hud_boost_event(int cat, int tier);

/* The game's callout string for (cat, tier), or NULL. */
const char *b3_hud_callout_text(int cat, int tier);

/* The ticker row label (the Globalus.bin string), or NULL. */
const char *b3_hud_tick_label(int row);

/* The EA TRAX banner's on-screen box for a given `elapsed`, in the
 * 640x480 virtual screen: fills x/y/w/h and returns the element alpha
 * (0 = not on screen).  Exposed so tools/validate_music.py can assert the
 * banner never collides with the boost bar or the speed cluster without
 * having to re-implement the animation.                                  */
float b3_hud_music_box(float elapsed, float *x, float *y,
                       float *w, float *h);

/* Back-compat shim: the pre-existing call site in burnout3_full.c.
 * boost_frac is treated as meter/bar_size at tier 3. */
void b3_hud_draw(float mph, float boost_frac, int lap, int total_laps,
                 int position, int n_cars, float dt_s);

/* -- pause-screen mixer overlay (harness UI, no retail counterpart) ----- */
#define B3HUD_MIX_X      170.0f   /* label column, 640x480 virtual px      */
#define B3HUD_MIX_BAR_X  260.0f   /* slider bar left edge                  */
#define B3HUD_MIX_W      300.0f   /* slider bar width                      */
#define B3HUD_MIX_Y0     200.0f   /* first row top                         */
#define B3HUD_MIX_DY      46.0f   /* row pitch                             */
#define B3HUD_MIX_H       18.0f   /* bar height                            */
#define B3HUD_MIX_BTN_X  260.0f   /* RESTART RACE button rect              */
#define B3HUD_MIX_BTN_Y  356.0f
#define B3HUD_MIX_BTN_W  180.0f
#define B3HUD_MIX_BTN_H   26.0f
/* vals = three 0..1 slider fractions (engine, sfx, music). */
void b3_hud_pause_mixer(const float vals[3]);

/* Individual pieces (kept for the old header's API surface). */
void b3_hud_draw_quad(GLuint tex, float x, float y, float w, float h,
                      float alpha);
void b3_hud_draw_speedo(float mph, float x, float y, float scale);
void b3_hud_draw_boost_bar(float frac, float x, float y, float w, float h);
void b3_hud_draw_lap_position(int lap, int total_laps,
                              int position, int n_cars,
                              float x, float y, float scale);

void b3_hud_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_HUD_H */
