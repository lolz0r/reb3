/* Takedown presentation FX -- see burnout3_takedown.h and
 * docs/RE_TAKEDOWN_FX.md.
 *
 * The four cinematic functions and the two timer functions below are 1:1
 * ports of real code, each with a green differential case in
 * tools/validate_takedown.py that runs the actual x86 under Unicorn
 * (tools/emulate_tdfx.py) and asserts this C reproduces its post-state.
 *
 * Anything marked GLUE is this harness's own, not recovered.
 */
#include "burnout3_takedown.h"

#include <math.h>
#include <string.h>

/* ======================================================================
 * 1. the frame timer  --  FUN_001B5AC0 / FUN_001B5B60   [C]
 * ====================================================================== */

void b3_tdfx_timer_init(B3TdfxTimer *t, float period)
{
    /* GLUE: the game's timer is constructed elsewhere; only the field
     * values matter and they are what the tick/rescale code expects. */
    memset(t, 0, sizeof(*t));
    t->period    = period;
    t->divisor   = 1;
    t->requested = 1;
    t->running   = 1;
    t->dt        = period;
}

/* FUN_001B5B60 (ECX = timer).  Applies +0x24 to +0x18 and recomputes the
 * per-frame dt.  The remainder rebase is the game's, including the
 * round-to-nearest on the way back to divisor 1. */
void b3_tdfx_timer_rescale(B3TdfxTimer *t)
{
    int req = t->requested;
    if (req == 1) {
        if (t->divisor / 2 < t->rem)
            t->whole += 1;
        t->rem = 0;
    } else {
        t->rem = (t->rem * req - 1 + t->divisor) / t->divisor;
    }
    t->divisor = req;
    t->dt = t->period / (float)req;
}

/* FUN_001B5AC0 (ECX = timer, DAT_004A1EB4 = the raw frame counter). */
void b3_tdfx_timer_tick(B3TdfxTimer *t, int frame_counter)
{
    if (t->last_raw != frame_counter) {
        int since_base, delta;
        if (t->divisor != t->requested)
            b3_tdfx_timer_rescale(t);
        since_base = frame_counter - t->base;
        t->last_raw = frame_counter;
        delta = since_base - t->prev_delta;
        if (delta > 0) {
            if (t->running == 0) {
                t->paused_acc += delta;
            } else if (t->divisor == 1) {
                t->whole += delta;
            } else {
                int d = t->divisor;
                int r = t->rem + delta % d;
                t->rem = r;
                if (d < r) {
                    t->whole += r / d;
                    t->rem = r % d;
                }
                t->whole += delta / d;
            }
            t->prev_delta = since_base;
        }
    }
    t->clock = (t->period / (float)t->divisor) * (float)t->rem
             + (float)t->whole * t->period;
}

/* ======================================================================
 * 2. the takedown cinematic  --  FUN_00027A60 / 27920 / 27AD0 / 279C0  [C]
 * ====================================================================== */

static void post(B3TdfxPostSink *s, int hud_event, int hud_arg, int state)
{
    if (!s || s->count >= s->cap)
        return;
    s->posts[s->count].hud_event = hud_event;
    s->posts[s->count].hud_arg   = hud_arg;
    s->posts[s->count].state     = state;
    s->count++;
}

/* FUN_00027A60 (EDX = record).  `me` is racecar[record->slot]. */
int b3_td_cam_gate(const B3TdCamRecord *r, const B3TdRacecar *me)
{
    const B3TdRacecar *victim = me->victim;
    float age;
    if (victim == 0)                       return 0;   /* 0x00027A74 */
    if (!victim->crashed)                  return 0;   /* 0x00027A79 */
    if (me->crashed)                       return 0;   /* 0x00027A83 */
    if (r->active)                         return 0;   /* 0x00027A8D */
    if ((const void *)victim == r->victim) return 0;   /* 0x00027A94 */
    age = victim->clock - victim->crash_stamp;         /* 0x00027A98 */
    if (age < 0.0f)                        return 0;   /* 0x003B16E0 */
    if (B3_TDFX_TRIGGER_AGE < age)         return 0;   /* 0x003B1870 */
    return 1;
}

/* FUN_00027920 (ESI = record). */
void b3_td_cam_enter(B3TdCamRecord *r, B3TdRacecar *me,
                     B3TdfxTimer *timer, B3TdfxPostSink *sink)
{
    if (r->active)
        return;                                        /* 0x00027932 */
    r->victim       = me->victim;                      /* +0x15A4    */
    r->active       = 1;
    r->flag_c       = 1;
    r->elapsed      = 0.0f;
    r->callout_done = 0;
    r->chain        = me->chain_flag;                  /* +0x11EE    */
    timer->requested = B3_TDFX_DIV_AFTERTOUCH;         /* 0x00027959 */
    {
        int slot = me->victim ? me->victim->grid_slot : -1;
        int id = (me->signature == -1) ? B3_TDFX_EVT_TAKEDOWN_START
                                       : B3_TDFX_EVT_TAKEDOWN_SIGNATURE;
        post(sink, id, slot, -1);                      /* vtable +0x0C */
        post(sink, -1, 0, 3);                          /* FUN_00053D20 */
    }
}

/* FUN_000279C0 (EDX = record). */
void b3_td_cam_exit(B3TdCamRecord *r, B3TdRacecar *me, B3TdfxTimer *timer)
{
    me->hud_flag = 0;                                  /* +0x245D */
    if (me->cam_active) {
        me->cam_active = 0;                            /* +0x27D8 */
        if (me->pv) {
            me->pv->authority = 1.0f;                  /* +0x1534 */
            if (me->pv->drift_state == 4)
                me->pv->drift_state = 0;               /* +0x1524 */
        }
    }
    /* the second clear goes through the grid-slot -> vehicle table
     * DAT_0064B38C; in this port both resolve to the same vehicle */
    if (me->pv && me->pv->drift_state == 4)
        me->pv->drift_state = 0;
    timer->requested = B3_TDFX_DIV_NORMAL;             /* 0x000279FD */
    r->flag_c  = 0;
    r->active  = 0;
    r->victim  = 0;
    r->elapsed = -1.0f;
}

/* FUN_0018CB60 (EAX = racecar, CL = wanted).  Edge-triggered. */
static void cam_set(B3TdRacecar *me, int wanted)
{
    if ((unsigned char)wanted == me->cam_active)
        return;
    me->cam_active = (unsigned char)wanted;
    if (wanted) {
        /* the real code zeroes racecar+0x19D0..+0x19DC and calls the camera
         * reset FUN_00179760; this port has no camera object to reset. */
        return;
    }
    if (me->pv) {
        me->pv->authority = 1.0f;
        if (me->pv->drift_state == 4)
            me->pv->drift_state = 0;
    }
}

/* FUN_00027AD0 (ESI = record).  real_dt is DAT_004AE1FC. */
void b3_td_cam_update(B3TdCamRecord *r, B3TdRacecar *me,
                      B3TdfxTimer *timer, B3TdfxPostSink *sink,
                      float real_dt)
{
    float t;
    int in_slowmo, before_end;
    int victim_slot;

    if (!r->active)
        return;                                        /* 0x00027AD0 */

    t = real_dt + r->elapsed;                          /* 0x00027ADA */
    r->elapsed = t;

    if (me->race_state == 3 || me->crashed) {          /* 0x00027AFC/0x27B09 */
        b3_td_cam_exit(r, me, timer);
        return;
    }

    victim_slot = r->victim ? ((B3TdRacecar *)r->victim)->grid_slot : -1;

    if (!(B3_TDFX_CINE_TOTAL > t)) {                   /* 0x00027B38 */
        b3_td_cam_exit(r, me, timer);
        return;
    }
    before_end = 1;                                    /* always 1 here */
    in_slowmo  = (B3_TDFX_CINE_SLOWMO > t);            /* 0x00027B59 */

    /* 0x00027B83: leaving the slow-mo phase releases the victim's
     * cinematic drift state through the grid-slot -> vehicle table */
    if (me->cam_active && !in_slowmo && me->pv && me->pv->drift_state == 4)
        me->pv->drift_state = 0;

    cam_set(me, in_slowmo);                            /* 0x00027BB9 */

    timer->requested = in_slowmo ? B3_TDFX_DIV_AFTERTOUCH
                                 : B3_TDFX_DIV_NORMAL; /* 0x00027BCD */

    if (!r->callout_done && t >= B3_TDFX_CINE_CALLOUT
        && victim_slot != -1) {                        /* 0x00027BD2 */
        post(sink, B3_TDFX_EVT_TAKEDOWN_MID, victim_slot, -1);
        post(sink, -1, 0, 1);                          /* FUN_00053D20(1) */
        r->callout_done = 1;
    }
    if (r->flag_c && t >= B3_TDFX_CINE_CALLOUT && victim_slot != -1)
        r->flag_c = 0;                                 /* 0x00027C29 */

    if (in_slowmo) {                                   /* 0x00027C48 */
        if (r->chain && !me->boost_ramp_done)
            me->burnout_grant = 1;                     /* +0x2417 */
        me->hud_flag = (unsigned char)before_end;      /* +0x245D */
        return;
    }
    if (!before_end)
        return;
    if (me->pv)
        me->pv->flags1353 |= 0x18;                     /* 0x00027C9D */
}

/* ======================================================================
 * 3. the callout  --  FUN_00199350 + the message/descriptor tables
 * ====================================================================== */

/* The takedown slice of the 206-entry message table at 0x00389160.
 * Record = { u8 message id, u8 sign kind, u16 Globalus.bin string index }.
 * Every row here is asserted against the image byte-for-byte by
 * tools/validate_takedown.py section 9, and every string index against the
 * retail Data/Globalus.bin. */
typedef struct { unsigned char id, kind; unsigned short str; } TdfxMsg;

static const TdfxMsg TDFX_MSGS[] = {
    { 0x35,  1, 2100 },   /* "TAKEDOWN DENIED!"      */
    { 0x36,  1, 2101 },   /* "LUCKY ESCAPE!"         */
    { 0x7F,  3, 2103 },   /* "CRASHED!"              */
    { 0x80,  3, 2104 },   /* "TAKEN OUT!"            */
    { 0x81,  3, 2105 },   /* "TAKEDOWN AVENGED!"     */
    { 0x93,  4, 2074 },   /* "TAKEDOWN!"             */
    { 0x94,  4, 2079 },   /* "CAR TAKEDOWN!"         */
    { 0x95,  4, 2080 },   /* "VAN TAKEDOWN!"         */
    { 0x96,  4, 2081 },   /* "TRUCK TAKEDOWN!"       */
    { 0x97,  4, 2082 },   /* "BIG RIG TAKEDOWN!"     */
    { 0x98,  4, 2083 },   /* "BUS TAKEDOWN!"         */
    { 0x99,  4, 2084 },   /* "L-TRAIN TAKEDOWN!"     */
    { 0x9A,  4, 2086 },   /* "TRAM TAKEDOWN!"        */
    { 0x9B,  4, 2085 },   /* "MONORAIL TAKEDOWN!"    */
    { 0x9C,  4, 2087 },   /* "TRAILER TAKEDOWN!"     */
    { 0x9D,  4, 2089 },   /* "WALL TAKEDOWN!"        */
    { 0x9E,  4, 2088 },   /* "BONUS TAKEDOWN!"       */
    { 0x9F,  4, 2096 },   /* "TAKEDOWN 2-IN-A-ROW"   */
    { 0xA0,  4, 2097 },   /* "TAKEDOWN 3-IN-A-ROW"   */
    { 0xA1,  4, 2098 },   /* "TAKEDOWN HOT STREAK"   */
    { 0xA2,  4, 2099 },   /* "TAKEDOWN RAMPAGE!"     */
    { 0xA3,  4, 2075 },   /* "DOUBLE TAKEDOWN!"      */
    { 0xA4,  4, 2076 },   /* "TRIPLE TAKEDOWN!"      */
    { 0xA5,  4, 2077 },   /* "4-WAY TAKEDOWN!"       */
    { 0xA6,  4, 2078 },   /* "TOTAL TAKEDOWN!"       */
    { 0xA7,  4, 2094 },   /* "REVENGE!"              */
    { 0xA8,  4, 2095 },   /* "GRUDGE!"               */
    { 0xA9,  4, 2093 },   /* "PSYCHE OUT!"           */
    { 0xAA,  4, 2102 },   /* "AFTERTOUCH TAKEDOWN!"  */
    { 0xAB,  4, 2075 },   /* "DOUBLE TAKEDOWN!"      */
    { 0xAC,  4, 2076 },   /* "TRIPLE TAKEDOWN!"      */
    { 0xAD,  4, 2077 },   /* "4-WAY TAKEDOWN!"       */
    { 0xAE,  4, 2078 },   /* "TOTAL TAKEDOWN!"       */
    { 0xAF,  4, 2090 },   /* "SIGNATURE TAKEDOWN" x20 (0xAF..0xC2) */
    { 0xB0,  4, 2090 }, { 0xB1,  4, 2090 }, { 0xB2,  4, 2090 },
    { 0xB3,  4, 2090 }, { 0xB4,  4, 2090 }, { 0xB5,  4, 2090 },
    { 0xB6,  4, 2090 }, { 0xB7,  4, 2090 }, { 0xB8,  4, 2090 },
    { 0xB9,  4, 2090 }, { 0xBA,  4, 2090 }, { 0xBB,  4, 2090 },
    { 0xBC,  4, 2090 }, { 0xBD,  4, 2090 }, { 0xBE,  4, 2090 },
    { 0xBF,  4, 2090 }, { 0xC0,  4, 2090 }, { 0xC1,  4, 2090 },
    { 0xC2,  4, 2090 },
    { 0xC3,  4, 2115 },   /* "BOOST LOCKED!"         */
    { 0xC4,  4, 2189 },   /* "RACEBREAKER AVAILABLE!"*/
};
#define TDFX_NMSGS ((int)(sizeof(TDFX_MSGS) / sizeof(TDFX_MSGS[0])))

/* the 17 descriptors at 0x003895E8: {text param, anim style, sign index,
 * flags}; sign index 17 (0x11) means "no sign" */
typedef struct { int text, anim, sign, flags; } TdfxDesc;
static const TdfxDesc TDFX_DESCS[17] = {
    {1,1, 0,0x00}, {4,2, 1,0x02}, {1,1, 2,0x06}, {1,1, 4,0x00},
    {2,1, 5,0x00}, {3,1, 6,0x00}, {1,1, 3,0x0D}, {4,2, 1,0x0D},
    {1,1, 0,0x00}, {1,1, 8,0x00}, {1,1, 9,0x00}, {1,1,10,0x00},
    {1,1,11,0x00}, {1,1,12,0x00}, {1,1,13,0x00}, {4,1,14,0x00},
    {5,1,15,0x00},
};

/* FUN_00054700 @ 0x0005494C: `kind - 3` indexes the jump table at
 * 0x00054ACC; anything outside 3..23 lands on descriptor 0. */
static int kind_to_descriptor(int kind)
{
    static const signed char MAP[21] = {
        /*  3 */ 7, /*  4 */ 6, /*  5 */ 0, /*  6 */ 0, /*  7 */ 0,
        /*  8 */ 0, /*  9 */ 1, /* 10 */ 5, /* 11 */ 4, /* 12 */ 3,
        /* 13 */ 2, /* 14 */ 0, /* 15 */ 9, /* 16 */10, /* 17 */11,
        /* 18 */12, /* 19 */13, /* 20 */ 8, /* 21 */14, /* 22 */15,
        /* 23 */16,
    };
    int i = kind - 3;
    if (i < 0 || i > 20)
        return 0;
    return MAP[i];
}

int b3_tdfx_message_info(int msg_id, int *string_index, int *sign_kind,
                         int *descriptor, int *sign_index, int *anim_style,
                         int *flags)
{
    int i;
    for (i = 0; i < TDFX_NMSGS; i++) {
        if (TDFX_MSGS[i].id == (unsigned char)msg_id) {
            int d = kind_to_descriptor(TDFX_MSGS[i].kind);
            if (string_index) *string_index = TDFX_MSGS[i].str;
            if (sign_kind)    *sign_kind    = TDFX_MSGS[i].kind;
            if (descriptor)   *descriptor   = d;
            if (sign_index)   *sign_index   = TDFX_DESCS[d].sign;
            if (anim_style)   *anim_style   = TDFX_DESCS[d].anim;
            if (flags)        *flags        = TDFX_DESCS[d].flags;
            return 1;
        }
    }
    return 0;
}

/* FUN_001994D0's selection, in its recovered precedence order [S]:
 * signature > aftertouch(+multi) > multi > streak > revenge > wall > plain.
 * (The award function tests these in this order and the last write to its
 * message slot wins; there is no green whole-function case for it.) */
int b3_tdfx_select_takedown_message(int revenge, int aftertouch,
                                    int multi_count, int streak_count,
                                    int wall, int signature_index)
{
    if (signature_index >= 0 && signature_index < 20)
        return B3_TDFX_MSG_SIGNATURE_0 + signature_index;
    if (aftertouch) {
        int n = multi_count;
        if (n < 0) n = 0;
        if (n > 4) n = 4;
        return B3_TDFX_MSG_AFTERTOUCH_0 + n;
    }
    if (multi_count > 0)
        return B3_TDFX_MSG_MULTI_0 + (multi_count > 3 ? 3 : multi_count - 1);
    if (streak_count > 0)
        return B3_TDFX_MSG_STREAK_0 + (streak_count > 4 ? 3 : streak_count - 1);
    if (revenge)
        return B3_TDFX_MSG_REVENGE;
    if (wall)
        return B3_TDFX_MSG_WALL_TAKEDOWN;
    return B3_TDFX_MSG_TAKEDOWN;
}

/* ======================================================================
 * 4. harness-facing singleton
 * ====================================================================== */

#define TDFX_POSTS 16

static struct {
    B3TdfxTimer     timer;
    B3TdCamRecord   rec;
    B3TdRacecar     attacker;
    B3TdRacecar     victim;
    B3TdVehicle     att_pv;
    int             frame_counter;

    B3TdfxPost      post_ring[TDFX_POSTS];
    int             post_head, post_tail;
    B3TdfxPost      scratch[TDFX_POSTS];
    B3TdfxPostSink  sink;

    int             attacker_slot, victim_slot;
    int             last_event;
    B3TdfxCallout   callout;

    /* the attacker's control edges (see B3TdfxStatus) */
    int             wheel_was;      /* racecar+0x27D8 at the top of the frame */
    int             wheel_release;  /* its 1 -> 0 edge, one frame             */
    int             hud_flag_seen;  /* racecar+0x245D before it was consumed  */

    /* aftertouch ("Impact Time") request, FUN_00118410 @ 0x001188A4 */
    int             aftertouch_on;
    int             aftertouch_engaged;  /* vehicle +0x4AC7 [C-disasm]     */

    /* the player-crash window, RE_TAKEDOWN_FX section 9 */
    int             crash_credit;      /* racecar+0x16C8, 1 at event reset */
    int             crash_req_pending; /* FUN_00025CC0 fires once          */
    int             crash_end_pending; /* FUN_00119C00 / FUN_000269D0      */
    int             crash_slowmo_on;   /* the divisor-5 request is live    */
    float           crash_slowmo_start;/* dilated clock at the request     */

    /* impact-hit slam, FUN_00026050 @ 0x0002655B */
    int             impact_active;
    float           impact_start;
    /* the ARM's gate: the collision record's +0x174 (FUN_00026A70
     * @0x00026B18).  Zero = not a big hit = the window must not arm. */
    int             big_hit_flags;

    float           real_clock;
    /* the DILATED per-frame dt the last b3_tdfx_update produced.  This is
     * what the camera runs on -- see the note above b3_tdfx_crash_camera_x. */
    float           sim_dt;

    /* camera state */
    float           cam_eye[3];
    int             cam_primed;
    float           cam_pitch;       /* camera obj +0x1C, degrees        */
    float           cam_yaw;         /* camera obj +0x18, degrees        */
    int             crashcam_primed;
    B3CamFollow     crashcam;        /* the real mode-2 state [C]        */
} G;

void b3_tdfx_init(void)
{
    memset(&G, 0, sizeof(G));
    b3_tdfx_timer_init(&G.timer, B3_TDFX_PERIOD_NTSC);
    G.rec.elapsed = -1.0f;
    G.attacker.pv = &G.att_pv;
    G.attacker.signature = -1;
    G.att_pv.authority = 1.0f;
    G.sink.posts = G.scratch;
    G.sink.cap   = TDFX_POSTS;
    G.last_event = -1;
    G.victim_slot = -1;
    G.attacker_slot = -1;
    G.callout.msg_id = -1;
    G.callout.phase  = 3;
    b3_cam_follow_init(&G.crashcam);
    G.crash_credit = 1;          /* FUN_00025AB0 @0x00025AE5 */
}

/* FUN_00199350 "PostHudCallout" (ESI = callout slot, EDI = message id).
 * The message id doubles as the PRIORITY: a lower id cannot replace a
 * higher one that is still up (0x0019937F).  0x7F/0x80/0x81 require the
 * racecar's crashed flag (0x001993EF); every other id is refused while the
 * racecar is crashed unless the caller's flag byte is set (0x001993DA). */
int b3_tdfx_post_callout(int msg_id, int racecar_crashed, int flag_byte)
{
    int str = 0, kind = 0, desc = 0, sign = 0, anim = 1, flags = 0;

    if (!b3_tdfx_message_info(msg_id, &str, &kind, &desc, &sign, &anim,
                              &flags))
        return 0;
    if (G.callout.msg_id >= 0 && G.callout.phase < 3
        && msg_id < G.callout.msg_id)
        return 0;                                   /* 0x0019937F */
    if (msg_id == B3_TDFX_MSG_CRASHED || msg_id == B3_TDFX_MSG_TAKEN_OUT
        || msg_id == B3_TDFX_MSG_AVENGED) {
        if (!racecar_crashed) return 0;             /* 0x001993EF */
    } else if (racecar_crashed && !flag_byte) {
        return 0;                                   /* 0x001993DA */
    }

    G.callout.msg_id       = msg_id;
    G.callout.string_index = str;
    G.callout.sign_kind    = kind;
    G.callout.descriptor   = desc;
    G.callout.sign_index   = sign;
    G.callout.anim_style   = anim;
    G.callout.flags        = flags;
    G.callout.elapsed      = 0.0f;
    G.callout.phase        = 0;
    G.callout.phase_u      = 0.0f;
    G.callout.visible      = 1;
    /* FUN_00055C90's phase schedule: element +0x154 = IN + hold */
    if (anim == 2) {
        G.callout.t_in       = B3_TDFX_ANIM2_IN;
        G.callout.t_hold_end = B3_TDFX_ANIM2_IN
                             + ((flags & 8) ? B3_TDFX_HOLD2_LONG
                                            : B3_TDFX_HOLD2_SHORT);
        G.callout.t_total    = G.callout.t_hold_end + B3_TDFX_ANIM2_OUT;
    } else {
        G.callout.t_in       = B3_TDFX_ANIM1_IN;
        G.callout.t_hold_end = B3_TDFX_ANIM1_IN
                             + ((flags & 8) ? B3_TDFX_HOLD1_LONG
                                            : B3_TDFX_HOLD1_SHORT);
        G.callout.t_total    = G.callout.t_hold_end + B3_TDFX_ANIM1_OUT;
    }
    return 1;
}

/* FUN_00056120's phase walk. The dt the HUD element's +0x150 accumulator
 * is fed with was not identified [?]; this port uses the REAL frame delta,
 * matching the cinematic's own timer. */
static void callout_step(float real_dt)
{
    B3TdfxCallout *c = &G.callout;
    if (c->msg_id < 0 || c->phase >= 3)
        return;
    c->elapsed += real_dt;
    if (c->elapsed < c->t_in) {
        c->phase = 0;
        c->phase_u = c->t_in > 0.0f ? c->elapsed / c->t_in : 1.0f;
    } else if (c->elapsed < c->t_hold_end) {
        c->phase = 1;
        c->phase_u = (c->elapsed - c->t_in)
                   / (c->t_hold_end - c->t_in > 0.0f
                      ? c->t_hold_end - c->t_in : 1.0f);
    } else if (c->elapsed < c->t_total) {
        c->phase = 2;
        c->phase_u = (c->elapsed - c->t_hold_end)
                   / (c->t_total - c->t_hold_end);
    } else {
        c->phase = 3;
        c->phase_u = 1.0f;
        c->visible = 0;
        c->msg_id = -1;
        c->sign_index = -1;
        c->string_index = -1;
    }
}

const B3TdfxCallout *b3_tdfx_callout(void) { return &G.callout; }

static void drain_sink(void)
{
    int i;
    for (i = 0; i < G.sink.count; i++) {
        int nxt = (G.post_head + 1) % TDFX_POSTS;
        if (nxt == G.post_tail)
            break;                       /* ring full: drop, never block */
        G.post_ring[G.post_head] = G.scratch[i];
        G.post_head = nxt;
        if (G.scratch[i].hud_event >= 0)
            G.last_event = G.scratch[i].hud_event;
    }
    G.sink.count = 0;
}

int b3_tdfx_poll_post(B3TdfxPost *out)
{
    if (G.post_tail == G.post_head)
        return 0;
    if (out)
        *out = G.post_ring[G.post_tail];
    G.post_tail = (G.post_tail + 1) % TDFX_POSTS;
    return 1;
}

int b3_tdfx_on_takedown(int attacker_slot, int attacker_crashed,
                        int victim_slot, int victim_crashed,
                        float victim_clock, float victim_crash_time,
                        int signature_id, int burnout_chain_flag)
{
    /* The callout and the cinematic are two independent consumers of the
     * same commit: FUN_001994D0 posts the message while FUN_000273F0 ->
     * FUN_000278B0 arms the camera.  Post first so the sign is up even
     * when the cinematic's own gate refuses. */
    b3_tdfx_post_callout(B3_TDFX_MSG_TAKEDOWN, attacker_crashed != 0, 0);

    /* FUN_000278B0's gate chain, minus the game-mode virtual at
     * vtable+0x104 which this harness has no equivalent for (GLUE:
     * treated as "allowed"). */
    G.attacker.crashed     = (unsigned char)(attacker_crashed != 0);
    G.attacker.cls         = 0;
    G.attacker.grid_slot   = (unsigned char)attacker_slot;
    G.attacker.signature   = signature_id;
    G.attacker.chain_flag  = (unsigned char)(burnout_chain_flag != 0);
    G.attacker.race_state  = 0;
    G.attacker.pv          = &G.att_pv;

    G.victim.crashed     = (unsigned char)(victim_crashed != 0);
    G.victim.grid_slot   = (unsigned char)victim_slot;
    G.victim.clock       = victim_clock;
    G.victim.crash_stamp = victim_crash_time;
    G.attacker.victim    = &G.victim;
    G.rec.slot           = attacker_slot;

    if (!b3_td_cam_gate(&G.rec, &G.attacker))
        return 0;

    G.sink.count = 0;
    b3_td_cam_enter(&G.rec, &G.attacker, &G.timer, &G.sink);
    drain_sink();
    G.attacker_slot = attacker_slot;
    G.victim_slot   = victim_slot;
    G.cam_primed    = 0;
    G.crashcam_primed = 0;
    return 1;
}

void b3_tdfx_set_attacker_crashed(int crashed)
{
    G.attacker.crashed = (unsigned char)(crashed != 0);
}

void b3_tdfx_set_race_finished(int finished)
{
    G.attacker.race_state = finished ? 3 : 0;
}

void b3_tdfx_set_aftertouch(int player_crashed, int boost_held)
{
    /* FUN_00118410's race branch (0x0011889A): while the player's own car
     * is crashed, holding boost requests divisor 5 and stamps the engaged
     * flag veh+0x4AC7; releasing it restores divisor 1 ONLY if that flag
     * was set (0x001188CC) -- which is why the crash's own divisor-5
     * request survives a crash with no aftertouch. */
    G.aftertouch_on = (player_crashed && boost_held) ? 1 : 0;
}

/* FUN_00026A70 @0x00026B18/0x00026B2D -- the ARM, and its gate.
 *
 * CORRECTION (RE_TAKEDOWN_FX 9.7).  The impact window is NOT a property of
 * wrecking.  Its only arm site is the game-context "big hit" virtual
 * `vtbl+0x54` = FUN_00026A70, whose ONLY caller in the image is
 * FUN_00112E70 @0x001134EF -- the car-vs-OBJECT contact response (RE_TD_RULES
 * section 8) -- and there it is gated on the touched object's record:
 *
 *     +0x174 & 8   must be set        [0x00026B18]
 *     +0x174 & 2   must be clear      [0x00026B2D]
 *
 * An ordinary wall crash never goes near it: FUN_0011AEF0's wall trigger
 * calls FUN_0010DCA0 directly with a NULL cause record (XOR EDI,EDI at
 * 0x0011B9F1).  Arming the window on every wreck is what truncated the
 * crash's own divisor-5 request at 0.40 game-seconds and dropped the rest of
 * the flight to full speed -- measured with the whole-sequence oracle
 * (tools/emulate_crash_traj.py seq_wall35): the apparent per-rendered-frame
 * rotation goes from a 2.4 deg/frame peak (unarmed) to 11.9 deg/frame
 * (armed), and the frozen crash camera starts swinging 100 degrees of yaw.
 *
 * So the arm is gated here on the same qualification.  With no object record
 * supplied the window does not arm, which is the correct answer for the wall
 * and car-vs-car crashes the harness actually produces. */
void b3_tdfx_qualify_big_hit(int flags174)
{
    G.big_hit_flags = flags174;
}

void b3_tdfx_impact_hit_object(int flags174)
{
    /* 0x00026B18 / 0x00026B2D: the two record-flag tests. */
    if (!(flags174 & 8) || (flags174 & 2))
        return;
    /* start = clock + 0.05; the dilation engages once clock - start > 0 and
     * ends once clock - start > 0.35.  `clock` is the DILATED DAT_0060EA20
     * (0x00026507), so the window is 0.35 s of GAME time -- 2.1 s of wall
     * clock at divisor 6. */
    G.impact_active = 1;
    G.impact_start  = G.timer.clock + B3_TDFX_IMPACT_DELAY;
}

void b3_tdfx_impact_hit(void)
{
    /* The qualification must have been supplied by a real object contact.
     * A bare wreck does not qualify, so this is a no-op unless
     * b3_tdfx_qualify_big_hit() armed it this frame. */
    b3_tdfx_impact_hit_object(G.big_hit_flags);
    G.big_hit_flags = 0;
}

void b3_tdfx_event_reset(void)
{
    /* FUN_00025AB0 @0x00025AE5: every car gets exactly one crash
     * presentation credit at the start of the event. */
    G.crash_credit      = 1;
    G.crash_slowmo_on   = 0;
    G.crash_slowmo_start = 0.0f;
    G.crash_req_pending = 0;
    G.crash_end_pending = 0;
}

int b3_tdfx_crash_begin(void)
{
    /* THE CRASH HUD CALLOUT.  The sign is a SEPARATE chain from the dilation
     * credit: FUN_001994D0 -> FUN_00199350 posts the message, and 0x7F
     * "CRASHED!" is one of the three ids whose accept test at 0x001993EF
     * REQUIRES the racecar's crashed flag (the other two are 0x80 TAKEN OUT
     * and 0x81 TAKEDOWN AVENGED).  At the wreck instant that flag is set, so
     * post it here -- before the credit gate, because the sign is shown on
     * every crash while the divisor-5 presentation is once per event.
     *
     * The message id doubles as the PRIORITY (0x0019937F), so when the same
     * wreck is also attributed as a takedown the harness's 0x80 "TAKEN OUT"
     * post overrides this one by itself; no ordering logic is needed. */
    b3_tdfx_post_callout(B3_TDFX_MSG_CRASHED, 1, 0);

    /* FUN_00025850 @0x00025890 -> FUN_00025CC0: the credit racecar+0x16C8
     * must be non-zero (`MOV EAX,[ESI+0x16C8]; TEST EAX,EAX; JE 0x258C4`);
     * FUN_00025CC0 decrements it and only runs the body once it reads 0. */
    if (G.crash_credit == 0)
        return 0;
    G.crash_credit--;
    if (G.crash_credit != 0)
        return 0;
    G.crash_req_pending = 1;      /* 0x00025D5C: DAT_0060EA24 = 5 */
    return 1;
}

/* FUN_00025A30 "OnTakedown" @0x00025A7E / @0x00025AA3 -> FUN_00025CC0 [C]:
 * a committed takedown spends one crash-presentation credit on the
 * ATTACKER's racecar (both sites pass the attacker in EAX) -- no HUD
 * callout (that chain is separate).  Retail gate:
 *   (attacker AI  && victim human)           -> spend, no FX request
 *   (attacker human && 0.3 [0x003B1750] > health_16C4) -> spend + FX
 * The divisor-5 request is only possible on the human arm because
 * FUN_00025CC0 @0x00025D29 needs racecar+0x1920 == 0. */
int b3_tdfx_takedown_credit(int attacker_is_human, int victim_is_human,
                            float health_16C4)
{
    int spend = 0, fx = 0;
    if (!attacker_is_human && victim_is_human) {
        spend = 1;
    } else if (attacker_is_human && 0.3f > health_16C4) {
        spend = 1;
        fx = 1;
    }
    if (!spend || G.crash_credit == 0)
        return 0;
    G.crash_credit--;
    if (!fx || G.crash_credit != 0)
        return 0;
    /* Retail's FUN_00025CC0 @0x00025D5C writes the same DAT_0060EA24 = 5,
     * but its surrounding presentation flow ends the dilation. In the port
     * the CRASH-path latch (crash_req_pending -> crash_slowmo_on) is only
     * released by a crash-mode exit, which a takedown never runs -- arming
     * it here left the game stuck at divisor 5 after the cam (user
     * regression report). The takedown cinematic's own timeline already
     * carries the dilation and restores at t = 2.8, so the credit is
     * spent WITHOUT arming the crash latch. */
    return 1;
}

void b3_tdfx_crash_end(void)
{
    /* FUN_00119C00 @0x00119C24 / FUN_000269D0 @0x000269FD */
    G.crash_end_pending = 1;
}

float b3_tdfx_update(float real_dt)
{
    float el;

    G.real_clock += real_dt;

    /* FUN_00110AF0 @0x00110EA2: `mov byte [edx+0x1353], 0` inside the walk
     * over the live vehicle list -- the per-frame reset of the flag byte.
     * It matters that this is here and not merged into the cinematic: every
     * other writer of +0x1353 in the whole image is an OR (the disassembly
     * has no AND against that offset at all), so the ONLY thing that ever
     * takes a bit down is this reset.  Without it the cinematic's 0x18 would
     * latch on the first takedown and never clear. */
    G.att_pv.flags1353 = 0;

    /* --- run the cinematic first: it owns the divisor while active --- */
    G.sink.count = 0;
    G.wheel_was = G.attacker.cam_active;
    if (G.rec.active)
        b3_td_cam_update(&G.rec, &G.attacker, &G.timer, &G.sink, real_dt);
    drain_sink();

    /* The AI-driver leg of the same grace.  While the autopilot has the
     * wheel the cinematic only stamps racecar+0x245D (0x00027C6D); it is
     * FUN_00105340, the driver that then runs for that car, which turns the
     * stamp into the crash-entry suppression:
     *
     *   0x00105EB3  cmp byte [racecar+0x245D],0 / jne  -> bl = 0
     *   0x00105EC9  mov byte [racecar+0x245D],0        (consumed, one frame)
     *   0x00105F91  test bl,bl / jne  ->
     *   0x00105F95  or  byte [veh+0x1353],0x18
     *
     * so t in [0, 2.8) is crash-proof for exactly the same reason
     * t in [2.8, 4.8) is -- the cinematic just reaches it through the driver
     * instead of writing the byte itself.  This harness's driver is the
     * ported b3_ai, which has no +0x245D leg, so the module closes the loop
     * here.  Order matters: the cinematic stamps hud_flag above, the driver
     * consumes it below, exactly as the two run per frame in retail.
     * [C-disasm] */
    if (G.attacker.hud_flag) {
        G.att_pv.flags1353 |= 0x18;
        G.attacker.hud_flag = 0;          /* 0x00105EC9 consumes the stamp */
        G.hud_flag_seen = 1;
    } else {
        G.hud_flag_seen = 0;
    }

    /* FUN_0018CB60 @0x0018CB94 fires on the 1 -> 0 edge of racecar+0x27D8:
     * the attacker's steering authority veh+0x1534 goes back to 1.0.  Publish
     * the edge so the harness can apply the same restore to its own
     * out-of-control window -- in retail the restore sticks because the pad,
     * not FUN_00105340, drives the car from that frame on. */
    G.wheel_release = (G.wheel_was && !G.attacker.cam_active);

    /* ------------------------------------------------------------------
     * The retail machine has ONE request slot (DAT_0060EA24) and several
     * writers; the emergent timeline is the order they run in per frame.
     * Retail order (RE_TAKEDOWN_FX section 9):
     *   1. FUN_0011BE50 -> FUN_00118410     aftertouch  (vehicle step)
     *   2. FUN_00025850 -> FUN_00025CC0     the wreck instant
     *   3. FUN_00026D30 -> FUN_00026050     the impact-hit machine
     *   4. FUN_00119C00 / FUN_000269D0      crash-mode exit
     * The cinematic (FUN_00027AD0) owns the slot outright while it runs,
     * so it is evaluated first and the rest are skipped -- GLUE priority,
     * retail relies on the modes being mutually exclusive.
     * ---------------------------------------------------------------- */
    if (!G.rec.active) {
        /* 1. aftertouch: 0x001188A4 engage / 0x001188D6 release.  The
         * release only fires when the engaged flag veh+0x4AC7 was set. */
        if (G.aftertouch_on) {
            G.timer.requested   = B3_TDFX_DIV_AFTERTOUCH;
            G.aftertouch_engaged = 1;
        } else if (G.aftertouch_engaged) {
            G.timer.requested    = B3_TDFX_DIV_NORMAL;
            G.aftertouch_engaged = 0;
            G.crash_slowmo_on    = 0;
        }

        /* 2. the wreck instant: one request, then it latches. */
        if (G.crash_req_pending) {
            G.timer.requested   = B3_TDFX_DIV_AFTERTOUCH;   /* 0x00025D5C */
            G.crash_slowmo_on   = 1;
            G.crash_slowmo_start = G.timer.clock;
            G.crash_req_pending = 0;
        }
        /* 2b. ...and its BOUND.  Retail does not hold divisor 5 for the
         * whole wreck: DAT_0060EA24 has twelve writers and a graduated
         * profile (5 @0x0002795F / @0x00025D5C, 4 @0x00118986, 3
         * @0x0011888E, 1 @0x00026695 / @0x0002678D), and the restore at
         * @0x00026695 fires off a presentation state byte `[ebp+0x52]`
         * whose setter lives in unmapped code @0x00026xxx -- so the exact
         * end condition is [?].  Holding it to the respawn instead made a
         * crash last 5 GAME seconds at divisor 5 = 24.93 s of WALL clock
         * (measured), which the user reported as "stays in crash mode
         * seemingly forever"; an undilated crash measures 4.98 s.
         * Bounded here by the one dilation-window length that IS recovered,
         * B3_TDFX_IMPACT_LEN = 0.35 s of dilated clock ([0x0039B2B0], the
         * impact machine's own window), which restores the wall-clock crash
         * to ~5 s while keeping the slow-mo hit.  [S] -- replace with the
         * real @0x00026xxx machine when its setter is recovered. */
        if (G.crash_slowmo_on && !G.impact_active
            && G.timer.clock - G.crash_slowmo_start > B3_TDFX_IMPACT_LEN) {
            G.timer.requested = B3_TDFX_DIV_NORMAL;
            G.crash_slowmo_on = 0;
        }

        /* 3. the impact-hit machine, on the DILATED clock. */
        if (G.impact_active) {
            el = G.timer.clock - G.impact_start;
            if (el > B3_TDFX_IMPACT_LEN) {
                G.impact_active   = 0;
                G.timer.requested = B3_TDFX_DIV_NORMAL;     /* 0x00026525 */
                G.crash_slowmo_on = 0;   /* it took the crash's request */
            } else if (el > 0.0f) {
                G.timer.requested = B3_TDFX_DIV_IMPACT;     /* 0x0002655B */
            }
        }

        /* 4. crash-mode exit. */
        if (G.crash_end_pending) {
            G.timer.requested    = B3_TDFX_DIV_NORMAL;      /* 0x00119C24 */
            G.crash_end_pending  = 0;
            G.crash_slowmo_on    = 0;
            G.impact_active      = 0;
            G.aftertouch_engaged = 0;
        }
    }

    callout_step(real_dt);

    /* --- tick the real timer --- */
    G.frame_counter++;
    b3_tdfx_timer_tick(&G.timer, G.frame_counter);

    /* THE SIMULATION STEP.  Retail's is `timer+0x1C = period / divisor`,
     * written by the rescale tail of FUN_001B5B60 (the same line this file
     * ports as b3_tdfx_timer_rescale, `t->dt = t->period / req`) and read as
     * DAT_0060EA1C -- a FIXED step per rendered frame that never consumes
     * wall time.                                                       [C]
     *
     * PHYS-LEDGER wave 4: the old "GLUE, identical at 60 Hz" mark here is
     * STALE.  It was written while the harness fed this function a
     * wall-clock dt; the frame lock landed afterwards, and
     * burnout3_full.c's loop now hands b3_tdfx_step the NOMINAL period
     * unconditionally (`g_tdfx_real_dt = 0.016666668f`, which is
     * [0x003B1838], or B3_FIXED_DT for a deterministic test run).  So
     * `real_dt / divisor` IS `period / divisor` -- the same expression, bit
     * for bit, and the divisor itself is the recovered
     * FUN_001B5AC0/FUN_001B5B60 pair above.  The caller's period is kept in
     * the expression (rather than G.timer.period) only so a B3_FIXED_DT run
     * still scales; that is the one harness-side condition. */
    G.sim_dt = real_dt / (float)G.timer.divisor;
    return G.sim_dt;
}

float b3_tdfx_sim_dt(void) { return G.sim_dt; }

int   b3_tdfx_divisor(void)   { return G.timer.divisor; }
float b3_tdfx_timescale(void) { return 1.0f / (float)G.timer.divisor; }
float b3_tdfx_pitch(void)
{
    return (G.timer.divisor == 1) ? B3_TDFX_PITCH_NORMAL
                                  : B3_TDFX_PITCH_DILATED;
}

void b3_tdfx_status(B3TdfxStatus *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->active     = G.rec.active;
    out->t          = G.rec.active ? G.rec.elapsed : 0.0f;
    out->slowmo     = (G.timer.divisor != 1);
    out->divisor    = G.timer.divisor;
    out->timescale  = 1.0f / (float)G.timer.divisor;
    out->pitch      = b3_tdfx_pitch();
    out->dt         = G.timer.dt;
    out->clock      = G.timer.clock;
    out->real_clock = G.real_clock;
    out->victim_slot   = G.victim_slot;
    out->attacker_slot = G.attacker_slot;
    out->callout_visible = G.callout.visible && G.callout.phase < 3;
    out->callout_age     = out->callout_visible ? G.callout.elapsed : 0.0f;
    out->callout_sign    = out->callout_visible ? G.callout.sign_index : -1;
    out->callout_msg     = out->callout_visible ? G.callout.msg_id : -1;
    out->callout_string  = out->callout_visible ? G.callout.string_index : -1;
    out->callout_phase   = G.callout.phase;
    out->callout_phase_u = G.callout.phase_u;
    out->callout_event   = G.last_event;

    /* the attacker's control + crash state (see B3TdfxStatus) */
    out->ai_wheel       = G.attacker.cam_active;          /* +0x27D8 */
    out->att_flags1353  = G.att_pv.flags1353;             /* +0x1353 */
    out->att_crash_off  = (G.att_pv.flags1353 & 0x18) != 0;
    out->att_authority  = G.att_pv.authority;             /* +0x1534 */
    out->att_hud_flag   = G.hud_flag_seen;                /* +0x245D */
    out->handback       = G.rec.active && !G.attacker.cam_active;
    out->wheel_release  = G.wheel_release;
    /* the camera is released with the presentation state at 2.5, not at the
     * 4.8 s record exit -- see B3TdfxStatus for the [C] chain behind the [S] */
    out->cam_hold       = G.rec.active
                          && G.rec.elapsed < B3_TDFX_CINE_CALLOUT;
}

/* ----------------------------------------------------------------------
 * THE CAMERA  (RE_TAKEDOWN_FX section 8)
 *
 * Recovered [C]:
 *   - the cinematic's subject is the VICTIM (record+0x00 = attacker
 *     racecar +0x15A4);
 *   - the retail camera is a 20-mode director; the player chase camera is
 *     mode 2/3 (class vtable 0x003A9C28, update FUN_0015E550) and it is
 *     the only mode wired to the "Camera/Follow" ValueDB group;
 *   - that update's smoothing core, executed under Unicorn
 *     (tools/emulate_tdfx_camera.py follow_blend, span
 *     0x0015E5B6..0x0015E734):
 *         n     = (int)(60.0f * dt + 0.5f)
 *         gate  = min(1.0f, speed_ms * 2.236936330795288f * 0.02f)
 *         blend = 1 - ((1 - SpringCoeff) * gate)^n
 *     with SpringCoeff = (0.06, 0.1, 5.0) and the pitch axis gated by
 *     speed while the yaw axis is not;
 *   - the angle step itself (0x0015E885..0x0015E8A3) [S]:
 *         angle += (target - angle) * blend
 *     with pitch about X (axis constant 0x00414AD0 = (1,0,0), applied
 *     negated) and yaw about Y (0x00414AE0 = (0,1,0));
 *   - FOV = 90 degrees (0x003B1850, stored into the camera state at
 *     0x001678B5 / 0x001679D8).
 *
 * RETRACTED: the previous revision of this module used the "Crash/HandyCam"
 * spring 15/damping 10 from Data/vdb.xml as the camera coefficients.  That
 * group is registered (FUN_00160B90, manager DAT_0047A134 + 0x2C30) and is
 * VDB-overridable, but NO instruction in .text reads it -- the only two
 * references to the block are the constructor LEA at 0x00167273 and the
 * destructor ADD at 0x001AAA64.  It is dead config in this build, so those
 * numbers are gone from the law (validate_takedown section 10 asserts the
 * negative).
 *
 * STILL [?] and therefore GLUE, marked inline: which director mode retail
 * switches to for the takedown cinematic and for a player crash, and how
 * the camera's target angles are derived from the subject each frame.  The
 * target-angle choice below is this harness's own.
 * -------------------------------------------------------------------- */

int b3_cam_substeps(float dt)
{
    /* 0x0015E5DC: 60.0f (0x003F720C) * dt + 0.5f (0x003B1684), truncated. */
    int n = (int)(60.0f * dt + 0.5f);
    return n;
}

float b3_cam_speed_gate(float speed_ms)
{
    /* 0x0015E5B6: mph = speed_ms * 2.236936330795288f (0x0038994C), then
     * * 0.02f (0x003B1A08), clamped at 1.0f (0x003B168C). */
    float g = speed_ms * 2.236936330795288f * 0.02f;
    if (g > 1.0f)
        g = 1.0f;
    return g;
}

float b3_cam_follow_blend(float spring, float gate, int n)
{
    /* 0x0015E630..0x0015E74B: retain = ((1 - spring) * gate)^n by repeated
     * multiplication (the retail code unrolls it eight at a time; the
     * product is identical), blend = 1 - retain. */
    float retain = (1.0f - spring) * gate;
    float acc = 1.0f;
    int i;
    for (i = 0; i < n; i++)
        acc *= retain;
    return 1.0f - acc;
}

float b3_cam_smooth_angle(float cur_deg, float target_deg, float blend)
{
    /* 0x0015E885 sub / 0x0015E894 mul / 0x0015E898 add / 0x0015E8A3 store */
    return cur_deg + (target_deg - cur_deg) * blend;
}

/* Shared body: place an eye using the recovered Camera/Follow geometry and
 * the recovered angle smoothing around a subject.  `yaw_target` is GLUE. */
static void b3_cam_step(float st_pitch[1], float st_yaw[1], int *primed,
                        const float subject[3], float speed_ms, float dt,
                        float yaw_target_deg, B3TdfxCamera *out)
{
    int   n     = b3_cam_substeps(dt);
    float gate  = b3_cam_speed_gate(speed_ms);
    float bx    = b3_cam_follow_blend(B3_CAM_SPRING_X, gate, n);  /* pitch */
    float by    = b3_cam_follow_blend(B3_CAM_SPRING_Y, 1.0f, n);  /* yaw   */
    float pitch, yaw, cy, sy, cp, sp;
    float ox, oy, oz, fx, fy, fz;

    /* GLUE: retail derives the pitch target from the subject's transform;
     * the harness uses the recovered Down Angle as a constant target. */
    if (!*primed) {
        st_pitch[0] = B3_CAM_DOWN_ANGLE;
        st_yaw[0]   = yaw_target_deg;
        *primed = 1;
    } else {
        float dy = yaw_target_deg - st_yaw[0];
        while (dy >  180.0f) dy -= 360.0f;
        while (dy < -180.0f) dy += 360.0f;
        st_yaw[0]   = b3_cam_smooth_angle(st_yaw[0], st_yaw[0] + dy, by);
        st_pitch[0] = b3_cam_smooth_angle(st_pitch[0], B3_CAM_DOWN_ANGLE, bx);
    }
    pitch = st_pitch[0];
    yaw   = st_yaw[0];

    cy = cosf(yaw   * 0.017453293f);  sy = sinf(yaw   * 0.017453293f);
    cp = cosf(pitch * 0.017453293f);  sp = sinf(pitch * 0.017453293f);

    /* Camera Offset, rotated pitch-about-X then yaw-about-Y (the retail
     * composition order, FUN_0015E8AC then FUN_0015E942). */
    ox = B3_CAM_FOLLOW_OFF_X;
    oy = B3_CAM_FOLLOW_OFF_Y * cp - B3_CAM_FOLLOW_OFF_Z * sp;
    oz = B3_CAM_FOLLOW_OFF_Y * sp + B3_CAM_FOLLOW_OFF_Z * cp;
    out->eye[0] = subject[0] + (ox * cy + oz * sy);
    out->eye[1] = subject[1] + oy;
    out->eye[2] = subject[2] + (oz * cy - ox * sy);

    fx = B3_CAM_FOCUS_OFF_X;
    fy = B3_CAM_FOCUS_OFF_Y;
    fz = B3_CAM_FOCUS_OFF_Z;
    out->look[0] = subject[0] + (fx * cy + fz * sy);
    out->look[1] = subject[1] + fy;
    out->look[2] = subject[2] + (fz * cy - fx * sy);

    out->fov       = B3_CAM_FOV;
    out->pitch_deg = pitch;
    out->yaw_deg   = yaw;
}

/* GLUE: the yaw the harness aims the shot along.  Retail picks a director
 * mode whose framing is [?]; this points the camera down the attacker ->
 * victim line rotated 90 degrees, so the hit reads broadside. */
static float b3_cam_broadside_yaw(const float subject[3],
                                  const float other[3])
{
    float dx, dz;
    if (!other)
        return 0.0f;
    dx = subject[0] - other[0];
    dz = subject[2] - other[2];
    if (dx * dx + dz * dz < 1e-6f)
        return 0.0f;
    /* rotate the separation 90 degrees about up, then take its bearing */
    return atan2f(dz, -dx) * 57.29578f;
}

int b3_tdfx_camera(const float victim_pos[3], const float victim_vel[3],
                   const float attacker_pos[3], float real_dt,
                   B3TdfxCamera *out)
{
    float t, w, speed = 0.0f, yaw;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!G.rec.active || !victim_pos)
        return 0;

    t = G.rec.elapsed;
    /* GLUE: ease in over the first 0.15 s and out over the last 0.4 s. */
    w = 1.0f;
    if (t < 0.15f)
        w = t / 0.15f;
    if (t > B3_TDFX_CINE_TOTAL - 0.4f)
        w = (B3_TDFX_CINE_TOTAL - t) / 0.4f;
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;

    if (victim_vel)
        speed = sqrtf(victim_vel[0] * victim_vel[0] +
                      victim_vel[1] * victim_vel[1] +
                      victim_vel[2] * victim_vel[2]);
    yaw = b3_cam_broadside_yaw(victim_pos, attacker_pos);

    b3_cam_step(&G.cam_pitch, &G.cam_yaw, &G.cam_primed,
                victim_pos, speed, real_dt, yaw, out);
    out->active = 1;
    out->weight = w;
    return 1;
}

/* ----------------------------------------------------------------------
 * THE WHOLE CHASE-CAMERA UPDATE  --  FUN_0015E550, 1:1  [C]
 *
 * Executed end to end under Unicorn by
 * tools/emulate_tdfx_camera.py:follow_update and diffed frame-by-frame
 * against this C in validate_takedown section 11.  Every line below cites
 * the instruction it mirrors; see docs/RE_TAKEDOWN_FX.md section 9.
 * -------------------------------------------------------------------- */

/* FUN_00011900's matrices, row-vector, angle in DEGREES (0x0001190C
 * multiplies by 0.01745329238474369 before fsin/fcos). */
static void cam_rotx(float m[9], float deg)
{
    float a = deg * B3_CAM_DEG2RAD;
    float c = (float)cos((double)a), s = (float)sin((double)a);
    m[0] = 1; m[1] = 0; m[2] = 0;
    m[3] = 0; m[4] = c; m[5] = s;
    m[6] = 0; m[7] = -s; m[8] = c;
}

static void cam_roty(float m[9], float deg)
{
    float a = deg * B3_CAM_DEG2RAD;
    float c = (float)cos((double)a), s = (float)sin((double)a);
    m[0] = c; m[1] = 0; m[2] = -s;
    m[3] = 0; m[4] = 1; m[5] = 0;
    m[6] = s; m[7] = 0; m[8] = c;
}

/* FUN_000116E0: out = A . B, row-vector (out.row_i = A.row_i applied to B) */
static void cam_mul3(float out[9], const float a[9], const float b[9])
{
    int i;
    float t[9];
    for (i = 0; i < 3; i++) {
        t[i * 3 + 0] = a[i * 3] * b[0] + a[i * 3 + 1] * b[3] + a[i * 3 + 2] * b[6];
        t[i * 3 + 1] = a[i * 3] * b[1] + a[i * 3 + 1] * b[4] + a[i * 3 + 2] * b[7];
        t[i * 3 + 2] = a[i * 3] * b[2] + a[i * 3 + 1] * b[5] + a[i * 3 + 2] * b[8];
    }
    memcpy(out, t, sizeof(t));
}

static void cam_xform(float out[3], const float v[3], const float m[9])
{
    float x = v[0] * m[0] + v[1] * m[3] + v[2] * m[6];
    float y = v[0] * m[1] + v[1] * m[4] + v[2] * m[7];
    float z = v[0] * m[2] + v[1] * m[5] + v[2] * m[8];
    out[0] = x; out[1] = y; out[2] = z;
}

/* FUN_00011B10: rotation matrix -> quaternion (camera state +0x20). */
static void cam_quat(float q[4], const float m[9])
{
    float tr = m[0] + m[4] + m[8];
    if (tr > 0.0f) {
        float s = (float)sqrt((double)(tr + 1.0f));
        q[3] = s * 0.5f;
        s = 0.5f / s;
        q[0] = (m[5] - m[7]) * s;
        q[1] = (m[6] - m[2]) * s;
        q[2] = (m[1] - m[3]) * s;
    } else {
        int i = 0, j, k;
        float s;
        if (m[4] > m[0]) i = 1;
        if (m[8] > m[i * 3 + i]) i = 2;
        j = (i + 1) % 3; k = (j + 1) % 3;
        s = (float)sqrt((double)(m[i * 3 + i] - m[j * 3 + j] - m[k * 3 + k] + 1.0f));
        q[i] = s * 0.5f;
        s = 0.5f / s;
        q[3] = (m[j * 3 + k] - m[k * 3 + j]) * s;
        q[j] = (m[i * 3 + j] + m[j * 3 + i]) * s;
        q[k] = (m[i * 3 + k] + m[k * 3 + i]) * s;
    }
}

void b3_cam_follow_init(B3CamFollow *st)
{
    if (!st)
        return;
    st->yaw_deg = 0.0f;
    st->pitch_deg = 0.0f;
    st->yaw_gate = 1;     /* FUN_0015E060 @0x0015E072 */
    st->look_back = 0;
}

void b3_cam_follow_update(B3CamFollow *st, const float car_rows[12],
                          float speed_ms, float boost_ramp, float dt,
                          B3TdfxCamera *out)
{
    float R[3], U[3], F[3], P[3];
    float focus[3], eye[3], off[3];
    float rot[9], tmp[9], basis[9];
    float gate, bp, by, pt, c, ang, d, fov, push, l1, l2, sgn;
    float v1[3], v2[3];
    int   n, i;

    if (!st || !car_rows || !out)
        return;
    memset(out, 0, sizeof(*out));

    for (i = 0; i < 3; i++) {
        R[i] = car_rows[0 + i];
        U[i] = car_rows[3 + i];
        F[i] = car_rows[6 + i];
        P[i] = car_rows[9 + i];
    }
    /* 0x0015E753: the look-back flag negates the forward and right rows */
    if (st->look_back)
        for (i = 0; i < 3; i++) { F[i] = -F[i]; R[i] = -R[i]; }

    /* 0x0015E5B6 / 0x0015E5DC / 0x0015E630..0x0015E745 */
    n    = b3_cam_substeps(dt);
    gate = b3_cam_speed_gate(speed_ms);
    bp   = b3_cam_follow_blend(B3_CAM_SPRING_X, gate, n);   /* pitch, gated */
    by   = b3_cam_follow_blend(B3_CAM_SPRING_Y, 1.0f, n);   /* yaw, ungated */

    /* 0x0015E7B7..0x0015E7EE: the focus anchor lives in the CAR's frame */
    for (i = 0; i < 3; i++)
        focus[i] = P[i] + R[i] * B3_CAM_FOCUS_OFF_X
                        + U[i] * B3_CAM_FOCUS_OFF_Y
                        + F[i] * B3_CAM_FOCUS_OFF_Z;

    /* 0x0015E831..0x0015E8A3: pitch target = asin(car.forward.y) degrees */
    c  = F[1];
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    pt = (float)atan2((double)c, sqrt((double)(1.0f - c * c))) * B3_CAM_RAD2DEG;
    st->pitch_deg = b3_cam_smooth_angle(st->pitch_deg, pt, bp);

    /* 0x0015E7F6..0x0015E829: M2 = translate(Camera Offset), identity rows
     * 0x0015E8AC / 0x0015E8F7: M2 = M2 . Rx(-pitch)
     * 0x0015E942 / 0x0015E98D: M2 = M2 . Ry(yaw)                        */
    off[0] = B3_CAM_FOLLOW_OFF_X;
    off[1] = B3_CAM_FOLLOW_OFF_Y;
    off[2] = B3_CAM_FOLLOW_OFF_Z;
    cam_rotx(rot, -st->pitch_deg);
    cam_roty(tmp, st->yaw_deg);
    cam_mul3(basis, rot, tmp);              /* Rx(-pitch) . Ry(yaw) */
    cam_xform(off, off, basis);

    /* 0x0015E9C9..0x0015EBC9: the yaw error, both forwards flattened */
    v1[0] = F[0]; v1[1] = 0.0f; v1[2] = F[2];
    v2[0] = basis[6]; v2[1] = 0.0f; v2[2] = basis[8];
    /* the retail sequence is: len = sqrt(dot3), s = 1.0f/len (a scalar
     * DIVSS at 0x0015EA68 / 0x0015EAC0), v *= broadcast(s), then a
     * component-wise MULPS and a 3-term horizontal add -- reproduced
     * exactly, because the reciprocal form rounds differently and the
     * near-1.0 dot is what the yaw error is made of. */
    l1 = (float)sqrt((double)((v1[0] * v1[0] + v1[1] * v1[1]) + v1[2] * v1[2]));
    l2 = (float)sqrt((double)((v2[0] * v2[0] + v2[1] * v2[1]) + v2[2] * v2[2]));
    if (l1 == 0.0f || l2 == 0.0f) {
        c = 1.0f;
    } else {
        float s1 = 1.0f / l1, s2 = 1.0f / l2;
        float p0 = (v1[0] * s1) * (v2[0] * s2);
        float p1 = (v1[1] * s1) * (v2[1] * s2);
        float p2 = (v1[2] * s1) * (v2[2] * s2);
        c = (p0 + p1) + p2;
    }
    if (c < -1.0f) c = -1.0f;               /* 0x0015EB20 maxss -1 */
    if (c >  1.0f) c =  1.0f;               /* 0x0015EB26 minss +1 */
    ang = (float)(1.5707963705062866
                  - atan2((double)c, sqrt((double)(1.0f - c * c))))
        * B3_CAM_RAD2DEG;                   /* = acos(c), 0x0015EB82 */
    sgn = F[0] * basis[0] + F[1] * basis[1] + F[2] * basis[2];
    if (sgn < 0.0f)                         /* 0x0015EBB9 */
        ang = -ang;
    d = ang * by;                           /* 0x0015EBC9 */
    if (st->yaw_gate)                       /* 0x0015EBC4 */
        st->yaw_deg += d;
    cam_roty(tmp, d);                       /* 0x0015EBEF */
    cam_mul3(basis, basis, tmp);            /* 0x0015EC3A */
    cam_xform(off, off, tmp);

    /* 0x0015EC79: eye = rotated Camera Offset + the focus anchor */
    for (i = 0; i < 3; i++)
        eye[i] = off[i] + focus[i];

    /* 0x0015EC88..0x0015ECDE: basis = Rx(Down Angle) . basis, eye kept */
    cam_rotx(rot, B3_CAM_DOWN_ANGLE);
    cam_mul3(basis, rot, basis);

    /* 0x0015ED2E..0x0015ED5C: FOV from the boost ramp racecar+0x11AC */
    c   = boost_ramp - 1.0f;
    fov = (1.0f - c * c) * (B3_CAM_FOV_BOOST - B3_CAM_FOV) + B3_CAM_FOV;

    /* 0x0015ED61..0x0015EDC6: dolly the eye so the wider FOV keeps the car
     * the same size:  eye += forward * (2.3 - 2.3/tan(fov/2)) */
    push = (float)tan((double)(fov * 0.5f * B3_CAM_DEG2RAD));
    push = B3_CAM_FOV_PUSH - (1.0f / push) * B3_CAM_FOV_PUSH;
    for (i = 0; i < 3; i++)
        eye[i] += basis[6 + i] * push;

    for (i = 0; i < 3; i++) {
        out->eye[i]   = eye[i];
        out->focus[i] = focus[i];
        out->right[i] = basis[0 + i];
        out->up[i]    = basis[3 + i];
        out->fwd[i]   = basis[6 + i];
        /* the harness's look-at convenience: a point down the view ray */
        out->look[i]  = eye[i] + basis[6 + i] * 10.0f;
    }
    cam_quat(out->quat, basis);
    out->fov       = fov;
    out->pitch_deg = st->pitch_deg;
    out->yaw_deg   = st->yaw_deg;
    out->active    = 1;
    out->weight    = 1.0f;
}

void b3_tdfx_crash_camera_reset(void)
{
    b3_cam_follow_init(&G.crashcam);
    G.crashcam_primed = 0;
}

/* THE CAMERA'S dt -- FUN_000170B0 @0x00017147..0x0001717A [C-disasm].
 *
 * CORRECTION (RE_TAKEDOWN_FX 9.7).  This path used to be driven with the
 * UNDILATED frame delta.  The retail main loop does not do that:
 *
 *     00017147  CVTSI2SS XMM0,dword ptr [EDI + 0x2E20C]   ; sim ticks
 *     0001714F  MULSS    XMM0,dword ptr [0x0060EA1C]      ; * the DILATED dt
 *     0001715C  MOVSS    dword ptr [EDI + 0x2E210],XMM0
 *     0001716F  MOV EAX,dword ptr [ESP] ; PUSH EAX        ; -> param_1
 *     0001717A  CALL FUN_00167940
 *
 * and FUN_00167940's 20-mode dispatch hands `param_1` to every mode except
 * 0 and 14, which alone get the real DAT_004AE1FC.  The chase camera is
 * mode 2, so it runs on ticks * DAT_0060EA1C -- i.e. the DILATED dt.
 *
 * That is not a detail.  FUN_0015E550's blend exponent is
 * n = floor(60*dt + 0.5) (0x0015E5DC), so at divisor 5 (dt = 1/300) n = 0,
 * every blend is 1 - x^0 = 0, and the camera's yaw and pitch HOLD for the
 * whole dilated window -- only the focus anchor tracks the shell.  Fed the
 * undilated 1/60 instead, n = 1 and the speed gate has collapsed to ~0 by
 * then, so blend_pitch ~ 1: the camera SNAPS its pitch to
 * asin(wreck.forward.y) every rendered frame, and a tumbling wreck sweeps
 * that through the full +-90 degrees.  The view then counter-rotates with
 * the tumble and the shell reads as spinning far faster than it is.
 * Measured against the whole-sequence oracle (tools/emulate_crash_traj.py
 * seq_wall35, unarmed): retail holds yaw at -0.01 deg and pitch at
 * -0.81 deg for all 300 rendered frames of the crash.
 *
 * `real_dt` is therefore only the fallback for callers that run this outside
 * the dilation machine; the live path uses the dilated dt the timer
 * produced this frame. */
int b3_tdfx_crash_camera_x(const float car_rows[12], float speed_ms,
                           float boost_ramp, float real_dt,
                           B3TdfxCamera *out)
{
    float dt = (G.sim_dt > 0.0f) ? G.sim_dt : real_dt;
    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!car_rows)
        return 0;
    if (!G.crashcam_primed) {
        b3_cam_follow_init(&G.crashcam);
        /* FUN_0015E060 (the mode's enter) seeds the yaw from the car's own
         * heading so the first frame does not swing; 0x0015E06F.. */
        G.crashcam.yaw_deg = (float)atan2((double)car_rows[6],
                                          (double)car_rows[8]) * B3_CAM_RAD2DEG;
        G.crashcam_primed = 1;
    }
    b3_cam_follow_update(&G.crashcam, car_rows, speed_ms, boost_ramp,
                         dt, out);
    return 1;
}

int b3_tdfx_crash_camera(const float wreck_pos[3], const float wreck_vel[3],
                         float wreck_speed_ms, float real_dt,
                         B3TdfxCamera *out)
{
    /* DEGRADED compatibility path: build a levelled basis from the travel
     * direction.  The recovered law pitches on asin(car.forward.y), which a
     * levelled basis can never produce -- use b3_tdfx_crash_camera_x. */
    float rows[12];
    float fx = 0.0f, fz = 1.0f, l;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!wreck_pos)
        return 0;
    if (wreck_vel) {
        l = (float)sqrt((double)(wreck_vel[0] * wreck_vel[0]
                                 + wreck_vel[2] * wreck_vel[2]));
        if (l > 1e-3f) { fx = wreck_vel[0] / l; fz = wreck_vel[2] / l; }
    }
    rows[0] = fz;  rows[1] = 0.0f; rows[2] = -fx;
    rows[3] = 0.0f; rows[4] = 1.0f; rows[5] = 0.0f;
    rows[6] = fx;  rows[7] = 0.0f; rows[8] = fz;
    rows[9] = wreck_pos[0]; rows[10] = wreck_pos[1]; rows[11] = wreck_pos[2];
    return b3_tdfx_crash_camera_x(rows, wreck_speed_ms, 0.0f, real_dt, out);
}
