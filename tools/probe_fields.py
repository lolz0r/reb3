#!/usr/bin/env python3
"""
Identify unknown vehicle-struct fields by differential emulation.

Static reading tells you an offset is read; it does not tell you what it does.
This perturbs one input field at a time, re-runs the real function, and reports
which outputs moved and by how much. A field that scales an output linearly is a
coefficient; one that flips a branch is a mode/flag; one that changes nothing is
dead on this path.

This is the dynamic half of the analysis that previously needed a console
emulator -- it only needs the function, not the game.

Usage: python3 tools/probe_fields.py [func_addr]
"""
import importlib.util
import struct
import sys

_spec = importlib.util.spec_from_file_location("ev", "tools/emulate_vehicle.py")
ev = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ev)

# Outputs worth watching: the force accumulator plus the fields the static map
# flagged as written by FUN_0011D460.
WATCH = [0x0F0, 0x0F4, 0x0F8, 0x0B7C, 0x0C3C, 0x1430, 0x14A8,
         0x0008, 0x09F4, 0x0AB4, 0x142C, 0x143C, 0x1168]

# Read-only offsets the static pass could not name, ordered by how heavily the
# function uses them (0x1408 is read 15x, 0x1440 7x).
UNKNOWN_INPUTS = [0x1408, 0x1440, 0x1400, 0x13A4, 0x1404, 0x139C, 0x1390,
                  0x13D0, 0x13DC, 0x1350, 0x1364, 0x1368, 0x136C, 0x1370,
                  0x1374, 0x138C, 0x1398, 0x13AC, 0x13CC, 0x1414, 0x00C0,
                  0x00E0, 0x0100, 0x01B8, 0x01D8, 0x01E8, 0x0004, 0x000C]


def snapshot(after):
    return {o: struct.unpack_from('<f', after, o)[0] for o in WATCH}


PROBE_FUNC = 0x0011D460


def run_with(overrides):
    img = ev.build_vehicle(overrides)
    _, after, err = ev.run(PROBE_FUNC, img)
    if err:
        return None
    return snapshot(after)


def fmt(v):
    if v != v or abs(v) > 1e18:
        return "nan/inf"
    return "%.4g" % v


def main():
    global PROBE_FUNC
    if len(sys.argv) > 1:
        PROBE_FUNC = int(sys.argv[1], 0)

    base = run_with({})
    if base is None:
        raise SystemExit("baseline emulation faulted")

    print("baseline outputs:")
    for o in WATCH:
        if base[o] != 0.0:
            print("   +0x%04X = %s" % (o, fmt(base[o])))

    print("\nprobing %d unknown inputs (x2 perturbation):\n" % len(UNKNOWN_INPUTS))
    print("%-9s %s" % ("input", "outputs that responded"))
    inert = []
    for off in UNKNOWN_INPUTS:
        # Two probes: a scaled value and a distinctly different one, so a field
        # that only gates a branch still shows up.
        moved = {}
        for probe in (2.0, 7.5):
            out = run_with({off: probe})
            if out is None:
                continue
            for o in WATCH:
                if abs(out[o] - base[o]) > 1e-4:
                    moved.setdefault(o, set()).add(round(out[o], 4))
        if not moved:
            inert.append(off)
            continue
        desc = "  ".join("+0x%04X->{%s}" % (o, ",".join(fmt(v) for v in sorted(vals)))
                         for o, vals in sorted(moved.items()))
        print("+0x%04X   %s" % (off, desc))

    print("\ninert on this path (%d): %s"
          % (len(inert), " ".join("0x%04X" % o for o in inert)))


if __name__ == "__main__":
    main()
