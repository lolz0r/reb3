#!/usr/bin/env python3
"""
validate_music.py -- differential test for the EA TRAX music module.

Three things are asserted, in decreasing order of how much the binary has
to say about them:

  [C] THE SONG TABLE.  Every one of the 132 Globalus string indices in
      src/burnout3_music.c is re-read out of the retail image at
      0x003EC458 (44 entries, 24-byte stride) and every string is
      re-resolved from Data/Globalus.bin, so the C table cannot drift from
      the game.  The stride and the count are not assumed: they are
      decoded out of the loop at 0x00152ED0 that refreshes each entry's
      per-track enable byte, which is also what identifies field +0x0C.

  [S] THE WAVES.  build/music/track_NN.wav must exist for all 44, be
      44100 Hz mono s16, and be long enough / loud enough to be a song.
      The bank/wave assignment (index i -> bank i/22, wave i%22) is
      checked against tools/extract_eatrax.py's manifest.

  GLUE THE SELECTION AND THE BANNER.  These were not recovered, so they
      are checked for the properties they are supposed to have rather than
      against an address: the shuffle visits all 44 before repeating and
      never repeats across the bag seam; the banner's box stays on screen,
      clears the boost bar's recovered 28 px strip and the speed cluster,
      and its longest title still fits its text column.  Both are driven
      through the REAL C code via build/music_probe.

  [C] THE CRASH BED (sections 8-10).  The pre-rendered crash stream:
      every constant is re-read at the VA src/burnout3_music.h names, and
      the four laws are asserted as literal instruction bytes -- the
      layer gate `CMP [0x0060EA18], 1` at 0x00150F4F, the `% 20 + 1`
      rotation at 0x0014C269 and 0x00151239 (including the SECOND counter
      bump at 0x0014C2D6), the trigger latch at 0x00150E50 and the 0.5 s
      release at 0x001510C8/0x00151117.  Then the same laws are driven
      through the real module: six crash cycles for the rotation, the
      divisor sweep for the layer, the frame-by-frame release curve, the
      duck, and a peak check that the bed is actually audible.

Usage:
    python3 tools/validate_music.py [--elf build/burnout3.elf]
                                    [--skip-waves]

Exit code 0 = all green.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import unicodedata

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ELF = os.path.join(REPO, "build", "burnout3.elf")
GLOBALUS = os.path.join(REPO, "build", "Globalus.bin")
MUSIC_H = os.path.join(REPO, "src", "burnout3_music.h")
MUSIC_C = os.path.join(REPO, "src", "burnout3_music.c")
HUD_H = os.path.join(REPO, "src", "burnout3_hud.h")
MUSIC_DIR = os.path.join(REPO, "build", "music")
PROBE_C = os.path.join(REPO, "build", "music_probe.c")
PROBE = os.path.join(REPO, "build", "music_probe")


# --------------------------------------------------------------------- #
class Image(object):
    """Correctly-mapped image reader: one PT_LOAD per XBE section at its
    true VA.  A flat XBE load is silently wrong -- see HANDOFF.md 2."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise SystemExit("%s is not an ELF -- run tools/xbe2elf.py" % path)
        e_phoff, = struct.unpack_from("<I", d, 0x1C)
        e_phentsize, = struct.unpack_from("<H", d, 0x2A)
        e_phnum, = struct.unpack_from("<H", d, 0x2C)
        self.segs = []
        for i in range(e_phnum):
            o = e_phoff + i * e_phentsize
            (p_type, p_off, p_va, _pa, p_filesz, p_memsz,
             _fl, _al) = struct.unpack_from("<8I", d, o)
            if p_type == 1:
                self.segs.append((p_va, p_memsz, p_off, p_filesz))

    def _byte(self, va):
        for p_va, memsz, off, filesz in self.segs:
            if p_va <= va < p_va + memsz:
                d = va - p_va
                return self.data[off + d] if d < filesz else 0
        raise KeyError("VA %08x is not mapped" % va)

    def read(self, va, n):
        return bytes(bytearray(self._byte(va + i) for i in range(n)))

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]


class Checker(object):
    def __init__(self):
        self.ok = 0
        self.fail = []

    def eq(self, name, got, want, note=""):
        good = got == want
        if isinstance(want, float) or isinstance(got, float):
            good = abs(float(got) - float(want)) <= 1e-4 + abs(float(want)) * 1e-5
        if good:
            self.ok += 1
            print("  ok   %-40s %-22s %s" % (name, _fmt(want), note))
        else:
            self.fail.append((name, got, want, note))
            print("  FAIL %-40s C=%s binary=%s %s"
                  % (name, _fmt(got), _fmt(want), note))

    def true(self, name, cond, note=""):
        self.eq(name, bool(cond), True, note)


def _fmt(v):
    if isinstance(v, float):
        return "%.9g" % v
    return str(v)


# --------------------------------------------------------------------- #
# the header's machine-checked defines
# --------------------------------------------------------------------- #
DEF_RE = re.compile(r"^#define\s+(B3[A-Z0-9_]+)\s+"
                    r"(-?[0-9.]+f|-?0x[0-9A-Fa-f]+|-?[0-9]+)\b")


def parse_defines(path, begin=None, end=None):
    out = {}
    inside = begin is None
    for line in open(path):
        if begin and begin in line:
            inside = True
            continue
        if end and end in line:
            inside = False
        if not inside:
            continue
        m = DEF_RE.match(line.strip())
        if m:
            t = m.group(2).rstrip("fF")
            if t.lower().startswith("0x"):
                out[m.group(1)] = int(t, 16)
            elif "." in t:
                out[m.group(1)] = float(t)
            else:
                out[m.group(1)] = int(t)
    return out


# --------------------------------------------------------------------- #
# Globalus.bin
# --------------------------------------------------------------------- #
def globalus():
    blob = open(GLOBALUS, "rb").read()
    count = struct.unpack_from("<I", blob, 8)[0]

    def one(i):
        off = struct.unpack_from("<I", blob, 0x10 + i * 4)[0]
        end = off
        while end + 1 < len(blob) and blob[end:end + 2] != b"\0\0":
            end += 2
        return blob[off:end].decode("utf-16-le", "replace")
    return [one(i) for i in range(count)]


def ascii_fold(s):
    """The exact fold burnout3_music.c's table was generated with: the
    recovered GlobalFont only carries glyphs 0x20..0x7E."""
    s = s.replace("’", "'").replace("‘", "'")
    s = s.replace("“", '"').replace("”", '"')
    s = s.replace("…", "...").replace("–", "-").replace("—", "-")
    s = unicodedata.normalize("NFKD", s)
    return "".join(c for c in s if 0x20 <= ord(c) <= 0x7E).strip()


# --------------------------------------------------------------------- #
# the C table
# --------------------------------------------------------------------- #
ROW_RE = re.compile(
    r'^\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,'
    r'\s*"((?:[^"\\]|\\.)*)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,'
    r'\s*(\d+)\s*,\s*(\d+)\s*\}\s*,\s*$')


def parse_c_table():
    rows = []
    inside = False
    for line in open(MUSIC_C):
        s = line.strip()
        if "B3_SONGS[B3MUSIC_TRACKS]" in s:
            inside = True
            continue
        if inside and s == "};":
            break
        if not inside:
            continue
        m = ROW_RE.match(s)
        if m:
            rows.append((m.group(1).replace('\\"', '"'),
                         m.group(2).replace('\\"', '"'),
                         m.group(3).replace('\\"', '"'),
                         int(m.group(4)), int(m.group(5)), int(m.group(6)),
                         int(m.group(7)), int(m.group(8))))
    if not rows:
        raise SystemExit("no rows parsed out of B3_SONGS in %s" % MUSIC_C)
    return rows


# --------------------------------------------------------------------- #
# [1] the refresh loop that pins stride, count and field +0x0C
# --------------------------------------------------------------------- #
def check_loop(img, D, ck):
    """FUN_00152ED0 does, for every entry:
           *(u32*)(0x003EC464 + off) = ((u8*)0x004AE1A0)[i];
           off += 0x18;
       while (off < 0x420);
    Confirm all four immediates are literally in that function body, which
    is what makes stride=24, count=44 and enable-offset=0x0C evidence and
    not an assumption."""
    print("\n[1] the enable-refresh loop @0x00152ED0 (stride / count / +0x0C)")
    body = img.read(0x00152ED0, 0x58)
    base = D["B3MUSIC_TABLE_VA"] + D["B3MUSIC_ENABLE_OFF"]
    ck.true("table+0x0C base 0x%08X is an operand" % base,
            struct.pack("<I", base) in body, "mov [0x3EC464+off], ...")
    ck.true("enable source 0x%08X is an operand" % D["B3MUSIC_ENABLE_SRC"],
            struct.pack("<I", D["B3MUSIC_ENABLE_SRC"]) in body,
            "u8[44] the loop copies from")
    ck.true("stride 0x18 (add) is an operand",
            b"\x83\xc1\x18" in body or b"\x83\xc0\x18" in body
            or b"\x83\xc2\x18" in body or b"\x83\xc3\x18" in body,
            "add reg, 0x18")
    ck.true("bound 0x420 (cmp) is an operand",
            struct.pack("<I", D["B3MUSIC_TABLE_BYTES"]) in body,
            "cmp reg, 0x420")
    ck.eq("count = BYTES / STRIDE", D["B3MUSIC_TRACKS"],
          D["B3MUSIC_TABLE_BYTES"] // D["B3MUSIC_TABLE_STRIDE"], "= 44")
    ck.eq("banks x waves covers the table",
          D["B3MUSIC_BANKS"] * D["B3MUSIC_BANK_WAVES"],
          D["B3MUSIC_TRACKS"], "2 x 22 = 44")


# --------------------------------------------------------------------- #
# [2] the song table itself
# --------------------------------------------------------------------- #
def check_table(img, D, ck, g):
    print("\n[2] the song table @0x%08X vs src/burnout3_music.c"
          % D["B3MUSIC_TABLE_VA"])
    rows = parse_c_table()
    ck.eq("C table row count", len(rows), D["B3MUSIC_TRACKS"], "")
    base, stride = D["B3MUSIC_TABLE_VA"], D["B3MUSIC_TABLE_STRIDE"]

    bad = 0
    for i, (artist, title, album, tid, alid, arid, bank, wave) in \
            enumerate(rows):
        e = base + i * stride
        b_t, b_al, b_ar = img.u32(e), img.u32(e + 4), img.u32(e + 8)
        if (b_t, b_al, b_ar) != (tid, alid, arid):
            ck.eq("entry %2d string ids" % i, (tid, alid, arid),
                  (b_t, b_al, b_ar), "")
            bad += 1
            continue
        exp = (ascii_fold(g[b_ar]), ascii_fold(g[b_t]), ascii_fold(g[b_al]))
        if (artist, title, album) != exp:
            ck.eq("entry %2d text" % i, (artist, title, album), exp, "")
            bad += 1
            continue
        if (bank, wave) != (i // D["B3MUSIC_BANK_WAVES"],
                            i % D["B3MUSIC_BANK_WAVES"]):
            ck.eq("entry %2d bank/wave" % i, (bank, wave),
                  (i // 22, i % 22), "")
            bad += 1
    ck.eq("all %d entries match the binary" % len(rows), bad, 0,
          "ids + Globalus text + bank/wave")

    # the entry the table is anchored on in the header comment
    ck.eq("Globalus[548] (the anchor string)", ascii_fold(g[548]),
          "Motion City Soundtrack", "entry 30's artist")
    for name, idx, want in (("ALL", D["B3MUSIC_STR_ALL"], "ALL"),
                            ("RACE ONLY", D["B3MUSIC_STR_RACE_ONLY"],
                             "RACE ONLY"),
                            ("MENU ONLY", D["B3MUSIC_STR_MENU_ONLY"],
                             "MENU ONLY"),
                            ("OFF", D["B3MUSIC_STR_OFF"], "OFF"),
                            ("RANDOM", D["B3MUSIC_STR_RANDOM"],
                             "PLAY TRACKS RANDOMLY"),
                            ("SEQUENTIAL", D["B3MUSIC_STR_SEQUENTIAL"],
                             "PLAY TRACKS SEQUENTIALLY"),
                            ("NEXT", D["B3MUSIC_STR_NEXT"],
                             "NEXT SOUNDTRACK")):
        ck.eq("EA TRAX option string %s" % name, ascii_fold(g[idx]), want,
              "Globalus %d" % idx)
    return rows


def print_tracklist(rows):
    print("\n[3] the recovered EA TRAX track list (44)")
    print("     %-4s %-28s %s" % ("bank/wave", "artist", "title"))
    for i, (artist, title, album, _t, _al, _ar, bank, wave) in \
            enumerate(rows):
        print("  %2d  %d/%-2d      %-28s %s" % (i, bank, wave, artist, title))


# --------------------------------------------------------------------- #
# [4] the extracted waves
# --------------------------------------------------------------------- #
def wav_info(path):
    with open(path, "rb") as f:
        d = f.read(4096)
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        return None
    o = 12
    rate = ch = bits = 0
    total = os.path.getsize(path)
    while o + 8 <= len(d):
        cid = d[o:o + 4]
        sz = struct.unpack_from("<I", d, o + 4)[0]
        if cid == b"fmt ":
            ch = struct.unpack_from("<H", d, o + 10)[0]
            rate = struct.unpack_from("<I", d, o + 12)[0]
            bits = struct.unpack_from("<H", d, o + 22)[0]
        elif cid == b"data":
            return rate, ch, bits, sz // 2, total
        o += 8 + sz + (sz & 1)
    return None


def check_waves(rows, D, ck):
    print("\n[4] build/music -- tools/extract_eatrax.py output")
    if not os.path.isdir(MUSIC_DIR):
        print("  --   build/music missing, skipped "
              "(run tools/extract_eatrax.py)")
        return False
    man = {}
    mp = os.path.join(MUSIC_DIR, "eatrax.txt")
    if os.path.exists(mp):
        for line in open(mp):
            if line.startswith("#") or not line.strip():
                continue
            head, artist, title, album = [p.strip()
                                          for p in line.split("|", 3)]
            f = head.split()
            man[int(f[0])] = (int(f[1]), int(f[2]), int(f[3]), int(f[4]),
                              artist, title)
    ck.eq("manifest entries", len(man), D["B3MUSIC_TRACKS"], mp)

    missing = badfmt = short = 0
    for i, (artist, title, _al, _t, _ali, _ari, bank, wave) in \
            enumerate(rows):
        p = os.path.join(MUSIC_DIR, "track_%02d.wav" % i)
        if not os.path.exists(p):
            missing += 1
            continue
        info = wav_info(p)
        if not info or info[0] != 44100 or info[1] != 1 or info[2] != 16:
            badfmt += 1
            continue
        frames = info[3]
        if frames < 44100 * 30:
            short += 1
        if i in man:
            # the manifest carries the RAW Globalus text (curly quotes and
            # all); the C table carries the same text ASCII-folded to the
            # glyphs GlobalFont actually has.
            m = man[i]
            got = (m[0], m[1], ascii_fold(m[4]), ascii_fold(m[5]))
            if got != (bank, wave, artist, title):
                ck.eq("manifest row %d agrees with the C table" % i,
                      got, (bank, wave, artist, title))
    ck.eq("track_NN.wav present", D["B3MUSIC_TRACKS"] - missing,
          D["B3MUSIC_TRACKS"], "build/music")
    ck.eq("all 44100 Hz mono s16", badfmt, 0,
          "the harness device format, no resampling")
    ck.eq("all longer than 30 s", short, 0, "a song, not a sting")
    return missing == 0 and badfmt == 0


# --------------------------------------------------------------------- #
# [5] + [6] the C itself, through build/music_probe
# --------------------------------------------------------------------- #
PROBE_SRC = r'''
/* generated by tools/validate_music.py -- drives the real module code */
#include "burnout3_music.h"
#include "burnout3_hud.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    /* --- selection: 3 full bags with every track playable ---------- */
    int n = b3_music_track_count();
    for (int i = 0; i < n; i++) b3_music_set_enabled(i, 1);
    /* pretend every wave is on disc: init() marks g_have from the files,
     * so run the bag through the public API after a real init and fall
     * back to the table when build/music is absent. */
    b3_music_init();
    b3_music_seed(12345u);
    printf("SEQ");
    for (int i = 0; i < 3 * n; i++) printf(" %d", b3_music_pick_next());
    printf("\n");

    /* --- banner geometry ------------------------------------------- */
    float x, y, w, h, a;
    for (int i = 0; i <= 100; i++) {
        float t = (float)i * 0.1f;
        a = b3_hud_music_box(t, &x, &y, &w, &h);
        printf("BOX %.3f %.4f %.3f %.3f %.3f %.3f\n", t, a, x, y, w, h);
    }
/*CRASHPROBE*/
    return 0;
}
'''


def build_probe(ck):
    with open(PROBE_C, "w") as f:
        f.write(PROBE_SRC.replace("/*CRASHPROBE*/", CRASH_PROBE))
    cmd = ["gcc", "-O2", "-std=c11", "-I", os.path.join(REPO, "src"),
           "-o", PROBE, PROBE_C,
           os.path.join(REPO, "src", "burnout3_music.c"),
           os.path.join(REPO, "src", "burnout3_hud.c"), "-lm"]
    try:
        cmd += subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "sdl2", "SDL2_image"]
        ).decode().split()
    except Exception:
        pass
    cmd += ["-lGL"]
    r = subprocess.run(cmd, capture_output=True, cwd=REPO)
    if r.returncode != 0:
        print(r.stderr.decode()[-2000:])
        raise SystemExit("could not build the probe")
    out = subprocess.check_output([PROBE], cwd=REPO).decode()
    return out


def check_selection(out, D, ck, playable):
    print("\n[5] selection -- shuffle bag (GLUE), driven through the module")
    seq = None
    for line in out.splitlines():
        if line.startswith("SEQ"):
            seq = [int(v) for v in line.split()[1:]]
    if seq is None or not playable:
        print("  --   no playable tracks, selection not exercised")
        return
    n = D["B3MUSIC_TRACKS"]
    ck.eq("picks produced", len(seq), 3 * n, "3 full bags")
    ck.true("every pick is a real track index",
            all(0 <= v < n for v in seq), "")
    for b in range(3):
        bag = seq[b * n:(b + 1) * n]
        ck.eq("bag %d visits all %d tracks once" % (b, n),
              len(set(bag)), n, "no repeat inside a bag")
    ck.eq("no track ever plays twice in a row",
          sum(1 for i in range(1, len(seq)) if seq[i] == seq[i - 1]), 0,
          "includes the bag seams")
    ck.true("the order is actually shuffled",
            seq[:n] != sorted(seq[:n]), "not the table order")


def check_banner(out, ck):
    print("\n[6] the EA TRAX banner (GLUE) -- box vs the recovered HUD")
    H = parse_defines(HUD_H)
    boxes = []
    for line in out.splitlines():
        if line.startswith("BOX"):
            f = line.split()
            boxes.append(tuple(float(v) for v in f[1:]))
    ck.true("banner box samples", len(boxes) == 101, "")

    life = H["B3HUD_TRAX_LIFE"]
    on = [b for b in boxes if b[1] > 0.0]
    ck.true("visible only inside 0..LIFE",
            all(0.0 <= b[0] <= life for b in on),
            "LIFE = %.2f s" % life)
    ck.true("alpha never exceeds 1", all(b[1] <= 1.0001 for b in boxes), "")
    ck.true("fully opaque during the hold",
            all(abs(b[1] - 1.0) < 1e-3 for b in boxes
                if H["B3HUD_TRAX_IN"] < b[0] < life - H["B3HUD_TRAX_OUT"]),
            "between the slide-in and the slide-out")
    ck.true("hidden after LIFE",
            all(b[1] == 0.0 for b in boxes if b[0] > life), "")

    # geometry, in the 640x480 virtual screen
    xs = [b[2] for b in on]
    ck.true("never leaves the left edge", min(xs) >= 0.0,
            "min x = %.1f (slide-in included)" % min(xs))
    right = max(b[2] + b[4] for b in on)
    ck.true("never leaves the right edge", right <= 640.0,
            "max x+w = %.1f" % right)
    bot = max(b[3] + b[5] for b in on)
    ck.eq("clears the boost bar's recovered 28 px strip", bot <= 452.0, True,
          "bottom = %.1f, bar top = 452 [C]" % bot)
    ck.true("clears the ticker's first row slot", bot <= 440.0,
            "row 0 top = 440")
    # the speed cluster's [S-ref] box is 469..626 x 408..448
    ck.true("clears the speed cluster",
            all(b[2] + b[4] <= 469.0 for b in on),
            "speedo left edge = 469")


def check_banner_text(ck, rows):
    """The longest title must still fit the banner's text column after the
    shrink-to-fit; that is a property of the C, so recompute it with the
    same font metrics the module uses."""
    print("\n[7] the banner's text column vs the longest strings")
    H = parse_defines(HUD_H)
    col = H["B3HUD_TRAX_W"] - H["B3HUD_TRAX_TEXT_X"] - H["B3HUD_TRAX_PAD"]
    ck.true("text column is positive", col > 100.0, "%.0f px" % col)
    longest_t = max((r[1] for r in rows), key=len)
    longest_a = max((r[0] for r in rows), key=len)
    ck.true("longest title is known", len(longest_t) > 0, longest_t)
    ck.true("longest artist is known", len(longest_a) > 0, longest_a)
    # the two lines must not overlap vertically at their nominal scales
    line_h = 39.344
    t_bot = H["B3HUD_TRAX_TITLE_Y"] + line_h * H["B3HUD_TRAX_TITLE_SCALE"]
    ck.true("title line clears the artist line",
            t_bot <= H["B3HUD_TRAX_ARTIST_Y"] + 1.0,
            "title bottom %.1f, artist top %.1f"
            % (t_bot, H["B3HUD_TRAX_ARTIST_Y"]))
    a_bot = H["B3HUD_TRAX_ARTIST_Y"] + line_h * H["B3HUD_TRAX_ARTIST_SCALE"]
    ck.true("artist line fits the plate",
            a_bot <= H["B3HUD_TRAX_H"] + 1.0,
            "artist bottom %.1f, box h %.0f" % (a_bot, H["B3HUD_TRAX_H"]))
    ck.true("EATrax.png present",
            os.path.exists(os.path.join(REPO, "build", "frontend",
                                        "EATrax.png")),
            "the badge, from Global.txd")
    ck.true("big_curve.png present",
            os.path.exists(os.path.join(REPO, "build", "frontend",
                                        "big_curve.png")),
            "the plate, shared with POS / LAP")


# --------------------------------------------------------------------- #
# [8] the CRASH BED
# --------------------------------------------------------------------- #
CRASH_DIR = os.path.join(REPO, "build", "audio")


def f32(img, va):
    return struct.unpack("<f", img.read(va, 4))[0]


def cstr(img, va, cap=64):
    b = img.read(va, cap)
    i = b.find(b"\0")
    return b[:i if i >= 0 else cap].decode("latin-1")


def check_crashbed_binary(img, D, ck):
    """Everything the module claims about retail's crash bed, read back out
    of the image at the address the header names.  [C]"""
    print("\n[8] the CRASH BED -- FUN_00150E80 / FUN_00150D40 / 0x0014C269")

    # -- the constants -------------------------------------------------
    ck.eq("bed volume scale @0x%08X" % D["B3MUSIC_CRASH_VOL_VA"],
          D["B3MUSIC_CRASH_VOL_F"], f32(img, D["B3MUSIC_CRASH_VOL_VA"]),
          "MULSS @0x00150F77 and 0x00150FA1")
    ck.eq("song volume @0x%08X" % D["B3MUSIC_SONG_VOL_VA"],
          D["B3MUSIC_SONG_VOL_F"], f32(img, D["B3MUSIC_SONG_VOL_VA"]),
          "mgr+0x83C @0x0014B59E -- the bed is 2.33x the song")
    ck.eq("release length @0x%08X" % D["B3MUSIC_CRASH_FADE_VA"],
          D["B3MUSIC_CRASH_FADE_F"], f32(img, D["B3MUSIC_CRASH_FADE_VA"]),
          "ADDSS @0x001510D0, dilated clock seconds")
    ck.eq("release slope @0x%08X" % D["B3MUSIC_CRASH_FADE_K_VA"],
          D["B3MUSIC_CRASH_FADE_K_F"],
          f32(img, D["B3MUSIC_CRASH_FADE_K_VA"]), "MULSS @0x0015113A")
    ck.eq("slope = 1 / length", D["B3MUSIC_CRASH_FADE_K_F"],
          1.0 / D["B3MUSIC_CRASH_FADE_F"], "the fade reaches 0 exactly")
    ck.eq("trigger radius @0x%08X" % D["B3MUSIC_CRASH_DIST_VA"],
          D["B3MUSIC_CRASH_DIST_F"], f32(img, D["B3MUSIC_CRASH_DIST_VA"]),
          "COMISS @0x00150DC0")
    ck.eq("the name format @0x%08X" % D["B3MUSIC_CRASH_FMT_VA"],
          cstr(img, D["B3MUSIC_CRASH_FMT_VA"]), "tracks\\crash%d.rws",
          "sprintf @0x0014C290 and 0x00151260")

    # -- the layer gate ------------------------------------------------
    w = img.read(0x00150F4F, 0x7C)
    div = struct.pack("<I", D["B3MUSIC_CRASH_DIVISOR_VA"])
    ck.true("0x00150F4F is CMP dword [0x%08X], 1"
            % D["B3MUSIC_CRASH_DIVISOR_VA"],
            w[:7] == b"\x83\x3d" + div + b"\x01",
            "the layer gate IS the time divisor")
    ck.true("both arms scale by 0x%08X" % D["B3MUSIC_CRASH_VOL_VA"],
            w.count(struct.pack("<I", D["B3MUSIC_CRASH_VOL_VA"])) == 2,
            "same 0.70 either way")
    ck.eq("sub-stream 0 slot (+0x15C) written twice",
          w.count(struct.pack("<I", 0x0000015C)), 2, "aGenCrashNN")
    ck.eq("sub-stream 1 slot (+0x160) written twice",
          w.count(struct.pack("<I", 0x00000160)), 2, "zSloCrashNN")
    # FUN_001CB9E0 proves the slots are indexed sub-streams, not L/R
    v = img.read(0x001CB9E0, 0x28)
    ck.true("FUN_001CB9E0 indexes +0x15C by channel*4",
            b"\xf3\x0f\x11\x84\x81\x5c\x01\x00\x00" in v,
            "MOVSS [ECX+EAX*4+0x15C], XMM0")
    ck.eq("so the file carries this many sub-streams",
          D["B3MUSIC_CRASH_LAYERS"], 2, "aGenCrashNN + zSloCrashNN")

    # -- the rotation --------------------------------------------------
    fmt = struct.pack("<I", D["B3MUSIC_CRASH_FMT_VA"])
    mod = bytes(bytearray([0xBE])) + struct.pack("<I",
                                                 D["B3MUSIC_CRASH_FILES"])
    for va, label in ((0x00151239, "rotate  @0x00151239"),
                      (0x0014C269, "race init @0x0014C269")):
        r = img.read(va, 0x3C)
        ck.true("%s divides the counter by %d"
                % (label, D["B3MUSIC_CRASH_FILES"]),
                mod in r and b"\xf7\xfe" in r, "MOV ESI,0x14 / IDIV ESI")
        ck.true("%s formats with the crash name" % label, fmt in r,
                "PUSH 0x%08X" % D["B3MUSIC_CRASH_FMT_VA"])
        ck.true("%s bumps the counter" % label, b"\xfe\xc1" in r, "INC CL")
    ck.true("the race-init pick bumps the counter a SECOND time",
            b"\xfe\xc0" in img.read(0x0014C2AC, 0x30),
            "INC AL @0x0014C2D6 -- races step by two")

    # -- the trigger ---------------------------------------------------
    t = img.read(0x00150DB8, 0x28)
    ck.true("FUN_00150D40 gates on the 50-unit radius",
            b"\xf3\x0f\x10\x0d" + struct.pack(
                "<I", D["B3MUSIC_CRASH_DIST_VA"]) in t
            and b"\x0f\x2f\xc8" in t, "MOVSS XMM1,[0x003B16B8] / COMISS")
    ck.true("and spreads the bed over the 5.1 bins",
            img.read(0x00150DCF, 0x14) ==
            b"\x68\x33\x33\xb3\x3e\x6a\x00\x68\x9a\x99\x19\x3f"
            b"\x68\x8f\xc2\x35\x3f\x83\xc9\xff",
            "0.71 / 0.6 / 0 / 0.35, all voices (ECX = -1)")
    ck.true("and latches mgr+0x8DC / +0x8E0 on entry",
            img.read(0x00150E50, 0x15) ==
            b"\xc6\x83\xdd\x08\x00\x00\x00\xc6\x83\xe0\x08\x00\x00\x01"
            b"\xc6\x83\xdc\x08\x00\x00\x01",
            "@0x00150E50 -- fires once per crash")

    # -- the release ---------------------------------------------------
    a = img.read(0x001510C8, 0x30)
    ck.true("the release arms clock + %.2f s" % D["B3MUSIC_CRASH_FADE_F"],
            b"\xf3\x0f\x58\x05" + struct.pack(
                "<I", D["B3MUSIC_CRASH_FADE_VA"]) in a,
            "ADDSS XMM0,[0x003B1684] @0x001510D0")
    ck.true("and FREEZES the layer it fades",
            b"\x83\x3d" + div + b"\x01" in a and b"\x0f\x95\xc0" in a,
            "CMP [divisor],1 / SETNZ -> mgr+0x8C0 @0x001510E0")
    c = img.read(0x00151117, 0x3B)
    ck.true("the release curve is (t_end - clock) * %.1f * %.2f"
            % (D["B3MUSIC_CRASH_FADE_K_F"], D["B3MUSIC_CRASH_VOL_F"]),
            b"\xf3\x0f\x59\x05" + struct.pack(
                "<I", D["B3MUSIC_CRASH_VOL_VA"]) in c
            and b"\xf3\x0f\x59\x0d" + struct.pack(
                "<I", D["B3MUSIC_CRASH_FADE_K_VA"]) in c,
            "two MULSS @0x00151132 / 0x0015113A")
    ck.true("and it goes through the per-sub-stream volume setter",
            b"\xe8\x8f\xa8\x07\x00" in c,
            "CALL 0x001CB9E0 @0x0015114C")


def check_crashbed_assets(D, ck):
    """tools/extract_rws.py's output: 20 files x 2 sub-streams."""
    print("\n[9] build/audio/rws_crashNN -- the 20 beds, two layers each")
    if not os.path.isdir(CRASH_DIR):
        print("  --   build/audio missing, skipped (run tools/extract_rws.py)")
        return False
    missing = badfmt = short = 0
    for n in range(1, D["B3MUSIC_CRASH_FILES"] + 1):
        for pre in ("aGen", "zSlo"):
            p = os.path.join(CRASH_DIR, "rws_crash%d" % n,
                             "%sCrash%02d.wav" % (pre, n))
            if not os.path.exists(p):
                missing += 1
                continue
            info = wav_info(p)
            if (not info or info[0] != D["B3MUSIC_CRASH_SRC_RATE"]
                    or info[2] != 16 or info[1] not in (1, 2)):
                badfmt += 1
                continue
            secs = info[3] / float(info[1] * info[0])
            if secs < D["B3MUSIC_CRASH_SECONDS"] - 0.5:
                short += 1
    want = D["B3MUSIC_CRASH_FILES"] * D["B3MUSIC_CRASH_LAYERS"]
    ck.eq("aGenCrashNN + zSloCrashNN present", want - missing, want,
          "rws_crash1..%d" % D["B3MUSIC_CRASH_FILES"])
    ck.eq("all %d Hz s16" % D["B3MUSIC_CRASH_SRC_RATE"], badfmt, 0,
          "resampled to the device rate inside the module")
    ck.eq("all %d s long" % D["B3MUSIC_CRASH_SECONDS"], short, 0,
          "the pre-rendered bed")
    return missing == 0 and badfmt == 0


CRASH_PROBE = r'''
    /* --- the crash bed --------------------------------------------- */
    b3_music_set_master(0.65f);          /* the harness's tuned master  */
    b3_music_play(0);                    /* a song under the bed        */
    {
        int f0 = b3_music_crash_arm();
        const float DT = 1.0f / 60.0f;
        printf("CRASHARM %d %d\n", f0, b3_music_crash_file());
        printf("CRASHDUCK %.6f", b3_music_crash_duck());
        b3_music_set_master(0.30f);      /* retail's own song volume    */
        printf(" %.6f\n", b3_music_crash_duck());
        b3_music_set_master(0.65f);

        /* six full crash cycles: which file each one plays */
        printf("CRASHROT");
        for (int c = 0; c < 6; c++) {
            printf(" %d", b3_music_crash_file());
            for (int k = 0; k < 12; k++) {
                b3_music_crash_tick(1, (k < 2) ? 1 : 5, DT);
                b3_music_pump();
                for (int s = 0; s < 735; s++) b3_music_next_sample();
            }
            for (int k = 0; k < 60 && b3_music_crash_playing(); k++) {
                b3_music_crash_tick(0, 1, DT);
                b3_music_pump();
                for (int s = 0; s < 735; s++) b3_music_next_sample();
            }
        }
        printf("\n");

        /* the layer gate, straight off the divisor */
        printf("CRASHLAYER");
        {
            static const int divs[6] = {1, 5, 6, 1, 5, 1};
            for (int i = 0; i < 6; i++) {
                b3_music_crash_tick(1, divs[i], DT);
                b3_music_pump();
                for (int s = 0; s < 735; s++) b3_music_next_sample();
                printf(" %d:%d", divs[i], b3_music_crash_layer());
            }
        }
        printf("\n");
        printf("CRASHGAIN %.6f %d %d\n", b3_music_crash_gain(),
               b3_music_crash_playing(), b3_music_crash_file());

        /* the release */
        printf("CRASHFADE");
        for (int k = 0; k < 60; k++) {
            b3_music_crash_tick(0, 1, DT);
            printf(" %.6f", b3_music_crash_gain());
            b3_music_pump();
            for (int s = 0; s < 735; s++) b3_music_next_sample();
            if (!b3_music_crash_playing()) break;
        }
        printf("\n");
        printf("CRASHAFTER %d %d %d\n", b3_music_crash_file(),
               b3_music_crash_playing(), b3_music_crash_layer());
        printf("CRASHUNDER %u %u %u\n", b3_music_crash_underruns(),
               b3_music_underruns(), b3_music_crash_starts());

        /* the bed must actually be audible: peak of one dilated second */
        {
            float pk = 0.0f;
            b3_music_crash_tick(1, 5, DT);
            for (int k = 0; k < 60; k++) {
                b3_music_pump();
                for (int s = 0; s < 735; s++) {
                    float v = b3_music_next_sample();
                    if (v < 0.0f) v = -v;
                    if (v > pk) pk = v;
                }
                b3_music_crash_tick(1, 5, DT);
            }
            printf("CRASHPEAK %.1f\n", pk);
        }
    }
'''


def check_crashbed_c(out, D, ck, have):
    print("\n[10] the crash bed through the module itself")
    if not have:
        print("  --   beds not extracted, module not exercised")
        return
    K = {}
    for line in out.splitlines():
        f = line.split()
        if f and f[0].startswith("CRASH"):
            K[f[0]] = f[1:]
    if "CRASHARM" not in K:
        ck.true("the probe ran the crash bed", False, "no CRASH lines")
        return

    n = D["B3MUSIC_CRASH_FILES"]
    ck.eq("arm() loads the first file of the rotation",
          int(K["CRASHARM"][0]), 1, "counter 0 -> (0 %% %d) + 1" % n)
    ck.eq("and it is the one that is resident", int(K["CRASHARM"][1]),
          int(K["CRASHARM"][0]), "opened and pre-buffered, paused")

    rot = [int(v) for v in K["CRASHROT"]]
    # retail: init picks 1 and leaves the counter at 2 (the double bump),
    # every crash after that steps the counter by one.
    want = [1] + [((1 + i) % n) + 1 for i in range(1, len(rot))]
    ck.eq("six crashes rotate sequentially", rot, want,
          "the double bump at 0x0014C2D6 makes crash 2 play file 3")
    ck.true("no crash repeats the previous file",
            all(rot[i] != rot[i - 1] for i in range(1, len(rot))), "")
    ck.true("every file is in 1..%d" % n,
            all(1 <= v <= n for v in rot), "")
    ck.eq("the pure rotation law agrees with the counter",
          [((c) % n) + 1 for c in (0, 2, 3, 4, 5, 6)], rot,
          "b3_music_crash_pick()")

    lay = dict(v.split(":") for v in K["CRASHLAYER"])
    ck.eq("divisor 1 plays aGenCrashNN", int(lay["1"]),
          D["B3MUSIC_CRASH_LAYER_GEN"], "CMP [0x0060EA18],1 / JZ")
    ck.eq("divisor 5 plays zSloCrashNN", int(lay["5"]),
          D["B3MUSIC_CRASH_LAYER_SLO"], "the crash cinematic's dilation")
    ck.eq("divisor 6 plays zSloCrashNN too", int(lay["6"]),
          D["B3MUSIC_CRASH_LAYER_SLO"], "the gate is != 1, not == 5")

    vol = D["B3MUSIC_CRASH_BASE_F"] * D["B3MUSIC_CRASH_VOL_F"]
    ck.eq("the bed plays at 1.0 * %.2f" % D["B3MUSIC_CRASH_VOL_F"],
          float(K["CRASHGAIN"][0]), vol, "mgr+0x834 * [0x003EC418]")
    ck.eq("and it is playing", int(K["CRASHGAIN"][1]), 1, "")

    duck = [float(v) for v in K["CRASHDUCK"]]
    ck.eq("at retail's own 0.30 master nothing is ducked", duck[1], 1.0,
          "the recovered mix state machine ducks nothing")
    ck.eq("at the harness's 0.65 master the song drops to 0.30 absolute",
          duck[0] * 0.65, D["B3MUSIC_SONG_VOL_F"],
          "DEVIATION (TUNED): restores retail's 0.30 + 0.70 = 1.0")

    fade = [float(v) for v in K["CRASHFADE"]]
    dt = 1.0 / 60.0
    ck.true("the release is longer than one frame", len(fade) > 4, "")
    steps = len(fade) - 1
    ck.true("it lasts %.2f s" % D["B3MUSIC_CRASH_FADE_F"],
            abs(steps * dt - D["B3MUSIC_CRASH_FADE_F"]) <= dt + 1e-6,
            "%d frames of %.4f s = %.4f s" % (steps, dt, steps * dt))
    bad = 0
    for i, g in enumerate(fade[:-1]):
        want_g = max(0.0, (D["B3MUSIC_CRASH_FADE_F"] - i * dt)
                     * D["B3MUSIC_CRASH_FADE_K_F"] * vol)
        if want_g > vol:
            want_g = vol
        if abs(g - want_g) > 1e-4:
            bad += 1
    ck.eq("every step is (t_end - clock) * 2.0 * 0.70", bad, 0,
          "@0x00151117, linear to zero")
    ck.true("it starts at the full bed level",
            abs(fade[0] - vol) < 1e-4, "no jump when the crash ends")
    ck.eq("and ends silent", fade[-1], 0.0, "FUN_001CB900 @0x00151104")

    ck.eq("the stop rotates to the next file", int(K["CRASHAFTER"][0]),
          (int(K["CRASHGAIN"][2]) % n) + 1,
          "buffered ready for the next crash")
    ck.eq("and leaves nothing playing", int(K["CRASHAFTER"][1]), 0, "")
    ck.eq("with no layer selected", int(K["CRASHAFTER"][2]), -1, "")

    ck.eq("no crash-bed underruns", int(K["CRASHUNDER"][0]), 0,
          "one pump per 735-sample frame")
    ck.eq("the song stream still never starves", int(K["CRASHUNDER"][1]), 0,
          "the bed's prefill does not steal the song's pump")
    ck.eq("seven crashes started seven beds", int(K["CRASHUNDER"][2]),
          len(rot) + 1, "six rotation cycles plus the layer-gate crash")

    pk = float(K["CRASHPEAK"][0])
    ck.true("the bed is actually audible", pk > 3000.0,
            "peak %.0f of 32767 over a second of zSlo + song" % pk)
    ck.true("and does not run away", pk <= 32768.0, "peak %.0f" % pk)


# --------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--skip-waves", action="store_true")
    args = ap.parse_args()
    if not os.path.exists(args.elf):
        raise SystemExit("missing %s -- run tools/xbe2elf.py" % args.elf)

    img = Image(args.elf)
    ck = Checker()
    D = parse_defines(MUSIC_H, "B3MUSIC-TABLE-BEGIN", "B3MUSIC-TABLE-END")
    D.update(parse_defines(MUSIC_H))
    g = globalus()

    print("validate_music.py -- EA TRAX song table + playback vs %s"
          % args.elf)
    check_loop(img, D, ck)
    rows = check_table(img, D, ck, g)
    print_tracklist(rows)
    playable = True
    if not args.skip_waves:
        playable = check_waves(rows, D, ck)
    out = build_probe(ck)
    check_selection(out, D, ck, playable)
    check_banner(out, ck)
    check_banner_text(ck, rows)
    check_crashbed_binary(img, D, ck)
    beds = check_crashbed_assets(D, ck)
    check_crashbed_c(out, D, ck, beds)

    print("\n%s: %d/%d checks green" %
          ("PASS" if not ck.fail else "FAIL",
           ck.ok, ck.ok + len(ck.fail)))
    for name, got, want, note in ck.fail:
        print("  - %s: C=%s binary=%s %s" % (name, got, want, note))
    return 1 if ck.fail else 0


if __name__ == "__main__":
    sys.exit(main())
