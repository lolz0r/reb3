#ifndef BURNOUT3_PROPS_H
#define BURNOUT3_PROPS_H
/* =========================================================================
 * DESTRUCTIBLE TRACK PROPS -- traffic cones, roadwork barrier boards,
 * bulb-topped marker posts, signposts, boxes, benches.
 *
 * WHERE THEY COME FROM.  Every shipped track's `static.dat` carries a second
 * 0x70-record model table at header +0x3C (count u16 +0x36) plus an instance
 * transform table at +0x48 (count u16 +0x40, 0x40 bytes each = one 4x4), a
 * per-model class byte array at +0x44, and per-streamed-unit instance lists at
 * +0x4C/+0x50.  The game's own reader is `FUN_00110420 @0x001109CB..0x00110A46`
 * [C-disasm]; `tools/extract_props.py` transcribes it and bakes the result to
 * `build/tracks/<ID>/props.bin`.  13,359 prop instances across the 37 shipped
 * tracks -- 123 on US_C3_V1 (53 of them cones).
 *
 * WHAT HAPPENS WHEN YOU HIT ONE.  A prop lives in the world-object list as
 * slot TYPE 5 (`FUN_00110420 @0x00110A19`, slot stride 0x30).  The contact
 * dispatcher `FUN_00111CD0` routes a (car-ish, type-5) pair -- its third
 * branch @0x00111DEB..0x00111E1B, `FUN_0010FB20(other) && byte[handle] == 5`
 * -- to `FUN_00113890`, which: [C-disasm, addresses re-checked against
 * build/burnout3.elf]
 *     0x00113901  FUN_001084E0        -- the "may I knock it" gate: a purely
 *                                        GEOMETRIC box-overlap test on the two
 *                                        bboxes (@0x001084EF..0x00108524 reads
 *                                        both bbox rows and halves their sum).
 *                                        There is no mass/size veto: every
 *                                        static prop retail overlaps is knocked.
 *     0x0011392E  FUN_00197A20        -- prop-hit score/audio, and only when the
 *                                        other handle's type is 0/1/2 (cars)
 *     0x0011393B  FUN_00114730        -- PROMOTE: hand the prop one of the 16
 *                                        class-6 rigid bodies (pool at
 *                                        gameworld+0xC4380 @0x001147E4, stride
 *                                        0x780 @0x001147BD, allocation bitmask
 *                                        +0xE9CA0/+0xE9CA4 @0x00114737/0x114750,
 *                                        16-slot scan @0x0011476D; the handle's
 *                                        type byte becomes 6 @0x0011478B).  When
 *                                        the pool is full the body with the
 *                                        smallest +0x224 is recycled and the
 *                                        RECYCLED prop's world slot is retyped
 *                                        to 8 @0x0011480C -- and a type-8 handle
 *                                        is dropped by the dispatcher outright
 *                                        (@0x00111D0B), i.e. a prop that has had
 *                                        its body taken away stops colliding.
 *                                        That is B3P_SETTLED below.
 *     0x0011394E  FUN_00113960        -- resolve the contact with THE SAME
 *                                        generic rigid solver used for
 *                                        car-vs-car and car-vs-traffic (guarded
 *                                        by `type < 8` @0x00113947)
 * and the fresh body is set up by `FUN_0011A020 @0x0011A0A5..0x0011A317` [C]:
 *     body+0x1CC  radius = |model bbox max|            @0x0011A0DE sqrtss
 *     body+0x1D0/+0x1E0   the model's bbox max / min   @0x0011A0A5/0x0011A0AF
 *     body+0x20C  = 1 (the precise-contact flag)       @0x0011A12B
 *     body+0x1F0  MASS = max(100, (bbmax.z-bbmin.z) * (bbmax.x-bbmin.x) * 200)
 *                 @0x0011A137..0x0011A191; the three constants are the floats
 *                 [0x003A2928] = 100.0, [0x003A292C] = 200.0 and the MAXSS
 *                 @0x0011A17D, all read out of the image.
 *     body+0x10/+0x24/+0x38  INVERSE INERTIA, `FUN_00109BB0` @0x0011A199 ->
 *                 `FUN_00109190`.  With e_k = max(bbmax.k, -bbmin.k):
 *                     Ixx^-1 = 1 / ((e_y^2 + e_z^2) * m * 0.5)
 *                     Iyy^-1 = 1 / ((e_x^2 + e_z^2) * m * 0.5)
 *                     Izz^-1 = 1 / ((e_x^2 + e_y^2) * m * 0.5)
 *                 (@0x00109BB9..0x00109CC8; 0.5 = [0x003B1684], 1.0 =
 *                 [0x003B168C]).  FUN_00109190 then builds the WORLD inverse
 *                 inertia at +0x40 from the frame, so the very first contact
 *                 in the same frame already has it.
 *     body+0x1F4  com height = (bbmax.y + bbmin.y) * 0.5  @0x0011A2F8..0x0011A317
 *                 -- this is what makes a knocked prop TUMBLE: FUN_00109560's
 *                 state-6 branch (@0x00109606 `CMP byte[+0x215],6`) applies
 *                 gravity at pos + up*com_height, i.e. as a torque, because a
 *                 cone's authored origin is at its base.
 *     body+0xB0   THE LAUNCH.  @0x0011A1DD..0x0011A2F3 draws four numbers from
 *                 the game PRNG (state DAT_0064ACE8/DAT_0064ACEC, scale
 *                 [0x0054F46C] = 2^-32) and stores the product through
 *                 `FUN_000FFC80` (which also refreshes +0xBC speed / +0xC0
 *                 unit direction):
 *                     v = (u0-0.5, u1*0.5, u2-0.5) * ((u3+1.0) * 5.0)
 *                 [0x003B1684]=0.5 [0x003B168C]=1.0 [0x003B1694]=5.0.  The y
 *                 term is never negative, so EVERY knocked prop is thrown UP
 *                 as well as tumbling -- the retail cone pop.
 *     body+0x224  LRU key = DAT_0060EA20 + [0x003A7F34] = clock + 10.0
 *                 @0x0011A19E..0x0011A1B4.  It is NOT a despawn timer: the
 *                 only reader in the image is FUN_00114730's recycle scan,
 *                 which picks the SMALLEST (= oldest) among bodies with
 *                 +0x220 != 0 and +0x210 == 0.  Nothing compares it against
 *                 the clock, so a knocked prop keeps its body until a newer
 *                 knock needs the slot.  (Verified: the only +0x224 writers
 *                 are FUN_00119F40/FUN_0011A020/the release vfunc.)
 *     body+0x204  -> THE INSTANCE MATRIX ITSELF (@0x0011A062), so a knocked
 *                    prop's live pose overwrites its placement entry in place
 *                    (which is why the renderer needs no separate dynamic
 *                    list).  The four `w` slots m[3]/m[7]/m[11]/m[15] are
 *                    explicitly saved (@0x0011A032..0x0011A05D) and restored
 *                    (@0x0011A06D..0x0011A094) across the body reset -- they
 *                    are NOT part of the transform, they carry the instance's
 *                    baked half-range light colour.
 *
 * THE GENERIC SOLVER, `FUN_00113960`  [C].  It is the same function the car
 * agent already ported as b3_carcol_resolve_wreck (docs/RE_CARCOL.md), and a
 * (car, knocked prop) pair walks it exactly like a (car, wreck) pair:
 *     FUN_0010FC50 -> DAT_0039AE88[class(a)*7 + class(b)] gives each side's
 *       role.  A prop is handle type 5/6 -> class 6 (FUN_0010FBC0's jump table
 *       @0x0010FC04) and row/column 6 of the role table is all zeros, so
 *       neither side is statically immovable...
 *     ...but @0x00113B57..0x00113B7E FORCES role(A) = 2 when handle A is a car
 *       (type 0/1/2) that is NOT crashed.  Role 2 means "immovable in this
 *       pair", so an un-crashed car takes NO reaction from a cone and the prop
 *       takes the WHOLE impulse.  A wrecked car does take its half.
 *     @0x00113E9C..0x00113F16 the contact normal is BENT toward the relative
 *       velocity: n := normalize(n + (-0.9) * unit(v_prop_pt - v_car_pt)),
 *       [0x0041A4C0] = -0.9, skipped when |v_rel|^2 < [0x003B191C] (2^-32).
 *     @0x00113F78 `FUN_0010F8D0` -- the two-body impulse
 *           j = |-(e+1) * (n.v_rel)| / (1/m_car + 1/m_prop
 *                + n . [ (Iinv_c (r_c x n)) x r_c + (Iinv_p (r_p x n)) x r_p ])
 *       with e = DAT_004A1D98 = 0.0 (the generic-solver restitution).  Note
 *       BOTH masses and BOTH inertias are in the denominator even though only
 *       the prop is moved -- that is why a cone does not leave the county at
 *       150 mph, and why no launch cap exists (or is needed) in retail.
 *     @0x00113FAD the impulse is applied to the prop only, negated
 *       ([0x003B16C0] = -1.0), through `FUN_00106500`: +0x110 += J and
 *       +0x120 += (P - pos) x J.
 *     @0x0011408B..0x001140D4 the crash threshold ([0x003EBE50] = 5000) is
 *       gated behind FUN_0010FC30 / DAT_0039AE50, whose row 6 is zero -- so a
 *       prop can never wreck a car no matter how hard the impulse (below).
 *
 * THE PER-FRAME BODY UPDATE is vtable slot +0x00 of the class-6 body vtable at
 * 0x003B1120, i.e. `FUN_0011A330` [C], driven once per frame per allocated
 * body from the collision manager FUN_00110AF0 @0x00110FB4 with the frame dt:
 *     if (body+0x216 == 0xFF) return;          not inside a loaded track unit
 *     force  += dir(+0xC0) * -(speed(+0xBC)^2)          @0x0011A370 [0x3B16C0]
 *     torque += omega(+0xD0) * (|omega| * -2.0)         @0x0011A3B4 [0x3B17F8]
 *     FUN_00109560(body, dt)                            @0x0011A42F
 *     restore the matrix w slots                        @0x0011A434..0x0011A45B
 * That is the WHOLE update: two quadratic drag terms and the shared rigid-body
 * integrator.  There is no ground pass, no bounce coefficient, no spin cap and
 * no settle test anywhere in the class-6 body's path -- searched: the vtable
 * has only update/release/set-crashed/clear-contacts/unit-refresh/dtor, and
 * FUN_0011A330 calls nothing but the integrator.  Our harness adds a minimal
 * ground stop (marked GLUE in the .c) because our world persists behind the
 * player where the retail one recycles the body.
 *
 * WHY A CONE DOES NOT WRECK YOU  [C].  The object-crash trigger FUN_00112E70
 * has exactly one caller, the dispatcher's FIRST branch
 * @0x00111D14..0x00111D77, and that branch needs one handle of TYPE 3.  A
 * static prop is type 5 and a knocked one is type 6, so a car-vs-prop pair
 * takes the third branch (FUN_00113890) or, once promoted, the generic-solver
 * branch @0x00111E22 (FUN_0010FB20 accepts 6) -- it NEVER reaches the crash
 * trigger.  The type->class map FUN_0010FBC0 (jump table @0x0010FC04) says the
 * same thing from the other side: types 5, 6 and 7 all fall through to class 6
 * @0x0010FBFC, and DAT_0039AE50 row 6 is all zeros, so even if a prop pair did
 * reach the trigger the crashable test @0x0011304E would refuse it.  This
 * module therefore hands EVERY prop contact to b3_td_object_contact() with the
 * recovered class 6 and lets the retail table return the verdict.
 *
 * BOUNDARY.  This module owns placement, meshes, knock physics and drawing.
 * It does NOT implement the crash trigger: it fills B3PropHit below with the
 * kinematics and the recovered class/mass that FUN_00112E70's port
 * (b3_td_object_contact, burnout3_td_rules.h section 10) consumes.
 * ====================================================================== */

#include "burnout3_vehicle_sim.h"   /* B3RigidBody, b3_rigid_body_integrate */

/* One car-vs-prop contact resolved this frame.  Positions/normals are in
 * HARNESS (GL) space, the space `Vehicle.pos` and `b3_ground_probe` use. */
typedef struct {
    int   instance;      /* prop instance index, 0..b3_props_count()-1       */
    int   model;         /* prop model index within this track              */
    int   prop_class;    /* static.dat +0x44, 0..7.  1 == CONE on every      */
                         /* shipped track; 4/5 barrier boards, 6 signposts,  */
                         /* 2/3 benches/bins, 7 the heavy blocks/tables.     */
    int   car;           /* vehicle slot that hit it                        */
    int   obj_class;     /* FUN_0010FBC0 class for the crash trigger: 6 for  */
                         /* every static.dat prop (handle type 5/6 ->        */
                         /* @0x0010FBFC), which DAT_0039AE50 row 6 makes     */
                         /* non-crashable.  b3_props_object_class() below.   */
    float mass;          /* FUN_0011A020 @0x0011A191 [C], footprint*200 >=100 */
    float radius;        /* FUN_0011A020 @0x0011A0DE [C], |bbox max|         */
    float point[3];      /* contact point                                    */
    float normal[3];     /* unit, prop -> car                                */
    float vrel[3];       /* the PROP's point velocity minus the CAR's,       */
                         /* HARNESS space -- retail computes exactly this at */
                         /* FUN_00112E70 @0x00113311 and takes the normal    */
                         /* component of it.  Negate z for game space.       */
    float closing_mph;   /* |v_rel . normal| * 2.2369363 [0x0038994C], the   */
                         /* units FUN_00112E70 @0x00113331 compares against  */
                         /* authority*75 (normal race) / authority*20 (crash */
                         /* party).  The comparison itself is NOT done here. */
    float impulse;       /* N.s this module applied to the prop              */
} B3PropHit;

/* Load build/tracks/<ID>/props.bin plus the prop textures.  Returns the
 * instance count; 0 means "no prop data" and every other entry point becomes a
 * no-op.  Safe to call with a GL context current (it builds display lists). */
int  b3_props_load(const char* track_dir);
int  b3_props_ready(void);
int  b3_props_count(void);        /* instances placed on this track          */
int  b3_props_live(void);         /* dynamic bodies simulating right now     */

/* Race restart: every prop back to its authored transform, pool emptied. */
void b3_props_reset(void);
void b3_props_shutdown(void);

/* Integrate the knocked props.  `dt` is the DILATED sim delta, the same one
 * traffic and the animated-material ticker get. */
void b3_props_update(float dt);

/* Draw every prop at its live transform.  Call inside the world pass (retail
 * fogs the world only, and props are world geometry). */
void b3_props_draw(void);

/* Collide one car against the props and knock what it touches.  `pos` is the
 * car's body centre and `vel` its velocity, both HARNESS space; `yaw` is the
 * harness heading (forward = (sin y, 0, -cos y)); `half_ext` the car's object
 * space half extents.  Contacts are written to `out` (up to `max_out`) for the
 * object-crash trigger to judge; returns how many were written. */
int  b3_props_collide_car(int car, const float pos[3], const float vel[3],
                          float yaw, const float half_ext[3],
                          B3PropHit* out, int max_out);

/* The same thing over the car's REAL rigid body, which is what retail's
 * FUN_00113960 consumes: the impulse denominator needs the car's mass AND its
 * world inverse inertia, the relative velocity is taken at the contact POINT
 * (so the car's omega counts), and a CRASHED car is no longer role 2 and does
 * take its half of the impulse back (@0x00113B57).  `car_rb` may be written
 * when `car_crashed` is set; pass the vehicle's B3VehicleFull.rb.  Prefer this
 * entry point; b3_props_collide_car() above forwards to it with a synthesised
 * body (zero omega, identity inertia, B3P_CAR_MASS_FALLBACK kg). */
int  b3_props_collide_rb(int car, B3RigidBody* car_rb, float car_mass,
                         const float half_ext[3], int car_crashed,
                         B3PropHit* out, int max_out);
#define B3P_CAR_MASS_FALLBACK 1200.0f

/* Live-body telemetry for the validator / the trace: returns 1 and fills the
 * caller's copy when `instance` currently owns one of the 16 class-6 bodies. */
int  b3_props_body_state(int instance, B3RigidBody* out_rb, float* out_mass,
                         float* out_com_height, float* out_lru_key);

/* Reseed the prop PRNG (the retail one is the shared global at DAT_0064ACE8,
 * so the sequence itself is not reproducible from outside -- see the .c). */
void b3_props_seed(unsigned state, unsigned inc);

/* ---- differential test surface (tools/validate_props.py) ----------------- *
 * The recovered laws over caller-supplied state, so the acceptance suite can
 * run them against the real x86 under Unicorn without a GL context or a
 * props.bin.  Nothing in the game calls these. */

/* FUN_0011A020: hand a body the model's bbox.  Fills the inverse inertia
 * (FUN_00109BB0/FUN_00109190), mass, radius, com height and the PRNG launch
 * velocity (+0xB0/+0xBC/+0xC0). */
void  b3_props_test_body_setup(const float frame[4][4], const float bbmax[4],
                               const float bbmin[4], unsigned rng_state,
                               unsigned rng_inc, B3RigidBody* out_rb,
                               float* out_mass, float* out_com_height,
                               float* out_radius);
/* FUN_0011A330: one whole body update (both drag terms + FUN_00109560). */
void  b3_props_test_body_step(B3RigidBody* rb, float mass, float com_height,
                              float dt);
/* FUN_00113960's arm for (un-crashed car A, prop B) at a known contact:
 * point velocities, the -0.9 normal bend, FUN_0010F8D0 and the negated apply.
 * Returns |j|; out_normal receives the bent normal, out_imp the raw
 * FUN_0010F8D0 output (n * -|j|).  `prop_rb` is updated in place. */
float b3_props_test_contact(B3RigidBody* prop_rb, float prop_mass,
                            const B3RigidBody* car_rb, float car_mass,
                            const float point[4], const float normal[4],
                            float out_normal[4], float out_imp[4]);

/* Nearest prop to a point, for probes/telemetry.  Returns the instance index
 * or -1; `out_dist` may be NULL. */
int  b3_props_nearest(const float pos[3], float* out_dist);

/* Per-instance query for the crash trigger and for telemetry. */
int   b3_props_class_of(int instance);   /* static.dat +0x44 prop class 0..7 */
float b3_props_mass_of(int instance);    /* FUN_0011A020's law, kg           */

/* The FUN_0010FBC0 object class this instance presents to FUN_00112E70's port.
 * Recovered answer for every shipped prop: 6 (handle type 5/6), which
 * DAT_0039AE50 row 6 makes non-crashable -- a cone can never wreck you.
 * B3_PROP_CRASH_KG=<kg> is a GLUE escape hatch: props at or above that
 * recovered mass instead present class 2 (a type-3 prop ENTITY), the row that
 * DOES crash a racecar.  Retail ships no such promotion, so it is OFF by
 * default; see INTEGRATION_NOTE. */
int   b3_props_object_class(int instance);

#endif /* BURNOUT3_PROPS_H */
