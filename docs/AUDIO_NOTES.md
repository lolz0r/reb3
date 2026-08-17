# Audio extraction notes (Burnout 3: Takedown, Xbox)

Tools: `tools/extract_xwb.py`, `tools/extract_awd.py`, `tools/extract_rws.py`
(Python 3 stdlib only; `extract_xwb.py` optionally shells out to system
`ffmpeg` to convert WMA entries to PCM `.wav`).

Output: `build/audio/<bankname>/NNN.wav|.wma` (XWB),
`build/audio/awd_<dict>/<wave>.wav` (AWD),
`build/audio/rws_<path>/<stream>.wav` (RWS).

Provenance legend used below:
- **[verified]** — read directly off the game files' bytes and cross-checked
  (size arithmetic, magic values, decoded-audio statistics).
- **[community]** — layout knowledge taken from public format notes and then
  confirmed against these files.
- **[unknown]** — observed but unexplained; does not affect extraction.

All multi-byte values little-endian. All offsets hex.

---

## 1. XWB — Microsoft XACT wave banks (33 files)

Files: `Tracks/**/E_DJRACE.xwb` (18), `Tracks/E_DJ*.xwb` (12),
`Tracks/_EATrax0/1.xwb`, `ovid/movie.xwb`.

References: general XWB knowledge (segment structure, BANKDATA fields,
24-byte entry) from Luigi Auriemma's `unxwb` notes and MonoGame's
`WaveBank` reader **[community]**. Those document mostly XACT2/3 (v42+);
the v3 specifics below were re-derived from the bytes **[verified]**.

### Header (v3, XACT1 era) [verified]

```
0x00  "WBND"
0x04  u32 version            = 3 in all 34 banks
0x08  4 x {u32 off, u32 len} segments:
        0 BANKDATA        @0x28 len 0x28
        1 ENTRYMETADATA   @0x50 len 24*n
        2 ENTRYNAMES      0,0  (empty in every bank -- no wave names on disc)
        3 ENTRYWAVEDATA   @0x800
```

BANKDATA: `u32 flags(=1)`, `u32 entryCount`, `char[16] bankName`,
`u32 entryMetaElemSize(=24)`, `u32 entryNameElemSize(=64)`,
`u32 alignment(=0x800)`, `u32 0`.

Entry (24 B): `u32 flagsAndDuration(=0)`, `u32 format`, `u32 playOffset`
(relative to segment 3), `u32 playLength`, `u32 loopStart`, `u32 loopLength`
(loop fields 0 everywhere).

### Format dword packing [verified]

```
bit 0-1   codec tag   0=PCM, 1=Xbox ADPCM, 2=WMA
bit 2-4   channels
bit 5-22  sample rate (Hz, literal value)
bit 23-30 block align (0 for WMA)
bit 31    16-bit flag
```

Verification: the only observed values are `0x8017700A` -> (2, 2ch, 48000)
and `0x8015888A` -> (2, 2ch, 44100), and **every** entry payload begins with
the ASF/WMA magic `30 26 B2 75 8E 66 CF 11`; ffmpeg identifies each dumped
region as a complete `wmav2` file whose rate/channels match the decode of the
dword. XACT1 bit-layout candidates with a 1-bit tag produced nonsense
(5 channels / 96 kHz), so the 2-bit-tag layout above is the empirically
correct one for v3.

### Contents [verified]

- All 33 banks, **885 entries, 100% WMA** (wmav2), stereo.
  48000 Hz: DJ Stryker speech (E_DJ*). 44100 Hz: EA Trax songs
  (`EATrax0/1`, 22 each, 2.4–3.7 min — the licensed soundtrack) and FMV
  audio (`Movie`, 34).
- No PCM and no Xbox ADPCM entry exists anywhere in the game's XWBs, so the
  tool's Xbox ADPCM decoder (IMA variant, 36 bytes/channel blocks — 4-byte
  {s16 predictor, u8 stepIndex, u8 pad} header per channel then 4-byte
  nibble groups round-robin per channel) is implemented **but unexercised
  by real data** [community, untested].
- WMA cannot be decoded with the Python stdlib. Each entry is dumped
  verbatim as `NNN.wma` (a standalone ASF file); when `ffmpeg` is on PATH it
  is also decoded to `NNN.wav` and validated (rate/channel match vs the
  format dword, duration vs the ASF File Properties object, RMS non-silence).
- Extraction result: **876/885 entries valid** (ASF magic, ffmpeg decode,
  rate/channels/duration agreement, RMS 1600-13100). The 9 rejects are all
  in `movie.xwb`: 0.03–0.46 s entries that decode to pure digital silence
  (RMS 0) — placeholder/spacer audio for FMV transitions, present as such
  on the disc; the WMA payloads themselves are well-formed and are dumped.
- Wave names are not stored in the banks (ENTRYNAMES empty); the cue names
  live in the executable's XACT sound-bank data, not extracted here.

## 2. AWD — RenderWare Audio wave dictionaries (174 files)

Files: `Tracks/{AS,EU,US}/*_V1/SOUND.AWD` (18, per-track surface/impact SFX),
`sound/*.awd` (22: `Generic`, `Fe`, `Single`, `crash`, `crashmod`, `elim`,
`roadrage`, `g_crsh04..44`, `Ident0..9`), and — found by surveying the game
dir — `pveh/{COMP,CUPE,HEVY,HSPC,MSCL,SPRT,SUPR,TSPC}/Car*.hwd` and
`Car*.lwd` (67+67): per-vehicle engine banks in the identical format,
dictionaries named `high`/`low`, waves `eng_<rpm>`, `ex_<rpm>` (exhaust),
`gear______11`.

The .awd is a dumped in-memory image of an RWA WaveDictionary (fields that
were heap pointers at build time contain garbage). Existence of community
tools for this format is known (burnout.wiki / EdnessP's scripts) but the
layout below was reverse-engineered **from the bytes of these files**
and validated by the extractor on every file **[verified]**:

```
0x00  u32 chunk id 0x809 (RW audio WaveDict)
0x08  u32 dataOffset          (0x800/0x1000/0x1800/0x2000, sector aligned)
0x14  u32 dataSize            (dataOffset+dataSize == file size, all files)
0x18  u8[16] UUID 042d3a45-5fe4-4bc8-81f0-df758b01f273 (same in all files;
      platform/format id) [unknown]
0x28  u32 dataOffset again    (always equal to 0x08)
0x30  u32 dictOffset          (0x5C in all files)

dict @0x5C:  char name[] NUL-terminated, (len+1) padded to mult. of 4,
then a serialized linked list of wave records:

  u32 uuidOffset      (absolute, this record's UUID)
  u32 nameOffset      (absolute, this record's name; 0 terminates the list)
  u32 0
  u32 vtable ptr      (0x100f2e60 in every file)
  2 x format[0x1C]:   u32 rate; u32 channels; u32 dataBytes;
                      u8 bits; u8 fmtId; 0x12 B uninitialised junk
                      (two identical copies -- target/source format)
  u32 0
  u32 dataOffsetRel   (payload offset relative to header dataOffset)
  u32 offset-of(name-4) [serializer detail]
  u32 4 or 6          [unknown flags]
  u32 0; u32 prev,next,own list links; u32 0
  char name[] (NUL, (len+1) padded to 4);  u8 uuid[16]
  -> next record at uuidOffset+16
```

Payloads: raw PCM, `fmtId=1`, all waves mono 16-bit, rates 2000–44100 Hz
(2000 Hz only for placeholder waves). Payloads laid back-to-back with small
(0x10–0x70) alignment gaps; sum of sizes matches the data region in every
file.

Extraction result: **1575 waves, 1569 valid, 6 failed**. The 6 failures are
genuinely-silent placeholder data on the disc, correctly rejected by the RMS
check: the same `steam` wave in `g_crsh04/14/24/34/44.awd` (200 bytes of
zeros at 2000 Hz) and `gear______11` in `pveh/HSPC/Car25.hwd` (0.10 s of
zeros at 20000 Hz).

The 10 `Ident0..9.awd` are one 16000 Hz jingle each ("ident"); `Generic.awd`
has 45 boost/gear/traffic/horn/skid SFX (`boostloop`, `traff_horn14`,
`smltrafskd12`...); `Fe.awd` is frontend UI sounds; `g_crsh*` hold the
crash-impact suites (glass, metal, explosion); track `SOUND.AWD` hold
per-track object hits (`woodenboxh22`, `trafficone33`, `signpostht22`...);
engine loops live in the per-vehicle `pveh` banks (e.g.
`awd_pveh_COMP_Car1_high/eng_2873.wav`).

## 3. RWS — RenderWare Audio streams (111 files, 75 real + 36 dummies)

Files: `Tracks/**/CRASH1-3.RWS` (crash-mode music), `Tracks/crash1-20.rws`
(crash aftermath songs), `Tracks/_femain.rws` (front-end menu music).
All 36 `MUSIC.RWS` are 124-byte stubs (a 0x809 dictionary literally named
"dummy sound bank", zero waves) — detected and skipped **[verified]**.

Chunk layout **[verified]** (RW chunk = u32 id, u32 size, u32 version):
`0x80D` stream container > `0x80E` header (0x7DC bytes) + `0x80F` raw data
@0x800. Header body fields (base = 0x18): `+0x20 numSegments` (always 1),
`+0x28 numStreams` (1 or 2), `+0x30 dataOffset` (0x800),
`+0x34 clusterStride` (= numStreams * 0x10000), `+0x50 char[16] name`,
`+0x78 totalDataBytes` (== 0x80F chunk size == fileSize-0x800 in all 75),
`+0x80 u32 usedBytes[numStreams]`; then segment {uuid, char[12] name, u32};
then per-stream 0x28-byte records whose last u32 is the stream's
`clusterOffset` (0 / 0x10000); then per-stream 0x30-byte wave formats
{u32 rate; ptr; u32 usedBytes; u8 bits; u8 channels; pad; uuid; char[4]};
then per-stream UUIDs and per-stream `char[16]` names ("aGenCrash01",
"zSloCrash12", "zzfirst", ...). The tail of the 0x80E chunk is a leftover
serialized dev property tree ("rwawaveformat", "packetSize 65536",
`\Track Specific SlowMo\EU_C3_V12.WAV` ...) — ignored.

Sample data **[verified]**: 16-bit LE PCM, frame-interleaved stereo, in
clusters of `clusterStride` bytes; stream *i* owns the 0x10000-byte slice at
`clusterOffset[i]` of every cluster. Evidence: L/R correlation 0.996;
stream 1 starts with its own fade-in (not a continuation of stream 0);
mean |sample delta| at the 57–117 cluster seams of three audited files is
statistically identical to the in-cluster mean (e.g. 3802 vs 5113,
1096 vs 1144) — a wrong seam mapping would show order-of-magnitude jumps.

Contents: `CRASH1/2.RWS` = two 30.0 s 32 kHz stereo streams (crash-mode
music, normal + alternate); `CRASH3.RWS` = longer variant; root
`crashN.rws` = `aGenCrashNN` + `zSloCrashNN` pairs (crash aftermath song +
slow-motion mix, 30.0 s each); `_femain.rws` = one 44.2 s 44.1 kHz menu
loop (`zzfirst`).

Extraction result: **131 streams from 75 files, 0 failures** (all pass
rate/channel sanity, exact byte counts, RMS 1800–15800).

## 4. Not extracted / open items

- **XWB cue names**: ENTRYNAMES segments are empty; DJ line names would have
  to come from the XACT sound bank compiled into the XBE.
- **AWD [unknown] fields**: header dwords @0x38/0x3C/0x44 and the per-wave
  `4/6` dword — constant-ish, not needed for extraction.
- **RWS segment u32** after the segment name (0 or 2) — unexplained,
  extraction does not depend on it.
- **`.xmv` movie files** (Tracks/*.xmv) contain their own audio; out of
  scope for this pass (movie soundtrack audio is separately present in
  `ovid/movie.xwb`).

## 5. Reproduce

```
python3 tools/extract_awd.py "<game dir>" -o build/audio
python3 tools/extract_xwb.py "<game dir>" -o build/audio   # ffmpeg optional
python3 tools/extract_rws.py "<game dir>" -o build/audio
```

Each prints a per-bank table (index, name, codec, rate, channels, duration,
RMS, status) and exits non-zero if any wave fails validation.

---

## 6. How the game names and selects these waves (added by the SFX pass)

The extraction above recovers the wave *data*. The executable side — which
wave the game plays on which gameplay event, and the naming rule behind the
filenames — is `docs/RE_SFX.md`. Two facts from it belong here because they
explain the filenames this tool emits:

* **Wave names are 12-character base-40 values**, the same packing
  `FUN_001AECC0` uses for vlist car ids (`tools/extract_traffic.py b40()`).
  The XBE stores only the space-padded 10-character stem, e.g.
  `"SLAM______  "` at `0x0039C360`, `"IMPACTNUDG  "` at `0x0039CB78`.
  Searching the image for a shipped filename therefore mostly fails; search
  for the stem. **[verified]**

* **The trailing two characters of a shipped name are `<index><count>`.**
  The dictionary lookup `FUN_001C99D0` masks the key with `% 40*40` before
  comparing (`__aullrem` by `0x640`), so `impactfata16`..`impactfata66` all
  match the single stem `IMPACTFATA`, and a flag makes the code pick one at
  random via the module LCG at `DAT_004A1BE0`. That is why the AWDs are full
  of names like `glasssides13/23/33`, `kurbbounce14..44`, `carpartwpr11`.
  **[verified]**

Bank roles confirmed by the code: `sound/crashmod.awd` is the in-race crash
bank every impact/glass/panel emitter draws from (racecar-audio `+0x880`,
loaded at `0x0014B6DD` from the string `"sound\crashmod.awd"` at
`0x003AEDDC`); `sound/{elim,crash,roadrage,single}.awd` are the per-mode
banks holding `slam______`/`shunt_____`/`boostgain`/`boostloss`;
`sound/generic.awd` holds `boostloop` and the `staticpass` near-miss whooshes;
the per-track `SOUND.AWD` object hits are addressed by the prop stems
`WOODENBOXH`, `OILDRUMHIT`, `CABLEDRUMH`, … at `0x0039CC10`..`0x0039D048`,
whose min/max impulse and volume come from the retail ValueDB under
`Sound/Prop/<Thing>/` in `Payload.cfg` / `Tracks/<TRACK>.cfg`
(`tools/emulate_sfx_params.py` prints all 60 keys with their retail values).

Open here: the `g_crsh04/14/24/34/44.awd` variants of the crash bank are
byte-near-identical to `crashmod.awd` and the selector between them was not
traced **[unknown]**. The HUD/announcer stings (`tdown`, `tdownsp`, `swoosh`,
`medal1/2`, `trophy`) exist in the mode banks but appear nowhere in the
executable as either a packed name or a string, so their trigger path is
unresolved **[unknown]**.
