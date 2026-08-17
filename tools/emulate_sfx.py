#!/usr/bin/env python3
"""
GROUND TRUTH for Burnout 3's crash/impact sound emitters.

Runs the game's REAL sound-emitter functions (the 0x0014Bxxx..0x00156xxx
audio module) under Unicorn over a real, initialised racecar-audio object,
and captures -- at the two API boundaries the emitters cross -- exactly which
wave the emitter asks for and with what gain and playback rate:

  0x001C99D0  RwaWaveDictFind(EAX = &packed base-40 wave name,
                              [esp+0] = bank, [esp+4] = randomise-variant)
              -> wave record.  Hooked: the packed name is decoded and the
              call is short-circuited to a synthetic record whose descriptor
              carries a known sample rate.
  0x001CD8D0  PlaySound3D(EDI = &params, ESI = 0x0040B844 manager)
              Hooked: the params block is read out
                +0x00 pos[3]   +0x0C vel[3]   +0x18 wave record
                +0x1C gain     +0x24 playback rate (Hz; -1 = native)

The object itself is not synthesised field-by-field: FUN_0014A710, the real
initialiser, is executed first so the {min impulse, max impulse, min gain,
max gain, min pitch, max pitch} tuples are the game's own.

Usage:
  python3 tools/emulate_sfx.py                 # sweep every emitter
  python3 tools/emulate_sfx.py --json
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
                               UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_XMM0)

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "ev", os.path.join(_here, "emulate_vehicle.py"))
ev = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ev)

CS = " -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_"


def b40(v):
    """FUN_001AECC0 base-40 decode (12 chars, LSB first, reversed)."""
    out = []
    for _ in range(12):
        out.append(CS[v % 40])
        v //= 40
    return ''.join(reversed(out)).rstrip()


INIT_AUDIO = 0x0014A710
FIND_WAVE = 0x001C99D0
PLAY_3D = 0x001CD8D0
PLAY_CFG = 0x001CD0D0        # post-play voice config, RET 0x1C

PAGE = 0x1000
OBJ = 0x30000000
OBJ_SZ = 0x10000
BANK = 0x37000000
REC = 0x38000000             # synthetic wave record
DESC = 0x38001000            # its descriptor (+0x10 = sample rate)
VOICE = 0x38002000           # synthetic voice returned by PlaySound3D
POS = 0x39000000             # a world position vec4
MGR = 0x33000000
RECORDS = 0x34000000
VTAB = 0x35000000
STUBS = 0x36000000
STACK = 0x20000000
STACK_SZ = 0x100000
MAGIC_RET = 0x50000000
MANAGER_GLOBAL = 0x004A1E94

RATE = 22050.0               # synthetic descriptor rate; pitch = freq / RATE

# One row per real emitter:
#   (address, wave, description, call-convention, [6 param offsets])
# Call conventions, read off each function's prologue:
#   'ecx'      obj=ECX, position vec4=EAX, impulse=XMM0
#   'eax_x0'   obj=EAX, impulse=XMM0
#   'eax_m8'   obj=EAX, arg0=position, arg1=impulse          (RET 0xC)
#   'st_obj_m8' arg0=obj, arg1=impulse, position=EAX          (RET 8)
#   'st4'      arg0=obj, impulse=XMM0                         (RET 4)
# Param offsets are the six the function actually reads, in the order
# {min impulse, max impulse, min gain, max gain, min pitch, max pitch}
# (the glass block is interleaved with stride 0xC, hence the explicit lists).
EMITTERS = [
    (0x0014F3E0, 'IMPACTNUDG', 'car-vs-car nudge (victim not crashed)',
     'ecx', [0x6D0, 0x6D4, 0x6D8, 0x6DC, 0x6E0, 0x6E4]),
    (0x0014F690, 'IMPACTFATA', 'car-vs-car impact, victim crashed',
     'st_obj_m8', [0x700, 0x704, 0x708, 0x70C, 0x710, 0x714]),
    (0x0014F130, 'IMPACTFATA', 'crash-mode fatal impact',
     'eax_m8', [0x6E8, 0x6EC, 0x6F0, 0x6F4, 0x6F8, 0x6FC]),
    (0x0014EEA0, 'IMPACTWORL', 'car-vs-world impact',
     'ecx', [0x6A0, 0x6A4, 0x6A8, 0x6AC, 0x6B0, 0x6B4]),
    (0x0014D5F0, 'GLASSFRONT', 'windscreen breaking',
     'eax_m8', [0x598, 0x5A4, 0x5B0, 0x5BC, 0x5C8, 0x5D4]),
    (0x0014D8A0, 'GLASSSIDES', 'side windows breaking',
     'eax_m8', [0x5A0, 0x5AC, 0x5B8, 0x5C4, 0x5D0, 0x5DC]),
    (0x0014DB50, 'GLASSWINDS', 'rear window breaking',
     'eax_m8', [0x59C, 0x5A8, 0x5B4, 0x5C0, 0x5CC, 0x5D8]),
    (0x0014EB00, 'CARPARTLDE', 'large panel deform',
     'eax_x0', [0x5E0, 0x5E4, 0x5E8, 0x5EC, 0x5F0, 0x5F4]),
    (0x0014ECB0, 'CARPARTMDE', 'medium panel deform',
     'eax_x0', [0x5F8, 0x5FC, 0x600, 0x604, 0x608, 0x60C]),
    (0x0014FC80, 'CARPARTLRE', 'large panel detached',
     'eax_x0', [0x610, 0x614, 0x618, 0x61C, 0x620, 0x624]),
    (0x0014FE60, 'CARPARTMRE', 'medium panel detached',
     'eax_x0', [0x628, 0x62C, 0x630, 0x634, 0x638, 0x63C]),
    (0x00150040, 'CARPARTLPR', 'loose large panel hitting the world',
     'eax_x0', [0x640, 0x644, 0x648, 0x64C, 0x650, 0x654]),
    (0x00150260, 'CARPARTMPR', 'loose medium panel hitting the world',
     'eax_x0', [0x660, 0x664, 0x668, 0x66C, 0x670, 0x674]),
    (0x00150480, 'CARPARTWPR', 'loose wheel hitting the world',
     'eax_x0', [0x680, 0x684, 0x688, 0x68C, 0x690, 0x694]),
    (0x00150B90, 'KURBBOUNCE', 'kerb strike',
     'st4', None),
]


class Sfx:
    def __init__(self, rand_scale=0.0):
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        ev.load_elf(uc, os.path.join(_here, '..', ev.ELF)
                    if not os.path.exists(ev.ELF) else ev.ELF)
        for base, size in ((OBJ, OBJ_SZ), (BANK, 0x10000), (REC, 0x10000),
                           (POS, PAGE), (MGR, PAGE), (RECORDS, 0x20000),
                           (VTAB, PAGE), (STUBS, PAGE), (STACK, STACK_SZ),
                           (MAGIC_RET & ~(PAGE - 1), PAGE)):
            uc.mem_map(base, size, UC_PROT_ALL)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)
        self.uc = uc

        # registry manager (same shape extract_car_vdb.py uses)
        uc.mem_write(MANAGER_GLOBAL, struct.pack('<I', MGR))
        uc.mem_write(MGR + 0x10, struct.pack('<IIIII', VTAB, 0, 0,
                                             RECORDS, 0x400))
        uc.mem_write(STUBS, b"\x31\xC0\xC2\x04\x00")
        uc.mem_write(STUBS + 0x10, b"\xB0\x01\xC2\x04\x00")
        uc.mem_write(VTAB, struct.pack('<IIII', STUBS, STUBS, STUBS,
                                       STUBS + 0x10))

        self._run(INIT_AUDIO, {UC_X86_REG_EAX: OBJ, UC_X86_REG_ESI: OBJ})

        # synthetic wave record / descriptor / voice
        uc.mem_write(REC + 8, struct.pack('<I', DESC))
        uc.mem_write(DESC + 0x10, struct.pack('<I', int(RATE)))
        uc.mem_write(VOICE + 0xC, struct.pack('<I', VOICE + 0x100))

        # object runtime state: bank loaded, LCG seeded, no cooldowns
        uc.mem_write(OBJ + 0x880, struct.pack('<I', BANK))
        uc.mem_write(OBJ + 0x520, struct.pack('<II', 0x12345678, 0x9ABCDEF0))
        for off in range(0x8C0, 0x8F0):
            uc.mem_write(OBJ + off, b'\0')
        uc.mem_write(POS, struct.pack('<ffff', 10.0, 1.0, -20.0, 1.0))
        # one active viewport (0x0073A1C0) with its "audible" flag set --
        # the glass emitters walk this list (0x0014D607..0x0014D639) and go
        # silent when no viewport can hear the car.
        uc.mem_write(0x0073A1C0, struct.pack('<I', 1))
        uc.mem_write(0x0073BACA, b'\x01')
        # 0x0054F46C scales the per-object LCG into [0,1) for FUN_0014A6B0's
        # +/-10% pitch variance.  It is BSS, so the image value is 0 and the
        # variance collapses to its lower bound; seeding the real scale
        # (tools/emulate_pipeline.py G_RAND_SCALE) makes it genuinely random.
        if rand_scale:
            uc.mem_write(0x0054F46C, struct.pack('<f', rand_scale))

        self.captured = []
        uc.hook_add(UC_HOOK_CODE, self._on_find, begin=FIND_WAVE,
                    end=FIND_WAVE)
        uc.hook_add(UC_HOOK_CODE, self._on_play, begin=PLAY_3D, end=PLAY_3D)
        uc.hook_add(UC_HOOK_CODE, self._on_cfg, begin=PLAY_CFG, end=PLAY_CFG)
        self._pending_name = None

    # ---------------------------------------------------------------- hooks
    @staticmethod
    def _unmapped(mu, access, address, size, value, user):
        try:
            mu.mem_map(address & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        except UcError:
            return False
        return True

    def _ret(self, mu, pop):
        esp = mu.reg_read(UC_X86_REG_ESP)
        ret = struct.unpack('<I', mu.mem_read(esp, 4))[0]
        mu.reg_write(UC_X86_REG_ESP, esp + 4 + pop)
        mu.reg_write(UC_X86_REG_EIP, ret)

    def _on_find(self, mu, address, size, user):
        ptr = mu.reg_read(UC_X86_REG_EAX)
        try:
            v = struct.unpack('<Q', mu.mem_read(ptr, 8))[0]
            self._pending_name = b40(v)
        except UcError:
            self._pending_name = '?'
        mu.reg_write(UC_X86_REG_EAX, REC)
        self._ret(mu, 8)

    def _on_play(self, mu, address, size, user):
        p = mu.reg_read(UC_X86_REG_EDI)
        try:
            blk = bytes(mu.mem_read(p, 0x30))
        except UcError:
            blk = b'\0' * 0x30
        pos = struct.unpack_from('<3f', blk, 0)
        gain = struct.unpack_from('<f', blk, 0x1C)[0]
        freq = struct.unpack_from('<f', blk, 0x24)[0]
        self.captured.append({'wave': self._pending_name, 'gain': gain,
                              'freq': freq,
                              'pitch': (freq / RATE) if freq > 0 else 1.0,
                              'pos': pos})
        mu.reg_write(UC_X86_REG_EAX, VOICE)
        self._ret(mu, 0)

    def _on_cfg(self, mu, address, size, user):
        mu.reg_write(UC_X86_REG_EAX, 0)
        self._ret(mu, 0x1C)

    # ----------------------------------------------------------------- run
    def _run(self, func, regs, stack=(), max_steps=20_000_000):
        uc = self.uc
        sp = STACK + STACK_SZ - 0x8000
        uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
        for i, w in enumerate(stack):
            uc.mem_write(sp + 4 + 4 * i, struct.pack('<I', w))
        uc.reg_write(UC_X86_REG_ESP, sp)
        for r, v in regs.items():
            uc.reg_write(r, v)
        try:
            uc.emu_start(func, MAGIC_RET, count=max_steps)
        except UcError as e:
            return "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
        return None

    def _set_xmm0(self, f):
        self.uc.reg_write(UC_X86_REG_XMM0,
                          struct.unpack('<I', struct.pack('<f', f))[0])

    def clear_cooldowns(self):
        for off in range(0x8C0, 0x8F0):
            self.uc.mem_write(OBJ + off, b'\0')

    def fire(self, addr, kind, mag):
        """Invoke one emitter with `mag` as its impulse -> captured voices."""
        self.captured = []
        self.clear_cooldowns()
        magbits = struct.unpack('<I', struct.pack('<f', mag))[0]
        stack = ()
        if kind == 'ecx':
            regs = {UC_X86_REG_ECX: OBJ, UC_X86_REG_EAX: POS,
                    UC_X86_REG_ESI: OBJ, UC_X86_REG_EDI: POS}
            stack = (POS, magbits, POS)
        elif kind == 'eax_x0':
            regs = {UC_X86_REG_EAX: OBJ, UC_X86_REG_ECX: OBJ,
                    UC_X86_REG_EDI: POS, UC_X86_REG_ESI: OBJ}
            stack = (POS, magbits, POS)
        elif kind == 'eax_m8':
            regs = {UC_X86_REG_EAX: OBJ, UC_X86_REG_ECX: OBJ,
                    UC_X86_REG_EDI: POS, UC_X86_REG_ESI: OBJ}
            stack = (POS, magbits, POS)
        elif kind == 'st_obj_m8':
            regs = {UC_X86_REG_EAX: POS, UC_X86_REG_ECX: OBJ,
                    UC_X86_REG_EDI: POS, UC_X86_REG_ESI: OBJ}
            stack = (OBJ, magbits, POS)
        elif kind == 'st4':
            regs = {UC_X86_REG_EAX: OBJ, UC_X86_REG_ECX: OBJ,
                    UC_X86_REG_EDI: POS, UC_X86_REG_ESI: OBJ}
            stack = (OBJ, magbits, POS)
        else:
            raise ValueError(kind)
        self._set_xmm0(mag)
        err = self._run(addr, regs, stack)
        return list(self.captured), err

    def param(self, off):
        return struct.unpack('<f', self.uc.mem_read(OBJ + off, 4))[0]


def sweep():
    s = Sfx()
    rows = []
    for addr, wave, label, kind, offs in EMITTERS:
        tup = [s.param(o) for o in offs] if offs else None
        entry = {'addr': "FUN_%08X" % addr, 'wave': wave, 'label': label,
                 'kind': kind, 'offsets': offs, 'params': tup, 'shots': []}
        if tup:
            lo, hi = tup[0], tup[1]
            mags = [lo * 0.5, lo, lo + (hi - lo) * 0.25,
                    lo + (hi - lo) * 0.5, lo + (hi - lo) * 0.75, hi, hi * 2.0]
        else:
            mags = [0.0, 1.0, 10.0, 100.0, 1000.0]
        for m in mags:
            caps, err = s.fire(addr, kind, m)
            entry['shots'].append({'mag': m, 'voices': caps, 'err': err})
        rows.append(entry)
    return rows


def main():
    rows = sweep()
    if '--json' in sys.argv:
        json.dump(rows, sys.stdout, indent=1)
        return
    for e in rows:
        p = e['params']
        print("%s  %-11s %s" % (e['addr'], e['wave'], e['label']))
        if p:
            print("   +0x%03X  minImp %-8.4g maxImp %-8.4g gain %.3g..%.3g"
                  "  pitch %.3g..%.3g"
                  % (e['offsets'][0], p[0], p[1], p[2], p[3], p[4], p[5]))
        for sh in e['shots']:
            if not sh['voices']:
                print("     imp %-9.4g -> (silent)%s"
                      % (sh['mag'], "  ERR " + sh['err'] if sh['err'] else ""))
            for v in sh['voices']:
                print("     imp %-9.4g -> %-12s gain %.4f  pitch %.4f"
                      % (sh['mag'], v['wave'], v['gain'], v['pitch']))
        print()


if __name__ == '__main__':
    main()
