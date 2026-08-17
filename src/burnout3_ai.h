#ifndef BURNOUT3_AI_H
#define BURNOUT3_AI_H
/* The AI DRIVER: the real control law that steers/throttles/brakes an AI race
 * car, RE'd from the retail XBE, replacing the harness's line-follower GLUE.
 *
 * OWNERSHIP: this module (both files) belongs to the AI/control-audit agent.
 * The harness calls only this contract; burnout3_full.c call sites are patched
 * by the orchestrator.  Integration hunks: scratchpad/ai/integration_ai.md.
 *
 * ---------------------------------------------------------------------------
 * WHAT WAS RECOVERED (docs/RE_AI.md sections 8-13; every law below has a
 * differential case in tools/validate_ai.py that runs the REAL x86 under
 * Unicorn and asserts this C reproduces it bit-for-bit-ish (1e-6)).
 *
 * The key that unlocked the previously-"unlocated AI target writers": the AI
 * object is EMBEDDED IN the racecar at racecar+0x1A00.  The navigator ctor
 * FUN_001705F0 writes the aggregate back-pointers *(rc+0x1A04)=rc,
 * *(rc+0x21A0)=rc, *(rc+0x2160)=rc -- i.e. the `this+0x004`, `this+0x7A0`,
 * `this+0x760` slots that FUN_00171E30 / FUN_00171A10 / FUN_0016AAC0
 * dereference.  Therefore
 *      racecar+0x23C0 (target steering angle)  ==  AI+0x9C0
 *      racecar+0x23C4 (target speed)           ==  AI+0x9C4
 * and both ARE written by name, in FUN_00171E30 and FUN_001724F0.  There were
 * never any hidden derived-pointer writes; the search had simply been for the
 * wrong base.
 *
 * Per-frame chain (FUN_00170820 -> FUN_00171A10), all ported here:
 *   FUN_00173690   speed cap             -> AI+0xA08     b3_ai_speed_cap
 *   FUN_0016AAC0   arbitrator            -> AI+0x770/0x780/0x784
 *   FUN_0016AE20     "target wins" leg   -> dir + time-to-target
 *                                                        b3_ai_commit_target
 *   snapshot/normalize                   -> AI+0x7B0, AI+0x9C8
 *                                                        b3_ai_frame_snapshot
 *   FUN_00171E30   steering demand       -> AI+0x9C0     b3_ai_target_angle
 *   FUN_001724F0   speed demand          -> AI+0x9C4     b3_ai_target_speed
 *     FUN_00172E80   corner-speed law                    b3_ai_corner_speed
 *   FUN_00105340   the driver (inputs)                   b3_ai_drive
 *     FUN_00104CA0   brake helper                        b3_ai_brake
 *   0x00171078     out-of-range governor                 b3_ai_oor_governor
 *
 * NOT RECOVERED (the wall, documented precisely in docs/RE_AI.md section 12):
 * the aim POINT itself (AI+0x180 / AI+0x200) comes from the nav-graph walk in
 * FUN_00175B10 over the .bgd road network (node link tables + the 16-byte
 * point pool at DAT_0073A174), which the harness does not load in that form.
 * The caller therefore supplies the aim point; everything downstream of it is
 * the game's own arithmetic.
 * ---------------------------------------------------------------------------
 */

/* ===== AI config -- registrar FUN_0016AFD0, static struct 0x0047A140 ===== */
typedef struct {
    float oor_speed_dec_rate;   /* +0x000 Out of range speed decrease rate  */
    float oor_max_dir_deg;      /* +0x004 Max desDir angle change OOR       */
    float angle_min_speed_deg;  /* +0x008 Angle you want min spd at         */
    float top_speed_mps;        /* +0x00C Top speed mps                     */
    float min_speed_mps;        /* +0x010 Min speed mps                     */
    float car_at_weight;        /* +0x014 How much carAt affects steering   */
    float drift_start_deg;      /* +0x018 Angle at which drift is started   */
    float max_lock_deg;         /* +0x01C Max lock at 180 x degrees         */
    float drift_max_lock_deg;   /* +0x020 Drift Max lock at 180 x degrees   */
    /* AI/Avoidance close-range speed caps (+0x05C/+0x060/+0x064) */
    float avoid_speed_10m, avoid_speed_20m, avoid_speed_30m;
    /* +0x08C AI/Target "Dist to brake speed factor big=>fast" -- the
     * DAT_0047A1CC that FUN_00176150 @0x00176216 multiplies. */
    float brake_dist_factor;
} B3AiParams;

extern B3AiParams b3_ai_params;      /* VDB-tuned values after b3_ai_init() */

void b3_ai_init(void);               /* load the retail Data/vdb.xml column */

/* ===== per-frame view of the car (filled by the caller) ================== */
typedef struct {
    /* racecar transform: row0/row2/row3 of the matrix at racecar+0x10 */
    float pos[3];            /* racecar +0x40 */
    float fwd[3];            /* racecar +0x30 */
    float right[3];          /* racecar +0x10 */
    /* physics vehicle */
    float car_at[3];         /* v +0xC0  heading ("carAt") vector           */
    float veh_fwd[3];        /* v->frame row2 (v+0x204 +0x20)               */
    float veh_right[3];      /* v->frame row0                               */
    float speed_ms;          /* v +0xBC                                     */
    float yaw_rate;          /* v +0xD4                                     */
    float engine_rpm;        /* v +0x149C * 9.549296 (rad/s -> rpm)         */
    float change_up_rpm;     /* v +0x1470                                   */
    int   drift_state;       /* v +0x1524  (1/2 = drifting)                 */
    int   gear;              /* v +0x14C8  (-1 reverse)                     */
    int   lsdm_active;       /* v byte +0x1550                              */
    float lsdm_limit_mph;    /* v +0x13AC                                   */
    /* racecar / race state */
    int   traffic_class;     /* racecar +0x134C == 0                        */
    int   race_mode;         /* racecar +0x1920 (1 = normal racing)         */
    float crash_timer;       /* racecar +0x190C (>= 0.5 => not "stuck")     */
    int   free_speed_floor;  /* racecar +0x2450 == 1 => no min-speed floor  */
    int   boosting;          /* racecar byte +0x11EE (boost record burning) */
    int   boost_ramp_done;   /* racecar byte +0x11F1                        */
    int   brake_suppressed;  /* rc+0x2190 != 0 && rc+0x2189 (bVar3)         */
    /* DRIFT-lock flags -- writer recovered in section 14: FUN_00171BE0, NOT
     * the aggression machine.  racecar+0x2188 (AI+0x788) is the arbitrator's
     * drift-enable byte (FUN_0016AE20 copies AI+0x212 into it); +0x2413/
     * +0x2414/+0x2415 are drift-left / drift-right / drift-commit.  The old
     * names attack_active/left/right/commit were a mislabel. */
    int   attack_active;     /* racecar byte +0x2188  == drift_enable       */
    int   attack_left;       /* +0x2413               == drift_left         */
    int   attack_right;      /* +0x2414               == drift_right        */
    int   attack_commit;     /* +0x2415               == drift_commit       */
    int   wants_boost;       /* +0x2419  (FUN_00171D90's latch)             */
    /* out-of-control envelope (the composite window FUN_00105340 tests on
     * racecar+0x1198/+0x1598/+0x16C0/+0x16BC against Total OOC Time) */
    int   ooc_window;        /* inside the slam + aggressor window          */
    int   ooc_mode;          /* racecar +0x23F8 (0/1/2)                     */
    int   ooc_countersteer;  /* !FUN_00198190(): throw opposite lock        */
    float clock;             /* DAT_0060EA20 race clock                     */
} B3AiCar;

/* ===== persistent AI-object state (one per AI car) ====================== */
typedef struct {
    /* --- navigator/arbitrator outputs --- */
    float des_dir[4];        /* AI +0x770 arbitrated desired direction      */
    float des_dir_n[4];      /* AI +0x7B0 normalized snapshot (this frame)  */
    float time_to_target;    /* AI +0x784 dist/speed, seconds               */
    float t2t_snap;          /* AI +0x9C8                                   */
    float max_speed;         /* AI +0x780 arbitrated speed ceiling          */
    float speed_cap;         /* AI +0xA08 hard cap (FUN_00173690)           */
    int   target_mode;       /* AI +0x1F8                                   */
    int   boost_scale_flag;  /* AI byte +0x213 -> x1.45 corner speed        */
    /* --- targeting outputs (== racecar+0x23C0/+0x23C4) --- */
    float target_angle;      /* AI +0x9C0 target steering angle, DEGREES    */
    float target_speed;      /* AI +0x9C4 target speed, m/s                 */
    float prev_angle;        /* AI +0x9CC slew-limiter memory               */
    float corner_speed;      /* AI +0x9D0                                   */
    float steer_err;         /* AI +0x9D4 yaw-rate error (drift path)       */
    /* --- driver-side timers (live on the physics vehicle) --- */
    float stuck_arm;         /* v +0x1578  (-1 = disarmed)                  */
    float reverse_timer;     /* v +0x157C  (-1 = idle)                      */
    float dither_deadline;   /* v +0x1574  throttle-dither cycle            */
    float prev_throttle;     /* v +0x156C                                   */
    float brake_hold;        /* v +0x1570                                   */
    float steer_authority;   /* v +0x1534                                   */
    float prev_steer;        /* v +0x1408 carried across frames             */
    int   drift_state;       /* v +0x1524 (driver also writes it)           */
} B3AiState;

/* ===== driver outputs (the physics vehicle's input block) =============== */
typedef struct {
    float throttle;          /* v +0x1400 */
    float brake;             /* v +0x1404 */
    float steer;             /* v +0x1408 */
    float throttle_raw;      /* v +0x1414 */
    unsigned char bits;      /* v +0x13FC  (4 = boost anchor)               */
    int   gear_request;      /* v +0x14C8: -1 reverse, 1 forward, 0 = keep  */
    int   shift_kick;        /* v +0x14A4 / +0x14A0 = 0.35 on a gear swap   */
    int   stop_flag;         /* v +0x1552                                   */
    int   engage_boost;      /* call the verified FUN_0017A5B0 gate         */
    int   commit_boost;      /* set racecar byte +0x11EF (min-burn latch)   */
} B3AiInputs;

void b3_ai_state_init(B3AiState* s);

/* --- individual stages (each has its own differential case) ------------- */

/* FUN_0016AE20: desired direction := normalize(target_point - car pos);
 * time-to-target := |d| / speed  (or |d| when speed <= 1).  Returns |d|. */
float b3_ai_commit_target(B3AiState* s, const B3AiCar* c,
                          const float target_point[3]);

/* FUN_00171A10 head: AI+0x7B0 := normalize(AI+0x770); AI+0x9C8 := AI+0x784 */
void b3_ai_frame_snapshot(B3AiState* s);

/* FUN_00171E30 -> AI+0x9C0 target steering angle (degrees) + AI+0x9D4 */
void b3_ai_target_angle(B3AiState* s, const B3AiCar* c);

/* FUN_00172E80 corner-speed law (returns the speed; pure) */
float b3_ai_corner_speed(const B3AiState* s, const B3AiCar* c,
                         float max_speed);

/* FUN_001724F0 -> AI+0x9C4 target speed (m/s).  `catchup_bonus` is
 * FUN_001734C0's output (0 when the catch-up branch is inactive). */
void b3_ai_target_speed(B3AiState* s, const B3AiCar* c, float catchup_bonus);

/* FUN_00104CA0: route a brake amount to brake or (in reverse) throttle */
void b3_ai_brake(B3AiInputs* in, B3AiState* s, const B3AiCar* c,
                 float amount);

/* FUN_00105340: the driver.  `reverse_aim_dot` is FUN_000FF0E0's
 * clamp(dot(racecar+0x18E0, car right), -1, 1) used only while reversing. */
void b3_ai_drive(B3AiState* s, const B3AiCar* c, B3AiInputs* in,
                 float dt, float reverse_aim_dot);

/* Whole chain in one call: aim point + arbitrated ceiling in, inputs out. */
void b3_ai_update(B3AiState* s, const B3AiCar* c, B3AiInputs* in,
                  const float target_point[3], float arbitrated_max_speed,
                  float catchup_bonus, float dt);

/* 0x00171078 (tail of the out-of-range mover FUN_00170B30): the speed
 * governor.  Returns the commanded speed for FUN_001204C0. */
float b3_ai_oor_governor(float speed_ms, float speed_mph,
                         float target_speed_ms);

/* FUN_00105150 (traffic driver) steering + speed band; the traffic cars use
 * the same MaxLock/180 conversion but a 5 mph brake excess. */
void b3_ai_traffic_drive(const B3AiCar* c, B3AiInputs* in,
                         float target_angle_deg, float target_speed_ms);

/* Avoidance close-range speed caps (AI/Avoidance, [C] params / [S] use) */
float b3_ai_avoid_speed_cap(float speed, float dist_ahead_m);

/* =========================================================================
 * THE AGGRESSION (ATTACK / SLAM) STATE MACHINE  -- docs/RE_AI.md section 14
 *
 * Object: a sub-object of the AI object at AI+0x170 == racecar+0x1B70.
 * Proof: FUN_00175A10 @0x00175A1F does `LEA ECX,[ESI+0x170]; CALL 0x00169490`
 * with ESI = the AI/target-follower base, and FUN_0016AF10 reads exactly the
 * same bytes through the racecar base (AI+0x191/+0x192/+0x1B8 == aggro+0x21/
 * +0x22/+0x48).
 *
 * How it reaches the wheels -- three separate channels, all recovered:
 *   1. STEERING.  The machine writes an aim point into aggro+0x10 and a
 *      validity byte into aggro+0x20.  FUN_0016AE20 (@0x0016AE2C) tests that
 *      byte and, when set, builds the desired direction from the aggression
 *      aim INSTEAD of the racing line.  Everything downstream (the 2.4/8.1
 *      slew limiter, MaxLock/180) is the ordinary driver.  This is what
 *      manufactures the lateral closing speed a full slam needs: state 3
 *      aims 5 m to the OUTSIDE (`Steer out distance`), then state 4 aims
 *      dead at the victim.
 *   2. SPEED.  FUN_0016AF10 turns aggro+0x21/+0x22/+0x48 into AI+0x790 (mode
 *      4 = close in, 5 = lock to the target's speed) and AI+0x794 (target),
 *      which FUN_00172FA0 consumes inside FUN_001724F0.
 *   3. BOOST.  FUN_00172FA0 arms AI+0xA17/+0xA2C, FUN_00171D90 latches
 *      AI+0xA19 = the driver's wants-boost.
 *
 * Every field carries its retail offset.  Ported functions:
 *   FUN_00169490  ctor/reset                b3_aggro_init
 *   FUN_00169540  the state machine         b3_aggro_update
 *   FUN_00169BD0  target selection          (internal)
 *   FUN_00169D70  can-slam predicate        (internal, -> can_slam)
 *   FUN_00169E80  state 4 tick              (internal)
 *   FUN_0016A0A0  positioning aim           (internal)
 *   FUN_0016A310  reset to idle             (internal)
 *   FUN_0016A360  aim-valid gate            (internal)
 *   FUN_0016A3E0  block-range predicate     (internal, -> block_range)
 *   FUN_0016A4E0  block aim                 (internal)
 *   FUN_0016A620  slam-speed predicate      (internal, -> want_slam_speed)
 *   FUN_0016A7D0  per-frame measure         (internal)
 *   FUN_0016A8C0  rubbed-blind timer        (internal)
 *   FUN_0016A950  rival retaliation         (internal)
 *   FUN_001716D0  |lateral offset|          (internal)
 *   FUN_001717B0  signed longitudinal gap   (internal)
 *   FUN_0016AF10  -> AI+0x790 / AI+0x794    b3_aggro_arbitrate
 *   FUN_00172FA0  aggression speed demand   b3_ai_aggro_speed
 *   FUN_00171D90  boost latch               b3_ai_boost_latch
 *   FUN_00171BE0  drift-lock flags          b3_ai_drift_flags
 * ========================================================================= */

/* AI/Aggressive Driving + .../Slam config (registrar FUN_0016AFD0, the
 * 0x0047A204..0x0047A25C block; defaults -> retail Data/vdb.xml column). */
typedef struct {
    float min_aggression;    /* +0x0C4 Min. aggression before attacking      */
    float min_wait_s;        /* +0x0C8 Min. time between attacks   (UNUSED)  */
    float max_wait_s;        /* +0x0CC Max. time between attacks             */
    float dist_ahead_m;      /* +0x0D0 Max dist apart to begin, when ahead   */
    float dist_behind_m;     /* +0x0D4 Max dist apart to begin, when behind  */
    float min_target_mph;    /* +0x0D8 Min. target speed to consider         */
    float slow_factor;       /* +0x0DC How much slower while speed matching  */
    float boost_dist_m;      /* +0x0E0 How far in front to boost             */
    float boost_aggro_m;     /* +0x0E4 Extra dist to boost vs aggression     */
    float start_delay_s;     /* +0x0E8 Wait at race start                    */
    float immunity_s;        /* +0x0EC How long after hitting something      */
    float block_min_s;       /* +0x0F0 Slam: min time to block               */
    float block_max_s;       /* +0x0F4 Slam: max time to block               */
    float block_dist_m;      /* +0x0F8 Slam: max distance ahead to block     */
    float separation_m;      /* +0x0FC Slam: preferred separation  (UNUSED)  */
    float position_time_s;   /* +0x100 Slam: max time to get into position   */
    float ahead_gap_m;       /* +0x104 Slam: max distance apart, when ahead  */
    float speed_diff_mph;    /* +0x108 Slam: max difference in speeds        */
    float steer_out_m;       /* +0x10C Slam: steer out distance              */
    float steer_out_s;       /* +0x110 Slam: steer out time                  */
    float slam_s;            /* +0x114 Slam: slam time                       */
    float max_cos_off_lane;  /* +0x118 Slam: max cos angle off lane          */
    float commit_s;          /* +0x11C Slam: committed after (UNUSED here)   */
    float sticky_dist_m;     /* +0x0BC Driver: max dist, sticky matching     */
    float sticky_mph;        /* +0x0C0 Driver: max speed diff, sticky match  */
    float close_match_m;     /* +0x0B8 Driver: how close to start matching   */
} B3AiAggroParams;

extern B3AiAggroParams b3_ai_aggro_params;

/* AI/Target +0x0A0 (0x0047A1E0) -- the drift-commit time threshold that
 * FUN_00171BE0 compares |aim - pos| / speed against.  Registered five times
 * onto the same slot by FUN_0016AFD0 (RE_AI section 1's note); the live
 * value is whatever bound last. */
extern float b3_ai_drift_apex_time;

/* --- the aggression object (AI+0x170) ---------------------------------- */
enum {
    B3_AGGRO_IDLE      = 0,  /* look for a target                           */
    B3_AGGRO_APPROACH  = 1,  /* get into slamming position (30 s)           */
    B3_AGGRO_RETRY     = 2,  /* 1 s re-arm after a failed attempt           */
    B3_AGGRO_STEER_OUT = 3,  /* pull `steer out distance` clear (0.5 s)     */
    B3_AGGRO_SLAM      = 4,  /* aim at the victim (0.75 s)                  */
    B3_AGGRO_COOLDOWN  = 5,  /* wait aggression x max_wait seconds          */
    B3_AGGRO_RECOIL    = 6,  /* steer away after contact (0.5 s)            */
    B3_AGGRO_BLOCK     = 7   /* sit in front of the target                  */
};

typedef struct {
    int   state;             /* +0x00                                       */
    float aim[4];            /* +0x10 attack aim point (world) == AI+0x180  */
    unsigned char aim_valid; /* +0x20 == AI+0x190: use `aim` this frame     */
    unsigned char attacking; /* +0x21 == AI+0x191: arbitrator speed gate    */
    unsigned char slam_speed;/* +0x22 == AI+0x192: mode 5 instead of 4      */
    unsigned char blocked;   /* +0x23 == AI+0x193: hit something recently   */
    unsigned char hit;       /* +0x24 == AI+0x194: contact, set externally  */
    float blind_time;        /* +0x4C rubbed-blind window (FUN_0016A8C0)    */
    float blind_phase;       /* +0x50                                       */
    unsigned int blind_bits; /* +0x54 rotating mask                         */
    unsigned char blind_arm; /* +0x58                                       */
    unsigned char blind_out; /* -> own racecar +0x18FD                      */
    float timer;             /* +0x2C state deadline (-1 = never)           */
    float entered;           /* +0x30 clock when the state was entered      */
    float side;              /* +0x34 committed side, +-1                   */
    float lateral;           /* +0x38 |lateral offset| to the target (m)    */
    float longitudinal;      /* +0x3C signed gap, + = target ahead (m)      */
    unsigned char can_slam;      /* +0x40 FUN_00169D70                      */
    unsigned char block_range;   /* +0x41 FUN_0016A3E0                      */
    unsigned char want_slam_speed;/*+0x42 FUN_0016A620                      */
    int   target;            /* +0x48 rival index, -1 = none                */
} B3AiAggro;

/* One car as the aggression machine sees it.  Slot `i` of the world array
 * corresponds to racecar `DAT_0073A1D0 + i*0x27E0` in retail. */
typedef struct {
    float pos[4];            /* racecar +0x40                               */
    float fwd[4];            /* racecar +0x30                               */
    float right[4];          /* racecar +0x10                               */
    float road_dir[4];       /* racecar +0x18E0 (road direction at the node)*/
    float speed_ms;          /* physics vehicle +0xBC                       */
    float track_dist;        /* FUN_00194380(racecar+0x10D0), metres        */
    float aggression;        /* racecar +0x23E0 (per-opponent, 0..1)        */
    float car_width;         /* racecar +0x2444                             */
    float car_length;        /* racecar +0x2448                             */
    float ooc_time;          /* (racecar+0x1198)+0x1598, -1 = never         */
    float slammed_time;      /* racecar +0x16C0 last time we were hit, -1   */
    int   slammed_by;        /* racecar +0x16BC aggressor index, -1 = none  */
    float race_time;         /* racecar +0x10DC                             */
    int   race_mode;         /* racecar +0x1920 (0 = player, 1 = AI racer)  */
    int   car_class;         /* racecar +0x134C (0 = traffic)               */
    int   mode2450;          /* racecar +0x2450                             */
    int   rival;             /* racecar +0x1650 slot index, -1 = none       */
    int   wrecked;           /* racecar +0x18FA                             */
    int   in_takedown;       /* racecar +0x27D8                             */
    int   boosting;          /* racecar +0x11EE                             */
    float boost_start;       /* racecar +0x11C0 (boost-record start time)   */
    int   race_progress_zero;/* FUN_00194430(this car) == 0                 */
    int   progress_gate;     /* racecar +0x1394 > 0 (checked on the ATTACKER)*/
    int   no_slam_speed;     /* racecar +0x2431                             */
    int   drift_zone;        /* racecar +0x1C12 (AI+0x212)                  */
    int   node_open;         /* node link byte +2 == 0xFF (no junction)     */
    int   steer_ok;          /* physics vehicle byte +0x1550                */
    int   player_slot;       /* physics vehicle byte +0x1554 (fallback)     */
    const float* lateral_to; /* racecar +0x18A4[k], per-slot lateral offset */
    const float* last_hit;   /* racecar +0x15E0[k], per-slot last-hit time  */
} B3AiAggroCar;

typedef struct {
    const B3AiAggroCar* cars;
    int   ncars;             /* DAT_0073A1C0                                */
    float clock;             /* DAT_0060EA20                                */
    float dt;                /* DAT_0060EA1C                                */
    int   track_loaded;      /* DAT_0073A164                                */
} B3AiAggroWorld;

void  b3_aggro_init(B3AiAggro* a, float clock);              /* FUN_00169490 */
void  b3_aggro_update(B3AiAggro* a, const B3AiAggroWorld* w, int self);

/* FUN_0016AF10: aggro flags -> AI+0x790 mode (0/4/5) + AI+0x794 target.
 * Returns the mode; *target receives the rival index (-1 when idle). */
int   b3_aggro_arbitrate(const B3AiAggro* a, const B3AiAggroWorld* w,
                         int self, int* target);

/* --- the speed leg: FUN_00172FA0's persistent state (AI+0x9D8..+0xA30) --- */
typedef struct {
    int   mode;              /* AI+0x790  0 / 1 / 2 / 3 / 4 / 5             */
    int   target;            /* AI+0x794  rival index, -1                   */
    float matched;           /* AI+0x9D8                                    */
    unsigned char want_boost;/* AI+0xA17                                    */
    unsigned char boost_now; /* AI+0xA18                                    */
    unsigned char wants_boost;/*AI+0xA19 -> the driver                      */
    int   last_target;       /* AI+0xA1C                                    */
    float last_own_speed;    /* AI+0xA20                                    */
    float last_tgt_speed;    /* AI+0xA24                                    */
    float boost_rearm;       /* AI+0xA28                                    */
    float boost_until;       /* AI+0xA2C (-1 = disarmed)                    */
    unsigned char acquired_ahead; /* AI+0xA30                               */
    float brake_hold_out;    /* the v+0x1570 write at 0x00173457            */
    int   brake_hold_valid;
} B3AiAggroSpeed;

void  b3_aggro_speed_init(B3AiAggroSpeed* s);
/* FUN_00172FA0.  `spd` is FUN_00172E80's corner speed; returns the demand. */
float b3_ai_aggro_speed(B3AiAggroSpeed* s, const B3AiAggroWorld* w, int self,
                        float aggression, float spd);
/* FUN_00171D90: AI+0xA17/+0xA18 -> AI+0xA19 (the driver's wants-boost). */
void  b3_ai_boost_latch(B3AiAggroSpeed* s, float clock);

/* FUN_00171BE0: the drift-lock flags racecar+0x2413/+0x2414/+0x2415.
 * `drift_enable` is racecar+0x2188, `mode` is AI+0x1F4, `aim` is AI+0x200. */
void  b3_ai_drift_flags(int drift_enable, int mode, float target_angle_deg,
                        const float aim[4], const float pos[4],
                        const float fwd[4], float speed_ms,
                        int* out_left, int* out_right, int* out_commit);

/* ======================================================================== */
/* section 16: the ROUTE DRIVER's wheel + watchdogs -- FUN_00170820          */
/*                                                                          */
/* FUN_00170820 is the racecar vtable's per-frame AI entry (vtable base      */
/* 0x003B1204, slot +0x04; slot +0x20 = FUN_00171650 the re-placer, slot     */
/* +0x24 = FUN_00170B30 the out-of-range mover).  Everything below is read   */
/* from its disassembly and the two helpers it owns.                         */
/* ======================================================================== */

/* --- the 50 s / 50 s route-alternation square wave, FUN_00170820 head ----
 * @0x00170827..0x0017089C.  Two countdowns at racecar+0x1BE4 / +0x1BE8
 * (= AI+0x1E4 / +0x1E8) alternately drive the flag racecar+0x1BF0
 * (= AI+0x1F0); each expiry reloads the OTHER countdown with
 * DAT_003B16B8 = 50.0.  AI+0x1F0 is the type-1/type-3 admissibility gate in
 * the selector span FUN_00178310 (RE_AI 12), so retail's junction policy
 * flips every fifty seconds.  FUN_00175A10 starts it at 1.  [C-disasm] */
#define B3_AI_ROUTE_ALT_S        50.0f   /* DAT_003B16B8                    */

/* --- FUN_001712E0, the off-world watchdog ------------------------------
 * dy = racecar+0x44 - bilinear(node's four point heights) - vehicle+0x870.
 *   dy > -1.0            -> latch racecar+0x245A = racecar+0x18D0, clear the
 *                           counter (this is the LAST VALID NODE);
 *   dy < -5.0            -> racecar+0x2460++, at 0x3D frames FUN_001714F0;
 *   otherwise            -> clear the counter.                       [C]   */
#define B3_AI_ROAD_LATCH_M        1.0f   /* the -1.0 comparison @0x0017142B */
#define B3_AI_OFFWORLD_DROP_M     5.0f   /* the -5.0 comparison @0x00171446 */
#define B3_AI_OFFWORLD_FRAMES     61     /* 0x3D @0x00171459                */

/* --- FUN_00170820 @0x001708EE, the stuck rescue -------------------------
 * `CMP dword [ESI+0x1904],0xC8 / JLE` then
 * `MOVZX EAX,word [ESI+0x245A]; SUB EAX,8; XOR ECX,ECX; CALL FUN_001714F0`
 * followed by `FUN_001204C0(v, DAT_0047A150 * 0x003A5958)` --
 * "Min speed mps" (VDB 20) x 0.44704 = 8.9408 m/s = exactly 20 mph.
 * racecar+0x1904 is NOT a slow-speed counter: an exhaustive disp32 sweep of
 * all fourteen executable PT_LOADs finds exactly four references --
 *   0018d6b8  mov  [ebx+0x1904],eax   FUN_0018D0E0 spawn init
 *   0018d840  inc  [ebx+0x1904]       FUN_0018D790, when FUN_00174960 (the
 *                                     nav-graph cursor walk) returns 0
 *   001708ee  cmp  [esi+0x1904],0xC8  this test
 *   00170926  mov  [esi+0x1904],ebx   cleared by the rescue
 * -- so it counts CONSECUTIVE NAV-WALK FAILURES, and the 5 mph rule of
 * RE_AI 3 is the DRIVER's own reverse burst, a different mechanism.  [C]  */
#define B3_AI_NAVFAIL_FRAMES     200     /* strictly greater than 0xC8      */
#define B3_AI_RESCUE_BACK_NODES    8     /* SUB EAX,0x8                     */
#define B3_AI_RESCUE_SPEED_MS  8.9408f   /* "Min speed mps" x 0.44704       */

typedef struct {
    int   ai_enable;       /* racecar +0x19A8  AI drives this car           */
    int   ai_wheel;        /* racecar +0x27D8  the cinematic wheel flag     */
    int   skip_once;       /* racecar +0x245E  skip ONE AI update           */
    int   navfail_frames;  /* racecar +0x1904                              */
    int   below_frames;    /* racecar +0x2460                              */
    int   last_node;       /* racecar +0x245A  last node at road height    */
    float road_dy;         /* racecar +0x244C  height above the road       */
    int   route_alt;       /* AI +0x1F0                                    */
    float alt_t[2];        /* AI +0x1E4 / +0x1E8                           */
} B3AiWheel;

/* FUN_001705F0 / FUN_0018D0E0's spawn values (+0x245E = 0, +0x2460 = 0,
 * +0x244C = 0, +0x245A = node, +0x1904 = 0) plus FUN_00175A10's +0x1F0 = 1. */
void b3_ai_wheel_init(B3AiWheel* w, int node);

/* FUN_00170820 @0x00170827: advance the square wave.  Returns the (possibly
 * flipped) AI+0x1F0. */
int  b3_ai_route_alt(B3AiWheel* w, float dt);

/* FUN_0018CB60(racecar, _, on) -- the ONLY writer of racecar+0x27D8 that
 * carries handover semantics.  On a rising edge it zeroes the second half of
 * the direction block (racecar+0x19D0..+0x19DC) and calls FUN_00179760, the
 * navigator reset; on a falling edge it puts the physics vehicle's steering
 * authority v+0x1534 back to 1.0 and clears drift state 4.  Returns
 * B3_AI_WHEEL_TAKE / B3_AI_WHEEL_GIVE / 0 (no edge); the caller performs the
 * navigator reset on TAKE. */
enum { B3_AI_WHEEL_NONE = 0, B3_AI_WHEEL_TAKE = 1, B3_AI_WHEEL_GIVE = 2 };
int  b3_ai_wheel_set(B3AiWheel* w, B3AiState* s, int on);

/* FUN_00171650 (vtable +0x20), the re-place handler: after it has moved the
 * car, retail sets racecar+0x245E so the very next FUN_00170820 returns
 * before touching the brain.  Call after any re-place. */
void b3_ai_replace_done(B3AiWheel* w);

/* FUN_00170820's own gate: 1 = run the AI this frame.  Consumes +0x245E. */
int  b3_ai_wheel_gate(B3AiWheel* w, int have_section, int have_vehicle);

/* --- FUN_00176150, the corner-BRAKE law -> AI+0x1D0 --------------------
 * Read instruction for instruction at 0x00176150..0x00176283:
 *
 *   if (AI+0x1FC != 0 || AI+0x298 <= 0.0 [0x003B16E0])
 *       AI+0x1D0 = racecar+0x2408                        // the hard cap
 *   else {
 *       cs = AI+0x298;  if (AI+0x213) cs *= 1.02 [0x003A2C44];
 *       if (AI+0x1F8 == 0)                               // target mode 0
 *           AI+0x1D0 = DAT_0047A1CC * (cs - D) + cs      // D = FUN_00174AF0
 *       else
 *           AI+0x1D0 = cs;
 *   }
 *   AI+0x1D0 = min(AI+0x1D0, racecar+0x2408)
 *
 * and FUN_00174AF0 @0x00174AF0 returns
 *
 *   D = min(seg,  dot(carpos - pool[pair[node].point_a],
 *                     normalize(FUN_00174740(section, node))))
 *   seg = row->edge[node] - row->edge[node-1]   (node > 0; else row->edge[0])
 *
 * i.e. the car's SIGNED longitudinal offset from the node's A point along the
 * node's own forward direction, clamped by that node's segment length.  A
 * corner still ahead gives D < 0, so the ceiling RISES with the distance to
 * go -- `factor*(cs - D) + cs` = `(1+factor)*cs + factor*|D|`.
 *
 * The harness had the subtraction the other way round --
 * `factor*(dist - cs) + cs` = `factor*dist + (1-factor)*cs` -- which is
 * 1.2 x cs LOWER everywhere and collapses to 0.4 x cs at the node.  With a
 * planner corner speed of 5..10 m/s that is a 4..9 mph crawl the car can
 * never leave, because FUN_00105340's stuck detector is itself disarmed
 * below a zero target speed (@0x001054DA).  [C-disasm] */
float b3_ai_corner_brake(float corner_speed, float approach_d, int boost_scale,
                         float speed_cap, int target_mode, int mode_1fc);

/* FUN_001712E0.  `node` is racecar+0x18D0, `dy` is
 * pos.y - road_surface_y - vehicle+0x870.  Returns 1 when the 61-frame
 * off-world rescue (FUN_001714F0) must fire. */
int  b3_ai_offworld(B3AiWheel* w, int node, float dy);

/* FUN_0018D790 @0x0018D840 + FUN_00170820 @0x001708EE.  `walk_ok` is
 * FUN_00174960's return.  Returns 1 when the stuck rescue must fire; the
 * caller then re-places at `w->last_node - B3_AI_RESCUE_BACK_NODES` and
 * commands B3_AI_RESCUE_SPEED_MS. */
int  b3_ai_navfail(B3AiWheel* w, int walk_ok);

#endif
