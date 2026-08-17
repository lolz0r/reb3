#!/usr/bin/env python3
"""Anchored anti-misalignment sweep for displacement 0x19BE (racecar field).

Disassembly is aligned to real function entries: Ghidra's FUN_* list gives the
starts, each stream is decoded from its entry to the next entry (or the end of
the containing PT_LOAD). Within a function, linear fall-through decoding from a
real entry is reliable for finding memory-displacement references (capstone
misaligns only across the segment base, which each entry re-anchors).

Reports every instruction whose RUNTIME-accessed displacement is 0x19BE, and
flags the assumed read/write direction."""
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
        # only decode executable segments (PF_X = 1)
        fl = struct.unpack_from('<I', data, off + 24)[0]
        segs.append((pv, po, psz, fl))
segs.sort()

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

def seg_at(va):
    for v, o, fz, fl in segs:
        if v <= va < v + fz:
            return v, o, fz, fl
    return None

# function entries -> va set, from a file supplied on argv (Ghidra list_functions)
import json
raw = json.load(open(sys.argv[1]))
fns = raw if isinstance(raw, list) else raw.get('functions', raw)
entries = sorted(int(f['address'], 16) for f in fns)

def next_entry(va):
    lo, hi = 0, len(entries) - 1
    ans = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if entries[mid] > va:
            ans = entries[mid]; hi = mid - 1
        else:
            lo = mid + 1
    return ans

seen = set()
hits = []
for e in entries:
    sg = seg_at(e)
    if not sg:
        continue
    v, o, fz, fl = sg
    if not (fl & 1):
        continue                      # skip non-executable
    end = next_entry(e)
    if end is None:
        end = v + fz
    end = min(end, v + fz)
    blob = data[o + (e - v): o + (e - v) + (end - e)]
    for ins in md.disasm(blob, e):
        for op in ins.operands:
            if op.type != X86_OP_MEM:
                continue
            disp = op.mem.disp
            if (disp & 0xffffffff) == DISP:
                key = (ins.address, ins.mnemonic, ins.op_str)
                if key in seen:
                    continue
                seen.add(key)
                hits.append((ins.address, ins.mnemonic, ins.op_str))

print(f"instructions referencing disp {hex(DISP)}: {len(hits)}")
for a, m, os_ in hits:
    print(f"0x{a:08x}  {m:6s} {os_}")
