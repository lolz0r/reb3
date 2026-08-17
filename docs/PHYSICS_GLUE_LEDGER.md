# PHYSICS GLUE LEDGER

Every remaining `GLUE` / `TUNED` mark in `src/` classified **physics** vs
**presentation**, with a recoverability verdict for each physics site.

Sweep: `grep -nE "GLUE|TUNED" src/*.c src/*.h` — **284 marked lines across 32
files** at the time of writing (master `0334fcf` + this wave).

* **Presentation** (rendering, audio, camera, HUD, particles, colour, art
  sourcing) — 219 marks. They do not move a body and are listed at the bottom
  by file; each belongs to the agent that owns the file.
* **Physics** — 65 marks, clustered into the ledger rows below.
  * **PH-01 … PH-04** recovered by the first wave.
  * **PH-24, PH-25, PH-26** recovered by the SECOND wave (this document's
    section A tail) — the shared body-vs-world contact resolve, the OBB narrow
    phase and the class-7 update — plus **PH-05** (flight half), **PH-18** and
    **PH-23**, and a full call-graph audit in section E.
  * **PH-08 / gap B4, PH-27, PH-06 / PH-21, PH-22, PH-15** and **PH-09's wall
    arm** recovered by the THIRD wave — the substep relocation (FUN_0011AEF0
    at its own call site), the signed FUN_00106720 return it exposed, the
    wreck's world pass, the deletion of the retired scalar reconstruction and
    the trailer attach record.  Section E's gaps 1, 2 and 5 are closed.
  * **6 recoverable, specced, blocked on another agent's files** — the retail
    function is identified with addresses in every case; none is waiting on
    information from the executable.
  * **3 proven not recoverable from the image** (PH-19, PH-20, **PH-11**);
    four of the original six fell under wave 2's re-audit — see section C.

Evidence discipline: every address below is in the corrected ELF mapping
(`build/burnout3.elf`); `[C]` = read out of the binary, `[S]` = shape recovered
but a value is inferred, `[?]` = not located.

---

## A. RECOVERED (wave 1: PH-01…PH-04; wave 2: PH-24…PH-26; wave 3: PH-08/B4, PH-27, PH-06/21, PH-22)

### PH-01 — the prop knock solver *(src/burnout3_props.c — DONE)*

**Was:** a ballistic tumble with nine invented constants (`B3P_RESTITUTION`
0.35, `B3P_GROUND_BOUNCE` 0.30, `B3P_GROUND_FRICTION` 0.70, `B3P_SPIN_GAIN`
2.20, `B3P_SPIN_MAX` 22, `B3P_AIR_DRAG` 0.25, `B3P_SPIN_DAMP` 0.90,
`B3P_LIFT` 0.45, `B3P_REST_SPEED` 0.35) plus a 30 m/s launch cap
(`B3P_KNOCK_MAX_MS`), and a mass-cancelling impulse `j = m*(1+e)*vn`.

**Now:** the retail chain, ported verbatim and differentially verified
(`tools/validate_props.py`, 337/337):

| piece | retail | what it gives |
|---|---|---|
| promotion | `FUN_00114730` @0x00114730 | 16-body pool, LRU by +0x224 |
| body setup | `FUN_0011A020` @0x0011A0A5..0x0011A317 | radius, mass, inertia, com height, **the random up-biased LAUNCH** |
| inertia | `FUN_00109BB0`→`FUN_00109190` | `1/((e_j²+e_k²)·m·0.5)` per axis |
| contact | `FUN_00113960` @0x00113B57..0x00114032 | role-2 car, −0.9 normal bend, `FUN_0010F8D0`, `FUN_00106500` |
| update | `FUN_0011A330` (vtable 0x003B1120 +0) | `F += dir·−speed²`, `T += ω·(−2|ω|)`, `FUN_00109560` |

**Cap verdict — there is no launch cap in retail and none is needed.**  The
invented cap existed because the GLUE impulse cancelled mass
(`Δv = (1+e)·vn`, 90 m/s at 150 mph).  The real denominator is
`1/m_car + 1/m_prop + n·[(I⁻¹_c(r_c×n))×r_c + (I⁻¹_p(r_p×n))×r_p]`, so a
100 kg cone hit at 150 mph takes ~43 m/s and one hit at 60 mph ~17 m/s.  The
`150 mph cone dv < 60 m/s` assertion is a case in the validator's `contact`
section.  The cap is deleted.

**Life verdict — the 10 s "body life" was a misread.**  `body+0x224 =
clock + 10.0` (@0x0011A19E) is an **LRU key**, not a timer: the only reader in
the whole image is `FUN_00114730`'s recycle scan @0x001147A0, which picks the
*smallest* value among bodies with `+0x220 != 0 && +0x210 == 0`.  Nothing
compares it against the clock (verified: a modrm-aware scan of every `+0x224`
access in the image — the only writers are `FUN_00119F40`, `FUN_0011A020` and
the release vfunc @0x0011A470).  A knocked prop therefore keeps its body until
a newer knock needs the slot; the timed "settle" is gone.

### PH-02 — `b3_mat_orthonormalize` implements 1 of `FUN_000FF270`'s 3 branches *(recovered here; spec for vehicle_sim)*

`src/burnout3_vehicle_sim.c`'s `b3_mat_orthonormalize()` (the re-orthonormalise
`FUN_00109560` calls @0x00109A22) hard-codes one branch:
`r1 = ^(r2×r0); r0 = ^(r1×r2)`.

The real `FUN_000FF270` @0x000FF27D..0x000FF544 **normalises all three rows,
then keeps the pair that is already most orthogonal and rebuilds the other
two**:

```
L0,L1,L2 = normalize(r0),(r1),(r2)          FUN_0002C0D0 returns the old length
a = |r2·r1|   b = |r0·r2|   c = |r1·r0|     FUN_00013C60 x3, abs by FUN_000FF090
b > a && c > a  -> A: r0 = ^(r1 x r2); r2 = ^(r0 x r1)   @0x000FF332
b > a && c <= a -> C: r2 = ^(r0 x r1); r1 = ^(r2 x r0)   @0x000FF37C
b <= a && c <= b-> C                                     @0x000FF3D3
b <= a && c > b -> B: r1 = ^(r2 x r0); r0 = ^(r1 x r2)   @0x000FF3D5
L0 <= 0 -> A @0x000FF4E1   L1 <= 0 -> B @0x000FF47D   L2 <= 0 -> C @0x000FF41B
```

Branch **B** is the only one ported, which is why `validate_port.py` is green
(a racecar's frame, updated by a small omega, always lands in B).  A knocked
prop tumbling at 5–15 rad/s takes **A and C every few frames**; with only B the
orientation error was ~3e-3 per step and diverged to a completely different
attitude after 20 steps (measured: `frame0 = [0.38,-0.81,-0.44]` vs
`[0.74,-0.55,-0.39]`).

**Recovered here** as `b3p_orthonormalize()` in `burnout3_props.c`, together
with a local `b3p_integrate()` (line-for-line `FUN_00109560`, only so the prop
path can use the complete orthonormaliser).  **Spec for the vehicle-sim owner:
replace `b3_mat_orthonormalize`'s body with the four-way form above.**  It also
affects every fast-rotating body that shares the integrator: wrecks
(`burnout3_crash.c`) and panel pieces (`burnout3_panels.c`).  Cars are
unaffected in normal racing but a spinning wreck is not.

### PH-03 — the car body handed to the prop solver was in the wrong space *(src/burnout3_props.c + full.c hunk — DONE)*

`Vehicle.pos/vel/rot` are **HARNESS/GL** space; `Vehicle.fsim.rb` is **GAME**
space (`harness_ground_probe` @full.c:1247 mirrors `z` on the way in, and
full.c:1552 negates `vel.z` when seeding `rb`).  Props, the collision world and
the renderer are all harness.  `b3_props_collide_rb()` now mirrors the incoming
game-space body through `S = diag(1,1,-1)` (`b3p_mirror_rb`), with the correct
transformation rules: true vectors `S·v`, **pseudo-vectors (ω, torque) `−S·w`**
(because `S(a×b) = −(Sa × Sb)`), inertia `S·I·S`.  Without it the car and the
props were ~4 km apart and no prop could ever be touched.

### PH-04 — the prop body pool started fully allocated *(src/burnout3_props.c — DONE)*

`g_body[]` is static, so every slot's `owner` was 0, i.e. "owned by prop 0".
`b3p_acquire()` never found a free slot and recycled slot 0 on every knock, so
exactly one prop could be live at a time.  `b3_props_load()`/`shutdown()` now
seed the pool the way `FUN_00119F40` does (@0x00119F83 `+0x220 = 0`,
@0x00119F90 `+0x224 = -1.0`).  Measured after the fix: the pool fills to 10+
live bodies over a lap instead of 1.

---

### PH-24 — the shared body-vs-world contact resolve *(src/burnout3_vehicle_sim.c — DONE)*

`FUN_00109EA0` @0x00109EA0..0x0010A43A, **ported verbatim** as
`b3_rigid_body_world_contact()` and differentially verified against the real
x86 (`tools/validate_port.py`, 20 cases, every branch).

This is THE contact law every rigid body in the game runs once per frame,
through vtable slot **+0x10** of its class, immediately after the local
polygon soup is gathered (`FUN_00109D20`) and **BEFORE** the class's own
update slot runs the integrator `FUN_00109560`:

| class | vtable | slot +0x10 | reaches FUN_00109EA0 at |
|---|---|---|---|
| racecar (crashed) | 0x003B1240 | `FUN_0011BE40` → `FUN_00122D00` | @0x00122F81 |
| 6 — knocked prop | 0x003B1120 | `FUN_0011A490` | @0x0011A706 |
| 7 — panel/debris | 0x003B1108 | `FUN_001072A0` | @0x001073CF |

The law, address by address:

```
[+0x200][0] == 0            -> +0x212 = 0, +0x198 = 0, +0x194 = 0, return  @0x00109EBD
narrow phase (+0x20C == 1 ? FUN_00107950 : FUN_0010AAD0) -> +0x212        @0x00109EE6
+0x212 == 0                 -> the same early-out                          @0x00109FD8
vpt = FUN_001066A0(+0x160)                                                 @0x00109FED
j   = FUN_00106720(n = +0x170, pt, vpt, e = +0x1F8)                        @0x0010A010
j > 0 -> FUN_00106500(n*|j| at pt); +0x194 = j; +0x213 = 1                 @0x0010A023
u = vpt; if (+0x215 not in {1,2,3}) u += [0x0040A8A0] = (0,-20,0,0)        @0x0010A06C
F = (u - n*(u.n)) * mass(+0x1F0) * [0x003B1AC8] = -0.6                     @0x0010A0E2
  +0x215 in {1,2,3}:  |F|^2 > [0x003B168C]=1.0        -> FUN_001064B0(F)   @0x0010A110
                      else vel(+0xB0..B8) *= 0.95, speed(+0xBC) *= 0.95    @0x0010A139
  otherwise:          |F|^2 > [0x005A538C]=1440000.0  -> FUN_001064B0(F)   @0x0010A1A5
                      else k = 0.7 [0x003B17D8] (cls 6)
                             | 0.6 [0x003B16EC] (cls 7, +0x2BA == 2)
                             | 0.9 [0x003A69C0] (cls 7, otherwise)
                             | 0.875 [0x0039922C] (anything else)          @0x0010A1C2
L(+0xE0) *= [0x003B1758] = 0.99                                            @0x0010A25E
+0x215 in {6,7} -> L *= 0.95 [0x003A69B8], vel *= 0.95 (speed untouched)    @0x0010A299
speed < 1.0 and any |n . frame_row{1,2,0}| > 0.99                          @0x0010A307
     -> L *= 0.5 [0x003B1684];
        speed < 0.3 [0x003B1750] and |omega|^2 < [0x005A3A94] = 0.09
        -> +0x20E = 1  (the FREEZE latch)                                  @0x0010A3E2
speed < 1.0 -> L *= 0.9 [0x003A69C0]                                       @0x0010A3E9
+0x180 = pushout;  +0x130 += pushout;  +0x198 = 1                          @0x0010A40D
```

**The two BSS thresholds are PROVEN, not assumed.**  `xref --imm` over the
whole image returns exactly two absolute references to each: the read site and
a C++ static-initialiser thunk.  `[0x005A538C]` is read only @0x0010A1AD and
written only @0x002B92AC out of `[0x003B2334] = 1440000.0`; `[0x005A3A94]` is
read only @0x0010A3D9 and written only @0x002B928C out of
`[0x003B1D38] = 0.09`.  Seeding them is mandatory — under Unicorn they are
zero and every gate inverts.

`FUN_00038BC0` and `FUN_00013C60` were checked instruction by instruction:
both `MULPS` then fold lanes 0..2 with two `ADDSS`, so the force gate and the
sleep gate compare **squared** magnitudes.

The freeze latch `+0x20E` is not decorative: the collision manager's per-body
loop reads it @0x00110DAD and **skips the whole `vtbl+0x10` call** for a
frozen body of type 0/1/2/4/6 (`test dl,dl; je` / `cmp al,7; jne` — class 7
runs anyway, and `FUN_001072A0` @0x001072AC then takes its attached-pose arm).
`FUN_00123000` @0x001236F0 likewise skips the wreck's suspension pass once it
is set.

### PH-25 — the OBB narrow phase *(src/burnout3_vehicle_sim.c — DONE)*

`FUN_00107950` @0x00107950..0x00107E82, the arm every body with `+0x20C == 1`
takes (props @0x0011A12B, every class-7 piece @0x00106A36/0x00106ADB/
0x00106C5B), ported as `b3_rigid_body_obb_plane_contact()` for a soup of ONE
surface polygon:

```
per polygon: 3 verts -> BODY space with the inverse frame +0x70            @0x001079DD
             clip against the 3 OBB slabs [+0x1E0 bbmin, +0x1D0 bbmax]
             with FUN_001B09C0, one axis per call; < 3 verts survive -> drop @0x00107A3E
             centroid = sum(surviving verts) / count                       @0x00107B79
             -> world with the frame; accumulate; accumulate the poly normal @0x00107BEB
once:  |sum(normals)|^2 <= [0x003B191C] = 2^-32 -> NO CONTACT              @0x00107CDD
       +0x170 = normalize(sum of normals)                                  @0x00107CF2
       +0x160 = sum(centroids) * (1 / polygon count)                       @0x00107D14
       n_body = rotate(inv_frame, normal); c_body = xform(inv_frame, point) @0x00107D41
       t = min over axes of (n_body[a] >= 0 ? c_body[a] - bbmin[a]
                                            : bbmax[a] - c_body[a]) / |n_body[a]|
           (retail cross-multiplies and divides only once)                 @0x00107D75
       t = max(t, [0x003B194C] = 0.005)                                    @0x00107E47
       pushout = normal * t                                               @0x00107E69
```

`+0x1D0` = bbmax and `+0x1E0` = bbmin are copied straight off the model bbox
record by the prop ctor @0x0011A0A8/@0x0011A0AF, so `t` is literally "how far
the box reaches below the surface along the normal".

### PH-26 — the class-7 (panel / debris) per-frame update *(src/burnout3_vehicle_sim.c — DONE; harness hunk pending)*

`FUN_00106D00` @0x00106D00..0x00106EE8, vtable 0x003B1108 slot +0, ported as
`b3_rigid_body_class7_update()` and differentially verified (12 cases, every
branch, `tools/validate_port.py`).  See PH-05 below for the delivery state.

```
+0x4D0 != 0            -> clear it and return                              @0x00106D0D
+0x216 == 0xFF         -> return                                           @0x00106D28
+0xF0 += dir(+0xC0) * (speed^2 * [0x003B16C0] = -1.0)                      @0x00106D35
+0x2BA != 0            -> L *= [0x003B1DA0] = 0.98                         @0x00106D89
else if (+0x212) {
    d = |dot(+0x170, frame row0 = the RIGHT axis)|   (lanes 0..2)          @0x00106DBF
    0.1 [0x003A69C4] > speed && 0.1 > d  ->  L = [0x004A3830] = (0,0,0,0)  @0x00106DEA
    0.1 > d  ->  L -= row2*(row2.L)   FUN_00106630                         @0x00106E19
                 L -= row1*(row1.L)   the inline block                     @0x00106E2D
}
FUN_00109560(dt)                                                           @0x00106E65
```

Leaving only the row0 component of L, under a gate that fires when the right
axis lies in the surface, is what makes a settled panel roll flat on the road
instead of pirouetting on a corner.

### PH-08 / gap B4 — **THE SUBSTEP RELOCATION** *(vehicle_sim.c + crash.c — DONE)*

**The gap.**  `FUN_0011AEF0`, the chassis-vs-world contact resolve, was ported
in full but never called from where retail calls it.  Retail's substep, read
straight out of `FUN_0011BE50`:

```
0011c0a0  push esi; push ebx; call 0x11d460     tyre force pass
0011c0a9  mov byte [ebx+0x212], 0
0011c0b0  mov byte [ebx+0x213], 0
0011c0b7  call 0x11aef0                         THE CHASSIS CONTACT RESOLVE
0011c0bc  test eax,eax / je 0x11c0d3
0011c0c0    mov dword [ebx+0x1524], 3           forced drift state
0011c0ca    mov byte  [ebx+0x212], 1
0011c0d3  cmp dword [ebx+0x1524], 3 -> 0        release the state-3 latch
0011c0e6  push ebx;            call 0x1239c0    suspension pre-pass
0011c0ec  push esi; push ebx;  call 0x123fd0    suspension force pass
0011c0f3  0.5 > +0xBC && 0.1 >= +0x1400, or [[+0x13F4]+0x179C] in {0,2}
          -> zero +0xB0..+0xBC, +0xF0, +0xF8    the near-stop check
0011c15d  push esi; mov ecx,ebx; call 0x109560  INTEGRATE
0011c165  dec edi; jne 0x11c0a0
```

and the soup it reads (`veh+0x200`) is frozen ONCE per frame by
`FUN_0011BC60` @0x0011BF43, **outside** the loop.  The point of the position
is that everything `FUN_0011AEF0` produces is an ACCUMULATOR write — `+0x110`
impulse, `+0x120` angular impulse, `+0x130` deflection, `+0xF0` force — and
`FUN_00109560` @0x0011C160 consumes and clears all four at the end of the
SAME substep.  A wall response is therefore a force inside that solve, seen by
that substep's suspension pass and integrator.  We ran the ported response
from `full.c`'s `mesh_collide`, AFTER the whole step, inside an invented
1..8 iteration loop keyed on `mlen/0.6` — and PH-08's velocity write existed
only to compensate for that ordering.

**The port.**  `b3_vehicle_step_full` now has both call sites:

| retail | port |
|---|---|
| `FUN_0011BC60` @0x0011BF43, once per frame | `v->soup_freeze(v->soup_user, v)` right after the input stage |
| `veh+0x200` = {count, polys stride 0x40, u16 flags} | `B3ChassisSoup v->soup` |
| `FUN_0011AEF0` @0x0011C0B7, every substep | `v->chassis_resolve(v)` between `b3_d460_force_pass` and `b3_prepass` |
| the `eax != 0` arm @0x0011C0C0/CA | `drift_state_1524 = 3; contact_212 = 1` |
| the `eax == 0` arm @0x0011C0D3 | releases a state-3 latch |

`chassis_resolve` points at `b3_vehicle_chassis_contact` (burnout3_crash.c),
which projects the sim body onto `B3CrashVehicle`, runs `b3_crash_response`
(`FUN_0011AEF0` verbatim) and writes back exactly the set the real function
mutates.  A NULL hook is retail's own `eax == 0` arm, so a build that installs
nothing is bit-identical to the old stub — which is why the 127 pre-existing
`validate_port.py` cases were unaffected.

**Differential — 14 trajectories, `run_relocation_cases()`.**  The oracle does
not mirror the substep in Python: it seeds the vehicle through
`tools/emulate_pipeline.py`'s real init chain, runs the real `FUN_0011ECF0`,
and then executes **the retail instruction stream 0x0011C0A0 → 0x0011C16C
with EDI = 2** — both substeps, every call at its own address, in its own
order, with a real polygon soup in `veh+0x200`.  The C side is
`build/dump_traj --state <s> ... --soup <f> --authority <a>`, the real port.
Cases: ground only, a wall never touched, right / left / shallow / oblique /
head-on / two-wall / braking / mid-corner-separating / corner-into-the-wall /
deep head-on, plus the crash-trigger pair (a slammed car at authority 0.05
whose hit FIRES, and the same hit on a chevron board — surface low byte 0x20,
`cmp byte [edi+0x190], 0x20; je` @0x0011B944 — which must not).  Every case
asserts `+0x212`, `+0x198`, `+0x1524`, the gear and the crash decision frame
by frame with no slack.  **14/14.**  Nothing is patched except `FUN_0010DCA0`,
the crash-ENTRY state machine the trigger tail calls @0x0011B9F3 (`ret 0xC`,
verified at 0x0010DD0F), which is game state and not body state; a hook counts
its entries so both sides can be compared on the decision.

**What it found.**  `FUN_00106720`'s return value — see PH-27 immediately
below — which no other case in the suite could have exposed.

**Harness delivery.**  Hunks F2a…F2e of
`scratchpad/physledger/apply_harness_hunks3.py`: the per-frame soup, the two
hooks, the per-frame driver-authority refresh, and the deletion of the
post-step response (PH-08's GLUE) and of the post-step `+0x212` write.
Applied, built, run headless for 90 s of autodrive: the car laps at 115-129
mph and the physics is unchanged in character (both builds take the same
wall crash at t ≈ 7.0 s; the relocated build enters it 7 m/s slower because
the in-substep resolve has already bled the speed the trigger measures).

**PHYS-LEDGER wave 4 — the `crash_fired` CONSUMER question (differential +
verdict).**  Wave 3 left `v->fsim.crash_fired` computed at retail's program
point with **nothing reading it**; the harness still triggers a wall crash
from the `td_rules` arm.  This wave settled which is faithful and why the
switch must not land yet.

*Both arms run the SAME recovered gate* — `crash_wall_core`, i.e.
`FUN_0011AEF0` @0x0011B909..0x0011B9A3 (`dv > authority*27.5` and
`headon > authority*0.707`, the 0x20 chevron veto, the +0x1353 bit-3 veto,
`racecar_class == 2` veto).  `b3_td_wall_contact` reaches it through
`b3_crash_wall_eval` (apply = 0); the substep reaches it through
`b3_crash_response` (apply = 1).  They differ only in their INPUTS:

| | in-substep `crash_fired` | the `td_rules` arm |
|---|---|---|
| contact set | the FROZEN POLYGON SOUP (`harness_soup_freeze` ← `build/collision.bin`, the real game collision world) — retail's `FUN_0011BC60` set, aggregated min+max → flatten → renormalise @0x0011B47x | ONE `(pt, n)` at a time from `mesh_collide`'s 2-D barrier-SEGMENT approximation, keeping the strongest by dv |
| program point | inside the substep @0x0011C0B7, between the tyre pass and the suspension pre-pass | once per frame, after the whole step |
| cadence | per substep (retail's) | per frame |

so **`crash_fired` is the retail-faithful one on all three counts**, and it
is already differentially proven at instruction level: the 14 relocation
cases above compare it, frame by frame, against a hook on retail's own
`FUN_0010DCA0` — including every near-miss (distant wall never touched,
shallow graze, head-on under the gate, mid-corner separating, the chevron
refusal).  **14/14.**

**The in-game differential** (offscreen, real track + `collision.bin`,
`B3_TESTDRIVE=1 B3_CFIRE_TRACE=1`, 60 s, 6 cars, 2031 traced frames —
`scratchpad/physledger4/cfire_run.log`):

| | frames | distinct events (>1 s apart) |
|---|---|---|
| in-substep `crash_fired` | 107 | 14 |
| `td_rules` wall | 10 | 6 |
| **both on the same frame** | **0** | **0** |

696 frames had an in-substep wall contact (`+0x198 == 1`); 1367 had a
`td_rules` wall record.  On **every** substep-fire frame the `td_rules`
record was empty (`dv = 0`), and on **every** `td_rules` fire frame
`+0x198 == 0`.  **The two arms never see the same contact** — the harness
carries two independent wall systems (24 199 barrier segments from the render
mesh vs 60 373 collision triangles), and `mesh_collide` positionally pushes
the car out of its segments before the soup probe runs.

**Verdict: `crash_fired` is faithful, but the consumer switch is NOT a
fidelity win until the harness has ONE wall-contact source.**  Switching now
would not correct the trigger, it would swap one non-retail contact set for
another: ×2.3 the crash rate and the loss of all six crashes the segment arm
currently produces (including all three of the player's).  That precondition
is the PH-06 / PH-08 lineage (`mesh_collide` vs `harness_soup_freeze`), not
this row.

**SUPERSEDED — CRASH-AUDIT wave (this row is now RECOVERED).**  The switch
DID land (`full.c` `wall_fire = cfire || (!v->fsim_ready && fire)`), and two
things changed under it:

1. **The precondition is met — the harness now has ONE wall source.**  Not by
   merging the two, but because the segment/sphere arm was ORPHANED:
   `tdr_wall_report()` is reachable only from `mesh_collide()`, whose only
   live-racer caller was `apply_track_constraints()` — **which has no caller
   anywhere in `src/`**.  Its one surviving call site (the wreck containment)
   is gated `v->crashed_until <= 0.0f`, false for a wreck.  Measured over
   60 s with the real `collision.bin`: `mesh_collide` 434 calls / **0 hits /
   0 push-outs**, `td_rules` wall records populated on **0** frames, and
   in-substep wall contact on **1355**.  The live wall contact is the frozen
   soup at `FUN_0011BC60`'s program point resolved at `FUN_0011AEF0`'s — i.e.
   retail's, exclusively.

2. **The switch shipped one gate short, and that was the AI wall-crash
   pile-up.**  `FUN_00105BD0` writes `veh+0x1534` AND `veh+0x1353` from the
   same `FUN_00105FC0` result: `@0x00105F95 OR byte [ESI+0x1353],0x18` when
   the ladder's `crash_ok` byte is 0, which `FUN_0011AEF0` reads
   `@0x0011B94D TEST byte [EDI+0x1353],8` to REFUSE the crash.  `crash_ok` is
   `frac > 0`, i.e. `d2 < 0.4*base` = 79.06 m from the viewed car; retail
   re-arms the byte every frame in `FUN_00110AF0` @0x00110E20/E46/E70/EA2,
   before any car update (this is call-graph row 5, and it was never
   decorative).  The `td_rules` arm consumed `crash_ok`
   (`burnout3_td_rules.c:778`); the substep arm never did — `full.c` used the
   value-only `b3_td_crash_authority()` and `fsim.flags_1353` was written once
   at init.  Because the same out-of-band condition ALSO pins the authority at
   the 0.03 FLOOR `[0x00384148]`, the harness replaced "this car cannot
   wall-crash" with "this car wall-crashes at `dv > 0.825` / `headon >
   0.0212`" — 33x easier than the player's 27.5 / 0.707.  Measured: 14 rival
   wrecks in 60 s, 13 of them wall, two cars left stopped dead against a
   barrier; after the fix, **1** (the player's own takedown), and mean rival
   track progress at t=55 rises 0.313 → 0.474.

Landed as hunk **C1** of `scratchpad/crashaudit/apply_crashaudit_hunks.py`
(`flags_1353 = crash_ok ? 0 : 0x18`, retail's clear-then-OR collapsed — the
harness has no other writer of the field), and proved by **two new
`validate_port` relocation cases** that execute the retail instruction stream
0x0011C0A0..0x0011C16C with the byte set: `slammed car, +0x1353 bit 3
refuses` (25/25 wall-contact frames, **fire 0**, against the identical
geometry's `fire 22`) and `wall with +0x1353 bit 0 (resolve disabled)`
(`FUN_0011AEF0` bails at its head, `test byte [edi+0x1353],5` @0x0011AF0C).
`validate_port` 151 → 153.

**What DID land (hunk P4-2 of `apply_harness_hunks4.py`):**
* **A real bug fix — the drain.**  `crash_fired` is set and was never
  cleared, so it LATCHED on the first wall crash of the race and read 1 for
  ever after.  Retail has no latch: `FUN_0011AEF0` rewrites the verdict every
  substep (the port mirrors it with `v->crashed = 0` at the head of
  `b3_crash_response`).  It is now drained once per frame at the same place
  the `td_rules` record is drained — which is also exactly where the consumer
  would read it.
* `B3_CFIRE_TRACE=1`, the per-frame differential trace.

**The latch/cooldown retail applies between decision and crash-mode entry**
[C], `FUN_0010DCA0` @0x0010DCA0 → `FUN_0010DD20` @0x0010DD20, head of the
callee:
```
if ((char)veh[0x210] != 0)                    return;   // already crashed
if (slot == -NAN)                             return;
...
if (0.0 < *(float*)(mgr + slot*0x3C + 0x130)) return;   // per-slot COOLDOWN
*(u8*)(mgr + 0xD) = 0;
```
The harness's two guards at the same program point —
`v->crashed_until > 0.0f` and `g_race_time < v->immune_until` — are the
same pair, so a consumer switch needs no new latch.

### PH-27 — `FUN_00106720` returns a SIGNED impulse *(burnout3_crash.c — BUG FIXED)*

`b3_crash_impulse` returned `fabsf(j)`.  The real function does not:

```
00106863  divss xmm0, xmm2        j = -(1+e)*dot(n,vp) / denom
0010686a  movss [esp+0xc], xmm0
00106870  fld [esp+0xc] / fabs / fstp [esp+0xc]
0010687e  movss xmm2, [esp+0xc]   |j|
00106886  mulps xmm1, xmm2        out = n * |j|
0010688f  ret 0x10
```

XMM0 — the caller's float return — keeps the SIGNED quotient; the `fabs`
travels through the x87 stack and a memory slot into XMM2 and only scales the
OUTPUT VECTOR.  The sign is load bearing:

```
0011b75c  divss xmm3, [edi+0x1f0]   dv = j / mass
0011b764  comiss xmm3, [0x3b16e0]   vs 0.0
0011b771  jbe 0x11b909              dv <= 0 -> NO impact, NO impulse
```

so `FUN_0011AEF0` records a contact and still pushes `+0x130` out, but does
**not** kick a chassis that is already SEPARATING from the wall.  With the
`fabs` in the return, a car sliding away from a barrier took a full impulse
back into it.  `b3_contact_impulse` in `burnout3_vehicle_sim.c` — the wave-1
port of the SAME retail function — already returned the signed value, so the
two transcriptions of `FUN_00106720` disagreed with each other.  Caught by the
`wall mid-corner (separating)` relocation trajectory: the emulator applied no
impulse and the port applied one worth 934 N·s of `veh+0x194`.  Fixed; the
one non-retail call site (`b3_wreck_begin_*`, which hands in a
caller-classified crash-entry contact rather than a point velocity) takes
`fabsf` explicitly and is marked as such.  All 134 `validate_crash_traj` and
532 `validate_td_rules` cases still pass.

### PH-06 / PH-21 — the wreck's world pass *(burnout3_crash.c — DONE)*

A crashed racecar is an ordinary rigid body in the collision manager and its
world contact is the ORDINARY shared resolve, not a dedicated wall responder:
class vtable slot **+0x10** → `FUN_0011BE40` @0x0011BE4A → **`FUN_00122D00`**
(gated `veh+0x210 != 0`, `(veh+0x1353 & 5) == 0`) → `FUN_00109D20` @0x00122D4A
(soup) → the wreck appends its own volumes → **`FUN_00109EA0` @0x00122F81** →
`FUN_00126D40` @0x00122FB0 → `FUN_001239C0` @0x00122FD2 — run BEFORE slot +0
(`FUN_0011BE50` → `FUN_00123000`) integrates.

`b3_wreck_world_contact()` is that resolve: `b3_rigid_body_obb_plane_contact`
(PH-25) then `b3_rigid_body_world_contact` (PH-24) at `cls = veh+0x215` and
restitution `+0x1F8 = [0x003A69C4] = 0.1`, in GAME space (the wreck's
mirror-in/mirror-out convention), accumulating into three new
`+0x110`/`+0x120`/`+0x130` fields on `B3WreckState` which the next
`b3_wreck_update` consumes and clears through the verified `FUN_00109560`
port.  Zero unless the harness runs the pass, so `validate_crash_traj` is
bit-identical (134/134).

Hunk **F3** puts it where the manager has it — before `b3_wreck_update` — and
demotes `full.c`'s sphere-sweep block to an anti-tunnelling POSITION net:
its velocity kill and its damage report are `FUN_00109EA0`'s now.  Measured:
90 s headless, wrecks tumble, take wreck-takedowns at t = 14.3 s and 29.3 s,
and settle asleep with the freeze latch raised.

### PH-22 — the retired scalar reconstruction *(vehicle_sim.c — CLOSED BY DELETION)*

Both of the row's GLUE marks lived inside `b3_vehicle_step()`, which has had
no caller in `src/` or `tools/` since `full.c` switched to
`b3_vehicle_step_full`.  Both are superseded by recovered code in that
pipeline: `:173`'s brake drag is `FUN_0011D460` @0x0011DBD7..0x0011DCF7,
applied as a force at `pos + up*veh+0x1368` with its pitch torque (the
`b3_brake_drag_scalar` + `b3f_acc_add` + `b3f_torque_at` block in
`b3_d460_force_pass`), and `:224`'s spring relaxation is superseded by the
ported `FUN_001239C0` + `FUN_00123FD0` ray pass.  The function is deleted and
the header's "the game's integrator has not been reversed" preamble with it.

---

## B. RECOVERABLE — SPECCED (not this agent's files)

### PH-05 — panel/debris piece flight *(src/burnout3_panels.c:214-221, 252, 271-274, 382-390, 422, 482 — 11 marks)*

`B3_PANEL_MASS_FRAC 0.02`, `B3_PANEL_HALF 0.55`, a 3 rad/s tumble,
`B3_PIECE_RESTITUTION 0.25`, `B3_PIECE_FRICTION 0.55`, `B3_PIECE_SPIN_DAMP
0.80`, `B3_PIECE_AIR_DAMP 0.999`, `B3_PIECE_REST_SPEED 0.6`,
`B3_PIECE_REST_TIME 0.5`, a flat ground plane.  **All invented.**

**Recoverable — yes, and it is the same shape as PH-01.**  Retail's detached
panel is a **class-7 rigid body**: constructor `FUN_001068A0` @0x001068DA sets
`+0x215 = 7`; the pool is `gameworld+0xD3380`, **0x40 bodies of stride 0x4E0**
(`FUN_00110390` @0x0011037E / `FUN_00110780` @0x00110772); the collision
manager `FUN_00110AF0` drives one update per frame per allocated body through
the class-7 vtable at **0x003B1108**, slot +0 = **`FUN_00106D00`**:

```
+0x4D0 != 0            -> clear it and return (one-shot suppress)  @0x00106D0D
+0x216 == 0xFF         -> return (outside a loaded streaming unit) @0x00106D28
+0xF0 += dir(+0xC0) * -(speed(+0xBC)^2)          [0x003B16C0]=-1.0 @0x00106D35
if (+0x2BA)  +0xE0 (angular momentum) *= 0.98    [0x003B1DA0]      @0x00106D89
else if (+0x212 grounded):
    d = |dot(+0x170 contact normal, matrix)|                       @0x00106DBF
    if (0.1 > speed && 0.1 > d) [0x003A69C4]=0.1 -> settle branch  @0x00106DEA
    else if (0.1 > d)  FUN_00106630 + FUN_00013C60 + FUN_000BFB10  @0x00106E17
FUN_00109560(body, dt)                                             @0x00106E65
if (+0x2BA == 1)  pinned-pose fix-up via FUN_00031330              @0x00106E73
```

Note `FUN_0010FBC0`'s jump table @0x0010FC04 sends type 7 to the same class-6
arm as props, so debris is non-crashable by the same table row.
**STATUS — RECOVERED (this wave).**  The three retail functions are ported
and differentially verified in `src/burnout3_vehicle_sim.c`:

| piece | retail | port | cases |
|---|---|---|---|
| per-frame update | `FUN_00106D00` | `b3_rigid_body_class7_update` (PH-26) | 12 |
| world contact | `FUN_00109EA0` | `b3_rigid_body_world_contact` (PH-24) | 20 |
| narrow phase | `FUN_00107950` | `b3_rigid_body_obb_plane_contact` (PH-25) | — |

`burnout3_panels.c` is not this agent's file, so the wiring ships as the
idempotent exact-match hunk **P1** in
`scratchpad/physledger/apply_harness_hunks.py` (anchors verified unique
against master; applied, built, run headless and reverted).  P1 deletes SIX
of PH-05's eleven invented numbers — `B3_PIECE_RESTITUTION` (replaced by the
ctor's `+0x1F8 = [0x003A69C4] = 0.1`), `_FRICTION`, `_SPIN_DAMP`,
`_AIR_DAMP`, `_REST_SPEED`, `_REST_TIME` — and puts the contact BEFORE the
update, which is the manager's order.

**SEEDING — CLOSED (PHYS-LEDGER wave 4).**  The premise above was wrong:
`FUN_001068A0` is a defaults-only pool ctor and seeds nothing.  The mass,
the OBB and the inertia are written at piece **ACTIVATION**, by
`FUN_001069C0` — the call `FUN_00111340` makes the instant it takes a free
slot — and the flight seeding by `FUN_00106F20`.  All addresses [C]:

```
FUN_00123000 @0x0012314E  acc > 999 (DAT_005A80C8)          -> FUN_00125A50
FUN_00125A50 @0x00125A6F  PUSH 1 / PUSH veh / PUSH k / PUSH 0x64B310
                                                            -> FUN_00111340
FUN_00111340              take a slot, then                 -> FUN_001069C0
FUN_001069C0 @0x001069EA  piece+0x2BA = the allocator's 4th arg = the MODE
                          @tail  piece+0x1F0 = 0x43820000 = 260.0f (MASS,
                                 unconditional, all three modes)
             mode 1 arm:  bgv = *(*(veh+0xCC0)+0x40)
                          piece+0x1D0 (bbMAX) = bgv[0xEA0 + k*0x20 + 0x00]
                          piece+0x1E0 (bbMIN) = bgv[0xEA0 + k*0x20 + 0x10]
                          piece+0x260 (CENTRE) = (min+max)*0.5, with one
                            component zeroed by bgv+0xADC+k
                            (0 -> y, 1 -> x, 2 -> y and z)
                          then FUN_00109BB0 from the RAW box
FUN_00109BB0 @0x00109BBF  a = max(bbmax.x,-bbmin.x), b, c likewise;
                          K = [0x003B1684] = 0.5, N = [0x003B168C] = 1.0
                          Iinv = diag( N/(K m (b²+c²)),
                                       N/(K m (a²+c²)),
                                       N/(K m (a²+b²)) )   -> FUN_00109190
                          (body+0x10 / +0x24 / +0x38; Ghidra's decompile
                           shows only the first axis, the disassembly all 3)
FUN_00106F20 @0x00107173  ratio = piece+0x1F0 / veh+0x1F0 = 260 / m_car
             @0x001071B7  seed  = veh+0xD70 + k*0xC0
             @0x001071D6  L = (L_car*ratio + seed·R_car) * 2.5 [0x003A2D50]
             @0x00107217  mode 1: bbmin -= centre; bbmax -= centre
```

**The `[?]` on the narrow-phase arm is CLOSED.**  The MODE byte at `+0x2BA`
is the pool allocator's 4th argument, and the three call sites pass three
different values: `FUN_00123000` @0x001231E0 `PUSH 0` = a detached **wheel**,
`FUN_00125A50` @0x00125A6F `PUSH 1` = a detached **body panel**,
`FUN_00125AC0` `PUSH 2` = glass/debris.  `FUN_001072A0`'s sphere arm is gated
on `+0x2BA == 0`, so it is reached **only by a wheel** (whose radius `+0x1CC`
is `veh+0x870+idx*0xC0` @0x00106A43); a panel always takes the **OBB** arm —
the existing PH-25 port is the right one.  A modrm-aware displacement sweep
of every executable LOAD segment finds exactly three writers of `+0x2BA`:
0x0010692C (`=3`, the pool ctor), 0x001069EA (`= mode`) and 0x00106EFB
(`=3`, the orphan helper that also clears the owner `+0x2B0`).  So `3` is
only the *unowned* default and `0` is only ever a wheel.
It also means `b3_rigid_body_class7_update` must run a detached panel with
`attach_2ba == 1` (the `L *= 0.98` arm @0x00106D89), not 0 — fixed.

**The per-part seed `veh+0xD70 + k*0xC0` is recovered too**: it is
**panel record + 0x90**, where the record array is `veh+0xCE0`, stride 0xC0.
Vehicle ctor `FUN_00122830` @0x001229C8 REP-STOSDs `veh+0xCE0..+0x1160` to
zero, then @0x001229ED builds the array (`FUN_00121D70(rec)`;
`rec+0x20 = bgv+0xAFC+k*4`; `rec+0x28 = ctx+0x180+k*0x40`;
`FUN_00121F80(rec, bgv+0xEA0+k*0x20)` — the *same* inertia law, and
`FUN_00121D70` sets `rec+0xB4 = 0x43820000 = 260.0` and zeroes
`rec+0x90..+0x9C`).  The **only** writer of `rec+0x90` in the image is
`FUN_00122270` @0x001222B1, the LOOSE (state 1) **hinge integrator**
(`rec+0x80 += rec+0x70*dt; rec+0x90 += rec+0x80`).  The harness does not
animate the loose pose, so the seed is the ctor's zero — exactly retail's
value for a panel that never flapped.  **[S] the invented 3 rad/s tumble is
deleted, not replaced.**

Port: `src/burnout3_panels.c` (mass 260, the .bgv box + recentring, the
`FUN_00109BB0` diagonal, the real mass ratio, `attach_2ba = 1`),
`src/burnout3_panels.h` (`b3_panels_set_box`, `b3_piece_inertia`,
`b3_piece_recentre`), `tools/extract_bgv.py` (the `.panels` sidecar's new
`panelbb <k> <axis> <max> <min>` line), `tools/dump_traj.c --pieceseed`, and
**10 new Unicorn differential cases** in `tools/validate_port.py`
(`run_pieceseed_cases`, real x86 `FUN_00109BB0` vs the port; port 141 -> 151).
The full.c side is hunk **P4-1** of
`scratchpad/physledger4/apply_harness_hunks4.py`.
The only remaining GLUE is the `0.55` cube *fallback* used when a `.panels`
sidecar predates this wave (`box_ok[k] == 0`); a re-extract removes it.

### PH-06 — wreck containment sweep *(src/burnout3_full.c:2388-2438 — 1 mark, ~50 lines)*

A `b3_sweep_sphere` (radius 1.0, skin 0.6) against the real collision mesh,
pushing the wreck out along the returned normal and killing the into-wall
velocity component.

**Recoverable — partially, and the premise is right but the mechanism is not.**
Retail's crashed path (`FUN_0011BE50` @0x0011BE83 → `FUN_00123000`) has **no
sphere sweep and no dedicated wall responder**; what stops a retail wreck is
the *same* per-substep chain a live car uses — `FUN_001239C0` (suspension
pre-pass rays) + `FUN_00123FD0` over the polygon soup `FUN_0011BC60` collected,
and the soup **contains the out-of-bounds sky-walls**.  So the correct port is
"give the wreck sim the real soup and run the recovered suspension pass",
not a sphere sweep.
**Blocked on:** `burnout3_crash.c`'s wreck sim (CRASH-CINEMA owns it) and the
soup collector (collision agent).  **Effort:** ~2 days, mostly plumbing.
**RE-AUDITED (this wave) — the chain is now fully addressed.**  A retail
wreck runs TWO world passes per frame:

1. vtable slot +0x10: `FUN_0011BE40` @0x0011BE4A (`veh+0x210 == 0` -> no-op)
   -> **`FUN_00122D00`** @0x00122D00..0x00122FF3, gated `veh+0x210 != 0` and
   `(veh+0x1353 & 5) == 0`:
   `veh+0x200 = 0x005A3AA0` -> `FUN_00109D20` @0x00122D4A (soup) -> the wreck
   APPENDS its own volumes (`veh+0x11D0` stride 0x40, surface 0x26; and with
   `veh+0x215 == 1`, `veh+0x3A50` records from `veh+0x1590` with surface 0x21)
   -> **`FUN_00109EA0` @0x00122F81** -> per-panel `FUN_00126D40` @0x00122FB0
   -> **`FUN_001239C0` @0x00122FD2** (the suspension pre-pass) -> on
   `veh+0x212`, tail-jmp `FUN_0010ED30`.
2. vtable slot +0 -> `FUN_0011BE50` crashed path @0x0011BE83 ->
   `FUN_00123000` @0x0011BEF7, whose loop is `FUN_00123FD0` (skipped once
   `+0x20E` is set, @0x001236F0) + `FUN_00109560` @0x00123704.

**The resolve half is now PORTED** (PH-24) and only needs the soup.  So the
sphere sweep is replaceable the moment the wreck can be handed real polygons;
what is left is plumbing, not law.

**Substep correction.**  An earlier reading of `FUN_00123000` as "2 substeps
at dt/2" is WRONG.  `[esp+0x1B]` is 0 unless `byte [0x005A3759] != 0` AND
`[0x004D5370]->[0x1B8]` is one of `0x004D4FFC` / `0x004D5070` (@0x00123009);
only then does it take `dt *= [0x003B1684] = 0.5, n = 2` @0x00123272.  The
DEFAULT wreck path is **n = 1 at full dt**, which is what `burnout3_crash.c`
already does.  (The live car's gate is different and unrelated:
`[[veh+0x13F4]+0x1920] == 0` @0x0011BFF5 -> n = 2, which is the normal race
path and matches our hard-coded 2.)

**STATUS — RECOVERED (this wave).** See section A, "PH-06 / PH-21 — the
wreck's world pass". `b3_wreck_world_contact()` is `FUN_00109EA0` over the
wreck's own box, accumulating into `+0x110`/`+0x120`/`+0x130`; the caller
gathers the same collision triangles as the live chassis and submits every
candidate plane before `b3_wreck_update`. The sphere sweep is now only an
anti-tunnelling POSITION net with no velocity, impulse, or damage report.

### PH-07 — traffic / synthesised-body knock write-back and its decay *(full.c traffic update + car-contact pass)*

Bodies that do not own a `B3RigidBody` (traffic, harness-driven wrecks) get one
synthesised per frame; the solver's `+0x130` deflection becomes a position
offset and `+0x110` an impulse converted to a **knock velocity that decays at
`1 - 4·dt` per frame** (invented).

**Recoverable — yes, by deleting it rather than tuning it.**  Retail traffic
cars are ordinary rigid bodies (object type 2 → class 0) that go through
`FUN_00109560` like everything else; there is no decay term anywhere in the
image.  The correct fix is to give `TrafficCar` a real `B3RigidBody` and let
the integrator consume `+0x110/+0x120/+0x130`, exactly as PH-01 now does for
props.

**Located (wave 3), and it is bigger than a constant.**  The traffic /
articulated-vehicle body's per-frame update is **`FUN_00120F30`**
@0x00120F30..~0x00121393, installed at vtable slot `0x003B11EC`
(@0x001A9946/@0x001A9950), and it **tail-calls the ordinary vehicle update
`FUN_00123000` @0x0012138E** — so a retail traffic car really is an ordinary
rigid body running the same integrator loop a wreck does.  Its own body is the
streaming/sleep protocol plus the TOW CONSTRAINT, and that is why the decay
cannot be deleted in isolation:

```
00120f40  [esi+0x210] == 0                      -> whole update skipped
00120f5b  [esi+0x216] == 0xFF (outside every loaded unit)
             -> [esi+0x1353] |= 3, [esi+0x242C] = 0, FUN_00104840(esi)
00120f98  entering a unit: [esi+0x242C] = 1, FUN_001213C0(esi)
00120fca  [esi+0x2424] (the TOWED partner) != 0:
             both bodies' [+0x20E] sleep bytes are driven TOGETHER
             (@0x00120FE6 / @0x00120FF6 / @0x00121024)
00121044  [esi+0x2428] == 1 (this body is the MASTER)
             -> the two-rigid-body tow constraint  (see PH-15)
0012138e  jmp FUN_00123000                       the base vehicle update
```

**Status — partly retired.** `TrafficCar` now retains a `B3RigidBody` through
the generic car-contact pass and the shared `FUN_00109560` integrator; the
invented knock write-back and `1 − 4·dt` decay are gone. Residency gates and
the coupled sleep byte run from collision-unit ownership. Towed rigs retain a
second body, use the full raw `+0x16A8/+0x16A4` vectors at spawn and at the
constraint, and use `FUN_0010F8D0` for the opposing normal deflections.
The route driver is still missing. The recovered jackknife and roll
angular-momentum projections, a conservative hitch-separation fallback, and
the recovered `dot(Zaxis_a, Zaxis_b) < -0.5` unlink keep a separately
simulated trailer body until the pool recycler reattaches it, and the retail
vertical kingpin spring is now live. `FUN_00104840`, incidentally, is
called HERE (@0x00120F7B /
@0x00120FB7) rather than per frame: it zeroes
`+0x160..+0x1BF`, `+0x212` **and sets `+0x1353 |= 4`** @0x00104848 — the bit
that DISABLES `FUN_0011AEF0` — so it is a park/reset path, not the per-frame
contact-scratch clear it was taken for.

### PH-08 — object-crash knock write-back *(full.c:964-1000 — 1 mark)*

"the linear half is applied to the velocity" — the object-crash trigger's
reaction on the car.  Same shape as PH-07: retail accumulates into `+0x110`
and lets `FUN_00109560` apply it.  **STATUS — RETIRED (this wave).**  The call site moved: `FUN_0011AEF0` now
runs @0x0011C0B7's position inside `b3_vehicle_step_full`'s substep, so
`+0x110` is consumed by that same substep's integrator and there is no
ordering difference left to compensate for.  Hunk **F2d** deletes the velocity
write and the centre-of-mass fallback for any body that has a pipeline of its
own.  Full walkthrough and the 14-trajectory differential: section A,
"PH-08 / gap B4".

### PH-09 — the crash trigger's collision world *(full.c:1412-1426 — 2 marks)*

"sphere/capsule collision world is GLUE; `contact_n` points from the obstacle".
The retail trigger is `FUN_0011AEF0` (wall) / `FUN_00113960` (car) / the object
arm `FUN_00112E70`; all three are ported.  What was GLUE was the *geometry*
that produces `contact_n`.  **SPLIT (this wave).**  Collision triangles now
feed the live-racer, wreck, and knocked-prop world resolvers; the remaining
open half is the retail type-3 entity lifecycle:

* **the WALL arm — RECOVERED.**  `b3_collision_gather_walls` supplies nearby
  collision triangles to the frozen live-car soup and to wrecks; each polygon
  reaches the recovered box clip and `FUN_0011AEF0` response. Knocked props
  use that same soup with `b3_rigid_body_world_contact`. Sphere sweeps remain
  only as anti-tunnelling containment or no-pipeline fallback.
* **the OBJECT arm — PARTLY RECOVERED.**  It is not a generic scenery-hull
  path: `FUN_00111CD0` routes collision-handle type-3 entities here. Static
  props report their recovered object class through `b3_props`, and
  `b3_td_object_contact` ports the narrow phase/trigger. The unported part is
  the retail type-3 traffic entity lifecycle. Ghidra MCP proves the sole
  big-hit flag is forwarded as `FUN_001A2B20` `[EBP+0x3C]` into
`FUN_001A7210`, with `FUN_001A5C70` reading byte `+0x1B` of a separate
0x20-byte runtime request record—not TDESC's transform record. `FUN_001A13F0`
sets the manager `+0x363B8` queue mode from `TDESC+0xB4 & 4`; only that mode
makes `FUN_001A5880` call `FUN_001A5C70`. `FUN_001A3AE0` state 6 reads TDESC
schedule row `+0x38/+0x3C` as 0x0C descriptors and writes each address to
`manager+0x30+slot*4` using descriptor bytes `+0x0A/+0x0B`. Descriptor `+0`
points to 0x20 records, byte `+8` is their count, and record `+0x1B` supplies
the designation bit. Normal-mode `FUN_001A5910`
and dynamic road-agent creator `FUN_001A6070` pass zero for that final flag,
so ordinary pool traffic remains outside the type-3 big-hit lifecycle. The
All 3,005 static records across 377 event TDESCs have bit 0 clear, so the
designation writer is runtime-only or absent from the shipped modes. Payload
semantics and lifecycle policy are still unextracted.

**What remains GLUE:** caller coverage and entity lifecycle, not the primary
world-plane source. The collision gather applies harness streaming filters and
no-pipeline callers still fall back to a sphere query. Retail's type-3 traffic
spawn/designation path is also unresolved.

### PH-10 — crash recovery placement *(full.c:2160-2210 — 2 marks)*

Route points stand in for the `.bgd` nav nodes; "~3 nodes back" and the 30 mph
relaunch are recovered (`FUN_001714F0` place + `FUN_001204C0` speed,
`0x41569446` = 13.4112 m/s = 30 mph [C]); the *node graph* is the GLUE.
**Recoverable — yes**, once the `.bgd` nav-node walk (`FUN_00179760`, RE_AI 10)
is ported.  **Owner:** AI-DRIVE.  **Effort:** part of the nav-graph wave.

### PH-11 — crash recovery bound — **PROVEN NOT RECOVERABLE, and the row's own premise was wrong**

**Was:** `crash_real_left = 5` WALL seconds plus a "don't release a wreck in
flight" rule, with the row asserting that retail stamps **5 GAME seconds**
(`FUN_00198E60` @0x00198F65 `[C]`) and that an earlier releaser exists at
`racecar+0x19BE`, `[?]`.  **Both halves of that are now refuted.**

**`racecar+0x19BE` is the asset-load-complete latch, not a releaser.**  A
disp32 sweep over ALL FOURTEEN executable `PT_LOAD` segments (not just
`.text`; the image has X segments at 0x2CC200, 0x2F3F40, 0x300D00, 0x31AA80,
0x3391E0, 0x34C2E0, 0x360A60, 0x362AE0, 0x36B7C0, 0x3B2360, 0x76B940,
0x772AC0, 0x774000 as well) finds **exactly three** instructions:

```
0018d740  mov byte [ebx+0x19be], 1      the ONLY write, immediate 1
00119c09  mov cl, byte [eax+0x19be]     eax = [ebx+0x1580] (racecar)
0011fec1  mov al, byte [ecx+0x19be]     ecx = [edi+0x13F4] (racecar)
```

The write sits at the tail of `FUN_0018D0E0`, the racecar spawn/asset-load
state machine keyed on `[ebx+0x1978]`, immediately after
`[ebx+0x19BD] = 1` @0x0018D739 and beside `[ebx+0x18FB] = 1`,
`[ebx+0x18FE] = 0`, `[ebx+0x18F0] = [ebx+0x18F4] = -1`; the state is then
0x17, which the jump table @0x0018D764 routes to the exit, so the body never
runs again.  **Nothing in the image ever clears it.**  A per-crash release
gate has to be re-armed every crash, so this cannot be one.  Both readers gate
the same thing: "the car's assets are resident, so run the place-on-track
virtual" (`FUN_0011FE90` @0x0011FECF/@0x0011FEDE, and `FUN_00119C00` skipping
its whole body @0x00119C09).

**The 5-second stamp exists and is DEAD.**  `FUN_00198E60` @0x00198F65..
0x00198F75 writes `racecar+0x240C = racecar+0x10DC + [0x003B1694] = clock +
5.0`, gated `cmp dword [edi+0x1920], 1` @0x00198F5C.  The same all-segment
sweep for displacement 0x240C returns **only that write** plus an unrelated
allocator pointer pair (`mov esi,[eax+0x240c]` @0x0003D434 /
`mov [eax+0x240c],edx` @0x0003D478, a different struct) and one site at
0x0034E6EE which reads it as an ARRAY INDEX (`lea edx,[eax+eax*2]; shl edx,8`
-> `*0x300`, then `rep movsd` out of `[edx+esi+0x1E04]`) — a table walk, not a
clock.  **`racecar+0x240C` has no reader on the racecar path.**  The value is
stamped and never consumed in this image.

**And there is no timed releaser at all.**  Leaving crash mode is
`racecar+0x18FA = 0`, reached through vtable slot **+0x10** — proved across
the three racecar vtables (`0x003B11A0` slot +0x10 = `FUN_0018C820`, clearing
`+0x18FA` @0x0018C884/@0x0018C8A7; `0x003B11C8` -> `FUN_0018E040`;
`0x003B1204` -> `FUN_001709B0`, which also sets `+0x18FB` and zeroes
`+0x19C0`/`+0x19D0` before `jmp 0x179760`).  A sweep of every indirect
`call/jmp [reg+0x10]` in `.text` finds 97, of which exactly **two** have a
racecar receiver: @0x0011FEDE (the load-latch-gated placement above) and
@0x0018D91E (inside `FUN_0018D790`, on the `[ebx+0x18FA] == 0` — NOT wrecked —
branch, after `[ebx+0x18C4] == 0`, i.e. the car lost its road segment).
Neither is timer-driven.  The nearest frame-counted re-placer,
`FUN_001711B0` @0x00171456 (60 consecutive ticks of progress delta in
(−5.0 `[0x003B16E4]`, −1.0 `[0x003B16C0]`] -> `FUN_001714F0(seg−8, 0)`), has
its counter `+0x2460` reset at 0x0017097C **on the branch taken when
`[esi+0x18FA] != 0`** (@0x00170935) — it is held at zero for the whole time
the car is wrecked, so it is a wrong-way/stuck reset, not a crash release.

**Verdict:** the release deadline is not in the executable.  What ends a crash
in retail is driven from outside the vehicle module (the crash-cinema /
sequence layer, which owns the vtable +0x10 call), and the only physics-side
evidence — a 5-second stamp with no reader — is a leftover.  The harness's
wall-clock cap and in-flight guard **stay GLUE, and now for a proven reason
rather than an unfinished search.**  The row's `[C]` claim on the 5 seconds is
downgraded: the CONSTANT is `[C]` (0x003B1694 = 5.0, written to
`racecar+0x240C`), the RULE is not.

### PH-12 — AI-wheel handovers *(full.c:130, 1746-1754, 2078, 2205 — 3 marks)*

The *flag* semantics are `[C]` (`racecar+0x27D8`, `FUN_0018C510` @0x0018C53A →
`FUN_00170820`, released by `FUN_0018CB60`); the *driver law* and the 1.2 s
duration are GLUE/[S].  **Recoverable — yes**, by porting `FUN_00170820` (the
route-following driver).  **Owner:** AI-DRIVE.  **Effort:** ~2 days.

### PH-13 — traffic mover, braking horizon, follow term *(full.c traffic update — 6 marks)*

**The road-agent mover is located.** `FUN_001A20F0` enumerates active
0x50-byte agents at `0x0063DCB0`, calls `FUN_0019F560` for their speed law,
then `FUN_0019F1C0`. The latter advances persistent `S+0x30` by
`S+0x2C * frame_dt / S+0x20`, carries across path sections, and calls
`FUN_0019FFA0`, which clamps rows `floor(S+0x30)-1 .. +2` to its path-table
end, fetches each row's two u16 point IDs from 16-byte point records, and
evaluates the target transform/tangent through its cubic helper chain.
`FUN_0019F3B0` then updates occupancy. This is direct raw-code evidence, not
the earlier `FUN_00105150` attribution.

The runtime entry at `DAT_0060EC2C + path_id*0x4C` is now structurally known:
`+0x00` pair-ID rows, `+0x04` cumulative-distance rows, `+0x0C` shared
16-byte-point base, and `+0x10` row count. `FUN_00158CC0` relocates the first
three pointers in each source `0x14`-byte descriptor and writes the shared
point base to `+0x0C`; `FUN_0018B250` then seeds the runtime table from that
relocated event block. The source is now located: event `param+0x3CC/+0x3D0`
is the RIDX image, whose `{descriptor_rows, point_base, path_count}` header
drives `BGD.traffic_paths()`; US_C3/OFFSGRCF has 21 validated paths.
`FUN_0019F1C0` disables an agent when its cursor exceeds that descriptor's row
count; `FUN_001A09F0` only selects its next speed profile at a boundary, not a
successor path. These are pool segments, not an implicit traffic-loop graph.
`FUN_001A0750` identifies the formerly unknown third source pointer as one
0x12-byte branch row per descriptor row: four target cursors at `+0x00..+0x07`
and four target descriptor bytes at `+0x0C..+0x0F`; its selected descriptor and
cursor become `FUN_001A9040`'s reassignment inputs. `traffic_paths.bin` v3 now
preserves these rows verbatim and verifies target descriptor bounds. It also
preserves `FUN_001A28B0`'s TDESC `+0xA4/+0xA8` progress-window table: each
0x18 row covers inclusive `+0x04/+0x08` progress and points at `+0x14`
six-byte `{first_row,last_row,path_id,direction}` requests. The manager visits
the current window and two circular predecessors.
`FUN_001A20F0` initializes the branch-attempt count at agent `+0x48` to 1
when the selected racecar has `+0x1920 == 0`, otherwise 0; `FUN_001A8EE0`
uses that count to invoke the selector and waits for its source switch row
before `FUN_001A9040` commits the new descriptor. The manager's progress-window
trigger is recovered; applying its requests to the harness's physical pool
entries remains GLUE.

`FUN_001A3470` writes each selected six-byte request's row span into the
retail occupancy bit map. `FUN_001A2B20` then pops one physical entry from
`manager+0x36364` via `FUN_001A38F0` and one 0x50-byte road agent from
`manager+0x3636C` via `FUN_001A3A10`. `FUN_001A41A0` appends released
physical entries to the tail (FIFO reuse), whereas `FUN_001A3A80` pushes
released road agents at the head (LIFO reuse). The harness now has a tested
representation of this split lifecycle; integrating it with persistent traffic
bodies remains the port boundary.

**Status — partial.** The harness now loads `traffic_paths.bin` from the
RIDX descriptors, uses its cumulative-distance rows for a persistent cursor,
samples its pair line with `FUN_0019FFA0`'s verified clamped four-row uniform
cubic B-spline, and retires/reseeds at a descriptor end. `FUN_001A2B20`'s
0.45..0.5499 per-agent lateral initialization is retained, but the retail RNG
sequence, `FUN_0019FEC0` avoidance magnitude, and neighbourhood-pool
replacement policy remain GLUE.
The manager ordering is now also preserved:
`FUN_001A20F0` performs speed, cursor, then occupancy for every selected road
agent before it calls `FUN_001A6B40` for physical bodies and `FUN_001A8640`
for trailers. The harness rebuilds the same-descriptor owner map immediately
after each cursor commit, so later agents see the current-frame advance. It
therefore advances the route cursor through an
off-unit coupled-body sleep and only re-enters/resynchronises the rigid bodies
when their target collision units are resident. **Recoverable — yes.**

### PH-14 — traffic crash routing *(full.c:8269 — 2 marks)*

**Recovered.** The live traffic path now consumes `b3_carcol_resolve()`'s
contact flags instead of a mass-based "smaller car" rule.  An alive
racer/traffic pair can only wreck mutually through the resolver's recovered
high-closing-speed gate.  Once traffic is already wrecked, the living racer is
the resolver's `a` body and receives `FUN_00113960`'s traffic-specific
`j > 2500` gate (`[0x003EBE54]`); ordinary contacts remain physical only.
The traffic five-second recycle duration is still harness policy, but it no
longer decides whether a collision crashes.  `DAT_0039AE50` continues to gate
the classes in the shared resolver.  Folded into PH-09.

### PH-15 — trailer kingpin offset — **RECOVERED (record + mechanism)**

**Was:** `king_z` = "trailer nose − 1 m", marked `[?]`, with the articulation
law itself ("one-axle bogie chasing the fifth wheel") also GLUE.

**The invented constant was wrong in KIND, not just in value.**  Retail does
not use a derived geometric kingpin at all: the anchor is an arbitrary
body-space `vec4` carried by the model, and the tow is a genuine
**two-rigid-body point constraint**.

**The record.**  The model record behind `vehicle+0x13A0` carries FOUR
attach-point groups, each a `ptr -> vec4[]` with a `u8` count:

| ptr | count | read by |
|---|---|---|
| `model+0x169C` | `model+0x16BA` | `FUN_001A7210` @0x001A7321 |
| `model+0x16A0` | `model+0x16BB` | `FUN_001A7210` @0x001A7345 |
| `model+0x16A4` | `model+0x16BC` | `FUN_00120F30` @0x00121075, `FUN_001A7210` @0x001A7369 |
| `model+0x16A8` | `model+0x16BD` | `FUN_00120F30` @0x00121082 / @0x0012109E, `FUN_001A7210` @0x001A7391 |

Neither the pointers nor the counts have a WRITER anywhere in `.text` (scanned
for memory-operand displacements and for any instruction encoding those
dwords; the only other hits are a displacement collision at
0x000AB190..0x000AB220 on an unrelated 0x190-stride class).  They are
read-only payload of the loaded model with the pointers fixed up at load time
— which is what makes the offset recoverable from the DATA, not from the
executable.

**How to read it** (`FUN_00120F30` @0x00121051..0x001210A4, gated
`[veh+0x2428] == 1`, i.e. only the MASTER runs the constraint):

For the shipped traffic pairs the selected points are the trailer's
`model+0x16A4` kingpin (count `+0x16BC`) and the tractor's
`model+0x16A8` fifth wheel (count `+0x16BD`). The code has count-selected
branches around these groups, but `HEVYCAR23 -> HEVYCAR24/27` selects exactly
those records; their full raw vec3 values are generated by
`tools/extract_traffic.py` rather than reconstructed from wheel locations.

**The constraint** (`FUN_00120F30`, the articulated-vehicle class's per-frame
update, installed at vtable slot `0x003B11EC` @0x001A9946/@0x001A9950 and
tail-calling the base update `FUN_00123000` @0x0012138E):

* both local anchors -> world through `FUN_00013CA0` and each body's `+0x204`
  (@0x001210C2 / @0x001210DF);
* point velocities `FUN_001066A0` (@0x001210FA self, @0x00121115 other);
* `d = worldAnchor(master) − worldAnchor(slave)` @0x00121190, degenerate test
  `FUN_0003B060` @0x001211A3, normalise `FUN_00011640` @0x001211BE;
* impulse through **`FUN_0010F8D0`** @0x001211F4 — the same shared normal-
  impulse denominator car-vs-car contacts use — added to BOTH bodies'
  `+0x130` deflection (@0x0012124E, @0x00121274);
* kingpin branch only: a vertical spring-damper at the master's anchor,
  `dy = anchorY(master) − (anchorY(slave) + 0.5 [0x003B1684])`,
  `F.y = (v_y/dt)·(−1000 [0x003B1744]) + (dy − 0.3 [0x003B1750])·(−80000
  [0x003B1E80])`, applied through `FUN_001064B0` (@0x0012112C..0x0012117D);
* jackknife/rollover/breakaway: `dot(Zaxis_a, Zaxis_b) < −0.1736
  [0x0039B3E8]` (>100°) -> `FUN_00120880`; and on the kingpin branch
  `< −0.5 [0x003B16A4]` (>120°) -> detach `FUN_00121400` @0x00121311;
  `dot(Yaxis_a, Yaxis_b) < 0.9848 [0x0039B3EC]` (>10° relative roll) ->
  `FUN_00120AA0` + `FUN_00120990`; impulse magnitude vs `5.0 [0x003B1694]`
  @0x0012127B/@0x00121292 and again @0x0012134E/@0x00121378 -> detach.
* detach `FUN_00121400` transfers momentum (`+0xBC` mass, `+0xC0` vec4) then
  zeroes both `+0x2424` (@0x00121485, @0x0012148F).

**The link itself** is `FUN_00114910` (the self-recursive spawner): it spawns
the partner named by the spawn descriptor's `[+0x110]` / `[+0x10C]` and stores
ONLY the partner pointer `+0x2424` and the master bool `+0x2428`
(@0x00114ACF/@0x00114ADC when the partner is master, @0x00114B4F/@0x00114B5C
when this body is; init clear @0x00120E5E).  No offset accompanies it — the
offset is in the model, which is why lead (a) alone could never find it.

**Status — partly landed.** The traffic extractor preserves the full selected
raw vec3 anchors, spawn makes the two selected points coincident, and the
persistent bodies use `FUN_0010F8D0`'s opposing normal deflection writes and
the selected `+0x16BC == 1` branch's vertical force-at-point. The
recovered jackknife/roll angular-momentum projections, a conservative
hitch-separation fallback, and `dot(Zaxis_a, Zaxis_b) < -0.5` unlink now
leave the trailer as an independently collidable body.

**Bonus finding, previously unrecorded:** `FUN_00120BA0` @0x00120C08 also
copies a SECOND 0x40-stride matrix table from `model+0xD00` (count
`model+0x0C`) into `vehicle+0x1560`, beside the known wheel table at
`model+0xB80` (count `model+0x0D`) -> `vehicle+0x13E0` @0x00120C82.  The
`vehicle+0x1590` poly set `FUN_0011AEF0` reads as its class-0 second soup
lives immediately after it.

### PH-16 — aftertouch cadence credit *(full.c:2318-2362, takedown.c:857 — 3 marks)*

The *producer* (crashed-input corner kicks) is only exercised behind
`B3_WRECK_AFTERTOUCH=1` because driving it pumped `|omega|`; the *consumer*
`b3_wreck_aftertouch` is a faithful `FUN_00118410` port.  takedown.c:857 is a
"same 1/divisor — identical at 60 Hz, stable off it" cadence GLUE.
~~**Recoverable — yes**: the omega pumping is a symptom of PH-02 (the wrong
orthonormaliser degrades a fast-spinning frame, which feeds back through
`I⁻¹_world = RᵀI₀R`).  **Retest the producer after PH-02 lands in
vehicle_sim.**~~  **Owner:** CRASH-CINEMA.

**CLOSED (PHYS-LEDGER wave 4).  Both halves of the row are answered, and the
PH-02 hypothesis above is REFUTED by measurement.**

1. **There is no producer to enable — retail never runs one.**  The corner
   kick is `FUN_00117F90`'s block (the RACING input stage), gated at
   `0x0011817E` on `byte veh+0x4AC2`.  Independent byte scan of **all 18
   LOAD segments** of `build/burnout3.elf` for the 4-byte disp32
   `0x00004AC2`: **exactly two occurrences in the whole image** — the gate
   read at 0x0011817E and `MOV byte [ESI+0x4AC2], AL` at 0x00117799 inside
   the vehicle reset `FUN_00117730`, where `AL == 0` (`XOR EAX,EAX`
   @0x0011774C, and nothing writes EAX between there and the store; the
   neighbouring `+0x4AC0`/`+0x4AC3`/`+0x4AC4` writes all use CL or the same
   zeroed AL).  Nothing in the shipped image ever sets `veh+0x4AC2`
   non-zero, so **the block is dead code** and the harness must not drive
   it.  The `B3_WRECK_AFTERTOUCH` env gate is already retired; the live
   aftertouch is `FUN_00118410`'s steer block, ported as
   `b3_wreck_aftertouch_steer` and **ON by default**.  [C]
2. **PH-02 was NOT the cause of the |omega| pumping.**  Measured on the
   post-PH-02 tree (`63e8852` landed) with `build/crash_traj_drv wreck`,
   1200 frames at 1/60, the standard crash-entry state:
   | producer | max &#124;omega&#124; | final | kicks |
   |---|---|---|---|
   | off | **15.9 rad/s** | 0.65 | 0 |
   | held (+x every frame) | **92.6 rad/s** | 0.72 | 1200 |
   Still past 65 rad/s, so the orthonormaliser was never the mechanism —
   the block has no cooldown, no airborne gate and nothing to absorb one
   0.6-magnitude corner kick per frame, which is exactly why retail keeps
   it behind a byte it never sets.
3. **The LIVE aftertouch is bounded.**  Offscreen, `B3_AUTODRIVE=1
   B3_TEST_CRASH_AT=12 B3_EXIT_AT=22`, crash trace over 480/454 frames:
   producer-free baseline `max|omega| = 8.407` (entry kick), and with
   `B3_TEST_AFTERTOUCH="1,0"` held for the whole crash (453 applications)
   `max|omega| = 8.406` — the steer block rotates the velocity vector and
   does not touch angular momentum at all.  Traces:
   `scratchpad/physledger4/ph16_base_trace.log` / `ph16_at_trace.log`.
4. **The `takedown.c:857` cadence GLUE is stale, not GLUE.**  Retail's
   simulation step is `timer+0x1C = period / divisor`, written by
   `FUN_001B5B60`'s rescale tail (ported verbatim as
   `b3_tdfx_timer_rescale`) and read as `DAT_0060EA1C`.  The mark was
   written while the harness fed `b3_tdfx_step` a wall-clock dt; the frame
   lock landed afterwards and `burnout3_full.c`'s loop now passes the
   NOMINAL period unconditionally (`g_tdfx_real_dt = 0.016666668f` =
   `[0x003B1838]`, or `B3_FIXED_DT`).  So `real_dt / divisor` **is**
   `period / divisor`, the same expression bit for bit.  Comment corrected
   to [C]; no behaviour change.

### PH-17 — off-world / stuck watchdogs *(full.c:146-152, 1794-1846, 2602 — 6 marks)*

`beach_time`, `stuck_ref`, `unstuck_side/until`, `immune_until`, the wall-grind
detector and the off-world drop recovery.  Retail's equivalent is the 5 mph
stuck rule plus the nav-graph re-place.  **Recoverable — partially** (the 5 mph
rule is known); the rest exists because the harness's road representation is
two drive lines (full.c:5645).  **Blocked on:** the nav graph (PH-10/PH-12).

---

## C. RE-AUDITED TO PROOF GRADE — four of the six fell

Every row of the old "not recoverable from the image" section was re-audited
with exhaustive writer scans and `xref --imm` sweeps.  **PH-18, PH-21 and
PH-23 are recovered; PH-22 splits; PH-19's producer is located (its VALUE is
file data); only PH-20 stands, and it was never a physics invention.**

### PH-18 — steering axle pick — **RECOVERED, the row's reason was false**

Retail has **no per-axle steer flag anywhere**.  The rule is hard-coded
**"wheel index < 2 steers"**.  Proof: the steered-wheel basis is cached at
`veh+0x14E0` (right) / `veh+0x1500` (at); a modrm-aware sweep of all of
`.text` for those two displacements returns **6 hits total** — one write
@0x0011DD04 (in `FUN_0011D460`, from `FUN_00011900(axis-angle, veh+0x1164)`
fed @0x0011DD3D), its `lea` @0x0011DD13, the two basis reads @0x0011DE5E and
@0x0011DE6D, and two unrelated matrix-init `lea`s (@0x0011AA6A, @0x001201E2).
Both basis reads sit inside one guard, `cmp esi,2 / jge` @0x0011DE56, with
`esi` the wheel index of a **fixed 4-iteration** loop (`inc esi` @0x0011E65C,
`cmp esi,4` @0x0011E666).  The same `cmp esi,2` splits front/rear free-roll
(@0x0011DEFE), front tyre stiffness (@0x0011DF81), and more (@0x0011DE8E,
@0x0011E278, @0x0011E401); `FUN_00134710` @0x00134795 and `FUN_00123FD0`
@0x001240A6 split the attach height and the spring set the same way.
**Data check:** over all 78 extracted `build/cars/*.wheels`, retail's `w < 2`
and the harness's geometric `z > mean(z)` agree **78/78, zero mismatches**.
**Spec:** replace the geometric test at `src/burnout3_full.c:3326` with
`w < 2`.  Identical today, correct by construction for any `.bgv`.  [C]

### PH-19 — AI aggression producer — **producer LOCATED; the value is `.bgd` payload**

`DAT_0073A170` is a pointer with **exactly 5 absolute references in the whole
image**: reads @0x001727D3 and @0x00172937 (the consumer `FUN_00172870`,
record = `ptr + veh+0x19BC * 0x98`, aggression byte at `+0x90`), writes
@0x0018B458 and @0x0018B478, and the zero-init @0x0018BDB8.  Both writes are
in **`FUN_0018B250`**, an async `.bgd` loader state machine reached only from
`FUN_001AA100` @0x001AA17B: state 2 opens (`FUN_001B33A0`), states 3/4/6 read
0x800 / 0x2800 / 0x800 byte blocks, state 7 publishes
`DAT_0073A170 = obj+0x568 + obj+0x57C*0x390` (with the slot count at
`+0x2010 + obj+0x57C*4`) or `= obj+0x564` (count at `+0x3B4`).  The arithmetic
pins the shape: `0x2010/0x390 = 9` groups x `0x390/0x98 = 6` slots x `0x98`
bytes.  **The chain dies at the file-read vfuncs** — the record is raw
payload, never computed by any instruction.  So the row keeps its verdict but
changes its reason: *not* "the producer is not located", but "the producer is
`FUN_0018B250` and the value lives in `Gamedata.bgd` (string @0x003AB414)".
The `1.0` stand-in is correct-by-construction only for chase mode, where
`FUN_00172870` writes `[0x003B168C] = 1.0` on `racecar+0x2450 == 1`.  [C]

### PH-20 — box hull fallback — **CONFIRMED (with a scope note)**

The real car hull is real data: `.bgv+0x1060`, a 0x600-byte record copied to
`veh+0x220` by `FUN_00122C20`, with `veh+0x208 = veh+0x220` set by the
vehicle ctor @0x00122698; all 107 `pveh/*.bgv|btv` carry a well-formed one.
So `b3_carcol_hull_from_extents` is purely a data-availability fallback.
Retail DOES have a box-hull builder, `FUN_00156510` (8 verts, 6 axis planes
from `DAT_004130C0..0x00413118`, 12 tris, **18** edges), but its callers are
exhaustively `FUN_001069C0` (detached parts, via `FUN_00111340`),
`FUN_00106F20` (detached panels/wheels) and `FUN_0011A020` @0x0011A132 (the
class-6 prop).  **No car body ever gets a box hull in retail.**  [C]

### PH-21 — wreck ground test — **RECOVERED (was mis-filed)**

See PH-06 above: the racecar vtable's slot +0x10 (`FUN_0011BE40` ->
`FUN_00122D00`) resolves a crashed car against the world through
`FUN_00109D20` + **`FUN_00109EA0`** @0x00122F81 + `FUN_001239C0` @0x00122FD2,
and `FUN_00123000`'s loop is `FUN_00123FD0` + `FUN_00109560`.  The resolve is
now ported (PH-24).  The settle gate is `0.5 [0x003B1684] > speed`
@0x00123518 and `0.25 [0x005A80B8 <- 0x003B1730] > |omega|^2` @0x00123551 and
`veh+0x1354 > 5 [0x003EBF88]` @0x00123595 -> zero every wheel omega and set
`veh+0x20E = 1`.  **Blocked only on the soup**, same as PH-06.  [C]

### PH-22 — reconstruction glue — **SPLITS: `:173` recovered, `:224` blocked**

`:173`'s stated reason ("the harness has no `.bgd` surface record") is wrong.
Retail's brake-drag direction is `veh+0xC0`, the unit travel direction
`FUN_000FFC80` maintains inside `FUN_00109560` — no surface record involved.
`FUN_0011D460` @0x0011DBD7..0x0011DCF7 [C]: `pt = pos + up * veh+0x1368`;
`s = veh+0x13CC * gate - veh+0x138C * brake * 20000.0 [0x003A35E4]`;
`s *= (speed + 1)`; `s *= 0.014285714 [0x003B1D8C] = 1/70`; drifting with
`veh+0x142C < 0.3 [0x003B1750]` zeroes it and moves `pt` to the origin;
`F = veh+0xC0 * s`; `FUN_001064B0(F at pt)` @0x0011DCF7.  Our
`b3_brake_drag_scalar` already matches the scalar; what is missing is only the
APPLICATION as a force at `pos + up*BrakeForceHeight` (which also makes a
pitch torque).  Portable today.
`:224` stays blocked on the soup, but "not recoverable" is wrong — the
builder, the grid and the record layout are all pinned: `veh+0x200` always
points at the ONE global list `0x005A3AA0` (count there, 0x40-stride records
via `0x005A3AA4 -> 0x005A3AB0`, u16 types via `0x005A3AA8 -> 0x005A52B0`);
the literal is stored at exactly 6 sites (@0x001068B0, @0x00107325,
@0x00119F51, @0x0011A5C7, @0x0012250F, @0x00122D20); the driving car's builder
is `FUN_0011BC60` (@0x0011BD50 cell lookup, @0x0011BD9B gather, then up to 6
of the car's own volumes from `veh+0x11D0`); the wreck/prop/debris builder is
`FUN_00109D20` (`FUN_001AD4A0` over the grid at `DAT_007397C8` stride 0x1CC
indexed by `veh+0x217`, then `FUN_0019D3B0` with callback `FUN_00109CE0`).  [C]

### PH-23 — prop ground stop — **REFUTED AND RECOVERED**

The row claimed "retail has no ground pass for a class-6 body at all —
`FUN_0011A330` is two drag terms and `FUN_00109560`, and the vtable at
0x003B1120 has no other per-frame slot".  **The second clause is false.**
The class-6 vtable's slot **+0x10 is `FUN_0011A490`**, and the collision
manager `FUN_00110AF0` drives it once per frame per allocated body
(@0x00110DC4, `call [vtbl+0x10]`, for body types {0,1,2,4,6,7}).
`FUN_0011A490` caches a collision sector in `+0x230` against a validity sphere
`+0x560/+0x56C`, redirects `+0x200` to the staging list `0x005A3AA0`, calls
`FUN_00109D20` @0x0011A5FB, and then calls **`FUN_00109EA0` @0x0011A706**,
guarded by `+0x216 != 0xFF`; when `+0x216 == 0xFF` (outside every loaded
streaming unit) it instead CLEARS `+0xF0/+0x100/+0x110/+0x120/+0x130`
@0x0011A6D5 and skips.

So a retail knocked prop **is** resolved against the world, **before** its own
update integrates.  `src/burnout3_props.c` now does exactly that:
`b3_rigid_body_obb_plane_contact` (PH-25) + `b3_rigid_body_world_contact`
(PH-24) at restitution `+0x1F8 = [0x003A69C4] = 0.1`, then `b3p_body_step`.
`B3P_GROUND_SKIN` and the "no ground pass" note are deleted; the OBB comes
from the model bbox the way `FUN_0011A020` @0x0011A0A8/@0x0011A0AF fills
`+0x1D0`/`+0x1E0`.  **Measured:** a knocked cone settles at a fixed
`y - ground = +0.242 m` with the freeze latch raised, dead still for 25 s
(capture `scratchpad/physledger/prop_rest.png`); before the change it
integrated through the surface.  `B3_PROP_BALLISTIC=1` still skips the pass.

### Still not recoverable

| id | site | why (re-audited) |
|---|---|---|
| PH-19 | `full.c:1612-1681` aggression world inputs | producer located (`FUN_0018B250` @0x0018B458/78); the VALUE is `Gamedata.bgd` payload, 9 groups x 6 slots x 0x98. Not in the executable. |
| PH-20 | `carcol.c:100` box hull fallback | data-availability fallback only; the real hull IS recovered, and retail's own box builder never serves a car body. |
| **PH-11** | `full.c:126, 132, 2133-2152` crash-release bound | **NEW (wave 3), proof-grade.** No timed crash releaser exists in the image: the field the row named (`racecar+0x19BE`) is the asset-load latch (1 write, immediate `1`, never cleared, 2 readers — all-segment sweep), the 5-second stamp `racecar+0x240C` has ZERO readers, and the wreck-exit virtual (racecar vtable slot +0x10) is dispatched from only two non-timer sites. See section B. |
| ~~PH-22b~~ | — | **CLOSED (wave 3)** — the marks lived in `b3_vehicle_step()`, which had no caller; deleted. |

---

## D. PRESENTATION MARKS (no body moves) — 219 across 24 files

| file | marks | nature |
|---|---|---|
| `burnout3_carfx.c` / `.h` | 41 | shader/art stand-ins, envmap, damage decals |
| `burnout3_hud.h` / `.c` | 24 | layout, ticker, tag placement |
| `burnout3_postfx.c` / `.h` | 26 | bloom/zoom strengths, LUT ramps |
| `burnout3_sfx.c` / `.h` | 16 | mix balance, cue routing |
| `burnout3_takedown.c` / `.h` | 15 | camera director yaw/pitch/ease |
| `burnout3_particlefx.c` / `.h` | 12 | emitter counts, art sourcing |
| `burnout3_music.c` / `.h` | 10 | playlist, ducking |
| `burnout3_panels.h` | 6 | doc echoes of PH-05 |
| `burnout3_trackmesh.c` | 5 | headroom curve (TUNED, user-authorised), GLSL vs NV2A |
| `burnout3_td_rules.h` / `.c` | 5 | doc marks on omitted inputs |
| `burnout3_score_events.h` | 4 | doc marks |
| `burnout3_crash.h` | 3 | doc marks |
| `burnout3_boostfx.c` / `.h` | 4 | flame level |
| `burnout3_carcol.c` (guards) | 3 | bounds guards, not laws |
| `burnout3_full.c` (display) | 8 | MSAA, anisotropy, brightness LUT, camera offsets |
| `burnout3_gameplay.h`, `burnout3_ai.h`, `burnout3_props.h` | 3 | doc marks |

---

## E. CALL-GRAPH AUDIT — retail's per-frame physics order vs ours

Retail runs **the entire physics frame in ONE call**:
`FUN_00156400` -> `FUN_000165F0` -> `FUN_00017060` -> `FUN_001AA720`
@0x001AA85A -> **`FUN_00110AF0(physworld, dt)`**.  `FUN_0011BE50` is not a
sibling of it — it runs *inside* it.  The class installer is `FUN_001A99F0`
(physworld = world+0x3C910):

| array | count | stride | vtable | slot +0 (update, dt) | slot +0x10 (world contact) |
|---|---|---|---|---|---|
| +0x1CB80 player cars | +0xE6C80 | 0x4AD0 | 0x003B1160 | `FUN_00117870`->`FUN_0011BE50` | `FUN_0011BE40`->`FUN_00122D00` |
| +0x26120 | +0xE6C84 | 0x1580 | 0x003B1240 | `FUN_00104A90`->`FUN_0011BE50` | `FUN_0011BE40` |
| +0x2CCA0 | +0xE6C88 | 0x1560 | 0x003B1138 | `FUN_0010F370`->`FUN_0011BE50` | `FUN_0011BE40` |
| +0x33780 traffic (64) | +0xE6C8C | 0x2430 | 0x003B11EC | `FUN_00120F30` | `FUN_00122D00` |
| +0xC4380 props (32) | mask +0xE9CA0/A4 | 0x780 | 0x003B1120 | `FUN_0011A330` | `FUN_0011A490` |
| +0xD3380 debris (64) | mask +0xE9C98/9C | 0x4E0 | 0x003B1108 | `FUN_00106D00` | `FUN_001072A0` |

Retail's order inside `FUN_00110AF0`:
AABB refresh (`FUN_00114270`) -> sweep-and-prune + `_qsort` -> **every
class's `vtbl+0x10` world contact** (@0x00110DC4, gated `[obj+0x20E] == 0 ||
type == 7`, then `[obj+0x211] = 0`) -> **every pair's narrow phase**
(`FUN_00111CD0` @0x00110DF4) -> `[car+0x1353]` clear pass -> `FUN_00114E60`
-> `FUN_0010D1C0(dt)` -> **car updates** (@0x00110EE7/F15/F45/F75) ->
**prop updates** (@0x00110FBB) -> **debris updates** (@0x00111011) ->
`FUN_0012FA40` (@0x001110E2).

| # | retail stage | our equivalent | verdict |
|---|---|---|---|
| 1 | `FUN_00110AF0` = one physics call | spread over `full.c` `game_update()` :7164-:7372 in five stages with gameplay code between | **SPLIT** |
| 2 | AABB refresh, all classes | `carcol_fill_racer`/`_traffic` — cars + traffic only | SPLIT |
| 3 | sweep-and-prune, all classes | `b3_carcol_broadphase` at frame END; props bolted on as a separate O(n·m) pass | REORDERED + SPLIT |
| 4 | `vtbl+0x10` world contact, **before every integrator** | props: SAME-ORDER (wave 2); debris: same-order via hunk P1; wreck: **NOW SAME-ORDER** (`b3_wreck_world_contact`, hunk F3); live car: **NOW IN THE SUBSTEP** (B4) | **SAME-ORDER** |
| 5 | `[obj+0x211] = 0` per body, `[car+0x1353]` clear pass | — | MISSING |
| 6 | pair narrow phase `FUN_00111CD0`, before every integrator | `b3_carcol_resolve` + `b3_props_collide_car`, after | REORDERED + SPLIT |
| 7 | car `vtbl+0` -> `FUN_0011BE50` | `b3_vehicle_step_full` | SAME-ORDER |
| 8 | traffic `vtbl+0` -> `FUN_00120F30`, a real rigid body | persistent traffic/trailer bodies, residency/sleep gates and normal tow constraint; lane driver remains harness-controlled | PARTIAL |
| 9 | prop `vtbl+0` -> `FUN_0011A330` = drag + `FUN_00109560` | `b3p_body_step` — **now update-only, contact moved out** | SAME-ORDER |
| 10 | debris `vtbl+0` -> `FUN_00106D00`, all 64 slots every frame | `panels_pieces_update` advances every detached panel once after the vehicle, traffic, and prop passes; each piece gets its own collision down-ray height | PARTIAL — global cadence landed; narrow phase is still one ground plane |
| 11 | `FUN_0012FA40` inside the physics call | `tdr_frame_pass` at frame end | REORDERED |
| B1 | `FUN_0011BE50` @0x0011BF43 `FUN_0011BC60` — soup gathered ONCE per frame, outside the substeps | **NOW SAME-ORDER for chassis and rays**: `v->soup_freeze` takes one raw local collision snapshot; rays use it directly and the chassis view applies the recovered `FUN_0011BBE0` wall predicate. Retail's crash-floor append remains absent: `FUN_00125790` builds six `veh+0x11D0` records from a crash-director basis and sets `+0x1351`, then `FUN_0011BC60` appends surfaces `0x26,0x26,0x1A,0x1A,0x1A,0x1A`. The harness does not model that basis, so it must not synthesize records | PARTIAL |
| B2 | @0x0011C048 `dt *= 0.5`, n = 2 (gate `[[veh+0x13F4]+0x1920] == 0`) | `vehicle_sim.c` always 2 | SAME-ORDER (race path); n=1 mode unmodelled |
| B3 | @0x0011C0A2 `FUN_0011D460` | `b3_d460_force_pass` | SAME-ORDER |
| **B4** | **@0x0011C0B7 `FUN_0011AEF0`** — chassis-vs-soup, INSIDE the substep, between the force pass and the pre-pass, twice per frame | `v->chassis_resolve` at exactly that point in `b3_vehicle_step_full`, running `b3_crash_response` over the frozen soup; the `eax != 0` / `eax == 0` arms @0x0011C0C0/CA/D3 are the retail ones | **SAME-ORDER (14 trajectory differentials against the retail instruction stream)** |
| B5 | @0x0011C0E7 `FUN_001239C0` | `b3_prepass` | SAME-ORDER |
| B6 | @0x0011C0EE `FUN_00123FD0` | `b3_suspension_pass` | SAME-ORDER |
| B7 | @0x0011C160 `FUN_00109560` | `b3_rigid_body_integrate` | SAME-ORDER (verbatim) |
| B8 | @0x0011C213 `FUN_00126D40` + @0x0011C21E `FUN_00126520` on a LIVE car | panels only run on the crashed path | MISSING |
| ACC | `FUN_00109560` consumes and clears `+0x110`/`+0x120` (@0x00109728..0x0010983B) and `+0x130` (@0x00109A27..0x00109A3E); nothing else touches them | `b3_rigid_body_integrate` | **SAME-ORDER (verbatim)** |
| X | — | AI velocity governor between the integrator and the collision pass (`full.c:2703`). The wreck sphere sweep is demoted to an anti-tunnelling position net (F3) and PH-08's post-step velocity write is deleted (F2d) | EXTRA (PH-07) |

**The gaps, ranked — after wave 3.**

Gaps 1, 2 and 5 of the wave-2 list are CLOSED.  `FUN_0011AEF0` runs at
@0x0011C0B7's position inside the substep (B4), the soup is frozen once per
frame at @0x0011BF43's position (B1), the wreck resolves through the shared
`FUN_00109EA0` before its integrator (4), and the invented 1..8 `mlen/0.6`
loop and PH-08's velocity write are deleted.  What is left:

1. **Props and debris are not in the broadphase** (3), so pair ordering and
   the A/B swap differ from retail in pileups.
2. **Traffic remains hybrid** (8) — PH-07/PH-13. `FUN_00120F30`'s
   streaming-unit gates, coupled sleep, persistent bodies, full hitch anchors,
   normal tow constraint and kingpin spring are live; the lane-route driver
   and detachment branches still differ from retail. `+0x13A0` is the model
   pointer for the attach records, not a route driver.
3. **The contact GEOMETRY is partly unified** (PH-09). Live-car, wreck, and
   knocked-prop response receive gathered collision triangles. Sphere sweeps
   remain only as anti-tunnelling containment and no-pipeline fallbacks; the
   retail type-3 traffic lifecycle remains outside the shared path.
4. **The shared raw snapshot still lacks retail's crash-floor append** (B1).
5. **Four manager stages are unported [?]**: `FUN_00114E60` @0x00110EB9,
   `FUN_0010D1C0(0x0064ACE8, dt)` @0x00110ECB (it takes dt, so it is a
   simulation step), `FUN_00164FB0(dt)` x2 @0x001AA8E8/@0x001AA8F7,
   `FUN_00111850` @0x001AA907.
6. **`FUN_0011BE50`'s own head is not run** (0x0011BE5F..0x0011BF43: the
   `+0x19A8` pipeline gate, the `+0x153F`/`+0x1353 |= 0x10` latch, the ctx
   `+0x58` HUD-flag block) — presentation and gating, no body moves.

---

## HEADLINE

Wave 4 landed **the panel-piece seeding** (the last open half of PH-05) and
**closed PH-16 by refuting its own hypothesis**, so the row-by-row tally now
stands at **18 recovered / 3 proven-unrecoverable / 6 blocked**:

* **recovered (wave 4): 2** — **PH-05's SEEDING half**, so PH-05 is closed
  entire: the activation ctor `FUN_001069C0` seeds mass `260.0f`
  (`0x43820000`, unconditional), the OBB from `.bgv+0xEA0 + k*0x20`
  recentred by the `.bgv+0xADC+k` hinge-axis rule, and the inverse-inertia
  diagonal `1/(0.5*m*(b²+c²))` per axis read off the **disassembly**
  0x00109BBF..0x00109CC8 (Ghidra's decompile of `FUN_00109BB0` drops two of
  the three axes) — the invented 3 rad/s tumble was **deleted**, not
  replaced, because retail's extra term is the LOOSE-panel hinge state at
  panel record `+0x90`, identically zero for a panel that never flapped;
  and **PH-16**, closed by proving the producer is retail DEAD CODE (the
  only writer of the `0x4AC2` gate stores `AL = 0`), which also makes
  `takedown.c:857`'s cadence mark stale rather than GLUE.
  Wave 4 also fixed a latent bug — `crash_fired` had no drain and latched
  at 1 for the rest of the race after the first wall crash.
* **recovered (waves 1-3): 16** — PH-01 prop solver, PH-02 orthonormaliser, PH-03
  coordinate space, PH-04 pool init (wave 1); PH-24 the shared body-vs-world
  contact resolve, PH-25 the OBB narrow phase, PH-26 the class-7 update,
  PH-05 (flight half), PH-18 steering axle, PH-23 the prop ground pass
  (wave 2); **PH-08 / gap B4** the substep relocation, **PH-27** the signed
  `FUN_00106720` return, **PH-06 / PH-21** the wreck world pass, **PH-22**
  (closed by deletion), **PH-15** the trailer attach record + tow constraint,
  and **PH-09's WALL arm** (wave 3)
* **proven not recoverable from the image: 3** — PH-19 (the value is
  `Gamedata.bgd` payload; the producer IS located), PH-20 (a data fallback,
  never a physics invention) and **PH-11** (no timed crash releaser exists:
  the named field is a load latch and the 5-second stamp has no reader —
  all-segment sweeps, wave 3)
* **recoverable, specced, and BLOCKED ON ANOTHER AGENT'S FILES: 6** —
  PH-07 and PH-13 (both need the remaining `FUN_00120F30` road-agent branches
  ported into `full.c`'s traffic section), PH-09's type-3 traffic lifecycle,
  PH-10, PH-12 and
  PH-17 (all three need the `.bgd` nav-node walk `FUN_00179760`, AI-DRIVE's
  wave).  Every one of them names the retail function and the blocker; none
  of them is waiting on information from the executable.
* **CLOSED by the CRASH-AUDIT wave: the `crash_fired` CONSUMER switch.**  It
  is landed, its "one wall source" precondition turned out to be met (the
  segment arm is orphaned — `apply_track_constraints()` has no caller, so
  `mesh_collide` scores 0 hits in 60 s of racing), and the one thing it was
  actually missing was retail's CRASH-ENTRY VETO `veh+0x1353` bit 3
  (`FUN_00105BD0` @0x00105F95 → `FUN_0011AEF0` @0x0011B94D).  Without it every
  AI car past 79 m ran a wall-crash gate 33x easier than the player's and the
  pack wrecked itself on the first barrier.  Fixed and differentially proved;
  see the SUPERSEDED block under PH-08 / gap B4.  The paragraph below is the
  wave-4 state and is kept only for the record:
  ~~decided, and deliberately NOT landed: the `crash_fired` CONSUMER
  switch.~~  `crash_fired` IS the retail-faithful decision (retail's contact
  set, program point and cadence; proven instruction-level by the 14
  relocation cases), but the in-game differential — 2031 traced frames, 6
  cars, 60 s — shows the two arms are fed by two **disjoint** wall systems
  and never fire on the same frame (107 vs 10 frames, 14 vs 6 events, zero
  overlap).  Switching today swaps one non-retail contact set for another:
  2.3x the crash rate, and every crash currently routed through
  `b3_td_on_crash` is lost.  **Blocked on one wall source** (PH-06/PH-08
  lineage).  Retail's latch/cooldown is documented `[C]`
  (`FUN_0010DD20`: `veh+0x210 != 0` plus the per-slot timer at
  `mgr + slot*0x3C + 0x130`) and maps onto the harness's existing
  `crashed_until`/`immune_until` pair, so the switch is a two-line change
  the day the wall sources merge.  Trace it with `B3_CFIRE_TRACE=1`.

**The relocation, in one line.**  `FUN_0011AEF0` now runs at @0x0011C0B7's
position inside `b3_vehicle_step_full`'s substep, over a soup frozen at
@0x0011BF43's position, so its impulse and deflection are consumed by that
substep's `FUN_00109560` — proved by **14 trajectory differentials against the
retail instruction stream 0x0011C0A0..0x0011C16C executed verbatim**, and it
deleted PH-08's GLUE outright.  It also exposed a bug no other case could
reach: `FUN_00106720` returns the SIGNED impulse (XMM0 keeps the quotient; the
`fabs` at 0x00106874 only scales the output vector), and `FUN_0011AEF0` skips
its whole impact/impulse block on `dv <= 0` @0x0011B771 — so retail does not
kick a chassis that is already separating from a wall, and our port did.

Differential coverage added this wave: `tools/validate_port.py` **127 -> 141**
cases (14 in-substep chassis-contact trajectories), all against the real x86
under Unicorn with the C port itself on the other side.

**The single highest-value remaining item** is now the retail road-agent
driver: bind its path tables, reproduce four-knot interpolation and avoidance,
and preserve route state through coupled-unit streaming (PH-07/PH-13).
