#ifndef BURNOUT3_SFX_H
#define BURNOUT3_SFX_H
/* Crash / slam / takedown / boost SOUND EFFECTS.
 *
 * This is the game's own audio EVENT system, recovered from the retail XBE
 * and the shipped RenderWare Audio banks.  Full evidence, with addresses and
 * the emulation transcripts that pinned every number, is in docs/RE_SFX.md.
 *
 * The short version of what was recovered:
 *
 *   * Wave names are 12-character base-40 values (the FUN_001AECC0 alphabet,
 *     the same packing vlist.bin uses for car ids).  A sound-event site does
 *         MOV EAX, <address of the packed name>
 *         PUSH bank ; PUSH randomise-flag ; CALL 0x001C99D0
 *     and 0x001C99D0 is a lower-bound binary search over the bank with the
 *     key taken MODULO 40*40 (`__aullrem` by 0x640) -- i.e. the last two
 *     base-40 characters are masked off.  Those two characters are the
 *     variant index and the variant COUNT, so "IMPACTFATA" in the table
 *     matches the bank's "impactfata16".."impactfata66" and the randomise
 *     flag picks one of the six.  That is why the shipped wave files are
 *     named `<base10><index><count>`.
 *
 *   * Each emitter owns a six-float tuple in the per-racecar audio object
 *         {min impulse, max impulse, min gain, max gain, min pitch, max pitch}
 *     written by the real initialiser FUN_0014A710.  Every emitter computes
 *         t    = clamp01((impulse - minImpulse) / (maxImpulse - minImpulse))
 *         gain = minGain  + t * (maxGain  - minGain)
 *         rate = wave.sampleRate * (minPitch + t*(maxPitch - minPitch)) * v
 *     with v = 1 + U(-0.1,+0.1) from FUN_0014A6B0, and refuses to play at
 *     all when impulse < minImpulse.  That single law is the whole
 *     "small tap vs huge crash" behaviour.
 *
 * OWNERSHIP: this module (both files) belongs to the SFX agent.  The harness
 * calls only this contract; the burnout3_full.c call sites are patched by the
 * orchestrator on landing (see scratchpad/sfx/integration_sfx.md).
 */

#include <stdint.h>

/* ---------------------------------------------------------------------- */
/* Events.  Each is one real emitter function in the 0x0014Bxxx..0x00156xxx
 * audio module; the address is the emitter, the wave is the base-40 name it
 * passes to 0x001C99D0.                                                   */
typedef enum {
    B3_SFX_IMPACT_NUDGE = 0, /* FUN_0014F3E0  IMPACTNUDG  car-vs-car, alive  */
    B3_SFX_IMPACT_FATAL,     /* FUN_0014F690  IMPACTFATA  car-vs-car, wrecked*/
    B3_SFX_IMPACT_FATAL_CM,  /* FUN_0014F130  IMPACTFATA  crash-mode impact  */
    B3_SFX_IMPACT_WORLD,     /* FUN_0014EEA0  IMPACTWORL  car-vs-world       */
    B3_SFX_GLASS_FRONT,      /* FUN_0014D5F0  GLASSFRONT                     */
    B3_SFX_GLASS_SIDES,      /* FUN_0014D8A0  GLASSSIDES                     */
    B3_SFX_GLASS_WINDS,      /* FUN_0014DB50  GLASSWINDS                     */
    B3_SFX_PANEL_L_DEFORM,   /* FUN_0014EB00  CARPARTLDE                     */
    B3_SFX_PANEL_M_DEFORM,   /* FUN_0014ECB0  CARPARTMDE                     */
    B3_SFX_PANEL_L_REMOVE,   /* FUN_0014FC80  CARPARTLRE                     */
    B3_SFX_PANEL_M_REMOVE,   /* FUN_0014FE60  CARPARTMRE                     */
    B3_SFX_PANEL_L_PROP,     /* FUN_00150040  CARPARTLPR  (gain x0.5)        */
    B3_SFX_PANEL_M_PROP,     /* FUN_00150260  CARPARTMPR  (gain x0.5)        */
    B3_SFX_PANEL_W_PROP,     /* FUN_00150480  CARPARTWPR                     */
    B3_SFX_SLAM,             /* FUN_00140610  SLAM______  takedown slam      */
    B3_SFX_SHUNT,            /* FUN_00140480  SHUNT_____  light shunt        */
    B3_SFX_BOOST_GAIN,       /* FUN_00140DF0  BOOSTGAIN                      */
    B3_SFX_BOOST_LOSS,       /* FUN_00140EF0  BOOSTLOSS                      */
    B3_SFX_BOOST_LOOP,       /* FUN_00141D20  BOOSTLOOP  (looping)           */
    B3_SFX_BOOST_IN,         /* FUN_00136F80  "BoostIn"   racecar boost attack*/
    B3_SFX_BOOST_OUT,        /* FUN_00136F80  "BoostOut"  racecar boost release*/
    B3_SFX_SCRAPE_LO,        /* FUN_001521C0  CARSCRAPLO (looping)           */
    B3_SFX_SCRAPE_HI,        /* FUN_001521C0  CARSCRAPHI (looping)           */
    B3_SFX_KERB,             /* FUN_00150B90  KURBBOUNCE                     */
    B3_SFX_AIRRAM,           /* FUN_00151490  AIRRAM                         */
    B3_SFX_EXPLOSION,        /* FUN_001516C0  EXP                            */
    B3_SFX_FIRE,             /* FUN_0014B600  FIRE       (looping)           */
    B3_SFX_STEAM,            /* FUN_00151B70  STEAMRADAT (looping)           */
    B3_SFX_CAR_ALARM,        /* 0x0015204D    CARALARMLP (looping)           */
    B3_SFX_NEAR_MISS,        /* FUN_00156300  STATICPASS  scenery/tunnel     *
                              * whoosh -- NOT the traffic pass.  The old     *
                              * "near miss" label was wrong: FUN_00155FD0    *
                              * drives it off a WORLD-GEOMETRY box probe     *
                              * ahead of the car (fires on entry, and on     *
                              * exit after >= 0.5 s inside), and the retail  *
                              * near-miss SCORER FUN_00194EE0 calls nothing  *
                              * in the audio module at all.              [C] */
    B3_SFX_TRAFFIC_PASS_BIG,  /* FUN_00146530 "bstpsl0%i" van/lorry/bus pass */
    B3_SFX_TRAFFIC_PASS_SMALL,/* FUN_00146530 "bstpss0%i" car/small-van pass */

    /* ---- DRIVING-TIME LOOPS (see the block comment below) -------------- *
     * The five road-surface beds FUN_00136610 plays, named through the
     * 20-entry base-40 table at 0x0039BBF8 (index = the eCS_* surface id,
     * `lea eax,[esi*8+0x39bbf8]` @0x00136B0F).  They live in the TRACK
     * bank, not crashmod/generic.                                      [C] */
    B3_SFX_SURFACE_TAR,      /* FUN_00136610  TAR    tar.wav             */
    B3_SFX_SURFACE_GVL,      /* FUN_00136610  GVL    gvl.wav             */
    B3_SFX_SURFACE_WOOD,     /* FUN_00136610  WOOD   wood.wav            */
    B3_SFX_SURFACE_METAL,    /* FUN_00136610  METAL  metal.wav           */
    B3_SFX_SURFACE_SNOW,     /* FUN_00136610  SNOW   snow.wav            */
    /* The three Sound/Skids samples FUN_0013DE10 crossfades on slip/spin,
     * loaded by FUN_0013DBC0 @0x0013DC4E from 0x003EC108 + i*0x48.    [C] */
    B3_SFX_SKID_1,           /* FUN_0013DE10  CONST01 const01.wav        */
    B3_SFX_SKID_2,           /* FUN_0013DE10  CONST02 const02.wav        */
    B3_SFX_SKID_3,           /* FUN_0013DE10  CONST03 const03.wav        */
    /* Gear change: the record is bound once at 0x00136059 (MOV EAX,
     * 0x0039BD30 -> obj+0x18C) and FUN_00136C50 plays it whenever the
     * gear obj+0x148 rises.                                           [C] */
    B3_SFX_GEAR,             /* FUN_00136C50  GEAR______ gear______11.wav*/
    B3_SFX_COUNT
} B3SfxEvent;

/* ===================================================================== *
 *  THE DRIVING-TIME LOOP EMITTERS  (2026-08-13)
 *
 *  The per-frame racecar-audio update is FUN_00136120 [C]:
 *
 *      FUN_00136120(vehicle)                     this = racecar audio obj
 *        -> FUN_001372F0   boost/turbo block  @0x0013612A
 *             -> FUN_0013DCA0                 @0x00137301
 *                  -> FUN_0013DE10  x2        THE TYRE SKID/SQUEAL LOOPS
 *        -> FUN_00136610                      @0x00136132
 *                                             THE ROAD-SURFACE LOOPS
 *        -> FUN_00136C50                      @0x00136138  GEAR CHANGE
 *        -> FUN_0013E640                      @0x00136148  ENGINE REV
 *
 *  ---------------------------------------------------------------------
 *  1. FUN_00136610 -- the ROAD SURFACE loops, FOUR looping voices    [C]
 *
 *  It walks four 0x40-byte slots at 0x00479DB0..0x00479E70 (the loop's
 *  `add edi,0x40 ; cmp edi,0x479eb0 ; jl` @0x00136C08).  Slot i reads the
 *  wheel record for (i & 1): wheel0 at veh+0x820, wheel1 at veh+0x8E0
 *  (`mov eax,[esp+ecx*4+0x9c]` @0x001367E7), i.e. the two FRONT wheels.
 *
 *    slots 0,1 (edi <= 0x479DF0)  wave = the FIXED name at 0x0039BC00
 *                                 (= TAR, `mov eax,0x39bc00` @0x00136B02)
 *                                 curves = table set A @0x0040E130..
 *    slots 2,3                    wave = 0x0039BBF8[surface]
 *                                 (`lea eax,[esi*8+0x39bbf8]` @0x00136B0F)
 *                                 curves = table set B @0x0040E220..
 *    slots 0 and 3 add +0.24 to the pitch (0x003B1A74 @0x00136A6C) so the
 *    two voices of a pair cannot phase-lock.
 *
 *  Both sets are ten rows deep, indexed by the surface->row byte table at
 *  0x0040E318 (`movsx eax,[esi+0x40e318]` @0x00136912); the rows are the
 *  ValueDB group names None/Tarmac/Concrete/Offline Tarmac/Gravel/
 *  Pavement/Snow/Offline Gravel/Metal/Wood (recovered by executing the
 *  registrar FUN_00137F50 with EDI = 0x0040E130 and reading the keys off
 *  the ValueDB registrar 0x001AEE20, exactly as RE_SFX.md section 3 does
 *  for the crash tuples).                                             [C]
 *
 *      t     = clamp01((speed - MinSpeed) / (MaxSpeed - MinSpeed))
 *      gain  = lerp(MinVol,   MaxVol,   t) * (1 + 4.0  * s)
 *      pitch = (lerp(MinPitch, MaxPitch, t) + slotOffset) * (1 - 0.15 * s)
 *      s     = min(|slip|, 8.0) * 0.125        0x003B16B0 / 0x003B1728
 *              4.0 @0x003B1690, -0.15 @0x003B1BE4, +0.24 @0x003B1A74
 *
 *  `slip` is the audio object's own per-front-wheel skid value (obj+0x34,
 *  obj+0x38); the function also keeps a lag-limited copy at obj+0x184 with
 *  the per-frame delta clamped to [-0.4, +0.15] (0x003B1BE8 / 0x00384A80)
 *  which nothing else in the module reads.  On the surfaces in the jump
 *  table at 0x00136C2C/0x00136C34 -- Tarmac, Concrete, Chevrons, Cobbles,
 *  Paving, Offline Tarmac, Offline Concrete, Offline Pavement -- the slip
 *  term is FORCED TO ZERO (the case branch @0x001368FC), i.e. the "the
 *  loop gets louder as you slide" behaviour is a LOOSE-SURFACE effect
 *  only; on tarmac the squeal comes from the Sound/Skids samples.     [C]
 *
 *  The voice is restarted whenever the row changes and stopped when the
 *  surface id is 0 or the wheel has no contact (`cmp eax,[ebx+0x10]`
 *  @0x00136A74, the stop path @0x00136A83).                           [C]
 *
 *  SHIPPED table set B (ValueDB Sound/Surface.cfg overrides; where a key
 *  has no override the compiled-in default stands -- both were read out
 *  of the executed registrar):                                        [C]
 *      row              MinSpd MaxSpd MinVol MaxVol MinPitch MaxPitch
 *      0 None              0     0      0     0.00    1.00    1.00
 *      1 Tarmac            0    44      0     0.20    0.90    1.00
 *      2 Concrete          0    44      0     0.35    0.60    0.70
 *      3 Offline Tarmac    0     0      0     0.00    1.00    1.00
 *      4 Gravel            0    44      0     0.40    0.90    1.00
 *      5 Pavement          0    44      0     0.18    1.40    1.40
 *      6 Snow              0    44      0     0.40    0.70    0.70
 *      7 Offline Gravel    0     0      0     0.00    1.00    1.00
 *      8 Metal            10    44      0     0.40    1.00    1.00
 *      9 Wood              0    44      0     0.40    0.95    1.00
 *  Set A ("Tarmac <field>" keys) ships with MaxVol 0 on every row except
 *  Wood (0.2) -- so the always-TAR layer is effectively SILENT in the
 *  retail build and only the surface-indexed pair is audible.         [C]
 *
 *  ---------------------------------------------------------------------
 *  2. FUN_0013DE10 -- the TYRE SKID / SQUEAL loops                    [C]
 *
 *  FUN_0013DCA0 calls it TWICE, once per side: side 0 = wheels {1,3}
 *  (`lea edx,[esi+0x8e0]` / `[eax+0x760]`) with flag 1, side 1 =
 *  wheels {0,2} with flag 0 (@0x0013DDAD / @0x0013DDEC).  Each side owns
 *  three looping voices, one per Sound/Skids sample.
 *
 *  Per wheel (the `iVar10 = 2` loop @0x0013DE70):
 *      raw  = (speed - omega * (1/2pi) * radius * 2pi) * ModeMul[wheel+0x78]
 *             (0.15915494 @0x003B1734, 6.2831855 @0x003B1738)
 *      if (raw > 0) raw *= SlipSpeedMul * speed        0.025 @0x003EC218
 *      acc += clamp(raw - acc, LagMin, LagMax)         -0.35 / +0.75
 *      acc  = clamp(acc, -MaxSpin, +MaxSlip)           16 / 16
 *      no contact (wheel+0xB3 == 0) -> raw = 0
 *      total += SurfaceWeight(wheel+0xB0) * acc
 *  SurfaceWeight is the jump table at 0x0013E4D0/0x0013E4DC: 1.0 on
 *  Tarmac/Concrete/Chevrons/Cobbles/Paving/RumbleStrip/Offline Tarmac/
 *  Offline Concrete/Offline Pavement/Offline RumbleStrip, 0.5 on
 *  SnowyCobbles/MetalGrille/Snow/Offline Snow, 0.0 everywhere else --
 *  the exact complement of the surface-loop mute list above.          [C]
 *  `total` is then clamped to the same +/-16 window.
 *
 *  Per sample (the 3-iteration loop @0x0013E000, record = 0x003EC108 +
 *  i*0x48, curves at +0x08):
 *      total >= 0 (SLIP, wheel slower than the car -- braking/lock-up)
 *          gain    = piecewise(SlipVolume,    total)
 *          rateHz  = piecewise(SlipFrequency, total)
 *      total <  0 (SPIN, wheel faster than the car -- wheelspin)
 *          gain    = piecewise(SpinVolume,    -total)
 *          rateHz  = piecewise(SpinFrequency, -total)
 *      rate = wave.sampleRate + rateHz + (side ? LeftPitch : RightPitch)
 *             (+1000 / -1000 Hz, 0x003EC210 / 0x003EC214)
 *      if (racecar+0x18FA) gain *= CrashVolumeMultiplier   0.7
 *      gain <= 1.52588e-05 (0x00384208) -> the voice is stopped
 *  Each "piecewise" is a two-point clamp-and-lerp over
 *  {(x0,y0),(x1,y1)}; x is first clamped into [x0,x1].                [C]
 *
 *  SHIPPED Sound/Skids values (ValueDB over the compiled defaults):   [C]
 *      sample  SlipVol          SlipFreq(Hz)     SpinVol          SpinFreq(Hz)
 *      1       0.75,0 -> 3,0.42   2,1000 -> 8,-3500   0.95,0 -> 3,1.15   2,1000 -> 8,-3500
 *      2       0.5,0 -> 5.5,0.8   3.5,1000 -> 10,-3500 0.95,0 -> 1,0.225 3.5,1000 -> 10,-3500
 *      3       6,0 -> 10,0.3      10,1000 -> 16,-2500  2,0 -> 10,0.3     10,1000 -> 16,-2500
 *      Maximum Slip/Spin 16, SlipSpin Lag Min/Max -0.35/+0.75,
 *      Slip Speed Multipler 0.025, Crash Volume Multipler 0.7,
 *      Left/Right Pitch Offset +1000/-1000 Hz,
 *      Emitter Distance AT 1.0 / RIGHT 3.0,
 *      Wheel Mode 'Driving' Multipler 0.5, every other mode 1.0.
 *
 *  ---------------------------------------------------------------------
 *  3. FUN_00136C50 -- the GEAR CHANGE                                 [C]
 *      prev = obj+0x148;  now = racecar+0x10B8
 *      if (prev != 0 && prev != -1 && prev < now) play obj+0x18C
 *      obj+0x148 = now
 *  i.e. one shot on every UPSHIFT, none out of neutral or reverse.
 * ===================================================================== */

/* --- FUN_00136610 constants, all read out of the image ---------------- */
#define B3_SFX_SURF_SLIP_CLAMP     8.0f    /* 0x003B16B0 */
#define B3_SFX_SURF_SLIP_SCALE     0.125f  /* 0x003B1728 */
#define B3_SFX_SURF_GAIN_K         4.0f    /* 0x003B1690 */
#define B3_SFX_SURF_PITCH_K       -0.15f   /* 0x003B1BE4 */
#define B3_SFX_SURF_DETUNE         0.24f   /* 0x003B1A74, slots 0 and 3 */
#define B3_SFX_SURF_LAG_MIN       -0.4f    /* 0x003B1BE8 */
#define B3_SFX_SURF_LAG_MAX        0.15f   /* 0x00384A80 */
#define B3_SFX_SURF_ROWS          10
#define B3_SFX_SURF_IDS           40       /* eCS_None .. eCS_CrashRamp   */

/* --- FUN_0013DE10 / Sound/Skids constants ----------------------------- */
#define B3_SFX_SKID_MAX_SLIP      16.0f    /* vdb 0x...; 0x003EC21C       */
#define B3_SFX_SKID_MAX_SPIN      16.0f    /* 0x003EC220                  */
#define B3_SFX_SKID_LAG_MIN       -0.35f   /* 0x003EC1E0                  */
#define B3_SFX_SKID_LAG_MAX        0.75f   /* 0x003EC1E4                  */
#define B3_SFX_SKID_SLIP_SPEED_MUL 0.025f  /* 0x003EC218                  */
#define B3_SFX_SKID_CRASH_VOL_MUL  0.7f    /* 0x003EC1E8                  */
#define B3_SFX_SKID_PITCH_LEFT   1000.0f   /* 0x003EC210, Hz              */
#define B3_SFX_SKID_PITCH_RIGHT (-1000.0f) /* 0x003EC214, Hz              */
#define B3_SFX_SKID_EMIT_AT        1.0f    /* 0x003EC208, forward metres  */
#define B3_SFX_SKID_EMIT_RIGHT     3.0f    /* 0x003EC20C, lateral metres  */
#define B3_SFX_SKID_MODE_DRIVING   0.5f    /* Wheel Mode 'Driving' mult   */
#define B3_SFX_SKID_GAIN_EPS  1.52588e-05f /* 0x00384208                  */
#define B3_SFX_SURF_EMIT_OFF       5.5f    /* 0x003EC074 / 0x003EC078     */

/* One wheel as the two emitters read it out of the live record
 * (veh+0x820 + i*0xC0).                                                  */
typedef struct {
    float omega;    /* +0x5C rad/s                                        */
    float radius;   /* +0x50                                              */
    int   surface;  /* +0xB0 low byte -- the eCS_* id                      */
    int   contact;  /* +0xB3                                              */
    int   mode;     /* +0x78 wheel mode, 0..7 (0 = 'Driving')             */
    float pos[3];   /* +0x00 world position                                */
} B3SfxWheel;

/* Everything the two loop emitters + the gear shot keep between frames.
 * All-zero is a valid initial state; one per car.                        */
typedef struct {
    int   surf_voice[4];   /* obj+0x164..+0x170                            */
    int   surf_row[4];     /* obj+0x174..+0x180, -1 = no voice             */
    float surf_lag[2];     /* obj+0x184/+0x188                             */
    float skid_acc[2][2];  /* skid obj+0x24 / +0x2C                        */
    int   skid_voice[2][3];/* skid obj+0x0C..+0x20                         */
    int   gear_prev;       /* obj+0x148                                    */
    int   primed;
} B3SfxDriveState;

/* One frame of the racecar's state, as FUN_00136120 hands it on. */
typedef struct {
    B3SfxWheel wheel[4];
    int   wheel_count;   /* veh+0x1169                                     */
    float speed_ms;      /* veh+0xBC                                       */
    int   gear;          /* veh+0x14C8 (reverse = -1) / racecar+0x10B8     */
    int   crashed;       /* racecar+0x18FA -- the Crash Volume flag        */
    float pos[3];        /* racecar+0x40 world position                    */
    float right[3];      /* racecar+0x10                                   */
    float fwd[3];        /* racecar+0x30                                   */
} B3SfxDriveIn;

/* THE per-frame driving-audio update.  Ports FUN_00136120's three loop
 * children in the order retail runs them.  Call once per frame per car
 * that owns loops (the harness drives the player).                       */
void b3_sfx_drive_tick(B3SfxDriveState* st, const B3SfxDriveIn* in);

/* Stop every loop this state owns (race end / respawn / pause).          */
void b3_sfx_drive_stop(B3SfxDriveState* st);

/* ---- the pure laws, for tools/validate_sfx.py ------------------------ */

/* 0x0039BBF8[id] -> which of the five surface waves, as a B3SfxEvent
 * (B3_SFX_COUNT when the id has no wave).                               */
B3SfxEvent b3_sfx_surface_wave(int surface_id);
/* 0x0040E318[id] -> the curve row 0..9.                                  */
int   b3_sfx_surface_row(int surface_id);
/* The FUN_00136610 jump table @0x00136C2C: 1 = the slip term is used,
 * 0 = the surface forces it to zero.                                     */
int   b3_sfx_surface_slip_active(int surface_id);
/* The FUN_0013DE10 surface weight: 1.0 / 0.5 / 0.0.                      */
float b3_sfx_skid_surface_weight(int surface_id);

/* FUN_00136610's slot law.  `slot` 0..3, `slip` is obj+0x34/+0x38.
 * Returns 1 when the slot has a voice (surface id != 0 and contact),
 * filling gain and the pitch MULTIPLIER.                                 */
int b3_sfx_surface_slot(int slot, int surface_id, int contact,
                        float speed_ms, float slip,
                        float* out_gain, float* out_pitch);

/* FUN_0013DE10's per-wheel slip integrator.  `acc` is updated in place
 * and returned (before the surface weight is applied).                   */
float b3_sfx_skid_wheel(const B3SfxWheel* w, float speed_ms, int reverse,
                        float* acc);

/* FUN_0013DE10's per-sample response.  `sample` 0..2, `total` the summed
 * weighted slip, `left` the side flag (1 = the +1000 Hz side).  Fills the
 * gain and the ABSOLUTE playback rate in Hz for a wave whose native rate
 * is `native_hz`.  Returns 1 when the voice should be running.           */
/* NOTE `total` is IN/OUT: when the value is negative the emitter takes the
 * SPIN branch and writes |total| back into the slot the three-sample loop
 * re-reads (`movss [esp+0x10],xmm0` @0x0013E20B / reload @0x0013E49B), so
 * samples 2 and 3 of that same frame run the SLIP curves instead.  Retail
 * quirk, reproduced verbatim -- pass the SAME variable through samples
 * 0, 1, 2 in that order.                                              [C] */
int b3_sfx_skid_sample(int sample, float* total, int left, int crashed,
                       float native_hz, float* out_gain, float* out_rate_hz);

/* ---- BODY SCRAPE (the wall-grind loop pair) --------------------------
 * FUN_001521C0 owns a CARSCRAPLO + CARSCRAPHI voice PER CONTACT SLOT (the
 * six 0x40-byte slots at audioObj+0x10, `mov edi,6 ; call 0x001521C0 ;
 * add esi,0x40` @0x0014C1D0), started together and crossfaded by the
 * caller FUN_0014B600 -- so the LEVELS are [S] (RE_SFX.md section 4) and
 * the harness supplies the crossfade weight itself.  `hi` is that weight:
 * 0 = all CARSCRAPLO, 1 = all CARSCRAPHI.  Calling with contact = 0 stops
 * both.  The trigger (a live wall contact) is harness GLUE -- it is the
 * same event that already drives b3_pfx_grind_spark.                     */
void b3_sfx_scrape_tick(int contact, float gain, float hi,
                        const float pos[3]);
void b3_sfx_scrape_stop(void);

/* ===================================================================== *
 *  THE PASSING-TRAFFIC WHOOSH  (FUN_00146530, 2026-08-13)
 *
 *  Chain [C]:  FUN_00145F60 (traffic-audio manager, per frame)
 *                -> FUN_00146FA0(masterVol=[mgr+0xD8])   per traffic car
 *                     -> FUN_00147DF0(rec, masterVol)
 *                          -> FUN_00146530               THE EMITTER
 *
 *  The waves are ASCII literals expanded by sprintf, not packed base-40:
 *      "bstpsl0%i" @0x003AE824 (PUSH @0x00146914)   big   -> bstpsl01..04
 *      "bstpss0%i" @0x003AE830 (PUSH @0x00146940)   small -> bstpss01..04
 *  in bank 0x0040B7F4 = generic; all eight are 24000 Hz mono, 0.500 s.
 *  Variant = rand() & 0x80000003 sign-fixed to mod 4, +1
 *  (@0x001468FD..0x0014690E / @0x00146929..0x0014693A).               [C]
 *
 *  Gates, in emitter order [C]:
 *    1. traffic class 7 never whooshes            @0x00146541
 *    2. masterVol = max(masterVol, 0.09) * 2.0    @0x0014654C (0.09
 *       @0x003B1D38, 2.0 @0x003B1688)
 *    3. closing speed >= MinSpeed                 @0x00146575
 *    4. cooldown 0x3C = 60 frames = 1 s           @0x00146591/@0x001468B1
 *    5. THE FIRE CONDITION -- a zero crossing     @0x001468CA:
 *         lead      = -(SampleMidpoint/pitch)*closing - CarLength*0.5
 *         leadPoint = playerPos + norm(playerDir)*lead
 *         cur       = dot(norm(playerDir), norm(trafficPos - leadPoint))
 *         fire when prev >= 0 && cur < 0 && no voice live
 *       -- leadPoint is pushed AHEAD by SampleMidpoint/pitch seconds so
 *       the 0.25 s peak of the 0.500 s sample lands on the real pass.
 *
 *  Gain / pitch [C]:
 *      t     = clamp01((MaxSpeed - closing)/(MaxSpeed - MinSpeed))
 *      dot2  = dot(norm(playerDir), norm(trafficDir))^2
 *      W     = PlaneWidth * dot2
 *      L     = |dot(playerRight, trafficPos - leadPoint)|
 *      u     = (W - min(L,W)) / W          1 = dead centre, 0 = at the edge
 *      gain  = PassScalar * lerp(MinVolume,MaxVolume,t) * u * dot2 * masterVol
 *      pitch = (lerp(MinPitch,MaxPitch,t) + U(-0.1,+0.0999))
 *              * lerp(EdgePitch, CentrePitch, u)
 *  NOTE the pitch jitter is ADDITIVE here, not the usual multiplicative
 *  FUN_0014A6B0 +/-10%.  It coincides with the port's multiplicative
 *  `pitch_var` only because the pitch lerp is pinned at 1.0.          [C]
 *
 *  The Sound/Traffic node's compiled-in defaults (FUN_001434E0; none of
 *  these twelve has a Data/vdb.xml override, so the defaults ship) [C]:
 *      Min Speed 40.0 @0x003B1884 -> 0x0040FBF0   Max Speed 200.0 -> +0x44
 *      Min Pitch 0.3  -> +0x48                    Max Pitch 1.0   -> +0x4C
 *      Sample Midpoint 0.25 -> +0x50              Car Length 4.5  -> +0x54
 *      Min Volume 0.0 -> +0x5C                    Max Volume 0.75 -> +0x60
 *      Plane Width 18.0 -> +0x64                  Centre Pitch 1.0-> +0x68
 *      Edge Pitch 0.9 -> +0x6C
 *  The two SCALARS *are* ValueDB overrides, and -- the surprise -- they
 *  live under Sound/Boost, not Sound/Traffic (which fits the wave names:
 *  bstps = BooST PaSs).  Data/vdb.xml, hashes verified against the known
 *  "Mass (Kg)/COMPCAR1" = 0x6B81AAA0:
 *      "Big Pass Scalar"    @0x003AE628 -> 0x0040FC20  hash 0x7D47F5DD = 1.0
 *      "Little Pass Scalar" @0x003AE638 -> 0x0040FC24  hash 0xFEC430E9 = 0.8
 *  Class 1 or 2 takes the LITTLE scalar, every other class the BIG one
 *  (@0x001468F0).                                                     [C]
 *
 *  DEGENERACY [C]: gate 3 needs closing >= 40, so t = (200-closing)/160
 *  is always clamped to 1 -- in the shipped build every pass plays at
 *  MaxVolume/MaxPitch and only `u` and `dot2` vary it.
 *
 *  THE UNIT SLIP [S]: 40 m/s is ~89.5 mph of CLOSING speed, which a
 *  same-direction overtake essentially never reaches -- so in retail
 *  this is an oncoming-traffic sound.  Min/Max Speed read exactly like
 *  mph values compared against an m/s quantity, and that one slip
 *  explains both the very high gate and the permanently-pinned t.
 *  b3_sfx_traffic_pass() therefore gates on B3_SFX_PASS_MIN_SPEED,
 *  which defaults to the mph reading (TUNED, so the whoosh is audible
 *  on a normal overtake as the user asked); set B3_SFX_RETAIL_PASS_GATE
 *  in the environment to restore the literal 40.0 m/s.
 * ===================================================================== */
#define B3_SFX_PASS_MIN_SPEED_RETAIL   40.0f  /* @0x003B1884 -> 0x0040FBF0 */
#define B3_SFX_PASS_MAX_SPEED         200.0f  /* @0x003A292C -> 0x0040FBF4 */
#define B3_SFX_PASS_MIN_PITCH           0.3f  /* @0x003B1750 -> 0x0040FBF8 */
#define B3_SFX_PASS_MAX_PITCH           1.0f  /* @0x003B168C -> 0x0040FBFC */
#define B3_SFX_PASS_SAMPLE_MID         0.25f  /* @0x003B1730 -> 0x0040FC00 */
#define B3_SFX_PASS_CAR_LENGTH          4.5f  /* @0x003B1AB4 -> 0x0040FC04 */
#define B3_SFX_PASS_MIN_VOLUME          0.0f  /* xorps      -> 0x0040FC0C */
#define B3_SFX_PASS_MAX_VOLUME         0.75f  /* @0x003A55F8 -> 0x0040FC10 */
#define B3_SFX_PASS_PLANE_WIDTH        18.0f  /* @0x003B1880 -> 0x0040FC14 */
#define B3_SFX_PASS_CENTRE_PITCH        1.0f  /* @0x003B168C -> 0x0040FC18 */
#define B3_SFX_PASS_EDGE_PITCH          0.9f  /* @0x003A69C0 -> 0x0040FC1C */
#define B3_SFX_PASS_BIG_SCALAR          1.0f  /* vdb 0x7D47F5DD           */
#define B3_SFX_PASS_LITTLE_SCALAR       0.8f  /* vdb 0xFEC430E9           */
#define B3_SFX_PASS_MASTER_FLOOR       0.09f  /* @0x003B1D38              */
#define B3_SFX_PASS_MASTER_SCALE        2.0f  /* @0x003B1688              */
#define B3_SFX_PASS_COOLDOWN              60  /* 0x3C @0x001468B1, frames */
/* TUNED: 40 read as mph -> m/s.  See the unit-slip note above. */
#define B3_SFX_PASS_MIN_SPEED_TUNED  17.8816f


/* Confidence marker carried per event, same legend as docs/RE_NOTES.md. */
typedef enum { B3_SFX_C = 0, B3_SFX_S = 1, B3_SFX_Q = 2 } B3SfxConf;

/* The recovered definition of one event. All fields except `files`/`dir`
 * come out of the executable; `dir`/`files` name the extracted waves. */
typedef struct {
    const char*  name;        /* human label                                */
    const char*  wave;        /* base-40 name in the XBE, e.g. "IMPACTNUDG" */
    const char*  emitter;     /* "FUN_0014F3E0"                             */
    const char*  dir;         /* build/audio bank directory                 */
    const char*  files[8];    /* the variant wave files, NULL-terminated    */
    float        min_impulse; /* tuple[0]  (object offset in `param_off`)   */
    float        max_impulse; /* tuple[1]                                   */
    float        min_gain;    /* tuple[2]                                   */
    float        max_gain;    /* tuple[3]                                   */
    float        min_pitch;   /* tuple[4]                                   */
    float        max_pitch;   /* tuple[5]                                   */
    float        gain_scale;  /* extra constant factor read by the emitter  */
    int          param_off;   /* audio-object offset of tuple[0]; -1 = none */
    int          cooldown;    /* ticks before this event may retrigger      */
    int          cooldown_rnd;/* + rand()%cooldown_rnd ticks                */
    int          gated;       /* 1 = silent below min_impulse               */
    int          loop;        /* 1 = looping voice                          */
    int          pitch_var;   /* 1 = FUN_0014A6B0 +/-10% pitch variance     */
    B3SfxConf    conf;
} B3SfxDef;

/* What one triggered event resolves to. */
typedef struct {
    int          play;        /* 0 = gated out / on cooldown                */
    int          variant;     /* index into def->files                      */
    const char*  file;        /* build/audio-relative path of the wave      */
    float        gain;        /* 0..1                                       */
    float        pitch;       /* playback rate multiplier (1 = native)      */
    float        t;           /* normalised impulse                         */
} B3SfxShot;

/* ---------------------------------------------------------------------- */
/* Table access (pure; no state, no audio device -- this is what
 * tools/validate_sfx.py checks against the emulated real code).           */
const B3SfxDef* b3_sfx_def(B3SfxEvent ev);

/* Resolve an event at a given impulse WITHOUT touching cooldowns or the
 * mixer.  `variant_sel` picks the wave variant deterministically
 * (variant_sel < 0 => random).  Pitch variance is NOT applied here so the
 * result is exactly the emitter's lerp.                                    */
int b3_sfx_resolve(B3SfxEvent ev, float impulse, int variant_sel,
                   B3SfxShot* out);

/* One traffic car's pass state -- the emitter's [ESI+0x24]/[+0x28]/[+0x6]
 * (cur / prev plane dot, cooldown).  The caller keeps one per traffic
 * slot; all-zero is a valid initial state. */
typedef struct B3SfxPassState {
    float prev_dot;
    int   cooldown;      /* frames, counted down by the call itself      */
    int   primed;        /* 0 until the first frame has seeded prev_dot  */
    /* The live voice, so it can be RE-POSITIONED every frame the way
     * retail does (@0x0014663C, see the master-volume note below).      */
    int   voice;         /* -1 = none                                    */
    float voice_gain;    /* the gain the emitter computed at trigger     */
    float voice_pitch;
} B3SfxPassState;

/* THE TRAFFIC-AUDIO MASTER VOLUME (2026-08-13 re-verification).
 *
 * FUN_00146530 @0x0014654C does `masterVol = max(masterVol, 0.09) * 2.0`
 * and the final gain multiplies by it (@0x001469BE).  masterVol is the
 * traffic-audio manager's own +0xD8, and FUN_00145F60 @0x00146000 RAMPS
 * that field toward 1.0 at +0.01 per traffic car per frame (0x003A7ED8),
 * and back toward 0.0 at -0.1 (0x003A69C4), clamped to [0,1].  So during
 * normal driving it is pinned at 1.0 and the emitter's factor is 2.0 --
 * NOT the 0.09*2.0 = 0.18 floor the first port hard-coded, which made
 * every whoosh 11.1x (-20.9 dB) too quiet.                           [C]
 *
 * The module runs the ramp itself out of b3_sfx_tick(); these expose it. */
float b3_sfx_pass_master(void);       /* the live +0xD8 value, 0..1      */
void  b3_sfx_pass_master_set(float v);/* force it (tests / race reset)   */
#define B3_SFX_PASS_MASTER_RAMP_UP    0.01f  /* 0x003A7ED8 */
#define B3_SFX_PASS_MASTER_RAMP_DOWN  0.1f   /* 0x003A69C4 */

/* THE PASSING-TRAFFIC WHOOSH.  Port of FUN_00146530: does the zero
 * crossing, the cooldown, the u/dot2 geometry and the gain/pitch law,
 * and plays the big or little wave.  Everything is in harness world
 * space, metres and m/s.
 *
 *   st            per-traffic-slot state (see above)
 *   closing_ms    +|closing speed| (retail's param_7 is negative while
 *                 closing; pass the magnitude)
 *   ppos/pdir     player position and unit direction of travel
 *   pright        the player's right axis (retail uses the view matrix
 *                 row 0) -- used for the lateral offset L
 *   tpos/tdir     the traffic car's position and unit direction
 *   small         1 for traffic class 1/2 (car / small van), else 0
 *
 * Returns the voice handle, or -1 when nothing fired.               [C] */
int b3_sfx_traffic_pass(B3SfxPassState *st, float closing_ms,
                        const float ppos[3], const float pdir[3],
                        const float pright[3],
                        const float tpos[3], const float tdir[3],
                        int small);

/* The gate the port actually uses (see the unit-slip note). */
float b3_sfx_pass_min_speed(void);

/* Positional play of an already-resolved shot -- the distance roll-off and
 * the master gain only.  Used by b3_sfx_event_at and by emitters that
 * compute their own gain/pitch (the traffic pass). */
int b3_sfx_play_shot(B3SfxEvent ev, const B3SfxShot* sh,
                     float x, float y, float z);


/* ---------------------------------------------------------------------- */
/* Runtime (harness side).                                                 */

/* Load every referenced wave from build/audio.  Safe to call twice; missing
 * files are reported once and simply never play. Returns waves loaded.     */
int  b3_sfx_init(void);

/* Fire an event. `impulse` is in the emitter's own units (see RE_SFX.md
 * section 4 for what each one measures).  Returns a voice handle >= 0 when
 * a voice actually started, or -1 (gated, on cooldown, or no free voice).  */
int  b3_sfx_event(B3SfxEvent ev, float impulse);

/* Same, positioned; distance attenuation uses the emitter's own 15/50-unit
 * min/max distance constants (0x003B16B4 / 0x003B16B8).                    */
int  b3_sfx_event_at(B3SfxEvent ev, float impulse,
                     float x, float y, float z);

void b3_sfx_stop(int voice);            /* stop one voice (loops)           */
int  b3_sfx_voice_active(int voice);
void b3_sfx_set_listener(float x, float y, float z);
void b3_sfx_set_master(float gain);     /* default 0.9                      */

/* Advance cooldown counters. Call once per simulation tick (60 Hz).        */
void b3_sfx_tick(void);

/* ---------------------------------------------------------------------- */
/* Mixing.  Two shapes so the harness callback can use whichever is
 * cleaner; both are safe to call from the SDL audio thread.                */

/* Per-sample: advances all voices one output frame at 44100 Hz and returns
 * the summed sample in the same s16-scaled float domain burnout3_full.c's
 * audio_callback already works in.                                         */
float b3_sfx_next_sample(void);

/* Block form: ADDS `frames` samples into an existing mono s16 buffer.      */
void  b3_sfx_mix_s16(int16_t* out, int frames);

/* Diagnostics: total voices started since init (used by the race log).     */
unsigned b3_sfx_voices_started(void);

/* ---------------------------------------------------------------------- */
/* CRASH-STATE AUDIO.  docs/RE_SFX.md section 6.
 *
 * Three separate laws, all recovered; the addresses are in RE_SFX.md 6 and
 * repeated at each implementation site in burnout3_sfx.c:
 *
 *  1. THE ENGINE.  Retail never mutes or fades the crashing car's engine.
 *     It STARVES it: the crashed branch of the vehicle main path
 *     (FUN_0011BE50 @0x0011BE75..0x0011BED4) drops the gearbox to neutral
 *     and calls the engine/transmission update FUN_00121560 with
 *     throttle = wheel_omega = kick = boost = 0, every frame, for the whole
 *     crash -- while the racing path's engine/AI block (FUN_00104A90
 *     @0x00104B6A) and the drive-torque path are skipped.  In neutral at
 *     zero throttle FUN_00121560's target is 0 and its down-slew is 16
 *     rad/s per frame (9.6 for a sub-6000-rpm engine), so the engine falls
 *     to its idle floor in about half a second and stays there.  That is
 *     why a retail crash has no engine rev in it.
 *
 *  2. THE CRUNCHES.  FUN_0014D0F0 is the world-impact router: it plays
 *     IMPACTWORL (FUN_0014EEA0) for a car that is not crashed and the
 *     crashed-car impact (FUN_0014F130, the IMPACTFATA family) for one that
 *     is -- with the contact impulse divided by the car's mass (veh+0x1F0)
 *     @0x0014D223, i.e. the emitter's argument is a velocity CHANGE in m/s,
 *     which is exactly what that emitter's recovered 2..10 window measures.
 *
 *  3. SLOW MOTION.  The audio's own gates run off the game clock
 *     DAT_0060EA20, which is the DILATED clock (RE_TAKEDOWN_FX 1.1), so
 *     every crash-side timer stretches with the divisor by construction.
 *     Nothing pitches the effects down; retail's slow-motion "sound" is a
 *     dedicated pre-rendered stream (see RE_SFX.md 6.4), not a mix change.
 */

/* Per-frame crash feed.  Call ONCE per rendered frame, with the PLAYER's
 * state, next to b3_sfx_tick().  `crashed` is the harness's veh+0x210,
 * `dt` the game's own dilated frame dt (DAT_0060EA1C).  `sim_rpm` is the
 * live drivetrain rpm; it is latched on the crash edge and ignored while
 * the crash law owns the engine.                                          */
void b3_sfx_crash_tick(int crashed, const float pos[3], const float vel[3],
                       float sim_rpm, float idle_rpm, float max_rpm,
                       float dt);

/* The rpm the engine voice must play at this frame: the crashed-path
 * engine speed while a crash is running, otherwise `sim_rpm` untouched.
 * Safe to call from the audio thread.                                     */
float b3_sfx_engine_rpm(float sim_rpm);

/* 1 while the crash law owns the engine voice (diagnostics / logging).    */
int b3_sfx_crash_active(void);

/* FUN_0014D0F0's router.  `dv` is the contact impulse over the car's mass
 * (m/s).  Returns the voice handle, or -1.                                */
int b3_sfx_impact_world(float dv, int crashed, float x, float y, float z);

/* ---------------------------------------------------------------------- */
/* BOOST AUDIO.  docs/RE_BOOSTFX.md section 3.
 *
 * The racecar audio object owns FOUR boost waves, loaded by FUN_00136F80
 * (0x00136F80) from ASCII literals -- not from packed base-40 constants --
 * with FUN_001AEAA0 doing the ASCII->base-40 pack at runtime and the usual
 * FUN_001C9E50 doing the bank lookup against 0x0040B7F4:
 *
 *     obj+0xA4  "BoostLoop"  0x003AD404  @0x00136F8F   -> boostloop.wav
 *     obj+0xA8  "fire"       0x003AD3FC  @0x00136FBF   -> fire.wav
 *     obj+0xAC  "BoostIn"    0x003AD3F4  @0x00136FE7   -> boostin.wav
 *     obj+0xB0  "BoostOut"   0x003AD3E8  @0x0013700F   -> boostout.wav
 *
 * all five of which ship in sound\generic.awd (build/audio/awd_generic).  [C]
 * Two more, "BoostBig" (0x003AD3D0) and "BoostSml" (0x003AD3DC), are the
 * chained-boost stings FUN_00137600 picks between on the meter fraction; they
 * are not part of the sustain loop and are not ported.
 *
 * The mode singleton has a SECOND path for the same sound -- FUN_00141D20
 * starts the packed-name BOOSTLOOP and FIRE voices at gain 1.0
 * (0x003B168C @0x00141DD9 / 0x00141F07) positioned at the CAMERA
 * ([0x0073C610]+0x204 +0x30..0x38 @0x00141E14), i.e. head-locked, not 3D.
 * That is why the loop below is unpositioned.                            [C]
 *
 * Nine "Sound/Boost" ValueDB volumes exist (group string 0x003AD4C0,
 * "Boost In/Loop/Out/Chain/Ready Volume" + three AI variants, registered by
 * FUN_00136DA0 @0x00136E1E..0x00136F6C against BSS fields 0x00479EC0/EC4 and
 * 0x0047A018..0x0047A030).  None of the nine has an override in the shipped
 * Data/vdb.xml (the Boost.cfg path hash IS in its filedef list, so the
 * lookup is right and the answer is "no override"), and the BSS defaults have
 * no findable writer, so the LEVELS are [?] and the module uses the [C] 1.0
 * that FUN_00141D20 plays the loop at.                                      */

/* Per-frame boost feed. Call once per frame with the PLAYER's state, next to
 * b3_sfx_tick(). Rising edge -> BoostIn + the looping BoostLoop; falling edge
 * -> stop the loop + BoostOut; `crashed` cuts the loop with no release, since
 * FUN_0017F730 gates all boost FX on carObj+0x18FA and the crash bed takes
 * the mix (that last choice is GLUE).                                       */
void b3_sfx_boost_tick(int boosting, int crashed);
void b3_sfx_boost_stop(void);         /* hard stop (race end, reset)        */
int  b3_sfx_boost_active(void);       /* 1 while the loop voice is running  */

#endif
