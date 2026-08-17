#!/usr/bin/env python3
"""extract_eatrax.py -- decode the EA TRAX soundtrack banks to build/music/.

The two licensed-soundtrack wave banks

    Tracks/_EATrax0.xwb   22 entries
    Tracks/_EATrax1.xwb   22 entries

are XACT v3 wave banks whose entries are all **wmav2, 2ch, 44100 Hz**
(docs/AUDIO_NOTES.md section 1 -- the format dword's 2-bit codec tag is 2 =
WMA and every payload starts with the ASF magic).  WMA cannot be decoded
with the Python stdlib, so each entry is carved out verbatim as a
standalone ASF file and handed to `ffmpeg`, which writes

    build/music/track_NN.wav      44100 Hz, MONO, 16-bit PCM

-- mono because the harness's SDL device is 44100/mono/s16 and
`b3_music_next_sample()` feeds it one sample at a time, so storing stereo
would only make the module average two channels 44100 times a second.

Also writes `build/music/eatrax.txt`, the manifest
`index bank wave frames rate sha1 artist | title | album`
that tools/validate_music.py checks the C table against.

The artist/title/album strings are NOT invented here: they are the game's
own, from the 44-entry song table at VA 0x003EC458 in the retail XBE
(24-byte stride, +0x00 title / +0x04 album / +0x08 artist as Globalus.bin
string indices).  See docs/RE_MUSIC.md section 1.  This file only carries
the *indices*; the text is resolved from build/Globalus.bin at run time so
the two sources can never silently disagree.

Usage:
    python3 tools/extract_eatrax.py [game-dir] [-o build/music] [-j 8]
                                    [--only 0,7,43] [--rate 44100]
                                    [--globalus build/Globalus.bin]
"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

DEFAULT_GAME = (game_root())

# --- the song table, VA 0x003EC458, 44 x 24 bytes ------------------------
# (title_id, album_id, artist_id) per entry, in table order.  Entries 0..39
# use the contiguous Globalus block 456..575; entries 40..43 were appended
# late in development and use 3255..3266.
SONG_TABLE_VA = 0x003EC458
SONG_STRIDE = 24
SONG_COUNT = 44
SONG_IDS = [(456 + 3 * i, 457 + 3 * i, 458 + 3 * i) for i in range(40)] + \
           [(3255 + 3 * i, 3256 + 3 * i, 3257 + 3 * i) for i in range(4)]

BANKS = ["_EATrax0.xwb", "_EATrax1.xwb"]
PER_BANK = 22


# --- Globalus.bin --------------------------------------------------------
def globalus_strings(path):
    blob = open(path, "rb").read()
    count = struct.unpack_from("<I", blob, 8)[0]

    def one(i):
        off = struct.unpack_from("<I", blob, 0x10 + i * 4)[0]
        end = off
        while end + 1 < len(blob) and blob[end:end + 2] != b"\0\0":
            end += 2
        return blob[off:end].decode("utf-16-le", "replace")
    return [one(i) for i in range(count)]


def song_list(globalus_path):
    """[(index, bank, wave, artist, title, album)] x 44, game order."""
    g = globalus_strings(globalus_path)
    out = []
    for i, (t, al, ar) in enumerate(SONG_IDS):
        out.append((i, i // PER_BANK, i % PER_BANK,
                    g[ar].strip(), g[t].strip(), g[al].strip()))
    return out


# --- XWB -----------------------------------------------------------------
def xwb_entries(path):
    """[(codec, channels, rate, offset, length)] for one XACT v3 bank."""
    d = open(path, "rb").read()
    if d[:4] != b"WBND":
        raise SystemExit("%s: not a WBND wave bank" % path)
    ver = struct.unpack_from("<I", d, 4)[0]
    if ver != 3:
        raise SystemExit("%s: unexpected XWB version %d" % (path, ver))
    segs = [struct.unpack_from("<II", d, 8 + i * 8) for i in range(4)]
    n = struct.unpack_from("<I", d, segs[0][0] + 4)[0]
    meta, data = segs[1][0], segs[3][0]
    out = []
    for i in range(n):
        _, fmt, off, ln, _, _ = struct.unpack_from("<6I", d, meta + i * 24)
        out.append((fmt & 3, (fmt >> 2) & 7, (fmt >> 5) & 0x3FFFF,
                    data + off, ln))
    return d, out


# --- one track -----------------------------------------------------------
def decode_one(job):
    (idx, bank, wave, artist, title, album,
     blob, ent, outdir, rate) = job
    codec, ch, srate, off, ln = ent
    if codec != 2:
        return (idx, None, "codec %d is not WMA" % codec)
    payload = blob[off:off + ln]
    if payload[:8] != b"\x30\x26\xB2\x75\x8E\x66\xCF\x11":
        return (idx, None, "payload is not ASF")

    dst = os.path.join(outdir, "track_%02d.wav" % idx)
    tmp = tempfile.NamedTemporaryFile(suffix=".wma", delete=False)
    try:
        tmp.write(payload)
        tmp.close()
        cmd = ["ffmpeg", "-v", "error", "-y", "-i", tmp.name,
               "-ac", "1", "-ar", str(rate), "-acodec", "pcm_s16le", dst]
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode != 0:
            return (idx, None, r.stderr.decode("utf-8", "replace")[:200])
    finally:
        os.unlink(tmp.name)

    wav = open(dst, "rb").read()
    if wav[:4] != b"RIFF" or wav[8:12] != b"WAVE":
        return (idx, None, "ffmpeg output is not RIFF/WAVE")
    frames = (len(wav) - 44) // 2
    # non-silence: peak over a 1 s window a third of the way in
    mid = 44 + (frames // 3) * 2
    win = wav[mid:mid + rate * 2]
    peak = max(abs(v) for v in struct.unpack("<%dh" % (len(win) // 2), win)) \
        if len(win) >= 2 else 0
    sha = hashlib.sha1(wav).hexdigest()[:12]
    return (idx, (bank, wave, frames, rate, peak, sha,
                  artist, title, album, srate), None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game", nargs="?", default=DEFAULT_GAME)
    ap.add_argument("-o", "--out", default="build/music")
    ap.add_argument("-j", "--jobs", type=int, default=8)
    ap.add_argument("--rate", type=int, default=44100)
    ap.add_argument("--only", default="",
                    help="comma-separated track indices (default: all 44)")
    ap.add_argument("--globalus", default="build/Globalus.bin")
    args = ap.parse_args()

    songs = song_list(args.globalus)
    want = set(range(SONG_COUNT))
    if args.only:
        want = set(int(x) for x in args.only.split(","))

    os.makedirs(args.out, exist_ok=True)

    jobs = []
    for b, name in enumerate(BANKS):
        path = os.path.join(args.game, "Tracks", name)
        if not os.path.exists(path):
            raise SystemExit("missing bank: " + path)
        blob, entries = xwb_entries(path)
        if len(entries) != PER_BANK:
            raise SystemExit("%s: expected %d entries, found %d"
                             % (name, PER_BANK, len(entries)))
        for w in range(PER_BANK):
            idx = b * PER_BANK + w
            if idx not in want:
                continue
            _, _, _, artist, title, album = songs[idx]
            jobs.append((idx, b, w, artist, title, album,
                         blob, entries[w], args.out, args.rate))

    print("EA TRAX: %d tracks -> %s  (44100->%d Hz, mono s16)"
          % (len(jobs), args.out, args.rate))
    results = {}
    bad = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for idx, info, err in pool.map(decode_one, jobs):
            if err:
                bad += 1
                print("  [%2d] FAIL  %s" % (idx, err))
            else:
                results[idx] = info

    print("%-3s %-4s %-4s %-8s %-6s %s" %
          ("#", "bank", "wave", "seconds", "peak", "artist -- title"))
    for idx in sorted(results):
        (bank, wave, frames, rate, peak, sha,
         artist, title, album, srate) = results[idx]
        secs = frames / float(rate)
        status = "" if (peak > 1000 and secs > 30.0) else "   << SUSPECT"
        print("%-3d %-4d %-4d %8.1f %6d  %s -- %s%s"
              % (idx, bank, wave, secs, peak, artist, title, status))
        if status:
            bad += 1

    man = os.path.join(args.out, "eatrax.txt")
    with open(man, "w") as f:
        f.write("# index bank wave frames rate sha1 | artist | title | album\n")
        for idx in sorted(results):
            (bank, wave, frames, rate, peak, sha,
             artist, title, album, srate) = results[idx]
            f.write("%d %d %d %d %d %s | %s | %s | %s\n"
                    % (idx, bank, wave, frames, rate, sha,
                       artist, title, album))
    print("manifest: %s" % man)
    print("%d/%d ok" % (len(results) - bad, len(jobs)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
