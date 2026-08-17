#!/usr/bin/env python3
"""Recovery-path audit: crash set (+0x210=1), crash clear (+0x210=0), and any
time-comparison against the +0x240C deadline."""
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
clear, set1, ddl_cmp = [], [], []
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
        if m.startswith('comiss') or m.startswith('ucomiss') or m == 'cmp':
            for op in ins.operands:
                if op.type == X86_OP_MEM and (op.mem.disp & 0xffffffff) == 0x240c:
                    ddl_cmp.append((ins.address, m, ins.op_str))
        for op in ins.operands:
            if op.type != X86_OP_MEM or (op.mem.disp & 0xffffffff) != 0x210:
                continue
            if m in ('mov', 'movzx') and len(ins.operands) >= 2 and ins.operands[0].type == X86_OP_MEM and ins.operands[1].type == X86_OP_IMM:
                val = ins.operands[1].imm & 0xff
                if val == 0:
                    clear.append((ins.address, m, ins.op_str))
                elif val == 1:
                    set1.append((ins.address, m, ins.op_str))

print("=== +0x210 SET to 1 (crash begin) ===")
for a, m, os_ in set1:
    print("   0x%08x %-6s %s" % (a, m, os_))
print("=== +0x210 CLEARED to 0 (recovery done) ===")
for a, m, os_ in clear:
    print("   0x%08x %-6s %s" % (a, m, os_))
print("=== comparisons against +0x240c ===")
for a, m, os_ in ddl_cmp:
    print("   0x%08x %-8s %s" % (a, m, os_))
