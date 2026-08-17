// The GAME'S OWN collision world, loaded from build/collision.bin
// (tools/extract_collision.py: the per-unit kd-tree poly soups inside
// streamed.dat's unit LOD blocks -- format and query chain in
// docs/RE_NOTES.md section 15, execution-verified against FUN_001aff70 /
// FUN_00123790 in tools/validate_gameplay.py's 'collision' section).
//
// Queries run in the harness's GL space (game coordinates with Z negated,
// the same reflection trackmesh_load applies); the loader mirrors each
// triangle and swaps v1/v2 so the game's one-sided ray test keeps its
// orientation.

#ifndef BURNOUT3_COLLISION_H
#define BURNOUT3_COLLISION_H

// Loads build/collision.bin. Returns triangle count, 0 on failure.
int b3_collision_load(const char* path);
int b3_collision_ready(void);

// Raw triangle access (GL space, post-mirror) so the harness can derive
// structures (e.g. the AI's 2D barrier grid) from the real collision data.
int b3_collision_tri_count(void);
// Fills any non-NULL out; returns 0 if i is out of range.
int b3_collision_tri_get(int i, float* v0, float* v1, float* v2,
                         float* normal, unsigned short* type, int* excluded);

// Downward ray probe, the harness equivalent of the game's under-body ray in
// FUN_001239C0 (frame pos, 30 units straight down, nearest parametric hit;
// ray core FUN_001b2230 with the game's exact epsilon constants).
// Casts from (x, y + 2, z) down to (x, y - 28, z).  On hit fills
// *out_height (surface y) and out_normal (unit), and returns the winning
// triangle's u16 surface type (>= 0); returns -1 on no ground.  This is the
// contract burnout3_vehicle_sim.h declares for its wheel pipeline.
// (Surface-type note: low byte 0x26 is the low-grip wreck surface the game
// stamps on crashed-car planes -- FUN_00123790 drops grip to 0.2 on it;
// the static track polys carry low bytes 0x01..0x25.)
int b3_ground_probe(float x, float y, float z,
                    float* out_height, float out_normal[3]);

// Same wheel/body ground query, additionally returning the streamed.dat unit
// that owned the winning triangle.  `out_unit` is 0xff when no ground was
// found.  Vehicle class-2 updates store this identity at body+0x216.
int b3_ground_probe_unit(float x, float y, float z,
                         float* out_height, float out_normal[3],
                         unsigned char* out_unit);

// Sphere sweep from 'from' to 'to' (float[3] each) against the collision
// triangles, for body-vs-wall contact.  ONE-SIDED like the game's own
// det>0 ray test: a triangle only blocks a sphere centre on its front
// (winding) side -- the data depends on this (fences are pairs of offset
// one-sided faces; the tall course-boundary walls are single one-sided
// quads passable from outside).  On hit fills hit_pos (the CONTACT POINT
// on the winning triangle), hit_normal (unit push direction, oriented
// toward the sphere centre) and returns 1; from==to degenerates to an
// overlap test at that position.
// Only "wall-like" triangles block (|normal.y| < wall_ny_max); pass 1.1 to
// sweep against everything.  Triangles the game's gather callback excludes
// (surface low byte 0x22/0x23, or type bit 0x1000) never block.
int b3_sweep_sphere(const float* from, const float* to, float radius,
                    float wall_ny_max, float* hit_pos, float* hit_normal);

// ---------------------------------------------------------------------------
// THE RACING GATHER'S TWO RUNTIME FILTERS -- FUN_0011BBE0 [C-disasm]
//
// FUN_0011BBE0 is the per-frame kd-walk callback FUN_0011BE50 hands to
// FUN_001AFF70 (`PUSH 0x11BBE0` @0x0011BD90), i.e. the RACING soup builder.
// Its whole predicate, with EDI = the candidate record (normal at +0x10,
// prim ptr at +0x60, surface u16 at prim+4; layout proved by the appender
// FUN_0010A8E0) and ESI = the vehicle:
//
//   low = type & 0xFF
//   0x0011BBFE  low == 0x23                                  -> skip
//   0x0011BC03  low == 0x22                                  -> skip
//   0x0011BC08  type & 0x1000                                -> skip
//   0x0011BC0D  0x15 <= low   \  the STRUCTURE band
//   0x0011BC15  low <= 0x20   /
//   0x0011BC2D     j = dot(normal, *(vec3*)(veh + 0xB0))     (FUN_00013C60)
//   0x0011BC32     j > 0.5   [0x003B1684]                    -> skip
//   0x0011BC43  -0.7 [0x0039B264] > normal.y                 -> skip
//   0x0011BC4B  FUN_0010A8E0 -- append to the soup
//
// veh+0xB0 is the car's OWN linear velocity vector (RE_NOTES 14: "+0xB0 is
// the true velocity vector, +0xBC its magnitude"), not a relative velocity,
// so (a) reads "drop the structure faces I am already separating from" --
// which is what stops a car snagging on the back plate of a paired one-sided
// armco.  (b) drops downward-facing faces outright, whatever their type.
//
// The first three tests are baked into the loader's `excl` byte; the two
// below need runtime state and are applied here.  `vel` is the car's world
// velocity in the SAME space as from/to (the harness GL space -- both
// filters are invariant under the loader's z mirror, see burnout3_collision.c),
// or NULL to run without (a).  `hit_type` receives the winning triangle's u16
// surface type.  b3_sweep_sphere == this with vel = NULL, hit_type = NULL.
// ---------------------------------------------------------------------------
#define B3_COL_GATHER_VDOT_MAX   0.5f    /* 0x003B1684 @0x0011BC32 */
#define B3_COL_GATHER_NY_MIN   (-0.7f)   /* 0x0039B264 @0x0011BC43 */
#define B3_COL_STRUCT_LO         0x15    /* 0x0011BC0D */
#define B3_COL_STRUCT_HI         0x20    /* 0x0011BC15 */

int b3_sweep_sphere_ex(const float* from, const float* to, float radius,
                       float wall_ny_max, const float* vel,
                       float* hit_pos, float* hit_normal,
                       unsigned short* hit_type);

typedef struct {
    float v0[3], v1[3], v2[3];
    float normal[3];
    unsigned short type;
} B3CollisionPoly;

int b3_collision_gather_walls(const float center[3], const float half[3],
                               const float* vel, float wall_ny_max,
                               B3CollisionPoly* out, int cap);

int b3_collision_gather(const float center[3], const float half[3],
                        B3CollisionPoly* out, int cap);

// Apply FUN_0011BBE0's runtime wall predicate to an already-frozen raw soup.
// This lets chassis contact and wheel rays share one per-frame gather.
int b3_collision_filter_walls(const B3CollisionPoly* input, int input_count,
                              const float center[3], const float half[3],
                              const float* vel, float wall_ny_max,
                              B3CollisionPoly* out, int cap);

int b3_collision_ray_polys(const B3CollisionPoly* polys, int count,
                           const float start[3], const float end[3],
                           float* hit_t, float normal[3]);

int b3_collision_ray_polys_game_space(const B3CollisionPoly* polys,
                                      int count, const float start[3],
                                      const float end[3], float* hit_t,
                                      float normal[3]);

// 1 when the surface type's low byte lies in FUN_0011BBE0's structure band
// (0x15..0x20) -- the only classification of soup surface types the retail
// code itself makes.  The harness uses it as its OBJECT/PROP class for
// FUN_00112E70's crash trigger (burnout3_td_rules.h section 10).
int b3_collision_is_structure(unsigned short type);

#endif // BURNOUT3_COLLISION_H
