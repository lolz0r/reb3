/* The Burnout 3 opponent-AI driver control law -- see burnout3_ai.h for the
 * provenance header and the function map.
 *
 * EVERY numeric constant below is read out of the retail image (analyzed
 * burnout3.elf, .text = flat + 0x10000) and every formula is asserted against
 * the real x86 executed under Unicorn by tools/validate_ai.py.  Where the
 * compiler used a specific evaluation order that changes the float result
 * (normalize = multiply by 1/len, not divide; acos via pi/2 - atan2), this
 * file reproduces THAT order, not the mathematically equivalent one.
 */
#include "burnout3_ai.h"

#include <math.h>
#include <string.h>

/* ---- compiled-in constants (address = where they live in the image) ---- */
#define B3AI_RAD2DEG      57.295780f   /* 0x00395D78 */
#define B3AI_HALF_PI       1.5707964f  /* 0x0039A25C */
#define B3AI_MPS2MPH       2.2369363f  /* 0x0038994C  (the TRUE ratio here) */
#define B3AI_RADS2RPM      9.549296f   /* 0x003B17C4 */
#define B3AI_SLEW_WIND     2.4f        /* 0x003895AC -> DAT_00754B28 */
#define B3AI_SLEW_UNWIND   8.1f        /* 0x003A2AB0 -> DAT_00754B24 */
#define B3AI_YAW_GAIN_LO   0.01f       /* 0x0041A510 */
#define B3AI_YAW_GAIN_HI   0.04f       /* 0x0041A50C */
#define B3AI_EPS2          2.3283064e-10f
#define B3AI_BOOST_SCALE   1.45f       /* 0x003A2AB4 */
#define B3AI_STEER_CUT     0.125f      /* 0x003B1728 */
#define B3AI_LOCK_PER_DEG (-0.0055555557f)  /* compiled -1/180 */
#define B3AI_BRAKE_EXCESS 13.4112f     /* 0x003B1A5C = exactly 30 mph */
#define B3AI_STOPFLAG_MS   8.9408f     /* 0x003B1A60 = exactly 20 mph */
#define B3AI_STUCK_MPH     5.0f        /* 0x003B1694 */
#define B3AI_CRASH_GATE    0.5f        /* 0x003B1684 */
#define B3AI_REVERSE_S     2.0f        /* 0x003B1688 */
#define B3AI_ARM_S         1.0f        /* 0x003B168C */
#define B3AI_SHIFT_KICK    0.35f       /* 0x0039B2B0 */
#define B3AI_STOP_MS       0.1f        /* 0x003A69C4 */
#define B3AI_DITHER_HALF   0.5f        /* 0x0041A4AC */
#define B3AI_CATCHUP_TOL   0.0010309278732165694f  /* 0x003B1E04 */
/* DAT_005A9770, the traffic-class AI target-speed cap: 22.352 m/s = exactly
 * 50 mph, installed by the static-init snippet at 0x002C5E80 from .data
 * 0x003B2110 (same copy-snippet pattern as the 13.4112 brake threshold). */
#define B3AI_TRAFFIC_CAP  22.352f

/* AI config defaults: compiled-in (FUN_0016AFD0) overridden by the retail
 * Data/vdb.xml column -- docs/RE_AI.md section 1, all [C] hash-mirrored. */
B3AiParams b3_ai_params = {
    /* oor_speed_dec_rate  */ 10.0f,
    /* oor_max_dir_deg     */ 1.0f,
    /* angle_min_speed_deg */ 90.0f,
    /* top_speed_mps       */ 88.0f,
    /* min_speed_mps       */ 20.0f,
    /* car_at_weight       */ 0.9f,
    /* drift_start_deg     */ 20.0f,
    /* max_lock_deg        */ 10.0f,
    /* drift_max_lock_deg  */ 50.0f,
    /* avoid 10/20/30 m    */ 26.2f, 40.0f, 60.0f,
    /* brake_dist_factor   */ 0.6f,
};

void b3_ai_init(void) {
    /* The table above already carries the retail VDB values; this entry point
     * exists so the harness has one place to re-point it at a per-track or
     * per-difficulty column later (FUN_00172870's 0x98-byte per-slot record). */
}

void b3_ai_state_init(B3AiState* s) {
    memset(s, 0, sizeof *s);
    s->des_dir[2] = 1.0f;
    s->des_dir_n[2] = 1.0f;
    s->time_to_target = 1.0f;
    s->t2t_snap = 1.0f;
    s->max_speed = b3_ai_params.top_speed_mps;
    s->speed_cap = b3_ai_params.top_speed_mps;
    s->stuck_arm = -1.0f;
    s->reverse_timer = -1.0f;
    s->brake_hold = -1.0f;
    s->steer_authority = 1.0f;
    s->drift_state = 4;
}

/* --- vector helpers, matching the game's SSE helpers exactly ------------ */

/* FUN_00011640: inv = 1/sqrt(x^2+y^2+z^2) computed as sqrtss then divss into
 * a broadcast 1.0, then a 4-wide multiply.  The multiply-by-reciprocal (not
 * a per-component divide) is what the retail code does. */
static void b3_norm3(float* v) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float inv = 1.0f / len;
    v[0] *= inv; v[1] *= inv; v[2] *= inv; v[3] *= inv;
}

/* FUN_0002C0D0: same, but returns the length. */
static float b3_norm3_len(float* v) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float inv = 1.0f / len;
    v[0] *= inv; v[1] *= inv; v[2] *= inv; v[3] *= inv;
    return len;
}

static float b3_dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float b3_clamp(float x, float lo, float hi) {
    if (x < lo) x = lo;          /* maxss */
    if (x > hi) x = hi;          /* minss */
    return x;
}

/* the compiled acos: (pi/2 - fpatan(d, sqrt(1-d*d))) [* rad2deg] */
static float b3_acos_c(float d) {
    return B3AI_HALF_PI - atan2f(d, sqrtf(1.0f - d * d));
}

/* ======================================================================== */
/* FUN_0016AE20 -- the arbitrator's "target follower wins" leg.             */
/*   AI+0x770 = target_point - racecar_pos, normalized (4-wide)             */
/*   AI+0x784 = |d| / speed   when speed > 1.0, else |d|                    */
/* ======================================================================== */
float b3_ai_commit_target(B3AiState* s, const B3AiCar* c,
                          const float target_point[3]) {
    float len;
    s->des_dir[0] = target_point[0] - c->pos[0];
    s->des_dir[1] = target_point[1] - c->pos[1];
    s->des_dir[2] = target_point[2] - c->pos[2];
    s->des_dir[3] = 0.0f;          /* row3.w of the matrix minus 1.0 -> 0 */
    len = b3_norm3_len(s->des_dir);
    if (c->speed_ms > 1.0f)
        s->time_to_target = len / c->speed_ms;
    else
        s->time_to_target = len;
    return len;
}

/* FUN_00171A10 head: snapshot + normalize + latch the time-to-target. */
void b3_ai_frame_snapshot(B3AiState* s) {
    s->des_dir_n[0] = s->des_dir[0];
    s->des_dir_n[1] = s->des_dir[1];
    s->des_dir_n[2] = s->des_dir[2];
    s->des_dir_n[3] = s->des_dir[3];
    s->t2t_snap = s->time_to_target;
    b3_norm3(s->des_dir_n);
}

/* ======================================================================== */
/* FUN_00171E30 -- the target steering angle (racecar+0x23C0).              */
/* ======================================================================== */
void b3_ai_target_angle(B3AiState* s, const B3AiCar* c) {
    float ang, prev, delta, wind, unwind;

    s->steer_err = 0.0f;

    /* gate: the LSDM/drift path (byte v+0x1550 set AND (below the LSDM speed
     * limit OR already drifting)) uses a yaw-RATE demand instead. */
    if (c->lsdm_active &&
        (c->speed_ms * B3AI_MPS2MPH <= c->lsdm_limit_mph ||
         c->drift_state == 2 || c->drift_state == 1)) {
        float a[4], cross[4], cxa[4], d, rate, cur, gain, sp;
        a[0] = c->car_at[0]; a[1] = c->car_at[1]; a[2] = c->car_at[2];
        a[3] = 0.0f;
        if (s->target_mode != 1) {
            a[0] += c->veh_fwd[0];
            a[1] += c->veh_fwd[1];
            a[2] += c->veh_fwd[2];
        }
        if (a[0] * a[0] + a[1] * a[1] + a[2] * a[2] >= B3AI_EPS2) {
            b3_norm3(a);
        } else {
            a[0] = c->veh_fwd[0]; a[1] = c->veh_fwd[1]; a[2] = c->veh_fwd[2];
        }
        d = b3_clamp(b3_dot3(s->des_dir_n, a), -1.0f, 1.0f);
        ang = b3_acos_c(d);                       /* radians here */

        cross[0] = a[1] * c->veh_right[2] - a[2] * c->veh_right[1];
        cross[1] = a[2] * c->veh_right[0] - a[0] * c->veh_right[2];
        cross[2] = a[0] * c->veh_right[1] - a[1] * c->veh_right[0];
        if (cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]
            < B3AI_EPS2) {
            cross[0] = a[1] * c->veh_fwd[2] - a[2] * c->veh_fwd[1];
            cross[1] = a[2] * c->veh_fwd[0] - a[0] * c->veh_fwd[2];
            cross[2] = a[0] * c->veh_fwd[1] - a[1] * c->veh_fwd[0];
        }
        cross[3] = 0.0f;
        b3_norm3(cross);
        cxa[0] = cross[1] * a[2] - cross[2] * a[1];
        cxa[1] = cross[2] * a[0] - cross[0] * a[2];
        cxa[2] = cross[0] * a[1] - cross[1] * a[0];
        if (b3_dot3(cxa, s->des_dir_n) < 0.0f) ang = 0.0f - ang;

        rate = ang / s->t2t_snap;                 /* demanded yaw rate */
        cur = c->yaw_rate;
        s->steer_err = cur - rate;
        /* gain 0.04 while the error UNWINDS the yaw, 0.01 while it winds up
         * (DAT_0041A50C / DAT_0041A510); the opposite-sign multiplier
         * DAT_0041A508 is 1.0 in retail, i.e. a shipped no-op. */
        if (cur <= 0.0f)
            gain = (rate <= cur) ? B3AI_YAW_GAIN_LO : B3AI_YAW_GAIN_HI;
        else
            gain = (rate <= cur) ? B3AI_YAW_GAIN_HI : B3AI_YAW_GAIN_LO;
        sp = c->speed_ms;
        s->target_angle = (rate - cur) * sp * sp * gain;
        if (s->target_angle > 90.0f) s->target_angle = 90.0f;
        if (s->target_angle < -90.0f) s->target_angle = -90.0f;
        s->prev_angle = 0.0f;
        return;
    }

    /* --- normal racing path --- */
    {
        float k = b3_ai_params.car_at_weight;
        float one_minus = 1.0f - k;
        float blend[4], d;
        blend[0] = c->car_at[0] * one_minus + c->fwd[0] * k;
        blend[1] = c->car_at[1] * one_minus + c->fwd[1] * k;
        blend[2] = c->car_at[2] * one_minus + c->fwd[2] * k;
        blend[3] = 0.0f;
        b3_norm3(blend);
        d = b3_dot3(s->des_dir_n, blend);
        d = b3_clamp(d, -1.0f, 1.0f);
        ang = b3_acos_c(d) * B3AI_RAD2DEG;
        s->target_angle = ang;
        if (0.0f > b3_dot3(c->right, s->des_dir_n))
            s->target_angle = 0.0f - ang;
    }

    /* asymmetric per-frame slew limiter (DAT_00754B28 / DAT_00754B24, lazily
     * initialised to 2.4 and 8.1): winding the wheel AWAY from centre is
     * limited to 2.4 deg/frame, unwinding toward centre to 8.1. */
    prev = s->prev_angle;
    delta = s->target_angle - prev;
    wind = B3AI_SLEW_WIND;
    unwind = B3AI_SLEW_UNWIND;
    if (prev > 0.0f) {
        if (delta > wind) s->target_angle = wind + prev;
        if (0.0f - unwind > delta) s->target_angle = prev - unwind;
    } else {
        if (delta > unwind) s->target_angle = unwind + prev;
        if (0.0f - wind > delta) s->target_angle = prev - wind;
    }
    s->prev_angle = s->target_angle;
}

/* ======================================================================== */
/* FUN_00172E80 -- corner speed from the demanded steering angle.           */
/*   t   = max(0, 1 - |angle| / "Angle you want min spd at")                */
/*   s   = drifting ? S : t*S                                               */
/*   spd = "Min speed mps" + s   [ *1.45 if the boost flag is set ]         */
/*   return min(spd, S, AI+0xA08)                                           */
/* ======================================================================== */
float b3_ai_corner_speed(const B3AiState* s, const B3AiCar* c,
                         float max_speed) {
    float a = fabsf(s->target_angle);
    float u = a / b3_ai_params.angle_min_speed_deg;
    float t = 1.0f - u;
    float lerped, spd;
    if (0.0f > t) t = 0.0f;                       /* maxss(0, 1-u) */
    if (c->drift_state == 2 || c->drift_state == 1)
        lerped = max_speed;
    else
        lerped = t * max_speed;
    spd = b3_ai_params.min_speed_mps + lerped;
    if (s->boost_scale_flag) spd *= B3AI_BOOST_SCALE;
    if (spd > max_speed) spd = max_speed;
    if (spd > s->speed_cap) spd = s->speed_cap;
    return spd;
}

/* ======================================================================== */
/* FUN_001724F0 -- the target speed (racecar+0x23C4).                       */
/* ======================================================================== */
void b3_ai_target_speed(B3AiState* s, const B3AiCar* c, float catchup_bonus) {
    float spd = b3_ai_corner_speed(s, c, s->max_speed);
    float err, f;
    s->corner_speed = spd;
    s->target_speed = spd;

    if (c->traffic_class) {
        /* traffic-class cars are pinned to DAT_005A9770 = 50 mph */
        float cap = B3AI_TRAFFIC_CAP;
        s->target_speed = (spd < cap) ? spd : cap;
    } else if (c->race_mode == 1) {
        /* FUN_00172FA0 (aggression speed matching) then FUN_001734C0
         * (catch-up).  Both are supplied by the caller as a delta because
         * their state machines are [S] -- see docs/RE_AI.md section 11. */
        s->target_speed = spd + catchup_bonus;
        if (c->free_speed_floor) {
            if (s->target_speed < 0.0f) s->target_speed = 0.0f;
        } else if (s->target_speed < b3_ai_params.min_speed_mps) {
            s->target_speed = b3_ai_params.min_speed_mps;
        }
    }

    /* steering-error speed cut: |AI+0x9D4| above 1.0 scales the target down
     * by clamp(|err| * 0.125, 0, 1).  AI+0x9D4 is only non-zero on the
     * drift/LSDM path, so this is the drift-recovery brake. */
    err = fabsf(s->steer_err);
    if (err > 1.0f) {
        f = b3_clamp(err * B3AI_STEER_CUT, 0.0f, 1.0f);
        s->target_speed = (1.0f - f) * s->target_speed;
    }
}

/* ======================================================================== */
/* FUN_00104CA0 -- brake helper.  In reverse (gear -1) the "brake" goes to  */
/* the THROTTLE input; forward it goes to the brake and throttle is zeroed. */
/* ======================================================================== */
void b3_ai_brake(B3AiInputs* in, B3AiState* s, const B3AiCar* c,
                 float amount) {
    if (s->brake_hold != -1.0f && s->brake_hold <= c->clock)
        s->brake_hold = -1.0f;
    if (c->gear == -1) {
        in->brake = 0.0f;
        in->throttle = amount;
        in->throttle_raw = 0.0f;
    } else if (c->gear != 0) {
        in->brake = amount;
        in->throttle = 0.0f;
        in->throttle_raw = 0.0f;
    }
}

/* ======================================================================== */
/* FUN_00105340 -- the AI racer driver.                                     */
/* ======================================================================== */
void b3_ai_drive(B3AiState* s, const B3AiCar* c, B3AiInputs* in,
                 float dt, float reverse_aim_dot) {
    int rev_high;        /* [esp+7]: engine at/above the change-up point   */
    float lock, st, d;

    memset(in, 0, sizeof *in);
    in->gear_request = 0;

    rev_high = (c->engine_rpm >= c->change_up_rpm);

    /* ---- 1. active reverse burst ------------------------------------- */
    if (s->reverse_timer != -1.0f) {
        s->reverse_timer -= dt;
        if (s->reverse_timer > 0.0f) {
            in->throttle_raw = 0.0f;
            in->throttle = 0.0f;
            in->brake = 1.0f;
            /* steer full lock toward the aim side while backing out */
            if (reverse_aim_dot < -0.1f)      in->steer = -1.0f;
            else if (reverse_aim_dot > 0.1f)  in->steer = 1.0f;
            else                              in->steer = 0.0f;
            s->prev_steer = in->steer;
            return;
        }
        s->reverse_timer = 0.0f;
        if (c->speed_ms < B3AI_STOP_MS) {
            s->reverse_timer = -1.0f;
            in->gear_request = 1;               /* back into forward */
            in->shift_kick = 1;
            in->throttle_raw = 0.0f;
            in->throttle = 0.0f;
            in->brake = 0.0f;
            return;
        }
        b3_ai_brake(in, s, c, 1.0f);
        in->stop_flag = 1;
        return;
    }

    /* ---- 2. stuck detector: below 5 mph with the crash timer clear ----- */
    if (c->crash_timer >= B3AI_CRASH_GATE || s->target_speed <= 0.0f ||
        c->speed_ms * B3AI_MPS2MPH >= B3AI_STUCK_MPH) {
        s->stuck_arm = -1.0f;
    } else if (s->stuck_arm == -1.0f) {
        s->stuck_arm = B3AI_ARM_S;
    } else {
        s->stuck_arm -= dt;
        if (s->stuck_arm <= 0.0f) {
            s->reverse_timer = B3AI_REVERSE_S;
            s->stuck_arm = -1.0f;
            in->gear_request = -1;
            in->shift_kick = 1;
            in->throttle_raw = 0.0f;
            in->throttle = 0.0f;
            in->brake = 0.0f;
            in->steer = 0.0f;
            return;
        }
    }

    /* ---- 3. steering: target angle -> input, MaxLock/180 --------------- */
    if (c->attack_active && (c->attack_left || c->attack_right))
        lock = b3_ai_params.drift_max_lock_deg;
    else
        lock = b3_ai_params.max_lock_deg;
    st = b3_clamp(s->target_angle * lock * B3AI_LOCK_PER_DEG, -1.0f, 1.0f);

    /* out-of-control envelope: inside the slam+aggressor window the AI's
     * steering authority collapses (mode 2 -> 0.1, otherwise 0.05) and in
     * mode 0 the driver throws full OPPOSITE lock. */
    if (c->ooc_window) {
        if (c->ooc_mode == 2) {
            s->steer_authority = 0.1f;
        } else {
            s->steer_authority = 0.05f;
            if (c->ooc_mode == 0) {
                if (c->ooc_countersteer)
                    st = 0.0f - ((st < 0.0f) ? -1.0f : 1.0f);
                else
                    st = s->prev_steer;      /* hold the previous input */
            } else if (c->ooc_mode == 1) {
                st = s->prev_steer;
            }
        }
    }
    in->steer = st;
    s->prev_steer = st;

    /* ---- 4. speed band ------------------------------------------------ */
    if (c->attack_active) {
        if (c->attack_left) {
            if (c->attack_commit) { s->drift_state = 2; in->steer = 1.0f; }
        } else if (c->attack_right && c->attack_commit) {
            s->drift_state = 1; in->steer = -1.0f;
        }
        d = s->target_speed - c->speed_ms;
        if (d > 1.0f) {
            in->brake = 0.0f;
            in->throttle_raw = 1.0f;
            in->throttle = 1.0f;
        } else if (d >= 0.0f - B3AI_BRAKE_EXCESS) {
            in->brake = 0.0f;
            if (rev_high) {
                in->throttle_raw = 1.0f;
                in->throttle = 1.0f;
            } else {
                in->throttle_raw = 0.0f;
                in->throttle = 0.0f;
            }
        } else {
            b3_ai_brake(in, s, c, 1.0f);
            if (s->target_speed < B3AI_STOPFLAG_MS) in->stop_flag = 1;
            goto boost_tail;
        }
    } else {
        if (s->drift_state != 2 && s->drift_state != 1) s->drift_state = 4;
        d = s->target_speed - c->speed_ms;
        if (d > 1.0f) {
            in->throttle_raw = 1.0f;
            if (!c->traffic_class) {
                in->brake = 0.0f;
                in->throttle = 1.0f;
            }
        } else if (!c->brake_suppressed && d < 0.0f - B3AI_BRAKE_EXCESS) {
            b3_ai_brake(in, s, c, 1.0f);
            if (s->target_speed < B3AI_STOPFLAG_MS) in->stop_flag = 1;
            goto boost_tail;
        } else if (rev_high) {
            in->brake = 0.0f;
            in->throttle_raw = 1.0f;
            in->throttle = 1.0f;
        } else {
            in->throttle_raw = 0.0f;
            in->throttle = 0.0f;
            in->brake = 0.0f;
        }
    }

    /* ---- 5. launch throttle dither ------------------------------------ */
    {
        float duty = 1.0f;
        if (s->dither_deadline > c->clock)
            duty = 1.0f - (s->dither_deadline - c->clock) * B3AI_DITHER_HALF;
        if (in->throttle != s->prev_throttle) {
            if (in->throttle == 1.0f && c->speed_ms < B3AI_BRAKE_EXCESS)
                s->dither_deadline = -1.0f;
            else
                s->dither_deadline = duty * B3AI_REVERSE_S + c->clock;
            duty = 1.0f - duty;
        }
        s->prev_throttle = in->throttle;
        if (in->throttle == 1.0f) {
            in->throttle = duty;
            in->throttle_raw = duty;
        } else {
            in->throttle = 1.0f - duty;
            in->throttle_raw = 1.0f - duty;
        }
    }

    /* ---- 6. boost request -------------------------------------------- */
    if (c->wants_boost && c->gear > 0 && !c->boosting)
        in->engage_boost = 1;

boost_tail:
    /* the min-burn latch racecar+0x11EF is only set on the paths that did NOT
     * just call the engage gate (the engage branch jumps past it) */
    if (!in->engage_boost && c->boosting && !c->boost_ramp_done)
        in->commit_boost = 1;
    if (c->boosting) {
        in->bits = 4;                    /* the transmission's boost anchor */
        in->brake = 0.0f;
    } else {
        in->bits = 0;
    }
}

/* ======================================================================== */
/* Whole chain, one call.                                                   */
/* ======================================================================== */
void b3_ai_update(B3AiState* s, const B3AiCar* c, B3AiInputs* in,
                  const float target_point[3], float arbitrated_max_speed,
                  float catchup_bonus, float dt) {
    /* FUN_00173690: the hard cap is Top speed mps while racing */
    s->speed_cap = b3_ai_params.top_speed_mps;
    b3_ai_commit_target(s, c, target_point);
    s->max_speed = (arbitrated_max_speed < s->speed_cap)
                 ? arbitrated_max_speed : s->speed_cap;
    b3_ai_frame_snapshot(s);
    b3_ai_target_angle(s, c);
    b3_ai_target_speed(s, c, catchup_bonus);
    b3_ai_drive(s, c, in, dt, b3_dot3(s->des_dir_n, c->right));
}

/* ======================================================================== */
/* 0x00171078 -- the out-of-range mover's speed governor.                   */
/* ======================================================================== */
float b3_ai_oor_governor(float speed_ms, float speed_mph,
                         float target_speed_ms) {
    float d = speed_ms - target_speed_ms;
    float up = speed_mph * B3AI_CATCHUP_TOL;
    float band = b3_ai_params.oor_speed_dec_rate;
    if (d > band) return speed_ms - band;        /* too fast: step down */
    if (0.0f - up > d) return speed_ms + up;     /* too slow: step up   */
    return target_speed_ms;                      /* inside band: snap   */
}

/* ======================================================================== */
/* FUN_00105150 -- the reduced traffic driver.                              */
/* ======================================================================== */
void b3_ai_traffic_drive(const B3AiCar* c, B3AiInputs* in,
                         float target_angle_deg, float target_speed_ms) {
    float d;
    memset(in, 0, sizeof *in);
    in->steer = b3_clamp(target_angle_deg * b3_ai_params.max_lock_deg
                         * B3AI_LOCK_PER_DEG, -1.0f, 1.0f);
    d = target_speed_ms - c->speed_ms;
    if (d > 1.0f) {
        in->throttle = 1.0f;
        in->throttle_raw = 1.0f;
    } else if (d < -2.2352f) {         /* 5 mph excess */
        in->brake = 1.0f;
    }
}

/* AI/Avoidance close-range speed caps [C params]. */
float b3_ai_avoid_speed_cap(float speed, float dist_ahead_m) {
    if (dist_ahead_m < 10.0f && speed > b3_ai_params.avoid_speed_10m)
        return b3_ai_params.avoid_speed_10m;
    if (dist_ahead_m < 20.0f && speed > b3_ai_params.avoid_speed_20m)
        return b3_ai_params.avoid_speed_20m;
    if (dist_ahead_m < 30.0f && speed > b3_ai_params.avoid_speed_30m)
        return b3_ai_params.avoid_speed_30m;
    return speed;
}

/* =========================================================================
 * THE AGGRESSION (ATTACK / SLAM) STATE MACHINE -- FUN_00169540 family.
 * docs/RE_AI.md section 14.  Object: AI+0x170 == racecar+0x1B70.
 *
 * Constants below are the compiled immediates, with their addresses:
 *   0x003A2D50 2.5   lateral above which the slam skips the steer-out
 *   0x00384A80 0.15  steer-out lead time (x own speed)
 *   0x003A69C4 0.1   slam lead time / the "aggression x max_wait < 0.1" test
 *   0x003B1694 5.0   speed-match overshoot in the FUN_00172FA0 state-4 leg
 *   0x003A7F34 10.0  |lateral| ceiling for can-slam; FUN_00172FA0 K2
 *   0x003A7950 20.0  |speed diff| ceiling for block range
 *   0x003B16B8 50.0  target-pick lateral ceiling
 *   0x003B16B4 15.0  block-aim lead base/scale
 *   0x003B1884 40.0 / 0x003B17E8 60.0 / 0x003A2928 100.0  slam-speed gates
 *   0x003B19D0 1.1   width multiplier in the slam-speed gate
 *   0x003B188C 1e-4  block-range epsilon
 *   0x003B1698 3.0 / 0x003B1688 2.0 / 0x003A795C 25.0 / 0x003A69BC 0.05
 * ========================================================================= */
#define B3AG_SKIP_STEEROUT  2.5f        /* 0x003A2D50 */
#define B3AG_STEEROUT_LEAD  0.15f       /* 0x00384A80 */
#define B3AG_SLAM_LEAD      0.1f        /* 0x003A69C4 */
#define B3AG_MATCH_BONUS    5.0f        /* 0x003B1694 */
#define B3AG_TEN            10.0f       /* 0x003A7F34 */
#define B3AG_TWENTY         20.0f       /* 0x003A7950 */
#define B3AG_PICK_LATERAL   50.0f       /* 0x003B16B8 */
#define B3AG_BLOCK_LEAD     15.0f       /* 0x003B16B4 */
#define B3AG_MPH40          40.0f       /* 0x003B1884 */
#define B3AG_MPH60          60.0f       /* 0x003B17E8 */
#define B3AG_MPH100         100.0f      /* 0x003A2928 */
#define B3AG_WIDTH_MUL      1.1f        /* 0x003B19D0 */
#define B3AG_EPS            1.0e-4f     /* 0x003B188C */
#define B3AG_OOC_GRACE      3.0f        /* 0x003B1698 */
#define B3AG_AIM_GRACE      2.0f        /* 0x003B1688 */
#define B3AG_BLIND_S        25.0f       /* 0x003A795C */
#define B3AG_BLIND_STEP     0.05f       /* 0x003A69BC */
#define B3AG_RETRY_S        1.0f        /* 0x003B168C */
#define B3AG_HALF           0.5f        /* 0x003B1684 */

/* AI/Target +0x0A0 == 0x0047A1E0 (the last of the five bindings onto that
 * slot); FUN_00171BE0 tests |aim - pos| / speed against it. */
float b3_ai_drift_apex_time = 1.0f;

B3AiAggroParams b3_ai_aggro_params = {
    /* min_aggression  */ 0.002f,
    /* min_wait_s      */ 0.0f,
    /* max_wait_s      */ 3.0f,
    /* dist_ahead_m    */ 40.0f,
    /* dist_behind_m   */ 150.0f,
    /* min_target_mph  */ 75.0f,
    /* slow_factor     */ 0.9f,
    /* boost_dist_m    */ 15.0f,
    /* boost_aggro_m   */ -22.5f,
    /* start_delay_s   */ 3.0f,
    /* immunity_s      */ 3.0f,
    /* block_min_s     */ 3.0f,
    /* block_max_s     */ 15.0f,
    /* block_dist_m    */ 15.0f,
    /* separation_m    */ 3.0f,
    /* position_time_s */ 30.0f,
    /* ahead_gap_m     */ 3.5f,
    /* speed_diff_mph  */ 50.0f,
    /* steer_out_m     */ 5.0f,
    /* steer_out_s     */ 0.5f,
    /* slam_s          */ 0.75f,
    /* max_cos_off_lane*/ 0.8f,
    /* commit_s        */ 0.075f,
    /* sticky_dist_m   */ 10.0f,
    /* sticky_mph      */ 40.0f,
    /* close_match_m   */ 10.0f,
};

/* the compiled sign extract `(bits & 0xBF800000) | 0x3F800000` -- note it
 * yields +1.0 for +0.0 and for a zero-initialised field. */
static float b3_sign1(float x) {
    union { float f; unsigned int u; } v;
    v.f = x;
    v.u = (v.u & 0xBF800000u) | 0x3F800000u;
    return v.f;
}

static float b3_len3(const float* v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* FUN_001716D0: |dot(own.pos - targ.pos, normalize(flatten(own.right)))|.
 * Both the difference and the basis vector have their Y lane zeroed. */
static float b3_agg_lateral(const B3AiAggroCar* own, const B3AiAggroCar* tg) {
    float d[3], r[3], l2;
    d[0] = own->pos[0] - tg->pos[0];
    d[1] = 0.0f;
    d[2] = own->pos[2] - tg->pos[2];
    r[0] = own->right[0]; r[1] = 0.0f; r[2] = own->right[2];
    l2 = r[0] * r[0] + r[2] * r[2];
    if (l2 > B3AI_EPS2) {
        float inv = 1.0f / sqrtf(l2);
        r[0] *= inv; r[2] *= inv;
    } else {
        r[0] = own->right[0]; r[1] = own->right[1]; r[2] = own->right[2];
    }
    return fabsf(d[0] * r[0] + d[1] * r[1] + d[2] * r[2]);
}

/* FUN_001717B0: -dot(own.pos - targ.pos, normalize(flatten(own.road_dir)))
 * == the signed longitudinal gap, POSITIVE when the target is ahead. */
static float b3_agg_longit(const B3AiAggroCar* own, const B3AiAggroCar* tg) {
    float d[3], a[3], l2;
    d[0] = own->pos[0] - tg->pos[0];
    d[1] = 0.0f;
    d[2] = own->pos[2] - tg->pos[2];
    a[0] = own->road_dir[0]; a[1] = 0.0f; a[2] = own->road_dir[2];
    l2 = a[0] * a[0] + a[2] * a[2];
    if (l2 > B3AI_EPS2) {
        float inv = 1.0f / sqrtf(l2);
        a[0] *= inv; a[2] *= inv;
    } else {
        a[0] = own->fwd[0]; a[1] = own->fwd[1]; a[2] = own->fwd[2];
    }
    return 0.0f - (d[0] * a[0] + d[1] * a[1] + d[2] * a[2]);
}

static float b3_mph(float ms) { return ms * B3AI_MPS2MPH; }

static const B3AiAggroCar* b3_agg_car(const B3AiAggroWorld* w, int i) {
    return (i >= 0 && i < w->ncars) ? &w->cars[i] : 0;
}

/* --- FUN_0016A360: may the aggression aim override the racing line? ----- */
static int b3_agg_aim_ok(const B3AiAggroCar* own) {
    if (!own->steer_ok) return 0;
    if (!(0.0f > own->ooc_time)) {
        if (!(own->race_time > own->ooc_time + B3AG_AIM_GRACE)) return 0;
    }
    if (own->node_open) return 1;
    return own->drift_zone == 0;
}

/* --- FUN_00169D70: can we slam right now?  -> aggro+0x40 ---------------- */
static int b3_agg_can_slam(const B3AiAggro* a, const B3AiAggroCar* own,
                           const B3AiAggroCar* tg) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    float tmph, omph, lg, cosa;
    if (!tg) return 0;
    tmph = b3_mph(tg->speed_ms);
    if (P->min_target_mph > tmph) return 0;
    omph = b3_mph(own->speed_ms);
    if (tmph - omph > P->speed_diff_mph) return 0;
    lg = a->longitudinal;
    if (0.0f > lg) {                          /* the target is BEHIND us */
        if (!(own->car_length * B3AG_HALF > 0.0f - lg)) return 0;
    } else {                                  /* the target is AHEAD */
        if (!(P->ahead_gap_m > lg)) return 0;
    }
    cosa = own->road_dir[0] * own->fwd[0] + own->road_dir[1] * own->fwd[1]
         + own->road_dir[2] * own->fwd[2];
    if (P->max_cos_off_lane > cosa) return 0;
    return !(a->lateral > B3AG_TEN);
}

/* --- FUN_0016A3E0: are we the right distance AHEAD to block?  -> +0x41 -- */
static int b3_agg_block_range(const B3AiAggro* a, const B3AiAggroCar* own,
                              const B3AiAggroCar* tg) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    float dm, gap, ahead;
    (void)a;
    if (!tg) return 0;
    dm = fabsf(b3_mph(tg->speed_ms) - b3_mph(own->speed_ms));
    if (dm > B3AG_TWENTY) return 0;
    gap = tg->track_dist - own->track_dist;
    ahead = 0.0f - gap;                       /* + when WE are ahead */
    if (own->car_length + B3AG_EPS > ahead) return 0;
    if (ahead > own->car_length + P->block_dist_m) return 0;
    return own->node_open != 0;
}

/* --- FUN_0016A620: hold the target's speed exactly?  -> aggro+0x42 ------ */
static int b3_agg_slam_speed(const B3AiAggro* a, const B3AiAggroCar* own,
                             const B3AiAggroCar* tg) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    float win, gap, y;
    if (!tg) return 0;
    if (own->no_slam_speed) return 0;
    if (!(0.0f > own->ooc_time)) {
        if (!(own->race_time > own->ooc_time + B3AG_OOC_GRACE)) return 0;
    }
    win = (own->mode2450 == 1) ? B3AG_MPH40 : P->sticky_dist_m;
    win = (win - B3AG_OOC_GRACE) * own->aggression + B3AG_OOC_GRACE;
    gap = fabsf(own->track_dist - tg->track_dist);
    if (gap > win) return 0;
    if (!a->slam_speed && own->mode2450 != 1) {
        if (fabsf(b3_mph(own->speed_ms) - b3_mph(tg->speed_ms)) > P->sticky_mph)
            return 0;
    }
    y = (own->mode2450 == 1) ? B3AG_MPH60 : B3AG_MPH100;
    if (y > b3_mph(tg->speed_ms)) return 0;
    if (own->mode2450 == 0 && tg->boosting) return 0;
    return !(own->car_width * B3AG_WIDTH_MUL > a->lateral);
}

/* --- FUN_0016A7D0: the per-frame measurements ---------------------------*/
static void b3_agg_measure(B3AiAggro* a, const B3AiAggroWorld* w, int self) {
    const B3AiAggroCar* own = &w->cars[self];
    const B3AiAggroCar* tg = b3_agg_car(w, a->target);
    if (!tg) {
        a->lateral = 0.0f;
        a->longitudinal = 0.0f;
        a->can_slam = a->block_range = a->want_slam_speed = 0;
        return;
    }
    a->lateral = b3_agg_lateral(own, tg);
    a->longitudinal = b3_agg_longit(own, tg);
    a->can_slam = (unsigned char)b3_agg_can_slam(a, own, tg);
    a->block_range = (unsigned char)b3_agg_block_range(a, own, tg);
    a->want_slam_speed = (unsigned char)b3_agg_slam_speed(a, own, tg);
}

/* --- FUN_0016A8C0: the "rubbed AI car goes blind" timer ----------------- */
static void b3_agg_blind(B3AiAggro* a, float dt) {
    a->blind_out = 0;
    a->blind_time -= dt;
    if (0.0f < a->blind_time) {
        if (a->blind_arm) {
            a->blind_phase -= dt;
            if (a->blind_phase <= 0.0f) {
                a->blind_phase += B3AG_BLIND_STEP;
                a->blind_bits = (a->blind_bits >> 1) | (a->blind_bits << 31);
            }
            if (a->blind_bits & 1u) a->blind_out = 1;
        }
        return;
    }
    if (!a->blind_arm) { a->blind_time = 0.0f; return; }
    a->blind_time = B3AG_BLIND_S;
    a->blind_arm = 0;
}

/* --- the shared "enter state X at clock, for `dur` seconds" tail -------- */
static void b3_agg_arm(B3AiAggro* a, int state, float dur, float clock) {
    a->state = state;
    a->entered = clock;
    a->timer = (dur == -1.0f) ? -1.0f : clock + dur;
}

/* the reset block at 0x001697F1 (shared by every abandon path) */
static void b3_agg_drop(B3AiAggro* a) {
    a->aim_valid = 0;
    a->attacking = 0;
    a->slam_speed = 0;
    a->target = -1;
    a->lateral = 0.0f;
    a->longitudinal = 0.0f;
    a->can_slam = a->block_range = a->want_slam_speed = 0;
}

/* --- FUN_0016A310: back to idle ---------------------------------------- */
static void b3_agg_to_idle(B3AiAggro* a, float clock) {
    a->state = B3_AGGRO_IDLE;
    a->entered = clock;
    a->timer = -1.0f;
    b3_agg_drop(a);
}

/* --- FUN_00169BD0: pick a target --------------------------------------- */
static int b3_agg_pick(B3AiAggro* a, const B3AiAggroWorld* w, int self) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    const B3AiAggroCar* own = &w->cars[self];
    const B3AiAggroCar* tg;
    int best = -1, i;
    float bestv = 3.4028234663852886e+38f;
    a->target = -1;
    if (P->min_aggression > own->aggression) return 0;
    if (w->ncars == 1) {
        best = 0;
    } else {
        for (i = 0; i < w->ncars; i++) {
            float lat;
            if (i == self) continue;
            lat = fabsf(own->lateral_to[i]);
            if (bestv < lat) continue;
            if (w->cars[i].wrecked) continue;
            best = i;
            bestv = lat;
        }
        if (bestv > B3AG_PICK_LATERAL) {
            if (w->ncars == 0) return 0;
            best = own->player_slot;
        }
    }
    if (best < 0 || best >= w->ncars) return 0;
    if (best == self) return 0;
    tg = &w->cars[best];
    if (tg->race_mode == 0 && tg->in_takedown) return 0;
    if (own->mode2450 == 0) {
        float gap = tg->track_dist - own->track_dist;
        if (gap > P->dist_behind_m) return 0;
        if (0.0f - P->dist_ahead_m > gap) return 0;
        if (P->min_target_mph > b3_mph(tg->speed_ms)) return 0;
    }
    a->target = best;
    b3_agg_measure(a, w, self);
    return 1;
}

/* --- FUN_0016A950: someone has been designated our rival -> retaliate --- */
static int b3_agg_retaliate(B3AiAggro* a, const B3AiAggroWorld* w, int self) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    const B3AiAggroCar* own = &w->cars[self];
    const B3AiAggroCar* rv;
    if (own->mode2450 != 0) return 0;
    if (own->rival < 0 || own->rival >= w->ncars) return 0;
    rv = &w->cars[own->rival];
    if (rv->race_mode != 1) return 0;
    a->target = own->rival;
    b3_agg_measure(a, w, self);
    a->can_slam = 1;
    if (a->lateral <= 1.0f)
        b3_agg_arm(a, B3_AGGRO_STEER_OUT, P->steer_out_s, w->clock);
    else
        b3_agg_arm(a, B3_AGGRO_SLAM, P->slam_s, w->clock);
    return 1;
}

/* --- FUN_0016A0A0: the positioning aim (state 1/2) ---------------------- */
static int b3_agg_approach(B3AiAggro* a, const B3AiAggroWorld* w, int self) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    const B3AiAggroCar* own = &w->cars[self];
    const B3AiAggroCar* tg = b3_agg_car(w, a->target);
    float dist, sgn, cr[3], d[4];
    int k;
    if (!tg) return 0;
    if (P->min_target_mph > b3_mph(tg->speed_ms)) {
        float f = own->aggression * P->max_wait_s;
        b3_agg_arm(a, B3_AGGRO_COOLDOWN, f, w->clock);
        b3_agg_drop(a);
        return 0;
    }
    d[0] = own->pos[0] - tg->pos[0];
    d[1] = own->pos[1] - tg->pos[1];
    d[2] = own->pos[2] - tg->pos[2];
    d[3] = 0.0f;
    b3_norm3(d);                       /* FUN_0002C0D0 @0x0016A189 */
    /* dist = (targ_dist + targ_speed*0.25) - (own_dist + own_speed*0.25),
     * pushed out to 50 m on whichever side of the target we end up on. */
    dist = (tg->track_dist + tg->speed_ms * 0.25f)
         - (own->track_dist + own->speed_ms * 0.25f);
    if (dist <= 0.0f) dist = B3AG_PICK_LATERAL - dist;
    else              dist = dist + B3AG_PICK_LATERAL;
    sgn = b3_sign1(a->side);
    /* cross((0,1,0), target road direction) -- the target's lane normal */
    cr[0] = 1.0f * tg->road_dir[2] - 0.0f * tg->road_dir[1];
    cr[1] = 0.0f * tg->road_dir[0] - 0.0f * tg->road_dir[2];
    cr[2] = 0.0f * tg->road_dir[1] - 1.0f * tg->road_dir[0];
    for (k = 0; k < 3; k++)
        a->aim[k] = tg->pos[k] + own->road_dir[k] * dist
                  + cr[k] * sgn * B3AG_MATCH_BONUS;   /* literal 5.0 here */
    a->aim[3] = tg->pos[3];
    a->aim_valid = (unsigned char)b3_agg_aim_ok(own);
    a->side = d[0] * own->right[0] + d[1] * own->right[1]
            + d[2] * own->right[2];
    return 1;
}

/* --- FUN_0016A4E0: the blocking aim (state 7) --------------------------- */
static int b3_agg_block_aim(B3AiAggro* a, const B3AiAggroWorld* w, int self) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    const B3AiAggroCar* own = &w->cars[self];
    const B3AiAggroCar* tg = b3_agg_car(w, a->target);
    float d[4], lat, sgn, lead, gap;
    int k;
    if (!b3_agg_block_range(a, own, tg)) return 0;
    for (k = 0; k < 3; k++) d[k] = own->pos[k] - tg->pos[k];
    d[3] = 0.0f;
    b3_norm3(d);
    lat = d[0] * own->right[0] + d[1] * own->right[1] + d[2] * own->right[2];
    sgn = b3_sign1(lat);
    gap = tg->track_dist - own->track_dist;
    lead = ((0.0f - gap) / (own->car_length + P->block_dist_m))
           * B3AG_BLOCK_LEAD + B3AG_BLOCK_LEAD;
    for (k = 0; k < 4; k++)
        a->aim[k] = tg->pos[k] + tg->fwd[k] * lead
                  + tg->right[k] * (0.0f - sgn);
    a->aim_valid = (unsigned char)b3_agg_aim_ok(own);
    return 1;
}

/* --- the steer-out / recoil aim (FUN_00169540 cases 3 and 6) ------------ */
static void b3_agg_steer_out_aim(B3AiAggro* a, const B3AiAggroCar* own,
                                 float sgn) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    float lead = own->speed_ms * B3AG_STEEROUT_LEAD;
    int k;
    for (k = 0; k < 4; k++)
        a->aim[k] = own->pos[k] + own->fwd[k] * lead
                  + own->right[k] * P->steer_out_m * sgn;
    a->aim_valid = (unsigned char)b3_agg_aim_ok(own);
}

/* --- FUN_00169E80: the state-4 tick ------------------------------------ */
static void b3_agg_slam_tick(B3AiAggro* a, const B3AiAggroWorld* w, int self) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    const B3AiAggroCar* own = &w->cars[self];
    const B3AiAggroCar* tg = b3_agg_car(w, a->target);
    float f, d[3], side_now, lead;
    int k;

    if (a->hit) {
        b3_agg_arm(a, B3_AGGRO_RECOIL, P->steer_out_s, w->clock);
        a->side = 0.0f - a->side;
        b3_agg_drop(a);
        return;
    }
    if (!tg) {                       /* the retail code would fault here */
        b3_agg_arm(a, B3_AGGRO_COOLDOWN, own->aggression * P->max_wait_s,
                   w->clock);
        b3_agg_drop(a);
        return;
    }
    for (k = 0; k < 3; k++) d[k] = own->pos[k] - tg->pos[k];
    side_now = b3_sign1(d[0] * own->right[0] + d[1] * own->right[1]
                        + d[2] * own->right[2]);
    f = own->aggression * P->max_wait_s;
    if (b3_sign1(a->side) != side_now) {
        /* we crossed to the other side of the victim -- give up */
        if ((B3AG_SLAM_LEAD > f || own->mode2450 != 0) && !tg->wrecked) {
            b3_agg_arm(a, B3_AGGRO_RETRY, B3AG_RETRY_S, w->clock);
            return;
        }
        b3_agg_arm(a, B3_AGGRO_COOLDOWN, f, w->clock);
        b3_agg_drop(a);
        return;
    }
    if (!a->can_slam) {
        if (tg->race_mode != 1) {
            b3_agg_arm(a, B3_AGGRO_RETRY, B3AG_RETRY_S, w->clock);
            return;
        }
        b3_agg_arm(a, B3_AGGRO_COOLDOWN, f, w->clock);
        b3_agg_drop(a);
        return;
    }
    /* THE SLAM: aim dead at the victim, 0.1 s of its travel ahead. */
    lead = own->speed_ms * B3AG_SLAM_LEAD;
    for (k = 0; k < 4; k++) a->aim[k] = tg->pos[k] + tg->fwd[k] * lead;
    a->aim_valid = (unsigned char)b3_agg_aim_ok(own);
}

/* the "slam or steer out first" fork at LAB_00169687 */
static void b3_agg_fork(B3AiAggro* a, float clock) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    if (a->lateral > B3AG_SKIP_STEEROUT)
        b3_agg_arm(a, B3_AGGRO_SLAM, P->slam_s, clock);
    else
        b3_agg_arm(a, B3_AGGRO_STEER_OUT, P->steer_out_s, clock);
}

/* the "abandon: cooldown or 1 s retry" fork used by states 2/4/6 */
static void b3_agg_abandon(B3AiAggro* a, const B3AiAggroWorld* w, int self) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    const B3AiAggroCar* own = &w->cars[self];
    const B3AiAggroCar* tg = b3_agg_car(w, a->target);
    float f = own->aggression * P->max_wait_s;
    if ((B3AG_SLAM_LEAD > f || own->mode2450 != 0) && tg && !tg->wrecked) {
        b3_agg_arm(a, B3_AGGRO_RETRY, B3AG_RETRY_S, w->clock);
        return;
    }
    b3_agg_arm(a, B3_AGGRO_COOLDOWN, f, w->clock);
    b3_agg_drop(a);
}

void b3_aggro_init(B3AiAggro* a, float clock) {
    memset(a, 0, sizeof *a);
    a->target = -1;
    a->timer = -1.0f;
    a->side = 0.0f;                /* +0x34 = 0 (b3_sign1(0) == +1) */
    a->blocked = 1;                /* +0x23 = 1 */
    a->state = B3_AGGRO_COOLDOWN;  /* the race starts in the cooldown state */
    a->entered = clock;
    a->timer = (b3_ai_aggro_params.start_delay_s == -1.0f)
             ? -1.0f : clock + b3_ai_aggro_params.start_delay_s;
}

void b3_aggro_update(B3AiAggro* a, const B3AiAggroWorld* w, int self) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    const B3AiAggroCar* own;
    const B3AiAggroCar* tg;
    int expired, i;

    if (self < 0 || self >= w->ncars) return;
    own = &w->cars[self];
    /* entry gate: player cars (unless class 3) and traffic never attack */
    if ((own->race_mode == 0 && own->car_class != 3) || own->car_class == 0) {
        a->aim_valid = 0;
        a->hit = 0;
        return;
    }
    b3_agg_blind(a, w->dt);

    expired = (a->timer != -1.0f) && (a->timer <= w->clock);

    tg = b3_agg_car(w, a->target);
    if (tg && tg->wrecked) {                    /* the victim is out */
        b3_agg_arm(a, B3_AGGRO_COOLDOWN, own->aggression * P->max_wait_s,
                   w->clock);
        b3_agg_drop(a);
        tg = 0;
    }
    b3_agg_measure(a, w, self);

    switch (a->state) {
    case B3_AGGRO_IDLE:
        a->aim_valid = 0;
        if (b3_agg_pick(a, w, self)) {
            a->attacking = 1;
            b3_agg_arm(a, B3_AGGRO_APPROACH, P->position_time_s, w->clock);
        }
        break;

    case B3_AGGRO_APPROACH:
        if (b3_agg_retaliate(a, w, self)) goto blocked;
        a->slam_speed = a->want_slam_speed;
        if (a->can_slam) { b3_agg_fork(a, w->clock); break; }
        if (b3_agg_approach(a, w, self) && a->block_range) {
            float dur = (P->block_max_s - P->block_min_s) * own->aggression
                      + P->block_min_s;
            b3_agg_arm(a, B3_AGGRO_BLOCK, dur, w->clock);
        }
        break;

    case B3_AGGRO_RETRY:
        if (b3_agg_retaliate(a, w, self)) goto blocked;
        a->slam_speed = a->want_slam_speed;
        if (a->can_slam) { b3_agg_fork(a, w->clock); break; }
        if (expired) b3_agg_abandon(a, w, self);
        else         b3_agg_approach(a, w, self);
        break;

    case B3_AGGRO_STEER_OUT:
        a->slam_speed = a->want_slam_speed;
        if (expired) {
            b3_agg_arm(a, B3_AGGRO_SLAM, P->slam_s, w->clock);
            break;
        }
        if (!tg) { b3_agg_abandon(a, w, self); break; }
        {   /* latch the side we are on, then aim `steer out distance` clear */
            float d[3];
            int k;
            for (k = 0; k < 3; k++) d[k] = own->pos[k] - tg->pos[k];
            a->side = b3_sign1(d[0] * own->right[0] + d[1] * own->right[1]
                               + d[2] * own->right[2]);
            b3_agg_steer_out_aim(a, own, a->side);
        }
        break;

    case B3_AGGRO_SLAM:
        a->slam_speed = 1;
        if (expired) b3_agg_abandon(a, w, self);
        else         b3_agg_slam_tick(a, w, self);
        break;

    case B3_AGGRO_COOLDOWN:
        if (expired) b3_agg_to_idle(a, w->clock);
        break;

    case B3_AGGRO_RECOIL:
        a->slam_speed = 0;
        if (!expired) b3_agg_steer_out_aim(a, own, b3_sign1(0.0f - a->side));
        else          b3_agg_abandon(a, w, self);
        break;

    case B3_AGGRO_BLOCK:
        a->slam_speed = 0;
        a->attacking = 1;
        if (b3_agg_retaliate(a, w, self)) goto blocked;
        if (!expired && b3_agg_block_aim(a, w, self)) {
            if (!a->can_slam) goto blocked;
            if (a->lateral > 1.0f)
                b3_agg_arm(a, B3_AGGRO_SLAM, P->slam_s, w->clock);
            else
                b3_agg_arm(a, B3_AGGRO_STEER_OUT, P->steer_out_s, w->clock);
        } else {
            b3_agg_arm(a, B3_AGGRO_APPROACH, P->position_time_s, w->clock);
        }
        /* every exit of case 7 lands on 0x00169B78 -- it never runs the
         * immunity sweep, so the block state always reports "blocked". */
        goto blocked;

    default:
        goto blocked;
    }

    /* tail: "immunity" -- anything we hit in the last `immunity_s` seconds
     * blocks the machine's output for this frame (FUN_00169540 @0x00169715) */
    for (i = 0; i < w->ncars; i++) {
        if (own->last_hit[i] + P->immunity_s > own->race_time) goto blocked;
    }
    a->blocked = 0;
    a->hit = 0;
    return;
blocked:
    a->blocked = 1;
    a->hit = 0;
}

/* --- FUN_0016AF10: aggro flags -> AI+0x790 mode, AI+0x794 target -------- */
int b3_aggro_arbitrate(const B3AiAggro* a, const B3AiAggroWorld* w, int self,
                       int* target) {
    const B3AiAggroCar* own;
    const B3AiAggroCar* tg;
    if (target) *target = -1;
    if (!w->track_loaded) return 0;
    if (!a->attacking) return 0;
    tg = b3_agg_car(w, a->target);
    if (!tg) return 0;
    own = &w->cars[self];
    if (own->mode2450 == 0 && tg->boosting) return 0;
    if (!(a->target == own->rival || self == tg->rival || own->mode2450 != 0)) {
        if (B3AG_MPH100 > b3_mph(tg->speed_ms)) return 0;
    }
    if (target) *target = a->target;
    return a->slam_speed ? 5 : 4;
}

/* ======================================================================== */
/* FUN_00172FA0 -- the aggression speed demand, called from FUN_001724F0.   */
/* ======================================================================== */
void b3_aggro_speed_init(B3AiAggroSpeed* s) {
    memset(s, 0, sizeof *s);
    s->target = -1;
    s->last_target = -1;
    s->boost_until = -1.0f;
    s->brake_hold_out = -1.0f;
}

float b3_ai_aggro_speed(B3AiAggroSpeed* s, const B3AiAggroWorld* w, int self,
                        float aggression, float spd) {
    const B3AiAggroParams* P = &b3_ai_aggro_params;
    const B3AiAggroCar* own;
    const B3AiAggroCar* tg;
    float own_speed, tgt_speed, rel, c, base, out;
    int ahead;

    s->brake_hold_valid = 0;
    if (s->mode == 0) { s->last_target = -1; return spd; }
    own = b3_agg_car(w, self);
    tg = b3_agg_car(w, s->target);
    if (!own || !tg) { s->last_target = -1; return spd; }

    own_speed = own->speed_ms;
    tgt_speed = tg->speed_ms;
    rel = b3_agg_longit(own, tg);          /* FUN_001717B0, + = target ahead */
    ahead = (rel > 0.0f);
    c = b3_clamp(rel * 0.01f, -1.0f, 1.0f);
    base = c * tgt_speed + tgt_speed;
    if (base < b3_ai_params.min_speed_mps) base = b3_ai_params.min_speed_mps;

    if (s->mode == 5) {
        /* lock to our own speed +-1 m/s around the matched demand */
        out = b3_clamp(base, own_speed - 1.0f, own_speed + 1.0f);
        s->matched = out;
        s->last_target = -1;
        s->want_boost = s->boost_now =
            (unsigned char)(tg->boosting &&
                            (tg->race_time - tg->boost_start) > 1.5f);
        return out;
    }
    if (s->mode != 4) {
        s->last_target = -1;
        if (s->mode == 3) return tgt_speed;
        if (ahead ? (s->mode == 1) : (s->mode == 2)) return tgt_speed;
        return spd;
    }

    /* ---- mode 4: close in on the target ------------------------------- */
    {
        int ok = 1;
        int same = (s->last_target == s->target);
        /* `ok` (byte [esp+0xe], 0x0017318E): a PLAYER car hit us less than a
         * second ago -- the stamp is racecar+0x16C0 and the aggressor
         * pointer racecar+0x16BC -- or the target has not actually started
         * the race (FUN_00194430 == 0). */
        if (own->slammed_time != -1.0f
            && own->race_time <= own->slammed_time + 1.0f
            && own->slammed_by >= 0 && own->slammed_by < w->ncars
            && w->cars[own->slammed_by].race_mode == 0)
            ok = 0;
        if (tg->race_progress_zero && own->progress_gate && w->track_loaded)
            ok = 0;

        out = spd;
        if (own->mode2450 == 1) {
            /* BLOCK_B (0x0017336E): the "chase the player" variant */
            out = base;
            if (same) {
                if (s->acquired_ahead && ok &&
                    rel > aggression * P->boost_aggro_m + P->boost_dist_m) {
                    out = b3_ai_params.top_speed_mps;
                    s->want_boost = 1;
                }
            } else {
                s->acquired_ahead =
                    (unsigned char)(ahead && rel > B3AG_TEN);
                s->last_target = s->target;
            }
        } else {
            /* BLOCK_A (0x0017321D): close the track-distance gap */
            float gap = tg->track_dist - own->track_dist;
            if (!same) {
                s->acquired_ahead = (unsigned char)ahead;
            } else {
                float demand = (tgt_speed + B3AG_MATCH_BONUS + tg->track_dist)
                             - (own->track_dist + own_speed);
                int done = 0;
                if (ok && gap > 0.0f) {
                    out = b3_ai_params.top_speed_mps;
                    s->want_boost = 1;
                    done = 1;
                }
                if (!done) {
                    /* 0x00173287: unreachable in retail (it wants gap < 10
                     * AND demand > 10 AND own-tgt > 30 mph at once), kept
                     * so the port is a faithful transcription. */
                    if (B3AG_TEN > gap && demand > B3AG_TEN
                        && own_speed - tgt_speed > B3AI_BRAKE_EXCESS)
                        s->want_boost = 1;
                    demand += own_speed;
                    out = (tgt_speed > demand) ? tgt_speed : demand;
                    if (fabsf(gap) > P->close_match_m) {
                        if (gap > 0.0f) {
                            out = out * 2.0f;
                        } else {
                            float alt = spd - B3AG_TWENTY;
                            float slow = out * P->slow_factor;
                            out = (slow > alt) ? slow : alt;
                        }
                    }
                }
            }
            s->last_target = s->target;
            s->last_own_speed = own_speed;
            s->last_tgt_speed = tgt_speed;
        }
    }

    /* tail 0x001733DB: arm a 2 s boost window, then a 2 s re-arm lockout */
    if (s->want_boost && s->boost_until == -1.0f && w->clock > s->boost_rearm) {
        s->boost_until = w->clock + B3AG_AIM_GRACE;
        s->boost_rearm = s->boost_until + B3AG_AIM_GRACE;
    }
    if (s->wants_boost && !s->want_boost) {
        s->brake_hold_out = w->clock + 1.0f;   /* -> physics vehicle +0x1570 */
        s->brake_hold_valid = 1;
    }
    return out;
}

void b3_ai_boost_latch(B3AiAggroSpeed* s, float clock) {
    if (s->boost_until != -1.0f) {
        if (s->boost_until <= clock && clock != s->boost_until)
            s->boost_until = -1.0f;
        s->boost_now = 1;
    }
    if (s->boost_now) {
        s->want_boost = 0;
        s->boost_now = 0;
        s->wants_boost = 1;
        return;
    }
    s->wants_boost = (unsigned char)(s->want_boost != 0);
    s->want_boost = 0;
    s->boost_now = 0;
}

/* ======================================================================== */
/* FUN_00171BE0 -- the drift-lock flags racecar+0x2413/+0x2414/+0x2415.     */
/* (Previously mislabelled as the attack flags -- see section 14.)          */
/* ======================================================================== */
void b3_ai_drift_flags(int drift_enable, int mode, float target_angle_deg,
                       const float aim[4], const float pos[4],
                       const float fwd[4], float speed_ms,
                       int* out_left, int* out_right, int* out_commit) {
    float d[3], c[3], t;
    int left = 0, right = 0;
    *out_left = 0;
    *out_right = 0;
    if (!drift_enable) return;
    d[0] = aim[0] - pos[0];
    d[1] = aim[1] - pos[1];
    d[2] = aim[2] - pos[2];
    if (mode == 4 && 0.0f > target_angle_deg) {
        left = 1;
        c[0] = d[1] * 0.0f - d[2] * 1.0f;      /* cross(d, up(0,1,0)) */
        c[1] = d[2] * 0.0f - d[0] * 0.0f;
        c[2] = d[0] * 1.0f - d[1] * 0.0f;
    } else if (mode == 2 && target_angle_deg > 0.0f) {
        right = 1;
        c[0] = 1.0f * d[2] - 0.0f * d[1];      /* cross(up, d) */
        c[1] = 0.0f * d[0] - 0.0f * d[2];
        c[2] = 0.0f * d[1] - 1.0f * d[0];
    } else {
        return;
    }
    *out_left = left;
    *out_right = right;
    if (c[0] * fwd[0] + c[1] * fwd[1] + c[2] * fwd[2] > 0.0f) {
        *out_commit = 0;
        return;
    }
    t = b3_len3(d) / speed_ms;
    if (b3_ai_drift_apex_time > t
        || 0.0f - b3_ai_params.drift_start_deg > target_angle_deg)
        *out_commit = 1;
    else
        *out_commit = 0;
}

/* ======================================================================== */
/* section 16: the route driver's wheel + watchdogs (FUN_00170820)          */
/* ======================================================================== */

void b3_ai_wheel_init(B3AiWheel* w, int node) {
    memset(w, 0, sizeof *w);
    w->ai_enable = 1;              /* FUN_0018D0E0 @0x0018D563: +0x19A8 = 1 */
    w->last_node = node;           /* FUN_001705F0 @0x00170762              */
    w->route_alt = 1;              /* FUN_00175A10: AI+0x1F0 = 1            */
    w->alt_t[0] = B3_AI_ROUTE_ALT_S;
    w->alt_t[1] = B3_AI_ROUTE_ALT_S;
}

/* FUN_00170820 @0x00170827..0x0017089C, transcribed branch for branch:
 *   flag != 0 : t1 -= dt; if (0 > t1) { flag = 0; t0 = 50.0; }
 *   flag == 0 : t0 -= dt; if (0 > t0) { flag = 1; t1 = 50.0; }
 * (the COMISS is `0 vs new`, so the reload happens strictly below zero). */
int b3_ai_route_alt(B3AiWheel* w, float dt) {
    if (w->route_alt) {
        w->alt_t[1] -= dt;
        if (w->alt_t[1] < 0.0f) {
            w->route_alt = 0;
            w->alt_t[0] = B3_AI_ROUTE_ALT_S;
        }
    } else {
        w->alt_t[0] -= dt;
        if (w->alt_t[0] < 0.0f) {
            w->route_alt = 1;
            w->alt_t[1] = B3_AI_ROUTE_ALT_S;
        }
    }
    return w->route_alt;
}

/* FUN_0018CB60 @0x0018CB60:
 *   if (on == racecar+0x27D8) return;            -- edge-triggered
 *   racecar+0x27D8 = on;
 *   if (on)  { racecar+0x19D0..+0x19DC = 0; FUN_00179760(AI); }
 *   else     { v+0x1534 = 1.0; if (v+0x1524 == 4) v+0x1524 = 0; }          */
int b3_ai_wheel_set(B3AiWheel* w, B3AiState* s, int on) {
    on = on ? 1 : 0;
    if (on == w->ai_wheel) return B3_AI_WHEEL_NONE;
    w->ai_wheel = on;
    if (on) return B3_AI_WHEEL_TAKE;   /* caller resets the navigator       */
    if (s) {
        s->steer_authority = 1.0f;     /* MOVSS [v+0x1534], 1.0             */
        if (s->drift_state == 4) s->drift_state = 0;
    }
    return B3_AI_WHEEL_GIVE;
}

/* FUN_00171650 @0x00171694 */
void b3_ai_replace_done(B3AiWheel* w) { w->skip_once = 1; }

/* FUN_00170820 @0x001708A3..0x001708D7 */
int b3_ai_wheel_gate(B3AiWheel* w, int have_section, int have_vehicle) {
    if (!have_section || !have_vehicle || !w->ai_enable) return 0;
    if (w->skip_once) { w->skip_once = 0; return 0; }
    return 1;
}

/* FUN_001712E0 @0x00171411..0x001714A3 */
int b3_ai_offworld(B3AiWheel* w, int node, float dy) {
    w->road_dy = dy;
    if (dy > 0.0f - B3_AI_ROAD_LATCH_M) {
        w->last_node = node;
        w->below_frames = 0;
        return 0;
    }
    if (dy < 0.0f - B3_AI_OFFWORLD_DROP_M) {
        if (++w->below_frames < B3_AI_OFFWORLD_FRAMES) return 0;
        w->below_frames = 0;
        return 1;
    }
    w->below_frames = 0;
    return 0;
}

/* FUN_0018D790 @0x0018D840 (the increment) + FUN_00170820 @0x001708EE (the
 * test and the clear).  Note retail never clears the counter on a SUCCESSFUL
 * walk -- only the rescue itself does, at 0x00170926. */
int b3_ai_navfail(B3AiWheel* w, int walk_ok) {
    if (!walk_ok) w->navfail_frames++;
    if (w->navfail_frames > B3_AI_NAVFAIL_FRAMES) {
        w->navfail_frames = 0;
        w->below_frames = 0;        /* MOV [ESI+0x2460],EBX @0x0017092C     */
        return 1;
    }
    return 0;
}

/* FUN_00176150 -- see the header for the full transcription. */
float b3_ai_corner_brake(float corner_speed, float approach_d, int boost_scale,
                         float speed_cap, int target_mode, int mode_1fc) {
    float out;
    if (mode_1fc != 0 || corner_speed <= 0.0f) {          /* 0x003B16E0 = 0 */
        out = speed_cap;
    } else {
        float cs = corner_speed;
        if (boost_scale) cs *= 1.02f;                     /* 0x003A2C44     */
        if (target_mode == 0)
            out = b3_ai_params.brake_dist_factor * (cs - approach_d) + cs;
        else
            out = cs;
    }
    return out < speed_cap ? out : speed_cap;             /* MINSS          */
}
