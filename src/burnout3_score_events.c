/* burnout3_score_events.c -- ONCOMING / NEAR MISS / DRIFT / AIR earn events.
 *
 * Every rule below is a 1:1 port of the retail code named in the header;
 * the differential test tools/validate_score_events.py runs the ORIGINAL
 * x86 under Unicorn from the same seeded state and asserts this C matches
 * (event open/close, accumulators, awarded units, BP, tier).
 *
 * No constant in this file was invented: each one is either a registrar
 * parameter (address in burnout3_score_events.h) or an immediate read off
 * the instruction cited beside it.
 */
#include "burnout3_score_events.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

B3ScoreParams b3_score_params;

/* Harness diagnostic only (no retail counterpart): B3_SCORE_TRACE=1 prints
 * one line per near-miss arm / contact-disarm / payout / crash reset, which
 * is how the classifier is checked against a live race. */
static int se_trace(void)
{
    static int on = -1;
    if (on < 0) on = getenv("B3_SCORE_TRACE") != NULL;
    return on;
}

/* Trace-only: a stable small index per B3ScoreEvents instance, assigned in
 * b3_score_events_reset() order (the harness resets car 0 first). */
static const void* se_tag_tab[16];
static int se_tag_n;
static int se_tag(const B3ScoreEvents* s)
{
    int i;
    for (i = 0; i < se_tag_n; i++) if (se_tag_tab[i] == (const void*)s) return i;
    if (se_tag_n < 16) { se_tag_tab[se_tag_n] = (const void*)s; return se_tag_n++; }
    return -1;
}

/* ===================================================================== *
 * Parameters
 * ===================================================================== */

/* Compiled-in defaults -- the .data image at 0x3F73A0.., i.e. what a
 * pre-init emulation of the real functions reads.  Verified byte-for-byte
 * against build/burnout3.elf by the validator. */
void b3_score_params_defaults(B3ScoreParams* p)
{
    static const B3ScoreParams d = {
        5.0f, 2.0f,                 /* air:      min m, boost/m            */
        30.0f, 10.0f, 0.3f,         /* oncoming: min mph, min m, boost/m   */
        30.0f, 10.0f, 1.0f,         /* drift:    min mph, min m, boost/m   */
        2.0f, 30.0f, 30.0f, 60.0f, 5.0f,   /* near miss                    */
        {100, 250, 500, 1000},      /* air category BP                     */
        {100, 250, 500, 1000},      /* oncoming category BP                */
        {100, 250, 500, 1000},      /* drift category BP                   */
        {5, 10, 15, 25},            /* near miss category BP               */
        20,                         /* near miss BP per chain link         */
        {15.0f, 30.0f, 60.0f, 90.0f},      /* air minima (m)               */
        {50.0f, 100.0f, 200.0f, 300.0f},   /* oncoming minima (m)          */
        {45.0f, 90.0f, 180.0f, 270.0f},    /* drift minima (m)             */
        {999.0f, 2.0f, 5.0f, 9.0f},        /* near miss minima (counts)    */
        /* RUBBING -- the .data image at the cited addresses, read straight
         * out of build/burnout3.elf. */
        0.3f,                       /* 0x3F73F0 Min Contact Time For Rubbing */
        15.0f,                      /* 0x3F73F4 Boost value for rubbing      */
        1.0f,                       /* 0x3F7404 Maximum Crash Wait Time      */
        0.5f,                       /* 0x3F7408 Max Crash Wait Time - No Slam*/
        {5, 10, 15},                /* 0x3F74CC Rubbing Category BP          */
        {0.1f, 4.0f, 8.0f}          /* 0x3F758C rubbing minima (seconds)     */
    };
    *p = d;
}

/* The VDB-tuned values the shipped game plays with (Data/vdb.xml, resolved
 * through the game's own key hash -- docs/RE_GAMEPLAY.md 1-2).  These are
 * the same numbers burnout3_gameplay.h already carries for the awards; the
 * tier minima and category BP tables come from the same table. */
void b3_score_params_vdb(B3ScoreParams* p)
{
    static const B3ScoreParams v = {
        B3_AIR_MIN_M, B3_AIR_BOOST_PER_M,
        B3_ONCOMING_MIN_MPH, B3_ONCOMING_MIN_M, B3_ONCOMING_PER_M,
        B3_DRIFT_MIN_MPH, B3_DRIFT_MIN_M, B3_DRIFT_PER_M,
        B3_NEARMISS_DIST, B3_NEARMISS_MIN_MPH,
        60.0f,                      /* Minimum Near Miss Chain Speed (MPH) */
        B3_NEARMISS_BOOST, B3_NEARMISS_CHAIN_S,
        {0, 10, 50, 100},           /* Air Category BP                     */
        {0, 20, 75, 150},           /* Oncoming Category BP                */
        {0, 20, 50, 100},           /* Drift Category BP                   */
        {5, 10, 15, 25},            /* Near Miss Category BP (no VDB entry:
                                     * the compiled default stands)        */
        B3_BP_NEAR_MISS,            /* Near Miss BP per chain link (= 0)   */
        /* The tuned tier-0 minima are huge sentinels exactly where the
         * tuned tier-0 BP is 0: Criterion disabled the lowest medal tier
         * for air/oncoming/drift in the retail tune (RE_GAMEPLAY 2). */
        {1115.0f, 10.0f, 20.0f, 35.0f},
        {9150.0f, 500.0f, 900.0f, 1400.0f},
        {9980.0f, 100.0f, 150.0f, 250.0f},
        {999.0f, 2.0f, 5.0f, 9.0f}, /* near miss: no VDB entry             */
        /* No VDB entry recovered for any of the Aggressive/Takedown rubbing
         * parameters, so the compiled defaults stand -- same treatment the
         * Near Miss Category BP already gets above. */
        0.3f, 15.0f, 1.0f, 0.5f,
        {5, 10, 15},
        {0.1f, 4.0f, 8.0f}
    };
    *p = v;
}

void b3_score_events_init(void)
{
    b3_score_params_vdb(&b3_score_params);
}

/* ===================================================================== *
 * FUN_00192D20 -- the category tier tracker (shared by all four events)
 * ===================================================================== */
void b3_cat_track(B3CatRecord* r, float value, float clock)
{
    int n, i;
    const float* p;

    r->prev_value = r->value;      /* param_1[2] = *param_1 */
    r->value = value;              /* *param_1 = in_XMM0_Da */
    r->clock = clock;              /* param_1[1] = *(clocksrc + 0xC) */

    n = r->count;
    if ((int)r->tier < n - 1 && n != 0) {
        p = r->minima + n;
        i = n;
        for (;;) {
            --p;
            --i;
            if (*p <= value) break;
            if (i == 0) return;    /* no threshold reached: tier unchanged */
        }
        r->prev_tier = r->tier;
        r->tier = (signed char)i;
    }
}

void b3_cat_reset(B3CatRecord* r, float clock)
{
    r->prev_value = r->value;
    r->prev_tier = r->tier;
    r->clock = clock;
    r->value = 0.0f;
    r->tier = -1;
}

/* ===================================================================== *
 * The HUD earn callout.  A closing event claims the announcement slot only
 * when the pending id is <= its own (see B3_SE_EVID_* in the header).
 * ===================================================================== */
static void se_callout(B3ScoreEvents* s, int event_id, int cat, int tier)
{
    /* A CRASHED car never claims the announcement.  Every one of the six
     * claim sites is `CMP [score+0x254],<id> / JLE take` and then, inside
     * `take`, `MOV AL,[racecar+0x18FA] / TEST AL,AL / JNZ done`:
     *   drift    FUN_00196E10   air   FUN_00196940 @0x00196A72
     *   oncoming FUN_00196BE0   near miss FUN_00194EE0 @0x00195xxx
     *   0x75 event FUN_001935F0 @0x00193FFE
     *   perfect/burnout lap FUN_00194600 @0x00194772 / @0x0019488C
     *   boost start FUN_00197AD0 @0x00197B7A
     * The Burnout Points are added BEFORE that test and are NOT lost -- only
     * the HUD callout is dropped. */
    if (s->rc_crashed) return;
    if (s->callout_id <= event_id) {
        s->callout_id = event_id;
        s->callout_cat = cat;
        s->callout_tier = tier;
    }
}

int b3_score_events_take_callout(B3ScoreEvents* s, int* cat, int* tier)
{
    if (s->callout_id == 0) return 0;
    if (cat) *cat = s->callout_cat;
    if (tier) *tier = s->callout_tier;
    s->callout_id = 0;
    return 1;
}

/* ===================================================================== *
 * The air / oncoming / drift template.
 *
 * FUN_00196940 / FUN_00196BE0 / FUN_00196E10 are the same function three
 * times over; only the state flag, the speed gate, the record and the
 * parameter set differ.  Structure (oncoming shown, @0x00196BE0):
 *
 *   active = state_flag && (speed_mph >= min_mph)      ; AIR HAS NO SPEED GATE
 *   if (active) {
 *       b3_cat_track(rec, rec->value + dist_step, clock)
 *       if (rec->value > min_dist)                     ; strictly greater
 *           award(per_m * (first_payment ? rec->value : dist_step))
 *   } else if (this event ever paid) {
 *       if (tier >= 0) { BP += cat_bp[tier]; claim the callout }
 *       total += rec->value; best = max(best, rec->value)
 *   }
 *   ... then the record resets unconditionally, tier := -1
 *
 * The FIRST payment covers the whole accumulated distance (including the
 * metres below the minimum); later frames pay only their own step.
 * ===================================================================== */
static void cat_event(B3ScoreEvents* s, B3BoostBar* bar,
                      B3CatRecord* rec,
                      unsigned char* active, unsigned char* scored,
                      int state, int use_speed_gate,
                      float speed_mph, float min_mph,
                      float dist_step, float clock,
                      float min_dist, float per_m,
                      const int* cat_bp, int event_id, int cat,
                      float* total_stat, float* best_stat, int* count_stat)
{
    int on = state && (!use_speed_gate || speed_mph >= min_mph);
    *active = (unsigned char)on;

    if (on) {
        b3_cat_track(rec, rec->value + dist_step, clock);
        if (rec->value > min_dist) {
            /* first payment of this event pays for everything so far */
            float units = (*scored == 0) ? rec->value : dist_step;
            b3_boost_award(bar, per_m * units);
            *scored = 1;
        }
        return;
    }

    if (*scored) {
        if (rec->tier >= 0) {
            int bp = cat_bp[(int)rec->tier];
            s->bp += bp;          /* score+0x4C  (racecar+0x111C) */
            s->bp_event += bp;    /* score+0xB8  (racecar+0x1188) */
            se_callout(s, event_id, cat, (int)rec->tier);
        }
        *total_stat += rec->value;
        if (count_stat) (*count_stat)++;          /* air only (score+0x354) */
        if (*best_stat < rec->value) *best_stat = rec->value;
    }

    b3_cat_reset(rec, clock);
    *active = 0;
    *scored = 0;
}

/* ===================================================================== *
 * THE CRASH GATE -- FUN_001935F0.
 *
 * 0x001939AD  MOV AL,byte ptr [ESI + 0x18fa]   ; racecar, crashed
 * 0x001939B3  TEST AL,AL
 * 0x001939CB  JNZ 0x00193a80                   ; -> the reset block
 * 0x001939D1  MOV AL,byte ptr [ESI + 0x18fb]   ; racecar, respawning
 * 0x001939D7  TEST AL,AL
 * 0x001939D9  JNZ 0x00193a80
 * 0x001939DF  ...                              ; distance step + detectors
 *
 * i.e. FUN_00013C10 (the step), FUN_00194A80 (rubbing), FUN_00194EE0 (near
 * miss), FUN_00196940/BE0/E10 (air/oncoming/drift) are ALL skipped, and the
 * block at 0x00193A80 runs in their place.  Nothing is paid there: it only
 * destroys state.  This is why crashing into traffic cannot produce a NEAR
 * MISS callout as the wreck separates -- the armed slot is wiped, not
 * awarded.
 * ===================================================================== */
void b3_score_events_set_crash(B3ScoreEvents* s, int crashed, int respawning)
{
    if (se_trace() && (s->rc_crashed != (crashed != 0)
                       || s->rc_respawning != (respawning != 0)))
        printf("[score] car %d GATE crashed=%d respawning=%d\n",
               se_tag(s), crashed != 0, respawning != 0);
    s->rc_crashed    = (unsigned char)(crashed != 0);
    s->rc_respawning = (unsigned char)(respawning != 0);
}

void b3_score_events_set_race_finished(B3ScoreEvents* s, int finished)
{
    s->race_finished = (unsigned char)(finished != 0);
}

int b3_score_events_suspended(const B3ScoreEvents* s)
{
    return s->rc_crashed != 0 || s->rc_respawning != 0;
}

void b3_score_events_crash_reset(B3ScoreEvents* s, float clock)
{
    int i;

    /* 0x00193A86..0x00193A8F: EAX = -1; [score+0x3E8] = [score+0x3EC] = EAX,
     * i.e. all eight slot ids released in two dword stores.  The ARMED flags
     * (score+0x410) and the last-seen clocks (score+0x3F0) are deliberately
     * left alone -- a released slot is re-armed when it is next claimed. */
    if (se_trace()) {
        for (i = 0; i < B3_SE_NM_SLOTS; i++)
            if (s->nm_id[i] != -1 && s->nm_armed[i])
                printf("[score] car %d t=%.2f NM VOID(crash) slot %d id %d\n",
                       se_tag(s), clock, i, (int)s->nm_id[i]);
    }
    for (i = 0; i < B3_SE_NM_SLOTS; i++) s->nm_id[i] = -1;

    s->nm_chain = 0;              /* 0x00193AA0  MOV [EDI+0x3d0],ECX(0)   */
    s->air_scored   = 0;          /* 0x00193AAC  MOV [EDI+0x3c8],CL(0)    */
    s->onc_scored   = 0;          /* 0x00193AB2  MOV [EDI+0x3c9],CL       */
    s->drift_scored = 0;          /* 0x00193AB8  MOV [EDI+0x3ca],CL       */

    /* 0x00193B36 / 0x00193B76 / 0x00193BB3 / 0x00193CA7: the standard
     * end-of-event record reset, but WITHOUT the paying tail -- no Category
     * BP, no total/best stat, no callout claim. */
    /* 0x00193AE5..0x00193B34: the per-opponent contact arrays.  The loop
     * runs DAT_0073A19C (racer count) times over
     *     [score+0x528+i*4] = 0.0        the rubbing timer
     *     [score+0x55E+i]   = 0          in-contact this frame
     *     [score+0x558+i]   = 0          in-contact last frame
     * (it also writes score+0x440+i*4 = -1.0 and score+0x438+i = 0, a fourth
     *  per-opponent pair this module does not carry). */
    for (i = 0; i < B3_SE_RUB_CARS; i++) {
        s->rub_time[i] = 0.0f;
        s->rub_touch[i] = 0;
        s->rub_prev_touch[i] = 0;
    }

    b3_cat_reset(&s->air, clock);    s->air_active   = 0;
    b3_cat_reset(&s->onc, clock);    s->onc_active   = 0;
    b3_cat_reset(&s->drift, clock);  s->drift_active = 0;
    /* 0x00193BF0..0x00193C26: the RUBBING record takes the same reset, and
     * score+0x574 (active) / score+0x575 (tier) go back to 0 / -1. */
    b3_cat_reset(&s->rub, clock);    s->rub_active   = 0;
    b3_cat_reset(&s->nm, clock);     s->nm_active    = 0;

    /* Not reset here (verified against the real block): nm_seen, nm_armed,
     * nm_total, nm_last, nm_chain_end, prev_clock, bp/bp_event, the stats
     * and the pending callout. */
}

void b3_score_events_frame(B3ScoreEvents* s, B3BoostBar* bar,
                           const B3ScoreFrame* in)
{
    const B3ScoreParams* P = &b3_score_params;

    /* The gate at 0x001939AD/0x001939D1 -- before anything else.  The two
     * racecar bytes come in through b3_score_events_set_crash(). */
    if (b3_score_events_suspended(s)) {
        b3_score_events_crash_reset(s, in->clock);
        return;
    }

    /* FUN_00196940 -- AIR.  No speed gate: the airborne flag alone opens it. */
    cat_event(s, bar, &s->air, &s->air_active, &s->air_scored,
              in->airborne, 0, in->speed_mph, 0.0f,
              in->dist_step, in->clock,
              P->air_min_m, P->air_boost_per_m,
              P->air_cat_bp, B3_SE_EVID_AIR, B3_SE_CAT_AIR,
              &s->air_total, &s->air_best, &s->air_count);

    /* FUN_00196BE0 -- ONCOMING. */
    cat_event(s, bar, &s->onc, &s->onc_active, &s->onc_scored,
              in->oncoming, 1, in->speed_mph, P->onc_min_mph,
              in->dist_step, in->clock,
              P->onc_min_m, P->onc_boost_per_m,
              P->onc_cat_bp, B3_SE_EVID_ONCOMING, B3_SE_CAT_ONCOMING,
              &s->onc_total, &s->onc_best, NULL);

    /* FUN_00196E10 -- DRIFT. */
    cat_event(s, bar, &s->drift, &s->drift_active, &s->drift_scored,
              in->drifting, 1, in->speed_mph, P->drift_min_mph,
              in->dist_step, in->clock,
              P->drift_min_m, P->drift_boost_per_m,
              P->drift_cat_bp, B3_SE_EVID_DRIFT, B3_SE_CAT_DRIFT,
              &s->drift_total, &s->drift_best, NULL);
}

/* ===================================================================== *
 * FUN_00195DD0 -- the near-miss proximity test.
 *
 * A ground-plane OBB-vs-OBB minimum-gap test with a height gate.  Both
 * boxes are reduced to a centre + two half-axis vectors (row0 * halfX,
 * row2 * halfZ); the axes are flipped to point at the other box, giving
 * the two nearest corners; the gap is then the smallest of |corner-corner|
 * and the four point-to-edge distances.
 * ===================================================================== */
int b3_score_obb_near(const B3ScoreObb* me, const B3ScoreObb* other,
                      float dist)
{
    float h1x, h1z, c1x, c1z, h2x, h2z, c2x, c2z;
    float ax, az, bx, bz, cx, cz, dx_, dz_;
    float dx, dz, px, pz, best, t, qx, qz, tmp;

    /* +-4 m height gate: other's row3.y vs mine (imm @0x00195DE9/0x00195DF8) */
    tmp = other->m[3][1] - me->m[3][1];
    if (tmp > 4.0f || tmp < -4.0f) return 0;

    /* half extents + centre of each box, from the .bgv corners */
    h1x = (me->bmax[0] - me->bmin[0]) * 0.5f;
    h1z = (me->bmax[2] - me->bmin[2]) * 0.5f;
    c1x = h1x + me->bmin[0];
    c1z = h1z + me->bmin[2];
    h2x = (other->bmax[0] - other->bmin[0]) * 0.5f;
    h2z = (other->bmax[2] - other->bmin[2]) * 0.5f;
    c2x = h2x + other->bmin[0];
    c2z = h2z + other->bmin[2];

    /* half-axis vectors in world XZ */
    ax = me->m[0][0] * h1x;  az = me->m[0][2] * h1x;   /* mine, row0    */
    bx = me->m[2][0] * h1z;  bz = me->m[2][2] * h1z;   /* mine, row2    */
    cx = other->m[0][0] * h2x;  cz = other->m[0][2] * h2x;  /* other row0 */
    dx_ = other->m[2][0] * h2z; dz_ = other->m[2][2] * h2z; /* other row2 */

    /* centre-to-centre delta, both centres taken to world space */
    dx = (other->m[0][0] * c2x + other->m[2][0] * c2z + other->m[3][0])
       - (me->m[0][0] * c1x + me->m[2][0] * c1z + me->m[3][0]);
    dz = (other->m[2][2] * c2z + other->m[0][2] * c2x + other->m[3][2])
       - (me->m[2][2] * c1z + me->m[0][2] * c1x + me->m[3][2]);

    /* bounding-circle reject */
    if (sqrtf(h2x * h2x + h2z * h2z) + sqrtf(h1x * h1x + h1z * h1z) + dist
        < sqrtf(dx * dx + dz * dz))
        return 0;

    /* orient each half-axis toward the other box */
    if (dx * ax + dz * az < 0.0f) { ax = -ax; az = -az; }
    if (dx * bx + dz * bz < 0.0f) { bx = -bx; bz = -bz; }
    if (dx * cx + dz * cz >= 0.0f) { cz = -cz; cx = -cx; }
    if (dx * dx_ + dz * dz_ >= 0.0f) { dx_ = -dx_; dz_ = -dz_; }

    /* nearest corner of mine -> nearest corner of theirs */
    tmp = (dx_ + cx) - (bx + ax);
    px = tmp + dx;
    pz = ((dz_ + cz) - (bz + az)) + dz;

    best = px * px + pz * pz;

    /* against the other box's two edges */
    tmp = px * dx_ + pz * dz_;
    if (tmp < 0.0f) {
        t = px * cx + pz * cz;
        if (t >= 0.0f) {
            t = t / (cx * cx + cz * cz);
            qx = px - cx * t;
            qz = pz - cz * t;
            tmp = qz * qz + qx * qx;
            if (tmp < best) best = tmp;
        }
    } else {
        t = tmp / (dx_ * dx_ + dz_ * dz_);
        qz = dz_ * t;
        qx = px - dx_ * t;
        tmp = (pz - qz) * (pz - qz) + qx * qx;
        if (tmp < best) best = tmp;
    }

    /* against my box's two edges */
    tmp = px * bx + pz * bz;
    if (tmp >= 0.0f) {
        tmp = px * ax + pz * az;
        if (tmp < 0.0f) {
            t = tmp / (ax * ax + az * az);
            qz = az * t;
            px = px - ax * t;
            pz = pz - qz;
            tmp = pz * pz + px * px;
            if (tmp < best) best = tmp;
        }
        /* both dots >= 0: the corner distance already is the gap */
    } else {
        t = tmp / (bx * bx + bz * bz);
        px = px - bx * t;
        qz = bz * t;
        pz = pz - qz;
        tmp = pz * pz + px * px;
        if (tmp < best) best = tmp;
    }

    return best < dist * dist;
}

/* ===================================================================== *
 * FUN_00194EE0 -- NEAR MISS.
 *
 * Three passes per frame:
 *  1. proximity scan.  Every candidate that passes the OBB gap test and is
 *     met at >= "Minimum Near Miss Speed" claims one of 8 tracking slots
 *     (refresh if already tracked, else a free slot, else evict the least
 *     recently seen) and is ARMED.
 *  2. chain expiry.  A live chain dies "Near Miss Chain Time" after its
 *     last link, or the moment speed drops below the chain speed; the chain
 *     length's tier then pays Near Miss Category BP and claims the callout.
 *  3. award scan.  A slot whose last-proximity clock has fallen behind the
 *     PREVIOUS frame's clock is a completed pass: disarm, chain += 1,
 *     award "Boost value per Near Miss" and chain_len * "Near Miss BP",
 *     retier on the chain length.  The slot is released 1 s later, which is
 *     what makes the award once-per-vehicle-per-pass.
 * ===================================================================== */
void b3_score_events_near_miss(B3ScoreEvents* s, B3BoostBar* bar,
                               const B3ScoreObb* me, float speed_mph,
                               float clock,
                               const B3ScoreObb* others, const int* ids,
                               int n_others)
{
    const B3ScoreParams* P = &b3_score_params;
    int j, i, k;

    /* The crash gate (FUN_001935F0 @0x001939AD/0x001939D1): FUN_00194EE0 sits
     * at 0x00193A09, inside the branch the gate jumps over, so while the car
     * is crashed or being re-placed nothing here happens at all -- no slot is
     * claimed, no armed slot pays, and score+0x3DC does not advance.  The
     * reset that replaces this work is b3_score_events_crash_reset(), run
     * once per frame by b3_score_events_frame(). */
    if (b3_score_events_suspended(s)) return;

    /* ---- 1. proximity scan ---------------------------------------- */
    for (j = 0; j < n_others; j++) {
        signed char id;
        float oldest;

        if (!b3_score_obb_near(me, &others[j], P->nm_dist)) continue;
        if (speed_mph < P->nm_min_mph) continue;
        id = (signed char)ids[j];

        for (i = 0; i < B3_SE_NM_SLOTS; i++)
            if (s->nm_id[i] == id) { s->nm_seen[i] = clock; break; }
        if (i < B3_SE_NM_SLOTS) continue;          /* already tracked */

        for (i = 0; i < B3_SE_NM_SLOTS; i++)
            if (s->nm_id[i] == -1) {
                s->nm_id[i] = id;
                s->nm_seen[i] = clock;
                s->nm_armed[i] = 1;
                if (se_trace())
                    printf("[score] car %d t=%.2f NM ARM slot %d id %d\n",
                           se_tag(s), clock, i, (int)id);
                break;
            }
        if (i < B3_SE_NM_SLOTS) continue;          /* took a free slot */

        /* evict the least recently seen slot, but only one that has not
         * been seen since the previous frame */
        oldest = s->prev_clock;
        k = -1;
        for (i = 0; i < B3_SE_NM_SLOTS; i++)
            if (s->nm_seen[i] < oldest) { oldest = s->nm_seen[i]; k = i; }
        if (k < 0) continue;
        s->nm_id[k] = id;
        s->nm_seen[k] = clock;
        s->nm_armed[k] = 1;
        if (se_trace())
            printf("[score] car %d t=%.2f NM ARM(evict) slot %d id %d\n",
                   se_tag(s), clock, k, (int)id);
    }

    /* ---- 2. chain expiry ------------------------------------------ */
    if (s->nm_chain != 0
        && (s->nm_last + P->nm_chain_time < clock
            || speed_mph < P->nm_chain_mph)) {
        s->nm_chain_end = clock;
        s->nm_chain = 0;
        if (s->nm.tier >= 0) {
            int bp = P->nm_cat_bp[(int)s->nm.tier];
            s->bp += bp;
            s->bp_event += bp;
            se_callout(s, B3_SE_EVID_NEARMISS, B3_SE_CAT_NEARMISS,
                       (int)s->nm.tier);
        }
        b3_cat_reset(&s->nm, clock);
        s->nm_active = 0;
    }

    /* ---- 3. award scan -------------------------------------------- */
    for (i = 0; i < B3_SE_NM_SLOTS; i++) {
        if (s->nm_id[i] == -1) continue;

        if (s->nm_seen[i] < s->prev_clock && s->nm_armed[i]) {
            int n, t, bp;
            const float* p;

            s->nm_armed[i] = 0;
            if (se_trace())
                printf("[score] car %d t=%.2f NM PAY slot %d id %d chain %d\n",
                       se_tag(s), clock, i, (int)s->nm_id[i], s->nm_chain + 1);
            s->nm_chain++;      /* score+0x3D0 */
            s->nm_total++;      /* score+0x3CC */

            b3_boost_award(bar, P->nm_boost);

            bp = s->nm_chain * P->nm_bp_per_link;
            s->bp += bp;
            s->bp_event += bp;

            s->nm_active = 1;
            /* the chain record's value IS the chain length; the tier scan
             * is FUN_00192D20's, inlined here in the original */
            s->nm.prev_value = s->nm.value;
            n = s->nm.count;
            t = s->nm.tier;
            s->nm.value = (float)s->nm_chain;
            s->nm.clock = clock;
            if (t < n - 1 && n != 0) {
                p = s->nm.minima + n;
                k = n;
                do {
                    --p;
                    --k;
                    if (*p <= (float)s->nm_chain) {
                        s->nm.prev_tier = s->nm.tier;
                        s->nm.tier = (signed char)k;
                        break;
                    }
                } while (k != 0);
            }
            s->nm_last = clock;     /* score+0x3E0 */
        }

        /* release the slot 1 s after it was last in proximity
         * (imm 1.0 @0x001951F?; the +1.0 is the constant in the compare) */
        if (s->nm_id[i] != -1 && s->nm_seen[i] + 1.0f < s->prev_clock)
            s->nm_id[i] = -1;
    }

    s->prev_clock = clock;          /* score+0x3DC */
}

/* ===================================================================== *
 * FUN_00197920 -- CONTACT: the near-miss cancel.
 *
 * 0x00197928  CMP  [param_3+0x27c],3        ; race over -> nothing
 * 0x00197934  MOV  EAX,[param_3+0xc8]       ; my racecar
 * 0x0019793a  CMP  BYTE [EAX+0x18fa],0      ; crashed  -> nothing
 * 0x00197979  CMP  [param_3+0x3e8+i],id     ; already tracked?  -> disarm
 * 0x001979b1  CMP  [param_3+0x3e8+i],0xff   ; free slot?        -> claim
 * 0x001979c3  MOV  [param_3+0x3e8+i],id
 * 0x001979c8  MOV  [param_3+0x3f0+i*4],now
 * 0x001979d4  MOV  BYTE [param_3+0x410+i],0 ; <- ARMED := 0
 *
 * NOTE the asymmetry that makes this work: the claim here does NOT arm the
 * slot, and FUN_00194EE0's proximity scan only arms a slot it takes off the
 * FREE list (0x0019508A is reached from the free-slot and evict paths, never
 * from the "already tracked" path at 0x00195092).  So a vehicle you have
 * touched stays un-armed until its slot is released a second after the last
 * proximity -- one contact kills the near miss for that whole pass.
 * ===================================================================== */
void b3_score_events_contact(B3ScoreEvents* s, int id, float clock)
{
    signed char want = (signed char)id;
    int i;

    /* A/B SWITCH, harness diagnostic only (retail has no such thing):
     * B3_SCORE_NO_CANCEL=1 restores the pre-recovery behaviour -- no contact
     * cancel at all -- so the reported bug can be demonstrated live against
     * the same race.  See the scoring agent's NOTE.md. */
    {
        static int off = -1;
        if (off < 0) off = getenv("B3_SCORE_NO_CANCEL") != NULL;
        if (off) return;
    }

    if (s->race_finished) return;          /* score+0x27C == 3            */
    if (s->rc_crashed) return;             /* racecar+0x18FA              */

    for (i = 0; i < B3_SE_NM_SLOTS; i++)
        if (s->nm_id[i] == want) goto disarm;

    i = 0;
    while (s->nm_id[i] != -1) {            /* 0x001979a8 loop shape       */
        i++;
        if (i > 7) return;                 /* no free slot: nothing at all*/
    }
    s->nm_id[i] = want;
    s->nm_seen[i] = clock;

disarm:
    if (se_trace() && s->nm_armed[i])
        printf("[score] car %d t=%.2f NM CANCEL(contact) slot %d id %d\n",
               se_tag(s), clock, i, id);
    s->nm_armed[i] = 0;
}

/* ===================================================================== *
 * FUN_001979E0 -- mark one opponent as in contact THIS frame.
 *
 * 0x001979e8  CMP  [param_1+0x27c],3
 * 0x001979f4  CMP  BYTE [[param_1+0xc8]+0x18fa],0
 * 00197a02    MOV  BYTE [param_1+0x55e+idx],1
 * 00197a0d    MOV  [param_1+0x510+idx*4],[param_1+0xc]
 * where idx = (s8)*(other_racecar+0x19BC), the other car's racer slot.
 * ===================================================================== */
void b3_score_events_mark_contact(B3ScoreEvents* s, int idx, float clock)
{
    if (s->race_finished) return;
    if (s->rc_crashed) return;
    if (idx < 0 || idx >= B3_SE_RUB_CARS) return;   /* harness bounds only */
    s->rub_touch[idx] = 1;
    s->rub_last[idx]  = clock;
}

/* ===================================================================== *
 * FUN_00194A80 -- RUBBING.
 * ===================================================================== */
void b3_score_events_rubbing(B3ScoreEvents* s, B3BoostBar* bar,
                             float clock, float dt,
                             const unsigned char* slam, int n)
{
    const B3ScoreParams* P = &b3_score_params;
    int i, fired = 0;

    /* FUN_00194A80 is called from inside the branch the crash gate jumps
     * over (0x001939DF..), exactly like FUN_00194EE0. */
    if (b3_score_events_suspended(s)) return;

    if (n > B3_SE_RUB_CARS) n = B3_SE_RUB_CARS;
    if (n < 0) n = 0;

    for (i = 0; i < n; i++) {
        if (!s->rub_touch[i]) {
            /* 0x00194C61: out of contact.  A live timer survives
             * "Maximum Crash Wait Time - No Slam" past the last contact
             * before it is zeroed, so a scrape that bounces stays one rub. */
            if (s->rub_time[i] > 0.0f
                && s->rub_last[i] + P->rub_grace_s < clock)
                s->rub_time[i] = 0.0f;
            continue;
        }
        if (slam && slam[i]) {
            /* 0x00194B7E..0x00194C22 -> 0x00194C88: this contact belongs to
             * a slam inside the last "Maximum Crash Wait Time" second.  A
             * slam is a takedown attempt, not rubbing. */
            s->rub_time[i] = 0.0f;
            continue;
        }
        if (s->rub_time[i] == 0.0f)          /* 0x00194C24/0x00194C34 */
            s->rub_start[i] = clock;
        s->rub_time[i] += dt;                /* 0x00194C3B += DAT_004AE1FC */
        if (s->rub_time[i] >= P->rub_min_s) {/* 0x00194C47 COMISS / JC     */
            fired = 1;
            s->rub_target = i;               /* 0x00194C59 [score+0x580]   */
        }
    }

    if (fired) {
        /* 0x00194CB4: accumulate rubbing SECONDS and re-tier (the same
         * top-down minima scan FUN_00192D20 does, inlined in the original
         * at 0x00194CED..0x00194D2B). */
        float v = s->rub.value + dt;
        int cnt = s->rub.count, k;
        s->rub.prev_value = s->rub.value;
        s->rub.value = v;
        s->rub.clock = clock;
        if ((int)s->rub.tier < cnt - 1 && cnt != 0) {
            const float* p = s->rub.minima + cnt;
            k = cnt;
            do {
                --p;
                --k;
                if (*p <= v) {
                    s->rub.prev_tier = s->rub.tier;
                    s->rub.tier = (signed char)k;
                    break;
                }
            } while (k != 0);
        }
        /* 0x00194D32..0x00194E22: "Boost value for rubbing" is a per-second
         * rate -- dt * 15.0 through the standard multiplier and clamp. */
        b3_boost_award(bar, dt * P->rub_boost_per_s);
        s->rub_active = 1;                   /* 0x00194E2E */
        return;
    }

    /* 0x00194E5A: nothing rubbed this frame.  If the event was open it
     * closes: retail hands (target, Rubbing Category BP[tier], tier) to
     * FUN_0019A050 -- the shared aggression/combo payout, which is NOT
     * ported, so the tier is published for the caller instead of an
     * invented BP number. */
    if (s->rub_active) {
        if (s->rub.tier >= 0) {              /* 0x00194E64 CMP EAX,-1 */
            s->rub_payout_tier   = s->rub.tier;
            s->rub_payout_target = s->rub_target;
        }
        b3_cat_reset(&s->rub, clock);        /* 0x00194E95..0x00194ECE */
        s->rub_active = 0;
    }
}

/* FUN_001935F0's tail @0x001940A1: after every detector has run, the
 * in-contact flags rotate into score+0x558 and clear.  Kept separate so a
 * caller can mark contacts and run the detectors in the game's own order. */
void b3_score_events_frame_end(B3ScoreEvents* s)
{
    int i;
    for (i = 0; i < B3_SE_RUB_CARS; i++) {
        s->rub_prev_touch[i] = s->rub_touch[i];   /* [eax-6] = [eax] */
        s->rub_touch[i] = 0;                      /* [eax]   = 0     */
    }
}

/* ===================================================================== *
 * Full reset -- race start.  tier := -1 is the game's own "no tier yet"
 * value (the detectors write 0xFF to the s8 field).
 *
 * NOT the same thing as b3_score_events_crash_reset(): a crash wipes the
 * slots, the chain, the `scored` flags and the four records, but KEEPS the
 * lifetime near-miss count, the BP totals, the stats and the previous-frame
 * clock (FUN_001935F0 @0x00193A80).  This one zeroes everything.
 * ===================================================================== */
static void cat_init_n(B3CatRecord* r, const float* minima, int count)
{
    memset(r, 0, sizeof(*r));
    r->minima = minima;
    r->count = (signed char)count;
    r->tier = -1;
    r->prev_tier = -1;
}
static void cat_init(B3CatRecord* r, const float* minima)
{
    cat_init_n(r, minima, 4);
}

void b3_score_events_reset(B3ScoreEvents* s)
{
    int i;
    memset(s, 0, sizeof(*s));
    cat_init(&s->air,   b3_score_params.air_minima);
    cat_init(&s->onc,   b3_score_params.onc_minima);
    cat_init(&s->drift, b3_score_params.drift_minima);
    cat_init(&s->nm,    b3_score_params.nm_minima);
    /* THREE tiers, not four: FUN_00192EA0 @0x001933A2 loads 3 into both
     * score+0x577 (count) and score+0x578, and 0x3F758C..0x3F7597 is a
     * three-float minima triple. */
    cat_init_n(&s->rub, b3_score_params.rub_minima, 3);
    s->rub_target = -1;
    s->rub_payout_tier = -1;
    s->rub_payout_target = -1;
    for (i = 0; i < B3_SE_RUB_CARS; i++) {
        s->rub_last[i] = 0.0f;
        s->rub_time[i] = 0.0f;
        s->rub_start[i] = 0.0f;
        s->rub_touch[i] = 0;
        s->rub_prev_touch[i] = 0;
    }
    for (i = 0; i < B3_SE_NM_SLOTS; i++) {
        s->nm_id[i] = -1;
        s->nm_seen[i] = 0.0f;
        s->nm_armed[i] = 0;
    }
    s->prev_clock = 0.0f;
    s->nm_last = 0.0f;
    s->nm_chain_end = 0.0f;
    s->callout_id = 0;
}
