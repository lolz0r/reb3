# EA TRAX — the music system (Burnout 3: Takedown, Xbox)

Module: `src/burnout3_music.c` / `.h` (playback + selection),
`src/burnout3_hud.c` (the now-playing banner, also RE_FRONTEND §6.9).
Tools: `tools/extract_eatrax.py` (banks → `build/music`),
`tools/extract_rws.py` (the crash beds → `build/audio/rws_crashNN`),
`tools/validate_music.py` (differential test, 100/100 green).

Two streams live here: the **EA TRAX song** (§1–§5) and the **crash bed**
(§6), the pre-rendered stream retail lays over a crash. The song table is
recovered and the song's selection/playback is GLUE; the crash bed is
recovered end to end — selection, layering, level, trigger and release —
with three marked deviations in §6.5.

Marks, same legend as the rest of `docs/`:
**[C]** read off the binary at the cited address and independently
re-derived by the validator · **[S]** strong static inference ·
**[S-ref]** reference-frame calibrated · **GLUE** harness code written to
described behaviour, no address claimed · **[?]** open.

All addresses are `build/burnout3.elf` VAs (`tools/xbe2elf.py`; **never**
a flat XBE load — HANDOFF §2).

---

## 1. The song table — 0x003EC458 [C]

44 entries, 24-byte stride, in `.data`:

```
+0x00  u32  title   Globalus.bin string index
+0x04  u32  album   Globalus.bin string index
+0x08  u32  artist  Globalus.bin string index
+0x0C  u32  per-track enable    (compiled default 15 in every entry)
+0x10  u32  1-based ordinal 1..43, entry 43 holds 0                  [?]
+0x14  u32  0 in every entry, entry 43 holds 44                      [?]
```

### 1.1 How it was found

The one string the brief supplied — Globalus **548 = "Motion City
Soundtrack"** — turns out to sit inside a long, obviously musical run of
Globalus entries. Dumping 456..575 shows a clean **(title, album, artist)**
repeat, 40 triples deep:

```
456 Independence Day / 457 Daylight Breaking / 458 No Motiv
459 Always You       / 460 Fading Days       / 461 Amber Pacific
...
546 My Favorite Accident / 547 I Am The Movie / 548 Motion City Soundtrack
```

Scanning the correctly-mapped image for the u32 triple `456,457,458` finds
exactly **one** hit, at `0x003EC458`, and reading forward at a 24-byte
stride walks the whole block and then continues past it into four more
entries whose ids are `3255..3266` — the four songs added late in
development, whose strings live far away from the 456..575 block:
Burning Brides, Atreyu, Letter Kills, Jimmy Eat World.

### 1.2 What pins the stride, the count and field +0x0C [C]

`FUN_00152ED0` (the "apply music options" path) refreshes one field of
every entry from a byte array:

```c
iVar2 = 0;  uVar1 = 0;
do {
    *(uint *)((int)&DAT_003ec464 + uVar1) = (uint)(byte)(&DAT_004ae1a0)[iVar2];
    uVar1 = uVar1 + 0x18;
    iVar2 = iVar2 + 1;
} while (uVar1 < 0x420);
```

That single loop is the whole proof:

* base `0x003EC464` = table + **0x0C** → identifies the field;
* `+= 0x18` → the **24-byte stride**;
* `< 0x420` → `0x420 / 0x18` = **44 entries**;
* source `DAT_004AE1A0` is a `u8[44]` in BSS (zero in the image, written
  from the profile at runtime) → the field is a **per-track enable**,
  one byte per song.

`tools/validate_music.py` §1 re-reads all four immediates out of the
function body, so none of this is assumed.

### 1.3 The options the table is filtered by [C]

The Globalus entries immediately below the song block are the EA TRAX
options screen, which is the corroboration that the block is the
soundtrack and not some other list:

| index | string |
|---|---|
| 449 | `ALL` |
| 450 | `RACE ONLY` |
| 451 | `MENU ONLY` |
| 452 | `OFF` |
| 453 | `PLAY TRACKS RANDOMLY` |
| 454 | `PLAY TRACKS SEQUENTIALLY` |
| 455 | `NEXT SOUNDTRACK` |

`0x003EC458+0x0C`'s compiled default of `15` (0x0F) is consistent with a
context mask rather than a plain bool, but the writer of `DAT_004AE1A0`
was not traced, so the *meaning* of the individual bits is **[?]**. The
port treats the field as a bool (`b3_music_set_enabled`).

---

## 2. The waves [S]

`Tracks/_EATrax0.xwb` and `_EATrax1.xwb` are XACT v3 wave banks holding
**22 entries each = 44**, every one `wmav2 / 2ch / 44100 Hz`
(docs/AUDIO_NOTES.md §1 for the header layout and the format-dword
packing). 44 songs, 44 waves.

The index→wave assignment is therefore forced: **table index `i` is bank
`i / 22`, wave `i % 22`**. It is [S] rather than [C] because no code path
was traced from the table to a bank index — but three independent
duration coincidences corroborate it, and they are not subtle:

| table index | song | decoded length |
|---|---|---|
| 6 | The Von Bondies — C'mon C'mon | 2:13 |
| 7 | Ramones — I Wanna Be Sedated | 2:29 |
| 32 | The Bouncing Souls — Sing Along Forever | 1:35 |

Those are the three shortest tracks on the soundtrack and they land in
exactly the three slots the table puts them in. Every other entry decodes
to 1:35–4:16, i.e. all 44 are full songs.

`tools/extract_eatrax.py` carves each entry out as a standalone ASF and
lets `ffmpeg` write `build/music/track_NN.wav` at **44100 Hz mono s16** —
the harness's device format, so playback needs no resampling — plus
`build/music/eatrax.txt`, the manifest the validator cross-checks. 44/44
decode, peak 24413–32768, no silent entries.

The string ids in the C table are the game's; the *text* is ASCII-folded
(U+2019 → `'`) because the recovered GlobalFont only carries glyphs
0x20..0x7E. The validator applies the same fold before comparing, so the
C can never drift from Globalus without failing.

---

## 3. The full track list (44) [C]

| # | bank/wave | artist | title | album |
|---|---|---|---|---|
| 0 | 0/0 | No Motiv | Independence Day | Daylight Breaking |
| 1 | 0/1 | Amber Pacific | Always You | Fading Days |
| 2 | 0/2 | The Ordinary Boys | Over The Counter Culture | Over The Counter Culture |
| 3 | 0/3 | Funeral For A Friend | Rookie Of The Year | Casually Dressed & Deep In Conversation |
| 4 | 0/4 | Chronic Future | Time And Time Again | Lines In My Face |
| 5 | 0/5 | Franz Ferdinand | This Fire | Franz Ferdinand |
| 6 | 0/6 | The Von Bondies | C'mon C'mon | Pawn Shoppe Heart |
| 7 | 0/7 | Ramones | I Wanna Be Sedated | EA Throwback Trax |
| 8 | 0/8 | Autopilot Off | Make A Sound | Make A Sound |
| 9 | 0/9 | Ash | Orpheus | Meltdown |
| 10 | 0/10 | Yellowcard | Breathing | Ocean Avenue |
| 11 | 0/11 | Pennywise | Rise Up | From The Ashes |
| 12 | 0/12 | Fall Out Boy | Reinventing The Wheel To Run Myself Over | Take This To Your Grave |
| 13 | 0/13 | The FUps | Lazy Generation | `..... ...` |
| 14 | 0/14 | The Lot Six | Autobrats | Major Fables |
| 15 | 0/15 | Sahara Hotnights | Hot Night Crash | Kiss & Tell |
| 16 | 0/16 | Eighteen Visions | I Let Go | Obsession |
| 17 | 0/17 | Donots | Saccharine Smile | Amplify The Good Times |
| 18 | 0/18 | From First To Last | Populace In Two | Dear Diary, My Teen Angst Has A Bodycount |
| 19 | 0/19 | Sugarcult | Memory | Palm Trees And Power Lines |
| 20 | 0/20 | Finger Eleven | Stay In Shadow | Finger Eleven |
| 21 | 0/21 | Reggie And The Full Effect | Congratulations Smack And Katy | Under The Tray |
| 22 | 1/0 | Local H | Everyone Alive | Whatever Happened To P.J. Soles? |
| 23 | 1/1 | Maxeen | Please | Maxeen |
| 24 | 1/2 | New Found Glory | At Least I'm Known For Something | Catalyst |
| 25 | 1/3 | My Chemical Romance | I'm Not Okay (I Promise) | Three Cheers For Sweet Revenge |
| 26 | 1/4 | Go Betty Go | C'mon | Worst Enemy |
| 27 | 1/5 | Moments In Grace | Broken Promises | Moonlight Survived |
| 28 | 1/6 | Midtown | Give It Up | Forget What You Know |
| 29 | 1/7 | 1208 | Fall Apart | Turn of the Screw |
| 30 | 1/8 | Motion City Soundtrack | My Favorite Accident | I Am The Movie |
| 31 | 1/9 | Rise Against | Paper Wings | Siren Song Of The Counter Culture |
| 32 | 1/10 | The Bouncing Souls | Sing Along Forever | Anchors Aweigh |
| 33 | 1/11 | The Matches | Audio Blood | E. Von Dahl Killed The Locals |
| 34 | 1/12 | Silent Drive | 4/16 | Love Is Worth It |
| 35 | 1/13 | The Explosion | Here I Am | Black Tape |
| 36 | 1/14 | The D4 | Come On! | 6Twenty |
| 37 | 1/15 | The Mooney Suzuki | Shake That Bush Again | Alive & Amplified |
| 38 | 1/16 | Mudmen | Animal | Overrated |
| 39 | 1/17 | The Futureheads | Decent Days And Nights | The Futureheads |
| 40 | 1/18 | Burning Brides | Heart Full Of Black | Leave No Ashes |
| 41 | 1/19 | Atreyu | Right Side Of The Bed | The Curse |
| 42 | 1/20 | Letter Kills | Radio Up | The Bridge |
| 43 | 1/21 | Jimmy Eat World | Just Tonight... | Futures |

---

## 4. Selection and playback — GLUE

Not recovered. The retail selector was not traced (the mandate was
relaxed to "a straightforward shuffle with no immediate repeats is
fine"), so the module implements the behaviour the options screen
describes and claims no addresses for it:

* **shuffle bag.** `b3_music_pick_next()` draws from a Fisher–Yates
  permutation of every enabled+present track and refills when empty, so
  all 44 play before any repeats. If a fresh bag would open on the track
  that just finished, its head is swapped away — no repeat across the
  seam either. Seeded from `time(NULL)`, or deterministically with
  `b3_music_seed()`.
* **auto-advance.** `b3_music_pump()` opens the next track the moment the
  current file is exhausted, so the stream never gaps.
* **streaming.** A 512 KiB (11.9 s) ring: `b3_music_pump()` is the only
  writer (main thread, once per frame), `b3_music_next_sample()` the only
  reader (SDL audio thread). They share two monotone 64-bit counters, no
  lock. Nothing allocates or touches the disk on the audio thread.
* **now playing.** Track changes are queued as `{absolute sample, track}`
  marks that the *mixer* consumes, so `b3_music_now_playing()` reports
  what is actually being heard rather than what has been buffered — which
  is what makes the banner turn over on the beat instead of ~10 s early.
* **edges and ducking.** A 0.25 s ramp at each track edge (a hard cut into
  a song clicks), and `b3_music_set_duck()` for the crash slow-mo,
  slewed over 0.2 s.

Retail behaviour that is **[?]** and deliberately not faked: which PRNG
the game uses, whether "PLAY TRACKS SEQUENTIALLY" restarts at index 0,
the DJ-speech ducking law, and the volume the game mixes music at.

---

## 5. The banner

See `docs/RE_FRONTEND.md` §6.9. Short version: GLUE layout, but built
out of recovered parts — the `big_curve` plate the POS/LAP elements use,
the recovered GlobalFont through the same pen/shadow path as every other
HUD label, the game's own `EATrax` badge from `Global.txd`, and
`big_curve`'s own swoosh blue (99,142,214) for the artist line. Driven by
additive `B3HudState.music` fields; a pure function of `elapsed`, so a
track change re-runs the whole animation with no HUD-side state.

---

## 6. The CRASH BED — the pre-rendered crash stream [C]

The other thing this module streams. When a car crashes, retail lays a
**30 s pre-rendered stereo bed** over the race, out of
`Tracks/crash1.rws .. crash20.rws`. Each of those files holds **two
sub-streams**, `aGenCrashNN` and `zSloCrashNN`, and which one you hear is
decided by the time divisor. Extracted by `tools/extract_rws.py` to
`build/audio/rws_crashNN/{aGen,zSlo}CrashNN.wav` (32000 Hz stereo s16).

Three functions carry the whole thing:

| function | role |
|---|---|
| `FUN_0014B600` @`0x0014C269` | race init: creates the stream object (`mgr+0x884`, `FUN_001CBF30`) and picks the race's first bed |
| `FUN_00150D40` | the trigger, called from the impact router `FUN_0014D0F0` @`0x0014D20B` |
| `FUN_00150E80` | the per-frame driver, called from `FUN_0014C880` @`0x0014CDC5`/`0x0014CDE5` |

### 6.1 Selection — a sequential rotation, not a shuffle [C]

```
0014C269  MOV   CL, byte ptr [EBP+0x890]     ; the u8 rotation counter
0014C273  MOV   ESI, 0x14                    ; 20
0014C278  IDIV  ESI
0014C27A  LEA   EAX, [EBP+0x4B0]             ; the name buffer
0014C280  INC   EDX                          ; (counter % 20) + 1
0014C284  PUSH  0x3AED84                     ; "tracks\crash%d.rws"
0014C282  INC   CL                           ; counter++
0014C290  CALL  _sprintf
...
0014C2D6  INC   AL                           ; ... and AGAIN, unconditionally
0014C2D8  MOV   byte ptr [EBP+0x890], AL
```

So the race-init pick advances the counter **twice** and every in-race
rotation advances it once. From a fresh audio object: race 1 opens on
`crash1`, its second crash plays `crash3`, then 4, 5, 6…; race 2 opens
where race 1 left off. The identical `%20 + 1` block is at `0x00151239`
inside `FUN_00150E80`, which re-picks as soon as a bed finishes so the
next file is already buffered.

The bed is opened and **pre-buffered while idle** — `FUN_001CB6C0(bed, 0,
mgr+0x4B0, 0, 4)` @`0x001512A0`, gated on the stream state being 0/1/4 and
`mgr+0x8DB` clear — and only unpaused when a crash happens.

**Recovered, deliberately not ported:** one rotation in five
(`mgr+0x891 % 5 == 0`, with `DAT_0073A198`/`DAT_0073A199` clear,
@`0x001511AE`) uses the **current track's own** `crash1.rws`/`crash2.rws`
(alternating on `mgr+0x892 & 1`) under the directory
`FUN_001513E0` builds, instead of the global set — the extracted
`build/audio/rws_<TRACK>_V1_CRASH{1,2}` directories. Those files pair the
track's own bed with an `aGenCrashNN`, **not** `aGen` with `zSlo`, so the
divisor layer gate below does not describe them, and the harness has no
track → audio-directory map yet. The port always takes the global pick.

### 6.2 Layers — the time divisor picks the sub-stream [C]

`FUN_001CB9E0` settles what `+0x15C` and `+0x160` on a stream object are:

```
001CB9E0  CMP   EAX, -1
001CB9E9  JNZ   0x001CB9FE                   ; -1 = every sub-stream
001CB9EB  MOVSS [ECX+0x15C], XMM0
001CB9F3  MOVSS [ECX+0x160], XMM0
001CB9FE  MOVSS [ECX+EAX*4+0x15C], XMM0      ; ... else index it
```

They are **per-sub-stream volumes**, `+0x15C + chan*4`. And
`FUN_00150E80` sets them straight off `DAT_0060EA18`, the current time
divisor (RE_TAKEDOWN_FX §1.1):

```
00150F4F  CMP   dword ptr [0x0060EA18], 0x1
00150F56  JZ    0x00150F93
          ; divisor != 1 -- the crash cinematic's dilated window
00150F61  MOVSS [bed+0x15C], 0.0             ; aGenCrashNN off
00150F77  MULSS XMM0, [0x003EC418]           ; mgr+0x834 * 0.7
00150F7F  MOVSS [bed+0x160], XMM0            ; zSloCrashNN on
00150F87  MOV   byte ptr [EBP+0x8DF], 1
          ; divisor == 1 -- normal speed
00150FA1  MULSS XMM0, [0x003EC418]
00150FA9  MOVSS [bed+0x15C], XMM0            ; aGenCrashNN on
00150FBA  MOVSS [bed+0x160], 0.0             ; zSloCrashNN off
00150FC2  MOV   byte ptr [EBP+0x8DF], 0
```

**`zSloCrashNN` is the slow-motion treatment and it plays exactly while
the divisor is not 1.** A hard cut, no crossfade, and both sub-streams
share one file position — the switch is only two volumes. The same
`SETNZ` on the divisor is recomputed every frame at `0x0014CB80`.

### 6.3 Level — 0.70 against the song's 0.30, nothing ducked [C]

`FUN_0014B3D0` initialises the manager's three stream volumes:

| field | init | used for |
|---|---|---|
| `mgr+0x834` | `1.0` @`0x0014B534` | the bed in a **race** |
| `mgr+0x838` | `0.85` (`[0x0039CC00]`) @`0x0014B564` | the bed in **Crash Mode** |
| `mgr+0x83C` | `0.30` (`[0x003EC424]`) @`0x0014B59E` | the **EA TRAX song** |

The race bed therefore plays at `1.0 * [0x003EC418] (0.7)` = **0.70**
against the song's **0.30** — 2.33× the music, summing to exactly 1.0 —
and **nothing ducks**: the audio mix state machine's crash row raises one
group to 1.1 and leaves the other fourteen at 1.0 (docs/RE_SFX.md §6.1).
`FUN_00150D40` also spreads the bed over the 5.1 bins,
`FUN_001CBCA0(0.71, 0.6, 0, 0.35)` @`0x00150DE3` with `ECX = -1` (all
voices): fronts 0.71, rears 0.6, centre 0, LFE 0.35.

### 6.4 Trigger and release [C]

`FUN_00150D40`, from the crashed branch of the impact router, wants
`DAT_0073A1C0 <= 1` (single player) and the crash inside **50.0** units of
the listener (`COMISS` against `[0x003B16B8]` @`0x00150DC0`). It then
forces the stream to state 6 (unpause) and latches:

```
00150E50  MOV   byte ptr [EBX+0x8DD], 0
00150E57  MOV   byte ptr [EBX+0x8E0], 1      ; fires once per crash
00150E5E  MOV   byte ptr [EBX+0x8DC], 1
00150E65  MOVSS [EBX+0x8A4], [0x003B1688]    ; 2.0, the Crash Mode hold
```

`FUN_00150E80`'s second argument is the sustain condition: `FUN_0014C880`
ORs every local player's crashing byte `+0x236` into it at `0x0014CAA0`.
When it goes false with the bed still up, the release runs:

```
001510C8  MOVSS [EBP+0x8B0], clock           ; DAT_0060EA20, the DILATED clock
001510D0  ADDSS XMM0, [0x003B1684]           ; + 0.5 s
001510D8  MOVSS [EBP+0x8B4], XMM0
001510E0  CMP   dword ptr [0x0060EA18], 1
001510E7  SETNZ AL -> [EBP+0x8C0]            ; FREEZE the layer being faded
...
001510F9  COMISS XMM0, [EBP+0x8B4]
00151104  CALL  FUN_001CB900                 ; past the end -> STOP
...
00151126  SUBSS XMM1, clock                  ; (t_end - clock)
00151132  MULSS XMM0, [0x003EC418]           ; * 0.7
0015113A  MULSS XMM1, [0x003B1688]           ; * 2.0
0015114C  CALL  FUN_001CB9E0                 ; on channel mgr+0x8C0 only
```

A linear 0.5 s fade to zero, on the layer that was playing when the crash
ended, then stop — and the stop drops the stream to state 0, which is what
makes the `{0,1,4}` branch open the next rotation's file.

**Recovered, not ported: Crash Mode.** `FUN_00017310() != 0` (the same
test that loads `sound\crashmod.awd` @`0x0014B652`) takes a different
branch @`0x00150FCD`: `aGen` only, at `mgr+0x838` (0.85) times a ramp held
at 1.0 while `mgr+0x8A8 > 15.0` (`[0x003B16B4]`) and decayed by 0.01 per
call (`[0x003A7ED8]`) once below, stopping at zero. The harness has no
Crash Mode.

### 6.5 What the port does differently

| | retail | port | why |
|---|---|---|---|
| song level under the bed | 0.30, never ducked | ducked to `0.30 / master`, released on stop | **TUNED.** The harness's song master is a user setting (0.65 as of 2026-08-13, "music too quiet"). Ducking to retail's own 0.30 absolute restores both the 2.33:1 balance and the 0.30 + 0.70 = 1.0 sum retail mixed at. At a 0.30 master the duck is 1.0 and nothing is ducked, exactly like retail. |
| volume steps | written once per frame | walked to over 5 ms in the mixer | **GLUE (anti-zipper).** A 735-sample step would click; 5 ms still reads as a cut. |
| 1-in-5 track-local bed | `<track>/crash{1,2}.rws` | always the global pick | §6.1 — different sub-stream pairing, and no track→audio-dir map yet. |
| Crash Mode branch | `mgr+0x838` ramp | absent | no Crash Mode in the harness |
| nearby AI crashes | the router's 50-unit gate starts the bed for **any** crash, so an AI wreck nearby gets a ~0.5 s stab before the release takes it | only the player's crash | the harness has no per-crash router hook in this module; `b3_music_crash_tick`'s first argument is the sustain condition and would carry it if one is added |
| 32000 Hz stereo source | DirectSound resamples | downmixed to mono and linearly resampled to 44100 in `crash_pump()` | the harness device is 44100 mono |

---

## 7. Reproduce

```bash
python3 tools/extract_eatrax.py            # -> build/music/track_00..43.wav
python3 tools/extract_rws.py               # -> build/audio/rws_crashNN/*.wav
python3 tools/validate_music.py            # 100/100 green, prints the track list
python3 tools/validate_hud.py              # 310/310 green
make && ./burnout3
```

Headless crash-bed check:

```bash
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy B3_AUTODRIVE=1 \
  B3_FIXED_DT=0.0166 B3_TEST_CRASH_AT=10 B3_EXIT_AT=30 B3_MUSIC_LOG=1 \
  ./burnout3
```

`validate_music.py` builds `build/music_probe` and drives the real
`b3_music_pick_next()` / `b3_hud_music_box()` — the selection and banner
checks are not a Python re-implementation.
