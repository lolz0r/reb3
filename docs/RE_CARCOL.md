# Car-vs-car collision — the recovered chain (2026-08-11)

How Burnout 3 makes two *vehicles* collide with each other: pair enumeration,
the convex-hull narrow phase, the mutual response, and the slam/takedown
classification that turns a contact into a takedown.

Evidence marks: **[C]** = confirmed by executing the real x86 under Unicorn
and asserting a 1:1 C mirror reproduces its writes; **[S]** = read from the
instructions, constants confirmed in the image, no green case; **[?]** = open.
Anything not from the binary is marked **GLUE**.

Acceptance: `tools/validate_carcol.py` — **730/730 green**. Every case seeds
two vehicles, runs the real chain under Unicorn (`tools/emulate_carcol.py`)
and the compiled `src/burnout3_carcol.c` from identical state, and diffs the
contact point, normal, per-body separation, impulses, forces, torques, impact
magnitude, slam classification, and crash triggers field for field.

All addresses are the **corrected ELF mapping** (`build/burnout3.elf`, see
HANDOFF §2). Old flat-load addresses are `new − 0x10000` in `.text`.

---

## 0. The chain at a glance

```
FUN_001AA720                       (collision manager tick)
└─ FUN_00110AF0   [C-disasm]       sort + sweep over per-object world AABBs
   │                               -> pair array (stride 0x30, cap 0x100)
   ├─ FUN_00114270 [C]             per-object world AABB  (rec +0x10 / +0x20)
   ├─ FUN_00114610 [S]             pair filter (3-vs-3, 5-vs-5, both asleep)
   └─ FUN_00111CD0 [C-disasm]      per-pair dispatch on the object type byte
      ├─ both cars, BOTH un-crashed (veh+0x210 == 0)  -> FUN_001121F0  [C]
      ├─ both cars, one/both crashed                   -> FUN_00113960  [C]
      ├─ car vs type-3 object                          -> FUN_00112E70  [S, not ported]
      └─ car vs type-5 object                          -> FUN_00113890  [?]

FUN_001121F0 / FUN_00113960 both open with
   FUN_0010A9D0  [C]  build the hull-query context (two frames, two inverse
                      frames, two hulls, mode byte)
   └─ FUN_0010ABC0 [C] coincidence reject, then
      └─ FUN_0010AC20 [C] the convex-hull narrow phase
         ├─ FUN_000116E0 matrix multiply        ├─ FUN_00013CA0 point xform
         ├─ FUN_0010B210 support-edge soup      ├─ FUN_0010C0D0 clip+dedup
         │  └─ FUN_0010C220 half-space clip     ├─ FUN_00038C00 batch xform
         ├─ FUN_0010B310 AABB centre            ├─ FUN_0010C000 closest plane
         ├─ FUN_00031330 rotate vector          └─ FUN_0010BE70 ray exit plane
```

---

## 1. Broad phase — FUN_00110AF0 [C for the predicate]

The collision world object (`param_1`) holds:

| offset | meaning |
|---|---|
| `+0x70` | object records, **stride 0x30** |
| `+0x1CB70` | object count |
| `+0xE5F0` | sort array, stride 8: `{float key, u16 obj, u8 begin}` |
| `+0x1CB74` | sort entry count (2 per object) |
| `+0xE6C90` | pair records, **stride 0x30** |
| `+0xE9C90` | pair count (cap `0x100`) |

Object record:

| offset | meaning |
|---|---|
| `+0x00` | **type byte** (0..8) |
| `+0x01` | flag |
| `+0x04` | frame (4×4) pointer |
| `+0x08` | `{bbmax, bbmin}` vec4 pair = `veh+0x1D0` |
| `+0x0C` | vehicle pointer |
| `+0x10` | world AABB **lo** (vec4) |
| `+0x20` | world AABB **hi** (vec4) |

`FUN_00114270` recomputes `+0x10`/`+0x20` from the frame rows and the box
(standard OBB→AABB: centre ± Σ|R<sub>ij</sub>|·half<sub>j</sub>) [C — 6
asserts, `validate_carcol.py` "broad phase"].

The sweep sorts the 2·N interval endpoints by the **x** key
(`rec+0x20` for the end entry, `rec+0x10` for the begin entry;
`_qsort` with comparator `LAB_00110AD0`), then walks the sorted list keeping
an active set and testing each new object against it with
`b.lo.z < a.hi.z && a.lo.z < b.hi.z` **and** `b.lo.y < a.hi.y && a.lo.y <
b.hi.y` plus `FUN_00114610`. Emitted pairs are ordered
(`FUN_00011510`/`FUN_000114E0` = max/min index).

The port keeps the predicate exactly and replaces the sort+sweep with the
direct O(n²) test: **the emitted set is identical** (SAP is an acceleration
structure over the same 3-axis AABB overlap), and the harness has 18 bodies.
`b3_carcol_broadphase()`; documented as an equivalence, not a behaviour
change.

`FUN_00114610` [S]: rejects type-3 vs type-3, type-5 vs type-5, and any car
pair where **both** have `veh+0x20E == 1` (asleep).

### Object types [S, from the dispatch and the class map]

`FUN_0010C550` returns "is a car" for types **0, 1, 2**.
`FUN_0010FBC0` maps type → interaction class:
`0,2 → 0`; `1 → 1`; `3 → 2`; `4 → 3 or 5` (5 when
`veh+0x242B != DAT_0073BB8C`); everything else → 6.

Two compiled tables index by that class:

* `DAT_0039AE50[a*7+b]` (u8, "can crash") =
  `row0 [1,1,1,1,1,1,0]`, `row2 [1,0,0,1,0,0,0]`, all other rows 0.
* `DAT_0039AE88[a*7+b]` (u32, interaction kind, read by `FUN_0010FC50`) =
  `row0 [0,1,0,0,1,1,0]`, `row1 [2,0,2,2,0,2,0]`, `row2 [3,…]`,
  `row3 = row0`, `row4 [2,0,2,2,0,2,2]`, `row5 [2,2,2,2,2,2,0]`, `row6 [0…]`.
  Kind **2** means "this side is immovable in the response".

---

## 2. The collision hull is real data — .bgv +0x1060 [C]

`FUN_00122830` (the collision-object init) does
`FUN_00122C20(veh+0x220, bgvfile+0x1060)` and then sets
`veh+0x208 = veh+0x220`, relinking the record's internal offsets to
pointers. `FUN_00122C20` is a fixed-shape copy of **0x600 bytes**:

```
hull record (0x600 bytes; at .bgv +0x1060, copied to veh+0x220)
  +0x000  u32[7] header; the first five are internal offsets
          (0x1C, 0xA0, 0x320, 0x480, 0x4F8) that the copier turns into
          absolute pointers at +0x00/+0x04/+0x08/+0x0C/+0x10
  +0x014  u32  = 2
  +0x018  u8   vertex count   (<= 22)
  +0x019  u8   plane count    (<= 40)
  +0x01A  u8   edge count     (<= 60)
  +0x01C  u8[40][3]   per-plane triple (unread by the contact chain)
  +0x0A0  f32[40][4]  PLANES  {n.xyz, d}; INSIDE iff dot3(n,p) <= d
  +0x320  f32[22][4]  VERTICES (car-local)
  +0x480  u16[60]     EDGES: low byte = v0, high byte = v1
  +0x4F8  u32[22][3]  per-vertex triple (unread by the contact chain)
```

Verified on the retail files: COMP/Car1 = 16 verts / 28 planes / 42 edges,
SUPR/Car1 = 16 / 28 / 42; every one of the 107 `pveh/*.bgv|btv` files carries
a well-formed record. Extract with

```bash
python3 tools/emulate_carcol.py --extract-hulls     # -> build/cars/*.hull
```

Note the hull is **not** the `+0x1D0`/`+0x1E0` box: on COMP/Car1 the hull's
nose reaches z = 2.143 against `bbmax.z` = 2.064, and 53 of 107 cars have
hull tails behind `bbmin.z`. That difference is load-bearing — see §5.

---

## 3. Narrow phase — FUN_0010A9D0 / FUN_0010ABC0 / FUN_0010AC20 [C]

`FUN_0010A9D0` (regparm3: `EDX = vehA`, `ECX = vehB`; stack `ctx`, `mode`)
fills a 0x210-byte query context:

| ctx offset | contents |
|---|---|
| `+0x000` | A hull (`vehA+0x208`) |
| `+0x030` | A frame (4×4, rows right/up/at/**pos**) |
| `+0x070` | A inverse frame (`vehA+0x70`) |
| `+0x0F0` | B hull |
| `+0x120` | B frame |
| `+0x160` | B inverse frame |
| `+0x1E0` | mode (`0` from FUN_001121F0, `1` from FUN_00113960) |
| `+0x1E4` | result flag, set to 1 on contact |
| `+0x1F0` | contact **normal** (world) |
| `+0x200` | contact **point** (world) |

`+0x60` and `+0x150` are the copies of the two frames' positions; the solver
writes the **separated** positions back into them, so
`ctx+0x60 − frameA.pos` is A's push-out displacement (and likewise for B).

`FUN_0010ABC0`: `|posA − posB|² < 0.0009` → no contact.

`FUN_0010AC20`:

1. `M_A2B = frameA · invB`, `M_B2A = frameB · invA` (FUN_000116E0, row-vector).
2. `pA_in_B = invB · posA`, `pB_in_A = invA · posB` (FUN_00013CA0, 4 lanes).
3. A's vertices → B-local via `M_A2B`; B's vertices → A-local via `M_B2A`.
4. `dir = normalize(pB_in_A)`; `FUN_0010B210` emits **both endpoints of every
   edge of A whose two vertices satisfy `dot3(dir, vert_local) >= 0`** — the
   support-side edge soup — taking the points from the B-local vertex array.
5. `FUN_0010C0D0` clips that segment list against **all** of B's planes
   (`FUN_0010C220` per plane, ping-ponging two 120-vec4 scratch buffers at
   `DAT_005A53C0`/`DAT_005A5B40`), then copies the survivors out, dropping
   duplicates component-wise at `1e-7` (`0x0039AACC`).
6. Symmetrically for B against A's planes, appended after A's points.
   `total <= 1` → no contact.
7. Group 1 (B-local) is lifted to world by `frameB`, group 2 (A-local) by
   `frameA` (`FUN_00038C00`).
8. **Contact point** = centre of the AABB of all points (`FUN_0010B310`,
   `(min+max)*0.5`, all four lanes).
9. **Contact normal** = `normalize(rot(frameA, planeA[iA]) −
   rot(frameB, planeB[iB]))` where `iA`/`iB` are the planes with the smallest
   `|dot3(n, cp_local) − d|` (`FUN_0010C000`). `|n|² < 2.3283064e-10`
   (`0x003B191C`) → no contact.
10. **Penetration**: `FUN_0010BE70` shoots the segment
    `cp_local → cp_local + centre_to_centre*100` (`0x003A2928`) through the
    hull's planes and returns the **last plane crossed**; then
    `pen = plane.d − dot3(cp_local, plane.n)`, clamped at 0, and the body's
    position is moved by `rot(frame, plane.n * −pen)`. Each body is separated
    along **its own** exit plane. `ctx+0x1E4 = 1`.

Verified: 8 poses (side-by-side, nose-to-tail, angled, T-bone with two
different cars, deep overlap, rear-into-nose, pitched, and a no-contact case)
— point, normal and both displacements match to 1e-4, and the miss case
misses. 36 asserts.

---

## 4. Racer vs racer — FUN_001121F0 [C]

`thiscall`-ish, `[EBP+8] = pair`. `A = pair+0x24 → veh`, `B = pair+0x28 → veh`.
`FUN_00111CD0` only routes here when **both** cars have `veh+0x210 == 0`
(RE_NOTES §14: `+0x210` set selects the crashed/simplified path).

1. `FUN_0010A9D0(vehA, vehB, ctx, mode = 0)`; `pair+0x2C = hit`. No hit → out.
2. `vehA+0x211 = vehB+0x211 = 1` ("touching a car this frame").
3. **Separation** (only when `ctx+0x1E4`):
   `dA = ctxPosA − posA`, `dB = ctxPosB − posB`, **both `.y` zeroed** — car
   separation is purely horizontal. `D = dA − dB`, `w = mA/(mA+mB)`:
   * `vehB+0x212` set → `vehA+0x130 += D`
   * else `vehA+0x212` set → `vehB+0x130 += −D`
   * else `vehA+0x130 += D·(1−w)` and `vehB+0x130 += D·(−w)` — the heavier
     car moves less.
4. **Contact point fix-up**: `pair+0x00 = ctx contact point` but with
   `y := posA.y * 2.0 * 0.5 + 0.1` (`0x003B1688`, `0x003B1684`,
   `0x003EBE40`), i.e. the frame-origin height plus 10 cm.
   `pair+0x10 = normal` with `.y := 0`. Both cars get `veh+0x150 = pair+0x00`.
5. **Longitudinal contact parameters**
   `tA = clamp( dot3(cp − (posA + atA·bbminA.z), atA·(bbmaxA.z − bbminA.z))
                / |atA·(bbmaxA.z − bbminA.z)|², 0, 1 )`, and `tB` likewise
   (`veh+0x1D8` = bbmax.z, `veh+0x1E8` = bbmin.z). 0 = tail plane,
   1 = nose plane.
6. **Relative velocity** `vrel = vp(B, cp) − vp(A, cp)` (`FUN_001066A0` =
   `vel + ω × (cp − pos)`), `vn_mph = |dot3(vrel, n)| · 2.23693633`
   (`0x0038994C`, the TRUE mph constant, not the physics 2.2374146).
7. **Mutual impulse** `FUN_0010F8D0(EAX = vehB, ECX = vehA, ptA, ptB, vrel,
   n, e = 0.1 [0x003EBE3C], out)`:
   ```
   j   = | −(1+e)·dot(n, vrel)
           / ( 1/mA + 1/mB + dot(n, cross(IinvA·(rA×n), rA)
                                  + cross(IinvB·(rB×n), rB)) ) |
   out = n · (−j)
   ```
   Returned in XMM0. If `j > 0`: **`vehA+0x110 += out`, `vehB+0x110 −= out`**
   — a **pure linear** impulse (no `FUN_00106500`, so no angular part).
8. **The shove** — this is the shunt. Each car gets a FORCE at the contact
   point:
   ```
   fA = n · min(mB, 2000) · (−20.0) · (|vehB+0x1408| + 1)
   fB = n · min(mA, 2000) · (+20.0) · (|vehA+0x1408| + 1)
   ```
   (`0x003EBE70` = 2000, `0x0041A4D0` = 20). Each is added to `veh+0xF0`
   **and then** routed through `FUN_001205E0`, which adds it to `+0xF0`
   *again* and, when it does not take the linear-only branch, also adds the
   torque `(cp − pos) × f` to `+0x100`. **The linear component is therefore
   applied twice and the torque once** — reproduced verbatim, and the
   differential cases confirm it. `FUN_001205E0`'s routing:
   drifting (`veh+0x1524 ∈ {1,2}`) → linear only; else if `|ω.y| > 2.0`
   (`0x003EBF68`) and `sign(ω.y) == sign(torque.y)` → linear only; else
   force + torque at the point (`FUN_001064B0`).
   The torque is what yaws the victim out of line.
9. **Impact magnitude** `pair+0x20 = (mA + mB) · vn_mph · 0.1 · 0.5`
   (`0x003EBE74`, `0x003B1684`).
10. **Crash trigger**: `vn_mph > 150` (`0x003EBE4C`) → `FUN_0010DCA0` for A
    if `DAT_0039AE50[clsA][clsB]`, and for B if `DAT_0039AE50[clsB][clsA]`;
    otherwise the game-context virtual `+0x64` is called as
    `(1, vehA, vehB, 1.0)` — a "rub". **Either way execution continues into
    the slam classification.**

---

## 5. Slam classification — the takedown entry [C]

Still inside `FUN_001121F0`. `lat = dot3(frameB.row0, posB − posA)`
(which side of B the contact is on), `spA/spB = veh+0xBC`, and
`eps = 1.52587891e-05` (`0x00384208`).

**Rear-end** (`|1 − tA| <= eps` and `|tB| <= eps`, i.e. the contact clamps to
A's nose plane and B's tail plane):

```
require vn_mph > 20            [0x003EBE60]
require spA > spB
s = min((vn_mph − 20) / 50, 1) [0x003EBE68]
light = (0.3 >= s)             [0x003EBE80];  if light and s > 0: s /= 0.3
report vtable+0x64( light ? 4 : 6, attacker = A, victim = B, s )
if not light also FUN_00141700(vehA, ..., vehB)   (sound cue)
on a true return: pair+0x2D = 2
                  victimB+0x153C = (0 > lat);  if light attackerA+0x153C = !that
```

The mirrored branch (`|1 − tB| <= eps` and `|tA| <= eps`) requires
`spB > spA` and reports with **attacker = B**.

**Side** (everything else): `ang` = heading difference of the two flattened
forward axes (`FUN_000FF160`, degrees, computed with `rsqrtss` so it is
approximate by construction).

```
hard   = vn_mph > 35                                   [0x0041A4C4]
hard  |= vn_mph > 20 and |ang| > 40                    [0x0041A4C8 / 0x0041A4CC]
thresh = (35 − 20)/40 · ang + 20
require vn_mph > thresh or hard
s = min((vn_mph − 30) / 20, 1)                         [0x003EBE5C / 0x003EBE64]
light = (0.3 >= s);  if light and s > 0: s /= 0.3
attacker = (tA > tB) ? (spB − spA > 17.8816 ? B : A)   [0x003B1B68 = 40 mph]
                     : (spA − spB > 17.8816 ? A : B)
report vtable+0x64( light ? 3 : 5, attacker, victim, s )
on a true return: pair+0x2D = 1; victim+0x153C = side; if light attacker gets !side
```

The virtual at game-context `+0x64` is the entry to the slam/BP chain
(`FUN_001989A0`, docs/RE_GAMEPLAY.md §6): it awards the attacker Slam BP and
boost and takes the same base off the victim, stamps `racecar+0x1598`
(slam timestamp) and `+0x16BC/+0x16C0` (aggressor + time), and returns
whether the slam counted. Those stamps are what drive the already-verified
steer-away envelope (`FUN_0011ECF0`) and the AI out-of-control authority
`0.1` (`FUN_00105340`) — i.e. **the physical shunt of §4.8 and the loss of
steering control are two separate mechanisms and both start here.**

**Note on reachability [C]:** the rear-end branch needs *exact* clamps on both
parameters, which only happens when the attacker's hull nose overhangs its
`+0x1D0` box and the victim's hull tail overhangs its `+0x1E0` box. 57 of 107
cars have a front overhang and 53 a rear one; COMP/Car4 (+0.197 front) into
SUPR/Car10 (−0.373 rear) reaches it, and that pair is in the suite as the
type-4/type-6 cases. COMP/Car1 into SUPR/Car10 never does (tA peaks at
0.9953) and classifies as a *side* slam instead. This is the retail
behaviour, not an artefact of the port.

---

## 6. Car vs crashed car — FUN_00113960 [C]

`FUN_00111CD0` orders the pair so **A is the un-crashed car**, then calls
this with `(param_1, pair)`, `RET 8`.

* `kindA/kindB = DAT_0039AE88[clsX][clsY]` (`FUN_0010FC50`); both 2 → return.
  A car with `veh+0x210 == 0` is then **forced to kind 2** (immovable).
* `DAT_004A52B3` (retail value **0**) selects an OBB-vs-OBB path
  (`FUN_00108EF0`); with 0 the code takes `FUN_0010A9D0(..., mode = 1)` —
  the same convex-hull narrow phase.
* `vehA+0x211 = vehB+0x211 = 1`.
* Separation, **not** y-zeroed: `D = dA − dB`;
  `kindA == 2` → `vehB+0x130 += (dB − dA)`;
  `kindB == 2` → `vehA+0x130 += D`;
  else mass split `D·(1−w)` / `D·(−w)` with `w = mA/(mA+mB)`.
  (`FUN_00114F30` refines this when `vehA+0x212` is set — mapped, **not
  ported [?]**; the suite's cases keep `+0x212` clear, where it is a no-op.)
* The normal is **blended toward the relative-velocity direction**:
  `n = normalize(n + (−0.9)·normalize(vrel))` (`0x0041A4C0`) unless `vrel` is
  degenerate (`FUN_0003B060`).
* `FUN_0010F8D0` with `e = DAT_004A1D98` (**0.0**), applied through
  `FUN_00106500` — **linear *and* angular** at the contact point, unlike the
  racer path: `kindA == 2` → only B, with the impulse scaled by
  `DAT_003B16C0 = −1`; `kindB == 2` → only A; else A gets `+imp`, B `−imp`.
* `pair+0x20 = j` (the impulse magnitude, or 0).
* Crash trigger: `j > 5000` (`0x003EBE50`), or `> 2500` (`0x003EBE54`) when a
  **traffic (type 2)** car hits an un-crashed non-traffic car; gated by
  `DAT_0039AE50` and by a recent-slam window of **1.5 s** (`0x003EBE7C`)
  measured from `racecarA+0x10DC − racecarB+0x140C` (or the attribution stamp
  `racecarA+0x15A8[victim slot]`).

---

## 7. What the staging soup does — correction to RE_NOTES §15

`FUN_00122D00` (per-vehicle poly staging into `veh+0x200`) does **not**
carry live opponents' hulls in the normal racing case:

* the 4- or 6-plane blocks appended from **`veh+0x11D0`** (`puVar6` walks
  from `veh+0x11F0` with negative displacements; RE_NOTES §15's
  "from veh+0x11f0" is off by the record head) are stamped **0x26** and gated
  by bytes `+0x1350` / `+0x1351`;
* the other-car hull sets at **`veh+0x1590`** (count `+0x3A50`) and
  **`veh+0x2010`** (count `+0x3A58`) are stamped **0x21** and are only staged
  when `byte veh+0x215 == 1` — a crash-mode state.

So in normal racing, car-vs-car does **not** go through the chassis response
`FUN_0011AEF0` at all: it is the separate manager pass above.
Surface types are ASCII (`0x20 = ' '`, `0x21 = '!'`, `0x26 = '&'`), which is
why the gather callback's skip set is `0x20/0x22/0x23/0x24`.

---

## 8. Constants (all read out of `build/burnout3.elf`)

| address | value | use |
|---|---|---|
| `0x0038994C` | 2.23693633 | m/s → mph for the impact/slam tests |
| `0x003EBE3C` | 0.1 | restitution, racer vs racer |
| `0x004A1D98` | 0.0 | restitution, car vs wreck |
| `0x0041A4D0` | 20.0 | shove force coefficient |
| `0x003EBE70` | 2000.0 | shove mass clamp |
| `0x003EBE74` | 0.1 | impact scale (× 0.5 from `0x003B1684`) |
| `0x003EBE4C` | 150.0 | crash closing speed (mph) |
| `0x003EBE60` | 20.0 | rear-end minimum (mph) |
| `0x003EBE68` | 50.0 | rear-end strength range |
| `0x003EBE5C` | 30.0 | side minimum (mph) |
| `0x003EBE64` | 20.0 | side strength range |
| `0x003EBE80` | 0.3 | light/heavy slam split |
| `0x0041A4C4/C8/CC` | 35 / 20 / 40 | side gate: hi mph, lo mph, angle |
| `0x003B1B68` | 17.8815994 | attacker Δspeed (= 40 mph) |
| `0x003EBE50` | 5000.0 | wreck-path crash impulse |
| `0x003EBE54` | 2500.0 | …when traffic hits a racer |
| `0x003EBE7C` | 1.5 | wreck-path re-crash wait (s) |
| `0x0041A4C0` | −0.9 | normal ↔ vrel blend (wreck path) |
| `0x003B16C0` | −1.0 | immovable-side impulse scale |
| `0x00384208` | 1.52587891e-05 | rear-end clamp epsilon |
| `0x003EBF68` | 2.0 | `FUN_001205E0` yaw-lock rate |
| `0x003B191C` | 2.3283064e-10 | degenerate-vector epsilon |
| `0x0039AACC` | 1e-07 | clip-point dedup |
| `0x003A2928` | 100.0 | penetration ray length |
| `0x003B188C` | 1e-04 | ray parallel-plane epsilon |
| `0x003EBE40` | 0.1 | contact-point y bias |
| `0x004A52B3` | 0 | OBB-path selector (off in retail) |

---

## 9. Ported + verified

`src/burnout3_carcol.c` / `.h`:

| C entry point | real function | mark |
|---|---|---|
| `b3_carcol_hull_load` / `_from_record` | `FUN_00122C20` layout | [C] |
| `b3_carcol_world_aabb` | `FUN_00114270` | [C] |
| `b3_carcol_aabb_overlap` / `_broadphase` | `FUN_00110AF0` predicate | [C] |
| `b3_carcol_contact` | `FUN_0010A9D0` → `FUN_0010AC20` + 9 helpers | [C] |
| `b3_carcol_point_velocity` | `FUN_001066A0` | [C] |
| `b3_carcol_mutual_impulse` | `FUN_0010F8D0` | [C] |
| `b3_carcol_apply_force` | `FUN_001205E0` (+`FUN_001064B0`/`FUN_00106590`) | [C] |
| `b3_carcol_resolve_alive` | `FUN_001121F0` | [C] |
| `b3_carcol_resolve_wreck` | `FUN_00113960` | [C] |
| `b3_carcol_resolve` | `FUN_00111CD0`'s car/car ordering + dispatch | [C] |
| `b3_carcol_hull_from_extents` | — | **GLUE** fallback |

`tools/validate_carcol.py` sections and counts:

| section | function(s) | asserts |
|---|---|---|
| narrow phase | `FUN_0010A9D0`/`FUN_0010AC20` | 36 |
| broad phase | `FUN_00114270` | 6 |
| impulse | `FUN_0010F8D0` | 4 |
| force routing | `FUN_001205E0` | 8 |
| racer vs racer | `FUN_001121F0` | 338 |
| car vs wreck | `FUN_00113960` | 338 |
| **total** | | **730** |

The response sections cover: rub (type 1), light and full side slams
(3 / 5), light and full rear-end slams (4 / 6) with the attacker on both
sides, the `> 150 mph` double-crash trigger, `+0x212` on either car, a
drifting attacker, a heavy-vs-light mass split, and the wreck path's 5000
threshold. The vtable `+0x64` arguments and the `FUN_0010DCA0` calls are
captured with Unicorn code hooks, so the classification is checked against the
real calls and not just against the port's own bookkeeping.

## 10. Open [?] / not ported

* `FUN_00114F30` — the `veh+0x212` refinement of the wreck-path separation
  (projects the displacement onto `veh+0x170`). Mapped, unported.
* `FUN_00112E70` (car vs type-3 object) and `FUN_00113890` (type-5). Type 3
  is something a car can land on — the `veh+0x211` "landed on a car" tail in
  `FUN_0011AEF0` belongs to that family.
* `FUN_000FF160`'s exact `rsqrtss` result: the side-impact angle is computed
  with the approximate reciprocal-square-root instruction, so that gate is
  hardware-approximate by construction. The port uses exact `acos`; the
  differential cases pass because they sit well away from the threshold.
* The `veh+0x2424` "linked vehicle" branches (articulated trucks) in both
  response functions: mapped, not ported (no articulated vehicle is in play).
* `DAT_004A52B3` — who sets it, and therefore when the OBB path
  (`FUN_00108EF0`) is ever taken. Retail default 0.
* The producer side of the slam report (what the game-context virtual `+0x64`
  does before `FUN_001989A0`) — RE_GAMEPLAY §8 already lists the slam boost
  transfer as [S].
