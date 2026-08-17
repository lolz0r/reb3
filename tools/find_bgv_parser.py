#!/usr/bin/env python3
"""
Find Burnout 3's .bgv (vehicle model) parser by running candidate functions
against a real file under emulation.

Rationale: the Burnout modding community never solved the .bgv mesh format --
their Noesis plugin loads vehicle textures and stops. But the game parses these
files every frame, so its own loader is the spec. Static searching for the
parser is noisy (the header pointers live at small disp8 offsets that occur
everywhere), so instead we execute candidates and watch their access pattern.

A real parser, handed a pointer to the file image, should:
  * read the header table pointers (+0x4C..+0x58) and/or the texture ptr (+0x60)
  * then follow them, i.e. read at the offsets those pointers name

That "read a pointer, then read where it points" signature is very hard to
produce by accident, so it discriminates far better than offset frequency.

Requires: pip install unicorn
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import importlib.util
import json
import struct
import sys
import urllib.request

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_MEM_READ, UC_PROT_ALL, UcError)
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX,
                               UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX)

_spec = importlib.util.spec_from_file_location("ev", "tools/emulate_vehicle.py")
ev = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ev)

MCP = "http://127.0.0.1:8089"
PAGE = 0x1000
BGV = 0x60000000          # where the file image is mapped
STACK = 0x20000000
SCRATCH = 0x40000000
MAGIC_RET = 0x50000000

BGV_PATH = (game_path('pveh/COMP/Car1.bgv'))

HEADER_PTRS = (0x4C, 0x50, 0x54, 0x58, 0x60, 0x64, 0x68)


def load_file():
    data = open(BGV_PATH, 'rb').read()
    tables = {}
    for off in HEADER_PTRS:
        v = struct.unpack_from('<I', data, off)[0]
        if 0 < v < len(data):
            tables[off] = v
    return data, tables


def run_candidate(addr, data, tables, max_steps=120000):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, ev.ELF)
    for base, size in ((STACK, 0x100000), (SCRATCH, 0x100000),
                       (MAGIC_RET & ~(PAGE - 1), PAGE)):
        uc.mem_map(base, size, UC_PROT_ALL)
    span = (len(data) + PAGE - 1) & ~(PAGE - 1)
    uc.mem_map(BGV, span, UC_PROT_ALL)
    uc.mem_write(BGV, data)

    hdr_reads = set()
    follow_reads = set()
    targets = {BGV + v: off for off, v in tables.items()}

    def on_unmapped(u, access, address, size, value, user):
        try:
            u.mem_map(address & ~(PAGE - 1), PAGE, UC_PROT_ALL)
            return True
        except UcError:
            return False

    def on_read(u, access, address, size, value, user):
        if BGV <= address < BGV + len(data):
            rel = address - BGV
            if rel in HEADER_PTRS:
                hdr_reads.add(rel)
            # Did it read at/near where a header pointer points?
            for tgt, off in targets.items():
                if tgt <= address < tgt + 0x400:
                    follow_reads.add(off)

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    uc.hook_add(UC_HOOK_MEM_READ, on_read)

    sp = STACK + 0x100000 - 0x1000
    uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
    for i in range(1, 5):                  # buffer ptr in several arg slots
        uc.mem_write(sp + 4 * i, struct.pack('<I', BGV))
    uc.reg_write(UC_X86_REG_ESP, sp)
    for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX):
        uc.reg_write(r, BGV)               # and in every likely register

    try:
        uc.emu_start(addr, MAGIC_RET, count=max_steps)
    except UcError:
        pass
    return hdr_reads, follow_reads


def candidates():
    """Functions in the asset-loading neighbourhood."""
    url = "%s/list_functions?limit=20000" % MCP
    fns = json.loads(urllib.request.urlopen(url, timeout=300).read())["functions"]
    out = []
    for f in fns:
        a = int(f["address"], 16)
        if 0x00185000 <= a <= 0x00195000:
            out.append(a)
    return sorted(out)


def main():
    data, tables = load_file()
    print("Car1.bgv: 0x%X bytes; header pointers: %s"
          % (len(data), ", ".join("+0x%02X->0x%X" % (o, v)
                                  for o, v in sorted(tables.items()))))
    cands = candidates()
    print("testing %d functions in 0x185000..0x195000\n" % len(cands))

    hits = []
    for a in cands:
        try:
            hdr, follow = run_candidate(a, data, tables)
        except Exception:
            continue
        if hdr or follow:
            score = len(hdr) + 3 * len(follow)   # following a pointer counts more
            hits.append((score, a, sorted(hdr), sorted(follow)))

    hits.sort(reverse=True)
    if not hits:
        print("no function read the header pointers")
        return 1
    print("%-12s %-6s %-26s %s" % ("function", "score", "header ptrs read", "followed"))
    for score, a, hdr, follow in hits[:15]:
        print("FUN_%08x %-6d %-26s %s"
              % (a, score,
                 " ".join("+0x%02X" % h for h in hdr) or "-",
                 " ".join("+0x%02X" % f for f in follow) or "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
