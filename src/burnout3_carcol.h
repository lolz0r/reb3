#ifndef BURNOUT3_CARCOL_H
#define BURNOUT3_CARCOL_H
/* ===========================================================================
 * Car-vs-car collision -- recovered from the retail XBE and ported 1:1.
 *
 * OWNERSHIP: this module (both files) belongs to the car-collision agent.
 * Full evidence, addresses and provenance: docs/RE_CARCOL.md.
 *
 * The real chain (all addresses are the corrected ELF mapping, see HANDOFF 2):
 *
 *   FUN_00110AF0   collision-manager frame: sweep-and-prune over per-object
 *                  world AABBs -> up to 0x100 pairs {objA,objB} (stride 0x30)
 *   FUN_00111CD0   pair dispatch by object type byte (obj+0x00)
 *                    both cars, BOTH un-crashed (veh+0x210 == 0) -> FUN_001121F0
 *                    one crashed                                 -> FUN_00113960
 *   FUN_0010A9D0   builds the hull-query context (two frames, two inverse
 *                  frames, two hulls) then FUN_0010ABC0 -> FUN_0010AC20
 *   FUN_0010AC20   convex-hull narrow phase: support-edge soup of each hull
 *                  clipped against the other hull's planes -> contact point,
 *                  contact normal, per-body penetration push-out
 *   FUN_001121F0   racer-vs-racer response: horizontal mass-split separation,
 *                  mutual impulse (FUN_0010F8D0), the mutual SHOVE force at
 *                  the contact point (FUN_001205E0), impact magnitude, the
 *                  crash trigger and the rear-end / side SLAM classification
 *                  that feeds the takedown chain (game-context vtable +0x64,
 *                  i.e. FUN_001989A0's caller)
 *   FUN_00113960   car-vs-crashed-car response: mass-split separation and the
 *                  same mutual impulse, applied linear+angular at the contact
 *                  point (FUN_00106500), plus its own crash threshold
 *
 * The per-car collision hull is REAL DATA: a 0x600-byte convex-polyhedron
 * record at .bgv +0x1060 (FUN_00122830 -> FUN_00122C20 copies it to
 * veh+0x220 and points veh+0x208 at it).  Extract with
 *   python3 tools/emulate_carcol.py --extract-hulls
 * which writes build/cars/<CLS>_<CarN>.hull; b3_carcol_hull_load() reads it.
 * Without the file the module falls back to a box hull built from the
 * .bgv +0xE80/+0xE90 collision extents (marked GLUE at the call site).
 * ===========================================================================
 */

#include "burnout3_vehicle_sim.h"   /* B3RigidBody */

#ifdef __cplusplus
extern "C" {
#endif

/* --- the hull record (.bgv +0x1060, relinked layout) --------------------- */
#define B3_HULL_MAX_VERTS   22      /* veh+0x540..+0x6A0 */
#define B3_HULL_MAX_PLANES  40      /* veh+0x2C0..+0x540 */
#define B3_HULL_MAX_EDGES   60      /* veh+0x6A0..+0x718 */

typedef struct B3CarHull {
    int   nverts;                       /* hull+0x18 */
    int   nplanes;                      /* hull+0x19 */
    int   nedges;                       /* hull+0x1A */
    float verts[B3_HULL_MAX_VERTS][4];  /* hull+0x320 */
    float planes[B3_HULL_MAX_PLANES][4];/* hull+0x0A0  {n.xyz, d}, inside: n.p <= d */
    unsigned short edges[B3_HULL_MAX_EDGES]; /* hull+0x480  lo byte = v0, hi = v1 */
} B3CarHull;

/* Collision object types as the game's object records carry them
 * (record+0x00; FUN_0010C550 treats 0/1/2 as "car", FUN_0010FBC0 maps
 * 0 and 2 to interaction class 0, 1 to class 1). */
#define B3_COL_TYPE_RACER    0   /* player / AI race car */
#define B3_COL_TYPE_CLASS1   1   /* class-1 car (immovable w.r.t. class 0) */
#define B3_COL_TYPE_TRAFFIC  2

/* One participant. Mirrors the live vehicle fields the chain touches. */
typedef struct B3CarBody {
    B3RigidBody*     rb;          /* +0x204 frame, +0xB0.. dynamics, +0xF0.. accs */
    const B3CarHull* hull;        /* +0x208 */
    float            mass;        /* +0x1F0 */
    float            bbmax[4];    /* +0x1D0  (.bgv +0xE80) */
    float            bbmin[4];    /* +0x1E0  (.bgv +0xE90) */
    unsigned char    type;        /* collision object record +0x00 */
    unsigned char    crashed;     /* +0x210  (set => crashed/simplified path) */
    unsigned char    grounded;    /* +0x212 */
    unsigned char    asleep;      /* +0x20E  (pair filter FUN_00114610) */
    /* FUN_00113960's recent-slam suppression.  Retail computes, on B's
     * dilated clock (+0x10DC), the age of this pair's slam stamp -- either
     * the attribution entry B+0x15A8[A+0x19BC] or A's own +0x140C when
     * A+0x15D6 marks B as its current victim -- and clears the crash flag
     * when that age is < 1.5 s ([0x003EBE7C]).  Crucially the whole test is
     * SKIPPED when a non-traffic car hits a traffic car (the guard
     * `A->type == 2 || B->type != 2` @0x00114...), so it never gates the
     * racer-vs-traffic cascade.  Kept as a caller-supplied boolean so the
     * clocks stay in the harness: 0 (the memset default) means "no recent
     * slam", which is retail's fVar28 = 1000.0 initial value. */
    unsigned char    slam_recent;  /* 1 => pair slammed within 1.5 s */
    int              drift_state; /* +0x1524 */
    float            yaw_input;   /* +0x1408 */
    /* outputs written by the response */
    unsigned char    touched;     /* +0x211  "in contact with a car this frame" */
    unsigned char    hit_side;    /* +0x153C */
    float            contact_pt[4]; /* +0x150 */
} B3CarBody;

/* Slam/impact event kinds -- the first argument the game passes to the
 * game-context virtual +0x64 (which forwards to the FUN_001989A0 slam chain,
 * docs/RE_GAMEPLAY.md 6). */
#define B3_SLAM_NONE        0
#define B3_SLAM_RUB         1   /* contact under the crash speed, strength 1.0 */
#define B3_SLAM_SIDE_LIGHT  3
#define B3_SLAM_REAR_LIGHT  4
#define B3_SLAM_SIDE        5
#define B3_SLAM_REAR        6

/* The pair record (collision world +0xE6C90, stride 0x30) after a resolve. */
typedef struct B3CarContact {
    float point[4];      /* +0x00  world contact point (y overridden, see .c) */
    float normal[4];     /* +0x10  horizontal contact normal, A -> B */
    float impact;        /* +0x20  (mA+mB) * |vn_mph| * 0.1 * 0.5 */
    int   hit;           /* +0x2C */
    int   slam_class;    /* +0x2D  0 none, 1 side, 2 rear-end */
    /* classification results (the vtable +0x64 arguments) */
    int   event;         /* B3_SLAM_* */
    int   attacker_is_b; /* 0 => A is the attacker, 1 => B is */
    float strength;      /* 0..1 */
    int   crash_a;       /* FUN_0010DCA0 fired for A */
    int   crash_b;
    /* diagnostics */
    float vn_mph;        /* |dot(v_rel, n)| * 2.23693633 */
    float impulse[4];    /* the mutual impulse actually applied (A gets +) */
    float j;             /* its magnitude (FUN_0010F8D0's return) */
    float t_a, t_b;      /* longitudinal contact parameter, 0 = rear, 1 = nose */
    float pen_a[4], pen_b[4]; /* per-body separation displacement (pre-split) */
} B3CarContact;

/* ------------------------------------------------------------------------ */
void b3_carcol_init(void);

/* Hull sourcing. b3_carcol_hull_load reads build/cars/<CLS>_<CarN>.hull
 * (written by tools/emulate_carcol.py --extract-hulls; that file is the raw
 * 0x600-byte .bgv record). Returns 1 on success. */
int  b3_carcol_hull_load(const char* path, B3CarHull* out);
int  b3_carcol_hull_from_record(const void* rec600, B3CarHull* out);
/* GLUE fallback: an 8-vertex box hull from the .bgv collision extents. */
void b3_carcol_hull_from_extents(const float bbmax[4], const float bbmin[4],
                                 B3CarHull* out);

/* Broad phase: world AABB of a body's collision box (FUN_00114270), and the
 * overlap predicate the sweep-and-prune in FUN_00110AF0 applies. */
void b3_carcol_world_aabb(const B3CarBody* b, float lo[3], float hi[3]);
int  b3_carcol_aabb_overlap(const B3CarBody* a, const B3CarBody* b);
/* Fills pairs[][2] with the indices the game's sweep would emit (same set;
 * the SAP sort is an acceleration, see docs/RE_CARCOL.md). */
int  b3_carcol_broadphase(B3CarBody* const* bodies, int n,
                          int (*pairs)[2], int max_pairs);

/* Narrow phase (FUN_0010A9D0 -> FUN_0010AC20). Returns 1 on contact and
 * fills point/normal plus the per-body separation displacements. */
int  b3_carcol_contact(const B3CarBody* a, const B3CarBody* b,
                       B3CarContact* out);

/* Response.  b3_carcol_resolve picks the right one exactly as FUN_00111CD0
 * does (ordering the pair so A is the un-crashed car). */
int  b3_carcol_resolve(B3CarBody* a, B3CarBody* b, B3CarContact* out);
int  b3_carcol_resolve_alive(B3CarBody* a, B3CarBody* b, B3CarContact* out);
int  b3_carcol_resolve_wreck(B3CarBody* a, B3CarBody* b, B3CarContact* out);

/* The mutual contact impulse, FUN_0010F8D0.  bodies are (b1 = the body whose
 * point is pt1, b3 = the body whose point is pt0), matching the real
 * parameter split; returns |j| and writes n*(-|j|) to out. */
float b3_carcol_mutual_impulse(const B3RigidBody* rb1, float m1,
                               const B3RigidBody* rb3, float m3,
                               const float pt3[4], const float pt1[4],
                               const float vrel[4], const float n[4],
                               float restitution, float out[4]);

/* FUN_001205E0: route a force either linear-only (+0xF0) or at the contact
 * point (+0xF0 and +0x100), by the drift state and the yaw-direction test. */
void b3_carcol_apply_force(B3RigidBody* rb, int drift_state,
                           const float force[4], const float point[4]);

/* Contact-point velocity, FUN_001066A0 (vel + omega x (pt - pos)). */
void b3_carcol_point_velocity(const B3RigidBody* rb, const float pt[4],
                              float out[4]);

/* ---- tuning constants recovered from the image (see docs/RE_CARCOL.md) --- */
#define B3_CARCOL_RESTITUTION      0.1f        /* 0x003EBE3C */
#define B3_CARCOL_WRECK_RESTITUTION 0.0f       /* 0x004A1D98 */
#define B3_CARCOL_MPH              2.23693633f /* 0x0038994C */
#define B3_CARCOL_SHOVE_K          20.0f       /* 0x0041A4D0 */
#define B3_CARCOL_SHOVE_MASS_CAP   2000.0f     /* 0x003EBE70 */
#define B3_CARCOL_IMPACT_SCALE     0.1f        /* 0x003EBE74 */
#define B3_CARCOL_CRASH_MPH        150.0f      /* 0x003EBE4C */
#define B3_CARCOL_REAR_MIN_MPH     20.0f       /* 0x003EBE60 */
#define B3_CARCOL_REAR_RANGE_MPH   50.0f       /* 0x003EBE68 */
#define B3_CARCOL_SIDE_MIN_MPH     30.0f       /* 0x003EBE5C */
#define B3_CARCOL_SIDE_RANGE_MPH   20.0f       /* 0x003EBE64 */
#define B3_CARCOL_LIGHT_FRAC       0.3f        /* 0x003EBE80 */
#define B3_CARCOL_SIDE_HI_MPH      35.0f       /* 0x0041A4C4 */
#define B3_CARCOL_SIDE_LO_MPH      20.0f       /* 0x0041A4C8 */
#define B3_CARCOL_SIDE_ANG         40.0f       /* 0x0041A4CC */
#define B3_CARCOL_ATTACK_DV        17.8815994f /* 0x003B1B68 (= 40 mph) */
#define B3_CARCOL_WRECK_IMPACT     5000.0f     /* 0x003EBE50 */
#define B3_CARCOL_WRECK_IMPACT_TR  2500.0f     /* 0x003EBE54 */
#define B3_CARCOL_SLAM_WINDOW_S    1.5f        /* 0x003EBE7C */
#define B3_CARCOL_NORM_BLEND      (-0.9f)      /* 0x0041A4C0 */

#ifdef __cplusplus
}
#endif
#endif /* BURNOUT3_CARCOL_H */
