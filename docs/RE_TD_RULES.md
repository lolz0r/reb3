# Takedown trigger rules — slam → out-of-control → crash → attribution → commit

Recovered 2026-08-11 from the analysed `burnout3.elf` (corrected VAs;
`.text` = old flat address + 0x10000).

Evidence marks:

* **[C]** — the REAL x86 executed under Unicorn (`tools/emulate_td_rules.py`)
  with a green differential case in `tools/validate_td_rules.py` (**315/315**),
  or bytes read out of the retail image and asserted there.
* **[C-disasm]** — read out of the instruction bytes at the given address, no
  execution case.
* **[S]** — read from the decompile, self-consistent, no green case.
* **[?]** — open.
* **GLUE** — this harness's own, not from the binary.

Ported module: `src/burnout3_td_rules.c/.h`.
Oracle: `tools/emulate_td_rules.py`.  Acceptance: `tools/validate_td_rules.py`.

---

## 0. The headline: **a slam never wrecks anybody** [C]

The mission premise was that a "full slam" wrecks the victim. It does not.

`FUN_001989A0`, the slam handler behind the game-context virtual `+0x64`,
contains **no call to the crash entry** `FUN_0010DCA0` / `FUN_0010DD20`.
Two independent proofs, both asserted:

1. **Executed**: `tools/emulate_td_rules.py` installs a recording stub on
   `FUN_0010DCA0` and runs `FUN_001989A0` (and the whole `FUN_00029F30`
   dispatch above it) over 10 seedings — full side slam, full rear-end slam,
   a slam on an already-crashed victim, a slam on a slow victim. The stub is
   never reached (validate §1, §2).
2. **Static**: every `E8` relative call inside `FUN_001989A0`'s 0x520 bytes is
   decoded and neither `0x0010DCA0` nor `0x0010DD20` appears (validate §11).

What a full slam actually does to the victim is stamp its **out-of-control
clock**:

```
racecar+0x1598 = clock        the steer-away / OOC timestamp
racecar+0x159C = slam type    0 = side, 1 = rear-end
racecar+0x16BC = attacker     "last aggressive contact"
racecar+0x16C0 = clock
racecar+0x1590 += 1           times slammed
```

and symmetrically stamp the attacker's `+0x1174` (slams made), `+0x158C`
(last slam clock) and `+0x16BC/+0x16C0` (each car records the other).
`racecar+0x1598` is exactly the input to the already-verified steer-away
envelope `FUN_0011ECF0` and the AI authority write `FUN_00105340`
(docs/RE_GAMEPLAY.md §7). So the victim loses the wheel for **Total
Out-Of-Control Time = 1.0 s** — and then either crashes on its own through
the normal crash triggers, in which case the crash is attributed back to the
slammer, or it recovers, in which case the slammer gets **TAKEDOWN DENIED**.

---

## 1. The chain at a glance

```
FUN_001121F0 / FUN_0011AEF0        classify the contact  (docs/RE_CARCOL.md §5)
  └─ game-context vtable +0x64 = FUN_00029F30       [C-disasm 0x00029F30]
       switch (kind - 1) via the jump table at 0x0002A01C:
         1 rub          -> FUN_001979E0   contact stamps only
         2 wall shunt   -> FUN_00197EA0   aggressor stamp, both cars >= 40 mph
         3 side  light  -> FUN_00197D20(flag 0)  light-slam BP, NO OOC stamp
         4 rear  light  -> FUN_00197D20(flag 1)
         5 side  full   -> FUN_00197BE0(type 0) -> FUN_001989A0
         6 rear  full   -> FUN_00197BE0(type 1) -> FUN_001989A0

... the victim is now out of control for 1.0 s and may crash ...

any crash trigger  (FUN_0011AEF0 wall, FUN_001121F0's >150 mph, FUN_00113960's
                    impulse, FUN_0011BE50's roll-over, FUN_00197260 grinding)
  └─ FUN_0010DCA0 -> FUN_0010DD20                    EAX = the CAUSE RECORD
       └─ game-context vtable +0x48 = FUN_00027CC0 / FUN_00024940 / FUN_0002BFE0
            └─ FUN_00197750(record, score)   stores the record at score+0x308
                 └─ FUN_00197430             the ATTRIBUTION stamps

per frame FUN_001935F0:
  FUN_00199080  double/spree window expiry
  FUN_00195CE0  TAKEDOWN DENIED (0x35) / LUCKY ESCAPE (0x36)
  FUN_00197040  the claim scan: commit after Race Car Clear Wait
       └─ FUN_00198E60  commit  -> FUN_001994D0  BP + message selection
                                -> vtable +0x5C
```

Correction to docs/RE_GAMEPLAY.md §6, which described the slam entry as
"FUN_001989A0(victim_pv, attacker_pv, impact, type)" without the dispatcher:
**only kinds 5 and 6 reach it.** Kinds 3/4 (the LIGHT slams that
`FUN_001121F0` reports when the strength scale `s <= 0.3`) go to
`FUN_00197D20` and never stamp `+0x1598`, so a light slam does *not* put the
victim out of control and can never lead to a takedown on its own.

---

## 2. `FUN_00029F30` — the game-context `+0x64` entry [C-disasm]

Signature `(kind, attacker_pv, victim_pv, strength)`, `ret 0x10`; vtable slot
index 25. The vtables that carry it are `0x003A9280`, `0x003A9528`,
`0x003A98A0`, `0x003A9EA0` (asserted byte-for-byte, validate §11). The other
family (`0x003A93E8`, `0x003A9170`, `0x003A9650`, `0x003A9C40`) has
`FUN_00026BA0` there — `mov al,1; ret 0x10`, i.e. slams are inert in those
modes.

Handler arguments (all recovered from the dispatcher's register set-up):

| kind | callee | convention |
|---|---|---|
| 1 | `FUN_001979E0` ×2 | EAX = *one* car's score, ECX = the other's racecar |
| 2 | `FUN_00197EA0` | EAX = victim racecar, ECX = attacker racecar |
| 3/4 | `FUN_00197D20` | EAX = attacker pv, EBX = victim pv, stack (strength, flag) |
| 5/6 | `FUN_00197BE0` | EAX = type byte, ECX = attacker pv, EDI = victim pv, stack (strength) |
| — | `FUN_001989A0` | EAX = victim pv, stack (attacker pv, strength, type) |

### 2.1 `FUN_001979E0` — the rub [C-disasm]

```
if (score+0x27C != 3 && !racecar+0x18FA) {
    score+0x55E[other grid] = 1 ;  score+0x510[other grid] = clock
}
```
The *accumulated* per-opponent contact timer at `score+0x528[]` that the
attribution reads is grown by the rubbing pass `FUN_00194A80` **[S]**.

### 2.2 `FUN_00197EA0` — "shunted into the scenery by car N" [C]

```
require attacker+0x134C != 3 and victim+0x134C != 3
require attacker pv+0xBC * 2.2369363 >= 40.0        (both cars)
require victim   pv+0xBC * 2.2369363 >= 40.0
attacker+0x16B4 = 1 ; attacker+0x16B0 = victim racecar
victim+0x16BC   = attacker racecar ; victim+0x16C0 = victim clock
return 1
```
Note it stamps the **aggressor** but not `+0x1598`: the victim becomes
attributable for the next `Maximum Crash Wait Time` without losing steering.
This is the mechanism behind pushing a rival into a barrier.
The producer is `FUN_0011AEF0` @`0x0011BAB8` (`push 1.0; push edi; push eax;
push 2; call [edx+0x64]`), gated by `veh+0x1650` (the slot of the car in
contact) and the flags `racecar[slot]+0x1628[..]` / `+0x162E[..]`. 5 executed
cases (validate §4).

### 2.3 `FUN_00197D20` — light slams [C-disasm, gate mirrored]

```
if (attacker+0x134C == 3 || victim+0x134C == 3) return 0
va = (victim+0x16C0 != -1 && clock <= victim+0x16C0 + 1.0) ? victim+0x16BC : 0
if (va == attacker) {                              0x00197D9x
    if (0 <= attacker+0x1690 && clock <= +0x1690 + 1.0) return 0
    if (FUN_001981D0(attacker score, 1.0)) return 0        # +0x1690 window
}
if (!(attacker+0x158C < 0 || attacker+0x158C + 1.0 < clock)) return 0
if (FUN_00198190(attacker score, 1.0)) return 0            # +0x1598 window
FUN_001987A0(attacker pv, victim pv, strength, flag)       # the BP [S]
return 1
```
`FUN_00198190`/`FUN_001981D0` are three-line predicates ("is this car still
inside its own out-of-control window on `+0x1598` / `+0x1690`"); the oracle
executes them for real rather than stubbing them.

### 2.4 `FUN_00197BE0` — the full-slam gate [C, 7 cases]

```
if (attacker+0x134C == 3 || victim+0x134C == 3) return 0
aa = (attacker+0x16C0 != -1 && clock <= attacker+0x16C0 + 1.0) ? attacker+0x16BC : 0
va = (victim  +0x16C0 != -1 && clock <= victim  +0x16C0 + 1.0) ? victim  +0x16BC : 0
if (va == attacker racecar || aa == victim racecar) return 0     # 0x00197C92
FUN_001989A0(victim pv, attacker pv, strength, type)
return 1
```

i.e. a **1.0 s mutual cooldown**: two cars that already traded an aggressive
contact within the last second cannot slam each other again. This is the
gate that stops the shove of RE_CARCOL §4.8 producing a slam every frame
while the two cars are locked together. The `1.0` is `0x003B168C`.

The return value is what `FUN_001121F0` latches into `pair+0x2D` and what
selects `victim+0x153C` (the "which side was I hit on" byte).

---

## 3. The crash-cause record [C-disasm]

A 16-byte stack record built by whoever triggers the crash and handed to
`FUN_0010DCA0` **in EAX** (the wrapper moves it into `FUN_0010DD20`'s EDI at
`0x0010DCEA`/`0x0010DD2E`). `FUN_0010DD20` @`0x0010DD71` passes it to the
game-context virtual `+0x48`, whose race-mode implementations
(`FUN_00027CC0` / `FUN_00024940` / `FUN_0002BFE0`) call
`FUN_00197750(record, score)`, which copies it to **`score+0x308` =
`racecar+0x13D8`** (`0x00197784`).

| offset | meaning | writer |
|---|---|---|
| `+0x00` u8 | non-zero ⇒ **wall / scenery** | `FUN_0011AEF0` @`0x0011B9B3` writes 1 |
| `+0x01` u8 | non-zero ⇒ `+0x08` holds a hit object | `FUN_00112E70` @`0x00113556` writes 1 |
| `+0x04` u32 | surface id, `(u16 obj+0x190) & 0xFF` | `FUN_0011AEF0` @`0x0011B9C4` |
| `+0x08` ptr | the object hit; its **class byte is `obj+0x173`** | `FUN_00112E70` @`0x00113560` |
| `+0x0C` ptr | a racecar — **the WRECK that hit me** | `FUN_00113960` @`0x0011411C` |

Car-vs-car crashes pass **EAX = 0**: `FUN_001121F0` @`0x00112881` and
`0x00112959`, `FUN_0011BE50` @`0x0011C27F`, `FUN_0017C170` @`0x0017C4B6`,
`FUN_00197260` @`0x00197396`, `FUN_00024F10` @`0x00024F81` all do
`xor esi,esi` first. No record ⇒ the plain `TAKEDOWN!` message.

`FUN_0011AEF0` also writes `racecar+0x15CC = (u16 obj+0x190) >> 15` at
`0x0011B9D7`; `FUN_001994D0` @`0x001995E5` reads it as the gate on the
**signature wall takedown** table at `DAT_003A4BC8`.

---

## 4. `FUN_00197430` — the attribution stamps [C, 8 scenarios]

ESI = the crashing car's score object, the stack arg is the cause record.
`0x003B1944` = **25600.0 = (160 m)²**.

```
score+0x50C = 0 ; score+0x506 = 0 ; score+0x507 = 0
for i in 0 .. DAT_0073A19C-1:
    rc = DAT_0073A1A8[i]
    if (  score+0x528[i] > "Min Collide Time to enable TD" (0x003F740C = 0.1)
       || (score+0x5B8 != 0 && rc+0x1684 == my grid slot) )        # psyche-out
       && |rc.pos - my.pos|^2 < 25600
       && rc+0x134C != 3
       && rc->self(+0x1198)+0x18FA == 0                            # not crashed
    then rc+0x15A8[my grid] = rc+0x10DC                            # THE CLAIM
         rc+0x15C0[my grid] = 0                                    # aftertouch
         rc+0x15C6[my grid] = score+0x5B8                          # psyche-out

# the car that slammed me, inside Maximum Crash Wait Time (0x003F7404 = 2.0)
if (score+0x5F0 != -1 && clock <= score+0x5F0 + MaxCrashWait && score+0x5EC)
   && within 160 m && not finished && not crashed:
       att+0x15D8 = my racecar
       att+0x15A8[my grid] = att+0x10DC ; +0x15C0 = 0 ; +0x15C6 = 0

# the cause record's +0x0C -- AFTERTOUCH
if (record && (w = record+0x0C) && w+0x18FA && w+0x1920 == 0
    && DAT_00667E90[w+0x27D0 * 0x4AD0 + 0x4AC5] && w+0x134C != 3):
       w+0x15A8[my grid] = w+0x10DC
       w+0x15C0[my grid] = 1          # <- the AFTERTOUCH flag
       w+0x15C6[my grid] = 0
```

`score+0x4D8[]` ≡ `racecar+0x15A8[]`, `score+0x4F0[]` ≡ `racecar+0x15C0[]`,
`score+0x4F6[]` ≡ `racecar+0x15C6[]` (score = racecar + 0x10D0).

**Correction worth recording:** the first clause reads the **crashing car's
own** `score+0x528[i]` array (`local_18 = score+0x528` walking with the loop),
not the opponent's. The first draft of the port had it the other way round and
the differential case caught it.

### 4.1 AFTERTOUCH TAKEDOWN — the detection [C-disasm + C case]

`FUN_00113960` (car vs crashed car, RE_CARCOL §6) zeroes a 16-byte record at
`[esp+0x50]` (`0x001140DA`) and then, when the other object is a car
(`FUN_0010C550`), stores **the already-crashed car's racecar** into `+0x0C`
(`0x0011411C`) before crashing the live one at `0x001141EB`. So: your wreck
hits a rival hard enough (`j > 5000`, or `> 2500` for traffic) → the rival
crashes → the record names you → `FUN_00197430`'s third block gives you a
claim with the aftertouch flag → the commit pays **Aftertouch Takedown BP
(1250)** and message `0xAA + min(count, 4)` from `DAT_003A4B38`.

Gates on that block: you must be crashed (`+0x18FA`), class 0 (a human) and
the per-player aftertouch-enable byte at `DAT_00667E90 + player*0x4AD0 +
0x4AC5` must be set **[S — this harness treats it as enabled]**.

---

## 5. `FUN_00197040` — the claim scan [C, 4 cases]

Per frame, from `FUN_001935F0`. `param_1` = my score object.

```
for i in 0 .. DAT_0073A19C-1:
    t = score+0x4D8[i]
    if (t >= 0 && (t + "Race Car Clear Wait Time" (0x003F7400 = 0.5) <= clock
                   || score+0x4F0[i]))                 # force byte
        if (!my racecar+0x18FA || score+0x4F0[i])
            victim = DAT_0073A1A8[i]                   # 0x739CD0 + 0x4D8 + 4i
            FUN_00198E60()                             # ESI = score, EDI = victim
        score+0x4D8[i] = -1 ; score+0x4F0[i] = 0 ; score+0x4F6[i] = 0
if (score+0x4D4 && !score+0x4D4->+0x18FA) score+0x4D4 = 0
```

So the deferral is: the crash is attributed immediately, but the takedown is
only *credited* 0.5 s later and **only if the attacker has not crashed in the
meantime**. Crashing inside that window silently drops the claim.

---

## 6. `FUN_00198E60` — the commit [C, 4 cases]

ESI = attacker score, EDI = victim racecar.

```
cause = (victim+0x18FA) ? victim+0x13D8 : NULL          # 0x00198E6D
if (victim+0x15D6) return 0                             # dedup
victim+0x15DC = attacker racecar ; victim+0x15D6 = 1 ; victim+0x15D7 = 1
score+0x500 = clock ; score+0x68 += 1 ; score+0x4D4 = victim racecar
[medal-threshold walk over score+0x80 -> score+0x74]                     [S]
FUN_001994D0(score+0x124, victim, score+0x4F6[vg], score+0x4F0[vg])  ECX = cause
game-context vtable +0x5C (victim racecar, attacker racecar)
if (score+0x5B9[victim grid]) { clear it ; victim+0x168F = 1 }   # REVENGE
else                            victim+0x1689[my grid] = 1
if (victim+0x1920 == 1) victim+0x240C = victim clock + 5.0       # AI recovery
return 1
```

`score+0x124` is the **callout slot**; `slot+0x148` is the racecar back
pointer, so all the `slot+0x1xx` fields below are `racecar+0x12xx`.

---

## 7. `FUN_001994D0` — BP and the message [C, 27 executed cases]

Arguments `(callout slot, victim racecar, psyche byte, aftertouch byte)`,
ECX = the victim's stored cause record.

```
slot+0x09 = cause ? cause[0] : 0 ;  slot+0x0C = cause ? cause+0x08 : 0
slot+0x07 = psyche ; slot+0x08 = aftertouch
slot+0x06 = attacker racecar+0x1689[victim grid]        # revenge snapshot
msg = 0x93 (TAKEDOWN!) ; BP = Takedown BP (0x003F746C = 150)

if (!aftertouch) {
    if (cause) {
        if (!cause.wall) {
            if (cause.has_obj) switch (cause.obj+0x173) {     # 0x0019958C
                1->0x94 CAR   2->0x95 VAN     3->0x96 TRUCK  4->0x97 BIG RIG
                5->0x98 BUS   7->0x99 L-TRAIN 8->0x9A TRAM   9->0x9B MONORAIL
                11->0x9C TRAILER      (6 and 10 have no case -> stay 0x93)
                + the signature-takedown table DAT_003A4FF4/0x003A5000    [S]
            }
        } else {
            msg = 0x9D  WALL TAKEDOWN!                        # 0x001995CD
            + the signature-wall table DAT_003A4BC8, gated by victim+0x15CC
              and the track/progress range -> BP 1500 (0x5DC)             [S]
        }
    }
    if (psyche && !signature) { msg = 0xA9 PSYCHE OUT!; BP = 0x003F7474 = 150 }
    post(msg, BP)
    if (0xAF <= msg <= 0xC2) { slot+0x12C = msg-0xAF; racecar+0x1178 |= 1<<..;
                               racecar+0x1190 += 1 }
    else racecar+0x1194 += 1
    # double takedown  (0x001998AA)
    if (clock < slot+0x110 || slot+0x110 == -1) {
        n = ++slot+0x114 ; slot+0x110 = clock + 0x003F7410 (1.0)
        if (n > 1) { i = min(n-2, 4) ; BP += DAT_003F7508[i] ; post(0xA3+i, BP) }
    }
    # takedown spree   (0x0019991F)
    if (clock < slot+0x118 || slot+0x118 == -1) {
        n = ++slot+0x11C ; slot+0x118 = clock + 0x003F7414 (30.0)
        if (n > 1) { i = min(n-1, 4) ; BP += *(0x003F7514 + 4i) ;
                     post(0x9F + i-1, BP) }
    }
    # revenge          (0x00199973)
    if (slot+0x06) { BP += 0x003F7470 (350) ; ... post(0xA7) }
} else {
    msg = DAT_003A4B38[min(slot+0x128, 4)] = {0xAA,0xAB,0xAC,0xAD,0xAE}
    slot+0x128 += 1 ; racecar+0x118C += 1
    BP = Aftertouch Takedown BP (0x003F7478 = 1250) ; post(msg, BP)
}
racecar+0x111C += BP ; racecar+0x117C += BP     (gated by vtable +0x40 & 3)
```

Two details worth flagging:

* the double-takedown index is `min(n-2, 4)` against a **4**-entry table, so
  `n >= 6` reads one dword past it into `Takedown Spree BP[0] = 300`. The port
  reproduces that with an explicit 5-entry table.
* `FUN_00199350`'s accept rule is "*the message id IS the priority*" — an id
  lower than `slot+0x130` is rejected. Within one commit the last-posted
  highest id therefore wins; the port's `B3TdEvent.message` is that maximum
  and `extra_message` carries the ladder post.

---

## 8. TAKEDOWN DENIED (0x35) / LUCKY ESCAPE (0x36) [C, 9 cases]

Their trigger conditions were listed as unrecovered. They are:

**Arming — `FUN_00197920`**, reached from the game-context virtual **`+0x54`**
(`FUN_00026A70` in race mode, `FUN_000273D0` elsewhere), whose only caller in
the image is `FUN_00112E70` @`0x001134EF` — the car-vs-object contact
response. ECX = my score, EDX = the object I touched.

```
if (score+0x27C == 3) return                       # race over
if (my racecar+0x18FA) return                      # I am crashed
if (score+0x5F0 != -1                              # I was slammed  (+0x16C0)
    && clock <= score+0x5F0 + Maximum Crash Wait Time (2.0)
    && score+0x5EC != 0) {                         # ... by a real car (+0x16BC)
        score+0x5E5 = 1 ; score+0x5E6 = 1 ; score+0x5E8 = clock
}
... plus the near-miss slot bookkeeping at score+0x3E8/+0x3F0/+0x410   [S]
```

**Award — `FUN_00195CE0`**, per frame from `FUN_001935F0`, EDI = my score:

```
if (!score+0x5E5) return
if (score+0x5F0 == -1) return
if (clock > score+0x5F0 + Maximum Crash Wait Time) return
attacker = score+0x5EC ; if (!attacker) return
if (score+0x5E6) { FUN_00199CA0(ESI = score+0x124)          -> msg 0x36 LUCKY
                   BP += Denied (You Were Lucky) 0x003F7488 = 15 }
FUN_00199BE0(ESI = attacker racecar + 0x11F4)                -> msg 0x35 DENIED
BP += Denied (Takedown Denied) 0x003F748C = 10
score+0x5E5 = 0 ; score+0x5E6 = 0
```

Note the two different callout slots: **LUCKY ESCAPE! posts to the survivor's
own slot, TAKEDOWN DENIED! posts to the attacker's** (`attacker racecar +
0x11F4` *is* `attacker score + 0x124`). Both BP awards land in the survivor's
totals (`score+0x4C` / `+0xB0`). `FUN_00199CA0`/`FUN_00199BE0` are inline
copies of `FUN_00199350`'s accept test — they write the id straight into
`slot+0x130` rather than calling it, which is why the oracle reads the message
back from there.

So in plain terms: **you survive a slam, and then touch something inside the
2-second Maximum Crash Wait window without crashing → the slammer is told
"TAKEDOWN DENIED!" and you are told "LUCKY ESCAPE!".** Nothing arms them if
you are never touched again inside the window.

---

## 9. Control handover — verified [C-disasm, asserted in validate §11]

The orchestrator's assumption is correct.

| address | instruction | meaning |
|---|---|---|
| `0x0018CB60` | `cmp cl,[eax+0x27D8]`; equal ⇒ `ret` | `FUN_0018CB60` is edge-triggered |
| **`0x0018CB6A`** | `mov [eax+0x27D8], cl` | **the set/clear site** |
| `0x0018CB75` | `movaps [eax+0x19D0], 0` then `jmp FUN_00179760` with EAX = racecar+0x1A00 | rising edge: navigator reset |
| **`0x0018CB94`** | `movss [ecx+0x1534], 1.0` (`0x003B168C`), ECX = `racecar+0x2440` | **falling edge restores steering authority** |
| `0x0018CBA2` | `if (pv+0x1524 == 4) = 0` | clears the cinematic drift state |
| `0x0018C53A` | `mov al,[esi+0x27D8]` | the consumer reads it… |
| `0x0018C54A` | `call FUN_00170820` | …and routes to the route-following **auto-driver** |
| `0x000279CE` | `mov cl,[eax+0x27D8]` | `FUN_000279C0` (cinematic exit) |
| **`0x000279F0`** | `mov [eax+0x27D8], bl` (bl = 0) | **the exit clear** |
| **`0x000279F6`** | `movss [ecx+0x1534], 1.0` | the exit's authority restore |
| `0x00027A0D` / `0x00027A2F` | `+0x1524 == 4 -> 0` | attacker's, then the victim's |

So `racecar+0x27D8` really is "the AI has the wheel", set/cleared at exactly
those two addresses, and the release really does put `pv+0x1534` back to 1.0.

---

## 10. The takedown-cinematic camera message — partial [?]

`docs/RE_TAKEDOWN_FX.md` §8.4 left the *sender* of the camera message open.
This session narrowed it, with the correct `program=burnout3.elf` parameter:

* the 23-message dispatcher at `0x001684AF` lives inside the function starting
  at **`0x00168380`** (Ghidra has no symbol there; the first switch is on the
  message *class* `[esp+4] ∈ 0..5` through the jump table at `0x001689D4`,
  then on the message *id* `[esp+0x1C] ∈ 0..0x16` through `0x001689EC`);
* its only data reference is **`0x003A9398`**, i.e. it is **slot 0 of the
  vtable at `0x003A9398`** — a listener interface;
* that vtable is installed at **`0x0001592D`** into a composite camera object
  at **`+0x158`** (`mov [eax+0x158], 0x3A9398`), alongside the camera-mode
  vtables of RE_TAKEDOWN_FX §8.3 at `+0x70`, `+0x100`, `+0x120`, `+0x138`,
  `+0x190`, `+0x1A8`, `+0x1C8`, `+0x1E0`.

**Still [?]:** what broadcasts to that listener. Neither `FUN_00198E60`,
`FUN_000273F0` nor `FUN_000278B0` reaches it — the commit chain's only
outward virtual is `+0x5C` on the game context (`0x00198F25`), and the
cinematic's own posts go through `DAT_004CFB20[i]->vtable[+0x0C]`, which
RE_TAKEDOWN_FX §4 already showed is inert. The sender is therefore **not** in
the commit chain, contrary to the hypothesis; it is somewhere in the
presentation-state broadcast's listener list (`FUN_00053D20`, §4.6), whose
installer is still unfound.

---

## 11. What the port implements

`src/burnout3_td_rules.c` / `.h`:

| C entry point | real function(s) | mark |
|---|---|---|
| `b3_td_slam_report` | `FUN_00029F30` dispatch | [C] |
| ↳ kind 1 | `FUN_001979E0` | [C-disasm] |
| ↳ kind 2 | `FUN_00197EA0` | [C] |
| ↳ kind 3/4 | `FUN_00197D20` gate (+`FUN_00198190`/`FUN_001981D0`) | [C-disasm], BP [S] |
| ↳ kind 5/6 | `FUN_00197BE0` → `FUN_001989A0` stamps | [C] |
| `b3_td_contact` | `FUN_001979E0` + `score+0x528` accumulation | [C-disasm] / [S] |
| `b3_td_cause_*` | the 4 cause-record producers | [C-disasm] |
| `b3_td_on_crash` | `FUN_00197750` + `FUN_00197430` | [C] |
| `b3_td_contact_notify` | `FUN_00197920` | [C] |
| `b3_td_frame` | `FUN_00199080` + `FUN_00195CE0` + `FUN_00197040` + `FUN_00198E60` + `FUN_001994D0` | [C] |
| `b3_td_select_message` | `FUN_001994D0`'s chooser | [C] |
| `b3_td_out_of_control` | `FUN_0011ECF0` / `FUN_00105340`'s predicate | [C] |

**Not ported (deliberately, all marked in the source):** the slam boost/BP
transfer (`FUN_001989A0`'s middle — already [S] in RE_GAMEPLAY §8), the
light-slam BP (`FUN_001987A0`), the signature-takedown tables
(`DAT_003A4FF4`, `DAT_003A4BC8`), the medal-threshold walk in the commit, and
the near-miss slot bookkeeping inside `FUN_00197920` — those belong to the
score module.

## 12. THE CRASH-THRESHOLD AUTHORITY `veh+0x1534`, and the wall trigger it scales (2026-08-12)

Recovered while checking collision parity end to end. This section answers
two complaints about the harness: takedowns were harder to land than retail,
and accidental obstacle hits were too forgiving. Both trace to the same
missing quantity.

### 12.1 The trigger the harness was not running

`FUN_0011AEF0` @`0x0011B909`..`0x0011B9F3` is the ONLY wall-crash decision in
the game. It is not a closing-speed test:

```
dv     = |j| / mass        j = FUN_00106720(nh, contact centroid,
                               POINT velocity, restitution 0)      ESP+0x1c
headon = |dot(n_flat, at)|                                         ESP+0x14

crash party (FUN_00017310):
        dv > authority*10.0   [0x003A7F34]  &&  headon > authority*0.303 [0x0039B308]
normal race:
        surface_lo(+0x190) != 0x20  &&  !(veh+0x1353 & 8)
     && dv > authority*27.5  [0x0039B2FC]  &&  headon > authority*0.707 [0x003B1A20]
both:   racecar class (+0x1920) != 2                               @0x0011B998
```

Two things about `dv` matter and neither survives a centre-of-mass closing
speed:

* it is measured **at the contact point**, so `omega x r` is in it. A car
  spun out by a slam whose tail swings into a barrier has a large `dv` with
  almost no chassis velocity along the normal. The harness's `vin =
  -(vel . n)` could not see that case at all -- which is exactly the case a
  slam creates.
* it is measured **after** the two velocity scrubs the same function applies
  first (head-on `1 - (headon - 0.707)*0.1`, then the surface scrub), so a
  dead head-on hit reports `0.961 x` the closing speed, not `1.0 x`.

`b3_crash_wall_eval` (burnout3_crash.h) is this tail, factored out of the
ported `FUN_0011AEF0` so the two share one body of code; it takes a contact
point and normal from whatever the caller's world collision is and returns
the retail verdict.

### 12.2 `authority` -- the whole dynamic range

At `authority` 1.0 a barrier needs a 27.5 m/s head-on delta-v within 45
degrees. That is why you can grind walls for a lap in the retail game and
never wreck. At 0.05 it needs 1.375 m/s at any angle inside 88 degrees --
the next thing the car touches wrecks it. Everything interesting is in this
one float. Two writers, in this per-frame order:

```
FUN_00104A90 @0x00104A9C -> FUN_00105BD0    (a) the base value
FUN_00104A90 @0x00104C42 -> FUN_0011BE50
                              -> FUN_00104D30 @0x00104D8D -> FUN_00105340   (b) the override
                              -> stage 6 FUN_0011AEF0    reads +0x1534
```

**(a) `FUN_00105BD0` -- a VIEW-DISTANCE ladder.** [C, executed]

The human's car takes an early-out @`0x00105D6E` (`racecar+0x1920 == 0` and
`racecar+0x27D9 == 0`) that leaves the ladder fraction at 1.0, so the player
runs at `authority` 1.0 -- the same value `FUN_0018CB60` @`0x0018CB94` and
`FUN_000279C0` @`0x000279F6` write when a human takes the wheel.

Every other car goes through `FUN_00105FC0` on `d2`, the squared distance to
the nearest racecar in `DAT_0073A1D0[0 .. DAT_0073A1C0-1]` that passes the
`FUN_001AD4A0` visibility test. `DAT_0073A1C0` is the LOCAL PLAYER count
(the same global `single_player` reads), so in single player `d2` is the
squared distance to the player's car. `base` is `19600` or `15625` on
`veh+0x1550` [`0x0039A850` / `0x0039A854`; the runtime-tunable copies
`DAT_005A39E0` / `DAT_005A39FC` are BSS but their reset thunks @`0x002B8D80`
and @`0x002B8DA0` seed them from those same two image constants, so the
retail defaults ARE in the image]. Band edges are
`base * DAT_0039A858[1..5]` = `{0.0, 0.1, 0.4, 0.5, 1.0}`:

| `d2` | distance at base 15625 | `authority` | crash entries |
|---|---|---|---|
| `<= 0.1*base` | `<= 39.5 m` | 1.0 | on |
| `.. 0.4*base` | `.. 79.1 m`  | ramps 1.0 -> 0.03 | on |
| `.. base`     | `.. 125 m`   | 0.03 | **OFF** |
| `>  base`     | `>  125 m`   | 0.03 | **OFF** |

"OFF" is `FUN_00105BD0` @`0x00105FA0`: when the ladder's fraction is not
strictly positive it sets `veh+0x1353 |= 0x18`, and bit 3 is precisely the
flag `FUN_0011AEF0` @`0x0011B94D` and `FUN_00112E70` @`0x0011329A` test to
skip the crash. So the fragile cars are the ones in the MIDDLE band, roughly
40 m to 79 m from the player, and cars past 79 m cannot wall-crash at all.
The out struct `FUN_00105FC0` fills is zero-initialised
@`0x00105BFB`..`0x00105C1C`, which is what makes the ladder monotone (the
`sel >= 2` path never writes `out[1]`).

`racecar+0x1688` (an armed psyche-out) forces the value to the 0.03 floor
@`0x00105F63` and forces the crash entries back ON @`0x00105EA6`.

**(b) `FUN_00105340` -- the SLAM OVERRIDE.** [C-disasm]

@`0x0010563C` (normal race, stores `[0x003A69BC]` = **0.05**) and
@`0x0010574A` (game context `+0x23F8 == 2`, stores `[0x003A69C4]` = **0.1**).
Both blocks are the same six-condition test; each condition jumps to the
common bail at `0x001057BA`, so any failure leaves the ladder's value alone:

| address | test | meaning |
|---|---|---|
| `0x0010563C` | `COMISS 0.0, +0x1598` `JA` | the car has been slammed at all |
| `0x0010565F` | `COMISS now, +0x1598 + v+0x13E4` `JA` | still inside Total Out-Of-Control |
| `0x00105679` | `UCOMISS +0x16C0, -1.0` `JNP` | an aggressive contact is recorded |
| `0x0010569A` | `COMISS now, +0x16C0 + v+0x13E4` `JA` | and it is recent too |
| `0x001056AB` | `TEST attacker, attacker` `JZ` | the aggressor is still known |
| `0x001056B1` | `CMP attacker+0x1920, 0` `JNZ` | **the aggressor is the HUMAN** |

The last condition is the design, not an accident: the override exists so
the PLAYER's slams convert. An AI that slams another AI does not get it --
AI-vs-AI fragility comes from (a)'s distance ladder instead. `v+0x13E4` is
the per-car vdb "Total Out-Of-Control Time (s)" (config `+0x1C0`,
`FUN_00134710` @`0x00134A88`), retail 1.0.

Ported: `b3_td_crash_authority_full` / `b3_td_crash_authority`.

[S] on two points -- the OOC stamp is read through `*(racecar+0x1198)` while
the aggressor stamps come off the racecar itself (this harness carries one
`B3TdCar` per car and reads both off it), and `veh+0x1553`, a one-shot that
can defer the `0x18` store by a frame, is taken at its steady-state 0.

### 12.3 The object/prop path `FUN_00112E70` -- same scale, different units [C-disasm]

@`0x001132A7`..`0x001133E4`, for completeness; **not ported**, the harness
has no prop entities (its world collision is one triangle soup):

```
require  the object is crashable, !(veh+0x1353 & 0x10), X > veh+0x152C
closing_mph = |v_rel| * 2.2369363          [0x0038994C]
crash party: closing_mph > authority * 20.0   [0x003EBE48]
normal race: closing_mph > authority * 75.0   [0x003EBE44]
    -> the push-out is skipped and FUN_0010DCA0 is called @0x0011357E
damage record: obj+0x20 = mass * 2.0 * closing_mph * 0.1 * 0.5
```

So at full authority a prop wrecks you above **75 mph**, and a car that was
just slammed wrecks on one at 3.75 mph. This is the everyday "I clipped
something" crash and the harness cannot currently produce it.

### 12.4 CORRECTION -- the `-0.7071` gate is a LAUNCH selector, not a crash gate

The harness gated its wall crash on `dot(at, n) < -0.7071`, citing
`FUN_0010DD20`. The constant is real (`[0x003B1DE0]` = `-0.707106769`) and
the dot product is real, but it does not decide whether a crash happens:

```
0010e3d3  CALL FUN_0010E510            build the contact vector from veh+0x160
0010e3e5  CALL FUN_00013C60            XMM0 = dot(that, frame.at)
0010e3ea  MOVSS XMM1, [0x003B1DE0]     -0.70710677
0010e3f2  COMISS XMM1, XMM0
0010e3f5  JBE   0x0010E42A             ---> skips ONLY the two FUN_00125100 calls
0010e3f7..0010e40f                     the crash-entry LAUNCH KICK pair
0010e42a  *(rec+0x114) = 0xB           the crash proceeds either way
```

It selects the violent head-on **launch** (the pair of `FUN_00125100` kicks
that throw the car into the air) inside the already-committed wall crash.
`FUN_0010DCA0` itself has no magnitude test at all -- it resolves the game
mode and the racecar slot and always calls `FUN_0010DD20`, whose only refusal
is `if (*(rec+0x130) > 0.0) return`, i.e. do not re-enter while the previous
crash timer is still running. So the caller's decision is final, and for
walls the caller is `FUN_0011AEF0`'s test in 12.1.

Retail's head-on requirement is `headon > authority*0.707` inside 12.1 and it
is **scaled, not skipped**, when a car is out of control. The harness applied
a hard `0.7071` to every non-OOC hit and none at all to an OOC one, which is
neither.

### 12.5 Full-chain parity table

Every value re-read at its VA from `build/burnout3.elf` for this pass.

**Wall crash -- `FUN_0011AEF0`**

| gate | VA | retail | ours | verdict |
|---|---|---|---|---|
| dv scale, normal race | `0x0039B2FC` | 27.5 | 27.5 | OK |
| head-on scale, normal race | `0x003B1A20` | 0.707 | 0.707 | OK |
| dv scale, crash party | `0x003A7F34` | 10.0 | 10.0 | OK |
| head-on scale, crash party | `0x0039B308` | 0.303 | 0.303 | OK |
| dv floor before the impact block | `0x003B16E0` | 0.0 | 0.0 | OK |
| impact speed cap | `0x003B1E18` | 89.408 | 89.408 | OK |
| impact 1/cap | `0x003B1E14` | 0.011184681 | 0.011184681 | OK |
| impact head-on weight | `0x003B18B8` | 1.75 | 1.75 | OK |
| impact head-on clamp | `0x003A69C0` | 0.9 | 0.9 | OK |
| impact scale | `0x003B1A68` | 0.175 | 0.175 | OK |
| surface skip | `veh+0x190 & 0xFF == 0x20` | skip | same | OK |
| flag skip | `veh+0x1353 & 8` | skip | same | OK |
| class skip | `racecar+0x1920 == 2` | skip | same | OK |
| the magnitude the gate reads | contact-point `dv` | **was CoM `vin`** | **FIXED** |
| the head-on gate's scope | scaled by authority, always | **was hard 0.7071, non-OOC only** | **FIXED** |

**Authority -- `FUN_00105BD0` / `FUN_00105FC0` / `FUN_00105340`**

| value | VA | retail | ours | verdict |
|---|---|---|---|---|
| base radius^2 | `0x0039A854` | 15625 | 15625 | OK |
| alt radius^2 | `0x0039A850` | 19600 | 19600 | OK |
| ladder bands | `0x0039A858[1..5]` | 0, 0.1, 0.4, 0.5, 1.0 | same | OK |
| ladder scale | `0x003B1A2C` | 0.97 | 0.97 | OK |
| floor | `0x00384148` | 0.03 | 0.03 | OK |
| full | `0x003B168C` | 1.0 | 1.0 | OK |
| slam override, race | `0x003A69BC` | 0.05 | 0.05 | OK |
| slam override, crash mode | `0x003A69C4` | 0.1 | 0.1 | OK |
| OOC window | vdb `+0x1C0` -> `v+0x13E4` | 1.0 | `B3_TDR_TOTAL_OOC_S` 1.0 | OK |
| the whole thing | -- | -- | **was absent (a fixed 26 / 4 m/s glue)** | **ADDED** |

**Car vs car -- `FUN_001121F0` / `FUN_00113960` (`burnout3_carcol.h`)**

Every one re-read this pass; all match.

| constant | VA | retail |
|---|---|---|
| `B3_CARCOL_RESTITUTION` | `0x003EBE3C` | 0.1 |
| `B3_CARCOL_MPH` | `0x0038994C` | 2.2369363 |
| `B3_CARCOL_SHOVE_K` | `0x0041A4D0` | 20.0 |
| `B3_CARCOL_SHOVE_MASS_CAP` | `0x003EBE70` | 2000.0 |
| `B3_CARCOL_IMPACT_SCALE` | `0x003EBE74` | 0.1 |
| `B3_CARCOL_CRASH_MPH` | `0x003EBE4C` | 150.0 |
| `B3_CARCOL_REAR_MIN_MPH` | `0x003EBE60` | 20.0 |
| `B3_CARCOL_REAR_RANGE_MPH` | `0x003EBE68` | 50.0 |
| `B3_CARCOL_SIDE_MIN_MPH` | `0x003EBE5C` | 30.0 |
| `B3_CARCOL_SIDE_RANGE_MPH` | `0x003EBE64` | 20.0 |
| `B3_CARCOL_LIGHT_FRAC` | `0x003EBE80` | 0.3 |
| `B3_CARCOL_SIDE_HI_MPH` | `0x0041A4C4` | 35.0 |
| `B3_CARCOL_SIDE_LO_MPH` | `0x0041A4C8` | 20.0 |
| `B3_CARCOL_SIDE_ANG` | `0x0041A4CC` | 40.0 |
| `B3_CARCOL_ATTACK_DV` | `0x003B1B68` | 17.8815994 (40 mph) |
| `B3_CARCOL_WRECK_IMPACT` | `0x003EBE50` | 5000.0 |
| `B3_CARCOL_WRECK_IMPACT_TR` | `0x003EBE54` | 2500.0 |
| `B3_CARCOL_NORM_BLEND` | `0x0041A4C0` | -0.9 |
| `B3_CARCOL_WRECK_RESTITUTION` | `0x004A1D98` | BSS, **no writer anywhere in the image** (one reference, the load @`0x00113F50`), so 0.0 -- ours 0.0 |

**Takedown rules -- unchanged, still `315/315` of the original inventory.**
The slam kinds, the OOC stamps, the 2.0 s attribution window, the 1.0 s
re-slam cooldown, the contact timers and the claim scan were all re-checked
against §13's existing executed cases and none moved.

### 12.6 What the harness must call

Two calls. `b3_td_wall_contact` per world contact, `b3_td_wall_take` once per
car per frame; the authority, the ladder and `FUN_0011AEF0`'s gate all run
inside. The exact `src/burnout3_full.c` hunks (and an idempotent script that
applies them) are in the integration note that accompanies this section.

```c
/* per world contact, before any push-out has altered the velocity */
b3_td_wall_contact(&g_tdr, slot, g_race_time, &cv, pt, n, vin);

/* once per car per frame, in tdr_frame_pass */
B3TdWallHit wh;
if (b3_td_wall_take(&g_tdr, i, &wh)) { ...enter the crash... }
```

`cv` is a `B3CrashVehicle` filled from the rigid body in GAME space (frame,
world inverse inertia, velocity, dir, omega, mass, bbox); `pt` and `n` are
world contact point and normal with `n` pointing at the car. The ladder needs
`g_tdr.car[i].view_dist2` set once per frame to the squared distance from
that car to the player's.

## 13. Verification inventory — `tools/validate_td_rules.py` (**401/401**)

| § | function(s) executed | cases / checks |
|---|---|---|
| 1 | `FUN_001989A0` | 4 seedings × 9 asserts + "the crash entry is never reached" |
| 2 | `FUN_00029F30` (full dispatch) | 6 kinds × 4 asserts |
| 3 | `FUN_00197BE0` | 7 cooldown / race-state seedings × 2 |
| 4 | `FUN_00197EA0` | 5 speed-gate seedings × 3 |
| 5 | `FUN_00197430` | 8 scenarios × 10 asserts (timer, radius, aggressor, aftertouch) |
| 6 | `FUN_00197040` | 4 timing/crashed seedings × 2 |
| 7 | `FUN_00198E60` | 4 seedings × 6 (dedup, counters, revenge, AI recovery) |
| 8 | `FUN_001994D0` | 10 vehicle classes, wall, none, psyche, 6 aftertouch, 4 BP ladders |
| 9 | `FUN_00197920` + `FUN_00195CE0` | 6 arm seedings + 3 award seedings |
| 10 | the whole chain, both sides | slam → OOC → wall crash → WALL TAKEDOWN, and the DENIED branch |
| 11 | image bytes | radius, mph, ladders, the 4 vtable slots, the 5 handover sites, and the static "no crash call" proof |
| 12 | `FUN_00105FC0` (the view-distance ladder, executed) | 13 distance bands x 3 asserts + 2 alt-radius + the human early-out, the 6 override conditions one at a time, the image bytes of both stored constants and the class compare, and 12 end-to-end wall-trigger decisions through `b3_td_wall_contact` |

Emulation notes: `DAT_004D5370` holds a **pointer** (the mode object is at
`ptr+0x1B8`); `FUN_001989A0` tolerates it being NULL (`0x00198C6B`), which is
the deterministic path the slam sections use, while the commit/award/denied
sections wire it up. `FUN_001987A0` is **cdecl** (`FUN_00197D20` @`0x00197E38`
does `add esp,0x10`) while `FUN_001994D0` is stdcall `ret 0x10` — getting
either wrong unbalances the stack and the caller "returns" into the ELF
header, the same class of failure RE_GAMEPLAY §9 records.

---

## 14. CRASH-PARITY wave 2 (2026-08-13)

Four items closed, plus the takedown-attribution audit the live-play report
asked for. Everything below is `[C-disasm]` unless a case is named; the new
differential sections are `validate_td_rules.py` 13/14/15 and
`validate_carcol.py`'s `gather` section.

### 14.1 The OBJECT / PROP crash trigger — the caller chain [C-disasm]

`FUN_00112E70` has exactly ONE caller: `FUN_00111CD0` @`0x00111D77`, the
collision-pair dispatcher. A pair record carries two handles at `pair+0x24`
and `pair+0x28`; a handle is `{u8 type, .., +0x04 matrix, +0x08 bbox,
+0x0C entity}`. The dispatcher's first branch @`0x00111D0B`:

```
   (A.type in {0,1,2,4,6,7} && B.type == 3)
|| (B.type in {0,1,2,4,6,7} && A.type == 3)
        -> the type-3 handle is SWAPPED into slot [10]   @0x00111D3E
           A.type in {0,1,2,4} -> FUN_00112E70     <-- THE OBJECT PATH
           A.type in {6,7}     -> FUN_001135E0
```

so **`FUN_00112E70` is <car-ish handle> vs a TYPE-3 (object/prop) handle**,
and it is the only route to that function. `A.type in {0,1,2}` against
`B.type in {0,1,2}` is the car-vs-car dispatch (`FUN_001121F0` /
`FUN_00113960`) and `type == 5` goes to `FUN_00113890`; none of those reach
here. `type == 8` returns immediately @`0x00111D06`.

**Which pairs may crash** is the 7×7 byte table `DAT_0039AE50`, indexed
`[classB*7 + classA]` (`IMUL EAX,EAX,0x7` @`0x0011303B`, the load
@`0x0011304E`). Classes come from `FUN_0010FBC0`:

| handle type | class |
|---|---|
| 0, 2 | 0 (a racecar) |
| 1 | 1 |
| 3 | 2 (an object / prop) |
| 4 | 3 when `entity+0x242B == DAT_0073BB8C`, else 5 |
| anything else | 6 |

The table, read from `build/burnout3.elf`, is all zero except

```
row 0 (B is a racecar): 1 1 1 1 1 1 0
row 2 (B is an OBJECT): 1 0 0 1 0 0 0
```

so against an object the crashable A classes are exactly **{0, 3}**: a
RACECAR, and the **designated big-hit traffic vehicle** (RE_NOTES 16.3) —
which is the "traffic big-hit vehicle passes near here" note resolved. One
extra veto @`0x00113069`: a type-3 handle whose entity has `+0x174 & 8` is
never crashable (the same bit `FUN_00026A70` reads to arm the big-hit
window).

### 14.2 The trigger itself @`0x0011329A`..`0x001133E4` [C-disasm]

```
require crashable && !(veh+0x1353 & 0x10) && veh+0x152C < 0
v_rel   = object point velocity - car point velocity          0x00113311
normal race:  closing = |dot(v_rel, contact normal)| * 2.2369363  [0x0038994C]
              fire if closing > veh+0x1534 * 75.0                 [0x003EBE44]
crash party:  closing = |v_rel| * 2.2369363   (FUN_00013C10 = the LENGTH,
              not the normal component)                           0x00113393
              fire if closing > veh+0x1534 * 20.0                 [0x003EBE48]
pair+0x20 = mass * 2.0 [0x003B1688] * closing_normal * 0.1 [0x003EBE74]
                       * 0.5 [0x003B1684]                          0x00113349
on fire: the push-out is SKIPPED ([ESP+0x1B] = 0 @0x001133E4) and
         FUN_0010DCA0(&DAT_0064ACE8, veh, grid) runs @0x0011357E with a cause
         record whose +0x01 = 1 and +0x08 = the object (§3).
```

**CORRECTION to §12.3:** the two branches do not use the same magnitude. A
normal race compares the NORMAL-DIRECTION closing speed; crash party compares
the full `|v_rel|`. §12.3's `closing_mph = |v_rel|` was right only for the
crash-party arm.

**`veh+0x152C` identified:** a post-spawn object-crash immunity timer.
`FUN_0011FE90` @`0x0011FF81` sets it to **1.0** [`0x003B168C`],
`FUN_0011BE50` @`0x0011C1CB` decays it by `2*dt` while it is above `-1e-4`
[`0x003B1E84`], and the vehicle init @`0x0011AAB2` leaves it at **-1.0**
[`0x003B16C0`]. So it blocks the object crash for the first 0.5 s of a car's
life and is negative (open) otherwise. `veh+0x1353` bit 4 is set together
with bit 3 by the ladder's `|= 0x18` @`0x00105FA0`, so `B3TdAuthority`'s
`crash_ok` gates both triggers.

**Why this matters:** at authority 1.0 the object test needs 75 mph
(33.5 m/s) of normal closing speed and **no head-on alignment at all**,
where `FUN_0011AEF0` additionally requires `|dot(n, at)| > authority*0.707`.
A fast SIDE-ON clip of a prop wrecks you here and nowhere else. At the 0.05
slammed authority it is 3.75 mph.

Ported: `b3_td_object_class` / `b3_td_object_crashable` /
`b3_td_object_contact` / `b3_td_object_take` (burnout3_td_rules.h §10). The
interface takes the two recovered class ids and the contact kinematics, so a
real prop entity plugs straight in; until the prop system lands,
`mesh_collide` maps the soup's structure band (low byte `0x15..0x20`, the one
classification the retail code itself makes — `FUN_0011BBE0`) onto class 2.
That mapping is GLUE and lives in the harness, not the module.

### 14.3 The NON-CRASHING wall SCRAPE — `FUN_0011AEF0`'s chassis response

The crash decision and the physical response are independent branches of the
same function: everything from the flattened normal @`0x0011B4B0` down to
`0x0011B904` runs on every wall contact, and the only test between them is
`dv > 0` [`0x003B16E0`] @`0x0011B764` whose `JBE` @`0x0011B771` jumps
straight to the crash tail. The terms, in retail order:

| # | address | term |
|---|---|---|
| 1 | `0x0011B489` | deflection `veh+0x130 += n_flat * min_edge_dist * 1.5` [`0x003B1870`] |
| 2 | `0x0011B55A` | head-on scrub: `headon > 0.707` → `vel *= 1 - (headon-0.707)*0.1` |
| 3 | `0x0011B5E5` | class-0 scrub by `veh+0x13A8`; others `*0.99` unless `veh+0x153E` |
| 4 | `0x0011B724` | `j = FUN_00106720(nh, contact point, POINT velocity, e = 0)` |
| 5 | `0x0011B777` | the `veh+0x194` impact magnitude |
| 6 | `0x0011B88E` | `d2 = normalize(nh - up*dot(up, nh))` — deliberately horizontal |
| 7 | `0x0011B8A2` | `veh+0x1404 > 0.1` → `veh+0x110 += d2*impact` (LINEAR ONLY); else `FUN_001206D0(d2*impact, point)` — **at the point, with torque** |

`veh+0x1404` is the BRAKE input (RE_NOTES 14): braking suppresses the
straightening torque, coasting keeps it. Step 7's at-point routing is the
term the harness was missing — it both stops the car AND turns its nose off
the barrier.

**The `-1000 * mass * dir` brake is NOT part of a static-wall scrape.**
`0x0011B0A5`..`0x0011B0F7` [`0x003B1744` = -1000] is guarded by
@`0x0011B09F` `CMP` of the wall count against its value BEFORE the second
poly-set loop, i.e. it fires only when the `veh+0x1590` set (the OTHER-CAR
hull polys, flag word `0x20`) contributed the contact. It is a car-vs-car
term and writes `veh+0xF0` (force accumulator), not the impulse.

Ported: `b3_td_wall_response` (burnout3_td_rules.h §9b) over the crash
module's existing exports, applied at `mesh_collide`'s contact site.

### 14.4 The ladder's distance input — [S] → [C] (executed)

`FUN_00105BD0` executed WHOLE under Unicorn (validate_td_rules §13) settles
both [S] claims of §12.2(a):

* the loop @`0x00105DC0` walks `EDI = 0x0073A1D0`, **stride `0x27E0`**,
  `i < [0x0073A1C0]`, and `this`-calls `*(*EDI + 0x14)` with `ECX = EDI` —
  the records ARE the racecar objects, not pointers to something else;
* it stores `|returned position − veh+0x204 row 3|²` into `veh+0x1560[i]`
  and keeps the nearest that passes `FUN_001AD4A0` on the SCORED car's
  position;
* `FUN_00106370` @`0x001063B0`..`0x001063F0` closes it independently: it
  picks a LOCAL PLAYER INDEX into `veh+0x1554` (`0` when `[0x0073A1C0] == 1`,
  else `grid & 1` — i.e. alternating between the two split-screen players)
  and indexes THE SAME base with `IMUL EAX,EAX,0x27E0; ADD EAX,0x73A1D0`.
  A human's own index comes from `racecar+0x27D0` @`0x0010639A`.

⇒ **`DAT_0073A1C0` is the LOCAL PLAYER count and `DAT_0073A1D0` the inline
array of those players' racecars.** In single player the ladder's `d2` is the
squared 3-D distance to THE PLAYER'S CAR, which is exactly what
`g_tdr.car[i].view_dist2` feeds. No fix needed. **[C]**

**New [S] surfaced:** @`0x00105EA0` the crash-entry flag has a second veto
independent of the ladder — `if (racecar+0x1688 == 0 && (racecar+0x245D != 0
|| racecar+0x1B93 == 0)) crash_ok = 0`. Neither byte is modelled by this
harness; retail's steady state must leave the entries enabled, so the port
treats them as such and the oracle seeds that state.

### 14.5 The presentation credit on a TAKEDOWN — [?] → [C-disasm]

RE_TAKEDOWN_FX §9.3 documented `FUN_00025850` (OnVehicleWrecked). The two
other entries are now recovered:

**`FUN_00025A30` "OnTakedown"** (`vtbl+0x0B4`; EBX = attacker racecar,
ESI = victim racecar, ECX = the rules object). After `FUN_000273F0` it
spends a credit at **two** sites, and in BOTH the racecar handed to
`FUN_00025CC0` in EAX is the **ATTACKER**:

```
0x00025A67  attacker+0x1920 == 1 && victim+0x1920 == 0
            -> FUN_00025CC0(EAX = attacker, ECX = rules, push victim)   0x00025A7E
0x00025A83  attacker+0x1920 == 0 && 0.3 [0x003B1750] > attacker+0x16C4
            -> FUN_00025CC0(EAX = attacker, ECX = rules, push victim)   0x00025AA3
```

Because `FUN_00025CC0` @`0x00025D29` gates the divisor-5 request on
`racecar+0x1920 == 0`, the first site (an AI taking the human down) spends
the AI's credit and requests NO slow-motion; the second (the human taking
someone down) spends the PLAYER's credit and DOES request divisor 5.

**`FUN_00025C50` "ForceWreck"** (`vtbl+0x0CC`) spends the credit
unconditionally at `0x00025C5B` with a NULL victim, on the racecar being
wrecked, and only then calls `FUN_0010DCA0` + `FUN_00125100(10, 0.3, up)`.

**Verdict:** takedown-caused crashes are NOT exempt — they run the same
one-credit machine. The credit is charged to the ATTACKER's racecar, and the
divisor-5 presentation only issues when that attacker is the human.

The counter semantics, from `0x00025CC6`..`0x00025D29`: the decrement only
happens while the value is `> 0` and `!= -1`, and the body runs once the
value READS 0 — so `+0x16C8 == 1` (what the event reset `FUN_00025AB0`
@`0x00025AE5` writes) means "this call presents", `== 2` means "the next one
does", `== 0` means "present every time" and `== -1` means **presentation
permanently disabled**. Every suppressed call sets `+0x16C4 = 1.0`.

**Harness divergence (open, needs a takedown-module export — see the
integration note):** `b3_tdfx_crash_begin` spends the player's credit on the
player's OWN crash, which is `FUN_00025850`'s path and correct. Nothing
spends it when the PLAYER TAKES SOMEONE DOWN, so after a player takedown our
player still holds a credit retail would already have spent.

### 14.6 The racing gather's two RUNTIME filters — `FUN_0011BBE0` [C-disasm]

`FUN_0011BBE0` is the callback `FUN_0011BE50` pushes at `0x0011BD90` into the
kd walker `FUN_001AFF70`; `EDI` is the candidate record (normal at `+0x10`,
prim at `+0x60`, surface u16 at `prim+4` — layout proved by the appender
`FUN_0010A8E0`) and `ESI` is the vehicle.

```
low = type & 0xFF
0x0011BBFE  low == 0x23                          -> skip
0x0011BC03  low == 0x22                          -> skip
0x0011BC08  type & 0x1000                        -> skip
0x0011BC0D  0x15 <= low  \  the STRUCTURE band
0x0011BC15  low <= 0x20  /
0x0011BC2D     j = dot(normal, *(vec3*)(veh + 0xB0))     (FUN_00013C60)
0x0011BC32     j > 0.5 [0x003B1684]              -> skip
0x0011BC43  -0.7 [0x0039B264] > normal.y         -> skip
0x0011BC4B  FUN_0010A8E0 -- append
```

The velocity is the car's OWN `veh+0xB0` (RE_NOTES 14: "+0xB0 is the true
velocity vector, +0xBC its magnitude"), not a relative velocity, so filter
(a) reads "drop the structure faces I am already separating from" — the
anti-snag. Filter (b) applies to every type, not just the band.

Ported into `b3_sweep_sphere_ex` (burnout3_collision.h). Both filters are
invariant under the loader's z mirror (`M(n).y == n.y` and
`dot(Mv, Mn) == dot(v, n)`), so they evaluate on the stored normal against a
harness-space velocity. The down-ray keeps the static set only: its
differential oracle is the WRECK gather `FUN_00109CE0`, it has no velocity
input, and filter (b) is provably a no-op there (a face with `n.y < -0.7` can
only be reached on its back side by a downward segment, which the one-sided
`det > 0` test already rejects).

### 14.7 Takedown attribution audit (live-play report)

The report was "I ram a CPU, it crashes, no takedown". The module chain is
sound — §10's end-to-end case executes the real functions, and a live
autodrive session reproduces it in full:

```
[TD-CHAIN] t=28.27 SLAM kind=5 att=0 vic=4 str=0.95 -> victim slam_t=28.27
                                          aggressor=0 aggressor_t=28.27
[TD-CHAIN] t=28.77 car 4 CRASH (wall) aggressor=0 aggressor_t=28.27
                                          claims: car0@28.77
[Burnout3] t=28.77 car 4 WALL CRASH ... authority 0.05, out of control: 1
[Burnout3] TAKEDOWN COMMIT: car 0 took down car 4 -- message 0x9D, +150 BP
```

Audited against retail, one at a time:

1. **kind classification** — `FUN_001121F0` is executed for real by
   `validate_carcol` (752/752); the light/full split is retail's own
   `LIGHT_FRAC >= s`. A LIGHT slam (kind 3/4) genuinely does NOT stamp:
   `FUN_00197D20` ends at `FUN_001987A0` (BP) with no write to
   `racecar+0x1598/+0x16BC` (§2.3). Faithful-but-strict.
2. **window** — the aggressor claim needs the crash inside "Maximum Crash
   Wait Time" 2.0 s [`0x003F7404`] of the stamp, and the stamp is
   single-valued: a later full slam by another car REPLACES it. Both retail.
3. **claim scan** — `FUN_00197040` re-read: the only cancel is the SCANNING
   car's own `+0x18FA`, exactly as ported. No "lesser contact" cancel exists.
4. **the AI class byte** — `veh+0x215` is the vehicle-state byte; the
   takedown bookkeeping keys off `racecar+0x1920`, which the harness sets
   from the grid slot. Orthogonal; unaffected.

**The real defect, fixed:** a racer wrecked by a TRAFFIC car set
`crashed_until` directly and never called the crash entry, so
`FUN_00197430` never ran and NO claim was armed. Retail has exactly one crash
entry (`FUN_0010DCA0` → `FUN_0010DD20` → game-context `+0x48` →
`FUN_00197750` → `FUN_00197430`), so every crash arms claims. A CPU the
player has just slammed is at 0.05 authority and out of control — the case
where it most often ends up in oncoming traffic — which is exactly the
"they crash but I get nothing" the report describes. The harness path now
calls `b3_td_on_crash` with the OBJECT cause (vehicle class CAR → message
`0x94`), and no longer clobbers the victim's out-of-control stamp with a
`slam_by = -1` write that would have hidden the aggressor. The
`B3_TEST_CRASH_AT` debug injection got the same treatment.

**GLUE noted, not changed:** the harness fires `b3_td_contact_notify`
(`FUN_00197920`, which arms DENIED/LUCKY) for every pair inside a 4 m radius
every frame. Retail reaches it through the game-context `+0x54` virtual on a
contact/pass event. Over-firing it produces premature "TAKEDOWN DENIED"
callouts but cannot block an award — neither `FUN_00195CE0` nor the port
touches the claim. [S]

### 14.8 Verification inventory update

`tools/validate_td_rules.py` **532/532** (was 401), `validate_carcol.py`
**752/752** (was 730).

| § | function(s) executed | checks |
|---|---|---|
| 13 | `FUN_00105BD0` WHOLE (the ladder's distance input) | 14 |
| 14 | `FUN_0010FBC0` (16 handle/designated combinations), `DAT_0039AE50` byte for byte, and the ported object trigger over 16 contacts | 94 |
| 15 | `FUN_0011AEF0`'s branch bytes (the response is unconditional, the brake gate, the -1000 guard) + the ported response's yaw behaviour | 24 |
| carcol `gather` | `FUN_0011BBE0`'s predicate bytes + the filters over the real `build/collision.bin` | 22 |
