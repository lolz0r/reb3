#ifndef BURNOUT3_MUSIC_H
#define BURNOUT3_MUSIC_H
/*
 * burnout3_music.h -- the EA TRAX music system.
 *
 * WHAT IS RECOVERED  (docs/RE_MUSIC.md)
 * -------------------------------------
 * The SONG TABLE is the game's own.  It lives at VA 0x003EC458 in the
 * retail XBE: 44 entries, 24-byte stride,
 *
 *      +0x00  u32 title  string index into Data/Globalus.bin
 *      +0x04  u32 album  string index
 *      +0x08  u32 artist string index
 *      +0x0C  u32 per-track enable byte -- rewritten every time the music
 *             options are applied, by the loop at 0x00152ED0:
 *                 for (i = 0; i < 44; i++)
 *                     table[i].enable = ((u8*)0x004AE1A0)[i];
 *             which is what pins BOTH the 24-byte stride and the count
 *             (the loop bound is `off < 0x420` = 44*24).            [C]
 *      +0x10  u32 1-based ordinal (entry 43 holds 0)                [?]
 *      +0x14  u32 0 (entry 43 holds 44)                             [?]
 *
 * Entries 0..39 use the contiguous Globalus block 456..575 (title, album,
 * artist repeating); entries 40..43 were appended late and use 3255..3266.
 * The neighbouring Globalus strings 449..455 are the EA TRAX options this
 * table is filtered by: "ALL" / "RACE ONLY" / "MENU ONLY" / "OFF",
 * "PLAY TRACKS RANDOMLY" / "PLAY TRACKS SEQUENTIALLY" / "NEXT SOUNDTRACK".
 *
 * The AUDIO is the game's own too: table index i is bank `i / 22`, wave
 * `i % 22` of Tracks/_EATrax0.xwb / _EATrax1.xwb (44 wmav2 entries, all
 * 44100 Hz stereo -- docs/AUDIO_NOTES.md 1).  tools/extract_eatrax.py
 * decodes them to build/music/track_NN.wav as 44100 Hz mono s16.  The
 * index->wave assignment is [S]: it is the only order that makes the two
 * 22-entry banks cover the 44-entry table, and the decoded durations
 * corroborate it independently (index 7 = Ramones "I Wanna Be Sedated"
 * decodes to 2:29, index 6 = The Von Bondies "C'mon C'mon" to 2:13,
 * index 32 = The Bouncing Souls "Sing Along Forever" to 1:35).
 *
 * WHAT IS GLUE
 * ------------
 * Everything below the table: the shuffle, the auto-advance, the stream
 * reader, the ducking and the banner timing are harness code written to
 * the *described* retail behaviour, not decompiled.  They are marked GLUE
 * in docs/RE_MUSIC.md and carry no addresses.
 *
 * OWNERSHIP: this module (both files) belongs to the music agent.  The
 * harness calls only this contract; burnout3_full.c call sites are patched
 * by the orchestrator on landing (scratchpad/music/integration_music.md).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== *
 * B3MUSIC-TABLE-BEGIN
 *   Machine-checked block: tools/validate_music.py parses every
 *       #define B3MUSIC_NAME <value>   /-* @VA note *-/
 *   line and re-reads <value> out of build/burnout3.elf at VA.
 * ===================================================================== */
#define B3MUSIC_TABLE_VA        0x003EC458  /* the song table            */
#define B3MUSIC_TABLE_STRIDE            24  /* add 0x18 @0x00152F13      */
#define B3MUSIC_TABLE_BYTES        0x00000420 /* cmp @0x00152F16, 44*24  */
#define B3MUSIC_TRACKS                  44  /* = BYTES / STRIDE          */
#define B3MUSIC_ENABLE_OFF           0x0C   /* base 0x003EC464 @0x00152EF9 */
#define B3MUSIC_ENABLE_SRC      0x004AE1A0  /* u8[44] @0x00152F05        */
#define B3MUSIC_BANKS                    2  /* _EATrax0.xwb/_EATrax1.xwb */
#define B3MUSIC_BANK_WAVES              22  /* entries per bank          */
/* the EA TRAX option strings that sit just below the table's block */
#define B3MUSIC_STR_ALL                449  /* Globalus "ALL"            */
#define B3MUSIC_STR_RACE_ONLY          450
#define B3MUSIC_STR_MENU_ONLY          451
#define B3MUSIC_STR_OFF                452
#define B3MUSIC_STR_RANDOM             453  /* "PLAY TRACKS RANDOMLY"    */
#define B3MUSIC_STR_SEQUENTIAL         454  /* "PLAY TRACKS SEQUENTIALLY"*/
#define B3MUSIC_STR_NEXT               455  /* "NEXT SOUNDTRACK"         */
/* B3MUSIC-TABLE-END */

/* The output device the harness opens: 44100 Hz, mono, s16 -- the waves
 * are stored in exactly that format so playback needs no resampling. */
#define B3MUSIC_RATE                 44100

/* ===================================================================== *
 *  THE CRASH BED                                                     [C]
 *
 *  Retail lays a PRE-RENDERED 30 s stereo stream over the race whenever a
 *  car crashes.  All of the following is read out of build/burnout3.elf;
 *  the addresses are checked back by tools/validate_music.py [7].
 *
 *  SELECTION  `sprintf(mgr+0x4B0, "tracks\\crash%d.rws",
 *                      (mgr[0x890] % 20) + 1); mgr[0x890]++;`
 *             @0x0014C269 (race init) and @0x00151239 (the rotate).  A
 *             SEQUENTIAL u8 rotation over Tracks/crash1.rws..crash20.rws,
 *             not a random draw.  The race-init pick bumps the counter
 *             TWICE (0x0014C282 and again at 0x0014C2D6), so consecutive
 *             races step by two; every crash inside a race steps by one.
 *             The file is opened and buffered AHEAD of the crash
 *             (FUN_001CB6C0 @0x001512A0, gated on the stream being idle
 *             and mgr+0x8DB clear) and only unpaused when a car crashes.
 *
 *  LAYERS     crashNN.rws holds TWO sub-streams.  FUN_001CB9E0 proves the
 *             stream object's per-sub-stream volume slots are +0x15C and
 *             +0x160 (`obj + 0x15C + chan*4`), and FUN_00150E80 picks
 *             between them off the TIME DIVISOR:
 *                 0x00150F4F  CMP dword ptr [0x0060EA18], 0x1
 *                 divisor == 1 -> +0x15C = vol, +0x160 = 0   (aGenCrashNN)
 *                 divisor != 1 -> +0x15C = 0,   +0x160 = vol (zSloCrashNN)
 *             i.e. the SLOW-MOTION layer plays exactly while the crash
 *             cinematic's dilation runs, and the general layer whenever
 *             the clock is at normal speed.  A hard cut, no crossfade;
 *             both sub-streams share one file position.  mgr+0x8DF caches
 *             the choice (also recomputed at 0x0014CB80).
 *
 *  LEVEL      vol = mgr+0x834 (initialised to 1.0 @0x0014B534) *
 *             [0x003EC418] (0.7) = 0.70, against the EA TRAX stream's own
 *             mgr+0x83C = [0x003EC424] = 0.30 (@0x0014B59E).  The bed is
 *             2.33x the song and NOTHING ducks: the audio mix state
 *             machine's crash row leaves 14 of its 15 groups at 1.0
 *             (docs/RE_SFX.md 6).
 *
 *  END        crash over, stream still up:  arm at the first frame
 *             (mgr+0x8B0 = clock, mgr+0x8B4 = clock + 0.5 [0x003B1684],
 *             mgr+0x8C0 = the layer in use), then
 *             vol = (mgr+0x8B4 - clock) * 2.0 [0x003B1688] * 0.70 on that
 *             REMEMBERED layer only (@0x00151117), and STOP once the clock
 *             passes mgr+0x8B4 (@0x001510F9).  The clock is DAT_0060EA20,
 *             the dilated one.
 *
 *  GATES      the whole bed is single-player only (`DAT_0073A1C0 <= 1`,
 *             @0x00150D4C and 0x00150E86) and the trigger FUN_00150D40
 *             additionally wants the crash within 50.0 units of the
 *             listener ([0x003B16B8], COMISS @0x00150DC0).
 * ===================================================================== */

/* B3MUSIC-CRASHBED-BEGIN
 *   Machine-checked block, same contract as B3MUSIC-TABLE above.
 *   ..._VA defines name a float the validator re-reads out of the image;
 *   the value it must hold is the matching ..._F define.               */
#define B3MUSIC_CRASH_FILES             20  /* IDIV 0x14 @0x00151248     */
#define B3MUSIC_CRASH_LAYERS             2  /* +0x15C/+0x160 @0x001CB9E0 */
#define B3MUSIC_CRASH_FMT_VA    0x003AED84  /* "tracks\crash%d.rws"      */
#define B3MUSIC_CRASH_DIVISOR_VA 0x0060EA18 /* CMP ...,1 @0x00150F4F     */
#define B3MUSIC_CRASH_VOL_VA    0x003EC418  /* MULSS @0x00150F77/0F A1   */
#define B3MUSIC_CRASH_VOL_F          0.70f  /* the value there           */
#define B3MUSIC_CRASH_BASE_F         1.00f  /* mgr+0x834 @0x0014B534     */
#define B3MUSIC_SONG_VOL_VA     0x003EC424  /* mgr+0x83C @0x0014B59E     */
#define B3MUSIC_SONG_VOL_F           0.30f  /* the value there           */
#define B3MUSIC_CRASH_FADE_VA   0x003B1684  /* ADDSS @0x001510D0         */
#define B3MUSIC_CRASH_FADE_F         0.50f  /* seconds, dilated clock    */
#define B3MUSIC_CRASH_FADE_K_VA 0x003B1688  /* MULSS @0x0015113A         */
#define B3MUSIC_CRASH_FADE_K_F       2.00f  /* = 1 / FADE                */
#define B3MUSIC_CRASH_DIST_VA   0x003B16B8  /* COMISS @0x00150DC0        */
#define B3MUSIC_CRASH_DIST_F        50.00f  /* trigger radius, units     */
/* B3MUSIC-CRASHBED-END */

/* The beds as tools/extract_rws.py wrote them: build/audio/rws_crashNN/
 * {aGenCrashNN,zSloCrashNN}.wav, 32000 Hz stereo s16, 30.0 s each.  They
 * are downmixed and resampled to the device rate inside this module.    */
#define B3MUSIC_CRASH_SRC_RATE       32000
#define B3MUSIC_CRASH_SECONDS           30
#define B3MUSIC_CRASH_LAYER_GEN          0  /* aGenCrashNN -> obj+0x15C  */
#define B3MUSIC_CRASH_LAYER_SLO          1  /* zSloCrashNN -> obj+0x160  */

/* ---- the song table -------------------------------------------------- */

typedef struct B3MusicTrack {
    const char    *artist;      /* Globalus[artist_id]                    */
    const char    *title;       /* Globalus[title_id]                     */
    const char    *album;       /* Globalus[album_id]                     */
    unsigned short title_id;    /* entry +0x00                            */
    unsigned short album_id;    /* entry +0x04                            */
    unsigned short artist_id;   /* entry +0x08                            */
    unsigned char  bank;        /* 0 = _EATrax0.xwb, 1 = _EATrax1.xwb     */
    unsigned char  wave;        /* entry index inside that bank           */
} B3MusicTrack;

int                 b3_music_track_count(void);
const B3MusicTrack *b3_music_track(int i);       /* NULL out of range     */

/* build/music/track_NN.wav -- the file extract_eatrax.py writes.  Fills
 * `out` and returns it (never NULL).                                     */
char *b3_music_track_path(int i, char *out, int cap);

/* ---- selection (GLUE: a no-immediate-repeat shuffle bag) ------------- */

/* Deterministic reseed; the default seed is time(NULL).  The bag is
 * rebuilt on the next pick.                                             */
void b3_music_seed(unsigned s);

/* Next track index from the bag.  Draws a fresh shuffled permutation of
 * every PLAYABLE track when the bag empties, and rerolls the new bag's
 * head if it would repeat the track that just finished.  Pure: no audio
 * is touched.  Returns -1 when nothing is playable.                      */
int  b3_music_pick_next(void);

/* Enable/disable one track (the retail per-track EA TRAX option).  A
 * disabled track is never drawn from the bag.  Default: all enabled.     */
void b3_music_set_enabled(int i, int on);
int  b3_music_enabled(int i);

/* ---- runtime -------------------------------------------------------- */

/* Point the module at the wave directory (default "build/music").        */
void b3_music_set_dir(const char *dir);

/* Scan the directory; returns the number of playable tracks (0 = the
 * extractor has not been run; every call below then no-ops).             */
int  b3_music_init(void);
void b3_music_shutdown(void);

/* Start the race soundtrack: picks a track and begins streaming it.
 * Returns the track index, or -1.  Safe to call repeatedly.              */
int  b3_music_start_race(void);

/* Play one specific track (the validator and the debug key use this).    */
int  b3_music_play(int track);

/* Retail "NEXT SOUNDTRACK": jump to the next track in the bag.           */
int  b3_music_skip(void);
void b3_music_stop(void);

/* Refill the stream buffer.  MAIN THREAD, once per frame.  Advances to
 * the next track when the current one runs out.  Cheap when full.        */
void b3_music_pump(void);

/* ---- the now-playing banner feed ------------------------------------ */

/* What the mixer is *actually* emitting right now (not what pump() has
 * queued).  Returns 1 when a track is playing and fills any non-NULL
 * out-parameter; `elapsed` is seconds since its first sample was mixed.  */
int  b3_music_now_playing(const char **artist, const char **title,
                          float *elapsed);
int  b3_music_current(void);     /* track index, -1 = silent             */

/* ---- mixing (audio thread) ------------------------------------------ */

/* One output frame at 44100 Hz, in the same s16-scaled float domain
 * burnout3_full.c's audio_callback works in -- the same contract as
 * b3_sfx_next_sample().  Returns 0 on underrun.                          */
float b3_music_next_sample(void);

/* Block form: ADDS `frames` samples into an existing mono s16 buffer.    */
void  b3_music_mix_s16(int16_t *out, int frames);

void  b3_music_set_master(float g);   /* default 0.30                     */
float b3_music_master(void);

/* Duck target, 0..1, applied on top of master and slewed over
 * B3_MUSIC_DUCK_SLEW seconds so it cannot click.  1 = no ducking.        */
void  b3_music_set_duck(float g);

/* Diagnostics: tracks started since init, and samples lost to underrun. */
unsigned b3_music_tracks_started(void);
unsigned b3_music_underruns(void);

/* ---- the crash bed --------------------------------------------------- *
 *
 * The bed rides inside this module: b3_music_pump() fills its ring too and
 * b3_music_next_sample() already carries it, so the harness only has to
 * say when a race starts and what the crash state is.
 * ---------------------------------------------------------------------- */

/* Where build/audio/rws_crashNN/ lives (default "build/audio").          */
void b3_music_crash_set_dir(const char *dir);

/* RACE START.  Ports the pick at 0x0014C269: takes the next name off the
 * sequential rotation, opens it, pre-buffers it and leaves it paused --
 * and bumps the rotation the extra time retail does at 0x0014C2D6, so the
 * across-races sequence is 1, 3, 5, ...  Returns the file number 1..20,
 * or -1 when the beds are not on disc.                                   */
int  b3_music_crash_arm(void);

/* PER FRAME, main thread -- the port of FUN_00150E80(mgr, crash_active).
 *
 *   crash_active  retail ORs every local player's crashing byte (+0x236)
 *                 at 0x0014CAA0; in the harness that is
 *                 `g_player.crashed_until > 0.0f`.
 *   divisor       DAT_0060EA18, the current time divisor (b3_tdfx_status's
 *                 `divisor`).  1 selects aGenCrashNN, anything else
 *                 selects zSloCrashNN.
 *   dt            the DILATED per-frame dt, because the 0.5 s release is
 *                 measured on the dilated clock DAT_0060EA20.
 *
 * Starting, layer selection, the release fade, the stop and the rotation
 * to the next file all happen in here; there is nothing else to call.    */
void b3_music_crash_tick(int crash_active, int divisor, float dt);

/* RACE END / teardown.  Hard stop, releases the song's duck.             */
void b3_music_crash_stop(void);

/* ---- crash-bed diagnostics (the validator and B3_MUSIC_LOG read these) */
int      b3_music_crash_file(void);      /* 1..20 loaded, 0 = none        */
int      b3_music_crash_playing(void);   /* 1 while the bed is audible    */
int      b3_music_crash_layer(void);     /* B3MUSIC_CRASH_LAYER_*, -1 off */
float    b3_music_crash_gain(void);      /* the recovered 0..0.70 volume  */
float    b3_music_crash_elapsed(void);   /* seconds of bed emitted        */
float    b3_music_crash_duck(void);      /* the song duck the bed asks for*/
unsigned b3_music_crash_starts(void);    /* beds started since init       */
unsigned b3_music_crash_underruns(void);

/* The pure rotation law, for tests: the file number retail's counter
 * yields at `counter`, i.e. (counter % B3MUSIC_CRASH_FILES) + 1.         */
int  b3_music_crash_pick(unsigned counter);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_MUSIC_H */
