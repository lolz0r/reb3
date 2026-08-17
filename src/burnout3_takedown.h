#ifndef BURNOUT3_TAKEDOWN_H
#define BURNOUT3_TAKEDOWN_H
/* Takedown presentation FX: the game's slow-motion moment, the takedown /
 * crash camera hand-over, and the callout trigger chain.
 *
 * Everything marked [C] below is confirmed by executing the real x86 under
 * Unicorn (tools/emulate_tdfx.py) and asserted by tools/validate_takedown.py.
 * [S] = read from the instructions, no green differential case.  GLUE = this
 * harness's own assembly around the recovered pieces, not from the binary.
 * Full derivation + addresses: docs/RE_TAKEDOWN_FX.md.
 *
 * OWNERSHIP: this module (both files) belongs to the takedown-FX agent.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * 1. TIME DILATION  [C]
 *
 * The game has no float "timescale".  DAT_0060EA00 is a frame-tick timer
 * object; DAT_004A1EB4 (a raw frame counter, ++ once per rendered frame in
 * the main loop at 0x000166D1) is its input, and DAT_0049C120 is the
 * nominal frame period (1/60 NTSC / 1/50 PAL, selected at 0x00015B4F).
 *
 *   timer+0x18 = the integer time DIVISOR      (DAT_0060EA18)
 *   timer+0x1C = per-frame dt = period/divisor (DAT_0060EA1C)
 *   timer+0x20 = game clock                    (DAT_0060EA20)
 *   timer+0x24 = the REQUESTED divisor         (DAT_0060EA24)
 *
 * FUN_001B5AC0 ticks it; when +0x18 != +0x24 it calls FUN_001B5B60, which
 * applies the request and recomputes dt.  Slow motion is therefore
 * "divisor N" -> the whole simulation and clock run at 1/N speed on the
 * same frame rate.  Requested values seen in the retail code: 1 (normal),
 * 3, 4, 5, 6.
 * ====================================================================== */

#define B3_TDFX_PERIOD_NTSC   0.016666668f   /* 0x003B1838 */
#define B3_TDFX_PERIOD_PAL    0.02f          /* 0x003B1A08 */
#define B3_TDFX_PITCH_DILATED 0.75f          /* 0x003A55F8 -> DAT_003EBFD0 */
#define B3_TDFX_PITCH_NORMAL  1.0f           /* 0x003B168C */

/* the divisors the retail code requests, with their writers */
#define B3_TDFX_DIV_NORMAL      1  /* every restore site */
#define B3_TDFX_DIV_CRASHPARTY  3  /* 0x0011888E aftertouch, crash mode   */
#define B3_TDFX_DIV_CRASHBRK    4  /* 0x00118986 after a crashbreaker      */
#define B3_TDFX_DIV_AFTERTOUCH  5  /* 0x001188A4 race aftertouch (boost held),
                                    * 0x00027959 takedown cinematic,
                                    * 0x00025D5C player crash              */
#define B3_TDFX_DIV_IMPACT      6  /* 0x0002655B big-hit impact slam       */

typedef struct B3TdfxTimer {
    /* the real object's fields, same order (offsets in the comments) */
    int   last_raw;      /* +0x00 last frame-counter value seen            */
    int   prev_delta;    /* +0x04 previous (counter - base)                */
    int   paused_acc;    /* +0x08 accumulated while +0x28 == 0             */
    int   whole;         /* +0x0C accumulated whole ticks at current rate  */
    int   rem;           /* +0x10 sub-tick remainder                       */
    int   base;          /* +0x14 counter origin                           */
    int   divisor;       /* +0x18 current time divisor                     */
    float dt;            /* +0x1C per-frame dt seconds                     */
    float clock;         /* +0x20 clock seconds                           */
    int   requested;     /* +0x24 requested divisor                        */
    int   running;       /* +0x28 running flag                             */
    /* not in the game object: this port keeps its own period copy instead
     * of the DAT_0049C120 global */
    float period;
} B3TdfxTimer;

void  b3_tdfx_timer_init(B3TdfxTimer *t, float period);        /* GLUE ctor */
void  b3_tdfx_timer_rescale(B3TdfxTimer *t);                   /* FUN_001B5B60 [C] */
void  b3_tdfx_timer_tick(B3TdfxTimer *t, int frame_counter);   /* FUN_001B5AC0 [C] */

/* ======================================================================
 * 2. THE TAKEDOWN CINEMATIC  [C]
 *
 * Chain: takedown commit -> FUN_000273F0 -> FUN_000278B0 -> gate
 * FUN_00027A60 -> entry FUN_00027920; per frame FUN_00027700 runs
 * FUN_00027AD0 over 2 records at director+0x20/+0x30 (one per local
 * player); exit FUN_000279C0.
 *
 * Timeline, verified frame-by-frame under Unicorn at real dt 1/60:
 *   t = 0      divisor := 5, camera on, HUD event 9 (or 11 when the
 *              signature-takedown selector racecar+0x1320 != -1),
 *              presentation state 3
 *   t >= 2.5   HUD event 10 + presentation state 1  (0x003A2D50)
 *   t >= 2.8   divisor := 1, camera off, victim vehicle's steering
 *              authority restored, attacker vehicle +0x1353 |= 0x18
 *              (0x00395E88)
 *   t >= 4.8   exit: record cleared                 (0x00385A00)
 * The timer advances on the REAL (undilated) dt DAT_004AE1FC, so the
 * cinematic is 4.8 wall-clock seconds regardless of the dilation.
 * ====================================================================== */

/* camera record: the real 0x10-byte record at director+0x20 + i*0x10 */
typedef struct B3TdCamRecord {
    void         *victim;        /* +0x00 victim racecar                  */
    float         elapsed;       /* +0x04 seconds since entry (-1 = idle) */
    int           slot;          /* +0x08 owner racecar slot / player idx */
    unsigned char flag_c;        /* +0x0C armed flag (cleared at t>=2.5)  */
    unsigned char callout_done;  /* +0x0D "event 10 already posted"       */
    unsigned char active;        /* +0x0E cinematic running               */
    unsigned char chain;         /* +0x0F copy of racecar+0x11EE          */
} B3TdCamRecord;

/* the subset of the racecar the cinematic reads/writes (offsets from
 * DAT_0073A1D0 + slot*0x27E0) */
typedef struct B3TdVehicle {
    float         authority;     /* v+0x1534 steering authority           */
    int           drift_state;   /* v+0x1524 (4 = cinematic-owned)        */
    unsigned char flags1353;     /* v+0x1353 crash/ghost bits             */
} B3TdVehicle;

typedef struct B3TdRacecar {
    struct B3TdRacecar *victim;      /* +0x15A4 last car I took down      */
    float          clock;            /* +0x10DC race clock                */
    float          crash_stamp;      /* +0x140C crash-start stamp         */
    int            cls;              /* +0x1920 0 = human                 */
    int            race_state;       /* +0x134C 3 = finished              */
    int            signature;        /* +0x1320 signature-takedown id     */
    int            player_index;     /* +0x27D0 local player index        */
    unsigned char  crashed;          /* +0x18FA                           */
    unsigned char  grid_slot;        /* +0x19BC                           */
    unsigned char  chain_flag;       /* +0x11EE burnout-chain flag        */
    unsigned char  boost_ramp_done;  /* +0x11F1                           */
    unsigned char  cam_active;       /* +0x27D8 takedown camera engaged   */
    unsigned char  hud_flag;         /* +0x245D                           */
    unsigned char  burnout_grant;    /* +0x2417                           */
    B3TdVehicle   *pv;               /* +0x2440 physics vehicle           */
} B3TdRacecar;

/* every HUD/presentation post the cinematic makes, captured in order */
typedef struct B3TdfxPost {
    int hud_event;   /* 9 / 10 / 11, or -1 when only a state was posted   */
    int hud_arg;     /* the victim's grid slot (the real arg is &slot)    */
    int state;       /* FUN_00053D20 presentation state, or -1            */
} B3TdfxPost;

typedef struct B3TdfxPostSink {
    B3TdfxPost *posts;
    int         count;
    int         cap;
} B3TdfxPostSink;

/* 1:1 ports; all four assert-verified against the real functions */
int  b3_td_cam_gate  (const B3TdCamRecord *r, const B3TdRacecar *me);
void b3_td_cam_enter (B3TdCamRecord *r, B3TdRacecar *me,
                      B3TdfxTimer *timer, B3TdfxPostSink *sink);
void b3_td_cam_update(B3TdCamRecord *r, B3TdRacecar *me,
                      B3TdfxTimer *timer, B3TdfxPostSink *sink,
                      float real_dt);
void b3_td_cam_exit  (B3TdCamRecord *r, B3TdRacecar *me, B3TdfxTimer *timer);

/* the recovered timeline constants, exported so callers do not re-invent
 * them (addresses in docs/RE_TAKEDOWN_FX.md) */
#define B3_TDFX_CINE_TOTAL    4.8f   /* 0x00385A00 */
#define B3_TDFX_CINE_SLOWMO   2.8f   /* 0x00395E88 */
#define B3_TDFX_CINE_CALLOUT  2.5f   /* 0x003A2D50 */
#define B3_TDFX_TRIGGER_AGE   1.5f   /* 0x003B1870 max victim-crash age   */

/* the impact-hit slam (FUN_00026050 state machine, armed at 0x00026B28) */
#define B3_TDFX_IMPACT_DELAY  0.05f  /* 0x003A69BC */
#define B3_TDFX_IMPACT_LEN    0.35f  /* 0x0039B2B0 */

/* ======================================================================
 * 3. THE CALLOUT  (for the HUD agent)
 *
 * The real chain is  takedown commit -> FUN_001994D0 (BP award, which also
 * SELECTS the message) -> FUN_00199350 "PostHudCallout" -> the HUD callout
 * slot -> FUN_00054700 -> FUN_00055C90 (element build) -> FUN_00056120
 * (3-phase animation).  It runs in PARALLEL with the cinematic of section
 * 2, both starting at the commit.
 *
 * Message table: 206 x 4 bytes at 0x00389160,
 *   { u8 message id, u8 sign kind, u16 Data/Globalus.bin string index }
 * Sign kind -> descriptor via the jump table at 0x00054ACC (kind-3 index,
 * kinds outside 3..23 fall through to descriptor 0); descriptors are the
 * 16-byte records at 0x003895E8, +0x04 = anim style, +0x08 = sign index
 * (17 = none), +0x0C = flags (bit 3 = long hold).
 * All of the above verified byte-for-byte against the image and the retail
 * Globalus.bin by tools/validate_takedown.py sections 7 and 9.  [C]
 *
 * NOTE the other dispatch: FUN_00027920/FUN_00027AD0 also call
 * DAT_004CFB20[i]->vtable[+0x0C] with ids 9 / 10 / 11.  That object's
 * listener pointer (+0x04) is set to 0 by its Init (FUN_001B4170 @
 * 0x001B4191) and NO writer of a non-zero value exists in the image, so in
 * retail those posts are inert [S].  They are still reported through
 * b3_tdfx_poll_post() because they are real, ordered, and useful as the
 * cinematic's phase markers.
 * ====================================================================== */

/* sign textures: FUN_0004DD00 binds these by name into DAT_004608F0[] from
 * the pointer table at 0x00388560 */
#define B3_TDFX_SIGN_FLAME          0   /* "hud_sign_flame"    */
#define B3_TDFX_SIGN_SKULL          1   /* "hud_sign_skull"    */
#define B3_TDFX_SIGN_AWARD_STAR     2   /* "hud_award_star"    */
#define B3_TDFX_SIGN_TAKEDOWN       3   /* "hud_signs_td"      */
#define B3_TDFX_SIGN_MEDAL_GOLD     4
#define B3_TDFX_SIGN_MEDAL_SILVER   5
#define B3_TDFX_SIGN_MEDAL_BRONZE   6
#define B3_TDFX_SIGN_NONE          17   /* the 0x11 sentinel in FUN_00055C90 */

/* HUD event ids on the inert DAT_004CFB20 hook (cinematic phase markers) */
#define B3_TDFX_EVT_TAKEDOWN_START      9   /* cinematic entry, plain       */
#define B3_TDFX_EVT_TAKEDOWN_MID       10   /* cinematic t >= 2.5 s         */
#define B3_TDFX_EVT_TAKEDOWN_SIGNATURE 11   /* cinematic entry, signature   */

/* --- callout message ids (FUN_00199350's EDI) ------------------------- */
#define B3_TDFX_MSG_NONE               (-1)
#define B3_TDFX_MSG_TD_DENIED          0x35  /* "TAKEDOWN DENIED!"   2100 */
#define B3_TDFX_MSG_LUCKY_ESCAPE       0x36  /* "LUCKY ESCAPE!"      2101 */
#define B3_TDFX_MSG_CRASHED            0x7F  /* "CRASHED!"           2103 */
#define B3_TDFX_MSG_TAKEN_OUT          0x80  /* "TAKEN OUT!"         2104 */
#define B3_TDFX_MSG_AVENGED            0x81  /* "TAKEDOWN AVENGED!"  2105 */
#define B3_TDFX_MSG_TAKEDOWN           0x93  /* "TAKEDOWN!"          2074 */
#define B3_TDFX_MSG_VEHICLE_CLASS_0    0x94  /* 0x94..0x9C CAR..TRAILER   */
#define B3_TDFX_MSG_WALL_TAKEDOWN      0x9D  /* "WALL TAKEDOWN!"     2089 */
#define B3_TDFX_MSG_BONUS_TAKEDOWN     0x9E  /* "BONUS TAKEDOWN!"    2088 */
#define B3_TDFX_MSG_STREAK_0           0x9F  /* 0x9F..0xA2 2-IN-A-ROW..RAMPAGE */
#define B3_TDFX_MSG_MULTI_0            0xA3  /* 0xA3..0xA6 DOUBLE..TOTAL  */
#define B3_TDFX_MSG_REVENGE            0xA7  /* "REVENGE!"           2094 */
#define B3_TDFX_MSG_GRUDGE             0xA8  /* "GRUDGE!"            2095 */
#define B3_TDFX_MSG_PSYCHE_OUT         0xA9  /* "PSYCHE OUT!"        2093 */
#define B3_TDFX_MSG_AFTERTOUCH_0       0xAA  /* 0xAA..0xAE AFTERTOUCH+multi */
#define B3_TDFX_MSG_SIGNATURE_0        0xAF  /* 0xAF..0xC2 SIGNATURE TAKEDOWN */

/* animation constants, all read from the image [C] */
#define B3_TDFX_ANIM1_IN     1.5f    /* DAT_0054FB90 <- 0x003B1870 */
#define B3_TDFX_ANIM2_IN     0.75f   /* DAT_0054FB50 <- 0x003A55F8 */
#define B3_TDFX_ANIM1_OUT    0.75f   /* DAT_0054FB84 = 0.5 + 0.25   */
#define B3_TDFX_ANIM2_OUT    1.0f    /* 0x003B168C                  */
#define B3_TDFX_HOLD1_LONG   2.0f    /* 0x003B1688, descriptor flags bit 3 */
#define B3_TDFX_HOLD1_SHORT  0.125f  /* 0x003B1728                  */
#define B3_TDFX_HOLD2_LONG   2.25f   /* 0x003B1F34                  */
#define B3_TDFX_HOLD2_SHORT  1.0f    /* 0x003B168C                  */

typedef struct B3TdfxCallout {
    int   msg_id;        /* B3_TDFX_MSG_*, or -1 when nothing is up      */
    int   string_index;  /* Data/Globalus.bin string index               */
    int   sign_kind;     /* the table's kind byte                        */
    int   descriptor;    /* index into the 0x003895E8 table              */
    int   sign_index;    /* DAT_004608F0[] index; 17 = no sign           */
    int   anim_style;    /* descriptor +0x04: 1 or 2                     */
    int   flags;         /* descriptor +0x0C                             */
    float t_in;          /* end of the slam-in phase                     */
    float t_hold_end;    /* = element +0x154                             */
    float t_total;       /* end of the fly-out phase                     */
    float elapsed;       /* element +0x150                               */
    int   phase;         /* 0 in, 1 hold, 2 out, 3 finished              */
    float phase_u;       /* 0..1 progress within the phase               */
    int   visible;
} B3TdfxCallout;

/* Table lookup, straight off the recovered 0x00389160 table.  Returns 0
 * for a message id this module does not carry. */
int b3_tdfx_message_info(int msg_id, int *string_index, int *sign_kind,
                         int *descriptor, int *sign_index, int *anim_style,
                         int *flags);

/* FUN_00199350's accept test: a post is rejected when its id is LOWER than
 * the id already up (the message id doubles as the priority), and the
 * victim-side ids 0x7F/0x80/0x81 require the racecar's crashed flag.
 * Returns 1 if accepted. */
int b3_tdfx_post_callout(int msg_id, int racecar_crashed, int flag_byte);
/* OnTakedown credit spend (FUN_00025A30 @0x00025A7E/@0x00025AA3 ->
 * FUN_00025CC0): returns 1 if the divisor-5 request was made. */
int b3_tdfx_takedown_credit(int attacker_is_human, int victim_is_human,
                            float health_16C4);

const B3TdfxCallout *b3_tdfx_callout(void);

/* FUN_001994D0's message selection, in its recovered precedence order.
 * [S] -- read from the instructions, no green whole-function case. */
int b3_tdfx_select_takedown_message(int revenge, int aftertouch,
                                    int multi_count, int streak_count,
                                    int wall, int signature_index);

/* ======================================================================
 * 4. HARNESS-FACING SINGLETON API
 * The pieces above are pure and differentially testable; this layer is the
 * thin driver src/burnout3_full.c calls.  Marked GLUE where it is not a
 * direct port (camera framing, the world query).
 * ====================================================================== */

typedef struct B3TdfxCamera {
    int   active;       /* 1 while the cinematic owns the view            */
    float weight;       /* 0..1 ease-in/out blend (GLUE)                  */
    float eye[3];
    float look[3];
    float fov;          /* degrees; 90 base / 110 at full boost [C]       */
    float pitch_deg;    /* the smoothed pitch state, mode obj +0x1C  [C]  */
    float yaw_deg;      /* the smoothed yaw state,   mode obj +0x18  [C]  */
    /* --- appended by the crash-director pass (section 9); the retail
     * camera state's own fields --- */
    float right[3];     /* the camera basis rows (state +0x20 as a quat)  */
    float up[3];
    float fwd[3];
    float quat[4];      /* camera state +0x20, FUN_00011B10's output      */
    float focus[3];     /* the look anchor: car.pos in Focus Offset space */
} B3TdfxCamera;

/* ----------------------------------------------------------------------
 * 3b. THE RECOVERED CAMERA LAW  (RE_TAKEDOWN_FX section 8)
 *
 * The retail camera is a 20-mode director (FUN_001674B0 builds it,
 * FUN_00167940 ticks it, FUN_00167CE0 / FUN_00167C90 switch modes).  The
 * player's chase camera is director mode 2/3, class vtable 0x003A9C28,
 * per-frame update FUN_0015E550, and it is the only camera mode wired to
 * the "Camera/Follow" ValueDB group.  Its smoothing core is ported here.
 * -------------------------------------------------------------------- */

/* Camera/Follow, read out of FUN_00160840's compiled-in defaults; none of
 * the six Camera.cfg geometry keys has a Data/vdb.xml override, so these
 * ARE the shipped retail values [C -- image bytes]. */
#define B3_CAM_FOLLOW_OFF_X    0.0f    /* cfg+0x60                        */
#define B3_CAM_FOLLOW_OFF_Y    0.95f   /* cfg+0x64  0x003A69B8            */
#define B3_CAM_FOLLOW_OFF_Z   (-6.8f)  /* cfg+0x68  0x003B1B80            */
#define B3_CAM_FOCUS_OFF_X     0.0f    /* cfg+0x70                        */
#define B3_CAM_FOCUS_OFF_Y     0.35f   /* cfg+0x74  0x0039B2B0            */
#define B3_CAM_FOCUS_OFF_Z     2.0f    /* cfg+0x78  0x003B1688            */
#define B3_CAM_SPRING_X        0.06f   /* cfg+0x80  0x00387C04            */
#define B3_CAM_SPRING_Y        0.1f    /* cfg+0x84  0x003A69C4            */
#define B3_CAM_SPRING_Z        5.0f    /* cfg+0x88  0x003B1694            */
#define B3_CAM_DOWN_ANGLE      1.8f    /* cfg+0x90  0x003B03EC            */
#define B3_CAM_FOV            90.0f    /* 0x003B1850, stored at 0x001678B5 */

/* Sub-step count the update quantises the frame delta to [C]:
 *   n = (int)(60.0f * dt + 0.5f)      60.0 @0x003F720C, 0.5 @0x003B1684 */
int   b3_cam_substeps(float dt);

/* Speed gate on the pitch spring [C]:
 *   g = min(1.0f, speed_ms * 2.236936330795288f * 0.02f)
 * (0x0015E5B6; the mph constant is the game's own 2.2374146-free path --
 * this site uses the true 2.236936330795288 at 0x0038994C, 0.02 at
 * 0x003B1A08, clamp against 1.0 at 0x003B168C.) */
float b3_cam_speed_gate(float speed_ms);

/* The blend factor the camera lerps its angles with [C].  gate is 1.0f for
 * the ungated (yaw) axis:  blend = 1 - ((1 - spring) * gate)^n            */
float b3_cam_follow_blend(float spring, float gate, int n);

/* One exponential angle step, FUN_0015E550 @0x0015E885..0x0015E8A3 [C]. */
float b3_cam_smooth_angle(float cur_deg, float target_deg, float blend);

/* ----------------------------------------------------------------------
 * 3c. THE WHOLE CHASE-CAMERA UPDATE  --  FUN_0015E550, 1:1  [C]
 *
 * RE_TAKEDOWN_FX section 9.  This is the camera the player is looking
 * through while their own car is a wreck: nothing in the retail image
 * switches the director's mode on a crash (no camera code reads
 * racecar+0x18FA, no sender posts a crash camera message -- and the two
 * ids FUN_00025CC0 hands to FUN_0018E050 are RACE-RESULT kinds, not
 * camera messages: they land on the dispatcher's default handler).  So
 * mode 2 keeps running, over the wreck's own tumbling transform.
 *
 * Recovered law, all of it executed under Unicorn
 * (tools/emulate_tdfx_camera.py:follow_update):
 *
 *   focus  = car.pos + car.right*Fx + car.up*Fy + car.fwd*Fz   [Focus Offset]
 *   pitch += (asin(car.fwd.y)*180/pi - pitch) * blend_pitch     0x0015E869
 *   yawerr = signed acos( dot( flatten(car.fwd), flatten(cam.fwd) ) )
 *            (sign = dot(car.fwd, cam.right) < 0 -> negative)   0x0015EB6F
 *   yaw   += yawerr * blend_yaw                                 0x0015EBD9
 *   eye    = focus + CameraOffset . Rx(-pitch) . Ry(yaw)        0x0015EC79
 *   basis  = Rx(DownAngle) . Rx(-pitch) . Ry(yaw)               0x0015ECDE
 *   fov    = 90 + (1 - (boost_ramp-1)^2) * (110 - 90)           0x0015ED5C
 *   eye   += basis.fwd * (2.3 - 2.3/tan(fov/2))                 0x0015EDBD
 *
 * with Rx(a) = {{1,0,0},{0,cos a,sin a},{0,-sin a,cos a}} and
 *      Ry(a) = {{cos a,0,-sin a},{0,1,0},{sin a,0,cos a}}, row-vector
 * (FUN_00011900 takes DEGREES: it multiplies by 0.0174533 at 0x0001190C).
 * -------------------------------------------------------------------- */

#define B3_CAM_FOV_BOOST     110.0f  /* 0x003F7210 */
#define B3_CAM_FOV_PUSH        2.3f  /* 0x003F7218 */
#define B3_CAM_LOWSPEED_MPH   50.0f  /* 0x003F721C: camera-collision gate  */
#define B3_CAM_DEG2RAD  0.01745329238474369f  /* 0x003A1248 */
#define B3_CAM_RAD2DEG  57.29578f             /* 0x00395D78 */

/* the real mode object's per-frame state */
typedef struct B3CamFollow {
    float yaw_deg;      /* mode +0x18                                    */
    float pitch_deg;    /* mode +0x1C                                    */
    int   yaw_gate;     /* mode +0x24, set to 1 by the enter FUN_0015E060 */
    int   look_back;    /* mode +0x26, negates car.fwd and car.right      */
} B3CamFollow;

void b3_cam_follow_init(B3CamFollow *st);

/* car_rows = the vehicle's own matrix at *(veh+0x204): four rows of three
 * floats -- right, up, forward, position.  speed_ms = veh+0x0BC,
 * boost_ramp = racecar+0x11AC (0..2, the FOV ramp). */
void b3_cam_follow_update(B3CamFollow *st, const float car_rows[12],
                          float speed_ms, float boost_ramp, float dt,
                          B3TdfxCamera *out);

typedef struct B3TdfxStatus {
    int   active;        /* cinematic running                             */
    float t;             /* seconds since entry (real time)               */
    int   slowmo;        /* 1 while the divisor-5 phase runs              */
    int   divisor;       /* current time divisor                          */
    float timescale;     /* 1.0f / divisor                                */
    float pitch;         /* audio rate scale (1.0 or 0.75)                */
    float dt;            /* dilated per-frame dt                          */
    float clock;         /* dilated game clock                            */
    float real_clock;    /* undilated clock                               */
    int   victim_slot;   /* grid slot of the car being shown              */
    int   attacker_slot;
    int   callout_visible;   /* the callout sign should be on screen      */
    float callout_age;       /* = the element's elapsed seconds           */
    int   callout_sign;      /* DAT_004608F0[] index while visible, else -1 */
    int   callout_msg;       /* B3_TDFX_MSG_* currently up, else -1       */
    int   callout_string;    /* its Globalus.bin string index, else -1    */
    int   callout_phase;     /* 0 in, 1 hold, 2 out                       */
    float callout_phase_u;   /* 0..1 within the phase                     */
    int   callout_event;     /* last cinematic phase marker (9/10/11)     */

    /* --- the ATTACKER's control + crash state, the retail per-frame values.
     * These are what the harness needs to reproduce the handback correctly;
     * without them the cinematic's own crash-immunity window is computed and
     * then thrown away.
     *
     * ai_wheel     racecar+0x27D8, written by FUN_0018CB60 @0x0018CB6A.  Its
     *              only per-frame consumer is FUN_0018C510 @0x0018C53A:
     *              set -> FUN_00170820 (the route-following auto-driver)
     *              drives the car; clear -> the human's pad does.  Retail
     *              holds it for t in [0, 2.8) only.  This is the correct
     *              fork variable -- `divisor > 1` is the GLOBAL dilation and
     *              several other producers write it.
     *
     * att_flags1353  vehicle+0x1353 as of this frame.  It is an EPHEMERAL
     *              flag byte: FUN_00110AF0 @0x00110EA2 zeroes it for every
     *              vehicle at the top of each frame and every other writer
     *              in the image is an OR (there is no AND anywhere), so a
     *              bit only stays set while something re-asserts it.
     *
     * att_crash_off  (att_flags1353 & 0x18) != 0.  FUN_00027AD0 @0x00027C9D
     *              re-ORs 0x18 into the attacker's vehicle EVERY frame while
     *              t is in [2.8, 4.8) -- i.e. for the 2.0 s AFTER the camera
     *              hands control back.  Bit 3 is the flag FUN_0011AEF0
     *              @0x0011B94D tests to skip the WALL crash; bit 4 is the one
     *              FUN_00112E70 @0x0011329A tests to skip the OBJECT crash.
     *              So retail gives the player a two-second grace in which the
     *              car it just auto-drove cannot wreck.  [C-disasm]
     *
     * att_authority  vehicle+0x1534, the crash-threshold scale.  FUN_0018CB60
     *              @0x0018CB94 and FUN_000279C0 @0x000279F6 both snap it back
     *              to 1.0 on the falling edge of ai_wheel, undoing whatever
     *              the AI driver left behind (FUN_00105340 @0x00105F21 writes
     *              0.97*x + 0.03 and @0x00105F4B/@0x00105F63 the bare 0.03 --
     *              a car under the auto-driver crashes at a thirtieth of the
     *              normal impact threshold).  [C-disasm]
     *
     * att_hud_flag  racecar+0x245D as of this frame.  FUN_00027AD0 @0x00027C6D
     *              stamps it 1 on every slow-mo frame (t < 2.8) and
     *              FUN_000279C0 zeroes it on exit.  Its ONE reader is the AI
     *              driver FUN_00105340 @0x00105EB3, which turns it into
     *              `veh+0x1353 |= 0x18` at 0x00105F95 and consumes it at
     *              0x00105EC9 -- that is how retail makes the attacker
     *              crash-proof for the 2.8 s the autopilot is steering it,
     *              the first half of the grace whose second half is the
     *              cinematic's own [2.8, 4.8) OR at 0x00027C9D.  [C-disasm]
     *
     * handback     the record is live but the wheel is already back:
     *              t in [2.8, 4.8).
     *
     * wheel_release  one-frame pulse on the 1 -> 0 edge of ai_wheel, i.e. the
     *              frame FUN_0018CB60 @0x0018CB94 snaps att_authority back to
     *              1.0.  It STICKS in retail: once the pad drives the car
     *              again FUN_00105340 no longer runs for it, so nothing
     *              re-collapses the authority.  [C-disasm]
     *
     * cam_hold     the cinematic camera should still be on the VICTIM: the
     *              record is live AND t < 2.5.
     *
     *              [S], one step from [C].  The [C] facts: the cinematic
     *              broadcasts presentation state 3 through FUN_00053D20 on
     *              entry (FUN_00027920 @0x00027965) and state 1 -- racing --
     *              at t >= 2.5 alongside the callout (FUN_00027AD0
     *              @0x00027C1D); FUN_00053D20 is a state-change broadcast
     *              over a listener list (each listener's vtable +0x04/+0x08
     *              is called with new/old state); NOTHING is broadcast at the
     *              4.8 s record exit -- FUN_000279C0 posts nothing at all;
     *              and the camera director's message handlers 9/10/11
     *              (0x001685B5, 0x001686CE, 0x00168788) blend between the
     *              chase camera and mode 10 over 0.5 s (0x003B1684).  The
     *              inference: the takedown camera is released when the
     *              presentation state goes back to 1, i.e. at 2.5, NOT when
     *              the record expires at 4.8.  Which listener the camera
     *              controller is installed as is still [?]
     *              (docs/RE_TAKEDOWN_FX.md section 8.4).
     *
     *              This matters more than it looks.  On the 2.5 reading the
     *              player gets the VIEW back 0.3 s BEFORE the wheel and then
     *              has 2.0 s of crash grace to re-orient -- a clean handback.
     *              On the 4.8 reading the player drives blind for two full
     *              seconds, which is exactly the reported bug. */
    int   ai_wheel;
    int   att_flags1353;
    int   att_crash_off;
    float att_authority;
    int   att_hud_flag;
    int   handback;
    int   wheel_release;
    int   cam_hold;
} B3TdfxStatus;

void  b3_tdfx_init(void);

/* Arm the takedown cinematic.  Mirrors FUN_000278B0's gate chain:
 * only for a class-0 (human) attacker, with the global enable
 * DAT_004AE1DB, then FUN_00027A60.  Returns 1 if the camera armed. */
int   b3_tdfx_on_takedown(int attacker_slot, int attacker_crashed,
                          int victim_slot, int victim_crashed,
                          float victim_clock, float victim_crash_time,
                          int signature_id, int burnout_chain_flag);

/* Per-frame driver.  Ticks the timer with the real frame delta, runs the
 * cinematic update, and returns the SIM dt for this frame (real_dt scaled
 * by 1/divisor).  Call once per frame, before stepping the simulation. */
float b3_tdfx_update(float real_dt);

/* External state the cinematic reads (the harness pushes it in). */
void  b3_tdfx_set_attacker_crashed(int crashed);
void  b3_tdfx_set_race_finished(int finished);

/* Aftertouch / "Impact Time": while the player's own car is crashed, the
 * boost button requests divisor 5 (FUN_00118410 @ 0x001188A4).  Call every
 * frame with the crashed state and the boost-input bit. */
void  b3_tdfx_set_aftertouch(int player_crashed, int boost_held);

/* The 0.35 s impact-hit slam (divisor 6).  NOTE the 0.35 s is measured on
 * the DILATED game clock (DAT_0060EA20, 0x00026507), so at divisor 6 the
 * window is 2.1 s of wall clock.
 *
 * IMPORTANT -- this is NOT "the crash slow-mo".  Its only arm site in the
 * image is the game-context big-hit virtual vtbl+0x54 = FUN_00026A70, whose
 * only caller is the car-vs-OBJECT contact response FUN_00112E70
 * @0x001134EF, and it is gated there on the touched object's record:
 * (+0x174 & 8) set and (+0x174 & 2) clear.  A plain wall or car-vs-car
 * wreck never reaches it -- FUN_0011AEF0's wall trigger calls FUN_0010DCA0
 * with a NULL cause record.  Arming it on every wreck TRUNCATES the crash's
 * own divisor-5 request at 0.40 game-seconds (RE_TAKEDOWN_FX 9.4) and drops
 * the rest of the flight to full speed, which is what made wrecks appear to
 * spin fast.  b3_tdfx_impact_hit() therefore only arms when a real object
 * contact qualified it this frame. */
void  b3_tdfx_qualify_big_hit(int flags174);
void  b3_tdfx_impact_hit_object(int flags174);
void  b3_tdfx_impact_hit(void);

/* The DILATED per-frame delta the last b3_tdfx_update() produced (the port's
 * DAT_0060EA1C).  This is what the camera modes other than 0 and 14 run on
 * (FUN_000170B0 @0x0001714F). */
float b3_tdfx_sim_dt(void);

/* ======================================================================
 * 4b. THE PLAYER-CRASH DILATION WINDOW  (RE_TAKEDOWN_FX section 9)
 *
 * Recovered chain, all [C-disasm]:
 *   FUN_00025850  race-rules "OnVehicleWrecked" (vtable 0x003A99F4 slot,
 *                 ECX = the rules object, arg = the vehicle).  At
 *                 0x00025890 it calls FUN_00025CC0 only when
 *                     racecar+0x16C8 != 0   (crash-presentation credit)
 *                 &&  racecar+0x16C4 == 0.0 (health floored)
 *                 &&  racecar+0x1920 == 0   (the human)
 *   FUN_00025CC0  decrements the credit (0x00025CD6) and, once it reaches
 *                 zero, requests divisor 5 (0x00025D5C) and latches
 *                 DAT_00649B9E = 1 (0x00025D66) -- gated on the rules
 *                 object's enable byte +0x4C (0x00025D33), which
 *                 FUN_00025040 / FUN_000243D0 set to 1 at event start.
 *   The credit racecar+0x16C8 is initialised to 1 for every car by the
 *   event reset FUN_00025AB0 (0x00025AE5) and cleared again for class-1
 *   cars (0x00025B63), so in a race exactly ONE crash per car gets the
 *   divisor-5 request.
 *   The request is made ONCE, not per frame; what takes it back is
 *   whichever of these fires next:
 *     0x00026525  the impact-hit window expiring (divisor 1) -- this
 *                 truncates the crash window on any wreck that armed it
 *     0x001188D6  FUN_00118410 releasing aftertouch (only if it engaged)
 *     0x00119C24  FUN_00119C00, the vehicle leaving crash mode
 *     0x000269FD  the mode's "car recovered" virtual
 * ====================================================================== */

/* The wreck instant: FUN_00025850's gate + FUN_00025CC0.  Pass the
 * player's crash-presentation credit state; returns 1 if the divisor-5
 * request was made. */
int   b3_tdfx_crash_begin(void);

/* FUN_00119C00 / FUN_000269D0: the wreck leaves crash mode (recovery or
 * respawn) -- restores divisor 1 and audio pitch 1.0. */
void  b3_tdfx_crash_end(void);

/* FUN_00025AB0 @0x00025AE5: the event reset that re-arms the one
 * crash-presentation credit racecar+0x16C8. */
void  b3_tdfx_event_reset(void);

void  b3_tdfx_status(B3TdfxStatus *out);
float b3_tdfx_timescale(void);
int   b3_tdfx_divisor(void);
float b3_tdfx_pitch(void);

/* Cinematic camera.  Target is the VICTIM [C].  The angle smoothing, the
 * sub-step quantisation, the speed gate, the Camera/Follow offsets and the
 * 90 degree FOV are all recovered; the CHOICE to frame the victim with the
 * follow camera's geometry (rather than whichever director mode retail
 * selects, which is still [?]) is GLUE and marked at the call site.
 * Returns 1 when it owns the view. */
int   b3_tdfx_camera(const float victim_pos[3], const float victim_vel[3],
                     const float attacker_pos[3], float real_dt,
                     B3TdfxCamera *out);

/* Player-crash camera -- THE RECOVERED ONE.  RESOLVED (was [?]): retail
 * does not switch camera mode on a crash at all, so the crash view is
 * director mode 2 (FUN_0015E550) still running over the wreck's own
 * transform.  This runs that law 1:1 (section 3c above); the only choice
 * left to the harness is which transform it feeds in.
 *
 * car_rows = the wreck's matrix rows {right, up, forward, position}
 * (12 floats).  speed_ms and boost_ramp are veh+0x0BC and racecar+0x11AC.
 * Returns 1 when it owns the view.  Reset with
 * b3_tdfx_crash_camera_reset() when the wreck recovers. */
int   b3_tdfx_crash_camera_x(const float car_rows[12], float speed_ms,
                             float boost_ramp, float real_dt,
                             B3TdfxCamera *out);

/* Compatibility entry point for callers that only have a position and a
 * velocity: it synthesises a levelled basis from the velocity and calls
 * b3_tdfx_crash_camera_x.  DEGRADED -- the recovered law's pitch target is
 * asin(car.forward.y), so a wreck fed through here cannot pitch the camera
 * with its tumble.  New callers should use b3_tdfx_crash_camera_x. */
int   b3_tdfx_crash_camera(const float wreck_pos[3], const float wreck_vel[3],
                           float wreck_speed_ms, float real_dt,
                           B3TdfxCamera *out);
void  b3_tdfx_crash_camera_reset(void);

/* Drain the posted HUD/presentation events in order (returns 0 when
 * empty).  The HUD agent consumes these to drive the callout. */
int   b3_tdfx_poll_post(B3TdfxPost *out);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_TAKEDOWN_H */
