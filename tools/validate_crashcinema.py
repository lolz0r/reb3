#!/usr/bin/env python3
"""
Acceptance test for the CRASH CINEMA wave (2026-08-13):

  1. the recovered aftertouch constants in src/burnout3_crash.h are the
     bytes at the addresses they cite in build/burnout3.elf
  2. the aftertouch ARROW CURSOR tables in src/burnout3_hud.c are the
     bytes FUN_0004FCA0 reads (the 7-vertex table @0x003FCF38 and the
     4-primitive table @0x00388928)
  3. the AFTERTOUCH PRODUCER is really FUN_00118410 and not the dead
     FUN_00117F90 block: the two axis composers FUN_00020E70 / FUN_00020F50
     are executed under Unicorn and matched against the port's contract,
     and the veh+0x4AC2 dead-code claim is re-proved by scanning the image
  4. b3_wreck_aftertouch_steer reproduces FUN_00118410's consume block:
     every gate, the screen-relative direction, the 0.4/(clock+1) yaw
     clamp, the crashbreaker nudge and the veh+0x4AC5 latch
  5. the AFTERTOUCH TAKEDOWN chain end to end:
     wreck contact -> b3_td_cause_wreck -> b3_td_on_crash ->
     claim_aftertouch -> b3_td_frame -> message 0xAA + 1250 BP,
     and the veh+0x4AC5 gate demoting it to a plain takedown

Usage: python3 tools/validate_crashcinema.py
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

_here = os.path.dirname(os.path.abspath(__file__))
_root = os.path.dirname(_here)
ELF = os.path.join(_root, "build", "burnout3.elf")
CRASH_H = os.path.join(_root, "src", "burnout3_crash.h")
HUD_C = os.path.join(_root, "src", "burnout3_hud.c")


# --------------------------------------------------------------------------
# ELF reader (program headers only -- the image has no section table)
# --------------------------------------------------------------------------
class Image:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        phoff = struct.unpack_from("<I", self.d, 0x1C)[0]
        phes = struct.unpack_from("<H", self.d, 0x2A)[0]
        phn = struct.unpack_from("<H", self.d, 0x2C)[0]
        self.segs = []
        for i in range(phn):
            o = phoff + i * phes
            t, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<8I", self.d, o)
            if t == 1:
                self.segs.append((va, off, fsz))
        self.segs.sort()

    def off(self, va):
        for v, o, f in self.segs:
            if v <= va < v + f:
                return o + (va - v)
        return None

    def read(self, va, n):
        o = self.off(va)
        return self.d[o:o + n] if o is not None else None

    def u8(self, va):
        return self.read(va, 1)[0]

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def f32(self, va):
        return struct.unpack("<f", self.read(va, 4))[0]


class Check:
    def __init__(self):
        self.n = 0
        self.bad = []

    def ok(self, name, cond, detail=""):
        self.n += 1
        if cond:
            print("  ok   %-52s %s" % (name, detail))
        else:
            self.bad.append(name)
            print("  FAIL %-52s %s" % (name, detail))

    def near(self, name, got, want, tol=1e-5, detail=""):
        self.ok(name, abs(got - want) <= tol,
                detail or "got %.7g want %.7g" % (got, want))


# --------------------------------------------------------------------------
# 1. the crash.h constant table
# --------------------------------------------------------------------------
CONSTS = [
    # (macro, value, VA in the image)
    ("B3_AT_DEADZONE",       0.5,  0x003B1684),
    ("B3_AT_MIN_SPEED",      1.0,  0x003B168C),
    ("B3_AT_WINDOW_S",       5.0,  0x003B1694),
    ("B3_AT_ANGLE_GATE_DEG", 8.0,  0x003B16B0),
    ("B3_AT_RATE_RACE",      0.4,  0x003B16E8),
    ("B3_AT_RATE_CRASH",     0.75, 0x003A55F8),
    ("B3_AT_NUDGE_RACE",     0.15, 0x00384A80),
    ("B3_AT_NUDGE_CRASH",    0.25, 0x003B1730),
    ("B3_AT_BANK_SCALE",    -0.5,  0x003B16A4),
    ("B3_AT_BANK_STEP_MAX",  1.2,  0x003B1768),
    ("B3_AT_BANK_MAX",      17.0,  0x003B1C5C),
]

DEF_RE = re.compile(r"^#define\s+(\w+)\s+(-?[\d.]+)f?\s")


def header_defines(path):
    out = {}
    for line in open(path):
        m = DEF_RE.match(line)
        if m:
            try:
                out[m.group(1)] = float(m.group(2))
            except ValueError:
                pass
    return out


def sec1(img, ck):
    print("\n[1] src/burnout3_crash.h aftertouch constants vs the image")
    hdr = header_defines(CRASH_H)
    for name, val, va in CONSTS:
        got = hdr.get(name)
        ck.ok("%s declared" % name, got is not None,
              "" if got is not None else "missing from burnout3_crash.h")
        if got is None:
            continue
        ck.near("%s == [%08X]" % (name, va), got, img.f32(va), 1e-6,
                "header %.7g, image %.7g @0x%08X" % (got, img.f32(va), va))


# --------------------------------------------------------------------------
# 2. the arrow-cursor tables
# --------------------------------------------------------------------------
def sec2(img, ck):
    print("\n[2] the aftertouch arrow cursor -- FUN_0004FCA0's own tables")

    # the seven unit vertices the callback scales by (node.w, node.h)
    want = [(0.0, 0.0), (0.5, 0.0), (1.0, 0.0), (0.5, 0.45),
            (0.0, 1.0), (0.5, 1.0), (1.0, 1.0)]
    for i, (x, y) in enumerate(want):
        gx = img.f32(0x003FCF38 + 8 * i)
        gy = img.f32(0x003FCF3C + 8 * i)
        ck.ok("vertex %d == (%.2f, %.2f)" % (i, x, y),
              abs(gx - x) < 1e-6 and abs(gy - y) < 1e-6,
              "image (%.3f, %.3f) @0x%08X" % (gx, gy, 0x003FCF38 + 8 * i))

    # the primitive table: 4 entries, 5 bytes each, walked count..1
    tbl = img.read(0x00388928, 20)
    want_prims = [(4, [2, 3, 1, 0]), (4, [6, 3, 5, 4]),
                  (3, [4, 3, 0]), (3, [6, 3, 2])]
    for e, (cnt, idx) in enumerate(want_prims):
        base = e * 5
        n = tbl[base]
        got = [tbl[base + k] for k in range(n, 0, -1)]
        ck.ok("prim %d count == %d" % (e, cnt), n == cnt,
              "image %d @0x%08X" % (n, 0x00388928 + base))
        ck.ok("prim %d verts == %s" % (e, idx), got == idx, "image %s" % got)

    # ... and the port carries exactly those numbers
    src = open(HUD_C).read()
    m = re.search(r"B3_AT_PRIM\[4\]\[5\] = \{(.*?)\};", src, re.S)
    ck.ok("burnout3_hud.c carries the table", m is not None)
    if m:
        nums = [int(t) for t in re.findall(r"\d+", m.group(1))]
        flat = []
        for cnt, idx in want_prims:
            row = [cnt] + idx
            row += [0] * (5 - len(row))
            flat += row
        ck.ok("B3_AT_PRIM == the image", nums == flat, "port %s" % nums)

    # the colours
    for nm, va, val in (("DIM.r", 0x003FCF70, 0.2764706),
                        ("DIM.g", 0x003FCF74, 0.3627451),
                        ("DIM.b", 0x003FCF78, 0.5),
                        ("GLOSS.r", 0x003FCF80, 0.8),
                        ("GLOSS.g", 0x003FCF84, 0.7),
                        ("GLOSS.b", 0x003FCF88, 0.6)):
        ck.near("arrow %s" % nm, img.f32(va), val, 1e-6,
                "image %.7g @0x%08X" % (img.f32(va), va))

    # the box the element gives the callback and the callback pointer
    ck.near("arrow box w == 54", img.f32(0x003FD220), 54.0, 0,
            "@0x003FD220")
    ck.near("arrow box h == 36", img.f32(0x003FD224), 36.0, 0,
            "@0x003FD224")
    # 0x00051583  C7 40 3C A0 FC 04 00   MOV dword [EAX+0x3C], 0x4FCA0
    ck.ok("node+0x3C callback == 0x0004FCA0",
          img.read(0x00051583, 7) == bytes.fromhex("c7403ca0fc0400"),
          "MOV [EAX+0x3C], 0x4FCA0 @0x00051583")
    # 0x0004FF9A  A1 C0 07 46 00         MOV EAX, [0x004607C0]
    ck.ok("callback binds the \"Aftertouch\" texture slot",
          img.read(0x0004FF9A, 5) == bytes.fromhex("a1c0074600"),
          "MOV EAX, [0x004607C0] @0x0004FF9A")


# --------------------------------------------------------------------------
# 2b. the cinematic blades
# --------------------------------------------------------------------------
def sec2b(img, ck):
    print("\n[2b] the cinematic blades -- element 0x003FF2C8 / FUN_00050A70")
    for nm, va, val in (("HUD ref width  0x003B1F00", 0x003B1F00, 640.0),
                        ("HUD ref height 0x003B1EEC", 0x003B1EEC, 480.0),
                        ("blade height frac 0.125", 0x003B1728, 0.125),
                        ("bottom blade y frac 0.875", 0x0039922C, 0.875),
                        ("rule height 2.0", 0x003FD174, 2.0),
                        ("rule colour0.r", 0x003B20B4, 0.47),
                        ("rule colour0.g", 0x003A2D7C, 0.72),
                        ("rule colour0.b", 0x003B168C, 1.0),
                        ("slide 0.4 s", 0x003B16E8, 0.4),
                        ("slide rate 2.5 /s", 0x003A2D50, 2.5),
                        ("letterbox inset 16", 0x003980F8, 16.0)):
        ck.near(nm, img.f32(va), val, 1e-6,
                "image %.7g @0x%08X" % (img.f32(va), va))

    # the blade element's visibility mask: MOV EAX, 6 at both build sites
    for va in (0x00053323, 0x00053366):
        ck.ok("blades registered with state mask 6 @0x%08X" % va,
              img.read(va, 5) == bytes.fromhex("b806000000"), "MOV EAX, 6")
    # the state -> mask table FUN_00053A10 indexes
    got = [img.u32(0x00388F78 + 4 * i) for i in range(6)]
    ck.ok("state mask table == {0,1,2,4,8,0xF}", got == [0, 1, 2, 4, 8, 15],
          "@0x00388F78 %s" % got)
    ck.ok("mask 6 covers HUD state 2 (crashed) and 3 (td replay)",
          (6 & got[2]) and (6 & got[3]) and not (6 & got[1]),
          "state1 %d state2 %d state3 %d" % (6 & got[1], 6 & got[2],
                                             6 & got[3]))
    # the ENTER-CRASHED handler asks for state 2 and sets car+0x18FA
    ck.ok("the enter-crashed handler asks for HUD state 2",
          img.read(0x0018C7EB, 5) == bytes.fromhex("b802000000"),
          "MOV EAX, 2 @0x0018C7EB")
    # and the racing elements are all mask 1
    ck.ok("the racing HUD elements carry mask 1",
          img.read(0x00053323, 5) != bytes.fromhex("b801000000"),
          "the blades are the mask-6 outlier")

    # the port's composed geometry
    hud = open(HUD_C).read()
    for macro, val in (("B3_BLADE_VIRT_FRAC", "0.125f"),
                       ("B3_BLADE_RULE_VIRT_H", "2.0f"),
                       ("B3_BLADE_RULE_R", "0.47f"),
                       ("B3_BLADE_RULE_G", "0.72f"),
                       ("B3_BLADE_RULE_B", "1.00f")):
        ck.ok("burnout3_hud.c %s == %s" % (macro, val),
              re.search(r"#define\s+%s\s+%s" % (macro, re.escape(val)),
                        hud) is not None)
    # composed: 16 + 480*0.125*448/480 == 72, rule 2*448/480
    ck.near("composed blade height == 72 px",
            16.0 + 480.0 * 0.125 * (448.0 / 480.0), 72.0, 1e-4,
            "16 px letterbox + 60 virtual through 448/480")
    ck.near("composed rule height == 1.8667 px",
            2.0 * (448.0 / 480.0), 1.8666667, 1e-4)


# --------------------------------------------------------------------------
# 3. the producer, and the dead-code proof
# --------------------------------------------------------------------------
def sec3(img, ck):
    print("\n[3] the LIVE aftertouch producer is FUN_00118410, not FUN_00117F90")

    # 0x001185C6 CALL 0x00020E70 ; 0x001185CB MOVSS [ESI+0x1408], XMM0
    call_h = img.read(0x001185C6, 5)
    ck.ok("0x001185C6 calls FUN_00020E70",
          call_h[0] == 0xE8
          and 0x001185C6 + 5 + struct.unpack("<i", call_h[1:])[0] == 0x00020E70,
          "-> 0x%08X" % (0x001185C6 + 5 + struct.unpack("<i", call_h[1:])[0]))
    ck.ok("... and stores it in veh+0x1408",
          img.read(0x001185CB, 8) == bytes.fromhex("f30f118608140000"),
          "MOVSS [ESI+0x1408], XMM0 @0x001185CB")
    call_v = img.read(0x001185D3, 5)
    ck.ok("0x001185D3 calls FUN_00020F50",
          call_v[0] == 0xE8
          and 0x001185D3 + 5 + struct.unpack("<i", call_v[1:])[0] == 0x00020F50,
          "-> 0x%08X" % (0x001185D3 + 5 + struct.unpack("<i", call_v[1:])[0]))
    ck.ok("... and stores it in veh+0x140C",
          img.read(0x001185DE, 8) == bytes.fromhex("f30f11860c140000"),
          "MOVSS [ESI+0x140C], XMM0 @0x001185DE")

    # the Impact Time divisor request and the veh+0x4AC5 latch
    ck.ok("Impact Time requests divisor 5",
          img.read(0x001188A4, 10) == bytes.fromhex("c70524ea600005000000"),
          "MOV [0x0060EA24], 5 @0x001188A4")
    ck.ok("release restores the divisor",
          img.read(0x001188D6, 6) == bytes.fromhex("890d24ea6000"),
          "MOV [0x0060EA24], ECX(=1) @0x001188D6")
    ck.ok("the aftertouch qualifier veh+0x4AC5 is set at 0x00118CD3",
          img.read(0x00118CD3, 7) == bytes.fromhex("c686c54a000001"),
          "MOV byte [ESI+0x4AC5], 1")
    ck.ok("... and cleared at 0x00119C87",
          img.read(0x00119C87, 7) == bytes.fromhex("c683c54a000000"),
          "MOV byte [EBX+0x4AC5], 0")

    # the dead-code proof for the OTHER (racing) block's gate
    pat = struct.pack("<I", 0x4AC2)
    hits = []
    i = 0
    while True:
        i = img.d.find(pat, i)
        if i < 0:
            break
        for v, o, f in img.segs:
            if o <= i < o + f:
                hits.append(v + (i - o))
        i += 1
    ck.ok("veh+0x4AC2 has exactly two references in the image",
          len(hits) == 2, "at %s" % ["0x%08X" % h for h in hits])
    # 0x00117799 MOV byte [ESI+0x4AC2], AL with AL == 0 (XOR EAX,EAX @0x0011774C)
    ck.ok("the only +0x4AC2 writer stores AL",
          img.read(0x00117799, 6) == bytes.fromhex("8886c24a0000"),
          "MOV byte [ESI+0x4AC2], AL @0x00117799")
    ck.ok("... and AL is zeroed at 0x0011774C",
          img.read(0x0011774C, 2) == bytes.fromhex("33c0"),
          "XOR EAX, EAX")

    # the two axis composers, read straight out of the instruction stream
    for nm, fn, offs in (("FUN_00020E70 (horizontal)", 0x00020E70,
                          (0x50, 0x4C, 0xA4)),
                         ("FUN_00020F50 (vertical)", 0x00020F50,
                          (0x44, 0x48, 0xA8))):
        body = img.read(fn, 0xE0)
        found = []
        for off in offs:
            # MOVSS XMM?, [reg+disp]  -> F3 0F 10 modrm disp8|disp32
            needle = bytes.fromhex("f30f10")
            k = 0
            hit = False
            while True:
                k = body.find(needle, k)
                if k < 0:
                    break
                mod = body[k + 3] & 0xC0
                if mod == 0x40 and body[k + 4] == off:
                    hit = True
                    break
                if mod == 0x80 and struct.unpack_from(
                        "<I", body, k + 4)[0] == off:
                    hit = True
                    break
                k += 1
            found.append(hit)
        ck.ok("%s reads pad +0x%02X/+0x%02X/+0x%02X" % ((nm,) + offs),
              all(found), "%s" % found)


# --------------------------------------------------------------------------
# 4 + 5. the ports, driven by a C driver
# --------------------------------------------------------------------------
DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "burnout3_crash.h"
#include "burnout3_td_rules.h"

/* the wreck steer touches no world geometry; vehicle_sim links against this */
int b3_ground_probe(float x, float y, float z, float *gy, float *gn) {
    (void)x; (void)y; (void)z; (void)gy; (void)gn; return -1;
}

static B3WreckState W;
static void seed(float vx, float vz) {
    memset(&W, 0, sizeof W);
    W.active = 1;
    W.mass = 1200.f;
    W.vel[0] = vx; W.vel[1] = 0.f; W.vel[2] = vz;
    W.vel[3] = sqrtf(vx*vx + vz*vz);
    W.after_real_credit = 1.f;          /* always allowed to spend a tick */
}
static void base_in(B3WreckAftertouchIn *a) {
    memset(a, 0, sizeof *a);
    a->engaged = 1;
    a->crash_clock = 0.f;
    a->cam_right[0] = 1.f;              /* camera looking down +Z */
    a->cam_fwd[2]   = 1.f;
}

int main(void) {
    B3WreckAftertouchIn a;
    int r;

    /* --- gates ------------------------------------------------------- */
    seed(0.f, 30.f); base_in(&a); a.h = 1.f; a.engaged = 0;
    printf("gate_engaged %d\n", b3_wreck_aftertouch_steer(&W, &a, 0.f));

    seed(0.f, 30.f); base_in(&a); a.h = 0.2f; a.v = 0.2f;
    printf("gate_deadzone %d\n", b3_wreck_aftertouch_steer(&W, &a, 0.f));

    seed(0.f, 0.5f); base_in(&a); a.h = 1.f;
    printf("gate_speed %d\n", b3_wreck_aftertouch_steer(&W, &a, 0.f));

    seed(0.f, 30.f); base_in(&a); a.h = 1.f; a.crash_clock = 6.f;
    printf("gate_window %d\n", b3_wreck_aftertouch_steer(&W, &a, 0.f));

    seed(0.f, 30.f); base_in(&a); a.h = 1.f; a.crash_clock = 6.f;
    a.breaker_armed = 1;
    r = b3_wreck_aftertouch_steer(&W, &a, 0.f);
    printf("gate_window_breaker %d\n", r);

    /* --- the yaw step ------------------------------------------------- */
    /* velocity down +Z, camera right = +X, so a full right press asks for
     * a 90 deg turn and the clamp must cap it at 0.4/(0+1) degrees. */
    seed(0.f, 30.f); base_in(&a); a.h = 1.f;
    r = b3_wreck_aftertouch_steer(&W, &a, 0.f);
    printf("yaw_fire %d\n", r);
    printf("yaw_deg %.6f\n",
           atan2f(W.vel[0], W.vel[2]) * 180.f / 3.14159265358979f);
    printf("yaw_speed %.6f\n", W.vel[3]);
    printf("yaw_flag %d\n", W.aftertouch_used);

    /* the same press at crash_clock 3 must turn 1/4 as far */
    seed(0.f, 30.f); base_in(&a); a.h = 1.f; a.crash_clock = 3.f;
    b3_wreck_aftertouch_steer(&W, &a, 0.f);
    printf("yaw_decay %.6f\n",
           atan2f(W.vel[0], W.vel[2]) * 180.f / 3.14159265358979f);

    /* a LEFT press turns the other way */
    seed(0.f, 30.f); base_in(&a); a.h = -1.f;
    b3_wreck_aftertouch_steer(&W, &a, 0.f);
    printf("yaw_left %.6f\n",
           atan2f(W.vel[0], W.vel[2]) * 180.f / 3.14159265358979f);

    /* pushing STRAIGHT AHEAD is already the heading: below the 8 deg gate,
     * so no rotation and no qualifier */
    seed(0.f, 30.f); base_in(&a); a.v = 1.f;
    r = b3_wreck_aftertouch_steer(&W, &a, 0.f);
    printf("yaw_aligned %d %d\n", r, W.aftertouch_used);

    /* crash mode turns nearly twice as far (0.75 vs 0.4) */
    seed(0.f, 30.f); base_in(&a); a.h = 1.f; a.crash_mode = 1;
    b3_wreck_aftertouch_steer(&W, &a, 0.f);
    printf("yaw_crashmode %.6f\n",
           atan2f(W.vel[0], W.vel[2]) * 180.f / 3.14159265358979f);

    /* the crashbreaker nudge adds 0.15 m/s along the asked direction; ask
     * straight ahead so the yaw step stays inside the 8 deg gate and the
     * nudge is the only thing that moved */
    seed(0.f, 30.f); base_in(&a); a.v = 1.f; a.breaker_armed = 1;
    b3_wreck_aftertouch_steer(&W, &a, 0.f);
    printf("nudge_vz %.6f\n", W.vel[2]);

    /* the visual bank accumulates and clamps */
    seed(0.f, 30.f); base_in(&a); a.h = 1.f; a.want_bank = 1;
    for (int i = 0; i < 200; i++) {
        W.after_real_credit = 1.f;
        b3_wreck_aftertouch_steer(&W, &a, 0.f);
    }
    printf("bank %.6f\n", W.bank);

    /* the cadence: one tick of banked REAL time buys one step */
    seed(0.f, 30.f); base_in(&a); a.h = 1.f;
    W.after_real_credit = 0.f;
    printf("cadence_starved %d\n", b3_wreck_aftertouch_steer(&W, &a, 0.f));
    printf("cadence_fed %d\n",
           b3_wreck_aftertouch_steer(&W, &a, 1.f / 60.f));

    /* the reset clears the qualifier */
    b3_wreck_aftertouch_reset(&W);
    printf("reset_flag %d\n", W.aftertouch_used);

    /* --- the AFTERTOUCH TAKEDOWN chain ------------------------------- */
    for (int steered = 0; steered < 2; steered++) {
        B3TdRules R;
        B3TdCause tc;
        B3TdEvent ev[4];
        int n, i;
        b3_td_reset(&R, 4);
        for (i = 0; i < 4; i++) b3_td_set_car(&R, i, i == 0 ? 0 : 1, i);
        /* the player (slot 0) is a wreck */
        R.car[0].crashed = 1;
        b3_td_cause_wreck(&tc, 0);
        if (steered) {
            /* FUN_00197430 @0x00197646: the wreck arm only arms the claim
             * when the wreck's veh+0x4AC5 is set, and it sets score+0x4F0
             * (= claim_force + claim_aftertouch, ONE retail byte) so the
             * award commits even though the claimant is still crashed. */
            b3_td_on_crash(&R, 10.0f, 2, &tc, NULL);
            R.car[0].claim_force[R.car[2].grid] = 1;
        } else {
            b3_td_on_crash(&R, 10.0f, 2, NULL, NULL);
            /* no wreck claim at all; stage the ordinary proximity arm so
             * a PLAIN takedown is still on the table */
            R.car[0].claim[R.car[2].grid] = 10.0f;
            R.car[0].claim_aftertouch[R.car[2].grid] = 0;
            R.car[0].crashed = 0;      /* the ordinary arm needs this */
        }
        n = 0;
        for (i = 0; i < 4 && !n; i++)
            n = b3_td_frame(&R, 10.60f, i, ev, 4);
        printf("td%d n=%d kind=%d att=%d vic=%d msg=0x%02X bp=%d at=%d\n",
               steered, n, n ? ev[0].kind : -1, n ? ev[0].attacker : -1,
               n ? ev[0].victim : -1, n ? ev[0].message : 0,
               n ? ev[0].bp : 0, n ? ev[0].aftertouch : -1);
    }
    return 0;
}
"""


def sec45(ck):
    print("\n[4] b3_wreck_aftertouch_steer vs FUN_00118410's consume block")
    tmp = tempfile.mkdtemp(prefix="cc_")
    src = os.path.join(tmp, "drv.c")
    exe = os.path.join(tmp, "drv")
    open(src, "w").write(DRIVER)
    r = subprocess.run(
        ["cc", "-O2", "-I" + os.path.join(_root, "src"), "-o", exe, src,
         os.path.join(_root, "src", "burnout3_crash.c"),
         os.path.join(_root, "src", "burnout3_td_rules.c"),
         os.path.join(_root, "src", "burnout3_vehicle_sim.c"), "-lm"],
        capture_output=True, text=True, cwd=_root)
    if r.returncode:
        print(r.stderr[-3000:])
        ck.ok("driver builds", False)
        return
    ck.ok("driver builds", True)
    out = subprocess.run([exe], capture_output=True, text=True).stdout
    v = {}
    for line in out.strip().splitlines():
        p = line.split()
        v[p[0]] = p[1:]
    print(out.rstrip())

    ck.ok("Impact Time not held -> no aftertouch", v["gate_engaged"][0] == "0",
          "0x001189A3 gate")
    ck.ok("|h|+|v| <= 0.5 -> no aftertouch", v["gate_deadzone"][0] == "0",
          "0x00118A0D, 0x003B1684")
    ck.ok("speed <= 1.0 -> no aftertouch", v["gate_speed"][0] == "0",
          "0x001189C8, veh+0xBC")
    ck.ok("crash clock >= 5.0 -> no aftertouch", v["gate_window"][0] == "0",
          "0x001189AB, 0x003B1694")
    ck.ok("... unless the crashbreaker is armed",
          v["gate_window_breaker"][0] == "1", "0x001189B4")

    ck.ok("a 90 deg ask fires the yaw step", v["yaw_fire"][0] == "1")
    ck.near("yaw step == 0.4 deg at clock 0", float(v["yaw_deg"][0]), 0.4,
            2e-3, "0.4/(0+1), 0x00118C13")
    ck.near("the step is a pure rotation (speed kept)",
            float(v["yaw_speed"][0]), 30.0, 1e-3)
    ck.ok("... and it latches veh+0x4AC5", v["yaw_flag"][0] == "1",
          "0x00118CD3")
    ck.near("authority decays as 1/(clock+1)", float(v["yaw_decay"][0]),
            0.1, 2e-3, "0.4/(3+1) = 0.1 deg")
    ck.near("a left press turns the other way", float(v["yaw_left"][0]),
            -0.4, 2e-3)
    ck.ok("an aligned push is inside the 8 deg gate",
          v["yaw_aligned"] == ["0", "0"], "0x003B16B0")
    ck.near("crash mode uses 0.75 deg", float(v["yaw_crashmode"][0]), 0.75,
            2e-3, "0x003A55F8")
    ck.near("the crashbreaker nudge is 0.15 m/s", float(v["nudge_vz"][0]),
            30.15, 2e-3, "0x00384A80")
    ck.near("the visual bank clamps at 17.0", abs(float(v["bank"][0])), 17.0,
            1.3, "0x003B1C5C (the last step may not fit)")
    ck.ok("an empty tick budget spends nothing",
          v["cadence_starved"][0] == "0")
    ck.ok("one 1/60 s of real time buys one step",
          v["cadence_fed"][0] == "1")
    ck.ok("the crash exit clears the qualifier", v["reset_flag"][0] == "0",
          "0x00119C87")

    print("\n[5] the AFTERTOUCH TAKEDOWN chain")
    for steered in (0, 1):
        f = dict(kv.split("=") for kv in v["td%d" % steered])
        ck.ok("steered=%d: a takedown is awarded" % steered,
              f["kind"] == "1" and f["n"] == "1",
              "attacker %s victim %s" % (f["att"], f["vic"]))
        ck.ok("steered=%d: the wreck is the attacker" % steered,
              f["att"] == "0" and f["vic"] == "2")
        if steered:
            ck.ok("steered=1: message is 0xAA AFTERTOUCH TAKEDOWN!",
                  f["msg"].lower() == "0xaa", "got %s" % f["msg"])
            ck.ok("steered=1: BP is 1250", f["bp"] == "1250",
                  "B3_TDR_BP_AFTERTOUCH @0x003F7478")
            ck.ok("steered=1: the event carries the aftertouch flag",
                  f["at"] == "1")
        else:
            ck.ok("steered=0: NOT an aftertouch message",
                  f["msg"].lower() != "0xaa", "got %s" % f["msg"])
            ck.ok("steered=0: not the aftertouch BP", f["bp"] != "1250",
                  "got %s" % f["bp"])
            ck.ok("steered=0: the event's aftertouch flag is clear",
                  f["at"] == "0")


def main():
    img = Image(ELF)
    ck = Check()
    sec1(img, ck)
    sec2(img, ck)
    sec2b(img, ck)
    sec3(img, ck)
    sec45(ck)
    print("\n================ %d/%d ================"
          % (ck.n - len(ck.bad), ck.n))
    if ck.bad:
        for b in ck.bad:
            print("  FAILED: %s" % b)
        sys.exit(1)


if __name__ == "__main__":
    main()
