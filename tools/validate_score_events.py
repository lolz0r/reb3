#!/usr/bin/env python3
"""Differential test: the ported earn-event detectors vs the REAL x86.

For every case this seeds a game-shaped score object (+ racecar, boost record,
category records, proximity objects), executes the ORIGINAL functions under
Unicorn over that memory, then runs the C port (build/dump_score_events, built
from src/burnout3_score_events.c) from the byte-identical seed and asserts the
two post-images agree on every field the detectors own.

Covered:
  FUN_00196940  AIR detector
  FUN_00196BE0  ONCOMING detector
  FUN_00196E10  DRIFT detector
  FUN_00192D20  category tier tracker (exercised through all of the above)
  FUN_00195DD0  near-miss OBB proximity test
  FUN_00194EE0  NEAR MISS detector (multi-frame sequences)
  FUN_0017A530  boost award (executed for real; the meter is asserted)
  FUN_001935F0  the CRASH GATE + its reset block -- the real slice
                0x001939AD..0x00193CE4 is executed, so the crashed cases run
                the game's own branch decision and its own reset
  FUN_00197920  CONTACT -> near-miss cancel (the takedown/crash classifier)
  FUN_001979E0  CONTACT -> the per-opponent rubbing arrays
  FUN_00194A80  RUBBING, one frame

Run:  python3 tools/validate_score_events.py
"""
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emulate_vehicle as ev  # noqa: E402  (ELF loader + Unicorn plumbing)

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_CODE, UcError, UC_PROT_ALL)
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EDI,
                               UC_X86_REG_ESI)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(ROOT, "build", "dump_score_events")

# --- real function addresses (burnout3.elf VAs) ---------------------------
F_AIR      = 0x00196940
F_ONCOMING = 0x00196BE0
F_DRIFT    = 0x00196E10
F_NEARMISS = 0x00194EE0
F_CONTACT  = 0x00197920      # near-miss cancel        (regparm3: _, obj, score)
F_MARK     = 0x001979E0      # rubbing contact mark    (regparm3: score, _, car)
F_RUBBING  = 0x00194A80      # rubbing, one frame      (score in ESI)
# FUN_00194A80's payout call; not ported (FUN_0019A050 is the shared
# aggression/combo payout), so the differential skips over it.
RUB_PAYOUT_CALL = 0x00194E90
RUB_PAYOUT_NEXT = 0x00194E95

# rubbing globals
D_CARCOUNT = 0x0073A19C      # DAT_0073A19C  racer count
# The loop indexes [EDI + ECX] with EDI = score+0x528+i*4 and
# ECX = 0x00739C80 - score, i.e. *(0x00739C80 + 0x528 + i*4) --
# 0x00739C80 + 0x528 == 0x0073A1A8, the SAME array the payout tail uses as
# (&DAT_0073A1A8)[score+0x580].  [C @0x00194AAD/0x00194AB3/0x00194AD0]
D_CARLIST  = 0x0073A1A8      # racecar pointer per racer slot
D_DT_RUB   = 0x004AE1FC      # the per-frame step the rub timer grows by
D_DT_ACC   = 0x0060EA1C      # the per-frame step the record accumulates
P_RUB_MIN_T   = 0x3F758C     # score+0x570 rubbing minima  (3 floats)
P_RUB_MIN_S   = 0x3F73F0     # Min Contact Time For Rubbing (Seconds)
P_RUB_BOOST_S = 0x3F73F4     # Boost value for rubbing (Boost units)
P_RUB_GRACE   = 0x3F7408     # Maximum Crash Wait Time - No Slam (Seconds)

# --- FUN_001935F0's crash gate, executed as a real code slice -------------
#   0x001939AD  MOV AL,[ESI+0x18fa] ; TEST AL,AL ; JNZ 0x00193a80  (crashed)
#   0x001939D1  MOV AL,[ESI+0x18fb] ; TEST AL,AL ; JNZ 0x00193a80  (respawn)
#   0x001939DF  LEA EAX,[ESP+0x10]  ; CALL FUN_00013C10 -> the detectors
#   0x00193A80  PUSH EDI ; CALL FUN_00197040 -> the reset block
#   0x00193CE4  the two branches join
GATE_ENTRY = 0x001939AD
GATE_RUN   = 0x001939DF
GATE_SKIP  = 0x00193A80
GATE_JOIN  = 0x00193CE4

# --- memory layout --------------------------------------------------------
SCORE   = ev.SCRATCH + 0x10000
RACECAR = ev.SCRATCH + 0x20000
BOX_ME  = ev.SCRATCH + 0x30000   # racecar+0x50 -> box info (+0xE80/+0xE90)
PV      = ev.SCRATCH + 0x31000   # racecar+0x2440 -> physics vehicle
CLKSRC  = ev.SCRATCH + 0x32000   # category records' clock source (+0xC)
BOX_OT  = ev.SCRATCH + 0x33000   # per-candidate box info, stride 0x100
CODE    = ev.SCRATCH + 0x34000   # trampoline
GAME_CTX = 0x004D5370

# proximity broadphase globals + object table (FUN_00194EE0 @0x00194EFF..)
P_COUNTS = 0x00649B3C            # [slot] -> candidate count
P_LIST   = 0x00649A8E            # [slot*8 + j] -> candidate id
OBJ_BASE = 0x00625FB0            # object table, stride 0x180
OBJ_STRIDE = 0x180

SCORE_SZ = 0x600

# --- parameter storage globals (registrar FUN_00190430) -------------------
P_AIR_MIN, P_AIR_PER = 0x3F73A0, 0x3F73A4
P_ONC_MPH, P_ONC_MIN, P_ONC_PER = 0x3F73A8, 0x3F73AC, 0x3F73B0
P_DRF_MPH, P_DRF_MIN, P_DRF_PER = 0x3F73B4, 0x3F73B8, 0x3F73BC
P_NM_DIST, P_NM_MPH, P_NM_CHAIN_MPH = 0x3F73C0, 0x3F73C4, 0x3F73C8
P_NM_BOOST, P_NM_CHAIN_T = 0x3F73CC, 0x3F73D0
P_AIR_BP, P_ONC_BP, P_DRF_BP, P_NM_BP_CAT = 0x3F7490, 0x3F74A0, 0x3F74B0, 0x3F74E4
P_NM_BP_LINK = 0x3F7480
P_AIR_MIN_T, P_ONC_MIN_T, P_DRF_MIN_T, P_NM_MIN_T = 0x3F7550, 0x3F7560, 0x3F7570, 0x3F75A4

TUNES = {
    "default": dict(
        air=(5.0, 2.0), onc=(30.0, 10.0, 0.3), drift=(30.0, 10.0, 1.0),
        nm=(2.0, 30.0, 30.0, 60.0, 5.0),
        air_bp=(100, 250, 500, 1000), onc_bp=(100, 250, 500, 1000),
        drift_bp=(100, 250, 500, 1000), nm_bp=(5, 10, 15, 25), nm_link=20,
        air_min=(15.0, 30.0, 60.0, 90.0), onc_min=(50.0, 100.0, 200.0, 300.0),
        drift_min=(45.0, 90.0, 180.0, 270.0), nm_min=(999.0, 2.0, 5.0, 9.0)),
    "vdb": dict(
        air=(5.0, 0.3), onc=(50.0, 40.0, 0.15), drift=(90.0, 20.0, 0.1),
        nm=(1.8, 60.0, 60.0, 15.0, 2.5),
        air_bp=(0, 10, 50, 100), onc_bp=(0, 20, 75, 150),
        drift_bp=(0, 20, 50, 100), nm_bp=(5, 10, 15, 25), nm_link=0,
        air_min=(1115.0, 10.0, 20.0, 35.0), onc_min=(9150.0, 500.0, 900.0, 1400.0),
        drift_min=(9980.0, 100.0, 150.0, 250.0), nm_min=(999.0, 2.0, 5.0, 9.0)),
}


def f32(x):
    return struct.pack('<f', x)


def param_writes(tune):
    t = TUNES[tune]
    mw = {
        P_AIR_MIN: f32(t['air'][0]), P_AIR_PER: f32(t['air'][1]),
        P_ONC_MPH: f32(t['onc'][0]), P_ONC_MIN: f32(t['onc'][1]),
        P_ONC_PER: f32(t['onc'][2]),
        P_DRF_MPH: f32(t['drift'][0]), P_DRF_MIN: f32(t['drift'][1]),
        P_DRF_PER: f32(t['drift'][2]),
        P_NM_DIST: f32(t['nm'][0]), P_NM_MPH: f32(t['nm'][1]),
        P_NM_CHAIN_MPH: f32(t['nm'][2]), P_NM_BOOST: f32(t['nm'][3]),
        P_NM_CHAIN_T: f32(t['nm'][4]),
        P_NM_BP_LINK: struct.pack('<i', t['nm_link']),
    }
    for addr, key in ((P_AIR_BP, 'air_bp'), (P_ONC_BP, 'onc_bp'),
                      (P_DRF_BP, 'drift_bp'), (P_NM_BP_CAT, 'nm_bp')):
        mw[addr] = struct.pack('<4i', *t[key])
    for addr, key in ((P_AIR_MIN_T, 'air_min'), (P_ONC_MIN_T, 'onc_min'),
                      (P_DRF_MIN_T, 'drift_min'), (P_NM_MIN_T, 'nm_min')):
        mw[addr] = struct.pack('<4f', *t[key])
    return mw


# --- fake game context (same construction as tools/validate_gameplay.py) ---
def fake_ctx():
    G, O, VT = ev.SCRATCH + 0x7000, ev.SCRATCH + 0x7100, ev.SCRATCH + 0x7200
    ST_R1, ST_R0, ST_F1 = ev.SCRATCH + 0x7300, ev.SCRATCH + 0x7310, ev.SCRATCH + 0x7320
    ST_R0_8 = ev.SCRATCH + 0x7330
    mw = {
        GAME_CTX: struct.pack('<I', G),
        G + 0x1B8: struct.pack('<I', O),
        O: struct.pack('<I', VT),
        ST_R1: b"\xb8\x01\x00\x00\x00\xc3",
        ST_R0: b"\x31\xc0\xc3",
        ST_F1: b"\xd9\xe8\xc3",
        ST_R0_8: b"\x31\xc0\xc2\x08\x00",
    }
    for off, tgt in {0x40: ST_R1, 0x90: ST_R0, 0x94: ST_R0, 0xAC: ST_F1,
                     0x5C: ST_R0_8, 0x00: ST_R0, 0x04: ST_R0, 0x08: ST_R0,
                     0x0C: ST_R0}.items():
        mw[VT + off] = struct.pack('<I', tgt)
    return mw


# ==========================================================================
# score-object image construction
# ==========================================================================
REC_BASES = {'air': 0x358, 'onc': 0x374, 'drift': 0x390, 'nm': 0x418}
MINIMA_PTR = {'air': P_AIR_MIN_T, 'onc': P_ONC_MIN_T,
              'drift': P_DRF_MIN_T, 'nm': P_NM_MIN_T}
ACTIVE_OFF = {'air': 0x368, 'onc': 0x384, 'drift': 0x3A0, 'nm': 0x428}
SCORED_OFF = {'air': 0x3C8, 'onc': 0x3C9, 'drift': 0x3CA}


def blank_score(clock=0.0):
    """A score-object image with the four category records wired up."""
    img = bytearray(SCORE_SZ)
    struct.pack_into('<I', img, 0xC8, RACECAR)     # score+0xC8  -> racecar
    struct.pack_into('<I', img, 0x26C, RACECAR)    # score+0x26C -> racecar
    struct.pack_into('<f', img, 0x0C, clock)       # score+0xC   race clock
    for name, base in REC_BASES.items():
        struct.pack_into('<I', img, base + 0x0C, MINIMA_PTR[name])
        struct.pack_into('<I', img, base + 0x18, CLKSRC)
        img[base + 0x11] = 0xFF                    # tier = -1
        img[base + 0x12] = 0xFF
        img[base + 0x13] = 4                       # 4 thresholds
    for i in range(8):
        img[0x3E8 + i] = 0xFF                      # near-miss slots free
    # the RUBBING record, wired exactly as FUN_00192EA0 leaves it
    # (0x0019338A..0x001933AD): minima 0x3F758C, three tiers, tier -1.
    struct.pack_into('<I', img, 0x570, P_RUB_MIN_T)
    struct.pack_into('<I', img, 0x57C, CLKSRC)
    img[0x575] = 0xFF
    img[0x576] = 0xFF
    img[0x577] = 3
    struct.pack_into('<i', img, 0x578, 3)
    struct.pack_into('<f', img, 0x5F0, -1.0)       # no shunt window
    # No PENDING TAKEDOWN CLAIM.  FUN_00192EA0 @0x00193308 seeds
    # score+0x4D8+i*4 (6 entries) with -1.0, and FUN_00197040 -- the resolver
    # the crash-reset block runs first (0x00193A80) -- treats `>= 0.0` as a
    # live claim and COMMITS the takedown.  Leaving these at 0 makes a blank
    # image look like six pending takedowns at t=0.
    for i in range(6):
        struct.pack_into('<f', img, 0x4D8 + i * 4, -1.0)
    # a workable boost bar: tier 0, 240-unit bar, x1 multiplier
    struct.pack_into('<i', img, 0xFC, 0)
    struct.pack_into('<f', img, 0x100, 240.0)
    struct.pack_into('<f', img, 0x104, 0.0)
    struct.pack_into('<f', img, 0x108, 0.0)
    struct.pack_into('<f', img, 0x10C, 36.0)
    struct.pack_into('<f', img, 0x110, 18.0)
    struct.pack_into('<f', img, 0x114, 1.0)
    struct.pack_into('<f', img, 0x118, 0.0)
    return img


def set_rec(img, name, value=0.0, tier=-1, scored=0, active=0):
    base = REC_BASES[name]
    struct.pack_into('<f', img, base + 0x00, value)
    img[base + 0x11] = tier & 0xFF
    img[ACTIVE_OFF[name]] = active
    if name in SCORED_OFF:
        img[SCORED_OFF[name]] = scored
    return img


# ==========================================================================
# Unicorn execution
# ==========================================================================
def build_trampoline(calls, step_addr):
    """MOV EDI,SCORE; MOV ESI,SCORE; {MOVSS XMM1,[step]; CALL f} x N; RET

    This reproduces FUN_001935F0's own call sequence: the air/oncoming/drift
    detectors take the score object in EDI (0x00193A2C..0x00193A48, each
    re-loading the same per-frame distance step into XMM1), while
    FUN_00194EE0 takes it in ESI -- the caller does `MOV ESI,EDI` at
    0x001939F4 before calling it.  Setting both covers either convention."""
    code = b""
    addr = CODE
    code += b"\xbf" + struct.pack('<I', SCORE)             # MOV EDI, SCORE
    addr += 5
    code += b"\xbe" + struct.pack('<I', SCORE)             # MOV ESI, SCORE
    addr += 5
    for f in calls:
        code += b"\xf3\x0f\x10\x0d" + struct.pack('<I', step_addr)  # MOVSS XMM1,[step]
        addr += 8
        rel = f - (addr + 5)
        code += b"\xe8" + struct.pack('<i', rel)           # CALL f
        addr += 5
    code += b"\xc3"
    return code


def run_real(score_img, calls, tune, step=0.0, racecar=None, extra=None,
             max_steps=400000):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, os.path.join(ROOT, ev.ELF))
    uc.mem_map(ev.STACK_BASE, ev.STACK_SIZE, UC_PROT_ALL)
    uc.mem_map(ev.SCRATCH, ev.SCRATCH_SZ, UC_PROT_ALL)
    uc.mem_map(ev.MAGIC_RET & ~(ev.PAGE - 1), ev.PAGE, UC_PROT_ALL)

    faults = []

    def on_unmapped(uc_, access, address, size, value, user):
        page = address & ~(ev.PAGE - 1)
        try:
            uc_.mem_map(page, ev.PAGE, UC_PROT_ALL)
            faults.append(page)
        except UcError:
            return False
        return True

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    mw = {}
    mw.update(fake_ctx())
    mw.update(param_writes(tune))
    # racecar defaults: not crashed, AI class (skips the air FX call), clock
    rc = {0x18FA: b"\x00", 0x18FB: b"\x00", 0x1920: struct.pack('<i', 1),
          0x2440: struct.pack('<I', PV), 0x50: struct.pack('<I', BOX_ME),
          0x19BC: b"\x00"}
    rc.update(racecar or {})
    for off, data in rc.items():
        mw[RACECAR + off] = data
    mw[CLKSRC + 0xC] = struct.pack('<f', struct.unpack_from('<f', score_img, 0xC)[0])
    step_addr = CODE + 0x800
    mw[step_addr] = f32(step)
    mw[CODE] = build_trampoline(calls, step_addr)
    mw[SCORE] = bytes(score_img)
    for a, d in (extra or {}).items():
        mw[a] = d
    for addr, data in mw.items():
        uc.mem_write(addr, data)

    sp = ev.STACK_BASE + ev.STACK_SIZE - 0x1000
    uc.mem_write(sp, struct.pack('<I', ev.MAGIC_RET))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EDI, SCORE)

    err = None
    try:
        uc.emu_start(CODE, ev.MAGIC_RET, count=max_steps)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
    return bytearray(uc.mem_read(SCORE, SCORE_SZ)), err


def run_slice(score_img, tune, crashed, respawn, racecar=None, extra=None,
              stop_at_branch=False, max_steps=2000000):
    """Execute the REAL FUN_001935F0 slice 0x001939AD..0x00193CE4.

    EDI/ESI are what the function itself holds there (score object / racecar),
    so the game's own gate decides which branch runs.  Returns
    (post score image, branch address reached, error)."""
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, os.path.join(ROOT, ev.ELF))
    uc.mem_map(ev.STACK_BASE, ev.STACK_SIZE, UC_PROT_ALL)
    uc.mem_map(ev.SCRATCH, ev.SCRATCH_SZ, UC_PROT_ALL)
    uc.mem_map(ev.MAGIC_RET & ~(ev.PAGE - 1), ev.PAGE, UC_PROT_ALL)

    def on_unmapped(uc_, access, address, size, value, user):
        page = address & ~(ev.PAGE - 1)
        try:
            uc_.mem_map(page, ev.PAGE, UC_PROT_ALL)
        except UcError:
            return False
        return True

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    verdict = []

    def on_code(uc_, addr, size, user):
        if addr in (GATE_RUN, GATE_SKIP) and not verdict:
            verdict.append(addr)
            if stop_at_branch:
                uc_.emu_stop()
        elif addr == GATE_JOIN:
            uc_.emu_stop()

    uc.hook_add(UC_HOOK_CODE, on_code)

    mw = {}
    mw.update(fake_ctx())
    mw.update(param_writes(tune))
    rc = {0x18FA: bytes([crashed]), 0x18FB: bytes([respawn]),
          0x1920: struct.pack('<i', 1), 0x179C: struct.pack('<i', 1),
          0x2440: struct.pack('<I', PV), 0x50: struct.pack('<I', BOX_ME),
          0x19BC: b"\x00"}
    rc.update(racecar or {})
    for off, data in rc.items():
        mw[RACECAR + off] = data
    mw[CLKSRC + 0xC] = struct.pack('<f', struct.unpack_from('<f', score_img, 0xC)[0])
    mw[SCORE] = bytes(score_img)
    for a, d in (extra or {}).items():
        mw[a] = d
    for addr, data in mw.items():
        uc.mem_write(addr, data)

    sp = ev.STACK_BASE + ev.STACK_SIZE - 0x1000
    uc.mem_write(sp, struct.pack('<I', ev.MAGIC_RET))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EDI, SCORE)
    uc.reg_write(UC_X86_REG_ESI, RACECAR)

    err = None
    try:
        uc.emu_start(GATE_ENTRY, ev.MAGIC_RET, count=max_steps)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
    return (bytearray(uc.mem_read(SCORE, SCORE_SZ)),
            verdict[0] if verdict else None, err)


def run_regparm3(score_img, func, eax, edx, ecx, tune, racecar=None,
                 extra=None, max_steps=400000):
    """MOV EAX,imm; MOV EDX,imm; MOV ECX,imm; CALL func; RET.

    FUN_00197920 and FUN_001979E0 are both __regparm3 (EAX, EDX, ECX) and
    are called exactly like this from the collision dispatcher -- see
    0x000273D0 (score in ECX, object in EDX) and 0x00027525 (score in EAX,
    other car in ECX)."""
    code = (b"\xb8" + struct.pack('<I', eax)
            + b"\xba" + struct.pack('<I', edx)
            + b"\xb9" + struct.pack('<I', ecx))
    rel = func - (CODE + len(code) + 5)
    code += b"\xe8" + struct.pack('<i', rel) + b"\xc3"

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, os.path.join(ROOT, ev.ELF))
    uc.mem_map(ev.STACK_BASE, ev.STACK_SIZE, UC_PROT_ALL)
    uc.mem_map(ev.SCRATCH, ev.SCRATCH_SZ, UC_PROT_ALL)
    uc.mem_map(ev.MAGIC_RET & ~(ev.PAGE - 1), ev.PAGE, UC_PROT_ALL)

    def on_unmapped(uc_, access, address, size, value, user):
        try:
            uc_.mem_map(address & ~(ev.PAGE - 1), ev.PAGE, UC_PROT_ALL)
        except UcError:
            return False
        return True

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    mw = {}
    mw.update(fake_ctx())
    mw.update(param_writes(tune))
    rc = {0x18FA: b"\x00", 0x18FB: b"\x00", 0x1920: struct.pack('<i', 1),
          0x19BC: b"\x00"}
    rc.update(racecar or {})
    for off, data in rc.items():
        mw[RACECAR + off] = data
    mw[CLKSRC + 0xC] = struct.pack(
        '<f', struct.unpack_from('<f', score_img, 0xC)[0])
    mw[CODE] = code
    mw[SCORE] = bytes(score_img)
    for a, d in (extra or {}).items():
        mw[a] = d
    for addr, data in mw.items():
        uc.mem_write(addr, data)

    sp = ev.STACK_BASE + ev.STACK_SIZE - 0x1000
    uc.mem_write(sp, struct.pack('<I', ev.MAGIC_RET))
    uc.reg_write(UC_X86_REG_ESP, sp)

    err = None
    try:
        uc.emu_start(CODE, ev.MAGIC_RET, count=max_steps)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
    return bytearray(uc.mem_read(SCORE, SCORE_SZ)), err


def run_rubbing(score_img, tune, extra=None, max_steps=400000):
    """Execute the REAL FUN_00194A80 with the score object in ESI."""
    code = (b"\xbe" + struct.pack('<I', SCORE))            # MOV ESI, SCORE
    rel = F_RUBBING - (CODE + len(code) + 5)
    code += b"\xe8" + struct.pack('<i', rel) + b"\xc3"

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, os.path.join(ROOT, ev.ELF))
    uc.mem_map(ev.STACK_BASE, ev.STACK_SIZE, UC_PROT_ALL)
    uc.mem_map(ev.SCRATCH, ev.SCRATCH_SZ, UC_PROT_ALL)
    uc.mem_map(ev.MAGIC_RET & ~(ev.PAGE - 1), ev.PAGE, UC_PROT_ALL)

    def on_unmapped(uc_, access, address, size, value, user):
        try:
            uc_.mem_map(address & ~(ev.PAGE - 1), ev.PAGE, UC_PROT_ALL)
        except UcError:
            return False
        return True

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)

    skipped = []

    def on_code(uc_, addr, size, user):
        # FUN_0019A050 is not ported: step over the call and drop its five
        # pushed arguments, so both sides end in the same state.
        if addr == RUB_PAYOUT_CALL:
            uc_.reg_write(UC_X86_REG_EIP, RUB_PAYOUT_NEXT)
            uc_.reg_write(UC_X86_REG_ESP, uc_.reg_read(UC_X86_REG_ESP) + 20)
            skipped.append(addr)

    uc.hook_add(UC_HOOK_CODE, on_code)

    mw = {}
    mw.update(fake_ctx())
    mw.update(param_writes(tune))
    for off, data in {0x18FA: b"\x00", 0x18FB: b"\x00",
                      0x1920: struct.pack('<i', 1),
                      0x19BC: b"\x00"}.items():
        mw[RACECAR + off] = data
    mw[CLKSRC + 0xC] = struct.pack(
        '<f', struct.unpack_from('<f', score_img, 0xC)[0])
    mw[CODE] = code
    mw[SCORE] = bytes(score_img)
    for a, d in (extra or {}).items():
        mw[a] = d
    for addr, data in mw.items():
        uc.mem_write(addr, data)

    sp = ev.STACK_BASE + ev.STACK_SIZE - 0x1000
    uc.mem_write(sp, struct.pack('<I', ev.MAGIC_RET))
    uc.reg_write(UC_X86_REG_ESP, sp)

    err = None
    try:
        uc.emu_start(CODE, ev.MAGIC_RET, count=max_steps)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
    return bytearray(uc.mem_read(SCORE, SCORE_SZ)), err


def run_port(score_img, args, tune):
    p = subprocess.run([DRIVER] + [args[0], tune] + [str(a) for a in args[1:]],
                       input=bytes(score_img).hex() + "\n",
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("driver failed: %s" % p.stderr)
    return bytearray.fromhex(p.stdout.strip().splitlines()[-1])


# ==========================================================================
# assertions
# ==========================================================================
FIELDS = (
    [("air.value", 0x358, 'f'), ("air.clock", 0x35C, 'f'),
     ("air.prev", 0x360, 'f'), ("air.active", 0x368, 'b'),
     ("air.tier", 0x369, 's'), ("air.prev_tier", 0x36A, 's'),
     ("onc.value", 0x374, 'f'), ("onc.clock", 0x378, 'f'),
     ("onc.prev", 0x37C, 'f'), ("onc.active", 0x384, 'b'),
     ("onc.tier", 0x385, 's'), ("onc.prev_tier", 0x386, 's'),
     ("drift.value", 0x390, 'f'), ("drift.clock", 0x394, 'f'),
     ("drift.prev", 0x398, 'f'), ("drift.active", 0x3A0, 'b'),
     ("drift.tier", 0x3A1, 's'), ("drift.prev_tier", 0x3A2, 's'),
     ("air.scored", 0x3C8, 'b'), ("onc.scored", 0x3C9, 'b'),
     ("drift.scored", 0x3CA, 'b'),
     ("stat.air_total", 0x50, 'f'), ("stat.air_best", 0x54, 'f'),
     ("stat.air_count", 0x354, 'i'),
     ("stat.onc_total", 0x58, 'f'), ("stat.onc_best", 0x5C, 'f'),
     ("stat.drift_total", 0x60, 'f'), ("stat.drift_best", 0x64, 'f'),
     ("bp.total", 0x4C, 'i'), ("bp.event", 0xB8, 'i'),
     ("boost.meter", 0x104, 'f'), ("boost.earned", 0x108, 'f'),
     ("callout.id", 0x254, 'i'), ("callout.tier", 0x260, 'i')]
)

NM_FIELDS = (
    [("nm.value", 0x418, 'f'), ("nm.prev", 0x420, 'f'),
     ("nm.active", 0x428, 'b'), ("nm.tier", 0x429, 's'),
     ("nm.prev_tier", 0x42A, 's'),
     ("nm.total", 0x3CC, 'i'), ("nm.chain", 0x3D0, 'i'),
     ("nm.prev_clock", 0x3DC, 'f'), ("nm.last", 0x3E0, 'f'),
     ("nm.chain_end", 0x3E4, 'f'),
     ("bp.total", 0x4C, 'i'), ("bp.event", 0xB8, 'i'),
     ("boost.meter", 0x104, 'f'), ("boost.earned", 0x108, 'f'),
     ("callout.id", 0x254, 'i'), ("callout.tier", 0x260, 'i')]
    + [("nm.id[%d]" % i, 0x3E8 + i, 's') for i in range(8)]
    + [("nm.seen[%d]" % i, 0x3F0 + i * 4, 'f') for i in range(8)]
    + [("nm.armed[%d]" % i, 0x410 + i, 'b') for i in range(8)]
)


RUB_FIELDS = (
    [("rub.value", 0x564, 'f'), ("rub.clock", 0x568, 'f'),
     ("rub.prev", 0x56C, 'f'), ("rub.active", 0x574, 'b'),
     ("rub.tier", 0x575, 's'), ("rub.prev_tier", 0x576, 's'),
     ("rub.target", 0x580, 'i'),
     ("boost.meter", 0x104, 'f'), ("boost.earned", 0x108, 'f')]
    + [("rub.last[%d]" % i, 0x510 + i * 4, 'f') for i in range(6)]
    + [("rub.time[%d]" % i, 0x528 + i * 4, 'f') for i in range(6)]
    + [("rub.start[%d]" % i, 0x540 + i * 4, 'f') for i in range(6)]
    + [("rub.prev_touch[%d]" % i, 0x558 + i, 'b') for i in range(6)]
    + [("rub.touch[%d]" % i, 0x55E + i, 'b') for i in range(6)]
)

CONTACT_FIELDS = (
    [("nm.id[%d]" % i, 0x3E8 + i, 's') for i in range(8)]
    + [("nm.seen[%d]" % i, 0x3F0 + i * 4, 'f') for i in range(8)]
    + [("nm.armed[%d]" % i, 0x410 + i, 'b') for i in range(8)]
)


def read_field(img, off, kind):
    if kind == 'f':
        return struct.unpack_from('<f', img, off)[0]
    if kind == 'i':
        return struct.unpack_from('<i', img, off)[0]
    if kind == 's':
        return struct.unpack_from('<b', img, off)[0]
    return img[off]


def compare(name, real, port, fields, results, tol=1e-4):
    bad = []
    for fname, off, kind in fields:
        a = read_field(real, off, kind)
        b = read_field(port, off, kind)
        if kind == 'f':
            ok = abs(a - b) <= tol * max(1.0, abs(a))
        else:
            ok = a == b
        if not ok:
            bad.append("%s: real=%r port=%r" % (fname, a, b))
    results.append((name, not bad, bad))


# ==========================================================================
# cases
# ==========================================================================
def case_cat(results):
    """AIR / ONCOMING / DRIFT: open, accumulate, pay, close."""
    cases = [
        # name, tune, flags(air,onc,drift), mph, step, clock, seed
        ("air open, below minimum", "default", (1, 0, 0), 100.0, 3.0, 1.0,
         dict(air=(0.0, -1, 0))),
        ("air first payment (whole accumulation)", "default", (1, 0, 0),
         100.0, 3.0, 1.0, dict(air=(4.0, -1, 0))),
        ("air later payment (step only)", "default", (1, 0, 0), 100.0, 3.0,
         1.0, dict(air=(20.0, 1, 1))),
        ("air close with tier -> BP + callout", "default", (0, 0, 0), 100.0,
         0.0, 2.0, dict(air=(64.0, 2, 1))),
        ("air close, never paid -> no BP", "default", (0, 0, 0), 100.0, 0.0,
         2.0, dict(air=(3.0, -1, 0))),
        ("oncoming speed gate blocks open", "default", (0, 1, 0), 20.0, 3.0,
         1.0, dict(onc=(0.0, -1, 0))),
        ("oncoming open + accumulate", "default", (0, 1, 0), 60.0, 3.0, 1.0,
         dict(onc=(8.0, -1, 0))),
        ("oncoming first payment", "default", (0, 1, 0), 60.0, 5.0, 1.0,
         dict(onc=(8.0, -1, 0))),
        ("oncoming tier climb", "default", (0, 1, 0), 60.0, 40.0, 1.0,
         dict(onc=(70.0, 0, 1))),
        ("oncoming close -> BP + callout id 0x73", "default", (0, 0, 0),
         60.0, 0.0, 3.0, dict(onc=(210.0, 2, 1))),
        ("oncoming close below speed gate", "default", (0, 1, 0), 10.0, 2.0,
         3.0, dict(onc=(120.0, 1, 1))),
        ("drift open + pay", "default", (0, 0, 1), 60.0, 6.0, 1.0,
         dict(drift=(8.0, -1, 0))),
        ("drift close -> BP + callout id 0x71", "default", (0, 0, 0), 60.0,
         0.0, 2.0, dict(drift=(200.0, 3, 1))),
        ("drift speed gate (vdb 90 mph)", "vdb", (0, 0, 1), 80.0, 4.0, 1.0,
         dict(drift=(30.0, 1, 1))),
        ("all three closing at once (callout priority)", "default", (0, 0, 0),
         100.0, 0.0, 4.0,
         dict(air=(64.0, 2, 1), onc=(210.0, 2, 1), drift=(200.0, 3, 1))),
        ("all three active at once", "default", (1, 1, 1), 120.0, 5.0, 1.0,
         dict(air=(20.0, 1, 1), onc=(60.0, 0, 1), drift=(50.0, 0, 1))),
        ("vdb tune: oncoming pays 0.15/m past 40 m", "vdb", (0, 1, 0), 60.0,
         10.0, 1.0, dict(onc=(45.0, -1, 1))),
        ("vdb tune: air close tier 3", "vdb", (0, 0, 0), 60.0, 0.0, 2.0,
         dict(air=(40.0, 3, 1))),
        # Boundary: the payment gate is `value > minimum`, STRICTLY greater --
        # landing exactly on the minimum must NOT pay.  2.0 + 3.0 == 5.0 m.
        ("air exactly at the minimum pays nothing", "default", (1, 0, 0),
         100.0, 3.0, 1.0, dict(air=(2.0, -1, 0))),
        ("oncoming exactly at the minimum pays nothing", "default", (0, 1, 0),
         60.0, 4.0, 1.0, dict(onc=(6.0, -1, 0))),
        ("drift exactly at the minimum pays nothing", "default", (0, 0, 1),
         60.0, 4.0, 1.0, dict(drift=(6.0, -1, 0))),
        # ... and one ulp past it does pay.
        ("air just past the minimum pays", "default", (1, 0, 0),
         100.0, 3.001, 1.0, dict(air=(2.0, -1, 0))),
    ]
    for name, tune, flags, mph, step, clock, seed in cases:
        img = blank_score(clock)
        for rec, (val, tier, scored) in seed.items():
            set_rec(img, rec, value=val, tier=tier, scored=scored)
        rc = {0x64: f32(mph / 2.2369363),
              0x10C0: bytes([flags[0]]),
              0x18FC: bytes([flags[1]]),
              0x10C2: bytes([flags[2]]),
              0x10DC: f32(clock)}
        real, err = run_real(img, [F_AIR, F_ONCOMING, F_DRIFT], tune,
                             step=step, racecar=rc)
        if err:
            results.append((name, False, ["emulation error: " + err]))
            continue
        port = run_port(img, ["cat", flags[0], flags[1], flags[2], mph, step,
                              clock], tune)
        compare(name, real, port, FIELDS, results)


def obb(m00, m02, m20, m22, px, py, pz, bmax=(1.0, 0.8, 2.4, 0.0),
        bmin=(-1.0, 0.0, -2.4, 0.0)):
    """A world matrix + box corners in the game's own layout."""
    m = [[m00, 0.0, m02, 0.0], [0.0, 1.0, 0.0, 0.0],
         [m20, 0.0, m22, 0.0], [px, py, pz, 1.0]]
    return m, list(bmax), list(bmin)


def yaw_obb(yaw, px, py, pz, **kw):
    import math
    c, s = math.cos(yaw), math.sin(yaw)
    return obb(c, -s, s, c, px, py, pz, **kw)


def obb_bytes(o):
    m, bmax, bmin = o
    vals = [v for row in m for v in row] + bmax + bmin
    return " ".join(repr(float(v)) for v in vals)


def seed_obj(mw, idx, o, owner=0):
    """Write one proximity object (table at 0x625FB0, stride 0x180)."""
    m, bmax, bmin = o
    base = OBJ_BASE + idx * OBJ_STRIDE
    box = BOX_OT + idx * 0x100
    for r in range(4):
        for c in range(4):
            mw[base + 0x70 + (r * 4 + c) * 4] = f32(m[r][c])
    mw[base + 0x10C] = struct.pack('<I', owner)
    mw[base + 0xB0] = struct.pack('<I', box)
    mw[box + 0xE80] = f32(bmax[0])
    mw[box + 0xE88] = f32(bmax[2])
    mw[box + 0xE90] = f32(bmin[0])
    mw[box + 0xE98] = f32(bmin[2])
    return mw


def case_obb(results):
    """FUN_00195DD0 -- the near-miss proximity test, exercised directly.

    Driven through FUN_00194EE0 with a single candidate and a speed below the
    arm gate would hide the result, so instead each geometry is run through
    the full detector at arm speed and the slot claim is the observable."""
    import math
    me = obb(1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0)
    cases = [
        ("side by side, 1.0 m gap", me, obb(1.0, 0.0, 0.0, 1.0, 3.0, 0.0, 0.0), 1),
        ("side by side, 3.0 m gap", me, obb(1.0, 0.0, 0.0, 1.0, 5.0, 0.0, 0.0), 0),
        ("nose to tail, 0.6 m gap", me, obb(1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 5.4), 1),
        ("nose to tail, 5 m gap", me, obb(1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 9.8), 0),
        ("overlapping boxes", me, obb(1.0, 0.0, 0.0, 1.0, 0.5, 0.0, 0.5), 1),
        ("height gate: +5 m above", me, obb(1.0, 0.0, 0.0, 1.0, 2.5, 5.0, 0.0), 0),
        ("height gate: +3 m above still counts", me,
         obb(1.0, 0.0, 0.0, 1.0, 2.5, 3.0, 0.0), 1),
        ("perpendicular, corner gap", me, yaw_obb(math.pi / 2, 3.0, 0.0, 2.6), 1),
        ("perpendicular, clear", me, yaw_obb(math.pi / 2, 6.0, 0.0, 6.0), 0),
        ("45 deg, near", me, yaw_obb(math.pi / 4, 2.6, 0.0, 2.6), 1),
        ("oncoming lane, 1.2 m gap", me, yaw_obb(math.pi, 3.2, 0.0, 1.0), 1),
        ("far away", me, obb(1.0, 0.0, 0.0, 1.0, 40.0, 0.0, 40.0), 0),
    ]
    for name, a, b, expect in cases:
        # real: run FUN_00194EE0 with exactly this one candidate at arm speed
        clock = 1.0
        img = blank_score(clock)
        mw = {}
        seed_obj(mw, 3, b)
        mw[P_COUNTS + 0] = bytes([1])
        mw[P_LIST + 0] = bytes([3])
        m, bmax, bmin = a
        rc = {0x64: f32(100.0 / 2.2369363), 0x19BC: b"\x00", 0x10DC: f32(clock)}
        for r in range(4):
            for c in range(4):
                rc[0x10 + (r * 4 + c) * 4] = f32(m[r][c])
        mw[BOX_ME + 0xE80] = f32(bmax[0])
        mw[BOX_ME + 0xE88] = f32(bmax[2])
        mw[BOX_ME + 0xE90] = f32(bmin[0])
        mw[BOX_ME + 0xE98] = f32(bmin[2])
        real, err = run_real(img, [F_NEARMISS], "default", racecar=rc, extra=mw)
        if err:
            results.append((name, False, ["emulation error: " + err]))
            continue
        # port: feed the same geometry to the ported detector
        geom = obb_bytes(a) + "\n" + obb_bytes(b) + "\n3\n"
        p = subprocess.run([DRIVER, "nm", "default", "100.0", str(clock), "1"],
                           input=bytes(img).hex() + "\n" + geom,
                           capture_output=True, text=True)
        if p.returncode != 0:
            results.append((name, False, ["driver failed: " + p.stderr]))
            continue
        port = bytearray.fromhex(p.stdout.strip().splitlines()[-1])
        compare(name, real, port, NM_FIELDS, results)
        # Semantic guard: assert the REAL function actually reached the
        # expected verdict, so a case cannot pass by both sides doing nothing.
        claimed = 1 if struct.unpack_from('<b', real, 0x3E8)[0] != -1 else 0
        results.append((name + " [real verdict]", claimed == expect,
                        [] if claimed == expect else
                        ["real claimed=%d expected=%d" % (claimed, expect)]))


def case_near_miss(results):
    """Multi-frame near-miss sequences: arm -> pass -> award -> chain."""
    import math

    def frame(img, tune, mph, clock, cands):
        """One frame on both sides; returns (real, port, err)."""
        mw = {}
        for j, (idx, o) in enumerate(cands):
            seed_obj(mw, idx, o)
            mw[P_LIST + j] = bytes([idx])
        mw[P_COUNTS + 0] = bytes([len(cands)])
        struct.pack_into('<f', img, 0xC, clock)
        me = obb(1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0)
        m, bmax, bmin = me
        rc = {0x64: f32(mph / 2.2369363), 0x19BC: b"\x00", 0x10DC: f32(clock)}
        for r in range(4):
            for c in range(4):
                rc[0x10 + (r * 4 + c) * 4] = f32(m[r][c])
        mw[BOX_ME + 0xE80] = f32(bmax[0])
        mw[BOX_ME + 0xE88] = f32(bmax[2])
        mw[BOX_ME + 0xE90] = f32(bmin[0])
        mw[BOX_ME + 0xE98] = f32(bmin[2])
        real, err = run_real(img, [F_NEARMISS], tune, racecar=rc, extra=mw)
        geom = obb_bytes(me) + "\n"
        for idx, o in cands:
            geom += obb_bytes(o) + "\n%d\n" % idx
        p = subprocess.run([DRIVER, "nm", tune, str(mph), str(clock),
                            str(len(cands))],
                           input=bytes(img).hex() + "\n" + geom,
                           capture_output=True, text=True)
        if p.returncode != 0:
            raise RuntimeError(p.stderr)
        port = bytearray.fromhex(p.stdout.strip().splitlines()[-1])
        return real, port, err

    NEAR = obb(1.0, 0.0, 0.0, 1.0, 3.0, 0.0, 0.0)     # 1.0 m gap alongside
    FAR = obb(1.0, 0.0, 0.0, 1.0, 40.0, 0.0, 40.0)

    seqs = [
        ("single pass: arm then award", "default",
         [(100.0, 1.0, [(3, NEAR)]),
          (100.0, 1.05, [(3, NEAR)]),
          (100.0, 1.10, [(3, FAR)]),
          (100.0, 1.15, [(3, FAR)])]),
        ("two-car chain -> tier 1", "default",
         [(100.0, 1.0, [(3, NEAR)]),
          (100.0, 1.05, [(3, FAR)]),
          (100.0, 1.10, [(4, NEAR)]),
          (100.0, 1.15, [(4, FAR)]),
          (100.0, 1.20, [(4, FAR)])]),
        ("below arm speed: nothing happens", "default",
         [(20.0, 1.0, [(3, NEAR)]),
          (20.0, 1.05, [(3, FAR)]),
          (20.0, 1.10, [(3, FAR)])]),
        ("chain expiry by time -> category BP", "default",
         [(100.0, 1.0, [(3, NEAR)]),
          (100.0, 1.05, [(3, FAR)]),
          (100.0, 1.10, [(4, NEAR)]),
          (100.0, 1.15, [(4, FAR)]),
          (100.0, 7.5, [(4, FAR)]),
          (100.0, 7.6, [(4, FAR)])]),
        ("chain expiry by dropping below chain speed", "default",
         [(100.0, 1.0, [(3, NEAR)]),
          (100.0, 1.05, [(3, FAR)]),
          (100.0, 1.10, [(4, NEAR)]),
          (100.0, 1.15, [(4, FAR)]),
          (10.0, 1.20, [(4, FAR)])]),
        ("slot released 1 s after last proximity", "default",
         [(100.0, 1.0, [(3, NEAR)]),
          (100.0, 1.05, [(3, FAR)]),
          (100.0, 2.5, [(3, FAR)]),
          (100.0, 2.6, [(3, FAR)])]),
        ("same car stays in proximity: no repeat award", "default",
         [(100.0, 1.0, [(3, NEAR)]),
          (100.0, 1.05, [(3, NEAR)]),
          (100.0, 1.10, [(3, NEAR)]),
          (100.0, 1.15, [(3, NEAR)]),
          (100.0, 1.20, [(3, FAR)]),
          (100.0, 1.25, [(3, FAR)])]),
        ("vdb tune: 1.8 m gap, 60 mph gate, 15 units", "vdb",
         [(100.0, 1.0, [(3, NEAR)]),
          (100.0, 1.05, [(3, FAR)]),
          (100.0, 1.10, [(3, FAR)])]),
        ("vdb tune: below 60 mph does not arm", "vdb",
         [(50.0, 1.0, [(3, NEAR)]),
          (50.0, 1.05, [(3, FAR)]),
          (50.0, 1.10, [(3, FAR)])]),
        ("three cars in one chain -> tier climb", "default",
         [(100.0, 1.0, [(3, NEAR)]),
          (100.0, 1.05, [(3, FAR)]),
          (100.0, 1.10, [(4, NEAR)]),
          (100.0, 1.15, [(4, FAR)]),
          (100.0, 1.20, [(5, NEAR)]),
          (100.0, 1.25, [(5, FAR)]),
          (100.0, 1.30, [(5, FAR)])]),
    ]

    for name, tune, steps in seqs:
        img = blank_score(0.0)
        ok = True
        for i, (mph, clock, cands) in enumerate(steps):
            try:
                real, port, err = frame(img, tune, mph, clock, cands)
            except RuntimeError as e:
                results.append(("%s [frame %d]" % (name, i), False, [str(e)]))
                ok = False
                break
            if err:
                results.append(("%s [frame %d]" % (name, i), False,
                                ["emulation error: " + err]))
                ok = False
                break
            compare("%s [frame %d]" % (name, i), real, port, NM_FIELDS,
                    results)
            # carry the REAL post-state forward so a divergence cannot hide
            img = bytearray(real)
        if not ok:
            continue


# ==========================================================================
# THE CRASH GATE -- FUN_001935F0 @0x001939AD..0x00193CE4
# ==========================================================================
EXTRA_RECS = (0x564, 0x598, 0x5C4)      # rubbing / tailgate / grinding


def wire_extra_records(img):
    """The crash reset block also resets three records this module does not
    own; give them a valid clock source so the real block can run."""
    for base in EXTRA_RECS:
        struct.pack_into('<I', img, base + 0x0C, P_AIR_MIN_T)
        struct.pack_into('<I', img, base + 0x18, CLKSRC)
        img[base + 0x11] = 0xFF
        img[base + 0x12] = 0xFF
        img[base + 0x13] = 4
    struct.pack_into('<i', img, 0x5B4, -1)   # no tracked opponent to release
    return img


def live_score(clock, step=1.0):
    """A score object mid-race: an oncoming run in progress, two near-miss
    slots tracked with a live 2-link chain, one of them ARMED and already
    behind the previous-frame clock -- i.e. a slot that WOULD pay this frame.
    This is the state the reported bug fires from: crash into the traffic you
    were passing and the wreck separates from its box."""
    img = blank_score(clock)
    wire_extra_records(img)
    set_rec(img, 'onc', value=420.0, tier=2, scored=1, active=1)
    set_rec(img, 'nm', value=2.0, tier=1, active=1)
    img[0x3E8] = 3
    img[0x3E9] = 7
    struct.pack_into('<f', img, 0x3F0, clock - 0.10)   # < prev_clock: pays
    struct.pack_into('<f', img, 0x3F4, clock - 0.05)   # < prev_clock: pays
    img[0x410] = 1
    img[0x411] = 1
    struct.pack_into('<i', img, 0x3D0, 2)              # chain length
    struct.pack_into('<i', img, 0x3CC, 5)              # lifetime near misses
    struct.pack_into('<f', img, 0x3DC, clock - 0.02)   # previous frame clock
    struct.pack_into('<f', img, 0x3E0, clock - 0.05)
    # the packed position pair FUN_001935F0 differences into the step
    struct.pack_into('<4f', img, 0x2A0, 0.0, 0.0, 0.0, 0.0)
    struct.pack_into('<4f', img, 0x2B0, step, 0.0, 0.0, 0.0)
    return img


def slice_racecar(mph, clock, air=0, onc=0, drift=0, me=None):
    rc = {0x64: f32(mph / 2.2369363), 0x10DC: f32(clock), 0x19BC: b"\x00",
          0x10C0: bytes([air]), 0x18FC: bytes([onc]), 0x10C2: bytes([drift])}
    if me is not None:
        m, _, _ = me
        for r in range(4):
            for c in range(4):
                rc[0x10 + (r * 4 + c) * 4] = f32(m[r][c])
    return rc


def me_box_writes(me):
    m, bmax, bmin = me
    return {BOX_ME + 0xE80: f32(bmax[0]), BOX_ME + 0xE88: f32(bmax[2]),
            BOX_ME + 0xE90: f32(bmin[0]), BOX_ME + 0xE98: f32(bmin[2])}


def port_gate(img, tune, crashed, respawn, clock):
    p = subprocess.run([DRIVER, "gate", tune, str(crashed), str(respawn),
                        str(clock)],
                       input=bytes(img).hex() + "\n",
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("driver failed: %s" % p.stderr)
    lines = p.stdout.strip().splitlines()
    susp = int(lines[0].split("=")[1])
    return bytearray.fromhex(lines[-1]), susp


def port_frame(img, tune, flags, mph, step, clock, crashed, respawn,
               me, cands):
    geom = obb_bytes(me) + "\n"
    for idx, o in cands:
        geom += obb_bytes(o) + "\n%d\n" % idx
    p = subprocess.run([DRIVER, "frame", tune, str(flags[0]), str(flags[1]),
                        str(flags[2]), str(mph), str(step), str(clock),
                        str(crashed), str(respawn), str(len(cands))],
                       input=bytes(img).hex() + "\n" + geom,
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError("driver failed: %s" % p.stderr)
    return bytearray.fromhex(p.stdout.strip().splitlines()[-1])


def case_crash_gate(results):
    """Which branch does the REAL FUN_001935F0 take, and does the port agree?

    The gate is two byte tests: racecar+0x18FA (crashed) at 0x001939AD and
    racecar+0x18FB (being re-placed on the track) at 0x001939D1.  Either one
    jumps over FUN_00194A80/FUN_00194EE0/FUN_00196940/BE0/E10 entirely."""
    for crashed, respawn, expect in ((0, 0, 0), (1, 0, 1), (0, 1, 1),
                                     (1, 1, 1)):
        name = "gate: crashed=%d respawning=%d" % (crashed, respawn)
        img = live_score(2.0)
        real, verdict, err = run_slice(img, "default", crashed, respawn,
                                       racecar=slice_racecar(100.0, 2.0),
                                       stop_at_branch=True)
        if err:
            results.append((name, False, ["emulation error: " + err]))
            continue
        real_skip = 1 if verdict == GATE_SKIP else 0
        _, susp = port_gate(img, "default", crashed, respawn, 2.0)
        ok = (real_skip == expect) and (susp == expect)
        results.append((name, ok, [] if ok else
                        ["real=%s port_suspended=%d expected skip=%d" %
                         ({GATE_RUN: "RUN", GATE_SKIP: "SKIP"}.get(verdict,
                                                                   verdict),
                          susp, expect)]))


def case_crash_reset(results):
    """The reset block at 0x00193A80, executed for real, field by field."""
    cases = []

    # 1. the bug's own state: an armed slot that would pay this very frame
    cases.append(("crash reset: armed slot due to pay is destroyed", 1, 0,
                  live_score(2.0)))
    # 2. respawn flag alone takes the same branch
    cases.append(("crash reset: via respawning flag (+0x18FB)", 0, 1,
                  live_score(2.0)))

    # 3. all four records live with tiers, all eight slots occupied
    img = live_score(3.0)
    set_rec(img, 'air', value=64.0, tier=2, scored=1, active=1)
    set_rec(img, 'drift', value=200.0, tier=3, scored=1, active=1)
    for i in range(8):
        img[0x3E8 + i] = 10 + i
        struct.pack_into('<f', img, 0x3F0 + i * 4, 2.9 + 0.001 * i)
        img[0x410 + i] = 1
    struct.pack_into('<i', img, 0x3D0, 7)
    cases.append(("crash reset: 8 slots + air/onc/drift all live", 1, 0, img))

    # 4. nothing in flight -- the reset must still be a faithful no-op
    img = wire_extra_records(blank_score(1.0))
    cases.append(("crash reset: idle score object", 1, 0, img))

    # 5. mid-event with no tier yet reached (tier -1 must stay -1)
    img = live_score(4.0)
    set_rec(img, 'onc', value=12.0, tier=-1, scored=0, active=1)
    set_rec(img, 'nm', value=0.0, tier=-1, active=0)
    struct.pack_into('<i', img, 0x3D0, 0)
    cases.append(("crash reset: untiered event in progress", 1, 0, img))

    # 6. a crash while a chain is one frame from expiring by time
    img = live_score(9.0)
    struct.pack_into('<f', img, 0x3E0, 1.0)     # last link long ago
    cases.append(("crash reset: chain about to expire pays nothing", 1, 0,
                  img))

    # 7. a live RUB in progress: the block also wipes the per-opponent
    #    contact arrays and the rubbing record (0x00193AE5, 0x00193BF0).
    img = live_score(5.0)
    for i in range(6):
        struct.pack_into('<f', img, 0x510 + i * 4, 4.9)
        struct.pack_into('<f', img, 0x528 + i * 4, 0.4 + 0.1 * i)
        struct.pack_into('<f', img, 0x540 + i * 4, 4.2)
        img[0x558 + i] = 1
        img[0x55E + i] = 1
    struct.pack_into('<f', img, 0x564, 3.0)
    img[0x574] = 1
    img[0x575] = 1
    cases.append(("crash reset: a live RUB is wiped too", 1, 0, img))

    for name, crashed, respawn, img in cases:
        clock = struct.unpack_from('<f', img, 0xC)[0]
        real, verdict, err = run_slice(img, "default", crashed, respawn,
                                       racecar=slice_racecar(100.0, clock),
                                       extra=rub_extra(6, 1.0 / 60.0, clock))
        if err:
            results.append((name, False, ["emulation error: " + err]))
            continue
        if verdict != GATE_SKIP:
            results.append((name, False, ["real took the RUN branch"]))
            continue
        port, susp = port_gate(img, "default", crashed, respawn, clock)
        compare(name, real, port, FIELDS + NM_FIELDS + RUB_FIELDS, results)


def case_crash_vs_run(results):
    """The reported bug, both ways, on ONE seed and ONE piece of geometry.

    The car is alongside a traffic box (armed slot), then the box is gone:
    not crashed the game pays the near miss and closes the oncoming run;
    crashed it pays nothing and wipes both.  Real slice vs port, whole image.
    """
    NEAR = obb(1.0, 0.0, 0.0, 1.0, 3.0, 0.0, 0.0)
    FAR = obb(1.0, 0.0, 0.0, 1.0, 40.0, 0.0, 40.0)
    ME = obb(1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0)

    cases = [
        # name, crashed, respawn, oncoming flag, candidate, expect BP change
        ("wreck separates from traffic, NOT crashed -> pays", 0, 0, 0, FAR),
        ("wreck separates from traffic, CRASHED -> pays nothing", 1, 0, 0,
         FAR),
        ("still alongside traffic, NOT crashed", 0, 0, 0, NEAR),
        ("still alongside traffic, CRASHED", 1, 0, 0, NEAR),
        ("oncoming run + near miss, NOT crashed", 0, 0, 1, FAR),
        ("oncoming run + near miss, CRASHED", 1, 0, 1, FAR),
        ("oncoming run + near miss, RESPAWNING", 0, 1, 1, FAR),
    ]
    for name, crashed, respawn, onc, cand in cases:
        clock, mph, step = 2.0, 100.0, 1.0
        img = live_score(clock, step=step)
        mw = {}
        seed_obj(mw, 3, cand)
        mw[P_COUNTS + 0] = bytes([1])
        mw[P_LIST + 0] = bytes([3])
        mw.update(me_box_writes(ME))
        real, verdict, err = run_slice(
            img, "default", crashed, respawn,
            racecar=slice_racecar(mph, clock, onc=onc, me=ME), extra=mw)
        if err:
            results.append((name, False, ["emulation error: " + err]))
            continue
        port = port_frame(img, "default", (0, onc, 0), mph, step, clock,
                          crashed, respawn, ME, [(3, cand)])
        compare(name, real, port, FIELDS + NM_FIELDS, results)

        # Semantic guard: the crashed runs must pay NOTHING, the others must
        # actually have done something -- so neither side can pass by inertia.
        paid = (struct.unpack_from('<i', real, 0x4C)[0] != 0
                or struct.unpack_from('<f', real, 0x104)[0] != 0.0
                or struct.unpack_from('<i', real, 0x3CC)[0] != 5)
        suspended = crashed or respawn
        want = (not suspended)
        results.append((name + " [real paid?]", paid == want,
                        [] if paid == want else
                        ["real paid=%s expected=%s" % (paid, want)]))


# --------------------------------------------------------------------------
# FUN_00197920 -- CONTACT: the near-miss cancel
# --------------------------------------------------------------------------
OBJ = ev.SCRATCH + 0x40000        # the collided object (obj+0x10C, obj+0x177)


def seed_slots(img, slots):
    """slots: list of (index, id, seen, armed)."""
    for i, sid, seen, armed in slots:
        img[0x3E8 + i] = sid & 0xFF
        struct.pack_into('<f', img, 0x3F0 + i * 4, seen)
        img[0x410 + i] = armed


def case_contact(results):
    clock = 12.0
    cases = [
        ("contact DISARMS the slot that vehicle holds", 7, 0, 0,
         [(0, 3, 11.0, 1), (2, 7, 11.9, 1), (5, 9, 11.5, 1)]),
        ("contact on an un-armed tracked slot is a no-op", 7, 0, 0,
         [(2, 7, 11.9, 0)]),
        ("contact CLAIMS a free slot, un-armed", 7, 0, 0,
         [(0, 3, 11.0, 1), (1, 9, 11.5, 1)]),
        ("contact with the LAST free slot (index 7)", 7, 0, 0,
         [(i, 10 + i, 11.0, 1) for i in range(7)]),
        ("contact with every slot taken changes nothing", 7, 0, 0,
         [(i, 10 + i, 11.0, 1) for i in range(8)]),
        ("contact while CRASHED changes nothing", 7, 1, 0,
         [(2, 7, 11.9, 1)]),
        ("contact after the race is over changes nothing", 7, 0, 3,
         [(2, 7, 11.9, 1)]),
        ("contact with id 0 (slot ids are signed bytes)", 0, 0, 0,
         [(0, 0, 11.9, 1)]),
    ]
    for name, cid, crashed, state, slots in cases:
        img = blank_score(clock)
        seed_slots(img, slots)
        struct.pack_into('<i', img, 0x27C, state)
        extra = {OBJ + 0x10C: struct.pack('<I', 0), OBJ + 0x177: bytes([cid])}
        real, err = run_regparm3(img, F_CONTACT, 0, OBJ, SCORE, "default",
                                 racecar={0x18FA: bytes([crashed])},
                                 extra=extra)
        if err:
            results.append((name, False, ["emulation error: " + err]))
            continue
        port = run_port(img, ["contact", cid, clock, crashed], "default")
        compare(name, real, port, CONTACT_FIELDS, results)


# --------------------------------------------------------------------------
# FUN_001979E0 -- CONTACT: the per-opponent rubbing arrays
# --------------------------------------------------------------------------
CAROBJ = ev.SCRATCH + 0x41000     # the other car (+0x19BC = its racer slot)


def case_mark_contact(results):
    clock = 7.25
    for name, idx, crashed, state in [
            ("mark contact, slot 0", 0, 0, 0),
            ("mark contact, slot 3", 3, 0, 0),
            ("mark contact, slot 5 (last)", 5, 0, 0),
            ("mark contact while CRASHED is a no-op", 2, 1, 0),
            ("mark contact after the race is a no-op", 2, 0, 3)]:
        img = blank_score(clock)
        for i in range(6):
            struct.pack_into('<f', img, 0x510 + i * 4, 1.0 + i)
        struct.pack_into('<i', img, 0x27C, state)
        extra = {CAROBJ + 0x19BC: bytes([idx])}
        real, err = run_regparm3(img, F_MARK, SCORE, 0, CAROBJ, "default",
                                 racecar={0x18FA: bytes([crashed])},
                                 extra=extra)
        if err:
            results.append((name, False, ["emulation error: " + err]))
            continue
        port = run_port(img, ["mark", idx, clock, crashed], "default")
        compare(name, real, port, RUB_FIELDS, results)


# --------------------------------------------------------------------------
# FUN_00194A80 -- RUBBING
# --------------------------------------------------------------------------
OPP = ev.SCRATCH + 0x42000        # opponent racecars, stride 0x2000


def rub_extra(n, dt, clock, aggressor=None):
    """Globals + opponent racecars for FUN_00194A80.

    aggressor: {slot: attacker_pointer} seeds racecar+0x16BC/+0x16C0 so the
    shunt-suppression branch can be exercised."""
    mw = {D_CARCOUNT: struct.pack('<i', n),
          D_DT_RUB: f32(dt), D_DT_ACC: f32(dt)}
    for i in range(n):
        car = OPP + i * 0x2000
        mw[D_CARLIST + i * 4] = struct.pack('<I', car)
        mw[car + 0x10DC] = f32(clock)
        mw[car + 0x1920] = struct.pack('<i', 1)
        mw[car + 0x1198] = struct.pack('<I', car)
        mw[car + 0x1598] = f32(-1.0)
        mw[car + 0x1690] = f32(-1.0)
        mw[car + 0x16BC] = struct.pack('<I', 0)
        mw[car + 0x16C0] = f32(-1.0)
    for slot, who in (aggressor or {}).items():
        car = OPP + slot * 0x2000
        mw[car + 0x16BC] = struct.pack('<I', who)
        mw[car + 0x16C0] = f32(clock)
        mw[car + 0x1598] = f32(clock)
    return mw


def case_rubbing(results):
    dt = 1.0 / 60.0
    clock = 20.0
    # name, touch[], time[], last[], slam[], crashed
    cases = [
        ("no contact, no timer: nothing happens",
         [0] * 6, [0.0] * 6, [0.0] * 6, [0] * 6),
        ("first contact frame starts the timer and stamps the start",
         [1, 0, 0, 0, 0, 0], [0.0] * 6, [clock] * 6, [0] * 6),
        ("contact keeps growing the timer, still under 0.3 s",
         [1, 0, 0, 0, 0, 0], [0.2, 0, 0, 0, 0, 0], [clock] * 6, [0] * 6),
        ("timer passes Min Contact Time -> the event opens and pays",
         [1, 0, 0, 0, 0, 0], [0.29, 0, 0, 0, 0, 0], [clock] * 6, [0] * 6),
        ("two opponents in contact: the LAST one owns score+0x580",
         [1, 0, 1, 0, 0, 0], [0.5, 0, 0.5, 0, 0, 0], [clock] * 6, [0] * 6),
        ("contact lost, inside the no-slam grace: timer survives",
         [0] * 6, [0.5, 0, 0, 0, 0, 0], [clock - 0.2] + [0.0] * 5, [0] * 6),
        ("contact lost, past the grace: timer zeroed",
         [0] * 6, [0.5, 0, 0, 0, 0, 0], [clock - 0.9] + [0.0] * 5, [0] * 6),
        ("a SLAM zeroes the timer instead of growing it",
         [1, 0, 0, 0, 0, 0], [0.5, 0, 0, 0, 0, 0], [clock] * 6,
         [1, 0, 0, 0, 0, 0]),
        ("slot 5 (the last racer) rubs",
         [0, 0, 0, 0, 0, 1], [0.5] * 6, [clock] * 6, [0] * 6),
    ]
    for name, touch, tval, last, slam in cases:
        img = blank_score(clock)
        for i in range(6):
            img[0x55E + i] = touch[i]
            struct.pack_into('<f', img, 0x528 + i * 4, tval[i])
            struct.pack_into('<f', img, 0x510 + i * 4, last[i])
        # a live event where a timer is already past the threshold
        if any(t >= 0.3 for t in tval):
            img[0x574] = 1
            struct.pack_into('<f', img, 0x564, 1.0)
            img[0x575] = 0
        agg = {}
        for i, sl in enumerate(slam):
            if sl:
                agg[i] = RACECAR
        real, err = run_rubbing(img, "default",
                                extra=rub_extra(6, dt, clock, agg))
        if err:
            results.append((name, False, ["emulation error: " + err]))
            continue
        port = run_port(img, ["rub", clock, dt, 6] + list(slam), "default")
        compare(name, real, port, RUB_FIELDS, results)

    # the end-of-frame flag rotate (FUN_001935F0 @0x001940A1) -- there is no
    # standalone function for it, so this is a port-side invariant check.
    img = blank_score(clock)
    for i in range(6):
        img[0x55E + i] = i % 2
        img[0x558 + i] = 1
    port = run_port(img, ["frameend"], "default")
    ok = all(port[0x558 + i] == (i % 2) and port[0x55E + i] == 0
             for i in range(6))
    results.append(("end-of-frame flag rotate (0x001940A1)", ok,
                    [] if ok else ["prev=%s cur=%s" %
                                   (list(port[0x558:0x564]),
                                    list(port[0x55E:0x564]))]))


def case_takedown_vs_nearmiss(results):
    """THE REPORTED BUG, end to end and 1:1 against the real x86.

    Same geometry three ways: pass a car cleanly and the near miss pays; TOUCH
    it (a takedown / a crash into it) and FUN_00197920 disarms its slot, so
    the same separation pays nothing.  Both sides run FUN_00197920 and then
    FUN_00194EE0 in the game's own order (collision step, then score step).
    """
    NEAR = obb(1.0, 0.0, 0.0, 1.0, 3.0, 0.0, 0.0)    # 1.0 m clearance
    FAR = obb(1.0, 0.0, 0.0, 1.0, 40.0, 0.0, 40.0)
    ME = obb(1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0)
    CAND = 3

    def one_frame(real_img, port_img, mph, clock, cand, touch):
        if touch is not None:
            struct.pack_into('<f', real_img, 0xC, clock)
            struct.pack_into('<f', port_img, 0xC, clock)
            extra = {OBJ + 0x10C: struct.pack('<I', 0),
                     OBJ + 0x177: bytes([touch])}
            real_img, err = run_regparm3(real_img, F_CONTACT, 0, OBJ, SCORE,
                                         "default", extra=extra)
            if err:
                return None, None, err
            port_img = run_port(port_img, ["contact", touch, clock, 0],
                                "default")
        struct.pack_into('<f', real_img, 0xC, clock)
        struct.pack_into('<f', port_img, 0xC, clock)
        mw = {}
        seed_obj(mw, CAND, cand)
        mw[P_LIST + 0] = bytes([CAND])
        mw[P_COUNTS + 0] = bytes([1])
        mw.update(me_box_writes(ME))
        rc = slice_racecar(mph, clock, me=ME)
        real_img, err = run_real(real_img, [F_NEARMISS], "default",
                                 racecar=rc, extra=mw)
        if err:
            return None, None, err
        geom = obb_bytes(ME) + "\n" + obb_bytes(cand) + "\n%d\n" % CAND
        p = subprocess.run([DRIVER, "nm", "default", str(mph), str(clock), "1"],
                           input=bytes(port_img).hex() + "\n" + geom,
                           capture_output=True, text=True)
        if p.returncode != 0:
            raise RuntimeError(p.stderr)
        port_img = bytearray.fromhex(p.stdout.strip().splitlines()[-1])
        return real_img, port_img, None

    seqs = [
        ("clean pass pays the near miss",
         [(1.0, NEAR, None), (1.1, NEAR, None), (1.2, FAR, None),
          (1.3, FAR, None)], True),
        ("TOUCH mid-pass -> takedown, NOT a near miss",
         [(1.0, NEAR, None), (1.1, NEAR, CAND), (1.2, FAR, None),
          (1.3, FAR, None)], False),
        ("touched before ever being alongside -> no near miss",
         [(1.0, FAR, CAND), (1.1, NEAR, None), (1.2, NEAR, None),
          (1.3, FAR, None)], False),
        ("touch a DIFFERENT car -> the near miss still pays",
         [(1.0, NEAR, None), (1.1, NEAR, 9), (1.2, FAR, None),
          (1.3, FAR, None)], True),
    ]
    for name, frames, expect_pay in seqs:
        real_img = blank_score(0.9)
        port_img = bytearray(real_img)
        err = None
        for k, (clock, cand, touch) in enumerate(frames):
            real_img, port_img, err = one_frame(real_img, port_img, 100.0,
                                                clock, cand, touch)
            if err:
                results.append(("%s [frame %d]" % (name, k), False,
                                ["emulation error: " + err]))
                break
            compare("%s [frame %d]" % (name, k), real_img, port_img,
                    NM_FIELDS, results)
        if err:
            continue
        # Semantic guard: the REAL image must actually differ between the two
        # outcomes, so neither side can pass the diff by doing nothing.
        paid = struct.unpack_from('<i', real_img, 0x3CC)[0] != 0
        results.append(("%s [real paid?]" % name, paid == expect_pay,
                        [] if paid == expect_pay else
                        ["real near-miss count %d, expected paid=%s"
                         % (struct.unpack_from('<i', real_img, 0x3CC)[0],
                            expect_pay)]))


def _driver_stale():
    """Rebuild when a source is newer than the binary.  The old test was
    `not os.path.exists(DRIVER)`, which silently validated a stale driver
    for as long as the binary survived -- a new dump_score_events.c command
    then failed with "usage:" instead of rebuilding."""
    if not os.path.exists(DRIVER):
        return True
    built = os.path.getmtime(DRIVER)
    for src in ("tools/dump_score_events.c", "src/burnout3_score_events.c",
                "src/burnout3_score_events.h"):
        path = os.path.join(ROOT, src)
        if os.path.exists(path) and os.path.getmtime(path) > built:
            return True
    return False


def main():
    if _driver_stale():
        print("building the differential driver...")
        r = subprocess.run(
            ["gcc", "-Wall", "-Wextra", "-std=c11", "-O2", "-Isrc",
             "-o", DRIVER, "tools/dump_score_events.c",
             "src/burnout3_score_events.c", "-lm"], cwd=ROOT)
        if r.returncode != 0:
            print("driver build FAILED")
            return 1

    results = []
    print("=== AIR / ONCOMING / DRIFT (FUN_00196940/BE0/E10) ===")
    case_cat(results)
    print("=== near-miss proximity test (FUN_00195DD0) ===")
    case_obb(results)
    print("=== NEAR MISS sequences (FUN_00194EE0) ===")
    case_near_miss(results)
    print("=== CONTACT -> near-miss cancel (FUN_00197920) ===")
    case_contact(results)
    print("=== CONTACT -> rubbing arrays (FUN_001979E0) ===")
    case_mark_contact(results)
    print("=== RUBBING (FUN_00194A80) ===")
    case_rubbing(results)
    print("=== TAKEDOWN vs NEAR MISS (FUN_00197920 + FUN_00194EE0) ===")
    case_takedown_vs_nearmiss(results)
    print("=== CRASH GATE (FUN_001935F0 @0x001939AD..0x00193CE4) ===")
    case_crash_gate(results)
    case_crash_reset(results)
    case_crash_vs_run(results)

    npass = sum(1 for _, ok, _ in results if ok)
    for name, ok, bad in results:
        print("  %-52s %s" % (name[:52], "PASS" if ok else "FAIL"))
        for b in bad:
            print("      " + b)
    print("\n%d/%d" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
