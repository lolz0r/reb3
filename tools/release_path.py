#!/usr/bin/env python3
"""Confirm the crash-release path: who clears +0x18FA (wrecked), who consumes
+0x19BE (respawn latch), and any store of 0 to +0x210 (crash done)."""
import struct, json, bisect
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_IMM

data = open('build/burnout3.elf', 'rb').read()
e_phoff = struct.unpack_from('<I', data, 28)[0]
e_phnum = struct.unpack_from('<H', data, 44)[0]
segs = []
for i in range(e_phnum):
    off = e_phoff + i * 32
    pt = struct.unpack_from('<I', data, off)[0]
    if pt == 1:
        po, pv, psz = struct.unpack_from('<IIII', data, off + 4)[0], struct.unpack_from('<I', data, off + 8)[0], struct.unpack_from('<I', data, off + 16)[0]
        fl = struct.unpack_from('<I', data, off + 24)[0]
        segs.append((pv, po, psz, fl))
segs.sort()
fns = json.load(open('/tmp/fns.json'))['functions']
entries = sorted(int(f['address'], 16) for f in fns)

def seg_at(va):
    for v, o, fz, fl in segs:
        if v <= va < v + fz:
            return v, o, fz, fl
    return None

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
stores = {0x18fa: [], 0x19be: [], 0x210: []}
for e in entries:
    sg = seg_at(e)
    if not sg or not (sg[3] & 1):
        continue
    v, o, fz, fl = sg
    i = bisect.bisect_right(entries, e)
    end = entries[i] if i < len(entries) else v + fz
    end = min(end, v + fz)
    blob = data[o + (e - v): o + (e - v) + (end - e)]
    for ins in md.disasm(blob, e):
        m = ins.mnemonic
        for op in ins.operands:
            if op.type != X86_OP_MEM:
                continue
            disp = op.mem.disp & 0xffffffff
            if disp not in stores:
                continue
            # a store to memory is when the FIRST operand is the memory operand
            if ins.operands and ins.operands[0].type == X86_OP_MEM and op is ins.operands[0]:
                val = None
                if len(ins.operands) >= 2 and ins.operands[1].type == X86_OP_IMM:
                    val = ins.operands[1].imm
                stores[disp].append((ins.address, m, ins.op_str, val))

for t in (0x18fa, 0x19be, 0x210):
    print("=== stores to +%04x ===" % t)
    for a, m, os_, val in stores[t]:
        v = ("imm=0x%x" % val) if val is not None else ""
        print("   0x%08x %-6s %-42s %s" % (a, m, os_, v))
