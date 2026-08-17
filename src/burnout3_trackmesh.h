// Loader for real Burnout 3 track geometry extracted from static.dat.
//
// The mesh itself is genuine game data -- see tools/extract_track.py, which
// implements the Xbox track format documented by the Burnout Modding community
// (EdnessP's Noesis plugin). This file only reads the OBJ that tool produces.

#ifndef BURNOUT3_TRACKMESH_H
#define BURNOUT3_TRACKMESH_H

#define TRACKMESH_MAX_GROUPS 4096
#define TRACKMESH_NAME_LEN   64

// D3DRS_ALPHAREF / D3DRS_ALPHAFUNC for the whole world draw: GREATER, 64/255.
// The world setup FUN_00038D10 writes render state 0x3A := 0x204
// (D3DCMP_GREATER) at 0x0003901B and 0x3D := 0x40 at 0x00038FEE, and neither
// material apply touches them again -- so every cut-out in the world is tested
// at 0.25, not the 0.5 a renderer reaches for by reflex.               [C]
#define TRACKMESH_ALPHA_REF (64.0f / 255.0f)

// The per-track scene lighting and fog, parsed out of the MTL's `# scene_*`
// header, which tools/extract_track.py copies from the track's enviro.dat.
// See parse_enviro() there for the byte-level chain; the short version is that
// FUN_001888F0 copies enviro.dat's first 0xB0 bytes over the environment
// object at 0x0060E040 and builds the two identical 0x40-byte light records at
// 0x0060E0F0/0x0060E130 from it, and the world setup FUN_00038D10 turns those
// into vertex-shader constant c[0x61] and the D3DRS_FOG* states.
typedef struct {
    int valid;              // 0 when the MTL carried no scene block
    // Vertex-shader constant c[0x61] = -(light record +0x00), i.e. the vector
    // the class-1/2/7 vertex programs dot the vertex normal against. Stored in
    // the SAME space as TrackMesh.positions, i.e. with the loader's Z negation
    // already applied.
    float light_dir[3];
    // The scene light colour DAT_0060E0A0 = enviro.dat +0x60. Classes 1 and 7
    // hand light_rgb * material+0x04 to the pixel shader as combiner factor
    // C0.rgb; class 10 hands it over unscaled.
    float light_rgb[3];
    // D3DRS_FOGCOLOR as the hardware receives it -- the authored colour HALVED,
    // because FUN_00038D10 packs it with *127.5 (0x003B1E68) and not *255.
    float fog_rgb[3];
    float fog_start;        // D3DRS_FOGSTART = fog_far * 0.05
    float fog_end;          // D3DRS_FOGEND   = (fog_far-start)/div + start
    // The world vertex programs write `oFog = min(clip z, fog_far)`, so the
    // linear fog factor is floored at (fog_end-fog_far)/(fog_end-fog_start)
    // -- 0.75 on US_C3_V1/US_C1_V1, 0.80 on AS_C1_V1/AS_M1_V1, 0.70 on
    // AS_C2_V1, 0.90 on EU_C3_V1. This value is vertex-shader constant
    // c[120].z, loaded from the same fog record field as FOGSTART/FOGEND
    // (0x00038D8D -> 0x00038DD8, uploaded at 0x00038F59, the only writer of
    // slot 0x78 in the image). trackmesh_fog_begin() reproduces the clamp.
    float fog_far;
    int fog_enabled;
} TrackScene;

// The most animation frames any shipped material carries is 17 (`water`).
#define TRACKMESH_MAX_FRAMES 32

// TEXTURE-FRAME CYCLING -- the animated-material ticker FUN_0019B1E0's OTHER
// arm, and the one that drives every bulb-matrix message board in the game.
//
// The ticker is a single `if`: frame count (+0x11) < 2 takes the UV-SCROLL arm
// (below, on TrackMeshGroup), >= 2 takes this one. No material can do both,
// and this arm tests NO flag bit -- two or more frames is the whole entry
// condition, which is why EU_C4_V1's `Freeway_info_slow` animates on flags
// 0x000. Material +0x0C is not a pointer to one texture but to an ARRAY of
// texture-record pointers, one per frame, and the arm advances an index into
// it, swapping the BOUND TEXTURE where the scroll arm offsets the UVs.
//
//     if (hold(+0x18) + last(+0x1C) < T) {           // T = DAT_0060EA20
//         last = T;
//         if (flags & 0x100)  ...ping-pong on step(+0x13)...
//         else                index = (index + 1) % count;
//         nxt = (index + 1) % count;
//         if (nxt == 0) { hold = period(+0x14); label(+0x12) = 1; }
//         else { L = atol(name[nxt] + strlen(name[0]));
//                hold = (L - label) * period; label = L; }
//     }
//
// THE FRAME DURATIONS LIVE IN THE TEXTURE NAMES: the ticker atol()s the tail
// of the next frame's name past frame 0's name length, and that integer is a
// KEYFRAME TIME in periods, not an ordinal. `bk_warnsignb` .. `bk_warnsignb4`
// .. `bk_warnsignb5` .. `bk_warnsignb8` gives 4,5,8,9,12 at period 1 s, i.e. a
// board that reads for 3 s then blinks for 1 s, three messages per 12 s cycle.
// Uniformly numbered sets (`water`..`water17`, `Chgo_Flag`..`Chgo_Flag10`)
// collapse to a flat one-period flipbook, which is what makes the numbering
// look like nothing more than an ordinal.                                 [C]
typedef struct {
    int count;                  // +0x11, >= 2
    float period;               // +0x14
    float hold;                 // +0x18, live -- seconds the current frame runs
    float last;                 // +0x1C, live -- clock at the last swap
    int label;                  // +0x12, live -- current keyframe time
    int index;                  // +0x10, live -- current frame
    int step;                   // +0x13, live -- +1 / -1, ping-pong only
    int pingpong;               // flag bit 0x100; unset on every shipped material
    int frame_label[TRACKMESH_MAX_FRAMES];      // the atol'd keyframe times
    char frame_texture[TRACKMESH_MAX_FRAMES][256];
    unsigned frame_gl[TRACKMESH_MAX_FRAMES];    // filled by the renderer
} TrackMeshAnim;

// A run of consecutive triangles sharing one material (one usemtl span).
typedef struct {
    char material[TRACKMESH_NAME_LEN];  // usemtl name, "" if none
    char texture[256];                  // map_Kd path resolved from the MTL, "" if none
    int first_triangle;
    int triangle_count;
    // Road-decal layer: the MTL carries "# decal 1" for materials with the
    // game's material flag bit 0x400 (bk_roaddecals, bk_roaddecals2,
    // bk_decalshadows in C1_V1). The game draws these last and with
    // D3DRS_ZWRITEENABLE (render state 64) := 0 -- see the .c. Groups are
    // already sorted so every decal group comes last; the flag is here so a
    // renderer that can change state per group can also drop the depth write.
    int decal;
    // Transparent pass: the MTL carries "# alpha 1" for materials that sat at
    // or above a streamed unit's alpha cut (the PVS byte pvsRT+0x73), i.e. the
    // ones the game draws in FUN_001ADD60, a separate pass that runs after ALL
    // opaque world geometry rather than inside the unit. In C1_V1 that is the
    // coplanar facade detail layer -- bk_shoptop1/2, bk_ShopTop4Glass,
    // bk_meshfence, bk_roaddecals, the glass/reflective materials. The groups
    // are already emitted in that order by tools/extract_track.py; the flag is
    // here so a renderer with per-group state can also turn alpha blending on
    // (material flag +0x24 bit 0x10 -> render state 0x3C) for them.
    int alpha;

    // ---- material state, straight from the game's material record ----------
    // `cls` is the shader class at material +0x00; tools/extract_track.py's
    // "Shader classes" section carries the recovered colour equation for each.
    int cls;
    unsigned flags;        // the material flag word at +0x24

    // 1 once the MTL has been read for this group, i.e. `cls`, `flags`,
    // `alpha_test`, `alpha_blend` and `alpha_scalar` are the GAME'S values and
    // not defaults. Testing `(flags || cls)` instead is NOT a substitute and
    // was a real bug: US_C3_V1's tunnel interior walls are GL_tunnel_lightout
    // and GL_tunnel_light -- class 0 with flag word 0x0000 -- so that test
    // reads "no material info" and lets the texel heuristic decide. The
    // heuristic sees 97.7% of their texels at alpha == 0, alpha-tests 99% of
    // the wall away, and leaves you looking out of the tunnel at the mountains
    // (build/dump012.png: 470 triangles / 3821 m2 of interior wall, plus 56
    // triangles of GL_SHOPTOP2b elsewhere).
    int have_material;

    // D3DRS_ALPHABLENDENABLE (RS 0x3B) and D3DRS_ALPHATESTENABLE (RS 0x3C),
    // which the streamed material apply FUN_000393C0 computes as flags&0x001
    // (@0x00039B21) and flags&0x010 (@0x00039BD4).  NOTE THE ORDER -- the two
    // were swapped here until 2026-08-12; see tools/extract_track.py's
    // MAT_ALPHA_BLEND comment for the render-state identification.
    // THESE, AND ONLY THESE, DECIDE WHETHER A
    // WORLD TEXTURE'S ALPHA CHANNEL MEANS TRANSPARENCY.  On the class 0/1/7/10
    // materials the alpha channel is a specular/emissive mask and the surface
    // is opaque; a renderer that instead guesses "cut-out" from the fraction
    // of transparent texels alpha-tests away 95% of Silver Lake's dirt road
    // (GL_Droad1b: 55.7% of texels alpha==0, 4.3% above 0.5) and the sky shows
    // through it -- that is the "the road renders flat slate blue" bug.
    // The test itself is GREATER TRACKMESH_ALPHA_REF.
    int alpha_test;
    int alpha_blend;

    // Material +0x20 -- the ALPHA of combiner factor C0, and the opacity of
    // every blended world surface. Class 6 (all of the shadow and road-decal
    // sheets, the chevron boards, the fences) outputs tex.a * alpha_scalar;
    // classes 0 and 7 output alpha_scalar alone. US_C3_V1's tree/rock/bridge
    // shadows carry 0.6 and its road-decal sheets 0.5..0.6, so retail lays
    // them on at half strength -- blending them at full texture alpha paints
    // solid black tree shadows across the road (build/dump013.png). In GL,
    // fragment alpha = texture alpha * primary alpha under GL_MODULATE, so
    // this belongs in the vertex colour's w. Default 1.
    float alpha_scalar;

    // The animated-material UV scroll (flag bit 0x200 + membership of the
    // header+0x10 animated list). rate = material +0x14, step = +0x18, both 0
    // for a material that does not scroll. `phase` is the game's +0x1C, ticked
    // by trackmesh_tick(); `uv_offset` is what the material apply pushes into
    // vertex-shader constant 0x63, which the world vertex programs ADD to
    // v9.xy -- (1 - frac(phase), 0). US_C3_V1: `Arrows`, the wrong-way chevron
    // board, rate 1.2 step 0.125 = one bulb column every 1/9.6 s.
    // A scroll whose rate is <= 0 never moves IN RETAIL EITHER: the arm only
    // assigns when `step + phase <= rate * T`, and with the shipped step of 0
    // and phase 0 that is `0 <= rate*T`, false for every T > 0. AS_C1_V1 ships
    // one `bk_skytrainads` at rate -0.1 and another at +0.1; only the second
    // one moves. Groups with rate <= 0 therefore stay in the baked display
    // list, which is exactly the frozen phase-0 board.                    [C]
    float uv_scroll_rate;
    float uv_scroll_step;
    float uv_scroll_phase;
    float uv_offset[2];

    // Non-NULL for a material the ticker FRAME-CYCLES (frame count >= 2), and
    // then mutually exclusive with the uv_scroll fields above -- the two are
    // the two arms of one `if`. Heap-allocated, only for the handful of
    // animated groups; see TrackMeshAnim.
    TrackMeshAnim* anim;

    // Class 1/7/10 additive term: the pixel shader adds
    //     tex.a * (vertex.a if shine_gate) * pow(max(R.V,0), shine_power)
    //           * sceneLight.rgb * shine_strength
    // on top of the 2*tex*vertexColour base.  strength = material +0x04,
    // power = material +0x08, gate = flag bit 0x40 (which selects between two
    // otherwise byte-identical class-1 vertex programs whose only difference
    // is `MOV oD0.w, r1.z` vs `MUL oD0.w, v3.w, r1.z`).  Zero strength means
    // "no shine pass for this group".
    float shine_strength;
    float shine_power;
    int shine_gate;

    // 0 when the material's shader class has NO D3DCOLOR register in its
    // vertex declaration -- classes 8 (position+texcoord, 0x003875E8) and 9
    // (position+normal+texcoord, 0x003875D4), the foliage/prop/cone families.
    // The game never reads the stored vertex colour for those; their diffuse
    // is computed by the vertex program. Modulating them by TrackMesh.colors
    // anyway buries the trees in shadow the game does not apply. Default 1.
    int use_vertex_color;

    // Filled in by the renderer via trackmesh_set_group_texture() so the shine
    // pass can bind the same GL texture the base pass uses. 0 = none.
    unsigned gl_texture;
} TrackMeshGroup;

typedef struct {
    float* positions;     // 3 floats per vertex
    float* uvs;           // 2 floats per vertex, may be NULL
    // 4 floats per vertex, may be NULL when the OBJ predates vertex colours.
    // rgb is the game's D3DCOLOR diffuse ALREADY DOUBLED and clamped, because
    // every world pixel shader carries PS_COMBINEROUTPUT_SHIFTLEFT_1 on its
    // stage-0 RGB output (rgb = 2 * tex * colour) and the stored colour is
    // half range -- 128 is white, and 128/255*2 = 1.004, so the clamp is
    // exact to 0.4%.  a is the raw vertex alpha (0 or 1), the specular gate.
    float* colors;
    float* normals;       // 3 floats per vertex, may be NULL
    unsigned* indices;    // 3 per triangle
    int vertex_count;
    int triangle_count;
    float min[3], max[3]; // bounding box, in game units

    TrackMeshGroup groups[TRACKMESH_MAX_GROUPS];
    int group_count;      // 0 means "no material info; draw everything flat"

    // Per-track scene lighting/fog out of the MTL's `# scene_*` header.
    TrackScene scene;
    // The game's global clock DAT_0060EA20 as the animated-material ticker
    // FUN_0019B1E0 reads it: seconds, accumulated by trackmesh_tick().
    float anim_time;
    // How many groups the ticker touches -- a moving UV scroll (rate > 0) OR a
    // frame cycle. These are exactly the groups that cannot be baked into a
    // display list, because their texture coordinates or their texture BINDING
    // change every frame; trackmesh_draw_scroll() draws them all.
    int anim_groups;

    // Scratch colour buffer for trackmesh_draw_shine(), grown on demand.
    float* shine_scratch;
    int shine_scratch_verts;
} TrackMesh;

// Load an OBJ produced by tools/extract_track.py. Returns 0 on success.
// Material texture paths are resolved via the mtllib file, relative to the
// OBJ's own directory.
//
// SIDE EFFECT: the first call made with a GL context current turns on backface
// culling with clockwise front faces, because the game's world geometry is
// single-sided and drawing it double-sided shows the back of walls and
// backdrops through the level. See the comment on trackmesh_gl_single_sided()
// in the .c; B3_TRACK_NOCULL=1 disables it.
//
// The returned groups are in the game's own frame order -- streamed opaque,
// static backdrop groups, water, streamed alpha, chevron group (FUN_001AE340;
// see tools/extract_track.py) -- with the decal groups (road markings and blob
// shadows) then moved behind everything else, so a renderer that walks groups
// front-to-back in array order reproduces it. B3_TRACK_NODECALSORT=1 keeps the
// file order.
int trackmesh_load(TrackMesh* m, const char* path);
void trackmesh_free(TrackMesh* m);

// Hand the shine pass the GL texture the renderer already uploaded for this
// group (it needs the same image, for its ALPHA channel).
void trackmesh_set_group_texture(TrackMesh* m, int group, unsigned gl_texture);

// ---- per-group render state -------------------------------------------------
// Issue the GL state one world group is drawn with: depth mask, blend,
// alpha test and the texture bind. This is the harness spelling of what the
// streamed material apply FUN_000393C0 queues per material -- see the .c for
// the state-by-state citations. `cutout_fallback` is the caller's texel
// heuristic, used ONLY for groups the OBJ carries no material record for.
// Safe to call while compiling a display list.
void trackmesh_group_state(const TrackMesh* m, int group, unsigned gl_texture,
                           int cutout_fallback);

// The vertex colour to hand glColor4fv for vertex `v` of group `group`:
// rgb = the doubled baked diffuse (or white for the classes with no D3DCOLOR
// register), w = the material's alpha scalar. Writes 4 floats into `out`.
void trackmesh_group_vertex_color(const TrackMesh* m, int group, unsigned v,
                                  float out[4]);

// ---- animated materials ------------------------------------------------------
// Advance the animated-material clock by `dt` seconds and re-run FUN_0019B1E0
// -- BOTH arms -- for every animated group: the UV-scroll stepper for the
// scrolling ones and the keyframe-timed texture-frame cycle for the rest. Call
// once per rendered frame, before the world draw. Returns the group count.
int trackmesh_tick(TrackMesh* m, float dt);

// 1 for a group the ticker animates, i.e. one that MUST be left out of a baked
// display list: either its UVs move (scroll) or its texture binding does
// (frame cycle). The renderer's bake loop skips exactly these.
int trackmesh_group_animated(const TrackMesh* m, int group);

// The GL texture to bind for `group` this frame -- the current animation frame
// for a frame-cycling group, its single texture otherwise.
unsigned trackmesh_group_texture(const TrackMesh* m, int group);

// Resolve the per-frame textures of every frame-cycling group through the
// renderer's own image loader. `fn` gets a path exactly as the MTL spelled it
// (already prefixed with the OBJ's directory) and returns a GL texture name,
// or 0 if it cannot load it -- a frame that fails to load falls back to the
// group's base texture, so a track whose extra frames were never extracted
// simply keeps standing still instead of drawing untextured. Returns the
// number of distinct frames resolved.
typedef unsigned (*TrackMeshTexLoader)(const char* path, void* user);
int trackmesh_load_frame_textures(TrackMesh* m, TrackMeshTexLoader fn,
                                  void* user);

// Draw the scrolling groups with the current UV offset. They must be EXCLUDED
// from any baked display list, because their texture coordinates change every
// frame. Returns the number of triangles drawn.
int trackmesh_draw_scroll(const TrackMesh* m);

// ---- fog ---------------------------------------------------------------------
// Turn the recovered world fog on/off around the track draw. The world setup
// FUN_00038D10 enables D3DRS_FOGENABLE and the world teardown FUN_00039140
// (@0x000391C5) disables it again, so retail fogs the WORLD ONLY -- not cars,
// traffic or HUD. B3_TRACK_NOFOG=1 disables it here.
//
// fog_begin also binds a one-line GLSL VERTEX program (no fragment program, so
// the whole fixed-function fragment stage including the linear fog table stays
// in charge) whose only job is `gl_FogFragCoord = min(|z_eye|, fog_far)` --
// the microcode's `MIN oFog, r12.z, c[120].z`, which fixed-function GL cannot
// express. fog_end unbinds it. B3_TRACK_NOFOGFLOOR=1, or a GL without the 2.0
// entry points, falls back to the old unclamped fog. See the .c for why no
// arrangement of GL_FOG_START/END/COLOR reproduces the floor.
void trackmesh_fog_begin(const TrackMesh* m);
void trackmesh_fog_end(void);

// Draw the class-1/7/10 additive specular pass for one frame. `eye` is the
// camera position and `light_dir` the game's vertex-shader constant c[0x61] --
// both in the SAME space as TrackMesh.positions, i.e. after the loader's Z
// negation. Pass light_dir = NULL to use TrackMesh.scene.light_dir, which is
// the track's own enviro.dat value; the pass then also tints the additive term
// by scene.light_rgb, which is what the material apply hands the pixel shader
// as combiner factor C0.rgb. Returns the number
// of triangles drawn (0 when the mesh carries no shine groups, no vertex
// colours, or B3_TRACK_NOSHINE is set).
//
// Call it after the opaque world and before the transparent/car passes; it
// leaves GL blending, depth-mask and texture-env state as it found them.
int trackmesh_draw_shine(TrackMesh* m, const float eye[3],
                         const float light_dir[3]);

#endif // BURNOUT3_TRACKMESH_H
