#include "burnout3_trackmesh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <SDL2/SDL_video.h>
#ifdef __ANDROID__
/* ANDROID PORT: resolve GL 2.0 through gl4es, not the raw GLES2 driver --
 * see the same block in burnout3_postfx.c for why. */
#include <gl4esinit.h>
#define SDL_GL_GetProcAddress gl4es_GetProcAddress
#endif

// World headroom curve -- TUNED (user-authorized deviation 2026-08-13).
// Positive lifts, negative compresses, 0 is the identity. See
// trackmesh_group_vertex_color() for the shape, the reason and the reference
// measurements this was judged against.
#ifndef TRACKMESH_LIFT_G
#define TRACKMESH_LIFT_G 0.0f
#endif

// Minimal OBJ reader: only the subset tools/extract_track.py emits (v, vt, f
// with v/vt indices, triangles only, usemtl spans, one mtllib). Deliberately
// not a general OBJ parser.

// Directory part of `path` (including trailing slash) into `dir`.
static void path_dir(const char* path, char* dir, size_t cap) {
    const char* slash = strrchr(path, '/');
    if (!slash) { dir[0] = '\0'; return; }
    size_t n = (size_t)(slash - path) + 1;
    if (n >= cap) n = cap - 1;
    memcpy(dir, path, n);
    dir[n] = '\0';
}

// Fill in the per-group material state from the MTL that tools/extract_track.py
// writes: the map_Kd path plus its "# <key> <value>" annotation lines, each of
// which records one field of the game's own 0x28-byte material record. Texture
// paths in the MTL are relative to the MTL's own directory.
//
// The MTL's `newmtl` identity is (texture, shader class, flag word), not the
// texture alone -- US_C3_V1 ships GL_road5, GL_road7 and GL_road7grass as both
// an unshaded class-0 record and a specular class-1 one, and several backdrop
// cards appear with and without D3DCULL_NONE. Distinct records get a
// "<texture>.mNN" suffix; nothing here has to know that, it just matches the
// usemtl name it was given.
static void resolve_materials(TrackMesh* m, const char* obj_path, const char* mtl_name) {
    if (!mtl_name[0] || m->group_count == 0) return;

    char dir[256], mtl_path[512];
    path_dir(obj_path, dir, sizeof(dir));
    snprintf(mtl_path, sizeof(mtl_path), "%s%s", dir, mtl_name);

    FILE* f = fopen(mtl_path, "r");
    if (!f) return;

    char line[512], cur[TRACKMESH_NAME_LEN] = "";
    while (fgets(line, sizeof(line), f)) {
        char arg[256];
        int ival;
        unsigned uval;
        float s, p, q;
        int gate;
        int ai, af, as, ap;
        // The scene block sits in the MTL's header comment, before any
        // `newmtl`, and describes the whole track (see TrackScene).
        if (sscanf(line, "# scene_light_dir %f %f %f", &s, &p, &q) == 3) {
            // Same reflection the positions get: the game's vector is in game
            // space and TrackMesh lives in the mirrored one.
            m->scene.light_dir[0] = s;
            m->scene.light_dir[1] = p;
            m->scene.light_dir[2] = -q;
            m->scene.valid = 1;
            continue;
        }
        if (sscanf(line, "# scene_light_rgb %f %f %f", &s, &p, &q) == 3) {
            m->scene.light_rgb[0] = s;
            m->scene.light_rgb[1] = p;
            m->scene.light_rgb[2] = q;
            continue;
        }
        if (sscanf(line, "# scene_fog_rgb %f %f %f", &s, &p, &q) == 3) {
            m->scene.fog_rgb[0] = s;
            m->scene.fog_rgb[1] = p;
            m->scene.fog_rgb[2] = q;
            continue;
        }
        if (sscanf(line, "# scene_fog_range %f %f %f", &s, &p, &q) == 3) {
            m->scene.fog_start = s;
            m->scene.fog_end = p;
            m->scene.fog_far = q;
            m->scene.fog_enabled = (p > s);
            continue;
        }
        if (sscanf(line, "newmtl %63s", cur) == 1) continue;
        if (!cur[0]) continue;

        // Everything below applies to every group using the current material.
        for (int g = 0; g < m->group_count; g++) {
            TrackMeshGroup* grp = &m->groups[g];
            if (strcmp(grp->material, cur) != 0) continue;
            if (sscanf(line, "map_Kd %255s", arg) == 1)
                snprintf(grp->texture, sizeof(grp->texture), "%s%s", dir, arg);
            else if (sscanf(line, "# class %d", &ival) == 1) {
                grp->cls = ival;
                // Every material the extractor writes carries a "# class"
                // line, so this is the presence flag for the whole record.
                grp->have_material = 1;
            }
            else if (sscanf(line, "# flags 0x%x", &uval) == 1)
                grp->flags = uval;
            else if (strncmp(line, "# alpha_test 1", 14) == 0)
                grp->alpha_test = 1;
            else if (strncmp(line, "# alpha_blend 1", 15) == 0)
                grp->alpha_blend = 1;
            else if (strncmp(line, "# vcolor 0", 10) == 0)
                grp->use_vertex_color = 0;
            else if (sscanf(line, "# shine %f %f %d", &s, &p, &gate) == 3) {
                grp->shine_strength = s;
                grp->shine_power = p;
                grp->shine_gate = gate;
            }
            else if (sscanf(line, "# alpha_scalar %f", &s) == 1)
                grp->alpha_scalar = s;
            else if (sscanf(line, "# uv_scroll %f %f", &s, &p) == 2) {
                grp->uv_scroll_rate = s;
                grp->uv_scroll_step = p;
            }
            // FUN_0019B1E0's frame-cycle arm: one `# anim_frames` header, then
            // one `# anim_frame <k> <label> <path>` per frame. Every field is
            // straight out of the material record -- see TrackMeshAnim for the
            // recovered algorithm and why <label> is a keyframe TIME.
            else if (sscanf(line, "# anim_frames %d %f %f %d %d %d %d",
                            &ival, &s, &p, &ai, &af, &as, &ap) == 7) {
                if (!grp->anim)
                    grp->anim = (TrackMeshAnim*)calloc(1, sizeof(TrackMeshAnim));
                if (grp->anim) {
                    TrackMeshAnim* a = grp->anim;
                    a->count = ival > TRACKMESH_MAX_FRAMES
                             ? TRACKMESH_MAX_FRAMES : ival;
                    a->period = s;
                    a->hold = p;        // +0x18, the value the file ships
                    a->last = 0.0f;     // +0x1C, zeroed at load
                    a->label = ai;      // +0x12
                    a->index = af;      // +0x10
                    a->step = as;       // +0x13
                    a->pingpong = ap;   // flag bit 0x100
                }
            }
            else if (sscanf(line, "# anim_frame %d %d %255s",
                            &ival, &ai, arg) == 3) {
                if (grp->anim && ival >= 0 && ival < grp->anim->count) {
                    grp->anim->frame_label[ival] = ai;
                    snprintf(grp->anim->frame_texture[ival],
                             sizeof(grp->anim->frame_texture[ival]),
                             "%s%s", dir, arg);
                }
            }
            else if (strncmp(line, "# decal 1", 9) == 0)
                grp->decal = 1;
            else if (strncmp(line, "# alpha 1", 9) == 0)
                grp->alpha = 1;
        }
    }
    fclose(f);
}

// Move every decal group behind the rest of the world, keeping the relative
// order within each half.
//
// THE MECHANISM (see tools/extract_track.py's docstring for the byte-level
// citations). Road markings, arrows and blob shadows are coplanar overlays
// resting a hair above the road -- a median of +0.0114 world units over the
// surface underneath in C1_V1. The game has no depth bias to lean on: the
// NV2A polygon-offset render states 76..81 are written once, to 4/0/0/0/0/0,
// at 0x001BFC07..0x001BFCC4, and never touched again. What keeps the overlays
// on top is purely that they are drawn AFTER the road, under the ambient
// LESSEQUAL depth test (render state 57 := 0x203, 0x001BFAEC), with depth
// WRITES off: the streamed-world material apply FUN_000393C0 computes
// D3DRS_ZWRITEENABLE (render state 64, shadow slot 0x0075D5A0) as
// !(flags & 0x400) at 0x00039AF5..0x00039B1B, and 0x400 is the decal bit.
//
// "After the road" is not the file order. A streamed unit's material table is
// sorted, and a submesh's place in the frame is its material SLOT: the opaque
// slots [0, pvs+0x73) draw in FUN_001AD7A0 and the slots above the cut draw
// later, in the separate transparent pass FUN_001ADD60, after ALL opaque
// geometry of every visible unit. extract_track.py now emits both in that
// order, which fixes the 2392 of 2465 C1_V1 decal triangles that used to be
// emitted before the road they sit on.
//
// What is left over is the cross-unit case: a decal of one streamed unit lying
// on the road of another. The game gets those right because its passes span
// every visible unit at once; the harness bakes the whole track into a single
// display list with no per-cell visibility, so that global consequence is
// applied here instead.
//
// Without it the road ties with the marking on top of it once the depth
// buffer can no longer resolve a hundredth of a unit -- at distance, or under
// this harness's 0.1/5000 near/far -- and, drawn later, wins: the markings
// shutter in and out as the camera creeps forward.
//
// Depth writes are handled per group by trackmesh_group_state()'s
// glDepthMask(decal ? GL_FALSE : GL_TRUE) -- the exact GL spelling of
// D3DRS_ZWRITEENABLE := 0 -- and those calls are recorded into the display
// list, so baking the world costs nothing here.
//
// THE RUNTIME LAW OF THE LAYER: there is none beyond this.               [C]
//
// The layer is the single biggest darkener the harness applies to the road
// (it can cost 1.9x on a patch of Silver Lake asphalt), so it was audited end
// to end for a distance fade, a per-batch alpha ramp or an LOD/range cutoff
// that would let retail draw it fainter or not at all. There is no such
// thing anywhere in the streamed world path:
//
//   * OPACITY IS A PER-MATERIAL CONSTANT. FUN_000393C0's class-6 arm builds
//     combiner factor C0 = (0, 0, 0, material+0x20) on the stack and pushes
//     it once per material:
//        000396b8  XORPS XMM0,XMM0
//        000396c1  MOVSS [ESP+0x40],XMM0     ; C0.r = 0
//        000396c7  MOVSS [ESP+0x44],XMM0     ; C0.g = 0
//        000396cd  MOVSS [ESP+0x48],XMM0     ; C0.b = 0
//        000396d3  MOVSS XMM0,[ESI + 0x20]   ; material +0x20
//        000396d9  MOVSS [ESP+0x4C],XMM0     ; C0.a = alpha scalar
//        000396f3  LEA EAX,[ESP+0x40] / PUSH / JMP -> CALL 0x0034e9a0
//     FUN_0034E9A0 clamps the float4 to [0,1] (`0x003F7BE0` = 1.0) and packs
//     it to a D3DCOLOR with a plain *255 per lane (`0x003F7BF0..FC` = 255.0,
//     four times) into NV097_SET_COMBINER_FACTOR0 (method dword 0x00040A60).
//     Nothing between that upload and the draw touches it. So 0.6 in the
//     file is 153/255 on screen, at every distance, for every batch.
//   * THE VERTEX PROGRAM HAS NO FADE. Classes 0/3/6/10 share the 9-instruction
//     program at 0x003E8828: UV scroll, `MOV oD0, v3`, the four oPos DP4s,
//     `MIN oFog, r12.z, c[120].z` and the screen-space epilogue. No view
//     vector, no length, no ramp -- and class 6's output alpha is
//     `T0.a * C0.a`, so oD0.w never reaches the blend at all.
//   * THE ONLY PER-SUBMESH GATE IS A FRUSTUM TEST. FUN_001AD510 walks a
//     material slot's submesh chain (`pvs+0x12D + unitslot*0xA9 + slot`, next
//     at submesh+0x8C, stride 0x90) and draws each link if the caller's
//     "whole unit inside" flag is set OR `FUN_001B23F0(&DAT_004D67F0, submesh)`
//     is non-zero. FUN_001B23F0 is a plain bounding-box-vs-plane-set test: it
//     dots the submesh's eight corner float4s (submesh+0x00..0x7F, right in
//     front of the fmt/count/ptr triple at +0x80/+0x84/+0x88) against two
//     four-plane sets with MOVMSKPS and returns 0 out / 1 straddling / 2
//     inside. Visible is visible; there is no distance term in it.
//   * THE UNIT LOD IS A GEOMETRY SWAP, NOT A DECAL SWITCH. FUN_0019D100 takes
//     the chunk's signed unit offset and at 0x0019D23B..0x0019D249 computes
//     |offset| and compares it with 4: units within +/-4 of the camera's cell
//     draw the blockA model, units 5..8 away draw the blockB one (the
//     lower-poly LOD block; extract_track.py extracts blockA only). The near
//     road -- everything these sheets lie on -- is always blockA.
//
// So retail lays the sheets down wherever they are, at full strength, out to
// the edge of the frustum, exactly as this loader does. The apparent "band
// our render has and retail's frame does not" on the Silver Lake pair is a
// FRAME-MATCH artefact: only 5.1% of the track's road area carries a
// Tree_shadow1 sheet (9.9% carries any decal at all), our f1100 happens to sit
// inside one and the retail capture does not. Frames 1120..1250 of the same
// stretch measure 58..62 on the same box against f1100's 30.
static void trackmesh_decals_last(TrackMesh* m) {
    if (m->group_count <= 1 || getenv("B3_TRACK_NODECALSORT")) return;

    int ndecal = 0, covered = 0;
    for (int g = 0; g < m->group_count; g++) {
        ndecal += m->groups[g].decal ? 1 : 0;
        covered += m->groups[g].triangle_count;
    }
    // Nothing to do, or the groups do not tile the index array (then moving
    // them would drop the triangles no group owns).
    if (ndecal == 0 || ndecal == m->group_count || covered != m->triangle_count)
        return;
    // Already sorted? Leave the arrays alone.
    {
        int seen_decal = 0, sorted = 1;
        for (int g = 0; g < m->group_count; g++) {
            if (m->groups[g].decal) seen_decal = 1;
            else if (seen_decal) { sorted = 0; break; }
        }
        if (sorted) return;
    }

    unsigned* idx = malloc((size_t)m->triangle_count * 3 * sizeof(unsigned));
    TrackMeshGroup* grp = malloc((size_t)m->group_count * sizeof(TrackMeshGroup));
    if (!idx || !grp) { free(idx); free(grp); return; }

    int t = 0, n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int g = 0; g < m->group_count; g++) {
            const TrackMeshGroup* s = &m->groups[g];
            if ((s->decal != 0) != pass) continue;
            memcpy(idx + (size_t)t * 3,
                   m->indices + (size_t)s->first_triangle * 3,
                   (size_t)s->triangle_count * 3 * sizeof(unsigned));
            grp[n] = *s;
            grp[n].first_triangle = t;
            n++;
            t += s->triangle_count;
        }
    }
    free(m->indices);
    m->indices = idx;
    memcpy(m->groups, grp, (size_t)m->group_count * sizeof(TrackMeshGroup));
    free(grp);
}

// The game's world geometry is SINGLE-SIDED: it draws with backface culling
// on and only turns it off per material, for the fences/foliage/signs/clutter
// that carry material flag +0x24 bit 0x20 (D3DRS_CULLMODE := D3DCULL_NONE --
// FUN_0003A3C0 @0x0003A674; see tools/extract_track.py for the full citation).
// Drawing that data double-sided shows the BACK of walls, facades and
// backdrops: the Bangkok underpass at route progress ~0.48 was sealed off by
// the reverse side of a bk_downtown_bd facade standing across the road.
//
// So culling has to be on. Front faces are CLOCKWISE here, for two reasons
// that agree: trackmesh_load mirrors the world (negate Z, below), which
// inverts triangle facing; and the harness's own screen-space geometry (HUD
// quads) is wound the same way. GL defaults to GL_CCW, hence the glFrontFace.
// The two-sided materials need no special case -- extract_track.py already
// emitted a reverse-wound duplicate of every one of their triangles.
//
// Everything the harness draws is wound that way -- track, cars, traffic and
// HUD all verified on screen. The only geometry that is not is the untextured
// box/quad *fallbacks* in burnout3_full.c, which only appear when a car or
// track mesh failed to load; a face of those may now be culled.
// B3_TRACK_NOCULL=1 restores the old double-sided behaviour if some other
// draw path turns out to need it.
static void trackmesh_gl_single_sided(void) {
    static int done = 0;
    if (done) return;
    if (getenv("B3_TRACK_NOCULL")) { done = 1; return; }
    if (!SDL_GL_GetCurrentContext()) return;   // no context yet (or headless)
    done = 1;
    glFrontFace(GL_CW);
    glEnable(GL_CULL_FACE);
}

int trackmesh_load(TrackMesh* m, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;

    memset(m, 0, sizeof(*m));
    int vcap = 1 << 14, tcap = 1 << 14;
    m->positions = malloc((size_t)vcap * 3 * sizeof(float));
    m->uvs = malloc((size_t)vcap * 2 * sizeof(float));
    m->colors = malloc((size_t)vcap * 4 * sizeof(float));
    m->normals = malloc((size_t)vcap * 3 * sizeof(float));
    m->indices = malloc((size_t)tcap * 3 * sizeof(unsigned));
    if (!m->positions || !m->uvs || !m->colors || !m->normals || !m->indices) {
        fclose(f); trackmesh_free(m); return -1;
    }

    // Per-corner vt/vn indices, kept only so the expansion pass below can
    // rebuild the mesh if the OBJ does not use one index per vertex.
    unsigned* corner_t = malloc((size_t)tcap * 3 * sizeof(unsigned));
    unsigned* corner_n = malloc((size_t)tcap * 3 * sizeof(unsigned));
    int split_channels = 0;

    int nuv = 0, nnrm = 0, have_color = 0, have_normal = 0;
    char mtl_name[256] = "";
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            if (m->vertex_count >= vcap) {
                vcap *= 2;
                m->positions = realloc(m->positions, (size_t)vcap * 3 * sizeof(float));
                m->uvs = realloc(m->uvs, (size_t)vcap * 2 * sizeof(float));
                m->colors = realloc(m->colors, (size_t)vcap * 4 * sizeof(float));
                m->normals = realloc(m->normals, (size_t)vcap * 3 * sizeof(float));
                if (!m->positions || !m->uvs || !m->colors || !m->normals) {
                    fclose(f); free(corner_t); free(corner_n);
                    trackmesh_free(m); return -1;
                }
            }
            float* p = m->positions + (size_t)m->vertex_count * 3;
            float* c = m->colors + (size_t)m->vertex_count * 4;
            // "v x y z [r g b]" -- the optional trailing triple is the game's
            // D3DCOLOR diffuse at vertex +0x10 (the widely used OBJ vertex
            // colour extension). It is HALF RANGE: 128/255 is white, because
            // every world pixel shader doubles it (SHIFTLEFT_1 on the stage-0
            // RGB output, 0x000100C0 in all six D3DPIXELSHADERDEFs). Double it
            // here, once, so the renderer can hand it straight to glColor.
            float r = 0.5f, g = 0.5f, b = 0.5f;
            int n = sscanf(line + 2, "%f %f %f %f %f %f", p, p + 1, p + 2,
                           &r, &g, &b);
            if (n >= 3) {
                // The game's data is D3D left-handed; GL is right-handed.
                // Rendering LH data through a RH camera mirrors the image
                // (billboard text read backwards). One uniform reflection
                // (negate Z) applied to ALL world data -- this loader serves
                // both track and car meshes, and burnout3_full.c applies the
                // same flip to the path/collision arrays -- makes the GL
                // output match the original game's exactly.
                p[2] = -p[2];
                if (n >= 6) have_color = 1;
                c[0] = r * 2.0f > 1.0f ? 1.0f : r * 2.0f;
                c[1] = g * 2.0f > 1.0f ? 1.0f : g * 2.0f;
                c[2] = b * 2.0f > 1.0f ? 1.0f : b * 2.0f;
                c[3] = 1.0f;                 // set from the vt line below
                m->vertex_count++;
            }
        } else if (line[0] == 'v' && line[1] == 't') {
            if (nuv < vcap) {
                float* t = m->uvs + (size_t)nuv * 2;
                // "vt u v [a]" -- the third component carries the vertex
                // ALPHA, which is the class-1 specular gate (0 or 1).
                float a = 1.0f;
                if (sscanf(line + 3, "%f %f %f", t, t + 1, &a) >= 2) {
                    if (nuv < m->vertex_count) m->colors[(size_t)nuv * 4 + 3] = a;
                    nuv++;
                }
            }
        } else if (line[0] == 'v' && line[1] == 'n') {
            if (nnrm < vcap) {
                float* nv = m->normals + (size_t)nnrm * 3;
                if (sscanf(line + 3, "%f %f %f", nv, nv + 1, nv + 2) == 3) {
                    nv[2] = -nv[2];   // same reflection as the positions
                    nnrm++;
                    have_normal = 1;
                }
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            unsigned a, b, c;
            unsigned at = 0, bt = 0, ct = 0, an = 0, bn = 0, cn = 0;
            // Emitted as "f v/vt v/vt v/vt" (track) or "f v/vt/vn ..." (cars);
            // accept bare "f v v v" too.
            //
            // TrackMesh is a FLAT vertex model: positions, uvs, colours and
            // normals are four parallel arrays that `indices` indexes with a
            // single number. That only holds if a face's vt and vn indices
            // equal its v index -- otherwise a corner's normal would be read
            // from the wrong vertex, and on a car body (where the SH specular
            // is evaluated along reflect(-V,N)) that shows up as chaotic
            // marbled streaking rather than a smooth sheen.
            //
            // Both producers do satisfy it: tools/extract_track.py and
            // tools/extract_bgv.py write one v, one vt and one vn per vertex
            // in the same order and emit "f a/a/a b/b/b c/c/c". So the fast
            // path below is exact for the data this harness ships. But that
            // is an invariant of the WRITERS, not of the format, so it is
            // CHECKED rather than assumed: any face whose vt or vn index
            // differs from its v index sets `split_channels`, and the
            // expansion pass after the read loop then rebuilds the mesh with
            // one vertex per distinct (v, vt, vn) triple. A general OBJ
            // therefore loads correctly instead of silently scrambling.
            if (sscanf(line + 2, "%u/%u/%u %u/%u/%u %u/%u/%u",
                       &a, &at, &an, &b, &bt, &bn, &c, &ct, &cn) == 9) {
                if (at != a || bt != b || ct != c ||
                    an != a || bn != b || cn != c)
                    split_channels = 1;
            } else if (sscanf(line + 2, "%u/%u %u/%u %u/%u",
                              &a, &at, &b, &bt, &c, &ct) == 6) {
                an = a; bn = b; cn = c;
                if (at != a || bt != b || ct != c) split_channels = 1;
            } else if (sscanf(line + 2, "%u//%u %u//%u %u//%u",
                              &a, &an, &b, &bn, &c, &cn) == 6) {
                at = a; bt = b; ct = c;
                if (an != a || bn != b || cn != c) split_channels = 1;
            } else if (sscanf(line + 2, "%u %u %u", &a, &b, &c) == 3) {
                at = a; bt = b; ct = c;
                an = a; bn = b; cn = c;
            } else {
                continue;
            }
            if (m->triangle_count >= tcap) {
                tcap *= 2;
                m->indices = realloc(m->indices, (size_t)tcap * 3 * sizeof(unsigned));
                if (!m->indices) {
                    fclose(f); free(corner_t); free(corner_n);
                    trackmesh_free(m); return -1;
                }
                if (corner_t && corner_n) {
                    unsigned* rt = realloc(corner_t, (size_t)tcap * 3 * sizeof(unsigned));
                    unsigned* rn = realloc(corner_n, (size_t)tcap * 3 * sizeof(unsigned));
                    if (rt) corner_t = rt;
                    if (rn) corner_n = rn;
                    // Out of memory for the side channels only: drop the
                    // expansion capability, never the mesh.
                    if (!rt || !rn) {
                        free(corner_t); free(corner_n);
                        corner_t = corner_n = NULL;
                        split_channels = 0;
                    }
                }
            }
            unsigned* i = m->indices + (size_t)m->triangle_count * 3;
            i[0] = a - 1; i[1] = b - 1; i[2] = c - 1;   // OBJ is 1-based
            if (corner_t && corner_n) {
                unsigned* it = corner_t + (size_t)m->triangle_count * 3;
                unsigned* in = corner_n + (size_t)m->triangle_count * 3;
                it[0] = at - 1; it[1] = bt - 1; it[2] = ct - 1;
                in[0] = an - 1; in[1] = bn - 1; in[2] = cn - 1;
            }
            m->triangle_count++;
        } else if (strncmp(line, "usemtl ", 7) == 0) {
            char name[TRACKMESH_NAME_LEN];
            if (sscanf(line + 7, "%63s", name) != 1) continue;
            // Extend the current group if the material repeats back-to-back.
            if (m->group_count > 0 &&
                strcmp(m->groups[m->group_count - 1].material, name) == 0 &&
                m->groups[m->group_count - 1].first_triangle +
                m->groups[m->group_count - 1].triangle_count == m->triangle_count)
                continue;
            if (m->group_count < TRACKMESH_MAX_GROUPS) {
                TrackMeshGroup* g = &m->groups[m->group_count++];
                memset(g, 0, sizeof(*g));
                strncpy(g->material, name, sizeof(g->material) - 1);
                g->first_triangle = m->triangle_count;
                g->use_vertex_color = 1;   // cleared by the MTL's "# vcolor 0"
                g->alpha_scalar = 1.0f;    // set by the MTL's "# alpha_scalar"
            }
        } else if (strncmp(line, "mtllib ", 7) == 0) {
            sscanf(line + 7, "%255s", mtl_name);
        }

        // Keep the open group's span current as faces stream in.
        if (m->group_count > 0) {
            TrackMeshGroup* g = &m->groups[m->group_count - 1];
            g->triangle_count = m->triangle_count - g->first_triangle;
        }
    }
    fclose(f);

    if (m->vertex_count == 0 || m->triangle_count == 0) {
        free(corner_t); free(corner_n); trackmesh_free(m); return -1;
    }

    // ---- channel expansion -------------------------------------------------
    // Only runs for an OBJ whose faces index vt/vn separately from v (see the
    // face parser). Neither of this repo's extractors produces one, so on the
    // shipped data this whole block is skipped and the mesh is bit-identical
    // to what the loader has always produced. It exists so that "the normals
    // are the wrong normals" can never be a silent failure mode again.
    if (split_channels && corner_t && corner_n) {
        int ncorner = m->triangle_count * 3;
        int cap = 16;
        while (cap < ncorner * 2) cap <<= 1;
        int* hslot = malloc((size_t)cap * sizeof(int));
        unsigned* key_v = malloc((size_t)ncorner * sizeof(unsigned));
        unsigned* key_t = malloc((size_t)ncorner * sizeof(unsigned));
        unsigned* key_n = malloc((size_t)ncorner * sizeof(unsigned));
        float* np = malloc((size_t)ncorner * 3 * sizeof(float));
        float* nt = malloc((size_t)ncorner * 2 * sizeof(float));
        float* nc = malloc((size_t)ncorner * 4 * sizeof(float));
        float* nn = malloc((size_t)ncorner * 3 * sizeof(float));
        if (hslot && key_v && key_t && key_n && np && nt && nc && nn) {
            for (int i = 0; i < cap; i++) hslot[i] = -1;
            int nvert = 0;
            for (int ci = 0; ci < ncorner; ci++) {
                unsigned v = m->indices[ci], tt = corner_t[ci], nnn = corner_n[ci];
                // A face referencing a vertex we never loaded is dropped, not
                // clamped: mark the corner out of range and let the existing
                // compaction pass below delete the triangle and fix the group
                // spans, exactly as it does on the non-expanded path.
                if (v >= (unsigned)m->vertex_count) {
                    m->indices[ci] = (unsigned)-1;
                    continue;
                }
                if (tt >= (unsigned)nuv) tt = v;
                if (nnn >= (unsigned)nnrm) nnn = v;
                unsigned h = (v * 73856093u) ^ (tt * 19349663u) ^ (nnn * 83492791u);
                h &= (unsigned)(cap - 1);
                int found = -1;
                while (hslot[h] >= 0) {
                    int k = hslot[h];
                    if (key_v[k] == v && key_t[k] == tt && key_n[k] == nnn) {
                        found = k; break;
                    }
                    h = (h + 1u) & (unsigned)(cap - 1);
                }
                if (found < 0) {
                    found = nvert++;
                    hslot[h] = found;
                    key_v[found] = v; key_t[found] = tt; key_n[found] = nnn;
                    memcpy(np + (size_t)found * 3,
                           m->positions + (size_t)v * 3, 3 * sizeof(float));
                    if (m->uvs && nuv > 0)
                        memcpy(nt + (size_t)found * 2,
                               m->uvs + (size_t)tt * 2, 2 * sizeof(float));
                    else
                        nt[found * 2] = nt[found * 2 + 1] = 0.0f;
                    // rgb travels with the position, alpha with the vt (the
                    // reader stuffs the vt line's third component there)
                    nc[found * 4 + 0] = m->colors[(size_t)v * 4 + 0];
                    nc[found * 4 + 1] = m->colors[(size_t)v * 4 + 1];
                    nc[found * 4 + 2] = m->colors[(size_t)v * 4 + 2];
                    nc[found * 4 + 3] = m->colors[(size_t)tt * 4 + 3];
                    if (nnrm > 0)
                        memcpy(nn + (size_t)found * 3,
                               m->normals + (size_t)nnn * 3, 3 * sizeof(float));
                    else {
                        nn[found * 3] = 0.0f; nn[found * 3 + 1] = 1.0f;
                        nn[found * 3 + 2] = 0.0f;
                    }
                }
                m->indices[ci] = (unsigned)found;
            }
            free(m->positions); free(m->uvs); free(m->colors); free(m->normals);
            m->positions = np; m->uvs = nt; m->colors = nc; m->normals = nn;
            m->vertex_count = nvert;
            nuv = nnrm = nvert;
            np = nt = nc = nn = NULL;
        }
        free(hslot); free(key_v); free(key_t); free(key_n);
        free(np); free(nt); free(nc); free(nn);
    }
    free(corner_t); free(corner_n);
    corner_t = corner_n = NULL;

    // An OBJ written before the vertex-colour/normal channels existed (or a
    // car mesh, which this loader also serves) leaves these unset -- drop them
    // so callers can test for NULL rather than get a buffer of 0.5 grey.
    if (!have_color || nuv < m->vertex_count) { free(m->colors); m->colors = NULL; }
    if (!have_normal || nnrm < m->vertex_count) { free(m->normals); m->normals = NULL; }

    // B3_TRACK_NOVCOLOR=1: keep the alpha (the shine gate) but force the
    // colour to white, i.e. go back to the pre-recovery flat look. The baked
    // vertex lighting is dark -- the mean over every world vertex is 0.21 of
    // full on US_C3_V1 and 0.14 on AS_C1_V1, and retail makes that up with
    // the scene light record and the fog blend in the final combiner, neither
    // of which this harness reproduces yet. This is the escape hatch for
    // judging that gap by eye.
    if (m->colors && getenv("B3_TRACK_NOVCOLOR")) {
        for (int v = 0; v < m->vertex_count; v++) {
            m->colors[(size_t)v * 4 + 0] = 1.0f;
            m->colors[(size_t)v * 4 + 1] = 1.0f;
            m->colors[(size_t)v * 4 + 2] = 1.0f;
        }
    }

    // Drop any face referencing a vertex we did not load, so the renderer can
    // never index out of bounds. Group spans must shift with the compaction.
    {
        int kept = 0, gi = 0;
        int gstart[TRACKMESH_MAX_GROUPS] = {0}, gcount[TRACKMESH_MAX_GROUPS] = {0};
        for (int t = 0; t < m->triangle_count; t++) {
            while (gi < m->group_count &&
                   t >= m->groups[gi].first_triangle + m->groups[gi].triangle_count)
                gi++;
            unsigned* i = m->indices + (size_t)t * 3;
            if (i[0] < (unsigned)m->vertex_count && i[1] < (unsigned)m->vertex_count &&
                i[2] < (unsigned)m->vertex_count) {
                unsigned* d = m->indices + (size_t)kept * 3;
                d[0] = i[0]; d[1] = i[1]; d[2] = i[2];
                if (gi < m->group_count) {
                    if (gcount[gi] == 0) gstart[gi] = kept;
                    gcount[gi]++;
                }
                kept++;
            }
        }
        m->triangle_count = kept;
        for (int g = 0; g < m->group_count; g++) {
            m->groups[g].first_triangle = gstart[g];
            m->groups[g].triangle_count = gcount[g];
        }
    }

    trackmesh_gl_single_sided();
    resolve_materials(m, path, mtl_name);
    trackmesh_decals_last(m);

    // Prime the animated-material state so a renderer that never calls
    // trackmesh_tick() still draws the boards at phase 0 / frame 0, which is
    // what the loader FUN_0019AE10 case 0x17 zeroes them to.
    m->anim_time = 0.0f;
    m->anim_groups = 0;
    for (int g = 0; g < m->group_count; g++) {
        m->groups[g].uv_offset[0] = 0.0f;
        m->groups[g].uv_offset[1] = 0.0f;
        if (trackmesh_group_animated(m, g)) m->anim_groups++;
    }

    for (int k = 0; k < 3; k++) { m->min[k] = 1e30f; m->max[k] = -1e30f; }
    for (int v = 0; v < m->vertex_count; v++) {
        const float* p = m->positions + (size_t)v * 3;
        for (int k = 0; k < 3; k++) {
            if (p[k] < m->min[k]) m->min[k] = p[k];
            if (p[k] > m->max[k]) m->max[k] = p[k];
        }
    }
    return 0;
}

void trackmesh_free(TrackMesh* m) {
    free(m->positions); free(m->uvs); free(m->colors); free(m->normals);
    free(m->indices); free(m->shine_scratch);
    for (int g = 0; g < m->group_count; g++) free(m->groups[g].anim);
    memset(m, 0, sizeof(*m));
}

void trackmesh_set_group_texture(TrackMesh* m, int group, unsigned gl_texture) {
    if (group >= 0 && group < m->group_count)
        m->groups[group].gl_texture = gl_texture;
}

// The GL spelling of the render state the streamed material apply
// FUN_000393C0 queues for one material. Every branch below is a render state
// that function writes into the deferred shadow at 0x0075D4A0 + id*4:
//
//   flags & 0x001 -> RS 0x3B D3DRS_ALPHABLENDENABLE   @0x00039B21
//   flags & 0x010 -> RS 0x3C D3DRS_ALPHATESTENABLE    @0x00039BD4
//   flags & 0x400 -> RS 0x40 D3DRS_ZWRITEENABLE := 0  @0x00039AF5
//
// with the blend factors and the alpha test set once, globally:
//   RS 62/63/74 SRCBLEND/DESTBLEND/BLENDOP := 0x302/0x303/0x8006
//               (0x00038B06/0x00038B2F/0x00038B58 -- the NV2A blend tokens
//               ARE the GL enums, so 0x302/0x303 read straight across as
//               GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA and 0x8006 as
//               GL_FUNC_ADD)
//   RS 0x3A/0x3D ALPHAFUNC/ALPHAREF := D3DCMP_GREATER / 0x40, by the world
//               setup FUN_00038D10 (0x0003901B / 0x00038FEE)
//
// THE ALPHA BITS WERE SWAPPED IN THIS HARNESS until 2026-08-12, which is why
// the tree-shadow sheets (flags 0x040F: blend, no test) were being alpha
// TESTED and drawn opaque. Render state 0x3B is ALPHABLENDENABLE and 0x3C is
// ALPHATESTENABLE, pinned by the coherent triple FUN_00038D10 writes around
// them -- RS 0x3A := 0x204 (D3DCMP_GREATER on the 0x200 base that RS 57/ZFUNC
// already established), RS 0x3C := 0, RS 0x3D := 0x40 -- which only parses as
// ALPHAFUNC / ALPHATESTENABLE / ALPHAREF, in the same order the already-pinned
// 57 ZFUNC, 62 SRCBLEND, 63 DESTBLEND, 64 ZWRITEENABLE and 67
// COLORWRITEENABLE run in.
//
// `cutout_fallback` is the caller's texel-statistics guess. It is used only
// for a group with NO material record at all (an OBJ written before the MTL
// carried the flag word, or a submesh whose material index was out of range):
// with a record, the flags are the game's own answer and there is nothing to
// guess. The presence test is TrackMeshGroup.have_material and NOT
// `(flags || cls)` -- see the header comment on have_material for the tunnel
// walls that the latter erased.
//
// COPLANAR DRAW PRIORITY: THERE IS NONE TO RECOVER, AND NONE IS NEEDED.   [C]
//
// The big roadside billboard of US_C3_V1 (Chgo_BigAdverts_02, the poster, and
// Chgo_BigAdverts_Backs, its frame / backing panel / catwalk -- dump 028) was
// reported as flickering, on the theory that two coplanar surfaces were being
// drawn in an unstable order. Both materials carry flag word 0x0032, which
// FUN_000393C0 decodes as:
//
//   bit 0x001 CLEAR -> RS 0x3B D3DRS_ALPHABLENDENABLE := 0     @0x00039B21
//   bit 0x010 SET   -> RS 0x3C D3DRS_ALPHATESTENABLE  := 1     @0x00039BD4
//   bit 0x020 SET   -> RS 0x93 D3DRS_CULLMODE := 0, TWO-SIDED  @0x00039C86
//   bit 0x400 CLEAR -> RS 0x40 D3DRS_ZWRITEENABLE     := 1     @0x00039AF5
//   bit 0x002 SET   -> 4 into the texture-stage slot 0x0075D7F0 under
//                      deferred token 0x0B @0x00039C80. A stage slot, NOT an
//                      ordering field; its purpose is still [?].
//
// So the pair is OPAQUE, alpha-TESTED, depth-WRITING cut-out geometry. It is
// not blended at all -- 0x010 is the alpha TEST bit and the BLEND bit 0x001 is
// clear. Opaque surfaces under the ambient LESSEQUAL depth test compose
// order-independently, which is exactly why the material record carries no
// sort key and the world draw loop applies no sort: the streamed renderer's
// ONLY draw-priority mechanism is the 0x400 decal bit (ZWRITE off, drawn in
// the later transparent-slot pass FUN_001ADD60), and neither material has it.
//
// The geometry agrees. The poster quad stands 0.6040 world units IN FRONT of
// the 113.99 u^2 backing panel, and the Backs triangles that ARE exactly
// coplanar with the poster (|sep| <= 1e-4) are the board's border frame, which
// ABUTS the poster without covering it: clipping every cross-material pair
// against the other in their shared plane returns 0.0 u^2 of true overlap.
// Measured over frames 4169..4171, 0-1 of ~12,000 poster pixels ever resolve
// to the Backs texture rather than the poster's.
//
// This harness could not reorder them in any case: the world is compiled once
// into a single display list (glNewList / glCallList in burnout3_full.c), so
// group order is fixed for the whole run and cannot vary with the camera.
//
// The residual frame-to-frame churn on the board is TEXTURE ALIASING, not
// depth. Rendered ALONE through B3_TRACK_ONLYMAT below -- with nothing left in
// the world to contend with it for depth or order -- the poster face still
// shows 6.91% strongly-reversing pixels, against 9.71% in the full scene.
//
// => No polygon offset and no extra sort belong here. Do not re-chase this.
void trackmesh_group_state(const TrackMesh* m, int group, unsigned gl_texture,
                           int cutout_fallback) {
    int decal = 0, blend = 0, test = 0;
    if (group >= 0 && group < m->group_count) {
        const TrackMeshGroup* g = &m->groups[group];
        decal = g->decal;
        if (g->have_material) {
            blend = g->alpha_blend;
            test = g->alpha_test;
        } else {
            test = cutout_fallback;
        }
    } else {
        test = cutout_fallback;
    }
    if (!gl_texture) { blend = 0; test = 0; }

    // B3_TRACK_ONLYMAT=<substr>: draw ONLY the groups whose material name
    // contains <substr> and discard every other one. Diagnostic; it works
    // inside the baked display list because "discard everything" is a render
    // STATE here -- GL_GREATER against an alpha reference of 1.0, which no
    // fragment can pass -- rather than a skipped draw call. Used to answer
    // "do these two materials actually overlap in screen space?" by rendering
    // each of them alone from the same camera.
    static const char* onlymat = (const char*)1;
    if (onlymat == (const char*)1) onlymat = getenv("B3_TRACK_ONLYMAT");
    if (onlymat && group >= 0 && group < m->group_count
        && !strstr(m->groups[group].material, onlymat)) {
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 1.0f);
        glDisable(GL_TEXTURE_2D);
        return;
    }

    glDepthMask(decal ? GL_FALSE : GL_TRUE);
    if (blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    if (test) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, TRACKMESH_ALPHA_REF);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    if (gl_texture) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, gl_texture);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
}

void trackmesh_group_vertex_color(const TrackMesh* m, int group, unsigned v,
                                  float out[4]) {
    const TrackMeshGroup* g = (group >= 0 && group < m->group_count)
                            ? &m->groups[group] : NULL;
    // B3_TRACK_CLASSVIZ=1: replace the diffuse with a per-shader-class hue so
    // a screenshot says which material class covers which part of the frame.
    // Diagnostic only; the env lookup is cached because this runs once per
    // vertex of the whole track while the display list is built.
    static int classviz = -1;
    if (classviz < 0) classviz = getenv("B3_TRACK_CLASSVIZ") != NULL;
    if (classviz) {
        static const float pal[11][3] = {
            {1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}, {1,0,1}, {0,1,1},
            {1,.5f,0}, {.5f,0,1}, {0,.5f,.5f}, {.5f,.5f,.5f}, {1,1,1}
        };
        int c = g ? g->cls : 9;
        if (c < 0 || c > 10) c = 9;
        out[0] = pal[c][0]; out[1] = pal[c][1]; out[2] = pal[c][2];
        out[3] = 1.0f;
        return;
    }
    // rgb: the baked diffuse, already doubled by the loader (the world pixel
    // shaders carry SHIFTLEFT_1 on their stage-0 RGB output). Classes 8 and 9
    // have no D3DCOLOR register at all, so the game never reads it for them.
    if (m->colors && (!g || g->use_vertex_color) && v < (unsigned)m->vertex_count) {
        const float* c = m->colors + (size_t)v * 4;
        out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
    } else {
        out[0] = out[1] = out[2] = 1.0f;
    }
    // WORLD HEADROOM CURVE -- TUNED (user-authorized deviation 2026-08-13).
    //
    // The world path above is the recovered one and it is complete: 2*T*V,
    // the per-class specular/emissive adds, the fog blend, and (since the
    // PRESENT wave) the recovered present composite's x2 and the ^0.95 output
    // ramp. This is the one magnitude knob on it, applied at the single choke
    // point every world vertex colour passes through -- the display-list build
    // and the scroll pass both call this function.
    //
    // Shape: out = (1 - e^-G c) / (1 - e^-G), a one-parameter family that is
    // monotonic for every G, fixes 0 -> 0 and 1 -> 1, and is the identity at
    // G = 0. G > 0 LIFTS the shadows (what was needed before the present
    // composite existed, when the world sat ~1.6x under the references);
    // G < 0 COMPRESSES them (headroom, if the composite's x2 overshoots).
    // Because both ends are pinned, nothing clips and nothing inverts, the
    // white-vertex population (128 = white in the source data) is untouched,
    // and no per-track data is needed.
    //
    // Judged by eye against the xemu references WITH the present composite and
    // the gamma ramp in place -- which is the only meaningful comparison, the
    // references being final frames. B3_TRACK_LIFT=<G> overrides at runtime.
    {
        static float lg = 1e9f, lnorm = 1.0f;
        if (lg > 1e8f) {
            const char* e = getenv("B3_TRACK_LIFT");
            lg = e ? (float)atof(e) : TRACKMESH_LIFT_G;
            lnorm = (lg < -1e-4f || lg > 1e-4f)
                  ? 1.0f / (1.0f - expf(-lg)) : 0.0f;
        }
        if (lg < -1e-4f || lg > 1e-4f) {
            for (int i = 0; i < 3; i++)
                out[i] = (1.0f - expf(-lg * out[i])) * lnorm;
        }
    }
    // B3_TRACK_NODECAL=1: force the blended decal/shadow layer fully
    // transparent, to measure how much of the road's darkness is the shadow
    // sheets lying on it rather than the road's own baked colour. Diagnostic.
    static int nodecal = -1;
    if (nodecal < 0) nodecal = getenv("B3_TRACK_NODECAL") != NULL;
    if (nodecal && g && (g->decal || g->alpha_blend)) {
        out[3] = 0.0f;
        return;
    }
    // B3_TRACK_DECALONLY=<substr>: the same switch, inverted and narrowed --
    // keep only the decal/blended groups whose material name contains
    // <substr>, making every other one transparent, so a screenshot
    // attributes a dark band to one named sheet. Diagnostic.
    static const char* only = (const char*)1;
    if (only == (const char*)1) only = getenv("B3_TRACK_DECALONLY");
    if (only && g && (g->decal || g->alpha_blend) && !strstr(g->material, only)) {
        out[3] = 0.0f;
        return;
    }
    // w: material +0x20. Under GL_MODULATE the fragment alpha is
    // texture.a * primary.a, which is exactly the class-6 output alpha
    // tex.a * C0.a -- and C0.a IS material +0x20 (FUN_000393C0 loads it at
    // 0x0003945D/0x0003951E/... and FUN_0034E9A0 packs the float4 to a
    // D3DCOLOR with a plain *255, so 0.6 in the file means 0.6 on screen).
    out[3] = g ? g->alpha_scalar : 1.0f;
}

// The animated-material ticker FUN_0019B1E0's scroll branch, verbatim.
//
//   T      = the global clock DAT_0060EA20, in seconds (read @0x0019B1E8)
//   target = rate(+0x14) * T
//   next   = step(+0x18) + phase(+0x1C)
//   if (next <= target) phase = (step == 0) ? target : next
//
// at most one step per call, and the branch only runs for materials in the
// header+0x10 animated list whose frame count (+0x11) is < 2 and whose flag
// bit 0x200 is set (tested as byte +0x25 & 2). The material apply then pushes
// (1 - frac(phase), 0, 0) into vertex-shader constant 0x63 (0x00039CE0
// streamed / 0x0003A6C8 static) and every world vertex program starts with
// `add oT0.xy, v9.xy, c[99].xy` -- so U scrolls and V never moves.
// THE FRAME ARM is the other half of the same `if`, and it is the one the port
// was missing: `Arrows` was the only material in US_C3_V1 that takes the
// scroll arm, so tagging scrolls alone left every bulb-matrix message board in
// the game -- the SLOW/DOWN accident boards, the warning signs, the flags,
// even the water -- standing on frame 0 forever. See TrackMeshAnim for the
// decompile and for why the frame durations come out of the texture names.
static void trackmesh_tick_frames(TrackMeshAnim* a, float T) {
    if (a->count < 2) return;
    // `if (hold(+0x18) + last(+0x1C) < T)`, one swap per call at most.
    if (!(a->hold + a->last < T)) return;
    a->last = T;
    int nxt;
    if (a->pingpong) {
        // flags & 0x100. No shipped material sets it; kept for fidelity.
        int st = a->step;
        int f = a->index + st;
        a->index = f;
        if (st == 1 && a->count - 1 <= f) a->step = -1;
        else if (st == -1 && f < 1) { a->step = 1; a->label = 1; }
        if (a->index < 0) a->index = 0;
        if (a->index >= a->count) a->index = a->count - 1;
        nxt = (a->index + 1) % a->count;
        if (a->step == -1) {
            // Running backwards the arm re-reads the CURRENT frame's label and
            // subtracts, so the same keyframe spacing plays in reverse.
            if (nxt == 0) { a->hold = a->period; }
            else {
                int L = a->frame_label[a->index];
                a->hold = (float)(a->label - L) * a->period;
                a->label = L;
            }
            return;
        }
    } else {
        a->index = (a->index + 1) % a->count;
        nxt = (a->index + 1) % a->count;
        if (nxt == 0) {
            // Wrapping back to frame 0: the arm does NOT parse frame 0's name
            // (it has no digits), it hard-codes one period and label 1.
            a->hold = a->period;
            a->label = 1;
            return;
        }
    }
    {
        int L = a->frame_label[nxt];
        a->hold = (float)(L - a->label) * a->period;
        a->label = L;
    }
}

int trackmesh_tick(TrackMesh* m, float dt) {
    if (!m || m->anim_groups == 0) return 0;
    if (dt > 0.0f) m->anim_time += dt;
    for (int g = 0; g < m->group_count; g++) {
        TrackMeshGroup* grp = &m->groups[g];
        // The two arms are exclusive in the game (`frame_count < 2` picks the
        // scroll one), and the extractor tags a material for one or the other,
        // never both.
        if (grp->anim) { trackmesh_tick_frames(grp->anim, m->anim_time); continue; }
        if (grp->uv_scroll_rate <= 0.0f) continue;
        float target = grp->uv_scroll_rate * m->anim_time;
        float next = grp->uv_scroll_step + grp->uv_scroll_phase;
        if (next <= target)
            grp->uv_scroll_phase = (grp->uv_scroll_step == 0.0f) ? target : next;
        float ph = grp->uv_scroll_phase;
        grp->uv_offset[0] = 1.0f - (ph - floorf(ph));
        grp->uv_offset[1] = 0.0f;
    }
    return m->anim_groups;
}

int trackmesh_group_animated(const TrackMesh* m, int group) {
    if (!m || group < 0 || group >= m->group_count) return 0;
    const TrackMeshGroup* grp = &m->groups[group];
    // B3_TRACK_NOFRAMEANIM=1: pretend the ticker's frame arm does not exist,
    // which bakes the frame-cycling groups at frame 0 and reproduces exactly
    // what this port drew before the arm was recovered -- the SLOW/DOWN boards
    // frozen on one message. Diagnostic, for before/after captures.
    static int noframe = -1;
    if (noframe < 0) noframe = getenv("B3_TRACK_NOFRAMEANIM") != NULL;
    if (grp->anim && grp->anim->count >= 2) return !noframe;
    // rate <= 0 is frozen in retail too -- see the uv_scroll_rate comment in
    // the header -- so those stay baked, at the phase-0 offset retail shows.
    return grp->uv_scroll_rate > 0.0f;
}

unsigned trackmesh_group_texture(const TrackMesh* m, int group) {
    if (!m || group < 0 || group >= m->group_count) return 0;
    const TrackMeshGroup* grp = &m->groups[group];
    const TrackMeshAnim* a = grp->anim;
    if (a && a->count >= 2) {
        int i = a->index;
        if (i >= 0 && i < a->count && a->frame_gl[i]) return a->frame_gl[i];
    }
    return grp->gl_texture;
}

int trackmesh_load_frame_textures(TrackMesh* m, TrackMeshTexLoader fn,
                                  void* user) {
    if (!m || !fn) return 0;
    int n = 0;
    for (int g = 0; g < m->group_count; g++) {
        TrackMeshAnim* a = m->groups[g].anim;
        if (!a || a->count < 2) continue;
        for (int i = 0; i < a->count; i++) {
            if (!a->frame_texture[i][0]) continue;
            // Frames repeat across groups (every submesh of one material has
            // its own copy of the record) and across materials (`water` is
            // shared), so reuse a name already resolved for the same path.
            unsigned tex = 0;
            for (int h = 0; h <= g && !tex; h++) {
                const TrackMeshAnim* b = m->groups[h].anim;
                if (!b) continue;
                int lim = (h == g) ? i : b->count;
                for (int k = 0; k < lim; k++)
                    if (b->frame_gl[k]
                        && strcmp(b->frame_texture[k], a->frame_texture[i]) == 0) {
                        tex = b->frame_gl[k];
                        break;
                    }
            }
            if (!tex) {
                tex = fn(a->frame_texture[i], user);
                if (tex) n++;
            }
            a->frame_gl[i] = tex;
        }
    }
    return n;
}

// Draw one group's triangles with a UV offset. Shared by the scroll pass; the
// baked-display-list path in the renderer does the same thing with offset 0.
static int trackmesh_emit_group(const TrackMesh* m, int g, const float uv[2]) {
    const TrackMeshGroup* grp = &m->groups[g];
    if (grp->triangle_count <= 0) return 0;
    const unsigned* idx = m->indices + (size_t)grp->first_triangle * 3;
    int n = grp->triangle_count * 3;
    float col[4];
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < n; i++) {
        unsigned v = idx[i];
        trackmesh_group_vertex_color(m, g, v, col);
        glColor4fv(col);
        if (m->uvs)
            glTexCoord2f(m->uvs[(size_t)v * 2] + uv[0],
                         m->uvs[(size_t)v * 2 + 1] + uv[1]);
        glVertex3fv(m->positions + (size_t)v * 3);
    }
    glEnd();
    return grp->triangle_count;
}

int trackmesh_draw_scroll(const TrackMesh* m) {
    if (!m || m->anim_groups == 0) return 0;
    int drawn = 0;
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_CURRENT_BIT);
    for (int g = 0; g < m->group_count; g++) {
        const TrackMeshGroup* grp = &m->groups[g];
        if (!trackmesh_group_animated(m, g) || grp->triangle_count <= 0) continue;
        // A frame-cycling group binds THIS FRAME's texture; a scrolling one
        // binds its only one and moves the UVs instead.
        trackmesh_group_state(m, g, trackmesh_group_texture(m, g), 0);
        drawn += trackmesh_emit_group(m, g, grp->uv_offset);
    }
    glDepthMask(GL_TRUE);
    glPopAttrib();
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    return drawn;
}

// The world fog, as FUN_00038D10 programs it (see TrackScene for the chain).
//
//   D3DRS_FOGENABLE     1                        0x00038F47
//   D3DRS_FOGTABLEMODE  3 = D3DFOG_LINEAR        0x00038F23
//   D3DRS_FOGSTART      fog_far * 0.05           0x00038ECB
//   D3DRS_FOGEND        (fog_far-start)/div+start 0x00038EFC
//   D3DRS_FOGCOLOR      authored colour * 127.5  0x00038E0E (NOT *255)
//
// and the fog COORDINATE is not the raw depth: every world vertex program
// ends with `min oFog, r12.z, c[120].z` with c[120].z = fog_far, so the
// coordinate is clamped at fog_far and the linear factor never falls below
//     f_min = (fog_end - fog_far) / (fog_end - fog_start)
// (0.75 on US_C3_V1: far 1000, start 50, end 3850).
//
// THE FOG-COORDINATE CLAMP, and how it is reproduced here.
//
// `c[120]` is uploaded by exactly ONE call site in the whole executable --
// `MOV ECX,0x78 / CALL SetVertexShaderConstant` at 0x00038F59, inside this
// same world setup (a whole-image scan for `MOV ECX,0x78` feeding either
// uploader finds one hit; scratchpad cscan.py). Its float4 is built on the
// stack at ESP+0x40..0x4C and the .z lane is written at 0x00038DD8 from XMM1,
// which 0x00038D8D loaded as `[ESI + 0x10]` -- the SAME field the very next
// block turns into D3DRS_FOGSTART and D3DRS_FOGEND:
//
//   00038d87  ADD   ESI,0x60e100            ; fog record = 0x0060E100 + env*0x40
//   00038d8d  MOVSS XMM1,[ESI + 0x10]       ; fog_far
//   00038dd8  MOVSS [ESP + 0x48],XMM1       ; c[120].z := fog_far
//   00038f59  MOV   ECX,0x78 / CALL 0x0034f840
//   00038e09  MOVSS XMM0,[ESI + 0x10]       ; fog_far
//   00038e10  MULSS XMM0,[0x003a69bc]       ;   * 0.05          -> FOGSTART
//   00038e23  SUBSS XMM1,XMM0 / DIVSS [ESI+0x14] / ADDSS XMM0   -> FOGEND
//
// So `c[120].z` IS `fog_far`, in the same units as FOGSTART/FOGEND, and the
// floor is a property of the track's own three numbers -- nothing is assumed.
// (0.75 on US_C3_V1, 0.80 on AS_C1_V1/AS_M1_V1, 0.70 on AS_C2_V1, 0.90 on
// EU_C3_V1, 0.75 on US_C1_V1.)                                          [C]
//
// MECHANISM -- GLUE. Fixed-function GL cannot express the clamp: its linear
// factor is affine in the fragment's eye distance and clamps to 0, and no
// choice of GL_FOG_START/GL_FOG_END/GL_FOG_COLOR can make an affine ramp hold
// still at f_min while still matching the true ramp inside [start, fog_far]
// (matching the ramp fixes both endpoints, and then the clamp is 0 by
// construction). GL_FOG_COORD would express it exactly but has to be supplied
// per vertex, and the world is one baked display list.
//
// What IS both exact and cheap is to write the fog coordinate from a vertex
// shader: with a vertex program bound and NO fragment program, GL 2.0 keeps
// the whole fixed-function fragment stage -- texturing, alpha test, blend and
// the linear fog table -- and simply takes the fog coordinate from
// `gl_FogFragCoord`. That is one line, `min(|z_eye|, fog_far)`, i.e. the
// microcode's `MIN oFog, r12.z, c[120].z` verbatim, and it works through
// glCallList because the display list replays vertices into the shader.
//
// The GLUE in it is (a) GLSL instead of NV2A microcode and (b) reading the
// microcode's `r12.z` as the GL eye-space depth |z_eye|. Both fog ramps are
// programmed from the same recovered fog_start/fog_end, so inside [start,
// fog_far] this changes nothing; past fog_far it stops the factor falling.
// B3_TRACK_NOFOGFLOOR=1 falls back to the old unclamped fixed-function fog,
// which is also what happens if the GL 2.0 entry points are unavailable.
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER  0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS    0x8B82
#endif

static const char* TRACKMESH_FOG_VS =
    "uniform float uFogFar;\n"
    "void main() {\n"
    "  gl_Position    = ftransform();\n"
    "  gl_FrontColor  = gl_Color;\n"
    "  gl_BackColor   = gl_Color;\n"
    "  gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    // MIN oFog, r12.z, c[120].z  -- the last instruction of every world
    // vertex program (0x003E88C0 +15 and siblings).
    "  vec4 e = gl_ModelViewMatrix * gl_Vertex;\n"
    "  gl_FogFragCoord = min(abs(e.z), uFogFar);\n"
    "}\n";

static unsigned (*p_glCreateShader)(unsigned);
static void (*p_glShaderSource)(unsigned, int, const char* const*, const int*);
static void (*p_glCompileShader)(unsigned);
static void (*p_glGetShaderiv)(unsigned, unsigned, int*);
static void (*p_glGetShaderInfoLog)(unsigned, int, int*, char*);
static unsigned (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(unsigned, unsigned);
static void (*p_glLinkProgram)(unsigned);
static void (*p_glGetProgramiv)(unsigned, unsigned, int*);
static void (*p_glUseProgram)(unsigned);
static void (*p_glDeleteShader)(unsigned);
static void (*p_glDeleteProgram)(unsigned);
static int (*p_glGetUniformLocation)(unsigned, const char*);
static void (*p_glUniform1f)(int, float);

static int      g_fogprog_state = -1;   // -1 untried, 0 unavailable, 1 ready
static unsigned g_fogprog;
static int      g_fogprog_far;

// Compile the fog-coordinate vertex program. Returns 1 once it is usable.
static int trackmesh_fogprog(void) {
    if (g_fogprog_state >= 0) return g_fogprog_state;
    g_fogprog_state = 0;
    if (getenv("B3_TRACK_NOFOGFLOOR")) return 0;
#define B3TM_GET(fn) do { *(void**)(&p_##fn) = SDL_GL_GetProcAddress(#fn); \
                          if (!p_##fn) return 0; } while (0)
    B3TM_GET(glCreateShader);  B3TM_GET(glShaderSource);
    B3TM_GET(glCompileShader); B3TM_GET(glGetShaderiv);
    B3TM_GET(glGetShaderInfoLog);
    B3TM_GET(glCreateProgram); B3TM_GET(glAttachShader);
    B3TM_GET(glLinkProgram);   B3TM_GET(glGetProgramiv);
    B3TM_GET(glUseProgram);    B3TM_GET(glDeleteShader);
    B3TM_GET(glDeleteProgram); B3TM_GET(glGetUniformLocation);
    B3TM_GET(glUniform1f);
#undef B3TM_GET
    unsigned vs = p_glCreateShader(GL_VERTEX_SHADER);
    p_glShaderSource(vs, 1, &TRACKMESH_FOG_VS, NULL);
    p_glCompileShader(vs);
    int ok = 0;
    p_glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        int n = 0;
        p_glGetShaderInfoLog(vs, (int)sizeof log, &n, log);
        log[(n > 0 && n < (int)sizeof log) ? n : 0] = 0;
        fprintf(stderr, "[trackmesh] fog vertex shader failed: %s\n", log);
        p_glDeleteShader(vs);
        return 0;
    }
    unsigned pr = p_glCreateProgram();
    p_glAttachShader(pr, vs);
    p_glLinkProgram(pr);
    p_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    p_glDeleteShader(vs);
    if (!ok) { p_glDeleteProgram(pr); return 0; }
    g_fogprog = pr;
    g_fogprog_far = p_glGetUniformLocation(pr, "uFogFar");
    g_fogprog_state = 1;
    return 1;
}

void trackmesh_fog_begin(const TrackMesh* m) {
    if (!m || !m->scene.valid || !m->scene.fog_enabled) return;
    if (getenv("B3_TRACK_NOFOG")) return;
    GLfloat c[4] = { m->scene.fog_rgb[0], m->scene.fog_rgb[1],
                     m->scene.fog_rgb[2], 1.0f };
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogfv(GL_FOG_COLOR, c);
    glFogf(GL_FOG_START, m->scene.fog_start);
    glFogf(GL_FOG_END, m->scene.fog_end);
    glHint(GL_FOG_HINT, GL_NICEST);
    glEnable(GL_FOG);
    if (trackmesh_fogprog()) {
        p_glUseProgram(g_fogprog);
        if (g_fogprog_far >= 0) p_glUniform1f(g_fogprog_far, m->scene.fog_far);
    }
}

void trackmesh_fog_end(void) {
    if (g_fogprog_state == 1) p_glUseProgram(0);
    glDisable(GL_FOG);
}

// x^p for x in [0,1], p > 0, to within about 2% -- the classic exp2/log2
// bit-trick pair. The shine pass evaluates this once per vertex per frame
// (about 12k vertices on US_C3_V1) and the result lands in an 8-bit colour
// channel, so libm's powf would be 50x the cost for no visible difference.
static float fast_powf(float x, float p) {
    union { float f; unsigned u; } v;
    if (x <= 0.0f) return 0.0f;
    v.f = x;
    // log2(x) ~ (bits/2^23 - 127) corrected by a quadratic in the mantissa
    float lg = (float)v.u * (1.0f / 8388608.0f) - 127.0f;
    float frac = lg - (float)(int)lg;
    if (frac < 0.0f) frac += 1.0f;
    lg -= (frac - frac * frac) * 0.346607f;
    float e = lg * p;
    if (e < -60.0f) return 0.0f;
    if (e > 0.0f) return 1.0f;                 // x<=1 and p>0 => x^p <= 1
    float ef = e - (float)(int)e;
    if (ef < 0.0f) ef += 1.0f;
    e += (ef - ef * ef) * 0.346607f;
    v.u = (unsigned)((e + 127.0f) * 8388608.0f);
    return v.f;
}

// The class-1/7/10 additive term, as its own GL pass.
//
// THE MECHANISM (tools/extract_track.py's "Shader classes" section carries the
// byte-level citations). The retail world pixel shaders are single-stage
// register-combiner programs; the class-1 one (D3DPIXELSHADERDEF at
// 0x003E8EE8, bound from slot 0x004D6578 by FUN_000393C0) has TWO stages:
//
//     stage 0: R0.rgb = 2 * (T0.rgb * V0.rgb)    R0.a = T0.a * V0.a
//     stage 1: R0.rgb = R0.a * C0.rgb + R0.rgb
//
// so the surface is its texture modulated by the vertex colour, PLUS an
// additive term masked per texel by the texture's alpha channel. V0 comes from
// the class-1 vertex program at 0x003E88C0, which is the class-0 program plus
// a Phong specular: it builds R = 2*N*(N.L) - L from the NORMPACKED3 normal at
// vertex +0x0C and the light direction in constant c[1], normalises the view
// vector V = position - c[0], and evaluates LIT(R.V) with the exponent taken
// from constant 0x62.w = the material's +0x08. oD0.rgb is the plain vertex
// colour; only oD0.w carries the specular, and material flag bit 0x40 selects
// the variant that multiplies it by the artist-painted vertex alpha.
// C0.rgb is the scene light colour (DAT_0060E0A0..AC) times material +0x04.
//
// So: colour = 2*tex.rgb*vcol.rgb + tex.a * gate * pow(max(R.V,0),power)
//                                         * light.rgb * strength.
//
// The first term is the ordinary modulated base pass; this function draws the
// second. In GL that is an additive pass (GL_ONE/GL_ONE, depth test LEQUAL,
// depth writes off) whose fragment colour is tex.alpha * primary colour, which
// GL_COMBINE expresses exactly as MODULATE(TEXTURE.alpha, PRIMARY.rgb) -- and
// the per-vertex primary colour is light*strength*gate*specular, computed here
// because the specular is view dependent and cannot live in a display list.
//
// NO LONGER GLUE. The light direction is vertex-shader constant c[0x61], which
// FUN_00038D10 fills at 0x00038D62..0x00038D92 by negating the float4 at
// +0x00 of the active light record (DAT_0060E0F0 + DAT_0060E170*0x40) -- and
// that float4 is enviro.dat +0x80, normalised, copied there by FUN_001888F0's
// 2-iteration loop at 0x00188B40. The scene light colour is DAT_0060E0A0 =
// enviro.dat +0x60. tools/extract_track.py's parse_enviro() reads both out of
// the track's own enviro.dat and writes them into the MTL header, so the
// values below are the game's, per track. B3_TRACK_SUN still overrides.
int trackmesh_draw_shine(TrackMesh* m, const float eye[3],
                         const float light_dir[3]) {
    if (!m->colors || !m->normals || m->group_count == 0) return 0;
    if (getenv("B3_TRACK_NOSHINE")) return 0;

    // The pre-2026-08-12 default was a CALIBRATED GUESS -- (0.9, -0.35, -0.25),
    // fitted to the xemu reference frames -- because the light record had not
    // been traced back to a shipped file. It is kept only as the fallback for
    // an OBJ whose MTL carries no scene block.
    //
    // THE SIGN QUESTION IS CLOSED, AND THE ANSWER IS "THE LOBE POINTS AWAY
    // FROM THE SKY". The old comment left it [?]; every link is now read
    // byte-exact, so it is [C], and the geometric consequence is stated here
    // because it is the whole behaviour of this term:
    //
    //   * the microcode. Re-disassembled from 0x003E88C0 with the canonical
    //     NV2A field map (scratchpad worldspec/vp3.py; the xvs block is a
    //     4-byte header {u16 ver 0x2078, u16 count 18} followed by
    //     instructions in natural DWORD0..3 order, DWORD0 always zero).
    //     Instruction 0 is `ADD r2.xyz, v0, -c[96]` (A_MUX=V/v0 unnegated,
    //     C_MUX=C/c[96] with C_NEG set) and instruction 9 is
    //     `ADD r8.xyz, r7, -c[97]` (A=r7, C=c[97] with C_NEG set), so
    //     r2 = P - eye and r8 = 2N(N.c97) - c97, exactly as implemented
    //     below.                                                          [C]
    //   * c[96] = the float4 at 0x004D67D0 with .y += 5.0 (literal 5.0 at
    //     0x003B1694, added at 0x00038F63..0x00038F79), uploaded by
    //     SetVertexShaderConstant(ECX=0x60) at 0x00039128. 0x004D67D0 is the
    //     camera world position: the sky dome uses it as the translation row
    //     of its world matrix (0x00032601..0x0003262D) and the CAR vertex
    //     program takes the same address as its eye constant c[108]
    //     (FUN_00031690 @0x00031750).                                     [C]
    //   * c[97] = -(light record +0x00) -- XORPS against a broadcast
    //     0x80000000 at 0x00038D4C..0x00038D65 -- and light record +0x00 is
    //     enviro.dat +0x80 normalised in place (FUN_001888F0's MOVAPS pair at
    //     0x00188B50/0x00188B57).                                         [C]
    //   * enviro.dat +0x80 is the sun's DOWNWARD TRAVEL direction on all 36
    //     shipped tracks: its y is -sin(elevation) for round elevations
    //     (-0.2588 = 15 deg, -0.3420 = 20, -0.4226 = 25, -0.5 = 30,
    //     -0.6428 = 40, -0.7071 = 45), never positive, and the vector is
    //     already unit (scratchpad worldspec/envdump.py).                 [C]
    //
    // Put together: because reflect2(A) := 2N(N.A) - A obeys
    // reflect2(-A) = -reflect2(A), the term equals
    //     ( reflect2(enviro+0x80) . unit(eye - P) )^power
    // i.e. textbook Phong with L := enviro.dat+0x80 -- a light vector that
    // points BELOW the horizon. The lobe therefore sits below the surface for
    // anything facing the sky, and the term is identically zero on level
    // road: measured 0.0000 on every GL_road5/6/7 group at frame 1100, and it
    // only fires where the surface is above the eye (uphill crests, max 0.21)
    // or near-vertical (signs, shopfronts, class 2 glass, where it reaches
    // ~1.0). That is what the shipped shader computes, and it means THIS TERM
    // CANNOT BE THE SOURCE OF A BROAD ADDITIVE LIFT ON THE NEAR ROAD.
    //
    // The old guess's large contribution was standing in for the three things
    // that were actually missing -- the shadow sheets' alpha scalar, the
    // 64/255 alpha reference and the fog -- all of which are recovered now.
    float L[3] = { 0.900f, -0.350f, -0.250f };
    float LC[3] = { 1.0f, 1.0f, 1.0f };
    int have_light = 1;
    if (m->scene.valid) {
        L[0] = m->scene.light_dir[0];
        L[1] = m->scene.light_dir[1];
        L[2] = m->scene.light_dir[2];
        LC[0] = m->scene.light_rgb[0];
        LC[1] = m->scene.light_rgb[1];
        LC[2] = m->scene.light_rgb[2];
    }
    if (light_dir) { L[0] = light_dir[0]; L[1] = light_dir[1]; L[2] = light_dir[2]; }
    {
        const char* e = getenv("B3_TRACK_SUN");
        if (e && strcmp(e, "none") == 0) {
            have_light = 0;
        } else if (e && sscanf(e, "%f,%f,%f", &L[0], &L[1], &L[2]) == 3) {
            have_light = 1;
        }
    }
    if (have_light) {
        float n = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (n < 1e-6f) return 0;
        L[0] /= n; L[1] /= n; L[2] /= n;
    }

    // c[96] IS NOT THE RAW CAMERA POSITION. FUN_00038D10 copies the float4 at
    // 0x004D67D0 into the constant and then adds the literal 5.0 (0x003B1694)
    // to its .y lane before the upload:
    //     00038F63  MOVSS XMM0,[ESP+0x24]        ; the .y lane
    //     00038F71  ADDSS XMM0,[0x003B1694]      ; + 5.0
    //     00038F79  MOVSS [ESP+0x24],XMM0
    //     0003911F  LEA EDX,[ESP+0x20] / MOV ECX,0x60 / CALL SetVSConst  [C]
    // The port used the raw camera position, which tilts the view vector on
    // the near road by a couple of degrees. Recovered value, no fitting.
    float E[3] = { eye[0], eye[1] + 5.0f, eye[2] };

    if (m->shine_scratch_verts < m->vertex_count) {
        float* p = realloc(m->shine_scratch,
                           (size_t)m->vertex_count * 3 * sizeof(float));
        if (!p) return 0;
        m->shine_scratch = p;
        m->shine_scratch_verts = m->vertex_count;
    }

    // B3_TRACK_SHINE_STATS=1: report what the specular term actually evaluates
    // to this frame (how many vertices land on the lit side of the lobe, and
    // how strong the term gets). Diagnostic only, off by default.
    int stats = getenv("B3_TRACK_SHINE_STATS") != NULL;
    double st_sum = 0.0; float st_max = 0.0f; long st_n = 0, st_pos = 0;
    double g_sum = 0.0; float g_max = 0.0f; long g_n = 0;

    int drawn = 0, state = 0;
    for (int g = 0; g < m->group_count; g++) {
        const TrackMeshGroup* grp = &m->groups[g];
        if (grp->shine_strength <= 0.0f || grp->triangle_count <= 0) continue;
        if (!grp->gl_texture) continue;

        if (!state) {
            state = 1;
            glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_TEXTURE_BIT
                         | GL_FOG_BIT | GL_CURRENT_BIT);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glDepthMask(GL_FALSE);
            glDepthFunc(GL_LEQUAL);
            glDisable(GL_ALPHA_TEST);
            glEnable(GL_TEXTURE_2D);
            // FOG ON AN ADDITIVE PASS. In retail this term is part of the same
            // pixel-shader result the final combiner fogs:
            //     out = f*(base + spec) + (1-f)*fogColour
            // Splitting it into two GL passes gives
            //     [f*base + (1-f)*fogColour] + <this pass>
            // so this pass must contribute exactly f*spec -- attenuated by the
            // fog factor but adding NO fog colour. GL's fog equation
            // f*C + (1-f)*Cfog does exactly that with Cfog = black, so the
            // additive pass runs the same fog range with a black fog colour.
            if (m->scene.valid && m->scene.fog_enabled
                && !getenv("B3_TRACK_NOFOG")) {
                GLfloat black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                glFogi(GL_FOG_MODE, GL_LINEAR);
                glFogfv(GL_FOG_COLOR, black);
                glFogf(GL_FOG_START, m->scene.fog_start);
                glFogf(GL_FOG_END, m->scene.fog_end);
                glEnable(GL_FOG);
            } else {
                glDisable(GL_FOG);
            }
            // fragment = texture.alpha * primary.rgb
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_ALPHA);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
        }
        // The shine term is gated on tex.a, and a frame-cycling material's
        // alpha mask changes with the frame, so bind the frame the base pass
        // just drew rather than frame 0's texture.
        glBindTexture(GL_TEXTURE_2D, trackmesh_group_texture(m, g));

        // Per-vertex specular for exactly the vertices this group touches.
        const unsigned* idx = m->indices + (size_t)grp->first_triangle * 3;
        int n = grp->triangle_count * 3;
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < n; i++) {
            unsigned v = idx[i];
            const float* P = m->positions + (size_t)v * 3;
            const float* N = m->normals + (size_t)v * 3;
            const float* C = m->colors + (size_t)v * 4;
            float gate = grp->shine_gate ? C[3] : 1.0f;
            float s = 0.0f;
            if (gate > 0.0f) {
                if (grp->shine_power <= 0.0f || !have_light) {
                    // class 10 is emissive (no specular at all), and with no
                    // light direction the factor is pinned at 1 -- see above.
                    s = 1.0f;
                } else {
                    float ndl = N[0] * L[0] + N[1] * L[1] + N[2] * L[2];
                    float R[3] = { 2.0f * N[0] * ndl - L[0],
                                   2.0f * N[1] * ndl - L[1],
                                   2.0f * N[2] * ndl - L[2] };
                    float V[3] = { P[0] - E[0], P[1] - E[1], P[2] - E[2] };
                    float vl = sqrtf(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
                    if (vl > 1e-6f) {
                        float rv = (R[0] * V[0] + R[1] * V[1] + R[2] * V[2]) / vl;
                        if (rv > 0.0f) s = fast_powf(rv, grp->shine_power);
                    }
                }
            }
            s *= gate * grp->shine_strength;
            if (stats) {
                st_n++; st_sum += s;
                if (s > 0.0f) st_pos++;
                if (s > st_max) st_max = s;
                g_n++; g_sum += s;
                if (s > g_max) g_max = s;
            }
            // C0.rgb = scene light colour * material +0x04 -- the material
            // apply multiplies the two at 0x000394D7..0x00039518 (class 1) and
            // 0x0003941A (class 7) and hands the product to FUN_0034E9A0 as
            // combiner factor 0. The light colour is enviro.dat +0x60, warm on
            // most tracks (US_C3_V1: 0.992, 0.894, 0.675).
            glColor3f(s * LC[0], s * LC[1], s * LC[2]);
            glTexCoord2fv(m->uvs + (size_t)v * 2);
            glVertex3fv(P);
        }
        glEnd();
        drawn += grp->triangle_count;
        if (stats && g_n > 0) {
            fprintf(stderr, "   [shinegrp] %-22s tris=%5d tex=%3u str=%.2f "
                    "pow=%.2f gate=%d  meanS=%.4f maxS=%.4f\n",
                    grp->material, grp->triangle_count, grp->gl_texture,
                    grp->shine_strength, grp->shine_power, grp->shine_gate,
                    g_sum / (double)g_n, g_max);
            g_sum = 0.0; g_max = 0.0f; g_n = 0;
        }
    }
    if (state) {
        glPopAttrib();
        glColor3f(1.0f, 1.0f, 1.0f);
    }
    if (stats)
        fprintf(stderr, "[shine] L=(%.3f %.3f %.3f) c96=(%.1f %.1f %.1f) "
                "verts=%ld lit=%ld (%.2f%%) mean=%.4f max=%.4f tris=%d\n",
                L[0], L[1], L[2], E[0], E[1], E[2], st_n, st_pos,
                st_n ? 100.0 * (double)st_pos / (double)st_n : 0.0,
                st_n ? st_sum / (double)st_n : 0.0, st_max, drawn);
    return drawn;
}
