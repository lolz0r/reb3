#!/usr/bin/env python3
"""Decode Tracks/tlist.bin -- the shipped TRACK LIST -- and resolve track ids.

This module is both a tool (`python3 tools/extract_tlist.py` prints the full
36-track table) and the shared track-selection helper the other per-track
extractors import.  Nothing here is calibrated to one track.

================================================================= tlist.bin ==
`Tracks/tlist.bin` is the exact same container shape as `pveh/vlist.bin`
(the vehicle list decoded by tools/extract_car_vdb.py): a raw dump of the
tool's fixed-size C arrays, of which only the first `count` entries are real
and the rest is uninitialised tool memory.  4096 bytes, little-endian.

    +0x000  u32   version        = 4        (vlist.bin is version 6)
    +0x004  u32   count          = 36       (vlist.bin: 107)
    +0x008  u32[128] flagsA      -- 36 used, ends 0x208; 1 on the eight P
                                   tracks (US_P1/P2, EU_P1/P2), 0 elsewhere
    +0x208  u32[128] flagsB      -- 36 used, ends 0x408; 1 on indices 0..27
                                   (US + EU), 0 on the eight AS tracks
    +0x408  u64[128] track ids   -- 36 used, ends 0x808; base-40 packed
                                   12-char ids, e.g. "US_C3_V1"
    +0x808  u32[36]  zero        -- a fourth per-track array, all zero
    (everything outside those spans is stale tool memory and decodes as junk)

The array bases are the same as vlist.bin's (0x008 / 0x408) because both are
dumps of `Thing arr[N]` declarations; the tlist stride is 128 entries where
vlist uses 256.  Derived structurally: each array is exactly `count` sane
values followed by high-entropy garbage, and the four spans tile 0x008..0x80C
with no overlap.                                                          [S]

The packed ids decode with the game's own base-40 decoder FUN_001AECC0
(charset " -/0-9A-Z_"), the same one used for vehicle ids and .bgd event
ids.                                                                      [C]

============================================================ id -> directory ==
`FUN_001574F0` builds the track directory from the packed id [C]:

    strncpy(buf, "tracks/");  FUN_001aecc0(id_lo, id_hi);   // -> "US_C3_V1"
    strncat(buf, chars[0:2]); strcat(buf, "/");             // "tracks/US/"
    strncat(buf, chars[3:5]); strcat(buf, "_");             // "tracks/US/C3_"
    strncat(buf, chars[6:8]); strcat(buf, "/");             // ".../C3_V1/"

so id `<REG>_<Cn>_<Vn>` maps to `Tracks/<REG>/<Cn>_<Vn>/`.

================================================================ id -> name ==
`FUN_00158680(track_index)` returns the Globalus string index of the track's
display name [C]:

    undefined4 FUN_00158680(int i) {
        if (-1 < i) return *(undefined4 *)(&DAT_0039ee00 + i * 4);
        return 0x4ea;                       /* 1258 = "UNKNOWN TRACK" */
    }

`DAT_0039EE00` is 36 u32 string indices -- exactly the tlist count, indexed by
the tlist index, and each of the 18 locations appears twice in a row (the V1
and V2 variants of one track share a location name).  Globalus.bin strings are
decoded exactly as tools/validate_takedown.py's globalus_string(): u32 count
at +0x08, u32 offset table at +0x10, UTF-16LE payload.                    [C]

Independent corroboration (texture-name prefixes in each track's static.dat)
agrees with the table on all 18 locations -- see the report printed by this
tool with --verify: US/C1 = WF_* (Waterfront), US/C2 = Chgo_* (Downtown =
Chicago), US/C3 = GL_* (Silver Lake), AS/C1 = bk_* (Bangkok = Golden City),
AS/C2 = HK_* (Dockside), EU/C3 = R_* (Riviera), EU/C4 = VYD_* (Vineyard),
and the M/P tracks carry a mix of the district prefixes they run through.

Usage:
    python3 tools/extract_tlist.py              # print the 36-track table
    python3 tools/extract_tlist.py --verify     # + texture-prefix cross-check
    B3_TRACK=AS_C1_V1 python3 tools/extract_track.py     # any extractor
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402
import os
import struct
import sys

GAME_DIR = os.environ.get(
    "B3_GAME_DIR",
    game_root())
TRACKS_DIR = os.path.join(GAME_DIR, "Tracks")
TLIST = os.path.join(TRACKS_DIR, "tlist.bin")
GLOBALUS = os.path.join(GAME_DIR, "Data", "Globalus.bin")
ELF = os.path.join(os.path.dirname(__file__), "..", "build", "burnout3.elf")

# FUN_00158680's table: 36 u32 Globalus string indices, indexed by track id. [C]
NAME_TABLE_VA = 0x0039EE00
UNKNOWN_TRACK_STR = 0x4EA          # FUN_00158680's out-of-range return

# The default track for the whole extractor chain.  Overridable per run with
# --track / B3_TRACK; nothing else in the tooling knows a track name.
DEFAULT_TRACK = os.environ.get("B3_TRACK", "US_C3_V1")   # SILVER LAKE

CS = " -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_"


def b40(v):
    """FUN_001AECC0: base-40 decode, LSB char first, then reversed. [C]"""
    out = []
    for _ in range(12):
        out.append(CS[v % 40])
        v //= 40
    return ''.join(reversed(out)).strip()


# --------------------------------------------------------------------------
# ELF (correctly mapped XBE) reader -- section-aware, never flat.  See
# HANDOFF.md section 2: a flat load silently reads the wrong bytes.
# --------------------------------------------------------------------------
class Image(object):
    def __init__(self, path=ELF):
        self.raw = open(path, 'rb').read()
        phoff = struct.unpack_from('<I', self.raw, 0x1C)[0]
        entsz, num = struct.unpack_from('<HH', self.raw, 0x2A)
        self.segs = []
        for i in range(num):
            typ, off, va, _pa, fsz, msz = struct.unpack_from(
                '<6I', self.raw, phoff + i * entsz)
            if typ == 1:
                self.segs.append((va, off, fsz, msz))

    def read(self, va, n):
        for base, off, fsz, _msz in self.segs:
            if base <= va < base + fsz:
                k = va - base
                if k + n <= fsz:
                    return self.raw[off + k:off + k + n]
        raise KeyError("VA %#x not mapped" % va)


def globalus_strings(path=GLOBALUS):
    blob = open(path, 'rb').read()
    count = struct.unpack_from('<I', blob, 8)[0]

    def get(idx):
        off = struct.unpack_from('<I', blob, 0x10 + idx * 4)[0]
        end = off
        while end + 1 < len(blob) and blob[end:end + 2] != b'\0\0':
            end += 2
        return blob[off:end].decode('utf-16-le', 'replace')
    return count, get


# --------------------------------------------------------------------------
def read_tlist(path=TLIST):
    """[(index, id, flagA, flagB)] in file order."""
    d = open(path, 'rb').read()
    version, count = struct.unpack_from('<II', d, 0)
    assert version == 4, "tlist.bin version %d (expected 4)" % version
    assert 0 < count <= 128, "implausible track count %d" % count
    out = []
    for i in range(count):
        fa = struct.unpack_from('<I', d, 0x008 + i * 4)[0]
        fb = struct.unpack_from('<I', d, 0x208 + i * 4)[0]
        pid = struct.unpack_from('<Q', d, 0x408 + i * 8)[0]
        name = b40(pid)
        assert len(name) == 8 and name[2] == '_' and name[5] == '_', \
            "track id %d decodes to %r" % (i, name)
        out.append((i, name, fa, fb))
    return out


def track_table():
    """Full track registry: list of dicts, one per shipped track variant."""
    rows = read_tlist()
    names = {}
    try:
        img = Image()
        raw = img.read(NAME_TABLE_VA, len(rows) * 4)
        idxs = list(struct.unpack_from('<%dI' % len(rows), raw, 0))
        _, gstr = globalus_strings()
        names = {i: gstr(idxs[i]) for i in range(len(rows))}
        strids = {i: idxs[i] for i in range(len(rows))}
    except (IOError, OSError, KeyError):
        strids = {}
    out = []
    for i, tid, fa, fb in rows:
        reg, code, var = tid.split('_')
        out.append(dict(
            index=i, id=tid, region=reg, code=code, variant=var,
            dir=os.path.join(TRACKS_DIR, reg, "%s_%s" % (code, var)),
            name=names.get(i, "TRACK %d" % i),
            name_str=strids.get(i, UNKNOWN_TRACK_STR),
            flagA=fa, flagB=fb))
    return out


def resolve(spec=None):
    """Resolve a track spec to its registry entry.

    Accepts a tlist id (`US_C3_V1`), a `REG/Cn_Vn` path fragment, a display
    name (`SILVER LAKE`, matching the V1 variant), a bare tlist index, or a
    directory path.  Falls back to B3_TRACK then DEFAULT_TRACK.
    """
    spec = spec or os.environ.get("B3_TRACK") or DEFAULT_TRACK
    tbl = track_table()
    s = str(spec).strip().rstrip('/')
    key = s.replace('/', '_').replace('\\', '_').upper()
    for t in tbl:
        if t['id'] == key:
            return t
    if os.path.isdir(s):
        parts = os.path.normpath(s).split(os.sep)
        key = "%s_%s" % (parts[-2], parts[-1])
        for t in tbl:
            if t['id'] == key.upper():
                return t
    if s.isdigit() and int(s) < len(tbl):
        return tbl[int(s)]
    hits = [t for t in tbl if t['name'].upper() == s.upper()]
    if hits:
        return hits[0]
    raise SystemExit("unknown track %r (try one of: %s)"
                     % (spec, ', '.join(t['id'] for t in tbl)))


def add_track_arg(parser):
    parser.add_argument("--track", default=None,
                        help="track id/dir/name (default $B3_TRACK or %s)"
                             % DEFAULT_TRACK)


def out_root(track, sub=""):
    """build/tracks/<ID>[/sub], created."""
    p = os.path.join(os.path.dirname(__file__), "..", "build", "tracks",
                     track['id'], sub)
    os.makedirs(p, exist_ok=True)
    return os.path.normpath(p)


# --------------------------------------------------------------------------
def _texture_prefixes(path):
    """Dominant texture-name prefix in a static.dat (independent check)."""
    import collections
    d = open(path, 'rb').read()
    cnt = struct.unpack_from('<H', d, 0x16)[0]
    tbl = struct.unpack_from('<I', d, 0x18)[0]
    pre = collections.Counter()
    for i in range(cnt):
        rec = struct.unpack_from('<I', d, tbl + i * 4)[0]
        if not (0 < rec < len(d) - 0x70):
            continue
        bd = struct.unpack_from('<I', d, rec + 0x40)[0]
        no = rec + (0x48 if bd in (4, 8, 32) else 0x44)
        end = d.find(b'\0', no)
        try:
            nm = d[no:end].decode('ascii')
        except (UnicodeDecodeError, AttributeError):
            continue
        if '_' in nm:
            pre[nm.split('_')[0].upper()] += 1
    return ', '.join("%s(%d)" % kv for kv in pre.most_common(3))


def main():
    verify = '--verify' in sys.argv
    tbl = track_table()
    print("Tracks/tlist.bin: %d tracks (version 4)\n" % len(tbl))
    print("%-3s %-9s %-24s %-6s %-5s %-5s %s"
          % ("#", "id", "name (Globalus)", "str", "flagA", "flagB",
             "directory" if not verify else "dominant texture prefixes"))
    for t in tbl:
        extra = os.path.relpath(t['dir'], GAME_DIR)
        if verify:
            sd = os.path.join(t['dir'], 'static.dat')
            extra = _texture_prefixes(sd) if os.path.exists(sd) else "(no static.dat)"
        print("%-3d %-9s %-24s %-6d %-5d %-5d %s"
              % (t['index'], t['id'], t['name'], t['name_str'],
                 t['flagA'], t['flagB'], extra))
    cur = resolve()
    print("\ndefault track: %s = %s  (%s)" % (cur['id'], cur['name'],
                                              cur['dir']))
    missing = [t['id'] for t in tbl if not os.path.isdir(t['dir'])]
    if missing:
        print("WARNING: tlist entries with no directory in this dump: %s"
              % ', '.join(missing))
    extra_dirs = []
    for reg in sorted(os.listdir(TRACKS_DIR)):
        rd = os.path.join(TRACKS_DIR, reg)
        if not os.path.isdir(rd):
            continue
        for sub in sorted(os.listdir(rd)):
            tid = "%s_%s" % (reg, sub)
            if not any(t['id'] == tid for t in tbl):
                extra_dirs.append(tid)
    if extra_dirs:
        print("NOTE: directories not listed in tlist.bin (not selectable "
              "in-game): %s" % ', '.join(extra_dirs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
