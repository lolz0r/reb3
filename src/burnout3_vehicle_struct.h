// Recovered layout of Burnout 3's live vehicle object.
//
// This is a working map for porting the real integrator (FUN_0011BE50 and its
// callees). It is NOT complete and NOT yet used by the running harness -- it
// exists so the port has a typed target instead of raw offsets.
//
// Every offset below carries its evidence. Confidence is marked:
//   [C] confirmed by two independent derivations
//   [S] single strong derivation
//   [?] inferred, needs a second source before relying on it
//
// See docs/RE_NOTES.md sections 7 and 8.

#ifndef BURNOUT3_VEHICLE_STRUCT_H
#define BURNOUT3_VEHICLE_STRUCT_H

// ---------------------------------------------------------------------------
// Wheel record
// ---------------------------------------------------------------------------
// Array base  : vehicle + 0x820   [S] FUN_00123FD0 (`iVar16 = param_1 + 0x820`)
// Stride      : 0xC0 bytes        [C] FUN_00123FD0 (`iVar16 += 0xc0`) and
//                                     FUN_00134710 (undefined4* += 0x30)
// Count       : byte at +0x1169   [C] loop bound in every wheel loop; the
//                                     suspension solver also hardcodes 4
//
// FUN_00134710 writes the suspension attach height to vehicle+0x894 stepping
// 0xC0, and 0x894 - 0x820 = 0x74 -- i.e. that write targets field +0x74 of each
// wheel, taking the FRONT value for wheels 0-1 and the REAR value for 2+.
#define B3_WHEEL_BASE      0x820u
#define B3_WHEEL_STRIDE    0x0C0u
#define B3_WHEEL_COUNT_OFF 0x1169u

#define B3_WHEEL_ATTACH_HEIGHT 0x74u  // [C] target of the FUN_00134710 loop
#define B3_WHEEL_ACTIVE_FLAG   0xB4u  // [S] `if (*(char *)(iVar16 + 0xb4) != 0)`

#define B3_WHEEL_AT(vehicle, i) \
    ((unsigned char *)(vehicle) + B3_WHEEL_BASE + (i) * B3_WHEEL_STRIDE)

// ---------------------------------------------------------------------------
// Vehicle fields
// ---------------------------------------------------------------------------
// Suspension config, copied in by FUN_00134710 from the 0x1D0 physics config.
#define B3_V_FRONT_ATTACH_HEIGHT 0x0CA0u  // [C] config +0x0BC
#define B3_V_FRONT_SPRING_DAMP   0x0CA4u  // [C] config +0x0C4
#define B3_V_FRONT_SPRING_FORCE  0x0CA8u  // [C] config +0x0C0
#define B3_V_FRONT_SPRING_LENGTH 0x0CACu  // [C] config +0x0C8
#define B3_V_REAR_ATTACH_HEIGHT  0x0CB0u  // [C] config +0x0D8
#define B3_V_REAR_SPRING_DAMP    0x0CB4u  // [C] config +0x0D0
#define B3_V_REAR_SPRING_FORCE   0x0CB8u  // [C] config +0x0CC
#define B3_V_REAR_SPRING_LENGTH  0x0CBCu  // [C] config +0x0D4

// Transmission and engine, likewise copied by FUN_00134710.
#define B3_V_GEAR_REVERSE        0x1448u  // [C] config +0x0E0
#define B3_V_GEAR_NEUTRAL        0x144Cu  // [C] gears run contiguously to
#define B3_V_GEAR_FIRST          0x1450u  //     +0x1468 = Final Gear
#define B3_V_GEAR_FINAL          0x1468u  // [C] config +0x100
#define B3_V_IDLE_RPM            0x146Cu  // [C] config +0x104
#define B3_V_CHANGE_UP_RPM       0x1470u  // [C] config +0x108, and the shift
                                          //     test in FUN_0011D460
#define B3_V_CHANGE_DOWN_RPM     0x1474u  // [C] config +0x10C
#define B3_V_MAX_RPM             0x1480u  // [C] config +0x118
#define B3_V_TORQUE              0x1484u  // [C] config +0x11C
#define B3_V_BOOST_KICK_TORQUE   0x1494u  // [C] config +0x128
#define B3_V_BOOST_KICK_TIME     0x1498u  // [C] config +0x12C

// The copy delta is 0x1368 for config +0x104..+0x11C but 0x136C for +0x128
// onward, so there is a 4-byte field in the live struct with no config source
// somewhere in 0x1488..0x1490.
//
// IMPORTANT for the port: Peak Torque Revs (config +0x120) and Fall Off Torque
// Revs (config +0x124) are NOT copied into the live vehicle by FUN_00134710.
// Only the peak torque VALUE (+0x11C) is. The curve is therefore evaluated
// against the config object itself, reached through a pointer on the vehicle
// (candidates: +0x13F4, +0x13F8 -- both are dereferenced as object pointers in
// FUN_00132D10 and FUN_0011ECF0). The evaluation site is not yet located;
// searching for co-located +0x120/+0x124 displacements is too noisy to be
// conclusive (54 candidate windows, offsets that common in any struct).

#define B3_V_MASS_KG             0x01F0u  // [C] config +0x0B8

// Runtime state found inside FUN_0011D460.
//
// The change-up test reads:
//     if (*(float *)(v + 0x1470) <= *(float *)(v + 0x149c) * 9.549296f)
// 9.549296 == 60 / (2*pi), so +0x149C is angular velocity in rad/s converted to
// RPM. That +0x1470 is Change Up RPM was derived independently from
// FUN_00134710 -- the two agree, which is what makes this block [C].
#define B3_V_ENGINE_OMEGA        0x149Cu  // [C] rad/s; x 9.549296 -> RPM
#define B3_V_SPEED_MPH           0x13D4u  // [S] x 0.44704 -> m/s
#define B3_V_GEAR_CURRENT        0x14C8u  // [S] compared against +0x14CC
#define B3_V_GEAR_TARGET         0x14CCu  // [S]
#define B3_V_DRIVETRAIN_FLAG_A   0x1444u  // [S] byte, gates the shift branch
#define B3_V_DRIVETRAIN_FLAG_B   0x1446u  // [S] byte
#define B3_V_RESIST_COEF         0x1360u  // [C] linear resistance coef; verified
                                          //     by emulation, 7/7 exact
// Identified by differential emulation (tools/probe_fields.py), each verified
// against the real instructions across multiple values:
#define B3_V_DOWNFORCE_COEF      0x1364u  // [C] vertical: -(c*mph*0.1 + 10)*mass
#define B3_V_RESIST_QUAD         0x1408u  // [C] quadratic: -k*speed*q*q

// The game's own m/s -> mph constant, taken from FUN_0011D460. Note it is NOT
// the true 2.2369363 -- Criterion's value is 0.02% high, and the downforce term
// uses this rather than the stored speed_mph field at +0x13D4.
#define B3_MS_TO_MPH_GAME 2.2374146f
// Gravity is 10.0, not 9.81 -- confirmed by emulation (-10000 for a 1000kg car).
#define B3_GRAVITY 10.0f
// +0xB0..+0xBC is a velocity DIRECTION vector plus a speed magnitude, not a
// force vector: +0xBC is compared directly against speed_mph * 0.44704 in
// FUN_0011D460.                                                          [S]
#define B3_V_VEL_DIR_X           0x00B0u
#define B3_V_VEL_DIR_Y           0x00B4u
#define B3_V_VEL_DIR_Z           0x00B8u
#define B3_V_SPEED_MS            0x00BCu
#define B3_V_FORCE_ACCUM_X       0x00F0u  // [S] resistance force accumulates here
#define B3_V_FORCE_ACCUM_Y       0x00F4u
#define B3_V_FORCE_ACCUM_Z       0x00F8u

// FUN_00123FD0 computes sin() of this via a minimax polynomial after scaling by
// -0.017453292 (= -pi/180), so it is an angle in degrees.
#define B3_V_STEER_ANGLE_DEG     0x1164u  // [S]

// Accumulators the suspension solver adds per-wheel contributions into.
#define B3_V_ACC_0               0x0130u  // [?] += wheel_term * w
#define B3_V_ACC_1               0x0134u  // [?]
#define B3_V_ACC_2               0x0138u  // [?]
#define B3_V_ACC_3               0x013Cu  // [?]

// Unit note: the game stores road speed in mph and converts with 0.44704 where
// SI is wanted. A port must preserve that or the numbers drift.
#define B3_MPH_TO_MS   0.44704f
#define B3_RADS_TO_RPM 9.549296f

#endif // BURNOUT3_VEHICLE_STRUCT_H
