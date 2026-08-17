// burnout3_crash.c -- Burnout 3's crash/collision response, ported from the
// retail XBE and verified by differential execution (tools/validate_gameplay.py
// "crash response" section runs the REAL x86 under Unicorn and asserts these
// mirrors reproduce its writes).
//
// Sources (all addresses = analyzed burnout3.elf, image base 0x10000):
//   FUN_0011AEF0   chassis-vs-world collision response        [C]
//   FUN_0011AC30   per-poly contact test + accumulation       [C]
//   FUN_0011ABB0   wall-sliver edge test                      [C]
//   FUN_001B0C00 / FUN_001B09C0  triangle-vs-bbox clipper     [C]
//   FUN_00106720   contact impulse magnitude                  [C]
//   FUN_00106500   apply impulse at point (linear + angular)  [C]
//   FUN_001206D0   impulse routing (drift / yaw-lock gate)    [C]
//   FUN_001066A0   point velocity                             [C]
//   FUN_00123000   crash-mode damping constants               [S-disasm]
//
// Data constants, read from the mapped image (never guessed):
//   0x3B1744 -1000.0   0x3B168C 1.0      0x3B191C 2.3283064e-10
//   0x3895C8 0.33      0x3B1870 1.5      0x3B1A20 0.707
//   0x3A69C4 0.1       0x3B1758 0.99     0x3B16C0 -1.0
//   0x3B16E0 0.0       0x3B1E18 89.408   0x3B18B8 1.75
//   0x3A69C0 0.9       0x3B1E14 0.011184681  0x3B1A68 0.175
//   0x3A7F34 10.0      0x39B308 0.303    0x39B2FC 27.5
//   0x3EBF64 3.0       0x3A69B4 0.2      0x3B1684 0.5
//   0x39B2B0 0.35      0x3B17D8 0.7

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "burnout3_crash.h"
#include "burnout3_vehicle_sim.h"

/* CRASH-AUDIT instrumentation.  B3_WALLGATE=1 prints one line per APPLIED
 * chassis-vs-world wall resolve whose impact block ran (dv > 0), so the
 * in-substep trigger's own inputs -- authority, dv, headon, surface -- can be
 * read live instead of through the td_rules record (which the substep arm
 * never populates).  Off by default; costs one getenv per process. */
int b3_wallgate_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("B3_WALLGATE") != NULL;
    return on;
}
void* g_b3_wallgate_who = 0;      /* set by the caller before the resolve */
float g_b3_wallgate_clock = 0.0f;

/* The last APPLIED wall evaluation, published for reporting only (see the
 * header).  The td_rules arm runs with apply == 0 and never lands here. */
static B3CrashWallEval g_last_wall_eval;
static int             g_last_wall_eval_valid = 0;

int b3_crash_last_wall_eval(B3CrashWallEval* out) {
    if (out && g_last_wall_eval_valid) *out = g_last_wall_eval;
    return g_last_wall_eval_valid;
}

// ---------------------------------------------------------------------------
// vec helpers mirroring the SSE idioms (4-wide where the real code is 4-wide)
// ---------------------------------------------------------------------------
static float dot3(const float a[4], const float b[4]) {
    // SSE horizontal add order: (x + y) + z
    return (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2];
}

// FUN_00011640 [C]: normalize by the 3-component length; w is scaled too.
static void normalize4(float v[4]) {
    float s = 1.0f / sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] *= s; v[1] *= s; v[2] *= s; v[3] *= s;
}

// FUN_00013CA0 [C]: row-vector point transform, out = r0*x + r1*y + r2*z + r3.
static void frame_point(const float m[4][4], const float p[4], float out[4]) {
    for (int i = 0; i < 4; i++)
        out[i] = ((p[0] * m[0][i] + m[3][i]) + p[1] * m[1][i]) + p[2] * m[2][i];
}

// ---------------------------------------------------------------------------
// FUN_001B09C0 [C]: clip a closed polygon against [lo, hi] on one component.
// Vertex regions: 1 below lo, 2 above hi, 0 inside (v==lo -> inside test path,
// v==hi -> inside; boundary semantics mirrored from the COMISS chain).
// ---------------------------------------------------------------------------
int b3_crash_clip_axis(float out[][4], const float in[][4], int n, int axis,
                       float lo, float hi) {
    int cnt = 0;
    float pv = in[n - 1][axis];
    int prev = (lo <= pv) ? ((pv <= hi) ? 0 : 2) : 1;
    for (int i = 0; i < n; i++) {
        float cv = in[i][axis];
        int cur = (lo <= cv) ? ((cv <= hi) ? 0 : 2) : 1;
        const float* vp = in[(i == 0 ? n : i) - 1];
        if (prev == cur) {
            if (cur == 0) {
                for (int k = 0; k < 4; k++) out[cnt][k] = in[i][k];
                cnt++;
            }
        } else {
            float inv = 1.0f / (cv - pv);
            if (prev == 1) {
                float t = (lo - pv) * inv, u = 1.0f - t;
                for (int k = 0; k < 4; k++)
                    out[cnt][k] = in[i][k] * t + vp[k] * u;
                cnt++;
            } else if (prev == 2) {
                float t = (hi - pv) * inv, u = 1.0f - t;
                for (int k = 0; k < 4; k++)
                    out[cnt][k] = in[i][k] * t + vp[k] * u;
                cnt++;
            }
            if (cur == 1) {
                float t = (lo - pv) * inv, u = 1.0f - t;
                for (int k = 0; k < 4; k++)
                    out[cnt][k] = in[i][k] * t + vp[k] * u;
                cnt++;
            } else if (cur == 2) {
                float t = (hi - pv) * inv, u = 1.0f - t;
                for (int k = 0; k < 4; k++)
                    out[cnt][k] = in[i][k] * t + vp[k] * u;
                cnt++;
            } else {
                for (int k = 0; k < 4; k++) out[cnt][k] = in[i][k];
                cnt++;
            }
        }
        prev = cur;
        pv = cv;
    }
    return cnt;
}

// FUN_001B0C00 [C]: slab order x (bb.x), z (bb.z), y (bb.y); >= 3 verts at
// every stage or 0. XMM7 = [box+0x00 block] = max side, XMM6 = min side.
int b3_crash_box_clip(const float bbmax[4], const float bbmin[4],
                      const float tri[3][4], float out[][4]) {
    float a[9][4], b[9][4];
    int n = b3_crash_clip_axis(a, tri, 3, 0, bbmin[0], bbmax[0]);
    if (n < 3) return 0;
    n = b3_crash_clip_axis(b, (const float(*)[4])a, n, 2, bbmin[2], bbmax[2]);
    if (n < 3) return 0;
    n = b3_crash_clip_axis(out, (const float(*)[4])b, n, 1, bbmin[1], bbmax[1]);
    return (n < 3) ? 0 : n;
}

void b3_crash_acc_init(B3CrashContactAcc* acc, const float inv[4][4],
                       const float bbmax[4], const float bbmin[4]) {
    memset(acc, 0, sizeof(*acc));
    memcpy(acc->inv, inv, sizeof(acc->inv));
    memcpy(acc->bbmax, bbmax, 16);
    memcpy(acc->bbmin, bbmin, 16);
}

// FUN_0011ABB0 [C]: vertically-thin, plan-short edge test.
static int sliver_edge(const float a[4], const float c[4]) {
    if (fabsf(c[1] - a[1]) < 0.5f &&
        (c[2] - a[2]) * (c[2] - a[2]) + (c[0] - a[0]) * (c[0] - a[0]) < 0.2f)
        return 1;
    return 0;
}

// FUN_0011AC30 [C] (normal race mode -- the crash-party '&'-poly and
// '&'-wheel skips at its head are gated on FUN_00017310 and are [S]).
void b3_crash_poly_contact(const B3CrashPoly* poly, unsigned short flags,
                           B3CrashContactAcc* acc) {
    // transform the three verts into body space (4-wide MULPS/ADDPS chain)
    float v[3][4];
    frame_point(acc->inv, poly->p0, v[0]);
    frame_point(acc->inv, poly->p1, v[1]);
    frame_point(acc->inv, poly->p2, v[2]);

    // near-vertical polys: reject slivers (|n.y| < 0.2 [0x3A69B4])
    if (fabsf(poly->n[1]) < 0.2f) {
        float ex = v[0][0] - v[1][0];
        float ey = v[0][1] - v[1][1];
        float ez = v[0][2] - v[1][2];
        if (fabsf(ey) < 0.5f && ez * ez + ex * ex < 0.2f) return;
        if (sliver_edge(v[0], v[2])) return;
        if (sliver_edge(v[1], v[2])) return;
    }

    float clip[9][4];
    int n = b3_crash_box_clip(acc->bbmax, acc->bbmin,
                              (const float(*)[4])v, clip);
    if (n < 3) return;

    int ground = poly->n[1] > 0.7f;          // [0x3B17D8]

    float cent[4] = {clip[0][0], clip[0][1], clip[0][2], clip[0][3]};
    for (int i = 1; i < n; i++)
        for (int k = 0; k < 4; k++) cent[k] += clip[i][k];
    float invn = 1.0f / (float)n;
    for (int k = 0; k < 4; k++) cent[k] *= invn;

    if (ground) {
        if (0.35f < cent[1]) return;         // [0x39B2B0]
        for (int k = 0; k < 4; k++) acc->gnd_n[k] += poly->n[k];
        for (int k = 0; k < 4; k++) acc->gnd_cent[k] += cent[k];
        acc->gnd_count++;
    } else {
        for (int k = 0; k < 4; k++) {
            acc->wall_nmin[k] = fminf(acc->wall_nmin[k], poly->n[k]);
            acc->wall_nmax[k] = fmaxf(acc->wall_nmax[k], poly->n[k]);
        }
        for (int k = 0; k < 4; k++) acc->wall_cent[k] += cent[k];
        acc->wall_count++;
    }
    // lowest nonzero low byte wins the surface slot
    if ((acc->flags & 0xFF) == 0 || (flags & 0xFF) < (acc->flags & 0xFF))
        acc->flags = flags;
}

// FUN_001066A0 [C]
void b3_crash_point_velocity(const B3CrashVehicle* v, const float pt[4],
                             float vp[4]) {
    float rx = pt[0] - v->frame[3][0];
    float ry = pt[1] - v->frame[3][1];
    float rz = pt[2] - v->frame[3][2];
    vp[0] = (v->omega[1] * rz - v->omega[2] * ry) + v->vel[0];
    vp[1] = (v->omega[2] * rx - v->omega[0] * rz) + v->vel[1];
    vp[2] = (v->omega[0] * ry - v->omega[1] * rx) + v->vel[2];
    vp[3] = v->vel[3];
}

// FUN_00106720 [C]
//
// THE RETURN VALUE IS SIGNED.  Read the tail literally:
//     00106863  divss xmm0, xmm2          j = -(1+e)*dot(n,vp) / denom
//     0010686a  movss [esp+0xc], xmm0
//     00106870  fld  [esp+0xc] / fabs / fstp [esp+0xc]
//     0010687e  movss xmm2, [esp+0xc]     |j|
//     00106886  mulps xmm1, xmm2          out = n * |j|
//     0010688f  ret 0x10
// XMM0 -- the caller's float return -- still holds the SIGNED quotient; the
// `fabs` travels through the x87 stack and a memory slot into XMM2 and only
// ever scales the OUTPUT VECTOR.  The sign is load bearing:
//     0011b75c  divss xmm3, [edi+0x1f0]   dv = j / mass
//     0011b764  comiss xmm3, [0x3b16e0]   vs 0.0
//     0011b771  jbe 0x11b909              dv <= 0 -> NO impact, NO impulse
// so FUN_0011AEF0 leaves a chassis that is already SEPARATING from a wall
// alone -- it still records the contact and still pushes +0x130 out, but it
// does not kick the car back into the wall.  (`b3_contact_impulse` in
// burnout3_vehicle_sim.c, the wave-1 port of this same function, already
// returned the signed value; this transcription did not, and the in-substep
// relocation's mid-corner trajectory case is what exposed the difference.)
float b3_crash_impulse(const B3CrashVehicle* v, const float n[4],
                       const float pt[4], const float vp[4],
                       float restitution, float out[4]) {
    float rx = pt[0] - v->frame[3][0];
    float ry = pt[1] - v->frame[3][1];
    float rz = pt[2] - v->frame[3][2];
    float cx = ry * n[2] - rz * n[1];
    float cy = rz * n[0] - rx * n[2];
    float cz = rx * n[1] - ry * n[0];
    // Iinv rows dotted column-wise (transpose; Iinv is symmetric)
    const float (*I)[4] = v->iinv_world;
    float tx = I[0][0] * cx + I[1][0] * cy + I[2][0] * cz;
    float ty = I[0][1] * cx + I[1][1] * cy + I[2][1] * cz;
    float tz = I[0][2] * cx + I[1][2] * cy + I[2][2] * cz;
    float j = (0.0f - (restitution + 1.0f) *
               (n[0] * vp[0] + n[1] * vp[1] + n[2] * vp[2]))
              / (1.0f / v->mass +
                 (ty * rz - tz * ry) * n[0] +
                 (tz * rx - tx * rz) * n[1] +
                 (tx * ry - ty * rx) * n[2]);
    const float mag = fabsf(j);                      // @0x00106874
    out[0] = n[0] * mag; out[1] = n[1] * mag;
    out[2] = n[2] * mag; out[3] = n[3] * mag;
    return j;                                        // signed, in XMM0
}

// FUN_00106500 [C]
void b3_crash_apply_impulse_at_point(B3CrashVehicle* v, const float imp[4],
                                     const float pt[4]) {
    for (int k = 0; k < 4; k++) v->imp[k] += imp[k];
    float rx = pt[0] - v->frame[3][0];
    float ry = pt[1] - v->frame[3][1];
    float rz = pt[2] - v->frame[3][2];
    v->ang_imp[0] += ry * imp[2] - rz * imp[1];
    v->ang_imp[1] += rz * imp[0] - rx * imp[2];
    v->ang_imp[2] += rx * imp[1] - ry * imp[0];
    // +0x12C accumulates (r.w*imp.w - r.w*imp.w) == 0 in the real code
}

// FUN_001206D0 [C]: DAT_003EBF64 = 3.0
void b3_crash_apply_impulse(B3CrashVehicle* v, const float imp[4],
                            const float pt[4]) {
    if (v->drift_state != 2 && v->drift_state != 1) {
        if (3.0f < fabsf(v->omega[1])) {
            float t = (pt[2] - v->frame[3][2]) * imp[0]
                    - (pt[0] - v->frame[3][0]) * imp[2];  // yaw torque sign
            // the real code compares sign bits (OR 0x3F800000 trick)
            if (!!signbit(v->omega[1]) == !!signbit(t)) {
                for (int k = 0; k < 4; k++) v->imp[k] += imp[k];
                return;
            }
        }
        b3_crash_apply_impulse_at_point(v, imp, pt);
        return;
    }
    for (int k = 0; k < 4; k++) v->imp[k] += imp[k];
}

// ---------------------------------------------------------------------------
// FUN_0011AEF0's WALL TAIL @0x0011B4F0..0x0011B9F8 [C], factored out so the
// full response and the standalone trigger evaluator below cannot drift
// apart.  `nsum` is the aggregate wall normal ALREADY flattened (y = 0) and
// normalised; `wpt` is the world contact point the impulse acts through.
//
//   apply != 0  -- the real response: scrub the velocity in place, write
//                  veh+0x194, push the impulse into +0x110/+0x120, set
//                  veh+0x15CC / the crash flag.
//   apply == 0  -- evaluate only: the velocity scrubs are replayed onto a
//                  LOCAL copy (the real code computes the point velocity
//                  from the ALREADY-SCRUBBED velocity, so this ordering is
//                  load bearing) and nothing on `v` is touched.
//
// Returns 0 only for the degenerate normal that makes the real function bail
// (|nh|^2 < 2.3283064e-10 @0x0011B4F5).
// ---------------------------------------------------------------------------
static int crash_wall_core(B3CrashVehicle* v, const float nsum[4],
                           const float wpt[4], int apply,
                           B3CrashWallEval* ev) {
    // project n into the right/at plane  @0x0011B4B0..0x0011B4F5
    float dot_r = dot3(nsum, v->frame[0]);
    float prods[4];
    for (int k = 0; k < 4; k++) prods[k] = nsum[k] * v->frame[2][k];
    float dot_a = (prods[0] + prods[1]) + prods[2];
    float nh[4];
    for (int k = 0; k < 4; k++)
        nh[k] = v->frame[2][k] * dot_a + v->frame[0][k] * dot_r;
    if (dot3(nh, nh) < 2.3283064e-10f) return 0;
    normalize4(nh);

    float ddn = fabsf(dot3(v->dir, nsum));                  // |dot(dir, n)|
    float headon = fabsf((prods[0] + prods[1]) + prods[2]); // |dot(n, at)| ESP+0x14

    // the two velocity scrubs, in the real order.  When evaluating we run
    // them on a copy so the caller's state is untouched.
    float vloc[4];
    float* vel = apply ? v->vel : vloc;
    if (!apply) for (int k = 0; k < 4; k++) vloc[k] = v->vel[k];

    // head-on velocity scrub [0x3B1A20 0.707, 0x3A69C4 0.1] @0x0011B55A
    if (0.707f < headon) {
        float k = 1.0f - (headon - 0.707f) * 0.1f;
        vel[0] *= k; vel[1] *= k; vel[2] *= k; vel[3] *= k;
    }
    // surface-grip scrub: class-0 uses +0x13A8 (and reverse gear flips the
    // drift direction); others a plain 0.99 unless +0x153E  @0x0011B5E5
    if (v->is_class0) {
        float g = v->surface_grip;
        vel[0] *= g; vel[1] *= g; vel[2] *= g; vel[3] *= g;
        if (apply && v->gear == -1) v->drift_dir *= -1.0f;
    } else if (!v->no_scrub) {
        vel[0] *= 0.99f; vel[1] *= 0.99f; vel[2] *= 0.99f; vel[3] *= 0.99f;
    }

    // contact impulse along the flattened normal  @0x0011B724..0x0011B771
    float vp[4], iv[4];
    {
        // FUN_001066A0 over the (possibly local) velocity
        float sv[4]; for (int k = 0; k < 4; k++) sv[k] = v->vel[k];
        if (!apply) for (int k = 0; k < 4; k++) ((float*)v->vel)[k] = vloc[k];
        b3_crash_point_velocity(v, wpt, vp);
        float jj = b3_crash_impulse(v, nh, wpt, vp, 0.0f, iv);
        if (!apply) for (int k = 0; k < 4; k++) ((float*)v->vel)[k] = sv[k];
        ev->dv = jj / v->mass;                                  // ESP+0x1c
    }

    ev->headon = headon;
    ev->impact = 0.0f;
    for (int k = 0; k < 4; k++) ev->nh[k] = nh[k];

    if (ev->dv > 0.0f) {                                        // [0x3B16E0]
        float speed_c = vel[3];
        if (speed_c > 89.408f) speed_c = 89.408f;               // [0x3B1E18]
        float headon_c = ddn * 1.75f;                           // [0x3B18B8]
        if (headon_c > 0.9f) headon_c = 0.9f;                   // [0x3A69C0]
        ev->impact = ((1.0f - speed_c * 0.011184681f) * 0.9f
                      + (1.0f - headon_c))
                     * v->mass * ev->dv * 0.175f;               // [0x3B1A68]
        if (apply) {
            v->impact = ev->impact;
            // impulse direction: nh minus its up-row component
            float du = dot3(v->frame[1], nh);
            float d2[4];
            for (int k = 0; k < 4; k++) d2[k] = nh[k] - v->frame[1][k] * du;
            normalize4(d2);
            float impv[4];
            for (int k = 0; k < 4; k++) impv[k] = d2[k] * ev->impact;
            if (v->ground_frac <= 0.1f)     // airborne: torque path
                b3_crash_apply_impulse(v, impv, wpt);
            else
                for (int k = 0; k < 4; k++) v->imp[k] += impv[k];
        }
    }

    // wall-crash trigger decision @0x0011B909..0x0011B9A3.  Both gates are
    // scaled by the driver-authority veh+0x1534 (docs/RE_TD_RULES.md 12).
    ev->fire = 0;
    ev->dv_thr = 0.0f;
    ev->headon_thr = 0.0f;
    if (v->party_mode) {
        ev->dv_thr = v->authority * 10.0f;                      // [0x3A7F34]
        ev->headon_thr = v->authority * 0.303f;                 // [0x39B308]
        if (ev->dv > ev->dv_thr) ev->fire = ev->headon > ev->headon_thr;
    } else {
        ev->dv_thr = v->authority * 27.5f;                      // [0x39B2FC]
        ev->headon_thr = v->authority * 0.707f;                 // [0x3B1A20]
        if ((v->surface & 0xFF) != 0x20 && !(v->flags1353 & 8)
            && ev->dv > ev->dv_thr)
            ev->fire = ev->headon > ev->headon_thr;
    }
    if (ev->fire && v->racecar_class == 2) ev->fire = 0;         // @0x0011B998
    if (apply) { g_last_wall_eval = *ev; g_last_wall_eval_valid = 1; }
    if (apply && b3_wallgate_trace() && ev->dv > 0.0f)
        printf("[WALLGATE] t=%.3f who %p fire=%d dv %.3f > %.3f headon "
               "%.4f > %.4f auth %.3f surf %04X f1353 %02X cls %d spd %.1f\n",
               g_b3_wallgate_clock, g_b3_wallgate_who, ev->fire,
               ev->dv, ev->dv_thr, ev->headon, ev->headon_thr,
               v->authority, (unsigned)(v->surface & 0xFFFF),
               (unsigned)(v->flags1353 & 0xFF), v->racecar_class, v->vel[3]);
    if (apply && ev->fire) {
        v->surf_bit15 = (unsigned char)(v->surface >> 15);
        v->crashed = 1;              // FUN_0010DCA0(&DAT_0064ACE8, v, slot)
    }
    return 1;
}

// FUN_0011AEF0's wall trigger over a caller-supplied contact [C for the
// maths and the gate; the CONTACT GEOMETRY is the caller's].  See the header.
int b3_crash_wall_eval(const B3CrashVehicle* vin, const float pt[4],
                       const float wall_n[4], B3CrashWallEval* out) {
    B3CrashVehicle v = *vin;         // the core never writes with apply == 0,
                                     // but copy anyway so `vin` stays const
    B3CrashWallEval ev;
    memset(&ev, 0, sizeof(ev));
    // aggregate normal for a single contact: min+max == 2n, flattened and
    // renormalised @0x0011B47x -- the same direction as the input.
    float nsum[4] = {wall_n[0], 0.0f, wall_n[2], 0.0f};
    if (dot3(nsum, nsum) < 2.3283064e-10f) { *out = ev; return 0; }
    normalize4(nsum);
    int ok = crash_wall_core(&v, nsum, pt, 0, &ev);
    *out = ev;
    return ok;
}

// FUN_0011AEF0 [C] -- see the header for the full walkthrough.
int b3_crash_response(B3CrashVehicle* v, const B3CrashPoly* polys,
                      const unsigned short* flags, int count) {
    v->crashed = 0;
    if (v->flags1353 & 5) {          // response disabled (ghost/retired bits)
        v->contact_state = 0;
        return 0;
    }

    B3CrashContactAcc acc;
    b3_crash_acc_init(&acc, (const float(*)[4])v->inv, v->bbmax, v->bbmin);

    for (int i = 0; i < count; i++)
        b3_crash_poly_contact(&polys[i], flags[i], &acc);

    if (v->is_class0 && v->set2_count > 0) {
        int pre = acc.wall_count;
        for (int i = 0; i < v->set2_count; i++)
            b3_crash_poly_contact(&v->set2[i], 0x20, &acc);
        if (acc.wall_count > pre) {
            // the -1000*mass*dir wall stop [0x3B1744]
            for (int k = 0; k < 4; k++)
                v->force_acc[k] += v->dir[k] * v->mass * -1000.0f;
        }
    }

    v->contact_state = 0;

    if (acc.wall_count != 0) {
        // ---- wall path (contact state 1) --------------------------------
        float nsum[4];
        for (int k = 0; k < 4; k++)
            nsum[k] = acc.wall_nmin[k] + acc.wall_nmax[k];
        float len2 = dot3(nsum, nsum);
        if (len2 < 2.3283064e-10f) {   // opposing walls cancelled [0x3B191C]
            float d = dot3(v->frame[0], acc.wall_nmax);
            const float* pick = (d < 0.0f) ? acc.wall_nmin : acc.wall_nmax;
            for (int k = 0; k < 4; k++) nsum[k] = pick[k];
        }
        float invc = 1.0f / (float)acc.wall_count;
        float cent[4];
        for (int k = 0; k < 4; k++) cent[k] = acc.wall_cent[k] * invc;
        nsum[1] = 0.0f;                // flattened before normalising
        normalize4(nsum);              // n

        // world contact point: centroid x/z at 1/3 up the collision bbox
        float st[4] = {cent[0],
                       (v->bbmax[1] - v->bbmin[1]) * 0.33f + v->bbmin[1],
                       cent[2], cent[3]};
        frame_point((const float(*)[4])v->frame, st, v->contact_pt);
        v->surface = acc.flags;
        v->impact = 0.0f;
        for (int k = 0; k < 4; k++) v->contact_n[k] = nsum[k];
        v->contact_state = 1;

        // bbox-edge distance of the (unmodified) centroid, x/z faces only
        float ed = fabsf(cent[0] - v->bbmax[0]);
        float t = fabsf(cent[0] - v->bbmin[0]); if (t <= ed) ed = t;
        t = fabsf(cent[2] - v->bbmax[2]);       if (t <= ed) ed = t;
        t = fabsf(cent[2] - v->bbmin[2]);       if (t <= ed) ed = t;

        // the shared wall tail: nh, the scrubs, the impulse, veh+0x194 and
        // the crash-trigger decision.  Deflection first -- the real code
        // writes +0x130 before the scrubs and the two do not interact.
        {
            float dot_r = dot3(nsum, v->frame[0]);
            float prods[4];
            for (int k = 0; k < 4; k++) prods[k] = nsum[k] * v->frame[2][k];
            float dot_a = (prods[0] + prods[1]) + prods[2];
            float nh[4];
            for (int k = 0; k < 4; k++)
                nh[k] = v->frame[2][k] * dot_a + v->frame[0][k] * dot_r;
            if (dot3(nh, nh) < 2.3283064e-10f)
                return 0;              // degenerate: full abort (real code
                                       // returns 0 leaving +0x198 = 1)
            normalize4(nh);
            for (int k = 0; k < 4; k++) v->contact_n[k] = nh[k];
            // deflection push-out: 1.5 x edge distance [0x3B1870]
            for (int k = 0; k < 4; k++) v->defl[k] += nh[k] * (ed * 1.5f);
        }
        float wpt[4];
        frame_point((const float(*)[4])v->frame, cent, wpt);
        B3CrashWallEval ev;
        memset(&ev, 0, sizeof(ev));
        if (!crash_wall_core(v, nsum, wpt, 1, &ev)) return 0;
        // (veh+0x211 == 1 "landed on a car" tail: NOT ported [S])
        return acc.wall_count;
    }

    if (acc.gnd_count != 0) {
        // ---- ground path (contact state 2): record only, no impulse -----
        float invc = 1.0f / (float)acc.gnd_count;
        float cent[4];
        for (int k = 0; k < 4; k++) cent[k] = acc.gnd_cent[k] * invc;
        v->contact_state = 2;
        frame_point((const float(*)[4])v->frame, cent, v->contact_pt);
        float n[4] = {acc.gnd_n[0], acc.gnd_n[1], acc.gnd_n[2], acc.gnd_n[3]};
        normalize4(n);
        for (int k = 0; k < 4; k++) v->contact_n[k] = n[k];
        v->surface = acc.flags;
        float vp[4], iv[4];
        b3_crash_point_velocity(v, v->contact_pt, vp);
        v->impact = b3_crash_impulse(v, v->contact_n, v->contact_pt, vp,
                                     0.0f, iv);
        // the real code stores the world point to +0x160/+0x1A0/+0x1B0 and
        // discards the impulse vector: state 2 only RECORDS the hit (it
        // feeds FUN_0010ED30's probability roll); the keep-out is the
        // suspension/crashed-wheel passes' job.
    }
    return 0;
}

// ===========================================================================
// THE SUBSTEP BRIDGE -- FUN_0011AEF0 as FUN_0011BE50 calls it @0x0011C0B7.
//
// `B3CrashVehicle` is the projection of the live vehicle FUN_0011AEF0 and its
// callees touch; `B3VehicleFull` is the same body with the rest of the
// pipeline attached.  This copies the one into the other and back, so the
// substep can run the response IN PLACE without either file learning the
// other's layout.  Every field below is named by the live offset it mirrors,
// and the write-back set is exactly the set the real function mutates:
//
//   +0xB0/+0xBC velocity + speed      the two scrubs   @0x0011B55A/@0x0011B5E5
//   +0xF0  force accumulator          the class-0 wall stop     @0x0011B1xx
//   +0x110/+0x120 impulse accumulators FUN_00106500 / FUN_001206D0
//   +0x130 deflection                 the 1.5 x edge push-out   @0x0011B6xx
//   +0x160/+0x170/+0x190/+0x194/+0x198  the contact record
//   +0x1434 drift direction           the reverse-gear flip     @0x0011B5E5
//   racecar+0x15CC                    surface >> 15             @0x0011B9D7
//
// veh+0x40 (world inverse inertia) is READ by FUN_00106720 and is rebuilt by
// FUN_00109560; the resolve never writes it.  Nothing else on the body is
// touched, which is why relocating the call site is safe: the accumulators
// are write-only until FUN_00109560 drains them.
// ===========================================================================
int b3_vehicle_chassis_contact(B3VehicleFull* v) {
    B3RigidBody* rb = &v->rb;
    B3CrashVehicle cv;
    int n;

    memset(&cv, 0, sizeof cv);
    memcpy(cv.frame, rb->frame, sizeof cv.frame);            // +0x204
    memcpy(cv.inv, rb->inv_frame, sizeof cv.inv);            // +0x70
    memcpy(cv.iinv_world, rb->inv_inertia_world, sizeof cv.iinv_world);
    memcpy(cv.vel, rb->vel, sizeof cv.vel);                  // +0xB0
    memcpy(cv.dir, rb->dir, sizeof cv.dir);                  // +0xC0
    memcpy(cv.omega, rb->omega, sizeof cv.omega);            // +0xD0
    memcpy(cv.force_acc, rb->force_acc, sizeof cv.force_acc);// +0xF0
    memcpy(cv.imp, rb->imp_force, sizeof cv.imp);            // +0x110
    memcpy(cv.ang_imp, rb->imp_torque, sizeof cv.ang_imp);   // +0x120
    memcpy(cv.defl, rb->deflection, sizeof cv.defl);         // +0x130
    memcpy(cv.bbmax, v->half_ext, sizeof cv.bbmax);          // +0x1D0
    memcpy(cv.bbmin, v->center_off, sizeof cv.bbmin);        // +0x1E0
    cv.mass          = v->mass;                              // +0x1F0
    cv.surface_grip  = v->surface_grip_13A8;
    cv.ground_frac   = v->brake_1404;                        // +0x1404
    cv.drift_dir     = v->drift_dir_1434;
    cv.drift_state   = v->drift_state_1524;
    cv.authority     = v->authority_1534;
    cv.no_scrub      = v->no_scrub_153E;
    cv.flags1353     = v->flags_1353;
    cv.gear          = v->trans.gear;                        // +0x14C8
    cv.racecar_class = v->racecar_class_1920;
    cv.is_class0     = v->is_class0;
    cv.party_mode    = v->party_mode;
    // The CONTACT RECORD carries IN.  FUN_0011AEF0 works on the LIVE vehicle,
    // so a substep that finds nothing leaves the previous substep's record
    // (+0x160 point, +0x170 normal, +0x190 surface, +0x194 impact) standing;
    // only +0x198 is rewritten unconditionally, to 0.  Nothing inside
    // FUN_0011BE50 clears the block, and nothing outside it does either on
    // the per-frame path: FUN_00104840 zeroes exactly +0x160..+0x19F and
    // +0x212 @0x00104856..0x00104888, but it ALSO sets +0x1353 |= 4
    // @0x00104848 -- the bit that DISABLES FUN_0011AEF0 -- and its callers are
    // FUN_00120F30's out-of-unit / parked arms (@0x00120F7B, @0x00120FB7), so
    // it is a reset path, not a frame tick.  [S] for "the record persists";
    // [C] for the block FUN_00104840 covers and for the disable bit.
    // Zeroing the record here instead made the second substep of every frame
    // publish impact = 0 -- caught by the head-on trajectory case at frame 0.
    memcpy(cv.contact_pt, v->contact_pt_160, sizeof cv.contact_pt);
    memcpy(cv.contact_n, v->contact_n_170, sizeof cv.contact_n);
    cv.surface       = v->surface_190;
    cv.impact        = v->impact_194;
    cv.contact_state = v->contact_state_198;

    g_b3_wallgate_clock = v->clock;
    g_b3_wallgate_who   = v->soup_user;
    n = b3_crash_response(&cv, v->soup.polys, v->soup.flags, v->soup.count);

    memcpy(rb->vel, cv.vel, sizeof cv.vel);
    memcpy(rb->force_acc, cv.force_acc, sizeof cv.force_acc);
    memcpy(rb->imp_force, cv.imp, sizeof cv.imp);
    memcpy(rb->imp_torque, cv.ang_imp, sizeof cv.ang_imp);
    memcpy(rb->deflection, cv.defl, sizeof cv.defl);
    memcpy(v->contact_pt_160, cv.contact_pt, sizeof cv.contact_pt);
    memcpy(v->contact_n_170, cv.contact_n, sizeof cv.contact_n);
    v->drift_dir_1434    = cv.drift_dir;
    v->surface_190       = cv.surface;
    v->impact_194        = cv.impact;
    v->contact_state_198 = cv.contact_state;
    if (cv.crashed) {
        B3CrashWallEval we;
        v->crash_fired      = 1;
        v->surf_bit15_15CC  = cv.surf_bit15;
        if (b3_crash_last_wall_eval(&we)) {   // reporting only, see the header
            v->crash_dv         = we.dv;
            v->crash_dv_thr     = we.dv_thr;
            v->crash_headon     = we.headon;
            v->crash_headon_thr = we.headon_thr;
        }
    }
    return n;
}

// ===========================================================================
// THE CRASH-ENTRY KICK -- FUN_00125100 / FUN_00125380 [C]
//
// Constants, all read from the mapped image:
//   0x0041A504 10.0   impulse scale      0x0041A500 1.2  fore/aft mag scale
//   0x003A5600 0.8    inverted roof arm
// ===========================================================================
void b3_crash_kick(B3CrashVehicle* v, unsigned flags, float mag,
                   const float axis[4]) {
    // application point: starts at the frame origin (frame row3)
    float P[4] = {v->frame[3][0], v->frame[3][1],
                  v->frame[3][2], v->frame[3][3]};

    // 0x0012510C / 0x00125130: lateral offset (bit3 = left, bit2 = right)
    if (flags & 0x08) {
        float s = 0.0f - v->bbmax[0];
        for (int k = 0; k < 4; k++) P[k] += v->frame[0][k] * s;
    } else if (flags & 0x04) {
        float s = 0.0f - v->bbmin[0];
        for (int k = 0; k < 4; k++) P[k] += v->frame[0][k] * s;
    }

    // 0x00125160: fore/aft offset; either one scales the magnitude by 1.2
    float m = mag;
    if (flags & 0x21) {
        float s = v->bbmax[2];
        for (int k = 0; k < 4; k++) P[k] += v->frame[2][k] * s;
        m = 1.2f * mag;
    } else if (flags & 0x42) {
        float s = v->bbmin[2];
        for (int k = 0; k < 4; k++) P[k] += v->frame[2][k] * s;
        m = 1.2f * mag;
    }

    // 0x001251E9: an INVERTED car (up.y < 0) is kicked at the roof instead
    if (0.0f > v->frame[1][1]) {
        float s = v->bbmax[1] * 0.8f;
        for (int k = 0; k < 4; k++) P[k] += v->frame[1][k] * s;
    }

    // 0x00125235: J = axis * 10.0 * mass * m  (4-wide)
    float J[4];
    for (int k = 0; k < 4; k++) J[k] = axis[k] * 10.0f * v->mass * m;

    // 0x001252CB: (flags & 0x90) == 0 -> at-point, but the linear part is
    // saved at 0x001252D0 and written straight back at 0x001252EC, so only
    // the TORQUE survives.  Otherwise a pure linear impulse.
    if ((flags & 0x90) == 0) {
        float rx = P[0] - v->frame[3][0];
        float ry = P[1] - v->frame[3][1];
        float rz = P[2] - v->frame[3][2];
        v->ang_imp[0] += ry * J[2] - rz * J[1];
        v->ang_imp[1] += rz * J[0] - rx * J[2];
        v->ang_imp[2] += rx * J[1] - ry * J[0];
    } else {
        for (int k = 0; k < 4; k++) v->imp[k] += J[k];
    }
    v->asleep = 0;                 // 0x00125360: the kick wakes the body
}

// FUN_00125380 [C]
void b3_crash_kick_axis(B3CrashVehicle* v, unsigned flags, float mag) {
    int row;
    if (flags & 0x60)        row = 0;      // right
    else if (flags & 0x80)   row = 2;      // at
    else                     row = 1;      // up
    b3_crash_kick(v, flags, mag, v->frame[row]);
}

// ---------------------------------------------------------------------------
// FUN_00125CF0 [C-disasm] -- the settle predicate.  ESI = vehicle, one byte
// stack argument, `ret 4`.
//   0x00125CF0  veh+0x212 (a chassis contact this frame) and the chassis
//               contact surface byte veh+0x190 <= 0x20  -> 1
//   0x00125D0E  arg == 0 -> 0
//   0x00125D16  walk the wheels from veh+0x1169-1 down: a wheel whose
//               contact byte veh+0x8D3+i*0xC0 is set, whose damage state
//               ctx1+0x4AC+i is not 3, and whose surface byte
//               veh+0x8D0+i*0xC0 is <= 0x20 -> 1
//   0x00125D66  otherwise 0
// (The `JL` after the AND at 0x00125D02/0x00125D5B can never be taken -- the
// byte is zero-extended first -- so the test is exactly "<= 0x20".)
// ---------------------------------------------------------------------------
int b3_crash_settle_test(int chassis_contact, int chassis_surface,
                         int check_wheels, int wheel_count,
                         const unsigned char* wheel_contact,
                         const unsigned char* wheel_surface,
                         const unsigned char* wheel_damage) {
    if (chassis_contact && chassis_surface <= 0x20) return 1;   // 0x00125D0C
    if (!check_wheels) return 0;                                // 0x00125D14
    if (wheel_count == 0) return 0;                             // 0x00125D1F
    for (int i = wheel_count - 1; i >= 0; i--) {                // 0x00125D30
        if (wheel_contact && wheel_contact[i] == 0) continue;
        if (wheel_damage && wheel_damage[i] == 3) continue;     // 0x00125D47
        int surf = wheel_surface ? wheel_surface[i] : 0;
        if (surf <= 0x20) return 1;                             // 0x00125D60
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The sleep gate, FUN_00123000 @0x00123412..0x001235ED [C-disasm].  Reached
// only from the settle branch, after the counter has been incremented.
// ---------------------------------------------------------------------------
static int crash_sleep_gate(const B3RigidBody* rb, const B3CrashModeState* s) {
    int dl = 1;
    if (s->state212) {
        // 0x0012341A..0x00123516: dl = 1 unless EVERY body-axis component of
        // the recorded chassis contact axis (veh+0x170) is within 0.99
        // [0x003B1758] -- i.e. the shell is squarely on a face.
        if (fabsf(dot3(s->contact_normal, rb->frame[1]))
                <= B3_SLEEP_AXIS_LIMIT
            && fabsf(dot3(s->contact_normal, rb->frame[2]))
                <= B3_SLEEP_AXIS_LIMIT
            && fabsf(dot3(s->contact_normal, rb->frame[0]))
                <= B3_SLEEP_AXIS_LIMIT)
            dl = 0;
    } else {
        // 0x00123662..0x001236DB: with no chassis contact the same question
        // is asked of the WHEELS -- every wheel reporting a contact
        // (wheel+0xB3) must have its contact normal (wheel+0x20) within 0.98
        // [0x003B1DA0] of the body UP row.  No wheels in contact -> passes.
        for (int i = 0; i < s->wheel_count; i++) {
            if (s->wheel_contact && !s->wheel_contact[i]) continue;
            float d = s->wheel_normal ? dot3(s->wheel_normal[i], rb->frame[1])
                                      : 1.0f;
            if (B3_SLEEP_WHEEL_AXIS_LIMIT > d) { dl = 0; break; }
        }
    }
    // 0x00123518: speed < 0.5 [0x003B1684]
    if (B3_SLEEP_SPEED_LIMIT <= rb->vel[3]) return 0;
    // 0x00123529: |omega|^2 < DAT_005A80B8 (= 0.25)
    if (B3_SLEEP_OMEGA2_LIMIT <= dot3(rb->omega, rb->omega)) return 0;
    // 0x00123564: the global clock must have passed the kick's stamp
    if (!s->clock_after_stamp) return 0;
    // 0x0012357F: the contact axis test, or every wheel grounded
    if (!dl && !s->all_grounded) return 0;
    // 0x0012358F: settle counter > 5 [byte 0x003EBF88]
    if (s->settle <= B3_SLEEP_SETTLE_COUNT) return 0;
    return 1;   // 0x001235ED: veh+0x20E := 1, and FUN_00123FD0 is skipped
}

// ---------------------------------------------------------------------------
// FUN_00123FD0 @0x001248EA..0x001249E9 [C-disasm] -- the crashed-path
// SCRAPE.  See the header for the equation and its constants.
// ---------------------------------------------------------------------------
void b3_crash_scrape(B3RigidBody* rb, const float pt[4], float mass,
                     int n_points, int full_strength) {
    if (!(rb->vel[3] > 0.1f)) return;                 // 0x0012490D [0x3A69C4]
    // FUN_001066A0: the velocity of the contact point
    float r[3], v[4];
    for (int i = 0; i < 3; i++) r[i] = pt[i] - rb->frame[3][i];
    v[0] = (rb->omega[1] * r[2] - rb->omega[2] * r[1]) + rb->vel[0];
    v[1] = (rb->omega[2] * r[0] - rb->omega[0] * r[2]) + rb->vel[1];
    v[2] = (rb->omega[0] * r[1] - rb->omega[1] * r[0]) + rb->vel[2];
    v[3] = rb->vel[3];
    normalize4(v);                                    // FUN_00011640
    float d = dot3(rb->frame[0], v);                  // FUN_00013C60, right row
    float f = (fabsf(d) + 1.0f) * mass * (float)n_points * -0.75f;
    if (!full_strength) f *= 0.5f;                    // 0x001249BF
    float F[4];
    for (int i = 0; i < 4; i++) F[i] = v[i] * f;
    // FUN_001064B0: force at point
    for (int i = 0; i < 4; i++) rb->force_acc[i] += F[i];
    rb->torque_acc[0] += r[1] * F[2] - r[2] * F[1];
    rb->torque_acc[1] += r[2] * F[0] - r[0] * F[2];
    rb->torque_acc[2] += r[0] * F[1] - r[1] * F[0];
}

// ---------------------------------------------------------------------------
// FUN_00123000's body maths, in its exact order [C].
// ---------------------------------------------------------------------------
void b3_crash_mode_frame(B3RigidBody* rb, B3CrashModeState* s,
                         float mass, float com_height, int substeps,
                         float dt) {
    // 0x001230A6: quadratic drag, once per frame, BEFORE the substep loop:
    //   force_acc += dir * (speed * speed * -1.0)      [0x003B16C0 = -1.0]
    {
        float k = (rb->vel[3] * rb->vel[3]) * -1.0f;
        for (int i = 0; i < 4; i++) rb->force_acc[i] += rb->dir[i] * k;
    }
    // 0x001230FA: the speed all three gates below test is captured ONCE here
    const float frame_speed = rb->vel[3];
    s->slept = 0;                     // OUT: set by the sleep gate below
    if (substeps < 1) substeps = 1;
    const float sdt = (substeps > 1) ? dt * 0.5f : dt;

    for (int n = 0; n < substeps; n++) {
        // 0x00123292: the rollover crawl needs veh+0x211 == 0 AND every
        // wheel grounded AND speed < 1.0
        if (!s->state211 && s->all_grounded && 1.0f > frame_speed) {
            rb->angmom[1] *= 0.95f;                    // [0x003A69B8]
            rb->vel[0] *= 0.95f; rb->vel[1] *= 0.95f;
            rb->vel[2] *= 0.95f; rb->vel[3] *= 0.95f;
        } else if (s->all_airborne) {
            // 0x001235FF: the airborne law
            rb->angmom[0] *= 0.99f;                    // [0x003B1758]
            rb->angmom[1] *= 0.99f;
            rb->angmom[2] *= 0.97f;                    // [0x003B1A2C]
        }
        // 0x00123346: settle, else wake + reset the settle counter
        if (1.0f > frame_speed && (s->settle_test || s->asleep)) {
            for (int i = 0; i < 4; i++) rb->angmom[i] *= 0.9f;  // [0x3A69C0]
            rb->vel[0] *= 0.95f; rb->vel[1] *= 0.95f;
            rb->vel[2] *= 0.95f; rb->vel[3] *= 0.95f;
            s->settle++;                       // 0x001233FC: a BYTE, it wraps
            // 0x00123412..0x001235ED: the sleep gate, on this branch only.
            if (crash_sleep_gate(rb, s)) {
                s->asleep = 1;                 // 0x001235ED: veh+0x20E := 1
                s->slept = 1;                  // caller skips FUN_00123FD0
            }
        } else if (1.0f <= frame_speed) {
            s->asleep = 0;                             // 0x001236E0
            s->settle = 0;
        }
        // 0x001236FA/0x00123701: the suspension force pass (caller's) then
        // the integrator -- in CRASH MODE, so gravity gets its torque arm.
        b3_rigid_body_integrate(rb, mass, com_height, /*crash_mode=*/1, 0,
                                sdt);
        // FUN_00109560 @0x00109592 [C-disasm]: the integrator itself CLEARS
        // the sleep byte whenever veh+0x211 is non-zero -- so a body in that
        // state can never stay asleep, however quiet it is.  (The shared
        // b3_rigid_body_integrate does not model this; it lives in
        // src/burnout3_vehicle_sim.c, so the crash path mirrors it here and
        // it is recorded as a hand-off item in RE_NOTES 16.2.)
        if (s->state211) {
            s->asleep = 0;
            s->slept = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// FUN_00123000 crash-mode damping [S-disasm]: constants lifted from the
// dispatcher's damping blocks (0x3F733333 = 0.95, 0x3F666666 = 0.9;
// airborne block at 0x001235FF).
// ---------------------------------------------------------------------------
void b3_crash_mode_damping(float angmom[4], float vel[4], int all_airborne,
                           int settling) {
    if (all_airborne) {
        angmom[0] *= 0.99f;
        angmom[1] *= 0.99f;
        angmom[2] *= 0.97f;
    }
    if (settling) {
        for (int k = 0; k < 4; k++) angmom[k] *= 0.9f;
        for (int k = 0; k < 4; k++) vel[k] *= 0.95f;
    }
}

// ===========================================================================
// Harness wreck simulation (GLUE assembly; every equation inside is ported).
// The real machine has no separate crash integrator: FUN_00123000 keeps
// running the suspension + FUN_00109560 every step with crash damping, and
// FUN_0011AEF0 keeps resolving chassis contacts. This mirrors that shape
// with the harness's flat-ground world standing in for the poly soup.
// ===========================================================================

static void mat_identity(float m[4][4]) {
    memset(m, 0, 64);
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

// ---------------------------------------------------------------------------
// HARNESS <-> GAME space (RE_NOTES 12: the harness world is the game world
// with Z negated).  The pipeline's algebra is chirality-bound -- a mirrored
// basis has det -1 and b3_mat_orthonormalize, which rebuilds rows from cross
// products (FUN_000FF270), NEGATES a row on the first step: that is what
// flipped the wreck upside down on frame 1.  Every ported law therefore runs
// on the GAME-space copy.  M = diag(1,1,-1):
//   points / vectors        (x, y, z) -> (x, y, -z)
//   pseudovectors (L, w)    (x, y, z) -> (-x, -y, z)   [ det(M) * M v ]
// Both are exact sign flips, so mirroring in and back out is bit-exact.
// ---------------------------------------------------------------------------
static void mirror_vec(float v[4])    { v[2] = -v[2]; }
static void mirror_pseudo(float v[4]) { v[0] = -v[0]; v[1] = -v[1]; }
static void mirror_frame(float m[4][4]) {
    for (int r = 0; r < 4; r++) m[r][2] = -m[r][2];
}

// world inverse inertia = R^T * diag(iinv_body) * R  (FUN_00109040 pair)
static void wreck_world_inertia(const B3WreckState* w, float out[3][4]) {
    // rows of R are the frame rows (right/up/at)
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            float s = 0.0f;
            for (int k = 0; k < 3; k++)
                s += w->frame[k][i] * w->iinv_body[k][k] * w->frame[k][j];
            out[i][j] = s;
        }
        out[i][3] = 0.0f;
    }
}

// world->body inverse (FUN_00040AE0: transpose + pos = -pos*R)
static void wreck_inv_frame(const float f[4][4], float inv[4][4]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            inv[i][j] = f[j][i];
    inv[0][3] = inv[1][3] = inv[2][3] = 0.0f;
    for (int j = 0; j < 3; j++)
        inv[3][j] = -(f[3][0] * f[0][j] + f[3][1] * f[1][j]
                      + f[3][2] * f[2][j]);
    inv[3][3] = 1.0f;
}

// Build the B3CrashVehicle view the ported primitives operate on.
static void wreck_as_crash_vehicle(const B3WreckState* w, B3CrashVehicle* cv) {
    memset(cv, 0, sizeof(*cv));
    memcpy(cv->frame, w->frame, sizeof(cv->frame));
    wreck_inv_frame((const float(*)[4])w->frame, cv->inv);
    wreck_world_inertia(w, cv->iinv_world);
    memcpy(cv->vel, w->vel, sizeof(cv->vel));
    memcpy(cv->omega, w->omega, sizeof(cv->omega));
    memcpy(cv->bbmax, w->bbmax, sizeof(cv->bbmax));
    memcpy(cv->bbmin, w->bbmin, sizeof(cv->bbmin));
    cv->mass = w->mass;
    cv->asleep = (unsigned char)(w->asleep != 0);
}

void b3_wreck_begin_kick(B3WreckState* w, const float pos[3], float heading,
                         const float vel[3], float mass,
                         const float bbmin[3], const float bbmax[3],
                         const float contact_pt[3], const float contact_n[3],
                         const float rel_vel[3],
                         unsigned corner, float mag_spin, float mag_launch) {
    memset(w, 0, sizeof(*w));
    w->active = 1;
    w->mass = mass > 1.0f ? mass : 1500.0f;

    // frame from the harness heading convention fwd = (sin h, -cos h)
    mat_identity(w->frame);
    float sh = sinf(heading), ch = cosf(heading);
    w->frame[0][0] = ch;  w->frame[0][2] = sh;    // right
    w->frame[1][1] = 1.0f;                        // up
    w->frame[2][0] = sh;  w->frame[2][2] = -ch;   // at (forward)
    w->frame[3][0] = pos[0]; w->frame[3][1] = pos[1]; w->frame[3][2] = pos[2];
    w->frame[3][3] = 1.0f;

    for (int k = 0; k < 3; k++) {
        w->bbmin[k] = bbmin[k];
        w->bbmax[k] = bbmax[k];
        w->vel[k] = vel[k];
    }
    w->vel[3] = sqrtf(vel[0]*vel[0] + vel[1]*vel[1] + vel[2]*vel[2]);

    // Body inverse inertia: the game's OWN compiled-in class diagonal
    // (FUN_001203A0 + .data [C], RE_NOTES section 14) -- a flat per-class
    // value, NOT derived from the box.  The old box formula gave the wreck
    // an inverse roll inertia ~2x the retail one, so the crash-entry corner
    // torque spun it about twice as fast as the real machine does.
    // b3_wreck_set_inertia() installs the HEVY / HEVYCAR5-6 variants.
    memset(w->iinv_body, 0, sizeof(w->iinv_body));
    w->iinv_body[0][0] = B3_WRECK_IINV_DEFAULT_X;
    w->iinv_body[1][1] = B3_WRECK_IINV_DEFAULT_Y;
    w->iinv_body[2][2] = B3_WRECK_IINV_DEFAULT_Z;

    // Apply the REAL impact response at the contact: build a crash-vehicle
    // view and run the ported impulse solver at the contact point, exactly
    // as FUN_0011AEF0's airborne branch does (b3_crash_apply_impulse ->
    // FUN_00106500 linear + angular). This is what turns the hit into
    // spin/tumble instead of a parked car.
    // com height, the gravity-torque arm the crash-mode integrator uses
    // (FUN_0011A8F0: veh+0x1F4 = (ext.y - center.y) * 0.1)
    w->com_height = (w->bbmax[1] - w->bbmin[1]) * 0.1f;

    // Every impulse below is chirality-bound, so it runs on the GAME-space
    // mirror of the pose (see the note on B3WreckState); the resulting
    // impulse pair is mirrored back before it is folded into the wreck.
    B3WreckState gw = *w;
    mirror_frame(gw.frame);
    mirror_vec(gw.vel);

    B3CrashVehicle cv;
    wreck_as_crash_vehicle(&gw, &cv);
    // relative velocity against the obstacle drives the impulse
    float rv[4] = {rel_vel[0], rel_vel[1], -rel_vel[2], 0.0f};
    float n4[4] = {contact_n[0], contact_n[1], -contact_n[2], 0.0f};
    float pt4[4] = {contact_pt[0], contact_pt[1], -contact_pt[2], 1.0f};
    // point velocity = rel_vel + omega x r  (omega starts 0 at wreck time)
    float iv[4];
    // MAGNITUDE here, deliberately.  This is not a retail call site: the
    // caller hands in an already-classified crash-entry contact whose
    // `rel_vel` sign convention is the harness's, so the retail
    // `dv <= 0 -> skip` gate (which reads a POINT velocity) does not apply.
    float j = fabsf(b3_crash_impulse(&cv, n4, pt4, rv, 0.0f, iv));
    float dv = j / cv.mass;
    // impact magnitude, the verified wall formula (speed cap 89.408,
    // head-on term via |dot(dir, n)|)
    float sp = w->vel[3]; if (sp > 89.408f) sp = 89.408f;
    float dir4[4] = {0, 0, 0, 0};
    if (w->vel[3] > 1e-4f)
        for (int k = 0; k < 3; k++) dir4[k] = gw.vel[k] / w->vel[3];
    float ddn = fabsf(dot3(dir4, n4));
    float hc = ddn * 1.75f; if (hc > 0.9f) hc = 0.9f;
    float impact = ((1.0f - sp * 0.011184681f) * 0.9f + (1.0f - hc))
                   * cv.mass * dv * 0.175f;
    float impv[4];
    for (int k = 0; k < 4; k++) impv[k] = n4[k] * impact;
    cv.ground_frac = 0.0f;          // wrecked: the torque path
    b3_crash_apply_impulse(&cv, impv, pt4);

    // ---- THE CRASH ENTRY (FUN_0010DCA0 -> FUN_0010DD20) ------------------
    // Every crash-entry site in the retail image fires this pair on the way
    // into crash mode, and it is what a wreck's flight is made of:
    //   corner torque  -> the tumble (multi-axis rotation)
    //   linear up kick -> the LAUNCH (dv = 10 * mag along the car's up row)
    // FUN_0011AEF0's own wall impulse, applied above, is horizontal by
    // construction, so without these the wreck can only ever slide.
    if (mag_spin > 0.0f && corner)
        b3_crash_kick(&cv, corner, mag_spin, cv.frame[1]);
    if (mag_launch > 0.0f)
        b3_crash_kick(&cv, B3_KICK_LINEAR, mag_launch, cv.frame[1]);

    // fold the impulses into the wreck state (what FUN_00109560 would do
    // on its next step: vel += imp/mass, L += imp_torque) -- mirrored back
    // into harness space on the way in (imp is a vector, the angular impulse
    // a pseudovector)
    mirror_vec(cv.imp);
    mirror_pseudo(cv.ang_imp);
    for (int k = 0; k < 3; k++) {
        w->vel[k] += cv.imp[k] / cv.mass;
        w->angmom[k] += cv.ang_imp[k];
    }
    w->vel[3] = sqrtf(w->vel[0]*w->vel[0] + w->vel[1]*w->vel[1]
                      + w->vel[2]*w->vel[2]);
    w->asleep = cv.asleep;
    w->airborne = 1;                // a launched wreck starts in the air
    w->yaw = heading;
    w->after_credit = B3_AFTERTOUCH_PERIOD;   // the first frame may kick
    w->after_real_credit = B3_AFTERTOUCH_PERIOD;  // ... and may steer

    // PANELS: the entry hit is NOT published here.  Retail damages the panels
    // from the COLLISION RESOLVER (FUN_00111CD0 -> FUN_0012FA40) BEFORE the
    // crash entry's own five-event burst (FUN_00115130 -> FUN_00127180), and
    // every one of those events re-snapshots the accumulators -- so a report
    // consumed on the first crashed frame would land AFTER the burst and wipe
    // the rise the rip test measures.  The harness therefore applies the
    // entry damage directly, in retail's order, from wreck_begin_for; this
    // channel carries what happens AFTER the entry.
    w->hit_count = 0;
    w->hit_impulse = 0.0f;
    w->hit_collision = 0;
    (void)j;
}

void b3_wreck_set_inertia(B3WreckState* w, const float diag[3]) {
    memset(w->iinv_body, 0, sizeof(w->iinv_body));
    for (int i = 0; i < 3; i++) w->iinv_body[i][i] = diag[i];
}

// The per-slot crash latch (crash_record+0x130).  See the table in the
// header for the retail provenance; this is a straight transcription of the
// LAB_0010e431 branch structure.
float b3_crash_latch_duration(int is_truck, int presented) {
    if (is_truck)
        return presented ? B3_CRASH_LATCH_TRUCK_PRESENT
                         : B3_CRASH_LATCH_TRUCK_FRESH;
    /* class != 2.  The presented arm splits on FUN_00017390(0x4A71A0) --
     * a runtime-only object-state query that the harness does not model
     * ([?]).  Use the 15.0 (the !17390 value) for both [S]. */
    return presented ? B3_CRASH_LATCH_CAR_PRESENT
                     : B3_CRASH_LATCH_CAR_FRESH;
}

void b3_wreck_begin(B3WreckState* w, const float pos[3], float heading,
                    const float vel[3], float mass,
                    const float bbmin[3], const float bbmax[3],
                    const float contact_pt[3], const float contact_n[3],
                    const float rel_vel[3]) {
    // GLUE: pick the corner the retail flags name from where the hit landed.
    // The real entries carry a fixed corner per crash kind (0x0A rear-left at
    // 0x00024F94, 0x08 left at 0x0011C439, ...); deriving it from the contact
    // keeps the tumble axis pointing away from the impact.
    unsigned corner = 0;
    float sh = sinf(heading), ch = cosf(heading);
    float dx = contact_pt[0] - pos[0], dz = contact_pt[2] - pos[2];
    float lx = dx * ch + dz * sh;            // body right axis
    float lz = dx * sh - dz * ch;            // body at axis
    corner |= (lx < 0.0f) ? B3_KICK_LEFT : B3_KICK_RIGHT;
    corner |= (lz < 0.0f) ? B3_KICK_REAR : B3_KICK_FRONT;
    // A car wrecked mid-race is the FUN_00024F10 crash-record case, whose
    // literal corner magnitude is 0.40 (0x00024F94); 0.90 belongs to the
    // FUN_0011BE50 rollover entry, which is a much harder flick.
    b3_wreck_begin_kick(w, pos, heading, vel, mass, bbmin, bbmax,
                        contact_pt, contact_n, rel_vel,
                        corner, B3_KICK_MAG_TAKEDOWN, B3_KICK_MAG_LAUNCH);
}

// THE PER-KIND CRASH-ENTRY (see B3WreckEntryKind in burnout3_crash.h).
// Maps a crash kind to the retail entry kick's corner / spin / launch, then
// runs the same seeding + contact-impulse path as b3_wreck_begin_kick.
//   WALL     -> corner 0, spin 0, launch 0 (null entry kick; the FUN_0011AEF0
//              contact impulse above is the whole wall-crash entry).
//   CAR      -> corner 0x0A (rear-left, the crash-record literal), spin 0.40,
//              launch 0 (torque-only, flags 0x0A carries no 0x10).
//   ROLLOVER -> corner 0x08 (left), spin 0.90, launch 0.65 (the FUN_0011BE50
//              tail's two kicks, 0x0011C439 + 0x0011C421).
void b3_wreck_begin_entry(B3WreckState* w, B3WreckEntryKind kind,
                          const float pos[3], float heading,
                          const float vel[3], float mass,
                          const float bbmin[3], const float bbmax[3],
                          const float contact_pt[3], const float contact_n[3],
                          const float rel_vel[3]) {
    unsigned corner = 0;
    float spin = 0.0f, launch = 0.0f;
    switch (kind) {
    case B3_WRECK_ENTRY_CAR:
        corner = B3_KICK_REAR | B3_KICK_LEFT;   /* 0x0A, retail fixed corner */
        spin   = B3_KICK_MAG_TAKEDOWN;          /* 0.40 */
        launch = 0.0f;                          /* torque-only */
        break;
    case B3_WRECK_ENTRY_ROLLOVER:
        corner = B3_KICK_LEFT;                  /* 0x08, 0x0011C439 */
        spin   = B3_KICK_MAG_SPIN;              /* 0.90 */
        launch = B3_KICK_MAG_LAUNCH;            /* 0.65, 0x0011C421 */
        break;
    case B3_WRECK_ENTRY_WALL:
    default:
        corner = 0; spin = 0.0f; launch = 0.0f; /* null entry kick */
        break;
    }
    b3_wreck_begin_kick(w, pos, heading, vel, mass, bbmin, bbmax,
                        contact_pt, contact_n, rel_vel, corner, spin, launch);
}

// The crashed-input corner-kick block @0x0011817E..0x0011824E [C-disasm].
//
// CITATION CORRECTED (CRASH-PIN): this block is NOT inside FUN_00118410,
// which Ghidra places at 0x00118410..0x00118E05 -- it sits below it, in a
// stretch Ghidra has not defined as a function at all
// (/get_function_by_address?address=0x0011817E -> "No function found").
// The bytes are nonetheless real; every line below was re-read straight out
// of build/burnout3.elf (LOAD @0x00011000, file offset 0x2000).
//
//   0x0011817E  if (byte veh+0x4AC2 == 0) the whole block is skipped
//   0x0011818C  ONE kick per frame, an else-if chain over the pad direction
//               bits of byte [EBX] in this fixed priority:
//                   bit 0x04 -> flags 9   front-left
//                   bit 0x10 -> flags 5   front-right
//                   bit 0x01 -> flags 0xA rear-left
//                   bit 0x02 -> flags 6   rear-right
//               each: FUN_0010DCA0(director, veh, slot) -- a no-op for the
//               body, FUN_0010DD20 bails immediately on veh+0x210 != 0 --
//               then FUN_00125100(flags, 0.6, [veh+0x204]+0x10 = the up row)
//   0x0011824A  ret
//
// AND THE BLOCK IS DEAD CODE (CRASH-PIN).  An exhaustive scan of all 18
// LOAD segments of build/burnout3.elf finds exactly TWO references to the
// disp32 0x4ac2 in the whole image: the gate's read above, and
//   0x00117799  88 86 c2 4a 00 00   MOV byte ptr [ESI + 0x4ac2], AL
// with AL == 0 (XOR EAX,EAX @0x0011774c; the same zero is stored to
// +0x4AC4 on the next instruction) inside a vehicle reset routine.  Nothing
// in the shipped image ever sets veh+0x4AC2, so retail never applies
// aftertouch torque to a wrecked car.  This port stays (it is the recovered
// block, and it is what the harness would need if a writer is ever found),
// but the harness must not drive it by default -- see the
// crash-cinema wave RETIRED the B3_WRECK_AFTERTOUCH env gate: the harness
// now drives b3_wreck_aftertouch_steer (FUN_00118410) instead and this
// function is exercised only by tools/validate_crash_traj.py.  Driven every
// 1/60 game-
// second it adds ~11.7 rad/s per kick with nothing to absorb it once the
// shell is airborne, and takes |omega| past 65 rad/s (10 rev/s).
//
// There is no cooldown and no airborne/grounded gate: a held direction fires
// every frame the shaper runs.  The kick's own side effect is the wake at
// 0x00125360 (veh+0x20E := 0); it does NOT touch the settle counter
// veh+0x1354 -- only speed >= 1.0 resets that (0x001236E0) -- so a wreck
// being aftertouched at rest keeps counting up and can re-sleep the next
// frame.  (The old port cleared the counter here; that was invented, and it
// meant held input could keep a settled wreck awake forever.)
void b3_wreck_aftertouch(B3WreckState* w, float dir_x, float dir_z) {
    if (!w->active) return;
    if (dir_x == 0.0f && dir_z == 0.0f) return;
    // CADENCE (B3_AFTERTOUCH_PERIOD): retail fires one kick per vehicle
    // TICK; the harness calls this per RENDERED frame, so under the crash
    // slow-mo (sim dt / 5) it would fire five times per retail tick.  Spend
    // banked simulated time instead -- a leaky bucket, so at the normal
    // frame rate every frame still kicks.
    if (w->after_credit < B3_AFTERTOUCH_PERIOD) return;
    w->after_credit -= B3_AFTERTOUCH_PERIOD;
    unsigned corner;
    if (fabsf(dir_z) >= fabsf(dir_x))
        corner = (dir_z > 0.0f) ? (B3_KICK_FRONT | B3_KICK_LEFT)   /* 9 */
                                : (B3_KICK_REAR  | B3_KICK_LEFT);  /* 0xA */
    else
        corner = (dir_x > 0.0f) ? (B3_KICK_FRONT | B3_KICK_RIGHT)  /* 5 */
                                : (B3_KICK_REAR  | B3_KICK_RIGHT); /* 6 */
    B3WreckState gw = *w;              // the kick is chirality-bound
    mirror_frame(gw.frame);
    mirror_vec(gw.vel);
    B3CrashVehicle cv;
    wreck_as_crash_vehicle(&gw, &cv);
    b3_crash_kick(&cv, corner, B3_KICK_MAG_AFTERTOUCH, cv.frame[1]);
    mirror_vec(cv.imp);
    mirror_pseudo(cv.ang_imp);
    for (int k = 0; k < 3; k++) {
        w->vel[k] += cv.imp[k] / cv.mass;
        w->angmom[k] += cv.ang_imp[k];
    }
    w->vel[3] = sqrtf(w->vel[0]*w->vel[0] + w->vel[1]*w->vel[1]
                      + w->vel[2]*w->vel[2]);
    w->asleep = 0;                     // 0x00125360, the kick's own wake
}

// ---------------------------------------------------------------------------
// THE REAL AFTERTOUCH -- the port of FUN_00118410's consume block
// (0x001189A3..0x00118DFD).  See the long note in burnout3_crash.h for the
// full derivation; this is a straight transcription of it.
//
// COORDINATES: everything here is HARNESS space and the maths is
// chirality-free (a yaw about world +Y, a dot, a 2-D cross), so unlike the
// kick path there is no mirror round trip.  The one convention choice is the
// horizontal sign: retail writes `A * (-h)` where A is row0 of the camera
// basis it unpacks out of veh+0x1410.  The harness hands us the camera's
// SCREEN-RIGHT vector directly, and the arrow cursor proves the intended
// mapping (+h lights the RIGHT wedge, FUN_0004FCA0 @0x0004FD18), so we use
// `cam_right * (+h)`: same law, retail's own basis convention folded into the
// caller.  [C for the law, S for the sign convention.]
// ---------------------------------------------------------------------------
void b3_wreck_aftertouch_reset(B3WreckState* w) {
    if (!w) return;
    w->aftertouch_used = 0;            // 0x00119C87
    w->bank = 0.0f;
}

int b3_wreck_aftertouch_steer(B3WreckState* w, const B3WreckAftertouchIn* in,
                              float real_dt) {
    if (!w || !in || !w->active) return 0;

    // ---- the cadence -----------------------------------------------------
    // Retail runs this once per VEHICLE TICK and the tick rate is unaffected
    // by the Impact Time divisor.  Bank REAL time and spend it at 1/60, so
    // the slow-mo really does buy five yaw steps per simulated second.
    if (real_dt > 0.0f) {
        w->after_real_credit += real_dt;
        if (w->after_real_credit > 2.0f * B3_AFTERTOUCH_PERIOD)
            w->after_real_credit = 2.0f * B3_AFTERTOUCH_PERIOD;
    }
    if (w->after_real_credit < B3_AFTERTOUCH_PERIOD) return 0;
    w->after_real_credit -= B3_AFTERTOUCH_PERIOD;

    // ---- the gates (0x001189A3..0x00118A14) ------------------------------
    // engaged || veh+0x4AC3 -- the harness has no +0x4AC3 (the AI/attract
    // arm), so `engaged` alone stands in.
    if (!in->engaged) return 0;
    // (veh+0x1530 < 5.0 || crashbreaker armed)
    if (!(in->crash_clock < B3_AT_WINDOW_S) && !in->breaker_armed) return 0;
    // veh+0xBC > 1.0 -- the velocity 4-vector's speed lane
    if (!(w->vel[3] > B3_AT_MIN_SPEED)) return 0;

    float h = in->h, v = in->v;
    if (h >  1.0f) h =  1.0f;           // the producer's own clamp
    if (h < -1.0f) h = -1.0f;
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    if (fabsf(h) + fabsf(v) <= B3_AT_DEADZONE) return 0;

    // ---- the screen-relative direction (0x00118A1A..0x00118ACE) ----------
    float A[3] = { in->cam_right[0], 0.0f, in->cam_right[2] };
    float B[3] = { in->cam_fwd[0],   0.0f, in->cam_fwd[2]   };
    float la = sqrtf(A[0]*A[0] + A[2]*A[2]);
    float lb = sqrtf(B[0]*B[0] + B[2]*B[2]);
    if (la < 1e-6f || lb < 1e-6f) return 0;
    A[0] /= la; A[2] /= la;
    B[0] /= lb; B[2] /= lb;

    float dir[3];
    dir[0] = A[0] * h + B[0] * v;
    dir[1] = 0.0f;
    dir[2] = A[2] * h + B[2] * v;
    float ld = sqrtf(dir[0]*dir[0] + dir[2]*dir[2]);
    if (ld < 1e-6f) return 0;
    dir[0] /= ld; dir[2] /= ld;

    // ---- the crashbreaker nudge (0x00118ACE..0x00118B5E) -----------------
    if (in->breaker_armed) {
        float k = in->crash_mode ? B3_AT_NUDGE_CRASH : B3_AT_NUDGE_RACE;
        w->vel[0] += dir[0] * k;
        w->vel[2] += dir[2] * k;
        w->vel[3] = sqrtf(w->vel[0]*w->vel[0] + w->vel[1]*w->vel[1]
                          + w->vel[2]*w->vel[2]);
        if (!(w->vel[3] > B3_AT_MIN_SPEED)) return 0;
    }

    // ---- the signed yaw error (0x00118B5E..0x00118BE9) -------------------
    // veh+0xC0 is the unit velocity direction; c = cross((0,1,0), dir).
    float sp = w->vel[3];
    float vd[3] = { w->vel[0] / sp, w->vel[1] / sp, w->vel[2] / sp };
    float c[3]  = { dir[2], 0.0f, -dir[0] };          // (0,1,0) x dir
    float d0 = vd[0]*c[0]   + vd[1]*c[1]   + vd[2]*c[2];
    float d1 = vd[0]*dir[0] + vd[1]*dir[1] + vd[2]*dir[2];
    if (d1 >  1.0f) d1 =  1.0f;
    if (d1 < -1.0f) d1 = -1.0f;
    float ang = (float)(acos((double)d1) * (180.0 / 3.14159265358979323846));
    if (d0 > 0.0f) ang = -ang;                        // 0x00118BBC

    int rotated = 0;
    if (fabsf(ang) > B3_AT_ANGLE_GATE_DEG) {          // 0x00118BE2
        float lim = (in->crash_mode ? B3_AT_RATE_CRASH : B3_AT_RATE_RACE)
                    / (in->crash_clock + 1.0f);       // 0x00118C13
        if (ang < -lim) ang = -lim;
        if (ang >  lim) ang =  lim;
        // vel = vel * axis_angle((0,1,0), ang) -- a plain yaw
        float r = (float)(ang * (3.14159265358979323846 / 180.0));
        float s = sinf(r), cs = cosf(r);
        float vx = w->vel[0], vz = w->vel[2];
        w->vel[0] = vx * cs + vz * s;
        w->vel[2] = -vx * s + vz * cs;
        w->vel[3] = sqrtf(w->vel[0]*w->vel[0] + w->vel[1]*w->vel[1]
                          + w->vel[2]*w->vel[2]);
        w->aftertouch_used = 1;                       // 0x00118CD3, veh+0x4AC5
        w->asleep = 0;
        rotated = 1;
    }

    // ---- the visual bank (0x00118CDA..0x00118DF5) ------------------------
    if (in->want_bank) {
        float step = ang * B3_AT_BANK_SCALE;
        if (step >  B3_AT_BANK_STEP_MAX) step =  B3_AT_BANK_STEP_MAX;
        if (step < -B3_AT_BANK_STEP_MAX) step = -B3_AT_BANK_STEP_MAX;
        float nb = w->bank + step;
        if (fabsf(nb) <= B3_AT_BANK_MAX) w->bank = nb;
    }
    return rotated;
}

void b3_wreck_report_hit(B3WreckState* w, const float n[3],
                         float raw_impulse, int collision) {
    if (!w) return;
    float m = raw_impulse < 0.0f ? -raw_impulse : raw_impulse;
    if (m <= w->hit_impulse) {          // a weaker report does not displace
        w->hit_count++;                 // an unconsumed stronger one
        return;
    }
    w->hit_count++;
    w->hit_impulse = m;
    w->hit_collision = collision ? 1 : 0;
    w->hit_normal[0] = n[0];
    w->hit_normal[1] = n[1];
    w->hit_normal[2] = n[2];
    w->hit_normal[3] = 0.0f;
}

// ---------------------------------------------------------------------------
// PH-06 / PH-21 -- the wreck's world pass.  See the header for the chain.
// The whole body of this function is retail code: FUN_00107950's narrow phase
// and FUN_00109EA0's resolve, the two functions FUN_00122D00 calls, over the
// wreck's own box.  What is NOT retail is the single contact plane it is
// given (row PH-09).
// ---------------------------------------------------------------------------
static B3ObbPlaneFn     g_wreck_narrow_phase = NULL;   /* FUN_00107950 */
static B3WorldContactFn g_wreck_resolve       = NULL;   /* FUN_00109EA0 */

void b3_wreck_set_world_resolve(B3ObbPlaneFn narrow_phase,
                                B3WorldContactFn resolve) {
    g_wreck_narrow_phase = narrow_phase;
    g_wreck_resolve = resolve;
}

int b3_wreck_world_contact(B3WreckState* w, const float hit_pos[3],
                           const float hit_n[3]) {
    if (!w->active || !g_wreck_resolve) return 0;

    // Chirality: every ported law runs in GAME space (see the note on
    // B3WreckState).  Vectors z-negate; the plane normal is a true vector.
    B3WreckState gw = *w;
    mirror_frame(gw.frame);
    mirror_vec(gw.vel);
    mirror_pseudo(gw.angmom);
    mirror_pseudo(gw.omega);

    B3RigidBody rb;
    memset(&rb, 0, sizeof rb);
    memcpy(rb.frame, gw.frame, sizeof rb.frame);
    memcpy(rb.vel, gw.vel, sizeof rb.vel);
    memcpy(rb.angmom, gw.angmom, sizeof rb.angmom);
    memcpy(rb.omega, gw.omega, sizeof rb.omega);
    memcpy(rb.inv_inertia_body, w->iinv_body, sizeof rb.inv_inertia_body);
    wreck_world_inertia(&gw, rb.inv_inertia_world);
    wreck_inv_frame((const float(*)[4])gw.frame, rb.inv_frame);
    for (int i = 0; i < 3; i++)
        rb.dir[i] = (w->vel[3] > 1e-4f) ? gw.vel[i] / w->vel[3]
                                        : rb.frame[2][i];
    // the accumulators carry: a second contact in the same frame adds to the
    // first, exactly as +0x110/+0x120/+0x130 do between the manager's passes
    memcpy(rb.imp_force, w->pend_imp, sizeof rb.imp_force);
    memcpy(rb.imp_torque, w->pend_imp_torque, sizeof rb.imp_torque);
    memcpy(rb.deflection, w->pend_defl, sizeof rb.deflection);
    mirror_vec(rb.imp_force);
    mirror_pseudo(rb.imp_torque);
    mirror_vec(rb.deflection);

    const float ppt[3] = { hit_pos[0], hit_pos[1], -hit_pos[2] };
    const float pn[3]  = { hit_n[0],   hit_n[1],   -hit_n[2] };
    B3WorldContact ct;
    B3WorldContactResult res;
    int hit = g_wreck_narrow_phase
            ? g_wreck_narrow_phase(&rb, w->bbmin, w->bbmax, ppt, pn, &ct) : 0;
    // cls = veh+0x215 (1/2/3 are the racecar states: gate 1.0, damp 0.95, no
    // gravity add); restitution = the rigid-body ctor's +0x1F8
    // [0x003A69C4] = 0.1 @0x001094C5 -- FUN_00122830 does not override it.
    g_wreck_resolve(&rb, w->mass, w->state215 ? w->state215 : 1,
                    0, 0.1f, hit ? &ct : NULL, &res);

    // fold the accumulators back into the wreck's pending channel, mirrored
    // to harness space (imp is a vector, the angular impulse a pseudovector)
    mirror_vec(rb.imp_force);
    mirror_pseudo(rb.imp_torque);
    mirror_vec(rb.deflection);
    memcpy(w->pend_imp, rb.imp_force, sizeof w->pend_imp);
    memcpy(w->pend_imp_torque, rb.imp_torque, sizeof w->pend_imp_torque);
    memcpy(w->pend_defl, rb.deflection, sizeof w->pend_defl);
    // the resolve's velocity scrub / damp arm acts in place, not through an
    // accumulator: FUN_00109EA0 writes +0xB0 directly.
    mirror_vec(rb.vel);
    mirror_pseudo(rb.angmom);
    mirror_pseudo(rb.omega);
    memcpy(w->vel, rb.vel, sizeof w->vel);
    memcpy(w->angmom, rb.angmom, sizeof w->angmom);
    memcpy(w->omega, rb.omega, sizeof w->omega);
    if (res.sleep) w->asleep = 1;
    if (res.impulsed) {
        // PANELS: the same channel the suspension pass publishes on.
        float n3[3] = { hit_n[0], hit_n[1], hit_n[2] };
        b3_wreck_report_hit(w, n3, res.impact, /*collision=*/1);
    }
    return hit;
}

void b3_wreck_update(B3WreckState* w, float ground_y, float dt) {
    if (!w->active) return;
    w->rest_clock += dt;
    // bank simulated time for the aftertouch cadence, capped at one tick so
    // a long frame cannot buy a burst of kicks
    w->after_credit += dt;
    if (w->after_credit > 2.0f * B3_AFTERTOUCH_PERIOD)
        w->after_credit = 2.0f * B3_AFTERTOUCH_PERIOD;

    // ---- mirror into GAME space (see the note on B3WreckState) ----------
    // Everything from here down is game-space: the frame is right-handed, so
    // b3_mat_orthonormalize's cross-product repair keeps the body upright
    // instead of negating a row on the first step.
    B3WreckState gw = *w;
    mirror_frame(gw.frame);
    mirror_vec(gw.vel);
    mirror_pseudo(gw.angmom);
    mirror_pseudo(gw.omega);

    B3RigidBody rb;
    memset(&rb, 0, sizeof(rb));
    memcpy(rb.frame, gw.frame, sizeof(rb.frame));
    memcpy(rb.vel, gw.vel, sizeof(rb.vel));
    memcpy(rb.angmom, gw.angmom, sizeof(rb.angmom));
    memcpy(rb.omega, gw.omega, sizeof(rb.omega));
    memcpy(rb.inv_inertia_body, w->iinv_body, sizeof(rb.inv_inertia_body));
    wreck_world_inertia(&gw, rb.inv_inertia_world);
    wreck_inv_frame((const float(*)[4])gw.frame, rb.inv_frame);
    for (int i = 0; i < 3; i++) rb.dir[i] = (w->vel[3] > 1e-4f)
        ? gw.vel[i] / w->vel[3] : rb.frame[2][i];
    // PH-06: whatever the world pass (b3_wreck_world_contact, the wreck's
    // vtbl+0x10) accumulated into +0x110/+0x120/+0x130 this frame is consumed
    // and cleared HERE, by the integrator below -- FUN_00109560's own
    // ordering (@0x00109728..0x0010983B and @0x00109A27..0x00109A3E).  Zero
    // unless the harness ran the world pass, so a caller that does not is
    // bit-identical to before.
    memcpy(rb.imp_force, w->pend_imp, sizeof rb.imp_force);
    memcpy(rb.imp_torque, w->pend_imp_torque, sizeof rb.imp_torque);
    memcpy(rb.deflection, w->pend_defl, sizeof rb.deflection);
    mirror_vec(rb.imp_force);
    mirror_pseudo(rb.imp_torque);
    mirror_vec(rb.deflection);
    memset(w->pend_imp, 0, sizeof w->pend_imp);
    memset(w->pend_imp_torque, 0, sizeof w->pend_imp_torque);
    memset(w->pend_defl, 0, sizeof w->pend_defl);

    // ---- the airborne / grounded classification -------------------------
    // The retail test (FUN_00123000 @0x0012303E) is per WHEEL: veh+0x1168 is
    // 1 only when NO wheel reports a contact surface byte (+0x8D3 + i*0xC0),
    // and the "all grounded" local is 1 only when every non-detached wheel
    // does.  The harness has no wheel rays under a wreck, so the equivalent
    // GLUE test is the four wheel-station corners of the body box against
    // the ground height -- crucially NOT "is any bbox corner touching",
    // which the old code used and which classified a freshly launched wreck
    // as grounded on its first frame and killed the tumble immediately.
    const float CONTACT_EPS = 0.02f;
    int ground_pts = 0;
    for (int c = 0; c < 4; c++) {
        float bx = (c & 1) ? w->bbmax[0] : w->bbmin[0];
        float bz = (c & 2) ? w->bbmax[2] : w->bbmin[2];
        float p[4] = {bx, w->bbmin[1], bz, 1.0f}, wp[4];
        frame_point((const float(*)[4])rb.frame, p, wp);
        if (wp[1] <= ground_y + CONTACT_EPS) ground_pts++;
    }

    // ---- chassis contact at the lowest oriented-bbox corner ---------------
    // GLUE frame (a flat box against the ground height) around the ported
    // pieces: the normal impulse is FUN_00106720/FUN_00106500, the position
    // correction goes through the integrator's DEFLECTION channel -- the
    // same channel FUN_00123FD0 uses for its bottom-out cut -- and the
    // friction is the ported crashed-path scrape (FUN_00123FD0 @0x001248EA).
    // The scrape is what actually brings a wreck to rest: without it a
    // sliding shell keeps |v| >= 1 forever, and every settle/rollover gate
    // in FUN_00123000 is speed < 1.
    // Retail supports the body at FOUR points (the wheel stations), which is
    // what lets it find a flat equilibrium; resolving only the single lowest
    // corner cannot -- a corner impulse goes ~90% into spin (the angular term
    // of FUN_00106720's denominator dominates for r ~ 2 m at the class
    // inverse inertia), so the shell kept a residual sink velocity and a
    // permanent slow rotation, and |v| never dropped under the 1.0 every
    // settle gate tests.  So every bbox corner under the ground is a contact.
    B3CrashVehicle cv;
    memset(&cv, 0, sizeof(cv));
    memcpy(cv.frame, rb.frame, sizeof(cv.frame));
    memcpy(cv.iinv_world, rb.inv_inertia_world, sizeof(cv.iinv_world));
    memcpy(cv.vel, rb.vel, sizeof(cv.vel));
    memcpy(cv.omega, rb.omega, sizeof(cv.omega));
    cv.mass = w->mass;
    const float up[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    float low_y = 1e9f;
    int touching = 0, contacts = 0;
    float defl = 0.0f;
    float cpt[8][4];
    for (int c = 0; c < 8; c++) {
        float bx = (c & 1) ? w->bbmax[0] : w->bbmin[0];
        float by = (c & 2) ? w->bbmax[1] : w->bbmin[1];
        float bz = (c & 4) ? w->bbmax[2] : w->bbmin[2];
        float p[4] = {bx, by, bz, 1.0f}, wp[4];
        frame_point((const float(*)[4])rb.frame, p, wp);
        if (wp[1] < low_y) low_y = wp[1];
        // A corner counts as touching inside the same CONTACT_EPS band the
        // classification above uses.  Resolving only corners already BELOW
        // the surface leaves a one-frame gap every frame -- gravity pulls
        // the shell in, the push-out lifts it back clear, and the pair
        // limit-cycles at ~1 cm.  Catching the corner while it is still in
        // the band lets the contact impulse cancel the approach instead.
        if (wp[1] > ground_y + CONTACT_EPS) continue;
        memcpy(cpt[contacts], wp, sizeof(wp));
        contacts++;
        if (wp[1] < ground_y) defl += ground_y - wp[1];
        // FUN_00123FD0 @0x001248EA [C-disasm], the crashed-path scrape: the
        // retail loop runs it once per contact point with n_points =
        // veh+0x1169 (the wheel count) in the scale term.  Skipped while
        // asleep, exactly as 0x001236F0 skips the whole suspension pass.
        if (!w->asleep)
            b3_crash_scrape(&rb, wp, w->mass, 4,
                            w->state215 >= 1 && w->state215 <= 3);
    }
    // Projected Gauss-Seidel over the contact set: one impulse per point per
    // sweep, each sweep seeing the velocity the previous ones produced, until
    // no point is still moving into the ground.  A single sweep is not
    // enough -- FUN_00106720's denominator is dominated by its angular term
    // at r ~ 2 m and the class inverse inertia, so ~90% of a corner impulse
    // becomes spin, and a body resting on an edge kept ~0.8 m/s of sink and
    // a permanent rotation that never fell under the settle gate's speed
    // < 1.0.  (Retail does not need this: its support is FUN_00123FD0's
    // spring FORCE, in the same accumulator gravity lands in, so it finds
    // the equilibrium by itself.)
    for (int sweep = 0; sweep < 8; sweep++) {
        int hit = 0;
        for (int i = 0; i < contacts; i++) {
            float vp[4];
            b3_crash_point_velocity(&cv, cpt[i], vp);
            // The contact must answer the velocity this frame is ABOUT to
            // add as well: FUN_00109560 applies gravity (DAT_0040A8A0 =
            // (0,-20,0,0)) after this pass, so a shell that only cancels the
            // velocity it already has re-acquires 20*dt every frame and
            // sinks at a steady rate the position push-out has to fight.
            if (sweep == 0) vp[1] -= 20.0f * dt;
            if (vp[1] >= 0.0f) continue;
            float iv[4], before[3];
            for (int k = 0; k < 3; k++) before[k] = cv.ang_imp[k];
            b3_crash_impulse(&cv, up, cpt[i], vp, 0.0f, iv);
            b3_crash_apply_impulse_at_point(&cv, iv, cpt[i]);
            for (int k = 0; k < 3; k++) cv.vel[k] += iv[k] / cv.mass;
            for (int j = 0; j < 3; j++)
                cv.omega[j] += (cv.ang_imp[0] - before[0]) * cv.iinv_world[0][j]
                             + (cv.ang_imp[1] - before[1]) * cv.iinv_world[1][j]
                             + (cv.ang_imp[2] - before[2]) * cv.iinv_world[2][j];
            hit = 1;
        }
        if (!hit) break;
    }
    // PANELS: publish this frame's contact set before the vertical clamp
    // below rewrites cv.imp -- the panel machine wants the impulse the
    // contact actually produced, which is what retail's suspension pass
    // hands FUN_0012C670.  The contact normal is the ground's, and its
    // mirror back to harness space is itself.
    if (contacts > 0) {
        const float gnd_n[3] = {0.0f, 1.0f, 0.0f};
        b3_wreck_report_hit(w, gnd_n,
                            sqrtf(cv.imp[0]*cv.imp[0] + cv.imp[1]*cv.imp[1]
                                  + cv.imp[2]*cv.imp[2]),
                            /*collision=*/0);
    }

    if (contacts > 0) {
        touching = 1;
        if (ground_pts == 0) ground_pts = 1;   // the shell itself is down
        // ONE position correction, through the deflection channel the
        // integrator applies after the pose update (FUN_00109560 0x001096xx),
        // averaged over the contributing points exactly as FUN_00123FD0
        // averages its bottom-out cut.  The old code pushed out here AND
        // again after integration, and the second push also zeroed a
        // downward vel.y -- that pair is what let the harness's own
        // containment sweep drive a per-frame bobble.
        rb.deflection[1] += defl / (float)contacts;
        // ZERO RESTITUTION.  FUN_00106720 is called here with restitution 0,
        // so the contact may STOP a descent -- it must never launch the
        // shell.  Retail's support is FUN_00123FD0's spring FORCE, whose
        // vertical effect in one tick is bounded by F*dt/m; a corner IMPULSE
        // has no such bound, and it converts angular momentum into upward
        // com velocity.  With the aftertouch producer feeding the shell a
        // verified 0.6 corner kick (about 27 rad/s of |omega| each) that
        // conversion is what took a wreck to +105 m/s of vy -- the vertical
        // cap in FUN_00109560 -- in a couple of dozen frames.  Clamp the
        // contact's LINEAR share (the angular share is untouched, so the
        // shell still tumbles over its nose or its side).
        {
            const float vy_cap = rb.vel[1] > 0.0f ? rb.vel[1] : 0.0f;
            if (rb.vel[1] + cv.imp[1] / w->mass > vy_cap)
                cv.imp[1] = (vy_cap - rb.vel[1]) * w->mass;
        }
        for (int k = 0; k < 3; k++) {
            rb.imp_force[k] += cv.imp[k];
            rb.imp_torque[k] += cv.ang_imp[k];
        }
        // NO per-frame linear velocity scrub here.  The old code multiplied
        // rb.vel by 0.99 on every frame with a contact, borrowing the
        // constant from FUN_0011AEF0's WALL path (@0x0011B5E5).  That is a
        // per-CONTACT-EVENT scrub in retail, and the crashed-path solver
        // FUN_00123000 has no linear analogue while the shell is moving:
        // its only multiplicative velocity decays are the rollover crawl
        // (0x001232C3) and the settle branch (0x00123372), both gated on
        // frame speed < 1.0.  Applied per RENDERED frame it was also
        // dt-independent, so the divisor-5 crash presentation ran it 300
        // times per game-second instead of 60 -- 0.99^300 = 0.049, i.e. the
        // wreck lost 95% of its speed in the first game-second and stopped
        // dead under the camera.  Measured against the whole-flight oracle
        // (tools/emulate_crash_traj.py seq_wall35): retail carries |v| 35.34
        // -> 34.94 m/s and 33.9 m of travel across all 300 rendered frames;
        // with the scrub the port managed 1.6 m/s and 18 m, and never left
        // the ground.  Without it: 55.8 m/s and 57.8 m from a 61 m/s entry
        // (the same distance-per-entry-speed ratio as retail, 1.71 vs 1.74)
        // and 142/302 airborne frames.
    }
    w->airborne = (ground_pts == 0);
    w->air_time = w->airborne ? (w->air_time + dt) : 0.0f;

    // ---- the real crashed-path frame: FUN_00123000's drag + damping laws
    // + FUN_00109560 in CRASH MODE (gravity applied at up*com_height, so it
    // carries a torque -- the term that keeps a tumbling wreck rotating).
    B3CrashModeState cms;
    memset(&cms, 0, sizeof(cms));
    cms.all_grounded = (ground_pts >= 4);
    cms.all_airborne = w->airborne;
    cms.state211 = 0;
    // FUN_00125CF0 [C-disasm]: the chassis arm of the predicate -- a contact
    // this frame (veh+0x212) on a surface the game calls solid (veh+0x190
    // <= 0x20).  The harness has no wheel rays under a wreck, so the wheel
    // arm is not offered (check_wheels = 0) and the shell contact stands in.
    cms.settle_test = b3_crash_settle_test(touching, w->ground_surface,
                                           0, 0, NULL, NULL, NULL);
    cms.state212 = touching;                       // veh+0x212
    cms.contact_normal[1] = touching ? 1.0f : 0.0f; // veh+0x170: the ground
    // the sleep gate's clock test: the global clock must have passed the
    // stamp FUN_00125100 wrote at the crash entry (ctx1+0xFFC)
    cms.clock_after_stamp = (w->rest_clock > 0.0f);
    cms.asleep = (unsigned char)(w->asleep != 0);
    cms.settle = (unsigned char)w->settle;
    b3_crash_mode_frame(&rb, &cms, w->mass, w->com_height, 1, dt);
    w->settle = (float)cms.settle;
    w->asleep = cms.asleep;

    // ---- mirror back out to harness space -------------------------------
    memcpy(w->frame, rb.frame, sizeof(w->frame));
    memcpy(w->vel, rb.vel, sizeof(w->vel));
    memcpy(w->angmom, rb.angmom, sizeof(w->angmom));
    memcpy(w->omega, rb.omega, sizeof(w->omega));
    mirror_frame(w->frame);
    mirror_vec(w->vel);
    mirror_pseudo(w->angmom);
    mirror_pseudo(w->omega);

    // draw decomposition for the harness (yaw convention fwd=(sin h,-cos h))
    w->yaw = atan2f(w->frame[2][0], -w->frame[2][2]);
    float aty = w->frame[2][1];
    if (aty > 1.0f) aty = 1.0f;
    if (aty < -1.0f) aty = -1.0f;
    w->pitch = asinf(aty);
    w->roll = atan2f(w->frame[0][1], w->frame[1][1]);
}
