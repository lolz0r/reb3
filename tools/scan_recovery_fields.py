#!/usr/bin/env python3
"""Find all accesses to [reg+0x240C] (racecar recovery stamp) and the
crash-record +0x130 (duration) across the whole image, to determine which
drives the physical wreck release.

Method: disassemble the executable LOAD segment with capstone and collect
memory operands whose displacement is 0x240C (the racecar+0x240C stamp) or
0x130 (the crash-record duration). We then intersect with the functions that
we know touch crash state.
"""
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_REG

data = open('build/burnout3.elf', 'rb').read()
e_phoff = struct.unpack_from('<I', data, 28)[0]
e_phnum = struct.unpack_from('<H', data, 44)[0]
# Executable (R E) segments only
code = []
for i in range(e_phnum):
    off = e_phoff + i * 32
    (pt, offv, vaddr, paddr, filesz, memsz, flags, align) = struct.unpack_from('<IIIIIIII', data, off)
    if pt == 1 and (flags & 4) and filesz:   # PF_X
        code.append((vaddr, offv, filesz))
code.sort()

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

def v2file(vaddr):
    for vbase, foff, fsz in code:
        if vbase <= vaddr < vbase + fsz:
            return foff + (vaddr - vbase)
    return None

# Collect candidate addresses (displacement 0x240C or 0x130) with the
# register used. We can't resolve the register->struct mapping without types,
# so we report raw hits; the caller (RE notes) maps them.
hits_240c = []
hits_130 = []
for vbase, foff, fsz in code:
    chunk = data[foff:foff+fsz]
    for insn in md.disasm(chunk, vbase):
        for op in insn.operands:
            if op.type == X86_OP_MEM:
                d = op.mem.disp
                if d == 0x240c:
                    hits_240c.append((insn.address, insn.mnemonic, insn.op_str))
                elif d == 0x130:
                    hits_130.append((insn.address, insn.mnemonic, insn.op_str))

print(f"[+0x240C] hits: {len(hits_240c)}")
for a, m, o in hits_240c:
    print(f"  0x{a:08x}  {m} {o}")
print()
print(f"[+0x130] hits: {len(hits_130)}")
for a, m, o in hits_130[:80]:
    print(f"  0x{a:08x}  {m} {o}")
if len(hits_130) > 80:
    print(f"  ... {len(hits_130)-80} more")
