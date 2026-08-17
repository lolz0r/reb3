#!/usr/bin/env python3
"""Extract RenderWare Audio streams (.rws) from Burnout 3 (Xbox).

Chunk layout (RW chunk header = u32 id, u32 size, u32 version):

  0x80D  AudioStream container (spans whole file)
    0x80E  stream header (size 0x7DC in every B3 file), body at 0x18:
      +0x00 u32  used-header-bytes (approx)
      +0x04 u32 0x14, +0x08 u32 0x10, +0x0C u32 0x24, +0x10 u32 ?
      +0x14..+0x1F ptr/0
      +0x20 u32  numSegments      (1 in every B3 file)
      +0x24 ptr
      +0x28 u32  numStreams       (1 or 2)
      +0x2C ptr
      +0x30 u32  dataOffset       (0x800)
      +0x34 u32  clusterStride    (numStreams * 0x10000)
      +0x38 u32  sectorAlign      (0x800)
      +0x3C u32  0
      +0x40 u8[16] stream file UUID
      +0x50 char[16] stream file name (e.g. "AS_C1_V1.cr1")
      +0x60 6 x u32 ptr/0
      +0x78 u32  totalDataBytes   (dataOffset+totalDataBytes == file size,
                                   == 0x80F chunk size)
      +0x7C u32  0
      +0x80 u32  usedBytes[numStreams]   per-stream payload byte counts
      then segment record: u8[16] uuid, char[12] name ("Segment0" etc), u32 ?
      then per stream, 0x28 bytes:
           ptr, ptr, 0, u32 1, u32 clusterSize(0x10000), ptr,
           u16 ?, u16 ?, u32 blockSize(0x4000), u32 clusterSize again,
           u32 clusterOffset  (this stream's slice offset inside a cluster
                               stride: 0, 0x10000, ...)
      then per stream, 0x30 bytes (wave format):
           u32 rate; ptr; u32 usedBytes; u8 bitsPerSample; u8 channels;
           u16 0; u8[12] 0; u8[16] format-class UUID (constant
           17d21bd0-8735-4eed-...); char[4] short name
      then per stream u8[16] UUID, then per stream char[16] stream name
           (e.g. "aGenCrash01", "AS_C1_V11") -- used as output names.
    0x80F  raw sample data at dataOffset (0x800)

Sample data: 16-bit little-endian PCM, channels interleaved per frame
(LRLR...).  The file is divided into clusters of clusterStride bytes;
stream i owns the 0x10000-byte slice at clusterOffset[i] of every cluster.
Verified empirically: L/R correlation ~1.0, waveform continuity across
cluster boundaries, and stream 1 beginning with its own fade-in.

Note: the 36 MUSIC.RWS files are 124-byte dummies (a 0x809 wave dictionary
named "dummy sound bank" with zero waves); they are detected and skipped.

Output: build/audio/rws_<relpath>/<streamname>.wav
"""
import argparse
import math
import os
import struct
import sys
import wave

CHUNK_STREAM = 0x80D
CHUNK_STREAM_HEADER = 0x80E
CHUNK_STREAM_DATA = 0x80F
CHUNK_WAVEDICT = 0x809


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


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


class RwsStream:
    __slots__ = ("name", "uuid", "rate", "channels", "bits", "used",
                 "cluster_off", "cluster_size", "block_size")


def parse_rws(path):
    with open(path, "rb") as f:
        head = f.read(0x800)
    cid, csz, _ = struct.unpack_from("<III", head, 0)
    if cid == CHUNK_WAVEDICT:
        return None  # dummy sound bank (124-byte MUSIC.RWS)
    if cid != CHUNK_STREAM:
        raise ValueError("not an RWS audio stream (chunk 0x%x)" % cid)
    hid, hsz, _ = struct.unpack_from("<III", head, 0xC)
    if hid != CHUNK_STREAM_HEADER:
        raise ValueError("missing 0x80E stream header")
    b = 0x18  # header body
    n_seg = u32(head, b + 0x20)
    n_str = u32(head, b + 0x28)
    data_off = u32(head, b + 0x30)
    stride = u32(head, b + 0x34)
    if n_seg != 1:
        raise ValueError("numSegments=%d not supported (all B3 files have 1)"
                         % n_seg)
    if not (1 <= n_str <= 8):
        raise ValueError("implausible numStreams=%d" % n_str)
    fname = head[b + 0x50:b + 0x60].split(b"\0")[0].decode("latin-1")
    total = u32(head, b + 0x78)
    used = [u32(head, b + 0x80 + 4 * i) for i in range(n_str)]
    p = b + 0x80 + 4 * n_str
    seg_name = head[p + 16:p + 28].split(b"\0")[0].decode("latin-1")
    p += 32  # segment uuid[16] + name[12] + u32
    streams = [RwsStream() for _ in range(n_str)]
    for s in streams:
        s.cluster_size = u32(head, p + 0x10)
        s.block_size = u32(head, p + 0x1C)
        s.cluster_off = u32(head, p + 0x24)
        p += 0x28
    for i, s in enumerate(streams):
        s.rate = u32(head, p)
        s.used = u32(head, p + 8)
        s.bits = head[p + 12]
        s.channels = head[p + 13]
        if s.used != used[i]:
            raise ValueError("per-stream byte counts disagree "
                             "(0x%x vs 0x%x)" % (s.used, used[i]))
        p += 0x30
    for s in streams:
        s.uuid = head[p:p + 16].hex()
        p += 16
    for s in streams:
        s.name = head[p:p + 16].split(b"\0")[0].strip().decode("latin-1")
        p += 16
    # data chunk sanity
    did, dsz, _ = struct.unpack_from("<III", head, 0x18 + hsz)
    if did != CHUNK_STREAM_DATA:
        raise ValueError("missing 0x80F data chunk")
    if dsz != total:
        raise ValueError("0x80F size 0x%x != totalDataBytes 0x%x"
                         % (dsz, total))
    if data_off + total != os.path.getsize(path):
        raise ValueError("dataOffset+totalDataBytes != file size")
    return dict(file_name=fname, seg_name=seg_name, n_streams=n_str,
                data_off=data_off, stride=stride, total=total,
                streams=streams)


def deinterleave(path, info, stream):
    """Collect one stream's payload from its cluster slices."""
    out = []
    remaining = stream.used
    with open(path, "rb") as f:
        pos = info["data_off"] + stream.cluster_off
        while remaining > 0:
            f.seek(pos)
            take = min(stream.cluster_size, remaining)
            out.append(f.read(take))
            remaining -= take
            pos += info["stride"]
    return b"".join(out)


def extract_rws(path, out_root, tag):
    info = parse_rws(path)
    if info is None:
        print("== %s == dummy 'sound bank' stub, no audio (skipped)" % path)
        return 0, 0, True

    out_dir = os.path.join(out_root, "rws_" + tag)
    os.makedirs(out_dir, exist_ok=True)
    print("\n== %s  ('%s', segment '%s', %d stream(s)) ==" %
          (path, info["file_name"], info["seg_name"], info["n_streams"]))
    print("%-4s %-16s %-6s %-6s %-3s %8s %8s  %s" %
          ("idx", "name", "codec", "rate", "ch", "dur(s)", "rms", "status"))

    n_ok = n_fail = 0
    used_names = set()
    for i, s in enumerate(info["streams"]):
        status = []
        if s.bits != 16:
            status.append("UNEXPECTED-BITS(%d)" % s.bits)
        if not (8000 <= s.rate <= 48000):
            status.append("BAD-RATE")
        if s.channels not in (1, 2, 4, 6):
            status.append("BAD-CH")
        dur = rms = None
        if not status:
            pcm = deinterleave(path, info, s)
            if len(pcm) != s.used:
                status.append("SHORT-READ")
            frame = 2 * s.channels
            pcm = pcm[: len(pcm) - (len(pcm) % frame)]
            dur = len(pcm) / (frame * s.rate)
            rms, nonconst = pcm16_stats(pcm)
            if rms < 1.0 or not nonconst:
                status.append("SILENT")
        if not status:
            name = s.name or "stream%d" % i
            if name in used_names:
                name = "%s_%d" % (name, i)
            used_names.add(name)
            write_wav(os.path.join(out_dir, name + ".wav"),
                      pcm, s.rate, s.channels)
            n_ok += 1
        else:
            n_fail += 1
        print("%-4d %-16s %-6s %-6d %-3d %8s %8s  %s" %
              (i, s.name, "pcm%d" % s.bits, s.rate, s.channels,
               "%.2f" % dur if dur is not None else "-",
               "%.0f" % rms if rms is not None else "-",
               " ".join(status) if status else "OK"))
    print("-- %s: %d ok, %d failed" % (tag, n_ok, n_fail))
    return n_ok, n_fail, False


def tag_for(path, roots):
    """Build a unique output-dir tag from the path relative to a given root."""
    ap = os.path.abspath(path)
    for r in roots:
        r = os.path.abspath(r)
        if ap.startswith(r + os.sep):
            rel = ap[len(r) + 1:]
            break
    else:
        rel = os.path.basename(ap)
    rel = os.path.splitext(rel)[0]
    for junk in ("Tracks" + os.sep, "tracks" + os.sep):
        if rel.startswith(junk):
            rel = rel[len(junk):]
    return rel.replace(os.sep, "_")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+",
                    help=".rws files or directories to scan recursively")
    ap.add_argument("-o", "--out", default="build/audio",
                    help="output root (default build/audio)")
    args = ap.parse_args()

    files = []
    for p in args.paths:
        if os.path.isdir(p):
            for root, _, fns in os.walk(p):
                files += [os.path.join(root, f) for f in fns
                          if f.lower().endswith(".rws")]
        else:
            files.append(p)
    files.sort()

    tot_ok = tot_fail = n_dummy = 0
    for f in files:
        try:
            ok, fail, dummy = extract_rws(f, args.out, tag_for(f, args.paths))
        except Exception as exc:
            print("\n== %s == ERROR: %s" % (f, exc))
            ok, fail, dummy = 0, 1, False
        tot_ok += ok
        tot_fail += fail
        n_dummy += dummy
    print("\nTOTAL: %d streams ok, %d failed, %d files (%d dummy stubs)" %
          (tot_ok, tot_fail, len(files), n_dummy))
    return 1 if tot_fail else 0


if __name__ == "__main__":
    sys.exit(main())
