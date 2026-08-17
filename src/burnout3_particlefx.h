#ifndef BURNOUT3_PARTICLEFX_H
#define BURNOUT3_PARTICLEFX_H
/* THE PARTICLE FX ENGINE -- crash dust/smoke/debris, wall-grind sparks,
 * tyre smoke and the offroad dust roostertail.
 *
 * Evidence marks as elsewhere in this repo: [C] byte- or execution-
 * verified, [S] read from the disassembly, [?] open, GLUE = this port's
 * own bridging, TUNED = a deliberate, user-authorised look deviation.
 *
 * =====================================================================
 * 0. IT IS NOT THE SPRITE POOL
 * =====================================================================
 * docs/RE_BOOSTFX.md 3.1's pool table at 0x003A3E7C is EXACTLY three
 * records and stops: {0x003B0428 "coronaglow"}, {0x003B041C
 * "coronaboost"}, {0x003B040C "coronaboostred"} -- the dword after the
 * third is 0x40400000 (+3.0f), not a count, and FUN_0017EE00 is its only
 * walker with FUN_00187BE0 / FUN_001871E0 its only consumers.  There are
 * no dust/smoke/debris pools in it.                                  [C]
 *
 * The particle FX are a SEPARATE engine with three data tables of its
 * own:
 *
 *   0x004182A0  24 FX DESCRIPTORS, 0x80 bytes each.  Record 0 is in
 *               .data; records 1..23 are written at run time by the
 *               straight-line initialiser 0x0025EE20..0x002610FA (1249
 *               instructions, no branches -- Ghidra never made it a
 *               function).  Reconstructed by executing it.
 *   0x003A3648  26 EMITTER TYPES, 0x38 bytes each: {descriptor index,
 *               rate, ten random-range floats, two stream weights}.
 *   0x003A3BF8  40 SURFACE rows, 0x10 bytes each: the wheel-FX emitter
 *               for each material id (0x1A = none).  Self-validating:
 *               byte +0x0F equals the row index on all 40 rows.
 *
 * Live systems sit at world+0x90 + i*0x100 (stride [C] from 0x0017E9B8);
 * built by FUN_00035740, updated+drawn by FUN_00034130, spawned into by
 * FUN_00035C00, emitted by FUN_00181A80 (streak/distance),
 * FUN_001820D0 (point) and FUN_001824C0 (burst).                    [C]
 *
 * =====================================================================
 * 1. THE UPDATE LAW -- analytic, no per-frame integration           [C]
 * =====================================================================
 * FUN_00034130 @0x00034228..0x000342C0:
 *     age = now - birth;  dead if age <= 0 or age >= life
 *     pos = p0 + v*age + g*age*age        <-- g*t^2, NOT 0.5*g*t^2
 * (the tabulated gravity has already absorbed the half.)
 *
 * Colour and size are THREE keys with the split at `mid_t`:
 *     if (age >= mid_t*life) u = (age - mid_t*life)/(life - mid_t*life)
 *                            and the keys run 1 -> 2
 *     else                   u = age/(mid_t*life), keys 0 -> 1
 *     size = lerp(sizeKeys,u) * p.size_mul * 0.9   (@0x00034646/4B)
 * There is NO drag term: desc+0x48 (0.5 on every smoke, 0.9 on fire, 0
 * on the solids) looks exactly like a drag coefficient but has no
 * reader anywhere in the image.                                      [?]
 *
 * =====================================================================
 * 1a. THE SIZE LAW -- the vertex path, end to end                   [C]
 * =====================================================================
 * The particles are PRE-TRANSFORMED (D3DFVF_XYZRHW-style) screen-space
 * quads: FUN_00034130 transforms the centre with FUN_00013CA0 (the 4x4
 * at 0x004D6790), divides by w and scales by {W,H} = the viewport ints
 * cam+0x78/+0x7C (@0x0034780..0x000347BB), then expands the quad IN
 * PIXELS.  The pixel HALF-extent is
 *
 *     half_px = size * (1/w) * {W/(4*A), H/(4*B)}     @0x00034678..C9
 *
 * with A,B = cam+0x58 -> +0x68/+0x6C (published to 0x004D6B10/14 by
 * FUN_001AE6F0 @0x001AE769) -- the per-axis half-extent at unit depth,
 * i.e. tan(fov/2).  Substituting the same projection into the position
 * path collapses every projection term and leaves
 *
 *     WORLD HALF-EXTENT = size / 2   -- `size` is a DIAMETER in metres.
 *
 * So hypothesis "size is a radius" is FALSE.  What IS 2x wrong if you
 * read the code loosely are the two SCREEN limits, because they are
 * applied to that HALF-extent, not to the sprite's full width:
 *     @0x000346B9  if (half_px > max_px*0.435) half_px = max_px*0.435
 *     @0x000346F1  if (min_px  > half_px) cull
 * i.e. in full-width pixels the cap is max_px*0.87 and the cull is
 * 2*min_px.
 *
 * THE QUAD ITSELF depends on the descriptor's render KIND (desc+0x08,
 * the switch at 0x000348C7 over the jump table 0x00035728):
 *   kind 0 @0x000348CE  corners = (+-half_x, +-half_y), axis-aligned
 *                       -> textured square of side 2*half
 *   kind 1 @0x0003491A  corners = {ax, ay, -ax, -ay} with
 *                       ax = half*{-sin,cos}, ay = half*{cos,sin}
 *                       (FUN_00011570 is a NEGATE, FUN_000116A0 a copy)
 *                       -> a DIAMOND: the texture's own corners sit on
 *                       the axes, so the square's side is only
 *                       half*sqrt(2) -- kind 1 is 1/sqrt(2) SMALLER
 *   kind 3 @0x00034D89  same diamond but ax/ay carry the extra
 *   kind 4 @0x00034FE5  sqrt(2) at 0x003A34B8 -> side 2*half again,
 *                       i.e. exactly kind 0's size, just rotated
 * kinds 2 and 5 are the velocity-aligned / oriented-debris paths and
 * their expansion was not decoded                                    [?]
 *
 * There is also a per-system DISTANCE FADE the port does not have
 * (@0x00034707..0x00034786: fade = min((z-near)/nr, (far-z)/fr, 1) and
 * cull at 0.005) whose four inputs live at system+0xC0..0xCC, outside
 * the 0x80-byte descriptor -- not recovered.                         [?]
 *
 * Colours are packed 0xAABBGGRR -- RED IS THE LOW BYTE.  The initialiser
 * writes the bytes R,G,B,A in ascending address order at
 * 0x0025F492..0x0025F4BF; FUN_00035740 @0x000357BE unpacks them low
 * first into the system's float4 (+0x18->+0x40 ... +0x1B->+0x4C) and
 * FUN_00034130 @0x0003547E packs that float4 into the vertex D3DCOLOR
 * as alpha<<24 | c0<<16 | c1<<8 | c2, so c0 -- the record's LOW byte --
 * is RED.  The check that settles it is fxfire: 0xFF4080C0 is
 * (0xC0,0x80,0x40) orange, not (0x40,0x80,0xC0) blue.              [C]
 * The engine's byte*0.25 is undone by the combiner's x4 output scale,
 * so effective colour = byte/255.                                 [C/S]
 *
 * BLEND MODES, literal Xbox D3D8 tokens (FUN_00035D00) -- this also
 * closes an item RE_BOOSTFX 5 and RE_CARFX 6 both list as [?]:
 *     0 : SRC_ALPHA / INV_SRC_ALPHA, ADD          (plain alpha)
 *     1 : SRC_ALPHA / ONE,           REVSUBTRACT  (SUBTRACTIVE)
 *     2 : SRC_ALPHA / ONE,           ADD          (additive)
 * So the crash-trail smoke (descriptor 14) DARKENS the frame rather
 * than greying it -- the single most visually important recovered fact.
 *
 * =====================================================================
 * 2. THE EMISSION LAW -- distance-based with a dither carry         [C]
 * =====================================================================
 * FUN_00181A80:
 *     r     = dist * tbl.rate * intensity * (0.875 + 0.25*rand01)
 *     carry = (*carry_byte) / 256
 *     nA    = (int)(tbl.strm_a * r * 0.75 + carry)
 *     nB    = (int)(frac      + tbl.strm_b * r)
 *     *carry_byte = a fresh random byte
 * (0.875 @0x0039922C, 0.25 @0x003B1730, 1/256 @0x003B1B40, 0.75
 * @0x003A55F8.)  FUN_001824C0 is the same law WITHOUT tbl.rate -- the
 * caller supplies the count.
 *
 * =====================================================================
 * 3. THE DRIVERS
 * =====================================================================
 * THE CRASH SLIDE TRAIL -- FUN_00186D50, read straight off the
 * disassembly [C]:
 *     t = now - impact_time ;  return if t >= 5.0        (@0x003B1694)
 *     k = (t < 2.5) ? 1.0 : 1.0 - (t-2.5)*0.4*0.9     (0.4 @0x003B16E8,
 *                                                      0.9 @0x003A69C0)
 *     prev = pos - vel*dt ;  dist = speed*dt
 *     for i in 0..2: streak(id = [0x0041A514 + i*4] = {20,21,22},
 *                           prev, pos, vel, dist, k, k,
 *                           carry = [0x007547CC + i], force, two-stream)
 * i.e. fxexplosionsmoke(desc 14, SUBTRACTIVE) + fxfire(desc 19,
 * additive) + fxexplosionsmoke(desc 20) laid along the swept body
 * segment for 5 s, full strength for 2.5 s then linearly down to 0.1.
 *
 * THE WRECK PLUME -- FUN_00181610, emitter 11 -> descriptor 6: 14 s,
 * intensity 1-(t/14)^2, from model light type 10.                    [C]
 *
 * THE IMPACT BURST -- FUN_00181130 [C]: at each type-9 model light,
 * min(100/n, 25) particles of emitter 12 (-> descriptor 4, fxglass),
 * plus the "popcorn" concrete dust (instance 2, 0x003EAE88); at light
 * types 5 and 6, min(20/n, 25) of emitter 13 (-> descriptor 5, the
 * blue-tinted glass).  So the retail impact burst is GLASS AND CONCRETE
 * DUST, not smoke -- the smoke arrives from the plume and the trail.
 *
 * This chain also resolves RE_CARFX's "corona light types 8..11 =
 * exhaust / roof / misc [?]":  8 = boost exhaust, 9 = impact glass,
 * 10 = wreck smoke, 11 = wreck fire.                                 [C]
 *
 * THE WHEEL FX -- FUN_001807C0, per wheel over veh[0x1169] wheels [C]:
 * four fixed emitter ids 0, 1, 3, 4 (@0x00180CDF, 0x00180D44,
 * 0x00180DDF, 0x00180E7A -> descriptors 8, 7, 10, 11, all fxsmoke),
 * PLUS a surface-dependent id read from the 0x003A3BF8 table
 * (@0x0018092A, emitted @0x00180EB9, skipped when the id is 0x1A =
 * "none").  Surfaces 4/5/14/15/16 give emitter 7 -> fxgravel, surfaces
 * 7/11/19 give emitter 6 -> fxsnow.  Surfaces 4, 11 and 14 additionally
 * lay the scrape decal through FUN_0003B350.
 *
 * THE GATES ARE THE SURFACE ROW ITSELF -- recovered 2026-08-13.      [C]
 * Each wheel record (veh + 0x820 + wheel*0xC0) carries THREE gate bytes
 * at +0xA0/+0xA1/+0xA2 and FOUR dither carries at +0xA4..+0xA7.  The
 * three bytes are slewed every frame by FUN_001805B0 towards
 *     +0xA0 <- surfrow[+0x00] "scale" * skid_flag    (@0x0018093B)
 *     +0xA1 <- surfrow[+0x04] "skid"                 (@0x00180941)
 *     +0xA2 <- surfrow[+0x08] "gravelness"           (@0x0018094C)
 * FUN_001805B0 is a byte slew: target = t*255 (@0x003B16C4), step =
 * dt*510 (@0x003B1840), clamped and rounded with +0.5 (@0x003B1684).
 * The skid_flag is 1.0 (@0x003B168C) only in the wheel states the
 * jump table at 0x00180EC8 selects out of wheel+0x78 = 1..6.
 * Then, once per wheel and only when the byte is non-zero:
 *     +0xA0 -> emitter 0  (and, if veh+0x215 == 1, ALSO emitter 1)
 *     +0xA1 -> emitter 3
 *     +0xA2 -> emitter 4
 * each with rate multiplier (byte/255*0.25 + 0.75) * spin_scale and
 * particle intensity min(byte/255*2, 1)  (@0x00180C77..0x00180CB0),
 * and the SURFACE emitter with rate spin_scale, intensity 1.0 --
 * that last one ONLY for wheels 2 and 3, i.e. THE REAR WHEELS
 * (`if (1 < wheel_index)` @0x00180924).
 *
 * So the offroad dust is NOT slip-gated at all: on a dirt row the
 * "gravelness" byte pins at 255 and emitter 4 (descriptor 11, a 2.5 ->
 * 4.0 m fxsmoke) runs flat out at every wheel, with emitter 3
 * (descriptor 10, 4.0 -> 9.0 m, 5 s) behind it at the row's "skid"
 * weight and the fxgravel roostertail off the rear wheels.  The port's
 * old slip>0.22 gate suppressed all four of them.
 * The skid_flag's own wheel-state source is not in this port, so it is
 * driven from the harness's drift proxy                       [S] GLUE.
 *
 * THE VALUEDB IS A DEAD END, and that is good news [C]: the strings
 * "../export/ValueDB/VehicleMaterial/default.cfg" (0x003AAEE4) and
 * "../export/ValueDB/VehicleMaterial/" (0x003AAF14) are referenced from
 * NOWHERE in the image (an exhaustive 4-byte pointer scan over every
 * PT_LOAD segment, plus a Ghidra xref query), while other ValueDB paths
 * such as Score.cfg ARE referenced -- so the VehicleMaterial FX tree was
 * baked in at build time and the 24 descriptors below ARE the shipped
 * tuning values, not a runtime-loaded copy.
 *
 * ART: run `python3 tools/extract_particlefx_art.py` first (it writes
 * build/particlefx/*.png out of Data/Global.txd).  With it missing every
 * entry point degrades to a no-op.
 */

#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define B3_PFX_DESCS      24
#define B3_PFX_EMITTERS   26
#define B3_PFX_SURFACES   40
#define B3_PFX_NONE     0x1A   /* the surface table's "no emitter"     [C] */
#define B3_PFX_WHEELS     32   /* wheel-FX state slots (harness bound)    */
/* The pool is now RINGS, one per descriptor, each exactly cap_a+cap_b
 * slots wide -- FUN_00035740 mallocs that and FUN_00035C00 wraps into
 * it, so a full system OVERWRITES ITS OLDEST particle instead of
 * refusing the new one.  Sum of the 24 rings is 5630, plus the four
 * popcorn rings (900/900/300/900).                                  [C] */
#define B3_PFX_MAX      9216

/* One FX descriptor; the comments carry the record's own offsets.   [C] */
typedef struct {
    const char *tex;      /* +0x00 texture name in Data/Global.txd      */
    int         kind;     /* +0x08 render-kind class 0..5               */
    int         blend;    /* +0x0C 0 alpha, 1 SUBTRACTIVE, 2 additive   */
    float       life;     /* +0x10 seconds                              */
    float       mid_t;    /* +0x14 mid-key time fraction                */
    unsigned    col[3];   /* +0x18/1C/20  0xAABBGGRR keys (R = low byte)*/
    float       size[3];  /* +0x28/2C/30  size keys, world units        */
    float       grav;     /* +0x44 gravity y (pos = p + v*t + g*t*t)    */
    float       cone;     /* +0x4C cos(cone half-angle)                 */
    int         cap_a;    /* +0x60 ring-A capacity                      */
    int         cap_b;    /* +0x64 ring-B capacity                      */
    float       max_px;   /* +0x6C max screen size, used x0.435         */
    float       min_px;   /* +0x70 min screen size cull                 */
} B3PfxDesc;

/* One emitter type.  The ADDRESSES of `v[]` are [C] (they are built into
 * two float4 min/max boxes at 0x00181BC5..0x00181E05); the
 * per-component assignment below is [S]. */
typedef struct {
    int   sys;                /* +0x00 descriptor index                 */
    float rate;               /* +0x04 particles per metre              */
    float v[6];               /* +0x08..+0x1C velocity / spin       [S] */
    float size_lo, size_hi;   /* +0x20/+0x24 size multiplier range  [S] */
    float alpha_lo, alpha_hi; /* +0x28/+0x2C alpha multiplier range [S] */
    float strm_a, strm_b;     /* +0x30/+0x34 the two ring weights   [C] */
} B3PfxEmitter;

/* One surface row.  `emit` is the wheel-FX emitter id, or B3_PFX_NONE. */
typedef struct {
    float scale;          /* +0x00 emit-strength scale              [C] */
    float skid;           /* +0x04 skid / darkening                 [S] */
    float gravelness;     /* +0x08                                  [S] */
    signed char emit;     /* +0x0C emitter id, 0x1A = none          [C] */
    signed char decal;    /* lays the fxscrape decal (surf 4/11/14) [C] */
} B3PfxSurface;

/* Read-only access to the three recovered tables (tools/validate_pfx.py
 * walks these against build/burnout3.elf). */
const B3PfxDesc    *b3_pfx_desc(int i);
const B3PfxEmitter *b3_pfx_emitter(int i);
const B3PfxSurface *b3_pfx_surface(int i);

/* ---- lifecycle ------------------------------------------------------ */
int  b3_pfx_init(const char *dir);   /* build/particlefx; 0 = disabled  */
int  b3_pfx_ready(void);
void b3_pfx_reset(void);
void b3_pfx_shutdown(void);

/* ---- the raw emitters (ports of FUN_00181A80 / FUN_001824C0) -------- */
/* Distance-based streak along p0 -> p1.  `carry` is the caller's dither
 * byte (retail keeps one per emitter at 0x007547CC+): pass the address
 * of a persistent unsigned char. */
void b3_pfx_streak(int emitter, const float p0[3], const float p1[3],
                   const float vel[3], float dist, float intensity,
                   unsigned char *carry);
/* Burst: the caller supplies the count instead of the table rate. */
void b3_pfx_burst(int emitter, const float pos[3], const float vel[3],
                  float count, float intensity);

/* ---- the recovered drivers ------------------------------------------ */
/* FUN_00186D50, the wreck slide trail.  `t` is seconds since impact. */
void b3_pfx_crash_slide(const float pos[3], const float vel[3],
                        float speed, float t, float dt);
/* FUN_00181610 emitter 11, the 14 s wreck smoke plume. */
void b3_pfx_wreck_plume(const float pos[3], const float vel[3],
                        float t, float dt);
/* FUN_00181130, the impact burst: glass + concrete dust.  `n_points` is
 * how many emit points to spread the recovered budget over -- retail
 * uses the model's type-9 light records, and the harness has no
 * per-model light table at the wreck site, so it passes 1 and the module
 * scatters.  GLUE on the POSITIONS, [C] on the counts and descriptors. */
void b3_pfx_impact_burst(const float pos[3], const float vel[3],
                         int n_points);
/* FUN_001807C0, the per-wheel FX -- now the recovered gate law.
 *   slot     0..B3_PFX_WHEELS-1, the caller's stable per-wheel id; the
 *            module keeps that wheel's three gate bytes (+0xA0..0xA2)
 *            and four dither carries (+0xA4..0xA7) behind it.
 *   surface  indexes the recovered 0x003A3BF8 table.
 *   slip     0..1, the skid_flag of the +0xA0 gate (wheel+0x78's state
 *            machine is not in this port)                     [S] GLUE.
 *   rear     non-zero on wheels 2/3 -- the SURFACE emitter is rear-only.
 *   intensity extra rate scale the harness uses to weight rivals. */
void b3_pfx_wheel(int slot, const float pos[3], const float vel[3],
                  float dist, int surface, float slip, float intensity,
                  int rear);
/* The wall / armco grind spark shower.  fxspark IS named by the engine
 * (0x003AAFC0) but no descriptor in the 24-record table uses it, so the
 * sparks come from the second, "popcorn" table at 0x003EADE8 whose
 * record 0 is fxpopcornspark (start rgb 1.9,0.8,0.4 -> 0.7,0.2,0.05,
 * gravity -14.7) [C].  The TRIGGER and the RATE are TUNED. */
void b3_pfx_grind_spark(const float pos[3], const float dir[3],
                        float speed, float dt, unsigned char *carry);

/* ---- simulation + draw ---------------------------------------------- */
void b3_pfx_update(float dt);
void b3_pfx_draw(void);
int  b3_pfx_live(void);
int  b3_pfx_live_of_desc(int desc);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_PARTICLEFX_H */
