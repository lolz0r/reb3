#!/usr/bin/env python3
"""
Classify how a function touches the vehicle struct: which offsets are read,
which are written, and with what instruction.

Motivation: porting FUN_0011D460 is blocked on 10 unidentified struct regions.
Identifying them by reading 1,000 lines of decompiler output is slow and is
where most of my errors have come from. A read/write map is mechanical and
checkable: a field that is only ever READ in the physics update is an input
(config or state from an earlier stage); a field only ever WRITTEN is an output;
read-then-written in the same function is integrated state.

This is the static half of what an emulator trace would give. It cannot tell you
what a value MEANS, but it tells you the role of every offset, which is enough to
prioritise and to sanity-check guesses.

Usage: python3 tools/field_usage.py [func_addr] [struct_size]
"""
import json
import re
import sys
import urllib.request
from collections import defaultdict

MCP = "http://127.0.0.1:8089"

# Instructions that write their memory operand vs read it. Anything not listed
# is reported as "other" rather than silently assumed.
WRITE_OPS = {"MOV", "MOVSS", "MOVSD", "MOVUPS", "MOVAPS", "FSTP", "FST",
             "ADD", "SUB", "AND", "OR", "XOR", "INC", "DEC", "SETNZ", "SETZ"}
READ_ONLY_OPS = {"CMP", "TEST", "FLD", "FCOM", "FCOMP", "UCOMISS", "COMISS",
                 "PUSH", "LEA"}

# MOV/MOVSS with the memory operand first = store, second = load.
MEM_OPERAND = re.compile(r'\[(E[A-D]X|E[SD]I|EBP|ESP)( \+ (0x[0-9a-f]+))?\]')


def disassemble(addr):
    url = "%s/disassemble_function?address=%s&limit=20000" % (MCP, addr)
    return json.loads(urllib.request.urlopen(url, timeout=600).read())["instructions"]


def classify(ins, struct_size):
    """Return {offset: {"r": n, "w": n, "ops": set()}} keyed by struct offset."""
    usage = defaultdict(lambda: {"r": 0, "w": 0, "ops": set()})
    for x in ins:
        text = x["instruction"]
        m = MEM_OPERAND.search(text)
        if not m or not m.group(3):
            continue
        off = int(m.group(3), 16)
        if not (0 <= off < struct_size):
            continue
        # ESP/EBP-relative is stack, not the struct.
        if m.group(1) in ("ESP", "EBP"):
            continue
        op = text.split()[0]
        operands = text[len(op):].strip()
        mem_is_dest = operands.startswith("[") or operands.startswith("dword ptr [")
        rec = usage[off]
        rec["ops"].add(op)
        if op in READ_ONLY_OPS:
            rec["r"] += 1
        elif op in WRITE_OPS and mem_is_dest:
            rec["w"] += 1
        else:
            rec["r"] += 1
    return usage


def main():
    addr = sys.argv[1] if len(sys.argv) > 1 else "0x0011d460"
    size = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x14D0

    ins = disassemble(addr)
    usage = classify(ins, size)

    # Known fields, so the report separates "already identified" from "to do".
    known = {}
    try:
        src = open("src/burnout3_vehicle_struct.h").read()
        for nm, off in re.findall(r'#define (B3_V_\w+)\s+0x([0-9A-Fa-f]+)u', src):
            known[int(off, 16)] = nm[5:].lower()
    except OSError:
        pass

    print("%-8s %-4s %-4s %-6s %-22s %s" % ("offset", "rd", "wr", "role", "known", "ops"))
    unknown_rw = []
    for off in sorted(usage):
        u = usage[off]
        if u["w"] and u["r"]:
            role = "state"
        elif u["w"]:
            role = "out"
        else:
            role = "in"
        nm = known.get(off, "")
        if not nm:
            unknown_rw.append((off, role, u["r"], u["w"]))
        print("0x%04X   %-4d %-4d %-6s %-22s %s"
              % (off, u["r"], u["w"], role, nm or "-", ",".join(sorted(u["ops"]))[:34]))

    print("\n%d distinct offsets touched; %d already identified, %d unknown"
          % (len(usage), len(usage) - len(unknown_rw), len(unknown_rw)))
    if unknown_rw:
        outs = [o for o, r, _, _ in unknown_rw if r == "out"]
        states = [o for o, r, _, _ in unknown_rw if r == "state"]
        ins_ = [o for o, r, _, _ in unknown_rw if r == "in"]
        print("  unknown outputs (written only) : %s"
              % " ".join("0x%04X" % o for o in outs) or "none")
        print("  unknown state (read+written)   : %s"
              % " ".join("0x%04X" % o for o in states) or "none")
        print("  unknown inputs (read only)     : %s"
              % " ".join("0x%04X" % o for o in ins_) or "none")


if __name__ == "__main__":
    main()
