#!/usr/bin/env python3
"""Gamedata.bgd reader + track path extraction -- track-agnostic.

This file is BOTH the shared Gamedata.bgd library (imported by
extract_start_grid.py and extract_traffic.py) and the tool that emits the
harness's path data:

    src/burnout3_track_paths.h          (compiled-in, legacy shape)
    build/tracks/<ID>/route.bin         (runtime-loadable, see FORMAT below)

Run with  --track US_C3_V1  /  B3_TRACK=US_C3_V1, ids from
tools/extract_tlist.py.  There are NO per-track constants below; every
address is read out of the file (or is a documented cross-corpus invariant
with the per-track check printed).

=========================================================== THE .bgd PARSER ==
Superseding the old "baked memory image with a root object at 0x178" model:
the game never maps the file and never follows the two header pointers.
`FUN_0018B250` is a 15-state streaming reader (loader object at 0x00734FC0,
`lea edi,[ebp+0x1265c0]` @0x001AA175).  It reads file[0..0x800], finds the
event slot by comparing the packed 64-bit event id, then locates EVERYTHING
ELSE through {size,offset} pairs inside that event's own 0x800-byte param
record.  All offsets below are FILE offsets.                            [C]

    +0x008  u64[n]  event ids, base-40 packed        (0x0018B398)
    +0x198  u32[n]  event param-record file offsets  (0x0018B3C5)
    +0x260  u32     event COUNT                      (0x0018B37A)
        -- observed 6..19 across the 36 shipped files.  The old fixed 18
           both reads stale slots and DROPS a real event on EU/C1_V1 and
           US/C2_V1 (19 events each).

  event param record P (0x800 bytes, read at 0x0018B3EB):
    P+0x3B8  u32  lap count                          (RE_BGD 3)
    P+0x3BC/0x3C0  size/offset  event SPATIAL record (0x0018B4D6/0x0018B4A1)
                   -- always 0x800; SHARED between events (US/C1_V1: 13
                      events -> 1 record).  The old 0xC000+slot*0x800 is
                      wrong in general.
    P+0x3C4/0x3C8  size/offset  mode block "TDESC"   (0x0018B5BB/0x0018B569)
                   -- the old 0x18000 + size-table-at-0x660 tiles on 1 of
                      36 files (AS/C1_V1, the previous calibration track).
    P+0x3CC/0x3D0  size/offset  route-index section  (0x0018B643/0x0018B67B)
    P+0x3D4/0x3D8  size/offset  road-network section (0x0018B71D/0x0018B755)
    (the old "section table @0x3BB8" is exactly event record[1] + 0x3B8)

  event SPATIAL record S (relocated at 0x0018B508):
    S+0x000,0x050,0x0A0,0x0F0,0x140,0x190   6 start-grid slots, 0x50 each:
        +0x00 f32[4] right  +0x10 f32[4] up  +0x20 f32[4] at (forward)
        +0x30 f32[4] position            +0x40 u32 node index
    S+0x1E0/0x200 (count S+0x3EC), S+0x220/0x240 (count S+0x3F0),
    S+0x260.. (count S+0x3F4): 0x20-byte anchors {pos[3], radius, dir[3], 0}
        -- checkpoints / finish
    S+0x3E0 -> pickups (count S+0x3F8), S+0x3E4 -> (count S+0x3FC)

  mode block / TDESC B (relocated by FUN_00158B70):
    B+0x00,0x08,0x10,0x18,0x20   u64 special-traffic ids, gated by byte B+0xB5
    B+0x54/0x58, 0x60/0x64, 0x6C/0x70, 0x78/0x7C, 0x84/0x88, 0x90/0x94
                                 six {ptr,count} traffic-vehicle lists,
                                 0x18-byte records with a packed id at +0x00
    B+0xAC/0xB0                  {ptr,count} 0x20-stride {pos[3],w,dir[3],w}
                                 table (spawn/entry points -- location [C],
                                 semantics [S]: no consumer located)
    B+0x4C/0x50                  {ptr,count} 0x58-stride traffic schedules.
                                 `FUN_00158D10` relocates every row.  Stage 2
                                 reads row+0x04 through parallel row+0x08
                                 manager and +0x0C field byte maps, count
                                 row+0x40.
                                 State 6 reads row+0x38/+0x3C as 0x0C request
                                 descriptors {record ptr, scalar, count:u8,
                                 ?, manager:u8, slot:u8}; it installs each at
                                 manager+0x30+slot*4.  Records are 0x20 bytes;
                                 byte +0x1B is the type-3 designation flag.

  road-network section N (relocated by FUN_00158DE0, four pointers):
    N+0x04 u32 index-directory ROW COUNT       N+0x08 u32 node count
    N+0x10 u32 route-section count
    N+0x14 ptr index directory   N+0x18/N+0x1C ptr tail tables
    N+0x20 ptr {x,y,z,0} array (N+0xB0 on every file checked)
    N+0x24 route-section records, stride 0x10 {u32 start_node, f32 length, ..}

  The index directory is itself relocated row-relative by FUN_00158DE0.
  Each 0x10 row is {pair_rel, edge_rel, link_rel, flags:u16|node_count:u16}:
    row+pair_rel -> node_count x {u16 point_a, u16 point_b}
    row+link_rel -> node_count x 10-byte records where forward is
                    {u8 section@+4, u16 node@+6} and reverse is
                    {u8 section@+5, u16 node@+8}.
  The point indices address the N+0x20 16-byte pool.  FUN_00175B10 walks
  those exact forward/reverse fields; the unused edge_rel table remains
  undecoded.

============================================ THE GEOMETRY POOLS ARE STILL [S] ==
No code path exposes the boundary strip or the drive lines, so those are
recovered from the file's own geometry, with the criteria below and a
per-track report.  None of the thresholds is a track constant: each is a
multiple of a statistic measured on the track's own data.

  * CORRIDOR STRIP: the one INTERLEAVED array in the network section
    (record 2k = strand A, 2k+1 = strand B).  Located by the interleave
    signature -- consecutive records span the road while the pair MIDPOINT
    barely advances, so |dmid|/|dstep| is ~0.05..0.25 inside it and ~1 in
    every plain polyline -- as the longest run of that ratio below 0.5.
    It starts at N+0x170 on AS/C1_V1 but 292 records later on US/C3_V1, so
    the old +0x170 constant was a per-track calibration.  This midline is
    the harness's driving route.
  * DRIVE LINES: everything in the network section either side of the strip
    is split into arrays at steps > 6 x the region's own median point
    spacing; a run whose length exceeds 1.5 laps and whose point count is an
    exact multiple of the node count is two node-paired arrays that happen
    to be geometrically continuous, and is split back at that count.  A run
    is a "lap loop" when its polyline length is 0.7..1.5 x the lap length
    (from the route-section records) AND it closes on itself within 0.08 of
    a lap -- which is what separates a real closed line from an open
    sub-path of similar length.
  * THE DRIVING ROUTE is not the same array on every track, and which one it
    is gets MEASURED, not assumed.  Every candidate (the corridor midline and
    each lap loop) is scored by
      (a) how many of its points have NO road-like collision triangle within
          6 m below them, using the game's own collision world out of
          build/tracks/<ID>/collision.bin (run extract_collision.py first;
          without it this term is skipped), and
      (b) the mean |lateral| offset of the six start-grid slots from it.
    Result on the two tracks driven so far:
      AS/C1_V1  corridor midline 0/868 unsupported, grid 2.1 m off  -> ROUTE
                drive line @0x1ABF20 273/1029 unsupported (it is the loop the
                harness already noted "cuts through fenced roundabout
                geometry"), @0x1A8540 83/926
      US/C3_V1  corridor midline 534/1012 unsupported, grid 46.2 m off
                drive line @0x0C0930 0/1013 unsupported, grid 5.8 m -> ROUTE
                (Silver Lake's strip is a 50 m right-of-way envelope, not the
                carriageway)
  * ONCOMING line = the nearest lap loop that is at least one grid-column
    away from the route.  The six start slots form two columns one lane
    apart, so their lateral spread is the track's own lane scale -- that is
    what separates the other carriageway from a near-duplicate of the route
    (US/C3_V1 has a loop 3.0 m off the route and the real oncoming lane at
    14.4 m).  RACE line = the route itself when the route is a drive line,
    else the nearest remaining loop (AS/C1_V1: the 1029-pt loop, which is all
    the harness uses it for -- a <=5 m lane nudge).
  * WALL STRANDS are always emitted so that (A[i]+B[i])/2 IS the route: the
    real de-interleaved strip when the corridor won, otherwise a SYNTHESISED
    pair half the route-to-oncoming distance either side (marked as such in
    the generated header).  route.bin carries the raw strip either way.
  * All of these are emitted ORIENTED: ascending index runs WITH the race
    direction for the route and race line, AGAINST it for the oncoming line,
    where "race direction" is start-grid slot 0's forward vector.  The pools
    are authored once and shared by the V1/V2 variants, so the sign genuinely
    comes from the grid, not the file order (RE_BGD 3).

================================================================== route.bin ==
Little-endian.  All points in the harness's GL space (z negated once on
load, RE_NOTES 12), so a loader can memcpy them.

    +0x00  char[4] 'B3RT'      +0x04  u32 version = 3
    +0x08  u32 wall_count      (strand A and strand B both have this many)
    +0x0C  u32 centerline_count(the race line)
    +0x10  u32 oncoming_count
    +0x14  u32 route_count     (== wall_count)
    +0x18  u32 route_start     (index into route/wall nearest grid slot 0)
    +0x1C  f32 lap_length      (metres, sum of the route-section records)
    +0x20  u32 flags           bit0 = arrays were reversed to match the race
                               direction; bit1 = the route is a drive line and
                               the wall strands are synthesised around it
    +0x24  u32 strip_pairs     (raw boundary strip pair count, may differ)
    +0x28  f32 wall_a[wall_count][3]
           f32 wall_b[wall_count][3]
           f32 centerline[centerline_count][3]
           f32 oncoming[oncoming_count][3]
           f32 route[route_count][3]
           f32 strip_a[strip_pairs][3]      raw de-interleaved boundary strip
           f32 strip_b[strip_pairs][3]
           u32 nav_point_count, nav_section_count, nav_pair_count,
               nav_link_count, nav_plan_count
           f32 nav_points[nav_point_count][3]             (GL space)
           {u32 pair_base, u32 link_base, u16 node_count, u16 flags}
               nav_sections[nav_section_count]
           {u16 point_a, u16 point_b} nav_pairs[nav_pair_count]
               (each section includes one terminal look-ahead guard pair)
           {u16 anchor, u16 link_data, u8 forward_section, u8 reverse_section,
               u16 forward_node, u16 reverse_node} nav_links[nav_link_count]
           {u16 node_a, u16 node_b, u16 node_c, u16 speed,
               u8 section, u8 byte9, u8 flags, u8 byte11} nav_plans[nav_plan_count]
"""
import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_tlist as tl                                   # noqa: E402

CS = tl.CS
b40 = tl.b40

# The single-race-forward event.  Mode ids are built by the game's mode-name
# table at .data 0x3E9CD8: [Off|On] + [SgRc|LpEl|BtRc|RRge|BrLp|Srvl|Crsh] +
# [F|R]  (RE_BGD 3).  Overridable with --event.
DEFAULT_EVENT = "OFFSGRCF"


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def u64(d, o):
    return struct.unpack_from('<Q', d, o)[0]


def f32(d, o):
    return struct.unpack_from('<f', d, o)[0]


def f3(d, o):
    return struct.unpack_from('<fff', d, o)


def dist2(a, b):
    return (a[0] - b[0]) ** 2 + (a[2] - b[2]) ** 2


def dist(a, b):
    return math.sqrt(dist2(a, b))


def median(v):
    s = sorted(v)
    return s[len(s) // 2] if s else 0.0


# ==========================================================================
class BGD(object):
    """Structural reader for one Tracks/<REG>/<Cn>_<Vn>/Gamedata.bgd."""

    def __init__(self, path):
        self.path = path
        self.d = open(path, 'rb').read()
        self.N = len(self.d)
        assert u32(self.d, 0) == 9, "Gamedata.bgd version != 9"
        d = self.d
        self.count = u32(d, 0x260)                            # [C] 0x0018B37A
        assert 0 < self.count <= 64, "event count %d" % self.count
        self.events = []
        for i in range(self.count):
            eid = b40(u64(d, 0x08 + i * 8))                   # [C] 0x0018B398
            off = u32(d, 0x198 + i * 4)                       # [C] 0x0018B3C5
            if not (0 < off < self.N - 0x800):
                continue
            ev = dict(index=i, id=eid, param=off,
                      laps=u32(d, off + 0x3B8),
                      spatial=(u32(d, off + 0x3BC), u32(d, off + 0x3C0)),
                      tdesc=(u32(d, off + 0x3C4), u32(d, off + 0x3C8)),
                      ridx=(u32(d, off + 0x3CC), u32(d, off + 0x3D0)),
                      net=(u32(d, off + 0x3D4), u32(d, off + 0x3D8)))
            self.events.append(ev)

    # ---------------------------------------------------------------- events
    def event(self, name=DEFAULT_EVENT):
        """The named event, else the first with a sane network section."""
        for ev in self.events:
            if ev['id'] == name and self._sane(ev):
                return ev
        for ev in self.events:
            if self._sane(ev):
                return ev
        raise SystemExit("%s: no event with a usable road-network section"
                         % self.path)

    def _sane(self, ev):
        ns, no = ev['net']
        rs, ro = ev['ridx']
        ss, so = ev['spatial']
        return (ns and no + ns == self.N and ro + rs == no
                and ss == 0x800 and 0 < so < self.N - 0x800)

    # ------------------------------------------------------------ start grid
    def grid(self, ev):
        """The 6 start-grid slots {pos, fwd, node} in RAW game space. [C]"""
        so = ev['spatial'][1]
        out = []
        for k in range(6):
            o = so + k * 0x50
            at = f3(self.d, o + 0x20)
            ps = f3(self.d, o + 0x30)
            node = u32(self.d, o + 0x40)
            n = math.sqrt(at[0] ** 2 + at[1] ** 2 + at[2] ** 2)
            assert 0.99 < n < 1.01, "grid slot %d forward is not a unit "
            out.append(dict(pos=ps, fwd=at, node=node))
        return out

    # -------------------------------------------------------- road network
    def network(self, ev):
        n = ev['net'][1]
        d = self.d
        rows = u32(d, n + 0x04)
        nodes = u32(d, n + 0x08)
        nsec = u32(d, n + 0x10)
        idx = n + u32(d, n + 0x14)
        # route-section records, stride 0x10 {u32 start_node, f32 length_m,
        # f32, f32} -- the first record's dword shares N+0x20 with the
        # relocated pointer.  Lap length = sum of the lengths.
        secs = [struct.unpack_from('<Ifff', d, n + 0x20 + i * 0x10)
                for i in range(nsec)]
        lap = sum(s[1] for s in secs)
        return dict(base=n, rows=rows, nodes=nodes, nsec=nsec, idx=idx,
                    secs=secs, lap=lap,
                    ptr20=n + u32(d, n + 0x20))

    def nav_graph(self, ev):
        """Return the retail section/node graph used by FUN_00175B10. [C]"""
        net = self.network(ev)
        d = self.d
        rows = []
        for section in range(net['rows']):
            row = net['idx'] + section * 0x10
            pair_rel, edge_rel, link_rel, count_flags = struct.unpack_from(
                '<IIII', d, row)
            node_count = count_flags & 0xffff
            rows.append(dict(section=section, node_count=node_count,
                             flags=count_flags >> 16,
                             pairs=row + pair_rel, edges=row + edge_rel,
                             links=row + link_rel))
        point_count = (net['idx'] - net['ptr20']) // 16
        return dict(points=net['ptr20'], point_count=point_count, rows=rows)

    def nav_node(self, graph, section, node):
        """Decode one retail node's point pair and bidirectional links. [C]"""
        row = graph['rows'][section]
        if node < 0 or node >= row['node_count']:
            raise IndexError('nav node %d outside section %d' % (node, section))
        point_a, point_b = struct.unpack_from('<HH', self.d,
                                              row['pairs'] + node * 4)
        link = row['links'] + node * 10
        forward_node, reverse_node = struct.unpack_from('<HH', self.d,
                                                         link + 6)
        return dict(point_a=point_a, point_b=point_b,
                    node_flags=self.d[link + 3] & 7,
                    forward_section=self.d[link + 4], forward_node=forward_node,
                    reverse_section=self.d[link + 5], reverse_node=reverse_node)

    def nav_nearest(self, graph, pos):
        """Match FUN_00174CF0's nearest four-point node-midpoint search. [C]"""
        best = None
        for row in graph['rows']:
            for node in range(row['node_count']):
                point_ids = struct.unpack_from('<HHHH', self.d,
                                               row['pairs'] + node * 4)
                center = [0.0, 0.0, 0.0]
                for point_id in point_ids:
                    point = struct.unpack_from('<3f', self.d,
                                               graph['points'] + point_id * 16)
                    center[0] += point[0] * 0.25
                    center[1] += point[1] * 0.25
                    center[2] += point[2] * 0.25
                dx, dy, dz = center[0] - pos[0], center[1] - pos[1], center[2] - pos[2]
                distance_sq = dx * dx + dy * dy + dz * dz
                if best is None or distance_sq < best['distance_sq']:
                    best = dict(section=row['section'], node=node,
                                distance_sq=distance_sq, point=center)
        return best

    def nav_step_flags(self, graph, section, node, pos):
        """Match FUN_00174050's forward/reverse ribbon classification. [C]"""
        row = graph['rows'][section]
        if node < 0 or node >= row['node_count']:
            raise IndexError('nav node %d outside section %d' % (node, section))
        point_ids = struct.unpack_from('<HHHH', self.d, row['pairs'] + node * 4)
        points = [struct.unpack_from('<3f', self.d,
                                     graph['points'] + point_id * 16)
                  for point_id in point_ids]

        def dot_edge(point, start, end):
            dx, dz = point[0] - start[0], point[2] - start[2]
            ex, ez = end[0] - start[0], end[2] - start[2]
            return -dx * ez + dz * ex

        flags = 0
        if point_ids[2] != point_ids[3] and dot_edge(pos, points[2], points[3]) < 0.0:
            flags |= 4
        if dot_edge(pos, points[0], points[2]) < 0.0:
            flags |= 1
        if dot_edge(pos, points[1], points[3]) > 0.0:
            flags |= 2
        if point_ids[0] != point_ids[1] and dot_edge(pos, points[0], points[1]) > 0.0:
            flags |= 8
        return flags

    def nav_walk(self, graph, section, node, pos):
        """Match FUN_00175570's bounded section/node update. [C]"""
        def local_step(delta):
            nonlocal section, node
            row = graph['rows'][section]
            candidate = node + delta
            if 0 <= candidate < row['node_count']:
                node = candidate
                return True
            if row['flags'] & 0xff:
                node = candidate % row['node_count']
                return True
            boundary = self.nav_node(graph, section, node)
            if boundary['forward_section'] != 0xff:
                section, node = boundary['forward_section'], boundary['forward_node']
                return True
            if boundary['reverse_section'] != 0xff:
                section, node = boundary['reverse_section'], boundary['reverse_node']
                return True
            return False

        def select_link(reverse):
            nonlocal section, node
            boundary = self.nav_node(graph, section, node)
            key = 'reverse' if reverse else 'forward'
            if boundary[key + '_section'] == 0xff:
                return False
            section, node = boundary[key + '_section'], boundary[key + '_node']
            return True

        state = 0x10
        steps = 0
        while steps < 127:
            flags = self.nav_step_flags(graph, section, node, pos)
            if flags == 0:
                break
            steps += 1
            row = graph['rows'][section]
            if flags & 4:
                if state == 8:
                    break
                if not local_step(1):
                    break
                state = 4
            elif flags & 8:
                if state == 4:
                    break
                if not local_step(-1):
                    break
                state = 8
            elif flags & 1:
                if state == 2:
                    break
                if not select_link(False):
                    break
                state = 1
            elif flags & 2:
                if state == 1:
                    break
                if not select_link(True):
                    break
                state = 2
            else:
                break
        return dict(section=section, node=node, steps=steps,
                    flags=self.nav_step_flags(graph, section, node, pos))

    def nav_forward(self, graph, section, node):
        """Match FUN_00174740's four-node forward finite difference. [C]"""
        row = graph['rows'][section]
        if node < 0 or node >= row['node_count']:
            raise IndexError('nav node %d outside section %d' % (node, section))

        def pair(index):
            return struct.unpack_from('<HH', self.d, row['pairs'] + index * 4)

        pair0, pair1 = pair(node), pair(node + 1)
        if node == row['node_count'] - 2:
            pair2 = pair(node + 2)
            pair3 = pair(node + 2) if not (row['flags'] & 0xff) else pair(1)
        elif node == row['node_count'] - 1:
            if row['flags'] & 0xff:
                pair2, pair3 = pair(1), pair(2)
            else:
                pair2 = pair3 = pair(row['node_count'])
        else:
            pair2, pair3 = pair(node + 2), pair(node + 3)

        past = [0.0, 0.0, 0.0]
        future = [0.0, 0.0, 0.0]
        for point_id in pair0 + pair1:
            point = struct.unpack_from('<3f', self.d,
                                       graph['points'] + point_id * 16)
            for axis in range(3):
                past[axis] += point[axis]
        for point_id in pair2 + pair3:
            point = struct.unpack_from('<3f', self.d,
                                       graph['points'] + point_id * 16)
            for axis in range(3):
                future[axis] += point[axis]
        return tuple(future[axis] - past[axis] for axis in range(3))

    def nav_plans(self, ev):
        """Return FUN_001772A0's 12-byte look-ahead planning records. [C]"""
        net = self.network(ev)
        count = u32(self.d, net['base'] + 0x0c)
        table = net['base'] + u32(self.d, net['base'] + 0x1c)
        plans = []
        for index in range(count):
            node_a, node_b, node_c, speed = struct.unpack_from(
                '<HHHH', self.d, table + index * 12)
            plans.append(dict(node_a=node_a, node_b=node_b, node_c=node_c,
                              speed=speed, section=self.d[table + index * 12 + 8],
                              byte9=self.d[table + index * 12 + 9],
                              flags=self.d[table + index * 12 + 10],
                              byte11=self.d[table + index * 12 + 11]))
        return plans

    # ----------------------------------------------------------- traffic paths
    def traffic_paths(self, ev):
        """Return FUN_0019FFA0's relocated traffic-path source. [C]

        The active event's RIDX image begins with the header relocated by
        FUN_00158CC0: {u32 descriptor_rows, u32 point_base, u32 path_count}.
        It owns path_count 0x14-byte descriptors.  Relocation fixes the first
        three relative pointers and supplies point_base at descriptor+0x0c:
        {u32 pair_rows, u32 distances, u32 aux, u32 point_base, u32 count}.
        FUN_0019FFA0 reads the pair rows and shared 16-byte point pool, while
        FUN_0019F1C0 reads the cumulative-distance rows.
        """
        size, base = ev['ridx']
        d = self.d
        if not (size >= 12 and 0 < base <= self.N - size):
            raise ValueError('invalid traffic RIDX image')
        rows_rel, points_rel, count = struct.unpack_from('<III', d, base)
        if not (0 < count <= 255 and rows_rel < size and points_rel < size
                and rows_rel + count * 0x14 <= size):
            raise ValueError('invalid traffic path header')
        points = base + points_rel
        point_count = (size - points_rel) // 16
        paths = []
        for index in range(count):
            row = base + rows_rel + index * 0x14
            pairs_rel, dist_rel, aux_rel, _reserved, path_count = struct.unpack_from(
                '<IIIII', d, row)
            if not (2 <= path_count <= point_count
                    and pairs_rel + path_count * 4 <= size
                    and dist_rel + path_count * 8 <= size
                    and aux_rel < size):
                raise ValueError('invalid traffic path %d' % index)
            pairs = []
            distances = []
            aux = d[base + aux_rel:base + aux_rel + path_count * 0x12]
            if len(aux) != path_count * 0x12:
                raise ValueError('invalid traffic path %d link rows' % index)
            for node in range(path_count):
                point_a, point_b = struct.unpack_from('<HH', d,
                                                       base + pairs_rel + node * 4)
                if point_a >= point_count or point_b >= point_count:
                    raise ValueError('traffic path %d point id' % index)
                pairs.append((point_a, point_b))
                distances.append(struct.unpack_from('<ff', d,
                                                    base + dist_rel + node * 8))
            paths.append(dict(index=index, pairs=pairs, distances=distances,
                              aux=aux, points=points,
                              point_count=point_count))
        return dict(base=base, size=size, points=points,
                    point_count=point_count, paths=paths)

    def traffic_path_centerline(self, paths, path_index):
        """Return one RIDX path's pair midpoints in raw game space. [C]"""
        path = paths['paths'][path_index]
        out = []
        for point_a, point_b in path['pairs']:
            a = f3(self.d, paths['points'] + point_a * 16)
            b = f3(self.d, paths['points'] + point_b * 16)
            out.append(((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5,
                        (a[2] + b[2]) * 0.5))
        return out

    # ----------------------------------------------------------- TDESC block
    TRAFFIC_LISTS = (0x54, 0x60, 0x6C, 0x78, 0x84, 0x90)
    SCHEDULE_PTR = 0x4C
    SCHEDULE_COUNT = 0x50
    POOL_WINDOW_PTR = 0xA4
    POOL_WINDOW_COUNT = 0xA8
    SPAWN_PTR = 0xAC

    # FUN_001A5E30's jump table @0x001A5F10 maps the runtime CLASS code onto
    # these six list offsets (index = class - 1, `ja 0xa` -> default):
    #   1 -> +0x54, 2 -> +0x60, 3 -> +0x78, 4 -> +0x84, 5 -> +0x6C, 0xB -> +0x90
    # so the extractor's slot order (0..5) is NOT the class order.  [C]
    LIST_CLASS = (1, 2, 5, 3, 4, 0x0B)
    MANAGER_RECORD_PTR = 0x3C
    MANAGER_RECORD_COUNT = 0x40

    def tdesc(self, ev):
        """Traffic lists, special ids and the spawn table of one event. [C]"""
        d = self.d
        size, base = ev['tdesc']
        lists = []
        list_records = []
        for p in self.TRAFFIC_LISTS:
            rel, cnt = u32(d, base + p), u32(d, base + p + 4)
            ids = []
            recs = []
            if cnt and 0 < rel < size:
                for k in range(cnt):
                    o = base + rel + k * 0x18
                    ids.append(b40(u64(d, o)))
                    # FUN_001A5F90 reads eight per-record paint percentages at
                    # +0x08..+0x0F; FUN_001A5E30 walks the u32 spawn weight at
                    # +0x10 against the list total at listoff+0x08.  [C]
                    recs.append(dict(id=ids[-1], colours=list(d[o + 8:o + 0x10]),
                                     weight=u32(d, o + 0x10),
                                     tail=u32(d, o + 0x14)))
            lists.append(ids)
            list_records.append(dict(off=p, cls=self.LIST_CLASS[len(lists) - 1],
                                     total=u32(d, base + p + 8), records=recs))
        gate = d[base + 0xB5]
        specials = []
        for k in range(5):
            if gate & (1 << k):
                specials.append(b40(u64(d, base + k * 8)))
        rel, cnt = u32(d, base + self.SPAWN_PTR), u32(d, base + self.SPAWN_PTR + 4)
        spawns = []
        if cnt and 0 < rel < size:
            for k in range(cnt):
                o = base + rel + k * 0x20
                spawns.append(f3(d, o) + f3(d, o + 0x10))
        schedule_rel = u32(d, base + self.SCHEDULE_PTR)
        schedule_count = u32(d, base + self.SCHEDULE_COUNT)
        schedules = []
        if schedule_count and 0 < schedule_rel < size:
            for k in range(schedule_count):
                o = base + schedule_rel + k * 0x58
                if o + 0x58 > base + size:
                    break
                source_rel = u32(d, o + 0x04)
                manager_rel = u32(d, o + 0x08)
                field_rel = u32(d, o + 0x0C)
                stage2_count = u32(d, o + 0x40)
                maps = []
                if (stage2_count and 0 < manager_rel < size
                        and 0 < field_rel < size):
                    n = min(stage2_count, base + size - (base + manager_rel),
                            base + size - (base + field_rel))
                    maps = [(d[base + manager_rel + j],
                             d[base + field_rel + j]) for j in range(n)]
                # Stage 2 (FUN_001A3AE0 case 2) installs a float at
                # record+slot*4+0x00: the road's cruise speed in MPH, which
                # FUN_001A6070 converts with 0.44704 (@0x003A5958).  [C]
                speeds = []
                if (stage2_count and 0 < source_rel < size
                        and 0 < manager_rel < size and 0 < field_rel < size):
                    for j in range(min(stage2_count, size - manager_rel,
                                       size - field_rel)):
                        speeds.append((d[base + manager_rel + j],
                                       d[base + field_rel + j],
                                       struct.unpack_from('<f', d,
                                                          base + source_rel
                                                          + j * 4)[0]))
                # Stage 3 (case 3) installs a 0x1C-byte record at
                # record+slot*4+0x10: seven floats whose [0..5] are the
                # per-CLASS spawn rates in vehicles/minute that FUN_001A6590
                # samples and FUN_001A6070 sums for its population law.  [C]
                rate_rel = u32(d, o + 0x10)
                rate_mgr = u32(d, o + 0x14)
                rate_fld = u32(d, o + 0x18)
                rate_count = u32(d, o + 0x44)
                rates = []
                if (rate_count and 0 < rate_rel < size and 0 < rate_mgr < size
                        and 0 < rate_fld < size):
                    for j in range(min(rate_count, size - rate_mgr,
                                       size - rate_fld)):
                        q = base + rate_rel + j * 0x1C
                        if q + 0x1C > base + size:
                            break
                        rates.append((d[base + rate_mgr + j],
                                      d[base + rate_fld + j],
                                      list(struct.unpack_from('<7f', d, q))))
                request_rel = u32(d, o + 0x38)
                request_count = u32(d, o + 0x3C)
                requests = []
                if request_count and 0 < request_rel < size:
                    n = min(request_count,
                            (base + size - (base + request_rel)) // 0x0C)
                    for j in range(n):
                        q = base + request_rel + j * 0x0C
                        records_rel = u32(d, q)
                        record_count = d[q + 8]
                        flags = []
                        if record_count and 0 < records_rel < size:
                            nr = min(record_count,
                                     (base + size - (base + records_rel)) // 0x20)
                            flags = [d[base + records_rel + r * 0x20 + 0x1B]
                                     for r in range(nr)]
                        requests.append(dict(off=q, records_rel=records_rel,
                                             scalar=struct.unpack_from('<f', d, q + 4)[0],
                                             record_count=record_count,
                                             byte9=d[q + 9], manager=d[q + 10],
                                             slot=d[q + 11], flags=flags))
                schedules.append(dict(off=o, source_rel=source_rel,
                                      stage2_count=stage2_count,
                                      manager_rel=manager_rel,
                                      field_rel=field_rel, stage2_maps=maps,
                                      speeds=speeds, rates=rates,
                                      trigger=struct.unpack_from('<H', d,
                                                                 o + 0x50)[0],
                                      trigger_hi=d[o + 0x52],
                                      requests=requests))
        pool_window_rel = u32(d, base + self.POOL_WINDOW_PTR)
        pool_window_count = u32(d, base + self.POOL_WINDOW_COUNT)
        pool_windows = []
        if pool_window_count and 0 < pool_window_rel < size:
            n = min(pool_window_count,
                    (base + size - (base + pool_window_rel)) // 0x18)
            for k in range(n):
                o = base + pool_window_rel + k * 0x18
                request_rel = u32(d, o)
                request_count = d[o + 0x14]
                refresh_count = d[o + 0x15]
                requests = []
                if request_count and 0 < request_rel < size:
                    nr = min(request_count,
                             (base + size - (base + request_rel)) // 6)
                    for j in range(nr):
                        q = base + request_rel + j * 6
                        first_row, last_row = struct.unpack_from('<HH', d, q)
                        requests.append(dict(first_row=first_row,
                                             last_row=last_row,
                                             path_id=d[q + 4],
                                             direction=d[q + 5]))
                pool_windows.append(dict(off=o, first_progress=u32(d, o + 4),
                                         last_progress=u32(d, o + 8),
                                         request_count=request_count,
                                         refresh_count=refresh_count,
                                         requests=requests))
        # Manager-record table: FUN_001A13F0 @0x001A1BE0 reads the count from
        # TDESC+0x40 and hands each 0x10-byte row to FUN_001A5680, which parks
        # it at record+0x40; the same loop inverts it into the path
        # descriptor's desc+0x06[]/desc+0x1E[]/desc+0x48 arrays that
        # FUN_0019E5B0 later searches.  Row = {u32 path_ids_rel,
        # u32 start_rows_rel, u32 slot_count, u8 flags, ...}.  [C]
        record_rel = u32(d, base + self.MANAGER_RECORD_PTR)
        record_count = d[base + self.MANAGER_RECORD_COUNT]
        records = []
        if record_count and 0 < record_rel < size:
            for k in range(record_count):
                o = base + record_rel + k * 0x10
                if o + 0x10 > base + size:
                    break
                paths_rel, rows_rel, nslot = (u32(d, o), u32(d, o + 4),
                                              u32(d, o + 8))
                slots = []
                if (0 < nslot <= 8 and 0 < paths_rel < size
                        and 0 < rows_rel < size):
                    for s in range(nslot):
                        slots.append((u32(d, base + paths_rel + s * 4),
                                      u32(d, base + rows_rel + s * 4)))
                records.append(dict(off=o, slots=slots, flags=d[o + 0xC]))
        return dict(base=base, size=size, lists=lists, specials=specials,
                    list_records=list_records, records=records,
                    spawn_off=base + rel if cnt else 0, spawns=spawns,
                    schedules=schedules, pool_windows=pool_windows)

    # ============================================================== geometry
    def corridor(self, net):
        """Road-boundary strip -> {off, pairs, A, B, mid, width, step}.  [S]

        The strip is the one INTERLEAVED array in the network section: its
        records alternate between the two road edges, so it ZIGZAGS across
        the road while every other array is a smooth polyline.  The test is
        therefore the turn angle, which is scale-free and needs no distance
        threshold at all:

            cos(angle at P[i+1]) < -0.2   -> zigzag (strip)
                                  ~ +1    -> smooth polyline

        The strip is the longest run of that, tolerating up to 5 consecutive
        non-zigzag samples (on AS/C1_V1's 1752-record strip only 75 samples
        fail the test at all, in runs of 1..4 -- corners where one strand
        bunches up).  (net+0x170 is the start on AS/C1_V1, but on
        US/C3_V1 292 records of other arrays come first -- the old constant
        was a per-track calibration.)  The pair phase is the one whose
        median strand separation is smaller, i.e. the perpendicular pairing.
        """
        d = self.d
        lo = net['base'] + 0x20 + net['nsec'] * 0x10
        lo = (lo + 15) & ~15
        hi = net['idx']
        n = (hi - lo) // 16
        P = [f3(d, lo + i * 16) for i in range(n)]
        cosang = []
        for i in range(n - 2):
            ax, az = P[i + 1][0] - P[i][0], P[i + 1][2] - P[i][2]
            bx, bz = P[i + 2][0] - P[i + 1][0], P[i + 2][2] - P[i + 1][2]
            na, nb = math.hypot(ax, az), math.hypot(bx, bz)
            cosang.append((ax * bx + az * bz) / (na * nb)
                          if na > 1e-6 and nb > 1e-6 else 1.0)
        best = (0, 0)
        start, bad = 0, 0
        for i in range(n - 2):
            if cosang[i] < -0.2:
                bad = 0
                if i + 2 - start > best[1]:
                    best = (start, i + 2 - start)
            else:
                bad += 1
                if bad >= 6:
                    start = i + 2
        s0, cnt = best
        if cnt < 64:
            raise SystemExit("%s: no interleaved boundary strip found"
                             % os.path.basename(self.path))
        phase = min((0, 1), key=lambda p: median(
            [dist(P[s0 + p + 2 * k], P[s0 + p + 2 * k + 1])
             for k in range((cnt - p) // 2)]))
        s0 += phase
        pairs = (cnt - phase) // 2
        A = [P[s0 + 2 * k] for k in range(pairs)]
        B = [P[s0 + 2 * k + 1] for k in range(pairs)]
        M = [((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2)
             for a, b in zip(A, B)]
        # The dropout tolerance can bridge into a small neighbouring array at
        # either end.  Trim to the sub-run that best CLOSES INTO A LAP: among
        # sub-runs covering 0.85..1.25 lap lengths and free of bridge jumps
        # (advance > 6 x median), take the one whose ends are closest.
        adv = [dist(M[i], M[i + 1]) for i in range(pairs - 1)]
        ma = median(adv) or 1.0
        arc = [0.0]
        for a in adv:
            arc.append(arc[-1] + a)
        jump = [0]
        for a in adv:
            jump.append(jump[-1] + (1 if a > 6 * ma else 0))
        lo_len, hi_len = 0.85 * net['lap'], 1.25 * net['lap']
        best_pair, best_close = (0, pairs - 1), None
        j = 0
        for i in range(pairs):
            if j < i:
                j = i
            while j + 1 < pairs and arc[j] - arc[i] < lo_len:
                j += 1
            k = j
            while k < pairs and arc[k] - arc[i] <= hi_len:
                if arc[k] - arc[i] >= lo_len and jump[k] == jump[i]:
                    c = dist(M[i], M[k])
                    if best_close is None or c < best_close:
                        best_close, best_pair = c, (i, k)
                k += 1
        b0, b1 = best_pair
        A, B, M = A[b0:b1 + 1], B[b0:b1 + 1], M[b0:b1 + 1]
        pairs = len(A)
        s0 += 2 * b0
        sep = [dist(a, b) for a, b in zip(A, B)]
        adv = [dist(M[i], M[i + 1]) for i in range(pairs - 1)]
        return dict(off=lo + s0 * 16, pairs=pairs, A=A, B=B, mid=M,
                    width=median(sep), step=median(adv),
                    length=sum(adv) + dist(M[0], M[-1]),
                    close=dist(M[0], M[-1]),
                    rel=lo + s0 * 16 - net['base'],
                    end=lo + (s0 + 2 * pairs) * 16)

    def lap_loops(self, net, corridor):
        """Closed lap-length point loops between the strip and the index
        directory.  Returns [{off,count,pts,length,close,area}].     [S]"""
        d = self.d
        out = []
        base = (net['base'] + 0x20 + net['nsec'] * 0x10 + 15) & ~15
        for lo, hi in ((base, corridor['off']), (corridor['end'], net['idx'])):
            out += self._loops_in(net, lo, hi)
        return out

    def _loops_in(self, net, lo, hi):
        d = self.d
        n = (hi - lo) // 16
        if n < 32:
            return []
        pts = [f3(d, lo + i * 16) for i in range(n)]
        st = [dist(pts[i], pts[i + 1]) for i in range(n - 1)]
        scale = median(st) or 1.0
        # Array boundaries: a step more than 6 x the region's own median
        # point spacing.  (A local-window median instead splits gentle
        # in-array gaps and merges nothing; the region median is stable
        # because every array here is sampled at the same ~6 m spacing.)
        bounds, start = [], 0
        for i in range(n - 1):
            if st[i] > 6.0 * scale:
                bounds.append((start, i + 1 - start))
                start = i + 1
        bounds.append((start, n - start))
        # Two node-paired arrays are geometrically continuous, so they can
        # land in one run.  A run longer than 1.5 laps whose length is an
        # exact multiple of the node count is that case: split it back.
        runs = []
        for s, c in bounds:
            plen = sum(st[s:s + c - 1])
            k = 1
            if plen > 1.5 * net['lap'] and net['nodes'] and c > net['nodes'] \
                    and c % net['nodes'] == 0:
                k = c // net['nodes']
            for j in range(k):
                runs.append((s + j * (c // k), c // k))
        out = []
        for s, c in runs:
            if c < 32:
                continue
            p = pts[s:s + c]
            steps = sorted(dist(p[i], p[i + 1]) for i in range(c - 1))
            med = steps[len(steps) // 2]
            close = dist(p[0], p[-1])
            length = sum(steps) + close
            if not (0.7 * net['lap'] <= length <= 1.5 * net['lap']):
                continue
            # a drive line closes into a lap; an open sub-path does not
            if close > 0.08 * net['lap']:
                continue
            area = 0.0
            for i in range(c):
                a, b = p[i], p[(i + 1) % c]
                area += a[0] * b[2] - b[0] * a[2]
            out.append(dict(off=lo + s * 16, count=c, pts=p, length=length,
                            close=close, med=med, area=area / 2))
        return out


# ==========================================================================
# Orientation / role assignment helpers (all data-derived).
# ==========================================================================
def polyline_lateral(ref, q):
    """Signed lateral offset of point q from closed polyline ref."""
    j = min(range(len(ref)), key=lambda k: dist2(ref[k], q))
    a, b = ref[(j + 1) % len(ref)], ref[j]
    tx, tz = a[0] - b[0], a[2] - b[2]
    m = math.hypot(tx, tz) or 1.0
    return ((q[0] - b[0]) * (-tz) + (q[2] - b[2]) * tx) / m, j


def median_offset(ref, pts, stride=7):
    return median([abs(polyline_lateral(ref, q)[0]) for q in pts[::stride]])


def tangent_dot(pts, pos, fwd):
    """Dot of the polyline tangent nearest `pos` with `fwd` (xz only)."""
    n = len(pts)
    j = min(range(n), key=lambda k: dist2(pts[k], pos))
    a, b = pts[(j + 1) % n], pts[j]
    tx, tz = a[0] - b[0], a[2] - b[2]
    m = math.hypot(tx, tz) or 1.0
    return (tx * fwd[0] + tz * fwd[2]) / m, j, math.sqrt(dist2(pts[j], pos))


def gl(p):
    """Game space -> harness GL space: one uniform z reflection (RE_NOTES 12)."""
    return (p[0], p[1], -p[2])


def offset_pair(pts, half):
    """Two strands `half` metres either side of a polyline (left, right)."""
    n = len(pts)
    A, B = [], []
    for i, p in enumerate(pts):
        a, b = pts[(i + 1) % n], pts[(i - 1) % n]
        tx, tz = a[0] - b[0], a[2] - b[2]
        m = math.hypot(tx, tz) or 1.0
        nx, nz = -tz / m, tx / m
        A.append((p[0] + nx * half, p[1], p[2] + nz * half))
        B.append((p[0] - nx * half, p[1], p[2] - nz * half))
    return A, B


# --------------------------------------------------------------------------
# On-road support, measured against the game's own collision world.
# build/tracks/<ID>/collision.bin is written by tools/extract_collision.py;
# when it is absent the route choice falls back to start-grid proximity only.
# --------------------------------------------------------------------------
def load_road_grid(track, cell=20.0):
    path = os.path.join(tl.out_root(track), "collision.bin")
    if not os.path.exists(path):
        return None
    d = open(path, 'rb').read()
    if d[:4] != b'B3CL':
        return None
    n = struct.unpack_from('<I', d, 8)[0]
    grid = {}
    o = 0x28
    for _ in range(n):
        v = struct.unpack_from('<9f', d, o)
        o += 40
        a, b, c = v[0:3], v[3:6], v[6:9]
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        nx = uy * vz - uz * vy
        ny = uz * vx - ux * vz
        nz = ux * vy - uy * vx
        L = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        if ny / L < 0.45:                     # not a drivable surface
            continue
        for gx in range(int(min(a[0], b[0], c[0]) // cell),
                        int(max(a[0], b[0], c[0]) // cell) + 1):
            for gz in range(int(min(a[2], b[2], c[2]) // cell),
                            int(max(a[2], b[2], c[2]) // cell) + 1):
                grid.setdefault((gx, gz), []).append((a, b, c))
    return (grid, cell)


def road_support(road, pts, tol=6.0):
    """How many of `pts` have NO road-like triangle within `tol` below them."""
    grid, cell = road
    bad = 0
    for p in pts:
        hit = False
        for a, b, c in grid.get((int(p[0] // cell), int(p[2] // cell)), ()):
            d1 = (b[0] - a[0]) * (p[2] - a[2]) - (b[2] - a[2]) * (p[0] - a[0])
            d2 = (c[0] - b[0]) * (p[2] - b[2]) - (c[2] - b[2]) * (p[0] - b[0])
            d3 = (a[0] - c[0]) * (p[2] - c[2]) - (a[2] - c[2]) * (p[0] - c[0])
            if not ((d1 >= 0 and d2 >= 0 and d3 >= 0)
                    or (d1 <= 0 and d2 <= 0 and d3 <= 0)):
                continue
            ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
            vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
            ny = uz * vx - ux * vz
            if abs(ny) < 1e-9:
                continue
            nx = uy * vz - uz * vy
            nz = ux * vy - uy * vx
            h = a[1] + (nx * (a[0] - p[0]) + nz * (a[2] - p[2])) / ny
            if abs(h - p[1]) < tol:
                hit = True
                break
        if not hit:
            bad += 1
    return bad


# ==========================================================================
def analyse(track, event=DEFAULT_EVENT, verbose=True):
    """Everything the path/grid/traffic extractors need for one track."""
    bgd = BGD(os.path.join(track['dir'], "Gamedata.bgd"))
    ev = bgd.event(event)
    grid = bgd.grid(ev)
    net = bgd.network(ev)
    cor = bgd.corridor(net)
    loops = bgd.lap_loops(net, cor)
    g0 = grid[0]

    # --- orientation: ascending index must run WITH the race direction -----
    dot_mid, _, _ = tangent_dot(cor['mid'], g0['pos'], g0['fwd'])
    reversed_walls = dot_mid < 0
    if reversed_walls:
        cor['A'].reverse()
        cor['B'].reverse()
        cor['mid'].reverse()

    # --- roles: nearest lap loop to the corridor is the oncoming lane ------
    for lp in loops:
        lp['corridor_offset'] = median_offset(cor['mid'], lp['pts'])
        lp['dot'], _, lp['grid_dist'] = tangent_dot(lp['pts'], g0['pos'],
                                                    g0['fwd'])
    ranked = sorted(loops, key=lambda l: l['corridor_offset'])
    oncoming = ranked[0] if ranked else None
    raceline = ranked[1] if len(ranked) > 1 else None
    for lp, want in ((oncoming, -1), (raceline, +1)):
        if lp is not None and lp['dot'] * want < 0:
            lp['pts'].reverse()
            lp['reversed'] = True
        elif lp is not None:
            lp['reversed'] = False

    # --- WHICH LINE IS THE DRIVING ROUTE? ---------------------------------
    # Not the same array on every track, and it is measurable rather than
    # assumed: score every candidate by how far the START GRID sits off it
    # (the six slots are on the racing surface by construction), and, when
    # build/tracks/<ID>/collision.bin exists, by how many of its points have
    # a road-like triangle under them in the game's own collision world.
    #   AS/C1_V1: corridor midline 0/868 unsupported, grid 2.1 m off
    #             -> route = corridor midline   (the harness's current
    #                behaviour, in-game verified)
    #   US/C3_V1: corridor midline 358/1012 unsupported, grid 46.2 m off;
    #             drive line @0x0C0930 0/1013 unsupported, grid 5.8 m off
    #             -> route = that drive line.  Silver Lake's strip is a
    #                50 m right-of-way envelope, not the carriageway.
    cands = [dict(name='corridor midline', pts=cor['mid'], loop=None)]
    for lp in loops:
        cands.append(dict(name='line @%#x' % lp['off'], pts=lp['pts'], loop=lp))
    road = load_road_grid(track)
    for c in cands:
        lat = [polyline_lateral(c['pts'], s['pos'])[0] for s in grid]
        c['grid_off'] = sum(abs(v) for v in lat) / len(lat)
        c['grid_spread'] = max(lat) - min(lat)
        c['unsupported'] = (road_support(road, c['pts']) if road else None)
    if road:
        cands.sort(key=lambda c: (c['unsupported'], c['grid_off']))
    else:
        cands.sort(key=lambda c: c['grid_off'])
    route_c = cands[0]
    route = route_c['pts']

    # --- roles, measured against the chosen route -------------------------
    # The oncoming carriageway is the nearest lap loop that is at least one
    # grid-column apart from the route (the six start slots form two columns
    # one lane apart, which is the track's own lane scale); anything closer
    # is a near-duplicate of the route, not the other carriageway.
    lane = max(route_c['grid_spread'], 1e-3)
    for lp in loops:
        lp['route_offset'] = median_offset(route, lp['pts'])
        lp['dot'], _, lp['grid_dist'] = tangent_dot(lp['pts'], g0['pos'],
                                                    g0['fwd'])
    apart = sorted((l for l in loops if l['route_offset'] > lane),
                   key=lambda l: l['route_offset'])
    oncoming = apart[0] if apart else None
    if route_c['loop'] is not None:
        raceline = route_c['loop']          # route already IS the race line
    else:
        rest = [l for l in loops if l is not oncoming]
        raceline = min(rest, key=lambda l: l['route_offset']) if rest else None
    for lp, want in ((oncoming, -1), (raceline, +1)):
        if lp is not None and lp['dot'] * want < 0:
            lp['pts'].reverse()
            lp['reversed'] = True
        elif lp is not None:
            lp['reversed'] = False

    # --- wall strands: the pair whose midpoint IS the route ---------------
    if route_c['loop'] is None:
        wall_a, wall_b, wall_src = cor['A'], cor['B'], "boundary strip"
    else:
        half = (oncoming['route_offset'] / 2.0) if oncoming else cor['width'] / 2
        wall_a, wall_b = offset_pair(route, half)
        wall_src = "SYNTHESISED +-%.1f m around the route" % half

    route_start = min(range(len(route)),
                      key=lambda k: dist2(route[k], g0['pos']))

    if verbose:
        print("%s (%s)  %s" % (track['id'], track['name'],
                               os.path.relpath(bgd.path, tl.GAME_DIR)))
        print("  events %d, using %s (laps=%d) param@%#x spatial@%#x "
              "tdesc@%#x net@%#x" % (bgd.count, ev['id'], ev['laps'],
                                     ev['param'], ev['spatial'][1],
                                     ev['tdesc'][1], ev['net'][1]))
        print("  network: %d nodes, %d idx rows, %d route sections, "
              "lap %.1f m, idx@%#x" % (net['nodes'], net['rows'], net['nsec'],
                                       net['lap'], net['idx']))
        print("  strip@%#x (net+%#x) %d pairs, width %.1f m, advance %.2f m,"
              " midline %.1f m (%.0f%% of lap) closes %.1f m, %s race "
              "direction" % (cor['off'], cor['rel'], cor['pairs'],
                             cor['width'], cor['step'], cor['length'],
                             100.0 * cor['length'] / net['lap'], cor['close'],
                             "reversed to match" if reversed_walls
                             else "already matches"))
        print("  route candidates (grid-off = mean |lateral| of the 6 start "
              "slots; unsup = points with no road under them):")
        for c in cands:
            print("     %-22s n=%-5d grid-off %6.2f m  unsup %s%s"
                  % (c['name'], len(c['pts']), c['grid_off'],
                     "%d/%d" % (c['unsupported'], len(c['pts']))
                     if c['unsupported'] is not None else "n/a",
                     "   <== ROUTE" if c is route_c else ""))
        if road is None:
            print("     (no collision.bin for this track -- route chosen on "
                  "start-grid proximity alone; run extract_collision.py first)")
        for lp in sorted(loops, key=lambda l: l['route_offset']):
            role = ("oncoming" if lp is oncoming else
                    "race line" if lp is raceline else "-")
            print("     lap loop @%#08x x%-5d len %7.1f close %5.1f "
                  "route-off %6.1f dot %+.3f  %s%s"
                  % (lp['off'], lp['count'], lp['length'], lp['close'],
                     lp['route_offset'], lp['dot'], role,
                     " (reversed)" if lp.get('reversed') else ""))
        print("  walls: %s;  route start index %d/%d (grid slot 0 %.2f m off "
              "the route)" % (wall_src, route_start, len(route),
                              dist(route[route_start], g0['pos'])))
    return dict(bgd=bgd, ev=ev, grid=grid, net=net, cor=cor, loops=loops,
                oncoming=oncoming, raceline=raceline, route=route,
                route_src=route_c['name'], wall_a=wall_a, wall_b=wall_b,
                wall_src=wall_src, cands=cands,
                route_start=route_start, reversed_walls=reversed_walls,
                track=track)


# ==========================================================================
def write_route_bin(path, a):
    """route.bin v3 -- see the module docstring."""
    cor, race, onc = a['cor'], a['raceline'], a['oncoming']
    rl = race['pts'] if race else []
    ol = onc['pts'] if onc else []
    graph = a['bgd'].nav_graph(a['ev'])
    plans = a['bgd'].nav_plans(a['ev'])
    with open(path, 'wb') as f:
        f.write(b'B3RT')
        f.write(struct.pack('<IIIIIIfI', 3, len(a['wall_a']), len(rl),
                            len(ol), len(a['route']), a['route_start'],
                            a['net']['lap'],
                            (1 if a['reversed_walls'] else 0)
                            | (2 if a['route_src'] != 'corridor midline'
                               else 0)))
        f.write(struct.pack('<I', cor['pairs']))
        for arr in (a['wall_a'], a['wall_b'], rl, ol, a['route'],
                    cor['A'], cor['B']):
            for p in arr:
                f.write(struct.pack('<3f', *gl(p)))
        pair_base = 0
        link_base = 0
        sections = []
        pairs = []
        links = []
        for row in graph['rows']:
            sections.append((pair_base, link_base, row['node_count'], row['flags']))
            for node in range(row['node_count'] + 1):
                pairs.append(struct.unpack_from('<HH', a['bgd'].d,
                                                row['pairs'] + node * 4))
            for node in range(row['node_count']):
                anchor, aux = struct.unpack_from('<HH', a['bgd'].d,
                                                 row['links'] + node * 10)
                forward_section = a['bgd'].d[row['links'] + node * 10 + 4]
                reverse_section = a['bgd'].d[row['links'] + node * 10 + 5]
                forward_node, reverse_node = struct.unpack_from(
                    '<HH', a['bgd'].d, row['links'] + node * 10 + 6)
                links.append((anchor, aux, forward_section, reverse_section,
                              forward_node, reverse_node))
            pair_base += row['node_count'] + 1
            link_base += row['node_count']
        f.write(struct.pack('<IIIII', graph['point_count'], len(sections),
                            len(pairs), len(links), len(plans)))
        for point_id in range(graph['point_count']):
            f.write(struct.pack('<3f', *gl(f3(a['bgd'].d,
                                               graph['points'] + point_id * 16))))
        for section in sections:
            f.write(struct.pack('<IIHH', *section))
        for pair in pairs:
            f.write(struct.pack('<HH', *pair))
        for link in links:
            f.write(struct.pack('<HHBBHH', *link))
        for plan in plans:
            f.write(struct.pack('<HHHHBBBB', plan['node_a'], plan['node_b'],
                                plan['node_c'], plan['speed'], plan['section'],
                                plan['byte9'], plan['flags'], plan['byte11']))
    return path


def write_header(path, a):
    t = a['track']
    cor, race = a['cor'], a['raceline']
    rl = race['pts'] if race else []
    graph = a['bgd'].nav_graph(a['ev'])
    plans = a['bgd'].nav_plans(a['ev'])
    with open(path, 'w') as f:
        w = f.write
        w("// GENERATED by tools/extract_bgd_paths.py -- DO NOT EDIT\n")
        w("// Track %s = %s (%s/Gamedata.bgd), event %s.\n"
          % (t['id'], t['name'], os.path.relpath(t['dir'], tl.GAME_DIR),
             a['ev']['id']))
        w("// Located structurally through the game's own .bgd parser\n")
        w("// FUN_0018B250 (event count @0x260, ids @0x08, param records\n")
        w("// @0x198; road network from param+0x3D4/0x3D8) [C]; the geometry\n")
        w("// pools inside the network section are recovered from the file's\n")
        w("// own geometry [S] -- see the module docstring for the criteria.\n")
        w("// RAW GAME SPACE (z NOT negated): burnout3_full.c's init_paths\n")
        w("// and route_lane_fixup apply the RE_NOTES 12 reflection (z=-z)\n")
        w("// themselves when they copy these arrays.  build/tracks/<ID>/\n")
        w("// route.bin carries the same points ALREADY reflected.\n")
        w("// Corridor strip @%#x, %d pairs, width %.1f m; race line @%#x\n"
          % (cor['off'], cor['pairs'], cor['width'],
             race['off'] if race else 0))
        w("// x%d; lap %.1f m.  Ascending index runs WITH the race\n"
          % (len(rl), a['net']['lap']))
        w("// direction (start-grid slot 0 forward).\n")
        w("// DRIVING ROUTE = %s, chosen by measured on-road support +\n"
          % a['route_src'])
        w("// start-grid proximity; the wall strands below are %s so that\n"
          % a['wall_src'])
        w("// (B3_WALL_A[i]+B3_WALL_B[i])/2 IS that route.\n\n")
        w("#ifndef BURNOUT3_TRACK_PATHS_H\n#define BURNOUT3_TRACK_PATHS_H\n\n")
        w("// Race line (the harness uses it only as a lane-nudge target).\n")
        w("static const float B3_CENTERLINE[][3] = {\n")
        for p in rl:
            w("    {%.3ff, %.3ff, %.3ff},\n" % tuple(p))
        w("};\nenum { B3_CENTERLINE_COUNT = %d };\n\n" % len(rl))
        w("typedef struct { unsigned short point_a, point_b; } B3NavPair;\n")
        w("typedef struct { unsigned short anchor, aux, forward_node, reverse_node; "
          "unsigned char forward_section, reverse_section; } B3NavLink;\n")
        w("typedef struct { unsigned int pair_base, link_base; unsigned short node_count, flags; } B3NavSection;\n")
        w("typedef struct { unsigned short node_a, node_b, node_c, speed; "
          "unsigned char section, byte9, flags, byte11; } B3NavPlan;\n\n")
        w("static const float B3_NAV_POINTS[][3] = {\n")
        for point_id in range(graph['point_count']):
            p = f3(a['bgd'].d, graph['points'] + point_id * 16)
            w("    {%.3ff, %.3ff, %.3ff},\n" % p)
        w("};\nenum { B3_NAV_POINT_COUNT = %d };\n\n" % graph['point_count'])
        pair_base = 0
        link_base = 0
        sections = []
        pairs = []
        links = []
        for row in graph['rows']:
            sections.append((pair_base, link_base, row['node_count'], row['flags']))
            for node in range(row['node_count'] + 1):
                pairs.append(struct.unpack_from('<HH', a['bgd'].d,
                                                 row['pairs'] + node * 4))
            for node in range(row['node_count']):
                anchor, aux = struct.unpack_from('<HH', a['bgd'].d,
                                                 row['links'] + node * 10)
                forward_section = a['bgd'].d[row['links'] + node * 10 + 4]
                reverse_section = a['bgd'].d[row['links'] + node * 10 + 5]
                forward_node, reverse_node = struct.unpack_from(
                    '<HH', a['bgd'].d, row['links'] + node * 10 + 6)
                links.append((anchor, aux, forward_node, reverse_node,
                              forward_section, reverse_section))
            pair_base += row['node_count'] + 1
            link_base += row['node_count']
        w("static const B3NavSection B3_NAV_SECTIONS[] = {\n")
        for pair_offset, link_offset, node_count, flags in sections:
            w("    {%du, %du, %du, %du},\n" %
              (pair_offset, link_offset, node_count, flags))
        w("};\nenum { B3_NAV_SECTION_COUNT = %d };\n\n" % len(sections))
        w("static const B3NavPair B3_NAV_PAIRS[] = {\n")
        for point_a, point_b in pairs:
            w("    {%du, %du},\n" % (point_a, point_b))
        w("};\nenum { B3_NAV_PAIR_COUNT = %d };\n\n" % len(pairs))
        w("static const B3NavLink B3_NAV_LINKS[] = {\n")
        for anchor, aux, forward_node, reverse_node, forward_section, reverse_section in links:
            w("    {%du, %du, %du, %du, %du, %du},\n" %
              (anchor, aux, forward_node, reverse_node,
               forward_section, reverse_section))
        w("};\nenum { B3_NAV_LINK_COUNT = %d };\n\n" % len(links))
        w("static const B3NavPlan B3_NAV_PLANS[] = {\n")
        for plan in plans:
            w("    {%du, %du, %du, %du, %du, %du, %du, %du},\n" %
              (plan['node_a'], plan['node_b'], plan['node_c'], plan['speed'],
               plan['section'], plan['byte9'], plan['flags'], plan['byte11']))
        w("};\nenum { B3_NAV_PLAN_COUNT = %d };\n\n" % len(plans))
        for nm, arr in (("A", a['wall_a']), ("B", a['wall_b'])):
            w("// Road strand %s -- %s.\n" % (nm, a['wall_src']))
            w("static const float B3_WALL_%s[][3] = {\n" % nm)
            for p in arr:
                w("    {%.3ff, %.3ff, %.3ff},\n" % tuple(p))
            w("};\nenum { B3_WALL_%s_COUNT = %d };\n\n" % (nm, len(arr)))
        w("// Index into the wall/midline arrays nearest start-grid slot 0:\n"
          "// the harness rotates its route so this becomes progress 0.\n")
        w("#define B3_ROUTE_START %d\n\n" % a['route_start'])
        w("// Lap length from the road-network route-section records.\n")
        w("#define B3_LAP_LENGTH %.1ff\n\n" % a['net']['lap'])
        w("#endif // BURNOUT3_TRACK_PATHS_H\n")
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    tl.add_track_arg(ap)
    ap.add_argument("--event", default=DEFAULT_EVENT)
    ap.add_argument("--header", default=None,
                    help="output C header (default src/burnout3_track_paths.h)")
    ap.add_argument("--all", action="store_true",
                    help="analyse every track in tlist.bin and report")
    args = ap.parse_args()

    if args.all:
        ok = 0
        for t in tl.track_table():
            if not os.path.exists(os.path.join(t['dir'], "Gamedata.bgd")):
                print("%s: MISSING" % t['id'])
                continue
            try:
                analyse(t, args.event)
                ok += 1
            except Exception as e:                        # noqa: BLE001
                print("%s (%s): FAIL %s: %s"
                      % (t['id'], t['name'], type(e).__name__, e))
            print()
        print("== %d tracks analysed cleanly" % ok)
        return 0

    track = tl.resolve(args.track)
    a = analyse(track, args.event)
    if a['raceline'] is None or a['oncoming'] is None:
        print("WARNING: only %d lap loop(s) found; race/oncoming lines "
              "incomplete" % len(a['loops']))
    hdr = args.header or os.path.join(os.path.dirname(__file__), "..", "src",
                                      "burnout3_track_paths.h")
    write_header(hdr, a)
    rb = write_route_bin(os.path.join(tl.out_root(track), "route.bin"), a)
    print("wrote %s" % os.path.normpath(hdr))
    print("wrote %s (%d route/wall points, %d race, %d oncoming, %d raw "
          "strip pairs)"
          % (rb, len(a['wall_a']),
             len(a['raceline']['pts']) if a['raceline'] else 0,
             len(a['oncoming']['pts']) if a['oncoming'] else 0,
             a['cor']['pairs']))
    return 0


if __name__ == "__main__":
    sys.exit(main())
