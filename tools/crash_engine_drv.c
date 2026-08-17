/* Reference driver for tools/validate_sfx.py section 6.
 *
 * Runs the RETAIL crashed branch of the vehicle main path -- FUN_0011BE50
 * @0x0011BE8B..0x0011BED4 -- over the real ported engine/transmission update
 * (b3_engine_transmission_update == FUN_00121560, verified by
 * tools/validate_port.py), and prints the engine rpm per frame.  The SFX
 * module's own crash-engine law must reproduce this trace.
 *
 * usage: crash_engine_drv <start rpm> <idle rpm> <max rpm> <frames> [speed]
 */
#include "burnout3_vehicle_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* burnout3_vehicle_sim.c's full-pipeline half calls the harness ground probe;
 * nothing on the engine path does, so a stub satisfies the linker. */
int b3_ground_probe(float x, float y, float z, float* out_y, float out_n[3]);
int b3_ground_probe(float x, float y, float z, float* out_y, float out_n[3])
{
    (void)x; (void)y; (void)z;
    if (out_y) *out_y = 0.0f;
    if (out_n) { out_n[0] = 0.0f; out_n[1] = 1.0f; out_n[2] = 0.0f; }
    return -1;
}

int main(int argc, char** argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <start rpm> <idle rpm> <max rpm> <frames> "
                        "[speed m/s]\n", argv[0]);
        return 2;
    }
    float start_rpm = (float)atof(argv[1]);
    float idle_rpm  = (float)atof(argv[2]);
    float max_rpm   = (float)atof(argv[3]);
    int   frames    = atoi(argv[4]);
    float speed     = (argc > 5) ? (float)atof(argv[5]) : 20.0f;

    B3PhysicsConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    /* a plausible six-speed set; only the ratios' SIGNS and count matter in
     * neutral (gear_count), and the crashed path never leaves neutral. */
    static const float gears[9] = { -3.2f, 0.0f, 3.1f, 2.2f, 1.6f, 1.25f,
                                     1.0f, 0.85f, 3.9f };
    for (int i = 0; i < 9; i++) cfg.gear[i] = gears[i];
    cfg.idle_rpm            = idle_rpm;
    cfg.change_up_rpm       = max_rpm * 0.95f;
    cfg.change_down_rpm     = max_rpm * 0.6f;
    cfg.max_rpm             = max_rpm;
    cfg.torque              = 600.0f;
    cfg.peak_torque_revs    = max_rpm * 0.72f;
    cfg.falloff_torque_revs = max_rpm * 0.95f;

    B3EngineTransmission t;
    b3_engine_transmission_init(&t, &cfg);
    /* pre-crash state: a forward gear at the impact rpm */
    t.gear  = 4;
    t.omega = start_rpm * B3_RPM_TO_RADS;

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < frames; i++) {
        /* FUN_0011BE50 @0x0011BE8B: gear -> neutral, in-shift flag, 0.35 s */
        if (t.gear != 0) {
            t.gear = 0;
            t.shifting = 1;
            t.shift_timer = 0.35f;
        }
        /* @0x0011BEC6: PUSH 0 x3 (throttle, wheel omega, kick), EDI = 0 */
        b3_engine_transmission_update(&t, 0.0f, 0.0f, 0, 0, dt, speed,
                                      0.0f, 0.0f, 0);
        printf("%d %.9g\n", i, (double)(t.omega * B3_RADS_TO_RPM_));
    }
    return 0;
}
