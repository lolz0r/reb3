/* dump_score_events.c -- differential driver for tools/validate_score_events.py
 *
 * Pattern: tools/dump_traj.c.  Reads a hex image of the GAME's score object,
 * projects the fields the earn-event detectors own into this port's structs,
 * runs one step of the ported detector, writes the fields back into the image
 * and prints it.  The validator seeds the same image into Unicorn, executes
 * the REAL FUN_00196940/BE0/E10/FUN_00194EE0 over it and diffs the images.
 *
 * Because the exchange format IS the game's own struct layout, a passing diff
 * asserts the memory transition, not just a hand-picked scalar.
 *
 * Build (not in the Makefile by design -- the harness does not need it):
 *   cc -O2 -Isrc -o build/dump_score_events tools/dump_score_events.c \
 *      src/burnout3_score_events.c -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "burnout3_score_events.h"

#define SCORE_SZ 0x600

static unsigned char img[SCORE_SZ];

/* ---- score-object field accessors (offsets: docs/RE_SCORE_EVENTS.md) ---- */
static float  gf(int o) { float v;  memcpy(&v, img + o, 4); return v; }
static int    gi(int o) { int v;    memcpy(&v, img + o, 4); return v; }
static void   sf(int o, float v)   { memcpy(img + o, &v, 4); }
static void   si(int o, int v)     { memcpy(img + o, &v, 4); }

/* the boost record lives at score+0xCC (racecar+0x119C) */
#define REC 0xCC
static void load_bar(B3BoostBar* b)
{
    b->tier       = gi(REC + 0x30);
    b->size       = gf(REC + 0x34);
    b->meter      = gf(REC + 0x38);
    b->earned     = gf(REC + 0x3C);
    b->rate       = gf(REC + 0x40);
    b->min_units  = gf(REC + 0x44);
    b->mult_base  = gf(REC + 0x48);
    b->mult_bonus = gf(REC + 0x4C);
    b->start_time = gf(REC + 0x24);
    b->end_time   = gf(REC + 0x28);
    b->boosting   = img[REC + 0x52];
    b->fixed_burn = img[REC + 0x53];
    b->ramp_done  = img[REC + 0x55];
}
static void store_bar(const B3BoostBar* b)
{
    si(REC + 0x30, b->tier);
    sf(REC + 0x34, b->size);
    sf(REC + 0x38, b->meter);
    sf(REC + 0x3C, b->earned);
    sf(REC + 0x40, b->rate);
    sf(REC + 0x44, b->min_units);
    sf(REC + 0x48, b->mult_base);
    sf(REC + 0x4C, b->mult_bonus);
}

/* a 0x1C-byte category record at `base` */
static void load_rec(B3CatRecord* r, int base, const float* minima)
{
    r->value      = gf(base + 0x00);
    r->clock      = gf(base + 0x04);
    r->prev_value = gf(base + 0x08);
    r->minima     = minima;
    r->tier       = (signed char)img[base + 0x11];
    r->prev_tier  = (signed char)img[base + 0x12];
    r->count      = (signed char)img[base + 0x13];
}
static void store_rec(const B3CatRecord* r, int base)
{
    sf(base + 0x00, r->value);
    sf(base + 0x04, r->clock);
    sf(base + 0x08, r->prev_value);
    img[base + 0x11] = (unsigned char)r->tier;
    img[base + 0x12] = (unsigned char)r->prev_tier;
    img[base + 0x13] = (unsigned char)r->count;
}

/* record bases + their satellite flags/stats */
#define AIR_REC   0x358
#define ONC_REC   0x374
#define DRF_REC   0x390
#define NM_REC    0x418
#define RUB_REC   0x564

static void load_state(B3ScoreEvents* s)
{
    int i;
    memset(s, 0, sizeof(*s));
    load_rec(&s->air,   AIR_REC, b3_score_params.air_minima);
    load_rec(&s->onc,   ONC_REC, b3_score_params.onc_minima);
    load_rec(&s->drift, DRF_REC, b3_score_params.drift_minima);
    load_rec(&s->nm,    NM_REC,  b3_score_params.nm_minima);
    s->air_active   = img[0x368];
    s->onc_active   = img[0x384];
    s->drift_active = img[0x3A0];
    s->nm_active    = img[0x428];
    s->air_scored   = img[0x3C8];
    s->onc_scored   = img[0x3C9];
    s->drift_scored = img[0x3CA];
    s->air_total = gf(0x50);  s->air_best = gf(0x54);  s->air_count = gi(0x354);
    s->onc_total = gf(0x58);  s->onc_best = gf(0x5C);
    s->drift_total = gf(0x60); s->drift_best = gf(0x64);
    s->bp        = gi(0x4C);
    s->bp_event  = gi(0xB8);
    s->nm_total     = gi(0x3CC);
    s->nm_chain     = gi(0x3D0);
    s->prev_clock   = gf(0x3DC);
    s->nm_last      = gf(0x3E0);
    s->nm_chain_end = gf(0x3E4);
    for (i = 0; i < 8; i++) {
        s->nm_id[i]    = (signed char)img[0x3E8 + i];
        s->nm_seen[i]  = gf(0x3F0 + i * 4);
        s->nm_armed[i] = img[0x410 + i];
    }
    s->callout_id = gi(0x254);
    /* --- rubbing / per-opponent contact (score+0x510..0x580) --- */
    s->race_finished = (gi(0x27C) == 3);
    for (i = 0; i < B3_SE_RUB_CARS; i++) {
        s->rub_last[i]  = gf(0x510 + i * 4);
        s->rub_time[i]  = gf(0x528 + i * 4);
        s->rub_start[i] = gf(0x540 + i * 4);
        s->rub_prev_touch[i] = img[0x558 + i];
        s->rub_touch[i]      = img[0x55E + i];
    }
    s->rub.value      = gf(RUB_REC + 0x00);
    s->rub.clock      = gf(RUB_REC + 0x04);
    s->rub.prev_value = gf(RUB_REC + 0x08);
    s->rub.minima     = b3_score_params.rub_minima;
    s->rub.tier       = (signed char)img[0x575];
    s->rub.prev_tier  = (signed char)img[0x576];
    s->rub.count      = (signed char)img[0x577];
    s->rub_active     = img[0x574];
    s->rub_target     = gi(0x580);
}

static void store_state(const B3ScoreEvents* s)
{
    int i;
    store_rec(&s->air,   AIR_REC);
    store_rec(&s->onc,   ONC_REC);
    store_rec(&s->drift, DRF_REC);
    store_rec(&s->nm,    NM_REC);
    img[0x368] = s->air_active;
    img[0x384] = s->onc_active;
    img[0x3A0] = s->drift_active;
    img[0x428] = s->nm_active;
    img[0x3C8] = s->air_scored;
    img[0x3C9] = s->onc_scored;
    img[0x3CA] = s->drift_scored;
    sf(0x50, s->air_total);  sf(0x54, s->air_best);  si(0x354, s->air_count);
    sf(0x58, s->onc_total);  sf(0x5C, s->onc_best);
    sf(0x60, s->drift_total); sf(0x64, s->drift_best);
    si(0x4C, s->bp);
    si(0xB8, s->bp_event);
    si(0x3CC, s->nm_total);
    si(0x3D0, s->nm_chain);
    sf(0x3DC, s->prev_clock);
    sf(0x3E0, s->nm_last);
    sf(0x3E4, s->nm_chain_end);
    for (i = 0; i < 8; i++) {
        img[0x3E8 + i] = (unsigned char)s->nm_id[i];
        sf(0x3F0 + i * 4, s->nm_seen[i]);
        img[0x410 + i] = s->nm_armed[i];
    }
    si(0x254, s->callout_id);
    si(0x260, s->callout_tier);
    for (i = 0; i < B3_SE_RUB_CARS; i++) {
        sf(0x510 + i * 4, s->rub_last[i]);
        sf(0x528 + i * 4, s->rub_time[i]);
        sf(0x540 + i * 4, s->rub_start[i]);
        img[0x558 + i] = s->rub_prev_touch[i];
        img[0x55E + i] = s->rub_touch[i];
    }
    sf(RUB_REC + 0x00, s->rub.value);
    sf(RUB_REC + 0x04, s->rub.clock);
    sf(RUB_REC + 0x08, s->rub.prev_value);
    img[0x575] = (unsigned char)s->rub.tier;
    img[0x576] = (unsigned char)s->rub.prev_tier;
    img[0x577] = (unsigned char)s->rub.count;
    img[0x574] = s->rub_active;
    si(0x580, s->rub_target);
}

/* ---- hex io ---- */
static void read_img(void)
{
    char* line = NULL;
    size_t cap = 0;
    int i;
    if (getline(&line, &cap, stdin) < 0) exit(2);
    for (i = 0; i < SCORE_SZ; i++) {
        unsigned v;
        sscanf(line + i * 2, "%2x", &v);
        img[i] = (unsigned char)v;
    }
    free(line);
}
static void write_img(void)
{
    int i;
    for (i = 0; i < SCORE_SZ; i++) printf("%02x", img[i]);
    printf("\n");
}
static double rdnum(void)
{
    double v = 0.0;
    if (scanf("%lf", &v) != 1) exit(3);
    return v;
}
static void read_obb(B3ScoreObb* o)
{
    int r, c;
    for (r = 0; r < 4; r++) for (c = 0; c < 4; c++) o->m[r][c] = (float)rdnum();
    for (c = 0; c < 4; c++) o->bmax[c] = (float)rdnum();
    for (c = 0; c < 4; c++) o->bmin[c] = (float)rdnum();
}

int main(int argc, char** argv)
{
    const char* cmd = argc > 1 ? argv[1] : "";
    const char* tune = argc > 2 ? argv[2] : "default";

    if (!strcmp(tune, "vdb")) b3_score_params_vdb(&b3_score_params);
    else                      b3_score_params_defaults(&b3_score_params);

    if (!strcmp(cmd, "obb")) {
        B3ScoreObb a, b;
        float d;
        read_obb(&a);
        read_obb(&b);
        d = (float)rdnum();
        printf("%d\n", b3_score_obb_near(&a, &b, d));
        return 0;
    }

    if (!strcmp(cmd, "cat")) {
        /* argv: cat <tune> <airflag> <oncflag> <driftflag> <mph> <step> <clk> */
        B3ScoreEvents s;
        B3BoostBar bar;
        B3ScoreFrame f;
        read_img();
        load_state(&s);
        load_bar(&bar);
        f.airborne  = atoi(argv[3]);
        f.oncoming  = atoi(argv[4]);
        f.drifting  = atoi(argv[5]);
        f.speed_mph = (float)atof(argv[6]);
        f.dist_step = (float)atof(argv[7]);
        f.clock     = (float)atof(argv[8]);
        b3_score_events_frame(&s, &bar, &f);
        store_state(&s);
        store_bar(&bar);
        write_img();
        return 0;
    }

    if (!strcmp(cmd, "nm")) {
        /* argv: nm <tune> <mph> <clock> <n_others>
         * stdin: image line, then me OBB, then n_others * (OBB + id) */
        B3ScoreEvents s;
        B3BoostBar bar;
        B3ScoreObb me;
        B3ScoreObb others[16];
        int ids[16];
        int n = atoi(argv[5]), i;
        float mph = (float)atof(argv[3]);
        float clk = (float)atof(argv[4]);
        read_img();
        load_state(&s);
        load_bar(&bar);
        read_obb(&me);
        for (i = 0; i < n; i++) {
            read_obb(&others[i]);
            ids[i] = (int)rdnum();
        }
        b3_score_events_near_miss(&s, &bar, &me, mph, clk, others, ids, n);
        store_state(&s);
        store_bar(&bar);
        write_img();
        return 0;
    }

    if (!strcmp(cmd, "gate")) {
        /* argv: gate <tune> <crashed> <respawning> <clock>
         * FUN_001935F0's crash gate alone (0x001939AD..0x00193CE4).  Prints
         * the branch verdict first, then the post image. */
        B3ScoreEvents s;
        B3BoostBar bar;
        int crashed = atoi(argv[3]), respawn = atoi(argv[4]);
        float clk = (float)atof(argv[5]);
        read_img();
        load_state(&s);
        load_bar(&bar);
        b3_score_events_set_crash(&s, crashed, respawn);
        printf("suspended=%d\n", b3_score_events_suspended(&s));
        if (b3_score_events_suspended(&s))
            b3_score_events_crash_reset(&s, clk);
        store_state(&s);
        store_bar(&bar);
        write_img();
        return 0;
    }

    if (!strcmp(cmd, "frame")) {
        /* argv: frame <tune> <air> <onc> <drift> <mph> <step> <clk>
         *             <crashed> <respawn> <n_others>
         * stdin: image line, then me OBB, then n_others * (OBB + id)
         *
         * One whole FUN_001935F0 body in the game's own order: the crash gate,
         * then FUN_00194EE0 (0x00193A09), then FUN_00196940/BE0/E10
         * (0x00193A32/3D/48).  On a suspended frame only the reset runs. */
        B3ScoreEvents s;
        B3BoostBar bar;
        B3ScoreFrame f;
        B3ScoreObb me;
        B3ScoreObb others[16];
        int ids[16];
        int n = atoi(argv[11]), i;
        read_img();
        load_state(&s);
        load_bar(&bar);
        read_obb(&me);
        for (i = 0; i < n; i++) {
            read_obb(&others[i]);
            ids[i] = (int)rdnum();
        }
        f.airborne   = atoi(argv[3]);
        f.oncoming   = atoi(argv[4]);
        f.drifting   = atoi(argv[5]);
        f.speed_mph  = (float)atof(argv[6]);
        f.dist_step  = (float)atof(argv[7]);
        f.clock      = (float)atof(argv[8]);
        /* Both entry points are called UNCONDITIONALLY on purpose: the crash
         * gate belongs to the module, and the differential must fail if it
         * is not there. */
        b3_score_events_set_crash(&s, atoi(argv[9]), atoi(argv[10]));
        b3_score_events_near_miss(&s, &bar, &me, f.speed_mph, f.clock,
                                  others, ids, n);
        b3_score_events_frame(&s, &bar, &f);
        store_state(&s);
        store_bar(&bar);
        write_img();
        return 0;
    }

    if (!strcmp(cmd, "contact")) {
        /* argv: contact <tune> <id> <clock> <crashed>
         * FUN_00197920 -- the near-miss cancel. */
        B3ScoreEvents s;
        read_img();
        load_state(&s);
        b3_score_events_set_crash(&s, atoi(argv[5]), 0);
        b3_score_events_contact(&s, atoi(argv[3]), (float)atof(argv[4]));
        store_state(&s);
        write_img();
        return 0;
    }

    if (!strcmp(cmd, "mark")) {
        /* argv: mark <tune> <idx> <clock> <crashed>
         * FUN_001979E0 -- the rubbing contact mark. */
        B3ScoreEvents s;
        read_img();
        load_state(&s);
        b3_score_events_set_crash(&s, atoi(argv[5]), 0);
        b3_score_events_mark_contact(&s, atoi(argv[3]), (float)atof(argv[4]));
        store_state(&s);
        write_img();
        return 0;
    }

    if (!strcmp(cmd, "rub")) {
        /* argv: rub <tune> <clock> <dt> <n> <slam0..slam(n-1)>
         * FUN_00194A80 -- one rubbing frame. */
        B3ScoreEvents s;
        B3BoostBar bar;
        unsigned char slam[B3_SE_RUB_CARS];
        int n = atoi(argv[5]), i;
        read_img();
        load_state(&s);
        load_bar(&bar);
        for (i = 0; i < B3_SE_RUB_CARS; i++)
            slam[i] = (unsigned char)((6 + i < argc) ? atoi(argv[6 + i]) : 0);
        b3_score_events_rubbing(&s, &bar, (float)atof(argv[3]),
                                (float)atof(argv[4]), slam, n);
        store_state(&s);
        store_bar(&bar);
        write_img();
        return 0;
    }

    if (!strcmp(cmd, "frameend")) {
        B3ScoreEvents s;
        read_img();
        load_state(&s);
        b3_score_events_frame_end(&s);
        store_state(&s);
        write_img();
        return 0;
    }

    fprintf(stderr,
            "usage: dump_score_events "
            "{cat|nm|obb|gate|frame|contact|mark|rub|frameend} <tune> ...\n");
    return 1;
}
