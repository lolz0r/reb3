// Burnout 3 gameplay rules: boost bar, scoring, takedown bookkeeping,
// out-of-control reaction -- recovered from the retail default.xbe and
// VERIFIED by differential execution (tools/validate_gameplay.py, 43/43
// against the real x86 under Unicorn).  Evidence notes: docs/RE_GAMEPLAY.md.
//
// PROVENANCE
//   Parameters: registrars FUN_00190430 ("Score/...", 79 params) and
//     FUN_0017A0F0 ("Boost Bar", 5 params) walked from the decompile; values
//     resolved through the game's own VDB hash pipeline (key = "<param>
//     <group>/../Export/ValueDB/Score.cfg", table-CRC at 0x001AF250) against
//     the retail Data/vdb.xml.  The numbers below are the VDB-TUNED values
//     the shipped game plays with; compiled-in defaults are in comments.
//   Rules: FUN_0017A530 (award), FUN_0017A480 (drain/stop), FUN_0017A5B0
//     (engage), FUN_000273F0 (takedown boost + bar upgrade), FUN_00192D20
//     (category tiers), FUN_00196940 (air BP), FUN_00198E60/FUN_001994D0
//     (takedown commit + BP), FUN_0011ECF0 head (steer-away envelope),
//     FUN_00105340 (out-of-control authority).  Every formula here has a
//     green case in tools/validate_gameplay.py; rules that could not be
//     execution-verified are documented in docs/RE_GAMEPLAY.md as [S] and
//     are deliberately NOT in this header.
//
// The boost record lives at racecar+0x119C in the real game (score object =
// racecar+0x10D0, record = score+0xCC); the transmission's anchors
// obj+0x11C0 (boost start clock) and obj+0x11F1 (ramp-done byte) are
// record+0x24 / record+0x55 of this struct.

#ifndef BURNOUT3_GAMEPLAY_H
#define BURNOUT3_GAMEPLAY_H

// ---------------------------------------------------------------------------
// "Boost Bar" group (registrar FUN_0017A0F0, storage 0x3F72DC..0x3F7310).
// VDB-tuned values; compiled defaults in comments.
// ---------------------------------------------------------------------------
#define B3_MIN_BOOST_TIME      0.5f    // "Minimum Boost Time (seconds)"    (dflt 1.0)
#define B3_MIN_BOOST_RECOVERY  0.1f    // "Minimum Boost Recovery Time"     (dflt 0.5)
// Per bar tier 0..3 (tier raised by takedowns, FUN_000273F0):
static const float B3_BAR_SIZE[4] = {240.f, 360.f, 540.f, 720.f}; // dflt ..600,720
static const float B3_BAR_EARN_MULT[4] = {0.666667f, 1.0f, 1.5f, 2.0f};
                                                        // dflt {1,1.5,2.5,3}
#define B3_BOOST_RATE          36.0f   // "Boost Rate (Boost Units per Second)"

// ---------------------------------------------------------------------------
// "Score/Boost/*" awards in Boost units (registrar FUN_00190430).
// ---------------------------------------------------------------------------
#define B3_TAKEDOWN_BOOST      360.0f  // one full base bar        (dflt 720)
#define B3_NEARMISS_BOOST       15.0f  // per near miss            (dflt 60)
#define B3_NEARMISS_DIST         1.8f  // metres                   (dflt 2)
#define B3_NEARMISS_MIN_MPH     60.0f  // min speed                (dflt 30)
#define B3_NEARMISS_CHAIN_S      2.5f  // chain window seconds     (dflt 5)
#define B3_AIR_MIN_M             5.0f  // min air metres           (dflt 5)
#define B3_AIR_BOOST_PER_M       0.3f  // units per metre          (dflt 2)
#define B3_ONCOMING_MIN_MPH     50.0f  //                          (dflt 30)
#define B3_ONCOMING_MIN_M       40.0f  //                          (dflt 10)
#define B3_ONCOMING_PER_M        0.15f //                          (dflt 0.3)
#define B3_DRIFT_MIN_MPH        90.0f  //                          (dflt 30)
#define B3_DRIFT_MIN_M          20.0f  //                          (dflt 10)
#define B3_DRIFT_PER_M           0.1f  //                          (dflt 1)
#define B3_TAILGATE_PER_S       30.0f  //                          (dflt 15)
#define B3_GRIND_PER_S          30.0f  //                          (dflt 15)

// ---------------------------------------------------------------------------
// "Score/Burnout Points" (BP; integer params).  VDB-tuned / (default).
// ---------------------------------------------------------------------------
#define B3_BP_TAKEDOWN         150    // (dflt 1000)
#define B3_BP_REVENGE_BONUS    350    // (dflt 350)
#define B3_BP_AFTERTOUCH      1250    // (dflt 250)
#define B3_BP_NEAR_MISS          0    // per chain link x count  (dflt 20)
// escalation tables, indexed by (count - 2) clamped to the table:
static const int B3_BP_DOUBLE_TAKEDOWN[4] = {300, 500, 750, 1000};
static const int B3_BP_TAKEDOWN_SPREE[4] = {300, 500, 750, 1000};
#define B3_DOUBLE_TD_WINDOW_S    1.0f  // "Time window for ... double takedown"
#define B3_TD_SPREE_WINDOW_S    30.0f  // "Time window for ... takedown spree"
// Air distance categories: BP per tier, minima in metres (tier tracker
// FUN_00192D20 walks the minima top-down; tier never downgrades in-event).
static const int B3_BP_AIR_CAT[4] = {0, 10, 50, 100};      // dflt {100,250,500,1000}
static const float B3_AIR_CAT_MIN[4] = {1115.f, 10.f, 20.f, 35.f};

// ---------------------------------------------------------------------------
// "Score/Boost/Takedowns" detection windows (used by the scoring module's
// crash-attribution scan, FUN_00197040/FUN_00197430).
// ---------------------------------------------------------------------------
#define B3_MAX_CRASH_WAIT_S      2.0f  // slam -> victim crash window (dflt 1)
#define B3_RACECAR_CLEAR_WAIT_S  0.5f  // attacker must stay clear    (dflt 1)

// ---------------------------------------------------------------------------
// "Physics/Aggressive Driving Reaction" (config +0x1BC/+0x1C0/+0x1C4,
// live copies v+0x13E0/+0x13E4/+0x13E8 via FUN_00134710).
// ---------------------------------------------------------------------------
#define B3_STEER_AWAY_TIME_S     0.3f  // full-response phase
#define B3_TOTAL_OOC_TIME_S      1.0f  // total out-of-control time
#define B3_OOC_STEER_MAX_DEG    24.0f  // response override target
#define B3_OOC_AI_AUTHORITY      0.1f  // FUN_00105340 writes 0.1 (mode 2)

// ---------------------------------------------------------------------------
// Boost bar state -- mirrors the record at racecar+0x119C (constructor
// FUN_0017A3C0).  Field comments give the real offsets.
// ---------------------------------------------------------------------------
typedef struct {
    int   tier;        // +0x30  bar tier 0..3, raised per takedown
    float size;        // +0x34  current bar size  = B3_BAR_SIZE[tier]
    float meter;       // +0x38  boost units in the bar
    float earned;      // +0x3C  lifetime units earned (stat)
    float rate;        // +0x40  drain, units/second = B3_BOOST_RATE
    float min_units;   // +0x44  engage gate = rate * B3_MIN_BOOST_TIME
    float mult_base;   // +0x48  earning multiplier = B3_BAR_EARN_MULT[tier]
    float mult_bonus;  // +0x4C  event bonus multiplier (0 in normal race)
    float start_time;  // +0x24  clock when boost engaged (the transmission's
                       //        obj+0x11C0: elapsed = clock - start_time)
    float end_time;    // +0x28  clock when boost last stopped (-1 = never)
    int   boosting;    // +0x52
    int   fixed_burn;  // +0x53  AI minimum-burn commitment
    int   ramp_done;   // +0x55  = obj+0x11F1, consumed by the transmission
} B3BoostBar;

// FUN_0017A3C0 verbatim (reset to tier 0).
static inline void b3_boost_reset(B3BoostBar* b) {
    b->tier = 0;
    b->size = B3_BAR_SIZE[0];
    b->meter = 0.0f;
    b->earned = 0.0f;
    b->rate = B3_BOOST_RATE;
    b->mult_base = B3_BAR_EARN_MULT[0];
    b->mult_bonus = 0.0f;
    b->min_units = B3_MIN_BOOST_TIME * b->rate;
    b->start_time = -1.0f;
    b->end_time = -1.0f;
    b->boosting = 0;
    b->fixed_burn = 0;
    b->ramp_done = 0;
}

// FUN_0017A530 verbatim (validate_gameplay "boost award", 4/4):
//   amount = units * (mult_base + mult_bonus); earned += amount;
//   meter = min(meter + amount, size).
static inline void b3_boost_award(B3BoostBar* b, float units) {
    float amount = units * (b->mult_base + b->mult_bonus);
    b->earned += amount;
    b->meter += amount;
    if (b->meter > b->size) b->meter = b->size;
}

// FUN_0017A5B0 verbatim (validate_gameplay "boost engage", 5/5):
// engage iff not boosting, meter >= min_units, and past the recovery gate.
// Returns 1 when boost starts.
static inline int b3_boost_engage(B3BoostBar* b, float clock) {
    if (b->boosting) return 0;
    if (b->meter < b->min_units) return 0;
    if (!(b->end_time < 0.0f || b->end_time + B3_MIN_BOOST_RECOVERY <= clock))
        return 0;
    b->boosting = 1;
    b->fixed_burn = 0;
    b->start_time = clock;
    return 1;
}

// FUN_0017A480 head verbatim (validate_gameplay "meter update", 7/7):
// drain rate*dt while boosting; stop at empty (or, for an AI fixed burn,
// after B3_MIN_BOOST_TIME); the peg-full flags of the real record are not
// carried (crash-mode only).
static inline void b3_boost_update(B3BoostBar* b, float clock, float dt) {
    if (!b->boosting) return;
    b->meter -= b->rate * dt;
    int stop = 0;
    if (!b->fixed_burn) {
        if (b->meter <= 0.0f) { b->meter = 0.0f; stop = 1; }
    } else if (b->start_time + B3_MIN_BOOST_TIME <= clock) {
        if (b->meter < 0.0f) b->meter = 0.0f;
        stop = 1;
    }
    if (stop) {
        b->boosting = 0;
        b->fixed_burn = 0;
        b->end_time = clock;
    }
}

// Human release: retail burns only while the input bit (v+0x13FC & 4) is
// held -- releasing ends the burn, and the engage recovery gate
// (B3_MIN_BOOST_RECOVERY) then applies to the next press. AI fixed burns
// ignore release. [S: the release site is the input-bit consumer, not
// byte-traced; the drain head FUN_0017A480 above is verbatim/unchanged.]
static inline void b3_boost_release(B3BoostBar* b, float clock) {
    if (!b->boosting || b->fixed_burn) return;
    b->boosting = 0;
    b->end_time = clock;
}

// FUN_000273F0 verbatim (validate_gameplay "bar upgrade", 2/2): a takedown
// raises the bar tier (max 3), re-loads size/rate/multiplier, then awards
// B3_TAKEDOWN_BOOST units through the standard multiplier + clamp.
static inline void b3_boost_takedown(B3BoostBar* b) {
    if (b->tier < 3) {
        b->tier += 1;
        b->size = B3_BAR_SIZE[b->tier];
        b->rate = B3_BOOST_RATE;
        b->mult_base = B3_BAR_EARN_MULT[b->tier];
        b->min_units = b->rate * B3_MIN_BOOST_TIME;
    }
    b3_boost_award(b, B3_TAKEDOWN_BOOST);
}

// FUN_00192D20 verbatim (validate_gameplay "tier tracker", 5/5): highest
// tier whose minimum <= value; never downgrades within an event.
static inline int b3_category_tier(float value, const float* minima, int n,
                                   int tier) {
    if (tier < n - 1 && n != 0) {
        for (int i = n - 1; i >= 0; i--)
            if (minima[i] <= value) return i;
    }
    return tier;
}

// FUN_0011ECF0 head verbatim (validate_gameplay "steer-away", 5/5): steering
// response after being slammed at time t: full B3_OOC_STEER_MAX_DEG during
// the steer-away phase, then a linear decay back to base over the rest of
// the out-of-control time.  (The wall/spin event type scales the windows by
// 0.6 and the decay target by 0.8 -- pass scale06 = 1 for it.)
static inline float b3_steer_away_response(float base, float t_slam,
                                           float clock, int scale06) {
    float t_sa = B3_STEER_AWAY_TIME_S, t_ooc = B3_TOTAL_OOC_TIME_S;
    float target = B3_OOC_STEER_MAX_DEG;
    if (scale06) { t_sa *= 0.6f; t_ooc *= 0.6f; target *= 0.8f; }
    if (t_slam < 0.0f || clock > t_slam + t_ooc) return base;
    if (clock <= t_slam + t_sa) return B3_OOC_STEER_MAX_DEG;
    return base + (target - base)
                * (1.0f - (clock - (t_slam + t_sa)) / (t_ooc - t_sa));
}

// FUN_001994D0 (validate_gameplay "takedown commit", 3/3 incl. the BP chain
// executed end-to-end): base takedown BP plus escalation inside the double /
// spree windows.  State the caller keeps per car: window ends + counts.
typedef struct {
    int   takedowns;       // score+0x68
    int   bp;              // racecar+0x111C accumulated Burnout Points
    float dbl_window_end;  // sub+0x110 (-1 = closed)
    int   dbl_count;       // sub+0x114
    float spree_window_end;// sub+0x118
    int   spree_count;     // sub+0x11C
    float last_td_time;    // score+0x500
} B3TakedownScore;

static inline void b3_takedown_score_reset(B3TakedownScore* s) {
    s->takedowns = 0;
    s->bp = 0;
    s->dbl_window_end = -1.0f;   // -1 = closed, matches the real record
    s->dbl_count = 0;
    s->spree_window_end = -1.0f;
    s->spree_count = 0;
    s->last_td_time = -1.0f;
}

// FUN_00199080 verbatim (validate_gameplay "window expiry", 1/1): run once
// per frame; an expired window resets its chain count.
static inline void b3_takedown_frame(B3TakedownScore* s, float clock) {
    if (s->dbl_count != 0 && s->dbl_window_end <= clock) {
        s->dbl_count = 0;
        s->dbl_window_end = -1.0f;
    }
    if (s->spree_count != 0 && s->spree_window_end <= clock) {
        s->spree_count = 0;
        s->spree_window_end = -1.0f;
    }
}

static inline int b3_takedown_bp(B3TakedownScore* s, float clock,
                                 int revenge) {
    int bp = B3_BP_TAKEDOWN;
    s->takedowns += 1;
    s->last_td_time = clock;
    // double takedown: consecutive takedowns within 1 s of each other
    if (clock < s->dbl_window_end || s->dbl_window_end < 0.0f) {
        s->dbl_count += 1;
        s->dbl_window_end = clock + B3_DOUBLE_TD_WINDOW_S;
        if (s->dbl_count > 1) {
            int i = s->dbl_count - 2;
            if (i > 3) i = 3;
            bp += B3_BP_DOUBLE_TAKEDOWN[i];
        }
    }
    // takedown spree: takedowns within a rolling 30 s window
    if (clock < s->spree_window_end || s->spree_window_end < 0.0f) {
        s->spree_count += 1;
        s->spree_window_end = clock + B3_TD_SPREE_WINDOW_S;
        if (s->spree_count > 1) {
            int i = s->spree_count - 2;
            if (i > 3) i = 3;
            bp += B3_BP_TAKEDOWN_SPREE[i];
        }
    }
    if (revenge) bp += B3_BP_REVENGE_BONUS;
    s->bp += bp;
    return bp;
}

#endif // BURNOUT3_GAMEPLAY_H
