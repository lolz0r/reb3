#ifndef BURNOUT3_TD_RULES_H
#define BURNOUT3_TD_RULES_H
/* ===========================================================================
 * Takedown TRIGGER rules -- the retail
 *
 *     slam -> out-of-control -> (maybe) crash -> attribution -> commit
 *
 * flow, the DENIED / LUCKY ESCAPE outcomes, and the takedown message
 * selection (WALL / vehicle-class / AFTERTOUCH / REVENGE / PSYCHE OUT /
 * double / spree).  Recovered from the retail XBE; full derivation, every
 * address and every evidence mark: docs/RE_TD_RULES.md.
 *
 * THE HEADLINE FINDING [C-disasm]: **a slam never wrecks anybody.**
 * FUN_001989A0 (the slam handler behind game-context vtable +0x64) contains
 * no call to the crash entry FUN_0010DCA0 -- it only moves BP/boost and
 * stamps the victim's out-of-control clocks (racecar+0x1598) and aggressor
 * (racecar+0x16BC/+0x16C0).  The victim then either crashes on its own
 * through the NORMAL crash triggers while it is out of control, and the
 * crash is attributed back to the slammer, or it recovers -- and the
 * slammer gets TAKEDOWN DENIED instead.
 *
 * Evidence marks used in the comments:
 *   [C]  executed under Unicorn with a green case in tools/validate_td_rules.py
 *   [C-disasm] read out of the instruction bytes (addresses given), no
 *        execution case
 *   [S]  read from the decompile, self-consistent, no green case
 *   [?]  open
 *   GLUE this harness's own, not from the binary
 *
 * OWNERSHIP: this module (both files) belongs to the takedown-rules agent.
 * The harness calls only this contract.
 * ===========================================================================
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 1. Tuned parameters this module needs, with the storage address the
 *    registrar FUN_00190430 binds them to and the retail Data/vdb.xml value.
 *    (Deliberately module-scoped names: burnout3_gameplay.h carries the same
 *    numbers for the harness's other systems and both must stay in step.)
 * ------------------------------------------------------------------------- */
#define B3_TDR_MAX_CARS              8

#define B3_TDR_MAX_CRASH_WAIT_S      2.0f   /* 0x003F7404 "Maximum Crash Wait Time"      */
#define B3_TDR_MIN_COLLIDE_TD_S      0.1f   /* 0x003F740C "Min Collide Time to enable TD"*/
#define B3_TDR_CLEAR_WAIT_S          0.5f   /* 0x003F7400 "Race Car Clear Wait Time"     */
#define B3_TDR_DOUBLE_WINDOW_S       1.0f   /* 0x003F7410 "Double takedown window"       */
#define B3_TDR_SPREE_WINDOW_S       30.0f   /* 0x003F7414 "Takedown spree window"        */
#define B3_TDR_TOTAL_OOC_S           1.0f   /* config +0x1C0 -> v+0x13E4 (RE_GAMEPLAY 7) */

#define B3_TDR_BP_TAKEDOWN         150      /* 0x003F746C */
#define B3_TDR_BP_REVENGE          350      /* 0x003F7470 */
#define B3_TDR_BP_AFTERTOUCH      1250      /* 0x003F7478 */
#define B3_TDR_BP_PSYCHE_OUT       150      /* 0x003F7474 */
#define B3_TDR_BP_LUCKY             15      /* 0x003F7488 "Denied (You Were Lucky)"  */
#define B3_TDR_BP_DENIED            10      /* 0x003F748C "Denied (Takedown Denied)" */
#define B3_TDR_BP_SIGNATURE       1500      /* 0x001997E8 literal 0x5DC */

/* Double Takedown BP[4] @0x003F7508, Takedown Spree BP[4] @0x003F7518 */
extern const int B3_TDR_BP_DOUBLE[4];
extern const int B3_TDR_BP_SPREE[4];

/* attribution radius: FUN_00197430 compares squared distance against
 * 25600.0 (0x003B1944) == 160 m [C-disasm 0x00197439] */
#define B3_TDR_ATTRIB_RADIUS_M     160.0f
/* the mutual re-slam cooldown inside FUN_00197BE0 / FUN_00197D20 */
#define B3_TDR_RESLAM_COOLDOWN_S     1.0f   /* 0x003B168C */
/* both cars must exceed this to register a wall shunt (FUN_00197EA0) */
#define B3_TDR_WALL_SHUNT_MIN_MPH   40.0f
#define B3_TDR_MPH                 2.2369363f  /* 0x0038994C */

/* ---------------------------------------------------------------------------
 * 2. The slam kinds the game-context virtual +0x64 receives.
 *    FUN_00029F30 (the race-mode +0x64 entry, vtable index 25) switches on
 *    kind-1 through the jump table at 0x0002A01C:
 * ------------------------------------------------------------------------- */
#define B3_TDR_KIND_RUB          1  /* -> FUN_001979E0  contact stamps only    */
#define B3_TDR_KIND_WALL_SHUNT   2  /* -> FUN_00197EA0  aggressor stamp, >=40mph*/
#define B3_TDR_KIND_SIDE_LIGHT   3  /* -> FUN_00197D20(flag 0) light-slam BP   */
#define B3_TDR_KIND_REAR_LIGHT   4  /* -> FUN_00197D20(flag 1) light-slam BP   */
#define B3_TDR_KIND_SIDE         5  /* -> FUN_00197BE0(type 0) -> FUN_001989A0 */
#define B3_TDR_KIND_REAR         6  /* -> FUN_00197BE0(type 1) -> FUN_001989A0 */

/* ---------------------------------------------------------------------------
 * 3. Retail callout message ids (docs/RE_TAKEDOWN_FX.md 4.2)
 * ------------------------------------------------------------------------- */
#define B3_TDR_MSG_TAKEDOWN      0x93   /* "TAKEDOWN!"            */
#define B3_TDR_MSG_VEHICLE0      0x94   /* CAR..TRAILER, +class-1 */
#define B3_TDR_MSG_WALL          0x9D   /* "WALL TAKEDOWN!"       */
#define B3_TDR_MSG_SPREE0        0x9F   /* 2-IN-A-ROW..RAMPAGE!   */
#define B3_TDR_MSG_DOUBLE0       0xA3   /* DOUBLE..TOTAL          */
#define B3_TDR_MSG_REVENGE       0xA7
#define B3_TDR_MSG_PSYCHE_OUT    0xA9
#define B3_TDR_MSG_AFTERTOUCH0   0xAA   /* DAT_003A4B38[] ladder  */
#define B3_TDR_MSG_SIGNATURE0    0xAF
#define B3_TDR_MSG_DENIED        0x35   /* "TAKEDOWN DENIED!"     */
#define B3_TDR_MSG_LUCKY         0x36   /* "LUCKY ESCAPE!"        */

/* Traffic-vehicle class byte (obj+0x173) -> message, from FUN_001994D0's
 * switch at 0x0019958C..0x00199602 [C-disasm].  Values 6 and 10 have no
 * case, i.e. they fall through to plain TAKEDOWN!. */
#define B3_TDR_VCLASS_CAR        1
#define B3_TDR_VCLASS_VAN        2
#define B3_TDR_VCLASS_TRUCK      3
#define B3_TDR_VCLASS_BIGRIG     4
#define B3_TDR_VCLASS_BUS        5
#define B3_TDR_VCLASS_LTRAIN     7
#define B3_TDR_VCLASS_TRAM       8
#define B3_TDR_VCLASS_MONORAIL   9
#define B3_TDR_VCLASS_TRAILER   11

/* ---------------------------------------------------------------------------
 * 4. The crash-cause record.
 *
 * A 16-byte stack record built by whoever triggers the crash and handed to
 * FUN_0010DCA0 in EAX; FUN_0010DD20 passes it to game-context vtable +0x48
 * (FUN_00027CC0 / FUN_00024940), which stores it at
 * score+0x308 == racecar+0x13D8 (FUN_00197750 @0x00197784).  The commit
 * later reads it back: FUN_00198E60 @0x00198E6D does
 * `if (victim+0x18FA) ECX = victim+0x13D8` before calling FUN_001994D0.
 *
 *   +0x00 u8   != 0  -> WALL / scenery   (FUN_0011AEF0 @0x0011B9B3 writes 1)
 *   +0x01 u8   != 0  -> +0x08 is a hit object (FUN_00112E70 @0x00113556)
 *   +0x04 u32  surface id  (FUN_0011AEF0: (u16 obj+0x190) & 0xFF)
 *   +0x08 ptr  the object hit; its class byte is obj+0x173
 *   +0x0C ptr  a racecar -- the WRECK that hit me (FUN_00113960 @0x0011411C)
 *
 * Car-vs-car crashes pass EAX = 0 (FUN_001121F0 @0x00112881/0x00112959,
 * FUN_0011BE50 @0x0011C27F, FUN_00197260 @0x00197396) -> no record -> the
 * plain TAKEDOWN! message.
 * ------------------------------------------------------------------------- */
typedef struct B3TdCause {
    int present;        /* the record pointer was non-NULL                   */
    int wall;           /* +0x00                                             */
    int has_obj;        /* +0x01                                             */
    int surface;        /* +0x04                                             */
    int obj_class;      /* obj+0x173 through +0x08 (0 = none)                */
    int by_wreck;       /* +0x0C as a grid slot, -1 = none                   */
} B3TdCause;

void b3_td_cause_none(B3TdCause* c);
void b3_td_cause_wall(B3TdCause* c, int surface);      /* FUN_0011AEF0    */
void b3_td_cause_object(B3TdCause* c, int vclass);     /* FUN_00112E70    */
void b3_td_cause_wreck(B3TdCause* c, int wreck_slot);  /* FUN_00113960    */

/* ---------------------------------------------------------------------------
 * 5. Per-car state.  Field names carry the racecar offset they mirror; the
 *    score object is racecar+0x10D0 and the callout slot is score+0x124, so
 *    e.g. score+0x4D8[i] == racecar+0x15A8[i] and slot+0x148 == racecar.
 * ------------------------------------------------------------------------- */
typedef struct B3TdCar {
    int   cls;                  /* +0x1920  0 human, 1 AI racer, 2 other     */
    int   grid;                 /* +0x19BC                                   */
    int   race_state;           /* +0x134C  (3 = finished)                   */
    int   crashed;              /* +0x18FA                                   */
    float crash_time;           /* +0x140C  clock the crash started          */
    float speed_ms;             /* physics vehicle +0xBC (wall-shunt gate)   */

    /* aggression / out-of-control */
    float slam_time;            /* +0x1598  set ONLY by FUN_001989A0         */
    int   slam_type;            /* +0x159C                                   */
    int   aggressor;            /* +0x16BC  slot, -1 = none                  */
    float aggressor_time;       /* +0x16C0, -1 = none                        */
    int   slams_made;           /* +0x1174                                   */
    float last_slam_time;       /* +0x158C  attacker-side slam rate limit     */
    int   times_slammed;        /* +0x1590                                   */
    int   psyche_target;        /* +0x1684  the slot this car is stalking    */
    int   psyche_armed;         /* +0x1688  (score+0x5B8)                    */
    float shunt_victim_time;    /* +0x16B0/+0x16B4 (FUN_00197EA0 bookkeeping)*/

    /* attribution (score+0x4D8/+0x4F0/+0x4F6 == racecar+0x15A8/+0x15C0/+0x15C6) */
    float claim[B3_TDR_MAX_CARS];        /* +0x15A8[]  -1 = idle             */
    int   claim_aftertouch[B3_TDR_MAX_CARS];  /* +0x15C0[]                   */
    int   claim_psyche[B3_TDR_MAX_CARS];      /* +0x15C6[]                   */
    int   claim_force[B3_TDR_MAX_CARS];       /* score+0x4F0 force byte      */
    float contact_time[B3_TDR_MAX_CARS];      /* score+0x528[] contact timer */
    float contact_stamp[B3_TDR_MAX_CARS];     /* score+0x510[] last touched  */

    /* commit bookkeeping */
    int   td_credited;          /* +0x15D6 dedup: I have been taken down     */
    int   td_by;                /* +0x15DC slot of the credited attacker     */
    int   last_victim;          /* +0x15A4 (score+0x4D4)                     */
    float last_td_time;         /* score+0x500                               */
    int   td_count;             /* score+0x68                                */
    int   taken_down_by[B3_TDR_MAX_CARS];   /* +0x1689[]                     */
    int   revenge_flag;         /* +0x168F                                   */
    float recover_at;           /* +0x240C  AI recovery = clock + 5.0        */

    /* callout-slot counters (score+0x124 + ...) */
    float dbl_window;           /* +0x110, -1 = closed                       */
    int   dbl_count;            /* +0x114                                    */
    float spree_window;         /* +0x118, -1 = closed                       */
    int   spree_count;          /* +0x11C                                    */
    int   aftertouch_count;     /* +0x128                                    */
    int   aftertouch_td;        /* racecar+0x118C                            */
    int   td_made;              /* racecar+0x1194                            */

    /* DENIED / LUCKY ESCAPE (score+0x5E5/+0x5E6/+0x5E8) */
    int   denied_pending;
    int   lucky_pending;
    float denied_time;

    int   bp;                   /* racecar+0x111C running BP total           */
    B3TdCause cause;            /* +0x13D8 stored crash cause                */

    /* input to the view-distance authority ladder (section 8a): the squared
     * distance to the nearest VIEWED racecar.  -1 = unknown, which takes the
     * ladder's in-range/full-authority path. */
    float view_dist2;
    int   view_radius_alt;      /* veh+0x1550: use the 19600 base instead    */
} B3TdCar;

/* One world contact, scored by FUN_0011AEF0's own wall test -- see section 9
 * below for how it is fed and taken. */
typedef struct B3TdWallHit {
    int   valid;        /* a contact was reported this frame               */
    int   fire;         /* FUN_0011AEF0's crash decision                   */
    float dv;           /* |j| / mass at the contact point                 */
    float headon;       /* |dot(n_flat, at)|                               */
    float impact;       /* the veh+0x194 magnitude                         */
    float dv_thr;       /* authority * 27.5   (party: * 10.0)              */
    float headon_thr;   /* authority * 0.707  (party: * 0.303)             */
    float authority;    /* veh+0x1534 used for the test                    */
    float vin;          /* the caller's closing speed (reporting only)     */
    float normal[3];    /* the contact normal, as supplied                 */
} B3TdWallHit;

/* One car-vs-OBJECT contact, scored by FUN_00112E70's own test -- section 10
 * below carries the recovery and every address. */
typedef struct B3TdObjectHit {
    int   valid;
    int   fire;
    float closing_mph;  /* the magnitude the live branch compared          */
    float thr;          /* authority * 75.0  (crash party: * 20.0)         */
    float authority;    /* veh+0x1534 used for the test                    */
    float damage;       /* pair+0x20                                       */
    float normal[3];
} B3TdObjectHit;

typedef struct B3TdRules {
    B3TdCar car[B3_TDR_MAX_CARS];
    int     ncars;              /* DAT_0073A19C                              */
    int     single_player;      /* DAT_0073A1C0 == 1 (signature TDs)         */
    int     crash_mode;         /* game context +0x23F8 == 2                 */
    /* this frame's strongest world contact per car (section 9) */
    B3TdWallHit wall[B3_TDR_MAX_CARS];
    /* this frame's strongest car-vs-OBJECT contact per car (section 10) */
    B3TdObjectHit obj[B3_TDR_MAX_CARS];
} B3TdRules;

/* ---------------------------------------------------------------------------
 * 6. Events the state machine emits.
 * ------------------------------------------------------------------------- */
#define B3_TDE_TAKEDOWN   1
#define B3_TDE_DENIED     2   /* posted to the attacker's callout slot */
#define B3_TDE_LUCKY      3   /* posted to the survivor's callout slot */

typedef struct B3TdEvent {
    int kind;          /* B3_TDE_*                                        */
    int attacker;      /* slot                                            */
    int victim;        /* slot, -1 when not applicable                    */
    int message;       /* retail callout message id                       */
    int bp;            /* BP added to `owner`                             */
    int owner;         /* the slot whose BP total the award landed in     */
    int aftertouch;    /* the takedown was an aftertouch takedown         */
    int revenge;       /* the takedown was a revenge takedown             */
    int extra_message; /* second post (double/spree ladder), -1 = none    */
    int extra_bp;      /* BP already included in `bp`                     */
} B3TdEvent;

/* ---------------------------------------------------------------------------
 * 7. API
 * ------------------------------------------------------------------------- */

void b3_td_rules_init(void);                        /* legacy no-arg entry   */
void b3_td_reset(B3TdRules* R, int ncars);
void b3_td_set_car(B3TdRules* R, int slot, int cls, int grid);

/* game-context vtable +0x64 == FUN_00029F30: returns 1 when the report was
 * accepted (what FUN_001121F0 latches into pair+0x2D).  `strength` is the
 * 0..1 slam strength FUN_001121F0 computed.  NOTHING here wrecks anybody. */
int  b3_td_slam_report(B3TdRules* R, float clock, int kind,
                       int attacker, int victim, float strength);

/* FUN_001979E0's per-pair contact stamp plus the score+0x528 accumulation
 * FUN_00197430 reads.  Call once per touching pair per frame. */
void b3_td_contact(B3TdRules* R, float clock, float dt,
                   int a, int b, int touching);

/* game-context vtable +0x54 == FUN_00026A70 -> FUN_00197920: a contact/pass
 * notification.  Arms DENIED + LUCKY ESCAPE when it lands inside the
 * Maximum Crash Wait window after being slammed. */
void b3_td_contact_notify(B3TdRules* R, float clock, int slot);

/* game-context vtable +0x48 == FUN_00027CC0 -> FUN_00197750 -> FUN_00197430:
 * `slot` just crashed.  pos[] is the world position of every car (metres);
 * pass NULL to skip the 160 m radius test. */
void b3_td_on_crash(B3TdRules* R, float clock, int slot,
                    const B3TdCause* cause, const float (*pos)[3]);

/* the per-frame score update FUN_001935F0 runs for one car, in its order:
 * FUN_00199080 window expiry, FUN_00195CE0 denied award, FUN_00197040 claim
 * scan.  Returns the number of events written to out[]. */
int  b3_td_frame(B3TdRules* R, float clock, int slot,
                 B3TdEvent* out, int max_out);

/* the out-of-control predicate the steer-away envelope (FUN_0011ECF0) and
 * the AI authority write (FUN_00105340) key off. */
int  b3_td_out_of_control(const B3TdRules* R, int slot, float clock);

/* FUN_001994D0's message chooser, exposed for the validator. */
int  b3_td_select_message(const B3TdRules* R, int attacker, int victim,
                          int aftertouch, int psyche);

/* ---------------------------------------------------------------------------
 * 8. THE CRASH-THRESHOLD AUTHORITY  veh+0x1534  [C-disasm]
 *
 * This one float is the entire dynamic range of every crash trigger in the
 * game.  FUN_0011AEF0's wall test is `dv > authority*27.5 && headon >
 * authority*0.707`; FUN_00112E70's object test is `closing_mph >
 * authority*75`.  At authority 1.0 a barrier needs a 27.5 m/s head-on
 * delta-v -- you can grind walls for a whole lap and never wreck, which is
 * exactly how the retail game plays.  At 0.05 it needs 1.375 m/s at any
 * angle, so the next thing the car touches wrecks it.
 *
 * Two writers, in this per-frame order (FUN_00104A90 @0x00104A9C calls the
 * first, then FUN_0011BE50 -> FUN_00104D30 @0x00104D8D calls the second,
 * then FUN_0011BE50 stage 6 runs FUN_0011AEF0 and reads the result):
 *
 * (a) FUN_00105BD0 -- the base value, a VIEW-DISTANCE ladder.
 *
 *     The human's car takes an early-out at @0x00105D6E (racecar+0x1920 == 0
 *     and racecar+0x27D9 == 0) that leaves the ladder fraction at 1.0, so
 *     authority = 1.0 -- the same value FUN_0018CB60 @0x0018CB94 and
 *     FUN_000279C0 @0x000279F6 write when a human takes the wheel.
 *
 *     Every other car goes through FUN_00105FC0 @0x00105FC0 on d2, the
 *     squared distance to the nearest VIEWED racecar (FUN_00105BD0's loop
 *     over the racecar array DAT_0073A1D0, stride 0x27E0, keeping the
 *     nearest that passes the FUN_001AD4A0 visibility test).  base is
 *     19600 or 15625 on veh+0x1550 [0x0039A850 / 0x0039A854 -- also the
 *     runtime-tunable copies DAT_005A39E0 / DAT_005A39FC, whose reset
 *     thunks @0x002B8D80 / @0x002B8DA0 seed them from those same two
 *     image constants].  The band edges are base * DAT_0039A858[1..5] =
 *     {0.0, 0.1, 0.4, 0.5, 1.0}:
 *
 *         d2 <= 0.1*base      frac = 1      -> authority 1.0
 *         .. 0.4*base         frac ramps    -> authority 1.0 .. 0.03
 *         .. base             frac = 0      -> authority 0.03, CRASH OFF
 *         >  base             out of range  -> authority 0.03, CRASH OFF
 *
 *     "CRASH OFF" is FUN_00105BD0 @0x00105FA0: when the ladder's frac is
 *     not > 0 it sets veh+0x1353 |= 0x18, and bit 3 is the flag
 *     FUN_0011AEF0 @0x0011B94D and FUN_00112E70 @0x0011329A test to skip
 *     the crash entirely.  So the fragile cars are the ones in the MIDDLE
 *     band -- roughly 40 m to 79 m from the viewer at the retail radius --
 *     and that band is where the pack visibly wrecks itself.  The out
 *     struct FUN_00105FC0 fills is zero-initialised @0x00105BFB..0x00105C1C,
 *     which is what makes the ladder monotone.
 *
 * (b) FUN_00105340 -- the SLAM OVERRIDE, @0x0010563C (normal race, stores
 *     [0x003A69BC] = 0.05) and @0x0010574A (game context +0x23F8 == 2,
 *     stores [0x003A69C4] = 0.1).  Both blocks are the same six-condition
 *     test and every condition is required:
 *
 *     0x0010563C  COMISS 0.0, +0x1598          JA  bail -> +0x1598 >= 0
 *     0x0010565F  COMISS now, +0x1598 + 13E4   JA  bail -> inside the OOC
 *     0x00105679  UCOMISS +0x16C0, -1.0        JNP bail -> contact recorded
 *     0x0010569A  COMISS now, +0x16C0 + 13E4   JA  bail -> and it is recent
 *     0x001056AB  TEST attacker, attacker      JZ  bail -> aggressor known
 *     0x001056B1  CMP  attacker+0x1920, 0      JNZ bail -> IT IS THE HUMAN
 *
 *     The last condition is not incidental: this override exists so that
 *     the PLAYER's slams convert.  An AI that slams another AI does not get
 *     it -- AI-vs-AI takedowns come from the view-distance ladder in (a)
 *     instead.
 *
 * The window v+0x13E4 is the per-car vdb "Total Out-Of-Control Time (s)"
 * (config +0x1C0, FUN_00134710 @0x00134A88), retail value 1.0.
 *
 * [S] on two points: the OOC stamp is read through *(racecar+0x1198) while
 * the aggressor stamps come off the racecar itself (this harness carries one
 * B3TdCar per car and reads both off it), and veh+0x1553 -- a one-shot that
 * can defer the 0x18 store for one frame -- is taken as its steady-state 0.
 * ------------------------------------------------------------------------- */
#define B3_TDR_AUTHORITY_FULL        1.0f   /* 0x003B168C                  */
#define B3_TDR_AUTHORITY_SLAMMED     0.05f  /* 0x003A69BC, normal race     */
#define B3_TDR_AUTHORITY_SLAMMED_CM  0.1f   /* 0x003A69C4, ctx+0x23F8 == 2 */
#define B3_TDR_AUTHORITY_FLOOR       0.03f  /* 0x00384148 FUN_00105BD0     */
#define B3_TDR_AUTHORITY_SCALE       0.97f  /* 0x003B1A2C                  */
#define B3_TDR_VIEW_RADIUS2         15625.0f/* 0x0039A854, veh+0x1550 == 0 */
#define B3_TDR_VIEW_RADIUS2_ALT     19600.0f/* 0x0039A850, veh+0x1550 != 0 */
#define B3_TDR_VIEW_BAND_NEAR         0.1f  /* 0x0039A860                  */
#define B3_TDR_VIEW_BAND_FAR          0.4f  /* 0x0039A864                  */

typedef struct B3TdAuthority {
    float value;      /* veh+0x1534                                        */
    int   crash_ok;   /* FUN_00105BD0's cVar6: 0 => veh+0x1353 |= 0x18,
                       * i.e. the wall AND object crash entries are OFF     */
    int   in_range;   /* cVar5: the car is inside the ladder's base radius  */
    int   slammed;    /* the FUN_00105340 override fired                    */
} B3TdAuthority;

/* The whole law.  `slot`'s view distance comes from B3TdCar::view_dist2. */
void  b3_td_crash_authority_full(const B3TdRules* R, int slot, float clock,
                                 B3TdAuthority* out);
/* Just the value. */
float b3_td_crash_authority(const B3TdRules* R, int slot, float clock);

/* ---------------------------------------------------------------------------
 * 9. WORLD CONTACT -> the real wall-crash trigger.
 *
 * The harness owns the contact geometry (its world collision is a sphere
 * sweep, not the game's per-poly clip).  It reports each contact here and
 * this module runs FUN_0011AEF0's OWN decision over it -- the real impulse
 * solver at the real contact point with the real authority scale -- and
 * keeps the frame's strongest.  b3_td_wall_take then hands back the retail
 * verdict, which is what actually enters the crash.
 * ------------------------------------------------------------------------- */
struct B3CrashVehicle;   /* burnout3_crash.h */

/* Report one world contact for `slot`.  `cv` supplies the pose/dynamics
 * fields FUN_0011AEF0 reads (frame, iinv_world, vel, dir, omega, mass,
 * surface, flags1353, racecar_class, is_class0, surface_grip, no_scrub);
 * `authority` and `party_mode` are filled in from the rules state here.
 * `pt`/`n` are world (game-space) 4-vectors, `n` pointing at the car. */
void b3_td_wall_contact(B3TdRules* R, int slot, float clock,
                        const struct B3CrashVehicle* cv,
                        const float pt[4], const float n[4], float vin);

/* Take (and clear) this frame's strongest contact for `slot`.
 * Returns out->fire. */
int  b3_td_wall_take(B3TdRules* R, int slot, B3TdWallHit* out);

/* ---------------------------------------------------------------------------
 * 9b. THE NON-CRASHING WALL SCRAPE -- FUN_0011AEF0's chassis response.
 *
 * The crash decision above and the physical response are INDEPENDENT branches
 * of the same function: everything from the flattened normal down to
 * @0x0011B904 runs unconditionally on the wall path, and only the tail
 * @0x0011B909..0x0011B9A3 decides whether FUN_0010DCA0 is also called.  A
 * harness that resolves a barrier contact by killing the into-wall velocity
 * at the CENTRE OF MASS therefore loses the term that STRAIGHTENS a clipped
 * car, because the retail response resolves at the CONTACT POINT.
 *
 * The terms, in the retail order (`vel` = veh+0xB0, w = +0xBC magnitude):
 *
 *  1  @0x0011B489  deflection: veh+0x130 += n_flat * min_edge_dist * 1.5
 *                  [0x003B1870].  The harness owns its own push-out (the
 *                  sphere sweep already separates the bodies), so
 *                  b3_td_wall_response reports `defl` but the harness must
 *                  not apply it on top -- GLUE decision, documented.
 *  2  @0x0011B55A  head-on velocity scrub: headon = |dot(n_flat, at)|;
 *                  if (0.707 [0x003B1A20] < headon)
 *                      vel *= 1 - (headon - 0.707) * 0.1 [0x003A69C4]
 *  3  @0x0011B5E5  class-0 cars scrub by veh+0x13A8 (and reverse flips
 *                  veh+0x1434); every other class by 0.99 [0x003B1758]
 *                  unless veh+0x153E
 *  4  @0x0011B724  j = FUN_00106720(nh, contact point, point velocity,
 *                  restitution 0) -- the POINT velocity, omega x r included
 *  5  @0x0011B777  impact = ((1 - min(|v|,89.408)*0.011184681) * 0.9
 *                           + (1 - min(|dot(dir,n)|*1.75, 0.9)))
 *                          * mass * (j/mass) * 0.175
 *  6  @0x0011B88E  d2 = normalize(nh - up * dot(up, nh))     (the response is
 *                  deliberately HORIZONTAL: no vertical launch on a scrape)
 *  7  @0x0011B8A2  veh+0x1404 (the BRAKE input) > 0.1 [0x003A69C4]
 *                     -> veh+0x110 += d2 * impact           LINEAR ONLY
 *                  else
 *                     -> FUN_001206D0(d2 * impact, contact point), i.e. the
 *                        impulse AT THE POINT with torque, unless the car is
 *                        drifting (veh+0x1524 in {1,2}) or already yawing
 *                        past 3 rad/s the same way.  <-- the straightening
 *
 * NOT part of a static-wall scrape: the `-1000 * mass * dir` brake at
 * @0x0011B0A5..0x0011B0F7 [0x003B1744].  It is guarded by
 * @0x0011B098 `wall_count_after > wall_count_before_the_SECOND_poly_set`,
 * i.e. it fires only when the veh+0x1590 set (the OTHER-CAR hull polys,
 * flags 0x20) contributed the wall contact.  Car-vs-car, not car-vs-world.
 *
 * b3_td_wall_response reproduces 2..7 over the caller's contact using the
 * crash module's own exported solvers (b3_crash_wall_eval for the flattened
 * normal + the impact magnitude, b3_crash_apply_impulse for step 7's
 * routing); nothing on `cv` is modified.
 * ------------------------------------------------------------------------- */
typedef struct B3TdWallResponse {
    int   valid;         /* the contact resolved (non-degenerate normal)   */
    float nh[4];         /* the flattened + normalised contact normal      */
    float vel[4];        /* veh+0xB0 AFTER steps 2 and 3                   */
    float scrub;         /* the product of the two scrub factors           */
    float impact;        /* veh+0x194                                      */
    float imp[4];        /* veh+0x110 delta -- divide by mass for dv       */
    float ang_imp[4];    /* veh+0x120 delta -- add to angular momentum     */
    int   at_point;      /* 1 = step 7 took the torque path                */
} B3TdWallResponse;

int b3_td_wall_response(const struct B3CrashVehicle* cv, const float pt[4],
                        const float n[4], B3TdWallResponse* out);

/* ---------------------------------------------------------------------------
 * 10. THE OBJECT / PROP CRASH TRIGGER -- FUN_00112E70 [C-disasm]
 *
 * Recovered caller chain.  FUN_00112E70 has exactly ONE caller,
 * FUN_00111CD0 @0x00111D77 -- the collision-pair dispatcher.  A pair record
 * carries two handles (pair+0x24 / pair+0x28), each `{u8 type, ..., +0x4
 * matrix, +0x8 bbox, +0xC entity}`.  FUN_00111CD0's first branch
 * @0x00111D0B..0x00111D77 is
 *
 *     (A.type in {0,1,2,4,6,7} && B.type == 3)
 *  || (B.type in {0,1,2,4,6,7} && A.type == 3)
 *
 * with the type-3 handle SWAPPED into slot [10] (@0x00111D3E), then
 *     A.type in {0,1,2,4}  -> FUN_00112E70     <- THE OBJECT PATH
 *     else (6, 7)          -> FUN_001135E0
 * A.type in {0,1,2} vs B.type in {0,1,2} is the car-vs-car dispatch
 * (FUN_001121F0 / FUN_00113960) and never reaches here.  So:
 *
 *     FUN_00112E70 == <car-ish handle> vs a TYPE-3 (object/prop) handle.
 *
 * Which pairs may crash is the 7x7 byte table DAT_0039AE50, indexed
 * `[classB * 7 + classA]` (@0x0011303B IMUL 7, @0x0011304E), classes from
 * FUN_0010FBC0: handle type {0,2} -> 0, {1} -> 1, {3} -> 2,
 * {4} -> 3 when entity+0x242B == DAT_0073BB8C else 5, anything else -> 6.
 * The table read out of the image is all zero except
 *     row 0: 1 1 1 1 1 1 0        (B is a racecar)
 *     row 2: 1 0 0 1 0 0 0        (B is a type-3 OBJECT)
 * so against an object the crashable A classes are exactly {0, 3}:
 *     class 0 = handle type 0 or 2  -> A RACECAR
 *     class 3 = handle type 4 with entity+0x242B == DAT_0073BB8C
 *               -> THE DESIGNATED BIG-HIT TRAFFIC VEHICLE (RE_NOTES 16.3)
 * and one veto @0x00113069: a type-3 handle whose entity has
 * `+0x174 & 8` is never crashable (the same bit FUN_00026A70 reads to arm
 * the big-hit window).
 *
 * The trigger itself, @0x0011329A..0x001133E4:
 *
 *     require crashable && !(veh+0x1353 & 0x10) && veh+0x152C < 0
 *     v_rel        = point velocity of the object - point velocity of the car
 *     normal race: closing = |dot(v_rel, contact normal)| * 2.2369363
 *                            [0x0038994C]                    @0x00113331
 *                  fire if closing > veh+0x1534 * 75.0 [0x003EBE44]
 *     crash party: closing = |v_rel| * 2.2369363 (FUN_00013C10, the LENGTH,
 *                            not the normal component)       @0x00113393
 *                  fire if closing > veh+0x1534 * 20.0 [0x003EBE48]
 *     pair+0x20   = mass * 2.0 [0x003B1688] * closing_normal * 0.1
 *                   [0x003EBE74] * 0.5 [0x003B1684]          @0x00113349
 *     on fire: the push-out at 0x001134EF is SKIPPED (`[ESP+0x1B] = 0`
 *              @0x001133E4) and FUN_0010DCA0(&DAT_0064ACE8, veh, grid) runs
 *              @0x0011357E with a cause record whose +0x01 = 1 and +0x08 =
 *              the object (RE_TD_RULES 3) -- the OBJECT takedown message.
 *
 * veh+0x152C is a post-spawn immunity timer: FUN_0011FE90 @0x0011FF81 sets
 * it to 1.0 and FUN_0011BE50 @0x0011C1CB decays it by 2*dt while it is above
 * -1e-4 [0x003B1E84], so it blocks the object crash for the first 0.5 s of a
 * car's life and is -1.0 [0x003B16C0] from the vehicle init @0x0011AAB2
 * onward.  veh+0x1353 bit 4 is set together with bit 3 by the view-distance
 * ladder's `|= 0x18` @0x00105FA0, so B3TdAuthority::crash_ok gates both.
 *
 * At authority 1.0 that is 75 mph = 33.5 m/s of NORMAL closing speed with no
 * head-on requirement at all -- which is the case FUN_0011AEF0 cannot make:
 * its own gate additionally needs |dot(n, at)| > authority*0.707, so a fast
 * SIDE-ON clip of a prop wrecks you here and nowhere else.  At the 0.05
 * slammed authority it is 3.75 mph.
 *
 * HARNESS MAPPING (GLUE, and the only glue in this section): retail props are
 * separate entities; this harness's world is one triangle soup.  The object
 * class is mapped onto the soup surface types whose low byte lies in
 * FUN_0011BBE0's own STRUCTURE band 0x15..0x20 (burnout3_collision.h) -- the
 * one classification of soup types the retail code itself makes, and the band
 * that holds the barriers, the buildings and the 0x20 chevron boards that
 * FUN_0011AEF0 @0x0011B944 explicitly refuses to wall-crash.  Road (< 0x15)
 * and everything above 0x20 stay wall-only.
 * ------------------------------------------------------------------------- */
#define B3_TDR_OBJ_MPH_SCALE     75.0f   /* 0x003EBE44 normal race        */
#define B3_TDR_OBJ_MPH_SCALE_CP  20.0f   /* 0x003EBE48 crash party        */
#define B3_TDR_OBJ_DAMAGE_K       0.1f   /* 0x003EBE74                    */

/* FUN_0010FBC0 @0x0010FBC0: collision-handle type byte -> class. */
int b3_td_object_class(int handle_type, int designated_traffic);
/* DAT_0039AE50[classB * 7 + classA] -- 1 when the pair may crash. */
int b3_td_object_crashable(int class_b, int class_a);

/* ---------------------------------------------------------------------------
 * THE PROP-SYSTEM INTERFACE.
 *
 * This is the trigger a real prop entity plugs into: nothing here knows about
 * triangle soups.  A caller supplies the two RECOVERED class ids (from
 * b3_td_object_class over its own collision-handle type byte), the car's mass
 * (veh+0x1F0 -- the damage record uses the CAR's mass, not the prop's) and
 * the contact kinematics; the crashable test, the authority scale and both
 * mode thresholds are taken from the image here.
 *
 *   obj_class  the object's FUN_0010FBC0 class.  A retail prop entity is
 *              collision-handle type 3 -> class 2.
 *   car_class  the car's FUN_0010FBC0 class.  A racecar is handle type 0 or 2
 *              -> class 0; the designated big-hit traffic vehicle is handle
 *              type 4 with entity+0x242B == DAT_0073BB8C -> class 3.
 *   vrel       the OBJECT's point velocity minus the CAR's, world/game space
 *              (a static prop gives -car point velocity; a knocked-loose one
 *              gives the difference, which is what retail computes at
 *              0x00113311 from the two point velocities).
 *   n          the contact normal, world/game space.
 *   immune     veh+0x152C >= 0, the 0.5 s post-spawn object-crash immunity.
 *
 * Keeps the frame's strongest contact; returns out->fire for this one.
 * Until the prop system lands, src/burnout3_full.c's mesh_collide is the
 * interim consumer and maps the soup's structure band onto obj_class 2 (the
 * GLUE noted above); that mapping lives in the harness, not here, so prop
 * entities replace it without touching this module.
 * ------------------------------------------------------------------------- */
int  b3_td_object_contact(B3TdRules* R, int slot, float clock,
                          const float vrel[3], const float n[3],
                          float car_mass, int obj_class, int car_class,
                          int immune);

/* Take (and clear) this frame's strongest object contact.  Returns fire. */
int  b3_td_object_take(B3TdRules* R, int slot, B3TdObjectHit* out);

#ifdef __cplusplus
}
#endif
#endif /* BURNOUT3_TD_RULES_H */
