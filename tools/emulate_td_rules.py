#!/usr/bin/env python3
"""
Ground truth for the takedown TRIGGER rules: run Burnout 3's REAL slam ->
out-of-control -> crash-attribution -> commit chain under Unicorn.

This is the acceptance oracle for src/burnout3_td_rules.c, in the pattern
tools/emulate_vehicle.py / tools/emulate_tdfx.py established: map
build/burnout3.elf at its true VAs, seed the real environment, execute the
actual x86, read the post-state back out.  Nothing here transcribes the
decompiler.

Functions executed for real (corrected VAs; .text = old flat + 0x10000):

  FUN_00029F30  the RACE game-mode's game-context vtable +0x64 -- the slam
                report entry.  Switches on the slam kind and routes to the
                rub / wall-shunt / light-slam / full-slam handlers.
  FUN_00197BE0  the kind 5/6 gate in front of the slam handler (the 1.0 s
                mutual re-slam cooldown).
  FUN_001989A0  the slam handler itself.  THE POINT: it has no call to the
                crash entry FUN_0010DCA0 -- a slam only stamps the victim's
                out-of-control clock racecar+0x1598.
  FUN_00197EA0  kind 2, "shunted into the scenery by car N".
  FUN_00197430  the crash-attribution stamps (racecar+0x15A8[victim slot]).
  FUN_00197040  the deferred takedown-claim scan.
  FUN_00198E60  the takedown commit.
  FUN_001994D0  the BP award + takedown message selection.
  FUN_00197920  the DENIED / LUCKY ESCAPE arming.
  FUN_00195CE0  the DENIED / LUCKY ESCAPE award.

Usage:  python3 tools/emulate_td_rules.py [section]
"""
import os
import struct
import sys

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)

from emulate_tdfx import Emu, STUB_BASE, SCRATCH            # noqa: E402
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX,   # noqa: E402
                               UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI)

# --------------------------------------------------------------------------
# addresses
# --------------------------------------------------------------------------
F_SLAM_ENTRY = 0x00029F30   # game-context vtable +0x64 (race modes)
F_SLAM_GATE  = 0x00197BE0   # kind 5/6 gate -> FUN_001989A0
F_SLAM       = 0x001989A0   # the slam handler
F_WALLSHUNT  = 0x00197EA0   # kind 2
F_LIGHTSLAM  = 0x00197D20   # kind 3/4
F_RUBSTAMP   = 0x001979E0   # kind 1
F_ATTRIB     = 0x00197430   # crash attribution
F_CRASHNOTE  = 0x00197750   # crash notification (stores the cause record)
F_CLAIMSCAN  = 0x00197040   # deferred claim scan
F_COMMIT     = 0x00198E60   # takedown commit
F_AWARD      = 0x001994D0   # BP + message selection
F_DENYARM    = 0x00197920   # DENIED / LUCKY arming
F_DENYAWARD  = 0x00195CE0   # DENIED / LUCKY award
F_POSTHUD    = 0x00199350   # PostHudCallout
F_CRASHENTRY = 0x0010DCA0   # the crash-state entry (never called by a slam)

# helpers the above call that this oracle stubs
STUB_FUNCS = {
    0x00017310: ('crash_party_mode', 0),
    0x00197F90: ('slam_type', 0),
    0x0019A050: ('combo_helper', 0),
    0x00140610: ('sfx_a', 0),
    0x00140480: ('sfx_b', 0),
    0x00141700: ('sfx_c', 0),
    0x00190270: ('stat_a', 0),
    0x001902A0: ('stat_b', 0),
    0x001902D0: ('stat_c', 0),
    0x00190300: ('stat_d', 0),
    0x00190330: ('stat_e', 0),
    0x00190380: ('stat_f', 0),
    0x001903D0: ('stat_g', 0),
    0x00190400: ('stat_h', 0),
    0x00048760: ('rand_f', 0),
    0x00125100: ('fx', 12),
    0x001987A0: ('light_slam_bp', 0),   # cdecl: FUN_00197D20 @0x00197E38 does add esp,0x10
    0x00158640: ('sig_track', 0),
    0x001586A0: ('sig_event', 0),
    0x0019CEE0: ('sig_progress', 0),
    0x0017ABB0: ('score_reset_helper', 0),
    0x001991F0: ('score_reset_helper2', 0),
}

# globals
G_RC_TABLE   = 0x0073A1A8   # racecar pointer table (stride 4)
G_RC_COUNT   = 0x0073A19C   # number of racecars
G_LOCALS     = 0x0073A1C0   # number of local players (1 => signature TDs)
G_PVTABLE    = 0x0064B38C   # grid slot -> physics vehicle, stride 0x30
G_GAMECTX    = 0x004D5370   # -> +0x1B8 -> the game-mode object -> vtable
G_PLAYERCTX  = 0x00667E90   # per-player context, stride 0x4AD0 (+0x4AC5 = AT on)
G_PLAYERCNT  = 0x00731F90

# tuned score parameters (registrar storage; retail Data/vdb.xml values)
P_CLEAR_WAIT   = 0x003F7400   # 0.5
P_MAX_CRASH    = 0x003F7404   # 2.0
P_MIN_COLLIDE  = 0x003F740C   # 0.1
P_DOUBLE_WIN   = 0x003F7410   # 1.0
P_SPREE_WIN    = 0x003F7414   # 30.0
P_BP_TAKEDOWN  = 0x003F746C   # 150
P_BP_REVENGE   = 0x003F7470   # 350
P_BP_PSYCHE    = 0x003F7474   # 150
P_BP_AFTERT    = 0x003F7478   # 1250
P_BP_LUCKY     = 0x003F7488   # 15
P_BP_DENIED    = 0x003F748C   # 10
P_BP_DOUBLE    = 0x003F7508   # [4]
P_BP_SPREE     = 0x003F7518   # [4]
P_SLAM_BP      = 0x003F7448   # Slam Type BP[4]
P_SUPER_BP     = 0x003F7458   # Super Slam Type BP[4]
P_SLAM_BOOST   = 0x003F73EC   # 180
P_BURNING_BP   = 0x003F7444   # 15

RC_STRIDE = 0x27E0
RC_BASE   = SCRATCH + 0x10000        # racecars live here
PV_BASE   = SCRATCH + 0x90000        # physics vehicles
MISC      = SCRATCH + 0xE0000        # vtables / cause records / scratch

VDB = {                       # the retail values, asserted by the validator
    P_CLEAR_WAIT: 0.5, P_MAX_CRASH: 2.0, P_MIN_COLLIDE: 0.1,
    P_DOUBLE_WIN: 1.0, P_SPREE_WIN: 30.0,
}
VDB_I = {P_BP_TAKEDOWN: 150, P_BP_REVENGE: 350, P_BP_PSYCHE: 150,
         P_BP_AFTERT: 1250, P_BP_LUCKY: 15, P_BP_DENIED: 10}


def _ret0(e, args, regs):
    e.uc.reg_write(UC_X86_REG_EAX, 0)


# --------------------------------------------------------------------------
class World(Emu):
    """A seeded 6-car race world with the score objects wired up."""

    def __init__(self, ncars=6):
        Emu.__init__(self)
        self.ncars = ncars
        for addr, (name, argb) in STUB_FUNCS.items():
            self.stub(addr, name, argbytes=argb, cb=_ret0)
        # a hard failure if anything reaches the crash entry from a slam
        self.stub(F_CRASHENTRY, 'CRASH_ENTRY', argbytes=12)
        self.stub(F_POSTHUD, 'post_callout', argbytes=12, cb=self._post_cb)
        self.posts = []
        self._seed_world()

    # -- callbacks --------------------------------------------------------
    def _post_cb(self, e, args, regs):
        # FUN_00199350: ESI = callout slot, EDI = message id, [esp+4] = BP
        self.posts.append(dict(slot=regs['esi'], msg=regs['edi'], bp=args[0]))

    # -- seeding ----------------------------------------------------------
    def rc(self, i):
        return RC_BASE + i * RC_STRIDE

    def pv(self, i):
        return PV_BASE + i * 0x4000

    def score(self, i):
        return self.rc(i) + 0x10D0

    def cslot(self, i):                       # the callout slot score+0x124
        return self.score(i) + 0x124

    def _seed_world(self):
        # score parameters at their registrar storage
        for a, v in VDB.items():
            self.wf(a, v)
        for a, v in VDB_I.items():
            self.wi(a, v)
        for k, v in enumerate((300, 500, 750, 1000)):
            self.wi(P_BP_DOUBLE + 4 * k, v)
            self.wi(P_BP_SPREE + 4 * k, v)
        for k, v in enumerate((50, 20, 30, 15)):
            self.wi(P_SLAM_BP + 4 * k, v)
        for k, v in enumerate((80, 30, 30, 30)):
            self.wi(P_SUPER_BP + 4 * k, v)
        self.wf(P_SLAM_BOOST, 180.0)
        self.wi(P_BURNING_BP, 15)

        self.wi(G_RC_COUNT, self.ncars)
        self.wi(G_LOCALS, 1)
        self.wi(G_PLAYERCNT, 2)
        for i in range(self.ncars):
            self.wi(G_RC_TABLE + 4 * i, self.rc(i))

        # game-mode object + vtable, with recording/const stubs.
        # DAT_004D5370 holds a POINTER; the mode object is at ptr+0x1B8 and
        # its vtable is at mode[0].  FUN_001989A0 explicitly tolerates a NULL
        # DAT_004D5370 (0x00198C6B) -- that path skips the global boost scale,
        # which is what the slam sections want, so the pointer is off by
        # default and turned on for the commit / award / denied sections.
        vt = MISC + 0x100
        ctx = MISC + 0x300
        obj = MISC + 0x400
        self._gamectx_ptr = ctx
        self.wi(G_GAMECTX, 0)
        self.wi(ctx + 0x1B8, obj)
        self.wi(obj, vt)
        for off in range(0, 0x120, 4):
            self.wi(vt + off, STUB_BASE + 0x200)          # generic ret-0
        self.stub(STUB_BASE + 0x200, 'gamectx_generic', argbytes=0)
        self.wi(vt + 0x40, STUB_BASE + 0x210)
        self.stub(STUB_BASE + 0x210, 'gamectx_modeflags', argbytes=0,
                  cb=lambda e, a, r: e.uc.reg_write(UC_X86_REG_EAX, 3))
        self.wi(vt + 0x5C, STUB_BASE + 0x220)
        self.stub(STUB_BASE + 0x220, 'gamectx_ontakedown', argbytes=8)
        self.wi(vt + 0xAC, STUB_BASE + 0x230)
        self.stub(STUB_BASE + 0x230, 'gamectx_boostscale', argbytes=0)

        # per-player context: aftertouch enabled
        for p in range(2):
            self.wb(G_PLAYERCTX + p * 0x4AD0 + 0x4AC5, 1)

        # per-player HUD objects (FUN_001989A0 posts through vtable +0x0C)
        hvt = MISC + 0x600
        for off in range(0, 0x40, 4):
            self.wi(hvt + off, STUB_BASE + 0x240)
        self.stub(STUB_BASE + 0x240, 'hud_post', argbytes=8)
        for p in range(2):
            self.wi(0x004CFB20 + p * 0xC50, hvt)

        for i in range(self.ncars):
            self.seed_car(i)

    def seed_car(self, i, cls=None, grid=None):
        rc, pv = self.rc(i), self.pv(i)
        self.w(rc, b'\0' * RC_STRIDE)
        self.w(pv, b'\0' * 0x2000)
        grid = i if grid is None else grid
        cls = (0 if i == 0 else 1) if cls is None else cls
        self.wi(pv + 0x13F4, rc)             # physics vehicle -> racecar
        self.wi(rc + 0x2440, pv)             # racecar -> physics vehicle
        self.wi(G_PVTABLE + grid * 0x30, pv)
        self.wi(rc + 0x1198, rc)             # score+0xC8  self back-pointer
        self.wi(rc + 0x133C, rc)             # score+0x26C self back-pointer
        self.wb(rc + 0x19BC, grid)
        self.wi(rc + 0x1920, cls)
        self.wi(rc + 0x134C, 0)              # race state
        self.wb(rc + 0x18FA, 0)
        self.wb(rc + 0x18FB, 0)
        self.wi(rc + 0x1320, -1)             # signature selector
        self.wi(rc + 0x27D0, 0 if cls == 0 else 0)
        self.wf(rc + 0x10DC, 0.0)            # race clock
        self.wf(rc + 0x140C, -1.0)           # crash start
        self.wf(rc + 0x1598, -1.0)           # slam clock
        self.wf(rc + 0x158C, -1.0)           # attacker last-slam clock
        self.wf(rc + 0x1690, -1.0)           # second OOC clock
        self.wf(rc + 0x240C, -1.0)           # AI recovery clock (idle)
        self.wi(rc + 0x16BC, 0)              # aggressor pointer
        self.wf(rc + 0x16C0, -1.0)           # aggressor clock
        self.wi(rc + 0x1684, -1)             # psyche target slot
        self.wf(rc + 0x11D0, 720.0)          # boost bar size
        self.wf(rc + 0x11D4, 0.0)            # boost meter
        self.wf(rc + 0x11E4, 1.0)            # earning multiplier
        self.wf(rc + 0x11E8, 0.0)
        self.wf(rc + 0x11CC, 0)              # boost tier
        # the score's claim slots.  NOTE the array is exactly ncars wide:
        # score+0x4D8 + 4*ncars runs straight into score+0x4F0 for 6 cars.
        for k in range(self.ncars):
            self.wf(self.score(i) + 0x4D8 + 4 * k, -1.0)
            self.wf(self.score(i) + 0x528 + 4 * k, 0.0)
            self.wf(self.score(i) + 0x510 + 4 * k, -1.0)
        for k in range(6):
            self.wb(self.score(i) + 0x4F0 + k, 0)
            self.wb(self.score(i) + 0x4F6 + k, 0)
        self.wb(self.score(i) + 0x5B8, 0)     # psyche-out armed
        for k in range(6):
            self.wb(self.score(i) + 0x5B9 + k, 0)
        self.wb(self.score(i) + 0x5E5, 0)
        self.wb(self.score(i) + 0x5E6, 0)
        self.wi(self.score(i) + 0x5EC, 0)
        self.wf(self.score(i) + 0x5F0, -1.0)
        self.wf(self.cslot(i) + 0x110, -1.0)     # double window
        self.wi(self.cslot(i) + 0x114, 0)
        self.wf(self.cslot(i) + 0x118, -1.0)     # spree window
        self.wi(self.cslot(i) + 0x11C, 0)
        self.wi(self.cslot(i) + 0x128, 0)        # aftertouch count
        self.wi(self.cslot(i) + 0x130, 0)        # posted message priority
        self.wi(self.cslot(i) + 0x148, rc)       # slot -> racecar

    def gamectx(self, on=True):
        """Wire DAT_004D5370 to the seeded game-mode object (or NULL)."""
        self.wi(G_GAMECTX, self._gamectx_ptr if on else 0)

    def set_clock(self, t):
        for i in range(self.ncars):
            self.wf(self.rc(i) + 0x10DC, t)

    def set_pos(self, i, x, y, z):
        for k, v in enumerate((x, y, z)):
            self.wf(self.rc(i) + 0x40 + 4 * k, v)

    # -- the executed entry points ---------------------------------------
    def slam_entry(self, kind, attacker, victim, strength):
        """FUN_00029F30(kind, attacker_pv, victim_pv, strength) -- ret 0x10."""
        return self.call(F_SLAM_ENTRY,
                         stack_args=(kind, self.pv(attacker), self.pv(victim),
                                     struct.unpack('<I',
                                                   struct.pack('<f', strength))[0]))

    def slam_gate(self, attacker, victim, strength, type_byte):
        """FUN_00197BE0: EAX = type, ECX = attacker pv, EDI = victim pv."""
        return self.call(F_SLAM_GATE,
                         regs={UC_X86_REG_EAX: type_byte,
                               UC_X86_REG_ECX: self.pv(attacker),
                               UC_X86_REG_EDI: self.pv(victim)},
                         stack_args=(struct.unpack('<I',
                                     struct.pack('<f', strength))[0],))

    def slam(self, attacker, victim, strength, type_byte):
        """FUN_001989A0: EAX = victim pv, stack = attacker pv, strength, type."""
        return self.call(F_SLAM,
                         regs={UC_X86_REG_EAX: self.pv(victim)},
                         stack_args=(self.pv(attacker),
                                     struct.unpack('<I',
                                     struct.pack('<f', strength))[0],
                                     type_byte))

    def wall_shunt(self, attacker, victim):
        """FUN_00197EA0: EAX = victim racecar, ECX = attacker racecar."""
        return self.call(F_WALLSHUNT,
                         regs={UC_X86_REG_EAX: self.rc(victim),
                               UC_X86_REG_ECX: self.rc(attacker)})

    def attribute(self, slot, cause=0):
        """FUN_00197430(record): ESI = the crashing car's score object."""
        return self.call(F_ATTRIB,
                         regs={UC_X86_REG_ESI: self.score(slot)},
                         stack_args=(cause,))

    def claim_scan(self, slot):
        """FUN_00197040(score)."""
        return self.call(F_CLAIMSCAN, stack_args=(self.score(slot),))

    def commit(self, attacker, victim):
        """FUN_00198E60: ESI = attacker score, EDI = victim racecar."""
        return self.call(F_COMMIT,
                         regs={UC_X86_REG_ESI: self.score(attacker),
                               UC_X86_REG_EDI: self.rc(victim)})

    def award(self, attacker, victim, psyche, aftertouch, cause=0):
        """FUN_001994D0(slot, victim rc, psyche, aftertouch); ECX = cause."""
        return self.call(F_AWARD,
                         regs={UC_X86_REG_ECX: cause},
                         stack_args=(self.cslot(attacker), self.rc(victim),
                                     psyche, aftertouch))

    def deny_arm(self, slot, obj):
        """FUN_00197920: ECX = score, EDX = the contacted object."""
        return self.call(F_DENYARM,
                         regs={UC_X86_REG_ECX: self.score(slot),
                               UC_X86_REG_EDX: obj})

    def deny_award(self, slot):
        """FUN_00195CE0: EDI = score."""
        return self.call(F_DENYAWARD, regs={UC_X86_REG_EDI: self.score(slot)})

    # -- readback ---------------------------------------------------------
    def car_state(self, i):
        rc = self.rc(i)
        s = self.score(i)
        return dict(
            slam_time=self.rf(rc + 0x1598),
            slam_type=self.rb(rc + 0x159C),
            aggressor=self.ru(rc + 0x16BC),
            aggressor_time=self.rf(rc + 0x16C0),
            slams_made=self.ri(rc + 0x1174),
            times_slammed=self.ri(rc + 0x1590),
            td_credited=self.rb(rc + 0x15D6),
            td_by=self.ru(rc + 0x15DC),
            revenge=self.rb(rc + 0x168F),
            recover_at=self.rf(rc + 0x240C),
            bp=self.ri(rc + 0x111C),
            claim=[self.rf(s + 0x4D8 + 4 * k) for k in range(6)],
            claim_at=[self.rb(s + 0x4F0 + k) for k in range(6)],
            claim_ps=[self.rb(s + 0x4F6 + k) for k in range(6)],
            td_count=self.ri(s + 0x68),
            last_victim=self.ru(s + 0x4D4),
            denied=self.rb(s + 0x5E5),
            lucky=self.rb(s + 0x5E6),
            dbl_window=self.rf(self.cslot(i) + 0x110),
            dbl_count=self.ri(self.cslot(i) + 0x114),
            spree_window=self.rf(self.cslot(i) + 0x118),
            spree_count=self.ri(self.cslot(i) + 0x11C),
            at_count=self.ri(self.cslot(i) + 0x128),
        )

    def cause_record(self, wall=0, has_obj=0, surface=0, obj=0, wreck_rc=0):
        a = MISC + 0x800
        self.w(a, b'\0' * 0x10)
        self.wb(a + 0x00, wall)
        self.wb(a + 0x01, has_obj)
        self.wi(a + 0x04, surface)
        self.wi(a + 0x08, obj)
        self.wi(a + 0x0C, wreck_rc)
        return a

    def traffic_object(self, vclass):
        """An object whose class byte at +0x173 drives the vehicle messages."""
        a = MISC + 0x1000
        self.w(a, b'\0' * 0x200)
        self.wb(a + 0x173, vclass)
        self.wb(a + 0x176, 0)
        self.wb(a + 0x171, 0)
        self.wi(a + 0x16C, 0)
        return a


# --------------------------------------------------------------------------
# THE CRASH-THRESHOLD AUTHORITY veh+0x1534 (docs/RE_TD_RULES.md 12)
#
# FUN_00105FC0 is the view-distance ladder FUN_00105BD0 feeds; it is small,
# self-contained and takes its whole input in registers, so it runs for real
# here.  FUN_00105BD0's tail arithmetic around it (@0x00105ED8..0x00105F4B)
# is three instructions and is reproduced from the image constants, which the
# validator asserts byte for byte.
# --------------------------------------------------------------------------
F_AUTH_LADDER = 0x00105FC0   # the ladder itself
F_PARTY_TEST  = 0x001942F0   # "are we in a crash-party mode" (stubbed)

G_VIEW_R2_ALT = 0x005A39E0   # BSS, reset thunk @0x002B8D80 seeds it 19600
G_VIEW_R2     = 0x005A39FC   # BSS, reset thunk @0x002B8DA0 seeds it 15625
K_VIEW_R2_ALT = 0x0039A850   # 19600.0  (the thunks' source constants)
K_VIEW_R2     = 0x0039A854   # 15625.0
K_LADDER      = 0x0039A858   # float[6]; [1..5] = the band fractions
K_AUTH_SCALE  = 0x003B1A2C   # 0.97
K_AUTH_FLOOR  = 0x00384148   # 0.03
K_AUTH_SLAM   = 0x003A69BC   # 0.05   FUN_00105340 @0x001056C6
K_AUTH_SLAMCM = 0x003A69C4   # 0.1    FUN_00105340 @0x001057B2
K_ONE         = 0x003B168C   # 1.0

AUTH_VEH  = SCRATCH + 0xC0000    # the physics vehicle FUN_00105FC0 reads
AUTH_RC   = SCRATCH + 0xC2000    # its racecar
AUTH_NEAR = SCRATCH + 0xC4000    # the nearest viewed racecar (param_1)
AUTH_OUT  = SCRATCH + 0xC6000    # the 12-byte out struct

# FUN_00112E70's object path: the handle-class getter and the crashable table
F_HANDLE_CLASS = 0x0010FBC0  # ECX = collision handle -> class 0..6
G_BIGHIT_ID    = 0x0073BB8C  # the DESIGNATED big-hit traffic vehicle id
K_OBJ_TABLE    = 0x0039AE50  # 7x7 crashable[classB][classA]
OBJ_HANDLE = SCRATCH + 0xCC000
OBJ_ENTITY = SCRATCH + 0xCD000

# --------------------------------------------------------------------------
# THE LADDER'S DISTANCE INPUT -- FUN_00105BD0 run WHOLE.
#
# RE_TD_RULES 12.2(a) carried two [S] claims: that DAT_0073A1C0 is the LOCAL
# PLAYER count and DAT_0073A1D0 the array those players' racecars live in, so
# that in single player the ladder's d2 is the squared distance to THE
# PLAYER'S CAR.  Executing FUN_00105BD0 settles both: the loop @0x00105DC0
# walks EDI = 0x0073A1D0, stride 0x27E0, i < [0x0073A1C0], calls the virtual
# at *(*EDI + 0x14) with ECX = EDI (a `this` call on the record itself) and
# stores |returned position - veh+0x204 row 3|^2 into veh+0x1560[i].
# FUN_00106370 @0x001063B0..0x001063F0 closes it independently: it picks the
# local player index into veh+0x1554 (0 when [0x0073A1C0] == 1, else
# grid & 1) and indexes THE SAME base -- `IMUL EAX,EAX,0x27E0; ADD EAX,
# 0x73A1D0`.
# --------------------------------------------------------------------------
F_AUTH_FULL   = 0x00105BD0   # the whole authority pass
F_UNIT_LOOKUP = 0x001AD4A0   # the visibility / resident-unit test
F_AUTH_ON     = 0x00106290   # veh+0x1550 transition helpers (stubbed)
F_AUTH_OFF    = 0x00106150

G_LOCAL_COUNT = 0x0073A1C0   # the LOCAL PLAYER count  [proved below]
G_LOCAL_CARS  = 0x0073A1D0   # their racecars, inline, stride 0x27E0
G_WORLDOBJS   = 0x007397C8   # world objects, stride 0x1CC
G_PVTABLE2    = 0x0064B38C   # grid -> physics-vehicle record, stride 0x30

LP_POS   = SCRATCH + 0xC8000     # 16-aligned position buffers, stride 0x20
LP_VT    = SCRATCH + 0xC9000     # one vtable per local player


class Authority(Emu):
    """Runs the REAL view-distance authority ladder."""

    def __init__(self):
        Emu.__init__(self)
        # the BSS radii, as their reset thunks leave them
        self.wf(G_VIEW_R2, self.rf(K_VIEW_R2))
        self.wf(G_VIEW_R2_ALT, self.rf(K_VIEW_R2_ALT))
        self.party = 0
        self.stub(F_PARTY_TEST, 'party_test', 0,
                  lambda e, a, r: e.uc.reg_write(UC_X86_REG_EAX, e.party))

    def ladder(self, d2, alt=0, party=0, seen=1):
        """FUN_00105FC0(EAX = nearest viewed racecar, veh, out), XMM1 = d2.
        Returns the out struct plus the authority FUN_00105BD0 derives."""
        from unicorn.x86_const import UC_X86_REG_XMM1
        self.party = 1 if party else 0
        self.w(AUTH_VEH, b'\0' * 0x2000)
        self.w(AUTH_RC, b'\0' * 0x2000)
        self.w(AUTH_NEAR, b'\0' * 0x2000)
        self.w(AUTH_OUT, b'\0' * 0x10)          # zero-init @0x00105BFB
        self.wi(AUTH_VEH + 0x1568, AUTH_RC)
        self.wb(AUTH_VEH + 0x216, 0x00 if seen else 0xFF)
        self.wb(AUTH_VEH + 0x1550, 1 if alt else 0)
        self.uc.reg_write(UC_X86_REG_XMM1, struct.unpack(
            '<I', struct.pack('<f', float(d2)))[0])
        self.call(F_AUTH_LADDER,
                  regs={UC_X86_REG_EAX: AUTH_NEAR if seen else 0},
                  stack_args=(AUTH_VEH, AUTH_OUT))
        out0 = self.rf(AUTH_OUT + 0)
        out1 = self.rf(AUTH_OUT + 4)
        in_range = self.rb(AUTH_OUT + 8)
        flag9 = self.rb(AUTH_OUT + 9)
        crash_ok = self.rb(AUTH_OUT + 10)
        # FUN_00105BD0 @0x00105ED8 / @0x00105F2B
        scale = self.rf(K_AUTH_SCALE)
        floor = self.rf(K_AUTH_FLOOR)
        authority = (out1 * scale + floor) if in_range else floor
        return dict(out0=out0, out1=out1, in_range=in_range, flag9=flag9,
                    crash_ok=crash_ok, authority=authority)

    # ----------------------------------------------------------------------
    # FUN_0010FBC0 -- the collision-handle CLASS getter FUN_00112E70 uses to
    # index DAT_0039AE50.  ECX = the handle; handle+0x00 is the type byte and
    # handle+0x0C the entity (whose +0x242B is compared with DAT_0073BB8C to
    # pick out the DESIGNATED big-hit traffic vehicle).  Run for real.
    # ----------------------------------------------------------------------
    def handle_class(self, handle_type, designated):
        h = OBJ_HANDLE
        ent = OBJ_ENTITY
        self.w(h, b'\0' * 0x20)
        self.w(ent, b'\0' * 0x2500)
        self.wb(h, handle_type)
        self.wi(h + 0x0C, ent)
        self.wb(G_BIGHIT_ID, 7)                 # DAT_0073BB8C
        self.wb(ent + 0x242B, 7 if designated else 9)
        return self.call(F_HANDLE_CLASS,
                         regs={UC_X86_REG_ECX: h}) & 0xFF


class AuthorityFull(Authority):
    """Runs FUN_00105BD0 WHOLE over a synthetic local-player array."""

    def __init__(self):
        Authority.__init__(self)
        self.seen = {}
        self.getpos_calls = []
        self.stub(F_UNIT_LOOKUP, 'unit_lookup', argbytes=8,
                  cb=self._unit_cb)
        self.stub(F_AUTH_ON, 'auth_on', argbytes=0)
        self.stub(F_AUTH_OFF, 'auth_off', argbytes=0)
        # the position getter every local player's vtable +0x14 points at
        self.stub(STUB_BASE + 0x300, 'get_world_pos', argbytes=4,
                  cb=self._getpos_cb)

    def _unit_cb(self, e, args, regs):
        # FUN_001AD4A0(world object, &pos): -1 = not in a resident unit.
        # `self.seen` is keyed by the local-player index the loop is on.
        i = self._loop_i
        e.uc.reg_write(UC_X86_REG_EAX, 0 if self.seen.get(i, 1) else -1)

    def _getpos_cb(self, e, args, regs):
        # `this` (ECX) is the record itself -- that is the proof the array
        # elements ARE the racecars, not pointers to something else.
        this = regs['ecx']
        i = (this - G_LOCAL_CARS) // 0x27E0
        self._loop_i = i
        self.getpos_calls.append(this)
        e.uc.reg_write(UC_X86_REG_EAX, LP_POS + i * 0x20)

    def run(self, veh_pos, players, cls=1, alt=0, seen=None):
        """veh_pos = the SCORED car's frame position, players = [(x,y,z), ..]
        the local players' world positions.  Returns the ladder outputs."""
        self.seen = seen or {}
        self._loop_i = 0
        self.getpos_calls = []
        veh, rc = AUTH_VEH, AUTH_RC
        self.w(veh, b'\0' * 0x2000)
        self.w(rc, b'\0' * 0x2800)
        frame = SCRATCH + 0xCA000
        self.w(frame, b'\0' * 0x40)
        for k in range(3):
            self.wf(frame + 0x30 + 4 * k, veh_pos[k])
        self.wi(veh + 0x204, frame)
        self.wi(veh + 0x1568, rc)
        self.wb(veh + 0x1550, 1 if alt else 0)
        self.wi(rc + 0x1920, cls)          # class 1 = an AI racer
        self.wb(rc + 0x27D9, 1)            # not the human early-out
        self.wb(rc + 0x19BC, 0)            # grid 0
        # @0x00105EA0's OWN crash-entry veto, independent of the ladder:
        #   if (+0x1688 == 0 && (+0x245D != 0 || +0x1B93 == 0)) crash_ok = 0
        # Neither byte is modelled by the harness; retail's steady state must
        # leave the entries enabled, so the oracle seeds that state.  [S]
        self.wb(rc + 0x1B93, 1)
        self.wb(rc + 0x245D, 0)
        # the grid -> physics-vehicle table FUN_00105BD0 @0x00105D19 reads
        # to pick the winner's world instance byte (+0x4ACC)
        pv = SCRATCH + 0xCB000
        self.w(pv, b'\0' * 0x5000)
        self.wi(G_PVTABLE2, pv)
        self.wi(pv + 0x4ACC, 0)
        self.wi(G_LOCAL_COUNT, len(players))
        for i, p in enumerate(players):
            base = G_LOCAL_CARS + i * 0x27E0
            self.w(base, b'\0' * 0x27E0)
            vt = LP_VT + i * 0x40
            self.w(vt, b'\0' * 0x40)
            self.wi(vt + 0x14, STUB_BASE + 0x300)
            self.wi(base, vt)              # record+0x00 = the vtable
            self.wb(base + 0x19BC, i)
            self.w(LP_POS + i * 0x20, b'\0' * 0x20)
            for k in range(3):
                self.wf(LP_POS + i * 0x20 + 4 * k, p[k])
        self.call(F_AUTH_FULL, regs={UC_X86_REG_ESI: veh})
        return dict(
            authority=self.rf(veh + 0x1534),
            flags1353=self.rb(veh + 0x1353),
            d2=[self.rf(veh + 0x1560 + 4 * i) for i in range(len(players))],
            visited=list(self.getpos_calls),
        )


# --------------------------------------------------------------------------
def _demo():
    w = World()
    w.set_clock(10.0)
    print("slam handler calls the crash entry?",
          any(c[0] == 'CRASH_ENTRY' for c in
              (w.slam(1, 0, 1.0, 0) or True) and w.calls))
    print("victim after slam:", w.car_state(0)['slam_time'],
          w.car_state(0)['aggressor'] == w.rc(1))


if __name__ == '__main__':
    _demo()
