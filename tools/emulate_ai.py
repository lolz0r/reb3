#!/usr/bin/env python3
"""Ground truth for the Burnout 3 AI driver chain: runs the REAL x86 of the
opponent-AI functions under Unicorn, the same way tools/emulate_vehicle.py
does for the physics.

The AI object.  Every function in the chain is a method on ONE object that
lives INSIDE the racecar at `racecar + 0x1A00` (proved by the navigator
constructor FUN_001705F0, which writes the aggregate back-pointers
`*(rc+0x1A04) = rc`, `*(rc+0x21A0) = rc`, `*(rc+0x2160) = rc` -- exactly the
`+0x004` / `+0x7A0` / `+0x760` slots the AI code dereferences).  That single
fact is what turns the previously "unlocated AI target writers" into ordinary
code: the driver FUN_00105340 reads `racecar+0x23C0/+0x23C4`, which is
`AI+0x9C0/+0x9C4`, and those ARE written by name in FUN_00171E30 /
FUN_001724F0.

Chain (per AI car, per frame), from FUN_00170820 -> FUN_00171A10:
    FUN_00173690   speed cap  AI+0xA08
    FUN_0016AAC0   arbitrator -> AI+0x770 desired dir, +0x780 max speed,
                   +0x784 time-to-target      (calls FUN_00175B10 target
                   follower + FUN_0016C450 avoidance; FUN_0016AE20 commits)
    AI+0x7B0 := normalize(AI+0x770);  AI+0x9C8 := AI+0x784
    FUN_00171E30   -> AI+0x9C0 TARGET STEERING ANGLE (deg), AI+0x9D4 error
    FUN_00171BE0   drift decision
    FUN_001724F0   -> AI+0x9C4 TARGET SPEED (m/s)   (FUN_00172E80 corner law,
                   FUN_00172FA0 aggression match, FUN_001734C0 catch-up)
    FUN_00171D90
then the physics-side driver FUN_00105340 turns those two numbers into
throttle/brake/steer/boost inputs.

Run standalone for a smoke test:  python3 tools/emulate_ai.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
                     UcError, UC_PROT_ALL)
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_EIP,
                               UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_XMM0)

import emulate_vehicle as ev                                  # noqa: E402

ELF = ev.ELF
PAGE = 0x1000

# --- memory map (well clear of the 0x10000..0x778200 image) -----------------
STACK_BASE = 0x20000000
STACK_SIZE = 0x00100000
VEH = 0x30000000            # physics vehicle  (racecar+0x2440 -> here)
VEH_SZ = 0x2000
RC = 0x40000000             # racecar object; AI object = RC + AI_OFF
RC_SZ = 0x10000
AI_OFF = 0x1A00
FRAME = 0x60000000          # RenderWare frame behind vehicle+0x204
FRAME_SZ = 0x1000
OTHER = 0x70000000          # second racecar / misc pointer targets
OTHER_SZ = 0x10000
MAGIC_RET = 0x50000000

AI = RC + AI_OFF

# --- function addresses (analyzed burnout3.elf; .text = flat + 0x10000) -----
F_TARGET_ANGLE = 0x00171E30      # -> AI+0x9C0 target steering angle (deg)
F_CORNER_SPEED = 0x00172E80      # corner speed law (returns XMM0)
F_TARGET_SPEED = 0x001724F0      # -> AI+0x9C4 target speed (m/s)
F_SPEED_CAP = 0x00173690         # -> AI+0xA08
F_COMMIT_TARGET = 0x0016AE20     # -> AI+0x770 dir, +0x784 time-to-target
F_DRIVER = 0x00105340            # the AI racer driver (physics inputs)
F_TRAFFIC_DRIVER = 0x00105150    # the reduced traffic driver
F_BRAKE_HELPER = 0x00104CA0
F_OOR_MOVER_GOV = 0x00171078     # governor tail of the out-of-range mover
F_NORMALIZE = 0x00011640
F_NORMALIZE_LEN = 0x0002C0D0

# --- globals the chain reads ------------------------------------------------
G_DT = 0x0060EA1C                # frame dt
G_CLOCK = 0x0060EA20             # race clock
G_ONE = 0x003B168C               # 1.0
G_BOOST_SCALE = 0x003A2AB4       # 1.45
G_TRAFFIC_CAP = 0x005A9770       # traffic-class target-speed cap
G_STEER_CUT = 0x003B1728         # 0.125 steer-error speed cut
G_CATCHUP_TOL = 0x003B1E04       # 0.0010309278 governor catch-up factor
G_BRAKE_MS_A = 0x005A39EC        # 13.4112 (30 mph) driver brake threshold
G_BRAKE_MS_B = 0x005A3A10
G_SLEW_WIND = 0x00754B28         # 2.4  deg/frame away from zero
G_SLEW_UNWIND = 0x00754B24       # 8.1  deg/frame toward zero
G_SLEW_INIT = 0x00754B2C         # lazy-init bitmask for the two above
G_GAIN_LO = 0x0041A510           # 0.01 yaw-rate gain (winding up)
G_GAIN_HI = 0x0041A50C           # 0.04 yaw-rate gain (unwinding)
G_GAIN_SIGN = 0x0041A508         # 1.0  opposite-sign multiplier (no-op)

# --- AI config struct (registrar FUN_0016AFD0, static 0x0047A140) ----------
P_OOR_DEC = 0x0047A140           # AI/Car Out of range speed decrease rate
P_OOR_ANG = 0x0047A144           # AI/Car Max desDir angle change OOR
P_ANGLE_MIN_SPD = 0x0047A148     # AI/Car Angle you want min spd at
P_TOP_SPEED = 0x0047A14C         # AI/Car Top speed mps
P_MIN_SPEED = 0x0047A150         # AI/Car Min speed mps
P_CARAT = 0x0047A154             # AI/Car How much carAt affects steering
P_DRIFT_ANGLE = 0x0047A158       # AI/Car Angle at which drift is started
P_MAX_LOCK = 0x0047A15C          # AI/Car Max lock at 180 x degrees
P_DRIFT_MAX_LOCK = 0x0047A160    # AI/Car Drift Max lock at 180 x degrees

# VDB-tuned values (docs/RE_AI.md section 1; retail Data/vdb.xml column)
VDB = {
    P_OOR_DEC: 10.0, P_OOR_ANG: 1.0, P_ANGLE_MIN_SPD: 90.0,
    P_TOP_SPEED: 88.0, P_MIN_SPEED: 20.0, P_CARAT: 0.9,
    P_DRIFT_ANGLE: 20.0, P_MAX_LOCK: 10.0, P_DRIFT_MAX_LOCK: 50.0,
}

SLEW_WIND = 2.4
SLEW_UNWIND = 8.1
BRAKE_MS = 13.4112               # .data 0x3B1A5C = exactly 30 mph
DT = 1.0 / 60.0


def f2u(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def u2f(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]


class Session(object):
    """One Unicorn instance with the racecar / AI / vehicle / frame laid out."""

    def __init__(self, clock=10.0, dt=DT, params=None):
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        ev.load_elf(uc, ELF)
        for base, size in ((STACK_BASE, STACK_SIZE), (VEH, VEH_SZ),
                           (RC, RC_SZ), (FRAME, FRAME_SZ), (OTHER, OTHER_SZ),
                           (MAGIC_RET & ~(PAGE - 1), PAGE)):
            uc.mem_map(base, size, UC_PROT_ALL)
        self.uc = uc
        self.fault = None
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)

        # identity frame behind vehicle+0x204
        for i, v in enumerate([1, 0, 0, 0, 0, 1, 0, 0,
                               0, 0, 1, 0, 0, 0, 0, 1]):
            self.wf(FRAME + i * 4, float(v))

        # wire the object graph exactly as the constructors do
        self.wu(VEH + 0x204, FRAME)
        self.wu(VEH + 0x1568, RC)        # driver's racecar pointer
        self.wu(VEH + 0x13F4, RC)
        self.wu(VEH + 0x13F8, OTHER)
        self.wu(RC + 0x2440, VEH)        # racecar -> physics vehicle
        self.wu(RC + 0x1A04, RC)         # FUN_001705F0 back-pointers
        self.wu(RC + 0x1A00, AI)
        self.wu(RC + 0x1A08, AI)
        self.wu(RC + 0x2150, AI)
        self.wu(RC + 0x2154, RC)
        self.wu(RC + 0x2158, AI)
        self.wu(RC + 0x2160, RC)
        self.wu(RC + 0x2164, AI)
        self.wu(RC + 0x21A0, RC)
        self.wu(RC + 0x1198, RC)

        self.wf(G_DT, dt)
        self.wf(G_CLOCK, clock)
        self.wf(G_ONE, 1.0)
        self.wf(G_BOOST_SCALE, 1.45)
        self.wf(G_STEER_CUT, 0.125)
        self.wf(G_CATCHUP_TOL, 0.0010309278732165694)
        self.wf(G_BRAKE_MS_A, BRAKE_MS)
        self.wf(G_BRAKE_MS_B, BRAKE_MS)
        self.wf(G_GAIN_LO, 0.01)
        self.wf(G_GAIN_HI, 0.04)
        self.wf(G_GAIN_SIGN, 1.0)
        # the two slew limits are lazy-initialised statics; pre-arm them so the
        # first call sees the same values every subsequent call sees
        self.wf(G_SLEW_WIND, SLEW_WIND)
        self.wf(G_SLEW_UNWIND, SLEW_UNWIND)
        self.wu(G_SLEW_INIT, 3)

        for a, v in (params if params is not None else VDB).items():
            self.wf(a, v)

    # --- memory helpers ---
    def _unmapped(self, uc, access, address, size, value, user):
        if self.fault is None:
            self.fault = "unmapped 0x%08X @ EIP 0x%08X" % (
                address, uc.reg_read(UC_X86_REG_EIP))
        return False

    def wf(self, addr, val):
        self.uc.mem_write(addr, struct.pack('<f', float(val)))

    def wu(self, addr, val):
        self.uc.mem_write(addr, struct.pack('<I', int(val) & 0xFFFFFFFF))

    def wi(self, addr, val):
        self.uc.mem_write(addr, struct.pack('<i', int(val)))

    def wb(self, addr, val):
        self.uc.mem_write(addr, bytes([int(val) & 0xFF]))

    def wv(self, addr, vec):
        for i, c in enumerate(vec):
            self.wf(addr + i * 4, c)

    def rf(self, addr):
        return struct.unpack('<f', self.uc.mem_read(addr, 4))[0]

    def ru(self, addr):
        return struct.unpack('<I', self.uc.mem_read(addr, 4))[0]

    def ri(self, addr):
        return struct.unpack('<i', self.uc.mem_read(addr, 4))[0]

    def rb(self, addr):
        return self.uc.mem_read(addr, 1)[0]

    # --- call ---
    def call(self, addr, regs=None, stack_args=(), max_steps=400000,
             frame_ebp=False):
        """Run one function; returns its XMM0 lane 0 as a float.

        frame_ebp: entering a function BELOW its prologue (the out-of-range
        mover's governor tail at 0x171078) means EBP was never set up, so its
        `mov esp,ebp; pop ebp; ret` epilogue would fault.  Point EBP at a
        synthetic frame whose saved-EBP slot is followed by the return address.
        """
        uc = self.uc
        self.fault = None
        sp = STACK_BASE + STACK_SIZE - 0x2000
        uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
        for i, a in enumerate(stack_args):
            uc.mem_write(sp + 4 + i * 4, struct.pack('<I', a & 0xFFFFFFFF))
        uc.reg_write(UC_X86_REG_ESP, sp)
        if frame_ebp:
            uc.mem_write(sp - 4, struct.pack('<I', 0))
            uc.reg_write(UC_X86_REG_EBP, sp - 4)
        for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                  UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI):
            uc.reg_write(r, AI)
        for r, v in (regs or {}).items():
            uc.reg_write(r, v & 0xFFFFFFFF)
        try:
            uc.emu_start(addr, MAGIC_RET, count=max_steps)
        except UcError as e:
            if self.fault is None:
                self.fault = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
        xmm = uc.reg_read(UC_X86_REG_XMM0)
        return u2f(xmm & 0xFFFFFFFF)


# ---------------------------------------------------------------------------
# Canonical seeding of a plausible AI race car.
# ---------------------------------------------------------------------------
def seed_car(s, pos=(0.0, 0.0, 0.0), fwd=(0.0, 0.0, 1.0),
             right=(1.0, 0.0, 0.0), speed=40.0, yaw_rate=0.0,
             car_at=None, drift_state=0, lsdm_limit=25.0, gear=3):
    """Seed racecar frame + physics vehicle for the targeting stage."""
    up = (0.0, 1.0, 0.0)
    s.wv(RC + 0x10, list(right) + [0.0])     # racecar matrix row0 = right
    s.wv(RC + 0x20, list(up) + [0.0])        # row1 = up
    s.wv(RC + 0x30, list(fwd) + [0.0])       # row2 = forward
    s.wv(RC + 0x40, list(pos) + [1.0])       # row3 = position
    # RenderWare frame behind the physics vehicle (same basis)
    s.wv(FRAME + 0x00, list(right) + [0.0])
    s.wv(FRAME + 0x10, list(up) + [0.0])
    s.wv(FRAME + 0x20, list(fwd) + [0.0])
    s.wv(FRAME + 0x30, list(pos) + [1.0])
    if car_at is None:
        car_at = fwd
    s.wv(VEH + 0xC0, list(car_at) + [0.0])   # "carAt" heading vector
    s.wf(VEH + 0xBC, speed)
    s.wf(VEH + 0x13D4, speed * 2.2369363)
    s.wf(VEH + 0xD4, yaw_rate)
    s.wi(VEH + 0x1524, drift_state)
    s.wf(VEH + 0x13AC, lsdm_limit)
    s.wi(VEH + 0x14C8, gear)
    s.wi(VEH + 0x14CC, 6)
    s.wb(VEH + 0x1550, 0)
    s.wi(VEH + 0x1558, 0)
    s.wf(VEH + 0x1570, -1.0)
    s.wf(VEH + 0x1578, -1.0)
    s.wf(VEH + 0x157C, -1.0)
    s.wf(VEH + 0x1534, 1.0)
    s.wf(VEH + 0x13E4, 1.0)
    s.wi(RC + 0x134C, 1)      # race-class car (not traffic)
    s.wi(RC + 0x1920, 1)      # normal AI mode
    s.wi(RC + 0x179C, 1)
    s.wb(RC + 0x19A8, 1)
    s.wf(RC + 0x190C, 1.0)    # crash timer clear
    s.wf(RC + 0x10DC, 10.0)
    s.wi(RC + 0x2450, 0)
    s.wi(AI + 0x1F8, 0)       # target mode
    s.wb(AI + 0x213, 0)
    s.wf(AI + 0xA08, VDB[P_TOP_SPEED])
    s.wf(AI + 0xA0C, 1e9)     # keep the catch-up branch out unless asked
    s.wb(AI + 0xA31, 1)
    s.wi(AI + 0x790, 0)       # aggression state machine idle
    return s


def main():
    s = Session()
    seed_car(s)
    s.wv(AI + 0x7B0, [0.0, 0.0, 1.0, 0.0])
    s.wf(AI + 0x9C8, 1.0)
    s.wf(AI + 0x9CC, 0.0)
    ang = s.call(F_TARGET_ANGLE, regs={UC_X86_REG_ESI: AI})
    print("straight ahead   -> target angle %.4f deg (fault %s)"
          % (s.rf(AI + 0x9C0), s.fault))
    s.wv(AI + 0x7B0, [0.5, 0.0, 0.8660254, 0.0])
    s.wf(AI + 0x9CC, 0.0)
    s.call(F_TARGET_ANGLE, regs={UC_X86_REG_ESI: AI})
    print("30 deg to right  -> target angle %.4f deg (slew-limited)"
          % s.rf(AI + 0x9C0))
    s.wf(AI + 0x780, 88.0)
    v = s.call(F_TARGET_SPEED, regs={UC_X86_REG_EAX: AI, UC_X86_REG_ESI: AI})
    print("target speed     -> %.4f m/s (fault %s)"
          % (s.rf(AI + 0x9C4), s.fault))
    _ = ang, v


if __name__ == "__main__":
    main()


# ===========================================================================
# THE AGGRESSION (attack / slam) STATE MACHINE -- FUN_00169540 family.
#
# Second session type.  Where `Session` above puts one racecar at a scratch
# address, the aggression code walks the GLOBAL racecar array at
# `DAT_0073A1D0` (stride 0x27E0) and the physics-vehicle pointer table at
# `DAT_0064B38C` (stride 0x30), so the cars have to live at their real
# addresses.  The aggression object is a sub-object of the AI object at
# AI+0x170  ==  racecar+0x1B70  (proved by FUN_00175A10 @0x00175A1F:
# `LEA ECX,[ESI+0x170]; CALL 0x00169490`, ESI = the AI/target-follower base).
# ===========================================================================
G_RC_ARRAY = 0x0073A1D0          # racecar[0]
RC_STRIDE = 0x27E0
G_RC_COUNT = 0x0073A1C0          # int: number of racecars
G_RC_PTRS = 0x0073A1A8           # racecar* by index (FUN_0016A950's rival)
G_VEH_TBL = 0x0064B38C           # [slot*0x30] -> physics vehicle*
G_SECT_COUNT = 0x0073A188        # FUN_00194380: number of route sections
G_SECT_LEN = 0x0073A184          # FUN_00194380: cumulative length table (x8)
G_LAPFLAG = 0x0073A198           # FUN_00194380: byte, 1 = ignore laps
G_TRACK_LOADED = 0x0073A164
AGG_OFF = 0x1B70                 # aggression object, from the racecar base

F_AGG_CTOR = 0x00169490          # ctor / reset          (EAX=rc, ECX=aggro)
F_AGG_UPDATE = 0x00169540        # the state machine     (EAX=aggro)
F_AGG_PICK = 0x00169BD0          # target selection      (EDI=aggro)
F_AGG_CANSLAM = 0x00169D70       # -> aggro+0x40         (ESI=aggro)
F_AGG_SLAM = 0x00169E80          # state 4 tick          (ESI=aggro)
F_AGG_APPROACH = 0x0016A0A0      # positioning aim       (ESI=aggro)
F_AGG_RESET = 0x0016A310         # -> idle               (EAX=aggro)
F_AGG_AIMOK = 0x0016A360         # aim-valid gate        (EDX=aggro)
F_AGG_BLOCKRNG = 0x0016A3E0      # -> aggro+0x41         (EAX=aggro)
F_AGG_BLOCKAIM = 0x0016A4E0      # block aim             (ESI=aggro)
F_AGG_SLAMSPD = 0x0016A620       # -> aggro+0x42         (ESI=aggro)
F_AGG_MEASURE = 0x0016A7D0       # +0x38/+0x3C/+0x40..42 (EAX=aggro)
F_AGG_BLIND = 0x0016A8C0         # rubbed-blind timer    (EAX=aggro)
F_AGG_RETAL = 0x0016A950         # rival retaliation     (ESI=aggro)
F_AGG_SETSTATE = 0x0016AF10      # -> AI+0x790/+0x794    (ECX=AI)
F_AGG_SPEED = 0x00172FA0         # aggression speed      (ESI=AI, arg=spd)
F_AGG_BOOST = 0x00171D90         # boost latch           (EDI=AI)
F_DRIFT_FLAGS = 0x00171BE0       # +0xA13/+0xA14/+0xA15  (ESI=AI)
F_LATERAL = 0x001716D0           # |lateral| (EAX=own, ECX=targ)
F_LONGIT = 0x001717B0            # signed longitudinal   (EAX=own, ECX=targ)
F_TRACKDIST = 0x00194380         # (ECX = racecar+0x10D0)

# AI/Aggressive Driving config block (registrar FUN_0016AFD0, 0x0047A140)
P_MIN_AGGRO = 0x0047A204         # Min. aggression before we start attacking
P_MIN_WAIT = 0x0047A208          # Min. time to wait between attacks (s)
P_MAX_WAIT = 0x0047A20C          # Max. time to wait between attacks (s)
P_DIST_AHEAD = 0x0047A210        # Max dist apart to begin attacking, ahead
P_DIST_BEHIND = 0x0047A214       # Max dist apart to begin attacking, behind
P_MIN_TGT_MPH = 0x0047A218       # Min. target speed to consider attacking
P_SLOW_FACTOR = 0x0047A21C       # How much slower than the player to drive
P_BOOST_DIST = 0x0047A220        # How far in front to boost
P_BOOST_AGGRO = 0x0047A224       # Extra dist in front to boost vs aggression
P_START_DELAY = 0x0047A228       # Wait at race start before aggressive drive
P_IMMUNITY = 0x0047A22C          # How long after hitting something...
P_BLOCK_MIN = 0x0047A230         # Slam: Min time to try and block you for
P_BLOCK_MAX = 0x0047A234         # Slam: Max time to try and block you for
P_BLOCK_DIST = 0x0047A238        # Slam: Max distance ahead to start blocking
P_SEPARATION = 0x0047A23C        # Slam: Preferred car separation (UNUSED)
P_POS_TIME = 0x0047A240          # Slam: Max time to get into slam position
P_AHEAD_GAP = 0x0047A244         # Slam: Max distance between cars, when ahead
P_SPD_DIFF = 0x0047A248          # Slam: Max. difference in speeds (MPH)
P_STEER_OUT_D = 0x0047A24C       # Slam: Steer out distance (meters)
P_STEER_OUT_T = 0x0047A250       # Slam: Steer out time (seconds)
P_SLAM_TIME = 0x0047A254         # Slam: Slam time (seconds)
P_MAX_COS = 0x0047A258           # Slam: Max cos angle off lane to stop attack
P_COMMIT = 0x0047A25C            # Slam: committed after ... seconds
P_STICKY_D = 0x0047A1FC          # Driver: max dist from player, sticky match
P_STICKY_MPH = 0x0047A200        # Driver: max speed diff, sticky match
P_CLOSE_MATCH = 0x0047A1F8       # Driver: how close to start speed matching
P_APEX_TIME = 0x0047A1E0         # AI/Target +0xA0 (drift commit time)

VDB_AGGRO = {
    P_MIN_AGGRO: 0.002, P_MIN_WAIT: 0.0, P_MAX_WAIT: 3.0,
    P_DIST_AHEAD: 40.0, P_DIST_BEHIND: 150.0, P_MIN_TGT_MPH: 75.0,
    P_SLOW_FACTOR: 0.9, P_BOOST_DIST: 15.0, P_BOOST_AGGRO: -22.5,
    P_START_DELAY: 3.0, P_IMMUNITY: 3.0, P_BLOCK_MIN: 3.0, P_BLOCK_MAX: 15.0,
    P_BLOCK_DIST: 15.0, P_SEPARATION: 3.0, P_POS_TIME: 30.0,
    P_AHEAD_GAP: 3.5, P_SPD_DIFF: 50.0, P_STEER_OUT_D: 5.0,
    P_STEER_OUT_T: 0.5, P_SLAM_TIME: 0.75, P_MAX_COS: 0.8, P_COMMIT: 0.075,
    P_STICKY_D: 10.0, P_STICKY_MPH: 40.0, P_CLOSE_MATCH: 10.0,
    P_APEX_TIME: 1.0,
}

# scratch areas for the aggression session
A_STACK = 0x20000000
A_STACK_SZ = 0x00100000
A_VEH = 0x30000000               # physics vehicles, stride A_VEH_STRIDE
A_VEH_STRIDE = 0x4000
A_FRAME = 0x60000000             # RenderWare frames, stride 0x100
A_SECT = 0x61000000              # route section object + node link table
A_LEN = 0x61010000               # FUN_00194380 cumulative length table
A_RET = 0x50000000


class AggroSession(object):
    """Two (or more) racecars laid out at their REAL global addresses so the
    aggression code's array walks work.  Slot i -> racecar G_RC_ARRAY+i*0x27E0,
    physics vehicle A_VEH+i*A_VEH_STRIDE, AI object racecar+0x1A00, aggression
    object racecar+0x1B70."""

    def __init__(self, ncars=2, clock=20.0, dt=DT, params=None):
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        ev.load_elf(uc, ELF)
        for base, size in ((A_STACK, A_STACK_SZ),
                           (A_VEH, A_VEH_STRIDE * 8),
                           (A_FRAME, 0x1000), (A_SECT, 0x10000),
                           (A_LEN, 0x1000), (A_RET & ~(PAGE - 1), PAGE)):
            uc.mem_map(base, size, UC_PROT_ALL)
        self.uc = uc
        self.fault = None
        self.ncars = ncars
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)

        self.wf(G_DT, dt)
        self.wf(G_CLOCK, clock)
        self.wf(G_ONE, 1.0)
        self.wi(G_RC_COUNT, ncars)
        self.wi(G_TRACK_LOADED, 1)
        # FUN_00194380: 2 sections, lap-agnostic, unit length -> track distance
        # is simply racecar+0x1418 (the within-section fraction slot).
        self.wi(G_SECT_COUNT, 2)
        self.wu(G_SECT_LEN, A_LEN)
        self.wb(G_LAPFLAG, 1)
        self.wf(A_LEN + 0, 0.0)
        self.wf(A_LEN + 8, 1.0)
        # route section object: rc+0x18C4 -> [+4] -> [+8] = node link table
        self.wu(A_SECT + 0x00, 0)
        self.wu(A_SECT + 0x04, A_SECT + 0x40)
        self.wu(A_SECT + 0x40 + 0x08, A_SECT + 0x100)
        for i in range(64):                      # 10 bytes per node
            self.uc.mem_write(A_SECT + 0x100 + i * 10, b"\x00" * 10)
            self.wb(A_SECT + 0x100 + i * 10 + 2, 0xFF)   # no junction
        for i in range(ncars):
            self._init_car(i)
        for a, v in VDB.items():
            self.wf(a, v)
        for a, v in (params if params is not None else VDB_AGGRO).items():
            self.wf(a, v)

    # --- addresses ---
    def rc(self, i):
        return G_RC_ARRAY + i * RC_STRIDE

    def ai(self, i):
        return self.rc(i) + 0x1A00

    def agg(self, i):
        return self.rc(i) + AGG_OFF

    def veh(self, i):
        return A_VEH + i * A_VEH_STRIDE

    def frame(self, i):
        return A_FRAME + i * 0x100

    # --- memory helpers (same shapes as Session) ---
    def _unmapped(self, uc, access, address, size, value, user):
        if self.fault is None:
            self.fault = "unmapped 0x%08X @ EIP 0x%08X" % (
                address, uc.reg_read(UC_X86_REG_EIP))
        return False

    wf = Session.wf
    wu = Session.wu
    wi = Session.wi
    wb = Session.wb
    wv = Session.wv
    rf = Session.rf
    ru = Session.ru
    ri = Session.ri
    rb = Session.rb

    def _init_car(self, i):
        rc, veh, fr = self.rc(i), self.veh(i), self.frame(i)
        self.wu(G_VEH_TBL + i * 0x30, veh)
        self.wu(G_RC_PTRS + i * 4, rc)
        self.wb(rc + 0x19BC, i)               # this car's vehicle slot
        self.wu(rc + 0x2440, veh)
        self.wu(veh + 0x204, fr)
        self.wu(rc + 0x1198, rc)              # OOC record -> self
        self.wu(rc + 0x1A00, rc + 0x1A00)     # FUN_001705F0 back-pointers
        self.wu(rc + 0x1A04, rc)
        self.wu(rc + 0x1A08, rc + 0x1A00)
        self.wu(rc + 0x2150, rc + 0x1A00)
        self.wu(rc + 0x2154, rc)
        self.wu(rc + 0x2158, rc + 0x1A00)
        self.wu(rc + 0x2160, rc)
        self.wu(rc + 0x2164, rc + 0x1A00)
        self.wu(rc + 0x21A0, rc)
        self.wu(rc + 0x18C4, A_SECT)          # route section object
        self.wu(rc + 0x18C8, i)               # node index (one per car)
        self.wi(rc + 0x1920, 1)               # AI racer
        self.wi(rc + 0x134C, 1)               # race class (not traffic)
        self.wi(rc + 0x2450, 0)
        self.wi(rc + 0x1650, -1)              # no designated rival
        self.wf(rc + 0x1598, -1.0)            # never been out of control
        self.wf(rc + 0x10DC, 20.0)            # race time
        self.wf(rc + 0x2444, 1.9)             # car width
        self.wf(rc + 0x2448, 4.4)             # car length
        self.wf(rc + 0x23E0, 0.5)             # per-opponent aggression
        self.wb(rc + 0x18FA, 0)               # not wrecked
        self.wb(rc + 0x18FD, 0)
        self.wb(rc + 0x27D8, 0)
        self.wb(rc + 0x11EE, 0)
        self.wb(rc + 0x2431, 0)
        self.wb(rc + 0x1C12, 0)
        self.wb(rc + 0x1BF1, 0)
        self.wb(veh + 0x1550, 1)              # the FUN_0016A360 gate
        self.wb(veh + 0x1554, 0)
        self.wf(veh + 0x1570, -1.0)
        for k in range(8):                    # per-slot tables on the racecar
            self.wf(rc + 0x18A4 + k * 4, 100.0)   # lateral offset to slot k
            self.wf(rc + 0x15E0 + k * 4, -100.0)  # last time we hit slot k
        self.place(i, (0.0, 0.0, float(i) * 10.0))

    def place(self, i, pos, fwd=(0.0, 0.0, 1.0), right=(1.0, 0.0, 0.0),
              speed=40.0, track=0.0):
        rc, veh, fr = self.rc(i), self.veh(i), self.frame(i)
        up = (0.0, 1.0, 0.0)
        self.wv(rc + 0x10, list(right) + [0.0])
        self.wv(rc + 0x20, list(up) + [0.0])
        self.wv(rc + 0x30, list(fwd) + [0.0])
        self.wv(rc + 0x40, list(pos) + [1.0])
        self.wv(rc + 0x18E0, list(fwd) + [0.0])    # road direction at the node
        self.wv(fr + 0x00, list(right) + [0.0])
        self.wv(fr + 0x10, list(up) + [0.0])
        self.wv(fr + 0x20, list(fwd) + [0.0])
        self.wv(fr + 0x30, list(pos) + [1.0])
        self.wv(veh + 0xC0, list(fwd) + [0.0])
        self.wf(veh + 0xBC, speed)
        self.wi(rc + 0x141C, 1)                    # FUN_00194380 section idx
        self.wf(rc + 0x1418, track)                # -> track distance (m)
        self.wi(rc + 0x1420, 0)

    def agg_init(self, i):
        """FUN_00169490 -- construct/reset the aggression object."""
        return self.call(F_AGG_CTOR, regs={UC_X86_REG_EAX: self.rc(i),
                                           UC_X86_REG_ECX: self.agg(i)})

    def call(self, addr, regs=None, stack_args=(), max_steps=400000):
        uc = self.uc
        self.fault = None
        sp = A_STACK + A_STACK_SZ - 0x2000
        uc.mem_write(sp, struct.pack('<I', A_RET))
        for i, a in enumerate(stack_args):
            uc.mem_write(sp + 4 + i * 4, struct.pack('<I', a & 0xFFFFFFFF))
        uc.reg_write(UC_X86_REG_ESP, sp)
        for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                  UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI):
            uc.reg_write(r, 0)
        for r, v in (regs or {}).items():
            uc.reg_write(r, v & 0xFFFFFFFF)
        try:
            uc.emu_start(addr, A_RET, count=max_steps)
        except UcError as e:
            if self.fault is None:
                self.fault = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
        return u2f(uc.reg_read(UC_X86_REG_XMM0) & 0xFFFFFFFF)

    def call_b(self, addr, regs=None, stack_args=()):
        """Same, but returns AL (the boolean predicates)."""
        self.call(addr, regs, stack_args)
        return self.uc.reg_read(UC_X86_REG_EAX) & 0xFF

    # --- the aggression object, read back ---
    def agg_dump(self, i):
        a = self.agg(i)
        return {
            "state": self.ri(a + 0x00),
            "aim": [self.rf(a + 0x10 + k * 4) for k in range(4)],
            "aim_valid": self.rb(a + 0x20),
            "attacking": self.rb(a + 0x21),
            "slam_speed": self.rb(a + 0x22),
            "blocked": self.rb(a + 0x23),
            "hit": self.rb(a + 0x24),
            "timer": self.rf(a + 0x2C),
            "entered": self.rf(a + 0x30),
            "side": self.rf(a + 0x34),
            "lateral": self.rf(a + 0x38),
            "longit": self.rf(a + 0x3C),
            "can_slam": self.rb(a + 0x40),
            "block_range": self.rb(a + 0x41),
            "want_slam_speed": self.rb(a + 0x42),
            "target": self.ru(a + 0x48),
        }
