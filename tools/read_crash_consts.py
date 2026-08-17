#!/usr/bin/env python3
"""Read float32 constants at VMA addresses in build/burnout3.elf.
Validated against the known DAT_003B1694 = 5.0 pin before trusting output."""
import struct

data = open('build/burnout3.elf', 'rb').read()
e_phoff = struct.unpack_from('<I', data, 28)[0]
e_phnum = struct.unpack_from('<H', data, 44)[0]
segs = []
for i in range(e_phnum):
    off = e_phoff + i * 32
    (pt, offv, vaddr, paddr, filesz, memsz, flags, align) = struct.unpack_from('<IIIIIIII', data, off)
    if pt == 1 and filesz:
        segs.append((vaddr, offv, filesz))

def file_off(vaddr):
    for vbase, foff, fsz in segs:
        if vbase <= vaddr < vbase + fsz:
            return foff + (vaddr - vbase)
    return None

def read_float(vaddr):
    roff = file_off(vaddr)
    if roff is None or roff + 4 > len(data):
        return None
    return struct.unpack_from('<f', data, roff)[0], data[roff:roff+4].hex()

# Validate the mapping with a known pin first.
f, hx = read_float(0x003b1694)
print(f"VALIDATE 0x003b1694 = {f!r} ({hx})  [expected 5.0 = 40a00000]")
print()

labels = {
    0x003b1708: "class2 fresh (rec+0x12==0)",
    0x003b16b4: "class2 presented ; other presented !17390",
    0x003b1698: "other fresh (rec+0x12==0)",
    0x003b1c5c: "other presented 17390",
    0x003b1730: "rec+0x13c 2nd-timer",
    0x003b1688: "rec+0x134 2nd-timer value",
    0x003b1694: "known pin (5.0)",
}
for a, lab in sorted(labels.items()):
    f, hx = read_float(a)
    print(f"0x{a:08x}  {f!r:>16}  {hx}   # {lab}")
