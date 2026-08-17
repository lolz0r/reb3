#ifndef BURNOUT3_CARFX_H
#define BURNOUT3_CARFX_H
/* Car appearance FX, recovered from the retail XBE (build/burnout3.elf) and
 * checked against the xemu reference captures in "REFERENCE IMAGES/".
 *
 *   1. BODY SHINE  -- the car body is lit by an L2 spherical-harmonic
 *      IRRADIANCE environment, not by a cube/sphere env map.  FUN_000313E0
 *      (0x000313E0) builds the Ramamoorthi-Hanrahan irradiance matrix from
 *      nine per-model SH coefficients (modelobj+0x5C) with the published
 *      constants 0.429043 / 0.511664 / 0.743125 / 0.886227 / 0.247708
 *      (VAs 0x003868F0/F4/F8/FC, 0x003B1AE0); FUN_000315C0 scales all four
 *      rows and uploads them as vertex-shader constants c0..c3 (hw slots
 *      96..99).  Per-record material constants come from the shine table at
 *      0x0045BB20 (initialised by FUN_00030150) and the Fresnel pair at
 *      0x004D6FE0/0x004D6FF0.  All execution-verified -- see docs/RE_CARFX.md.
 *
 *   2. SHADOW      -- a "blobbyshadow"-textured ground quad, sized from the
 *      car's half width (.bgv +0xE80) and its two axle Z's (.bgv +0xBF8 /
 *      +0xB38+nw*0x40), lifted 0.06, SRCALPHA/INVSRCALPHA with ZWRITE off
 *      (FUN_0019A7C0 / FUN_00043570 / FUN_00043350).
 *
 *   3. LIGHT GLOW  -- "coronaglow" billboards at the .bgv light table
 *      (+0x1664 offsets / +0x16AC counts, 0x30-byte records), colour picked
 *      per light-byte bit from the constants at 0x004161A0..0x004161F0
 *      (FUN_00187C70 / FUN_001879E0 / FUN_00187BE0 / FUN_00042BC0).
 *
 * Evidence marks used throughout the .c: [C] execution- or byte-verified,
 * [S] read from the disassembly, [?] open, GLUE = this port's own bridging.
 *
 * OWNERSHIP: this module (both files) belongs to the car-FX agent. The
 * harness calls only this contract; burnout3_full.c call sites are patched
 * by the orchestrator on landing (see scratchpad integration_carfx.md).
 *
 * ART: run `python3 tools/extract_carfx_art.py` first -- it writes
 * build/carfx/{blobbyshadow,coronaglow}.png and build/cars/<CAR>.lights.
 * With the art absent every entry point degrades to a no-op and the harness
 * renders exactly as it did before.
 */

/* ---- light-byte bits (vehicle+0x58 of the model instance) ---------------
 * Written per frame by FUN_0011BE50: cleared at 0x0011BF55, brake bit ORed
 * at 0x0011BFC3, reverse bit at 0x0011BFD6, the rest ORed in wholesale from
 * carObj+0x18FF (whose writer is [?]).  Bits 2..7 (mask 0xFC) are also what
 * gates the mesh's own light records in FUN_00031AB0.                  [S] */
#define B3_CARFX_LIGHT_HEAD_HI   0x02u   /* corona type 0, colour 1.5 white */
#define B3_CARFX_LIGHT_HEAD      0x04u   /* corona type 0, colour 1.0 white */
#define B3_CARFX_LIGHT_TAIL      0x08u   /* corona type 1, colour 1.1 red   */
#define B3_CARFX_LIGHT_BRAKE     0x10u   /* corona type 2, colour 1.4 red   */
#define B3_CARFX_LIGHT_INDIC_R   0x20u   /* corona type 5, amber            */
#define B3_CARFX_LIGHT_INDIC_L   0x40u   /* corona type 6, amber            */
#define B3_CARFX_LIGHT_REVERSE   0x80u   /* corona type 4, 0.9 white        */

#define B3_CARFX_MAX_CARS 16
/* second registry, for entities that are not racers (traffic .btv fleet) */
#define B3_CARFX_MAX_EXTRA 16

/* ---- lifecycle --------------------------------------------------------- */
void b3_carfx_init(void);          /* loads art + compiles the shine shader */
int  b3_carfx_ready(void);         /* 0 = every entry point is a no-op      */
void b3_carfx_shutdown(void);

/* Per-car tables from build/cars/<cls>_<base>.lights (extract_carfx_art.py).
 * Returns the number of corona lights loaded, 0 if the file is absent. */
int  b3_carfx_load_car(int slot, const char* cls, const char* base);

/* ---- environment inputs -- BOTH RECOVERED [C] ---------------------------
 * 1. The nine SH coefficients at modelInstance+0x5C are nine LITERALS in
 *    .rdata, not a runtime projection.  Three sites write the identical set
 *    just before the car draw: FUN_000C047C, FUN_001A74C5 and FUN_001AE93C.
 *    They do not vary per track or per car.  B3_CARFX_SH_RETAIL is that set;
 *    b3_carfx_init() installs it, so callers need do nothing.
 * 2. The RGB at DAT_0060E0A0 (ps c14.xyz) is the environment object's +0x60,
 *    and the environment object (0x0060E040) is overwritten wholesale from
 *    the track's "enviro.dat" by FUN_001888F0 -> FUN_00188C00 @0x00188A40.
 *    So c14.xyz = enviro.dat bytes 0x60..0x6B -- the track's sun colour.
 *    Call b3_carfx_set_track("US_C3_V1") once per track load.            */
extern const float B3_CARFX_SH_RETAIL[9];         /* the in-race probe   [C] */
extern const float B3_CARFX_SH_RETAIL_MIRROR[9];  /* showcase reflection [C] */

/* Installs B3_CARFX_SH_RETAIL and the track's enviro.dat sun colour.
 * Returns 0 (and leaves the light RGB alone) for an unknown track name;
 * names are "<REGION>_<TRACK>", e.g. "US_C3_V1", 37 known. */
int  b3_carfx_set_track(const char* track);
/* the sun colour on its own, without touching module state */
int  b3_carfx_env_light_rgb(const char* track, float out_rgb[3]);

void b3_carfx_set_sh(const float L[9]);            /* modelInstance+0x5C [C] */
void b3_carfx_set_light_rgb(float r, float g, float b);  /* ps c14.xyz   [C] */

/* THE MIRROR.  The SH probe is consumed in WORLD space (RE_CARFX.md 2.7), and
 * this harness's GL world is the Z-mirror of the game world (trackmesh_load
 * negates the Z of every position and every normal; RE_NOTES 12).  So the
 * module evaluates the probe on a Z-mirrored copy of the nine coefficients:
 * negating the three whose R-H basis function is odd in z -- i = 2 (L10),
 * 5 (L2-1) and 7 (L21) -- gives E'(x,y,z) == E(x,y,-z) exactly.  Set by
 * b3_carfx_set_sh(); exposed here for the validator.                   GLUE */
void  b3_carfx_sh_mirror_z(const float in9[9], float out9[9]);
/* E(n) = n^T M n.  ..._irradiance() is in the harness's GL space (mirrored),
 * ..._irradiance_game() in the game's own space; they agree under n.z -> -n.z.
 * `n` need not be unit, but the shader always normalises. */
float b3_carfx_irradiance(const float n[3]);
float b3_carfx_irradiance_game(const float n[3]);
/* GLUE, SUPERSEDED by B3_CARFX_SH_RETAIL: projects a two-radiance
 * hemisphere onto the L2 SH basis.  Kept only for callers with no track. */
void b3_carfx_sh_from_hemisphere(const float sky_rgb[3],
                                 const float ground_rgb[3],
                                 const float up[3]);

/* ---- body / glass shine ------------------------------------------------ */
/* ---- light probes -- the per-position source of the nine SH coefficients
 * FUN_0019D400 writes into every car every frame.  Container and consumer are
 * both recovered; see src/burnout3_carfx.c section 4b and
 * tools/extract_light_probes.py.  b3_carfx_set_track() loads
 * build/tracks/<ID>/light_probes.bin automatically, so a caller that already
 * calls set_track needs to do nothing but fill in B3CarFxBodyParams::pos and
 * ::slot.                                                              [C] */
int  b3_carfx_probes_load(const char* path);   /* triangles loaded, 0 = none */
int  b3_carfx_probes_loaded(void);
void b3_carfx_probes_free(void);
/* pos is in GAME space (the harness' GL Z-mirror already undone).  Returns 0
 * on a miss, in which case the caller must KEEP its previous nine. */
int  b3_carfx_probe_sample(const float pos_game[3], float out9[9]);

/* ---- the body's texture stage 1 -- the environment map ------------------
 * The recovered pixel shader lerps the paint toward this map along the
 * recovered reflection vector.  Which texture retail binds is [?]
 * (DAT_004D6C00 has exactly one reference in the image and it is the read;
 * four exhaustive searches for the writer are recorded in section 5C of the
 * .c), so b3_carfx_set_track() loads the [S] substitute -- the track's own
 * `enviro.dat +0xA0` reflection sheet, written by tools/extract_envmap.py to
 * build/tracks/<ID>/envmap.png.  B3_CARFX_ENVMAP=<png> overrides it, and this
 * call installs one directly.  With no map at all (four tracks ship none) the
 * shader falls back to the SH probe sampled along the same ray.       GLUE */
int  b3_carfx_load_env_map(const char* png_path);

typedef struct {
    float rot3[9];     /* object->world 3x3, ROW-major (r0,r1,r2 rows)      */
    float pos[3];      /* car world position in the harness' GL space --
                        * FUN_0019D400's probe sample point (carObj+0x40)   */
    int   slot;        /* per-car probe cache index, -1 = no probe lookup   */
    float sh_scale;    /* FUN_000303D0's SH row scale: 1.0, or the 0.4 s
                        * despawn ramp 1-t*1.875 clamped at 0.25       [C]  */
    float fade;        /* pixel-shader c3.w (DAT_004A1CF4)             [C]  */
    int   has_texture; /* paint texture is bound on unit 0                  */
    int   has_normals; /* display list emits glNormal (else face normals).
                        * LOAD-BEARING for the shine: the specular term is
                        * evaluated along reflect(-V,N), so a face normal
                        * makes the highlight a flat per-triangle patch
                        * instead of the smooth streak the game has.  Emit
                        * the OBJ's `vn` and set this to 1.                */
    int   alt_material;/* record+0x14 != 0 -> c14.w = 0.5 not the table[C]  */
    int   paint_index; /* modelobj+0x59, indexes the 8-entry shine table    */
} B3CarFxBodyParams;

void b3_carfx_body_defaults(B3CarFxBodyParams* p);
void b3_carfx_body_begin(const B3CarFxBodyParams* p);
void b3_carfx_body_end(void);
/* Glass uses the same program with the glass row of the shine table and the
 * per-record tint FUN_000300A0 writes (1.0 intact / 0.5 cracked / 0.6
 * shattered [C, RE_NOTES 13]). */
void b3_carfx_glass_begin(const B3CarFxBodyParams* p, float tint);
void b3_carfx_glass_end(void);

/* helper: build rot3 from a yaw already in the harness' GL convention */
void b3_carfx_rot3_from_yaw(float yaw_rad, float out9[9]);

/* ---- shadow ------------------------------------------------------------ */
void b3_carfx_shadow_pass_begin(void);
/* pos       : car origin in world/GL space
 * yaw_rad   : the same angle the harness passes to glRotatef, in radians
 * ground_y  : world Y of the ground under the car
 * ramp      : FUN_0019A7C0's opacity ramp input (vehicle+0x70) [?] -- pass
 *             1.0 for the fully-faded-in shadow
 * air_h     : height above resting ride height, i.e. (drop to ground) minus
 *             the .bgv wheel radius; 0 when the car is sitting down    [C] */
/* Ground-plane variant: ground_n = probed surface normal (NULL = flat +Y).
 * Drapes the blob onto the slope so it is not cut off on hills. */
void b3_carfx_shadow_draw_n(int slot, const float pos[3], float yaw_rad,
                            float ground_y, float ramp, float air_h,
                            const float ground_n[3]);
void b3_carfx_shadow_draw(int slot, const float pos[3], float yaw_rad,
                          float ground_y, float ramp, float air_h);
void b3_carfx_shadow_pass_end(void);

/* ---- light coronas ----------------------------------------------------- */
void b3_carfx_corona_pass_begin(void);
void b3_carfx_corona_draw(int slot, const float pos[3], float yaw_rad,
                          unsigned light_byte);
/* FULL-POSE corona draw.  The yaw-only entry above places the lamp offsets
 * with a yaw rotation, which separates the emitters from the bodywork the
 * moment the car has any pitch or roll -- crests, and above all a tumbling
 * wreck (debug dump 018: both tail coronas hanging off the car).  This entry
 * takes the SAME object->world 3x3 the body draw uses (B3CarFxBodyParams::
 * rot3, row-major), so the emitters ride the bodywork through any attitude.
 * rot3 == NULL is identity.  The yaw entry is now a wrapper over this. */
void b3_carfx_corona_draw_pose(int slot, const float pos[3],
                               const float rot3[9], unsigned light_byte);
/* NON-RACER light tables.  Traffic (.btv) and any other entity that is not one
 * of the B3_CARFX_MAX_CARS racer slots gets its own registry, so a traffic
 * fleet larger than the racer grid cannot collide with it.  `idx` indexes that
 * second table; the loader takes the same build/cars/<cls>_<base>.lights
 * sidecar (tools/extract_traffic_lights.py writes them from the .btv light
 * table, which is byte-identical in layout to the .bgv one). */
int  b3_carfx_load_extra(int idx, const char* cls, const char* base);
void b3_carfx_corona_draw_extra(int idx, const float pos[3],
                                const float rot3[9], unsigned light_byte);
void b3_carfx_corona_pass_end(void);
/* Convenience: build the light byte from harness state the way FUN_0011BE50
 * does for the two bits whose writers ARE recovered (brake / reverse); the
 * head/tail/indicator bits come from carObj+0x18FF whose writer is [?], so
 * they are passed in by the caller. */
unsigned b3_carfx_light_byte(int braking, int reversing, unsigned ambient);

#endif
