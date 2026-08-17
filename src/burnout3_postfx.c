/* World post FX — the sky dome and the radial speed blur.
 *
 * See burnout3_postfx.h for the contract and docs/RE_POSTFX.md for the full
 * evidence trail. Everything marked [C] is transcribed from the retail XBE at
 * the cited address; everything marked GLUE is harness-side.
 *
 * The mesh generator (b3_sky_build) is deliberately GL-free so
 * tools/validate_postfx.py can compile and run it as a probe with
 * -DB3_POSTFX_NO_GL.
 */
#include "burnout3_postfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef B3_POSTFX_NO_GL
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <GL/gl.h>
#ifdef __ANDROID__
/* ANDROID PORT (android/): desktop GL here is gl4es on top of GLES2, so the
 * GL 2.0 entry points below MUST be resolved by gl4es' own lookup.
 * SDL_GL_GetProcAddress would hand back the raw GLES2 driver symbols, which
 * (a) cannot compile this file's GLSL 1.10 source and (b) sit outside the
 * program/uniform state gl4es maintains for the fixed-function emulation --
 * the next immediate-mode draw would silently unbind the program. */
#include <gl4esinit.h>
#define SDL_GL_GetProcAddress gl4es_GetProcAddress
#endif
#endif

/* ==========================================================================
 * 1. THE SKY DOME MESH  —  1:1 transcription of FUN_00032020 (0x00032020)
 * ==========================================================================
 *
 * The function runs 0x21 = 33 azimuth rings. Per ring it writes ONE "skirt"
 * vertex (before the inner loop) and then 7 dome vertices, giving 8 rows per
 * ring at the 0x20-byte stride, and it is called twice — once with param_1==1
 * (the sky half, s=+1) and once with param_1==0 (the ground half, s=-1) —
 * into one 0x41C0-byte buffer, i.e. two 8416-byte blocks.
 *
 * Per-vertex maths, straight out of the decompilation:
 *
 *   theta = ring * 0.19634954                       [C] DAT_004D916C
 *   u0    = theta * 0.15915494                      [C] 0x000320A9
 *   skirt: pos = (cos t, s * -0.25, sin t)          [C] 0x0003206A
 *          tc0 = (u0, -0.25*(vHigh-vLow) + vLow)    [C] 0x00032090
 *   dome k = 0..6:
 *          a    = (k * 0.2617994) * 0.63661975      [C] 0x00032258/0x0003225E
 *          yhat = 2a - a*a                          [C] 0x00032168
 *          rad  = 1 - a*a                           [C] 0x00032175
 *          pos  = (cos t * rad, yhat * s, sin t * rad)
 *          tc0  = (u0, yhat*(vHigh-vLow) + vLow)
 *          tc1  = (u0*2, 1 - yhat*1.1764705)   (sky half only) [C] 0x000321F0
 *
 * NOTE on the skirt's tc1: retail's inner loop writes [0xd]/[0xe] only for the
 * 7 dome vertices, so the skirt vertex of every ring keeps whatever was in
 * that memory (the previous run's value). Reproducing an uninitialised read is
 * neither possible nor useful; the skirt here gets the tc1 the formula would
 * give at yhat = -0.25, which is the continuous extension. Marked GLUE in the
 * docs; it only affects the cloud layer 0.25 units BELOW the horizon, which
 * the world geometry covers.
 */
int b3_sky_build(int sky, B3SkyVertex *verts, unsigned short *idx)
{
    const float s     = sky ? 1.0f : -1.0f;
    const float v_low = sky ? B3_SKY_V_LOW  : B3_GND_V_LOW;
    const float v_hi  = sky ? B3_SKY_V_HIGH : B3_GND_V_HIGH;
    const float v_rng = v_hi - v_low;
    int ring, k, n = 0;

    if (!verts) return 0;

    for (ring = 0; ring < B3_SKY_RINGS; ring++) {
        const float theta = (float)ring * B3_SKY_THETA_STEP;
        const float ct = cosf(theta), st = sinf(theta);
        const float u0 = theta * B3_SKY_INV_TWO_PI;
        B3SkyVertex *v;

        /* the skirt vertex — 0.25 below (sky) / above (ground) the horizon */
        v = &verts[n++];
        v->pos[0] = ct;
        v->pos[1] = s * B3_SKY_SKIRT;
        v->pos[2] = st;
        v->color  = B3_SKY_VERTEX_COLOR;
        v->tc0[0] = u0;
        v->tc0[1] = B3_SKY_SKIRT * v_rng + v_low;
        v->tc1[0] = u0 * 2.0f;
        v->tc1[1] = 1.0f - B3_SKY_SKIRT * B3_SKY_TC1_VSCALE;   /* GLUE, see above */

        for (k = 0; k < B3_SKY_ROWS - 1; k++) {
            const float a    = ((float)k * B3_SKY_T_STEP) * B3_SKY_TWO_OVER_PI;
            const float yhat = a * 2.0f - a * a;
            const float rad  = 1.0f - a * a;

            v = &verts[n++];
            v->pos[0] = ct * rad;
            v->pos[1] = yhat * s;
            v->pos[2] = st * rad;
            v->color  = B3_SKY_VERTEX_COLOR;
            v->tc0[0] = u0;
            v->tc0[1] = yhat * v_rng + v_low;
            if (sky) {
                v->tc1[0] = u0 * 2.0f;
                v->tc1[1] = 1.0f - yhat * B3_SKY_TC1_VSCALE;
            } else {
                v->tc1[0] = 0.0f;
                v->tc1[1] = 0.0f;
            }
        }
    }

    /* [C] the index build at 0x000322FD..0x00032397: 16 columns, each emitting
     * the quad strip for ring c/c+1 and then for ring c+16/c+17 (the +0x80 =
     * 16*8 vertex offset), 7 quads of 6 indices each. */
    if (idx) {
        int i = 0, c, half, j;
        for (c = 0; c < 16; c++) {
            for (half = 0; half < 2; half++) {
                const int b0 = half * 0x80 + c * 8;
                const int b1 = b0 + 8;
                for (j = 0; j < 7; j++) {
                    idx[i++] = (unsigned short)(b0 + j);
                    idx[i++] = (unsigned short)(b1 + j);
                    idx[i++] = (unsigned short)(b0 + 1 + j);
                    idx[i++] = (unsigned short)(b1 + j);
                    idx[i++] = (unsigned short)(b0 + 1 + j);
                    idx[i++] = (unsigned short)(b1 + 1 + j);
                }
            }
        }
    }
    return n;
}

/* ==========================================================================
 * 1b. THE RUNTIME GRADIENT LUT — 1:1 transcription of FUN_001891F0
 * ==========================================================================
 *
 * See burnout3_postfx.h for the full evidence trail (the three blend modes out
 * of FUN_001C82E0's four tables, the blit colour, the two v spans, the azimuth
 * span and the elevation warp, all with addresses). What this file does is run
 * those three blits on the CPU into a 64x32 RGBA image, because that is the
 * cheapest way to get a render-to-texture that is only 2048 texels big.
 *
 *   pass 1  LUT.rgb = blit * sheet[ column(progress) ]         RGB write only
 *   pass 2  LUT.a   = sheet.a at (azimuth, elevation) rel sun  ALPHA write only
 *   pass 3  LUT.rgb = lerp(LUT.rgb, blit * sheet[col+0.5], LUT.a)
 */

/* The recovered blit colour's RGB, resolved once so B3_POSTFX_SKYBLIT=<f> can
 * A/B the one [S] step in the chain (whether the 2D blitter modulates by the
 * vertex colour). Defaults to the recovered 0.5. */
static float g_lut_blit = B3_SKY_LUT_BLIT_RGB;

void b3_sky_lut_set_blit(float f) { g_lut_blit = (f < 0.0f) ? 0.0f : f; }

/* The blitter's texture sampling: bilinear, WRAP in u for pass 2 (whose span
 * is exactly one wrap of the sheet), CLAMP otherwise and always in v. */
static void sky_sample(const unsigned char *src, int sw, int sh,
                       float u, float v, int wrap_u, float out[4])
{
    float fx = u * (float)sw - 0.5f;
    float fy = v * (float)sh - 0.5f;
    int   x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - (float)x0,  ty = fy - (float)y0;
    int   x1 = x0 + 1,          y1 = y0 + 1;
    int   c;

    if (wrap_u) {
        x0 = ((x0 % sw) + sw) % sw;
        x1 = ((x1 % sw) + sw) % sw;
    } else {
        if (x0 < 0) x0 = 0;
        if (x0 > sw - 1) x0 = sw - 1;
        if (x1 < 0) x1 = 0;
        if (x1 > sw - 1) x1 = sw - 1;
    }
    if (y0 < 0) y0 = 0;
    if (y0 > sh - 1) y0 = sh - 1;
    if (y1 < 0) y1 = 0;
    if (y1 > sh - 1) y1 = sh - 1;

    for (c = 0; c < 4; c++) {
        float a = (float)src[(y0 * sw + x0) * 4 + c];
        float b = (float)src[(y0 * sw + x1) * 4 + c];
        float d = (float)src[(y1 * sw + x0) * 4 + c];
        float e = (float)src[(y1 * sw + x1) * 4 + c];
        float top = a + (b - a) * tx;
        float bot = d + (e - d) * tx;
        out[c] = top + (bot - top) * ty;
    }
}

static unsigned char sky_u8(float t)
{
    if (t < 0.0f) return 0;
    if (t > 255.0f) return 255;
    return (unsigned char)(t + 0.5f);
}

/* [C] FUN_00189660 @0x00189660. The eight-octant azimuth (0 at +X, increasing
 * toward +Z, scaled by 0.15915494 = 1/(2*pi)) and the elevation parameter
 * `1 - sqrt(1-y)` / `sqrt(1+y) - 1`. atan2f collapses the octant ladder: the
 * ladder exists only because the x87 FPATAN wants the smaller operand first,
 * and every one of its eight branches reduces to the same angle. */
void b3_sky_sun_angles(const float dir[3], float *az, float *elev)
{
    float x = dir[0], y = dir[1], z = dir[2];
    float len = sqrtf(x * x + y * y + z * z);
    float a;

    if (len > 1e-6f) { x /= len; y /= len; z /= len; }
    if (y >  1.0f) y =  1.0f;
    if (y < -1.0f) y = -1.0f;

    /* atan2f(x,z) is 0 at +Z and +pi/2 at +X; retail's ladder is 0 at +X and
     * 0.25 at +Z — the complement. */
    a = 0.25f - atan2f(x, z) * B3_SKY_INV_TWO_PI;
    a -= floorf(a);                                   /* into [0,1)          */
    if (az) *az = a;

    if (elev) *elev = (y >= 0.0f) ? (1.0f - sqrtf(1.0f - y))
                                  : (sqrtf(1.0f + y) - 1.0f);
}

/* [C] 0x001893BC..0x001893F7 (the y == 0 case) and 0x00189485..0x001894CE (the
 * loop): v = min((0.5 - ((1 - sqrt(y/24)) - X)) * 24, 24), in source texels. */
float b3_sky_glow_v_texels(float y, float sun_elev)
{
    float t = y * (1.0f / B3_SKY_GLOW_V_SCALE);
    float a_row, v;
    if (t < 0.0f) t = 0.0f;
    a_row = 1.0f - sqrtf(t);
    v = (B3_SKY_GLOW_V_CENTER - (a_row - sun_elev)) * B3_SKY_GLOW_V_SCALE;
    if (v > B3_SKY_GLOW_V_SCALE) v = B3_SKY_GLOW_V_SCALE;      /* 0x001893F5 */
    return v;
}

void b3_sky_build_lut(const unsigned char *src, int sw, int sh,
                      float progress, float sun_az, float sun_elev,
                      int have_sun, unsigned char *out)
{
    const float blit = g_lut_blit;
    float col_base, col_glow;
    int x, y, c;

    if (!src || !out || sw <= 0 || sh <= 0) return;

    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    /* [C] 0x001892E1..0x001892FB (pass 1) and 0x001895C7 (pass 3). */
    col_base = progress * B3_SKY_LUT_U_SPAN + B3_SKY_LUT_U_BASE;
    col_glow = col_base + B3_SKY_GLOW_U_OFFSET;

    /* ---- pass 1: mode 3 = ONE/ZERO, write mask RGB. One quad, dest
     * (0,0)-(64,32), source u degenerate at col_base, v 0.015625..1.015625.
     * At dest texel centres that v span lands exactly halfway between source
     * rows y and y+1, so the LUT is a 2-tap vertical prefilter of the sheet. */
    for (y = 0; y < B3_SKY_LUT_H; y++) {
        float v = B3_SKY_LUT_SRC_V0 + (((float)y + 0.5f) / (float)B3_SKY_LUT_H)
                                    * (B3_SKY_LUT_SRC_V1 - B3_SKY_LUT_SRC_V0);
        float s[4];
        sky_sample(src, sw, sh, col_base, v, 0, s);
        for (x = 0; x < B3_SKY_LUT_W; x++) {
            unsigned char *p = out + (y * B3_SKY_LUT_W + x) * 4;
            for (c = 0; c < 3; c++) p[c] = sky_u8(s[c] * blit);
            p[3] = 0;
        }
    }

    if (!have_sun) return;   /* no enviro sun vector: pass 1 only, as before */

    /* ---- pass 2: mode 7 = ONE/ZERO, write mask ALPHA. 13 quads. The first
     * twelve cover dest rows 0..24 two rows at a time, so the elevation warp
     * is evaluated at the band EDGES and linearly interpolated across each
     * band — that piecewise-linear-in-twelve-segments shape is retail's, not
     * an approximation of it. The thirteenth covers rows 24..32 (the ground
     * hemisphere) from the two env floats. */
    for (y = 0; y < B3_SKY_LUT_H; y++) {
        float yc = (float)y + 0.5f;
        float v_src;
        if (yc < (float)B3_SKY_GLOW_SKY_ROWS) {
            int   band = (int)(yc * 0.5f);
            float y0   = (float)(band * 2), y1 = y0 + 2.0f;
            float f    = (yc - y0) * 0.5f;
            float v0   = b3_sky_glow_v_texels(y0, sun_elev);
            float v1   = b3_sky_glow_v_texels(y1, sun_elev);
            v_src = (v0 + (v1 - v0) * f) * (1.0f / (float)B3_SKY_LUT_H);
        } else {
            float f = (yc - (float)B3_SKY_GLOW_SKY_ROWS)
                      / (float)(B3_SKY_LUT_H - B3_SKY_GLOW_SKY_ROWS);
            v_src = B3_SKY_GLOW_GND_V0
                    + (B3_SKY_GLOW_GND_V1 - B3_SKY_GLOW_GND_V0) * f;
        }
        for (x = 0; x < B3_SKY_LUT_W; x++) {
            float u = (B3_SKY_GLOW_U_BASE - sun_az)
                      + (((float)x + 0.5f) / (float)B3_SKY_LUT_W)
                        * B3_SKY_GLOW_U_SPAN;
            float s[4];
            sky_sample(src, sw, sh, u, v_src, 1, s);
            out[(y * B3_SKY_LUT_W + x) * 4 + 3] =
                sky_u8(s[3] * B3_SKY_LUT_BLIT_A);
        }
    }

    /* ---- pass 3: mode 4 = DST_ALPHA / INV_DST_ALPHA, write mask RGB. One
     * quad exactly like pass 1 but at col_base + 0.5, blended by the alpha
     * pass 2 just wrote:  dst.rgb = src.rgb*dst.a + dst.rgb*(1 - dst.a). */
    for (y = 0; y < B3_SKY_LUT_H; y++) {
        float v = B3_SKY_LUT_SRC_V0 + (((float)y + 0.5f) / (float)B3_SKY_LUT_H)
                                    * (B3_SKY_LUT_SRC_V1 - B3_SKY_LUT_SRC_V0);
        float s[4];
        sky_sample(src, sw, sh, col_glow, v, 0, s);
        for (x = 0; x < B3_SKY_LUT_W; x++) {
            unsigned char *p = out + (y * B3_SKY_LUT_W + x) * 4;
            float a = (float)p[3] * (1.0f / 255.0f);
            for (c = 0; c < 3; c++)
                p[c] = sky_u8(s[c] * blit * a + (float)p[c] * (1.0f - a));
        }
    }
}

/* ==========================================================================
 * 2. BLUR STRENGTH  ->  `s`, the float the game reads at blurState+0x54
 * ==========================================================================
 * GLUE. The recovered facts are the blur's SHAPE (a radial zoom about screen
 * centre by 0.99 per tap, masked by `radialblurmask`, one state per screen —
 * FUN_0002EBE0) and the CONSUMER of the strength (b3_postfx_present_alpha,
 * below, which is [C]). The PRODUCER of `s` is still not recovered: a whole-
 * image scan for a float store at blurState+0x54 — both through the published
 * pointer DAT_004D6524 and through the renderer base as [reg+0x224] — found
 * no writer, so it is set through a pointer to the +0x50 sub-struct whose
 * owner was not identified. The ramp below is fitted to the reference
 * captures and is marked GLUE everywhere it appears. `boost_ramp` is
 * racecar+0x11AC, the SAME quantity the recovered FOV 90->110 law consumes
 * (RE_TAKEDOWN_FX 9.2) — reusing it here is a modelling choice, not a
 * recovered coupling.
 *
 * The output is on the game's own scale: `s` is what FUN_0003DA90 reads, and
 * its meaningful range is 0..2 (above 2 the recovered clamp pins C0.a at 1).
 */
float b3_postfx_blur_strength(float speed_mph, float boost_ramp)
{
    float t, boost;

    t = (speed_mph - B3_BLUR_MPH_ON) / (B3_BLUR_MPH_FULL - B3_BLUR_MPH_ON);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    t = t * t;                          /* GLUE: quiet at cruise, hard at top */

    /* the recovered FOV law's shape, 1-(r-1)^2, peaks at r == 1 */
    boost = boost_ramp - 1.0f;
    boost = 1.0f - boost * boost;
    if (boost < 0.0f) boost = 0.0f;
    if (boost > 1.0f) boost = 1.0f;

    return B3_BLUR_S_MAX * t * (1.0f + B3_BLUR_BOOST_GAIN * boost);
}

/* [C] FUN_0003DA90 @0x0003DC37..0x0003DC89 — the pixel-shader constant the
 * present composite multiplies the blur surface by:
 *
 *   0003dc3a  MOVSS  XMM0,[EAX + 0x4]        ; s = blurState+0x54
 *   0003dc42  COMISS XMM0,[0x003b1688]       ; 2.0
 *   0003dc49  JBE    0003dc55
 *   0003dc4b  MOVSS  XMM0,[0x003b168c]       ; 1.0
 *   0003dc55  MOVSS  XMM0,[EAX + 0x4]
 *   0003dc5a  MULSS  XMM0,[0x003b1684]       ; 0.5
 *   ...       C0 = (0, 0, 0, XMM0), SetPixelShaderConstant(reg 0)
 *
 * The register is a D3DCOLOR once FUN_0034E9A0 packs it, so it is clamped to
 * [0,1] on the way to NV097_SET_COMBINER_FACTOR0. */
float b3_postfx_present_alpha(float s)
{
    float a = (s <= B3_PRESENT_S_MAX) ? s * B3_PRESENT_S_SCALE : 1.0f;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return a;
}

/* [C] FUN_0003C8A0 @0x0003D3F0..0x0003D484 — the output gamma ramp, the same
 * table for R, G and B. */
void b3_postfx_gamma_table(unsigned char *ramp)
{
    int i;
    if (!ramp) return;
    for (i = 0; i < 256; i++) {
        double v = pow((double)i / 255.0, B3_GAMMA_EXPONENT) * 255.0;
        int r = (int)(v + 0.5);                 /* 0x0003D40B: round */
        ramp[i] = (unsigned char)(r < 0 ? 0 : (r > 255 ? 255 : r));
    }
}

/* ==========================================================================
 * 2b. THE CAPTURE ROW FLIP  —  the window-resize corruption
 * ==========================================================================
 *
 * glReadPixels hands back the frame BOTTOM-UP, so every capture path has to
 * mirror the rows before it hands the buffer to SDL_SaveBMP. All three of them
 * (debug_dump(), B3_SLAM_SHOT, B3_SHOT) did that through a FIXED 8192-byte
 * stack scratch row and clamped the copy to it:
 *
 *     unsigned char row[8192];
 *     int rb = sw * 4 < (int)sizeof(row) ? sw * 4 : (int)sizeof(row);
 *
 * 8192 bytes is 2048 pixels. At the default 2048x1536 that is exactly the full
 * row and the captures are correct, which is why this never showed up — but
 * the window is resizable, and the moment it is dragged WIDER than 2048 the
 * clamp silently stops mirroring the tail of every row. Everything past pixel
 * 2048 keeps glReadPixels' bottom-up order, so the capture comes out with its
 * left 2048 columns upright and the remainder vertically mirrored.
 *
 * That is exactly build/debug_dump_035.png: it is 3706x2034, and the seam sits
 * at 8192/4/3706 = 0.5526 of the width -- the reported "right half upside
 * down", measured at 0.553 in the image. It is a CAPTURE-path bug only: the
 * live window is untouched (the postfx scene copy already reallocates on any
 * size change, see postfx_grab_frame), so nothing the player sees was ever
 * wrong, only the screenshots the port is analysed from.
 *
 * The fix is to stop needing a scratch row at all: swap the two rows byte by
 * byte in place, which is width-agnostic and allocation-free. GL-free, so
 * tools/validate_postfx.py can execute it directly. */
void b3_postfx_flip_rows(unsigned char *px, int w, int h)
{
    int y, i;
    size_t stride;

    if (!px || w <= 0 || h <= 1) return;
    stride = (size_t)w * 4;
    for (y = 0; y < h / 2; y++) {
        unsigned char *a = px + (size_t)y * stride;
        unsigned char *b = px + (size_t)(h - 1 - y) * stride;
        for (i = 0; i < (int)stride; i++) {
            unsigned char t = a[i];
            a[i] = b[i];
            b[i] = t;
        }
    }
}

/* ==========================================================================
 * 3. GL SIDE
 * ========================================================================== */
#ifndef B3_POSTFX_NO_GL

static char   g_art_dir[512]  = "build/postfx";
static char   g_track_tag[64] = "US_C3_V1";

static B3SkyVertex   g_sky_v[B3_SKY_VERTS];
static B3SkyVertex   g_gnd_v[B3_SKY_VERTS];
static unsigned short g_sky_i[B3_SKY_INDICES];

static GLuint g_tex_clouds    = 0;   /* enviro.dat env+0x9C, 1024x128        */
static GLuint g_tex_frame     = 0;   /* frame grab for the blur              */
static int    g_frame_w = 0, g_frame_h = 0;
static int    g_ready = 0;
static int    g_verbose = 0;

/* The gradient sheet stays on the CPU (enviro.dat env+0x98, 32x32): it is the
 * SOURCE of FUN_001891F0's three blits, not the texture the dome samples. */
static unsigned char *g_grad_px = NULL;
static int            g_grad_w = 0, g_grad_h = 0;

/* The 64x32 LUT those blits produce — retail's DAT_004A1D04. */
static GLuint        g_tex_lut = 0;
static unsigned char g_lut_px[B3_SKY_LUT_W * B3_SKY_LUT_H * 4];
static float         g_lut_progress = -1.0f;
static int           g_lut_valid = 0;

/* The sun, out of enviro.dat env+0x80 (see the header). g_sun is already the
 * TO-SUN direction, i.e. the stored vector negated. */
static float g_sun[3] = {0.0f, 0.0f, 0.0f};
static float g_sun_az = 0.0f, g_sun_elev = 0.0f;
static int   g_have_sun = 0;

void b3_postfx_init(void)
{
    g_verbose = getenv("B3_POSTFX_VERBOSE") != NULL;
    b3_sky_build(1, g_sky_v, g_sky_i);
    b3_sky_build(0, g_gnd_v, NULL);
}

void b3_postfx_set_art(const char *dir, const char *track_tag)
{
    if (dir) {
        strncpy(g_art_dir, dir, sizeof(g_art_dir) - 1);
        g_art_dir[sizeof(g_art_dir) - 1] = '\0';
    }
    if (track_tag) {
        strncpy(g_track_tag, track_tag, sizeof(g_track_tag) - 1);
        g_track_tag[sizeof(g_track_tag) - 1] = '\0';
    }
}

/* Upload one extracted enviro.dat PNG. wrap_t is CLAMP for the cloud sheet so
 * the dome's tc1.v of -0.176 at the zenith does not wrap the cloud band back
 * over the top; retail's texture addressing mode is a stage state that was not
 * recovered [?]. */
static GLuint postfx_load_png(const char *name, GLint wrap_s, GLint wrap_t)
{
    char path[768];
    SDL_Surface *img, *rgba;
    GLuint tex = 0;

    snprintf(path, sizeof(path), "%s/%s_%s.png", g_art_dir, g_track_tag, name);
    img = IMG_Load(path);
    if (!img) {
        if (g_verbose)
            fprintf(stderr, "[postfx] missing %s (run tools/extract_postfx_art.py)\n",
                    path);
        return 0;
    }
    rgba = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(img);
    if (!rgba) return 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
    if (g_verbose)
        fprintf(stderr, "[postfx] loaded %s (%dx%d)\n", path, rgba->w, rgba->h);
    SDL_FreeSurface(rgba);
    return tex;
}

/* NOTE: retail masks the blur with the `radialblurmask` texture out of the
 * global dictionary (FUN_0002EBE0 @0x0002EC3C stores its handle at +0x40).
 * That dictionary is another module's extractor; until it is available the
 * blur below applies an equivalent radial ramp per vertex of a 12x12 screen
 * grid, which keeps the centre sharp and smears the edges exactly as the
 * reference captures show. GLUE, see docs/RE_POSTFX.md section 5. */

/* Load the gradient sheet into CPU memory. It is never bound as a texture:
 * the dome samples the 64x32 LUT the three blits build out of it. */
static int postfx_load_sheet(void)
{
    char path[768];
    SDL_Surface *img, *rgba;
    int y;

    snprintf(path, sizeof(path), "%s/%s_gradients.png", g_art_dir, g_track_tag);
    img = IMG_Load(path);
    if (!img) {
        if (g_verbose)
            fprintf(stderr, "[postfx] missing %s (run tools/extract_postfx_art.py)\n",
                    path);
        return 0;
    }
    rgba = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(img);
    if (!rgba) return 0;

    free(g_grad_px);
    g_grad_w  = rgba->w;
    g_grad_h  = rgba->h;
    g_grad_px = (unsigned char *)malloc((size_t)g_grad_w * g_grad_h * 4);
    if (!g_grad_px) { SDL_FreeSurface(rgba); g_grad_w = g_grad_h = 0; return 0; }
    for (y = 0; y < g_grad_h; y++)
        memcpy(g_grad_px + (size_t)y * g_grad_w * 4,
               (unsigned char *)rgba->pixels + (size_t)y * rgba->pitch,
               (size_t)g_grad_w * 4);
    if (g_verbose)
        fprintf(stderr, "[postfx] loaded %s (%dx%d) as the LUT source\n",
                path, g_grad_w, g_grad_h);
    SDL_FreeSurface(rgba);
    return 1;
}

/* The sun vector sidecar tools/extract_postfx_art.py writes from enviro.dat
 * env+0x80 (see the header). One line: "sun_dir <x> <y> <z>", in the GAME's
 * left-handed world frame and pointing the way the light TRAVELS. */
static void postfx_load_env(void)
{
    char path[768], line[256];
    FILE *f;

    g_have_sun = 0;
    snprintf(path, sizeof(path), "%s/%s_env.txt", g_art_dir, g_track_tag);
    f = fopen(path, "r");
    if (!f) {
        if (g_verbose)
            fprintf(stderr, "[postfx] no %s -- sky LUT passes 2/3 disabled\n", path);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        float x, y, z;
        if (sscanf(line, "sun_dir %f %f %f", &x, &y, &z) == 3) {
            /* [S] negate: env+0x80 is the direction of TRAVEL (y < 0 on all 40
             * shipped tracks); FUN_0019A3E0 flips the same block's signs too. */
            g_sun[0] = -x; g_sun[1] = -y; g_sun[2] = -z;
            g_have_sun = 1;
        }
    }
    fclose(f);
    if (g_have_sun) {
        b3_sky_sun_angles(g_sun, &g_sun_az, &g_sun_elev);
        if (g_verbose)
            fprintf(stderr, "[postfx] sun to (%.4f,%.4f,%.4f) az=%.4f elev=%.4f\n",
                    g_sun[0], g_sun[1], g_sun[2], g_sun_az, g_sun_elev);
    }
    if (getenv("B3_SKY_NOGLOW")) g_have_sun = 0;   /* documented A/B switch */
}

int b3_postfx_gl_init(void)
{
    if (!g_sky_v[0].color) b3_postfx_init();
    {
        const char *e = getenv("B3_POSTFX_SKYBLIT");
        if (e) b3_sky_lut_set_blit((float)atof(e));
    }
    g_ready = postfx_load_sheet();
    postfx_load_env();
    /* Clouds wrap twice around the dome in S; T clamps so the zenith's
     * tc1.v = -0.176 does not wrap the band back over the top. */
    g_tex_clouds = postfx_load_png("clouds", GL_REPEAT, GL_CLAMP_TO_EDGE);
    if (g_ready) {
        glGenTextures(1, &g_tex_lut);
        glBindTexture(GL_TEXTURE_2D, g_tex_lut);
        /* The LUT now VARIES with azimuth (pass 2/3), and its column 63 and
         * column 0 are adjacent azimuths, so S repeats. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        {int fl = getenv("B3_SKY_LUT_NEAREST") ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, fl);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fl);}
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, B3_SKY_LUT_W, B3_SKY_LUT_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, g_lut_px);
    }
    return g_ready;
}

/* Retail rebuilds the LUT EVERY frame (FUN_001891F0 is called from the world
 * draw). Nothing in it moves unless `progress` does, so rebuild on change --
 * 2048 texels of bilinear either way, but this keeps it off the per-frame
 * budget entirely at a standstill. */
static void postfx_lut_update(float progress)
{
    if (!g_ready) return;
    if (g_lut_valid && fabsf(progress - g_lut_progress) < 1.0f / 4096.0f) return;

    b3_sky_build_lut(g_grad_px, g_grad_w, g_grad_h, progress,
                     g_sun_az, g_sun_elev, g_have_sun, g_lut_px);
    glBindTexture(GL_TEXTURE_2D, g_tex_lut);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, B3_SKY_LUT_W, B3_SKY_LUT_H,
                    GL_RGBA, GL_UNSIGNED_BYTE, g_lut_px);
    g_lut_progress = progress;
    g_lut_valid = 1;
}

/* B3_SKY_RT_GAIN ships at IDENTITY now that the recovered blit colour carries
 * the sky's render-target scale (see the header). B3_POSTFX_SKYGAIN=<f> is the
 * override, and the LUT's own blit colour is B3_POSTFX_SKYBLIT=<f>. */
static float postfx_sky_gain(void)
{
    static float gain = -1.0f;
    if (gain < 0.0f) {
        const char *e = getenv("B3_POSTFX_SKYGAIN");
        gain = e ? (float)atof(e) : B3_SKY_RT_GAIN;
        if (gain < 0.0f) gain = 0.0f;
    }
    return gain;
}

/* Emit one dome as immediate-mode triangles. `unit` selects which texcoord
 * set feeds glTexCoord2f: 0 = the gradient LUT, 1 = the cloud sheet.
 * tc0.u is now the vertex's OWN azimuth: the LUT stopped being horizontally
 * uniform the moment passes 2 and 3 went in. */
static void postfx_emit_dome(const B3SkyVertex *v, int unit)
{
    int i;
    glBegin(GL_TRIANGLES);
    for (i = 0; i < B3_SKY_INDICES; i++) {
        const B3SkyVertex *p = &v[g_sky_i[i]];
        if (unit == 0) glTexCoord2f(p->tc0[0], p->tc0[1]);
        else           glTexCoord2f(p->tc1[0], p->tc1[1]);
        /* RE_NOTES 12: the harness mirrors the D3D left-handed world into GL
         * by negating Z on every mesh. The dome follows the same convention so
         * the cloud panorama runs the same way round as the world. */
        glVertex3f(p->pos[0], p->pos[1], -p->pos[2]);
    }
    glEnd();
}

void b3_postfx_sky_draw(const float eye[3], float far_clip, float progress)
{
    float scale;

    if (!g_ready || !eye) return;
    if (getenv("B3_SKY_OFF")) return;   /* TEMP diagnostic */

    /* [C] 0x000325AB: uniform scale = farClip - 1000, centred on the eye. */
    scale = far_clip - B3_SKY_FAR_MARGIN;
    if (scale < 1.0f) scale = far_clip * 0.9f;

    /* [C] FUN_001891F0 runs BEFORE the dome draw, from the same world-render
     * function (FUN_001AE340 @0x00189385, then the sky at 0x001AE3EA). */
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    postfx_lut_update(progress);

    /* [C] FUN_000323D0: alpha test off (RS 0x3B), alpha blend off (RS 0x3C),
     * ZWRITEENABLE off (RS 0x40), CULLMODE = D3DCULL_NONE (RS 0x93). */
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_TEXTURE_BIT
                 | GL_CURRENT_BIT | GL_POLYGON_BIT | GL_COLOR_BUFFER_BIT);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    /* [C] the dome shader's final combiner is ABCD = ZERO,ZERO,R0.rgb,ZERO
     * (def 0x003E9B08 +0x20) — the FOG register appears nowhere in it, unlike
     * the six world defs whose final combiner is 0x130C0300. The sky is never
     * fogged, so make sure a fog state left on by the world cannot touch it. */
    glDisable(GL_FOG);
    glDepthMask(GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glTranslatef(eye[0], eye[1], eye[2]);
    glScalef(scale, scale, scale);

    /* dome pass A — the 64x32 LUT over both hemispheres, opaque.
     *
     * The primary colour is now WHITE: the sky's render-target scale lives in
     * the LUT itself, as the recovered blit colour B3_SKY_LUT_BLIT_RGB, and
     * B3_SKY_RT_GAIN ships at identity (B3_POSTFX_SKYGAIN=<f> overrides).
     * Under GL_MODULATE this is exactly `gain * LUT.rgb`. */
    {
        float sg = postfx_sky_gain();
        glColor4f(sg, sg, sg, 1.0f);
    }
    glBindTexture(GL_TEXTURE_2D, g_tex_lut);
    postfx_emit_dome(g_gnd_v, 0);
    postfx_emit_dome(g_sky_v, 0);

    /* pass 2 — the cloud sheet over the sky half only.
     *
     * [C] THE DOME'S COMBINER, decoded in full. The pixel shader retail binds
     * for the dome is the renderer's slot `this+0x44C`; the renderer object
     * base is 0x004D6170 (its six world slots this+0x400..0x414 are exactly
     * the six addresses FUN_000393C0 reads at 0x004D6570..0x004D6584), so
     * DAT_004D65BC = this+0x44C, and FUN_0003C8A0 @0x0003D0B0 copies the
     * D3DPIXELSHADERDEF at 0x003E9B08 into it. That def decodes as:
     *
     *   PSTextureModes   = 0x00000021   -> TWO PROJECT2D samplers (T0, T1)
     *   PSCombinerCount  = 0x00011102   -> 2 stages
     *   s0 RGB  A=C0.rgb B=T1.rgb C=ZERO D=ZERO  -> R1.rgb = C0.rgb * T1.rgb
     *   s0 A    A=C0.a   B=T1.a                  -> R1.a   = C0.a   * T1.a
     *   s1 RGB  A=R1.rgb B=R1.a C=T0.rgb D=1-R1.a
     *                                -> R0.rgb = R1.rgb*R1.a + T0.rgb*(1-R1.a)
     *   final   ABCD = ZERO,ZERO,R0.rgb,ZERO     -> out.rgb = R0.rgb
     *
     * EVERY output shift field is 0 -- there is NO x2 anywhere in this shader,
     * unlike the six world defs. And FUN_00032580 @0x000327A7 sets pixel
     * shader constant C0 = (0.5, 0.5, 0.5, 1.0) right before the draw
     * (0x0034E9A0 is D3DDevice_SetPixelShaderConstant: it packs the float4 to
     * a D3DCOLOR and pushes NV097_SET_COMBINER_FACTOR0/1 0x0A60..0x0A9C).
     * Substituting C0:
     *
     *   out.rgb = 0.5*T1.rgb * T1.a  +  T0.rgb * (1 - T1.a)
     *
     * i.e. an ordinary alpha-over of the cloud sheet on the gradient with the
     * cloud colour HALVED. This code used to read the same 0.5 constant, infer
     * "MODULATE2X" from it, and then apply only the doubling with a white
     * primary colour -- 2*T1 where retail has 0.5*T1, a factor of FOUR too
     * bright on the cloud plates, which is what made our sky read ~161 median
     * against retail's ~144 (and ~195 vs ~155 in the open frames).
     *
     * GL_MODULATE against a primary colour of exactly retail's C0 reproduces
     * the recovered equation term for term: SRC_ALPHA/ONE_MINUS_SRC_ALPHA
     * gives dst = src.rgb*src.a + dst*(1-src.a), src.rgb = 0.5*T1.rgb and
     * src.a = 1.0*T1.a. */
    if (g_tex_clouds && !getenv("B3_SKY_NOCLOUD")) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindTexture(GL_TEXTURE_2D, g_tex_clouds);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        /* The recovered C0 = (0.5, 0.5, 0.5, 1.0), times the (now identity)
         * B3_SKY_RT_GAIN so the whole dome still scales together under the
         * B3_POSTFX_SKYGAIN override. */
        {
            float cg = B3_SKY_CLOUD_C0_RGB * postfx_sky_gain();
            glColor4f(cg, cg, cg, B3_SKY_CLOUD_C0_A);
        }
        postfx_emit_dome(g_sky_v, 1);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    }

    glPopMatrix();
    glPopAttrib();
    glDepthMask(GL_TRUE);
}

/* ------------------------------------------------- shared full-screen bits */

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL 0x813C
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191
#endif
#ifndef GL_LINEAR_MIPMAP_NEAREST
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif

/* ------------------------------------------------------------ GLSL loader
 *
 * Two passes want shaders now -- the present composite's tap accumulator and
 * the output gamma ramp -- so the entry points are resolved once, here. Both
 * degrade to their fixed-function paths when GL 2.0 is unavailable. */
static unsigned (*p_glCreateShader)(unsigned);
static void (*p_glShaderSource)(unsigned, int, const char* const*, const int*);
static void (*p_glCompileShader)(unsigned);
static void (*p_glGetShaderiv)(unsigned, unsigned, int*);
static void (*p_glGetShaderInfoLog)(unsigned, int, int*, char*);
static unsigned (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(unsigned, unsigned);
static void (*p_glLinkProgram)(unsigned);
static void (*p_glGetProgramiv)(unsigned, unsigned, int*);
static void (*p_glUseProgram)(unsigned);
static void (*p_glDeleteShader)(unsigned);
static void (*p_glDeleteProgram)(unsigned);
static int  (*p_glGetUniformLocation)(unsigned, const char*);
static void (*p_glUniform1i)(int, int);
static void (*p_glUniform1f)(int, float);
static void (*p_glActiveTexture)(unsigned);

static int postfx_glsl_load(void)
{
    static int state = -1;
    if (state >= 0) return state;
    state = 0;
#define B3PF_GET(fn) do { *(void**)(&p_##fn) = SDL_GL_GetProcAddress(#fn); \
                          if (!p_##fn) return 0; } while (0)
    B3PF_GET(glCreateShader);   B3PF_GET(glShaderSource);
    B3PF_GET(glCompileShader);  B3PF_GET(glGetShaderiv);
    B3PF_GET(glGetShaderInfoLog);
    B3PF_GET(glCreateProgram);  B3PF_GET(glAttachShader);
    B3PF_GET(glLinkProgram);    B3PF_GET(glGetProgramiv);
    B3PF_GET(glUseProgram);     B3PF_GET(glDeleteShader);
    B3PF_GET(glDeleteProgram);  B3PF_GET(glGetUniformLocation);
    B3PF_GET(glUniform1i);      B3PF_GET(glUniform1f);
    B3PF_GET(glActiveTexture);
#undef B3PF_GET
    state = 1;
    return 1;
}

/* Compile one fragment-only program. Returns 0 on any failure. */
static unsigned postfx_build_fs(const char *src, const char *what)
{
    unsigned fs, pr;
    int ok = 0;

    fs = p_glCreateShader(GL_FRAGMENT_SHADER);
    p_glShaderSource(fs, 1, &src, NULL);
    p_glCompileShader(fs);
    p_glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        int n = 0;
        p_glGetShaderInfoLog(fs, (int)sizeof log, &n, log);
        log[(n > 0 && n < (int)sizeof log) ? n : 0] = 0;
        fprintf(stderr, "[postfx] %s shader failed: %s\n", what, log);
        p_glDeleteShader(fs);
        return 0;
    }
    pr = p_glCreateProgram();
    p_glAttachShader(pr, fs);
    p_glLinkProgram(pr);
    p_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    p_glDeleteShader(fs);
    if (!ok) { p_glDeleteProgram(pr); return 0; }
    return pr;
}

/* Push the 2D state both full-screen passes want: an identity 0..1 ortho, no
 * depth, no lighting, no culling, no alpha test. Paired with postfx_2d_end. */
static void postfx_2d_begin(void)
{
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_TEXTURE_BIT
                 | GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT | GL_VIEWPORT_BIT);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

static void postfx_2d_end(void)
{
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glPopAttrib();
    glDepthMask(GL_TRUE);
}

/* One screen-covering quad. `z` zooms the texture coordinates about the
 * recovered (0.5, 0.5) centre; z == 1 is the untransformed frame. */
static void postfx_quad(float z)
{
    const float c = B3_BLUR_CENTER_X, d = B3_BLUR_CENTER_Y;
    glBegin(GL_QUADS);
    glTexCoord2f(c + (0.0f - c) * z, d + (0.0f - d) * z); glVertex2f(0.0f, 0.0f);
    glTexCoord2f(c + (1.0f - c) * z, d + (0.0f - d) * z); glVertex2f(1.0f, 0.0f);
    glTexCoord2f(c + (1.0f - c) * z, d + (1.0f - d) * z); glVertex2f(1.0f, 1.0f);
    glTexCoord2f(c + (0.0f - c) * z, d + (1.0f - d) * z); glVertex2f(0.0f, 1.0f);
    glEnd();
}

/* The same quad, tessellated so a per-vertex weight can stand in for retail's
 * `radialblurmask` texture (FUN_0002EBE0 @0x0002EC3C stores its handle at
 * blurState+0x40; the texture itself lives in a global dictionary this port
 * does not extract). Sharp centre, full weight at the edge. GLUE shape.
 * `k` scales the weight; `rgb` selects whether the weight goes to the colour
 * (additive blending) or to alpha (an over blend). */
static void postfx_quad_masked(float z, float k, int weight_is_rgb)
{
    const int G = 12;
    static const float cx[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    static const float cy[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    int gx, gy, c;

    glBegin(GL_QUADS);
    for (gy = 0; gy < G; gy++) {
        for (gx = 0; gx < G; gx++) {
            for (c = 0; c < 4; c++) {
                float px = ((float)gx + cx[c]) / (float)G;
                float py = ((float)gy + cy[c]) / (float)G;
                float dx = px - B3_BLUR_CENTER_X;
                float dy = py - B3_BLUR_CENTER_Y;
                float r  = sqrtf(dx * dx + dy * dy) / 0.7071068f;
                float m  = (r - B3_BLUR_MASK_R0) / (1.0f - B3_BLUR_MASK_R0);
                if (m < 0.0f) m = 0.0f;
                if (m > 1.0f) m = 1.0f;
                m = powf(m, B3_BLUR_MASK_POW) * k;
                if (weight_is_rgb) glColor4f(m, m, m, 1.0f);
                else               glColor4f(1.0f, 1.0f, 1.0f, m);
                glTexCoord2f(B3_BLUR_CENTER_X + dx * z,
                             B3_BLUR_CENTER_Y + dy * z);
                glVertex2f(px, py);
            }
        }
    }
    glEnd();
}

/* Copy the back buffer into g_tex_frame. The texture asks the driver to keep
 * its mip chain current (GL_GENERATE_MIPMAP, core since GL 1.4) because the
 * blur half of the composite samples level B3_PRESENT_BLUR_LOD — retail's
 * 160x120 surface is exactly level 2 of its 640x480 one. */
static void postfx_grab_frame(int w, int h)
{
    if (!g_tex_frame || g_frame_w != w || g_frame_h != h) {
        if (!g_tex_frame) glGenTextures(1, &g_tex_frame);
        glBindTexture(GL_TEXTURE_2D, g_tex_frame);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                        B3_PRESENT_BLUR_LOD);
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, w, h, 0);
        g_frame_w = w; g_frame_h = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, g_tex_frame);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    }
}

/* ---------------------------------------------------- the present composite
 *
 * [C] FUN_0003DA90. One full-screen quad, blending OFF, pixel shader
 * 0x003E9EA8:
 *
 *     out.rgb = 2 * ( T0.rgb + C0.a * T1.rgb )
 *
 * T0 = the 640x480 scene surface (renderer+0x890, the render target the whole
 * frame was drawn into), T1 = the 160x120 radially-zoomed reduction of it
 * (renderer+0x8D8, built by FUN_0003E520), C0.a = b3_postfx_present_alpha(s).
 *
 * The harness draws straight to the window, so "T0" is the back buffer as it
 * stands when this is called (all 3D, no HUD — retail's HUD is drawn after
 * FUN_0003DA90 as well, at 0x001AE620 onward, so it is not doubled either).
 * The composite is assembled with fixed-function blending in three steps,
 * which is arithmetically identical to the combiner including the clamp:
 *
 *   1. blend off,           quad at LOD 0            -> dst = frame
 *   2. blend ONE/ONE,       N quads at LOD 2, zoom
 *      0.99^k, colour a/N                            -> dst = frame + a*blur
 *   3. blend DST_COLOR/ONE, untextured white quad    -> dst = 2 * dst
 *
 * Step 3's clamp is the only one that bites: clamp(2*clamp(x,0,1),0,1) ==
 * clamp(2x,0,1) for x >= 0, so the intermediate saturation in step 2 cannot
 * change the result. Retail's CLAMP_SUM bit (final combiner setting 0x80 in
 * the def) is the same single clamp.
 *
 * GLUE inside the composite: the tap count and the per-tap weights of the
 * radial accumulation (retail's third reduction pass masks its taps with the
 * `radialblurmask` texture, which lives in a texture dictionary this port
 * does not extract), and the speed -> s law. The x2, the LOD, the 0.99 zoom,
 * the (0.5,0.5) centre and the alpha law are all recovered.
 *
 * THE x2 IS NOW ON BY DEFAULT (B3_POSTFX_PRESENT=0 turns it off)
 * --------------------------------------------------------------
 * TUNED (user-authorized deviation 2026-08-13) -- the DEFAULT flipped, not the
 * recovered composite, which is untouched.
 *
 * The x2 is real -- see the header, and it is the same OP field that settles
 * the world's x2 -- and it means retail's SCENE is authored to live in the
 * bottom half of the range, with the present pass expanding it. When it was
 * first recovered it shipped OFF, because our port was not in render-target
 * space: measured against the xemu captures (scratchpad present/qq.py,
 * percentile-for-percentile on the pre-ramp values),
 *
 *     retail_screen / ours  =  1.33 .. 1.67  on the ground band
 *                              1.20 .. 1.34  on the sky/backdrop band
 *
 * so with retail_screen = 2 x retail_RT our frame sat at 1.2x..1.7x of
 * retail's RENDER TARGET, and the x2 clipped 21% of the frame where the retail
 * captures clip 0.11%. The excess was localised and measured band by band:
 * the sky clipped 57%, the mid band 10%, the near road 4.7%.
 *
 * This wave puts the port INTO render-target space by bringing those two down
 * to meet the composite instead of leaving the recovered pass switched off:
 *
 *   * the sky dome's output is scaled by B3_SKY_RT_GAIN (below) -- it was
 *     calibrated against xemu FINAL frames, i.e. against post-x2 pixels, so it
 *     was carrying the x2 already;
 *   * the world's baked vertex colour gets a matching headroom curve in
 *     src/burnout3_trackmesh.c (TRACKMESH_LIFT_G), same reason.
 *
 * Both are magnitudes on already-recovered equations, both are marked TUNED at
 * their line, and both are judged by eye against the reference captures with
 * the composite and the ^0.95 ramp in place -- which is the only place the
 * comparison is meaningful, because that is what the references are.
 */
/* ------------------------------------------ the tap accumulator, in ONE draw
 *
 * THE ARC BUG. The 16 zoomed taps used to be 16 separate GL_ONE/GL_ONE quads
 * straight into the 8-bit back buffer, so every tap's contribution was rounded
 * to an integer BEFORE it was summed. In a smooth region all 16 taps carry
 * essentially the same value, so they all cross the same round-to-0 / round-
 * to-1 threshold at the same pixel and the sum jumps by the whole TAP COUNT at
 * once: measured on a flat background (B3_SKY_OFF=1, frame 3900) the red
 * channel held exact plateaus of 76, 92, 108 -- steps of 16 == B3_BLUR_TAPS.
 * Step 3's x2 doubles them to 32 and the ^0.95 ramp lands them on 156 / 187 /
 * 218, which is the hard-edged arc the user reported. It reads as CIRCULAR
 * because the plateaus are iso-contours of the radial mask, and as SUN-LINKED
 * because the threshold is crossed at m == 8/(alpha*frame): the brighter the
 * local sky, the smaller the radius at which the step lands, so the contour
 * bows around the bright part of the sky. It is rainbow-coloured because R, G
 * and B have different local values and therefore step at different radii.
 *
 * This is a PORTING artefact, not retail behaviour: retail never accumulates
 * into the frame. FUN_0003E520 builds the whole radial reduction in its own
 * 160x120 surface and FUN_0003DA90 then composites it once
 * (out.rgb = 2*(T0.rgb + C0.a*T1.rgb), PS 0x003E9EA8), so retail quantises
 * ONCE, at the end. Summing the taps in a fragment shader restores that: one
 * rounding instead of sixteen, i.e. the step drops from 16 LSB to 1 (2 after
 * the x2), which is below the ramp's own resolution.
 *
 * The maths is the SAME maths -- same B3_BLUR_TAPS, same B3_BLUR_ZOOM_A per
 * tap, same recovered (0.5,0.5) centre, same GLUE mask -- so the shader is
 * built from the very macros postfx_quad_masked() uses. The one difference is
 * that the mask is evaluated per FRAGMENT instead of at the corners of the
 * 12x12 grid postfx_quad_masked() tessellates, which also removes the grid's
 * faceting. B3_POSTFX_TAPSHADER=0 forces the old 16-quad path back for A/B,
 * and it is also the automatic fallback when GL 2.0 is unavailable. */
static int      g_taps_state = -1;     /* -1 untried, 0 unavailable, 1 ready */
static unsigned g_taps_prog;
static int      g_taps_w_loc = -1;

static int postfx_taps_init(void)
{
    char fs[1200];
    unsigned pr;
    int loc;

    if (g_taps_state >= 0) return g_taps_state;
    g_taps_state = 0;
    if (getenv("B3_POSTFX_TAPSHADER")
        && atoi(getenv("B3_POSTFX_TAPSHADER")) == 0) return 0;
    if (!postfx_glsl_load()) return 0;

    snprintf(fs, sizeof fs,
        "uniform sampler2D uFrame;\n"
        "uniform float uW;\n"                     /* C0.a / B3_BLUR_TAPS     */
        "void main() {\n"
        "  vec2 c = vec2(%.9g, %.9g);\n"          /* [C] the (0.5,0.5) centre */
        "  vec2 d = gl_TexCoord[0].xy - c;\n"
        "  float r = length(d) / 0.7071068;\n"
        "  float m = clamp((r - %.9g) / (1.0 - %.9g), 0.0, 1.0);\n"
        "  m = pow(m, %.9g) * uW;\n"
        "  vec3 acc = vec3(0.0);\n"
        "  float z = 1.0;\n"
        "  for (int i = 0; i < %d; i++) {\n"      /* B3_BLUR_TAPS            */
        "    z *= %.9g;\n"                        /* [C] B3_BLUR_ZOOM_A      */
        "    acc += texture2D(uFrame, c + d * z).rgb;\n"
        "  }\n"
        "  gl_FragColor = vec4(acc * m, 1.0);\n"
        "}\n",
        (double)B3_BLUR_CENTER_X, (double)B3_BLUR_CENTER_Y,
        (double)B3_BLUR_MASK_R0,  (double)B3_BLUR_MASK_R0,
        (double)B3_BLUR_MASK_POW, B3_BLUR_TAPS, (double)B3_BLUR_ZOOM_A);

    pr = postfx_build_fs(fs, "present tap");
    if (!pr) return 0;

    p_glUseProgram(pr);
    loc = p_glGetUniformLocation(pr, "uFrame");
    if (loc >= 0) p_glUniform1i(loc, 0);
    g_taps_w_loc = p_glGetUniformLocation(pr, "uW");
    p_glUseProgram(0);

    g_taps_prog  = pr;
    g_taps_state = 1;
    if (g_verbose) fprintf(stderr, "[postfx] present tap shader ready\n");
    return 1;
}

void b3_postfx_blur(int w, int h, float speed_mph, float boost_ramp,
                    float real_dt)
{
    float s, alpha;
    int tap;
    static int   knobs = 0;
    static float blur_scale = 1.0f;
    /* TUNED (user-authorized deviation 2026-08-13): default ON -- see above */
    static int   present_on = 1;

    (void)real_dt;                     /* the effect is instantaneous         */

    if (w <= 0 || h <= 0) return;

    if (!knobs) {
        const char *e = getenv("B3_POSTFX_BLUR");
        const char *p = getenv("B3_POSTFX_PRESENT");
        if (e) blur_scale = (float)atof(e);
        if (p) present_on = atoi(p) != 0;
        knobs = 1;
    }

    s     = b3_postfx_blur_strength(speed_mph, boost_ramp) * blur_scale;
    alpha = b3_postfx_present_alpha(s);

    /* With the x2 disabled and no blur there is nothing to do at all. */
    if (!present_on && alpha <= 0.002f) return;

    postfx_grab_frame(w, h);
    postfx_2d_begin();
    glBindTexture(GL_TEXTURE_2D, g_tex_frame);

    if (present_on) {
        /* step 1 — the sharp frame, opaque */
        glDisable(GL_BLEND);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        postfx_quad(1.0f);

        /* step 2 — the radial accumulation, at the recovered 1/4 resolution,
         * weighted by the `radialblurmask` stand-in so the centre of the
         * screen stays at exactly 2x frame and only the edges gain. */
        if (alpha > 0.002f) {
            const float wgt = alpha / (float)B3_BLUR_TAPS;
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            /* BASE_LEVEL selects the recovered 1/4-res reduction for the
             * shader exactly as it does for the 16-quad path, so both sample
             * the very same texels. */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL,
                            B3_PRESENT_BLUR_LOD);
            if (postfx_taps_init()) {
                /* ONE draw, one rounding -- see postfx_taps_init(). */
                p_glUseProgram(g_taps_prog);
                if (g_taps_w_loc >= 0) p_glUniform1f(g_taps_w_loc, wgt);
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
                postfx_quad(1.0f);
                p_glUseProgram(0);
            } else {
                for (tap = 1; tap <= B3_BLUR_TAPS; tap++)
                    postfx_quad_masked(powf(B3_BLUR_ZOOM_A, (float)tap),
                                       wgt, 1);
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        }

        /* step 3 — the combiner's SHIFTLEFTBY1 */
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_ONE);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        postfx_quad(1.0f);
        glEnable(GL_TEXTURE_2D);
    } else {
        /* B3_POSTFX_PRESENT=0: the pre-recovery behaviour, an alpha-over of
         * the zoomed taps with no x2. Kept so the wave's before/after
         * measurements can be reproduced from one binary. */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (tap = 1; tap <= B3_BLUR_TAPS; tap++)
            postfx_quad_masked(powf(B3_BLUR_ZOOM_A, (float)tap),
                               2.0f * alpha / (float)(tap + 1), 0);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    }

    postfx_2d_end();
}

/* ------------------------------------------------------- the gamma ramp pass
 *
 * The harness half of D3DDevice_SetGammaRamp. SDL_SetWindowGammaRamp cannot
 * be used: it is a no-op on every Wayland compositor and on most X11 setups,
 * and its return value used to be ignored — so the live window rendered with
 * no ramp at all while every analysed screenshot had it applied in software.
 * Doing it here, in the GL present path, means the window and both
 * glReadPixels captures are literally the same pixels.
 *
 * The table is uploaded as a 256x1 GL_NEAREST texture and used as a dependent
 * lookup, so the 8-bit quantisation of retail's ramp (a rounded byte table,
 * not a continuous pow) is reproduced exactly: for an 8-bit frame the channel
 * value is i/255, and floor((i/255)*256) == i for every i in 0..255.
 */
static const char *POSTFX_GAMMA_FS =
    "uniform sampler2D uFrame;\n"
    "uniform sampler2D uRamp;\n"
    "void main() {\n"
    "  vec3 c = texture2D(uFrame, gl_TexCoord[0].xy).rgb;\n"
    "  gl_FragColor = vec4(texture2D(uRamp, vec2(c.r, 0.5)).r,\n"
    "                      texture2D(uRamp, vec2(c.g, 0.5)).r,\n"
    "                      texture2D(uRamp, vec2(c.b, 0.5)).r, 1.0);\n"
    "}\n";

static int      g_gamma_state = -1;    /* -1 untried, 0 unavailable, 1 ready */
static unsigned g_gamma_prog;
static GLuint   g_tex_ramp;

static int postfx_gamma_init(void)
{
    unsigned char ramp[256];
    unsigned char rgba[256 * 4];
    unsigned pr;
    int i, loc;

    if (g_gamma_state >= 0) return g_gamma_state;
    g_gamma_state = 0;
    if (getenv("B3_NOGAMMA")) return 0;
    if (!postfx_glsl_load()) return 0;

    pr = postfx_build_fs(POSTFX_GAMMA_FS, "gamma");
    if (!pr) return 0;

    b3_postfx_gamma_table(ramp);
    for (i = 0; i < 256; i++) {
        rgba[i * 4 + 0] = ramp[i];
        rgba[i * 4 + 1] = ramp[i];
        rgba[i * 4 + 2] = ramp[i];
        rgba[i * 4 + 3] = 255;
    }
    glGenTextures(1, &g_tex_ramp);
    glBindTexture(GL_TEXTURE_2D, g_tex_ramp);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    p_glUseProgram(pr);
    loc = p_glGetUniformLocation(pr, "uFrame"); if (loc >= 0) p_glUniform1i(loc, 0);
    loc = p_glGetUniformLocation(pr, "uRamp");  if (loc >= 0) p_glUniform1i(loc, 1);
    p_glUseProgram(0);

    g_gamma_prog  = pr;
    g_gamma_state = 1;
    if (g_verbose) fprintf(stderr, "[postfx] gamma ramp pass ready\n");
    return 1;
}

int b3_postfx_gamma(int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    if (!postfx_gamma_init()) return 0;

    /* Reuse the frame grab. The composite above may already have run this
     * frame, but the back buffer has changed since (the HUD), so re-copy. */
    postfx_grab_frame(w, h);

    postfx_2d_begin();
    p_glActiveTexture(0x84C1 /*GL_TEXTURE1*/);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_ramp);
    p_glActiveTexture(0x84C0 /*GL_TEXTURE0*/);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex_frame);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glDisable(GL_BLEND);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    p_glUseProgram(g_gamma_prog);
    postfx_quad(1.0f);
    p_glUseProgram(0);

    p_glActiveTexture(0x84C1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    p_glActiveTexture(0x84C0);
    postfx_2d_end();
    return 1;
}

#else  /* B3_POSTFX_NO_GL — validator probe build */

void b3_postfx_init(void) {}
void b3_postfx_set_art(const char *dir, const char *tag) { (void)dir; (void)tag; }
int  b3_postfx_gl_init(void) { return 0; }
void b3_postfx_sky_draw(const float eye[3], float f, float p)
{ (void)eye; (void)f; (void)p; }
void b3_postfx_blur(int w, int h, float s, float b, float dt)
{ (void)w; (void)h; (void)s; (void)b; (void)dt; }
int  b3_postfx_gamma(int w, int h) { (void)w; (void)h; return 0; }

#endif
