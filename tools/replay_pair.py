#!/usr/bin/env python3
"""Replay a REAL in-game car pair through retail's FUN_001121F0 under Unicorn
and diff its contact normal / vn against what the C port produced live.

The harness writes the pair inputs with B3_PAIR_DUMP=<path> (see the
carcol pass in burnout3_full.c); this feeds the identical state to
tools/emulate_carcol.py and prints both answers side by side, so a
"the gate never saw the head-on" report can be settled as either a port
divergence or retail behaviour.

    B3_PAIR_DUMP=/tmp/pairs.txt ./burnout3
    python3 tools/replay_pair.py /tmp/pairs.txt [--racer-only] [--top N]
"""
import argparse
import math
import importlib.util
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

ec = _load("ec", os.path.join(ROOT, "tools", "emulate_carcol.py"))

MPH = 2.23693633

HEAD = re.compile(
    r"pair t=([\d.]+) impact=([\d.-]+) vn=([\d.-]+) "
    r"n=([\d.-]+),([\d.-]+),([\d.-]+) p=([\d.-]+),([\d.-]+),([\d.-]+)")
BODY = re.compile(
    r"\s+([AB]) m=([\d.-]+) type=(\d+) hull=(\S+) "
    r"pos=([\d.-]+),([\d.-]+),([\d.-]+) "
    r"yaw=([\d.-]+) v=([\d.-]+),([\d.-]+),([\d.-]+) "
    r"w=([\d.-]+),([\d.-]+),([\d.-]+)"
    r"(?: crashed=(\d+) drift=(-?\d+) yawin=([\d.eE+-]+) ii=([\d.eE+,-]+))?")


def parse(path):
    pairs, cur = [], None
    for line in open(path, errors="replace"):
        h = HEAD.match(line)
        if h:
            cur = dict(t=float(h.group(1)), impact=float(h.group(2)),
                       vn=float(h.group(3)),
                       n=[float(h.group(i)) for i in (4, 5, 6)],
                       p=[float(h.group(i)) for i in (7, 8, 9)], bodies={})
            pairs.append(cur)
            continue
        b = BODY.match(line)
        if b and cur is not None:
            cur["bodies"][b.group(1)] = dict(
                mass=float(b.group(2)), type=int(b.group(3)),
                hull=b.group(4),
                pos=[float(b.group(i)) for i in (5, 6, 7)],
                yaw=float(b.group(8)),
                vel=[float(b.group(i)) for i in (9, 10, 11)],
                omega=[float(b.group(i)) for i in (12, 13, 14)],
                crashed=int(b.group(15)) if b.group(15) else 0,
                drift=int(b.group(16)) if b.group(16) else 0,
                yaw_input=float(b.group(17)) if b.group(17) else 0.0,
                ii=[float(x) for x in b.group(18).split(",")]
                   if b.group(18) else None)
    return [p for p in pairs if len(p["bodies"]) == 2]


def car(cls, cid, **kw):
    bmax, bmin = ec.bbox(cls, cid)
    st = dict(hull=ec.load_hull(cls, cid), bbmax=bmax, bbmin=bmin,
              _cls=cls, _car=cid)
    st.update(kw)
    return st


def hull_of(tag):
    """"COMP/Car5" -> ("COMP","Car5");  "HEVYCAR23" -> ("HEVY","Car23")."""
    if "/" in tag:
        cls, cid = tag.split("/", 1)
        return cls, cid
    m = re.match(r"([A-Z]+?)CAR(\d+)", tag)
    if m:
        return m.group(1), "Car" + m.group(2)
    return "COMP", "Car1"


def replay(pair):
    """Seed retail with this pair's exact state and run FUN_001121F0."""
    out = []
    for key in ("A", "B"):
        b = pair["bodies"][key]
        st = car(*hull_of(b.get("hull", "COMP/Car1")))
        st["frame"] = ec.frame_from(b["yaw"], tuple(b["pos"]), 0.0)
        st["mass"] = b["mass"]
        st["vel"] = b["vel"]
        st["omega"] = b["omega"]
        st["type"] = b["type"]
        st["drift"] = b.get("drift", 0)
        st["crashed"] = b.get("crashed", 0)
        st["yaw_input"] = b.get("yaw_input", 0.0)
        ii = b.get("ii")
        if ii and len(ii) == 9:
            # the live WORLD inverse inertia, so the impulse solve sees the
            # same tensor the harness used rather than the emulator default
            st["inv_inertia"] = [[ii[0], ii[1], ii[2], 0.0],
                                 [ii[3], ii[4], ii[5], 0.0],
                                 [ii[6], ii[7], ii[8], 0.0]]
        out.append(st)
    s = ec.Session()
    s.seed(0, out[0])
    s.seed(1, out[1])
    # FUN_00111CD0's dispatch: both alive -> FUN_001121F0, else the wreck
    # arm FUN_00113960 (which blends the normal toward vrel and does NOT
    # flatten y -- comparing the wrong arm reads as a false divergence).
    if out[0].get("crashed") or out[1].get("crashed"):
        return s.resolve_wreck()
    return s.resolve_alive()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--racer-only", action="store_true",
                    help="only pairs involving a type-0 racer")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print the contact normal/point for divergences")
    ap.add_argument("--top", type=int, default=8,
                    help="replay the N highest-impact pairs")
    a = ap.parse_args()

    pairs = parse(a.dump)
    if a.racer_only:
        pairs = [p for p in pairs
                 if any(b["type"] == 0 for b in p["bodies"].values())]
    pairs.sort(key=lambda p: -p["impact"])
    pairs = pairs[:a.top]
    print("replaying %d pair(s) through retail FUN_001121F0\n" % len(pairs))
    print("%-8s %-11s %-11s %-10s %s"
          % ("t", "port impact", "retail imp", "n agree", "verdict"))
    bad = 0
    for p in pairs:
        try:
            g = replay(p)
        except Exception as exc:                       # noqa: BLE001
            print("%-8.2f  replay failed: %s" % (p["t"], exc))
            continue
        if not g.get("hit"):
            print("%-8.2f  port hit, RETAIL REPORTS NO CONTACT "
                  "(port vn %.1f)" % (p["t"], p["vn"]))
            bad += 1
            continue
        rn = g.get("normal", [0, 0, 0])
        pn = p["n"]

        def _unit(v):
            n = math.sqrt(sum(x * x for x in v))
            return [x / n for x in v] if n > 1e-9 else v
        # NB the stored normals are NOT unit length (~0.82), so compare
        # directions explicitly rather than dotting the raw vectors
        dot = abs(sum(x * y for x, y in zip(_unit(pn), _unit(rn))))
        rvn = g.get("impact", float("nan"))
        agree = abs(rvn - p["impact"]) < max(50.0, 0.05 * max(rvn, p["impact"]))
        if not agree:
            bad += 1
        print("%-8.2f %-11.0f %-11.0f %-10.3f %s"
              % (p["t"], p["impact"], rvn, dot,
                 "match" if agree and dot > 0.98 else "DIVERGES"))
        if a.verbose and not (agree and dot > 0.98):
            rp = g.get("point", [0, 0, 0])
            print("           port   n=(%7.4f,%7.4f,%7.4f) p=(%9.3f,%8.3f,%9.3f)"
                  % (pn[0], pn[1], pn[2], p["p"][0], p["p"][1], p["p"][2]))
            print("           retail n=(%7.4f,%7.4f,%7.4f) p=(%9.3f,%8.3f,%9.3f)"
                  % (rn[0], rn[1], rn[2], rp[0], rp[1], rp[2]))
    print("\n%d of %d diverge" % (bad, len(pairs)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
