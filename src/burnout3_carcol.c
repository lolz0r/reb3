/* ===========================================================================
 * Car-vs-car collision -- 1:1 port of the retail chain.  See
 * burnout3_carcol.h for the map and docs/RE_CARCOL.md for the evidence.
 *
 * Every numeric constant below carries the image address it was read from.
 * Anything not from the binary is marked GLUE.
 * ===========================================================================
 */
#include <stdlib.h>
#include "burnout3_carcol.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * compiled-in constants (image addresses in comments)
 * ------------------------------------------------------------------------ */
#define K_EPS_LEN2      2.3283064e-10f  /* 0x003B191C  FUN_0003B060 */
#define K_DEDUP         1.0e-07f        /* 0x0039AACC  FUN_0010C0D0 */
#define K_RAY_LEN       100.0f          /* 0x003A2928  FUN_0010BE70 */
#define K_RAY_PAR       1.0e-04f        /* 0x003B188C  FUN_0010BE70 */
#define K_RAY_MIN       0.005f          /* 0x003B194C  FUN_0010BE70 */
#define K_MIN_SEP2      0.0009f         /* FUN_0010ABC0 immediate */
#define K_HALF          0.5f            /* 0x003B1684 */
#define K_TWO           2.0f            /* 0x003B1688 */
#define K_ONE           1.0f            /* 0x003B168C */
#define K_CP_Y_BIAS     0.1f            /* 0x003EBE40  FUN_001121F0 */
#define K_SLAM_EPS      1.52587891e-05f /* 0x00384208  FUN_001121F0 */
#define K_YAW_LOCK      2.0f            /* 0x003EBF68  FUN_001205E0 */

/* --------------------------------------------------------------------------
 * small vector / matrix helpers -- each is one real function
 * ------------------------------------------------------------------------ */

/* FUN_00011640: normalise using the xyz length, scaling all four lanes. */
static void v_norm4(float v[4]) {
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    float s = K_ONE / l;
    v[0] *= s; v[1] *= s; v[2] *= s; v[3] *= s;
}
/* FUN_0003B060: |v.xyz|^2 < 2.3283064e-10 */
static int v_tiny(const float v[4]) {
    return (v[0]*v[0] + v[1]*v[1] + v[2]*v[2]) < K_EPS_LEN2;
}
static float v_dot3(const float a[4], const float b[4]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static void v_copy4(float d[4], const float s[4]) {
    d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
}
/* FUN_00013CA0: full 4-lane affine point transform, row-vector matrix. */
static void m_xform_point(float out[4], const float m[4][4], const float p[4]) {
    float x = p[0], y = p[1], z = p[2];
    for (int i = 0; i < 4; i++)
        out[i] = x*m[0][i] + m[3][i] + y*m[1][i] + z*m[2][i];
}
/* FUN_00031330: rotation only (no translation), 4 lanes. */
static void m_xform_vec(float out[4], const float m[4][4], const float v[4]) {
    float x = v[0], y = v[1], z = v[2];
    for (int i = 0; i < 4; i++)
        out[i] = x*m[0][i] + y*m[1][i] + z*m[2][i];
}
/* FUN_000116E0: out = a * b (row-vector convention, translation in row 3). */
static void m_mul(float out[4][4], const float a[4][4], const float b[4][4]) {
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < 4; i++)
            out[r][i] = a[r][0]*b[0][i] + a[r][1]*b[1][i] + a[r][2]*b[2][i]
                      + (r == 3 ? b[3][i] : 0.0f);
}

/* --------------------------------------------------------------------------
 * hull sourcing
 * ------------------------------------------------------------------------ */
int b3_carcol_hull_from_record(const void* rec600, B3CarHull* out) {
    const unsigned char* r = (const unsigned char*)rec600;
    if (!r || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->nverts  = r[0x18];
    out->nplanes = r[0x19];
    out->nedges  = r[0x1A];
    if (out->nverts  <= 0 || out->nverts  > B3_HULL_MAX_VERTS)  return 0;
    if (out->nplanes <= 0 || out->nplanes > B3_HULL_MAX_PLANES) return 0;
    if (out->nedges  <= 0 || out->nedges  > B3_HULL_MAX_EDGES)  return 0;
    memcpy(out->planes, r + 0x0A0, (size_t)out->nplanes * 16);
    memcpy(out->verts,  r + 0x320, (size_t)out->nverts  * 16);
    memcpy(out->edges,  r + 0x480, (size_t)out->nedges  * 2);
    return 1;
}

int b3_carcol_hull_load(const char* path, B3CarHull* out) {
    unsigned char rec[0x600];
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(rec, 1, sizeof(rec), f);
    fclose(f);
    if (n != sizeof(rec)) return 0;
    return b3_carcol_hull_from_record(rec, out);
}

/* GLUE: an axis-aligned box hull from the .bgv collision extents, used only
 * when the real hull record is unavailable.  Plane/vertex/edge conventions
 * are the game's (inside iff dot(n,p) <= d). */
void b3_carcol_hull_from_extents(const float bbmax[4], const float bbmin[4],
                                 B3CarHull* out) {
    static const int AX[6] = {0,0,1,1,2,2};
    static const float SG[6] = {1,-1,1,-1,1,-1};
    /* box edges as vertex-index pairs (12 edges of a cube) */
    static const unsigned char E[12][2] = {
        {0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    memset(out, 0, sizeof(*out));
    out->nverts = 8; out->nplanes = 6; out->nedges = 12;
    for (int i = 0; i < 8; i++) {
        out->verts[i][0] = (i & 1) ? bbmax[0] : bbmin[0];
        out->verts[i][1] = (i & 2) ? bbmax[1] : bbmin[1];
        out->verts[i][2] = (i & 4) ? bbmax[2] : bbmin[2];
        out->verts[i][3] = 0.0f;
    }
    for (int p = 0; p < 6; p++) {
        int a = AX[p];
        out->planes[p][0] = out->planes[p][1] = out->planes[p][2] = 0.0f;
        out->planes[p][a] = SG[p];
        out->planes[p][3] = SG[p] > 0 ? bbmax[a] : -bbmin[a];
    }
    for (int e = 0; e < 12; e++)
        out->edges[e] = (unsigned short)(E[e][0] | (E[e][1] << 8));
}

void b3_carcol_init(void) { /* nothing to prepare; hulls are supplied per car */ }

/* --------------------------------------------------------------------------
 * broad phase -- FUN_00114270 (world AABB) + FUN_00110AF0's overlap tests
 * ------------------------------------------------------------------------ */
void b3_carcol_world_aabb(const B3CarBody* b, float lo[3], float hi[3]) {
    const float (*m)[4] = b->rb->frame;
    for (int i = 0; i < 3; i++) {
        float c = m[3][i], e = 0.0f;
        for (int a = 0; a < 3; a++) {
            float h = 0.5f * (b->bbmax[a] - b->bbmin[a]);
            float o = 0.5f * (b->bbmax[a] + b->bbmin[a]);
            c += o * m[a][i];
            e += fabsf(m[a][i]) * h;
        }
        lo[i] = c - e; hi[i] = c + e;
    }
}

int b3_carcol_aabb_overlap(const B3CarBody* a, const B3CarBody* b) {
    float alo[3], ahi[3], blo[3], bhi[3];
    /* FUN_00114610's pair filter: two sleeping cars never pair. */
    if (a->asleep && b->asleep) return 0;
    b3_carcol_world_aabb(a, alo, ahi);
    b3_carcol_world_aabb(b, blo, bhi);
    for (int i = 0; i < 3; i++)
        if (!(blo[i] < ahi[i] && alo[i] < bhi[i])) return 0;
    return 1;
}

int b3_carcol_broadphase(B3CarBody* const* bodies, int n,
                         int (*pairs)[2], int max_pairs) {
    int c = 0;
    /* The game runs a sort + sweep on the x interval (record +0x10/+0x20)
     * with y/z overlap tests inside; the emitted set is exactly the set of
     * AABB-overlapping pairs, so the sweep is replaced by the direct test
     * (docs/RE_CARCOL.md "broad phase").  Cap 0x100 is the game's. */
    if (max_pairs > 0x100) max_pairs = 0x100;
    for (int i = 0; i < n && c < max_pairs; i++) {
        if (!bodies[i]) continue;
        for (int j = i + 1; j < n && c < max_pairs; j++) {
            if (!bodies[j]) continue;
            if (!b3_carcol_aabb_overlap(bodies[i], bodies[j])) continue;
            pairs[c][0] = i; pairs[c][1] = j; c++;
        }
    }
    return c;
}

/* --------------------------------------------------------------------------
 * narrow phase -- FUN_0010AC20 and its helpers
 * ------------------------------------------------------------------------ */

/* FUN_0010B210: emit both endpoints of every hull edge whose two vertices
 * both satisfy dot3(dir, vert_local) >= 0.  Points come from wverts (the
 * hull's vertices already expressed in the OTHER body's local space). */
static int hull_support_edges(const float dir[4], const B3CarHull* h,
                              const float (*wverts)[4], float (*out)[4],
                              int out_cap) {
    float d[B3_HULL_MAX_VERTS];
    int n = 0;
    for (int i = 0; i < h->nverts; i++) d[i] = v_dot3(dir, h->verts[i]);
    for (int e = 0; e < h->nedges; e++) {
        int a = h->edges[e] & 0xFF, b = (h->edges[e] >> 8) & 0xFF;
        if (a >= h->nverts || b >= h->nverts) continue;   /* GLUE guard */
        if (!(d[a] >= 0.0f) || !(d[b] >= 0.0f)) continue;
        if (n + 2 > out_cap) break;                        /* GLUE guard */
        v_copy4(out[n++], wverts[a]);
        v_copy4(out[n++], wverts[b]);
    }
    return n;
}

/* FUN_0010C220: clip a list read as consecutive segment pairs against one
 * half-space dot3(n,p) <= d.  Returns the number of output points. */
static int clip_plane(const float (*in)[4], int count, const float pl[4],
                      float (*out)[4], int out_cap) {
    int n = 0;
    if (count == 0) return 0;
    int pairs = ((count - 1) >> 1) + 1;
    for (int k = 0; k < pairs; k++) {
        const float* a = in[2*k];
        const float* b = in[2*k + 1];
        float s0 = v_dot3(pl, a), s1 = v_dot3(pl, b);
        int in0 = (s0 <= pl[3]), in1 = (s1 <= pl[3]);
        if (in0 != in1 && n < out_cap) {
            float t = (pl[3] - s0) / (s1 - s0);
            for (int i = 0; i < 4; i++) out[n][i] = (b[i] - a[i]) * t + a[i];
            n++;
        }
        if (in0 && n < out_cap) { v_copy4(out[n], a); n++; }
        if (in1 && n < out_cap) { v_copy4(out[n], b); n++; }
    }
    return n;
}

/* FUN_0010C0D0: clip against every plane of a hull (ping-pong scratch), then
 * copy the survivors to dst discarding duplicates (component-wise 1e-7). */
#define CLIP_SCRATCH 120           /* 0x780 bytes = the game's DAT_005A53C0 */
static int clip_all(const float (*in)[4], int count,
                    const float (*planes)[4], int nplanes,
                    float (*dst)[4], int dst_cap) {
    static float buf[2][CLIP_SCRATCH][4];
    int cur = 0, n;
    n = clip_plane(in, count, planes[0], buf[0], CLIP_SCRATCH);
    for (int p = 1; p < nplanes; p++) {
        if (n < 2) break;
        n = clip_plane((const float (*)[4])buf[cur], n, planes[p],
                       buf[1 - cur], CLIP_SCRATCH);
        cur = 1 - cur;
    }
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        const float* p = buf[cur][i];
        int dup = 0;
        for (int q = 0; q < emitted; q++) {
            if (fabsf(p[0] - dst[q][0]) - K_DEDUP < 0.0f &&
                fabsf(p[1] - dst[q][1]) - K_DEDUP < 0.0f &&
                fabsf(p[2] - dst[q][2]) - K_DEDUP < 0.0f) { dup = 1; break; }
        }
        if (dup) continue;
        if (emitted >= dst_cap) break;                     /* GLUE guard */
        v_copy4(dst[emitted], p);
        emitted++;
    }
    return emitted;
}

/* FUN_0010C000: index of the plane whose |dot3(n,p) - d| is smallest. */
static int closest_plane(const float (*planes)[4], int n, const float p[4]) {
    int best = 0;
    float f = fabsf(v_dot3(planes[0], p) - planes[0][3]);
    for (int i = 1; i < n; i++) {
        float fi = fabsf(v_dot3(planes[i], p) - planes[i][3]);
        if (f > fi) { f = fi; best = i; }
    }
    return best;
}

/* FUN_0010BE70: shoot a segment start -> start + dir*100 through the hull's
 * planes; return the index of the last plane crossed (the exit plane). */
static int ray_exit_plane(const float (*planes)[4], int n,
                          const float start[4], const float dir[4]) {
    float p1[4];
    int out = -1;
    for (int i = 0; i < 4; i++) p1[i] = dir[i] * K_RAY_LEN + start[i];
    for (int i = 0; i < n; i++) {
        float d1 = v_dot3(p1, planes[i]);
        if (d1 < planes[i][3]) continue;
        out = i;
        float d0 = v_dot3(start, planes[i]);
        if (d0 >= planes[i][3]) return out;
        if (fabsf(d1 - d0) > K_RAY_PAR) {
            float t = (planes[i][3] - d0) / (d1 - d0);
            for (int k = 0; k < 4; k++) p1[k] = start[k] + (p1[k]-start[k]) * t;
        }
    }
    if (out < 0) out = 0;
    return out;
}

/* FUN_0010B310: centre of the AABB of a point set (all four lanes). */
static void points_centre(const float (*p)[4], int n, float out[4]) {
    float lo[4], hi[4];
    v_copy4(lo, p[0]); v_copy4(hi, p[0]);
    for (int i = 1; i < n; i++)
        for (int k = 0; k < 4; k++) {
            if (p[i][k] < lo[k]) lo[k] = p[i][k];
            if (p[i][k] > hi[k]) hi[k] = p[i][k];
        }
    for (int k = 0; k < 4; k++) out[k] = (lo[k] + hi[k]) * K_HALF;
}

int b3_carcol_contact(const B3CarBody* A, const B3CarBody* B,
                      B3CarContact* out) {
    const B3CarHull* hA = A->hull;
    const B3CarHull* hB = B->hull;
    const float (*fA)[4] = A->rb->frame;
    const float (*fB)[4] = B->rb->frame;
    const float (*iA)[4] = A->rb->inv_frame;
    const float (*iB)[4] = B->rb->inv_frame;

    memset(out, 0, sizeof(*out));
    if (!hA || !hB) return 0;

    /* FUN_0010ABC0 head: coincident frames -> no contact. */
    {
        float dx = fA[3][0] - fB[3][0];
        float dy = fA[3][1] - fB[3][1];
        float dz = fA[3][2] - fB[3][2];
        if (dx*dx + dy*dy + dz*dz < K_MIN_SEP2) return 0;
    }

    float m_a2b[4][4], m_b2a[4][4];
    m_mul(m_a2b, fA, iB);          /* A-local -> B-local */
    m_mul(m_b2a, fB, iA);          /* B-local -> A-local */

    float pA_in_B[4], pB_in_A[4];
    m_xform_point(pA_in_B, iB, fA[3]);
    m_xform_point(pB_in_A, iA, fB[3]);

    float va[B3_HULL_MAX_VERTS][4], vb[B3_HULL_MAX_VERTS][4];
    for (int i = 0; i < hA->nverts; i++) m_xform_point(va[i], m_a2b, hA->verts[i]);
    for (int i = 0; i < hB->nverts; i++) m_xform_point(vb[i], m_b2a, hB->verts[i]);

    static float segs[CLIP_SCRATCH][4];
    static float pts[64][4];               /* the game's local_880, 0x400 */
    int n1, n2;

    {   /* A's support edges (in B-local) clipped by B's planes */
        float dir[4]; v_copy4(dir, pB_in_A); v_norm4(dir);
        int ns = hull_support_edges(dir, hA, (const float (*)[4])va,
                                    segs, CLIP_SCRATCH);
        n1 = clip_all((const float (*)[4])segs, ns, hB->planes, hB->nplanes,
                      pts, 64);
    }
    {   /* B's support edges (in A-local) clipped by A's planes */
        float dir[4]; v_copy4(dir, pA_in_B); v_norm4(dir);
        int ns = hull_support_edges(dir, hB, (const float (*)[4])vb,
                                    segs, CLIP_SCRATCH);
        n2 = clip_all((const float (*)[4])segs, ns, hA->planes, hA->nplanes,
                      pts + n1, 64 - n1);
    }
    int total = n1 + n2;
    if (total <= 1) return 0;

    /* group 1 is in B-local, group 2 in A-local: lift both to world */
    for (int i = 0; i < n1; i++)    { float t[4]; m_xform_point(t, fB, pts[i]);      v_copy4(pts[i], t); }
    for (int i = n1; i < total; i++){ float t[4]; m_xform_point(t, fA, pts[i]);      v_copy4(pts[i], t); }

    float cp[4];
    points_centre((const float (*)[4])pts, total, cp);

    float cpA[4], cpB[4];
    m_xform_point(cpA, iA, cp);
    m_xform_point(cpB, iB, cp);

    int ia = closest_plane(hA->planes, hA->nplanes, cpA);
    int ib = closest_plane(hB->planes, hB->nplanes, cpB);
    float nAw[4], nBw[4], n[4];
    m_xform_vec(nAw, fA, hA->planes[ia]);
    m_xform_vec(nBw, fB, hB->planes[ib]);
    for (int k = 0; k < 4; k++) n[k] = nAw[k] - nBw[k];
    if (v_tiny(n)) return 0;
    v_norm4(n);

    v_copy4(out->point, cp);
    v_copy4(out->normal, n);

    /* per-body penetration along its own exit plane */
    int ra = ray_exit_plane(hA->planes, hA->nplanes, cpA, pB_in_A);
    int rb = ray_exit_plane(hB->planes, hB->nplanes, cpB, pA_in_B);
    float penA = hA->planes[ra][3] - v_dot3(cpA, hA->planes[ra]);
    float penB = hB->planes[rb][3] - v_dot3(cpB, hB->planes[rb]);
    if (penA < 0.0f) penA = 0.0f;
    if (penB < 0.0f) penB = 0.0f;
    {
        float la[4], lb[4];
        for (int k = 0; k < 4; k++) la[k] = hA->planes[ra][k] * (-penA);
        for (int k = 0; k < 4; k++) lb[k] = hB->planes[rb][k] * (-penB);
        m_xform_vec(out->pen_a, fA, la);
        m_xform_vec(out->pen_b, fB, lb);
    }
    out->hit = 1;
    return 1;
}

/* --------------------------------------------------------------------------
 * impulse / force plumbing
 * ------------------------------------------------------------------------ */

/* FUN_001066A0 */
void b3_carcol_point_velocity(const B3RigidBody* rb, const float pt[4],
                              float out[4]) {
    float r[4];
    for (int k = 0; k < 4; k++) r[k] = pt[k] - rb->frame[3][k];
    out[0] = rb->omega[1]*r[2] - rb->omega[2]*r[1] + rb->vel[0];
    out[1] = rb->omega[2]*r[0] - rb->omega[0]*r[2] + rb->vel[1];
    out[2] = rb->omega[0]*r[1] - rb->omega[1]*r[0] + rb->vel[2];
    out[3] = rb->vel[3];
}

/* FUN_0010F8D0 -- the two-body contact impulse.  Layout follows the real
 * parameter split: (param_1, param_5) is one body/point, (param_3, param_4)
 * the other; the impulse written out is n * -|j|. */
float b3_carcol_mutual_impulse(const B3RigidBody* rb1, float m1,
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
    /* Iinv rows at +0x40/+0x50/+0x60, applied column-wise (the real code) */
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
    float num = -(restitution + 1.0f) * (n[0]*vrel[0] + n[1]*vrel[1] + n[2]*vrel[2]);
    float j   = fabsf(num / den);
    for (int k = 0; k < 4; k++) out[k] = n[k] * (-j);
    return j;
}

/* FUN_00106590: torque = (pt - pos) x f */
static void torque_at(const B3RigidBody* rb, const float pt[4],
                      const float f[4], float out[4]) {
    float r[3];
    for (int k = 0; k < 3; k++) r[k] = pt[k] - rb->frame[3][k];
    out[0] = r[1]*f[2] - r[2]*f[1];
    out[1] = r[2]*f[0] - r[0]*f[2];
    out[2] = r[0]*f[1] - r[1]*f[0];
    out[3] = 0.0f;
}

/* FUN_00106500: linear + angular IMPULSE at a point (+0x110 / +0x120). */
static void impulse_at(B3RigidBody* rb, const float imp[4], const float pt[4]) {
    float t[4];
    for (int k = 0; k < 4; k++) rb->imp_force[k] += imp[k];
    torque_at(rb, pt, imp, t);
    for (int k = 0; k < 4; k++) rb->imp_torque[k] += t[k];
}

/* FUN_001064B0: linear + angular FORCE at a point (+0xF0 / +0x100). */
static void force_at(B3RigidBody* rb, const float f[4], const float pt[4]) {
    float t[4];
    for (int k = 0; k < 4; k++) rb->force_acc[k] += f[k];
    torque_at(rb, pt, f, t);
    for (int k = 0; k < 4; k++) rb->torque_acc[k] += t[k];
}

/* copysign(1, x) exactly as the real code does it (mask/or on the bits). */
static float sign_one(float x) {
    unsigned int u; memcpy(&u, &x, 4);
    u = (u & 0xBF800000u) | 0x3F800000u;
    float r; memcpy(&r, &u, 4); return r;
}

/* FUN_001205E0 */
void b3_carcol_apply_force(B3RigidBody* rb, int drift_state,
                           const float force[4], const float point[4]) {
    float t[4];
    torque_at(rb, point, force, t);
    if (drift_state == 1 || drift_state == 2) {
        for (int k = 0; k < 4; k++) rb->force_acc[k] += force[k];
        return;
    }
    if (fabsf(rb->omega[1]) > K_YAW_LOCK
        && sign_one(rb->omega[1]) == sign_one(t[1])) {
        /* already yawing that way: linear only */
        for (int k = 0; k < 4; k++) rb->force_acc[k] += force[k];
        return;
    }
    force_at(rb, force, point);
}

/* --------------------------------------------------------------------------
 * FUN_001121F0 -- racer vs racer (neither crashed)
 * ------------------------------------------------------------------------ */
static float clamp01(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > K_ONE) v = K_ONE;
    return v;
}

/* longitudinal contact parameter along a body: 0 at the tail plane
 * (bbmin.z), 1 at the nose plane (bbmax.z). */
static float long_param(const B3CarBody* b, const float cp[4]) {
    const float (*m)[4] = b->rb->frame;
    float zmin = b->bbmin[2], zmax = b->bbmax[2];
    float axis[4], rear[4], d[4];
    for (int k = 0; k < 4; k++) {
        axis[k] = m[2][k] * (zmax - zmin);
        rear[k] = m[3][k] + m[2][k] * zmin;
        d[k]    = cp[k] - rear[k];
    }
    float len2 = v_dot3(axis, axis);
    return clamp01(v_dot3(d, axis) / len2);
}

int b3_carcol_resolve_alive(B3CarBody* A, B3CarBody* B, B3CarContact* out) {
    if (!b3_carcol_contact(A, B, out)) return 0;

    A->touched = 1;                                 /* veh+0x211 */
    B->touched = 1;

    /* ---- separation: horizontal only, split by mass (0x1122xx) ---------- */
    if (out->hit) {
        float dA[4], dB[4], D[4];
        for (int k = 0; k < 4; k++) {
            dA[k] = out->pen_a[k];                  /* corrected pos - pos */
            dB[k] = out->pen_b[k];
        }
        dA[1] = 0.0f; dB[1] = 0.0f;                 /* [esp+0x64]/[esp+0x54] */
        float w = A->mass / (A->mass + B->mass);
        for (int k = 0; k < 4; k++) D[k] = dA[k] - dB[k];
        if (B->grounded) {
            for (int k = 0; k < 4; k++) A->rb->deflection[k] += D[k];
        } else if (A->grounded) {
            for (int k = 0; k < 4; k++) B->rb->deflection[k] += -D[k];
        } else {
            for (int k = 0; k < 4; k++) A->rb->deflection[k] += D[k] * (K_ONE - w);
            for (int k = 0; k < 4; k++) B->rb->deflection[k] += D[k] * (-w);
        }
    }

    /* ---- contact point / normal fix-up (0x112373) ----------------------- */
    out->point[1] = A->rb->frame[3][1] * K_TWO * K_HALF + K_CP_Y_BIAS;
    out->normal[1] = 0.0f;
    v_copy4(A->contact_pt, out->point);             /* veh+0x150 */
    v_copy4(B->contact_pt, out->point);

    /* ---- longitudinal contact parameters (0x1123CB) --------------------- */
    out->t_a = long_param(A, out->point);
    out->t_b = long_param(B, out->point);

    /* ---- relative velocity at the contact ------------------------------- */
    float vpa[4], vpb[4], vrel[4];
    b3_carcol_point_velocity(A->rb, out->point, vpa);
    b3_carcol_point_velocity(B->rb, out->point, vpb);
    for (int k = 0; k < 4; k++) vrel[k] = vpb[k] - vpa[k];
    float vn = v_dot3(vrel, out->normal);
    out->vn_mph = fabsf(vn * B3_CARCOL_MPH);

    /* ---- mutual impulse + the shove (0x112600 .. 0x1127FD) -------------- */
    float imp[4];
    float j = b3_carcol_mutual_impulse(B->rb, B->mass, A->rb, A->mass,
                                       out->point, out->point, vrel,
                                       out->normal, B3_CARCOL_RESTITUTION, imp);
    out->j = j;
    if (j > 0.0f) {
        v_copy4(out->impulse, imp);
        for (int k = 0; k < 4; k++) A->rb->imp_force[k] += imp[k];
        for (int k = 0; k < 4; k++) B->rb->imp_force[k] -= imp[k];

        float mb = B->mass < B3_CARCOL_SHOVE_MASS_CAP ? B->mass : B3_CARCOL_SHOVE_MASS_CAP;
        float ma = A->mass < B3_CARCOL_SHOVE_MASS_CAP ? A->mass : B3_CARCOL_SHOVE_MASS_CAP;
        float fa[4], fb[4];
        float ka = mb * (-B3_CARCOL_SHOVE_K) * (fabsf(B->yaw_input) + K_ONE);
        float kb = ma * ( B3_CARCOL_SHOVE_K) * (fabsf(A->yaw_input) + K_ONE);
        for (int k = 0; k < 4; k++) fa[k] = out->normal[k] * ka;
        for (int k = 0; k < 4; k++) fb[k] = out->normal[k] * kb;
        /* the retail code adds the force to +0xF0 AND then routes it through
         * FUN_001205E0, which adds it a second time (linear component twice,
         * torque once).  Reproduced verbatim -- see docs/RE_CARCOL.md. */
        for (int k = 0; k < 4; k++) A->rb->force_acc[k] += fa[k];
        b3_carcol_apply_force(A->rb, A->drift_state, fa, out->point);
        for (int k = 0; k < 4; k++) B->rb->force_acc[k] += fb[k];
        b3_carcol_apply_force(B->rb, B->drift_state, fb, out->point);
    }

    /* ---- impact magnitude + crash threshold (0x112808) ------------------ */
    out->impact = (A->mass + B->mass) * out->vn_mph
                * B3_CARCOL_IMPACT_SCALE * K_HALF;
    if (out->vn_mph > B3_CARCOL_CRASH_MPH) {
        /* DAT_0039AE50[clsA][clsB]: class 0 (types 0/2) crashes against
         * classes 0..5, class 2 (type 3) only against 0 and 3.  For the two
         * car types in play both directions are 1.  The crash path skips the
         * rub report but STILL falls into the slam classification (0x112974
         * jumps past the report to 0x112988). */
        out->crash_a = 1;
        out->crash_b = 1;
    } else {
        out->event = B3_SLAM_RUB;                /* vtable+0x64(1, A, B, 1.0) */
        out->strength = K_ONE;
    }

    /* ---- slam classification (0x112991 ..) ------------------------------ */
    float lat;
    {
        float d[4];
        for (int k = 0; k < 4; k++) d[k] = B->rb->frame[3][k] - A->rb->frame[3][k];
        lat = v_dot3(B->rb->frame[0], d);
    }
    float spA = A->rb->vel[3], spB = B->rb->vel[3];
    float tA = out->t_a, tB = out->t_b;
    int light = 0;
    float s;

    if (fabsf(K_ONE - tA) <= K_SLAM_EPS && fabsf(-tB) <= K_SLAM_EPS) {
        /* A's nose into B's tail */
        if (!(out->vn_mph > B3_CARCOL_REAR_MIN_MPH)) return 1;
        if (!(spA > spB)) return 1;
        s = (out->vn_mph - B3_CARCOL_REAR_MIN_MPH) / B3_CARCOL_REAR_RANGE_MPH;
        if (s > K_ONE) s = K_ONE;
        if (B3_CARCOL_LIGHT_FRAC >= s) { light = 1; if (s > 0.0f) s /= B3_CARCOL_LIGHT_FRAC; }
        out->event = light ? B3_SLAM_REAR_LIGHT : B3_SLAM_REAR;
        out->attacker_is_b = 0;
        out->strength = s;
        out->slam_class = 2;
        B->hit_side = (unsigned char)(0.0f > lat);
        if (light) A->hit_side = (unsigned char)(B->hit_side != 1);
        return 1;
    }
    if (fabsf(K_ONE - tB) <= K_SLAM_EPS && fabsf(-tA) <= K_SLAM_EPS) {
        /* B's nose into A's tail */
        if (!(out->vn_mph > B3_CARCOL_REAR_MIN_MPH)) return 1;
        if (!(spB > spA)) return 1;
        s = (out->vn_mph - B3_CARCOL_REAR_MIN_MPH) / B3_CARCOL_REAR_RANGE_MPH;
        if (s > K_ONE) s = K_ONE;
        if (B3_CARCOL_LIGHT_FRAC >= s) { light = 1; if (s > 0.0f) s /= B3_CARCOL_LIGHT_FRAC; }
        out->event = light ? B3_SLAM_REAR_LIGHT : B3_SLAM_REAR;
        out->attacker_is_b = 1;
        out->strength = s;
        out->slam_class = 2;
        A->hit_side = (unsigned char)(lat > 0.0f);
        if (light) B->hit_side = (unsigned char)(A->hit_side != 1);
        return 1;
    }

    /* side impact: gate on closing speed vs the heading-difference ramp */
    {
        float fa[4], fb[4];
        v_copy4(fa, A->rb->frame[2]); fa[1] = 0.0f;
        v_copy4(fb, B->rb->frame[2]); fb[1] = 0.0f;
        /* FUN_000FF160: heading difference of the two flattened forward
         * axes, in degrees (rsqrt-approximated in the original). */
        float la = sqrtf(v_dot3(fa, fa)), lb = sqrtf(v_dot3(fb, fb));
        float c = (la > 0.0f && lb > 0.0f) ? v_dot3(fa, fb) / (la * lb) : 1.0f;
        if (c > 1.0f) c = 1.0f;
        if (c < -1.0f) c = -1.0f;
        float ang = acosf(c) * (180.0f / 3.14159265358979f);

        int hard = (out->vn_mph > B3_CARCOL_SIDE_HI_MPH);
        if (out->vn_mph > B3_CARCOL_SIDE_LO_MPH
            && fabsf(ang) > B3_CARCOL_SIDE_ANG) hard = 1;
        float thresh = (B3_CARCOL_SIDE_HI_MPH - B3_CARCOL_SIDE_LO_MPH)
                     / B3_CARCOL_SIDE_ANG * ang + B3_CARCOL_SIDE_LO_MPH;
        if (!(out->vn_mph > thresh) && !hard) return 1;

        s = (out->vn_mph - B3_CARCOL_SIDE_MIN_MPH) / B3_CARCOL_SIDE_RANGE_MPH;
        if (s > K_ONE) s = K_ONE;
        if (B3_CARCOL_LIGHT_FRAC >= s) { light = 1; if (s > 0.0f) s /= B3_CARCOL_LIGHT_FRAC; }

        int a_forward = (tA > tB);
        int attacker_b;
        if (a_forward)  attacker_b = ((spB - spA) > B3_CARCOL_ATTACK_DV);
        else            attacker_b = !((spA - spB) > B3_CARCOL_ATTACK_DV);

        out->event = light ? B3_SLAM_SIDE_LIGHT : B3_SLAM_SIDE;
        out->attacker_is_b = attacker_b;
        out->strength = s;
        out->slam_class = 1;
        if (attacker_b) {
            A->hit_side = (unsigned char)(lat > 0.0f);
            if (light) B->hit_side = (unsigned char)(A->hit_side != 1);
        } else {
            B->hit_side = (unsigned char)(0.0f > lat);
            if (light) A->hit_side = (unsigned char)(B->hit_side != 1);
        }
    }
    return 1;
}

/* --------------------------------------------------------------------------
 * FUN_00113960 -- car vs crashed car
 * ------------------------------------------------------------------------ */
int b3_carcol_resolve_wreck(B3CarBody* A, B3CarBody* B, B3CarContact* out) {
    if (!b3_carcol_contact(A, B, out)) return 0;

    /* FUN_0010FC50 -> DAT_0039AE88.  For the two car classes in play the
     * kinds are 0/0 (both movable); a car that is NOT crashed is forced to
     * kind 2 (immovable) at 0x113B75, which is exactly this function's
     * contract: A is the un-crashed car. */
    int kindA = (A->type <= 2 && !A->crashed) ? 2 : 0;
    int kindB = 0;

    A->touched = 1;
    B->touched = 1;

    float dA[4], dB[4], D[4];
    v_copy4(dA, out->pen_a);
    v_copy4(dB, out->pen_b);
    for (int k = 0; k < 4; k++) D[k] = dA[k] - dB[k];
    if (kindA == 2) {
        for (int k = 0; k < 4; k++) B->rb->deflection[k] += (dB[k] - dA[k]);
    } else if (kindB == 2) {
        for (int k = 0; k < 4; k++) A->rb->deflection[k] += D[k];
    } else {
        float w = A->mass / (A->mass + B->mass);
        for (int k = 0; k < 4; k++) A->rb->deflection[k] += D[k] * (K_ONE - w);
        for (int k = 0; k < 4; k++) B->rb->deflection[k] += D[k] * (-w);
    }

    /* relative velocity at the contact, then the normal blended toward the
     * relative-velocity direction by -0.9 and re-normalised (0x113EFA). */
    float vpa[4], vpb[4], vrel[4], n[4];
    b3_carcol_point_velocity(A->rb, out->point, vpa);
    b3_carcol_point_velocity(B->rb, out->point, vpb);
    for (int k = 0; k < 4; k++) vrel[k] = vpb[k] - vpa[k];
    v_copy4(n, out->normal);
    if (!v_tiny(vrel)) {
        float u[4]; v_copy4(u, vrel); v_norm4(u);
        for (int k = 0; k < 4; k++) n[k] += u[k] * B3_CARCOL_NORM_BLEND;
        v_norm4(n);
        v_copy4(out->normal, n);
    }
    out->vn_mph = fabsf(v_dot3(vrel, n) * B3_CARCOL_MPH);

    float imp[4];
    float j = b3_carcol_mutual_impulse(B->rb, B->mass, A->rb, A->mass,
                                       out->point, out->point, vrel, n,
                                       B3_CARCOL_WRECK_RESTITUTION, imp);
    out->j = j;
    out->impact = 0.0f;
    if (j > 0.0f) {
        v_copy4(out->impulse, imp);
        if (kindA == 2) {
            float neg[4];
            for (int k = 0; k < 4; k++) neg[k] = -imp[k];   /* 0x003B16C0 = -1 */
            impulse_at(B->rb, neg, out->point);
        } else if (kindB == 2) {
            impulse_at(A->rb, imp, out->point);
        } else {
            float neg[4];
            impulse_at(A->rb, imp, out->point);
            for (int k = 0; k < 4; k++) neg[k] = -imp[k];
            impulse_at(B->rb, neg, out->point);
        }
        out->impact = j;
    }

    /* crash threshold: 2500 when a traffic car (type 2) hits a non-traffic
     * un-crashed car, otherwise 5000 (0x0011408B / 0x00114081). */
    /* FUN_00113960's recent-slam suppression (see B3CarBody::slam_recent).
     * The guard is retail's: skipped when a NON-traffic car hits a TRAFFIC
     * car, so the cheap traffic cascade is never gated by it. */
    if (!(A->type != B3_COL_TYPE_TRAFFIC && B->type == B3_COL_TYPE_TRAFFIC)
        && (A->slam_recent || B->slam_recent)) {
        if (getenv("B3_WRECK_TRACE"))
            fprintf(stderr, "[wreck] suppressed by the 1.5 s slam window\n");
        return 1;                      /* contact resolved, no crash */
    }

    float thresh = B3_CARCOL_WRECK_IMPACT;
    if (A->type <= 2 && !A->crashed && A->type != B3_COL_TYPE_TRAFFIC
        && B->type <= 2 && B->crashed && B->type == B3_COL_TYPE_TRAFFIC)
        thresh = B3_CARCOL_WRECK_IMPACT_TR;
    if (A->type <= 2 && !A->crashed && out->impact > thresh)
        out->crash_a = 1;
    if (getenv("B3_WRECK_TRACE"))
        fprintf(stderr, "[wreck] A.type=%d A.crashed=%d B.type=%d B.crashed=%d "
                "j=%.0f impact=%.0f thresh=%.0f -> crash_a=%d\n",
                A->type, A->crashed, B->type, B->crashed,
                out->j, out->impact, thresh, out->crash_a);
    return 1;
}

int b3_carcol_resolve(B3CarBody* a, B3CarBody* b, B3CarContact* out) {
    /* FUN_00111CD0's ordering + dispatch. */
    if (!a->crashed && !b->crashed)
        return b3_carcol_resolve_alive(a, b, out);
    if (!b->crashed) { B3CarBody* t = a; a = b; b = t; }
    return b3_carcol_resolve_wreck(a, b, out);
}
