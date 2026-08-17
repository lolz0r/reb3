/* THE PARTICLE FX ENGINE -- see burnout3_particlefx.h for the whole
 * evidence story (tables, addresses, the analytic update law, the
 * distance-based emission law with its dither carry, the three blend
 * modes and the four drivers).
 *
 * The three tables below are transcribed from the retail image; the
 * mixer/renderer around them is original harness code, not decompiled.
 */
#include "burnout3_particlefx.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== *
 * B3PFX-DESC-BEGIN -- the 24 FX descriptors at 0x004182A0, stride 0x80.
 * Record 0 is in .data; 1..23 are written by the straight-line
 * initialiser 0x0025EE20..0x002610FA and were recovered by executing
 * it.  Every number here is read off those bytes.                   [C]
 * ===================================================================== */
static const B3PfxDesc B3_PFX_DESC[B3_PFX_DESCS] = {
/*  tex                 kind bl  life  midT   col0        col1        col2        size0 size1 size2  grav  cone capA capB maxpx minpx
 *  colours are 0xAABBGGRR -- RED IS THE LOW BYTE (see the header)   [C] */
{ "fxdebris1",          3, 0,  2.50f, 0.800f, {0xff606060, 0xff606060, 0x40606060}, {0.35f,0.35f,0.35f}, -12.00f, 0.1f, 100, 200,  32.f, 1.5f },
{ "fxdebris2",          5, 0,  2.50f, 0.800f, {0xff606060, 0xff606060, 0x40606060}, {0.35f,0.35f,0.35f}, -12.00f, 0.1f, 100, 200,  32.f, 1.5f },
{ "fxgravel",           1, 0,  0.50f, 0.010f, {0x0090b0c0, 0xff90b0c0, 0x4090b0c0}, {0.60f,0.60f,0.60f}, -25.00f, 0.1f,  60,   0, 112.f, 1.5f },
{ "fxsnow",             3, 0,  0.50f, 0.020f, {0x10ffffff, 0x80ffffff, 0x00ffffff}, {0.40f,0.80f,2.50f},  -4.90f, 0.1f, 100,   0, 112.f, 2.0f },
{ "fxglass",            5, 0,  3.50f, 0.900f, {0xc0ffffff, 0xc0ffffff, 0x00ffffff}, {0.22f,0.22f,0.22f},  -9.80f, 0.1f, 180, 540, 112.f, 1.5f },
{ "fxglass",            5, 0,  3.50f, 0.900f, {0x961964ff, 0x961964ff, 0x001964ff}, {0.16f,0.16f,0.16f},  -9.80f, 0.1f,  80, 160, 112.f, 1.5f },
{ "fxsmoke",            4, 0,  4.00f, 0.100f, {0x00e0e0e0, 0x70e4e4e4, 0x00ffffff}, {0.60f,1.00f,3.00f},  +0.40f, 0.1f, 120, 150, 112.f, 2.0f },
{ "fxsmoke",            4, 0,  0.50f, 0.080f, {0x00ffffff, 0xe0ffffff, 0x00ffffff}, {0.40f,0.40f,1.00f},  +0.10f, 0.0f, 200,   0,  96.f, 4.0f },
{ "fxsmoke",            4, 0,  3.00f, 0.020f, {0x00c0c0c0, 0xe0c0c0c0, 0x00ffffff}, {0.35f,0.75f,3.50f},  +0.10f, 0.0f, 240, 240, 112.f, 4.0f },
{ "fxsmoke",            4, 0,  0.80f, 0.200f, {0x00ffffff, 0xa0ffffff, 0x00ffffff}, {1.50f,2.80f,4.00f},  +0.10f, 0.0f,   0, 240, 128.f, 4.0f },
{ "fxsmoke",            4, 0,  5.00f, 0.100f, {0x0ac8d7dc, 0xb4c8d7dc, 0x00f0ffff}, {0.55f,4.00f,9.00f},  +0.05f, 0.0f, 180, 220, 125.f, 2.0f },
{ "fxsmoke",            4, 0,  1.25f, 0.025f, {0x604b7d8c, 0xc0648c96, 0x0082b4d2}, {0.65f,2.50f,4.00f},  -1.50f, 0.0f, 240,   0, 112.f, 4.0f },
{ "fxsmoke",            4, 0,  2.50f, 0.030f, {0x00a0aac0, 0xe0a0a0c0, 0x00a0a0c0}, {0.75f,1.20f,4.00f},  +0.10f, 0.0f, 240,   0, 112.f, 4.0f },
{ "fxsmoke",            4, 0,  1.20f, 0.120f, {0x00b8b0a8, 0x60b8b0a8, 0x00b8b0a8}, {0.10f,0.20f,0.30f},  +0.28f, 0.0f, 240,   0, 112.f, 4.0f },
{ "fxexplosionsmoke",   1, 1,  1.80f, 0.050f, {0x10ffffff, 0xffffffff, 0x00ffffff}, {1.00f,3.00f,3.50f},  +0.90f, 0.0f,   0, 120, 128.f, 4.0f },
{ "fxexplosionsmoke",   1, 0,  3.00f, 0.250f, {0xffc0ffff, 0x80183060, 0x00000000}, {0.25f,1.75f,3.50f},  +0.60f, 0.0f,   0, 200, 160.f, 4.0f },
{ "fxfire",             1, 2,  1.20f, 0.140f, {0xffffffff, 0xff4080c0, 0x00081080}, {0.08f,0.90f,0.80f},  +2.80f, 0.0f,   0, 150,  96.f, 4.0f },
{ "fxexplosionsmoke",   1, 0,  1.80f, 0.500f, {0xff000000, 0xa0ffffff, 0x00ffffff}, {0.20f,1.50f,3.00f},  +0.90f, 0.0f,   0, 120, 112.f, 4.0f },
{ "fxexplosionsmoke",   1, 0,  0.80f, 0.250f, {0xffc0ffff, 0x80183060, 0x00000000}, {0.25f,1.75f,3.50f},  +0.90f, 0.0f,   0, 200, 160.f, 4.0f },
{ "fxfire",             1, 2,  0.50f, 0.080f, {0xffffffff, 0xff4070c0, 0x00081080}, {0.20f,2.40f,2.00f},  +4.20f, 0.0f,   0, 240,  96.f, 4.0f },
{ "fxexplosionsmoke",   1, 0,  0.70f, 0.200f, {0xff000000, 0xa0ffffff, 0x00ffffff}, {0.50f,2.50f,3.50f},  +0.90f, 0.0f,   0, 120, 112.f, 4.0f },
{ "fxexplosionfire",    1, 0,  1.50f, 0.250f, {0x80ffffff, 0xff90f0ff, 0x00001060}, {3.50f,4.00f,3.50f},  +2.00f, 0.0f,   0, 200, 192.f, 4.0f },
{ "fxexplosionsmoke",   4, 0,  3.00f, 0.060f, {0xff70b0ff, 0x80203040, 0x00606060}, {4.00f,0.40f,4.00f},  +1.00f, 0.0f,   0, 200, 160.f, 4.0f },
{ "fxexplosionflash",   0, 2,  0.12f, 0.200f, {0xffffffff, 0x40ffffff, 0x0080ffff}, {8.00f,3.00f,1.00f},  +1.50f, 0.0f,   0,  50, 256.f, 4.0f }
};
/* B3PFX-DESC-END */

/* ===================================================================== *
 * B3PFX-EMIT-BEGIN -- the 26 emitter types at 0x003A3648, stride 0x38.
 * `sys` (+0x00) and `rate` (+0x04) and the two stream weights
 * (+0x30/+0x34) are [C]; the six v[] slots are the ten random-range
 * floats at +0x08..+0x2C, whose ADDRESSES are [C] and whose
 * per-component meaning is [S].
 * ===================================================================== */
static const B3PfxEmitter B3_PFX_EMIT[B3_PFX_EMITTERS] = {
/*  sys     rate         v0       v1       v2       v3       v4       v5     szLo   szHi   aLo    aHi    strmA  strmB */
{  8,     4.0f, {    0.0f,    0.1f,    0.6f,   0.25f,   0.25f,   0.04f},   0.8f,  1.0f,  0.6f,  0.8f,   1.0f,  0.5f },
{  7,    14.0f, {    0.0f,    0.1f,    0.6f,   0.25f,    2.0f,   0.04f},   1.0f,  1.0f,  0.6f,  1.0f,   1.0f,  0.0f },
{  8,     4.0f, {    0.0f,    0.1f,    0.6f,   0.25f,   0.25f,   0.04f},   0.8f,  1.0f,  0.5f,  0.8f,   0.4f,  0.6f },
{ 10,     3.0f, {    0.0f,    0.2f,    2.0f,   0.25f,   0.25f,   0.04f},   1.0f,  1.0f,  0.4f,  0.7f,   1.0f,  0.5f },
{ 11,     1.6f, {    0.0f,    0.2f,    1.5f,   0.25f,   0.25f,   0.04f},   0.9f,  1.0f,  0.7f,  1.0f,   1.0f,  0.0f },
{ 12,     0.0f, {    0.0f,    0.5f,    3.0f,    4.0f,   0.25f,    0.0f},   0.9f,  1.0f,  1.0f,  1.0f,   1.0f,  0.0f },
{  3,     2.0f, {    0.0f,    0.1f,    0.5f,   0.25f,    1.0f,   0.15f},   0.8f,  1.0f,  0.9f,  1.0f,   1.0f,  0.0f },
{  2,     5.0f, {    0.0f,    0.3f,    2.0f,    0.8f,    1.0f,    0.2f},   0.3f,  1.0f,  0.9f,  1.0f,   1.0f,  0.0f },
{ 10,     6.0f, {    0.0f,   0.15f,    0.4f,   0.25f,    0.4f,    0.0f},   0.8f,  1.0f,  0.9f,  1.0f,   0.3f,  0.7f },
{  9,     0.0f, {    0.0f,   0.15f,    0.4f,   0.25f,    0.4f,    0.0f},   0.8f,  1.0f,  0.9f,  1.0f,   0.0f,  1.0f },
{ 13,     0.0f, {    0.0f,    1.0f,    0.2f,   0.15f,   0.55f,    0.0f},   0.8f,  1.0f,  0.9f,  1.0f,   1.0f,  0.0f },
{  6,     0.0f, {    0.0f,    0.1f,    0.4f,   0.25f,   0.25f,    0.0f},   0.8f,  1.0f,  0.9f,  1.0f,   0.3f,  0.7f },
{  4,     0.0f, {    0.0f,    0.7f,   10.0f,   10.0f,    9.0f,    0.0f},   0.2f,  1.0f,  1.0f,  1.0f,  0.25f, 0.75f },
{  5,     0.0f, {    0.0f,    0.9f,   10.0f,   10.0f,    3.0f,    0.0f},   0.2f,  0.8f,  1.0f,  1.0f,  0.33f, 0.67f },
{  0,     0.0f, {    0.0f,    0.7f,   12.0f,   12.0f,    8.0f,    0.0f},   0.1f,  1.0f,  1.0f,  1.0f,  0.33f, 0.67f },
{  1,     0.0f, {    0.0f,    0.7f,   12.0f,   12.0f,    8.0f,    0.0f},   0.1f,  1.0f,  1.0f,  1.0f,  0.33f, 0.67f },
{  0,     0.0f, {    0.0f,    0.7f,   14.0f,   14.0f,   10.0f,    0.0f},   0.1f,  0.4f,  1.0f,  1.0f,  0.33f, 0.67f },
{  1,     0.0f, {    0.0f,    0.7f,   14.0f,   14.0f,   10.0f,    0.0f},   0.1f,  0.4f,  1.0f,  1.0f,  0.33f, 0.67f },
{  8,     6.0f, {    0.0f,    1.0f,   0.25f,    0.0f,   0.25f,   0.01f},   0.9f,  1.0f,  0.7f,  0.9f,   1.0f,  0.0f },
{ 11,     2.0f, {    0.0f,    1.0f,    0.1f,    0.0f,   0.25f,   0.05f},   0.9f,  1.0f,  0.7f,  0.9f,   1.0f,  0.0f },
{ 14,     5.0f, {    1.5f,   0.25f,    0.5f,    0.1f,    0.5f,    0.0f},   0.9f,  1.0f,  0.9f,  1.0f,   0.0f,  1.0f },
{ 19,    16.0f, {    1.0f,   0.25f,    2.5f,    0.1f,    1.0f,    0.0f},   0.5f,  1.0f,  0.9f,  1.0f,   0.0f,  1.0f },
{ 20,     8.0f, {    1.0f,   0.25f,    1.5f,    0.1f,    1.0f,    0.0f},   0.5f,  1.0f,  0.9f,  1.0f,   0.0f,  1.0f },
{ 14,     8.0f, {    1.0f,   0.25f,    0.5f,    0.1f,    0.5f,    0.0f},   0.3f,  0.6f,  0.9f,  1.0f,   0.0f,  1.0f },
{ 19,    20.0f, {    0.5f,   0.25f,    2.0f,    0.1f,    1.0f,    0.0f},   0.2f,  0.6f,  0.9f,  1.0f,   0.0f,  1.0f },
{ 20,    12.0f, {    0.8f,   0.25f,    1.5f,    0.1f,    1.0f,    0.0f},   0.2f,  0.6f,  0.9f,  1.0f,   0.0f,  1.0f }
};
/* B3PFX-EMIT-END */

/* ===================================================================== *
 * B3PFX-SURF-BEGIN -- the 40 surface rows at 0x003A3BF8, stride 0x10.
 * Every row's byte +0x0F equals its own index, which is what validates
 * the stride and the base.  `emit` 0x1A means "no wheel FX".        [C]
 * ===================================================================== */
#define N B3_PFX_NONE
static const B3PfxSurface B3_PFX_SURF[B3_PFX_SURFACES] = {
/*  0 */ {    1.0f,    0.0f,    0.0f,  N, 0 },
/*  1 */ {    1.0f,    0.0f,    0.0f,  N, 0 },
/*  2 */ {    1.0f,    0.0f,    0.0f,  N, 0 },
/*  3 */ {    1.0f,    0.1f,    0.0f,  N, 0 },
/*  4 */ {    0.5f,    0.1f,    1.0f,  7, 1 },
/*  5 */ {    0.0f,    0.1f,    1.0f,  7, 0 },
/*  6 */ {    1.0f,    0.0f,    0.0f,  N, 0 },
/*  7 */ {    0.5f,    0.0f,    0.0f,  6, 0 },
/*  8 */ {    1.0f,    0.0f,    0.0f,  N, 0 },
/*  9 */ {    1.0f,    0.0f,    0.0f,  N, 0 },
/* 10 */ {    1.0f,    0.0f,    0.0f,  N, 0 },
/* 11 */ {    0.2f,    0.0f,    0.0f,  6, 1 },
/* 12 */ {    0.8f,    0.2f,    0.0f,  N, 0 },
/* 13 */ {    0.8f,    0.3f,    0.0f,  N, 0 },
/* 14 */ {    0.0f,    0.0f,    1.0f,  7, 1 },
/* 15 */ {    0.0f,    0.1f,    0.8f,  7, 0 },
/* 16 */ {    0.0f,    0.3f,    1.0f,  7, 0 },
/* 17 */ {    0.8f,    0.2f,    0.0f,  N, 0 },
/* 18 */ {    0.0f,    0.5f,    0.5f,  N, 0 },
/* 19 */ {    0.2f,    0.0f,    0.0f,  6, 0 },
/* 20 */ {    0.8f,    0.2f,    0.0f,  N, 0 },
/* 21 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 22 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 23 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 24 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 25 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 26 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 27 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 28 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 29 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 30 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 31 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 32 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 33 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 34 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 35 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 36 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 37 */ {    0.0f,    0.0f,    0.0f,  N, 0 },
/* 38 */ {    0.5f,    0.5f,    0.0f,  N, 0 },
/* 39 */ {    0.0f,    0.0f,    0.0f,  N, 0 }
};
#undef N
/* B3PFX-SURF-END */

/* The second, "popcorn" table at 0x003EADE8, stride 0x50, 4 records --
 * colours as NORMALISED floats (x255 at 0x0017E9C3+).  Emitter is
 * FUN_00183140.  Record 2 is the light-grey CONCRETE DUST the impact
 * burst uses; record 0 is the spark.  The colours and the gravities are
 * [C]; the tabulated `life` values (0.012 / 0.011 / 0.0055 / 0.014)
 * are far too small to be seconds and their unit was not pinned, so
 * the lifetimes below are TUNED and the ratios preserved.        [C/?] */
typedef struct {
    const char *tex;
    float rgba0[4], rgba1[4];
    float grav, life;
    int   cap;                       /* the record's own capacity     [C] */
} B3PfxPopcorn;
static const B3PfxPopcorn B3_PFX_POP[4] = {
    { "fxpopcornspark",  {1.90f,0.80f,0.40f,1.00f}, {0.70f,0.20f,0.05f,0.50f}, -14.7f, 0.42f, 900 },
    { "fxpopcorndebris", {0.07f,0.06f,0.02f,1.00f}, {0.07f,0.06f,0.02f,0.50f}, -14.7f, 0.39f, 900 },
    { "fxpopcorndebris", {0.87f,0.86f,0.82f,0.65f}, {0.87f,0.86f,0.82f,0.25f}, -24.5f, 0.20f, 300 },
    { "fxpopcorndebris", {0.137f,0.137f,0.137f,0.80f}, {0.137f,0.137f,0.137f,0.50f}, -24.5f, 0.49f, 900 }
};

/* The three crash-trail emitter ids, from the literal array at
 * 0x0041A514 that FUN_00186D50 indexes.                             [C] */
static const int B3_PFX_SLIDE_IDS[3] = { 20, 21, 22 };
/* FUN_00186D50's own constants. */
#define B3_PFX_SLIDE_WINDOW   5.0f   /* @0x003B1694 */
#define B3_PFX_SLIDE_HOLD     2.5f
#define B3_PFX_SLIDE_FADE_A   0.4f   /* @0x003B16E8 */
#define B3_PFX_SLIDE_FADE_B   0.9f   /* @0x003A69C0 */
/* FUN_00181610's plume window. */
#define B3_PFX_PLUME_WINDOW  14.0f
#define B3_PFX_PLUME_EMIT      11
/* FUN_00181130's budgets. */
#define B3_PFX_BURST_GLASS_ID   12
#define B3_PFX_BURST_TINT_ID    13
#define B3_PFX_BURST_GLASS_N  100.0f
#define B3_PFX_BURST_TINT_N    20.0f
#define B3_PFX_BURST_CAP       25.0f
/* FUN_001807C0's fixed wheel emitters, in gate-byte order:
 *   wheel+0xA0 -> 0 (@0x00180CDF, and 1 @0x00180D44 when veh+0x215==1)
 *   wheel+0xA1 -> 3 (@0x00180DDF)
 *   wheel+0xA2 -> 4 (@0x00180E7A)                                   [C] */
static const int B3_PFX_WHEEL_IDS[4] = { 0, 1, 3, 4 };
static const int B3_PFX_GATE_IDS[3]  = { 0, 3, 4 };
#define B3_PFX_WHEEL_EXTRA_ID   1      /* the veh+0x215==1 companion   */
/* FUN_001805B0, the gate-byte slew. */
#define B3_PFX_GATE_FULL     255.0f    /* @0x003B16C4 */
#define B3_PFX_GATE_SLEW     510.0f    /* @0x003B1840 -- 0..255 in 0.5s */
#define B3_PFX_GATE_ROUND      0.5f    /* @0x003B1684 */
/* FUN_001807C0's per-call weights, @0x00180C77..0x00180CB0. */
#define B3_PFX_GATE_RATE_K    0.25f    /* @0x003B1730 */
#define B3_PFX_GATE_RATE_B    0.75f    /* @0x003A55F8 */
#define B3_PFX_GATE_INTEN_K    2.0f    /* @0x003B1688 */
/* FUN_00181A80's emission constants. */
#define B3_PFX_RATE_JITTER_LO  0.875f  /* @0x0039922C */
#define B3_PFX_RATE_JITTER_SP  0.25f   /* @0x003B1730 */
#define B3_PFX_STREAM_A_K      0.75f   /* @0x003A55F8 */
/* FUN_00034130's size scale + the two SCREEN limits.  Both limits are
 * applied to the pixel HALF-extent (@0x000346B9 / @0x000346F1), so in
 * this port's full-width pixels they are max_px*0.435*2 and min_px*2. */
#define B3_PFX_SIZE_K          0.9f
#define B3_PFX_MAXPX_K         0.435f
#define B3_PFX_HALF_TO_FULL    2.0f
#define B3_PFX_SQRT2           1.41421356f  /* @0x003A34B8, kinds 3/4  */
/* The recovered alphas are authored for retail's own x2 present pass,
 * which this port now runs by default, so they go in verbatim.  Set
 * B3_PFX_LEVEL below 1.0 only as a deliberate look deviation. */
#define B3_PFX_LEVEL           1.00f

/* ===================================================================== *
 * runtime
 * ===================================================================== */

typedef struct {
    float p[3], v[3];
    float birth, life, grav;
    float size_mul, alpha_mul;
    float spin, spin_rate;
    unsigned char desc;
    unsigned char pop;      /* 1 => `desc` indexes B3_PFX_POP           */
    unsigned char live;
} B3PfxP;

/* One wheel's retail state: the three gate bytes at wheel+0xA0..0xA2 and
 * the four dither carries at wheel+0xA4..0xA7.                      [C] */
typedef struct { unsigned char gate[3], carry[4]; } B3PfxWheelState;

static struct {
    int      ready;
    GLuint   tex[B3_PFX_DESCS];      /* one per descriptor (shared names) */
    GLuint   tex_pop[4];
    B3PfxP   p[B3_PFX_MAX];
    /* the per-system RINGS: base slot, width (= cap_a+cap_b) and the
     * write cursor.  FUN_00035740/FUN_00035C00 semantics.           [C] */
    int      ring_base[B3_PFX_DESCS], ring_cap[B3_PFX_DESCS];
    int      ring_cur[B3_PFX_DESCS];
    int      pring_base[4], pring_cap[4], pring_cur[4];
    int      n_desc[B3_PFX_DESCS];   /* live count per descriptor        */
    int      n_pop[4];               /* ... and per popcorn record       */
    B3PfxWheelState wheel[B3_PFX_WHEELS];
    float    now, dt;
    unsigned rng;
} g;

const B3PfxDesc *b3_pfx_desc(int i) {
    return (i >= 0 && i < B3_PFX_DESCS) ? &B3_PFX_DESC[i] : NULL;
}
const B3PfxEmitter *b3_pfx_emitter(int i) {
    return (i >= 0 && i < B3_PFX_EMITTERS) ? &B3_PFX_EMIT[i] : NULL;
}
const B3PfxSurface *b3_pfx_surface(int i) {
    return (i >= 0 && i < B3_PFX_SURFACES) ? &B3_PFX_SURF[i] : NULL;
}

static float frand(void) {
    g.rng = g.rng * 1664525u + 1013904223u;
    return (float)((g.rng >> 8) & 0xFFFFFF) / 16777216.0f;
}
static float srand2(void) { return frand() * 2.0f - 1.0f; }
static unsigned char rbyte(void) {
    g.rng = g.rng * 1664525u + 1013904223u;
    return (unsigned char)(g.rng >> 16);
}

static GLuint load_png(const char *path) {
    SDL_Surface *img = IMG_Load(path), *rgba;
    GLuint t = 0;
    if (!img) return 0;
    rgba = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(img);
    if (!rgba) return 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
    SDL_FreeSurface(rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

/* Lay the per-system rings out over the flat pool: ring i is exactly
 * cap_a+cap_b slots wide, which is what FUN_00035740 mallocs.       [C] */
static void rings_layout(void) {
    int i, at = 0;
    for (i = 0; i < B3_PFX_DESCS; i++) {
        int cap = B3_PFX_DESC[i].cap_a + B3_PFX_DESC[i].cap_b;
        if (cap < 1) cap = 1;
        if (at + cap > B3_PFX_MAX) cap = B3_PFX_MAX - at;
        g.ring_base[i] = at; g.ring_cap[i] = cap; g.ring_cur[i] = 0;
        at += cap;
    }
    for (i = 0; i < 4; i++) {
        int cap = B3_PFX_POP[i].cap;
        if (cap < 1) cap = 1;
        if (at + cap > B3_PFX_MAX) cap = B3_PFX_MAX - at;
        g.pring_base[i] = at; g.pring_cap[i] = cap; g.pring_cur[i] = 0;
        at += cap;
    }
}

int b3_pfx_init(const char *dir) {
    char path[512];
    int i, j, loaded = 0;
    if (!dir) dir = "build/particlefx";
    memset(&g, 0, sizeof g);
    g.rng = 0x1BADB002u;
    rings_layout();
    for (i = 0; i < B3_PFX_DESCS; i++) {
        for (j = 0; j < i; j++)           /* share: many descs, few names */
            if (!strcmp(B3_PFX_DESC[i].tex, B3_PFX_DESC[j].tex)) break;
        if (j < i) { g.tex[i] = g.tex[j]; continue; }
        snprintf(path, sizeof path, "%s/%s.png", dir, B3_PFX_DESC[i].tex);
        g.tex[i] = load_png(path);
        if (g.tex[i]) loaded++;
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < i; j++)
            if (!strcmp(B3_PFX_POP[i].tex, B3_PFX_POP[j].tex)) break;
        if (j < i) { g.tex_pop[i] = g.tex_pop[j]; continue; }
        snprintf(path, sizeof path, "%s/%s.png", dir, B3_PFX_POP[i].tex);
        g.tex_pop[i] = load_png(path);
        if (g.tex_pop[i]) loaded++;
    }
    g.ready = (g.tex[6] != 0);            /* descriptor 6 = the fxsmoke */
    printf("[particlefx] %d textures from %s%s\n", loaded, dir,
           g.ready ? "" : "  -- disabled, run "
                          "tools/extract_particlefx_art.py");
    return loaded;
}

int  b3_pfx_ready(void) { return g.ready; }
void b3_pfx_reset(void) {
    memset(g.p, 0, sizeof g.p);
    memset(g.n_desc, 0, sizeof g.n_desc);
    memset(g.n_pop, 0, sizeof g.n_pop);
    memset(g.ring_cur, 0, sizeof g.ring_cur);
    memset(g.pring_cur, 0, sizeof g.pring_cur);
    memset(g.wheel, 0, sizeof g.wheel);
}
void b3_pfx_shutdown(void) {
    int i, j;
    for (i = 0; i < B3_PFX_DESCS; i++) {
        if (!g.tex[i]) continue;
        for (j = 0; j < i; j++) if (g.tex[j] == g.tex[i]) break;
        if (j == i) glDeleteTextures(1, &g.tex[i]);
        g.tex[i] = 0;
    }
    for (i = 0; i < 4; i++) {
        if (!g.tex_pop[i]) continue;
        for (j = 0; j < i; j++) if (g.tex_pop[j] == g.tex_pop[i]) break;
        if (j == i) glDeleteTextures(1, &g.tex_pop[i]);
        g.tex_pop[i] = 0;
    }
    memset(&g, 0, sizeof g);
}

/* FUN_00035C00: write one particle into its system's RING.  The ring is
 * cap_a+cap_b slots wide and it WRAPS -- a saturated system recycles its
 * OLDEST particle, it does not refuse the new one.  (The old port
 * refused, which left every busy system holding only stale, nearly
 * transparent particles -- exactly the "barely visible" dust.)      [C] */
static B3PfxP *ring_alloc(int base, int cap, int *cur, int *live_count) {
    B3PfxP *q;
    if (cap < 1) return NULL;
    q = &g.p[base + *cur];
    *cur = (*cur + 1) % cap;
    if (q->live && live_count) (*live_count)--;
    memset(q, 0, sizeof *q);
    return q;
}

/* One particle of emitter `e` at `pos`, seeded from the carrier
 * velocity.  The v[] mapping is the [S] one documented in the header:
 *   v0 speed along the carrier direction, v1..v3 a symmetric velocity
 *   box (lateral, up, lateral), v4 extra upward, v5 spin seed. */
static void spawn(const B3PfxEmitter *e, const float pos[3],
                  const float vel[3], float intensity) {
    const B3PfxDesc *d = &B3_PFX_DESC[e->sys];
    B3PfxP *q;
    float n, dir[3] = {0.f, 0.f, 0.f};
    if (!g.tex[e->sys]) return;
    /* THE RING CAPACITIES ARE THE BUDGET: FUN_00035740 mallocs cap_a and
     * cap_b slots per system and the rings simply wrap, so a system can
     * never hold more than cap_a+cap_b particles no matter how hard the
     * emitter is driven -- and the ones it holds are always the NEWEST
     * cap_a+cap_b.                                                  [C] */
    q = ring_alloc(g.ring_base[e->sys], g.ring_cap[e->sys],
                   &g.ring_cur[e->sys], &g.n_desc[e->sys]);
    if (!q) return;
    n = sqrtf(vel[0]*vel[0] + vel[1]*vel[1] + vel[2]*vel[2]);
    if (n > 1e-4f) { dir[0] = vel[0]/n; dir[1] = vel[1]/n; dir[2] = vel[2]/n; }

    /* [S] mapping of the emitter's random-range floats: v0 is the speed
     * along the carrier direction, v2/v3 a symmetric HORIZONTAL box, v4
     * the upward throw and v1 a small vertical jitter.  Reading v2 as a
     * vertical range instead put the impact burst's glass into the sky
     * (first capture), which is what settled the assignment. */
    q->p[0] = pos[0]; q->p[1] = pos[1]; q->p[2] = pos[2];
    q->v[0] = dir[0]*e->v[0] + srand2()*e->v[2];
    q->v[1] = dir[1]*e->v[0] + frand()*e->v[4] + srand2()*e->v[1];
    q->v[2] = dir[2]*e->v[0] + srand2()*e->v[3];
    q->birth = g.now;
    q->life  = d->life;
    q->grav  = d->grav;
    q->size_mul  = e->size_lo + (e->size_hi - e->size_lo) * frand();
    q->alpha_mul = (e->alpha_lo + (e->alpha_hi - e->alpha_lo) * frand())
                 * (intensity > 1.f ? 1.f : intensity);
    q->spin      = frand() * 6.2831853f;
    q->spin_rate = e->v[5] * 6.2831853f * srand2();
    q->desc = (unsigned char)e->sys;
    q->pop  = 0;
    q->live = 1;
    g.n_desc[e->sys]++;
}

static void spawn_pop(int rec, const float pos[3], const float vel[3],
                      float spread) {
    B3PfxP *q;
    if (rec < 0 || rec > 3 || !g.tex_pop[rec]) return;
    q = ring_alloc(g.pring_base[rec], g.pring_cap[rec],   /* capacity [C] */
                   &g.pring_cur[rec], &g.n_pop[rec]);
    if (!q) return;
    q->p[0] = pos[0]; q->p[1] = pos[1]; q->p[2] = pos[2];
    q->v[0] = vel[0] + srand2() * spread;
    q->v[1] = vel[1] + frand()  * spread;
    q->v[2] = vel[2] + srand2() * spread;
    q->birth = g.now;
    q->life  = B3_PFX_POP[rec].life;
    q->grav  = B3_PFX_POP[rec].grav;
    q->size_mul = 0.7f + 0.6f * frand();
    q->alpha_mul = 1.0f;
    q->spin = frand() * 6.2831853f;
    q->spin_rate = srand2() * 6.0f;
    q->desc = (unsigned char)rec;
    q->pop = 1;
    q->live = 1;
    g.n_pop[rec]++;
}

/* FUN_00181A80 verbatim, minus the two-ring bookkeeping.  `rate_k` is
 * the function's param_7 (it multiplies the table rate together with the
 * swept distance param_6) and `inten` is the per-particle size/alpha
 * weight; retail's crash trail passes the same k for both, its wheel FX
 * passes two different numbers.                                     [C] */
static void streak2(int emitter, const float p0[3], const float p1[3],
                    const float vel[3], float dist, float rate_k,
                    float inten, unsigned char *carry)
{
    const B3PfxEmitter *e;
    float r, carry_f, fa, fb;
    int na, nb, i;
    static unsigned char local_carry;
    if (!g.ready || emitter < 0 || emitter >= B3_PFX_EMITTERS) return;
    if (dist <= 0.0f || rate_k <= 0.0f || inten <= 0.0f) return;
    e = &B3_PFX_EMIT[emitter];
    if (!carry) carry = &local_carry;

    r = dist * e->rate * rate_k
      * (B3_PFX_RATE_JITTER_LO + B3_PFX_RATE_JITTER_SP * frand());
    carry_f = (float)(*carry) / 256.0f;
    fa = e->strm_a * r * B3_PFX_STREAM_A_K + carry_f;
    na = (int)fa;
    fb = (fa - (float)na) + e->strm_b * r;
    nb = (int)fb;
    *carry = rbyte();

    for (i = 0; i < na + nb && i < 96; i++) {
        float u = frand(), pos[3];
        pos[0] = p0[0] + (p1[0] - p0[0]) * u;
        pos[1] = p0[1] + (p1[1] - p0[1]) * u;
        pos[2] = p0[2] + (p1[2] - p0[2]) * u;
        spawn(e, pos, vel, inten);
    }
}

void b3_pfx_streak(int emitter, const float p0[3], const float p1[3],
                   const float vel[3], float dist, float intensity,
                   unsigned char *carry)
{
    streak2(emitter, p0, p1, vel, dist, intensity, intensity, carry);
}

/* FUN_001824C0: the same law, count supplied by the caller. */
void b3_pfx_burst(int emitter, const float pos[3], const float vel[3],
                  float count, float intensity)
{
    const B3PfxEmitter *e;
    int n, i;
    if (!g.ready || emitter < 0 || emitter >= B3_PFX_EMITTERS) return;
    e = &B3_PFX_EMIT[emitter];
    n = (int)(count * intensity
              * (B3_PFX_RATE_JITTER_LO + B3_PFX_RATE_JITTER_SP * frand()));
    if (n > 128) n = 128;
    for (i = 0; i < n; i++) spawn(e, pos, vel, intensity);
}

/* ---- the drivers ----------------------------------------------------- */

void b3_pfx_crash_slide(const float pos[3], const float vel[3],
                        float speed, float t, float dt)
{
    static unsigned char carry[3];       /* retail's 0x007547CC + i    [C] */
    float k, prev[3], dist;
    int i;
    if (!g.ready || t >= B3_PFX_SLIDE_WINDOW || dt <= 0.0f) return;
    k = (t < B3_PFX_SLIDE_HOLD)
      ? 1.0f
      : 1.0f - (t - B3_PFX_SLIDE_HOLD) * B3_PFX_SLIDE_FADE_A
                                       * B3_PFX_SLIDE_FADE_B;
    if (k <= 0.0f) return;
    prev[0] = pos[0] - vel[0] * dt;
    prev[1] = pos[1] - vel[1] * dt;
    prev[2] = pos[2] - vel[2] * dt;
    dist = speed * dt;
    if (dist <= 0.0f) return;
    for (i = 0; i < 3; i++)
        b3_pfx_streak(B3_PFX_SLIDE_IDS[i], prev, pos, vel, dist, k,
                      &carry[i]);
}

void b3_pfx_wreck_plume(const float pos[3], const float vel[3],
                        float t, float dt)
{
    static unsigned char carry;
    float f, k, up[3] = {0.f, 0.6f, 0.f}, p1[3];
    (void)vel;
    if (!g.ready || t >= B3_PFX_PLUME_WINDOW || dt <= 0.0f) return;
    f = t / B3_PFX_PLUME_WINDOW;
    k = 1.0f - f * f;                         /* intensity 1-(t/14)^2 [C] */
    if (k <= 0.0f) return;
    /* Emitter 11's TABLE RATE IS ZERO -- it is one of the point emitters
     * (FUN_001820D0), not a streak, so the count comes from the caller.
     * Retail emits from the model's type-10 light points; the harness has
     * no per-model light table here, so the plume rises off the body
     * origin.  GLUE on the POINT and the 18/s rate, [C] on the 14 s
     * window and the 1-(t/14)^2 intensity. */
    /* the same stochastic-rounding dither the streak emitter uses, so a
     * sub-1 per-frame count still emits at the right average rate. */
    {
        float r = 18.0f * dt * k + (float)carry / 256.0f;
        int n = (int)r;
        carry = (unsigned char)((r - (float)n) * 256.0f);
        while (n-- > 0) {
            p1[0] = pos[0] + srand2() * 0.35f;
            p1[1] = pos[1] + 0.55f + frand() * 0.35f;
            p1[2] = pos[2] + srand2() * 0.35f;
            b3_pfx_burst(B3_PFX_PLUME_EMIT, p1, up, 1.0f, k);
        }
    }
}

void b3_pfx_impact_burst(const float pos[3], const float vel[3],
                         int n_points)
{
    float n_glass, n_tint, base[3], v[3];
    int i;
    if (!g.ready) return;
    if (n_points < 1) n_points = 1;
    /* min(100/n, 25) and min(20/n, 25)                               [C] */
    n_glass = B3_PFX_BURST_GLASS_N / (float)n_points;
    if (n_glass > B3_PFX_BURST_CAP) n_glass = B3_PFX_BURST_CAP;
    n_tint = B3_PFX_BURST_TINT_N / (float)n_points;
    if (n_tint > B3_PFX_BURST_CAP) n_tint = B3_PFX_BURST_CAP;

    for (i = 0; i < n_points; i++) {
        base[0] = pos[0] + srand2() * 0.6f;
        base[1] = pos[1] + 0.3f + frand() * 0.5f;
        base[2] = pos[2] + srand2() * 0.6f;
        v[0] = vel[0] * 0.3f; v[1] = vel[1] * 0.3f + 1.0f;
        v[2] = vel[2] * 0.3f;
        b3_pfx_burst(B3_PFX_BURST_GLASS_ID, base, v, n_glass, 1.0f);
        b3_pfx_burst(B3_PFX_BURST_TINT_ID,  base, v, n_tint,  1.0f);
    }
    /* the popcorn CONCRETE DUST -- instance 2, 0x003EAE88            [C] */
    {
        float dv[3] = { vel[0] * 0.15f, 1.5f, vel[2] * 0.15f };
        for (i = 0; i < 24; i++) spawn_pop(2, pos, dv, 4.5f);
    }
}

/* FUN_001805B0: slew one gate byte towards target*255 at 510/s.     [C] */
static void gate_slew(unsigned char *b, float target, float dt) {
    float cur = (float)*b, tgt = target * B3_PFX_GATE_FULL;
    float step = dt * B3_PFX_GATE_SLEW, nv;
    if (tgt < 0.0f) tgt = 0.0f;
    if (tgt > B3_PFX_GATE_FULL) tgt = B3_PFX_GATE_FULL;
    if (tgt > cur) { nv = cur + step; if (nv > tgt) nv = tgt; }
    else           { nv = cur - step; if (nv < tgt) nv = tgt; }
    if (nv < 0.0f) nv = 0.0f;
    if (nv > B3_PFX_GATE_FULL) nv = B3_PFX_GATE_FULL;
    *b = (unsigned char)(nv + B3_PFX_GATE_ROUND);
}

void b3_pfx_wheel(int slot, const float pos[3], const float vel[3],
                  float dist, int surface, float slip, float intensity,
                  int rear)
{
    B3PfxWheelState *w;
    const B3PfxSurface *s = NULL;
    float tgt[3] = {0.f, 0.f, 0.f}, p0[3], n;
    int emit_surf = B3_PFX_NONE, k;
    if (!g.ready || slot < 0 || slot >= B3_PFX_WHEELS) return;
    w = &g.wheel[slot];

    {   static int lg = -1; static int last = -99;
        if (lg < 0) lg = getenv("B3_PFX_LOG") != NULL;
        if (lg && surface != last) {
            last = surface;
            printf("[particlefx] t=%.1f surface %d -> emitter %d\n",
                   (double)g.now, surface,
                   (surface >= 0 && surface < B3_PFX_SURFACES)
                   ? B3_PFX_SURF[surface].emit : -1);
        } }

    /* THE GATE TARGETS ARE THE SURFACE ROW, @0x0018093B..0x0018094C:
     *   +0xA0 <- row.scale * skid_flag, +0xA1 <- row.skid,
     *   +0xA2 <- row.gravelness -- and the surface emitter is read only
     *   for the REAR wheels (`if (1 < wheel)` @0x00180924).        [C] */
    if (surface >= 0 && surface < B3_PFX_SURFACES) {
        s = &B3_PFX_SURF[surface];
        if (slip < 0.0f) slip = 0.0f;
        if (slip > 1.0f) slip = 1.0f;
        tgt[0] = s->scale * slip;
        tgt[1] = s->skid;
        tgt[2] = s->gravelness;
        if (rear) emit_surf = (int)(unsigned char)s->emit;
    }
    for (k = 0; k < 3; k++) gate_slew(&w->gate[k], tgt[k], g.dt);

    if (dist <= 0.0f || intensity <= 0.0f) return;
    /* retail's segment is the wheel's own frame travel (pos - prev_pos,
     * @0x001809E7); the harness gives the length, so walk it back down
     * the velocity direction.                                     GLUE */
    n = sqrtf(vel[0]*vel[0] + vel[1]*vel[1] + vel[2]*vel[2]);
    if (n > 1e-4f) {
        p0[0] = pos[0] - vel[0] * (dist / n);
        p0[1] = pos[1] - vel[1] * (dist / n);
        p0[2] = pos[2] - vel[2] * (dist / n);
    } else { p0[0] = pos[0]; p0[1] = pos[1]; p0[2] = pos[2]; }

    for (k = 0; k < 3; k++) {
        float a, rate_k, inten;
        if (!w->gate[k]) continue;
        a = (float)w->gate[k] * (1.0f / B3_PFX_GATE_FULL);
        rate_k = (a * B3_PFX_GATE_RATE_K + B3_PFX_GATE_RATE_B);   /* [C] */
        inten  = a * B3_PFX_GATE_INTEN_K;                         /* [C] */
        if (inten > 1.0f) inten = 1.0f;
        streak2(B3_PFX_GATE_IDS[k], p0, pos, vel, dist,
                rate_k * intensity, inten, &w->carry[k]);
        /* the veh+0x215==1 companion on the +0xA0 gate (@0x00180D44):
         * rate 1.0, same intensity.  The class byte is not in this
         * port, so it rides the same gate.                    [S] GLUE */
        if (k == 0)
            streak2(B3_PFX_WHEEL_EXTRA_ID, p0, pos, vel, dist,
                    intensity, inten, &w->carry[0]);
    }
    /* the SURFACE-dependent emitter, rate = the spin scale (1.0 here --
     * retail's wheelspin bump @0x00180A93 needs the wheel's angular
     * velocity, which this port has not recovered [?]), intensity 1.0
     * (@0x00180EA1).                                                [C] */
    if (emit_surf != B3_PFX_NONE)
        streak2(emit_surf, p0, pos, vel, dist, intensity, 1.0f,
                &w->carry[3]);
}

void b3_pfx_grind_spark(const float pos[3], const float dir[3],
                        float speed, float dt, unsigned char *carry)
{
    /* TUNED: the spark shower's own emitter was not found (fxspark is
     * named at 0x003AAFC0 but no descriptor in the 24-row table binds
     * it), so this fires the recovered popcorn SPARK record 0 at a rate
     * proportional to the grind speed. */
    float r, v[3];
    int n, i;
    static unsigned char local_carry;
    if (!g.ready || speed <= 3.0f || dt <= 0.0f) return;
    if (!carry) carry = &local_carry;
    r = speed * dt * 22.0f
      * (B3_PFX_RATE_JITTER_LO + B3_PFX_RATE_JITTER_SP * frand())
      + (float)(*carry) / 256.0f;
    *carry = rbyte();
    n = (int)r;
    if (n > 40) n = 40;
    for (i = 0; i < n; i++) {
        float sp = 2.5f + speed * 0.22f;
        v[0] = dir[0] * sp + srand2() * sp * 0.5f;
        v[1] = 1.2f + frand() * sp * 0.35f;
        v[2] = dir[2] * sp + srand2() * sp * 0.5f;
        spawn_pop(0, pos, v, 1.4f);
    }
}

/* ---- simulation ------------------------------------------------------ */

void b3_pfx_update(float dt) {
    static int log_on = -1;
    static float log_t;
    int i;
    if (!g.ready) return;
    if (dt < 0.0f) dt = 0.0f;
    g.now += dt;
    g.dt = dt;
    if (log_on < 0) log_on = getenv("B3_PFX_LOG") != NULL;
    if (log_on && (log_t += dt) >= 1.0f) {
        log_t = 0.0f;
        printf("[particlefx] t=%.1f live %d  (smoke7 %d smoke8 %d smoke10 %d "
               "smoke11 %d gravel2 %d snow3 %d glass4 %d trail14 %d "
               "fire19 %d smoke20 %d plume6 %d)\n",
               (double)g.now, b3_pfx_live(), b3_pfx_live_of_desc(7), b3_pfx_live_of_desc(8),
               b3_pfx_live_of_desc(10), b3_pfx_live_of_desc(11),
               b3_pfx_live_of_desc(2), b3_pfx_live_of_desc(3),
               b3_pfx_live_of_desc(4), b3_pfx_live_of_desc(14),
               b3_pfx_live_of_desc(19), b3_pfx_live_of_desc(20),
               b3_pfx_live_of_desc(6));
    }
    for (i = 0; i < B3_PFX_MAX; i++) {
        B3PfxP *q = &g.p[i];
        if (!q->live) continue;
        if (g.now - q->birth >= q->life) {
            q->live = 0;
            if (q->pop) g.n_pop[q->desc]--; else g.n_desc[q->desc]--;
        } else {
            q->spin += q->spin_rate * dt;
        }
    }
}

int b3_pfx_live(void) {
    int i, n = 0;
    for (i = 0; i < B3_PFX_MAX; i++) if (g.p[i].live) n++;
    return n;
}
int b3_pfx_live_of_desc(int desc) {
    return (desc >= 0 && desc < B3_PFX_DESCS) ? g.n_desc[desc] : 0;
}

/* ---- draw ------------------------------------------------------------ *
 * The recovered blend modes are applied per descriptor, so the pass is
 * ordered by blend group: plain alpha, then subtractive, then additive.
 * Retail submits one quad list per system with vertex stride 0x20 via
 * FUN_001D7D50(8, n, 0x004D9430, 0x20); the exact 32-byte vertex layout
 * was not decoded [?], so this uses the harness's own.                  */
/* FUN_00035D00 writes the Xbox D3D8 render states 0x3E (SRCBLEND), 0x3F
 * (DESTBLEND) and 0x4A (BLENDOP) with the NV2A token values, which ARE
 * the GL enums -- so the mapping below is literal, not interpreted:
 *   blend 0: 0x0302 SRC_ALPHA / 0x0303 ONE_MINUS_SRC_ALPHA, 0x8006 ADD
 *   blend 1: 0x0302 SRC_ALPHA / 0x0001 ONE, 0x800B REVERSE_SUBTRACT
 *   blend 2: 0x0302 SRC_ALPHA / 0x0001 ONE, 0x8006 ADD
 * (0x800B is only written on the subtractive path -- everything else
 * restores 0x8006 -- which is why b3_pfx_draw restores GL_FUNC_ADD.)
 *                                                                  [C] */
static void set_blend(int blend) {
    switch (blend) {
    case 1:      /* SRC_ALPHA / ONE, REVSUBTRACT -- it DARKENS      [C] */
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        break;
    case 2:      /* SRC_ALPHA / ONE, ADD                            [C] */
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glBlendEquation(GL_FUNC_ADD);
        break;
    default:     /* SRC_ALPHA / INV_SRC_ALPHA, ADD                  [C] */
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);
        break;
    }
}

/* FUN_00034130's three-key ramp: split at mid_t, keys 0->1 then 1->2. */
static void key_lerp(const B3PfxDesc *d, float age,
                     float rgba[4], float *size) {
    float split = d->mid_t * d->life, u;
    const unsigned *c0, *c1;
    unsigned a, b;
    float s0, s1;
    if (age >= split && d->life > split) {
        u = (age - split) / (d->life - split);
        c0 = &d->col[1]; c1 = &d->col[2];
        s0 = d->size[1]; s1 = d->size[2];
    } else {
        u = (split > 0.0f) ? age / split : 1.0f;
        c0 = &d->col[0]; c1 = &d->col[1];
        s0 = d->size[0]; s1 = d->size[1];
    }
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    a = *c0; b = *c1;
    /* THE BYTE ORDER IS 0xAABBGGRR, NOT 0xAARRGGBB.  FUN_00035740
     * @0x000357BE unpacks the record's four bytes LOW FIRST into the
     * system's float4 (+0x18 -> +0x40, +0x19 -> +0x44, +0x1A -> +0x48,
     * +0x1B -> +0x4C), and FUN_00034130 @0x0003547E packs that float4
     * back into the D3DCOLOR as alpha<<24 | c0<<16 | c1<<8 | c2 -- so
     * the RED lane is the record's LOW byte.  Which is why fxfire's
     * middle key 0xFF4080C0 is (0xC0,0x80,0x40) ORANGE, fxgravel's is
     * (0xC0,0xB0,0x90) SAND, and descriptor 11's offroad dust is
     * (0x96,0x8C,0x64) warm BROWN -- reading it as 0xAARRGGBB turned
     * every one of them blue.                                      [C] */
    rgba[0] = ((a & 0xFF) + (((int)(b & 0xFF) - (int)(a & 0xFF)) * u))
              / 255.0f;
    rgba[1] = (((a >>  8) & 0xFF) + (((int)((b >>  8) & 0xFF)
              - (int)((a >>  8) & 0xFF)) * u)) / 255.0f;
    rgba[2] = (((a >> 16) & 0xFF) + (((int)((b >> 16) & 0xFF)
              - (int)((a >> 16) & 0xFF)) * u)) / 255.0f;
    rgba[3] = (((a >> 24) & 0xFF) + (((int)((b >> 24) & 0xFF)
              - (int)((a >> 24) & 0xFF)) * u)) / 255.0f;
    *size = (s0 + (s1 - s0) * u) * B3_PFX_SIZE_K;
}

void b3_pfx_draw(void) {
    float mv[16], pr[16], rt[3], up[3], px_k, res_k;
    GLint vp[4];
    int pass, i;
    if (!g.ready) return;

    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    glGetFloatv(GL_PROJECTION_MATRIX, pr);
    glGetIntegerv(GL_VIEWPORT, vp);
    /* max_px / min_px are absolute pixels on RETAIL's 640-wide render
     * target (FUN_00034130 scales by the viewport ints cam+0x78/0x7C
     * straight out of the 640x480 back buffer), so on a different
     * output they have to travel as a screen FRACTION or a 1080p frame
     * would clamp every big puff to a sixth of retail's coverage and
     * would keep specks retail culls.                            GLUE */
    res_k = (float)vp[2] * (1.0f / 640.0f);
    if (res_k < 0.25f) res_k = 0.25f;
    rt[0] = mv[0]; rt[1] = mv[4]; rt[2] = mv[8];
    up[0] = mv[1]; up[1] = mv[5]; up[2] = mv[9];
    /* pixels per world unit at unit view depth: P[1][1] * height/2 */
    px_k = pr[5] * (float)vp[3] * 0.5f;

    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT
                 | GL_TEXTURE_BIT | GL_CURRENT_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    /* depth-TEST but do not WRITE -- the particle pass never occludes
     * itself or anything drawn after it. */
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);

    for (pass = 0; pass < 3; pass++) {
        GLuint bound = 0;
        int open = 0;
        set_blend(pass);
        for (i = 0; i < B3_PFX_MAX; i++) {
            const B3PfxP *q = &g.p[i];
            const B3PfxDesc *d;
            GLuint tex;
            float age, rgba[4], size, h, c, s, ax[3], ay[3], pp[3];
            if (!q->live) continue;
            if (q->pop) {
                if (pass != 2) continue;         /* popcorn = additive  */
                d = NULL;
                tex = g.tex_pop[q->desc];
            } else {
                d = &B3_PFX_DESC[q->desc];
                if (d->blend != pass) continue;
                tex = g.tex[q->desc];
            }
            if (!tex) continue;
            age = g.now - q->birth;
            if (age <= 0.0f || age >= q->life) continue;

            if (d) {
                key_lerp(d, age, rgba, &size);
            } else {
                const B3PfxPopcorn *pc = &B3_PFX_POP[q->desc];
                float u = age / q->life;
                int k;
                for (k = 0; k < 4; k++)
                    rgba[k] = pc->rgba0[k] + (pc->rgba1[k] - pc->rgba0[k]) * u;
                size = 0.10f;
            }
            /* KIND 4 IS DRAWN AT HALF ALPHA.  Its four vertices are
             * coloured individually inside the 0x00035280 loop and that
             * path multiplies the alpha by 0.5 (@0x000352FD, the 0.5 at
             * 0x003B1684) before packing the D3DCOLOR -- every fxsmoke
             * descriptor is kind 4, so the whole smoke family lands at
             * half its tabulated alpha.  (The same loop also modulates
             * the RGB by a two-key directional shade out of system+0x70
             * / +0x80, which is not in the 0x80-byte descriptor and was
             * not recovered.)                                  [C]/[?] */
            if (d && d->kind == 4) rgba[3] *= 0.5f;
            rgba[3] *= q->alpha_mul * B3_PFX_LEVEL;
            if (rgba[3] <= 0.005f) continue;     /* FUN_00034130's cull */
            size *= q->size_mul;
            if (size <= 0.0005f) continue;

            /* pos = p + v*t + g*t*t   (NOT 0.5*g*t*t)              [C] */
            pp[0] = q->p[0] + q->v[0] * age;
            pp[1] = q->p[1] + q->v[1] * age + q->grav * age * age;
            pp[2] = q->p[2] + q->v[2] * age;

            /* THE SCREEN-SIZE LIMITS, FUN_00034130 @0x000346B9 and
             * @0x000346F1.  Both are applied to the pixel HALF-extent,
             * i.e. to half the sprite's width, so in the full-width
             * pixels this port works in the cap is max_px*0.435*2 and
             * the cull is min_px*2.  (The old port compared them to the
             * FULL width, which clamped every big puff to half retail's
             * size.)                                              [C] */
            {
                float zv = mv[2]*pp[0] + mv[6]*pp[1] + mv[10]*pp[2] + mv[14];
                float depth = zv < 0.0f ? -zv : zv;
                float px, cap, lo;
                if (depth < 0.05f) continue;
                px  = size * px_k / depth;
                cap = (d ? d->max_px : 64.0f) * B3_PFX_MAXPX_K
                    * B3_PFX_HALF_TO_FULL * res_k;
                lo  = (d ? d->min_px : 1.5f) * B3_PFX_HALF_TO_FULL * res_k;
                if (px < lo) continue;
                if (px > cap) size *= cap / px;
            }

            /* THE QUAD, per render kind (the 0x00035728 jump table):
             * kinds 0/3/4 give a textured square of side `size`; kind 1
             * is the same diamond WITHOUT the sqrt(2) at 0x003A34B8, so
             * its texture square is 1/sqrt(2) smaller.  Kinds 2 and 5
             * were not decoded and ride the default.           [C]/[?] */
            if (d && d->kind == 1) size *= 1.0f / B3_PFX_SQRT2;

            if (tex != bound) {
                if (open) { glEnd(); open = 0; }
                glBindTexture(GL_TEXTURE_2D, tex);
                bound = tex;
            }
            if (!open) { glBegin(GL_QUADS); open = 1; }

            h = size * 0.5f;
            c = cosf(q->spin); s = sinf(q->spin);
            ax[0] = (rt[0]*c + up[0]*s) * h;
            ax[1] = (rt[1]*c + up[1]*s) * h;
            ax[2] = (rt[2]*c + up[2]*s) * h;
            ay[0] = (up[0]*c - rt[0]*s) * h;
            ay[1] = (up[1]*c - rt[1]*s) * h;
            ay[2] = (up[2]*c - rt[2]*s) * h;
            glColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
            glTexCoord2f(0.f, 0.f);
            glVertex3f(pp[0]-ax[0]+ay[0], pp[1]-ax[1]+ay[1], pp[2]-ax[2]+ay[2]);
            glTexCoord2f(1.f, 0.f);
            glVertex3f(pp[0]+ax[0]+ay[0], pp[1]+ax[1]+ay[1], pp[2]+ax[2]+ay[2]);
            glTexCoord2f(1.f, 1.f);
            glVertex3f(pp[0]+ax[0]-ay[0], pp[1]+ax[1]-ay[1], pp[2]+ax[2]-ay[2]);
            glTexCoord2f(0.f, 1.f);
            glVertex3f(pp[0]-ax[0]-ay[0], pp[1]-ax[1]-ay[1], pp[2]-ax[2]-ay[2]);
        }
        if (open) glEnd();
    }
    glBlendEquation(GL_FUNC_ADD);
    glBindTexture(GL_TEXTURE_2D, 0);
    glPopAttrib();
}
