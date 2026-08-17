#!/usr/bin/env python3
"""
Execute Burnout 3's real physics functions under a CPU emulator and observe
which vehicle-struct fields they read and write.

Why this exists: transcribing FUN_0011D460 by reading decompiler output is where
every one of my errors has come from, and a ported equation that produces
plausible numbers is indistinguishable from a correct one. Running the actual
x86 gives ground truth without needing a console emulator, a BIOS, or a GPU --
we only need the function, not the game.

Approach:
  * map build/burnout3.elf at its real VAs (the corrected image from xbe2elf.py)
  * place a synthetic vehicle struct in scratch memory
  * call the function with the vehicle pointer on the stack (__cdecl)
  * lazily map any page the code faults on, so execution proceeds instead of
    dying on the first unresolved pointer
  * diff the struct before/after to get the true write set
  * optionally vary one input and re-run, to see which outputs depend on it

Requires: pip install unicorn
"""
import struct
import sys

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UcError,
                     UC_PROT_ALL)
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX,
                               UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX)

ELF = "build/burnout3.elf"
PAGE = 0x1000

# Scratch layout, chosen well clear of the 0x10000..0x778200 image.
STACK_BASE = 0x20000000
STACK_SIZE = 0x100000
VEHICLE    = 0x30000000
# The struct is larger than 0x14D0: the drivetrain reads +0x1520/+0x1524/+0x153D.
VEHICLE_SZ = 0x2000
SCRATCH    = 0x40000000      # target for every pointer field in the vehicle
SCRATCH_SZ = 0x100000
MAGIC_RET  = 0x50000000      # sentinel return address; execution stops here


def load_elf(uc, path):
    data = open(path, 'rb').read()
    ph_off = struct.unpack_from('<I', data, 0x1C)[0]
    ph_num = struct.unpack_from('<H', data, 0x2C)[0]
    segs = []
    for i in range(ph_num):
        p_type, off, va, _, fsz, msz, _, _ = struct.unpack_from(
            '<IIIIIIII', data, ph_off + i * 32)
        if p_type == 1:
            segs.append((va, off, fsz, msz))
    if not segs:
        raise SystemExit("no PT_LOAD segments in " + path)

    # Page-rounded segments abut and sometimes overlap, so map the whole image
    # span in one call rather than per-segment (which silently left holes).
    lo = min(va for va, _, _, _ in segs) & ~(PAGE - 1)
    hi = max(va + msz for va, _, _, msz in segs)
    hi = (hi + PAGE - 1) & ~(PAGE - 1)
    uc.mem_map(lo, hi - lo, UC_PROT_ALL)
    for va, off, fsz, _ in segs:
        uc.mem_write(va, data[off:off + fsz])
    return lo, hi


class Tracer:
    """Lazily maps faulting pages and records struct-relative accesses."""

    def __init__(self, uc, struct_base, struct_size):
        self.uc = uc
        self.base = struct_base
        self.size = struct_size
        self.reads = {}
        self.writes = {}
        self.faults = 0

    def on_unmapped(self, uc, access, address, size, value, user):
        page = address & ~(PAGE - 1)
        try:
            uc.mem_map(page, PAGE, UC_PROT_ALL)
            self.faults += 1
        except UcError:
            return False
        return True  # retry the access

    def on_read(self, uc, access, address, size, value, user):
        if self.base <= address < self.base + self.size:
            off = address - self.base
            self.reads[off] = self.reads.get(off, 0) + 1

    def on_write(self, uc, access, address, size, value, user):
        if self.base <= address < self.base + self.size:
            off = address - self.base
            self.writes[off] = self.writes.get(off, 0) + 1


def build_vehicle(fields=None):
    """Synthetic vehicle image. Pointer fields aim at SCRATCH so derefs work."""
    buf = bytearray(VEHICLE_SZ)

    def put_f(off, val):
        struct.pack_into('<f', buf, off, val)

    def put_p(off, val):
        struct.pack_into('<I', buf, off, val & 0xFFFFFFFF)

    # Plausible non-zero state so branches are exercised and divides don't blow up.
    put_f(0x0B0, 0.0); put_f(0x0B4, 0.0); put_f(0x0B8, 1.0)   # vel dir +z
    put_f(0x0BC, 40.0)                                         # speed m/s
    put_f(0x1F0, 1000.0)                                       # mass
    put_f(0x1164, 5.0)                                         # steer deg
    buf[0x1169] = 4                                            # wheel count
    put_f(0x1360, 0.5)                                         # resist coef
    put_f(0x13D4, 89.48)                                       # speed mph
    put_f(0x1470, 6800.0)                                      # change up rpm
    put_f(0x149C, 500.0)                                       # engine omega
    put_p(0x14C8, 3); put_p(0x14CC, 3)                         # gears equal
    put_f(0x1408, 1.0); put_f(0x1440, 1.0); put_f(0x13C0, 0.01)

    # Every known pointer field -> scratch.
    for off in (0x204, 0x13F4, 0x13F8, 0x13FC):
        put_p(off, SCRATCH)
    # +0x14D8 is the transmission's back-pointer to its owning vehicle
    # (written by the transmission init FUN_001214A0's second argument;
    # FUN_00121560 reads speed through it).
    put_p(0x14D8, VEHICLE)

    # Offsets that are genuinely integer/pointer fields. Everything else in an
    # override dict is treated as a float -- relying on isinstance(val, float)
    # silently corrupted inputs when a caller passed `40` instead of `40.0`.
    INT_FIELDS = {0x204, 0x13F4, 0x13F8, 0x13FC, 0x14C8, 0x14CC, 0x1169,
                  # transmission block ints (see RE_NOTES 8.3): shifting flag,
                  # upshift inhibit, PRNG state pair, mode flag, gear count,
                  # owner pointer
                  0x14A4, 0x14A8, 0x14B0, 0x14B4, 0x14B8, 0x14C0, 0x14C4,
                  0x14D8, 0x1524,
                  # suspension solver environment pointers (FUN_001239C0 /
                  # FUN_00123FD0): ground poly soup, per-vehicle aux object,
                  # per-wheel frame matrix pointers
                  0x200, 0xCC4, 0xCC8, 0xCCC, 0xCD0, 0xCD4}
    for off, val in (fields or {}).items():
        if off in INT_FIELDS:
            put_p(off, int(val))
        else:
            put_f(off, float(val))
    return bytes(buf)


# The pointer fields on the vehicle resolve to a RenderWare frame whose first
# 0x40 bytes are a 4x4 transform matrix -- the drivetrain reads +0x00..+0x34
# heavily. Leaving it zeroed collapses every directional term and makes the
# function look branch-insensitive, which is what initially hid the real code
# paths. Seed it with identity so the maths is meaningful.
def scratch_image():
    buf = bytearray(0x200)
    ident = [1.0, 0.0, 0.0, 0.0,
             0.0, 1.0, 0.0, 0.0,
             0.0, 0.0, 1.0, 0.0,
             0.0, 0.0, 0.0, 1.0]
    for i, v in enumerate(ident):
        struct.pack_into('<f', buf, i * 4, v)
    return bytes(buf)


def run(func_addr, vehicle_image, max_steps=200000, scratch=None,
        stack_args=None, regs=None, mem_writes=None, ret_f32=False,
        ret_scratch=0):
    """Execute one function against a synthetic vehicle.

    Extensions (all optional, defaults reproduce the original behaviour):
      stack_args -- list of u32 values for [esp+4], [esp+8], ...
                    (default: [VEHICLE], the original single-argument call)
      regs       -- dict of unicorn register const -> value, applied after the
                    defaults, for __usercall functions (e.g. FUN_00121560 takes
                    its transmission block in ESI and a boost flag in EDI)
      mem_writes -- dict of absolute address -> bytes, written before the call.
                    Used to seed globals the function reads (e.g. the frame dt
                    at 0x0060EA1C and the PRNG scale at 0x0054F46C, both BSS
                    and therefore zero unless seeded).
      ret_f32    -- capture an x87 float return value: a stub at the return
                    address does FSTP dword so ST0 is observable. Stored on the
                    returned tracer as tr.ret_f32 (None if not requested).
      ret_scratch-- if nonzero, that many bytes of SCRATCH memory after the
                    call are stored on the tracer as tr.scratch_after (used by
                    the integrator cases, which modify the frame matrix that
                    lives behind the vehicle's +0x204 pointer).
    """
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    load_elf(uc, ELF)
    uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
    uc.mem_map(VEHICLE, VEHICLE_SZ, UC_PROT_ALL)
    uc.mem_map(SCRATCH, SCRATCH_SZ, UC_PROT_ALL)
    uc.mem_map(MAGIC_RET & ~(PAGE - 1), PAGE, UC_PROT_ALL)

    uc.mem_write(VEHICLE, vehicle_image)
    uc.mem_write(SCRATCH, scratch if scratch is not None else scratch_image())
    for addr, data in (mem_writes or {}).items():
        uc.mem_write(addr, data)

    tr = Tracer(uc, VEHICLE, VEHICLE_SZ)
    tr.ret_f32 = None
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, tr.on_unmapped)
    uc.hook_add(UC_HOOK_MEM_READ, tr.on_read)
    uc.hook_add(UC_HOOK_MEM_WRITE, tr.on_write)

    # [esp] = return address, [esp+4..] = stack arguments.
    sp = STACK_BASE + STACK_SIZE - 0x1000
    uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
    if stack_args is None:
        stack_args = [VEHICLE]
    for i, a in enumerate(stack_args):
        uc.mem_write(sp + 4 + i * 4, struct.pack('<I', a & 0xFFFFFFFF))
    uc.reg_write(UC_X86_REG_ESP, sp)
    for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX):
        uc.reg_write(r, VEHICLE)      # some callees take `this` in a register
    for r, val in (regs or {}).items():
        uc.reg_write(r, val)

    stop_at = MAGIC_RET
    if ret_f32:
        # FSTP dword ptr [MAGIC_RET+0x100]; stop after this instruction.
        out_addr = MAGIC_RET + 0x100
        uc.mem_write(MAGIC_RET, b"\xd9\x1d" + struct.pack('<I', out_addr))
        stop_at = MAGIC_RET + 6

    err = None
    try:
        uc.emu_start(func_addr, stop_at, count=max_steps)
    except UcError as e:
        err = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))

    if ret_f32 and err is None:
        tr.ret_f32 = struct.unpack('<f', uc.mem_read(MAGIC_RET + 0x100, 4))[0]

    tr.scratch_after = bytes(uc.mem_read(SCRATCH, ret_scratch)) \
        if ret_scratch else None
    after = uc.mem_read(VEHICLE, VEHICLE_SZ)
    return tr, bytes(after), err


def diff(before, after):
    out = {}
    for off in range(0, len(before), 4):
        b = before[off:off + 4]
        a = after[off:off + 4]
        if b != a:
            out[off] = (struct.unpack('<f', b)[0], struct.unpack('<f', a)[0],
                        struct.unpack('<I', b)[0], struct.unpack('<I', a)[0])
    return out


def main():
    addr = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x0011D460
    img = build_vehicle()
    tr, after, err = run(addr, img)

    print("function      : 0x%08X" % addr)
    print("steps         : %s" % ("faulted: " + err if err else "ran to return"))
    print("pages faulted : %d (lazily mapped)" % tr.faults)
    print("struct reads  : %d distinct offsets" % len(tr.reads))
    print("struct writes : %d distinct offsets" % len(tr.writes))

    d = diff(img, after)
    print("\nfields actually modified: %d" % len(d))
    for off in sorted(d):
        fb, fa, ib, ia = d[off]
        if abs(fa) < 1e18 and (fa != 0 or fb != 0):
            print("  +0x%04X  %-14.6g -> %-14.6g   (u32 0x%08X -> 0x%08X)"
                  % (off, fb, fa, ib, ia))
        else:
            print("  +0x%04X  u32 0x%08X -> 0x%08X" % (off, ib, ia))


if __name__ == "__main__":
    main()
