# TODO — Burnout 3: Takedown RE harness

State as of **2026-08-14, master `a88c43f`**. Everything below is open; each
item names its blocker and the retail function to start from. Nothing here is
waiting on information that is missing from the executable — every physics row
has been traced far enough to name what unlocks it.

Ground rules that apply to all of it: physics/collision/triggers/control flow
are strict 1:1 with retail (`[C]`/`[S]`/`[?]` provenance with addresses, GLUE
marks on harness inventions); rendering LOOK is explicitly relaxed. Keep all
15 suites green, and build BOTH targets (`make -j4` and `android/`).

---

## 1. Physics fidelity — the main goal

Ledger: `docs/PHYSICS_GLUE_LEDGER.md` — **18 recovered / 3 proven-unrecoverable
/ 6 blocked**, plus 1 decided-but-not-landed.

The six blocked rows collapse onto **three root blockers**. Work the blockers,
not the rows — each one closes two or three rows at once.

### Blocker A — the `.bgd` nav-node walk *(closes PH-10, PH-12, PH-17)*

Port `FUN_00175B10` (nav-node walk) and `FUN_00170820` (the route-following
driver reached from the autopilot flag `racecar+0x27D8`). `FUN_00179760` is
the navigator reset helper invoked by `FUN_001714F0`, not the walker. Owner:
the AI-DRIVE lane of work.

* The retail graph layout is now recovered: every row is a row-relative
  `{pair, edge, link, node_count|flags}` directory, with two point IDs per
  node and forward/reverse links at `+4/+6` and `+5/+8`. The extractor exposes
  it via `BGD.nav_graph()`, `BGD.nav_nearest()`, and the exact XZ ribbon
  classifier/walker `BGD.nav_step_flags()` / `BGD.nav_walk()`, plus
  `BGD.nav_forward()`. The runtime now loads this graph from `route.bin` v3
  and maintains each AI vehicle's retail section/node cursor. The separate
  12-byte target-planning records at network `+0x1C` are exposed as
  `BGD.nav_plans()`. The runtime selects each current section's closest
  upcoming planner window and uses its A/B/C pair aim. `FUN_00174050`'s
  ribbon walker consumes bits 4, 8, 1, then 2 in retail priority order.
  Bits 4/8 take signed local steps and, only at a non-loop terminal, try the
  forward then reverse link; bits 1/2 directly take the forward/reverse link.
  `FUN_00173C60`'s one-node type-5 gate and `FUN_00178310`'s separate
  eight-node selector span belong to `FUN_00176290`; the live `+0x27D8`
  cursor follows `FUN_00174960` →
  `FUN_00175570` without that gate. The remaining navigator work is the
  target follower's mutable route-selection state and recovery-state timing.
  `racecar+0x1920` is the selector's mode gate (not a route-object pointer);
  the reset-state selector now carries a
  separate target cursor through type-5 entry (including its recovered
  two-successor vector tie-break) and type-4 lookahead branches.
  `FUN_00178310`'s complete span mask is differential-tested under Unicorn
  (including the open-row clamp and its unusual wrapped mode-zero tail); its
  higher-level state writers are still unmapped.

* **PH-10** crash-recovery placement now uses retail nav-node placement and
  heading; the surrounding retail reset state remains.
* **PH-12** AI-wheel handovers (`full.c:130, 1746-1754, 2078, 2205`).
* **PH-17** off-world / stuck watchdogs (`full.c`, `FUN_001712E0`) —
  `beach_time`, `stuck_ref`, wall-grind detector, off-world drop recovery.
  Retail's 5 mph stuck rule is already known; the rest exists only because the
  harness road model is two drive lines (`full.c:5645`).

### Blocker B — the traffic body update *(closes PH-07, PH-13, gap 2)*

Port **`FUN_00120F30`** (traffic vtable `0x003B11EC` slot +0, ~0x460 bytes)
into `full.c`'s traffic section: streaming-unit gates on `+0x216`/`+0x242C`,
then the towed-body link `+0x2424` (both bodies share the `+0x20E` sleep
byte, @0x00120FE6..0x00121032). Direct disassembly refutes the former
``route driver behind +0x13A0`` wording: that field is the model pointer used
only for the `+0x16A4/+0x16A8` tow anchors in this function.

* **PH-07** is partly landed: traffic now carries a persistent `B3RigidBody`
  through the generic car-contact pass and shared `FUN_00109560` integrator;
  the synthetic knock write-back and `1-4·dt` decay are gone. Its `+0x216`
  residency identity now comes from the streamed collision unit owning the
  ground query, and the `+0x242C` entry/exit gate clears or skips the base
  update accordingly. Partner sleep coupling, the model's full
  `+0x16A8/+0x16A4` hitch vectors, and the shared `FUN_0010F8D0` two-body
  normal deflection solve and kingpin spring now run across persistent
  tractor/trailer bodies. `FUN_001A20F0` runs its road-agent speed/cursor/
  occupancy passes before `FUN_001A6B40` updates physical traffic bodies and
  `FUN_001A8640` updates trailers; the harness now preserves that separation,
  so a coupled body sleeping outside a resident unit no longer freezes its
  route cursor. `FUN_0019FFA0` is now decoded as a four-row, clamped cubic
  ribbon sampler (each row has two u16 point IDs into 16-byte points). Its
  relocated event descriptor is also known as `{pairs, distances, branch_rows,
  point_base, count}`: `FUN_00158CC0` relocates the first three pointers in
  each 0x14-byte descriptor and supplies the shared point base. The source is
  now extracted from the event RIDX image (`param+0x3CC/+0x3D0`) through
  `BGD.traffic_paths()` (21 paths for US_C3/OFFSGRCF). The runtime now loads
  `traffic_paths.bin`, advances a distance-table cursor, samples each pair at
  its centre through the retail four-row uniform cubic B-spline, and
  retires/reseeds at segment ends. Each agent retains the retail initializer's
  0.45..0.5499 randomized lateral fraction. `FUN_001A03F0` / `FUN_001A0600`'s
  same-descriptor owner lifecycle is now refreshed after every cursor commit:
  every descriptor row names its nearest agent ahead, and each agent follows
  that linked owner rather than a world-space lane scan. The retail RNG
  sequence, cross-descriptor occupancy
  pool replacement policy, and avoidance magnitude remain. `FUN_001A20F0`
  initializes the road-agent branch-attempt counter (`+0x48`) to 1 when its
  selected racecar has `+0x1920 == 0`, otherwise 0; `FUN_001A8EE0` invokes
  the selector while that counter is nonzero and only `FUN_001A9040` commits
  its selected descriptor after the source cursor reaches the selected switch
  row. `FUN_001A0750` now identifies `branch_rows` as one 0x12-byte record
  per cursor row, with four `{u16 target_row at +0x00, u8 target_path at
  +0x0C}` columns. `traffic_paths.bin` v3 also preserves `FUN_001A28B0`'s
  TDESC `+0xA4/+0xA8` pool-window table: each 0x18 window has an inclusive
  progress range at `+0x04/+0x08` and `+0x14` six-byte `{first_row,last_row,
  path_id,direction}` requests at `+0x00`; the manager processes the current
  and two preceding windows circularly. The runtime validates the extracted
  bounds. `FUN_001A3470` occupancy-stamps each request, then
  `FUN_001A2B20` pops physical bodies and 0x50-byte road agents from separate
  free lists (`FUN_001A38F0`/`FUN_001A3A10`): physical release appends at the
  tail (FIFO reuse), while agents return at the head (LIFO reuse). The harness's persistent
  physical-pool lifecycle remains unported. The recovered roll/jackknife angular-momentum projections, a
  conservative hitch-separation fallback, and the recovered
  `dot(Zaxis_a, Zaxis_b) < -0.5` unlink now leave a separately simulated
  trailer body until recycling.
* **PH-13** traffic mover / braking horizon — cursor movement and speed law
  are recovered; interpolation, avoidance and streamer remain.

### Blocker C — one wall source / real contact geometry *(closes PH-09's object arm, the `crash_fired` switch, gap 3)*

The live racer, wreck, and knocked-prop response paths now gather real
collision triangles into their recovered contact solvers. Wreck containment
and no-pipeline fallbacks still use sphere queries only as
anti-tunnelling/bootstrapping nets, so the systems are not fully unified yet.

* **PH-09 OBJECT arm** (`full.c:1412-1426`) — the wall arm is recovered. The
  retail object arm is for collision-handle type-3 entities, including the
  designated big-hit traffic vehicle, not generic scenery triangles. Static
  props now report their recovered class through `b3_props`; the remaining
  traffic type-3 lifecycle and designation flag are unknown.
* **`crash_fired` consumer switch — landed for live racers.** `crash_fired`
  is the retail-faithful decision and now starts the existing wall-crash
  consequence path using its contact record. Retail's latch/cooldown is
  documented `[C]` — `FUN_0010DD20`: `veh+0x210 != 0` plus the per-slot timer
  at `mgr + slot*0x3C + 0x130` — and maps onto
  `crashed_until`/`immune_until`. Debug with `B3_CFIRE_TRACE=1`.
  **Per-slot latch table now recovered and wired** (2026-08-15): the
  `+0x130` latch (the `immune_until` side) is a per-class/per-presentation
  table, not a flat value — truck fresh 7.0 / truck-presented 15.0 /
  other fresh 3.0 / other-presented 15.0 (17.0 when `FUN_00017390()` is
  live, runtime-only `[?]`), on the dilated clock. `b3_crash_latch_duration()`
  (crash.c) transcribes the `LAB_0010e431` branch; all six racer
  `immune_until` sites call it via `crash_latch_for()`. The worklog's
  "16.0/7.0/5.0/4.0/6.0" table was a mislabel of the float words.

### Remaining call-graph gaps (`PHYSICS_GLUE_LEDGER.md` "gaps, ranked")

1. **Props and debris are not in the broadphase** — pair ordering and the A/B
   swap differ from retail in pileups.
2. **Wheel and chassis contact now share one frozen raw-collision snapshot**;
   the chassis view applies `FUN_0011BBE0`'s recovered wall predicate. The
   snapshot still omits retail's appended crash-floor records. This is now a
   narrow source-data gap rather than an unknown algorithm: `FUN_00125790`
   creates six transformed 0x40-byte records at `veh+0x11D0` and sets
   `+0x1351`; `FUN_0011BC60` appends them as `0x26,0x26,0x1A,0x1A,0x1A,0x1A`.
   Their sole caller, `FUN_0017D0F0`, receives a crash-director basis that the
   harness does not yet model, so the records must not be synthesized.
3. **Four manager stages unported `[?]`**: `FUN_00114E60` @0x00110EB9,
   `FUN_0010D1C0(0x0064ACE8, dt)` @0x00110ECB (takes dt, so it is a sim step),
   `FUN_00164FB0(dt)` x2 @0x001AA8E8/@0x001AA8F7, `FUN_00111850` @0x001AA907.
4. **`FUN_0011BE50`'s own head is not run** (0x0011BE5F..0x0011BF43).

### Open `[?]` questions — highest value first (`docs/RE_NOTES.md`)

* **The wreck recovery clock — RESOLVED (2026-08-15), no bug.** The
  "25 s of wall time" is the *intended* value, and the harness already
  reproduces it 1:1. `g_delta_time = b3_tdfx_update()` returns the dilated
  `period/divisor` (takedown.c:872), `g_race_time += g_delta_time`, and the
  player never leaves the RACING state (there is no `g_state = CRASHED`
  writer), so `crashed_until = g_race_time + 5.0f` is already 5.0 GAME
  seconds = 25 s wall at divisor 5 — a direct mirror of
  `FUN_00198E60`'s `racecar+0x10DC = dilated_clock + 5.0` @0x00198F65. The
  worklog's "5 s WALL cap bug" was a false alarm built on the assumption
  that `g_race_time` runs on wall time. No change made.
* **Which crash-entry site fires for which crash kind — RESOLVED (2026-08-15),
  per-kind table landed.** The old `b3_wreck_begin` mixed two sites (a 0.40
  corner torque that belongs to the car entry with a 0.65 launch that belongs
  to the rollover). It is now split by a per-kind table: `b3_wreck_begin_entry`
  is the single funnel (full.c:2174); `wreck_begin_for` classifies each crash
  consequence site into CAR / WALL / ROLLOVER; the table (crash.c:1124-1139) is
  CAR = 0x0A corner, 0.40 spin, 0.0 launch (torque-only), ROLLOVER = 0x08
  corner, 0.90 spin, 0.65 launch, WALL = null kick (0/0/0). The wall crash's
  real answer is "neither — only `FUN_0011AEF0`'s impulse", and that is what
  the null WALL row produces. The old `b3_wreck_begin(` wrapper has zero
  remaining callers. ROLLOVER has no caller today (an inverted car is not
  detected), so the 0.65/0.90 row is dormant but correct.
* **Port the alternate type-3 request lifecycle.** TDESC `+0xB4 & 0x04`
  selects the `FUN_001A5C70` queue. `FUN_001A3AE0` state 6 maps TDESC
  schedule row `+0x38/+0x3C` 0x0C descriptors into
  `manager+0x30+slot*4`; each descriptor points to 0x20 records whose `+0x1B`
  bit 0 reaches `FUN_001A2B20`. The 3,005 static records across the shipped
  event data all have that bit clear, so the remaining writer is runtime-only
  (or an unavailable mode), plus payload semantics and lifecycle integration.
  Normal scheduling
  (`FUN_001A5910`) and dynamic road agents (`FUN_001A6070`) explicitly pass
  zero for the designation flag.

---

## 2. Android

Runs and renders on the Pixel (Mali, Android 16). See `docs/ANDROID_PORT.md`.

**Correctness**

* **The postfx present composite + motion blur are OFF on Android**
  (`B3_POSTFX_PRESENT` / `B3_POSTFX_BLUR` default 0 in `b3_android.c`). The
  `glCopyTexImage2D` back-buffer grab produces a solid white frame under
  gl4es on Mali. Needs an FBO-based grab; until then the phone renders without
  retail's x2 composite.
* **Dark-red blob at the bottom-centre** of the frame at the phone's ultrawide
  aspect — suspect the player car's own geometry against the near plane.
  Reproduce with a device screencap; not seen on desktop.
* **Pause/resume is untested.** SDL destroys the GL context on background;
  every display list, texture and gl4es state the harness uploaded at init
  would need rebuilding. Expect a black screen after a task switch.
* **Tilt/button ergonomics** — awaiting play feedback. Knobs already exist:
  `B3_TILT_LOCK_G` (default 0.42 g ≈ 25° for full lock), `B3_TILT_SIGN=-1` to
  invert. Button geometry is in `b3_touch.c`'s `BTN[]` table.

**Packaging**

* **Release signing config** — `assembleRelease` still has no `signingConfigs`;
  the APK is hand-signed with the debug keystore in the build loop.
* **Asset diet** — `build/cars/` is 133 MiB of the 143 MiB payload. Restricting
  it to the roster slots plus the `.bgd` traffic set should drop the APK under
  60 MiB; the packer needs the tables out of `burnout3_vehicle_data.h` +
  `burnout3_traffic_data.h`.
* **Crash audio beds are not packed** (`B3_PACK_CRASH_AUDIO=1`, ~147 MiB), so
  the phone logs "no crash beds" and crashes are quiet.
* **On-device asset sideload** so another track can be `adb push`ed without a
  rebuild — the extractor already skips when the stamp matches, so a manual
  stamp write is all it takes.
* **`armeabi-v7a` + app bundle**; at that point the asset zip must move to Play
  Asset Delivery (a 150 MB APK is fine to sideload, not to publish).
* **Audio latency** — SDL OpenSLES, 1024-frame buffers at 44.1 kHz mono. Check
  for underruns; consider `SDL_HINT_AUDIODRIVER=aaudio`.

**Process**

* **Keep the two source lists in lockstep**: a module added to the Makefile's
  `SRCS` must also be added to `android/app/src/main/cpp/CMakeLists.txt`.

---

## 3. Validation & tooling

* **`tools/validate_gameplay.py` — green (91/91)**. Its collision-world
  section now compares the shared u16 source grid rather than float32/float64
  decimal expansions, uses valid resident-unit samples, and probes the
  current `US_C3_V1` low-road contact instead of a stale track-specific
  coordinate.
* The other 15 suites are green and must stay so: port 151, crash_traj 134,
  crashcinema 115, td_rules 532, takedown 973, props 337, ai 163, carcol 752,
  sfx 399, hud 760, carfx 223, postfx 171, music 100, particlefx 600,
  boostfx 77.
* **`.panels` sidecars must be regenerated** after any `tools/extract_bgv.py`
  change — they now carry the `panelbb` line that seeds the recovered panel
  OBB. Without it `panel_piece_spawn` silently falls back to the invented
  `B3_PANEL_HALF` cube (`box_ok[]` records which pieces got a real box).

---

## 4. Process notes for agent waves

* Agents work in their own worktree (`git worktree add --detach
  .claude/worktrees/agent-<name> master`), never commit, and never edit
  `src/burnout3_full.c` — that ships as an idempotent patch script whose
  anchors each occur **exactly once**.
* **Write deliverables incrementally.** A wave-4 attempt died on an API credit
  error and lost everything because it saved `changes.patch` for the end.
* **Verify agent claims against Ghidra before banking them.** Wave 4's
  inertia lead was correct but its stated blocker premise was wrong, and
  Ghidra's decompile of `FUN_00109BB0` silently drops two of three axes — the
  disassembly is the ground truth, not the decompile.
* Always run the game with `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy`,
  one instance at a time.
