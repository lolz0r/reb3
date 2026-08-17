/*
 * burnout3_hud.c -- the real Burnout 3 in-race HUD.
 *
 * ART (all genuine game assets, build/frontend/):
 *   GlobalFont.png       the game's HUD font, recovered from the XBE .data
 *                        section (font object 0x3c84d8 + embedded 256x256
 *                        DXT5 record 0x3c9c38; tools/extract_font.py).
 *                        White glyphs + alpha; colour is vertex colour at
 *                        draw time, exactly like the game's 2D pipeline.
 *   BoostBits.png        64x256 tyre-tread sheet (Global.txd).  The race
 *                        boost bar samples u 0.75..0.9922, v 0.00195..0.1230
 *                        -- i.e. the 16x31 column at (48,0) -- stretched
 *                        across the whole bar [C, FUN_000496E0].
 *   BoostFireEdge01..41  boost plume strip     [C, FUN_00049FD0]
 *   BoostFireCore01..30  boost core strip      [C, FUN_00049AD0]
 *   BoostFireOver01..20  boost overlay strip   [C, FUN_0004A470]
 *   BoostEarnFlame.png   the EARN indicator    [C, FUN_00049E40]
 *   hud_element01.png    THE plate: POS / LAP / speed corners and
 *                        the EA TRAX banner, drawn as a three-slice
 *                        stretch  [C, FUN_00048430]
 *
 * WHAT IS BINARY-DERIVED IN HERE  (docs/RE_FRONTEND.md 6.6)
 * ---------------------------------------------------------
 * The whole boost bar.  Chain of custody:
 *
 *   FUN_0004DD00        binds boostbits / boostfireXX / boostearnflame
 *   FUN_00052CF0/2FC0   in-race HUD builder: `mov ebx,0x3FD550; mov ecx,3;
 *                       call FUN_0004BFC0`  -> the boost bar element object
 *                       is the static at 0x003FD550, anchor slot 3
 *   FUN_0004E100        base ctor; FUN_00053BE0 resolves anchor slot ->
 *                       screen point through the table at 0x003FD410
 *                       (slot 3 = {0.0,1.0} = viewport BOTTOM-LEFT)
 *   FUN_00048800        fills the draw node: box = 360 x 28 px
 *                       (0x3FCAA0/0x3FCAA4), placed at
 *                       ref - anchor*size with anchor {0,1}
 *                       => box (0,452)..(360,480) in 640x480   [C]
 *                       and installs 0x0004AE40 as the draw callback
 *   FUN_0004C390        per-frame state machine (fill chase, earn flash,
 *                       flame level, sparks, bar-size animation)   [C]
 *   FUN_0004AE40        draw: plate -> sparks -> earn flame ->
 *                       tread band -> core -> edge -> over          [C]
 *
 * Everything the boost bar does below cites its address in
 * burnout3_hud.h's machine-checked table; tools/validate_hud.py re-reads
 * every one of them out of build/burnout3.elf and fails on a mismatch.
 *
 * ...and, since 2026-08-12 (docs/RE_FRONTEND.md 6.10), the POS / LAP /
 * SPEED plates and the OPPONENT TAGS:
 *
 *   FUN_00048430        the plates' draw callback -- NOT the generic
 *                       sprite builder 6.6.7 took it for, but a fixed
 *                       three-slice stretch of `hud_element01` (handle
 *                       0x0046093C @0x00048588) with the two cap
 *                       fractions in the draw node at +0x20 / +0x24  [C]
 *   FUN_00053ED0        POS   element 0x003FD4A0, slot 1 (top-left)  [C]
 *   FUN_00051650        LAP   element 0x003FD4F8, slot 7 -- the MIRROR,
 *                       done by swapping the caps, not by flipping u [C]
 *   FUN_00059850        SPEED element 0x003FDD48, slot 5, both caps  [C]
 *   FUN_0018F060        one opponent tag: project, cull, size, then
 *                       either the Globalus ordinal or a flat
 *                       three-vertex triangle, switched at 35 units [C]
 *
 * WHAT IS STILL CALIBRATED
 * ------------------------
 * The ANCHOR VIEWPORT (B3HUD_SAFE_FRAC in the header -- a 4.5%-per-side
 * title-safe inset, the one number the recovered anchor rule needs and
 * the image does not hold), the inner TEXT PLACEMENT inside the three
 * plates, the speedo's extra digit tracking, and every HUD text COLOUR
 * (the 2D colour float4s at 0x0054FA00.. have no writer in the image;
 * they are solved off the retail frame, not guessed -- 6.10.4).  All of
 * those are marked [S-ref] where they appear.
 *
 * The drawing code itself is original harness code, NOT decompiled.
 */
#include "burnout3_hud.h"
#include "burnout3_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL_image.h>

/* ---- art ------------------------------------------------------------- */

static GLuint g_font_global;
static GLuint g_panel;                       /* hud_element01: THE plate */
static GLuint g_tread;                       /* BoostBits                */
static GLuint g_earnflame;                   /* BoostEarnFlame           */
static GLuint g_stars;                       /* hud_boost_stars          */
static GLuint g_eatrax;                      /* EATrax (Global.txd)      */
static GLuint g_abutton;                     /* A_Button: the (A) glyph  */
static GLuint g_aftertouch;                  /* Aftertouch: the arrow    */
static GLuint g_edge[B3HUD_EDGE_FRAMES];
static GLuint g_core[B3HUD_CORE_FRAMES];
static GLuint g_over[B3HUD_OVER_FRAMES];
static int    g_edge_n, g_core_n, g_over_n;
static int    g_ready;

/* ---- the boost bar's recovered per-frame state (FUN_0004C390) -------- *
 * field names carry the game's offset inside the element object 0x3FD550 */
typedef struct {
    float clock;        /* +0x520  animation clock, seconds              */
    float bar_frac;     /* +0x524  A = (tier+1) * 0.25                   */
    float fill;         /* +0x528  B, fraction of the FULL 4-tier bar    */
    float earn_flash;   /* +0x52C  0..1, drives BoostEarnFlame           */
    float flame;        /* +0x530  0..1, drives the plume intensity      */
    float tier_anim;    /* +0x534  bar-size change timer, starts at -0.5 */
    int   segments;     /* +0x538  drawn bar segments                    */
    int   mode;         /* +0x53A  0 idle, 1 shrank, 2 grew              */
    float prev_earned;  /* +0x53C  last frame's lifetime-earned          */
    int   prev_seg;     /* +0x56B  last frame's (tier+1)                 */
    float spark_acc;    /* +0x540  spark spawn accumulator               */
    float shake;        /* local_18, applied to the node x               */
    int   started;
} B3BoostHud;

static B3BoostHud g_boost;


/* ---- the EVENT TICKER's per-row state -------------------------------- *
 * One entry per row slot of the SAME element object (obj+0x570 + i*0x28);
 * the field comments carry the slot-relative offsets FUN_0004D130 and
 * FUN_0004D310 use.  `live` is the game's row+0x00 (its draw-node
 * pointer): non-zero exactly while the row is on screen.            [C] */
typedef struct {
    int   live;         /* +0x00  owns a draw node                       */
    float timer;        /* +0x08  lifetime, refreshed to 0.85 while live */
    float y;            /* +0x10  current y, element-local (up = -ve)    */
    int   tier;         /* +0x18  filled stars, -1 = none                */
    float flash;        /* +0x1C  newest star's zoom, 2.0 -> 1.0         */
    float phase;        /* +0x20  pending star's spin, radians           */
    float pulse;        /* +0x24  pending star's alpha, 0..1             */
    /* what FUN_0004D310 writes into the row's draw node each frame */
    float node_x, node_y, node_w, node_h, node_a;
} B3TickRow;

static B3TickRow g_tick[B3_HUD_TICK_ROWS];
static int       g_tick_order[B3_HUD_TICK_ROWS];  /* obj+0x688 list, head first */
static int       g_tick_n;

/* Row table.  `label` is the Data/Globalus.bin entry the constructor
 * FUN_0004BFC0 loads into row+0x04 (byte offset / 4); `thresh` is the
 * float FUN_0004D310 pushes at that row's FUN_0004D130 call site;
 * `probed` marks the five... six slots retail actually probes.      [C] */
static const struct {
    const char *label;
    int         str;      /* Globalus.bin entry index                    */
    float       thresh;   /* [ebp+8] at the probe site                   */
    int         probed;   /* 0 = FUN_0004D310 never probes this slot     */
    int         rec;      /* score-object offset of its B3CatRecord      */
} B3_TICK[B3_HUD_TICK_ROWS] = {
    { "ONCOMING",   B3HUD_TICK_STR_ONCOMING / 4, B3HUD_TICK_ONC_MIN, 1, 0x374 },
    { "DRIFT",      B3HUD_TICK_STR_DRIFT    / 4, 0.0f, 1, 0x390 },
    { "NEAR MISS",  B3HUD_TICK_STR_NEARMISS / 4, 0.0f, 1, 0x418 },
    { "AIR",        B3HUD_TICK_STR_AIR      / 4, 0.0f, 0, 0x358 },
    { "TAILGATING", B3HUD_TICK_STR_TAILGATE / 4, 1.0f, 1, 0x598 },
    { "GRINDING",   B3HUD_TICK_STR_GRINDING / 4, 1.0f, 1, 0x5C4 },
    { "RUBBING",    B3HUD_TICK_STR_RUBBING  / 4, 1.0f, 1, 0x564 }
};

/* FUN_0004D310's probe order -- it is also the row-list push order, so
 * it decides which row lands nearest the boost bar when several open on
 * the same frame (the list is push-front: last probed = bottom).    [C] */
static const int B3_TICK_PROBE[6] = {0, 1, 2, 4, 5, 6};

/* ---- earn / generic callout ------------------------------------------ */

/* The game's own strings, table 0x003C8390, order [cat][tier]:
 * cat  0 AIR, 1 DRIFT, 2 ONCOMING, 3 NEAR MISS
 * tier 0 GOOD, 1 GREAT, 2 FANTASTIC, 3 AWESOME                     [C] */
static const char *const B3_CALLOUT[B3HUD_CALLOUT_CATS][B3HUD_CALLOUT_TIERS] = {
    { "GOOD AIR!",       "GREAT AIR!",       "FANTASTIC AIR!",       "AWESOME AIR!"       },
    { "GOOD DRIFT!",     "GREAT DRIFT!",     "FANTASTIC DRIFT!",     "AWESOME DRIFT!"     },
    { "GOOD ONCOMING!",  "GREAT ONCOMING!",  "FANTASTIC ONCOMING!",  "AWESOME ONCOMING!"  },
    { "GOOD NEAR MISS!", "GREAT NEAR MISS!", "FANTASTIC NEAR MISS!", "AWESOME NEAR MISS!" }
};

/* Internally-fired earn callout (boost events).  The caller's own
 * callout (takedown FX) is passed through B3HudState and wins if both
 * are live -- the takedown module owns the loud channel. */
static B3HudCallout g_earn_callout;

/* position-change transient (kept from the previous revision, [S-ref]) */
static int   g_last_pos = -1;
static float g_pos_callout_t = 99.f;

/* ---- texture loading -------------------------------------------------- */

GLuint b3_hud_load_texture(const char *path) {
    SDL_Surface *img = IMG_Load(path);
    if (!img) return 0;
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(img);
    if (!rgba) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
    SDL_FreeSurface(rgba);
    return tex;
}

static GLuint load_from(const char *dir, const char *name, int *count) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    GLuint t = b3_hud_load_texture(path);
    if (t && count) (*count)++;
    return t;
}

static int load_strip(const char *dir, const char *stem, GLuint *out, int n,
                      int *count) {
    int got = 0;
    for (int i = 0; i < n; i++) {
        char name[48];
        snprintf(name, sizeof(name), "%s%02d", stem, i + 1);
        out[i] = load_from(dir, name, count);
        if (out[i]) got++;
    }
    return got;
}

int b3_hud_init(const char *dir) {
    int loaded = 0;
    g_font_global = load_from(dir, "GlobalFont", &loaded);
    g_panel       = load_from(dir, "hud_element01", &loaded);
    g_tread       = load_from(dir, "BoostBits", &loaded);
    g_earnflame   = load_from(dir, "BoostEarnFlame", &loaded);
    g_stars       = load_from(dir, "hud_boost_stars", &loaded);
    g_eatrax      = load_from(dir, "EATrax", &loaded);
    /* FUN_000511C0 @0x000511EC loads it by the literal name at 0x003AB2E0 */
    g_abutton     = load_from(dir, "A_Button", &loaded);
    /* FUN_0004DD00's strcmp loop caches this one into 0x004607C0, which is
     * the only thing FUN_0004FCA0 (the arrow cursor) binds. */
    g_aftertouch  = load_from(dir, "Aftertouch", &loaded);
    g_edge_n = load_strip(dir, "BoostFireEdge", g_edge, B3HUD_EDGE_FRAMES, &loaded);
    g_core_n = load_strip(dir, "BoostFireCore", g_core, B3HUD_CORE_FRAMES, &loaded);
    g_over_n = load_strip(dir, "BoostFireOver", g_over, B3HUD_OVER_FRAMES, &loaded);
    g_ready = loaded > 0;
    memset(&g_boost, 0, sizeof(g_boost));
    memset(g_tick, 0, sizeof(g_tick));
    g_tick_n = 0;
    for (int i = 0; i < B3_HUD_TICK_ROWS; i++) {
        g_tick[i].tier  = -1;
        g_tick[i].flash = B3HUD_TICK_FLASH_FLOOR;
    }
    g_boost.tier_anim = 0.f;
    g_boost.prev_seg  = 0;
    b3_hud_crash_reset();
    if (!g_font_global)
        printf("[Burnout3 HUD] WARNING: GlobalFont.png missing -- run "
               "tools/extract_font.py\n");
    printf("[Burnout3 HUD] %d textures from %s (edge %d/%d, core %d/%d, "
           "over %d/%d)\n", loaded, dir, g_edge_n, B3HUD_EDGE_FRAMES,
           g_core_n, B3HUD_CORE_FRAMES, g_over_n, B3HUD_OVER_FRAMES);
    return loaded;
}

/* ---- primitive drawing ------------------------------------------------ *
 * All internal drawing is in the game's 640x480 virtual space, y down.    */

static void vtx(float x, float y) {
    glVertex2f(x * (2.f / B3HUD_VIRT_W) - 1.f,
               1.f - y * (2.f / B3HUD_VIRT_H));
}

/* The AMBIENT 2D state FUN_001C72F0 establishes for the whole HUD pass:
 * SRC_ALPHA / ONE_MINUS_SRC_ALPHA, BLENDOP ADD, texture address WRAP,
 * and COLORWRITEENABLE = 0x010101 -- RGB only, the alpha channel is
 * never written.  [C, RE_FRONTEND 6.7.1/6.7.7]  The old code left the
 * alpha mask on, so every translucent HUD quad also stamped its own
 * alpha into the framebuffer; on a target whose alpha is read back
 * (post-fx, or any composite) that is the "HUD doesn't alpha-blend
 * right" symptom.  Blend equation and mask are now set explicitly. */
static void state_begin(void) {
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);   /* 0x010101 [C] */
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
}

static void state_end(void) {
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPopAttrib();
}

/* ===================================================================== *
 *  THE ANCHOR RULE -- FUN_00053BE0, applied to the title-safe viewport
 *  (burnout3_hud.h, B3HUD_SAFE_FRAC).  Every in-race element resolves
 *  its screen point this way; only the viewport rect is [S-ref].    [C] */
#define B3HUD_ANCHOR_X(fx) (B3HUD_VP_X0 + (fx) * B3HUD_VIRT_W * B3HUD_VP_SX)
#define B3HUD_ANCHOR_Y(fy) (B3HUD_VP_Y0 + (fy) * B3HUD_VIRT_H * B3HUD_VP_SY)

static void tex_quad_px(GLuint tex, float x, float y, float w, float h,
                        float u0, float v0, float u1, float v1,
                        float alpha) {
    if (!tex) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor4f(1.f, 1.f, 1.f, alpha);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); vtx(x, y);
    glTexCoord2f(u1, v0); vtx(x + w, y);
    glTexCoord2f(u1, v1); vtx(x + w, y + h);
    glTexCoord2f(u0, v1); vtx(x, y + h);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

/* ---- text ------------------------------------------------------------- */

typedef struct B3TextStyle {
    float scale;
    float shear;                /* italic lean [S-ref] */
    float top[4], bot[4];
    float outline[4];
    float shadow_a;             /* the style block's A offset [C]     */
} B3TextStyle;

static float text_width(const B3Font *f, const char *s, float scale) {
    float w = 0.f;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E) continue;
        const B3Glyph *g = &f->glyph[c - 0x20];
        w += (g->present ? g->advance : 7.f) * scale;
    }
    return w;
}

static void text_pass(const B3Font *f, GLuint tex, const char *s,
                      float x, float y, const B3TextStyle *st,
                      const float top[4], const float bot[4]) {
    float base = y + f->line_h * st->scale;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBegin(GL_QUADS);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E) continue;
        const B3Glyph *g = &f->glyph[c - 0x20];
        if (!g->present) { x += 7.f * st->scale; continue; }
        if (g->w > 0.f) {
            float x0 = x + g->xoff * st->scale;
            float y0 = y + g->yoff * st->scale;
            float x1 = x0 + g->w * st->scale;
            float y1 = y0 + g->h * st->scale;
            float s0 = (base - y0) * st->shear;
            float s1 = (base - y1) * st->shear;
            glColor4f(top[0], top[1], top[2], top[3]);
            glTexCoord2f(g->u0, g->v0); vtx(x0 + s0, y0);
            glTexCoord2f(g->u1, g->v0); vtx(x1 + s0, y0);
            glColor4f(bot[0], bot[1], bot[2], bot[3]);
            glTexCoord2f(g->u1, g->v1); vtx(x1 + s1, y1);
            glTexCoord2f(g->u0, g->v1); vtx(x0 + s1, y1);
        }
        x += g->advance * st->scale;
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

/* The game's own text shadow, recovered from FUN_0004B4D0 (RE_FRONTEND
 * 6.8.5) and generalised with the other two style blocks FUN_0004DA90
 * fills: EIGHT passes -- four at the combinations of A+-B down-right and
 * four at +-B diagonally -- all in BLACK at 0.25*alpha^2, so the eight
 * quarter-alpha copies build a solid black edge that is heavier on the
 * down-right side.  A is 2.2 / 3.0 / 4.0 depending on the style block
 * (0x0054F520 / 0x0054F540 / 0x0054F560), B is 1.2 in all three, and
 * the colour {0,0,0,0.25} is all three blocks' `movaps` source.     [C]
 *
 * The old code used eight unit-offset copies of an opaque dark-navy at
 * full alpha, which is why the numerals read as outlined-in-blue rather
 * than the retail black drop shadow. */
static void draw_text(const B3Font *f, GLuint tex, const char *s,
                      float x, float y, const B3TextStyle *st) {
    if (!tex) return;
    if (st->outline[3] > 0.f) {
        const float A = st->shadow_a > 0.f ? st->shadow_a : B3HUD_TICK_SHADOW_A;
        const float Bo = B3HUD_TICK_SHADOW_B;
        const float off[8][2] = {
            { A + Bo, A + Bo }, { A - Bo, A - Bo },
            { A - Bo, A + Bo }, { A + Bo, A - Bo },
            { Bo, Bo }, { -Bo, -Bo }, { -Bo, Bo }, { Bo, -Bo }
        };
        for (int i = 0; i < 8; i++)
            text_pass(f, tex, s, x + off[i][0], y + off[i][1],
                      st, st->outline, st->outline);
    }
    text_pass(f, tex, s, x, y, st, st->top, st->bot);
}

/* Styles: GLUE.  Gold gradient digits, dark navy outline, italic lean --
 * eyeballed from a reference frame, NOT recovered.
 *
 * What IS recovered about the text/sign channel (2026-08-11):
 *   * an E8-rel32 scan of the whole 2D HUD module 0x000461C0..0x0005E360
 *     finds exactly 2 texture-address preset switches and 9 blend preset
 *     switches, and NONE of them is in the callout-sign path (the sign
 *     table 0x004608F0 is read at 0x00055D01, inside a function with no
 *     preset call).  So the TAKEDOWN! sign and every text run draw under
 *     the 2D pass's ambient state that FUN_001C72F0 establishes:
 *     SRC_ALPHA / ONE_MINUS_SRC_ALPHA, BLENDOP ADD, COLORWRITEENABLE
 *     0x010101, texture address WRAP/WRAP.                          [C]
 *   * the sign element's vertex colour is the shared 2D default node
 *     colour float4 at 0x0054FA00 (movaps xmm2,[0x54FA00] ;
 *     movaps [node+0x10],xmm2 @0x00051E09), read by 20 element
 *     constructors.  It has no static initialiser of either decodable
 *     shape (single-float thunk or movaps float4 thunk -- a scan of
 *     0x00260000..0x00280000 finds zero of the latter), so its runtime
 *     RGBA is                                                       [?]
 * Until 0x0054FA00's writer is found, these stay GLUE and are not in the
 * machine-checked table.                                                */
/* The shadow colour every style uses: {0,0,0,0.25}, the float4 all three
 * style blocks 0x0054F510 / 0x0054F530 / 0x0054F550 are filled with by
 * FUN_0004DA90 (@0x0004DBE2/E9/F0).                                  [C] */
#define B3_SHADOW_RGBA  {0.f, 0.f, 0.f, B3HUD_TICK_SHADOW_LEVEL}

/* The HUD GOLD, sampled off the retail frame (the numerals are a
 * vertical gradient; POS and LAP read identically at identical screen y,
 * so it is the text run's own gradient, not a per-glyph one):
 *     top #F7DB24 (247,219,36)   bottom #F4AF14 (244,175,20)     [S-ref]
 * and the deeper unit-label orange under "mph": #F3AA12 -> #F18F07.  */
static const B3TextStyle STYLE_GOLD_BIG = {
    1.27f, 0.20f,
    {0.969f, 0.859f, 0.141f, 1.f}, {0.957f, 0.686f, 0.078f, 1.f},
    B3_SHADOW_RGBA, B3HUD_SHADOW_A_BIG
};
static const B3TextStyle STYLE_GOLD_POS = {
    1.414f, 0.20f,
    {0.969f, 0.859f, 0.141f, 1.f}, {0.957f, 0.686f, 0.078f, 1.f},
    B3_SHADOW_RGBA, B3HUD_SHADOW_A_BIG
};
/* the "/N" denominator and the "mph" unit: same pen, smaller run */
static const B3TextStyle STYLE_GOLD_FRAC = {
    0.68f, 0.20f,
    {0.969f, 0.859f, 0.141f, 1.f}, {0.957f, 0.686f, 0.078f, 1.f},
    B3_SHADOW_RGBA, B3HUD_TICK_SHADOW_A
};
static const B3TextStyle STYLE_GOLD_LABEL = {
    0.66f, 0.20f,
    {0.953f, 0.667f, 0.071f, 1.f}, {0.945f, 0.561f, 0.027f, 1.f},
    B3_SHADOW_RGBA, B3HUD_SHADOW_A_MED
};
/* "POS" / "LAP": a flat light blue, #91BBFF, no gradient.        [S-ref] */
static const B3TextStyle STYLE_WHITE_LABEL = {
    0.68f, 0.20f,
    {0.569f, 0.733f, 1.f, 1.f}, {0.569f, 0.733f, 1.f, 1.f},
    B3_SHADOW_RGBA, B3HUD_TICK_SHADOW_A
};
/* the opponent tag's ordinal: the pale yellow the ticker labels use --
 * both measure #F9F883 in the retail frame, which is the runtime value
 * of the [?] 2D text colour 0x0054FA10 those two share.          [S-ref] */
static const B3TextStyle STYLE_TAG = {
    1.0f, 0.20f,
    {0.976f, 0.973f, 0.514f, 1.f}, {0.976f, 0.973f, 0.514f, 1.f},
    B3_SHADOW_RGBA, B3HUD_SHADOW_A_MED
};

/* Globalus.bin entries 146..148 [C]. */
static const char *ordinal(int p) {
    static const char *tab[] = {"1st", "2nd", "3rd", "4th", "5th", "6th"};
    if (p < 1) p = 1;
    if (p > 6) p = 6;
    return tab[p - 1];
}

/* ===================================================================== *
 *  BOOST BAR -- binary-derived
 * ===================================================================== */

/* The bar box, resolved exactly the way the game resolves it:
 *   anchor point = slot table[3] * screen  (viewport = full screen here)
 *   box.x = anchor.x - ANCHOR_X * W ; box.y = anchor.y - ANCHOR_Y * H  */
#define BOOST_X (B3HUD_VP_X0 + B3HUD_VP_SX * (B3HUD_BOOST_SLOT_FX * B3HUD_VIRT_W) \
                 - B3HUD_BOOST_ANCHOR_X * B3HUD_BOOST_W)
#define BOOST_Y (B3HUD_VP_Y0 + B3HUD_VP_SY * (B3HUD_BOOST_SLOT_FY * B3HUD_VIRT_H) \
                 - B3HUD_BOOST_ANCHOR_Y * B3HUD_BOOST_H)

/* FUN_000496E0's per-segment silhouette table, DAT_003AB038, 10 rows of
 * 6 floats {rail0, rail1, rail2, mix0, mix1, mix2}.  Rails are fractions
 * of the box height; mixes weight the amber->white colour lerp.     [C] */
static const float B3_TREAD_PROFILE[10][6] = {
    {0.30f, 0.63f, 0.95f, 0.20f, 0.30f, 0.20f},
    {0.25f, 0.62f, 1.00f, 0.20f, 1.00f, 0.00f},
    {0.32f, 0.70f, 0.97f, 0.00f, 0.20f, 0.90f},
    {0.28f, 0.60f, 0.99f, 0.30f, 0.00f, 0.40f},
    {0.15f, 0.50f, 0.95f, 1.00f, 0.00f, 0.10f},
    {0.08f, 0.60f, 0.94f, 0.20f, 0.80f, 0.00f},
    {0.02f, 0.48f, 1.00f, 0.00f, 0.40f, 0.20f},
    {0.05f, 0.57f, 0.97f, 0.70f, 0.00f, 0.00f},
    {0.04f, 0.45f, 0.98f, 1.00f, 0.00f, 0.80f},
    {0.00f, 0.50f, 1.00f, 0.10f, 0.30f, 1.00f}
};

/* ---- the per-section vertex colours -------------------------------- *
 * Every 2D batch takes its vertex colour from a float4 the section hands
 * FUN_001C7430 / FUN_001C7710 in ECX (one colour for the batch) or as a
 * per-vertex array to FUN_001C7960; FUN_001C6920 packs it to ARGB8888 as
 * (int)(c*255).  Each section's float4 is a constant in the block below
 * multiplied by the draw node's own rgba (node+0x10 = {1,1,1,1}, written
 * by FUN_00048800).  All six were read off the binary AND confirmed by
 * capturing the emitted vertex pool under Unicorn
 * (tools/emulate_hud.py).                                          [C]  */
static const float B3_PLATE_COL[4] = {1.0f, 1.0f, 1.0f, 1.0f};  /* 0x3FCAB0 */
static const float B3_TREAD_C0[4]  = {1.0f, 0.8f, 0.1f, 0.8f};  /* 0x3FCAC0 */
static const float B3_TREAD_C1[4]  = {1.0f, 0.9f, 0.8f, 0.0f};  /* 0x3FCAD0 */
static const float B3_EARN_COL[4]  = {1.0f, 1.0f, 1.0f, 0.5f};  /* 0x3FCAE0 */
static const float B3_CORE_COL[4]  = {1.0f, 1.0f, 0.0f, 0.67f}; /* 0x3FCAF0 */
static const float B3_OVER_COL[4]  = {1.0f, 1.0f, 0.0f, 0.8f};  /* 0x3FCB00 */
/* the in-race spark ramp FUN_0004A740 walks, table 0x003FCB10 [C];
 * the particle RECORDS are still [?], so no sparks are drawn yet -- the
 * ramp is kept (and validated) so whoever closes that gap has it. */
static const float B3_SPARK_RAMP[3][4] = {
    {1.0f, 0.8f,  0.4f,  0.0f},                                 /* 0x3FCB10 */
    {1.0f, 0.15f, 0.05f, 0.4f},                                 /* 0x3FCB20 */
    {1.0f, 0.1f,  0.0f,  0.0f}                                  /* 0x3FCB30 */
};

/* ---- the two render-state presets FUN_0004AE40 switches between ----- *
 * The 2D pass enters with blend preset 0 (alpha) and address preset 1
 * (WRAP) -- FUN_001C72F0 sets exactly that.  FUN_0004AE40 then keeps
 * WRAP + alpha for the PLATE, and switches to CLAMP + ADDITIVE for the
 * sparks / earn / tread / core / edge / over, restoring both at the end.
 * COLORWRITEENABLE is 0x010101 (RGB, no alpha write) in both.       [C] */
static void state_plate(void) {              /* FUN_001C8470(1) + preset 0 */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

static void state_fire(void) {               /* FUN_001C8470(0) + preset 1 */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void state_restore(void) {            /* back to the ambient 2D state */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_TEXTURE_2D);
}

/* A textured axis-aligned rect with one colour, the shape FUN_001C7430
 * emits (2 xy pairs + 2 uv pairs -> 4 verts). */
static void rect_c(GLuint tex, const float c[4],
                   float x0, float y0, float x1, float y1,
                   float u0, float v0, float u1, float v1) {
    if (!tex) return;
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor4f(c[0], c[1], c[2], c[3]);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); vtx(x0, y0);
    glTexCoord2f(u1, v0); vtx(x1, y0);
    glTexCoord2f(u1, v1); vtx(x1, y1);
    glTexCoord2f(u0, v1); vtx(x0, y1);
    glEnd();
}

/* frame = (int)(rate * clock) % count  [C: FUN_00049FD0 / AD0 / A470] */
static int strip_frame(float rate, float clock, int count) {
    int f = (int)(rate * clock) % count;
    if (f < 0) f += count;
    return f;
}

/* FUN_0004C390: the whole per-frame boost HUD state machine. */
static void boost_update(B3BoostHud *b, const B3HudBoostIn *in, float dt) {
    b->clock += dt;

    int seg = in->tier + 1;
    if (seg < 1) seg = 1;
    if (seg > B3HUD_CALLOUT_TIERS) seg = B3HUD_CALLOUT_TIERS;
    if (!b->started) { b->prev_seg = seg; b->started = 1; }

    /* A = (tier+1) * 0.25   [0x0004C49E] */
    b->bar_frac = (float)seg * B3HUD_TIER_STEP;

    /* bar-size change: timer restarts at -0.5, mode 1 shrank / 2 grew */
    if (seg != b->prev_seg) {
        b->tier_anim = -0.5f;
        b->mode = (seg < b->prev_seg) ? 1 : 2;
    }
    b->prev_seg = seg;

    if (b->mode) {
        float prev = b->tier_anim;
        b->tier_anim += dt;
        /* the shake the game applies to the element's node x, for
         * 0.3 < t < 0.7:  (50 - (t-0.3)*125) * cos(2*pi*10*(t-0.3)) */
        b->shake = 0.f;
        if (b->mode == 2 && b->tier_anim > B3HUD_TIERANIM_GROW_SFX_T &&
            b->tier_anim < B3HUD_TIERANIM_SHAKE_END) {
            float u = b->tier_anim - B3HUD_TIERANIM_GROW_SFX_T;
            b->shake = (B3HUD_TIERANIM_SHAKE_A - u * B3HUD_TIERANIM_SHAKE_B) *
                       cosf(u * 62.831856f);
        }
        if (b->tier_anim > 2.2f) { b->mode = 0; b->shake = 0.f; }
        (void)prev;
    } else {
        b->shake = 0.f;
    }

    int earning = 0;

    /* fill chase: target = meter/size * A.  Instant down, 0.625/s up.   */
    if (in->bar_size > 0.f &&
        (in->min_units <= in->meter || in->boosting)) {
        float target = in->meter * (1.f / in->bar_size) * b->bar_frac;
        if (target <= b->fill) {
            b->fill = target;
        } else if (b->mode != 2 || b->tier_anim > B3HUD_TIERANIM_GROW_GATE) {
            earning = (target - b->fill) > B3HUD_EARN_DELTA_THRESH;
            b->fill += B3HUD_FILL_RISE_RATE * dt;
            if (b->fill > target) b->fill = target;
        }
    } else {
        b->fill = 0.f;
    }

    /* flame level: +5/s while boosting or earning, -2/s otherwise      */
    if (in->boosting || earning) {
        b->flame += B3HUD_FLAME_RISE_RATE * dt;
        if (b->flame > 1.f) b->flame = 1.f;
        b->spark_acc += B3HUD_SPARK_RATE * dt;
        while (b->spark_acc >= 1.f) b->spark_acc -= 1.f;
    } else {
        b->flame -= B3HUD_FLAME_FALL_RATE * dt;
        if (b->flame < 0.f) b->flame = 0.f;
    }

    /* earn flash: +3/s on the frame lifetime-earned went up, -0.75/s   */
    if (in->earned > b->prev_earned) {
        b->earn_flash += B3HUD_EARNFLASH_RISE_RATE * dt;
        if (b->earn_flash > 1.f) b->earn_flash = 1.f;
    } else {
        b->earn_flash -= B3HUD_EARNFLASH_FALL_RATE * dt;
        if (b->earn_flash < 0.f) b->earn_flash = 0.f;
    }
    b->prev_earned = in->earned;

    b->segments = seg;
}

/* The bar PLATE -- FUN_000488A0.  This is the backdrop that makes the bar
 * readable over any road surface, and it was the module's one piece of
 * pure glue (a hand-picked dark translucent quad).  It is now the game's:
 *
 *   texture   BoostBits, bound by FUN_0004AE40 @0x0004AF0B before the call
 *   state     alpha blend (SRC_ALPHA, INV_SRC_ALPHA) + WRAP/WRAP
 *   colour    0x003FCAB0 {1,1,1,1} * node.rgba  ->  OPAQUE WHITE
 *   body rect x 0 .. (A - 1/6)*w      u 0 .. (A - 1/6)*6   (tiles!)
 *             v 0.130859375 .. 0.244140625  = BoostBits y 33.5..62.5
 *   cap rect  x (A - 1/6)*w .. A*w    u 0 .. 1
 *             v seg*0.25 - 0.244140625, + 0.11328125
 *             = BoostBits y 65.5/129.5/193.5 for tier 2/3/4
 *   when seg <= 1 there is no cap: one rect x 0..A*w, u 0..A*6, body v
 *
 * Confirmed by capturing the emitted vertex pool under Unicorn for
 * A = 0.25/0.5/0.75/1.0 (tools/emulate_hud.py).                     [C] */
static void boost_plate(float x, float y, float w, float h, float A,
                        int segments) {
    if (!g_tread || A <= 0.f) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tread);
    state_plate();

    const float body_v0 = B3HUD_PLATE_V1 - B3HUD_PLATE_VH;
    const float body_v1 = B3HUD_PLATE_V1;

    if (segments <= 1) {
        rect_c(g_tread, B3_PLATE_COL, x, y, x + w * A, y + h,
               0.f, body_v0, A * B3HUD_PLATE_U_SCALE, body_v1);
    } else {
        float split = A - B3HUD_PLATE_CAP_FRAC;
        rect_c(g_tread, B3_PLATE_COL, x, y, x + w * split, y + h,
               0.f, body_v0, split * B3HUD_PLATE_U_SCALE, body_v1);
        float cv0 = (float)segments * B3HUD_TIER_STEP - B3HUD_PLATE_V1;
        rect_c(g_tread, B3_PLATE_COL, x + w * split, y, x + w * A, y + h,
               0.f, cv0, 1.f, cv0 + B3HUD_PLATE_VH);
    }
    /* leave the texture as the fire sections expect it */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glDisable(GL_TEXTURE_2D);
}

/* FUN_000496E0: the BoostBits tread band, 9 segments wide at full fill.
 *   seg width = (1 + 2*OVERLAP) * SEG_FRAC * w
 *   x0        = -OVERLAP * w
 *   rails     = PROFILE[i][0..2] * HSCALE * h  (+ YBASE * h)
 *   colour    = lerp(C0, C1, PROFILE[i][3+k]*0.6 + tri(i*0.25 + t*1.8)*0.4)
 *   u         = 0.75 + (i / (9*B)) * 0.2421875
 *   v rails   = 0.001953125 / 0.0625 / 0.123046875                  [C] */
static void boost_tread(float bx, float by, float w, float h, float B,
                        float clock) {
    if (!g_tread || B <= 0.0005f) return;
    const float segw = (1.f + 2.f * B3HUD_TREAD_OVERLAP) *
                       B3HUD_TREAD_SEG_FRAC * w;
    const float y0 = B3HUD_TREAD_YBASE * h;
    const float ys = B3HUD_TREAD_HSCALE * h;
    const float vr[3] = { B3HUD_TREAD_V0, B3HUD_TREAD_V1, B3HUD_TREAD_V2 };

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tread);
    state_fire();

    float x = -B3HUD_TREAD_OVERLAP * w;
    for (int i = 0; i < B3HUD_TREAD_SEGMENTS; i++) {
        float t0 = (float)i * (B3HUD_TREAD_SEG_FRAC / B);
        float t1 = (float)(i + 1) * (B3HUD_TREAD_SEG_FRAC / B);
        if (t0 >= 0.999f) break;

        /* animated triangle wave, amplitude 0.4 [0x0004986E] */
        float ph = (float)i * B3HUD_TREAD_WAVE_PHASE +
                   clock * B3HUD_TREAD_WAVE_SPEED;
        float f = (ph - floorf(ph)) * 2.f;
        if (f > 1.f) f = 2.f - f;
        f *= B3HUD_TREAD_WAVE_AMP;

        const float *p0 = B3_TREAD_PROFILE[i];
        const float *p1 = B3_TREAD_PROFILE[(i + 1) % 10];
        float u0 = B3HUD_TREAD_U0 + t0 * B3HUD_TREAD_DU;
        float u1 = B3HUD_TREAD_U0 + (t1 < 1.f ? t1 : 1.f) * B3HUD_TREAD_DU;

        for (int k = 0; k < 2; k++) {          /* two stacked strips */
            float ay0 = y0 + p0[k]     * ys, ay1 = y0 + p0[k + 1] * ys;
            float by0 = y0 + p1[k]     * ys, by1 = y0 + p1[k + 1] * ys;
            float wa0 = p0[3 + k]     * B3HUD_TREAD_PROFILE_MIX + f;
            float wa1 = p0[3 + k + 1] * B3HUD_TREAD_PROFILE_MIX + f;
            float ca[4], cb[4];
            for (int c = 0; c < 4; c++) {
                ca[c] = B3_TREAD_C0[c] + (B3_TREAD_C1[c] - B3_TREAD_C0[c]) * wa0;
                cb[c] = B3_TREAD_C0[c] + (B3_TREAD_C1[c] - B3_TREAD_C0[c]) * wa1;
            }
            /* the lerped colour goes out UNMODIFIED -- the old 1.15 gain
             * and 0.55+0.45a alpha remap were the [S-ref] layer
             * intensities and are gone; the capture shows the game emits
             * exactly lerp(C0, C1, mix) * node.rgba.                 [C] */
            glBegin(GL_QUADS);
            glColor4f(ca[0], ca[1], ca[2], ca[3]);
            glTexCoord2f(u0, vr[k]);     vtx(bx + x,        by + ay0);
            glTexCoord2f(u1, vr[k]);     vtx(bx + x + segw, by + by0);
            glColor4f(cb[0], cb[1], cb[2], cb[3]);
            glTexCoord2f(u1, vr[k + 1]); vtx(bx + x + segw, by + by1);
            glTexCoord2f(u0, vr[k + 1]); vtx(bx + x,        by + ay1);
            glEnd();
        }
        x += segw;
    }
    glDisable(GL_TEXTURE_2D);
}

/* FUN_00049FD0: the BoostFireEdge plume that burns off the bar.
 * Recovered geometry, all relative to the 360x28 box (y down, 0 = box top):
 *   span      = (XBIAS + B) * w                          [x extent of fill]
 *   x(t)      = box.x - XSHIFT*w + span*t*lean,
 *               lean_top = LEAN_A + LEAN_B*flame, lean_bot = 1.1 + 0.05*flame
 *   top rail  = (YTOP-YSCALE)*h  -  t^2 * curve * B * h   [= -22 - dip]
 *   mid rail  = ((YTOP*h) + (YTOP-YSCALE)*h) * 0.5        [= -8]
 *   curve     = (CURVE_HI-CURVE_LO)*flame + (CURVE_LO-YSCALE)
 *   for t < TAPER (0.125) both rails lerp back to YTOP*h  [flat at the tail]
 *   v         = V0 (0.015625) top .. V1 (0.65) bottom
 *   frame     = (int)(24*clock) % 41, rate *= 0.95 for each further layer,
 *               each layer's span quantised down to a multiple of
 *               (STEP-STEP2)*w                                       [C]  */
static void boost_edge(float bx, float by, float w, float h, float B,
                       float flame, float clock) {
    if (g_edge_n <= 0 || B <= 0.0005f) return;
    const float x0       = bx - B3HUD_EDGE_XSHIFT * w;
    const float span0    = (B3HUD_EDGE_XBIAS + B) * w;
    const float lean_top = B3HUD_EDGE_LEAN_A + B3HUD_EDGE_LEAN_B * flame;
    const float lean_bot = 1.1f + 0.05f * flame;
    const float y_in     = B3HUD_EDGE_YTOP * h;                        /* +6  */
    const float y_top    = (B3HUD_EDGE_YTOP - B3HUD_EDGE_YSCALE) * h;  /* -22 */
    const float y_mid    = (y_in + y_top) * 0.5f;                      /* -8  */
    const float curve    = (B3HUD_EDGE_CURVE_HI - B3HUD_EDGE_CURVE_LO) * flame +
                           (B3HUD_EDGE_CURVE_LO - B3HUD_EDGE_YSCALE);
    const float step     = (B3HUD_EDGE_STEP - B3HUD_EDGE_STEP2) * w;

    /* the plume's THIRD rail: the capture shows two stacked quad strips
     * per layer, upper v 0.015625..0.65 and lower v 0.65..0.984375, with
     * leans 1.15+0.15*flame (top) / 1.10+0.05*flame (mid) / 1.0 (bottom)
     * and the bottom rail ending at the box bottom.  The harness only had
     * the upper strip, so the plume had no body.                     [C] */
    const float lean_low = 1.0f;
    const float y_bot    = h;

    glEnable(GL_TEXTURE_2D);
    state_fire();

    float rate = B3HUD_EDGE_RATE;
    float span = span0;
    for (int layer = 0; layer < 5 && span > 0.001f; layer++) {
        int fi = strip_frame(rate, clock, g_edge_n);
        if (!g_edge[fi]) break;
        glBindTexture(GL_TEXTURE_2D, g_edge[fi]);
        /* vertex colour is node.rgba = pure opaque WHITE; the plume's
         * brightness is the texture's alone (the old k*(0.55+0.85*flame)
         * grey ramp was [S-ref] glue and is gone).                    [C] */
        glColor4f(1.f, 1.f, 1.f, 1.f);
        const int N = 4;                            /* the game emits 5 columns */
        for (int half = 0; half < 2; half++) {
            glBegin(GL_QUAD_STRIP);
            for (int i = 0; i <= N; i++) {
                float t   = (float)i / (float)N;    /* 0 tail .. 1 head */
                float yt  = y_top - t * t * curve * B * h;
                float ym  = y_mid;
                float yb  = y_bot;
                if (t < B3HUD_EDGE_TAPER) {         /* flatten the tail */
                    float a = t * (1.f / B3HUD_EDGE_TAPER);
                    yt = y_in + (yt - y_in) * a;
                    ym = y_in + (ym - y_in) * a;
                    yb = y_in + (yb - y_in) * a;
                }
                if (half == 0) {
                    glTexCoord2f(t, B3HUD_EDGE_V0);
                    vtx(x0 + span * t * lean_top, by + yt);
                    glTexCoord2f(t, B3HUD_EDGE_V1);
                    vtx(x0 + span * t * lean_bot, by + ym);
                } else {
                    glTexCoord2f(t, B3HUD_EDGE_V1);
                    vtx(x0 + span * t * lean_bot, by + ym);
                    glTexCoord2f(t, B3HUD_EDGE_V2);
                    vtx(x0 + span * t * lean_low, by + yb);
                }
            }
            glEnd();
        }
        rate *= B3HUD_FLAME_RATE_DECAY;
        span  = floorf((span - 0.001f) / step) * step;
    }
    glDisable(GL_TEXTURE_2D);
}

/* FUN_00049AD0 -- the BoostFireCore blobs.  Not magenta: the game emits
 * them at {1,1,0,0.67} (0x003FCAF0) * node.rgba, i.e. YELLOW at 2/3 alpha,
 * additive.  Geometry, all confirmed against the captured vertex pool:
 *   head    = (0.0222222 + B) * w
 *   x1      = head - i*0.0888889*w ; x0 = max(x1 - 0.177778*w, 0.0222222*w)
 *   y       = -0.0357143*h .. 1.28571*h, MIRRORED about h/2 every blob
 *             (the game recomputes y := h - y each pass -- a zig-zag)
 *   frame   = (int)(rate*clock) % 30 with rate 30, *= 0.95 per blob
 *   the run ends with a gradient quad 0 .. 0.0222222*w that fades the
 *   yellow to {0,0,0,0.67} (invisible under additive).               [C] */
static void boost_core(float bx, float by, float w, float h, float B,
                       float clock) {
    (void)B3_SPARK_RAMP;                    /* [?] particle records */
    if (g_core_n <= 0 || B <= 0.0005f) return;
    const float head  = (B3HUD_CORE_XBIAS + B) * w;
    const float xend  = B3HUD_CORE_XBIAS * w;
    const float blobw = B3HUD_CORE_BLOB_W * w;
    const float step  = B3HUD_CORE_STEP * w;
    const float ya    = B3HUD_CORE_YTOP * h;
    const float yb    = B3HUD_CORE_YBOT * h;

    glEnable(GL_TEXTURE_2D);
    state_fire();

    float rate = B3HUD_CORE_RATE;
    int   flip = 0;
    for (float x1 = head; x1 > xend; x1 -= step) {
        int fi = strip_frame(rate, clock, g_core_n);
        if (!g_core[fi]) break;
        float y0 = flip ? h - yb : ya;
        float y1 = flip ? h - ya : yb;
        float x0 = x1 - blobw;
        if (x0 < xend) {
            x0 = xend;
            /* the tail: same rect shape, colour lerped to black */
            glBindTexture(GL_TEXTURE_2D, g_core[fi]);
            glBegin(GL_QUADS);
            glColor4f(0.f, 0.f, 0.f, B3_CORE_COL[3]);
            glTexCoord2f(0.f, 0.f); vtx(bx, by + y0);
            glColor4f(B3_CORE_COL[0], B3_CORE_COL[1], B3_CORE_COL[2],
                      B3_CORE_COL[3]);
            glTexCoord2f(1.f, 0.f); vtx(bx + xend, by + y0);
            glTexCoord2f(1.f, 1.f); vtx(bx + xend, by + y1);
            glColor4f(0.f, 0.f, 0.f, B3_CORE_COL[3]);
            glTexCoord2f(0.f, 1.f); vtx(bx, by + y1);
            glEnd();
        }
        rect_c(g_core[fi], B3_CORE_COL, bx + x0, by + y0, bx + x1, by + y1,
               0.f, 0.f, 1.f, 1.f);
        rate *= B3HUD_FLAME_RATE_DECAY;
        flip ^= 1;
    }
    glDisable(GL_TEXTURE_2D);
}

/* FUN_0004A470 -- the BoostFireOver streak band.  Also yellow, not
 * magenta: {1,1,0,0.8} (0x003FCB00) * node.rgba, additive, with both ends
 * faded to transparent black over 0.0333*w:
 *   x   -0.00833333*w .. (0.0166667 + B)*w, y 0 .. 0.857143*h
 *   u   (x - x0)/w * (1 + 0.0166667 + 0.00833333)                    [C] */
static void boost_over(float bx, float by, float w, float h, float B,
                       float clock) {
    if (g_over_n <= 0 || B <= 0.0005f) return;
    int fi = strip_frame(B3HUD_OVER_RATE, clock, g_over_n);
    if (!g_over[fi]) return;
    const float x0   = -B3HUD_OVER_XSHIFT * w;
    const float x3   = (B3HUD_OVER_XBIAS + B) * w;
    const float fade = B3HUD_OVER_FADE * w;
    const float us   = (1.f + B3HUD_OVER_XBIAS + B3HUD_OVER_XSHIFT) / w;
    const float y1   = B3HUD_OVER_H * h;
    float x1 = x0 + fade, x2 = x3 - fade;
    if (x1 > x2) { x1 = x2 = (x0 + x3) * 0.5f; }
    const float xs[4] = { x0, x1, x2, x3 };
    const float ea[4] = { 0.f, 1.f, 1.f, 0.f };   /* the end fade */

    glEnable(GL_TEXTURE_2D);
    state_fire();
    glBindTexture(GL_TEXTURE_2D, g_over[fi]);
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i < 4; i++) {
        float k = ea[i];
        glColor4f(B3_OVER_COL[0] * k, B3_OVER_COL[1] * k,
                  B3_OVER_COL[2] * k, B3_OVER_COL[3] * k);
        float u = (xs[i] - x0) * us;
        glTexCoord2f(u, 0.f); vtx(bx + xs[i], by);
        glTexCoord2f(u, 1.f); vtx(bx + xs[i], by + y1);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

/* FUN_00049E40: the EARN indicator.  A single BoostEarnFlame quad,
 * drawn whenever the earn flash (obj+0x52C) is above zero -- i.e. the
 * whole time boost units are being awarded, fading over ~1.3 s after.
 * It does NOT track the fill head (the function is never handed B): it
 * flares at the bar's left end and grows with k.
 *
 *   colour = 0x003FCAE0 {1,1,1,0.5} * node.rgba, with the RGB (not the
 *            alpha) scaled by  base*k , base = flag/255*0.06 + 0.94
 *            (flag = obj+0x519, 0 in normal play)
 *   x  from -0.0111111*w  to  w * (base*(0.5+0.5k)*(0.22 + 0.0111111)
 *                                  - 0.0111111)
 *   y  from -0.428571*h   to  h - (-0.428571*h)
 * additive, CLAMP.  All four checked against the capture at k =
 * 0.25/0.5/0.8/1.0.                                                 [C] */
static void boost_earn_flame(float bx, float by, float w, float h,
                             float B, float k) {
    (void)B;
    if (!g_earnflame || k <= 0.001f) return;
    const float base = B3HUD_EARN_BASE;          /* flag byte 0 */
    const float x0   = B3HUD_EARN_X0 * w;
    const float x1   = w * (base * (0.5f + 0.5f * k) *
                            (B3HUD_EARN_X1 - B3HUD_EARN_X0) + B3HUD_EARN_X0);
    const float y0   = B3HUD_EARN_YTOP * h;
    const float y1   = h - y0;
    const float c[4] = { B3_EARN_COL[0] * base * k, B3_EARN_COL[1] * base * k,
                         B3_EARN_COL[2] * base * k, B3_EARN_COL[3] };

    glEnable(GL_TEXTURE_2D);
    state_fire();
    rect_c(g_earnflame, c, bx + x0, by + y0, bx + x1, by + y1,
           0.f, 0.f, 1.f, 1.f);
    glDisable(GL_TEXTURE_2D);
}

/* Draw order is FUN_0004AE40's:
 *   plate -> sparks -> earn flame -> tread -> core -> edge -> over,
 * with the plate under the WRAP + alpha preset and everything after it
 * under CLAMP + additive; both are restored on the way out.          [C]
 * (sparks: FUN_0004A740 with the ramp at 0x003FCB10 -- the ramp is [C]
 * but the particle records are still [?], so they are not drawn.)      */
static void elem_boost(const B3BoostHud *b) {
    const float x = BOOST_X + b->shake;
    const float y = BOOST_Y;
    const float w = B3HUD_BOOST_W;
    const float h = B3HUD_BOOST_H;

    float B = b->fill;
    if (B < 0.f) B = 0.f;
    if (B > 1.f) B = 1.f;

    boost_plate(x, y, w, h, b->bar_frac, b->segments);
    boost_earn_flame(x, y, w, h, B, b->earn_flash);
    boost_tread(x, y, w, h, B, b->clock);
    boost_core(x, y, w, h, B, b->clock);
    boost_edge(x, y, w, h, B, b->flame, b->clock);
    boost_over(x, y, w, h, B, b->clock);
    state_restore();
}

/* ===================================================================== *
 *  THE EVENT TICKER -- binary-derived (docs/RE_FRONTEND.md 6.8)
 *
 *  FUN_0004D130  one category probe -> row state + row creation
 *  FUN_0004D310  the six probes + the live-row walk (life, fade, stack)
 *  FUN_0004B1C0  the row's draw node (210 x 26 at x=4, callback 0x4B4D0)
 *  FUN_0004B4D0  one row: the Globalus label + the star pips
 *
 *  Verified end-to-end by executing the real code under Unicorn:
 *  tools/emulate_hud_ticker.py.
 * ===================================================================== */

/* The element's stacking base: the boost bar's node y + h*0.5, i.e. the
 * bar's centre line.  In the 640x480 virtual screen that is y 466; rows
 * enter at 440..466 and settle 26 px higher, 414..440.              [C] */
#define TICK_BASE  (BOOST_Y + B3HUD_BOOST_H * B3HUD_TICK_BASE_FRAC)

/* the row's ref.x is 4.0 in the BOOST ELEMENT's space (@0x0004D2B0),
 * so on screen it sits 4 px right of the bar's own left edge.   [C] */
#define TICK_ROW_X (BOOST_X + B3HUD_TICK_ROW_X)

/* Row i's label scale, in atlas px -> screen px.  FUN_0004B280 multiplies
 * every glyph field by (box.w/210) * GlobalFont+0x08 * 26, and the glyph
 * records hold texture-normalised numbers, so dividing by the atlas size
 * puts it back in the atlas pixels src/burnout3_font.h stores.      [C] */
static float tick_scale_x(float node_w) {
    return (node_w / B3HUD_TICK_W) * B3HUD_TICK_FONT_SCALE
           * B3HUD_TICK_TEXT_EM / (float)b3_font_globalfont.tex_w;
}
static float tick_scale_y(float node_h) {
    return (node_h / B3HUD_TICK_H) * B3HUD_TICK_FONT_SCALE
           * B3HUD_TICK_TEXT_EM / (float)b3_font_globalfont.tex_h;
}

/* GlobalFont's ' ' record advances 7 atlas px (retail record 0x003C8758,
 * +0x18 = 0.02734375 * 256); src/burnout3_font.h -- which this module
 * cannot edit -- stores 8 for it, a generator artefact.  The ticker uses
 * the recovered number so its label metrics match the game's to the
 * 0.003 px the rest of the table already achieves.                  [C] */
static float tick_advance(unsigned char c, const B3Glyph *g) {
    if (c == ' ')
        return B3HUD_TICK_SPACE_ADV * (float)b3_font_globalfont.tex_w;
    return g->present ? g->advance : 7.f;
}

/* The x of the last vertex FUN_0004B280 emits -- the game reads exactly
 * that (`[ecx]` at 0x0004BAF7) to place the stars.                  [C] */
static float tick_text_right(const B3Font *f, const char *s,
                             float pen, float sx) {
    float right = pen;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E) continue;
        const B3Glyph *g = &f->glyph[c - 0x20];
        if (g->present && g->w > 0.f) right = pen + (g->xoff + g->w) * sx;
        pen += tick_advance(c, g) * sx;
    }
    return right;
}

/* FUN_0004B280's glyph loop: pen-relative quads, no shear, blank records
 * emit nothing but still advance.  Two colours = the vertical gradient
 * FUN_0004B4D0 applies per vertex.                                  [C] */
static void tick_text_pass(const char *s, float pen, float y,
                           float sx, float sy,
                           const float top[4], const float bot[4]) {
    const B3Font *f = &b3_font_globalfont;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_font_global);
    glBegin(GL_QUADS);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E) continue;
        const B3Glyph *g = &f->glyph[c - 0x20];
        if (g->present && g->w > 0.f) {
            float x0 = pen + g->xoff * sx;
            float y0 = y   + g->yoff * sy;
            float x1 = x0 + g->w * sx;
            float y1 = y0 + g->h * sy;
            glColor4f(top[0], top[1], top[2], top[3]);
            glTexCoord2f(g->u0, g->v0); vtx(x0, y0);
            glTexCoord2f(g->u1, g->v0); vtx(x1, y0);
            glColor4f(bot[0], bot[1], bot[2], bot[3]);
            glTexCoord2f(g->u1, g->v1); vtx(x1, y1);
            glTexCoord2f(g->u0, g->v1); vtx(x0, y1);
        }
        pen += tick_advance(c, g) * sx;
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

/* FUN_0004D130, verbatim: one category probe.  Returns 1 when it created
 * the row this frame (that is what fires the SFX at 0x0004D47D).    [C] */
static int tick_probe(int i, const B3HudTickIn *rec, float clock, float dt) {
    B3TickRow *r = &g_tick[i];
    int   show = 0;
    int   lvl  = -1;                       /* cl, "or cl,0xFF" @0x0004D151 */
    int   created = 0;

    if (rec->open) {                                   /* rec+0x10 != 0    */
        if (rec->value < B3_TICK[i].thresh) {          /* @0x0004D167      */
            if (!r->live) return 0;                    /* @0x0004D1F4      */
        } else {
            lvl  = rec->tier;                          /* rec+0x11         */
            show = 1;
            if (r->tier > lvl) r->tier = -1;           /* @0x0004D174      */
        }
    } else {
        if (rec->prev_value >= B3_TICK[i].thresh)      /* rec+0x08         */
            lvl = rec->prev_tier;                      /* rec+0x12         */
        if (!r->live) return 0;
    }

    /* the newest star pops: level up -> flash 2.0, spin restarts      [C] */
    if (lvl > -1 && lvl > r->tier) {
        r->tier  = lvl;
        r->flash = B3HUD_TICK_FLASH;
        r->phase = 0.f;
    }
    r->flash -= B3HUD_TICK_FLASH_RATE * dt;
    if (r->flash < B3HUD_TICK_FLASH_FLOOR) r->flash = B3HUD_TICK_FLASH_FLOOR;

    if (lvl == rec->count - 1) {           /* topped out: no pending star  */
        r->pulse = 0.f;
    } else if (show) {
        if (i == B3_HUD_TICK_NEARMISS) {
            /* the chain's pending star spins DOWN as the 5 s chain window
             * runs out: (1 - elapsed/window) * 5pi rad/s        @0x0004D20C */
            float t = (clock - rec->clock) / B3HUD_TICK_NM_WINDOW;
            r->phase += (1.f - t) * B3HUD_TICK_SPIN_NM * dt;
        } else {
            r->phase += B3HUD_TICK_SPIN * dt;          /* @0x0004D248      */
        }
        r->pulse = 1.f;
    } else {
        r->pulse -= B3HUD_TICK_FLASH_RATE * dt;        /* @0x0004D26A      */
    }

    if (!r->live) {                                    /* @0x0004D273      */
        r->live = 1;
        r->y    = TICK_BASE - B3HUD_TICK_ROW_STEP;     /* @0x0004D295      */
        r->node_x = TICK_ROW_X;
        r->node_w = B3HUD_TICK_W;
        r->node_h = B3HUD_TICK_H;
        r->node_a = 1.f;
        /* push to the FRONT of the element's row list (obj+0x688)         */
        for (int k = g_tick_n; k > 0; k--) g_tick_order[k] = g_tick_order[k - 1];
        g_tick_order[0] = i;
        g_tick_n++;
        created = 1;
    }
    if (show) r->timer = B3HUD_TICK_LIFE;              /* @0x0004D2F5      */
    return created;
}

/* FUN_0004D310's row walk: age, fade/shrink out, stack 26 px apart.  [C] */
static void tick_update(const B3HudState *st, float dt) {
    int fired = 0;
    for (int k = 0; k < 6; k++) {
        int i = B3_TICK_PROBE[k];
        fired |= tick_probe(i, &st->ticker[i], st->race_clock, dt);
    }
    (void)fired;   /* retail also fires an SFX here (FUN_00141010) */

    float clamp_y  = TICK_BASE - B3HUD_TICK_ROW_STEP;
    float target_y = clamp_y - B3HUD_TICK_STACK_STEP;
    int   out = 0;
    for (int k = 0; k < g_tick_n; k++) {
        B3TickRow *r = &g_tick[g_tick_order[k]];
        r->timer -= dt;
        if (r->timer < 0.f) {                       /* @0x0004D4C9 -> free */
            r->live  = 0;
            r->tier  = -1;
            r->flash = B3HUD_TICK_FLASH_FLOOR;
            r->phase = 0.f;
            r->pulse = 0.f;
            continue;                                /* drops out of the list */
        }
        float dx = 0.f, dy = 0.f, a = 1.f;
        if (r->timer < B3HUD_TICK_FADE) {            /* @0x0004D579         */
            float s = r->timer * B3HUD_TICK_FADE_RATE;
            float k2 = (1.f - s) * B3HUD_TICK_SHRINK;
            dx = k2 * B3HUD_TICK_W;
            dy = k2 * B3HUD_TICK_H;
            a  = s;
        }
        r->node_a = a;
        r->node_w = B3HUD_TICK_W + 2.f * dx;
        r->node_h = B3HUD_TICK_H + 2.f * dy;

        float y = r->y;
        if (y > clamp_y) y = clamp_y;                /* minss @0x0004D70C   */
        if (target_y > y) {
            y += B3HUD_TICK_SLIDE_RATE * dt;
            if (y > target_y) y = target_y;
        } else if (y > target_y) {
            y -= B3HUD_TICK_SLIDE_RATE * dt;
            if (y < target_y) y = target_y;
        }
        r->y      = y;
        r->node_x = TICK_ROW_X - dx;
        r->node_y = y - dy;

        target_y -= B3HUD_TICK_STACK_STEP;
        clamp_y  -= B3HUD_TICK_STACK_STEP;
        g_tick_order[out++] = g_tick_order[k];
    }
    g_tick_n = out;
}

/* One star quad.  The solid pips are axis-aligned; the pending pip is a
 * diamond spun by row.phase (corners at k*90deg - phase, radius S/2) --
 * the vertex construction at 0x0004BD60..0x0004BE79.                [C] */
static void tick_star(float cx, float cy, float hx, float hy,
                      float u0, float u1, const float rgba[4]) {
    glColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
    glTexCoord2f(u0, B3HUD_TICK_STAR_V0); vtx(cx - hx, cy - hy);
    glTexCoord2f(u1, B3HUD_TICK_STAR_V0); vtx(cx + hx, cy - hy);
    glTexCoord2f(u1, B3HUD_TICK_STAR_V1); vtx(cx + hx, cy + hy);
    glTexCoord2f(u0, B3HUD_TICK_STAR_V1); vtx(cx - hx, cy + hy);
}

static void tick_star_spin(float cx, float cy, float r, float phase,
                           const float rgba[4]) {
    float s = sinf(phase), c = cosf(phase);
    glColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
    glTexCoord2f(B3HUD_TICK_PEND_U0, B3HUD_TICK_STAR_V0);
    vtx(cx + r * s, cy + r * c);
    glTexCoord2f(B3HUD_TICK_PEND_U1, B3HUD_TICK_STAR_V0);
    vtx(cx + r * c, cy - r * s);
    glTexCoord2f(B3HUD_TICK_PEND_U1, B3HUD_TICK_STAR_V1);
    vtx(cx - r * s, cy - r * c);
    glTexCoord2f(B3HUD_TICK_PEND_U0, B3HUD_TICK_STAR_V1);
    vtx(cx - r * c, cy + r * s);
}

/* The label's fill gradient is the only part of the row that is NOT
 * recovered: FUN_0004B4D0 multiplies node.rgba by the two float4s at
 * 0x0054FA10 / 0x0054FA50, and those live in BSS with no decodable
 * initialiser (the same [?] as the 0x0054FA00 default node colour
 * already noted above).  The 8 black offset passes under it are [C].
 *
 * SOLVED off the retail frame (2026-08-12): the ONCOMING row measures a
 * flat #F9F883 (249,248,131) over its whole 14-px ink height -- a PALE
 * YELLOW, not white, and with no visible gradient, so 0x0054FA10 and
 * 0x0054FA50 are the same colour to within the capture's precision.
 * The opponent tag (RE_FRONTEND 6.10), which multiplies 0x0054FA10 by
 * its own tint, measures the same #F8F884 -- two independent consumers
 * of the same [?] global agreeing is what promotes this from a guess to
 * a solved value.  Still [S-ref]: the reference is the only source.    */
static const float B3_TICK_TOP[3] = {0.976f, 0.973f, 0.514f};  /* [S-ref] */
static const float B3_TICK_BOT[3] = {0.976f, 0.973f, 0.514f};  /* [S-ref] */

/* One row: the 8 recovered black offset passes, the label, then the
 * pending pip and the solid pips.                                   [C] */
static void tick_draw_row(const B3TickRow *r, const char *label) {
    const B3Font *f = &b3_font_globalfont;
    if (!label || !g_font_global) return;

    /* box.w/210 and box.h/26 are always the same factor (the shrink-out
     * scales both), so the label is uniform; keep both for clarity. */
    float sx = tick_scale_x(r->node_w);
    float sy = tick_scale_y(r->node_h);
    float fw = r->node_w / B3HUD_TICK_W;
    float fh = r->node_h / B3HUD_TICK_H;
    float a  = r->node_a;

    unsigned char c0 = (unsigned char)label[0];
    const B3Glyph *g0 = (c0 >= 0x20 && c0 <= 0x7E) ? &f->glyph[c0 - 0x20] : NULL;
    float pen = r->node_x - (g0 ? g0->xoff : 0.f) * sx;   /* @0x0004B34D  */

    /* the shadow: 4 copies between A-B and A+B down-right at 0.25*a^2,
     * then 4 diagonal copies at +-B.  Offsets @0x0054F520 / 0x0054F528. */
    const float A = B3HUD_TICK_SHADOW_A, B = B3HUD_TICK_SHADOW_B;
    const float off[8][2] = {
        { A + B, A + B }, { A - B, A - B }, { A - B, A + B }, { A + B, A - B },
        { B, B }, { -B, -B }, { -B, B }, { B, -B }
    };
    float sh[4] = {0.f, 0.f, 0.f, B3HUD_TICK_SHADOW_LEVEL * a * a};
    for (int i = 0; i < 8; i++)
        tick_text_pass(label, pen + off[i][0] * fw,
                       r->node_y + off[i][1] * fh, sx, sy, sh, sh);

    float top[4] = {B3_TICK_TOP[0], B3_TICK_TOP[1], B3_TICK_TOP[2], a};
    float bot[4] = {B3_TICK_BOT[0], B3_TICK_BOT[1], B3_TICK_BOT[2], a};
    tick_text_pass(label, pen, r->node_y, sx, sy, top, bot);

    if (!g_stars) return;
    if (r->tier <= 0 && r->pulse <= 0.f) return;      /* @0x0004BA9A       */

    float right = tick_text_right(f, label, pen, sx);
    float half  = B3HUD_TICK_STAR * B3HUD_TICK_STAR_HALF;
    float cx  = right + (half + B3HUD_TICK_STAR_GAP) * fw;
    float cy  = r->node_y + half * fh;
    float adv = (B3HUD_TICK_STAR - B3HUD_TICK_STAR_OVERLAP) * fw;
    float hx  = half * fw;
    float hy  = half * fh;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_stars);
    glBegin(GL_QUADS);
    if (r->pulse > 0.f) {                              /* @0x0004BCEA      */
        float col[4] = {1.f, 1.f, 1.f, a * r->pulse};
        tick_star_spin(cx + adv * (float)(r->tier > 0 ? r->tier : 0), cy,
                       hx, r->phase, col);
    }
    if (r->tier > 0) {                                 /* @0x0004BED8      */
        float col[4] = {1.f, 1.f, 1.f, a};
        for (int i = 0; i < r->tier; i++) {
            float k = (i == r->tier - 1) ? r->flash : 1.f;
            tick_star(cx + adv * (float)i, cy, hx * k, hy * k,
                      B3HUD_TICK_STAR_U0, B3HUD_TICK_STAR_U1, col);
        }
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

static void elem_ticker(void) {
    for (int k = 0; k < g_tick_n; k++) {
        int i = g_tick_order[k];
        tick_draw_row(&g_tick[i], B3_TICK[i].label);
    }
}

/* ===================================================================== *
 *  The rest of the HUD -- anchors [C], boxes [S-ref]
 * ===================================================================== */

/* Anchor slots recovered from the in-race HUD builder [C]:
 *   slot 1 = top-left, slot 5 = bottom-right, slot 7 = top-right,
 *   slot 0 = top-centre.  See docs/RE_FRONTEND.md 6.6 table.           */

/* ===================================================================== *
 *  THE CORNER PLATES -- FUN_00048430's three-slice hud_element01
 *
 *  This is the piece the previous revision got wrong twice over: it drew
 *  `big_curve` (a FRONTEND/menu corner asset whose translucent field is
 *  OUTSIDE its swoosh, which is what read as "flipped + inverted alpha")
 *  as a single stretched quad with a hand-picked 0.95 alpha.  The retail
 *  plate is `hud_element01` -- the same blade the speedo already used --
 *  cut into three horizontal slices and drawn at the 2D pass's own
 *  vertex colour under plain alpha blending.  See burnout3_hud.h.
 *
 *  Nothing here flips a texture: extract_txd.py writes PNG row 0 = v 0
 *  (THE V ORIGIN RULE), tex coords go v0 at the top of the quad, and the
 *  LAP plate's mirror is done by swapping the CAPS, exactly the way
 *  FUN_00051650 mirrors FUN_00053ED0 -- not by flipping u.          [C] */

/* The plate's vertex colour: the shared 2D default node float4 at
 * 0x0054FA00.  It has no decodable initialiser in the image (6.7.7), but
 * the retail frame pins it: the plate's dark fill is texel (0,0,0) with
 * alpha 153/255 = 0.6, and over the sky at (95,115,164) it composites to
 * (41,50,70) -- 0.4 * background to within a unit, i.e. the fill is at
 * exactly its own texture alpha and the vertex colour is OPAQUE WHITE.
 * (0.4*(95,115,164) = (38,46,66); measured (41,50,70).)          [S-ref] */
static const float B3_PLATE_NODE_RGBA[4] = {1.f, 1.f, 1.f, 1.f};

/* FUN_00048430 verbatim: up to three rects, left cap / stretched middle
 * / right cap, all at v EL_V0..EL_V1, degenerate caps skipped.      [C] */
static void plate_3slice(float x, float y, float w, float h,
                         float s0, float s1) {
    const float *c = B3_PLATE_NODE_RGBA;
    if (!g_panel) return;
    glEnable(GL_TEXTURE_2D);
    if (s0 > 0.f)                                   /* @0x00048468 */
        rect_c(g_panel, c, x, y, x + s0 * w, y + h,
               0.f, B3HUD_EL_V0, B3HUD_EL_U1, B3HUD_EL_V1);
    rect_c(g_panel, c, x + s0 * w, y, x + (1.f - s1) * w, y + h,
           B3HUD_EL_U1, B3HUD_EL_V0, B3HUD_EL_U2, B3HUD_EL_V1);
    if (s1 > 0.f)                                   /* @0x0004850D */
        rect_c(g_panel, c, x + (1.f - s1) * w, y, x + w, y + h,
               B3HUD_EL_U2, B3HUD_EL_V0, 1.f, B3HUD_EL_V1);
    glDisable(GL_TEXTURE_2D);
}

/* cap fraction = texels / ((V1-V0)*32) * H/W -- the cap keeps the
 * texture's aspect at the box height, however wide the plate is.    [C] */
static float plate_cap(float texels, float h, float w) {
    return texels / ((B3HUD_EL_V1 - B3HUD_EL_V0) * B3HUD_PLATE_CAP_DEN)
           * h / w;
}

/* The inner text placement is the only [S-ref] left in these three:
 * offsets measured off xemu-2026-08-12-16-23-19.png, expressed against
 * the element's own recovered anchor so they travel with it. */
#define POS_NUM_DX     7.0f      /* [S-ref] number ink-left,  from anchor  */
#define POS_COL_DX    37.0f      /* [S-ref] label/frac column, from anchor */
#define LAP_NUM_DX   -29.0f      /* [S-ref] number ink-RIGHT, from anchor  */
#define LAP_COL_DX   -34.0f      /* [S-ref] label/frac column, from anchor */
#define PLATE_LABEL_DY 6.9f      /* [S-ref] "POS"/"LAP" ink-top           */
#define PLATE_NUM_DY  14.4f      /* [S-ref] big numeral ink-top           */
#define PLATE_FRAC_DY 31.6f      /* [S-ref] "/N" ink-top                  */

/* The pen x that puts the first glyph's leftmost INK on `ink_x` -- the
 * rule FUN_0004B280 uses (@0x0004B34D), plus the italic shear, whose
 * leftmost point is the glyph's bottom-left corner. */
static float pen_for_ink(const B3Font *f, const char *s, float ink_x,
                         const B3TextStyle *st) {
    unsigned char c = (unsigned char)s[0];
    const B3Glyph *g = (c >= 0x20 && c <= 0x7E) ? &f->glyph[c - 0x20] : NULL;
    if (!g) return ink_x;
    float lean = (f->line_h - g->yoff - g->h) * st->scale * st->shear;
    return ink_x - g->xoff * st->scale - lean;
}

/* ...and the run y that puts the first glyph's ink TOP on `ink_y`
 * (draw_text's y is the run's origin; a glyph starts at y+yoff*scale). */
static float y_for_ink(const B3Font *f, const char *s, float ink_y,
                       const B3TextStyle *st) {
    unsigned char c = (unsigned char)s[0];
    const B3Glyph *g = (c >= 0x20 && c <= 0x7E) ? &f->glyph[c - 0x20] : NULL;
    return g ? ink_y - g->yoff * st->scale : ink_y;
}

/* POS plate: slot 1 (top-left), no left cap -- it runs off the edge. */
static void elem_position(int position, int n_cars) {
    const B3Font *f = &b3_font_globalfont;
    const float ax = B3HUD_ANCHOR_X(0.0f), ay = B3HUD_ANCHOR_Y(0.0f);
    const float w = B3HUD_POS_W, h = B3HUD_PLATE_H;
    char num[16], den[16];

    plate_3slice(ax + B3HUD_POS_REF_X,
                 ay + B3HUD_PLATE_REF_Y - B3HUD_PLATE_REF_DY, w, h,
                 0.f, plate_cap(B3HUD_CAP_R_NUM, h, w));

    snprintf(num, sizeof(num), "%d", position);
    snprintf(den, sizeof(den), "/%d", n_cars);
    draw_text(f, g_font_global, num,
              pen_for_ink(f, num, ax + POS_NUM_DX, &STYLE_GOLD_POS),
              y_for_ink(f, num, ay + PLATE_NUM_DY, &STYLE_GOLD_POS),
              &STYLE_GOLD_POS);
    draw_text(f, g_font_global, "POS",              /* Globalus 2002 [C] */
              pen_for_ink(f, "POS", ax + POS_COL_DX, &STYLE_WHITE_LABEL),
              y_for_ink(f, "POS", ay + PLATE_LABEL_DY, &STYLE_WHITE_LABEL),
              &STYLE_WHITE_LABEL);
    draw_text(f, g_font_global, den,
              pen_for_ink(f, den, ax + POS_COL_DX, &STYLE_GOLD_FRAC),
              y_for_ink(f, den, ay + PLATE_FRAC_DY, &STYLE_GOLD_FRAC),
              &STYLE_GOLD_FRAC);
}

/* LAP plate: slot 7 (top-right), the MIRROR -- cap on the left, box
 * right edge at anchor + LAP_REF_X.                                 [C] */
static void elem_lap(int lap, int total_laps) {
    const B3Font *f = &b3_font_globalfont;
    const float ax = B3HUD_ANCHOR_X(1.0f), ay = B3HUD_ANCHOR_Y(0.0f);
    const float w = B3HUD_POS_W, h = B3HUD_PLATE_H;
    char num[16], den[16];

    plate_3slice(ax + B3HUD_LAP_REF_X - w,
                 ay + B3HUD_PLATE_REF_Y - B3HUD_PLATE_REF_DY, w, h,
                 plate_cap(B3HUD_CAP_L_NUM, h, w), 0.f);

    snprintf(num, sizeof(num), "%d", lap);
    snprintf(den, sizeof(den), "/%d", total_laps);
    float nw = text_width(f, num, STYLE_GOLD_POS.scale);
    draw_text(f, g_font_global, num,
              pen_for_ink(f, num, ax + LAP_NUM_DX - nw, &STYLE_GOLD_POS),
              y_for_ink(f, num, ay + PLATE_NUM_DY, &STYLE_GOLD_POS),
              &STYLE_GOLD_POS);
    draw_text(f, g_font_global, "LAP",              /* Globalus 2003 [C] */
              pen_for_ink(f, "LAP", ax + LAP_COL_DX, &STYLE_WHITE_LABEL),
              y_for_ink(f, "LAP", ay + PLATE_LABEL_DY, &STYLE_WHITE_LABEL),
              &STYLE_WHITE_LABEL);
    draw_text(f, g_font_global, den,
              pen_for_ink(f, den, ax + LAP_COL_DX, &STYLE_GOLD_FRAC),
              y_for_ink(f, den, ay + PLATE_FRAC_DY, &STYLE_GOLD_FRAC),
              &STYLE_GOLD_FRAC);
}

/* Speed cluster: slot 5 (bottom-right), BOTH caps, box (-W,-H) from the
 * anchor.  The numerals deliberately overhang the plate's top, as they
 * do in the retail frame.                                           [C] */
#define SPEED_NUM_DX  -65.0f     /* [S-ref] numeral ink-RIGHT from anchor  */
#define SPEED_NUM_DY  -35.9f     /* [S-ref] numeral ink-top                */
#define SPEED_UNIT_DX -62.0f     /* [S-ref] "mph" ink-left                 */
#define SPEED_UNIT_DY -24.0f     /* [S-ref] "mph" ink-top                  */
#define SPEED_TRACK     5.5f     /* [S-ref] extra digit tracking, px       */

static void elem_speed(float mph) {
    const B3Font *f = &b3_font_globalfont;
    const float ax = B3HUD_ANCHOR_X(1.0f), ay = B3HUD_ANCHOR_Y(1.0f);
    const float w = B3HUD_SPEED_W, h = B3HUD_SPEED_H;
    char num[8];
    int v = (int)(mph + 0.5f);
    if (v < 0) v = 0;
    if (v > 999) v = 999;

    plate_3slice(ax - w, ay - h, w, h,
                 plate_cap(B3HUD_CAP_L_NUM, h, w),
                 plate_cap(B3HUD_CAP_R_NUM, h, w));

    snprintf(num, sizeof(num), "%d", v);
    /* the retail speedo tracks its digits ~5.5 px wider than the font's
     * own advance (26 px per digit measured, 15*1.27 = 19 from the
     * metrics) -- drawn glyph by glyph so the extra spacing is real. */
    float adv = 0.f;
    for (const char *p = num; *p; p++) {
        unsigned char c = (unsigned char)*p;
        const B3Glyph *g = &f->glyph[c - 0x20];
        adv += g->advance * STYLE_GOLD_BIG.scale + SPEED_TRACK;
    }
    adv -= SPEED_TRACK;
    float pen = ax + SPEED_NUM_DX - adv;
    for (const char *p = num; *p; p++) {
        char one[2] = { *p, 0 };
        unsigned char c = (unsigned char)*p;
        const B3Glyph *g = &f->glyph[c - 0x20];
        draw_text(f, g_font_global, one, pen,
                  y_for_ink(f, one, ay + SPEED_NUM_DY, &STYLE_GOLD_BIG),
                  &STYLE_GOLD_BIG);
        pen += g->advance * STYLE_GOLD_BIG.scale + SPEED_TRACK;
    }
    draw_text(f, g_font_global, "mph",              /* Globalus 1987 [C] */
              pen_for_ink(f, "mph", ax + SPEED_UNIT_DX,
                          &STYLE_GOLD_LABEL),
              y_for_ink(f, "mph", ay + SPEED_UNIT_DY, &STYLE_GOLD_LABEL),
              &STYLE_GOLD_LABEL);
}

/* ===================================================================== *
 *  THE OPPONENT TAGS -- binary-derived (docs/RE_FRONTEND.md 6.10)
 *
 *  FUN_0018F060, one call per rival.  The whole element is here: the
 *  size curve, the near/far alpha envelope, the off-screen culls, the
 *  35-unit ordinal/triangle switch and the flat three-vertex triangle.
 *  Every constant carries its address in burnout3_hud.h.
 *
 *  Two things are NOT the game's:
 *    * the hysteresis latch retail keeps per tag slot (state[slot] +
 *      the [ebx+0x18] gate @0x0018F218) is omitted -- the harness's
 *      entry point is stateless per call, and the latch only matters
 *      for a car sitting exactly on the threshold;
 *    * the colour, which retail takes from the [?] 2D text colour
 *      0x0054FA10 through a per-game-mode tint table at 0x00416830
 *      (rival = (0.49,0.25,0,1), takedown-able = (0.5,0,0,1), other =
 *      (0.5,0.5,0.5,1)).  Measured off the retail frame instead.
 * ===================================================================== */

const char *b3_hud_place_ordinal(int place) {
    /* Globalus entries B3HUD_TAG_STR_1ST/4 + (place-1) = 1993..1998, the
     * SIX strings retail indexes with word[car+0x10D0]-1 (@0x0018EDBB).
     * The run is only six long -- entries 1999/2000 are ":" and " x " --
     * which is consistent with the six-car race grid; a seventh place
     * would read the next table entry, so the element simply draws
     * nothing there.  (The other "1st".."8th" run at entry 587 is a
     * different table, used by the results screens.)                 [C] */
    static const char *const ORD[6] = {
        "1st", "2nd", "3rd", "4th", "5th", "6th"
    };
    if (place < 1 || place > 6) return NULL;
    return ORD[place - 1];
}

/* The alpha envelope: TAG_ALPHA, faded out past TAG_FADE_FAR at
 * TAG_FADE_RATE per unit and faded in over TAG_NEAR_LO..TAG_NEAR_HI.
 * Returns <= 0 when the tag must not be drawn at all.               [C] */
static float tag_alpha(float z) {
    float a = B3HUD_TAG_OPACITY;                       /* @0x0018F7B8 */
    if (z > B3HUD_TAG_FADE_FAR)                      /* @0x0018F66E */
        a -= (z - B3HUD_TAG_FADE_FAR) * B3HUD_TAG_FADE_RATE;
    if (z < B3HUD_TAG_NEAR_HI) {                     /* @0x0018F694 */
        if (z < B3HUD_TAG_NEAR_LO) return 0.f;       /* @0x0018F6A1 */
        a *= (z - B3HUD_TAG_NEAR_LO) * B3HUD_TAG_NEAR_K;
    }
    if (a > B3HUD_TAG_OPACITY) a = B3HUD_TAG_OPACITY;
    return a;
}

/* FUN_0018F060's size curve, @0x0018F28E..@0x0018F2BF.  `scale` is the
 * caller's per-tag scale (retail pushes 0.5 @0x0018EDAF).           [C] */
static float tag_size(float z, float scale) {
    float s = ((z - 1.f) * B3HUD_TAG_SIZE_K + 1.f) / z
              * B3HUD_VIRT_W * B3HUD_TAG_SIZE_SCALE * scale;
    if (s > B3HUD_TAG_SIZE_MAX) s = B3HUD_TAG_SIZE_MAX;  /* @0x0018F2BC */
    return s;
}

/* The base scale retail hands FUN_0018F060.  0.5 is the literal the
 * caller pushes (0x3F000000 @0x0018EDAF), but that is scaled against the
 * retail viewport's own pixel width; against the reference frame the
 * tag's "4th" measures 15 virtual px of cap height at z ~ 22 m and the
 * far triangles 7 x 5 px at z ~ 120 m, which lands the harness's base
 * here.                                                        [S-ref] */
#define B3_TAG_SCALE  0.5f
/* size_y / size.  Retail builds this from the viewport in [esp+0x40]
 * (@0x0018F0B7) out of camera fields that are runtime data.  Pinned off
 * the reference frame instead: the far triangles there measure 7 x 5 px,
 * and half-width = size*0.3 / half-height = size_y*0.5 with size = 10.0
 * at the 35 m threshold gives 6.0 x 4.3 -- the measured shape to within
 * the capture's antialiasing.                                   [S-ref] */
#define B3_TAG_ASPECT 0.428571f
/* The text run's em: retail multiplies size_y by 2.0 (@0x0018F7AC) and
 * hands that to the text builder, so with the MIN_H = 7 floor the
 * ordinal never draws smaller than a 14 px cap -- which is exactly what
 * "4th" measures in the reference frame (14 rows / 15 virtual px).  [C] */
#define B3_TAG_EM     2.0f

int b3_hud_opponent_tag(float screen_x, float screen_y, float distance,
                        int place, int visible) {
    if (!g_ready || !visible) return 0;                  /* @0x0018EC8D */
    if (distance <= 0.f) return 0;

    float a = tag_alpha(distance);
    if (a <= 0.f) return 0;

    float size   = tag_size(distance, B3_TAG_SCALE);
    float size_y = size * B3_TAG_ASPECT;
    float sx = screen_x;
    float sy = screen_y - size_y * B3HUD_TAG_YOFF;       /* @0x0018F2F7 */

    /* the four off-screen culls, @0x0018F338..@0x0018F384 */
    if (sx < -size * B3HUD_TAG_HALF)   return 0;
    if (sy < -size_y * B3HUD_TAG_HALF) return 0;
    if (sx > B3HUD_VIRT_W + size * B3HUD_TAG_HALF)   return 0;
    if (sy > B3HUD_VIRT_H + size_y * B3HUD_TAG_HALF) return 0;

    state_begin();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   /* preset 0 [C] */

    if (distance >= B3HUD_TAG_DIST) {
        /* FAR: a flat three-vertex triangle, apex DOWN, no texture.
         * FUN_001C7C90 @0x0018F757 with the verts built @0x0018F6D7. */
        float hw = size * B3HUD_TAG_TRI_W;               /* @0x0018F6E3 */
        float hh = size_y * B3HUD_TAG_HALF;              /* @0x0018F373 */
        glDisable(GL_TEXTURE_2D);
        glColor4f(STYLE_TAG.top[0], STYLE_TAG.top[1], STYLE_TAG.top[2], a);
        glBegin(GL_TRIANGLES);
        vtx(sx - hw, sy - hh);
        vtx(sx + hw, sy - hh);
        vtx(sx,      sy + hh);
        glEnd();
    } else {
        /* NEAR: the ordinal, floors applied (@0x0018F761..@0x0018F78F). */
        const char *ord = b3_hud_place_ordinal(place);
        if (!ord) { state_end(); return 0; }
        if (size   < B3HUD_TAG_MIN_W) size   = B3HUD_TAG_MIN_W;
        if (size_y < B3HUD_TAG_MIN_H) size_y = B3HUD_TAG_MIN_H;
        B3TextStyle st = STYLE_TAG;
        /* cap height = size_y * 2 (@0x0018F7AC); GlobalFont's digits are
         * 22 atlas px tall. */
        st.scale = size_y * B3_TAG_EM / 22.f;
        st.top[3] = st.bot[3] = a;
        st.outline[3] = B3HUD_TICK_SHADOW_LEVEL * a * a;   /* [C] 6.8.5 */
        st.shadow_a = B3HUD_SHADOW_A_MED * (st.scale > 1.f ? 1.f : st.scale);
        float w = text_width(&b3_font_globalfont, ord, st.scale);
        /* box top-left = (sx, sy - size_y) @0x0018F7B4, centred on sx */
        draw_text(&b3_font_globalfont, g_font_global, ord,
                  sx - w * 0.5f,
                  y_for_ink(&b3_font_globalfont, ord, sy - size_y, &st), &st);
    }
    state_end();
    return 1;
}

/* ===================================================================== *
 *  THE EA TRAX NOW-PLAYING BANNER -- GLUE (RE_FRONTEND 6.9)
 *
 *  A sibling of the POS / LAP plates, built out of the same three things
 *  they are built out of:
 *    * the plate is `big_curve` stretched across the box in the POS
 *      element's orientation, so the same swoosh that runs under
 *      "POS. 4/6" runs the length of the banner under the two lines;
 *    * the glyphs are the recovered GlobalFont through the same
 *      draw_text() pen + 8-offset shadow every other label uses;
 *    * the badge is the game's own EATrax sheet -- a 256x128 pair whose
 *      RIGHT 128x128 half is the EA TRAX mark (the left half is the EA
 *      GAMES roundel).  It is stored rotated, TRAX reading top-to-bottom,
 *      so the quad's UVs are rotated a quarter turn to stand it up.
 *
 *  White primary line = the song title, HUD-blue secondary line = the
 *  band, matching the way POS. / LAP pair a white label with a coloured
 *  value.  The blue is big_curve's own swoosh colour, sampled from the
 *  texture: (99,142,214) top, deepened to (60,105,190) at the baseline.
 * ===================================================================== */

static const B3TextStyle STYLE_TRAX_TITLE = {
    B3HUD_TRAX_TITLE_SCALE, 0.08f,
    {1.00f, 1.00f, 1.00f, 1.f}, {0.86f, 0.90f, 0.96f, 1.f},
    B3_SHADOW_RGBA, B3HUD_TICK_SHADOW_A
};
static const B3TextStyle STYLE_TRAX_ARTIST = {
    B3HUD_TRAX_ARTIST_SCALE, 0.08f,
    {0.388f, 0.557f, 0.839f, 1.f}, {0.235f, 0.412f, 0.745f, 1.f},
    B3_SHADOW_RGBA, B3HUD_TICK_SHADOW_A
};

/* Longest titles in the table run to 40 characters ("Reinventing The
 * Wheel To Run Myself Over"), so the line is shrunk to fit rather than
 * clipped or scrolled. */
static float trax_fit(const char *s, float scale, float max_w) {
    float w = text_width(&b3_font_globalfont, s, scale);
    if (w > max_w && w > 0.f) scale *= max_w / w;
    return scale;
}

/* The animation, factored out so tools/validate_music.py can call it. */
float b3_hud_music_box(float elapsed, float *x, float *y,
                       float *w, float *h) {
    float a = 1.f, slide = 0.f;
    if (w) *w = B3HUD_TRAX_W;
    if (h) *h = B3HUD_TRAX_H;
    if (y) *y = B3HUD_TRAX_Y;
    if (x) *x = B3HUD_TRAX_X;
    if (elapsed < 0.f || elapsed > B3HUD_TRAX_LIFE) return 0.f;
    if (elapsed < B3HUD_TRAX_IN) {
        float t = elapsed / B3HUD_TRAX_IN;
        a = t;
        slide = -(1.f - t) * B3HUD_TRAX_SLIDE;
    } else if (elapsed > B3HUD_TRAX_LIFE - B3HUD_TRAX_OUT) {
        float t = (B3HUD_TRAX_LIFE - elapsed) / B3HUD_TRAX_OUT;
        a = t;
        slide = -(1.f - t) * B3HUD_TRAX_SLIDE * 0.5f;
    }
    if (x) *x = B3HUD_TRAX_X + slide;
    return a;
}

static void elem_music(const B3HudMusicIn *m) {
    float bx, by, bw, bh, a;
    if (!m || !m->artist || !m->title) return;
    a = b3_hud_music_box(m->elapsed, &bx, &by, &bw, &bh);
    if (a <= 0.f) return;

    /* The plate: the recovered THREE-SLICE hud_element01 the POS / LAP /
     * speed corners draw (RE_FRONTEND 6.10) -- both caps, so the banner
     * reads as a free-standing blade rather than a screen-edge corner.
     * The banner used to draw `big_curve` here on top of a hand-picked
     * opaque dark band; big_curve is the menu asset whose translucent
     * field sits OUTSIDE its swoosh, which is what made this (and the
     * POS / LAP plates) look flipped with inverted alpha.  The blade's
     * own 0.6-alpha fill is what the band was faking, so the band is
     * gone and the element's node rgba scales the plate the way
     * node+0x10 scales every 2D batch (FUN_001C6920).                [C] */
    {
        float capl = plate_cap(B3HUD_CAP_L_NUM, bh, bw);
        float capr = plate_cap(B3HUD_CAP_R_NUM, bh, bw);
        const float col[4] = {1.f, 1.f, 1.f, a};
        glEnable(GL_TEXTURE_2D);
        rect_c(g_panel, col, bx, by, bx + capl * bw, by + bh,
               0.f, B3HUD_EL_V0, B3HUD_EL_U1, B3HUD_EL_V1);
        rect_c(g_panel, col, bx + capl * bw, by, bx + (1.f - capr) * bw,
               by + bh, B3HUD_EL_U1, B3HUD_EL_V0, B3HUD_EL_U2, B3HUD_EL_V1);
        rect_c(g_panel, col, bx + (1.f - capr) * bw, by, bx + bw, by + bh,
               B3HUD_EL_U2, B3HUD_EL_V0, 1.f, B3HUD_EL_V1);
        glDisable(GL_TEXTURE_2D);
    }

    /* the EA TRAX badge: right half of the sheet, stood up a quarter
     * turn (the texture stores TRAX reading downwards). */
    if (g_eatrax) {
        float ix = bx + B3HUD_TRAX_PAD;
        float iy = by + (bh - B3HUD_TRAX_ICON) * 0.5f;
        float s = B3HUD_TRAX_ICON;
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_eatrax);
        glColor4f(1.f, 1.f, 1.f, a);
        glBegin(GL_QUADS);
        glTexCoord2f(1.0f, 0.f); vtx(ix,     iy);
        glTexCoord2f(1.0f, 1.f); vtx(ix + s, iy);
        glTexCoord2f(0.5f, 1.f); vtx(ix + s, iy + s);
        glTexCoord2f(0.5f, 0.f); vtx(ix,     iy + s);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }

    float tx = bx + B3HUD_TRAX_TEXT_X;
    float max_w = bw - B3HUD_TRAX_TEXT_X - B3HUD_TRAX_PAD;

    B3TextStyle st = STYLE_TRAX_TITLE;
    st.scale = trax_fit(m->title, st.scale, max_w);
    st.top[3] = st.bot[3] = a;
    st.outline[3] = 0.90f * a;
    draw_text(&b3_font_globalfont, g_font_global, m->title,
              tx, by + B3HUD_TRAX_TITLE_Y, &st);

    st = STYLE_TRAX_ARTIST;
    st.scale = trax_fit(m->artist, st.scale, max_w);
    st.top[3] = st.bot[3] = a;
    st.outline[3] = 0.90f * a;
    draw_text(&b3_font_globalfont, g_font_global, m->artist,
              tx, by + B3HUD_TRAX_ARTIST_Y, &st);
}

/* ===================================================================== *
 *  THE CRASH SHOW  --  "(A) IMPACT TIME" + the crash ticker band
 *  Recovery: the crash block of burnout3_hud.h (FUN_00051230,
 *  FUN_0004ED40, FUN_0017A720, FUN_0017A6B0, and the tables 0x003A2F70 /
 *  0x003A06F0 / 0x003A0830 / 0x003A08D0).
 * ===================================================================== */

/* B3HUD-CRASHDESC-BEGIN -- one row per 16 bytes of 0x003A2F70.  `str` is
 * the Globalus entry, `flags` the second u32, `dbl`/`tri` the third and
 * fourth (the "Double ..."/"Triple ..." replacements, 0 = none).  The
 * English text is the Data/Globalus.bin string at `str`, checked by
 * tools/validate_hud.py against the file itself.                    [C] */
static const struct {
    const char *fmt;      /* Globalus[str]                                */
    int         str;      /* row+0x00                                     */
    unsigned    flags;    /* row+0x04                                     */
    int         dbl, tri; /* row+0x08, row+0x0C                           */
    const char *dbl_s, *tri_s;
} B3_CRASH_DESC[B3_HUD_CD_COUNT] = {
    { "Into The Wall",        1833, 0, 0,    0,    NULL, NULL },
    { "Into A Vehicle",       1834, 0, 0,    0,    NULL, NULL },
    { "Hit A Ramp",           1835, 0, 0,    0,    NULL, NULL },
    { "Rollover",             1836, 0, 0,    0,    NULL, NULL },
    { "Rightsided",           1837, 0, 0,    0,    NULL, NULL },
    { "Barrel Roll x%1",      1838, 1, 2246, 2249, "Double Barrel Roll", "Triple Barrel Roll" },
    { "Front Flip x%1",       1839, 1, 2247, 2250, "Double Front Flip",  "Triple Front Flip"  },
    { "Back Flip x%1",        1840, 1, 2248, 2251, "Double Back Flip",   "Triple Back Flip"   },
    { "Helicopter %1",        1841, 3, 0,    0,    NULL, NULL },
    { "Cartwheel %1",         1842, 3, 0,    0,    NULL, NULL },
    { "Rodeo %1",             1843, 3, 0,    0,    NULL, NULL },
    { "Alley Oop %1",         1844, 3, 0,    0,    NULL, NULL },
    { "Crazy Style %1",       1845, 3, 0,    0,    NULL, NULL },
    { "Head Spin %1",         1846, 3, 0,    0,    NULL, NULL },
    { "%1%2 Sunroof Skid",    1847, 4, 0,    0,    NULL, NULL },
    { "%1%2 Skid",            1848, 4, 0,    0,    NULL, NULL },
    { "%1%2 Side Panel Skid", 1849, 4, 0,    0,    NULL, NULL },
    { "%1%2 Nose Grind",      1850, 4, 0,    0,    NULL, NULL },
    { "%1%2 Tail Grind",      1851, 4, 0,    0,    NULL, NULL },
    { "Roof Wreck",           1852, 0, 0,    0,    NULL, NULL },
    { "Nose Dive",            1853, 0, 0,    0,    NULL, NULL },
    { "Exhaust Stand",        1854, 0, 0,    0,    NULL, NULL },
    { "Wing Mirror Crush",    1855, 0, 0,    0,    NULL, NULL },
    { "Soft Landing",         1856, 0, 0,    0,    NULL, NULL },
    { "Payload Spill",        1857, 0, 0,    0,    NULL, NULL },
    { "%1%2 Air",             1858, 8, 0,    0,    NULL, NULL },
    { "Burst Into Flames",    1859, 0, 0,    0,    NULL, NULL },
    { "Exploded",             1860, 0, 0,    0,    NULL, NULL },
    { "Over Vehicle",         1861, 0, 0,    0,    NULL, NULL },
    { "Collected Pickup",     1862, 0, 0,    0,    NULL, NULL }
};
/* B3HUD-CRASHDESC-END */

/* B3HUD-CRASHVEH-BEGIN -- the 40 traffic models FUN_00158AD0 knows.
 * `id` is the 64-bit base-40 key pair at 0x003A06F0 + i*8 decoded with
 * the VEHICLE alphabet "!'_0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ?" (the
 * SFX charmap rotated by two -- validated by all 40 rows decoding to a
 * clean <CLASS>CAR<nn>), and it is exactly the string B3_TRAFFIC_CARS[]
 * .id carries.  `name`/`into` are Globalus 0x003A0830[i]/0x003A08D0[i].
 *                                                                   [C] */
static const struct { const char *id, *name, *into; } B3_CRASH_VEH[40] = {
    { "COMPCAR11", "Car",               "Into Car"               },
    { "COMPCAR12", "Car",               "Into Car"               },
    { "COMPCAR13", "Car",               "Into Car"               },
    { "COMPCAR14", "Car",               "Into Car"               },
    { "COMPCAR15", "Taxi",              "Into Taxi"              },
    { "COMPCAR16", "Taxi",              "Into Taxi"              },
    { "COMPCAR17", "Taxi",              "Into Taxi"              },
    { "COMPCAR18", "Tuk Tuk",           "Into Tuk Tuk"           },
    { "COMPCAR19", "Car",               "Into Car"               },
    { "HEVYCAR11", "Car",               "Into Car"               },
    { "HEVYCAR12", "Car",               "Into Car"               },
    { "HEVYCAR13", "Car",               "Into Car"               },
    { "HEVYCAR14", "Pickup",            "Into Pickup"            },
    { "HEVYCAR15", "Pickup",            "Into Pickup"            },
    { "HEVYCAR16", "Van",               "Into Van"               },
    { "HEVYCAR17", "Van",               "Into Van"               },
    { "HEVYCAR18", "Van",               "Into Van"               },
    { "HEVYCAR19", "Bus",               "Into Bus"               },
    { "HEVYCAR20", "Bus",               "Into Bus"               },
    { "HEVYCAR21", "Bus",               "Into Bus"               },
    { "HEVYCAR22", "Big Rig",           "Into Big Rig"           },
    { "HEVYCAR23", "Big Rig",           "Into Big Rig"           },
    { "HEVYCAR24", "Freight Container", "Into Freight Container" },
    { "HEVYCAR25", "Freight Container", "Into Freight Container" },
    { "HEVYCAR26", "Freight Container", "Into Freight Container" },
    { "HEVYCAR27", "Flatbed",           "Into Flatbed"           },
    { "HEVYCAR28", "Lumber Trailer",    "Into Lumber Trailer"    },
    { "HEVYCAR29", "Tanker",            "Into Tanker"            },
    { "HEVYCAR30", "Covered Trailer",   "Into Covered Trailer"   },
    { "HEVYCAR31", "Minibus",           "Into Minibus"           },
    { "HEVYCAR32", "Gritting Truck",    "Into Gritting Truck"    },
    { "HEVYCAR33", "Van",               "Into Van"               },
    { "HEVYCAR34", "Motorhome",         "Into Motorhome"         },
    { "HEVYCAR35", "Van",               "Into Van"               },
    { "TSPCCAR1",  "Tram",              "Into Tram"              },
    { "TSPCCAR2",  "Tram",              "Into Tram"              },
    { "TSPCCAR3",  "Tram",              "Into Tram"              },
    { "TSPCCAR4",  "Tram",              "Into Tram"              },
    { "TSPCCAR5",  "Tram",              "Into Tram"              },
    { "TSPCCAR6",  "Tram",              "Into Tram"              }
};
/* B3HUD-CRASHVEH-END */

int b3_hud_crash_traffic_index(const char *id) {
    if (!id) return -1;
    for (int i = 0; i < B3HUD_CRASH_MODEL_MAX; i++)
        if (!strcmp(B3_CRASH_VEH[i].id, id)) return i;
    return -1;              /* FUN_00158AD0's `or eax,-1` @0x00158AFC */
}

const char *b3_hud_crash_vehicle_name(int i) {
    return (i >= 0 && i < B3HUD_CRASH_MODEL_MAX) ? B3_CRASH_VEH[i].name : NULL;
}

const char *b3_hud_crash_vehicle_into(int i) {
    return (i >= 0 && i < B3HUD_CRASH_MODEL_MAX) ? B3_CRASH_VEH[i].into : NULL;
}

/* Substitute the game's %1 / %2 placeholders.  Retail hands the two
 * arguments to the text formatter as a pointer pair (FUN_00058A40's
 * &number / &unit, @0x0017A7A2..0x0017A7B9); the strings themselves are
 * the Globalus ones, so the substitution rule is all the port needs. [C] */
static int crash_fmt(char *out, int cap, const char *fmt,
                     const char *a1, const char *a2) {
    int n = 0;
    for (; *fmt && n < cap - 1; fmt++) {
        const char *sub = NULL;
        if (fmt[0] == '%' && fmt[1] == '1') sub = a1;
        else if (fmt[0] == '%' && fmt[1] == '2') sub = a2;
        if (sub) {
            fmt++;
            for (; *sub && n < cap - 1; sub++) out[n++] = *sub;
        } else {
            out[n++] = *fmt;
        }
    }
    out[n] = '\0';
    return n;
}

/* Units: [0x0045B9BC] 0 or 1 selects imperial (x3.28084, "ft"), any
 * other value metric (x1, "m").  The harness HUD is imperial (the
 * speedo is mph), so imperial is the default.                       [C] */
static int g_crash_metric;

int b3_hud_crash_descriptor(char *out, int cap, int type,
                            int hit_kind, const char *hit_id,
                            int count, float value) {
    char num[32];
    const char *fmt;
    unsigned flags;

    if (!out || cap <= 0) return 0;
    out[0] = '\0';
    if (type < 0 || type >= B3_HUD_CD_COUNT) return 0;
    flags = B3_CRASH_DESC[type].flags;
    fmt   = B3_CRASH_DESC[type].fmt;

    /* FUN_0017A6B0: type 1 resolves against the thing that was hit. */
    if (type == B3_HUD_CD_INTO_VEHICLE) {
        const char *s = NULL;
        if (hit_kind == B3_HUD_HIT_RIVAL) s = "Into Rival";   /* 2245 */
        else if (hit_kind == B3_HUD_HIT_TRAFFIC)
            s = b3_hud_crash_vehicle_into(b3_hud_crash_traffic_index(hit_id));
        if (!s) s = "Into Car";                               /* 0x797 */
        fmt = s;
        flags = 0;
    }

    if (flags & 4u) {                    /* distance: metres -> ft or m  */
        float k = g_crash_metric ? B3HUD_CRASH_M_PER_M : B3HUD_CRASH_FT_PER_M;
        snprintf(num, sizeof num, "%d", (int)(value * k));  /* CVTTSS2SI */
        return crash_fmt(out, cap, fmt, num, g_crash_metric ? "m" : "ft");
    }
    if (flags & 8u) {                    /* air: sprintf("%.1f") + "s"   */
        snprintf(num, sizeof num, "%.1f", value);
        return crash_fmt(out, cap, fmt, num, "s");
    }
    if (flags & 1u) {
        if (flags & 2u) {                /* half-turns -> degrees        */
            snprintf(num, sizeof num, "%d", count * B3HUD_CRASH_SPIN_DEG);
            return crash_fmt(out, cap, fmt, num, "");
        }
        if (count >= B3HUD_CRASH_MULTI_LO && count <= B3HUD_CRASH_MULTI_HI) {
            const char *s = (count == B3HUD_CRASH_MULTI_LO)
                          ? B3_CRASH_DESC[type].dbl_s
                          : B3_CRASH_DESC[type].tri_s;
            if (s) return crash_fmt(out, cap, s, "", "");
        }
        snprintf(num, sizeof num, "%d", count);
        return crash_fmt(out, cap, fmt, num, "");
    }
    return crash_fmt(out, cap, fmt, "", "");
}

/* ---- the band's live state ------------------------------------------ */

#define B3_CRASH_MAX_EVENTS 8
#define B3_CRASH_LINE_CAP   256

typedef struct {
    int   active;
    int   n;
    char  ev[B3_CRASH_MAX_EVENTS][48];
    char  line[B3_CRASH_LINE_CAP];   /* FUN_0004ED40's buffer, everything
                                      * but the newest: "<d0> + <d1> "   */
    float age;
    /* trackers -- GLUE (see crash_track) */
    float skid_m;
    int   skid_kind;
    float air_s;
    float roll_acc;        /* accumulated roll about fwd, RADIANS (signed) */
    int   rolls;
    float prev_right_y;    /* retained: the skid family still reads it     */
    float prev_roll;       /* last unwrapped roll angle                    */
    int   roll_init;       /* 0 until the first airborne sample            */
    int   filed_wall;
} B3CrashHud;

static B3CrashHud g_crash;

/* Drop everything the band remembers (also the module's reset path). */
void b3_hud_crash_reset(void) { memset(&g_crash, 0, sizeof g_crash); }

/* FUN_0004ED40 over every event but the newest: each descriptor gets a
 * leading "+ " when it is not the first and a trailing " ".         [C] */
static void crash_rebuild_line(void) {
    int n = 0, i;
    g_crash.line[0] = '\0';
    for (i = 0; i < g_crash.n - 1; i++) {
        const char *s;
        if (i > 0 && n < B3_CRASH_LINE_CAP - 3) {   /* L"+ " @0x0038867C */
            g_crash.line[n++] = '+';
            g_crash.line[n++] = ' ';
        }
        for (s = g_crash.ev[i]; *s && n < B3_CRASH_LINE_CAP - 2; s++)
            g_crash.line[n++] = *s;
        if (n < B3_CRASH_LINE_CAP - 1)              /* L" "  @0x00388678 */
            g_crash.line[n++] = ' ';
        g_crash.line[n] = '\0';
    }
}

static void crash_push(int type, int hit_kind, const char *hit_id,
                       int count, float value) {
    char buf[48];
    int i;
    if (type < 0) return;
    if (!b3_hud_crash_descriptor(buf, (int)sizeof buf, type,
                                 hit_kind, hit_id, count, value))
        return;
    if (g_crash.n > 0 && !strcmp(g_crash.ev[g_crash.n - 1], buf)) return;
    if (g_crash.n == B3_CRASH_MAX_EVENTS) {
        for (i = 1; i < B3_CRASH_MAX_EVENTS; i++)
            memcpy(g_crash.ev[i - 1], g_crash.ev[i], sizeof g_crash.ev[0]);
        g_crash.n--;
    }
    snprintf(g_crash.ev[g_crash.n], sizeof g_crash.ev[0], "%s", buf);
    g_crash.n++;
    g_crash.age = 0.f;
    crash_rebuild_line();
}

const char *b3_hud_crash_ticker(const char **newest) {
    if (newest) *newest = g_crash.n > 0 ? g_crash.ev[g_crash.n - 1] : NULL;
    return (g_crash.active && g_crash.n > 0) ? g_crash.line : NULL;
}

/* GLUE / TUNED.  Retail files each descriptor from its own per-panel
 * contact bookkeeping, which was NOT recovered; what IS recovered is the
 * FORMAT and its argument kind, so the port feeds the recovered formats
 * with quantities the wreck sim already produces:
 *   - the sliding attitude picks the skid family (up.y for the roof,
 *     right.y for a side panel, fwd.y for a nose/tail grind);
 *   - the argument is the ground path travelled while that attitude
 *     holds, in METRES, which is exactly what the &4 formats want;
 *   - air time in SECONDS is the &8 argument;
 *   - a barrel roll is one whole sign cycle of right.y while airborne. */
static void crash_track(const B3HudCrashIn *in, float dt) {
    int kind;
    if (in->hit_wall && !g_crash.filed_wall) {
        crash_push(B3_HUD_CD_INTO_WALL, 0, NULL, 0, 0.f);
        g_crash.filed_wall = 1;
    }
    if (in->airborne) {
        g_crash.air_s += dt;
        /* A barrel roll is a whole 360 deg turn about the car's FORWARD
         * axis, so measure the angle and count complete turns.  The old
         * test counted a roll on every sign change of right.y -- but an
         * upright car's right vector is horizontal, i.e. right.y sits AT
         * zero, so ordinary wreck jitter crossed it repeatedly and filed
         * "Barrel Roll" while the car was visibly not rolling (user
         * report).  atan2(right.y, up.y) is that roll angle; unwrapped and
         * accumulated it only reaches 2*pi on a real revolution, and
         * oscillation about level nets out to ~0.  Still GLUE: retail
         * files this descriptor from per-panel contact bookkeeping that is
         * not recovered -- only the FORMAT and its argument kind are. */
        {
            float roll = atan2f(in->right_y, in->up_y);
            if (!g_crash.roll_init) {
                g_crash.prev_roll = roll;
                g_crash.roll_init = 1;
            }
            float d = roll - g_crash.prev_roll;
            while (d >  3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            g_crash.roll_acc += d;
            g_crash.prev_roll = roll;
            int turns = (int)(fabsf(g_crash.roll_acc) / 6.28318531f);
            if (turns > g_crash.rolls) {
                if (getenv("B3_ROLL_TRACE"))
                    printf("[roll] turn %d filed  (acc %.2f rad, right_y %.3f "
                           "up_y %.3f)\n", turns, g_crash.roll_acc,
                           in->right_y, in->up_y);
                g_crash.rolls = turns;
                crash_push(B3_HUD_CD_BARREL_ROLL, 0, NULL, g_crash.rolls, 0.f);
            }
        }
        g_crash.prev_right_y = in->right_y;
        g_crash.skid_m = 0.f;
        return;
    }
    if (g_crash.air_s > 0.6f) {                  /* landed: file the air */
        crash_push(B3_HUD_CD_AIR, 0, NULL, 0, g_crash.air_s);
    }
    g_crash.air_s = 0.f;
    if (in->up_y < -0.4f)               kind = B3_HUD_CD_SUNROOF_SKID;
    else if (in->right_y >  0.55f ||
             in->right_y < -0.55f)      kind = B3_HUD_CD_SIDE_PANEL_SKID;
    else if (in->fwd_y   >  0.55f)      kind = B3_HUD_CD_TAIL_GRIND;
    else if (in->fwd_y   < -0.55f)      kind = B3_HUD_CD_NOSE_GRIND;
    else                                kind = B3_HUD_CD_SKID;
    if (kind != g_crash.skid_kind) {
        if (g_crash.skid_m > 4.f)            /* > ~13 ft is worth a row  */
            crash_push(g_crash.skid_kind, 0, NULL, 0, g_crash.skid_m);
        g_crash.skid_kind = kind;
        g_crash.skid_m = 0.f;
    }
    if (in->speed_ms > 1.0f) g_crash.skid_m += in->speed_ms * dt;
}

static void crash_update(const B3HudCrashIn *in, float dt) {
    if (!in || !in->active) {
        if (g_crash.active) b3_hud_crash_reset();
        return;
    }
    if (!g_crash.active) {
        b3_hud_crash_reset();
        g_crash.active = 1;
        g_crash.skid_kind = -1;
        /* the opening descriptor is what was hit -- FUN_0017A6B0's job */
        crash_push(in->hit_kind ? B3_HUD_CD_INTO_VEHICLE
                                : B3_HUD_CD_INTO_WALL,
                   in->hit_kind, in->hit_id, 0, 0.f);
        if (!in->hit_kind) g_crash.filed_wall = 1;
    }
    g_crash.age += dt;
    crash_track(in, dt);
    if (in->speed_ms < 0.8f && g_crash.skid_m > 4.f) {   /* came to rest */
        crash_push(g_crash.skid_kind, 0, NULL, 0, g_crash.skid_m);
        g_crash.skid_m = 0.f;
        g_crash.skid_kind = -1;
    }
}

/* ---- the band's look ------------------------------------------------ *
 * GLUE / [S-ref]: the band is the crash element's own draw callback
 * (0x0004E450), whose box was not recovered.  The two runs, their
 * colours and their baseline are measured off the reference frame
 * xemu-2026-08-13-14-27-52.png in the 640x480 virtual space: the newest
 * descriptor white and italic with its ink right edge at x ~ 610 over
 * the cap band y 386..405, the accumulated line light-blue italic one
 * step smaller, immediately to its left with an 8 px gap.               */
#define B3_CRASH_BAND_RIGHT   610.0f   /* [S-ref] ink right edge          */
#define B3_CRASH_BAND_Y       380.0f   /* [S-ref] the reference frame's
                                       * white run inks y 384..402, i.e.
                                       * hard against the bottom blade  */
#define B3_CRASH_NEW_SCALE      0.86f  /* [S-ref] the white run           */
#define B3_CRASH_OLD_SCALE      0.62f  /* [S-ref] the blue run            */
#define B3_CRASH_GAP            8.0f   /* [S-ref]                         */

static void elem_crash_ticker(void) {
    const char *newest = NULL;
    const char *line;
    B3TextStyle sn, so;
    float wn, wo, x;

    line = b3_hud_crash_ticker(&newest);
    if (!newest || !g_crash.active) return;

    sn = STYLE_GOLD_BIG;                       /* the newest: loud white */
    sn.scale = B3_CRASH_NEW_SCALE;
    sn.top[0] = sn.top[1] = sn.top[2] = 1.f;
    sn.bot[0] = sn.bot[1] = sn.bot[2] = 1.f;
    sn.top[3] = sn.bot[3] = 1.f;
    sn.shadow_a = B3HUD_SHADOW_A_MED;
    wn = text_width(&b3_font_globalfont, newest, sn.scale);
    x = B3_CRASH_BAND_RIGHT - wn;
    draw_text(&b3_font_globalfont, g_font_global, newest, x,
              B3_CRASH_BAND_Y, &sn);

    if (line && line[0]) {                     /* the history: HUD blue  */
        so = STYLE_WHITE_LABEL;
        so.scale = B3_CRASH_OLD_SCALE;
        so.shadow_a = B3HUD_TICK_SHADOW_A;
        wo = text_width(&b3_font_globalfont, line, so.scale);
        draw_text(&b3_font_globalfont, g_font_global, line,
                  x - B3_CRASH_GAP - wo,
                  B3_CRASH_BAND_Y + b3_font_globalfont.line_h
                                    * (sn.scale - so.scale) * 0.62f,
                  &so);
    }
}

/* ===================================================================== *
 *  THE CINEMATIC BLADES  --  the crash presentation's letterbox bars
 *  (crash-cinema wave, 2026-08-13).  RECOVERED, not eyeballed.
 *
 *  They are ONE HUD element, instance 0x003FF2C8, ctor FUN_000509B0,
 *  builder FUN_00050A70(elem, mode=2), instantiated by FUN_00053300 at
 *  0x0005332F / 0x00053372 for every racing game mode.  Five nodes:
 *
 *    +0x20  TOP BLADE     (0,   0) 640 x 60, rgba (0,0,0,1), draw
 *                         FUN_001C1930 -> an UNTEXTURED flat quad
 *                         built @0x00050B8B..0x00050C48
 *    +0x24  TOP RULE      (0,  60) 640 x  2, anchor (0,0)
 *                         built @0x00050CEA..0x00050D87
 *    +0x28  BOTTOM BLADE  (0, 420) 640 x 60, rgba (0,0,0,1)
 *                         built @0x00050C4B..0x00050CE7
 *    +0x2C  BOTTOM RULE   (0, 480) 640 x  2, anchor (0,1) -> y 418..420
 *                         built @0x00050D8C..0x00050E34
 *    +0x34  a right/bottom-anchored caption at (610, 404), line height 36
 *
 *  Geometry, all from the static-init thunks [C]:
 *    60.0  = 480 * 0.125   (0x003B1728)   thunk 0x00266440 / 0x002664B0
 *   420.0  = 480 * 0.875   (0x0039922C)   thunk 0x00266480
 *   640.0  = 0x003B1F00                   thunk 0x00266430 / 0x002664A0
 *     2.0  = 0x003FD174 (a static with no writer -- the rule height)
 *  The x origins 0x004A2570 / 0x004A2580 and the top y 0x004A2574 are BSS
 *  with no writer anywhere in the image, so they are 0.0.        [C/S]
 *
 *  THE RULES are gradient nodes (FUN_000B52A0, draw LAB_000B53F0): two
 *  quads split at the horizontal midpoint whose per-vertex colours run
 *  colour1 at both screen edges to colour0 at the centre, i.e. a
 *  centre-weighted glow line, not a flat rule.
 *    colour0 = (0.47, 0.72, 1.0, 1.0)   0x003B20B4 / 0x003A2D7C / 0x003B168C
 *    colour1 = (0, 0, 0, 1.0)
 *
 *  VISIBILITY [C]: elem+0x14 is the element's state mask and the blades'
 *  is 6 (MOV EAX, 6 @0x00053323 / 0x00053366).  FUN_00053A10 tests it
 *  against DAT_00388F78[state] = {0,1,2,4,8,0xF}, so the blades live in
 *  HUD state 2 (mask 2) and state 3 (mask 4).  State 2 is set by the
 *  ENTER-CRASHED handler at 0x0018C7EB -- the same function that writes
 *  car+0x18FA = 1 -- and state 3 by the takedown-replay camera
 *  (0x000279AF).  EVERY racing element (boost bar, speedo, POS, LAP, the
 *  event ticker, the EA TRAX banner) is registered with mask 1, so they
 *  all vanish in state 2; the callout/sign element 0x003FE268 has mask 7
 *  and stays, which is why CRASHED! + hud_sign_skull are still drawn.
 *
 *  ANIMATION [C]: FUN_00050F60 slides the blades off screen by exactly
 *  their own height over t = 0..1 and FUN_00053970 runs t at 2.5/s from a
 *  timer FUN_00053D20 sets to 0.4 s (0x003B16E8) -- EXCEPT that any
 *  transition whose new or old state is 2 gets timer = 0
 *  (0x00053D5F..0x00053D85).  A race crash is exactly 1 -> 2, so the
 *  blades SNAP in and out; only the takedown replay (state 3) slides.
 *
 *  COMPOSITION: retail's HUD virtual space is 640x480 but it is composed
 *  into the 640x448-in-640x480 letterbox (RE_FRONTEND 6, screen_y =
 *  16 + virt_y * 448/480), so on screen the top blade's black runs 0..71
 *  with its rule at 72..73 and the bottom rule sits at 406..407 with
 *  black from 408 -- which is precisely what the retail reference frame
 *  xemu-2026-08-13-14-27-52.png measures (black 0..73 and 406..479, the
 *  2 px blue gradient rules on rows 72..73 and 406..407, peaking at
 *  (123,186,255) = (0.482,0.729,1.0), the recovered colour0).  THIS
 *  module's virtual space IS the screen (see vtx()), so the constants
 *  below are the composed ones.                     [C + S-ref for the 16]
 * ===================================================================== */
#define B3_BLADE_VIRT_FRAC   0.125f  /* @0x003B1728, 60/480               */
#define B3_BLADE_RULE_VIRT_H  2.0f   /* @0x003FD174                       */
#define B3_BLADE_LETTERBOX   16.0f   /* [S-ref] the 640x448 inset         */
#define B3_BLADE_COMPOSE   (448.0f / 480.0f)
#define B3_BLADE_RULE_H  (B3_BLADE_RULE_VIRT_H * B3_BLADE_COMPOSE)
#define B3_BLADE_H       (B3_BLADE_LETTERBOX \
                          + B3HUD_VIRT_H * B3_BLADE_VIRT_FRAC \
                            * B3_BLADE_COMPOSE)
#define B3_BLADE_RULE_R      0.47f   /* @0x003B20B4                       */
#define B3_BLADE_RULE_G      0.72f   /* @0x003A2D7C                       */
#define B3_BLADE_RULE_B      1.00f   /* @0x003B168C                       */
/* 0x00053D5F: any transition touching state 2 runs with timer 0, so a
 * race crash snaps.  The 0.4 s / 2.5-per-second slide (0x003B16E8 /
 * 0x003A2D50) is the takedown-replay path and is kept for reference. */
#define B3_BLADE_SLIDE_S     0.4f    /* @0x003B16E8                       */

static float g_blade_t;              /* 0 .. 1, how far the blades are in */

/* One rule: LAB_000B53F0's two quads, split at the horizontal midpoint,
 * colour1 (black) at both screen edges and colour0 at the centre. */
static void blade_rule(float y, float a) {
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    for (int i = 0; i < 2; i++) {
        float x0 = i ? B3HUD_VIRT_W * 0.5f : 0.f;
        float x1 = i ? B3HUD_VIRT_W        : B3HUD_VIRT_W * 0.5f;
        float k0 = i ? 1.f : 0.f, k1 = i ? 0.f : 1.f;
        glColor4f(B3_BLADE_RULE_R * k0, B3_BLADE_RULE_G * k0,
                  B3_BLADE_RULE_B * k0, a); vtx(x0, y);
        glColor4f(B3_BLADE_RULE_R * k1, B3_BLADE_RULE_G * k1,
                  B3_BLADE_RULE_B * k1, a); vtx(x1, y);
        glColor4f(B3_BLADE_RULE_R * k1, B3_BLADE_RULE_G * k1,
                  B3_BLADE_RULE_B * k1, a); vtx(x1, y + B3_BLADE_RULE_H);
        glColor4f(B3_BLADE_RULE_R * k0, B3_BLADE_RULE_G * k0,
                  B3_BLADE_RULE_B * k0, a); vtx(x0, y + B3_BLADE_RULE_H);
    }
    glEnd();
    glColor4f(1.f, 1.f, 1.f, 1.f);
}

/* FUN_001C1930's flat untextured quad. */
static void blade_bar(float y, float h) {
    if (h <= 0.f) return;
    glDisable(GL_TEXTURE_2D);
    glColor4f(0.f, 0.f, 0.f, 1.f);
    glBegin(GL_QUADS);
    vtx(0.f, y);              vtx(B3HUD_VIRT_W, y);
    vtx(B3HUD_VIRT_W, y + h); vtx(0.f, y + h);
    glEnd();
    glColor4f(1.f, 1.f, 1.f, 1.f);
}

static void elem_crash_blades(const B3HudCrashIn *in, float dt) {
    float h, black;
    (void)dt;
    /* the crash transition is 1 <-> 2, and FUN_00053D20 @0x00053D5F gives
     * every state-2 transition timer 0 -- so it is a SNAP, not a slide. */
    g_blade_t = (in && in->active) ? 1.f : 0.f;
    if (g_blade_t <= 0.f) return;
    /* The rules sit on the blades' INNER EDGE, outside the black: the top
     * blade is virtual y 0..60 and its rule virtual y 60..62, the bottom
     * blade virtual 420..480 and its rule virtual 418..420. */
    h = B3_BLADE_H * g_blade_t;
    black = h;
    blade_bar(0.f, black);
    blade_rule(black, g_blade_t);
    blade_bar(B3HUD_VIRT_H - black, black);
    blade_rule(B3HUD_VIRT_H - black - B3_BLADE_RULE_H, g_blade_t);
}

/* How far the blades have come in.  Retail expresses this as HUD state 2:
 * every racing element carries mask 1 and is switched off there. */
static int crash_cinema_on(void) { return g_blade_t > 0.f; }

/* ===================================================================== *
 *  THE AFTERTOUCH ARROW CURSOR -- the port of FUN_0004FCA0
 *  (crash-cinema wave, 2026-08-13).
 *
 *  The callback owns a 54x36 box (node+0x08/+0x0C) and draws FOUR wedges
 *  radiating from the point (0.5, 0.45) of that box.  Recovered pieces:
 *
 *  GEOMETRY [C]  seven unit vertices @0x003FCF38..0x003FCF6C, each scaled
 *    by (node.w, node.h) at 0x0004FDFF..0x0004FF93:
 *      0 (0.00, 0.00)  1 (0.50, 0.00)  2 (1.00, 0.00)
 *      3 (0.50, 0.45)  <- the centre
 *      4 (0.00, 1.00)  5 (0.50, 1.00)  6 (1.00, 1.00)
 *    and the primitive table @0x00388928, five bytes per entry
 *    { count, i0, i1, i2, i3 } walked count..1 (0x00050031):
 *      UP    4: 2,3,1,0      (the top band)
 *      DOWN  4: 6,3,5,4      (the bottom band)
 *      LEFT  3: 4,3,0        (the left triangle)
 *      RIGHT 3: 6,3,2        (the right triangle)
 *    The loop advances the table pointer by 5 and the alpha pointer by 4
 *    until it reaches 0x0038893C (0x00050101..0x0005012A).
 *
 *  MAGNITUDES [C]  0x0004FD0F..0x0004FDD0, from the two aftertouch axes
 *    the crashed input stage FUN_00118410 publishes:
 *      UP    = max(0,  veh+0x140C)    DOWN  = max(0, -veh+0x140C)
 *      LEFT  = max(0, -veh+0x1408)    RIGHT = max(0,  veh+0x1408)
 *
 *  PULSE + ALPHA [C]  0x0004FCB4 builds  p = 1 - frac(clock * 2)^2  and
 *    0x0005006D..0x0005008D turns each magnitude into
 *      a = mag * p ;  alpha = 2a - a^2   (an ease-out)
 *    then lerps the wedge colour from DIM (0x003FCF70, a desaturated
 *    blue-grey) to the node colour and draws.  A SECOND pass
 *    (0x00050130..0x001DF) re-draws every wedge whose alpha > 0.01 with
 *    the U coordinate mirrored (u := 1 - u), colour 0x003FCF80 and
 *    alpha^2 -- the gloss highlight.
 *
 *  UVs [S]  the callback reads them from a RUNTIME table at 0x0054F680
 *    that FUN_00265D10 fills from four ValueDB scalars (0x0054F664 /
 *    0x0054F678 / 0x0054F6D4 / 0x0054F6E0), none of which are in the
 *    image.  Its SHAPE is recovered: u takes one value at the box's
 *    x = 0 and x = 1 and the other at x = 0.5, v runs top -> centre ->
 *    bottom -- i.e. the sprite is mirrored about the box's vertical
 *    centreline.  This port instead maps the whole "Aftertouch" sprite
 *    into each wedge with a per-direction 90-degree UV rotation, which is
 *    what makes the sprite's arrowhead point outward in every wedge.
 * ===================================================================== */

/* the seven unit vertices, @0x003FCF38 */
static const float B3_AT_V[7][2] = {
    { B3HUD_AT_VX_TL,    B3HUD_AT_VY_TOP    },   /* 0 */
    { B3HUD_AT_VX_MID,   B3HUD_AT_VY_TOP    },   /* 1 */
    { B3HUD_AT_VX_RIGHT, B3HUD_AT_VY_TOP    },   /* 2 */
    { B3HUD_AT_CENTRE_X, B3HUD_AT_CENTRE_Y  },   /* 3 */
    { B3HUD_AT_VX_TL,    B3HUD_AT_VY_BOTTOM },   /* 4 */
    { B3HUD_AT_VX_MID,   B3HUD_AT_VY_BOTTOM },   /* 5 */
    { B3HUD_AT_VX_RIGHT, B3HUD_AT_VY_BOTTOM },   /* 6 */
};

/* the primitive table @0x00388928, already walked in the retail order
 * (count..1) so the winding matches. */
static const unsigned char B3_AT_PRIM[4][5] = {
    { 4, 2, 3, 1, 0 },   /* UP    */
    { 4, 6, 3, 5, 4 },   /* DOWN  */
    { 3, 4, 3, 0, 0 },   /* LEFT  */
    { 3, 6, 3, 2, 0 },   /* RIGHT */
};

/* The RECOVERED UV shape (see the note above): u takes one value at the
 * box's x = 0 and x = 1 and the other at x = 0.5, v runs top -> centre ->
 * bottom -- the sprite (the right half of the badge, an arrowhead swoosh
 * with its point at +u) is MIRRORED about the box's vertical centreline,
 * composing the closed gold "eye" the retail reference shows (user image
 * xemu-2026-08-13-14-27-52 top right). The exact ValueDB endpoints are
 * not in the image; 0/1 endpoints fitted to the reference ([S], look-
 * authorized). The old per-wedge 90-degree rotation drew four rotated
 * arrowheads instead of the eye -- user report. */
static float g_at_clock;                 /* stands in for [0x004AE200] */

static void at_uv(int rot, float x, float y, float *u, float *v) {
    (void)rot;
    *u = fabsf(x - 0.5f) * 2.0f;   /* 1 at both edges, 0 at the centreline */
    *v = y;                        /* top -> centre -> bottom */
}

static void at_wedge(GLuint tex, int prim, float x, float y, float w, float h,
                     const float col[4], float alpha, int mirror_u) {
    const unsigned char *p = B3_AT_PRIM[prim];
    int n = p[0], i;
    if (alpha <= 0.f) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor4f(col[0], col[1], col[2], col[3] * alpha);
    glBegin(GL_TRIANGLE_FAN);
    for (i = 1; i <= n; i++) {
        const float *vv = B3_AT_V[p[i]];
        float u, t;
        at_uv(prim, vv[0], vv[1], &u, &t);
        if (mirror_u) u = 1.f - u;         /* pass 2, 0x00050180 */
        glTexCoord2f(u, t);
        vtx(x + vv[0] * w, y + vv[1] * h);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

/* Draw the cursor into the recovered 54x36 box at (bx, by). */
static void elem_aftertouch_arrow(float bx, float by, float h, float v) {
    static const float DIM[4]   = { B3HUD_AT_DIM_R, B3HUD_AT_DIM_G,
                                    B3HUD_AT_DIM_B, B3HUD_AT_DIM_A };
    static const float GLOSS[4] = { B3HUD_AT_GLOSS_R, B3HUD_AT_GLOSS_G,
                                    B3HUD_AT_GLOSS_B, B3HUD_AT_GLOSS_A };
    float mag[4], al[4], frac, pulse;
    int i;

    if (!g_aftertouch) return;

    /* 0x0004FCB4: p = 1 - frac(clock * PULSE_RATE)^2 */
    frac = g_at_clock * B3HUD_AT_PULSE_RATE;
    frac -= (float)(int)frac;
    pulse = B3HUD_AT_PULSE_ONE - frac * frac;

    /* 0x0004FD0F..0x0004FDD0, in the table's order */
    mag[0] = v >  0.f ?  v : 0.f;    /* UP    */
    mag[1] = v <  0.f ? -v : 0.f;    /* DOWN  */
    mag[2] = h <  0.f ? -h : 0.f;    /* LEFT  */
    mag[3] = h >  0.f ?  h : 0.f;    /* RIGHT */

    for (i = 0; i < 4; i++) {        /* 0x0005006D: alpha = 2a - a^2 */
        float a = mag[i] * pulse;
        if (a < 0.f) a = 0.f;
        if (a > 1.f) a = 1.f;
        al[i] = 2.f * a - a * a;
    }

    /* pass 1 (0x0005009F): colour = lerp(DIM, node colour, alpha).  The
     * node colour is the element's own (1,1,1,1) @0x0005155F, and DIM's
     * own alpha is 1.0, so the lerp only moves the RGB: every wedge is
     * always drawn, the idle ones in the blue-grey, the pressed one
     * white.  [0x0054FA50 is the engine's white colour vector -- BSS, so
     * its value is S; the lerp itself is C.] */
    for (i = 0; i < 4; i++) {
        float c[4];
        int k;
        for (k = 0; k < 4; k++) c[k] = DIM[k] + (1.f - DIM[k]) * al[i];
        at_wedge(g_aftertouch, i, bx, by,
                 B3HUD_IMPACT_BOX_W, B3HUD_IMPACT_BOX_H, c, 1.f, 0);
    }

    /* pass 2 (0x00050130): the gloss highlight, additive, alpha^2 */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    for (i = 0; i < 4; i++) {
        if (al[i] <= B3HUD_AT_ALPHA_MIN) continue;   /* 0x00050168 */
        at_wedge(g_aftertouch, i, bx, by,
                 B3HUD_IMPACT_BOX_W, B3HUD_IMPACT_BOX_H, GLOSS,
                 al[i] * al[i], 1);
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

/* ---- "(A) IMPACT TIME" ---------------------------------------------- *
 * The recovered geometry is RELATIVE: the run is right-aligned in a 124
 * px box (anchor {1.0, 0.5}) and the glyph is a 30x30 sprite centred at
 * (text ink left - 4 - 15) on the same ref y.  The element's anchor is
 * the top-right slot's viewport point, which is runtime data, so the
 * absolute placement uses the same title-safe rule as every other
 * element plus the reference frame's band centre.  [C relative, S-ref abs] */
/* [S-ref] the retail frame's "IMPACT TIME" ink is x 461..609, y 44..62,
 * i.e. it sits ON the top blade with its centre line at y = 53. */
#define B3_IMPACT_CY  53.0f

static void elem_impact_time(const B3HudCrashIn *in) {
    B3TextStyle st;
    const char *s = "IMPACT TIME";     /* Globalus 2191                  */
    float w, x, gx;

    if (!in || !in->active || !in->aftertouch) return;

    st = STYLE_WHITE_LABEL;
    st.scale = 0.86f;                  /* [S-ref] fits the 124 px box    */
    st.top[0] = st.top[1] = st.top[2] = 1.f;
    st.bot[0] = st.bot[1] = st.bot[2] = 1.f;
    st.shadow_a = B3HUD_SHADOW_A_MED;

    w = text_width(&b3_font_globalfont, s, st.scale);
    if (w > B3HUD_IMPACT_TEXT_W) {     /* the box the builder is given   */
        st.scale *= B3HUD_IMPACT_TEXT_W / w;
        w = B3HUD_IMPACT_TEXT_W;
    }
    x = B3HUD_VIRT_W - B3HUD_VP_X0 - w;                 /* anchor {1,.5} */
    draw_text(&b3_font_globalfont, g_font_global, s, x,
              B3_IMPACT_CY - b3_font_globalfont.line_h * st.scale * 0.5f,
              &st);

    /* FUN_00051230 @0x00051412 chooses between the two shapes on the raw
     * pad+0x84 value -- the BOOST button -- not on the steer axes:
     *   pad+0x84 == 0  ->  the 30x30 (A) glyph          @0x00051451
     *   pad+0x84 != 0  ->  the 54x36 arrow-cursor box   @0x000514DD
     * `input` is kept as the legacy trigger so an untouched caller still
     * swaps in the arrow when it reports steering.                   [C] */
    if (in->impact_held || in->input != 0.f) {
        elem_aftertouch_arrow(x - B3HUD_IMPACT_GLYPH_GAP - B3HUD_IMPACT_BOX_W,
                              B3_IMPACT_CY - B3HUD_IMPACT_BOX_H * 0.5f,
                              in->steer_h, in->steer_v);
        return;
    }
    if (!g_abutton) return;
    gx = x - B3HUD_IMPACT_GLYPH_GAP - B3HUD_IMPACT_GLYPH_BACK;
    tex_quad_px(g_abutton,
                gx - B3HUD_IMPACT_GLYPH_W * 0.5f,
                B3_IMPACT_CY - B3HUD_IMPACT_GLYPH_H * 0.5f,
                B3HUD_IMPACT_GLYPH_W, B3HUD_IMPACT_GLYPH_H,
                0.f, 0.f, 1.f, 1.f, 1.f);
}

/* [S-ref] the crash cinema's top-blade layout, measured off
 * xemu-2026-08-13-14-27-52.png. */
#define B3_CRASHED_SIGN      62.0f
#define B3_CRASHED_SIGN_CX   72.0f
#define B3_CRASHED_SIGN_CY   62.0f
#define B3_CRASHED_TEXT_X    96.0f
#define B3_CRASHED_TEXT_CY   55.0f
#define B3_CRASHED_SCALE      1.35f

/* Generic callout renderer.  Takes (label, art, timer) so the boost-earn
 * tiers and the takedown-FX module's callout state machine share it. */
static void draw_callout(const B3HudCallout *c, float cy) {
    if (!c || !c->label || c->t < 0.f) return;
    float life = c->life > 0.f ? c->life : 1.5f;
    if (c->t > life) return;
    float a = 1.f;
    float fade = life * 0.33f;
    if (c->t > life - fade) a = (life - c->t) / fade;
    if (a < 0.f) a = 0.f;
    /* small pop-in on the first 0.12 s */
    float pop = c->t < 0.12f ? (0.75f + 0.25f * (c->t / 0.12f)) : 1.f;

    B3TextStyle st = STYLE_GOLD_BIG;
    st.scale = 1.55f * pop;
    st.top[3] = st.bot[3] = a;
    st.outline[3] = 0.95f * a;
    float w = text_width(&b3_font_globalfont, c->label, st.scale);
    float cx = B3HUD_VIRT_W * 0.5f - w * 0.5f;

    /* CRASH CINEMA: while the blades are in, the callout is not a centred
     * banner -- the retail reference frame puts the skull sign and the
     * "CRASHED!" run side by side on the TOP BLADE, left-aligned:
     *   sign ink   x  47..96,  y 31..93 (it overhangs the blade's edge)
     *   text ink   x  96..270, y 39..70
     * so the sign is a ~62 px square centred at (72, 62) and the run's
     * left edge is x = 96 with its cap band centred on y = 55.   [S-ref] */
    if (crash_cinema_on()) {
        float sw = B3_CRASHED_SIGN * pop, sh = sw;
        st.scale = B3_CRASHED_SCALE * pop;
        w = text_width(&b3_font_globalfont, c->label, st.scale);
        if (c->art)
            tex_quad_px(c->art, B3_CRASHED_SIGN_CX - sw * 0.5f,
                        B3_CRASHED_SIGN_CY - sh * 0.5f, sw, sh,
                        0.f, 0.f, 1.f, 1.f, a);
        draw_text(&b3_font_globalfont, g_font_global, c->label,
                  B3_CRASHED_TEXT_X,
                  B3_CRASHED_TEXT_CY
                      - b3_font_globalfont.line_h * st.scale * 0.5f, &st);
        return;
    }

    if (c->art) {
        float aw = 96.f * pop, ah = 96.f * pop;
        tex_quad_px(c->art, B3HUD_VIRT_W * 0.5f - aw * 0.5f, cy - ah - 6.f,
                    aw, ah, 0.f, 0.f, 1.f, 1.f, a);
    }
    draw_text(&b3_font_globalfont, g_font_global, c->label, cx, cy, &st);
}

/* Position-change transient ("4th"), [S-ref]. */
static void elem_pos_callout(int position) {
    if (g_pos_callout_t > 1.5f) return;
    float a = 1.f;
    if (g_pos_callout_t > 1.0f) a = 1.f - (g_pos_callout_t - 1.0f) / 0.5f;
    B3TextStyle st = STYLE_GOLD_BIG;
    st.scale = 2.4f;
    st.top[3] = st.bot[3] = a;
    st.outline[3] = 0.95f * a;
    const char *s = ordinal(position);
    float w = text_width(&b3_font_globalfont, s, st.scale);
    draw_text(&b3_font_globalfont, g_font_global, s,
              500.f - w * 0.5f, 205.f, &st);
}

/* ---- public API ------------------------------------------------------- */

const char *b3_hud_callout_text(int cat, int tier) {
    if (cat < 0 || cat >= B3HUD_CALLOUT_CATS) return NULL;
    if (tier < 0 || tier >= B3HUD_CALLOUT_TIERS) return NULL;
    return B3_CALLOUT[cat][tier];
}

const char *b3_hud_tick_label(int row) {
    if (row < 0 || row >= B3_HUD_TICK_ROWS) return NULL;
    return B3_TICK[row].label;
}

void b3_hud_boost_event(int cat, int tier) {
    const char *s = b3_hud_callout_text(cat, tier);
    if (!s) return;
    g_earn_callout.label = s;
    g_earn_callout.art   = 0;
    g_earn_callout.t     = 0.f;
    g_earn_callout.life  = 1.6f;
    g_earn_callout.id    = cat * B3HUD_CALLOUT_TIERS + tier;
}

/* Pause-screen mixer (harness UI, GLUE by design -- no retail
 * counterpart; drawn with the HUD's own font/state so it matches the
 * game's look). Coordinates shared with the harness hit-testing via the
 * B3HUD_MIX_* defines. */
static void solid_px(float x, float y, float w, float h,
                     float r, float g, float b, float a) {
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    vtx(x, y); vtx(x + w, y); vtx(x + w, y + h); vtx(x, y + h);
    glEnd();
}

void b3_hud_pause_mixer(const float vals[3]) {
    static const char* names[3] = { "ENGINE", "SFX", "MUSIC" };
    state_begin();
    solid_px(0.f, 0.f, B3HUD_VIRT_W, B3HUD_VIRT_H,
             0.02f, 0.04f, 0.10f, 0.62f);
    B3TextStyle ts = STYLE_GOLD_BIG;
    ts.scale = 1.15f;
    draw_text(&b3_font_globalfont, g_font_global, "PAUSED",
              B3HUD_MIX_X, B3HUD_MIX_Y0 - 70.f, &ts);
    for (int i = 0; i < 3; i++) {
        float y = B3HUD_MIX_Y0 + i * B3HUD_MIX_DY;
        B3TextStyle ls = STYLE_WHITE_LABEL;
        draw_text(&b3_font_globalfont, g_font_global, names[i],
                  B3HUD_MIX_X, y - 3.f, &ls);
        solid_px(B3HUD_MIX_BAR_X - 2.f, y - 2.f, B3HUD_MIX_W + 4.f,
                 B3HUD_MIX_H + 4.f, 0.05f, 0.08f, 0.16f, 0.90f);
        float f = vals[i] < 0.f ? 0.f : (vals[i] > 1.f ? 1.f : vals[i]);
        solid_px(B3HUD_MIX_BAR_X, y, B3HUD_MIX_W * f, B3HUD_MIX_H,
                 0.95f, 0.75f, 0.15f, 0.95f);
    }
    solid_px(B3HUD_MIX_BTN_X - 2.f, B3HUD_MIX_BTN_Y - 2.f,
             B3HUD_MIX_BTN_W + 4.f, B3HUD_MIX_BTN_H + 4.f,
             0.05f, 0.08f, 0.16f, 0.90f);
    solid_px(B3HUD_MIX_BTN_X, B3HUD_MIX_BTN_Y,
             B3HUD_MIX_BTN_W, B3HUD_MIX_BTN_H,
             0.16f, 0.28f, 0.55f, 0.92f);
    {
        B3TextStyle bs = STYLE_WHITE_LABEL;
        draw_text(&b3_font_globalfont, g_font_global, "RESTART RACE",
                  B3HUD_MIX_BTN_X + 18.f, B3HUD_MIX_BTN_Y + 4.f, &bs);
    }
    state_end();
}

void b3_hud_draw_quad(GLuint tex, float x, float y, float w, float h,
                      float alpha) {
    state_begin();
    float px = (x + 1.f) * (B3HUD_VIRT_W * 0.5f);
    float py = (1.f - (y + h)) * (B3HUD_VIRT_H * 0.5f);
    tex_quad_px(tex, px, py, w * (B3HUD_VIRT_W * 0.5f),
                h * (B3HUD_VIRT_H * 0.5f), 0.f, 0.f, 1.f, 1.f, alpha);
    state_end();
}

void b3_hud_draw_speedo(float mph, float x, float y, float scale) {
    (void)x; (void)y; (void)scale;
    state_begin(); elem_speed(mph); state_end();
}

void b3_hud_draw_boost_bar(float frac, float x, float y, float w, float h) {
    (void)x; (void)y; (void)w; (void)h;
    B3BoostHud b = g_boost;
    b.fill = frac; b.bar_frac = 1.f; b.shake = 0.f;
    b.segments = B3HUD_CALLOUT_TIERS;
    state_begin(); elem_boost(&b); state_end();
}

void b3_hud_draw_lap_position(int lap, int total_laps,
                              int position, int n_cars,
                              float x, float y, float scale) {
    (void)x; (void)y; (void)scale;
    state_begin();
    elem_position(position, n_cars);
    elem_lap(lap, total_laps);
    state_end();
}

/* B3_HUD_TEST=<1..4>: force one of the four boost-bar states so the bar
 * can be screenshotted headlessly without driving a whole race.  Debug
 * only; it never fires unless the env var is set.
 *   1 empty   2 filling (earn comet live)   3 full + fire   4 draining  */
static const B3HudState *hud_test_override(const B3HudState *st,
                                           B3HudState *tmp, float dt_s) {
    static int   mode = -1;
    static float earned = 0.f;
    /* CRASH-CINEMA capture aid: B3_HUD_AT="h,v" pins the crash prompt open
     * with the Impact Time button held and the two aftertouch axes at the
     * given values, so an offscreen capture can show the arrow cursor. */
    static int   at_mode = -1;
    static float at_h, at_v;
    if (at_mode < 0) {
        const char *e = getenv("B3_HUD_AT");
        at_mode = (e && sscanf(e, "%f,%f", &at_h, &at_v) == 2) ? 1 : 0;
    }
    if (mode < 0) {
        const char *e = getenv("B3_HUD_TEST");
        mode = e ? atoi(e) : 0;
        if (e && mode == 0) mode = 3;
    }
    if (at_mode) {
        *tmp = *st;
        tmp->crash.active      = 1;
        tmp->crash.aftertouch  = 1;
        tmp->crash.impact_held = 1;
        tmp->crash.steer_h     = at_h;
        tmp->crash.steer_v     = at_v;
        if (!mode) return tmp;
        st = tmp;
    }
    if (!mode) return st;
    *tmp = *st;
    tmp->boost.bar_size  = 720.f;
    tmp->boost.min_units = 0.f;
    tmp->boost.tier      = B3HUD_CALLOUT_TIERS - 1;
    switch (mode) {
    case 1: tmp->boost.meter = 0.f;          tmp->boost.boosting = 0; break;
    case 2: tmp->boost.meter = 720.f * 0.45f; tmp->boost.boosting = 0;
            earned += 90.f * dt_s;                                    break;
    case 3: tmp->boost.meter = 720.f;         tmp->boost.boosting = 1; break;
    default: tmp->boost.meter = 720.f * 0.62f; tmp->boost.boosting = 1; break;
    }
    tmp->boost.earned = earned;
    return tmp;
}

void b3_hud_draw_state(const B3HudState *st, float dt_s) {
    B3HudState tmp;
    if (!g_ready || !st) return;
    if (dt_s < 0.f) dt_s = 0.f;
    st = hud_test_override(st, &tmp, dt_s);

    boost_update(&g_boost, &st->boost, dt_s);
    /* the ticker belongs to the SAME element as the bar: FUN_0004D800
     * runs FUN_0004C390 then FUN_0004D310, in that order.          [C] */
    tick_update(st, dt_s);
    /* the crash show is its own element pair (FUN_00051230 for the
     * prompt, the crash band for the ticker); all-zero input = inert */
    crash_update(&st->crash, dt_s);
    g_at_clock += dt_s;              /* stands in for [0x004AE200] */

    if (st->position != g_last_pos) {
        if (g_last_pos > 0) g_pos_callout_t = 0.f;
        g_last_pos = st->position;
    }
    g_pos_callout_t += dt_s;
    if (g_earn_callout.label) {
        g_earn_callout.t += dt_s;
        if (g_earn_callout.t > g_earn_callout.life) g_earn_callout.label = NULL;
    }

    state_begin();
    /* CRASH CINEMA: the blades come in and the racing HUD goes away.  The
     * retail reference frame shows no POS/LAP plate, no speedo, no boost
     * bar, no event ticker and no EA TRAX banner while the crash
     * presentation runs -- only CRASHED! + skull, (A) IMPACT TIME and the
     * crash ticker.  [S-ref] */
    elem_crash_blades(&st->crash, dt_s);
    if (!crash_cinema_on()) {
        elem_position(st->position, st->n_cars);
        elem_lap(st->lap, st->total_laps);
        elem_speed(st->mph);
        elem_boost(&g_boost);
        elem_ticker();
        elem_music(&st->music);
    }
    elem_crash_ticker();
    elem_impact_time(&st->crash);
    /* the caller's callout (takedown FX) owns the loud channel; the
     * boost-earn tier callout sits under it. */
    /* Callout height: was 178 (mid-screen), which sat straight over the
     * road; moved up below the top chips at the user's request
     * (user-authorized placement deviation, 2026-08-13). */
    if (st->callout.label && st->callout.t >= 0.f)
        draw_callout(&st->callout, 112.f);
    else if (g_earn_callout.label && !crash_cinema_on())
        draw_callout(&g_earn_callout, 112.f);
    if (!crash_cinema_on())
        elem_pos_callout(st->position);
    state_end();
}

/* Back-compat shim for the existing burnout3_full.c call site.
 * boost_frac is interpreted as meter/bar_size at tier 3, and the earn
 * indicator is driven by the frac rising (the same 0.025/frame test the
 * game uses on its own target).  Real earn events need
 * b3_hud_draw_state(). */
void b3_hud_draw(float mph, float boost_frac, int lap, int total_laps,
                 int position, int n_cars, float dt_s) {
    static float s_earned = 0.f, s_prev = 0.f;
    static int   s_test = -1;
    if (s_test < 0) s_test = getenv("B3_HUD_TEST") != NULL;

    if (boost_frac < 0.f) boost_frac = 0.f;
    if (boost_frac > 1.f) boost_frac = 1.f;
    if (boost_frac > s_prev) s_earned += (boost_frac - s_prev) * 720.f;
    s_prev = boost_frac;

    B3HudState st;
    memset(&st, 0, sizeof(st));
    st.mph = mph;
    st.lap = lap; st.total_laps = total_laps;
    st.position = position; st.n_cars = n_cars;
    st.boost.tier      = B3HUD_CALLOUT_TIERS - 1;   /* full 4-segment bar */
    st.boost.bar_size  = 720.f;
    st.boost.meter     = boost_frac * 720.f;
    st.boost.earned    = s_earned;
    st.boost.min_units = 0.f;
    st.boost.boosting  = 0;
    st.callout.t       = -1.f;

    if (s_test) {
        /* B3_HUD_TEST=1: force a full-ish bar + a live earn + a callout */
        st.boost.meter  = 720.f * 0.65f;
        s_earned += 60.f * dt_s;
        st.boost.earned = s_earned;
        st.boost.boosting = 1;
        if (!g_earn_callout.label) b3_hud_boost_event(B3_HUD_CAT_NEARMISS, 3);
        g_pos_callout_t = 0.5f;
    }
    b3_hud_draw_state(&st, dt_s);
}

void b3_hud_shutdown(void) {
    GLuint all[9 + B3HUD_EDGE_FRAMES + B3HUD_CORE_FRAMES + B3HUD_OVER_FRAMES];
    int n = 0;
    if (g_font_global) all[n++] = g_font_global;
    if (g_panel)       all[n++] = g_panel;
    if (g_tread)       all[n++] = g_tread;
    if (g_earnflame)   all[n++] = g_earnflame;
    if (g_stars)       all[n++] = g_stars;
    if (g_eatrax)      all[n++] = g_eatrax;
    if (g_abutton)     all[n++] = g_abutton;
    if (g_aftertouch)  all[n++] = g_aftertouch;
    for (int i = 0; i < B3HUD_EDGE_FRAMES; i++) if (g_edge[i]) all[n++] = g_edge[i];
    for (int i = 0; i < B3HUD_CORE_FRAMES; i++) if (g_core[i]) all[n++] = g_core[i];
    for (int i = 0; i < B3HUD_OVER_FRAMES; i++) if (g_over[i]) all[n++] = g_over[i];
    if (n) glDeleteTextures(n, all);
    memset(g_edge, 0, sizeof(g_edge));
    memset(g_core, 0, sizeof(g_core));
    memset(g_over, 0, sizeof(g_over));
    g_font_global = g_panel = g_tread = g_earnflame = 0;
    g_stars = g_eatrax = g_abutton = g_aftertouch = 0;
    g_edge_n = g_core_n = g_over_n = 0;
    g_ready = 0;
    g_last_pos = -1;
    g_pos_callout_t = 99.f;
    memset(g_tick, 0, sizeof(g_tick));
    g_tick_n = 0;
    memset(&g_boost, 0, sizeof(g_boost));
    memset(&g_earn_callout, 0, sizeof(g_earn_callout));
}
