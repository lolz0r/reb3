#!/usr/bin/env python3
"""
Differential test: run Burnout 3's real x86 physics code under emulation and
check the ported C equations reproduce it exactly.

This is the missing verification loop. Before this, a ported equation that
produced plausible numbers was indistinguishable from a correct one, and every
error in this project was caught only by luck -- a second derivation happening
to disagree. Now each ported block is checked against the actual instructions.

Add a case here for every block ported into src/burnout3_vehicle_sim.c. A port
without a passing case here should be treated as unverified.

Requires: pip install unicorn
"""
import importlib.util
import math
import struct
import sys

_spec = importlib.util.spec_from_file_location("ev", "tools/emulate_vehicle.py")
ev = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ev)

DRIVETRAIN = 0x0011D460

# Vehicle offsets the cases below poke and read.
O_VEL_X, O_VEL_Y, O_VEL_Z, O_SPEED_MS = 0x0B0, 0x0B4, 0x0B8, 0x0BC
O_ACC_X, O_ACC_Y, O_ACC_Z, O_ACC_W = 0x0F0, 0x0F4, 0x0F8, 0x0FC
O_MASS = 0x1F0
O_RESIST, O_KQUAD, O_SPEED_MPH, O_Q = 0x1360, 0x13C0, 0x13D4, 0x1408

MPH_TO_MS = 0.44704


def model_resistance(rc, speed_ms, k_quad, q, vel_dir):
    """The ported equation from src/burnout3_vehicle_sim.c (b3_resistance_force).

    Returns 4 components: xyz force plus w = speed*scale, matching the game's
    4-wide accumulator.
    """
    extra = k_quad * speed_ms * q * q
    s = -(rc * speed_ms) - extra
    return tuple(d * s for d in vel_dir) + (speed_ms * s,)


MS_TO_MPH_GAME = 2.2374146   # the game's constant, not the true 2.2369363
GRAVITY = 10.0               # the game's value, not 9.81


def model_vertical(downforce_coef, speed_ms, mass):
    """Vertical term from FUN_0011D460, verified 16/16 by emulation."""
    return -(downforce_coef * (speed_ms * MS_TO_MPH_GAME) * 0.1 + GRAVITY) * mass


O_DOWNFORCE = 0x1364


def emulate(rc, speed_ms, k_quad, q, vel_dir, mass=1000.0, downforce=0.0):
    img = ev.build_vehicle({
        O_RESIST: rc, O_SPEED_MS: speed_ms, O_KQUAD: k_quad, O_Q: q,
        O_DOWNFORCE: downforce,
        O_VEL_X: vel_dir[0], O_VEL_Y: vel_dir[1], O_VEL_Z: vel_dir[2],
        O_SPEED_MPH: speed_ms / MPH_TO_MS, O_MASS: mass,
    })
    _, after, err = ev.run(DRIVETRAIN, img)
    if err:
        raise RuntimeError("emulation faulted: " + err)
    return (struct.unpack_from('<f', after, O_ACC_X)[0],
            struct.unpack_from('<f', after, O_ACC_Y)[0],
            struct.unpack_from('<f', after, O_ACC_Z)[0],
            struct.unpack_from('<f', after, O_ACC_W)[0])


CASES = [
    # rc,   v,    k,    q,   direction
    (0.50, 40.0, 0.01, 1.0, (0.0, 0.0, 1.0)),
    (0.25, 40.0, 0.01, 1.0, (0.0, 0.0, 1.0)),
    (0.50, 20.0, 0.00, 1.0, (0.0, 0.0, 1.0)),
    (0.50, 40.0, 0.02, 3.0, (0.0, 0.0, 1.0)),
    (0.50, 40.0, 0.01, 1.0, (0.6, 0.0, 0.8)),
    (0.10,  5.0, 0.05, 2.0, (-1.0, 0.0, 0.0)),
    (0.75, 60.0, 0.00, 0.0, (0.0, 0.0, -1.0)),
]

TOL = 1e-3

# ===========================================================================
# Engine / transmission update -- FUN_00121560, called with ESI = &v->trans
# (vehicle+0x1448), EDI = boost flag (byte v+0x13FC & 4), and three stack args
# (throttle-brake, driven wheel angular velocity, torque-kick flag). Verified
# call sites: 0x0011BED4 (FUN_0011BE50) and 0x0011F3D2 (FUN_0011ECF0), both
# doing LEA ESI,[EBX+0x1448] first. Returns the drive torque in ST0; the
# caller stores it at v+0x1520 and FUN_0011D460 splits it 50/50 onto the rear
# wheels. The model below mirrors b3_engine_transmission_update() in
# src/burnout3_vehicle_sim.c line for line.
# ===========================================================================
from unicorn.x86_const import UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_ECX

ENGINE = 0x00121560
D460 = DRIVETRAIN
ECF0 = 0x0011ECF0

RPM_TO_RADS = 0.10471976          # written by FUN_00134710 (config -> live)
RADS_TO_RPM = 9.549296
RAND_SCALE = 2.3283064365386963e-10   # 1/2^32; _DAT_0054F46C, seeded from
                                      # the float at 0x003B191C by 0x264880
DT_GLOBAL = 0x0060EA1C            # per-frame dt global (BSS)
RAND_SCALE_GLOBAL = 0x0054F46C    # BSS; must be seeded for emulation

TRANS = 0x1448                    # transmission block base inside the vehicle
OT = dict(idle=TRANS+0x24, up=TRANS+0x28, down=TRANS+0x2C, max=TRANS+0x38,
          torque=TRANS+0x3C, limit=TRANS+0x40, peak=TRANS+0x44,
          falloff=TRANS+0x48, kick_t=TRANS+0x4C, kick_time=TRANS+0x50,
          omega=TRANS+0x54, timer=TRANS+0x58, shifting=TRANS+0x5C,
          inhibit=TRANS+0x60, rng_a=TRANS+0x68, rng_c=TRANS+0x6C,
          flag70=TRANS+0x70, gear=TRANS+0x80, ngears=TRANS+0x84,
          upblk=TRANS+0x88, dnblk=TRANS+0x8C)

ENGINE_BASE = {
    # gears: Reverse, Neutral, 1st..6th, Final (live copies of config +0xE0..)
    'gears': [-3.0, 0.0, 3.5, 2.4, 1.8, 1.4, 1.15, 0.95, 4.0],
    'idle': 1000.0, 'up': 6800.0, 'down': 4200.0, 'max': 6800.0,
    'torque': 630.0, 'limit': 6800.0,
    'peak': 5000.0 * RPM_TO_RADS, 'falloff': 6500.0 * RPM_TO_RADS,
    'kick_t': 2.0, 'kick_time': 1.0,
    'omega': 400.0, 'timer': 0.0, 'shifting': 0, 'inhibit': 0,
    'rng_a': 0xFD462907, 'rng_c': 0x02B9D6F8,   # seeds from FUN_001214A0
    'flag70': 0, 'gear': 3, 'ngears': 6, 'upblk': 0.0, 'dnblk': 0.0,
    'speed_ms': 40.0, 'max_boost_mph': 141.0,
}


def f2u(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def lcg(a, c):
    """The game's PRNG step (FUN_00048760 and inlined in FUN_00121560).
    state = state*0x10000 + (state >> 16 arithmetic) + inc; inc += state."""
    sa = a - 0x100000000 if a >= 0x80000000 else a
    na = (sa * 0x10000 + (sa >> 16) + c) & 0xFFFFFFFF
    nc = (na + c) & 0xFFFFFFFF
    return na, nc, float(na) * RAND_SCALE


def model_engine(st, throttle, wheel_omega, kick, boost, dt,
                 boost_elapsed=None, boost_ramp_done=0):
    """Mirror of b3_engine_transmission_update() (FUN_00121560)."""
    s = dict(st)
    s['gears'] = list(st['gears'])
    if boost and throttle < 0.9:
        throttle = 0.9
    gear = s['gear']
    if gear == 1 and wheel_omega < 0.0:
        wheel_omega = wheel_omega * 100.0
    target = s['gears'][8] * s['gears'][gear + 1] * wheel_omega
    idle_omega = s['idle'] * RPM_TO_RADS
    down_omega = s['down'] * RPM_TO_RADS
    if gear == 0:
        target = s['limit'] * RPM_TO_RADS * throttle
    if s['shifting'] == 0:
        if s['flag70'] == 0:
            s['dnblk'] = s['dnblk'] - dt
            up_del = s['upblk'] - dt
            s['upblk'] = up_del
            if ((target < down_omega and gear > 1 and throttle < 0.5)
                    or (target < down_omega * 0.75 and gear > 1)):
                if s['dnblk'] <= 0.0:
                    s['timer'] = 0.35
                    s['gear'] = gear - 1
                    s['shifting'] = 1
                    s['upblk'] = 1.0
            elif (s['up'] * RPM_TO_RADS < target and gear > 0
                  and gear < s['ngears'] and s['inhibit'] == 0
                  and up_del <= 0.0):
                ratio = s['gears'][gear + 1]     # note: OLD gear's ratio
                s['gear'] = gear + 1
                s['shifting'] = 1
                if not boost:
                    s['timer'] = 0.35
                    s['dnblk'] = 1.0
                else:
                    s['timer'] = 0.1
                    s['dnblk'] = 0.1
                target = s['gears'][8] * ratio * wheel_omega
    else:
        s['timer'] = s['timer'] - dt
        if 0.0 < s['timer']:
            throttle = throttle * 0.1           # torque cut during the shift
        else:
            s['shifting'] = 0
    gear = s['gear']
    limit_omega = s['limit'] * RPM_TO_RADS
    if gear == -1:
        limit_omega *= 0.8
    elif gear != 0:
        if boost:
            limit_omega *= 1.1
        elif s['flag70'] != 0:
            limit_omega *= 1.05
    if s['omega'] < limit_omega - 0.001:
        s['limit'] = s['max']
        if gear == 0:
            if s['max'] >= 6000.0:
                rate_up, rate_dn = 45.0, 16.0
            else:
                rate_up, rate_dn = 22.5, 9.6
            s['rng_a'], s['rng_c'], r = lcg(s['rng_a'], s['rng_c'])
            target = (r * 0.06 + 0.97) * target
        else:
            rate_dn = 8.0
            rate_up = 45.0 if boost else 15.0
        hi = rate_up + s['omega']
        if target <= hi:
            lo = s['omega'] - rate_dn
            s['omega'] = target if lo <= target else lo
        else:
            s['omega'] = hi
        if (s['omega'] < idle_omega) or (s['speed_ms'] * 2.2369363 < 1.0):
            if s['omega'] <= 0.1 + idle_omega:
                s['rng_a'], s['rng_c'], r = lcg(s['rng_a'], s['rng_c'])
                s['omega'] = r * 16.0 + idle_omega
            if throttle <= 0.0 and (s['gear'] == 1 or s['gear'] == -1
                                    or s['flag70'] != 0):
                s['gear'] = 0
                s['shifting'] = 1
                s['timer'] = 0.035
    else:
        # rev limiter: randomise a slightly lower limit, knock 20 rad/s off,
        # and turn the throttle term into engine braking
        if s['limit'] == s['max'] and not boost:
            s['rng_a'], s['rng_c'], r = lcg(s['rng_a'], s['rng_c'])
            if gear == 0:
                s['limit'] = s['max'] - r * s['max'] * 0.1
            else:
                s['limit'] = (s['max'] - r * 50.0) - 70.0
        s['omega'] = s['omega'] - 20.0
        throttle = -(target / limit_omega)
        if throttle > 0.0:
            throttle = 0.0
    # torque curve on the NEW omega
    om = s['omega']
    if om >= s['peak']:
        if om <= s['falloff']:
            scale = 1.0
        else:
            scale = ((limit_omega - om) / (limit_omega - s['falloff'])) * 0.25 + 0.75
    else:
        scale = (om / s['peak'] + 1.0) * 0.5
    if kick:
        scale = 2.0
    if s['gear'] == -1:
        scale = (limit_omega - om) / limit_omega
    if boost and s['speed_ms'] * 2.2369363 < s['max_boost_mph']:
        el = boost_elapsed if boost_elapsed is not None else 0.0
        if not boost_ramp_done:
            if el < s['kick_time']:
                scale = (1.0 - el / s['kick_time']) * s['kick_t'] + 1.0
        else:
            scale = s['kick_t'] + 1.0
    s['rng_a'], s['rng_c'], r = lcg(s['rng_a'], s['rng_c'])
    ret = ((r * 0.1 + 0.95) * s['gears'][s['gear'] + 1] * s['torque']
           * s['gears'][8] * scale * throttle)
    s['ret'] = f32(ret)
    return s


def emulate_engine(st, throttle, wheel_omega, kick, boost, dt,
                   boost_elapsed=None, boost_ramp_done=0):
    ov = {}
    for i, g in enumerate(st['gears']):
        ov[TRANS + i * 4] = g
    for k in ('idle', 'up', 'down', 'max', 'torque', 'limit', 'peak',
              'falloff', 'kick_t', 'kick_time', 'omega', 'timer',
              'upblk', 'dnblk'):
        ov[OT[k]] = st[k]
    for k in ('shifting', 'inhibit', 'rng_a', 'rng_c', 'flag70', 'gear',
              'ngears'):
        ov[OT[k]] = st[k]
    ov[0xBC] = st['speed_ms']
    ov[0x13D4] = st['max_boost_mph']
    mw = {DT_GLOBAL: struct.pack('<f', dt),
          RAND_SCALE_GLOBAL: struct.pack('<f', RAND_SCALE)}
    if boost_elapsed is not None:
        # the boost branch reads (owner+0x13F4) -> obj+0x10DC minus obj+0x11C0
        # (elapsed boost time) and the flag byte obj+0x11F1
        mw[ev.SCRATCH + 0x10DC] = struct.pack('<f', boost_elapsed)
        mw[ev.SCRATCH + 0x11C0] = struct.pack('<f', 0.0)
        mw[ev.SCRATCH + 0x11F1] = bytes([boost_ramp_done])
    img = ev.build_vehicle(ov)
    tr, after, err = ev.run(ENGINE, img,
                            stack_args=[f2u(throttle), f2u(wheel_omega), kick],
                            regs={UC_X86_REG_ESI: ev.VEHICLE + TRANS,
                                  UC_X86_REG_EDI: 1 if boost else 0},
                            mem_writes=mw, ret_f32=True)
    if err:
        raise RuntimeError("emulation faulted: " + err)
    out = {}
    for k in ('omega', 'timer', 'limit', 'upblk', 'dnblk'):
        out[k] = struct.unpack_from('<f', after, OT[k])[0]
    for k in ('gear', 'shifting', 'rng_a', 'rng_c'):
        out[k] = struct.unpack_from('<I', after, OT[k])[0]
    out['ret'] = tr.ret_f32
    return out


DT = 1.0 / 60.0

# name, state overrides, call kwargs. Each case asserts the FULL post-state:
# return torque, omega, gear, shifting flag, shift timer, rev limit, both
# shift-block timers, and the PRNG state pair.
ENGINE_CASES = [
    ("curve below peak",   dict(omega=400.0),
     dict(throttle=1.0, wheel_omega=400.0 / 7.2, kick=0, boost=0, dt=DT)),
    ("curve flat region",  dict(omega=534.0),
     dict(throttle=1.0, wheel_omega=536.0 / 7.2, kick=0, boost=0, dt=DT)),
    ("curve falloff",      dict(omega=690.0),
     dict(throttle=1.0, wheel_omega=695.0 / 7.2, kick=0, boost=0, dt=DT)),
    ("slew rate limit up", dict(omega=300.0),
     dict(throttle=1.0, wheel_omega=500.0 / 7.2, kick=0, boost=0, dt=DT)),
    ("slew rate limit dn", dict(omega=600.0),
     dict(throttle=1.0, wheel_omega=300.0 / 7.2, kick=0, boost=0, dt=DT)),
    ("upshift 2->3",       dict(gear=2, omega=700.0),
     dict(throttle=1.0, wheel_omega=75.0, kick=0, boost=0, dt=DT)),
    ("upshift blocked",    dict(gear=2, omega=700.0, upblk=0.5),
     dict(throttle=1.0, wheel_omega=75.0, kick=0, boost=0, dt=DT)),
    ("top gear no upshift", dict(gear=6, omega=700.0),
     dict(throttle=1.0, wheel_omega=190.0, kick=0, boost=0, dt=DT)),
    ("downshift 3->2",     dict(gear=3, omega=300.0),
     dict(throttle=1.0, wheel_omega=20.0, kick=0, boost=0, dt=DT)),
    ("mid-shift torque cut", dict(shifting=1, timer=0.2, omega=500.0),
     dict(throttle=1.0, wheel_omega=69.0, kick=0, boost=0, dt=DT)),
    ("shift completes",    dict(shifting=1, timer=0.005, omega=500.0),
     dict(throttle=1.0, wheel_omega=69.0, kick=0, boost=0, dt=DT)),
    ("rev limiter",        dict(gear=6, omega=6800.0 * RPM_TO_RADS + 1.0),
     dict(throttle=1.0, wheel_omega=200.0, kick=0, boost=0, dt=DT)),
    ("neutral rev",        dict(gear=0, omega=300.0),
     dict(throttle=0.7, wheel_omega=0.0, kick=0, boost=0, dt=DT)),
    ("reverse",            dict(gear=-1, omega=300.0),
     dict(throttle=1.0, wheel_omega=-30.0, kick=0, boost=0, dt=DT)),
    ("idle drop to neutral", dict(gear=1, omega=104.0, speed_ms=0.1),
     dict(throttle=0.0, wheel_omega=0.0, kick=0, boost=0, dt=DT)),
    ("torque kick",        dict(omega=400.0),
     dict(throttle=1.0, wheel_omega=55.0, kick=1, boost=0, dt=DT)),
    ("boost ramp",         dict(omega=500.0, speed_ms=20.0),
     dict(throttle=0.5, wheel_omega=69.0, kick=0, boost=1, dt=DT,
          boost_elapsed=0.5)),
    ("boost sustained",    dict(omega=500.0, speed_ms=20.0),
     dict(throttle=0.5, wheel_omega=69.0, kick=0, boost=1, dt=DT,
          boost_elapsed=0.5, boost_ramp_done=1)),
    ("backward spin in 1st", dict(gear=1, omega=200.0),
     dict(throttle=1.0, wheel_omega=-5.0, kick=0, boost=0, dt=DT)),
]


def run_engine_cases():
    fails = 0
    print("\nengine/transmission update (FUN_00121560), full post-state:")
    for name, ovr, kw in ENGINE_CASES:
        st = dict(ENGINE_BASE, **ovr)
        try:
            emu = emulate_engine(st, **kw)
        except RuntimeError as e:
            print("  %-24s %s" % (name, e))
            fails += 1
            continue
        m = model_engine(st, **kw)
        bad = []
        for k in ('gear', 'shifting', 'rng_a', 'rng_c'):
            if emu[k] != (m[k] & 0xFFFFFFFF):
                bad.append((k, m[k], emu[k]))
        for k in ('omega', 'timer', 'limit', 'upblk', 'dnblk'):
            if abs(emu[k] - m[k]) > max(1e-3, abs(m[k]) * 1e-5):
                bad.append((k, m[k], emu[k]))
        if abs(emu['ret'] - m['ret']) > max(1e-2, abs(m['ret']) * 1e-4):
            bad.append(('ret', m['ret'], emu['ret']))
        if bad:
            fails += 1
            print("  %-24s FAIL" % name)
            for k, mv, evv in bad:
                print("      %-8s model %r  emu %r" % (k, mv, evv))
        else:
            print("  %-24s OK   torque %10.2f  gear %2d  omega %7.2f"
                  % (name, emu['ret'],
                     emu['gear'] - 0x100000000 if emu['gear'] >= 0x80000000
                     else emu['gear'], emu['omega']))
    return fails


# ===========================================================================
# Drive torque -> wheels -- FUN_0011D460. The stored drive torque (v+0x1520,
# written by FUN_0011ECF0 from FUN_00121560's return) is split 50/50 onto the
# rear wheels' torque accumulators (wheel+0x54), then every wheel integrates
# omega (wheel+0x5C) += torque * 0.04 * dt and clears the accumulator.
# Mirrors b3_drive_torque_to_wheels() in src/burnout3_vehicle_sim.c.
# ===========================================================================
WHEEL0, WSTRIDE, WO_TORQUE, WO_OMEGA = 0x820, 0xC0, 0x54, 0x5C

WHEEL_CASES = [
    # drive torque, dt, initial per-wheel torque, initial per-wheel omega
    (6000.0, 1 / 60., (0, 0, 0, 0), (10, 10, 10, 10)),
    (-2000.0, 1 / 60., (100, 0, 50, 0), (0, 5, 20, 30)),
    (0.0, 1 / 30., (10, 20, 30, 40), (1, 2, 3, 4)),
    (14000.0, 1 / 60., (0, 0, 0, 0), (0, 0, 0, 0)),
]


def run_wheel_cases():
    fails = 0
    print("\ndrive torque -> wheel omega (FUN_0011D460):")
    for T, dt, torques, omegas in WHEEL_CASES:
        ov = {0x1520: T}
        for i in range(4):
            ov[WHEEL0 + i * WSTRIDE + WO_TORQUE] = torques[i]
            ov[WHEEL0 + i * WSTRIDE + WO_OMEGA] = omegas[i]
        img = ev.build_vehicle(ov)
        tr, after, err = ev.run(D460, img, stack_args=[ev.VEHICLE, f2u(dt)])
        if err:
            print("  T=%8.0f dt=%.4f  FAULT %s" % (T, dt, err))
            fails += 1
            continue
        ok = True
        for i in range(4):
            t_a = struct.unpack_from('<f', after,
                                     WHEEL0 + i * WSTRIDE + WO_TORQUE)[0]
            o_a = struct.unpack_from('<f', after,
                                     WHEEL0 + i * WSTRIDE + WO_OMEGA)[0]
            drive = 0.5 * T if i >= 2 else 0.0
            expect = omegas[i] + (torques[i] + drive) * 0.04 * dt
            ok = ok and (abs(o_a - expect) < max(1e-3, abs(expect) * 1e-5)
                         and t_a == 0.0)
        print("  T=%8.0f dt=%.4f  %s" % (T, dt, "OK" if ok else "FAIL"))
        fails += not ok
    return fails


# ===========================================================================
# Gear engagement -- FUN_0011ECF0 (input stage; __thiscall, ECX = vehicle,
# speed float passed in ESI by the outer frame). When gear < 2 and no shift
# is in progress it decides neutral -> 1st / reverse from throttle (+0x1400),
# brake (+0x1404) and the forward velocity component. It then calls
# FUN_00121560 and stores the returned drive torque at +0x1520, so these
# cases also verify the two stages composed end to end.
# Mirrors b3_gear_engage() in src/burnout3_vehicle_sim.c.
# ===========================================================================
def model_engage(gear, throttle, brake, fwd_vel, speed_ms, flag_1446=0):
    """Returns (gear, shifting, timer, new_throttle_field, new_brake_field)."""
    shifting, timer = 0, 0.0
    if gear < 2 and not shifting:
        if (gear == -1 or brake <= throttle or brake <= 0.1
                or 0.5 <= fwd_vel or flag_1446):
            if (gear == 1 or throttle <= brake or throttle <= 0.1
                    or fwd_vel <= -10.5):
                if gear == 0 and 0.1 < speed_ms:
                    brake = 1.0
                return gear, 0, 0.0, throttle, brake
            gear = 1
        else:
            gear = -1
        shifting, timer = 1, 0.35
    if gear < 0:
        throttle, brake = brake, throttle
    return gear, shifting, timer, throttle, brake


ENGAGE_CASES = [
    # name, gear, throttle, brake, speed, vel_z, rear wheel spin, expected gear
    ("neutral+throttle -> 1st",  0, 1.0, 0.0, 0.5, 1.0, 0.0, 1),
    ("neutral+brake -> reverse", 0, 0.0, 1.0, 0.2, 0.0, 0.0, -1),
    ("neutral idle stays",       0, 0.0, 0.0, 0.0, 0.0, 0.0, 0),
    ("in 3rd stays",             3, 1.0, 0.0, 30.0, 1.0, 90.0, 3),
    ("reverse+throttle -> 1st", -1, 1.0, 0.0, 0.5, 0.0, 0.0, 1),
    ("rolling fwd, no reverse",  0, 0.0, 1.0, 5.0, 1.0, 0.0, 0),
]


def run_engage_cases():
    fails = 0
    print("\ngear engagement + composed drive torque (FUN_0011ECF0):")
    for name, gear, thr, brk, spd, velz, spin, expect in ENGAGE_CASES:
        st = dict(ENGINE_BASE, omega=300.0, gear=gear, speed_ms=spd)
        ov = {0x1400: thr, 0x1404: brk, 0xBC: spd,
              0xB0: 0.0, 0xB4: 0.0, 0xB8: velz,
              0x9FC: spin, 0xABC: spin,
              0x14A4: 0, 0x14A0: 0.0}
        for i, g in enumerate(st['gears']):
            ov[TRANS + i * 4] = g
        for k in ('idle', 'up', 'down', 'max', 'torque', 'limit', 'peak',
                  'falloff', 'kick_t', 'kick_time', 'omega', 'upblk', 'dnblk'):
            ov[OT[k]] = st[k]
        for k in ('rng_a', 'rng_c', 'gear', 'ngears'):
            ov[OT[k]] = st[k]
        ov[0x13D4] = st['max_boost_mph']
        mw = {DT_GLOBAL: struct.pack('<f', DT),
              RAND_SCALE_GLOBAL: struct.pack('<f', RAND_SCALE),
              # *(v+0x13F4)+0x179C == 1 selects the in-race input path
              ev.SCRATCH + 0x179C: struct.pack('<I', 1)}
        img = ev.build_vehicle(ov)
        tr, after, err = ev.run(ECF0, img, stack_args=[],
                                regs={UC_X86_REG_ECX: ev.VEHICLE,
                                      UC_X86_REG_ESI: f2u(spd),
                                      UC_X86_REG_EDI: 0},
                                mem_writes=mw)
        if err:
            print("  %-26s FAULT %s" % (name, err))
            fails += 1
            continue
        g_a = struct.unpack_from('<i', after, 0x14C8)[0]
        sh_a = struct.unpack_from('<I', after, 0x14A4)[0]
        tq_a = struct.unpack_from('<f', after, 0x1520)[0]
        # model: engage, then the engine update composed on the new state
        mg, msh, mtimer, mthr, mbrk = model_engage(gear, thr, brk,
                                                   velz * spd, spd)
        ms = dict(st, gear=mg, shifting=msh, timer=mtimer)
        m = model_engine(ms, throttle=mthr - mbrk, wheel_omega=spin,
                         kick=0, boost=0, dt=DT)
        ok = (g_a == expect and g_a == m['gear'] and sh_a == m['shifting']
              and abs(tq_a - m['ret']) < max(1e-2, abs(m['ret']) * 1e-4))
        print("  %-26s gear %2d  torque %10.2f  %s"
              % (name, g_a, tq_a, "OK" if ok else
                 "FAIL (model gear %d torque %.2f)" % (m['gear'], m['ret'])))
        fails += not ok
    return fails


# ===========================================================================
# Suspension solver -- FUN_00123FD0 (force pass) and FUN_001239C0 (ray-cast
# pre-pass, airborne path). Wheel record base 0x820 stride 0xC0:
#   +0x00 world pos, +0x10 contact pt, +0x20 contact normal, +0x30 prev-frame
#   copy, +0x50 pre-pass height, +0x58 spin, +0x5C omega, +0x60 prev spring
#   length, +0x64 current spring length, +0x74 attach height, +0xB2 bump flag,
#   +0xB3 contact flag. Config at v+0xCA0..0xCBC (FUN_00134710). The models
# below mirror b3_wheel_spring_damper / b3_wheel_droop / b3_wheel_spin_update
# / b3_wheel_force_apply / b3_wheel_prepass_airborne line for line.
# ===========================================================================
SUSP = 0x00123FD0
PREPASS = 0x001239C0
S_OBJ_CC4 = 0x1000    # scratch offsets: *(v+0xCC4) aux object
S_WFRAME = 0x3000     # per-wheel frame matrices
S_GROUND = 0x5000     # *(v+0x200) ground soup {count, recs, types}
W0, WSTR = 0x820, 0xC0


def model_spring_damper(k, c, ln, attach, cur, prev, dt, in_race):
    """Mirror of b3_wheel_spring_damper (f32 at every step)."""
    bump = 1 if f32(cur - prev) > 0.12 else 0
    comp = f32(attach - cur)
    lo = f32(ln * 0.25)
    if lo <= comp:
        if in_race and f32(ln * 0.75) < comp:
            comp = f32(ln * 0.75)
    else:
        comp = lo
        cur = f32(attach - lo)
    vel = f32(f32(cur - prev) / dt)
    prev = cur
    F = f32(f32(-(f32(f32(comp - ln) * k))) + f32(vel * c))
    if in_race:
        ratio = f32(comp / f32(ln * 0.75))
        if 0.5 < ratio:
            F = f32(F * f32(f32(1.0 - ratio) * 2.0))
    return F, cur, prev, bump


def model_droop(k, ln, attach, dt):
    hi = f32(ln * 0.75)
    cur = f32(attach - hi)
    t = f32(1.0 - f32(ln / f32(attach - f32(attach - hi))))
    t = min(max(t, 0.0), 1.0)
    cur = f32(cur - f32(f32(f32(t * k) * dt) * dt) * dt)
    ext = f32(attach - cur)
    lo = f32(ln * 0.25)
    if ext < lo:
        cur = f32(attach - lo)
    elif hi < ext:
        cur = f32(attach - hi)
    return cur


def model_spin(spin, omega, decay, contact, dt):
    if decay:
        w0 = omega
        omega = f32(w0 * 0.99)
        if contact:
            omega = f32(f32(w0 * 0.99) * 0.8)
    s = f32(f32(dt * omega) + spin)
    if s > 628.31854:
        s = f32(s - 628.31854)
    elif s < -628.31854:
        s = f32(s + 628.31854)
    return s, omega


def model_force_apply(normal, F, wheel_pos, frame_pos, accF, accT):
    fv = [f32(n * F) for n in normal]
    for i in range(4):
        accF[i] = f32(accF[i] + fv[i])
    r = [f32(wheel_pos[i] - frame_pos[i]) for i in range(3)]
    accT[0] = f32(accT[0] + f32(f32(r[1] * fv[2]) - f32(r[2] * fv[1])))
    accT[1] = f32(accT[1] + f32(f32(r[2] * fv[0]) - f32(r[0] * fv[2])))
    accT[2] = f32(accT[2] + f32(f32(r[0] * fv[1]) - f32(r[1] * fv[0])))
    return accF, accT


SUSP_CFG = dict(front_force=54000.0, front_damp=5300.0, front_len=0.18,
                rear_force=56000.0, rear_damp=5300.0, rear_len=0.18,
                front_attach=0.11, rear_attach=0.11)


def susp_build(wheels, cfg, in_race=0, scrape_off=1):
    """Vehicle image + mem_writes for a FUN_00123FD0 / FUN_001239C0 run.

    wheels: per wheel dict: surface (0 normal, 3 skip), contact 0/1, attach,
    cur, prev, normal[4], local[4] (wheel-frame local position), spin, omega,
    h (wheel+0x50), pos8 (prev world pos, 8 floats +0x00..0x1C).
    """
    S = ev.SCRATCH
    n = len(wheels)
    fields = {0x200: S + S_GROUND, 0xCC4: S + S_OBJ_CC4, 0x1169: n,
              0xCA0: cfg['front_attach'], 0xCA4: cfg['front_damp'],
              0xCA8: cfg['front_force'], 0xCAC: cfg['front_len'],
              0xCB0: cfg['rear_attach'], 0xCB4: cfg['rear_damp'],
              0xCB8: cfg['rear_force'], 0xCBC: cfg['rear_len']}
    for i in range(n):
        fields[0xCC8 + 4 * i] = S + S_WFRAME + 0x40 * i
    mw = {S + S_GROUND: struct.pack('<III', 0, S + S_GROUND + 0x100,
                                    S + S_GROUND + 0x800),
          S + S_OBJ_CC4 + 0x4AC:
              bytes(w.get('surface', 0) for w in wheels)}
    for i, w in enumerate(wheels):
        m = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0] + \
            list(w.get('local', (0.8, 0.0, 1.2, 1.0)))
        mw[S + S_WFRAME + 0x40 * i] = struct.pack('<16f', *m)
        b = ev.VEHICLE + W0 + WSTR * i
        mw[b + 0x20] = struct.pack('<4f', *w.get('normal', (0., 1., 0., 0.)))
        mw[b + 0x50] = struct.pack('<f', w.get('h', 0.0))
        mw[b + 0x58] = struct.pack('<2f', w.get('spin', 0.), w.get('omega', 0.))
        mw[b + 0x60] = struct.pack('<2f', w.get('prev', 0.), w.get('cur', 0.))
        mw[b + 0x74] = struct.pack('<f', w.get('attach', 0.11))
        mw[b + 0xB3] = bytes([w.get('contact', 0)])
        if 'pos8' in w:
            mw[b + 0x00] = struct.pack('<8f', *w['pos8'])
    if in_race:
        mw[ev.VEHICLE + 0x210] = b'\x01'
        if scrape_off:
            mw[ev.VEHICLE + 0x116B] = b'\x01'   # suppress the scrape branch
    return ev.build_vehicle(fields), mw


def susp_read(after, n):
    out = {'accF': [struct.unpack_from('<f', after, 0xF0 + 4 * i)[0]
                    for i in range(4)],
           'accT': [struct.unpack_from('<f', after, 0x100 + 4 * i)[0]
                    for i in range(4)],
           'wheels': []}
    for i in range(n):
        b = W0 + WSTR * i
        out['wheels'].append({
            'pos': [struct.unpack_from('<f', after, b + 4 * j)[0]
                    for j in range(4)],
            'cpt': [struct.unpack_from('<f', after, b + 0x10 + 4 * j)[0]
                    for j in range(4)],
            'nrm': [struct.unpack_from('<f', after, b + 0x20 + 4 * j)[0]
                    for j in range(4)],
            'prevcopy': [struct.unpack_from('<f', after, b + 0x30 + 4 * j)[0]
                         for j in range(8)],
            'spin': struct.unpack_from('<f', after, b + 0x58)[0],
            'omega': struct.unpack_from('<f', after, b + 0x5C)[0],
            'prev': struct.unpack_from('<f', after, b + 0x60)[0],
            'cur': struct.unpack_from('<f', after, b + 0x64)[0],
            'bump': after[b + 0xB2], 'contact': after[b + 0xB3],
            'active': after[b + 0xB4]})
    return out


def model_susp(wheels, cfg, dt, in_race=0):
    """Full mirror of the FUN_00123FD0 paths the cases exercise."""
    accF = [0.0] * 4
    accT = [0.0] * 4
    outw = []
    frame_pos = (0.0, 0.0, 0.0)
    for i, w in enumerate(wheels):
        o = dict(w)
        front = i < 2
        k = cfg['front_force'] if front else cfg['rear_force']
        c = cfg['front_damp'] if front else cfg['rear_damp']
        ln = cfg['front_len'] if front else cfg['rear_len']
        local = w.get('local', (0.8, 0.0, 1.2, 1.0))
        o['bump'] = 0
        if w.get('surface', 0) != 3:
            if w.get('contact', 0):
                F, cur, prev, bump = model_spring_damper(
                    k, c, ln, w['attach'], w['cur'], w['prev'], dt, in_race)
                o.update(cur=cur, prev=prev, bump=bump,
                         pos=[local[0], cur, local[2], 1.0])
                accF, accT = model_force_apply(
                    w.get('normal', (0., 1., 0., 0.)), F, o['pos'],
                    frame_pos, accF, accT)
            else:
                prev = model_droop(k, ln, w['attach'], dt)
                o.update(prev=prev, pos=[local[0], prev, local[2], 1.0])
            spin, omega = model_spin(w.get('spin', 0.), w.get('omega', 0.),
                                     1, w.get('contact', 0), dt)
            o.update(spin=spin, omega=omega)
        outw.append(o)
    return {'accF': accF, 'accT': accT, 'wheels': outw}


SUSP_CASES = [
    ("pure spring 4 wheels", 0, DT,
     [dict(surface=0, contact=1, attach=0.11, cur=0.02, prev=0.02,
           omega=50.0, spin=1.0,
           local=(0.8 if i % 2 == 0 else -0.8, 0.0,
                  1.2 if i < 2 else -1.2, 1.0)) for i in range(4)]),
    ("damper only", 0, DT,
     [dict(surface=0, contact=1, attach=0.11, cur=-0.07, prev=-0.076,
           omega=0.0, spin=0.0) for i in range(4)]),
    ("droop + skip + contact", 0, DT,
     [dict(surface=0, contact=0, attach=0.11, cur=0.05, prev=0.01,
           omega=30.0, spin=0.5),
      dict(surface=3, contact=1, attach=0.11, cur=0.05, prev=0.01,
           omega=30.0, spin=0.5),
      dict(surface=0, contact=0, attach=0.11, cur=0.05, prev=0.01,
           omega=30.0, spin=0.5),
      dict(surface=0, contact=1, attach=0.11, cur=0.05, prev=0.048,
           omega=30.0, spin=0.5)]),
    ("bump flag + tilted normal", 0, DT,
     [dict(surface=0, contact=1, attach=0.11, cur=0.0, prev=-0.15,
           normal=(0.1, 0.95, 0.2, 0.0), omega=10.0, spin=0.0),
      dict(surface=0, contact=0, attach=0.11, cur=0.0, prev=0.0),
      dict(surface=0, contact=0, attach=0.11, cur=0.0, prev=0.0),
      dict(surface=0, contact=0, attach=0.11, cur=0.0, prev=0.0)]),
    ("compression clamp (<1mm)", 0, DT,
     # comp = 0.0445 is inside (0.25*len - 0.001, 0.25*len): the clamp fires
     # but the bottom-out impulse block (unported) stays out of reach
     [dict(surface=0, contact=1, attach=0.11, cur=0.0655, prev=0.0655),
      dict(surface=0, contact=0, attach=0.11, cur=0.0, prev=0.0),
      dict(surface=0, contact=0, attach=0.11, cur=0.0, prev=0.0),
      dict(surface=0, contact=0, attach=0.11, cur=0.0, prev=0.0)]),
    ("in-race extension clamp", 1, DT,
     # comp = 0.21 > 0.75*len: clamps to 0.135, then the soft-clip fades the
     # force by (1-ratio)*2 with ratio = 1.0 -> F = 0 exactly
     [dict(surface=0, contact=1, attach=0.11, cur=-0.10, prev=-0.10),
      dict(surface=0, contact=1, attach=0.11, cur=-0.02, prev=-0.02),
      dict(surface=0, contact=0, attach=0.11, cur=0.0, prev=0.0),
      dict(surface=0, contact=0, attach=0.11, cur=0.0, prev=0.0)]),
    ("spin wrap + dt/30", 0, 1.0 / 30.0,
     [dict(surface=0, contact=1, attach=0.11, cur=0.02, prev=0.02,
           omega=300.0, spin=628.0),
      dict(surface=0, contact=0, attach=0.11, cur=0.05, prev=0.01,
           omega=-500.0, spin=-628.0),
      dict(surface=0, contact=1, attach=0.11, cur=0.03, prev=0.035,
           omega=90.0, spin=100.0),
      dict(surface=0, contact=0, attach=0.11, cur=0.0, prev=0.0)]),
]


def run_susp_cases():
    fails = 0
    print("\nsuspension spring/damper (FUN_00123FD0):")
    for name, in_race, dt, wheels in SUSP_CASES:
        img, mw = susp_build(wheels, SUSP_CFG, in_race=in_race)
        tr, after, err = ev.run(SUSP, img,
                                stack_args=[ev.VEHICLE, f2u(dt)],
                                mem_writes=mw, max_steps=2_000_000)
        if err:
            print("  %-28s FAULT %s" % (name, err))
            fails += 1
            continue
        emu = susp_read(after, len(wheels))
        m = model_susp(wheels, SUSP_CFG, f32(dt), in_race=in_race)
        bad = []

        def cmpf(tag, mv, evv, tol_abs=1e-3):
            if abs(mv - evv) > max(tol_abs, abs(mv) * 1e-4):
                bad.append((tag, mv, evv))
        for j in range(4):
            cmpf("accF[%d]" % j, m['accF'][j], emu['accF'][j])
            cmpf("accT[%d]" % j, m['accT'][j], emu['accT'][j])
        for i, (mw_, ew) in enumerate(zip(m['wheels'], emu['wheels'])):
            if mw_.get('surface', 0) == 3:
                continue   # untouched wheel: nothing to assert
            cmpf("w%d.prev" % i, mw_['prev'], ew['prev'], 1e-5)
            cmpf("w%d.cur" % i, mw_['cur'], ew['cur'], 1e-5)
            cmpf("w%d.spin" % i, mw_['spin'], ew['spin'], 1e-3)
            cmpf("w%d.omega" % i, mw_['omega'], ew['omega'], 1e-3)
            for j in range(3):
                cmpf("w%d.pos[%d]" % (i, j), mw_['pos'][j], ew['pos'][j], 1e-4)
            if mw_['bump'] != ew['bump']:
                bad.append(("w%d.bump" % i, mw_['bump'], ew['bump']))
        if bad:
            fails += 1
            print("  %-28s FAIL" % name)
            for tag, mv, evv in bad[:6]:
                print("      %-10s model %r  emu %r" % (tag, mv, evv))
        else:
            print("  %-28s OK   F.y %10.2f  T.x %10.2f"
                  % (name, emu['accF'][1], emu['accT'][0]))
    return fails


def run_prepass_case():
    """FUN_001239C0 with an empty ground soup: every wheel takes the airborne
    path. Verifies the prev-frame snapshot, the synthesised contact point
    (pos - up*h) and normal (= frame up axis), and the flag clears."""
    fails = 0
    print("\nsuspension pre-pass, airborne path (FUN_001239C0):")
    wheels = [dict(surface=0, contact=1, attach=0.11, cur=0.02, prev=0.02,
                   h=0.31 + 0.01 * i,
                   pos8=[1.0 + i, 2.0 + i, 3.0 + i, 1.0,
                         4.0 + i, 5.0 + i, 6.0 + i, 1.0])
              for i in range(4)]
    img, mw = susp_build(wheels, SUSP_CFG)
    tr, after, err = ev.run(PREPASS, img, stack_args=[ev.VEHICLE],
                            mem_writes=mw, max_steps=2_000_000)
    if err:
        print("  airborne x4                  FAULT %s" % err)
        return 1
    emu = susp_read(after, 4)
    up = (0.0, 1.0, 0.0, 0.0)   # identity frame: +0x10..0x1C
    bad = []
    for i, w in enumerate(wheels):
        ew = emu['wheels'][i]
        cpt = [0.0] * 4
        nrm = [0.0] * 4
        for j in range(4):
            cpt[j] = f32(w['pos8'][j] - f32(up[j] * w['h']))
            nrm[j] = up[j]
        for j in range(4):
            if abs(ew['cpt'][j] - cpt[j]) > 1e-4:
                bad.append(("w%d.cpt[%d]" % (i, j), cpt[j], ew['cpt'][j]))
            if abs(ew['nrm'][j] - nrm[j]) > 1e-6:
                bad.append(("w%d.nrm[%d]" % (i, j), nrm[j], ew['nrm'][j]))
        if ew['prevcopy'] != w['pos8']:
            bad.append(("w%d.prevcopy" % i, w['pos8'], ew['prevcopy']))
        if ew['contact'] != 0:
            bad.append(("w%d.contact" % i, 0, ew['contact']))
        if ew['active'] != 0:
            bad.append(("w%d.active" % i, 0, ew['active']))
    if after[0x1352] != 1:
        bad.append(("v+0x1352", 1, after[0x1352]))
    if bad:
        fails += 1
        print("  airborne x4                  FAIL")
        for tag, mv, evv in bad[:8]:
            print("      %-14s model %r  emu %r" % (tag, mv, evv))
    else:
        print("  airborne x4                  OK   flags cleared, contact "
              "pt/normal synthesised from frame up")
    return fails


# ===========================================================================
# Tyre grip / airborne dampers / body gravity pin -- FUN_0011D460 in full.
# The models below mirror the whole per-frame force pass:
#   * per-wheel tyre forces (the grip model): slip against a steered basis
#     (frame rotated about local up by v+0x1164 degrees, FUN_00011900 /
#     FUN_000116e0), the sine-curve friction term normalised by the BSS
#     constant 23.8164 at 0x005a8054 (static-init stub 0x002ba3c0 <- 0x3b2338),
#     per-axle stiffness (front 150000/60/curve 2.3+2.5*(1-thr); rear
#     t*500000/12/curve t*(6+7*(1-thr)), t = clamp(speed*0.025, .5, 1)),
#     wheel reaction torque -F_long*radius and omega integration *0.04*dt,
#     front wheels free-rolling omega = v_long/radius
#   * force application at body-roll heights (steer 0x1370 / drift 0x1374 /
#     accel 0x136C / brake 0x1368) with r x F about the frame origin
#   * drift-state (v+0x1524) force blend, exits, yaw torque rewrite with the
#     turn-momentum cap, and the anti-slowdown lateral push
#   * airborne path: 4 compiled-in damper records (up-axis gain 1000, right-
#     axis gain 10 at z = +/-(v+0x1D8*0.5) / (v+0x1E8*2.0)) -- THIS is the
#     "-20*dir.x" term that was the suite's KNOWN GAP -- plus corkscrew
#     damping of +0xE0 (FUN_0011f800) and the verified vertical force
#   * the tail: 20*mass of world-vertical gravity is removed and re-applied
#     along the body's -up axis when speed > 1 and frame at.y > 0
#     (DAT_0040a8a0 = (0,-20,0,0)) -- road-holding on banking
# Mirrors b3_tyre_grip / b3_tyre_wheel_reaction / b3_airborne_damper /
# b3_gravity_body_pin / b3_brake_drag_scalar / b3_drift_lateral_blend /
# b3_drift_yaw_torque in src/burnout3_vehicle_sim.c.
# ===========================================================================
import math

D460_LOAD_SCALE_GLOBAL = 0x005a8054
D460_LOAD_SCALE = 23.8164005279541


def dot3(a, b):
    return f32(f32(f32(a[0]*b[0]) + f32(a[1]*b[1])) + f32(a[2]*b[2]))


def xform_point(m, v):
    # FUN_00013ca0: out_i = v.x*m0[i] + m3[i] + v.y*m1[i] + v.z*m2[i]
    return [f32(f32(f32(f32(v[0]*m[0][i]) + m[3][i]) + f32(v[1]*m[1][i]))
                + f32(v[2]*m[2][i])) for i in range(4)]


def rot_vec(m, v):
    return [f32(f32(f32(v[0]*m[0][i]) + f32(v[1]*m[1][i])) + f32(v[2]*m[2][i]))
            for i in range(4)]


def point_vel(env, pt):
    # FUN_001066a0: omega(+0xD0) x (pt - frame_pos) + vel(+0xB0); w = speed
    m = env['frame']; w = env['omega']; vel = env['vel']
    r = [f32(pt[i] - m[3][i]) for i in range(4)]
    return [f32(f32(f32(w[1]*r[2]) - f32(w[2]*r[1])) + vel[0]),
            f32(f32(f32(w[2]*r[0]) - f32(w[0]*r[2])) + vel[1]),
            f32(f32(f32(w[0]*r[1]) - f32(w[1]*r[0])) + vel[2]),
            f32(env['speed'])]


def cross_r_f(env, pt, F):
    # FUN_00106590: (pt - frame_pos) x F, w = 0
    m = env['frame']
    r = [f32(pt[i] - m[3][i]) for i in range(3)]
    return [f32(f32(r[1]*F[2]) - f32(r[2]*F[1])),
            f32(f32(r[2]*F[0]) - f32(r[0]*F[2])),
            f32(f32(r[0]*F[1]) - f32(r[1]*F[0])), 0.0]


def acc_add(acc, F):
    for i in range(4):
        acc[i] = f32(acc[i] + F[i])


def rot_y_matrix(deg):
    # FUN_00011900 with axis (0,1,0): rotation about local up by deg degrees
    a = f32(deg * 0.017453292)
    s = f32(math.sin(a)); c = f32(math.cos(a)); t = f32(1.0 - c)
    return [[f32(1.0 - t), 0.0, f32(-s), 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [f32(s), 0.0, f32(1.0 - t), 0.0],
            [0.0, 0.0, 0.0, 0.0]]


def mat_mul_116e0(A, B):
    out = []
    for i in range(4):
        row = [f32(f32(f32(A[i][0]*B[0][j]) + f32(A[i][1]*B[1][j]))
                   + f32(A[i][2]*B[2][j])) for j in range(4)]
        if i == 3:
            row = [f32(row[j] + B[3][j]) for j in range(4)]
        out.append(row)
    return out


def d460_model(env):
    """Mirror of FUN_0011D460 (except the flag_b takedown block and the
    low-speed drive model FUN_0011c7c0 -- both gated off / asserted separately
    in the cases)."""
    m = env['frame']
    accF = [0.0]*4
    accT = [0.0]*4
    wheels = [dict(w) for w in env['wheels']]
    dt = env['dt']
    out = {'state_1524': env['state_1524'], 'airtime': env['airtime'],
           'f1168': env['f1168_prev'], 'no_upshift': 0}

    mph = f32(env['speed'] * MS_TO_MPH_GAME)
    airborne = not any(w['contact'] for w in wheels)
    fdot_right0 = dot3(env['dir_c0'], m[0])

    prev = env['f1168_prev']
    if airborne:
        if prev == 0:
            out['airtime'] = 0.0
        out['airtime'] = f32(out['airtime'] + dt)
        out['f1168'] = 1
        out['no_upshift'] = 1
    else:
        if prev == 1:
            out['no_upshift'] = 0
            if 0.1 < env['airtime']:
                thr = env['cos90mindrift']
                if fdot_right0 > thr:
                    out['state_1524'] = 2
                elif fdot_right0 < f32(0.0 - thr):
                    out['state_1524'] = 1
        out['f1168'] = 0

    state = out['state_1524']

    # longitudinal resistance (the block already ported as
    # b3_resistance_force, with its gates)
    extra = 0.0
    if state == 0 and env['byte_153d'] == 0:
        extra = f32(f32(f32(env['kq'] * env['speed']) * env['steer'])
                    * env['steer'])
    apply_res = False
    if not env['boost_1444'] and not env['flagb_1446']:
        apply_res = True
    elif f32(env['maxboost_mph'] * 0.44704) <= env['speed']:
        apply_res = True
    elif (env['gear'] != env['gear_count']
          and env['changeup_rpm'] <= f32(env['eng_omega'] * 9.549296)):
        apply_res = True
    if apply_res:
        s = f32(f32(0.0 - f32(env['rc'] * env['speed'])) - extra)
        acc_add(accF, [f32(env['vel'][0]*s), f32(env['vel'][1]*s),
                       f32(env['vel'][2]*s), f32(env['speed']*s)])

    if out['f1168']:
        # ---- airborne: damper table + corkscrew + vertical + drive ----
        L = f32(env['half_1d8'] * 0.5)
        W = f32(env['half_1e8'] * 2.0)
        recs = [((0.0, 0.0, L), (0.0, 1.0, 0.0), 1000.0),
                ((0.0, 0.0, L), (1.0, 0.0, 0.0), 10.0),
                ((0.0, 0.0, W), (0.0, 1.0, 0.0), 1000.0),
                ((0.0, 0.0, W), (1.0, 0.0, 0.0), 10.0)]
        flip = dot3(env['vel'], m[2]) < 0.0
        lastF = [0.0]*4
        for pos, axis, gain in recs:
            p = list(pos)
            if flip:
                p[2] = f32(p[2] * -1.0)
            wp = xform_point(m, p)
            aw = rot_vec(m, axis)
            vp = point_vel(env, wp)
            s = f32(0.0 - f32(gain * dot3(aw, vp)))
            Fd = [f32(aw[i]*s) for i in range(4)]
            acc_add(accF, Fd)
            acc_add(accT, cross_r_f(env, wp, Fd))
            lastF = Fd
        E0 = list(env['angmom_e0'])
        d = dot3(m[2], E0)
        for i in range(4):
            E0[i] = f32(E0[i] - f32(f32(m[2][i]*d) * env['corkscrew_13d8']))
        out['angmom_e0'] = E0
        vf = f32(0.0 - f32(f32(f32(f32(env['downforce'] * mph) * 0.1) + 10.0)
                           * env['mass']))
        acc_add(accF, [0.0, vf, 0.0, lastF[3]])
        wheels[2]['torque'] = f32(wheels[2]['torque']
                                  + f32(env['drive_torque'] * 0.5))
        wheels[3]['torque'] = f32(wheels[3]['torque']
                                  + f32(env['drive_torque'] * 0.5))
        for w in wheels:
            w['omega'] = f32(w['omega'] + f32(f32(w['torque'] * 0.04) * dt))
            w['torque'] = 0.0
        out.update(accF=accF, accT=accT, wheels=wheels)
        return out

    # ---- grounded ----
    lsdm = (env['flagb_1446'] == 0
            and (env['lsdm_limit'] > mph or env['gear'] == -1))
    out['lsdm'] = lsdm
    if not lsdm:
        wheels[2]['torque'] = f32(wheels[2]['torque']
                                  + f32(env['drive_torque'] * 0.5))
        wheels[3]['torque'] = f32(wheels[3]['torque']
                                  + f32(env['drive_torque'] * 0.5))
        # brake / engine-brake force along the travel direction
        pt = [f32(f32(m[1][i] * env['brake_h']) + m[3][i]) for i in range(4)]
        if state in (1, 2) or env['throttle'] > 0.1:
            gate = 0.0
        else:
            gate = 1.0
        sc = f32(f32(env['engbrake'] * gate)
                 - f32(f32(env['brakef'] * env['brake']) * 20000.0))
        sc = f32(f32(sc * f32(env['speed'] + 1.0)) * 0.014285714365541935)
        if state in (1, 2):
            if 0.3 > env['t_142c']:
                sc = 0.0
            pt = list(m[3])
        Fb = [f32(env['dir_c0'][i] * sc) for i in range(4)]
        acc_add(accF, Fb)
        acc_add(accT, cross_r_f(env, pt, Fb))
        # steered wheel basis: rotate the frame about local up by v+0x1164
        steered = mat_mul_116e0(rot_y_matrix(env['steer_deg']), m)
        T_saved = list(accT)
        latsum = [0.0]*4
        for i in range(4):
            w = wheels[i]
            load = f32(env['mass'] * 5.0)
            local = list(env['wheel_local'][i])
            local[1] = w['attach']
            wp = xform_point(m, local)
            vp = point_vel(env, wp)
            fwd = steered[2] if i < 2 else m[2]
            lat = steered[0] if i < 2 else m[0]
            roll = f32(w['omega'] * w['radius'])
            vlat = dot3(lat, vp)
            vlong = dot3(fwd, vp)
            if i < 2:
                w['omega'] = f32(vlong / w['radius'])
            slip_l = f32(vlong - roll)
            slip = f32(math.sqrt(f32(f32(vlat*vlat) + f32(slip_l*slip_l))))
            slip_c = slip
            if roll == 0.0:
                roll = 9.999999974752427e-07
            if slip == 0.0:
                slip_c = 1.0000000116860974e-07
            if i < 2:
                denom_k = 60.0
                curve = f32(f32(f32(1.0 - env['throttle']) * 2.5)
                            + 2.299999952316284)
                stiff = 150000.0
            else:
                t = f32(env['speed'] * 0.02500000037252903)
                denom_k = 12.0
                curve = f32(f32(f32(1.0 - env['throttle']) * 7.0) + 6.0)
                t = 0.5 if 0.5 > t else (1.0 if t > 1.0 else t)
                stiff = f32(t * 500000.0)
                curve = f32(t * curve)
            sr = abs(f32(f32(stiff * slip_c) / f32(f32(denom_k * roll) * load)))
            if sr > 1.0:
                sr = 1.0
            x = f32(0.4000000059604645 - f32(sr * 0.800000011920929))
            sinx = f32(math.sin(x))
            term1 = f32(f32(f32(x - 0.4000000059604645) * 0.9210609793663025)
                        + f32(0.3894183337688446 - sinx))
            term1 = f32(f32(f32(term1 * curve) * load) * D460_LOAD_SCALE)
            term2 = f32(f32(f32(f32(f32(x - -0.4000000059604645) / roll)
                                * stiff) * slip_c) * 0.5)
            scal = f32(f32(-1.0 / slip_c) * f32(term1 + term2))
            Flat = f32(vlat * scal)
            Flong = f32(slip_l * scal)
            # (flag_b takedown block: unported, gated off in every case)
            w['torque'] = f32(w['torque'] + f32(0.0 - f32(Flong * w['radius'])))
            w['omega'] = f32(w['omega'] + f32(f32(w['torque'] * 0.04) * dt))
            w['torque'] = 0.0
            if state in (1, 2):
                mag = f32(f32(1.0 - f32(env['slide_1440']
                                        * 0.6666666865348816))
                          * f32(f32(dot3(env['dir_c0'], m[2]) * 6400.0)
                                + 4200.0))
                if not (Flat > 0.0):
                    mag = f32(0.0 - mag)
                if 1.0 > env['t_142c']:
                    mag = f32(mag * env['t_142c'])
                one_m_slide = f32(1.0 - env['slide_1440'])
                mag = f32(mag * env['slide_1440'])
                Fl = [f32(f32(f32(lat[j]*Flat) * one_m_slide)
                          + f32(lat[j]*mag)) for j in range(4)]
                height = env['drift_h']
                extra_gate = env['side02_1b'] and env['side13_1a']
            else:
                Fl = [f32(lat[j] * Flat) for j in range(4)]
                height = env['steer_h']
                extra_gate = True
            if i < 2:
                contact_gate = wheels[0]['contact'] or wheels[1]['contact']
            else:
                contact_gate = w['contact']
            off = [f32(m[1][j] * height) for j in range(4)] \
                if (contact_gate and extra_gate) else [0.0]*4
            p2 = [f32(wp[j] + off[j]) for j in range(4)]
            acc_add(accF, Fl)
            acc_add(accT, cross_r_f(env, p2, Fl))
            acc_add(latsum, Fl)
            F2 = [f32(fwd[j] * Flong) for j in range(4)]
            p3 = [f32(m[3][j] + f32(m[1][j] * env['accel_h']))
                  for j in range(4)]
            acc_add(accF, F2)
            if env['flagb_1446'] == 0:
                acc_add(accT, cross_r_f(env, p3, F2))
        # drift-state exits
        fdot = dot3(env['dir_c0'], m[0])
        state = out['state_1524']
        if state in (1, 2) and 0.5 < env['t_142c']:
            if state == 1:
                if (fdot >= -0.009999999776482582
                        and not (env['steer'] < -0.004999999888241291)):
                    out['state_1524'] = 0
            elif (fdot <= 0.009999999776482582
                    and not (0.004999999888241291 < env['steer'])):
                out['state_1524'] = 0
        state = out['state_1524']
        if (state in (1, 2) and env['throttle'] < 0.5 and env['brake'] < 0.5
                and env['steer'] == 0.0 and mph < 40.0):
            out['state_1524'] = 0
        state = out['state_1524']
        # yaw torque rewrite + turn-momentum cap
        up = m[1]
        dT = [f32(accT[i] - T_saved[i]) for i in range(4)]
        d = dot3(dT, up)
        dT_perp = [f32(dT[i] - f32(up[i]*d)) for i in range(4)]
        yaw_mom = dot3(env['angmom_e0'], up)
        def steer_torque(k):
            return f32(f32(f32(f32(f32(1.0 - f32(env['slide_1440']*0.75))
                                    * env['turn_rate'])
                                * env['steer']) * abs(env['steer'])) * k)
        if state == 2:
            if -0.05 > env['steer']:
                d_new = steer_torque(-10000.0)
            elif env['cosmaxdrift'] > fdot:
                d_new = steer_torque(-5000.0) if env['steer'] > 0.05 \
                    else f32(env['v_1414'] * 1000.0)
            else:
                d_new = 1000.0
        elif state == 1:
            if env['steer'] > 0.05:
                d_new = steer_torque(-10000.0)
            elif fdot > f32(0.0 - env['cosmaxdrift']):
                d_new = steer_torque(-5000.0) if -0.05 > env['steer'] \
                    else f32(env['v_1414'] * -1000.0)
            else:
                d_new = -1000.0
        else:
            d_new = d
        if 60.0 > mph:
            cap = 800.0
        elif 110.0 > mph:
            t = f32(f32(mph - 60.0) * 0.019999999552965164)
            cap = f32(f32(env['turn_slow'] * t)
                      + f32(f32(1.0 - t) * env['turn_fast']))
        else:
            cap = env['turn_fast']
        if 0.25 > env['t_142c']:
            cap = f32(cap * f32(env['t_142c'] + 0.75))
        if d_new > 0.0 and yaw_mom > cap:
            d_new = f32(d_new * 0.10000000149011612)
        if 0.0 > d_new and f32(0.0 - cap) > yaw_mom:
            d_new = f32(d_new * 0.10000000149011612)
        for i in range(4):
            accT[i] = f32(T_saved[i] + f32(dT_perp[i] + f32(up[i]*d_new)))
        # anti-slowdown lateral push
        dirv = env['dir_c0']
        ldot = dot3(latsum, dirv)
        if (0.0 > ldot and out['state_1524'] in (1, 2)
                and env['throttle'] > 0.1):
            s2 = f32(0.0 - ldot)
            acc_add(accF, [f32(dirv[i]*s2) for i in range(4)])
            crossud = [f32(f32(up[(i+1) % 3]*dirv[(i+2) % 3])
                           - f32(up[(i+2) % 3]*dirv[(i+1) % 3]))
                       for i in range(3)] + [0.0]
            push = [f32(crossud[i]*ldot) for i in range(4)]
            if 0.0 > dot3(dirv, m[0]):
                push = [f32(p * -1.0) for p in push]
            acc_add(accF, push)
    # body gravity pin (grounded and LSDM paths; airborne returned above)
    if env['speed'] > 1.0 and m[2][1] > 0.0:
        accF[1] = f32(accF[1] - f32(-20.0 * env['mass']))
        s = f32(env['mass'] * -20.0)
        for i in range(4):
            accF[i] = f32(accF[i] + f32(m[1][i] * s))
    out.update(accF=accF, accT=accT, wheels=wheels)
    return out


D460_LOCALS = [(0.8, 0.0, 1.2, 1.0), (-0.8, 0.0, 1.2, 1.0),
               (0.8, 0.0, -1.2, 1.0), (-0.8, 0.0, -1.2, 1.0)]
IDENT4 = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0],
          [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 0.0]]


def d460_env(frame, vel, speed, omega, angmom, dir_c0, mass, wheels, dt, **kw):
    q = lambda v: [f32(x) for x in (list(v) + [0.0]*4)[:4]]
    env = dict(frame=[q(r) for r in frame], vel=q(vel), speed=f32(speed),
               omega=q(omega), angmom_e0=q(angmom), dir_c0=q(dir_c0),
               mass=f32(mass),
               wheels=[{k: (f32(v) if isinstance(v, float) else v)
                        for k, v in w.items()} for w in wheels],
               wheel_local=[q(l) for l in D460_LOCALS], dt=f32(dt))
    defaults = dict(rc=0.5, kq=0.0, steer=0.0, steer_deg=0.0, throttle=0.0,
                    brake=0.0, drive_torque=0.0, boost_1444=0, flagb_1446=0,
                    byte_153d=0, gear=3, gear_count=6, changeup_rpm=6800.0,
                    eng_omega=500.0, maxboost_mph=200.0, lsdm_limit=0.0,
                    lsdm_angle=0.0, brake_h=0.0, accel_h=0.0, steer_h=0.0,
                    drift_h=0.0, brakef=0.0, engbrake=0.0, downforce=0.0,
                    turn_rate=0.0, turn_slow=800.0, turn_fast=800.0,
                    cosmaxdrift=0.5, cos90mindrift=0.5, corkscrew_13d8=0.0,
                    half_1d8=0.0, half_1e8=0.0, slide_1440=0.0, t_142c=0.0,
                    v_1414=0.0, state_1524=0, f1168_prev=0, airtime=0.0,
                    slide_max=0.9)
    defaults.update(kw)
    for k, v in defaults.items():
        env[k] = f32(v) if isinstance(v, float) else v
    env['side02_1b'] = 1 if (wheels[0]['contact'] or wheels[2]['contact']) \
        else 0
    env['side13_1a'] = 1 if (wheels[1]['contact'] or wheels[3]['contact']) \
        else 0
    return env


def d460_run(env):
    S = ev.SCRATCH
    S_WF = 0x3000
    fields = {
        0x1360: env['rc'], 0x13C0: env['kq'], 0x1408: env['steer'],
        0x1164: env['steer_deg'], 0xBC: env['speed'], 0x1F0: env['mass'],
        0x13D4: env['maxboost_mph'], 0x1470: env['changeup_rpm'],
        0x149C: env['eng_omega'], 0x14C8: env['gear'],
        0x14CC: env['gear_count'], 0x1400: env['throttle'],
        0x1404: env['brake'], 0x1520: env['drive_torque'],
        0x13AC: env['lsdm_limit'], 0x13B0: env['lsdm_angle'],
        0x1368: env['brake_h'], 0x136C: env['accel_h'],
        0x1370: env['steer_h'], 0x1374: env['drift_h'],
        0x138C: env['brakef'], 0x13CC: env['engbrake'],
        0x1364: env['downforce'], 0x13A4: env['turn_rate'],
        0x1398: env['turn_slow'], 0x139C: env['turn_fast'],
        0x13D0: env['cosmaxdrift'], 0x13DC: env['cos90mindrift'],
        0x13D8: env['corkscrew_13d8'], 0x1D8: env['half_1d8'],
        0x1E8: env['half_1e8'], 0x1440: env['slide_1440'],
        0x142C: env['t_142c'], 0x1414: env['v_1414'],
        0x143C: env['airtime'], 0x1390: env['slide_max'],
    }
    for i in range(4):
        fields[0xCC8 + 4*i] = S + S_WF + 0x40*i
    img = bytearray(ev.build_vehicle(fields))
    struct.pack_into('<I', img, 0x1524, env['state_1524'])
    img[0x1168] = env['f1168_prev']
    img[0x1444] = env['boost_1444']
    img[0x1446] = env['flagb_1446']
    img[0x153D] = env['byte_153d']
    for j in range(4):
        struct.pack_into('<f', img, 0xB0 + 4*j,
                         env['vel'][j] if j < 3 else env['speed'])
        struct.pack_into('<f', img, 0xD0 + 4*j, env['omega'][j])
        struct.pack_into('<f', img, 0xE0 + 4*j, env['angmom_e0'][j])
        struct.pack_into('<f', img, 0xC0 + 4*j, env['dir_c0'][j])
    for i, w in enumerate(env['wheels']):
        b = WHEEL0 + WSTRIDE*i
        struct.pack_into('<f', img, b + 0x50, w['radius'])
        struct.pack_into('<f', img, b + 0x54, w['torque'])
        struct.pack_into('<f', img, b + 0x5C, w['omega'])
        struct.pack_into('<f', img, b + 0x74, w['attach'])
        img[b + 0xB3] = w['contact']
    scratch = bytearray(0x8000)
    for r in range(4):
        for c in range(4):
            struct.pack_into('<f', scratch, 16*r + 4*c, env['frame'][r][c])
    for i in range(4):
        wf = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0],
              list(env['wheel_local'][i])]
        for r in range(4):
            for c in range(4):
                struct.pack_into('<f', scratch, S_WF + 0x40*i + 16*r + 4*c,
                                 wf[r][c])
    mw = {D460_LOAD_SCALE_GLOBAL: struct.pack('<f', D460_LOAD_SCALE)}
    tr, after, err = ev.run(D460, bytes(img),
                            stack_args=[ev.VEHICLE, f2u(env['dt'])],
                            scratch=bytes(scratch), mem_writes=mw,
                            max_steps=3_000_000)
    if err:
        raise RuntimeError("emulation faulted: " + err)
    res = {'accF': [struct.unpack_from('<f', after, 0xF0 + 4*i)[0]
                    for i in range(4)],
           'accT': [struct.unpack_from('<f', after, 0x100 + 4*i)[0]
                    for i in range(4)],
           'angmom_e0': [struct.unpack_from('<f', after, 0xE0 + 4*i)[0]
                         for i in range(4)],
           'state_1524': struct.unpack_from('<I', after, 0x1524)[0],
           'f1168': after[0x1168],
           'airtime': struct.unpack_from('<f', after, 0x143C)[0],
           'steer_1164': struct.unpack_from('<f', after, 0x1164)[0],
           'wheels': [{'torque': struct.unpack_from(
                           '<f', after, WHEEL0 + WSTRIDE*i + 0x54)[0],
                       'omega': struct.unpack_from(
                           '<f', after, WHEEL0 + WSTRIDE*i + 0x5C)[0]}
                      for i in range(4)]}
    return res


def d460_wheels(contact=(1, 1, 1, 1), radius=0.346, omega=100.0):
    return [dict(radius=radius, torque=0.0, omega=omega, attach=0.11,
                 contact=contact[i]) for i in range(4)]


TYRE_CASES = [
    ("airborne dampers+corkscrew", dict(
        frame=IDENT4, vel=[24.0, 0.0, 32.0], speed=40.0,
        omega=[0.1, 0.2, -0.1], angmom=[50.0, 20.0, -10.0],
        dir_c0=[0.6, 0.0, 0.8], mass=1000.0,
        wheels=d460_wheels((0, 0, 0, 0)), dt=1/60.0),
     dict(half_1d8=2.4, half_1e8=0.9, downforce=1.5, corkscrew_13d8=0.3,
          drive_torque=3000.0)),
    ("airborne reversing z-flip", dict(
        frame=IDENT4, vel=[0.0, -3.0, -15.0], speed=15.3,
        omega=[0.4, 0.0, 0.2], angmom=[0.0, 0.0, 0.0],
        dir_c0=[0.0, -0.2, -0.98], mass=1000.0,
        wheels=d460_wheels((0, 0, 0, 0)), dt=1/60.0),
     dict(half_1d8=2.4, half_1e8=0.9, corkscrew_13d8=0.2,
          f1168_prev=1, airtime=0.3)),
    ("grounded straight driven", dict(
        frame=IDENT4, vel=[0.0, 0.0, 40.0], speed=40.0,
        omega=[0.0, 0.0, 0.0], angmom=[0.0, 0.0, 0.0],
        dir_c0=[0.0, 0.0, 1.0], mass=1000.0,
        wheels=d460_wheels(omega=40.0/0.346), dt=1/60.0),
     dict(brake_h=-0.55, accel_h=-0.25, steer_h=-0.15, engbrake=-1600.0,
          brakef=0.9, throttle=1.0, drive_torque=5000.0)),
    ("grounded steering slip", dict(
        frame=IDENT4, vel=[2.0, 0.0, 39.0], speed=39.05,
        omega=[0.0, 0.3, 0.0], angmom=[0.0, 500.0, 0.0],
        dir_c0=[0.05, 0.0, 0.9987], mass=1000.0,
        wheels=d460_wheels(omega=110.0), dt=1/60.0),
     dict(steer=0.5, steer_deg=-8.0, throttle=0.8, brake_h=-0.55,
          accel_h=-0.25, steer_h=-0.15, engbrake=-1600.0, brakef=0.9,
          drive_torque=6000.0, turn_rate=1.4)),
    ("drift state 2 blend+yaw", dict(
        frame=IDENT4, vel=[8.0, 0.0, 38.0], speed=38.83,
        omega=[0.0, 0.8, 0.0], angmom=[0.0, 900.0, 0.0],
        dir_c0=[0.2, 0.0, 0.9798], mass=1000.0,
        wheels=d460_wheels(omega=105.0), dt=1/60.0),
     dict(steer=0.7, steer_deg=-12.0, throttle=1.0, state_1524=2,
          slide_1440=0.6, t_142c=0.4, v_1414=0.3, brake_h=-0.55,
          accel_h=-0.25, steer_h=-0.15, drift_h=-0.35, engbrake=-1600.0,
          brakef=0.9, drive_torque=6000.0, turn_rate=1.4,
          turn_slow=1500.0, turn_fast=2500.0)),
    ("drift exit straightened", dict(
        frame=IDENT4, vel=[0.0, 0.0, 30.0], speed=30.0,
        omega=[0.0, 0.0, 0.0], angmom=[0.0, 100.0, 0.0],
        dir_c0=[0.0, 0.0, 1.0], mass=1000.0,
        wheels=d460_wheels(omega=86.7), dt=1/60.0),
     dict(throttle=1.0, state_1524=2, slide_1440=0.6, t_142c=0.8,
          turn_rate=1.4)),
    ("sideways landing -> drift", dict(
        frame=IDENT4, vel=[20.0, 0.0, 20.0], speed=28.28,
        omega=[0.0, 0.0, 0.0], angmom=[0.0, 0.0, 0.0],
        dir_c0=[0.7071, 0.0, 0.7071], mass=1000.0,
        wheels=d460_wheels(omega=80.0), dt=1/60.0),
     dict(throttle=0.5, f1168_prev=1, airtime=0.5, turn_rate=1.4)),
    ("banked frame gravity pin", dict(
        frame=[[0.98877, 0.14944, 0.0, 0.0], [-0.14944, 0.98877, 0.0, 0.0],
               [0.0, 0.02, 1.0, 0.0], [0.0, 0.0, 0.0, 0.0]],
        vel=[0.0, 0.0, 35.0], speed=35.0, omega=[0.0, 0.0, 0.0],
        angmom=[0.0, 0.0, 0.0], dir_c0=[0.0, 0.0, 1.0], mass=1200.0,
        wheels=d460_wheels(omega=101.0), dt=1/60.0),
     dict(throttle=1.0, drive_torque=4000.0)),
    ("wheelspin launch", dict(
        frame=IDENT4, vel=[0.0, 0.0, 10.0], speed=10.0,
        omega=[0.0, 0.0, 0.0], angmom=[0.0, 0.0, 0.0],
        dir_c0=[0.0, 0.0, 1.0], mass=1000.0,
        wheels=d460_wheels(omega=28.9), dt=1/60.0),
     dict(throttle=1.0, drive_torque=20000.0)),
    ("full braking", dict(
        frame=IDENT4, vel=[0.0, 0.0, 45.0], speed=45.0,
        omega=[0.0, 0.0, 0.0], angmom=[0.0, 0.0, 0.0],
        dir_c0=[0.0, 0.0, 1.0], mass=1000.0,
        wheels=d460_wheels(omega=130.0), dt=1/60.0),
     dict(brake=1.0, brakef=0.9, engbrake=-1600.0, brake_h=-0.55,
          accel_h=-0.25)),
]


def run_tyre_cases():
    fails = 0
    print("\ntyre grip / airborne dampers / gravity pin (FUN_0011D460):")
    for name, base, extra in TYRE_CASES:
        env = d460_env(dir_c0=base['dir_c0'], **{k: v for k, v in base.items()
                                                 if k != 'dir_c0'}, **extra)
        try:
            emu = d460_run(env)
        except RuntimeError as e:
            print("  %-28s %s" % (name, e))
            fails += 1
            continue
        mod = d460_model(env)
        bad = []
        def cmpf(tag, mv, evv, tol=1e-3):
            if abs(mv - evv) > max(tol, abs(mv) * 2e-4):
                bad.append((tag, mv, evv))
        for j in range(4):
            cmpf("accF[%d]" % j, mod['accF'][j], emu['accF'][j])
            cmpf("accT[%d]" % j, mod['accT'][j], emu['accT'][j])
        for i in range(4):
            cmpf("w%d.omega" % i, mod['wheels'][i]['omega'],
                 emu['wheels'][i]['omega'])
            cmpf("w%d.torque" % i, mod['wheels'][i]['torque'],
                 emu['wheels'][i]['torque'])
        if mod['state_1524'] != emu['state_1524']:
            bad.append(("state", mod['state_1524'], emu['state_1524']))
        if mod['f1168'] != emu['f1168']:
            bad.append(("f1168", mod['f1168'], emu['f1168']))
        if 'angmom_e0' in mod:
            for j in range(4):
                cmpf("E0[%d]" % j, mod['angmom_e0'][j], emu['angmom_e0'][j])
        if bad:
            fails += 1
            print("  %-28s FAIL" % name)
            for tag, mv, evv in bad[:8]:
                print("      %-10s model %r  emu %r" % (tag, mv, evv))
        else:
            print("  %-28s OK   F=(%.1f, %.1f, %.1f)  Tz %.1f"
                  % (name, emu['accF'][0], emu['accF'][1], emu['accF'][2],
                     emu['accT'][2]))
    # low-speed drive model gate (FUN_0011c7c0): only its steering-angle
    # write, the state reset, and the front free-roll writeback are asserted;
    # the bicycle model itself is NOT ported (see RE_NOTES 11).
    env = d460_env(frame=IDENT4, vel=[0.0, 0.0, 3.0], speed=3.0,
                   omega=[0.0, 0.0, 0.0], angmom=[0.0, 0.0, 0.0],
                   dir_c0=[0.0, 0.0, 1.0], mass=1000.0,
                   wheels=d460_wheels(omega=8.7), dt=1/60.0,
                   lsdm_limit=20.0, lsdm_angle=35.0, throttle=1.0, steer=0.4)
    try:
        emu = d460_run(env)
        want = f32(0.0 - f32(f32(35.0) * f32(0.4)))
        w_omega = f32(f32(3.0) / f32(0.346))
        ok = (abs(emu['steer_1164'] - want) < 1e-3
              and emu['state_1524'] == 0
              and abs(emu['wheels'][0]['omega'] - w_omega) < 1e-3
              and abs(emu['wheels'][1]['omega'] - w_omega) < 1e-3)
    except RuntimeError as e:
        print("  %-28s %s" % ("LSDM steering consumer", e))
        ok = False
    print("  %-28s %s   angle %.2f deg  (lsdm_steering_angle * steer)"
          % ("LSDM steering consumer", "OK" if ok else "FAIL",
             emu['steer_1164'] if ok else 0.0))
    fails += not ok
    return fails


# ===========================================================================
# Rigid-body integration -- FUN_00109560 (called from FUN_00123000 after the
# suspension pass). Verified against the real function:
#   * gravity = DAT_0040a8a0 = (0,-20,0,0) * mass into +0xF0 (with the
#     raised-point torque variant when in-race byte +0x210 is set;
#     application point = frame_pos + up * v+0x1F4)
#   * impulse accumulation +0x110/+0x120 += acc * dt; acc cleared
#   * vel(+0xB0, 4-wide) += impulse/mass; vel.y capped at 120
#   * angular momentum +0xE0 += torque impulse; omega(+0xD0) = L . M with
#     M = world inverse inertia rows at +0x40; |omega| > 100 squashes to
#     100/|omega| and L *= 0.95
#   * frame pos += vel*dt (+ the deflection accumulator +0x130, then cleared)
#   * frame rows -= row x (omega*dt), then FUN_000ff270 re-orthonormalises
#     (retail path traced from this call site: row1 = norm(row2 x row0),
#     row0 = norm(row1 x row2) -- the dot-product selector is compiled out)
#   * world inverse inertia +0x40 rebuilt as Rt . I0 . R from the BODY
#     inverse inertia at +0x10 (FUN_00109040 twice)
#   * +0x70 = inverse of the frame (FUN_00040ae0), and FUN_000ffc80 refreshes
#     speed(+0xBC) = |vel| and the unit travel direction +0xC0
# Mirrors b3_rigid_body_integrate() in src/burnout3_vehicle_sim.c.
# ===========================================================================
INTEG = 0x00109560


def normalize4(v):
    ln = f32(math.sqrt(dot3(v, v)))
    s = f32(1.0 / ln)
    return [f32(x * s) for x in v], ln


def cross4(a, b):
    return [f32(f32(a[1]*b[2]) - f32(a[2]*b[1])),
            f32(f32(a[2]*b[0]) - f32(a[0]*b[2])),
            f32(f32(a[0]*b[1]) - f32(a[1]*b[0])), 0.0]


def orthonormalize_ff270(m):
    r0, _ = normalize4(m[0])
    r1, _ = normalize4(m[1])
    r2, _ = normalize4(m[2])
    r1 = cross4(r2, r0)
    r1, _ = normalize4(r1)
    r0 = cross4(r1, r2)
    r0, _ = normalize4(r0)
    return [r0, r1, r2, list(m[3])]


def mat_invert_40ae0(m):
    o = [row[:] for row in m]
    o[0][1], o[1][0] = m[1][0], m[0][1]
    o[0][2], o[2][0] = m[2][0], m[0][2]
    o[1][2], o[2][1] = m[2][1], m[1][2]
    p = m[3]
    o[3] = [f32(-f32(f32(f32(p[0]*o[0][j]) + f32(p[1]*o[1][j]))
                     + f32(p[2]*o[2][j]))) for j in range(4)]
    return o


def mat_mul_109040(A, B):
    return [[f32(f32(f32(B[i][0]*A[0][j]) + f32(B[i][1]*A[1][j]))
                 + f32(B[i][2]*A[2][j])) for j in range(4)] for i in range(3)]


def integ_model(env):
    m = [list(r) for r in env['frame']]
    accF = list(env['accF']); accT = list(env['accT'])
    impF = list(env['impF']); impT = list(env['impT'])
    vel = list(env['vel']) + [env['speed']]
    L = list(env['angmom_e0'])
    dt = env['dt']
    g = [0.0, f32(-20.0 * env['mass']), 0.0, 0.0]
    if env['in_race'] == 0 and env['state_215'] != 6:
        for i in range(4):
            accF[i] = f32(accF[i] + g[i])
    else:
        pt = [f32(f32(m[1][i] * env['h_1f4']) + m[3][i]) for i in range(4)]
        for i in range(4):
            accF[i] = f32(accF[i] + g[i])
        r = [f32(pt[i] - m[3][i]) for i in range(3)]
        T = [f32(f32(r[1]*g[2]) - f32(r[2]*g[1])),
             f32(f32(r[2]*g[0]) - f32(r[0]*g[2])),
             f32(f32(r[0]*g[1]) - f32(r[1]*g[0])), 0.0]
        for i in range(4):
            accT[i] = f32(accT[i] + T[i])
    dtv = [dt, dt, dt, 0.0]      # the w-lane dt slot is uninitialised stack
    for i in range(4):
        impF[i] = f32(impF[i] + f32(accF[i] * dtv[i]))
        impT[i] = f32(impT[i] + f32(accT[i] * dtv[i]))
    for i in range(4):
        vel[i] = f32(vel[i] + f32(impF[i] / env['mass']))
    accF = [0.0]*4; accT = [0.0]*4; impF = [0.0]*4
    if vel[1] > 120.0:
        vel[1] = 120.0
    for i in range(4):
        L[i] = f32(L[i] + impT[i])
    impT = [0.0]*4
    M = env['inv_inertia_world']
    omega = [f32(f32(f32(L[0]*M[0][j]) + f32(L[1]*M[1][j]))
                 + f32(L[2]*M[2][j])) for j in range(4)]
    w2 = dot3(omega, omega)
    if w2 > 10000.0:
        omega, _ = normalize4(omega)
        scale = f32(f32(1.0 / f32(math.sqrt(w2))) * 100.0)
        omega = [f32(o * scale) for o in omega]
        L = [f32(l * 0.949999988079071) for l in L]
    for i in range(4):
        m[3][i] = f32(m[3][i] + f32(vel[i] * dtv[i]))
    r = [f32(omega[i] * dtv[i]) for i in range(4)]
    for row in (2, 1, 0):
        c = cross4(m[row], r)
        for i in range(4):
            m[row][i] = f32(m[row][i] - c[i])
    m = orthonormalize_ff270(m)
    for i in range(4):
        m[3][i] = f32(m[3][i] + env['defl_130'][i])
    Rt = [[m[0][0], m[1][0], m[2][0], 0.0],
          [m[0][1], m[1][1], m[2][1], 0.0],
          [m[0][2], m[1][2], m[2][2], 0.0]]
    Mw = mat_mul_109040(m, mat_mul_109040(env['inv_inertia_body'], Rt))
    inv = mat_invert_40ae0(m)
    ls = dot3(vel, vel)
    if 2.3283064365386963e-10 > ls:
        speed = 0.0
        dirv = list(m[2])
    else:
        dirv, speed = normalize4(list(vel))
    return dict(frame=m, vel=vel[:3], speed=speed, dir=dirv, omega=omega,
                angmom=L, impF=impF, impT=impT, accF=accF, accT=accT,
                inv_inertia_world=Mw, inv_frame=inv)


def integ_env(**kw):
    ident = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0],
             [0.0, 0.0, 1.0, 0.0], [10.0, 2.0, 50.0, 0.0]]
    diag = [[0.001, 0.0, 0.0, 0.0], [0.0, 0.0012, 0.0, 0.0],
            [0.0, 0.0, 0.0004, 0.0]]
    env = dict(frame=ident, vel=[0.0, 0.0, 30.0], speed=30.0,
               angmom_e0=[0.0]*4, accF=[0.0]*4, accT=[0.0]*4,
               impF=[0.0]*4, impT=[0.0]*4, defl_130=[0.0]*4,
               inv_inertia_body=[r[:] for r in diag],
               inv_inertia_world=[r[:] for r in diag],
               mass=1000.0, h_1f4=0.0, in_race=0, state_215=0, dt=1/60.0)
    env.update(kw)
    q = lambda v: [f32(x) for x in v]
    for k in ('vel', 'angmom_e0', 'accF', 'accT', 'impF', 'impT', 'defl_130'):
        env[k] = q(env[k])
    env['frame'] = [q(r) for r in env['frame']]
    env['inv_inertia_body'] = [q(r) for r in env['inv_inertia_body']]
    env['inv_inertia_world'] = [q(r) for r in env['inv_inertia_world']]
    env['speed'] = f32(env['speed'])
    env['mass'] = f32(env['mass'])
    return env


def integ_run(env):
    from unicorn.x86_const import UC_X86_REG_ECX as _ECX
    fields = {0xBC: env['speed'], 0x1F0: env['mass'], 0x1F4: env['h_1f4']}
    img = bytearray(ev.build_vehicle(fields))
    img[0x210] = env['in_race']
    img[0x215] = env['state_215']
    for j in range(4):
        struct.pack_into('<f', img, 0xB0 + 4*j,
                         env['vel'][j] if j < 3 else env['speed'])
        struct.pack_into('<f', img, 0xD0 + 4*j, 0.0)
        struct.pack_into('<f', img, 0xE0 + 4*j, env['angmom_e0'][j])
        struct.pack_into('<f', img, 0xF0 + 4*j, env['accF'][j])
        struct.pack_into('<f', img, 0x100 + 4*j, env['accT'][j])
        struct.pack_into('<f', img, 0x110 + 4*j, env['impF'][j])
        struct.pack_into('<f', img, 0x120 + 4*j, env['impT'][j])
        struct.pack_into('<f', img, 0x130 + 4*j, env['defl_130'][j])
    for r in range(3):
        for c in range(4):
            struct.pack_into('<f', img, 0x10 + 0x10*r + 4*c,
                             env['inv_inertia_body'][r][c])
            struct.pack_into('<f', img, 0x40 + 0x10*r + 4*c,
                             env['inv_inertia_world'][r][c])
    scratch = bytearray(0x1000)
    for r in range(4):
        for c in range(4):
            struct.pack_into('<f', scratch, 16*r + 4*c, env['frame'][r][c])
    tr, after, err = ev.run(INTEG, bytes(img),
                            stack_args=[f2u(env['dt'])],
                            regs={_ECX: ev.VEHICLE},
                            scratch=bytes(scratch), max_steps=3_000_000,
                            ret_scratch=0x100)
    if err:
        raise RuntimeError("emulation faulted: " + err)
    sc = tr.scratch_after
    frame = [[struct.unpack_from('<f', sc, 16*r + 4*c)[0] for c in range(4)]
             for r in range(4)]
    return after, frame


INTEG_CASES = [
    ("free fall (gravity -20)", {}),
    ("forces+torques integrate", dict(
        accF=[5000.0, 22000.0, -3000.0, 100.0],
        accT=[400.0, 2500.0, -800.0, 0.0],
        angmom_e0=[10.0, 300.0, -20.0, 0.0])),
    ("in-race gravity torque", dict(
        in_race=1, h_1f4=0.31,
        frame=[[0.9950042, 0.0998334, 0.0, 0.0],
               [-0.0998334, 0.9950042, 0.0, 0.0],
               [0.0, 0.0, 1.0, 0.0], [10.0, 2.0, 50.0, 0.0]],
        accT=[100.0, -50.0, 30.0, 0.0])),
    ("omega squash at 100", dict(
        angmom_e0=[0.0, 200000.0, 0.0, 0.0],
        inv_inertia_world=[[0.001, 0, 0, 0], [0, 0.001, 0, 0],
                           [0, 0, 0.001, 0]])),
    ("vel.y cap 120", dict(
        vel=[0.0, 119.0, 10.0], speed=119.4,
        accF=[0.0, 300000.0, 0.0, 0.0])),
    ("tilted frame rotation step", dict(
        frame=[[0.9950042, 0.0998334, 0.0, 0.0],
               [-0.0998334, 0.9950042, 0.0, 0.0],
               [0.0, 0.0, 1.0, 0.0], [-5.0, 1.0, 8.0, 0.0]],
        angmom_e0=[500.0, 1500.0, -300.0, 0.0],
        accT=[0.0, 4000.0, 0.0, 0.0],
        inv_inertia_body=[[0.0009, 0.0001, 0, 0], [0.0001, 0.0011, 0, 0],
                          [0, 0, 0.0005, 0]])),
    ("deflection accumulator", dict(defl_130=[0.5, 0.0, -0.25, 0.0])),
    ("stationary -> dir from at", dict(vel=[0.0, 0.0, 0.0], speed=0.0)),
]


def run_integ_cases():
    fails = 0
    print("\nrigid-body integration (FUN_00109560):")
    for name, kw in INTEG_CASES:
        env = integ_env(**kw)
        try:
            after, frame_emu = integ_run(env)
        except RuntimeError as e:
            print("  %-28s %s" % (name, e))
            fails += 1
            continue
        mod = integ_model(env)
        bad = []
        def cmpf(tag, mv, evv, tol=1e-3):
            if abs(mv - evv) > max(tol, abs(mv) * 2e-4):
                bad.append((tag, mv, evv))
        rd = lambda off: struct.unpack_from('<f', after, off)[0]
        for j in range(3):
            cmpf("vel[%d]" % j, mod['vel'][j], rd(0xB0 + 4*j), 1e-4)
            cmpf("omega[%d]" % j, mod['omega'][j], rd(0xD0 + 4*j), 1e-4)
            cmpf("L[%d]" % j, mod['angmom'][j], rd(0xE0 + 4*j), 1e-3)
            cmpf("dir[%d]" % j, mod['dir'][j], rd(0xC0 + 4*j), 1e-5)
        cmpf("speed", mod['speed'], rd(0xBC), 1e-4)
        for j in range(4):
            cmpf("impF[%d]" % j, mod['impF'][j], rd(0x110 + 4*j))
            cmpf("impT[%d]" % j, mod['impT'][j], rd(0x120 + 4*j))
            cmpf("accF[%d]" % j, mod['accF'][j], rd(0xF0 + 4*j))
            cmpf("accT[%d]" % j, mod['accT'][j], rd(0x100 + 4*j))
        for r in range(4):
            for c in range(3):
                cmpf("m[%d][%d]" % (r, c), mod['frame'][r][c],
                     frame_emu[r][c], 1e-5 if r < 3 else 1e-4)
        for r in range(3):
            for c in range(3):
                cmpf("Iw[%d][%d]" % (r, c), mod['inv_inertia_world'][r][c],
                     rd(0x40 + 0x10*r + 4*c), 1e-5)
        for r in range(4):
            for c in range(3):
                cmpf("inv[%d][%d]" % (r, c), mod['inv_frame'][r][c],
                     rd(0x70 + 0x10*r + 4*c), 1e-5)
        if bad:
            fails += 1
            print("  %-28s FAIL" % name)
            for tag, mv, evv in bad[:10]:
                print("      %-10s model %r  emu %r" % (tag, mv, evv))
        else:
            print("  %-28s OK   v=(%.2f, %.2f, %.2f) |w|=%.3f"
                  % (name, mod['vel'][0], mod['vel'][1], mod['vel'][2],
                     math.sqrt(max(dot3(mod['omega'], mod['omega']), 0.0))))
    return fails


# ===========================================================================
# Steering scheduler + slew -- inside FUN_0011ECF0 (the input stage).
#   max_deg = clamp(live_1384 - (speed - live_1380) * 1.4,
#                   steer_min_angle_1378, steer_max_angle_137C)
#   steer_1408 slews toward the raw input by at most steer_response_1388/frame
#   steering angle v+0x1164 = -(steer_1408 * max_deg)
# With v+0x215 == 3 the live 137C/1384/13C0 are restored from the config
# behind +0x13F8 (offsets +0x14C/+0x154/+0x190); otherwise the aggressive
# trio 13E8/13EC/13F0 (config +0x1C4/+0x1C8/+0x1CC) replaces them ("steer
# away" reaction). Mirrors b3_steer_schedule / b3_steer_slew.
# ===========================================================================
def model_steer(speed, s1384, s1380, smin, smax, steer_prev, steer_raw,
                resp):
    ang = f32(s1384 - f32(f32(speed - s1380) * 1.4))
    if ang < smin:
        ang = smin
    if smax < ang:
        ang = smax
    d = f32(steer_raw - steer_prev)
    steer = steer_raw
    if steer_raw <= steer_prev:
        if d < f32(0.0 - resp):
            steer = f32(steer_prev - resp)
    else:
        if d > resp:
            steer = f32(steer_prev + resp)
    return ang, steer, f32(0.0 - f32(steer * ang))


STEER_CASES = [
    # name, speed, raw steer, prev steer, response, cfg(min,max,1380,1384)
    ("slow full lock",      8.0, 1.0, 0.9, 0.2, (6.0, 40.0, 12.0, 45.0)),
    ("fast narrow angle",  55.0, 1.0, 1.0, 0.2, (6.0, 40.0, 12.0, 45.0)),
    ("slew limited up",    30.0, 1.0, 0.1, 0.15, (6.0, 40.0, 12.0, 45.0)),
    ("slew limited down",  30.0, -1.0, 0.5, 0.15, (6.0, 40.0, 12.0, 45.0)),
    ("mid speed partial",  25.0, 0.4, 0.38, 0.2, (6.0, 40.0, 12.0, 45.0)),
]


def run_steer_cases():
    fails = 0
    print("\nsteering scheduler + slew (FUN_0011ECF0):")
    for name, spd, raw, prev, resp, cfg in STEER_CASES:
        smin, smax, v1380, v1384 = cfg
        st = dict(ENGINE_BASE, omega=300.0, gear=3, speed_ms=spd)
        ov = {0x1400: 1.0, 0x1404: 0.0, 0xBC: spd,
              0xB0: 0.0, 0xB4: 0.0, 0xB8: spd,
              0x9FC: spd/0.346, 0xABC: spd/0.346,
              0x14A4: 0, 0x14A0: 0.0,
              0x1378: smin, 0x137C: smax, 0x1380: v1380, 0x1384: v1384,
              5000: resp,        # +0x1388 steer response
              0x1408: raw, 0x1424: prev, 0x13D0: 0.5, 0x1390: 0.9,
              0x1394: 0.3, 0x13C4: 40.0, 0x13C8: 2.0}
        for i, g in enumerate(st['gears']):
            ov[TRANS + i * 4] = g
        for k in ('idle', 'up', 'down', 'max', 'torque', 'limit', 'peak',
                  'falloff', 'kick_t', 'kick_time', 'omega', 'upblk',
                  'dnblk'):
            ov[OT[k]] = st[k]
        for k in ('rng_a', 'rng_c', 'gear', 'ngears'):
            ov[OT[k]] = st[k]
        ov[0x13D4] = st['max_boost_mph']
        mw = {DT_GLOBAL: struct.pack('<f', DT),
              RAND_SCALE_GLOBAL: struct.pack('<f', RAND_SCALE),
              ev.SCRATCH + 0x179C: struct.pack('<I', 1),
              # config behind +0x13F8: restored live steering params
              # (byte +0x215 == 3 selects the non-aggressive path)
              ev.SCRATCH + 0x14C: struct.pack('<f', smax),
              ev.SCRATCH + 0x154: struct.pack('<f', v1384),
              ev.SCRATCH + 0x190: struct.pack('<f', 0.0)}
        img = bytearray(ev.build_vehicle(ov))
        img[0x215] = 3
        tr, after, err = ev.run(ECF0, bytes(img), stack_args=[],
                                regs={UC_X86_REG_ECX: ev.VEHICLE,
                                      UC_X86_REG_ESI: f2u(spd),
                                      UC_X86_REG_EDI: 0},
                                mem_writes=mw)
        if err:
            print("  %-26s FAULT %s" % (name, err))
            fails += 1
            continue
        ang_m, steer_m, deg_m = model_steer(f32(spd), f32(v1384), f32(v1380),
                                            f32(smin), f32(smax), f32(prev),
                                            f32(raw), f32(resp))
        steer_e = struct.unpack_from('<f', after, 0x1408)[0]
        deg_e = struct.unpack_from('<f', after, 0x1164)[0]
        prev_e = struct.unpack_from('<f', after, 0x1424)[0]
        ok = (abs(steer_e - steer_m) < 1e-5 and abs(deg_e - deg_m) < 1e-3
              and abs(prev_e - steer_e) < 1e-6)
        print("  %-26s steer %.3f  angle %7.3f deg  %s"
              % (name, steer_e, deg_e, "OK" if ok else
                 "FAIL (model steer %.3f angle %.3f)" % (steer_m, deg_m)))
        fails += not ok
    return fails


# ===========================================================================
# AGGRESSIVE DRIVING REACTION ("steer away") -- FUN_0011ECF0's head, the
# 0x215 != 3 path this port used to skip entirely. The live steering trio
# (+0x137C max angle / +0x1384 schedule base / +0x13C0 steering drag) is
# replaced from the aggressive set while the owner is inside an
# out-of-control window, and for the first Steer-Away-Time seconds the
# steering INPUT (+0x1408) is forced to full lock signed by byte +0x153C.
# ===========================================================================
def model_steer_away_full(cls215, contact212, now, t_sa0, t_oo0, ang0,
                          aggr_vel, aggr_drag, cfg_max, cfg_maxvel, cfg_drag,
                          slam, wall):
    """Mirror of b3_steer_away_envelope (src/burnout3_vehicle_sim.c)."""
    window = phase1 = event2 = 0
    t_sa, t_oo, ang = t_sa0, t_oo0, ang0
    if cls215 == 3:
        return dict(window=0, phase1=0, event2=0, max=cfg_max,
                    base=cfg_maxvel, drag=cfg_drag)
    if not (slam < 0.0) and not (now > f32(slam + t_oo)):
        window = 1
    else:
        t_sa = f32(t_sa * 0.6)
        t_oo = f32(t_oo * 0.6)
        if not (wall < 0.0) and not (now > f32(wall + t_oo)):
            event2, window = 1, 1
            ang = f32(ang * 0.8)
    if contact212 == 0:
        t = wall if event2 else slam
        if not (t < 0.0) and not (now > f32(t + t_sa)):
            phase1 = 1
    if not window or contact212 != 0:
        return dict(window=window, phase1=phase1, event2=event2, max=cfg_max,
                    base=cfg_maxvel, drag=cfg_drag)
    if phase1:
        live_max = ang0                      # +0x13E8 read fresh
    else:
        t = wall if event2 else slam
        frac = f32(f32(now - f32(t + t_sa)) / f32(t_oo - t_sa))
        live_max = f32(f32(f32(ang - cfg_max) * f32(1.0 - frac)) + cfg_max)
    return dict(window=window, phase1=phase1, event2=event2, max=live_max,
                base=aggr_vel, drag=aggr_drag)


# name, class, contact212, now, slam(+0x1598), wall(+0x1690), hit side
STEER_AWAY_CASES = [
    ("no stamp -> config",      1, 0, 5.0, -1.0, -1.0, 0),
    ("class 3 ignores stamp",   3, 0, 5.0,  4.9, -1.0, 1),
    ("slam phase1 lock right",  1, 0, 5.0,  4.9, -1.0, 1),
    ("slam phase1 lock left",   1, 0, 5.0,  4.9, -1.0, 0),
    ("slam decay mid window",   1, 0, 5.0,  4.5, -1.0, 1),
    ("slam decay late window",  1, 0, 5.0,  4.15, -1.0, 0),
    ("slam expired",            1, 0, 5.0,  3.5, -1.0, 1),
    ("wall event 0.6 windows",  1, 0, 5.0, -1.0,  4.95, 1),
    ("wall event decay 0.8",    1, 0, 5.0, -1.0,  4.75, 0),
    ("chassis contact vetoes",  1, 1, 5.0,  4.9, -1.0, 1),
    ("traffic class 4 too",     4, 0, 5.0,  4.9, -1.0, 1),
]


def run_steer_away_cases():
    fails = 0
    print("\naggressive driving reaction / steer-away (FUN_0011ECF0 head):")
    # aggressive set = the recovered defaults; config set = COMPCAR1's
    T_SA, T_OO = 0.3, 1.0
    A_ANG, A_VEL, A_DRAG = 24.0, 50.0, 0.5
    C_MAX, C_MAXVEL, C_DRAG = 11.6, 15.0, 0.1
    SMIN, V1380, RESP = 10.6, 24.2, 0.088
    spd = 40.0
    for (name, cls, c212, now, slam, wall, side) in STEER_AWAY_CASES:
        st = dict(ENGINE_BASE, omega=300.0, gear=3, speed_ms=spd)
        ov = {0x1400: 1.0, 0x1404: 0.0, 0xBC: spd,
              0xB0: 0.0, 0xB4: 0.0, 0xB8: spd,
              0x9FC: spd/0.346, 0xABC: spd/0.346,
              0x14A4: 0, 0x14A0: 0.0,
              0x1378: SMIN, 0x137C: C_MAX, 0x1380: V1380, 0x1384: C_MAXVEL,
              5000: RESP,          # +0x1388 steer response
              0x1408: 0.0, 0x1424: 0.0, 0x13D0: 0.5, 0x1390: 0.9,
              0x1394: 0.3, 0x13C4: 40.0, 0x13C8: 2.0,
              0x13E0: T_SA, 0x13E4: T_OO, 0x13E8: A_ANG,
              0x13EC: A_VEL, 0x13F0: A_DRAG, 0x13C0: C_DRAG}
        for i, g in enumerate(st['gears']):
            ov[TRANS + i * 4] = g
        for k in ('idle', 'up', 'down', 'max', 'torque', 'limit', 'peak',
                  'falloff', 'kick_t', 'kick_time', 'omega', 'upblk',
                  'dnblk'):
            ov[OT[k]] = st[k]
        for k in ('rng_a', 'rng_c', 'gear', 'ngears'):
            ov[OT[k]] = st[k]
        ov[0x13D4] = st['max_boost_mph']
        mw = {DT_GLOBAL: struct.pack('<f', DT),
              RAND_SCALE_GLOBAL: struct.pack('<f', RAND_SCALE),
              ev.SCRATCH + 0x179C: struct.pack('<I', 1),
              # config behind +0x13F8
              ev.SCRATCH + 0x14C: struct.pack('<f', C_MAX),
              ev.SCRATCH + 0x154: struct.pack('<f', C_MAXVEL),
              ev.SCRATCH + 0x190: struct.pack('<f', C_DRAG),
              # the owner's OOC clock object: the game points
              # racecar+0x1198 at the racecar itself, which is why the
              # phase-2 lerp (reading +0x1598 DIRECTLY) agrees with the
              # window test (reading it through +0x1198)
              ev.SCRATCH + 0x1198: struct.pack('<I', ev.SCRATCH),
              ev.SCRATCH + 0x10DC: struct.pack('<f', now),
              ev.SCRATCH + 0x1598: struct.pack('<f', slam),
              ev.SCRATCH + 0x1690: struct.pack('<f', wall),
              ev.SCRATCH + 0x1350: struct.pack('<f', -100.0)}
        img = bytearray(ev.build_vehicle(ov))
        img[0x215] = cls
        img[0x212] = c212
        img[0x153C] = side
        tr, after, err = ev.run(ECF0, bytes(img), stack_args=[],
                                regs={UC_X86_REG_ECX: ev.VEHICLE,
                                      UC_X86_REG_ESI: f2u(spd),
                                      UC_X86_REG_EDI: 0},
                                mem_writes=mw)
        if err:
            print("  %-26s FAULT %s" % (name, err))
            fails += 1
            continue
        m = model_steer_away_full(cls, c212, f32(now), f32(T_SA), f32(T_OO),
                                  f32(A_ANG), f32(A_VEL), f32(A_DRAG),
                                  f32(C_MAX), f32(C_MAXVEL), f32(C_DRAG),
                                  f32(slam), f32(wall))
        max_e = struct.unpack_from('<f', after, 0x137C)[0]
        base_e = struct.unpack_from('<f', after, 0x1384)[0]
        drag_e = struct.unpack_from('<f', after, 0x13C0)[0]
        steer_e = struct.unpack_from('<f', after, 0x1408)[0]
        # the port's forced-lock rule, from the same flags
        steer_m = ((1.0 if side else -1.0)
                   if (m['window'] and m['phase1']) else 0.0)
        ok = (abs(max_e - m['max']) < 1e-4 and abs(base_e - m['base']) < 1e-5
              and abs(drag_e - m['drag']) < 1e-6
              and abs(steer_e - steer_m) < 1e-6)
        print("  %-26s win %d ph1 %d ev2 %d | maxang %7.3f base %5.1f "
              "drag %.2f steer %+.1f  %s"
              % (name, m['window'], m['phase1'], m['event2'], max_e, base_e,
                 drag_e, steer_e,
                 "OK" if ok else "FAIL (model max %.3f base %.1f drag %.2f "
                 "steer %+.1f)" % (m['max'], m['base'], m['drag'], steer_m)))
        fails += not ok
    return fails


# ===========================================================================
# FULL PIPELINE -- multi-frame differential trajectory. The real per-frame
# pipeline (FUN_0011ECF0 + FUN_0011BE50's main path: 2 substeps at dt/2 of
# FUN_0011D460 / [crash stub] / FUN_001239C0 / FUN_00123FD0 / stop-check /
# FUN_00109560) runs for consecutive frames under Unicorn on a fully seeded
# COMPCAR1 vehicle over a flat plane (tools/emulate_pipeline.py), and the C
# port b3_vehicle_step_full() (build/dump_traj) must reproduce the
# trajectory:
#   * strict window: 15 frames re-seeded from the emulation state at a
#     mid-scenario checkpoint -- pos/vel to 1e-2, rpm/omega to 1e-1,
#     gear + drift state exact (measured margins are 1e-6..1e-3)
#   * full run from t=0: gear/drift exact every frame, pos within 0.5 m,
#     speed within 0.15 m/s over 300-390 frames (fp noise at the
#     standstill fixed point and one PRNG borderline branch in deep
#     reverse bound the drift; see RE_NOTES 14)
# ===========================================================================
PIPELINE_WINDOWS = {
    # scenario: (checkpoint frame, window frames, window inputs)
    'accelerate': (190, 15, (1.0, 0.0, 0.0, 0)),
    'corner':     (300, 15, (0.6, 0.0, 0.5, 0)),
    'brake':      (250, 15, (0.0, 1.0, 0.0, 0)),
}


def run_pipeline_cases():
    import json
    import subprocess
    _pspec = importlib.util.spec_from_file_location(
        "ep", "tools/emulate_pipeline.py")
    ep = importlib.util.module_from_spec(_pspec)
    _pspec.loader.exec_module(ep)

    if not os.path.exists('build/dump_traj'):
        r = subprocess.run(['cc', '-O2', '-Isrc', '-o', 'build/dump_traj',
                            'tools/dump_traj.c',
                            'src/burnout3_vehicle_sim.c',
                            'src/burnout3_panels.c', '-lm'])
        if r.returncode != 0:
            print("\nfull pipeline: cannot build build/dump_traj")
            return len(PIPELINE_WINDOWS)

    fails = 0
    print("\nfull pipeline trajectory (FUN_0011ECF0 + FUN_0011BE50 vs "
          "b3_vehicle_step_full):")
    for name, (ckpt, wlen, inp) in PIPELINE_WINDOWS.items():
        p = ep.Pipeline()
        inputs = ep.scenario_inputs(name)
        emu_full = [p.frame(*inputs[i]) for i in range(ckpt)]
        sf = 'build/state_%s.txt' % name
        p.write_state(sf)
        emu_win = [p.frame(*inp) for _ in range(wlen)]

        # C full run from t=0 (coarse) and window from the checkpoint
        cf = subprocess.run(['./build/dump_traj', name],
                            capture_output=True, text=True)
        c_full = [json.loads(l) for l in cf.stdout.splitlines()]
        cw = subprocess.run(['./build/dump_traj', '--state', sf, str(wlen),
                             str(inp[0]), str(inp[1]), str(inp[2]),
                             str(inp[3])], capture_output=True, text=True)
        c_win = [json.loads(l) for l in cw.stdout.splitlines()]

        bad = []
        worst = dict(pos=0.0, vel=0.0, rpm=0.0, womega=0.0, omega=0.0)
        for i, (e, m) in enumerate(zip(emu_win, c_win)):
            for k in ('pos', 'vel'):
                for j in range(3):
                    worst[k] = max(worst[k], abs(e[k][j] - m[k][j]))
            worst['rpm'] = max(worst['rpm'], abs(e['rpm'] - m['rpm']))
            worst['omega'] = max(worst['omega'],
                                 max(abs(e['omega'][j] - m['omega'][j])
                                     for j in range(3)))
            for w in range(4):
                worst['womega'] = max(worst['womega'],
                                      abs(e['wheels'][w]['omega']
                                          - m['wheels'][w]['omega']))
            if e['gear'] != m['gear']:
                bad.append((i, 'gear', e['gear'], m['gear']))
            if e['drift'] != m['drift']:
                bad.append((i, 'drift', e['drift'], m['drift']))
        win_ok = (worst['pos'] < 1e-2 and worst['vel'] < 1e-2
                  and worst['rpm'] < 1e-1 and worst['womega'] < 1e-1
                  and worst['omega'] < 1e-1 and not bad)

        full_worst = dict(pos=0.0, speed=0.0)
        full_state_bad = 0
        for e, m in zip(emu_full, c_full):
            full_worst['pos'] = max(full_worst['pos'],
                                    max(abs(e['pos'][j] - m['pos'][j])
                                        for j in range(3)))
            full_worst['speed'] = max(full_worst['speed'],
                                      abs(e['speed'] - m['speed']))
            full_state_bad += (e['gear'] != m['gear'])
            full_state_bad += (e['drift'] != m['drift'])
        full_ok = (full_worst['pos'] < 0.5 and full_worst['speed'] < 0.15
                   and full_state_bad == 0)

        ok = win_ok and full_ok
        fails += not ok
        print("  %-11s window(f%d+%d): pos %.1e vel %.1e rpm %.1e "
              "womega %.1e | full(%df): pos %.3f speed %.3f states %d  %s"
              % (name, ckpt, wlen, worst['pos'], worst['vel'],
                 worst['rpm'], worst['womega'], len(emu_full),
                 full_worst['pos'], full_worst['speed'], full_state_bad,
                 "OK" if ok else "FAIL"))
        if bad:
            for b in bad[:4]:
                print("      window state mismatch:", b)
    return fails


# ===========================================================================
# ADVERSARIAL TRAJECTORIES -- the cases the three "drive it normally"
# windows above cannot reach, all of them about the car's HEADING: a
# mid-corner lateral/yaw impulse (a takedown knock), a big-slip recovery,
# a sideways airborne re-entry (the drift-entry clock reset that was
# missing from this port), and both out-of-control steer-away windows.
# Each one runs the base scenario to a checkpoint under Unicorn, mutates
# the emulated body (tools/emulate_pipeline.Pipeline.mutate), exports that
# state to the C port, and then requires the two to agree on the heading
# trajectory, not just the position.
# ===========================================================================
ADVERSARIAL = {
    # name: (base scenario, checkpoint, window frames, inputs, mutation)
    'yaw kick mid-corner':
        ('corner', 300, 120, (0.6, 0.0, 0.5, 0),
         dict(angmom_add=(0.0, 900.0, 0.0))),
    'counter kick + steer':
        ('corner', 300, 120, (0.8, 0.0, -0.6, 0),
         dict(angmom_add=(0.0, -1400.0, 0.0))),
    'high-slip recovery':
        ('accelerate', 190, 180, (1.0, 0.0, 0.0, 0),
         dict(angmom_add=(0.0, 2600.0, 0.0))),
    'lateral shove':
        ('accelerate', 190, 120, (1.0, 0.0, 0.0, 0),
         dict(vel_kick=(9.0, 0.0, 0.0), angmom_add=(0.0, 400.0, 0.0))),
    # a 30 m/s sideways re-entry: all four wheels bottom out, the
    # suspension contact impulse fires, and the landing trips the
    # drift-entry path. A hard impact is a genuine fp amplifier, so the
    # position/velocity bands are wider here -- the parity claim is the
    # HEADING (and every discrete state, asserted frame by frame).
    'sideways air landing':
        ('corner', 300, 150, (0.5, 0.0, 0.0, 0),
         dict(pos_add=(0.0, 1.6, 0.0), sideways=True, wheels_airborne=True,
              airtime=0.5, f1168=1,
              _tol=dict(pos=0.2, vel=0.2, omega=0.1, steer=0.05))),
    'ooc slam steer-away':
        ('corner', 300, 120, (0.7, 0.0, 0.2, 0),
         dict(class_215=1, hit_side=1, slam_now=True)),
    'ooc wall steer-away':
        ('corner', 300, 120, (0.7, 0.0, -0.2, 0),
         dict(class_215=1, hit_side=0, wall_now=True)),
    'long heading hold':
        ('corner', 240, 330, (0.9, 0.0, 0.35, 0), {}),
}


def _heading(rec):
    return math.atan2(rec['at'][0], rec['at'][2])


def run_adversarial_cases():
    import json
    import subprocess
    _pspec = importlib.util.spec_from_file_location(
        "ep2", "tools/emulate_pipeline.py")
    ep = importlib.util.module_from_spec(_pspec)
    _pspec.loader.exec_module(ep)

    if not os.path.exists('build/dump_traj'):
        r = subprocess.run(['cc', '-O2', '-Isrc', '-o', 'build/dump_traj',
                            'tools/dump_traj.c',
                            'src/burnout3_vehicle_sim.c',
                            'src/burnout3_panels.c', '-lm'])
        if r.returncode != 0:
            print("\nadversarial: cannot build build/dump_traj")
            return len(ADVERSARIAL)

    fails = 0
    print("\nadversarial heading trajectories (real FUN_0011BE50 path vs "
          "b3_vehicle_step_full):")
    for name, (base, ckpt, wlen, inp, mut) in ADVERSARIAL.items():
        p = ep.Pipeline()
        inputs = ep.scenario_inputs(base)
        for i in range(ckpt):
            p.frame(*inputs[i])

        m = dict(mut)
        tol = dict(pos=5e-2, vel=5e-2, omega=5e-2, head=0.0087, steer=1e-2)
        tol.update(m.pop('_tol', {}))
        # mutations that need the checkpoint state to be resolved first
        if m.pop('sideways', False):
            # re-aim the velocity along the body's RIGHT axis so the
            # landing trips FUN_0011D460's |dir.right| > cos(90 - Min Drift
            # Angle In Air) sideways-entry test
            row0 = [p.rf(ep.CTX0 + 4 * i) for i in range(3)]
            sp = p.vf(0xBC)
            m['vel_set'] = [row0[i] * sp for i in range(3)]
        if 'vel_kick' in m:
            k = m.pop('vel_kick')
            m['vel_set'] = [p.vf(0xB0 + 4 * i) + k[i] for i in range(3)]
        if m.pop('slam_now', False):
            m['slam_1598'] = p.clock
        if m.pop('wall_now', False):
            m['wall_1690'] = p.clock
        p.mutate(m)

        sf = 'build/state_adv_%s.txt' % name.replace(' ', '_')
        p.write_state(sf)
        emu = [p.frame(*inp) for _ in range(wlen)]

        cw = subprocess.run(['./build/dump_traj', '--state', sf, str(wlen),
                             str(inp[0]), str(inp[1]), str(inp[2]),
                             str(inp[3])], capture_output=True, text=True)
        cport = [json.loads(l) for l in cw.stdout.splitlines()]

        worst = dict(pos=0.0, vel=0.0, omega=0.0, angmom=0.0, head=0.0,
                     steer=0.0)
        bad = []
        for i, (e, c) in enumerate(zip(emu, cport)):
            for k in ('pos', 'vel', 'omega', 'angmom'):
                worst[k] = max(worst[k],
                               max(abs(e[k][j] - c[k][j]) for j in range(3)))
            dh = _heading(e) - _heading(c)
            while dh > math.pi:
                dh -= 2 * math.pi
            while dh < -math.pi:
                dh += 2 * math.pi
            worst['head'] = max(worst['head'], abs(dh))
            worst['steer'] = max(worst['steer'],
                                 abs(e['steer_deg'] - c['steer_deg']))
            if e['drift'] != c['drift']:
                bad.append((i, 'drift', e['drift'], c['drift']))
            if e['gear'] != c['gear']:
                bad.append((i, 'gear', e['gear'], c['gear']))
            if e['airborne'] != c['airborne']:
                bad.append((i, 'air', e['airborne'], c['airborne']))
        # heading tolerance: 0.5 deg (0.0087 rad) over the whole window
        ok = (worst['pos'] < tol['pos'] and worst['vel'] < tol['vel']
              and worst['omega'] < tol['omega'] and worst['head'] < tol['head']
              and worst['steer'] < tol['steer'] and not bad)
        # net heading change over the window, so the log shows the case
        # actually rotated the car
        swing = math.degrees(abs(_heading(emu[-1]) - _heading(emu[0])))
        print("  %-22s %3df swing %6.1f deg | pos %.1e vel %.1e omega %.1e "
              "head %.1e rad  %s"
              % (name, wlen, swing, worst['pos'], worst['vel'],
                 worst['omega'], worst['head'], "OK" if ok else "FAIL"))
        if bad:
            for b in bad[:4]:
                print("      state mismatch:", b)
        fails += not ok
    return fails


import os



# ===========================================================================
# BODY-vs-WORLD CONTACT RESOLUTION -- FUN_00109EA0 (0x00109EA0..0x0010A43A).
#
# THE shared contact resolve: every rigid body in the game runs it once per
# frame through vtable slot +0x10 of its class, right after the local polygon
# soup is gathered (FUN_00109D20) and BEFORE the class's own update slot runs
# the integrator FUN_00109560:
#     class 6  knocked props     0x003B1120+0x10 = FUN_0011A490 @0x0011A706
#     class 7  panels / debris   0x003B1108+0x10 = FUN_001072A0 @0x001073CF
#     racecar                                                   @0x00122F81
#
# The narrow phase inside it (FUN_00107950 when +0x20C == 1, FUN_0010AAD0
# otherwise) is a pure PRODUCER of one contact -- point +0x160, normal +0x170,
# a push-out vector and the hit byte +0x212. Both branches converge on
# 0x00109FD8, so the emulation below starts there with the prologue's stack
# frame rebuilt exactly (esp = S 16-aligned, [S+0x30] = the push-out,
# [S+0..0xB] = saved edi/esi/ebx, ebp = the caller frame) and runs the real
# instructions 0x00109FD8..0x0010A43A verbatim -- nothing is patched.
#
# The C side is the REAL port: build/dump_traj --wcontact runs
# b3_rigid_body_world_contact() on the identical state.
#
# The two BSS thresholds are C++ static initialisers and must be seeded, or
# every gate reads 0.0 and inverts:
#     [0x005A538C] = 1440000.0   written only by the thunk @0x002B92A0
#     [0x005A3A94] = 0.09        written only by the thunk @0x002B9280
# ===========================================================================
WC_ENTRY = 0x00109FD8
WC_BODY = 0x30000000
WC_FRAME = 0x40000000
WC_STACK = 0x20000000
WC_MAGIC = 0x50000000
WC_GATE_ADDR = 0x005A538C
WC_SLEEPW_ADDR = 0x005A3A94


def _wc_v4(img, off, v):
    for i in range(4):
        struct.pack_into('<f', img, off + 4 * i, float(v[i]))


def wc_emulate(st):
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UcError
    from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EBP,
                                   UC_X86_REG_EIP, UC_X86_REG_EAX,
                                   UC_X86_REG_EBX, UC_X86_REG_ECX,
                                   UC_X86_REG_EDX, UC_X86_REG_ESI,
                                   UC_X86_REG_EDI)
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, ev.ELF)
    uc.mem_map(WC_STACK, 0x10000, UC_PROT_ALL)
    uc.mem_map(WC_BODY, 0x1000, UC_PROT_ALL)
    uc.mem_map(WC_FRAME, 0x1000, UC_PROT_ALL)
    uc.mem_map(WC_MAGIC & ~0xFFF, 0x1000, UC_PROT_ALL)

    img = bytearray(0x1000)
    _wc_v4(img, 0xB0, st['vel'])
    _wc_v4(img, 0xC0, st.get('dir', [0.0, 0.0, 1.0, 0.0]))
    _wc_v4(img, 0xD0, st['omega'])
    _wc_v4(img, 0xE0, st['angmom'])
    _wc_v4(img, 0xF0, st.get('force', [0.0] * 4))
    _wc_v4(img, 0x100, st.get('torque', [0.0] * 4))
    _wc_v4(img, 0x110, st.get('impf', [0.0] * 4))
    _wc_v4(img, 0x120, st.get('impt', [0.0] * 4))
    _wc_v4(img, 0x130, st.get('defl', [0.0] * 4))
    _wc_v4(img, 0x160, st['point'])
    _wc_v4(img, 0x170, st['normal'])
    for r in range(3):
        _wc_v4(img, 0x40 + 0x10 * r, st['invI'][r])
    struct.pack_into('<f', img, 0x1F0, float(st['mass']))
    struct.pack_into('<f', img, 0x1F8, float(st['restitution']))
    struct.pack_into('<I', img, 0x204, WC_FRAME)
    img[0x212] = 1 if st['hit'] else 0
    img[0x215] = st['cls'] & 0xFF
    img[0x2BA] = st.get('attach', 0) & 0xFF
    uc.mem_write(WC_BODY, bytes(img))

    fr = bytearray(0x1000)
    for r in range(4):
        _wc_v4(fr, 0x10 * r, st['frame'][r])
    uc.mem_write(WC_FRAME, bytes(fr))
    uc.mem_write(WC_GATE_ADDR, struct.pack('<f', 1440000.0))
    uc.mem_write(WC_SLEEPW_ADDR, struct.pack('<f', 0.09))

    S = WC_STACK + 0x8000
    stk = bytearray(0x400)
    struct.pack_into('<III', stk, 0, 0xDEAD0000, 0xDEAD0001, 0xDEAD0002)
    _wc_v4(stk, 0x30, st['pushout'])
    uc.mem_write(S, bytes(stk))
    ebp = S + 0x300
    uc.mem_write(ebp, struct.pack('<II', 0xCAFE0000, WC_MAGIC))

    uc.reg_write(UC_X86_REG_ESP, S)
    uc.reg_write(UC_X86_REG_EBP, ebp)
    uc.reg_write(UC_X86_REG_ESI, WC_BODY)
    uc.reg_write(UC_X86_REG_EBX, WC_BODY + 0x170)
    uc.reg_write(UC_X86_REG_EDI, WC_BODY + 0x160)
    for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX):
        uc.reg_write(r, WC_BODY)

    err = None
    try:
        uc.emu_start(WC_ENTRY, WC_MAGIC, count=2_000_000)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
    if err:
        raise RuntimeError("emulation faulted: " + err)
    after = bytes(uc.mem_read(WC_BODY, 0x1000))
    g = lambda off: struct.unpack_from('<f', after, off)[0]
    g4 = lambda off: [g(off + 4 * i) for i in range(4)]
    return dict(vel=g4(0xB0), omega=g4(0xD0), angmom=g4(0xE0),
                force=g4(0xF0), torque=g4(0x100), impf=g4(0x110),
                impt=g4(0x120), defl=g4(0x130), pushout=g4(0x180),
                impact=g(0x194),
                valid=struct.unpack_from('<I', after, 0x198)[0],
                sleep=after[0x20E], impulsed=after[0x213], hit=after[0x212])


def wc_port(st):
    """Run the compiled C port over the same state."""
    import json
    import os
    import subprocess
    import tempfile
    vals = []
    for r in range(4):
        vals += list(st['frame'][r])
    for k in ('vel', 'omega', 'angmom'):
        vals += list(st[k])
    for k in ('force', 'torque', 'impf', 'impt', 'defl'):
        vals += list(st.get(k, [0.0] * 4))
    for r in range(3):
        vals += list(st['invI'][r])
    vals += [st['mass'], st['restitution'], float(st['cls']),
             float(st.get('attach', 0)), 1.0 if st['hit'] else 0.0]
    for k in ('point', 'normal', 'pushout'):
        vals += list(st[k])
    fd, path = tempfile.mkstemp(suffix=".wc")
    try:
        with os.fdopen(fd, 'w') as f:
            f.write(" ".join(repr(float(x)) for x in vals))
        out = subprocess.run(["build/dump_traj", "--wcontact", path],
                             capture_output=True, text=True, check=True)
    finally:
        os.unlink(path)
    return json.loads(out.stdout)


def wc_state(**kw):
    st = dict(
        frame=[[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0],
               [0.0, 0.0, 1.0, 0.0], [0.0, 0.5, 0.0, 0.0]],
        vel=[0.0, -5.0, 0.0, 5.0],
        omega=[0.0, 0.0, 0.0, 0.0],
        angmom=[0.0, 0.0, 0.0, 0.0],
        invI=[[0.02, 0.0, 0.0, 0.0], [0.0, 0.025, 0.0, 0.0],
              [0.0, 0.0, 0.018, 0.0]],
        mass=100.0, restitution=0.1, cls=6, attach=0, hit=1,
        point=[0.0, 0.0, 0.0, 0.0], normal=[0.0, 1.0, 0.0, 0.0],
        pushout=[0.0, 0.02, 0.0, 0.0])
    st.update(kw)
    return st


# A 45 deg banked frame, for the settle test's "no frame row is within 0.99
# of the contact normal" arm.
_C45 = 0.70710678
WC_CASES = [
    # --- no contact: retail's 0x00109ECA early-out --------------------------
    ("no contact (empty soup)", dict(hit=0)),
    # --- class 6, the knocked prop -----------------------------------------
    ("class6 slow ground hit", dict()),
    ("class6 tumbling oblique", dict(
        vel=[3.0, -6.0, 1.0, 6.8], omega=[0.4, -1.2, 2.0, 0.0],
        angmom=[40.0, -120.0, 200.0, 0.0],
        point=[0.2, 0.0, -0.1, 0.0],
        normal=[0.26726124, 0.80178373, 0.53452248, 0.0],
        pushout=[0.008, 0.024, 0.016, 0.0])),
    ("class6 heavy slide -> force arm", dict(
        mass=4000.0, vel=[40.0, -2.0, 12.0, 41.8],
        omega=[0.0, 0.5, 0.0, 0.0], angmom=[0.0, 5000.0, 0.0, 0.0],
        invI=[[0.0002, 0, 0, 0], [0, 0.0003, 0, 0], [0, 0, 0.00025, 0]])),
    ("class6 settle -> sleep latch", dict(
        vel=[0.05, -0.02, 0.03, 0.062], omega=[0.05, -0.1, 0.08, 0.0],
        angmom=[2.0, -4.0, 3.0, 0.0], pushout=[0.0, 0.001, 0.0, 0.0])),
    ("class6 settle, spin too fast", dict(
        vel=[0.05, -0.02, 0.03, 0.062], omega=[0.3, -0.4, 0.2, 0.0],
        angmom=[20.0, -30.0, 15.0, 0.0])),
    ("class6 slow, no axis aligned", dict(
        vel=[0.1, -0.2, 0.1, 0.24], omega=[0.05, 0.0, 0.0, 0.0],
        angmom=[3.0, 1.0, -2.0, 0.0],
        frame=[[_C45, _C45, 0.0, 0.0], [-_C45, _C45, 0.0, 0.0],
               [0.0, 0.0, 1.0, 0.0], [0.0, 0.4, 0.0, 0.0]])),
    ("class6 slow, row0 aligned", dict(
        vel=[0.5, -0.2, 0.1, 0.55], omega=[0.02, 0.0, 0.0, 0.0],
        angmom=[3.0, 1.0, -2.0, 0.0],
        frame=[[0.0, 1.0, 0.0, 0.0], [1.0, 0.0, 0.0, 0.0],
               [0.0, 0.0, -1.0, 0.0], [0.0, 0.4, 0.0, 0.0]])),
    ("class6 wall normal (+x)", dict(
        vel=[-12.0, 0.5, 2.0, 12.2], omega=[0.0, 2.0, 0.0, 0.0],
        angmom=[0.0, 80.0, 0.0, 0.0], normal=[1.0, 0.0, 0.0, 0.0],
        point=[-0.3, 0.5, 0.0, 0.0], pushout=[0.05, 0.0, 0.0, 0.0])),
    ("class6 separating (j <= 0)", dict(
        vel=[0.0, 4.0, 0.0, 4.0], pushout=[0.0, 0.0, 0.0, 0.0])),
    # --- class 7, the panel / debris piece ---------------------------------
    ("class7 attach 0 (0.9 damp)", dict(cls=7, attach=0, mass=20.0,
        vel=[6.0, -9.0, 2.0, 11.2], omega=[3.0, -1.0, 5.0, 0.0],
        angmom=[30.0, -10.0, 50.0, 0.0])),
    ("class7 attach 2 (0.6 damp)", dict(cls=7, attach=2, mass=20.0,
        vel=[6.0, -9.0, 2.0, 11.2], omega=[3.0, -1.0, 5.0, 0.0],
        angmom=[30.0, -10.0, 50.0, 0.0])),
    ("class7 wheel restitution 0.7", dict(cls=7, attach=1, mass=25.0,
        restitution=0.7, vel=[2.0, -12.0, 0.0, 12.2],
        omega=[0.0, 0.0, 8.0, 0.0], angmom=[0.0, 0.0, 90.0, 0.0])),
    ("class7 settle -> sleep latch", dict(cls=7, attach=0, mass=20.0,
        vel=[0.04, -0.05, 0.02, 0.068], omega=[0.1, 0.05, -0.05, 0.0],
        angmom=[1.0, 0.5, -0.5, 0.0])),
    # --- the racecar states 1/2/3 (gate 1.0, damp 0.95, no gravity add) ----
    ("class1 racecar force arm", dict(cls=1, mass=1000.0,
        vel=[20.0, -1.0, 5.0, 20.6], omega=[0.0, 1.0, 0.0, 0.0],
        angmom=[0.0, 900.0, 0.0, 0.0],
        invI=[[0.0008, 0, 0, 0], [0, 0.0011, 0, 0], [0, 0, 0.0013, 0]])),
    ("class2 racecar damp arm", dict(cls=2, mass=1000.0,
        vel=[0.0, -0.02, 0.0, 0.02], omega=[0.0, 0.0, 0.0, 0.0],
        angmom=[0.0, 0.0, 0.0, 0.0],
        invI=[[0.0008, 0, 0, 0], [0, 0.0011, 0, 0], [0, 0, 0.0013, 0]])),
    ("class3 racecar oblique", dict(cls=3, mass=1200.0,
        vel=[8.0, -3.0, 15.0, 17.3], omega=[0.2, 0.6, -0.1, 0.0],
        angmom=[240.0, 700.0, -120.0, 0.0],
        normal=[0.0, 0.9486833, 0.31622776, 0.0],
        invI=[[0.0008, 0, 0, 0], [0, 0.0011, 0, 0], [0, 0, 0.0013, 0]])),
    # --- the "anything else" 0.875 arm -------------------------------------
    ("class0 default 0.875 damp", dict(cls=0, mass=800.0,
        vel=[1.0, -2.0, 0.5, 2.3], omega=[0.1, 0.2, 0.3, 0.0],
        angmom=[80.0, 160.0, 240.0, 0.0])),
    ("class5 default 0.875 damp", dict(cls=5, mass=800.0,
        vel=[1.0, -2.0, 0.5, 2.3], omega=[0.1, 0.2, 0.3, 0.0],
        angmom=[80.0, 160.0, 240.0, 0.0])),
    # --- accumulators must be added to, never replaced ---------------------
    ("pre-loaded accumulators", dict(
        vel=[2.0, -8.0, 1.0, 8.3], omega=[0.5, 0.0, -0.5, 0.0],
        angmom=[50.0, 0.0, -50.0, 0.0],
        force=[10.0, 20.0, 30.0, 40.0], torque=[1.0, 2.0, 3.0, 4.0],
        impf=[5.0, 6.0, 7.0, 8.0], impt=[9.0, 10.0, 11.0, 12.0],
        defl=[0.1, 0.2, 0.3, 0.4])),
]


# ===========================================================================
# CLASS-7 (panel / debris piece) PER-FRAME UPDATE -- FUN_00106D00
# (0x00106D00..0x00106EE8), vtable 0x003B1108 slot +0.  Driven once per frame
# per allocated slot of the 0x40-entry pool at gameworld+0xD3380 (stride
# 0x4E0) by the collision manager FUN_00110AF0, AFTER slot +0x10
# (FUN_001072A0) has resolved the piece against the world.
#
# The whole function runs under Unicorn -- entry 0x00106D00, `this` in ECX,
# dt on the stack (`ret 4`) -- against the C port in build/dump_traj
# --class7.  The +0x2BA == 1 pinned-pose tail (@0x00106E6A) is presentation
# and is excluded by never using attach == 1 here.
# ===========================================================================
C7_ENTRY = 0x00106D00


def c7_emulate(st, dt):
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UcError
    from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP,
                                   UC_X86_REG_EAX, UC_X86_REG_EBX,
                                   UC_X86_REG_ECX, UC_X86_REG_EDX)
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, ev.ELF)
    uc.mem_map(WC_STACK, 0x10000, UC_PROT_ALL)
    uc.mem_map(WC_BODY, 0x1000, UC_PROT_ALL)
    uc.mem_map(WC_FRAME, 0x1000, UC_PROT_ALL)
    uc.mem_map(WC_MAGIC & ~0xFFF, 0x1000, UC_PROT_ALL)

    img = bytearray(0x1000)
    _wc_v4(img, 0xB0, st['vel'])
    _wc_v4(img, 0xC0, st['dir'])
    _wc_v4(img, 0xD0, st['omega'])
    _wc_v4(img, 0xE0, st['angmom'])
    _wc_v4(img, 0xF0, st.get('force', [0.0] * 4))
    _wc_v4(img, 0x100, st.get('torque', [0.0] * 4))
    _wc_v4(img, 0x110, st.get('impf', [0.0] * 4))
    _wc_v4(img, 0x120, st.get('impt', [0.0] * 4))
    _wc_v4(img, 0x130, st.get('defl', [0.0] * 4))
    _wc_v4(img, 0x170, st['normal'])
    for r in range(3):
        _wc_v4(img, 0x10 + 0x10 * r, st['invI_body'][r])
        _wc_v4(img, 0x40 + 0x10 * r, st['invI'][r])
    struct.pack_into('<f', img, 0x1F0, float(st['mass']))
    struct.pack_into('<f', img, 0x1F4, float(st.get('com', 0.0)))
    struct.pack_into('<I', img, 0x204, WC_FRAME)
    img[0x210] = 0
    img[0x212] = 1 if st['grounded'] else 0
    img[0x215] = 7
    img[0x216] = st.get('unit', 0) & 0xFF
    img[0x2BA] = st.get('attach', 0) & 0xFF
    img[0x4D0] = 1 if st.get('suppress') else 0
    uc.mem_write(WC_BODY, bytes(img))

    fr = bytearray(0x1000)
    for r in range(4):
        _wc_v4(fr, 0x10 * r, st['frame'][r])
    uc.mem_write(WC_FRAME, bytes(fr))

    sp = WC_STACK + 0x8000
    uc.mem_write(sp, struct.pack('<II', WC_MAGIC, f2u(dt)))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_ECX, WC_BODY)
    for r in (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_EDX):
        uc.reg_write(r, WC_BODY)
    err = None
    try:
        uc.emu_start(C7_ENTRY, WC_MAGIC, count=4_000_000)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
    if err:
        raise RuntimeError("emulation faulted: " + err)
    after = bytes(uc.mem_read(WC_BODY, 0x1000))
    frame = [[struct.unpack_from('<f', uc.mem_read(WC_FRAME, 0x40),
                                 0x10 * r + 4 * c)[0] for c in range(4)]
             for r in range(4)]
    g = lambda off: struct.unpack_from('<f', after, off)[0]
    g4 = lambda off: [g(off + 4 * i) for i in range(4)]
    return dict(vel=g4(0xB0), dir=g4(0xC0), omega=g4(0xD0), angmom=g4(0xE0),
                force=g4(0xF0), torque=g4(0x100), impf=g4(0x110),
                impt=g4(0x120), defl=g4(0x130), frame=frame,
                suppress=after[0x4D0])


def c7_port(st, dt):
    import json
    import os
    import subprocess
    import tempfile
    vals = []
    for r in range(4):
        vals += list(st['frame'][r])
    for k in ('vel', 'dir', 'omega', 'angmom'):
        vals += list(st[k])
    for k in ('force', 'torque', 'impf', 'impt', 'defl'):
        vals += list(st.get(k, [0.0] * 4))
    for r in range(3):
        vals += list(st['invI_body'][r])
    for r in range(3):
        vals += list(st['invI'][r])
    vals += list(st['normal'])
    vals += [st['mass'], st.get('com', 0.0), dt,
             float(st.get('suppress', 0)), float(st.get('unit', 0)),
             float(st.get('attach', 0)), 1.0 if st['grounded'] else 0.0]
    fd, path = tempfile.mkstemp(suffix=".c7")
    try:
        with os.fdopen(fd, 'w') as f:
            f.write(" ".join(repr(float(x)) for x in vals))
        out = subprocess.run(["build/dump_traj", "--class7", path],
                             capture_output=True, text=True, check=True)
    finally:
        os.unlink(path)
    return json.loads(out.stdout)


def c7_state(**kw):
    st = dict(
        frame=[[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0],
               [0.0, 0.0, 1.0, 0.0], [3.0, 1.2, -7.0, 0.0]],
        vel=[4.0, -3.0, 2.0, 5.385],
        dir=[0.7428, -0.5571, 0.3714, 0.0],
        omega=[1.0, -2.0, 3.0, 0.0],
        angmom=[10.0, -20.0, 30.0, 0.0],
        normal=[0.0, 1.0, 0.0, 0.0],
        invI_body=[[0.4, 0.0, 0.0, 0.0], [0.0, 0.5, 0.0, 0.0],
                   [0.0, 0.0, 0.35, 0.0]],
        invI=[[0.4, 0.0, 0.0, 0.0], [0.0, 0.5, 0.0, 0.0],
              [0.0, 0.0, 0.35, 0.0]],
        mass=20.0, com=0.0, grounded=1, attach=0, unit=0, suppress=0)
    st.update(kw)
    return st


_S45 = 0.70710678
C7_CASES = [
    ("suppress latch (+0x4D0)", dict(suppress=1)),
    ("outside a unit (+0x216=255)", dict(unit=0xFF)),
    ("airborne, no contact", dict(grounded=0)),
    ("attached: L *= 0.98", dict(attach=2)),
    ("grounded, right axis across n", dict()),
    ("grounded + slow -> L zeroed", dict(
        vel=[0.02, -0.03, 0.01, 0.037], omega=[0.2, -0.1, 0.05, 0.0],
        angmom=[2.0, -1.0, 0.5, 0.0])),
    ("grounded, right axis ALONG n", dict(
        frame=[[0.0, 1.0, 0.0, 0.0], [1.0, 0.0, 0.0, 0.0],
               [0.0, 0.0, -1.0, 0.0], [3.0, 1.2, -7.0, 0.0]])),
    ("grounded, tilted 45 frame", dict(
        frame=[[_S45, _S45, 0.0, 0.0], [-_S45, _S45, 0.0, 0.0],
               [0.0, 0.0, 1.0, 0.0], [3.0, 1.2, -7.0, 0.0]])),
    ("grounded, oblique normal", dict(
        normal=[0.26726124, 0.80178373, 0.53452248, 0.0])),
    ("fast piece, big drag", dict(
        vel=[18.0, -6.0, 9.0, 21.0], dir=[0.857, -0.286, 0.428, 0.0],
        omega=[6.0, -3.0, 9.0, 0.0], angmom=[60.0, -30.0, 90.0, 0.0])),
    ("pre-loaded accumulators", dict(
        force=[7.0, 8.0, 9.0, 0.0], torque=[1.0, 2.0, 3.0, 0.0],
        impf=[4.0, 5.0, 6.0, 0.0], impt=[7.0, 8.0, 9.0, 0.0],
        defl=[0.05, 0.1, -0.05, 0.0])),
    ("heavy piece, com offset", dict(mass=90.0, com=0.4,
        invI_body=[[0.09, 0, 0, 0], [0, 0.11, 0, 0], [0, 0, 0.08, 0]],
        invI=[[0.09, 0, 0, 0], [0, 0.11, 0, 0], [0, 0, 0.08, 0]])),
]


def run_class7_cases():
    fails = 0
    dt = 1.0 / 60.0
    print("\nclass-7 panel/debris update (FUN_00106D00), "
          "real x86 vs the C port:")
    for name, kw in C7_CASES:
        st = c7_state(**kw)
        try:
            emu = c7_emulate(st, dt)
            port = c7_port(st, dt)
        except Exception as e:
            print("  %-32s %s" % (name, e))
            fails += 1
            continue
        bad = []

        def cmpf(tag, pv, ev_, tol):
            if abs(pv - ev_) > max(tol, abs(ev_) * 5e-5):
                bad.append((tag, pv, ev_))

        for key in ('vel', 'dir', 'omega', 'angmom', 'force', 'torque',
                    'impf', 'impt', 'defl'):
            for i in range(4):
                cmpf("%s[%d]" % (key, i), port[key][i], emu[key][i], 1e-4)
        for r in range(4):
            for c in range(3):
                cmpf("m[%d][%d]" % (r, c), port['frame'][r][c],
                     emu['frame'][r][c], 1e-5)
        if int(port['suppress']) != int(emu['suppress']):
            bad.append(('suppress', port['suppress'], emu['suppress']))
        if bad:
            fails += 1
            print("  %-32s FAIL" % name)
            for tag, pv, ev_ in bad[:8]:
                print("      %-12s port %r  emu %r" % (tag, pv, ev_))
        else:
            print("  %-32s OK   |v|=%.3f |L|=%.2f pos=(%.2f %.2f %.2f)"
                  % (name,
                     math.sqrt(sum(x * x for x in emu['vel'][:3])),
                     math.sqrt(sum(x * x for x in emu['angmom'][:3])),
                     emu['frame'][3][0], emu['frame'][3][1],
                     emu['frame'][3][2]))
    return fails


# ===========================================================================
# PHYS-LEDGER-4 / PH-05 -- the FLYING-PART SEEDING law, FUN_00109BB0
# (0x00109BB0..0x00109CD0) -> FUN_00109190 (0x00109190).
#
# FUN_001069C0, the activation ctor FUN_00111340 calls the moment it takes a
# pool slot for a detaching panel/wheel/debris piece, sets the piece's OBB
# (+0x1D0 bbmax / +0x1E0 bbmin) and its MASS (+0x1F0 = 0x43820000 = 260.0f,
# unconditional, all three modes) and then calls FUN_00109BB0 to build the
# inverse inertia.  Ghidra's decompile of FUN_00109BB0 only shows the first
# axis; the disassembly has all three, with K = [0x003B1684] = 0.5 and
# N = [0x003B168C] = 1.0:
#     a = max(bbmax.x, -bbmin.x)   b, c likewise
#     diag = ( N/(K*m*(b*b+c*c)), N/(K*m*(a*a+c*c)), N/(K*m*(a*a+b*b)) )
# FUN_00109190 writes them to body+0x10 / +0x24 / +0x38.
#
# Entry EAX = the body, and FUN_00109190's tail reads body+0x204 as a matrix
# pointer, so a frame is mapped for it.  The port is b3_piece_inertia() in
# src/burnout3_panels.c, reached through build/dump_traj --pieceseed.
# ===========================================================================
PS_ENTRY = 0x00109BB0


def ps_emulate(mass, bbmax, bbmin):
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UcError
    from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP,
                                   UC_X86_REG_EAX, UC_X86_REG_EBX,
                                   UC_X86_REG_ECX, UC_X86_REG_EDX)
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, ev.ELF)
    uc.mem_map(WC_STACK, 0x10000, UC_PROT_ALL)
    uc.mem_map(WC_BODY, 0x1000, UC_PROT_ALL)
    uc.mem_map(WC_FRAME, 0x1000, UC_PROT_ALL)
    uc.mem_map(WC_MAGIC & ~0xFFF, 0x1000, UC_PROT_ALL)

    img = bytearray(0x1000)
    _wc_v4(img, 0x1D0, list(bbmax) + [0.0])
    _wc_v4(img, 0x1E0, list(bbmin) + [0.0])
    struct.pack_into('<f', img, 0x1F0, float(mass))
    struct.pack_into('<I', img, 0x204, WC_FRAME)
    img[0x215] = 7
    uc.mem_write(WC_BODY, bytes(img))

    fr = bytearray(0x1000)
    for r in range(4):
        row = [0.0, 0.0, 0.0, 0.0]
        if r < 3:
            row[r] = 1.0
        _wc_v4(fr, 0x10 * r, row)
    uc.mem_write(WC_FRAME, bytes(fr))

    sp = WC_STACK + 0x8000
    uc.mem_write(sp, struct.pack('<I', WC_MAGIC))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EAX, WC_BODY)
    for r in (UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX):
        uc.reg_write(r, WC_BODY)
    try:
        uc.emu_start(PS_ENTRY, WC_MAGIC, count=4_000_000)
    except UcError as e:
        raise RuntimeError("emulation faulted: %s @ 0x%08X"
                           % (e, uc.reg_read(UC_X86_REG_EIP)))
    after = bytes(uc.mem_read(WC_BODY, 0x1000))
    g = lambda off: struct.unpack_from('<f', after, off)[0]
    return [g(0x10), g(0x24), g(0x38)]


def ps_port(mass, bbmax, bbmin, axis):
    import json
    import os
    import subprocess
    import tempfile
    vals = [mass] + list(bbmax) + list(bbmin) + [float(axis)]
    fd, path = tempfile.mkstemp(suffix=".ps")
    try:
        with os.fdopen(fd, 'w') as f:
            f.write(" ".join(repr(float(x)) for x in vals))
        out = subprocess.run(["build/dump_traj", "--pieceseed", path],
                             capture_output=True, text=True, check=True)
    finally:
        os.unlink(path)
    return json.loads(out.stdout)


# mass 260.0 is the retail seed (FUN_001069C0 tail, 0x43820000); the boxes
# are the real .bgv+0xEA0 records of COMP/Car1, SUPR/Car1 and HEVY/Car1 plus
# the two synthetic extremes.  The last entry is the detached-WHEEL box
# FUN_001069C0's mode-0 arm builds: (0.1, r, r) / (-0.1, -r, -r).
PS_CASES = [
    ("COMP Car1 right door",  260.0, (0.175685, 1.050457, 0.036119),
                                     (-0.278408, -0.010155, -1.353693), 1),
    ("COMP Car1 front",       260.0, (0.357424, 0.087448, 0.313610),
                                     (-1.437424, -0.350515, -0.307115), 2),
    ("COMP Car1 bonnet",      260.0, (0.728289, 0.046761, 0.952716),
                                     (-0.728289, -0.399787, -0.069304), 0),
    ("SUPR Car1 boot",        260.0, (0.826000, 0.000000, 0.000000),
                                     (-0.826000, -0.366000, -1.515000), 0),
    ("HEVY Car1 rear",        260.0, (0.329000, 0.049000, 0.639000),
                                     (-1.729000, -0.357000, -0.148000), 2),
    ("asymmetric, min wins",  260.0, (0.05, 0.05, 0.05),
                                     (-2.0, -1.5, -0.25), 1),
    ("thin plate",            260.0, (0.9, 0.01, 1.4),
                                     (-0.9, -0.01, -1.4), 0),
    ("wheel disc r=0.34",     260.0, (0.1, 0.34, 0.34),
                                     (-0.1, -0.34, -0.34), 0),
    ("light piece m=20",       20.0, (0.4, 0.25, 0.7),
                                     (-0.4, -0.25, -0.7), 1),
    ("unit cube m=1",           1.0, (0.5, 0.5, 0.5),
                                     (-0.5, -0.5, -0.5), 2),
]


def run_pieceseed_cases():
    fails = 0
    print("\nflying-part inertia seed (FUN_00109BB0 -> FUN_00109190), "
          "real x86 vs the C port:")
    for name, mass, bbmax, bbmin, axis in PS_CASES:
        try:
            emu = ps_emulate(mass, bbmax, bbmin)
            port = ps_port(mass, bbmax, bbmin, axis)
        except Exception as e:
            print("  %-32s %s" % (name, e))
            fails += 1
            continue
        bad = []
        for i in range(3):
            pv, evv = port['diag'][i], emu[i]
            if abs(pv - evv) > max(1e-9, abs(evv) * 5e-6):
                bad.append(("diag[%d]" % i, pv, evv))
        # the recentring is the port's own (FUN_001069C0 + FUN_00106F20
        # @0x00107217); check it stays a box and the axis rule fired
        ctr = [(bbmin[i] + bbmax[i]) * 0.5 for i in range(3)]
        if axis == 0:
            ctr[1] = 0.0
        elif axis == 1:
            ctr[0] = 0.0
        elif axis == 2:
            ctr[1] = 0.0
            ctr[2] = 0.0
        for i in range(3):
            for key, ref in (('bbmax', bbmax), ('bbmin', bbmin)):
                want = ref[i] - ctr[i]
                if abs(port[key][i] - want) > 1e-5:
                    bad.append(("%s[%d]" % (key, i), port[key][i], want))
        if bad:
            fails += 1
            print("  %-32s FAIL" % name)
            for tag, pv, evv in bad[:8]:
                print("      %-12s port %r  emu %r" % (tag, pv, evv))
        else:
            print("  %-32s OK   Iinv=(%.6f %.6f %.6f)"
                  % (name, emu[0], emu[1], emu[2]))
    return fails


def run_world_contact_cases():
    fails = 0
    print("\nbody-vs-world contact resolve (FUN_00109EA0), "
          "real x86 vs the C port:")
    for name, kw in WC_CASES:
        st = wc_state(**kw)
        try:
            emu = wc_emulate(st)
            port = wc_port(st)
        except Exception as e:
            print("  %-32s %s" % (name, e))
            fails += 1
            continue
        bad = []

        def cmpf(tag, pv, ev_, tol=1e-4):
            if abs(pv - ev_) > max(tol, abs(ev_) * 2e-5):
                bad.append((tag, pv, ev_))

        for key in ('vel', 'omega', 'angmom', 'force', 'torque',
                    'impf', 'impt', 'defl', 'pushout'):
            for i in range(4):
                cmpf("%s[%d]" % (key, i), port[key][i], emu[key][i],
                     1e-3 if key in ('force', 'torque', 'impf', 'impt')
                     else 1e-5)
        cmpf("impact", port['impact'], emu['impact'], 1e-2)
        for key in ('valid', 'sleep', 'impulsed', 'hit'):
            if int(port[key]) != int(emu[key]):
                bad.append((key, port[key], emu[key]))
        if bad:
            fails += 1
            print("  %-32s FAIL" % name)
            for tag, pv, ev_ in bad[:8]:
                print("      %-12s port %r  emu %r" % (tag, pv, ev_))
        else:
            print("  %-32s OK   |v|=%.3f speed=%.3f |L|=%.1f j=%.1f%s"
                  % (name,
                     math.sqrt(sum(x * x for x in emu['vel'][:3])),
                     emu['vel'][3],
                     math.sqrt(sum(x * x for x in emu['angmom'][:3])),
                     emu['impact'],
                     "  sleep" if emu['sleep'] else ""))
    return fails


# ===========================================================================
# THE SUBSTEP RELOCATION -- FUN_0011AEF0 AT ITS OWN CALL SITE (0x0011C0B7)
#
# (see docs/PHYSICS_GLUE_LEDGER.md section E, gap B4)
#
# Retail's substep, read straight out of FUN_0011BE50:
#
#   0011c0a0  push esi; push ebx; call 0x11d460    tyre force pass
#   0011c0a9  mov byte [ebx+0x212], 0
#   0011c0b0  mov byte [ebx+0x213], 0
#   0011c0b7  call 0x11aef0                        CHASSIS CONTACT RESOLVE
#   0011c0bc  test eax,eax
#   0011c0be  je 0x11c0d3
#   0011c0c0    mov [ebx+0x1524], 3                forced drift state
#   0011c0ca    mov byte [ebx+0x212], 1
#   0011c0d3  cmp [ebx+0x1524], 3 / mov 0          release the state-3 latch
#   0011c0e6  push ebx; call 0x1239c0              suspension pre-pass
#   0011c0ec  push esi; push ebx; call 0x123fd0    suspension force pass
#   0011c0f3  ...                                  the near-stop zeroing
#   0011c15d  push esi; mov ecx,ebx; call 0x109560 INTEGRATE
#   0011c165  dec edi; jne 0x11c0a0
#
# and the soup FUN_0011AEF0 reads (veh+0x200) is frozen ONCE per frame by
# FUN_0011BC60 @0x0011BF43, outside this loop.
#
# The oracle below does not mirror any of that in Python: it seeds the vehicle
# through tools/emulate_pipeline.py's real init chain, runs the real
# FUN_0011ECF0, and then executes **the retail instruction stream from
# 0x0011C0A0 to 0x0011C16C with EDI = 2** -- both substeps, every call
# (FUN_0011D460, FUN_0011AEF0, FUN_001239C0, FUN_00123FD0, FUN_00109560) at
# its own address, in its own order, with a real polygon soup in veh+0x200.
# Nothing is patched except `FUN_0010DCA0`, the crash-ENTRY state machine the
# trigger tail calls @0x0011B9F3 (`ret 0xC`, verified at 0x0010DD0F): that is
# game state, not body state, and the port models the decision as a flag.  A
# hook counts every entry so the two sides can be compared on it.
#
# The other side is the real C: `build/dump_traj --state <s> ... --soup <f>`
# runs b3_vehicle_step_full with `chassis_resolve` installed.
# ===========================================================================
RELOC_SUB_ENTRY = 0x0011C0A0     # push esi; push ebx; call FUN_0011D460
RELOC_SUB_EXIT = 0x0011C16C      # the instruction after `dec edi; jne`
F_0010DCA0 = 0x0010DCA0          # crash-entry state machine (callee-pop, 12)


def _make_reloc_pipeline(ep):
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EAX,
                                   UC_X86_REG_EBX, UC_X86_REG_ECX,
                                   UC_X86_REG_ESI, UC_X86_REG_EDI)

    class RelocPipeline(ep.Pipeline):
        """ep.Pipeline with the substep loop executed as RETAIL CODE, so
        FUN_0011AEF0 runs where FUN_0011BE50 actually calls it."""

        def __init__(self, *a, **kw):
            super().__init__(*a, **kw)
            self.soup = []
            self.soup_count = 2          # _seed_objects' flat ground plane
            self.crash_fired = 0
            # FUN_0010DCA0 -> `ret 0xC` + an entry counter (see the header)
            self.uc.mem_write(F_0010DCA0, b'\xc2\x0c\x00')
            self.uc.hook_add(UC_HOOK_CODE, self._on_dca0,
                             begin=F_0010DCA0, end=F_0010DCA0)

        def _on_dca0(self, uc, address, size, user):
            self.crash_fired += 1

        def set_soup(self, tris):
            """tris = [((p0, p1, p2), normal, surface_u16), ...], GAME space,
            written into the veh+0x200 record FUN_0011AEF0 walks."""
            self.soup = list(tris)
            self.soup_count = len(self.soup)
            for t, (verts, n, surf) in enumerate(self.soup):
                b = ep.SOUP_REC + 0x40 * t
                for j, q in enumerate(verts):
                    for k in range(3):
                        self.wf(b + 0x10 * j + 4 * k, q[k])
                    self.wf(b + 0x10 * j + 12, 0.0)
                for k in range(3):
                    self.wf(b + 0x30 + 4 * k, n[k])
                self.wf(b + 0x3C, 0.0)
                self.uc.mem_write(ep.SOUP_TYPE + 2 * t,
                                  struct.pack('<H', surf))
            self.wu(ep.SOUP_HDR + 0, self.soup_count)

        def frame(self, throttle=0.0, brake=0.0, steer=0.0, boost=0,
                  run_skipped=False):
            V = self
            base = ep.VEHICLE
            dt = self.dt
            self.clock = struct.unpack(
                '<f', struct.pack('<f', self.clock + dt))[0]
            self.wf(ep.G_DT, dt)
            self.wf(ep.G_CLOCK, self.clock)
            self.wf(ep.RACECAR + 0x10DC, self.clock)
            # FUN_00104840 mirror: the per-frame contact scratch zeroing
            V.wb(base + 0x212, 0)
            self.uc.mem_write(base + 0x160, b'\0' * 0x40)
            # FUN_0011BC60 @0x0011BF43: freeze veh+0x200 once per frame
            V.wu(ep.SOUP_HDR + 0, self.soup_count)
            # FUN_00104D30 driver glue
            V.wf(base + 0x1414, throttle)
            V.wf(base + 0x1404, brake)
            V.wf(base + 0x1408, steer)
            V.wb(base + 0x13FC, 4 if boost else 0)
            if V.vf(0xBC) < 0.1:
                V.wf(base + 0x1408, 0.0)
            V.wf(base + 0x1438, V.vf(0x1438))
            th = throttle * V.vf(0x13BC)
            V.wf(base + 0x1400, min(th, 1.0))
            self.call(ep.F_ECF0, regs={UC_X86_REG_ECX: ep.VEHICLE,
                                       UC_X86_REG_EBX: 0, UC_X86_REG_EDI: 0,
                                       UC_X86_REG_ESI: 0})
            snaps = [bytes(self.uc.mem_read(base + 0x820 + 0xC0 * i, 0x20))
                     for i in range(4)]
            # ---- the retail substep loop, both substeps, verbatim --------
            uc = self.uc
            sp = ep.STACK_BASE + ep.STACK_SIZE - 0x2000
            uc.mem_write(sp - 0x1800, b'\0' * 0x2000)
            uc.reg_write(UC_X86_REG_ESP, sp)
            uc.reg_write(UC_X86_REG_EBX, ep.VEHICLE)
            uc.reg_write(UC_X86_REG_ESI, ep.f2u(dt * 0.5))
            uc.reg_write(UC_X86_REG_EDI, 2)
            uc.reg_write(UC_X86_REG_EAX, 0)
            uc.emu_start(RELOC_SUB_ENTRY, RELOC_SUB_EXIT, count=60_000_000)
            # BE50 substep tail (0x0011C190): prev-frame wheel records
            for i in range(4):
                self.uc.mem_write(base + 0x850 + 0xC0 * i, snaps[i])
            t = V.vf(0x152C)
            if t >= -0.0001:
                V.wf(base + 0x152C, t - dt)
            self.call(ep.F_C720, regs={UC_X86_REG_EAX: ep.VEHICLE})
            return self.capture()

        def capture(self):
            cap = super().capture()
            cap['c212'] = self.vb(0x212)
            cap['cstate'] = struct.unpack(
                '<i', struct.pack('<I', self.vu(0x198)))[0]
            cap['impact'] = self.vf(0x194)
            cap['cfire'] = self.crash_fired
            return cap

    return RelocPipeline


def _tri_plane(px, pz, nx, nz, half=40.0, ylo=-6.0, yhi=6.0, surf=0x0015):
    """Two triangles spanning a VERTICAL plane through (px, pz) whose unit
    in-plane normal is (nx, 0, nz).  |n.y| == 0 puts it on FUN_0011AC30's
    wall side (`ground = n.y > 0.7`, [0x003B17D8])."""
    tx, tz = -nz, nx
    a = (px + tx * half, ylo, pz + tz * half)
    b = (px - tx * half, ylo, pz - tz * half)
    c = (px - tx * half, yhi, pz - tz * half)
    d = (px + tx * half, yhi, pz + tz * half)
    n = (nx, 0.0, nz)
    return [((a, b, c), n, surf), ((a, c, d), n, surf)]


# Surface 0, exactly as emulate_pipeline._seed_objects seeds it: the
# accumulator's surface slot takes the LOWEST NONZERO low byte, so a ground
# plane with a real surface id would out-rank the wall's and hide the
# chevron (0x20) refusal at 0x0011B944.
def _ground_plane(y=0.0, surf=0x0000, s=5000.0):
    n = (0.0, 1.0, 0.0)
    return [(((-s, y, -s), (-s, y, s), (s, y, -s)), n, surf),
            (((s, y, s), (s, y, -s), (-s, y, s)), n, surf)]


# name -> (scenario, checkpoint frame, window frames, inputs, soup builder
#          [, tolerance overrides]).  The builder gets the car's world
# position at the checkpoint.
#
# TOLERANCES.  Every case asserts the DISCRETE state frame by frame with no
# slack at all -- the resolved-contact byte veh+0x212, the contact state
# veh+0x198, the forced drift state veh+0x1524, the gear, and whether the
# crash trigger fired -- because those are what the relocation is about.  The
# continuous bands are the same idea as the adversarial trajectories: a
# 40-frame sustained wall grind at 26-31 m/s is a genuine floating-point
# amplifier (the impulse re-solves from a re-derived contact centroid every
# substep), so the cases that hold a contact for the whole window carry the
# wider band and the HEADING is the tight claim.
_RELOC_TOL_HARD = dict(pos=2e-2, vel=1e-1, omega=1e-1, head=1e-3)
RELOC_CASES = {
    # The two controls.  The soup is also the suspension's world (the pre-pass
    # rays walk the same set), so "no world" has to mean "ground, no wall":
    # emptying it would drop the car through the floor on the emulator side
    # while the C driver's own ground probe kept answering y = 0.
    'flat ground only':
        ('accelerate', 240, 40, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)),
    'distant wall (never touched)':
        ('accelerate', 240, 40, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0] + 50.0, p[2], -1.0, 0.0)),
    'right wall, throttle':
        ('accelerate', 240, 40, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0] + 0.90, p[2], -1.0, 0.0)),
    'left wall, throttle':
        ('accelerate', 240, 40, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0] - 0.90, p[2], 1.0, 0.0)),
    'shallow graze (right)':
        ('accelerate', 240, 40, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0] + 1.00, p[2], -1.0, 0.0)),
    'head-on wall (under the gate)':
        ('accelerate', 240, 40, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0], p[2] + 1.6, 0.0, -1.0),
         _RELOC_TOL_HARD),
    'oblique wall 30 deg':
        ('accelerate', 240, 40, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0] + 0.85, p[2], -0.866, -0.5)),
    'two walls (inside corner)':
        ('accelerate', 240, 40, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0] + 0.90, p[2], -1.0, 0.0)
                   + _tri_plane(p[0], p[2] + 2.4, 0.0, -1.0),
         _RELOC_TOL_HARD),
    'wall while braking':
        ('brake', 250, 40, (0.0, 1.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0] + 0.90, p[2], -1.0, 0.0)),
    'wall mid-corner (separating)':
        ('corner', 300, 40, (0.6, 0.0, 0.5, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0] + 0.90, p[2], -1.0, 0.0)),
    'corner into the outside wall':
        ('corner', 300, 40, (0.6, 0.0, 0.5, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0] - 1.40, p[2], 1.0, 0.0),
         _RELOC_TOL_HARD),
    # 25 frames, not 40: a 1.66 m penetration at 31 m/s brings the car to a
    # near standstill against the wall by frame 27, and a stalled chassis
    # being re-solved every substep at ~0.1 m/s is the one genuine
    # floating-point amplifier in this set (positions still agree to 4e-3,
    # but the residual velocity splits).  The window ends before it.
    'deep head-on wall':
        ('accelerate', 299, 25, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0], p[2] + 0.4, 0.0, -1.0),
         _RELOC_TOL_HARD),
    # A car inside its out-of-control window carries veh+0x1534 = 0.05
    # (b3_td_crash_authority), which drops the wall-crash bars to 1.375 m/s
    # of dv and 0.0354 of head-on -- so this one FIRES and FUN_0011AEF0
    # reaches its FUN_0010DCA0 tail @0x0011B9F3.  At authority 1.0 the same
    # hit does not (dv tops out near 26 against a 27.5 bar), which is the
    # case above.
    'slammed car, wall crash fires':
        ('accelerate', 299, 25, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0], p[2] + 0.4, 0.0, -1.0),
         dict(_RELOC_TOL_HARD, authority=0.05)),
    # ...and the same hit on a chevron board (surface low byte 0x20) must
    # NOT fire: `cmp byte [edi+0x190], 0x20; je` @0x0011B944.
    'slammed car, chevron 0x20 refuses':
        ('accelerate', 299, 25, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0], p[2] + 0.4, 0.0, -1.0, surf=0x0020),
         dict(_RELOC_TOL_HARD, authority=0.05)),
    # CRASH-AUDIT: ...and the same hit with the CRASH-ENTRY VETO raised must
    # not fire either.  veh+0x1353 bit 3 is what FUN_00105BD0 @0x00105F95
    # (`OR byte [ESI+0x1353],0x18`) sets when the view-distance ladder's
    # crash_ok byte is 0, and FUN_0011AEF0 reads it @0x0011B94D
    # (`TEST byte [EDI+0x1353],8`) to refuse the crash while still resolving
    # the contact.  It is the one gate the substep arm was missing, and it
    # is separately observable from the chevron veto: the surface stays
    # ordinary, so only the flag can stop it.
    'slammed car, +0x1353 bit 3 refuses':
        ('accelerate', 299, 25, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0], p[2] + 0.4, 0.0, -1.0),
         dict(_RELOC_TOL_HARD, authority=0.05, flags1353=0x08)),
    # bits 0|2 are the OTHER half of the same byte: FUN_0011AEF0 bails at its
    # HEAD on `test byte [edi+0x1353],5` @0x0011AF0C, so the car takes no
    # contact at all -- +0x198 stays 0 and it drives straight through.
    'wall with +0x1353 bit 0 (resolve disabled)':
        ('accelerate', 299, 25, (1.0, 0.0, 0.0, 0),
         lambda p: _ground_plane(0.0)
                   + _tri_plane(p[0], p[2] + 0.4, 0.0, -1.0),
         dict(_RELOC_TOL_HARD, authority=0.05, flags1353=0x01)),
}


def run_relocation_cases():
    import json
    import os
    import subprocess

    _pspec = importlib.util.spec_from_file_location(
        "ep", "tools/emulate_pipeline.py")
    ep = importlib.util.module_from_spec(_pspec)
    _pspec.loader.exec_module(ep)
    RelocPipeline = _make_reloc_pipeline(ep)

    exe = 'build/dump_traj'
    r = subprocess.run(['cc', '-O2', '-Isrc', '-o', exe, 'tools/dump_traj.c',
                        'src/burnout3_vehicle_sim.c',
                            'src/burnout3_panels.c', '-lm'])
    if r.returncode != 0:
        print("\nsubstep relocation: cannot build %s" % exe)
        return len(RELOC_CASES)

    fails = 0
    print("\nFUN_0011AEF0 IN the substep (@0x0011C0B7) -- the retail "
          "instruction stream 0x0011C0A0..0x0011C16C vs b3_vehicle_step_full:")
    for name, spec in RELOC_CASES.items():
        scen, ckpt, wlen, inp, mk = spec[:5]
        tol = dict(pos=2e-3, vel=1e-2, omega=2e-2, head=1e-4, authority=1.0,
                   flags1353=0)
        if len(spec) > 5:
            tol.update(spec[5])
        try:
            p = RelocPipeline()
            inputs = ep.scenario_inputs(scen)
            for i in range(ckpt):
                p.frame(*inputs[i])
            p.wf(ep.VEHICLE + 0x1534, tol['authority'])
            # veh+0x1353 persists on BOTH sides: retail re-arms it in the
            # collision manager (FUN_00110AF0 @0x00110E20/E46/E70/EA2), which
            # is outside the instruction window this harness executes, and
            # b3_vehicle_step_full never writes it either.
            p.wb(ep.VEHICLE + 0x1353, tol['flags1353'])
            pos = [p.rf(ep.CTX0 + 0x30 + 4 * i) for i in range(3)]
            tris = mk(pos)
            p.set_soup(tris)
            sf = 'build/reloc_%s.txt' % name.split()[0].strip('(),')
            p.write_state(sf)
            base_fired = p.crash_fired
            emu = [p.frame(*inp) for _ in range(wlen)]
        except Exception as e:
            print("  %-30s emulation: %s" % (name, e))
            fails += 1
            continue

        soupf = sf.replace('.txt', '_soup.txt')
        with open(soupf, 'w') as f:
            for verts, n, surf in tris:
                f.write(' '.join('%.9g' % x for q in verts for x in q))
                f.write(' %.9g %.9g %.9g %d\n' % (n[0], n[1], n[2], surf))
        cw = subprocess.run([('./' + exe), '--state', sf, str(wlen),
                             str(inp[0]), str(inp[1]), str(inp[2]),
                             str(inp[3]), '--soup', soupf,
                             '--authority', str(tol['authority']),
                             '--flags1353', str(tol['flags1353'])],
                            capture_output=True, text=True)
        port = [json.loads(l) for l in cw.stdout.splitlines()]
        if len(port) != wlen:
            print("  %-30s port produced %d/%d frames: %s"
                  % (name, len(port), wlen, cw.stderr[:120]))
            fails += 1
            continue

        worst = dict(pos=0.0, vel=0.0, omega=0.0, head=0.0)
        bad = []
        for i, (e, m) in enumerate(zip(emu, port)):
            for j in range(3):
                worst['pos'] = max(worst['pos'], abs(e['pos'][j] - m['pos'][j]))
                worst['vel'] = max(worst['vel'], abs(e['vel'][j] - m['vel'][j]))
                worst['omega'] = max(worst['omega'],
                                     abs(e['omega'][j] - m['omega'][j]))
            worst['head'] = max(worst['head'],
                                abs(_heading(e) - _heading(m)))
            if e['drift'] != m['drift']:
                bad.append((i, 'drift', e['drift'], m['drift']))
            if e['c212'] != m['c212']:
                bad.append((i, 'c212', e['c212'], m['c212']))
            if e['cstate'] != m['cstate']:
                bad.append((i, 'cstate', e['cstate'], m['cstate']))
            if e['gear'] != m['gear']:
                bad.append((i, 'gear', e['gear'], m['gear']))
            ef = 1 if (e['cfire'] - base_fired) > 0 else 0
            if ef != (1 if m['cfire'] else 0):
                bad.append((i, 'crashfire', ef, m['cfire']))
        ok = (worst['pos'] < tol['pos'] and worst['vel'] < tol['vel']
              and worst['omega'] < tol['omega']
              and worst['head'] < tol['head'] and not bad)
        fails += not ok
        nwall = sum(1 for e in emu if e['cstate'] == 1)
        print("  %-30s polys %2d  wall-frames %2d/%d  fire %d | pos %.1e "
              "vel %.1e omega %.1e head %.1e  %s"
              % (name, len(tris), nwall, wlen,
                 emu[-1]['cfire'] - base_fired,
                 worst['pos'], worst['vel'], worst['omega'], worst['head'],
                 "OK" if ok else "FAIL"))
        for b in bad[:4]:
            print("      frame %d %s: emu %r port %r" % b)
    return fails


def main():
    fails = 0
    print("%-38s %-13s %-13s %s" % ("case", "model_z", "emulated_z", "result"))
    for rc, v, k, q, d in CASES:
        try:
            ax, ay, az, aw = emulate(rc, v, k, q, d)
        except RuntimeError as e:
            print("  %-36s %s" % ("rc=%.2f v=%.1f" % (rc, v), e))
            fails += 1
            continue
        mx, my, mz, mw = model_resistance(rc, v, k, q, d)
        # GAP CLOSED (was: unexplained -20.0*dir.x on X). These cases run
        # FUN_0011D460's airborne path, whose damper table adds
        # -2*10*vel.x on X (right-axis records) and -2*1000*vel.y on Y
        # (up-axis records) -- see the tyre-grip section / d460_model. With
        # that modelled, all four accumulator components are asserted.
        mx = mx + (-20.0) * d[0]
        ok = (abs(mx - ax) < max(TOL, abs(mx) * 1e-5)
              and abs(mz - az) < TOL
              and abs(mw - aw) < max(TOL, abs(mw) * 1e-6))
        # The vertical accumulator also carries gravity + the up-axis damper.
        gy = model_vertical(0.0, v, 1000.0) + my + (-2000.0) * d[1]
        ok_y = abs(gy - ay) < 1e-2
        print("rc=%.2f v=%-5.1f k=%.2f q=%.1f d=%-16s %-13.4f %-13.4f %s%s"
              % (rc, v, k, q, str(d), mz, az,
                 "OK" if ok else "FAIL",
                 "" if ok_y else "  (y: model %.2f vs emu %.2f)" % (gy, ay)))
        fails += (not ok) or (not ok_y)

    # Vertical force: gravity + speed-dependent downforce.
    print("\nvertical force (downforce + gravity):")
    vfail = 0
    for coef, v, mass in ((0.0, 40, 1000.), (1.0, 40, 1000.), (2.0, 25, 1500.),
                          (7.5, 40, 1000.), (7.5, 25, 1500.)):
        ax, ay, az, aw = emulate(0.5, v, 0.0, 0.0, (0., 0., 1.), mass, coef)
        pred = model_vertical(coef, v, mass)
        ok = abs(pred - ay) < max(0.05, abs(pred) * 1e-6)
        vfail += not ok
        print("  c=%-4.1f v=%-5.1f m=%-6.0f model %-14.4f emu %-14.4f %s"
              % (coef, v, mass, pred, ay, "OK" if ok else "FAIL"))
    fails += vfail

    efails = run_engine_cases()
    wfails = run_wheel_cases()
    gfails = run_engage_cases()
    sfails = run_susp_cases()
    pfails = run_prepass_case()
    tfails = run_tyre_cases()
    ifails = run_integ_cases()
    stfails = run_steer_cases()
    safails = run_steer_away_cases()
    plfails = run_pipeline_cases()
    advfails = run_adversarial_cases()
    wcfails = run_world_contact_cases()
    c7fails = run_class7_cases()
    psfails = run_pieceseed_cases()
    rlfails = run_relocation_cases()
    fails += (efails + wfails + gfails + sfails + pfails + tfails + ifails
              + stfails + safails + plfails + advfails + wcfails + c7fails
              + rlfails + psfails)

    total = (len(CASES) + 5 + len(ENGINE_CASES) + len(WHEEL_CASES)
             + len(ENGAGE_CASES) + len(SUSP_CASES) + 1
             + len(TYRE_CASES) + 1 + len(INTEG_CASES) + len(STEER_CASES)
             + len(STEER_AWAY_CASES) + len(PIPELINE_WINDOWS)
             + len(ADVERSARIAL) + len(WC_CASES) + len(C7_CASES)
             + len(RELOC_CASES) + len(PS_CASES))
    print("\n%d/%d cases match the real code "
          "(%d resistance, 5 vertical, %d engine/transmission, %d wheel, "
          "%d engage, %d suspension, 1 pre-pass, %d tyre/airborne (+1 LSDM), "
          "%d integrator, %d steering, %d steer-away, "
          "%d full-pipeline trajectories, %d adversarial trajectories, "
          "%d world-contact resolves, %d class-7 updates, "
          "%d in-substep chassis-contact trajectories, "
          "%d flying-part inertia seeds)"
          % (total - fails, total, len(CASES), len(ENGINE_CASES),
             len(WHEEL_CASES), len(ENGAGE_CASES), len(SUSP_CASES),
             len(TYRE_CASES), len(INTEG_CASES), len(STEER_CASES),
             len(STEER_AWAY_CASES), len(PIPELINE_WINDOWS), len(ADVERSARIAL),
             len(WC_CASES), len(C7_CASES), len(RELOC_CASES),
             len(PS_CASES)))
    print("The former KNOWN GAP (-20.0*dir.x on X) is CLOSED: it was the "
          "airborne damper table in FUN_0011D460 (see RE_NOTES section 11).")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
