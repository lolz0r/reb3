#!/usr/bin/env python3
"""Differential validation of the recovered GAMEPLAY rules (scoring, boost
meter, takedown bookkeeping, out-of-control reaction) against the real x86
executed under Unicorn.

Pattern-copied from tools/validate_port.py (which stays untouched): every rule
integrated into the harness gets a case here that
  1. seeds the real function's environment (score object / boost record /
     racecar object / globals),
  2. runs the actual instructions from build/burnout3.elf,
  3. asserts a pure-Python mirror of the recovered rule produces the same
     post-state.

Function map (all addresses = analyzed burnout3.elf, .text = flat + 0x10000):
  0x0017A530  boost award        meter += units*(mult_base+mult_bonus), clamp
  0x0017A480  boost update       drain rate*dt, stop/peg rules  (head; tail=FX)
  0x0017A5B0  boost engage       min-units + recovery-time gate
  0x00192D20  category tracker   value store + tier threshold scan
  0x00196940  air award (landing path)  CategoryBP[tier] -> BP accumulators
  0x00198E60  takedown commit    dedup, counters, revenge flags, BP chain
  0x0011ECF0  steer-away         +0x137C response envelope after a slam
  0x00105340  out-of-control     AI steering authority 0.05/0.1 inside window

Evidence notes: docs/RE_GAMEPLAY.md.  Run from the repo root:
  python3 tools/validate_gameplay.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emulate_vehicle as ev                        # noqa: E402
import validate_port as vp                          # noqa: E402  (import only)
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_XMM0)     # noqa: E402


def f2u(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def u2f(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]


DT = 1.0 / 60.0

# ---------------------------------------------------------------------------
# Globals (all verified in docs/RE_GAMEPLAY.md).
# BSS (zero until seeded):
DT_GLOBAL = 0x0060EA1C       # frame dt, same one the drivetrain uses
DT2_GLOBAL = 0x004AE1FC      # second per-frame dt used by the score module FX
CLOCK_GLOBAL = 0x0060EA20    # race clock global
GAME_CTX = 0x004D5370        # game-context ptr; FUN_00017310 reads it as
                             # [0x4A71A0 + 0x2E1D0] (MOV EAX,0x4A71A0 at the
                             # award call sites, e.g. 0x00196A1C)
# .data compiled defaults (registrars FUN_0017A0F0 "Boost Bar" and
# FUN_00190430 "Score/..."); pre-VDB values, which is what pre-init
# emulation sees:
MIN_BOOST_TIME = 1.0         # 0x003F72DC  (VDB tunes it to 0.5)
MIN_RECOVERY = 0.5           # 0x003F72E0  (VDB 0.1)
BAR3 = 720.0                 # 0x003F72F0 = "Boost Bar sizes"[3]
AIR_CAT_BP = [100, 250, 500, 1000]   # 0x003F7490 (VDB {0,10,50,100})

A530 = 0x0017A530
A480 = 0x0017A480
A5B0 = 0x0017A5B0
TIER = 0x00192D20
AIR = 0x00196940
TDCOMMIT = 0x00198E60
ECF0 = 0x0011ECF0
OOC = 0x00105340

# Boost record layout (score+0xCC == racecar+0x119C; FUN_0017A3C0 is the
# constructor that proves every offset):
#   +0x00 self ptr   +0x24 start clock  +0x28 end clock  +0x30 bar tier
#   +0x34 bar size   +0x38 meter        +0x3C total earned
#   +0x40 drain rate +0x44 min units    +0x48 mult base  +0x4C mult bonus
#   +0x50/+0x51 peg-full flags  +0x52 boosting  +0x53 fixed-duration burn
#   +0x55 ramp-done (racecar+0x11F1)  +0x56 set when +0x55 while boosting


def build_record(base, meter=0.0, barsize=240.0, rate=36.0, min_units=18.0,
                 mult_base=1.0, mult_bonus=0.0, start=-1.0, end=-1.0,
                 boosting=0, fixed=0, peg50=0, peg51=0, ramp55=0, tier=0):
    buf = bytearray(0x60)
    struct.pack_into('<I', buf, 0x00, base)          # self pointer
    struct.pack_into('<f', buf, 0x24, start)
    struct.pack_into('<f', buf, 0x28, end)
    struct.pack_into('<i', buf, 0x30, tier)
    struct.pack_into('<f', buf, 0x34, barsize)
    struct.pack_into('<f', buf, 0x38, meter)
    struct.pack_into('<f', buf, 0x40, rate)
    struct.pack_into('<f', buf, 0x44, min_units)
    struct.pack_into('<f', buf, 0x48, mult_base)
    struct.pack_into('<f', buf, 0x4C, mult_bonus)
    buf[0x50] = peg50
    buf[0x51] = peg51
    buf[0x52] = boosting
    buf[0x53] = fixed
    buf[0x55] = ramp55
    return bytes(buf)


def rec_field(blob, off, kind='f'):
    if kind == 'f':
        return struct.unpack_from('<f', blob, off)[0]
    if kind == 'i':
        return struct.unpack_from('<i', blob, off)[0]
    return blob[off]


# Fake game-context graph so the unconditional virtual calls in the score
# module resolve:  [GAME_CTX] -> G, [G+0x1B8] -> O, [O] -> VT,
#   VT+0x40  -> return 1   (per-player award mask; bit0 = this player)
#   VT+0x90  -> return 0   (game mode, != 6)
#   VT+0x94  -> return 0   (sub-mode, != 3/4/5 -> FUN_00017310 says "normal")
#   VT+0xAC  -> FLD1; RET  (global boost-earn scale = 1.0)
#   VT+0x5C  -> return 0   (takedown HUD hook)
def fake_ctx():
    G = ev.SCRATCH + 0x7000
    O = ev.SCRATCH + 0x7100
    VT = ev.SCRATCH + 0x7200
    ST_R1 = ev.SCRATCH + 0x7300      # MOV EAX,1 ; RET
    ST_R0 = ev.SCRATCH + 0x7310      # XOR EAX,EAX ; RET
    ST_F1 = ev.SCRATCH + 0x7320      # FLD1 ; RET
    ST_R0_8 = ev.SCRATCH + 0x7330    # XOR EAX,EAX ; RET 8  (+0x5C pushes 2)
    mw = {
        GAME_CTX: struct.pack('<I', G),
        G + 0x1B8: struct.pack('<I', O),
        O: struct.pack('<I', VT),
        ST_R1: b"\xb8\x01\x00\x00\x00\xc3",
        ST_R0: b"\x31\xc0\xc3",
        ST_F1: b"\xd9\xe8\xc3",
        ST_R0_8: b"\x31\xc0\xc2\x08\x00",
    }
    vt = {0x40: ST_R1, 0x90: ST_R0, 0x94: ST_R0, 0xAC: ST_F1, 0x5C: ST_R0_8,
          0x00: ST_R0, 0x04: ST_R0, 0x08: ST_R0, 0x0C: ST_R0}
    for off, tgt in vt.items():
        mw[VT + off] = struct.pack('<I', tgt)
    return mw


# ===========================================================================
# 1. Boost award -- FUN_0017A530 (ESI = boost record, stack arg = base units).
#    Rule: if not in crash/special mode:
#      amount = units * (mult_base + mult_bonus) [* global scale]
#      earned += amount;  meter = min(meter + amount, bar size)
# ===========================================================================
def model_award(meter, barsize, earned, mult_base, mult_bonus, units):
    amt = units * (mult_base + mult_bonus)
    earned += amt
    meter += amt
    if meter > barsize:
        meter = barsize
    return meter, earned


AWARD_CASES = [
    # name, meter, barsize, mult_base, mult_bonus, units
    ("plain add",            100.0, 240.0, 1.0, 0.0, 15.0),
    ("earning multiplier",   100.0, 240.0, 1.5, 0.5, 15.0),
    ("clamp at bar size",    230.0, 240.0, 1.0, 0.0, 20.0),
    ("takedown-sized award",  10.0, 360.0, 1.0, 0.5, 360.0),
]


def run_award_cases():
    fails = 0
    print("boost award (FUN_0017A530):")
    for name, meter, bar, mb, mx, units in AWARD_CASES:
        img = build_record(ev.VEHICLE, meter=meter, barsize=bar, mult_base=mb,
                           mult_bonus=mx) + b"\0" * (ev.VEHICLE_SZ - 0x60)
        tr, after, err = ev.run(A530, img, stack_args=[f2u(units)],
                                regs={UC_X86_REG_ESI: ev.VEHICLE})
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        m_a = rec_field(after, 0x38)
        e_a = rec_field(after, 0x3C)
        mm, me = model_award(meter, bar, 0.0, mb, mx, units)
        ok = abs(m_a - mm) < 1e-3 and abs(e_a - me) < 1e-3
        print("  %-24s meter %8.2f earned %8.2f  %s"
              % (name, m_a, e_a, "OK" if ok else
                 "FAIL (model %.2f / %.2f)" % (mm, me)))
        fails += not ok
    return fails


# ===========================================================================
# 2. Boost meter update -- FUN_0017A480 (EAX = racecar, ECX = record).
#    Head rule (the meter itself; the tail is camera/FX and is not asserted):
#      if boosting:
#        if peg50|peg51: meter = bar size     else: meter -= rate*dt
#        if fixed == 0:  stop when meter <= 0        (meter := 0)
#        else:           stop when clock >= start + MIN_BOOST_TIME
#                        (meter := 0 only if it went negative)
#        stop == (boosting=0, fixed=0, end=clock)
#        if ramp55: +0x56 := 1
#      else:
#        if peg50|peg51: meter = bar size
# ===========================================================================
def model_update(meter, bar, rate, boosting, fixed, peg, start, clock, dt):
    stopped = False
    if boosting:
        meter = bar if peg else meter - rate * dt
        if not fixed:
            if meter <= 0.0:
                meter = 0.0
                stopped = True
        elif start + MIN_BOOST_TIME <= clock:
            if meter < 0.0:
                meter = 0.0
            stopped = True
    else:
        if peg:
            meter = bar
    return meter, (0 if stopped else boosting), (clock if stopped else -1.0)


UPDATE_CASES = [
    # name, meter, boosting, fixed, peg, start, clock
    ("drain while boosting",  100.0, 1, 0, 0, 5.0, 10.0),
    ("empty -> stop",           0.5, 1, 0, 0, 5.0, 10.0),
    ("peg-full flag",          10.0, 1, 0, 1, 5.0, 10.0),
    ("fixed burn: keeps",      10.0, 1, 1, 0, 9.5, 10.0),
    ("fixed burn: expires",    10.0, 1, 1, 0, 8.5, 10.0),
    ("idle + peg-full",        10.0, 0, 0, 1, -1.0, 10.0),
    ("idle unchanged",         10.0, 0, 0, 0, -1.0, 10.0),
]


def run_update_cases():
    fails = 0
    print("\nboost meter update (FUN_0017A480):")
    REC = 0x1B00                    # record inside the racecar image
    for name, meter, boosting, fixed, peg, start, clock in UPDATE_CASES:
        img = bytearray(ev.VEHICLE_SZ)
        struct.pack_into('<f', img, 0x10DC, clock)      # racecar race clock
        img[0x19BC] = 0                                 # grid slot
        img[REC:REC + 0x60] = build_record(
            ev.VEHICLE + REC, meter=meter, boosting=boosting, fixed=fixed,
            peg50=peg, start=start)
        mw = {DT_GLOBAL: struct.pack('<f', DT),
              DT2_GLOBAL: struct.pack('<f', DT)}
        tr, after, err = ev.run(A480, bytes(img), stack_args=[],
                                regs={UC_X86_REG_EAX: ev.VEHICLE,
                                      UC_X86_REG_EDX: 0,
                                      UC_X86_REG_ECX: ev.VEHICLE + REC},
                                mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        m_a = rec_field(after, REC + 0x38)
        b_a = after[REC + 0x52]
        e_a = rec_field(after, REC + 0x28)
        mm, mb, mend = model_update(meter, 240.0, 36.0, boosting, fixed, peg,
                                    start, clock, DT)
        ok = (abs(m_a - mm) < 1e-3 and b_a == mb
              and (mend < 0 or abs(e_a - mend) < 1e-3))
        print("  %-24s meter %8.3f boosting %d end %6.2f  %s"
              % (name, m_a, b_a, e_a, "OK" if ok else
                 "FAIL (model %.3f / %d / %.2f)" % (mm, mb, mend)))
        fails += not ok
    return fails


# ===========================================================================
# 3. Boost engage -- FUN_0017A5B0 (EAX = physics vehicle, ESI = record).
#    Rule: engage iff  not boosting
#                  and meter >= min units (= MIN_BOOST_TIME * rate at reset)
#                  and (end < 0  or  end + MIN_RECOVERY <= clock)
#    On engage: boosting=1, fixed=0, start=clock (+ FX timer := 2.0).
# ===========================================================================
def model_engage(meter, min_units, end, clock, boosting):
    if boosting:
        return False
    if meter < min_units:
        return False
    if not (end < 0.0 or end + MIN_RECOVERY <= clock):
        return False
    return True


ENGAGE_CASES = [
    # name, meter, end(last stop), clock, boosting, expect_engage
    ("engage from full",     100.0, -1.0, 10.0, 0, True),
    ("below minimum units",   10.0, -1.0, 10.0, 0, False),
    ("recovery gate blocks", 100.0,  9.8, 10.0, 0, False),
    ("recovery elapsed",     100.0,  9.4, 10.0, 0, True),
    ("already boosting",     100.0, -1.0, 10.0, 1, False),
]


def run_engage_cases():
    fails = 0
    print("\nboost engage (FUN_0017A5B0):")
    REC = 0x800
    for name, meter, end, clock, boosting, expect in ENGAGE_CASES:
        # physics vehicle at VEHICLE; +0x13F4 -> SCRATCH racecar (default in
        # build_vehicle); +0x215 char = 0 skips the FX vtable call.
        img = ev.build_vehicle({})
        mw = {ev.SCRATCH + 0x10DC: struct.pack('<f', clock),
              ev.SCRATCH + REC: build_record(ev.SCRATCH + REC, meter=meter,
                                             end=end, boosting=boosting)}
        tr, after, err = ev.run(A5B0, img, stack_args=[],
                                regs={UC_X86_REG_EAX: ev.VEHICLE,
                                      UC_X86_REG_ESI: ev.SCRATCH + REC},
                                mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        # read the record back out of scratch
        blob = tr.uc.mem_read(ev.SCRATCH + REC, 0x60)
        b_a = blob[0x52]
        s_a = struct.unpack_from('<f', blob, 0x24)[0]
        want = model_engage(meter, 18.0, end, clock, boosting)
        ok = (b_a == (1 if want else boosting)) and \
             (not want or abs(s_a - clock) < 1e-4)
        print("  %-24s boosting %d start %6.2f  %s"
              % (name, b_a, s_a, "OK" if ok else
                 "FAIL (model engage=%s)" % want))
        fails += not ok
    return fails


# ===========================================================================
# 4. Category tier tracker -- FUN_00192D20 (EAX = record, XMM0 = new value).
#    Record: +0 value, +4 clock copy, +8 prev value, +C thresholds ptr,
#            +0x11 tier (s8, -1 = none), +0x12 prev tier, +0x13 count,
#            +0x18 clock-source ptr (reads +0xC from it).
#    Rule: store value/prev/clock; if tier < count-1, scan thresholds from the
#    top down and set tier to the highest index whose threshold <= value.
# ===========================================================================
def model_tier(value, thresholds, tier):
    n = len(thresholds)
    if tier < n - 1 and n != 0:
        for i in range(n - 1, -1, -1):
            if thresholds[i] <= value:
                return i
            # the real loop gives up when i reaches 0 without a match
    return tier


TIER_CASES = [
    # name, value, current tier, thresholds
    ("below tier 1",     5.0, -1, [15.0, 30.0, 60.0, 90.0]),
    ("enters tier 0",   16.0, -1, [15.0, 30.0, 60.0, 90.0]),
    ("mid tier",        35.0, -1, [15.0, 30.0, 60.0, 90.0]),
    ("top tier",       120.0,  1, [15.0, 30.0, 60.0, 90.0]),
    ("no downgrade",     5.0,  3, [15.0, 30.0, 60.0, 90.0]),
]


def run_tier_cases():
    fails = 0
    print("\ncategory tier tracker (FUN_00192D20):")
    REC = 0x400
    THR = 0x500
    CLK = 0x600
    for name, value, tier, thr in TIER_CASES:
        mw = {ev.SCRATCH + THR: struct.pack('<%df' % len(thr), *thr),
              ev.SCRATCH + CLK + 0xC: struct.pack('<f', 33.25)}
        rec = bytearray(0x20)
        struct.pack_into('<f', rec, 0x00, 1.5)            # old value
        struct.pack_into('<I', rec, 0x0C, ev.SCRATCH + THR)
        rec[0x11] = tier & 0xFF
        rec[0x13] = len(thr)
        struct.pack_into('<I', rec, 0x18, ev.SCRATCH + CLK)
        mw[ev.SCRATCH + REC] = bytes(rec)
        img = ev.build_vehicle({})
        tr, after, err = ev.run(TIER, img, stack_args=[],
                                regs={UC_X86_REG_EAX: ev.SCRATCH + REC,
                                      UC_X86_REG_XMM0: f2u(value)},
                                mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        blob = tr.uc.mem_read(ev.SCRATCH + REC, 0x20)
        t_a = struct.unpack_from('<b', blob, 0x11)[0]
        v_a = struct.unpack_from('<f', blob, 0x00)[0]
        p_a = struct.unpack_from('<f', blob, 0x08)[0]
        c_a = struct.unpack_from('<f', blob, 0x04)[0]
        mt = model_tier(value, thr, tier)
        ok = (t_a == mt and abs(v_a - value) < 1e-6 and
              abs(p_a - 1.5) < 1e-6 and abs(c_a - 33.25) < 1e-6)
        print("  %-24s tier %2d value %6.1f prev %4.1f clk %5.2f  %s"
              % (name, t_a, v_a, p_a, c_a,
                 "OK" if ok else "FAIL (model tier %d)" % mt))
        fails += not ok
    return fails


# ===========================================================================
# 5. Air landing award -- FUN_00196940 (EDI = score object).
#    Landing path (airborne byte cleared, +0x3C8 "was scoring" set):
#      BP = AirCategoryBP[tier] added to score+0x4C and +0xB8 (player mask),
#      best-event record (+0x254 id 0x72, +0x258 max BP, +0x260 tier),
#      stats: +0x50 total metres += distance, +0x354 count += 1,
#             +0x54 best air = max(best, distance),
#      record reset: +0x360=dist, +0x36A=tier, +0x358=0, +0x369=0xFF, +0x3C8=0
# ===========================================================================
def run_air_case():
    fails = 0
    print("\nair landing award (FUN_00196940):")
    tier = 2
    dist = 42.0
    bp0, bpb = 1000, 50
    total0, best0, count0 = 100.0, 30.0, 2
    img = bytearray(ev.VEHICLE_SZ)
    struct.pack_into('<I', img, 0x0C8, ev.SCRATCH)        # racecar ptr
    struct.pack_into('<I', img, 0x26C, ev.SCRATCH)        # racecar ptr (again)
    struct.pack_into('<I', img, 0x370, ev.SCRATCH + 0x300)  # air-record clock src
    struct.pack_into('<i', img, 0x04C, bp0)               # BP total
    struct.pack_into('<i', img, 0x0B8, bpb)               # BP subtotal
    struct.pack_into('<f', img, 0x050, total0)            # total air metres
    struct.pack_into('<f', img, 0x054, best0)             # best single air
    struct.pack_into('<i', img, 0x354, count0)            # air count
    struct.pack_into('<f', img, 0x358, dist)              # current air metres
    img[0x369] = tier
    img[0x3C8] = 1                                        # air scoring active
    struct.pack_into('<i', img, 0x254, 0)                 # best-event id
    mw = fake_ctx()
    mw[ev.SCRATCH + 0x10C0] = b"\0"                       # landed
    mw[ev.SCRATCH + 0x18FA] = b"\0"                       # not crashed
    mw[ev.SCRATCH + 0x1920] = struct.pack('<i', 1)        # not the DJ branch
    mw[ev.SCRATCH + 0x10DC] = struct.pack('<f', 12.5)     # racecar clock
    mw[ev.SCRATCH + 0x300 + 0xC] = struct.pack('<f', 77.0)
    tr, after, err = ev.run(AIR, bytes(img), stack_args=[],
                            regs={UC_X86_REG_EDI: ev.VEHICLE},
                            mem_writes=mw)
    if err:
        print("  landing path            FAULT %s" % err)
        return 1
    bp = AIR_CAT_BP[tier]
    checks = [
        ("BP total",   struct.unpack_from('<i', after, 0x4C)[0], bp0 + bp),
        ("BP subtotal", struct.unpack_from('<i', after, 0xB8)[0], bpb + bp),
        ("total air",  struct.unpack_from('<f', after, 0x50)[0], total0 + dist),
        ("best air",   struct.unpack_from('<f', after, 0x54)[0], dist),
        ("air count",  struct.unpack_from('<i', after, 0x354)[0], count0 + 1),
        ("event id",   struct.unpack_from('<i', after, 0x254)[0], 0x72),
        ("event BP",   struct.unpack_from('<i', after, 0x258)[0], bp),
        ("event tier", struct.unpack_from('<i', after, 0x260)[0], tier),
        ("last dist",  struct.unpack_from('<f', after, 0x360)[0], dist),
        ("last tier",  after[0x36A], tier),
        ("record clk", struct.unpack_from('<f', after, 0x35C)[0], 77.0),
        ("dist reset", struct.unpack_from('<f', after, 0x358)[0], 0.0),
        ("tier reset", after[0x369], 0xFF),
        ("flag reset", after[0x3C8], 0),
    ]
    for label, got, want in checks:
        ok = (abs(got - want) < 1e-4) if isinstance(want, float) else got == want
        if not ok:
            print("  landing path            FAIL %s: got %s want %s"
                  % (label, got, want))
            fails += 1
    if not fails:
        print("  landing path            BP +%d, stats, reset all match  OK"
              % bp)
    return 1 if fails else 0


# ===========================================================================
# 6. Takedown commit -- FUN_00198E60 (ESI = score object, EDI = victim
#    racecar). Rule (bookkeeping subset; the BP chain inside FUN_001994D0
#    executes too and lands in racecar+0x111C):
#      dedup on victim+0x15D6; victim+0x15DC = my racecar; my takedown
#      count +0x68 += 1; +0x500 = clock; +0x4D4 = victim;
#      revenge: score+0x5B9[victim slot] set -> cleared + victim+0x168F=1,
#               else victim+0x1689[my slot] = 1.
# ===========================================================================
def run_takedown_cases():
    fails = 0
    print("\ntakedown commit (FUN_00198E60):")
    VICTIM = ev.SCRATCH + 0x8000
    MYCAR = ev.SCRATCH + 0xA000
    for name, revenge, dup in [("plain takedown", 0, False),
                               ("revenge takedown", 1, False),
                               ("double-credit blocked", 0, True)]:
        img = bytearray(ev.VEHICLE_SZ)      # score object at VEHICLE
        struct.pack_into('<I', img, 0x0C8, MYCAR)
        struct.pack_into('<I', img, 0x26C, MYCAR)
        struct.pack_into('<f', img, 0x00C, 20.0)          # score clock
        struct.pack_into('<i', img, 0x068, 3)             # takedown count
        # sub-object at +0x124 (FUN_001994D0's `this`): +0x148 rel = racecar
        struct.pack_into('<I', img, 0x124 + 0x148, MYCAR)
        struct.pack_into('<f', img, 0x124 + 0x110, -1.0)  # double-TD window
        struct.pack_into('<f', img, 0x124 + 0x118, -1.0)  # spree window
        img[0x5B9 + 2] = revenge          # victim slot 2 took me down before
        mw = fake_ctx()
        mw[DT_GLOBAL] = struct.pack('<f', DT)
        mw[VICTIM + 0x19BC] = bytes([2])                  # victim grid slot
        mw[VICTIM + 0x15D6] = bytes([1 if dup else 0])
        mw[VICTIM + 0x1920] = struct.pack('<i', 0)
        mw[MYCAR + 0x19BC] = bytes([0])                   # my grid slot
        mw[MYCAR + 0x10DC] = struct.pack('<f', 20.0)
        mw[MYCAR + 0x18FA] = b"\0"
        tr, after, err = ev.run(TDCOMMIT, bytes(img), stack_args=[],
                                regs={UC_X86_REG_ESI: ev.VEHICLE,
                                      UC_X86_REG_EDI: VICTIM},
                                mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        vic = tr.uc.mem_read(VICTIM, 0x1A00)
        car = tr.uc.mem_read(MYCAR, 0x1200)
        count = struct.unpack_from('<i', after, 0x68)[0]
        credited = vic[0x15D6]
        attacker = struct.unpack_from('<I', vic, 0x15DC)[0]
        if dup:
            ok = count == 3          # nothing awarded twice
        else:
            ok = (count == 4 and credited == 1 and attacker == MYCAR
                  and struct.unpack_from('<f', after, 0x500)[0] == 20.0
                  and struct.unpack_from('<I', after, 0x4D4)[0] == VICTIM
                  # FUN_001994D0's BP chain: Takedown BP (compiled default
                  # 1000, VDB 150) into racecar+0x111C/+0x117C, double-TD
                  # window = now + 1.0s, spree window = now + 30.0s
                  and struct.unpack_from('<i', car, 0x111C)[0] == 1000
                  and struct.unpack_from('<i', car, 0x117C)[0] == 1000
                  and abs(struct.unpack_from('<f', after, 0x124 + 0x110)[0]
                          - 21.0) < 1e-4
                  and abs(struct.unpack_from('<f', after, 0x124 + 0x118)[0]
                          - 50.0) < 1e-4)
            if revenge:
                ok = ok and after[0x5B9 + 2] == 0 and vic[0x168F] == 1
            else:
                ok = ok and vic[0x1689 + 0] == 1
        print("  %-24s count %d credited %d  %s"
              % (name, count, credited, "OK" if ok else "FAIL"))
        fails += not ok
    return fails


# ===========================================================================
# 6a. Takedown window expiry -- FUN_00199080 (per scoring frame, ECX =
#     score+0x124 sub-object; FUN_001935F0 prologue: LEA ECX,[EDI+0x124]).
#     Once the double-takedown (+0x110/+0x114) or spree (+0x118/+0x11C)
#     window has passed the racecar clock, count := 0 and window := -1.
# ===========================================================================
def run_expiry_case():
    print("\ntakedown window expiry (FUN_00199080):")
    img = bytearray(ev.VEHICLE_SZ)          # score+0x124 sub-object at base
    struct.pack_into('<I', img, 0x148, ev.SCRATCH)     # racecar ptr
    struct.pack_into('<f', img, 0x110, 10.0)           # double window end
    struct.pack_into('<i', img, 0x114, 2)              # double count
    struct.pack_into('<f', img, 0x118, 40.0)           # spree end (still open)
    struct.pack_into('<i', img, 0x11C, 3)              # spree count
    struct.pack_into('<f', img, 0x144, -1.0)           # no pending event
    mw = fake_ctx()
    mw[DT_GLOBAL] = struct.pack('<f', DT)
    mw[ev.SCRATCH + 0x10DC] = struct.pack('<f', 12.0)  # clock past 10.0
    tr, after, err = ev.run(0x00199080, bytes(img), stack_args=[],
                            regs={UC_X86_REG_ECX: ev.VEHICLE}, mem_writes=mw)
    if err:
        print("  window expiry           FAULT %s" % err)
        return 1
    ok = (struct.unpack_from('<i', after, 0x114)[0] == 0
          and struct.unpack_from('<f', after, 0x110)[0] == -1.0
          and struct.unpack_from('<i', after, 0x11C)[0] == 3
          and struct.unpack_from('<f', after, 0x118)[0] == 40.0)
    print("  window expiry           double reset, spree kept  %s"
          % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


# ===========================================================================
# 6b. Takedown boost + bar upgrade -- FUN_000273F0 (stack args: event ptr,
#     racecar).  Rule: if bar tier (+0x11CC) < 3: tier += 1 and re-load
#     size/rate/multiplier from the "Boost Bar" arrays; min units =
#     rate * MIN_BOOST_TIME.  Then award Takedown-Boost-value units
#     (compiled default 720, VDB 360) through the standard multiplier+clamp.
# ===========================================================================
BAR_SIZES_DEF = [240.0, 360.0, 600.0, 720.0]     # 0x3F72E4 compiled defaults
EARN_MULT_DEF = [1.0, 1.5, 2.5, 3.0]             # 0x3F72F4
TAKEDOWN_BOOST_DEF = 720.0                       # 0x3F73FC (VDB 360)


def run_upgrade_cases():
    fails = 0
    print("\ntakedown boost + bar upgrade (FUN_000273F0):")
    for name, tier0, size0, meter0 in (("tier 0 -> 1", 0, 240.0, 10.0),
                                       ("tier 3 stays", 3, 720.0, 10.0)):
        img = bytearray(ev.VEHICLE_SZ)               # racecar object
        struct.pack_into('<f', img, 0x10DC, 20.0)
        struct.pack_into('<i', img, 0x11CC, tier0)
        struct.pack_into('<f', img, 0x11D0, size0)
        struct.pack_into('<f', img, 0x11D4, meter0)  # meter
        struct.pack_into('<f', img, 0x11DC, 36.0)    # rate
        struct.pack_into('<f', img, 0x11E4, 1.0)     # mult base
        struct.pack_into('<f', img, 0x11E8, 0.0)     # mult bonus
        tr, after, err = ev.run(0x000273F0, bytes(img),
                                stack_args=[ev.SCRATCH + 0xC000, ev.VEHICLE])
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        # model
        tier = tier0 + 1 if tier0 < 3 else tier0
        size = BAR_SIZES_DEF[tier] if tier0 < 3 else size0
        mult = EARN_MULT_DEF[tier] if tier0 < 3 else 1.0
        meter, earned = model_award(meter0, size, 0.0, mult, 0.0,
                                    TAKEDOWN_BOOST_DEF)
        t_a = struct.unpack_from('<i', after, 0x11CC)[0]
        s_a = struct.unpack_from('<f', after, 0x11D0)[0]
        m_a = struct.unpack_from('<f', after, 0x11D4)[0]
        e_a = struct.unpack_from('<f', after, 0x11D8)[0]
        u_a = struct.unpack_from('<f', after, 0x11E4)[0]
        ok = (t_a == tier and abs(s_a - size) < 1e-3 and
              abs(m_a - meter) < 1e-3 and abs(e_a - earned) < 1e-3 and
              abs(u_a - mult) < 1e-3)
        print("  %-24s tier %d size %5.0f meter %6.1f earned %6.1f  %s"
              % (name, t_a, s_a, m_a, e_a, "OK" if ok else
                 "FAIL (model %d/%.0f/%.1f/%.1f)" % (tier, size, meter,
                                                     earned)))
        fails += not ok
    return fails


# ===========================================================================
# 7. Steer-away envelope -- FUN_0011ECF0 head (the rest of the function is
#    the verified gear-engage path; seeding pattern from validate_port).
#    With racecar slam timestamp t (+0x1598) and clock c (+0x10DC):
#      in OOC       iff c <= t + T_ooc            (v+0x13E4)
#      steer-away   iff c <= t + T_sa             (v+0x13E0)
#      +0x137C = maxang (v+0x13E8)                        while steering away
#               = base + (maxang-base)*(1 - (c-t-T_sa)/(T_ooc-T_sa))  decaying
#               = base (config obj +0x14C)                once expired
#    The +0x1690 event type scales T_sa/T_ooc by 0.6 and maxang by 0.8.
# ===========================================================================
def model_steer_away(base, maxang, t_sa, t_ooc, t, t2, clock):
    target = maxang            # decay target; scaled 0.8x for wall events
    if t >= 0.0 and clock <= t + t_ooc:
        pass
    elif t2 >= 0.0 and clock <= t2 + 0.6 * t_ooc:
        t, t_sa, t_ooc, target = t2, 0.6 * t_sa, 0.6 * t_ooc, 0.8 * maxang
    else:
        return base
    if clock <= t + t_sa:
        return maxang          # full response during the steer-away phase
    return base + (target - base) * (1.0 - (clock - (t + t_sa)) / (t_ooc - t_sa))


STEER_CASES = [
    # name, slam t (+0x1598), wall t (+0x1690), clock
    ("steer-away phase",   10.0, -1.0, 10.2),
    ("decay phase",        10.0, -1.0, 10.65),
    ("expired -> config",  10.0, -1.0, 11.5),
    ("wall event (x0.6)",  -1.0, 10.0, 10.15),
    ("wall decay",         -1.0, 10.0, 10.40),
]


def run_steer_cases():
    fails = 0
    print("\nsteer-away response envelope (FUN_0011ECF0):")
    BASE, MAXANG, T_SA, T_OOC = 10.0, 24.0, 0.3, 1.0
    st = dict(vp.ENGINE_BASE, omega=300.0, gear=3, speed_ms=30.0)
    for name, t1, t2, clock in STEER_CASES:
        ov = {0x1400: 1.0, 0x1404: 0.0, 0xBC: 30.0,
              0xB0: 0.0, 0xB4: 0.0, 0xB8: 1.0,
              0x9FC: 90.0, 0xABC: 90.0,
              0x14A4: 0, 0x14A0: 0.0,
              0x13E0: T_SA, 0x13E4: T_OOC, 0x13E8: MAXANG,
              0x13D4: st['max_boost_mph']}
        for i, g in enumerate(st['gears']):
            ov[vp.TRANS + i * 4] = g
        for k in ('idle', 'up', 'down', 'max', 'torque', 'limit', 'peak',
                  'falloff', 'kick_t', 'kick_time', 'omega', 'upblk', 'dnblk'):
            ov[vp.OT[k]] = st[k]
        for k in ('rng_a', 'rng_c', 'gear', 'ngears'):
            ov[vp.OT[k]] = st[k]
        mw = {DT_GLOBAL: struct.pack('<f', DT),
              vp.RAND_SCALE_GLOBAL: struct.pack('<f', vp.RAND_SCALE),
              ev.SCRATCH + 0x179C: struct.pack('<I', 1),
              # racecar (v+0x13F4 -> SCRATCH): self ptr, timestamps, clock
              ev.SCRATCH + 0x1198: struct.pack('<I', ev.SCRATCH),
              ev.SCRATCH + 0x1598: struct.pack('<f', t1),
              ev.SCRATCH + 0x1690: struct.pack('<f', t2),
              ev.SCRATCH + 0x10DC: struct.pack('<f', clock),
              # config object (v+0x13F8 -> SCRATCH): base steering response
              ev.SCRATCH + 0x14C: struct.pack('<f', BASE)}
        img = ev.build_vehicle(ov)
        tr, after, err = ev.run(ECF0, img, stack_args=[],
                                regs={UC_X86_REG_ECX: ev.VEHICLE,
                                      UC_X86_REG_ESI: f2u(30.0),
                                      UC_X86_REG_EDI: 0},
                                mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        got = struct.unpack_from('<f', after, 0x137C)[0]
        want = model_steer_away(BASE, MAXANG, T_SA, T_OOC, t1, t2, clock)
        ok = abs(got - want) < 1e-3
        print("  %-24s response %7.3f  %s"
              % (name, got, "OK" if ok else "FAIL (model %.3f)" % want))
        fails += not ok
    return fails


# ===========================================================================
# 8. Out-of-control steering authority -- FUN_00105340 (EDI = physics
#    vehicle). While the racecar was slammed within Total-OOC-Time AND its
#    recorded aggressor was hit within the same window and is a player car,
#    the AI's steering authority v+0x1534 is written to 0.1 (mode 2) /
#    0.05 (mode != 2).  Outside the window it is left alone.
# ===========================================================================
OOC_CASES = [
    # name, slam t, aggr t, mode(+0x23F8), clock, expect_authority (None = untouched)
    ("in window, mode 2",   10.0, 10.0, 2, 10.5, 0.1),
    ("expired",             10.0, 10.0, 2, 11.5, None),
    ("no aggressor",        10.0, -1.0, 2, 10.5, None),
]


def run_ooc_cases():
    fails = 0
    print("\nout-of-control steering authority (FUN_00105340):")
    RC = ev.SCRATCH                       # racecar object in scratch
    AGGR = ev.SCRATCH + 0x9000            # aggressor racecar
    for name, t1, ta, mode, clock, expect in OOC_CASES:
        ov = {0x13E4: 1.0,                # Total Out-Of-Control Time
              0xBC: 30.0,                 # speed (not "stuck")
              0x157C: -1.0,               # no recovery timer running
              0x1578: -1.0,
              0x1534: 1.0,                # steering authority, normally 1.0
              0x1524: 0}
        img = bytearray(ev.build_vehicle(ov))
        # 0x1568 isn't in build_vehicle's INT_FIELDS -- patch it as raw u32.
        struct.pack_into('<I', img, 0x1568, RC)
        mw = {DT_GLOBAL: struct.pack('<f', DT),
              CLOCK_GLOBAL: struct.pack('<f', clock),
              RC + 0x2190: struct.pack('<i', 1),
              RC + 0x2189: b"\x01",
              RC + 0x190C: struct.pack('<f', 1.0),   # not stuck
              RC + 0x23C4: struct.pack('<f', 40.0),  # target speed
              RC + 0x23C0: struct.pack('<f', 0.0),   # steering error
              RC + 0x23F8: struct.pack('<i', mode),
              RC + 0x1198: struct.pack('<I', RC),    # self pointer
              RC + 0x1598: struct.pack('<f', t1),    # slammed at
              RC + 0x16C0: struct.pack('<f', ta),    # aggressor contact time
              RC + 0x16BC: struct.pack('<I', AGGR if ta >= 0 else 0),
              RC + 0x10DC: struct.pack('<f', clock),
              RC + 0x134C: struct.pack('<i', 1),
              AGGR + 0x1920: struct.pack('<i', 0)}   # aggressor is a player
        tr, after, err = ev.run(OOC, bytes(img), stack_args=[],
                                regs={UC_X86_REG_EDI: ev.VEHICLE},
                                mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        got = struct.unpack_from('<f', after, 0x1534)[0]
        want = 1.0 if expect is None else expect
        ok = abs(got - want) < 1e-6
        print("  %-24s authority %5.3f  %s"
              % (name, got, "OK" if ok else "FAIL (model %.3f)" % want))
        fails += not ok
    return fails


# ===========================================================================
# 8b. AI racer driver input formulas -- FUN_00105340 again, this time the
#     normal driving path (racecar+0x2188 == 0, no OOC window, no recovery
#     timer).  Recovered rules under test (docs/RE_AI.md):
#       steer  v+0x1408 = clamp(racecar+0x23C0 * DAT_0047A15C * -1/180)
#              (DAT_0047A15C = "AI/Car/Max lock at 180 x degrees")
#       speed  vs target racecar+0x23C4: deficit > 1.0  -> throttle 1
#              excess > DAT_005A39EC (init code copies 13.4112 = 30 mph from
#              .data 0x3B1A5C)      -> full brake via FUN_00104CA0(1.0)
#              (v+0x14C8 = 1 forward: +0x1404 = 1, throttle zeroed)
#              otherwise            -> coast (all inputs zero)
#       boost  while racecar+0x11EE (record boosting flag): input bits
#              v+0x13FC = 4 -- the transmission's boost input anchor.
# ===========================================================================
AI_DRIVER_CASES = [
    # name, steer_tgt(+23C0), target(+23C4), prev_thr(+156C), want_boost/11EE,
    #       checks {voff: (want, kind)}
    ("steer formula",   20.0, 40.0, 1.0, 0,
     {0x1408: (-20.0 * 6.0 / 180.0, 'f')}),
    ("steer clamp",    -60.0, 40.0, 1.0, 0,
     {0x1408: (1.0, 'f')}),
    ("throttle above band", 0.0, 40.0, 1.0, 0,
     {0x1400: (1.0, 'f'), 0x1414: (1.0, 'f'), 0x1404: (0.0, 'f')}),
    ("coast in band",   0.0, 30.5, 0.0, 0,
     {0x1400: (0.0, 'f'), 0x1414: (0.0, 'f'), 0x1404: (0.0, 'f')}),
    ("brake at 30mph over", 0.0, 10.0, 0.0, 0,
     {0x1404: (1.0, 'f'), 0x1400: (0.0, 'f'), 0x1414: (0.0, 'f')}),
    ("boost input bit", 0.0, 40.0, 1.0, 1,
     {0x13FC: (4, 'b'), 0x1404: (0.0, 'f')}),
]

AI_MAX_LOCK = 6.0        # seeded into DAT_0047A15C (compiled default)
AI_BRAKE_MS = 13.4112    # .data 0x3B1A5C -> DAT_005A39EC/0x5A3A10 (30 mph)


def run_ai_driver_cases():
    fails = 0
    print("\nAI racer driver inputs (FUN_00105340 driving path):")
    RC = ev.SCRATCH
    clock = 10.0
    for name, st, tgt, prev, boosting, checks in AI_DRIVER_CASES:
        ov = {0x13E4: 1.0,
              0xBC: 30.0,                 # speed m/s
              0x157C: -1.0,               # no reverse timer
              0x1578: -1.0,               # no stuck arm
              0x1534: 1.0,
              0x1524: 0,
              0x1574: 0.0,                # throttle-dither timer idle
              0x156C: prev,               # previous throttle (dither state)
              0x1570: -1.0,               # FUN_00104CA0's own timer
              0x1400: prev}
        img = bytearray(ev.build_vehicle(ov))
        struct.pack_into('<I', img, 0x1568, RC)
        struct.pack_into('<i', img, 0x14C8, 1)     # forward drive
        mw = {DT_GLOBAL: struct.pack('<f', DT),
              CLOCK_GLOBAL: struct.pack('<f', clock),
              0x0047A15C: struct.pack('<f', AI_MAX_LOCK),
              0x005A39EC: struct.pack('<f', AI_BRAKE_MS),
              0x005A3A10: struct.pack('<f', AI_BRAKE_MS),
              RC + 0x2190: struct.pack('<i', 0),   # bVar3 false: brake armed
              RC + 0x2189: b"\x00",
              RC + 0x190C: struct.pack('<f', 1.0),
              RC + 0x23C4: struct.pack('<f', tgt),
              RC + 0x23C0: struct.pack('<f', st),
              RC + 0x23F8: struct.pack('<i', 2),
              RC + 0x2188: b"\x00",                # normal driving path
              RC + 0x2413: b"\x00",
              RC + 0x2414: b"\x00",
              RC + 0x2419: b"\x01" if boosting else b"\x00",
              RC + 0x11EE: b"\x01" if boosting else b"\x00",
              RC + 0x11F1: b"\x00",
              RC + 0x1198: struct.pack('<I', RC),
              RC + 0x1598: struct.pack('<f', -1.0),   # never slammed
              RC + 0x16C0: struct.pack('<f', -1.0),
              RC + 0x16BC: struct.pack('<I', 0),
              RC + 0x10DC: struct.pack('<f', clock),
              RC + 0x134C: struct.pack('<i', 1)}
        tr, after, err = ev.run(OOC, bytes(img), stack_args=[],
                                regs={UC_X86_REG_EDI: ev.VEHICLE},
                                mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        bad = []
        for off, (want, kind) in checks.items():
            if kind == 'f':
                got = struct.unpack_from('<f', after, off)[0]
                if abs(got - want) > 1e-4:
                    bad.append("+%04X %.4f != %.4f" % (off, got, want))
            else:
                got = after[off]
                if got != want:
                    bad.append("+%04X %d != %d" % (off, got, want))
        print("  %-24s %s" % (name, "OK" if not bad else
                              "FAIL " + "; ".join(bad)))
        fails += bool(bad)
    return fails


# ===========================================================================
# 9. Parameter data checks: the registrar-recovered VDB keys resolve in
#    Data/vdb.xml to the tuned values used in the harness tables.
# ===========================================================================
def run_param_checks():
    import extract_car_vdb as cv
    fails = 0
    print("\nscore/boost parameter pipeline (registrar + Data/vdb.xml):")
    vdb, _ = cv.read_vdb()
    raw = open(cv.VDB_FILE, 'rb').read()
    fdo = struct.unpack_from('<I', raw, 16)[0]
    dvc = struct.unpack_from('<I', raw, 4)[0]

    def look(param, group, count=0, as_int=False):
        h = cv.gt_hash(("%s%s/../Export/ValueDB/Score.cfg"
                        % (param, group)).encode())
        v = vdb.get(h)
        if v is None:
            return None
        if count == 0:
            return v if as_int else u2f(v)
        assert 20 + dvc * 8 <= v < fdo
        out = [struct.unpack_from('<I', raw, v + 4 * i)[0]
               for i in range(count)]
        return out if as_int else [u2f(x) for x in out]

    checks = [
        ("Takedown Boost value (Boost units)", "Score/Boost/Takedowns",
         0, False, 360.0),
        ("Boost value per Near Miss (Boost units)", "Score/Boost/Near Miss",
         0, False, 15.0),
        ("Takedown BP", "Score/Burnout Points", 0, True, 150),
        ("Revenge Takedown Bonus BP", "Score/Burnout Points", 0, True, 350),
        ("Boost Bar sizes (Boost Units)", "Boost Bar", 4, False,
         [240.0, 360.0, 540.0, 720.0]),
        ("Boost Rate (Boost Units per Second)", "Boost Bar", 4, False,
         [36.0, 36.0, 36.0, 36.0]),
        ("Minimum Boost Time (seconds)", "Boost Bar", 0, False, 0.5),
        ("Double Takedown BP", "Score/Burnout Points", 4, True,
         [300, 500, 750, 1000]),
    ]
    for param, group, count, as_int, want in checks:
        got = look(param, group, count, as_int)
        ok = got == want
        print("  %-42s %-28s %s" % (param, got, "OK" if ok else
                                    "FAIL (want %s)" % (want,)))
        fails += not ok
    return fails


# ===========================================================================
# 10. Damage state machine (docs/RE_NOTES.md section 13).
#
# The per-panel damage state lives in the vehicle's damage/visual context
# ctx = *(vehicle+0xCC4): panel state bytes at ctx+0x4B2 (one per body part,
# count = model+0xC where model = *(*(vehicle+0xCC0)+0x40)), wheel state
# bytes at ctx+0x4AC (count = vehicle+0x1169).  States:
#   0 pristine / 1 (never written by any located writer; read as "like 0")
#   2 crumpled   3 detached (a flying-part pool record animates it)
#   4 gone (debris retired: FUN_00115fc0, or pool slot stolen: FUN_00111340)
#
# Writers verified here by executing the real code:
#   FUN_00123000 (head)  per-frame accumulator scan:
#     panel: acc ctx+0xF90[i] > threshold ctx+0xFC0[i]  -> state {0,1}->2
#            acc > DAT_005A80C8 (999.0, init 0x2BA780)  -> FUN_00125A50
#     wheel: acc ctx+0xF78[i] > 0.3                     -> state {0,1}->2
#            acc > 0.5 && state==2 && ctx+0x4D0[i]>0.05 -> 3 + pool alloc,
#            revert to 2 if the alloc fails
#   FUN_00125A50  panel detach: state 2 -> 3 stamped BEFORE the pool alloc
#            (FUN_00111340, stdcall ret 0x10), reverted to 2 on failure
#   FUN_00023DE0  health-driven distributor (per frame, all cars):
#            health = racecar+0x16C4 (1.0 pristine .. 0.0 wrecked);
#            ctx+0x1014 tier: 2 if health<0.7 else 1 if health<0.95;
#            target intact = int(nparts * min(1.0, health+0.1)); panels are
#            dented (0->2) / detached (2->3, "interesting" kinds only, i.e.
#            model+0xAC4 kind not in {0,1,3,5,6}) walking the priority table
#            DAT_00385224 = [3,6,0,1,5,4,2] until the count matches
#   FUN_00025CC0  crash sequencer: crash-count +0x16C8 ticks down; health
#            -= 1.0 (clamped) then reset to 1.0 unless the count hit 0
#   FUN_000300A0  glass dressing: EAX=part header (byte+1 = glass record
#            index), EDX=tier: 1 -> texture slot 3 tint 0.5, 2 -> slot 4
#            tint 0.6, else slot 2 tint restored from record+8
#
# The pool allocator FUN_00111340 / FX helpers FUN_0012AD60, FUN_0012B280 /
# flying-part init FUN_00106F20 / FUN_0014FC30 are NOT under test: they are
# replaced with convention-preserving stubs (same ret-N), exactly like the
# game-context virtual stubs above.  What is asserted is the state logic of
# the functions under test, executing their real instructions.
# ===========================================================================
DMG_SCAN = 0x00123000        # accumulator scan (head; tail = tyre grip)
DMG_SCAN_STOP = 0x00123265   # first instruction past the +0x116B block
DMG_DETACH = 0x00125A50
DMG_DISTRIB = 0x00023DE0
DMG_CRASHSEQ = 0x00025CC0
DMG_GLASS = 0x000300A0
POOL_ALLOC = 0x00111340      # stdcall, RET 0x10
FLYPART_INIT = 0x00106F20    # EAX = record, plain RET
SETF_HELPER = 0x0014FC30     # RET 0x8
FX_SMOKE = 0x0012AD60        # RET 0x10
FX_SMOKE2 = 0x0012B280       # RET 0xC

CTX = ev.SCRATCH + 0x10000   # damage/visual context (*(vehicle+0xCC4))
MODEL = ev.SCRATCH + 0x20000  # model object (*(*(vehicle+0xCC0)+0x40))
OBJ = ev.SCRATCH + 0x28000   # *(vehicle+0xCC0)
RACECAR = ev.SCRATCH + 0x30000
FLYREC = ev.SCRATCH + 0x38000  # fake flying-part record for alloc-success

PANEL_DETACH_ACC = 999.0     # DAT_005A80C8 (static init at 0x002BA780)
WHEEL_CRUMPLE_ACC = 0.3      # 0x003B1750
WHEEL_DETACH_ACC = 0.5       # 0x003B1684
WHEEL_DETACH_MIN = 0.05      # 0x003A69BC vs ctx+0x4D0[i]


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def stub_writes(alloc_ok):
    """Convention-preserving stubs for the collaborators (see header)."""
    mw = {
        FLYPART_INIT: b"\xc3",                          # ret
        SETF_HELPER: b"\xc2\x08\x00",                   # ret 8
        FX_SMOKE: b"\xc2\x10\x00",                      # ret 0x10
        FX_SMOKE2: b"\xc2\x0c\x00",                     # ret 0xC
    }
    if alloc_ok:
        mw[POOL_ALLOC] = b"\xb8" + struct.pack('<I', FLYREC) + b"\xc2\x10\x00"
    else:
        mw[POOL_ALLOC] = b"\x31\xc0\xc2\x10\x00"        # xor eax,eax; ret 0x10
    return mw


def dmg_env(panel_states, wheel_states, panel_acc, panel_thr, wheel_acc,
            wheel_d0, contacts, alloc_ok, health=1.0, tier_seed=7,
            kinds=None):
    """mem_writes seeding ctx/model/obj/racecar + stubs + globals."""
    n = len(panel_states)
    mw = stub_writes(alloc_ok)
    mw.update({
        CTX + 0x4AC: bytes(wheel_states),
        CTX + 0x4B2: bytes(panel_states),
        CTX + 0x4D0: struct.pack('<6f', *(wheel_d0 + [0.0] * (6 - len(wheel_d0)))),
        CTX + 0xF78: struct.pack('<%df' % len(wheel_acc), *wheel_acc),
        CTX + 0xF90: struct.pack('<%df' % len(panel_acc), *panel_acc),
        CTX + 0xFC0: struct.pack('<%df' % len(panel_thr), *panel_thr),
        CTX + 0x1014: bytes([tier_seed]),
        MODEL + 0xC: bytes([n]),
        OBJ + 0x40: struct.pack('<I', MODEL),
        RACECAR + 0x16C4: struct.pack('<f', health),
        0x005A80C8: struct.pack('<f', PANEL_DETACH_ACC),
    })
    if kinds is not None:
        mw[MODEL + 0xAC4] = struct.pack('<%di' % len(kinds), *kinds)
    return mw


def dmg_vehicle(contacts, nwheels=4):
    img = bytearray(ev.build_vehicle({0xCC4: CTX, 0x13F4: RACECAR}))
    struct.pack_into('<I', img, 0xCC0, OBJ)
    img[0x1169] = nwheels
    img[0x116B] = 0
    img[0x210] = 0            # not skipped by the distributor
    img[0x215] = 0            # no per-panel FX call (overridden per case)
    struct.pack_into('<4f', img, 0xC0, 0.3, 0.1, 0.9, 0.0)  # drag direction
    struct.pack_into('<4f', img, 0xF0, 1.0, 2.0, 3.0, 4.0)  # force accum
    for i, c in enumerate(contacts):
        img[0x8D3 + i * 0xC0] = c
    return img


def model_scan(panel_states, wheel_states, panel_acc, panel_thr, wheel_acc,
               wheel_d0, contacts, alloc_ok):
    """Mirror of the FUN_00123000 head (+ FUN_00125A50 for panel detach)."""
    ps, ws = list(panel_states), list(wheel_states)
    flag_1168 = 1
    for i, c in enumerate(contacts):
        if ws[i] != 3 and c:
            flag_1168 = 0
    for i in range(len(ps)):
        if panel_acc[i] > panel_thr[i] and ps[i] in (0, 1):
            ps[i] = 2
        if panel_acc[i] > PANEL_DETACH_ACC and ps[i] == 2:
            ps[i] = 3 if alloc_ok else 2      # stamped 3, reverted on failure
    for i in range(len(ws)):
        if wheel_acc[i] > WHEEL_CRUMPLE_ACC and ws[i] in (0, 1):
            ws[i] = 2
        if (wheel_acc[i] > WHEEL_DETACH_ACC and ws[i] == 2
                and wheel_d0[i] > WHEEL_DETACH_MIN):
            ws[i] = 3 if alloc_ok else 2
    return ps, ws, flag_1168


SCAN_CASES = [
    # name, panel_states, wheel_states, panel_acc, panel_thr,
    #       wheel_acc, wheel_d0, contacts, alloc_ok
    ("crumple thresholds",
     [0, 0, 0, 1], [0, 0, 1, 0],
     [0.10, 0.03, 0.2, 0.0], [0.05, 0.05, 0.5, 0.05],
     [0.4, 0.2, 0.31, 0.0], [0.0, 0.0, 0.0, 0.0], [1, 0, 0, 0], False),
    ("detach fail-revert",
     [0, 0, 0, 0], [2, 2, 0, 0],
     [1000.0, 0.0, 0.0, 0.0], [0.05, 0.05, 0.05, 0.05],
     [0.6, 0.6, 0.0, 0.0], [0.2, 0.01, 0.0, 0.0], [0, 0, 0, 0], False),
    ("detach success",
     [0, 0, 0, 0], [2, 2, 0, 0],
     [1000.0, 0.0, 0.0, 0.0], [0.05, 0.05, 0.05, 0.05],
     [0.6, 0.6, 0.0, 0.0], [0.2, 0.01, 0.0, 0.0], [0, 0, 0, 0], True),
]


def run_scan_cases():
    fails = 0
    print("\ndamage accumulator scan (FUN_00123000 head + FUN_00125A50):")
    for (name, ps0, ws0, pacc, pthr, wacc, wd0, contacts,
         alloc_ok) in SCAN_CASES:
        mw = dmg_env(ps0, ws0, pacc, pthr, wacc, wd0, contacts, alloc_ok)
        # stop the run where the tyre-grip tail begins: jump to MAGIC_RET
        rel = ev.MAGIC_RET - (DMG_SCAN_STOP + 5)
        mw[DMG_SCAN_STOP] = b"\xe9" + struct.pack('<i', rel)
        img = dmg_vehicle(contacts)
        tr, after, err = ev.run(DMG_SCAN, bytes(img), stack_args=[0],
                                regs={UC_X86_REG_ECX: ev.VEHICLE},
                                mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        uc_ps = list(tr.uc.mem_read(CTX + 0x4B2, len(ps0)))
        uc_ws = list(tr.uc.mem_read(CTX + 0x4AC, len(ws0)))
        got_flag = after[0x1168]
        got_f = struct.unpack_from('<4f', after, 0xF0)
        m_ps, m_ws, m_flag = model_scan(ps0, ws0, pacc, pthr, wacc, wd0,
                                        contacts, alloc_ok)
        speed = 40.0                          # dmg_vehicle default +0xBC
        drag = [f32(1.0 + 0.3 * -(speed * speed)),
                f32(2.0 + 0.1 * -(speed * speed)),
                f32(3.0 + 0.9 * -(speed * speed)), f32(4.0)]
        ok = (uc_ps == m_ps and uc_ws == m_ws and got_flag == m_flag
              and all(abs(a - b) < 1e-2 for a, b in zip(got_f, drag)))
        print("  %-24s panels %s wheels %s flag %d  %s"
              % (name, uc_ps, uc_ws, got_flag,
                 "OK" if ok else "FAIL (model %s %s %d)" % (m_ps, m_ws, m_flag)))
        fails += not ok
    return fails


def model_distribute(health, kinds, states, alloc_ok, tier_seed):
    """Mirror of FUN_00023DE0 for one car (FX/pool stubbed as in the run)."""
    EXCLUDED = {0, 1, 3, 5, 6}
    PRIORITY = [3, 6, 0, 1, 5, 4, 2]          # DAT_00385224
    tier = tier_seed
    if health < 0.7:
        tier = 2
    elif health < 0.95:
        tier = 1
    n = len(kinds)
    states = list(states)
    target = int(f32(n * f32(min(1.0, f32(health + 0.1)))))
    need = sum(1 for k, s in zip(kinds, states)
               if s not in (3, 4) and k not in EXCLUDED) - target
    guard = 0
    while need > 0 and guard < 64:
        guard += 1
        for pk in PRIORITY:
            idx = next((i for i, k in enumerate(kinds) if k == pk), None)
            if idx is None:
                continue
            if states[idx] == 0:
                states[idx] = 2               # FX then dent (0/1 -> 2)
            if states[idx] == 2 and kinds[idx] not in EXCLUDED:
                if alloc_ok:
                    states[idx] = 3           # FUN_00125A50 detach
                need -= 1
                break
    need = sum(1 for s in states if s in (0, 1)) - target
    guard = 0
    while need > 0 and guard < 64:
        guard += 1
        for pk in PRIORITY:
            idx = next((i for i, k in enumerate(kinds) if k == pk), None)
            if idx is None:
                continue
            if states[idx] in (0, 1):
                states[idx] = 2
                need -= 1
                break
    return tier, states


DISTRIB_CASES = [
    # name, health, kinds (model+0xAC4), initial panel states, alloc_ok
    ("pristine no-op",   1.0, [3, 6, 0, 4, 2], [0, 0, 0, 0, 0], True),
    ("worn -> tier 1",   0.8, [3, 6, 0, 4, 2], [0, 0, 0, 0, 0], True),
    ("wrecked -> burst", 0.0, [3, 6, 0, 4, 2], [0, 0, 0, 0, 0], True),
]


def run_distrib_cases():
    fails = 0
    print("\nhealth-driven panel distributor (FUN_00023DE0):")
    for name, health, kinds, ps0, alloc_ok in DISTRIB_CASES:
        contacts = [0, 0, 0, 0]
        mw = dmg_env(ps0, [0] * 4, [0.0] * len(ps0), [9.9] * len(ps0),
                     [0.0] * 4, [0.0] * 4, contacts, alloc_ok,
                     health=health, kinds=kinds)
        mw[0x0073A19C] = struct.pack('<i', 1)          # one car
        mw[0x0064B38C] = struct.pack('<I', ev.VEHICLE)  # slot table entry 0
        img = dmg_vehicle(contacts)
        img[0x215] = 1                        # exercise the FX stub path too
        tr, after, err = ev.run(DMG_DISTRIB, bytes(img), stack_args=[],
                                mem_writes=mw, max_steps=400000)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        uc_ps = list(tr.uc.mem_read(CTX + 0x4B2, len(ps0)))
        uc_tier = tr.uc.mem_read(CTX + 0x1014, 1)[0]
        m_tier, m_ps = model_distribute(health, kinds, ps0, alloc_ok, 7)
        ok = uc_ps == m_ps and uc_tier == m_tier
        print("  %-24s tier %d panels %s  %s"
              % (name, uc_tier, uc_ps,
                 "OK" if ok else "FAIL (model %d %s)" % (m_tier, m_ps)))
        fails += not ok
    return fails


CRASHSEQ_CASES = [
    # name, crash count +0x16C8, health, expect_health, expect_count
    ("countdown -> reset", 3, 0.9, 1.0, 2),
    ("count 0 -> zeroed",  1, 0.9, 0.0, 0),
]


def run_crashseq_cases():
    fails = 0
    print("\ncrash sequencer health rule (FUN_00025CC0):")
    for name, count, health, want_h, want_c in CRASHSEQ_CASES:
        img = bytearray(ev.build_vehicle({}))
        struct.pack_into('<i', img, 0x16C8, count)
        struct.pack_into('<f', img, 0x16C4, health)
        struct.pack_into('<i', img, 0x1920, 1)         # AI: skip HUD branch
        mw = {0x0073A1A4: struct.pack('<i', 5),
              0x0073A1C0: struct.pack('<i', 0)}
        tr, after, err = ev.run(DMG_CRASHSEQ, bytes(img), stack_args=[0],
                                mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        got_h = struct.unpack_from('<f', after, 0x16C4)[0]
        got_c = struct.unpack_from('<i', after, 0x16C8)[0]
        ok = abs(got_h - want_h) < 1e-6 and got_c == want_c
        print("  %-24s health %.2f count %d  %s"
              % (name, got_h, got_c,
                 "OK" if ok else "FAIL (want %.2f %d)" % (want_h, want_c)))
        fails += not ok
    return fails


GLASS_CASES = [
    # name, tier (EDX), expect_slot, expect_tint (None = restored from +0x8)
    ("intact restore", 0, 2, None),
    ("cracked",        1, 3, 0.5),
    ("shattered",      2, 4, 0.6),
]


def run_glass_cases():
    fails = 0
    print("\nglass damage dressing (FUN_000300A0):")
    HDR = ev.SCRATCH + 0x40000
    RECS = ev.SCRATCH + 0x41000
    GLASS_IDX, DEFAULT_TINT = 2, 0.85
    for name, tier, want_slot, want_tint in GLASS_CASES:
        rec = RECS + GLASS_IDX * 0x1C
        mw = {HDR: bytes([0, GLASS_IDX, 0, 0]) + struct.pack('<I', RECS),
              rec + 0x4: struct.pack('<f', -1.0),
              rec + 0x8: struct.pack('<f', DEFAULT_TINT),
              rec + 0x1A: b"\x00"}
        img = ev.build_vehicle({})
        tr, after, err = ev.run(DMG_GLASS, img, stack_args=[],
                                regs={UC_X86_REG_EAX: HDR,
                                      UC_X86_REG_EDX: tier},
                                mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        got_slot = tr.uc.mem_read(rec + 0x1A, 1)[0]
        got_tint = struct.unpack('<f', tr.uc.mem_read(rec + 0x4, 4))[0]
        want = DEFAULT_TINT if want_tint is None else want_tint
        ok = got_slot == want_slot and abs(got_tint - want) < 1e-6
        print("  %-24s slot %d tint %.2f  %s"
              % (name, got_slot, got_tint,
                 "OK" if ok else "FAIL (want %d %.2f)" % (want_slot, want)))
        fails += not ok
    return fails


# ---------------------------------------------------------------------------
# Collision world (streamed.dat unit LOD blocks) -- RE_NOTES section 15.
#
# Ground truth is the game's own code run over the game's own data:
#   FUN_001b02b0  collision-header relinker (offsets -> pointers)
#   FUN_001aff70  kd-tree walker: (header, sphere4*, callback, cbArg)
#   0x00109ce0    the per-frame gather callback (appends into the global soup
#                 at 0x005a3aa0, skipping surface-type low bytes 20/22/23/24)
#   FUN_00123790  the wheel ray query over the gathered soup
# Two case groups:
#   enum:  the walker's enumerated triangle set (huge sphere) must equal
#          tools/extract_collision.py's parse of the same unit, exactly.
#   probe: for real track positions, game gather + game ray must equal the
#          Python mirror of src/burnout3_collision.c's b3_ground_probe math
#          (Moller-Trumbore with the game's constants 1e-8 / -1e-5 / 1.00001)
#          run over the extractor's triangles: hit/miss, distance, winning
#          triangle normal + surface type, and the ctx+0x324 grip write.

COLL_BLOCK = 0x60000000
COLL_MAGIC = 0x50000000
COLL_SOUP_HDR, COLL_SOUP_RECS, COLL_SOUP_TYPS = 0x005a3aa0, 0x005a3ab0, 0x005a52b0
COLL_OVERFLOW = 0x00478a30
COLL_RELINK, COLL_WALK, COLL_CB, COLL_RAY = (0x001b02b0, 0x001aff70,
                                             0x00109ce0, 0x00123790)
# FUN_001b2230's compiled-in constants (read from the image, see section 15)
COLL_DET_EPS = 9.99999993922529e-09          # 0x003a6860
COLL_LO_K = -9.999999747378752e-06           # 0x003b16a0
COLL_HI_K = 1.0000100135803223               # 0x003b1918

ENUM_UNITS = [0, 24, 35]

# (name, x, y, z) in raw game coordinates; y = local road height.
# route N = fraction along the recovered 1029-pt race line.  0.43/0.46 ride
# the flyover deck, 0.49 is the underpass BELOW it (the ray must pick the
# lower road), deck+8 probes the same underpass x,z from deck height (the
# ray must pick the deck), low-road is an absolute streamed-world probe, and
# the offroad/kerb pair are agreed no-hit positions (game and mirror both
# miss).
PROBE_CASES = [
    ("route 0.02",  0.02, 0.0, 0.0),
    ("route 0.43",  0.43, 0.0, 0.0),
    ("route 0.46",  0.46, 0.0, 0.0),
    ("route 0.49",  0.49, 0.0, 0.0),
    ("deck+8",      0.49, 8.0, 0.0),
    ("route 0.60",  0.60, 0.0, 0.0),
    ("route 0.75",  0.75, 0.0, 0.0),
    ("route 0.90",  0.90, 0.0, 6.0),
    ("kerb+6",      0.097, 0.0, 6.0),
    ("offroad+20",  0.097, 0.0, 20.0),
    ("low road", None, None, None),          # absolute, filled below
    ("route 0.30",  0.30, 0.0, 0.0),         # agreed miss (median slot)
]
LOW_ROAD_PROBE = (5551.168, 136.266, 2781.530)


class CollisionRig:
    """One streamed unit's collision block, relinked by the game's own code."""

    def __init__(self, unit):
        from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32,
                             UC_HOOK_MEM_UNMAPPED, UcError, UC_PROT_ALL)
        import extract_collision as xc
        self.unit = unit
        sd = open(os.path.join(xc.TRACK_DIR, "static.dat"), "rb").read()
        st = open(os.path.join(xc.TRACK_DIR, "streamed.dat"), "rb").read()
        tab = struct.unpack_from("<i", sd, 0x58)[0]
        so, lo, ss, ls = struct.unpack_from("<iiii", sd, tab + unit * 0x10)
        blk = st[lo:lo + ls]
        self.tris = xc.parse_unit_collision(blk, unit)["tris"]

        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        ev.load_elf(uc, ev.ELF)
        for base, size in ((ev.STACK_BASE, ev.STACK_SIZE),
                           (COLL_BLOCK, (ls + 0xFFF) & ~0xFFF),
                           (ev.VEHICLE, 0x10000), (ev.SCRATCH, 0x10000),
                           (COLL_MAGIC & ~0xFFF, 0x1000)):
            uc.mem_map(base, size, UC_PROT_ALL)
        uc.mem_write(COLL_BLOCK, blk)

        def on_unmapped(u, a, addr, sz, val, us):
            try:
                u.mem_map(addr & ~0xFFF, 0x1000, UC_PROT_ALL)
            except UcError:
                return False
            return True
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
        self.uc = uc

        hdr_off = struct.unpack_from("<I", blk, 0xA0)[0]
        self.hdr = COLL_BLOCK + hdr_off
        uc.mem_write(COLL_BLOCK + 0xA0, struct.pack("<I", self.hdr))
        self.call(COLL_RELINK, [], ecx=self.hdr)     # the game's relinker

        # minimal vehicle for the gather callback + ray query
        veh, ctx, frame = ev.VEHICLE, ev.SCRATCH, ev.VEHICLE + 0x8000
        uc.mem_write(veh + 0x200, struct.pack("<I", COLL_SOUP_HDR))
        uc.mem_write(frame, struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0,
                                        0, 0, 1, 0, 0, 0, 0, 1))
        uc.mem_write(veh + 0x204, struct.pack("<I", frame))
        uc.mem_write(veh + 0xCC4, struct.pack("<I", ctx))
        uc.mem_write(veh + 0x210, b"\x00")           # no slope gate
        uc.mem_write(veh + 0x215, b"\x00")           # no crash-mode type filter
        uc.mem_write(ctx + 0x324, struct.pack("<f", 1.2))

    def call(self, fn, stack_args, ecx=None, esi=None):
        from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_ECX, \
            UC_X86_REG_ESI, UC_X86_REG_EAX
        uc = self.uc
        sp = ev.STACK_BASE + ev.STACK_SIZE - 0x4000
        uc.mem_write(sp, struct.pack("<I", COLL_MAGIC))
        for i, a in enumerate(stack_args):
            uc.mem_write(sp + 4 + i * 4, struct.pack("<I", a & 0xFFFFFFFF))
        uc.reg_write(UC_X86_REG_ESP, sp)
        if ecx is not None:
            uc.reg_write(UC_X86_REG_ECX, ecx)
        if esi is not None:
            uc.reg_write(UC_X86_REG_ESI, esi)
        uc.emu_start(fn, COLL_MAGIC, count=80_000_000)
        return uc.reg_read(UC_X86_REG_EAX)

    def enum_tris(self):
        """Walk with a hooked callback: every expanded prim the game sees."""
        from unicorn import UC_HOOK_CODE
        from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX, \
            UC_X86_REG_EIP
        uc = self.uc
        h = uc.mem_read(self.hdr, 0x20)
        bmax = struct.unpack_from("<3f", h, 0)
        bmin = struct.unpack_from("<3f", h, 0x10)
        c = [(a + b) / 2 for a, b in zip(bmin, bmax)]
        r = max(b - a for a, b in zip(bmin, bmax)) * 2 + 100
        sph = COLL_MAGIC + 0x200
        uc.mem_write(sph, struct.pack("<4f", c[0], c[1], c[2], r))
        cb = COLL_MAGIC + 0x800
        hits = []

        def on_code(u, address, size, user):
            if address != cb:
                return
            esp = u.reg_read(UC_X86_REG_ESP)
            rec = struct.unpack("<I", u.mem_read(esp + 4, 4))[0]
            m = u.mem_read(rec, 0x70)
            hits.append((struct.unpack_from("<3f", m, 0x20),
                         struct.unpack_from("<3f", m, 0x30),
                         struct.unpack_from("<3f", m, 0x40),
                         struct.unpack("<I", u.mem_read(
                             struct.unpack_from("<I", m, 0x60)[0] + 4, 4))[0]
                         & 0xFFFF))
            ret = struct.unpack("<I", u.mem_read(esp, 4))[0]
            u.reg_write(UC_X86_REG_EAX, 1)
            u.reg_write(UC_X86_REG_ESP, esp + 4)
            u.reg_write(UC_X86_REG_EIP, ret)
        hh = uc.hook_add(UC_HOOK_CODE, on_code, begin=cb, end=cb + 1)
        self.call(COLL_WALK, [self.hdr, sph, cb, 0])
        uc.hook_del(hh)
        return hits

    def gather(self, cx, cy, cz, r):
        """The game's own soup fill (real callback 0x00109ce0)."""
        uc = self.uc
        uc.mem_write(COLL_SOUP_HDR,
                     struct.pack("<3I", 0, COLL_SOUP_RECS, COLL_SOUP_TYPS))
        uc.mem_write(COLL_OVERFLOW, b"\x00")
        sph = COLL_MAGIC + 0x200
        uc.mem_write(sph, struct.pack("<4f", cx, cy, cz, r))
        self.call(COLL_WALK, [self.hdr, sph, COLL_CB, ev.VEHICLE])
        n = struct.unpack("<I", uc.mem_read(COLL_SOUP_HDR, 4))[0]
        return n, uc.mem_read(COLL_OVERFLOW, 1)[0]

    def ray(self, A, B):
        """FUN_00123790 on the gathered soup."""
        uc = self.uc
        pa, pb = COLL_MAGIC + 0x240, COLL_MAGIC + 0x250
        oi, od = COLL_MAGIC + 0x260, COLL_MAGIC + 0x264
        uc.mem_write(pa, struct.pack("<4f", A[0], A[1], A[2], 1.0))
        uc.mem_write(pb, struct.pack("<4f", B[0], B[1], B[2], 1.0))
        hit = self.call(COLL_RAY, [pa, pb, oi, od], esi=ev.VEHICLE) & 0xFF
        idx = struct.unpack("<i", uc.mem_read(oi, 4))[0]
        dist = struct.unpack("<f", uc.mem_read(od, 4))[0]
        grip = struct.unpack("<f", uc.mem_read(ev.SCRATCH + 0x324, 4))[0]
        nrm = typ = None
        if idx >= 0:
            rec = COLL_SOUP_RECS + idx * 0x40
            nrm = struct.unpack("<3f", uc.mem_read(rec + 0x30, 12))
            typ = struct.unpack("<H",
                                uc.mem_read(COLL_SOUP_TYPS + idx * 2, 2))[0]
        return hit, idx, dist, nrm, typ, grip


def _v3sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _v3cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _v3dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def model_ray_tri(A, B, v0, v1, v2):
    """Mirror of FUN_001b2230 (also the C b3_ground_probe core)."""
    d = _v3sub(B, A)
    e1 = _v3sub(v1, v0)
    e2 = _v3sub(v2, v0)
    P = _v3cross(d, e2)
    det = _v3dot(e1, P)
    if not det > COLL_DET_EPS:
        return None
    lo, hi = det * COLL_LO_K, det * COLL_HI_K
    T = _v3sub(A, v0)
    u = _v3dot(T, P)
    if not lo < u <= hi:
        return None
    Q = _v3cross(T, e1)
    v = _v3dot(d, Q)
    if not lo < v:
        return None
    if u + v > hi:
        return None
    t = _v3dot(e2, Q)
    if not lo < t <= hi:
        return None
    return t / det


def model_probe(tris, A, B):
    """Mirror of FUN_00123790 (no crash-mode filter): min parametric t."""
    best_t, best, grip = 999.0, -1, 1.2
    for i, (v0, v1, v2, styp, fl) in enumerate(tris):
        if fl:                       # gather callback excludes these types
            continue
        t = model_ray_tri(A, B, v0, v1, v2)
        if t is not None and t < best_t:
            best_t, best = t, i
        if (styp & 0xFF) == 0x26:
            grip = 0.2
        elif grip == 0.2:
            grip = 1.2
    seg = _v3sub(A, B)
    return best, best_t * (_v3dot(seg, seg) ** 0.5), grip


def _tri_key(v0, v1, v2, styp):
    # The source vertices are u16/65536*1000 plus 500-unit cells.  The game
    # expands them through float32, while the Python parser uses float64, so
    # compare their shared quantized source grid rather than decimal strings.
    q = lambda v: tuple(int(round(x * 65536.0 / 1000.0)) for x in v)  # noqa: E731
    return (q(v0), q(v1), q(v2), styp)


def _route_points():
    import re
    pts = re.findall(r"\{\s*(-?[\d.]+)f,\s*(-?[\d.]+)f,\s*(-?[\d.]+)f\}",
                     open(os.path.join(os.path.dirname(__file__), "..", "src",
                                       "burnout3_track_paths.h")).read())
    return [(float(a), float(b), float(c)) for a, b, c in pts][:1029]


def _unit_of(x, z):
    """FUN_0019d7f0's four half-plane point-in-unit test, first match."""
    import extract_collision as xc
    sd = open(os.path.join(xc.TRACK_DIR, "static.dat"), "rb").read()
    st = open(os.path.join(xc.TRACK_DIR, "streamed.dat"), "rb").read()
    cnt = struct.unpack_from("<H", sd, 0x54)[0]
    tab = struct.unpack_from("<i", sd, 0x58)[0]
    for u in range(cnt):
        so, lo, ss, ls = struct.unpack_from("<iiii", sd, tab + u * 0x10)
        p = struct.unpack_from("<12f", st[lo:lo + ls], 0x70)
        if all(p[i] * x + p[4 + i] * z - p[8 + i] >= 0 for i in range(4)):
            return u
    return -1


def _compile_collision_c():
    """Compile src/burnout3_collision.c's test main; None if gcc fails."""
    import subprocess
    import tempfile
    src = os.path.join(os.path.dirname(__file__), "..", "src",
                       "burnout3_collision.c")
    out = os.path.join(tempfile.gettempdir(), "b3_collision_test")
    r = subprocess.run(["gcc", "-O2", "-std=c11", "-DB3_COLLISION_TEST_MAIN",
                        src, "-lm", "-o", out],
                       capture_output=True, text=True)
    return out if r.returncode == 0 else None


def _c_probe(binary, x, y0, y1, z, unit):
    """One probe segment (game coords) through the compiled C module."""
    import subprocess
    binp = os.path.join(os.path.dirname(__file__), "..", "build",
                        "collision.bin")
    r = subprocess.run([binary, binp], input="%f %f %f %f %d\n" %
                       (x, y0, y1, z, unit),
                       capture_output=True, text=True)
    w = r.stdout.split()
    return (int(w[0]), float(w[1]), (float(w[2]), float(w[3]), float(w[4])),
            int(w[5]))


def run_collision_cases():
    import extract_collision as xc
    fails = 0
    print("\ncollision world (streamed unit kd-soups, RE_NOTES 15):")

    # -- enum: game walker vs extractor parse, exact triangle-set equality --
    for unit in ENUM_UNITS:
        rig = CollisionRig(unit)
        game = {}
        for v0, v1, v2, styp in rig.enum_tris():
            k = _tri_key(v0, v1, v2, styp)
            game[k] = game.get(k, 0) + 1
        mine = {}
        for v0, v1, v2, styp, fl in rig.tris:
            k = _tri_key(v0, v1, v2, styp)
            mine[k] = mine.get(k, 0) + 1
        ok = game == mine
        print("  enum unit %-2d               %4d tris game / %4d parsed  %s"
              % (unit, sum(game.values()), sum(mine.values()),
                 "OK" if ok else "FAIL"))
        fails += not ok

    # -- probes: game gather+ray vs Python mirror AND the compiled C -------
    cbin = _compile_collision_c()
    if cbin is None:
        print("  (gcc failed; C-side diff will FAIL every probe)")
    cl = _route_points()
    rigs = {}
    for name, frac, dy, lat in PROBE_CASES:
        if frac is None:
            x, y, z = LOW_ROAD_PROBE
        else:
            x, y, z = cl[int(frac * len(cl))]
            if lat:
                # lateral offset perpendicular to the local route direction
                nx, _, nz = cl[(int(frac * len(cl)) + 1) % len(cl)]
                dx, dz = nx - x, nz - z
                L = (dx * dx + dz * dz) ** 0.5
                x, z = x + (-dz / L) * lat, z + (dx / L) * lat
            y += dy
        unit = _unit_of(x, z)
        if unit < 0:
            print("  %-24s no unit -- FAIL" % name)
            fails += 1
            continue
        if unit not in rigs:
            rigs[unit] = CollisionRig(unit)
        rig = rigs[unit]
        n, soup_full = rig.gather(x, y - 1.5, z, 8.0)
        A, B = (x, y + 2.0, z), (x, y - 5.0, z)
        hit, idx, dist, nrm, typ, grip = rig.ray(A, B)
        mbest, mdist, mgrip = model_probe(rig.tris, A, B)
        # FUN_0010A8E0 retains the first 96 records and sets 0x478A30 on a
        # later append attempt.  A full soup is valid; the ray comparison
        # below proves whether the retained winner still agrees.
        ok = (hit != 0) == (mbest >= 0) and n <= 0x60
        detail = "miss==miss"
        if hit and mbest >= 0:
            v0, v1, v2, mtyp, fl = rig.tris[mbest]
            e1, e2 = _v3sub(v1, v0), _v3sub(v2, v0)
            mn = _v3cross(e1, e2)
            L = _v3dot(mn, mn) ** 0.5
            mn = (mn[0] / L, mn[1] / L, mn[2] / L)
            ok = (ok and abs(dist - mdist) < 1e-3 * max(1.0, dist)
                  and mtyp == typ
                  and all(abs(mn[i] - nrm[i]) < 1e-3 for i in range(3))
                  and abs(grip - mgrip) < 1e-6)
            detail = ("h=%.3f n=(%.2f,%.2f,%.2f) typ=%#x"
                      % (dist, nrm[0], nrm[1], nrm[2], typ))
        # C side: same segment through src/burnout3_collision.c (its loader
        # mirrors into GL space; the test main converts back).  The C world
        # is the UNION of all units, so on an agreed game-miss it may still
        # find a surface owned by a neighbouring unit -- assert C agreement
        # only on game hits, where the winner must be identical.
        if cbin is None:
            ok = False
        elif hit:
            chit, cy, cn, ctyp = _c_probe(cbin, x, y + 2.0, y - 5.0, z,
                                           unit)
            game_y = (y + 2.0) - dist
            ok = (ok and chit == 1
                  and abs(cy - game_y) < 2e-3
                  and ctyp == typ
                  and all(abs(cn[i] - nrm[i]) < 2e-3 for i in range(3)))
            detail += " C=ok" if ok else " C=DIFF(y=%.3f typ=%#x)" % (cy, ctyp)
        soup = "%d%s" % (n, "+" if soup_full else "")
        print("  probe %-18s unit %-2d soup %-3s %-36s %s"
              % (name, unit, soup, detail, "OK" if ok else "FAIL"))
        fails += not ok
    return fails


# ===========================================================================
# Crash response -- FUN_0011AEF0 (chassis-vs-world collision response) and
# its callees, ported to src/burnout3_crash.c.  Docs: RE_NOTES section 16.
#
# The REAL functions are executed whole under Unicorn against a synthetic
# vehicle + poly environment; the models below are 1:1 mirrors of the C port.
#
#   FUN_0011AEF0  response: per-poly FUN_0011AC30 accumulation over the set
#                 at veh+0x200 (+ the class-0 second set at veh+0x1590 with
#                 the -1000*mass*dir wall stop), wall path (flattened normal,
#                 1.5x edge-distance deflection, 0.707/0.1 head-on scrub,
#                 surface-grip scrub, contact impulse with torque when
#                 veh+0x1404 <= 0.1, impact magnitude to +0x194, wall-crash
#                 trigger via FUN_0010DCA0), ground path (record only).
#   FUN_0011AC30  world->body transform, sliver rejection, bbox clip
#                 (FUN_001B0C00: Sutherland-Hodgman slabs in x,z,y order),
#                 wall/ground centroid + normal accumulation.
#   FUN_00106720  impulse magnitude j = |-(1+e)dot(n,vp) /
#                 (1/m + dot(cross(Iinv*(rxn), r), n))|.
#   FUN_00106500 / FUN_001206D0  impulse application (linear + angular, with
#                 the drift-state and same-direction-yaw (|w.y|>3) locks).
#
# Constants (read from the image): -1000 [0x3B1744], 1.5 [0x3B1870],
# 0.707 [0x3B1A20], 0.1 [0x3A69C4], 0.99 [0x3B1758], 89.408 [0x3B1E18],
# 1.75 [0x3B18B8], 0.9 [0x3A69C0], 1/89.408 [0x3B1E14], 0.175 [0x3B1A68],
# 10.0 [0x3A7F34], 0.303 [0x39B308], 27.5 [0x39B2FC], 3.0 [0x3EBF64],
# 0.2 [0x3A69B4], 0.5 [0x3B1684], 0.35 [0x39B2B0], 0.7 [0x3B17D8].
# ===========================================================================
import math                                          # noqa: E402

CRASH_RESP = 0x0011AEF0
CRASH_IMPULSE = 0x00106720
CRASH_CLIP = 0x001B0C00
CRASH_ENTRY = 0x0010DCA0        # wall-crash entry -> stubbed to a marker

CR_VEH = 0x70000000             # own map: the response reads veh+0x3A50
CR_VEH_SZ = 0x4000
CR_FRAME = ev.SCRATCH + 0x50000
CR_POLYHDR = ev.SCRATCH + 0x51000
CR_POLYS = ev.SCRATCH + 0x52000
CR_FLAGS = ev.SCRATCH + 0x53000
CR_RACECAR = ev.SCRATCH + 0x54000
CR_MARK = ev.SCRATCH + 0x55000  # byte set by the CRASH_ENTRY stub
CR_BOX = ev.SCRATCH + 0x56000   # clip differential: box / tri / out
CR_NVEC = ev.SCRATCH + 0x57000  # impulse differential: n / pt / vp / out


def crash_ctx(mode=0, sub=0):
    """fake_ctx() variant with a selectable game mode/sub-mode so the
    crash-party thresholds (FUN_00017310 true) are reachable."""
    mw = fake_ctx()
    G = ev.SCRATCH + 0x7000
    VT = ev.SCRATCH + 0x7200
    ST_RM = ev.SCRATCH + 0x7340
    ST_RS = ev.SCRATCH + 0x7350
    mw[ST_RM] = b"\xb8" + struct.pack('<I', mode) + b"\xc3"
    mw[ST_RS] = b"\xb8" + struct.pack('<I', sub) + b"\xc3"
    mw[VT + 0x90] = struct.pack('<I', ST_RM)
    mw[VT + 0x94] = struct.pack('<I', ST_RS)
    # CRASH_ENTRY: mov byte [CR_MARK],1 ; ret 0xC   (stdcall, 3 args)
    mw[CRASH_ENTRY] = (b"\xc6\x05" + struct.pack('<I', CR_MARK) + b"\x01"
                       + b"\xc2\x0c\x00")
    mw[CR_MARK] = b"\x00"
    _ = G
    return mw


def run_crash(addr, img, regs=None, stack_args=(), mem_writes=None,
              max_steps=800000):
    """ev.run clone with a 0x4000-byte vehicle at its own base (FUN_0011AEF0
    reads the second-set count at veh+0x3A50, past ev.VEHICLE_SZ)."""
    from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
                         UC_PROT_ALL, UcError)
    from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, ev.ELF)
    uc.mem_map(ev.STACK_BASE, ev.STACK_SIZE, UC_PROT_ALL)
    uc.mem_map(CR_VEH, CR_VEH_SZ, UC_PROT_ALL)
    uc.mem_map(ev.SCRATCH, ev.SCRATCH_SZ, UC_PROT_ALL)
    uc.mem_map(ev.MAGIC_RET & ~0xFFF, 0x1000, UC_PROT_ALL)
    uc.mem_write(CR_VEH, bytes(img))
    for a, d in (mem_writes or {}).items():
        uc.mem_write(a, d)
    tr = ev.Tracer(uc, CR_VEH, CR_VEH_SZ)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, tr.on_unmapped)
    sp = ev.STACK_BASE + ev.STACK_SIZE - 0x2000
    uc.mem_write(sp, struct.pack('<I', ev.MAGIC_RET))
    for i, a in enumerate(stack_args):
        uc.mem_write(sp + 4 + i * 4, struct.pack('<I', a & 0xFFFFFFFF))
    uc.reg_write(UC_X86_REG_ESP, sp)
    for r, val in (regs or {}).items():
        uc.reg_write(r, val)
    err = None
    try:
        uc.emu_start(addr, ev.MAGIC_RET, count=max_steps)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
    return uc, bytes(uc.mem_read(CR_VEH, CR_VEH_SZ)), err


def pack_poly(p0, p1, p2, n):
    out = b""
    for v in (p0, p1, p2, n):
        v4 = list(v) + [0.0] * (4 - len(v))
        out += struct.pack('<4f', *v4)
    return out


def crash_vehicle(vel=(10.0, 0.0, 20.0), omega=(0.0, 0.5, 0.0), mass=1500.0,
                  pos=(0.0, 0.5, 0.0), bbmax=(0.9, 0.6, 2.2),
                  bbmin=(-0.9, -0.4, -2.2), ground_frac=1.0, drift=0,
                  authority=1.0, grip=0.97, gear=3, cls=1, flags1353=0,
                  no_scrub=0, set2=None, drift_dir=1.0):
    """Vehicle image + env for FUN_0011AEF0 (identity heading at pos)."""
    img = bytearray(CR_VEH_SZ)
    # frame (behind +0x204) and its inverse at +0x70 (identity rotation)
    frame = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0],
             [pos[0], pos[1], pos[2], 1.0]]
    inv = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0],
           [-pos[0], -pos[1], -pos[2], 1.0]]
    for i in range(4):
        struct.pack_into('<4f', img, 0x70 + i * 16, *inv[i])
    # world inverse inertia rows at +0x40
    iinv = [[1e-3, 0, 0, 0], [0, 1.2e-3, 0, 0], [0, 0, 8e-4, 0]]
    for i in range(3):
        struct.pack_into('<4f', img, 0x40 + i * 16, *iinv[i])
    sp = math.sqrt(sum(c * c for c in vel))
    d = [c / sp for c in vel] if sp > 1e-6 else [0.0, 0.0, 1.0]
    struct.pack_into('<4f', img, 0xB0, vel[0], vel[1], vel[2], sp)
    struct.pack_into('<4f', img, 0xC0, d[0], d[1], d[2], 0.25)
    struct.pack_into('<4f', img, 0xD0, omega[0], omega[1], omega[2], 0.0)
    struct.pack_into('<4f', img, 0xF0, 0.5, 1.0, 1.5, 2.0)     # acc seed
    struct.pack_into('<4f', img, 0x1D0, *(list(bbmax) + [0.0]))
    struct.pack_into('<4f', img, 0x1E0, *(list(bbmin) + [0.0]))
    struct.pack_into('<f', img, 0x1F0, mass)
    struct.pack_into('<I', img, 0x200, CR_POLYHDR)
    struct.pack_into('<I', img, 0x204, CR_FRAME)
    struct.pack_into('<I', img, 0x13F4, CR_RACECAR)
    struct.pack_into('<f', img, 0x13A8, grip)
    struct.pack_into('<f', img, 0x1404, ground_frac)
    struct.pack_into('<f', img, 0x1434, drift_dir)
    struct.pack_into('<i', img, 0x14C8, gear)
    struct.pack_into('<i', img, 0x1524, drift)
    struct.pack_into('<f', img, 0x1534, authority)
    img[0x153E] = no_scrub
    img[0x1353] = flags1353
    img[0x210] = 1
    img[0x211] = 0
    img[0x1169] = 0
    if set2:
        for i, p in enumerate(set2):
            img[0x1590 + i * 0x40:0x15D0 + i * 0x40] = pack_poly(*p)
        struct.pack_into('<i', img, 0x3A50, len(set2))
    env = {'frame': frame, 'inv': inv, 'iinv': iinv,
           'vel': [vel[0], vel[1], vel[2], sp],
           'dir': [d[0], d[1], d[2], 0.25],
           'omega': list(omega) + [0.0], 'facc': [0.5, 1.0, 1.5, 2.0],
           'bbmax': list(bbmax) + [0.0], 'bbmin': list(bbmin) + [0.0],
           'mass': mass, 'grip': grip, 'ground_frac': ground_frac,
           'drift_dir': drift_dir, 'gear': gear, 'drift': drift,
           'authority': authority, 'no_scrub': no_scrub,
           'flags1353': flags1353, 'cls': cls, 'party': 0, 'set2': set2}
    return img, env


def crash_env_writes(polys, flags, mode=0, sub=0, racecar_class=1):
    frame = b""
    for row in ([1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0]):
        frame += struct.pack('<4f', *row)
    mw = crash_ctx(mode, sub)
    mw[CR_FRAME] = frame            # pos row written by the caller's image
    mw[CR_POLYHDR] = struct.pack('<3I', len(polys), CR_POLYS, CR_FLAGS)
    mw[CR_POLYS] = b"".join(polys) if polys else b"\x00"
    mw[CR_FLAGS] = struct.pack('<%dH' % max(1, len(flags)),
                               *(flags or [0]))
    mw[CR_RACECAR + 0x1920] = struct.pack('<i', racecar_class)
    mw[CR_RACECAR + 0x19BC] = b"\x02"
    mw[CR_RACECAR + 0x15CC] = b"\xAA"       # sentinel: overwritten on crash
    return mw


def crash_frame_writes(mw, pos):
    mw[CR_FRAME + 0x30] = struct.pack('<4f', pos[0], pos[1], pos[2], 1.0)
    return mw


# ---- 1:1 mirrors of src/burnout3_crash.c ----------------------------------
def m_dot3(a, b):
    return (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]


def m_norm4(v):
    s = 1.0 / math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return [c * s for c in v]


def m_frame_pt(m, p):
    return [((p[0] * m[0][i] + m[3][i]) + p[1] * m[1][i]) + p[2] * m[2][i]
            for i in range(4)]


def m_clip_axis(pts, axis, lo, hi):
    out = []
    pv = pts[-1][axis]
    prev = (0 if pv <= hi else 2) if lo <= pv else 1
    for i, cur_pt in enumerate(pts):
        cv = cur_pt[axis]
        cur = (0 if cv <= hi else 2) if lo <= cv else 1
        vp = pts[i - 1]
        if prev == cur:
            if cur == 0:
                out.append(list(cur_pt))
        else:
            inv = 1.0 / (cv - pv)
            if prev == 1:
                t = (lo - pv) * inv
                out.append([cur_pt[k] * t + vp[k] * (1 - t) for k in range(4)])
            elif prev == 2:
                t = (hi - pv) * inv
                out.append([cur_pt[k] * t + vp[k] * (1 - t) for k in range(4)])
            if cur == 1:
                t = (lo - pv) * inv
                out.append([cur_pt[k] * t + vp[k] * (1 - t) for k in range(4)])
            elif cur == 2:
                t = (hi - pv) * inv
                out.append([cur_pt[k] * t + vp[k] * (1 - t) for k in range(4)])
            else:
                out.append(list(cur_pt))
        prev, pv = cur, cv
    return out


def m_box_clip(bbmax, bbmin, tri):
    a = m_clip_axis(tri, 0, bbmin[0], bbmax[0])
    if len(a) < 3:
        return []
    b = m_clip_axis(a, 2, bbmin[2], bbmax[2])
    if len(b) < 3:
        return []
    c = m_clip_axis(b, 1, bbmin[1], bbmax[1])
    return c if len(c) >= 3 else []


def m_poly_contact(poly, flags, acc):
    p0, p1, p2, n = poly
    v = [m_frame_pt(acc['inv'], list(p) + [0.0] * (4 - len(p)))
         for p in (p0, p1, p2)]
    if abs(n[1]) < 0.2:
        ex, ey, ez = (v[0][k] - v[1][k] for k in range(3))
        if abs(ey) < 0.5 and ez * ez + ex * ex < 0.2:
            return
        for a, c in ((v[0], v[2]), (v[1], v[2])):
            if (abs(c[1] - a[1]) < 0.5 and (c[2] - a[2]) ** 2
                    + (c[0] - a[0]) ** 2 < 0.2):
                return
    clip = m_box_clip(acc['bbmax'], acc['bbmin'], v)
    if not clip:
        return
    n4 = list(n) + [0.0] * (4 - len(n))
    cent = [sum(p[k] for p in clip) / len(clip) for k in range(4)]
    if n[1] > 0.7:
        if 0.35 < cent[1]:
            return
        acc['gnd_n'] = [acc['gnd_n'][k] + n4[k] for k in range(4)]
        acc['gnd_cent'] = [acc['gnd_cent'][k] + cent[k] for k in range(4)]
        acc['gnd_count'] += 1
    else:
        acc['nmin'] = [min(acc['nmin'][k], n4[k]) for k in range(4)]
        acc['nmax'] = [max(acc['nmax'][k], n4[k]) for k in range(4)]
        acc['wall_cent'] = [acc['wall_cent'][k] + cent[k] for k in range(4)]
        acc['wall_count'] += 1
    if (acc['flags'] & 0xFF) == 0 or (flags & 0xFF) < (acc['flags'] & 0xFF):
        acc['flags'] = flags


def m_point_velocity(env, pt):
    r = [pt[k] - env['frame'][3][k] for k in range(3)]
    o = env['omega']
    return [(o[1] * r[2] - o[2] * r[1]) + env['vel'][0],
            (o[2] * r[0] - o[0] * r[2]) + env['vel'][1],
            (o[0] * r[1] - o[1] * r[0]) + env['vel'][2],
            env['vel'][3]]


def m_impulse(env, n, pt, vp, e=0.0):
    r = [pt[k] - env['frame'][3][k] for k in range(3)]
    c = [r[1] * n[2] - r[2] * n[1], r[2] * n[0] - r[0] * n[2],
         r[0] * n[1] - r[1] * n[0]]
    I = env['iinv']
    t = [I[0][i] * c[0] + I[1][i] * c[1] + I[2][i] * c[2] for i in range(3)]
    j = abs((0.0 - (e + 1.0) * (n[0] * vp[0] + n[1] * vp[1] + n[2] * vp[2]))
            / (1.0 / env['mass'] + (t[1] * r[2] - t[2] * r[1]) * n[0]
               + (t[2] * r[0] - t[0] * r[2]) * n[1]
               + (t[0] * r[1] - t[1] * r[0]) * n[2]))
    return j, [n[k] * j for k in range(4)]


def m_apply_impulse(env, imp, pt, res):
    """FUN_001206D0 -> FUN_00106500 routing into res['imp']/res['ang']."""
    linear_only = env['drift'] in (1, 2)
    if not linear_only and abs(env['omega'][1]) > 3.0:
        t = ((pt[2] - env['frame'][3][2]) * imp[0]
             - (pt[0] - env['frame'][3][0]) * imp[2])
        if (math.copysign(1.0, env['omega'][1])
                == math.copysign(1.0, t)):
            linear_only = True
    res['imp'] = [res['imp'][k] + imp[k] for k in range(4)]
    if not linear_only:
        r = [pt[k] - env['frame'][3][k] for k in range(3)]
        res['ang'][0] += r[1] * imp[2] - r[2] * imp[1]
        res['ang'][1] += r[2] * imp[0] - r[0] * imp[2]
        res['ang'][2] += r[0] * imp[1] - r[1] * imp[0]


def m_response(env, polys, flagvals):
    res = {'state': 0, 'pt': [0.0] * 4, 'n': [0.0] * 4, 'surface': 0,
           'impact': 0.0, 'defl': [0.0] * 4, 'vel': list(env['vel']),
           'imp': [0.0] * 4, 'ang': [0.0] * 4, 'facc': list(env['facc']),
           'drift_dir': env['drift_dir'], 'crashed': 0, 'bit15': None,
           'ret': 0}
    if env['flags1353'] & 5:
        return res
    acc = {'inv': env['inv'], 'bbmax': env['bbmax'], 'bbmin': env['bbmin'],
           'wall_cent': [0.0] * 4, 'gnd_cent': [0.0] * 4, 'gnd_n': [0.0] * 4,
           'nmin': [0.0] * 4, 'nmax': [0.0] * 4,
           'wall_count': 0, 'gnd_count': 0, 'flags': 0}
    for poly, fl in zip(polys, flagvals):
        m_poly_contact(poly, fl, acc)
    if env['cls'] == 0 and env['set2']:
        pre = acc['wall_count']
        for poly in env['set2']:
            m_poly_contact(poly, 0x20, acc)
        if acc['wall_count'] > pre:
            res['facc'] = [res['facc'][k]
                           + env['dir'][k] * env['mass'] * -1000.0
                           for k in range(4)]
    if acc['wall_count']:
        nsum = [acc['nmin'][k] + acc['nmax'][k] for k in range(4)]
        if m_dot3(nsum, nsum) < 2.3283064e-10:
            d = m_dot3(env['frame'][0], acc['nmax'])
            nsum = list(acc['nmin'] if d < 0.0 else acc['nmax'])
        cent = [c / acc['wall_count'] for c in acc['wall_cent']]
        nsum[1] = 0.0
        nsum = m_norm4(nsum)
        st = [cent[0],
              (env['bbmax'][1] - env['bbmin'][1]) * 0.33 + env['bbmin'][1],
              cent[2], cent[3]]
        res['pt'] = m_frame_pt(env['frame'], st)
        res['surface'] = acc['flags']
        res['n'] = list(nsum)
        res['state'] = 1
        ed = min(abs(cent[0] - env['bbmax'][0]),
                 abs(cent[0] - env['bbmin'][0]),
                 abs(cent[2] - env['bbmax'][2]),
                 abs(cent[2] - env['bbmin'][2]))
        dot_r = m_dot3(nsum, env['frame'][0])
        prods = [nsum[k] * env['frame'][2][k] for k in range(4)]
        dot_a = (prods[0] + prods[1]) + prods[2]
        nh = [env['frame'][2][k] * dot_a + env['frame'][0][k] * dot_r
              for k in range(4)]
        if m_dot3(nh, nh) < 2.3283064e-10:
            res['ret'] = 0
            return res
        nh = m_norm4(nh)
        res['n'] = list(nh)
        res['defl'] = [res['defl'][k] + nh[k] * (ed * 1.5) for k in range(4)]
        ddn = abs(m_dot3(env['dir'], nsum))
        headon = abs((prods[0] + prods[1]) + prods[2])
        if 0.707 < headon:
            k = 1.0 - (headon - 0.707) * 0.1
            res['vel'] = [c * k for c in res['vel']]
        if env['cls'] == 0:
            res['vel'] = [c * env['grip'] for c in res['vel']]
            if env['gear'] == -1:
                res['drift_dir'] *= -1.0
        elif not env['no_scrub']:
            res['vel'] = [c * 0.99 for c in res['vel']]
        wpt = m_frame_pt(env['frame'], cent)
        env2 = dict(env, vel=res['vel'])
        vp = m_point_velocity(env2, wpt)
        j, _ = m_impulse(env, nh, wpt, vp)
        dv = j / env['mass']
        if dv > 0.0:
            sc = min(res['vel'][3], 89.408)
            hc = min(0.9, ddn * 1.75)
            res['impact'] = (((1.0 - sc * 0.011184681) * 0.9 + (1.0 - hc))
                             * env['mass'] * dv * 0.175)
            du = m_dot3(env['frame'][1], nh)
            d2 = m_norm4([nh[k] - env['frame'][1][k] * du for k in range(4)])
            impv = [d2[k] * res['impact'] for k in range(4)]
            if env['ground_frac'] <= 0.1:
                m_apply_impulse(env, impv, wpt, res)
            else:
                res['imp'] = [res['imp'][k] + impv[k] for k in range(4)]
        fire = False
        if env['party']:
            if dv > env['authority'] * 10.0:
                fire = headon > env['authority'] * 0.303
        elif ((res['surface'] & 0xFF) != 0x20
              and not (env['flags1353'] & 8)
              and dv > env['authority'] * 27.5):
            fire = headon > env['authority'] * 0.707
        if fire and env.get('racecar_class', 1) != 2:
            res['bit15'] = (res['surface'] >> 15) & 1
            res['crashed'] = 1
        res['ret'] = acc['wall_count']
        return res
    if acc['gnd_count']:
        cent = [c / acc['gnd_count'] for c in acc['gnd_cent']]
        res['state'] = 2
        res['pt'] = m_frame_pt(env['frame'], cent)
        res['n'] = m_norm4(acc['gnd_n'])
        res['surface'] = acc['flags']
        vp = m_point_velocity(env, res['pt'])
        res['impact'], _ = m_impulse(env, res['n'], res['pt'], vp)
    return res


def cr_close(a, b, tol=2e-3):
    return abs(a - b) <= tol * max(1.0, abs(a), abs(b))


# a tall wall crossing the car's nose plane; apex offset so the clipped
# centroid sits off-axis (nonzero yaw torque from the impulse)
def wall_tri(z=1.8, nx=0.0, ny=0.0, nz=-1.0, xoff=0.0):
    return ((-4.0 + xoff, 0.0, z), (4.0 + xoff, 0.0, z),
            (1.5 + xoff, 4.0, z), (nx, ny, nz))


def floor_tris(y=0.15):
    return [((-3.0, y, -3.0), (3.0, y, -3.0), (0.0, y, 4.0),
             (0.0, 1.0, 0.0))]


RESP_CASES = [
    # name, vehicle-kwargs, polys, flags, mode/sub, racecar_class, expect
    ("wall grounded linear",
     dict(ground_frac=1.0, authority=100.0), [wall_tri()], [0x0007],
     (0, 0), 1, dict(state=1, ang_zero=True)),
    ("wall airborne tumble",
     dict(ground_frac=0.0, omega=(0.0, -0.5, 0.0), authority=100.0),
     [wall_tri(xoff=2.0)], [0x0007], (0, 0), 1,
     dict(state=1, ang_zero=False)),
    ("wall airborne yaw-lock",
     dict(ground_frac=0.0, omega=(0.0, -5.0, 0.0), authority=100.0),
     [wall_tri(xoff=2.0)], [0x0007], (0, 0), 1,
     dict(state=1, ang_zero=True)),
    ("wall drifting linear",
     dict(ground_frac=0.0, drift=1, authority=100.0),
     [wall_tri(xoff=2.0)], [0x0007], (0, 0), 1,
     dict(state=1, ang_zero=True)),
    ("head-on crash trigger",
     dict(vel=(0.0, 0.0, 30.0), authority=0.1), [wall_tri()], [0x8041],
     (0, 0), 1, dict(state=1, crashed=1)),
    ("class-0 grip + wallstop",
     dict(cls=0, gear=-1, grip=0.9, authority=100.0,
          set2=[wall_tri()]),
     [], [], (0, 0), 0, dict(state=1, wallstop=True)),
    ("party-mode thresholds",
     dict(vel=(0.0, 0.0, 12.0), authority=0.05), [wall_tri()], [0x0020],
     (6, 3), 1, dict(state=1, crashed=1)),
    ("ground contact records",
     dict(vel=(10.0, -2.0, 20.0)), floor_tris(), [0x0009], (0, 0), 1,
     dict(state=2)),
    ("no contact",
     dict(), [wall_tri(z=50.0)], [0x0007], (0, 0), 1, dict(state=0)),
    ("bail on +0x1353 & 5",
     dict(flags1353=1), [wall_tri()], [0x0007], (0, 0), 1,
     dict(state=0, untouched=True)),
]


def run_crash_response_cases():
    fails = 0
    print("\ncrash response (FUN_0011AEF0 whole-function differential):")
    for name, kw, tris, flags, (mode, sub), rclass, expect in RESP_CASES:
        cls = kw.get('cls', 1)
        img, env = crash_vehicle(**kw)
        env['party'] = 1 if mode == 6 else 0
        env['racecar_class'] = rclass
        polys = [pack_poly(*t) for t in tris]
        mw = crash_env_writes(polys, flags, mode, sub, rclass)
        crash_frame_writes(mw, (0.0, 0.5, 0.0))
        uc, after, err = run_crash(CRASH_RESP, img,
                                   regs={UC_X86_REG_ECX: CR_VEH},
                                   mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        m = m_response(env, tris, flags)
        got_state = struct.unpack_from('<i', after, 0x198)[0]
        got_pt = struct.unpack_from('<4f', after, 0x160)
        got_n = struct.unpack_from('<4f', after, 0x170)
        got_surface = struct.unpack_from('<H', after, 0x190)[0]
        got_impact = struct.unpack_from('<f', after, 0x194)[0]
        got_defl = struct.unpack_from('<4f', after, 0x130)
        got_vel = struct.unpack_from('<4f', after, 0xB0)
        got_imp = struct.unpack_from('<4f', after, 0x110)
        got_ang = struct.unpack_from('<4f', after, 0x120)
        got_facc = struct.unpack_from('<4f', after, 0xF0)
        got_dd = struct.unpack_from('<f', after, 0x1434)[0]
        got_mark = uc.mem_read(CR_MARK, 1)[0]
        got_15cc = uc.mem_read(CR_RACECAR + 0x15CC, 1)[0]
        got_ret = struct.unpack('<i', struct.pack('<I',
                  uc.reg_read(UC_X86_REG_EAX)))[0]

        bad = []
        if got_state != m['state']:
            bad.append("state %d!=%d" % (got_state, m['state']))
        if m['state'] != 0:
            if not all(cr_close(got_pt[k], m['pt'][k]) for k in range(3)):
                bad.append("pt %s" % (tuple(got_pt[:3]),))
            if not all(cr_close(got_n[k], m['n'][k], 1e-4)
                       for k in range(3)):
                bad.append("n %s" % (tuple(got_n[:3]),))
            if got_surface != m['surface']:
                bad.append("surface %#x!=%#x" % (got_surface, m['surface']))
            if not cr_close(got_impact, m['impact']):
                bad.append("impact %.2f!=%.2f" % (got_impact, m['impact']))
        for gname, got, want in (("defl", got_defl, m['defl']),
                                 ("vel", got_vel, m['vel']),
                                 ("imp", got_imp, m['imp']),
                                 ("ang", got_ang, m['ang']),
                                 ("facc", got_facc, m['facc'])):
            if not all(cr_close(got[k], want[k]) for k in range(4)):
                bad.append("%s %s != %s"
                           % (gname, ["%.3f" % x for x in got],
                              ["%.3f" % x for x in want]))
        if not cr_close(got_dd, m['drift_dir'], 1e-6):
            bad.append("drift_dir %.2f" % got_dd)
        if got_mark != m['crashed']:
            bad.append("crash-entry %d!=%d" % (got_mark, m['crashed']))
        if m['bit15'] is not None and got_15cc != m['bit15']:
            bad.append("+0x15CC %d!=%d" % (got_15cc, m['bit15']))
        if got_ret != m['ret']:
            bad.append("ret %d!=%d" % (got_ret, m['ret']))
        # structural expectations (guards against a silently-degenerate case)
        if 'state' in expect and got_state != expect['state']:
            bad.append("case degenerated: state %d, expected %d"
                       % (got_state, expect['state']))
        if 'ang_zero' in expect:
            nz = any(abs(a) > 1e-3 for a in got_ang[:3])
            if expect['ang_zero'] == nz:
                bad.append("ang_imp %s but case expects %s"
                           % ("nonzero" if nz else "zero",
                              "zero" if expect['ang_zero'] else "nonzero"))
        if expect.get('crashed') and got_mark != 1:
            bad.append("expected the crash entry to fire")
        if expect.get('wallstop'):
            want0 = env['facc'][0] + env['dir'][0] * env['mass'] * -1000.0
            if not cr_close(got_facc[0], want0):
                bad.append("wall-stop force missing")
        if expect.get('untouched'):
            if any(abs(c) > 0 for c in got_imp[:3]):
                bad.append("bail path applied an impulse")
        if got_state == 2:
            dup1 = struct.unpack_from('<4f', after, 0x1A0)
            dup2 = struct.unpack_from('<4f', after, 0x1B0)
            if not (dup1 == got_pt and dup2 == got_pt):
                bad.append("+0x1A0/+0x1B0 duplicates differ")
        ok = not bad
        print("  %-24s state %d impact %9.2f  %s"
              % (name, got_state, got_impact,
                 "OK" if ok else "FAIL " + "; ".join(bad)))
        fails += not ok
    return fails


CLIP_CASES = [
    ("tri fully inside", ((-0.5, 0.0, -1.0), (0.5, 0.0, -1.0),
                          (0.0, 0.3, 1.0)), 3),
    ("tri clipped",      ((-4.0, -0.5, 1.8), (4.0, -0.5, 1.8),
                          (1.5, 3.5, 1.8)), None),
    ("tri outside",      ((-4.0, 5.0, 1.8), (4.0, 5.0, 1.8),
                          (1.5, 9.0, 1.8)), 0),
]


def run_crash_clip_cases():
    fails = 0
    print("\nbbox triangle clipper (FUN_001B0C00, x/z/y slabs):")
    bbmax = [0.9, 0.6, 2.2, 0.0]
    bbmin = [-0.9, -0.4, -2.2, 0.0]
    for name, tri, want_n in CLIP_CASES:
        img = bytearray(CR_VEH_SZ)
        mw = crash_ctx()
        mw[CR_BOX] = struct.pack('<4f', *bbmax) + struct.pack('<4f', *bbmin)
        tri4 = b""
        for v in tri:
            tri4 += struct.pack('<4f', v[0], v[1], v[2], 1.0)
        mw[CR_BOX + 0x40] = tri4
        mw[CR_BOX + 0x100] = b"\x00" * 0x120
        uc, after, err = run_crash(
            CRASH_CLIP, img,
            regs={UC_X86_REG_ESI: CR_BOX, UC_X86_REG_EAX: CR_BOX + 0x40,
                  UC_X86_REG_EDI: CR_BOX + 0x100},
            mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        got_n = uc.reg_read(UC_X86_REG_EAX) & 0xFFFFFFFF
        tri_l = [list(v) + [1.0] for v in tri]
        m = m_box_clip(bbmax, bbmin, tri_l)
        ok = got_n == len(m) and (want_n is None or got_n == want_n)
        if ok and got_n:
            got_v = [struct.unpack('<4f',
                     uc.mem_read(CR_BOX + 0x100 + 16 * i, 16))
                     for i in range(got_n)]
            ok = all(cr_close(got_v[i][k], m[i][k], 1e-4)
                     for i in range(got_n) for k in range(3))
        print("  %-24s verts %d (model %d)  %s"
              % (name, got_n, len(m), "OK" if ok else "FAIL"))
        fails += not ok
    return fails


IMP_CASES = [
    ("head-on stop",  (0.0, 0.0, -1.0), (0.0, 0.6, 1.9), (0.0, 0.0, 25.0)),
    ("offset corner", (-0.6, 0.0, -0.8), (0.8, 0.2, 2.0), (8.0, -1.0, 18.0)),
]


def run_crash_impulse_cases():
    fails = 0
    print("\ncontact impulse magnitude (FUN_00106720):")
    from unicorn.x86_const import UC_X86_REG_XMM0
    for name, n, pt, vp in IMP_CASES:
        img, env = crash_vehicle()
        mw = crash_env_writes([], [], 0, 0, 1)
        crash_frame_writes(mw, (0.0, 0.5, 0.0))
        mw[CR_NVEC] = struct.pack('<4f', n[0], n[1], n[2], 0.0)
        mw[CR_NVEC + 0x10] = struct.pack('<4f', pt[0], pt[1], pt[2], 1.0)
        mw[CR_NVEC + 0x20] = struct.pack('<4f', vp[0], vp[1], vp[2], 0.0)
        mw[CR_NVEC + 0x30] = b"\x00" * 16
        uc, after, err = run_crash(
            CRASH_IMPULSE, img,
            regs={UC_X86_REG_EAX: CR_VEH, UC_X86_REG_ECX: CR_NVEC,
                  UC_X86_REG_EDX: CR_NVEC + 0x10},
            stack_args=[CR_NVEC + 0x10, CR_NVEC + 0x20, 0,
                        CR_NVEC + 0x30],
            mem_writes=mw)
        if err:
            print("  %-24s FAULT %s" % (name, err))
            fails += 1
            continue
        got_j = u2f(uc.reg_read(UC_X86_REG_XMM0) & 0xFFFFFFFF)
        got_out = struct.unpack('<4f', uc.mem_read(CR_NVEC + 0x30, 16))
        mj, mout = m_impulse(env, list(n) + [0.0], list(pt) + [1.0],
                             list(vp) + [0.0])
        ok = (cr_close(got_j, mj)
              and all(cr_close(got_out[k], mout[k]) for k in range(3)))
        print("  %-24s j %10.2f (model %10.2f)  %s"
              % (name, got_j, mj, "OK" if ok else "FAIL"))
        fails += not ok
    return fails


def main():
    fails = 0
    counts = []
    for fn, n in ((run_award_cases, len(AWARD_CASES)),
                  (run_update_cases, len(UPDATE_CASES)),
                  (run_engage_cases, len(ENGAGE_CASES)),
                  (run_tier_cases, len(TIER_CASES)),
                  (run_air_case, 1),
                  (run_takedown_cases, 3),
                  (run_expiry_case, 1),
                  (run_upgrade_cases, 2),
                  (run_steer_cases, len(STEER_CASES)),
                  (run_ooc_cases, len(OOC_CASES)),
                  (run_param_checks, 8),
                  (run_scan_cases, len(SCAN_CASES)),
                  (run_distrib_cases, len(DISTRIB_CASES)),
                  (run_crashseq_cases, len(CRASHSEQ_CASES)),
                  (run_glass_cases, len(GLASS_CASES)),
                  (run_ai_driver_cases, len(AI_DRIVER_CASES)),
                  (run_collision_cases,
                   len(ENUM_UNITS) + len(PROBE_CASES)),
                  (run_crash_response_cases, len(RESP_CASES)),
                  (run_crash_clip_cases, len(CLIP_CASES)),
                  (run_crash_impulse_cases, len(IMP_CASES))):
        f = fn()
        fails += f
        counts.append(n)
    total = sum(counts)
    print("\n%d/%d gameplay cases match the real code "
          "(%d boost award, %d meter update, %d engage, %d tier, 1 air, "
          "3 takedown, 1 window expiry, 2 bar upgrade, %d steer-away, "
          "%d out-of-control, 8 parameter, %d damage scan, %d distributor, "
          "%d crash sequencer, %d glass, %d AI driver, %d collision, "
          "%d crash response, %d clip, %d impulse)"
          % (total - fails, total, counts[0], counts[1], counts[2], counts[3],
             counts[8], counts[9], counts[11], counts[12], counts[13],
             counts[14], counts[15], counts[16], counts[17], counts[18],
             counts[19]))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
