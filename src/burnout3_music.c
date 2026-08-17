/*
 * burnout3_music.c -- the EA TRAX music system.
 *
 * Two halves, and the line between them is the point of this file:
 *
 *   [C]  THE SONG TABLE below is the game's own, transcribed from the
 *        44-entry table at VA 0x003EC458 (see burnout3_music.h for the
 *        field map and the 0x00152ED0 loop that pins stride and count).
 *        The artist/title/album text is Data/Globalus.bin at the indices
 *        the table carries -- the three ids on every row -- ASCII-folded
 *        (U+2019 -> ') because the recovered GlobalFont only has glyphs
 *        0x20..0x7E.  tools/validate_music.py re-reads all 132 ids out of
 *        build/burnout3.elf and re-resolves every string, so this table
 *        cannot drift from the binary without the validator failing.
 *
 *   GLUE Everything else: the shuffle bag, the auto-advance, the WAV
 *        stream reader and the ducking slew.  Written to the described
 *        retail behaviour ("PLAY TRACKS RANDOMLY", "NEXT SOUNDTRACK"),
 *        not decompiled.  No addresses are claimed for any of it.
 *
 * Threading: b3_music_pump() runs on the main thread and is the only
 * writer of the ring; b3_music_next_sample() runs on the SDL audio thread
 * and is the only reader.  The two share a pair of monotonically
 * increasing 64-bit counters, so no lock is needed on the platforms this
 * harness targets (aligned 64-bit loads/stores are atomic on x86-64).
 */
#include "burnout3_music.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ===================================================================== *
 *  THE SONG TABLE -- 0x003EC458, 44 entries                          [C]
 * ===================================================================== */

static const B3MusicTrack B3_SONGS[B3MUSIC_TRACKS] = {
    /*   artist                      title                                          album                                          tid  alid arid bk wv */
    { "No Motiv",                  "Independence Day",                            "Daylight Breaking",                            456,  457,  458, 0,  0 },
    { "Amber Pacific",             "Always You",                                  "Fading Days",                                  459,  460,  461, 0,  1 },
    { "The Ordinary Boys",         "Over The Counter Culture",                    "Over The Counter Culture",                     462,  463,  464, 0,  2 },
    { "Funeral For A Friend",      "Rookie Of The Year",                          "Casually Dressed & Deep In Conversation",      465,  466,  467, 0,  3 },
    { "Chronic Future",            "Time And Time Again",                         "Lines In My Face",                             468,  469,  470, 0,  4 },
    { "Franz Ferdinand",           "This Fire",                                   "Franz Ferdinand",                              471,  472,  473, 0,  5 },
    { "The Von Bondies",           "C'mon C'mon",                                 "Pawn Shoppe Heart",                            474,  475,  476, 0,  6 },
    { "Ramones",                   "I Wanna Be Sedated",                          "EA Throwback Trax",                            477,  478,  479, 0,  7 },
    { "Autopilot Off",             "Make A Sound",                                "Make A Sound",                                 480,  481,  482, 0,  8 },
    { "Ash",                       "Orpheus",                                     "Meltdown",                                     483,  484,  485, 0,  9 },
    { "Yellowcard",                "Breathing",                                   "Ocean Avenue",                                 486,  487,  488, 0, 10 },
    { "Pennywise",                 "Rise Up",                                     "From The Ashes",                               489,  490,  491, 0, 11 },
    { "Fall Out Boy",              "Reinventing The Wheel To Run Myself Over",    "Take This To Your Grave",                      492,  493,  494, 0, 12 },
    { "The FUps",                  "Lazy Generation",                             "..... ...",                                    495,  496,  497, 0, 13 },
    { "The Lot Six",               "Autobrats",                                   "Major Fables",                                 498,  499,  500, 0, 14 },
    { "Sahara Hotnights",          "Hot Night Crash",                             "Kiss & Tell",                                  501,  502,  503, 0, 15 },
    { "Eighteen Visions",          "I Let Go",                                    "Obsession",                                    504,  505,  506, 0, 16 },
    { "Donots",                    "Saccharine Smile",                            "Amplify The Good Times",                       507,  508,  509, 0, 17 },
    { "From First To Last",        "Populace In Two",                             "Dear Diary, My Teen Angst Has A Bodycount",    510,  511,  512, 0, 18 },
    { "Sugarcult",                 "Memory",                                      "Palm Trees And Power Lines",                   513,  514,  515, 0, 19 },
    { "Finger Eleven",             "Stay In Shadow",                              "Finger Eleven",                                516,  517,  518, 0, 20 },
    { "Reggie And The Full Effect","Congratulations Smack And Katy",              "Under The Tray",                               519,  520,  521, 0, 21 },
    { "Local H",                   "Everyone Alive",                              "Whatever Happened To P.J. Soles?",             522,  523,  524, 1,  0 },
    { "Maxeen",                    "Please",                                      "Maxeen",                                       525,  526,  527, 1,  1 },
    { "New Found Glory",           "At Least I'm Known For Something",            "Catalyst",                                     528,  529,  530, 1,  2 },
    { "My Chemical Romance",       "I'm Not Okay (I Promise)",                    "Three Cheers For Sweet Revenge",               531,  532,  533, 1,  3 },
    { "Go Betty Go",               "C'mon",                                       "Worst Enemy",                                  534,  535,  536, 1,  4 },
    { "Moments In Grace",          "Broken Promises",                             "Moonlight Survived",                           537,  538,  539, 1,  5 },
    { "Midtown",                   "Give It Up",                                  "Forget What You Know",                         540,  541,  542, 1,  6 },
    { "1208",                      "Fall Apart",                                  "Turn of the Screw",                            543,  544,  545, 1,  7 },
    { "Motion City Soundtrack",    "My Favorite Accident",                        "I Am The Movie",                               546,  547,  548, 1,  8 },
    { "Rise Against",              "Paper Wings",                                 "Siren Song Of The Counter Culture",            549,  550,  551, 1,  9 },
    { "The Bouncing Souls",        "Sing Along Forever",                          "Anchors Aweigh",                               552,  553,  554, 1, 10 },
    { "The Matches",               "Audio Blood",                                 "E. Von Dahl Killed The Locals",                555,  556,  557, 1, 11 },
    { "Silent Drive",              "4/16",                                        "Love Is Worth It",                             558,  559,  560, 1, 12 },
    { "The Explosion",             "Here I Am",                                   "Black Tape",                                   561,  562,  563, 1, 13 },
    { "The D4",                    "Come On!",                                    "6Twenty",                                      564,  565,  566, 1, 14 },
    { "The Mooney Suzuki",         "Shake That Bush Again",                       "Alive & Amplified",                            567,  568,  569, 1, 15 },
    { "Mudmen",                    "Animal",                                      "Overrated",                                    570,  571,  572, 1, 16 },
    { "The Futureheads",           "Decent Days And Nights",                      "The Futureheads",                              573,  574,  575, 1, 17 },
    { "Burning Brides",            "Heart Full Of Black",                         "Leave No Ashes",                              3255, 3256, 3257, 1, 18 },
    { "Atreyu",                    "Right Side Of The Bed",                       "The Curse",                                   3258, 3259, 3260, 1, 19 },
    { "Letter Kills",              "Radio Up",                                    "The Bridge",                                  3261, 3262, 3263, 1, 20 },
    { "Jimmy Eat World",           "Just Tonight...",                             "Futures",                                     3264, 3265, 3266, 1, 21 },
};

int b3_music_track_count(void) { return B3MUSIC_TRACKS; }

const B3MusicTrack *b3_music_track(int i) {
    if (i < 0 || i >= B3MUSIC_TRACKS) return NULL;
    return &B3_SONGS[i];
}

/* ===================================================================== *
 *  Module state
 * ===================================================================== */

#define RING_BITS   19                       /* 524288 samples = 11.9 s   */
#define RING_SIZE   (1u << RING_BITS)
#define RING_MASK   (RING_SIZE - 1u)
#define READ_CHUNK  16384                    /* samples per fread         */
#define XFADE       (B3MUSIC_RATE / 4)       /* 0.25 s track-edge ramp    */

/* how fast b3_music_set_duck() reaches its target, seconds */
#define B3_MUSIC_DUCK_SLEW  0.20f

static char  g_dir[256] = "build/music";
static char  g_have[B3MUSIC_TRACKS];         /* file exists and parsed    */
static char  g_enabled[B3MUSIC_TRACKS];      /* the per-track EA TRAX opt */
static int   g_playable;
static int   g_ready;

/* ---- the shuffle bag (GLUE) ----------------------------------------- */
static unsigned g_rng = 0;
static int      g_bag[B3MUSIC_TRACKS];
static int      g_bag_n;                     /* entries left in the bag   */
static int      g_last_picked = -1;

/* ---- the stream ------------------------------------------------------ */
static FILE    *g_fp;
static int      g_fp_track = -1;
static long     g_fp_left;                   /* samples left in the file  */
static long     g_fp_total;

static short    g_ring[RING_SIZE];
/* absolute sample counters; g_wr is main-thread-owned, g_rd audio-owned */
static volatile unsigned long long g_wr, g_rd;

/* pending track boundaries, written by pump(), consumed by the mixer */
#define MARKS 8
static struct { unsigned long long at; int track; long len; } g_mark[MARKS];
static volatile unsigned g_mark_w, g_mark_r;

/* what the mixer is actually emitting */
static volatile int                g_now_track = -1;
static volatile unsigned long long g_now_start;
static volatile long               g_now_len;

static float g_master = 0.30f;
static float g_duck_target = 1.0f;
static float g_duck = 1.0f;
static unsigned g_started, g_underruns;

/* the crash bed rides in the same pump and the same mixer; see the CRASH
 * BED section at the bottom of this file */
static void  crash_pump(void);
static float crash_next_sample(void);
static void  crash_close_files(void);
static void  crash_module_reset(void);

/* ===================================================================== *
 *  Selection -- GLUE
 * ===================================================================== */

static unsigned rng_next(void) {
    /* the classic MSVC LCG shape; any decent 32-bit LCG would do */
    g_rng = g_rng * 214013u + 2531011u;
    return (g_rng >> 16) & 0x7FFFu;
}

void b3_music_seed(unsigned s) { g_rng = s ? s : 1u; g_bag_n = 0; }

void b3_music_set_enabled(int i, int on) {
    if (i >= 0 && i < B3MUSIC_TRACKS) g_enabled[i] = on ? 1 : 0;
}

int b3_music_enabled(int i) {
    return (i >= 0 && i < B3MUSIC_TRACKS) ? g_enabled[i] : 0;
}

static int track_ok(int i) { return g_have[i] && g_enabled[i]; }

/* Fisher-Yates over every playable track. */
static void bag_refill(void) {
    g_bag_n = 0;
    for (int i = 0; i < B3MUSIC_TRACKS; i++)
        if (track_ok(i)) g_bag[g_bag_n++] = i;
    for (int i = g_bag_n - 1; i > 0; i--) {
        int j = (int)(rng_next() % (unsigned)(i + 1));
        int t = g_bag[i]; g_bag[i] = g_bag[j]; g_bag[j] = t;
    }
    /* no immediate repeat across the bag seam: if the new bag would open
     * on the track that just played, swap it with a different one. */
    if (g_bag_n > 1 && g_bag[g_bag_n - 1] == g_last_picked) {
        int j = (int)(rng_next() % (unsigned)(g_bag_n - 1));
        int t = g_bag[g_bag_n - 1];
        g_bag[g_bag_n - 1] = g_bag[j];
        g_bag[j] = t;
    }
}

int b3_music_pick_next(void) {
    if (!g_rng) b3_music_seed((unsigned)time(NULL));
    if (g_bag_n <= 0) bag_refill();
    if (g_bag_n <= 0) return -1;             /* nothing playable          */
    g_last_picked = g_bag[--g_bag_n];        /* draw from the top         */
    return g_last_picked;
}

/* ===================================================================== *
 *  WAV stream reader -- GLUE
 * ===================================================================== */

char *b3_music_track_path(int i, char *out, int cap) {
    snprintf(out, (size_t)cap, "%s/track_%02d.wav", g_dir, i);
    return out;
}

/* Opens `path`, parses the RIFF header and leaves the handle positioned at
 * the first sample.  Returns the s16 sample count (frames * channels), or
 * -1, and reports the format through the out-parameters. */
static long wav_open_path(const char *path, FILE **out, int *rate_out,
                          int *ch_out) {
    unsigned char hdr[12];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fread(hdr, 1, 12, f) != 12 ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f); return -1;
    }
    int rate = 0, ch = 0, bits = 0;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, f) != 8) { fclose(f); return -1; }
        unsigned long sz = (unsigned long)ck[4] | ((unsigned long)ck[5] << 8) |
                           ((unsigned long)ck[6] << 16) | ((unsigned long)ck[7] << 24);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fm[16];
            if (sz < 16 || fread(fm, 1, 16, f) != 16) { fclose(f); return -1; }
            ch   = fm[2] | (fm[3] << 8);
            rate = fm[4] | (fm[5] << 8) | (fm[6] << 16) | (fm[7] << 24);
            bits = fm[14] | (fm[15] << 8);
            if (sz > 16) fseek(f, (long)(sz - 16), SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) {
            if (bits != 16 || ch < 1 || ch > 2 || rate <= 0) {
                fclose(f); return -1;
            }
            if (rate_out) *rate_out = rate;
            if (ch_out)   *ch_out   = ch;
            *out = f;
            return (long)(sz / 2);
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
}

/* Opens track_NN.wav, checks it is 44100/mono/s16 and leaves the handle
 * positioned at the first sample.  Returns sample count, or -1. */
static long wav_open(int track, FILE **out) {
    char path[320];
    FILE *f = NULL;
    int rate = 0, ch = 0;
    long n;
    b3_music_track_path(track, path, sizeof(path));
    n = wav_open_path(path, &f, &rate, &ch);
    if (n < 0) return -1;
    if (rate != B3MUSIC_RATE || ch != 1) { fclose(f); return -1; }
    *out = f;
    return n;
}

static void stream_close(void) {
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    g_fp_track = -1;
    g_fp_left = g_fp_total = 0;
}

/* Queue `track` as the next thing the ring will carry.  The mark tells
 * the mixer where in the stream the new track begins so the banner turns
 * over exactly when the first sample is heard, not when it is read. */
static int stream_open(int track) {
    FILE *f = NULL;
    long n;
    if (track < 0 || track >= B3MUSIC_TRACKS) return -1;
    n = wav_open(track, &f);
    if (n <= 0) { g_have[track] = 0; return -1; }
    if (g_fp) fclose(g_fp);
    g_fp = f;
    g_fp_track = track;
    g_fp_left = g_fp_total = n;

    unsigned w = g_mark_w;
    if (w - g_mark_r < MARKS) {
        g_mark[w % MARKS].at = g_wr;
        g_mark[w % MARKS].track = track;
        g_mark[w % MARKS].len = n;
        g_mark_w = w + 1;
    }
    g_started++;
    return track;
}

/* ===================================================================== *
 *  Public runtime
 * ===================================================================== */

void b3_music_set_dir(const char *dir) {
    if (!dir || !*dir) return;
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
}

int b3_music_init(void) {
    char path[320];
    crash_module_reset();   /* retail zeroes the rotation in the audio ctor */
    g_playable = 0;
    for (int i = 0; i < B3MUSIC_TRACKS; i++) {
        FILE *f = NULL;
        long n = wav_open(i, &f);
        if (f) fclose(f);
        g_have[i] = (n > 0);
        g_enabled[i] = 1;
        if (g_have[i]) g_playable++;
    }
    g_wr = g_rd = 0;
    g_mark_w = g_mark_r = 0;
    g_now_track = -1;
    g_bag_n = 0;
    g_last_picked = -1;
    g_ready = 1;
    if (!g_playable) {
        b3_music_track_path(0, path, sizeof(path));
        fprintf(stderr, "[b3_music] no playable tracks (looked for %s) -- "
                        "run tools/extract_eatrax.py\n", path);
    }
    return g_playable;
}

void b3_music_shutdown(void) {
    b3_music_crash_stop();
    crash_close_files();
    stream_close();
    g_ready = 0;
    g_playable = 0;
    g_now_track = -1;
    g_wr = g_rd = 0;
    g_mark_w = g_mark_r = 0;
    memset(g_ring, 0, sizeof(g_ring));
}

void b3_music_stop(void) {
    stream_close();
    g_rd = g_wr;                 /* drop whatever is buffered            */
    g_mark_r = g_mark_w;
    g_now_track = -1;
}

int b3_music_play(int track) {
    if (!g_ready || track < 0 || track >= B3MUSIC_TRACKS) return -1;
    if (!g_have[track]) return -1;
    b3_music_stop();
    g_last_picked = track;
    if (stream_open(track) < 0) return -1;
    b3_music_pump();
    return track;
}

int b3_music_start_race(void) {
    int t;
    if (!g_ready) b3_music_init();
    t = b3_music_pick_next();
    if (t < 0) return -1;
    return b3_music_play(t);
}

int b3_music_skip(void) {
    int t;
    if (!g_ready) return -1;
    t = b3_music_pick_next();
    if (t < 0) return -1;
    return b3_music_play(t);
}

void b3_music_pump(void) {
    short buf[READ_CHUNK];
    crash_pump();                 /* the crash bed's ring, same cadence   */
    if (!g_ready || !g_playable) return;
    for (;;) {
        unsigned long long used = g_wr - g_rd;
        unsigned long long space = RING_SIZE - used;
        size_t want, got;
        if (space <= READ_CHUNK) return;              /* full enough      */
        if (!g_fp) {
            /* the previous track ran out: roll on to the next one */
            int t = b3_music_pick_next();
            if (t < 0 || stream_open(t) < 0) return;
        }
        want = READ_CHUNK;
        if ((long)want > g_fp_left) want = (size_t)g_fp_left;
        got = fread(buf, 2, want, g_fp);
        if (got < want) g_fp_left = 0; else g_fp_left -= (long)got;
        for (size_t i = 0; i < got; i++)
            g_ring[(unsigned)((g_wr + i) & RING_MASK)] = buf[i];
        g_wr += got;
        if (g_fp_left <= 0 || got == 0) stream_close();
        if (got == 0) return;
    }
}

/* ===================================================================== *
 *  Mixing (audio thread)
 * ===================================================================== */

void b3_music_set_master(float g) {
    if (g < 0.f) g = 0.f;
    if (g > 4.f) g = 4.f;
    g_master = g;
}

float b3_music_master(void) { return g_master; }

void b3_music_set_duck(float g) {
    if (g < 0.f) g = 0.f;
    if (g > 1.f) g = 1.f;
    g_duck_target = g;
}

/* Apply the track-edge ramp so a cut never clicks: 0.25 s in at the head
 * of a track and 0.25 s out at its tail. */
static float edge_gain(unsigned long long pos) {
    long len = g_now_len;
    unsigned long long start = g_now_start;
    if (len <= 0) return 1.f;
    unsigned long long o = pos - start;
    float g = 1.f;
    if (o < (unsigned long long)XFADE)
        g = (float)o / (float)XFADE;
    if ((long)o > len - XFADE) {
        float t = (float)(len - (long)o) / (float)XFADE;
        if (t < 0.f) t = 0.f;
        if (t < g) g = t;
    }
    return g;
}

float b3_music_next_sample(void) {
    unsigned long long rd = g_rd;
    float bed = crash_next_sample();   /* the pre-rendered crash stream   */
    float s;
    if (!g_ready || rd >= g_wr) {
        if (g_ready && g_playable) g_underruns++;
        g_now_track = -1;
        return bed;
    }
    /* has a queued track boundary come due? */
    while (g_mark_r != g_mark_w) {
        unsigned k = g_mark_r % MARKS;
        if (g_mark[k].at > rd) break;
        g_now_track = g_mark[k].track;
        g_now_start = g_mark[k].at;
        g_now_len   = g_mark[k].len;
        g_mark_r++;
    }
    s = (float)g_ring[(unsigned)(rd & RING_MASK)];
    g_rd = rd + 1;

    /* duck slew, one step per output frame */
    {
        float step = 1.f / (B3_MUSIC_DUCK_SLEW * (float)B3MUSIC_RATE);
        if (g_duck < g_duck_target) {
            g_duck += step; if (g_duck > g_duck_target) g_duck = g_duck_target;
        } else if (g_duck > g_duck_target) {
            g_duck -= step; if (g_duck < g_duck_target) g_duck = g_duck_target;
        }
    }
    return s * g_master * g_duck * edge_gain(rd) + bed;
}

void b3_music_mix_s16(int16_t *out, int frames) {
    for (int i = 0; i < frames; i++) {
        float v = (float)out[i] + b3_music_next_sample();
        if (v >  32767.f) v =  32767.f;
        if (v < -32768.f) v = -32768.f;
        out[i] = (int16_t)v;
    }
}

/* ===================================================================== *
 *  Now playing
 * ===================================================================== */

int b3_music_current(void) {
    int t = g_now_track;
    return (t >= 0 && t < B3MUSIC_TRACKS) ? t : -1;
}

int b3_music_now_playing(const char **artist, const char **title,
                         float *elapsed) {
    int t = b3_music_current();
    if (t < 0) return 0;
    if (artist) *artist = B3_SONGS[t].artist;
    if (title)  *title  = B3_SONGS[t].title;
    if (elapsed) {
        unsigned long long rd = g_rd, st = g_now_start;
        *elapsed = rd > st ? (float)(rd - st) / (float)B3MUSIC_RATE : 0.f;
    }
    return 1;
}

unsigned b3_music_tracks_started(void) { return g_started; }
unsigned b3_music_underruns(void)      { return g_underruns; }

/* ===================================================================== *
 *  THE CRASH BED -- the port of FUN_00150E80 / FUN_00150D40 / 0x0014C269
 *
 *  Recovered law and addresses: burnout3_music.h, "THE CRASH BED".  What
 *  is retail here is the SELECTION (a sequential u8 rotation over
 *  crash1..20), the LAYER GATE (divisor == 1 -> aGenCrashNN, else
 *  zSloCrashNN, a hard cut, both sub-streams on one file position), the
 *  LEVEL (1.0 * 0.7 = 0.70 against the song's 0.30, nothing ducked) and
 *  the RELEASE (0.5 s linear on the layer that was playing, then stop and
 *  rotate).  Marked below are the three places this deviates.
 * ===================================================================== */

#define CN          B3MUSIC_CRASH_LAYERS
#define CRING_BITS  18                       /* 262144 samples = 5.94 s   */
#define CRING_SIZE  (1u << CRING_BITS)
#define CRING_MASK  (CRING_SIZE - 1u)
#define CSRC        4096                     /* source frames per fread   */

/* GLUE (anti-zipper): the recovered volumes are written once per frame,
 * which on a 44.1 kHz device is a 735-sample step.  The mixer walks the
 * gain to them over 5 ms instead of jumping, so neither the release fade
 * nor the aGen/zSlo hard cut can click.  It does not change the law --
 * at 5 ms the cut is still a cut. */
#define B3_CRASH_SLEW  0.005f

static char   g_cdir[256] = "build/audio";
static FILE  *g_cfp[CN];
static long   g_cleft[CN];                   /* s16 samples left in file  */
static int    g_cch[CN];
static int    g_crate = B3MUSIC_CRASH_SRC_RATE;

static unsigned char g_crot;                 /* retail's mgr+0x890        */
static int    g_cfile;                       /* 1..20 loaded, 0 = none    */
static int    g_cready;                      /* the beds are on disc      */
static int    g_clog;                        /* B3_MUSIC_LOG              */

static short  g_cring[CN][CRING_SIZE];
static volatile unsigned long long g_cwr, g_crd;

/* the 32000 -> 44100 resampler, both layers on one cursor */
static short  g_csrc[CN][CSRC];
static int    g_csrc_n, g_csrc_i, g_ceof;
static short  g_cprev[CN], g_ccur[CN];
static double g_cfrac;

/* the FUN_00150E80 state machine */
static volatile int   g_cplaying;            /* stream state 0x170 == 6   */
static int    g_clayer  = -1;                /* mgr+0x8DF                 */
static int    g_cfading;                     /* mgr+0x8DE                 */
static float  g_cfade_left;                  /* mgr+0x8B4 - clock         */
static int    g_cfade_layer;                 /* mgr+0x8C0                 */
static volatile float g_cgain[CN];           /* the recovered targets     */
static float  g_cgain_now[CN];               /* slewed, audio thread      */
static volatile unsigned long long g_cstart_rd;
static unsigned g_cstarts, g_cunder;
static int    g_cducking;
static unsigned g_ctick;                     /* B3_MUSIC_LOG pacing only  */

void b3_music_crash_set_dir(const char *dir) {
    if (!dir || !*dir) return;
    snprintf(g_cdir, sizeof(g_cdir), "%s", dir);
}

int b3_music_crash_pick(unsigned counter) {
    return (int)(counter % (unsigned)B3MUSIC_CRASH_FILES) + 1;
}

/* build/audio/rws_crashN/{aGen,zSlo}CrashNN.wav -- the directory is not
 * zero-padded (rws_crash7), the wave inside it is (aGenCrash07.wav). */
static void crash_layer_path(int file, int layer, char *out, int cap) {
    snprintf(out, (size_t)cap, "%s/rws_crash%d/%sCrash%02d.wav",
             g_cdir, file, layer == B3MUSIC_CRASH_LAYER_GEN ? "aGen" : "zSlo",
             file);
}

static void crash_close_files(void) {
    for (int i = 0; i < CN; i++) {
        if (g_cfp[i]) { fclose(g_cfp[i]); g_cfp[i] = NULL; }
        g_cleft[i] = 0;
        g_cch[i] = 0;
    }
    g_csrc_n = g_csrc_i = 0;
    g_ceof = 0;
    g_cfrac = 0.0;
    for (int i = 0; i < CN; i++) g_cprev[i] = g_ccur[i] = 0;
}

/* One mono source sample per layer.  Returns 0 at end of file. */
static int crash_src_next(short out[CN]) {
    if (g_csrc_i >= g_csrc_n) {
        static short raw[CSRC * 2];
        int frames = -1;
        if (g_ceof) return 0;
        for (int L = 0; L < CN; L++) {
            int nch = g_cch[L] ? g_cch[L] : 1;
            size_t need = (size_t)CSRC * (size_t)nch;
            size_t got;
            if ((long)need > g_cleft[L]) need = (size_t)g_cleft[L];
            got = (need && g_cfp[L]) ? fread(raw, 2, need, g_cfp[L]) : 0;
            g_cleft[L] -= (long)got;
            int nf = (int)(got / (size_t)nch);
            for (int i = 0; i < nf; i++)
                g_csrc[L][i] = (nch == 2)
                    ? (short)((raw[i * 2] + raw[i * 2 + 1]) / 2)
                    : raw[i];
            if (frames < 0 || nf < frames) frames = nf;
        }
        if (frames <= 0) { g_ceof = 1; g_csrc_n = g_csrc_i = 0; return 0; }
        g_csrc_n = frames;
        g_csrc_i = 0;
    }
    for (int L = 0; L < CN; L++) out[L] = g_csrc[L][g_csrc_i];
    g_csrc_i++;
    return 1;
}

/* Refill the bed ring at the device rate.  Main thread, from
 * b3_music_pump().  Cheap when full, and full is the resting state --
 * the mixer only drains it while the bed is audible. */
static void crash_pump(void) {
    const double ratio = (double)g_crate / (double)B3MUSIC_RATE;
    if (!g_cfile) return;
    for (;;) {
        unsigned long long used = g_cwr - g_crd;
        if (used + 1 >= (unsigned long long)CRING_SIZE) return;
        if (g_ceof && g_csrc_i >= g_csrc_n) return;
        {
            unsigned idx = (unsigned)(g_cwr & CRING_MASK);
            for (int L = 0; L < CN; L++) {
                float a = (float)g_cprev[L], b = (float)g_ccur[L];
                g_cring[L][idx] = (short)(a + (b - a) * (float)g_cfrac);
            }
        }
        g_cwr++;
        g_cfrac += ratio;
        while (g_cfrac >= 1.0) {
            short nx[CN];
            g_cfrac -= 1.0;
            for (int L = 0; L < CN; L++) g_cprev[L] = g_ccur[L];
            if (crash_src_next(nx)) {
                for (int L = 0; L < CN; L++) g_ccur[L] = nx[L];
            } else {
                for (int L = 0; L < CN; L++) g_ccur[L] = 0;
                g_ceof = 1;
            }
        }
    }
}

/* Open file `n` (1..20) and pre-buffer it, paused -- retail's
 * FUN_001CB6C0 @0x001512A0, which runs while the stream is idle so the
 * bed is already resident when the next crash unpauses it. */
static int crash_load(int n) {
    char path[400];
    int ok = 1;
    crash_close_files();
    g_cplaying = 0;
    for (int L = 0; L < CN; L++) { g_cgain[L] = 0.f; g_cgain_now[L] = 0.f; }
    g_cwr = g_crd = 0;
    g_cfile = 0;
    for (int L = 0; L < CN && ok; L++) {
        FILE *f = NULL;
        int rate = 0, ch = 0;
        long ns;
        crash_layer_path(n, L, path, sizeof(path));
        ns = wav_open_path(path, &f, &rate, &ch);
        if (ns <= 0) { ok = 0; break; }
        g_cfp[L] = f;
        g_cleft[L] = ns;
        g_cch[L] = ch;
        if (L == 0) g_crate = rate;
    }
    if (!ok) { crash_close_files(); return -1; }
    g_cfile = n;
    /* prime the interpolator with the first two source samples */
    {
        short nx[CN];
        if (crash_src_next(nx)) for (int L = 0; L < CN; L++) g_cprev[L] = nx[L];
        if (crash_src_next(nx)) for (int L = 0; L < CN; L++) g_ccur[L]  = nx[L];
    }
    crash_pump();
    return n;
}

/* The rotation itself: retail's `(mgr[0x890] % 20) + 1` then `mgr[0x890]++`
 * @0x00151239.  Load failures fall through the whole rotation once and
 * then give up, so a partial extraction cannot spin. */
static int crash_pick_and_load(void) {
    for (int tries = 0; tries < B3MUSIC_CRASH_FILES; tries++) {
        int n = b3_music_crash_pick(g_crot);
        g_crot++;
        if (crash_load(n) > 0) {
            if (g_clog)
                fprintf(stderr, "[b3_music] crash bed %d buffered and paused "
                                "(rotation counter now %u)\n",
                        n, (unsigned)g_crot);
            return n;
        }
    }
    g_cready = 0;
    return -1;
}

static void crash_release_duck(void) {
    if (g_cducking) { g_cducking = 0; b3_music_set_duck(1.0f); }
}

/* Back to the state FUN_0014B3D0 leaves the audio object in: no file, no
 * rotation, nothing playing. */
static void crash_module_reset(void) {
    crash_close_files();
    g_cplaying = 0;
    g_cfading  = 0;
    g_clayer   = -1;
    g_cfile    = 0;
    g_crot     = 0;
    g_cwr = g_crd = g_cstart_rd = 0;
    for (int L = 0; L < CN; L++) { g_cgain[L] = 0.f; g_cgain_now[L] = 0.f; }
    crash_release_duck();
}

/* Retail's 0.30 song / 0.70 bed is a fixed pair of DirectSound volumes.
 * The harness's song master is a user setting (0.65 as of 2026-08-13), so
 * playing the bed at the recovered 0.70 over it would neither reproduce
 * retail's 2.33:1 balance nor fit in the headroom.  DEVIATION (TUNED):
 * while the bed is up the song is ducked to retail's own 0.30 absolute,
 * which restores both the ratio and the 0.30 + 0.70 = 1.0 sum retail
 * mixed at.  At a 0.30 master the duck is 1.0 and nothing is ducked --
 * exactly what the recovered mix state machine does. */
static float crash_duck_target(void) {
    float m = b3_music_master();
    if (m <= B3MUSIC_SONG_VOL_F) return 1.0f;
    return B3MUSIC_SONG_VOL_F / m;
}

static void crash_stop_internal(int rotate) {
    int was = g_cfile;
    float el = b3_music_crash_elapsed();
    g_cplaying = 0;
    g_cfading = 0;
    g_clayer = -1;
    for (int L = 0; L < CN; L++) g_cgain[L] = 0.f;
    crash_release_duck();
    if (g_clog && was)
        fprintf(stderr, "[b3_music] crash bed %d stopped after %.2f s "
                        "emitted (%u underruns total)\n", was, el, g_cunder);
    if (rotate && g_cready) crash_pick_and_load();
}

int b3_music_crash_arm(void) {
    int n;
    static int probed;
    if (!probed) {
        const char *e = getenv("B3_MUSIC_LOG");
        g_clog = (e && *e && *e != '0');
        probed = 1;
    }
    g_cready = 1;
    n = crash_pick_and_load();
    if (n < 0) {
        char path[400];
        crash_layer_path(1, 0, path, sizeof(path));
        fprintf(stderr, "[b3_music] no crash beds (looked for %s) -- "
                        "run tools/extract_rws.py\n", path);
        return -1;
    }
    /* the extra bump at 0x0014C2D6: the race-init pick advances the u8
     * counter TWICE, so races open on crash1, crash3, crash5, ... */
    g_crot++;
    if (g_clog)
        fprintf(stderr, "[b3_music] crash bed armed: crash%d.rws "
                        "(rotation counter now %u)\n", n, (unsigned)g_crot);
    return n;
}

void b3_music_crash_stop(void) { crash_stop_internal(0); }

void b3_music_crash_tick(int crash_active, int divisor, float dt) {
    float vol = B3MUSIC_CRASH_BASE_F * B3MUSIC_CRASH_VOL_F;   /* 1.0 * 0.7 */
    if (!g_cfile) return;

    if (crash_active) {
        if (!g_cplaying) {                    /* FUN_00150D40 @0x00150E5E */
            g_cstart_rd = g_crd;
            g_cstarts++;
            g_cfading = 0;
            g_cplaying = 1;
            if (g_clog)
                fprintf(stderr, "[b3_music] crash bed %d START (divisor %d, "
                                "layer %s)\n", g_cfile, divisor,
                        divisor == 1 ? "aGen" : "zSlo");
        }
        /* 0x00150F4F: the layer IS the divisor test, nothing else. */
        {
            int layer = (divisor == 1) ? B3MUSIC_CRASH_LAYER_GEN
                                       : B3MUSIC_CRASH_LAYER_SLO;
            if (g_clog && layer != g_clayer && g_clayer >= 0)
                fprintf(stderr, "[b3_music] crash bed %d layer -> %s "
                                "(divisor %d)\n", g_cfile,
                        layer == B3MUSIC_CRASH_LAYER_GEN ? "aGen" : "zSlo",
                        divisor);
            g_clayer = layer;
        }
        g_cfading = 0;
        for (int L = 0; L < CN; L++) g_cgain[L] = (L == g_clayer) ? vol : 0.f;
        g_cducking = 1;
        b3_music_set_duck(crash_duck_target());
        if (g_clog && (g_ctick++ % 30) == 0)
            fprintf(stderr, "[b3_music] crash bed %d  divisor %d  layer %-4s "
                            "gain %.2f  duck %.2f  emitted %.2f s\n",
                    g_cfile, divisor,
                    g_clayer == B3MUSIC_CRASH_LAYER_GEN ? "aGen" : "zSlo",
                    vol, crash_duck_target(), b3_music_crash_elapsed());
        /* the bed outran its 30 s: stop and roll on (GLUE -- retail's
         * stream would simply end; no harness crash gets near 30 s). */
        if (g_ceof && g_crd >= g_cwr) crash_stop_internal(1);
        return;
    }

    if (!g_cplaying) return;

    if (!g_cfading) {                          /* arm, @0x001510C8        */
        g_cfading = 1;
        g_cfade_left  = B3MUSIC_CRASH_FADE_F;
        g_cfade_layer = (g_clayer >= 0) ? g_clayer : B3MUSIC_CRASH_LAYER_GEN;
        if (g_clog)
            fprintf(stderr, "[b3_music] crash bed %d release: %.2f s fade on "
                            "%s\n", g_cfile, B3MUSIC_CRASH_FADE_F,
                    g_cfade_layer == B3MUSIC_CRASH_LAYER_GEN ? "aGen" : "zSlo");
    } else {
        g_cfade_left -= (dt > 0.f) ? dt : 0.f;
        if (g_cfade_left <= 0.f) {             /* @0x00151104, STOP       */
            crash_stop_internal(1);
            return;
        }
    }
    /* @0x00151117: (0x8B4 - clock) * 2.0 * 1.0 * 0.7, on the layer the
     * fade armed with -- retail freezes it in mgr+0x8C0. */
    {
        float g = g_cfade_left * B3MUSIC_CRASH_FADE_K_F * vol;
        if (g > vol) g = vol;
        for (int L = 0; L < CN; L++)
            g_cgain[L] = (L == g_cfade_layer) ? g : 0.f;
    }
    b3_music_set_duck(crash_duck_target());
}

/* One device frame of bed, in the same s16-scaled float domain as the
 * song.  Audio thread; folded into b3_music_next_sample(). */
static float crash_next_sample(void) {
    const float step = 1.f / (B3_CRASH_SLEW * (float)B3MUSIC_RATE);
    unsigned long long rd = g_crd;
    int active = g_cplaying;
    int any = 0;
    float out = 0.f;
    for (int L = 0; L < CN; L++) {
        float t = active ? g_cgain[L] : 0.f;
        float g = g_cgain_now[L];
        if      (g < t) { g += step; if (g > t) g = t; }
        else if (g > t) { g -= step; if (g < t) g = t; }
        g_cgain_now[L] = g;
        if (g > 0.f) any = 1;
    }
    if (!any) return 0.f;
    if (rd >= g_cwr) { if (active) g_cunder++; return 0.f; }
    for (int L = 0; L < CN; L++)
        out += (float)g_cring[L][(unsigned)(rd & CRING_MASK)] * g_cgain_now[L];
    g_crd = rd + 1;
    return out;
}

int b3_music_crash_file(void)    { return g_cfile; }
int b3_music_crash_playing(void) { return g_cplaying ? 1 : 0; }
int b3_music_crash_layer(void)   { return g_cplaying ? g_clayer : -1; }

float b3_music_crash_gain(void) {
    float g = 0.f;
    for (int L = 0; L < CN; L++) if (g_cgain[L] > g) g = g_cgain[L];
    return g_cplaying ? g : 0.f;
}

float b3_music_crash_elapsed(void) {
    unsigned long long rd = g_crd, st = g_cstart_rd;
    if (!g_cplaying || rd <= st) return 0.f;
    return (float)(rd - st) / (float)B3MUSIC_RATE;
}

float b3_music_crash_duck(void) { return crash_duck_target(); }

unsigned b3_music_crash_starts(void)    { return g_cstarts; }
unsigned b3_music_crash_underruns(void) { return g_cunder; }
