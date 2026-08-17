// The GAME'S OWN collision world.
//
// Data: build/collision.bin, extracted by tools/extract_collision.py from the
// per-unit collision sections inside streamed.dat's LOD blocks (the kd-tree
// poly soups the retail game builds its vehicle collision from).  Format and
// the full query chain -- FUN_00109d20 gather, FUN_001aff70 walker,
// FUN_00123790 wheel ray, FUN_001b2230 ray/triangle core -- are documented
// with addresses in docs/RE_NOTES.md section 15 and execution-verified in
// tools/validate_gameplay.py ('collision' section, game code under Unicorn
// vs this module's exact math).
//
// The ray core below is a 1:1 port of FUN_001b2230: one-sided
// Moller-Trumbore with the game's compiled-in constants
//   det   >  1e-8            (0x003a6860)
//   u,v,t in (-1e-5*det, 1.00001*det]   (0x003b16a0 / 0x003b1918)
// The winner is the minimum PARAMETRIC t, exactly like FUN_00123790.
//
// Coordinates: collision.bin stores raw game data; the loader applies the
// harness's uniform reflection (negate Z, swap v1/v2 to keep the one-sided
// orientation) so queries and results live in the same GL space as
// trackmesh_load's geometry.  The sphere sweep is harness glue running over
// the real triangles (the game's own body test is the unported
// FUN_00107950/FUN_0010aad0 box path).

#include "burnout3_collision.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// FUN_001b2230's constants, read from the image (RE_NOTES 15).
#define B3C_DET_EPS 9.99999993922529e-09f
#define B3C_LO_K   -9.999999747378752e-06f
#define B3C_HI_K    1.0000100135803223f

typedef struct {
    float v0[3], v1[3], v2[3];
    float n[3];                 // unit normal (cross(v1-v0, v2-v0))
    unsigned short type;
    unsigned char unit;
    unsigned char excl;         // gather-callback-excluded (low 20/22/23/24)
} B3ColTri;

static B3ColTri* g_tris = NULL;
static int g_ntris = 0;
static float g_min[3], g_max[3];

// XZ grid of triangle indices (triangles registered over their XZ AABB).
#define B3C_CELL 8.0f
static int g_gw = 0, g_gh = 0;
static int* g_cell_start = NULL;    // per cell, into g_cell_idx
static int* g_cell_count = NULL;
static int* g_cell_idx = NULL;
static unsigned* g_seen = NULL;
static unsigned g_seen_stamp = 0;

static void cell_of(float x, float z, int* cx, int* cz) {
    *cx = (int)floorf((x - g_min[0]) / B3C_CELL);
    *cz = (int)floorf((z - g_min[2]) / B3C_CELL);
}

int b3_collision_ready(void) { return g_ntris > 0; }

int b3_collision_tri_count(void) { return g_ntris; }

int b3_collision_tri_get(int i, float* v0, float* v1, float* v2,
                         float* normal, unsigned short* type, int* excluded) {
    if (i < 0 || i >= g_ntris) return 0;
    const B3ColTri* t = &g_tris[i];
    if (v0) memcpy(v0, t->v0, 12);
    if (v1) memcpy(v1, t->v1, 12);
    if (v2) memcpy(v2, t->v2, 12);
    if (normal) memcpy(normal, t->n, 12);
    if (type) *type = t->type;
    if (excluded) *excluded = t->excl;
    return 1;
}

int b3_collision_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char hdr[0x28];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr ||
        memcmp(hdr, "B3CL", 4) != 0) {
        fclose(f);
        return 0;
    }
    unsigned version, count;
    memcpy(&version, hdr + 4, 4);
    memcpy(&count, hdr + 8, 4);
    if (version != 1 || count == 0 || count > 4u * 1000 * 1000) {
        fclose(f);
        return 0;
    }
    g_tris = malloc((size_t)count * sizeof(B3ColTri));
    free(g_seen);
    g_seen = calloc(count, sizeof(*g_seen));
    g_ntris = 0;
    g_min[0] = g_min[1] = g_min[2] = 1e30f;
    g_max[0] = g_max[1] = g_max[2] = -1e30f;
    for (unsigned i = 0; i < count; i++) {
        unsigned char rec[40];
        if (fread(rec, 1, sizeof rec, f) != sizeof rec) break;
        B3ColTri* t = &g_tris[g_ntris++];
        float v[9];
        memcpy(v, rec, 36);
        // game -> GL: negate Z and swap v1/v2 (keeps one-sided orientation)
        t->v0[0] = v[0]; t->v0[1] = v[1]; t->v0[2] = -v[2];
        t->v1[0] = v[6]; t->v1[1] = v[7]; t->v1[2] = -v[8];
        t->v2[0] = v[3]; t->v2[1] = v[4]; t->v2[2] = -v[5];
        memcpy(&t->type, rec + 36, 2);
        t->unit = rec[38];
        /* The exclusion set must be the RACING gather's (FUN_0011BBE0,
         * called per frame by FUN_0011BE50 @0x0011BF48): skip low bytes
         * 0x22/0x23 and anything with type bit 0x1000. The baked rec[39]
         * carried the WRECK gather's set (FUN_00109CE0: 0x20/22/23/24),
         * which made the collidable chevron boards (type 0x0020)
         * drive-through and paired one-sided armco (0x1417 back plates +
         * top caps) solid from both sides. [C] 0x0011BBFE/0x0011BC03/
         * 0x0011BC08; the gather's two runtime velocity/normal filters
         * are applied at query time where velocity exists. */
        t->excl = ((t->type & 0xFF) == 0x22) || ((t->type & 0xFF) == 0x23)
               || (t->type & 0x1000) != 0;
        float e1[3] = {t->v1[0]-t->v0[0], t->v1[1]-t->v0[1], t->v1[2]-t->v0[2]};
        float e2[3] = {t->v2[0]-t->v0[0], t->v2[1]-t->v0[1], t->v2[2]-t->v0[2]};
        t->n[0] = e1[1]*e2[2] - e1[2]*e2[1];
        t->n[1] = e1[2]*e2[0] - e1[0]*e2[2];
        t->n[2] = e1[0]*e2[1] - e1[1]*e2[0];
        float l = sqrtf(t->n[0]*t->n[0] + t->n[1]*t->n[1] + t->n[2]*t->n[2]);
        if (l > 1e-12f) { t->n[0] /= l; t->n[1] /= l; t->n[2] /= l; }
        for (int k = 0; k < 3; k++) {
            float lo = fminf(t->v0[k], fminf(t->v1[k], t->v2[k]));
            float hi = fmaxf(t->v0[k], fmaxf(t->v1[k], t->v2[k]));
            if (lo < g_min[k]) g_min[k] = lo;
            if (hi > g_max[k]) g_max[k] = hi;
        }
    }
    fclose(f);

    // grid
    g_gw = (int)((g_max[0] - g_min[0]) / B3C_CELL) + 2;
    g_gh = (int)((g_max[2] - g_min[2]) / B3C_CELL) + 2;
    int cells = g_gw * g_gh;
    g_cell_start = calloc(cells, sizeof(int));
    g_cell_count = calloc(cells, sizeof(int));
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            int total = 0;
            for (int c = 0; c < cells; c++) {
                g_cell_start[c] = total;
                total += g_cell_count[c];
                g_cell_count[c] = 0;
            }
            g_cell_idx = malloc((size_t)total * sizeof(int));
        }
        for (int i = 0; i < g_ntris; i++) {
            const B3ColTri* t = &g_tris[i];
            float x0 = fminf(t->v0[0], fminf(t->v1[0], t->v2[0]));
            float x1 = fmaxf(t->v0[0], fmaxf(t->v1[0], t->v2[0]));
            float z0 = fminf(t->v0[2], fminf(t->v1[2], t->v2[2]));
            float z1 = fmaxf(t->v0[2], fmaxf(t->v1[2], t->v2[2]));
            int cx0, cz0, cx1, cz1;
            cell_of(x0, z0, &cx0, &cz0);
            cell_of(x1, z1, &cx1, &cz1);
            for (int cz = cz0; cz <= cz1; cz++) {
                for (int cx = cx0; cx <= cx1; cx++) {
                    if (cx < 0 || cz < 0 || cx >= g_gw || cz >= g_gh) continue;
                    int c = cz * g_gw + cx;
                    if (pass == 0) g_cell_count[c]++;
                    else g_cell_idx[g_cell_start[c] + g_cell_count[c]++] = i;
                }
            }
        }
    }
    return g_ntris;
}

// 1:1 port of FUN_001b2230.  Returns parametric t in [~0,1] or -1.
static float b3c_ray_points(const float* A, const float* B,
                             const float* v0, const float* v1,
                             const float* v2) {
    float d[3]  = {B[0]-A[0], B[1]-A[1], B[2]-A[2]};
    float e1[3] = {v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2]};
    float e2[3] = {v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2]};
    float P[3]  = {d[1]*e2[2]-d[2]*e2[1], d[2]*e2[0]-d[0]*e2[2], d[0]*e2[1]-d[1]*e2[0]};
    float det = e1[0]*P[0] + e1[1]*P[1] + e1[2]*P[2];
    if (!(det > B3C_DET_EPS)) return -1.0f;
    float lo = det * B3C_LO_K, hi = det * B3C_HI_K;
    float T[3] = {A[0]-v0[0], A[1]-v0[1], A[2]-v0[2]};
    float u = T[0]*P[0] + T[1]*P[1] + T[2]*P[2];
    if (!(u > lo && u <= hi)) return -1.0f;
    float Q[3] = {T[1]*e1[2]-T[2]*e1[1], T[2]*e1[0]-T[0]*e1[2], T[0]*e1[1]-T[1]*e1[0]};
    float v = d[0]*Q[0] + d[1]*Q[1] + d[2]*Q[2];
    if (!(v > lo)) return -1.0f;
    if (u + v > hi) return -1.0f;
    float t = e2[0]*Q[0] + e2[1]*Q[1] + e2[2]*Q[2];
    if (!(t > lo && t <= hi)) return -1.0f;
    return t / det;
}

static float b3c_ray_tri(const float* A, const float* B, const B3ColTri* tri) {
    return b3c_ray_points(A, B, tri->v0, tri->v1, tri->v2);
}

// Vertical segment query used by the probe: min parametric t over the cell's
// triangles (FUN_00123790's winner rule; excluded types never in the soup).
static int b3c_down_ray(const float* A, const float* B,
                        int unit_filter, float* out_t, int* out_tri) {
    int cx, cz;
    cell_of(A[0], A[2], &cx, &cz);
    if (cx < 0 || cz < 0 || cx >= g_gw || cz >= g_gh) return 0;
    int c = cz * g_gw + cx;
    float best = 999.0f;
    int best_i = -1;
    for (int k = 0; k < g_cell_count[c]; k++) {
        int i = g_cell_idx[g_cell_start[c] + k];
        const B3ColTri* tri = &g_tris[i];
        if (unit_filter >= 0 && tri->unit != unit_filter) continue;
        if (tri->excl) continue;
        float t = b3c_ray_tri(A, B, tri);
        if (t >= 0.0f && t < best) { best = t; best_i = i; }
    }
    if (best_i < 0) return 0;
    *out_t = best;
    *out_tri = best_i;
    return 1;
}

int b3_ground_probe_unit(float x, float y, float z,
                         float* out_height, float out_normal[3],
                         unsigned char* out_unit) {
    if (out_unit) *out_unit = 0xff;
    if (!g_ntris) return -1;
    // FUN_001239C0's under-body ray is (frame pos, 30 units straight down);
    // starting 2 above the query point keeps a car sunk half a wheel into
    // the surface on the hit side of its own road.
    float A[3] = {x, y + 2.0f, z};
    float B[3] = {x, y - 28.0f, z};
    float t;
    int i;
    if (!b3c_down_ray(A, B, -1, &t, &i)) return -1;
    if (out_height) *out_height = A[1] + (B[1] - A[1]) * t;
    if (out_normal) memcpy(out_normal, g_tris[i].n, 3 * sizeof(float));
    if (out_unit) *out_unit = g_tris[i].unit;
    return (int)g_tris[i].type;
}

int b3_ground_probe(float x, float y, float z,
                    float* out_height, float out_normal[3]) {
    return b3_ground_probe_unit(x, y, z, out_height, out_normal, NULL);
}

// Closest point on triangle to p (standard Ericson), for the sphere sweep.
static void closest_on_tri(const B3ColTri* t, const float* p, float* out) {
    const float *a = t->v0, *b = t->v1, *c = t->v2;
    float ab[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]};
    float ac[3] = {c[0]-a[0], c[1]-a[1], c[2]-a[2]};
    float ap[3] = {p[0]-a[0], p[1]-a[1], p[2]-a[2]};
    float d1 = ab[0]*ap[0]+ab[1]*ap[1]+ab[2]*ap[2];
    float d2 = ac[0]*ap[0]+ac[1]*ap[1]+ac[2]*ap[2];
    if (d1 <= 0.0f && d2 <= 0.0f) { memcpy(out, a, 12); return; }
    float bp[3] = {p[0]-b[0], p[1]-b[1], p[2]-b[2]};
    float d3 = ab[0]*bp[0]+ab[1]*bp[1]+ab[2]*bp[2];
    float d4 = ac[0]*bp[0]+ac[1]*bp[1]+ac[2]*bp[2];
    if (d3 >= 0.0f && d4 <= d3) { memcpy(out, b, 12); return; }
    float vc = d1*d4 - d3*d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float w = d1 / (d1 - d3);
        for (int k = 0; k < 3; k++) out[k] = a[k] + w * ab[k];
        return;
    }
    float cp[3] = {p[0]-c[0], p[1]-c[1], p[2]-c[2]};
    float d5 = ab[0]*cp[0]+ab[1]*cp[1]+ab[2]*cp[2];
    float d6 = ac[0]*cp[0]+ac[1]*cp[1]+ac[2]*cp[2];
    if (d6 >= 0.0f && d5 <= d6) { memcpy(out, c, 12); return; }
    float vb = d5*d2 - d1*d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        for (int k = 0; k < 3; k++) out[k] = a[k] + w * ac[k];
        return;
    }
    float va = d3*d6 - d5*d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; k++) out[k] = b[k] + w * (c[k] - b[k]);
        return;
    }
    float den = 1.0f / (va + vb + vc);
    float vv = vb * den, ww = vc * den;
    for (int k = 0; k < 3; k++) out[k] = a[k] + ab[k]*vv + ac[k]*ww;
}

int b3_collision_is_structure(unsigned short type) {
    int lo = type & 0xFF;
    return lo >= B3_COL_STRUCT_LO && lo <= B3_COL_STRUCT_HI;
}

// FUN_0011BBE0's two RUNTIME filters (the header carries the full listing and
// every address).  The first three tests of that callback are static and
// already baked into the loader's `excl` byte; these two need the car's
// velocity / the normal, so they run at query time.
//
// COORDINATES.  The loader stores M(n), M(x,y,z) = (x,y,-z): it negates z AND
// swaps v1/v2, and for a reflection M a x M b = -M(a x b), so the two sign
// flips cancel and the stored normal is exactly the game normal reflected.
// Both filters are invariant under that map -- M(n).y == n.y and
// dot(M v, M n) == dot(v, n) -- so they evaluate directly on the stored
// normal against a harness-space velocity.
//
// NOTE the down-ray (b3c_down_ray / b3_ground_probe) deliberately does NOT
// run these.  Its differential oracle in validate_gameplay is the WRECK
// gather FUN_00109CE0 + the wheel ray FUN_00123790, not FUN_0011BBE0, and it
// has no velocity input.  Filter (b) is provably a no-op there anyway: a
// triangle with n.y < -0.7 faces down, so a downward segment can only reach
// its BACK face, which the one-sided det > 0 test already rejects.
static int gather_runtime_skip(const B3ColTri* t, const float* vel) {
    if (t->n[1] < B3_COL_GATHER_NY_MIN) return 1;             // 0x0011BC43
    if (vel && b3_collision_is_structure(t->type)) {           // 0x0011BC0D/15
        float j = vel[0]*t->n[0] + vel[1]*t->n[1] + vel[2]*t->n[2];
        if (j > B3_COL_GATHER_VDOT_MAX) return 1;              // 0x0011BC32
    }
    return 0;
}

int b3_sweep_sphere(const float* from, const float* to, float radius,
                    float wall_ny_max, float* hit_pos, float* hit_normal) {
    return b3_sweep_sphere_ex(from, to, radius, wall_ny_max, NULL,
                              hit_pos, hit_normal, NULL);
}

int b3_sweep_sphere_ex(const float* from, const float* to, float radius,
                       float wall_ny_max, const float* vel,
                       float* hit_pos, float* hit_normal,
                       unsigned short* hit_type) {
    if (!g_ntris) return 0;
    float d[3] = {to[0]-from[0], to[1]-from[1], to[2]-from[2]};
    float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    int steps = (int)(len / (radius * 0.5f)) + 1;
    for (int s = 0; s <= steps; s++) {
        float f = (float)s / (float)steps;
        float p[3] = {from[0] + d[0]*f, from[1] + d[1]*f, from[2] + d[2]*f};
        int cx0, cz0, cx1, cz1;
        cell_of(p[0] - radius, p[2] - radius, &cx0, &cz0);
        cell_of(p[0] + radius, p[2] + radius, &cx1, &cz1);
        float best_d2 = radius * radius;
        int best_i = -1;
        float best_q[3] = {0, 0, 0};
        for (int cz = cz0; cz <= cz1; cz++) for (int cx = cx0; cx <= cx1; cx++) {
            if (cx < 0 || cz < 0 || cx >= g_gw || cz >= g_gh) continue;
            int c = cz * g_gw + cx;
            for (int k = 0; k < g_cell_count[c]; k++) {
                int i = g_cell_idx[g_cell_start[c] + k];
                const B3ColTri* t = &g_tris[i];
                if (t->excl) continue;
                if (gather_runtime_skip(t, vel)) continue;    // FUN_0011BBE0
                if (fabsf(t->n[1]) > wall_ny_max) continue;   // not a wall
                float q[3];
                closest_on_tri(t, p, q);
                float dx = p[0]-q[0], dy = p[1]-q[1], dz = p[2]-q[2];
                // ONE-SIDED, like the game's det>0 ray test: a wall only
                // blocks from its front (winding) side. The data relies on
                // this -- fences are PAIRS of offset one-sided faces, the
                // tall course-boundary walls single one-sided quads, so a
                // car that ends up behind one can always come back through.
                if (dx*t->n[0] + dy*t->n[1] + dz*t->n[2] < 0.0f) continue;
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_i = i;
                    memcpy(best_q, q, 12);
                }
            }
        }
        if (best_i >= 0) {
            if (hit_pos) memcpy(hit_pos, best_q, 12);
            if (hit_type) *hit_type = g_tris[best_i].type;
            if (hit_normal) {
                // push direction: from the contact point toward the sphere
                // centre; degenerate (centre on the surface) falls back to
                // the triangle normal oriented toward the centre
                const B3ColTri* t = &g_tris[best_i];
                float dx = p[0]-best_q[0], dy = p[1]-best_q[1], dz = p[2]-best_q[2];
                float dl = sqrtf(dx*dx + dy*dy + dz*dz);
                if (dl > 1e-6f) {
                    hit_normal[0] = dx / dl; hit_normal[1] = dy / dl;
                    hit_normal[2] = dz / dl;
                } else {
                    float dn = dx*t->n[0] + dy*t->n[1] + dz*t->n[2];
                    float sgn = dn < 0.0f ? -1.0f : 1.0f;
                    hit_normal[0] = t->n[0]*sgn;
                    hit_normal[1] = t->n[1]*sgn;
                    hit_normal[2] = t->n[2]*sgn;
                }
            }
            return 1;
        }
    }
    return 0;
}

int b3_collision_gather_walls(const float center[3], const float half[3],
                               const float* vel, float wall_ny_max,
                               B3CollisionPoly* out, int cap) {
    if (!g_ntris || !g_seen || !out || cap <= 0) return 0;
    int cx0, cz0, cx1, cz1;
    cell_of(center[0] - half[0], center[2] - half[2], &cx0, &cz0);
    cell_of(center[0] + half[0], center[2] + half[2], &cx1, &cz1);
    if (++g_seen_stamp == 0) {
        memset(g_seen, 0, (size_t)g_ntris * sizeof(*g_seen));
        g_seen_stamp = 1;
    }
    int count = 0;
    for (int cz = cz0; cz <= cz1; cz++) for (int cx = cx0; cx <= cx1; cx++) {
        if (cx < 0 || cz < 0 || cx >= g_gw || cz >= g_gh) continue;
        int cell = cz * g_gw + cx;
        for (int k = 0; k < g_cell_count[cell]; k++) {
            int index = g_cell_idx[g_cell_start[cell] + k];
            if (g_seen[index] == g_seen_stamp) continue;
            g_seen[index] = g_seen_stamp;
            const B3ColTri* tri = &g_tris[index];
            if (tri->excl || gather_runtime_skip(tri, vel)
                || fabsf(tri->n[1]) > wall_ny_max)
                continue;
            float min_y = fminf(tri->v0[1], fminf(tri->v1[1], tri->v2[1]));
            float max_y = fmaxf(tri->v0[1], fmaxf(tri->v1[1], tri->v2[1]));
            if (max_y < center[1] - half[1] || min_y > center[1] + half[1])
                continue;
            B3CollisionPoly* poly = &out[count++];
            memcpy(poly->v0, tri->v0, sizeof(poly->v0));
            memcpy(poly->v1, tri->v1, sizeof(poly->v1));
            memcpy(poly->v2, tri->v2, sizeof(poly->v2));
            memcpy(poly->normal, tri->n, sizeof(poly->normal));
            poly->type = tri->type;
            if (count == cap) return count;
        }
    }
    return count;
}

int b3_collision_gather(const float center[3], const float half[3],
                        B3CollisionPoly* out, int cap) {
    if (!g_ntris || !g_seen || !out || cap <= 0) return 0;
    int cx0, cz0, cx1, cz1;
    cell_of(center[0] - half[0], center[2] - half[2], &cx0, &cz0);
    cell_of(center[0] + half[0], center[2] + half[2], &cx1, &cz1);
    if (++g_seen_stamp == 0) {
        memset(g_seen, 0, (size_t)g_ntris * sizeof(*g_seen));
        g_seen_stamp = 1;
    }
    int count = 0;
    for (int cz = cz0; cz <= cz1; cz++) for (int cx = cx0; cx <= cx1; cx++) {
        if (cx < 0 || cz < 0 || cx >= g_gw || cz >= g_gh) continue;
        int cell = cz * g_gw + cx;
        for (int k = 0; k < g_cell_count[cell]; k++) {
            int index = g_cell_idx[g_cell_start[cell] + k];
            if (g_seen[index] == g_seen_stamp) continue;
            g_seen[index] = g_seen_stamp;
            const B3ColTri* tri = &g_tris[index];
            if (tri->excl) continue;
            float min_y = fminf(tri->v0[1], fminf(tri->v1[1], tri->v2[1]));
            float max_y = fmaxf(tri->v0[1], fmaxf(tri->v1[1], tri->v2[1]));
            if (max_y < center[1] - half[1] || min_y > center[1] + half[1])
                continue;
            B3CollisionPoly* poly = &out[count++];
            memcpy(poly->v0, tri->v0, sizeof(poly->v0));
            memcpy(poly->v1, tri->v1, sizeof(poly->v1));
            memcpy(poly->v2, tri->v2, sizeof(poly->v2));
            memcpy(poly->normal, tri->n, sizeof(poly->normal));
            poly->type = tri->type;
            if (count == cap) return count;
        }
    }
    return count;
}

int b3_collision_filter_walls(const B3CollisionPoly* input, int input_count,
                              const float center[3], const float half[3],
                              const float* vel, float wall_ny_max,
                              B3CollisionPoly* out, int cap) {
    if (!input || input_count <= 0 || !center || !half || !out || cap <= 0)
        return 0;
    int count = 0;
    for (int index = 0; index < input_count; index++) {
        const B3CollisionPoly* poly = &input[index];
        if (poly->normal[1] < B3_COL_GATHER_NY_MIN
            || fabsf(poly->normal[1]) > wall_ny_max)
            continue;
        if (vel && b3_collision_is_structure(poly->type)) {
            float dot = vel[0] * poly->normal[0]
                      + vel[1] * poly->normal[1]
                      + vel[2] * poly->normal[2];
            if (dot > B3_COL_GATHER_VDOT_MAX) continue;
        }
        float min_y = fminf(poly->v0[1], fminf(poly->v1[1], poly->v2[1]));
        float max_y = fmaxf(poly->v0[1], fmaxf(poly->v1[1], poly->v2[1]));
        if (max_y < center[1] - half[1] || min_y > center[1] + half[1])
            continue;
        out[count++] = *poly;
        if (count == cap) break;
    }
    return count;
}

int b3_collision_ray_polys(const B3CollisionPoly* polys, int count,
                           const float start[3], const float end[3],
                           float* hit_t, float normal[3]) {
    if (!polys || count <= 0 || !start || !end) return -1;
    float best = 999.0f;
    int best_i = -1;
    for (int i = 0; i < count; i++) {
        float t = b3c_ray_points(start, end, polys[i].v0, polys[i].v1,
                                 polys[i].v2);
        if (t >= 0.0f && t < best) {
            best = t;
            best_i = i;
        }
    }
    if (best_i < 0) return -1;
    if (hit_t) *hit_t = best;
    if (normal) memcpy(normal, polys[best_i].normal, 3 * sizeof(float));
    return polys[best_i].type;
}

int b3_collision_ray_polys_game_space(const B3CollisionPoly* polys,
                                      int count, const float start[3],
                                      const float end[3], float* hit_t,
                                      float normal[3]) {
    float a[3] = {start[0], start[1], -start[2]};
    float b[3] = {end[0], end[1], -end[2]};
    int surface = b3_collision_ray_polys(polys, count, a, b, hit_t, normal);
    if (surface >= 0 && normal) normal[2] = -normal[2];
    return surface;
}

#ifdef B3_COLLISION_TEST_MAIN
// Differential test harness for tools/validate_gameplay.py: reads
// "x y0 y1 z [unit]" probe segments in RAW GAME coordinates from stdin, answers
// "hit surface_y nx ny nz type" per line (normal back in game space).
// The segment is cast through the same b3c_down_ray core b3_ground_probe
// wraps; explicit endpoints let the validator match the game's exact ray.
int main(int argc, char** argv) {
    if (b3_collision_load(argc > 1 ? argv[1] : "build/collision.bin") <= 0) {
        fprintf(stderr, "cannot load collision.bin\n");
        return 1;
    }
    float x, y0, y1, z;
    int unit;
    while (scanf("%f %f %f %f %d", &x, &y0, &y1, &z, &unit) == 5) {
        float A[3] = {x, y0, -z}, B[3] = {x, y1, -z};   // game -> GL
        float t;
        int i;
        if (b3c_down_ray(A, B, unit, &t, &i)) {
            const B3ColTri* tr = &g_tris[i];
            printf("1 %.6f %.6f %.6f %.6f %u\n",
                   A[1] + (B[1] - A[1]) * t,
                   tr->n[0], tr->n[1], -tr->n[2],       // GL -> game normal
                   (unsigned)tr->type);
        } else {
            printf("0 0 0 0 0 0\n");
        }
    }
    return 0;
}
#endif
