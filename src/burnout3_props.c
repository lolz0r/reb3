/* =========================================================================
 * burnout3_props.c -- destructible track props (cones, barrier boards,
 * marker posts, signposts, boxes).  See burnout3_props.h for the recovery of
 * the placement data and the knock chain, with addresses.
 *
 * Everything data-driven comes out of build/tracks/<ID>/props.bin, written by
 * tools/extract_props.py straight from static.dat.  Nothing about a particular
 * track is compiled in.
 * ====================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <GL/gl.h>

#include "burnout3_props.h"
#include "burnout3_collision.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191
#endif

/* ---- props.bin ------------------------------------------------------- */
#define B3P_MAGIC   0x50503342u        /* 'B3PP' little-endian            */
#define B3P_MODEL   0x60
#define B3P_INST    0x50

/* Retail hands a knocked prop one of exactly 0x10 class-6 rigid bodies
 * (FUN_00114730: the free-slot bitmask scan ends `CMP EDX,0x10` @0x0011476D
 * and the recycle scan walks the same 16 @0x001147C3, stride 0x780
 * @0x001147BD from the pool base gameworld+0xC4380 @0x001147E4).        [C] */
#define B3P_MAX_LIVE        16
/* FUN_0011A020 @0x0011A19E..0x0011A1B4: body+0x224 = DAT_0060EA20 +
 * [0x003A7F34] (= 10.0).  Recycle-order key only -- FUN_00114730's scan is its
 * ONLY reader and nothing compares it against the clock.                [C] */
#define B3P_LRU_OFFSET      10.0f
/* mph conversion FUN_00112E70 @0x001132B4 reads from [0x0038994C].      [C] */
#define B3P_MS_TO_MPH       2.2369363f

/* ---- the recovered constants of the knock chain ------------------------- */
/* FUN_0011A020 mass law @0x0011A137..0x0011A191.                        [C] */
#define B3P_MASS_MIN        100.0f     /* [0x003A2928] */
#define B3P_MASS_PER_M2     200.0f     /* [0x003A292C] */
/* FUN_00113960 @0x00113F16: the contact normal is bent toward the relative
 * velocity by this much before the impulse.  [0x0041A4C0]               [C] */
#define B3P_NORM_BLEND      (-0.9f)
/* FUN_00113960 @0x00113F5C: the restitution handed to FUN_0010F8D0 is the
 * global DAT_004A1D98, which is 0.0 in the image (the same number the car
 * agent recovered as B3_CARCOL_WRECK_RESTITUTION).                      [C] */
#define B3P_RESTITUTION     0.0f
/* FUN_0003B060's "is this vector zero" epsilon, [0x003B191C] = 2^-32.   [C] */
#define B3P_EPS2            2.3283064e-10f
/* FUN_0011A330's two drag coefficients: linear [0x003B16C0] = -1.0 on the
 * squared speed, angular [0x003B17F8] = -2.0 on |omega|.                [C] */
#define B3P_LIN_DRAG        (-1.0f)
#define B3P_ANG_DRAG        (-2.0f)
/* FUN_0011A020's launch draw @0x0011A1DD..0x0011A2F3.                   [C] */
#define B3P_LAUNCH_HALF     0.5f       /* [0x003B1684] */
#define B3P_LAUNCH_ONE      1.0f       /* [0x003B168C] */
#define B3P_LAUNCH_SCALE    5.0f       /* [0x003B1694] */

/* --- The knocked prop's WORLD contact.  RECOVERED, no GLUE left. ----------
 * The earlier ledger entry (PH-23) claimed retail had no ground pass for a
 * class-6 body because FUN_0011A330 -- the class-6 vtable's slot +0x00 -- is
 * only two drag terms and FUN_00109560.  That was a misread of the vtable:
 * the class-6 vtable at 0x003B1120 has a SECOND per-frame slot, +0x10 =
 * FUN_0011A490, which the collision manager drives once per frame per
 * allocated body.  FUN_0011A490 gathers the local polygon soup
 * (FUN_00109D20 @0x0011A5FB, into the staging list at 0x005A3AA0) and then
 * calls FUN_00109EA0 @0x0011A706 -- THE shared body-vs-world contact
 * resolve -- guarded by "the body is inside a loaded streaming unit"
 * (+0x216 != 0xFF, and when it is 0xFF the accumulators +0xF0/+0x100/
 * +0x110/+0x120/+0x130 are all cleared instead, @0x0011A6D5).
 * So retail DOES resolve a knocked prop against the world, BEFORE the body's
 * own update integrates -- and the whole chain is now ported:
 *     narrow phase  FUN_00107950  -> b3_rigid_body_obb_plane_contact()
 *     resolve       FUN_00109EA0  -> b3_rigid_body_world_contact()
 * The restitution is the rigid-body ctor's +0x1F8 = [0x003A69C4] = 0.1
 * (FUN_00109270 @0x001094C5); nothing in FUN_0011A020 overrides it.
 * B3_PROP_BALLISTIC=1 still skips the whole pass. */
#define B3P_WORLD_RESTITUTION  0.1f    /* [0x003A69C4] @0x001094C5      [C] */

typedef struct {
    float bb_min[3], bb_max[3];
    unsigned first_vertex, n_vertex, first_index, n_index;
    unsigned prop_class;
    float mass, radius, lod_near, lod_far;
    unsigned mat_flags;
    char texture[32];
    /* runtime */
    unsigned tex;
    unsigned list;
} B3PropModel;

/* World-slot type, exactly the retail one: 5 = static prop, 6 = knocked
 * (has a body), 8 = body taken away, dropped by the dispatcher @0x00111D0B. */
enum { B3P_REST = 0, B3P_KNOCKED = 1, B3P_SETTLED = 2 };

typedef struct {
    float base[16];        /* authored transform, HARNESS space, w row fixed */
    float cur[16];         /* live transform, HARNESS space                  */
    float tint[3];         /* doubled half-range instance colour             */
    unsigned model;
    unsigned prop_class;
    unsigned unit;
    float ground_y;        /* surface height under the authored position     */
    short body;            /* class-6 body slot, -1 = none                   */
    unsigned char state;
    unsigned char has_ground;
} B3PropInst;

/* One of the 16 class-6 rigid bodies (gameworld+0xC4380, stride 0x780).
 * The named fields are the body offsets FUN_0011A020 fills in. */
typedef struct {
    B3RigidBody rb;        /* +0x10/+0x40 inertia, +0xB0.. dynamics, +0x204  */
    float mass;            /* +0x1F0 */
    float com_height;      /* +0x1F4 */
    float radius;          /* +0x1CC */
    float lru_key;         /* +0x224 */
    float bbmax[3];        /* +0x1D0, copied from the model bbox @0x0011A0A8 */
    float bbmin[3];        /* +0x1E0, ditto @0x0011A0AF                      */
    int   owner;           /* +0x220 as a prop instance index, -1 = free     */
    unsigned char frozen;  /* +0x20E, the settle latch FUN_00109EA0 raises   */
} B3PropBody;

static B3PropModel* g_model;
static B3PropInst*  g_inst;
static B3PropBody   g_body[B3P_MAX_LIVE];
static float g_clock;      /* our mirror of DAT_0060EA20                     */
static int g_nmodel, g_ninst;
static int g_live;
static int g_ready;
static float* g_vtx;       /* 8 floats per vertex, HARNESS space            */
static unsigned short* g_idx;
static unsigned g_nvtx, g_nidx;
static char g_dir[512];

/* ---- small helpers ---------------------------------------------------- */
static unsigned rd_u32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static float rd_f32(const unsigned char* p) {
    unsigned v = rd_u32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float v_len(const float a[3]) {
    return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}
static float v_dot3(const float a[4], const float b[4]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* The GL column-major instance matrix and B3RigidBody's row frame are the same
 * 16 floats: GL column c holds the object's c-th axis in world space, which is
 * exactly frame row c.  cur[r*4+c] == frame[r][c]. */
static void mat_to_frame(const float m[16], float f[4][4]) {
    memcpy(f, m, 16 * sizeof(float));
}
static void frame_to_mat(const float f[4][4], float m[16]) {
    memcpy(m, f, 16 * sizeof(float));
}

/* ---- the game PRNG, FUN_0011A020 @0x0011A1DD..0x0011A2C0 ---------------- *
 * state = state*0x10000 + (int)state>>16 + inc;  inc += state;  u = state*2^-32
 * ([0x0054F46C]).  Retail draws from the SHARED global pair DAT_0064ACE8 /
 * DAT_0064ACEC, which every other system also advances, so the exact retail
 * sequence is not reproducible from outside the game -- the LAW is [C], the
 * seed is [?].  Same generator as b3_rand01 in burnout3_vehicle_sim.c. */
static unsigned g_rng_state = 0xFD462907u;   /* FUN_001214A0's seeds          */
static unsigned g_rng_inc   = 0x02B9D6F8u;

void b3_props_seed(unsigned state, unsigned inc) {
    g_rng_state = state;
    g_rng_inc = inc;
}
static double b3p_rand01(void) {
    int s = (int)g_rng_state;
    unsigned n = (unsigned)s * 0x10000u + (unsigned)(s >> 16) + g_rng_inc;
    g_rng_inc = n + g_rng_inc;
    g_rng_state = n;
    return (double)n * 2.3283064365386963e-10;
}

/* ---- FUN_000FF270 [C], the frame re-orthonormaliser FUN_00109560 calls ---
 * @0x000FF27D..0x000FF544.  All three rows are normalised first (FUN_0002C0D0
 * returns the PRE-normalisation length); then the function keeps the pair of
 * rows that is already the most orthogonal and rebuilds the other two:
 *
 *   a = |r2 . r1|   b = |r0 . r2|   c = |r1 . r0|      (FUN_00013C60, then the
 *                                                       ABS at FUN_000FF090)
 *   b > a && c > a  ->  A: r0 = ^(r1 x r2), r2 = ^(r0 x r1)   @0x000FF332
 *   b > a && c <= a ->  C: r2 = ^(r0 x r1), r1 = ^(r2 x r0)   @0x000FF37C
 *   b <= a && c <= b->  C                                     @0x000FF3D3
 *   b <= a && c > b ->  B: r1 = ^(r2 x r0), r0 = ^(r1 x r2)   @0x000FF3D5
 * and the three degenerate entries take the same three shapes:
 *   L0 <= 0 -> A @0x000FF4E1,  L1 <= 0 -> B @0x000FF47D,  L2 <= 0 -> C
 *   @0x000FF41B.
 *
 * NOTE: b3_mat_orthonormalize() in burnout3_vehicle_sim.c implements ONLY
 * branch B (which is what a racecar's frame takes, hence its green suite).  A
 * knocked prop tumbles fast enough to take A and C every few frames, so this
 * module carries the whole function -- see docs/PHYSICS_GLUE_LEDGER.md PH-01. */
static void b3p_norm4(float v[4]) {
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2] + v[3]*v[3]);
    if (l == 0.0f) return;
    for (int k = 0; k < 4; k++) v[k] /= l;
}
static float b3p_len4(const float v[4]) {
    return sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2] + v[3]*v[3]);
}
static void b3p_cross4(const float a[4], const float b[4], float o[4]) {
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
    o[3] = a[3]*b[3] - a[3]*b[3];      /* the real code's self-cancelling lane */
}
static void b3p_orthonormalize(float m[4][4]) {
    float L0 = b3p_len4(m[0]), L1 = b3p_len4(m[1]), L2 = b3p_len4(m[2]);
    b3p_norm4(m[0]); b3p_norm4(m[1]); b3p_norm4(m[2]);
    int br;                                   /* 0 = A, 1 = B, 2 = C */
    if (L0 <= 0.0f)      br = 0;
    else if (L1 <= 0.0f) br = 1;
    else if (L2 <= 0.0f) br = 2;
    else {
        float a = fabsf(v_dot3(m[2], m[1]));
        float b = fabsf(v_dot3(m[0], m[2]));
        float c = fabsf(v_dot3(m[1], m[0]));
        if (b > a) br = (c > a) ? 0 : 2;
        else       br = (c <= b) ? 2 : 1;
    }
    float t[4];
    if (br == 0) {
        b3p_cross4(m[1], m[2], t); memcpy(m[0], t, sizeof t); b3p_norm4(m[0]);
        b3p_cross4(m[0], m[1], t); memcpy(m[2], t, sizeof t); b3p_norm4(m[2]);
    } else if (br == 1) {
        b3p_cross4(m[2], m[0], t); memcpy(m[1], t, sizeof t); b3p_norm4(m[1]);
        b3p_cross4(m[1], m[2], t); memcpy(m[0], t, sizeof t); b3p_norm4(m[0]);
    } else {
        b3p_cross4(m[0], m[1], t); memcpy(m[2], t, sizeof t); b3p_norm4(m[2]);
        b3p_cross4(m[2], m[0], t); memcpy(m[1], t, sizeof t); b3p_norm4(m[1]);
    }
}

/* ---- FUN_00109560 [C], the shared rigid-body integrator ------------------
 * Line for line the same function burnout3_vehicle_sim.c ported and verified
 * (tools/validate_port.py, integrator section); repeated here ONLY so the prop
 * path can use the complete FUN_000FF270 above.  Prop bodies always take the
 * state-6 branch @0x00109606, so gravity is applied at pos + up*com_height. */
static void b3p_mat_mul3(const float A[3][4], const float B[3][4],
                         float out[3][4]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            out[i][j] = B[i][0]*A[0][j] + B[i][1]*A[1][j] + B[i][2]*A[2][j];
}
/* FUN_00040AE0: +0x70 = the inverse frame (rotation transposed, translation
 * back-rotated and negated).  Split out of the integrator because the
 * narrow phase FUN_00107950 reads it and, on the frame a body is promoted,
 * the integrator has not run yet. */
static void b3p_build_inv_frame(B3RigidBody* rb) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) rb->inv_frame[i][j] = rb->frame[i][j];
    float t;
    t = rb->inv_frame[0][1]; rb->inv_frame[0][1] = rb->inv_frame[1][0];
    rb->inv_frame[1][0] = t;
    t = rb->inv_frame[0][2]; rb->inv_frame[0][2] = rb->inv_frame[2][0];
    rb->inv_frame[2][0] = t;
    t = rb->inv_frame[1][2]; rb->inv_frame[1][2] = rb->inv_frame[2][1];
    rb->inv_frame[2][1] = t;
    float p[4];
    for (int j = 0; j < 4; j++)
        p[j] = rb->inv_frame[3][0]*rb->inv_frame[0][j]
             + rb->inv_frame[3][1]*rb->inv_frame[1][j]
             + rb->inv_frame[3][2]*rb->inv_frame[2][j];
    for (int j = 0; j < 4; j++) rb->inv_frame[3][j] = -p[j];
}

static void b3p_integrate(B3RigidBody* rb, float mass, float com, float dt) {
    /* DAT_0040A8A0 = (0, -20, 0, 0), scaled by (1, mass, 1, -) */
    const float g[4] = { 0.0f, -20.0f * mass, 0.0f, 0.0f };
    float pt[4], r[3];
    for (int i = 0; i < 4; i++) {
        pt[i] = rb->frame[1][i] * com + rb->frame[3][i];
        rb->force_acc[i] += g[i];
    }
    for (int i = 0; i < 3; i++) r[i] = pt[i] - rb->frame[3][i];
    rb->torque_acc[0] += r[1]*g[2] - r[2]*g[1];
    rb->torque_acc[1] += r[2]*g[0] - r[0]*g[2];
    rb->torque_acc[2] += r[0]*g[1] - r[1]*g[0];

    const float dtv[4] = { dt, dt, dt, 0.0f };
    float vel4[4] = { rb->vel[0], rb->vel[1], rb->vel[2], rb->vel[3] };
    for (int i = 0; i < 4; i++) {
        rb->imp_force[i] += rb->force_acc[i] * dtv[i];
        rb->imp_torque[i] += rb->torque_acc[i] * dtv[i];
        rb->force_acc[i] = 0.0f;
        rb->torque_acc[i] = 0.0f;
    }
    for (int i = 0; i < 4; i++) {
        vel4[i] += rb->imp_force[i] / mass;
        rb->imp_force[i] = 0.0f;
    }
    if (vel4[1] > 120.0f) vel4[1] = 120.0f;
    for (int i = 0; i < 4; i++) {
        rb->angmom[i] += rb->imp_torque[i];
        rb->imp_torque[i] = 0.0f;
    }
    for (int j = 0; j < 4; j++)
        rb->omega[j] = rb->angmom[0] * rb->inv_inertia_world[0][j]
                     + rb->angmom[1] * rb->inv_inertia_world[1][j]
                     + rb->angmom[2] * rb->inv_inertia_world[2][j];
    const float w2 = v_dot3(rb->omega, rb->omega);
    if (w2 > 10000.0f) {
        b3p_norm4(rb->omega);
        const float s = (1.0f / sqrtf(w2)) * 100.0f;
        for (int i = 0; i < 4; i++) rb->omega[i] *= s;
        for (int i = 0; i < 4; i++) rb->angmom[i] *= 0.95f;
    }
    float rr[4];
    for (int i = 0; i < 4; i++) {
        rb->frame[3][i] += vel4[i] * dtv[i];
        rr[i] = rb->omega[i] * dtv[i];
    }
    for (int row = 2; row >= 0; row--) {
        float c[4];
        b3p_cross4(rb->frame[row], rr, c);
        for (int i = 0; i < 4; i++) rb->frame[row][i] -= c[i];
    }
    b3p_orthonormalize(rb->frame);
    for (int i = 0; i < 4; i++) {
        rb->frame[3][i] += rb->deflection[i];
        rb->deflection[i] = 0.0f;
    }
    float Rt[3][4], tmp[3][4];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            Rt[i][j] = rb->frame[j][i];
    for (int i = 0; i < 3; i++) Rt[i][3] = 0.0f;
    b3p_mat_mul3(rb->inv_inertia_body, Rt, tmp);
    b3p_mat_mul3(rb->frame, tmp, rb->inv_inertia_world);
    b3p_build_inv_frame(rb);
    /* FUN_000FFC80: refresh +0xBC speed and +0xC0 unit travel direction */
    for (int i = 0; i < 3; i++) rb->vel[i] = vel4[i];
    rb->vel[3] = vel4[3];
    const float ls = v_dot3(vel4, vel4);
    if (ls < B3P_EPS2) {
        rb->vel[3] = 0.0f;
        for (int i = 0; i < 4; i++) rb->dir[i] = rb->frame[2][i];
    } else {
        float l = sqrtf(ls);
        rb->vel[3] = l;
        for (int i = 0; i < 3; i++) rb->dir[i] = vel4[i] / l;
        rb->dir[3] = 0.0f;
    }
}

/* ---- GAME <-> HARNESS mirror -------------------------------------------
 * The vehicle pipeline's B3RigidBody lives in GAME space; this module (like
 * the collision world and the renderer) lives in HARNESS/GL space, which is
 * game space reflected through S = diag(1,1,-1) (RE_NOTES 12; the same
 * reflection burnout3_collision.c's loader and harness_ground_probe apply).
 * Under an improper map a true vector transforms as S*v while a PSEUDO-vector
 * (omega, torque) transforms as -S*w, because S(a x b) = -(Sa x Sb); the
 * inertia tensor transforms as S I S, i.e. the entries with exactly one z
 * index flip sign. */
static void b3p_mirror_rb(const B3RigidBody* in, B3RigidBody* out) {
    memset(out, 0, sizeof *out);
    for (int r = 0; r < 3; r++) {
        out->frame[r][0] =  in->frame[r][0];
        out->frame[r][1] =  in->frame[r][1];
        out->frame[r][2] = -in->frame[r][2];
    }
    out->frame[3][0] =  in->frame[3][0];
    out->frame[3][1] =  in->frame[3][1];
    out->frame[3][2] = -in->frame[3][2];
    out->frame[3][3] = 1.0f;
    out->vel[0] =  in->vel[0]; out->vel[1] =  in->vel[1];
    out->vel[2] = -in->vel[2]; out->vel[3] =  in->vel[3];
    out->dir[0] =  in->dir[0]; out->dir[1] =  in->dir[1];
    out->dir[2] = -in->dir[2]; out->dir[3] =  in->dir[3];
    out->omega[0] = -in->omega[0];
    out->omega[1] = -in->omega[1];
    out->omega[2] =  in->omega[2];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out->inv_inertia_world[r][c] =
                ((r == 2) != (c == 2)) ? -in->inv_inertia_world[r][c]
                                       :  in->inv_inertia_world[r][c];
}

/* FUN_00106500 [C]: an impulse at a world point.  +0x110 += J and
 * +0x120 += (P - pos) x J. */
static void b3p_impulse_at(B3RigidBody* rb, const float imp[4],
                           const float pt[4]) {
    float r[3];
    for (int k = 0; k < 3; k++) r[k] = pt[k] - rb->frame[3][k];
    rb->imp_force[0] += imp[0];
    rb->imp_force[1] += imp[1];
    rb->imp_force[2] += imp[2];
    rb->imp_force[3] += imp[3];
    rb->imp_torque[0] += r[1] * imp[2] - r[2] * imp[1];
    rb->imp_torque[1] += r[2] * imp[0] - r[0] * imp[2];
    rb->imp_torque[2] += r[0] * imp[1] - r[1] * imp[0];
}

/* FUN_001066A0 [C]: point velocity = omega x (pt - pos) + vel. */
static void b3p_point_vel(const B3RigidBody* rb, const float pt[4],
                          float out[4]) {
    float r[3];
    for (int k = 0; k < 3; k++) r[k] = pt[k] - rb->frame[3][k];
    out[0] = rb->omega[1] * r[2] - rb->omega[2] * r[1] + rb->vel[0];
    out[1] = rb->omega[2] * r[0] - rb->omega[0] * r[2] + rb->vel[1];
    out[2] = rb->omega[0] * r[1] - rb->omega[1] * r[0] + rb->vel[2];
    out[3] = rb->vel[3];
}

/* FUN_0010F8D0 [C]: the two-body contact impulse.  Identical to the car
 * agent's b3_carcol_mutual_impulse (docs/RE_CARCOL.md); kept local so this
 * module does not depend on the car-collision translation unit.  Writes
 * n * -|j| and returns |j|. */
static float b3p_mutual_impulse(const B3RigidBody* rb1, float m1,
                                const B3RigidBody* rb3, float m3,
                                const float pt3[4], const float pt1[4],
                                const float vrel[4], const float n[4],
                                float restitution, float out[4]) {
    float r3[3], r1[3], c3[3], c1[3], a3[3], a1[3];
    for (int k = 0; k < 3; k++) {
        r3[k] = pt3[k] - rb3->frame[3][k];
        r1[k] = pt1[k] - rb1->frame[3][k];
    }
    c3[0] = r3[1]*n[2] - r3[2]*n[1];
    c3[1] = r3[2]*n[0] - r3[0]*n[2];
    c3[2] = r3[0]*n[1] - r3[1]*n[0];
    c1[0] = r1[1]*n[2] - r1[2]*n[1];
    c1[1] = r1[2]*n[0] - r1[0]*n[2];
    c1[2] = r1[0]*n[1] - r1[1]*n[0];
    for (int k = 0; k < 3; k++) {
        a3[k] = rb3->inv_inertia_world[0][k]*c3[0]
              + rb3->inv_inertia_world[1][k]*c3[1]
              + rb3->inv_inertia_world[2][k]*c3[2];
        a1[k] = rb1->inv_inertia_world[0][k]*c1[0]
              + rb1->inv_inertia_world[1][k]*c1[1]
              + rb1->inv_inertia_world[2][k]*c1[2];
    }
    float dx = (a3[1]*r3[2] - a3[2]*r3[1]) + (a1[1]*r1[2] - a1[2]*r1[1]);
    float dy = (a3[2]*r3[0] - a3[0]*r3[2]) + (a1[2]*r1[0] - a1[0]*r1[2]);
    float dz = (a3[0]*r3[1] - a3[1]*r3[0]) + (a1[0]*r1[1] - a1[1]*r1[0]);
    float den = 1.0f/m1 + 1.0f/m3 + dx*n[0] + dy*n[1] + dz*n[2];
    float num = -(restitution + 1.0f) * (n[0]*vrel[0] + n[1]*vrel[1]
                                       + n[2]*vrel[2]);
    float j = fabsf(num / den);
    for (int k = 0; k < 4; k++) out[k] = n[k] * (-j);
    return j;
}

/* ---- texture ---------------------------------------------------------- */
static unsigned load_tex(const char* dir, const char* name) {
    if (!name || !name[0]) return 0;
    char path[768];
    SDL_Surface* s = NULL;
    snprintf(path, sizeof path, "%s/textures/%s.png", dir, name);
    s = IMG_Load(path);
    if (!s) {
        snprintf(path, sizeof path, "build/textures/%s.png", name);
        s = IMG_Load(path);
    }
    if (!s) return 0;
    SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s);
    if (!c) return 0;
    unsigned id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
    SDL_FreeSurface(c);
    return id;
}

/* ---- load ------------------------------------------------------------- */
void b3_props_shutdown(void) {
    if (g_model) {
        for (int i = 0; i < g_nmodel; i++) {
            if (g_model[i].list) glDeleteLists(g_model[i].list, 1);
            if (g_model[i].tex) glDeleteTextures(1, &g_model[i].tex);
        }
    }
    free(g_model); free(g_inst); free(g_vtx); free(g_idx);
    g_model = NULL; g_inst = NULL; g_vtx = NULL; g_idx = NULL;
    g_nmodel = g_ninst = g_live = g_ready = 0;
    for (int i = 0; i < B3P_MAX_LIVE; i++) {
        memset(&g_body[i], 0, sizeof g_body[i]);
        g_body[i].owner = -1;
        g_body[i].lru_key = -1.0f;
    }
    g_nvtx = g_nidx = 0;
}

int b3_props_load(const char* track_dir) {
    b3_props_shutdown();
    if (!track_dir || !track_dir[0]) return 0;
    snprintf(g_dir, sizeof g_dir, "%s", track_dir);

    char path[768];
    snprintf(path, sizeof path, "%s/props.bin", track_dir);
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("props: no %s (props disabled)\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x30) { fclose(f); return 0; }
    unsigned char* d = (unsigned char*)malloc((size_t)sz);
    if (!d) { fclose(f); return 0; }
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); fclose(f); return 0; }
    fclose(f);

    if (rd_u32(d) != B3P_MAGIC || rd_u32(d + 4) != 1) {
        printf("props: %s is not a version-1 B3PP file\n", path);
        free(d);
        return 0;
    }
    g_nmodel   = (int)rd_u32(d + 0x08);
    g_ninst    = (int)rd_u32(d + 0x0C);
    g_nvtx     = rd_u32(d + 0x10);
    g_nidx     = rd_u32(d + 0x14);
    unsigned om = rd_u32(d + 0x18), oi = rd_u32(d + 0x1C);
    unsigned ov = rd_u32(d + 0x20), ox = rd_u32(d + 0x24);
    if (g_nmodel <= 0 || g_ninst <= 0) { free(d); return 0; }

    g_model = (B3PropModel*)calloc((size_t)g_nmodel, sizeof *g_model);
    g_inst  = (B3PropInst*)calloc((size_t)g_ninst, sizeof *g_inst);
    g_vtx   = (float*)calloc(g_nvtx ? g_nvtx * 8 : 1, sizeof(float));
    g_idx   = (unsigned short*)calloc(g_nidx ? g_nidx : 1, sizeof(unsigned short));
    if (!g_model || !g_inst || !g_vtx || !g_idx) { free(d); b3_props_shutdown(); return 0; }

    for (int i = 0; i < g_nmodel; i++) {
        const unsigned char* r = d + om + (size_t)i * B3P_MODEL;
        B3PropModel* m = &g_model[i];
        for (int k = 0; k < 3; k++) {
            m->bb_min[k] = rd_f32(r + k * 4);
            m->bb_max[k] = rd_f32(r + 0x0C + k * 4);
        }
        /* game -> harness: the Z axis is reflected (RE_NOTES 12), so the
         * bbox's z bounds swap sign and swap roles. */
        float zn = -m->bb_max[2], zx = -m->bb_min[2];
        m->bb_min[2] = zn; m->bb_max[2] = zx;
        m->first_vertex = rd_u32(r + 0x18);
        m->n_vertex     = rd_u32(r + 0x1C);
        m->first_index  = rd_u32(r + 0x20);
        m->n_index      = rd_u32(r + 0x24);
        m->prop_class   = rd_u32(r + 0x28);
        m->mass         = rd_f32(r + 0x2C);
        m->radius       = rd_f32(r + 0x30);
        m->lod_near     = rd_f32(r + 0x34);
        m->lod_far      = rd_f32(r + 0x38);
        m->mat_flags    = rd_u32(r + 0x3C);
        memcpy(m->texture, r + 0x40, 31);
        m->texture[31] = 0;
    }

    for (unsigned i = 0; i < g_nvtx; i++) {
        const unsigned char* v = d + ov + (size_t)i * 0x20;
        float* o = g_vtx + i * 8;
        o[0] = rd_f32(v + 0);
        o[1] = rd_f32(v + 4);
        o[2] = -rd_f32(v + 8);        /* game -> harness Z reflection */
        o[3] = rd_f32(v + 12);
        o[4] = rd_f32(v + 16);
        o[5] = -rd_f32(v + 20);
        o[6] = rd_f32(v + 24);
        o[7] = rd_f32(v + 28);
    }
    for (unsigned i = 0; i < g_nidx; i++)
        g_idx[i] = (unsigned short)(d[ox + i * 2] | (d[ox + i * 2 + 1] << 8));

    for (int i = 0; i < g_ninst; i++) {
        const unsigned char* r = d + oi + (size_t)i * B3P_INST;
        B3PropInst* p = &g_inst[i];
        float m[16];
        for (int k = 0; k < 16; k++) m[k] = rd_f32(r + k * 4);
        /* The four `w` slots are the instance's baked half-range colour, not
         * transform (FUN_0011A020 saves/restores them at 0x0011A03A). Lift
         * them out and make the matrix affine. */
        p->tint[0] = m[3] * 2.0f;
        p->tint[1] = m[7] * 2.0f;
        p->tint[2] = m[11] * 2.0f;
        m[3] = m[7] = m[11] = 0.0f;
        m[15] = 1.0f;
        /* game -> harness Z reflection of the whole transform, conjugated so
         * it composes with the reflected local vertices: negate the elements
         * with exactly one z index. */
        m[2] = -m[2]; m[6] = -m[6]; m[8] = -m[8]; m[9] = -m[9]; m[14] = -m[14];
        memcpy(p->base, m, sizeof m);
        memcpy(p->cur, m, sizeof m);
        p->model      = rd_u32(r + 0x40);
        p->prop_class = rd_u32(r + 0x44);
        p->unit       = rd_u32(r + 0x48);
        if ((int)p->model >= g_nmodel) p->model = 0;
        p->state = B3P_REST;
        p->body = -1;
    }
    free(d);

    /* Textures + one display list per model (the traffic-section pattern). */
    for (int i = 0; i < g_nmodel; i++) {
        B3PropModel* m = &g_model[i];
        m->tex = load_tex(g_dir, m->texture);
        if (!m->n_index) continue;
        m->list = glGenLists(1);
        if (!m->list) continue;
        glNewList(m->list, GL_COMPILE);
        glBegin(GL_TRIANGLES);
        for (unsigned k = 0; k < m->n_index; k++) {
            unsigned vi = m->first_vertex + g_idx[m->first_index + k];
            if (vi >= g_nvtx) continue;
            const float* v = g_vtx + vi * 8;
            glTexCoord2f(v[6], v[7]);
            glVertex3f(v[0], v[1], v[2]);
        }
        glEnd();
        glEndList();
    }

    /* Ground height under every prop, once -- the props sit on authored
     * ground and a knocked one has to come back down to it. */
    if (b3_collision_ready()) {
        for (int i = 0; i < g_ninst; i++) {
            B3PropInst* p = &g_inst[i];
            float h, n[3];
            if (b3_ground_probe(p->base[12], p->base[13] + 1.0f, p->base[14],
                                &h, n) >= 0) {
                p->ground_y = h;
                p->has_ground = 1;
            } else {
                p->ground_y = p->base[13];
            }
        }
    } else {
        for (int i = 0; i < g_ninst; i++) g_inst[i].ground_y = g_inst[i].base[13];
    }

    /* FUN_00119F40 @0x00119F83/0x00119F90: every pool body starts with
     * +0x220 = 0 (no owner) and +0x224 = -1.0 -- an EMPTY slot. */
    for (int i = 0; i < B3P_MAX_LIVE; i++) {
        memset(&g_body[i], 0, sizeof g_body[i]);
        g_body[i].owner = -1;
        g_body[i].lru_key = -1.0f;
    }
    g_live = 0;
    g_clock = 0.0f;

    g_ready = 1;
    int cones = 0;
    for (int i = 0; i < g_ninst; i++)
        if (g_inst[i].prop_class == 1) cones++;
    int grounded = 0;
    for (int i = 0; i < g_ninst; i++) grounded += g_inst[i].has_ground;
    printf("props: %s -> %d models, %d instances (%d cones, %d on ground)\n",
           path, g_nmodel, g_ninst, cones, grounded);
    return g_ninst;
}

int b3_props_ready(void) { return g_ready; }
int b3_props_count(void) { return g_ninst; }
int b3_props_live(void) { return g_live; }

int b3_props_class_of(int i) {
    if (!g_ready || i < 0 || i >= g_ninst) return -1;
    return (int)g_inst[i].prop_class;
}
float b3_props_mass_of(int i) {
    if (!g_ready || i < 0 || i >= g_ninst) return 0.0f;
    return g_model[g_inst[i].model].mass;
}

/* The object class FUN_00112E70's port sees.  RECOVERED [C]: a static prop's
 * collision handle is type 5 (FUN_00110420 @0x00110A19) and a knocked one is
 * type 6 (FUN_00114730 @0x0011478B); FUN_0010FBC0's jump table @0x0010FC04
 * sends both to the class-6 arm @0x0010FBFC, and DAT_0039AE50 row 6 is all
 * zeros -- no prop of any size crashes any car.  That is the law, and it is
 * why you can plough a whole cone field without a scratch.
 *
 * GLUE (default OFF): B3_PROP_CRASH_KG=<kg> promotes props at or above that
 * recovered mass to class 2, i.e. makes them behave like a retail type-3 prop
 * ENTITY, whose row [2][0] = 1 does crash a racecar.  Retail ships no such
 * promotion for static.dat props; the knob exists so a future entity-backed
 * prop family can be switched on and measured. */
int b3_props_object_class(int i)
{
    static float heavy_kg = -1.0f;
    float m;
    if (heavy_kg < 0.0f) {
        const char* e = getenv("B3_PROP_CRASH_KG");
        heavy_kg = (e && *e) ? (float)atof(e) : 0.0f;
    }
    if (!g_ready || i < 0 || i >= g_ninst) return 6;
    m = g_model[g_inst[i].model].mass;
    if (heavy_kg > 0.0f && m >= heavy_kg) return 2;   /* GLUE: type-3 entity */
    return 6;                                        /* [C] type 5/6 -> 6   */
}

void b3_props_reset(void) {
    if (!g_ready) return;
    for (int i = 0; i < g_ninst; i++) {
        B3PropInst* p = &g_inst[i];
        memcpy(p->cur, p->base, sizeof p->cur);
        p->state = B3P_REST;
        p->body = -1;
    }
    for (int i = 0; i < B3P_MAX_LIVE; i++) {
        memset(&g_body[i], 0, sizeof g_body[i]);
        g_body[i].owner = -1;
        g_body[i].lru_key = -1.0f;
    }
    g_live = 0;
    g_clock = 0.0f;
}

/* ---- knock ------------------------------------------------------------ */
/* FUN_00109BB0 @0x00109BB9..0x00109CC8 -> FUN_00109190 [C]: the body inverse
 * inertia is the diagonal built from the bbox half-extents and the mass. */
static void b3p_set_inertia(B3RigidBody* rb, const float bbmax[4],
                            const float bbmin[4], float mass) {
    float e[3];
    for (int k = 0; k < 3; k++) {
        float a = bbmax[k], b = -bbmin[k];
        e[k] = a > b ? a : b;
    }
    float d[3];
    d[0] = 1.0f / ((e[1]*e[1] + e[2]*e[2]) * mass * 0.5f);
    d[1] = 1.0f / ((e[0]*e[0] + e[2]*e[2]) * mass * 0.5f);
    d[2] = 1.0f / ((e[0]*e[0] + e[1]*e[1]) * mass * 0.5f);
    memset(rb->inv_inertia_body, 0, sizeof rb->inv_inertia_body);
    for (int k = 0; k < 3; k++) rb->inv_inertia_body[k][k] = d[k];
    /* FUN_00109190's tail: the WORLD inverse inertia is R^t . I0 . R, built
     * immediately so the contact resolved in the same frame already has it
     * (FUN_00113890 promotes @0x0011393B then resolves @0x0011394E). */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            rb->inv_inertia_world[i][j] =
                rb->frame[0][i] * d[0] * rb->frame[0][j]
              + rb->frame[1][i] * d[1] * rb->frame[1][j]
              + rb->frame[2][i] * d[2] * rb->frame[2][j];
    for (int i = 0; i < 3; i++) rb->inv_inertia_world[i][3] = 0.0f;
}

/* FUN_00114730 [C]: first a free slot in the 16-body pool (the bitmask scan
 * @0x00114750..0x0011476D), otherwise recycle the body with the SMALLEST
 * +0x224 (@0x001147A0..0x001147C6) -- the oldest.  The recycled prop's world
 * slot is retyped to 8 @0x0011480C, and the dispatcher drops type-8 pairs
 * @0x00111D0B, so it stops colliding: B3P_SETTLED here. */
static int b3p_acquire(int self)
{
    for (int i = 0; i < B3P_MAX_LIVE; i++)
        if (g_body[i].owner < 0) { g_live++; return i; }
    int victim = -1;
    float best = 999999.0f;                    /* FUN_00114730's seed value */
    for (int i = 0; i < B3P_MAX_LIVE; i++) {
        if (g_body[i].owner == self) continue;
        if (g_body[i].lru_key < best) { best = g_body[i].lru_key; victim = i; }
    }
    if (victim < 0) return -1;
    int prev = g_body[victim].owner;
    if (prev >= 0 && prev < g_ninst) {
        g_inst[prev].state = B3P_SETTLED;      /* world slot -> type 8 */
        g_inst[prev].body = -1;
    }
    g_body[victim].owner = -1;
    return victim;
}

/* FUN_0011A020 @0x0011A0A5..0x0011A317 [C]: everything the body setup derives
 * from the model bbox, over an already-posed frame. */
static void b3p_body_setup(B3RigidBody* rb, const float bbmax[4],
                           const float bbmin[4], float* out_mass,
                           float* out_com, float* out_radius)
{
    /* +0x1CC = |bbmax|  @0x0011A0DE */
    float radius = sqrtf(bbmax[0]*bbmax[0] + bbmax[1]*bbmax[1]
                       + bbmax[2]*bbmax[2]);
    /* +0x1F0 = max(100, (bbmax.x-bbmin.x)*(bbmax.z-bbmin.z)*200) @0x0011A137
     * -- MAXSS @0x0011A17D, so the 100 wins on a NaN/tie the same way. */
    float dx = bbmax[0] - bbmin[0];
    float dz = bbmax[2] - bbmin[2];
    float area = dz * dx * B3P_MASS_PER_M2;
    float mass = area > B3P_MASS_MIN ? area : B3P_MASS_MIN;

    b3p_set_inertia(rb, bbmax, bbmin, mass);
    /* +0x1F4 = bbmax.y - (bbmax.y - bbmin.y)*0.5  @0x0011A2F8..0x0011A317 */
    float com = bbmax[1] - (bbmax[1] - bbmin[1]) * 0.5f;

    /* THE LAUNCH, @0x0011A1DD..0x0011A2F3 -> FUN_000FFC80.  Four PRNG draws;
     * the y term is (u*0.5), never negative, so a knocked prop always pops up.
     * FUN_000FFC80 SETS the velocity (it does not add), then refreshes +0xBC
     * speed and +0xC0 unit direction. */
    float r0 = (float)(b3p_rand01() - B3P_LAUNCH_HALF);
    float r1 = (float)(b3p_rand01() * B3P_LAUNCH_HALF);
    float r2 = (float)(b3p_rand01() - B3P_LAUNCH_HALF);
    float s  = (float)((b3p_rand01() + B3P_LAUNCH_ONE) * B3P_LAUNCH_SCALE);
    rb->vel[0] = r0 * s;
    rb->vel[1] = r1 * s;
    rb->vel[2] = r2 * s;
    float l2 = v_dot3(rb->vel, rb->vel);
    if (l2 < B3P_EPS2) {
        rb->vel[3] = 0.0f;
        for (int k = 0; k < 4; k++) rb->dir[k] = rb->frame[2][k];
    } else {
        float l = sqrtf(l2);
        rb->vel[3] = l;
        for (int k = 0; k < 3; k++) rb->dir[k] = rb->vel[k] / l;
        rb->dir[3] = 0.0f;
    }
    if (out_mass) *out_mass = mass;
    if (out_com) *out_com = com;
    if (out_radius) *out_radius = radius;
}

/* FUN_0011A330 [C] minus the harness ground stop: the two quadratic drag
 * terms, FUN_00109560, and the matrix-w restore @0x0011A434. */
static void b3p_body_step(B3RigidBody* rb, float mass, float com, float dt)
{
    /* force += dir * -(speed^2)          @0x0011A370, [0x003B16C0] */
    float f = B3P_LIN_DRAG * rb->vel[3] * rb->vel[3];
    for (int k = 0; k < 4; k++) rb->force_acc[k] += rb->dir[k] * f;
    /* torque += omega * (|omega| * -2)   @0x0011A3B4, [0x003B17F8] */
    float wl = sqrtf(v_dot3(rb->omega, rb->omega)) * B3P_ANG_DRAG;
    for (int k = 0; k < 4; k++) rb->torque_acc[k] += rb->omega[k] * wl;
    /* in_race = body+0x210 = 0, state6 = (body+0x215 == 6) = 1, so gravity is
     * applied at pos + up*com_height and therefore tumbles the prop. */
    b3p_integrate(rb, mass, com, dt);
    rb->frame[0][3] = rb->frame[1][3] = rb->frame[2][3] = 0.0f;
    rb->frame[3][3] = 1.0f;
}

/* FUN_00113960's arm for (car A, prop B) at a resolved contact [C].  Returns
 * |j|; `car_rb` is only written when the car is crashed (role != 2). */
static float b3p_contact(B3RigidBody* prb, float pmass,
                         B3RigidBody* car_rb, float cmass,
                         const float cp[4], const float n_in[4],
                         int car_crashed, float out_n[4], float out_imp[4])
{
    /* @0x00113B57: an un-crashed car (handle type 0/1/2) is FORCED to role 2
     * -- immovable -- so it takes no reaction at all. */
    int kindA = car_crashed ? 0 : 2;

    /* point velocities and v_rel = v_prop - v_car   @0x00113E9C */
    float vpa[4], vpb[4], vrel[4], n[4];
    b3p_point_vel(car_rb, cp, vpa);
    b3p_point_vel(prb, cp, vpb);
    for (int k = 0; k < 4; k++) vrel[k] = vpb[k] - vpa[k];
    for (int k = 0; k < 4; k++) n[k] = n_in[k];
    n[3] = 0.0f;
    /* @0x00113EFA..0x00113F49: bend the normal toward v_rel by -0.9 and
     * re-normalise, skipped when |v_rel|^2 < 2^-32 (FUN_0003B060). */
    if (v_dot3(vrel, vrel) >= B3P_EPS2) {
        float u[4] = { vrel[0], vrel[1], vrel[2], vrel[3] };
        float ul = sqrtf(v_dot3(u, u));
        for (int k = 0; k < 3; k++) u[k] /= ul;
        for (int k = 0; k < 3; k++) n[k] += u[k] * B3P_NORM_BLEND;
        float nl = sqrtf(v_dot3(n, n));
        if (nl < 1e-20f) { if (out_n) memcpy(out_n, n, sizeof n); return 0.0f; }
        for (int k = 0; k < 3; k++) n[k] /= nl;
    }
    if (out_n) memcpy(out_n, n, 4 * sizeof(float));

    /* @0x00113F78 FUN_0010F8D0 -- both masses and both inertias are in the
     * denominator even though only the prop moves. */
    float imp[4];
    float j = b3p_mutual_impulse(prb, pmass, car_rb, cmass,
                                 cp, cp, vrel, n, B3P_RESTITUTION, imp);
    if (out_imp) memcpy(out_imp, imp, sizeof imp);
    /* @0x00113F91: nothing is applied unless j > 0 */
    if (j > 0.0f) {
        float neg[4] = { -imp[0], -imp[1], -imp[2], -imp[3] };
        /* @0x00113FAD kindA == 2: the prop takes -J and the car nothing */
        b3p_impulse_at(prb, neg, cp);
        if (kindA != 2) b3p_impulse_at(car_rb, imp, cp);
    }
    return j;
}

/* FUN_0011A020 [C]: hand a fresh class-6 body to prop `inst`. */
static int b3p_promote(int inst)
{
    B3PropInst* p = &g_inst[inst];
    const B3PropModel* m = &g_model[p->model];
    int slot = b3p_acquire(inst);
    if (slot < 0) return -1;
    B3PropBody* b = &g_body[slot];
    memset(b, 0, sizeof *b);
    b->owner = inst;

    /* +0x204 -> the instance matrix itself (@0x0011A062) */
    mat_to_frame(p->cur, b->rb.frame);
    b->rb.frame[0][3] = b->rb.frame[1][3] = b->rb.frame[2][3] = 0.0f;
    b->rb.frame[3][3] = 1.0f;

    b3p_build_inv_frame(&b->rb);

    float bbmax[4] = { m->bb_max[0], m->bb_max[1], m->bb_max[2], 0.0f };
    float bbmin[4] = { m->bb_min[0], m->bb_min[1], m->bb_min[2], 0.0f };
    /* +0x1D0 = bbmax, +0x1E0 = bbmin  @0x0011A0A8/@0x0011A0AF -- the OBB the
     * narrow phase FUN_00107950 clips the surface polygons against. */
    for (int k = 0; k < 3; k++) { b->bbmax[k] = bbmax[k]; b->bbmin[k] = bbmin[k]; }
    b3p_body_setup(&b->rb, bbmax, bbmin, &b->mass, &b->com_height, &b->radius);
    /* +0x224 = clock + 10  @0x0011A19E */
    b->lru_key = g_clock + B3P_LRU_OFFSET;

    p->body = (short)slot;
    p->state = B3P_KNOCKED;
    if (getenv("B3_PROP_TRACE"))
        printf("[propknock] t=%.2f inst %d -> body %d mass %.0f com %.2f "
               "launch (%.2f %.2f %.2f) live %d\n", g_clock, inst, slot,
               b->mass, b->com_height, b->rb.vel[0], b->rb.vel[1],
               b->rb.vel[2], g_live);
    return slot;
}

/* ---- differential test surface (see burnout3_props.h) ------------------- */
void b3_props_test_body_setup(const float frame[4][4], const float bbmax[4],
                              const float bbmin[4], unsigned rng_state,
                              unsigned rng_inc, B3RigidBody* out_rb,
                              float* out_mass, float* out_com_height,
                              float* out_radius) {
    memset(out_rb, 0, sizeof *out_rb);
    memcpy(out_rb->frame, frame, sizeof out_rb->frame);
    unsigned ss = g_rng_state, si = g_rng_inc;
    b3_props_seed(rng_state, rng_inc);
    b3p_body_setup(out_rb, bbmax, bbmin, out_mass, out_com_height, out_radius);
    b3_props_seed(ss, si);
}
void b3_props_test_body_step(B3RigidBody* rb, float mass, float com_height,
                             float dt) {
    b3p_body_step(rb, mass, com_height, dt);
}
float b3_props_test_contact(B3RigidBody* prop_rb, float prop_mass,
                            const B3RigidBody* car_rb, float car_mass,
                            const float point[4], const float normal[4],
                            float out_normal[4], float out_imp[4]) {
    B3RigidBody car = *car_rb;
    return b3p_contact(prop_rb, prop_mass, &car, car_mass, point, normal, 0,
                       out_normal, out_imp);
}

/* FUN_0011A330 [C], the class-6 body vtable's slot +0x00 (vtable 0x003B1120),
 * driven once per allocated body per frame by the collision manager
 * FUN_00110AF0 @0x00110FB4 with the frame dt.  The WHOLE update. */
void b3_props_update(float dt) {
    if (!g_ready || dt <= 0.0f) return;
    if (dt > 0.1f) dt = 0.1f;
    g_clock += dt;

    static int ballistic = -1;
    if (ballistic < 0) {
        const char* e = getenv("B3_PROP_BALLISTIC");
        ballistic = (e && *e && *e != '0') ? 1 : 0;
    }

    for (int s = 0; s < B3P_MAX_LIVE; s++) {
        B3PropBody* b = &g_body[s];
        if (b->owner < 0) continue;
        B3PropInst* p = &g_inst[b->owner];
        B3RigidBody* rb = &b->rb;

        /* ---- vtable slot +0x10, FUN_0011A490 -> FUN_00109EA0 -------------
         * Retail's collision manager runs this BEFORE the class's own update
         * slot, so the contact impulse lands in +0x110/+0x120 and the
         * push-out in +0x130 and the SAME frame's FUN_00109560 consumes all
         * three.  Doing it the other way round (what this file used to do)
         * costs a frame of lag and lets the body integrate through the
         * surface first, which is exactly the reported clipping. The same
         * gathered collision soup feeds live cars and wrecks: a knocked prop
         * must not be reduced to a single downward ground plane. */
        if (!ballistic) {
            int surf = -1;
            int hit = 0;
            int slept = 0;
            float h = 0.0f, nn[3] = {0.0f, 1.0f, 0.0f};
            if (b3_collision_ready()) {
                B3CollisionPoly soup[32];
                float half[3];
                float velocity[3] = {rb->vel[0], rb->vel[1], rb->vel[2]};
                for (int axis = 0; axis < 3; axis++) {
                    float lo = fabsf(b->bbmin[axis]);
                    float hi = fabsf(b->bbmax[axis]);
                    half[axis] = (lo > hi ? lo : hi) + 0.5f;
                }
                int nsoup = b3_collision_gather_walls(rb->frame[3], half,
                                                       velocity, 1.1f, soup,
                                                       (int)(sizeof(soup)
                                                             / sizeof(soup[0])));
                for (int poly = 0; poly < nsoup; poly++) {
                    B3WorldContact ct;
                    B3WorldContactResult res;
                    if (!b3_rigid_body_obb_plane_contact(
                            rb, b->bbmin, b->bbmax, soup[poly].v0,
                            soup[poly].normal, &ct))
                        continue;
                    b3_rigid_body_world_contact(rb, b->mass, 6, 0,
                                                B3P_WORLD_RESTITUTION, &ct,
                                                &res);
                    hit++;
                    if (res.sleep) slept = 1;
                }
                // Telemetry only; collision response above already used the
                // complete gathered soup.
                surf = b3_ground_probe(rb->frame[3][0],
                                       rb->frame[3][1] + b->radius + 1.0f,
                                       rb->frame[3][2], &h, nn);
            }
            if (!hit && surf < 0 && p->has_ground) {
                const float ppt[3] = {rb->frame[3][0], p->ground_y,
                                       rb->frame[3][2]};
                B3WorldContact ct;
                B3WorldContactResult res;
                if (b3_rigid_body_obb_plane_contact(rb, b->bbmin, b->bbmax,
                                                     ppt, nn, &ct)) {
                    b3_rigid_body_world_contact(rb, b->mass, 6, 0,
                                                B3P_WORLD_RESTITUTION, &ct,
                                                &res);
                    hit = 1;
                    slept = res.sleep;
                }
                h = p->ground_y;
                surf = 0;
            }
            if (slept) b->frozen = 1;
            if (getenv("B3_PROP_TRACE")) {
                static float next_at = 0.0f;
                if (g_clock >= next_at) {
                    next_at = g_clock + 1.0f;
                    printf("[propgnd] t=%.1f body %d inst %d pos=(%.2f %.3f "
                           "%.2f) ground=%.3f dy=%+.3f hit=%d |v|=%.2f "
                           "frozen=%d\n",
                           g_clock, s, b->owner, rb->frame[3][0],
                           rb->frame[3][1], rb->frame[3][2],
                           surf >= 0 ? h : -999.0f,
                           surf >= 0 ? rb->frame[3][1] - h : 0.0f,
                           hit, rb->vel[3], b->frozen);
                }
            }
        }

        b3p_body_step(rb, b->mass, b->com_height, dt);

        /* @0x0011A434..0x0011A45B: the four matrix w slots are restored after
         * the integrator -- they carry the instance colour, not transform. */
        rb->frame[0][3] = rb->frame[1][3] = rb->frame[2][3] = 0.0f;
        rb->frame[3][3] = 1.0f;
        frame_to_mat(rb->frame, p->cur);
    }
}

int b3_props_body_state(int instance, B3RigidBody* out_rb, float* out_mass,
                        float* out_com_height, float* out_lru_key) {
    if (!g_ready || instance < 0 || instance >= g_ninst) return 0;
    int s = g_inst[instance].body;
    if (s < 0 || s >= B3P_MAX_LIVE || g_body[s].owner != instance) return 0;
    if (out_rb) *out_rb = g_body[s].rb;
    if (out_mass) *out_mass = g_body[s].mass;
    if (out_com_height) *out_com_height = g_body[s].com_height;
    if (out_lru_key) *out_lru_key = g_body[s].lru_key;
    return 1;
}

static int b3p_collide(int car, B3RigidBody* car_rb, B3RigidBody* game_rb,
                       float car_mass, const float half_ext[3],
                       int car_crashed, B3PropHit* out, int max_out);

int b3_props_collide_car(int car, const float pos[3], const float vel[3],
                         float yaw, const float half_ext[3],
                         B3PropHit* out, int max_out)
{
    /* Compatibility entry: synthesise the car body FUN_00113960 would have
     * had.  Zero omega, unit inverse inertia and a nominal mass -- prefer
     * b3_props_collide_rb(), which gets the real numbers. */
    B3RigidBody rb;
    memset(&rb, 0, sizeof rb);
    if (!pos) return 0;
    float fw[3] = { sinf(yaw), 0.0f, -cosf(yaw) };
    rb.frame[0][0] = -fw[2]; rb.frame[0][2] = fw[0];
    rb.frame[1][1] = 1.0f;
    rb.frame[2][0] = fw[0];  rb.frame[2][2] = fw[2];
    rb.frame[3][0] = pos[0]; rb.frame[3][1] = pos[1]; rb.frame[3][2] = pos[2];
    rb.frame[3][3] = 1.0f;
    if (vel) {
        rb.vel[0] = vel[0]; rb.vel[1] = vel[1]; rb.vel[2] = vel[2];
        rb.vel[3] = v_len(vel);
    }
    for (int k = 0; k < 3; k++) rb.inv_inertia_world[k][k] = 1.0f / 1800.0f;
    /* the frame above is already HARNESS, so hand it over unmirrored */
    return b3p_collide(car, &rb, NULL, B3P_CAR_MASS_FALLBACK, half_ext, 0,
                       out, max_out);
}

int b3_props_collide_rb(int car, B3RigidBody* car_rb, float car_mass,
                        const float half_ext[3], int car_crashed,
                        B3PropHit* out, int max_out)
{
    /* car_rb is the pipeline's GAME-space body; mirror it into harness space,
     * run the solver there, and mirror any reaction back (only a CRASHED car
     * gets one -- an un-crashed one is role 2, @0x00113B57). */
    B3RigidBody h;
    if (!car_rb) return 0;
    b3p_mirror_rb(car_rb, &h);
    return b3p_collide(car, &h, car_crashed ? car_rb : NULL, car_mass,
                       half_ext, car_crashed, out, max_out);
}

/* `car_rb` HARNESS space; `game_rb` non-NULL means "mirror the reaction back
 * onto this game-space body when the solver moves the car". */
static int b3p_collide(int car, B3RigidBody* car_rb, B3RigidBody* game_rb,
                       float car_mass, const float half_ext[3],
                       int car_crashed, B3PropHit* out, int max_out)
{
    if (!g_ready || !car_rb) return 0;
    int nout = 0;
    if (car_mass < 1.0f) car_mass = B3P_CAR_MASS_FALLBACK;
    float car_imp0[4], car_tor0[4];
    memcpy(car_imp0, car_rb->imp_force, sizeof car_imp0);
    memcpy(car_tor0, car_rb->imp_torque, sizeof car_tor0);

    const float* pos = car_rb->frame[3];
    const float* vel = car_rb->vel;
    /* harness heading straight off the body frame (row 2 = at, row 0 = right) */
    float fw[3] = { car_rb->frame[2][0], 0.0f, car_rb->frame[2][2] };
    {
        float l = sqrtf(fw[0]*fw[0] + fw[2]*fw[2]);
        if (l > 1e-6f) { fw[0] /= l; fw[2] /= l; } else { fw[0] = 0.0f; fw[2] = 1.0f; }
    }
    float rt[3] = { -fw[2], 0.0f, fw[0] };
    /* B3_PROP_PROBE: once-a-second "where is the nearest prop" line, for
     * aiming the headless capture runs at a cone field. */
    if (getenv("B3_PROP_PROBE") && car == 0) {
        static float next = 0.0f;
        if (g_clock >= next) {
            next = g_clock + 1.0f;
            float dd = 0.0f;
            int ni = b3_props_nearest(pos, &dd);
            printf("[propprobe] t=%.1f car=(%.1f %.1f %.1f) nearest=%d d=%.2f "
                   "live=%d\n", g_clock, pos[0], pos[1], pos[2], ni, dd,
                   g_live);
            fflush(stdout);
        }
    }
    float hx = half_ext ? fabsf(half_ext[0]) : 0.95f;
    float hz = half_ext ? fabsf(half_ext[2]) : 2.20f;
    if (hx < 0.4f) hx = 0.4f;
    if (hz < 0.8f) hz = 0.8f;
    float hy = half_ext ? fabsf(half_ext[1]) : 0.75f;
    if (hy < 0.5f) hy = 0.5f;

    for (int i = 0; i < g_ninst; i++) {
        B3PropInst* p = &g_inst[i];
        if (p->state == B3P_SETTLED) continue;
        const B3PropModel* m = &g_model[p->model];

        /* prop collision volume: a sphere at half the model's height,
         * radius = the larger horizontal half extent (retail keeps the same
         * two numbers on the body, +0x1CC/+0x1F4). */
        float rx = 0.5f * (m->bb_max[0] - m->bb_min[0]);
        float rz = 0.5f * (m->bb_max[2] - m->bb_min[2]);
        float rr = rx > rz ? rx : rz;
        if (rr < 0.15f) rr = 0.15f;
        float ch = m->bb_max[1] - (m->bb_max[1] - m->bb_min[1]) * 0.5f;
        float c[3] = {
            p->cur[12] + p->cur[4] * ch,
            p->cur[13] + p->cur[5] * ch,
            p->cur[14] + p->cur[6] * ch
        };

        float d[3] = { c[0] - pos[0], c[1] - pos[1], c[2] - pos[2] };
        /* cheap reject */
        float far2 = (hx + hz + rr + 2.0f);
        if (d[0] * d[0] + d[2] * d[2] > far2 * far2) continue;

        float lx = d[0] * rt[0] + d[2] * rt[2];
        float lz = d[0] * fw[0] + d[2] * fw[2];
        float ly = d[1];
        float qx = lx < -hx ? -hx : (lx > hx ? hx : lx);
        float qz = lz < -hz ? -hz : (lz > hz ? hz : lz);
        float qy = ly < -hy ? -hy : (ly > hy ? hy : ly);
        float ex = lx - qx, ez = lz - qz, ey = ly - qy;
        float dist2 = ex * ex + ez * ez + ey * ey;
        if (dist2 > rr * rr) continue;

        /* contact normal, prop <- car, in world */
        float dist = sqrtf(dist2);
        float nl[3];
        if (dist > 1e-4f) {
            nl[0] = ex / dist; nl[1] = ey / dist; nl[2] = ez / dist;
        } else {
            nl[0] = (lx >= 0.0f) ? 1.0f : -1.0f; nl[1] = 0.0f; nl[2] = 0.0f;
        }
        float nw[3] = {
            nl[0] * rt[0] + nl[2] * fw[0],
            nl[1],
            nl[0] * rt[2] + nl[2] * fw[2]
        };
        float nlen = v_len(nw);
        if (nlen < 1e-4f) continue;
        nw[0] /= nlen; nw[1] /= nlen; nw[2] /= nlen;

        /* The contact point: the closest point on the car box.  (Retail gets
         * it out of the hull narrow phase FUN_0010A9D0 / the OBB test
         * FUN_00108EF0; the geometry here is the harness stand-in, the
         * response below is the real one.) */
        float cp[4] = {
            pos[0] + rt[0] * qx + fw[0] * qz,
            pos[1] + qy,
            pos[2] + rt[2] * qx + fw[2] * qz,
            0.0f
        };

        /* Pre-promotion gate: retail's FUN_001084E0 (@0x00113901) is a pure
         * box overlap with no velocity test, but a prop resting against a
         * parked car must not be re-knocked every frame, so a closing-speed
         * check stands in until the prop has a body (once it has one the real
         * solver's own `j > 0` test @0x00113F91 does the job). */
        if (p->state == B3P_REST) {
            float vc[3] = { vel[0], vel[1], vel[2] };
            float vnc = vc[0]*nw[0] + vc[1]*nw[1] + vc[2]*nw[2];
            if (vnc <= 0.05f) continue;
            if (b3p_promote(i) < 0) continue;
        }
        B3PropBody* b = &g_body[p->body];
        B3RigidBody* prb = &b->rb;

        /* ---- FUN_00113960, the generic solver, for (car A, prop B) ------ */
        float nbent[4], imp[4], nin[4] = { nw[0], nw[1], nw[2], 0.0f };
        float j = b3p_contact(prb, b->mass, car_rb, car_mass, cp, nin,
                              car_crashed, nbent, imp);
        b->lru_key = g_clock + B3P_LRU_OFFSET;   /* keep the freshest alive */
        float vpa[4], vpb[4], vrel[4];
        b3p_point_vel(car_rb, cp, vpa);
        b3p_point_vel(prb, cp, vpb);
        for (int k = 0; k < 4; k++) vrel[k] = vpb[k] - vpa[k];

        /* Separation.  FUN_00114F30's mass split degenerates to "the prop
         * takes all of it" when the car is role 2 (@0x00113BFF/@0x00113C05),
         * which is the un-crashed case; deflection is consumed by the next
         * FUN_00109560 (+0x130). */
        float pen = rr - dist;
        if (pen > 0.0f)
            for (int k = 0; k < 3; k++) prb->deflection[k] += nw[k] * pen;

        /* v_rel for the crash trigger is the OBJECT's point velocity minus
         * the CAR's -- FUN_00112E70 @0x00113311 -- which is exactly `vrel`. */
        float vnc = -v_dot3(vrel, nbent);

        if (out && nout < max_out) {
            B3PropHit* h = &out[nout++];
            h->instance = i;
            h->model = (int)p->model;
            h->prop_class = (int)p->prop_class;
            h->car = car;
            h->obj_class = b3_props_object_class(i);
            h->mass = b->mass;
            h->radius = b->radius;
            h->point[0] = cp[0]; h->point[1] = cp[1]; h->point[2] = cp[2];
            h->normal[0] = nbent[0]; h->normal[1] = nbent[1];
            h->normal[2] = nbent[2];
            h->vrel[0] = vrel[0]; h->vrel[1] = vrel[1]; h->vrel[2] = vrel[2];
            h->closing_mph = fabsf(vnc) * B3P_MS_TO_MPH;
            h->impulse = j;
        }
        (void)m;
    }
    /* Mirror the car's share back into game space: J is a true vector (S*J),
     * the torque is a pseudo-vector (-S*T). */
    if (game_rb) {
        float dj[4], dt[4];
        for (int k = 0; k < 4; k++) {
            dj[k] = car_rb->imp_force[k] - car_imp0[k];
            dt[k] = car_rb->imp_torque[k] - car_tor0[k];
        }
        game_rb->imp_force[0] += dj[0];
        game_rb->imp_force[1] += dj[1];
        game_rb->imp_force[2] -= dj[2];
        game_rb->imp_torque[0] -= dt[0];
        game_rb->imp_torque[1] -= dt[1];
        game_rb->imp_torque[2] += dt[2];
    }
    return nout;
}

int b3_props_nearest(const float pos[3], float* out_dist) {
    if (!g_ready || !pos) return -1;
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < g_ninst; i++) {
        float dx = g_inst[i].cur[12] - pos[0];
        float dy = g_inst[i].cur[13] - pos[1];
        float dz = g_inst[i].cur[14] - pos[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    if (out_dist) *out_dist = best >= 0 ? sqrtf(bd) : 0.0f;
    return best;
}

/* ---- draw ------------------------------------------------------------- */
void b3_props_draw(void) {
    if (!g_ready) return;

    GLboolean had_tex = glIsEnabled(GL_TEXTURE_2D);
    GLboolean had_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean had_alpha = glIsEnabled(GL_ALPHA_TEST);
    GLboolean had_blend = glIsEnabled(GL_BLEND);

    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    /* The prop models are shader class 8/9 -- no D3DCOLOR register, so the
     * game never modulates them by a baked per-vertex colour.  What it does
     * have is the per-INSTANCE half-range colour in the transform's w slots,
     * which is what is used here (doubled, exactly as the world's stage-0
     * combiner doubles the vertex colour). */
    glDisable(GL_CULL_FACE);        /* the Z reflection flips the winding */

    int last_model = -1;
    for (int i = 0; i < g_ninst; i++) {
        const B3PropInst* p = &g_inst[i];
        const B3PropModel* m = &g_model[p->model];
        if (!m->list) continue;

        if ((int)p->model != last_model) {
            last_model = (int)p->model;
            glBindTexture(GL_TEXTURE_2D, m->tex);
            /* material flag +0x24 bit 0x010 = D3DRS_ALPHATESTENABLE with the
             * world's fixed GREATER 64/255 (extract_track.py "FLAG BITS"),
             * bit 0x001 = D3DRS_ALPHABLENDENABLE. */
            if (m->mat_flags & 0x010u) {
                glEnable(GL_ALPHA_TEST);
                glAlphaFunc(GL_GREATER, 64.0f / 255.0f);
            } else {
                glDisable(GL_ALPHA_TEST);
            }
            if (m->mat_flags & 0x001u) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                glDisable(GL_BLEND);
            }
        }
        glColor4f(p->tint[0], p->tint[1], p->tint[2], 1.0f);
        glPushMatrix();
        glMultMatrixf(p->cur);
        glCallList(m->list);
        glPopMatrix();
    }

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    if (had_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (had_alpha) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
    if (had_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (!had_tex) glDisable(GL_TEXTURE_2D);
}
