#!/usr/bin/env python3
"""
Ground truth for the game's CRASH / IMPACT / SCRAPE sound parameter block.

The per-racecar audio object (the `ESI` of FUN_0014F3E0 & friends) carries, at
+0x580..+0x900, a block of {min impulse, max impulse, min volume, max volume,
min pitch, max pitch} tuples -- one tuple per crash-sound category.  Every
emitter in the 0x0014Bxxx..0x00152xxx audio module reads its tuple out of that
block, lerps volume+pitch on the normalised impulse, and plays a wave.

Two things are recovered here, both by EXECUTION, not by reading:

  1. the compiled-in DEFAULTS -- run the real initialiser FUN_0014A710 under
     Unicorn over a scratch object and read the block back out;
  2. the parameter NAMES + ValueDB keys -- the initialiser's tail registers
     every field with the same registrar the physics config uses
     (0x001AEE20 -> hash core 0x001AF250), so hooking those two addresses
     yields {field offset, "<param><group>/<cfg>", hash} for each field,
     exactly as tools/extract_car_vdb.py does for VehiclePhysics.

The registered keys are then looked up in the retail ValueDB (Data/vdb.xml)
to get the values the shipped game actually runs with; where a key is absent
the compiled-in default stands.  That is the identical evidence chain as
tools/extract_car_vdb.py (which is validated against the community VDB dump).

Usage:
  python3 tools/emulate_sfx_params.py            # human-readable report
  python3 tools/emulate_sfx_params.py --json     # machine-readable
"""
import importlib.util
import json
import os
import struct
import sys

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED, \
    UC_HOOK_CODE, UC_PROT_ALL, UcError
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX,
                               UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI)

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "ev", os.path.join(_here, "emulate_vehicle.py"))
ev = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ev)

_spec2 = importlib.util.spec_from_file_location(
    "cv", os.path.join(_here, "extract_car_vdb.py"))
cv = importlib.util.module_from_spec(_spec2)
_spec2.loader.exec_module(cv)

INIT_AUDIO = 0x0014A710        # racecar-audio parameter initialiser  [C]
INITS = (0x0014A710, 0x0014B600)   # both halves of the registration chain
REG_ENTRY = 0x001AEE20         # per-parameter registration entry
HASH_ENTRY = 0x001AF250        # CRC core: ECX = string, EDX = length
HASH_RET = 0x001AF27F          # its RET: EAX = hash
MANAGER_GLOBAL = 0x004A1E94

PAGE = 0x1000
OBJ = 0x30000000               # the racecar-audio object (ESI)
OBJ_SZ = 0x10000
MGR = 0x33000000
RECORDS = 0x34000000
VTAB = 0x35000000
STUBS = 0x36000000
BANK = 0x37000000               # stand-in for the loaded crash wave bank
STACK = 0x20000000
STACK_SZ = 0x100000
MAGIC_RET = 0x50000000


def run_init(func=INIT_AUDIO, state=None, max_steps=50_000_000):
    """Execute an audio initialiser over a scratch object -> (blob, events).

    FUN_0014B600 is the audio object's message handler (switch on obj+0x00,
    cases 1..0x18); `state` selects the case, and the object is its single
    stack argument.  FUN_0014A710 takes the object in EAX.
    """
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, os.path.join(_here, '..', ev.ELF)
                if not os.path.exists(ev.ELF) else ev.ELF)
    for base, size in ((OBJ, OBJ_SZ), (MGR, PAGE), (RECORDS, 0x20000), (BANK, 0x10000),
                       (VTAB, PAGE), (STUBS, PAGE), (STACK, STACK_SZ),
                       (MAGIC_RET & ~(PAGE - 1), PAGE)):
        uc.mem_map(base, size, UC_PROT_ALL)

    # registry manager, identical shape to extract_car_vdb.run_registrar
    uc.mem_write(MANAGER_GLOBAL, struct.pack('<I', MGR))
    reg = MGR + 0x10
    uc.mem_write(reg, struct.pack('<IIIII', VTAB, 0, 0, RECORDS, 0x400))
    stub_ret4 = STUBS
    stub_al1 = STUBS + 0x10
    uc.mem_write(stub_ret4, b"\x31\xC0\xC2\x04\x00")
    uc.mem_write(stub_al1, b"\xB0\x01\xC2\x04\x00")
    uc.mem_write(VTAB, struct.pack('<IIII', stub_ret4, stub_ret4,
                                   stub_ret4, stub_al1))

    events = []
    pending = {'dest': None, 'string': None}

    def on_unmapped(mu, access, address, size, value, user):
        page = address & ~(PAGE - 1)
        try:
            mu.mem_map(page, PAGE, UC_PROT_ALL)
        except UcError:
            return False
        return True

    def on_reg_entry(mu, address, size, user):
        esp = mu.reg_read(UC_X86_REG_ESP)
        pending['dest'] = struct.unpack('<I', mu.mem_read(esp + 8, 4))[0]

    def on_hash_entry(mu, address, size, user):
        ptr = mu.reg_read(UC_X86_REG_ECX)
        ln = mu.reg_read(UC_X86_REG_EDX)
        try:
            pending['string'] = bytes(mu.mem_read(ptr, ln))
        except UcError:
            pending['string'] = None

    def on_hash_ret(mu, address, size, user):
        events.append({'dest': pending['dest'],
                       'string': pending['string'],
                       'hash': mu.reg_read(UC_X86_REG_EAX)})
        pending['dest'] = pending['string'] = None

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    uc.hook_add(UC_HOOK_CODE, on_reg_entry, begin=REG_ENTRY, end=REG_ENTRY)
    uc.hook_add(UC_HOOK_CODE, on_hash_entry, begin=HASH_ENTRY, end=HASH_ENTRY)
    uc.hook_add(UC_HOOK_CODE, on_hash_ret, begin=HASH_RET, end=HASH_RET)

    sp = STACK + STACK_SZ - 0x4000
    uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
    if state is not None:
        uc.mem_write(OBJ, struct.pack('<I', state))
        uc.mem_write(sp + 4, struct.pack('<I', OBJ))
        # obj+0x880 is the loaded crash AWD bank (FUN_001C9C80's result at
        # 0x0014B6DD).  Non-NULL == "bank already loaded", which is the path
        # that runs the parameter registration (0x0014B636 -> 0x0014B708).
        uc.mem_write(OBJ + 0x880, struct.pack('<I', BANK))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EAX, OBJ)     # 0014a751: MOV ESI,EAX
    uc.reg_write(UC_X86_REG_ESI, OBJ)
    uc.reg_write(UC_X86_REG_ECX, OBJ)
    uc.reg_write(UC_X86_REG_EDX, OBJ)

    err = None
    try:
        uc.emu_start(func, MAGIC_RET, count=max_steps)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
    blob = bytes(uc.mem_read(OBJ, 0x1000))
    return blob, events, err


def f32(blob, off):
    return struct.unpack_from('<f', blob, off)[0]


def load_vdb():
    """hash -> f32 value, straight out of the retail ValueDB."""
    raw, _filedefs = cv.read_vdb()
    return {h: cv.f32(v) for h, v in raw.items()}


def collect():
    vdb = load_vdb()
    out = []
    errs = []
    blob = None
    runs = [(0x0014A710, None)] + [(0x0014B600, s) for s in range(1, 0x19)]
    for func, state in runs:
        b, events, err = run_init(func, state)
        if blob is None:
            blob = b
        if err and events:
            errs.append("0x%08X/%s: %s" % (func, state, err))
        for e in events:
            if e['dest'] is None or not (OBJ <= e['dest'] < OBJ + OBJ_SZ):
                continue
            off = e['dest'] - OBJ
            s = (e['string'] or b'').decode('latin1')
            if any(p['offset'] == off for p in out):
                continue
            out.append({
                'offset': off,
                'key': s,
                'hash': e['hash'],
                'from': "FUN_%08X%s" % (func, "" if state is None
                                        else "/msg%d" % state),
                'default': f32(b, off),
                'vdb': vdb.get(e['hash']),
            })
    return blob, out, "; ".join(errs)


def main():
    blob, params, err = collect()
    if err:
        print("emulation note:", err, file=sys.stderr)
    if '--json' in sys.argv:
        json.dump({'params': params,
                   'block': {hex(o): f32(blob, o)
                             for o in range(0x580, 0x900, 4)}},
                  sys.stdout, indent=1)
        return
    print("FUN_0014A710 registered %d audio parameters\n" % len(params))
    print("%-6s %-46s %-11s %-11s" % ("off", "key", "default", "vdb"))
    for p in sorted(params, key=lambda x: x['offset']):
        print("+0x%03X %-46s %-11.5g %s"
              % (p['offset'], p['key'][:46], p['default'],
                 "%-11.5g" % p['vdb'] if p['vdb'] is not None else "-"))


if __name__ == '__main__':
    main()
