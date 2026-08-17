/* ===========================================================================
 * Takedown trigger rules -- see burnout3_td_rules.h and docs/RE_TD_RULES.md.
 *
 * Every function below names the retail function it mirrors and the address
 * of every constant it uses.  Nothing here invents behaviour: where a retail
 * branch depends on something this harness does not model the branch is
 * reproduced with the modelled inputs and the omission is marked [S] or GLUE
 * inline.
 * ===========================================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include "burnout3_td_rules.h"
#include "burnout3_crash.h"

#include <math.h>
#include <string.h>

/* Double Takedown BP[4] @0x003F7508 and Takedown Spree BP[4] @0x003F7518,
 * retail Data/vdb.xml values (docs/RE_GAMEPLAY.md 2). */
const int B3_TDR_BP_DOUBLE[4] = {300, 500, 750, 1000};
const int B3_TDR_BP_SPREE[4]  = {300, 500, 750, 1000};

/* FUN_001994D0 @0x001998D5 clamps the double index with min(n-2, 4) and then
 * indexes the 4-entry table at 0x003F7508 -- index 4 reads one past it, i.e.
 * DAT_003F7518 = Takedown Spree BP[0] = 300.  Reproduced verbatim. */
static const int TDR_DOUBLE_BP5[5] = {300, 500, 750, 1000, 300};

#define TDR_CLAIM_IDLE   (-1.0f)

static float tdr_dist2(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static int tdr_valid(const B3TdRules* R, int s)
{
    return R && s >= 0 && s < R->ncars && s < B3_TDR_MAX_CARS;
}

/* ---------------------------------------------------------------------------
 * crash-cause records (the 16-byte record handed to FUN_0010DCA0 in EAX)
 * ------------------------------------------------------------------------- */
void b3_td_cause_none(B3TdCause* c)
{
    /* FUN_001121F0 @0x00112881 / @0x00112959, FUN_0011BE50 @0x0011C27F and
     * FUN_00197260 @0x00197396 all do `xor esi,esi` -- no record at all. */
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->by_wreck = -1;
}

void b3_td_cause_wall(B3TdCause* c, int surface)
{
    /* FUN_0011AEF0 @0x0011B9AE..0x0011B9D3:
     *   [esp+0x30] = 1     (+0x00 -> wall)
     *   [esp+0x31] = 0     (+0x01)
     *   [esp+0x34] = (u16 obj+0x190) & 0xFF      (+0x04 surface)
     *   [esp+0x38] = [esp+0x3C] = 0              (+0x08 / +0x0C) */
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->present = 1;
    c->wall = 1;
    c->surface = surface;
    c->by_wreck = -1;
}

void b3_td_cause_object(B3TdCause* c, int vclass)
{
    /* FUN_00112E70 @0x00113556..0x0011356B:
     *   [esp+0x50] = 0, [esp+0x51] = 1, [esp+0x58] = the object, [esp+0x5C]=0.
     * FUN_001994D0 @0x0019958C then switches on object+0x173. */
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->present = 1;
    c->has_obj = 1;
    c->obj_class = vclass;
    c->by_wreck = -1;
}

void b3_td_cause_wreck(B3TdCause* c, int wreck_slot)
{
    /* FUN_00113960 @0x001140DA zeroes +0x00/+0x01/+0x08/+0x0C, then
     * @0x0011411C stores the CRASHED car's racecar into +0x0C when the other
     * object is a car (FUN_0010C550). */
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->present = 1;
    c->by_wreck = wreck_slot;
}

/* ---------------------------------------------------------------------------
 * lifecycle
 * ------------------------------------------------------------------------- */
void b3_td_reset(B3TdRules* R, int ncars)
{
    int i, j;
    if (!R) return;
    memset(R, 0, sizeof(*R));
    R->ncars = ncars > B3_TDR_MAX_CARS ? B3_TDR_MAX_CARS : ncars;
    R->single_player = 1;                     /* DAT_0073A1C0 == 1 */
    for (i = 0; i < B3_TDR_MAX_CARS; i++) {
        B3TdCar* c = &R->car[i];
        c->grid = i;
        c->slam_time = -1.0f;                 /* +0x1598 idle */
        c->aggressor = -1;                    /* +0x16BC     */
        c->aggressor_time = -1.0f;            /* +0x16C0     */
        c->crash_time = -1.0f;                /* +0x140C     */
        c->last_slam_time = -1.0f;            /* +0x158C     */
        c->td_by = -1;
        c->last_victim = -1;
        c->psyche_target = -1;                /* +0x1684     */
        c->dbl_window = -1.0f;
        c->spree_window = -1.0f;
        c->recover_at = -1.0f;
        c->shunt_victim_time = -1.0f;
        c->view_dist2 = -1.0f;                /* ladder input unknown */
        b3_td_cause_none(&c->cause);
        for (j = 0; j < B3_TDR_MAX_CARS; j++) c->claim[j] = TDR_CLAIM_IDLE;
    }
}

void b3_td_rules_init(void) { /* legacy entry; state lives in the caller's B3TdRules */ }

void b3_td_set_car(B3TdRules* R, int slot, int cls, int grid)
{
    if (!tdr_valid(R, slot)) return;
    R->car[slot].cls = cls;
    R->car[slot].grid = grid;
}

/* ---------------------------------------------------------------------------
 * FUN_001979E0 -- the rub / contact stamp (game-context +0x64, kind 1)
 *
 *   if (score+0x27C != 3 && !racecar+0x18FA) {
 *       score+0x55E[other grid] = 1 ;  score+0x510[other grid] = clock
 *   }
 * [C-disasm 0x001979E0]
 *
 * The score+0x528[] *accumulated* contact time FUN_00197430 tests against
 * "Min Collide Time to enable TD" is grown by the rubbing pass FUN_00194A80
 * [S]; this harness accumulates it here from the same touching flag.
 * ------------------------------------------------------------------------- */
static void tdr_contact_stamp(B3TdRules* R, float clock, int me, int other)
{
    B3TdCar* c;
    if (!tdr_valid(R, me) || !tdr_valid(R, other)) return;
    c = &R->car[me];
    if (c->race_state == 3 || c->crashed) return;
    c->contact_stamp[R->car[other].grid] = clock;
}

void b3_td_contact(B3TdRules* R, float clock, float dt, int a, int b,
                   int touching)
{
    if (!tdr_valid(R, a) || !tdr_valid(R, b) || a == b) return;
    if (touching) {
        tdr_contact_stamp(R, clock, a, b);
        tdr_contact_stamp(R, clock, b, a);
        /* [S] accumulation mirroring FUN_00194A80's per-opponent timer */
        R->car[a].contact_time[R->car[b].grid] += dt;
        R->car[b].contact_time[R->car[a].grid] += dt;
    } else {
        R->car[a].contact_time[R->car[b].grid] = 0.0f;
        R->car[b].contact_time[R->car[a].grid] = 0.0f;
    }
}

/* ---------------------------------------------------------------------------
 * FUN_001989A0 -- the slam handler.  NO CRASH CALL EXISTS IN IT.
 *
 * Reproduced here: exactly the fields that drive the trigger flow.  The
 * boost transfer and the Slam/Super-Slam BP tables (0x003F7448 / 0x003F7458,
 * selected by FUN_00197F90 and the victim's crashed/70 mph test) are [S] in
 * docs/RE_GAMEPLAY.md 8 and deliberately left to the score module.
 * ------------------------------------------------------------------------- */
static void tdr_apply_slam(B3TdRules* R, float clock, int attacker, int victim,
                           float strength, int type)
{
    B3TdCar* A = &R->car[attacker];
    B3TdCar* V = &R->car[victim];
    (void)strength;

    /* attacker side (iVar2 in the decompile, param_2's racecar) */
    A->slams_made++;                       /* +0x1174 @0x00198C2F */
    A->last_slam_time = clock;             /* +0x158C @0x00198C43 */
    A->aggressor = victim;                 /* +0x16BC @0x00198CE2 */
    A->aggressor_time = clock;             /* +0x16C0 @0x00198CDB */

    /* victim side (iVar3, param_1's racecar) */
    V->times_slammed++;                    /* +0x1590 */
    V->slam_time = clock;                  /* +0x1598 -- THE OOC STAMP */
    V->slam_type = type;                   /* +0x159C */
    V->aggressor = attacker;               /* +0x16BC */
    V->aggressor_time = clock;             /* +0x16C0 */
}

/* FUN_00197BE0 -- the kind 5/6 gate in front of FUN_001989A0.
 * EDI = victim pv, ECX = attacker pv, EAX = type byte, [esp+4] = strength. */
static int tdr_slam_full(B3TdRules* R, float clock, int attacker, int victim,
                         float strength, int type)
{
    B3TdCar* A = &R->car[attacker];
    B3TdCar* V = &R->car[victim];
    int va, aa;

    if (A->race_state == 3 || V->race_state == 3) return 0;   /* 0x00197BFE */

    /* who each car's recent aggressor is, with a 1.0 s memory (0x003B168C) */
    aa = (A->aggressor_time != -1.0f && clock <= A->aggressor_time
                                              + B3_TDR_RESLAM_COOLDOWN_S)
         ? A->aggressor : -1;
    va = (V->aggressor_time != -1.0f && clock <= V->aggressor_time
                                              + B3_TDR_RESLAM_COOLDOWN_S)
         ? V->aggressor : -1;
    /* 0x00197C92/0x00197C96: reject when the two already traded an
     * aggressive contact inside that window (either direction). */
    if (va == attacker || aa == victim) return 0;

    tdr_apply_slam(R, clock, attacker, victim, strength, type);
    return 1;
}

/* FUN_00197D20 -- the kind 3/4 LIGHT slam path.  It never touches +0x1598,
 * so a light slam does not put the victim out of control; it only pays the
 * light-slam BP through FUN_001987A0 [S, not modelled here]. */
static int tdr_slam_light(B3TdRules* R, float clock, int attacker, int victim,
                          float strength, int flag)
{
    B3TdCar* A = &R->car[attacker];
    B3TdCar* V = &R->car[victim];
    int va;
    (void)strength; (void)flag;

    if (A->race_state == 3 || V->race_state == 3) return 0;   /* 0x00197D3x */

    va = (V->aggressor_time != -1.0f && clock <= V->aggressor_time
                                              + B3_TDR_RESLAM_COOLDOWN_S)
         ? V->aggressor : -1;
    if (va == attacker) {
        /* 0x00197D9x: the attacker's OTHER out-of-control clock (+0x1690, the
         * wall/spin event) and FUN_001981D0's 1.0 s test on it both veto a
         * repeat.  This harness produces no +0x1690 event, so with that clock
         * permanently idle the retail code falls through -- but FUN_001981D0
         * is executed by the oracle, so the case is still differential. */
        if (A->slam_time >= 0.0f
            && clock <= A->slam_time + B3_TDR_RESLAM_COOLDOWN_S) return 0;
    }
    /* attacker last-slam rate limit +0x158C (0x00197DE4):
     *   (last < 0) || (last + 1.0 < clock) */
    if (!(A->last_slam_time < 0.0f
          || A->last_slam_time + B3_TDR_RESLAM_COOLDOWN_S < clock)) return 0;
    /* FUN_00198190 @0x00197E12: the attacker must not itself be inside its
     * own out-of-control window (racecar+0x1598 + 1.0). */
    if (A->slam_time >= 0.0f
        && clock <= A->slam_time + B3_TDR_RESLAM_COOLDOWN_S) return 0;
    return 1;
}

/* FUN_00197EA0 -- kind 2, "shunted into the scenery by car N".
 * EAX = victim racecar, ECX = attacker racecar. */
static int tdr_wall_shunt(B3TdRules* R, float clock, int attacker, int victim)
{
    B3TdCar* A = &R->car[attacker];
    B3TdCar* V = &R->car[victim];

    if (A->race_state == 3 || V->race_state == 3) return 0;
    if (A->speed_ms * B3_TDR_MPH < B3_TDR_WALL_SHUNT_MIN_MPH) return 0;
    if (V->speed_ms * B3_TDR_MPH < B3_TDR_WALL_SHUNT_MIN_MPH) return 0;

    A->shunt_victim_time = clock;   /* +0x16B0 = victim, +0x16B4 = 1 */
    /* the victim becomes attributable WITHOUT an out-of-control stamp */
    V->aggressor = attacker;        /* +0x16BC */
    V->aggressor_time = clock;      /* +0x16C0 */
    return 1;
}

int b3_td_slam_report(B3TdRules* R, float clock, int kind,
                      int attacker, int victim, float strength)
{
    if (!tdr_valid(R, attacker) || !tdr_valid(R, victim) || attacker == victim)
        return 0;
    switch (kind) {                                /* FUN_00029F30 @0x00029F40 */
    case B3_TDR_KIND_RUB:
        tdr_contact_stamp(R, clock, attacker, victim);
        tdr_contact_stamp(R, clock, victim, attacker);
        /* 0x00029F7C returns `al = low byte of the strength argument`; the
         * one caller (FUN_001121F0 @0x0011297C) passes 1.0 -> 0. */
        return (int)(((unsigned)0) & 0xFF);
    case B3_TDR_KIND_WALL_SHUNT:
        return tdr_wall_shunt(R, clock, attacker, victim);
    case B3_TDR_KIND_SIDE_LIGHT:
        return tdr_slam_light(R, clock, attacker, victim, strength, 0);
    case B3_TDR_KIND_REAR_LIGHT:
        return tdr_slam_light(R, clock, attacker, victim, strength, 1);
    case B3_TDR_KIND_SIDE:
        return tdr_slam_full(R, clock, attacker, victim, strength, 0);
    case B3_TDR_KIND_REAR:
        return tdr_slam_full(R, clock, attacker, victim, strength, 1);
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------------
 * FUN_00197920 -- game-context vtable +0x54 (FUN_00026A70 / FUN_000273D0).
 * Arms DENIED + LUCKY ESCAPE.
 * ------------------------------------------------------------------------- */
void b3_td_contact_notify(B3TdRules* R, float clock, int slot)
{
    B3TdCar* c;
    if (!tdr_valid(R, slot)) return;
    c = &R->car[slot];
    if (c->race_state == 3) return;                     /* 0x00197920 */
    if (c->crashed) return;                             /* 0x0019793A */
    if (c->aggressor_time == -1.0f) return;             /* 0x0019794A */
    if (clock > c->aggressor_time + B3_TDR_MAX_CRASH_WAIT_S) return; /*0x00197957*/
    if (c->aggressor < 0) return;                       /* 0x00197969 */
    c->denied_pending = 1;                              /* score+0x5E5 */
    c->lucky_pending = 1;                               /* score+0x5E6 */
    c->denied_time = clock;                             /* score+0x5E8 */
    /* the near-miss slot bookkeeping at score+0x3E8/+0x3F0/+0x410 that the
     * same function does belongs to the score module [S]. */
}

/* ---------------------------------------------------------------------------
 * FUN_00197750 + FUN_00197430 -- the crash notification and the attribution
 * stamps.  Reached through game-context vtable +0x48 (FUN_00027CC0).
 * ------------------------------------------------------------------------- */
void b3_td_on_crash(B3TdRules* R, float clock, int slot,
                    const B3TdCause* cause, const float (*pos)[3])
{
    B3TdCar* me;
    int i, mygrid;
    const float r2 = B3_TDR_ATTRIB_RADIUS_M * B3_TDR_ATTRIB_RADIUS_M; /*0x003B1944*/

    if (!tdr_valid(R, slot)) return;
    me = &R->car[slot];
    if (me->race_state == 3) return;                   /* FUN_00197750 @0x00197774 */

    if (cause) me->cause = *cause;                     /* score+0x308 @0x00197784 */
    else b3_td_cause_none(&me->cause);

    mygrid = me->grid;

    /* --- FUN_00197430 body ------------------------------------------------ */
    for (i = 0; i < R->ncars; i++) {
        B3TdCar* o = &R->car[i];
        int eligible;
        if (i == slot) continue;      /* contact_time[self] is never grown and
                                       * psyche_target never names our own grid,
                                       * so the retail loop cannot select self */
        /* 0x00197468: `local_18 = score+0x528` walks the CRASHING car's own
         * per-opponent contact timers -- score+0x528[i] > Min Collide Time to
         * enable TD (0x003F740C). */
        eligible = (me->contact_time[o->grid] > B3_TDR_MIN_COLLIDE_TD_S);
        /* ... or I am psyche-armed and that car is stalking my slot */
        if (!eligible && me->psyche_armed && o->psyche_target == mygrid)
            eligible = 1;
        if (!eligible) continue;
        if (pos && tdr_dist2(pos[i], pos[slot]) >= r2) continue;
        if (o->race_state == 3) continue;
        if (o->crashed) continue;                      /* their +0x18FA */
        o->claim[mygrid] = clock;                      /* +0x15A8[my slot] */
        o->claim_aftertouch[mygrid] = 0;               /* +0x15C0[]        */
        o->claim_psyche[mygrid] = me->psyche_armed;    /* +0x15C6[]        */
    }

    /* the car that slammed me inside Maximum Crash Wait Time (0x003F7404) */
    if (me->aggressor_time != -1.0f
        && clock <= me->aggressor_time + B3_TDR_MAX_CRASH_WAIT_S
        && me->aggressor >= 0 && me->aggressor < R->ncars) {
        B3TdCar* a = &R->car[me->aggressor];
        if ((!pos || tdr_dist2(pos[me->aggressor], pos[slot]) < r2)
            && a->race_state != 3 && !a->crashed) {
            a->claim[mygrid] = clock;
            a->claim_aftertouch[mygrid] = 0;
            a->claim_psyche[mygrid] = 0;
        }
    }

    /* the crash-cause record's +0x0C: the WRECK that hit me.  Requires that
     * car to be crashed and class 0 (a human), plus the per-player
     * aftertouch-enabled byte at DAT_00667E90+player*0x4AD0+0x4AC5 [S -- this
     * harness treats aftertouch as enabled]. */
    if (me->cause.present && me->cause.by_wreck >= 0
        && me->cause.by_wreck < R->ncars) {
        B3TdCar* w = &R->car[me->cause.by_wreck];
        if (w->crashed && w->cls == 0 && w->race_state != 3) {
            w->claim[mygrid] = clock;
            w->claim_aftertouch[mygrid] = 1;           /* AFTERTOUCH */
            w->claim_psyche[mygrid] = 0;
        }
    }

    /* FUN_00197750's own bookkeeping that the trigger flow depends on */
    me->crashed = 1;                                   /* the caller's +0x18FA */
    me->crash_time = clock;                            /* score+0x33C = +0x140C */
}

/* ---------------------------------------------------------------------------
 * FUN_001994D0 -- message selection + BP.
 * ------------------------------------------------------------------------- */
static int tdr_vclass_message(int vclass)
{
    switch (vclass) {                        /* 0x0019958C jump table */
    case B3_TDR_VCLASS_CAR:      return 0x94;
    case B3_TDR_VCLASS_VAN:      return 0x95;
    case B3_TDR_VCLASS_TRUCK:    return 0x96;
    case B3_TDR_VCLASS_BIGRIG:   return 0x97;
    case B3_TDR_VCLASS_BUS:      return 0x98;
    case B3_TDR_VCLASS_LTRAIN:   return 0x99;
    case B3_TDR_VCLASS_TRAM:     return 0x9A;
    case B3_TDR_VCLASS_MONORAIL: return 0x9B;
    case B3_TDR_VCLASS_TRAILER:  return 0x9C;
    default:                     return B3_TDR_MSG_TAKEDOWN;
    }
}

int b3_td_select_message(const B3TdRules* R, int attacker, int victim,
                         int aftertouch, int psyche)
{
    const B3TdCar* V;
    int msg = B3_TDR_MSG_TAKEDOWN;
    if (!tdr_valid(R, attacker) || !tdr_valid(R, victim))
        return B3_TDR_MSG_TAKEDOWN;
    V = &R->car[victim];

    if (aftertouch) {
        /* DAT_003A4B38 = {0xAA,0xAB,0xAC,0xAD,0xAE}, index min(count,4) */
        int n = R->car[attacker].aftertouch_count;
        if (n > 4) n = 4;
        return B3_TDR_MSG_AFTERTOUCH0 + n;
    }
    /* the cause record is only read when the victim is actually crashed
     * (FUN_00198E60 @0x00198E60: `if (edi+0x18FA) ebx = edi+0x13D8`) */
    if (V->crashed && V->cause.present) {
        if (!V->cause.wall) {
            if (V->cause.has_obj) msg = tdr_vclass_message(V->cause.obj_class);
        } else {
            msg = B3_TDR_MSG_WALL;
        }
    }
    if (psyche) msg = B3_TDR_MSG_PSYCHE_OUT;   /* 0x001997F9, unless signature */
    return msg;
}

/* the commit's BP + ladder posts; returns the winning message id (the
 * priority rule in FUN_00199350 is "the id IS the priority"). */
static int tdr_award(B3TdRules* R, float clock, int attacker, int victim,
                     int aftertouch, int psyche, B3TdEvent* ev)
{
    B3TdCar* A = &R->car[attacker];
    B3TdCar* V = &R->car[victim];
    int msg, bp, best, revenge, idx, n;

    revenge = A->taken_down_by[V->grid] ? 1 : 0;     /* slot+0x06 snapshot */
    msg = b3_td_select_message(R, attacker, victim, aftertouch, psyche);
    best = msg;
    ev->extra_message = -1;
    ev->extra_bp = 0;

    if (aftertouch) {
        bp = B3_TDR_BP_AFTERTOUCH;                   /* 0x003F7478 */
        A->aftertouch_count++;                       /* slot+0x128 */
        A->aftertouch_td++;                          /* racecar+0x118C */
    } else {
        bp = (psyche) ? B3_TDR_BP_PSYCHE_OUT         /* 0x003F7474 */
                      : B3_TDR_BP_TAKEDOWN;          /* 0x003F746C */
        A->td_made++;                                /* racecar+0x1194 */

        /* double-takedown window (0x001998AA) */
        if (clock < A->dbl_window || A->dbl_window == -1.0f) {
            n = ++A->dbl_count;
            A->dbl_window = clock + B3_TDR_DOUBLE_WINDOW_S;
            if (n > 1) {
                idx = n - 2; if (idx > 4) idx = 4;
                bp += TDR_DOUBLE_BP5[idx];
                ev->extra_message = B3_TDR_MSG_DOUBLE0 + idx;
                ev->extra_bp = TDR_DOUBLE_BP5[idx];
                if (ev->extra_message > best) best = ev->extra_message;
            }
        }
        /* takedown-spree window (0x0019991F) */
        if (clock < A->spree_window || A->spree_window == -1.0f) {
            n = ++A->spree_count;
            A->spree_window = clock + B3_TDR_SPREE_WINDOW_S;
            if (n > 1) {
                idx = n - 1; if (idx > 4) idx = 4;
                bp += B3_TDR_BP_SPREE[idx - 1];
                if (B3_TDR_MSG_SPREE0 + idx - 1 > best)
                    best = B3_TDR_MSG_SPREE0 + idx - 1;
                if (ev->extra_message < 0) {
                    ev->extra_message = B3_TDR_MSG_SPREE0 + idx - 1;
                    ev->extra_bp = B3_TDR_BP_SPREE[idx - 1];
                }
            }
        }
        /* revenge (0x00199973) */
        if (revenge) {
            bp += B3_TDR_BP_REVENGE;                 /* 0x003F7470 */
            if (B3_TDR_MSG_REVENGE > best) best = B3_TDR_MSG_REVENGE;
        }
    }

    A->bp += bp;                                     /* racecar+0x111C/+0x117C */
    ev->kind = B3_TDE_TAKEDOWN;
    ev->attacker = attacker;
    ev->victim = victim;
    ev->message = best;
    ev->bp = bp;
    ev->owner = attacker;
    ev->aftertouch = aftertouch;
    ev->revenge = revenge;
    return best;
}

/* ---------------------------------------------------------------------------
 * FUN_00198E60 -- the commit.  ESI = attacker score, EDI = victim racecar.
 * ------------------------------------------------------------------------- */
static int tdr_commit(B3TdRules* R, float clock, int attacker, int victim,
                      B3TdEvent* ev)
{
    B3TdCar* A = &R->car[attacker];
    B3TdCar* V = &R->car[victim];
    int aftertouch, psyche;

    if (V->td_credited) return 0;                    /* 0x00198E73 dedup */
    V->td_credited = 1;                              /* +0x15D6 / +0x15D7 */
    V->td_by = attacker;                             /* +0x15DC */
    A->last_td_time = clock;                         /* score+0x500 */
    A->td_count++;                                   /* score+0x68 */
    A->last_victim = victim;                         /* score+0x4D4 = +0x15A4 */

    aftertouch = A->claim_aftertouch[V->grid];       /* score+0x4F0[victim] */
    psyche     = A->claim_psyche[V->grid];           /* score+0x4F6[victim] */
    tdr_award(R, clock, attacker, victim, aftertouch, psyche, ev);

    /* revenge bookkeeping (0x00198F28) */
    if (A->taken_down_by[V->grid]) {
        A->taken_down_by[V->grid] = 0;
        V->revenge_flag = 1;                         /* victim +0x168F */
    } else {
        V->taken_down_by[A->grid] = 1;               /* victim +0x1689[me] */
    }
    if (V->cls == 1) V->recover_at = clock + 5.0f;   /* +0x240C */
    return 1;
}

/* ---------------------------------------------------------------------------
 * per-frame: FUN_00199080 expiry, FUN_00195CE0 denied, FUN_00197040 claims
 * ------------------------------------------------------------------------- */
int b3_td_frame(B3TdRules* R, float clock, int slot,
                B3TdEvent* out, int max_out)
{
    B3TdCar* c;
    int i, n = 0;
    if (!tdr_valid(R, slot)) return 0;
    c = &R->car[slot];

    /* --- FUN_00199080: expire the double / spree windows ------------------ */
    if (c->dbl_window != -1.0f && clock > c->dbl_window) {
        c->dbl_count = 0; c->dbl_window = -1.0f;
    }
    if (c->spree_window != -1.0f && clock > c->spree_window) {
        c->spree_count = 0; c->spree_window = -1.0f;
    }

    /* --- FUN_00195CE0: TAKEDOWN DENIED / LUCKY ESCAPE -------------------- */
    if (c->denied_pending
        && c->aggressor_time != -1.0f
        && clock <= c->aggressor_time + B3_TDR_MAX_CRASH_WAIT_S
        && c->aggressor >= 0) {
        if (c->lucky_pending && n < max_out) {
            /* FUN_00199CA0: msg 0x36 into MY callout slot, +15 BP to me */
            B3TdEvent* e = &out[n++];
            memset(e, 0, sizeof(*e));
            e->kind = B3_TDE_LUCKY;
            e->attacker = c->aggressor;
            e->victim = slot;
            e->owner = slot;
            e->message = B3_TDR_MSG_LUCKY;
            e->bp = B3_TDR_BP_LUCKY;
            e->extra_message = -1;
            c->bp += B3_TDR_BP_LUCKY;
        }
        if (n < max_out) {
            /* FUN_00199BE0: msg 0x35 into the ATTACKER's callout slot,
             * +10 BP into MY total (0x00195D9F writes EDI's score). */
            B3TdEvent* e = &out[n++];
            memset(e, 0, sizeof(*e));
            e->kind = B3_TDE_DENIED;
            e->attacker = c->aggressor;
            e->victim = slot;
            e->owner = slot;
            e->message = B3_TDR_MSG_DENIED;
            e->bp = B3_TDR_BP_DENIED;
            e->extra_message = -1;
            c->bp += B3_TDR_BP_DENIED;
        }
        c->denied_pending = 0;
        c->lucky_pending = 0;
    }

    /* --- FUN_00197040: the claim scan ------------------------------------ */
    for (i = 0; i < R->ncars; i++) {
        float t = c->claim[i];
        if (t < 0.0f) continue;
        if (!(t + B3_TDR_CLEAR_WAIT_S <= clock || c->claim_force[i])) continue;
        if (!c->crashed || c->claim_force[i]) {
            int victim = -1, k;
            for (k = 0; k < R->ncars; k++)
                if (R->car[k].grid == i) { victim = k; break; }
            if (victim >= 0 && victim != slot && n < max_out) {
                B3TdEvent ev;
                memset(&ev, 0, sizeof(ev));
                if (tdr_commit(R, clock, slot, victim, &ev)) out[n++] = ev;
            }
        }
        c->claim[i] = TDR_CLAIM_IDLE;
        c->claim_force[i] = 0;
        c->claim_psyche[i] = 0;
    }
    /* 0x0019715A: forget the last victim once they are no longer crashed */
    if (c->last_victim >= 0 && c->last_victim < R->ncars
        && !R->car[c->last_victim].crashed)
        c->last_victim = -1;
    return n;
}

/* ---------------------------------------------------------------------------
 * the out-of-control predicate (FUN_0011ECF0 head / FUN_00105340)
 * ------------------------------------------------------------------------- */
int b3_td_out_of_control(const B3TdRules* R, int slot, float clock)
{
    const B3TdCar* c;
    if (!tdr_valid(R, slot)) return 0;
    c = &R->car[slot];
    if (c->slam_time < 0.0f) return 0;
    return clock <= c->slam_time + B3_TDR_TOTAL_OOC_S;
}

/* ---------------------------------------------------------------------------
 * the crash-threshold authority veh+0x1534 -- FUN_00105340 @0x0010563C
 * (normal race, writes [0x003A69BC] = 0.05) and @0x0010574A (game context
 * +0x23F8 == 2, writes [0x003A69C4] = 0.1).  [C-disasm]
 *
 * The two blocks are the same six-condition test; only the stored constant
 * differs.  Conditions in the retail order, with the compare direction the
 * instruction bytes carry (every one is a JA / JNZ / JZ to the common
 * bail at 0x001057BA, so ANY failure leaves the value alone -- and the
 * value it is left at is the 1.0 a human at the wheel wrote):
 *
 *   0x0010563C  COMISS XMM3(0.0), [ooc+0x1598]  JA  bail   -> +0x1598 >= 0
 *   0x0010565F  COMISS now, +0x1598 + v+0x13E4  JA  bail
 *   0x00105679  UCOMISS +0x16C0, -1.0           JNP bail   -> != -1
 *   0x0010569A  COMISS now, +0x16C0 + v+0x13E4  JA  bail
 *   0x001056AB  TEST attacker, attacker         JZ  bail   -> != NULL
 *   0x001056B1  CMP  attacker+0x1920, 0         JNZ bail   -> human player
 *
 * The window v+0x13E4 is the per-car vdb "Total Out-Of-Control Time (s)"
 * (config +0x1C0, FUN_00134710 @0x00134A88), retail value 1.0.
 *
 * [S] on one indirection: the OOC stamp is read through *(racecar+0x1198)
 * while the aggressor stamps are read off the racecar itself.  This harness
 * carries one B3TdCar per car and reads both off it.
 * ------------------------------------------------------------------------- */
/* FUN_00105340's six-condition slam test, @0x0010563C / @0x0010574A. */
static int tdr_slam_override(const B3TdRules* R, int slot, float clock)
{
    const B3TdCar* c = &R->car[slot];
    if (!(c->slam_time >= 0.0f))                            return 0;
    if (!(clock <= c->slam_time + B3_TDR_TOTAL_OOC_S))      return 0;
    if (c->aggressor_time == -1.0f)                         return 0;
    if (!(clock <= c->aggressor_time + B3_TDR_TOTAL_OOC_S)) return 0;
    if (c->aggressor < 0 || c->aggressor >= R->ncars
        || c->aggressor >= B3_TDR_MAX_CARS)                 return 0;
    if (R->car[c->aggressor].cls != 0)                      return 0;
    return 1;
}

void b3_td_crash_authority_full(const B3TdRules* R, int slot, float clock,
                                B3TdAuthority* out)
{
    const B3TdCar* c;
    float frac, base, t_near, t_far, d2;

    if (!out) return;
    out->value = B3_TDR_AUTHORITY_FULL;
    out->crash_ok = 1;
    out->in_range = 1;
    out->slammed = 0;
    if (!tdr_valid(R, slot)) return;
    c = &R->car[slot];

    /* ---- (a) FUN_00105BD0's base value ---------------------------------- */
    if (c->cls == 0) {
        /* the human early-out @0x00105D6E: ladder fraction left at 1.0,
         * cVar6 = 1.  (racecar+0x27D9, the second half of that gate, is not
         * modelled -- this harness always has a human at the wheel.) */
        frac = 1.0f;
        out->in_range = 1;
        out->crash_ok = 1;
    } else {
        d2 = c->view_dist2;
        if (d2 < 0.0f) {                    /* caller has no view distance */
            frac = 1.0f;
        } else {
            base = c->view_radius_alt ? B3_TDR_VIEW_RADIUS2_ALT
                                      : B3_TDR_VIEW_RADIUS2;
            if (d2 > base) {                /* @0x00106036 COMISS/JA */
                out->in_range = 0;
                frac = 0.0f;
            } else {
                t_near = B3_TDR_VIEW_BAND_NEAR * base;
                t_far  = B3_TDR_VIEW_BAND_FAR  * base;
                if (d2 <= t_far) {          /* sel <= 1 @0x001060E1 */
                    float f = (d2 - t_near) / (t_far - t_near);
                    if (f < 0.0f) f = 0.0f;
                    if (f > 1.0f) f = 1.0f;
                    frac = 1.0f - f;
                } else {
                    frac = 0.0f;            /* out[1] left at its zero init */
                }
            }
        }
        /* cVar6 @0x00106133: the ladder enables the crash entries only while
         * the fraction is strictly positive. */
        out->crash_ok = (frac > 0.0f);
    }
    /* @0x00105EA6: an armed psyche-out forces the crash entries back on. */
    if (c->psyche_armed) out->crash_ok = 1;

    /* @0x00105ED8 / @0x00105F2B */
    out->value = out->in_range
                 ? frac * B3_TDR_AUTHORITY_SCALE + B3_TDR_AUTHORITY_FLOOR
                 : B3_TDR_AUTHORITY_FLOOR;
    /* @0x00105F63 */
    if (c->psyche_armed) out->value = B3_TDR_AUTHORITY_FLOOR;

    /* ---- (b) FUN_00105340's slam override ------------------------------- */
    if (tdr_slam_override(R, slot, clock)) {
        out->slammed = 1;
        out->value = R->crash_mode ? B3_TDR_AUTHORITY_SLAMMED_CM
                                   : B3_TDR_AUTHORITY_SLAMMED;
    }
}

float b3_td_crash_authority(const B3TdRules* R, int slot, float clock)
{
    B3TdAuthority a;
    b3_td_crash_authority_full(R, slot, clock, &a);
    return a.value;
}

/* ---------------------------------------------------------------------------
 * world contact -> FUN_0011AEF0's wall trigger (section 9 of the header)
 * ------------------------------------------------------------------------- */
void b3_td_wall_contact(B3TdRules* R, int slot, float clock,
                        const struct B3CrashVehicle* cv,
                        const float pt[4], const float n[4], float vin)
{
    B3CrashVehicle v;
    B3CrashWallEval ev;
    B3TdWallHit* w;
    B3TdAuthority a;

    if (!tdr_valid(R, slot) || !cv || !pt || !n) return;
    w = &R->wall[slot];

    b3_td_crash_authority_full(R, slot, clock, &a);
    v = *cv;
    v.authority     = a.value;
    v.party_mode    = R->crash_mode;
    v.racecar_class = R->car[slot].cls;
    /* FUN_00105BD0 @0x00105FA0 sets veh+0x1353 |= 0x18 when the view-distance
     * ladder has the crash entries off; bit 3 is what FUN_0011AEF0
     * @0x0011B94D reads to skip the wall crash. */
    if (!a.crash_ok) v.flags1353 |= 0x08;

    {
        int ok = b3_crash_wall_eval(&v, pt, n, &ev);
        if (getenv("B3_TDWALL_TRACE"))
            fprintf(stderr, "[tdeval] car %d ok=%d fire=%d dv=%.2f/%.2f "
                    "headon=%.3f/%.3f auth=%.2f surf=0x%X flags=0x%X\n",
                    slot, ok, ev.fire, ev.dv, ev.dv_thr, ev.headon,
                    ev.headon_thr, v.authority, v.surface, v.flags1353);
        if (!ok) return;
    }

    /* The real response runs ONCE per frame over the aggregate of every
     * contacting poly.  A caller that reports contacts one at a time keeps
     * the strongest, which is the one the aggregate is dominated by. */
    if (w->valid && ev.dv <= w->dv) return;

    w->valid      = 1;
    w->fire       = ev.fire;
    w->dv         = ev.dv;
    w->headon     = ev.headon;
    w->impact     = ev.impact;
    w->dv_thr     = ev.dv_thr;
    w->headon_thr = ev.headon_thr;
    w->authority  = v.authority;
    w->vin        = vin;
    w->normal[0]  = n[0]; w->normal[1] = n[1]; w->normal[2] = n[2];
}

int b3_td_wall_take(B3TdRules* R, int slot, B3TdWallHit* out)
{
    B3TdWallHit w;
    if (!tdr_valid(R, slot)) {
        if (out) memset(out, 0, sizeof(*out));
        return 0;
    }
    w = R->wall[slot];
    memset(&R->wall[slot], 0, sizeof(R->wall[slot]));
    if (out) *out = w;
    return w.valid && w.fire;
}

/* ---------------------------------------------------------------------------
 * 9b. The non-crashing wall SCRAPE -- FUN_0011AEF0's chassis response.
 *     Header section 9b carries the term-by-term recovery and the addresses.
 * ------------------------------------------------------------------------- */
static float tdr_dot3v(const float a[4], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

int b3_td_wall_response(const struct B3CrashVehicle* cvin, const float pt[4],
                        const float n[4], B3TdWallResponse* out)
{
    B3CrashVehicle v;
    B3CrashWallEval ev;
    float scrub = 1.0f, headon, du, d2[4], impv[4], l;
    int k;

    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!cvin || !pt || !n) return 0;

    v = *cvin;
    /* The crash module's own evaluator gives us step 1's flattened normal,
     * step 4's point impulse and step 5's magnitude, computed in the retail
     * order over the SCRUBBED velocity, without touching `v`. */
    if (!b3_crash_wall_eval(&v, pt, n, &ev)) return 0;
    out->valid  = 1;
    out->impact = ev.impact;
    for (k = 0; k < 4; k++) out->nh[k] = ev.nh[k];

    /* Steps 2 and 3, re-derived here because the crash module keeps them
     * inside its private core.  headon is |dot(n_flat, at)| -- the same
     * quantity b3_crash_wall_eval reports.  @0x0011B55A / @0x0011B5E5. */
    headon = ev.headon;
    if (0.707f < headon)                      /* [0x003B1A20], [0x003A69C4] */
        scrub *= 1.0f - (headon - 0.707f) * 0.1f;
    if (v.is_class0)                          /* class-0: veh+0x13A8        */
        scrub *= v.surface_grip;
    else if (!v.no_scrub)                     /* [0x003B1758] @0x0011B6EB   */
        scrub *= 0.99f;
    out->scrub = scrub;
    for (k = 0; k < 4; k++) out->vel[k] = v.vel[k] * scrub;

    if (!(ev.impact > 0.0f)) return 1;        /* dv <= 0 [0x003B16E0]       */

    /* Step 6 @0x0011B88E: strip the up-row component, renormalise. */
    du = tdr_dot3v(v.frame[1], ev.nh);
    for (k = 0; k < 4; k++) d2[k] = ev.nh[k] - v.frame[1][k] * du;
    l = (float)sqrt((double)(d2[0]*d2[0] + d2[1]*d2[1] + d2[2]*d2[2]));
    if (!(l > 1e-12f)) return 1;
    for (k = 0; k < 4; k++) d2[k] /= l;
    for (k = 0; k < 4; k++) impv[k] = d2[k] * ev.impact;

    /* Step 7 @0x0011B8A2.  The accumulators start from zero so what the
     * caller reads back IS the delta this contact contributed. */
    for (k = 0; k < 4; k++) { v.imp[k] = 0.0f; v.ang_imp[k] = 0.0f; }
    if (v.ground_frac > 0.1f) {               /* braking: linear only       */
        for (k = 0; k < 4; k++) v.imp[k] += impv[k];
        out->at_point = 0;
    } else {                                  /* FUN_001206D0 routing       */
        b3_crash_apply_impulse(&v, impv, pt);
        out->at_point = (v.ang_imp[0] != 0.0f || v.ang_imp[1] != 0.0f
                         || v.ang_imp[2] != 0.0f);
    }
    for (k = 0; k < 4; k++) {
        out->imp[k]     = v.imp[k];
        out->ang_imp[k] = v.ang_imp[k];
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * 10. The object / prop crash trigger -- FUN_00112E70.  Header section 10
 *     carries the caller chain, the class table and every address.
 * ------------------------------------------------------------------------- */

/* DAT_0039AE50, read out of build/burnout3.elf: 49 bytes, all zero except
 * row 0 (B is a racecar) and row 2 (B is a type-3 object).  Indexed
 * [classB * 7 + classA] @0x0011304E. */
static const unsigned char TDR_OBJ_CRASHABLE[7][7] = {
    /* B class 0 */ {1, 1, 1, 1, 1, 1, 0},
    /* B class 1 */ {0, 0, 0, 0, 0, 0, 0},
    /* B class 2 */ {1, 0, 0, 1, 0, 0, 0},
    /* B class 3 */ {0, 0, 0, 0, 0, 0, 0},
    /* B class 4 */ {0, 0, 0, 0, 0, 0, 0},
    /* B class 5 */ {0, 0, 0, 0, 0, 0, 0},
    /* B class 6 */ {0, 0, 0, 0, 0, 0, 0},
};

int b3_td_object_class(int handle_type, int designated_traffic)
{
    switch (handle_type) {                     /* FUN_0010FBC0 */
    case 0: case 2: return 0;
    case 1:         return 1;
    case 3:         return 2;
    case 4:         return designated_traffic ? 3 : 5;
    default:        return 6;
    }
}

int b3_td_object_crashable(int class_b, int class_a)
{
    if (class_b < 0 || class_b > 6 || class_a < 0 || class_a > 6) return 0;
    return TDR_OBJ_CRASHABLE[class_b][class_a];
}

int b3_td_object_contact(B3TdRules* R, int slot, float clock,
                         const float vrel[3], const float n[3],
                         float car_mass, int obj_class, int car_class,
                         int immune)
{
    B3TdAuthority a;
    B3TdObjectHit* o;
    float closing, mag, thr;
    const float mass = car_mass;

    if (!tdr_valid(R, slot) || !vrel || !n) return 0;
    o = &R->obj[slot];

    b3_td_crash_authority_full(R, slot, clock, &a);

    /* @0x0011303B..0x0011304E DAT_0039AE50[objclass*7 + carclass].
     * @0x0011329A `TEST byte [EBX+0x1353], 0x10` -- the ladder's |= 0x18
     * store sets bit 4 with bit 3, so crash_ok covers it.
     * @0x001132A7 `COMISS XMM3(0.0), [EBX+0x152C]; JBE` -- proceed only
     * while veh+0x152C is strictly negative. */
    if (!b3_td_object_crashable(obj_class, car_class)) return 0;
    if (!a.crash_ok || immune) return 0;

    if (R->crash_mode) {
        /* crash party @0x00113393: FUN_00013C10 = the LENGTH of v_rel */
        mag = (float)sqrt((double)(vrel[0]*vrel[0] + vrel[1]*vrel[1]
                                   + vrel[2]*vrel[2]));
        thr = a.value * B3_TDR_OBJ_MPH_SCALE_CP;
    } else {
        /* normal race @0x00113331: the NORMAL component only */
        mag = vrel[0]*n[0] + vrel[1]*n[1] + vrel[2]*n[2];
        if (mag < 0.0f) mag = -mag;
        thr = a.value * B3_TDR_OBJ_MPH_SCALE;
    }
    closing = mag * B3_TDR_MPH;                          /* [0x0038994C] */

    if (o->valid && closing <= o->closing_mph) return 0;

    /* pair+0x20 @0x00113349 always uses the NORMAL-component closing speed,
     * whatever branch the gate takes. */
    {
        float dn = vrel[0]*n[0] + vrel[1]*n[1] + vrel[2]*n[2];
        if (dn < 0.0f) dn = -dn;
        o->damage = mass * 2.0f * (dn * B3_TDR_MPH) * B3_TDR_OBJ_DAMAGE_K
                    * 0.5f;
    }
    o->valid       = 1;
    o->closing_mph = closing;
    o->thr         = thr;
    o->authority   = a.value;
    o->fire        = closing > thr;                       /* @0x001133DA */
    o->normal[0]   = n[0]; o->normal[1] = n[1]; o->normal[2] = n[2];
    return o->fire;
}

int b3_td_object_take(B3TdRules* R, int slot, B3TdObjectHit* out)
{
    B3TdObjectHit o;
    if (!tdr_valid(R, slot)) {
        if (out) memset(out, 0, sizeof(*out));
        return 0;
    }
    o = R->obj[slot];
    memset(&R->obj[slot], 0, sizeof(R->obj[slot]));
    if (out) *out = o;
    return o.valid && o.fire;
}
