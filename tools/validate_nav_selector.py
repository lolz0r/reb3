#!/usr/bin/env python3
"""Differential regression for FUN_00178310's nav-span selector mask.

The target follower uses this predicate before it chooses a successor route.
This runner executes the retail function under Unicorn and compares it with
the recovered rule, keeping the unusual wrapped mode-zero branch covered.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emulate_ai as ea
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX


F_NAV_SPAN_MASK = 0x00178310
F_NAV_BRANCH_CROSS = 0x001746F0
SECTION = ea.OTHER + 0x100
SECTION_DATA = ea.OTHER + 0x200
LINKS = ea.OTHER + 0x400
PAIRS = ea.OTHER + 0x600
POINTS = ea.OTHER + 0x800
VECTOR_OUT = ea.OTHER + 0xA00
RACECAR_RACING_MODE = 1


def span_mask(flags, start, end, loop, racing_mode, type4_blocked,
              type13_enabled, type4_allowed, type13_allowed):
    """Register-accurate FUN_00178310 mask model.

    bit 1 requests a successor selection, bit 2 reports a clamped open-row
    end, and bit 4 rejects a type-5 current node or a disallowed gate.
    """
    count = len(flags)
    assert count and 0 <= start < count
    if (flags[start] & 7) == 5:
        return 4

    result = 0
    if end >= count:
        if loop:
            end -= count
        else:
            end = count - 1
            result = 2

    def apply_gate(node):
        nonlocal result
        kind = flags[node] & 7
        if not racing_mode:
            if kind == 4:
                result |= 1
            return
        if not type4_blocked and kind == 4:
            result |= 1 if type4_allowed else 4
        if type13_enabled and kind in (1, 3):
            result |= 1 if type13_allowed else 4

    if start <= end:
        for node in range(start, end + 1):
            apply_gate(node)
    else:
        for node in range(start, count):
            apply_gate(node)
        # The retail wrap tail has a distinct mode-zero fast path: it sets
        # bit 1 for every scanned tail node rather than checking type 4.
        if not racing_mode:
            result |= 1
        else:
            for node in range(0, end + 1):
                apply_gate(node)
    return result


def retail_mask(flags, start, end, loop, racing_mode, type4_blocked,
                type13_enabled, type4_allowed, type13_allowed):
    session = ea.Session()
    session.wu(SECTION + 4, SECTION_DATA)
    session.wu(SECTION_DATA + 8, LINKS)
    session.uc.mem_write(SECTION_DATA + 0xC, len(flags).to_bytes(2, 'little'))
    session.wb(SECTION_DATA + 0xE, loop)
    session.wu(ea.RC + 0x1920, RACECAR_RACING_MODE if racing_mode else 0)
    session.wb(ea.AI + 0x1F1, type4_blocked)
    session.wb(ea.AI + 0x1F0, 0 if type13_enabled else 1)
    session.wu(ea.AI + 0x1FC, 2 if type13_enabled else 0)
    session.wb(ea.AI + 0x291, type4_allowed)
    session.wb(ea.AI + 0x292, type13_allowed)
    for node, kind in enumerate(flags):
        session.wb(LINKS + node * 10 + 3, kind)
    session.call(F_NAV_SPAN_MASK,
                 regs={UC_X86_REG_EAX: 0, UC_X86_REG_ECX: SECTION,
                       UC_X86_REG_EDX: start},
                 stack_args=(ea.AI, end))
    assert session.fault is None, session.fault
    return session.uc.reg_read(UC_X86_REG_EAX)


def retail_branch_cross(raw_forward_z):
    """Run FUN_001746F0 over a four-pair straight synthetic section."""
    session = ea.Session()
    session.wu(SECTION + 4, SECTION_DATA)
    session.wu(SECTION_DATA, PAIRS)
    session.uc.mem_write(SECTION_DATA + 0xC, (4).to_bytes(2, 'little'))
    session.wb(SECTION_DATA + 0xE, 0)
    session.wu(0x0073A174, POINTS)
    for point in range(8):
        x = -1.0 if point % 2 == 0 else 1.0
        z = (point // 2) * raw_forward_z
        session.uc.mem_write(POINTS + point * 16,
                             struct.pack('<4f', x, 0.0, z, 1.0))
    for pair in range(4):
        session.uc.mem_write(PAIRS + pair * 4,
                             struct.pack('<HH', pair * 2, pair * 2 + 1))
    session.call(F_NAV_BRANCH_CROSS,
                 regs={UC_X86_REG_ECX: SECTION},
                 stack_args=(0, VECTOR_OUT))
    assert session.fault is None, session.fault
    return struct.unpack('<4f', session.uc.mem_read(VECTOR_OUT, 16))[0]


CASES = [
    ("plain row", [0, 0, 0], 0, 2, 0, 1, 0, 0, 1, 1),
    ("type 5 current reject", [5, 4, 1], 0, 2, 0, 1, 0, 1, 1, 1),
    ("type 4 request", [0, 4, 0], 0, 2, 0, 1, 0, 0, 1, 1),
    ("type 4 rejected", [0, 4, 0], 0, 2, 0, 1, 0, 0, 0, 1),
    ("type 4 disabled", [0, 4, 0], 0, 2, 0, 1, 1, 0, 1, 1),
    ("type 1 request", [0, 1, 0], 0, 2, 0, 1, 0, 1, 1, 1),
    ("type 3 rejected", [0, 3, 0], 0, 2, 0, 1, 0, 1, 1, 0),
    ("type 1 disabled", [0, 1, 0], 0, 2, 0, 1, 0, 0, 1, 1),
    ("open clamp", [0, 0, 0], 1, 9, 0, 1, 0, 0, 1, 1),
    ("loop wrap", [0, 0, 4], 2, 4, 1, 1, 0, 0, 1, 1),
    ("mode-zero plain", [0, 4, 0], 0, 2, 0, 0, 0, 0, 1, 1),
    ("mode-zero wrap tail", [0, 0, 0], 2, 4, 1, 0, 0, 0, 1, 1),
]


def main():
    for case in CASES:
        name, *args = case
        got = retail_mask(*args)
        want = span_mask(*args)
        assert got == want, "%s: retail %#x != model %#x" % (name, got, want)
    assert retail_branch_cross(10.0) > 0.0
    assert retail_branch_cross(-10.0) < 0.0
    print("nav target selector mask and tie-break: OK (%d retail mask cases)" %
          len(CASES))


if __name__ == '__main__':
    main()
