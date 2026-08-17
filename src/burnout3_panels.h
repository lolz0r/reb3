// burnout3_panels.h -- Burnout 3's PER-PANEL damage machine, recovered from
// the retail Xbox XBE, plus the flying-panel layer the harness draws.
//
// Provenance markers (same scheme as docs/RE_NOTES.md):
//   [C]  read out of the real code/data at the address given
//   [S]  read from the disassembly, self-consistent, no green case
//   [?]  open
//   GLUE harness assembly around the ported pieces
//
// ---------------------------------------------------------------------------
// WHAT RETAIL ACTUALLY DOES  (this corrects RE_NOTES 13's "crash entry
// detaches every panel" reading, which is what the harness implemented)
// ---------------------------------------------------------------------------
// The wreck stamp FUN_001253C0 takes an ARGUMENT, and the two callers pass
// DIFFERENT values [C, disassembled call sites]:
//
//   FUN_00115130  the ordinary crash entry
//                 0x0011560B  PUSH 0x1
//                 0x0011561A  CALL FUN_001253C0        -> param = 1
//   FUN_00120800  the EXPLODE variant (-Y impulse 100000 = 0x47C35000)
//                 0x0012085E  PUSH 0x0
//                 0x00120860  CALL FUN_001253C0        -> param = 0
//                 0x00120871  CALL FUN_00115130        (then the normal entry)
//
// and the two arms of FUN_001253C0 are completely different:
//   param != 0 : every panel state 0/1 -> 2 (CRUMPLED, still attached and
//                still drawn), every wheel state 0/1 -> 2.
//   param == 0 : every panel accumulator := 1000.0 (0x447A0000) and every
//                wheel accumulator := 2.0, i.e. past FUN_00123000's detach
//                gates -> everything detaches on the next scan.
//
// So an ordinary crash keeps ALL its bodywork at entry; it only crumples.
// Panels come off DURING the flight, one at a time, through the accumulator
// chain below.  The panel-less shell (the .bgv mask-bit1 record set) is the
// state a wreck ARRIVES at, never the state it starts in.
//
// ---------------------------------------------------------------------------
// PANEL STATES (damage ctx +0x4B2, one byte per body part) and their writers
// ---------------------------------------------------------------------------
//   0 PRISTINE   drawn at the placement matrix (.bgv+0xD00 -> ctx+0x180)
//   1 LOOSE      drawn at an ANIMATED matrix FUN_00129640 writes over
//                ctx+0x180 + k*0x40 -- the hanging/flapping panel.  Set by
//                FUN_0012C860 @0x0012C9F9 [C].
//   2 CRUMPLED   drawn at the placement matrix (the deformed record set)
//   3 DETACHED   NOT drawn by the car; a flying-part pool record (0x4E0
//                stride, 64 slots, FUN_00111340) carries the mesh through
//                the air with its own rigid state
//   4 GONE       not drawn at all -- written when the flying part retires
//                (FUN_00115FC0) or its pool slot is stolen (FUN_00111340)
//
// The draw path FUN_000303D0 draws panel slot k+1 for state < 3 only
// (execution-traced, tools/trace_panels.py) -- hence 0/1/2 attached, 3 in
// the air, 4 gone.
//
// ---------------------------------------------------------------------------
// THE ACCUMULATOR CHAIN (all four steps run every frame, in this order)
// ---------------------------------------------------------------------------
// 1. SNAPSHOT.  FUN_00123FD0 @0x001246BB copies ctx+0xF90+k*4 (the live
//    per-panel damage accumulator) to obj+0x160+k*4 before any damage is
//    added this frame [C].
//
// 2. ADD.  FUN_00123FD0 @0x00124709 -- when this frame's scaled impact
//    obj+0x47C exceeds 5.0 -- calls FUN_0012C670(obj, impact):     [C]
//        for each panel k:
//            if dot(contact_dir_body, panel_pivot_pos) < 0:
//                acc[k] += min(impact * 0.002, 0.1)
//        for each wheel w:                       (same shape, 10x and random)
//            if dot(contact_dir_body, wheel_pos) < 0:
//                acc[w] += min(rand01()*0.5*... * impact * 0.02, 0.5)
//    `contact_dir_body` is obj+0x150, the body-space contact axis; a panel
//    whose pivot has a NEGATIVE dot with it is on the struck side.  The
//    impact itself is the frame's contact impulse scaled by the car class
//    (0.0003125 = 1/3200 for class 0, 1/6400, 1/4800; x1.5 if obj+0x346).
//
// 3. RIP TEST.  FUN_0012C860 (from the damage visual update FUN_0012E4D0),
//    per panel, using the step-1 snapshot [C, disassembled 0x0012C940..]:
//        if (acc[k] - snapshot[k] > rip_band[k] * scale)   // ONE-FRAME rise
//            acc[k] = 1000.0                    [0x003B16CC]
//        else if (acc[k] > crumple_thr[k])
//            if state 0/1: acc[k] = crumple_thr[k] + 1.0   // clamp+promote
//        else if (acc[k] > loose_thr[k] && state == 0)
//            if kind == 4 (bonnet) and rand < 0.5: clamp as above instead
//            else state = 1                     // LOOSE
//    scale = 0.3 [0x003B1750] normally, 1.0 [0x003B168C] in the crash-party
//    /takedown modes.  So a single frame that adds more than ~0.042..0.060
//    of damage to a panel RIPS IT OFF; slow accumulation only dents it.
//
// 4. SCAN.  FUN_00123000's head, per panel [C]:
//        if (acc[k] > crumple_thr[k] && state 0/1) state = 2
//        if (acc[k] > 999.0 [DAT_005A80C8]) FUN_00125A50(veh, k)
//    and FUN_00125A50 only acts on state == 2: state := 3, allocate a
//    flying-part slot, and REVERT to 2 if the pool is full.
//
// THRESHOLDS, randomised per car at ctx init FUN_0012FEE0 @0x0012FF1D [C]:
//    ctx+0xFA8 loose   = rand01()*0.5 + 0.5, x 0.1     -> 0.050 .. 0.100
//    ctx+0xFC0 crumple = rand01()*0.5 + 0.5, x 0.17    -> 0.085 .. 0.170
//    ctx+0xFD8 rip     = (rand01()*0.3 + 0.7) x 0.2    -> 0.140 .. 0.200
//
// ---------------------------------------------------------------------------
// PANEL KINDS (.bgv +0xAC4, i32 per body part) -- what they mean
// ---------------------------------------------------------------------------
// The ids are matched against the priority table DAT_00385224 =
// {3,6,0,1,5,4,2} by the health distributor FUN_00023DE0, and the kind
// switch at 0x00023FFB makes kinds {0,1,3,5,6} DENT-ONLY on that path while
// 2, 4 and >6 detach.  Cross-referencing the ids with the placement pivots of
// all 67 extracted cars (build/cars/*.panels, .bgv+0xD00 row 3) fixes the
// geometry, and FUN_0012C860's special cases confirm kind 4:
//
//   kind 0  RIGHT DOOR   x = +0.0..1.21, y -0.11..1.64, z -0.83..3.97 (66 cars)
//   kind 1  LEFT DOOR    the mirror of kind 0, always slot 1          (66 cars)
//   kind 2  FRONT        z +1.37..5.15, low y: front wing/bumper      (63 cars)
//   kind 3  REAR         z -5.47..-1.43: rear wing/bumper             (58 cars)
//   kind 4  BONNET/HOOD  x == 0.0 EXACTLY, y +0.46..1.06, z -0.30..1.96
//                        (54 cars) -- and FUN_0012C860 gives kind 4 alone
//                        a 50% coin flip that SKIPS the loose state
//                        [0x0012C9D9 CMP ...,0x4 / 0x0012C9E5 rand], plus a
//                        dedicated second pass at 0x0012CAC8 that poses it
//                        from the .bgv+0x70 aux matrix DAT_003EBFAC = 4 --
//                        the bonnet-flips-up-over-the-windscreen animation.
//   kind 5  BOOT/HATCH   centreline, y +0.31..2.73, z negative        (55 cars)
//   kind 6  extra rear   z -1.50..-2.49, only 13 cars (truck tailgates,
//                        big spoilers); dent-only on the health path
//
// ---------------------------------------------------------------------------
// FLIGHT: what a released panel becomes (FUN_00106F20, the flying-part init)
// ---------------------------------------------------------------------------
//   world frame  = panel placement matrix x car frame        [C]
//                  (FUN_000116E0, row-vector, then copied to rec+0x70/+0x204)
//   velocity     = the CAR's velocity AT THE PIVOT POINT, FUN_001066A0
//                  (v + omega x r), scale 1.0                [C]
//                  (kind byte 2 = generic debris adds a random scatter
//                   (rand-0.5, rand*0.5, rand-0.5) x (rand+1)*15)
//   ang.momentum = car angular momentum x (part mass / car mass)
//                  + frame-transformed per-part vector, the sum then x 2.5
//                  [C, disassembled 0x00107151..0x00107204; the 2.5 is
//                  DAT_003A2D50 @0x001071D6, the mass ratio is the DIVSS
//                  piece+0x1F0 / veh+0x1F0 @0x00107173]
//   then FUN_000FFC80 installs the velocity and the part is integrated by
//   the same rigid-body path as the car.
//
// ---------------------------------------------------------------------------
// SEEDING: FUN_001069C0, the ACTIVATION ctor (PHYS-LEDGER wave 4)
// ---------------------------------------------------------------------------
// FUN_00111340 pulls a free slot out of the 64-slot arena and immediately
// calls FUN_001069C0(EAX = part index, ECX = owner vehicle, [EBP+8] = MODE).
// The MODE byte is the pool allocator's 4th stack argument and it lands at
// piece+0x2BA @0x001069EA.  The three call sites pass three values [C]:
//     FUN_00123000 @0x001231E0  PUSH 0  -> a WHEEL    (states ctx+0x4AC)
//     FUN_00125A50 @0x00125A6F  PUSH 1  -> a PANEL    (states ctx+0x4B2)
//     FUN_00125AC0              PUSH 2  -> glass/debris
// so a detached BODY PANEL runs with +0x2BA == 1.  That settles the old [?]:
// FUN_001072A0's SPHERE narrow phase is gated on +0x2BA == 0 and is only
// ever reached by a detached WHEEL (whose radius +0x1CC comes from
// veh+0x870+idx*0xC0 @0x00106A43); a panel always takes the OBB arm.  The
// modrm-aware sweep of every executable LOAD segment finds exactly three
// writers of +0x2BA -- 0x0010692C (=3, the pool ctor FUN_001068A0),
// 0x001069EA (= mode) and 0x00106EFB (=3, the orphan helper that also clears
// the owner +0x2B0) -- so 3 is only the "unowned" default and 0 is only ever
// a wheel.
//
// What FUN_001069C0's mode-1 arm seeds, all [C]:
//     bgv = *(*(veh+0xCC0)+0x40)            the .bgv image
//     piece+0x1D0 (bbMAX) = bgv[0xEA0 + k*0x20 + 0x00 .. +0x0C]
//     piece+0x1E0 (bbMIN) = bgv[0xEA0 + k*0x20 + 0x10 .. +0x1C]
//     piece+0x260 (CENTRE) = (min + max) * 0.5, then one axis zeroed by
//         bgv+0xADC+k:  0 -> centre.y = 0
//                       1 -> centre.x = 0
//                       2 -> centre.y = centre.z = 0
//     piece+0x1F0 (MASS)  = 0x43820000 = 260.0f, unconditional, all modes
//     then FUN_00109BB0 (below) builds the inverse inertia from the RAW
//     (not yet recentred) box, and only afterwards does FUN_00106F20
//     @0x00107217 do  bbmin -= centre; bbmax -= centre.
//
// FUN_00109BB0 @0x00109BB0 -> FUN_00109190 -- the INERTIA law.  (Ghidra's
// decompile shows only one axis; the disassembly 0x00109BBF..0x00109CC8 has
// all three.)  With a = max(bbmax.x, -bbmin.x) and b, c likewise,
// K = [0x003B1684] = 0.5 and N = [0x003B168C] = 1.0:
//     Iinv_body = diag( N/(K*m*(b*b+c*c)),
//                       N/(K*m*(a*a+c*c)),
//                       N/(K*m*(a*a+b*b)) )
// written to body+0x10 / +0x24 / +0x38.  Note K = 0.5, not the solid box's
// 1/3 -- this is the game's own law, not a textbook tensor.
//
// The per-part angular-momentum seed (veh+0xD70 + k*0xC0) is PANEL RECORD
// + 0x90: the vehicle ctor FUN_00122830 @0x001229C8 REP-STOSDs
// veh+0xCE0..+0x1160 to zero and then builds a record array at veh+0xCE0,
// stride 0xC0 (@0x001229ED: FUN_00121D70(rec); rec+0x20 = bgv+0xAFC+k*4;
// rec+0x28 = ctx+0x180+k*0x40; FUN_00121F80(rec, bgv+0xEA0+k*0x20)).
// FUN_00121D70 zeroes rec+0x90..+0x9C and sets rec+0xB4 = 260.0 -- the SAME
// mass -- and the only writer of rec+0x90 in the image is FUN_00122270
// @0x001222B1, the LOOSE (state 1) HINGE INTEGRATOR
// (rec+0x80 += rec+0x70*dt; rec+0x90 += rec+0x80).  This harness does not
// animate the loose pose (see "WHAT IS GLUE HERE"), so the seed is the
// ctor's zero -- which is exactly what retail hands a panel that never
// entered state 1.  [S]
//
// ---------------------------------------------------------------------------
// WHAT IS GLUE HERE
// ---------------------------------------------------------------------------
// * The IMPACT SOURCE.  Retail's obj+0x47C comes from the suspension pass's
//   contact impulses; the harness wreck has no wheel rays, so the caller
//   feeds b3_panels_add_impact from the crash module's own box-corner contact
//   set.  The 1/3200 class scale, the 5.0 gate and the 0.002/0.1 add law are
//   ported exactly.
// * The FLYING-PART INTEGRATOR.  Retail's pool record is a full rigid body
//   with a vtable; here a piece is integrated with the same verified
//   b3_rigid_body_integrate (FUN_00109560) at the recovered gravity, with a
//   GLUE box inertia, ground contact and rest test.
// * The LOOSE (state 1) POSE.  Retail animates it through FUN_00129640 about
//   a per-panel hinge axis chosen by .bgv+0xADC[k] (0/1/2 -> X/Y/Z, the unit
//   vectors at DAT_0040AF60/70/80) with the sign of .bgv+0xAFC+k*4.  Neither
//   field is in the extractor's .panels sidecar, so state 1 draws at the
//   placement matrix like state 0 here; the STATE ITSELF is ported.
// ---------------------------------------------------------------------------

#ifndef BURNOUT3_PANELS_H
#define BURNOUT3_PANELS_H

#include "burnout3_crash.h"          // B3WreckState, B3RigidBody

#ifdef __cplusplus
extern "C" {
#endif

// .bgv+0xC is a u8 and the section part table has 10 slots with slot 0 the
// aperture body and 7..9 the wheels, so numBodyParts <= 6 in every shipped
// file; 8 leaves headroom without costing anything.
#define B3_PANEL_MAX 8

// Panel states -- the ctx+0x4B2 byte values (see the header comment).
enum {
    B3_PANEL_PRISTINE = 0,
    B3_PANEL_LOOSE    = 1,
    B3_PANEL_CRUMPLED = 2,
    B3_PANEL_DETACHED = 3,
    B3_PANEL_GONE     = 4
};

// Panel kinds (.bgv+0xAC4) -- see the header comment for the derivation.
enum {
    B3_PANEL_KIND_DOOR_R = 0,
    B3_PANEL_KIND_DOOR_L = 1,
    B3_PANEL_KIND_FRONT  = 2,
    B3_PANEL_KIND_REAR   = 3,
    B3_PANEL_KIND_BONNET = 4,
    B3_PANEL_KIND_BOOT   = 5,
    B3_PANEL_KIND_REAR2  = 6
};

// --- the recovered constants ------------------------------------------------
#define B3_PANEL_LOOSE_BAND    0.10f    // FUN_0012FEE0 @0x0012FF1D    [C]
#define B3_PANEL_CRUMPLE_BAND  0.17f    // FUN_0012FEE0                [C]
#define B3_PANEL_RIP_BAND      0.20f    // FUN_0012FEE0                [C]
#define B3_PANEL_RIP_BASE      0.7f     // rip = (rand*0.3 + 0.7)*0.2  [C]
#define B3_PANEL_RIP_SPAN      0.3f
#define B3_PANEL_RIP_SCALE     0.3f     // DAT_003B1750                [C]
#define B3_PANEL_RIP_SCALE_TD  1.0f     // DAT_003B168C (crash party)  [C]
#define B3_PANEL_DETACH_ACC 1000.0f     // DAT_003B16CC / FUN_001253C0 [C]
#define B3_PANEL_DETACH_GATE 999.0f     // DAT_005A80C8                [C]
#define B3_PANEL_HIT_SCALE   0.002f     // FUN_0012C670                [C]
#define B3_PANEL_HIT_CAP     0.1f       // FUN_0012C670                [C]
#define B3_PANEL_IMPACT_GATE 5.0f       // DAT_003B1694                [C]
#define B3_PANEL_IMPACT_SCALE 0.0003125f// class 0; DAT_003B1E70 and
                                        // FUN_00123FD0's own copy     [C]
// The suspension/flight arm has its OWN pair: FUN_00123FD0 only looks at a
// contact whose raw impulse clears 2000 [0x00124364 COMISS ...,2000.0] and
// then halves it before the class scale [0x001243F6 fVar26 * 0.5].
#define B3_PANEL_WHEEL_GATE  2000.0f
#define B3_PANEL_WHEEL_HALF  0.5f

// One panel in flight.  Frame rows are right/up/at/pos in HARNESS space,
// exactly like B3WreckState::frame, so the harness builds its GL matrix the
// same way it does for the shell.
typedef struct {
    int   active;
    int   panel;               // index into B3PanelSet
    float frame[4][4];
    float vel[4];              // [3] = speed, as B3RigidBody wants
    float omega[4];
    float angmom[4];
    float iinv_body[3][4];     // +0x10 diag, FUN_00109BB0            [C]
    float mass;                // +0x1F0 = 260.0, FUN_001069C0 tail   [C]
    float bbmax[3];            // +0x1D0, recentred                   [C]
    float bbmin[3];            // +0x1E0, recentred                   [C]
    float half[3];             // max(|bbmax|,|bbmin|) -- the ground test
    float life;                // seconds since release
    float rest;                // seconds resting on the ground
} B3PanelPiece;

// Per-car panel machine -- the fields of the damage ctx this needs.
typedef struct {
    int   n;                                   // .bgv+0xC numBodyParts
    int   kind[B3_PANEL_MAX];                  // .bgv+0xAC4
    float attach[B3_PANEL_MAX][3];             // .bgv+0xD00 row 3, GAME space
    unsigned char state[B3_PANEL_MAX];         // ctx+0x4B2
    float acc[B3_PANEL_MAX];                   // ctx+0xF90
    float snap[B3_PANEL_MAX];                  // obj+0x160 (frame snapshot)
    float thr_loose[B3_PANEL_MAX];             // ctx+0xFA8
    float thr_crumple[B3_PANEL_MAX];           // ctx+0xFC0
    float thr_rip[B3_PANEL_MAX];               // ctx+0xFD8
    unsigned rng[2];                           // the game's LCG pair
    int   party;                               // rip scale selector (0/1)
    // The per-panel pivot-local AABB the flying piece is seeded from.
    // .bgv+0xEA0 + k*0x20: max at +0x00, min at +0x10; the hinge-axis byte
    // is .bgv+0xADC+k.  FUN_001069C0's mode-1 arm @0x001069C0 reads exactly
    // these three.  box_ok[k] == 0 means the .panels sidecar predates the
    // `panelbb` line and panel_piece_spawn falls back to the old cube.  [C]
    float box_max[B3_PANEL_MAX][3];
    float box_min[B3_PANEL_MAX][3];
    unsigned char box_axis[B3_PANEL_MAX];
    unsigned char box_ok[B3_PANEL_MAX];
    B3PanelPiece piece[B3_PANEL_MAX];
    int   detached_total;                      // diagnostics
} B3PanelSet;

// FUN_0012FEE0's panel half: zero every state/accumulator and roll the three
// per-panel thresholds.  `seed` seeds the ported LCG so a run is repeatable.
// attach[] positions are the .bgv+0xD00 row-3 pivots in GAME space (+Z nose);
// pass them straight from the extractor's .panels sidecar.
void b3_panels_reset(B3PanelSet* s, int n, const int* kinds,
                     const float (*attach)[3], unsigned seed);

// The per-panel pivot-local AABB out of the .bgv, for panel k.  `bbmax` is
// .bgv+0xEA0 + k*0x20 + 0x00, `bbmin` is +0x10, `axis` is .bgv+0xADC+k --
// exactly the three fields FUN_001069C0's mode-1 arm loads.  Call it after
// b3_panels_reset (which clears the set) and before the first crash.  [C]
void b3_panels_set_box(B3PanelSet* s, int k, const float bbmax[3],
                       const float bbmin[3], int axis);

// FUN_00109BB0 -> FUN_00109190: the flying part's inverse-inertia DIAGONAL
// from its OBB and its mass.  Exported so tools/dump_traj.c --pieceseed can
// be differentially checked against the real x86 (tools/validate_port.py).
void b3_piece_inertia(float mass, const float bbmax[3], const float bbmin[3],
                      float diag[3]);

// FUN_001069C0's box centre (one component zeroed by the .bgv+0xADC hinge
// byte) + FUN_00106F20 @0x00107217's subtraction.
void b3_piece_recentre(const float bbmax[3], const float bbmin[3], int axis,
                       float outmax[3], float outmin[3]);

// FUN_00127180, the CRASH-ENTRY DAMAGE BURST -- the piece that makes a crash
// actually shed bodywork.  The ordinary crash entry calls it at
// FUN_00115130 @0x00115265, BEFORE the crumple stamp at @0x0011561A, and it
// fires FIVE FUN_001270D0 deformation/damage events, each at magnitude
// 1000000.0 (0x49742400) from a face of the car's own bbox, in this order
// [C, disassembled 0x00127189..0x00127331]:
//     1  (0,-1,0)  roof crush   (bbox lerp 0.5,1.0,0.5)
//     2  (0,0,-1)  front crush
//     3  (0,0,+1)  rear crush
//     4  (-1,0,0)  right crush
//     5  (+1,0,0)  left crush
// Each event runs the whole FUN_0012FA40 chain, which RE-SNAPSHOTS the
// accumulators (@0x0012FCD0) before adding -- so all five push the panels
// far past their crumple thresholds, but the one-frame RIP test only ever
// sees the LAST event's rise.  That is what makes a crash tear off a panel
// or two at entry instead of the whole car, and leaves the rest hanging on a
// hair-trigger accumulator for the flight's next real impact.
// [?] each event is additionally gated by FUN_00127080 (a geometry hit test
// on the deformation ray); this port fires all five.
#define B3_PANEL_BURST_MAG 1000000.0f   // 0x49742400                   [C]
void b3_panels_entry_burst(B3PanelSet* s);

// FUN_001253C0.  detach_all = 0 -> the ORDINARY crash entry (FUN_00115130,
// PUSH 1): crumple every panel, detach nothing.  detach_all = 1 -> the
// EXPLODE stamp (FUN_00120800, PUSH 0): every accumulator to 1000, so the
// next b3_panels_scan releases everything.
void b3_panels_wreck_stamp(B3PanelSet* s, int detach_all);

// FUN_0012C670.  dir_body = the body-space contact axis (retail's obj+0x150);
// impact = the ALREADY SCALED magnitude (retail's obj+0x47C).  The 5.0 gate
// is applied here so callers can hand over the raw frame impact.
void b3_panels_add_impact(B3PanelSet* s, const float dir_body[3],
                          float impact);

// The COLLISION damage entry, FUN_00111CD0 -> FUN_0012FA40 -> FUN_0012C670.
// The collision resolver hands FUN_0012FA40 the hit's raw magnitude (N.s, the
// contact's momentum change); FUN_0012FA40 scales it by the car class
// (0.0003125 / 0.00020833 / 0.00015625 at DAT_003B1E70/74/78 -- the SAME
// family the suspension path uses), stashes it at obj+0x17C, and passes it on
// when it clears 5.0 [DAT_003B1694].  `n_world` is the contact normal
// pointing INTO the car; `frame` is the car's rows right/up/at.
void b3_panels_impact_world(B3PanelSet* s, const float frame[4][4],
                            const float n_world[3], float raw_impulse);

// FUN_0012C860's per-panel block: the one-frame rip test against the
// snapshot, the crumple clamp, and the LOOSE promotion (with kind 4's coin
// flip).  Call once per frame AFTER all of the frame's impacts.
void b3_panels_visual_pass(B3PanelSet* s);

// FUN_00123000's panel scan + FUN_00125A50: promote to CRUMPLED, then
// release anything past the 999 gate as a flying piece seeded from the
// wreck's pose/velocity (FUN_00106F20).  Returns the number released.
int  b3_panels_scan(B3PanelSet* s, const B3WreckState* w);

// FUN_00023DE0's health-driven distributor (the RACING path -- it is gated
// on veh+0x210 == 0, i.e. NOT in crash mode).  Dents and then detaches
// panels in the DAT_00385224 = {3,6,0,1,5,4,2} kind order until only
// int(nparts * min(1, health + 0.1)) are left; kinds {0,1,3,5,6} are
// dent-only.  Passing a NULL wreck leaves released panels as bare state 3.
void b3_panels_health(B3PanelSet* s, float health, const B3WreckState* w);

// One crashed frame: snapshot, feed the wreck's own contacts through
// FUN_0012C670's law, run the rip pass, and run the scan.
// The wreck's contact report is CONSUMED (cleared) here, so an entry hit
// published by b3_wreck_begin_kick is seen exactly once even though
// b3_wreck_update runs first in the harness frame.
void b3_panels_crash_frame(B3PanelSet* s, B3WreckState* w);

// Piece flight only (gravity, ground contact, rest). `ground_y[k]` is the
// collision height under detached piece k; exposed for the frame manager.
void b3_panels_pieces_update(B3PanelSet* s,
                             const float ground_y[B3_PANEL_MAX], float dt);

// Drawn attached?  The retail draw gate is state < 3 (trace_panels.py).
static inline int b3_panel_attached(const B3PanelSet* s, int k) {
    return k >= 0 && k < s->n && s->state[k] < B3_PANEL_DETACHED;
}

#ifdef __cplusplus
}
#endif

#endif // BURNOUT3_PANELS_H
