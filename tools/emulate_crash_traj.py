#!/usr/bin/env python3
"""
GOLD STANDARD for the CRASH trajectory: run Burnout 3's REAL crashed-path
frame under Unicorn, over a real vehicle struct, for hundreds of consecutive
frames, and capture the full 6DOF trajectory (position, orientation rows,
velocity, omega, angular momentum).

This is the acceptance oracle for the crash module (src/burnout3_crash.c):
tools/validate_crash_traj.py asserts the C mirrors reproduce it.

WHAT THE RETAIL MACHINE DOES WHEN A CAR WRECKS  (all addresses = the analysed
build/burnout3.elf; see docs/RE_NOTES.md section 16.1)

  1. A crash trigger (FUN_0011AEF0 wall / FUN_00113960 car-vs-wreck /
     FUN_0011BE50's up.y < 0.5 rollover / FUN_00118410 crashbreaker) calls
         FUN_0010DCA0 -> FUN_0010DD20
     with EBX = vehicle, ECX = slot, stack (crash director 0x0064ACE8,
     cause record or 0).
  2. FUN_0010DD20 classifies the crash and fires ONE OR TWO IMPULSE KICKS
     through FUN_00125380 / FUN_00125100.  Every crash-entry site in the
     image follows the same two-call shape:

         FUN_00125380(veh, corner_flags, mag)   axis-selected impulse applied
                                                AT AN OFFSET BODY POINT ->
                                                PURE TORQUE  (the tumble)
         FUN_00125100(0x10, mag, frame.up)      PURE LINEAR impulse along the
                                                car's own up row (the LAUNCH)

     FUN_00125100 is the whole mechanism:
         P   = frame.pos
               + frame.right * (-bbmax.x)   if flags & 0x08   (left  side)
               + frame.right * (-bbmin.x)   if flags & 0x04   (right side)
               + frame.at    *   bbmax.z    if flags & 0x21   (front) mag*=1.2
               + frame.at    *   bbmin.z    if flags & 0x42   (rear)  mag*=1.2
               + frame.up    *   bbmax.y * 0.8   if up.y < 0  (inverted: roof)
         J   = axis * 10.0 * mass * mag                [0x0041A504 = 10.0]
         (flags & 0x90) == 0 -> veh+0x120 += (P - pos) x J   TORQUE ONLY
                                (veh+0x110 is saved and RESTORED at 0x001252EC)
         else                -> veh+0x110 += J              LINEAR ONLY
         veh+0x20E := 0 (wake), ctx1+0xFF0..0xFFC = fx pair + clock.
  3. veh+0x210 (crash-mode byte) goes non-zero, and from then on
     FUN_0011BE50 takes its CRASHED BRANCH (0x0011BE83..0x0011BF09):

         veh+0x1530 += dt                          crash clock
         if (veh+0x14C8) { veh+0x14A0 = 0.35; veh+0x14C8 = 0; veh+0x14A4 = 1 }
         FUN_00121560(&veh+0x1448, 0, 0, 0)        engine idles
         if (racecar+0x1920 == 0) FUN_00118410(veh) crashed input shaper
                                                    (aftertouch / crashbreaker)
         FUN_00123000(veh, dt)                     THE CRASH-MODE SOLVER
         FUN_0011C720(veh)                         ctx export

     FUN_00123000 per substep:  low-speed rollover damping / settle damping /
     airborne damping, then FUN_00123FD0 (suspension force pass) and
     FUN_00109560 (rigid-body integration).  Because veh+0x210 != 0,
     FUN_00109560 takes its 0x00109622 branch: gravity is applied at
     pos + up*com_height, so it also produces a TORQUE -- the tumbling term.

Usage:
    python3 tools/emulate_crash_traj.py [scenario ...]   (default: all)
    python3 tools/emulate_crash_traj.py --dump           write build/*.json
"""
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "ep", os.path.join(_here, "emulate_pipeline.py"))
ep = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ep)

from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX,
                               UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI)

VEHICLE = ep.VEHICLE
RACECAR = ep.RACECAR
CTX0 = ep.CTX0
CTX1 = ep.CTX1
SOUP_HDR = ep.SOUP_HDR
SOUP_REC = ep.SOUP_REC
SOUP_TYPE = ep.SOUP_TYPE
f2u = ep.f2u

# ---------------------------------------------------------------------------
# real function addresses (analysed burnout3.elf)
# ---------------------------------------------------------------------------
F_25100 = 0x00125100   # the crash-entry impulse kick
F_25380 = 0x00125380   # axis selection wrapper around F_25100
F_23000 = 0x00123000   # crash-mode solver dispatcher
F_21560 = 0x00121560   # engine update (idle while crashed)
F_C720 = 0x0011C720    # ctx export
F_239C0 = 0x001239C0   # suspension pre-pass (soup rays)
F_AEF0 = 0x0011AEF0    # chassis-vs-world collision response
F_DCA0 = 0x0010DCA0    # crash entry (stubbed: needs the game-context object)
F_151490 = 0x00151490  # FX/sound spawn at the end of F_25100 (stubbed)

# ---------------------------------------------------------------------------
# crash-entry kick flag bits (FUN_00125100, all [C-disasm] at the addresses in
# the module header).  The corner is the SIDE OF THE BODY the impulse is
# applied at; the impulse direction is the axis argument.
# ---------------------------------------------------------------------------
KICK_FRONT = 0x01
KICK_REAR = 0x02
KICK_RIGHT = 0x04
KICK_LEFT = 0x08
KICK_LINEAR = 0x10      # (flags & 0x90) != 0 -> pure linear, no torque

# magnitudes read out of the retail instruction stream ([C-disasm])
MAG_ROLLOVER_LAUNCH = 0.65   # 0x0011C421  FUN_00125100(0x10, .65, frame.up)
MAG_ROLLOVER_SPIN = 0.90     # 0x0011C439  FUN_00125100(0x08, .90, frame.up)
MAG_TAKEDOWN_SPIN = 0.40     # 0x00024F94  FUN_00125100(0x0A, .40, frame.up)
MAG_TAKEDOWN_SPIN2 = 0.30    # 0x00025C8D  FUN_00125100(0x0A, .30, frame.up)
MAG_AFTERTOUCH = 0.60        # 0x00118241  FUN_00125100(corner, .60, frame.up)
MAG_GRIND = 0.25             # 0x00197376  FUN_00125100(4|8, .25, frame.up)


class CrashTraj(ep.Pipeline):
    """Persistent Unicorn session: warm the car up on the real racing
    pipeline, fire a real crash entry, then run real crashed-path frames."""

    def __init__(self, dt=1.0 / 60.0, prepass=True):
        super().__init__(dt=dt)
        # the crashed path never reaches FUN_0011BC60 / the vtable +0x24
        # collision update in FUN_0011BE50 itself; whether the world manager
        # still refreshes the wheel rays is an open question (RE_NOTES 16.1),
        # so it is an explicit ablation knob here.
        self.prepass = prepass
        self.crashed = False
        # stubs: the two callees that need objects this synthetic world has
        # no room for.  Both are recorded, neither touches the rigid body.
        # These are written BEFORE anything executes -- see patch() below for
        # why that matters.
        self.uc.mem_write(F_DCA0, b"\xC2\x0C\x00")        # ret 0xC
        self.uc.mem_write(F_151490, b"\xC2\x08\x00")      # ret 8

    # -- code patching / ablation ------------------------------------------
    def patch(self, addr, code):
        """Overwrite code AND flush Unicorn's translation cache.

        WITHOUT the flush, patching a function that has already executed
        (anything the warm-up frames touched) is a silent no-op: Unicorn keeps
        serving the cached translation.  RE_NOTES 16.1's "stubbing
        FUN_00123FD0 changes the trajectory by 0" was exactly that artifact --
        with the flush the same ablation moves the car 0.4 m in seven frames
        and then drops it through the world (RE_NOTES 16.2).
        """
        self.uc.mem_write(addr, code)
        self.uc.ctl_flush_tb()

    # -- the two real kick primitives --------------------------------------
    def kick(self, flags, mag, axis_addr):
        """FUN_00125100(flags, mag, axis) with ESI = vehicle."""
        self.call(F_25100, stack_args=[flags, f2u(mag), axis_addr],
                  regs={UC_X86_REG_ESI: VEHICLE})

    def kick_axis(self, flags, mag):
        """FUN_00125380(veh, flags, mag): axis = right row when flags & 0x60,
        the at row when flags & 0x80, else the up row."""
        self.call(F_25380, stack_args=[f2u(mag), 0],
                  regs={UC_X86_REG_EAX: VEHICLE, UC_X86_REG_ECX: flags})

    def frame_row(self, row):
        return CTX0 + 0x10 * row

    # -- crash entry --------------------------------------------------------
    def enter_crash(self, corner_flags, mag_spin, mag_launch):
        """The retail crash-entry shape: a corner TORQUE kick plus a pure
        LINEAR up-axis kick, then veh+0x210 := 1 (crash mode).  The impulse
        accumulators are consumed by the next FUN_00109560."""
        if mag_spin is not None:
            self.kick(corner_flags, mag_spin, self.frame_row(1))
        if mag_launch is not None:
            self.kick(KICK_LINEAR, mag_launch, self.frame_row(1))
        self.wb(VEHICLE + 0x210, 1)
        self.crashed = True

    # -- one crashed-path frame (FUN_0011BE50 0x0011BE83..0x0011BF09) -------
    def crashed_frame(self, dt=None):
        """One crashed-path vehicle step.

        `dt` defaults to the session dt.  When `self.timer_driven` is set the
        REAL frame timer (FUN_001B5AC0) already owns DAT_0060EA1C /
        DAT_0060EA20, so this must not clobber them -- see CrashSequence.
        """
        V = self
        base = VEHICLE
        if dt is None:
            dt = self.dt
        if getattr(self, 'timer_driven', False):
            self.clock = self.rf(ep.G_CLOCK)
        else:
            self.clock += dt
            self.wf(ep.G_DT, dt)
            self.wf(ep.G_CLOCK, self.clock)
        self.wf(RACECAR + 0x10DC, self.clock)

        # FUN_00104840 mirror (outer per-frame contact scratch zeroing)
        V.wb(base + 0x212, 0)
        self.uc.mem_write(base + 0x160, b"\0" * 0x40)

        # ablation: refresh the wheel rays the way the racing path does
        if self.prepass:
            V.wu(SOUP_HDR + 0, 2)
            self.call(F_239C0, stack_args=[VEHICLE])

        # 0x0011BE83: crash clock
        V.wf(base + 0x1530, V.vf(0x1530) + dt)
        # 0x0011BE8B: gear -> neutral once
        if V.vu(0x14C8) != 0:
            V.wf(base + 0x14A0, 0.35)      # [0x0039B2B0]
            V.wu(base + 0x14C8, 0)
            V.wu(base + 0x14A4, 1)
        # 0x0011BEC6: engine idles (ESI = &veh+0x1448, three zero args)
        self.call(F_21560, stack_args=[0, 0, 0],
                  regs={UC_X86_REG_ESI: base + 0x1448, UC_X86_REG_EDI: 0})
        # 0x0011BEEB: FUN_00118410 crashed input shaper -- skipped, see the
        # module notes: with no pad direction held (byte ptr [EBX] == 0) its
        # only body effect (the corner aftertouch kicks at 0x0011824A) does
        # not fire, and it needs the pad/HUD objects.
        # 0x0011BEF7: the crash-mode solver
        self.call(F_23000, stack_args=[f2u(dt)],
                  regs={UC_X86_REG_ECX: VEHICLE})
        # 0x0011BEFC: ctx export
        self.call(F_C720, regs={UC_X86_REG_EAX: VEHICLE})
        return self.capture6()

    # -- 6DOF capture -------------------------------------------------------
    def capture6(self):
        V = self
        m = [[self.rf(CTX0 + 16 * r + 4 * c) for c in range(4)]
             for r in range(4)]
        return dict(
            pos=m[3][:3],
            right=m[0][:3], up=m[1][:3], at=m[2][:3],
            vel=[V.vf(0xB0 + 4 * i) for i in range(3)],
            speed=V.vf(0xBC),
            dir=[V.vf(0xC0 + 4 * i) for i in range(4)],
            omega=[V.vf(0xD0 + 4 * i) for i in range(3)],
            angmom=[V.vf(0xE0 + 4 * i) for i in range(3)],
            imp=[V.vf(0x110 + 4 * i) for i in range(3)],
            angimp=[V.vf(0x120 + 4 * i) for i in range(3)],
            # veh+0x40: the world inverse inertia the integrator rebuilt at
            # the end of this step -- the next step's omega = L . this
            iinv_world=[[V.vf(0x40 + 0x10 * r + 4 * c) for c in range(4)]
                        for r in range(3)],
            airborne=V.vb(0x1168),
            settle=V.vb(0x1354),
            asleep=V.vb(0x20E),
            crashmode=V.vb(0x210),
            wheel_surf=[V.vb(0x8D3 + 0xC0 * i) for i in range(4)],
        )

    # -- helper: the vehicle image the C port has to be seeded with ---------
    def export_body(self):
        V = self
        st = {}
        for r in range(4):
            for c in range(4):
                st['frame_%d_%d' % (r, c)] = self.rf(CTX0 + 16 * r + 4 * c)
        for i in range(4):
            st['vel_%d' % i] = V.vf(0xB0 + 4 * i)
            st['omega_%d' % i] = V.vf(0xD0 + 4 * i)
            st['angmom_%d' % i] = V.vf(0xE0 + 4 * i)
            st['imp_%d' % i] = V.vf(0x110 + 4 * i)
            st['angimp_%d' % i] = V.vf(0x120 + 4 * i)
            st['bbmax_%d' % i] = V.vf(0x1D0 + 4 * i)
            st['bbmin_%d' % i] = V.vf(0x1E0 + 4 * i)
        for i in range(3):
            st['iinv_%d' % i] = V.vf(0x10 + 0x10 * i + 4 * i)
        st['mass'] = V.vf(0x1F0)
        st['com_height'] = V.vf(0x1F4)
        st['airborne'] = V.vb(0x1168)
        st['settle'] = V.vb(0x1354)
        st['asleep'] = V.vb(0x20E)
        return st


# ---------------------------------------------------------------------------
# scenarios
# ---------------------------------------------------------------------------
def _warmup(sim, frames, throttle=1.0):
    for _ in range(frames):
        sim.frame(throttle, 0.0, 0.0, 0)


def _wall_soup(sim, wall_z, span=200.0, height=40.0):
    """Replace the soup with the flat ground plus a wall plane facing -z
    (normal (0,0,-1)) at z = wall_z, so FUN_0011AEF0 sees a real wall."""
    S = 5000.0
    tris = [((-S, 0, -S), (-S, 0, S), (S, 0, -S)),
            ((S, 0, S), (S, 0, -S), (-S, 0, S)),
            ((-span, 0.0, wall_z), (-span, height, wall_z),
             (span, 0.0, wall_z)),
            ((span, height, wall_z), (span, 0.0, wall_z),
             (-span, height, wall_z))]
    norms = [(0, 1, 0), (0, 1, 0), (0, 0, -1), (0, 0, -1)]
    sim.wu(SOUP_HDR + 0, len(tris))
    for t, (p0, p1, p2) in enumerate(tris):
        base = SOUP_REC + 0x40 * t
        for j, p in enumerate((p0, p1, p2)):
            for k in range(3):
                sim.wf(base + 0x10 * j + 4 * k, p[k])
            sim.wf(base + 0x10 * j + 12, 0.0)
        for k in range(3):
            sim.wf(base + 0x30 + 4 * k, norms[t][k])
        sim.wf(base + 0x3C, 0.0)
        sim.uc.mem_write(SOUP_TYPE + 2 * t, struct.pack('<H', 0))


def scen_wall(dt=1.0 / 60.0, frames=240, prepass=True):
    """40 m/s head-on into a wall: the REAL FUN_0011AEF0 resolves the wall
    contact (flattened normal, deflection, head-on scrub, horizontal contact
    impulse), then the retail crash entry launches and spins the wreck."""
    sim = CrashTraj(dt=dt, prepass=prepass)
    _warmup(sim, 420, 1.0)
    speed = sim.vf(0xBC)
    # put a wall just ahead of the car
    z = sim.rf(CTX0 + 0x38)
    _wall_soup(sim, z + 3.0)
    sim.call(F_AEF0, regs={UC_X86_REG_ECX: VEHICLE})
    pre = sim.capture6()
    # crash entry: head-on wall -> a REAR corner torque + the linear launch
    # (FUN_0010DD20 0x0010E40F/0x0010E425 shape, magnitudes = the literal
    # rollover pair; the director's own +0x590 is BSS -> [?])
    sim.enter_crash(KICK_REAR, MAG_ROLLOVER_SPIN, MAG_ROLLOVER_LAUNCH)
    traj = [sim.crashed_frame() for _ in range(frames)]
    return sim, speed, pre, traj


def scen_slam(dt=1.0 / 60.0, frames=240, prepass=True):
    """Car-vs-car fatal slam: no wall response, just the crash entry's
    rear-left corner torque (0x00024F94, mag 0.40) plus the linear launch."""
    sim = CrashTraj(dt=dt, prepass=prepass)
    _warmup(sim, 300, 1.0)
    speed = sim.vf(0xBC)
    pre = sim.capture6()
    sim.enter_crash(KICK_REAR | KICK_LEFT, MAG_TAKEDOWN_SPIN,
                    MAG_ROLLOVER_LAUNCH)
    traj = [sim.crashed_frame() for _ in range(frames)]
    return sim, speed, pre, traj


def scen_spin_only(dt=1.0 / 60.0, frames=240, prepass=True):
    """Torque-only entry (no linear launch) -- the ablation that shows the
    linear kick is what makes the car leave the ground."""
    sim = CrashTraj(dt=dt, prepass=prepass)
    _warmup(sim, 300, 1.0)
    speed = sim.vf(0xBC)
    pre = sim.capture6()
    sim.enter_crash(KICK_REAR | KICK_LEFT, MAG_TAKEDOWN_SPIN, None)
    traj = [sim.crashed_frame() for _ in range(frames)]
    return sim, speed, pre, traj


SCENARIOS = {
    'wall': scen_wall,
    'slam': scen_slam,
    'spin_only': scen_spin_only,
}


# ===========================================================================
# THE WHOLE-SEQUENCE ORACLE  --  the crash PRESENTATION, per RENDERED frame
# ===========================================================================
#
# The three pieces above are each verified in isolation.  What the player
# actually sees is their COMPOSITION, and that is what this runs: one
# continuous session that, per RENDERED frame, executes the real pieces in
# the real order the retail main loop runs them.
#
# THE RETAIL MAIN LOOP  --  FUN_000165F0 @0x00016ABC..0x00016C37 [C-disasm]
#
#   0x00016ABC  if ([EBP+0x2E20C] <= 0) skip the sim entirely
#   0x00016AD0  do {                                   ; the SIM-TICK loop
#   0x00016ADC     INC [0x004A1EB4]                    ; the tick counter
#   0x00016AFC     CALL FUN_001B5AC0                   ; the other timer
#   0x00016B06     CALL FUN_001B5AC0 (ECX=0x0060EA00)  ; THE GAME TIMER
#                  ... the whole game update, at dt = DAT_0060EA1C ...
#   0x00016BFE  } while (++i < [EBP+0x2E20C])
#   0x00016C10  MOV EDI,EBP ; CALL FUN_000170B0        ; camera + render, ONCE
#   0x00016C31  CALL FUN_001B58E0(1,4)                 ; ticks for NEXT frame
#   0x00016C37  MOV [EBP+0x2E20C],EAX
#
# so DAT_004A1EB4 is a SIM-TICK counter, not a rendered-frame counter, and at
# a solid frame rate the governor returns 1 -- one sim tick per rendered
# frame at dt = period/divisor.  That is the arrangement modelled here.
#
# THE CAMERA'S dt  --  FUN_000170B0 @0x00017147..0x0001717A [C-disasm]
#
#   00017147  CVTSI2SS XMM0,dword ptr [EDI + 0x2E20C]   ; the tick count
#   0001714F  MULSS    XMM0,dword ptr [0x0060EA1C]      ; * the DILATED dt
#   0001715C  MOVSS    dword ptr [EDI + 0x2E210],XMM0
#   0001716F  MOV EAX,dword ptr [ESP] ; PUSH EAX        ; -> param_1
#   0001717A  CALL FUN_00167940
#
# and inside FUN_00167940's 20-mode dispatch loop:
#
#   if (mode_index == 0xE || mode_index == 0) dt = DAT_004AE1FC ; the REAL dt
#   else                                      dt = param_1      ; the DILATED
#   call [mode->vtbl + 0x10](dt, camstate)
#
# The chase camera is mode 2, so **it is driven by ticks * DAT_0060EA1C --
# the DILATED dt** -- not the undilated one.  Only modes 0 and 14 get real
# time.  This matters enormously: FUN_0015E550's blend exponent is
# n = floor(60*dt + 0.5), so at divisor 5 (dt = 1/300) n = 0, the blend is
# 1 - x^0 = 0, and the camera's yaw and pitch FREEZE for the whole dilated
# window.  [C-disasm]
# ---------------------------------------------------------------------------

F_TICK = 0x001B5AC0        # the frame timer tick
G_TIMER = 0x0060EA00
G_DIV = 0x0060EA18
G_DIV_REQ = 0x0060EA24
G_RUNNING = 0x0060EA28
G_FRAMES = 0x004A1EB4      # DAT_004A1EB4, the SIM-TICK counter
G_PERIOD = 0x0049C120

DIV_NORMAL = 1
DIV_CRASH = 5              # 0x00025D5C  FUN_00025CC0, the wreck instant
DIV_IMPACT = 6             # 0x0002655B  FUN_00026050, the impact-hit window
IMPACT_DELAY = 0.05        # 0x00026B18  arm = clock + 0.05   [0x003A69BC]
IMPACT_LEN = 0.35          # 0x00026500  expiry              [0x0039B2B0]


def _mat_angle(a, b):
    """Angle of the rotation taking basis `a` to basis `b` (both are the
    three body axes expressed in world coords, i.e. matrix ROWS)."""
    tr = 0.0
    for i in range(3):
        tr += sum(a[i][k] * b[i][k] for k in range(3))
    c = (tr - 1.0) * 0.5
    return math.acos(max(-1.0, min(1.0, c)))


class CrashSequence(CrashTraj):
    """The crash presentation, executed per RENDERED frame.

    Per rendered frame, in the retail order:
      1. the REAL frame timer tick   (FUN_001B5AC0) -> divisor, dt, clock
      2. the REAL crashed vehicle step (FUN_0011BE50's crashed path) at
         dt = period/divisor
      3. the impact-hit machine (FUN_00026050's arithmetic) on the DILATED
         clock, writing the request the NEXT tick will apply
      4. the REAL chase camera (FUN_0015E550) on ticks * dilated dt
    """

    def __init__(self, period=1.0 / 60.0, prepass=True):
        super().__init__(dt=period, prepass=prepass)
        self.period = period
        self.timer_driven = True
        self.ticks = 0
        # the frame timer, seeded exactly as the game boots it
        self.wf(G_PERIOD, period)
        for off in range(0, 0x2C, 4):
            self.wu(G_TIMER + off, 0)
        self.wu(G_DIV, 1)
        self.wu(G_DIV_REQ, 1)
        self.wu(G_RUNNING, 1)
        self.wf(ep.G_DT, period)
        self.wu(G_FRAMES, 0)
        # the presentation state machine
        self.impact_armed = False
        self.impact_start = 0.0
        self.crash_req_pending = False
        self.aftertouch = False       # FUN_00118410: boost held while crashed
        self._cam = None
        self.cam_yaw = 0.0
        self.cam_pitch = 0.0

    # -- the presentation writers, in the retail per-frame order -----------
    def _divisor_writers(self):
        """RE_TAKEDOWN_FX section 9.3's table, in the order the frame runs
        them.  Each writes the single request slot DAT_0060EA24; the tick at
        the top of the NEXT frame applies it."""
        clock = self.rf(ep.G_CLOCK)
        # 1. aftertouch (FUN_00118410 @0x001188A4 / 0x001188D6)
        if self.aftertouch:
            self.wu(G_DIV_REQ, DIV_CRASH)
        # 2. the wreck instant (FUN_00025CC0 @0x00025D5C) -- fires ONCE
        if self.crash_req_pending:
            self.wu(G_DIV_REQ, DIV_CRASH)
            self.crash_req_pending = False
        # 3. the impact-hit machine (FUN_00026050 @0x00026500), DILATED clock
        if self.impact_armed:
            el = clock - self.impact_start
            if el > IMPACT_LEN:
                self.wu(G_DIV_REQ, DIV_NORMAL)      # 0x00026525
                self.impact_armed = False
            elif el > 0.0:
                self.wu(G_DIV_REQ, DIV_IMPACT)      # 0x0002655B

    def reset_timer(self):
        """Re-zero the frame timer at the crash instant.

        The warm-up frames drive DAT_0060EA20 from the Pipeline's own python
        clock; from here on the REAL timer owns it and restarts its
        whole/rem accumulators at 0, so the presentation's clock references
        (the impact window's arm stamp) have to be taken on the same base.
        """
        for off in range(0, 0x2C, 4):
            self.wu(G_TIMER + off, 0)
        self.wu(G_DIV, 1)
        self.wu(G_DIV_REQ, 1)
        self.wu(G_RUNNING, 1)
        self.wf(ep.G_DT, self.period)
        self.wf(ep.G_CLOCK, 0.0)
        self.wu(G_FRAMES, 0)
        self.ticks = 0

    def begin_presentation(self, arm_impact):
        """The wreck instant: FUN_00025850 -> FUN_00025CC0 requests divisor 5,
        and a BIG HIT (the game-context 'big hit' virtual FUN_00026A70, whose
        only caller is the car-vs-OBJECT response FUN_00112E70 @0x001134EF)
        additionally arms the impact window at clock + 0.05."""
        self.crash_req_pending = True
        if arm_impact:
            self.impact_armed = True
            self.impact_start = self.rf(ep.G_CLOCK) + IMPACT_DELAY

    # -- the camera --------------------------------------------------------
    def _camera(self, st, dt_cam):
        if self._cam is None:
            _s = importlib.util.spec_from_file_location(
                "tdcam", os.path.join(_here, "emulate_tdfx_camera.py"))
            self._cam = importlib.util.module_from_spec(_s)
            _s.loader.exec_module(self._cam)
        rows = [st['right'], st['up'], st['at'], st['pos']]
        r = self._cam.follow_update(rows, st['speed'], dt_cam,
                                    self.cam_yaw, self.cam_pitch)
        self.cam_yaw = r['yaw']
        self.cam_pitch = r['pitch']
        return r

    # -- one RENDERED frame -------------------------------------------------
    def render_frame(self, prev_rows=None, camera=True):
        # 1. the REAL timer tick (0x00016ADC..0x00016B06)
        self.ticks += 1
        self.wu(G_FRAMES, self.ticks)
        self.call(F_TICK, regs={UC_X86_REG_ECX: G_TIMER})
        divisor = self.ru(G_DIV)
        dt = self.rf(ep.G_DT)
        # 2. the REAL crashed vehicle step at the frame-locked dt
        st = self.crashed_frame(dt=dt)
        # 3. the impact-hit machine, on the clock the tick just advanced
        self._divisor_writers()
        # 4. the camera, on ticks * the DILATED dt (0x00017147)
        cam = self._camera(st, dt) if camera else None
        rows = [st['right'], st['up'], st['at']]
        st = dict(st)
        st['divisor'] = divisor
        st['dt'] = dt
        st['clock'] = self.rf(ep.G_CLOCK)
        # THE THING THE EYE SEES: how far the shell turned in THIS frame
        st['apparent_rot'] = (_mat_angle(prev_rows, rows)
                              if prev_rows else 0.0)
        st['omega_mag'] = math.sqrt(sum(w * w for w in st['omega']))
        if cam:
            st['cam_eye'] = cam['eye']
            st['cam_fov'] = cam['fov']
            st['cam_yaw'] = cam['yaw']
            st['cam_pitch'] = cam['pitch']
        return st, rows


def _seq(entry_speed, corner, mag_spin, mag_launch, wall, frames,
         arm_impact, period=1.0 / 60.0, camera=True, aftertouch=False):
    """Warm a real car up to `entry_speed`, crash it for real, then run
    `frames` RENDERED frames of the whole presentation."""
    sim = CrashSequence(period=period)
    # warm up on the real racing pipeline until the target entry speed
    for _ in range(900):
        sim.frame(1.0, 0.0, 0.0, 0)
        if sim.vf(0xBC) >= entry_speed:
            break
    speed = sim.vf(0xBC)
    if wall:
        z = sim.rf(CTX0 + 0x38)
        _wall_soup(sim, z + 3.0)
        sim.call(F_AEF0, regs={UC_X86_REG_ECX: VEHICLE})
    pre = sim.capture6()
    sim.enter_crash(corner, mag_spin, mag_launch)
    sim.aftertouch = aftertouch
    sim.reset_timer()
    sim.begin_presentation(arm_impact)
    series, prev = [], None
    for _ in range(frames):
        st, prev = sim.render_frame(prev_rows=prev, camera=camera)
        series.append(st)
    return dict(entry_speed=speed, pre=pre, series=series,
                body=sim.export_body())


def seq_wall35(frames=300, arm_impact=False, camera=True, **kw):
    """~35 m/s head-on wall crash: the REAL FUN_0011AEF0 resolves the wall,
    then the retail head-on entry pair (FUN_0010DD20 @0x0010E3C9's branch:
    a REAR torque kick then the linear launch)."""
    return _seq(35.0, KICK_REAR, MAG_ROLLOVER_SPIN, MAG_ROLLOVER_LAUNCH,
                True, frames, arm_impact, camera=camera, **kw)


def seq_slam(frames=300, arm_impact=False, camera=True, **kw):
    """Car-vs-car fatal at speed: FUN_00024F10's crash-record entry, the
    rear-left corner torque at 0.40 (0x00024F94) plus the linear launch."""
    return _seq(40.0, KICK_REAR | KICK_LEFT, MAG_TAKEDOWN_SPIN,
                MAG_ROLLOVER_LAUNCH, False, frames, arm_impact,
                camera=camera, **kw)


SEQUENCES = {
    'seq_wall35': seq_wall35,
    'seq_slam': seq_slam,
}


def summarise_seq(name, r):
    s = r['series']
    print("== %s ==  entry %.2f m/s, %d rendered frames" %
          (name, r['entry_speed'], len(s)))
    # divisor timeline
    runs, cur, n0 = [], s[0]['divisor'], 0
    for i, f in enumerate(s):
        if f['divisor'] != cur:
            runs.append((cur, i - n0))
            cur, n0 = f['divisor'], i
    runs.append((cur, len(s) - n0))
    print("   divisor timeline (rendered frames): " +
          "  ".join("div%d x%d (%.2f s wall)" % (d, n, n / 60.0)
                    for d, n in runs))
    tot = sum(f['apparent_rot'] for f in s)
    peak = max(f['apparent_rot'] for f in s)
    print("   APPARENT rotation: %.1f deg total, peak %.2f deg/frame, "
          "mean %.2f deg/frame" %
          (math.degrees(tot), math.degrees(peak),
           math.degrees(tot) / len(s)))
    y0 = r['pre']['pos'][1]
    for i in (0, 5, 15, 30, 60, 90, 140, 200, len(s) - 1):
        if i >= len(s):
            continue
        f = s[i]
        print("   f%3d div%d dt%.5f clk%6.3f  y%+7.3f  |w|%6.2f  "
              "APP%6.2f deg  cam(yaw%+7.2f pitch%+7.2f) air%d"
              % (i, f['divisor'], f['dt'], f['clock'], f['pos'][1] - y0,
                 f['omega_mag'], math.degrees(f['apparent_rot']),
                 f.get('cam_yaw', 0.0), f.get('cam_pitch', 0.0),
                 f['airborne']))


def summarise(name, speed, pre, traj):
    y0 = pre['pos'][1]
    peak = max(t['pos'][1] for t in traj)
    air = sum(1 for t in traj if t['airborne'])
    total_rot = 0.0
    for t in traj:
        total_rot += math.sqrt(sum(w * w for w in t['omega'])) / 60.0
    print("%-10s entry speed %6.2f m/s   launch dy %+6.2f m  peak y %+6.2f "
          "airborne %3d/%d frames  total rotation %6.2f rad (%.2f turns)"
          % (name, speed, traj[0]['pos'][1] - y0, peak - y0, air, len(traj),
             total_rot, total_rot / (2 * math.pi)))
    for i in (0, 5, 15, 30, 60, 90, 120, len(traj) - 1):
        if i >= len(traj):
            continue
        t = traj[i]
        print("   f%3d y%+7.3f  vy%+7.2f  up(%+.2f,%+.2f,%+.2f) "
              "w(%+6.2f,%+6.2f,%+6.2f) air%d settle%d"
              % (i, t['pos'][1] - y0, t['vel'][1], t['up'][0], t['up'][1],
                 t['up'][2], t['omega'][0], t['omega'][1], t['omega'][2],
                 t['airborne'], t['settle']))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    dump = '--dump' in sys.argv
    names = args or list(SCENARIOS)
    for name in names:
        if name in SEQUENCES:
            for arm in (False, True):
                r = SEQUENCES[name](arm_impact=arm)
                summarise_seq("%s (impact window %s)"
                              % (name, "ARMED" if arm else "not armed"), r)
                if dump:
                    out = os.path.join(_here, '..', 'build',
                                       'crashseq_%s_%s.json'
                                       % (name, 'armed' if arm else 'plain'))
                    with open(out, 'w') as f:
                        json.dump(r, f)
                    print("   -> %s" % out)
            continue
        sim, speed, pre, traj = SCENARIOS[name]()
        summarise(name, speed, pre, traj)
        if dump:
            out = os.path.join(_here, '..', 'build',
                               'crashtraj_%s.json' % name)
            with open(out, 'w') as f:
                json.dump(dict(entry_speed=speed, pre=pre, traj=traj,
                               body=sim.export_body()), f)
            print("   -> %s" % out)


if __name__ == '__main__':
    main()
