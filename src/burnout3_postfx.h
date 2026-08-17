#ifndef BURNOUT3_POSTFX_H
#define BURNOUT3_POSTFX_H
/* World post FX: the sky rendering and the speed/boost blur post-effect —
 * RE'd from the retail XBE against the xemu reference captures.
 *
 * OWNERSHIP: this module (both files) belongs to the post-FX agent. The
 * harness calls only this contract; burnout3_full.c call sites are patched
 * by the orchestrator on landing (see scratchpad integration_postfx.md).
 *
 * Evidence marks follow the project convention: [C] confirmed against the
 * binary (address cited), [S] strongly supported, [?] unknown, GLUE =
 * harness-side invention, clearly labelled. Full write-up: docs/RE_POSTFX.md.
 *
 *
 * 1. THE SKY  —  a generated dome, not track geometry
 * ---------------------------------------------------
 * No material in any shipped track's static.dat names a sky texture (US/C3_V1
 * 167 records, AS/C1_V1 180 records — checked). The sky is a procedurally
 * generated dome built once at race init by FUN_00032020 (0x00032020) into a
 * 0x41C0-byte vertex buffer at stride 0x20, and drawn by FUN_00032580
 * (0x00032580) as D3DPT_TRIANGLELIST with 1344 (0x540) indices, centred on
 * the camera and uniformly scaled by (farClip - 1000). Its textures come from
 * the per-track file `Tracks/<region>/<track>/enviro.dat`, loaded by
 * FUN_001888F0 (0x001888F0):  gradients (32x32), clouds (1024x128),
 * envmapclouds (512x128), suncorona (128x256).
 *
 *
 * 2. THE SPEED BLUR  —  a per-screen radial zoom
 * -----------------------------------------------
 * FUN_0002EBE0 (0x0002EBE0) constructs one 0xA0-byte radial-blur state per
 * screen (at renderer+0x1D0 and +0x270; the active one is published to
 * DAT_004D6524 = renderer+0x3B4). It holds the `radialblurmask` texture
 * and two zoom layers. The strength-vs-speed curve is NOT recovered — see
 * B3_BLUR_* below, which is explicitly GLUE.
 *
 *
 * 3. THE PRESENT COMPOSITE  —  recovered, and it is OVER-UNITY
 * ------------------------------------------------------------
 * The whole in-race scene is rendered OFF SCREEN into a 640x480
 * X_D3DFMT_LIN_A8R8G8B8 surface at renderer+0x890 (created by FUN_0003D890
 * @0x0003D8A7 with (640, 480, fmt 0x12); bound as the render target by
 * FUN_0003FA20 @0x0003FA43, `SetRenderTarget(this+0x890, this+0x868)`).
 * FUN_0003E520 (0x0003E520) then reduces it 640x480 -> 320x240 (+0x8A8) ->
 * 160x120 (+0x8C0) -> 160x120 (+0x8D8), all X_D3DFMT_LIN_R5G6B5, with the
 * radial zoom applied in the last pass.
 *
 * FUN_0003DA90 (0x0003DA90) is the pass that puts the frame on screen: one
 * full-screen QUAD (NV097_SET_BEGIN_END = 8 @0x0003DD13), alpha blend and
 * alpha test both OFF (render states 0x3C/0x3B set to 0 @0x0003DC19/
 * 0x0003DBFA), texture stage 0 = renderer+0x890 (@0x0003DC96), stage 1 =
 * renderer+0x8D8 (@0x0003DC9C), pixel shader = renderer+0x4A4, whose
 * D3DPIXELSHADERDEF at 0x003E9EA8 decodes to
 *
 *     stage0 RGB in  = 0xC9D120C8   A = T1.rgb   B = C0.a
 *                                   C = 1        D = T0.rgb
 *     stage0 RGB out = 0x00010C00   SUM -> R0, OP field (bits 15..17) = 2
 *                                   = NV097 SHIFTLEFTBY1 = x2
 *     final  ABCD    = 0x00000C00   out.rgb = R0.rgb
 *
 *          out.rgb = 2 * ( sceneRT.rgb  +  C0.a * blurRT.rgb )
 *
 * That is the same OP field, read the same way, that settles the world's x2
 * (PSRGBOutputs[0] = 0x000100C0 in all six world defs). The present pass is
 * NOT energy conserving: the presented frame is TWICE the render target even
 * when the blur is idle. This is the missing global brightness step the
 * world-render waves measured as a 1.45x..2.0x deficit.
 *
 * C0 is pushed by FUN_0034E9A0 (SetPixelShaderConstant, register 0) at
 * 0x0003DC89 as (0, 0, 0, alpha) with, at 0x0003DC42..0x0003DC62,
 *
 *     alpha = (s <= 2.0) ? s * 0.5 : 1.0        (2.0 @0x003B1688,
 *                                                0.5 @0x003B1684)
 *
 * where s is the float at blurState+0x54 (the state object handed to
 * FUN_0003DA90 is blurState+0x50, @0x001AE5FC..0x001AE610). The producer of
 * s was NOT found — see B3_BLUR_* GLUE.
 *
 *
 * 4. THE OUTPUT GAMMA RAMP
 * ------------------------
 * FUN_0003C8A0 builds ramp[i] = round((i/255)^0.95 * 255) at
 * 0x0003D3F0..0x0003D484 and installs it with D3DDevice_SetGammaRamp. On
 * hardware that is a scanout transform; in the harness it is a final
 * full-screen pass so that the live window and every glReadPixels capture
 * see the identical image (SDL_SetWindowGammaRamp is a no-op under Wayland
 * and usually under X11, so the live window used to render WITHOUT it).
 */

/* ---------------------------------------------------------------- sky mesh */

/* [C] FUN_00032020: outer loop count 0x21, inner loop count 7, plus the one
 * "skirt" vertex written per ring before the inner loop => 8 rows per ring. */
#define B3_SKY_RINGS      33            /* [C] local_aa0 = 0x21 @0x0003203D   */
#define B3_SKY_ROWS       8             /* [C] 1 skirt + local_ab0 = 7        */
#define B3_SKY_VERTS      (B3_SKY_RINGS * B3_SKY_ROWS)   /* 264              */
/* [C] index loop: 16 columns x 2 halves x 7 quads x 6 indices = 1344 = 0x540,
 * matching the draw count pushed at 0x000327E4. */
#define B3_SKY_INDICES    1344

/* [C] DAT_004D916C = 0.19634954 = 2*pi/32, the ring azimuth step (init at
 * 0x0025EAE0 from 0x003B2144). */
#define B3_SKY_THETA_STEP   0.19634954f
/* [C] DAT_004D9174 = 0.2617994 = pi/12, the dome parameter step (0x0025EAC0
 * from 0x003B18F0); with the 2/pi scale below this makes a = k/6 over k=0..6.*/
#define B3_SKY_T_STEP       0.2617994f
/* [C] literal 0.63661975 = 2/pi @0x00032258, and 0.15915494 = 1/(2*pi)
 * @0x000320A9 (the azimuth -> u0 scale). */
#define B3_SKY_TWO_OVER_PI  0.63661975f
#define B3_SKY_INV_TWO_PI   0.15915494f
/* [C] literal -0.25 @0x0003206A: the skirt drops 0.25 below the horizon. */
#define B3_SKY_SKIRT        (-0.25f)
/* [C] literal 1.1764705 = 1/0.85 @0x000321FE, the second texcoord's v scale. */
#define B3_SKY_TC1_VSCALE   1.1764705f

/* [C] the two (vLow, vHigh) pairs selected by FUN_00032020's `param_1`:
 *   param_1 == 1 (SKY dome, s=+1): vLow DAT_004D7058 = 0.765625  (= 24.5/32)
 *                                  vHigh DAT_004D9154 = 0.015625 (=  0.5/32)
 *   param_1 == 0 (GROUND dome, s=-1): vLow DAT_004D9164 = 0.765625
 *                                     vHigh DAT_004D9168 = 1.015625 (= 32.5/32)
 * i.e. the sky half owns rows 0..24 of the 32-row gradient LUT and the ground
 * half owns rows 24..32. (inits at 0x0025EA40/60/80/A0.) */
#define B3_SKY_V_LOW        0.765625f
#define B3_SKY_V_HIGH       0.015625f
#define B3_GND_V_LOW        0.765625f
#define B3_GND_V_HIGH       1.015625f

/* [C] the vertex colour every dome vertex gets: 0x0000FF00 (stored as the
 * raw dword at +0x0C, seen as the denormal 9.14768e-41 in the decompiler). */
#define B3_SKY_VERTEX_COLOR 0x0000FF00u

/* [C] 0x000325AB..0x000325BB: the dome's uniform scale is
 * DAT_004D67E0 - 1000.0 (literal 1000.0 at 0x003B16CC). DAT_004D67E0 is the
 * view far clip [S] — FUN_0002ECC0 stamps 10000.0 as the far plane. */
#define B3_SKY_FAR_MARGIN   1000.0f

/* [C] FUN_001A9C50 @0x001AA00A creates the runtime gradient LUT 64 wide by
 * 32 tall; FUN_001891F0 renders into rows 0..24 of it each frame. */
#define B3_SKY_LUT_W        64
#define B3_SKY_LUT_H        32

/* [C] 0x001892E1..0x001892FB (FUN_001891F0, the LUT pass-1 column selector):
 *   u = progress * 0.46875 + 0.015625     (= texel 0.5 + 15*progress of 32)
 * with progress = (DAT_0073B5EC + DAT_0073B5E8) / *(DAT_0073A164 + 8) [S: the
 * identity of that ratio is not pinned; it is 0 -> u = 0.015625 when
 * DAT_0073A164 is null, i.e. the leftmost column). */
#define B3_SKY_LUT_U_BASE   0.015625f
#define B3_SKY_LUT_U_SPAN   0.46875f

/* [C] The dome's pixel-shader constant C0, set by FUN_00032580 @0x000327A7
 * (0x0034E9A0 = D3DDevice_SetPixelShaderConstant, Register=0, Count=1) from
 * the literals 0.5 @0x003B1684 and 1.0 @0x003B168C:
 *
 *     C0 = (0.5, 0.5, 0.5, 1.0)
 *
 * and the shader it feeds is the D3DPIXELSHADERDEF at 0x003E9B08 (renderer
 * slot this+0x44C = DAT_004D65BC; base 0x004D6170 — see burnout3_postfx.c),
 * which carries NO output shift on either stage:
 *
 *     out.rgb = C0.rgb*T1.rgb * C0.a*T1.a  +  T0.rgb * (1 - C0.a*T1.a)
 *             = 0.5*clouds.rgb*clouds.a    +  gradient.rgb * (1 - clouds.a)
 *
 * So the cloud sheet is HALVED, not doubled: these are the multipliers the
 * cloud pass must hand GL_MODULATE. */
#define B3_SKY_CLOUD_C0_RGB 0.5f
#define B3_SKY_CLOUD_C0_A   1.0f

/* ===================================================================
 * THE RUNTIME GRADIENT LUT — FUN_001891F0 (0x001891F0), all THREE passes
 * ===================================================================
 *
 * The dome's stage-0 texture is NOT the 32x32 `gradients` sheet. It is a
 * 64x32 A8R8G8B8 render target (FUN_001A9C50 @0x001A9FFD:
 * CreateTexture(0x40, 0x20, levels 1, format 6) -> DAT_004A1D04) that
 * FUN_001891F0 REBUILDS EVERY FRAME out of that sheet with three 2D blit
 * passes. Until this wave the port implemented pass 1 only, by binding the
 * sheet directly at a constant u — which is exact for pass 1 alone (pass 1
 * leaves the LUT horizontally uniform) but drops passes 2 and 3 entirely.
 *
 * All three passes go through the same quad blitter FUN_001C7430
 * (dest rects at EAX, {x0,y0,x1,y1}; src rects as the 3rd stack arg,
 * {u0,v0,u1,v1}; the count is the 2nd), and each is preceded by
 * FUN_001C82E0(ESI = mode) — a BLEND-STATE selector that writes four render
 * states from four tables built by FUN_001C7150:
 *
 *   RS 0x3E SRCBLEND    <- (&DAT_004A1A90)[mode]
 *   RS 0x3F DESTBLEND   <- (&DAT_004A1AB0)[mode]
 *   RS 0x4A BLENDOP     <- (&DAT_004A1B00)[mode]
 *   RS 0x43 COLORWRITE  <- (&DAT_004A1B34)[mode]
 *
 * (the four push-buffer slots 0x0075D598/59C/5C8/5AC sit at 0x0075D58C +
 * 4*(rs - 0x3B), and 0x0075D58C/590 are the RS 0x3B/0x3C pair RE_POSTFX 3.2
 * already pinned.) The table values are literal NV2A enums:
 *
 *   mode 3  src ONE(1)        dst ZERO(0)      ADD(0x8006)  mask 0x00010101 RGB
 *   mode 7  src ONE(1)        dst ZERO(0)      ADD(0x8006)  mask 0x01000000 A
 *   mode 4  src DST_ALPHA(0x304) dst INVDSTALPHA(0x305) ADD  mask 0x00010101 RGB
 *
 * so the three passes are:
 *
 *   pass 1 (mode 3, RGB only, replace)
 *       LUT.rgb := blit * gradients[ column(progress) ] stretched over 64x32
 *   pass 2 (mode 7, ALPHA only, replace) — 13 quads
 *       LUT.a   := gradients.a sampled in (azimuth, elevation)-RELATIVE-TO-SUN
 *   pass 3 (mode 4, RGB only, src*dst.a + dst*(1-dst.a))
 *       LUT.rgb := lerp( LUT.rgb, blit * gradients[ column+0.5 ], LUT.a )
 *
 * i.e. the sheet's LEFT half is the base sky column over time of day, its
 * RIGHT half (the same column + 0.5 of the width) is the SUN-GLOW colour for
 * that same moment, and its ALPHA CHANNEL is a 2D halo mask that says how
 * much glow each (azimuth, elevation) gets. The port now does all three.
 *
 * CORROBORATION [C-grade, from the art]: pass 2 samples the sheet at
 * normalised u = 15.5 (fract 0.5 -> texel 15.5) and v = 12/32 (texel 11.5)
 * exactly at the sun's own azimuth and elevation. The alpha channel of
 * US_C3_V1's `gradients` is a radial blob whose peak, 252, sits at
 * (col 15..16, row 11..12) — the bilinear tap of that very sample. The blob
 * is what makes this a sun halo and it is centred where the recovered
 * mapping says the sun is.
 */

/* [C] the blit quads' vertex colour. FUN_001891F0 stacks (0.5,0.5,0.5,1.0) at
 * ESP+0x30..0x3C (0x0018920C..0x00189227 from 0x003B1684 / 0x003B168C), hands
 * &that to every FUN_001C7430 call in ECX, and FUN_001C6920 packs it into the
 * D3DCOLOR every blit vertex carries:
 *   ((int)(c[3]*255)<<8 | (int)(c[0]*255))<<8 | (int)(c[1]*255))<<8
 *      | (int)(c[2]*255)     == 0xFF808080
 * [S] that the 2D blitter's combiner MODULATES texture by that colour, i.e.
 * that the LUT ends up at HALF the sheet — the blitter's own shader was not
 * decoded (FUN_001C8470(3) selects it out of two more tables). It is what the
 * references measure: this halving is what B3_SKY_RT_GAIN was standing in for
 * (see below). B3_POSTFX_SKYBLIT=<f> overrides at runtime. */
#define B3_SKY_LUT_BLIT_RGB 0.5f
#define B3_SKY_LUT_BLIT_A   1.0f

/* [C] pass 1 / pass 3's source v span: 0.015625 (0x003B1A90) .. 1.015625
 * (0x003B1F50) over dest rows 0..32 (0x0035BF1C = 64.0 is the dest x1,
 * 0x003B17C8 = 32.0 the dest y1). Sampled at dest texel centres this is a
 * 50/50 average of source rows y and y+1 — a 2-tap vertical prefilter that
 * halves the LUT's row-to-row second differences. */
#define B3_SKY_LUT_SRC_V0   0.015625f
#define B3_SKY_LUT_SRC_V1   1.015625f

/* [C] pass 3's column: ESP+0x14 (pass 1's column) + 0.5, at 0x001895C7. */
#define B3_SKY_GLOW_U_OFFSET 0.5f

/* [C] pass 2's azimuth span, 0x0018938B..0x001893AB:
 *     u0 = 15.5 - az      (0x003B0438 = 15.5)
 *     u1 = u0 + 1.0       (0x003B168C = 1.0)
 * over dest x 0..64, i.e. exactly ONE wrap of the sheet, positioned so that
 * the dome azimuth == the sun azimuth lands on normalised u 15.5 == texel
 * 15.5 of 32. */
#define B3_SKY_GLOW_U_BASE   15.5f
#define B3_SKY_GLOW_U_SPAN   1.0f

/* [C] pass 2's elevation warp, 0x001893BC..0x001893F7 and the loop at
 * 0x00189440..0x00189501:
 *     a_row = 1 - sqrt(y / 24)                 (0x003B183C = 1/24)
 *     v     = min( (0.5 - (a_row - X)) * 24, 24 )       texels
 *     v_src = v * 0.03125                      (0x003B1C30 = 1/32)
 * where X is the sun's own elevation parameter out of FUN_00189660. a_row is
 * the dome parameter `a` of LUT row y (row r <-> yhat = (24.5-r)/24 and
 * a = 1 - sqrt(1-yhat)), so v == 12 exactly when the row is at the sun's
 * elevation, and +-0.5 of `a` maps to the full 24-texel span.
 * The dest is 12 quads of 2 rows each covering y 0..24 (EDI += 2, EAX < 0xBC),
 * so the warp is piecewise linear in 12 segments, then ONE more quad for
 * y 24..32. */
#define B3_SKY_GLOW_V_CENTER 0.5f
#define B3_SKY_GLOW_V_SCALE  24.0f
#define B3_SKY_GLOW_BANDS    12
#define B3_SKY_GLOW_SKY_ROWS 24

/* [C] the 13th quad's source v (dest rows 24..32, the GROUND hemisphere):
 * DAT_0060E1C0 / DAT_0060E1C4 = env+0x180 / env+0x184, and the environment
 * record IS the global at 0x0060E040 (FUN_001AE340 @0x001AE37A passes it to
 * FUN_001891F0; 0x0060E170 = env+0x130 is the screen index it writes at
 * 0x001AE37F). Both are 0.0 in every shipped enviro.dat, so the ground half
 * of the LUT gets source row 0 — alpha 0 — no glow below the horizon. */
#define B3_SKY_GLOW_GND_V0   0.0f
#define B3_SKY_GLOW_GND_V1   0.0f

/* [C] FUN_00189660 (0x00189660): direction -> (azimuth turns, elevation
 * parameter). The azimuth is the classic 8-octant atan2 built on
 * 0.15915494 = 1/(2*pi), 0 at +X and increasing toward +Z — THE SAME
 * convention as the dome's own tc0.u = theta/(2*pi) (FUN_00032020 emits
 * pos = (cos t, y, sin t)), so the LUT column and the dome azimuth line up
 * with no extra offset. The elevation parameter is
 *     y >= 0 :  1 - sqrt(1 - y)
 *     y <  0 :  sqrt(1 + y) - 1
 * which is exactly the dome's `a` for a unit direction.
 *
 * The direction comes from env+0xB0 + 0x40*screen, which the loader fills
 * from env+0x80 (FUN_001888F0 @0x00188B40..0x00188B95, two 64-byte screen
 * blocks). env+0x80 is a UNIT vector with NEGATIVE y on all 40 shipped
 * tracks — the light's direction of TRAVEL, not the direction to the sun.
 * [S] the port negates it: FUN_0019A3E0 @0x0019A5E9 reads the same block and
 * flips all four signs before use, and with the unflipped vector the whole
 * glow falls off the sampled range (v < 0 everywhere the dome can reach), so
 * the halo the art carries would never be drawn. */

/* SKY RENDER-TARGET GAIN -- was TUNED (2026-08-13), now SUBSUMED.
 *
 * It used to be 0.57: one multiplier on the dome's finished colour, fitted so
 * the sky met the recovered present x2 (measured, our dome rendered about
 * (72, 89, 138) where retail's RENDER TARGET holds about (43, 51, 75)).
 * The SKY-LUT wave found what it was standing in for: the LUT is not the
 * sheet, it is a blit of the sheet through a 0x FF808080 vertex colour
 * (B3_SKY_LUT_BLIT_RGB above) with a sun-glow lerp on top, and 0.5 x that
 * lerp reproduces both the level AND the hue. So the recovered law replaces
 * the fit and this ships at IDENTITY. B3_POSTFX_SKYGAIN=<f> still overrides
 * it, so 0.57 is one environment variable away for A/B. */
#define B3_SKY_RT_GAIN 1.0f

/* One dome vertex. MUST be 32 bytes — the game's stride, pushed as 0x20 at
 * 0x00032633 (SetStreamSource) and implied by 0x41C0 / 2 / 0x20 = 263. */
typedef struct {
    float        pos[3];   /* +0x00 unit dome position                       */
    unsigned int color;    /* +0x0C 0x0000FF00                               */
    float        tc0[2];   /* +0x10 (azimuth/2pi, gradient-LUT v)            */
    float        tc1[2];   /* +0x18 (azimuth/pi, 1 - yhat/0.85) — sky only   */
} B3SkyVertex;

/* Build one dome exactly as FUN_00032020 does.
 *   sky != 0 -> param_1 == 1 (upper hemisphere, s = +1, writes tc1)
 *   sky == 0 -> param_1 == 0 (lower hemisphere, s = -1, tc1 left zero)
 * `verts` must hold B3_SKY_VERTS, `idx` B3_SKY_INDICES (may be NULL).
 * Returns the vertex count. GL-free: usable from a validator probe. */
int b3_sky_build(int sky, B3SkyVertex *verts, unsigned short *idx);

/* ------------------------------------------------------- the gradient LUT */

/* FUN_00189660: `dir` (need not be unit; it is normalised here as retail's
 * caller guarantees) -> `az` in turns [0,1) and `elev` = the elevation
 * parameter. Pass the direction TOWARD the sun (see the note above). */
void b3_sky_sun_angles(const float dir[3], float *az, float *elev);

/* Pass 2's elevation warp for LUT row `y`, in SOURCE TEXELS, upper-clamped at
 * 24 exactly as 0x001893EF does. `sun_elev` is b3_sky_sun_angles()'s second
 * output. Exposed so the validator can execute it. */
float b3_sky_glow_v_texels(float y, float sun_elev);

/* Rebuild the 64x32 gradient LUT the dome samples: FUN_001891F0's three
 * passes, run on the CPU against the extracted 32x32 `gradients` sheet.
 *   src / sw / sh   the sheet, 8-bit RGBA, row-major, tightly packed
 *   progress        pass 1/3's column selector (0..1)
 *   sun_az/sun_elev b3_sky_sun_angles() of the TO-SUN direction
 *   have_sun        0 -> passes 2 and 3 are skipped (LUT.a = 0), which is
 *                   exactly what the port did before this wave
 *   out             B3_SKY_LUT_W * B3_SKY_LUT_H * 4 bytes, RGBA
 * GL-free so tools/validate_postfx.py can run it as a probe. */
void b3_sky_build_lut(const unsigned char *src, int sw, int sh,
                      float progress, float sun_az, float sun_elev,
                      int have_sun, unsigned char *out);

/* Override the [S] blit-colour RGB used by b3_sky_build_lut (default
 * B3_SKY_LUT_BLIT_RGB). The GL side wires B3_POSTFX_SKYBLIT=<f> to it. */
void b3_sky_lut_set_blit(float f);

/* ------------------------------------------------------------- speed blur */

/* [C] FUN_0002EBE0's two zoom layers. Layer A steps the frame in by 1% per
 * tap (0x003B1758 = 0.99, written to +0x10/+0x14); layer B is the near-unity
 * one (0x003B18B4 = 0.9998999834, written to +0x78/+0x7C). Both layers'
 * centres are (0.5, 0.5) — 0x003B1684 written to +0x00..+0x0C and +0x70/+0x74
 * — i.e. screen centre, and both amounts default to 1.0 (+0x18, +0x98). */
#define B3_BLUR_ZOOM_A      0.99f
#define B3_BLUR_ZOOM_B      0.9998999834f
#define B3_BLUR_CENTER_X    0.5f
#define B3_BLUR_CENTER_Y    0.5f
#define B3_BLUR_AMOUNT      1.0f

/* ---------------------------------------------- the present composite [C] */

/* [C] PSRGBOutputs[0] = 0x00010C00 in the def at 0x003E9EA8: bits 15..17 (the
 * NV097_SET_COMBINER_COLOR_OCW OP field, mask 0x00038000) hold 2 =
 * SHIFTLEFTBY1. The presented frame is twice the render target. */
#define B3_PRESENT_SHIFT       2.0f
/* [C] 0x0003DC42 COMISS against 2.0 @0x003B1688, 0x0003DC5A MULSS by 0.5
 * @0x003B1684, else 1.0 @0x003B168C: C0.a = min(s, 2) * 0.5. */
#define B3_PRESENT_S_MAX       2.0f
#define B3_PRESENT_S_SCALE     0.5f
/* [C] FUN_0003D890: the scene surface is 640x480 (0x0003D89D/0x0003D8A2) and
 * the blur surface handed to stage 1 is 160x120 (0x0003D8D0/0x0003D8D2), i.e.
 * exactly mip level 2 of the scene. The texcoords the quad carries confirm it:
 * 640.0/480.0 on attribute 9 and 160.0/120.0 on attribute 10 (0x003B1F00,
 * 0x003B1EEC, 0x003A49FC, 0x003A1A00). */
#define B3_PRESENT_RT_W        640
#define B3_PRESENT_RT_H        480
#define B3_PRESENT_BLUR_W      160
#define B3_PRESENT_BLUR_H      120
#define B3_PRESENT_BLUR_LOD    2      /* log2(640/160) == log2(480/120)      */

/* [C] the retail output gamma ramp, FUN_0003C8A0 @0x0003D3F0..0x0003D484:
 * ramp[i] = round((i/255)^0.95 * 255), identical on R/G/B. The exponent is
 * the double at 0x003B20F8. */
#define B3_GAMMA_EXPONENT   0.949999988079071

/* GLUE — everything below is harness-side. The retail strength-vs-speed law
 * lives in the producer of blurState+0x54, which neither this session nor the
 * previous one found, and the mask lookup is a pixel shader in the Graphics
 * .bum bundles, which are not decoded. These numbers were fitted to the xemu
 * captures in "REFERENCE IMAGES/" (0 mph: no blur; 42/52 mph: faint edge
 * streaks; 85/103 mph: strong), NOT recovered. Do not cite them as game data.
 *
 * NOTE what changed with the present composite: b3_postfx_blur_strength() now
 * produces the RECOVERED quantity `s` (the float the game reads at
 * blurState+0x54, whose useful range is 0..2), and the conversion from s to
 * the combiner constant C0.a is the recovered law b3_postfx_present_alpha().
 * Only the speed -> s mapping is invented now; the composite around it is not. */
#define B3_BLUR_MPH_ON      30.0f   /* GLUE onset                            */
#define B3_BLUR_MPH_FULL   120.0f   /* GLUE saturation                       */
#define B3_BLUR_S_MAX       0.85f   /* GLUE peak s at top speed (of 2.0 max)  */
#define B3_BLUR_BOOST_GAIN  0.45f   /* GLUE extra from the recovered ramp     */
#define B3_BLUR_TAPS       16       /* GLUE tap count -- 0.99^16 = 0.851, i.e. a */
                                    /* 15% edge displacement, which is what   */
                                    /* the 85/103 mph captures show           */
#define B3_BLUR_MASK_R0     0.25f   /* GLUE mask inner radius (sharp centre)  */
#define B3_BLUR_MASK_POW    1.5f    /* GLUE mask falloff exponent             */

/* ------------------------------------------------------------------- API */

/* Non-GL setup. Safe to call before the GL context exists. */
void b3_postfx_init(void);

/* Point the module at the extracted art (default "build/postfx") and the
 * track tag used by tools/extract_postfx_art.py ("US_C3_V1"). Call before
 * b3_postfx_gl_init(); both may be NULL to keep the defaults. */
void b3_postfx_set_art(const char *dir, const char *track_tag);

/* Upload the dome textures and build the dome display data. Needs a current
 * GL context. Returns 1 when the sky can draw, 0 when the art is missing (in
 * which case b3_postfx_sky_draw() is a no-op and the harness keeps its clear).*/
int  b3_postfx_gl_init(void);

/* Draw the sky. Call FIRST in the frame, after the projection/view matrices
 * are loaded and before any world geometry. `eye` is the camera position in
 * harness world space, `far_clip` the projection far plane, `progress` the
 * 0..1 gradient-LUT column selector. Writes no depth and restores state. */
void b3_postfx_sky_draw(const float eye[3], float far_clip, float progress);

/* The present composite (FUN_0003DA90): re-draw the finished 3D frame as
 *     out = 2 * (frame + C0.a * blur(frame))
 * Call LAST, after the world and the cars but BEFORE the HUD — retail draws
 * its HUD after FUN_0003DA90 too, so the HUD is not doubled. `boost_ramp` is
 * racecar+0x11AC (0..2), the same quantity the recovered FOV law consumes;
 * pass 0 when it is unavailable.
 * B3_POSTFX_PRESENT=0 restores the pre-recovery behaviour (no x2, blur as an
 * alpha-over); B3_POSTFX_BLUR=<k> scales the GLUE strength (0 = no blur). */
void b3_postfx_blur(int w, int h, float speed_mph, float boost_ramp,
                    float real_dt);

/* The retail output gamma ramp as a full-screen pass over the finished back
 * buffer. Call as the LAST thing in the frame — after the HUD, before both
 * glReadPixels captures and before SDL_GL_SwapWindow — so the live window and
 * every capture see the identical transform. Returns 1 if the ramp was
 * applied in GL, 0 if it could not be (no GLSL, or B3_NOGAMMA=1), in which
 * case the caller may fall back to a software table on the readback. */
int b3_postfx_gamma(int w, int h);

/* Introspection for tools/validate_postfx.py's compiled probe. */
float b3_postfx_blur_strength(float speed_mph, float boost_ramp);
/* [C] C0.a = min(s, 2.0) * 0.5  (0x0003DC42..0x0003DC62). */
float b3_postfx_present_alpha(float s);
/* [C] ramp[i] = round((i/255)^0.95 * 255); fills 256 bytes. */
void  b3_postfx_gamma_table(unsigned char *ramp256);

/* Mirror a `w` x `h` RGBA8 buffer top-to-bottom IN PLACE -- the bottom-up to
 * top-down flip every glReadPixels capture path needs before SDL_SaveBMP.
 *
 * This exists because the three capture paths each open-coded the flip through
 * a fixed 8192-byte scratch row and CLAMPED the copy length to it, so any
 * window wider than 2048 px left the tail of every row unmirrored: the
 * window-drag-resize corruption in build/debug_dump_035.png (3706 px wide,
 * seam at 8192/4/3706 = 0.553 of the width). Width-agnostic and allocation-
 * free; GL-free so the validator can execute it. See burnout3_postfx.c 2b. */
void  b3_postfx_flip_rows(unsigned char *px, int w, int h);

#endif
