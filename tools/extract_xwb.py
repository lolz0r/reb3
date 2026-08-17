#!/usr/bin/env python3
"""Extract Microsoft XACT wave banks (.xwb) as used by Burnout 3: Takedown (Xbox).

Format (XWB version 3, XACT1 era, verified against the game's 34 banks):

  0x00  char[4]  "WBND"
  0x04  u32      version (3 in all B3 files)
  0x08  4 x {u32 offset, u32 length}   segment table:
                   seg0 BANKDATA      (0x28, len 0x28)
                   seg1 ENTRYMETADATA (0x50, len 24*n)
                   seg2 ENTRYNAMES    (0,0 in every B3 bank -- no names stored)
                   seg3 ENTRYWAVEDATA (0x800, ...)

  BANKDATA:  u32 flags(=1 streaming), u32 entryCount, char[16] bankName,
             u32 entryMetaElemSize(=24), u32 entryNameElemSize(=64),
             u32 alignment(=0x800), u32 pad/compact(=0)

  ENTRY (24 bytes): u32 flagsAndDuration(=0), u32 format,
             u32 playOffset (relative to seg3), u32 playLength,
             u32 loopStart, u32 loopLength

  format dword bit-packing (WAVEBANKMINIWAVEFORMAT, XACT1 layout -- verified
  empirically: decoding yields 48000/44100 Hz stereo and the payloads of
  tag==2 entries begin with the ASF/WMA magic 30 26 B2 75):
             tag      = bits 0-1    0=PCM, 1=Xbox ADPCM, 2=WMA
             channels = bits 2-4
             rate     = bits 5-22
             align    = bits 23-30  (0 for WMA entries)
             bits16   = bit  31     (1 = 16-bit for PCM)

  Every entry in every Burnout 3 bank is tag==2 (WMA): the play region is a
  complete standalone ASF file.  PCM / Xbox ADPCM handling is implemented for
  completeness but is *unexercised* by this game's data.

WMA cannot be decoded with the Python stdlib; entries are dumped as .wma and,
if ffmpeg is on PATH (optional), also converted to NNN.wav and validated
(RMS, non-constant, rate/channel cross-check).

References: layout cross-checked against community notes on Luigi Auriemma's
unxwb and MonoGame's XWB reader (both primarily document v42+; the v3 4-segment
layout and format packing above were verified byte-by-byte against these files).
"""
import argparse
import math
import os
import shutil
import struct
import subprocess
import sys
import wave

WBND = b"WBND"
ASF_MAGIC = bytes.fromhex("3026b2758e66cf11a6d900aa0062ce6c")
# ASF File Properties Object GUID (for duration parsing)
ASF_FILE_PROPS = bytes.fromhex("a1dcab8c47a9cf118ee400c00c205365")

TAG_PCM, TAG_XBOX_ADPCM, TAG_WMA = 0, 1, 2
TAG_NAMES = {TAG_PCM: "pcm", TAG_XBOX_ADPCM: "xadpcm", TAG_WMA: "wma"}

# ---------------------------------------------------------------- helpers

IMA_INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
IMA_STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767]


def decode_xbox_adpcm(data, channels):
    """Decode Xbox ADPCM (WAVE_FORMAT_XBOX_ADPCM 0x0069, an IMA-ADPCM variant
    with a 36-byte-per-channel block: 4-byte header {s16 predictor, u8 stepIdx,
    u8 pad} per channel, then 4-byte nibble groups round-robin per channel).
    Returns interleaved s16le bytes.  NOTE: no Burnout 3 file actually uses
    this codec (everything is PCM or WMA); this path is untested against real
    game data and follows the standard IMA-WAV/Xbox layout."""
    block = 36 * channels
    out = []
    for boff in range(0, len(data) - block + 1, block):
        preds, idxs = [], []
        for c in range(channels):
            pred, idx = struct.unpack_from("<hBx", data, boff + 4 * c)
            preds.append(pred)
            idxs.append(min(max(idx, 0), 88))
            out.append(pred)  # header predictor is emitted as first sample
        # remaining 32*ch bytes: 4-byte groups alternating channels
        pos = boff + 4 * channels
        nsamp = [0] * channels
        chans = [[] for _ in range(channels)]
        while pos < boff + block:
            for c in range(channels):
                grp = data[pos:pos + 4]
                pos += 4
                for b in grp:
                    for nib in (b & 0xF, b >> 4):
                        step = IMA_STEP_TABLE[idxs[c]]
                        diff = step >> 3
                        if nib & 1:
                            diff += step >> 2
                        if nib & 2:
                            diff += step >> 1
                        if nib & 4:
                            diff += step
                        if nib & 8:
                            diff = -diff
                        preds[c] = min(max(preds[c] + diff, -32768), 32767)
                        idxs[c] = min(max(idxs[c] + IMA_INDEX_TABLE[nib], 0), 88)
                        chans[c].append(preds[c])
        # interleave the 64 nibble samples per channel
        for i in range(len(chans[0])):
            for c in range(channels):
                out.append(chans[c][i])
    return struct.pack("<%dh" % len(out), *out)


def write_wav(path, pcm, rate, channels, sampwidth=2):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(sampwidth)
        w.setframerate(rate)
        w.writeframes(pcm)


def pcm16_stats(pcm):
    """Return (rms, non_constant) for s16le bytes."""
    n = len(pcm) // 2
    if n == 0:
        return 0.0, False
    samples = struct.unpack("<%dh" % n, pcm[:n * 2])
    acc = 0
    lo, hi = samples[0], samples[0]
    for s in samples:
        acc += s * s
        if s < lo:
            lo = s
        if s > hi:
            hi = s
    return math.sqrt(acc / n), hi != lo


def asf_duration(data):
    """Parse ASF File Properties Object -> playback duration in seconds
    (play duration minus preroll), or None."""
    if data[:16] != ASF_MAGIC:
        return None
    hdr_size, = struct.unpack_from("<Q", data, 16)
    nobj, = struct.unpack_from("<I", data, 24)
    pos = 30
    for _ in range(nobj):
        if pos + 24 > len(data) or pos + 24 > hdr_size + 30:
            break
        guid = data[pos:pos + 16]
        osize, = struct.unpack_from("<Q", data, pos + 16)
        if osize < 24:
            break
        if guid == ASF_FILE_PROPS:
            play_100ns, = struct.unpack_from("<Q", data, pos + 64)
            preroll_ms, = struct.unpack_from("<Q", data, pos + 80)
            return max(play_100ns / 1e7 - preroll_ms / 1e3, 0.0)
        pos += osize
    return None


def find_ffmpeg():
    return shutil.which("ffmpeg")


def wma_to_wav(ffmpeg, wma_path, wav_path):
    """Decode a dumped .wma to s16le .wav.  Returns (rate, channels, pcm) or None."""
    r = subprocess.run(
        [ffmpeg, "-v", "error", "-y", "-i", wma_path,
         "-acodec", "pcm_s16le", wav_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0 or not os.path.exists(wav_path):
        return None
    with wave.open(wav_path, "rb") as w:
        if w.getsampwidth() != 2:
            return None
        return w.getframerate(), w.getnchannels(), w.readframes(w.getnframes())


# ---------------------------------------------------------------- xwb parser

class XwbEntry:
    __slots__ = ("index", "name", "tag", "channels", "rate", "align",
                 "bits16", "offset", "length", "loop_start", "loop_len")


def parse_xwb(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != WBND:
        raise ValueError("not an XWB (missing WBND magic)")
    version, = struct.unpack_from("<I", data, 4)
    if version > 4:
        raise ValueError("XWB version %d not supported (only v<=4, 4-segment "
                         "XACT1 layout; Burnout 3 uses v3)" % version)
    segs = [struct.unpack_from("<II", data, 8 + 8 * i) for i in range(4)]
    bd_off, bd_len = segs[0]
    bank = data[bd_off:bd_off + bd_len]
    flags, count = struct.unpack_from("<II", bank, 0)
    bank_name = bank[8:24].split(b"\0")[0].decode("ascii", "replace")
    meta_size, name_size, alignment = struct.unpack_from("<III", bank, 24)
    if meta_size != 24:
        raise ValueError("unexpected entry meta size %d" % meta_size)

    names = []
    ne_off, ne_len = segs[2]
    if ne_len:
        for i in range(count):
            raw = data[ne_off + i * name_size: ne_off + (i + 1) * name_size]
            names.append(raw.split(b"\0")[0].decode("ascii", "replace"))

    me_off, _ = segs[1]
    wd_off, wd_len = segs[3]
    entries = []
    for i in range(count):
        fd, fmt, off, length, ls, ll = struct.unpack_from(
            "<IIIIII", data, me_off + i * meta_size)
        e = XwbEntry()
        e.index = i
        e.name = names[i] if names else None
        e.tag = fmt & 3
        e.channels = (fmt >> 2) & 7
        e.rate = (fmt >> 5) & 0x3FFFF
        e.align = (fmt >> 23) & 0xFF
        e.bits16 = (fmt >> 31) & 1
        e.offset = off
        e.length = length
        e.loop_start, e.loop_len = ls, ll
        entries.append(e)
    return dict(version=version, flags=flags, bank_name=bank_name,
                alignment=alignment, entries=entries,
                wavedata=(wd_off, wd_len), raw=data)


def extract_bank(path, out_root, ffmpeg=None):
    info = parse_xwb(path)
    data = info["raw"]
    wd_off, wd_len = info["wavedata"]
    bank_dir = os.path.join(out_root, info["bank_name"])
    os.makedirs(bank_dir, exist_ok=True)

    print("\n== %s  (bank '%s', xwb v%d, %d entries) ==" %
          (path, info["bank_name"], info["version"], len(info["entries"])))
    print("%-4s %-12s %-7s %-6s %-3s %8s %8s  %s" %
          ("idx", "name", "codec", "rate", "ch", "dur(s)", "rms", "status"))

    n_ok = n_fail = 0
    for e in info["entries"]:
        payload = data[wd_off + e.offset: wd_off + e.offset + e.length]
        name = e.name or ("%03d" % e.index)
        codec = TAG_NAMES.get(e.tag, "tag%d" % e.tag)
        status, dur, rms = [], None, None

        if not (8000 <= e.rate <= 48000):
            status.append("BAD-RATE")
        if e.channels not in (1, 2, 4, 6):
            status.append("BAD-CH")
        if len(payload) != e.length:
            status.append("TRUNCATED")

        if e.tag == TAG_WMA:
            if payload[:16] != ASF_MAGIC:
                status.append("NO-ASF-MAGIC")
            else:
                dur = asf_duration(payload)
            wma_path = os.path.join(bank_dir, "%03d.wma" % e.index)
            with open(wma_path, "wb") as f:
                f.write(payload)
            if ffmpeg and not status:
                wav_path = os.path.join(bank_dir, "%03d.wav" % e.index)
                dec = wma_to_wav(ffmpeg, wma_path, wav_path)
                if dec is None:
                    status.append("FFMPEG-FAIL")
                else:
                    drate, dch, pcm = dec
                    rms, nonconst = pcm16_stats(pcm)
                    if drate != e.rate:
                        status.append("RATE-MISMATCH(%d)" % drate)
                    if dch != e.channels:
                        status.append("CH-MISMATCH(%d)" % dch)
                    if dur is not None:
                        got = len(pcm) / (2 * dch * drate)
                        if abs(got - dur) > max(0.5, dur * 0.05):
                            status.append("DUR-MISMATCH(%.2f!=%.2f)" % (got, dur))
                    if rms < 1.0 or not nonconst:
                        status.append("SILENT")
            elif not ffmpeg:
                status.append("(wma dumped; no ffmpeg -> no PCM check)")
        elif e.tag == TAG_XBOX_ADPCM:
            pcm = decode_xbox_adpcm(payload, e.channels)
            expect = (e.length // (36 * e.channels)) * 65 * e.channels
            if len(pcm) // 2 != expect:
                status.append("LEN-MISMATCH")
            rms, nonconst = pcm16_stats(pcm)
            dur = len(pcm) / (2 * e.channels * e.rate)
            if rms < 1.0 or not nonconst:
                status.append("SILENT")
            write_wav(os.path.join(bank_dir, "%03d.wav" % e.index),
                      pcm, e.rate, e.channels)
        elif e.tag == TAG_PCM:
            width = 2 if e.bits16 else 1
            if e.bits16:
                rms, nonconst = pcm16_stats(payload)
                if rms < 1.0 or not nonconst:
                    status.append("SILENT")
            dur = len(payload) / (width * e.channels * e.rate)
            write_wav(os.path.join(bank_dir, "%03d.wav" % e.index),
                      payload, e.rate, e.channels, width)
        else:
            status.append("UNKNOWN-TAG")

        hard_fail = any(s for s in status if not s.startswith("("))
        n_fail += hard_fail
        n_ok += not hard_fail
        print("%-4d %-12s %-7s %-6d %-3d %8s %8s  %s" %
              (e.index, name, codec, e.rate, e.channels,
               "%.2f" % dur if dur is not None else "-",
               "%.0f" % rms if rms is not None else "-",
               " ".join(status) if status else "OK"))
    print("-- %s: %d ok, %d failed" % (info["bank_name"], n_ok, n_fail))
    return n_ok, n_fail


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+",
                    help=".xwb files or directories to scan recursively")
    ap.add_argument("-o", "--out", default="build/audio",
                    help="output root (default build/audio)")
    ap.add_argument("--no-ffmpeg", action="store_true",
                    help="do not use ffmpeg to convert WMA entries to wav")
    args = ap.parse_args()

    files = []
    for p in args.paths:
        if os.path.isdir(p):
            for root, _, fns in os.walk(p):
                files += [os.path.join(root, f) for f in fns
                          if f.lower().endswith(".xwb")]
        else:
            files.append(p)
    files.sort()
    ffmpeg = None if args.no_ffmpeg else find_ffmpeg()
    if not args.no_ffmpeg and not ffmpeg:
        print("note: ffmpeg not found; WMA entries will be dumped as .wma only")

    tot_ok = tot_fail = 0
    for f in files:
        try:
            ok, fail = extract_bank(f, args.out, ffmpeg)
        except Exception as exc:
            print("\n== %s == ERROR: %s" % (f, exc))
            fail, ok = 1, 0
        tot_ok += ok
        tot_fail += fail
    print("\nTOTAL: %d waves ok, %d failed, %d banks" %
          (tot_ok, tot_fail, len(files)))
    return 1 if tot_fail else 0


if __name__ == "__main__":
    sys.exit(main())
