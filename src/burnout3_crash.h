// burnout3_crash.h -- the crash/collision response recovered from the retail
// Burnout 3 XBE, plus the harness-facing wreck simulation built on it.
//
// Provenance markers (same scheme as docs/RE_NOTES.md):
//   [C]  execution-verified against the real x86 under Unicorn
//        (tools/validate_gameplay.py, "crash response" section)
//   [S]  read from the disassembly, self-consistent, no green case
//   GLUE original harness assembly around the ported pieces
//
// Ported functions and their sources (addresses = analyzed burnout3.elf):
//   FUN_0011AEF0  chassis-vs-world collision response   b3_crash_response  [C]
//   FUN_0011AC30  per-poly contact test + accumulation  b3_crash_poly_contact [C]
//   FUN_001B0C00/FUN_001B09C0  triangle-vs-bbox clipper b3_crash_box_clip  [C]
//   FUN_00106720  contact impulse magnitude             b3_crash_impulse   [C]
//   FUN_00106500  impulse at point (linear + angular)   b3_crash_apply_impulse_at_point [C]
//   FUN_001206D0  impulse routing (yaw/drift gate)      b3_crash_apply_impulse [C]
//   FUN_001066A0  point velocity                        b3_crash_point_velocity [C]
//   FUN_00125100  crash-entry impulse kick              b3_crash_kick      [C]
//   FUN_00125380  kick axis selection                   b3_crash_kick_axis [C]
//   FUN_00123000  crash-mode solver (body maths)        b3_crash_mode_frame [C]
//   FUN_00123000  crash-mode damping laws (head)        b3_crash_mode_damping [S-disasm]
//
// See docs/RE_NOTES.md section 16 (and 16.1 for the crash-entry launch /
// crashed-path solver correction) for the full write-up.

#ifndef BURNOUT3_CRASH_H
#define BURNOUT3_CRASH_H

#ifdef __cplusplus
extern "C" {
#endif

struct B3RigidBody;      // burnout3_vehicle_sim.h (included below)

// ---------------------------------------------------------------------------
// One world-space collision poly, mirroring the 0x40-byte record consumed by
// FUN_0011AC30 (built per frame into the global scratch set at 0x005A3AA0 by
// FUN_00122D00: track polys via FUN_00109D20, plus other-car hull polys from
// veh+0x1590/+0x2030 flagged '!' and crash-party barriers flagged '&') [C].
// p0/p1/p2 = triangle verts, n = world normal; all vec4 (w carried by the SSE
// paths, normally 0).
// ---------------------------------------------------------------------------
// (Tagged: burnout3_vehicle_sim.h forward-declares it for B3ChassisSoup,
// the veh+0x200 record FUN_0011AEF0 reads.)
typedef struct B3CrashPoly {
    float p0[4], p1[4], p2[4], n[4];
} B3CrashPoly;

// ---------------------------------------------------------------------------
// Contact accumulator -- mirrors the 0xE0-byte stack struct FUN_0011AEF0
// builds at ESP+0x80 and hands to every FUN_0011AC30 call [C]:
//   +0x00 world->body inverse frame (copy of veh+0x70)
//   +0x40 sum of wall-contact centroids (body space)      normal.y <= 0.7
//   +0x50 sum of ground-contact centroids (body space)    normal.y  > 0.7
//   +0x60 sum of ground-contact normals (world)
//   +0x70 componentwise MIN of wall normals (init 0)
//   +0x80 componentwise MAX of wall normals (init 0)
//   +0xB0 bbox max (copy of veh+0x1D0), +0xC0 bbox min (veh+0x1E0)
//   +0xD0 wall count, +0xD4 ground count, +0xDC surface flags u16
//     (lowest nonzero low byte wins)
// ---------------------------------------------------------------------------
typedef struct {
    float inv[4][4];       // world -> body
    float wall_cent[4];
    float gnd_cent[4];
    float gnd_n[4];
    float wall_nmin[4];
    float wall_nmax[4];
    float bbmax[4];        // veh+0x1D0  (proved max by the clip driver's slab
    float bbmin[4];        // veh+0x1E0   bounds; loaded from .bgv+0xE80/+0xE90
                           //             by FUN_00122830 [C-disasm])
    int wall_count;
    int gnd_count;
    unsigned short flags;
} B3CrashContactAcc;

// ---------------------------------------------------------------------------
// The vehicle fields FUN_0011AEF0 and its callees touch, by live offset.
// (Tagged: burnout3_td_rules.h forward-declares it for the wall trigger.)
// ---------------------------------------------------------------------------
typedef struct B3CrashVehicle {
    float frame[4][4];        // [veh+0x204] rows: right / up / at / pos
    float inv[4][4];          // veh+0x70   world->body inverse frame
    float iinv_world[3][4];   // veh+0x40   world inverse inertia rows
    float vel[4];             // veh+0xB0   (vel[3] = +0xBC speed magnitude)
    float dir[4];             // veh+0xC0   unit travel direction (w = +0xCC)
    float omega[4];           // veh+0xD0
    float force_acc[4];       // veh+0xF0
    float imp[4];             // veh+0x110  linear impulse accumulator
    float ang_imp[4];         // veh+0x120  angular impulse accumulator
    float defl[4];            // veh+0x130  deflection (added to pos, cleared)
    float bbmax[4];           // veh+0x1D0
    float bbmin[4];           // veh+0x1E0
    float mass;               // veh+0x1F0

    // outputs of the response
    float contact_pt[4];      // veh+0x160  world contact point
    float contact_n[4];       // veh+0x170  contact normal (wall: flattened
                              //            into the right/at plane)
    unsigned short surface;   // veh+0x190  u16 surface flags of the contact
    float impact;             // veh+0x194  impact magnitude (wall path) /
                              //            raw impulse (ground path); feeds
                              //            the crash probability rolls
                              //            (FUN_0010ED30: rand < (impact-8000)
                              //            /17000) [C-disasm]
    int contact_state;        // veh+0x198  0 none / 1 wall / 2 ground

    // inputs / state
    float surface_grip;       // veh+0x13A8 (class-0 velocity scrub factor)
    float ground_frac;        // veh+0x1404 (<= 0.1 -> impulse applied at the
                              //            contact point WITH torque)
    float drift_dir;          // veh+0x1434 (sign flipped when gear == -1)
    int   drift_state;        // veh+0x1524 (1/2 = drifting: linear-only)
    float authority;          // veh+0x1534 (crash thresholds scale by it)
    unsigned char no_scrub;   // veh+0x153E (non-class-0: skip the 0.99 scrub)
    unsigned char flags1353;  // veh+0x1353 (bit0|bit2 = response disabled;
                              //            bit3 = wall-crash trigger blocked)
    int   gear;               // veh+0x14C8 (-1 = reverse)
    int   racecar_class;      // racecar+0x1920 (2 never wall-crashes)
    int   is_class0;          // racecar class == 0 (players): second poly set
                              //            + surface-grip scrub path
    int   party_mode;         // FUN_00017310 result (crash-party thresholds)

    // second poly set (veh+0x1590, count veh+0x3A50): other-car hull polys.
    const B3CrashPoly* set2;
    int set2_count;

    // crash-decision outputs (the real code calls FUN_0010DCA0 here)
    int crashed;              // wall-crash entry fired
    unsigned char surf_bit15; // racecar+0x15CC := surface flags >> 15
    unsigned char asleep;     // veh+0x20E (FUN_00125100 clears it: any crash
                              //            kick WAKES the body)
} B3CrashVehicle;

// FUN_001B09C0 [C]: Sutherland-Hodgman clip of a closed polygon against the
// slab [lo, hi] on one component (axis 0=x,1=y,2=z). Returns the output count.
int b3_crash_clip_axis(float out[][4], const float in[][4], int n, int axis,
                       float lo, float hi);

// FUN_001B0C00 [C]: clip a triangle against the body bbox, axis order x, z, y;
// every stage must keep >= 3 verts or the result is 0. out needs >= 9 slots.
int b3_crash_box_clip(const float bbmax[4], const float bbmin[4],
                      const float tri[3][4], float out[][4]);

void b3_crash_acc_init(B3CrashContactAcc* acc, const float inv[4][4],
                       const float bbmax[4], const float bbmin[4]);

// FUN_0011AC30 [C] (normal race mode; the crash-party '&' skip is [S]):
// transform the poly into body space, reject wall slivers, clip against the
// bbox, accumulate the centroid + normal into the wall or ground slots.
void b3_crash_poly_contact(const B3CrashPoly* poly, unsigned short flags,
                           B3CrashContactAcc* acc);

// FUN_001066A0 [C]: vp = omega x (pt - frame_pos) + velocity  (w = speed).
void b3_crash_point_velocity(const B3CrashVehicle* v, const float pt[4],
                             float vp[4]);

// FUN_00106720 [C]: contact impulse
//   j   = -(1+e) * dot(n, vp) / (1/m + dot(cross(Iinv*(r x n), r), n))
//   out = n * |j|   (4-wide)
// RETURNS THE SIGNED j -- XMM0 keeps the quotient (0x00106863); the `fabs`
// at 0x00106874 runs through the x87 stack and only scales `out`.  Callers
// that gate on the sign are the retail ones: FUN_0011AEF0 skips its whole
// impact/impulse block when j/mass <= 0 (@0x0011B771), so a chassis already
// separating from a wall is recorded but not kicked.
float b3_crash_impulse(const B3CrashVehicle* v, const float n[4],
                       const float pt[4], const float vp[4],
                       float restitution, float out[4]);

// FUN_00106500 [C]: imp -> +0x110, (pt - pos) x imp -> +0x120.
void b3_crash_apply_impulse_at_point(B3CrashVehicle* v, const float imp[4],
                                     const float pt[4]);

// FUN_001206D0 [C]: route an impulse: drifting (state 1/2) -> linear only;
// already yawing (|omega.y| > 3) in the same direction as the impulse's yaw
// torque -> linear only; otherwise apply at point with torque.
void b3_crash_apply_impulse(B3CrashVehicle* v, const float imp[4],
                            const float pt[4]);

// FUN_0011AEF0 [C]: the full chassis-vs-world response. Returns the wall
// contact count (the real function's return value). Wall path: flattened
// normal, deflection push-out (1.5 x bbox-edge distance), head-on velocity
// scrub, surface-grip scrub, contact impulse (with torque when airborne),
// impact magnitude, wall-crash trigger decision. Ground path: contact record
// + impulse magnitude only. The landing-on-car tail (veh+0x211 == 1) is NOT
// ported ([S], needs the racecar table).
int b3_crash_response(B3CrashVehicle* v, const B3CrashPoly* polys,
                      const unsigned short* flags, int count);

// ---------------------------------------------------------------------------
// THE WALL-CRASH TRIGGER, STANDALONE -- FUN_0011AEF0 @0x0011B4B0..0x0011B9A3
//
// The full response above needs the game's own poly soup to reach its
// trigger.  A harness whose world contact comes from somewhere else (a sphere
// sweep, a capsule, a segment grid) can still take the REAL decision by
// handing the contact it already has to b3_crash_wall_eval: everything from
// the flattened normal onward is the retail code, byte for byte the same
// path the response runs.
//
//   dv     = |j| / mass, j = FUN_00106720 along nh at the contact point, so
//            the POINT velocity -- omega x r included.  A spun car whose tail
//            swings into a barrier has a large dv with almost no chassis
//            velocity along the normal; that case is invisible to a
//            centre-of-mass closing speed.
//   headon = |dot(n_flat, at)|                                     ESP+0x14
//   fire   = normal race: surface_lo != 0x20 && !(flags1353 & 8)
//                         && dv > authority*27.5 && headon > authority*0.707
//            crash party: dv > authority*10.0 && headon > authority*0.303
//            and in both cases racecar class (+0x1920) != 2.
//
// The `authority` scale veh+0x1534 is the whole dynamic range of this test:
// 1.0 for a car whose driver has the wheel, 0.05 for one inside its
// out-of-control window after being slammed (b3_td_crash_authority in
// burnout3_td_rules.h).  At 1.0 a barrier needs a 27.5 m/s head-on delta-v;
// at 0.05 it needs 1.375 m/s and any angle at all.
//
// `pt` and `wall_n` are WORLD (game-space) 4-vectors; wall_n points at the
// car, the same orientation the game's wall normals carry.  Nothing on `v`
// is modified -- the velocity scrubs the real code applies before measuring
// the impulse are replayed onto a local copy.  Returns 0 for a degenerate
// normal (the case the real function bails on).
// ---------------------------------------------------------------------------
typedef struct {
    float nh[4];       // the flattened normal projected into right/at
    float headon;      // |dot(n_flat, at)|
    float dv;          // |j| / mass
    float impact;      // the veh+0x194 magnitude for this contact
    float dv_thr;      // authority * 27.5   (party: * 10.0)
    float headon_thr;  // authority * 0.707  (party: * 0.303)
    int   fire;        // the retail crash decision
} B3CrashWallEval;

int b3_crash_wall_eval(const B3CrashVehicle* v, const float pt[4],
                       const float wall_n[4], B3CrashWallEval* out);

// The last APPLIED (apply != 0) wall evaluation the in-substep resolve made,
// i.e. the numbers FUN_0011AEF0's gate @0x0011B909..0x0011B9A3 actually
// tested.  Reporting only -- nothing in the port reads it.  Returns 0 if no
// applied wall contact has been resolved yet.  The td_rules arm
// (b3_td_wall_contact, apply == 0) never publishes here, which is the whole
// point: on a crash_fired frame the td_rules record is empty.
int b3_crash_last_wall_eval(B3CrashWallEval* out);

// B3_WALLGATE=1 -- one printf per APPLIED wall resolve whose impact block ran
// (dv > 0), carrying the gate's own inputs (authority, dv/threshold,
// headon/threshold, the surface word and veh+0x1353).  Diagnostic only.
int b3_wallgate_trace(void);

// ---------------------------------------------------------------------------
// THE CRASH-ENTRY KICK -- FUN_00125100 [C] and its axis wrapper FUN_00125380
// [C].  This is what LAUNCHES and TUMBLES a wrecked car; without it a wreck
// only ever gets FUN_0011AEF0's deliberately HORIZONTAL wall impulse (the
// normal is flattened to y == 0 at 0x0011B48x and then has its up-row
// component removed at 0x0011B89x), so it can never leave the ground.
//
// Every crash-entry site in the image fires the same pair:
//     b3_crash_kick(v, corner_flags,   mag, frame.up)   -> PURE TORQUE
//     b3_crash_kick(v, B3_KICK_LINEAR, mag, frame.up)   -> PURE LINEAR launch
//
// FUN_00125100(flags, mag, axis), ESI = vehicle:
//   P  = frame.pos
//        + frame.right * (-bbmax.x)      flags & 0x08  (hit on the LEFT)
//        + frame.right * (-bbmin.x)      flags & 0x04  (hit on the RIGHT)
//        + frame.at    *   bbmax.z       flags & 0x21  (FRONT)  mag *= 1.2
//        + frame.at    *   bbmin.z       flags & 0x42  (REAR)   mag *= 1.2
//        + frame.up    *   bbmax.y*0.8   when up.y < 0 (inverted: the roof)
//   J  = axis * 10.0 * mass * mag            [0x0041A504 = 10.0, 0x0041A500
//                                             = 1.2, 0x003A5600 = 0.8]
//   (flags & 0x90) == 0 -> ang_imp += (P - pos) x J   and the linear part is
//                          explicitly SAVED AND RESTORED (0x001252D0 /
//                          0x001252EC): the at-point kick is TORQUE ONLY.
//   else                -> imp += J                    (linear only)
//   Always: veh+0x20E := 0 (the kick wakes a sleeping body).
//
// Magnitudes read straight out of the retail instruction stream [C-disasm]:
//   0.65 + 0.90  0x0011C421 / 0x0011C439  FUN_0011BE50 rollover crash
//   0.40         0x00024F94               crash-record handler FUN_00024F10
//   0.30         0x00025C8D               its sibling at 0x00025C60
//   0.60         0x00118241               FUN_00118410 aftertouch corners
//   0.25         0x00197376               FUN_00197260 grind crash
//   0.50 / 1.60  0x00026AEE / 0x00026B09  world-up and forward kicks
// The wall/cause-record entries take theirs from the crash-director object
// 0x0064ACE8 +0x57C..+0x594, which is BSS with no static writer -> [?].
// ---------------------------------------------------------------------------
#define B3_KICK_FRONT   0x01
#define B3_KICK_REAR    0x02
#define B3_KICK_RIGHT   0x04
#define B3_KICK_LEFT    0x08
#define B3_KICK_LINEAR  0x10   // (flags & 0x90) != 0 -> linear, no torque

// Retail literals, named for the sites above.
#define B3_KICK_MAG_LAUNCH     0.65f   // 0x0011C421 pure-linear up launch
#define B3_KICK_MAG_SPIN       0.90f   // 0x0011C439 corner torque
#define B3_KICK_MAG_TAKEDOWN   0.40f   // 0x00024F94 corner torque
#define B3_KICK_MAG_AFTERTOUCH 0.60f   // 0x00118241 corner torque

// ---------------------------------------------------------------------------
// THE PER-KIND CRASH-ENTRY TABLE -- recovered from the disassembly of every
// crash-entry site [C].  Which kick (if any) fires when a car enters crash
// mode depends on HOW it crashed, and the harness's old `b3_wreck_begin`
// fired the same 0.40 spin + 0.65 launch on every crash -- a mix of the
// crash-record torque and the ROLLOVER launch that belong to different sites.
//
//   B3_WRECK_ENTRY_WALL   chassis-vs-world (wall or object), NO collision
//                         pair record.  Retail DOES issue a kick pair here --
//                         FUN_0010DD20 @0x0010E3F7..0x0010E425, flags 0x02
//                         (REAR corner torque) then flags 0x10 (pure linear
//                         launch), both on the UP row, both at the SAME
//                         magnitude `director[+0x590]`.  That magnitude is
//                         0.0: 0x0064ACE8+0x590 = 0x0064B278 lands in the
//                         .bss half of the 0x003B2360 PT_LOAD (filesz
//                         0x6844C, memsz 0x3B95DC), so it is zero at load,
//                         and it has NO WRITER -- an absolute-reference scan
//                         finds none, and a modrm census of every executable
//                         segment finds ZERO stores at [reg+0x57C], [+0x580],
//                         [+0x584] or [+0x590] anywhere in the image (the
//                         only +0x594 hits are integer cmp/and/inc on an
//                         unrelated struct at 0x0031BE45..0x0031FF44).  The
//                         same is true of every other cause-code arm of that
//                         jump table (@0x0010E2AB/+0x584, @0x0010E2C4/+0x57C,
//                         @0x0010E33F/+0x580).  A zero-magnitude
//                         b3_crash_kick is a no-op, so corner=0, spin=0,
//                         launch=0 is behaviourally identical and the tumble
//                         comes entirely from FUN_0011AEF0's own contact
//                         impulse.  [C]
//   B3_WRECK_ENTRY_CAR    car wrecked MID-RACE by another body (car-vs-car,
//                         car-vs-wreck, racer-vs-traffic).  A collision pair
//                         record exists, so the crash-record sweep
//                         FUN_00024F10 fires: fixed 0x0A (rear-left) corner
//                         torque @ 0.40 on the up row, flags 0x0A, i.e.
//                         TORQUE-ONLY (no 0x10 -> no linear launch).  The 0.30
//                         re-hit (0x00025C8D) is the same corner, lower mag.
//                         corner=0x0A, spin=0.40, launch=0.
//   B3_WRECK_ENTRY_ROLLOVER  the car is actually INVERTED (up.y < 0).  The
//                         FUN_0011BE50 tail fires the pure-linear up launch
//                         0x10 @ 0.65 (0x0011C421) plus a corner spin 0x08/
//                         0x04 @ 0.90 (0x0011C439).  corner=0x08, spin=0.90,
//                         launch=0.65.
// ---------------------------------------------------------------------------
typedef enum {
    B3_WRECK_ENTRY_WALL     = 0,   // impulse-only, no entry kick
    B3_WRECK_ENTRY_CAR      = 1,   // 0.40 rear-left torque, no launch
    B3_WRECK_ENTRY_ROLLOVER = 2    // 0.90 spin + 0.65 launch
} B3WreckEntryKind;

void b3_crash_kick(B3CrashVehicle* v, unsigned flags, float mag,
                   const float axis[4]);

// FUN_00125380 [C]: axis = frame.right when flags & 0x60, frame.at when
// flags & 0x80, else frame.up; then b3_crash_kick.
void b3_crash_kick_axis(B3CrashVehicle* v, unsigned flags, float mag);

// ---------------------------------------------------------------------------
// The crashed-path solver FUN_00123000 [C for the body maths].  Reached only
// through FUN_0011BE50's crashed branch (byte veh+0x210 != 0, 0x0011BE83..
// 0x0011BF09: crash clock, gear -> neutral, engine idle FUN_00121560,
// FUN_00118410 input shaper, FUN_00123000, FUN_0011C720).  Per frame:
//   * quadratic drag  force_acc += dir * (speed^2 * -1.0)   [0x001230A6]
//   * per substep (1, or 2 when DAT_005A3759 selects the half-dt modes):
//       - rollover crawl: veh+0x211 == 0 AND every wheel grounded AND
//         frame speed < 1  ->  L.y *= 0.95, vel *= 0.95        [0x001232C3]
//       - airborne: veh+0x1168 (NO wheel has a contact surface)
//                            ->  L *= (0.99, 0.99, 0.97)       [0x001235FF]
//       - settle: frame speed < 1 AND (FUN_00125CF0 || asleep)
//                            ->  L *= 0.9, vel *= 0.95, +0x1354++
//                                                              [0x00123372]
//         else (speed >= 1) ->  asleep = 0, settle counter = 0 [0x001236E0]
//       - FUN_00123FD0 suspension force pass (skipped while asleep)
//       - FUN_00109560 integration -- and because veh+0x210 != 0 it takes
//         its 0x00109622 branch, applying gravity at pos + up*com_height,
//         i.e. WITH A TORQUE.  That gravity torque is the term that keeps a
//         tumbling wreck rotating; the racing path never sees it.
// NOTE the frame speed used by all three gates is captured ONCE before the
// substep loop (stored at [ESP+0x1C], 0x001230FA) -- not refreshed.
// Sleep needs settle counter > 5 [byte 0x003EBF88], speed < 0.5, |omega|^2
// < DAT_005A80B8 [BSS, ?], clock > ctx+0xFFC and an upright/contact test.
// ---------------------------------------------------------------------------
typedef struct {
    int all_grounded;         // every non-detached wheel reports a surface
    int all_airborne;         // veh+0x1168
    int state211;             // veh+0x211
    int settle_test;          // FUN_00125CF0(veh, 1)
    unsigned char asleep;     // veh+0x20E
    unsigned char settle;     // veh+0x1354
    // ---- sleep gate inputs (FUN_00123000 @0x00123412..0x001235ED) [C] ----
    // The block runs on the settle branch only.  Its "squarely on a surface"
    // term has two arms, chosen by veh+0x212 at 0x00123414:
    //   veh+0x212 != 0   the chassis contact axis veh+0x170 must be within
    //                    0.99 [0x003B1758] of one of the three body rows
    //                    (0x0012341A..0x00123516)
    //   veh+0x212 == 0   every wheel that reports a contact (wheel+0xB3)
    //                    must have its contact normal (wheel+0x20) within
    //                    0.98 [0x003B1DA0] of the body UP row
    //                    (0x00123662..0x001236DB) -- with no wheels in
    //                    contact at all the term passes
    int state212;             // veh+0x212: chassis contact this frame
    float contact_normal[4];  // veh+0x170: the recorded contact axis
    int wheel_count;          // veh+0x1169 (the veh+0x212 == 0 arm)
    const float (*wheel_normal)[4];      // wheel+0x20, stride 0xC0
    const unsigned char* wheel_contact;  // wheel+0xB3
    int clock_after_stamp;    // clock > ctx1+0xFFC (the FUN_00125100 stamp)
    int slept;                // OUT: the sleep gate fired this frame
} B3CrashModeState;

// FUN_00125CF0 [C-disasm] -- the SETTLE PREDICATE.  Returns 1 when the body
// is resting on a surface the game considers solid:
//   * chassis: veh+0x212 (a contact this frame) AND the chassis contact
//     surface byte veh+0x190 <= 0x20                      [0x00125CF0..0D0C]
//   * wheels (only when `check_wheels`, the arg FUN_00123000 passes as 1):
//     any wheel whose contact byte veh+0x8D3+i*0xC0 is set, whose damage
//     state ctx1+0x4AC+i != 3, and whose surface byte veh+0x8D0+i*0xC0
//     is <= 0x20                                          [0x00125D16..0D60]
// The arrays are indexed 0..wheel_count-1 and may be NULL when count is 0.
int b3_crash_settle_test(int chassis_contact, int chassis_surface,
                         int check_wheels, int wheel_count,
                         const unsigned char* wheel_contact,
                         const unsigned char* wheel_surface,
                         const unsigned char* wheel_damage);

// The sleep limits, read out of the image [C]:
//   speed  < 0.5                                    [0x003B1684]
//   |w|^2  < DAT_005A80B8 = 0.25                    (BSS, written once by the
//        static initialiser at 0x002BA7A0 from [0x003B1730] = 0.25; that
//        thunk is entry 0x3BDE84 of the CRT init table -- this closes the
//        "[?] BSS, no static writer" note in RE_NOTES 16.1)
//   settle counter > 5                              [byte 0x003EBF88]
//   contact-axis test threshold 0.99                [0x003B1758]
//   the wheel arm's threshold          0.98           [0x003B1DA0]
#define B3_SLEEP_SPEED_LIMIT   0.5f
#define B3_SLEEP_OMEGA2_LIMIT  0.25f
#define B3_SLEEP_SETTLE_COUNT  5
#define B3_SLEEP_AXIS_LIMIT    0.99f
#define B3_SLEEP_WHEEL_AXIS_LIMIT 0.98f

// The crashed-path SCRAPE, FUN_00123FD0 @0x001248EA..0x001249E9 [C-disasm]:
// while veh+0x210 (crash mode) is set, veh+0x116B is clear and the body is
// moving (speed > 0.1 [0x003A69C4]), each contact point gets a friction
// force opposing the velocity OF THAT POINT:
//     v    = point_velocity(pt)            FUN_001066A0
//     vhat = normalize(v)                  FUN_00011640
//     f    = (|dot(frame.right, vhat)| + 1.0) * mass * n_points * -0.75
//                                          [0x003B168C, 0x003B17EC = -0.75]
//     F    = vhat * f,  halved unless veh+0x215 is 1/2/3   [0x003B1684]
//     force_acc += F;  torque_acc += (pt - pos) x F        FUN_001064B0
// This is the term that bleeds a sliding wreck to a stop.  RE_NOTES 16.2.
void b3_crash_scrape(struct B3RigidBody* rb, const float pt[4], float mass,
                     int n_points, int full_strength);

// One crashed-path frame of body motion: the drag term, the three damping
// laws in FUN_00123000's order, and `substeps` x b3_rigid_body_integrate in
// crash mode.  The suspension force pass is the vehicle module's and is not
// run here (while airborne it contributes nothing, which is the window the
// trajectory oracle asserts over).
void b3_crash_mode_frame(struct B3RigidBody* rb, B3CrashModeState* s,
                         float mass, float com_height, int substeps,
                         float dt);

// ---------------------------------------------------------------------------
// Crash-mode damping laws from FUN_00123000's dispatcher head [S-disasm --
// constants read from the instructions; the surrounding dispatcher is not
// execution-verified]:
//   all wheels airborne:            L *= (0.99, 0.99, 0.97)
//   rollover crawl (speed < 1):     L.y *= 0.95, velocity *= 0.95
//   settle pass (crashed, slow):    L *= 0.9,  velocity *= 0.95
// The same dispatcher proves crash motion IS the normal pipeline: it runs the
// suspension + FUN_00109560 rigid-body integration every step, crashed or not.
// ---------------------------------------------------------------------------
void b3_crash_mode_damping(float angmom[4], float vel[4], int all_airborne,
                           int settling);

// ---------------------------------------------------------------------------
// Harness wreck state (GLUE assembly around the ported response + the
// verified b3_rigid_body_integrate). One per grid slot.
// ---------------------------------------------------------------------------
#include "burnout3_vehicle_sim.h"        // B3RigidBody

// COORDINATES.  The stored frame/vel/pos are in HARNESS space (the game
// world with Z negated, RE_NOTES 12) because the harness reads them
// straight back for the draw and the position write-back.  That basis is
// LEFT-handed, and the pipeline's cross-product algebra is chirality-bound:
// b3_mat_orthonormalize rebuilds a row from a cross product and flips a
// mirrored basis upside down on the first step (the racing path avoids this
// by keeping its rigid body in GAME space, src/burnout3_full.c
// full_sim_reset).  So b3_wreck_* mirrors in at entry, runs every ported law
// in GAME space, and mirrors back out: vectors z-negate, the PSEUDOvectors
// omega and angular momentum map (x,y,z) -> (-x,-y,z).  Both maps are exact
// sign flips, so the round trip is bit-exact.  RE_NOTES 16.2.
typedef struct {
    int   active;
    float frame[4][4];     // rows right/up/at/pos, harness GL space
    float vel[4];          // m/s (w = speed)
    float angmom[4];
    float omega[4];
    float iinv_body[3][4]; // GLUE: box inertia from the car bbox
    float mass;
    float bbmax[4], bbmin[4];
    float settle;          // veh+0x1354 semantics (settle counter)
    int   asleep;          // veh+0x20E semantics
    float yaw, pitch, roll; // extracted for the harness draw
    float com_height;      // veh+0x1F4 = (bbmax.y - bbmin.y) * 0.1 (the
                           // gravity-torque arm the crash-mode integrator
                           // uses; FUN_0011A8F0)
    int   airborne;        // last frame's veh+0x1168 classification
    float air_time;        // seconds since the wreck last touched ground
    // -- at-rest inputs/outputs (RE_NOTES 16.2) ---------------------------
    int   ground_surface;  // veh+0x190 stand-in: the surface type under the
                           // wreck.  0 (the default) is a solid road, so a
                           // caller that does not set it still settles.
    int   state215;        // veh+0x215: 1/2/3 -> the scrape runs at full
                           // strength, otherwise it is halved [0x001249AF]
    float rest_clock;      // seconds since b3_wreck_begin (the clock the
                           // sleep gate compares against the kick stamp)
    float after_credit;    // simulated seconds banked for the aftertouch
                           // cadence (see b3_wreck_aftertouch)
    // -- per-frame CONTACT REPORT, consumed by the panel machine ----------
    // Retail feeds the per-panel damage accumulators from the suspension
    // pass's contact impulses (FUN_00123FD0 @0x001246FF -> FUN_0012C670; see
    // burnout3_panels.h for the whole chain).  The harness wreck has no wheel
    // rays, so it publishes its OWN contact set here and burnout3_panels.c
    // puts that through the same recovered class scale, 5.0 gate and
    // 0.002/0.1 add law.  Rewritten by every b3_wreck_update;
    // b3_wreck_begin_kick seeds it with the ENTRY impact so the crash's first
    // frame already damages the struck side.
    int   hit_count;       // contact points resolved this frame
    float hit_normal[4];   // world contact normal, harness space (unit)
    float hit_impulse;     // |linear contact impulse| this frame, N.s
    int   hit_collision;   // 1 = a COLLISION event (retail's FUN_00111CD0 ->
                           //     FUN_0012FA40 arm: full class scale, gate
                           //     5.0), 0 = a ground/suspension contact
                           //     (FUN_00123FD0's arm: raw gate 2000, then
                           //     halved before the class scale)
    // -- RETAIL AFTERTOUCH (FUN_00118410, crash-cinema wave) --------------
    float after_real_credit;  // REAL seconds banked for the retail tick
                              // cadence (see b3_wreck_aftertouch_steer)
    int   aftertouch_used;    // veh+0x4AC5: set the first frame the player
                              // actually steered the wreck.  This is the
                              // qualifier retail's score module reads to
                              // turn a wreck-caused takedown into an
                              // AFTERTOUCH TAKEDOWN [0x00118CD3 sets it,
                              // 0x00119C87 clears it on crash exit].
    float bank;               // veh+0x3A78: the accumulated visual bank the
                              // aftertouch rotation feeds [0x00118D30..]
    // -- PH-06/PH-21: the wreck's OWN world-contact accumulators ----------
    // veh+0x110 / +0x120 / +0x130, in HARNESS space.  b3_wreck_world_contact
    // writes them and the next b3_wreck_update's integrator consumes and
    // clears them, which is the retail cadence: the wreck's vtable slot
    // +0x10 (FUN_0011BE40 -> FUN_00122D00 -> FUN_00109EA0 @0x00122F81) runs
    // in the collision manager's world pass, BEFORE slot +0's
    // FUN_0011BE50 -> FUN_00123000 integrates.
    float pend_imp[4];
    float pend_imp_torque[4];
    float pend_defl[4];
} B3WreckState;

// Aftertouch cadence.  FUN_00118410 fires at most ONE corner kick per
// VEHICLE UPDATE, and retail's vehicle update is a fixed-rate tick, so the
// retail rate is one kick per tick of SIMULATED time.  The harness calls
// b3_wreck_aftertouch once per RENDERED frame, and while the crash slow-mo
// runs (the takedown FX divides the sim dt by 5) that is five kicks per
// retail tick -- five times the torque per simulated second.  Each kick is
// an impulse (dt-independent) while gravity integrates per sim-dt, so the
// asymmetry alone will hold a wreck in the air indefinitely.  The module
// therefore banks simulated time (b3_wreck_update) and spends it at the
// retail cadence.
// (overridable at build time only so the acceptance test can ablate it --
//  tools/validate_crash_traj.py section 8)
#ifndef B3_AFTERTOUCH_PERIOD
#define B3_AFTERTOUCH_PERIOD (1.0f / 60.0f)
#endif

// The game's compiled-in per-class body INVERSE inertia diagonal
// (FUN_001203A0 + .data [C], RE_NOTES section 14).  A wreck is the same
// rigid body as the driving car, so it uses the same tensor.
#define B3_WRECK_IINV_DEFAULT_X  0.0008f
#define B3_WRECK_IINV_DEFAULT_Y  0.0011f
#define B3_WRECK_IINV_DEFAULT_Z  0.0013f
// HEVY* trucks: 0.0004 / 0.0006 / 0.0007;  HEVYCAR5-6: 0.00075/0.0008/0.0011
void b3_wreck_set_inertia(B3WreckState* w, const float diag[3]);

// ---------------------------------------------------------------------------
// THE PER-SLOT CRASH LATCH -- crash_record+0x130 (FUN_0010DD20, the crash
// director, LAB_0010e431..0x0010e4c2 [C-disasm + .rodata reader]).
//
// The director stamps a 0x3C crash record per vehicle slot.  Its +0x130
// duration is the PER-SLOT LATCH: while it is > 0, a new crash record for
// that slot will not open (the entry test @0x0010dddf: `MOVSS [rec+0x130];
// COMISS 0.0; JA ret`).  The harness maps it onto `immune_until` (the
// "cannot be re-crashed yet" cooldown), as the TODO/ledger already decided.
//
//   class = racecar+0x1920, presented = FUN_00017310(0x4A71A0) != 0 --
//   the GLOBAL crash-presentation object is in a live mode (3-6), i.e. a
//   crash-cinema is ALREADY running when this crash lands (a chain crash
//   mid-presentation).  A FRESH crash (no presentation yet) is the else arm:
//   the director only STARTS the presentation afterwards, at
//   @0x0010e4f4 (record vtable +0xC), which is why the fresh arm is the
//   common one.  Constants read from .rodata (tools/read_crash_consts.py,
//   mapping validated against the known DAT_003B1694 = 5.0 pin):
//
//     +0x003b1708 = 7.0    class 2 (HEVY/truck), fresh
//     +0x003b16b4 = 15.0   class 2 presented ; also class!=2 presented !17390
//     +0x003b1698 = 3.0    class != 2, fresh
//     +0x003b1c5c = 17.0   class != 2, presented, FUN_00017390(0x4A71A0) != 0
//
//   rec[+0x13c] = 0.25 [C 0x003b1730] ; rec[+0x140..+0x144] = 0 ;
//   rec[+0x148] = 0.
//
//   CRASH-AUDIT correction: rec[+0x134] is NOT "2.0 both arms".  The shared
//   tail @0x0010e4ca stores whatever XMM0 holds, and only the CLASS-2 arm
//   loads 2.0 into it (@0x0010e470 `MOVSS XMM0,[0x003b1688]` then
//   `JMP 0x0010e4c2`).  The class != 2 arm's last XMM0 write is the
//   `XORPS XMM0,XMM0` @0x0010e4ba, so it stores 0.0.  Nothing in the
//   harness reads +0x134; the note is corrected, not ported.
//
//   The whole table (the four immediates, both store sites, the +0x130 read
//   the head gates on, and `rec = mgr + slot*0x3C`) is asserted against the
//   image bytes by validate_crash_traj section 9.
//
// The in-flight worklog's "duration table" (16.0/7.0 trucks, 5.0/4.0/6.0
// other) was a misread of these immediates; this table is the correction.
// Note the RECOVERY RELEASE is a DIFFERENT field -- racecar+0x240C =
// dilated_clock + 5.0 (FUN_00198E60 @0x00198f65, always 5.0) -- which the
// harness already mirrors 1:1 via `crashed_until = g_race_time + 5.0f`
// (g_race_time runs on the dilated period/divisor clock).  This latch does
// NOT gate the release.
//
// The 17.0 arm's gate FUN_00017390(0x4A71A0) is a runtime-only object-state
// query ([?] -- the object at 0x004A71A0 is not the crash presentation, so
// the exact condition is unmapped); the 15.0 fresh-presented value is used
// for it [S].  All harness racers are class 0, so only the class!=2 arm
// (3.0 / 15.0) is reachable in a race; the truck arm is ported for the
// record.
// ---------------------------------------------------------------------------
#define B3_CRASH_LATCH_TRUCK_FRESH   7.0f
#define B3_CRASH_LATCH_TRUCK_PRESENT 15.0f
#define B3_CRASH_LATCH_CAR_FRESH     3.0f
#define B3_CRASH_LATCH_CAR_PRESENT   15.0f
// rec[+0x13c] second timer, stamped alongside the latch [C 0x003b1730].
#define B3_CRASH_LATCH_SUB_TIMER     0.25f

// The per-slot crash latch duration (game seconds, dilated-clock units).
// `is_truck` = racecar+0x1920 == 2; `presented` = a crash-cinema is already
// running when the crash lands (the harness's crash-presentation active
// state).  See the table above for the retail provenance.
float b3_crash_latch_duration(int is_truck, int presented);

// Begin a wreck: seed the rigid state from the car pose/velocity, apply the
// real impact response at the given world contact (normal pointing away from
// the obstacle), and then fire the ROLLOVER crash-entry kick pair -- the 0.90
// corner spin plus the 0.65 pure-linear up-axis launch (the FUN_0011BE50 tail
// literals).  This is the INVERSION entry, kept for callers that model a car
// tipping over.  For the per-cause entries (wall = impulse-only, car-wrecked-
// mid-race = 0.40 torque-only) use b3_wreck_begin_entry, which is what the
// race consequence sites drive.  rel_vel = victim velocity relative to the
// obstacle.
void b3_wreck_begin(B3WreckState* w, const float pos[3], float heading,
                    const float vel[3], float mass,
                    const float bbmin[3], const float bbmax[3],
                    const float contact_pt[3], const float contact_n[3],
                    const float rel_vel[3]);

// Same as b3_wreck_begin, but the crash-entry kick is chosen by KIND (the
// per-kind table above) instead of the rollover literals: WALL fires no entry
// kick (the contact impulse alone), CAR fires the 0.40 rear-left torque only,
// ROLLOVER fires the 0.90 spin + 0.65 launch.  This is the 1:1 fix for the
// old wrapper that launched and spun every crash equally.
void b3_wreck_begin_entry(B3WreckState* w, B3WreckEntryKind kind,
                          const float pos[3], float heading,
                          const float vel[3], float mass,
                          const float bbmin[3], const float bbmax[3],
                          const float contact_pt[3], const float contact_n[3],
                          const float rel_vel[3]);

// Same, with the crash-entry kick spelled out: `corner` is a B3_KICK_*
// corner mask (front/rear + left/right), mag_spin the at-point torque
// magnitude and mag_launch the pure-linear up-axis launch magnitude.  Pass
// corner = 0 / mag = 0 to suppress either half.
void b3_wreck_begin_kick(B3WreckState* w, const float pos[3], float heading,
                         const float vel[3], float mass,
                         const float bbmin[3], const float bbmax[3],
                         const float contact_pt[3], const float contact_n[3],
                         const float rel_vel[3],
                         unsigned corner, float mag_spin, float mag_launch);

// Aftertouch (FUN_00118410 @0x00118176..0x0011824F [C-disasm]): while the
// wreck is down, a held pad direction fires one corner torque kick per frame
// at magnitude 0.6 along the car's own up row.  dir_x/dir_z in [-1, 1] pick
// the corner exactly like the retail flag sets 9 / 5 / 0xA / 6.
void b3_wreck_aftertouch(B3WreckState* w, float dir_x, float dir_z);

// ---------------------------------------------------------------------------
// THE REAL AFTERTOUCH -- FUN_00118410, the CRASHED-path input shaper
// (crash-cinema wave, 2026-08-13).  [C-disasm + Ghidra decompile]
//
// The block above (b3_wreck_aftertouch) is FUN_00117F90's -- the RACING
// input stage -- and its gate veh+0x4AC2 is provably never written non-zero,
// so it is dead.  Retail's live aftertouch is a DIFFERENT function on a
// DIFFERENT path: FUN_0011BE50's crashed branch calls FUN_00118410
// (0x00118410..0x00118E06), and that function both PRODUCES the aftertouch
// axes and CONSUMES them.  Nothing about it is a torque kick.
//
//   PRODUCE  0x001185C6  veh+0x1408 = FUN_00020E70(pad)
//                        = clamp(dpad_right - dpad_left + lstick_x, -1, 1)
//            0x001185D3  veh+0x140C = FUN_00020F50(pad)
//                        = clamp(dpad_up    - dpad_down + lstick_y, -1, 1)
//            (the pad floats are +0x50/+0x4C/+0xA4 and +0x44/+0x48/+0xA8;
//             |axis| > 0.15 also stamps the pad's activity clock +0x184)
//            These two are exactly what the HUD's aftertouch ARROW CURSOR
//            reads (FUN_0004FCA0 @0x0004FCF0/0x0004FD18): +0x140C > 0 lights
//            the UP wedge, +0x1408 > 0 the RIGHT wedge.
//
//   IMPACT TIME  0x0011889A  in a RACE the crashed stage sets the global time
//            divisor [0x0060EA24] = 5 while bit 4 of veh+0x13FC is set, i.e.
//            while pad+0x84 -- the BOOST button -- is non-zero, and restores
//            it to 1 on release (0x001188D6).  Single player only
//            ([0x0073A1C0] == 1).  In the crash-junction family the divisor
//            is 3/4 instead.  veh+0x4AC7 latches "engaged".
//
//   CONSUME  0x001189A3..0x00118CD3, gated on
//              engaged (the same held-boost flag, [esp+0x1D]) || veh+0x4AC3
//              && (veh+0x1530 < 5.0 || crashbreaker armed)   [0x001189AB]
//              && veh+0xBC > 1.0        (speed, the vel 4-vector's w lane)
//              && |h| + |v| > 0.5       [0x00118A0D, 0x003B1684]
//            veh+0x1530 is the CRASH CLOCK (FUN_0011BE50 @0x0011BE98).
//
//            The direction is SCREEN-RELATIVE.  veh+0x1410 holds the camera
//            orientation as a packed quaternion (FUN_00117240 @0x0011867F);
//            FUN_00117520 unpacks it and FUN_00013D10 turns it into a basis.
//              A = row0 (camera right), B = row2 (camera at)
//              A.y = B.y = 0, both renormalised          [0x00118A5F/0x118A71]
//              dir = normalise(A * (-h) + B * (v))       [0x00118A79..0x118ACE]
//
//            The crashbreaker's linear nudge, only while veh+0x3A74 > 0 and
//            the breaker is armed:
//              vel += dir * (0.25 crash mode / 0.15 race)  [0x00118AF0/0x118B08]
//
//            Then the yaw step -- the whole of aftertouch:
//              c    = cross((0,1,0), dir)                 [FUN_000328F0]
//              d0   = dot(veldir veh+0xC0, c)             [FUN_00013C60]
//              d1   = dot(veldir veh+0xC0, dir)
//              ang  = acos(d1) in DEGREES                 [FUN_000FEFD0 =
//                     (pi/2 - asin(x)) * 57.29578]
//              if (d0 > 0) ang = -ang
//              if (|ang| > 8.0) {                         [0x003B16B0]
//                  lim = (0.75 crash mode / 0.4 race) / (crash_clock + 1.0)
//                  ang = clamp(ang, -lim, +lim)
//                  vel = vel * axis_angle((0,1,0), ang)   [FUN_00011900 ->
//                                                          FUN_00031330]
//                  veh+0x4AC5 = 1        <-- THE AFTERTOUCH TAKEDOWN FLAG
//              }
//              if (veh+0x1540) veh+0x3A78 += clamp(ang * -0.5, -1.2, 1.2)
//                              (the visual body bank, capped at |17.0|)
//
// So aftertouch steers the wreck's VELOCITY VECTOR, one clamped yaw step per
// vehicle tick, with an authority that decays as 1/(crash_clock + 1).  It
// "works" in retail because holding boost also divides the sim clock by 5:
// the tick rate is unchanged, so you get five times the yaw per SIMULATED
// second.  The harness reproduces that by banking REAL time here (the
// harness's dt is already dilated) -- see B3_AFTERTOUCH_PERIOD.
// ---------------------------------------------------------------------------
#define B3_AT_DEADZONE        0.5f   /* |h|+|v| gate        @0x003B1684 */
#define B3_AT_MIN_SPEED       1.0f   /* veh+0xBC gate       @0x001189C8 */
#define B3_AT_WINDOW_S        5.0f   /* veh+0x1530 gate     @0x003B1694 */
#define B3_AT_ANGLE_GATE_DEG  8.0f   /* |ang| gate          @0x003B16B0 */
#define B3_AT_RATE_RACE       0.4f   /* deg/tick numerator  @0x003B16E8 */
#define B3_AT_RATE_CRASH     0.75f   /* deg/tick numerator  @0x003A55F8 */
#define B3_AT_NUDGE_RACE     0.15f   /* m/s per tick        @0x00384A80 */
#define B3_AT_NUDGE_CRASH    0.25f   /* m/s per tick        @0x003B1730 */
#define B3_AT_BANK_SCALE     -0.5f   /* veh+0x3A78 step     @0x003B16A4 */
#define B3_AT_BANK_STEP_MAX   1.2f   /*                     @0x003B1768 */
#define B3_AT_BANK_MAX       17.0f   /*                     @0x003B1C5C */

typedef struct B3WreckAftertouchIn {
    float h;              /* veh+0x1408, +1 = RIGHT wedge on the HUD arrow */
    float v;              /* veh+0x140C, +1 = UP    wedge                  */
    int   engaged;        /* Impact Time engaged: the boost button held    */
    int   crash_mode;     /* 1 = crash-junction family (FUN_00017310)      */
    int   breaker_armed;  /* crashbreaker fired < 0.9 s ago (veh+0x3A74)   */
    float crash_clock;    /* veh+0x1530, seconds since the crash began     */
    float cam_right[3];   /* the camera basis rows the retail code unpacks */
    float cam_fwd[3];     /* out of veh+0x1410; y is flattened here        */
    int   want_bank;      /* veh+0x1540: run the visual bank accumulator   */
} B3WreckAftertouchIn;

/* One retail aftertouch tick.  `real_dt` is UNDILATED seconds (the harness's
 * frame time before the slow-mo divisor), because retail's vehicle tick rate
 * does not change under Impact Time.  Returns 1 on the frames that actually
 * rotated the velocity -- the frames that set veh+0x4AC5. */
int b3_wreck_aftertouch_steer(B3WreckState* w, const B3WreckAftertouchIn* in,
                              float real_dt);

/* Clear the aftertouch qualifier -- retail's FUN_00119C00 @0x00119C87, run
 * when the car leaves crash mode. */
void b3_wreck_aftertouch_reset(B3WreckState* w);

// Publish a hit into the wreck's contact report (see the fields above).  The
// STRONGEST report between consumptions wins, so a hard entry hit is not lost
// to the gentle ground contacts of the same frame.  `n` points away from the
// obstacle and into the car; `raw_impulse` is the contact's momentum change
// in N.s; `collision` picks retail's FUN_0012FA40 arm over FUN_00123FD0's.
void b3_wreck_report_hit(B3WreckState* w, const float n[3],
                         float raw_impulse, int collision);

// One wreck frame: gravity (the verified -20), ported ground/wall response,
// crash-mode damping, rigid-body integration (the verified FUN_00109560
// port). ground_y = track surface height under the wreck.
void b3_wreck_update(B3WreckState* w, float ground_y, float dt);

// ---------------------------------------------------------------------------
// PH-06 / PH-21 -- THE WRECK'S WORLD PASS, the way retail runs it.
//
// A crashed racecar is an ordinary rigid body in the collision manager: its
// class vtable (0x003B1160 for the player array) has TWO per-frame slots and
// slot +0x10 is the world contact, `FUN_0011BE40` @0x0011BE4A ->
// **`FUN_00122D00`** @0x00122D00..0x00122FF3, gated `veh+0x210 != 0` and
// `(veh+0x1353 & 5) == 0`:
//     veh+0x200 = 0x005A3AA0        the staging soup list
//     FUN_00109D20  @0x00122D4A     gather
//     ... the wreck appends its own volumes (veh+0x11D0 stride 0x40,
//         surface 0x26; and with veh+0x215 == 1 the veh+0x3A50 records)
//     FUN_00109EA0  @0x00122F81     <-- THE SHARED BODY-vs-WORLD RESOLVE
//     FUN_00126D40  @0x00122FB0     per-panel
//     FUN_001239C0  @0x00122FD2     the suspension pre-pass
// -- the SAME `FUN_00109EA0` every other rigid body runs, already ported as
// `b3_rigid_body_world_contact` (PH-24) with `FUN_00107950`'s OBB narrow
// phase as `b3_rigid_body_obb_plane_contact` (PH-25).  There is no sphere
// sweep and no dedicated wall responder anywhere in that chain.
//
// This is that resolve over ONE caller-supplied contact plane. Live callers
// iterate the gathered collision soup; a sphere sweep supplies only
// anti-tunnelling or no-pipeline fallback planes. `hit_pos` is a point ON the
// surface and `hit_n` the unit normal pointing AT the wreck, both in HARNESS
// space. The response lands in the wreck's +0x110/+0x120/+0x130 accumulators
// and the next `b3_wreck_update` consumes it, so the caller must run this
// BEFORE the update, which is the manager's order. Returns 1 if the narrow
// phase found an overlap.
//
// THE TWO PIECES ARE INSTALLED, NOT LINKED, for the same reason
// `b3_vehicle_chassis_contact` is a hook on the sim struct: this file is
// linked WITHOUT burnout3_vehicle_sim.c by tools/validate_td_rules.py, which
// stubs the single pre-existing cross reference (b3_rigid_body_integrate) by
// hand.  Adding new link edges would break a validator this agent does not
// own, so the harness passes the two entry points in.  Without them
// b3_wreck_world_contact is a no-op returning 0, exactly like an uninstalled
// chassis_resolve.
typedef int (*B3ObbPlaneFn)(const struct B3RigidBody* rb,
                            const float bbmin[3], const float bbmax[3],
                            const float plane_pt[3], const float plane_n[3],
                            B3WorldContact* out);
typedef void (*B3WorldContactFn)(struct B3RigidBody* rb, float mass_kg,
                                 int cls, int attach_mode, float restitution,
                                 const B3WorldContact* c,
                                 B3WorldContactResult* out);
void b3_wreck_set_world_resolve(B3ObbPlaneFn narrow_phase,
                                B3WorldContactFn resolve);

int b3_wreck_world_contact(B3WreckState* w, const float hit_pos[3],
                           const float hit_n[3]);

// ---------------------------------------------------------------------------
// THE SUBSTEP HOOK -- `FUN_0011AEF0` where FUN_0011BE50 actually calls it.
//
// Retail's substep is
//     FUN_0011D460  @0x0011C0A2   tyre force pass
//     [veh+0x212] = [veh+0x213] = 0                       @0x0011C0A9/B0
//     FUN_0011AEF0  @0x0011C0B7   <- THIS
//     FUN_001239C0  @0x0011C0E7   suspension pre-pass
//     FUN_00123FD0  @0x0011C0EE   suspension force pass
//     FUN_00109560  @0x0011C160   integrate
// and the whole point of the position is that the resolve's outputs are
// ACCUMULATOR writes (+0x110 impulse, +0x120 angular impulse, +0x130
// deflection, +0xF0 force) which FUN_00109560 consumes and clears at the end
// of the SAME substep -- so a wall response is a force in that solve, never a
// post-hoc correction of an already-integrated pose.
//
// This is the bridge `B3VehicleFull.chassis_resolve` points at.  It builds
// the B3CrashVehicle view of the sim body, runs `b3_crash_response` over the
// frozen veh+0x200 soup and writes every mutated field back.  Install it with
//     v->chassis_resolve = b3_vehicle_chassis_contact;
// and fill `v->soup` from a `v->soup_freeze` callback (retail FUN_0011BC60
// @0x0011BF43, once per frame).  Returns the wall-contact count, which is
// FUN_0011AEF0's own return value.
int b3_vehicle_chassis_contact(B3VehicleFull* v);

#ifdef __cplusplus
}
#endif

#endif // BURNOUT3_CRASH_H
