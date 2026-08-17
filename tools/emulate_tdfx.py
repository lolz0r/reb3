#!/usr/bin/env python3
"""
Ground truth for the takedown-presentation FX: run Burnout 3's REAL
time-dilation and takedown-cinematic functions under Unicorn.

This is the acceptance oracle for src/burnout3_takedown.c, in exactly the
pattern tools/emulate_vehicle.py established for the physics: map
build/burnout3.elf at its true VAs, seed the real environment, execute the
actual x86, and read the post-state back out.  Nothing here transcribes the
decompiler -- the numbers come from the instructions running.

Functions executed for real:

  FUN_001B5AC0  the global frame timer tick (ECX = timer object).  Produces
                the per-frame dt at DAT_0060EA1C and the game clock at
                DAT_0060EA20 from the raw frame counter DAT_004A1EB4 and the
                nominal frame period DAT_0049C120.
  FUN_001B5B60  the divisor-change handler (ECX = timer object): applies the
                requested divisor at +0x24 to +0x18 and recomputes
                dt = period / divisor.  THIS is the time-dilation mechanism.
  FUN_00027A60  takedown-cinematic trigger gate (EDX = camera record).
  FUN_00027920  cinematic entry (ESI = camera record): arms the record,
                requests divisor 5, posts the HUD callout event.
  FUN_00027AD0  cinematic per-frame update (ESI = camera record): the
                divisor 5 -> 1 phase change, the delayed callout, the exit.
  FUN_000279C0  cinematic exit (EDX = camera record).

Stubbed (with the reason):
  FUN_00179760  camera-object reset -- pure presentation bookkeeping on a
                camera object we do not model; stubbing it keeps the
                cinematic state machine observable in isolation.
  FUN_00053D20  presentation-state broadcast -- walks a listener list that
                does not exist here; the stub records the posted state id.
  vtable+0x0C on DAT_004CFB20[idx]  -- the HUD callout post; the stub
                records (event id, argument).

Usage:  python3 tools/emulate_tdfx.py [section]
"""
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED, \
    UC_HOOK_CODE, UC_PROT_ALL, UcError
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP,
                               UC_X86_REG_EAX, UC_X86_REG_EBX,
                               UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI)

PAGE = 0x1000
_here = os.path.dirname(os.path.abspath(__file__))
ELF = os.path.join(os.path.dirname(_here), "build", "burnout3.elf")

# --------------------------------------------------------------------------
# real addresses (corrected VAs; .text = old flat + 0x10000)
# --------------------------------------------------------------------------
F_TICK      = 0x001B5AC0   # timer tick
F_RESCALE   = 0x001B5B60   # divisor change -> dt recompute
F_GATE      = 0x00027A60   # cinematic trigger gate
F_ENTER     = 0x00027920   # cinematic entry
F_UPDATE    = 0x00027AD0   # cinematic per-frame update
F_EXIT      = 0x000279C0   # cinematic exit
F_CAMSET    = 0x0018CB60   # takedown-camera enable/disable on the racecar
F_CAMRESET  = 0x00179760   # camera-object reset (STUBBED)
F_POSTSTATE = 0x00053D20   # presentation-state broadcast (STUBBED)

# globals
G_TIMER      = 0x0060EA00   # the gameplay (dilated) timer object
G_DT         = 0x0060EA1C   # = timer+0x1C  per-frame dt seconds
G_CLOCK      = 0x0060EA20   # = timer+0x20  game clock seconds
G_DIV        = 0x0060EA18   # = timer+0x18  current time divisor
G_DIV_REQ    = 0x0060EA24   # = timer+0x24  requested time divisor
G_RUNNING    = 0x0060EA28   # = timer+0x28  running flag
G_FRAMES     = 0x004A1EB4   # raw frame counter (incremented once per frame)
G_PERIOD     = 0x0049C120   # nominal frame period (1/60 NTSC, 1/50 PAL)
G_REALDT     = 0x004AE1FC   # real-time (undilated) per-frame dt
G_PITCH      = 0x003EBFD0   # global audio pitch/rate scale
G_RACECARS   = 0x0073A1D0   # racecar array base, stride 0x27E0
G_HUDOBJS    = 0x004CFB20   # per-player HUD object array, stride 0xC50
G_STATEOBJS  = 0x0054F900   # per-player presentation-state objects, stride 0x70
G_TDCAM_EN   = 0x004AE1DB   # global takedown-camera enable byte
G_PVTABLE    = 0x0064B38C   # grid slot -> physics vehicle, stride 0xC dwords

RACECAR_STRIDE = 0x27E0

# recovered constants (address -> value); asserted against the image
CONST = {
    0x00385A00: 4.8,     # cinematic total length (s)
    0x00395E88: 2.8,     # slow-motion phase length (s)
    0x003A2D50: 2.5,     # callout post time (s)
    0x003B1870: 1.5,     # max victim-crash age for the trigger (s)
    0x003B16E0: 0.0,
    0x003B168C: 1.0,
    0x003A55F8: 0.75,    # audio pitch scale while dilated
    0x0039B2B0: 0.35,    # impact-hit slow-mo length (s)
    0x003A69BC: 0.05,    # impact-hit slow-mo start delay (s)
    0x003B1838: 1.0 / 60.0,
    0x003B1A08: 0.02,
}

# --------------------------------------------------------------------------
# scratch layout
# --------------------------------------------------------------------------
STACK_BASE = 0x20000000
STACK_SIZE = 0x00100000
SCRATCH    = 0x30000000
SCRATCH_SZ = 0x00100000
MAGIC_RET  = 0x50000000
STUB_BASE  = 0x51000000     # fake code addresses used as stub targets


def _load_elf(uc, path):
    data = open(path, 'rb').read()
    ph_off = struct.unpack_from('<I', data, 0x1C)[0]
    ph_num = struct.unpack_from('<H', data, 0x2C)[0]
    segs = []
    for i in range(ph_num):
        p_type, off, va, _, fsz, _, msz, _ = (0,) * 8
        p_type, off, va, _pa, fsz, msz, _fl, _al = struct.unpack_from(
            '<IIIIIIII', data, ph_off + i * 32)
        if p_type == 1:
            segs.append((va, off, fsz, msz))
    lo = min(va for va, _, _, _ in segs) & ~(PAGE - 1)
    hi = max(va + msz for va, _, _, msz in segs)
    hi = (hi + PAGE - 1) & ~(PAGE - 1)
    uc.mem_map(lo, hi - lo, UC_PROT_ALL)
    for va, off, fsz, _ in segs:
        uc.mem_write(va, data[off:off + fsz])
    return lo, hi


class Emu:
    """One persistent Unicorn session with the image mapped at its true VAs."""

    def __init__(self):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        _load_elf(self.uc, ELF)
        self.uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
        self.uc.mem_map(SCRATCH, SCRATCH_SZ, UC_PROT_ALL)
        self.uc.mem_map(MAGIC_RET & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        self.uc.mem_map(STUB_BASE, PAGE, UC_PROT_ALL)
        self.faults = 0
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)
        self.stubs = {}          # addr -> (name, argbytes, callback)
        self.calls = []          # [(name, args...)]
        self.uc.hook_add(UC_HOOK_CODE, self._on_code)

    # -- memory helpers ---------------------------------------------------
    def r(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def w(self, a, b):
        self.uc.mem_write(a, bytes(b))

    def rf(self, a):
        return struct.unpack('<f', self.r(a, 4))[0]

    def wf(self, a, v):
        self.w(a, struct.pack('<f', float(v)))

    def ri(self, a):
        return struct.unpack('<i', self.r(a, 4))[0]

    def ru(self, a):
        return struct.unpack('<I', self.r(a, 4))[0]

    def wi(self, a, v):
        self.w(a, struct.pack('<I', int(v) & 0xFFFFFFFF))

    def rb(self, a):
        return self.r(a, 1)[0]

    def wb(self, a, v):
        self.w(a, bytes([int(v) & 0xFF]))

    # -- stubs ------------------------------------------------------------
    def _on_unmapped(self, uc, access, address, size, value, user):
        page = address & ~(PAGE - 1)
        try:
            uc.mem_map(page, PAGE, UC_PROT_ALL)
            self.faults += 1
        except UcError:
            return False
        return True

    def _on_code(self, uc, address, size, user):
        st = self.stubs.get(address)
        if st is None:
            return
        name, argbytes, cb = st
        esp = uc.reg_read(UC_X86_REG_ESP)
        ret = struct.unpack('<I', bytes(uc.mem_read(esp, 4)))[0]
        args = [struct.unpack('<I', bytes(uc.mem_read(esp + 4 + 4 * i, 4)))[0]
                for i in range(4)]
        regs = dict(eax=uc.reg_read(UC_X86_REG_EAX),
                    ecx=uc.reg_read(UC_X86_REG_ECX),
                    edx=uc.reg_read(UC_X86_REG_EDX),
                    esi=uc.reg_read(UC_X86_REG_ESI),
                    edi=uc.reg_read(UC_X86_REG_EDI))
        self.calls.append((name, args, regs))
        if cb is not None:
            cb(self, args, regs)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + argbytes)
        uc.reg_write(UC_X86_REG_EIP, ret)

    def stub(self, addr, name, argbytes=0, cb=None):
        self.stubs[addr] = (name, argbytes, cb)

    # -- call -------------------------------------------------------------
    def call(self, addr, regs=None, stack_args=(), max_steps=400000):
        uc = self.uc
        esp = STACK_BASE + STACK_SIZE - 0x400
        for i, v in enumerate(reversed(stack_args)):
            esp -= 4
            uc.mem_write(esp, struct.pack('<I', v & 0xFFFFFFFF))
        esp -= 4
        uc.mem_write(esp, struct.pack('<I', MAGIC_RET))
        uc.reg_write(UC_X86_REG_ESP, esp)
        for rc in (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                   UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI):
            uc.reg_write(rc, 0)
        for k, v in (regs or {}).items():
            uc.reg_write(k, v & 0xFFFFFFFF)
        self.calls = []
        uc.emu_start(addr, MAGIC_RET, count=max_steps)
        return uc.reg_read(UC_X86_REG_EAX)


# --------------------------------------------------------------------------
# environment seeding
# --------------------------------------------------------------------------
def seed_common(e, period=1.0 / 60.0, real_dt=1.0 / 60.0):
    """The globals every section needs."""
    e.wf(G_PERIOD, period)
    e.wf(G_REALDT, real_dt)
    e.wf(G_PITCH, 1.0)
    e.wi(G_FRAMES, 0)
    # timer object: running, divisor 1, clean
    for off in range(0, 0x2C, 4):
        e.wi(G_TIMER + off, 0)
    e.wi(G_DIV, 1)
    e.wi(G_DIV_REQ, 1)
    e.wi(G_RUNNING, 1)
    e.wf(G_DT, period)


def racecar(slot):
    return G_RACECARS + slot * RACECAR_STRIDE


def seed_racecar(e, slot, cls=0, crashed=0, grid=None, player_index=0):
    base = racecar(slot)
    e.w(base, b'\0' * 0x27E0)
    e.wi(base + 0x1920, cls)                 # class (0 = human)
    e.wb(base + 0x18FA, crashed)             # crashed flag
    e.wb(base + 0x19BC, slot if grid is None else grid)
    e.wi(base + 0x134C, 0)                   # race state (3 = finished)
    e.wi(base + 0x1320, -1)                  # signature-takedown selector
    e.wb(base + 0x11EE, 0)                   # burnout-chain flag
    e.wb(base + 0x11F1, 0)                   # boost ramp-done byte
    e.wi(base + 0x27D0, player_index)        # local player index
    e.wb(base + 0x27D8, 0)                   # takedown-camera-active flag
    e.wf(base + 0x10DC, 0.0)                 # race clock
    e.wf(base + 0x140C, 0.0)                 # crash-start stamp
    e.wi(base + 0x2440, 0)
    return base


def seed_vehicle(e, addr):
    e.w(addr, b'\0' * 0x2000)
    e.wf(addr + 0x1534, 0.25)                # steering authority
    e.wi(addr + 0x1524, 4)                   # drift/camera state
    e.wb(addr + 0x1353, 0)
    return addr


def seed_cinematic_env(e, slot=0, player_index=0):
    """Everything FUN_00027920/AD0/79C0 touch beyond the record itself."""
    pv = SCRATCH + 0x8000
    seed_vehicle(e, pv)
    rc = racecar(slot)
    e.wi(rc + 0x2440, pv)
    # grid-slot -> physics vehicle table (stride 0xC dwords = 0x30 bytes)
    grid = e.rb(rc + 0x19BC)
    e.wi(G_PVTABLE + grid * 0x30, pv)
    # per-player HUD object with a vtable whose +0x0C is a recording stub
    hud = G_HUDOBJS + player_index * 0xC50
    vtbl = SCRATCH + 0x9000
    e.wi(hud, vtbl)
    e.wi(vtbl + 0x0C, STUB_BASE + 0x10)
    e.stub(STUB_BASE + 0x10, 'hud_post', argbytes=8)
    # presentation-state broadcast + camera reset
    e.stub(F_POSTSTATE, 'post_state', argbytes=0)
    e.stub(F_CAMRESET, 'camera_reset', argbytes=0)
    e.wb(G_TDCAM_EN, 1)
    return pv


CAMREC = SCRATCH + 0x1000   # a standalone 0x10-byte cinematic record


def seed_record(e, slot=0, victim=0, elapsed=-1.0, active=0, flag_c=0,
                callout_done=0, chain=0):
    e.w(CAMREC, b'\0' * 0x10)
    e.wi(CAMREC + 0x00, victim)
    e.wf(CAMREC + 0x04, elapsed)
    e.wi(CAMREC + 0x08, slot)
    e.wb(CAMREC + 0x0C, flag_c)
    e.wb(CAMREC + 0x0D, callout_done)
    e.wb(CAMREC + 0x0E, active)
    e.wb(CAMREC + 0x0F, chain)
    return CAMREC


def read_record(e):
    return dict(victim=e.ru(CAMREC + 0x00),
                elapsed=e.rf(CAMREC + 0x04),
                slot=e.ru(CAMREC + 0x08),
                flag_c=e.rb(CAMREC + 0x0C),
                callout_done=e.rb(CAMREC + 0x0D),
                active=e.rb(CAMREC + 0x0E),
                chain=e.rb(CAMREC + 0x0F))


# --------------------------------------------------------------------------
# section 1: the time-dilation mechanism
# --------------------------------------------------------------------------
def run_timer(divisor_schedule, period=1.0 / 60.0, frames=None):
    """Tick the REAL frame timer once per entry of divisor_schedule.

    Returns a list of per-frame dicts {frame, req, div, dt, clock}.
    """
    e = Emu()
    seed_common(e, period=period)
    # the dt field is only written by the rescale handler, so prime it the way
    # the game does: request the starting divisor and let the real code apply it
    out = []
    for i, req in enumerate(divisor_schedule):
        e.wi(G_DIV_REQ, req)
        e.wi(G_FRAMES, i + 1)
        e.call(F_TICK, regs={UC_X86_REG_ECX: G_TIMER})
        out.append(dict(frame=i + 1,
                        req=req,
                        div=e.ri(G_DIV),
                        dt=e.rf(G_DT),
                        clock=e.rf(G_CLOCK)))
    return out


def run_rescale(cur_div, req_div, whole, rem, period=1.0 / 60.0):
    """Execute FUN_001B5B60 alone over a seeded timer state."""
    e = Emu()
    seed_common(e, period=period)
    e.wi(G_TIMER + 0x0C, whole)
    e.wi(G_TIMER + 0x10, rem)
    e.wi(G_DIV, cur_div)
    e.wi(G_DIV_REQ, req_div)
    e.call(F_RESCALE, regs={UC_X86_REG_ECX: G_TIMER})
    return dict(div=e.ri(G_DIV), dt=e.rf(G_DT),
                whole=e.ri(G_TIMER + 0x0C), rem=e.ri(G_TIMER + 0x10))


# --------------------------------------------------------------------------
# section 2: the cinematic trigger gate  (FUN_00027A60)
# --------------------------------------------------------------------------
def run_gate(*, victim_slot=1, attacker_slot=0, victim_crashed=1,
             attacker_crashed=0, cam_active=0, last_victim=0,
             victim_clock=1.0, victim_crash_stamp=0.5, victim_ptr=None):
    e = Emu()
    seed_common(e)
    a = seed_racecar(e, attacker_slot, cls=0, crashed=attacker_crashed)
    v = seed_racecar(e, victim_slot, cls=1, crashed=victim_crashed)
    e.wf(v + 0x10DC, victim_clock)
    e.wf(v + 0x140C, victim_crash_stamp)
    e.wi(a + 0x15A4, v if victim_ptr is None else victim_ptr)
    seed_record(e, slot=attacker_slot, victim=last_victim, active=cam_active)
    al = e.call(F_GATE, regs={UC_X86_REG_EDX: CAMREC}) & 0xFF
    return al


# --------------------------------------------------------------------------
# section 3: cinematic entry  (FUN_00027920)
# --------------------------------------------------------------------------
def run_enter(*, attacker_slot=0, victim_slot=1, already_active=0,
              signature=-1, chain_flag=0, player_index=0):
    e = Emu()
    seed_common(e)
    a = seed_racecar(e, attacker_slot, cls=0, player_index=player_index)
    v = seed_racecar(e, victim_slot, cls=1, crashed=1, grid=victim_slot)
    e.wi(a + 0x15A4, v)
    e.wi(a + 0x1320, signature)
    e.wb(a + 0x11EE, chain_flag)
    seed_cinematic_env(e, slot=attacker_slot, player_index=player_index)
    seed_record(e, slot=attacker_slot, active=already_active, elapsed=-1.0)
    e.call(F_ENTER, regs={UC_X86_REG_ESI: CAMREC})
    res = read_record(e)
    res['div_req'] = e.ri(G_DIV_REQ)
    res['calls'] = [(n, a_[0], a_[1], r['eax']) for n, a_, r in e.calls]
    # the HUD post's second argument is a pointer to the victim grid slot
    res['hud_events'] = [(a_[0], e.ri(a_[1])) for n, a_, r in e.calls
                         if n == 'hud_post']
    res['states'] = [r['eax'] for n, a_, r in e.calls if n == 'post_state']
    return res


# --------------------------------------------------------------------------
# section 4: cinematic update  (FUN_00027AD0)
# --------------------------------------------------------------------------
def run_update_series(*, attacker_slot=0, victim_slot=1, real_dt=1.0 / 60.0,
                      nframes=320, attacker_crashes_at=None,
                      race_state_3_at=None, chain_flag=0, ramp_done=0,
                      player_index=0):
    """Drive one armed record through the real per-frame update."""
    e = Emu()
    seed_common(e, real_dt=real_dt)
    a = seed_racecar(e, attacker_slot, cls=0, player_index=player_index)
    v = seed_racecar(e, victim_slot, cls=1, crashed=1, grid=victim_slot)
    e.wi(a + 0x15A4, v)
    e.wb(a + 0x11F1, ramp_done)
    pv = seed_cinematic_env(e, slot=attacker_slot, player_index=player_index)
    seed_record(e, slot=attacker_slot, victim=v, elapsed=0.0, active=1,
                flag_c=1, chain=chain_flag)
    e.wi(G_DIV_REQ, 5)

    frames = []
    for i in range(nframes):
        if attacker_crashes_at is not None and i == attacker_crashes_at:
            e.wb(a + 0x18FA, 1)
        if race_state_3_at is not None and i == race_state_3_at:
            e.wi(a + 0x134C, 3)
        e.call(F_UPDATE, regs={UC_X86_REG_ESI: CAMREC})
        rec = read_record(e)
        frames.append(dict(
            i=i,
            t=rec['elapsed'],
            active=rec['active'],
            callout_done=rec['callout_done'],
            flag_c=rec['flag_c'],
            div_req=e.ri(G_DIV_REQ),
            cam_on=e.rb(a + 0x27D8),
            hud_flag=e.rb(a + 0x245D),
            burnout_grant=e.rb(a + 0x2417),
            pv_flags=e.rb(pv + 0x1353),
            pv_auth=e.rf(pv + 0x1534),
            pv_drift=e.ri(pv + 0x1524),
            hud_events=[(ar[0], e.ri(ar[1])) for n, ar, r in e.calls
                        if n == 'hud_post'],
            states=[r['eax'] for n, ar, r in e.calls if n == 'post_state'],
        ))
        if rec['active'] == 0:
            break
    return frames


# --------------------------------------------------------------------------
# section 5: cinematic exit  (FUN_000279C0)
# --------------------------------------------------------------------------
def run_exit(*, attacker_slot=0, cam_on=1, drift_state=4, player_index=0):
    e = Emu()
    seed_common(e)
    a = seed_racecar(e, attacker_slot, cls=0, player_index=player_index)
    pv = seed_cinematic_env(e, slot=attacker_slot, player_index=player_index)
    e.wb(a + 0x27D8, cam_on)
    e.wi(pv + 0x1524, drift_state)
    e.wf(pv + 0x1534, 0.1)
    e.wb(a + 0x245D, 1)
    seed_record(e, slot=attacker_slot, victim=0x11223344, elapsed=3.0,
                active=1, flag_c=1)
    e.wi(G_DIV_REQ, 5)
    e.call(F_EXIT, regs={UC_X86_REG_EDX: CAMREC})
    rec = read_record(e)
    rec['div_req'] = e.ri(G_DIV_REQ)
    rec['cam_on'] = e.rb(a + 0x27D8)
    rec['hud_flag'] = e.rb(a + 0x245D)
    rec['pv_auth'] = e.rf(pv + 0x1534)
    rec['pv_drift'] = e.ri(pv + 0x1524)
    return rec


# --------------------------------------------------------------------------
# constants read straight out of the mapped image
# --------------------------------------------------------------------------
def read_constants():
    e = Emu()
    return {a: e.rf(a) for a in CONST}


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else 'all'

    if which in ('all', 'const'):
        print("== image constants ==")
        got = read_constants()
        for a, want in CONST.items():
            v = got[a]
            ok = abs(v - want) < 1e-6
            print("  0x%08X = %-12g expect %-12g %s"
                  % (a, v, want, "OK" if ok else "MISMATCH"))

    if which in ('all', 'timer'):
        print("\n== time dilation: real FUN_001B5AC0 / FUN_001B5B60 ==")
        sched = [1] * 3 + [5] * 5 + [1] * 3
        for f in run_timer(sched):
            print("  frame %2d req=%d div=%d dt=%.9f clock=%.9f"
                  % (f['frame'], f['req'], f['div'], f['dt'], f['clock']))
        print("  rescale 1->5:", run_rescale(1, 5, 10, 0))
        print("  rescale 5->1 (rem 3):", run_rescale(5, 1, 10, 3))
        print("  rescale 5->1 (rem 2):", run_rescale(5, 1, 10, 2))

    if which in ('all', 'gate'):
        print("\n== trigger gate: real FUN_00027A60 ==")
        print("  nominal              ->", run_gate())
        print("  victim not crashed   ->", run_gate(victim_crashed=0))
        print("  attacker crashed     ->", run_gate(attacker_crashed=1))
        print("  camera already up    ->", run_gate(cam_active=1))
        print("  crash 1.4s ago       ->", run_gate(victim_clock=1.9,
                                                    victim_crash_stamp=0.5))
        print("  crash 1.6s ago       ->", run_gate(victim_clock=2.1,
                                                    victim_crash_stamp=0.5))
        print("  no victim            ->", run_gate(victim_ptr=0))

    if which in ('all', 'enter'):
        print("\n== cinematic entry: real FUN_00027920 ==")
        r = run_enter()
        print("  record:", {k: r[k] for k in
                            ('victim', 'elapsed', 'active', 'flag_c',
                             'callout_done', 'chain')})
        print("  divisor request:", r['div_req'])
        print("  HUD events (id, arg):", r['hud_events'])
        print("  presentation states:", r['states'])
        r2 = run_enter(signature=7)
        print("  signature!=-1 HUD events:", r2['hud_events'])
        r3 = run_enter(already_active=1)
        print("  re-entry blocked (active stays 1, no events):",
              r3['active'], r3['hud_events'])

    if which in ('all', 'update'):
        print("\n== cinematic update: real FUN_00027AD0 ==")
        fr = run_update_series()
        div_changes = [(f['t'], f['div_req']) for i, f in enumerate(fr)
                       if i == 0 or f['div_req'] != fr[i - 1]['div_req']]
        print("  frames run:", len(fr))
        print("  divisor changes (t, div):",
              [(round(t, 4), d) for t, d in div_changes])
        ev = [(round(f['t'], 4), f['hud_events'], f['states'])
              for f in fr if f['hud_events'] or f['states']]
        print("  callout posts:", ev)
        cam = [(round(f['t'], 4), f['cam_on']) for i, f in enumerate(fr)
               if i == 0 or f['cam_on'] != fr[i - 1]['cam_on']]
        print("  camera-on changes:", cam)
        print("  last frame:", {k: fr[-1][k] for k in
                                ('t', 'active', 'div_req', 'cam_on',
                                 'pv_flags', 'pv_auth', 'pv_drift')})
        fr2 = run_update_series(attacker_crashes_at=30)
        print("  attacker crashes at frame 30 -> ended after %d frames, "
              "div=%d" % (len(fr2), fr2[-1]['div_req']))

    if which in ('all', 'exit'):
        print("\n== cinematic exit: real FUN_000279C0 ==")
        print(" ", run_exit())


if __name__ == '__main__':
    main()
