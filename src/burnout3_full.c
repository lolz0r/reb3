// Burnout 3: Takedown - playable test harness
//
// IMPORTANT: this is NOT a decompilation of the game. The gameplay, physics and
// rendering below are original code written to have something runnable to drive
// the reverse engineering against. The only parts sourced from the real game are
// the vehicle roster in burnout3_vehicle_data.h (extracted from pveh/ and
// cross-validated against vlist.bin) and the identity strings printed at start-up.
//
// Real RE findings live in docs/RE_NOTES.md. Where a routine here has a genuine
// counterpart in the binary it is cited by its corrected virtual address;
// uncited code has no counterpart and should not be read as recovered logic.

#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <limits.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <GL/gl.h>
#ifdef __ANDROID__
/* ANDROID PORT (android/): <GL/gl.h> above resolves to gl4es' desktop-GL
 * header there, and this brings in the three glue calls implemented by
 * android/app/src/main/cpp/b3_android.c.  Desktop builds never see it. */
#include "b3_android.h"
#endif
#include "burnout3_vehicle_data.h"
#include "burnout3_vehicle_sim.h"
#include "burnout3_car_physics.h"
#include "burnout3_gameplay.h"
#include "burnout3_trackmesh.h"
#include "burnout3_collision.h"
#include "burnout3_crash.h"
#include "burnout3_panels.h"      /* PANELS: per-panel damage machine */
#include "burnout3_carcol.h"
#include "burnout3_sfx.h"   /* SFX: event sound system */
#include "burnout3_track_paths.h"
#include "burnout3_start_grid.h"
#include "burnout3_traffic_data.h"
#include "burnout3_traffic_reservations.h"
#include "burnout3_traffic_pool.h"
#include "burnout3_hud.h"
#include "burnout3_particlefx.h"
#include "burnout3_takedown.h"
#include "burnout3_td_rules.h"
#include "burnout3_score_events.h"
#include "burnout3_postfx.h"
#include "burnout3_carfx.h"
#include "burnout3_boostfx.h" /* BOOSTFX: exhaust flame (light type 8) */
#include "burnout3_music.h"
#include "burnout3_ai.h"
#include "burnout3_props.h" /* PROPS: destructible track props */

#define DEG_TO_RAD 0.01745329252f
#define RAD_TO_DEG 57.2957795130823208764f
#define PI 3.14159265358979323846f
#define TAU (PI * 2.0f)

// ---------------------------------------------------------------------------
// AI parameters recovered from the binary (docs/RE_AI.md). The named params
// come from the AI registrar FUN_0016AFD0 (static instance 0x0047A140, cfg
// "../Export/ValueDB/AI/defaults.cfg", same hash pipeline as the car VDB);
// values are the retail Data/vdb.xml tune, compiled defaults in comments.
// The bare constants are immediates read out of the AI racer driver
// FUN_00105340 / traffic driver FUN_00105150; the driver's input formulas
// are execution-verified (tools/validate_gameplay.py, AI driver section).
// ---------------------------------------------------------------------------
#define B3_AI_MAX_LOCK_DEG        10.0f   // "AI/Car/Max lock at 180 x degrees" (dflt 6)
#define B3_AI_TOP_SPEED_MPS       88.0f   // "AI/Car/Top speed mps"             (dflt 80)
#define B3_AI_MIN_SPEED_MPS       20.0f   // "AI/Car/Min speed mps"             (dflt 10)
#define B3_AI_ANGLE_MIN_SPEED_DEG 90.0f   // "AI/Car/Angle you want min spd at" (dflt 38.4)
#define B3_AI_OOR_DECREASE        10.0f   // "AI/Car/Out of range speed decrease rate" (dflt 60)
#define B3_AI_THROTTLE_DEFICIT_MS  1.0f   // FUN_00105340: full throttle above this deficit [C]
#define B3_AI_BRAKE_EXCESS_MS  13.4112f   // FUN_00105340: brake above 30 mph excess
                                          // (DAT_005A39EC/0x5A3A10 <- .data 0x3B1A5C) [C]
#define B3_AI_STUCK_MPH            5.0f   // FUN_00105340 head: "stuck" below 5 mph [C]
#define B3_AI_STUCK_ARM_S          1.0f   // v+0x1578 arm countdown = 1.0 [C]
#define B3_AI_REVERSE_S            2.0f   // v+0x157C reverse timer = 2.0 [C]
#define B3_AI_AVOID_SPEED_10M     26.2f   // "AI/Avoidance/Speed when car is <10m away" (dflt 10)
#define B3_AI_AVOID_SPEED_20M     40.0f   // "AI/Avoidance/Speed when car is <20m away" (dflt 25)
#define B3_AI_AVOID_SPEED_30M     60.0f   // "AI/Avoidance/Speed when car is <30m away" (dflt 40)
// Traffic driver FUN_00105150 (dispatcher FUN_00104D30 route for
// racecar+0x134C == 0) [C]: same steer mapping as the racer AI; throttle
// above 1 m/s deficit, brake above 2.2352 m/s excess (= exactly 5 mph,
// DAT_005A3A20 <- .data 0x3B2330).
#define B3_TRAFFIC_BRAKE_EXCESS_MS 2.2352f

// ============================================================
// Types (original; not recovered from the binary)
// ============================================================

typedef struct { float x, y, z; } Vec3;
typedef struct { float x, y, z, w; } Quat;
typedef struct { float m[4][4]; } Mat4;
typedef struct { float r, g, b, a; } Color;

typedef struct {
    Vec3 pos, vel, acc, rot;
    float speed, max_speed, accel, brake, steer;
    float health, boost, boost_meter;
    int vehicle_id, lap, position;
    float track_progress;
    float prev_progress;      // per-vehicle lap-wrap detection
    int active;
    const VehicleInfo* info;  // roster entry extracted from pveh/, may be NULL
    B3VehicleState sim;       // legacy interface mirror (HUD/audio/AI read
                              // speed/rpm/gear/trans from here; synced from
                              // fsim every frame since the pipeline switch)
    B3VehicleFull fsim;       // THE GAME'S per-frame vehicle pipeline state
                              // (b3_vehicle_step_full: FUN_0011ECF0 +
                              // FUN_0011BE50 main path, validate_port.py
                              // full-pipeline section)
    int fsim_ready;           // 0 -> re-place the full model at pos/rot.y
                              // on the next update (spawn/respawn/recovery)
    B3PhysicsConfig cfg;      // THIS car's physics: defaults + its Data/vdb.xml
                              // overrides (burnout3_car_physics.h)
    const B3CarPhysics* vdb;  // matched override table entry, NULL = fallback
    float body_len;           // from the .bgv header, world units

    // --- gameplay state, driven by the RECOVERED + VERIFIED rules in
    // burnout3_gameplay.h (tools/validate_gameplay.py, 44/44). The glue that
    // decides when a harness collision counts as a slam/crash is original
    // (marked GLUE below); every formula applied to these fields is the
    // game's own.
    B3BoostBar bar;             // boost record (racecar+0x119C layout)
    B3TakedownScore score;      // takedown/BP bookkeeping (score-object subset)
    float slam_time;            // last time this car was slammed (-1 = never)
    unsigned char slam_side;    // veh+0x153C: 1 = hit came from the left
                                // (FUN_00112AC0 @0x112B06 side classifier)
    int   slam_by;              // aggressor slot (racecar+0x16BC), -1 = none
    float pending_td_time;      // attacker: commit pending since (FUN_00197040
    int   pending_td_victim;    //   defers by Race Car Clear Wait), -1 = none
    float crashed_until;        // crash respawn: g_race_time + 5.0 GAME s at
                                // entry (victim+0x240C = dilated clock + 5.0,
                                // FUN_00198E60); 5 game-s = 25 s wall at the
                                // divisor-5 presentation. The release is
                                // driven purely by this game-time stamp.
    float stuck_time;           // AI barrier-recovery timer (harness)
    int   stuck_frames;         // racecar+0x1904 stuck counter (rescue)
    int   offroad_frames;
    float last_brake;           // brake input this frame (corona light byte)
    float ai_wheel_until;       // post-relaunch AI-drive handover (the
                                //   +0x27D8 "AI has the wheel" semantics)
    B3AiWheel aiw;              // RECOVERED (RE_AI 16): racecar+0x19A8 /
                                //   +0x27D8 / +0x245E / +0x1904 / +0x2460 /
                                //   +0x245A / +0x244C and the AI+0x1F0
                                //   route-alternation square wave
    int   aiw_ready;
    Vec3  last_hit_pos;         // debug: last wall contact point
    Vec3  last_hit_n;           //   its push-out normal
    float last_hit_vin;         //   closing speed m/s
    float last_hit_time;        //   race clock (-1 = never)
    B3AiState ai;
    int   ai_ready;
    unsigned short nav_section;
    unsigned short nav_node;
    int   nav_ready;
    unsigned short nav_target_section;
    unsigned short nav_target_node;
    int   nav_target_ready;
    /* racecar+0x245A, the LAST VALID NODE: FUN_001712E0 latches racecar+0x18D0
     * into it every frame the body is within 1 m of the interpolated road
     * surface, and the stuck rescue re-places at that node minus eight
     * (FUN_00170820 @0x001708FA).  The harness carries the section too
     * because its cursor is a (section, node) pair rather than retail's
     * route-global index. */
    unsigned short nav_last_section;
    unsigned short nav_last_node;
    int   nav_last_ready;
    /* AGGRESSION: the recovered attack/slam machine (RE_AI section 14) */
    B3AiAggro     aggro;
    B3AiAggroSpeed aggspd;
    int   aggro_ready;
    float beach_time;           // GLUE no-progress clock -> track reset
    float beach_ref_prog;       // route progress at last advance
    Vec3  stuck_ref;            // GLUE wall-grind detector: last reference
    float stuck_ref_time;       //   position + when it was taken
    int   unstuck_side;         // GLUE: alternating lateral escape side
    float unstuck_until;        // GLUE: aim-offset active until this clock
    float immune_until;         // GLUE post-recovery grace so a parked car is
                                // not endlessly re-wrecked by passing traffic
    unsigned char taken_down_by[8]; // revenge flags (score+0x5B9 semantics)
    float tick_acc;           // fixed-step accumulator for the pipeline

    // --- boost/BP EARN EVENTS (src/burnout3_score_events.c): the
    // score object's air/oncoming/drift/near-miss records + chains.
    B3ScoreEvents sev;
    Vec3  sev_prev_pos;       // last frame's position -> the distance
    int   sev_prev_ok;        //   step FUN_001935F0 feeds the detectors
} Vehicle;

/* ===== AGGRESSION WORLD (src/burnout3_ai.c section 14) ==================
 * One B3AiAggroCar per racer, rebuilt each frame; the machine walks it the
 * way retail walks DAT_0073A1D0[i*0x27E0].  All vectors are in the MODULE's
 * frame (= the rigid body's), i.e. z is negated from the harness's. */
#define B3_AGGRO_MAX 8
static B3AiAggroCar  g_aggro_cars[B3_AGGRO_MAX];
static float         g_aggro_lat[B3_AGGRO_MAX][B3_AGGRO_MAX];
static float         g_aggro_hit[B3_AGGRO_MAX][B3_AGGRO_MAX];
static B3AiAggroWorld g_aggro_world;
static float         g_aggro_track_len = 0.0f;
static int           g_aggro_hit_init = 0;
static float*        g_aggro_cum = NULL;      /* cumulative arc length */
static int           g_aggro_shot_req = 0;   /* screenshot on the next slam */
static int           g_aggro_cam_slot = -1;  /* car currently mid-slam */

static void aggro_world_build(float dt);

// earn-event detectors (defined next to carcol_synth_rb, which the
// near-miss pass needs for the traffic cars' frames)

static void score_events_update(Vehicle* v);
static void score_near_miss_update(Vehicle* v);
/* SCORE-CLASSIFIER: the contact notify carcol_pass() feeds, and the
 * shunt-suppression verdict vehicle_update() hands to the rubbing detector. */
static void score_contact_pair(Vehicle* a, Vehicle* b, int a_traffic,
                               int b_traffic);
static void score_rub_slam(int self, unsigned char* slam);
static void carcol_synth_rb(B3RigidBody* rb, Vec3 pos_gl, float y_origin,
                            float yaw, Vec3 vel_gl);

typedef struct {
    Vec3 pos;
    float yaw, pitch;
} Camera;

typedef struct {
    Vec3* verts;
    int num_verts;
    int num_faces;
    unsigned short* indices;
} Mesh;

typedef struct {
    char name[256];
    Vec3 start_pos;
    Vec3* points;
    int num_points;
    float width;
} Track;

typedef enum { MENU, RACING, CRASHED, FINISHED } GameState;

// ============================================================
// Globals (original; not recovered from the binary)
// ============================================================

static GameState g_state = MENU;
static SDL_Window* g_window = NULL;
static SDL_GLContext g_gl_context = NULL;
static int g_running = 1;
static float g_delta_time = 0.016f;
static float g_tdfx_real_dt = 0.016f;  // TAKEDOWN-FX: undilated frame delta
static float g_cam_fov_deg = 60.0f;    // harness default; recovered in-game
                                       // camera FOV is B3_CAM_FOV = 90 [C]
static Mat4 g_cam_view, g_cam_proj;    // this frame's matrices (tag projection)
static float g_total_time = 0.0f;
static int g_frame_count = 0;

// NOTE: the real game keeps its entity array at 0x004AE728 (stride 0x188,
// active index at 0x004AED45) -- see docs/RE_NOTES.md section 6. These globals
// are NOT that structure; they are this harness's own state.
static int g_game_mode = 0;       // 0=menu, 1=race, 2=crashed, 3=finished
static float g_time_limit = 180.0f; // 3 minute race
static int g_lap_count = 3;
static int g_current_lap = 0;
static float g_race_time = 0.0f;
float g_real_clock_dbg = 0.0f;   /* undilated wall clock, debug only */

// Player vehicle (harness state). The player occupies slot 0 of the array so
// that rendering, collision and position scoring iterate over it like any other
// car; g_player is just a readable alias for that slot.
static Vehicle g_vehicles[8] = {0};
#define g_player (g_vehicles[0])
static int g_num_vehicles = 6;   // real event grid = 6 (Gamedata.bgd)

// Wreck rigid states, one per grid slot (src/burnout3_crash.c: the ported
// FUN_0011AEF0 impulse response + FUN_00123000 crash damping around the
// verified FUN_00109560 integrator; the assembly is GLUE, marked there).
static B3WreckState g_wrecks[8] = {0};

/* CRASH-SHOW H1: what the player's running crash started against.  The
 * HUD's crash ticker composes its first descriptor from this exactly as
 * FUN_0017A6B0 does off the hit entity (burnout3_hud.h's crash block):
 * B3_HUD_HIT_TRAFFIC + the traffic model's base-40 id -> "Into Van",
 * B3_HUD_HIT_RIVAL -> "Into Rival", 0 -> "Into The Wall". */
static int         g_crash_hit_kind = 0;
static const char* g_crash_hit_id   = NULL;
/* CRASH-CINEMA: the RETAIL crashed-path aftertouch state.
 * FUN_00118410 (the crashed input shaper, reached from FUN_0011BE50's
 * crashed branch) publishes two axes on the car:
 *   veh+0x1408 = clamp(dpad_right - dpad_left + lstick_x, -1, 1)
 *                                             FUN_00020E70 @0x001185C6
 *   veh+0x140C = clamp(dpad_up    - dpad_down + lstick_y, -1, 1)
 *                                             FUN_00020F50 @0x001185D3
 * and latches Impact Time on pad+0x84 -- the BOOST button -- at
 * 0x0011889A ([0x0060EA24] = 5, the slow-mo divisor).  Both the wreck
 * steer and the HUD arrow cursor (FUN_0004FCA0) read the same two
 * numbers, so they are published once per frame here. */
static float g_at_h, g_at_v;      /* veh+0x1408 / veh+0x140C          */
static int   g_at_held;           /* pad+0x84 != 0 -> Impact Time      */
/* The camera basis FUN_00118410 unpacks out of veh+0x1410 to make the
 * aftertouch direction screen-relative (FUN_00117520 -> FUN_00013D10,
 * rows 0 and 2 with y flattened).  Published from render_frame. */
static float g_at_cam_fwd[3]   = { 0.0f, 0.0f, 1.0f };
static float g_at_cam_right[3] = { 1.0f, 0.0f, 0.0f };
struct B3CarContact;
static void carcol_wreck_takedown(Vehicle* a, Vehicle* b,
                                  const struct B3CarContact* ct);
/* CRASH-SHOW H1: the particle layer's dither carries (retail keeps one
 * byte per emitter at 0x007547CC+) and its once-per-wreck burst latch. */
/* PARTICLE-2: b3_pfx_wheel keeps the recovered per-wheel gate bytes
 * (wheel+0xA0..0xA2) and dither carries (wheel+0xA4..0xA7) itself now;
 * the harness only has to hand it a stable slot id per wheel. */
static int b3_pfx_wheel_slot(int veh, int wheel) { return veh * 4 + wheel; }
static unsigned char  g_pfx_spark_carry[8];
static int            g_pfx_burst_done[8];

/* SFX-DRIVE: the player's live wall-grind contact, latched by the collision
 * pass and consumed by the per-frame SFX block below.  It drives the body
 * SCRAPE loop pair (CARSCRAPLO + CARSCRAPHI, FUN_001521C0) -- the audio
 * sibling of the grind-spark emitter that sets it.  GLUE: retail keeps six
 * contact slots and crossfades the pair itself; the harness has one player
 * contact and hands the module the weight. */
static int   g_sfx_scrape_hit = 0;
static float g_sfx_scrape_spd = 0.0f;
static Vec3  g_sfx_scrape_pos = {0, 0, 0};

// CRASH TRACE (user-requested diagnostic): while the player's crash
// presentation runs, every frame's context is appended to
// build/crash_trace_NNN.log (unique per crash, like the debug dumps) so a
// live-play crash report carries the whole flight: wreck pose/velocities,
// time dilation, containment interference. Disable with B3_NO_CRASH_TRACE=1.
static FILE* g_crash_trace = NULL;
static int   g_crash_trace_frames = 0;
// TD-RULES: the recovered takedown trigger/attribution state machine
// (src/burnout3_td_rules.c, validate_td_rules.py 315/315).
static B3TdRules g_tdr;
static float     g_tdr_wall[8];   // strongest barrier closing speed this frame
static Vec3      g_tdr_wall_n[8]; //   its push-out normal (head-on gate)
static int       g_tdr_ready = 0;

// Camera (harness state)
static Camera g_camera = {0};

// Vehicle physics config, populated from the values recovered from the binary.
static B3PhysicsConfig g_phys_cfg;

// Real track geometry extracted from static.dat (tools/extract_track.py).
// Unlike everything else drawn here, this IS the game's own data.
static TrackMesh g_real_track = {0};
static int g_have_real_track = 0;

// GL texture per material group of the real track. The images are the game's
// own DXT1/DXT5 textures decoded to PNG by tools/extract_textures.py; only the
// GL plumbing here is original harness code.
static GLuint g_track_tex[TRACKMESH_MAX_GROUPS] = {0};
// Whether a group's texture is a cutout (fence/foliage: mostly-transparent
// alpha) vs opaque (roads/buildings store a reflection mask in alpha, which
// must NOT be alpha-tested). The real game drives this off material flags;
// here it is inferred from the texture's transparent-texel fraction.
static unsigned char g_track_cutout[TRACKMESH_MAX_GROUPS] = {0};
// Whole track baked into one display list (106k tris; immediate mode per
// frame costs ~20ms, the list renders in ~2ms).
static GLuint g_track_list = 0;

// Real vehicle meshes extracted from pveh/*.bgv (tools/extract_bgv.py, layout
// from the game's own relinker -- see BGV_EXTRACTION.md). One display list per
// grid slot; ymin lifts the wheels onto the road. Falls back to boxes.
static GLuint g_car_lists[8] = {0};
static float g_car_ymin[8] = {0};

// Damage-state body variants + wheel sub-meshes (RE_NOTES 13). The .bgv body
// carries per-state records selected by the u16 mask at record+0x18:
// bit0 = intact one-piece car, bit1 = wrecked shell (panel regions absent --
// the verified damage machine's end state: a wreck stamps every panel
// accumulator to 1000 > the 999 detach threshold, FUN_001253C0/FUN_00123000,
// so all panels detach and retire), bit8 = glass (FUN_000300A0 retints).
// Wheels are separate origin-centred meshes drawn each frame at the attach
// matrices stored at .bgv+0xB80 [C: FUN_0012FEE0 copies them into the damage
// ctx the draw path consumes].
static GLuint g_car_intact_lists[8] = {0};   // mask bit0 + lights
static GLuint g_car_shell_lists[8] = {0};    // mask bit1 + lights
static GLuint g_car_glass_lists[8] = {0};    // mask bit8 (built untinted)
static GLuint g_car_wheel_lists[8] = {0};    // wheel mesh, origin-centred
// Motion-blur wheel variants: retail's draw picks S+0x1C/0x20/0x24 by
// |wheel spin| -- slot 7 below 25 rad/s, slot 8 above, slot 9 above 50
// [C, extract_bgv header]. Always drawing slot 7 made fast wheels read
// as slowly rotating (wagon-wheel strobe; user report).
static GLuint g_car_wheel_blur8[8] = {0};
static GLuint g_car_wheel_blur9[8] = {0};
/* PANELS: the damage panels of each roster car.  The .bgv part table's
 * slots 1..numBodyParts are the detachable bodywork (doors / front / rear /
 * bonnet / boot), extracted PIVOT-LOCAL to build/cars/parts/<car>/
 * panel<k>_kind<d>.obj with their placement matrices in <car>.panels
 * (.bgv+0xD00, [C]).  The mask-bit1 shell is the car MINUS these, so a
 * crashed car draws shell + whatever is still attached -- which is
 * EVERYTHING at the crash entry, because the ordinary entry FUN_00115130
 * stamps FUN_001253C0(1) (crumple), not (0) (detach-all).  See
 * src/burnout3_panels.h for the whole recovered chain. */
static GLuint g_car_panel_lists[8][B3_PANEL_MAX] = {{0}};
static int    g_car_panel_count[8] = {0};
static int    g_car_panel_kind[8][B3_PANEL_MAX] = {{0}};
static float  g_car_panel_pos[8][B3_PANEL_MAX][3];  // GAME space (.bgv+0xD00)
static B3PanelSet g_panels[8];
static float  g_car_wheel_pos[8][6][3];      // attach pos (loader Z-flip applied)
static int    g_car_wheel_mirror[8][6];      // right row x sign: -1 = mirrored
static int    g_car_wheel_front[8][6];       // GLUE: steers if on the +Z axle
static int    g_car_wheel_count[8] = {0};
static float  g_car_wheel_radius[8] = {0};   // .bgv+0x18, per car
static float  g_car_wheel_spin[8] = {0};     // visual spin, b3_wheel_spin_update
static float  g_car_wheel_omega[8] = {0};
static float  g_car_ext[8][4] = {{0}};       // .bgv+0xE80 half extents [C]
static float  g_car_cen[8][4] = {{0}};       // .bgv+0xE90 center offset [C]
// CAR-VS-CAR: the real per-car convex collision hull (.bgv +0x1060,
// build/cars/*.hull from tools/emulate_carcol.py --extract-hulls) and
// the B3CarBody views the recovered chain in burnout3_carcol.c consumes.
static B3CarHull g_car_hull[8];
static int       g_car_hull_ok[8] = {0};

// Forward: pipeline ground-query wrapper (defined with the track helpers)
static int harness_ground_probe(float x, float y, float z,
                                float* out_height, float out_normal[3]);

// Track data
static Track g_track = {0};
static Mesh g_road_mesh = {0};
static Mesh g_ground_mesh = {0};

typedef struct { unsigned short point_a, point_b; } B3RtNavPair;
typedef struct {
    unsigned int pair_base, link_base;
    unsigned short node_count, flags;
} B3RtNavSection;
typedef struct {
    unsigned short anchor, aux;
    unsigned char forward_section, reverse_section;
    unsigned short forward_node, reverse_node;
} B3RtNavLink;
typedef struct {
    unsigned short node_a, node_b, node_c, speed;
    unsigned char section, byte9, flags, byte11;
} B3RtNavPlan;
typedef struct {
    Vec3* points;
    B3RtNavSection* sections;
    B3RtNavPair* pairs;
    B3RtNavLink* links;
    B3RtNavPlan* plans;
    unsigned int point_count, section_count, pair_count, link_count, plan_count;
    int loaded;
} B3RtNavData;

_Static_assert(sizeof(B3RtNavPair) == 4, "route.bin nav pair layout");
_Static_assert(sizeof(B3RtNavSection) == 12, "route.bin nav section layout");
_Static_assert(sizeof(B3RtNavLink) == 10, "route.bin nav link layout");
_Static_assert(sizeof(B3RtNavPlan) == 12, "route.bin nav plan layout");

static B3RtNavData g_nav = {0};

// Input state
static int g_keys[SDL_NUM_SCANCODES] = {0};

// ---- Pause + user mixer (Escape/P or pad Start while racing). Gains are
// live-tunable via mouse sliders on the pause screen and persist across
// runs in build/mixer.cfg. g_mix = {engine, sfx master, music master}.
static int g_paused = 0;
static float g_mix[3] = { 0.33f, 0.24f, 0.78f };
static const float g_mix_max[3] = { 0.80f, 0.80f, 1.50f };  // slider tops
static int g_mix_drag = -1;

static void mixer_apply(void) {
    b3_sfx_set_master(g_mix[1]);
    b3_music_set_master(g_mix[2]);
    /* engine gain (g_mix[0]) is read directly by the audio callback */
}
static void mixer_save(void) {
    FILE* f = fopen("build/mixer.cfg", "w");
    if (!f) return;
    fprintf(f, "engine %.4f\nsfx %.4f\nmusic %.4f\n",
            g_mix[0], g_mix[1], g_mix[2]);
    fclose(f);
}
static void mixer_load(void) {
    FILE* f = fopen("build/mixer.cfg", "r");
    if (!f) return;
    char k[32];
    float v;
    while (fscanf(f, "%31s %f", k, &v) == 2) {
        if (!strcmp(k, "engine")) g_mix[0] = v;
        else if (!strcmp(k, "sfx")) g_mix[1] = v;
        else if (!strcmp(k, "music")) g_mix[2] = v;
    }
    fclose(f);
    for (int i = 0; i < 3; i++) {
        if (g_mix[i] < 0.0f) g_mix[i] = 0.0f;
        if (g_mix[i] > g_mix_max[i]) g_mix[i] = g_mix_max[i];
    }
}
// Window px -> the HUD's 640x480 virtual space; slider row hit or -1.
static int mixer_hit(int mx, int my, float* out_frac) {
    int ww, wh;
    SDL_GetWindowSize(g_window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return -1;
    float vx = (float)mx * 640.0f / (float)ww;
    float vy = (float)my * 480.0f / (float)wh;
    for (int i = 0; i < 3; i++) {
        float y = B3HUD_MIX_Y0 + i * B3HUD_MIX_DY;
        if (vy >= y - 8.0f && vy <= y + B3HUD_MIX_H + 8.0f
            && vx >= B3HUD_MIX_BAR_X - 12.0f
            && vx <= B3HUD_MIX_BAR_X + B3HUD_MIX_W + 12.0f) {
            float f = (vx - B3HUD_MIX_BAR_X) / B3HUD_MIX_W;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            *out_frac = f;
            return i;
        }
    }
    return -1;
}

// Full race restart (R from CRASHED/FINISHED, and the pause-menu button).
static void race_restart(void);

// ---- Xbox 360 / SDL game controller (keyboard stays fully live; the
// strongest input wins at each merge site). Retail Xbox layout: A =
// accelerate, B = brake, X = boost, triggers analog, left stick steer.
static SDL_GameController* g_pad = NULL;

static float pad_axis(SDL_GameControllerAxis a, float dead) {
    if (!g_pad) return 0.0f;
    float v = SDL_GameControllerGetAxis(g_pad, a) / 32767.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    float m = fabsf(v);
    if (m <= dead) return 0.0f;
    return (m - dead) / (1.0f - dead) * (v < 0.0f ? -1.0f : 1.0f);
}
static int pad_btn(SDL_GameControllerButton b) {
    return g_pad && SDL_GameControllerGetButton(g_pad, b);
}
static void pad_open_first(void) {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        g_pad = SDL_GameControllerOpen(i);
        if (g_pad) {
            printf("[Burnout3] controller: %s\n",
                   SDL_GameControllerName(g_pad));
            return;
        }
    }
}
static int g_prev_keys[SDL_NUM_SCANCODES] = {0};

// Audio
static SDL_AudioDeviceID g_audio_dev = 0;
static Uint8* g_audio_buf = NULL;
static int g_audio_len = 0;

// T-key gamestate dump (user-facing debug handoff)
static int g_debug_dump_req = 0;
static void debug_dump(void);
static void b3_write_gamestate(FILE* f, int n);
static void drive_log_tick(void);

// ============================================================
// Retail string table + callout sign art (takedown-FX plumbing)
// ============================================================

// Data/Globalus.bin: u32 offset table at +0x10, UTF-16LE zero-terminated
// strings (format verified against the image by tools/validate_takedown.py
// section 9). Loader/ASCII fold are GLUE; the data is the retail file.
static unsigned char* g_globalus = NULL;
static long g_globalus_len = 0;

static void globalus_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("[Burnout3] no %s (callout text off)\n", path); return; }
    fseek(f, 0, SEEK_END);
    g_globalus_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_globalus = malloc((size_t)g_globalus_len);
    if (!g_globalus || fread(g_globalus, 1, (size_t)g_globalus_len, f)
                       != (size_t)g_globalus_len) {
        free(g_globalus); g_globalus = NULL; g_globalus_len = 0;
    }
    fclose(f);
}

static const char* globalus_string(int idx) {
    static char buf[96];
    if (!g_globalus || idx < 0) return NULL;
    long tab = 0x10 + (long)idx * 4;
    if (tab + 4 > g_globalus_len) return NULL;
    unsigned long off = (unsigned long)g_globalus[tab]
                      | ((unsigned long)g_globalus[tab + 1] << 8)
                      | ((unsigned long)g_globalus[tab + 2] << 16)
                      | ((unsigned long)g_globalus[tab + 3] << 24);
    int n = 0;
    while ((long)off + 1 < g_globalus_len && n < 95) {
        unsigned cu = (unsigned)g_globalus[off]
                    | ((unsigned)g_globalus[off + 1] << 8);
        if (!cu) break;
        buf[n++] = (cu < 0x20 || cu > 0x7E) ? '?' : (char)cu;
        off += 2;
    }
    buf[n] = 0;
    return buf;
}

// Sign roundel textures by DAT_004608F0[] index (names per the recovered
// FUN_0004DD00 binding table; art extracted from Data/Global.txd).
static GLuint tdfx_sign_texture(int sign) {
    static GLuint cache[18];
    static int tried[18];
    static const char* names[18] = {
        [B3_TDFX_SIGN_FLAME]    = "hud_sign_flame",
        [B3_TDFX_SIGN_SKULL]    = "hud_sign_skull",
        [B3_TDFX_SIGN_TAKEDOWN] = "hud_signs_td",
    };
    if (sign < 0 || sign >= 18 || !names[sign]) return 0;
    if (!tried[sign]) {
        char path[256];
        snprintf(path, sizeof path, "build/frontend/%s.png", names[sign]);
        cache[sign] = b3_hud_load_texture(path);
        tried[sign] = 1;
    }
    return cache[sign];
}

// ============================================================
// Math Functions
// ============================================================

static Vec3 vec3_cross(Vec3 a, Vec3 b) { return (Vec3){a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; }
static Vec3 vec3_add(Vec3 a, Vec3 b) { return (Vec3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static Vec3 vec3_sub(Vec3 a, Vec3 b) { return (Vec3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static Vec3 vec3_scale(float s, Vec3 v) { return (Vec3){v.x*s, v.y*s, v.z*s}; }
static Vec3 vec3_normalize(Vec3 v) {
    float len = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    if (len < 0.0001f) return (Vec3){0, 1, 0};
    return (Vec3){v.x/len, v.y/len, v.z/len};
}
static float vec3_dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float vec3_len(Vec3 v) { return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z); }

static Mat4 mat4_identity(void) {
    Mat4 m = {0};
    for (int i = 0; i < 4; i++) m.m[i][i] = 1.0f;
    return m;
}

static Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r = {0};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

static Mat4 mat4_lookat(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = vec3_normalize(vec3_sub(target, eye));
    Vec3 s = vec3_normalize(vec3_cross(f, up));
    Vec3 u = vec3_cross(s, f);
    
    // Laid out exactly as glLoadMatrixf consumes it (column-major, m[col][row]),
    // matching gluLookAt: rows of the rotation are s, u, -f; translation is
    // (-s.eye, -u.eye, +f.eye).
    Mat4 m = mat4_identity();
    m.m[0][0] = s.x;  m.m[1][0] = s.y;  m.m[2][0] = s.z;
    m.m[0][1] = u.x;  m.m[1][1] = u.y;  m.m[2][1] = u.z;
    m.m[0][2] = -f.x; m.m[1][2] = -f.y; m.m[2][2] = -f.z;
    m.m[3][0] = -vec3_dot(s, eye);
    m.m[3][1] = -vec3_dot(u, eye);
    m.m[3][2] = vec3_dot(f, eye);
    return m;
}

static Mat4 mat4_perspective(float fov, float aspect, float near, float far) {
    Mat4 m = mat4_identity();
    float tan_half = tanf(fov * 0.5f);
    m.m[0][0] = 1.0f / (aspect * tan_half);
    m.m[1][1] = 1.0f / tan_half;
    m.m[2][2] = -(far + near) / (far - near);
    m.m[2][3] = -1.0f;
    m.m[3][2] = -(2.0f * far * near) / (far - near);
    m.m[3][3] = 0.0f;
    return m;
}

static Vec3 transform_vec3(Mat4 m, Vec3 v) {
    Vec3 r;
    float w = m.m[3][0]*v.x + m.m[3][1]*v.y + m.m[3][2]*v.z + m.m[3][3];
    r.x = (m.m[0][0]*v.x + m.m[0][1]*v.y + m.m[0][2]*v.z + m.m[0][3]) / w;
    r.y = (m.m[1][0]*v.x + m.m[1][1]*v.y + m.m[1][2]*v.z + m.m[1][3]) / w;
    r.z = (m.m[2][0]*v.x + m.m[2][1]*v.y + m.m[2][2]*v.z + m.m[2][3]) / w;
    return r;
}

// ============================================================
// Track Generation (based on game's track data)
// ============================================================

// Path data from Gamedata.bgd, reflected into GL right-handed space (negate
// Z) to match the same flip trackmesh_load applies to all mesh data. The
// game's world is D3D left-handed; without one uniform reflection the GL
// render is a mirror image (billboards read backwards).
// Race route: the road-boundary corridor's midline from Gamedata.bgd. The
// REAL start grid sits 2.6m off this midline with matching direction (dot
// 0.999) -- vs 24m off the 1029-point loop, which also cuts through fenced
// roundabout geometry, so that loop is NOT the driving line (its role stays
// open; docs/RE_BGD.md). Array rotated so index 0 = the grid (start/finish
// at progress 0); its two section seams (127m on the start straight, 30m)
// become straight bridges mid-route.
#define ROUTE_COUNT B3_WALL_A_COUNT
// Spawn index on the route: DATA-DRIVEN from the track's own start-grid
// record (the generated header carries it per track; the old literal 298
// was AS/C1_V1-only and even that came from the stale grid record).
#define ROUTE_START B3_ROUTE_START
static float g_cl[ROUTE_COUNT][3];          // route midline (AI/progress/height)
static float g_wa[B3_WALL_A_COUNT][3];      // road-edge strands (debug overlay)
static float g_wb[B3_WALL_B_COUNT][3];

static void init_paths(void) {
    for (int i = 0; i < B3_WALL_A_COUNT; i++) {
        g_wa[i][0] = B3_WALL_A[i][0];
        g_wa[i][1] = B3_WALL_A[i][1];
        g_wa[i][2] = -B3_WALL_A[i][2];
    }
    for (int i = 0; i < B3_WALL_B_COUNT; i++) {
        g_wb[i][0] = B3_WALL_B[i][0];
        g_wb[i][1] = B3_WALL_B[i][1];
        g_wb[i][2] = -B3_WALL_B[i][2];
    }
    for (int i = 0; i < ROUTE_COUNT; i++) {
        int j = (i + ROUTE_START) % ROUTE_COUNT;
        g_cl[i][0] = (g_wa[j][0] + g_wb[j][0]) * 0.5f;
        g_cl[i][1] = (g_wa[j][1] + g_wb[j][1]) * 0.5f;
        g_cl[i][2] = (g_wa[j][2] + g_wb[j][2]) * 0.5f;
    }
}

static void nav_free(void) {
    free(g_nav.points);
    free(g_nav.sections);
    free(g_nav.pairs);
    free(g_nav.links);
    free(g_nav.plans);
    memset(&g_nav, 0, sizeof(g_nav));
}

static int nav_read(FILE* file, void* dst, size_t size, size_t count) {
    return count == 0 || fread(dst, size, count, file) == count;
}

static void nav_load(void) {
    struct {
        char magic[4];
        unsigned int version, wall_count, center_count, oncoming_count;
        unsigned int route_count, route_start;
        float lap_length;
        unsigned int flags, strip_pairs;
    } header;
    unsigned int counts[5];
    const char* track = getenv("B3_TRACK");
    char path[256];
    FILE* file;
    size_t geometry_count;

    if (!track) track = getenv("B3_POSTFX_TRACK");
    if (!track) track = "US_C3_V1";
    snprintf(path, sizeof(path), "build/tracks/%s/route.bin", track);
    file = fopen(path, "rb");
    if (!file) return;
    if (!nav_read(file, &header, sizeof(header), 1)
        || memcmp(header.magic, "B3RT", 4) != 0 || header.version != 3) {
        fclose(file);
        return;
    }
    geometry_count = (size_t)header.wall_count * 2 + header.center_count
                   + header.oncoming_count + header.route_count
                   + (size_t)header.strip_pairs * 2;
    if (fseek(file, (long)(geometry_count * 12), SEEK_CUR) != 0
        || !nav_read(file, counts, sizeof(counts[0]), 5)
        || counts[0] == 0 || counts[1] == 0
        || counts[2] == 0 || counts[3] == 0) {
        fclose(file);
        return;
    }
    nav_free();
    g_nav.points = calloc(counts[0], sizeof(*g_nav.points));
    g_nav.sections = calloc(counts[1], sizeof(*g_nav.sections));
    g_nav.pairs = calloc(counts[2], sizeof(*g_nav.pairs));
    g_nav.links = calloc(counts[3], sizeof(*g_nav.links));
    g_nav.plans = calloc(counts[4], sizeof(*g_nav.plans));
    if (!g_nav.points || !g_nav.sections || !g_nav.pairs || !g_nav.links
        || (counts[4] && !g_nav.plans)
        || !nav_read(file, g_nav.points, sizeof(*g_nav.points), counts[0])
        || !nav_read(file, g_nav.sections, sizeof(*g_nav.sections), counts[1])
        || !nav_read(file, g_nav.pairs, sizeof(*g_nav.pairs), counts[2])
        || !nav_read(file, g_nav.links, sizeof(*g_nav.links), counts[3])
        || !nav_read(file, g_nav.plans, sizeof(*g_nav.plans), counts[4])) {
        fclose(file);
        nav_free();
        return;
    }
    fclose(file);
    for (unsigned int section = 0; section < counts[1]; section++) {
        const B3RtNavSection* row = &g_nav.sections[section];
        if (row->pair_base + row->node_count + 1 > counts[2]
            || row->link_base + row->node_count > counts[3]) {
            nav_free();
            return;
        }
    }
    for (unsigned int pair = 0; pair < counts[2]; pair++) {
        if (g_nav.pairs[pair].point_a >= counts[0]
            || g_nav.pairs[pair].point_b >= counts[0]) {
            nav_free();
            return;
        }
    }
    for (unsigned int link = 0; link < counts[3]; link++) {
        const B3RtNavLink* edge = &g_nav.links[link];
        if ((edge->forward_section != 0xff
             && edge->forward_section >= counts[1])
            || (edge->reverse_section != 0xff
                && edge->reverse_section >= counts[1])) {
            nav_free();
            return;
        }
    }
    g_nav.point_count = counts[0];
    g_nav.section_count = counts[1];
    g_nav.pair_count = counts[2];
    g_nav.link_count = counts[3];
    g_nav.plan_count = counts[4];
    g_nav.loaded = 1;
    printf("[Burnout3] retail nav: %u points, %u sections, %u plans\n",
           g_nav.point_count, g_nav.section_count, g_nav.plan_count);
}

static int nav_state_valid(unsigned int section, unsigned int node) {
    return g_nav.loaded && section < g_nav.section_count
        && node < g_nav.sections[section].node_count;
}

static Vec3 nav_point(unsigned short point_id) {
    return g_nav.points[point_id];
}

static Vec3 nav_midpoint(unsigned int section, unsigned int node) {
    const B3RtNavSection* row = &g_nav.sections[section];
    const B3RtNavPair* pair = &g_nav.pairs[row->pair_base + node];
    Vec3 a = nav_point(pair->point_a);
    Vec3 b = nav_point(pair->point_b);
    return (Vec3){(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f,
                  (a.z + b.z) * 0.5f};
}

static float nav_edge_dot(Vec3 point, Vec3 start, Vec3 end) {
    float dx = point.x - start.x, dz = point.z - start.z;
    float ex = end.x - start.x, ez = end.z - start.z;
    return -dx * ez + dz * ex;
}

static unsigned int nav_step_flags(unsigned int section, unsigned int node,
                                   Vec3 pos) {
    const B3RtNavSection* row = &g_nav.sections[section];
    const B3RtNavPair* pairs = &g_nav.pairs[row->pair_base + node];
    Vec3 p0 = nav_point(pairs[0].point_a);
    Vec3 p1 = nav_point(pairs[0].point_b);
    Vec3 p2 = nav_point(pairs[1].point_a);
    Vec3 p3 = nav_point(pairs[1].point_b);
    unsigned int flags = 0;
    if (pairs[1].point_a != pairs[1].point_b
        && nav_edge_dot(pos, p2, p3) < 0.0f) flags |= 4;
    if (nav_edge_dot(pos, p0, p2) < 0.0f) flags |= 1;
    if (nav_edge_dot(pos, p1, p3) > 0.0f) flags |= 2;
    if (pairs[0].point_a != pairs[0].point_b
        && nav_edge_dot(pos, p0, p1) > 0.0f) flags |= 8;
    return flags;
}

static int nav_nearest(Vec3 pos, unsigned int* out_section,
                       unsigned int* out_node) {
    float best = 1e30f;
    int found = 0;
    for (unsigned int section = 0; section < g_nav.section_count; section++) {
        const B3RtNavSection* row = &g_nav.sections[section];
        for (unsigned int node = 0; node < row->node_count; node++) {
            const B3RtNavPair* pairs = &g_nav.pairs[row->pair_base + node];
            Vec3 point[4] = {nav_point(pairs[0].point_a),
                             nav_point(pairs[0].point_b),
                             nav_point(pairs[1].point_a),
                             nav_point(pairs[1].point_b)};
            Vec3 center = {0, 0, 0};
            for (int i = 0; i < 4; i++) {
                center.x += point[i].x * 0.25f;
                center.y += point[i].y * 0.25f;
                center.z += point[i].z * 0.25f;
            }
            float dx = center.x - pos.x, dy = center.y - pos.y;
            float dz = center.z - pos.z;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best) {
                best = d2;
                *out_section = section;
                *out_node = node;
                found = 1;
            }
        }
    }
    return found;
}

static float nav_node_dist2(unsigned int section, unsigned int node, Vec3 pos) {
    if (!nav_state_valid(section, node)) return 1e30f;
    Vec3 c = nav_midpoint(section, node);
    float dx = c.x - pos.x, dy = c.y - pos.y, dz = c.z - pos.z;
    return dx * dx + dy * dy + dz * dz;
}

static unsigned int nav_node_type(unsigned int section, unsigned int node) {
    const B3RtNavSection* row = &g_nav.sections[section];
    return (g_nav.links[row->link_base + node].aux >> 8) & 7u;
}

static int nav_nearest_non_type5(Vec3 pos, unsigned int* out_section,
                                 unsigned int* out_node) {
    float best = 1e30f;
    int found = 0;
    for (unsigned int section = 0; section < g_nav.section_count; section++) {
        const B3RtNavSection* row = &g_nav.sections[section];
        for (unsigned int node = 0; node < row->node_count; node++) {
            if (nav_node_type(section, node) == 5) continue;
            Vec3 center = nav_midpoint(section, node);
            float dx = center.x - pos.x, dy = center.y - pos.y;
            float dz = center.z - pos.z;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best) {
                best = d2;
                *out_section = section;
                *out_node = node;
                found = 1;
            }
        }
    }
    return found;
}

/* FUN_00174EE0: take a local signed node step.  At either end of a
 * non-looping section, retail tries its forward link first and then its
 * reverse link; the signed step does not select which terminal link wins. */
static int nav_walk_local_step(unsigned int* section, unsigned int* node,
                               int delta) {
    const B3RtNavSection* row = &g_nav.sections[*section];
    int candidate = (int)*node + delta;
    if (candidate >= 0 && candidate < row->node_count) {
        *node = (unsigned int)candidate;
        return 1;
    }
    if (row->flags & 0xff) {
        *node = (unsigned int)((candidate + row->node_count) % row->node_count);
        return 1;
    }
    const B3RtNavLink* link = &g_nav.links[row->link_base + *node];
    if (nav_state_valid(link->forward_section, link->forward_node)) {
        *section = link->forward_section;
        *node = link->forward_node;
        return 1;
    }
    if (nav_state_valid(link->reverse_section, link->reverse_node)) {
        *section = link->reverse_section;
        *node = link->reverse_node;
        return 1;
    }
    return 0;
}

/* FUN_00175570's bit-1/bit-2 transition path: unlike the bit-4/bit-8
 * local step, these request one specific link and have no alternate-link
 * fallback. */
static int nav_walk_link(unsigned int* section, unsigned int* node,
                         int reverse) {
    const B3RtNavSection* row = &g_nav.sections[*section];
    const B3RtNavLink* link = &g_nav.links[row->link_base + *node];
    unsigned int next_section = reverse ? link->reverse_section
                                        : link->forward_section;
    unsigned int next_node = reverse ? link->reverse_node : link->forward_node;
    if (!nav_state_valid(next_section, next_node)) return 0;
    *section = next_section;
    *node = next_node;
    return 1;
}

static void nav_walk(unsigned int* section, unsigned int* node, Vec3 pos) {
    int mode = 0x10;
    for (int steps = 0; steps < 127; steps++) {
        if (!nav_state_valid(*section, *node)) return;
        unsigned int flags = nav_step_flags(*section, *node, pos);
        if (flags == 0) return;
        if (flags & 4) {
            if (mode == 8) return;
            if (!nav_walk_local_step(section, node, 1)) return;
            mode = 4;
        } else if (flags & 8) {
            if (mode == 4) return;
            if (!nav_walk_local_step(section, node, -1)) return;
            mode = 8;
        } else if (flags & 1) {
            if (mode == 2) return;
            if (!nav_walk_link(section, node, 0)) return;
            mode = 1;
        } else if (flags & 2) {
            if (mode == 1) return;
            if (!nav_walk_link(section, node, 1)) return;
            mode = 2;
        } else {
            return;
        }
    }
}

static Vec3 nav_forward(unsigned int section, unsigned int node);

/* FUN_00178310 under FUN_00176290's reset state: racecar+0x1920 is nonzero,
 * type-4 selection is enabled, and type-1/type-3 selection is not.
 * Bit 2 marks a clamped open-row lookahead; bit 4 rejects a type-5 start.
 * The wider state machine that changes these reset fields is still separate. */
/* AI+0x1FC.  FUN_00175A10 starts it at 2 and no writer is mapped, so it is
 * carried as the initializer's constant; it only ever gates whether type-1
 * and type-3 nodes can contribute at all. */
#define B3_NAV_STATE_1FC  2

static int nav_span_type_ok(unsigned int type, int route_alt, int slamming) {
    /* FUN_00178310 under the racing-mode gate racecar+0x1920 != 0:
     *   type 4      contributes bit 1 while AI+0x1F1 == 0
     *   type 1 / 3  contribute bit 1 only when AI+0x1F0 == 0 && AI+0x1FC != 0
     * AI+0x1F1 is the aggression machine's slam byte (state 4 sets it,
     * FUN_00179760 clears it) and AI+0x1F0 is FUN_00170820's 50 s / 50 s
     * square wave -- so retail's junction policy is not static: type-1/type-3
     * options open for fifty seconds out of every hundred, and every option
     * closes while the car is committed to a slam. */
    if (type == 4) return !slamming;
    if (type == 1 || type == 3)
        return route_alt == 0 && B3_NAV_STATE_1FC != 0;
    return 0;
}

static unsigned int nav_target_span_mask(unsigned int section,
                                         unsigned int node, int route_alt,
                                         int slamming) {
    const B3RtNavSection* row = &g_nav.sections[section];
    unsigned int end = node + 8;
    unsigned int mask = 0;
    if (nav_node_type(section, node) == 5) return 4;
    if (end >= row->node_count) {
        if (row->flags & 0xff) end -= row->node_count;
        else { end = row->node_count - 1; mask = 2; }
    }
    if (node <= end) {
        for (unsigned int scan = node; scan <= end; scan++)
            if (nav_span_type_ok(nav_node_type(section, scan), route_alt,
                                 slamming)) mask |= 1;
    } else {
        for (unsigned int scan = node; scan < row->node_count; scan++)
            if (nav_span_type_ok(nav_node_type(section, scan), route_alt,
                                 slamming)) mask |= 1;
        for (unsigned int scan = 0; scan <= end; scan++)
            if (nav_span_type_ok(nav_node_type(section, scan), route_alt,
                                 slamming)) mask |= 1;
    }
    return mask;
}

/* FUN_00176290's branch probe after its eight-node selector span reports a
 * route choice: follow forward successors first, then reverse successors,
 * accepting the first non-type-5 destination whose selector span is clear. */
static int nav_target_successor(unsigned int section, unsigned int node,
                                unsigned int* out_section,
                                unsigned int* out_node, int route_alt,
                                int slamming) {
    for (int reverse = 0; reverse <= 1; reverse++) {
        unsigned int probe_section = section, probe_node = node;
        for (int steps = 0; steps < 127; steps++) {
            const B3RtNavSection* row = &g_nav.sections[probe_section];
            const B3RtNavLink* link = &g_nav.links[row->link_base + probe_node];
            unsigned int next_section = reverse ? link->reverse_section
                                                : link->forward_section;
            unsigned int next_node = reverse ? link->reverse_node
                                              : link->forward_node;
            if (!nav_state_valid(next_section, next_node)
                || nav_node_type(next_section, next_node) == 5)
                break;
            if (nav_target_span_mask(next_section, next_node, route_alt,
                                     slamming) == 0) {
                *out_section = next_section;
                *out_node = next_node;
                return 1;
            }
            probe_section = next_section;
            probe_node = next_node;
        }
    }
    return 0;
}

/* FUN_00176290's initial type-5 branch handling checks each direct successor
 * with FUN_00173C60(count=1).  If both pass, FUN_001746F0 computes
 * up x FUN_00174740(nav-forward), and the following COMISS chooses forward
 * exactly when raw-game forward.z is positive (GL forward.z is negative). */
static int nav_target_initial_successor(unsigned int section, unsigned int node,
                                        unsigned int* out_section,
                                        unsigned int* out_node) {
    const B3RtNavSection* row = &g_nav.sections[section];
    const B3RtNavLink* link = &g_nav.links[row->link_base + node];
    int forward_ok = nav_state_valid(link->forward_section, link->forward_node)
                  && nav_node_type(link->forward_section, link->forward_node) != 5;
    int reverse_ok = nav_state_valid(link->reverse_section, link->reverse_node)
                  && nav_node_type(link->reverse_section, link->reverse_node) != 5;
    if (!forward_ok && !reverse_ok) return 0;
    if (forward_ok && (!reverse_ok || nav_forward(section, node).z < 0.0f)) {
        *out_section = link->forward_section;
        *out_node = link->forward_node;
    } else {
        *out_section = link->reverse_section;
        *out_node = link->reverse_node;
    }
    return 1;
}

static int nav_target_update(Vehicle* vehicle, int route_alt, int slamming) {
    unsigned int section = vehicle->nav_target_section;
    unsigned int node = vehicle->nav_target_node;
    if (!nav_state_valid(vehicle->nav_section, vehicle->nav_node)) return 0;
    if (!vehicle->nav_target_ready || !nav_state_valid(section, node)) {
        section = vehicle->nav_section;
        node = vehicle->nav_node;
        if (nav_node_type(section, node) == 5
            && !nav_target_initial_successor(section, node, &section, &node)
            && !nav_nearest_non_type5(vehicle->pos, &section, &node))
            return 0;
        vehicle->nav_target_ready = 1;
    }
    if (nav_target_span_mask(section, node, route_alt, slamming) != 0)
        (void)nav_target_successor(section, node, &section, &node, route_alt,
                                   slamming);
    nav_walk(&section, &node, vehicle->pos);
    vehicle->nav_target_section = (unsigned short)section;
    vehicle->nav_target_node = (unsigned short)node;
    return 1;
}

static int nav_update_vehicle(Vehicle* vehicle) {
    static int trace = -1;
    unsigned int section = vehicle->nav_section;
    unsigned int node = vehicle->nav_node;
    if (!g_nav.loaded) return 0;
    if (!vehicle->nav_ready || !nav_state_valid(section, node)) {
        if (!nav_nearest(vehicle->pos, &section, &node)) return 0;
        vehicle->nav_ready = 1;
    } else {
        Vec3 center = nav_midpoint(section, node);
        float dx = center.x - vehicle->pos.x, dz = center.z - vehicle->pos.z;
        if (dx * dx + dz * dz > 2500.0f
            && !nav_nearest(vehicle->pos, &section, &node)) return 0;
    }
    float before_d2 = nav_node_dist2(section, node, vehicle->pos);
    nav_walk(&section, &node, vehicle->pos);
    /* FUN_00174960 re-seeds through FUN_00174CF0 whenever its cursor is
     * missing or stale; retail's staleness is structural (`section == 0 ||
     * node == 0xFFFF`) because its walker cannot lose the car.  The harness's
     * can: US_C3's graph is five PARALLEL 1013-node lane sections plus three
     * junction sections, and `nav_step_flags` classifies in XZ only, so a car
     * knocked into another lane -- or driving under an elevated section --
     * keeps a cursor that is metres above it or a lane across from it, and
     * the ribbon walk has no link to cross back.  Re-seed on a POSITIVE
     * staleness test (25 m in 3D, so the vertical separation of stacked
     * carriageways counts) and re-walk from there.  Without this the
     * FUN_001712E0 watchdog measures the car against the wrong ribbon and
     * re-places it once a second for ever.  GLUE test, retail action. */
    /* Reject a RUNAWAY walk.  FUN_00175570 is bounded only by its 128-step
     * guard, which is enough in retail because the car is always inside its
     * own ribbon and the classifier stops after one or two steps.  The
     * harness's car is not: US_C3's graph is five PARALLEL 1013-node lane
     * sections plus three junction sections, `nav_step_flags` classifies in
     * XZ only, and a car that is a lane across (or under an elevated
     * section) is inside NO cell of its own row -- so the walker takes all
     * 128 steps and lands the cursor up to ~800 m away.  Measured: cars
     * carrying a cursor 193 m ahead, whose ribbon was 13 m above them, which
     * made FUN_001712E0 re-place them once a second for the whole race.
     * Retail cannot produce this state, so there is nothing to port: reject
     * the result and re-seed the way FUN_00174960 does (FUN_00174CF0's
     * closest-centroid search), but over a WINDOW around the previous cursor
     * rather than the whole graph, which is the same answer for a fraction
     * of the cost. */
    if (nav_node_dist2(section, node, vehicle->pos) > before_d2
        && nav_node_dist2(section, node, vehicle->pos) > 625.0f) {
        section = vehicle->nav_section;
        node = vehicle->nav_node;
    }
    /* Re-seed with the EXHAUSTIVE closest-centroid search, exactly as
     * FUN_00174CF0 does -- a windowed form is wrong here, because the node
     * INDEX is only meaningful inside one section: US_C3's graph is five
     * 1013-node lane rows plus three short junction rows (89 / 791 / 60
     * nodes), so a cursor that has slipped onto junction row 0 at index 69
     * can never see the correct index-800 node in a lane row.  Measured: a
     * whole grid carrying a section-0 cursor 0.65 of a lap away from itself,
     * which every crash recovery then teleported them to.  The search is
     * rate-limited to one car every 4 frames because it is 8 x 1013 nodes
     * x 4 point reads and the state it corrects is rare. */
    if (nav_node_dist2(section, node, vehicle->pos) > 625.0f
        && (g_frame_count & 3u) == (unsigned)((vehicle - g_vehicles) & 3)) {
        unsigned int rs = section, rn = node;
        if (nav_nearest(vehicle->pos, &rs, &rn)) {
            unsigned int ws = rs, wn = rn;
            nav_walk(&ws, &wn, vehicle->pos);
            if (nav_node_dist2(ws, wn, vehicle->pos)
                <= nav_node_dist2(rs, rn, vehicle->pos)) {
                section = ws;
                node = wn;
            } else {
                section = rs;
                node = rn;
            }
        }
    }
    vehicle->nav_section = (unsigned short)section;
    vehicle->nav_node = (unsigned short)node;
    if (trace < 0) trace = getenv("B3_NAV_TRACE") != NULL;
    if (trace && g_frame_count % 60 == 0)
        printf("[nav] car%d %u/%u\n", (int)(vehicle - g_vehicles),
               section, node);
    return 1;
}

/* FUN_00178100 / FUN_001781C0: retain the car's lateral placement across
 * the selected node pair, but keep it inside the retail 0.4..0.6 lane band.
 * The planner selects the longitudinal node; this is the exact final pair
 * interpolation used when FUN_001769E0 dispatches its active target. */
static Vec3 nav_pair_target(unsigned int section, unsigned int node, Vec3 pos) {
    const B3RtNavSection* row = &g_nav.sections[section];
    const B3RtNavPair* pair = &g_nav.pairs[row->pair_base + node];
    Vec3 a = nav_point(pair->point_a);
    Vec3 b = nav_point(pair->point_b);
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    float d2 = dx * dx + dy * dy + dz * dz;
    float factor = 0.5f;
    if (d2 > 1e-6f) {
        factor = ((pos.x - a.x) * dx + (pos.y - a.y) * dy
                  + (pos.z - a.z) * dz) / d2;
        if (factor < 0.4f) factor = 0.4f;
        if (factor > 0.6f) factor = 0.6f;
    }
    return (Vec3){a.x + dx * factor, a.y + dy * factor,
                  a.z + dz * factor};
}

static int nav_surface_height(unsigned int section, unsigned int node,
                              Vec3 pos, float* height) {
    if (!nav_state_valid(section, node) || !height) return 0;
    const B3RtNavSection* row = &g_nav.sections[section];
    const B3RtNavPair* pair0 = &g_nav.pairs[row->pair_base + node];
    const B3RtNavPair* pair1 = &g_nav.pairs[row->pair_base + node + 1];
    Vec3 a0 = nav_point(pair0->point_a), b0 = nav_point(pair0->point_b);
    Vec3 a1 = nav_point(pair1->point_a), b1 = nav_point(pair1->point_b);
    Vec3 c0 = vec3_scale(0.5f, vec3_add(a0, b0));
    Vec3 c1 = vec3_scale(0.5f, vec3_add(a1, b1));
    float dx = c1.x - c0.x, dz = c1.z - c0.z;
    float d2 = dx * dx + dz * dz;
    float u = 0.0f;
    if (d2 > 1e-6f)
        u = fmaxf(0.0f, fminf(1.0f, ((pos.x - c0.x) * dx
                                    + (pos.z - c0.z) * dz) / d2));
    Vec3 a = vec3_add(a0, vec3_scale(u, vec3_sub(a1, a0)));
    Vec3 b = vec3_add(b0, vec3_scale(u, vec3_sub(b1, b0)));
    dx = b.x - a.x;
    dz = b.z - a.z;
    d2 = dx * dx + dz * dz;
    float v = 0.5f;
    if (d2 > 1e-6f)
        v = fmaxf(0.0f, fminf(1.0f, ((pos.x - a.x) * dx
                                    + (pos.z - a.z) * dz) / d2));
    *height = a.y + (b.y - a.y) * v;
    return 1;
}

static int nav_forward_delta(const B3RtNavSection* row, unsigned int from,
                             unsigned int to) {
    int delta = (int)to - (int)from;
    if (delta < 0 && (row->flags & 0xff)) delta += row->node_count;
    return delta;
}

/* FUN_001772A0 selects the closest strictly upcoming plan.node_b in the
 * current section.  FUN_00176AF0 then moves through its A/B/C node targets
 * as its local +12-node look-ahead reaches them.  The surrounding branch and
 * lane-routing state is still recovered separately; this gives the driver's
 * normal racing aim the retail planner's longitudinal target now. */
static int nav_plan_target_at(const Vehicle* vehicle, unsigned int section,
                              unsigned int node, Vec3* target,
                              float* corner_speed, unsigned int* out_section,
                              unsigned int* out_node) {
    if (!nav_state_valid(section, node) || g_nav.plan_count == 0) return 0;
    const B3RtNavSection* row = &g_nav.sections[section];
    const B3RtNavPlan* best = NULL;
    int best_delta = INT_MAX;
    for (unsigned int index = 0; index < g_nav.plan_count; index++) {
        const B3RtNavPlan* plan = &g_nav.plans[index];
        if (plan->section != section
            || plan->node_a >= row->node_count
            || plan->node_b >= row->node_count
            || plan->node_c >= row->node_count) continue;
        int delta = nav_forward_delta(row, node, plan->node_b);
        if (delta > 0 && delta < best_delta) {
            best = plan;
            best_delta = delta;
        }
    }
    if (!best) return 0;

    unsigned int probe = node + 12;
    if (probe >= row->node_count) {
        if (row->flags & 0xff) probe %= row->node_count;
        else probe = row->node_count - 1;
    }
    unsigned int selected_node = best->node_c;
    if (nav_forward_delta(row, probe, best->node_a) > 0)
        selected_node = best->node_a;
    else if (nav_forward_delta(row, probe, best->node_b) > 0)
        selected_node = best->node_b;
    *target = nav_pair_target(section, selected_node, vehicle->pos);
    if (corner_speed) *corner_speed = (float)best->speed;
    if (out_section) *out_section = section;
    if (out_node) *out_node = selected_node;
    return 1;
}

static int nav_plan_target(const Vehicle* vehicle, Vec3* target,
                           float* corner_speed, unsigned int* out_section,
                           unsigned int* out_node) {
    unsigned int section = vehicle->nav_target_section;
    unsigned int node = vehicle->nav_target_node;
    if (vehicle->nav_target_ready
        && nav_plan_target_at(vehicle, section, node, target, corner_speed,
                              out_section, out_node))
        return 1;
    return nav_plan_target_at(vehicle, vehicle->nav_section, vehicle->nav_node,
                              target, corner_speed, out_section, out_node);
}

static int nav_step_node(unsigned int* section, unsigned int* node, int dir) {
    if (!nav_state_valid(*section, *node)) return 0;
    const B3RtNavSection* row = &g_nav.sections[*section];
    int candidate = (int)*node + dir;
    if (candidate >= 0 && candidate < row->node_count) {
        *node = (unsigned int)candidate;
        return 1;
    }
    if (row->flags & 0xff) {
        *node = (unsigned int)((candidate + row->node_count) % row->node_count);
        return 1;
    }
    const B3RtNavLink* link = &g_nav.links[row->link_base + *node];
    if (dir > 0 && nav_state_valid(link->forward_section, link->forward_node)) {
        *section = link->forward_section;
        *node = link->forward_node;
        return 1;
    }
    if (dir < 0 && nav_state_valid(link->reverse_section, link->reverse_node)) {
        *section = link->reverse_section;
        *node = link->reverse_node;
        return 1;
    }
    return 0;
}

static Vec3 nav_forward(unsigned int section, unsigned int node) {
    const B3RtNavSection* row = &g_nav.sections[section];
    const B3RtNavPair* pair[4];
    unsigned int base = row->pair_base;
    pair[0] = &g_nav.pairs[base + node];
    pair[1] = &g_nav.pairs[base + node + 1];
    if (node + 2 == (unsigned int)row->node_count) {
        pair[2] = &g_nav.pairs[base + node + 2];
        pair[3] = (row->flags & 0xff) ? &g_nav.pairs[base + 1] : pair[2];
    } else if (node + 1 == (unsigned int)row->node_count) {
        if (row->flags & 0xff) {
            pair[2] = &g_nav.pairs[base + 1];
            pair[3] = &g_nav.pairs[base + 2];
        } else {
            pair[2] = pair[3] = &g_nav.pairs[base + row->node_count];
        }
    } else {
        pair[2] = &g_nav.pairs[base + node + 2];
        pair[3] = &g_nav.pairs[base + node + 3];
    }
    Vec3 past = {0, 0, 0}, future = {0, 0, 0};
    for (int index = 0; index < 2; index++) {
        Vec3 a = nav_point(pair[index]->point_a);
        Vec3 b = nav_point(pair[index]->point_b);
        past.x += a.x + b.x; past.y += a.y + b.y; past.z += a.z + b.z;
        a = nav_point(pair[index + 2]->point_a);
        b = nav_point(pair[index + 2]->point_b);
        future.x += a.x + b.x; future.y += a.y + b.y;
        future.z += a.z + b.z;
    }
    return vec3_sub(future, past);
}

/* FUN_001714F0's placement leg: walk `back` nodes against the route from an
 * explicit cursor, then take FUN_00174740's four-pair forward difference for
 * the heading and FUN_001781C0's pair interpolation for the position.  The
 * stuck rescue passes the LAST VALID NODE (racecar+0x245A) and 8; the crash /
 * off-world re-place passes the live cursor and FUN_001709F0's 3. */
static int nav_recovery_pose_from(Vehicle* vehicle, unsigned int section,
                                  unsigned int node, int back,
                                  Vec3* pos, float* yaw) {
    if (!nav_state_valid(section, node)) return 0;
    for (int step = 0; step < back; step++)
        if (!nav_step_node(&section, &node, -1)) return 0;
    Vec3 fwd = nav_forward(section, node);
    if (fwd.x * fwd.x + fwd.z * fwd.z < 1e-6f) return 0;
    *pos = nav_pair_target(section, node, vehicle->pos);
    *yaw = atan2f(fwd.x, -fwd.z);
    vehicle->nav_section = (unsigned short)section;
    vehicle->nav_node = (unsigned short)node;
    vehicle->nav_ready = 1;
    return 1;
}

/* FUN_00174AF0 @0x00174AF0, the approach projection FUN_00176150 feeds to the
 * corner-brake law:
 *   D = min(seg, dot(carpos - pool[pair[node].point_a],
 *                    normalize(FUN_00174740(section, node))))
 * `seg` is the node's own arc-length span, read in retail from the index
 * row's `edge` table (`row->+4`, stride 8: `edge[node] - edge[node-1]`, or
 * `edge[0]` at node 0).  route.bin v3 does not carry that table, so the
 * harness measures the same quantity off the pair centroids -- the table IS
 * the cumulative length of that polyline.  [C law, [S] for `seg`'s source.] */
static float nav_approach_dist(unsigned int section, unsigned int node,
                               Vec3 pos) {
    if (!nav_state_valid(section, node)) return 0.0f;
    const B3RtNavSection* row = &g_nav.sections[section];
    const B3RtNavPair* pair = &g_nav.pairs[row->pair_base + node];
    Vec3 a = nav_point(pair->point_a);
    Vec3 fwd = nav_forward(section, node);
    float fl = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
    if (fl < 1e-6f) return 0.0f;
    float proj = ((pos.x - a.x) * fwd.x + (pos.y - a.y) * fwd.y
                  + (pos.z - a.z) * fwd.z) / fl;
    Vec3 c0 = nav_midpoint(section, node);
    unsigned int nxt = node + 1;
    if (nxt >= (unsigned int)row->node_count)
        nxt = (row->flags & 0xff) ? 0u : (unsigned int)row->node_count - 1u;
    Vec3 c1 = nav_midpoint(section, nxt);
    float sx = c1.x - c0.x, sy = c1.y - c0.y, sz = c1.z - c0.z;
    float seg = sqrtf(sx * sx + sy * sy + sz * sz);
    return proj < seg ? proj : seg;          /* MINSS @0x00174BBA */
}

// Lane fixup for the driving route: on dual carriageways the corridor
// MIDLINE runs down the median (under the elevated-road underpass at prog
// ~0.49 it threads a 2 m rail slot BETWEEN the carriageways and cars wedge
// in it). The game's forward race line (Gamedata.bgd 0x1ABF20) drives the
// correct lanes by design, so each route point is shifted LATERALLY toward
// it -- longitudinal position (and so the start-grid anchoring at index 0)
// is preserved, the shift is capped at 5 m, and points where the race line
// is far off (its roundabout chord cuts fenced geometry) are left alone.
static void route_lane_fixup(void) {
    int moved = 0;
    for (int i = 0; i < ROUTE_COUNT; i++) {
        // local route direction
        const float* a = g_cl[(i + ROUTE_COUNT - 1) % ROUTE_COUNT];
        const float* b = g_cl[(i + 1) % ROUTE_COUNT];
        float dx = b[0] - a[0], dz = b[2] - a[2];
        float dl = sqrtf(dx * dx + dz * dz);
        if (dl < 1e-6f) continue;
        dx /= dl;
        dz /= dl;
        float px = -dz, pz = dx;                 // lateral unit
        // nearest race-line point (header data is world space: z = -z here)
        float best = 1e30f, bx = 0, bz = 0;
        for (int r = 0; r < B3_CENTERLINE_COUNT; r++) {
            float rx = B3_CENTERLINE[r][0] - g_cl[i][0];
            float rz = -B3_CENTERLINE[r][2] - g_cl[i][2];
            float d2 = rx * rx + rz * rz;
            if (d2 < best) { best = d2; bx = rx; bz = rz; }
        }
        if (best > 12.0f * 12.0f) continue;      // diverged (chord/cut zone)
        float lat = bx * px + bz * pz;           // lateral offset to the line
        if (lat > 5.0f) lat = 5.0f;
        if (lat < -5.0f) lat = -5.0f;
        g_cl[i][0] += px * lat;
        g_cl[i][2] += pz * lat;
        if (lat > 1.0f || lat < -1.0f) moved++;
    }
    printf("[Burnout3] route lane fixup: %d/%d points shifted toward the "
           "forward race line\n", moved, ROUTE_COUNT);
}

// ---------------------------------------------------------------------------
// Collision. Primary source: the GAME'S OWN collision world -- the per-unit
// kd-tree poly soups inside streamed.dat's LOD blocks, extracted by
// tools/extract_collision.py into build/collision.bin and queried through
// src/burnout3_collision.c (b3_ground_probe for the road surface,
// b3_sweep_sphere for body-vs-barrier; format + query chain RE_NOTES 15,
// execution-verified in validate_gameplay.py's 'collision' section).
//
// aim_blocked's 2D barrier grid is likewise built from the real collision
// walls when collision.bin is present. Fallback (no collision.bin): steep
// triangles of the RENDER mesh, the previous approximation.
// ---------------------------------------------------------------------------
#define COLGRID_CELL 16.0f
#define COLGRID_DIM  256
typedef struct { float ax, az, bx, bz, ylo, yhi; } ColSeg;
static ColSeg* g_colsegs = NULL;
static int g_colseg_count = 0;
static int* g_colgrid = NULL;        // per cell: start index into g_colidx
static int* g_colgrid_n = NULL;      // per cell: count
static int* g_colidx = NULL;
static float g_col_x0, g_col_z0;

static int colcell(float x, float z) {
    int cx = (int)((x - g_col_x0) / COLGRID_CELL);
    int cz = (int)((z - g_col_z0) / COLGRID_CELL);
    if (cx < 0 || cz < 0 || cx >= COLGRID_DIM || cz >= COLGRID_DIM) return -1;
    return cz * COLGRID_DIM + cx;
}

// Emit one triangle's edges into the ColSeg pool (shared by both sources).
static void colseg_add_tri(const float* p0, const float* p1, const float* p2,
                           int* cap) {
    const float* e[3][2] = {{p0,p1},{p1,p2},{p2,p0}};
    for (int k = 0; k < 3; k++) {
        float dx = e[k][1][0]-e[k][0][0], dz = e[k][1][2]-e[k][0][2];
        if (dx*dx + dz*dz < 0.01f) continue;               // vertical edge
        if (g_colseg_count >= *cap) {
            *cap *= 2;
            g_colsegs = realloc(g_colsegs, (size_t)*cap * sizeof(ColSeg));
        }
        ColSeg* s = &g_colsegs[g_colseg_count++];
        s->ax = e[k][0][0]; s->az = e[k][0][2];
        s->bx = e[k][1][0]; s->bz = e[k][1][2];
        // Height window of the EDGE, not the whole triangle: the top
        // edge of a tall facade / underpass portal face must not act
        // as a road-level barrier 80 m below itself.
        s->ylo = fminf(e[k][0][1], e[k][1][1]);
        s->yhi = fmaxf(e[k][0][1], e[k][1][1]);
    }
}

static void build_collision(void) {
    // Primary: the game's own collision world (see tools/extract_collision.py
    // + src/burnout3_collision.c). The 2D ColSeg grid for aim_blocked (and
    // the render-mesh fallback for mesh_collide) is derived from whichever
    // source is available.
    int loaded = b3_collision_load("build/collision.bin");
    if (loaded > 0)
        printf("[Burnout3] GAME collision world: %d triangles "
               "(build/collision.bin)\n", loaded);
    else
        printf("[Burnout3] collision.bin missing -- render-mesh wall "
               "approximation in use\n");
    // the full vehicle pipeline's ground rays go through the harness
    // wrapper (mesh probe first, route-height placeholder fallback)
    b3_ground_probe_hook = harness_ground_probe;
    if (loaded <= 0 && !g_have_real_track) return;

    int cap = 1 << 16;
    g_colsegs = malloc((size_t)cap * sizeof(ColSeg));

    if (loaded > 0) {
        // Real barrier polys: wall-like (steep-normal), not gather-excluded.
        float minx = 1e30f, minz = 1e30f;
        for (int i = 0; i < b3_collision_tri_count(); i++) {
            float p0[3], p1[3], p2[3], n[3];
            int ex;
            b3_collision_tri_get(i, p0, p1, p2, n, NULL, &ex);
            minx = fminf(minx, fminf(p0[0], fminf(p1[0], p2[0])));
            minz = fminf(minz, fminf(p0[2], fminf(p1[2], p2[2])));
            if (ex) continue;                              // camera/net walls
            if (fabsf(n[1]) > 0.45f) continue;             // not steep
            float ylo = fminf(p0[1], fminf(p1[1], p2[1]));
            float yhi = fmaxf(p0[1], fmaxf(p1[1], p2[1]));
            if (yhi - ylo < 0.12f) continue;               // paint-thin
            colseg_add_tri(p0, p1, p2, &cap);
        }
        g_col_x0 = minx;
        g_col_z0 = minz;
    } else {
        const TrackMesh* m = &g_real_track;
        g_col_x0 = m->min[0];
        g_col_z0 = m->min[2];
        // Fallback pass: collect steep-triangle edges of the RENDER mesh.
        for (int t = 0; t < m->triangle_count; t++) {
            const unsigned* idx = m->indices + (size_t)t * 3;
            const float* p0 = m->positions + (size_t)idx[0] * 3;
            const float* p1 = m->positions + (size_t)idx[1] * 3;
            const float* p2 = m->positions + (size_t)idx[2] * 3;
            float ux = p1[0]-p0[0], uy = p1[1]-p0[1], uz = p1[2]-p0[2];
            float vx = p2[0]-p0[0], vy = p2[1]-p0[1], vz = p2[2]-p0[2];
            float nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
            float nl = sqrtf(nx*nx + ny*ny + nz*nz);
            if (nl < 1e-9f || fabsf(ny) / nl > 0.45f) continue;  // not steep
            float ylo = fminf(p0[1], fminf(p1[1], p2[1]));
            float yhi = fmaxf(p0[1], fmaxf(p1[1], p2[1]));
            if (yhi - ylo < 0.12f) continue;   // paint-thin (0.12: keep low
                                               // kerb and parapet faces)
            colseg_add_tri(p0, p1, p2, &cap);
        }
    }

    // Pass 2: bucket by cell (segments are short; register at both endpoints
    // and midpoint, deduped per segment).
    int cells = COLGRID_DIM * COLGRID_DIM;
    g_colgrid = calloc(cells, sizeof(int));
    g_colgrid_n = calloc(cells, sizeof(int));
    int* touch = malloc(3 * sizeof(int));
    // count
    for (int i = 0; i < g_colseg_count; i++) {
        ColSeg* s = &g_colsegs[i];
        touch[0] = colcell(s->ax, s->az);
        touch[1] = colcell(s->bx, s->bz);
        touch[2] = colcell((s->ax+s->bx)*0.5f, (s->az+s->bz)*0.5f);
        for (int k = 0; k < 3; k++) {
            int c = touch[k];
            int dup = 0;
            for (int j = 0; j < k; j++) dup |= (touch[j] == c);
            if (c >= 0 && !dup) g_colgrid_n[c]++;
        }
    }
    int total = 0;
    for (int c = 0; c < cells; c++) { g_colgrid[c] = total; total += g_colgrid_n[c]; }
    g_colidx = malloc((size_t)total * sizeof(int));
    int* fill = calloc(cells, sizeof(int));
    for (int i = 0; i < g_colseg_count; i++) {
        ColSeg* s = &g_colsegs[i];
        touch[0] = colcell(s->ax, s->az);
        touch[1] = colcell(s->bx, s->bz);
        touch[2] = colcell((s->ax+s->bx)*0.5f, (s->az+s->bz)*0.5f);
        for (int k = 0; k < 3; k++) {
            int c = touch[k];
            int dup = 0;
            for (int j = 0; j < k; j++) dup |= (touch[j] == c);
            if (c >= 0 && !dup) g_colidx[g_colgrid[c] + fill[c]++] = i;
        }
    }
    free(fill);
    free(touch);
    printf("[Burnout3] mesh collision: %d barrier segments from the real track\n",
           g_colseg_count);
}

// 2D segment intersection helper for aim_blocked below.
static int seg2d_cross(float ax, float az, float bx, float bz,
                       float cx, float cz, float dx, float dz) {
    float rpx = bx - ax, rpz = bz - az;
    float spx = dx - cx, spz = dz - cz;
    float den = rpx * spz - rpz * spx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((cx - ax) * spz - (cz - az) * spx) / den;
    float u = ((cx - ax) * rpz - (cz - az) * rpx) / den;
    return t > 0.0f && t < 1.0f && u > 0.0f && u < 1.0f;
}

// Does the 2D ray from (ax,az) to (bx,bz) at height y cross any barrier
// segment of the collision grid? Used by the AI so it never aims a pursuit
// point through a wall/road-closure barrier (GLUE routing on the real
// collision geometry).
static int aim_blocked(float ax, float az, float bx, float bz, float y) {
    if (!g_colsegs) return 0;
    float x0 = fminf(ax, bx), x1 = fmaxf(ax, bx);
    float z0 = fminf(az, bz), z1 = fmaxf(az, bz);
    int cx0 = (int)((x0 - g_col_x0) / COLGRID_CELL) - 1;
    int cx1 = (int)((x1 - g_col_x0) / COLGRID_CELL) + 1;
    int cz0 = (int)((z0 - g_col_z0) / COLGRID_CELL) - 1;
    int cz1 = (int)((z1 - g_col_z0) / COLGRID_CELL) + 1;
    if (cx0 < 0) cx0 = 0;
    if (cz0 < 0) cz0 = 0;
    if (cx1 >= COLGRID_DIM) cx1 = COLGRID_DIM - 1;
    if (cz1 >= COLGRID_DIM) cz1 = COLGRID_DIM - 1;
    for (int cz = cz0; cz <= cz1; cz++) for (int cx = cx0; cx <= cx1; cx++) {
        int c = cz * COLGRID_DIM + cx;
        for (int k = 0; k < g_colgrid_n[c]; k++) {
            const ColSeg* s = &g_colsegs[g_colidx[g_colgrid[c] + k]];
            // A barrier only counts if its top clears the wheel-contact
            // height (pos.y - 0.5). The old +1.2 margin caught structure up
            // to 0.7 m BELOW the road surface -- at the prog-0.427 junction
            // the route crosses a deck whose under-structure (top 0.6 below
            // the deck) walled the road off completely.
            if (y < s->ylo - 0.5f || y > s->yhi + 0.45f) continue;
            if (seg2d_cross(ax, az, bx, bz, s->ax, s->az, s->bx, s->bz))
                return 1;
        }
    }
    return 0;
}

// Push the car out of any nearby barrier. With the real collision world
// loaded this is a sphere-vs-triangle test against the game's own barrier
// polys (b3_sweep_sphere over build/collision.bin; the push-out response
// itself is harness logic -- the game's body path FUN_00107950/FUN_0010aad0
// is unported). Fallback: the 2D ColSeg grid.
// Build the B3CrashVehicle view FUN_0011AEF0's wall test reads and hand it
// one world contact.  The physics state is GAME space (z negated relative to
// the harness), so the contact point and normal are mirrored on the way in.
// The frame's translation row is overridden with the CURRENT substepped
// position: mesh_collide runs between the integrator and the writeback.
static void tdr_build_crash_vehicle(Vehicle* v, unsigned short surface,
                                   B3CrashVehicle* out) {
    B3RigidBody* rb = &v->fsim.rb;
    B3CrashVehicle cv;
    memset(&cv, 0, sizeof(cv));
    memcpy(cv.frame, rb->frame, sizeof(cv.frame));
    cv.frame[3][0] = v->pos.x;
    cv.frame[3][2] = -v->pos.z;
    memcpy(cv.iinv_world, rb->inv_inertia_world, sizeof(cv.iinv_world));
    cv.vel[0] = v->vel.x; cv.vel[1] = v->vel.y; cv.vel[2] = -v->vel.z;
    cv.vel[3] = sqrtf(cv.vel[0]*cv.vel[0] + cv.vel[1]*cv.vel[1]
                      + cv.vel[2]*cv.vel[2]);
    if (cv.vel[3] > 1e-6f)
        for (int k = 0; k < 3; k++) cv.dir[k] = cv.vel[k] / cv.vel[3];
    memcpy(cv.omega, rb->omega, sizeof(cv.omega));
    cv.mass = v->fsim.mass;
    for (int k = 0; k < 4; k++) {
        cv.bbmax[k] = v->fsim.half_ext[k];
        cv.bbmin[k] = v->fsim.center_off[k];
    }
    cv.ground_frac  = v->fsim.brake_1404;
    cv.surface_grip = 1.0f;      // veh+0x13A8 is not modelled; class != 0
    cv.is_class0    = 0;         // below uses the 0.99 scrub either way
    cv.no_scrub     = 0;
    /* CRASH-PARITY item 1: the REAL surface type, not a stand-in.  It is what
     * FUN_0011AEF0 @0x0011B944 reads to refuse a wall crash on low byte 0x20
     * (the chevron boards -- which the OBJECT path below now handles instead)
     * and what @0x0011B9D7 shifts into racecar+0x15CC for the signature wall
     * takedown table. */
    cv.surface      = surface ? surface : (unsigned short)0x0001;
    cv.flags1353    = 0;
    /* veh+0x1524, the drift gate FUN_001206D0 @0x001206E4 reads to route an
     * impulse linear-only. */
    cv.drift_state  = v->fsim.drift_state_1524;
    cv.gear         = v->fsim.trans.gear;
    *out = cv;
}

static void tdr_wall_report(Vehicle* v, int slot, const float q[3],
                            float px, float pz, float vin,
                            unsigned short surface) {
    B3CrashVehicle cv;
    tdr_build_crash_vehicle(v, surface, &cv);
    // world -> game: mirror z
    float pt[4] = {q[0], q[1], -q[2], 0.0f};
    float nn[4] = {px,   0.0f, -pz,   0.0f};
    if (getenv("B3_TDWALL_TRACE"))
        printf("[tdwall] t=%.2f car %d report vin=%.1f m/s surf=0x%X\n",
               g_race_time, slot, vin, surface);
    b3_td_wall_contact(&g_tdr, slot, g_race_time, &cv, pt, nn, vin);
}

// CRASH-PARITY item 3 -- FUN_0011AEF0's CHASSIS RESPONSE for a barrier
// contact, crashing or not.  The crash decision (tdr_wall_report above) and
// the response are independent branches of the same retail function: the
// whole block @0x0011B4B0..0x0011B904 runs on every wall contact and only the
// tail @0x0011B909 decides whether FUN_0010DCA0 is also called.  The terms
// and their addresses are listed in burnout3_td_rules.h section 9b; the point
// that matters here is that retail resolves the contact at the CONTACT POINT
// (FUN_00106720 with e = 0, routed through FUN_001206D0), so the impulse
// carries a TORQUE that straightens a car clipping a barrier.  The old
// harness response killed the into-wall velocity at the centre of mass and
// left L/omega untouched, which is why a shallow clip pivoted the car.
//
// GLUE, and the only glue here: the linear half is applied to the velocity

        // PH-08 [RECOVERED SHAPE]: retail never writes a velocity here.
        // FUN_00112E70's object arm accumulates the reaction into the rigid
        // body's impulse channel +0x110/+0x120 (FUN_00106500) and lets
        // FUN_00109560 apply it -- v->fsim.rb.imp_force / imp_torque are the
        // ported equivalents and are cleared by the integrator every frame.
        // Route this through them instead of v->vel; see
        // docs/PHYSICS_GLUE_LEDGER.md PH-08.
// immediately instead of through veh+0x110 (a one-frame ordering difference)
// so the sweep's second iteration sees the reduced closing speed, exactly as
// the centre-of-mass kill it replaces did.  The angular half goes into the
// retail accumulator veh+0x120 and is consumed by the next
// b3_rigid_body_integrate, which is FUN_00109560's own ordering.
// Returns 1 when the ported response was applied.
static int tdr_wall_scrape(Vehicle* v, const float q[3], float px, float pz,
                           unsigned short surface) {
    B3RigidBody* rb = &v->fsim.rb;
    B3CrashVehicle cv;
    B3TdWallResponse rsp;
    float m;
    if (!v->fsim_ready) return 0;
    tdr_build_crash_vehicle(v, surface, &cv);
    float pt[4] = {q[0], q[1], -q[2], 0.0f};
    float nn[4] = {px,   0.0f, -pz,   0.0f};
    if (!b3_td_wall_response(&cv, pt, nn, &rsp)) return 0;
    // steps 2 + 3: the head-on and surface-grip velocity scrubs.  Both are
    // uniform scalings, so they need no mirror.
    v->vel.x *= rsp.scrub;
    v->vel.y *= rsp.scrub;
    v->vel.z *= rsp.scrub;
    v->sim.speed *= rsp.scrub;
    // step 7 linear (game -> harness: z negated)
    m = cv.mass > 1.0f ? cv.mass : 1.0f;
    v->vel.x +=  rsp.imp[0] / m;
    v->vel.y +=  rsp.imp[1] / m;
    v->vel.z += -rsp.imp[2] / m;
    // step 7 angular -- GAME space, straight into veh+0x120
    for (int k = 0; k < 3; k++) rb->imp_torque[k] += rsp.ang_imp[k];
    return 1;
}

static void mesh_collide(Vehicle* v) {
    const float margin = 1.0f;
    if (b3_collision_ready()) {
        float c[3] = {v->pos.x, v->pos.y + 0.3f, v->pos.z};
        for (int it = 0; it < 2; it++) {          // resolve up to 2 contacts
            float q[3], n[3];
            /* CRASH-PARITY item 4: FUN_0011BBE0's two RUNTIME gather filters
             * (skip structure faces the car is separating from at more than
             * 0.5 m/s, skip normal.y < -0.7) need the car's own velocity
             * veh+0xB0 -- in harness space, which both filters are invariant
             * under.  The sweep also hands back the winning triangle's
             * surface type, which the wall and object triggers both read. */
            unsigned short stype = 0;
            float svel[3] = {v->vel.x, v->vel.y, v->vel.z};
            if (!b3_sweep_sphere_ex(c, c, margin, 0.45f, svel, q, n, &stype))
                break;
            float px = n[0], pz = n[2];
            float pl = sqrtf(px*px + pz*pz);
            if (pl < 1e-6f) break;                 // top/bottom graze
            px /= pl; pz /= pl;
            float dx = c[0] - q[0], dz = c[2] - q[2];
            float dy = c[1] - q[1];
            float d = sqrtf(dx*dx + dy*dy + dz*dz);
            float depth = margin - d;
            if (depth <= 0.0f) break;
            v->pos.x += px * depth;
            v->pos.z += pz * depth;
            c[0] = v->pos.x; c[2] = v->pos.z;
            float vin = -(v->vel.x * px + v->vel.z * pz);
            {   // TD-RULES: hand the contact to FUN_0011AEF0's OWN wall test
                // (b3_td_wall_contact -> b3_crash_wall_eval).  The geometry
                // is this harness's sphere sweep, but everything from the
                // flattened normal onward -- the point velocity, the impulse
                // solver, the veh+0x194 magnitude and the
                // dv > authority*27.5 / headon > authority*0.707 gate -- is
                // the retail code.  The old closing-speed GLUE could not see
                // a spun car's tail hitting a barrier (omega x r never
                // entered it); dv does.
                int sl = (int)(v - g_vehicles);
                if (sl >= 0 && sl < 8 && v->fsim_ready
                    && v->crashed_until <= 0.0f) {
                    tdr_wall_report(v, sl, q, px, pz, vin, stype);
                    /* CRASH-PARITY item 1: FUN_00112E70's OBJECT/PROP crash
                     * trigger.  Retail routes a car (collision-handle type 0
                     * or 2) against a type-3 prop entity through
                     * FUN_00111CD0 @0x00111D77; the crashable pair is the
                     * DAT_0039AE50 table entry [class 2][class 0] = 1.  This
                     * world is one triangle soup, so the prop class maps onto
                     * FUN_0011BBE0's own structure band 0x15..0x20 (GLUE,
                     * burnout3_td_rules.h section 10).  v_rel is the object's
                     * point velocity minus the car's; a soup triangle is
                     * static, so it is -v.  Game space: z negated. */
                    /* DEFAULT OFF (B3_OBJECT_SOUP=1 re-enables): mapping the
                     * soup's structure band onto the object trigger misfired
                     * in live play -- retail's object path fires ONLY
                     * against type-3 PROP ENTITIES (and the designated
                     * big-hit traffic vehicle); static world geometry never
                     * routes through it, only through the wall test with
                     * its dv + head-on gates.  A 160 mph brush against a
                     * structure-typed SURFACE cleared the 75 mph closing
                     * bar with no head-on requirement and wrecked the user
                     * out of nowhere ("my car randomly stopped moving").
                     * The real consumer is the props system's entities. */
                    static int soup_obj = -1;
                    if (soup_obj < 0)
                        soup_obj = getenv("B3_OBJECT_SOUP") != NULL;
                    if (soup_obj && b3_collision_is_structure(stype)) {
                        float vr[3] = {-v->vel.x, -v->vel.y, v->vel.z};
                        float on[3] = {px, 0.0f, -pz};
                        b3_td_object_contact(
                            &g_tdr, sl, g_race_time, vr, on, v->fsim.mass,
                            b3_td_object_class(3, 0),   /* prop  -> class 2 */
                            b3_td_object_class(0, 0),   /* racer -> class 0 */
                            g_race_time < v->immune_until);
                    }
                    // veh+0x212: FUN_0011AEF0 sets it when it resolves a
                    // chassis contact; FUN_0011ECF0 vetoes the steer-away
                    // envelope for that frame.
                    /* PHYS-LEDGER-3 / F2e: the substep owns this byte now.
                     * FUN_0011BE50 clears it @0x0011C0A9 and FUN_0011AEF0's
                     * caller sets it @0x0011C0CA, both inside the loop, so a
                     * post-step write here would survive into the NEXT
                     * frame's input stage -- one frame late.  Kept only for
                     * a body with no pipeline. */
                    if (!(v->fsim_ready && v->fsim.chassis_resolve))
                        v->fsim.contact_212 = 1;
                }
                if (sl >= 0 && sl < 8 && vin > g_tdr_wall[sl]) {
                    g_tdr_wall[sl] = vin;
                    g_tdr_wall_n[sl] = (Vec3){px, 0.0f, pz};
                }
                if (vin > 3.0f) {
                    v->last_hit_pos = (Vec3){q[0], q[1], q[2]};
                    v->last_hit_n   = (Vec3){px, 0.0f, pz};
                    v->last_hit_vin = vin;
                    v->last_hit_time = g_race_time;
                }
                /* CRASH-SHOW H7: the wall/armco GRIND SPARKS.  The
                 * shower rides the contact point, thrown back along the
                 * body's direction of travel; the rate follows the
                 * along-wall speed, not the closing speed. */
                if (sl >= 0 && sl < 8 && v->sim.speed > 6.0f) {
                    float cp[3] = { q[0], q[1], q[2] };
                    float hd = sqrtf(v->vel.x*v->vel.x + v->vel.z*v->vel.z);
                    float dir[3] = { hd > 0.01f ? -v->vel.x/hd : 0.0f, 0.0f,
                                     hd > 0.01f ? -v->vel.z/hd : 0.0f };
                    b3_pfx_grind_spark(cp, dir, v->sim.speed, g_delta_time,
                                       &g_pfx_spark_carry[sl]);
                }
                    if (sl == 0) {
                        /* SFX-DRIVE: the same contact feeds the body
                         * scrape loops. */
                        g_sfx_scrape_hit = 1;
                        g_sfx_scrape_spd = v->sim.speed;
                        g_sfx_scrape_pos = (Vec3){q[0], q[1], q[2]};
                    }
            }
            // SFX: car-vs-world crunch (IMPACTWORL emitter, window 6..22 m/s
            // closing speed -- exactly this vin). The emitter's own silence
            // gate and 20-frame cooldown do the spam control.
            /* SFX-CRASH: FUN_0014D0F0 routes a world contact by the
             * car's crashed byte (veh+0x210): a crashed car plays the
             * crashed-car impact FUN_0014F130, not IMPACTWORL. */
            if (vin > 3.0f)
                b3_sfx_impact_world(vin, v->crashed_until > 0.0f,
                                    v->pos.x, v->pos.y, v->pos.z);
            /* CRASH-PARITY item 3: the ported chassis response (scrubs +
             * the e = 0 point impulse WITH its torque).  The centre-of-mass
             * kill below survives only as the fallback for a car with no
             * full-pipeline body (the pre-fsim_ready frames and the wreck
             * containment call). */
            /* PHYS-LEDGER-3 / F2d -- PH-08 RETIRED.  A car with the
             * full pipeline resolved this contact INSIDE its substep
             * (FUN_0011AEF0 @0x0011C0B7, over the soup harness_soup_freeze
             * handed it), and that substep's FUN_00109560 has already
             * consumed the +0x110/+0x120 impulse and the +0x130 deflection.
             * Applying the response a second time here is what the old
             * `the linear half is applied to the velocity immediately`
             * GLUE was compensating for; both are gone.  The centre-of-mass
             * kill survives ONLY for a body that has no pipeline of its own
             * -- the pre-fsim_ready frames and the wreck-containment call
             * (burnout3_full.c's wreck branch), which is what
             * tdr_wall_scrape's own fallback comment always said. */
            if (!(v->fsim_ready && v->fsim.chassis_resolve)) {
                if (!tdr_wall_scrape(v, q, px, pz, stype) && vin > 0.0f) {
                    v->vel.x += vin * px;
                    v->vel.z += vin * pz;
                    v->sim.speed *= 0.96f;   // scrape bleeds speed
                }
            }
        }
        return;
    }
    for (int oz = -1; oz <= 1; oz++) for (int ox = -1; ox <= 1; ox++) {
        int c = colcell(v->pos.x + ox * COLGRID_CELL, v->pos.z + oz * COLGRID_CELL);
        if (c < 0) continue;
        for (int k = 0; k < g_colgrid_n[c]; k++) {
            const ColSeg* s = &g_colsegs[g_colidx[g_colgrid[c] + k]];
            // Same contact-height rule as aim_blocked: ignore structure
            // whose top sits below the wheels (bridge decks over lower
            // roads); 0.45 still catches kerbs at pos.y = surface + 0.5.
            if (v->pos.y < s->ylo - 0.5f || v->pos.y > s->yhi + 0.45f) continue;
            float abx = s->bx - s->ax, abz = s->bz - s->az;
            float len2 = abx*abx + abz*abz;
            float t = len2 > 1e-9f
                    ? ((v->pos.x - s->ax) * abx + (v->pos.z - s->az) * abz) / len2
                    : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float cx = s->ax + abx * t, cz = s->az + abz * t;
            float dx = v->pos.x - cx, dz = v->pos.z - cz;
            float d = sqrtf(dx*dx + dz*dz);
            if (d >= margin || d < 1e-6f) continue;
            float ux = dx / d, uz = dz / d;
            v->pos.x = cx + ux * margin;
            v->pos.z = cz + uz * margin;
            float vin = -(v->vel.x * ux + v->vel.z * uz);
            if (vin > 0.0f) {
                v->vel.x += vin * ux;
                v->vel.z += vin * uz;
                v->sim.speed *= 0.96f;   // scrape bleeds speed
            }
        }
    }
}

static void generate_track(void) {
    // Real circuit: center line recovered from Gamedata.bgd (Bangkok, C1_V1),
    // see tools/extract_bgd_paths.py and build/bgd_walls.png for verification.
    int num_points = ROUTE_COUNT;
    g_track.points = malloc(num_points * sizeof(Vec3));
    g_track.num_points = num_points;
    for (int i = 0; i < num_points; i++)
        g_track.points[i] = (Vec3){g_cl[i][0], g_cl[i][1], g_cl[i][2]};

    // Mean corridor width measured from the recovered wall strip.
    g_track.width = 14.5f;
    strcpy(g_track.name, "Bangkok (Tracks/AS/C1_V1)");

    printf("[Burnout3] REAL circuit from Gamedata.bgd: %s (%d points)\n",
           g_track.name, num_points);
}

// Closest point on a closed polyline loop to (x,z); XZ metric, interpolated
// along the winning segment so Y comes out smooth. Returns squared distance.
static float loop_closest(const float (*pts)[3], int n, float x, float z,
                          Vec3* out, int* out_seg) {
    float best = 1e30f;
    for (int i = 0; i < n; i++) {
        const float* a = pts[i];
        const float* b = pts[(i + 1) % n];
        float abx = b[0] - a[0], abz = b[2] - a[2];
        float apx = x - a[0], apz = z - a[2];
        float len2 = abx * abx + abz * abz;
        float t = len2 > 1e-9f ? (apx * abx + apz * abz) / len2 : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float cx = a[0] + abx * t, cz = a[2] + abz * t;
        float dx = x - cx, dz = z - cz;
        float d2 = dx * dx + dz * dz;
        if (d2 < best) {
            best = d2;
            if (out) *out = (Vec3){cx, a[1] + (b[1] - a[1]) * t, cz};
            if (out_seg) *out_seg = i;
        }
    }
    return best;
}

// Keep a car inside the road corridor bounded by the recovered wall strips,
// and follow the road height. The wall/center point data is the game's own;
// this collision response is harness logic.
static void apply_track_constraints(Vehicle* v) {
    Vec3 c;
    loop_closest(g_cl, ROUTE_COUNT, v->pos.x, v->pos.z, &c, NULL);

    // Collide against the real track mesh (barriers/kerbs/buildings).
    mesh_collide(v);

    // Safety net: far off the route (e.g. through an unfenced gap into the
    // river), pull back toward the road.
    float dcx = v->pos.x - c.x, dcz = v->pos.z - c.z;
    float dc = sqrtf(dcx * dcx + dcz * dcz);
    if (dc > 60.0f && dc > 1e-6f) {
        v->pos.x = c.x + dcx / dc * 60.0f;
        v->pos.z = c.z + dcz / dc * 60.0f;
    }

    // Road-height follow. Primary: the game's own road surface via
    // b3_ground_probe (the under-body down-ray of FUN_001239C0 over the real
    // collision polys) -- bridges and underpasses resolve to the correct
    // deck instead of the single route-line height. Fallback when the probe
    // misses (gap in the collision world / off-world): the route line.
    float gh, gn[3];
    if (b3_collision_ready() &&
        b3_ground_probe(v->pos.x, v->pos.y, v->pos.z, &gh, gn) >= 0) {
        v->pos.y = gh + 0.5f;
    } else {
        v->pos.y = c.y + 0.5f;
    }
}

// Ground query for the full vehicle pipeline (b3_ground_probe_hook). The
// pipeline runs in GAME space; the harness's data is in GL space (z
// negated), so the query mirrors in and the normal mirrors back out.
// Primary: the game's own collision world (b3_ground_probe over
// build/collision.bin, the collision agent's module); PLACEHOLDER fallback
// while that path is not yet live: the recovered route line's height with a
// flat-up normal, surface type 0.
static int harness_ground_probe(float x, float y, float z,
                                float* out_height, float out_normal[3]) {
    float zh = -z;                      // game -> harness/GL
    if (b3_collision_ready()) {
        int s = b3_ground_probe(x, y, zh, out_height, out_normal);
        if (s >= 0) {
            out_normal[2] = -out_normal[2];   // harness -> game
            return s;
        }
    }
    Vec3 c;
    loop_closest(g_cl, ROUTE_COUNT, x, zh, &c, NULL);
    (void)y;
    *out_height = c.y;
    out_normal[0] = 0.0f;
    out_normal[1] = 1.0f;
    out_normal[2] = 0.0f;
    return 0;
}

static void generate_road_mesh(void) {
    int n = g_track.num_points;
    Vec3* pts = g_track.points;
    
    // One quad per track segment: 6 vertices, 2 triangles. The counts must match
    // what the loop below actually writes -- overshooting leaves uninitialised
    // indices that the renderer then dereferences out of bounds.
    g_road_mesh.num_verts = n * 6;
    g_road_mesh.verts = malloc(g_road_mesh.num_verts * sizeof(Vec3));
    g_road_mesh.num_faces = n * 2;
    g_road_mesh.indices = malloc(g_road_mesh.num_faces * 3 * sizeof(unsigned short));
    
    float hw = g_track.width * 0.5f;
    int vi = 0, fi = 0;
    
    for (int i = 0; i < n; i++) {
        Vec3 p0 = pts[i];
        Vec3 p1 = pts[(i + 1) % n];
        Vec3 dir = vec3_normalize(vec3_sub(p1, p0));
        Vec3 perp = (Vec3){-dir.z, 0.0f, dir.x};
        
        Vec3 l0 = vec3_add(p0, vec3_scale(hw, perp));
        Vec3 r0 = vec3_sub(p0, vec3_scale(hw, perp));
        Vec3 l1 = vec3_add(p1, vec3_scale(hw, perp));
        Vec3 r1 = vec3_sub(p1, vec3_scale(hw, perp));
        
        // Two triangles per strip
        g_road_mesh.verts[vi++] = l0;
        g_road_mesh.verts[vi++] = r0;
        g_road_mesh.verts[vi++] = l1;
        g_road_mesh.verts[vi++] = r0;
        g_road_mesh.verts[vi++] = r1;
        g_road_mesh.verts[vi++] = l1;
        
        g_road_mesh.indices[fi++] = (unsigned short)(vi - 6);
        g_road_mesh.indices[fi++] = (unsigned short)(vi - 5);
        g_road_mesh.indices[fi++] = (unsigned short)(vi - 4);
        g_road_mesh.indices[fi++] = (unsigned short)(vi - 3);
        g_road_mesh.indices[fi++] = (unsigned short)(vi - 2);
        g_road_mesh.indices[fi++] = (unsigned short)(vi - 1);
    }
    
    printf("[Burnout3] Generated road mesh (%d faces)\n", g_road_mesh.num_faces);
}

static void generate_ground_mesh(void) {
    // Simple ground plane
    int n = g_track.num_points;
    Vec3* pts = g_track.points;
    
    g_ground_mesh.num_verts = 4;
    g_ground_mesh.verts = malloc(4 * sizeof(Vec3));
    g_ground_mesh.num_faces = 2;
    g_ground_mesh.indices = malloc(6 * sizeof(unsigned short));
    
    // Get bounds
    float min_x = 10000.0f, max_x = -10000.0f;
    float min_z = 10000.0f, max_z = -10000.0f;
    for (int i = 0; i < n; i++) {
        min_x = fminf(min_x, pts[i].x);
        max_x = fmaxf(max_x, pts[i].x);
        min_z = fminf(min_z, pts[i].z);
        max_z = fmaxf(max_z, pts[i].z);
    }
    
    float pad = 200.0f;
    g_ground_mesh.verts[0] = (Vec3){min_x - pad, -2.0f, min_z - pad};
    g_ground_mesh.verts[1] = (Vec3){min_x + pad, -2.0f, min_z - pad};
    g_ground_mesh.verts[2] = (Vec3){min_x + pad, -2.0f, max_z + pad};
    g_ground_mesh.verts[3] = (Vec3){min_x - pad, -2.0f, max_z + pad};
    
    g_ground_mesh.indices[0] = 0; g_ground_mesh.indices[1] = 1; g_ground_mesh.indices[2] = 2;
    g_ground_mesh.indices[3] = 0; g_ground_mesh.indices[4] = 2; g_ground_mesh.indices[5] = 3;
    
    printf("[Burnout3] Generated ground mesh\n");
}

// ============================================================
// Vehicle physics (original -- the game's handling model is not yet reversed)
// ============================================================

static float find_track_progress(Vec3 pos) {
    // Find closest point on track
    int n = g_track.num_points;
    Vec3* pts = g_track.points;
    
    float min_dist = 1e18f;
    int closest = 0;
    
    for (int i = 0; i < n; i++) {
        Vec3 diff = vec3_sub(pos, pts[i]);
        float dist = diff.x * diff.x + diff.z * diff.z;
        if (dist < min_dist) {
            min_dist = dist;
            closest = i;
        }
    }
    
    return (float)closest / (float)n;
}

static float get_track_normal(Vec3 pos) {
    // Return distance from track center
    int n = g_track.num_points;
    Vec3* pts = g_track.points;
    
    float min_dist = 1e18f;
    for (int i = 0; i < n; i++) {
        Vec3 diff = vec3_sub(pos, pts[i]);
        float dist = sqrtf(diff.x * diff.x + diff.z * diff.z);
        if (dist < min_dist) min_dist = dist;
    }
    
    return min_dist - g_track.width * 0.5f;
}

// Smallest signed angle a-b, wrapped to [-pi, pi].
static float angle_diff(float a, float b) {
    float d = fmodf(a - b + PI, TAU);
    if (d < 0) d += TAU;
    return d - PI;
}

// Distance to the nearest OTHER racer inside a forward cone (AI avoidance
// input); traffic is scanned by traffic_nearest_ahead() in the traffic
// section below. Returns a large value when the road ahead is clear.
static float traffic_nearest_ahead(const Vec3* pos, float yaw, float maxd);
static float nearest_car_ahead(const Vehicle* self) {
    float fx = sinf(self->rot.y), fz = -cosf(self->rot.y);
    float best = 1e9f;
    for (int i = 0; i < g_num_vehicles; i++) {
        const Vehicle* o = &g_vehicles[i];
        if (o == self || !o->active) continue;
        float dx = o->pos.x - self->pos.x, dz = o->pos.z - self->pos.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d < 0.1f || d > 30.0f) continue;
        if ((dx * fx + dz * fz) / d < 0.7f) continue;   // ~45 degree cone
        if (d < best) best = d;
    }
    float t = traffic_nearest_ahead(&self->pos, self->rot.y, 30.0f);
    return t < best ? t : best;
}

// CRASH-EVENT: start a physical wreck for a racer slot. The impact response
// (contact impulse with torque -> spin/tumble) is the ported FUN_0011AEF0 /
// FUN_00106720 / FUN_00106500 chain; picking the contact from the harness's
// sphere/capsule collision world is GLUE. contact_n points from the obstacle
// into the victim; rel_vel = victim velocity relative to the obstacle.
/* AI-DRIVE measurement: per-slot wreck accounting.  B3_AI_CRASHLOG=1 prints
 * one line per rival wreck and a SUMMARY at B3_EXIT_AT, which is how the
 * before/after crash counts in the integration note were taken. */
static int   g_aicrash_n[8];
static int   g_aicrash_kind[8][3];   /* 0 = takedown, 1 = traffic, 2 = world */
static float traffic_nearest_ahead(const Vec3* pos, float yaw, float maxd);
static void ai_crash_note(Vehicle* v, int slot) {
    static int on = -1;
    if (on < 0) on = getenv("B3_AI_CRASHLOG") != NULL;
    int kind = 2;
    if (v->slam_by >= 0 && g_race_time - v->slam_time < 1.5f) kind = 0;
    else if (traffic_nearest_ahead(&v->pos, v->rot.y, 12.0f) < 12.0f) kind = 1;
    g_aicrash_n[slot]++;
    g_aicrash_kind[slot][kind]++;
    if (on)
        printf("[aicrash] t=%6.2f car%d #%d %s spd %.1f pos %.0f %.0f %.0f\n",
               g_race_time, slot, g_aicrash_n[slot],
               kind == 0 ? "takedown" : (kind == 1 ? "traffic" : "world"),
               v->sim.speed, v->pos.x, v->pos.y, v->pos.z);
}

// The per-slot crash latch (crash_record+0x130), on the DILATED game clock.
// is_truck = racecar+0x1920 == 2 (the harness HEVY class); presented = a
// crash-cinema is already running when this crash lands (retail's
// FUN_00017310 presentation query).  In the harness the crash-cinema is
// live while any vehicle is in a crashed state (crashed_until > 0) or the
// takedown presentation is active -- a chain crash landing mid-presentation
// takes the longer 15.0 arm.  Replaces the old GLUE 8.0/12.0 flat immune
// windows.  See the table in burnout3_crash.h for the retail provenance.
static float crash_latch_for(Vehicle* v) {
    int is_truck = v->info && strcmp(v->info->class_code, "HEVY") == 0;
    // "presented" = ANOTHER car's crash-cinema is already running when this
    // crash lands (a chain crash).  The victim's own crashed state is
    // excluded -- the crash sites stamp crashed_until before this call, and
    // retail's query is on the global presentation object, never the
    // victim's own byte.  (A self re-crash while crashed is blocked by the
    // crashed_until gate at every site, so the re-armed-record path retail
    // has through rec[+0x12] is unreachable here.)
    int presented = 0;
    for (int i = 0; i < g_num_vehicles && !presented; i++)
        presented = (&g_vehicles[i] != v) && g_vehicles[i].crashed_until > 0.0f;
    if (!presented) { B3TdfxStatus tst; b3_tdfx_status(&tst); presented = tst.active; }
    return b3_crash_latch_duration(is_truck, presented);
}

// The per-kind crash-entry wiring (see B3WreckEntryKind); the six crash
// consequence sites classify the crash into WALL / CAR / ROLLOVER.
static void wreck_begin_for(Vehicle* v, Vec3 contact_pt, Vec3 contact_n,
                            Vec3 rel_vel, B3WreckEntryKind kind);

static void wreck_begin_for(Vehicle* v, Vec3 contact_pt, Vec3 contact_n,
                            Vec3 rel_vel, B3WreckEntryKind kind) {
    int slot = (int)(v - g_vehicles);
    if (slot < 0 || slot >= 8) return;
    ai_crash_note(v, slot);
    // TAKEDOWN-FX: the big-hit IMPACT slow-down (divisor 6 for 0.35 s,
    // FUN_00026050's window [C]). Retail's raiser of the impact flag is
    // [?]; firing it at the player's own wreck moment is the GLUE trigger
    // for the recovered mechanism.
    // The impact window's only retail arm is the designated big-hit
    // TRAFFIC vehicle (FUN_00026A70, +0x174 & 8 -- RE_NOTES 16.3); a
    // plain crash keeps its own divisor-5 presentation.
    if (v == &g_player) b3_tdfx_crash_begin();
    float wr = g_car_wheel_radius[slot] > 0.05f ? g_car_wheel_radius[slot]
                                                : 0.35f;
    float hl = v->body_len > 3.6f ? v->body_len * 0.5f : 2.1f;
    float origin[3] = {v->pos.x, v->pos.y - 0.5f - g_car_ymin[slot],
                       v->pos.z};
    float bbmin[3] = {-0.95f, -wr, -hl};
    float bbmax[3] = {0.95f, 1.15f, hl};
    float velv[3] = {v->vel.x, v->vel.y, v->vel.z};
    float cp[3] = {contact_pt.x, contact_pt.y, contact_pt.z};
    float cn[3] = {contact_n.x, contact_n.y, contact_n.z};
    float rv[3] = {rel_vel.x, rel_vel.y, rel_vel.z};
    b3_wreck_begin_entry(&g_wrecks[slot], kind, origin, v->rot.y, velv,
                   v->cfg.mass_kg > 1.0f ? v->cfg.mass_kg : 1500.0f,
                   bbmin, bbmax, cp, cn, rv);
    /* PANELS: the ordinary crash entry's damage stamp.  FUN_00115130
     * @0x0011560B pushes 1 and calls FUN_001253C0 -- the CRUMPLE arm: every
     * panel state 0/1 -> 2, nothing detaches.  (FUN_001253C0(0), the
     * accumulators-to-1000 detach-all arm, belongs to the EXPLODE entry
     * FUN_00120800 @0x0012085E.)  So a car entering a crash still has all of
     * its bodywork; the panels come off through the flight's own impacts. */
    b3_panels_reset(&g_panels[slot], g_car_panel_count[slot],
                    g_car_panel_kind[slot], g_car_panel_pos[slot],
                    0x1234567u + (unsigned)slot);
    /* Retail's ORDER, which the one-frame rip test depends on:
     *  1  the crash-causing collision damages the struck side --
     *     FUN_00111CD0 -> FUN_0012FA40 -> FUN_0012C670.  The magnitude is
     *     that contact's normal momentum change, the same N.s quantity the
     *     resolver hands FUN_0012FA40.
     *  2  the crash entry's five-event burst, FUN_00115130 @0x00115265.
     *  3  the crumple stamp, FUN_001253C0(1) @0x0011561A.
     * Every add re-snapshots, so only step 2's LAST event is rip-tested by
     * the visual pass this same frame -- one or two panels leave at the
     * entry and the rest hang on a loaded accumulator for the flight. */
    {
        float cdot = rv[0]*cn[0] + rv[1]*cn[1] + rv[2]*cn[2];
        if (cdot < 0.0f) cdot = -cdot;
        b3_panels_impact_world(&g_panels[slot],
                               (const float (*)[4])g_wrecks[slot].frame, cn,
                               cdot * (v->cfg.mass_kg > 1.0f
                                       ? v->cfg.mass_kg : 1500.0f));
    }
    b3_panels_entry_burst(&g_panels[slot]);
    b3_panels_wreck_stamp(&g_panels[slot], /*detach_all=*/0);
    // CRASH-EVENT: the wreck is the same rigid body as the driving car, so
    // it uses the same compiled-in class inverse inertia (FUN_001203A0 [C]:
    // default 0.0008/0.0011/0.0013, HEVY trucks 0.0004/0.0006/0.0007) --
    // the same selection full_sim_reset() makes.
    {
        float iinv[3] = {0.0008f, 0.0011f, 0.0013f};
        if (v->info && strcmp(v->info->class_code, "HEVY") == 0) {
            iinv[0] = 0.0004f; iinv[1] = 0.0006f; iinv[2] = 0.0007f;
        }
        b3_wreck_set_inertia(&g_wrecks[slot], iinv);
    }
}

// (Re)place the full-pipeline vehicle model at the harness pose. Geometry is
// this car's own .bgv data (wheel attach positions +0xB80, radius +0x18,
// extents +0xE80/+0xE90); inverse inertia is the game's compiled-in class
// diagonal (FUN_001203A0 [C]: default 0.0008/0.0011/0.0013, HEVY trucks
// 0.0004/0.0006/0.0007, HEVYCAR5/6 0.00075/0.0008/0.0011).
//
// COORDINATES: the harness world is the game world with Z negated
// (RE_NOTES 12). The pipeline's cross-product algebra is chirality-bound
// (frame rows satisfy up = at x right in GAME space; a mirrored basis gets
// flipped upside down by the FUN_000ff270 orthonormalizer), so the physics
// state lives in GAME space and vehicle_update converts at the boundary:
// pos/vel z-negated in and out, steering negated in (mirror-odd input),
// and the ground hook maps game -> harness queries.
#define B3_CHASSIS_SOUP_MAX 32
#define B3_WHEEL_SOUP_MAX 1024
static B3CrashPoly g_chassis_poly[8][B3_CHASSIS_SOUP_MAX];
static unsigned short g_chassis_flag[8][B3_CHASSIS_SOUP_MAX];
typedef struct {
    B3CollisionPoly poly[B3_WHEEL_SOUP_MAX];
    int count;
} B3WheelSoup;
static B3WheelSoup g_wheel_soup[8];

static void harness_copy_soup(B3CrashPoly* poly, unsigned short* flag,
                              const B3CollisionPoly* gathered, int n) {
    for (int i = 0; i < n; i++) {
        const B3CollisionPoly* src = &gathered[i];
        const float* verts[3] = {src->v0, src->v2, src->v1};
        float* dst[3] = {poly[i].p0, poly[i].p1, poly[i].p2};
        for (int p = 0; p < 3; p++) {
            dst[p][0] = verts[p][0];
            dst[p][1] = verts[p][1];
            dst[p][2] = -verts[p][2];
            dst[p][3] = 0.0f;
        }
        poly[i].n[0] = src->normal[0];
        poly[i].n[1] = src->normal[1];
        poly[i].n[2] = -src->normal[2];
        poly[i].n[3] = 0.0f;
        flag[i] = src->type;
    }
}

static int harness_soup_ground_ray(void* user, const float start[3],
                                   const float end[3], float* hit_t,
                                   float normal[3]) {
    const B3WheelSoup* soup = user;
    return b3_collision_ray_polys_game_space(soup->poly, soup->count, start,
                                             end, hit_t, normal);
}

static int harness_soup_freeze(void* user, B3VehicleFull* fs) {
    Vehicle* v = (Vehicle*)user;
    int slot = (int)(v - g_vehicles);
    int n = 0;
    if (slot >= 0 && slot < 8 && b3_collision_ready()) {
        B3CrashPoly* poly = g_chassis_poly[slot];
        unsigned short* flag = g_chassis_flag[slot];
        B3CollisionPoly gathered[B3_CHASSIS_SOUP_MAX];
        float c[3] = { fs->rb.frame[3][0], fs->rb.frame[3][1],
                       -fs->rb.frame[3][2] };
        float half[3] = { 4.5f, 2.5f, 4.5f };
        float svel[3] = { fs->rb.vel[0], fs->rb.vel[1], -fs->rb.vel[2] };
        B3WheelSoup* wheel_soup = &g_wheel_soup[slot];
        float ground_half[3] = {5.5f, 34.0f, 5.5f};
        wheel_soup->count = b3_collision_gather(c, ground_half,
                                                 wheel_soup->poly,
                                                 B3_WHEEL_SOUP_MAX);
        n = b3_collision_filter_walls(wheel_soup->poly, wheel_soup->count,
                                      c, half, svel, 0.45f, gathered,
                                      B3_CHASSIS_SOUP_MAX);
        harness_copy_soup(poly, flag, gathered, n);
        fs->soup.polys = poly;
        fs->soup.flags = flag;
        fs->soup_ground_user = wheel_soup;
        fs->soup_ground_ray = harness_soup_ground_ray;
    } else {
        fs->soup_ground_user = NULL;
        fs->soup_ground_ray = NULL;
    }
    fs->soup.count = n;
    return n;
}

static void full_sim_reset(Vehicle* v) {
    int slot = (int)(v - g_vehicles);
    float wxz[4][2] = {{-0.76f, 1.24f}, {0.76f, 1.24f},
                       {-0.76f, -1.31f}, {0.76f, -1.31f}};
    int fi = 0, ri = 2;
    for (int w = 0; w < g_car_wheel_count[slot] && w < 6; w++) {
        float x = g_car_wheel_pos[slot][w][0];
        float z = -g_car_wheel_pos[slot][w][2];   // undo loader Z-flip
        if (g_car_wheel_front[slot][w]) {
            if (fi < 2) { wxz[fi][0] = x; wxz[fi][1] = z; fi++; }
        } else if (ri < 4) {
            wxz[ri][0] = x; wxz[ri][1] = z; ri++;
        }
    }
    float radius = g_car_wheel_radius[slot] > 0.05f
                 ? g_car_wheel_radius[slot] : 0.3117f;
    float ext[4], cen[4];
    if (g_car_ext[slot][2] > 0.1f) {
        for (int j = 0; j < 4; j++) {
            ext[j] = g_car_ext[slot][j];
            cen[j] = g_car_cen[slot][j];
        }
    } else {                                     // sidecar missing: derive
        float hl = v->body_len > 3.6f ? v->body_len * 0.5f : 2.1f;
        ext[0] = 1.0f; ext[1] = 1.1f; ext[2] = hl; ext[3] = 0.0f;
        cen[0] = -1.0f; cen[1] = -0.15f; cen[2] = -hl; cen[3] = hl;
    }
    float inv_inertia[3] = {0.0008f, 0.0011f, 0.0013f};
    if (v->info && strcmp(v->info->class_code, "HEVY") == 0) {
        inv_inertia[0] = 0.0004f;
        inv_inertia[1] = 0.0006f;
        inv_inertia[2] = 0.0007f;
    }
    // physics frame origin = wheel-hub plane = the render origin
    // (v->pos.y - 0.5 - g_car_ymin, see draw_vehicle). GAME space: z
    // negated; harness heading h maps to game heading h (fwd_h = M(fwd_g)).
    float pos[3] = {v->pos.x, v->pos.y - 0.5f - g_car_ymin[slot],
                    -v->pos.z};
    b3_vehicle_full_init(&v->fsim, &v->cfg, (const float(*)[2])wxz, radius,
                         ext, cen, inv_inertia, pos, v->rot.y);
    // +0x215 VEHICLE CLASS: retail stamps 1 on the player bodies
    // (FUN_00117730) and 2/3 on the two AI racer pools (FUN_00110280);
    // only class 3 skips the steer-away envelope. The init default (1)
    // had every AI car running the PLAYER class -- AI-vs-AI slams then
    // forced full-lock spins on cars whose driver never counter-steers,
    // crashing rivals constantly (user: "computer racers crash pretty
    // often without me doing anything"). Pool split across 2/3 is [S]
    // (membership of the two pools not yet recovered); alternating
    // matches the two-pool structure.
    v->fsim.class_215 = (slot == 0) ? 1
                      : (unsigned char)(2 + (slot & 1));
    // PHYS-LEDGER-3 / F2b: the FUN_0011AEF0 slot.  `soup_freeze` runs where
    // FUN_0011BC60 does (@0x0011BF43, once per frame, outside the substep
    // loop) and `chassis_resolve` where FUN_0011AEF0 does (@0x0011C0B7,
    // inside it, between the tyre force pass and the suspension pre-pass).
    // The response is b3_crash_response, ported verbatim.
    v->fsim.soup_user         = v;
    v->fsim.soup_freeze       = harness_soup_freeze;
    v->fsim.chassis_resolve   = b3_vehicle_chassis_contact;
    v->fsim.surface_grip_13A8 = 1.0f;   // veh+0x13A8, the SURFACE GRIP the
                                        // class-0 scrub multiplies; 1.0 is
                                        // tarmac (FUN_00123790 drops it to
                                        // 0.2 on the 0x26 wreck surface)
    v->fsim.authority_1534    = 1.0f;   // refreshed per frame from td_rules
    // racecar+0x1920 is the racecar MODE, not a per-car identity: 0 is the
    // normal race path, and it is the same 0 that makes FUN_0011BE50 take two
    // substeps at dt/2 (@0x0011BFF5) -- which b3_vehicle_step_full already
    // hard-codes.  So every car here is mode 0, and FUN_0011AEF0's `class-0`
    // test (`[racecar+0x1920] == 0`, ebx = veh @0x0011AF4F..0x0011AFEE) is
    // true for all of them.  The two fields must not disagree.
    v->fsim.racecar_class_1920 = 0;
    v->fsim.is_class0         = 1;
    v->fsim.party_mode        = 0;
    v->fsim.flags_1353        = 0;
    // ...and the wreck's world pass (PH-06, hunk F3).  Same reason it is
    // installed rather than linked: burnout3_crash.c is built WITHOUT
    // burnout3_vehicle_sim.c by tools/validate_td_rules.py.
    b3_wreck_set_world_resolve(b3_rigid_body_obb_plane_contact,
                               b3_rigid_body_world_contact);
    // Seed the body's velocity from the harness pose (harness -> game:
    // z negated) so relaunches happen AT SPEED -- the retail relaunch
    // places the car then sets speed via FUN_001204C0 (the constants at
    // FUN_00025500: 0x41569446 = 13.4112 = exactly 30 mph common case).
    {
        float nv[3] = { v->vel.x, v->vel.y, -v->vel.z };
        float ns = sqrtf(nv[0]*nv[0] + nv[1]*nv[1] + nv[2]*nv[2]);
        if (ns > 0.5f) {
            B3RigidBody* rb = &v->fsim.rb;
            for (int j = 0; j < 3; j++) {
                rb->vel[j] = nv[j];
                rb->dir[j] = nv[j] / ns;
            }
            rb->vel[3] = ns;
            // GLUE gear pick for a moving start; the pipeline's own shift
            // logic settles it within a few frames.
            v->fsim.trans.gear = ns > 18.0f ? 3 : 2;
        }
    }
    v->fsim_ready = 1;
}

/* FUN_00194380's job: a smooth along-track distance.  find_track_progress()
 * is quantised to route-point indices, which makes the block-range window
 * (car_length .. car_length + 15 m) flicker frame to frame; project onto the
 * segment instead. */
static float aggro_track_dist(Vec3 pos, int lap) {
    int n = g_track.num_points;
    if (n < 2 || !g_aggro_cum) return 0.0f;
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < n; i++) {
        float dx = pos.x - g_track.points[i].x;
        float dz = pos.z - g_track.points[i].z;
        float d = dx * dx + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    /* The nearest ROUTE POINT is not enough: when the car sits between
     * best-1 and best, clamping the projection onto [best, best+1] snaps the
     * distance backwards by a whole segment, and the block-range window
     * (car_length .. car_length + 15 m) flickers frame to frame.  Test both
     * adjacent segments and keep the one the car is actually on. */
    float bestd = 1e30f, out = 0.0f;
    for (int k = 0; k < 2; k++) {
        int i0 = (best + n - 1 + k) % n;
        int i1 = (i0 + 1) % n;
        Vec3 a = g_track.points[i0], b = g_track.points[i1];
        float sx = b.x - a.x, sz = b.z - a.z;
        float L2 = sx * sx + sz * sz;
        float t = 0.0f;
        if (L2 > 1e-6f) {
            t = ((pos.x - a.x) * sx + (pos.z - a.z) * sz) / L2;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
        }
        float px = a.x + sx * t - pos.x, pz = a.z + sz * t - pos.z;
        float d = px * px + pz * pz;
        if (d < bestd) {
            bestd = d;
            out = g_aggro_cum[i0] + t * sqrtf(L2);
            if (i0 > best) out -= g_aggro_track_len;   /* wrapped backwards */
        }
    }
    return (float)lap * g_aggro_track_len + out;
}
/* Build the per-frame aggression world.  GLUE inputs (data the harness does
 * not load, NOT rules): the per-opponent aggression (retail reads it from
 * the 0x98-byte per-slot record at DAT_0073A170 +0x90 x 0.01 via
 * FUN_00172870, and raises it on every hit in FUN_001989A0 @0x00198D95) --
 * standing in with the compiled chase-mode value 1.0 (0x003B168C, what
 * FUN_00172870 writes when racecar+0x2450 == 1); the road direction and the
 * along-track distance, taken from the harness route instead of the .bgd
 * node graph. */
static void aggro_world_build(float dt) {
    int n = g_track.num_points;
    if (!g_aggro_hit_init) {
        for (int i = 0; i < B3_AGGRO_MAX; i++)
            for (int k = 0; k < B3_AGGRO_MAX; k++)
                g_aggro_hit[i][k] = -1000.0f;
        g_aggro_hit_init = 1;
    }
    if (g_aggro_track_len <= 0.0f && n > 1) {
        float L = 0.0f;
        for (int i = 0; i < n; i++) {
            Vec3 a = g_track.points[i], b = g_track.points[(i + 1) % n];
            L += sqrtf((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
        }
        g_aggro_track_len = L > 1.0f ? L : 1.0f;
        g_aggro_cum = (float*)calloc((size_t)n + 1, sizeof(float));
        if (g_aggro_cum) {
            float c = 0.0f;
            for (int i = 0; i < n; i++) {
                g_aggro_cum[i] = c;
                Vec3 a = g_track.points[i], b = g_track.points[(i + 1) % n];
                c += sqrtf((b.x - a.x) * (b.x - a.x)
                         + (b.z - a.z) * (b.z - a.z));
            }
        }
    }
    int nc = g_num_vehicles < B3_AGGRO_MAX ? g_num_vehicles : B3_AGGRO_MAX;
    for (int i = 0; i < nc; i++) {
        Vehicle* v = &g_vehicles[i];
        B3AiAggroCar* c = &g_aggro_cars[i];
        memset(c, 0, sizeof *c);
        c->lateral_to = g_aggro_lat[i];
        c->last_hit   = g_aggro_hit[i];
        if (v->fsim_ready) {
            const B3RigidBody* rb = &v->fsim.rb;
            for (int j = 0; j < 3; j++) {
                c->pos[j]   = rb->frame[3][j];
                c->right[j] = rb->frame[0][j];
                c->fwd[j]   = rb->frame[2][j];
            }
        } else {
            c->pos[0] = v->pos.x; c->pos[1] = v->pos.y; c->pos[2] = -v->pos.z;
            c->fwd[0] = sinf(v->rot.y); c->fwd[2] = cosf(v->rot.y);
            c->right[0] = cosf(v->rot.y); c->right[2] = -sinf(v->rot.y);
        }
        c->pos[3] = 1.0f;
        /* road direction at this car (module frame: z negated) */
        if (n > 1) {
            int idx = (int)(v->track_progress * (float)n) % n;
            Vec3 a = g_track.points[idx], b = g_track.points[(idx + 3) % n];
            float dx = b.x - a.x, dz = -(b.z - a.z);
            float L = sqrtf(dx * dx + dz * dz);
            if (L > 1e-4f) { c->road_dir[0] = dx / L; c->road_dir[2] = dz / L; }
            else           { c->road_dir[2] = 1.0f; }
        } else {
            c->road_dir[0] = c->fwd[0];
            c->road_dir[1] = c->fwd[1];
            c->road_dir[2] = c->fwd[2];
        }
        c->speed_ms   = v->sim.speed;
        c->track_dist = aggro_track_dist(v->pos, v->lap);
        c->aggression = 1.0f;              /* GLUE stand-in, see above */
        c->car_width  = 1.9f;              /* racecar+0x2444 (.bgv body box) */
        c->car_length = v->body_len > 1.0f ? v->body_len : 4.4f;  /* +0x2448 */
        c->ooc_time   = v->slam_time;      /* (rc+0x1198)+0x1598 */
        c->slammed_time = v->slam_time;    /* racecar +0x16C0 */
        c->slammed_by = v->slam_by;        /* racecar +0x16BC */
        c->race_time  = g_race_time;
        c->boost_start = 0.0f;
        c->race_mode  = (v == &g_player) ? 0 : 1;
        c->car_class  = 1;
        c->mode2450   = 0;
        c->rival      = v->slam_by;
        c->wrecked    = (v->crashed_until > 0.0f);
        c->in_takedown = 0;
        c->boosting   = v->bar.boosting;
        c->race_progress_zero = 0;
        c->progress_gate = 1;
        c->no_slam_speed = 0;
        c->drift_zone = 0;
        c->node_open  = 1;
        c->steer_ok   = 1;
        c->player_slot = 0;
    }
    /* racecar+0x18A4[k]: our lateral offset to every other car (the table
     * FUN_00169BD0 sorts targets by). */
    for (int i = 0; i < nc; i++) {
        for (int k = 0; k < nc; k++) {
            if (i == k) { g_aggro_lat[i][k] = 1000.0f; continue; }
            float dx = g_aggro_cars[i].pos[0] - g_aggro_cars[k].pos[0];
            float dy = g_aggro_cars[i].pos[1] - g_aggro_cars[k].pos[1];
            float dz = g_aggro_cars[i].pos[2] - g_aggro_cars[k].pos[2];
            (void)dy;
            g_aggro_lat[i][k] = fabsf(dx * g_aggro_cars[i].right[0]
                                    + dz * g_aggro_cars[i].right[2]);
        }
    }
    g_aggro_world.cars = g_aggro_cars;
    g_aggro_world.ncars = nc;
    g_aggro_world.clock = g_race_time;
    g_aggro_world.dt = dt;
    g_aggro_world.track_loaded = 1;
}

/* AI AVOIDANCE (RE_AI section 15): the ported FUN_0016C450 chain.  Defined
 * after the traffic section (it reads g_traffic / the route helpers); the
 * arbitrator side of it runs from the AI branch below. */
typedef struct {
    int   valid;      /* a profile exists (built this frame or cached)      */
    int   state;      /* avoid+0x4AC: 0x10 = no threat, 1 / 2 = side chosen */
    int   override;   /* arbitrator commits the avoidance dir (FUN_0016ADF0)*/
    float aim[3];     /* avoid+0x460, world (harness space)                 */
    float speed;      /* avoid+0x488 -> AI+0x738 avoidance speed ceiling    */
    float risk_here;  /* risk on the car's own 0.2 m strip                  */
    float risk_lo, risk_hi;
    float dmin;       /* nearest occupier range inside the band (m)         */
} B3AiAvoidOut;
static void ai_avoid_update(Vehicle* v, int slot, int corner,
                            Vec3* target, float* ceiling, B3AiAvoidOut* out);

/* FUN_001714F0 + FUN_00179760 + FUN_00171650, as one call: put the car on the
 * nav graph `back` nodes behind `section/node`, zero the physics accumulators
 * (v+0xF0..+0x13C -- the harness re-inits the pipeline model, which is the
 * same thing), zero the direction block racecar+0x19C0..+0x19DC, reset the
 * navigator target cursor, and set racecar+0x245E so the next AI update is
 * skipped.  `speed_ms` < 0 leaves the velocity alone. */
static const char* g_replace_why = "?";
static int nav_replace_car(Vehicle* v, unsigned int section, unsigned int node,
                           int back, float speed_ms) {
    Vec3 rp;
    float yaw;
    /* Refuse a seed the cursor cannot vouch for.  Every re-place site has a
     * route-line fallback, and a cursor more than 50 m from the car is how
     * the harness used to teleport a whole grid two thirds of a lap. */
    if (nav_node_dist2(section, node, v->pos) > 2500.0f) return 0;
    {   static int nt = -1;
        if (nt < 0) nt = getenv("B3_NAV_TRACE") != NULL;
        if (nt) printf("[replace] t=%.2f car%d why=%s from %u/%u back %d "
                       "spd %.1f prog %.3f\n", g_race_time,
                       (int)(v - g_vehicles), g_replace_why, section, node,
                       back, speed_ms, v->track_progress);
    }
    unsigned short keep_s = v->nav_section, keep_n = v->nav_node;
    int keep_r = v->nav_ready;
    if (!nav_recovery_pose_from(v, section, node, back, &rp, &yaw)) {
        v->nav_section = keep_s; v->nav_node = keep_n; v->nav_ready = keep_r;
        return 0;
    }
    int slot = (int)(v - g_vehicles);
    {   /* The nav pair's y is the RIBBON's, not the drivable surface's, and
         * re-initing the pipeline model below the one-sided collision sinks
         * the car through it -- so the placement height has to come from the
         * collision probe.  It is only usable when the two AGREE: under an
         * elevated section the downward probe finds the road BELOW the
         * ribbon, and putting the car there puts it inside the deck, where
         * the push-out throws it to several hundred mph and it never comes
         * back (measured: one car frozen at 347 mph for two thirds of a
         * race).  Dropping it from the ribbon instead lands it in the same
         * geometry.  So when they disagree the re-place is REFUSED, and the
         * caller falls back to its route-line placement (crash recovery) or
         * simply leaves the car where it is (the stuck rescues). */
        float gh, gn[3];
        if (slot < 0 || slot >= 8
            || b3_ground_probe(rp.x, rp.y + 3.0f, rp.z, &gh, gn) < 0
            || fabsf(gh - rp.y) > B3_AI_OFFWORLD_DROP_M) {
            v->nav_section = keep_s; v->nav_node = keep_n;
            v->nav_ready = keep_r;
            return 0;
        }
        v->pos = rp;
        v->pos.y = gh + 0.5f + g_car_ymin[slot];
    }
    v->rot.y = yaw;
    v->rot.x = v->rot.z = 0.0f;
    if (speed_ms >= 0.0f) {
        v->vel = (Vec3){sinf(yaw) * speed_ms, 0.0f, -cosf(yaw) * speed_ms};
        v->sim.speed = speed_ms;
    }
    v->fsim_ready = 0;              /* re-init zeroes v+0xF0..+0x13C        */
    /* FUN_00179760: navigator +0x1D8 = 0xFFFF and the aggression sub-object
     * back to idle; FUN_0018CB60's take path zeroes the direction block. */
    v->nav_target_ready = 0;
    memset(v->ai.des_dir, 0, sizeof v->ai.des_dir);
    memset(v->ai.des_dir_n, 0, sizeof v->ai.des_dir_n);
    if (v->aggro_ready) b3_aggro_init(&v->aggro, g_race_time);
    v->ai.reverse_timer = -1.0f;
    v->stuck_time = 0.0f;
    v->stuck_ref = v->pos;
    v->stuck_ref_time = g_race_time;
    b3_ai_replace_done(&v->aiw);    /* racecar+0x245E = 1 @0x00171694       */
    return 1;
}

/* The harness's PRE-EXISTING placement: `back` route points behind the car's
 * own projected progress, heading along the route, snapped to the collision
 * ground.  This is the DEFAULT, and `nav_replace_car` above -- retail's
 * actual FUN_001714F0 + FUN_001709F0 walk over the .bgd graph -- is behind
 * B3_NAV_RESPAWN=1, because measured over 180 s races the graph placement is
 * the worse of the two: it drops cars into geometry often enough that one
 * ends up pinned at 340+ mph for two thirds of the race, and the rivals lose
 * roughly half their distance (0.75-1.10 net laps -> 0.16-0.84).  The
 * recovered code is complete and its LAW is validated; what it needs before
 * it can be the default is the same thing the off-world watchdog needed --
 * a nav ribbon whose height and lane agree with the drivable surface the
 * harness's collision world actually exposes. */
static int route_replace_car(Vehicle* v, int back, float speed_ms) {
    int slot = (int)(v - g_vehicles);
    int n2 = g_track.num_points;
    if (n2 <= 0 || slot < 0 || slot >= 8) return 0;
    int idx = (int)(find_track_progress(v->pos) * n2);
    int bi = ((idx - back) % n2 + n2) % n2;
    Vec3 rp = g_track.points[bi];
    Vec3 rq = g_track.points[(bi + 1) % n2];
    float yaw = atan2f(rq.x - rp.x, -(rq.z - rp.z));
    v->pos = rp;
    {   float gh, gn[3];
        if (b3_ground_probe(rp.x, rp.y + 3.0f, rp.z, &gh, gn) >= 0)
            v->pos.y = gh + 0.5f + g_car_ymin[slot];
    }
    v->rot.x = v->rot.z = 0.0f;
    v->rot.y = yaw;
    if (speed_ms >= 0.0f) {
        v->vel = (Vec3){sinf(yaw) * speed_ms, 0.0f, -cosf(yaw) * speed_ms};
        v->sim.speed = speed_ms;
    }
    v->fsim_ready = 0;
    /* FUN_00179760's navigator reset applies whichever placement is used. */
    v->nav_target_ready = 0;
    memset(v->ai.des_dir, 0, sizeof v->ai.des_dir);
    memset(v->ai.des_dir_n, 0, sizeof v->ai.des_dir_n);
    if (v->aggro_ready) b3_aggro_init(&v->aggro, g_race_time);
    v->ai.reverse_timer = -1.0f;
    v->stuck_time = 0.0f;
    v->stuck_ref = v->pos;
    v->stuck_ref_time = g_race_time;
    b3_ai_replace_done(&v->aiw);
    return 1;
}

static int b3_nav_respawn(void) {
    static int on = -1;
    if (on < 0) on = getenv("B3_NAV_RESPAWN") != NULL;
    return on;
}

static void vehicle_update(Vehicle* v, float dt) {
    if (!v->active) return;

    // Per-vehicle inputs: keyboard for the player, center-line following for AI.
    float throttle = 0.0f, brake = 0.0f, steer_input = 0.0f;
    int boost = 0;
    int ai_game_steer = 0;
    float ai_target_ms = -1.0f;   // AI speed target (racecar+0x23C4 analog)
    float ai_reverse_ms = 0.0f;   // >0: back out at this speed (the sim's
                                  // scalar speed cannot go negative, so the
                                  // real gear -1 reverse is applied
                                  // kinematically here -- GLUE)

    // B3_AUTODRIVE: the player car steers itself (smoke tests / demo attract).
    static int autodrive = -1;
    if (autodrive < 0) autodrive = getenv("B3_AUTODRIVE") != NULL;

    // TAKEDOWN-FX: while the cinematic owns the view the ATTACKER's car is
    // driven by the auto-driver, exactly as retail does -- the cinematic
    // entry sets racecar+0x27D8, whose only per-frame consumer routes to
    // the route-following driver (FUN_0018C510 @0x0018C53A -> FUN_00170820),
    // and the camera release restores steering authority (FUN_0018CB60).
    // [C flag semantics; the driver law below is the harness AI GLUE until
    // the real FUN_00170820 port lands.] Without this, control returns from
    // the cinematic with the car ploughed into whatever it drifted toward.
    if (!v->aiw_ready) {
        b3_ai_wheel_init(&v->aiw, (int)v->nav_node);
        v->aiw_ready = 1;
    }
    /* FUN_00170820's own head, before anything else it does: the 50 s / 50 s
     * route-alternation square wave on AI+0x1F0 (racecar+0x1BF0), reloaded
     * from DAT_003B16B8 = 50.0.  [C-disasm @0x00170827] */
    (void)b3_ai_route_alt(&v->aiw, dt);
    int td_ai_wheel = 0;
    if (v == &g_player) {
        B3TdfxStatus tst;
        b3_tdfx_status(&tst);
        /* FUN_0018CB60 is EDGE triggered on racecar+0x27D8: taking the wheel
         * zeroes racecar+0x19D0..+0x19DC and runs the navigator reset
         * FUN_00179760; giving it back writes v+0x1534 = 1.0 and clears
         * drift state 4.  The old code only carried a 1.2 s timer and never
         * restored the steering authority. */
        int want = (tst.active && tst.divisor > 1)
                 || g_race_time < v->ai_wheel_until;
        int edge = b3_ai_wheel_set(&v->aiw, &v->ai, want);
        if (edge == B3_AI_WHEEL_TAKE) {
            /* FUN_00179760: navigator +0x1D8 = 0xFFFF (re-seed the target
             * cursor) and the aggression sub-object back to idle. */
            v->nav_target_ready = 0;
            if (v->aggro_ready) b3_aggro_init(&v->aggro, g_race_time);
            memset(v->ai.des_dir, 0, sizeof v->ai.des_dir);
            memset(v->ai.des_dir_n, 0, sizeof v->ai.des_dir_n);
        }
        td_ai_wheel = v->aiw.ai_wheel;
    }

    if (v == &g_player && !autodrive && !td_ai_wheel) {
        if (g_keys[SDL_SCANCODE_W] || g_keys[SDL_SCANCODE_UP]) throttle = 1.0f;
        if (g_keys[SDL_SCANCODE_S] || g_keys[SDL_SCANCODE_DOWN]) brake = 1.0f;
        if (g_keys[SDL_SCANCODE_A] || g_keys[SDL_SCANCODE_LEFT]) steer_input = -1.0f;
        if (g_keys[SDL_SCANCODE_D] || g_keys[SDL_SCANCODE_RIGHT]) steer_input = 1.0f;
        if (g_keys[SDL_SCANCODE_SPACE]) boost = 1;
        if (g_pad) {   // retail Xbox layout; ANALOG steer/triggers
            float sx = pad_axis(SDL_CONTROLLER_AXIS_LEFTX, 0.15f);
            float rt = pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0.05f);
            float lt = pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 0.05f);
            if (sx != 0.0f) steer_input = sx;
            if (pad_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT))  steer_input = -1.0f;
            if (pad_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) steer_input = 1.0f;
            if (rt > throttle) throttle = rt;
            if (pad_btn(SDL_CONTROLLER_BUTTON_A)) throttle = 1.0f;
            if (lt > brake) brake = lt;
            if (pad_btn(SDL_CONTROLLER_BUTTON_B)) brake = 1.0f;
            if (pad_btn(SDL_CONTROLLER_BUTTON_X)) boost = 1;
        }
#ifdef __ANDROID__
        {   /* ANDROID PORT: tilt steering + on-screen buttons (b3_touch.c).
             * Merged exactly like the pad: analog steer wins over keys only
             * when non-zero, buttons OR into the digital inputs. */
            float t_steer, t_gas, t_brake;
            int   t_boost;
            b3_touch_state(&t_steer, &t_gas, &t_brake, &t_boost);
            if (t_steer != 0.0f)   steer_input = t_steer;
            if (t_gas > throttle)  throttle = t_gas;
            if (t_brake > brake)   brake = t_brake;
            if (t_boost)           boost = 1;
        }
#endif
        // Headless check of the player-input path (no window focus needed).
        static int testdrive = -1;
        if (testdrive < 0) testdrive = getenv("B3_TESTDRIVE") != NULL;
        if (testdrive) throttle = 1.0f;
    } else {
        // AI racer driver. Structure follows the real driver FUN_00105340
        // (reached via dispatcher FUN_00104D30 when racecar+0x134C != 0 and
        // +0x179C == 1); see docs/RE_AI.md. What the real function CONSUMES
        // -- the target steering angle (racecar+0x23C0, degrees) and target
        // speed (racecar+0x23C4, m/s) -- is produced upstream by the
        // node/lane targeting whose writer is not yet located, so the
        // look-ahead point + corner-angle measurement on the race line here
        // is GLUE standing in for that stage.
        /* FUN_0018D790 @0x0018D815: the live cursor walk FUN_00174960, and
         * @0x0018D840 the ONLY increment of racecar+0x1904 -- one per frame
         * in which that walk FAILS.  (Exhaustive disp32 sweep: RE_AI 16.) */
        int nav_walk_ok = nav_update_vehicle(v);
        /* AI+0x1F0 (FUN_00170820's square wave) and AI+0x1F1 (the
         * aggression machine's slam byte) are FUN_00178310's two mutable
         * gates -- RE_AI 16. */
        (void)nav_target_update(v, v->aiw.route_alt,
                                v->aggro_ready && v->aggro.state == 4);
        int n = g_track.num_points;
        int idx = (int)(find_track_progress(v->pos) * n);
        int lead = 5 + (int)(v->sim.speed * 0.10f);
        Vec3 target = g_track.points[(idx + lead) % n];
        float nav_corner_speed = 0.0f;
        unsigned int aim_section = v->nav_section, aim_node = v->nav_node;
        /* FUN_001772A0's planner window, behind B3_NAV_AIM=1 and OFF by
         * default.  It was effectively dead before this wave -- the nav
         * cursor was routinely hundreds of metres from its car (see the
         * runaway guard in nav_update_vehicle), so `plan->section` almost
         * never matched and the aim fell through to the route line.  With
         * the cursor corrected the planner window DOES select, and the
         * measured result is worse, not better: FUN_001781C0's pair
         * interpolation is clamped to 0.4..0.6 of the node pair, i.e. the
         * MIDDLE of the ribbon, and on this track the ribbon midline is the
         * median -- which is exactly the geometry `route_lane_fixup` exists
         * to steer the harness away from ("the corridor MIDLINE runs down
         * the median ... cars wedge in it").  Rivals lose about a third of
         * their race distance and gain about ten wall crashes per 180 s.
         * The selection itself is retail's; what is missing is the LANE
         * offset that decides where inside the pair the racing line runs
         * (FUN_00173E40 / FUN_00174610 / FUN_00174680's no-go offsets, still
         * unported).  Until those land the route line -- which
         * `route_lane_fixup` has already pulled onto the game's own forward
         * race line -- is the better aim. */
        static int nav_aim = -1;
        if (nav_aim < 0) nav_aim = getenv("B3_NAV_AIM") != NULL;
        if (nav_aim)
            (void)nav_plan_target(v, &target, &nav_corner_speed,
                                  &aim_section, &aim_node);

        // Wall-grind ARM (GLUE trigger, retail action): the driver's own
        // 5 mph stuck rule (FUN_00105340 @0x001054AF) cannot see a car
        // grinding a barrier at ~6 m/s, because the harness's mesh_collide
        // keeps reporting that speed while the position freezes.  Retail
        // has no equivalent -- so this only ARMS the driver's own reverse
        // burst (v+0x157C = 2.0) further down; the lateral aim offset and
        // the 12 m/s ceiling cap it used to raise are GONE.  Those were
        // pure invention and they fought the navigator: an aim shoved 8 m
        // sideways off the nav pair is exactly how a recovering car turns
        // back into the barrier it just left.
        {
            float mdx = v->pos.x - v->stuck_ref.x;
            float mdz = v->pos.z - v->stuck_ref.z;
            if (mdx * mdx + mdz * mdz > 36.0f
                || v->crashed_until > 0.0f       // parked: not "stuck"
                || g_race_time < 3.0f) {         // race launch grace
                v->stuck_ref = v->pos;
                v->stuck_ref_time = g_race_time;
            } else if (g_race_time - v->stuck_ref_time > 2.5f
                       && v->stuck_time <= B3_AI_STUCK_ARM_S) {
                v->stuck_time = B3_AI_STUCK_ARM_S + 0.01f;
                v->stuck_ref = v->pos;
                v->stuck_ref_time = g_race_time;
            }
        }

        // Anti-corner-cut: only when far off the line, gently bias the
        // pursuit point back toward it (prevents diving across kerbs
        // without fighting normal racing-line offsets). GLUE.
        int agg_aim_live = 0;
        {
            int _s = (int)(v - g_vehicles);
            agg_aim_live = (_s >= 0 && _s < g_aggro_world.ncars
                            && v->aggro_ready && v->aggro.aim_valid);
        }
        Vec3 nearpt = g_track.points[(idx + 2) % n];
        if (agg_aim_live) { nearpt = target; }
        float offx = nearpt.x - v->pos.x, offz = nearpt.z - v->pos.z;
        float offline = sqrtf(offx * offx + offz * offz);
        if (offline > 12.0f) {
            float wline = fminf(0.6f, (offline - 12.0f) / 15.0f);
            target.x = target.x * (1.0f - wline) + nearpt.x * wline;
            target.z = target.z * (1.0f - wline) + nearpt.z * wline;
        }

        // Barrier-aware aim (GLUE routing on the real collision mesh): if
        // the straight line to the pursuit point crosses a barrier (e.g.
        // the road-closure rows at junction mouths, which the corridor
        // midline threads at prog ~0.427), first fall back to the game's
        // own forward race line (Gamedata.bgd 0x1ABF20 -- it passes the
        // junction barriers on the open side by design), then slide the
        // aim laterally / closer until the ray is clear.
        if (aim_blocked(v->pos.x, v->pos.z, target.x, target.z,
                        v->pos.y + 0.5f)) {
            int rbest = 0;
            float rbd = 1e30f;
            for (int ri = 0; ri < B3_CENTERLINE_COUNT; ri++) {
                float rdx = B3_CENTERLINE[ri][0] - v->pos.x;
                float rdz = -B3_CENTERLINE[ri][2] - v->pos.z;   // z = -z
                float rd2 = rdx * rdx + rdz * rdz;
                if (rd2 < rbd) { rbd = rd2; rbest = ri; }
            }
            for (int ra = 6; ra <= 14; ra += 4) {
                const float* rp =
                    B3_CENTERLINE[(rbest + ra) % B3_CENTERLINE_COUNT];
                if (!aim_blocked(v->pos.x, v->pos.z, rp[0], -rp[2],
                                 v->pos.y + 0.5f)) {
                    target.x = rp[0];
                    target.z = -rp[2];
                    break;
                }
            }
        }
        if (aim_blocked(v->pos.x, v->pos.z, target.x, target.z,
                        v->pos.y + 0.5f)) {
            Vec3 rn = g_track.points[(idx + lead + 2) % n];
            Vec3 rd = vec3_normalize(vec3_sub(rn, target));
            float px = -rd.z, pz = rd.x;
            static const float offs[] = {4, -4, 8, -8, 12, -12, 16, -16};
            int found = 0;
            for (int ci = 0; ci < 2 && !found; ci++) {
                // pass 0: full lookahead; pass 1: halfway point
                Vec3 base = target;
                if (ci == 1) {
                    base.x = (v->pos.x + target.x) * 0.5f;
                    base.z = (v->pos.z + target.z) * 0.5f;
                }
                for (unsigned oi = 0; oi < sizeof offs / sizeof *offs; oi++) {
                    float tx = base.x + px * offs[oi];
                    float tz = base.z + pz * offs[oi];
                    if (!aim_blocked(v->pos.x, v->pos.z, tx, tz,
                                     v->pos.y + 0.5f)) {
                        target.x = tx;
                        target.z = tz;
                        found = 1;
                        break;
                    }
                }
            }
        }
        float want = atan2f(target.x - v->pos.x, -(target.z - v->pos.z));
        float err = angle_diff(want, v->rot.y);

        // Upcoming corner sharpness: heading change of the line over the
        // braking horizon (GLUE measurement feeding the real speed map).
        Vec3 a0 = g_track.points[(idx + 2) % n];
        Vec3 a1 = g_track.points[(idx + 8) % n];
        Vec3 a2 = g_track.points[(idx + 14 + (int)(v->sim.speed * 0.20f)) % n];
        float h01 = atan2f(a1.x - a0.x, -(a1.z - a0.z));
        float h12 = atan2f(a2.x - a1.x, -(a2.z - a1.z));
        float curve = fabsf(angle_diff(h12, h01));

        // ---- The arbitrated SPEED CEILING (AI+0x780). In retail this is
        // min(avoidance speed, AI+0x1D0), with FUN_00176150 applying the
        // native-m/s planner corner speed over the approach distance. The
        // curvature scan remains only when the .bgd planner has no record.
        float ceiling;
        {
            if (nav_corner_speed > 0.0f) {
                /* FUN_00176150 [C-disasm], see b3_ai_corner_brake:
                 *   AI+0x1D0 = factor * (cs - D) + cs, capped by AI+0xA08,
                 *   D = FUN_00174AF0 = the SIGNED longitudinal offset of the
                 *   car from the node's A point along the node forward
                 *   direction, clamped by that node's segment length.
                 * The old line used +|target - pos| with the subtraction
                 * REVERSED, which is 1.2 x cs lower everywhere and collapses
                 * to 0.4 x cs on top of the node -- a permanent 4..9 mph
                 * crawl, because FUN_00105340's stuck detector is disarmed
                 * below a zero target speed. */
                float dd = nav_approach_dist(aim_section, aim_node, v->pos);
                ceiling = b3_ai_corner_brake(nav_corner_speed, dd,
                                             /*boost_scale=*/0,
                                             B3_AI_TOP_SPEED_MPS,
                                             /*target_mode=*/0,
                                             /*mode_1fc=*/0);
            } else {
                int horiz = 8 + (int)(v->sim.speed * 0.55f);
                if (horiz > 70) horiz = 70;
                ceiling = B3_AI_TOP_SPEED_MPS;
                for (int li = 2; li < horiz; li += 3) {
                    Vec3 c0 = g_track.points[(idx + li) % n];
                    Vec3 c1 = g_track.points[(idx + li + 4) % n];
                    Vec3 c2 = g_track.points[(idx + li + 8) % n];
                    float ha = atan2f(c1.x - c0.x, -(c1.z - c0.z));
                    float hb = atan2f(c2.x - c1.x, -(c2.z - c1.z));
                    float cv = fabsf(angle_diff(hb, ha)) * RAD_TO_DEG;
                    float k2 = cv / B3_AI_ANGLE_MIN_SPEED_DEG;
                    if (k2 > 1.0f) k2 = 1.0f;
                    float lim = B3_AI_TOP_SPEED_MPS
                              + (B3_AI_MIN_SPEED_MPS - B3_AI_TOP_SPEED_MPS) * k2;
                    float away = (float)(li - 2) / (float)(horiz > 3 ? horiz - 2 : 1);
                    lim += (B3_AI_TOP_SPEED_MPS - lim) * away * 0.55f;
                    if (lim < ceiling) ceiling = lim;
                }
            }
            ceiling = b3_ai_avoid_speed_cap(ceiling, nearest_car_ahead(v));
            /* Queue behind a nearly-stopped racer dead ahead.  This is GLUE
             * -- retail's close-range rule is FUN_0016C4B0's ladder, which
             * floors at "Speed when car is <10m away" = 26.2 m/s and relies
             * on the avoidance stage steering AROUND (RE_AI 15.4) -- and the
             * harness needs it because its pack does not spread.  But the
             * zero it writes VIOLATES a retail invariant, and that is worth
             * recording: FUN_00172E80 returns min(Min speed + t*S, S, cap),
             * so S = 0 makes the whole demand 0, and FUN_00105340's stuck
             * detector is then disarmed by its own second test
             * (`COMISS [ECX+0x23C4],0 / JBE` @0x001054DA) -- the 1 s arm
             * never starts and the 2 s reverse burst never fires.  Retail's
             * own design guarantees S > 0 (AI+0x1D0 is
             * (1+factor)*corner_speed + factor*|D|, RE_AI 16), so nothing
             * here is ported: it is GLUE covering for a pack that does not
             * spread.  The zero stays anyway, because both alternatives were
             * MEASURED over 180 s races and both are worse -- deleting the
             * rule doubles the wall crashes (26 -> 56) as the pack rear-ends
             * itself, and flooring it at 5 mph (44 crashes) still creeps into
             * the car in front.  What actually breaks the deadlock is the
             * harness's own position-based wall-grind arm below, which drops
             * the driver into its retail reverse burst after 2.5 s of not
             * moving.  The real fix is the lateral spread FUN_0016C450 is
             * supposed to produce (RE_AI 14.11).
             * The 12 m/s cap the wall-grind glue used to raise is gone. */
            float ffx = sinf(v->rot.y), ffz = -cosf(v->rot.y);
            for (int qi = 0; qi < g_num_vehicles; qi++) {
                const Vehicle* q = &g_vehicles[qi];
                if (q == v || !q->active) continue;
                if (q->sim.speed > 6.0f) continue;
                float qx = q->pos.x - v->pos.x, qz = q->pos.z - v->pos.z;
                float qd = sqrtf(qx * qx + qz * qz);
                if (qd < 9.0f && qd > 0.1f
                    && (qx * ffx + qz * ffz) / qd > 0.75f) {
                    ceiling = 0.0f;
                    break;
                }
            }
        }

        // ---- RECOVERED AI DRIVER (src/burnout3_ai.c, validate_ai 161/161):
        // FUN_0016AE20 -> FUN_00171E30 -> FUN_001724F0 -> FUN_00105340.
        if (!v->ai_ready) { b3_ai_state_init(&v->ai); v->ai_ready = 1; }
        // ---- RECOVERED AGGRESSION MACHINE (RE_AI section 14):
        // FUN_00169540 -> aim override (FUN_0016AE20 @0x0016AE2C reads the
        // machine's aim point) + FUN_0016AF10 -> FUN_00172FA0 speed +
        // FUN_00171D90 boost latch.
        int agg_slot = (int)(v - g_vehicles);
        int agg_boost = 0;
        if (!v->aggro_ready) {
            b3_aggro_init(&v->aggro, g_race_time);
            b3_aggro_speed_init(&v->aggspd);
            v->aggro_ready = 1;
        }
        static int agg_off = -1;
        if (agg_off < 0) agg_off = getenv("B3_AGGRO_OFF") != NULL;
        if (!agg_off && agg_slot >= 0 && agg_slot < g_aggro_world.ncars
            && v->crashed_until <= 0.0f) {
            int agg_prev = v->aggro.state;
            b3_aggro_update(&v->aggro, &g_aggro_world, agg_slot);
            if (v->aggro.state == 4 && v->aggro.aim_valid) {
                g_aggro_shot_req = 1;
                g_aggro_cam_slot = agg_slot;
            }
            if (agg_prev != v->aggro.state && getenv("B3_AGGRO_TRACE"))
                printf("[AGGRO] t=%.2f car %d state %d -> %d tgt %d "
                       "lat %.2f lon %.2f cs %d br %d wss %d aimv %d "
                       "blocked %d ahead %.2f len %.2f dmph %.1f\n",
                       g_race_time, agg_slot, agg_prev, v->aggro.state,
                       v->aggro.target, v->aggro.lateral,
                       v->aggro.longitudinal, v->aggro.can_slam,
                       v->aggro.block_range, v->aggro.want_slam_speed,
                       v->aggro.aim_valid, v->aggro.blocked,
                       v->aggro.target >= 0
                         ? g_aggro_cars[agg_slot].track_dist
                           - g_aggro_cars[v->aggro.target].track_dist : 0.0f,
                       g_aggro_cars[agg_slot].car_length,
                       v->aggro.target >= 0
                         ? 2.2369363f * (g_aggro_cars[agg_slot].speed_ms
                           - g_aggro_cars[v->aggro.target].speed_ms) : 0.0f);
            /* FUN_0016AE20 @0x0016AE2C tests ONLY aggro+0x20.  aggro+0x23
             * ("blocked") has a single reader in the whole image,
             * FUN_00105BD0 @0x00105EBC, and it is not the aim gate. */
            if (v->aggro.aim_valid) {
                // the aggression aim replaces the racing line, exactly as
                // FUN_0016AE20 does when AI+0x190 is set
                target.x = v->aggro.aim[0];
                target.z = -v->aggro.aim[2];
            }
            int agg_tgt = -1;
            v->aggspd.mode = b3_aggro_arbitrate(&v->aggro, &g_aggro_world,
                                                agg_slot, &agg_tgt);
            v->aggspd.target = agg_tgt;
        }
        /* ---- AI AVOIDANCE (RE_AI section 15).  Retail order inside the
         * arbitrator FUN_0016AAC0: the target follower runs, then
         * FUN_0016C450 (avoidance), then the ceiling becomes
         * min(AI+0x1D0 corner brake, AI+0x738 avoidance) [C @0x0016AB16],
         * and only then does the risk test decide whether to commit the
         * racing/aggression aim (FUN_0016AE20) or the avoidance direction
         * (FUN_0016ADF0).  So this sits AFTER the aggression aim override,
         * exactly as FUN_0016AE20 does. */
        {
            B3AiAvoidOut av;
            ai_avoid_update(v, agg_slot, curve > 0.25f, &target, &ceiling,
                            &av);
            (void)av;
        }
        if (v->fsim_ready) {
            B3RigidBody* arb = &v->fsim.rb;
            B3AiCar ac;
            B3AiInputs ain;
            memset(&ac, 0, sizeof ac);
            for (int j = 0; j < 3; j++) {
                ac.pos[j]       = arb->frame[3][j];
                ac.right[j]     = arb->frame[0][j];
                ac.fwd[j]       = arb->frame[2][j];
                ac.veh_right[j] = arb->frame[0][j];
                ac.veh_fwd[j]   = arb->frame[2][j];
                ac.car_at[j]    = arb->dir[j];
            }
            ac.speed_ms       = arb->vel[3];
            ac.yaw_rate       = arb->omega[1];
            ac.engine_rpm     = v->fsim.trans.omega * 9.549296f;
            ac.change_up_rpm  = v->fsim.trans.change_up_rpm;
            ac.drift_state    = v->fsim.drift_state_1524;
            ac.gear           = v->fsim.trans.gear;
            ac.lsdm_limit_mph = v->fsim.lsdm_limit_13AC;
            ac.race_mode      = 1;
            ac.crash_timer    = (v->crashed_until > 0.0f) ? 0.0f : 1.0f;
            ac.boosting       = v->bar.boosting;
            ac.clock          = g_race_time;
            ac.ooc_window = (v->slam_time >= 0.0f
                        && g_race_time <= v->slam_time + B3_TOTAL_OOC_TIME_S);
            ac.ooc_mode = 2;
            float tp[3] = { target.x, arb->frame[3][1], -target.z };
            // GLUE trigger, real action: the harness's position-based
            // wall-grind detector arms the driver's own reverse burst
            // (v+0x157C = 2.0), which the 5 mph rule cannot see.
            if (v->stuck_time > B3_AI_STUCK_ARM_S && v->ai.reverse_timer < 0.0f) {
                v->ai.reverse_timer = B3_AI_REVERSE_S;
                v->stuck_time = 0.0f;
            }
            // Stuck RESCUE (FUN_00170820 @0x001708EE [C-disasm], RE_AI 16):
            //   CMP dword [ESI+0x1904],0xC8 / JLE
            //   MOVZX EAX,word [ESI+0x245A] / SUB EAX,8 / CALL FUN_001714F0
            //   FUN_001204C0(v, DAT_0047A150 * 0x003A5958)   = 8.9408 m/s
            // racecar+0x1904 counts NAV-WALK FAILURES, not slow frames --
            // the four references in the whole image are the spawn init,
            // FUN_0018D790's `inc` on a failed FUN_00174960, this test, and
            // this clear.  The old code counted "under 5 mph" frames, which
            // is FUN_00105340's SEPARATE reverse-burst rule (already ported
            // in b3_ai_drive); doubling it up here teleported cars that were
            // merely waiting in traffic.  The re-place is now relative to
            // racecar+0x245A, the last node at which the car was ON the road.
            {
                unsigned int rs = v->nav_last_ready ? v->nav_last_section
                                                    : v->nav_section;
                unsigned int rn = v->nav_last_ready ? v->nav_last_node
                                                    : v->nav_node;
                if (b3_ai_navfail(&v->aiw, nav_walk_ok)) {
                    g_replace_why = "navfail200";
                    if ((b3_nav_respawn()
                         && nav_replace_car(v, rs, rn,
                                            B3_AI_RESCUE_BACK_NODES,
                                            B3_AI_RESCUE_SPEED_MS))
                        || route_replace_car(v, B3_AI_RESCUE_BACK_NODES,
                                             B3_AI_RESCUE_SPEED_MS))
                        printf("[Burnout3] t=%.2f stuck rescue: car %d "
                               "respawned %d nodes back (nav-walk failed "
                               "%d frames)\n", g_race_time,
                               (int)(v - g_vehicles), B3_AI_RESCUE_BACK_NODES,
                               B3_AI_NAVFAIL_FRAMES);
                }
                /* HARNESS-ONLY position rescue, on its own counter.  Retail
                 * does not have one: FUN_00105340's 5 mph reverse burst frees
                 * its cars, and racecar+0x1904 (ported above) counts NAV-WALK
                 * failures, which the harness's always-seedable cursor almost
                 * never produces.  The harness DOES need one -- mesh_collide
                 * reports ~6 m/s for a car whose position is frozen against a
                 * barrier, and a car wedged between two others never reverses
                 * out -- and dropping it was measured to cost the rivals half
                 * their race distance (0.75-1.10 net laps over 180 s down to
                 * 0.30-0.46) and to leave them stalled 26-31% of the time
                 * instead of 16-21%.  Only the TRIGGER is glue: the response
                 * is the recovered re-place, eight nodes behind
                 * racecar+0x245A at "Min speed mps" x 0.44704 = 8.9408 m/s. */
                int stuck_now = v->crashed_until <= 0.0f
                             && v->sim.speed * 2.2369363f < 5.0f;
                v->stuck_frames = stuck_now ? v->stuck_frames + 1 : 0;
                /* ... and the same rescue on the POSITION test, because the
                 * speed test cannot see the harness's other wedge mode: a car
                 * jammed in geometry keeps a large internal speed while its
                 * position is frozen (measured: one car reading 347 mph and
                 * not moving for two thirds of a race, which no 5 mph rule
                 * will ever reach).  stuck_ref_time is the clock at which the
                 * car last moved 6 m, so this is "has not moved 6 m in 8 s". */
                int pinned = v->crashed_until <= 0.0f && g_race_time > 3.0f
                          && g_race_time - v->stuck_ref_time > 8.0f;
                if (v->stuck_frames > B3_AI_NAVFAIL_FRAMES || pinned) {
                    v->stuck_frames = 0;
                    g_replace_why = pinned ? "pinned8s" : "stuck200";
                    if ((b3_nav_respawn()
                         && nav_replace_car(v, rs, rn,
                                            B3_AI_RESCUE_BACK_NODES,
                                            B3_AI_RESCUE_SPEED_MS))
                        || route_replace_car(v, B3_AI_RESCUE_BACK_NODES,
                                             B3_AI_RESCUE_SPEED_MS)) {
                        v->stuck_ref = v->pos;
                        v->stuck_ref_time = g_race_time;
                    }
                }
            }
            b3_ai_update(&v->ai, &ac, &ain, tp, ceiling, 0.0f, dt);
            /* FUN_001724F0's own order: the aggression leg REPLACES the
             * corner-law speed, then the Min speed mps floor, then the
             * driver runs again on the new demand. */
            if (v->aggspd.mode != 0) {
                float m = b3_ai_aggro_speed(&v->aggspd, &g_aggro_world,
                                            agg_slot,
                                            g_aggro_cars[agg_slot].aggression,
                                            v->ai.corner_speed);
                if (m < b3_ai_params.min_speed_mps)
                    m = b3_ai_params.min_speed_mps;
                v->ai.target_speed = m;
                b3_ai_drive(&v->ai, &ac, &ain, dt,
                            v->ai.des_dir_n[0] * ac.right[0]
                          + v->ai.des_dir_n[1] * ac.right[1]
                          + v->ai.des_dir_n[2] * ac.right[2]);
            }
            b3_ai_boost_latch(&v->aggspd, g_race_time);
            agg_boost = v->aggspd.wants_boost;
            throttle      = ain.throttle;
            brake         = ain.brake;
            steer_input   = ain.steer;
            ai_game_steer = 1;
            // racecar+0x2419 comes from the recovered FUN_00171D90 latch
            // now; the GLUE heuristic is the fallback when the aggression
            // machine is idle.
            boost         = agg_boost
                          || (fabsf(err) < 0.15f && curve < 0.2f
                              && v->sim.speed > 25.0f);
            ai_target_ms  = -1.0f;   // governor OFF (audit verdict item 2)
            if (ain.gear_request != 0)
                v->fsim.trans.gear = ain.gear_request;
            {   /* B3_AI_WHY=1: one line per car per second naming every term
                 * that can hold a rival at a standstill.  Diagnostic only. */
                static int why = -1;
                if (why < 0) why = getenv("B3_AI_WHY") != NULL;
                if (why && g_frame_count % 60 == 0)
                    printf("[why] t=%.1f car%d spd %.1f ceil %.1f tspd %.1f "
                           "cspd %.1f cs %.1f dd %.1f thr %.2f brk %.2f "
                           "str %.2f rev %.2f arm %.2f gear %d crash %.1f "
                           "auth %.2f aim %.1f\n",
                           g_race_time, agg_slot, v->sim.speed, ceiling,
                           v->ai.target_speed, v->ai.corner_speed,
                           nav_corner_speed,
                           nav_approach_dist(aim_section, aim_node, v->pos),
                           ain.throttle, ain.brake, ain.steer,
                           v->ai.reverse_timer, v->ai.stuck_arm,
                           v->fsim.trans.gear, v->crashed_until,
                           v->ai.steer_authority,
                           sqrtf((target.x - v->pos.x) * (target.x - v->pos.x)
                               + (target.z - v->pos.z) * (target.z - v->pos.z)));
            }
        }
    }

    // CRASH-EVENT: a crashed car is a physical wreck until its recovery
    // clock (the +5 s value is the game's own AI recovery write,
    // FUN_00198E60: victim+0x240C = clock + 5.0). Its motion while down is
    // the real machine's shape -- there is NO separate crash integrator in
    // the binary: FUN_00123000 keeps running the rigid-body step with crash
    // damping, and FUN_0011AEF0 keeps resolving chassis contacts (see
    // RE_NOTES 16). The wreck sim below mirrors that with the ported
    // impulse/damping laws around the verified integrator.
    if (v->crashed_until > 0.0f) {
        // Recovery clock (1:1): retail stamps 5 GAME seconds on the DILATED
        // clock (FUN_00198E60 @0x00198F65: racecar+0x10DC = clock + 5.0). At
        // the divisor-5 crash presentation, 5 game-s = 25 s wall -- the car
        // is a physical wreck that long, then the crash-exit FUN_00119C00
        // (gated on racecar+0x19BE, set by the reset FUN_0018D0E0) re-places
        // it on the road at 30 mph. The old 5 s WALL cap (crash_real_left)
        // was a hedge against an "unmodelled early releaser"; that hypothesis
        // is now REFUTED -- +0x19BE is set by the off-road re-placer
        // FUN_0018D0E0 <- FUN_001706A0 (0x18c493), NOT a crash timer. The
        // release is therefore driven purely by the game-time stamp below
        // (g_race_time >= crashed_until), matching retail's 25 s wall.
        if (v == &g_player && getenv("B3_CRASH_TIMING")) {
            static float wall0 = -1.0f, game0 = -1.0f;
            static int was = 0;
            int now = g_race_time < v->crashed_until;
            if (now && !was) { wall0 = g_real_clock_dbg; game0 = g_race_time;
                printf("[crashtime] ENTER wall=%.2f game=%.2f div=%d\n",
                       wall0, game0, b3_tdfx_divisor()); }
            if (!now && was) printf("[crashtime] EXIT  wall=%.2f (%.2f s WALL, "
                                    "%.2f s GAME) div=%d\n",
                                    g_real_clock_dbg, g_real_clock_dbg - wall0,
                                    g_race_time - game0, b3_tdfx_divisor());
            was = now;
        }
        if (g_race_time < v->crashed_until) {
            throttle = 0.0f;
            brake = 1.0f;
            boost = 0;
        } else {
            v->crashed_until = 0.0f;
            // Recovery: FUN_001714F0 places the car back ON THE ROAD a few
            // nodes behind (RE_AI 10: set node, re-walk links, zero the
            // accumulators; FUN_00179760 places ~3 nodes back). The old
            // recovery kept the wreck's rest position, which after a real
            // launch can be far off the map. GLUE boundary: route points
            // stand in for the .bgd nav nodes.
            int slot = (int)(v - g_vehicles);
            if (slot >= 0 && slot < 8 && g_wrecks[slot].active) {
                g_wrecks[slot].active = 0;
                /* PANELS: respawn repairs the car (FUN_000241A0 puts
                 * health back to 1.0 and FUN_0012FEE0 re-rolls the damage
                 * ctx), so every panel returns and the thresholds re-roll. */
                b3_panels_reset(&g_panels[slot], g_car_panel_count[slot],
                                g_car_panel_kind[slot], g_car_panel_pos[slot],
                                0x1234567u + (unsigned)slot);
                if (v == &g_player) b3_tdfx_crash_end();  /* FUN_00119C00 */
                /* CRASH-CINEMA: FUN_00119C00 also clears the
                 * aftertouch qualifier veh+0x4AC5 @0x00119C87. */
                b3_wreck_aftertouch_reset(&g_wrecks[slot]);
                // PH-10: the placement is now retail's own -- walk 3 nodes
                // back over the .bgd graph from the LAST NODE at which the
                // car was on the road (racecar+0x245A, latched by
                // FUN_001712E0), take FUN_00174740's four-pair forward
                // difference for the heading and FUN_001781C0's pair
                // interpolation for the position, zero the physics
                // accumulators, run FUN_00179760's navigator reset and set
                // the FUN_00171650 skip-one-frame latch.  Relaunch speed is
                // FUN_001204C0's 0x41569446 = 13.4112 m/s = 30 mph [C].
                // (Route points used to stand in for the nav nodes here; a
                // wreck's rest position is a bad seed for that search, which
                // is exactly why the recovered car sometimes faced the way it
                // had been thrown.)
                unsigned int rs = v->nav_last_ready ? v->nav_last_section
                                                    : v->nav_section;
                unsigned int rn = v->nav_last_ready ? v->nav_last_node
                                                    : v->nav_node;
                g_replace_why = "crashrecover";
                if (!b3_nav_respawn()
                    || !nav_replace_car(v, rs, rn, 3, 13.4112f)) {
                    int n2 = g_track.num_points;
                    int ni = (int)(find_track_progress(v->pos) * n2);
                    int back = (ni - 3 + n2) % n2;
                    Vec3 rp = g_track.points[back];
                    Vec3 rq = g_track.points[(back + 1) % n2];
                    v->pos = rp;
                    {
                        float gh, gn[3];
                        if (b3_ground_probe(rp.x, rp.y + 3.0f, rp.z,
                                            &gh, gn) >= 0)
                            v->pos.y = gh + 0.5f + g_car_ymin[slot];
                    }
                    v->rot.x = v->rot.z = 0.0f;
                    v->rot.y = atan2f(rq.x - rp.x, -(rq.z - rp.z));
                    v->vel = (Vec3){sinf(v->rot.y) * 13.4112f, 0.0f,
                                    -cosf(v->rot.y) * 13.4112f};
                    v->sim.speed = 13.4112f;
                    v->fsim_ready = 0;
                }
                // User 2026-08-13: IMMEDIATE control after a crash
                // reset (no AI handover); the car is already placed on a
                // good heading at 30 mph, and immune_until keeps the
                // short crash immunity.
                v->ai_wheel_until = 0.0f;
            }
        }
    }

    // --- Boost bar: the game's own rules (burnout3_gameplay.h, all
    // execution-verified). Input engages via FUN_0017A5B0's gate, the meter
    // drains per FUN_0017A480, and the transmission consumes the boosting
    // flag exactly like the real input bit v+0x13FC & 4.
    int was_boosting = v->bar.boosting;
    if (boost) b3_boost_engage(&v->bar, g_race_time);
    else       b3_boost_release(&v->bar, g_race_time);  // hold-to-boost
    b3_boost_update(&v->bar, g_race_time, dt);
    v->last_brake = brake;   // CARFX: brake-light state for the coronas
    /* SFX: BOOSTGAIN on ignition, BOOSTLOSS when the burn ends
     * (FUN_00140DF0 / FUN_00140EF0). Player only -- these are 2D UI-side
     * sounds in the game, not positioned. */
    if (v == &g_player) {
        if (!was_boosting && v->bar.boosting)
            b3_sfx_event(B3_SFX_BOOST_GAIN, 0.0f);
        else if (was_boosting && !v->bar.boosting)
            b3_sfx_event(B3_SFX_BOOST_LOSS, 0.0f);
    }
    v->boost = (float)v->bar.boosting;
    v->boost_meter = v->bar.meter;

    // Per-frame takedown window expiry (FUN_00199080).
    b3_takedown_frame(&v->score, g_race_time);

    // EARN EVENTS: air / oncoming / drift, then near miss vs traffic.
    // Both award boost through b3_boost_award into the same record.
    // FUN_001935F0 @0x001939AD [C]: while crashed (+0x18FA) or being
    // respawned this frame (+0x18FB) the whole detector block is skipped
    // and pending near-miss slots are DESTROYED unpaid. fsim_ready == 0 is
    // the harness's respawn-frame signal (recovery teleports then re-inits),
    // preventing the teleport distance from scoring as AIR/ONCOMING.
    b3_score_events_set_crash(&v->sev, v->crashed_until > 0.0f,
                              v->fsim_ready == 0);
    score_events_update(v);
    score_near_miss_update(v);
    /* SCORE-CLASSIFIER: RUBBING (FUN_00194A80) and FUN_001935F0's tail
     * @0x001940A1, which rotates the per-opponent in-contact flags.  Both
     * sit inside the branch the crash gate jumps over, so the module's own
     * suspension check covers them. */
    b3_score_events_set_race_finished(&v->sev, g_state == FINISHED);
    {
        unsigned char slam[B3_SE_RUB_CARS];
        score_rub_slam((int)(v - g_vehicles), slam);
        b3_score_events_rubbing(&v->sev, &v->bar, g_race_time,
                                g_delta_time, slam, B3_SE_RUB_CARS);
        b3_score_events_frame_end(&v->sev);
    }
    v->boost_meter = v->bar.meter;

    // TAKEDOWN-FX: FUN_00027AD0 aborts the cinematic when the attacker
    // crashes (racecar+0x18FA) or the race ends (racecar+0x134C == 3).
    if (v == &g_player) {
        b3_tdfx_set_attacker_crashed(v->crashed_until > 0.0f);
        b3_tdfx_set_race_finished(g_state == FINISHED);
        // "Impact Time": while the player's own car is crashed the boost
        // button requests divisor 5 (FUN_00118410 @ 0x001188A4).
        // Crash dilation: only the verified paths remain -- the 0.35 s
        // impact hit (divisor 6) at the wreck moment, and divisor-5
        // aftertouch while the BOOST button is held (FUN_00118410 [C]).
        // A plain crash tumbles at full speed and weight.
        b3_tdfx_set_aftertouch(v->crashed_until > 0.0f, boost);
    }

    // Deferred takedown commit: the attacker must stay clear of a crash for
    // Race Car Clear Wait (0.5 s) after the victim goes down (the score
    // module's +0x4D8 timer scan, FUN_00197040); crashing cancels it.
    if (v->pending_td_victim >= 0) {
        if (v->crashed_until > 0.0f) {
            v->pending_td_victim = -1;           // attacker crashed: denied
        } else if (g_race_time >= v->pending_td_time
                                  + B3_RACECAR_CLEAR_WAIT_S) {
            int self = v->vehicle_id, vi = v->pending_td_victim;
            // Revenge flags: FUN_00198E60's score+0x5B9 bookkeeping.
            int revenge = v->taken_down_by[vi] != 0;
            if (revenge) v->taken_down_by[vi] = 0;
            g_vehicles[vi].taken_down_by[self] = 1;
            int bp = b3_takedown_bp(&v->score, g_race_time, revenge);
            b3_boost_takedown(&v->bar);          // +360 units, bar tier up
            if (v == &g_player)
                printf("[Burnout3] %sTAKEDOWN on car %d! +%d BP "
                       "(total %d, bar x%d)\n",
                       revenge ? "REVENGE " : "", vi, bp, v->score.bp,
                       v->bar.tier + 1);
            // TAKEDOWN-FX: FUN_000273F0 -> FUN_000278B0 -> gate FUN_00027A60
            // -> entry FUN_00027920. Only the human attacker (class 0) arms
            // the cinematic, exactly as the real gate does.
            if (v == &g_player) {
                Vehicle* vic = &g_vehicles[vi];
                float vcrash = vic->crashed_until - 5.0f;   // crash start
                b3_tdfx_on_takedown(self, v->crashed_until > 0.0f,
                                    vi, vic->crashed_until > 0.0f,
                                    g_race_time, vcrash,
                                    -1, v->bar.tier >= 3);
            }
            v->pending_td_victim = -1;
        }
    }

    // CRASH-EVENT: while the wreck is down its motion comes from the crash
    // module -- the ported FUN_0011AEF0 contact response + FUN_00123000
    // damping laws around the verified FUN_00109560 integrator -- instead of
    // the drive pipeline. The car physically tumbles/slides and settles.
    {
        int slot = (int)(v - g_vehicles);
        if (v->crashed_until > 0.0f && slot >= 0 && slot < 8
            && g_wrecks[slot].active) {
            B3WreckState* wk = &g_wrecks[slot];
            // Road height under the wreck: the game's own collision world
            // via the down-probe; route line as the fallback.
            float ground, gy, gn[3];
            if (b3_collision_ready()
                && b3_ground_probe(v->pos.x, v->pos.y, v->pos.z,
                                   &gy, gn) >= 0) {
                ground = gy;
            } else {
                Vec3 c;
                loop_closest(g_cl, ROUTE_COUNT, v->pos.x, v->pos.z, &c, NULL);
                ground = c.y;
            }
            // CRASH-CINEMA: AFTERTOUCH.  The old block here drove
            // b3_wreck_aftertouch -- the corner-kick code from FUN_00117F90
            // (the RACING input stage), whose gate veh+0x4AC2 is provably
            // never written non-zero (two references in the whole image: the
            // read at 0x0011817E and `MOV byte [ESI+0x4ac2], AL` with AL==0
            // at 0x00117799).  That block is dead in retail, which is why it
            // had to be opt-in (B3_WRECK_AFTERTOUCH) and why it pumped
            // |omega| to 67 rad/s when driven.
            //
            // The LIVE aftertouch is a different function on a different
            // path: FUN_0011BE50's crashed branch calls FUN_00118410, which
            // both publishes the two aftertouch axes (veh+0x1408 / +0x140C)
            // and, at 0x001189A3..0x00118CD3, STEERS THE WRECK'S VELOCITY
            // VECTOR -- one clamped yaw step per vehicle tick toward a
            // screen-relative direction, authority 0.4/(crash_clock+1)
            // degrees, gated on the Impact Time button being held.  It sets
            // veh+0x4AC5 on every frame it actually turns the wreck, and
            // that byte is the AFTERTOUCH TAKEDOWN qualifier.
            // b3_wreck_aftertouch_steer is the port; ON by default.
            if (v == &g_player) {
                float ax = 0.0f, az = 0.0f;
                if (g_keys[SDL_SCANCODE_A] || g_keys[SDL_SCANCODE_LEFT])
                    ax = -1.0f;
                if (g_keys[SDL_SCANCODE_D] || g_keys[SDL_SCANCODE_RIGHT])
                    ax = 1.0f;
                if (g_keys[SDL_SCANCODE_W] || g_keys[SDL_SCANCODE_UP])
                    az = 1.0f;
                if (g_keys[SDL_SCANCODE_S] || g_keys[SDL_SCANCODE_DOWN])
                    az = -1.0f;
                if (g_pad) {   // left stick / dpad -- FUN_00020E70/F50
                    float sx = pad_axis(SDL_CONTROLLER_AXIS_LEFTX, 0.15f);
                    float sy = pad_axis(SDL_CONTROLLER_AXIS_LEFTY, 0.15f);
                    if (sx != 0.0f) ax = sx;
                    if (sy != 0.0f) az = -sy;      // stick up = forward
                    if (pad_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT))  ax = -1.0f;
                    if (pad_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) ax = 1.0f;
                    if (pad_btn(SDL_CONTROLLER_BUTTON_DPAD_UP))    az = 1.0f;
                    if (pad_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN))  az = -1.0f;
                }
                g_at_h = ax;
                g_at_v = az;
                /* pad+0x84: the same BOOST button the driving path reads,
                 * and the one FUN_00118410 @0x0011889A tests to request the
                 * divisor-5 slow-mo. */
                g_at_held = (g_keys[SDL_SCANCODE_SPACE]
                             || (g_pad && pad_btn(SDL_CONTROLLER_BUTTON_X)))
                            ? 1 : 0;
                /* TEST AID: B3_TEST_AFTERTOUCH="h,v" pins the two
                 * aftertouch axes and holds Impact Time, so a headless
                 * run can exercise the whole chain (same class of aid as
                 * B3_TEST_CRASH_AT). */
                {
                    static int tat = -1;
                    static float th, tv;
                    if (tat < 0) {
                        const char* e = getenv("B3_TEST_AFTERTOUCH");
                        tat = (e && sscanf(e, "%f,%f", &th, &tv) == 2)
                              ? 1 : 0;
                    }
                    if (tat) {
                        ax = th; az = tv;
                        g_at_h = ax; g_at_v = az; g_at_held = 1;
                    }
                }

                B3WreckAftertouchIn ai;
                memset(&ai, 0, sizeof(ai));
                ai.h = ax;
                ai.v = az;
                ai.engaged   = g_at_held;
                ai.crash_mode = 0;              /* race, not crash junction */
                ai.breaker_armed = 0;           /* no crashbreaker in a race */
                /* veh+0x1530, the crash clock FUN_0011BE50 @0x0011BE98 runs */
                ai.crash_clock = 5.0f - (v->crashed_until - g_race_time);
                if (ai.crash_clock < 0.0f) ai.crash_clock = 0.0f;
                ai.cam_right[0] = g_at_cam_right[0];
                ai.cam_right[1] = g_at_cam_right[1];
                ai.cam_right[2] = g_at_cam_right[2];
                ai.cam_fwd[0]   = g_at_cam_fwd[0];
                ai.cam_fwd[1]   = g_at_cam_fwd[1];
                ai.cam_fwd[2]   = g_at_cam_fwd[2];
                ai.want_bank    = 1;            /* veh+0x1540 */
                if (b3_wreck_aftertouch_steer(wk, &ai, g_tdfx_real_dt)) {
                    static int at_trace = -1;
                    if (at_trace < 0)
                        at_trace = getenv("B3_AT_TRACE") != NULL;
                    if (at_trace)
                        printf("[AFTERTOUCH] t=%.2f h=%.2f v=%.2f clock=%.2f"
                               " -> vel(%.1f,%.1f,%.1f) used=%d\n",
                               g_race_time, ai.h, ai.v, ai.crash_clock,
                               wk->vel[0], wk->vel[1], wk->vel[2],
                               wk->aftertouch_used);
                }
            }
            /* TEST AID (headless acceptance only): B3_TEST_AT_TAKEDOWN=<slot>
             * synthesises the wreck-vs-racer CONTACT that
             * b3_carcol_resolve_wreck would produce (crash_a, impact past
             * the 5000 gate of 0x0011408B) once the player's wreck has
             * latched the aftertouch qualifier veh+0x4AC5, so the whole
             * AFTERTOUCH TAKEDOWN chain can be exercised without a human on
             * the pad.  The carcol side of that contact is already proven
             * live by AI wrecks (the "WRECK TAKEDOWN: wreck N -> car M"
             * lines); what this stages is only the PLAYER's qualifier.
             * Inert unless the env is set. */
            if (v == &g_player && wk->aftertouch_used) {
                static int atd = -2;
                if (atd == -2) {
                    const char* e = getenv("B3_TEST_AT_TAKEDOWN");
                    atd = e ? atoi(e) : -1;
                }
                if (atd > 0 && atd < g_num_vehicles) {
                    Vehicle* r = &g_vehicles[atd];
                    if (r->active && r->crashed_until <= 0.0f) {
                        B3CarContact ct;
                        memset(&ct, 0, sizeof(ct));
                        ct.crash_a = 1;
                        ct.impact  = 9000.0f;
                        ct.normal[0] = 1.0f;
                        ct.point[0] = r->pos.x;
                        ct.point[1] = r->pos.y;
                        ct.point[2] = -r->pos.z;
                        r->immune_until = 0.0f;   /* the aid is the point */
                        printf("[Burnout3] t=%.2f STAGED: wreck-vs-racer "
                               "contact with car %d\n", g_race_time, atd);
                        carcol_wreck_takedown(&g_player, r, &ct);
                        atd = -1;
                    }
                }
            }
            float wpre[3] = { wk->frame[3][0], wk->frame[3][1],
                              wk->frame[3][2] };
            /* PHYS-LEDGER-3 / F3 -- PH-06.  A retail wreck's contact with
             * the world is the ORDINARY rigid-body world pass, not a
             * dedicated wall responder: the class vtable's slot +0x10,
             * FUN_0011BE40 @0x0011BE4A -> FUN_00122D00 -> **FUN_00109EA0**
             * @0x00122F81, driven by the collision manager BEFORE slot +0
             * (FUN_0011BE50 -> FUN_00123000) integrates.  b3_wreck_world_
             * contact is that resolve verbatim -- b3_rigid_body_world_
             * contact (PH-24) over b3_rigid_body_obb_plane_contact's narrow
             * phase (PH-25) -- and it accumulates into the wreck's
             * +0x110/+0x120/+0x130, which the b3_wreck_update below consumes
             * through the verified FUN_00109560 port.  The contact PLANE is
             * still the harness sphere sweep (row PH-09); everything done
             * with it is now retail's.  The push-out block further down is
             * demoted to an anti-tunnelling net: no velocity, no impulse, no
             * damage report -- those all come from here. */
            if (b3_collision_ready()) {
                B3CollisionPoly soup[B3_CHASSIS_SOUP_MAX];
                float half[3];
                float center[3] = {wk->frame[3][0], wk->frame[3][1],
                                   wk->frame[3][2]};
                float velocity[3] = {wk->vel[0], wk->vel[1], wk->vel[2]};
                for (int axis = 0; axis < 3; axis++) {
                    float lo = fabsf(wk->bbmin[axis]);
                    float hi = fabsf(wk->bbmax[axis]);
                    half[axis] = (lo > hi ? lo : hi) + 0.5f;
                }
                int nsoup = b3_collision_gather_walls(
                    center, half, velocity, 0.6f, soup,
                    B3_CHASSIS_SOUP_MAX);
                for (int poly = 0; poly < nsoup; poly++)
                    b3_wreck_world_contact(wk, soup[poly].v0,
                                           soup[poly].normal);
            }
            b3_wreck_update(wk, ground, dt);
            /* PANELS: one frame of the per-panel damage machine off the
             * wreck's own contact report -- snapshot, FUN_0012C670 add,
             * FUN_0012C860 rip test, and FUN_00123000 scan. */
            if (slot >= 0 && slot < 8)
                b3_panels_crash_frame(&g_panels[slot], wk);
            /* CRASH-SHOW H5: the crash particle layer.  Three recovered
             * drivers: FUN_00181130's impact burst (glass + concrete
             * dust) once, FUN_00186D50's 5 s slide trail along the swept
             * body segment, and FUN_00181610's 14 s smoke plume. */
            if (slot >= 0 && slot < 8) {
                float wp[3] = { wk->frame[3][0], wk->frame[3][1],
                                wk->frame[3][2] };
                float wv[3] = { wk->vel[0], wk->vel[1], wk->vel[2] };
                if (!g_pfx_burst_done[slot]) {
                    g_pfx_burst_done[slot] = 1;
                    b3_pfx_impact_burst(wp, wv, 2);
                }
                b3_pfx_crash_slide(wp, wv, wk->vel[3], wk->rest_clock, dt);
                b3_pfx_wreck_plume(wp, wv, wk->rest_clock, dt);
            }
            // GLUE containment against the REAL collision world (incl. the
            // 420 m one-sided out-of-bounds sky-walls): the retail crashed
            // path carries no chassis response, but its world's sky-walls
            // stop a flying wreck; our flat-box ground GLUE ignores them,
            // so a launched wreck could sail off the map (and recovery
            // then had nothing sane to return to). Sweep in GAME space.
            {
                // b3_sweep_sphere lives in GL space (the loader mirrors the
                // game triangles on load) -- no conversion here.
                float hp[3], hn[3];
                if (b3_sweep_sphere(wpre, wk->frame[3], 1.0f, 0.6f,
                                    hp, hn)) {
                    // hp is the CONTACT POINT on the triangle, not a
                    // corrected position: assigning it teleported the wreck
                    // onto the wall (traced: 75-100% of wreck frames moved,
                    // up to 10.6 m each, which the crash module's ground
                    // push-out then fought -- RE_NOTES 16.2). Push the
                    // sphere out along the returned normal instead, exactly
                    // as mesh_collide() does.
                    float dx = wk->frame[3][0] - hp[0];
                    float dy = wk->frame[3][1] - hp[1];
                    float dz = wk->frame[3][2] - hp[2];
                    float d  = sqrtf(dx * dx + dy * dy + dz * dz);
                    float depth = 1.0f - d;          // 1.0 = the sweep radius
                    if (depth > 0.0f) {
                        wk->frame[3][0] += hn[0] * depth;
                        wk->frame[3][1] += hn[1] * depth;
                        wk->frame[3][2] += hn[2] * depth;
                    }
                    float vn = wk->vel[0]*hn[0] + wk->vel[1]*hn[1]
                             + wk->vel[2]*hn[2];
                    /* PHYS-LEDGER-3 / F3b: the velocity kill and the damage
                     * report are FUN_00109EA0's now (see F3 above); what is
                     * left here is the anti-tunnelling POSITION net, which
                     * retail does not need because its soup is refreshed
                     * against the swept pose every frame. */
                    if (0) {
                        wk->vel[0] -= vn * hn[0];
                        wk->vel[1] -= vn * hn[1];
                        wk->vel[2] -= vn * hn[2];
                    }
                    /* PANELS: a wall hit is a COLLISION event -- the
                     * momentum the contact removes, along the wall normal
                     * (which points away from the wall and into the car,
                     * FUN_0012C670's sense). */
                    /* PHYS-LEDGER-3 / F3c: reported by the world pass. */
                    if (g_crash_trace && v == &g_player)
                        fprintf(g_crash_trace,
                                "  containment: depth %.3f push n (%.2f %.2f"
                                " %.2f) vn %.2f%s\n",
                                depth > 0.0f ? depth : 0.0f,
                                hn[0], hn[1], hn[2], vn,
                                vn < 0.0f ? " (killed)" : "");
                }
            }
            v->pos.x = wk->frame[3][0];
            v->pos.z = wk->frame[3][2];
            v->pos.y = wk->frame[3][1] + 0.5f + g_car_ymin[slot];
            v->vel = (Vec3){wk->vel[0], wk->vel[1], wk->vel[2]};
            float premc[2] = { v->pos.x, v->pos.z };
            mesh_collide(v);                 // low barrier segments too
            if (g_crash_trace && v == &g_player) {
                float dmx = v->pos.x - premc[0], dmz = v->pos.z - premc[1];
                if (dmx * dmx + dmz * dmz > 1e-6f)
                    fprintf(g_crash_trace,
                            "  mesh_collide push (%.3f %.3f)\n", dmx, dmz);
            }
            wk->frame[3][0] = v->pos.x;
            wk->frame[3][2] = v->pos.z;
            wk->vel[0] = v->vel.x; wk->vel[2] = v->vel.z;
            v->rot.y = wk->yaw;
            v->rot.x = wk->pitch;
            v->rot.z = wk->roll;
            v->sim.speed = 0.0f;             // drivetrain is parked
            v->speed = wk->vel[3];
            v->track_progress = find_track_progress(v->pos);
            v->prev_progress = v->track_progress;
            return;                          // the wreck owns the motion
        }
    }

    // ---- THE GAME'S per-frame vehicle pipeline (b3_vehicle_step_full):
    // FUN_0011ECF0 input stage (steering schedule/slew, gear engage, engine)
    // then FUN_0011BE50's main path, two substeps at dt/2 of FUN_0011D460
    // (tyres/drift/LSDM/resistance) + FUN_001239C0/FUN_00123FD0 (per-wheel
    // ground rays + suspension over b3_ground_probe) + FUN_00109560
    // (rigid-body integration). Trajectory-verified against the real code
    // (validate_port.py, full-pipeline section). The old scalar
    // speed-along-heading reconstruction (b3_vehicle_step) is retired.
    if (!v->fsim_ready) full_sim_reset(v);
    int slot = (int)(v - g_vehicles);

    // Aggressive-driving reaction GLUE that survives the switch: AI steering
    // authority drops to 0.1 inside the out-of-control window
    // (FUN_00105340 [C]); the in-pipeline steer-away replacement (ECF0's
    // 0x215 != 3 path) belongs to the crash agent.
    // AGGRESSIVE DRIVING REACTION (FUN_0011ECF0's 0x215 != 3 head, now
    // ported as b3_steer_away_envelope): the pipeline needs the global
    // clock and the owner's two out-of-control stamps. The player car is
    // class 1 (FUN_00117730), which is what b3_vehicle_full_init defaults
    // to, so only the clocks have to be plumbed.
    v->fsim.clock          = g_race_time;          // DAT_0060EA20
    // PHYS-LEDGER-3 / F2c: veh+0x1534, the driver-authority scale
    // FUN_0011AEF0's wall-crash gate multiplies (RE_TD_RULES 12).  The
    // in-substep resolve reads the same value the standalone trigger does,
    // so the two cannot disagree about whether a hit is a crash.
    //
    // CRASH-AUDIT C1 -- and the ladder's SECOND output with it.
    // FUN_00105BD0 writes veh+0x1534 AND veh+0x1353 from the same
    // FUN_00105FC0 result [C]:
    //
    //   00105f21  MOVSS [ESI+0x1534],XMM0   authority = frac*0.97 + 0.03
    //   00105f4b  MOVSS [ESI+0x1534],XMM1   authority = 0.03  (out of band)
    //   00105f91  TEST  BL,BL               BL = the ladder's crash_ok byte
    //   00105f95  OR    byte [ESI+0x1353],0x18   <-- the CRASH-ENTRY VETO
    //
    // and FUN_0011AEF0's wall-crash gate refuses on bit 3 of it:
    //
    //   0011b94d  TEST  byte [EDI+0x1353],8
    //
    // crash_ok is `frac > 0`, i.e. `d2 < 0.4 * base` -- 79.06 m from the
    // viewed car for the 15625 base [0x0039A854]; the band table is
    // DAT_0039A858[1..5] = {0, 0.1, 0.4, 0.5, 1.0} scaled by that base.
    // Retail re-arms the byte every frame in the collision manager
    // (FUN_00110AF0 @0x00110E20 / E46 / E70 / EA2, before ANY car update)
    // and only then lets FUN_00105BD0 OR into it, so the write below is
    // retail's clear-then-OR collapsed: nothing else in this harness writes
    // fsim.flags_1353.
    //
    // Dropping it was not one missing gate.  The same out-of-band condition
    // pins the authority at the 0.03 FLOOR, so an AI car that retail refuses
    // to crash was instead crashing at dv > 0.825 / headon > 0.0212 against
    // the player's 27.5 / 0.707 -- which is why the pack wall-crashed at the
    // first barrier and never completed a lap.  Bit 4 (0x10) is set with it
    // because retail's OR is 0x18; its readers (@0x0011329A, @0x001141C2 --
    // the CAR-crash entries) are not ported yet, so it is inert here.
    {
        B3TdAuthority auth_1534;
        b3_td_crash_authority_full(&g_tdr, slot, g_race_time, &auth_1534);
        v->fsim.authority_1534 = auth_1534.value;
        v->fsim.flags_1353     = auth_1534.crash_ok ? 0u : 0x18u;
    }
    v->fsim.party_mode     = g_tdr.crash_mode;
    v->fsim.ooc_slam_1598  = v->slam_time;         // racecar+0x1598
    v->fsim.ooc_wall_1690  = -1.0f;                // +0x1690 not tracked yet
    v->fsim.hit_side_153C  = v->slam_side;         // veh+0x153C
    if (v->slam_time >= 0.0f) {
        if ((v != &g_player || autodrive)
            && g_race_time <= v->slam_time + B3_TOTAL_OOC_TIME_S)
            steer_input *= B3_OOC_AI_AUTHORITY;
        if (g_race_time > v->slam_time + B3_TOTAL_OOC_TIME_S) {
            v->slam_time = -1.0f;    // window over
            v->slam_side = 0;
            v->slam_by = -1;
        }
    }

    // The physics runs in GAME space; steering is the one mirror-odd input
    // (a harness-left turn is a game-right turn under the z mirror).
    //
    // FIXED TICK: the Xbox game steps this pipeline at its hard 60 Hz frame
    // tick (DAT_0060EA1C); feeding raw variable frame dt destabilises it
    // exactly as it would the original. Accumulate wall dt and step at the
    // game tick, carrying the remainder (inputs hold across sub-ticks like
    // the game's own frame inputs).
    {
        // 1:1 frame-to-physics, the retail arrangement: one pipeline step
        // per rendered frame with the frame-locked dt (period/divisor).
        // The old fixed-tick accumulator beat against the frame rate and
        // needed render interpolation to hide it; both are gone.
        b3_vehicle_step_full(&v->fsim, throttle, brake,
                             /* HANDEDNESS: with the display mirror in the
                              * projection (see render_frame), screen-left is
                              * game-left again, so the human input no longer
                              * needs the mirror-odd sign flip. AI input was
                              * always game-space. */
                             steer_input,
                             v->bar.boosting, dt);
    }
    if (getenv("B3_DBG") && v == &g_player) {
        float gh, gn[3];
        int s = harness_ground_probe(v->fsim.rb.frame[3][0],
                                     v->fsim.rb.frame[3][1],
                                     v->fsim.rb.frame[3][2], &gh, gn);
        fprintf(stderr, "[dbg] fy %.2f gh %.2f s %d w0(c%d cur %.3f) "
                "vel.y %.2f up.y %.2f\n",
                v->fsim.rb.frame[3][1], s >= 0 ? gh : -999.f, s,
                v->fsim.wheel[0].contact, v->fsim.wheel[0].cur_len,
                v->fsim.rb.vel[1], v->fsim.rb.frame[1][1]);
    }

    B3RigidBody* rb = &v->fsim.rb;

    // AI speed governor -- the mover tail at 0x171078 [C-disasm, docs/
    // RE_AI.md section 4], applied through the velocity vector now that the
    // car HAS one. [S scaling: per-second rate.]
    if (ai_target_ms >= 0.0f && rb->vel[3] > 1e-3f
        && rb->vel[3] - ai_target_ms > B3_AI_OOR_DECREASE) {
        float s = (rb->vel[3] - B3_AI_OOR_DECREASE * dt) / rb->vel[3];
        for (int j = 0; j < 3; j++) rb->vel[j] *= s;
        rb->vel[3] *= s;
    }
    (void)ai_reverse_ms;  // real reverse: brake at standstill -> gear -1
                          // through the pipeline's own gear engage now

    // Pose out of the rigid body (game -> harness: z negated). pos.y keeps
    // the harness convention (render origin = pos.y - 0.5 - g_car_ymin =
    // the physics frame origin).
    Vec3 prev_pos = v->pos;      /* pre-writeback pose: the sweep below
                                  * walks the frame's motion from here */
    v->pos.x = rb->frame[3][0];
    v->pos.z = -rb->frame[3][2];
    v->pos.y = rb->frame[3][1] + 0.5f + g_car_ymin[slot];
    v->vel = (Vec3){rb->vel[0], rb->vel[1], -rb->vel[2]};
    // game at = (sin g, 0, cos g) mirrors to harness fwd (sin g, 0, -cos g)
    v->rot.y = atan2f(rb->frame[2][0], rb->frame[2][2]);

    /* CRASH-CONTACT SOURCE (GLUE, B3_WALL_SWEEP=0 disables).
     *
     * PHYS-LEDGER wave 3 deleted this substepped sphere push-out as an
     * invented "1..8 mlen/0.6 loop", on the understanding that the retail
     * in-substep resolve (FUN_0011AEF0 -> fsim.crash_fired) would replace
     * it.  It does replace the RESPONSE, but nothing replaced it as the
     * CONTACT SOURCE: measured on 5b33704 the player wall-crashed 3x in a
     * 60 s autodrive run (dv 32.8 > 27.50, headon 0.996 > 0.707), and after
     * the deletion BOTH arms are starved -- crash_fired stays 0 across 580
     * player wall contacts, and forcing the td_rules arm back on
     * (`cfire || fire`) still yields zero crashes, because no wh record is
     * ever produced.  The user's report was blunt: "I cant seem to crash my
     * car now", including head-on into traffic.
     *
     * So it is restored, explicitly marked GLUE, until crash_fired is proven
     * to fire on real wall geometry.  It is a contact DETECTOR only; the
     * response remains retail's. */
    {
        static int sweep = -1;
        if (sweep < 0) {
            const char* e = getenv("B3_WALL_SWEEP");
            sweep = !(e && atoi(e) == 0);
        }
        if (sweep) {
            float mx = v->pos.x - prev_pos.x, mz = v->pos.z - prev_pos.z;
            float mlen = sqrtf(mx * mx + mz * mz);
            int nsub = 1 + (int)(mlen / 0.6f);
            if (nsub > 8) nsub = 8;
            Vec3 goal = v->pos;
            for (int s2 = 1; s2 <= nsub; s2++) {
                float t = (float)s2 / (float)nsub;
                v->pos.x = prev_pos.x + (goal.x - prev_pos.x) * t;
                v->pos.z = prev_pos.z + (goal.z - prev_pos.z) * t;
                mesh_collide(v);
            }
        }
    }

    if (v->fsim.contact_state_198 == 1) {
        float vin = -(rb->vel[0] * v->fsim.contact_n_170[0]
                    + rb->vel[1] * v->fsim.contact_n_170[1]
                    + rb->vel[2] * v->fsim.contact_n_170[2]);
        v->last_hit_pos = (Vec3){v->fsim.contact_pt_160[0],
                                 v->fsim.contact_pt_160[1],
                                -v->fsim.contact_pt_160[2]};
        v->last_hit_n = (Vec3){v->fsim.contact_n_170[0],
                               v->fsim.contact_n_170[1],
                              -v->fsim.contact_n_170[2]};
        v->last_hit_vin = vin > 0.0f ? vin : 0.0f;
        v->last_hit_time = g_race_time;
    }

    {
        Vec3 c;
        loop_closest(g_cl, ROUTE_COUNT, v->pos.x, v->pos.z, &c, NULL);
        float dcx = v->pos.x - c.x, dcz = v->pos.z - c.z;
        float dc = sqrtf(dcx * dcx + dcz * dcz);
        if (dc > 60.0f && dc > 1e-6f) {
            v->pos.x = c.x + dcx / dc * 60.0f;
            v->pos.z = c.z + dcz / dc * 60.0f;
            rb->frame[3][0] = v->pos.x;
            rb->frame[3][2] = -v->pos.z;
            b3_vehicle_full_refresh_derived(&v->fsim);
        }
    }

    /* FUN_001712E0, the off-world watchdog, ported whole (RE_AI 16):
     *   dy = racecar+0x44 - bilinear(the node's four point heights)
     *                     - vehicle+0x870
     *   dy > -1.0  -> racecar+0x245A = racecar+0x18D0 (LAST VALID NODE),
     *                 racecar+0x2460 = 0
     *   dy < -5.0  -> racecar+0x2460++, and at 0x3D frames FUN_001714F0
     *   otherwise  -> racecar+0x2460 = 0
     * The old code raised its counter on the same 5 m / 61 frame shape but
     * only re-SEEDED the nav cursor at the end of it and kept a separate
     * 60 m teleport; retail's response is the full re-place. */
    {
        Vec3 road;
        /* the cursor is maintained in the AI branch; a HUMAN-driven player
         * never goes through it, so refresh it here before the watchdog
         * measures the car against its node (retail maintains the cursor for
         * every racecar, in FUN_0018D790, before FUN_001712E0 runs). */
        if (!v->nav_ready
            || nav_node_dist2(v->nav_section, v->nav_node, v->pos) > 625.0f)
            (void)nav_update_vehicle(v);
        loop_closest(g_cl, ROUTE_COUNT, v->pos.x, v->pos.z, &road, NULL);
        (void)nav_surface_height(v->nav_section, v->nav_node, v->pos,
                                 &road.y);
        int slot = (int)(v - g_vehicles);
        float body_off = (slot >= 0 && slot < 8) ? (0.5f + g_car_ymin[slot])
                                                 : 0.5f;
        float dy = v->pos.y - road.y - body_off;
        int had = v->nav_last_ready;
        {   static int nt = -1;
            if (nt < 0) nt = getenv("B3_NAV_TRACE") != NULL;
            if (nt && g_frame_count % 30 == 0)
            {   Vec3 mc = nav_state_valid(v->nav_section, v->nav_node)
                          ? nav_midpoint(v->nav_section, v->nav_node)
                          : (Vec3){0,0,0};
                printf("[navdy] car%d dy %.2f pos %.0f %.2f %.0f roady %.2f "
                       "off %.2f below %d sec %u node %u ctr %.0f %.2f %.0f\n",
                       slot, dy, v->pos.x, v->pos.y, v->pos.z, road.y,
                       body_off, v->aiw.below_frames,
                       v->nav_section, v->nav_node, mc.x, mc.y, mc.z);
            }
        }
        if (b3_ai_offworld(&v->aiw, (int)v->nav_node, dy)) {
            g_replace_why = "offworld61";
            if ((b3_nav_respawn()
                 && nav_replace_car(v,
                        had ? v->nav_last_section : v->nav_section,
                        had ? v->nav_last_node : v->nav_node, 3, -1.0f))
                || route_replace_car(v, 3, -1.0f)) {
                v->vel = (Vec3){0, 0, 0};
                v->sim.speed = 0.0f;
            }
        } else if (dy > 0.0f - B3_AI_ROAD_LATCH_M
                   && nav_state_valid(v->nav_section, v->nav_node)) {
            v->nav_last_section = v->nav_section;
            v->nav_last_node = v->nav_node;
            v->nav_last_ready = 1;
        }
        v->offroad_frames = v->aiw.below_frames;
        /* collision-world containment only: a car that has left the mesh
         * entirely is below every surface, so the 61-frame rule above never
         * gets a valid node to work from.  GLUE, kept as the backstop. */
        if (v->pos.y < road.y - 60.0f) {
            unsigned int rs = had ? v->nav_last_section : v->nav_section;
            unsigned int rn = had ? v->nav_last_node : v->nav_node;
            g_replace_why = "below60m";
            if ((b3_nav_respawn() && nav_replace_car(v, rs, rn, 3, -1.0f))
                || route_replace_car(v, 3, -1.0f)) {
                v->vel = (Vec3){0, 0, 0};
                v->sim.speed = 0.0f;
            }
            v->offroad_frames = 0;
        }
    }

    // Legacy interface sync: HUD (mph/gear), audio (rpm), AI and gameplay
    // rules keep reading v->sim/v->speed exactly as before the switch.
    v->sim.trans = v->fsim.trans;
    v->sim.rpm = v->fsim.trans.omega * 9.549296f;
    v->sim.gear = v->fsim.trans.gear + 1;
    v->sim.speed = rb->vel[3];
    v->sim.slide = v->fsim.slide_1440;
    {   // travel-vs-nose angle (the old drift_angle proxy, visual/AI only)
        float da = rb->dir[0] * rb->frame[0][0] + rb->dir[1] * rb->frame[0][1]
                 + rb->dir[2] * rb->frame[0][2];
        float dn = rb->dir[0] * rb->frame[2][0] + rb->dir[1] * rb->frame[2][1]
                 + rb->dir[2] * rb->frame[2][2];
        v->sim.drift_angle = (rb->vel[3] > 1.0f)
                           ? atan2f(da, dn) * RAD_TO_DEG : 0.0f;
    }
    v->speed = v->sim.speed;

    // Update track progress
    v->track_progress = find_track_progress(v->pos);

    // Lap detection (per-vehicle wrap of the progress fraction)
    if (v->prev_progress > 0.9f && v->track_progress < 0.1f && v->speed > 5.0f) {
        v->lap++;
        if (v == &g_player) {
            g_current_lap = v->lap;
            printf("[Burnout3] Lap %d/%d completed!\n", g_current_lap, g_lap_count);
        }
    }
    v->prev_progress = v->track_progress;

    // The old health-driven crash/takedown placeholder is gone: crashes are
    // now slam-driven (collision pass below) and takedowns follow the
    // recovered attribution windows. v->health remains only as an inert
    // legacy field.
}

// Place a car in a 2-wide grid behind the recovered center line's first point,
// headed along the line. Grid layout is harness convention; the start position
// and direction are the game's own path data.
static void spawn_on_grid(Vehicle* v, int slot) {
    // Real start grid: position + forward vector per slot straight from the
    // game's OFFSGRCF spatial record (burnout3_start_grid.h, [C]). The real
    // event grid is 6 cars.
    const B3GridSlot* g = &B3_START_GRID[slot % B3_START_GRID_COUNT];
    v->pos = (Vec3){g->pos[0], g->pos[1] + 0.5f, g->pos[2]};
    // Harness overflow beyond 6 slots: offset an extra row back.
    if (slot >= B3_START_GRID_COUNT) {
        v->pos.x -= g->fwd[0] * 40.0f;
        v->pos.z -= g->fwd[2] * 40.0f;
    }
    v->vel = (Vec3){0, 0, 0};
    v->rot.y = atan2f(g->fwd[0], -g->fwd[2]);
    // Seed lap-wrap detection from the slot's REAL progress. Grid slots
    // that sit just BEHIND the start line's progress wrap (~0.99) cross
    // it at launch; with prev_progress hard-set to 0 and the rolling
    // start instantly satisfying the speed guard, those cars banked a
    // phantom lap at t~0 (dumps 015/016: cars 1/2 at "lap 1" 22 s in) --
    // permanently ranking ahead of the player, so the place indicator
    // could never show better than 3rd. Slots behind the wrap start at
    // lap -1 so the launch crossing lands everyone on lap 0 together.
    v->track_progress = find_track_progress(v->pos);
    v->prev_progress = v->track_progress;
    v->lap = (v->track_progress > 0.5f) ? -1 : 0;
}

// Pick the Nth player-class vehicle out of the extracted roster, skipping
// traffic entries. Returns NULL once the roster is exhausted.
// Debug: B3_PLAYER_CAR="CLASS_CarN" (e.g. SUPR_Car1) overrides the PLAYER
// slot's roster entry -- screenshot verification of any car's mesh.
static const VehicleInfo* roster_player_car(int nth) {
    const char* want = getenv("B3_PLAYER_CAR");
    if (nth == 0 && want && *want) {
        for (int i = 0; i < VEHICLE_COUNT; i++) {
            char full[64];
            snprintf(full, sizeof(full), "%s_%s",
                     VEHICLES[i].class_code, VEHICLES[i].file);
            char* dot = strrchr(full, '.');
            if (dot) *dot = '\0';
            if (strcmp(full, want) == 0) return &VEHICLES[i];
        }
        fprintf(stderr, "[Burnout3] B3_PLAYER_CAR=%s not in roster\n", want);
    }
    for (int i = 0, seen = 0; i < VEHICLE_COUNT; i++) {
        if (VEHICLES[i].kind != VEH_PLAYER) continue;
        if (seen++ == nth) return &VEHICLES[i];
    }
    return NULL;
}

static void load_real_track(void) {
    const char* path = getenv("B3_TRACK_OBJ");
    if (!path) path = "build/track.obj";
    if (trackmesh_load(&g_real_track, path) == 0) {
        g_have_real_track = 1;
        printf("[Burnout3] REAL track geometry: %d verts, %d tris from %s\n",
               g_real_track.vertex_count, g_real_track.triangle_count, path);
        printf("           bounds X[%.0f %.0f] Y[%.0f %.0f] Z[%.0f %.0f]\n",
               g_real_track.min[0], g_real_track.max[0],
               g_real_track.min[1], g_real_track.max[1],
               g_real_track.min[2], g_real_track.max[2]);
    } else {
        printf("[Burnout3] no real track at %s (run tools/extract_track.py); "
               "falling back to the placeholder circuit\n", path);
    }
}

// ---- retail output gamma ramp -----------------------------------------------
// FUN_0003C8A0 builds a 256-entry ramp inline at 0x0003D3F0..0x0003D484 and
// installs it with D3DDevice_SetGammaRamp:
//     ramp[i] = round((i/255)^0.95 * 255)
// identical on R, G and B (the three byte stores at 0x0003D410/0x0003D416/
// 0x0003D41C target 0x0045D1A8/0x0045D2A8/0x0045D3A8, which nothing else in
// the executable writes). It is installed once, at renderer construction --
// not animated, not per-scene. The exponent is below 1, so it LIFTS midtones
// and shadows: 20->22.6, 40->44.3, 80->86.4, 128->133.2, 255->255.
//
// On real hardware this is a SCANOUT transform: it changes what the display
// (and an emulator's screenshot) shows, but not what the frame buffer holds.
//
// The harness applies it in the GL PRESENT PATH instead -- b3_postfx_gamma()
// in src/burnout3_postfx.c, a 256x1 LUT texture and one full-screen quad,
// run as the last thing in render_frame(). That is the only way the live
// window and the captures can be guaranteed to agree: SDL_SetWindowGammaRamp
// fails or no-ops on every Wayland compositor and on most X11 setups, so the
// live window used to render with NO ramp while every screenshot had it
// applied in software here. The software table below survives only as the
// fallback for a GL with no GLSL, and g_gamma_in_gl keeps the two mutually
// exclusive so nothing is ever double-applied.
// B3_NOGAMMA=1 disables both halves.
static unsigned char b3_gamma_ramp[256];
static int b3_gamma_ready = 0;

static void b3_gamma_build(void) {
    if (b3_gamma_ready) return;
    b3_gamma_ready = 1;
    /* B3_BRIGHTNESS=<x>: user display-preference multiplier folded into
     * the output LUT (default 1.0 = the recovered ramp alone). TUNED
     * knob, user-authorized deviation 2026-08-13 ("my live screen is
     * dark") -- clips at the top for x > 1, like any TV brightness. */
    double bright = 1.0;
    {
        const char* e = getenv("B3_BRIGHTNESS");
        if (e && *e) {
            bright = atof(e);
            if (bright < 0.25) bright = 0.25;
            if (bright > 4.0)  bright = 4.0;
        }
    }
    for (int i = 0; i < 256; i++) {
        double v = pow((double)i / 255.0, 0.949999988079071) * 255.0 * bright;
        int r = (int)(v + 0.5);
        b3_gamma_ramp[i] = (unsigned char)(r < 0 ? 0 : (r > 255 ? 255 : r));
    }
}

// Set every frame from b3_postfx_gamma()'s return value: 1 when the ramp was
// applied in GL (so the back buffer is already post-ramp and the readback
// must NOT be touched), 0 when it could not be.
static int b3_gamma_in_gl = 0;

// The fallback: apply the same table to a captured RGBA buffer. Only reached
// when the GL pass is unavailable.
static void b3_gamma_apply_rgba(unsigned char* px, int n) {
    if (b3_gamma_in_gl || getenv("B3_NOGAMMA")) return;
    b3_gamma_build();
    for (int i = 0; i < n; i++) {
        px[i * 4 + 0] = b3_gamma_ramp[px[i * 4 + 0]];
        px[i * 4 + 1] = b3_gamma_ramp[px[i * 4 + 1]];
        px[i * 4 + 2] = b3_gamma_ramp[px[i * 4 + 2]];
    }
}

// Upload one PNG (a decoded game texture) as a GL texture. Returns 0 on failure.
// *cutout is set when >35% of texels are fully transparent (fences, foliage).
static GLuint load_gl_texture(const char* path, int* cutout) {
    SDL_Surface* img = IMG_Load(path);
    if (!img) return 0;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(img);
    if (!rgba) return 0;

    if (cutout) {
        long clear = 0, total = (long)rgba->w * rgba->h;
        const unsigned char* px = rgba->pixels;
        for (long i = 0; i < total; i++)
            if (px[i * 4 + 3] == 0) clear++;
        *cutout = total > 0 && clear * 100 > total * 35;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    /* GLUE (display quality): anisotropic filtering. The BILLBOARD wave
     * proved the perceived "flicker" on roadside boards is pure texture
     * aliasing at grazing view angles (6.9% strongly-reversing pixels with
     * the board rendered ALONE; zero draw-order/z contention). Trilinear
     * alone shimmers there; 8x aniso is the standard remedy. Not a retail
     * mechanism (the NV2A had its own per-stage filtering) -- marked GLUE. */
    {
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
        static GLfloat maxaniso = -1.0f;
        if (maxaniso < 0.0f) {
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxaniso);
            if (glGetError() != GL_NO_ERROR) maxaniso = 0.0f;
            if (maxaniso > 8.0f) maxaniso = 8.0f;
        }
        if (maxaniso >= 2.0f)
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                            maxaniso);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
    SDL_FreeSurface(rgba);
    return tex;
}

// SIGNS: trackmesh's frame-texture resolver calls back here, because the image
// decode (SDL_image + the cut-out probe) lives in this file and trackmesh.c is
// GL-only. A frame that will not load returns 0 and the group keeps its base
// texture, so a track whose extra frames were never extracted stands still
// rather than drawing untextured.
static unsigned b3_track_frame_texture(const char* path, void* user) {
    (void)user;
    return (unsigned)load_gl_texture(path, NULL);
}

// Bind every material group of the real track to its decoded game texture.
// Must run after the GL context exists. Duplicate paths share one GL texture.
static void load_track_textures(void) {
    if (!g_have_real_track) return;
    int loaded = 0, missing = 0;
    for (int g = 0; g < g_real_track.group_count; g++) {
        const char* path = g_real_track.groups[g].texture;
        if (!path[0]) { missing++; continue; }
        GLuint tex = 0;
        int cutout = 0, found = 0;
        for (int h = 0; h < g; h++) {
            if (strcmp(g_real_track.groups[h].texture, path) == 0) {
                tex = g_track_tex[h];
                cutout = g_track_cutout[h];
                found = 1;
                break;
            }
        }
        if (!found) {
            tex = load_gl_texture(path, &cutout);
            if (tex) loaded++;
        }
        if (!tex) missing++;
        g_track_tex[g] = tex;
        // TRACK-BLEND: whether a world texture's alpha channel means
        // transparency AT ALL is decided by the material flag word, not by how
        // many texels happen to be clear. The streamed material apply
        // FUN_000393C0 sets D3DRS_ALPHATESTENABLE from bit 0x001 (@0x00039B21)
        // and D3DRS_ALPHABLENDENABLE from bit 0x010 (@0x00039BD4); a material
        // with NEITHER draws fully opaque and its alpha is a shader mask.
        // That is the whole class-1 road / shopfront / window family, 40-77%
        // of whose texels are alpha==0 -- the old texel heuristic called them
        // cut-outs and alpha-tested away 95% of Silver Lake's dirt road, so
        // the sky dome showed through it as a slate-blue "river".
        // The heuristic still decides HOW to draw the genuinely transparent
        // families, because this harness bakes one display list and cannot
        // reproduce the game's far-to-near transparent pass (FUN_001ADD60).
        // STATIC-WORLD-2: the alpha state is now decided entirely by
        // trackmesh_group_state() from the material flag word, using the
        // explicit have_material presence flag. `cutout` survives only as the
        // fallback for a group the MTL carried no record for.
        g_track_cutout[g] = (unsigned char)cutout;
        trackmesh_set_group_texture(&g_real_track, g, tex);
    }
    // SIGNS: a frame-cycling material carries one texture PER FRAME (material
    // +0x0C is a pointer ARRAY the ticker indexes by frame index), so the base
    // pass above resolved only frame 0. Load the rest through the same loader.
    int animframes = trackmesh_load_frame_textures(&g_real_track,
                                                   b3_track_frame_texture, NULL);
    printf("[Burnout3] REAL textures: %d loaded, %d groups unresolved (of %d), "
           "%d animation frames\n",
           loaded, missing, g_real_track.group_count, animframes);

    // Bake the whole textured track into a display list.
    g_track_list = glGenLists(1);
    glNewList(g_track_list, GL_COMPILE);
    int ngroups = g_real_track.group_count > 0 ? g_real_track.group_count : 1;
    // STATIC-WORLD-2: the world's alpha test is GREATER 64/255, not 0.5 --
    // D3DRS_ALPHAFUNC/ALPHAREF are set once by the world setup FUN_00038D10
    // (0x0003901B / 0x00038FEE). trackmesh_group_state() sets it per group.
    glAlphaFunc(GL_GREATER, TRACKMESH_ALPHA_REF);
    for (int g = 0; g < ngroups; g++) {
        int first = 0, count = g_real_track.triangle_count;
        GLuint tex = 0;
        if (g_real_track.group_count > 0) {
            first = g_real_track.groups[g].first_triangle;
            count = g_real_track.groups[g].triangle_count;
            tex = g_track_tex[g];
        }
        if (count <= 0) continue;
        // STATIC-WORLD-2: a material whose UV scroll the animated-material
        // ticker FUN_0019B1E0 advances (US_C3_V1: `Arrows`, the wrong-way
        // chevron boards) cannot be baked -- its texture coordinates change
        // every frame. trackmesh_draw_scroll() draws those below.
        // SIGNS: the animated-material ticker FUN_0019B1E0 has TWO arms.
        // `uv_scroll_rate > 0` is only the scroll one (US_C3_V1: `Arrows`).
        // Its other arm FRAME-CYCLES the bound texture -- the SLOW/DOWN
        // accident boards, the warning signs, the flags, the water -- and
        // those cannot be baked either, because the display list would freeze
        // the texture BIND. trackmesh_group_animated() covers both.
        if (g_real_track.group_count > 0
            && trackmesh_group_animated(&g_real_track, g))
            continue;
        // Decal layer: material flag bit 0x400 selects D3DRS_ZWRITEENABLE=0
        // (FUN_000393C0 @0x00039AF5..0x00039B1B [C]); the loader already
        // hoists these groups last, matching the game's material-major
        // decals-last order. The paint lives in the texture's ALPHA channel,
        // so the layer draws source-alpha BLENDED (the ambient blend preset,
        // SRC_ALPHA/ONE_MINUS_SRC_ALPHA [S]) -- alpha-TESTING it fills the
        // shapes solid white. All three compile into the display list.
        // STATIC-WORLD-2: depth mask, blend, alpha test and the texture
        // bind are the game's own per-material render state -- see
        // trackmesh_group_state() for the state-by-state citations. The two
        // alpha bits used to be swapped here, which drew the tree-shadow
        // sheets alpha-tested and opaque instead of blended at their
        // material's 0.6 alpha scalar (the black roads in build/dump013.png).
        trackmesh_group_state(&g_real_track, g, tex, g_track_cutout[g]);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_TRIANGLES);
        for (int t = first; t < first + count; t++) {
            const unsigned* idx = g_real_track.indices + (size_t)t * 3;
            for (int k = 0; k < 3; k++) {
                const float* p = g_real_track.positions + (size_t)idx[k] * 3;
                if (tex) {
                    const float* uv = g_real_track.uvs + (size_t)idx[k] * 2;
                    // TRACK-BLEND: rgb = 2 * tex * vertexColour is what every
                    // world pixel shader computes (PS_COMBINEROUTPUT_
                    // SHIFTLEFT_1 on the stage-0 RGB output word, 0x000100C0
                    // in all six D3DPIXELSHADERDEFs). trackmesh_load already
                    // applied the doubling, so this is a plain MODULATE.
                    // Without it the world loses every bit of its baked
                    // lighting and reads uniformly flat and bright.
                    // Classes 8 and 9 (foliage, props, cones) have no
                    // D3DCOLOR register in their vertex declaration, so the
                    // game never reads the stored colour for them -- draw
                    // those at full white.
                    // STATIC-WORLD-2: w carries the material's alpha scalar
                    // (+0x20). Under GL_MODULATE the fragment alpha is
                    // texture.a * primary.a, which is the class-6 output alpha
                    // tex.a * C0.a exactly.
                    {
                        float vc[4];
                        trackmesh_group_vertex_color(&g_real_track, g,
                                                     idx[k], vc);
                        glColor4fv(vc);
                    }
                    glTexCoord2f(uv[0], uv[1]);
                } else {
                    // Flat shade from the face normal so untextured groups
                    // stay legible.
                    const float* p0 = g_real_track.positions + (size_t)idx[0] * 3;
                    const float* p1 = g_real_track.positions + (size_t)idx[1] * 3;
                    const float* p2 = g_real_track.positions + (size_t)idx[2] * 3;
                    float ux = p1[0]-p0[0], uy = p1[1]-p0[1], uz = p1[2]-p0[2];
                    float vx = p2[0]-p0[0], vy = p2[1]-p0[1], vz = p2[2]-p0[2];
                    float nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
                    float len = sqrtf(nx*nx + ny*ny + nz*nz);
                    float shade = len > 1e-6f ? (0.35f + 0.65f * fabsf(ny / len)) : 0.5f;
                    glColor3f(shade * 0.75f, shade * 0.78f, shade * 0.82f);
                }
                glVertex3f(p[0], p[1], p[2]);
            }
        }
        glEnd();
    }
    glDepthMask(GL_TRUE);       // restore after the decal tail
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glEndList();
}

// UNDERBODY: the .bgv record texture slot (rec+0x1A) that means "the shared
// VehicleUnderside chassis page", not this car's paint.  rec+0x1A indexes a
// five-entry texture array on the draw context at ctx+0x334 (FUN_00031AB0
// opens with MOV EAX,[ESI + 0x334 + slot*4] at 0x00031AC5).  FUN_000303D0
// fills entry 0 from the model's own +0x60 "compact1" paint record
// (0x00030546); FUN_000315C0 fills entries 1..4 from four BSS globals
// (0x000317C0..0x000317E8), and the car-system init at 0x0002F260 writes
// those four from name lookups in the global texture bank:
//     "UnbrokenGlass" -> 0x004D61A8 -> slot 2
//     "CrackedGlass"  -> 0x004D61AC -> slot 3
//     "SmashedGlass"  -> 0x004D61B0 -> slot 4
//     "VehicleUnderside" -> 0x004D61B4 -> SLOT 1
// Slots 2/3/4 land in exactly the intact/cracked/shattered order
// FUN_000300A0 stamps into rec+0x1A, which is the anchor that pins the whole
// table.  See tools/extract_bgv.py's `record` section for the full chain.
#define B3_BGV_TEX_UNDERSIDE 1

// The page itself lives in Data/Global.txd, decoded to PNG by
// tools/extract_txd.py -- ONE 512x256 chassis raster (exhausts, transmission
// tunnel, diff, subframes) shared by every car, loaded on first use.  A build
// without it falls back to the car's paint page, i.e. the old behaviour.
static GLuint car_underside_texture(void) {
    static int tried;
    static GLuint tex;
    if (!tried) {
        tried = 1;
        tex = load_gl_texture("build/frontend/VehicleUnderside.png", NULL);
        if (!tex)
            printf("[Burnout3] build/frontend/VehicleUnderside.png missing "
                   "(run tools/extract_txd.py) -- car undersides fall back "
                   "to the paint page\n");
    }
    return tex;
}

// Build a shaded display list for one extracted car OBJ. The geometry is the
// game's own; the flat shading here is harness rendering (textures pending --
// the .bgv "compact1" paint raster is decoded for the whole-car livery only).
// use_color=0 leaves glColor to the caller (used by the glass list so the
// verified FUN_000300A0 tints can be applied at draw time).
static GLuint car_list_from_obj(const char* path, GLuint tex, int use_color,
                                float* ymin_out) {
    TrackMesh m;
    if (trackmesh_load(&m, path) != 0) return 0;
    if (ymin_out) *ymin_out = m.min[1];

    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    // UNDERBODY: one bind per `usemtl b3tex<slot>` span, not one per mesh.
    // The extractor tags every record span with its .bgv texture slot, and
    // slot B3_BGV_TEX_UNDERSIDE is the shared VehicleUnderside chassis page
    // (see the block above this function).  Binding the paint page for it
    // made the car's belly sample the livery -- mirrored sponsor decals and
    // tail-light art smeared over the underside, which is what showed the
    // moment a car flipped.  Slots 2..4 (the glass tiers) deliberately keep
    // the paint bind: the harness draws glass from the separate _glass.obj
    // list through the recovered carfx glass shader, which never samples
    // those pages.  An OBJ with no `usemtl` (traffic bodies, or a build/cars
    // predating the extractor change) has group_count 0 and takes exactly
    // the old single-bind path.
    int ngroups = m.group_count > 0 ? m.group_count : 1;
    for (int g = 0; g < ngroups; g++) {
        int first = 0, count = m.triangle_count;
        GLuint gtex = tex;
        if (m.group_count > 0) {
            first = m.groups[g].first_triangle;
            count = m.groups[g].triangle_count;
            if (count <= 0) continue;
            int slot = -1;
            if (tex && sscanf(m.groups[g].material, "b3tex%d", &slot) == 1
                    && slot == B3_BGV_TEX_UNDERSIDE) {
                GLuint u = car_underside_texture();
                if (u) gtex = u;
            }
        }
        if (gtex) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, gtex);
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        glBegin(GL_TRIANGLES);
        for (int t = first; t < first + count; t++) {
            const unsigned* idx = m.indices + (size_t)t * 3;
            const float* p0 = m.positions + (size_t)idx[0] * 3;
            const float* p1 = m.positions + (size_t)idx[1] * 3;
            const float* p2 = m.positions + (size_t)idx[2] * 3;
            float ux = p1[0]-p0[0], uy = p1[1]-p0[1], uz = p1[2]-p0[2];
            float vx = p2[0]-p0[0], vy = p2[1]-p0[1], vz = p2[2]-p0[2];
            float nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
            float len = sqrtf(nx*nx + ny*ny + nz*nz);
            float shade = 0.75f;
            if (len > 1e-9f) {
                // Fixed light, double-sided: the Z-mirror (RE_NOTES 12)
                // flipped triangle winding, so one-sided diffuse left most
                // panels at the floor term -- cars rendered near-black.
                shade = 0.60f + 0.40f * fabsf(
                        (nx*0.30f + ny*0.85f + nz*0.42f) / len);
            }
            if (use_color) glColor3f(shade, shade, shade);
            for (int k = 0; k < 3; k++) {
                unsigned vi = idx[k];
                if (gtex && m.uvs)
                    glTexCoord2f(m.uvs[(size_t)vi * 2],
                                 m.uvs[(size_t)vi * 2 + 1]);
                // CARFX: the .bgv's real per-vertex normals drive the
                // specular reflect() -- without them the streak degrades to
                // flat facets (docs/RE_CARFX.md, has_normals load-bearing).
                if (m.normals)
                    glNormal3fv(m.normals + (size_t)vi * 3);
                glVertex3fv(m.positions + (size_t)vi * 3);
            }
        }
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);
    glEndList();
    trackmesh_free(&m);
    return list;
}

// Parse the extractor's .wheels sidecar: wheel radius (.bgv+0x18) and the
// attach matrices' position rows + mirror flags (.bgv+0xB80, [C] -- see
// tools/extract_bgv.py header). The loader Z-flip (RE_NOTES 12) applied to
// every mesh must be applied to these positions too so they stay in the same
// (GL) space as the flipped car body.
static int load_car_wheels(int slot, const char* cls, const char* base) {
    char path[160];
    snprintf(path, sizeof(path), "build/cars/%s_%s.wheels", cls, base);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[160];
    int n = 0;
    float zsum = 0.0f;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        float x, y, z; int mir;
        if (sscanf(line, "radius %f", &x) == 1) {
            g_car_wheel_radius[slot] = x;
        } else if (sscanf(line, "wheel %f %f %f %d", &x, &y, &z, &mir) == 4
                   && n < 6) {
            g_car_wheel_pos[slot][n][0] = x;
            g_car_wheel_pos[slot][n][1] = y;
            g_car_wheel_pos[slot][n][2] = -z;    // loader Z-flip
            g_car_wheel_mirror[slot][n] = mir;
            zsum += z;
            n++;
        } else if (sscanf(line, "ext %f %f %f %f", &g_car_ext[slot][0],
                          &g_car_ext[slot][1], &g_car_ext[slot][2],
                          &g_car_ext[slot][3]) == 4) {
            // .bgv+0xE80: physics half extents (live vehicle +0x1D0 [C])
        } else if (sscanf(line, "center %f %f %f %f", &g_car_cen[slot][0],
                          &g_car_cen[slot][1], &g_car_cen[slot][2],
                          &g_car_cen[slot][3]) == 4) {
            // .bgv+0xE90: center offset (live vehicle +0x1E0 [C])
        }
    }
    fclose(f);
    g_car_wheel_count[slot] = n;
    // GLUE: the axle nearer the nose steers. In .bgv space the nose is +Z,
    // so pre-flip z above the average marks the front pair.
    for (int w = 0; w < n; w++)
        g_car_wheel_front[slot][w] =
            (-g_car_wheel_pos[slot][w][2]) > (zsum / (n > 0 ? n : 1));
    return n;
}

static void load_car_meshes(void) {
    for (int i = 0; i < g_num_vehicles; i++) {
        const VehicleInfo* info = g_vehicles[i].info;
        if (!info) continue;
        char base[32], path[160];
        snprintf(base, sizeof(base), "%s", info->file);
        char* dot = strrchr(base, '.');
        if (dot) *dot = '\0';

        // Paint livery: real game texture, color variant picked by grid slot
        // (the real color choice lives in the menus; slot-based pick is
        // harness convention).
        GLuint tex = 0;
        for (int k = i % 8; k >= 0 && !tex; k--) {
            snprintf(path, sizeof(path), "build/cars/%s_%s_p%d.png",
                     info->class_code, base, k);
            tex = load_gl_texture(path, NULL);
        }

        snprintf(path, sizeof(path), "build/cars/%s_%s.obj",
                 info->class_code, base);
        g_car_lists[i] = car_list_from_obj(path, tex, 1, &g_car_ymin[i]);
        if (!g_car_lists[i]) continue;

        // Damage-state variants (mask bit0 / bit1 / bit8 record splits --
        // extract_bgv.py; see the globals' provenance comment).
        snprintf(path, sizeof(path), "build/cars/%s_%s_intact.obj",
                 info->class_code, base);
        g_car_intact_lists[i] = car_list_from_obj(path, tex, 1, NULL);
        snprintf(path, sizeof(path), "build/cars/%s_%s_shell.obj",
                 info->class_code, base);
        g_car_shell_lists[i] = car_list_from_obj(path, tex, 1, NULL);
        snprintf(path, sizeof(path), "build/cars/%s_%s_glass.obj",
                 info->class_code, base);
        g_car_glass_lists[i] = car_list_from_obj(path, 0, 0, NULL);
        snprintf(path, sizeof(path), "build/cars/%s_%s_wheel.obj",
                 info->class_code, base);
        g_car_wheel_lists[i] = car_list_from_obj(path, tex, 1, NULL);
        snprintf(path, sizeof(path),
                 "build/cars/parts/%s_%s/wheel_slot8.obj",
                 info->class_code, base);
        g_car_wheel_blur8[i] = car_list_from_obj(path, tex, 1, NULL);
        snprintf(path, sizeof(path),
                 "build/cars/parts/%s_%s/wheel_slot9.obj",
                 info->class_code, base);
        g_car_wheel_blur9[i] = car_list_from_obj(path, tex, 1, NULL);

        /* PANELS: the per-panel meshes + the .bgv+0xD00 placement sidecar.
         * Loaded for the 8 roster slots only (~6 small lists per car). */
        {
            char ppath[224];
            snprintf(ppath, sizeof(ppath), "build/cars/%s_%s.panels",
                     info->class_code, base);
            FILE* pf = fopen(ppath, "r");
            int pn = 0;
            /* PHYS-LEDGER-4 / PH-05: the per-panel PIVOT-LOCAL AABB
             * and hinge-axis byte the flying-part activation ctor
             * FUN_001069C0 seeds a detached panel from (.bgv+0xEA0 +
             * k*0x20 max/min, .bgv+0xADC+k axis).  Stashed here and
             * applied after b3_panels_reset(), which memsets. */
            float pbmax[B3_PANEL_MAX][3], pbmin[B3_PANEL_MAX][3];
            int pbaxis[B3_PANEL_MAX], pbok[B3_PANEL_MAX];
            memset(pbok, 0, sizeof(pbok));
            memset(pbaxis, 0, sizeof(pbaxis));
            memset(pbmax, 0, sizeof(pbmax));
            memset(pbmin, 0, sizeof(pbmin));
            if (pf) {
                char line[512];
                while (fgets(line, sizeof(line), pf)) {
                    int k, kind;
                    float m[16];
                    if (line[0] == '#') continue;
                    /* PHYS-LEDGER-4 / PH-05: panelbb <k> <axis>
                     * <max.xyzw> <min.xyzw>, tools/extract_bgv.py. */
                    {
                        int bk, bax;
                        float bb[8];
                        if (sscanf(line,
                                "panelbb %d %d %f %f %f %f %f %f %f"
                                " %f",
                                &bk, &bax, &bb[0], &bb[1], &bb[2],
                                &bb[3], &bb[4], &bb[5], &bb[6],
                                &bb[7]) == 10) {
                            if (bk >= 0 && bk < B3_PANEL_MAX) {
                                for (int q = 0; q < 3; q++) {
                                    pbmax[bk][q] = bb[q];
                                    pbmin[bk][q] = bb[4 + q];
                                }
                                pbaxis[bk] = bax;
                                pbok[bk] = 1;
                            }
                            continue;
                        }
                    }
                    if (sscanf(line,
                            "panel %d %d %f %f %f %f %f %f %f %f %f %f %f %f"
                            " %f %f %f %f",
                            &k, &kind, &m[0], &m[1], &m[2], &m[3], &m[4],
                            &m[5], &m[6], &m[7], &m[8], &m[9], &m[10], &m[11],
                            &m[12], &m[13], &m[14], &m[15]) != 18)
                        continue;
                    if (k < 0 || k >= B3_PANEL_MAX) continue;
                    g_car_panel_kind[i][k] = kind;
                    /* row 3 = the pivot, GAME space (+Z nose).  The mesh
                     * itself is Z-flipped by the loader like every other car
                     * mesh, so the DRAW flips this z and the wreck-space
                     * maths (which works in the car's own rows) does not. */
                    g_car_panel_pos[i][k][0] = m[12];
                    g_car_panel_pos[i][k][1] = m[13];
                    g_car_panel_pos[i][k][2] = m[14];
                    snprintf(ppath, sizeof(ppath),
                             "build/cars/parts/%s_%s/panel%d_kind%d.obj",
                             info->class_code, base, k, kind);
                    g_car_panel_lists[i][k] =
                        car_list_from_obj(ppath, tex, 1, NULL);
                    if (k + 1 > pn) pn = k + 1;
                }
                fclose(pf);
            }
            g_car_panel_count[i] = pn;
            b3_panels_reset(&g_panels[i], pn, g_car_panel_kind[i],
                            g_car_panel_pos[i], 0x1234567u + (unsigned)i);
            /* PHYS-LEDGER-4 / PH-05: after the reset, which memsets. */
            for (int k = 0; k < pn && k < B3_PANEL_MAX; k++)
                if (pbok[k])
                    b3_panels_set_box(&g_panels[i], k, pbmax[k],
                                      pbmin[k], pbaxis[k]);
        }

        snprintf(path, sizeof(path), "build/cars/%s_%s.hull",
                 info->class_code, base);
        g_car_hull_ok[i] = b3_carcol_hull_load(path, &g_car_hull[i]);
        b3_carfx_load_car(i, info->class_code, base);      /* CARFX lights */
        b3_boostfx_load_car(i, info->class_code, base);    /* BOOSTFX type 8 */

        if (load_car_wheels(i, info->class_code, base) > 0
                && g_car_wheel_radius[i] > 0.0f) {
            // Ground the car through its wheels: the .bgv wheel matrices put
            // the hub at y = 0 in car space, so the contact line is at
            // -radius. (The whole-car OBJ's min-y includes pivot-local panel
            // vertices, which sat the body too low.)
            g_car_ymin[i] = -g_car_wheel_radius[i];
        }
    }
    printf("[Burnout3] REAL car meshes: ");
    for (int i = 0; i < g_num_vehicles; i++) {
        char c = g_car_lists[i] ? 'M' : '-';
        if (g_car_lists[i] && g_car_shell_lists[i] && g_car_wheel_lists[i]
                && g_car_wheel_count[i] > 0)
            c = 'D';   // full damage set: intact/shell/glass/wheels
        printf("%c", c);
    }
    printf(" (D = mesh+damage states+wheels, M = mesh only, - = box)\n");
}

// ============================================================
// Traffic -- the game's own C1_V1 race-mode traffic data
// (burnout3_traffic_data.h, tools/extract_traffic.py; docs/RE_BGD.md 4-6):
//   * car set: mode block 0's 0x18-stride id records -- the same record
//     layout the traffic manager FUN_001A13F0 walks before FUN_001A4260
//     appends ".btv" and loads each car through the .bgv relinker [C chain,
//     S data binding] -- plus the TSPC special at block+0x18;
//   * spawn/entry points: the 71 x 0x20 {pos,dir} table @0x1C450 [S];
//   * route: the 926-pt reverse-direction loop @0x1A8540 -- the oncoming
//     lane in Forward events [S]. Ascending index drives against race
//     direction (opposite winding to the race line).
// Traffic vehicles use the REDUCED 9-param config (registrar FUN_00134AC0,
// mass + suspension; per-car values from Data/vdb.xml in
// burnout3_car_physics.h). The retail road-agent pass is now located:
// FUN_0019F560 sets speed, FUN_0019F1C0 advances its path cursor, and
// FUN_0019FFA0 generates the target transform from four clamped path rows,
// each holding two point IDs. The retail descriptor is now known as pair rows,
// cumulative distances, a shared point base and count. The runtime loads that
// RIDX source and follows its persistent distance cursor through the recovered
// four-knot sampler; replacement policy, avoidance magnitude and neighbourhood
// pool remain harness-side approximations.
// ============================================================

#define B3_TRAFFIC_N B3_TRAFFIC_POOL_MAX
#define B3_TRAFFIC_SPEED_MS 22.352f   // DAT_005A9770 traffic speed cap: .data
                                      // 0x003B2110 = exactly 50 mph [C]
                                      // (init snippet 0x002C5E80)

typedef struct {
    Vec3  pos;
    B3RigidBody rb;
    B3RigidBody trailer_rb;
    float yaw;
    float speed;          // m/s along its lane
    float mass_kg;        // reduced config +0xB8 (Data/vdb.xml, 9-param set)
    float trailer_mass_kg;
    int   car;            // index into B3_TRAFFIC_CARS
    int   spawn;          // spawn-table slot last used (diagnostics)
    float crashed_until;  // wrecked: parked until this race clock (+5 s,
                          // same recovery constant as FUN_00198E60's write)
    int   active;
    int   pool_request;
    int   pool_owner;
    int   pool_agent;
    /* TRAFFIC-MIX.  A pool request is not one car: FUN_001A6070 walks the
     * request's row range one manager-record section at a time and emits
     * n = span/(mph*0.44704)/60 * sum(rate[0..5]) of them, so the occupancy
     * identity of a spawned car is the (request, section, index) triple.
     * pool_seq packs section*64 + index. [C @0x001A6183] */
    int   pool_seq;
    /* road agent +0x00: the per-(record,slot) cruise speed from the TDESC
     * stage-2 table, with FUN_001A6070's +-15% jitter. [C @0x001A64B1] */
    float cruise_ms;
    int   paint;          /* FUN_001A5F90's paint draw [C @0x001A5F90] */
    unsigned char pool_seen;
    unsigned char stream_unit; // body+0x216: winning streamed.dat unit, ff off-unit
    unsigned char streamed;    // body+0x242C: base body update is permitted
    unsigned char asleep;      // body+0x20E: coupled to its towed partner
    unsigned char trailer_stream_unit;
    // TRAFFIC-2 -- lane state.  A traffic car drives a LANE, i.e. the route
    // polyline pushed sideways by the lane's recovered lateral offset; the
    // bare polyline is the road edge/median line and is not drivable.
    int   lane;           // index into B3_TRAFFIC_LANES
    float lane_lat;       // signed lateral offset of that lane (m)
    int   lane_dir;       // +1 ascending route index, -1 descending
    int   seg;            // PERSISTENT projection segment (continuity: a
                          // global nearest-point search snaps to another
                          // part of the loop once a knock throws the car
                          // far enough sideways, and it never comes back)
    float seg_t;
    unsigned short path_id; // FUN_0019FFA0 descriptor index (RIDX source)
    float path_cursor;      // road-agent +0x30 cursor, in descriptor rows
    signed char path_dir;   // +1 along the path, -1 against it (the
                            // request's `direction` byte -> ONCOMING)
    float path_lateral;     // road-agent +0x34, blended within each pair
    int   reservation_ahead;  // road-agent +0x44: next agent on this path
    float avoid_nudge;        // road-agent +0x1C: the persisted avoid dv
    unsigned char avoid_active;
    int   reservation_behind; // road-agent +0x45: previous agent on this path
    float off_time;       // s continuously outside the road cross-section
    float stall_time;     // s continuously below crawl speed
    float max_lat_err;    // diagnostics
    float brake_latch;    // S+0x10 of the retail road agent: the gap that
                          // armed the current brake, so a closing gap keeps
                          // tightening the target and an opening one releases
    // TRAFFIC-2 -- towing.  Trailers (TDESC list slot 5) have no front axle
    // and cannot drive; they only ever exist behind the event's slot-4
    // tractor.
    int   trailer;        // B3_TRAFFIC_CARS index of the towed trailer, -1
    int   trailer_ready;
    int   trailer_linked;
    Vec3  tr_pos;         // trailer mesh origin
    float tr_yaw;
    Vec3  tr_bogie;       // trailer bogie centre (articulation state)
} TrafficCar;

typedef struct { unsigned short point_a, point_b; } B3TrafficPathPair;
typedef struct {
    unsigned short target_row[4];
    unsigned char unknown[4];
    unsigned char target_path[4];
    unsigned char tail[2];
} B3TrafficPathLink;
typedef struct {
    unsigned int first_progress;
    unsigned int last_progress;
    unsigned int request_base;
    unsigned char request_count;
    unsigned char refresh_count;
    unsigned short pad;
} B3TrafficPoolWindow;
typedef struct {
    unsigned short first_row;
    unsigned short last_row;
    unsigned char path_id;
    unsigned char direction;
} B3TrafficPoolRequest;
/* TRAFFIC-MIX: traffic_paths.bin v4's spawn-policy section, extracted straight
 * out of the event TDESC by tools/extract_traffic.py.  See docs/RE_BGD.md 6. */
typedef struct {          /* one of FUN_001A5E30's six class lists */
    unsigned int cls;         /* the runtime class code: 1,2,3,4,5,0xB */
    unsigned int entry_base;
    unsigned int entry_count;
    unsigned int weight_total; /* the list's own +0x08 dword, the rng modulus */
} B3TrafficMixClass;
typedef struct {          /* one 0x18-byte traffic-set record */
    char id[16];              /* base-40 vehicle id, resolved to B3_TRAFFIC_CARS */
    unsigned int weight;      /* record +0x10 */
    unsigned char colours[8]; /* record +0x08..+0x0F, percentages summing to 100 */
    unsigned int reserved;
} B3TrafficMixEntry;
typedef struct {          /* FUN_0019E5B0's (path,row) -> (record,slot) map */
    unsigned char path_id, record, slot, pad;
    unsigned int start_row;
} B3TrafficMixBinding;
typedef struct {          /* the stage-2 speed + stage-3 rate tables */
    unsigned char record, slot;
    unsigned short pad;
    float speed_mph;
    float rate[6];            /* vehicles per minute, indexed BY CLASS CODE */
} B3TrafficMixRoad;
typedef B3TrafficReservationPath B3TrafficPath;
typedef struct {
    Vec3* points;
    B3TrafficPathPair* pairs;
    float* distances;       // two floats per pair row: cumulative distance, width
    B3TrafficPathLink* links; // FUN_001A0750's 0x12-byte source rows
    B3TrafficPoolWindow* pool_windows; // FUN_001A28B0's TDESC progress table
    B3TrafficPoolRequest* pool_requests; // its 6-byte path-range requests
    B3TrafficPath* paths;
    int* reservation_owner; // manager's DAT_00649B7C row-owner map
    B3TrafficMixClass* mix_classes;
    B3TrafficMixEntry* mix_entries;
    B3TrafficMixBinding* mix_bindings;
    B3TrafficMixRoad* mix_roads;
    int* mix_entry_car;     // resolved index into B3_TRAFFIC_CARS, -1 if absent
    unsigned int point_count, pair_count, path_count;
    unsigned int pool_window_count, pool_request_count;
    unsigned int mix_class_count, mix_entry_count;
    unsigned int mix_binding_count, mix_road_count;
    int loaded;
} B3TrafficPathData;

_Static_assert(sizeof(B3TrafficPathPair) == 4,
               "traffic_paths.bin pair layout");
_Static_assert(sizeof(B3TrafficPathLink) == 0x12,
               "traffic_paths.bin link row layout");
_Static_assert(sizeof(B3TrafficPoolWindow) == 0x10,
               "traffic_paths.bin pool-window layout");
_Static_assert(sizeof(B3TrafficPoolRequest) == 6,
               "traffic_paths.bin pool-request layout");
_Static_assert(sizeof(B3TrafficMixClass) == 16,
               "traffic_paths.bin mix class layout");
_Static_assert(sizeof(B3TrafficMixEntry) == 32,
               "traffic_paths.bin mix entry layout");
_Static_assert(sizeof(B3TrafficMixBinding) == 8,
               "traffic_paths.bin mix binding layout");
_Static_assert(sizeof(B3TrafficMixRoad) == 32,
               "traffic_paths.bin mix road layout");

static TrafficCar g_traffic[B3_TRAFFIC_N];
static int g_traffic_n = 0;                          // 0 = disabled/absent
static B3TrafficPool g_traffic_pool;
static B3CarHull  g_traffic_hull[B3_TRAFFIC_CAR_COUNT];
static int        g_traffic_hull_ok[B3_TRAFFIC_CAR_COUNT] = {0};
/* CRASH-SHOW H1b: FUN_00146530's per-traffic-car pass-voice state
 * ([ESI+0x24] cur, [ESI+0x28] prev, [ESI+0x6] cooldown). */
static B3SfxPassState g_pass_state[B3_TRAFFIC_N];
static GLuint g_traffic_lists[B3_TRAFFIC_CAR_COUNT] = {0};
static float g_traffic_ymin[B3_TRAFFIC_CAR_COUNT] = {0};
// Traffic wheels (same .bgv-family records, shared relinker [C]; their
// omission left traffic wheel-less and sunk to the body skirt, dump 023).
static GLuint g_traffic_wheel_lists[B3_TRAFFIC_CAR_COUNT] = {0};
static float  g_traffic_wheel_pos[B3_TRAFFIC_CAR_COUNT][6][3];
static int    g_traffic_wheel_mirror[B3_TRAFFIC_CAR_COUNT][6];
static int    g_traffic_wheel_count[B3_TRAFFIC_CAR_COUNT] = {0};
static float  g_traffic_wheel_radius[B3_TRAFFIC_CAR_COUNT] = {0};
static float  g_traffic_spin[B3_TRAFFIC_N] = {0};      // rad, presentation
static Vec3   g_traffic_prev[B3_TRAFFIC_N];            // for spin distance
static float g_traffic_len[B3_TRAFFIC_CAR_COUNT] = {0};
// Spawn slots aligned with the oncoming direction. The table carries BOTH
// race directions (docs/RE_BGD.md 4); a Forward event's oncoming traffic
// uses the subset whose heading runs WITH the reverse loop -- the rest
// would U-turn across the racing lane.
static int g_traffic_slots[B3_TRAFFIC_SPAWN_COUNT];
static int g_traffic_nslots = 0;
static B3TrafficPathData g_traffic_paths = {0};

// Mass from the 9-param reduced VDB set (offset 0xB8); traffic entries in
// B3_CAR_PHYSICS carry exactly that set (n_params == 9).
static float traffic_mass(const char* id) {
    for (int i = 0; i < B3_CAR_PHYSICS_COUNT; i++) {
        const B3CarPhysics* c = &B3_CAR_PHYSICS[i];
        if (strcmp(c->id, id) != 0) continue;
        for (int p = 0; p < c->n_params; p++)
            if (c->params[p].offset == 0x0B8)
                return c->params[p].value;
    }
    return 1200.0f;   // fallback: no VDB override for this id
}

// ===================== TRAFFIC-2: route / lane geometry =====================
// The route polyline (B3_ONCOMING) plus B3_TRAFFIC_LANES, whose lateral
// offsets and driving directions are recovered from the spawn/entry table
// (tools/extract_traffic.py lane_table(), evidence in INTEGRATION_NOTE.md).
// The polyline itself is at lateral 0 and is the road EDGE: on US_C3_V1 the
// four lanes sit +2.66 / +9.07 (driven descending) and +14.56 / +20.83
// (ascending) metres to one side of it.  Driving cars ON the polyline --
// what the harness used to do -- puts every one of them outside the
// outermost lane, which is the "traffic is not on the road" report.

static float g_trailer_axle_z[B3_TRAFFIC_CAR_COUNT] = {0};
static int traffic_legacy(void);

static void traffic_paths_free(void) {
    free(g_traffic_paths.points);
    free(g_traffic_paths.pairs);
    free(g_traffic_paths.distances);
    free(g_traffic_paths.links);
    free(g_traffic_paths.pool_windows);
    free(g_traffic_paths.pool_requests);
    free(g_traffic_paths.paths);
    free(g_traffic_paths.reservation_owner);
    free(g_traffic_paths.mix_classes);
    free(g_traffic_paths.mix_entries);
    free(g_traffic_paths.mix_bindings);
    free(g_traffic_paths.mix_roads);
    free(g_traffic_paths.mix_entry_car);
    memset(&g_traffic_paths, 0, sizeof(g_traffic_paths));
}

/* The extracted B3TP asset is the relocated source consumed by
 * FUN_0019FFA0/FUN_0019F1C0: one shared point pool plus per-path pair and
 * cumulative-distance rows. Version 2 also preserves FUN_001A0750's
 * 0x12-byte branch rows for the traffic-pool manager. The retail sampler blends each pair at a
 * lateral lane fraction, then evaluates the four surrounding cross-sections
 * with the uniform cubic B-spline basis. */
static void traffic_paths_load(void) {
    struct { char magic[4]; unsigned int version, point_count, path_count; } header;
    struct { unsigned int window_count, request_count; } pool_header = {0};
    const char* track = getenv("B3_TRACK");
    char path[256];
    FILE* file;
    unsigned int total = 0;

    if (!track) track = getenv("B3_POSTFX_TRACK");
    if (!track) track = "US_C3_V1";
    snprintf(path, sizeof(path), "build/tracks/%s/traffic_paths.bin", track);
    file = fopen(path, "rb");
    if (!file) return;
    if (!nav_read(file, &header, sizeof(header), 1)
        || memcmp(header.magic, "B3TP", 4) != 0
        || (header.version < 1 || header.version > 4)
        || header.point_count == 0 || header.point_count > 1000000
        || header.path_count == 0 || header.path_count > 4096) {
        fclose(file);
        return;
    }
    if (header.version >= 3
        && (!nav_read(file, &pool_header, sizeof(pool_header), 1)
            || pool_header.window_count > 4096
            || pool_header.request_count > 1000000)) {
        fclose(file);
        return;
    }
    traffic_paths_free();
    g_traffic_paths.points = calloc(header.point_count,
                                    sizeof(*g_traffic_paths.points));
    g_traffic_paths.paths = calloc(header.path_count,
                                   sizeof(*g_traffic_paths.paths));
    if (!g_traffic_paths.points || !g_traffic_paths.paths
        || !nav_read(file, g_traffic_paths.points,
                     sizeof(*g_traffic_paths.points), header.point_count))
        goto fail;
    for (unsigned int path_id = 0; path_id < header.path_count; path_id++) {
        unsigned int count;
        B3TrafficPathPair* pairs;
        float* distances;
        B3TrafficPathLink* links;
        if (!nav_read(file, &count, sizeof(count), 1) || count < 2
            || count > 1000000 || total > 2000000 - count)
            goto fail;
        pairs = realloc(g_traffic_paths.pairs,
                        (size_t)(total + count) * sizeof(*pairs));
        if (!pairs) goto fail;
        g_traffic_paths.pairs = pairs;
        distances = realloc(g_traffic_paths.distances,
                            (size_t)(total + count) * 2 * sizeof(*distances));
        if (!distances) goto fail;
        g_traffic_paths.distances = distances;
        if (header.version >= 2) {
            links = realloc(g_traffic_paths.links,
                            (size_t)(total + count) * sizeof(*links));
            if (!links) goto fail;
            g_traffic_paths.links = links;
        }
        g_traffic_paths.paths[path_id] = (B3TrafficPath){total, count};
        if (!nav_read(file, pairs + total, sizeof(*pairs), count)
            || !nav_read(file, distances + (size_t)total * 2,
                         sizeof(*distances) * 2, count))
            goto fail;
        if (header.version >= 2
            && !nav_read(file, g_traffic_paths.links + total,
                         sizeof(*g_traffic_paths.links), count))
            goto fail;
        for (unsigned int row = 0; row < count; row++) {
            B3TrafficPathPair pair = pairs[total + row];
            if (pair.point_a >= header.point_count
                || pair.point_b >= header.point_count)
                goto fail;
            if (header.version >= 2) {
                const B3TrafficPathLink* link = &g_traffic_paths.links[total + row];
                for (int column = 0; column < 4; column++) {
                    if (link->target_path[column] != 0xff
                        && link->target_path[column] >= header.path_count)
                        goto fail;
                }
            }
        }
        total += count;
    }
    if (header.version >= 3) {
        g_traffic_paths.pool_windows = calloc(pool_header.window_count,
                                               sizeof(*g_traffic_paths.pool_windows));
        g_traffic_paths.pool_requests = calloc(pool_header.request_count,
                                                sizeof(*g_traffic_paths.pool_requests));
        if ((pool_header.window_count && !g_traffic_paths.pool_windows)
            || (pool_header.request_count && !g_traffic_paths.pool_requests)
            || !nav_read(file, g_traffic_paths.pool_windows,
                         sizeof(*g_traffic_paths.pool_windows),
                         pool_header.window_count)
            || !nav_read(file, g_traffic_paths.pool_requests,
                         sizeof(*g_traffic_paths.pool_requests),
                         pool_header.request_count))
            goto fail;
        for (unsigned int window = 0; window < pool_header.window_count;
             window++) {
            const B3TrafficPoolWindow* pool = &g_traffic_paths.pool_windows[window];
            if (pool->first_progress > pool->last_progress
                || pool->request_base > pool_header.request_count
                || pool->request_count > pool_header.request_count - pool->request_base)
                goto fail;
        }
        for (unsigned int request = 0; request < pool_header.request_count;
             request++) {
            const B3TrafficPoolRequest* pool = &g_traffic_paths.pool_requests[request];
            if (pool->path_id >= header.path_count
                || pool->first_row >= g_traffic_paths.paths[pool->path_id].count
                || pool->last_row >= g_traffic_paths.paths[pool->path_id].count)
                goto fail;
        }
    }
    if (header.version >= 4) {
        struct { unsigned int classes, entries, bindings, roads; } mix;
        if (!nav_read(file, &mix, sizeof(mix), 1)
            || mix.classes > 64 || mix.entries > 4096
            || mix.bindings > 65536 || mix.roads > 4096)
            goto fail;
        g_traffic_paths.mix_classes = calloc(mix.classes ? mix.classes : 1,
                                             sizeof(*g_traffic_paths.mix_classes));
        g_traffic_paths.mix_entries = calloc(mix.entries ? mix.entries : 1,
                                             sizeof(*g_traffic_paths.mix_entries));
        g_traffic_paths.mix_bindings = calloc(mix.bindings ? mix.bindings : 1,
                                              sizeof(*g_traffic_paths.mix_bindings));
        g_traffic_paths.mix_roads = calloc(mix.roads ? mix.roads : 1,
                                           sizeof(*g_traffic_paths.mix_roads));
        g_traffic_paths.mix_entry_car = calloc(mix.entries ? mix.entries : 1,
                                               sizeof(*g_traffic_paths.mix_entry_car));
        if (!g_traffic_paths.mix_classes || !g_traffic_paths.mix_entries
            || !g_traffic_paths.mix_bindings || !g_traffic_paths.mix_roads
            || !g_traffic_paths.mix_entry_car
            || !nav_read(file, g_traffic_paths.mix_classes,
                         sizeof(*g_traffic_paths.mix_classes), mix.classes)
            || !nav_read(file, g_traffic_paths.mix_entries,
                         sizeof(*g_traffic_paths.mix_entries), mix.entries)
            || !nav_read(file, g_traffic_paths.mix_bindings,
                         sizeof(*g_traffic_paths.mix_bindings), mix.bindings)
            || !nav_read(file, g_traffic_paths.mix_roads,
                         sizeof(*g_traffic_paths.mix_roads), mix.roads))
            goto fail;
        for (unsigned int index = 0; index < mix.classes; index++) {
            const B3TrafficMixClass* group = &g_traffic_paths.mix_classes[index];
            if (group->entry_base > mix.entries
                || group->entry_count > mix.entries - group->entry_base)
                goto fail;
        }
        for (unsigned int index = 0; index < mix.bindings; index++)
            if (g_traffic_paths.mix_bindings[index].path_id >= header.path_count)
                goto fail;
        for (unsigned int index = 0; index < mix.entries; index++) {
            g_traffic_paths.mix_entry_car[index] = -1;
            g_traffic_paths.mix_entries[index].id[15] = '\0';
            for (int car = 0; car < B3_TRAFFIC_CAR_COUNT; car++)
                if (strcmp(g_traffic_paths.mix_entries[index].id,
                           B3_TRAFFIC_CARS[car].id) == 0) {
                    g_traffic_paths.mix_entry_car[index] = car;
                    break;
                }
        }
        g_traffic_paths.mix_class_count = mix.classes;
        g_traffic_paths.mix_entry_count = mix.entries;
        g_traffic_paths.mix_binding_count = mix.bindings;
        g_traffic_paths.mix_road_count = mix.roads;
    }
    if (fgetc(file) != EOF) goto fail;
    fclose(file);
    g_traffic_paths.point_count = header.point_count;
    g_traffic_paths.pair_count = total;
    g_traffic_paths.path_count = header.path_count;
    g_traffic_paths.pool_window_count = pool_header.window_count;
    g_traffic_paths.pool_request_count = pool_header.request_count;
    g_traffic_paths.reservation_owner = malloc(
        (size_t)total * sizeof(*g_traffic_paths.reservation_owner));
    if (!g_traffic_paths.reservation_owner) goto fail;
    g_traffic_paths.loaded = 1;
    printf("[Burnout3] retail traffic paths: %u points, %u paths, %u rows, %u pool windows\n",
           g_traffic_paths.point_count, g_traffic_paths.path_count,
           g_traffic_paths.pair_count, g_traffic_paths.pool_window_count);
    if (g_traffic_paths.mix_road_count)
        printf("[Burnout3] retail spawn policy: %u class lists / %u models, "
               "%u path->record bindings, %u road speed+rate rows\n",
               g_traffic_paths.mix_class_count, g_traffic_paths.mix_entry_count,
               g_traffic_paths.mix_binding_count, g_traffic_paths.mix_road_count);
    return;
fail:
    fclose(file);
    traffic_paths_free();
}

static int traffic_paths_active(void) {
    return g_traffic_paths.loaded && !traffic_legacy();
}

static Vec3 traffic_path_midpoint(unsigned int path_id, unsigned int row) {
    const B3TrafficPath* path = &g_traffic_paths.paths[path_id];
    B3TrafficPathPair pair = g_traffic_paths.pairs[path->pair_base + row];
    Vec3 a = g_traffic_paths.points[pair.point_a];
    Vec3 b = g_traffic_paths.points[pair.point_b];
    return (Vec3){(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f,
                  (a.z + b.z) * 0.5f};
}

static Vec3 traffic_path_cross_section(unsigned int path_id, unsigned int row,
                                       float lateral) {
    const B3TrafficPath* path = &g_traffic_paths.paths[path_id];
    B3TrafficPathPair pair = g_traffic_paths.pairs[path->pair_base + row];
    Vec3 a = g_traffic_paths.points[pair.point_a];
    Vec3 b = g_traffic_paths.points[pair.point_b];
    return vec3_add(a, vec3_scale(lateral, vec3_sub(b, a)));
}

static void traffic_path_sample(unsigned int path_id, float cursor,
                                float lateral,
                                Vec3* out_pos, Vec3* out_tangent) {
    const B3TrafficPath* path = &g_traffic_paths.paths[path_id];
    int row = (int)floorf(cursor);
    float u;
    Vec3 knot[4];
    Vec3 c0, c1, c2, c3;
    Vec3 tangent;
    if (row < 0) row = 0;
    if (row >= (int)path->count - 1) {
        row = (int)path->count - 2;
        cursor = (float)(path->count - 1);
    }
    u = cursor - (float)row;
    for (int offset = -1; offset <= 2; offset++) {
        int knot_row = row + offset;
        if (knot_row < 0) knot_row = 0;
        if (knot_row >= (int)path->count) knot_row = (int)path->count - 1;
        knot[offset + 1] = traffic_path_cross_section(path_id,
                                                       (unsigned int)knot_row,
                                                       lateral);
    }
    c0 = vec3_scale(1.0f / 6.0f,
                    vec3_add(knot[0], vec3_add(vec3_scale(4.0f, knot[1]),
                                                knot[2])));
    c1 = vec3_scale(0.5f, vec3_sub(knot[2], knot[0]));
    c2 = vec3_scale(0.5f, vec3_add(knot[0],
                                    vec3_add(vec3_scale(-2.0f, knot[1]),
                                             knot[2])));
    c3 = vec3_scale(1.0f / 6.0f,
                    vec3_add(vec3_scale(-1.0f, knot[0]),
                             vec3_add(vec3_scale(3.0f, knot[1]),
                             vec3_add(vec3_scale(-3.0f, knot[2]), knot[3]))));
    *out_pos = vec3_add(c0, vec3_add(vec3_scale(u, c1),
                    vec3_add(vec3_scale(u * u, c2),
                             vec3_scale(u * u * u, c3))));
    tangent = vec3_add(c1, vec3_add(vec3_scale(2.0f * u, c2),
                                     vec3_scale(3.0f * u * u, c3)));
    if (vec3_len(tangent) < 1e-4f)
        tangent = vec3_sub(knot[2], knot[1]);
    *out_tangent = vec3_normalize(tangent);
}

/* FUN_0019F1C0 consumes distance rows to convert metres into persistent
 * descriptor-row cursor movement, then retires an agent at the path end. */
static int traffic_path_advance(TrafficCar* t, float metres) {
    const B3TrafficPath* path = &g_traffic_paths.paths[t->path_id];
    float remain = metres > 0.0f ? metres : 0.0f;
    if (t->path_dir < 0) {
        /* ONCOMING: run the agent DOWN its descriptor rows. */
        while (remain > 1e-4f && t->path_cursor > 0.0f) {
            unsigned int row = (unsigned int)floorf(t->path_cursor);
            float fraction = t->path_cursor - (float)row;
            if (fraction <= 1e-6f) {
                if (row == 0) break;
                row--;
                fraction = 1.0f;
            }
            float segment =
                g_traffic_paths.distances[(path->pair_base + row + 1) * 2]
              - g_traffic_paths.distances[(path->pair_base + row) * 2];
            if (segment < 1e-4f) { t->path_cursor = (float)row; continue; }
            float available = fraction * segment;
            if (available > remain) {
                t->path_cursor -= remain / segment;
                remain = 0.0f;
            } else {
                t->path_cursor = (float)row;
                remain -= available;
            }
        }
        return t->path_cursor <= 0.0f;
    }
    while (remain > 1e-4f && t->path_cursor < (float)(path->count - 1)) {
        unsigned int row = (unsigned int)floorf(t->path_cursor);
        if (row >= path->count - 1) break;
        float segment = g_traffic_paths.distances[(path->pair_base + row + 1) * 2]
                      - g_traffic_paths.distances[(path->pair_base + row) * 2];
        if (segment < 1e-4f) {
            t->path_cursor = (float)(row + 1);
            continue;
        }
        float fraction = t->path_cursor - (float)row;
        float available = (1.0f - fraction) * segment;
        if (available > remain) {
            t->path_cursor += remain / segment;
            remain = 0.0f;
        } else {
            t->path_cursor = (float)(row + 1);
            remain -= available;
        }
    }
    return t->path_cursor >= (float)(path->count - 1);
}

static float traffic_path_distance(unsigned int path_id, float cursor) {
    const B3TrafficPath* path = &g_traffic_paths.paths[path_id];
    if (cursor <= 0.0f)
        return g_traffic_paths.distances[(size_t)path->pair_base * 2];
    if (cursor >= (float)(path->count - 1))
        return g_traffic_paths.distances[
            (size_t)(path->pair_base + path->count - 1) * 2];
    unsigned int row = (unsigned int)floorf(cursor);
    float start = g_traffic_paths.distances[(size_t)(path->pair_base + row) * 2];
    float end = g_traffic_paths.distances[(size_t)(path->pair_base + row + 1) * 2];
    return start + (end - start) * (cursor - (float)row);
}

/* FUN_001A03F0/FUN_001A0600 maintain a dynamic owner byte for every
 * descriptor row and a linked list of agents on that descriptor.  Rebuild
 * the same state from the harness pool after each cursor pass: each row names
 * the nearest agent at or ahead of it, and every agent names the next one
 * ahead.  The RIDX asset has the row topology; retail's cross-descriptor
 * branch reassignment remains a separate manager feature. */
static void traffic_reservations_rebuild(void) {
    B3TrafficReservationAgent agents[B3_TRAFFIC_N];
    if (!traffic_paths_active() || !g_traffic_paths.reservation_owner) return;
    for (int index = 0; index < g_traffic_n; index++) {
        const TrafficCar* traffic = &g_traffic[index];
        agents[index] = (B3TrafficReservationAgent){
            traffic->path_id, traffic->path_cursor, traffic->active,
            -1, -1
        };
    }
    b3_traffic_reservations_rebuild(g_traffic_paths.paths,
                                    g_traffic_paths.path_count, agents,
                                    g_traffic_n,
                                    g_traffic_paths.reservation_owner,
                                    g_traffic_paths.pair_count);
    for (int index = 0; index < g_traffic_n; index++) {
        g_traffic[index].reservation_ahead = agents[index].ahead;
        g_traffic[index].reservation_behind = agents[index].behind;
    }
}

// Frame at (segment, t): centre point, height and unit tangent.
static void route_frame(int seg, float t, float* cx, float* cy, float* cz,
                        float* tx, float* tz) {
    const float* a = B3_ONCOMING[seg];
    const float* b = B3_ONCOMING[(seg + 1) % B3_ONCOMING_COUNT];
    float ax = b[0] - a[0], az = b[2] - a[2];
    float l = sqrtf(ax * ax + az * az);
    if (l < 1e-6f) l = 1.0f;
    *cx = a[0] + ax * t;
    *cz = a[2] + az * t;
    *cy = a[1] + (b[1] - a[1]) * t;
    *tx = ax / l;
    *tz = az / l;
}

// Nearest point on the polyline, searched in a WINDOW of +-win segments
// around `from` (win < 0 = whole loop).  Returns the squared distance and
// the SIGNED lateral offset in *out_lat, with the same sign convention the
// extractor used for the lane table (s = tx*(z-cz) - tz*(x-cx)).
static float route_project(float x, float z, int from, int win,
                           int* out_seg, float* out_t, float* out_lat) {
    float best = 1e30f;
    int bs = from;
    float bt = 0.0f;
    int lo = (win < 0) ? 0 : -win;
    int hi = (win < 0) ? B3_ONCOMING_COUNT - 1 : win;
    for (int k = lo; k <= hi; k++) {
        int i = (from + k) % B3_ONCOMING_COUNT;
        if (i < 0) i += B3_ONCOMING_COUNT;
        const float* a = B3_ONCOMING[i];
        const float* b = B3_ONCOMING[(i + 1) % B3_ONCOMING_COUNT];
        float ax = b[0] - a[0], az = b[2] - a[2];
        float l2 = ax * ax + az * az;
        float t = l2 > 1e-9f ? ((x - a[0]) * ax + (z - a[2]) * az) / l2 : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float cx = a[0] + ax * t, cz = a[2] + az * t;
        float d2 = (x - cx) * (x - cx) + (z - cz) * (z - cz);
        if (d2 < best) { best = d2; bs = i; bt = t; }
    }
    if (out_seg) *out_seg = bs;
    if (out_t)   *out_t = bt;
    if (out_lat) {
        float cx, cy, cz, tx, tz;
        route_frame(bs, bt, &cx, &cy, &cz, &tx, &tz);
        *out_lat = tx * (z - cz) - tz * (x - cx);
    }
    return best;
}

// Walk `adv` metres along the polyline from (seg,t) in direction dir.
static void route_advance(int seg, float t, int dir, float adv,
                          int* oseg, float* ot) {
    float remain = adv > 0.0f ? adv : 0.0f;
    int i = seg;
    float u = t;
    for (int g = 0; g < B3_ONCOMING_COUNT && remain > 1e-4f; g++) {
        const float* a = B3_ONCOMING[i];
        const float* b = B3_ONCOMING[(i + 1) % B3_ONCOMING_COUNT];
        float ax = b[0] - a[0], az = b[2] - a[2];
        float sl = sqrtf(ax * ax + az * az);
        if (sl < 1e-4f) sl = 1e-4f;
        float avail = (dir > 0) ? (1.0f - u) * sl : u * sl;
        if (avail >= remain) {
            u += (dir > 0) ? remain / sl : -remain / sl;
            remain = 0.0f;
            break;
        }
        remain -= avail;
        if (dir > 0) { i = (i + 1) % B3_ONCOMING_COUNT; u = 0.0f; }
        else         { i = (i + B3_ONCOMING_COUNT - 1) % B3_ONCOMING_COUNT;
                       u = 1.0f; }
    }
    *oseg = i;
    *ot = u;
}

// World point of a lane at (seg,t): the polyline pushed sideways by `lat`.
static void lane_point(int seg, float t, float lat, Vec3* out) {
    float cx, cy, cz, tx, tz;
    route_frame(seg, t, &cx, &cy, &cz, &tx, &tz);
    out->x = cx + (-tz) * lat;
    out->y = cy;
    out->z = cz + tx * lat;
}

// B3_TRAFFIC_LEGACY=1: reproduce the OLD follower for a before/after
// measurement -- every car drives the bare polyline (lane offset 0) and the
// watchdogs are off.  Diagnostic only.
static int traffic_legacy(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("B3_TRAFFIC_LEGACY");
        v = (e && atoi(e)) ? 1 : 0;
    }
    return v;
}

static void traffic_lane_of(TrafficCar* t, int lane) {
    if (B3_TRAFFIC_LANE_COUNT > 0) {
        lane = ((lane % B3_TRAFFIC_LANE_COUNT) + B3_TRAFFIC_LANE_COUNT)
               % B3_TRAFFIC_LANE_COUNT;
        t->lane = lane;
        t->lane_lat = B3_TRAFFIC_LANES[lane].lat;
        /* WRONG-SIDE FIX [C].  B3_TRAFFIC_LANES' `dir` column is INVERTED:
         * it calls the two lanes the RACERS THEMSELVES OCCUPY "oncoming".
         * Proof, all in this repo's own recovered data and independent of
         * any runtime:
         *   - the game's six START-GRID slots (burnout3_start_grid.h, the
         *     .bgd SPATIAL record @0xa000 read by the game's own parser
         *     FUN_0018B250 [C]) project onto the route polyline at lateral
         *     +14.29 +21.44 +15.55 +21.72 +14.87 +21.42 -- i.e. exactly the
         *     lanes the table lists as 14.559 and 20.831 -- and all six face
         *     the DESCENDING polyline direction (fwd . asc_tangent = -1.00);
         *   - the game's own race line (burnout3_track_paths.h @0xc0930)
         *     spans lateral +11.4..+24.1, median +14.7, and its tangent dots
         *     the ascending polyline tangent at -1.000 over its whole length;
         *   - so DESCENDING polyline index IS the race direction, and the
         *     +14.559 / +20.831 lanes are the WITH-RACE pair.  The table
         *     marks them dir=+1 (ascending = against the race), and marks
         *     the far carriageway (+2.658 / +9.073) dir=-1.
         * Effect of the bug: every traffic car in the racers' own two lanes
         * drives head-on at them (correctly lit -- the corona gate is fine),
         * and the "with-race" traffic runs on the far carriageway.  That is
         * both halves of the user report.
         * The PERMANENT fix belongs in tools/extract_traffic.py's
         * lane_table() (sibling-owned, regenerates this header); this is the
         * runtime correction.  B3_TRAFFIC_LANE_DIR=raw restores the header
         * value for an A/B. */
        /* DOUBLE-FLIP FIX: the runtime negation that used to live here was
         * the AI-DRIVE wave's stand-in while the extractor was sibling-
         * owned. The PERMANENT fix (spawn dir[3] is the matrix Z column =
         * the BACKWARD axis) landed in tools/extract_traffic.py (commit
         * 7b70f69) and the header/traffic.bin were regenerated -- so the
         * runtime flip inverted the corrected data straight back (user:
         * RV on the left, rivals meeting head-on traffic again). The
         * header value is now used as-is. */
        t->lane_dir = B3_TRAFFIC_LANES[lane].dir >= 0 ? 1 : -1;
    } else {
        t->lane = 0; t->lane_lat = 0.0f; t->lane_dir = 1;   // no lane data
    }
    if (traffic_legacy()) t->lane_lat = 0.0f;
}

/* FUN_00120F30's residency gate.  The loaded collision file retains the
 * source streamed.dat unit on every triangle, so the normal body ground
 * query supplies the same identity the retail streamer writes at +0x216.
 * On a 0->1 transition FUN_001213C0 clears the force/impulse accumulators;
 * outside every unit the base update is skipped. */
static int traffic_stream_refresh(TrafficCar* t) {
    unsigned char unit = 0xff;
    float height, normal[3];
    int in_unit = b3_ground_probe_unit(t->pos.x, t->pos.y + 3.0f, t->pos.z,
                                       &height, normal, &unit) >= 0;
    if (!in_unit) unit = 0xff;
    t->stream_unit = unit;
    if (unit == 0xff) {
        t->streamed = 0;
        t->asleep = 1;
        return 0;
    }
    if (!t->streamed) {
        memset(t->rb.force_acc, 0, sizeof(t->rb.force_acc));
        memset(t->rb.torque_acc, 0, sizeof(t->rb.torque_acc));
        memset(t->rb.imp_force, 0, sizeof(t->rb.imp_force));
        memset(t->rb.imp_torque, 0, sizeof(t->rb.imp_torque));
        memset(t->rb.deflection, 0, sizeof(t->rb.deflection));
        t->streamed = 1;
    }
    if (t->trailer < 0) t->asleep = 0;
    return 1;
}

static void traffic_trailer_target(const TrafficCar* t, Vec3* out) {
    const float* tow = B3_TRAFFIC_CARS[t->car].tow_anchor;
    const float* king = B3_TRAFFIC_CARS[t->trailer].king_anchor;
    float cy = cosf(t->yaw), sy = sinf(t->yaw);
    float hx = t->pos.x + cy * tow[0] + sy * tow[2];
    float hy = t->pos.y - 0.5f - g_traffic_ymin[t->car] + tow[1];
    float hz = t->pos.z + sy * tow[0] - cy * tow[2];
    out->x = hx - cy * king[0] - sy * king[2];
    out->y = hy - king[1] + 0.5f + g_traffic_ymin[t->trailer];
    out->z = hz - sy * king[0] + cy * king[2];
}

static void traffic_tow_sleep_refresh(TrafficCar* t) {
    if (!t->streamed || t->trailer < 0 || !t->trailer_linked) return;
    unsigned char unit = 0xff;
    float height, normal[3];
    Vec3 target;
    int was_asleep = t->asleep;
    traffic_trailer_target(t, &target);
    if (b3_ground_probe_unit(target.x, target.y + 3.0f, target.z,
                             &height, normal, &unit) < 0)
        unit = 0xff;
    t->trailer_stream_unit = unit;
    /* FUN_00120F30 writes both body+0x20E bytes together: either partner
     * outside a resident unit sleeps the whole articulated pair. */
    t->asleep = (unit == 0xff);
    if (was_asleep && !t->asleep) {
        t->tr_pos = target;
        t->tr_bogie = target;
        t->tr_yaw = t->yaw;
        carcol_synth_rb(&t->trailer_rb, t->tr_pos,
                         t->tr_pos.y - 0.5f - g_traffic_ymin[t->trailer],
                         t->tr_yaw,
                         (Vec3){sinf(t->yaw) * t->speed, 0.0f,
                                -cosf(t->yaw) * t->speed});
        t->trailer_ready = 1;
    }
}

static void traffic_reset_pose(TrafficCar* t) {
    /* The road's own cruise speed when the spawn policy supplied one (retail
     * runs 30..60 mph per carriageway on US_C3_V1); the flat 50 mph cap only
     * where it did not. */
    t->speed = t->cruise_ms > 1.0f ? t->cruise_ms : B3_TRAFFIC_SPEED_MS;
    carcol_synth_rb(&t->rb, t->pos,
                    t->pos.y - 0.5f - g_traffic_ymin[t->car], t->yaw,
                    (Vec3){sinf(t->yaw) * t->speed, 0.0f,
                           -cosf(t->yaw) * t->speed});
    t->crashed_until = 0.0f;
    t->off_time = 0.0f;
    t->stall_time = 0.0f;
    t->active = 1;
    /* TRAFFIC-POOL: a reset pose means "this slot is a FRESH car now", so it
     * carries no pool reservation until one is stamped on it.  The legacy
     * lane recycler also reaches this function, and when it revived a slot
     * that still held a stale pool_request the very next
     * traffic_pool_refresh() saw `active && pool_request >= 0 && !pool_seen`
     * and released the car it had just spawned -- 26 cars per pass appearing
     * and vanishing.  traffic_pool_assign_request() re-stamps ownership
     * immediately AFTER seeding, so the pool path is unaffected. */
    t->pool_request = -1;
    t->pool_owner = -1;
    t->pool_agent = -1;
    t->pool_seen = 0;
    t->stream_unit = 0xff;
    t->streamed = 0;
    t->asleep = 1;
    t->trailer_stream_unit = 0xff;
    t->trailer_linked = t->trailer >= 0;
    (void)traffic_stream_refresh(t);
    // reset the articulation so a recycled tractor does not drag its
    // trailer across the map
    if (t->trailer >= 0) {
        const float* tow = B3_TRAFFIC_CARS[t->car].tow_anchor;
        const float* king = B3_TRAFFIC_CARS[t->trailer].king_anchor;
        float cy = cosf(t->yaw), sy = sinf(t->yaw);
        float hx = t->pos.x + cy * tow[0] + sy * tow[2];
        float hy = t->pos.y - 0.5f - g_traffic_ymin[t->car] + tow[1];
        float hz = t->pos.z + sy * tow[0] - cy * tow[2];
        t->tr_pos = (Vec3){hx - cy * king[0] - sy * king[2],
                            hy - king[1] + 0.5f
                                + g_traffic_ymin[t->trailer],
                            hz - sy * king[0] + cy * king[2]};
        t->tr_bogie = t->tr_pos;
        t->tr_yaw = t->yaw;
        carcol_synth_rb(&t->trailer_rb, t->tr_pos,
                         t->tr_pos.y - 0.5f - g_traffic_ymin[t->trailer],
                         t->tr_yaw,
                         (Vec3){sinf(t->yaw) * t->speed, 0.0f,
                                -cosf(t->yaw) * t->speed});
        t->trailer_ready = 1;
        traffic_tow_sleep_refresh(t);
    }
}

// Place a car in its lane `arc` metres from `ref` along the polyline, on the
// side given by `sign` (+1 = in the +index direction from ref).  This legacy
// fallback stays available when a track has no extracted RIDX traffic paths.
static void traffic_place(TrafficCar* t, int lane, const Vec3* ref,
                          float arc, int sign) {
    traffic_lane_of(t, lane);
    t->cruise_ms = 0.0f;      // legacy seeder: no road policy behind this car
    int rs = 0;
    float rt = 0.0f;
    route_project(ref->x, ref->z, 0, -1, &rs, &rt, NULL);
    int seg; float u;
    route_advance(rs, rt, sign >= 0 ? 1 : -1, arc, &seg, &u);
    Vec3 p;
    lane_point(seg, u, t->lane_lat, &p);
    t->pos = (Vec3){p.x, p.y + 0.5f, p.z};
    t->seg = seg;
    t->seg_t = u;
    int aseg; float at2;
    route_advance(seg, u, t->lane_dir, 8.0f, &aseg, &at2);
    Vec3 ah;
    lane_point(aseg, at2, t->lane_lat, &ah);
    t->yaw = atan2f(ah.x - p.x, -(ah.z - p.z));
    traffic_reset_pose(t);
}

/* Retail's source does not provide a public spawn-to-path association: the
 * manager owns the agent pool.  Select the nearest extracted pair midpoint to
 * the event's own spawn seed, then let FUN_0019F1C0-compatible cursor motion
 * carry that agent to this segment's terminal retirement. */
static int traffic_path_seed(TrafficCar* t, int spawn) {
    float best = 1e30f;
    t->cruise_ms = 0.0f;      // legacy seeder: no road policy behind this car
    unsigned int best_path = 0, best_row = 0;
    if (!traffic_paths_active() || B3_TRAFFIC_SPAWN_COUNT <= 0) return 0;
    spawn = ((spawn % B3_TRAFFIC_SPAWN_COUNT) + B3_TRAFFIC_SPAWN_COUNT)
          % B3_TRAFFIC_SPAWN_COUNT;
    const float* source = B3_TRAFFIC_SPAWN[spawn];
    for (unsigned int path_id = 0; path_id < g_traffic_paths.path_count;
         path_id++) {
        const B3TrafficPath* path = &g_traffic_paths.paths[path_id];
        for (unsigned int row = 0; row < path->count; row++) {
            Vec3 point = traffic_path_midpoint(path_id, row);
            float dx = point.x - source[0], dz = point.z - source[2];
            float d2 = dx * dx + dz * dz;
            if (d2 < best) {
                best = d2;
                best_path = path_id;
                best_row = row;
            }
        }
    }
    if (best == 1e30f) return 0;
    t->spawn = spawn;
    traffic_lane_of(t, spawn);
    t->path_id = (unsigned short)best_path;
    t->path_cursor = (float)best_row;
    static unsigned int lateral_rng = 0x6d2b79f5u;
    lateral_rng = lateral_rng * 1664525u + 1013904223u;
    t->path_lateral = 0.5f
                    + ((float)(lateral_rng % 1000u) - 500.0f) * 0.0001f;
    Vec3 point, tangent;
    traffic_path_sample(best_path, t->path_cursor, t->path_lateral,
                        &point, &tangent);
    t->pos = (Vec3){point.x, point.y + 0.5f, point.z};
    t->yaw = atan2f(tangent.x, -tangent.z);
    route_project(t->pos.x, t->pos.z, 0, -1, &t->seg, &t->seg_t, NULL);
    traffic_reset_pose(t);
    return 1;
}

/* ================= TRAFFIC-MIX: retail's own spawn policy =================
 * FUN_00048760 @0x00048760 is the game's generator; the traffic manager keeps
 * its own {state, carry} pair at 0x00649B28/0x00649B2C and FUN_001A3EA0
 * @0x001A3EA7/@0x001A3EB1 seeds it with exactly the two constants below.  The
 * conversion to a float is `fild` of the signed dword, +2^32 when negative
 * (0x003B16A8), times 1/2^32 -- i.e. (float)(uint32)state / 2^32.  [C]
 * This replaces the harness's two ad-hoc LCGs and closes PH-07's
 * "retail RNG sequence" item. */
static unsigned int g_traffic_rng_state = 0xfd462907u;
static unsigned int g_traffic_rng_carry = 0x02b9d6f8u;

static unsigned int traffic_rng_u32(void) {
    unsigned int next = (g_traffic_rng_state << 16)
                      + (unsigned int)(int)(short)(g_traffic_rng_state >> 16)
                      + g_traffic_rng_carry;
    g_traffic_rng_carry += next;
    g_traffic_rng_state = next;
    return next;
}

static float traffic_rng_f(void) {
    return (float)traffic_rng_u32() * (1.0f / 4294967296.0f);
}

/* FUN_0019E5B0 @0x0019E5B0: the manager record/slot that owns `row` of `path`
 * is the binding with the LARGEST start_row <= row.  FUN_0019E640 is the
 * mirror (smallest start_row > row) and gives the section's end. */
static const B3TrafficMixRoad* traffic_mix_road(unsigned int path_id,
                                                unsigned int row) {
    int best = -1;
    unsigned int best_row = 0;
    for (unsigned int index = 0; index < g_traffic_paths.mix_binding_count;
         index++) {
        const B3TrafficMixBinding* bind = &g_traffic_paths.mix_bindings[index];
        if (bind->path_id != path_id || bind->start_row > row) continue;
        if (best < 0 || bind->start_row > best_row) {
            best = (int)index;
            best_row = bind->start_row;
        }
    }
    if (best < 0) return NULL;
    for (unsigned int index = 0; index < g_traffic_paths.mix_road_count;
         index++) {
        const B3TrafficMixRoad* road = &g_traffic_paths.mix_roads[index];
        if (road->record == g_traffic_paths.mix_bindings[best].record
            && road->slot == g_traffic_paths.mix_bindings[best].slot)
            return road;
    }
    return NULL;
}

static unsigned int traffic_mix_next_start(unsigned int path_id,
                                           unsigned int row, int* found) {
    unsigned int best = 0;
    *found = 0;
    for (unsigned int index = 0; index < g_traffic_paths.mix_binding_count;
         index++) {
        const B3TrafficMixBinding* bind = &g_traffic_paths.mix_bindings[index];
        if (bind->path_id != path_id || bind->start_row <= row) continue;
        if (!*found || bind->start_row < best) {
            best = bind->start_row;
            *found = 1;
        }
    }
    return best;
}

/* FUN_001A6590 @0x001A6590.  The 6-float rate table at [record+slot*4+0x10] is
 * indexed BY CLASS CODE: entry 0 is unused (0.0 in every shipped row, which is
 * what keeps FUN_001A5E30's divide-by-zero class-0 path unreachable). */
static int traffic_mix_pick_class(const B3TrafficMixRoad* road) {
    float sum = 0.0f, draw, acc = 0.0f;
    for (int cls = 0; cls < 6; cls++) sum += road->rate[cls];
    draw = traffic_rng_f() * sum;
    for (int cls = 0; cls < 6; cls++) {
        acc += road->rate[cls];
        if (acc > draw) return cls;
    }
    return 1;
}

/* FUN_001A5E30 @0x001A5E30: `rng % list_total` against the running sum of each
 * 0x18-byte record's u32 weight at +0x10, `<=` compare, single-entry lists
 * short-circuit without consuming a draw. */
static int traffic_mix_pick_model(int cls) {
    const B3TrafficMixClass* group = NULL;
    unsigned int draw, acc = 0;
    for (unsigned int index = 0; index < g_traffic_paths.mix_class_count;
         index++)
        if ((int)g_traffic_paths.mix_classes[index].cls == cls)
            group = &g_traffic_paths.mix_classes[index];
    if (!group || group->entry_count == 0 || group->weight_total == 0)
        return -1;
    if (group->entry_count == 1) return (int)group->entry_base;
    draw = traffic_rng_u32() % group->weight_total;
    for (unsigned int index = 0; index < group->entry_count; index++) {
        acc += g_traffic_paths.mix_entries[group->entry_base + index].weight;
        if (draw <= acc) return (int)(group->entry_base + index);
    }
    return -1;
}

/* FUN_001A5F90 @0x001A5F90: `rng % 100` against the eight percentage bytes. */
static int traffic_mix_pick_paint(const B3TrafficMixEntry* entry) {
    unsigned int draw = traffic_rng_u32() % 100u, acc = 0;
    for (int index = 0; index < 8; index++) {
        acc += entry->colours[index];
        if (draw <= acc) return index;
    }
    return 0;
}

/* One car of FUN_001A6070's spawn run: draw the class, the model inside it and
 * its paint, take the road's cruise speed with the +-15% jitter, then place the
 * agent.  FUN_001A2B20 @0x001A2BD8 matches the drawn packed id against the
 * manager's loaded set and stamps the index at body+0x176 -- so the MODEL is a
 * property of the request, which is exactly what the old seeder discarded. */
static int traffic_pool_seed_at(TrafficCar* t, unsigned int path_id,
                                float cursor, const B3TrafficMixRoad* road,
                                int reverse) {
    Vec3 point, tangent;
    float limit;
    if (!traffic_paths_active() || path_id >= g_traffic_paths.path_count)
        return 0;
    limit = (float)(g_traffic_paths.paths[path_id].count - 1);
    if (cursor < 0.0f) cursor = 0.0f;
    if (cursor > limit) cursor = limit;
    t->path_id = (unsigned short)path_id;
    t->path_cursor = cursor;
    /* FUN_001A2B20 @0x001A2CE1: agent+0x34 = 0.5 + (rng % 1000 - 500)*0.0001 */
    t->path_lateral = 0.5f
                    + ((float)(traffic_rng_u32() % 1000u) - 500.0f) * 0.0001f;
    if (road) {
        int entry = traffic_mix_pick_model(traffic_mix_pick_class(road));
        int car = entry >= 0 ? g_traffic_paths.mix_entry_car[entry] : -1;
        if (car >= 0) {
            t->car = car;
            t->paint = traffic_mix_pick_paint(
                &g_traffic_paths.mix_entries[entry]);
            t->trailer = -1;
            if (B3_TRAFFIC_CARS[car].cat == B3_TRAFFIC_CAT_TRACTOR) {
                /* class 4 always draws a class-0xB partner (FUN_001A6070
                 * @0x001A63A6: `cmp esi,4` -> `mov ecx,0xB`) */
                int towed = traffic_mix_pick_model(0x0b);
                int towed_car = towed >= 0
                              ? g_traffic_paths.mix_entry_car[towed] : -1;
                if (towed_car >= 0) {
                    t->trailer = towed_car;
                    (void)traffic_mix_pick_paint(
                        &g_traffic_paths.mix_entries[towed]);
                }
            }
            t->mass_kg = traffic_mass(B3_TRAFFIC_CARS[t->car].id);
            t->trailer_mass_kg = t->trailer >= 0
                ? traffic_mass(B3_TRAFFIC_CARS[t->trailer].id) : 0.0f;
        }
        /* FUN_001A6070 @0x001A64B1: mph * (1 + (2u-1)*0.15), 0.44704 mph->m/s */
        t->cruise_ms = road->speed_mph * 0.44704f
                     * (1.0f + (2.0f * traffic_rng_f() - 1.0f) * 0.15f);
        if (t->cruise_ms < 1.0f) t->cruise_ms = 0.0f;
    }
    traffic_path_sample(t->path_id, t->path_cursor, t->path_lateral,
                        &point, &tangent);
    t->pos = (Vec3){point.x, point.y + 0.5f, point.z};
    t->yaw = atan2f(tangent.x, -tangent.z);
    /* ONCOMING.  Every pool request carries a `direction` byte
     * ({first_row,last_row,path_id,direction}, FUN_001A28B0's TDESC window
     * table).  It was extracted but never applied, so every car drove its
     * path forwards and the game had NO oncoming traffic: over 172
     * player-vs-traffic contacts the closing speed was always pspd MINUS
     * tspd (player 159.5 - traffic 39.9 = 119.6), never the sum, so the
     * player could not reach FUN_001121F0's 150 mph gate and could not
     * crash into traffic at all (user report). */
    t->path_dir = (signed char)(reverse ? -1 : 1);
    if (reverse) t->yaw += 3.14159265f;
    route_project(t->pos.x, t->pos.z, 0, -1, &t->seg, &t->seg_t, NULL);
    traffic_reset_pose(t);
    return 1;
}

static int g_pool_trace_nowindow = 0;
static int g_pool_trace_acquire = 0, g_pool_trace_seedfail = 0, g_pool_trace_cover = 0;

static int traffic_pool_progress(const Vehicle* vehicle, unsigned int* progress) {
    unsigned int section = vehicle->nav_section;
    unsigned int node = vehicle->nav_node;
    if (!nav_state_valid(section, node)
        && !nav_nearest(vehicle->pos, &section, &node))
        return 0;
    *progress = g_nav.links[g_nav.sections[section].link_base + node].anchor;
    return 1;
}

static const char* g_pool_release_why = "?";
static void traffic_pool_release_slot(int slot) {
    TrafficCar* traffic;
    if (slot < 0 || slot >= g_traffic_n) return;
    traffic = &g_traffic[slot];
    if (getenv("B3_SPAWN_TRACE") && traffic->active) {
        float dx = traffic->pos.x - g_player.pos.x;
        float dz = traffic->pos.z - g_player.pos.z;
        float fx = sinf(g_player.rot.y), fz = -cosf(g_player.rot.y);
        float d = sqrtf(dx*dx + dz*dz);
        printf("[despawn] t=%.2f slot %d %s at %.0f m ahead %+.0f m "
               "dir %d cursor %.1f why %s\n",
               g_race_time, slot, B3_TRAFFIC_CARS[traffic->car].id,
               d, dx*fx + dz*fz, traffic->path_dir, traffic->path_cursor,
               g_pool_release_why);
    }
    if (traffic->pool_request >= 0 && traffic->pool_agent >= 0)
        (void)b3_traffic_pool_release(&g_traffic_pool, slot,
                                      traffic->pool_agent);
    traffic->active = 0;
    traffic->pool_request = -1;
    traffic->pool_owner = -1;
    traffic->pool_agent = -1;
    traffic->pool_seen = 0;
    traffic->trailer_ready = 0;
    traffic->trailer_linked = 0;
}

/* FUN_001A6610 @0x001A6610 -> FUN_001A4150 @0x001A4150: the manager refuses to
 * spawn into a row already claimed in DAT_00498D80's two-bits-per-row map.  The
 * harness keeps no separate map, so it tests the condition the map encodes --
 * a live agent already standing on that row of that path.  This is what stops
 * the clamped tail of a spawn run from stacking cars on one row. */
static int traffic_row_taken(unsigned int path_id, unsigned int row) {
    for (int slot = 0; slot < g_traffic_n; slot++) {
        const TrafficCar* other = &g_traffic[slot];
        if (other->active && other->path_id == path_id
            && (unsigned int)other->path_cursor == row)
            return 1;
    }
    return 0;
}

/* FUN_001A6070's per-car placement, @0x001A6240..0x001A6338:
 *     x   = uniform() * spacing              (spacing = span / n_f, in metres)
 *     row = low + i * x
 *     row = row + row * (2*uniform() - 1) * 0.30      (const @0x003B1750)
 *     row = clamp(row, low, high)
 * then the occupancy veto, then the class/model/paint draws. */
static void traffic_pool_place(int owner, unsigned int request_index, int seq,
                               const B3TrafficMixRoad* road,
                               unsigned int path_id, unsigned int low,
                               unsigned int high, float spacing, int index,
                               int reverse) {
    int physical_slot;
    int agent_slot;
    TrafficCar* traffic;
    float step, row;
    /* Occupancy is per (request, section, index) and carries no owning racecar:
     * FUN_001A3470 stamps the path-indexed table with no racer in the stamp at
     * all, so matching on the owner too would cull a car the instant the racer
     * that spawned it drove past its window while another racer still covered
     * the same request.  [C] */
    for (int slot = 0; slot < g_traffic_n; slot++) {
        traffic = &g_traffic[slot];
        if (traffic->active && traffic->pool_request == (int)request_index
            && traffic->pool_seq == seq) {
            traffic->pool_seen = 1;
            return;
        }
    }
    step = traffic_rng_f() * spacing;
    /* Seed from the end the agent will travel AWAY from: a request carries
     * {first_row,last_row,direction} and `low`/`high` are just their min and
     * max, so a REVERSE agent -- which walks its cursor DOWN and retires at
     * row 0 -- has to start at the high end.  Seeding it at `low` like a
     * forward agent made it retire on the very frame it spawned
     * (measured: "spawn slot 20 ... / despawn ... dir -1 cursor 0.0 why
     * path-end" in the same frame), which respawned it immediately: 6102
     * path-end retirements in 60 s, nearly all dir -1. */
    if (reverse) {
        row = (float)high - (float)index * step;
        row -= ((float)high - row) * (2.0f * traffic_rng_f() - 1.0f) * 0.30f;
    } else {
        row = (float)low + (float)index * step;
        row += row * (2.0f * traffic_rng_f() - 1.0f) * 0.30f;
    }
    if (row < (float)low) row = (float)low;
    if (row > (float)high) row = (float)high;
    if (traffic_row_taken(path_id, (unsigned int)row)) return;
    g_pool_trace_cover++;
    if (!b3_traffic_pool_acquire(&g_traffic_pool, &physical_slot, &agent_slot)
        || physical_slot < 0 || physical_slot >= g_traffic_n)
        return;
    g_pool_trace_acquire++;
    traffic = &g_traffic[physical_slot];
    /* Seed FIRST: traffic_pool_seed_at() ends in traffic_reset_pose(), which
     * clears the pool bookkeeping (see the note there).  Stamping the
     * reservation afterwards is what keeps it. */
    if (!traffic_pool_seed_at(traffic, path_id, row, road, reverse)) {
        g_pool_trace_seedfail++;
        (void)b3_traffic_pool_release(&g_traffic_pool, physical_slot,
                                      agent_slot);
        traffic->active = 0;
        return;
    }
    if (getenv("B3_SPAWN_TRACE")) {
        float dx = traffic->pos.x - g_player.pos.x;
        float dz = traffic->pos.z - g_player.pos.z;
        float fx = sinf(g_player.rot.y), fz = -cosf(g_player.rot.y);
        float d = sqrtf(dx*dx + dz*dz);
        printf("[spawn] t=%.2f slot %d %s at %.0f m  ahead %+.0f m\n",
               g_race_time, physical_slot,
               B3_TRAFFIC_CARS[traffic->car].id, d, dx*fx + dz*fz);
    }
    traffic->pool_request = (int)request_index;
    traffic->pool_seq = seq;
    traffic->pool_owner = owner;
    traffic->pool_agent = agent_slot;
    traffic->pool_seen = 1;
}

/* FUN_001A3470 @0x001A3470 walks a request's row range one manager-record
 * section at a time (FUN_0019E5B0/FUN_0019E640) and hands each section to
 * FUN_001A6070, whose population law is
 *     n_f = (span_metres / (mph * 0.44704)) * (1/60) * sum(rate[0..5])
 * -- the rate table is literally vehicles per minute, so a request is not one
 * car.  Constants: 0.44704 @0x003A5958, 1/60 @0x003B1838, 0.5 @0x003B1684. */
static void traffic_pool_assign_request(int owner, unsigned int request_index) {
    const B3TrafficPoolRequest* request;
    const B3TrafficPath* path;
    unsigned int low, high, row;
    int section = 0;
    if (request_index >= g_traffic_paths.pool_request_count) return;
    request = &g_traffic_paths.pool_requests[request_index];
    if (request->path_id >= g_traffic_paths.path_count) return;
    path = &g_traffic_paths.paths[request->path_id];
    low = request->first_row < request->last_row ? request->first_row
                                                 : request->last_row;
    high = request->first_row < request->last_row ? request->last_row
                                                  : request->first_row;
    if (high >= path->count) return;
    if (g_traffic_paths.mix_road_count == 0) {
        /* no extracted spawn policy for this track: one car per request, the
         * pre-TRAFFIC-MIX behaviour */
        traffic_pool_place(owner, request_index, 0, NULL, request->path_id,
                           low, high, (float)(high - low) + 1.0f, 0,
                           request->direction != 0);
        return;
    }
    for (row = low; ; section++) {
        const B3TrafficMixRoad* road = traffic_mix_road(request->path_id, row);
        int has_next = 0;
        unsigned int next = traffic_mix_next_start(request->path_id, row,
                                                   &has_next);
        unsigned int end = (has_next && next > 0 && next - 1 < high)
                         ? next - 1 : high;
        if (road) {
            float rate_sum = 0.0f;
            for (int cls = 0; cls < 6; cls++) rate_sum += road->rate[cls];
            float speed = road->speed_mph * 0.44704f;
            float span = fabsf(
                traffic_path_distance(request->path_id, (float)end)
                - traffic_path_distance(request->path_id, (float)row));
            if (rate_sum > 0.0f && speed > 0.0f && span > 0.0f) {
                float count_f = (span / speed) * (1.0f / 60.0f) * rate_sum;
                if (count_f >= 0.5f) {
                    float spacing = span / count_f;
                    int count = (int)(count_f + 0.5f);
                    for (int index = 0; index < count; index++)
                        traffic_pool_place(owner, request_index,
                                           section * 64 + index, road,
                                           request->path_id, row, end,
                                           spacing, index,
                                           request->direction != 0);
                }
            }
        }
        if (end >= high) break;
        row = end + 1;
    }
}

static void traffic_pool_refresh(void) {
    if (!traffic_paths_active() || g_traffic_paths.pool_window_count == 0
        || g_traffic_paths.pool_request_count == 0)
        return;
    for (int slot = 0; slot < g_traffic_n; slot++)
        g_traffic[slot].pool_seen = 0;
    for (int owner = 0; owner < g_num_vehicles; owner++) {
        const Vehicle* vehicle = &g_vehicles[owner];
        unsigned int progress;
        unsigned int current = g_traffic_paths.pool_window_count;
        if (!vehicle->active) continue;
        if (!traffic_pool_progress(vehicle, &progress)) {
            g_pool_trace_nowindow++; continue;
        }
        for (unsigned int window = 0;
             window < g_traffic_paths.pool_window_count; window++) {
            const B3TrafficPoolWindow* candidate =
                &g_traffic_paths.pool_windows[window];
            if (candidate->first_progress <= progress
                && progress <= candidate->last_progress) {
                current = window;
                break;
            }
        }
        if (current == g_traffic_paths.pool_window_count) {
            g_pool_trace_nowindow++; continue;
        }
        for (unsigned int offset = 0; offset < 3; offset++) {
            unsigned int window = (current + g_traffic_paths.pool_window_count
                                   - offset) % g_traffic_paths.pool_window_count;
            const B3TrafficPoolWindow* candidate =
                &g_traffic_paths.pool_windows[window];
            for (unsigned int index = candidate->request_count; index > 0; index--)
                traffic_pool_assign_request(owner,
                    candidate->request_base + index - 1);
        }
    }
    {   /* B3_POOL_TRACE=1: why traffic vanishes.  Counts the owners that
         * contributed no window this pass (either the nav progress query
         * failed or the progress sat in a gap between windows) beside the
         * releases that pass caused. */
        static int trace = -1;
        int released = 0, active_before = 0, active_after = 0;
        if (trace < 0) trace = getenv("B3_POOL_TRACE") != NULL;
        for (int slot = 0; slot < g_traffic_n; slot++)
            if (g_traffic[slot].active) active_before++;
        for (int slot = 0; slot < g_traffic_n; slot++) {
            TrafficCar* traffic = &g_traffic[slot];
            if (traffic->active && traffic->pool_request >= 0
                && !traffic->pool_seen) {
                traffic_pool_release_slot(slot);
                released++;
            }
        }
        if (trace) {
            for (int slot = 0; slot < g_traffic_n; slot++)
                if (g_traffic[slot].active) active_after++;
            if (released || (g_frame_count % 120) == 0)
                printf("[POOL] t=%.2f active %d->%d released %d "
                       "(owners no-window %d of %d)\n",
                       g_race_time, active_before, active_after, released,
                       g_pool_trace_nowindow, g_num_vehicles),
                printf("        newreq %d acquired %d seedfail %d\n",
                       g_pool_trace_cover, g_pool_trace_acquire,
                       g_pool_trace_seedfail);
        }
        g_pool_trace_nowindow = 0;
        g_pool_trace_acquire = g_pool_trace_seedfail = g_pool_trace_cover = 0;
    }
}

static void traffic_init(void) {
    if (getenv("B3_TRAFFIC") && atoi(getenv("B3_TRAFFIC")) == 0) {
        printf("[Burnout3] traffic disabled (B3_TRAFFIC=0)\n");
        return;
    }
    traffic_paths_load();
    // One display list per traffic car type; real .btv meshes + paint
    // (extract_traffic.py), box fallback where absent.
    char path[128];
    int meshes = 0;
    for (int c = 0; c < B3_TRAFFIC_CAR_COUNT; c++) {
        snprintf(path, sizeof(path), "build/cars/%s_%s.obj",
                 B3_TRAFFIC_CARS[c].cls, B3_TRAFFIC_CARS[c].car);
        TrackMesh m;
        if (trackmesh_load(&m, path) != 0) continue;
        g_traffic_ymin[c] = m.min[1];
        g_traffic_len[c] = m.max[2] - m.min[2];
        // Wheels sidecar (extract_traffic.py; format = the racer .wheels):
        // attach positions get the loader Z-flip like every mesh.
        {
            char wpath[160];
            snprintf(wpath, sizeof wpath, "build/cars/%s_%s.wheels",
                     B3_TRAFFIC_CARS[c].cls, B3_TRAFFIC_CARS[c].car);
            FILE* wf = fopen(wpath, "r");
            if (wf) {
                char line[160];
                int n = 0;
                while (fgets(line, sizeof line, wf)) {
                    float x, y, z; int mir;
                    if (line[0] == '#') continue;
                    if (sscanf(line, "radius %f", &x) == 1) {
                        g_traffic_wheel_radius[c] = x;
                    } else if (sscanf(line, "wheel %f %f %f %d",
                                      &x, &y, &z, &mir) == 4 && n < 6) {
                        g_traffic_wheel_pos[c][n][0] = x;
                        g_traffic_wheel_pos[c][n][1] = y;
                        g_traffic_wheel_pos[c][n][2] = -z;  // loader Z-flip
                        g_traffic_wheel_mirror[c][n] = mir;
                        n++;
                    }
                }
                fclose(wf);
                g_traffic_wheel_count[c] = n;
            }
        }
        snprintf(path, sizeof(path), "build/cars/%s_%s.hull",
                 B3_TRAFFIC_CARS[c].cls, B3_TRAFFIC_CARS[c].car);
        g_traffic_hull_ok[c] = b3_carcol_hull_load(path, &g_traffic_hull[c]);
        // CARFX: this traffic car's own corona light table. The .btv files
        // carry the same table the .bgv ones do (offsets model+0x1664+type*4,
        // counts model+0x16AC+type, 0x30-byte records) -- verified on all 40
        // shipped .btv files by tools/extract_traffic_lights.py, which writes
        // the sidecar. Traffic used to draw with no lights at all.
        b3_carfx_load_extra(c, B3_TRAFFIC_CARS[c].cls, B3_TRAFFIC_CARS[c].car);
        snprintf(path, sizeof(path), "build/cars/%s_%s_p0.png",
                 B3_TRAFFIC_CARS[c].cls, B3_TRAFFIC_CARS[c].car);
        GLuint tex = load_gl_texture(path, NULL);
        GLuint list = glGenLists(1);
        glNewList(list, GL_COMPILE);
        if (tex) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, tex);
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        glBegin(GL_TRIANGLES);
        for (int t = 0; t < m.triangle_count; t++) {
            const unsigned* idx = m.indices + (size_t)t * 3;
            const float* p0 = m.positions + (size_t)idx[0] * 3;
            const float* p1 = m.positions + (size_t)idx[1] * 3;
            const float* p2 = m.positions + (size_t)idx[2] * 3;
            float ux = p1[0]-p0[0], uy = p1[1]-p0[1], uz = p1[2]-p0[2];
            float vx = p2[0]-p0[0], vy = p2[1]-p0[1], vz = p2[2]-p0[2];
            float nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
            float len = sqrtf(nx*nx + ny*ny + nz*nz);
            float shade = 0.75f;
            if (len > 1e-9f)
                shade = 0.60f + 0.40f * fabsf(
                        (nx*0.30f + ny*0.85f + nz*0.42f) / len);
            glColor3f(shade, shade, shade);
            for (int k = 0; k < 3; k++) {
                unsigned vi = idx[k];
                if (tex && m.uvs)
                    glTexCoord2f(m.uvs[(size_t)vi * 2],
                                 m.uvs[(size_t)vi * 2 + 1]);
                glVertex3fv(m.positions + (size_t)vi * 3);
            }
        }
        glEnd();
        glDisable(GL_TEXTURE_2D);
        glEndList();
        g_traffic_lists[c] = list;
        // Wheel mesh list + the racer origin convention: with wheel data
        // the model origin is the hub plane, so the rest offset is the
        // wheel radius, not the body skirt (this was the "sinking").
        snprintf(path, sizeof(path), "build/cars/%s_%s_wheel.obj",
                 B3_TRAFFIC_CARS[c].cls, B3_TRAFFIC_CARS[c].car);
        g_traffic_wheel_lists[c] = car_list_from_obj(path, tex, 1, NULL);
        if (g_traffic_wheel_lists[c] && g_traffic_wheel_count[c] > 0
            && g_traffic_wheel_radius[c] > 0.05f)
            g_traffic_ymin[c] = -g_traffic_wheel_radius[c];
        meshes++;
    }

    // Trailer bogie centre, mesh-local .btv Z (the harness mesh loader
    // negates Z, so the sidecar values come back negated).
    for (int c = 0; c < B3_TRAFFIC_CAR_COUNT; c++) {
        if (B3_TRAFFIC_CARS[c].cat != B3_TRAFFIC_CAT_TRAILER) continue;
        float s = 0.0f; int nn = g_traffic_wheel_count[c];
        for (int w = 0; w < nn; w++) s += -g_traffic_wheel_pos[c][w][2];
        g_trailer_axle_z[c] = nn ? s / nn : -3.5f;
    }

    // The spawn/entry table is kept for diagnostics: its 32 records are what
    // the LANE table was recovered from (each entry is a lane seed, not a
    // free-standing spawn point -- projecting them onto the route collapses
    // them onto four lateral offsets with two directions).
    g_traffic_nslots = 0;
    for (int s = 0; s < B3_TRAFFIC_SPAWN_COUNT; s++) {
        const float* sp = B3_TRAFFIC_SPAWN[s];
        int seg = 0; float lat = 0.0f;
        route_project(sp[0], sp[2], 0, -1, &seg, NULL, &lat);
        (void)lat;
        g_traffic_slots[g_traffic_nslots++] = s;
    }

    // DRIVING POOL.  A slot-5 record is a TRAILER: the .btv has no front
    // axle (attach table = a rear bogie only), so it can never be a
    // free-standing traffic car -- that is what put a trailer on the road
    // "pulled by a sedan".  Trailers are pulled out of the pool here and
    // handed to the event's slot-4 tractor instead.  Specials keep their
    // old exclusion (their routing is their own system [?]).
    int pool_idx[B3_TRAFFIC_CAR_COUNT];
    int trailer_idx[B3_TRAFFIC_CAR_COUNT];
    int pool = 0, ntrailer = 0;
    for (int c = 0; c < B3_TRAFFIC_CAR_COUNT; c++) {
        int cat = B3_TRAFFIC_CARS[c].cat;
        if (cat == B3_TRAFFIC_CAT_TRAILER) trailer_idx[ntrailer++] = c;
        else if (cat != B3_TRAFFIC_CAT_SPECIAL) pool_idx[pool++] = c;
    }
    if (pool == 0) { pool_idx[pool++] = 0; }

    g_traffic_n = B3_TRAFFIC_N;
    b3_traffic_pool_init(&g_traffic_pool, g_traffic_n, g_traffic_n);
    int nt = 0;
    for (int i = 0; i < g_traffic_n; i++) {
        TrafficCar* t = &g_traffic[i];
        t->car = pool_idx[i % pool];
        t->mass_kg = traffic_mass(B3_TRAFFIC_CARS[t->car].id);
        t->trailer_mass_kg = 0.0f;
        t->trailer = -1;
        t->trailer_ready = 0;
        t->trailer_linked = 0;
        if (B3_TRAFFIC_CARS[t->car].cat == B3_TRAFFIC_CAT_TRACTOR
            && ntrailer > 0) {
            t->trailer = trailer_idx[nt++ % ntrailer];
            t->trailer_mass_kg =
                traffic_mass(B3_TRAFFIC_CARS[t->trailer].id);
            t->trailer_linked = 1;
        }
        t->pool_request = -1;
        t->pool_owner = -1;
        t->pool_agent = -1;
        t->pool_seen = 0;
        if (g_traffic_paths.pool_window_count == 0) {
            if (!traffic_path_seed(t, i % B3_TRAFFIC_SPAWN_COUNT))
                traffic_place(t, i, &g_player.pos,
                              30.0f + (float)i * 34.0f, +1);
        }
    }
    printf("[Burnout3] TRAFFIC: %d cars on %d recovered lanes (route %d pts,"
           " %d spawn-table lane seeds),\n           %d/%d real .btv meshes,"
           " pool %d + %d trailer(s) towed by the slot-4 tractor.\n"
           "           lanes:",
           g_traffic_n, B3_TRAFFIC_LANE_COUNT, B3_ONCOMING_COUNT,
           B3_TRAFFIC_SPAWN_COUNT, meshes, B3_TRAFFIC_CAR_COUNT,
           pool, ntrailer);
    for (int l = 0; l < B3_TRAFFIC_LANE_COUNT; l++)
        printf(" %+.2fm/%s", B3_TRAFFIC_LANES[l].lat,
               B3_TRAFFIC_LANES[l].dir > 0 ? "asc" : "desc");
    printf("\n           set:");
    for (int c = 0; c < B3_TRAFFIC_CAR_COUNT; c++)
        printf(" %s[%d]", B3_TRAFFIC_CARS[c].id, B3_TRAFFIC_CARS[c].cat);
    printf("\n           path follower: %s\n",
           traffic_paths_active() ? "RIDX pair/distance cursor" : "legacy lane fallback");
}

// Nearest active traffic car inside the forward cone (AI avoidance input).
static float traffic_nearest_ahead(const Vec3* pos, float yaw, float maxd) {
    float fx = sinf(yaw), fz = -cosf(yaw);
    float best = 1e9f;
    for (int i = 0; i < g_traffic_n; i++) {
        const TrafficCar* t = &g_traffic[i];
        if (!t->active) continue;
        float dx = t->pos.x - pos->x, dz = t->pos.z - pos->z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d < 0.1f || d > maxd) continue;
        if ((dx * fx + dz * fz) / d < 0.7f) continue;
        if (d < best) best = d;
    }
    return best;
}

/* ==================== AI AVOIDANCE (RE_AI section 15) ====================
 * Retail's rival avoidance is FUN_0016C450, called by the arbitrator
 * FUN_0016AAC0 @0x0016AADD with EAX = AI+0x2B0 (= racecar+0x1CB0), the
 * avoidance sub-object.  It rebuilds ONE FRAME IN THREE (the +0x4AF counter
 * at 0x0016C453) through FUN_0016D2F0 and then chooses with FUN_0016C4B0.
 *
 * THE PROFILE (FUN_0016D2F0 @0x0016D32F resets it, three parallel 256-entry
 * arrays) is a LATERAL histogram of the road at the car's route node:
 *     +0x140[i]  u8   time until strip i is occupied, x 8/255      0..8 s
 *     +0x240[i]  s16  range to the occupier,       x 1000/65536  +-500 m
 *     +0x040[i]  u8   what put it there (2..4 = a vehicle, 6 = road no-go)
 * pitch = 1/DAT_005A96EC = 1/5.0 = 0.2 m (DAT_003B1694, installed by the
 * static-init snippet 0x002C5AC0), index 128 = the node line -- the "+0x80"
 * bias at 0x0016E36C/0x0016E384.  +0x442 / +0x441 / +0x440 hold the band's
 * low index / high index / window width.                             [C]
 *
 * THE STAMPERS, in call order at 0x0016D3E9..0x0016D407:
 *     FUN_0016F6C0   road / no-go edges, using the four Soft+Hard No Go
 *                    offset params and the two "extra ... for future" ones
 *     FUN_0016EA40   the other RACECARS (DAT_0073A1A8[] table)
 *     FUN_0016EB60   the per-slot TRAFFIC list (0x649B36 counts,
 *                    0x6499F8 index table, 0x625FB0 + i*0x180 records)
 *     FUN_0016EC70   the physics-vehicle list DAT_00731E90[]
 * Each writes  time[i] = min(time[i], eta)  over the lateral strips the
 * obstacle sweeps, plus type[i] and dist[i] (0x0016E3D3..0x0016E41C).  [C]
 *
 * THE CHOOSER FUN_0016C4B0:
 *   tmin = min time over [lo,hi];  tmin >= 4.0 (DAT_003B1690) = no threat
 *   otherwise walk out from each edge while time >= running_best - 0.2
 *   (DAT_003A69B4) and take the run's MIDPOINT ((a+b)>>1 @0x0016C6C4 /
 *   0x0016C816); lateral offset = (idx - edge) * 0.2 (@0x0016CB84);
 *   risk = mean over `win` strips of (8.0 - time) (@0x0016C8AC), totals over
 *   the band with a time < 2.0 filter (@0x0016C929);
 *   aim = car_pos + road_fwd * max(5.0, 2.0*dmin/SteeringFactor)
 *                 + road_lat * offset                          (@0x0016CBED)
 *   speed avoid+0x488 = 26.2 / 40 / 60 for dmin < 10 / 20 / 30
 *   (@0x0016CCEF..0x0016CD48), else 16 / 30 when the type-2..4 risk
 *   fraction exceeds 0.95 / 0.9, else the corner-brake ceiling.       [C]
 *
 * THE ARBITRATION FUN_0016AAC0: ceiling = min(AI+0x1D0, AI+0x738)
 * (@0x0016AB16); risk at the car's own position above "Current risk
 * threshold" (corner variant when the target mode is 1/2) commits the
 * avoidance direction outright (@0x0016ABD9/@0x0016AC46 -> 0x0016AC4F), and
 * otherwise the side risk minus the chosen side's must clear "Risk
 * threshold" (@0x0016AD00 -> FUN_0016ADF0).                           [C]
 *
 * WHAT IS GLUE HERE.  Retail indexes the histogram off the .bgd nav graph
 * (road SECTION at racecar+0x18C4, node +0x18C8, the DAT_0073A174 edge-point
 * pool) which this harness does not load -- the same wall as RE_AI section
 * 12.  The lateral frame below is therefore built on the harness route
 * polyline, the road no-go comes from barrier rays on the REAL collision
 * mesh instead of FUN_0016F6C0's node offsets, and the obstacle sweep uses
 * the harness's own traffic/racer arrays.  Everything from the histogram
 * onward -- the 0.2 m pitch, the 4.0 s threat gate, the 0.2 s band slack,
 * the midpoint pick, the (8 - time) risk means, the 10/20/30 -> 26.2/40/60
 * speed map, the 0.9/0.95 risk speeds, the max(5, 2d/5.1) aim distance and
 * every arbitrator threshold -- is the recovered law with the real
 * registered parameter values.
 */
#define B3_AV_N      256
#define B3_AV_MID    128
#define B3_AV_PITCH  0.2f     /* 1/DAT_005A96EC (= DAT_003B1694 = 5.0)  [C] */
#define B3_AV_TMAX   8.0f     /* u8 0xFF * 8/255                        [C] */
#define B3_AV_DMAX   500.0f   /* s16 32767 * 1000/65536                 [C] */
#define B3_AV_THREAT 4.0f     /* DAT_003B1690                           [C] */
#define B3_AV_SLACK  0.2f     /* DAT_003A69B4                           [C] */
/* AI/Avoidance, group defaults -> Data/vdb.xml tune (RE_AI section 1) [C] */
#define B3_AV_LOOKAHEAD  20.0f   /* AVOID: LookAhead dist racecars           */
#define B3_AV_DISCARD_V   5.0f   /* Vert dist to discard traffic and racecars*/
#define B3_AV_DISCARD_D 100.0f   /* Distance to discard fatally colliding .. */
#define B3_AV_STEERF      5.1f   /* Steering factor big=>extreme             */
#define B3_AV_SPD_10     26.2f   /* Speed when car is <10m away              */
#define B3_AV_SPD_20     40.0f   /* Speed when car is <20m away              */
#define B3_AV_SPD_30     60.0f   /* Speed when car is <30m away              */
#define B3_AV_SPD_R95    16.0f   /* Speed when risk is >0.95                 */
#define B3_AV_SPD_R90    30.0f   /* Speed when risk is >0.9                  */
/* AI/Arbitrator thresholds (0x0047A164..0x0047A178)                    [C] */
#define B3_AV_RISK        1.0f
#define B3_AV_RISK_TOT    2.0f
#define B3_AV_RISK_CUR    5.0f
#define B3_AV_CRISK       0.5f
#define B3_AV_CRISK_TOT   1.0f
#define B3_AV_CRISK_CUR   4.0f

typedef struct {
    float time[B3_AV_N];
    float dist[B3_AV_N];
    unsigned char type[B3_AV_N];
    int   lo, hi, win;
    int   phase;               /* avoid+0x4AF: rebuild 1 frame in 3    [C] */
    int   ready;
    B3AiAvoidOut out;
} B3AiAvoidState;
static B3AiAvoidState g_ai_avoid[8];

static int ai_avoid_on(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("B3_AI_AVOID");
        v = (e && atoi(e) == 0) ? 0 : 1;    /* default ON; =0 = the old feed */
    }
    return v;
}

/* Stamp one moving occupier into the histogram.  This is FUN_0016E3D0's
 * loop: over the lateral strips the obstacle sweeps between now and its
 * arrival, keep the EARLIEST time and the SHORTEST range.               [C] */
static void ai_avoid_stamp(B3AiAvoidState* s, float lat_a, float lat_b,
                           float half, float eta, float range, int kind) {
    if (eta < 0.0f) eta = 0.0f;
    if (eta > B3_AV_TMAX) return;
    float l0 = (lat_a < lat_b ? lat_a : lat_b) - half;
    float l1 = (lat_a > lat_b ? lat_a : lat_b) + half;
    int i0 = B3_AV_MID + (int)floorf(l0 / B3_AV_PITCH);
    int i1 = B3_AV_MID + (int)ceilf(l1 / B3_AV_PITCH);
    if (i1 < 0 || i0 >= B3_AV_N) return;
    if (i0 < 0) i0 = 0;
    if (i1 >= B3_AV_N) i1 = B3_AV_N - 1;
    for (int i = i0; i <= i1; i++) {
        if (eta < s->time[i]) { s->time[i] = eta; s->type[i] = (unsigned char)kind; }
        /* range < 0 = "occupies the strip but is not something we close on"
         * (a car alongside).  It must still block the lateral pick, but it
         * is not a braking cue -- retail's range word is fed from the swept
         * ARRIVAL, not from a static separation. */
        if (range >= 0.0f && range < s->dist[i]) s->dist[i] = range;
    }
}

static void ai_avoid_build(Vehicle* v, int slot, B3AiAvoidState* s) {
    int n = g_track.num_points;
    if (n < 8) { s->ready = 0; return; }
    int idx = (int)(find_track_progress(v->pos) * n) % n;
    if (idx < 0) idx += n;
    Vec3 c  = g_track.points[idx];
    Vec3 fa = g_track.points[(idx + n - 2) % n];
    Vec3 fb = g_track.points[(idx + 2) % n];
    float fx = fb.x - fa.x, fz = fb.z - fa.z;
    float fl = sqrtf(fx * fx + fz * fz);
    if (fl < 1e-4f) { s->ready = 0; return; }
    fx /= fl; fz /= fl;
    float rx = -fz, rz = fx;                  /* harness right of forward   */
    float s0 = (v->pos.x - c.x) * rx + (v->pos.z - c.z) * rz;

    for (int i = 0; i < B3_AV_N; i++) {
        s->time[i] = B3_AV_TMAX;
        s->dist[i] = B3_AV_DMAX;
        s->type[i] = 0;
    }

    /* --- road no-go (FUN_0016F6C0's job).  GLUE routing on the game's own
     * barrier geometry: a strip whose look-ahead ray is blocked is type 6
     * with zero time -- retail's HARD no-go.  Coarse (1 m) because
     * aim_blocked is a grid walk; the rebuild is 1-in-3 frames anyway. */
    int b_lo = 0, b_hi = B3_AV_N - 1;
    {
        int have = 0, run_lo = 0, run_hi = 0;
        int c_lo = 0, c_hi = -1, in = 0;
        int s0i = B3_AV_MID + (int)(s0 / B3_AV_PITCH);
        for (int m = -20; m <= 20; m++) {
            float sl = (float)m;
            float tx = c.x + rx * sl + fx * B3_AV_LOOKAHEAD;
            float tz = c.z + rz * sl + fz * B3_AV_LOOKAHEAD;
            int blocked = aim_blocked(v->pos.x, v->pos.z, tx, tz,
                                      v->pos.y + 0.5f);
            int bi = B3_AV_MID + (int)(sl / B3_AV_PITCH);
            if (blocked) {
                int j0 = bi - 2, j1 = bi + 2;
                if (j0 < 0) j0 = 0;
                if (j1 >= B3_AV_N) j1 = B3_AV_N - 1;
                for (int j = j0; j <= j1; j++) {
                    s->time[j] = 0.0f;
                    s->type[j] = 6;
                }
                if (in) {
                    if (!have || (c_lo <= s0i && s0i <= c_hi)
                        || (c_hi - c_lo > run_hi - run_lo)) {
                        run_lo = c_lo; run_hi = c_hi; have = 1;
                    }
                    in = 0;
                }
            } else {
                if (!in) { c_lo = bi; in = 1; }
                c_hi = bi;
            }
        }
        if (in && (!have || (c_lo <= s0i && s0i <= c_hi)
                   || (c_hi - c_lo > run_hi - run_lo))) {
            run_lo = c_lo; run_hi = c_hi; have = 1;
        }
        if (have && run_hi - run_lo >= 10) { b_lo = run_lo; b_hi = run_hi; }
    }
    s->lo = b_lo;
    s->hi = b_hi;
    s->win = (b_hi - b_lo) / 8;
    if (s->win < 4) s->win = 4;

    /* --- the other RACECARS (FUN_0016EA40) --------------------------- */
    for (int i = 0; i < g_num_vehicles; i++) {
        const Vehicle* o = &g_vehicles[i];
        if (o == v || !o->active) continue;
        float dy = o->pos.y - v->pos.y;
        if (dy < -B3_AV_DISCARD_V || dy > B3_AV_DISCARD_V) continue;
        float dx = o->pos.x - v->pos.x, dz = o->pos.z - v->pos.z;
        float along = dx * fx + dz * fz;
        if (along < -4.0f || along > B3_AV_DISCARD_D) continue;
        float lat = dx * rx + dz * rz;
        float ovf = o->vel.x * fx + o->vel.z * fz;
        float ovr = o->vel.x * rx + o->vel.z * rz;
        float close = v->sim.speed - ovf;
        float half = 2.6f;
        int over = (along < 2.0f && fabsf(lat) < half);
        float eta = over ? 0.0f
                  : ((close > 0.5f && along > 0.0f) ? along / close
                                                    : B3_AV_TMAX);
        float rng = sqrtf(along * along + lat * lat);
        ai_avoid_stamp(s, lat, lat + ovr * eta, half, eta,
                       along > 2.0f ? rng : -1.0f, 2);
    }

    /* --- TRAFFIC (FUN_0016EB60 / FUN_0016EC70) ----------------------- */
    for (int i = 0; i < g_traffic_n; i++) {
        const TrafficCar* t = &g_traffic[i];
        if (!t->active) continue;
        float dy = t->pos.y - v->pos.y;
        if (dy < -B3_AV_DISCARD_V || dy > B3_AV_DISCARD_V) continue;
        float dx = t->pos.x - v->pos.x, dz = t->pos.z - v->pos.z;
        float along = dx * fx + dz * fz;
        if (along < -4.0f || along > B3_AV_DISCARD_D) continue;
        float lat = dx * rx + dz * rz;
        float tvx = sinf(t->yaw) * t->speed, tvz = -cosf(t->yaw) * t->speed;
        if (t->crashed_until > g_race_time) { tvx = 0.0f; tvz = 0.0f; }
        float ovf = tvx * fx + tvz * fz;
        float ovr = tvx * rx + tvz * rz;
        float close = v->sim.speed - ovf;      /* oncoming: closes fast */
        /* a rig is as wide as its trailer and much longer */
        float half = (t->trailer >= 0) ? 3.4f : 2.6f;
        int over = (along < 2.0f && fabsf(lat) < half);
        float eta = over ? 0.0f
                  : ((close > 0.5f && along > 0.0f) ? along / close
                                                    : B3_AV_TMAX);
        float rng = sqrtf(along * along + lat * lat);
        ai_avoid_stamp(s, lat, lat + ovr * eta, half, eta,
                       along > 2.0f ? rng : -1.0f, 3);
        if (t->trailer >= 0) {
            float tdx = t->tr_pos.x - v->pos.x, tdz = t->tr_pos.z - v->pos.z;
            float ta = tdx * fx + tdz * fz;
            if (ta > -4.0f && ta < B3_AV_DISCARD_D) {
                float tl = tdx * rx + tdz * rz;
                int to2 = (ta < 2.0f && fabsf(tl) < half);
                float te = to2 ? 0.0f
                         : ((close > 0.5f && ta > 0.0f) ? ta / close
                                                        : B3_AV_TMAX);
                float trg = sqrtf(ta * ta + tl * tl);
                ai_avoid_stamp(s, tl, tl + ovr * te, half, te,
                               ta > 2.0f ? trg : -1.0f, 3);
            }
        }
    }

    /* --- PROPS (the fourth stamper's job).  One nearest-prop probe on the
     * look-ahead point: the recovered verdict is that shipped props are
     * NON-CRASHABLE (props.h b3_props_object_class), so this only shapes the
     * line, it never has to save the car.  GLUE. */
    if (b3_props_ready()) {
        float pp[3] = { c.x + rx * s0 + fx * 15.0f, v->pos.y,
                        c.z + rz * s0 + fz * 15.0f };
        float pd = 1e9f;
        if (b3_props_nearest(pp, &pd) >= 0 && pd < 3.0f) {
            float eta = 15.0f / (v->sim.speed > 1.0f ? v->sim.speed : 1.0f);
            ai_avoid_stamp(s, s0, s0, 1.5f, eta, 15.0f, 4);
        }
    }
    s->ready = 1;
    (void)slot;
}

static void ai_avoid_choose(Vehicle* v, B3AiAvoidState* s, int corner,
                            float fx, float fz, float rx, float rz, float s0)
{
    B3AiAvoidOut* o = &s->out;
    memset(o, 0, sizeof *o);
    o->valid = 1;
    o->speed = 1e9f;
    o->dmin = B3_AV_DMAX;
    int lo = s->lo, hi = s->hi;

    float tmin = B3_AV_TMAX;
    for (int i = lo; i <= hi; i++) if (s->time[i] < tmin) tmin = s->time[i];

    int s0i = B3_AV_MID + (int)(s0 / B3_AV_PITCH);
    if (s0i < 0) s0i = 0;
    if (s0i >= B3_AV_N) s0i = B3_AV_N - 1;
    o->risk_here = B3_AV_TMAX - s->time[s0i];

    /* dmin over the middle half of the band, skipping the road no-go
     * strips -- FUN_0016C4B0 @0x0016CC79 (q = (hi-lo)/4). */
    {
        int q = (hi - lo) / 4;
        for (int i = lo + q; i < hi - q; i++)
            if (s->type[i] != 6 && s->dist[i] < o->dmin) o->dmin = s->dist[i];
    }

    if (tmin >= B3_AV_THREAT) {          /* @0x0016C5F8: nothing to dodge */
        o->state = 0x10;
        o->override = 0;
    } else {
        /* widest band whose time stays within 0.2 s of its best -- the
         * midpoint of that run is the target strip (@0x0016C6C4). */
        int best = s0i < lo ? lo : (s0i > hi ? hi : s0i);
        for (int i = lo; i <= hi; i++) {
            if (s->time[i] > s->time[best]
                || (s->time[i] == s->time[best]
                    && abs(i - s0i) < abs(best - s0i)))
                best = i;
        }
        float thr = s->time[best] - B3_AV_SLACK;
        if (thr < 0.0f) thr = 0.0f;
        int a = best, b = best;
        while (a > lo && s->time[a - 1] >= thr) a--;
        while (b < hi && s->time[b + 1] >= thr) b++;
        int tgt = (a + b) / 2;

        float rl = 0.0f, rh = 0.0f;
        for (int k = 1; k <= s->win; k++) {
            int i = tgt - k; if (i < 0) i = 0;
            rl += B3_AV_TMAX - s->time[i];
            i = tgt + k; if (i >= B3_AV_N) i = B3_AV_N - 1;
            rh += B3_AV_TMAX - s->time[i];
        }
        o->risk_lo = rl / (float)s->win;
        o->risk_hi = rh / (float)s->win;
        o->state = (o->risk_lo <= o->risk_hi) ? 1 : 2;

        /* the aim point, FUN_0016C4B0 @0x0016CBED */
        float lead = 2.0f * o->dmin / B3_AV_STEERF;
        if (lead < 5.0f) lead = 5.0f;          /* FUN_000198E0(5.0, x)  [C] */
        if (lead > 40.0f) lead = 40.0f;
        float off = (float)(tgt - s0i) * B3_AV_PITCH;
        o->aim[0] = v->pos.x + fx * lead + rx * (s0 + off);
        o->aim[1] = v->pos.y;
        o->aim[2] = v->pos.z + fz * lead + rz * (s0 + off);
        /* the same aim must not cross a barrier (retail's no-go strips
         * already guarantee this; the harness ray test is the stand-in). */
        if (aim_blocked(v->pos.x, v->pos.z, o->aim[0], o->aim[2],
                        v->pos.y + 0.5f)) {
            o->aim[0] = v->pos.x + fx * lead;
            o->aim[2] = v->pos.z + fz * lead;
        }

    }
    (void)corner;

    /* the avoidance speed avoid+0x488 (@0x0016CCEF..0x0016CD48) */
    if      (o->dmin < 10.0f) o->speed = B3_AV_SPD_10;
    else if (o->dmin < 20.0f) o->speed = B3_AV_SPD_20;
    else if (o->dmin < 30.0f) o->speed = B3_AV_SPD_30;
    else {
        float frac = 0.0f;
        int cnt = 0;
        for (int i = lo; i <= hi; i++)
            if (s->type[i] >= 2 && s->type[i] <= 4) {
                frac += B3_AV_TMAX - s->time[i];
                cnt++;
            }
        frac = (hi > lo) ? frac / ((float)(hi - lo + 1) * B3_AV_TMAX) : 0.0f;
        (void)cnt;
        if      (frac > 0.95f) o->speed = B3_AV_SPD_R95;
        else if (frac > 0.9f)  o->speed = B3_AV_SPD_R90;
    }
}

static void ai_avoid_update(Vehicle* v, int slot, int corner,
                            Vec3* target, float* ceiling, B3AiAvoidOut* out)
{
    memset(out, 0, sizeof *out);
    if (slot < 0 || slot >= 8) return;
    B3AiAvoidState* s = &g_ai_avoid[slot];

    int n = g_track.num_points;
    if (n < 8) return;
    int idx = (int)(find_track_progress(v->pos) * n) % n;
    if (idx < 0) idx += n;
    Vec3 c  = g_track.points[idx];
    Vec3 fa = g_track.points[(idx + n - 2) % n];
    Vec3 fb = g_track.points[(idx + 2) % n];
    float fx = fb.x - fa.x, fz = fb.z - fa.z;
    float fl = sqrtf(fx * fx + fz * fz);
    if (fl < 1e-4f) return;
    fx /= fl; fz /= fl;
    float rx = -fz, rz = fx;
    float s0 = (v->pos.x - c.x) * rx + (v->pos.z - c.z) * rz;

    /* avoid+0x4AF: the whole stage runs one frame in three [C @0x0016C453] */
    if (++s->phase >= 3) {
        s->phase = 0;
        ai_avoid_build(v, slot, s);
        if (s->ready) ai_avoid_choose(v, s, corner, fx, fz, rx, rz, s0);
    }
    if (!s->ready || !s->out.valid) goto trace;
    *out = s->out;

    /* THE ARBITRATOR'S TWO RISK TESTS -- FUN_0016AAC0.  These run EVERY
     * frame (only FUN_0016C450's profile rebuild is 1-in-3), and the second
     * one is evaluated at the TRACKED AIM POINT (`FUN_0016FB50(AI+0x200)`
     * @0x0016AC81), not at the car:
     *     r_here > Current(corner) risk threshold        -> @0x0016AC4F
     *     r_aim - side_risk > (corner) Risk threshold    -> FUN_0016ADF0
     * both commit the avoidance direction AI+0x720.                    [C] */
    if (out->state != 0x10) {
        float tl = (target->x - c.x) * rx + (target->z - c.z) * rz;
        int ai = B3_AV_MID + (int)(tl / B3_AV_PITCH);
        if (ai < 0) ai = 0;
        if (ai >= B3_AV_N) ai = B3_AV_N - 1;
        float r_aim = B3_AV_TMAX - s->time[ai];
        float cur  = corner ? B3_AV_CRISK_CUR : B3_AV_RISK_CUR;
        float side = corner ? B3_AV_CRISK     : B3_AV_RISK;
        float chosen = (out->state == 1) ? out->risk_lo : out->risk_hi;
        out->override = (out->risk_here > cur)
                     || ((r_aim - chosen) > side);
    }
    /* B3_AI_AVOID=0 keeps the profile (so the telemetry stays comparable)
     * but restores the pre-port feed: no ceiling cap, no aim override, no
     * band clamp.  That switch is how the "before" column of the crash
     * table in the integration note was measured. */
    if (!ai_avoid_on()) goto trace;

    /* AI+0x780 = min(corner brake, avoidance)  [C @0x0016AB16] */
    if (out->speed < *ceiling) *ceiling = out->speed;

    /* commit the avoidance direction (FUN_0016ADF0) */
    if (out->override) {
        target->x = out->aim[0];
        target->z = out->aim[2];
    } else {
        /* Otherwise keep the racing/aggression aim, but hold it inside the
         * band -- retail's HARD no-go does this structurally (a strip off
         * the road simply never wins the midpoint pick).  Without it the
         * harness's own barrier-fallback offsets (+-4..16 m) and the
         * wall-grind escape (+-8 m) push a rival clean across the road into
         * the ONCOMING lanes, which is the reported behaviour. */
        float tl = (target->x - c.x) * rx + (target->z - c.z) * rz;
        float blo = (float)(s->lo - B3_AV_MID) * B3_AV_PITCH;
        float bhi = (float)(s->hi - B3_AV_MID) * B3_AV_PITCH;
        float cl = tl;
        if (cl < blo) cl = blo;
        if (cl > bhi) cl = bhi;
        if (cl != tl) {
            target->x += rx * (cl - tl);
            target->z += rz * (cl - tl);
        }
    }
trace:
    if (getenv("B3_AI_AVOID_TRACE")) {
        /* rlat = the SAME signed lateral coordinate the traffic lane table
         * is expressed in (route_project's s = tx*(z-cz) - tz*(x-cx)), so a
         * rival's rlat can be read straight against B3_TRAFFIC_LANES: on
         * US_C3_V1 the with-race lanes are +2.66 / +9.07 and the ONCOMING
         * lanes +14.56 / +20.83, so rlat > ~11.8 means the rival has
         * crossed into the oncoming half. */
        int rs = 0; float rt = 0.0f, rlat = 0.0f;
        route_project(v->pos.x, v->pos.z, 0, -1, &rs, &rt, &rlat);
        printf("[avoid] t=%6.2f car%d st%d ovr%d band[%d,%d] lat%+6.2f "
               "rlat%+7.2f here%.2f lo%.2f hi%.2f dmin%6.1f spd%6.1f\n",
               g_race_time, slot, out->state, out->override, s->lo, s->hi,
               s0, rlat, out->risk_here, out->risk_lo, out->risk_hi,
               out->dmin, out->speed);
    }
}

// Update the trailer articulation of a towed rig.  One-axle trailer
// kinematics: the bogie centre chases the fifth wheel, the body lies along
// the line between them. Hitch geometry is the model's own fifth-wheel
// (`+0x16A8`) and kingpin (`+0x16A4`) records; in harness mesh space forward
// is -Z (the loader negates Z), so their raw .btv Z values run along the
// car's forward axis.
static void traffic_trailer_update(TrafficCar* t) {
    if (t->trailer < 0) return;
    if (t->trailer_ready) {
        t->tr_pos = (Vec3){t->trailer_rb.frame[3][0],
                           t->trailer_rb.frame[3][1] + 0.5f
                               + g_traffic_ymin[t->trailer],
                          -t->trailer_rb.frame[3][2]};
        t->tr_yaw = atan2f(t->trailer_rb.frame[2][0],
                            t->trailer_rb.frame[2][2]);
        return;
    }
    float fx = sinf(t->yaw), fz = -cosf(t->yaw);
    float towz = B3_TRAFFIC_CARS[t->car].tow_anchor[2];
    float kz   = B3_TRAFFIC_CARS[t->trailer].king_anchor[2];
    float az   = g_trailer_axle_z[t->trailer];       // negative = bogie
    float L    = kz - az;                            // kingpin -> bogie
    if (L < 1.0f) L = 1.0f;
    float hx = t->pos.x + fx * towz, hz = t->pos.z + fz * towz;
    float ux = hx - t->tr_bogie.x, uz = hz - t->tr_bogie.z;
    float ul = sqrtf(ux * ux + uz * uz);
    if (ul < 1e-3f) { ux = fx; uz = fz; ul = 1.0f; }
    ux /= ul; uz /= ul;
    t->tr_bogie.x = hx - ux * L;
    t->tr_bogie.z = hz - uz * L;
    t->tr_bogie.y = t->pos.y;
    t->tr_yaw = atan2f(ux, -uz);
    t->tr_pos.x = hx - ux * kz;
    t->tr_pos.z = hz - uz * kz;
    t->tr_pos.y = t->pos.y;
}

static void traffic_anchor_world(const B3RigidBody* rb, const float local[3],
                                 float out[4]) {
    for (int axis = 0; axis < 3; axis++)
        out[axis] = rb->frame[3][axis] + rb->frame[0][axis] * local[0]
                  + rb->frame[1][axis] * local[1]
                  + rb->frame[2][axis] * local[2];
    out[3] = 0.0f;
}

static void traffic_force_at(B3RigidBody* rb, const float force[4],
                             const float point[4]) {
    float rx = point[0] - rb->frame[3][0];
    float ry = point[1] - rb->frame[3][1];
    float rz = point[2] - rb->frame[3][2];
    for (int axis = 0; axis < 4; axis++) rb->force_acc[axis] += force[axis];
    rb->torque_acc[0] += ry * force[2] - rz * force[1];
    rb->torque_acc[1] += rz * force[0] - rx * force[2];
    rb->torque_acc[2] += rx * force[1] - ry * force[0];
}

static void traffic_remove_angmom_axis(B3RigidBody* rb, int axis,
                                       float scale) {
    float projected = rb->frame[axis][0] * rb->angmom[0]
                    + rb->frame[axis][1] * rb->angmom[1]
                    + rb->frame[axis][2] * rb->angmom[2];
    for (int component = 0; component < 3; component++)
        rb->angmom[component] -= rb->frame[axis][component] * projected * scale;
}

/* FUN_00120F30's articulated-body constraint: point velocities at the
 * trailer kingpin and tractor fifth wheel feed FUN_0010F8D0. Its returned
 * normal vector is written with opposite signs into the two +0x130
 * deflection accumulators; it is not a point impulse at +0x110/+0x120. */
static void traffic_tow_constraint(TrafficCar* t, float dt) {
    if (t->trailer < 0 || !t->trailer_ready || !t->trailer_linked
        || t->asleep) return;
    float master_pt[4], trailer_pt[4], master_v[4], trailer_v[4];
    float delta[4], normal[4], impulse[4] = {0, 0, 0, 0};
    traffic_anchor_world(&t->rb, B3_TRAFFIC_CARS[t->car].tow_anchor,
                         master_pt);
    traffic_anchor_world(&t->trailer_rb,
                         B3_TRAFFIC_CARS[t->trailer].king_anchor, trailer_pt);
    float d2 = 0.0f;
    for (int axis = 0; axis < 3; axis++) {
        delta[axis] = trailer_pt[axis] - master_pt[axis];
        d2 += delta[axis] * delta[axis];
    }
    if (d2 <= 2.3283064e-10f) return;
    float inv_d = 1.0f / sqrtf(d2);
    for (int axis = 0; axis < 3; axis++) normal[axis] = delta[axis] * inv_d;
    normal[3] = 0.0f;

    b3_carcol_point_velocity(&t->rb, master_pt, master_v);
    b3_carcol_point_velocity(&t->trailer_rb, trailer_pt, trailer_v);
    if (B3_TRAFFIC_CARS[t->trailer].kingpin_spring && dt > 1e-6f) {
        float dy = trailer_pt[1] - (master_pt[1] + 0.5f);
        float force[4] = {0.0f,
                          trailer_v[1] / dt * -1000.0f
                              + (dy - 0.3f) * -80000.0f,
                          0.0f, 0.0f};
        traffic_force_at(&t->trailer_rb, force, trailer_pt);
    }
    float vrel[4];
    for (int axis = 0; axis < 4; axis++)
        vrel[axis] = master_v[axis] - trailer_v[axis];
    float j = b3_carcol_mutual_impulse(
        &t->trailer_rb, t->trailer_mass_kg, &t->rb, t->mass_kg,
        master_pt, trailer_pt, vrel, normal, 0.0f, impulse);
    if (j > 0.0f) {
        for (int axis = 0; axis < 4; axis++) {
            t->trailer_rb.deflection[axis] += impulse[axis];
            t->rb.deflection[axis] -= impulse[axis];
        }
    }
    float forward_dot = t->rb.frame[2][0] * t->trailer_rb.frame[2][0]
                      + t->rb.frame[2][1] * t->trailer_rb.frame[2][1]
                      + t->rb.frame[2][2] * t->trailer_rb.frame[2][2];
    if (forward_dot < -0.17364818f) {
        traffic_remove_angmom_axis(&t->rb, 1, 2.0f);
        traffic_remove_angmom_axis(&t->trailer_rb, 1, 2.0f);
    }
    float up_dot = t->rb.frame[1][0] * t->trailer_rb.frame[1][0]
                 + t->rb.frame[1][1] * t->trailer_rb.frame[1][1]
                 + t->rb.frame[1][2] * t->trailer_rb.frame[1][2];
    if (up_dot < 0.98480775f) {
        traffic_remove_angmom_axis(&t->rb, 0, 1.5f);
        traffic_remove_angmom_axis(&t->trailer_rb, 0, 1.5f);
        traffic_remove_angmom_axis(&t->rb, 2, 1.5f);
        traffic_remove_angmom_axis(&t->trailer_rb, 2, 1.5f);
    }
    if (sqrtf(d2) > 1.0f || forward_dot < -0.5f) {
        t->trailer_linked = 0;
        t->asleep = 0;
    }
}

// FUN_0019F1C0 advances the retail road agent's persistent path cursor by
// speed * frame time, then FUN_0019FFA0 turns that cursor into its target
// transform through the recovered clamped four-knot sampler.
static void traffic_update(float dt) {
    traffic_pool_refresh();
    traffic_reservations_rebuild();
    for (int i = 0; i < g_traffic_n; i++) {
        TrafficCar* t = &g_traffic[i];
        if (!t->active) continue;
        Vec3 body_pos = {t->rb.frame[3][0],
                         t->rb.frame[3][1] + 0.5f + g_traffic_ymin[t->car],
                        -t->rb.frame[3][2]};
        int raw_path = traffic_paths_active();
        if (t->crashed_until > 0.0f) {
            if (g_race_time < t->crashed_until) {
                traffic_trailer_update(t);
                continue;                                   // parked wreck
            }
            if (raw_path) {
                if (t->pool_request >= 0) {
                    /* retail retires the body (FUN_001A41A0) and lets the next
                     * FUN_001A28B0 pass re-fill the request with a freshly
                     * drawn class/model/paint -- it never re-seeds in place */
                    g_pool_release_why = "raw-path-reseed";
                    traffic_pool_release_slot(i);
                    continue;
                }
                if (!traffic_path_seed(t, t->spawn + 1 + i))
                    traffic_place(t, t->lane + 1, &g_player.pos, 320.0f, +1);
            } else {
                traffic_place(t, t->lane + 1, &g_player.pos, 320.0f, +1);
            }
        }

        // Keep the public road position only for diagnostics.  The retail
        // agent never reprojects this from its body every frame: +0x30 is a
        // persistent distance cursor, so a contact cannot snap it across a
        // nearby fold in the loop.
        int seg = t->seg;
        float u = t->seg_t, lat = 0.0f;
        if (!raw_path)
            route_project(body_pos.x, body_pos.z, seg, 24, NULL, NULL, &lat);
        float lat_err = raw_path ? 0.0f : lat - t->lane_lat;
        if (fabsf(lat_err) > t->max_lat_err) t->max_lat_err = fabsf(lat_err);

        // ================= SPEED: the retail traffic agent law ============
        // Ported from FUN_0019F560, the per-frame update of the game's own
        // traffic road agents (the 0x50-byte S records at 0x0063DCB0, driven
        // by FUN_001A20F0).  That -- not FUN_00105150, which is the AI-RIVAL
        // RACER's input generator (vtable 0x3B1240 slot +0x64) -- is what
        // moves traffic in retail.  Constants [C]:
        //   K   = 1/6.5   = 0.15384616  approach gain, PER FRAME
        //   dv  in [-0.1, +0.08] m/s    cruise ramp clamp, PER FRAME
        //                               (= -6.0 / +4.8 m/s^2 at 60 Hz)
        //   R   = 35.0 m (30.0 in dense traffic)  racecar detect radius
        //   the car-ahead gap subtracts both half-lengths and 2.5 m
        // The per-frame numbers are converted to per-second here and scaled
        // by dt, so they hold at any harness step.
        const float K_PS   = (1.0f / 6.5f) * 60.0f;   // approach gain / s
        const float RAMP_UP = 0.08f * 60.0f;          // +4.8 m/s^2
        const float RAMP_DN = 0.10f * 60.0f;          // -6.0 m/s^2
        const float AVOID_R = 35.0f;
        float myhalf = 0.5f * (g_traffic_len[t->car] > 2.0f
                               ? g_traffic_len[t->car] : 4.2f);
        if (t->trailer >= 0) myhalf += 0.5f * g_traffic_len[t->trailer];
        // brakeTriggerDist (S+0x14) is filled from track data we do not have;
        // a stopping distance at the recovered brake ramp stands in (GLUE).
        float trigger = t->speed * t->speed / (2.0f * RAMP_DN) + 6.0f;
        float fxq = sinf(t->yaw), fzq = -cosf(t->yaw);
        int   state = 0;                 // 0 cruise, 3 follow, 4 avoid
        float acc = 0.0f;

        // (1) avoid the player / AI racecars ahead.
        // Retail's avoid (FUN_0019F560, fresh-trigger 0x0019f726-0x0019f77d):
        // gap = max(d - (their_half + my_half + 2.0), 0) -- a GEOMETRIC gap
        // (their body+0xE88 half-length, agent+0x08 my half-length, 2.0 m).
        // The avoid gate is speed > gap/2 (COMISS speed, gap*0.5; JBE skip).
        // [C] on the gap + gate.
        // The avoid ACC is retail's persisted nudge: at the fresh trigger
        // state=4 and FUN_0019FEC0 stores a traffic-LCG random (~[-0.15,0.12]
        // m/s/frame) into agent+0x1C; state 4 (JT[4]=0x0019fd78) reuses that
        // saved value each frame. That is a global-seed RNG, so the harness
        // uses a deterministic drive-down toward gap/2 as a GLUE stand-in.
        for (int j = 0; j < g_num_vehicles; j++) {
            const Vehicle* rv = &g_vehicles[j];
            if (!rv->active) continue;
            float qx = rv->pos.x - t->pos.x, qz = rv->pos.z - t->pos.z;
            float d = sqrtf(qx * qx + qz * qz);
            if (d >= AVOID_R || qx * fxq + qz * fzq <= 0.0f) continue;
            float rhalf = rv->body_len > 2.0f ? rv->body_len * 0.5f : 2.2f;
            float gap = d - (rhalf + myhalf + 2.0f);
            if (gap < 0.0f) gap = 0.0f;
            // gate [C]: speed > gap/2, AND (state==1, or latch<0.5 when not).
            if (gap * 0.5f < t->speed
                && (t->brake_latch < 0.5f || state == 1)) {
                /* FUN_0019FEC0 stores ONE traffic-LCG draw in agent+0x1C at
                 * the fresh trigger and state 4 (JT[4]=0x0019FD78) reuses
                 * that same value every frame: a persistent NUDGE in
                 * [-0.15, +0.12] m/s per frame, not a servo.  The old
                 * stand-in drove the speed at (gap*0.8 - speed) * K_PS,
                 * which parks a car at a standstill whenever the player is
                 * alongside -- and a stopped car becomes the leader for the
                 * follow law behind it, which is how one avoid seeded a
                 * whole jam.  The draw uses the recovered traffic RNG, so
                 * the distribution is retail's even though the sequence
                 * cannot be (its seed is global). */
                if (!t->avoid_active) {
                    float u = traffic_rng_f();          /* [0,1) */
                    t->avoid_nudge = (-0.15f + u * 0.27f) * 60.0f;
                    t->avoid_active = 1;
                }
                state = 4;
                acc = t->avoid_nudge;
            }
        }
        if (state != 4) t->avoid_active = 0;

        // (2) follow the traffic car ahead in the same lane
        {
            const TrafficCar* ah = NULL;
            float ahd = 1e9f;
            float path_distance = raw_path
                                ? traffic_path_distance(t->path_id,
                                                        t->path_cursor)
                                : 0.0f;
            if (raw_path && t->reservation_ahead >= 0
                && t->reservation_ahead < g_traffic_n) {
                const TrafficCar* o = &g_traffic[t->reservation_ahead];
                float d = traffic_path_distance(o->path_id, o->path_cursor)
                        - path_distance;
                if (o->active && o->path_id == t->path_id && d > 0.0f) {
                    ah = o;
                    ahd = d;
                }
            }
            if (!raw_path) for (int j = 0; j < g_traffic_n; j++) {
                const TrafficCar* o = &g_traffic[j];
                if (o == t || !o->active || o->lane != t->lane) continue;
                float d;
                {
                    float qx = o->pos.x - t->pos.x, qz = o->pos.z - t->pos.z;
                    float along = qx * fxq + qz * fzq;
                    if (along <= 0.0f) continue;
                    float side = fabsf(qx * fzq - qz * fxq);
                    if (side > 3.2f) continue;
                    d = sqrtf(qx * qx + qz * qz);
                }
                if (d < ahd) { ahd = d; ah = o; }
            }
            if (ah) {
                float ohalf = 0.5f * (g_traffic_len[ah->car] > 2.0f
                                      ? g_traffic_len[ah->car] : 4.2f);
                if (ah->trailer >= 0)
                    ohalf += 0.5f * g_traffic_len[ah->trailer];
                float gap = ahd - (myhalf + ohalf) - 2.5f;
                if (gap < 0.0f) gap = 0.0f;
                if (gap < trigger) {
                    float vT = gap * 0.8f;
                    float v2 = (gap / trigger) * ah->speed;
                    if (v2 > vT) vT = v2;
                    if (vT < t->speed
                        && (t->brake_latch <= 0.0f || gap < t->brake_latch)) {
                        t->brake_latch = gap;
                        state = 3;
                        acc = (vT - t->speed) * K_PS;
                    }
                } else {
                    t->brake_latch = 0.0f;       // gap opened: release
                }
            } else {
                t->brake_latch = 0.0f;
            }
        }
        // (4) resolve the state into this frame's speed change
        // The cruise target is this car's own road speed (TDESC stage-2 mph
        // with FUN_001A6070's +-15% jitter), not one global 50 mph. [C]
        float cruise = t->cruise_ms > 1.0f ? t->cruise_ms
                                           : B3_TRAFFIC_SPEED_MS;
        if (state == 0) {
            /* The dv clamp is the CRUISE ramp -- retail's own note calls it
             * that, and the avoid nudge below is drawn from a wider range
             * ([-0.15,+0.12] m/s/frame) than the clamp allows, which only
             * makes sense if the clamp does not gate it.  So it stays on
             * this branch; the follow servo's steep instantaneous rate is
             * an exponential approach (dv = (vT - speed)/6.5 per frame),
             * not a sustained 6.5 g, and is retail's. */
            acc = (cruise - t->speed) / dt;                // cruise error
            if (acc >  RAMP_UP) acc =  RAMP_UP;
            if (acc < -RAMP_DN) acc = -RAMP_DN;
        }
        if (getenv("B3_FOLLOW_TRACE") && t->speed < 8.0f && t->active) {
            static float next_dbg = 0.0f;
            if (g_race_time >= next_dbg) {
                next_dbg = g_race_time + 0.25f;
                printf("[follow] t=%.1f slot %d spd %.1f cruise %.1f state %d "
                       "acc %+.1f latch %.2f trigger %.1f ahead %d\n",
                       g_race_time, i, t->speed, cruise, state, acc,
                       t->brake_latch, trigger, t->reservation_ahead);
            }
        }
        t->speed += acc * dt;
        if (t->speed < 0.0f) t->speed = 0.0f;
        if (t->speed > cruise * 1.05f)
            t->speed = cruise * 1.05f;

        Vec3 lp, tangent;
        if (raw_path) {
            if (traffic_path_advance(t, t->speed * dt)) {
                if (t->pool_request >= 0) {
                    /* FUN_0019F1C0 retires an agent at the descriptor end */
                    g_pool_release_why = "path-end";
                    traffic_pool_release_slot(i);
                    continue;
                }
                if (traffic_path_seed(t, t->spawn + 1 + i)) continue;
                raw_path = 0;
            }
            if (raw_path) {
                traffic_path_sample(t->path_id, t->path_cursor,
                                    t->path_lateral, &lp, &tangent);
                t->pos = (Vec3){lp.x, lp.y + 0.5f, lp.z};
                t->yaw = atan2f(tangent.x, -tangent.z);
                if (t->path_dir < 0) t->yaw += 3.14159265f;
                traffic_reservations_rebuild();
            }
        }
        if (!raw_path) {
            route_advance(seg, u, t->lane_dir, t->speed * dt, &seg, &u);
            t->seg = seg;
            t->seg_t = u;
            lane_point(seg, u, t->lane_lat, &lp);
            int aseg; float at2;
            route_advance(seg, u, t->lane_dir, 1.0f, &aseg, &at2);
            Vec3 ahead;
            lane_point(aseg, at2, t->lane_lat, &ahead);
            t->pos = (Vec3){lp.x, lp.y + 0.5f, lp.z};
            t->yaw = atan2f(ahead.x - lp.x, -(ahead.z - lp.z));
        }
        int body_ready = traffic_stream_refresh(t);
        if (body_ready) {
            traffic_tow_sleep_refresh(t);
            body_ready = !t->asleep;
        }
        if (body_ready) {
            traffic_tow_constraint(t, dt);
            B3RigidBody pose;
            Vec3 vel = {t->rb.vel[0], t->rb.vel[1], -t->rb.vel[2]};
            carcol_synth_rb(&pose, t->pos,
                            t->pos.y - 0.5f - g_traffic_ymin[t->car],
                            t->yaw, vel);
            memcpy(t->rb.frame, pose.frame, sizeof(t->rb.frame));
            memcpy(t->rb.inv_frame, pose.inv_frame, sizeof(t->rb.inv_frame));
            memcpy(t->rb.inv_inertia_body, pose.inv_inertia_body,
                   sizeof(t->rb.inv_inertia_body));
            memcpy(t->rb.inv_inertia_world, pose.inv_inertia_world,
                   sizeof(t->rb.inv_inertia_world));
            t->rb.force_acc[0] += (sinf(t->yaw) * t->speed - t->rb.vel[0])
                                  * t->mass_kg / dt;
            t->rb.force_acc[1] += 20.0f * t->mass_kg;
            t->rb.force_acc[2] += (cosf(t->yaw) * t->speed - t->rb.vel[2])
                                  * t->mass_kg / dt;
            traffic_trailer_update(t);
        }

        // ------------------------------------------------------ watchdogs
        // Everything below is GLUE standing in for the retail traffic
        // streamer: retail keeps a pool of cars around the player's
        // neighbourhood and recycles the rest, so a car can neither wander
        // off the map nor sit wedged for a whole lap.
        int recycle = 0;
        if (!raw_path && fabsf(lat_err) > 9.0f) {
            t->off_time += dt;
            // 9 m off lane is already the next lane over; 22 m is off the
            // whole four-lane cross-section and unrecoverable at 22 m/s.
            if (t->off_time > 1.5f || fabsf(lat_err) > 22.0f) recycle = 1;
        } else {
            t->off_time = 0.0f;
        }
        if (t->crashed_until <= g_race_time && t->speed < 1.0f) {
            t->stall_time += dt;
            if (t->stall_time > 8.0f) recycle = 1;      // wedged
        } else {
            t->stall_time = 0.0f;
        }
        if (!raw_path) {
            /* GLUE range cull, for the LEGACY lane fallback only.
             *
             * Retail decides traffic residency by WINDOW MEMBERSHIP, not by
             * a radius: FUN_001A28B0 walks the current pool window and the
             * two preceding ones, FUN_001A3470 stamps their requests, and a
             * body whose request is no longer stamped is retired -- which is
             * exactly what traffic_pool_refresh()'s "not-seen" pass already
             * does.  Applying this 420 m cull on top of that fought the pool
             * directly: the window set legitimately reaches much further (the
             * spawn trace shows cars seeded 772 m out), so the pool created a
             * car and this destroyed it in the same frame, forever.  Measured
             * over 60 s: 175,868 recycles from here against 9,592 legitimate
             * path-end retirements, ~1,100 respawns per second, with slots
             * changing car model every frame -- and because each respawn
             * re-draws a row, some landed in front of the player (400 spawns
             * within 150 m ahead, one at 2 m).  That is the "traffic
             * disappears / appears in front of me" report. */
            float px = t->pos.x - g_player.pos.x;
            float pz = t->pos.z - g_player.pos.z;
            if (px * px + pz * pz > 420.0f * 420.0f) recycle = 1;
        }
        if (traffic_legacy()) recycle = 0;
        if (recycle) {
            // sign -1 = advance along DESCENDING oncoming index = AHEAD
            // of the player in RACE direction. +1 placed every recycled
            // car 260 m BEHIND a player receding at 50+ m/s -- the pool
            // chased the player's old position forever and the user saw
            // no traffic at all.
            if (raw_path) {
                if (t->pool_request >= 0) {
                    g_pool_release_why = "watchdog-recycle";
                    traffic_pool_release_slot(i);
                    continue;
                }
                if (!traffic_path_seed(t, t->spawn + 1 + i))
                    traffic_place(t, t->lane + 1 + (i & 1), &g_player.pos,
                                  260.0f + (float)(i % 5) * 26.0f, -1);
            } else {
                traffic_place(t, t->lane + 1 + (i & 1), &g_player.pos,
                              260.0f + (float)(i % 5) * 26.0f, -1);
            }
        }
    }
    traffic_reservations_rebuild();
    // ------------------------------------------------------- telemetry
    // B3_TRAFFIC_TELEM=<period_s>: per-car lane error / stall / off-road
    // census, plus running worst-case counters (headless lap diagnosis).
    {
        static float period = -1.0f, next = 0.0f;
        static float worst_lat = 0.0f;
        static int ever_off = 0, ever_stall = 0;
        if (period < 0.0f) {
            const char* e = getenv("B3_TRAFFIC_TELEM");
            period = e ? (float)atof(e) : 0.0f;
            next = 0.0f;
        }
        if (period > 0.0f && g_race_time >= next) {
            next = g_race_time + period;
            int noff = 0, nstall = 0, nactive = 0, nidle = 0;
            int raw_paths = traffic_paths_active();
            for (int i = 0; i < g_traffic_n; i++) {
                TrafficCar* t = &g_traffic[i];
                if (t->active) {
                    nactive++;
                    if (t->speed < 1.0f) nidle++;
                }
                int seg = t->seg; float lat = 0.0f;
                if (!raw_paths)
                    route_project(t->pos.x, t->pos.z, seg, 24, &seg, NULL, &lat);
                float le = raw_paths ? 0.0f : lat - t->lane_lat;
                if (fabsf(le) > worst_lat) worst_lat = fabsf(le);
                if (fabsf(le) > 9.0f) { noff++; ever_off++; }
                if (t->stall_time > 4.0f) { nstall++; ever_stall++; }
                float px = t->pos.x - g_player.pos.x;
                float pz = t->pos.z - g_player.pos.z;
                printf("[tfc] t=%6.1f c%02d act%d %-9s cat%d paint%d "
                       "cruise%5.1f lane%d(%+6.2f%s) "
                       "dir%+d path%u@%6.1f "
                       "lat%+7.2f err%+7.2f spd%5.1f stall%4.1f off%4.1f "
                       "dplayer%6.0f pos %7.1f %6.1f %7.1f %s%s\n",
                       g_race_time, i, t->active,
                       B3_TRAFFIC_CARS[t->car].id,
                       B3_TRAFFIC_CARS[t->car].cat, t->paint,
                       t->cruise_ms > 1.0f ? t->cruise_ms
                                           : B3_TRAFFIC_SPEED_MS,
                       t->lane, t->lane_lat,
                       t->lane_dir > 0 ? "+" : "-", (int)t->path_dir, t->path_id,
                       t->path_cursor, lat, le, t->speed,
                       t->stall_time, t->off_time, sqrtf(px * px + pz * pz),
                       t->pos.x, t->pos.y, t->pos.z,
                       t->crashed_until > g_race_time ? "CRASHED " : "",
                       t->trailer >= 0 ? B3_TRAFFIC_CARS[t->trailer].id : "");
            }
            printf("[tfc] t=%6.1f SUMMARY active=%d idle=%d off_now=%d "
                   "stall_now=%d worst_lane_err=%.2f off_samples=%d "
                   "stall_samples=%d\n",
                   g_race_time, nactive, nidle, noff, nstall, worst_lat,
                   ever_off, ever_stall);
        }
    }
}

// Legacy capsule contact retained only as a disabled reference.  The live
// traffic contact path is carcol_pass(), which uses b3_carcol_resolve()'s
// recovered alive/wreck thresholds rather than this mass-based GLUE.
static void traffic_interact(void) {
    for (int i = 0; i < g_traffic_n; i++) {
        TrafficCar* t = &g_traffic[i];
        if (!t->active || !t->streamed || t->asleep
            || t->crashed_until > g_race_time) continue;
        // Capsule along the traffic car's heading (buses/trucks are long;
        // a bounding sphere would shove racers off the adjacent lane).
        float fx = sinf(t->yaw), fz = -cosf(t->yaw);
        float half = 0.5f * (g_traffic_len[t->car] > 2.0f
                             ? g_traffic_len[t->car] - 1.6f : 1.4f);
        for (int j = 0; j < g_num_vehicles; j++) {
            Vehicle* v = &g_vehicles[j];
            if (!v->active) continue;
            float rx = v->pos.x - t->pos.x, rz = v->pos.z - t->pos.z;
            float along = rx * fx + rz * fz;
            if (along > half) along = half;
            if (along < -half) along = -half;
            float cx = t->pos.x + fx * along, cz = t->pos.z + fz * along;
            float dx = v->pos.x - cx, dz = v->pos.z - cz;
            float dist = sqrtf(dx * dx + dz * dz);
            const float rad = 2.2f;
            if (dist >= rad || dist < 1e-4f) continue;

            // Separate along the contact normal.
            float ux = dx / dist, uz = dz / dist;
            v->pos.x = cx + ux * rad;
            v->pos.z = cz + uz * rad;

            float tvx = sinf(t->yaw) * t->speed;
            float tvz = -cosf(t->yaw) * t->speed;
            float rvx = v->vel.x - tvx, rvz = v->vel.z - tvz;
            float rel = sqrtf(rvx * rvx + rvz * rvz);

            if (rel > 8.0f && v->crashed_until <= 0.0f
                && g_race_time >= v->immune_until) {
                if (v->sim.speed < 3.0f) {
                    // Quasi-stationary racer: the traffic car is the one
                    // driving into a pileup -- it wrecks, whatever the
                    // masses (GLUE; stops recovery chain-wrecking at
                    // junction mouths the oncoming line threads).
                    t->crashed_until = g_race_time + 5.0f;
                    t->speed = 0.0f;
                } else if (v->cfg.mass_kg >= t->mass_kg) {
                    // Traffic car is the smaller: it gets wrecked.
                    t->crashed_until = g_race_time + 5.0f;
                    t->speed = 0.0f;
                    v->sim.speed *= 0.92f;
                    if (v == &g_player)
                        printf("[Burnout3] traffic hit: wrecked %s "
                               "(%.0fkg vs your %.0fkg)\n",
                               B3_TRAFFIC_CARS[t->car].id, t->mass_kg,
                               v->cfg.mass_kg);
                } else {
                    // Racer is the smaller: crash through the verified
                    // chain fields (slam stamp + 5 s recovery + the
                    // per-slot latch, the crash-record+0x130 duration --
                    // 3.0 fresh for a car / 15.0 mid-presentation).
                    v->slam_time = g_race_time;     // racecar+0x1598
                    v->slam_by = -1;                // environment, not a car
                    /* SFX: car-vs-world impact (FUN_0014EEA0, IMPACTWORL);
                     * its window is 6..22, i.e. a speed-scale quantity. */
                    b3_sfx_event_at(B3_SFX_IMPACT_WORLD, v->sim.speed,
                                    v->pos.x, v->pos.y, v->pos.z);
                    v->crashed_until = g_race_time + 5.0f;
                    v->immune_until = g_race_time + crash_latch_for(v);
                    /* CRASH-SHOW H2: the ticker's opening descriptor */
                    if (v == &g_player) {
                        g_crash_hit_kind = B3_HUD_HIT_TRAFFIC;
                        g_crash_hit_id   = B3_TRAFFIC_CARS[t->car].id;
                    }
                    v->sim.speed *= 0.25f;
                    // CRASH-EVENT: physical wreck -- impact at the capsule
                    // contact, normal from the traffic car into the racer.
                    // Body-to-body mid-race wreck -> the crash-record entry
                    // (0.40 rear-left torque, no launch); the launch belongs
                    // only to the rollover entry.
                    wreck_begin_for(v,
                        (Vec3){cx, v->pos.y - 0.2f, cz},
                        (Vec3){ux, 0.0f, uz},
                        (Vec3){rvx, 0.0f, rvz}, B3_WRECK_ENTRY_CAR);
                    if (v == &g_player)
                        printf("[Burnout3] CRASHED into %s traffic "
                               "(%.0fkg vs your %.0fkg)!\n",
                               B3_TRAFFIC_CARS[t->car].id, t->mass_kg,
                               v->cfg.mass_kg);
                }
            } else if (rel > 2.0f) {
                // Scrape: traffic driver brakes off its cruise speed; the
                // racer only loses a light rub (a parked wreck must not
                // drag passers to a halt).
                t->speed *= 0.80f;
                v->sim.speed *= 0.995f;
            }
        }
    }
}

// Draw the traffic (own block -- the racer draw loop stays untouched).
// Slope-conform pose for the yaw-only traffic follower: probe the road
// under the car and lean the body onto the surface plane (dump 035: the
// flat pose sank the uphill half into inclines). origin = hub-plane point
// (probed ground + wheel radius via -ymin), R9 = row-major object->world
// (carfx convention). Flat ground reduces exactly to the old yaw path.
static void traffic_pose(const TrafficCar* t, float origin[3], float R9[9])
{
    float gy = t->pos.y - 0.5f - g_traffic_ymin[t->car];
    float up[3] = { 0.0f, 1.0f, 0.0f };
    float gh, gn[3];
    if (b3_collision_ready()
        && b3_ground_probe(t->pos.x, t->pos.y + 1.0f, t->pos.z,
                           &gh, gn) >= 0) {
        gy = gh - g_traffic_ymin[t->car];
        if (gn[1] > 0.5f) { up[0] = gn[0]; up[1] = gn[1]; up[2] = gn[2]; }
    }
    origin[0] = t->pos.x; origin[1] = gy; origin[2] = t->pos.z;
    float at[3] = { sinf(t->yaw), 0.0f, -cosf(t->yaw) };
    float d = at[0]*up[0] + at[1]*up[1] + at[2]*up[2];
    at[0] -= d*up[0]; at[1] -= d*up[1]; at[2] -= d*up[2];
    float l = sqrtf(at[0]*at[0] + at[1]*at[1] + at[2]*at[2]);
    if (l > 1e-4f) { at[0] /= l; at[1] /= l; at[2] /= l; }
    else { at[0] = sinf(t->yaw); at[1] = 0.0f; at[2] = -cosf(t->yaw); }
    float rt[3] = { at[1]*up[2] - at[2]*up[1],
                    at[2]*up[0] - at[0]*up[2],
                    at[0]*up[1] - at[1]*up[0] };
    R9[0] = rt[0]; R9[1] = up[0]; R9[2] = -at[0];
    R9[3] = rt[1]; R9[4] = up[1]; R9[5] = -at[1];
    R9[6] = rt[2]; R9[7] = up[2]; R9[8] = -at[2];
}

// Draw one traffic body + its wheels at an explicit pose (used for the
// towed trailer; the tractor keeps the main loop's path unchanged).
static void traffic_draw_at(int car, const float org[3], const float R9[9],
                            float spin_deg) {
    if (!g_traffic_lists[car]) return;
    glPushMatrix();
    float M[16] = {
        R9[0], R9[3], R9[6], 0,
        R9[1], R9[4], R9[7], 0,
        R9[2], R9[5], R9[8], 0,
        org[0], org[1], org[2], 1,
    };
    glMultMatrixf(M);
    glCallList(g_traffic_lists[car]);
    if (g_traffic_wheel_lists[car]) {
        for (int w = 0; w < g_traffic_wheel_count[car]; w++) {
            glPushMatrix();
            glTranslatef(g_traffic_wheel_pos[car][w][0],
                         g_traffic_wheel_pos[car][w][1],
                         g_traffic_wheel_pos[car][w][2]);
            if (g_traffic_wheel_mirror[car][w] < 0)
                glRotatef(180.0f, 0, 1, 0);
            glRotatef(spin_deg * (g_traffic_wheel_mirror[car][w] < 0
                                  ? 1.0f : -1.0f), 1, 0, 0);
            glCallList(g_traffic_wheel_lists[car]);
            glPopMatrix();
        }
    }
    glPopMatrix();
}

// Towed trailers: their own pose, drawn after the tractors.
static void traffic_render_trailers(void) {
    for (int i = 0; i < g_traffic_n; i++) {
        const TrafficCar* t = &g_traffic[i];
        if (!t->active || t->trailer < 0) continue;
        TrafficCar tt = *t;
        tt.pos = t->tr_pos;
        tt.yaw = t->tr_yaw;
        tt.car = t->trailer;
        float org[3], R9[9];
        traffic_pose(&tt, org, R9);
        traffic_draw_at(t->trailer, org, R9, g_traffic_spin[i] * RAD_TO_DEG);
    }
}

static void traffic_render(void) {
    traffic_render_trailers();
    for (int i = 0; i < g_traffic_n; i++) {
        const TrafficCar* t = &g_traffic[i];
        if (!t->active) continue;
        glPushMatrix();
        if (g_traffic_lists[t->car]) {
            float org[3], R9[9];
            traffic_pose(t, org, R9);
            // columns (right, up, -at) from the row-major R9; same mapping
            // as the racer full-pose draw. Flat ground == old yaw path.
            float M[16] = {
                R9[0], R9[3], R9[6], 0,
                R9[1], R9[4], R9[7], 0,
                R9[2], R9[5], R9[8], 0,
                org[0], org[1], org[2], 1,
            };
            glMultMatrixf(M);
            glCallList(g_traffic_lists[t->car]);
            if (g_traffic_wheel_lists[t->car]) {
                // Spin from actual travelled distance (parked/crashed cars
                // do not move, so their wheels hold still for free).
                float dx = t->pos.x - g_traffic_prev[i].x;
                float dz = t->pos.z - g_traffic_prev[i].z;
                float r = g_traffic_wheel_radius[t->car];
                if (r > 0.05f)
                    g_traffic_spin[i] += sqrtf(dx * dx + dz * dz) / r;
                if (g_traffic_spin[i] > 6.2831853f)
                    g_traffic_spin[i] -= 6.2831853f;
                float spin_deg = g_traffic_spin[i] * RAD_TO_DEG;
                for (int w = 0; w < g_traffic_wheel_count[t->car]; w++) {
                    glPushMatrix();
                    glTranslatef(g_traffic_wheel_pos[t->car][w][0],
                                 g_traffic_wheel_pos[t->car][w][1],
                                 g_traffic_wheel_pos[t->car][w][2]);
                    if (g_traffic_wheel_mirror[t->car][w] < 0)
                        glRotatef(180.0f, 0, 1, 0);
                    glRotatef(spin_deg
                              * (g_traffic_wheel_mirror[t->car][w] < 0
                                 ? 1.0f : -1.0f), 1, 0, 0);
                    glCallList(g_traffic_wheel_lists[t->car]);
                    glPopMatrix();
                }
            }
            g_traffic_prev[i] = t->pos;
        } else {
            glTranslatef(t->pos.x, t->pos.y, t->pos.z);
            glRotatef(t->yaw * RAD_TO_DEG, 0, 1, 0);
            glDisable(GL_TEXTURE_2D);
            glColor3f(0.75f, 0.75f, 0.78f);   // box fallback
            glBegin(GL_QUADS);
            glVertex3f(-0.9f, 0.0f, 1.8f);
            glVertex3f(0.9f, 0.0f, 1.8f);
            glVertex3f(0.9f, 0.0f, -1.8f);
            glVertex3f(-0.9f, 0.0f, -1.8f);
            glVertex3f(-0.7f, 0.8f, 0.9f);
            glVertex3f(0.7f, 0.8f, 0.9f);
            glVertex3f(0.7f, 0.8f, -0.9f);
            glVertex3f(-0.7f, 0.8f, -0.9f);
            glEnd();
        }
        glPopMatrix();
    }

    // CARFX: traffic light coronas. Head and tail are on permanently, the
    // same ambient pair the racers are drawn with (carObj+0x18FF's writer is
    // [?], and the reference captures show running lights on in daylight);
    // the emitter's own cosine gate on the lamp normal is what makes an
    // oncoming car show white headlights and a receding one show red tails.
    // Traffic is a yaw-only line follower, so a yaw pose is the full pose.
    b3_carfx_corona_pass_begin();
    for (int i = 0; i < g_traffic_n; i++) {
        const TrafficCar* t = &g_traffic[i];
        if (!t->active || !g_traffic_lists[t->car]) continue;
        float p[3], R[9];
        traffic_pose(t, p, R);   // slope-conform, matches the body draw
        b3_carfx_corona_draw_extra(t->car, p, R,
                                   B3_CARFX_LIGHT_HEAD
                                   | B3_CARFX_LIGHT_TAIL);
    }
    b3_carfx_corona_pass_end();
}

// Find the per-car VDB override entry (burnout3_car_physics.h, extracted from
// the retail Data/vdb.xml by tools/extract_car_vdb.py) for a roster vehicle.
static const B3CarPhysics* car_vdb_lookup(const VehicleInfo* info) {
    if (!info) return NULL;
    for (int i = 0; i < B3_CAR_PHYSICS_COUNT; i++) {
        const B3CarPhysics* c = &B3_CAR_PHYSICS[i];
        if (strcmp(c->class_code, info->class_code) == 0 &&
            strcmp(c->file, info->file) == 0)
            return c;
    }
    return NULL;
}

// Race-start gameplay state: boost record reset per FUN_0017A3C0, takedown
// bookkeeping cleared, no pending slams/attributions.
static void reset_gameplay_state(Vehicle* v) {
    b3_boost_reset(&v->bar);
    /* B3_MAX_BOOST=1 (testing): start the PLAYER with a full bar.  Reaching
     * FUN_001121F0's 150 mph closing gate needs real speed, so hunting
     * traffic crashes by hand is painful from a cold bar.  Player only, and
     * off unless asked for, so it cannot skew a parity measurement. */
    {
        static int maxb = -1;
        if (maxb < 0) maxb = getenv("B3_MAX_BOOST") != NULL;
        if (maxb && v == &g_player) {
            v->bar.meter = v->bar.size;
            v->boost_meter = v->bar.meter;
        }
    }
    b3_score_events_reset(&v->sev);
    v->sev_prev_ok = 0;
    b3_takedown_score_reset(&v->score);
    v->slam_time = -1.0f;
    v->slam_side = 0;
    v->slam_by = -1;
    v->pending_td_time = 0.0f;
    v->pending_td_victim = -1;
    v->crashed_until = 0.0f;
    v->immune_until = 0.0f;
    v->stuck_ref = (Vec3){0, 0, 0};    // far off-track: re-referenced at once
    v->stuck_ref_time = 0.0f;
    v->unstuck_side = 0;
    v->unstuck_until = 0.0f;
    memset(v->taken_down_by, 0, sizeof v->taken_down_by);
    if (!g_tdr_ready) {
        b3_td_reset(&g_tdr, g_num_vehicles);
        for (int i = 0; i < g_num_vehicles; i++)
            b3_td_set_car(&g_tdr, i, i == 0 ? 0 : 1, i);  // racecar+0x1920/+0x19BC
        g_tdr_ready = 1;
    }
}

static void init_vehicles(void) {
    // Defaults kept for anything that still reads the global config; each car
    // below gets its OWN config with its real Data/vdb.xml values applied.
    b3_physics_defaults(&g_phys_cfg);

    // The grid is populated from the real roster extracted out of pveh/ by
    // tools/extract_vehicles.py. dim_a is the per-car header float at +0x14;
    // it scales consistently with vehicle class, so it is used as the body
    // length.
    g_num_vehicles = 6;   // real event grid size

    for (int i = 0; i < g_num_vehicles; i++) {
        const VehicleInfo* info = roster_player_car(i);
        Vehicle* v = &g_vehicles[i];

        *v = (Vehicle){
            .max_speed = (i == 0) ? 85.0f : 65.0f + (i - 1) * 3.0f,
            .health = 100.0f,
            .vehicle_id = i,
            .active = 1,
        };
        spawn_on_grid(v, i);
        v->info = info;
        if (info) v->body_len = info->dim_a;

        // THIS car's physics: compiled-in defaults, then its own VDB values
        // (all 64 params for drivable cars). Cars without VDB overrides keep
        // the marked reconstruction fallback in b3_physics_defaults().
        b3_physics_defaults(&v->cfg);
        v->vdb = car_vdb_lookup(info);
        if (v->vdb)
            for (int p = 0; p < v->vdb->n_params; p++)
                b3_config_set_by_offset(&v->cfg, v->vdb->params[p].offset,
                                        v->vdb->params[p].value);
        v->sim = (B3VehicleState){ .gear = 2, .rpm = v->cfg.idle_rpm };
        reset_gameplay_state(v);
    }

    printf("[Burnout3] Grid of %d cars from a roster of %d real vehicles\n"
           "           (per-car physics from Data/vdb.xml via burnout3_car_physics.h):\n",
           g_num_vehicles, VEHICLE_COUNT);
    for (int i = 0; i < g_num_vehicles; i++) {
        const Vehicle* v = &g_vehicles[i];
        if (!v->info) continue;
        printf("           %d. %-10s %-9s %-10s mass %5.0fkg tq %4.0f "
               "gears %.2f/%.2f/%.2f/%.2f/%.2f/%.2f final %.2f %s\n",
               i, v->vdb ? v->vdb->id : v->info->file, v->info->class_code,
               v->info->class_name, v->cfg.mass_kg, v->cfg.torque,
               v->cfg.gear[2], v->cfg.gear[3], v->cfg.gear[4],
               v->cfg.gear[5], v->cfg.gear[6], v->cfg.gear[7],
               v->cfg.gear[8],
               v->vdb ? "[VDB]" : "[fallback defaults]");
    }
}

// ============================================================
// Audio (original)
// ============================================================

// Real audio extracted from the game (tools/extract_awd.py / extract_rws.py):
// rpm-labelled engine loops from the player's car's own AWD bank, and the
// front-end music stream. Only the mixing below is harness code.
typedef struct { Sint16* pcm; Uint32 frames; int rate; float rpm; } EngineLoop;
static EngineLoop g_eng[8];
static int g_eng_n = 0;
static double g_eng_phase = 0.0;
static Sint16* g_music = NULL;
static Uint32 g_music_frames = 0;
static double g_music_pos = 0.0;
static int g_music_rate = 44100, g_music_ch = 2;

static int load_wav_s16(const char* path, Sint16** pcm, Uint32* frames,
                        int* rate, int* channels) {
    SDL_AudioSpec spec;
    Uint8* buf;
    Uint32 blen;
    if (!SDL_LoadWAV(path, &spec, &buf, &blen)) return -1;
    if (spec.format != AUDIO_S16LSB && spec.format != AUDIO_S16SYS) {
        SDL_FreeWAV(buf);
        return -1;
    }
    *pcm = (Sint16*)buf;
    *frames = blen / 2 / spec.channels;
    *rate = spec.freq;
    if (channels) *channels = spec.channels;
    return 0;
}

static void load_real_audio(void) {
    const VehicleInfo* info = g_player.info;
    if (info) {
        char base[32], path[160];
        snprintf(base, sizeof(base), "%s", info->file);
        char* dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        // The rpm labels the loops ship with (see docs/AUDIO_NOTES.md).
        static const int rpms[] = {2873, 4317, 5279, 6234};
        for (int i = 0; i < 4 && g_eng_n < 8; i++) {
            snprintf(path, sizeof(path),
                     "build/audio/awd_pveh_%s_%s_high/eng_%d.wav",
                     info->class_code, base, rpms[i]);
            EngineLoop* L = &g_eng[g_eng_n];
            int ch;
            if (load_wav_s16(path, &L->pcm, &L->frames, &L->rate, &ch) == 0
                && ch == 1 && L->frames > 0) {
                L->rpm = (float)rpms[i];
                g_eng_n++;
            }
        }
    }
    int mus = b3_music_init();          /* MUSIC: scans build/music */
    printf("[Burnout3] REAL audio: %d engine loops, EA TRAX %d/44 tracks\n",
           g_eng_n, mus);
    /* Mix balance (GLUE, user-tuned): the SFX master default of 0.9 over
     * music's 0.30 drowned the soundtrack. */
    mixer_load();               /* build/mixer.cfg overrides defaults */
    mixer_apply();
}

static void audio_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    Sint16* s = (Sint16*)stream;
    int n = len / 2;

    /* SFX-CRASH: while a crash is running the engine voice follows the
     * CRASHED PATH's own engine speed, not the parked drivetrain's last
     * rpm.  Retail never mutes this voice -- FUN_0011BE50's crashed branch
     * starves it (neutral + FUN_00121560 at zero throttle), so it coasts to
     * idle in about half a second.  See burnout3_sfx.h. */
    float rpm = b3_sfx_engine_rpm(g_player.sim.rpm);
    if (rpm < 900.0f) rpm = 900.0f;

    // Nearest loop in log-rpm, pitch-shifted to the live engine speed.
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < g_eng_n; i++) {
        float d = fabsf(logf(rpm / g_eng[i].rpm));
        if (d < bd) { bd = d; best = i; }
    }

    for (int i = 0; i < n; i++) {
        float out = 0.0f;
        if (best >= 0) {
            EngineLoop* L = &g_eng[best];
            g_eng_phase += (double)L->rate / 44100.0 * (rpm / L->rpm);
            while (g_eng_phase >= L->frames) g_eng_phase -= L->frames;
            Uint32 i0 = (Uint32)g_eng_phase;
            Uint32 i1 = (i0 + 1 < L->frames) ? i0 + 1 : 0;
            float fr = (float)(g_eng_phase - i0);
            /* 0.55 -> 0.33: user 2026-08-13 "engine noise is 40% too
             * loud". NOTE this gain sits OUTSIDE b3_sfx master -- it never
             * followed the master reductions, which is also why it masked
             * the traffic-pass whoosh. */
            out += (L->pcm[i0] * (1.0f - fr) + L->pcm[i1] * fr) * g_mix[0];
        }
        out += b3_music_next_sample(); /* MUSIC: the EA TRAX stream */
        out += b3_sfx_next_sample();   /* SFX: event voices */
        if (out > 32767.0f) out = 32767.0f;
        if (out < -32768.0f) out = -32768.0f;
        s[i] = (Sint16)out;
    }
}

static void audio_init(void) {
    load_real_audio();
    b3_sfx_init();                 /* SFX: crash/slam/boost event waves */
    SDL_AudioSpec spec;
    SDL_zero(spec);   // was uninitialised; SDL reads padding/userdata from it
    spec.freq = 44100;
    spec.format = AUDIO_S16SYS;
    spec.channels = 1;
    spec.samples = 1024;
    spec.callback = audio_callback;
    
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (g_audio_dev) {
        SDL_PauseAudioDevice(g_audio_dev, 0);
        printf("[Burnout3] Audio initialized\n");
    }
}

// ============================================================
// Rendering (original; the game uses RenderWare RW36)
// ============================================================

static void render_init(int w, int h) {
    glViewport(0, 0, w, h);
    glClearColor(0.3f, 0.5f, 0.8f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    // Fixed-function lighting stays off: the track uses the game's textures
    // (lighting there is baked into them), and nothing here submits normals.
    glDisable(GL_LIGHTING);
}

// Axis-aligned HUD rectangle in NDC; caller wraps in glBegin(GL_QUADS).
static void hud_rect(float x, float y, float w, float h) {
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
}

// Seven-segment digit at (x,y), segment length s. Segments A..G bit 6..0.
static void hud_digit(float x, float y, float s, int d) {
    static const unsigned char seg[10] = {
        0x7E, 0x30, 0x6D, 0x79, 0x33, 0x5B, 0x5F, 0x70, 0x7F, 0x7B };
    float t = s * 0.28f;   // segment thickness
    unsigned char m = seg[d % 10];
    glBegin(GL_QUADS);
    if (m & 0x40) hud_rect(x, y + 2*s, s, t);              // A top
    if (m & 0x20) hud_rect(x + s - t, y + s, t, s);        // B top-right
    if (m & 0x10) hud_rect(x + s - t, y, t, s);            // C bottom-right
    if (m & 0x08) hud_rect(x, y, s, t);                    // D bottom
    if (m & 0x04) hud_rect(x, y, t, s);                    // E bottom-left
    if (m & 0x02) hud_rect(x, y + s, t, s);                // F top-left
    if (m & 0x01) hud_rect(x, y + s, s, t);                // G middle
    glEnd();
}

// Right-aligned n-digit number; leading zeros blanked (except last digit).
static void hud_number(float x, float y, float s, int value, int digits) {
    float adv = s * 1.35f;
    for (int i = digits - 1; i >= 0; i--) {
        int div = 1;
        for (int k = 0; k < i; k++) div *= 10;
        int d = (value / div) % 10;
        if (value >= div || i == 0)
            hud_digit(x + (digits - 1 - i) * adv, y, s, d);
    }
}

static void render_frame(void) {
    int w, h;
    SDL_GetWindowSize(g_window, &w, &h);
    // Resizable window: track the live size every frame (projection aspect,
    // HUD scale and the blur's frame grab all already read w/h per frame;
    // the viewport was the only fixed piece).
    glViewport(0, 0, w, h);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // Chase camera: sits behind the car along its heading, smoothed so wall
    // impacts and drift snaps do not jerk the view.
    float hy = g_player.rot.y;
    Vec3 fwd = (Vec3){sinf(hy), 0.0f, -cosf(hy)};
    // RECOVERED chase camera (Camera/Follow mode 2, FUN_0015E550 [C]):
    // offset (0, 0.95, -6.8), focus 2 m ahead in the car frame, speed-gated
    // pitch blend, FOV 90->110 on boost. The user-visible symptom of the
    // old GLUE (-10 back, +3.6 up) was "camera too high" vs the reference.
    static B3CamFollow s_follow;
    static int s_follow_init = 0;
    static float s_boost_ramp = 0.0f;   // GLUE ramp toward the FOV law's r=1
    if (!s_follow_init) { b3_cam_follow_init(&s_follow); s_follow_init = 1; }
    s_boost_ramp += ((g_player.bar.boosting ? 1.0f : 0.0f) - s_boost_ramp)
                  * fminf(1.0f, 4.0f * g_tdfx_real_dt);
    Vec3 want_pos = vec3_add(vec3_sub(g_player.pos, vec3_scale(10.0f, fwd)),
                             (Vec3){0, 3.6f, 0});
    static Vec3 cam_smooth;
    static int cam_init = 0;
    if (!cam_init) { cam_smooth = want_pos; cam_init = 1; }
    // CRASH-EVENT camera (GLUE): while the player's wreck tumbles, stop
    // chasing the (spinning) heading -- hold the position instead. The real
    // crash camera is its own director object, not recovered.
    int player_wrecked = g_player.crashed_until > 0.0f && g_wrecks[0].active;
    if (player_wrecked) want_pos = cam_smooth;
    // Camera smoothing is presentation-rate: use the undilated delta so the
    // chase cam doesn't go sluggish during takedown slow-motion.
    float k = 1.0f - expf(-8.0f * g_tdfx_real_dt);
    cam_smooth = vec3_add(cam_smooth, vec3_scale(k, vec3_sub(want_pos, cam_smooth)));
    Vec3 cam_pos = cam_smooth;
    Vec3 cam_target = vec3_add(vec3_add(g_player.pos, vec3_scale(6.0f, fwd)),
                               (Vec3){0, 1.4f, 0});
    // Normal driving runs the recovered mode-2 law over the live body
    // (GL-space rows: z components negated from the game-space frame).
    if (!player_wrecked && g_player.fsim_ready) {
        float rows[12];
        const B3RigidBody* prb = &g_player.fsim.rb;
        for (int r = 0; r < 4; r++) {
            rows[r*3+0] = prb->frame[r][0];
            rows[r*3+1] = prb->frame[r][1];
            rows[r*3+2] = -prb->frame[r][2];
        }
        B3TdfxCamera cc;
        b3_cam_follow_update(&s_follow, rows, prb->vel[3], s_boost_ramp,
                             g_delta_time, &cc);
        cam_pos    = (Vec3){ cc.eye[0],  cc.eye[1],  cc.eye[2]  };
        cam_target = (Vec3){ cc.look[0], cc.look[1], cc.look[2] };
        cam_smooth = cam_pos;
        g_cam_fov_deg += (cc.fov - g_cam_fov_deg) * 0.2f;
    }
    // ... and slow-look at the wreck itself.
    if (player_wrecked)
        cam_target = vec3_add(g_player.pos,
                              (Vec3){0, 0.8f + g_wrecks[0].air_time * 1.5f, 0});
    // TAKEDOWN-FX: the cinematic camera takes the view and frames the
    // VICTIM (record+0x00 = attacker racecar +0x15A4) for its 4.8 s.
    {
        B3TdfxStatus st;
        b3_tdfx_status(&st);
        if (st.active && st.victim_slot >= 0 && st.victim_slot < 8) {
            Vehicle* vic = &g_vehicles[st.victim_slot];
            float vp[3] = { vic->pos.x, vic->pos.y, vic->pos.z };
            float vv[3] = { vic->vel.x, vic->vel.y, vic->vel.z };
            float ap[3] = { g_player.pos.x, g_player.pos.y, g_player.pos.z };
            B3TdfxCamera tc;
            if (b3_tdfx_camera(vp, vv, ap, g_tdfx_real_dt, &tc)) {
                float wgt = tc.weight;
                cam_pos.x += (tc.eye[0]  - cam_pos.x) * wgt;
                cam_pos.y += (tc.eye[1]  - cam_pos.y) * wgt;
                cam_pos.z += (tc.eye[2]  - cam_pos.z) * wgt;
                cam_target.x += (tc.look[0] - cam_target.x) * wgt;
                cam_target.y += (tc.look[1] - cam_target.y) * wgt;
                cam_target.z += (tc.look[2] - cam_target.z) * wgt;
                cam_smooth = cam_pos;   // do not snap back on release
                // recovered in-game FOV (90 deg, 0x003B1850) blends in
                g_cam_fov_deg += (tc.fov - g_cam_fov_deg) * wgt;
            }
        } else if (player_wrecked) {
            // CRASH CAMERA: there is no crash camera MODE in retail -- the
            // ordinary chase camera (director mode 2, FUN_0015E550) keeps
            // running over the wreck's tumbling TRANSFORM. Its focus sits
            // 2 m ahead inside the car's own frame and its pitch target is
            // asin(forward.y), so the tumble sweeps the view (RE_TAKEDOWN_FX
            // 9.2). boost_ramp = racecar+0x11AC; harness has none yet -> 0
            // keeps the FOV at the base 90.
            float wv[3] = { g_player.vel.x, g_player.vel.y, g_player.vel.z };
            float sp = sqrtf(wv[0]*wv[0] + wv[1]*wv[1] + wv[2]*wv[2]);
            float rows[12];
            for (int ri = 0; ri < 4; ri++)
                for (int rj = 0; rj < 3; rj++)
                    rows[ri*3+rj] = g_wrecks[0].frame[ri][rj];
            B3TdfxCamera cc;
            if (b3_tdfx_crash_camera_x(rows, sp, 0.0f, g_tdfx_real_dt, &cc)) {
                cam_pos    = (Vec3){ cc.eye[0],  cc.eye[1],  cc.eye[2]  };
                cam_target = (Vec3){ cc.look[0], cc.look[1], cc.look[2] };
                cam_smooth = cam_pos;
                g_cam_fov_deg += (cc.fov - g_cam_fov_deg) * 0.15f;
            }
        }
        if (!player_wrecked)
            b3_tdfx_crash_camera_reset();   // re-prime for the next crash
        if (!st.active && !player_wrecked)
            g_cam_fov_deg += (60.0f - g_cam_fov_deg) * 0.1f;
    }

    /* B3_AGGRO_CAM=1: while any AI is in aggression state 4 (slamming),
     * park the camera behind and beside it so the swing is visible. */
    if (getenv("B3_AGGRO_CAM") && g_aggro_cam_slot >= 0
        && g_aggro_cam_slot < g_num_vehicles) {
        Vehicle* av = &g_vehicles[g_aggro_cam_slot];
        float chy = av->rot.y;
        Vec3 cfwd = (Vec3){sinf(chy), 0.0f, -cosf(chy)};
        Vec3 clat = (Vec3){-cfwd.z, 0.0f, cfwd.x};
        cam_pos = vec3_add(vec3_add(av->pos, vec3_scale(-11.0f, cfwd)),
                           vec3_add(vec3_scale(6.0f, clat),
                                    (Vec3){0, 5.0f, 0}));
        Vec3 look = av->pos;
        if (av->aggro.target >= 0 && av->aggro.target < g_num_vehicles) {
            Vehicle* tvv = &g_vehicles[av->aggro.target];
            look.x = (look.x + tvv->pos.x) * 0.5f;
            look.z = (look.z + tvv->pos.z) * 0.5f;
        }
        cam_target = vec3_add(look, (Vec3){0, 0.6f, 0});
        Mat4 view_a = mat4_lookat(cam_pos, cam_target, (Vec3){0, 1, 0});
        Mat4 proj_a = mat4_perspective(60.0f * DEG_TO_RAD,
                                       (float)w / (float)h, 0.1f, 5000.0f);
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf((float*)proj_a.m);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf((float*)view_a.m);
    }

    // Debug: B3_CAMSIDE=1 parks the camera broadside of the player car
    // (nose-direction verification shots).
    if (getenv("B3_CAMSIDE")) {
        float chy = g_player.rot.y;
        Vec3 cfwd = (Vec3){sinf(chy), 0.0f, -cosf(chy)};
        Vec3 clat = (Vec3){-cfwd.z, 0.0f, cfwd.x};
        cam_pos = vec3_add(vec3_add(g_player.pos, vec3_scale(7.0f, clat)),
                           (Vec3){0, 1.6f, 0});
        cam_target = vec3_add(g_player.pos, (Vec3){0, 0.6f, 0});
        Mat4 view_s = mat4_lookat(cam_pos, cam_target, (Vec3){0, 1, 0});
        Mat4 proj_s = mat4_perspective(45.0f * DEG_TO_RAD, (float)w / (float)h,
                                       0.1f, 5000.0f);
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf((float*)proj_s.m);
        glScalef(-1.0f, 1.0f, 1.0f);   // display mirror (see main path)
        glFrontFace(GL_CCW);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf((float*)view_s.m);
        goto cam_done;
    }
    // Debug: B3_CAM="ex,ey,ez,tx,ty,tz" pins the camera for verification shots.
    const char* camspec = getenv("B3_CAM");
    if (camspec) {
        float c[6];
        if (sscanf(camspec, "%f,%f,%f,%f,%f,%f",
                   &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) == 6) {
            cam_pos = (Vec3){c[0], c[1], c[2]};
            cam_target = (Vec3){c[3], c[4], c[5]};
        }
    }
    {
        Mat4 view = mat4_lookat(cam_pos, cam_target, (Vec3){0, 1, 0});
        Mat4 proj = mat4_perspective(g_cam_fov_deg * DEG_TO_RAD, (float)w / (float)h, 0.1f, 5000.0f);
        g_cam_view = view;   // saved for HUD-space projections (opponent tags)
        /* CRASH-CINEMA: the aftertouch direction basis.  Retail takes
         * rows 0/2 of the camera orientation (veh+0x1410, unpacked at
         * 0x00118A24) and flattens y; the harness derives the same two
         * rows from this frame's lookat.  right = cross(fwd, +Y). */
        {
            float fx = cam_target.x - cam_pos.x;
            float fz = cam_target.z - cam_pos.z;
            float fl = sqrtf(fx * fx + fz * fz);
            if (fl > 1e-4f) {
                fx /= fl; fz /= fl;
                g_at_cam_fwd[0] = fx; g_at_cam_fwd[1] = 0.0f;
                g_at_cam_fwd[2] = fz;
                g_at_cam_right[0] = -fz; g_at_cam_right[1] = 0.0f;
                g_at_cam_right[2] =  fx;
            }
        }
        g_cam_proj = proj;
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf((float*)proj.m);
        // HANDEDNESS: the GL world is the Z-mirror of the game world
        // (RE_NOTES 12), so the raw render is the horizontal MIRROR of
        // retail (proven against the xemu references: signage reads
        // backwards, building layout swapped). Flip the final image in the
        // projection; winding flips with it, so the scene renders CCW-front
        // (restored to CW before the HUD, whose quads are CW-wound).
        glScalef(-1.0f, 1.0f, 1.0f);
        glFrontFace(GL_CCW);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf((float*)view.m);
    }
cam_done:;
    /* POSTFX: the sky dome draws first, before any world geometry, with
     * depth writes off (FUN_00032580 / FUN_000323D0 [C]); centred on the
     * camera, scaled by far_clip - 1000. progress picks the gradient-LUT
     * column ([S] identity; track progress is the harness stand-in). */
    {
        float eye[3] = { cam_pos.x, cam_pos.y, cam_pos.z };
        b3_postfx_sky_draw(eye, 5000.0f, g_player.track_progress);
    }
    
    // Draw ground plane (only when the real track is absent; the real mesh has
    // its own ground and a river below road level).
    if (!g_have_real_track) {
        glColor3f(0.2f, 0.5f, 0.2f);
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < g_ground_mesh.num_faces; i++) {
            for (int j = 0; j < 3; j++) {
                Vec3 v = g_ground_mesh.verts[g_ground_mesh.indices[i * 3 + j]];
                glVertex3f(v.x, v.y, v.z);
            }
        }
        glEnd();
    }
    
    // Draw the real extracted track geometry when available (baked once into a
    // display list by load_track_textures).
    if (g_have_real_track && g_track_list) {
        // STATIC-WORLD-2: the world fog. FUN_00038D10 enables D3DRS_FOGENABLE
        // before every world pass and the teardown FUN_00039140 disables it
        // again (@0x000391C5), so retail fogs the WORLD ONLY. The animated
        // materials are ticked here too -- FUN_0019B1E0 runs once a frame off
        // DAT_0060EA20, the same clock the vehicle step reads, so it is the
        // DILATED sim delta and the boards slow down with a takedown replay.
        trackmesh_tick(&g_real_track, g_delta_time);
        trackmesh_fog_begin(&g_real_track);
        glCallList(g_track_list);
        trackmesh_draw_scroll(&g_real_track);
        /* PROPS: the cones/barrier boards/marker posts are world geometry
         * (static.dat +0x3C model table, +0x48 instance transforms), so they
         * are drawn inside the world pass -- retail fogs the world only. */
        b3_props_draw();
        trackmesh_fog_end();
        // TRACK-BLEND: the class-1/7/10 additive term of the world shader,
        // tex.a * gate * pow(max(R.V,0), power) * light * strength. It is view
        // dependent, so it cannot be baked into the display list. See
        // trackmesh_draw_shine() for the recovered equation and its citations.
        float shine_eye[3] = { cam_pos.x, cam_pos.y, cam_pos.z };
        trackmesh_draw_shine(&g_real_track, shine_eye, NULL);
    }

    // B3_DEBUGWALLS=1: draw the collision polylines over the world so wall
    // data vs rendered kerbs can be verified by eye/screenshot.
    static int dbgwalls = -1;
    if (dbgwalls < 0) dbgwalls = getenv("B3_DEBUGWALLS") != NULL;
    if (dbgwalls) {
        glDisable(GL_TEXTURE_2D);
        glLineWidth(3.0f);
        glColor3f(1.0f, 0.15f, 0.15f);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < B3_WALL_A_COUNT; i++)
            glVertex3f(g_wa[i][0], g_wa[i][1] + 0.4f, g_wa[i][2]);
        glEnd();
        glColor3f(0.15f, 0.4f, 1.0f);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < B3_WALL_B_COUNT; i++)
            glVertex3f(g_wb[i][0], g_wb[i][1] + 0.4f, g_wb[i][2]);
        glEnd();
        glColor3f(0.2f, 1.0f, 0.3f);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < B3_WALL_A_COUNT; i++)
            glVertex3f(g_cl[i][0], g_cl[i][1] + 0.4f, g_cl[i][2]);
        glEnd();
        glLineWidth(1.0f);
    }

    // Draw placeholder road (only when no real geometry is loaded)
    glColor3f(0.3f, 0.3f, 0.3f);
    if (!g_have_real_track)
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < (g_have_real_track ? 0 : g_road_mesh.num_faces); i++) {
        for (int j = 0; j < 3; j++) {
            Vec3 v = g_road_mesh.verts[g_road_mesh.indices[i * 3 + j]];
            glVertex3f(v.x, v.y, v.z);
        }
    }
    if (!g_have_real_track) glEnd();
    
    // Draw vehicles: real .bgv meshes where extracted, boxes otherwise.
    for (int i = 0; i < g_num_vehicles; i++) {
        Vehicle* v = &g_vehicles[i];
        if (!v->active) continue;

        glPushMatrix();
        if (g_car_lists[i]) {
            if (v->crashed_until > 0.0f && g_wrecks[i].active) {
                // CRASH-EVENT (GLUE): the tumbling wreck draws with the
                // crash module's full rigid pose. Column mapping follows
                // the verified yaw convention below (mesh nose = -Z):
                // mesh X -> right row, mesh Y -> up row, mesh Z -> -at row.
                const B3WreckState* wk = &g_wrecks[i];
                float M[16] = {
                    wk->frame[0][0],  wk->frame[0][1],  wk->frame[0][2], 0,
                    wk->frame[1][0],  wk->frame[1][1],  wk->frame[1][2], 0,
                    -wk->frame[2][0], -wk->frame[2][1], -wk->frame[2][2], 0,
                    wk->frame[3][0],  wk->frame[3][1],  wk->frame[3][2], 1,
                };
                glMultMatrixf(M);
            } else if (v->fsim_ready) {
            // FULL POSE: the recovered pipeline's rigid body carries pitch
            // and roll (suspension over per-wheel ground rays); rendering
            // yaw only buried the rear on crests (debug dump 016). Game ->
            // harness is the z-negation; columns = (right, up, -at) with
            // mesh nose = -Z, which reduces exactly to the verified
            // glRotatef(-yaw) mapping when pitch = roll = 0.
                const float (*f)[4] = (const float (*)[4])v->fsim.rb.frame;
                float M[16] = {
                    f[0][0],  f[0][1], -f[0][2], 0,
                    f[1][0],  f[1][1], -f[1][2], 0,
                   -f[2][0], -f[2][1],  f[2][2], 0,
                    v->pos.x, v->pos.y - 0.5f - g_car_ymin[i], v->pos.z, 1,
                };
                glMultMatrixf(M);
            } else {
            // Rest the wheels on the road (pos.y sits 0.5 above the surface;
            // with wheel data, g_car_ymin = -wheel radius, hub at y = 0).
            glTranslatef(v->pos.x, v->pos.y - 0.5f - g_car_ymin[i], v->pos.z);
            glRotatef(-v->rot.y * RAD_TO_DEG, 0, 1, 0); // mesh nose = -Z after the
            // loader Z-flip (windshield/driver-position verified). The
            // yaw is NEGATED: heading fwd=(sin h,-cos h) vs glRotatef's
            // CCW-about-+Y mapping of -Z to (-sin h,-cos h) -- rendering
            // +h mirrors the heading in X (cars showed their side to a
            // camera dead ahead; error = 2x heading angle)
            }

            // Damage-state body selection. Intact cars draw the mask-bit0
            // record set: it IS panel-complete (verified by offline render
            // of the split meshes -- closed doors, full body). Drawing the
            // whole-record union instead double-draws every panel region
            // (bit0 intact + bit1 shell variants of the same surfaces),
            // z-fighting so the interior showed through "missing" doors.
            // A parked wreck (crashed_until running) swaps to the bit1
            // shell -- the verified end state of the damage machine
            // (FUN_001253C0 wreck stamp -> FUN_00123000 detaches all panels;
            // validate_gameplay.py; RE_NOTES 13). Respawn = repaired.
            int wrecked = (v->crashed_until > 0.0f) && g_car_shell_lists[i];
            // CARFX: the recovered SH-irradiance body shine wraps the body
            // draw (docs/RE_CARFX.md; validate_carfx 99/99).
            B3CarFxBodyParams fxp;
            b3_carfx_body_defaults(&fxp);
            if (v->fsim_ready && !(v->crashed_until > 0.0f && g_wrecks[i].active)) {
                // Same full pose as the draw matrix above (row-major
                // object->world; columns right/up/-at, z-negated to harness).
                const float (*f)[4] = (const float (*)[4])v->fsim.rb.frame;
                fxp.rot3[0] =  f[0][0]; fxp.rot3[1] =  f[1][0]; fxp.rot3[2] = -f[2][0];
                fxp.rot3[3] =  f[0][1]; fxp.rot3[4] =  f[1][1]; fxp.rot3[5] = -f[2][1];
                fxp.rot3[6] = -f[0][2]; fxp.rot3[7] = -f[1][2]; fxp.rot3[8] =  f[2][2];
            } else if (v->crashed_until > 0.0f && g_wrecks[i].active) {
                // Same full pose as the WRECK draw matrix above. The wreck
                // frame is already in harness space, so only the at row is
                // negated (rows right/up/at -> columns right/up/-at), and the
                // rot3 the shader wants is that matrix's transpose.
                const B3WreckState* wk = &g_wrecks[i];
                fxp.rot3[0] = wk->frame[0][0]; fxp.rot3[1] = wk->frame[1][0];
                fxp.rot3[2] = -wk->frame[2][0];
                fxp.rot3[3] = wk->frame[0][1]; fxp.rot3[4] = wk->frame[1][1];
                fxp.rot3[5] = -wk->frame[2][1];
                fxp.rot3[6] = wk->frame[0][2]; fxp.rot3[7] = wk->frame[1][2];
                fxp.rot3[8] = -wk->frame[2][2];
            } else
            b3_carfx_rot3_from_yaw(v->rot.y, fxp.rot3);
            /* CARFX light probe: FUN_0019D400 samples the probe volume
             * at the car's own world position every frame, per car, and a
             * miss keeps that car's previous nine -- hence the slot. */
            fxp.slot = i;
            fxp.pos[0] = v->pos.x;
            fxp.pos[1] = v->pos.y;
            fxp.pos[2] = v->pos.z;
            fxp.paint_index = i % 8;
            fxp.has_normals = 1;   // car lists emit the .bgv vn now
            b3_carfx_body_begin(&fxp);
            if (wrecked) {
                /* PANELS: the wreck assembly is the mask-bit1 aperture body
                 * PLUS every panel that has not detached yet.  The retail
                 * draw path FUN_000303D0 places panel slot k+1 at
                 * (ctx+0x180 + k*0x40) x frame for state < 3 only
                 * (execution-traced, tools/trace_panels.py); the placement
                 * rotation is exactly identity in all 67 shipped cars, so the
                 * pivot translation is the whole transform.  z is negated
                 * because the mesh loader Z-flips every car mesh
                 * (RE_NOTES 12). */
                glCallList(g_car_shell_lists[i]);
                for (int pk = 0; pk < g_car_panel_count[i]; pk++) {
                    if (!g_car_panel_lists[i][pk]) continue;
                    if (!b3_panel_attached(&g_panels[i], pk)) continue;
                    glPushMatrix();
                    glTranslatef(g_car_panel_pos[i][pk][0],
                                 g_car_panel_pos[i][pk][1],
                                 -g_car_panel_pos[i][pk][2]);
                    glCallList(g_car_panel_lists[i][pk]);
                    glPopMatrix();
                }
            } else
                glCallList(g_car_intact_lists[i] ? g_car_intact_lists[i]
                                                 : g_car_lists[i]);
            b3_carfx_body_end();

            // Glass (mask bit8), always as a translucent pass over the body.
            // Wrecked tint 0.6 is the execution-verified FUN_000300A0
            // shattered value; the intact pale-blue alpha is harness styling
            // (the game's glass shader isn't recovered).
            if (g_car_glass_lists[i]) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                if (wrecked) glColor4f(0.35f, 0.40f, 0.45f, 0.60f);
                else         glColor4f(0.55f, 0.65f, 0.75f, 0.38f);
                b3_carfx_glass_begin(&fxp, wrecked ? 0.60f : 0.38f);
                glCallList(g_car_glass_lists[i]);
                b3_carfx_glass_end();
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }

            // Wheels: separate origin-centred .bgv meshes at the real attach
            // positions (file+0xB80 matrices [C]); right-side wheels are
            // mirrored 180 deg about Y exactly as their attach matrices'
            // negated Right/At rows encode. Since the pipeline switch the
            // spin rate is the REAL per-wheel omega from the full model's
            // wheel records (driven rear, wheel+0x5C) and the steer pose is
            // the pipeline's own steering angle (v+0x1164).
            if (g_car_wheel_lists[i] && g_car_wheel_count[i] > 0) {
                g_car_wheel_omega[i] = v->fsim_ready
                    ? v->fsim.wheel[3].omega
                    : v->sim.speed
                      / (g_car_wheel_radius[i] > 0.01f
                         ? g_car_wheel_radius[i] : 0.346f);
                b3_wheel_spin_update(&g_car_wheel_spin[i],
                                     &g_car_wheel_omega[i],
                                     /*decay_enabled=*/0, /*contact=*/1,
                                     g_delta_time);
                // sign: pipeline steer input is -harness steer (mirror), so
                // steer_deg_1164 = -(-steer * ang) = the old steer * ang
                float steer_deg = v->fsim_ready
                                ? v->fsim.steer_deg_1164
                                : b3_steer_angle(&v->cfg, v->sim.speed)
                                  * v->steer;
                float spin_deg = g_car_wheel_spin[i] * RAD_TO_DEG;
                for (int w = 0; w < g_car_wheel_count[i]; w++) {
                    glPushMatrix();
                    glTranslatef(g_car_wheel_pos[i][w][0],
                                 g_car_wheel_pos[i][w][1],
                                 g_car_wheel_pos[i][w][2]);
                    if (g_car_wheel_front[i][w])
                        glRotatef(-steer_deg, 0, 1, 0);
                    if (g_car_wheel_mirror[i][w] < 0)
                        glRotatef(180.0f, 0, 1, 0);
                    glRotatef(spin_deg * (g_car_wheel_mirror[i][w] < 0
                                          ? 1.0f : -1.0f), 1, 0, 0);
                    {   // blur variant by |spin rate| (25/50 rad/s [C])
                        float aw = fabsf(g_car_wheel_omega[i]);
                        GLuint wl = g_car_wheel_lists[i];
                        if (aw >= 50.0f && g_car_wheel_blur9[i])
                            wl = g_car_wheel_blur9[i];
                        else if (aw >= 25.0f && g_car_wheel_blur8[i])
                            wl = g_car_wheel_blur8[i];
                        glCallList(wl);
                    }
                    glPopMatrix();
                }
            }
        } else {
            glTranslatef(v->pos.x, v->pos.y, v->pos.z);
            glRotatef(v->rot.y * RAD_TO_DEG, 0, 1, 0);
            Color c = v == &g_player ? (Color){1, 0.2f, 0.2f, 1}
                                     : (Color){0.2f, 0.2f, 1, 1};
            glColor3f(c.r, c.g, c.b);
            glBegin(GL_QUADS);
            glVertex3f(-1.0f, 0.0f, 1.5f);
            glVertex3f(1.0f, 0.0f, 1.5f);
            glVertex3f(1.0f, 0.0f, -1.5f);
            glVertex3f(-1.0f, 0.0f, -1.5f);
            glVertex3f(-0.7f, 0.6f, 0.5f);
            glVertex3f(0.7f, 0.6f, 0.5f);
            glVertex3f(0.7f, 0.6f, -0.5f);
            glVertex3f(-0.7f, 0.6f, -0.5f);
            glEnd();
        }
        glPopMatrix();
    }

    /* PANELS: the detached panels, in flight.  Each piece carries its own
     * rigid pose (state 3, retail's flying-part pool record) so it is drawn
     * in WORLD space, outside the car's matrix.  The column mapping is the
     * wreck draw's: mesh nose = -Z, so rows right/up/at -> columns
     * right/up/-at. */
    for (int i = 0; i < g_num_vehicles; i++) {
        if (!g_vehicles[i].active) continue;
        const B3PanelSet* ps = &g_panels[i];
        for (int pk = 0; pk < ps->n; pk++) {
            const B3PanelPiece* pp = &ps->piece[pk];
            if (!pp->active || !g_car_panel_lists[i][pk]) continue;
            const float (*f)[4] = (const float (*)[4])pp->frame;
            float M[16] = {
                 f[0][0],  f[0][1],  f[0][2], 0,
                 f[1][0],  f[1][1],  f[1][2], 0,
                -f[2][0], -f[2][1], -f[2][2], 0,
                 f[3][0],  f[3][1],  f[3][2], 1,
            };
            B3CarFxBodyParams pfx;
            b3_carfx_body_defaults(&pfx);
            pfx.rot3[0] =  f[0][0]; pfx.rot3[1] =  f[1][0];
            pfx.rot3[2] = -f[2][0];
            pfx.rot3[3] =  f[0][1]; pfx.rot3[4] =  f[1][1];
            pfx.rot3[5] = -f[2][1];
            pfx.rot3[6] =  f[0][2]; pfx.rot3[7] =  f[1][2];
            pfx.rot3[8] = -f[2][2];
            pfx.slot = i;
            pfx.pos[0] = f[3][0];
            pfx.pos[1] = f[3][1];
            pfx.pos[2] = f[3][2];
            pfx.paint_index = i % 8;
            pfx.has_normals = 1;
            glPushMatrix();
            glMultMatrixf(M);
            b3_carfx_body_begin(&pfx);
            glCallList(g_car_panel_lists[i][pk]);
            b3_carfx_body_end();
            glPopMatrix();
        }
    }

    /* CARFX: one blended shadow pass over all cars, then the coronas --
     * the game's own order (FUN_001AE340 draws every car, THEN the single
     * shadow pass FUN_0019A580 @0x001AE4BE). air_h 0 = sitting down;
     * ambient HEAD|TAIL = running lights (carObj+0x18FF writer is [?],
     * daylight references show them on). */
    b3_carfx_shadow_pass_begin();
    for (int i = 0; i < g_num_vehicles; i++) {
        Vehicle* v = &g_vehicles[i];
        if (!v->active || !g_car_lists[i]) continue;
        float p[3] = { v->pos.x, v->pos.y, v->pos.z };
        // Probe the real surface under the car so the blob rests ON the
        // slope plane instead of a flat quad slicing the upslope (dump 030).
        float gy = v->pos.y - 0.5f, gn[3] = { 0.0f, 1.0f, 0.0f };
        float gh;
        if (b3_collision_ready()
            && b3_ground_probe(v->pos.x, v->pos.y, v->pos.z, &gh, gn) >= 0)
            gy = gh;
        b3_carfx_shadow_draw_n(i, p, v->rot.y, gy, 1.0f, 0.0f, gn);
    }
    b3_carfx_shadow_pass_end();

    b3_carfx_corona_pass_begin();
    for (int i = 0; i < g_num_vehicles; i++) {
        Vehicle* v = &g_vehicles[i];
        if (!v->active || !g_car_lists[i]) continue;
        // FULL POSE. The lamp offsets are model-space points on the bodywork,
        // so they must ride the SAME rigid pose the body draws with -- yaw
        // only tore the emitters off the car on any pitch or roll, and off a
        // tumbling wreck entirely (debug dump 018: both tail coronas hanging
        // in mid air beside the car). Same three-way selection as the draw
        // matrix: wreck frame, else the rigid body's frame, else yaw.
        float p[3] = { v->pos.x, v->pos.y - 0.5f - g_car_ymin[i], v->pos.z };
        float R[9];
        if (v->crashed_until > 0.0f && g_wrecks[i].active) {
            const B3WreckState* wk = &g_wrecks[i];
            R[0] = wk->frame[0][0]; R[1] = wk->frame[1][0];
            R[2] = -wk->frame[2][0];
            R[3] = wk->frame[0][1]; R[4] = wk->frame[1][1];
            R[5] = -wk->frame[2][1];
            R[6] = wk->frame[0][2]; R[7] = wk->frame[1][2];
            R[8] = -wk->frame[2][2];
            // the wreck body draws AT its own frame's pos row, not v->pos
            p[0] = wk->frame[3][0];
            p[1] = wk->frame[3][1];
            p[2] = wk->frame[3][2];
        } else if (v->fsim_ready) {
            const float (*f)[4] = (const float (*)[4])v->fsim.rb.frame;
            R[0] =  f[0][0]; R[1] =  f[1][0]; R[2] = -f[2][0];
            R[3] =  f[0][1]; R[4] =  f[1][1]; R[5] = -f[2][1];
            R[6] = -f[0][2]; R[7] = -f[1][2]; R[8] =  f[2][2];
        } else {
            b3_carfx_rot3_from_yaw(v->rot.y, R);
        }
        unsigned lb = b3_carfx_light_byte(v->last_brake > 0.05f, 0,
                                          B3_CARFX_LIGHT_HEAD
                                          | B3_CARFX_LIGHT_TAIL);
        b3_carfx_corona_draw_pose(i, p, R, lb);
    }
    b3_carfx_corona_pass_end();

    /* BOOSTFX: the boost exhaust flame.  FUN_0017F730 emits it in the
     * same per-car FX step as the coronas (0x0017F73C), from the SAME
     * light table -- type 8, the tailpipes -- into sprite pools 1/2
     * (`coronaboost` / `coronaboostred`).  Additive, ZWRITE off, same
     * render origin as the corona pass.  red = -1 uses the RECOVERED
     * per-car-model carObj+0x1901 (FUN_0018D0E0 @0x0018D4CB..D534: the
     * five Car10 specials burn orange, everyone else blue). */
    b3_boostfx_pass_begin();
    for (int i = 0; i < g_num_vehicles; i++) {
        Vehicle* v = &g_vehicles[i];
        if (!v->active || !g_car_lists[i]) continue;
        float p[3] = { v->pos.x, v->pos.y - 0.5f - g_car_ymin[i], v->pos.z };
        // Full pose so the flame stays ON the tailpipes on hills (crashed
        // cars have no flame -- carObj+0x18FA gate -- so no wreck branch).
        float R[9];
        if (v->fsim_ready) {
            const float (*f)[4] = (const float (*)[4])v->fsim.rb.frame;
            R[0] =  f[0][0]; R[1] =  f[1][0]; R[2] = -f[2][0];
            R[3] =  f[0][1]; R[4] =  f[1][1]; R[5] = -f[2][1];
            R[6] = -f[0][2]; R[7] = -f[1][2]; R[8] =  f[2][2];
        } else {
            b3_carfx_rot3_from_yaw(v->rot.y, R);
        }
        b3_boostfx_draw_pose(i, p, R, -1);
    }
    b3_boostfx_pass_end();

    /* CRASH-SHOW H8: the particle layer draws after the opaque world and
     * the boost flames, before the traffic pass and the HUD. */
    b3_pfx_update(g_tdfx_real_dt);
    b3_pfx_draw();

    /* CRASH-SHOW H6: the per-wheel particle FX -- tyre smoke on
     * slip, plus the SURFACE-keyed dust/gravel/snow emitter from the
     * recovered 0x003A3BF8 table.  The surface id is the collision
     * triangle's own type, which is exactly what b3_ground_probe
     * returns, so the table lookup is the game's. */
    for (int wi = 0; wi < g_num_vehicles; wi++) {
        Vehicle* wv = &g_vehicles[wi];
        float wpos[3], wvel[3], gh, gn[3], slip, dist;
        int surf;
        if (!wv->active || wv->crashed_until > 0.0f) continue;
        if (wv->sim.speed < 2.0f) continue;
        wpos[0] = wv->pos.x; wpos[1] = wv->pos.y - 0.45f;
        wpos[2] = wv->pos.z;
        wvel[0] = wv->vel.x; wvel[1] = 0.0f; wvel[2] = wv->vel.z;
        surf = b3_collision_ready()
             ? b3_ground_probe(wpos[0], wv->pos.y, wpos[2], &gh, gn)
             : -1;
        /* the recovered 0x003A3BF8 table is indexed by the surface
         * type's LOW BYTE -- the static track polys carry 0x01..0x25,
         * exactly the table's 40-row range (docs note in
         * burnout3_collision.h). */
        if (surf >= 0) { wpos[1] = gh + 0.10f; surf &= 0xFF; }
        /* The gates themselves are RECOVERED now (the surface row's
         * scale/skid/gravelness, FUN_001807C0 @0x0018093B) -- all the
         * harness still owes the module is the skid_flag, whose own
         * source (the wheel state machine at wheel+0x78) is not in this
         * port, so the drift angle the tyre stage reports stands in for
         * it.                                                  [S] GLUE */
        slip = fabsf(wv->sim.drift_angle) * (1.0f / 22.0f);
        if (slip > 1.0f) slip = 1.0f;
        dist = wv->sim.speed * g_delta_time;
        /* retail runs this PER WHEEL over veh[0x1169] wheels, and each
         * wheel sweeps the WHOLE frame travel [C]; the player gets all
         * four corners and the rivals a single body point.  Wheels 2/3
         * are the rear pair -- the only ones retail lets emit the
         * surface row's own dust/gravel/snow. */
        if (wi == 0) {
            float cs = cosf(wv->rot.y), sn = sinf(wv->rot.y);
            static const float WHEEL[4][2] = {
                {-0.78f, 1.25f}, {0.78f, 1.25f},
                {-0.78f, -1.30f}, {0.78f, -1.30f} };
            for (int wq = 0; wq < 4; wq++) {
                float cw[3];
                cw[0] = wpos[0] + WHEEL[wq][0]*cs + WHEEL[wq][1]*sn;
                cw[1] = wpos[1];
                cw[2] = wpos[2] - WHEEL[wq][0]*sn + WHEEL[wq][1]*cs;
                b3_pfx_wheel(b3_pfx_wheel_slot(wi, wq), cw, wvel, dist,
                             surf, slip, 1.0f, wq >= 2);
            }
        } else {
            b3_pfx_wheel(b3_pfx_wheel_slot(wi & 5, 2), wpos, wvel, dist,
                         surf, slip, 0.5f, 1);
        }
    }

    // Traffic vehicles (real .btv meshes on the oncoming line; own section).
    traffic_render();

    /* POSTFX: radial speed blur over the finished world, before the HUD
     * (the HUD must stay sharp). Shape is the recovered per-screen radial
     * zoom; the strength ramp is GLUE (see burnout3_postfx.h). boost_ramp
     * = racecar+0x11AC; harness has none yet -> 0. */
    b3_postfx_blur(w, h, g_player.sim.speed * 2.2374146f, 0.0f,
                   g_tdfx_real_dt);

    // HUD: the real game HUD (XBE-embedded fonts, Global.txd art,
    // Globalus.bin labels -- src/burnout3_hud.c, docs/RE_FRONTEND.md),
    // driven by the verified sim/meter/score values. The old 7-segment
    // harness overlay is gone; everything drawn is the game's own art.
    glFrontFace(GL_CW);   // leave the mirrored-scene winding (HUD is CW)
    glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Speed in mph, using the game's own (slightly off) conversion constant.
    int mph = (int)(g_player.sim.speed * 2.2374146f + 0.5f);
    if (mph > 999) mph = 999;

    int pos = 1;
    for (int i = 1; i < g_num_vehicles; i++) {
        // Higher lap, or same lap and further along, ranks ahead of the player.
        if (g_vehicles[i].lap > g_player.lap ||
            (g_vehicles[i].lap == g_player.lap &&
             g_vehicles[i].track_progress > g_player.track_progress)) pos++;
    }

    {
        // The HUD consumes the boost record directly: the recovered
        // FUN_0004C390 state machine needs tier / size / meter / lifetime
        // earned / min units / boosting, not a single fraction.
        B3HudState hs;
        memset(&hs, 0, sizeof(hs));
        hs.mph         = (float)mph;
        hs.lap         = g_current_lap + 1;
        hs.total_laps  = g_lap_count;
        hs.position    = pos;
        hs.n_cars      = g_num_vehicles;
        hs.boost.tier      = g_player.bar.tier;
        hs.boost.bar_size  = g_player.bar.size;
        hs.boost.meter     = g_player.bar.meter;
        hs.boost.earned    = g_player.bar.earned;
        hs.boost.min_units = g_player.bar.min_units;
        hs.boost.boosting  = g_player.bar.boosting;
        // ---- EVENT TICKER (docs/RE_FRONTEND.md 6.8) --------------------
        // The lower-left rows of earn labels + star pips belong to the
        // same element as the boost bar; feed each category's live record
        // + its active byte and the race clock (near-miss pip spin).
        // AIR's slot exists but retail never probes it -- not fed.
        hs.race_clock = g_race_time;
        {
            const B3ScoreEvents* se = &g_player.sev;
            const struct { const B3CatRecord* r; int open; int row; } tk[] = {
                { &se->onc,   se->onc_active,   B3_HUD_TICK_ONCOMING },
                { &se->drift, se->drift_active, B3_HUD_TICK_DRIFT    },
                { &se->nm,    se->nm_active,    B3_HUD_TICK_NEARMISS }
            };
            for (unsigned t = 0; t < sizeof(tk) / sizeof(tk[0]); t++) {
                B3HudTickIn* d = &hs.ticker[tk[t].row];
                d->open       = tk[t].open;
                d->value      = tk[t].r->value;
                d->prev_value = tk[t].r->prev_value;
                d->clock      = tk[t].r->clock;
                d->tier       = tk[t].r->tier;
                d->prev_tier  = tk[t].r->prev_tier;
                d->count      = tk[t].r->count;
            }
        }
        // TAKEDOWN-FX callout: the module runs FUN_001994D0/FUN_00199350's
        // trigger chain; the label is the real Globalus.bin string and the
        // roundel the real hud_sign texture. Life = the recovered
        // slam-in + hold + fly-out timeline (4.25 s).
        hs.callout.t = -1.0f;
        // MUSIC: the EA TRAX now-playing banner (RE_FRONTEND 6.9)
        { const char *mar, *mti; float mel;
          if (b3_music_now_playing(&mar, &mti, &mel)) {
              hs.music.artist  = mar;
              hs.music.title   = mti;
              hs.music.elapsed = mel; } }
        // Boost EARN callout ("GREAT NEAR MISS!"...): the closing
        // event with the highest priority id claims the slot.
        { int ecat, etier;
          if (b3_score_events_take_callout(&g_player.sev, &ecat, &etier))
              b3_hud_boost_event(ecat, etier); }
        B3TdfxStatus ts;
        b3_tdfx_status(&ts);
        if (ts.callout_visible && ts.callout_string >= 0) {
            hs.callout.label = globalus_string(ts.callout_string);
            hs.callout.art   = tdfx_sign_texture(ts.callout_sign);
            hs.callout.t     = ts.callout_age;
            hs.callout.life  = B3_TDFX_ANIM1_IN + B3_TDFX_HOLD1_LONG
                             + B3_TDFX_ANIM1_OUT;
            hs.callout.id    = ts.callout_msg;
        }
        // HUD animation runs at presentation rate: real dt, not the
        // dilated sim dt (the callout timeline is wall-clock in retail).
        /* CRASH-SHOW H3: the crash presentation feed.  All-zero is
         * inert, so this only lights up while the player is a wreck.
         * The HUD module owns the whole descriptor state machine; the
         * harness just hands over the wreck's pose.  The prompt's
         * `input` is the live aftertouch steer, which is what
         * FUN_00051230 reads (car+0x84) to hide the (A) glyph. */
        {
            const B3WreckState* wk = &g_wrecks[0];
            int crashed = (g_player.crashed_until > 0.0f);
            hs.crash.active     = crashed;
            hs.crash.aftertouch = crashed;
            hs.crash.input =
                (g_keys[SDL_SCANCODE_A] || g_keys[SDL_SCANCODE_LEFT] ||
                 g_keys[SDL_SCANCODE_D] || g_keys[SDL_SCANCODE_RIGHT] ||
                 g_keys[SDL_SCANCODE_LSHIFT] ||
                 pad_axis(SDL_CONTROLLER_AXIS_LEFTX, 0.15f) != 0.0f ||
                 pad_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT) ||
                 pad_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) ? 1.0f : 0.0f;
            /* CRASH-CINEMA: the AFTERTOUCH ARROW CURSOR.  The element
             * swaps the (A) glyph for the 54x36 arrow box the moment
             * pad+0x84 (boost) is down -- FUN_00051230 @0x00051412 --
             * and FUN_0004FCA0 lights its four wedges from the two
             * aftertouch axes. */
            hs.crash.impact_held = crashed && g_at_held;
            hs.crash.steer_h     = crashed ? g_at_h : 0.0f;
            hs.crash.steer_v     = crashed ? g_at_v : 0.0f;
            hs.crash.hit_kind = g_crash_hit_kind;
            hs.crash.hit_id   = g_crash_hit_id;
            if (wk->active) {
                hs.crash.up_y     = wk->frame[1][1];
                hs.crash.right_y  = wk->frame[0][1];
                hs.crash.fwd_y    = wk->frame[2][1];
                hs.crash.speed_ms = wk->vel[3];
                hs.crash.airborne = wk->airborne;
            }
            hs.crash.hit_wall =
                (g_player.last_hit_time > 0.0f &&
                 g_race_time - g_player.last_hit_time < 0.20f);
        }
        b3_hud_draw_state(&hs, g_tdfx_real_dt);

        // OPPONENT TAGS (docs/RE_FRONTEND.md 6.10): the ordinal floats over
        // the nearby rival, a yellow triangle over distant ones -- the
        // switch/size/fades are all inside the module. Project each rival's
        // roof point through this frame's matrices; the display mirror
        // (projection X flip) is applied to screen_x here. Far-to-near
        // matches retail's depth-sorted order (0x0018EB40).
        {
            struct { float sx, sy, z; int place, ok; } tags[8];
            int order[8], tn = 0;
            for (int oi = 1; oi < g_num_vehicles && oi < 8; oi++) {
                Vehicle* ov = &g_vehicles[oi];
                if (!ov->active || ov->crashed_until > 0.0f) continue;
                float wx = ov->pos.x;
                float wy = ov->pos.y - 0.5f - g_car_ymin[oi]
                         + g_car_ext[oi][1] * 2.0f;   // roof point
                float wz = ov->pos.z;
                const float (*V)[4] = g_cam_view.m;
                float ex = wx*V[0][0] + wy*V[1][0] + wz*V[2][0] + V[3][0];
                float ey = wx*V[0][1] + wy*V[1][1] + wz*V[2][1] + V[3][1];
                float ez = wx*V[0][2] + wy*V[1][2] + wz*V[2][2] + V[3][2];
                if (ez > -0.1f) continue;             // behind the camera
                const float (*P)[4] = g_cam_proj.m;
                float cx = ex*P[0][0] + ey*P[1][0] + ez*P[2][0] + P[3][0];
                float cy = ex*P[0][1] + ey*P[1][1] + ez*P[2][1] + P[3][1];
                float cw = ex*P[0][3] + ey*P[1][3] + ez*P[2][3] + P[3][3];
                if (cw <= 1e-6f) continue;
                // display mirror: the on-screen x is the flip of clip x
                float sx = (0.5f - 0.5f * cx / cw) * 640.0f;
                float sy = (0.5f - 0.5f * cy / cw) * 480.0f;
                int place = 1;
                for (int j = 0; j < g_num_vehicles; j++) {
                    if (j == oi || !g_vehicles[j].active) continue;
                    if (g_vehicles[j].lap > ov->lap ||
                        (g_vehicles[j].lap == ov->lap &&
                         g_vehicles[j].track_progress > ov->track_progress))
                        place++;
                }
                tags[tn].sx = sx; tags[tn].sy = sy; tags[tn].z = -ez;
                tags[tn].place = place; tags[tn].ok = 1;
                order[tn] = tn;
                tn++;
            }
            for (int a = 0; a < tn; a++)          // far-to-near
                for (int b = a + 1; b < tn; b++)
                    if (tags[order[b]].z > tags[order[a]].z) {
                        int t = order[a]; order[a] = order[b]; order[b] = t;
                    }
            for (int a = 0; a < tn; a++) {
                const int k = order[a];
                b3_hud_opponent_tag(tags[k].sx, tags[k].sy, tags[k].z,
                                    tags[k].place, tags[k].ok);
            }
        }
    }

    glEnable(GL_DEPTH_TEST);

    /* POSTFX: the retail output gamma ramp, LAST -- after the HUD, before
     * every glReadPixels capture and before SDL_GL_SwapWindow, so the live
     * window and the captures are the same pixels. Returns 0 when GLSL is
     * unavailable, in which case the capture paths fall back to the software
     * table (they are mutually exclusive -- see b3_gamma_apply_rgba). */
    if (g_paused) {   // pause mixer overlay (under the gamma pass)
        float fr[3] = { g_mix[0] / g_mix_max[0], g_mix[1] / g_mix_max[1],
                        g_mix[2] / g_mix_max[2] };
        b3_hud_pause_mixer(fr);
    }
    b3_gamma_in_gl = b3_postfx_gamma(w, h);
}

// ============================================================
// Input handling (original)
// ============================================================

static void process_input(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            g_running = 0;
            return;
        }

        // Controller hotplug + Start = the R (restart) key.
        if (e.type == SDL_CONTROLLERDEVICEADDED) pad_open_first();
        if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad
            && e.cdevice.which == SDL_JoystickInstanceID(
                       SDL_GameControllerGetJoystick(g_pad))) {
            SDL_GameControllerClose(g_pad);
            g_pad = NULL;
            pad_open_first();
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN
            && e.cbutton.button == SDL_CONTROLLER_BUTTON_START
            && g_state == RACING) {
            g_paused = !g_paused;
            if (!g_paused) { mixer_save(); }
        } else if (e.type == SDL_CONTROLLERBUTTONDOWN
            && e.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
            SDL_Event ke;
            SDL_zero(ke);
            ke.type = SDL_KEYDOWN;
            ke.key.keysym.scancode = SDL_SCANCODE_R;
            SDL_PushEvent(&ke);
        }

        // Pause-screen mixer: mouse drag on the sliders.
        if (g_paused && e.type == SDL_MOUSEBUTTONDOWN
            && e.button.button == SDL_BUTTON_LEFT) {
            float f;
            int row = mixer_hit(e.button.x, e.button.y, &f);
            if (row >= 0) {
                g_mix_drag = row;
                g_mix[row] = f * g_mix_max[row];
                mixer_apply();
            } else {
                int ww, wh;
                SDL_GetWindowSize(g_window, &ww, &wh);
                if (ww > 0 && wh > 0) {
                    float vx = (float)e.button.x * 640.0f / (float)ww;
                    float vy = (float)e.button.y * 480.0f / (float)wh;
                    if (vx >= B3HUD_MIX_BTN_X
                        && vx <= B3HUD_MIX_BTN_X + B3HUD_MIX_BTN_W
                        && vy >= B3HUD_MIX_BTN_Y
                        && vy <= B3HUD_MIX_BTN_Y + B3HUD_MIX_BTN_H) {
                        g_paused = 0;
                        mixer_save();
                        race_restart();
                        printf("[Burnout3] Restarting race (pause menu)\n");
                    }
                }
            }
        }
        if (e.type == SDL_MOUSEMOTION && g_paused && g_mix_drag >= 0) {
            float f;
            int row = mixer_hit(e.motion.x, e.motion.y, &f);
            (void)row;                     /* stay on the grabbed slider */
            float vx = 0.0f;
            {
                int ww, wh;
                SDL_GetWindowSize(g_window, &ww, &wh);
                if (ww > 0)
                    vx = (float)e.motion.x * 640.0f / (float)ww;
            }
            f = (vx - B3HUD_MIX_BAR_X) / B3HUD_MIX_W;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            g_mix[g_mix_drag] = f * g_mix_max[g_mix_drag];
            mixer_apply();
        }
        if (e.type == SDL_MOUSEBUTTONUP && g_mix_drag >= 0) {
            g_mix_drag = -1;
            mixer_save();
        }

        if (e.type == SDL_KEYDOWN) {
            g_keys[e.key.keysym.scancode] = 1;

            // Pause toggle (Escape or P) with the mixer overlay.
            if ((e.key.keysym.scancode == SDL_SCANCODE_ESCAPE
                 || e.key.keysym.scancode == SDL_SCANCODE_P)
                && g_state == RACING) {
                g_paused = !g_paused;
                if (!g_paused) mixer_save();
            }

            // T: dump the full gamestate + a screenshot to build/debug/
            // (user-facing debug handoff; see debug_dump()).
            if (e.key.keysym.scancode == SDL_SCANCODE_T)
                g_debug_dump_req = 1;

            // Game state transitions
            if (g_state == MENU && (e.key.keysym.scancode == SDL_SCANCODE_RETURN ||
                                    e.key.keysym.scancode == SDL_SCANCODE_SPACE)) {
                g_state = RACING;
                g_current_lap = 0;
                g_race_time = 0;
                printf("[Burnout3] Race started!\n");
            }
            
            if (g_state == CRASHED && e.key.keysym.scancode == SDL_SCANCODE_R) {
                race_restart();
                printf("[Burnout3] Restarting...\n");
            }
            
            if (g_state == FINISHED && e.key.keysym.scancode == SDL_SCANCODE_R) {
                race_restart();
                printf("[Burnout3] Restarting race\n");
            }
        }
        
        if (e.type == SDL_KEYUP) {
            g_keys[e.key.keysym.scancode] = 0;
        }
#ifdef __ANDROID__
        /* ANDROID PORT: on-screen buttons (gas / brake / boost) + tilt
         * steering live in android/.../b3_touch.c; every event is fed
         * through and the merged state is read in the input gather. */
        b3_touch_event(&e);
#endif
    }
}

static void race_restart(void) {
    b3_tdfx_event_reset();  // FUN_00025AB0: new event, new credit
    b3_props_reset();       // PROPS: every prop back to its authored pose
    g_state = RACING;
    g_current_lap = 0;
    g_race_time = 0;
    for (int i = 0; i < g_num_vehicles; i++) {
        g_vehicles[i].active = 1;
        g_vehicles[i].health = 100;
        g_vehicles[i].sim = (B3VehicleState){ .gear = 2,
                      .rpm = g_vehicles[i].cfg.idle_rpm };
        g_vehicles[i].fsim_ready = 0;
        reset_gameplay_state(&g_vehicles[i]);
        spawn_on_grid(&g_vehicles[i], i);
    }
}

// ============================================================
// Game logic (original)
// ============================================================


// ============================================================
// CAR-VS-CAR -- integration of the recovered chain (src/burnout3_carcol.c,
// docs/RE_CARCOL.md; differential suite tools/validate_carcol.py 730/730).
//
// A racing car's B3RigidBody already lives in GAME space, which is exactly
// the space the module works in, so an un-crashed racer is passed straight
// through and the response lands in its real accumulators (+0x130 push-out,
// +0x110/+0x120 impulse, +0xF0/+0x100 force) for the next
// b3_vehicle_step_full to consume -- the game's own frame order
// (FUN_00110AF0 once per frame, then FUN_0011BE50's two substeps).
//
// Cars currently driven by the wreck sim have no such body in the harness,
// so they are synthesised from their pose for the collision pass. Traffic
// carries a persistent B3RigidBody and consumes the collision accumulators
// through the shared integrator.
// ============================================================
typedef struct {
    B3CarBody   body;
    B3RigidBody rb;          // used only for synthesised bodies
    Vehicle*    veh;         // racer source, or NULL
    int         traffic;     // traffic index, or -1
    int         synth;       // 1 = write the result back by hand
} CarColEntry;

static Vec3 g_carcol_knock[8];        // racer wrecks (GLUE)
/* DRIVE-LOG: the last car/traffic contact each racer had, so a log frame
 * that loses speed with both wall arms silent still identifies what it
 * hit and what the crash gate measured on it. */
typedef struct { float t, vn, impact, other_mph; int other, is_traffic, crash_a, crash_b; } B3CarcolDbg;
/* "<CLASS>/<CarN>" for a racer, so tools/replay_pair.py can load the same
 * hull retail would.  info->file is the pveh source name ("Car1.bgv"). */
static const char* carcol_dump_id(const Vehicle* v) {
    static char buf[64];
    const char* cls = (v->info && v->info->class_code) ? v->info->class_code : "COMP";
    const char* f   = (v->info && v->info->file) ? v->info->file : "Car1.bgv";
    size_t n = strcspn(f, ".");
    if (n >= sizeof buf - 16) n = sizeof buf - 16;
    snprintf(buf, sizeof buf, "%s/%.*s", cls, (int)n, f);
    return buf;
}
static B3CarcolDbg g_carcol_dbg[8];

static void carcol_synth_rb(B3RigidBody* rb, Vec3 pos_gl, float y_origin,
                            float yaw, Vec3 vel_gl) {
    memset(rb, 0, sizeof(*rb));
    float cy = cosf(yaw), sy = sinf(yaw);
    rb->frame[0][0] =  cy; rb->frame[0][2] = -sy;   // right
    rb->frame[1][1] =  1.0f;                        // up
    rb->frame[2][0] =  sy; rb->frame[2][2] =  cy;   // at  (GAME space)
    rb->frame[3][0] = pos_gl.x;
    rb->frame[3][1] = y_origin;
    rb->frame[3][2] = -pos_gl.z;
    rb->frame[3][3] = 1.0f;
    rb->vel[0] = vel_gl.x;
    rb->vel[1] = vel_gl.y;
    rb->vel[2] = -vel_gl.z;
    rb->vel[3] = sqrtf(rb->vel[0]*rb->vel[0] + rb->vel[2]*rb->vel[2]);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) rb->inv_frame[i][j] = rb->frame[i][j];
    { float tt, (*m)[4] = rb->inv_frame, p[4];
      tt = m[0][1]; m[0][1] = m[1][0]; m[1][0] = tt;
      tt = m[0][2]; m[0][2] = m[2][0]; m[2][0] = tt;
      tt = m[1][2]; m[1][2] = m[2][1]; m[2][1] = tt;
      for (int c = 0; c < 4; c++)
          p[c] = m[3][0]*m[0][c] + m[3][1]*m[1][c] + m[3][2]*m[2][c];
      for (int c = 0; c < 4; c++) m[3][c] = -p[c]; }
    rb->inv_inertia_body[0][0] = 0.0008f;
    rb->inv_inertia_body[1][1] = 0.0011f;
    rb->inv_inertia_body[2][2] = 0.0013f;
    rb->inv_inertia_world[0][0] = 0.0008f;
    rb->inv_inertia_world[1][1] = 0.0011f;
    rb->inv_inertia_world[2][2] = 0.0013f;
}

static void carcol_fill_racer(CarColEntry* e, Vehicle* v, int slot) {
    B3CarBody* b = &e->body;
    memset(b, 0, sizeof(*b));
    e->veh = v; e->traffic = -1;
    b->hull = &g_car_hull[slot];
    b->mass = v->fsim.mass;
    for (int k = 0; k < 4; k++) {
        b->bbmax[k] = v->fsim.half_ext[k];    // +0x1D0 (.bgv +0xE80)
        b->bbmin[k] = v->fsim.center_off[k];  // +0x1E0 (.bgv +0xE90)
    }
    b->type = B3_COL_TYPE_RACER;
    if (v->crashed_until > 0.0f) {
        e->synth = 1;                 // driven by the wreck sim
        b->crashed = 1;               // -> FUN_00113960's path
        carcol_synth_rb(&e->rb, v->pos, v->pos.y - 0.5f - g_car_ymin[slot],
                        v->rot.y, v->vel);
        b->rb = &e->rb;
    } else {
        e->synth = 0;
        b->rb = &v->fsim.rb;
        b->drift_state = v->fsim.drift_state_1524;
        b->yaw_input   = v->fsim.steer_1408;
    }
}


// ===========================================================================
// EARN EVENTS -- the state the score module's detectors consume.
//
// racecar+0x18FC ("driving on the wrong side of the road") is set by
// FUN_0018D790 @0x0018D89F from a 3-BIT LANE-TYPE FIELD on the road-network
// segment under the car:  flag = ((seg_record[3] & 7) == 1 || == 3).
// GLUE: the harness's road representation is the two recovered drive lines,
// not the per-segment lane table, so the same decision is mirrored as "closer
// to the reverse-direction line (0x1A8540, 926 pts) than to the forward race
// line".  Everything downstream of this flag is the game's own rule.
static int score_oncoming_flag(const Vehicle* v) {
    float d_fwd = loop_closest(g_cl, ROUTE_COUNT, v->pos.x, v->pos.z,
                               NULL, NULL);
    float d_onc = loop_closest(B3_ONCOMING, B3_ONCOMING_COUNT,
                               v->pos.x, v->pos.z, NULL, NULL);
    return d_onc < d_fwd;
}

// racecar+0x10C0: airborne.  The harness reads it off the ported per-wheel
// contact state rather than the (unlocated) writer of that byte.
static int score_airborne_flag(const Vehicle* v) {
    return !(v->fsim.wheel[0].contact || v->fsim.wheel[1].contact
             || v->fsim.wheel[2].contact || v->fsim.wheel[3].contact);
}

// One frame of AIR + ONCOMING + DRIFT for a car (FUN_001935F0's own order).
/* SCORE-CLASSIFIER helpers -------------------------------------------- *
 * Retail keeps ONE id space for the near-miss slots and the collision
 * notifies (object+0x177).  The harness's traffic ids are pool slots 0..63,
 * so racers take 100..107 -- disjoint, and inside the signed-byte range the
 * slots (score+0x3E8, s8) can hold. */
#define B3_NM_RACER_ID(slot) (100 + (slot))

/* Retail's SHUNT-SUPPRESSION verdict for FUN_00194A80 (0x00194B73..
 * 0x00194C22): a contact that belongs to a slam inside the last "Maximum
 * Crash Wait Time" second is a takedown attempt, not rubbing.
 *
 *   recent(t) := t >= 0 && now <= t + 1.0
 *   slam[j] = ( other.aggressor(+0x16BC) == me && recent(other.+0x16C0)
 *               && ( recent(other'.slam_time(+0x1598))
 *                    || recent(other'.+0x1690) ) )
 *          || ( score+0x5EC == other && recent(score+0x5F0)
 *               && ( recent(my.+0x1598) || recent(my.+0x1690) ) )
 *
 * GLUE: score+0x5EC/+0x5F0 ("the car I slammed, and when") has no td_rules
 * mirror, so the second clause reuses the victim-side record that the same
 * event writes -- FUN_00197EA0 sets victim+0x16BC = attacker at the moment
 * FUN_00197840 sets attacker's score+0x5EC = victim.  racecar+0x1690 is
 * still [?] and is simply absent, which can only make the suppression less
 * eager, never more. */
static void score_rub_slam(int self, unsigned char* slam) {
    for (int j = 0; j < B3_SE_RUB_CARS; j++) {
        slam[j] = 0;
        if (j == self || j >= g_num_vehicles || j >= B3_TDR_MAX_CARS) continue;
        if (self < 0 || self >= B3_TDR_MAX_CARS) continue;
        const B3TdCar* o = &g_tdr.car[j];
        const B3TdCar* m = &g_tdr.car[self];
        const float W = b3_score_params.crash_wait_s;   /* 0x3F7404 = 1.0 */
        int blames_me = (o->aggressor == self && o->aggressor_time >= 0.0f
                         && g_race_time <= o->aggressor_time + W);
        int recent_o = (o->slam_time >= 0.0f
                        && g_race_time <= o->slam_time + W);
        int recent_m = (m->slam_time >= 0.0f
                        && g_race_time <= m->slam_time + W);
        slam[j] = (unsigned char)(blames_me && (recent_o || recent_m));
    }
}

/* One car-on-car contact, both directions -- the dispatcher's class-1 case
 * (0x00027525) calls FUN_001979E0 for A-vs-B and B-vs-A, and the generic
 * notify FUN_00197920 goes through the 0x000273D0 thunk. */
static void score_contact_pair(Vehicle* a, Vehicle* b, int a_traffic,
                               int b_traffic) {
    int a_slot = a ? (int)(a - g_vehicles) : -1;
    int b_slot = b ? (int)(b - g_vehicles) : -1;
    if (a) {
        int other = b ? B3_NM_RACER_ID(b_slot) : b_traffic;
        if (other >= 0) b3_score_events_contact(&a->sev, other, g_race_time);
        if (b) b3_score_events_mark_contact(&a->sev, b_slot, g_race_time);
    }
    if (b) {
        int other = a ? B3_NM_RACER_ID(a_slot) : a_traffic;
        if (other >= 0) b3_score_events_contact(&b->sev, other, g_race_time);
        if (a) b3_score_events_mark_contact(&b->sev, a_slot, g_race_time);
    }
}

static void score_events_update(Vehicle* v) {
    B3ScoreFrame f;
    float dx = v->pos.x - v->sev_prev_pos.x;
    float dy = v->pos.y - v->sev_prev_pos.y;
    float dz = v->pos.z - v->sev_prev_pos.z;
    f.dist_step = v->sev_prev_ok ? sqrtf(dx * dx + dy * dy + dz * dz) : 0.0f;
    v->sev_prev_pos = v->pos;
    v->sev_prev_ok = 1;
    f.clock     = g_race_time;
    // the SCORING side uses the true m/s->mph constant, not the physics one
    f.speed_mph = v->sim.speed * B3_SE_MPH_PER_MS;
    f.airborne  = score_airborne_flag(v);
    f.oncoming  = score_oncoming_flag(v);
    f.drifting  = v->fsim.drift_state_1524 != 0;
    b3_score_events_frame(&v->sev, &v->bar, &f);
}

// Fill an OBB exactly as FUN_00195DD0 reads one (GAME space).
static void score_obb_from_frame(B3ScoreObb* o, const float frame[4][4],
                                 const float bmax[4], const float bmin[4]) {
    int r, c;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) o->m[r][c] = frame[r][c];
    for (c = 0; c < 4; c++) { o->bmax[c] = bmax[c]; o->bmin[c] = bmin[c]; }
}

// NEAR MISS for one racer against the traffic set.  In the game the candidate
// list is an 8-entry-per-car broadphase (DAT_00649A8E); the harness passes its
// traffic cars directly -- GLUE at the broadphase only.
static void score_near_miss_update(Vehicle* v) {
    B3ScoreObb me, others[B3_TRAFFIC_N];
    int ids[B3_TRAFFIC_N], n = 0, i;
    score_obb_from_frame(&me, (const float (*)[4])v->fsim.rb.frame,
                         v->fsim.half_ext, v->fsim.center_off);
    for (i = 0; i < g_traffic_n && n < B3_TRAFFIC_N; i++) {
        TrafficCar* t = &g_traffic[i];
        B3RigidBody rb;
        float bmax[4], bmin[4], half;
        if (!t->active || t->crashed_until > g_race_time) continue;
        carcol_synth_rb(&rb, t->pos,
                        t->pos.y - 0.5f - g_traffic_ymin[t->car], t->yaw,
                        (Vec3){0, 0, 0});
        half = 0.5f * (g_traffic_len[t->car] > 2.0f
                       ? g_traffic_len[t->car] : 4.2f);
        bmax[0] = 1.0f; bmax[1] = 1.2f; bmax[2] = half; bmax[3] = 0.0f;
        bmin[0] = -1.0f; bmin[1] = -0.2f; bmin[2] = -half; bmin[3] = 0.0f;
        score_obb_from_frame(&others[n], (const float (*)[4])rb.frame,
                             bmax, bmin);
        ids[n] = i;                       // stable per traffic slot
        n++;
    }
    /* SCORE-CLASSIFIER: the RIVAL RACERS are near-miss candidates too.
     * FUN_00194EE0 walks the generic proximity broadphase (DAT_00649A8E ->
     * the 0x180-stride object table) and keys its eight slots on
     * object+0x177 -- the SAME id FUN_00197920 uses when a collision
     * cancels a slot.  Traffic and racers therefore share one id space; the
     * harness keeps them apart with B3_NM_RACER_ID so a traffic slot index
     * can never be confused with a racer slot. */
    for (i = 0; i < g_num_vehicles && n < B3_TRAFFIC_N; i++) {
        Vehicle* r = &g_vehicles[i];
        if (r == v || !r->active || !r->fsim_ready) continue;
        score_obb_from_frame(&others[n], (const float (*)[4])r->fsim.rb.frame,
                             r->fsim.half_ext, r->fsim.center_off);
        ids[n] = B3_NM_RACER_ID(i);
        n++;
    }
    b3_score_events_near_miss(&v->sev, &v->bar, &me,
                              v->sim.speed * B3_SE_MPH_PER_MS,
                              g_race_time, others, ids, n);
    /* CRASH-SHOW H4: the passing-traffic WHOOSH -- the port of
     * FUN_00146530 (bstpsl0N / bstpss0N).  The emitter is geometric,
     * not score-driven: retail runs it per traffic car out of the
     * traffic-audio manager and the near-miss scorer plays no sound at
     * all.  This loop is simply the place where the harness already has
     * every traffic car's pose to hand.  `small` is the recovered class
     * 1/2 test, mapped here to the COMP (light) traffic class. */
    if (v == &g_player) {
        float ppos[3] = { v->pos.x, v->pos.y, v->pos.z };
        float pdir[3] = { sinf(v->rot.y), 0.0f, -cosf(v->rot.y) };
        float pright[3] = { cosf(v->rot.y), 0.0f, sinf(v->rot.y) };
        for (i = 0; i < g_traffic_n && i < B3_TRAFFIC_N; i++) {
            TrafficCar* t = &g_traffic[i];
            float tpos[3], tdir[3], u[3], d, closing;
            int small;
            if (!t->active) { g_pass_state[i].primed = 0; continue; }
            tpos[0] = t->pos.x; tpos[1] = t->pos.y; tpos[2] = t->pos.z;
            tdir[0] = sinf(t->yaw); tdir[1] = 0.0f; tdir[2] = -cosf(t->yaw);
            u[0] = ppos[0] - tpos[0];
            u[1] = ppos[1] - tpos[1];
            u[2] = ppos[2] - tpos[2];
            d = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
            if (d < 0.5f || d > 60.0f) { g_pass_state[i].primed = 0; continue; }
            u[0] /= d; u[1] /= d; u[2] /= d;
            /* rec+0xFC: dot(playerVel,u) - dot(trafficVel,u); retail's is
             * negative while closing, so take the magnitude. */
            closing = -((v->vel.x*u[0] + v->vel.y*u[1] + v->vel.z*u[2])
                        - (tdir[0]*t->speed*u[0] + tdir[2]*t->speed*u[2]));
            if (closing < 0.0f) closing = -closing;
            small = (strncmp(B3_TRAFFIC_CARS[t->car].cls, "COMP", 4) == 0);
            int pev = b3_sfx_traffic_pass(&g_pass_state[i], closing, ppos,
                                          pdir, pright, tpos, tdir, small);
            if (pev >= 0 && getenv("B3_SFX_LOG"))
                printf("[sfx] t=%.2f traffic PASS whoosh (closing %.1f m/s, "
                       "%s)\n", g_race_time, closing,
                       small ? "small" : "big");
        }
    }
}

static void carcol_fill_traffic(CarColEntry* e, TrafficCar* t, int idx) {
    B3CarBody* b = &e->body;
    memset(b, 0, sizeof(*b));
    e->veh = NULL; e->traffic = idx;
    if (idx >= 0 && t == &g_traffic[idx]) {
        e->synth = 0;
        b->rb = &t->rb;
    } else {
        e->synth = 1;
        carcol_synth_rb(&e->rb, t->pos,
                        t->pos.y - 0.5f - g_traffic_ymin[t->car], t->yaw,
                        (Vec3){sinf(t->yaw) * t->speed, 0.0f,
                               -cosf(t->yaw) * t->speed});
        b->rb = &e->rb;
    }
    b->hull = &g_traffic_hull[t->car];
    b->mass = t->mass_kg;
    float half = 0.5f * (g_traffic_len[t->car] > 2.0f
                         ? g_traffic_len[t->car] : 4.2f);
    b->bbmax[0] =  1.0f; b->bbmax[1] =  1.2f; b->bbmax[2] =  half;
    b->bbmin[0] = -1.0f; b->bbmin[1] = -0.2f; b->bbmin[2] = -half;
    b->type    = B3_COL_TYPE_TRAFFIC;
    b->crashed = (unsigned char)(t->crashed_until > g_race_time);
    b->asleep  = t->asleep;
}

static void carcol_writeback(CarColEntry* e, float dt) {
    if (!e->synth) return;
    B3RigidBody* rb = &e->rb;
    float m = e->body.mass > 1.0f ? e->body.mass : 1.0f;
    Vec3 d = { rb->deflection[0], 0.0f, -rb->deflection[2] };
    Vec3 k = { (rb->imp_force[0] + rb->force_acc[0] * dt) / m, 0.0f,
               -(rb->imp_force[2] + rb->force_acc[2] * dt) / m };
    if (e->traffic >= 0) {
        TrafficCar* t = &g_traffic[e->traffic];
        t->pos.x += d.x; t->pos.z += d.z;
    } else if (e->veh) {
        int slot = (int)(e->veh - g_vehicles);
        e->veh->pos.x += d.x; e->veh->pos.z += d.z;
        if (slot >= 0 && slot < 8) {
            g_carcol_knock[slot].x += k.x;
            g_carcol_knock[slot].z += k.z;
            if (g_wrecks[slot].active) {
                g_wrecks[slot].frame[3][0] += d.x;
                g_wrecks[slot].frame[3][2] += d.z;
                /* PANELS: a car-vs-car knock is a COLLISION event too. */
                {
                    float kl = sqrtf(k.x * k.x + k.z * k.z);
                    if (kl > 1e-3f) {
                        float kn[3] = { k.x / kl, 0.0f, k.z / kl };
                        b3_wreck_report_hit(&g_wrecks[slot], kn, m * kl,
                                            /*collision=*/1);
                    }
                }
                g_wrecks[slot].vel[0] += k.x;
                g_wrecks[slot].vel[2] += k.z;
            }
        }
    }
}

// Route a resolved slam through the already-verified gameplay bookkeeping
// (docs/RE_GAMEPLAY.md 6/7): the victim gets racecar+0x1598 / +0x16BC and a
// takedown claim is armed for the attacker inside Maximum Crash Wait Time.
// The classification itself -- who is the attacker, side vs rear-end, light
// vs full slam, and the crash trigger -- now comes from FUN_001121F0.
/* B3_TD_TRACE=1: dump who holds a claim on `slot` the instant it crashes.
 * The chain is slam (racecar+0x1598/+0x16BC) -> crash (FUN_00197430 arms
 * racecar+0x15A8[victim grid]) -> Race Car Clear Wait -> FUN_00197040
 * commits.  A crash with NO claim line here is the whole reason an award
 * never lands. */
static int tdr_trace_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("B3_TD_TRACE") != NULL;
    return on;
}

static void tdr_trace_claims(const char* how, int slot) {
    if (!tdr_trace_on() || slot < 0 || slot >= B3_TDR_MAX_CARS) return;
    printf("[TD-CHAIN] t=%.2f car %d CRASH (%s) aggressor=%d "
           "aggressor_t=%.2f claims:", g_race_time, slot, how,
           g_tdr.car[slot].aggressor, g_tdr.car[slot].aggressor_time);
    for (int j = 0; j < g_num_vehicles && j < B3_TDR_MAX_CARS; j++)
        if (g_tdr.car[j].claim[g_tdr.car[slot].grid] >= 0.0f)
            printf(" car%d@%.2f", j,
                   g_tdr.car[j].claim[g_tdr.car[slot].grid]);
    printf("\n");
}

/* CRASH-CINEMA: WRECK vs RACER -- the AFTERTOUCH TAKEDOWN chain.
 *
 * b3_carcol_resolve orders the pair so the CRASHED body is `b` and runs
 * b3_carcol_resolve_wreck (the port of FUN_00113960).  That function sets
 * ct->crash_a when the still-driving car's contact impulse passes the 5000
 * gate (0x0011408B) -- i.e. when the wreck TAKES THE OTHER CAR OUT -- but it
 * never writes ct->event, because `event` is the alive-vs-alive slam
 * classifier only.  carcol_slam_racers returns on its first line for that
 * reason, so before this hook a wreck could shove a rival but never wreck
 * one, and the aftertouch takedown could not fire at all.
 *
 * Retail's chain:
 *   FUN_00113960 @0x0011411C  stores the CRASHED car's racecar into the
 *                             victim's crash-cause record +0x0C
 *   FUN_00197430              turns that record into a takedown claim with
 *                             the AFTERTOUCH qualifier set
 *   the qualifier is gated on the wreck's veh+0x4AC5, the byte FUN_00118410
 *   sets @0x00118CD3 the first frame the player actually steered the wreck
 *   (and FUN_00119C00 clears @0x00119C87)
 *   FUN_001994D0              pays B3_TDR_BP_AFTERTOUCH (1250, 0x003F7478)
 *                             and picks message 0xAA "AFTERTOUCH TAKEDOWN!"
 *
 * b3_td_cause_wreck + b3_td_on_crash is the ported mirror of the first two
 * steps; burnout3_td_rules.c treats the +0x4AC5 byte as always enabled [S],
 * so the qualifier is gated HERE instead, which is observably identical.
 */
static void carcol_wreck_takedown(Vehicle* a, Vehicle* b,
                                  const B3CarContact* ct) {
    if (!ct->crash_a && !ct->crash_b) return;
    int aw = a->crashed_until > 0.0f, bw = b->crashed_until > 0.0f;
    if (aw == bw) return;         /* both alive -> the slam path owns it;
                                     both wrecked -> nobody left to take out */
    Vehicle* wreck = aw ? a : b;
    Vehicle* vic   = aw ? b : a;
    int ws = (int)(wreck - g_vehicles), vs = (int)(vic - g_vehicles);
    if (ws < 0 || ws >= 8 || vs < 0 || vs >= 8) return;
    if (vic->crashed_until > 0.0f || g_race_time < vic->immune_until) return;
    if (!g_wrecks[ws].active) return;
    /* YOU DO NOT GET TAKEN OUT BY A WRECK YOU CAUSED.
     *
     * Retail gates this in FUN_00113960: before committing the crash it
     * computes, on the wreck's dilated clock (+0x10DC), the age of the
     * ATTRIBUTION stamp `wreck+0x15A8[attacker+0x19BC]` -- "when did this
     * car last claim that one" -- and clears the crash flag when the age
     * is < DAT_003EBE7C = 1.5 s.  b3_carcol_resolve_wreck() applies that
     * window already, but a takedown PRESENTATION runs longer than 1.5 s,
     * so control came back to the attacker just after the protection
     * lapsed and the victim's still-burning wreck took them out --
     * measured: "wreck 0 -> car 5 ... wreck_age 1.40 s,
     * victim_was_its_attacker=1", and reported from play as crashing into
     * the flaming wreck right after the cinematic.
     *
     * Held here for as long as the wreck is still IN its crashed window,
     * which is the span the attribution claim covers.  GLUE on the
     * duration only -- the attribution rule itself is retail's.  [S] */
    if (g_tdr.car[ws].aggressor == vs) {
        if (getenv("B3_WRECK_TRACE"))
            fprintf(stderr, "[wreck] t=%.2f car %d spared: it caused wreck %d\n",
                    g_race_time, vs, ws);
        return;
    }

    vic->crashed_until = g_race_time + 5.0f;
    vic->immune_until  = g_race_time + crash_latch_for(vic);
    Vec3 n  = { ct->normal[0], 0.0f, -ct->normal[2] };
    Vec3 cp = { ct->point[0], vic->pos.y - 0.2f, -ct->point[2] };
    wreck_begin_for(vic, cp, n, vec3_sub(vic->vel, wreck->vel),
                    B3_WRECK_ENTRY_CAR);
    b3_sfx_event_at(B3_SFX_IMPACT_FATAL, ct->impact,
                    vic->pos.x, vic->pos.y, vic->pos.z);
    b3_sfx_event_at(B3_SFX_GLASS_FRONT, 0.02f,
                    vic->pos.x, vic->pos.y, vic->pos.z);

    /* veh+0x4AC5 -- FUN_00197430 @0x00197646 tests it BEFORE it arms the
     * claim, so a wreck the player never steered files no wreck claim at
     * all and the takedown falls back to the ordinary arms. */
    int at = g_wrecks[ws].aftertouch_used;
    g_tdr.car[ws].crashed = 1;
    g_tdr.car[vs].crashed = 1;
    if (at) {
        B3TdCause tc;
        b3_td_cause_wreck(&tc, ws);             /* cause record +0x0C */
        b3_td_on_crash(&g_tdr, g_race_time, vs, &tc, NULL);
        /* RETAIL FINDING: score+0x4F0[] (racecar+0x15C0[]) is ONE byte
         * doing TWO jobs -- FUN_00197040 @0x00197093 treats it as the
         * flag that FORCES a claim past the "claimant is still crashed"
         * gate, and FUN_00198E60 @0x00198EF0 passes the same byte to
         * FUN_001994D0 as the AFTERTOUCH qualifier.  Only the wreck arm
         * ever sets it (0x00197667), which is exactly what lets an
         * aftertouch takedown pay out while the player is still a wreck.
         * burnout3_td_rules.c models it as TWO arrays, claim_force[] and
         * claim_aftertouch[], and nothing writes the first -- so without
         * this line the claim is silently dropped 0.5 s later. */
        g_tdr.car[ws].claim_force[g_tdr.car[vs].grid] = 1;
    } else {
        b3_td_on_crash(&g_tdr, g_race_time, vs, NULL, NULL);
    }
    printf("[Burnout3] t=%.2f WRECK TAKEDOWN: wreck %d -> car %d "
           "(impact %.0f, aftertouch=%d, wreck_age %.2f s, "
           "victim_was_its_attacker=%d)\n",
           g_race_time, ws, vs, ct->impact, at,
           g_wrecks[ws].rest_clock,
           g_tdr.car[ws].aggressor == vs);
    tdr_trace_claims("wreck-vs-racer", vs);
}

static void carcol_wreck_racer_from_traffic(Vehicle* v, TrafficCar* t,
                                             const B3CarContact* ct) {
    if (!v || !t || v->crashed_until > 0.0f
        || g_race_time < v->immune_until) return;
    int slot = (int)(v - g_vehicles);
    v->crashed_until = g_race_time + 5.0f;
    v->immune_until = g_race_time + crash_latch_for(v);
    if (v == &g_player) {
        g_crash_hit_kind = B3_HUD_HIT_TRAFFIC;
        g_crash_hit_id = B3_TRAFFIC_CARS[t->car].id;
    }
    Vec3 normal = {ct->normal[0], 0.0f, -ct->normal[2]};
    Vec3 point = {ct->point[0], v->pos.y - 0.2f, -ct->point[2]};
    wreck_begin_for(v, point, normal, (Vec3){0, 0, 0}, B3_WRECK_ENTRY_CAR);
    printf("[Burnout3] t=%.2f car %d CRASHED into %s traffic "
           "(vn %.1f mph, impact %.0f)\n",
           g_race_time, slot, B3_TRAFFIC_CARS[t->car].id,
           ct->vn_mph, ct->impact);
    b3_sfx_event_at(B3_SFX_IMPACT_FATAL, ct->impact,
                    v->pos.x, v->pos.y, v->pos.z);
    if (slot >= 0 && slot < B3_TDR_MAX_CARS) {
        b3_td_on_crash(&g_tdr, g_race_time, slot, NULL, NULL);
        tdr_trace_claims("traffic", slot);
    }
}

static void carcol_slam_racers(Vehicle* a, Vehicle* b, const B3CarContact* ct) {
    if (ct->event < 1) return;
    Vehicle* att = ct->attacker_is_b ? b : a;
    Vehicle* vic = ct->attacker_is_b ? a : b;
    int as = (int)(att - g_vehicles), vs = (int)(vic - g_vehicles);
    if (as < 0 || as >= 8 || vs < 0 || vs >= 8) return;

    /* TD-RULES: the carcol event id IS the game-context vtable +0x64 kind
     * (RE_CARCOL 5), so hand it straight to FUN_00029F30's mirror.  A slam
     * NEVER wrecks (proved two ways, RE_TD_RULES 0): kinds 5/6 only stamp
     * the victim's out-of-control clock, kinds 3/4 pay light-slam BP and do
     * NOT destabilize, kind 2 stamps the aggressor, kind 1 rubs. */
    g_tdr.car[as].speed_ms = att->sim.speed;
    g_tdr.car[vs].speed_ms = vic->sim.speed;
    g_tdr.car[as].crashed  = att->crashed_until > 0.0f;
    g_tdr.car[vs].crashed  = vic->crashed_until > 0.0f;
    b3_td_slam_report(&g_tdr, g_race_time, ct->event, as, vs, ct->strength);
    if (tdr_trace_on())
        printf("[TD-CHAIN] t=%.2f SLAM kind=%d att=%d vic=%d str=%.2f -> "
               "victim slam_t=%.2f aggressor=%d aggressor_t=%.2f\n",
               g_race_time, ct->event, as, vs, ct->strength,
               g_tdr.car[vs].slam_time, g_tdr.car[vs].aggressor,
               g_tdr.car[vs].aggressor_time);
    /* AGGRESSION: aggro+0x24 (the contact byte FUN_00169E80 reads) and the
     * per-slot last-hit stamps racecar+0x15E0[k] that gate the immunity
     * sweep at 0x00169715. */
    if (as < B3_AGGRO_MAX && vs < B3_AGGRO_MAX) {
        att->aggro.hit = 1;
        g_aggro_hit[as][vs] = g_race_time;
        g_aggro_hit[vs][as] = g_race_time;
    }
    /* mirror the recovered stamp into the fields the harness already reads
     * for the steer-away envelope / AI authority */
    vic->slam_time = g_tdr.car[vs].slam_time;
    vic->slam_by   = g_tdr.car[vs].aggressor;
    // veh+0x153C: the slam classifier (FUN_00112AC0 @0x112B06) stores
    // (side_dot < 0) on the victim and its complement on the attacker; the
    // steer-away lock takes its sign from it. Both ct->normal (A -> B) and
    // fsim.rb.frame are GAME space here, so the dot needs no z mirror.
    {
        const float (*vf)[4] = (const float (*)[4])vic->fsim.rb.frame;
        float d = ct->normal[0] * vf[0][0] + ct->normal[2] * vf[0][2];
        if (ct->attacker_is_b == (vic == a)) d = -d;   /* normal is A -> B */
        vic->slam_side = (d < 0.0f) ? 1 : 0;
        att->slam_side = (unsigned char)!vic->slam_side;
    }

    int heavy = (ct->event == B3_SLAM_SIDE || ct->event == B3_SLAM_REAR);
    /* SFX: the same split the game makes -- FUN_001989A0 calls FUN_00140610
     * (SLAM______) for a full slam and FUN_00140480 (SHUNT_____) for a light
     * one; the car-vs-car body impact is the separate IMPACTNUDG/IMPACTFATA
     * emitter fed with the contact impulse. */
    b3_sfx_event_at(heavy ? B3_SFX_SLAM : B3_SFX_SHUNT, 0.0f,
                    vic->pos.x, vic->pos.y, vic->pos.z);
    b3_sfx_event_at(vic->crashed_until > 0.0f ? B3_SFX_IMPACT_FATAL
                                              : B3_SFX_IMPACT_NUDGE,
                    ct->impact, vic->pos.x, vic->pos.y, vic->pos.z);

    /* FUN_001121F0's own >150 mph mutual crash trigger still wrecks. */
    if (ct->crash_a || ct->crash_b) {
        if (vic->crashed_until <= 0.0f && g_race_time >= vic->immune_until) {
            vic->crashed_until = g_race_time + 5.0f;
            vic->immune_until  = g_race_time + crash_latch_for(vic);
            Vec3 n = {ct->normal[0], 0.0f, -ct->normal[2]};
            if (!ct->attacker_is_b) { n.x = -n.x; n.z = -n.z; }
            Vec3 cp = {ct->point[0], vic->pos.y - 0.2f, -ct->point[2]};
            wreck_begin_for(vic, cp, n, vec3_sub(vic->vel, att->vel),
                            B3_WRECK_ENTRY_CAR);
            b3_sfx_event_at(B3_SFX_IMPACT_FATAL, ct->impact,
                            vic->pos.x, vic->pos.y, vic->pos.z);
            b3_sfx_event_at(B3_SFX_GLASS_FRONT, 0.02f,
                            vic->pos.x, vic->pos.y, vic->pos.z);
            b3_sfx_event_at(B3_SFX_GLASS_SIDES, 0.4f,
                            vic->pos.x, vic->pos.y, vic->pos.z);
            b3_sfx_event_at(B3_SFX_PANEL_L_DEFORM, 0.4f,
                            vic->pos.x, vic->pos.y, vic->pos.z);
            b3_td_on_crash(&g_tdr, g_race_time, vs, NULL, NULL); /* no cause */
            tdr_trace_claims("car-vs-car", vs);
        }
    }
    if (heavy)
        printf("[Burnout3] t=%.2f kind %d FULL SLAM: car %d -> car %d (%.2f):"
               " victim out of control, NOT wrecked\n",
               g_race_time, ct->event, as, vs, ct->strength);
    if (heavy) g_aggro_shot_req = 1;
    static int agg_trace = -1;
    if (agg_trace < 0) agg_trace = getenv("B3_AGGRO_TRACE") != NULL;
    if (agg_trace)
        printf("[AGGRO-CONTACT] t=%.2f kind=%d att=%d vic=%d str=%.2f "
               "attstate=%d attaim=%d\n", g_race_time, ct->event, as, vs,
               ct->strength, att->aggro.state, att->aggro.aim_valid);
}

/* TD-RULES per-frame pass: FUN_0011AEF0's cause record + FUN_00197430
 * attribution + FUN_00197040 deferred claim scan + DENIED/LUCKY (recovered
 * chain, validate_td_rules.py 315/315).  Magnitude gates and the contact
 * radius are GLUE stand-ins (RE_TD_RULES integration note 7). */
// ===========================================================================
// T-KEY DEBUG DUMP: write the full gamestate + a screenshot to build/ so a
// play-session moment can be handed to the developer/assistant as context.
// Files: build/debug_dump_NNN.txt + build/debug_dump_NNN.bmp
// ===========================================================================
static void debug_dump(void) {
    static int n = -1;
    char path[128];
    if (n < 0) {          // first dump this run: continue past existing files
        n = 0;
        for (int i = 1; i < 1000; i++) {
            FILE* probe;
            snprintf(path, sizeof path, "build/debug_dump_%03d.txt", i);
            probe = fopen(path, "r");
            if (!probe) break;
            fclose(probe);
            n = i;
        }
    }
    n++;

    // --- screenshot (same pipeline as B3_SHOT) ---
    snprintf(path, sizeof path, "build/debug_dump_%03d.bmp", n);
    int sw, sh;
    SDL_GetWindowSize(g_window, &sw, &sh);
    unsigned char* px = malloc((size_t)sw * sh * 4);
    if (px) {
        glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px);
        b3_postfx_flip_rows(px, sw, sh);   // burnout3_postfx.c 2b
        SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormatFrom(
            px, sw, sh, 32, sw * 4, SDL_PIXELFORMAT_ABGR8888);
        if (sf) { SDL_SaveBMP(sf, path); SDL_FreeSurface(sf); }
        free(px);
    }

    // --- gamestate text ---
    snprintf(path, sizeof path, "build/debug_dump_%03d.txt", n);
    FILE* f = fopen(path, "w");
    if (!f) return;
    b3_write_gamestate(f, n);
    fclose(f);
    printf("[Burnout3] debug dump %03d -> build/debug_dump_%03d.{txt,bmp}\n",
           n, n);
}

/* B3_DRIVE_LOG: append the whole game state, every frame, to one file.
 * B3_DRIVE_LOG=1 writes build/drive_log.txt; any other value is used as the
 * path.  Same block the T-key dump writes, so a frame-by-frame trace and a
 * one-shot report read identically.  Roughly 1-3 KB per frame -- a 60 s run
 * at 60 fps is on the order of 10 MB, so it is opt-in. */
static void drive_log_tick(void) {
    static FILE* f = NULL;
    static int   state = 0;          /* 0 unknown, 1 open, -1 disabled */
    if (state < 0) return;
    if (!state) {
        const char* e = getenv("B3_DRIVE_LOG");
        if (!e || !*e) { state = -1; return; }
        const char* path = (strcmp(e, "1") == 0) ? "build/drive_log.txt" : e;
        f = fopen(path, "w");
        if (!f) {
            printf("[Burnout3] drive log: cannot open %s\n", path);
            state = -1;
            return;
        }
        setvbuf(f, NULL, _IOFBF, 1 << 16);
        state = 1;
        printf("[Burnout3] drive log -> %s (every frame; Ctrl-C safe)\n",
               path);
    }
    b3_write_gamestate(f, g_frame_count);
    fputc('\n', f);
    /* flush periodically so a crash/kill still leaves a usable tail */
    if ((g_frame_count % 120) == 0) fflush(f);
}

/* The whole game state as text.  Shared by the T-key dump (one file per
 * press) and by B3_DRIVE_LOG (one block per frame, appended), so a live
 * bug report and a frame-by-frame trace read identically. */
static void b3_write_gamestate(FILE* f, int n) {
    B3TdfxStatus st;
    b3_tdfx_status(&st);
    fprintf(f, "=== Burnout3 RE harness debug dump %03d ===\n", n);
    fprintf(f, "frame %d  race_time %.3f  sim_dt %.5f  real_dt %.5f\n",
            g_frame_count, g_race_time, g_delta_time, g_tdfx_real_dt);
    fprintf(f, "game_state %d  lap %d/%d  cam_fov %.1f\n",
            (int)g_state, g_current_lap + 1, g_lap_count, g_cam_fov_deg);
    fprintf(f, "tdfx: divisor %d timescale %.3f cinematic=%d t=%.2f "
            "victim=%d callout msg=0x%X age=%.2f\n",
            st.divisor, st.timescale, st.active, st.t, st.victim_slot,
            st.callout_msg, st.callout_age);
    for (int i = 0; i < g_num_vehicles; i++) {
        Vehicle* v = &g_vehicles[i];
        fprintf(f, "\ncar %d%s: pos (%.1f, %.1f, %.1f) vel (%.1f, %.1f, %.1f)"
                " yaw %.2f\n", i, i == 0 ? " (PLAYER)" : "",
                v->pos.x, v->pos.y, v->pos.z,
                v->vel.x, v->vel.y, v->vel.z, v->rot.y);
        fprintf(f, "  speed %.1f mph  gear %d  rpm %.0f  progress %.4f  "
                "lap %d\n", v->sim.speed * 2.2369363f, v->fsim.trans.gear,
                v->fsim.trans.omega * 9.549296f, v->track_progress, v->lap);
        fprintf(f, "  boost: tier %d meter %.1f/%.0f boosting %d  bp %d\n",
                v->bar.tier, v->bar.meter, v->bar.size, v->bar.boosting,
                g_tdr.car[i].bp);
        fprintf(f, "  crash: crashed_until %.2f immune %.2f  slam_time %.2f"
                " slam_by %d  td_credited %d aggressor %d\n",
                v->crashed_until, v->immune_until, v->slam_time, v->slam_by,
                g_tdr.car[i].td_credited, g_tdr.car[i].aggressor);
        fprintf(f, "  ai: tgt_angle %.1f tgt_speed %.1f rev_timer %.2f "
                "stuck %d/%0.1f  aggro state %d aim_valid %d\n",
                v->ai.target_angle, v->ai.target_speed, v->ai.reverse_timer,
                v->stuck_frames, v->stuck_time,
                v->aggro.state, v->aggro.aim_valid);
        fprintf(f, "  score: bp %d  air %.1f/t%d onc %.1f/t%d drift %.1f/t%d"
                " nm %.0f\n", v->sev.bp,
                v->sev.air.value, v->sev.air.tier,
                v->sev.onc.value, v->sev.onc.tier,
                v->sev.drift.value, v->sev.drift.tier, v->sev.nm.value);
        if (v->last_hit_time > 0.0f)
            fprintf(f, "  last wall hit: t %.2f (%.1fs ago) at "
                    "(%.1f, %.1f, %.1f) n (%.2f, %.2f) vin %.1f m/s\n",
                    v->last_hit_time, g_race_time - v->last_hit_time,
                    v->last_hit_pos.x, v->last_hit_pos.y, v->last_hit_pos.z,
                    v->last_hit_n.x, v->last_hit_n.z, v->last_hit_vin);
        /* CRASH GATE.  Everything the two wall arms decide on, so a drive
         * log answers "why was there no crash here?" without a rebuild:
         * the in-substep soup arm (contact_state_198 -> crash_fired, with
         * FUN_0011AEF0's own dv/head-on pair) and the td_rules record that
         * mesh_collide's sweep fills.  `veto` is +0x1353 bit 3, `auth` is
         * veh+0x1534. */
        if (i < 8) {
            const B3TdWallHit* wh = &g_tdr.wall[i];
            fprintf(f, "  gate: contact %d soup %d veto 0x%X auth %.2f "
                    "| substep dv %.2f/%.2f headon %.3f/%.3f fired %d\n",
                    v->fsim.contact_state_198, v->fsim.soup.count,
                    v->fsim.flags_1353, v->fsim.authority_1534,
                    v->fsim.crash_dv, v->fsim.crash_dv_thr,
                    v->fsim.crash_headon, v->fsim.crash_headon_thr,
                    v->fsim.crash_fired);
            fprintf(f, "        tdwall valid %d fire %d dv %.2f/%.2f "
                    "headon %.3f/%.3f auth %.2f vin %.1f\n",
                    wh->valid, wh->fire, wh->dv, wh->dv_thr, wh->headon,
                    wh->headon_thr, wh->authority, wh->vin);
            {   /* the OTHER two contact systems, so a frame that loses
                 * speed with both wall arms silent still says what hit. */
                const B3TdObjectHit* oh = &g_tdr.obj[i];
                fprintf(f, "        object valid %d fire %d closing %.1f/%.1f"
                        " auth %.2f damage %.0f\n",
                        oh->valid, oh->fire, oh->closing_mph, oh->thr,
                        oh->authority, oh->damage);
                fprintf(f, "        carcol last t %.2f vs %s%d (%.1f mph) "
                        "vn %.1f mph impact %.0f crashA %d crashB %d\n",
                        g_carcol_dbg[i].t,
                        g_carcol_dbg[i].is_traffic ? "traffic " : "car ",
                        g_carcol_dbg[i].other, g_carcol_dbg[i].other_mph,
                        g_carcol_dbg[i].vn,
                        g_carcol_dbg[i].impact, g_carcol_dbg[i].crash_a,
                        g_carcol_dbg[i].crash_b);
            }
        }
        if (i < 8 && g_wrecks[i].active)
            fprintf(f, "  WRECK: pos (%.1f, %.1f, %.1f) airborne %d "
                    "air_time %.2f\n",
                    g_wrecks[i].frame[3][0], g_wrecks[i].frame[3][1],
                    g_wrecks[i].frame[3][2], g_wrecks[i].airborne,
                    g_wrecks[i].air_time);
    }
    int tn = 0, tc = 0;
    for (int i = 0; i < g_traffic_n; i++) {
        if (g_traffic[i].active) tn++;
        if (g_traffic[i].crashed_until > g_race_time) tc++;
    }
    fprintf(f, "\ntraffic: %d active, %d crashed\n", tn, tc);
}

// ---------------------------------------------------------------------------
// CRASH TRACE tick (see the statics by g_wrecks). Called once per frame
// after game_update(): opens build/crash_trace_NNN.log when the player's
// crash presentation starts, appends one line per frame (dilation, wreck
// pose, velocities, spin, rest state -- containment events are interleaved
// by the wreck path itself), and closes it when the car is released.
// ---------------------------------------------------------------------------
static void crash_trace_tick(void) {
    static int disabled = -1;
    if (disabled < 0) disabled = getenv("B3_NO_CRASH_TRACE") != NULL;
    if (disabled) return;
    Vehicle* v = &g_player;
    B3TdfxStatus st;
    b3_tdfx_status(&st);
    int active = (v->crashed_until > 0.0f)
              || (st.active && st.victim_slot == 0);
    if (active && !g_crash_trace) {
        char path[128];
        int n = 1;
        for (; n < 1000; n++) {
            snprintf(path, sizeof path, "build/crash_trace_%03d.log", n);
            FILE* t = fopen(path, "r");
            if (!t) break;
            fclose(t);
        }
        g_crash_trace = fopen(path, "w");
        if (!g_crash_trace) return;
        g_crash_trace_frames = 0;
        fprintf(g_crash_trace,
                "=== crash trace %03d ===\n"
                "entry: frame %d race_time %.3f state %d\n"
                "player pos (%.2f %.2f %.2f) vel (%.2f %.2f %.2f) yaw %.3f"
                " speed %.1f mph\n",
                n, g_frame_count, g_race_time, (int)g_state,
                v->pos.x, v->pos.y, v->pos.z,
                v->vel.x, v->vel.y, v->vel.z, v->rot.y,
                v->sim.speed * 2.2374146f);
        if (v->last_hit_time > 0.0f)
            fprintf(g_crash_trace,
                    "last wall hit: t %.2f at (%.1f %.1f %.1f) n (%.2f %.2f)"
                    " vin %.1f m/s\n",
                    v->last_hit_time, v->last_hit_pos.x, v->last_hit_pos.y,
                    v->last_hit_pos.z, v->last_hit_n.x, v->last_hit_n.z,
                    v->last_hit_vin);
        printf("[Burnout3] crash trace started -> %s\n", path);
    }
    if (g_crash_trace) {
        const B3WreckState* wk = &g_wrecks[0];
        fprintf(g_crash_trace,
                "f=%d t=%.3f dt=%.5f div=%d ts=%.3f tdfx_t=%.2f cin=%d | "
                "wr%d pos %.3f %.3f %.3f | vel %.3f %.3f %.3f (%.2f) | "
                "om %.3f %.3f %.3f | upY %.3f | air=%d at=%.2f "
                "settle=%.2f asleep=%d rest=%.2f | until %.2f\n",
                g_frame_count, g_race_time, g_delta_time,
                st.divisor, st.timescale, st.t, st.active,
                wk->active, wk->frame[3][0], wk->frame[3][1], wk->frame[3][2],
                wk->vel[0], wk->vel[1], wk->vel[2], wk->vel[3],
                wk->omega[0], wk->omega[1], wk->omega[2],
                wk->frame[1][1], wk->airborne, wk->air_time,
                wk->settle, wk->asleep, wk->rest_clock, v->crashed_until);
        {   /* PANELS: per-frame panel machine state -- one char per panel
             * (state 0..4) plus the accumulator/threshold pair, so a trace
             * shows exactly when each panel loosened, crumpled and left. */
            const B3PanelSet* ps = &g_panels[0];
            if (ps->n > 0) {
                char st[B3_PANEL_MAX + 1];
                for (int pk = 0; pk < ps->n; pk++)
                    st[pk] = (char)('0' + ps->state[pk]);
                st[ps->n] = 0;
                fprintf(g_crash_trace, "  panels %s hit=%d imp=%.0f acc=",
                        st, wk->hit_count, wk->hit_impulse);
                for (int pk = 0; pk < ps->n; pk++)
                    fprintf(g_crash_trace, "%s%.3f/%.3f", pk ? "," : "",
                            ps->acc[pk], ps->thr_rip[pk] * B3_PANEL_RIP_SCALE);
                fprintf(g_crash_trace, " off=%d\n", ps->detached_total);
            }
        }
        g_crash_trace_frames++;
        if (!active || g_crash_trace_frames > 3600) {
            fprintf(g_crash_trace,
                    "=== end: frame %d race_time %.3f (%d frames)%s ===\n",
                    g_frame_count, g_race_time, g_crash_trace_frames,
                    g_crash_trace_frames > 3600 ? " [frame cap]" : "");
            fclose(g_crash_trace);
            g_crash_trace = NULL;
            printf("[Burnout3] crash trace complete (%d frames)\n",
                   g_crash_trace_frames);
        }
    }
}

static void tdr_frame_pass(void) {
    static float pos[B3_TDR_MAX_CARS][3];
    for (int i = 0; i < g_num_vehicles && i < B3_TDR_MAX_CARS; i++) {
        pos[i][0] = g_vehicles[i].pos.x;
        pos[i][1] = g_vehicles[i].pos.y;
        pos[i][2] = g_vehicles[i].pos.z;
        g_tdr.car[i].crashed  = g_vehicles[i].crashed_until > 0.0f;
        g_tdr.car[i].speed_ms = g_vehicles[i].sim.speed;
    }
    /* per-pair contact timers (FUN_001979E0 / score+0x528) and the
     * game-context +0x54 contact notify that arms DENIED / LUCKY ESCAPE */
    for (int i = 0; i < g_num_vehicles; i++)
        for (int j = i + 1; j < g_num_vehicles; j++) {
            Vec3 d = vec3_sub(g_vehicles[i].pos, g_vehicles[j].pos);
            int touch = (d.x * d.x + d.z * d.z) < 16.0f;    /* GLUE radius */
            b3_td_contact(&g_tdr, g_race_time, g_delta_time, i, j, touch);
            if (touch) { b3_td_contact_notify(&g_tdr, g_race_time, i);
                         b3_td_contact_notify(&g_tdr, g_race_time, j); }
        }
    for (int i = 0; i < g_num_vehicles && i < B3_TDR_MAX_CARS; i++)
        if (g_tdr_wall[i] > 1.0f
            || (g_vehicles[i].fsim_ready
                && g_vehicles[i].fsim.contact_state_198 == 1))
            b3_td_contact_notify(&g_tdr, g_race_time, i);

    /* a barrier hit -> the crash the takedown attaches to, WALL cause.
     * The decision is FUN_0011AEF0's own (b3_td_wall_take): the harness no
     * longer imposes a closing-speed threshold or a head-on gate of its
     * own.  Retail's head-on test is |dot(n, at)| > authority*0.707 and it
     * is SCALED, not skipped, when the car is out of control -- the old
     * code applied a hard 0.7071 to every non-OOC hit and none at all to an
     * OOC one, which is neither. */
    for (int i = 0; i < g_num_vehicles && i < B3_TDR_MAX_CARS; i++) {
        Vehicle* v = &g_vehicles[i];
        B3TdWallHit wh;
        int fire = b3_td_wall_take(&g_tdr, i, &wh);
        /* CRASH-PARITY item 1: FUN_00112E70's object verdict enters the SAME
         * consequence path.  It is the case FUN_0011AEF0 cannot make -- its
         * gate additionally needs |dot(n, at)| > authority*0.707, so a fast
         * SIDE-ON clip of a prop only ever wrecks you here. */
        B3TdObjectHit oh;
        int ofire = b3_td_object_take(&g_tdr, i, &oh);
        float vin = g_tdr_wall[i];
        g_tdr_wall[i] = 0.0f;
        /* PHYS-LEDGER-4 / item 3.  v->fsim.crash_fired is the decision
         * FUN_0011AEF0 makes AT ITS OWN PROGRAM POINT -- inside the
         * substep, over the frozen soup, at 0x0011B909..0x0011B9A3,
         * where retail follows it with FUN_0010DCA0 @0x0011B9F3.  The
         * port sets it in b3_vehicle_chassis_contact (burnout3_crash.c
         * @`if (cv.crashed) v->crash_fired = 1`) and, until this hunk,
         * NOTHING drained it -- it latched on the first wall crash of
         * the race and stayed 1 for ever.  Retail has no such latch:
         * FUN_0011AEF0 rewrites its verdict every substep (the port
         * mirrors that with `v->crashed = 0` at the head of
         * b3_crash_response), and re-entry is refused downstream by
         * FUN_0010DCA0 on veh+0x210.  Drain it here, once per frame,
         * at the same place the td_rules record is drained. */
        int cfire = v->fsim_ready ? v->fsim.crash_fired : 0;
        v->fsim.crash_fired = 0;
        /* EITHER wall source may fire.  The harness still has two: the
         * in-substep soup resolve (crash_fired) and the td_rules record
         * that mesh_collide's sweep fills.  Deferring to crash_fired alone
         * whenever the sim was ready threw away every crash the soup did
         * not see -- a drive log caught the player losing 134.3 -> 47.6 mph
         * in ONE frame (the recovered head-on scrub, FUN_0011AEF0 steps
         * 2+3, so a wall WAS hit) with contact_state_198 == 0 and no crash
         * at all.  Unifying the two sources is the open item in
         * docs/RE_CRASH_PARITY.md section 6; until then, honour both. */
        int wall_fire = cfire || fire;
        {   /* B3_CFIRE_TRACE=1: one line per frame in which EITHER
             * decision fires, so the two can be differenced live. */
            static int cft = -1;
            if (cft < 0) cft = getenv("B3_CFIRE_TRACE") != NULL;
            if (cft && (cfire || fire || ofire || wh.valid
                       || v->fsim.contact_state_198 == 1))
                printf("[CFIRE] t=%.3f car %d substep=%d td_wall=%d "
                       "td_obj=%d | dv %.2f/%.2f headon %.3f/%.3f "
                       "auth %.2f impact %.0f cstate=%d crashed=%d "
                       "immune=%d soup=%d\n",
                       g_race_time, i, cfire, fire, ofire,
                       wh.dv, wh.dv_thr, wh.headon, wh.headon_thr,
                       wh.authority, wh.impact,
                       v->fsim.contact_state_198,
                       v->crashed_until > 0.0f,
                       g_race_time < v->immune_until,
                       v->fsim.soup.count);
        }
        if (v->crashed_until > 0.0f || g_race_time < v->immune_until) continue;
        if (!wall_fire && !ofire) continue;
        vin = wall_fire ? (cfire ? v->last_hit_vin : wh.vin)
                         : oh.closing_mph / B3_TDR_MPH;
        int ooc = b3_td_out_of_control(&g_tdr, i, g_race_time);
        B3TdCause cause;
        if (wall_fire) b3_td_cause_wall(&cause, 0);
        else      b3_td_cause_object(&cause, 0);   /* record +0x01 = 1 */
        v->crashed_until = g_race_time + 5.0f;
        v->immune_until  = g_race_time + crash_latch_for(v);
        /* CRASH-PIN: hand the wreck the contact that actually fired it.
         * (v->pos, (0,0,0)) zeroes FUN_0011AEF0's impulse outright --
         * j = -dot(rel_vel, n) / ... == 0 -- so the guard keeps a stale hit
         * from an earlier corner out.  The WALL entry adds no kick of its
         * own (the director magnitude is 0.0 BSS), so this impulse is the
         * whole entry. */
        Vec3 wcp = v->pos, wcn = (Vec3){0, 0, 0};
        if (g_race_time - v->last_hit_time <= 2.0f * g_tdfx_real_dt) {
            wcp = v->last_hit_pos;
            wcn = v->last_hit_n;
        }
        // Chassis-vs-world (wall/object), NO collision-pair record: the
        // crash-director entry magnitude is 0.0 BSS (RE_NOTES 16), so the
        // entry kick is NULL -- the tumble is FUN_0011AEF0's own impulse.
        wreck_begin_for(v, wcp, wcn, v->vel, B3_WRECK_ENTRY_WALL);
        b3_sfx_event_at(B3_SFX_IMPACT_WORLD, vin, v->pos.x, v->pos.y, v->pos.z);
        b3_sfx_event_at(B3_SFX_GLASS_FRONT, 0.02f, v->pos.x, v->pos.y, v->pos.z);
        b3_sfx_event_at(B3_SFX_PANEL_L_DEFORM, 0.4f, v->pos.x, v->pos.y, v->pos.z);
        b3_td_on_crash(&g_tdr, g_race_time, i, &cause, pos);
        tdr_trace_claims(wall_fire ? "wall" : "object", i);
        if (wall_fire) {
            /* CRASH-AUDIT C2: report the numbers the arm that FIRED gated
             * on.  `wh` is the td_rules record and it is EMPTY on a
             * crash_fired frame (the two arms never see the same contact),
             * which is why every AI wall crash used to print
             * "dv 0.0 > 0.00, headon 0.000 > 0.000, authority 0.00".  The
             * substep numbers are published per-car by
             * b3_vehicle_chassis_contact when it raises crash_fired. */
            printf("[Burnout3] t=%.2f car %d WALL CRASH at %.1f m/s "
                   "(dv %.1f > %.2f, headon %.3f > %.3f, authority %.2f, "
                   "out of control: %d, slammed by %d) [%s]\n",
                   g_race_time, i, vin,
                   cfire ? v->fsim.crash_dv         : wh.dv,
                   cfire ? v->fsim.crash_dv_thr     : wh.dv_thr,
                   cfire ? v->fsim.crash_headon     : wh.headon,
                   cfire ? v->fsim.crash_headon_thr : wh.headon_thr,
                   cfire ? v->fsim.authority_1534   : wh.authority,
                   ooc, g_tdr.car[i].aggressor,
                   cfire ? "substep" : "td_rules");
        }
        else
            printf("[Burnout3] t=%.2f car %d OBJECT CRASH at %.1f m/s "
                   "(closing %.1f mph > %.2f, authority %.2f, damage %.0f, "
                   "out of control: %d, slammed by %d)\n",
                   g_race_time, i, vin, oh.closing_mph, oh.thr, oh.authority,
                   oh.damage, ooc, g_tdr.car[i].aggressor);
    }

    /* window expiry + DENIED/LUCKY award + the deferred claim scan.
     * Retail dedups repeated DENIED/LUCKY through the callout slot's
     * priority field (FUN_00199350); the harness mirrors that with a
     * per-pair mute window (GLUE, integration note 7). */
    static float dl_mute[B3_TDR_MAX_CARS][B3_TDR_MAX_CARS];
    for (int i = 0; i < g_num_vehicles && i < B3_TDR_MAX_CARS; i++) {
        B3TdEvent ev[8];
        int n = b3_td_frame(&g_tdr, g_race_time, i, ev, 8);
        for (int k = 0; k < n; k++) {
            int at = ev[k].attacker, vi = ev[k].victim;
            if (at < 0 || at >= B3_TDR_MAX_CARS) at = i;
            if (ev[k].kind == B3_TDE_TAKEDOWN) {
                b3_boost_takedown(&g_vehicles[at].bar);     /* FUN_000273F0 */
                g_vehicles[at].score.bp = g_tdr.car[at].bp;
                if (at == 0 && vi >= 0) {
                    /* the cinematic + the retail message id (0x93..0xAE) --
                     * the higher id wins in the callout slot's priority test */
                    b3_tdfx_on_takedown(0, 0, vi, 1, g_race_time,
                                        g_tdr.car[vi].crash_time,
                                        -1, g_vehicles[at].bar.tier >= 3);
                    b3_tdfx_post_callout(ev[k].message, 0, 0);
                    if (ev[k].extra_message >= 0)
                        b3_tdfx_post_callout(ev[k].extra_message, 0, 0);
                }
                if (vi == 0)   /* the player got taken down */
                    b3_tdfx_post_callout(B3_TDFX_MSG_TAKEN_OUT, 1, 0);
                /* OnTakedown spends one presentation credit on the
                 * ATTACKER (FUN_00025A30 -> FUN_00025CC0 [C]); 0.0f for
                 * +0x16C4 = the unmodelled damage-machine field, keeping
                 * the human arm's 0.3 > health gate open. */
                b3_tdfx_takedown_credit(at == 0, vi == 0, 0.0f);
                printf("[Burnout3] TAKEDOWN COMMIT: car %d took down car %d"
                       " -- message 0x%X, +%d BP%s%s\n",
                       at, vi, ev[k].message, ev[k].bp,
                       ev[k].revenge ? " (revenge)" : "",
                       ev[k].aftertouch ? " (aftertouch)" : "");
            } else if (ev[k].kind == B3_TDE_DENIED
                       || ev[k].kind == B3_TDE_LUCKY) {
                int su = (ev[k].kind == B3_TDE_DENIED) ? vi : vi;
                if (su < 0 || su >= B3_TDR_MAX_CARS || at == su) continue;
                if (g_race_time < dl_mute[at][su]) continue;
                dl_mute[at][su] = g_race_time + B3_TDR_MAX_CRASH_WAIT_S;
                if (ev[k].kind == B3_TDE_DENIED) {
                    if (at == 0) b3_tdfx_post_callout(0x35, 0, 0);
                    printf("[Burnout3] TAKEDOWN DENIED: car %d survived"
                           " car %d\n", su, at);
                } else if (su == 0) {
                    b3_tdfx_post_callout(0x36, 0, 0);
                }
            }
        }
    }

    /* recovery clears the credit so a car can be taken down again */
    for (int i = 0; i < g_num_vehicles && i < B3_TDR_MAX_CARS; i++)
        if (g_vehicles[i].crashed_until <= 0.0f && g_tdr.car[i].td_credited
            && g_race_time > g_tdr.car[i].crash_time + 5.0f) {
            g_tdr.car[i].td_credited = 0;
            g_tdr.car[i].slam_time = -1.0f;
            g_tdr.car[i].aggressor = -1;
            g_tdr.car[i].aggressor_time = -1.0f;
        }
}

#define CARCOL_MAX (8 + B3_TRAFFIC_N * 2)

static void carcol_pass(void) {
    static CarColEntry ent[CARCOL_MAX];
    B3CarBody* list[CARCOL_MAX];
    // Rig id per entry: the traffic-car slot a body belongs to (-1 = racer).
    // A tractor and the trailer it tows share one, so the solver can be told
    // not to resolve them against each other -- the trailer nose sits inside
    // the tractor's chassis by design, and resolving that pair shoved the
    // whole rig sideways every frame (measured: the rig was the only car in
    // the lap census that ever left its lane, drifting to +-30 m).
    int rig[CARCOL_MAX];
    int traffic_owner[CARCOL_MAX];
    int n = 0;
    for (int i = 0; i < g_num_vehicles && n < CARCOL_MAX; i++) {
        Vehicle* v = &g_vehicles[i];
        if (!v->active || !v->fsim_ready || !g_car_hull_ok[i]) continue;
        carcol_fill_racer(&ent[n], v, i);
        rig[n] = -1;
        traffic_owner[n] = -1;
        list[n] = &ent[n].body; n++;
    }
    for (int i = 0; i < g_traffic_n && n < CARCOL_MAX; i++) {
        TrafficCar* t = &g_traffic[i];
        if (!t->active || !t->streamed || !g_traffic_hull_ok[t->car]) continue;
        carcol_fill_traffic(&ent[n], t, i);
        rig[n] = i;
        traffic_owner[n] = i;
        list[n] = &ent[n].body; n++;
        // A towed trailer is a second live rigid body, linked to its tractor
        // by FUN_00120F30's anchor constraint above.
        if (t->trailer >= 0 && t->trailer_ready
            && g_traffic_hull_ok[t->trailer]
            && n < CARCOL_MAX) {
            TrafficCar tt = *t;
            tt.pos = t->tr_pos;
            tt.yaw = t->tr_yaw;
            tt.car = t->trailer;
            tt.speed = t->speed;
            carcol_fill_traffic(&ent[n], &tt, i);
            ent[n].synth = 0;
            ent[n].body.rb = &t->trailer_rb;
            ent[n].body.mass = t->trailer_mass_kg;
            ent[n].traffic = -1;
            rig[n] = t->trailer_linked ? i : -1;
            traffic_owner[n] = i;
            list[n] = &ent[n].body; n++;
        }
    }
    if (n >= 2) {
        int pairs[0x100][2];
        int np = b3_carcol_broadphase(list, n, pairs, 0x100);
        for (int p = 0; p < np; p++) {
            int i = pairs[p][0], j = pairs[p][1];
            /* A tractor never collides with the trailer it is towing: the
             * trailer nose sits inside the tractor chassis by design.  The
             * old test keyed on rig[], which is only set while the trailer
             * is LINKED (`rig[n] = t->trailer_linked ? i : -1`), so an
             * unlinked trailer started colliding with its own tractor --
             * an overlapping pair with gap 0, which the follow law brakes
             * to a standstill and never releases.  traffic_owner[] is set
             * for both entries of a rig whatever the link state, so key on
             * that instead. */
            if (rig[i] >= 0 && rig[i] == rig[j]) continue;
            if (traffic_owner[i] >= 0 && traffic_owner[i] == traffic_owner[j])
                continue;
            B3CarContact ct;
            /* FUN_00113960's recent-slam window: retail clears the wreck
             * crash flag when this PAIR's slam stamp is younger than 1.5 s
             * ([0x003EBE7C]).  The stamps live on the vehicle records, so
             * the age is computed here and handed to the solver.  Retail
             * skips the test entirely when a non-traffic car hits a traffic
             * car, and b3_carcol_resolve_wreck() reproduces that guard. */
            ent[i].body.slam_recent = 0;
            ent[j].body.slam_recent = 0;
            if (ent[i].veh && ent[j].veh) {
                int sa = (int)(ent[i].veh - g_vehicles);
                int sb = (int)(ent[j].veh - g_vehicles);
                if (ent[i].veh->slam_by == sb && ent[i].veh->slam_time >= 0.0f
                    && g_race_time - ent[i].veh->slam_time
                       < B3_CARCOL_SLAM_WINDOW_S)
                    ent[i].body.slam_recent = 1;
                if (ent[j].veh->slam_by == sa && ent[j].veh->slam_time >= 0.0f
                    && g_race_time - ent[j].veh->slam_time
                       < B3_CARCOL_SLAM_WINDOW_S)
                    ent[j].body.slam_recent = 1;
            }
            if (!b3_carcol_resolve(list[i], list[j], &ct)) continue;
            /* SCORE-CLASSIFIER: the collision dispatcher at 0x00027500
             * notifies the score object of every car-on-car contact --
             * FUN_00197920 for the near-miss cancel and FUN_001979E0 for the
             * rubbing timers, both taking *(racecar+0x13F4)+0x10D0.  That is
             * what makes a takedown a takedown instead of a near miss. */
            score_contact_pair(ent[i].veh, ent[j].veh,
                               traffic_owner[i], traffic_owner[j]);
            {   /* B3_PAIR_DUMP=<path>: append the EXACT inputs of a heavy
                 * contact so tools/replay_pair.py can push the same pair
                 * through retail's FUN_001121F0 under Unicorn and diff the
                 * contact normal / vn against this port.  Threshold keeps it
                 * to the contacts worth diffing. */
                static FILE* pd = NULL; static int pd_state = 0;
                if (!pd_state) {
                    const char* e = getenv("B3_PAIR_DUMP");
                    pd = e && *e ? fopen(e, "w") : NULL;
                    pd_state = 1;
                }
                if (pd && ct.impact > 2000.0f) {
                    const B3CarBody* A = list[i]; const B3CarBody* B = list[j];
                    fprintf(pd,
                        "pair t=%.3f impact=%.1f vn=%.3f n=%.4f,%.4f,%.4f "
                        "p=%.3f,%.3f,%.3f\n"
                        "  A m=%.1f type=%d hull=%s pos=%.3f,%.3f,%.3f "
                        "yaw=%.5f v=%.3f,%.3f,%.3f w=%.4f,%.4f,%.4f "
                        "crashed=%d drift=%d yawin=%.4f "
                        "ii=%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g\n"
                        "  B m=%.1f type=%d hull=%s pos=%.3f,%.3f,%.3f "
                        "yaw=%.5f v=%.3f,%.3f,%.3f w=%.4f,%.4f,%.4f "
                        "crashed=%d drift=%d yawin=%.4f "
                        "ii=%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g,%.8g\n",
                        g_race_time, ct.impact, ct.vn_mph,
                        ct.normal[0], ct.normal[1], ct.normal[2],
                        ct.point[0], ct.point[1], ct.point[2],
                        A->mass, A->type,
                        ent[i].veh ? carcol_dump_id(ent[i].veh)
                                   : B3_TRAFFIC_CARS[g_traffic[traffic_owner[i]].car].id,
                        A->rb->frame[3][0], A->rb->frame[3][1], A->rb->frame[3][2],
                        atan2f(A->rb->frame[2][0], A->rb->frame[2][2]),
                        A->rb->vel[0], A->rb->vel[1], A->rb->vel[2],
                        A->rb->omega[0], A->rb->omega[1], A->rb->omega[2],
                        A->crashed, A->drift_state, A->yaw_input,
                        A->rb->inv_inertia_world[0][0], A->rb->inv_inertia_world[0][1],
                        A->rb->inv_inertia_world[0][2],
                        A->rb->inv_inertia_world[1][0], A->rb->inv_inertia_world[1][1],
                        A->rb->inv_inertia_world[1][2],
                        A->rb->inv_inertia_world[2][0], A->rb->inv_inertia_world[2][1],
                        A->rb->inv_inertia_world[2][2],
                        B->mass, B->type,
                        ent[j].veh ? carcol_dump_id(ent[j].veh)
                                   : B3_TRAFFIC_CARS[g_traffic[traffic_owner[j]].car].id,
                        B->rb->frame[3][0], B->rb->frame[3][1], B->rb->frame[3][2],
                        atan2f(B->rb->frame[2][0], B->rb->frame[2][2]),
                        B->rb->vel[0], B->rb->vel[1], B->rb->vel[2],
                        B->rb->omega[0], B->rb->omega[1], B->rb->omega[2],
                        B->crashed, B->drift_state, B->yaw_input,
                        B->rb->inv_inertia_world[0][0], B->rb->inv_inertia_world[0][1],
                        B->rb->inv_inertia_world[0][2],
                        B->rb->inv_inertia_world[1][0], B->rb->inv_inertia_world[1][1],
                        B->rb->inv_inertia_world[1][2],
                        B->rb->inv_inertia_world[2][0], B->rb->inv_inertia_world[2][1],
                        B->rb->inv_inertia_world[2][2]);
                    fflush(pd);
                }
            }
            for (int side = 0; side < 2; side++) {
                CarColEntry* me = side ? &ent[j] : &ent[i];
                CarColEntry* ot = side ? &ent[i] : &ent[j];
                int slot = me->veh ? (int)(me->veh - g_vehicles) : -1;
                if (slot < 0 || slot >= 8) continue;
                g_carcol_dbg[slot].t = g_race_time;
                g_carcol_dbg[slot].vn = ct.vn_mph;
                g_carcol_dbg[slot].impact = ct.impact;
                g_carcol_dbg[slot].is_traffic = ot->veh ? 0 : 1;
                g_carcol_dbg[slot].other = ot->veh
                    ? (int)(ot->veh - g_vehicles) : ot->traffic;
                g_carcol_dbg[slot].crash_a = ct.crash_a;
                g_carcol_dbg[slot].crash_b = ct.crash_b;
                {   /* the other body's own speed: a "head-on" that reads as
                     * vn == the player's speed means the other car was
                     * STATIONARY, not oncoming. */
                    const B3CarBody* ob = side ? list[i] : list[j];
                    float vx = ob->rb->vel[0], vz = ob->rb->vel[2];
                    g_carcol_dbg[slot].other_mph =
                        sqrtf(vx * vx + vz * vz) * 2.23693633f;
                }
            }
            if (ent[i].veh && ent[j].veh) {
                carcol_slam_racers(ent[i].veh, ent[j].veh, &ct);
                carcol_wreck_takedown(ent[i].veh, ent[j].veh, &ct);
            }
            else {
                Vehicle* v = ent[i].veh ? ent[i].veh : ent[j].veh;
                int ti = traffic_owner[i] >= 0 ? traffic_owner[i]
                                                    : traffic_owner[j];
                /* TRAFFIC vs TRAFFIC.  Retail runs these through the SAME
                 * FUN_001121F0 gate as any other car pair -- FUN_0010FBC0
                 * maps type 2 (traffic) to interaction class 0, and
                 * DAT_0039AE50[0][0] = 1, so a traffic pair is allowed to
                 * crash exactly like a racer pair (verified by executing
                 * the retail function under Unicorn: B.type 0/1/2/3 all
                 * take the identical 150 mph bar).  This branch used to do
                 * nothing at all, because it keys on a racer `v` that is
                 * NULL when both bodies are traffic -- so traffic could
                 * never wreck traffic, and retail's pile-up cascade could
                 * never seed.  That matters: once a traffic car IS wrecked,
                 * FUN_00113960 wrecks a racer at impact > 2500 instead of
                 * 5000, i.e. from ~15 mph of closing. */
                if (!v && traffic_owner[i] >= 0 && traffic_owner[j] >= 0
                    && traffic_owner[i] != traffic_owner[j]) {
                    TrafficCar* ta = &g_traffic[traffic_owner[i]];
                    TrafficCar* tb = &g_traffic[traffic_owner[j]];
                    int ca = ta->crashed_until > g_race_time;
                    int cb = tb->crashed_until > g_race_time;
                    /* b3_carcol_resolve() reproduces FUN_00111CD0's
                     * ordering: with both bodies alive it runs
                     * FUN_001121F0 and crash_a/crash_b stay positional,
                     * but with one already crashed it SWAPS so the wreck
                     * is `b`, and FUN_00113960 then reports the surviving
                     * car in crash_a.  Map accordingly rather than by
                     * position. */
                    if (!ca && !cb) {
                        if (ct.crash_a) {
                            ta->crashed_until = g_race_time + 5.0f;
                            ta->speed = 0.0f;
                        }
                        if (ct.crash_b) {
                            tb->crashed_until = g_race_time + 5.0f;
                            tb->speed = 0.0f;
                        }
                    } else if (ca != cb && ct.crash_a) {
                        TrafficCar* fresh = ca ? tb : ta;
                        fresh->crashed_until = g_race_time + 5.0f;
                        fresh->speed = 0.0f;
                    }
                }
                if (v && ti >= 0) {
                    TrafficCar* t = &g_traffic[ti];
                    if (getenv("B3_TRAFFIC_HIT"))
                    {
                        float cvx = v->vel.x - (sinf(t->yaw) * t->speed);
                        float cvz = v->vel.z - (-cosf(t->yaw) * t->speed);
                        float closing = sqrtf(cvx*cvx + cvz*cvz) * 2.23693633f;
                        float pspd = sqrtf(v->vel.x*v->vel.x + v->vel.z*v->vel.z) * 2.23693633f;
                        printf("[thit] t=%.2f car %d vs traffic %d  vn=%.1f mph "
                               "closing=%.1f pspd=%.1f tspd=%.1f "
                               "impact=%.0f crash_a=%d crash_b=%d tcrashed=%d\n",
                               g_race_time, (int)(v - g_vehicles), ti,
                               ct.vn_mph, closing, pspd, t->speed*2.23693633f,
                               ct.impact, ct.crash_a, ct.crash_b,
                               t->crashed_until > g_race_time);
                    }
                    if (ct.crash_a && ct.crash_b) {
                        t->crashed_until = g_race_time + 5.0f;
                        t->speed = 0.0f;
                        carcol_wreck_racer_from_traffic(v, t, &ct);
                    } else if (ct.crash_a
                               && t->crashed_until > g_race_time) {
                        carcol_wreck_racer_from_traffic(v, t, &ct);
                    }
                }
            }
        }
    }
    for (int i = 0; i < n; i++) carcol_writeback(&ent[i], g_delta_time);
    for (int i = 0; i < g_traffic_n; i++) {
        TrafficCar* t = &g_traffic[i];
        if (!t->active || !t->streamed || t->crashed_until > g_race_time) continue;
        b3_rigid_body_integrate(&t->rb, t->mass_kg, 0.0f, 0, 0,
                                g_delta_time);
        if (t->trailer_ready)
            b3_rigid_body_integrate(&t->trailer_rb, t->trailer_mass_kg,
                                    0.0f, 0, 0, g_delta_time);
        t->pos = (Vec3){t->rb.frame[3][0],
                        t->rb.frame[3][1] + 0.5f + g_traffic_ymin[t->car],
                       -t->rb.frame[3][2]};
        t->yaw = atan2f(t->rb.frame[2][0], t->rb.frame[2][2]);
        traffic_trailer_update(t);
    }
    for (int i = 0; i < 8; i++) {
        Vec3* k = &g_carcol_knock[i];
        float d = 1.0f - 4.0f * g_delta_time; if (d < 0.0f) d = 0.0f;
        k->x *= d; k->z *= d;
    }
}

static void panels_pieces_update(float dt) {
    for (int slot = 0; slot < g_num_vehicles && slot < 8; slot++) {
        B3PanelSet* panels = &g_panels[slot];
        float ground_y[B3_PANEL_MAX];
        int active = 0;
        for (int panel = 0; panel < panels->n; panel++) {
            B3PanelPiece* piece = &panels->piece[panel];
            if (!piece->active) continue;
            float normal[3];
            float height;
            active = 1;
            if (b3_collision_ready()
                && b3_ground_probe(piece->frame[3][0], piece->frame[3][1],
                                   piece->frame[3][2], &height, normal) >= 0) {
                ground_y[panel] = height;
            } else {
                Vec3 route;
                loop_closest(g_cl, ROUTE_COUNT, piece->frame[3][0],
                             piece->frame[3][2], &route, NULL);
                ground_y[panel] = route.y;
            }
        }
        if (active) b3_panels_pieces_update(panels, ground_y, dt);
    }
}

static void game_update(void) {
    // Update race timer
    if (g_state == RACING) {
        g_race_time += g_delta_time;
        g_real_clock_dbg += g_tdfx_real_dt;
        
        // Check lap completion
        if (g_current_lap >= g_lap_count) {
            g_state = FINISHED;
            printf("[Burnout3] Race finished! Time: %.1fs\n", g_race_time);
        }
        
        // Check time limit
        if (g_race_time >= g_time_limit) {
            g_state = FINISHED;
            printf("[Burnout3] Time's up!\n");
        }
    }
    
    // Update vehicles (slot 0 is the player)
    aggro_world_build(g_delta_time);   /* RE_AI section 14 */
    /* TD-RULES 8a: FUN_00105BD0's authority ladder keys off the squared
     * distance to the nearest VIEWED racecar.  Single player: that is the
     * player's car.  A car roughly 40-79 m from the player has its crash
     * thresholds collapsing toward the 0.03 floor; past the 125 m base
     * radius the crash entries are switched off entirely. */
    for (int i = 0; i < g_num_vehicles && i < B3_TDR_MAX_CARS; i++) {
        float dx = g_vehicles[i].pos.x - g_player.pos.x;
        float dy = g_vehicles[i].pos.y - g_player.pos.y;
        float dz = g_vehicles[i].pos.z - g_player.pos.z;
        g_tdr.car[i].view_dist2 = dx * dx + dy * dy + dz * dz;
    }
    for (int i = 0; i < g_num_vehicles; i++) {
        vehicle_update(&g_vehicles[i], g_delta_time);
    }

    // Traffic: drive the oncoming line; carcol_pass() resolves its contacts.
    traffic_update(g_delta_time);
    /* PROPS: integrate the knocked props, then collide every racer against
     * them.  Retail routes a (car-ish, world-slot-type-5) pair through
     * FUN_00111CD0's third branch @0x00111DEB to FUN_00113890, which
     * promotes the prop into one of the 16 class-6 rigid bodies
     * (FUN_00114730) and resolves the contact with the generic solver;
     * b3_props_collide_car() is that boundary.  Each contact is then handed
     * to FUN_00112E70's port with the RECOVERED object class -- 6 for every
     * static.dat prop (FUN_0010FBC0's jump table @0x0010FC04 sends handle
     * types 5 and 6 there), and DAT_0039AE50 row 6 is all zeros, so the
     * table itself returns the verdict `a prop never crashes a car`.  That
     * is why you can plough a cone field at 200 mph and keep driving. */
    if (b3_props_ready()) {
        b3_props_update(g_delta_time);
        for (int i = 0; i < g_num_vehicles; i++) {
            Vehicle* pv = &g_vehicles[i];
            if (!pv->active) continue;
            float ppos[3] = { pv->pos.x, pv->pos.y, pv->pos.z };
            float pvel[3] = { pv->vel.x, pv->vel.y, pv->vel.z };
            float pext[3] = { g_car_ext[i][0], g_car_ext[i][1],
                              g_car_ext[i][2] };
            B3PropHit ph[8];
            int nph;
            /* PROP-PHYSICS: the recovered generic solver (FUN_00113960 ->
             * FUN_0010F8D0) puts BOTH masses and BOTH world inverse inertias
             * in its denominator and takes the relative velocity at the
             * contact POINT, so it needs the car's real rigid body -- and
             * @0x00113B57 an UN-CRASHED car is forced to role 2 (immovable)
             * while a wreck takes its half back.  Hand it the live body when
             * the full pipeline owns the car; the pos/vel entry point is the
             * pre-fsim_ready fallback. */
            if (pv->fsim_ready)
                nph = b3_props_collide_rb(i, &pv->fsim.rb, pv->fsim.mass,
                                          pext,
                                          pv->crashed_until > 0.0f ? 1 : 0,
                                          ph, 8);
            else
                nph = b3_props_collide_car(i, ppos, pvel, pv->rot.y,
                                           pext, ph, 8);
            for (int k = 0; k < nph; k++) {
                /* game space = harness with z negated (RE_NOTES 12) */
                float vr[3] = { ph[k].vrel[0], ph[k].vrel[1],
                                -ph[k].vrel[2] };
                float on[3] = { ph[k].normal[0], ph[k].normal[1],
                                -ph[k].normal[2] };
                if (i < B3_TDR_MAX_CARS && pv->fsim_ready
                    && pv->crashed_until <= 0.0f)
                    b3_td_object_contact(&g_tdr, i, g_race_time, vr, on,
                                         pv->fsim.mass, ph[k].obj_class,
                                         b3_td_object_class(0, 0),
                                         g_race_time < pv->immune_until);
                if (getenv("B3_PROP_TRACE"))
                    printf("[prop] car %d hit inst %d model %d class %d "
                           "objcls %d mass %.0f closing %.1f mph J %.0f\n",
                           ph[k].car, ph[k].instance, ph[k].model,
                           ph[k].prop_class, ph[k].obj_class, ph[k].mass,
                           ph[k].closing_mph, ph[k].impulse);
            }
        }
    }
    panels_pieces_update(g_delta_time);
    // traffic_interact()'s capsule pass would fight the recovered hull
    // separation, so it remains disabled.  carcol_pass() is authoritative;
    // the reference is retained only to keep this change focused.
    (void)traffic_interact;

    // SFX: cooldown counters (60 Hz, the units the emitters stamp) and the
    // listener the 15/50-unit distance roll-off is measured from.
    b3_sfx_tick();
    b3_sfx_set_listener(g_player.pos.x, g_player.pos.y, g_player.pos.z);
    /* BOOSTFX: the exhaust-flame level, FUN_0017A480's tail
     * (boostRecord+0x14 = carObj+0x11B0).  Every car, not just the
     * player -- FUN_0017F730 runs the emitter for every car object and
     * the AI boosts.  g_delta_time is the dilated dt, DAT_004AE1FC. */
    for (int i = 0; i < g_num_vehicles; i++)
        b3_boostfx_update(i, g_vehicles[i].bar.boosting,
                          g_vehicles[i].crashed_until > 0.0f,
                          g_delta_time);
    /* SFX: the racecar boost chain -- BoostIn on the rising edge, the
     * looping BoostLoop while boosting, BoostOut on release, cut on a
     * crash (FUN_00136F80's four waves; burnout3_sfx.h BOOST AUDIO). */
    b3_sfx_boost_tick(g_player.bar.boosting,
                      g_player.crashed_until > 0.0f);
    /* SFX-CRASH: the crash-state audio laws (burnout3_sfx.h section
     * "CRASH-STATE AUDIO"): the engine is starved to idle for as long as
     * the crash runs (FUN_0011BE50 @0x0011BE75..0x0011BED4) and the wreck's
     * own contacts play the crashed-car impact (FUN_0014D0F0's router).
     * g_delta_time is the dilated frame dt, the game's DAT_0060EA1C. */
    {
        float cpos[3] = { g_player.pos.x, g_player.pos.y, g_player.pos.z };
        float cvel[3] = { g_player.vel.x, g_player.vel.y, g_player.vel.z };
        b3_sfx_crash_tick(g_player.crashed_until > 0.0f, cpos, cvel,
                          g_player.sim.rpm, g_player.cfg.idle_rpm,
                          g_player.cfg.max_rpm, g_delta_time);
    }

    /* SFX-DRIVE: the racecar-audio per-frame LOOP emitters -- the port of
     * FUN_00136120's three children (the DRIVING-TIME LOOP EMITTERS block
     * comment in burnout3_sfx.h carries the whole evidence chain):
     *   FUN_0013DCA0 -> FUN_0013DE10 x2  the tyre SKID/SQUEAL samples
     *   FUN_00136610                     the ROAD SURFACE beds
     *   FUN_00136C50                     the GEAR shot
     * Every feed is a live field of the vehicle pipeline's own wheel
     * records (veh+0x820 stride 0xC0: +0x50 radius, +0x5C omega,
     * +0xB0 surface, +0xB3 contact) -- nothing is synthesised. */
    {
        static B3SfxDriveState s_drive;
        B3SfxDriveIn din;
        float cs = cosf(g_player.rot.y), sn = sinf(g_player.rot.y);
        memset(&din, 0, sizeof din);
        din.wheel_count = g_player.fsim.wheel_count > 4
                        ? 4 : g_player.fsim.wheel_count;
        if (din.wheel_count <= 0) din.wheel_count = 4;
        /* harness frame: fwd = (sin, 0, -cos), right = (cos, 0, sin) */
        din.fwd[0]   = sn;  din.fwd[1]   = 0.0f; din.fwd[2]   = -cs;
        din.right[0] = cs;  din.right[1] = 0.0f; din.right[2] = sn;
        din.pos[0] = g_player.pos.x;
        din.pos[1] = g_player.pos.y;
        din.pos[2] = g_player.pos.z;
        for (int wq = 0; wq < 4; wq++) {
            const B3WheelSim* ws = &g_player.fsim.wheel[wq];
            din.wheel[wq].omega   = ws->omega;
            din.wheel[wq].radius  = ws->radius;
            din.wheel[wq].surface = (int)(ws->surface & 0xFF);
            din.wheel[wq].contact = ws->contact;
            din.wheel[wq].mode    = -1;   /* wheel+0x78 is not ported */
            din.wheel[wq].pos[0]  = g_player.pos.x
                                  + din.right[0] * ws->local_x
                                  + din.fwd[0]   * ws->local_z;
            din.wheel[wq].pos[1]  = g_player.pos.y;
            din.wheel[wq].pos[2]  = g_player.pos.z
                                  + din.right[2] * ws->local_x
                                  + din.fwd[2]   * ws->local_z;
        }
        din.speed_ms = g_player.sim.speed;
        din.gear     = g_player.fsim.trans.gear;
        din.crashed  = g_player.crashed_until > 0.0f;
        /* A wreck runs the crash integrator, not the wheel pipeline, so the
         * wheel records go stale -- stop the loops rather than drive them
         * from stale state.  GLUE (retail keeps running FUN_00136120). */
        if (g_state == RACING && !din.crashed)
            b3_sfx_drive_tick(&s_drive, &din);
        else
            b3_sfx_drive_stop(&s_drive);
    }
    /* SFX-DRIVE: the body SCRAPE loop pair off the wall-grind latch.  The
     * crossfade weight and level are the harness's (RE_SFX.md marks the
     * caller's crossfade [S]); the along-wall speed picks both. */
    {
        float sp = g_sfx_scrape_spd;
        float g  = sp > 3.0f ? (sp - 3.0f) / 12.0f : 0.0f;   /* level  */
        float hi = sp > 8.0f ? (sp - 8.0f) / 52.0f : 0.0f;   /* LO->HI */
        float pos[3] = { g_sfx_scrape_pos.x, g_sfx_scrape_pos.y,
                         g_sfx_scrape_pos.z };
        if (g > 1.0f) g = 1.0f;
        if (hi > 1.0f) hi = 1.0f;
        if (getenv("B3_SCRAPE_TRACE") && g_sfx_scrape_hit)
            printf("[scrape] t=%.2f spd %.1f m/s level %.2f hi %.2f\n",
                   g_race_time, sp, g, hi);
        b3_sfx_scrape_tick(g_sfx_scrape_hit && g_state == RACING, g,
                           hi, pos);
        g_sfx_scrape_hit = 0;
    }

    // Vehicle collision -- the RECOVERED car-vs-car chain
    // (src/burnout3_carcol.c: FUN_00110AF0 broad phase + FUN_0010AC20 convex
    // hull narrow phase + FUN_001121F0 / FUN_00113960 response and slam
    // classification; tools/validate_carcol.py 730/730).  The separation
    // lands in +0x130 and the impulses/forces in +0x110/+0x120 and
    // +0xF0/+0x100, which the next b3_vehicle_step_full consumes -- the same
    // frame order the game itself uses (FUN_00110AF0 once, FUN_0011BE50's
    // two substeps after it).
    carcol_pass();
    tdr_frame_pass();
}

// ============================================================
// Main entry point (harness; the game's real entry is 0x001D2807)
// ============================================================

int main(int argc, char* argv[]) {
#ifdef __ANDROID__
    /* ANDROID PORT: chdir into the extracted asset tree so every relative
     * "build/..." path below resolves unchanged, and route printf to
     * logcat.  Must precede the first asset touch. */
    b3_android_boot();
#endif
    b3_ai_init();
    (void)argc; (void)argv;
    
    printf("========================================\n");
    printf("Burnout 3: Takedown - RE harness\n");
    printf("========================================\n");
    printf("Target:   default.xbe (Xbox), entry 0x001D2807\n");
    printf("Engine:   RenderWare RW36 (confirmed via $Id strings)\n");

    printf("Ghidra functions analyzed: 7,434 (corrected ELF mapping)\n");
    printf("NOTE: gameplay below is an original harness, not decompiled code.\n\n");
    
    // Init SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO
                 | SDL_INIT_GAMECONTROLLER) < 0) {
        printf("SDL init failed: %s\n", SDL_GetError());
        return 1;
    }
    pad_open_first();   // Xbox 360 / any SDL game controller, if present
    
#ifdef __ANDROID__
    /* ANDROID PORT: the real context is GLES 2.0; gl4es re-implements the
     * 2.1 COMPATIBILITY surface this harness draws through (immediate mode,
     * display lists, GL_QUADS, glPushAttrib) on top of it. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    // Road decals lift only ~0.011 units above the road; a 16-bit depth
    // buffer (the SDL default on some drivers) stops resolving that at
    // ~11 m and the markings shutter. Request 24 explicitly.
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    
    // Resolution: B3_RES=WxH overrides; default 2048x1536 (2x the classic
    // 1024x768). The window stays freely resizable by dragging -- the whole
    // pipeline tracks the live size per frame -- and SDL clamps the initial
    // size to the usable display bounds if the default doesn't fit.
    int win_w = 2048, win_h = 1536;
    {
        const char* res = getenv("B3_RES");
        int rw, rh;
        if (res && sscanf(res, "%dx%d", &rw, &rh) == 2
            && rw >= 320 && rh >= 240) {
            win_w = rw;
            win_h = rh;
        }
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
            if (win_w > usable.w) win_w = usable.w;
            if (win_h > usable.h) win_h = usable.h;
        }
    }
    // GLUE (display quality): MSAA. B3_MSAA=N samples (default 4, 0 off).
    // Falls back to no-multisample if the driver refuses (offscreen/soft
    // GL often does). Framebuffer reads (postfx copies, captures) resolve
    // implicitly on the default framebuffer, so the pipeline is unchanged.
    int msaa = 4;
    {
        const char* e = getenv("B3_MSAA");
        if (e && *e) {
            msaa = atoi(e);
            if (msaa < 0) msaa = 0;
            if (msaa > 16) msaa = 16;
        }
    }
    if (msaa > 1) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaa);
    }
    g_window = SDL_CreateWindow("Burnout 3: Takedown",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                win_w, win_h,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
                                                  | SDL_WINDOW_RESIZABLE);
    if (!g_window && msaa > 1) {
        // Driver refused the multisample visual: retry plain.
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        msaa = 0;
        g_window = SDL_CreateWindow("Burnout 3: Takedown",
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    win_w, win_h,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
                                                      | SDL_WINDOW_RESIZABLE);
    }
    if (!g_window) {
        printf("Window creation failed: %s\n", SDL_GetError());
        return 1;
    }

    g_gl_context = SDL_GL_CreateContext(g_window);
    if (!g_gl_context && msaa > 1) {
        // Some drivers fail at context time instead: retry plain.
        SDL_DestroyWindow(g_window);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        msaa = 0;
        g_window = SDL_CreateWindow("Burnout 3: Takedown",
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    win_w, win_h,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
                                                      | SDL_WINDOW_RESIZABLE);
        if (g_window) g_gl_context = SDL_GL_CreateContext(g_window);
    }
    if (!g_gl_context) {
        printf("GL context creation failed: %s\n", SDL_GetError());
        return 1;
    }
#ifdef __ANDROID__
    /* ANDROID PORT: gl4es is built with NO_INIT_CONSTRUCTOR -- bring it up
     * now that the ES context is current, before the first gl* call.  Then
     * take the window size from the surface Android actually gave us
     * (SDL ignores the requested size on Android). */
    b3_android_gl_init();
    SDL_GL_GetDrawableSize(g_window, &win_w, &win_h);
#endif
    if (msaa > 1) {
        int got = 0;
        SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &got);
        glEnable(GL_MULTISAMPLE);
        printf("[Burnout3] MSAA: requested %dx, got %dx\n", msaa, got);
    }
    
    // Generate track and meshes
    init_paths();
    nav_load();
    route_lane_fixup();   // shift midline points onto the driven lane
    generate_track();
    generate_road_mesh();
    load_real_track();
    build_collision();
    generate_ground_mesh();
    
    // Init vehicles
    init_vehicles();
    
    // Init audio
    audio_init();
    
    // Init renderer
    render_init(win_w, win_h);
    IMG_Init(IMG_INIT_PNG);
    b3_carfx_init();                 /* CARFX: art + shine program (before
                                      * load_car_meshes fills its tables) */
    b3_boostfx_init();               /* BOOSTFX: coronaboost pools 1/2 */
    /* CRASH-SHOW H8: the particle FX engine (crash dust/debris, tyre
     * smoke, offroad dust, grind sparks).  Art from Data/Global.txd via
     * tools/extract_particlefx_art.py. */
    b3_pfx_init("build/particlefx");
    /* CARFX environment: BOTH inputs recovered [C] -- the nine SH
     * coefficients are .rdata literals (0x003B1900..) installed by
     * b3_carfx_init(); the light RGB is enviro.dat bytes 0x60..0x6B,
     * selected per track here (docs/RE_CARFX.md 2.7/2.8). */
    b3_carfx_set_track(getenv("B3_POSTFX_TRACK")
                       ? getenv("B3_POSTFX_TRACK") : "US_C3_V1");
    /* POSTFX: world sky + present composite + gamma (burnout3_postfx.c).
     * The gamma ramp is no longer installed through SDL here; it is a GL
     * pass at the end of render_frame(). */
    b3_postfx_init();
    b3_postfx_set_art("build/postfx", getenv("B3_POSTFX_TRACK")
                      ? getenv("B3_POSTFX_TRACK") : "US_C3_V1");
    b3_postfx_gl_init();
    load_track_textures();
    load_car_meshes();
    traffic_init();                  // real .bgd traffic set + spawn table
    {   /* PROPS: destructible track props -- static.dat's +0x3C model table
         * and +0x48 instance transforms, baked by tools/extract_props.py to
         * build/tracks/<ID>/props.bin.  Data-driven: no track constants. */
        const char* tid = getenv("B3_TRACK");
        if (!tid) tid = getenv("B3_POSTFX_TRACK");
        if (!tid) tid = "US_C3_V1";
        char pdir[256];
        snprintf(pdir, sizeof pdir, "build/tracks/%s", tid);
        b3_props_load(pdir);
    }
    b3_hud_init("build/frontend");   // real HUD art from Data/Global.txd
    b3_score_events_init();          // Score/* params -> the retail VDB tune
    b3_tdfx_init();                  // takedown slow-mo / camera / callout
    b3_tdfx_event_reset();           // FUN_00025AB0 @0x00025AE5: one crash-
                                     // presentation credit per car per event
    globalus_load("build/Globalus.bin");  // retail string table (callout text)
    
    printf("\nControls:\n");
    printf("  W/Up    - Throttle\n");
    printf("  S/Down  - Brake\n");
    printf("  A/D     - Steer\n");
    printf("  Space   - Boost\n");
    printf("  Enter   - Start Race (from menu)\n");
    printf("  R       - Restart (after crash/race)\n");
    printf("  T       - Dump gamestate + screenshot to build/ (debugging)\n");
    printf("  ESC     - Quit\n\n");
    
    Uint32 last_time = SDL_GetTicks();
    
    while (g_running) {
        Uint32 current_time = SDL_GetTicks();
        g_delta_time = (current_time - last_time) / 1000.0f;
        if (g_delta_time > 0.1f) g_delta_time = 0.016f; // Cap at 60fps
        // B3_FIXED_DT=<seconds>: deterministic step for headless testing
        // (SDL_VIDEODRIVER=offscreen runs at >1000 fps, where a wall-clock
        // dt of <1 ms distorts the per-frame parts of the sim).
        static float fixed_dt = -1.0f;
        if (fixed_dt < 0.0f) {
            const char* fd = getenv("B3_FIXED_DT");
            fixed_dt = fd ? (float)atof(fd) : 0.0f;
        }
        // FRAME-LOCKED timing, the retail arrangement: the Xbox steps its
        // simulation once per rendered frame at the NOMINAL period (1/60,
        // 0x003B1838), divided by the dilation divisor (DAT_0060EA1C =
        // period/divisor) -- wall-clock time is never consumed by the sim.
        // At 58-62 fps the whole game runs within +-3% of real time, with
        // rendering and physics 1:1 (no tick beat, no interpolation).
        // B3_FIXED_DT still overrides the nominal period for tests.
        g_tdfx_real_dt = (fixed_dt > 0.0f) ? fixed_dt : 0.016666668f;
        g_delta_time   = b3_tdfx_update(g_tdfx_real_dt);
        last_time = current_time;
        g_total_time += g_delta_time;
        g_frame_count++;
        
        process_input();

        // There is no menu UI, so a boot-time MENU state just looks like a
        // frozen game that ignores input. Start racing immediately.
        if (g_state == MENU) {
            g_state = RACING;
            printf("[Burnout3] Race started -- drive with W/A/S/D, boost with Space\n");
            b3_music_start_race();   /* MUSIC: pick + start the EA TRAX song */
            /* CRASH BED: retail picks the race's crashNN.rws here too
             * (0x0014C269) and leaves it buffered and paused until a
             * car crashes.  Sequential rotation over crash1..20. */
            b3_music_crash_arm();
            // ROLLING START (user-requested; retail starts standing): every
            // car launches at exactly 50 mph (22.352 m/s, the same exact-mph
            // constant family as the recovered relaunch speeds) along its
            // grid heading, so nobody bogs into the pack off the line.
            for (int rs = 0; rs < g_num_vehicles; rs++) {
                Vehicle* rv = &g_vehicles[rs];
                if (!rv->active) continue;
                rv->vel = (Vec3){sinf(rv->rot.y) * 22.352f, 0.0f,
                                 -cosf(rv->rot.y) * 22.352f};
                rv->sim.speed = 22.352f;
                rv->fsim_ready = 0;   // re-init seeds the body velocity
            }
        }

        // B3_POS_DEBUG=1: one-shot lap/progress census at t=5 and t=22
        // (the phantom-lap diagnostic; harmless gated print).
        {
            static int pd = -1, shot5 = 0, shot22 = 0;
            if (pd < 0) pd = getenv("B3_POS_DEBUG") != NULL;
            if (pd && ((!shot5 && g_race_time >= 5.0f)
                       || (!shot22 && g_race_time >= 22.0f))) {
                if (g_race_time >= 22.0f) shot22 = 1; else shot5 = 1;
                for (int ci = 0; ci < g_num_vehicles; ci++)
                    printf("[pos] t=%.1f car %d lap %d prog %.4f\n",
                           g_race_time, ci, g_vehicles[ci].lap,
                           g_vehicles[ci].track_progress);
            }
        }
        if ((g_state == RACING || g_state == CRASHED) && !g_paused) {
            {   // B3_TEST_CRASH_AT=<sec>: push the player through the real
                // wall-crash consequence path once (wreck + presentation +
                // trace), to exercise the crash plumbing without a live hit.
                static float tca = -2.0f;
                if (tca < -1.0f) {
                    const char* e = getenv("B3_TEST_CRASH_AT");
                    tca = e ? (float)atof(e) : -1.0f;
                }
                if (tca > 0.0f && g_race_time >= tca
                    && g_player.crashed_until <= 0.0f) {
                    tca = -1.0f;
                    Vehicle* v = &g_player;
                    v->crashed_until = g_race_time + 5.0f;
                    v->immune_until  = g_race_time + crash_latch_for(v);
                    /* PANELS: a real head-on wall normal (opposite the
                     * car's travel).  The old (0,0,0) made the contact
                     * impulse vanish, so the injected crash was a straight
                     * upright slide.  The WALL entry adds no kick of its own
                     * (the crash-director magnitude is 0.0 BSS), so this
                     * contact impulse is the whole entry -- the 1:1
                     * wall-crash shape. */
                    {
                        float sp = sqrtf(v->vel.x * v->vel.x
                                       + v->vel.z * v->vel.z);
                        Vec3 hn = sp > 0.1f
                            ? (Vec3){-v->vel.x / sp, 0.0f, -v->vel.z / sp}
                            : (Vec3){0.0f, 0.0f, 1.0f};
                        wreck_begin_for(v, v->pos, hn, v->vel,
                                        B3_WRECK_ENTRY_WALL);
                    }
                    {   /* the crash entry retail always runs: FUN_0010DCA0
                         * -> FUN_0010DD20 -> game-context +0x48 ->
                         * FUN_00197750 -> FUN_00197430.  Without it the
                         * injected crash arms no takedown claims and so does
                         * NOT exercise "the real wall-crash consequence path"
                         * its own comment promises. */
                        B3TdCause tc;
                        b3_td_cause_wall(&tc, 0);
                        b3_td_on_crash(&g_tdr, g_race_time, 0, &tc, NULL);
                        tdr_trace_claims("test-crash", 0);
                    }
                    printf("[Burnout3] t=%.2f TEST CRASH injected\n",
                           g_race_time);
                }
            }
            game_update();
            crash_trace_tick();   // user-requested per-frame crash log
        }
        {   /* test harness: stop after B3_EXIT_AT race seconds */
            static float exit_at = -2.0f;
            if (exit_at < -1.0f) {
                const char* e = getenv("B3_EXIT_AT");
                exit_at = e ? (float)atof(e) : -1.0f;
            }
            if (exit_at > 0.0f && g_race_time >= exit_at) g_running = 0;
        }
        
        {   /* CRASH BED: FUN_00150E80(mgr, crash_active).  Retail ORs
             * every local player's crashing byte (+0x236) into that
             * argument at 0x0014CAA0, and picks the bed's layer off
             * the time divisor at 0x00150F4F -- divisor 1 plays
             * aGenCrashNN, the dilated crash window plays zSloCrashNN.
             * The 0.5 s release is measured on the dilated clock, so
             * the dt handed over is g_delta_time. */
            B3TdfxStatus cbst;
            b3_tdfx_status(&cbst);
            b3_music_crash_tick(g_player.crashed_until > 0.0f,
                                cbst.divisor, g_delta_time);
        }
        b3_music_pump();                 /* MUSIC: refill the stream */
        render_frame();
        // Screenshot/dump captures read the BACK buffer, so they must run
        // BEFORE the swap -- after it the back buffer holds stale previous-
        // frame data (the "half of the shot is the last frame" artifact;
        // never visible in play, only in captures).

        // T key: dump gamestate + screenshot to build/ for debugging
        // handoffs (numbered so several dumps in one session don't clobber).
        // B3_DUMP_FRAME=<n> triggers the same dump headless.
        static int dump_frame = -2;
        if (dump_frame == -2) {
            const char* df = getenv("B3_DUMP_FRAME");
            dump_frame = df ? atoi(df) : -1;
        }
        if (dump_frame == g_frame_count) g_debug_dump_req = 1;
        if (g_debug_dump_req) {
            g_debug_dump_req = 0;
            debug_dump();
        }

        // Debug screenshot: B3_SHOT=<path.bmp> writes a frame (default 60,
        // override with B3_SHOT_FRAME) and exits. Used to verify rendering
        // without a human watching the window.
        const char* slamshot = getenv("B3_SLAM_SHOT");
        if (slamshot && g_aggro_shot_req) {
            g_aggro_shot_req = 0;
            int sw, sh;
            SDL_GetWindowSize(g_window, &sw, &sh);
            unsigned char* px = malloc((size_t)sw * sh * 4);
            if (px) {
                glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px);
                b3_gamma_apply_rgba(px, sw * sh);
                SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormatFrom(
                    px, sw, sh, 32, sw * 4, SDL_PIXELFORMAT_ABGR8888);
                if (sf) {
                    b3_postfx_flip_rows(px, sw, sh);   // burnout3_postfx.c 2b
                    SDL_SaveBMP(sf, slamshot);
                    SDL_FreeSurface(sf);
                    printf("[Burnout3] slam screenshot t=%.2f -> %s\n",
                           g_race_time, slamshot);
                }
                free(px);
            }
        }
        const char* shot = getenv("B3_SHOT");
        static int shot_frame = 0;
        if (!shot_frame) {
            const char* sf = getenv("B3_SHOT_FRAME");
            shot_frame = sf ? atoi(sf) : 60;
            if (shot_frame <= 0) shot_frame = 60;
        }
        if (shot && g_frame_count == shot_frame) {
            int sw, sh;
            SDL_GetWindowSize(g_window, &sw, &sh);
            unsigned char* px = malloc((size_t)sw * sh * 4);
            if (px) {
                glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px);
                b3_gamma_apply_rgba(px, sw * sh);
                SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(
                    px, sw, sh, 32, sw * 4, SDL_PIXELFORMAT_ABGR8888);
                if (s) {
                    // GL reads bottom-up; flip rows for the BMP.
                    b3_postfx_flip_rows(px, sw, sh);   // burnout3_postfx.c 2b
                    SDL_SaveBMP(s, shot);
                    SDL_FreeSurface(s);
                    printf("[Burnout3] screenshot -> %s\n", shot);
                }
                free(px);
            }
            g_running = 0;
        }

#ifdef __ANDROID__
        {   /* ANDROID PORT: the button overlay is drawn dead last -- after
             * every capture path -- so T-dumps keep showing pure game
             * rendering while the on-screen display carries the controls. */
            int tw, th;
            SDL_GetWindowSize(g_window, &tw, &th);
            b3_touch_draw(tw, th);
        }
#endif
        drive_log_tick();   /* B3_DRIVE_LOG: full state, every frame */

        SDL_GL_SwapWindow(g_window);   // after all back-buffer captures

        // FPS counter (every 60 frames)
        if (g_frame_count % 60 == 0) {
        if (getenv("B3_AI_TRACE")) {
            fprintf(stderr, "[trace] t=%.1f", g_race_time);
            for (int ti = 0; ti < g_num_vehicles; ti++)
                fprintf(stderr, "  c%d lap%d p%.3f %.0fmph",
                        ti, g_vehicles[ti].lap,
                        g_vehicles[ti].track_progress,
                        g_vehicles[ti].sim.speed * 2.2374146f);
            fprintf(stderr, "\n");
        }
            printf("[Burnout3] FPS: %d | %.0f mph | gear %d | %.0f rpm | Lap: %d/%d\n",
                   (int)(1.0f / g_delta_time), g_player.sim.speed * 2.2374146f,
                   g_player.sim.trans.gear, g_player.sim.rpm,
                   g_current_lap, g_lap_count);
            // B3_TELEM=1: per-second world position + route progress of every
            // car (stall-zone diagnosis; debug only).
            static int telem = -1;
            if (telem < 0) telem = getenv("B3_TELEM") != NULL;
            if (telem)
                for (int ti = 0; ti < g_num_vehicles; ti++)
                    printf("[telem] f%d car%d pos %.0f %.0f %.0f prog %.3f "
                           "spd %.1f stuck %.1f crash %.1f\n",
                           g_frame_count, ti, g_vehicles[ti].pos.x,
                           g_vehicles[ti].pos.y, g_vehicles[ti].pos.z,
                           g_vehicles[ti].track_progress,
                           g_vehicles[ti].sim.speed, g_vehicles[ti].stuck_time,
                           g_vehicles[ti].crashed_until);
        }
    }
    
    // Cleanup
    free(g_track.points);
    nav_free();
    free(g_road_mesh.verts);
    free(g_road_mesh.indices);
    free(g_ground_mesh.verts);
    free(g_ground_mesh.indices);
    
    SDL_GL_DeleteContext(g_gl_context);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    
    if (getenv("B3_AI_CRASHLOG")) {
        int tot = 0, rt = 0;
        printf("[aicrash] SUMMARY t=%.1f", g_race_time);
        for (int i = 0; i < g_num_vehicles; i++) {
            printf(" car%d=%d(td%d/tf%d/wl%d)", i, g_aicrash_n[i],
                   g_aicrash_kind[i][0], g_aicrash_kind[i][1],
                   g_aicrash_kind[i][2]);
            tot += g_aicrash_n[i];
            if (i > 0) rt += g_aicrash_n[i];
        }
        printf(" TOTAL=%d RIVALS=%d\n", tot, rt);
    }
    printf("\n[Burnout3] Goodbye!\n");
    return 0;
}
