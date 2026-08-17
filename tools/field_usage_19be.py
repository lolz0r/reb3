#!/usr/bin/env python3
"""Scan burnout3.elf .text for every instruction whose memory operand carries
displacement 0x19BE (racecar field access). Reports read vs write (base reg
class) and the containing function / address, so we can find the writer of the
recovery-gate byte racecar+0x19BE. RE_NOTES / TODO list this byte as the
prime suspect for the wreck-recovery releaser."""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM

DISP = 0x19be

data = open('build/burnout3.elf', 'rb').read()
e_phoff = struct.unpack_from('<I', data, 28)[0]
e_phentsize = struct.unpack_from('<H', data, 42)[0]
e_phnum = struct.unpack_from('<H', data, 44)[0]
segs = []
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    pt = struct.unpack_from('<I', data, off)[0]
    if pt == 1:
        po, pv = struct.unpack_from('<II', data, off + 4)[0], struct.unpack_from('<I', data, off + 8)[0]
        psz = struct.unpack_from('<I', data, off + 16)[0]
        segs.append((pv, po, psz))
segs.sort()

# locate the executable segment(s)
def seg_for(va):
    for v, o, fz in segs:
        if v <= va < v + fz:
            return v, o, fz
    return None

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

hits = []
for (v, o, fz) in segs:
    blob = data[o:o + fz]
    base = v
    for ins in md.disasm(blob, base):
        opnds = ins.operands
        disp = None
        is_write = False
        for op in opnds:
            if op.type == X86_OP_MEM:
                disp = op.mem.disp
                # displacement 0x19be appears as a small positive disp; but a
                # reg+disp with disp==0x19be is what we want
        if disp is None:
            continue
        if (disp & 0xffffffff) == DISP or disp == DISP or disp == -DISP:
            pass
        else:
            continue
        # heuristics: classify read/write from mnemonic
        m = ins.mnemonic
        if m.startswith('mov') and len(opnds) >= 2 and opnds[0].type == X86_OP_MEM:
            is_write = True
        elif 'mov' in m and any(getattr(op, 'type', None) == X86_OP_MEM for op in opnds):
            # movx r, [mem] -> read
            is_write = False
        hits.append((ins.address, m, ins.op_str, is_write))

print(f"instructions referencing disp {hex(DISP)}: {len(hits)}")
for a, m, os_, w in hits:
    kind = 'WRITE' if w else 'read '
    print(f"0x{a:08x}  {kind}  {m:6s} {os_}")
