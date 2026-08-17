#!/usr/bin/env python3
"""Decode the directory/section structure of a Burnout 3 Tracks/*/*/Gamedata.bgd.

The .bgd is a BAKED MEMORY IMAGE written by Criterion's track pipeline tool:
C++ objects serialized raw at base VA 0x00320000 (header dword[1] = 0x00320178
= VA of the root object at file offset 0x178).  Non-serialized fields still
hold the *tool's* stack/heap garbage (values like 0x0012Fxxx / 0x7C3Axxxx are
Win32 stack and XP-era DLL addresses) -- only dwords in
[0x320000, 0x320000+filesize) are real baked pointers, and the file's payload
sections reference each other by {offset,size}/index tables, not pointers.

Evidence level per field is marked [C]/[S]/[?] in comments and in RE_BGD.md:
  [C] confirmed by game code (address given) or by an invariant that holds on
      all 40 shipped track files
  [S] strongly supported (structure/geometry evidence, no code path yet)
  [?] guess

Key game code (burnout3.elf VAs, see docs/RE_BGD.md for the chain):
  FUN_0005f0a0/f4a0/f7b0/fb20/fd30  per-mode load-request builders; append
        "Gamedata.bgd" (string ptr 0x003eaf30) to "tracks/<REG>/<Cn>_<Vn>/"
  FUN_001aeaa0 / FUN_001aecc0       base-40 encode / decode of 12-char IDs
        (charset " -/0-9A-Z_", same as vlist.bin car IDs)
  0x3E9CD8 (.data)                  mode-name table "OffSgRcF".."OnSgRcR":
        [Off|On] + [SgRc|LpEl|BtRc|RRge|BrLp|Srvl|Crsh] + [F|R]
        = Offline/Online, game mode, Forward/Reverse route
  DAT_00735524                      -> current event record inside the loaded
        image; readers FUN_001986a0 (+0x3A0/+0x3A4/+0x3A8 medal thresholds,
        gold/silver/bronze), FUN_0017e030 (+0x3AC target count),
        0x1a8d0 stub (+0x3B8)
  FUN_001a13f0                      traffic manager: walks {ptr+0x54,count
        +0x58} list of 0x18-byte records with a packed car ID at +0x00 and
        hands each to FUN_001a4260, which appends ".btv" and loads the traffic
        vehicle via the .bgv relinker FUN_000310f0
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import struct, sys, os, glob

BASE = 0x320000
CS = " -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_"

DEFAULT = (game_path('Tracks/AS/C1_V1/Gamedata.bgd'))


def b40(v):
    """FUN_001aecc0: base-40 decode, LSB char first, then reversed. [C]"""
    out = []
    for _ in range(12):
        out.append(CS[v % 40])
        v //= 40
    return ''.join(reversed(out)).rstrip(' ')


def u32(d, o): return struct.unpack_from('<I', d, o)[0]
def u64(d, o): return struct.unpack_from('<Q', d, o)[0]
def f32(d, o): return struct.unpack_from('<f', d, o)[0]
def f3(d, o): return struct.unpack_from('<fff', d, o)


def plausible_id(s):
    """A decoded ID that looks intentional, not stale tool memory."""
    return s and all(c in CS for c in s) and (' ' not in s.strip())


class BGD:
    def __init__(self, path):
        self.path = path
        self.d = open(path, 'rb').read()
        self.N = len(self.d)
        assert u32(self.d, 0) == 9, "version dword != 9"
        assert u32(self.d, 4) == BASE + 0x178, "root VA != 0x320178"
        self.parse()

    # ------------------------------------------------------------------
    def parse(self):
        d, N = self.d, self.N

        # 18 event/mode ID slots at +0x08 [C: count matches the 18-entry
        # offset table at +0x198; unused slots hold stale tool memory]
        self.ids = [u64(d, 0x08 + i * 8) for i in range(18)]
        self.id_names = [b40(v) for v in self.ids]

        # root object at 0x178; +0x20 (file 0x198): 18 u32 offsets of the
        # 0x800-byte per-event PARAM records [C: verified -- records hold the
        # medal thresholds the game reads via DAT_00735524]
        self.rec_off = [u32(d, 0x198 + i * 4) for i in range(18)]

        # section {size,offset} pairs at 0x3BB8 [C: on all 40 shipped files
        # the last pair ends exactly at EOF and the pairs tile the tail]
        t = struct.unpack_from('<9I', d, 0x3BB8)
        self.sections = [(t[1 + 2 * i] , t[2 + 2 * i]) for i in range(4)]
        (self.net_size, self.net_off) = self.sections[3]
        (self.ridx_size, self.ridx_off) = self.sections[2]
        assert self.net_off + self.net_size == N, "network section != EOF tile"

        # ---- road-network section header [C-structure] -------------------
        F = self.net_off
        h = struct.unpack_from('<8I', d, F)
        self.nodes = h[2]          # e.g. 910 (C1), 925 (C2) ... [C]
        self.nsec = h[4]           # checkpoint/route sections, 7-8 [S]
        self.idx_off = F + h[5]    # index-table directory [C: verified]
        # 8 route-section records at F+0x20: {u32 start_node, f32 length_m,
        # f32 a, f32 b} + u32 terminator start [S]
        self.route_secs = []
        o = F + 0x20
        for i in range(self.nsec):
            s = struct.unpack_from('<Ifff', d, o + i * 0x10)
            self.route_secs.append(s)
        self.sec_term = u32(d, o + self.nsec * 0x10)

        # node-paired arrays: the two <nodes>-point loops directly before the
        # index directory [C: arrlo = idx_off - 2*nodes*16 lands exactly on a
        # loop start in every file checked]
        self.routeA_off = self.idx_off - 2 * self.nodes * 16
        self.routeB_off = self.idx_off - self.nodes * 16

        # boundary strip right after the header [S: 2-strand interleave
        # verified visually build/bgd_walls.png]
        self.strip_off = F + 0x170

        # the forward/reverse drive lines fill the gap up to routeA;
        # boundary between them located by the seam (single discontinuity)
        self.lines = self._split_lines()

        # ---- per-event records ------------------------------------------
        self.events = []
        for i in range(18):
            ro = self.rec_off[i]
            name = self.id_names[i]
            ev = {'slot': i, 'id': name, 'param_off': ro,
                  'spatial_off': 0xC000 + i * 0x800}
            # unused slots keep stale tool memory in BOTH tables
            if not (0x3000 <= ro <= 0xB800 and ro % 0x800 == 0):
                ev['stale'] = True
                ev.update(car='', gold=0, silver=0, bronze=0,
                          target=0, laps=0, grid=[])
                self.events.append(ev)
                continue
            ev['stale'] = False
            # car id is stored as two 8-byte base-40 halves [S]
            ev['car'] = (b40(u64(d, ro)) + b40(u64(d, ro + 8))).strip()
            ev['gold'] = f32(d, ro + 0x3A0)     # [C] FUN_001986a0
            ev['silver'] = f32(d, ro + 0x3A4)   # [C] FUN_001986a0
            ev['bronze'] = f32(d, ro + 0x3A8)   # [C] FUN_001986a0
            ev['target'] = u32(d, ro + 0x3AC)   # [C] FUN_0017e030 (remaining
                                                # = target - takedowns)
            ev['laps'] = u32(d, ro + 0x3B8)     # [S] 3 for races, 1/0 else
            ev['grid'] = self._grid(ev['spatial_off'])
            self.events.append(ev)

        # ---- per-mode blocks at 0x18000, sizes at 0x660 [S] --------------
        self.mode_blocks = self._mode_blocks()

    # ------------------------------------------------------------------
    def _split_lines(self):
        """The reverse and forward drive lines sit contiguously, ending
        exactly at routeA (the node arrays).  Walking back from routeA, each
        line is recovered as the shortest suffix that closes into a loop
        (first point ~ last point) at a >60u seam."""
        d = self.d

        def pt(o): return f3(d, o)

        def prev_loop(hi):
            """largest closed loop ending at hi, delimited by a seam"""
            last = pt(hi - 16)
            o = hi - 16
            prev = None
            while o >= self.strip_off:
                p = pt(o)
                if prev is not None:
                    dd = ((p[0] - prev[0]) ** 2 + (p[2] - prev[2]) ** 2) ** .5
                    if dd > 60.0:
                        # seam between o and prev record: candidate start=o+16
                        start = o + 16
                        s = pt(start)
                        close = ((s[0] - last[0]) ** 2
                                 + (s[2] - last[2]) ** 2) ** .5
                        if close < 60.0:
                            return (start, (hi - start) // 16)
                prev = p
                o -= 16
            return None

        out = []
        hi = self.routeA_off
        for _ in range(2):
            got = prev_loop(hi)
            if not got:
                break
            out.append(got)
            hi = got[0]
        out.sort()
        return out  # [(off,count) reverse-dir line, (off,count) forward line]

    def _grid(self, so):
        """Start-grid records in the spatial block: 0x50 bytes =
        {3x[f32 x,y,z,pad] rotation rows, [f32 x,y,z,0] position,
         u32 node_index, 12 pad} [S: matrix rows are unit vectors]"""
        d = self.d
        out = []
        for i in range(8):
            o = so + i * 0x50
            r0 = f3(d, o)
            pos = f3(d, o + 0x30)
            node = u32(d, o + 0x40)
            n = (r0[0] ** 2 + r0[1] ** 2 + r0[2] ** 2) ** .5
            if not (0.99 < n < 1.01):
                break
            out.append({'pos': pos, 'fwd': f3(d, o), 'node': node})
        return out

    def _mode_blocks(self):
        """Per-mode blocks start at 0x18000; dword size table at 0x660.
        Sizes are read until they tile exactly to the route-index section.
        Each block carries its own traffic-vehicle set: 0x18-byte records
        {u64 base-40 car id, ...} (walked by FUN_001a13f0 -> .btv loads via
        FUN_001a4260)."""
        d = self.d
        sizes = []
        o = 0x660
        st = 0x18000
        while len(sizes) < 48:
            s = u32(d, o)
            if s == 0 or s > 0x80000 or st + s > self.ridx_off + 0x10000:
                break
            sizes.append(s)
            st += s
            o += 4
            if st == self.ridx_off:
                break
        blocks = []
        st = 0x18000
        ok = (sum(sizes) + 0x18000 == self.ridx_off)
        for s in sizes:
            blk = {'off': st, 'size': s, 'traffic': self._traffic_set(st, s)}
            blocks.append(blk)
            st += s
        return {'valid': ok, 'blocks': blocks}

    def _traffic_set(self, lo, size):
        """Scan a mode block for its run of 0x18-stride packed car IDs."""
        d = self.d
        best = []
        run = []
        o = lo
        end = min(lo + size, self.N - 8)
        while o < end:
            s = b40(u64(d, o))
            if s and 3 <= len(s) <= 12 and ('CAR' in s):
                if run and o - run[-1][0] != 0x18:
                    if len(run) > len(best):
                        best = run
                    run = []
                run.append((o, s))
                o += 0x18
            else:
                if len(run) > len(best):
                    best = run
                run = []
                o += 8
        if len(run) > len(best):
            best = run
        return best

    # ------------------------------------------------------------------
    def report(self):
        print("== %s  (%#x bytes)" % (self.path, self.N))
        print("root @0x178 (VA 0x320178), 18 event slots:")
        for ev in self.events:
            if ev['stale']:
                print("  [%2d] (stale slot)" % ev['slot'])
                continue
            g = ev['grid']
            print("  [%2d] %-14s param@%05X spatial@%05X car=%-10s "
                  "medals g/s/b=%g/%g/%g target=%d laps=%d grid=%d" % (
                      ev['slot'], ev['id'],
                      ev['param_off'], ev['spatial_off'], ev['car'] or '-',
                      ev['gold'], ev['silver'], ev['bronze'],
                      ev['target'], ev['laps'], len(g)))
        print("sections (size,offset) @0x3BB8: %s" %
              ["(%#x,%#x)" % s for s in self.sections])
        print("road network @%#x: %d nodes, %d route sections, index dir @%#x"
              % (self.net_off, self.nodes, self.nsec, self.idx_off))
        for i, (start, ln, a, b) in enumerate(self.route_secs):
            print("  section %d: start node %4d  length %7.1f  (%g, %g)"
                  % (i, start, ln, a, b))
        print("  boundary strip @%#x (2 interleaved strands)" % self.strip_off)
        for off, cnt in self.lines:
            print("  drive line   @%#x x%d" % (off, cnt))
        print("  route loop A @%#x x%d (outer)" % (self.routeA_off, self.nodes))
        print("  route loop B @%#x x%d (inner)" % (self.routeB_off, self.nodes))
        mb = self.mode_blocks
        print("mode blocks @0x18000 (size table @0x660, tiles=%s): %d blocks"
              % (mb['valid'], len(mb['blocks'])))
        for i, blk in enumerate(mb['blocks']):
            ts = blk['traffic']
            cars = [s for _, s in ts]
            print("  block %2d @%06X +%06X traffic x%-2d %s" % (
                i, blk['off'], blk['size'], len(cars),
                (', '.join(cars[:6]) + ('...' if len(cars) > 6 else ''))))


def main():
    paths = sys.argv[1:] or [DEFAULT]
    if paths == ['--all']:
        tracks = os.path.dirname(os.path.dirname(os.path.dirname(DEFAULT)))
        paths = sorted(glob.glob(tracks + "/*/*/Gamedata.bgd"))
    for p in paths:
        BGD(p).report()
        print()


if __name__ == '__main__':
    main()
