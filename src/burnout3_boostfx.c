/* Boost exhaust flame -- see burnout3_boostfx.h for the contract and
 * docs/RE_BOOSTFX.md for the full evidence chain.
 *
 * Every numeric constant below carries the VA it was read out of
 * build/burnout3.elf with.  Marks: [C] byte/execution-verified, [S] read from
 * the disassembly, [?] open, GLUE = this port's own bridging.
 *
 * The whole system in the retail image:
 *
 *   FUN_0017F730 (0x0017F730)     per-car FX dispatcher
 *     +-- FUN_00187C70            the light coronas (docs/RE_CARFX.md 4)
 *     +-- FUN_001871E0            THE FLAME, gated on carObj+0x18FA == 0
 *           +-- FUN_00179F30      size + colour + pool from carObj+0x11B0
 *           +-- FUN_00042B00 x3   push one sprite into the pool  (x N records)
 *   FUN_0017F7E0 -> FUN_00042BC0  the pool's own quad draw, once per pool
 *
 * so the flame shares its sprite pool machinery with the coronas and differs
 * only in (a) which light-table type supplies the positions -- 8, the
 * tailpipes -- (b) which pool/texture -- 1 `coronaboost` or 2
 * `coronaboostred` -- and (c) the three-sprite, halving cascade this file
 * reproduces.
 */
#include "burnout3_boostfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <GL/gl.h>

/* ======================================================================
 * 1. Recovered constants
 * ====================================================================== */

/* ---- FUN_00179F30 (0x00179F30): level -> {colour, size, pool} ----------
 * __regparm3(EAX = out float[6], ECX = boostRecord (= carObj+0x119C),
 *            [esp+4] = the flame-type byte carObj+0x1901).
 * Its input is `*(float*)(ECX + 0x14)` = carObj+0x11B0.                 [C] */
#define B3BF_K_BLUE   0.6000000238418579f  /* 0x003B16EC @0x00179F6C, pool 1 */
#define B3BF_K_RED    0.7200000286102295f  /* 0x003A2D7C @0x00179F54, pool 2 */
/* the pool index the branch stores into out[5] (`MOV [EAX+0x14],1` /
 * `...,2` @0x00179F74 / 0x00179F5C) is what selects the texture         [C] */
/* the two colour float4s the branch picks between                        [C] */
static const float B3BF_COL_BLUE[3] = {                    /* 0x00415CC0 */
    0.699999988079071f, 0.7200000286102295f, 0.75f };
static const float B3BF_COL_RED[3]  = {                    /* 0x00415CD0 */
    0.800000011920929f, 0.800000011920929f, 0.800000011920929f };

/* ---- FUN_001871E0 (0x001871E0) ----------------------------------------
 * COMISS against 0x003A7ED8 @0x0018720F/0x00187220: nothing is emitted
 * unless size > 0.01.                                                   [C] */
#define B3BF_SIZE_GATE   0.009999999776482582f   /* 0x003A7ED8 */

/* The three sprites of the cascade.  Every one of these twelve numbers is a
 * literal the function loads; the VAs are in the comments.               [C]
 *
 *   dist : offset along the emitter's outward normal
 *              0x0039C16C = 0.08   (sprite 0)
 *              0x003B1B28 = 0.14   (sprite 1)
 *              0x003B1B24 = 0.18   (sprite 2)
 *   size : FUN_00042B00's arg3 is {S, 0.5S, 0.25S} (0x003B1684 = 0.5,
 *          0x003B1730 = 0.25, 0x003B1728 = 0.125) and the pool record stores
 *          arg3*0.5 @0x00042B64, so the stored half-size is {0.5,0.25,0.125}S
 *   pull : the record's +0x14, arg4 = {0.5S, 0.25S, 0.125S} -- i.e. equal to
 *          the half-size.  FUN_00042BC0 builds the quad as
 *              corner = pos +- right*size +- up*size - (pos-eye)*pull
 *          so `pull` slides the sprite that fraction of the way to the eye,
 *          exactly as the corona's constant 0.5 does.
 *   cmin/cspan : the colour is `C * U(cmin, cmin+cspan)` -- three bands, each
 *          half the previous: [0.5,1.0], [0.28,0.56], [0.14,0.28]
 *          (0x003B1684 = 0.5, 0x003A5A58 = 0.28, 0x003B1B2C = 0.56,
 *           0x003B1B28 = 0.14).  THIS RANDOM IS THE FLICKER.
 *   far  : the pool record's +0x0C, FUN_00042BC0's per-sprite far cut
 *          (0x44480000 = 800, 0x43C80000 = 400, 0x43480000 = 200)          */
typedef struct {
    float dist, size, pull, cmin, cspan, far_cut;
} B3BFSprite;

static const B3BFSprite B3BF_SPRITE[3] = {
    { 0.07999999821186066f, 0.5f,   0.5f,   0.5f,  0.5f,  800.0f },
    { 0.14000000059604645f, 0.25f,  0.25f,  0.28f, 0.28f, 400.0f },
    { 0.18000000715255737f, 0.125f, 0.125f, 0.14f, 0.14f, 200.0f },
};
#define B3BF_NSPRITE 3

/* ---- FUN_0017A480's tail (0x001797E0..0x001798F2): the level -----------
 * DECAY_OFF is the `- dt*2.0` of the not-boosting branch and FLARE the
 * 0x40000000 the ignition path stores.  DECAY_ON / FLOOR are the
 * `- dt*2.5` / `<= 1.0 -> 1.0` of the boosting branch.                   [C] */
#define B3BF_DECAY_OFF   2.0f
#define B3BF_DECAY_ON    2.5f
#define B3BF_FLOOR       1.0f
#define B3BF_FLARE       2.0f

/* ======================================================================
 * 2. Module state
 * ====================================================================== */
typedef struct { float pos[3], nrm[3]; } B3BFEmitter;

typedef struct {
    B3BFEmitter e[8];        /* type-8 records; the shipped max is 4      */
    int         n;
    int         red;         /* carObj+0x1901 -- see B3BF_RED_CARS        */
    float       level;       /* carObj+0x11B0                             */
} B3BFCar;

/* carObj+0x1901, the pool selector, is a PER-CAR-MODEL CONSTANT, not a
 * gameplay state.  The car-object loader FUN_0018D0E0 (the same function that
 * builds "<car>.bgv" and seeds carObj+0x10 with the identity) clears it and
 * then sets it to 1 for exactly five packed car ids, comparing the 64-bit
 * base-40 name as an (EAX,ECX) dword pair:
 *
 *   0x0018D4CB  MOV byte ptr [EBX + 0x1901],0x0    ; the default
 *   0x0018D4C6/D4D4/D4DC  EAX 0x95EE2E00 ECX 0x5B55839B -> "COMPCAR10"
 *   0x0018D4E3/D4EA/D4F2  EAX 0x81EE2E00 ECX 0x961647D1 -> "MSCLCAR10"
 *   0x0018D4F9/D500/D508  EAX 0xE2EE2E00 ECX 0x5C3791C1 -> "CUPECAR10"
 *   0x0018D50F/D516/D51E  EAX 0x21EE2E00 ECX 0xB8A15FE9 -> "SPRTCAR10"
 *   0x0018D525/D52C/D534  EAX 0x4FEE2E00 ECX 0xB959BADE -> "SUPRCAR10"
 *
 * so the ORANGE `coronaboostred` flame is the badge of the five Car10
 * specials and every other car burns blue-white through `coronaboost`.  The
 * ids decode with the repo's own base-40 alphabet (tools/validate_sfx.py
 * b40 / tools/extract_traffic.py b40).                                   [C] */
static const char* const B3BF_RED_CARS[] = {
    "COMPCAR10", "MSCLCAR10", "CUPECAR10", "SPRTCAR10", "SUPRCAR10", 0
};

static struct {
    B3BFCar  car[B3_BOOSTFX_MAX_CARS];
    GLuint   tex_blue, tex_red;   /* pools 1 and 2                        */
    GLuint   bound;
    int      ready;
    /* FUN_001871E0 seeds its per-record generator from a global counter at
     * [renderCtx+0x64550] (`MOV EAX,[EDI+0x64550]` @0x00187240, then
     * `NOT EAX`), so the flicker re-rolls every frame.  The counter itself
     * is GLUE here; the generator below is the game's.                    */
    unsigned seed_ctr;
    int      rng_x, rng_c;
} g;

/* The game's own inline generator, verbatim in shape          [C]
 *     x = (x >> 16) + (x << 16) + carry;   carry += x;
 *     u = (unsigned)x * DAT_0054F46C
 * DAT_0054F46C is BSS so its value is [S]: 2^-32, which is the scale
 * FUN_0014A6B0 uses for the identical `(unsigned)LCG * scale` idiom
 * (docs/RE_SFX.md 2). */
static float b3bf_rand01(void)
{
    int x = (g.rng_x >> 16) + (int)((unsigned)g.rng_x << 16) + g.rng_c;
    g.rng_x = x;
    g.rng_c += x;
    return (float)((unsigned)x) * 2.3283064365386963e-10f;   /* 2^-32 */
}

/* ======================================================================
 * 3. Art
 * ====================================================================== */
static GLuint load_png(const char* path)
{
    SDL_Surface* img = IMG_Load(path);
    if (!img) return 0;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(img);
    if (!rgba) return 0;
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
    SDL_FreeSurface(rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

void b3_boostfx_init(void)
{
    memset(&g, 0, sizeof g);
    g.rng_x = 0x1234567;
    g.rng_c = 0x89ABCDE;
    /* pool 1 / pool 2 of the table at 0x003A3E7C; FUN_0017EE00 creates them */
    g.tex_blue = load_png("build/boostfx/coronaboost.png");
    g.tex_red  = load_png("build/boostfx/coronaboostred.png");
    g.ready = (g.tex_blue != 0);
    printf("[boostfx] flame=%s (coronaboost=%s coronaboostred=%s)\n",
           g.ready ? "on" : "OFF",
           g.tex_blue ? "on" : "missing", g.tex_red ? "on" : "missing");
    if (!g.ready)
        printf("[boostfx] run: python3 tools/extract_boostfx_art.py\n");
}

int b3_boostfx_ready(void) { return g.ready; }

void b3_boostfx_shutdown(void)
{
    if (g.tex_blue) glDeleteTextures(1, &g.tex_blue);
    if (g.tex_red)  glDeleteTextures(1, &g.tex_red);
    memset(&g, 0, sizeof g);
}

/* ======================================================================
 * 4. Per-car emitters -- corona light table type 8
 * ====================================================================== */
int b3_boostfx_load_car(int slot, const char* cls, const char* base)
{
    if (slot < 0 || slot >= B3_BOOSTFX_MAX_CARS) return 0;
    char path[192];
    snprintf(path, sizeof path, "build/cars/%s_%s.lights", cls, base);
    FILE* f = fopen(path, "r");
    B3BFCar* c = &g.car[slot];
    float keep = c->level;
    memset(c, 0, sizeof *c);
    c->level = keep;
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        int t;
        char name[32];
        float px, py, pz, nx, ny, nz;
        if (sscanf(line, "light %d %31s %f %f %f %f %f %f",
                   &t, name, &px, &py, &pz, &nx, &ny, &nz) != 8) continue;
        if (t != 8) continue;      /* model+0x1684 / +0x16B4 -- the exhaust */
        if (c->n >= (int)(sizeof c->e / sizeof c->e[0])) continue;
        B3BFEmitter* e = &c->e[c->n++];
        /* loader Z-flip (RE_NOTES 12), identical to b3_carfx_load_car: every
         * mesh this harness loads is mirrored in Z, so the model-space table
         * must be too. */
        e->pos[0] = px; e->pos[1] = py; e->pos[2] = -pz;
        e->nrm[0] = nx; e->nrm[1] = ny; e->nrm[2] = -nz;
    }
    fclose(f);
    c->red = b3_boostfx_car_is_red(cls, base);
    return c->n;
}

int b3_boostfx_emitters(int slot)
{
    if (slot < 0 || slot >= B3_BOOSTFX_MAX_CARS) return 0;
    return g.car[slot].n;
}

int b3_boostfx_car_is_red(const char* cls, const char* base)
{
    char id[32];
    size_t n = 0;
    for (const char* s = cls; s && *s && n + 1 < sizeof id; s++)
        id[n++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
    for (const char* s = base; s && *s && n + 1 < sizeof id; s++)
        id[n++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
    id[n] = '\0';
    for (int i = 0; B3BF_RED_CARS[i]; i++)
        if (!strcmp(id, B3BF_RED_CARS[i])) return 1;
    return 0;
}

int b3_boostfx_car_red(int slot)
{
    if (slot < 0 || slot >= B3_BOOSTFX_MAX_CARS) return 0;
    return g.car[slot].red;
}

/* ======================================================================
 * 5. The level -- FUN_0017A480's tail
 * ====================================================================== */
void b3_boostfx_update(int slot, int boosting, int crashed, float dt)
{
    if (slot < 0 || slot >= B3_BOOSTFX_MAX_CARS) return;
    B3BFCar* c = &g.car[slot];
    /* FUN_0017F730 @0x0017F73C: a crashed car (carObj+0x18FA) never runs the
     * emitter.  Retail leaves the level where it is and simply stops drawing;
     * doing the same keeps the fade-in on recovery identical.            [C] */
    if (crashed) { c->level = 0.0f; return; }

    /* fxByte := boosting.  The retail gate is (*(veh+0xCC4))+0x1033, whose
     * writer was not chased -> [S] for the substitution, [C] for the two
     * decay rates, the 1.0 floor and the 2.0 flare. */
    if (!boosting) {
        float v = c->level - B3BF_DECAY_OFF * dt;
        c->level = (v <= 0.0f) ? 0.0f : v;
        return;
    }
    if (c->level == 0.0f) c->level = B3BF_FLARE;    /* MOV 0x40000000 */
    {
        float v = c->level - B3BF_DECAY_ON * dt;
        c->level = (v <= B3BF_FLOOR) ? B3BF_FLOOR : v;
    }
}

float b3_boostfx_level(int slot)
{
    if (slot < 0 || slot >= B3_BOOSTFX_MAX_CARS) return 0.0f;
    return g.car[slot].level;
}

void b3_boostfx_reset(int slot)
{
    if (slot < 0 || slot >= B3_BOOSTFX_MAX_CARS) return;
    g.car[slot].level = 0.0f;
}

/* FUN_00179F30 verbatim. */
int b3_boostfx_resolve(float level, int red, float* out_size, float out_rgb[3])
{
    if (out_size) *out_size = 0.0f;
    if (out_rgb) out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.0f;
    if (level <= 0.0f) return 0;                    /* the `<= 0.0` early out */
    const float* C = red ? B3BF_COL_RED : B3BF_COL_BLUE;
    float k = red ? B3BF_K_RED : B3BF_K_BLUE;
    float size, rgb[3];
    if (level >= 1.0f) {
        size = k * level;
        rgb[0] = C[0]; rgb[1] = C[1]; rgb[2] = C[2];
    } else {
        size = k;
        rgb[0] = C[0] * level; rgb[1] = C[1] * level; rgb[2] = C[2] * level;
    }
    if (out_size) *out_size = size;
    if (out_rgb) { out_rgb[0] = rgb[0]; out_rgb[1] = rgb[1]; out_rgb[2] = rgb[2]; }
    /* FUN_001871E0's gate */
    return (size > B3BF_SIZE_GATE);
}

/* ======================================================================
 * 6. Draw
 * ====================================================================== */
void b3_boostfx_pass_begin(void)
{
    if (!g.ready) return;
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT
                 | GL_TEXTURE_BIT | GL_CURRENT_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    /* Same reading as the corona pass (docs/RE_CARFX.md 4.3): the literal
     * Xbox blend factors for the sprite pool were not decoded [?], the pool
     * colour is multiplied by 64.0 (DAT_0035BF1C @0x00042B1A) and both
     * coronaboost rasters ship fully opaque with the glow in RGB, which only
     * makes sense saturating into an additive blend.                   GLUE */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glEnable(GL_DEPTH_TEST);      /* flames occluded by world geometry */
    glDepthMask(GL_FALSE);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    g.bound = 0;
    /* re-seed once per pass, the way the retail counter does */
    g.seed_ctr++;
    g.rng_x = (int)(~g.seed_ctr);
    g.rng_c = (int)(g.seed_ctr);
}

void b3_boostfx_draw(int slot, const float pos[3], float yaw_rad, int red)
{
    /* yaw wrapper -- identical to the full-pose path at zero pitch/roll */
    const float s = sinf(-yaw_rad), c = cosf(-yaw_rad);
    const float R[9] = { c, 0, s,  0, 1, 0,  -s, 0, c };
    b3_boostfx_draw_pose(slot, pos, R, red);
}

void b3_boostfx_draw_pose(int slot, const float pos[3], const float rot3[9],
                          int red)
{
    if (!g.ready || slot < 0 || slot >= B3_BOOSTFX_MAX_CARS) return;
    const B3BFCar* c = &g.car[slot];
    if (c->n <= 0) return;
    if (red < 0) red = c->red;      /* the recovered per-car-model flag */

    float size, col[3];
    int   emit = b3_boostfx_resolve(c->level, red, &size, col);
    {   /* B3_BOOSTFX_LOG=1 -> one line per drawing car per frame; the pass
         * counter is the frame number, so it lines up with B3_SHOT_FRAME. */
        static int lg = -1;
        if (lg < 0) lg = getenv("B3_BOOSTFX_LOG") ? 1 : 0;
        if (lg && c->level > 0.0f)
            printf("[boostfx] frame %u slot %d level %.3f size %.3f "
                   "rgb %.3f %.3f %.3f emit %d n %d\n",
                   g.seed_ctr, slot, (double)c->level, (double)size,
                   (double)col[0], (double)col[1], (double)col[2],
                   emit, c->n);
    }
    if (!emit) return;

    GLuint tex = (red && g.tex_red) ? g.tex_red : g.tex_blue;
    if (tex != g.bound) { glBindTexture(GL_TEXTURE_2D, tex); g.bound = tex; }

    /* camera basis + position out of the live modelview -- the harness has
     * already loaded the world view matrix, and taking the axes from it makes
     * the billboard immune to the projection-level x-mirror.  Identical to
     * b3_carfx_corona_draw. */
    float mv[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    const float rt[3] = { mv[0], mv[4], mv[8] };
    const float up[3] = { mv[1], mv[5], mv[9] };
    const float cam[3] = {
        -(mv[0]*mv[12] + mv[1]*mv[13] + mv[2]*mv[14]),
        -(mv[4]*mv[12] + mv[5]*mv[13] + mv[6]*mv[14]),
        -(mv[8]*mv[12] + mv[9]*mv[13] + mv[10]*mv[14]) };

    glBegin(GL_QUADS);
    for (int i = 0; i < c->n; i++) {
        const B3BFEmitter* e = &c->e[i];
        /* FUN_001871E0 blends three part matrices (veh+0x590 + idx*0x40) by
         * the record's own barycentric weights at +0x24/+0x28 with the bone
         * indices at +0x2C/+0x2D, the third slot being the literal identity
         * at 0x003F8110..0x004A1F70, and then transforms by the car's world
         * matrix (carObj+0x10).  An UNDAMAGED car's part matrices are that
         * same identity (FUN_0018D0E0 @0x0018D4xx seeds carObj+0x10 from the
         * same four rows), so with no per-part deformation in this harness
         * the blend collapses to the identity and the whole chain is exactly
         * "transform the record by the car matrix".                   [C/S] */
        /* full object->world pose (row-major rot3, same convention as the
         * carfx corona/body pose): emitters stay ON the tailpipes when the
         * car pitches/rolls on hills (user report, 2026-08-13). */
        const float wp[3] = {
            pos[0] + rot3[0]*e->pos[0] + rot3[1]*e->pos[1] + rot3[2]*e->pos[2],
            pos[1] + rot3[3]*e->pos[0] + rot3[4]*e->pos[1] + rot3[5]*e->pos[2],
            pos[2] + rot3[6]*e->pos[0] + rot3[7]*e->pos[1] + rot3[8]*e->pos[2] };
        const float wn[3] = {
            rot3[0]*e->nrm[0] + rot3[1]*e->nrm[1] + rot3[2]*e->nrm[2],
            rot3[3]*e->nrm[0] + rot3[4]*e->nrm[1] + rot3[5]*e->nrm[2],
            rot3[6]*e->nrm[0] + rot3[7]*e->nrm[1] + rot3[8]*e->nrm[2] };

        for (int k = 0; k < B3BF_NSPRITE; k++) {
            const B3BFSprite* s = &B3BF_SPRITE[k];
            /* position = emitter + normal * dist                        [C] */
            const float sp[3] = { wp[0] + wn[0]*s->dist,
                                  wp[1] + wn[1]*s->dist,
                                  wp[2] + wn[2]*s->dist };
            const float ex = cam[0]-sp[0], ey = cam[1]-sp[1], ez = cam[2]-sp[2];
            const float dist = sqrtf(ex*ex + ey*ey + ez*ez);
            if (dist >= s->far_cut) continue;      /* FUN_00042BC0's far cut */

            const float half = s->size * size;     /* arg3*0.5 @0x00042B64  */
            const float pull = s->pull * size;     /* the record's +0x14    */
            /* corner = pos +- right*half +- up*half - (pos-eye)*pull    [C] */
            const float qx = sp[0] + pull * ex;
            const float qy = sp[1] + pull * ey;
            const float qz = sp[2] + pull * ez;

            /* the colour band, re-rolled per sprite per frame -- the flicker */
            const float u = s->cmin + s->cspan * b3bf_rand01();
            /* The pool packs rgb*64.0 into a D3DCOLOR byte and the combiner
             * scales the result by 4, i.e. 64/255*4 = 1.0039 -- a 0..4 HDR
             * range in a byte, so the effective vertex colour is the raw
             * value.  Same reading (and same choice) as the corona pass. */
            glColor4f(col[0]*u, col[1]*u, col[2]*u, 1.0f);

            /* V-origin: v = 0 is texel row 0, matching the corona pass and
             * the harness convention; the sprite is radially symmetric so
             * the winding/orientation is cosmetic, but keep it identical to
             * FUN_00042BC0's four vertices. */
            glTexCoord2f(0.0f, 0.0f);
            glVertex3f(qx - (rt[0]+up[0])*half, qy - (rt[1]+up[1])*half,
                       qz - (rt[2]+up[2])*half);
            glTexCoord2f(1.0f, 0.0f);
            glVertex3f(qx + (rt[0]-up[0])*half, qy + (rt[1]-up[1])*half,
                       qz + (rt[2]-up[2])*half);
            glTexCoord2f(1.0f, 1.0f);
            glVertex3f(qx + (rt[0]+up[0])*half, qy + (rt[1]+up[1])*half,
                       qz + (rt[2]+up[2])*half);
            glTexCoord2f(0.0f, 1.0f);
            glVertex3f(qx - (rt[0]-up[0])*half, qy - (rt[1]-up[1])*half,
                       qz - (rt[2]-up[2])*half);
        }
    }
    glEnd();
}

void b3_boostfx_pass_end(void)
{
    if (!g.ready) return;
    glPopAttrib();
}

/* ======================================================================
 * 7. Table driver -- what tools/validate_boostfx.py compares the retail
 *    image against.  Built with -DB3_BOOSTFX_TEST_MAIN; touches no GL.
 * ====================================================================== */
#ifdef B3_BOOSTFX_TEST_MAIN
int main(int argc, char** argv)
{
    if (argc >= 2 && !strcmp(argv[1], "consts")) {
        printf("GATE %.9g\n", (double)B3BF_SIZE_GATE);
        printf("K %.9g %.9g\n", (double)B3BF_K_BLUE, (double)B3BF_K_RED);
        printf("COL0 %.9g %.9g %.9g\n", (double)B3BF_COL_BLUE[0],
               (double)B3BF_COL_BLUE[1], (double)B3BF_COL_BLUE[2]);
        printf("COL1 %.9g %.9g %.9g\n", (double)B3BF_COL_RED[0],
               (double)B3BF_COL_RED[1], (double)B3BF_COL_RED[2]);
        printf("LEVEL %.9g %.9g %.9g %.9g\n", (double)B3BF_DECAY_OFF,
               (double)B3BF_DECAY_ON, (double)B3BF_FLOOR, (double)B3BF_FLARE);
        for (int k = 0; k < B3BF_NSPRITE; k++)
            printf("SPRITE %d %.9g %.9g %.9g %.9g %.9g %.9g\n", k,
                   (double)B3BF_SPRITE[k].dist,  (double)B3BF_SPRITE[k].size,
                   (double)B3BF_SPRITE[k].pull,  (double)B3BF_SPRITE[k].cmin,
                   (double)B3BF_SPRITE[k].cspan, (double)B3BF_SPRITE[k].far_cut);
        for (int i = 0; B3BF_RED_CARS[i]; i++)
            printf("REDCAR %s\n", B3BF_RED_CARS[i]);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "resolve")) {
        float sz, rgb[3];
        int e = b3_boostfx_resolve((float)atof(argv[2]), atoi(argv[3]), &sz, rgb);
        printf("%d %.9g %.9g %.9g %.9g\n", e, (double)sz,
               (double)rgb[0], (double)rgb[1], (double)rgb[2]);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "leveltrace")) {
        /* argv[2] = a string of '1' boosting / '0' not / 'x' crashed,
         * argv[3] = dt.  One level per step. */
        float dt = (float)atof(argv[3]);
        b3_boostfx_reset(0);
        for (const char* s = argv[2]; *s; s++) {
            b3_boostfx_update(0, *s == '1', *s == 'x', dt);
            printf("%.9g\n", (double)b3_boostfx_level(0));
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "red")) {
        printf("%d\n", b3_boostfx_car_is_red(argv[2], argv[3]));
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "emitters")) {
        int n = b3_boostfx_load_car(0, argv[2], argv[3]);
        printf("N %d RED %d\n", n, b3_boostfx_car_red(0));
        return 0;
    }
    fprintf(stderr, "usage: %s consts|resolve|leveltrace|red|emitters\n", argv[0]);
    return 2;
}
#endif
