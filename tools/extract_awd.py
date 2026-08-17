#!/usr/bin/env python3
"""Extract RenderWare Audio Wave Dictionaries (.awd) from Burnout 3 (Xbox).

The .awd is an in-memory image of the RenderWare Audio (RWA) WaveDictionary,
chunk id 0x809.  Layout, reverse-engineered byte-by-byte from the 41 .awd
files in the game (all offsets are absolute file offsets; "ptr" fields hold
meaningless build-time heap addresses):

  0x00  u32  chunk id 0x809
  0x04  ptr
  0x08  u32  dataOffset   (0x800/0x1000/0x1800/0x2000; sector aligned)
  0x0C  u32  0x2C
  0x10  u32  0
  0x14  u32  dataSize     (dataOffset+dataSize == file size)
  0x18  u8[16] UUID       (constant 042d3a45-5fe4-4bc8-81f0-df758b01f273
                           in every file -- platform/format id)
  0x28  u32  dataOffset (again)
  0x2C  ptr
  0x30  u32  dictOffset   (0x5C in every file)
  0x38  u32  ?, 0x3C u32 ?, 0x44 u32 0x04000000, 0x48/0x4C ptr

  dict @0x5C:
    char name[]           dictionary name, NUL terminated, (len+1) padded
                          to a multiple of 4
    ... then wave records, a serialized linked list.  Record k:
    u32  uuidOffset       absolute offset of this record's UUID
    u32  nameOffset       absolute offset of this record's name (0 == end)
    u32  0
    ptr  vtable           (0x100f2e60 in every file)
    fmt[0x1C]             u32 rate; u32 channels; u32 dataBytes;
                          u8 bitsPerSample; u8 fmtId(1=PCM);
                          u16+u8[10]+u16 junk/uninitialised
    fmt[0x1C]             identical second copy ("target" vs "source"
                          format; always equal in these files)
    u32  0
    u32  dataOffsetRel    wave payload offset relative to header dataOffset
    u32  (offset of the u32 just before the name -- serializer detail)
    u32  flags?           (4 or 6; meaning unknown)
    u32  0
    u32  prevLink, nextLink, ownLink   (list bookkeeping, unused here)
    u32  0
    char name[]           NUL terminated, (len+1) padded to 4
    u8   uuid[16]
    -> next record starts at uuidOffset+16

  Wave payloads: raw PCM (fmtId 1).  Every wave in Burnout 3 is mono 16-bit,
  rates 2000..44100 Hz.  Payloads are stored back-to-back (small padding gaps)
  in the data region starting at dataOffset.

The per-vehicle engine sound banks pveh/*/Car*.hwd and Car*.lwd ("high"/"low"
detail wave dictionaries, waves eng_NNNN / ex_NNNN / gear...) use the exact
same format and are picked up by the directory scan.

Output: build/audio/awd_<dictname>/<wavename>.wav; when several files share
a dictionary name (all .hwd are "high", all .lwd are "low") the directory is
qualified with the source path: awd_<path>_<dictname>.
"""
import argparse
import math
import os
import struct
import sys
import wave

CHUNK_WAVEDICT = 0x809


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def cstr(b, o):
    end = b.index(b"\0", o)
    return b[o:end].decode("latin-1")


def pad4(n):
    return (n + 3) & ~3


def write_wav(path, pcm, rate, channels, sampwidth=2):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(sampwidth)
        w.setframerate(rate)
        w.writeframes(pcm)


def pcm16_stats(pcm):
    n = len(pcm) // 2
    if n == 0:
        return 0.0, False
    samples = struct.unpack("<%dh" % n, pcm[:n * 2])
    acc = 0
    lo = hi = samples[0]
    for s in samples:
        acc += s * s
        if s < lo:
            lo = s
        if s > hi:
            hi = s
    return math.sqrt(acc / n), hi != lo


class AwdWave:
    __slots__ = ("name", "uuid", "rate", "channels", "bits", "fmt_id",
                 "size", "offset")


def parse_awd(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 0x60 or u32(data, 0) != CHUNK_WAVEDICT:
        raise ValueError("not an AWD (chunk id != 0x809)")
    data_off = u32(data, 0x08)
    data_size = u32(data, 0x14)
    if u32(data, 0x28) != data_off:
        raise ValueError("dataOffset fields at 0x08/0x28 disagree")
    if data_off + data_size != len(data):
        raise ValueError("dataOffset+dataSize != file size")
    dict_off = u32(data, 0x30)
    dict_name = cstr(data, dict_off)
    p = dict_off + pad4(len(dict_name) + 1)

    waves = []
    while p + 8 <= data_off:
        uuid_off = u32(data, p)
        name_off = u32(data, p + 4)
        if name_off == 0 or name_off >= data_off or uuid_off >= data_off:
            break
        q = p + 16  # skip zero + vtable ptr
        w = AwdWave()
        w.rate = u32(data, q)
        w.channels = u32(data, q + 4)
        w.size = u32(data, q + 8)
        w.bits = data[q + 12]
        w.fmt_id = data[q + 13]
        # second format copy must agree (rate/channels/size/bits/fmt)
        if (u32(data, q + 0x1C), u32(data, q + 0x20), u32(data, q + 0x24),
                data[q + 0x28], data[q + 0x29]) != (
                w.rate, w.channels, w.size, w.bits, w.fmt_id):
            raise ValueError("format copies disagree at 0x%x" % q)
        if u32(data, q + 0x38) != 0:
            raise ValueError("expected 0 after formats at 0x%x" % (q + 0x38))
        w.offset = u32(data, q + 0x3C)
        w.name = cstr(data, name_off)
        w.uuid = data[uuid_off:uuid_off + 16].hex()
        waves.append(w)
        p = uuid_off + 16
    return dict(data_off=data_off, data_size=data_size, dict=dict_name,
                waves=waves, raw=data)


def extract_awd(path, out_root, dir_name, info):
    data = info["raw"]
    base = info["data_off"]
    out_dir = os.path.join(out_root, dir_name)
    os.makedirs(out_dir, exist_ok=True)

    print("\n== %s  (dict '%s', %d waves, data @0x%x+0x%x) ==" %
          (path, info["dict"], len(info["waves"]), base, info["data_size"]))
    print("%-4s %-24s %-6s %-6s %-3s %8s %8s  %s" %
          ("idx", "name", "codec", "rate", "ch", "dur(s)", "rms", "status"))

    n_ok = n_fail = 0
    for i, w in enumerate(info["waves"]):
        status = []
        if w.fmt_id != 1 or w.bits != 16:
            status.append("UNEXPECTED-FMT(id=%d,bits=%d)" % (w.fmt_id, w.bits))
        if not (2000 <= w.rate <= 48000):
            status.append("BAD-RATE")
        if w.channels not in (1, 2, 4, 6):
            status.append("BAD-CH")
        if w.offset + w.size > info["data_size"]:
            status.append("OUT-OF-RANGE")
        pcm = b""
        dur = rms = None
        if not status:
            pcm = data[base + w.offset: base + w.offset + w.size]
            if len(pcm) != w.size:
                status.append("TRUNCATED")
            frame = 2 * w.channels
            pcm = pcm[: len(pcm) - (len(pcm) % frame)]
            dur = len(pcm) / (frame * w.rate)
            rms, nonconst = pcm16_stats(pcm)
            if rms < 1.0 or not nonconst:
                status.append("SILENT")
        if not status:
            write_wav(os.path.join(out_dir, w.name + ".wav"),
                      pcm, w.rate, w.channels)
            n_ok += 1
        else:
            n_fail += 1
        print("%-4d %-24s %-6s %-6d %-3d %8s %8s  %s" %
              (i, w.name, "pcm%d" % w.bits, w.rate, w.channels,
               "%.2f" % dur if dur is not None else "-",
               "%.0f" % rms if rms is not None else "-",
               " ".join(status) if status else "OK"))
    print("-- %s: %d ok, %d failed" % (info["dict"], n_ok, n_fail))
    return n_ok, n_fail


AWD_EXTS = (".awd", ".hwd", ".lwd")


def path_tag(path, roots):
    """Path-derived tag: input path relative to whichever root it came from,
    with separators flattened to underscores."""
    ap = os.path.abspath(path)
    for r in roots:
        r = os.path.abspath(r)
        if ap.startswith(r + os.sep):
            rel = ap[len(r) + 1:]
            break
    else:
        rel = os.path.basename(ap)
    return os.path.splitext(rel)[0].replace(os.sep, "_")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+",
                    help=".awd/.hwd/.lwd files or directories to scan "
                         "recursively")
    ap.add_argument("-o", "--out", default="build/audio",
                    help="output root (default build/audio)")
    args = ap.parse_args()

    files = []
    for p in args.paths:
        if os.path.isdir(p):
            for root, _, fns in os.walk(p):
                files += [os.path.join(root, f) for f in fns
                          if f.lower().endswith(AWD_EXTS)]
        else:
            files.append(p)
    files.sort()

    # pass 1: parse everything, count dictionary-name collisions
    parsed, errors = [], []
    dict_count = {}
    for f in files:
        try:
            info = parse_awd(f)
        except Exception as exc:
            errors.append((f, exc))
            continue
        parsed.append((f, info))
        dict_count[info["dict"]] = dict_count.get(info["dict"], 0) + 1

    # pass 2: extract; collide -> qualify with source path
    tot_ok = tot_fail = 0
    for f, info in parsed:
        if dict_count[info["dict"]] > 1:
            dir_name = "awd_%s_%s" % (path_tag(f, args.paths), info["dict"])
        else:
            dir_name = "awd_" + info["dict"]
        try:
            ok, fail = extract_awd(f, args.out, dir_name, info)
        except Exception as exc:
            errors.append((f, exc))
            ok, fail = 0, 1
        tot_ok += ok
        tot_fail += fail
    for f, exc in errors:
        print("\n== %s == ERROR: %s" % (f, exc))
    print("\nTOTAL: %d waves ok, %d failed, %d dictionaries, %d errors" %
          (tot_ok, tot_fail, len(parsed), len(errors)))
    return 1 if (tot_fail or errors) else 0


if __name__ == "__main__":
    sys.exit(main())
