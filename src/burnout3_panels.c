// burnout3_panels.c -- the per-panel damage machine (see burnout3_panels.h
// for the full recovery write-up, addresses and provenance).

#include "burnout3_panels.h"

#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The game's own LCG pair, as it appears inline in FUN_0012FEE0, FUN_0012C670
// and FUN_00106F20 [C]:
//     s0 = s0 * 0x10000 + (s0 >> 16) + s1;      (arithmetic shift, signed)
//     s1 = s0 + s1;
//     r  = (float)(unsigned)s0 * DAT_0054F46C
// DAT_0054F46C is BSS (written by the CRT init) and is the 1/2^32 scale that
// makes r land in [0,1) -- every consumer in the image treats it that way
// (FUN_00106F20: `r - 0.5`, `(r + 1.0) * 15.0`; FUN_0012FEE0: `(r + 1.0) *
// 0.5 * band`).  [S for the constant, C for the recurrence.]
// ---------------------------------------------------------------------------
#define B3_PANEL_RAND_SCALE (1.0f / 4294967296.0f)

static float panel_rand(B3PanelSet* s) {
    int s0 = (int)s->rng[0];
    int s1 = (int)s->rng[1];
    int nx = s0 * 0x10000 + (s0 >> 16) + s1;
    s->rng[0] = (unsigned)nx;
    s->rng[1] = (unsigned)(nx + s1);
    return (float)s->rng[0] * B3_PANEL_RAND_SCALE;
}

// ---- harness <-> game space (the maps B3WreckState documents) --------------
static void mirror_vec(float v[4])    { v[2] = -v[2]; }
static void mirror_pseudo(float v[4]) { v[0] = -v[0]; v[1] = -v[1]; }
static void mirror_frame(float m[4][4]) {
    for (int r = 0; r < 4; r++) m[r][2] = -m[r][2];
}

static void panel_world_inertia(const float frame[4][4],
                                const float iinv_body[3][4], float out[3][4]) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            float s = 0.0f;
            for (int k = 0; k < 3; k++)
                s += frame[k][i] * iinv_body[k][k] * frame[k][j];
            out[i][j] = s;
        }
        out[i][3] = 0.0f;
    }
}

static void panel_inv_frame(const float f[4][4], float inv[4][4]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) inv[i][j] = f[j][i];
    inv[0][3] = inv[1][3] = inv[2][3] = 0.0f;
    for (int j = 0; j < 3; j++)
        inv[3][j] = -(f[3][0]*f[0][j] + f[3][1]*f[1][j] + f[3][2]*f[2][j]);
    inv[3][3] = 1.0f;
}

// ---------------------------------------------------------------------------
// FUN_0012FEE0's panel half [C @0x0012FF1D..0x0012FF9C]:
//   ctx+0xFA8 = (r + 1.0) * 0.5 * 0.1
//   ctx+0xFC0 = (r + 1.0) * 0.5 * 0.17
//   ctx+0xFD8 = (r * 0.3 + 0.7) * 0.2
// plus states and accumulators zeroed (@0x0013021E).
// ---------------------------------------------------------------------------
void b3_panels_reset(B3PanelSet* s, int n, const int* kinds,
                     const float (*attach)[3], unsigned seed) {
    memset(s, 0, sizeof(*s));
    if (n < 0) n = 0;
    if (n > B3_PANEL_MAX) n = B3_PANEL_MAX;
    s->n = n;
    // The LCG dies on the all-zero state, so a zero seed takes the game's
    // own static pair shape instead.
    s->rng[0] = seed ? seed : 0x2545F491u;
    s->rng[1] = seed ? (seed ^ 0x9E3779B9u) : 0x0139408Du;
    for (int k = 0; k < n; k++) {
        s->kind[k] = kinds ? kinds[k] : k;
        if (attach) {
            s->attach[k][0] = attach[k][0];
            s->attach[k][1] = attach[k][1];
            s->attach[k][2] = attach[k][2];
        }
        s->state[k] = B3_PANEL_PRISTINE;
        s->acc[k] = s->snap[k] = 0.0f;
        {
            float r = panel_rand(s);
            s->thr_loose[k] = (r + 1.0f) * 0.5f * B3_PANEL_LOOSE_BAND;
            r = panel_rand(s);
            s->thr_crumple[k] = (r + 1.0f) * 0.5f * B3_PANEL_CRUMPLE_BAND;
            r = panel_rand(s);
            s->thr_rip[k] = (r * B3_PANEL_RIP_SPAN + B3_PANEL_RIP_BASE)
                          * B3_PANEL_RIP_BAND;
        }
    }
}

// ---------------------------------------------------------------------------
// FUN_00127180's five-event crash-entry burst (see the header).  The
// directions are body space; the bbox-face lerp each event also carries only
// picks the deformation's origin and does not reach the panel accumulators.
// ---------------------------------------------------------------------------
void b3_panels_entry_burst(B3PanelSet* s) {
    static const float dirs[5][3] = {
        { 0.0f, -1.0f,  0.0f},   // 0x001271A0  roof
        { 0.0f,  0.0f, -1.0f},   // 0x001271E1  front
        { 0.0f,  0.0f,  1.0f},   // 0x00127233  rear
        {-1.0f,  0.0f,  0.0f},   // 0x0012728A  right
        { 1.0f,  0.0f,  0.0f},   // 0x001272E4  left
    };
    for (int e = 0; e < 5; e++)
        b3_panels_add_impact(s, dirs[e],
                             B3_PANEL_BURST_MAG * B3_PANEL_IMPACT_SCALE);
}

// ---------------------------------------------------------------------------
// FUN_001253C0.  Retail walks KINDS 0..6 and acts on the FIRST panel carrying
// each kind (the inner search over .bgv+0xAC4); with kinds unique per car
// that is every panel exactly once, which is what the loop below does.
// ---------------------------------------------------------------------------
void b3_panels_wreck_stamp(B3PanelSet* s, int detach_all) {
    for (int kind = 0; kind <= 6; kind++) {
        for (int k = 0; k < s->n; k++) {
            if (s->kind[k] != kind) continue;
            if (detach_all) {
                s->acc[k] = B3_PANEL_DETACH_ACC;      // 0x447A0000
            } else if (s->state[k] == B3_PANEL_PRISTINE
                       || s->state[k] == B3_PANEL_LOOSE) {
                s->state[k] = B3_PANEL_CRUMPLED;
            }
            break;                                    // first match only
        }
    }
}

// ---------------------------------------------------------------------------
// FUN_0012C670's panel loop [C].  The wheel loop is the vehicle module's
// business and is not mirrored here.
// ---------------------------------------------------------------------------
void b3_panels_add_impact(B3PanelSet* s, const float dir_body[3],
                          float impact) {
    if (!(impact > B3_PANEL_IMPACT_GATE)) return;     // DAT_003B1694 gate
    // Both arms into FUN_0012C670 RE-SNAPSHOT the accumulators immediately
    // before adding (FUN_0012FA40 @0x0012FCD0, FUN_00123FD0 @0x001246BB), so
    // the one-frame rip test only ever measures the LAST event's rise even
    // when several land in the same frame.
    for (int k = 0; k < s->n; k++) s->snap[k] = s->acc[k];
    float add = impact * B3_PANEL_HIT_SCALE;
    if (add >= B3_PANEL_HIT_CAP) add = B3_PANEL_HIT_CAP;
    for (int k = 0; k < s->n; k++) {
        if (s->state[k] >= B3_PANEL_DETACHED) continue;
        const float d = dir_body[0] * s->attach[k][0]
                      + dir_body[1] * s->attach[k][1]
                      + dir_body[2] * s->attach[k][2];
        if (d < 0.0f) s->acc[k] += add;
    }
}

// ---------------------------------------------------------------------------
// FUN_0012FA40's damage entry (from the collision resolver FUN_00111CD0
// @0x00111FDE and @0x00112106): scale the hit by the car class, then hand it
// to FUN_0012C670 if it clears 5.0.  The body-space contact axis is the world
// normal resolved onto the car's own rows -- retail builds obj+0x150 the same
// way at 0x0012FC93 (FUN_00031330, vector x frame).
// ---------------------------------------------------------------------------
void b3_panels_impact_world(B3PanelSet* s, const float frame[4][4],
                            const float n_world[3], float raw_impulse) {
    if (!s || s->n <= 0) return;
    float dir_body[3];
    for (int i = 0; i < 3; i++)
        dir_body[i] = n_world[0]*frame[i][0] + n_world[1]*frame[i][1]
                    + n_world[2]*frame[i][2];
    b3_panels_add_impact(s, dir_body,
                         fabsf(raw_impulse) * B3_PANEL_IMPACT_SCALE);
}

// ---------------------------------------------------------------------------
// FUN_0012C860's per-panel block, disassembled at 0x0012C940..0x0012CA00.
// ---------------------------------------------------------------------------
void b3_panels_visual_pass(B3PanelSet* s) {
    const float scale = s->party ? B3_PANEL_RIP_SCALE_TD : B3_PANEL_RIP_SCALE;
    for (int k = 0; k < s->n; k++) {
        if (s->state[k] >= B3_PANEL_DETACHED) continue;
        const float rise = s->acc[k] - s->snap[k];
        if (rise > s->thr_rip[k] * scale) {
            s->acc[k] = B3_PANEL_DETACH_ACC;          // 0x0012C96C
            continue;
        }
        if (s->acc[k] > s->thr_crumple[k]) {
            if (s->state[k] == B3_PANEL_PRISTINE || s->state[k] == B3_PANEL_LOOSE)
                s->acc[k] = s->thr_crumple[k] + 1.0f; // 0x0012C9AB
            continue;
        }
        if (s->acc[k] > s->thr_loose[k] && s->state[k] == B3_PANEL_PRISTINE) {
            // The bonnet alone gets a coin flip that sends it straight to the
            // crumple clamp instead of the loose pose (0x0012C9D9/0x0012C9E5).
            if (s->kind[k] == B3_PANEL_KIND_BONNET && panel_rand(s) < 0.5f) {
                s->acc[k] = s->thr_crumple[k] + 1.0f;
                continue;
            }
            s->state[k] = B3_PANEL_LOOSE;             // 0x0012C9F9
        }
    }
}

// ---------------------------------------------------------------------------
// FUN_00106F20's flying-part seeding, for one panel.
//   frame    = placement matrix x car frame.  Every shipped car's placement
//              rotation is EXACTLY identity (checked over all 67 .panels
//              sidecars), so the piece inherits the wreck's rows and only the
//              pivot translation applies.
//   velocity = the car's velocity AT THE PIVOT (FUN_001066A0: v + w x r).
//   angmom   = car angular momentum x (part mass / car mass), then x 2.5.
//
// PHYS-LEDGER wave 4 -- the SEEDING half is no longer GLUE.  The activation
// ctor FUN_001069C0 (the call FUN_00111340 makes the moment it takes a pool
// slot) seeds the mass, the OBB and the inertia; see the long block in
// burnout3_panels.h for the full derivation and every address.
//   mass       piece+0x1F0 = 0x43820000 = 260.0f, unconditional  [C]
//   OBB        piece+0x1D0 / +0x1E0 = .bgv+0xEA0 + k*0x20 (max, min),
//              recentred by (min+max)*0.5 with one axis zeroed per
//              .bgv+0xADC+k                                      [C]
//   inertia    FUN_00109BB0's diagonal, from the RAW box          [C]
//   the tumble term the old port invented is retail's LOOSE-panel hinge
//   state (panel record +0x90), which this harness never integrates, so it
//   is zero -- the invented 3 rad/s is deleted, not replaced.     [S]
// ---------------------------------------------------------------------------
#define B3_PIECE_MASS      260.0f     // 0x43820000, FUN_001069C0 tail [C]
#define B3_PIECE_INERTIA_K 0.5f       // [0x003B1684] @0x00109C58      [C]
#define B3_PIECE_INERTIA_N 1.0f       // [0x003B168C] @0x00109C60      [C]
#define B3_PANEL_SPIN_GAIN 2.5f       // [0x003A2D50] @0x001071D6      [C]
// Fallback ONLY: a .panels sidecar written before this wave carries no
// `panelbb` line, so there is no .bgv box to seed from.  The old invented
// cube stands in, and box_ok[] says which one a piece actually used.
#define B3_PANEL_HALF      0.55f      // GLUE fallback (no sidecar box)

// FUN_00109BB0 @0x00109BB0 -> FUN_00109190 @0x00109190.  The inverse inertia
// a flying part is seeded with, straight off its OBB and its mass.  Ghidra's
// decompile of FUN_00109BB0 shows only the FIRST axis (it drops the XMM
// tracking); the disassembly 0x00109BBF..0x00109CC8 has all three, and the
// two constants are [0x003B1684] = 0.5 and [0x003B168C] = 1.0.  Retail
// writes the three values to body+0x10 / +0x24 / +0x38, i.e. the diagonal of
// a 3x4 -- there is no off-diagonal term.                              [C]
void b3_piece_inertia(float mass, const float bbmax[3], const float bbmin[3],
                      float diag[3]) {
    const float a = fmaxf(bbmax[0], -bbmin[0]);       /* 0x00109BBF */
    const float b = fmaxf(bbmax[1], -bbmin[1]);       /* 0x00109BF0 */
    const float c = fmaxf(bbmax[2], -bbmin[2]);       /* 0x00109C1E */
    const float km = B3_PIECE_INERTIA_K * mass;
    const float d[3] = { b*b + c*c, a*a + c*c, a*a + b*b };
    for (int i = 0; i < 3; i++)
        diag[i] = (d[i] > 0.0f) ? B3_PIECE_INERTIA_N / (km * d[i]) : 0.0f;
}

// FUN_001069C0's mode-1 box centre (@0x001069C0, the (min+max)*0.5 with one
// component killed by the .bgv+0xADC hinge byte) followed by FUN_00106F20's
// subtraction @0x00107217.  Retail splits them across the two functions
// because the inertia above is built from the RAW box in between.      [C]
void b3_piece_recentre(const float bbmax[3], const float bbmin[3], int axis,
                       float outmax[3], float outmin[3]) {
    float ctr[3];
    for (int i = 0; i < 3; i++) ctr[i] = (bbmin[i] + bbmax[i]) * 0.5f;
    switch (axis) {
    case 0:  ctr[1] = 0.0f; break;
    case 1:  ctr[0] = 0.0f; break;
    case 2:  ctr[1] = 0.0f; ctr[2] = 0.0f; break;
    default: break;
    }
    for (int i = 0; i < 3; i++) {
        outmax[i] = bbmax[i] - ctr[i];
        outmin[i] = bbmin[i] - ctr[i];
    }
}

void b3_panels_set_box(B3PanelSet* s, int k, const float bbmax[3],
                       const float bbmin[3], int axis) {
    if (!s || k < 0 || k >= B3_PANEL_MAX || !bbmax || !bbmin) return;
    for (int i = 0; i < 3; i++) {
        s->box_max[k][i] = bbmax[i];
        s->box_min[k][i] = bbmin[i];
    }
    s->box_axis[k] = (unsigned char)axis;
    // A zero box is what the .bgv holds for k >= numBodyParts; refuse it so
    // the inertia divide cannot produce an infinity.
    s->box_ok[k] = (bbmax[0] > bbmin[0] || bbmax[1] > bbmin[1]
                    || bbmax[2] > bbmin[2]) ? 1 : 0;
}

static void panel_piece_spawn(B3PanelSet* s, int k, const B3WreckState* w) {
    B3PanelPiece* p = &s->piece[k];
    memset(p, 0, sizeof(*p));
    p->active = 1;
    p->panel = k;
    p->mass = B3_PIECE_MASS;                  // +0x1F0, FUN_001069C0    [C]

    // ---- the OBB, FUN_001069C0's mode-1 arm ----------------------------
    float bmax[3], bmin[3];
    if (s->box_ok[k]) {
        for (int i = 0; i < 3; i++) {
            bmax[i] = s->box_max[k][i];
            bmin[i] = s->box_min[k][i];
        }
    } else {
        for (int i = 0; i < 3; i++) {
            bmax[i] =  B3_PANEL_HALF;
            bmin[i] = -B3_PANEL_HALF;
        }
    }
    // ---- the inertia, FUN_00109BB0 -- from the RAW box, BEFORE the
    // recentring (FUN_001069C0 calls it in its own tail; the recentring is
    // FUN_00106F20 @0x00107217, which runs later).
    {
        float diag[3];
        b3_piece_inertia(p->mass, bmax, bmin, diag);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++)
                p->iinv_body[i][j] = (i == j) ? diag[i] : 0.0f;
    }
    // ---- the centre, then the recentring FUN_00106F20 @0x00107217 ------
    if (s->box_ok[k]) {
        float rmax[3], rmin[3];
        b3_piece_recentre(bmax, bmin, s->box_axis[k], rmax, rmin);
        for (int i = 0; i < 3; i++) { bmax[i] = rmax[i]; bmin[i] = rmin[i]; }
    }
    for (int i = 0; i < 3; i++) {
        p->bbmax[i] = bmax[i];
        p->bbmin[i] = bmin[i];
        p->half[i]  = fmaxf(bmax[i], -bmin[i]);
    }

    // rows = the wreck's rows (identity placement rotation)
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) p->frame[r][c] = w->frame[r][c];
    // pivot -> world: pos + ax*right + ay*up + az*at  (az is the GAME-space
    // z of the .bgv pivot; the wreck's `at` row already carries the harness
    // sign, so no flip is applied here -- see the note in the header).
    const float ax = s->attach[k][0], ay = s->attach[k][1], az = s->attach[k][2];
    float r_off[3];
    for (int c = 0; c < 3; c++) {
        r_off[c] = ax * w->frame[0][c] + ay * w->frame[1][c] + az * w->frame[2][c];
        p->frame[3][c] = w->frame[3][c] + r_off[c];
    }
    p->frame[3][3] = 1.0f;

    // v = v_car + omega x r   (FUN_001066A0)
    p->vel[0] = w->vel[0] + w->omega[1]*r_off[2] - w->omega[2]*r_off[1];
    p->vel[1] = w->vel[1] + w->omega[2]*r_off[0] - w->omega[0]*r_off[2];
    p->vel[2] = w->vel[2] + w->omega[0]*r_off[1] - w->omega[1]*r_off[0];
    p->vel[3] = sqrtf(p->vel[0]*p->vel[0] + p->vel[1]*p->vel[1]
                    + p->vel[2]*p->vel[2]);

    // L_piece = ( L_car * (m_p / m_c) + seed . R_car ) * 2.5
    //   the mass ratio is the DIVSS piece+0x1F0 / veh+0x1F0 @0x00107173 [C]
    //   the 2.5 is [0x003A2D50] @0x001071D6                            [C]
    //   `seed` is veh+0xD70 + k*0xC0 = panel record +0x90, the LOOSE-panel
    //   hinge integrator's accumulated state (FUN_00122270 @0x001222B1);
    //   the vehicle ctor zeroes it (FUN_00122830 @0x001229C8 REP STOSD /
    //   FUN_00121D70) and this harness never runs the hinge animation, so
    //   it is exactly 0 here -- retail's own value for a panel that never
    //   entered state 1.                                               [S]
    {
        const float mc = (w->mass > 1.0f) ? w->mass : 1500.0f;
        const float ratio = p->mass / mc;
        for (int i = 0; i < 3; i++)
            p->angmom[i] = w->angmom[i] * ratio * B3_PANEL_SPIN_GAIN;
    }
    // omega = L . Iinv_world
    {
        float iw[3][4];
        panel_world_inertia((const float(*)[4])p->frame,
                            (const float(*)[4])p->iinv_body, iw);
        for (int j = 0; j < 3; j++)
            p->omega[j] = p->angmom[0]*iw[0][j] + p->angmom[1]*iw[1][j]
                        + p->angmom[2]*iw[2][j];
    }
    // (The invented 3 rad/s pivot tumble that used to live here is gone --
    // see the L_piece note above: retail's extra term is the loose-panel
    // hinge state, and it is zero for a panel that never flapped.)
}

// ---------------------------------------------------------------------------
// FUN_00123000's panel scan (@0x001230F0..0x00123154) + FUN_00125A50.
// ---------------------------------------------------------------------------
int b3_panels_scan(B3PanelSet* s, const B3WreckState* w) {
    int released = 0;
    for (int k = 0; k < s->n; k++) {
        if (s->state[k] >= B3_PANEL_DETACHED) continue;
        if (s->acc[k] > s->thr_crumple[k]
            && (s->state[k] == B3_PANEL_PRISTINE
                || s->state[k] == B3_PANEL_LOOSE))
            s->state[k] = B3_PANEL_CRUMPLED;
        // FUN_00125A50 acts on state == 2 only, and the caller's gate is
        // acc > DAT_005A80C8 = 999.0
        if (s->acc[k] > B3_PANEL_DETACH_GATE
            && s->state[k] == B3_PANEL_CRUMPLED) {
            s->state[k] = B3_PANEL_DETACHED;
            if (w && w->active) panel_piece_spawn(s, k, w);
            s->detached_total++;
            released++;
        }
    }
    return released;
}

// ---------------------------------------------------------------------------
// FUN_00023DE0's health distributor (the RACING path).  Priority table
// DAT_00385224 = {3,6,0,1,5,4,2}; kinds {0,1,3,5,6} are dent-only.
// ---------------------------------------------------------------------------
static const int B3_PANEL_PRIORITY[7] = {3, 6, 0, 1, 5, 4, 2};

static int panel_kind_detaches(int kind) {
    switch (kind) {
    case 0: case 1: case 3: case 5: case 6: return 0;
    default: return 1;                     // 2, 4 and anything > 6
    }
}

void b3_panels_health(B3PanelSet* s, float health, const B3WreckState* w) {
    if (s->n <= 0) return;
    float frac = health + 0.1f;
    if (frac > 1.0f) frac = 1.0f;
    const int keep = (int)((float)s->n * frac);

    int detachable = 0;
    for (int k = 0; k < s->n; k++)
        if (s->state[k] != B3_PANEL_DETACHED && s->state[k] != B3_PANEL_GONE
            && panel_kind_detaches(s->kind[k]))
            detachable++;

    int todo = detachable - keep;
    while (todo > 0) {
        int acted = 0;
        for (int pi = 0; pi < 7 && !acted; pi++) {
            for (int k = 0; k < s->n; k++) {
                if (s->kind[k] != B3_PANEL_PRIORITY[pi]) continue;
                if (s->state[k] == B3_PANEL_PRISTINE)
                    s->state[k] = B3_PANEL_CRUMPLED;   // the dent arm
                if (s->state[k] == B3_PANEL_CRUMPLED
                    && panel_kind_detaches(s->kind[k])) {
                    s->state[k] = B3_PANEL_DETACHED;
                    if (w && w->active) panel_piece_spawn(s, k, w);
                    s->detached_total++;
                    todo--;
                    acted = 1;
                }
                break;                                 // first of this kind
            }
        }
        if (!acted) break;
    }

    // Second pass: dent (never detach) until only `keep` are still 0/1.
    int fresh = 0;
    for (int k = 0; k < s->n; k++)
        if (s->state[k] == B3_PANEL_PRISTINE || s->state[k] == B3_PANEL_LOOSE)
            fresh++;
    todo = fresh - keep;
    while (todo > 0) {
        int acted = 0;
        for (int pi = 0; pi < 7 && !acted; pi++) {
            for (int k = 0; k < s->n; k++) {
                if (s->kind[k] != B3_PANEL_PRIORITY[pi]) continue;
                if (s->state[k] == B3_PANEL_PRISTINE
                    || s->state[k] == B3_PANEL_LOOSE) {
                    s->state[k] = B3_PANEL_CRUMPLED;
                    todo--;
                    acted = 1;
                }
                break;
            }
        }
        if (!acted) break;
    }
}

// ---------------------------------------------------------------------------
// Piece flight.  The integrator is the verified FUN_00109560 port in
// NON-crash mode (gravity at the centre of mass, no torque -- retail's
// flying part is not a race vehicle, so it takes b3_rigid_body_integrate's
// !in_race && !state6 arm).  Everything around it -- the ground plane, the
// restitution, the tangential scrub and the rest test -- is GLUE: retail's
// part rides the full collision world through its pool vtable.
// ---------------------------------------------------------------------------
// PH-05 RECOVERED.  The six constants that used to live here
// (B3_PIECE_RESTITUTION/_FRICTION/_SPIN_DAMP/_AIR_DAMP/_REST_SPEED/
// _REST_TIME) were a stand-in for the real class-7 chain, which is now
// ported in burnout3_vehicle_sim.c and differentially verified against the
// real x86 (tools/validate_port.py, 20 world-contact + 12 class-7 cases):
//     narrow phase  FUN_00107950  -> b3_rigid_body_obb_plane_contact()
//     resolve       FUN_00109EA0  -> b3_rigid_body_world_contact()
//     update        FUN_00106D00  -> b3_rigid_body_class7_update()
// A detached panel is a class-7 rigid body: ctor FUN_001068A0 @0x001068DA
// sets +0x215 = 7, the pool is gameworld+0xD3380 (0x40 slots, stride 0x4E0)
// and the collision manager FUN_00110AF0 runs slot +0x10 (FUN_001072A0,
// the world contact) and then slot +0 (FUN_00106D00, the update) once per
// frame per allocated slot -- contact FIRST, so the impulse and the push-out
// land in +0x110/+0x120/+0x130 and the SAME frame's FUN_00109560 consumes
// them.  The restitution is the rigid-body ctor's +0x1F8 = [0x003A69C4] =
// 0.1 (FUN_00109270 @0x001094C5).
#define B3_PIECE_RESTITUTION 0.1f     /* [0x003A69C4] @0x001094C5      [C] */

void b3_panels_pieces_update(B3PanelSet* s,
                             const float ground_y[B3_PANEL_MAX], float dt) {
    for (int k = 0; k < s->n; k++) {
        B3PanelPiece* p = &s->piece[k];
        if (!p->active) continue;
        p->life += dt;

        // ---- into GAME space (b3_mat_orthonormalize is chirality-bound;
        // the same round trip B3WreckState documents) --------------------
        B3RigidBody rb;
        memset(&rb, 0, sizeof(rb));
        memcpy(rb.frame, p->frame, sizeof(rb.frame));
        memcpy(rb.vel, p->vel, sizeof(rb.vel));
        memcpy(rb.angmom, p->angmom, sizeof(rb.angmom));
        memcpy(rb.omega, p->omega, sizeof(rb.omega));
        mirror_frame(rb.frame);
        mirror_vec(rb.vel);
        mirror_pseudo(rb.angmom);
        mirror_pseudo(rb.omega);
        memcpy(rb.inv_inertia_body, p->iinv_body, sizeof(rb.inv_inertia_body));
        panel_world_inertia((const float(*)[4])rb.frame,
                            (const float(*)[4])p->iinv_body,
                            rb.inv_inertia_world);
        panel_inv_frame((const float(*)[4])rb.frame, rb.inv_frame);
        for (int i = 0; i < 3; i++)
            rb.dir[i] = (p->vel[3] > 1e-4f) ? rb.vel[i] / p->vel[3]
                                            : rb.frame[2][i];

        // ---- vtable slot +0x10 first: FUN_001072A0 -> FUN_00109EA0 ----
        // The manager resolves the piece against the world BEFORE its own
        // update integrates, so the contact impulse (+0x110/+0x120) and the
        // penetration push-out (+0x130) are consumed by the same frame's
        // FUN_00109560.  The half-extents are the piece's OBB (retail's
        // +0x1D0 bbmax / +0x1E0 bbmin).
        B3Class7State c7;
        memset(&c7, 0, sizeof c7);
        /* +0x2BA.  FUN_00125A50 @0x00125A6F pushes 1 for a body panel, so a
         * detached panel is mode 1, NOT 0 -- FUN_001069C0 @0x001069EA
         * stamps the allocator's 4th argument straight into +0x2BA.  Mode 0
         * is a detached WHEEL (FUN_00123000 @0x001231E0 PUSH 0) and is the
         * only case that reaches FUN_001072A0's sphere arm.  The class-7
         * update therefore takes the L *= 0.98 branch @0x00106D89, not the
         * grounded-settle branch.                                      [C] */
        c7.attach_2ba = 1;
        {
            const float* bbmin = p->bbmin;
            const float* bbmax = p->bbmax;
            const float ppt[3] = { rb.frame[3][0], ground_y[k], rb.frame[3][2] };
            const float pn[3] = { 0.0f, 1.0f, 0.0f };
            B3WorldContact ct;
            B3WorldContactResult res;
            const int hit = b3_rigid_body_obb_plane_contact(
                &rb, bbmin, bbmax, ppt, pn, &ct);
            b3_rigid_body_world_contact(&rb, p->mass, 7, c7.attach_2ba,
                                        B3_PIECE_RESTITUTION,
                                        hit ? &ct : NULL, &res);
            c7.grounded_212 = res.grounded;
            for (int i = 0; i < 4; i++) c7.normal[i] = ct.normal[i];
            if (res.sleep) p->rest = 1.0f; else p->rest = 0.0f;
        }

        // ---- then vtable slot +0: FUN_00106D00, which ends in the
        // integrator FUN_00109560.
        b3_rigid_body_class7_update(&rb, p->mass, 0.0f, &c7, dt);

        // ---- back to harness space --------------------------------------
        memcpy(p->frame, rb.frame, sizeof(p->frame));
        memcpy(p->vel, rb.vel, sizeof(p->vel));
        memcpy(p->angmom, rb.angmom, sizeof(p->angmom));
        memcpy(p->omega, rb.omega, sizeof(p->omega));
        mirror_frame(p->frame);
        mirror_vec(p->vel);
        mirror_pseudo(p->angmom);
        mirror_pseudo(p->omega);
    }
}

// ---------------------------------------------------------------------------
// One crashed frame, in retail's own order:
//   FUN_00123FD0  snapshot -> impact add       (steps 1 and 2)
//   FUN_0012E4D0 -> FUN_0012C860  rip pass     (step 3)
//   FUN_00123000  scan + FUN_00125A50          (step 4)
// then the released pieces fly.
//
// GLUE: the impact SOURCE.  Retail's obj+0x47C is built by the suspension
// pass out of the frame's contact impulses; the harness wreck reports its own
// box-corner contacts through B3WreckState (hit_count/hit_normal/hit_impulse),
// and those go through the SAME class scale and gate.
// ---------------------------------------------------------------------------
void b3_panels_crash_frame(B3PanelSet* s, B3WreckState* w) {
    if (!s || s->n <= 0) return;

    // 1. The snapshot is NOT taken here.  Retail's snapshot (FUN_00123FD0
    // @0x001246BB, FUN_0012FA40 @0x0012FCD0) sits immediately before each
    // ADD, and b3_panels_add_impact carries it -- which is what lets the
    // crash entry's burst (b3_panels_entry_burst, fired from the harness's
    // wreck_begin_for) still be rip-tested by THIS frame's visual pass, the
    // same way retail's FUN_00115130 burst is rip-tested by the FUN_0012C860
    // that runs later in its own entry frame.  A frame with no impact leaves
    // the last rise standing, which is idempotent: it either already ripped
    // the panel or is still under its band.

    // 2. Retail has TWO arms into FUN_0012C670 and they scale differently:
    //    * the COLLISION resolver (FUN_00111CD0 -> FUN_0012FA40): the raw hit
    //      takes the class scale straight, gate 5.0;
    //    * the per-frame CONTACT arm (FUN_00123FD0): the contact only counts
    //      once its raw impulse clears 2000 (@0x00124364) and it is HALVED
    //      before the class scale (@0x001243F6) -- which is why a wreck that
    //      merely slides along the ground loses no bodywork, in retail or
    //      here.
    if (w && w->active && w->hit_count > 0) {
        if (w->hit_collision) {
            b3_panels_impact_world(s, (const float(*)[4])w->frame,
                                   w->hit_normal, w->hit_impulse);
        } else if (w->hit_impulse > B3_PANEL_WHEEL_GATE) {
            b3_panels_impact_world(s, (const float(*)[4])w->frame,
                                   w->hit_normal,
                                   w->hit_impulse * B3_PANEL_WHEEL_HALF);
        }
    }
    // consumed: b3_wreck_update overwrites the report every frame, so the
    // entry hit b3_wreck_begin_kick published would otherwise be lost
    if (w) { w->hit_count = 0; w->hit_impulse = 0.0f; w->hit_collision = 0; }

    // 3 + 4
    b3_panels_visual_pass(s);
    b3_panels_scan(s, w);

}
