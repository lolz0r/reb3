// See burnout3_vehicle_sim.h for the provenance split: the parameters and
// defaults below are recovered from the binary; the force equations wiring them
// together are a reconstruction pending the update functions being reversed.

#include "burnout3_vehicle_sim.h"
#include <math.h>
#include <string.h>

#define DEG2RAD 0.01745329252f

// Route a value to the config field owning a given 0x1D0-struct offset.
// This is the single offset->field mapping (same offsets as the registrar
// FUN_00132D10 / burnout3_physics_params.h); both the compiled-in defaults
// and the per-car VDB overrides from Data/vdb.xml go through it.
void b3_config_set_by_offset(B3PhysicsConfig* cfg, unsigned offset,
                             float value) {
    if (offset >= 0x0E0 && offset <= 0x100) {          // gear ratio block
        cfg->gear[(offset - 0x0E0) / 4] = value;
        return;
    }
    switch (offset) {
    case 0x0B8: cfg->mass_kg = value; break;
    case 0x0BC: cfg->front_attach_height = value; break;
    case 0x0C0: cfg->front_spring_force = value; break;
    case 0x0C4: cfg->front_spring_damping = value; break;
    case 0x0C8: cfg->front_spring_length = value; break;
    case 0x0CC: cfg->rear_spring_force = value; break;
    case 0x0D0: cfg->rear_spring_damping = value; break;
    case 0x0D4: cfg->rear_spring_length = value; break;
    case 0x0D8: cfg->rear_attach_height = value; break;
    case 0x104: cfg->idle_rpm = value; break;
    case 0x108: cfg->change_up_rpm = value; break;
    case 0x10C: cfg->change_down_rpm = value; break;
    case 0x118: cfg->max_rpm = value; break;
    case 0x11C: cfg->torque = value; break;
    case 0x120: cfg->peak_torque_revs = value; break;
    case 0x124: cfg->falloff_torque_revs = value; break;
    case 0x128: cfg->boost_kick_torque = value; break;
    case 0x12C: cfg->boost_kick_time = value; break;
    case 0x130: cfg->drag_coef = value; break;
    case 0x134: cfg->downforce_coef = value; break;
    case 0x138: cfg->brake_force_height = value; break;
    case 0x13C: cfg->accel_force_height = value; break;
    case 0x140: cfg->steer_force_height = value; break;
    case 0x144: cfg->drift_force_height = value; break;
    case 0x148: cfg->steer_min_angle = value; break;
    case 0x14C: cfg->steer_max_angle = value; break;
    case 0x150: cfg->steer_min_velocity = value; break;
    case 0x154: cfg->steer_max_velocity = value; break;
    case 0x158: cfg->steer_response = value; break;
    case 0x15C: cfg->braking_factor = value; break;
    case 0x160: cfg->slide_max = value; break;
    case 0x164: cfg->slide_min = value; break;
    case 0x168: cfg->turn_momentum_slow = value; break;
    case 0x16C: cfg->turn_momentum_fast = value; break;
    case 0x170: cfg->auto_drift_delay = value; break;
    case 0x174: cfg->turn_rate = value; break;
    case 0x178: cfg->wall_collision_penalty = value; break;
    case 0x17C: cfg->lsdm_speed_limit = value; break;
    case 0x180: cfg->lsdm_steering_angle = value; break;
    case 0x184: cfg->lsdm_torque1 = value; break;
    case 0x188: cfg->lsdm_torque2 = value; break;
    case 0x18C: cfg->accel_multiplier = value; break;
    case 0x190: cfg->steer_drag_coef = value; break;
    case 0x194: cfg->min_drift_speed = value; break;
    case 0x198: cfg->max_speed_pressure = value; break;
    case 0x19C: cfg->engine_braking_factor = value; break;
    case 0x1A0: cfg->max_drift_angle = value; break;
    case 0x1A4: cfg->max_speed_in_boost = value; break;
    case 0x1B0: cfg->in_air_corkscrew_damping = value; break;
    case 0x1B4: cfg->min_drift_angle_in_air = value; break;
    case 0x1BC: cfg->steer_away_time = value; break;
    case 0x1C0: cfg->total_out_of_control_time = value; break;
    case 0x1C4: cfg->aggr_steering_max_angle = value; break;
    case 0x1C8: cfg->aggr_steering_max_velocity = value; break;
    case 0x1CC: cfg->aggr_steering_drag_coef = value; break;
    default: break;   // offsets outside the recovered 64 are ignored
    }
}

void b3_physics_defaults(B3PhysicsConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    // Compiled-in defaults (FUN_00132950), routed through the shared
    // offset->field mapping so this stays in lockstep with the extractor.
    for (int i = 0; i < PHYSICS_PARAM_COUNT; i++)
        if (PHYSICS_PARAMS[i].has_def)
            b3_config_set_by_offset(cfg, PHYSICS_PARAMS[i].offset,
                                    PHYSICS_PARAMS[i].def);

    // RECONSTRUCTION FALLBACK: the XBE's compiled-in gear defaults are
    // placeholders (every ratio 5.0, which redlines all gears instantly).
    // The real per-car ratios live in Data/vdb.xml and are applied over
    // these defaults via burnout3_car_physics.h (tools/extract_car_vdb.py);
    // this substitution only survives for the handful of vehicles with no
    // VDB overrides (COMPCAR18, HEVYCAR35, most TSPC traffic).
    if (cfg->gear[2] == 5.0f && cfg->gear[8] == 5.0f) {
        static const float approx[9] =
            { -3.0f, 0.0f, 3.20f, 2.10f, 1.55f, 1.20f, 0.97f, 0.82f, 3.90f };
        for (int i = 0; i < 9; i++) cfg->gear[i] = approx[i];
    }
}

// The game stores three points on the curve -- peak torque value, the rpm it
// peaks at, and the rpm where it falls off. Rising to the peak and decaying
// past the falloff point is the reconstruction.
float b3_engine_torque(const B3PhysicsConfig* cfg, float rpm) {
    if (rpm <= 0.0f) return 0.0f;
    if (rpm <= cfg->peak_torque_revs) {
        // Ramp in from idle so torque is non-zero off the line.
        float t = rpm / (cfg->peak_torque_revs > 0.0f ? cfg->peak_torque_revs : 1.0f);
        return cfg->torque * (0.55f + 0.45f * t);
    }
    if (rpm <= cfg->falloff_torque_revs) return cfg->torque;
    float span = cfg->max_rpm - cfg->falloff_torque_revs;
    if (span <= 0.0f) return cfg->torque;
    float t = (rpm - cfg->falloff_torque_revs) / span;
    if (t > 1.0f) t = 1.0f;
    return cfg->torque * (1.0f - 0.45f * t);
}

float b3_steer_angle(const B3PhysicsConfig* cfg, float speed) {
    // The PORTED scheduler from FUN_0011ECF0 (b3_steer_schedule, verified by
    // executing the real input stage): angle = Max Velocity param minus
    // 1.4 deg per m/s past Min Velocity, clamped to the min/max angle window.
    // (Replaces the old interpolation reconstruction.)
    return b3_steer_schedule(speed, cfg->steer_max_velocity,
                             cfg->steer_min_velocity,
                             cfg->steer_min_angle, cfg->steer_max_angle);
}

// (The old heuristic gearbox lived here; superseded by the ported
// b3_engine_transmission_update, which is verified against the real code.)

// PH-22 -- THE RETIRED SCALAR RECONSTRUCTION, DELETED.
//
// `b3_vehicle_step()` lived here: a speed-along-heading reconstruction with
// two reconstruction-GLUE marks, the drivers of ledger row PH-22.  It has had
// no caller since burnout3_full.c switched to `b3_vehicle_step_full` (the
// FUN_0011ECF0 + FUN_0011BE50 port), and both of its marks are superseded by
// recovered code in that pipeline:
//
//   :173  "brake drag applied along the travel direction, GLUE: the harness
//          has no .bgd surface record".  The reason was false -- retail's
//          brake drag direction is veh+0xC0, the unit travel direction
//          FUN_000FFC80 maintains inside FUN_00109560, and no surface record
//          is involved.  FUN_0011D460 @0x0011DBD7..0x0011DCF7 applies it as a
//          FORCE at `pos + up * veh+0x1368` through FUN_001064B0, which is
//          what `b3_d460_force_pass` does today (the b3_brake_drag_scalar +
//          b3f_acc_add + b3f_torque_at block), pitch torque included.
//   :224  "the harness has no ray-cast ground contact, so each wheel's spring
//          length is relaxed toward the static-load equilibrium".  The real
//          per-wheel ray pass FUN_001239C0 + FUN_00123FD0 is ported and runs
//          every substep (`b3_prepass` / `b3_suspension_pass`) over the
//          harness ground probe.
//
// Nothing referenced the function (checked across src/ and tools/), so the
// row closes by deletion rather than by porting a second time.

// ===========================================================================
// PORTED 1:1 from the game
// ===========================================================================
// Everything below this line is transcribed directly from the decompilation of
// b3_vehicle_drivetrain_update (0x0011D460) with the recovered struct applied,
// not inferred. Each block cites its source. Unlike b3_vehicle_step() above,
// these are the game's own equations.

// Longitudinal resistance force, from the block immediately following the
// gear-change test in 0x0011D460. Decompiled form:
//
//   if (cond_extra) extra = k * speed_ms * q * q; else extra = 0;
//   if ((!flag_a && !flag_b)
//       || (speed_mph * 0.44704f <= speed_ms)
//       || (gear_current != gear_target && change_up_rpm <= omega * 9.549296f))
//       s = -(resist_coef * speed_ms) - extra;
//   else
//       s = 0;
//   accum.xyz += vel_dir * s;
//   accum.w   += speed_ms * s;      <- 4th component, easy to miss
//
// Note the sign: the scale is negative, so the force opposes the velocity
// direction. `extra` is a quadratic (v * q^2) term gated by a state flag; `q`
// lives at vehicle +0x1408 and `k` at +0x13C0, both still untyped, so they are
// passed in rather than guessed at.
void b3_resistance_force(const B3ResistanceIn* in, float out_force[4]) {
    float extra = 0.0f;
    if (in->extra_enabled)
        extra = in->k_quad * in->speed_ms * in->q * in->q;

    const int shifting = (in->gear_current != in->gear_target) &&
                         (in->change_up_rpm <= in->engine_omega_rads * B3_RADS_TO_RPM);
    const int apply = (!in->drivetrain_flag_a && !in->drivetrain_flag_b)
                   || (in->speed_mph * B3_MPH_TO_MS <= in->speed_ms)
                   || shifting;

    const float s = apply ? (-(in->resist_coef * in->speed_ms) - extra) : 0.0f;

    out_force[0] = in->vel_dir[0] * s;
    out_force[1] = in->vel_dir[1] * s;
    out_force[2] = in->vel_dir[2] * s;
    // The accumulator is a 4-vector: the game scales the speed magnitude by the
    // same factor, giving force-dot-velocity (rate of work) in w. Verified 6/6
    // against the real code; omitting it silently loses a term the rest of the
    // pipeline reads back from +0x0FC.
    out_force[3] = in->speed_ms * s;
}

// Vertical force: gravity plus speed-dependent downforce. From 0x0011D460:
//
//   0.0 - (downforce_coef * (speed_ms * 2.2374146f) * 0.1f + 10.0f) * mass
//
// Two details that only came out of running the real code (16/16 exact across
// coef, mass and speed):
//   * gravity is 10.0, not 9.81
//   * the m/s->mph conversion is the game's 2.2374146, not the true 2.2369363,
//     and it converts speed_ms here rather than reading the stored mph field
float b3_vertical_force(float downforce_coef, float speed_ms, float mass_kg) {
    const float mph = speed_ms * B3_MS_TO_MPH_GAME;
    return -(downforce_coef * mph * 0.1f + B3_GRAVITY) * mass_kg;
}

// Gear change-up test, from the same function. 9.549296f == 60 / (2*pi).
int b3_should_shift_up(float change_up_rpm, float engine_omega_rads) {
    return change_up_rpm <= engine_omega_rads * B3_RADS_TO_RPM;
}

// ---------------------------------------------------------------------------
// Engine / transmission -- FUN_00121560 and its support functions.
// Every function below is transcribed from the decompilation and verified by
// tools/validate_port.py against the real x86 running under Unicorn: 19 cases
// asserting the complete post-state (torque, omega, gear, timers, PRNG).
// ---------------------------------------------------------------------------

// The game's PRNG (FUN_00048760, also inlined in FUN_00121560).
//   state = state*0x10000 + (state >> 16, arithmetic) + inc;  inc += state;
// returning the new state as an unsigned fraction of 2^32. The scale constant
// lives at 0x0054F46C, seeded from the float at 0x003B191C (= 1/2^32) by the
// initialiser at 0x264880. Seeds (FUN_001214A0): 0xFD462907 / 0x02B9D6F8.
// Returns double: the real code keeps the FILD result on the x87 stack in
// extended precision through the consuming multiply chain and rounds to
// f32 only at the final store, so rounding the rand itself to f32 here
// loses bits the game keeps (measured: ~5e-6 relative on the drive torque,
// which the tyre slip feedback amplifies ~2.5x per frame).
static double b3_rand01(unsigned* state, unsigned* inc) {
    int s = (int)*state;
    unsigned n = (unsigned)s * 0x10000u + (unsigned)(s >> 16) + *inc;
    *inc = n + *inc;
    *state = n;
    return (double)n * 2.3283064365386963e-10;
}

// Config -> live transmission copy (FUN_00134710, offsets +0x1448..+0x1498)
// followed by the reset FUN_001214A0 (gear count, rev limit, PRNG seeds).
void b3_engine_transmission_init(B3EngineTransmission* t,
                                 const B3PhysicsConfig* cfg) {
    memset(t, 0, sizeof *t);
    for (int i = 0; i < 9; i++) t->gears[i] = cfg->gear[i];
    t->idle_rpm          = cfg->idle_rpm;
    t->change_up_rpm     = cfg->change_up_rpm;
    t->change_down_rpm   = cfg->change_down_rpm;
    t->max_rpm           = cfg->max_rpm;
    t->torque            = cfg->torque;
    t->peak_omega        = cfg->peak_torque_revs    * B3_RPM_TO_RADS;
    t->falloff_omega     = cfg->falloff_torque_revs * B3_RPM_TO_RADS;
    t->boost_kick_torque = cfg->boost_kick_torque;
    t->boost_kick_time   = cfg->boost_kick_time;
    // FUN_001214A0: live limit starts at max rpm, PRNG seeded, neutral, and
    // the forward gear count = number of positive ratios from 1st, capped 6.
    t->rev_limit_rpm = t->max_rpm;
    t->rng_state = 0xFD462907u;
    t->rng_inc   = 0x02B9D6F8u;
    t->gear = 0;
    t->gear_count = 0;
    float r = t->gears[2];
    while (r > 0.0f && t->gear_count < 6) {
        t->gear_count++;
        r = t->gears[2 + t->gear_count];
    }
}

// FUN_00121560, verbatim. ESI = &vehicle->trans (+0x1448), EDI = boost input
// bit, stack args (throttle-brake, driven wheel omega, kick). Returns drive
// torque; the caller (FUN_0011ECF0 at 0x0011F3D2) stores it at v+0x1520.
float b3_engine_transmission_update(B3EngineTransmission* t, float throttle,
                                    float wheel_omega, int kick, int boost,
                                    float dt, float speed_ms,
                                    float max_boost_mph, float boost_elapsed,
                                    int boost_ramp_done) {
    if (boost && throttle < 0.9f) throttle = 0.9f;
    int gear = t->gear;
    // backwards wheel spin in 1st is amplified so it downshifts/corrects hard
    if (gear == 1 && wheel_omega < 0.0f) wheel_omega *= 100.0f;
    // target engine speed through the current ratio (gears[gear+1]: -1 maps
    // to Reverse, 0 to Neutral, 1..6 to 1st..6th) and the final drive
    float target = t->gears[8] * t->gears[gear + 1] * wheel_omega;
    const float idle_omega = t->idle_rpm * B3_RPM_TO_RADS;
    const float down_omega = t->change_down_rpm * B3_RPM_TO_RADS;
    if (gear == 0) target = t->rev_limit_rpm * B3_RPM_TO_RADS * throttle;

    if (t->shifting == 0) {
        if (t->flag_14B8 == 0) {
            t->downshift_block -= dt;
            const float up_del = t->upshift_block - dt;
            t->upshift_block = up_del;
            if ((target < down_omega && gear > 1 && throttle < 0.5f)
                || (target < down_omega * 0.75f && gear > 1)) {
                if (t->downshift_block <= 0.0f) {
                    t->shift_timer = 0.35f;
                    t->gear = gear - 1;
                    t->shifting = 1;
                    t->upshift_block = 1.0f;
                }
            } else if (t->change_up_rpm * B3_RPM_TO_RADS < target && gear > 0
                       && gear < t->gear_count && t->no_upshift == 0
                       && up_del <= 0.0f) {
                const float ratio = t->gears[gear + 1]; // OLD gear's ratio
                t->gear = gear + 1;
                t->shifting = 1;
                if (!boost) { t->shift_timer = 0.35f; t->downshift_block = 1.0f; }
                else        { t->shift_timer = 0.1f;  t->downshift_block = 0.1f; }
                target = t->gears[8] * ratio * wheel_omega;
            }
        }
    } else {
        t->shift_timer -= dt;
        if (0.0f < t->shift_timer) throttle *= 0.1f; // torque cut mid-shift
        else t->shifting = 0;
    }

    gear = t->gear;
    float limit_omega = t->rev_limit_rpm * B3_RPM_TO_RADS;
    if (gear == -1) limit_omega *= 0.8f;
    else if (gear != 0) {
        if (boost) limit_omega *= 1.1f;
        else if (t->flag_14B8 != 0) limit_omega *= 1.05f;
    }

    if (t->omega < limit_omega - 0.001f) {
        t->rev_limit_rpm = t->max_rpm;
        float rate_up, rate_dn;
        if (gear == 0) {
            if (t->max_rpm >= 6000.0f) { rate_up = 45.0f; rate_dn = 16.0f; }
            else                       { rate_up = 22.5f; rate_dn = 9.6f; }
            // free-revving target jitters by up to ±3%
            target *= b3_rand01(&t->rng_state, &t->rng_inc) * 0.06f + 0.97f;
        } else {
            rate_dn = 8.0f;
            rate_up = boost ? 45.0f : 15.0f;
        }
        // slew-limited approach to the target (rates are per call, the game
        // runs this at a fixed frame rate)
        const float hi = rate_up + t->omega;
        if (target <= hi) {
            const float lo = t->omega - rate_dn;
            t->omega = (lo <= target) ? target : lo;
        } else {
            t->omega = hi;
        }
        // idle floor -- note the TRUE m/s->mph constant here (2.2369363),
        // unlike the 2.2374146 used elsewhere in the drivetrain
        if (t->omega < idle_omega || speed_ms * 2.2369363f < 1.0f) {
            if (t->omega <= 0.1f + idle_omega)
                t->omega = b3_rand01(&t->rng_state, &t->rng_inc) * 16.0f
                           + idle_omega;
            if (throttle <= 0.0f && (t->gear == 1 || t->gear == -1
                                     || t->flag_14B8 != 0)) {
                t->gear = 0;               // drop to neutral at rest
                t->shifting = 1;
                t->shift_timer = 0.035f;
            }
        }
    } else {
        // rev limiter: pick a randomised slightly-lower limit, knock
        // 20 rad/s off, and turn the throttle term into engine braking
        if (t->rev_limit_rpm == t->max_rpm && !boost) {
            const double r = b3_rand01(&t->rng_state, &t->rng_inc);
            if (gear == 0) t->rev_limit_rpm = t->max_rpm - r * t->max_rpm * 0.1f;
            else           t->rev_limit_rpm = (t->max_rpm - r * 50.0f) - 70.0f;
        }
        t->omega -= 20.0f;
        throttle = -(target / limit_omega);
        if (throttle > 0.0f) throttle = 0.0f;
    }

    // torque curve, evaluated on the NEW omega:
    //   below peak:      (omega/peak + 1) / 2          (0.5 .. 1.0)
    //   peak..falloff:   1.0
    //   above falloff:   (limit-omega)/(limit-falloff) * 0.25 + 0.75
    float scale;
    const float om = t->omega;
    if (om >= t->peak_omega) {
        if (om <= t->falloff_omega) scale = 1.0f;
        else scale = ((limit_omega - om) / (limit_omega - t->falloff_omega))
                     * 0.25f + 0.75f;
    } else {
        scale = (om / t->peak_omega + 1.0f) * 0.5f;
    }
    if (kick) scale = 2.0f;
    if (t->gear == -1) scale = (limit_omega - om) / limit_omega;
    if (boost && speed_ms * 2.2369363f < max_boost_mph) {
        if (!boost_ramp_done) {
            if (boost_elapsed < t->boost_kick_time)
                scale = (1.0f - boost_elapsed / t->boost_kick_time)
                        * t->boost_kick_torque + 1.0f;
        } else {
            scale = t->boost_kick_torque + 1.0f;
        }
    }
    // output torque, with a 0.95..1.05 random flutter. The real code
    // computes this chain on the x87 stack (extended precision, one final
    // FSTP rounding); double intermediates with a single rounding mirror
    // that -- an all-float chain rounds 5 times and the 1e-6-relative
    // difference is amplified by the tyre slip feedback loop (RE_NOTES 14).
    return (float)(((double)b3_rand01(&t->rng_state, &t->rng_inc) * 0.1
                    + 0.95)
                   * (double)t->gears[t->gear + 1] * (double)t->torque
                   * (double)t->gears[8] * (double)scale
                   * (double)throttle);
}

// FUN_0011ECF0 (input stage): neutral/reverse engagement, verified by
// executing the function (6 cases incl. the composed FUN_00121560 call).
// fwd_vel is the forward velocity component the game computes from the
// velocity vector at v+0xB0 (exact frame transform not modelled; the sign
// and 0.5 / -10.5 thresholds are what the branches consume).
void b3_gear_engage(B3EngineTransmission* t, float* throttle, float* brake,
                    float fwd_vel, float speed_ms) {
    if (t->gear < 2 && t->shifting == 0) {
        int gear = t->gear;
        if (gear == -1 || *brake <= *throttle || *brake <= 0.1f
            || 0.5f <= fwd_vel) {
            if (gear == 1 || *throttle <= *brake || *throttle <= 0.1f
                || fwd_vel <= -10.5f) {
                if (gear == 0 && 0.1f < speed_ms) *brake = 1.0f;
                goto engaged;   // no change
            }
            t->gear = 1;
        } else {
            t->gear = -1;
        }
        t->shifting = 1;
        t->shift_timer = 0.35f;
    }
engaged:
    // while in reverse the game swaps the throttle and brake channels
    if (t->gear < 0) { float tmp = *throttle; *throttle = *brake; *brake = tmp; }
}

// FUN_0011D460: the stored drive torque (v+0x1520) is split 50/50 onto the
// rear wheels' torque accumulators (wheel+0x54), then every wheel integrates
//   omega (wheel+0x5C) += torque * 0.04 * dt
// and clears the accumulator. 0.04 is the compiled-in inverse wheel inertia.
void b3_drive_torque_to_wheels(float drive_torque, float dt, int wheel_count,
                               float wheel_torque[], float wheel_omega[]) {
    if (wheel_count > 2) wheel_torque[2] += drive_torque * 0.5f;
    if (wheel_count > 3) wheel_torque[3] += drive_torque * 0.5f;
    for (int i = 0; i < wheel_count; i++) {
        wheel_omega[i] += wheel_torque[i] * 0.04f * dt;
        wheel_torque[i] = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Suspension solver -- FUN_00123FD0 (with pre-pass FUN_001239C0).
// Wheel record: base v+0x820, stride 0xC0. Field map recovered by executing
// both functions (tools/validate_port.py, suspension section):
//   +0x00 world position      +0x10 contact point     +0x20 contact normal
//   +0x30 prev-frame copy of +0x00..+0x1C (pre-pass)  +0x50 pre-pass height
//   +0x58 spin angle          +0x5C angular velocity
//   +0x60 previous spring length   +0x64 current spring length (pre-pass out)
//   +0x74 attach height (config)   +0xB2 bump flag    +0xB3 contact flag
// Config lives at v+0xCA0..0xCBC (front/rear attach, damping, force, length --
// copied from the 0x1D0 struct by FUN_00134710).
// ---------------------------------------------------------------------------

// Per-wheel spring/damper force, FUN_00123FD0 contact path (0x001240xx).
//   comp = attach - cur, clamped to [0.25*len, in_race ? 0.75*len : inf)
//   F = -(comp - len)*k + ((cur' - prev)/dt)*c
//   in_race && comp/(0.75 len) > 0.5:  F *= (1 - comp/(0.75 len)) * 2
// (constants 0.75/0.5/2.0/1.0 at 0x3A55F8/0x3B1684/0x3B1688/0x3B168C).
// cur is only rewritten by the 0.25*len clamp; prev always becomes cur'.
// bump_flag (wheel+0xB2) = (cur - prev > 0.12), tested BEFORE any clamp.
// NOT modelled here (located, unported): the bottom-out block that runs when
// the clamp cuts more than 0.001 -- it accumulates normal*(0.25len - comp)
// and feeds an impulse solver (FUN_001066a0/FUN_00106720); the validate cases
// stay outside it. The DAT_004D617E dt*1.2 alternate is also not modelled.
float b3_wheel_spring_damper(float k, float c, float len, float attach,
                             float* cur, float* prev, float dt,
                             int in_race, int* bump_flag) {
    *bump_flag = (0.12f < *cur - *prev) ? 1 : 0;
    float comp = attach - *cur;
    const float lo = len * 0.25f;
    if (lo <= comp) {
        if (in_race && len * 0.75f < comp)
            comp = len * 0.75f;                 // extension clamp (in race)
    } else {
        comp = lo;                              // compression clamp
        *cur = attach - lo;                     // and the stored length moves
    }
    const float vel = (*cur - *prev) / dt;      // damper uses the CLAMPED cur
    *prev = *cur;
    float f = -((comp - len) * k) + vel * c;
    if (in_race) {
        const float ratio = comp / (len * 0.75f);
        if (0.5f < ratio) f *= (1.0f - ratio) * 2.0f;  // fades to 0 at droop
    }
    return f;
}

// Force application, FUN_00123FD0 + FUN_00106590: the scalar force acts along
// the contact normal; torque is r x F about the frame origin (+0x204 row 3),
// with r = wheel world position. Accumulators: force v+0xF0, torque v+0x100.
void b3_wheel_force_apply(const float normal[4], float f,
                          const float wheel_pos[4], const float frame_pos[4],
                          float force_acc[4], float torque_acc[4]) {
    float fv[4];
    for (int i = 0; i < 4; i++) {
        fv[i] = normal[i] * f;
        force_acc[i] += fv[i];
    }
    const float rx = wheel_pos[0] - frame_pos[0];
    const float ry = wheel_pos[1] - frame_pos[1];
    const float rz = wheel_pos[2] - frame_pos[2];
    torque_acc[0] += ry * fv[2] - rz * fv[1];
    torque_acc[1] += rz * fv[0] - rx * fv[2];
    torque_acc[2] += rx * fv[1] - ry * fv[0];
    // w: the game computes rw*fw - rw*fw == 0; kept for the 4-wide layout
}

// No-contact droop, FUN_00123FD0 (0x001240a8..): the stored spring length
// relaxes to attach - 0.75*len. The decompiled rate term
//   t = clamp(1 - len / (attach - (attach - 0.75 len)), 0, 1);  cur -= t*k*dt^3
// is dead in practice: the denominator is 0.75*len, so t = 1 - 1/0.75 < 0
// clamps to 0 for every len. Mirrored exactly anyway, including the final
// re-clamp to [0.25*len, 0.75*len] below attach.
float b3_wheel_droop(float k, float len, float attach, float dt) {
    const float hi = len * 0.75f;
    float cur = attach - hi;
    float t = 1.0f - len / (attach - (attach - hi));
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    cur = cur - t * k * dt * dt * dt;
    const float ext = attach - cur;
    const float lo = len * 0.25f;
    if (ext < lo) cur = attach - lo;
    else if (hi < ext) cur = attach - hi;
    return cur;
}

// Wheel spin integration, FUN_00123FD0 tail (0x00124d30..): free rolling
// decay 0.99 per call, a further 0.8 while the tyre has ground contact, then
// the spin angle integrates the NEW omega and wraps at +/-200*pi (628.31854).
// The sin/cos of the wrapped angle feed the visual wheel matrix (not ported).
void b3_wheel_spin_update(float* spin, float* omega, int decay_enabled,
                          int contact, float dt) {
    if (decay_enabled) {
        const float w0 = *omega;
        *omega = w0 * 0.99f;
        if (contact) *omega = w0 * 0.99f * 0.8f;
    }
    float s = dt * *omega + *spin;
    if (s > 628.31854f) s -= 628.31854f;
    else if (s < -628.31854f) s += 628.31854f;
    *spin = s;
}

// Pre-pass airborne path, FUN_001239C0 (LAB_00123e01): when the wheel ray
// finds no ground (or surface type 3), the contact flag clears and the
// contact point/normal are synthesised from the frame's up axis:
//   contact = prev_world_pos - up * h     (h = wheel+0x50)
//   normal  = up
// The pre-pass also copies wheel +0x00..+0x1C to +0x30..+0x4C first (the
// prev-frame snapshot) -- done by the caller in the real code path.
void b3_wheel_prepass_airborne(const float up[4], float h,
                               const float pos_prev[4],
                               float contact_pt[4], float normal[4]) {
    for (int i = 0; i < 4; i++) {
        contact_pt[i] = pos_prev[i] - up[i] * h;
        normal[i] = up[i];
    }
}

// ---------------------------------------------------------------------------
// Tyre grip -- FUN_0011D460's per-wheel force loop (0x0011ddf0..0x0011e675).
// This was the last big unrecovered physics block ("what makes drift/handling
// faithful"). Everything below is transcribed from the disassembly and
// verified by executing the real function under Unicorn with a fully seeded
// environment (tools/validate_port.py, tyre section, all green).
// The wheel basis: front wheels use the frame rotated about local up by the
// steering angle v+0x1164 degrees (FUN_00011900 axis-angle + FUN_000116e0
// multiply, built once per frame into v+0x14E0); rears use the frame itself.
// ---------------------------------------------------------------------------

// The sine-curve friction term is normalised so its peak is 1.0 by the BSS
// constant at 0x005a8054, seeded by the static-init stub at 0x002ba3c0 from
// the float at 0x3b2338. 0.9210610 = cos(0.4), 0.3894183 = sin(0.4): the
// curve is the gap between sin(x) and its tangent line at x = 0.4, with
// x = 0.4 - 0.8 * min(1, |slip ratio|)  in [-0.4, 0.4].
#define B3_TYRE_CURVE_NORM 23.8164005279541f   // [0x3b2338] -> 0x005a8054

void b3_tyre_grip(int front_axle, float throttle, float speed_ms,
                  float mass_kg, float vlat, float vlong, float roll_speed,
                  float* out_flat, float* out_flong) {
    const float load = mass_kg * 5.0f;         // per-wheel load surrogate
    const float slip_long = vlong - roll_speed;
    float slip = sqrtf(vlat * vlat + slip_long * slip_long);
    if (roll_speed == 0.0f) roll_speed = 1e-06f;
    if (slip == 0.0f) slip = 1e-07f;           // 0x33d6bf95 exactly
    float denom_k, curve, stiff;
    if (front_axle) {
        denom_k = 60.0f;
        curve = (1.0f - throttle) * 2.5f + 2.3f;
        stiff = 150000.0f;
    } else {
        float t = speed_ms * 0.025f;
        if (t < 0.5f) t = 0.5f;
        else if (t > 1.0f) t = 1.0f;
        denom_k = 12.0f;
        curve = t * ((1.0f - throttle) * 7.0f + 6.0f);
        stiff = t * 500000.0f;
    }
    float sr = fabsf((stiff * slip) / (denom_k * roll_speed * load));
    if (sr > 1.0f) sr = 1.0f;
    const float x = 0.4f - sr * 0.8f;
    const float term1 = ((x - 0.4f) * 0.9210610f + (0.3894183f - sinf(x)))
                        * curve * load * B3_TYRE_CURVE_NORM;
    const float term2 = ((x + 0.4f) / roll_speed) * stiff * slip * 0.5f;
    const float scal = (-1.0f / slip) * (term1 + term2);
    *out_flat = vlat * scal;
    *out_flong = slip_long * scal;
}

// Wheel reaction (same loop): torque -= F_long * radius, then
// omega += torque * 0.04 * dt and the accumulator clears. Front wheels are
// free-rolling: the loop sets omega = v_long / radius BEFORE this (using the
// OLD omega for roll_speed).
void b3_tyre_wheel_reaction(float flong, float radius, float dt,
                            float* wheel_torque, float* wheel_omega) {
    *wheel_torque += -(flong * radius);
    *wheel_omega += *wheel_torque * 0.04f * dt;
    *wheel_torque = 0.0f;
}

// ---------------------------------------------------------------------------
// Airborne attitude dampers -- FUN_0011D460 airborne branch (0x0011d75b..).
// Four compiled-in records damp the point velocity at two fore/aft stations:
//   { pos (0,0, half_1d8*0.5), axis up,    gain 1000 }
//   { pos (0,0, half_1d8*0.5), axis right, gain 10 }
//   { pos (0,0, half_1e8*2.0), axis up,    gain 1000 }
//   { pos (0,0, half_1e8*2.0), axis right, gain 10 }
// (gains at 0x3b16cc / 0x3a7f34; z negated when dot(at, vel) < 0, i.e.
// reversing). F = -gain * dot(axis_world, v + omega x r) along axis_world,
// applied at the record's world point. With the synthetic identity frame and
// zero offsets this is exactly the "-20.0 * dir.x" term recorded in
// RE_NOTES 8.2 as unexplained: 2 right-axis records x gain 10 x vel.x.
// ---------------------------------------------------------------------------
static void b3_xform_point(const float m[4][4], const float v[4],
                           float out[4]) {
    for (int i = 0; i < 4; i++)                   // FUN_00013ca0
        out[i] = v[0]*m[0][i] + m[3][i] + v[1]*m[1][i] + v[2]*m[2][i];
}

static void b3_rot_vec(const float m[4][4], const float v[4], float out[4]) {
    for (int i = 0; i < 4; i++)
        out[i] = v[0]*m[0][i] + v[1]*m[1][i] + v[2]*m[2][i];
}

static float b3_dot3(const float a[4], const float b[4]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

void b3_airborne_damper(const float frame[4][4], const float vel4[4],
                        const float omega[4], float half_1d8, float half_1e8,
                        float force_acc[4], float torque_acc[4]) {
    static const float axes[2][4] = {{0.f, 1.f, 0.f, 0.f},
                                     {1.f, 0.f, 0.f, 0.f}};
    static const float gains[2] = {1000.0f, 10.0f};
    const float z[2] = {half_1d8 * 0.5f, half_1e8 * 2.0f};
    const int flip = b3_dot3(vel4, frame[2]) < 0.0f;
    for (int s = 0; s < 2; s++) {
        for (int a = 0; a < 2; a++) {
            float local[4] = {0.0f, 0.0f, flip ? -z[s] : z[s], 0.0f};
            float wp[4], aw[4], r[3], vp[4];
            b3_xform_point(frame, local, wp);
            b3_rot_vec(frame, axes[a], aw);
            for (int i = 0; i < 3; i++) r[i] = wp[i] - frame[3][i];
            vp[0] = omega[1]*r[2] - omega[2]*r[1] + vel4[0];
            vp[1] = omega[2]*r[0] - omega[0]*r[2] + vel4[1];
            vp[2] = omega[0]*r[1] - omega[1]*r[0] + vel4[2];
            vp[3] = vel4[3];
            const float sc = -(gains[a] * b3_dot3(aw, vp));
            float F[4];
            for (int i = 0; i < 4; i++) {
                F[i] = aw[i] * sc;
                force_acc[i] += F[i];
            }
            torque_acc[0] += r[1]*F[2] - r[2]*F[1];
            torque_acc[1] += r[2]*F[0] - r[0]*F[2];
            torque_acc[2] += r[0]*F[1] - r[1]*F[0];
        }
    }
}

// Gravity body pin -- FUN_0011D460 tail (0x0011ec55). DAT_0040a8a0 is the
// gravity vector (0, -20, 0, 0); FUN_00109560 applies it world-vertical, and
// this block converts 20*mass of it to the body's -up axis while grounded:
// on a banked road the car is pressed into the surface instead of straight
// down. Gate: speed > 1 && frame at.y (offset +0x24) > 0.
void b3_gravity_body_pin(float mass_kg, float speed_ms, float at_y,
                         const float up[4], float force_acc[4]) {
    if (speed_ms > 1.0f && at_y > 0.0f) {
        force_acc[1] -= -20.0f * mass_kg;
        const float s = mass_kg * -20.0f;
        for (int i = 0; i < 4; i++)
            force_acc[i] += up[i] * s;
    }
}

// Brake / engine-brake force -- FUN_0011D460 (0x0011dbd7..0x0011dcf7).
// Applied along the unit travel direction (+0xC0) at pos + up*brake_height
// (frame origin while drifting). 20000 at 0x3a35e4, 1/70 at 0x3b1d8c.
float b3_brake_drag_scalar(float engine_braking, float braking_factor,
                           float throttle, float brake, float speed_ms,
                           int drift_state, float drift_time) {
    const int drifting = (drift_state == 1 || drift_state == 2);
    const float gate = (drifting || throttle > 0.1f) ? 0.0f : 1.0f;
    float sc = (engine_braking * gate - braking_factor * brake * 20000.0f)
               * (speed_ms + 1.0f) * 0.014285714f;
    if (drifting && drift_time < 0.3f) sc = 0.0f;
    return sc;
}

// Drift-mode lateral blend -- FUN_0011D460 (0x0011e355..): while sliding the
// per-wheel lateral force is faded toward a scripted magnitude
//   mag = (1 - slide*2/3) * (dot(dir, at)*6400 + 4200)
// signed like the tyre force and ramped in over the first second of drift.
float b3_drift_lateral_blend(float flat, float slide_1440, float drift_time,
                             float dir_dot_at) {
    float mag = (1.0f - slide_1440 * 0.6666667f)
                * (dir_dot_at * 6400.0f + 4200.0f);
    if (!(flat > 0.0f)) mag = -mag;
    if (drift_time < 1.0f) mag *= drift_time;
    return flat * (1.0f - slide_1440) + mag * slide_1440;
}

// Drift-mode yaw torque -- FUN_0011D460 (0x0011e83d..0x0011ea24): while in
// drift state the up-axis component of the wheel loop's torque is REPLACED by
// a steering-scripted value (Turn rate * steer * |steer| * -10000 against the
// slide, -5000 with it, +/-1000 fallbacks scaled by v+0x1414).
float b3_drift_yaw_torque(int state, float steer, float v_1414,
                          float slide_1440, float turn_rate,
                          float cos_max_drift, float dir_dot_right) {
    const float base = (1.0f - slide_1440 * 0.75f) * turn_rate
                       * steer * fabsf(steer);
    if (state == 2) {
        if (-0.05f > steer) return base * -10000.0f;
        if (cos_max_drift > dir_dot_right)
            return (steer > 0.05f) ? base * -5000.0f : v_1414 * 1000.0f;
        return 1000.0f;
    }
    if (state == 1) {
        if (steer > 0.05f) return base * -10000.0f;
        if (dir_dot_right > -cos_max_drift)
            return (-0.05f > steer) ? base * -5000.0f : v_1414 * -1000.0f;
        return -1000.0f;
    }
    return 0.0f;   // state 0: the wheel-loop torque passes through unchanged
}

// The cap on that yaw torque (0x0011ea24..0x0011eaf8): 800 below 60 mph,
// blending to Turn momentum (fast->slow) from 60 to 110 mph; reduced during
// the first 0.25 s of the drift. Torque beyond the cap (compared against the
// up-axis angular momentum) is cut to 10%.
float b3_yaw_torque_cap(float speed_mph, float turn_momentum_slow,
                        float turn_momentum_fast, float drift_time) {
    float cap;
    if (speed_mph < 60.0f) cap = 800.0f;
    else if (speed_mph < 110.0f) {
        const float t = (speed_mph - 60.0f) * 0.02f;
        cap = turn_momentum_slow * t + (1.0f - t) * turn_momentum_fast;
    } else {
        cap = turn_momentum_fast;
    }
    if (drift_time < 0.25f) cap *= drift_time + 0.75f;
    return cap;
}

// Steering scheduler -- FUN_0011ECF0 (0x0011f1e2..): the steering angle in
// degrees shrinks 1.4 deg per m/s from the base at +0x1384 (config Steer max
// velocity, offset by +0x1380 = Steer min velocity), clamped to the
// min/max angle window. Verified by executing FUN_0011ECF0.
float b3_steer_schedule(float speed_ms, float live_1384, float live_1380,
                        float steer_min_angle, float steer_max_angle) {
    float ang = live_1384 - (speed_ms - live_1380) * 1.4f;
    if (ang < steer_min_angle) ang = steer_min_angle;
    if (steer_max_angle < ang) ang = steer_max_angle;
    return ang;
}

// Steering slew -- same function: the live steering input at +0x1408 moves
// toward the raw input by at most steer_response (+0x1388) per frame.
// The final angle written to +0x1164 is -(steer * schedule).
float b3_steer_slew(float prev, float raw, float steer_response) {
    const float d = raw - prev;
    if (raw <= prev) {
        if (d < -steer_response) return prev - steer_response;
    } else {
        if (d > steer_response) return prev + steer_response;
    }
    return raw;
}

// ---------------------------------------------------------------------------
// Rigid-body integration -- FUN_00109560, ported 1:1 (validate_port.py,
// integrator section, all green). Helpers first; all are the game's own
// routines, not textbook reconstructions.
// ---------------------------------------------------------------------------
static float b3_normalize4(float v[4]) {       // FUN_0002c0d0 / FUN_00011640
    const float ln = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    const float s = 1.0f / ln;
    for (int i = 0; i < 4; i++) v[i] *= s;
    return ln;
}

static void b3_cross4(const float a[4], const float b[4], float out[4]) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
    out[3] = 0.0f;
}

// FUN_000ff270. The intended dot-product row selector calls a function that
// is a no-op stub in the retail binary (FUN_00013c60), so the selection reads
// stale stack; traced from FUN_00109560's call site the surviving path is
// at-row anchored: row1 = norm(row2 x row0), row0 = norm(row1 x row2).
/* FUN_000FF270 [C] IN FULL (@0x000FF27D..0x000FF544): normalise all three
 * rows, then keep the MOST-ORTHOGONAL pair and rebuild the other two.
 * The old port hard-coded branch B -- the branch a racecar's frame always
 * takes (validate_port stayed green) -- but a fast-tumbling body (wreck,
 * panel piece, prop) takes A and C every few frames, and the wrong branch
 * feeds energy into the spin (the PROP-PHYSICS wave's finding; suspected
 * root of wreck omega anomalies). Branch table:
 *   a=|r2.r1| b=|r0.r2| c=|r1.r0|
 *   b>a && c>a  -> A: r0=^(r1xr2), r2=^(r0xr1)   @0x000FF332
 *   b>a && c<=a -> C: r2=^(r0xr1), r1=^(r2xr0)   @0x000FF37C
 *   b<=a && c<=b-> C                             @0x000FF3D3
 *   b<=a && c>b -> B: r1=^(r2xr0), r0=^(r1xr2)   @0x000FF3D5
 *   degenerate: L0<=0 -> A, L1<=0 -> B, L2<=0 -> C */
void b3_mat_orthonormalize(float m[4][4]) {
    float L0 = sqrtf(m[0][0]*m[0][0] + m[0][1]*m[0][1] + m[0][2]*m[0][2]
                     + m[0][3]*m[0][3]);
    float L1 = sqrtf(m[1][0]*m[1][0] + m[1][1]*m[1][1] + m[1][2]*m[1][2]
                     + m[1][3]*m[1][3]);
    float L2 = sqrtf(m[2][0]*m[2][0] + m[2][1]*m[2][1] + m[2][2]*m[2][2]
                     + m[2][3]*m[2][3]);
    b3_normalize4(m[0]);
    b3_normalize4(m[1]);
    b3_normalize4(m[2]);
    int br;                                   /* 0 = A, 1 = B, 2 = C */
    if (L0 <= 0.0f)      br = 0;
    else if (L1 <= 0.0f) br = 1;
    else if (L2 <= 0.0f) br = 2;
    else {
        float a = fabsf(m[2][0]*m[1][0] + m[2][1]*m[1][1] + m[2][2]*m[1][2]);
        float b = fabsf(m[0][0]*m[2][0] + m[0][1]*m[2][1] + m[0][2]*m[2][2]);
        float c = fabsf(m[1][0]*m[0][0] + m[1][1]*m[0][1] + m[1][2]*m[0][2]);
        if (b > a) br = (c > a) ? 0 : 2;
        else       br = (c <= b) ? 2 : 1;
    }
    float t[4];
    if (br == 0) {
        b3_cross4(m[1], m[2], t);
        for (int i = 0; i < 4; i++) m[0][i] = t[i];
        b3_normalize4(m[0]);
        b3_cross4(m[0], m[1], t);
        for (int i = 0; i < 4; i++) m[2][i] = t[i];
        b3_normalize4(m[2]);
    } else if (br == 1) {
        b3_cross4(m[2], m[0], t);
        for (int i = 0; i < 4; i++) m[1][i] = t[i];
        b3_normalize4(m[1]);
        b3_cross4(m[1], m[2], t);
        for (int i = 0; i < 4; i++) m[0][i] = t[i];
        b3_normalize4(m[0]);
    } else {
        b3_cross4(m[0], m[1], t);
        for (int i = 0; i < 4; i++) m[2][i] = t[i];
        b3_normalize4(m[2]);
        b3_cross4(m[2], m[0], t);
        for (int i = 0; i < 4; i++) m[1][i] = t[i];
        b3_normalize4(m[1]);
    }
}

// FUN_00040ae0: invert a rigid transform in place (transpose the 3x3,
// pos = -(pos . new rows)).
static void b3_mat_invert_rigid(float m[4][4]) {
    float t;
    t = m[0][1]; m[0][1] = m[1][0]; m[1][0] = t;
    t = m[0][2]; m[0][2] = m[2][0]; m[2][0] = t;
    t = m[1][2]; m[1][2] = m[2][1]; m[2][1] = t;
    float p[4];
    for (int j = 0; j < 4; j++)
        p[j] = m[3][0]*m[0][j] + m[3][1]*m[1][j] + m[3][2]*m[2][j];
    for (int j = 0; j < 4; j++) m[3][j] = -p[j];
}

// FUN_00109040: out_row_i = B_i.x*A0 + B_i.y*A1 + B_i.z*A2 (3 rows).
static void b3_mat_mul3(const float A[3][4], const float B[3][4],
                        float out[3][4]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            out[i][j] = B[i][0]*A[0][j] + B[i][1]*A[1][j] + B[i][2]*A[2][j];
}

void b3_rigid_body_integrate(B3RigidBody* rb, float mass_kg, float com_height,
                             int in_race, int state6, float dt) {
    // gravity: DAT_0040a8a0 = (0, -20, 0, 0), scaled by (1, mass, 1, -)
    const float g[4] = {0.0f, -20.0f * mass_kg, 0.0f, 0.0f};
    if (!in_race && !state6) {
        for (int i = 0; i < 4; i++) rb->force_acc[i] += g[i];
    } else {
        // application point raised by com_height along up: gravity torque
        float pt[4], r[3];
        for (int i = 0; i < 4; i++) {
            pt[i] = rb->frame[1][i] * com_height + rb->frame[3][i];
            rb->force_acc[i] += g[i];
        }
        for (int i = 0; i < 3; i++) r[i] = pt[i] - rb->frame[3][i];
        rb->torque_acc[0] += r[1]*g[2] - r[2]*g[1];
        rb->torque_acc[1] += r[2]*g[0] - r[0]*g[2];
        rb->torque_acc[2] += r[0]*g[1] - r[1]*g[0];
    }
    // impulse accumulation and velocity update (4-wide in the game; the
    // w-lane dt slot is an uninitialised stack dword -- mirrored as 0)
    const float dtv[4] = {dt, dt, dt, 0.0f};
    float vel4[4] = {rb->vel[0], rb->vel[1], rb->vel[2], rb->vel[3]};
    for (int i = 0; i < 4; i++) {
        rb->imp_force[i] += rb->force_acc[i] * dtv[i];
        rb->imp_torque[i] += rb->torque_acc[i] * dtv[i];
        rb->force_acc[i] = 0.0f;
        rb->torque_acc[i] = 0.0f;
    }
    for (int i = 0; i < 4; i++) {
        vel4[i] += rb->imp_force[i] / mass_kg;
        rb->imp_force[i] = 0.0f;
    }
    if (vel4[1] > 120.0f) vel4[1] = 120.0f;   // vertical speed cap
    // angular momentum and omega = L . M (M = world inverse inertia rows)
    for (int i = 0; i < 4; i++) {
        rb->angmom[i] += rb->imp_torque[i];
        rb->imp_torque[i] = 0.0f;
    }
    for (int j = 0; j < 4; j++)
        rb->omega[j] = rb->angmom[0] * rb->inv_inertia_world[0][j]
                     + rb->angmom[1] * rb->inv_inertia_world[1][j]
                     + rb->angmom[2] * rb->inv_inertia_world[2][j];
    const float w2 = b3_dot3(rb->omega, rb->omega);
    if (w2 > 10000.0f) {
        // |omega| > 100: normalise, squash to 100/|omega|, damp L by 0.95
        b3_normalize4(rb->omega);
        const float s = (1.0f / sqrtf(w2)) * 100.0f;
        for (int i = 0; i < 4; i++) rb->omega[i] *= s;
        for (int i = 0; i < 4; i++) rb->angmom[i] *= 0.95f;
    }
    // pose update: pos += vel*dt, rows -= row x (omega*dt), orthonormalise
    float r[4];
    for (int i = 0; i < 4; i++) {
        rb->frame[3][i] += vel4[i] * dtv[i];
        r[i] = rb->omega[i] * dtv[i];
    }
    for (int row = 2; row >= 0; row--) {
        float c[4];
        b3_cross4(rb->frame[row], r, c);
        for (int i = 0; i < 4; i++) rb->frame[row][i] -= c[i];
    }
    b3_mat_orthonormalize(rb->frame);
    for (int i = 0; i < 4; i++) {
        rb->frame[3][i] += rb->deflection[i];
        rb->deflection[i] = 0.0f;
    }
    // world inverse inertia = Rt . I0 . R (FUN_00109040 twice)
    float Rt[3][4], tmp[3][4];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            Rt[i][j] = rb->frame[j][i];
    for (int i = 0; i < 3; i++) Rt[i][3] = 0.0f;
    b3_mat_mul3(rb->inv_inertia_body, Rt, tmp);
    b3_mat_mul3(rb->frame, tmp, rb->inv_inertia_world);
    // +0x70 = inverse frame transform (FUN_00040ae0)
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            rb->inv_frame[i][j] = rb->frame[i][j];
    b3_mat_invert_rigid(rb->inv_frame);
    // FUN_000ffc80: refresh speed and the unit travel direction
    for (int i = 0; i < 3; i++) rb->vel[i] = vel4[i];
    rb->vel[3] = vel4[3];
    const float ls = b3_dot3(vel4, vel4);
    if (ls < 2.3283064e-10f) {
        rb->vel[3] = 0.0f;
        for (int i = 0; i < 4; i++) rb->dir[i] = rb->frame[2][i];
    } else {
        for (int i = 0; i < 4; i++) rb->dir[i] = vel4[i];
        rb->vel[3] = b3_normalize4(rb->dir);
    }
}

// ===========================================================================
// FULL PIPELINE -- FUN_0011BE50 main path + FUN_0011ECF0, composed 1:1 from
// the decompiled stages over the real struct layout. Verified against the
// real code running multi-frame under Unicorn (tools/emulate_pipeline.py /
// validate_port.py full-pipeline section). Evidence per stage: RE_NOTES 14.
// ===========================================================================

// -- small helpers shared by the pipeline stages ----------------------------

// FUN_00013ca0: world = local.x*row0 + row3 + local.y*row1 + local.z*row2
static void b3f_xform_point(const float m[4][4], float x, float y, float z,
                            float out[4]) {
    for (int i = 0; i < 4; i++)
        out[i] = x * m[0][i] + m[3][i] + y * m[1][i] + z * m[2][i];
}

// FUN_001066a0: point velocity = omega x (pt - pos) + vel (w = speed)
static void b3f_point_vel(const B3RigidBody* rb, const float pt[4],
                          float out[4]) {
    float r[3];
    for (int i = 0; i < 3; i++) r[i] = pt[i] - rb->frame[3][i];
    out[0] = rb->omega[1] * r[2] - rb->omega[2] * r[1] + rb->vel[0];
    out[1] = rb->omega[2] * r[0] - rb->omega[0] * r[2] + rb->vel[1];
    out[2] = rb->omega[0] * r[1] - rb->omega[1] * r[0] + rb->vel[2];
    out[3] = rb->vel[3];
}

// FUN_00106590: torque += (pt - frame_pos) x F
static void b3f_torque_at(const B3RigidBody* rb, const float pt[4],
                          const float F[4], float acc[4]) {
    float r[3];
    for (int i = 0; i < 3; i++) r[i] = pt[i] - rb->frame[3][i];
    acc[0] += r[1] * F[2] - r[2] * F[1];
    acc[1] += r[2] * F[0] - r[0] * F[2];
    acc[2] += r[0] * F[1] - r[1] * F[0];
}

static void b3f_acc_add(float acc[4], const float F[4]) {
    for (int i = 0; i < 4; i++) acc[i] += F[i];
}

// FUN_00011900 with axis (0,1,0) composed with the frame (FUN_000116e0):
// the steered wheel basis rows (right and at) cached at v+0x14E0/+0x1500.
static void b3f_steered_basis(const float m[4][4], float deg,
                              float right[4], float at[4]) {
    float a = deg * 0.017453292f;
    float s = sinf(a), c = cosf(a), t = 1.0f - c;
    float rot[3][4] = {{1.0f - t, 0.0f, -s, 0.0f},
                       {0.0f, 1.0f, 0.0f, 0.0f},
                       {s, 0.0f, 1.0f - t, 0.0f}};
    for (int j = 0; j < 4; j++) {
        right[j] = rot[0][0] * m[0][j] + rot[0][1] * m[1][j]
                 + rot[0][2] * m[2][j];
        at[j] = rot[2][0] * m[0][j] + rot[2][1] * m[1][j]
              + rot[2][2] * m[2][j];
    }
}

// ---------------------------------------------------------------------------
// Aggressive driving reaction ("steer away") -- FUN_0011ECF0
// 0x0011ED17..0x0011EF29, transcribed branch for branch.
//
//   byte +0x215 == 3      -> restore the config trio behind +0x13F8
//                            (+0x14C/+0x154/+0x190 -> +0x137C/+0x1384/+0x13C0)
//   otherwise             -> two out-of-control stamps on the owner's
//                            +0x1198 object are tested against the clock:
//     +0x1598 (slam)        full Steer Away Time / Total OOC Time windows
//     +0x1690 (wall/spin)   both windows x0.6, decay target x0.8
//   inside Total OOC (and with no chassis contact byte +0x212):
//     first Steer-Away-Time seconds: +0x137C := the aggressive max angle
//       (read FRESH -- the 0.8 wall-event scaling does NOT apply here) and
//       the steering input +0x1408 is forced to +/-1 by byte +0x153C
//       (0x0011F309);
//     after that: +0x137C lerps from the (possibly x0.8) aggressive angle
//       back to the config max angle across the rest of the window;
//     either way +0x1384 := +0x13EC and +0x13C0 := +0x13F0.
//
// RETAIL QUIRK [C]: the window/phase tests read the stamps through
// owner+0x1198 while the phase-2 lerp reads owner+0x1598/+0x1690/+0x10DC
// DIRECTLY. The game points racecar+0x1198 at the racecar itself, so both
// resolve to the same words; this port keeps one copy of each.
// Constants: 0.6 [0x003B16EC], 0.8 [0x003A5600], 1.0 [0x003B168C],
// -1.0 [0x003B16C0].
// ---------------------------------------------------------------------------
void b3_steer_away_envelope(B3VehicleFull* v, B3SteerAway* out) {
    float t_sa = v->aggr_time_13E0;     // xmm1
    float t_oo = v->aggr_total_13E4;    // xmm0
    float ang  = v->aggr_angle_13E8;    // xmm2 (the DECAY target only)
    const float now = v->clock;
    out->window = 0;
    out->phase1 = 0;
    out->event2 = 0;

    if (v->class_215 == 3) goto restore;          // 0x0011ED69

    if (!(v->ooc_slam_1598 < 0.0f)
        && !(now > v->ooc_slam_1598 + t_oo)) {
        out->window = 1;                          // LAB_0011EDF2
    } else {
        t_sa *= 0.6f;                             // 0x0011EDB2
        t_oo *= 0.6f;
        if (!(v->ooc_wall_1690 < 0.0f)
            && !(now > v->ooc_wall_1690 + t_oo)) {
            out->event2 = 1;                      // 0x0011EDE9
            ang *= 0.8f;
            out->window = 1;
        }
    }

    if (v->contact_212 == 0) {                    // 0x0011EDF7
        const float t = out->event2 ? v->ooc_wall_1690 : v->ooc_slam_1598;
        if (!(t < 0.0f) && !(now > t + t_sa)) out->phase1 = 1;  // 0x0011EE62
    }

    if (!out->window || v->contact_212 != 0) goto restore;      // 0x0011EE6D

    if (out->phase1) {
        v->steer_max_137C = v->aggr_angle_13E8;   // 0x0011EE83, unscaled
    } else {
        const float t = out->event2 ? v->ooc_wall_1690 : v->ooc_slam_1598;
        const float frac = (now - (t + t_sa)) / (t_oo - t_sa);
        v->steer_max_137C = (ang - v->cfg_steer_max_14C) * (1.0f - frac)
                          + v->cfg_steer_max_14C;
    }
    v->steer_base_1384 = v->aggr_vel_13EC;        // 0x0011EEF1
    v->kquad_13C0 = v->aggr_drag_13F0;
    return;

restore:                                          // LAB_0011EF05
    v->steer_max_137C  = v->cfg_steer_max_14C;
    v->steer_base_1384 = v->cfg_steer_maxvel_154;
    v->kquad_13C0      = v->cfg_steer_drag_190;
}

// ---------------------------------------------------------------------------
// Input stage -- FUN_0011ECF0 (racing path: racecar +0x179C == 1).
// Steering-parameter select (the steer-away envelope above), flag_b
// override, gear engage (note the retail FUN_00013c60 no-op stub leaves
// fwd = vel.x), reverse control swap, steering schedule/slew + the forced
// out-of-control lock, engine update, drive-torque store + boost shaping +
// top-speed cut, slide schedule, auto-drift state machine.
// ---------------------------------------------------------------------------
static void b3_input_stage(B3VehicleFull* v, float dt) {
    B3EngineTransmission* t = &v->trans;
    const float speed = v->rb.vel[3];

    // steering-parameter select: config restore (byte 0x215 == 3) or the
    // aggressive-driving-reaction replacement. Rewrites 0x137C/0x1384/0x13C0.
    B3SteerAway sa;
    b3_steer_away_envelope(v, &sa);

    // flag_b block (racecar +0x179C == 1 path)
    if (v->flag_b_1446) {
        v->brake_1404 = 0.0f;
        v->throttle_1400 = 1.0f;
        v->throttle_raw_1414 = 1.0f;
        // 0x0011EF72: the boost-start latch expires 3 s after the owner's
        // +0x1350 stamp, for every class except 3.
        if (v->class_215 != 3 && 3.0f < v->clock - v->launch_time_1350)
            v->flag_b_1446 = 0;
    }

    // gear engage (gear < 2, not mid-shift). Retail quirk: the forward
    // velocity is vel.x -- the intended projection calls FUN_00013c60,
    // a no-op stub in the shipped binary, leaving the raw +0xB0 lane.
    if (t->gear < 2 && t->shifting == 0) {
        int gear = t->gear;
        float fwd = v->rb.vel[0];
        if (gear == -1 || v->brake_1404 <= v->throttle_1400
            || v->brake_1404 <= 0.1f || 0.5f <= fwd || v->flag_b_1446) {
            if (gear == 1 || v->throttle_1400 <= v->brake_1404
                || v->throttle_1400 <= 0.1f || fwd <= -10.5f) {
                if (gear == 0 && 0.1f < speed) v->brake_1404 = 1.0f;
                goto engaged;
            }
            t->gear = 1;
        } else {
            t->gear = -1;
        }
        t->shifting = 1;
        t->shift_timer = 0.35f;
    }
engaged:
    // manual-shift latch block: gated by +0x14B8 (flag_14B8 == 0 here)

    // reverse: swap throttle/brake channels
    if (t->gear < 0) {
        float tmp = v->brake_1404;
        v->brake_1404 = v->throttle_1400;
        v->throttle_1400 = tmp;
    }

    // steering schedule + slew (b3_steer_schedule/b3_steer_slew, verified)
    {
        float ang = v->steer_base_1384 - (speed - v->steer_v0_1380) * 1.4f;
        if (ang < v->steer_min_1378) ang = v->steer_min_1378;
        if (v->steer_max_137C < ang) ang = v->steer_max_137C;
        v->steer_1408 = b3_steer_slew(v->steer_prev_1424, v->steer_1408,
                                      v->steer_resp_1388);
        // 0x0011F309: inside the steer-away phase the slewed input is
        // OVERWRITTEN with full lock, signed by the hit-side byte +0x153C
        // (+1.0 [0x003B168C] when set, -1.0 [0x003B16C0] when clear).
        if (sa.window && sa.phase1)
            v->steer_1408 = v->hit_side_153C ? 1.0f : -1.0f;
        v->steer_deg_1164 = 0.0f - v->steer_1408 * ang;
    }

    // engine update (FUN_00121560): wheel omega arg = max of the rear pair
    {
        float womega = v->wheel[2].omega;
        if (womega <= v->wheel[3].omega) womega = v->wheel[3].omega;
        int boost = (v->input_bits_13FC & 4) ? 1 : 0;
        float torque = b3_engine_transmission_update(
            t, v->throttle_1400 - v->brake_1404, womega,
            /*kick=*/0, boost, dt, speed, v->maxboost_13D4,
            v->boost_elapsed, v->boost_ramp_done);
        v->drive_torque_1520 = torque;
        v->boost_1444 = boost ? 1 : 0;
        // boost push: engine-brake torque flips to a half push while
        // boosting in top gear under the boost top speed
        if (v->boost_1444 && v->drive_torque_1520 < 0.0f
            && t->gear == t->gear_count
            && speed < v->maxboost_13D4 * 0.44704f)
            v->drive_torque_1520 *= -0.5f;
        // top-speed cut
        if (v->maxboost_13D4 * 0.44704f < speed)
            v->drive_torque_1520 = 0.0f;
    }
    t->no_upshift = 0;

    // brake-pressure gate for drift entry: linear in mph from 1.0 at
    // "Min Drift Speed" to "Max Speed Pressure" at 130 mph
    int trig;
    {
        float k = (v->maxpress_13C8 - 1.0f) / (130.0f - v->mindrift_13C4);
        float gate = (1.0f - v->mindrift_13C4 * k)
                   + k * (speed * 2.2374146f);
        if (gate < 0.1f) gate = 0.1f;
        trig = 0;   // racecar+0x134C != 3 here
        if (v->brake_1404 > gate
            || (v->throttle_1400 > 0.2f && v->brake_1404 > 0.2f))
            trig = 1;
    }

    // slide target + lerp (0x1440 <- lerp toward target by dt)
    {
        float target;
        if (v->drift_state_1524 == 1 || v->drift_state_1524 == 2) {
            float d = v->rb.dir[0] * v->rb.frame[0][0]
                    + v->rb.dir[1] * v->rb.frame[0][1]
                    + v->rb.dir[2] * v->rb.frame[0][2];
            target = (1.0f - fabsf(d) / v->cos_maxdrift_13D0)
                     * (v->slide_max_1390 - v->slide_min_1394)
                     + v->slide_min_1394 + v->brake_1404;
            if (v->throttle_raw_1414 > 0.5f)
                target -= (v->throttle_raw_1414 - 0.5f);
            if (target < v->slide_min_1394) target = v->slide_min_1394;
            if (target > v->slide_max_1390) target = v->slide_max_1390;
        } else {
            target = v->slide_max_1390;
        }
        v->slide_1440 = (target - v->slide_prev_1430) * dt
                      + v->slide_prev_1430;
    }

    // auto-drift entry (state 0) / drift clocks (state 1/2)
    if (v->drift_state_1524 == 0) {
        if (fabsf(v->steer_1408) > 0.9f
            && v->throttle_1400 > 30.0f / speed)
            v->drift_timer_1438 += dt;
        else
            v->drift_timer_1438 = 0.0f;
        float thresh = (1.5f - v->throttle_1400)
                     - (speed - 43.0f) * 0.04f + v->autodrift_13A0;
        if (thresh < 0.2f) thresh = 0.2f;
        int trig2 = (speed > 43.0f && v->drift_timer_1438 > thresh);
        if (trig || trig2) {
            if (v->steer_1408 > 0.1f) {
                v->drift_state_1524 = 2;
                v->drift_time_142C = 0.0f;
                v->slide_prev_1430 = v->slide_max_1390;
            } else if (v->steer_1408 < -0.1f) {
                v->drift_state_1524 = 1;
                v->drift_time_142C = 0.0f;
                v->slide_prev_1430 = v->slide_max_1390;
            }
        }
    } else {
        v->drift_timer_1438 = 0.0f;
        v->drift_time_142C += dt;
    }

    // latches
    v->thr_prev_141C = (t->gear == 0) ? 0.0f : v->throttle_1400;
    v->brake_prev_1420 = v->brake_1404;
    v->steer_prev_1424 = v->steer_1408;
    v->slide_prev_1430 = v->slide_1440;
}

// ---------------------------------------------------------------------------
// LSDM low-speed drive model -- FUN_0011C7C0, transcribed line for line.
// 4 substeps at dt/4 of a bicycle model: fpatan slip angles capped 3 deg
// (rear) / 8 deg (front), engine braking, rear wheel omega integration,
// then body-space delta-v converted to a force + yaw momentum/damping
// torques, front wheels rewritten free-rolling, full-stop scrub.
// ---------------------------------------------------------------------------
void b3_lsdm_update(B3VehicleFull* v, float dt) {
    B3RigidBody* rb = &v->rb;
    const float(*m)[4] = rb->frame;
    int reversing = (m[2][0] * rb->vel[0] + m[2][1] * rb->vel[1]
                     + m[2][2] * rb->vel[2]) < 0.0f;
    const float inv_yaw_I = v->rb.inv_inertia_body[1][1];  // v+0x24
    const float inv_mass = 1.0f / v->mass;
    const float h = dt * 0.25f;
    float heading = 0.0f;

    v->steer_deg_1164 = 0.0f - v->lsdm_angle_13B0 * v->steer_1408;
    v->drift_state_1524 = 0;

    float yaw0 = rb->omega[0] * m[1][0] + rb->omega[1] * m[1][1]
               + rb->omega[2] * m[1][2];
    float yaw = yaw0;
    // FUN_00031330: velocity rotated by the inverse frame -> body space
    const float(*inv)[4] = (const float(*)[4])rb->inv_frame;
    float vx0 = rb->vel[0] * inv[0][0] + rb->vel[1] * inv[1][0]
              + rb->vel[2] * inv[2][0];
    float vz0 = rb->vel[0] * inv[0][2] + rb->vel[1] * inv[1][2]
              + rb->vel[2] * inv[2][2];
    float vx = vx0, vz = vz0;
    float slip_mag = 0.0f;

    for (int step = 0; step < 4; step++) {
        float hs = v->steer_deg_1164 * 0.017453292f + heading;
        float sh = sinf(heading), ch = cosf(heading);
        float shs = sinf(hs), chs = cosf(hs);
        float vx_f = ch * yaw + vx;          // front station
        float nsh = 0.0f - sh;
        float vz_f = vz + nsh * yaw;
        float vx_r = vx - ch * yaw;          // rear station
        float vz_r = vz - nsh * yaw;

        // drive/brake/engine-brake force on the rear axle
        float drive = v->drive_torque_1520 * v->throttle_1400;
        float ac = 0.0f;
        if (v->throttle_1400 < 0.1f && 1.0f < rb->vel[3])
            ac = ((v->trans.omega * 9.549296f) / v->trans.max_rpm)
                 * v->engbrake_13CC * 0.5f;
        if (v->wheel[3].omega >= 0.0f)
            ac = (drive - v->brakef_138C * v->brake_1404 * 5000.0f) + ac;
        else
            ac = (v->brakef_138C * v->brake_1404 * 6000.0f + drive) - ac;
        v->wheel[3].omega += ac * h * 0.04f;

        // rear slip
        float roll = v->wheel[3].omega * v->wheel[3].radius;
        float vlat_r = vz_r * nsh + ch * vx_r;
        float slip_r = (vz_r * ch + vx_r * sh) - roll;
        slip_mag = sqrtf(slip_r * slip_r + vlat_r * vlat_r);
        if (slip_mag == 0.0f) slip_mag = 1e-12f;
        float mm = v->mass;
        if (mm >= 2000.0f) mm = 2000.0f;
        float adeg = fabsf(atan2f(slip_mag, roll) * 57.29578f);
        if (adeg >= 3.0f) adeg = 3.0f;
        float fr = adeg * 0.33333334f * mm * 16.0f;
        float Fr_lat = 0.0f - (1.0f / slip_mag) * fr * vlat_r;
        float Fr_long = 0.0f - (1.0f / slip_mag) * fr * slip_r;
        v->wheel[3].omega -= Fr_long * v->wheel[3].radius * h * 0.04f;
        v->wheel[2].omega = v->wheel[3].omega;

        // front lateral
        float vlat_f = vz_f * (0.0f - shs) + chs * vx_f;
        mm = v->mass;
        if (mm >= 2000.0f) mm = 2000.0f;
        adeg = fabsf(atan2f(vlat_f, vz_f * chs + shs * vx_f) * 57.29578f);
        if (adeg >= 8.0f) adeg = 8.0f;
        float Ff = adeg * 0.125f * mm * 12.0f;
        if (0.0f < vlat_f) Ff = -Ff;
        float cs = cosf(v->steer_deg_1164 * 0.017453292f);

        vx += (chs * Ff + ch * Fr_lat + sh * Fr_long) * h * inv_mass;
        yaw += (cs * Ff + (0.0f - Fr_lat)) * h * inv_yaw_I;
        vz += ((0.0f - shs) * Ff + nsh * Fr_lat + ch * Fr_long) * h
              * inv_mass;
        heading = yaw * h + heading;
    }

    if (1.5258789e-05f < fabsf(vz) && 0.5f < fabsf(slip_mag / vz))
        v->trans.no_upshift = 1;

    // body delta-v -> world force into the accumulator
    float s = v->mass / dt;
    float fz = (vz - vz0) * s;
    float fx = (vx - vx0) * s;
    float F[4];
    for (int i = 0; i < 4; i++)
        F[i] = m[0][i] * fx + m[2][i] * fz;
    b3f_acc_add(rb->force_acc, F);
    // torque about pos - up * (steer_h * -0.5)
    {
        float k = v->steer_h_1370 * -0.5f;
        float r[3];
        for (int i = 0; i < 3; i++) r[i] = -m[1][i] * k;
        rb->torque_acc[0] += r[1] * F[2] - r[2] * F[1];
        rb->torque_acc[1] += r[2] * F[0] - r[0] * F[2];
        rb->torque_acc[2] += r[0] * F[1] - r[1] * F[0];
    }
    // yaw momentum change + yaw damping torques along up
    {
        float t1 = (yaw - yaw0) / (inv_yaw_I * dt);
        for (int i = 0; i < 4; i++) rb->torque_acc[i] += m[1][i] * t1;
        float t2 = 0.0f - ((v->lsdm_t1_13B4 - fabsf(v->steer_1408))
                           * v->lsdm_t2_13B8 * yaw) / (inv_yaw_I * dt);
        for (int i = 0; i < 4; i++) rb->torque_acc[i] += m[1][i] * t2;
    }
    // front wheels free-roll from their point velocity
    for (int i = 0; i < 2; i++) {
        float pt[4], vp[4];
        b3f_xform_point(m, v->wheel[i].local_x, v->wheel[i].attach,
                        v->wheel[i].local_z, pt);
        b3f_point_vel(rb, pt, vp);
        float w = sqrtf(vp[0] * vp[0] + vp[1] * vp[1] + vp[2] * vp[2])
                  / v->wheel[i].radius;
        v->wheel[i].omega = reversing ? -w : w;
    }
    // full stop scrub
    if (rb->vel[3] < 0.2f && v->trans.gear == 0) {
        for (int i = 0; i < v->wheel_count; i++) v->wheel[i].omega = 0.0f;
        for (int i = 0; i < 4; i++) {
            rb->vel[i] *= 0.9f;
            rb->omega[i] *= 0.9f;
        }
    }
}

// ---------------------------------------------------------------------------
// Force pass -- FUN_0011D460, the full function (mirrors the emulation-
// verified d460_model in validate_port.py plus the LSDM dispatch).
// The flag_b takedown/recoil block stays unported (flag_b == 0 here);
// FUN_0011AEF0 (crash response) is the crash agent's.
// ---------------------------------------------------------------------------
static void b3_d460_force_pass(B3VehicleFull* v, float dt) {
    B3RigidBody* rb = &v->rb;
    const float(*m)[4] = rb->frame;
    const float speed = rb->vel[3];
    const float mph = speed * B3_MS_TO_MPH_GAME;
    int state = v->drift_state_1524;

    int airborne = 1;
    for (int i = 0; i < v->wheel_count; i++)
        if (v->wheel[i].contact) airborne = 0;
    float fdot_right = v->rb.dir[0] * m[0][0] + v->rb.dir[1] * m[0][1]
                     + v->rb.dir[2] * m[0][2];

    int prev_air = v->f1168;
    if (airborne) {
        if (prev_air == 0) v->airtime_143C = 0.0f;
        v->airtime_143C += dt;
        v->f1168 = 1;
        v->trans.no_upshift = 1;
    } else {
        if (prev_air == 1) {
            v->trans.no_upshift = 0;
            if (0.1f < v->airtime_143C) {
                // sideways landing -> drift entry (0x0011D4B7..0x0011D51D).
                // The drift CLOCK and the slide latch are reset with it
                // when the car was not already drifting -- without that the
                // fresh drift starts with a stale +0x142C, which skips the
                // 1 s lateral ramp, the 0.25 s yaw-cap reduction and the
                // 0.3 s brake hold, and lets the 0.5 s exit test fire on
                // the very next substep. (Was MISSING in this port.)
                float thr = v->cos90_mindrift_air_13DC;
                if (fdot_right > thr) {
                    if (v->drift_state_1524 == 0) {
                        v->drift_time_142C = 0.0f;
                        v->slide_prev_1430 = v->slide_max_1390;
                    }
                    v->drift_state_1524 = 2;
                } else if (fdot_right < -thr) {
                    if (v->drift_state_1524 == 0) {
                        v->drift_time_142C = 0.0f;
                        v->slide_prev_1430 = v->slide_max_1390;
                    }
                    v->drift_state_1524 = 1;
                }
            }
        }
        v->f1168 = 0;
    }
    state = v->drift_state_1524;

    // longitudinal resistance (b3_resistance_force gates, verified)
    {
        float extra = 0.0f;
        if (state == 0 && v->byte_153D == 0)
            extra = v->kquad_13C0 * speed * v->steer_1408 * v->steer_1408;
        int apply = 0;
        if (!v->boost_1444 && !v->flag_b_1446) apply = 1;
        else if (v->maxboost_13D4 * 0.44704f <= speed) apply = 1;
        else if (v->trans.gear != v->trans.gear_count
                 && v->trans.change_up_rpm
                    <= v->trans.omega * 9.549296f) apply = 1;
        if (apply) {
            float s = -(v->resist_1360 * speed) - extra;
            float F[4] = {rb->vel[0] * s, rb->vel[1] * s, rb->vel[2] * s,
                          speed * s};
            b3f_acc_add(rb->force_acc, F);
        }
    }

    if (v->f1168) {
        // ---- airborne: damper table, corkscrew, vertical, drive ----
        b3_airborne_damper(rb->frame, rb->vel, rb->omega,
                           v->half_ext[2], v->center_off[2],
                           rb->force_acc, rb->torque_acc);
        {   // corkscrew damping of the angular momentum (FUN_0011f800)
            float d = m[2][0] * rb->angmom[0] + m[2][1] * rb->angmom[1]
                    + m[2][2] * rb->angmom[2];
            for (int i = 0; i < 4; i++)
                rb->angmom[i] -= m[2][i] * d * v->corkscrew_13D8;
        }
        rb->force_acc[1] += b3_vertical_force(v->downforce_1364, speed,
                                              v->mass);
        v->wheel[2].torque += v->drive_torque_1520 * 0.5f;
        v->wheel[3].torque += v->drive_torque_1520 * 0.5f;
        for (int i = 0; i < v->wheel_count; i++) {
            v->wheel[i].omega += v->wheel[i].torque * 0.04f * dt;
            v->wheel[i].torque = 0.0f;
        }
        return;
    }

    // ---- grounded ----
    int lsdm = (v->flag_b_1446 == 0
                && (v->lsdm_limit_13AC > mph || v->trans.gear == -1));
    if (lsdm) {
        b3_lsdm_update(v, dt);
    } else {
        v->wheel[2].torque += v->drive_torque_1520 * 0.5f;
        v->wheel[3].torque += v->drive_torque_1520 * 0.5f;

        // brake / engine-brake along the travel direction (verified law)
        {
            float pt[4];
            for (int i = 0; i < 4; i++)
                pt[i] = m[1][i] * v->brake_h_1368 + m[3][i];
            float sc = b3_brake_drag_scalar(
                v->engbrake_13CC, v->brakef_138C, v->throttle_1400,
                v->brake_1404, speed, state, v->drift_time_142C);
            if (state == 1 || state == 2)
                for (int i = 0; i < 4; i++) pt[i] = m[3][i];
            float F[4];
            for (int i = 0; i < 4; i++) F[i] = rb->dir[i] * sc;
            b3f_acc_add(rb->force_acc, F);
            b3f_torque_at(rb, pt, F, rb->torque_acc);
        }

        // per-wheel tyre forces in the steered basis
        float sright[4], sat[4];
        b3f_steered_basis(m, v->steer_deg_1164, sright, sat);
        float T_saved[4], latsum[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; i++) T_saved[i] = rb->torque_acc[i];
        int side02 = v->wheel[0].contact || v->wheel[2].contact;
        int side13 = v->wheel[1].contact || v->wheel[3].contact;
        for (int i = 0; i < 4; i++) {
            B3WheelSim* w = &v->wheel[i];
            float wp[4], vp[4];
            b3f_xform_point(m, w->local_x, w->attach, w->local_z, wp);
            b3f_point_vel(rb, wp, vp);
            const float* fwd = (i < 2) ? sat : m[2];
            const float* lat = (i < 2) ? sright : m[0];
            float roll = w->omega * w->radius;
            float vlat = lat[0] * vp[0] + lat[1] * vp[1] + lat[2] * vp[2];
            float vlong = fwd[0] * vp[0] + fwd[1] * vp[1] + fwd[2] * vp[2];
            if (i < 2) w->omega = vlong / w->radius;  // front free-roll
            float flat, flong;
            b3_tyre_grip(i < 2, v->throttle_1400, speed, v->mass, vlat,
                         vlong, roll, &flat, &flong);
            b3_tyre_wheel_reaction(flong, w->radius, dt, &w->torque,
                                   &w->omega);
            float Fl[4];
            float height;
            int extra_gate;
            if (state == 1 || state == 2) {
                float blended = b3_drift_lateral_blend(
                    flat, v->slide_1440, v->drift_time_142C,
                    rb->dir[0] * m[2][0] + rb->dir[1] * m[2][1]
                    + rb->dir[2] * m[2][2]);
                float oms = 1.0f - v->slide_1440;
                // blended = flat*(1-slide) + mag*slide, split as the game
                // does: lat * (flat*(1-slide) + mag*slide)
                (void)oms;
                for (int j = 0; j < 4; j++) Fl[j] = lat[j] * blended;
                height = v->drift_h_1374;
                extra_gate = side02 && side13;
            } else {
                for (int j = 0; j < 4; j++) Fl[j] = lat[j] * flat;
                height = v->steer_h_1370;
                extra_gate = 1;
            }
            int contact_gate = (i < 2)
                ? (v->wheel[0].contact || v->wheel[1].contact)
                : w->contact;
            float p2[4];
            for (int j = 0; j < 4; j++) {
                float off = (contact_gate && extra_gate)
                          ? m[1][j] * height : 0.0f;
                p2[j] = wp[j] + off;
            }
            b3f_acc_add(rb->force_acc, Fl);
            b3f_torque_at(rb, p2, Fl, rb->torque_acc);
            b3f_acc_add(latsum, Fl);
            float F2[4], p3[4];
            for (int j = 0; j < 4; j++) {
                F2[j] = fwd[j] * flong;
                p3[j] = m[3][j] + m[1][j] * v->accel_h_136C;
            }
            b3f_acc_add(rb->force_acc, F2);
            if (v->flag_b_1446 == 0)
                b3f_torque_at(rb, p3, F2, rb->torque_acc);
        }

        // drift-state exits
        float fdot = rb->dir[0] * m[0][0] + rb->dir[1] * m[0][1]
                   + rb->dir[2] * m[0][2];
        state = v->drift_state_1524;
        if ((state == 1 || state == 2) && 0.5f < v->drift_time_142C) {
            if (state == 1) {
                if (fdot >= -0.01f && !(v->steer_1408 < -0.005f))
                    v->drift_state_1524 = 0;
            } else if (fdot <= 0.01f && !(0.005f < v->steer_1408)) {
                v->drift_state_1524 = 0;
            }
        }
        state = v->drift_state_1524;
        if ((state == 1 || state == 2) && v->throttle_1400 < 0.5f
            && v->brake_1404 < 0.5f && v->steer_1408 == 0.0f
            && mph < 40.0f)
            v->drift_state_1524 = 0;
        state = v->drift_state_1524;

        // yaw torque rewrite + turn-momentum cap (verified laws)
        {
            const float* up = m[1];
            float dT[4], d;
            for (int i = 0; i < 4; i++)
                dT[i] = rb->torque_acc[i] - T_saved[i];
            d = dT[0] * up[0] + dT[1] * up[1] + dT[2] * up[2];
            float dT_perp[4];
            for (int i = 0; i < 4; i++) dT_perp[i] = dT[i] - up[i] * d;
            float yaw_mom = rb->angmom[0] * up[0] + rb->angmom[1] * up[1]
                          + rb->angmom[2] * up[2];
            float d_new;
            if (state == 1 || state == 2) {
                d_new = b3_drift_yaw_torque(state, v->steer_1408,
                                            v->throttle_raw_1414,
                                            v->slide_1440,
                                            v->turn_rate_13A4,
                                            v->cos_maxdrift_13D0, fdot);
            } else {
                d_new = d;
            }
            float cap = b3_yaw_torque_cap(mph, v->turn_slow_1398,
                                          v->turn_fast_139C,
                                          v->drift_time_142C);
            if (d_new > 0.0f && yaw_mom > cap) d_new *= 0.1f;
            if (0.0f > d_new && -cap > yaw_mom) d_new *= 0.1f;
            for (int i = 0; i < 4; i++)
                rb->torque_acc[i] = T_saved[i]
                                  + (dT_perp[i] + up[i] * d_new);
        }

        // anti-slowdown lateral push while drifting on throttle
        {
            float ldot = latsum[0] * rb->dir[0] + latsum[1] * rb->dir[1]
                       + latsum[2] * rb->dir[2];
            if (0.0f > ldot
                && (v->drift_state_1524 == 1 || v->drift_state_1524 == 2)
                && v->throttle_1400 > 0.1f) {
                float s2 = -ldot;
                float F[4];
                for (int i = 0; i < 4; i++) F[i] = rb->dir[i] * s2;
                b3f_acc_add(rb->force_acc, F);
                float crossud[4];
                crossud[0] = m[1][1] * rb->dir[2] - m[1][2] * rb->dir[1];
                crossud[1] = m[1][2] * rb->dir[0] - m[1][0] * rb->dir[2];
                crossud[2] = m[1][0] * rb->dir[1] - m[1][1] * rb->dir[0];
                crossud[3] = 0.0f;
                float push[4];
                for (int i = 0; i < 4; i++) push[i] = crossud[i] * ldot;
                float dr = rb->dir[0] * m[0][0] + rb->dir[1] * m[0][1]
                         + rb->dir[2] * m[0][2];
                if (0.0f > dr)
                    for (int i = 0; i < 4; i++) push[i] = -push[i];
                b3f_acc_add(rb->force_acc, push);
            }
        }
    }

    // body gravity pin (grounded + LSDM paths)
    b3_gravity_body_pin(v->mass, speed, m[2][1], m[1], rb->force_acc);
}

int (*b3_ground_probe_hook)(float, float, float, float*, float[3]) = 0;

static int b3f_ray(const B3VehicleFull* v, const float start[3],
                   const float end[3], float* hit_t, float n[3]) {
    if (v->soup_ground_ray)
        return v->soup_ground_ray(v->soup_ground_user, start, end, hit_t, n);
    float h;
    int surface = b3_ground_probe_hook
                ? b3_ground_probe_hook(start[0], start[1], start[2], &h, n)
                : b3_ground_probe(start[0], start[1], start[2], &h, n);
    if (surface >= 0) {
        float denom = start[1] - end[1];
        if (denom > 1e-8f) {
            *hit_t = (start[1] - h) / denom;
            return surface;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Suspension pre-pass -- FUN_001239C0 over the frozen-soup ray interface.
// The real function ray-tests the collected poly soup (FUN_00123790 /
// FUN_001B2230); the fallback abstracts ground height + normal + surface.
// ---------------------------------------------------------------------------
static void b3_prepass(B3VehicleFull* v) {
    B3RigidBody* rb = &v->rb;
    const float(*m)[4] = rb->frame;

    // prev-frame snapshots (wheel +0x30 <- +0x00, +0x40 <- +0x10)
    for (int i = 0; i < v->wheel_count; i++) {
        for (int j = 0; j < 4; j++) {
            v->wheel[i].prev_pos[j] = v->wheel[i].world_pos[j];
            v->wheel[i].prev_contact[j] = v->wheel[i].contact_pt[j];
        }
    }

    // per-wheel ray endpoints (local y: start = r + attach,
    // end = attach - 0.75*len - r), transformed to world
    float start_w[4][4], end_w[4][4], start_ly[4], end_ly[4];
    for (int i = 0; i < v->wheel_count; i++) {
        B3WheelSim* w = &v->wheel[i];
        float len = (i < 2) ? v->front_len : v->rear_len;
        start_ly[i] = w->radius + w->attach;
        end_ly[i] = (w->attach - len * 0.75f) - w->radius;
        b3f_xform_point(m, w->local_x, start_ly[i], w->local_z,
                        start_w[i]);
        b3f_xform_point(m, w->local_x, end_ly[i], w->local_z, end_w[i]);
    }
    // ray vector = up * -(max start y - min end y) over wheels 1 and 3
    float smax = start_ly[1];
    if (smax <= start_ly[3]) smax = start_ly[3];
    float emin = end_ly[1];
    if (end_ly[3] <= emin) emin = end_ly[3];
    float span = smax - emin;
    float rayv[4];
    for (int i = 0; i < 4; i++) rayv[i] = m[1][i] * (0.0f - span);
    float raylen = sqrtf(rayv[0] * rayv[0] + rayv[1] * rayv[1]
                         + rayv[2] * rayv[2]);

    for (int i = 0; i < v->wheel_count; i++) {
        B3WheelSim* w = &v->wheel[i];
        float hit_t, gn[3];
        int surf = b3f_ray(v, start_w[i], end_w[i], &hit_t, gn);
        int hit = 0;
        float dist = 0.0f;
        if (surf >= 0) {
            if (hit_t > -1e-5f && hit_t <= 1.00001f) {
                hit = 1;
                dist = hit_t * raylen;
            }
        }
        if (!hit) {
            // airborne path (LAB_00123e01): contact from the frame up
            float h = w->radius;
            w->contact = 0;
            w->force_flag = 0;
            for (int j = 0; j < 4; j++) {
                w->contact_pt[j] = w->world_pos[j] - m[1][j] * h;
                w->normal[j] = m[1][j];
            }
        } else {
            w->contact = 1;
            if (gn[1] >= 0.2f) {
                w->normal[0] = gn[0];
                w->normal[1] = gn[1];
                w->normal[2] = gn[2];
                w->normal[3] = 0.0f;
            } else {
                for (int j = 0; j < 4; j++) w->normal[j] = m[1][j];
            }
            unsigned short st = (unsigned short)surf;
            if ((st & 0xFF) < 0x15 || (st & 0xFF) == 0x26)
                w->surface = st;
            float t = dist / raylen;
            for (int j = 0; j < 4; j++)
                w->contact_pt[j] = start_w[i][j] + rayv[j] * t;
            w->cur_len = (start_ly[i] - dist) + w->radius;
            // grip scalar (FUN_00123790 side effect)
            if ((st & 0xFF) == 0x26) v->grip_scalar = 0.2f;
            else if (v->grip_scalar == 0.2f) v->grip_scalar = 1.2f;
        }
    }
    v->surface_1160 = v->wheel[0].surface;

    // body ground probe: 30 m down-ray from the frame origin
    v->ground_clear = 10000.0f;
    {
        float start[3] = {m[3][0], m[3][1], m[3][2]};
        float end[3] = {m[3][0], m[3][1] - 30.0f, m[3][2]};
        float hit_t, gn[3];
        int surf = b3f_ray(v, start, end, &hit_t, gn);
        if (surf >= 0) {
            if (hit_t >= 0.0f && hit_t <= 1.00001f) {
                v->ground_clear = hit_t * 30.0f;
                v->surface_1160 = (unsigned short)surf;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Contact impulse -- FUN_00106720 (0x00106720..0x0010688F), verbatim.
//   c  = r x n,  r = pt - frame.pos
//   w  = c . M              (M = the world inverse-inertia rows at +0x40)
//   j  = -(restitution + 1) * (n . v_point)
//        / (1/mass + ((w x r) . n))
// The function writes n*|j| through out_imp and RETURNS the SIGNED j; its
// only caller (the suspension bottom-out block) acts on j > 0, i.e. the
// contact point is closing on the surface.
// ---------------------------------------------------------------------------
float b3_contact_impulse(const B3RigidBody* rb, float mass_kg,
                         const float n[4], const float pt[4],
                         const float vpt[4], float restitution,
                         float out_imp[4]) {
    float r[3];
    for (int i = 0; i < 3; i++) r[i] = pt[i] - rb->frame[3][i];
    float c[3];
    c[0] = r[1]*n[2] - r[2]*n[1];
    c[1] = r[2]*n[0] - r[0]*n[2];
    c[2] = r[0]*n[1] - r[1]*n[0];
    float w[3];
    for (int j = 0; j < 3; j++)
        w[j] = c[0]*rb->inv_inertia_world[0][j]
             + c[1]*rb->inv_inertia_world[1][j]
             + c[2]*rb->inv_inertia_world[2][j];
    float wr[3];
    wr[0] = w[1]*r[2] - w[2]*r[1];
    wr[1] = w[2]*r[0] - w[0]*r[2];
    wr[2] = w[0]*r[1] - w[1]*r[0];
    const float denom = 1.0f / mass_kg
                      + wr[0]*n[0] + wr[1]*n[1] + wr[2]*n[2];
    const float vn = n[0]*vpt[0] + n[1]*vpt[1] + n[2]*vpt[2];
    const float j = (0.0f - (restitution + 1.0f) * vn) / denom;
    const float mag = fabsf(j);
    for (int i = 0; i < 4; i++) out_imp[i] = n[i] * mag;
    return j;
}

// FUN_00106500: impulse at a point -- +0x110 += imp (4-wide), +0x120 +=
// (pt - pos) x imp. (The w lane of the cross is self-cancelling in the
// original and stays 0 here.)
static void b3f_impulse_at(B3RigidBody* rb, const float pt[4],
                           const float imp[4]) {
    for (int i = 0; i < 4; i++) rb->imp_force[i] += imp[i];
    float r[4];
    for (int i = 0; i < 4; i++) r[i] = pt[i] - rb->frame[3][i];
    rb->imp_torque[0] += r[1]*imp[2] - r[2]*imp[1];
    rb->imp_torque[1] += r[2]*imp[0] - r[0]*imp[2];
    rb->imp_torque[2] += r[0]*imp[1] - r[1]*imp[0];
}

// ---------------------------------------------------------------------------
// FUN_00109EA0 [C] -- THE shared body-vs-world contact resolution, ported
// line for line from 0x00109EA0..0x0010A43A.
//
// Retail drives it once per frame per allocated body, from the collision
// manager FUN_00110AF0 through vtable slot +0x10:
//     class 6 (knocked props)   FUN_0011A490 @0x0011A706 -> here
//     class 7 (panels/debris)   FUN_001072A0 @0x001073CF -> here
//     the racecar                             @0x00122F81 -> here
// The slot-4 wrapper GATHERS the local polygon soup (FUN_00109D20 into the
// body's +0x200 list) and the narrow phase inside this function turns it into
// ONE contact -- FUN_00107950 (the OBB arm, taken by every body with
// +0x20C == 1, which is props @0x0011A12B and every class-7 piece
// @0x00106A36/0x00106ADB/0x00106C5B) or FUN_0010AAD0 otherwise -- writing
// +0x160 point, +0x170 normal and a push-out vector.  Everything AFTER the
// narrow phase is this function, and this is that part:
//
//   vpt = FUN_001066A0(pt)                                     @0x00109FED
//   j   = FUN_00106720(n, pt, vpt, restitution=+0x1F8)          @0x0010A010
//   if (j > 0) { FUN_00106500(n*|j| at pt); +0x194 = j; +0x213 = 1 }
//   u = vpt;  if (+0x215 not in {1,2,3}) u += G(0,-20,0,0)      @0x0010A06C
//   F = (u - n*(u.n)) * mass * -0.6            [0x003B1AC8]     @0x0010A0E2
//   if (+0x215 in {1,2,3})                                      @0x0010A110
//        |F|^2 > 1.0        -> FUN_001064B0(F at pt)
//        else               -> vel *= 0.95, speed *= 0.95
//   else                                                        @0x0010A1A5
//        |F|^2 > 1440000.0  -> FUN_001064B0(F at pt)
//        else k = 0.7 (class 6) | 0.6/0.9 (class 7, by +0x2BA) | 0.875
//                            -> vel *= k, speed *= k
//   L *= 0.99                                  [0x003B1758]     @0x0010A25E
//   if (+0x215 == 6 || == 7) { L *= 0.95; vel *= 0.95 }         @0x0010A299
//   if (speed < 1.0 && any |n . frame_row| > 0.99) {            @0x0010A307
//        L *= 0.5;  if (speed < 0.3 && |omega|^2 < 0.09) +0x20E = 1 (sleep)
//   }
//   if (speed < 1.0) L *= 0.9                  [0x003A69C0]     @0x0010A3E9
//   +0x180 = pushout;  +0x130 (deflection) += pushout;  +0x198 = 1
//
// The two thresholds live in BSS and are C++ static initialisers -- PROVEN,
// not assumed: the ONLY absolute references to 0x005A538C in the whole image
// are the read @0x0010A1AD and the write in the init thunk @0x002B92A0
// (`movss xmm0,[0x003B2334]; movss [0x005A538C],xmm0; ret`), whose address
// sits in the CRT initialiser array at 0x003BDBE4; likewise 0x005A3A94 is
// read only @0x0010A3D9 and written only by the thunk @0x002B9280 out of
// [0x003B1D38].  So:
//     [0x005A538C] = 1440000.0  (|F| > 1200 N applies the friction force)
//     [0x005A3A94] = 0.09       (|omega| < 0.3 rad/s may sleep)
// ---------------------------------------------------------------------------
/* the xmmword at [0x0040A8A0] is (0, -20, 0, 0) -- a 4-wide ADDPS, so only
   the y lane moves and the w lane (which carries +0xBC speed out of
   FUN_001066A0) is untouched.                                            [C] */
#define B3_WC_LIN_DRAG_G     (-20.0f)     /* [0x0040A8A0].y              [C] */
#define B3_WC_FRICTION       (-0.6f)      /* [0x003B1AC8]                [C] */
#define B3_WC_FORCE_GATE     1440000.0f   /* [0x005A538C] init'd @0x2B92A0[C] */
#define B3_WC_FORCE_GATE_CAR 1.0f         /* [0x003B168C]                [C] */
#define B3_WC_DAMP_CAR       0.95f        /* [0x003A69B8]                [C] */
#define B3_WC_DAMP_CLASS6    0.7f         /* [0x003B17D8]                [C] */
#define B3_WC_DAMP_CLASS7_2  0.6f         /* [0x003B16EC]                [C] */
#define B3_WC_DAMP_CLASS7    0.9f         /* [0x003A69C0]                [C] */
#define B3_WC_DAMP_OTHER     0.875f       /* [0x0039922C]                [C] */
#define B3_WC_SPIN_DAMP      0.99f        /* [0x003B1758]                [C] */
#define B3_WC_SPIN_DAMP_67   0.95f        /* [0x003A69B8]                [C] */
#define B3_WC_SLOW_SPEED     1.0f         /* [0x003B168C]                [C] */
#define B3_WC_AXIS_ALIGNED   0.99f        /* [0x003B1758] reused as a dot [C]*/
#define B3_WC_SPIN_KILL      0.5f         /* [0x003B1684]                [C] */
#define B3_WC_SPIN_SLOW      0.9f         /* [0x003A69C0]                [C] */
#define B3_WC_SLEEP_SPEED    0.3f         /* [0x003B1750]                [C] */
#define B3_WC_SLEEP_OMEGA2   0.09f        /* [0x005A3A94] init'd @0x2B9280[C] */

// FUN_001064B0 [C]: a FORCE at a world point -- +0xF0 += F, +0x100 +=
// (pt - pos) x F  (FUN_00106590).
static void b3f_force_at(B3RigidBody* rb, const float pt[4],
                         const float f[4]) {
    for (int i = 0; i < 4; i++) rb->force_acc[i] += f[i];
    float r[4];
    for (int i = 0; i < 4; i++) r[i] = pt[i] - rb->frame[3][i];
    rb->torque_acc[0] += r[1]*f[2] - r[2]*f[1];
    rb->torque_acc[1] += r[2]*f[0] - r[0]*f[2];
    rb->torque_acc[2] += r[0]*f[1] - r[1]*f[0];
}

// ---------------------------------------------------------------------------
// FUN_00107950 [C] -- the OBB narrow phase, ported from 0x00107950..0x00107E82
// and specialised to a soup of ONE surface polygon.
//
// Retail's version walks the polygon list at body+0x200 and, for each polygon:
//   @0x001079DD  transforms the 3 vertices into BODY space with the inverse
//                frame at body+0x70 (p.x*r0 + p.y*r1 + p.z*r2 + r3)
//   @0x00107A33/0x00107A5F/0x00107A8B  clips the polygon against the three OBB
//                slabs [+0x1E0 bbmin, +0x1D0 bbmax] with FUN_001B09C0, one
//                axis per call, and drops the polygon if fewer than 3 vertices
//                survive (`cmp eax,3; jl`)
//   @0x00107B79  centroid_body = (sum of the surviving vertices) / count
//   @0x00107BEB  transforms it back to world with the frame, accumulates it
//                into a running sum, and accumulates the polygon's WORLD
//                normal into a second running sum; bumps the polygon count
// and then, once:
//   @0x00107CBD  |sum(normals)|^2 <= [0x003B191C] = 2^-32  ->  NO CONTACT
//   @0x00107CF2  normal  = normalize(sum of normals)          -> +0x170
//   @0x00107D14  point   = sum(centroids) * (1 / polygon count) -> +0x160
//   @0x00107D41  n_body  = rotate(inv_frame, normal)
//   @0x00107D5B  c_body  = transform(inv_frame, point)
//   @0x00107D75  depth t = min over the three axes of
//                    (n_body[a] >= 0 ? c_body[a] - bbmin[a]
//                                    : bbmax[a] - c_body[a]) / |n_body[a]|
//                (the retail code does the minimum by cross-multiplication,
//                 never dividing until the winner is known)
//   @0x00107E3F  t = max(t, [0x003B194C] = 0.005)
//   @0x00107E69  pushout = normal * t                         -> arg10
//
// With one polygon the two running sums are that polygon's, so the whole
// function collapses to: clip the surface plane against the box, take the
// centroid of the cross-section, and push out by the depth of the box below
// the plane along the normal.  Every constant below is [C].
// ---------------------------------------------------------------------------
#define B3_NP_NORMAL_EPS2  2.3283064365386963e-10f  /* [0x003B191C] 2^-32  [C] */
#define B3_NP_MIN_PUSHOUT  0.004999999888241291f    /* [0x003B194C]        [C] */

// FUN_001B09C0's slab clip, one axis, both planes (Sutherland-Hodgman).
static int b3_np_clip_axis(const float in[8][4], int n, int axis,
                           float lo, float hi, float out[8][4]) {
    float tmp[8][4];
    int m = 0;
    for (int i = 0; i < n && m < 8; i++) {          // keep >= lo
        const float* a = in[i];
        const float* b = in[(i + 1) % n];
        const int ia = a[axis] >= lo, ib = b[axis] >= lo;
        if (ia) { for (int k = 0; k < 4; k++) tmp[m][k] = a[k]; m++; }
        if (ia != ib && m < 8) {
            const float d = b[axis] - a[axis];
            const float t = (d != 0.0f) ? (lo - a[axis]) / d : 0.0f;
            for (int k = 0; k < 4; k++) tmp[m][k] = a[k] + (b[k] - a[k]) * t;
            m++;
        }
    }
    int o = 0;
    for (int i = 0; i < m && o < 8; i++) {          // keep <= hi
        const float* a = tmp[i];
        const float* b = tmp[(i + 1) % m];
        const int ia = a[axis] <= hi, ib = b[axis] <= hi;
        if (ia) { for (int k = 0; k < 4; k++) out[o][k] = a[k]; o++; }
        if (ia != ib && o < 8) {
            const float d = b[axis] - a[axis];
            const float t = (d != 0.0f) ? (hi - a[axis]) / d : 0.0f;
            for (int k = 0; k < 4; k++) out[o][k] = a[k] + (b[k] - a[k]) * t;
            o++;
        }
    }
    return o;
}

int b3_rigid_body_obb_plane_contact(const B3RigidBody* rb,
                                    const float bbmin[3], const float bbmax[3],
                                    const float plane_pt[3],
                                    const float plane_n[3],
                                    B3WorldContact* out) {
    // the normal gate, @0x00107CBD
    float nrm[4] = { plane_n[0], plane_n[1], plane_n[2], 0.0f };
    const float n2 = nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2];
    if (n2 <= B3_NP_NORMAL_EPS2) return 0;
    b3_normalize4(nrm);

    // The one "polygon": a square in the surface plane, centred under the
    // body and comfortably larger than the box, so clipping it against the
    // slabs yields exactly the box's cross-section -- which is what retail's
    // real triangles produce whenever the body sits over the surface.
    float e0[4], e1[4];
    {
        const float ax = fabsf(nrm[0]), ay = fabsf(nrm[1]), az = fabsf(nrm[2]);
        float up[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (ay <= ax && ay <= az)      up[1] = 1.0f;
        else if (ax <= az)             up[0] = 1.0f;
        else                           up[2] = 1.0f;
        b3_cross4(nrm, up, e0);
        b3_normalize4(e0);
        b3_cross4(nrm, e0, e1);
        b3_normalize4(e1);
    }
    float span = 0.0f;
    for (int a = 0; a < 3; a++) {
        const float d = fabsf(bbmax[a]) + fabsf(bbmin[a]);
        span += d * d;
    }
    span = sqrtf(span) + 1.0f;
    float poly[8][4], clipped[8][4];
    for (int i = 0; i < 4; i++) {
        const float sx = (i == 0 || i == 3) ? -span : span;
        const float sz = (i < 2) ? -span : span;
        float w[4];
        for (int k = 0; k < 3; k++)
            w[k] = plane_pt[k] + e0[k] * sx + e1[k] * sz;
        w[3] = 1.0f;
        // -> body space with the inverse frame, @0x001079DD
        for (int k = 0; k < 4; k++)
            poly[i][k] = w[0] * rb->inv_frame[0][k]
                       + w[1] * rb->inv_frame[1][k]
                       + w[2] * rb->inv_frame[2][k]
                       + rb->inv_frame[3][k];
    }

    int n = 4;
    for (int a = 0; a < 3; a++) {
        n = b3_np_clip_axis((const float (*)[4])poly, n, a,
                            bbmin[a], bbmax[a], clipped);
        if (n < 3) return 0;                        // @0x00107A3E `jl`
        for (int i = 0; i < n; i++)
            for (int k = 0; k < 4; k++) poly[i][k] = clipped[i][k];
    }

    // centroid of the surviving vertices, @0x00107B79
    float cb[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) cb[k] += poly[i][k];
    for (int k = 0; k < 3; k++) cb[k] /= (float)n;

    // -> world with the frame, @0x00107BEB
    float cw[4];
    for (int k = 0; k < 4; k++)
        cw[k] = cb[0] * rb->frame[0][k] + cb[1] * rb->frame[1][k]
              + cb[2] * rb->frame[2][k] + rb->frame[3][k];
    cw[3] = 0.0f;

    // the normal in body space, @0x00107D41 (rotation only)
    float nb[3];
    for (int k = 0; k < 3; k++)
        nb[k] = nrm[0] * rb->inv_frame[0][k] + nrm[1] * rb->inv_frame[1][k]
              + nrm[2] * rb->inv_frame[2][k];

    // the depth, @0x00107D75: min over the axes of dist / |n|, retail's
    // cross-multiplied comparison order (x vs y, then the winner vs z).
    float dist[3], an[3];
    for (int a = 0; a < 3; a++) {
        if (nb[a] >= 0.0f) { dist[a] = cb[a] - bbmin[a]; an[a] = nb[a]; }
        else               { dist[a] = bbmax[a] - cb[a]; an[a] = -nb[a]; }
    }
    float t;
    if (dist[1] * an[0] > an[1] * dist[0]) {
        t = (dist[2] * an[0] > an[2] * dist[0]) ? dist[0] / an[0]
                                                : dist[2] / an[2];
    } else {
        t = (dist[2] * an[1] > an[2] * dist[1]) ? dist[1] / an[1]
                                                : dist[2] / an[2];
    }
    if (B3_NP_MIN_PUSHOUT > t) t = B3_NP_MIN_PUSHOUT;   // @0x00107E47

    if (out) {
        for (int k = 0; k < 4; k++) {
            out->point[k] = cw[k];
            out->normal[k] = nrm[k];
            out->pushout[k] = nrm[k] * t;
        }
        out->point[3] = 0.0f;
        out->normal[3] = 0.0f;
        out->pushout[3] = 0.0f;
    }
    return 1;
}

void b3_rigid_body_world_contact(B3RigidBody* rb, float mass_kg, int cls,
                                 int attach_mode, float restitution,
                                 const B3WorldContact* c,
                                 B3WorldContactResult* out) {
    // ---- the no-contact early-out -------------------- @0x00109EBD/0x109FD8
    // Retail takes it twice: an empty polygon soup ([+0x200][0] == 0) and a
    // narrow phase that returned 0.  Both land on 0x00109ECA, which clears
    // +0x212, +0x198 and +0x194 and returns -- the push-out is NOT applied
    // and +0x213 (already cleared at 0x00109EB6) stays 0.
    if (out) {
        out->impact = 0.0f;
        out->impulsed = 0;
        out->grounded = 0;
        out->sleep = 0;
        out->valid = 0;
        for (int i = 0; i < 4; i++) out->pushout[i] = 0.0f;
    }
    if (!c) return;
    const float* n = c->normal;

    // ---- the contact impulse ------------------------------- @0x00109FE6 --
    float vpt[4];
    b3f_point_vel(rb, c->point, vpt);
    float imp[4];
    const float j = b3_contact_impulse(rb, mass_kg, n, c->point, vpt,
                                       restitution, imp);
    if (out) out->grounded = 1;                      // +0x212
    if (j > 0.0f) {
        b3f_impulse_at(rb, c->point, imp);
        if (out) { out->impact = j; out->impulsed = 1; }   // +0x194, +0x213
    }

    // ---- the tangential friction force --------------------- @0x0010A04E --
    const int car_like = (cls == 1 || cls == 2 || cls == 3);
    float u[4] = { vpt[0], vpt[1], vpt[2], vpt[3] };
    if (!car_like) u[1] += B3_WC_LIN_DRAG_G;          // + G, w lane untouched
    // FUN_00013C60 / FUN_00038BC0 both fold only lanes 0..2; the vector ops
    // around them are 4-wide, and the w lane of the force is inert because
    // FUN_00109560 scales the accumulator by (dt,dt,dt,0).
    const float un = u[0]*n[0] + u[1]*n[1] + u[2]*n[2];
    float f[4];
    for (int k = 0; k < 4; k++)
        f[k] = (u[k] - n[k]*un) * mass_kg * B3_WC_FRICTION;
    const float f2 = f[0]*f[0] + f[1]*f[1] + f[2]*f[2];
    const float gate = car_like ? B3_WC_FORCE_GATE_CAR : B3_WC_FORCE_GATE;
    if (f2 > gate) {
        b3f_force_at(rb, c->point, f);
    } else {
        float k;
        if (car_like)        k = B3_WC_DAMP_CAR;
        else if (cls == 6)   k = B3_WC_DAMP_CLASS6;
        else if (cls == 7)   k = (attach_mode == 2) ? B3_WC_DAMP_CLASS7_2
                                                    : B3_WC_DAMP_CLASS7;
        else                 k = B3_WC_DAMP_OTHER;
        for (int i = 0; i < 3; i++) rb->vel[i] *= k;
        rb->vel[3] *= k;
    }

    // ---- the spin bleed ------------------------------------ @0x0010A25E --
    for (int i = 0; i < 4; i++) rb->angmom[i] *= B3_WC_SPIN_DAMP;
    if (cls == 6 || cls == 7) {
        for (int i = 0; i < 4; i++) rb->angmom[i] *= B3_WC_SPIN_DAMP_67;
        for (int i = 0; i < 3; i++) rb->vel[i] *= B3_WC_SPIN_DAMP_67;
    }

    // ---- the settle test ----------------------------------- @0x0010A307 --
    // Retail dots the contact normal against the frame rows in the order
    // row1 (@0x0010A322 +0x10), row2 (@0x0010A351 +0x20), row0 (@0x0010A37A
    // +0x00) and short-circuits out of the first two; row0 is the one that
    // must EXCEED 0.99 to enter (jbe skips), so the || form below is exact.
    if (B3_WC_SLOW_SPEED > rb->vel[3]) {
        const float d1 = fabsf(n[0]*rb->frame[1][0] + n[1]*rb->frame[1][1]
                             + n[2]*rb->frame[1][2]);
        const float d2 = fabsf(n[0]*rb->frame[2][0] + n[1]*rb->frame[2][1]
                             + n[2]*rb->frame[2][2]);
        const float d0 = fabsf(n[0]*rb->frame[0][0] + n[1]*rb->frame[0][1]
                             + n[2]*rb->frame[0][2]);
        if (d1 > B3_WC_AXIS_ALIGNED || d2 > B3_WC_AXIS_ALIGNED
            || d0 > B3_WC_AXIS_ALIGNED) {
            for (int i = 0; i < 4; i++) rb->angmom[i] *= B3_WC_SPIN_KILL;
            if (B3_WC_SLEEP_SPEED > rb->vel[3]) {
                const float w2 = rb->omega[0]*rb->omega[0]
                               + rb->omega[1]*rb->omega[1]
                               + rb->omega[2]*rb->omega[2];
                if (B3_WC_SLEEP_OMEGA2 > w2 && out) out->sleep = 1;
            }
        }
    }
    if (B3_WC_SLOW_SPEED > rb->vel[3])
        for (int i = 0; i < 4; i++) rb->angmom[i] *= B3_WC_SPIN_SLOW;

    // ---- the penetration push-out -------------------------- @0x0010A40D --
    // +0x180 = pushout; +0x130 (the integrator's deflection accumulator,
    // consumed and cleared by FUN_00109560) += pushout; +0x198 = 1.
    for (int i = 0; i < 4; i++) rb->deflection[i] += c->pushout[i];
    if (out) {
        for (int i = 0; i < 4; i++) out->pushout[i] = c->pushout[i];
        out->valid = 1;
    }
}

// ---------------------------------------------------------------------------
// FUN_00106D00 [C] -- the class-7 (panel / debris piece) per-frame update,
// ported line for line from 0x00106D00..0x00106EE8.  It is slot +0 of the
// class-7 vtable at 0x003B1108 and the collision manager FUN_00110AF0 runs it
// once per frame for every allocated piece of the 0x40-slot pool at
// gameworld+0xD3380 (stride 0x4E0), AFTER slot +0x10 (FUN_001072A0) has
// resolved the piece against the world.
//
//   +0x4D0 != 0            -> clear it and return  (one-shot suppress)  @0x00106D0D
//   +0x216 == 0xFF         -> return (outside a loaded streaming unit)  @0x00106D28
//   +0xF0 += dir(+0xC0) * (speed^2 * [0x003B16C0] = -1.0)               @0x00106D35
//   if (+0x2BA != 0)  L(+0xE0) *= [0x003B1DA0] = 0.98                   @0x00106D89
//   else if (+0x212 grounded) {
//        d = |dot(+0x170 contact normal, frame row0 = the RIGHT axis)|   @0x00106DBF
//        if (0.1 > speed && 0.1 > d)   L = (0,0,0,0)   [0x004A3830]      @0x00106DEA
//        if (+0x212 && 0.1 > d) {
//             L -= row2 * (row2 . L)          FUN_00106630               @0x00106E19
//             L -= row1 * (row1 . L)          the inline block           @0x00106E2D
//        }
//        // both gates use [0x003A69C4] = 0.1
//   }
//   FUN_00109560(dt)                                                    @0x00106E65
//   if (+0x2BA == 1)  the pinned-pose fix-up (FUN_00031330), which is a
//                     PRESENTATION transform of an ATTACHED piece and is
//                     left to the panel module.
//
// Leaving only the row0 component of L, under a gate that fires when the
// right axis is perpendicular to the contact normal, is what makes a settled
// panel roll flat on the road instead of spinning on a corner.
// ---------------------------------------------------------------------------
#define B3_C7_LIN_DRAG   (-1.0f)    /* [0x003B16C0]                      [C] */
#define B3_C7_SPIN_DAMP  0.98f      /* [0x003B1DA0], attached pieces     [C] */
#define B3_C7_SETTLE     0.1f       /* [0x003A69C4], both gates          [C] */

void b3_rigid_body_class7_update(B3RigidBody* rb, float mass_kg,
                                 float com_height, B3Class7State* st,
                                 float dt) {
    if (st && st->suppress_4d0) {                    // @0x00106D0D
        st->suppress_4d0 = 0;
        return;
    }
    if (st && st->unit_216 == 0xFF) return;          // @0x00106D28

    // the quadratic linear drag along the travel direction, @0x00106D35
    const float speed = rb->vel[3];
    const float f = speed * speed * B3_C7_LIN_DRAG;
    for (int i = 0; i < 4; i++) rb->force_acc[i] += rb->dir[i] * f;

    const int attach = st ? st->attach_2ba : 0;
    if (attach != 0) {                               // @0x00106D89
        for (int i = 0; i < 4; i++) rb->angmom[i] *= B3_C7_SPIN_DAMP;
    } else if (st && st->grounded_212) {             // @0x00106DA5
        const float* n = st->normal;
        // FUN_00013C60 folds lanes 0..2 only (mulps then two ADDSS).
        const float d = fabsf(n[0]*rb->frame[0][0] + n[1]*rb->frame[0][1]
                            + n[2]*rb->frame[0][2]);
        if (B3_C7_SETTLE > speed && B3_C7_SETTLE > d)          // @0x00106DE1
            for (int i = 0; i < 4; i++) rb->angmom[i] = 0.0f;  // [0x004A3830]
        if (B3_C7_SETTLE > d) {                                // @0x00106E10
            // FUN_00106630: L -= row2 * (row2 . L).  The dot folds lanes
            // 0..2; the MULPS/SUBPS that follow are 4-wide.
            float t2 = 0.0f;
            for (int i = 0; i < 3; i++) t2 += rb->frame[2][i] * rb->angmom[i];
            for (int i = 0; i < 4; i++) rb->angmom[i] -= rb->frame[2][i] * t2;
            // the inline block: L -= row1 * (row1 . L), same shape
            float t1 = 0.0f;
            for (int i = 0; i < 3; i++) t1 += rb->angmom[i] * rb->frame[1][i];
            for (int i = 0; i < 4; i++) rb->angmom[i] -= rb->frame[1][i] * t1;
        }
    }

    // @0x00106E65 -- the shared integrator.  A class-7 piece has +0x210 == 0
    // (never "in race") and +0x215 == 7, so FUN_00109560 takes neither the
    // in-race gravity-at-com arm nor the state-6 one.
    b3_rigid_body_integrate(rb, mass_kg, com_height, 0, 0, dt);
}

// ---------------------------------------------------------------------------
// Suspension force pass -- FUN_00123FD0, normal-race path (byte 0x210 == 0:
// no extension clamp, no soft clip, no body scrape). The bottom-out block is
// now ported in full: the deflection accumulation AND the contact-impulse
// solve (FUN_001066A0/FUN_00106720/FUN_00106500) that stops a car dead when
// its suspension runs out of travel. The HEVYCAR-ID crash bookkeeping behind
// it (0x001243FB.., byte 0x210 != 0 only) stays unported [S].
// ---------------------------------------------------------------------------
static void b3_suspension_pass(B3VehicleFull* v, float dt) {
    B3RigidBody* rb = &v->rb;
    const float(*m)[4] = rb->frame;
    float defl_acc[4] = {0, 0, 0, 0};
    int defl_count = 0;
    float imp_buf[4][4];
    int imp_count = 0;

    for (int i = 0; i < v->wheel_count; i++) {
        B3WheelSim* w = &v->wheel[i];
        w->bump = 0;
        w->force_flag = 0;
        float k = (i < 2) ? v->front_k : v->rear_k;
        float c = (i < 2) ? v->front_damp : v->rear_damp;
        float len = (i < 2) ? v->front_len : v->rear_len;
        int bottomed = 0;

        if (w->contact == 0) {
            // droop relax (verified b3_wheel_droop) + wheel world pos
            w->prev_len = b3_wheel_droop(k, len, w->attach, dt);
            b3f_xform_point(m, w->local_x, w->prev_len, w->local_z,
                            w->world_pos);
        } else {
            w->bump = (0.12f < w->cur_len - w->prev_len) ? 1 : 0;
            float comp = w->attach - w->cur_len;
            float lo = len * 0.25f;
            if (comp < lo) {
                float cut = lo - comp;
                comp = lo;
                w->cur_len = w->attach - lo;
                // 0x0011417C: the impulse arm is enabled when the car is
                // moving (speed > 1.0) OR it is not crashed (byte 0x210).
                // The racing path this port models has 0x210 == 0, so it
                // is always on.
                bottomed = (rb->vel[3] > 1.0f) || 1;
                if (cut > 0.001f) {
                    for (int j = 0; j < 4; j++)
                        defl_acc[j] += w->normal[j] * cut;
                    defl_count++;
                }
            }
            // byte 0x210 == 0: no 0.75len extension clamp, no soft clip
            float vel = (w->cur_len - w->prev_len) / dt;
            w->prev_len = w->cur_len;
            b3f_xform_point(m, w->local_x, w->cur_len, w->local_z,
                            w->world_pos);
            float f = -((comp - len) * k) + vel * c;
            float Fv[4];
            for (int j = 0; j < 4; j++) {
                Fv[j] = w->normal[j] * f;
                rb->force_acc[j] += Fv[j];
            }
            b3f_torque_at(rb, w->world_pos, Fv, rb->torque_acc);
            if (bottomed) {
                // 0x0012438C: point velocity -> impulse solve with
                // restitution 0 (a dead stop, not a bounce). The axis is
                // the FRAME'S UP ROW (+0x204 row 1), NOT the wheel's
                // contact normal -- confirmed by hooking the real call
                // (param_3 == v+0x204+0x10); the post-loop cancellation
                // below uses the same axis. Only a CLOSING contact
                // (j > 0) counts.
                float vp[4], imp[4];
                b3f_point_vel(rb, w->world_pos, vp);
                float j = b3_contact_impulse(rb, v->mass, m[1],
                                             w->world_pos, vp, 0.0f, imp);
                if (j > 0.0f) {
                    w->force_flag = 1;
                    for (int q = 0; q < 4; q++) imp_buf[i][q] = imp[q];
                    imp_count++;
                }
            }
        }
    }
    if (defl_count > 0) {
        float s = 1.0f / (float)defl_count;
        for (int j = 0; j < 4; j++) rb->deflection[j] += defl_acc[j] * s;
    }
    if (imp_count > 0) {
        // 0x00124B5C: the body's pending linear impulse loses its component
        // along the frame's UP axis first (the suspension is about to
        // replace it), then each bottomed wheel applies its share.
        const float* up = m[1];
        float d = rb->imp_force[0]*up[0] + rb->imp_force[1]*up[1]
                + rb->imp_force[2]*up[2];
        for (int j = 0; j < 4; j++) rb->imp_force[j] -= up[j] * d;
        const float s = 1.0f / (float)imp_count;
        for (int i = 0; i < v->wheel_count; i++) {
            B3WheelSim* w = &v->wheel[i];
            if (!w->force_flag) continue;
            float imp[4];
            for (int q = 0; q < 4; q++) imp[q] = imp_buf[i][q] * s;
            b3f_impulse_at(rb, w->world_pos, imp);
        }
    }

    // wheel spin + visual tail. Normal race (0x210 == 0, 0x215 == 3):
    // the 0.99 free-roll decay is OFF (it runs in crash states only).
    for (int i = 0; i < v->wheel_count; i++) {
        B3WheelSim* w = &v->wheel[i];
        b3_wheel_spin_update(&w->spin, &w->omega, /*decay=*/0,
                             w->contact, dt);
        w->frame_y = w->prev_len;   // wheel frame row3.y (visual drop)
    }
}

// ---------------------------------------------------------------------------
// The per-frame pipeline: FUN_00104D30 input glue + FUN_0011ECF0, then
// FUN_0011BE50's main path (mode 0: two substeps at dt/2).
// ---------------------------------------------------------------------------
void b3_vehicle_step_full(B3VehicleFull* v, float throttle, float brake,
                          float steer, int boost, float dt) {
    v->clock += dt;

    // driver-input glue (FUN_00104D30): raw inputs; live throttle =
    // raw * accel multiplier (capped); no steering below 0.1 m/s
    v->throttle_raw_1414 = throttle;
    v->brake_1404 = brake;
    v->steer_1408 = steer;
    v->input_bits_13FC = boost ? 4 : 0;
    if (v->rb.vel[3] < 0.1f) v->steer_1408 = 0.0f;
    {
        float th = v->throttle_raw_1414 * v->accel_mult_13BC;
        v->throttle_1400 = (th > 1.0f) ? 1.0f : th;
    }

    b3_input_stage(v, dt);

    // BE50 @0x0011BF43: FUN_0011BC60 collects the chassis polygon soup into
    // veh+0x200 ONCE per frame, OUTSIDE the substep loop.  Both substeps then
    // resolve against the same frozen set.
    if (v->soup_freeze) v->soup_freeze(v->soup_user, v);

    // BE50 main path (byte 0x210 == 0, racecar mode +0x1920 == 0)
    float snap_pos[4][4], snap_ct[4][4];
    for (int i = 0; i < v->wheel_count; i++)
        for (int j = 0; j < 4; j++) {
            snap_pos[i][j] = v->wheel[i].world_pos[j];
            snap_ct[i][j] = v->wheel[i].contact_pt[j];
        }
    float dt2 = dt * 0.5f;
    for (int sub = 0; sub < 2; sub++) {
        b3_d460_force_pass(v, dt2);
        // BE50 @0x0011C0A7: bytes +0x212/+0x213 are cleared after every
        // force pass, then FUN_0011AEF0 re-sets +0x212 when it resolves a
        // chassis contact (and forces drift state 3). The per-frame clear
        // that runs BEFORE the driver stage is FUN_00104840's, outside this
        // function -- the harness owns it, which is why +0x212 is left
        // alone at the top of the frame here.
        v->contact_212 = 0;
        v->contact_213 = 0;
        // BE50 @0x0011C0B7: `call FUN_0011AEF0` (thiscall, ECX = veh), the
        // chassis-vs-world contact resolve, IN the substep -- its impulse
        // lands in +0x110/+0x120 and its deflection in +0x130 while this
        // substep's pre-pass, suspension pass and integrator are all still
        // to come.  eax != 0 forces drift state 3 and sets +0x212
        // (@0x0011C0C0/@0x0011C0CA); eax == 0 releases a state-3 lock
        // (@0x0011C0D3).  The response is b3_crash_response (FUN_0011AEF0
        // ported verbatim, burnout3_crash.c) reached through the hook.
        {
            int wall_n = v->chassis_resolve ? v->chassis_resolve(v) : 0;
            if (wall_n != 0) {
                v->drift_state_1524 = 3;
                v->contact_212 = 1;
            } else if (v->drift_state_1524 == 3) {
                v->drift_state_1524 = 0;
            }
        }
        b3_prepass(v);
        b3_suspension_pass(v, dt2);
        // BE50 stop-check between suspension and integration
        if (v->rb.vel[3] < 0.5f && v->throttle_1400 <= 0.1f) {
            for (int j = 0; j < 4; j++) v->rb.vel[j] = 0.0f;
            v->rb.force_acc[0] = 0.0f;
            v->rb.force_acc[2] = 0.0f;
        }
        b3_rigid_body_integrate(&v->rb, v->mass, v->com_height,
                                /*in_race=*/0, /*state6=*/0, dt2);
    }
    // substep tail: prev-frame wheel records span the whole frame
    for (int i = 0; i < v->wheel_count; i++)
        for (int j = 0; j < 4; j++) {
            v->wheel[i].prev_pos[j] = snap_pos[i][j];
            v->wheel[i].prev_contact[j] = snap_ct[i][j];
        }
    if (v->timer_152C >= -0.0001f) v->timer_152C -= dt;
    // FUN_0011C720 export is the harness accessors (speed/gear/rpm live
    // in the struct); FUN_0018DA00/FUN_0011FFA0 are trajectory-inert
    // (differential ablation, RE_NOTES 14).
}

// ---------------------------------------------------------------------------
// Init, mirroring FUN_00109270 / FUN_00109190 / FUN_00122830 /
// FUN_0011A8F0 / FUN_00134710 / FUN_001214A0 with this car's real values.
// ---------------------------------------------------------------------------
void b3_vehicle_full_init(B3VehicleFull* v, const B3PhysicsConfig* cfg,
                          const float wheels_xz[4][2], float radius,
                          const float half_ext[4], const float center_off[4],
                          const float inv_inertia_diag[3],
                          const float pos[3], float heading_rad) {
    memset(v, 0, sizeof *v);
    B3RigidBody* rb = &v->rb;
    float ch = cosf(heading_rad), sh = sinf(heading_rad);
    // frame rows: right / up / at, at = (sin h, 0, cos h) game-space
    rb->frame[0][0] = ch; rb->frame[0][2] = -sh;
    rb->frame[1][1] = 1.0f;
    rb->frame[2][0] = sh; rb->frame[2][2] = ch;
    for (int i = 0; i < 3; i++) rb->frame[3][i] = pos[i];
    rb->dir[2] = 1.0f;   // FUN_00109270 default travel direction
    for (int i = 0; i < 3; i++) {
        rb->inv_inertia_body[i][i] = inv_inertia_diag[i];
        rb->inv_inertia_world[i][i] = inv_inertia_diag[i];
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) rb->inv_frame[i][j] = rb->frame[i][j];
    b3_mat_orthonormalize(rb->inv_frame);   // identity-safe
    {   // proper inverse for the seeded pose
        float t;
        float(*mm)[4] = rb->inv_frame;
        t = mm[0][1]; mm[0][1] = mm[1][0]; mm[1][0] = t;
        t = mm[0][2]; mm[0][2] = mm[2][0]; mm[2][0] = t;
        t = mm[1][2]; mm[1][2] = mm[2][1]; mm[2][1] = t;
        float p[4];
        for (int j = 0; j < 4; j++)
            p[j] = pos[0] * mm[0][j] + pos[1] * mm[1][j]
                 + pos[2] * mm[2][j];
        for (int j = 0; j < 4; j++) mm[3][j] = -p[j];
    }

    v->mass = cfg->mass_kg;
    for (int i = 0; i < 4; i++) {
        v->half_ext[i] = half_ext[i];
        v->center_off[i] = center_off[i];
    }
    v->com_height = (half_ext[1] - center_off[1]) * 0.1f;
    v->wheel_count = 4;

    v->front_attach = cfg->front_attach_height;
    v->front_k = cfg->front_spring_force;
    v->front_damp = cfg->front_spring_damping;
    v->front_len = cfg->front_spring_length;
    v->rear_attach = cfg->rear_attach_height;
    v->rear_k = cfg->rear_spring_force;
    v->rear_damp = cfg->rear_spring_damping;
    v->rear_len = cfg->rear_spring_length;

    for (int i = 0; i < 4; i++) {
        B3WheelSim* w = &v->wheel[i];
        w->local_x = wheels_xz[i][0];
        w->local_z = wheels_xz[i][1];
        w->radius = radius;
        w->attach = (i < 2) ? v->front_attach : v->rear_attach;
        float len = (i < 2) ? v->front_len : v->rear_len;
        w->prev_len = w->attach - 0.75f * len;   // droop spawn
        w->cur_len = w->prev_len;
        w->frame_y = w->prev_len;
    }

    // live 0x1360.. copies (FUN_00134710)
    v->resist_1360 = cfg->drag_coef;
    v->downforce_1364 = cfg->downforce_coef;
    v->brake_h_1368 = cfg->brake_force_height;
    v->accel_h_136C = cfg->accel_force_height;
    v->steer_h_1370 = cfg->steer_force_height;
    v->drift_h_1374 = cfg->drift_force_height;
    v->steer_min_1378 = cfg->steer_min_angle;
    v->steer_max_137C = cfg->steer_max_angle;
    v->steer_v0_1380 = cfg->steer_min_velocity;
    v->steer_base_1384 = cfg->steer_max_velocity;
    v->steer_resp_1388 = cfg->steer_response;
    v->brakef_138C = cfg->braking_factor;
    v->slide_max_1390 = cfg->slide_max;
    v->slide_min_1394 = cfg->slide_min;
    v->turn_slow_1398 = cfg->turn_momentum_slow;
    v->turn_fast_139C = cfg->turn_momentum_fast;
    v->autodrift_13A0 = cfg->auto_drift_delay;
    v->turn_rate_13A4 = cfg->turn_rate;
    v->lsdm_limit_13AC = cfg->lsdm_speed_limit;
    v->lsdm_angle_13B0 = cfg->lsdm_steering_angle;
    v->lsdm_t1_13B4 = cfg->lsdm_torque1;
    v->lsdm_t2_13B8 = cfg->lsdm_torque2;
    v->accel_mult_13BC = cfg->accel_multiplier;
    v->kquad_13C0 = cfg->steer_drag_coef;
    v->mindrift_13C4 = cfg->min_drift_speed;
    v->maxpress_13C8 = cfg->max_speed_pressure;
    v->engbrake_13CC = cfg->engine_braking_factor;
    v->cos_maxdrift_13D0 = cosf(cfg->max_drift_angle * 0.017453292f);
    v->maxboost_13D4 = cfg->max_speed_in_boost;
    v->corkscrew_13D8 = cfg->in_air_corkscrew_damping;
    v->cos90_mindrift_air_13DC =
        cosf((90.0f - cfg->min_drift_angle_in_air) * 0.017453292f);

    // control-state bytes + the aggressive-driving-reaction live copies
    // (FUN_00134710 config +0x1BC..+0x1CC -> +0x13E0..+0x13F0). class 1 is
    // what FUN_00117730 leaves on a player body; the AI racer pools get 2
    // and 3 and traffic 4 (FUN_00110280). With both stamps at -1 (never
    // slammed) classes 1/2/4 select the same config trio class 3 does, so
    // this default is trajectory-identical to the old hard-coded 0x215==3.
    v->class_215 = 1;
    v->contact_212 = 0;
    v->hit_side_153C = 0;
    v->aggr_time_13E0  = cfg->steer_away_time;
    v->aggr_total_13E4 = cfg->total_out_of_control_time;
    v->aggr_angle_13E8 = cfg->aggr_steering_max_angle;
    v->aggr_vel_13EC   = cfg->aggr_steering_max_velocity;
    v->aggr_drag_13F0  = cfg->aggr_steering_drag_coef;
    v->cfg_steer_max_14C    = cfg->steer_max_angle;
    v->cfg_steer_maxvel_154 = cfg->steer_max_velocity;
    v->cfg_steer_drag_190   = cfg->steer_drag_coef;
    v->ooc_slam_1598 = -1.0f;
    v->ooc_wall_1690 = -1.0f;
    v->launch_time_1350 = -100.0f;

    v->slide_prev_1430 = cfg->slide_max;
    v->slide_1440 = cfg->slide_max;
    v->timer_152C = -1.0f;
    v->grip_scalar = 1.2f;
    v->ground_clear = 10000.0f;

    b3_engine_transmission_init(&v->trans, cfg);
    v->trans.omega = cfg->idle_rpm * B3_RPM_TO_RADS;
}

void b3_vehicle_full_refresh_derived(B3VehicleFull* v) {
    B3RigidBody* rb = &v->rb;
    const float(*m)[4] = rb->frame;
    // inverse frame (FUN_00040ae0 semantics)
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) rb->inv_frame[r][c] = m[r][c];
    b3_mat_invert_rigid(rb->inv_frame);
    // world inverse inertia = Rt . I0 . R (FUN_00109040 twice)
    float Rt[3][4], tmp[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) Rt[i][j] = m[j][i];
        Rt[i][3] = 0.0f;
    }
    b3_mat_mul3(rb->inv_inertia_body, Rt, tmp);
    b3_mat_mul3(rb->frame, tmp, rb->inv_inertia_world);
}
