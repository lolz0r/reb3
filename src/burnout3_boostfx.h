#ifndef BURNOUT3_BOOSTFX_H
#define BURNOUT3_BOOSTFX_H
/* BOOST EXHAUST FLAME -- the jets that come out of a car's tailpipes while it
 * is boosting.  Recovered from the retail XBE (build/burnout3.elf); full
 * evidence in docs/RE_BOOSTFX.md.  Evidence marks as elsewhere in this repo:
 * [C] execution- or byte-verified, [S] read from the disassembly, [?] open,
 * GLUE = this port's own bridging.
 *
 * THE SYSTEM, in one paragraph.  It is not a particle system and not a
 * flipbook: it is the SAME billboard-pool sprite system the light coronas
 * use, with two extra pools and a different emitter.
 *
 *   FUN_0017F730 (0x0017F730), the per-car FX dispatcher, does
 *       if (carObj+0x68  != 0) FUN_00187C70();   // the light coronas
 *       if (carObj+0x18FA == 0) FUN_001871E0();  // THE FLAME           [C]
 *   carObj+0x18FA is the CRASHED flag (docs/RE_GAMEPLAY.md 3), so a wrecked
 *   car has no flame.
 *
 *   FUN_001871E0 (0x001871E0) reads the car's own corona light table at
 *       records = *(u32*)(model + 0x1664 + 8*4) = model+0x1684  @0x0018725B
 *       count   = *(u8*) (model + 0x16AC + 8)   = model+0x16B4  @0x00187261
 *   -- i.e. light TYPE 8, the entry docs/RE_CARFX.md 4.2 left as "[?] exhaust
 *   / roof / misc".  It IS the exhaust: on COMP/Car1 the two type-8 records
 *   are (+-0.453, 0.074, -1.972) with normal (0,0,-1) -- the tailpipes.  [C]
 *
 *   For each record it emits THREE additive billboards along the record's
 *   outward normal, each half the size and half the brightness of the one
 *   before, at 0.08 / 0.14 / 0.18 units out (0x0039C16C / 0x003B1B28 /
 *   0x003B1B24), with the brightness of each multiplied by a fresh uniform
 *   random in [0.5,1] of its band -- that random IS the heat flicker.    [C]
 *
 *   The size and colour come from FUN_00179F30 (0x00179F30), driven by the
 *   boost record's own FX level at carObj+0x11B0 (= boostRecord+0x14):
 *       t <= 0            -> nothing
 *       carObj+0x1901==0  -> k=0.6,  pool 1 "coronaboost"     (blue/white)
 *       carObj+0x1901!=0  -> k=0.72, pool 2 "coronaboostred"  (orange)
 *       t >= 1            -> size = k*t,  colour = C
 *       t <  1            -> size = k,    colour = C*t
 *   with C = (0.7,0.72,0.75) @0x00415CC0 and (0.8,0.8,0.8) @0x00415CD0. [C]
 *
 *   carObj+0x11B0 is driven by the tail of the boost update FUN_0017A480
 *   (the block at 0x001797E0..0x001798F2):  1.0 while boosting, a 2.0 flare
 *   on ignition, decay at 2.0/s to 0 when the boost stops.               [C]
 *
 * OWNERSHIP: this module (both files) belongs to the boost-FX agent.  The
 * harness calls only this contract; burnout3_full.c call sites are patched by
 * the orchestrator on landing (see scratchpad/boostfx/INTEGRATION_NOTE.md).
 *
 * ART: run `python3 tools/extract_boostfx_art.py` first -- it writes
 * build/boostfx/{coronaboost,coronaboostred}.png.  The per-car emitter
 * positions are corona-table type 8 and are already in
 * build/cars/<CLS>_<Car>.lights (tools/extract_carfx_art.py).  With either
 * missing, every entry point degrades to a no-op.
 */

#define B3_BOOSTFX_MAX_CARS 16

/* ---- lifecycle --------------------------------------------------------- */
void b3_boostfx_init(void);      /* loads the two pool textures            */
int  b3_boostfx_ready(void);     /* 0 = every entry point is a no-op       */
void b3_boostfx_shutdown(void);

/* Per-car emitters from build/cars/<cls>_<base>.lights, corona type 8.
 * Returns the number of exhaust emitters loaded, 0 if the file has none. */
int  b3_boostfx_load_car(int slot, const char* cls, const char* base);
int  b3_boostfx_emitters(int slot);

/* carObj+0x1901 -- which sprite POOL (and so which texture) the car burns
 * through.  RECOVERED [C]: it is a per-car-MODEL constant, not a gameplay
 * state.  The car loader FUN_0018D0E0 clears it (@0x0018D4CB) and sets it to
 * 1 for exactly five packed base-40 car ids -- COMPCAR10, MSCLCAR10,
 * CUPECAR10, SPRTCAR10, SUPRCAR10 (@0x0018D4D4..0x0018D534) -- so the five
 * `Car10` specials get the ORANGE `coronaboostred` flame and every other car
 * the blue-white `coronaboost`.  b3_boostfx_load_car() sets it from the
 * class+base it is handed; these expose it. */
int  b3_boostfx_car_is_red(const char* cls, const char* base);
int  b3_boostfx_car_red(int slot);

/* ---- the FX level (FUN_0017A480 tail) ----------------------------------
 * Call once per car per SIM frame, before the draw.  `boosting` is the boost
 * record's +0x52; `crashed` is carObj+0x18FA (FUN_0017F730's gate); `dt` is
 * the game's own dilated frame delta (DAT_004AE1FC in the recovered code).
 *
 *     if (!fxByte) { if (!boosting) { lvl -= 2.0*dt; max(lvl,0); return; } }
 *     else if (lvl == 0)  lvl = 2.0;              // the ignition flare
 *     lvl -= 2.5*dt;  if (lvl <= 1.0) lvl = 1.0;                        [C]
 *
 * The port substitutes `fxByte := boosting` -- the retail byte is
 * (*(veh+0xCC4))+0x1033, whose writer was not chased, and with that
 * substitution the recovered branch structure reproduces exactly: instant on
 * at 1.0, a 2.0 flare whenever the flame had fully died first, 0.5 s fade
 * out.  Marked [S] for the substitution, [C] for the rates.              */
void  b3_boostfx_update(int slot, int boosting, int crashed, float dt);
float b3_boostfx_level(int slot);      /* carObj+0x11B0                    */
void  b3_boostfx_reset(int slot);      /* level := 0                       */

/* FUN_00179F30's output: `size` (out[4]) and `colour` (out[0..2]) for a
 * level.  Exposed for the validator; the draw calls it internally.
 * `red` is carObj+0x1901 != 0.  Returns 0 when nothing should be drawn
 * (level <= 0, or size <= 0.01 -- the gate at 0x003A7ED8).                */
int   b3_boostfx_resolve(float level, int red, float* out_size,
                         float out_rgb[3]);

/* ---- draw --------------------------------------------------------------
 * Same shape as the corona pass: an additive, depth-write-off billboard pass
 * over all cars.  `pos` is the car's render origin and `yaw_rad` the same
 * angle the harness hands glRotatef, exactly as b3_carfx_corona_draw takes
 * them.  `red` selects the pool: pass -1 for the RECOVERED per-car-model
 * value of carObj+0x1901 (what retail does), or 0/1 to force one.        [C] */
void b3_boostfx_pass_begin(void);
void b3_boostfx_draw(int slot, const float pos[3], float yaw_rad, int red);
/* Full-pose variant (row-major object->world rot3, the carfx convention):
 * keeps the flame on the tailpipes under pitch/roll; the yaw entry above is
 * a wrapper that reduces to this at zero pitch/roll. */
void b3_boostfx_draw_pose(int slot, const float pos[3], const float rot3[9],
                          int red);
void b3_boostfx_pass_end(void);

#endif
