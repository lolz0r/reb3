/* Crash / slam / takedown / boost sound effects -- see burnout3_sfx.h and
 * docs/RE_SFX.md.
 *
 * PROVENANCE.  Everything in g_defs below is recovered, not invented:
 *
 *   wave names   the 12-char base-40 constants the emitters load and hand to
 *                the wave-dictionary lookup 0x001C99D0.  Every one was found
 *                by decoding 8-byte aligned base-40 values in the image and
 *                matching them against .text immediates
 *                (tools/validate_sfx.py re-derives the whole list).
 *   gain/pitch   the six-float tuple each emitter reads out of the racecar
 *                audio object; the object is built by executing the real
 *                initialiser FUN_0014A710 under Unicorn
 *                (tools/emulate_sfx_params.py), and each emitter's response
 *                curve is confirmed by executing the emitter itself and
 *                capturing its call into PlaySound3D 0x001CD8D0
 *                (tools/emulate_sfx.py).
 *   cooldowns    the byte the emitter stamps into the object before playing
 *                (offsets 0x8C3..0x8D0 / 0x50 / 0x54).
 *   files        the shipped waves the base name selects, by the
 *                <base><index><count> rule the modulo-1600 lookup implies.
 *
 * Anything that could NOT be pinned is marked B3_SFX_S / B3_SFX_Q with the
 * reason in docs/RE_SFX.md; nothing here is a plausible-sounding guess.
 *
 * The mixer below is GLUE -- original harness code, not decompiled.  The
 * game's own voice manager (0x001CD8D0 / RwaVoice) is a full 3D DirectSound
 * path that has no meaning outside the Xbox; this reproduces only what the
 * event law asks for: gain, playback rate, distance roll-off, looping.
 */
#include "burnout3_sfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* 1. The recovered event table                                            */
/* ====================================================================== */

/* Bank directories, as tools/extract_awd.py names them.
 *
 *  crashmod  [C] the emitters' bank pointer is racecar-audio +0x880, filled
 *                at 0x0014B6DD by loading the string at 0x003AEDDC,
 *                "sound\crashmod.awd".
 *  single    [S] SLAM/SHUNT/BOOSTGAIN/BOOSTLOSS take their bank from the
 *                mode object (+0x78 / the 0x00411560 singleton).  The four
 *                mode banks are the strings at 0x003AE17E..0x003AE1AE
 *                (elim / crash / roadrage / single); a single race is
 *                "sound\single.awd".  The bank POINTER was not traced to
 *                that string, hence [S] -- but all four banks hold
 *                byte-identical slam/shunt/boost waves, so the choice is
 *                not audible.
 *  generic   [S] BOOSTLOOP / STATICPASS exist only in sound\generic.awd.
 */
#define BK_CRASHMOD "awd_crashmod"
#define BK_SINGLE   "awd_single"
#define BK_GENERIC  "awd_generic"
/* The loaded TRACK bank -- the surface loops take theirs from DAT_00411E48
 * (`mov ecx,ds:0x411e48 ; push ecx` @0x00136AFB), which is the per-track
 * .awd the level loader installs; the shipped banks are one directory per
 * track id (awd_US_C3_V1 = Silver Lake, the track build/collision.bin is
 * extracted from).  B3_SFX_TRACK_BANK overrides it.                   [C] */
#define BK_TRACK    "awd_US_C3_V1"

static const B3SfxDef g_defs[B3_SFX_COUNT] = {
/* --- car vs car -------------------------------------------------------- */
[B3_SFX_IMPACT_NUDGE] = {
    "impact/nudge", "IMPACTNUDG", "FUN_0014F3E0", BK_CRASHMOD,
    {"impactnudg13.wav", "impactnudg23.wav", "impactnudg33.wav", NULL},
    700.0f, 4000.0f, 0.25f, 0.8f, 0.7f, 1.1f, 1.0f,
    0x6D0, 20, 20, 1, 0, 1, B3_SFX_C },
[B3_SFX_IMPACT_FATAL] = {
    "impact/fatal", "IMPACTFATA", "FUN_0014F690", BK_CRASHMOD,
    {"impactfata16.wav", "impactfata26.wav", "impactfata36.wav",
     "impactfata46.wav", "impactfata56.wav", "impactfata66.wav", NULL},
    100.0f, 5000.0f, 0.3f, 1.0f, 0.75f, 1.05f, 1.0f,
    0x700, 20, 0, 1, 0, 1, B3_SFX_C },
[B3_SFX_IMPACT_FATAL_CM] = {
    "impact/fatal(crashmode)", "IMPACTFATA", "FUN_0014F130", BK_CRASHMOD,
    {"impactfata16.wav", "impactfata26.wav", "impactfata36.wav",
     "impactfata46.wav", "impactfata56.wav", "impactfata66.wav", NULL},
    2.0f, 10.0f, 0.8f, 1.0f, 0.9f, 1.3f, 1.0f,
    0x6E8, 20, 0, 1, 0, 1, B3_SFX_C },
/* --- car vs world ------------------------------------------------------ */
[B3_SFX_IMPACT_WORLD] = {
    "impact/world", "IMPACTWORL", "FUN_0014EEA0", BK_CRASHMOD,
    {"impactworl14.wav", "impactworl24.wav", "impactworl34.wav",
     "impactworl44.wav", NULL},
    6.0f, 22.0f, 0.2f, 0.6f, 0.8f, 1.1f, 1.0f,
    0x6A0, 20, 20, 1, 0, 1, B3_SFX_C },
/* --- glass ------------------------------------------------------------- */
[B3_SFX_GLASS_FRONT] = {
    "glass/windscreen", "GLASSFRONT", "FUN_0014D5F0", BK_CRASHMOD,
    {"glassfront14.wav", "glassfront24.wav", "glassfront34.wav",
     "glassfront44.wav", NULL},
    0.01f, 0.015f, 0.5f, 0.8f, 0.8f, 1.2f, 1.0f,
    0x598, 30, 0, 0, 0, 0, B3_SFX_C },
[B3_SFX_GLASS_SIDES] = {
    "glass/sides", "GLASSSIDES", "FUN_0014D8A0", BK_CRASHMOD,
    {"glasssides13.wav", "glasssides23.wav", "glasssides33.wav", NULL},
    0.02f, 0.4f, 0.8f, 1.0f, 0.8f, 1.2f, 1.0f,
    0x5A0, 30, 0, 0, 0, 0, B3_SFX_C },
[B3_SFX_GLASS_WINDS] = {
    "glass/rear", "GLASSWINDS", "FUN_0014DB50", BK_CRASHMOD,
    {"glasswinds14.wav", "glasswinds24.wav", "glasswinds34.wav",
     "glasswinds44.wav", NULL},
    0.04f, 0.6f, 0.8f, 1.0f, 0.8f, 1.3f, 1.0f,
    0x59C, 30, 0, 0, 0, 0, B3_SFX_C },
/* --- body panels ------------------------------------------------------- */
[B3_SFX_PANEL_L_DEFORM] = {
    "panel/large deform", "CARPARTLDE", "FUN_0014EB00", BK_CRASHMOD,
    {"carpartlde12.wav", "carpartlde22.wav", NULL},
    0.035f, 0.4f, 0.8f, 1.0f, 0.7f, 1.2f, 1.0f,
    0x5E0, 4, 0, 1, 0, 1, B3_SFX_C },
[B3_SFX_PANEL_M_DEFORM] = {
    "panel/medium deform", "CARPARTMDE", "FUN_0014ECB0", BK_CRASHMOD,
    {"carpartmde12.wav", "carpartmde22.wav", NULL},
    0.05f, 0.3f, 0.6f, 1.0f, 0.8f, 1.2f, 1.0f,
    0x5F8, 7, 0, 1, 0, 1, B3_SFX_C },
[B3_SFX_PANEL_L_REMOVE] = {
    "panel/large detach", "CARPARTLRE", "FUN_0014FC80", BK_CRASHMOD,
    {"carpartlre12.wav", "carpartlre22.wav", NULL},
    99.0f, 100.0f, 0.12f, 0.12f, 0.8f, 1.2f, 1.0f,
    0x610, 4, 0, 1, 0, 1, B3_SFX_C },
[B3_SFX_PANEL_M_REMOVE] = {
    "panel/medium detach", "CARPARTMRE", "FUN_0014FE60", BK_CRASHMOD,
    {"carpartmre12.wav", "carpartmre22.wav", NULL},
    99.0f, 100.0f, 0.5f, 0.5f, 0.8f, 1.2f, 1.0f,
    0x628, 7, 0, 1, 0, 1, B3_SFX_C },
[B3_SFX_PANEL_L_PROP] = {
    "panel/large vs world", "CARPARTLPR", "FUN_00150040", BK_CRASHMOD,
    {"carpartlpr12.wav", "carpartlpr22.wav", NULL},
    800.0f, 1500.0f, 0.2f, 1.0f, 0.8f, 1.1f, 0.5f,
    0x640, 15, 0, 1, 0, 1, B3_SFX_C },
[B3_SFX_PANEL_M_PROP] = {
    "panel/medium vs world", "CARPARTMPR", "FUN_00150260", BK_CRASHMOD,
    {"carpartmpr12.wav", "carpartmpr22.wav", NULL},
    800.0f, 5000.0f, 0.2f, 1.0f, 0.8f, 1.1f, 0.5f,
    0x660, 10, 0, 1, 0, 1, B3_SFX_C },
[B3_SFX_PANEL_W_PROP] = {
    "panel/wheel vs world", "CARPARTWPR", "FUN_00150480", BK_CRASHMOD,
    {"carpartwpr11.wav", NULL},
    1000.0f, 2500.0f, 0.7f, 1.0f, 0.8f, 1.2f, 1.0f,
    0x680, 5, 0, 1, 0, 1, B3_SFX_C },
/* --- takedown slam / shunt --------------------------------------------- */
/* FUN_00140610 / FUN_00140480 do NOT use the impulse tuple: they play at a
 * fixed gain read from the mode object (+0x5C), halved when the other party
 * is not a racecar, at the wave's native rate (no FMUL before the FSTP at
 * 0x0014071F).  The +0x5C value lives in BSS and is written by code that was
 * not traced, so the level itself is [S]: 1.0 here, with the documented x0.5
 * branch exposed as gain_scale.                                            */
[B3_SFX_SLAM] = {
    "takedown/slam", "SLAM______", "FUN_00140610", BK_SINGLE,
    {"slam______12.wav", "slam______22.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 30, 0, 0, 0, 0, B3_SFX_S },
[B3_SFX_SHUNT] = {
    "takedown/shunt", "SHUNT_____", "FUN_00140480", BK_SINGLE,
    {"shunt_____12.wav", "shunt_____22.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 30, 0, 0, 0, 0, B3_SFX_S },
/* --- boost -------------------------------------------------------------- */
[B3_SFX_BOOST_GAIN] = {
    "boost/gain", "BOOSTGAIN", "FUN_00140DF0", BK_SINGLE,
    {"boostgain.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,   /* gain 1.0 @0x003EC26C [C] */
    -1, 0, 0, 0, 0, 0, B3_SFX_C },
[B3_SFX_BOOST_LOSS] = {
    "boost/loss", "BOOSTLOSS", "FUN_00140EF0", BK_SINGLE,
    {"boostloss.wav", NULL},
    0.0f, 0.0f, 0.8f, 0.8f, 1.0f, 1.0f, 1.0f,   /* gain 0.8 @0x003EC268 [C] */
    -1, 0, 0, 0, 0, 0, B3_SFX_C },
[B3_SFX_BOOST_LOOP] = {
    "boost/loop", "BOOSTLOOP", "FUN_00141D20", BK_GENERIC,
    {"boostloop.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,   /* gain 1.0 @0x00141DD9 [C] */
    -1, 0, 0, 0, 1, 0, B3_SFX_S },
/* The racecar's own boost attack/release.  Unlike every row above, these two
 * names are NOT packed base-40 constants: FUN_00136F80 loads the ASCII
 * literals "BoostIn" (0x003AD3F4 @0x00136FE7) and "BoostOut" (0x003AD3E8
 * @0x0013700F) and packs them at runtime with FUN_001AEAA0 before the same
 * FUN_001C9E50 bank lookup, so the `wave` field here is the ASCII literal and
 * validate_sfx section 1 checks it as such (mixed case = ASCII).  The same
 * function loads "BoostLoop" (0x003AD404) -- the identical boostloop.wav the
 * mode path above plays -- and "fire" (0x003AD3FC), which is the flame layer
 * B3_SFX_FIRE already carries.  Gains are the [?] of the .h header note; 1.0
 * is FUN_00141D20's [C] level for the loop and is used throughout.        */
[B3_SFX_BOOST_IN] = {
    "boost/in", "BoostIn", "FUN_00136F80", BK_GENERIC,
    {"boostin.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 0, 0, B3_SFX_C },
[B3_SFX_BOOST_OUT] = {
    "boost/out", "BoostOut", "FUN_00136F80", BK_GENERIC,
    {"boostout.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 0, 0, B3_SFX_C },
/* --- scraping ----------------------------------------------------------- */
/* FUN_001521C0 starts both scrape loops together and then crossfades them
 * per frame; the crossfade weights are computed in the caller FUN_0014B600's
 * update case, not here, so the LEVELS are [S] and the harness drives them
 * with the caller's magnitude directly.                                     */
[B3_SFX_SCRAPE_LO] = {
    "scrape/low", "CARSCRAPLO", "FUN_001521C0", BK_CRASHMOD,
    {"carscraplo.wav", NULL},
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_S },
[B3_SFX_SCRAPE_HI] = {
    "scrape/high", "CARSCRAPHI", "FUN_001521C0", BK_CRASHMOD,
    {"carscraphi.wav", NULL},
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_S },
/* --- misc one-shots ----------------------------------------------------- */
/* KURBBOUNCE/AIRRAM/EXP have no impulse tuple in the object (the emitters
 * read no +0x5xx/+0x6xx pair); they play at a fixed gain. FUN_00150B90 also
 * refuses to run in emulation without a live viewport list, so its gain was
 * not captured -- 1.0 here, marked [S].                                     */
[B3_SFX_KERB] = {
    "kerb", "KURBBOUNCE", "FUN_00150B90", BK_CRASHMOD,
    {"kurbbounce14.wav", "kurbbounce24.wav", "kurbbounce34.wav",
     "kurbbounce44.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 5, 0, 0, 0, 0, B3_SFX_S },
[B3_SFX_AIRRAM] = {
    "air ram", "AIRRAM", "FUN_00151490", BK_CRASHMOD,
    {"airram.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 0, 0, B3_SFX_S },
[B3_SFX_EXPLOSION] = {
    "explosion", "EXP", "FUN_001516C0", BK_CRASHMOD,
    {"exp.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 0, 0, B3_SFX_S },
[B3_SFX_FIRE] = {
    "fire", "FIRE", "FUN_0014B600", BK_CRASHMOD,
    {"fire.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_S },
[B3_SFX_STEAM] = {
    "steam", "STEAMRADAT", "FUN_00151B70", BK_CRASHMOD,
    {"steamradat12.wav", "steamradat22.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_S },
[B3_SFX_CAR_ALARM] = {
    "car alarm", "CARALARMLP", "0x0015204D", BK_CRASHMOD,
    {"caralarmlp14.wav", "caralarmlp24.wav", "caralarmlp34.wav",
     "caralarmlp44.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_S },
/* FUN_00156300(gain, x) -- the SCENERY whoosh (tunnels, overhangs, walls),
 * NOT the traffic pass.  Upgraded from [S] to [C] 2026-08-13:
 *   gain  = arg1, and its only driver FUN_00155FD0 passes 1.0
 *   pitch = [0x005A8A40]*arg2 + [0x003A5600]   @0x001563B7..0x001563E9,
 *           [0x005A8A40] = 0.4 (its single writer is the static-init
 *           thunk 0x002BEA80 reading 0x003B2348), [0x003A5600] = 0.8,
 *           arg2 = DAT_0040E310 + 1.0 and DAT_0040E310 has no writer
 *           at all -> 0.0.  So the rate is a FIXED 1.2x native.
 *   re-arm  0.5 s = 30 ticks (FUN_00155FD0)
 *   the block's max distance is 64.0 (0x0035BF1C), not the usual 50 */
[B3_SFX_NEAR_MISS] = {
    "scenery pass (tunnel/wall)", "STATICPASS", "FUN_00156300", BK_GENERIC,
    {"staticpass14.wav", "staticpass24.wav", "staticpass34.wav",
     "staticpass44.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.2f, 1.2f, 1.0f,
    -1, 30, 0, 0, 0, 0, B3_SFX_C },
/* --- the passing-traffic whoosh (the law is in burnout3_sfx.h) -------- *
 * min/max impulse are the Min/Max Speed window 0x0040FBF0/0x0040FBF4; the
 * gain and pitch ends are BOTH the max because retail's own gate pins its
 * t at 1, so no curve is invented here.  gain_scale is the Data/vdb.xml
 * pass scalar.  pitch_var stays 0 -- this emitter's +/-0.1 jitter is
 * ADDITIVE, and b3_sfx_traffic_pass applies it itself. */
[B3_SFX_TRAFFIC_PASS_BIG] = {
    "traffic pass/big", "bstpsl0%i", "FUN_00146530", BK_GENERIC,
    {"bstpsl01.wav", "bstpsl02.wav", "bstpsl03.wav", "bstpsl04.wav", NULL},
    B3_SFX_PASS_MIN_SPEED_RETAIL, B3_SFX_PASS_MAX_SPEED,
    B3_SFX_PASS_MAX_VOLUME, B3_SFX_PASS_MAX_VOLUME,
    B3_SFX_PASS_MAX_PITCH,  B3_SFX_PASS_MAX_PITCH,
    B3_SFX_PASS_BIG_SCALAR,
    -1, B3_SFX_PASS_COOLDOWN, 0, 1, 0, 0, B3_SFX_C },
[B3_SFX_TRAFFIC_PASS_SMALL] = {
    "traffic pass/little", "bstpss0%i", "FUN_00146530", BK_GENERIC,
    {"bstpss01.wav", "bstpss02.wav", "bstpss03.wav", "bstpss04.wav", NULL},
    B3_SFX_PASS_MIN_SPEED_RETAIL, B3_SFX_PASS_MAX_SPEED,
    B3_SFX_PASS_MAX_VOLUME, B3_SFX_PASS_MAX_VOLUME,
    B3_SFX_PASS_MAX_PITCH,  B3_SFX_PASS_MAX_PITCH,
    B3_SFX_PASS_LITTLE_SCALAR,
    -1, B3_SFX_PASS_COOLDOWN, 0, 1, 0, 0, B3_SFX_C },
/* --- the driving-time loops (the laws are in burnout3_sfx.h) ---------- *
 * These five carry no impulse tuple: FUN_00136610 computes gain and pitch
 * from the surface curve tables and the slip term, so the rows below only
 * bind the WAVE (the base-40 name at 0x0039BBF8[id]) and the bank.  The
 * bank is the loaded TRACK bank (DAT_00411E48 @0x00136AFB), which is why
 * `dir` is the placeholder BK_TRACK the loader rewrites from
 * B3_SFX_TRACK_BANK.                                                  [C] */
[B3_SFX_SURFACE_TAR] = {
    "surface/tar", "TAR", "FUN_00136610", BK_TRACK,
    {"tar.wav", NULL},
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_C },
[B3_SFX_SURFACE_GVL] = {
    "surface/gravel", "GVL", "FUN_00136610", BK_TRACK,
    {"gvl.wav", NULL},
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_C },
[B3_SFX_SURFACE_WOOD] = {
    "surface/wood", "WOOD", "FUN_00136610", BK_TRACK,
    {"wood.wav", NULL},
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_C },
[B3_SFX_SURFACE_METAL] = {
    "surface/metal", "METAL", "FUN_00136610", BK_TRACK,
    {"metal.wav", NULL},
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_C },
[B3_SFX_SURFACE_SNOW] = {
    "surface/snow", "SNOW", "FUN_00136610", BK_TRACK,
    {"snow.wav", NULL},
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_C },
/* The three Sound/Skids samples.  Their names are packed base-40 values
 * held in the ValueDB-backed sample records (0x003EC108 + i*0x48), loaded
 * by FUN_0013DBC0 @0x0013DC4E against the GENERIC bank pointer
 * (`push 0x40b7f4` @0x0013DBD9).  Gain and rate come from the piecewise
 * curves, so no tuple here either.                                    [C] */
[B3_SFX_SKID_1] = {
    "skid/sample1", "CONST01", "FUN_0013DE10", BK_GENERIC,
    {"const01.wav", NULL},
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_C },
[B3_SFX_SKID_2] = {
    "skid/sample2", "CONST02", "FUN_0013DE10", BK_GENERIC,
    {"const02.wav", NULL},
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_C },
[B3_SFX_SKID_3] = {
    "skid/sample3", "CONST03", "FUN_0013DE10", BK_GENERIC,
    {"const03.wav", NULL},
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 1, 0, B3_SFX_C },
/* GEAR______ is bound once at 0x00136059 into obj+0x18C and FUN_00136C50
 * plays it at the wave's native rate with no tuple; the record is fetched
 * from the GENERIC bank list (`mov eax,0x40b7f4` @0x00136073).  The level
 * is not read from any tuple, hence [S] on the gain alone.               */
[B3_SFX_GEAR] = {
    "gear change", "GEAR______", "FUN_00136C50", BK_GENERIC,
    {"gear______11.wav", NULL},
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    -1, 0, 0, 0, 0, 0, B3_SFX_S },
};

/* ====================================================================== */
/* 2. The event law  (the whole "small tap vs huge crash" behaviour)       */
/* ====================================================================== */

static float clamp01(float v)
{
    /* Written in the emitters' own order: MAXSS against 0 then MINSS
     * against 1, so a NaN t collapses the same way the SSE does. */
    if (!(v > 0.0f)) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

/* xorshift; only used for variant choice and the +/-10% pitch variance, both
 * of which the real code drives from its own per-object LCG at +0x520. */
static unsigned g_rng = 0x1234567u;
const B3SfxDef* b3_sfx_def(B3SfxEvent ev)
{
    if ((unsigned)ev >= B3_SFX_COUNT) return NULL;
    return &g_defs[ev];
}

static int def_nvariants(const B3SfxDef* d)
{
    int n = 0;
    while (n < 8 && d->files[n]) n++;
    return n;
}

static unsigned sfx_rand(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

int b3_sfx_resolve(B3SfxEvent ev, float impulse, int variant_sel,
                   B3SfxShot* out)
{
    const B3SfxDef* d = b3_sfx_def(ev);
    if (!d || !out) return 0;
    memset(out, 0, sizeof *out);

    /* COMISS [obj+min] , impulse ; JA <skip>  -- strictly below is silent. */
    if (d->gated && impulse < d->min_impulse) return 0;

    float span = d->max_impulse - d->min_impulse;
    float t = (span != 0.0f) ? (impulse - d->min_impulse) / span : 0.0f;
    t = clamp01(t);

    out->t = t;
    out->gain = (d->min_gain + t * (d->max_gain - d->min_gain)) * d->gain_scale;
    out->pitch = d->min_pitch + t * (d->max_pitch - d->min_pitch);

    int n = def_nvariants(d);
    if (n <= 0) return 0;
    out->variant = (variant_sel >= 0) ? (variant_sel % n)
                                      : (int)(sfx_rand() % (unsigned)n);
    out->file = d->files[out->variant];
    out->play = 1;
    return 1;
}

/* ====================================================================== */
/* 3. Wave loading (minimal RIFF reader -- no SDL, so the module compiles
 *    standalone for tools/validate_sfx.py)                                */
/* ====================================================================== */

typedef struct {
    int16_t* pcm;      /* mono s16                                      */
    unsigned frames;
    int      rate;
    int      loaded;
} B3Wave;

#define MAXWAVES (B3_SFX_COUNT * 8)
static B3Wave g_waves[MAXWAVES];
static int    g_wave_of[B3_SFX_COUNT][8];
static int    g_nwaves = 0;
static int    g_inited = 0;

static unsigned rd32(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned rd16(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static int load_wav(const char* path, B3Wave* w)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f); return -1;
    }
    int channels = 0, bits = 0, rate = 0;
    long dpos = 0; unsigned dlen = 0;
    unsigned char ck[8];
    while (fread(ck, 1, 8, f) == 8) {
        unsigned len = rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fmt[16];
            unsigned take = len < 16 ? len : 16;
            if (fread(fmt, 1, take, f) != take) break;
            channels = (int)rd16(fmt + 2);
            rate = (int)rd32(fmt + 4);
            bits = (int)rd16(fmt + 14);
            if (len > take) fseek(f, (long)(len - take), SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) {
            dpos = ftell(f);
            dlen = len;
            fseek(f, (long)len, SEEK_CUR);
        } else {
            fseek(f, (long)len, SEEK_CUR);
        }
        if (len & 1) fseek(f, 1, SEEK_CUR);
    }
    if (!dpos || bits != 16 || channels < 1 || rate <= 0) { fclose(f); return -1; }

    unsigned frames = dlen / 2u / (unsigned)channels;
    if (!frames) { fclose(f); return -1; }
    int16_t* raw = (int16_t*)malloc((size_t)frames * (size_t)channels * 2u);
    int16_t* mono = (int16_t*)malloc((size_t)frames * 2u);
    if (!raw || !mono) { free(raw); free(mono); fclose(f); return -1; }
    fseek(f, dpos, SEEK_SET);
    if (fread(raw, 2, (size_t)frames * (size_t)channels, f)
        != (size_t)frames * (size_t)channels) {
        free(raw); free(mono); fclose(f); return -1;
    }
    fclose(f);
    for (unsigned i = 0; i < frames; i++) {
        int acc = 0;
        for (int c = 0; c < channels; c++) acc += raw[i * channels + c];
        mono[i] = (int16_t)(acc / channels);
    }
    free(raw);
    w->pcm = mono; w->frames = frames; w->rate = rate; w->loaded = 1;
    return 0;
}

int b3_sfx_init(void)
{
    if (g_inited) return g_nwaves;
    g_inited = 1;
    const char* root = getenv("B3_AUDIO_DIR");
    if (!root) root = "build/audio";
    /* the surface loops live in the loaded TRACK bank (DAT_00411E48) */
    const char* track = getenv("B3_SFX_TRACK_BANK");
    if (!track) track = BK_TRACK;
    int missing = 0;
    for (int e = 0; e < B3_SFX_COUNT; e++) {
        const B3SfxDef* d = &g_defs[e];
        int is_track = !strcmp(d->dir, BK_TRACK);
        const char* dir = is_track ? track : d->dir;
        for (int v = 0; v < 8; v++) g_wave_of[e][v] = -1;
        for (int v = 0; v < 8 && d->files[v]; v++) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s/%s", root, dir, d->files[v]);
            if (g_nwaves >= MAXWAVES) break;
            if (load_wav(path, &g_waves[g_nwaves]) == 0) {
                g_wave_of[e][v] = g_nwaves++;
            } else if (!is_track) {
                /* a TRACK bank legitimately ships only the surfaces its
                 * level uses (Silver Lake has no snow.wav) -- not an error */
                missing++;
                if (missing <= 6)
                    fprintf(stderr, "[b3_sfx] missing wave %s\n", path);
            }
        }
    }
    printf("[b3_sfx] %d event waves loaded from %s%s\n", g_nwaves, root,
           missing ? " (some missing -- those events stay silent)" : "");
    return g_nwaves;
}

/* ====================================================================== */
/* 4. Voices + mixer (GLUE)                                                */
/* ====================================================================== */

#define NVOICES 24
#define OUT_RATE 44100.0

typedef struct {
    int      active;
    int      wave;
    double   pos;
    double   step;      /* source frames per output frame                 */
    float    gain;
    int      loop;
    unsigned gen;
} Voice;

static Voice g_voices[NVOICES];
static unsigned g_gen = 1;
static unsigned g_started = 0;
static int g_cooldown[B3_SFX_COUNT];
static float g_lx, g_ly, g_lz;
static int   g_have_listener = 0;
static float g_master = 0.9f;
static int   g_log = -1;

/* 0x003B16B4 = 15 (min distance) and 0x003B16B8 = 50 (max distance): every
 * emitter stores exactly this pair into the play-params block. */
#define DIST_MIN 15.0f
#define DIST_MAX 50.0f

void b3_sfx_set_listener(float x, float y, float z)
{
    g_lx = x; g_ly = y; g_lz = z; g_have_listener = 1;
}

void b3_sfx_set_master(float gain) { g_master = gain; }

/* The traffic-audio manager's master volume, mgr+0xD8.  FUN_00145F60
 * @0x00146000 ramps it toward 1.0 at +0.01 (0x003A7ED8) while the traffic
 * audio is live and back toward 0 at -0.1 (0x003A69C4), clamped both ends;
 * FUN_00146530 then takes max(it, 0.09) * 2.0 as the whoosh's master.  [C]
 * The harness has no "traffic audio off" state, so the ramp only runs up. */
static float g_pass_master = 0.0f;

float b3_sfx_pass_master(void) { return g_pass_master; }

void b3_sfx_pass_master_set(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_pass_master = v;
}

void b3_sfx_tick(void)
{
    for (int i = 0; i < B3_SFX_COUNT; i++)
        if (g_cooldown[i] > 0) g_cooldown[i]--;
    if (g_pass_master < 1.0f) {
        g_pass_master += B3_SFX_PASS_MASTER_RAMP_UP;
        if (g_pass_master > 1.0f) g_pass_master = 1.0f;
    }
}

unsigned b3_sfx_voices_started(void) { return g_started; }

static int alloc_voice(void)
{
    for (int i = 0; i < NVOICES; i++)
        if (!g_voices[i].active) return i;
    /* steal the quietest non-looping voice, as a finite voice pool must */
    int best = -1;
    float bg = 1e30f;
    for (int i = 0; i < NVOICES; i++)
        if (!g_voices[i].loop && g_voices[i].gain < bg) { bg = g_voices[i].gain; best = i; }
    return best;
}

static int start(B3SfxEvent ev, const B3SfxShot* sh, float gain)
{
    int wi = g_wave_of[ev][sh->variant];
    if (wi < 0 || !g_waves[wi].loaded) return -1;
    int vi = alloc_voice();
    if (vi < 0) return -1;
    Voice* v = &g_voices[vi];
    v->wave = wi;
    v->pos = 0.0;
    v->step = (double)g_waves[wi].rate / OUT_RATE * (double)sh->pitch;
    v->gain = gain;
    v->loop = g_defs[ev].loop;
    v->gen = ++g_gen;
    v->active = 1;
    g_started++;

    if (g_log < 0) g_log = getenv("B3_SFX_LOG") ? 1 : 0;
    if (g_log)
        printf("[b3_sfx] %-22s %-10s %-16s gain %.3f pitch %.3f t %.2f "
               "voice %d\n", g_defs[ev].name, g_defs[ev].wave, sh->file,
               (double)gain, (double)sh->pitch, (double)sh->t, vi);
    return (vi << 16) | (int)(v->gen & 0xFFFF);
}

int b3_sfx_event(B3SfxEvent ev, float impulse)
{
    if ((unsigned)ev >= B3_SFX_COUNT) return -1;
    if (g_cooldown[ev] > 0) return -1;
    B3SfxShot sh;
    if (!b3_sfx_resolve(ev, impulse, -1, &sh)) return -1;

    const B3SfxDef* d = &g_defs[ev];
    g_cooldown[ev] = d->cooldown +
        (d->cooldown_rnd ? (int)(sfx_rand() % (unsigned)d->cooldown_rnd) : 0);

    /* FUN_0014A6B0: 1 + U(-0.1,+0.1) applied on top of the pitch lerp. */
    if (d->pitch_var) {
        float u = (float)(sfx_rand() & 0xFFFF) / 65535.0f;
        sh.pitch *= 1.0f + 0.1f * (2.0f * u - 1.0f);
    }
    return start(ev, &sh, sh.gain * g_master);
}

int b3_sfx_event_at(B3SfxEvent ev, float impulse, float x, float y, float z)
{
    if ((unsigned)ev >= B3_SFX_COUNT) return -1;
    if (g_cooldown[ev] > 0) return -1;
    B3SfxShot sh;
    if (!b3_sfx_resolve(ev, impulse, -1, &sh)) return -1;

    const B3SfxDef* d = &g_defs[ev];
    g_cooldown[ev] = d->cooldown +
        (d->cooldown_rnd ? (int)(sfx_rand() % (unsigned)d->cooldown_rnd) : 0);
    if (d->pitch_var) {
        float u = (float)(sfx_rand() & 0xFFFF) / 65535.0f;
        sh.pitch *= 1.0f + 0.1f * (2.0f * u - 1.0f);
    }

    return b3_sfx_play_shot(ev, &sh, x, y, z);
}

/* Positional play of an already-resolved shot: the distance roll-off and
 * the master gain, shared by b3_sfx_event_at and the traffic-pass
 * emitter (which computes its own gain/pitch and must not have them
 * overwritten by the table lerp). */
/* Distance roll-off, the emitters' own 15/50 pair (0x003B16B4 /
 * 0x003B16B8).  Shared by the one-shots and by the driving loops. */
static float dist_att_xyz(float x, float y, float z)
{
    float dx, dy, dz, dist;
    if (!g_have_listener) return 1.0f;
    dx = x - g_lx; dy = y - g_ly; dz = z - g_lz;
    dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist <= DIST_MIN) return 1.0f;
    if (dist >= DIST_MAX) return 0.0f;
    return 1.0f - (dist - DIST_MIN) / (DIST_MAX - DIST_MIN);
}

/* Retail hands its gain to DirectSound's SetVolume, whose scale tops out at
 * DSBVOLUME_MAX = 0 dB -- so any emitter gain above 1.0 (the traffic
 * whoosh's PassScalar*MaxVolume*masterVol reaches 1.5, and the surface
 * loops' slip lift reaches 2.0) SATURATES on the real hardware rather than
 * amplifying.  The module's own mixer would clip instead, so it applies the
 * same ceiling.  [S] on the platform behaviour, [C] on the laws feeding it. */
static float gain_ceiling(float g)
{
    if (!(g > 0.0f)) return 0.0f;
    return g > 1.0f ? 1.0f : g;
}

int b3_sfx_play_shot(B3SfxEvent ev, const B3SfxShot* sh,
                     float x, float y, float z)
{
    float att;
    if ((unsigned)ev >= B3_SFX_COUNT || !sh) return -1;
    att = dist_att_xyz(x, y, z);
    if (att <= 0.0f) return -1;
    return start(ev, sh, gain_ceiling(sh->gain) * att * g_master);
}

void b3_sfx_stop(int voice)
{
    if (voice < 0) return;
    int vi = voice >> 16;
    unsigned gen = (unsigned)(voice & 0xFFFF);
    if (vi < 0 || vi >= NVOICES) return;
    if ((g_voices[vi].gen & 0xFFFF) == gen) g_voices[vi].active = 0;
}

int b3_sfx_voice_active(int voice)
{
    if (voice < 0) return 0;
    int vi = voice >> 16;
    unsigned gen = (unsigned)(voice & 0xFFFF);
    if (vi < 0 || vi >= NVOICES) return 0;
    return g_voices[vi].active && (g_voices[vi].gen & 0xFFFF) == gen;
}

float b3_sfx_next_sample(void)
{
    float acc = 0.0f;
    for (int i = 0; i < NVOICES; i++) {
        Voice* v = &g_voices[i];
        if (!v->active) continue;
        const B3Wave* w = &g_waves[v->wave];
        unsigned i0 = (unsigned)v->pos;
        if (i0 >= w->frames) {
            if (!v->loop) { v->active = 0; continue; }
            v->pos -= (double)w->frames;
            i0 = (unsigned)v->pos;
            if (i0 >= w->frames) { v->pos = 0.0; i0 = 0; }
        }
        unsigned i1 = (i0 + 1 < w->frames) ? i0 + 1 : (v->loop ? 0u : i0);
        float fr = (float)(v->pos - (double)i0);
        float s = (float)w->pcm[i0] * (1.0f - fr) + (float)w->pcm[i1] * fr;
        acc += s * v->gain;
        v->pos += v->step;
    }
    return acc;
}

void b3_sfx_mix_s16(int16_t* out, int frames)
{
    for (int i = 0; i < frames; i++) {
        float v = (float)out[i] + b3_sfx_next_sample();
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        out[i] = (int16_t)v;
    }
}

/* ====================================================================== */
/* 5. Crash-state audio                                                    */
/*                                                                          */
/* The user-visible symptom this section exists for: the engine kept        */
/* screaming at the pre-impact rpm through the whole crash flight.  Retail  */
/* does not mute or fade that voice -- it STARVES the engine.  See the      */
/* contract in burnout3_sfx.h and docs/RE_SFX.md section 6; the addresses   */
/* are repeated at each law below.                                          */
/* ====================================================================== */

/* rad/s <-> rpm, the game's own constants (FUN_00134710 / 0x0011D460). */
#define SFX_RADS_TO_RPM 9.549296f
#define SFX_RPM_TO_RADS 0.10471976f

/* FUN_00121560's neutral-gear slew rates [C], read off the ported
 * b3_engine_transmission_update (validate_port.py, engine section):
 *     max rpm >= 6000  ->  up 45.0, DOWN 16.0   rad/s per frame
 *     max rpm <  6000  ->  up 22.5, DOWN  9.6
 * Only the down rate matters here: the crashed path's target is
 * rev_limit * throttle = 0.                                               */
#define SFX_NEUTRAL_RATE_DN_HI 16.0f
#define SFX_NEUTRAL_RATE_DN_LO  9.6f
#define SFX_NEUTRAL_RATE_SPLIT_RPM 6000.0f
/* the idle floor's randomised overshoot, FUN_00121560: idle + U(0,16) */
#define SFX_IDLE_JITTER_RADS 16.0f
/* crash-path gravity, the wreck integrator's own -20 (RE_NOTES 16.2). Used
 * only to take free fall out of the frame-to-frame velocity delta below. */
#define SFX_CRASH_GRAVITY 20.0f

static int   g_crash_on = 0;       /* the crash law owns the engine voice  */
static float g_crash_omega = 0.0f; /* veh+0x149C under the crashed path    */
static float g_crash_idle_omega = 0.0f;
static float g_crash_rate_dn = SFX_NEUTRAL_RATE_DN_HI;
static float g_crash_clock = 0.0f; /* veh+0x1530, the crash clock          */
static float g_crash_prev_vel[3];
static int   g_crash_have_prev = 0;
static unsigned g_crash_crunches = 0;

int b3_sfx_crash_active(void) { return g_crash_on; }

float b3_sfx_engine_rpm(float sim_rpm)
{
    /* Read from the audio thread: one float, no state change. */
    return g_crash_on ? g_crash_omega * SFX_RADS_TO_RPM : sim_rpm;
}

/* ====================================================================== */
/* 5b. Boost audio -- the racecar boost attack / sustain / release          */
/* ====================================================================== */
/* Retail's racecar audio object holds four boost waves, loaded by
 * FUN_00136F80 (0x00136F80) from ASCII literals rather than packed base-40
 * constants -- see the block comment in burnout3_sfx.h:
 *
 *     obj+0xAC "BoostIn"   @0x00136FE7 -> the attack
 *     obj+0xA4 "BoostLoop" @0x00136F8F -> the sustain, looping
 *     obj+0xB0 "BoostOut"  @0x0013700F -> the release
 *     obj+0xA8 "fire"      @0x00136FBF -> the flame layer (B3_SFX_FIRE)
 *
 * and the mode singleton's second path FUN_00141D20 starts the same sustain
 * head-locked at the camera at gain 1.0 (0x003B168C @0x00141DD9), which is
 * why nothing here is positioned.                                       [C]
 *
 * The transitions themselves follow the boost record's own +0x52 flag (the
 * `boosting` argument), which is the only state retail's boost FX read; the
 * crash cut is the audio-side mirror of FUN_0017F730's carObj+0x18FA gate on
 * the visual FX.  The choice not to play the release on a crash is GLUE. */
static int g_boost_voice = -1;   /* the looping BoostLoop voice handle */
static int g_boost_on    = 0;    /* last frame's `boosting`            */
static int g_boost_crash = 0;    /* last frame's `crashed`             */

void b3_sfx_boost_stop(void)
{
    if (g_boost_voice >= 0) b3_sfx_stop(g_boost_voice);
    g_boost_voice = -1;
    g_boost_on = 0;
}

int b3_sfx_boost_active(void)
{
    return g_boost_voice >= 0 && b3_sfx_voice_active(g_boost_voice);
}

void b3_sfx_boost_tick(int boosting, int crashed)
{
    boosting = !!boosting;
    crashed  = !!crashed;

    if (crashed) {
        /* FUN_0017F730 @0x0017F73C gates the boost FX on carObj+0x18FA; a
         * wrecked car has none.  Cut the sustain with no release.     GLUE */
        if (g_boost_voice >= 0) { b3_sfx_stop(g_boost_voice); g_boost_voice = -1; }
        g_boost_on = 0;
        g_boost_crash = 1;
        return;
    }
    int was_crashed = g_boost_crash;
    g_boost_crash = 0;

    if (boosting && !g_boost_on) {
        b3_sfx_event(B3_SFX_BOOST_IN, 0.0f);
        if (g_boost_voice >= 0) b3_sfx_stop(g_boost_voice);
        g_boost_voice = b3_sfx_event(B3_SFX_BOOST_LOOP, 0.0f);
    } else if (!boosting && g_boost_on) {
        if (g_boost_voice >= 0) { b3_sfx_stop(g_boost_voice); g_boost_voice = -1; }
        if (!was_crashed) b3_sfx_event(B3_SFX_BOOST_OUT, 0.0f);
    } else if (boosting && g_boost_voice >= 0
               && !b3_sfx_voice_active(g_boost_voice)) {
        /* the voice pool stole it, or the wave is missing -- re-arm */
        g_boost_voice = b3_sfx_event(B3_SFX_BOOST_LOOP, 0.0f);
    }
    g_boost_on = boosting;
}

int b3_sfx_impact_world(float dv, int crashed, float x, float y, float z)
{
    /* FUN_0014D0F0 @0x0014D17B..0x0014D2A7: the crashed byte veh+0x210
     * selects the emitter.  Not crashed -> FUN_0014EEA0 (IMPACTWORL);
     * crashed -> FUN_0014F130 (the IMPACTFATA family).  The impulse the
     * router hands over is `impulse / veh+0x1F0` (mass) @0x0014D223, i.e.
     * a velocity change in m/s -- which is what `dv` is here.
     *   NOTE the crash-MODE halving (`* 0.5` @0x0014D248, taken when the
     * game mode ctx+0x1920 != 0) is not applied: a single race is mode 0.  */
    return b3_sfx_event_at(crashed ? B3_SFX_IMPACT_FATAL_CM
                                   : B3_SFX_IMPACT_WORLD, dv, x, y, z);
}

void b3_sfx_crash_tick(int crashed, const float pos[3], const float vel[3],
                       float sim_rpm, float idle_rpm, float max_rpm,
                       float dt)
{
    if (!crashed) {
        if (g_crash_on && g_log > 0)
            printf("[b3_sfx] crash engine law released after %.2f s "
                   "(%u crunches)\n", (double)g_crash_clock, g_crash_crunches);
        g_crash_on = 0;
        g_crash_have_prev = 0;
        g_crash_clock = 0.0f;
        return;
    }

    if (!g_crash_on) {
        /* ---- the crash edge -------------------------------------------
         * FUN_0011BE50 @0x0011BE75: `MOV AL,[EBX+0x210]; TEST AL,AL; JZ`
         * -- a set crash byte takes the crashed branch, and from here on
         * the racing path (and with it every throttle/torque write to the
         * engine) is skipped for this car.
         * @0x0011BE8B..0x0011BEBE: if the gear veh+0x14C8 is not neutral it
         * is forced to 0, the in-shift flag veh+0x14A4 to 1 and the shift
         * timer veh+0x14A0 to 0.35 (the float at 0x0039B2B0).  The gear is
         * then never touched again for the rest of the crash.
         * The audio consequence of the in-shift flag is separate and also
         * recovered: the rev emitter FUN_0013E640 @0x0013E663 zeroes its
         * rpm term whenever veh+0x1400 (throttle) <= 0 OR veh+0x14A4 != 0,
         * so it contributes nothing for the whole crash either.          */
        g_crash_omega = sim_rpm * SFX_RPM_TO_RADS;
        if (!(g_crash_omega > 0.0f)) g_crash_omega = 0.0f;
        g_crash_idle_omega = idle_rpm * SFX_RPM_TO_RADS;
        g_crash_rate_dn = (max_rpm >= SFX_NEUTRAL_RATE_SPLIT_RPM)
                        ? SFX_NEUTRAL_RATE_DN_HI : SFX_NEUTRAL_RATE_DN_LO;
        g_crash_clock = 0.0f;
        g_crash_have_prev = 0;
        g_crash_on = 1;
        if (g_log < 0) g_log = getenv("B3_SFX_LOG") ? 1 : 0;
        if (g_log)
            printf("[b3_sfx] crash engine law engaged at %.0f rpm "
                   "(idle %.0f, down-slew %.1f rad/s per frame)\n",
                   (double)sim_rpm, (double)idle_rpm,
                   (double)g_crash_rate_dn);
        g_crash_crunches = 0;
    }

    g_crash_clock += dt;              /* veh+0x1530 @0x0011BE83 */

    /* ---- the engine, FUN_00121560 with throttle = wheel_omega = 0 ------
     * @0x0011BEC6..0x0011BED4: `PUSH 0; PUSH 0; PUSH 0; LEA ESI,[EBX+0x1448];
     * XOR EDI,EDI; CALL FUN_00121560`.  In neutral the update's target is
     * rev_limit * throttle = 0, so the slew is a pure down-ramp at the
     * neutral rate, and the idle floor catches it:
     *     omega = max(0, omega - rate_dn)
     *     if (omega < idle) and (omega <= idle + 0.1) omega = idle + U(0,16)
     * The rates are per CALL (the game runs this at its fixed frame tick),
     * so this is not scaled by dt -- and because the frame rate does not
     * change under time dilation, the coast-down takes the same ~0.5 s of
     * WALL time whatever the crash divisor is, exactly as retail.        */
    g_crash_omega -= g_crash_rate_dn;
    if (g_crash_omega < 0.0f) g_crash_omega = 0.0f;
    if (g_crash_omega < g_crash_idle_omega) {
        if (g_crash_omega <= g_crash_idle_omega + 0.1f) {
            /* the game draws this from the transmission's own PRNG at
             * +0x14B0; the module's xorshift stands in for it (the LAW is
             * the recovered idle + U(0,16), the stream is not). */
            float u = (float)(sfx_rand() & 0xFFFF) / 65535.0f;
            g_crash_omega = u * SFX_IDLE_JITTER_RADS + g_crash_idle_omega;
        }
    }

    if (g_log > 0) {
        static int trace = 0;
        if ((trace++ % 6) == 0)
            printf("[b3_sfx] crash t %5.2f  engine %6.0f rpm\n",
                   (double)g_crash_clock,
                   (double)(g_crash_omega * SFX_RADS_TO_RPM));
    }

    /* ---- the crunches -------------------------------------------------
     * Retail gets one FUN_0014D0F0 call per resolved chassis contact, with
     * the contact impulse; the router divides it by the mass, so what the
     * emitter sees is the contact's velocity change.  The harness's wreck
     * has no contact-event stream, so the trigger (not the law) is GLUE:
     * the frame-to-frame velocity change with free fall taken out is that
     * same quantity, and the emitter's own 2 m/s silence gate and 20-frame
     * cooldown do the rest.                                              */
    if (vel) {
        if (g_crash_have_prev) {
            float dx = vel[0] - g_crash_prev_vel[0];
            float dy = vel[1] - g_crash_prev_vel[1];
            float dz = vel[2] - g_crash_prev_vel[2];
            float dv = sqrtf(dx * dx + dy * dy + dz * dz)
                     - SFX_CRASH_GRAVITY * dt;
            if (dv > 0.0f && pos) {
                if (b3_sfx_impact_world(dv, 1, pos[0], pos[1], pos[2]) >= 0)
                    g_crash_crunches++;
            }
        }
        g_crash_prev_vel[0] = vel[0];
        g_crash_prev_vel[1] = vel[1];
        g_crash_prev_vel[2] = vel[2];
        g_crash_have_prev = 1;
    }
}

/* ====================================================================== */
/* 7. THE DRIVING-TIME LOOP EMITTERS                                       */
/*                                                                         */
/* Port of FUN_00136120's three loop children -- the road-surface beds     */
/* (FUN_00136610), the tyre skid/squeal samples (FUN_0013DE10 x2 through   */
/* FUN_0013DCA0) and the gear shot (FUN_00136C50).  The full evidence      */
/* chain, with every address and the executed-registrar transcript the     */
/* numbers came out of, is the block comment in burnout3_sfx.h.            */
/* ====================================================================== */

/* --- 0x0039BBF8[id]: which of the five surface beds the id names.  The
 * table is 8-byte packed base-40 values; decoded once here so the port
 * carries the mapping and validate_sfx re-derives it from the image.  [C] */
static const signed char g_surf_wave[B3_SFX_SURF_IDS] = {
/*  0 None              */ -1,
/*  1 RoadTarmac        */ 0, /*  2 RoadConcrete   */ 0,
/*  3 RoadChevrons      */ 0, /*  4 RoadDirt       */ 1,
/*  5 RoadGravel        */ 1, /*  6 RoadCobbles    */ 2,
/*  7 RoadSnowyCobbles  */ 2, /*  8 RoadPaving     */ 0,
/*  9 RoadRumbleStrip   */ 2, /* 10 RoadMetalGrille*/ 3,
/* 11 RoadSnow          */ 4, /* 12 OfflineTarmac  */ 0,
/* 13 OfflineConcrete   */ 0, /* 14 OfflineDirt    */ 1,
/* 15 OfflineGrass      */ 1, /* 16 OfflineGravel  */ 1,
/* 17 OfflinePavement   */ 0, /* 18 OfflineSand    */ 1,
/* 19 OfflineSnow       */ 4, /* 20 OfflineRumble  */ 2,
/* 21..38 walls / non-driving surfaces have no entry */
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1
};

/* --- 0x0040E318[id]: the curve row.  Read out of the executed registrar
 * FUN_00137F50 (the table is BSS in the image and filled at init).    [C] */
static const unsigned char g_surf_row[B3_SFX_SURF_IDS] = {
    0, 1, 2, 1, 4, 4, 9, 9, 5, 9, 8, 6, 1, 2, 4, 4, 4, 5, 0, 6,
    9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0
};

/* --- the jump table at 0x00136C2C / 0x00136C34, index = surface - 1.
 * 0 selects the case branch @0x001368FC which zeroes the slip term. [C]  */
static const unsigned char g_surf_slip_on[17] = {
/* 1 Tarmac */ 0, /* 2 Concrete */ 0, /* 3 Chevrons */ 0, /* 4 Dirt */ 1,
/* 5 Gravel */ 1, /* 6 Cobbles */ 0, /* 7 SnowyCobbles */ 1,
/* 8 Paving */ 0, /* 9 RumbleStrip */ 1, /* 10 MetalGrille */ 1,
/* 11 Snow */ 1, /* 12 OffTarmac */ 0, /* 13 OffConcrete */ 0,
/* 14 OffDirt */ 1, /* 15 OffGrass */ 1, /* 16 OffGravel */ 1,
/* 17 OffPavement */ 0
};

/* --- table set A (slots 0,1) and set B (slots 2,3), the SHIPPED values
 * (ValueDB Sound/Surface.cfg over the compiled-in defaults, both read out
 * of the executed FUN_00137F50).  Column order is the object's own:
 * {MinSpeed, MaxSpeed, MinVol, MaxVol, MinPitch, MaxPitch}.           [C] */
static const float g_surf_curve[2][B3_SFX_SURF_ROWS][6] = {
    { /* set A -- 0x0040E130..0x0040E1F8, the always-TAR layer */
    /* 0 None           */ { 0.0f,   0.0f, 0.0f, 0.0f, 1.0f, 1.0f },
    /* 1 Tarmac         */ { 0.0f,   0.0f, 0.0f, 0.0f, 1.0f, 1.0f },
    /* 2 Concrete       */ { 0.0f,   0.0f, 0.0f, 0.0f, 1.0f, 1.0f },
    /* 3 Offline Tarmac */ { 0.0f,   0.0f, 0.0f, 0.0f, 1.0f, 1.0f },
    /* 4 Gravel         */ { 0.0f,   0.0f, 0.0f, 0.0f, 1.0f, 1.0f },
    /* 5 Pavement       */ { 0.0f,   0.0f, 0.0f, 0.0f, 0.5f, 0.5f },
    /* 6 Snow           */ { 0.0f, 100.0f, 0.0f, 0.0f, 0.5f, 1.0f },
    /* 7 Offline Gravel */ { 0.0f,   0.0f, 0.0f, 0.0f, 1.0f, 1.0f },
    /* 8 Metal          */ { 0.0f,  44.0f, 0.0f, 0.0f, 0.4f, 0.4f },
    /* 9 Wood           */ { 0.0f,  44.0f, 0.0f, 0.2f, 0.9f, 1.0f },
    },
    { /* set B -- 0x0040E220..0x0040E2E8, the surface-indexed layer */
    /* 0 None           */ { 0.0f,   0.0f, 0.0f, 0.00f, 1.00f, 1.0f },
    /* 1 Tarmac         */ { 0.0f,  44.0f, 0.0f, 0.20f, 0.90f, 1.0f },
    /* 2 Concrete       */ { 0.0f,  44.0f, 0.0f, 0.35f, 0.60f, 0.7f },
    /* 3 Offline Tarmac */ { 0.0f,   0.0f, 0.0f, 0.00f, 1.00f, 1.0f },
    /* 4 Gravel         */ { 0.0f,  44.0f, 0.0f, 0.40f, 0.90f, 1.0f },
    /* 5 Pavement       */ { 0.0f,  44.0f, 0.0f, 0.18f, 1.40f, 1.4f },
    /* 6 Snow           */ { 0.0f,  44.0f, 0.0f, 0.40f, 0.70f, 0.7f },
    /* 7 Offline Gravel */ { 0.0f,   0.0f, 0.0f, 0.00f, 1.00f, 1.0f },
    /* 8 Metal          */ {10.0f,  44.0f, 0.0f, 0.40f, 1.00f, 1.0f },
    /* 9 Wood           */ { 0.0f,  44.0f, 0.0f, 0.40f, 0.95f, 1.0f },
    }
};

/* --- Sound/Skids: three samples x four two-point curves, SHIPPED values.
 * Curve order matches the record: SlipVol, SlipFreq, SpinVol, SpinFreq;
 * each is {x0, y0, x1, y1}.                                           [C] */
static const float g_skid_curve[3][4][4] = {
    { /* CONST01 @0x003EC108 */
      { 0.75f, 0.0f,  3.0f,  0.42f  },   /* slip volume        */
      { 2.00f, 1000.0f, 8.0f, -3500.0f },/* slip frequency, Hz */
      { 0.95f, 0.0f,  3.0f,  1.15f  },   /* spin volume        */
      { 2.00f, 1000.0f, 8.0f, -3500.0f },/* spin frequency, Hz */
    },
    { /* CONST02 @0x003EC150 */
      { 0.50f, 0.0f,  5.5f,  0.80f  },
      { 3.50f, 1000.0f, 10.0f, -3500.0f },
      { 0.95f, 0.0f,  1.0f,  0.225f },
      { 3.50f, 1000.0f, 10.0f, -3500.0f },
    },
    { /* CONST03 @0x003EC198 */
      { 6.00f, 0.0f, 10.0f,  0.30f  },
      { 10.0f, 1000.0f, 16.0f, -2500.0f },
      { 2.00f, 0.0f, 10.0f,  0.30f  },
      { 10.0f, 1000.0f, 16.0f, -2500.0f },
    }
};

/* the FUN_0013DE10 surface weights, jump table @0x0013E4D0/0x0013E4DC  [C] */
float b3_sfx_skid_surface_weight(int id)
{
    switch (id) {
    case 1: case 2: case 3: case 6: case 8: case 9:
    case 12: case 13: case 17: case 20:
        return 1.0f;
    case 7: case 10: case 11: case 19:
        return 0.5f;
    default:
        return 0.0f;
    }
}

B3SfxEvent b3_sfx_surface_wave(int id)
{
    if (id < 0 || id >= B3_SFX_SURF_IDS || g_surf_wave[id] < 0)
        return B3_SFX_COUNT;
    return (B3SfxEvent)(B3_SFX_SURFACE_TAR + g_surf_wave[id]);
}

int b3_sfx_surface_row(int id)
{
    if (id < 0 || id >= B3_SFX_SURF_IDS) return 0;
    return g_surf_row[id];
}

int b3_sfx_surface_slip_active(int id)
{
    /* `dec eax ; cmp eax,0x10 ; ja default` @0x0013681D: only 1..17 are in
     * the table, everything else takes the default (slip term active). */
    if (id < 1 || id > 17) return 1;
    return g_surf_slip_on[id - 1];
}

int b3_sfx_surface_slot(int slot, int surface_id, int contact,
                        float speed_ms, float slip,
                        float* out_gain, float* out_pitch)
{
    int set, row;
    float t, span, s, gain, pitch;
    const float* c;

    if (out_gain) *out_gain = 0.0f;
    if (out_pitch) *out_pitch = 1.0f;
    if (slot < 0 || slot > 3) return 0;
    if (surface_id < 0 || surface_id >= B3_SFX_SURF_IDS) return 0;
    /* `if (rec[0xB3] == 0) surface = 0` @0x001367FE, then the stop path
     * @0x00136A9F takes the "no voice" branch on surface == 0. */
    if (!contact) surface_id = 0;
    if (surface_id == 0) return 0;

    set = (slot >= 2) ? 1 : 0;
    row = g_surf_row[surface_id];
    c = g_surf_curve[set][row];

    /* the SPEED lerp, in the emitters' own MAXSS-then-MINSS order */
    span = c[1] - c[0];
    t = clamp01((speed_ms - c[0]) / span);

    /* the SLIP term.  min(|slip|, 8) * 0.125, zeroed on the hard
     * surfaces in the jump table. */
    if (!b3_sfx_surface_slip_active(surface_id)) slip = 0.0f;
    s = slip < 0.0f ? -slip : slip;
    if (s > B3_SFX_SURF_SLIP_CLAMP) s = B3_SFX_SURF_SLIP_CLAMP;
    s *= B3_SFX_SURF_SLIP_SCALE;

    gain  = (c[2] + t * (c[3] - c[2]))
          * (1.0f + B3_SFX_SURF_GAIN_K * s);
    pitch =  c[4] + t * (c[5] - c[4]);
    /* slots 0 and 3 (0x00479DB0 and 0x00479E70) detune by +0.24 */
    if (slot == 0 || slot == 3) pitch += B3_SFX_SURF_DETUNE;
    pitch *= (1.0f + B3_SFX_SURF_PITCH_K * s);

    if (out_gain) *out_gain = gain;
    if (out_pitch) *out_pitch = pitch;
    return 1;
}

float b3_sfx_skid_wheel(const B3SfxWheel* w, float speed_ms, int reverse,
                        float* acc)
{
    float raw, d, v;
    if (!w || !acc) return 0.0f;
    if (reverse) speed_ms = -speed_ms;   /* @0x0013DE32, ECX = gear == -1 */

    if (!w->contact) {
        raw = 0.0f;                      /* @0x0013DEB7 */
    } else {
        /* speed - omega*(1/2pi)*radius*(2pi) = speed - the wheel's own
         * surface speed; the two reciprocal constants are literally in
         * the code (0x003B1734 / 0x003B1738) and are kept here so the
         * float rounding matches. */
        raw = speed_ms - (w->omega * 0.15915494f) * (w->radius * 6.2831855f);
        /* ModeMul[wheel+0x78] -- the eight "Wheel Mode '<x>' Multipler"
         * ValueDB slots at 0x003EC1EC.  Only 'Driving' has an override
         * (0.5); every other mode ships at 1.0.  Which INDEX each named
         * mode occupies was not traced (the wheel state machine at
         * wheel+0x78 is not in this port), so a caller that cannot supply
         * the index passes mode < 0 and gets the 1.0 every mode but one
         * uses.                                                        [?] */
        if (w->mode == 0) raw *= B3_SFX_SKID_MODE_DRIVING;
        if (raw > 0.0f)
            raw = raw * B3_SFX_SKID_SLIP_SPEED_MUL * speed_ms;
    }

    /* the lag limiter, then the +/-max window */
    d = raw - *acc;
    if (d < B3_SFX_SKID_LAG_MIN)      d = B3_SFX_SKID_LAG_MIN;
    else if (d > B3_SFX_SKID_LAG_MAX) d = B3_SFX_SKID_LAG_MAX;
    v = *acc + d;
    if (v > 0.0f) {
        if (v > B3_SFX_SKID_MAX_SLIP) v = B3_SFX_SKID_MAX_SLIP;
    } else {
        float n = -v;
        if (!(n >= 0.0f)) n = 0.0f;
        if (n > B3_SFX_SKID_MAX_SPIN) n = B3_SFX_SKID_MAX_SPIN;
        v = -n;
    }
    *acc = v;
    return v;
}

/* the two-point clamp-and-lerp every Sound/Skids curve is */
static float skid_curve(const float* c, float x)
{
    float d;
    if (x < c[0]) x = c[0];
    else if (x > c[2]) x = c[2];
    d = c[2] - c[0];
    if (d == 0.0f) return c[1];
    return ((x - c[0]) / d) * (c[3] - c[1]) + c[1];
}

int b3_sfx_skid_sample(int sample, float* total, int left, int crashed,
                       float native_hz, float* out_gain, float* out_rate_hz)
{
    float gain, hz, x;
    const float (*cv)[4];

    if (out_gain) *out_gain = 0.0f;
    if (out_rate_hz) *out_rate_hz = native_hz;
    if (sample < 0 || sample > 2 || !total) return 0;
    cv = g_skid_curve[sample];

    if (*total >= 0.0f) {         /* SLIP  -- comiss/jb @0x0013E0D3 */
        x = *total;
        gain = skid_curve(cv[0], x);
        hz   = skid_curve(cv[1], x);
    } else {                      /* SPIN */
        x = -*total;
        gain = skid_curve(cv[2], x);
        hz   = skid_curve(cv[3], x);
        /* RETAIL QUIRK, executed and confirmed: the spin branch writes the
         * ABSOLUTE value back into the slot the three-sample loop re-reads
         * the running total from (`movss [esp+0x10],xmm0` @0x0013E20B, and
         * the loop tail reloads it at @0x0013E49B).  So only SAMPLE 1 ever
         * sees the Spin curves -- samples 2 and 3 evaluate |total| against
         * the SLIP curves for the rest of that frame.  Reproduced, not
         * corrected: it is audible (a wheelspin plays const01's spin bed
         * plus const02/03's slip beds).                                [C] */
        *total = x;
    }

    hz += native_hz;                                   /* @0x0013E315 */
    hz += left ? B3_SFX_SKID_PITCH_LEFT                /* @0x0013E327 */
               : B3_SFX_SKID_PITCH_RIGHT;
    if (crashed) gain *= B3_SFX_SKID_CRASH_VOL_MUL;    /* @0x0013E348 */

    if (out_gain) *out_gain = gain;
    if (out_rate_hz) *out_rate_hz = hz;
    /* `comiss xmm0,[esp+0x18] ; jb start` @0x0013E3A9 with xmm0 =
     * 0x00384208 -- at or below the epsilon the voice is stopped. */
    return (gain > B3_SFX_SKID_GAIN_EPS) && (hz > 0.0f);
}

/* ---------------------------------------------------------------------- *
 * The loop plumbing.  Retail hands PlaySound3D a live params block and
 * re-positions/re-gains the voice every frame through FUN_001CC3E0; the
 * module's mixer needs the same, so a live voice can be re-gained and
 * re-pitched in place.  GLUE (the mixer is, per RE_SFX.md 7).            */
static void voice_set(int handle, float gain, float pitch)
{
    int vi;
    unsigned gen;
    if (handle < 0) return;
    vi = handle >> 16;
    gen = (unsigned)(handle & 0xFFFF);
    if (vi < 0 || vi >= NVOICES) return;
    if ((g_voices[vi].gen & 0xFFFF) != gen || !g_voices[vi].active) return;
    g_voices[vi].gain = gain;
    g_voices[vi].step = (double)g_waves[g_voices[vi].wave].rate / OUT_RATE
                      * (double)pitch;
}

static int loop_voice(B3SfxEvent ev, int handle, int want,
                      float gain, float pitch, const float pos[3])
{
    B3SfxShot sh;
    if (want && pos)
        gain = gain_ceiling(gain) * dist_att_xyz(pos[0], pos[1], pos[2])
             * g_master;
    /* retail's own "is this voice worth having" test is the skid path's
     * epsilon 0x00384208; the surface slots have no such test but a loop
     * at zero gain only costs a voice, so the module applies it to both.
     * GLUE (voice-pool management, not a level).                        */
    if (!want || !(gain > B3_SFX_SKID_GAIN_EPS) || pitch <= 0.0f) {
        if (handle >= 0) b3_sfx_stop(handle);
        return -1;
    }
    if (handle >= 0 && b3_sfx_voice_active(handle)) {
        voice_set(handle, gain, pitch);
        return handle;
    }
    if (!b3_sfx_resolve(ev, 0.0f, 0, &sh)) return -1;
    sh.gain = gain;
    sh.pitch = pitch;
    return start(ev, &sh, gain);
}

void b3_sfx_drive_stop(B3SfxDriveState* st)
{
    int i, s;
    if (!st) return;
    for (i = 0; i < 4; i++) {
        if (st->surf_voice[i] >= 0) b3_sfx_stop(st->surf_voice[i]);
        st->surf_voice[i] = -1;
        st->surf_row[i] = -1;
    }
    for (s = 0; s < 2; s++)
        for (i = 0; i < 3; i++) {
            if (st->skid_voice[s][i] >= 0) b3_sfx_stop(st->skid_voice[s][i]);
            st->skid_voice[s][i] = -1;
        }
    st->surf_lag[0] = st->surf_lag[1] = 0.0f;
    st->skid_acc[0][0] = st->skid_acc[0][1] = 0.0f;
    st->skid_acc[1][0] = st->skid_acc[1][1] = 0.0f;
    st->gear_prev = 0;
    st->primed = 1;
}

/* FUN_0013DCA0's two sides: side 0 = wheels {1,3} with the LEFT (+1000 Hz)
 * flag, side 1 = wheels {0,2} with the RIGHT (-1000 Hz) flag.          [C] */
static const int g_skid_pair[2][2] = { {1, 3}, {0, 2} };

void b3_sfx_drive_tick(B3SfxDriveState* st, const B3SfxDriveIn* in)
{
    int slot, side, i, surf, row, gear;
    float slip_in[2];

    if (!st || !in) return;
    if (!st->primed) {
        for (i = 0; i < 4; i++) { st->surf_voice[i] = -1; st->surf_row[i] = -1; }
        for (side = 0; side < 2; side++)
            for (i = 0; i < 3; i++) st->skid_voice[side][i] = -1;
        st->primed = 1;
    }

    /* ---- 1. FUN_0013DCA0 -> FUN_0013DE10 x2, the skid/squeal loops ---- *
     * Run FIRST because retail runs FUN_001372F0 (which owns them) before
     * FUN_00136610, and the surface loops read the same per-wheel slip.  */
    {
        int reverse = (in->gear == -1);
        for (side = 0; side < 2; side++) {
            float total = 0.0f, emit[3];
            int   left = (side == 0);
            for (i = 0; i < 2; i++) {
                int wi = g_skid_pair[side][i];
                float v, w;
                if (wi >= in->wheel_count) { st->skid_acc[side][i] = 0.0f; continue; }
                v = b3_sfx_skid_wheel(&in->wheel[wi], in->speed_ms, reverse,
                                      &st->skid_acc[side][i]);
                w = b3_sfx_skid_surface_weight(in->wheel[wi].contact
                                               ? in->wheel[wi].surface : 0);
                total += w * v;
            }
            /* the summed value is clamped into the same window @0x0013DF7A */
            if (total > B3_SFX_SKID_MAX_SLIP)  total = B3_SFX_SKID_MAX_SLIP;
            if (total < -B3_SFX_SKID_MAX_SPIN) total = -B3_SFX_SKID_MAX_SPIN;

            /* the emitter position: racecar pos + fwd*AT +/- right*RIGHT
             * (@0x0013DD53 / @0x0013DDA5, 1.0 and 3.0). */
            for (i = 0; i < 3; i++)
                emit[i] = in->pos[i] + in->fwd[i] * B3_SFX_SKID_EMIT_AT
                        + (left ? 1.0f : -1.0f) * in->right[i]
                          * B3_SFX_SKID_EMIT_RIGHT;

            for (i = 0; i < 3; i++) {
                B3SfxEvent ev = (B3SfxEvent)(B3_SFX_SKID_1 + i);
                int wi = g_wave_of[ev][0];
                float native = (wi >= 0 && g_waves[wi].loaded)
                             ? (float)g_waves[wi].rate : 24000.0f;
                float gain, hz;
                /* `total` is passed BY REFERENCE: the spin branch rewrites
                 * it for the remaining samples, exactly as retail does. */
                int on = b3_sfx_skid_sample(i, &total, left, in->crashed,
                                            native, &gain, &hz);
                st->skid_voice[side][i] =
                    loop_voice(ev, st->skid_voice[side][i], on,
                               gain, on ? hz / native : 1.0f, emit);
            }
        }
    }

    /* the surface loops' own slip inputs are the audio object's
     * per-front-wheel skid values (obj+0x34/+0x38).  The port feeds them
     * the skid emitter's lag-limited accumulator for the same two wheels
     * -- same quantity, same units; the exact field aliasing inside the
     * audio object was not traced.                                 [S] GLUE */
    slip_in[0] = st->skid_acc[1][0];   /* wheel 0, side 1 slot 0 */
    slip_in[1] = st->skid_acc[0][0];   /* wheel 1, side 0 slot 0 */

    /* ---- 2. FUN_00136610, the four road-surface loops ----------------- */
    for (slot = 0; slot < 4; slot++) {
        int wi = slot & 1;                       /* wheel 0 / wheel 1 */
        const B3SfxWheel* w = &in->wheel[wi];
        float gain, pitch, emit[3];
        B3SfxEvent ev;
        int on;

        surf = w->contact ? w->surface : 0;
        /* the lag-limited copy the function keeps at obj+0x184 (nothing
         * else reads it, but it is state the emitter owns) */
        {
            float d = slip_in[wi] - st->surf_lag[wi];
            if (d < B3_SFX_SURF_LAG_MIN)      d = B3_SFX_SURF_LAG_MIN;
            else if (d > B3_SFX_SURF_LAG_MAX) d = B3_SFX_SURF_LAG_MAX;
            st->surf_lag[wi] = b3_sfx_surface_slip_active(surf)
                             ? st->surf_lag[wi] + d : 0.0f;
        }

        on = b3_sfx_surface_slot(slot, surf, w->contact, in->speed_ms,
                                 slip_in[wi], &gain, &pitch);
        /* slots 0,1 take the FIXED name at 0x0039BC00 = TAR; slots 2,3
         * the surface-indexed one. */
        ev = (slot < 2) ? B3_SFX_SURFACE_TAR : b3_sfx_surface_wave(surf);
        row = on ? g_surf_row[surf] : -1;
        if (ev == B3_SFX_COUNT) { on = 0; row = -1; }

        /* the restart-on-row-change rule @0x00136A74 */
        if (row != st->surf_row[slot] && st->surf_voice[slot] >= 0) {
            b3_sfx_stop(st->surf_voice[slot]);
            st->surf_voice[slot] = -1;
        }
        st->surf_row[slot] = row;

        for (i = 0; i < 3; i++)
            emit[i] = w->pos[i] - in->fwd[i] * B3_SFX_SURF_EMIT_OFF
                    + ((wi == 0) ? -1.0f : 1.0f) * in->right[i]
                      * B3_SFX_SURF_EMIT_OFF;

        st->surf_voice[slot] = loop_voice(ev, st->surf_voice[slot], on,
                                          gain, pitch, emit);
    }

    /* ---- 3. FUN_00136C50, the gear shot ------------------------------- */
    gear = in->gear;
    if (st->gear_prev != 0 && st->gear_prev != -1 && st->gear_prev < gear)
        b3_sfx_event_at(B3_SFX_GEAR, 0.0f, in->pos[0], in->pos[1], in->pos[2]);
    st->gear_prev = gear;

    /* ---- diagnostics: one line per half second under B3_SFX_LOG ------- */
    if (g_log < 0) g_log = getenv("B3_SFX_LOG") ? 1 : 0;
    if (g_log) {
        static int trace = 0;
        if ((trace++ % 30) == 0) {
            float gL = 0.0f, gR = 0.0f, gs = 0.0f;
            float tot[2] = {0.0f, 0.0f};
            for (side = 0; side < 2; side++)
                for (i = 0; i < 2; i++)
                    tot[side] += b3_sfx_skid_surface_weight(
                                     in->wheel[g_skid_pair[side][i]].contact
                                     ? in->wheel[g_skid_pair[side][i]].surface
                                     : 0) * st->skid_acc[side][i];
            for (i = 0; i < 3; i++) {
                float g, hz;
                if (b3_sfx_skid_sample(i, &tot[0], 1, in->crashed, 24000.0f,
                                       &g, &hz)) gL += g;
                if (b3_sfx_skid_sample(i, &tot[1], 0, in->crashed, 24000.0f,
                                       &g, &hz)) gR += g;
            }
            for (slot = 2; slot < 4; slot++) {
                float g, p;
                int wq = slot & 1;
                if (b3_sfx_surface_slot(slot, in->wheel[wq].contact
                                              ? in->wheel[wq].surface : 0,
                                        in->wheel[wq].contact, in->speed_ms,
                                        slip_in[wq], &g, &p)) gs += g;
            }
            printf("[b3_sfx] drive spd %5.1f m/s surf %2d/%2d slip %6.2f/%6.2f"
                   " skid L %.3f R %.3f  surface %.3f  gear %d\n",
                   (double)in->speed_ms, in->wheel[0].contact
                   ? in->wheel[0].surface : 0, in->wheel[1].contact
                   ? in->wheel[1].surface : 0,
                   (double)tot[0], (double)tot[1],
                   (double)gL, (double)gR, (double)gs, gear);
        }
    }
}

/* ---------------------------------------------------------------------- *
 * BODY SCRAPE.  The pair FUN_001521C0 starts together; the crossfade is
 * the caller's, so the harness supplies it (see burnout3_sfx.h).         */
static int g_scrape_voice[2] = { -1, -1 };

void b3_sfx_scrape_stop(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (g_scrape_voice[i] >= 0) b3_sfx_stop(g_scrape_voice[i]);
        g_scrape_voice[i] = -1;
    }
}

void b3_sfx_scrape_tick(int contact, float gain, float hi, const float pos[3])
{
    static const B3SfxEvent ev[2] = { B3_SFX_SCRAPE_LO, B3_SFX_SCRAPE_HI };
    float w[2];
    int i;
    if (!contact || gain <= 0.0f) { b3_sfx_scrape_stop(); return; }
    if (hi < 0.0f) hi = 0.0f;
    if (hi > 1.0f) hi = 1.0f;
    w[0] = gain * (1.0f - hi);
    w[1] = gain * hi;
    for (i = 0; i < 2; i++)
        g_scrape_voice[i] = loop_voice(ev[i], g_scrape_voice[i],
                                       w[i] > 0.0f, w[i], 1.0f, pos);
    if (getenv("B3_SCRAPE_TRACE"))
        fprintf(stderr, "[scrapefx] w %.2f/%.2f att %.3f master %.2f "
                "-> voices %d/%d\n", w[0], w[1],
                dist_att_xyz(pos[0], pos[1], pos[2]), g_master,
                g_scrape_voice[0], g_scrape_voice[1]);
}

/* ====================================================================== */
/* 6. Table dump driver, used by tools/validate_sfx.py.                    */
/*    Built with -DB3_SFX_TEST_MAIN; not part of the harness binary.       */
/* ====================================================================== */
#ifdef B3_SFX_TEST_MAIN
int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "table")) {
        for (int e = 0; e < B3_SFX_COUNT; e++) {
            const B3SfxDef* d = &g_defs[e];
            printf("DEF %d|%s|%s|%s|%s|%d|%.9g|%.9g|%.9g|%.9g|%.9g|%.9g|"
                   "%.9g|%d|%d|%d|%d|%d|%d", e, d->name, d->wave, d->emitter,
                   d->dir, d->param_off, (double)d->min_impulse,
                   (double)d->max_impulse, (double)d->min_gain,
                   (double)d->max_gain, (double)d->min_pitch,
                   (double)d->max_pitch, (double)d->gain_scale,
                   d->cooldown, d->cooldown_rnd, d->gated, d->loop,
                   d->pitch_var, (int)d->conf);
            for (int v = 0; v < 8 && d->files[v]; v++)
                printf("|%s", d->files[v]);
            printf("\n");
        }
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "route")) {
        /* route <dv>: fire FUN_0014D0F0's router both ways and let the
         * module's own event log name the emitter each one picked. */
        float dv = (float)atof(argv[2]);
        b3_sfx_init();
        g_log = 1;
        printf("route0:\n");
        b3_sfx_impact_world(dv, 0, 0.0f, 0.0f, 0.0f);
        printf("route1:\n");
        b3_sfx_impact_world(dv, 1, 0.0f, 0.0f, 0.0f);
        return 0;
    }
    if (argc > 5 && !strcmp(argv[1], "crash")) {
        /* crash <start rpm> <idle rpm> <max rpm> <frames>: the crashed-path
         * engine speed, one line per frame, for the differential test
         * against the real FUN_00121560 port. */
        float rpm = (float)atof(argv[2]);
        float idle = (float)atof(argv[3]);
        float maxr = (float)atof(argv[4]);
        int n = atoi(argv[5]);
        float pos[3] = {0, 0, 0};
        for (int i = 0; i < n; i++) {
            b3_sfx_crash_tick(1, pos, NULL, rpm, idle, maxr, 1.0f / 60.0f);
            printf("%d %.9g\n", i, (double)b3_sfx_engine_rpm(-1.0f));
        }
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "boost")) {
        /* boost <pattern>: '1' boosting, '0' not, 'x' crashed. Drives the
         * recovered attack/sustain/release state machine and reports each
         * transition, so validate_sfx can assert the ordering. */
        b3_sfx_init();
        int prev_loop = 0, prev_crash = 0;
        for (const char* s = argv[2]; *s; s++) {
            int boosting = (*s == '1'), crashed = (*s == 'x');
            int was = b3_sfx_boost_active();
            b3_sfx_boost_tick(boosting, crashed);
            int now = b3_sfx_boost_active();
            if (!was && now) printf("in loop\n");
            if (was && !now) printf(crashed ? "crashcut\n" : "out\n");
            (void)prev_loop; (void)prev_crash;
        }
        printf("end active=%d\n", b3_sfx_boost_active());
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "surftab")) {
        /* the two id-keyed tables the surface emitter walks, and the
         * ten-row curve sets, straight out of the module. */
        int id, row, k;
        for (id = 0; id < B3_SFX_SURF_IDS; id++)
            printf("SURFID %d %d %d %d\n", id, (int)b3_sfx_surface_wave(id),
                   b3_sfx_surface_row(id), b3_sfx_surface_slip_active(id));
        for (row = 0; row < B3_SFX_SURF_ROWS; row++)
            for (k = 0; k < 2; k++)
                printf("SURFROW %d %d %.9g %.9g %.9g %.9g %.9g %.9g\n", k, row,
                       (double)g_surf_curve[k][row][0],
                       (double)g_surf_curve[k][row][1],
                       (double)g_surf_curve[k][row][2],
                       (double)g_surf_curve[k][row][3],
                       (double)g_surf_curve[k][row][4],
                       (double)g_surf_curve[k][row][5]);
        for (id = 0; id < B3_SFX_SURF_IDS; id++)
            printf("SKIDW %d %.9g\n", id, (double)b3_sfx_skid_surface_weight(id));
        for (row = 0; row < 3; row++)
            for (k = 0; k < 4; k++)
                printf("SKIDCURVE %d %d %.9g %.9g %.9g %.9g\n", row, k,
                       (double)g_skid_curve[row][k][0],
                       (double)g_skid_curve[row][k][1],
                       (double)g_skid_curve[row][k][2],
                       (double)g_skid_curve[row][k][3]);
        return 0;
    }
    if (argc > 6 && !strcmp(argv[1], "surface")) {
        float g = 0.0f, p = 0.0f;
        int ok = b3_sfx_surface_slot(atoi(argv[2]), atoi(argv[3]),
                                     atoi(argv[4]), (float)atof(argv[5]),
                                     (float)atof(argv[6]), &g, &p);
        printf("%d %.9g %.9g\n", ok, (double)g, (double)p);
        return 0;
    }
    if (argc > 6 && !strcmp(argv[1], "skid")) {
        float g = 0.0f, hz = 0.0f;
        float tot = (float)atof(argv[3]);
        int ok = b3_sfx_skid_sample(atoi(argv[2]), &tot, atoi(argv[4]),
                                    atoi(argv[5]), (float)atof(argv[6]),
                                    &g, &hz);
        printf("%d %.9g %.9g %.9g\n", ok, (double)g, (double)hz,
               (double)tot);
        return 0;
    }
    if (argc > 7 && !strcmp(argv[1], "skidwheel")) {
        /* skidwheel <speed> <omega> <radius> <contact> <mode> <acc0> [n] */
        B3SfxWheel w;
        float acc = (float)atof(argv[7]);
        int n = (argc > 8) ? atoi(argv[8]) : 1, i;
        memset(&w, 0, sizeof w);
        w.omega = (float)atof(argv[3]);
        w.radius = (float)atof(argv[4]);
        w.contact = atoi(argv[5]);
        w.mode = atoi(argv[6]);
        w.surface = 1;
        for (i = 0; i < n; i++)
            printf("%.9g\n",
                   (double)b3_sfx_skid_wheel(&w, (float)atof(argv[2]), 0, &acc));
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "passmaster")) {
        int n = atoi(argv[2]), i;
        for (i = 0; i < n; i++) b3_sfx_tick();
        printf("%.9g\n", (double)b3_sfx_pass_master());
        return 0;
    }
    if (argc > 3 && !strcmp(argv[1], "resolve")) {
        B3SfxShot sh;
        int ev = atoi(argv[2]);
        float imp = (float)atof(argv[3]);
        int ok = b3_sfx_resolve((B3SfxEvent)ev, imp, 0, &sh);
        printf("%d %.9g %.9g %.9g %s\n", ok, (double)sh.gain,
               (double)sh.pitch, (double)sh.t, ok ? sh.file : "-");
        return 0;
    }
    fprintf(stderr, "usage: %s table | resolve <event> <impulse>\n", argv[0]);
    return 2;
}
#endif

/* The gate the port uses.  Retail's literal is 40.0 compared against a
 * closing speed in m/s (~89.5 mph), which a same-direction overtake never
 * reaches -- the number reads exactly like an mph value authored against
 * an m/s quantity, and that single slip also explains why retail's speed
 * lerp is permanently clamped.  The port defaults to the mph reading so
 * the whoosh is audible on a normal pass (TUNED, user-authorised);
 * B3_SFX_RETAIL_PASS_GATE=1 restores the literal.                     [S] */
float b3_sfx_pass_min_speed(void)
{
    static int retail = -1;
    if (retail < 0) retail = getenv("B3_SFX_RETAIL_PASS_GATE") != NULL;
    return retail ? B3_SFX_PASS_MIN_SPEED_RETAIL
                  : B3_SFX_PASS_MIN_SPEED_TUNED;
}

/* Port of FUN_00146530.  Every named constant is the recovered one; the
 * only deviations are the gate above and the fact that the port's mixer
 * cannot re-position a live voice each frame the way FUN_001CC3E0 does
 * (@0x0014663C), so the whoosh is placed once, at the traffic car.  GLUE */
int b3_sfx_traffic_pass(B3SfxPassState *st, float closing_ms,
                        const float ppos[3], const float pdir[3],
                        const float pright[3],
                        const float tpos[3], const float tdir[3],
                        int small)
{
    float pd[3], td[3], rel[3], lead_pt[3], to_t[3];
    float n, lead, cur, a, dot2, W, L, u, t, gain, pitch, jitter, mv;
    int i, ev;

    if (!st || !ppos || !pdir || !tpos || !tdir) return -1;
    if (!st->primed && st->voice == 0) st->voice = -1;   /* zeroed state */
    if (st->cooldown > 0) st->cooldown--;

    /* ---- RE-POSITION the live voice, retail's @0x0014663C ------------ *
     * FUN_00146530 re-runs FUN_001CD180 into a fresh params block, writes
     * only the traffic car's CURRENT position into it and hands it to
     * FUN_001CC3E0 every frame the voice is alive; gain and rate stay at
     * the block's -1.0 "unchanged".  The audible consequence is that the
     * 3D path re-evaluates the 15/50 roll-off against the MOVING source,
     * so the 0.25 s peak of the sample lands with the car right beside the
     * listener.  The first port could not move a live voice and pinned it
     * at the trigger point; this does what retail does.                [C] */
    if (st->voice >= 0) {
        if (b3_sfx_voice_active(st->voice)) {
            voice_set(st->voice,
                      gain_ceiling(st->voice_gain)
                          * dist_att_xyz(tpos[0], tpos[1], tpos[2]) * g_master,
                      st->voice_pitch);
        } else {
            st->voice = -1;
        }
    }

    /* normalise the two headings */
    n = sqrtf(pdir[0]*pdir[0] + pdir[1]*pdir[1] + pdir[2]*pdir[2]);
    if (n < 1e-4f) return -1;
    for (i = 0; i < 3; i++) pd[i] = pdir[i] / n;
    n = sqrtf(tdir[0]*tdir[0] + tdir[1]*tdir[1] + tdir[2]*tdir[2]);
    for (i = 0; i < 3; i++) td[i] = (n > 1e-4f) ? tdir[i] / n : pd[i];

    /* alignment: dot^2, so head-on and same-direction both count      [C] */
    a = pd[0]*td[0] + pd[1]*td[1] + pd[2]*td[2];
    dot2 = a * a;

    /* the lead point: pushed ahead so the sample's 0.25 s peak lands on
     * the real pass.  pitch here is the previous frame's nominal 1.0. [C] */
    lead = -(B3_SFX_PASS_SAMPLE_MID / B3_SFX_PASS_MAX_PITCH) * (-closing_ms)
         - B3_SFX_PASS_CAR_LENGTH * 0.5f;
    for (i = 0; i < 3; i++) lead_pt[i] = ppos[i] + pd[i] * lead;
    for (i = 0; i < 3; i++) to_t[i] = tpos[i] - lead_pt[i];
    n = sqrtf(to_t[0]*to_t[0] + to_t[1]*to_t[1] + to_t[2]*to_t[2]);
    if (n < 1e-4f) return -1;
    cur = (pd[0]*to_t[0] + pd[1]*to_t[1] + pd[2]*to_t[2]) / n;

    if (!st->primed) { st->primed = 1; st->prev_dot = cur; return -1; }

    /* the fire condition: the traffic car crosses the plane          [C] */
    {
        int cross = (st->prev_dot >= 0.0f && cur < 0.0f);
        st->prev_dot = cur;
        if (!cross) return -1;
    }
    if (st->cooldown > 0) return -1;
    if (closing_ms < b3_sfx_pass_min_speed()) return -1;

    /* lateral offset -> u, the centring factor                       [C] */
    W = B3_SFX_PASS_PLANE_WIDTH * dot2;
    if (W < 1e-4f) return -1;
    for (i = 0; i < 3; i++) rel[i] = to_t[i];
    L = pright ? (rel[0]*pright[0] + rel[1]*pright[1] + rel[2]*pright[2])
               : 0.0f;
    if (L < 0.0f) L = -L;
    if (L > W) L = W;
    u = (W - L) / W;

    /* retail's t is pinned at 1 by its own gate; keep the lerp honest
     * against the port's (lower) gate rather than silently assuming 1. */
    t = (B3_SFX_PASS_MAX_SPEED - closing_ms)
      / (B3_SFX_PASS_MAX_SPEED - B3_SFX_PASS_MIN_SPEED_RETAIL);
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;

    ev = small ? B3_SFX_TRAFFIC_PASS_SMALL : B3_SFX_TRAFFIC_PASS_BIG;
    /* masterVol = max(mgr+0xD8, 0.09) * 2.0   @0x0014654C, and mgr+0xD8 is
     * ramped to 1.0 by FUN_00145F60 -- so this is 2.0 in normal driving,
     * not the 0.18 floor. See the note in burnout3_sfx.h.             [C] */
    mv = b3_sfx_pass_master();
    if (mv < B3_SFX_PASS_MASTER_FLOOR) mv = B3_SFX_PASS_MASTER_FLOOR;
    mv *= B3_SFX_PASS_MASTER_SCALE;
    gain = (small ? B3_SFX_PASS_LITTLE_SCALAR : B3_SFX_PASS_BIG_SCALAR)
         * (B3_SFX_PASS_MIN_VOLUME
            + t * (B3_SFX_PASS_MAX_VOLUME - B3_SFX_PASS_MIN_VOLUME))
         * u * dot2 * mv;
    /* ADDITIVE +/-0.1 jitter (r = lcg()%2000, r*0.0001 - 0.1)        [C] */
    jitter = (float)(sfx_rand() % 2000u) * 0.0001f - 0.1f;
    pitch = ((B3_SFX_PASS_MIN_PITCH
              + t * (B3_SFX_PASS_MAX_PITCH - B3_SFX_PASS_MIN_PITCH))
             + jitter)
          * (B3_SFX_PASS_EDGE_PITCH
             + u * (B3_SFX_PASS_CENTRE_PITCH - B3_SFX_PASS_EDGE_PITCH));
    if (gain <= 0.0005f || pitch <= 0.05f) return -1;

    st->cooldown = B3_SFX_PASS_COOLDOWN;
    {
        B3SfxShot sh;
        int h;
        if (!b3_sfx_resolve((B3SfxEvent)ev, closing_ms, -1, &sh)) return -1;
        sh.gain  = gain;
        sh.pitch = pitch;
        h = b3_sfx_play_shot((B3SfxEvent)ev, &sh, tpos[0], tpos[1], tpos[2]);
        if (h >= 0) {
            st->voice = h;
            st->voice_gain = gain;
            st->voice_pitch = pitch;
        }
        return h;
    }
}

