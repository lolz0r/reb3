#!/usr/bin/env python3
"""
Differential acceptance suite for the RECOVERED prop knock physics in
src/burnout3_props.c.

Every case runs the REAL x86 out of build/burnout3.elf under Unicorn and the
compiled C port from identical state, and asserts they agree field for field.

  setup    FUN_0011A020  (-> FUN_00109BB0 -> FUN_00109190, -> FUN_000FFC80)
           the class-6 body a knocked prop is handed: radius +0x1CC, mass
           +0x1F0, inverse inertia +0x10/+0x40, com height +0x1F4 and the
           PRNG LAUNCH velocity +0xB0/+0xBC/+0xC0.
  step     FUN_0011A330  the whole per-frame body update: the two quadratic
           drag terms and FUN_00109560 (state-6 gravity-at-com torque).
  contact  FUN_00113960's arm for (un-crashed car, prop): FUN_001066A0 point
           velocities, the -0.9 normal bend + FUN_00011640 re-normalise,
           FUN_0010F8D0's two-body impulse and FUN_00106500's negated apply.

Usage: python3 tools/validate_props.py [section ...]
"""
import math
import os
import struct
import subprocess
import sys
import tempfile

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
                     UC_PROT_ALL, UcError)
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX,
                               UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ELF = os.path.join(ROOT, "build", "burnout3.elf")
PAGE = 0x1000

# --- real function addresses (corrected ELF mapping) ------------------------
F_1A020 = 0x0011A020    # class-6 body setup (the promotion tail)
F_1A330 = 0x0011A330    # class-6 body per-frame update (vtable 0x3B1120 +0)
F_09BB0 = 0x00109BB0    # inverse inertia from the bbox + mass
F_066A0 = 0x001066A0    # point velocity
F_0F8D0 = 0x0010F8D0    # two-body contact impulse
F_06500 = 0x00106500    # impulse at a point
F_11640 = 0x00011640    # normalise in place
F_56510 = 0x00156510    # hull build from the bbox record (stubbed)

G_CLOCK   = 0x0060EA20  # DAT_0060EA20
G_RNG_S   = 0x0064ACE8
G_RNG_I   = 0x0064ACEC

# --- synthetic memory -------------------------------------------------------
BODY     = 0x30000000
BODY_SZ  = 0x800
FRAME    = 0x30002000
BBOX     = 0x30003000       # {bbmax vec4, bbmin vec4}
CAR      = 0x30010000
CARFRAME = 0x30012000
SCRATCH  = 0x30020000
REGION   = 0x30000000
REGION_SZ = 0x30000
STACK    = 0x20000000
STACK_SZ = 0x100000
MAGIC    = 0x50000000


def f2b(v):
    return struct.pack('<f', float(v))


def load_elf(uc, path):
    data = open(path, 'rb').read()
    ph_off = struct.unpack_from('<I', data, 0x1C)[0]
    ph_num = struct.unpack_from('<H', data, 0x2C)[0]
    segs = []
    for i in range(ph_num):
        t, off, va, _, fsz, msz, _, _ = struct.unpack_from(
            '<IIIIIIII', data, ph_off + i * 32)
        if t == 1:
            segs.append((va, off, fsz, msz))
    lo = min(s[0] for s in segs) & ~(PAGE - 1)
    hi = max(s[0] + s[3] for s in segs)
    hi = (hi + PAGE - 1) & ~(PAGE - 1)
    uc.mem_map(lo, hi - lo, UC_PROT_ALL)
    for va, off, fsz, _ in segs:
        uc.mem_write(va, data[off:off + fsz])


def _fault(uc, access, address, size, value, user):
    try:
        uc.mem_map(address & ~(PAGE - 1), PAGE * 2, UC_PROT_ALL)
    except UcError:
        return False
    return True


class Session:
    def __init__(self):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        load_elf(self.uc, ELF)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, _fault)
        self.uc.mem_map(REGION, REGION_SZ, UC_PROT_ALL)
        self.uc.mem_map(STACK, STACK_SZ, UC_PROT_ALL)
        self.uc.mem_map(MAGIC & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        self.uc.mem_write(REGION, b'\0' * REGION_SZ)
        # FUN_00156510 builds the prop hull from the bbox record; the impulse
        # chain never reads it, and it walks structures we do not model.
        self.uc.mem_write(F_56510, b'\xC3')                # ret
        # DAT_0054F46C (the PRNG's 2^-32 scale) is seeded at runtime by the
        # initialiser at 0x00264880 from the float at 0x003B191C; the static
        # image has it as 0, which would make every draw come out 0.
        self.uc.mem_write(0x0054F46C, f2b(2.3283064365386963e-10))

    # -- raw helpers --------------------------------------------------------
    def wf(self, a, v):
        self.uc.mem_write(a, f2b(v))

    def rf(self, a):
        return struct.unpack('<f', self.uc.mem_read(a, 4))[0]

    def wv(self, a, v):
        self.uc.mem_write(a, b''.join(f2b(x) for x in v))

    def rv(self, a, n=4):
        return list(struct.unpack('<%df' % n, self.uc.mem_read(a, 4 * n)))

    def wmat(self, a, m):
        for i, row in enumerate(m):
            self.wv(a + i * 16, row)

    def rmat(self, a):
        return [self.rv(a + i * 16) for i in range(4)]

    def call(self, addr, eax=0, ecx=0, edx=0, ebx=0, esi=0, edi=0,
             stack_args=()):
        uc = self.uc
        sp = STACK + STACK_SZ - 0x2000
        for i, v in enumerate(reversed(stack_args)):
            sp -= 4
            uc.mem_write(sp, struct.pack('<I', v & 0xFFFFFFFF))
        sp -= 4
        uc.mem_write(sp, struct.pack('<I', MAGIC))
        uc.reg_write(UC_X86_REG_ESP, sp)
        uc.reg_write(UC_X86_REG_EAX, eax & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_ECX, ecx & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_EDX, edx & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_EBX, ebx & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_ESI, esi & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_EDI, edi & 0xFFFFFFFF)
        uc.emu_start(addr, MAGIC, count=4000000)


# ---------------------------------------------------------------------------
# body seeding (the fields FUN_0011A330 / FUN_0010F8D0 read)
# ---------------------------------------------------------------------------
def invert_rigid(m):
    r = [row[:] for row in m]
    r[0][1], r[1][0] = r[1][0], r[0][1]
    r[0][2], r[2][0] = r[2][0], r[0][2]
    r[1][2], r[2][1] = r[2][1], r[1][2]
    p = [m[3][0] * r[0][j] + m[3][1] * r[1][j] + m[3][2] * r[2][j]
         for j in range(4)]
    r[3] = [-x for x in p]
    return r


def frame_from(yaw, pos, pitch=0.0):
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    return [[cy, 0.0, -sy, 0.0],
            [sy * sp, cp, cy * sp, 0.0],
            [sy * cp, -sp, cy * cp, 0.0],
            [pos[0], pos[1], pos[2], 1.0]]


def seed_body(S, base, frame_addr, frame, st):
    uc = S.uc
    uc.mem_write(base, b'\0' * BODY_SZ)
    S.wmat(frame_addr, frame)
    S.wmat(base + 0x70, invert_rigid(frame))
    uc.mem_write(base + 0x204, struct.pack('<I', frame_addr))
    vel = st.get('vel', [0.0, 0.0, 0.0])
    spd = math.sqrt(sum(v * v for v in vel))
    S.wv(base + 0xB0, list(vel) + [spd])
    if spd > 1e-9:
        S.wv(base + 0xC0, [v / spd for v in vel] + [0.0])
    else:
        S.wv(base + 0xC0, frame[2])
    S.wv(base + 0xD0, list(st.get('omega', [0, 0, 0])) + [0.0])
    S.wv(base + 0xE0, list(st.get('angmom', [0, 0, 0])) + [0.0])
    ii = st['inv_inertia']
    # body inverse inertia is a 3x4 at +0x10; FUN_00109190 puts the diagonal
    # at +0x10 / +0x24 / +0x38, i.e. rows of 4 floats.
    for i in range(3):
        S.wv(base + 0x10 + i * 0x10, ii[i])
    # world inverse inertia rows at +0x40 / +0x50 / +0x60
    for i in range(3):
        S.wv(base + 0x40 + i * 0x10, st['inv_inertia_world'][i])
    S.wf(base + 0x1F0, st['mass'])
    S.wf(base + 0x1F4, st.get('com', 0.0))
    uc.mem_write(base + 0x210, bytes([0]))       # not crashed
    uc.mem_write(base + 0x215, bytes([6]))       # class 6 -> state6 gravity
    uc.mem_write(base + 0x216, bytes([0]))       # inside a loaded unit
    S.wv(base + 0xF0, [0.0] * 4)
    S.wv(base + 0x100, [0.0] * 4)
    S.wv(base + 0x110, [0.0] * 4)
    S.wv(base + 0x120, [0.0] * 4)
    S.wv(base + 0x130, [0.0] * 4)


def diag_inertia(bbmax, bbmin, mass):
    e = [max(bbmax[k], -bbmin[k]) for k in range(3)]
    d = [1.0 / ((e[1]**2 + e[2]**2) * mass * 0.5),
         1.0 / ((e[0]**2 + e[2]**2) * mass * 0.5),
         1.0 / ((e[0]**2 + e[1]**2) * mass * 0.5)]
    return d


def world_inertia(frame, d):
    out = []
    for i in range(3):
        row = []
        for j in range(3):
            row.append(sum(frame[k][i] * d[k] * frame[k][j] for k in range(3)))
        out.append(row + [0.0])
    return out


# ---------------------------------------------------------------------------
# the C driver
# ---------------------------------------------------------------------------
DRIVER = r'''
/* Differential driver for burnout3_props.c (built by validate_props.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "burnout3_props.h"

static float rf(void) { double d; if (scanf("%lf", &d) != 1) exit(2); return (float)d; }
static unsigned ru(void) { unsigned u; if (scanf("%u", &u) != 1) exit(2); return u; }

static void pv(const char* k, const float v[4]) {
    printf("%s %.9g %.9g %.9g %.9g\n", k, v[0], v[1], v[2], v[3]);
}

static void dump_rb(const B3RigidBody* rb) {
    for (int r = 0; r < 4; r++) { char b[16]; sprintf(b, "frame%d", r); pv(b, rb->frame[r]); }
    pv("vel", rb->vel);
    pv("dir", rb->dir);
    pv("omega", rb->omega);
    pv("angmom", rb->angmom);
    for (int r = 0; r < 3; r++) { char b[16]; sprintf(b, "iib%d", r); pv(b, rb->inv_inertia_body[r]); }
    for (int r = 0; r < 3; r++) { char b[16]; sprintf(b, "iiw%d", r); pv(b, rb->inv_inertia_world[r]); }
    pv("impf", rb->imp_force);
    pv("impt", rb->imp_torque);
}

static void read_rb(B3RigidBody* rb) {
    memset(rb, 0, sizeof *rb);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) rb->frame[r][c] = rf();
    for (int c = 0; c < 4; c++) rb->vel[c] = rf();
    for (int c = 0; c < 4; c++) rb->dir[c] = rf();
    for (int c = 0; c < 4; c++) rb->omega[c] = rf();
    for (int c = 0; c < 4; c++) rb->angmom[c] = rf();
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) rb->inv_inertia_body[r][c] = rf();
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) rb->inv_inertia_world[r][c] = rf();
    /* the game rebuilds +0x70 every step; seed it the same way */
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) rb->inv_frame[r][c] = rb->frame[r][c];
}

int main(void) {
    char cmd[64];
    while (scanf("%63s", cmd) == 1) {
        if (!strcmp(cmd, "setup")) {
            float frame[4][4], bbmax[4], bbmin[4];
            for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) frame[r][c] = rf();
            for (int c = 0; c < 4; c++) bbmax[c] = rf();
            for (int c = 0; c < 4; c++) bbmin[c] = rf();
            unsigned s = ru(), i = ru();
            B3RigidBody rb; float mass, com, rad;
            b3_props_test_body_setup(frame, bbmax, bbmin, s, i,
                                     &rb, &mass, &com, &rad);
            printf("mass %.9g\ncom %.9g\nradius %.9g\n", mass, com, rad);
            dump_rb(&rb);
        } else if (!strcmp(cmd, "step")) {
            B3RigidBody rb; read_rb(&rb);
            float mass = rf(), com = rf(), dt = rf();
            int n = (int)rf();
            for (int k = 0; k < n; k++) b3_props_test_body_step(&rb, mass, com, dt);
            dump_rb(&rb);
        } else if (!strcmp(cmd, "contact")) {
            B3RigidBody prb, crb; read_rb(&prb); read_rb(&crb);
            float pmass = rf(), cmass = rf();
            float pt[4], n[4];
            for (int c = 0; c < 4; c++) pt[c] = rf();
            for (int c = 0; c < 4; c++) n[c] = rf();
            float nb[4], imp[4];
            float j = b3_props_test_contact(&prb, pmass, &crb, cmass, pt, n,
                                            nb, imp);
            printf("j %.9g\n", j);
            pv("nbent", nb);
            pv("imp", imp);
            dump_rb(&prb);
        } else {
            fprintf(stderr, "bad cmd %s\n", cmd);
            return 2;
        }
        printf("END\n");
        fflush(stdout);
    }
    return 0;
}
'''


def build_driver():
    d = tempfile.mkdtemp(prefix="b3props_")
    src = os.path.join(d, "drv.c")
    open(src, 'w').write(DRIVER)
    exe = os.path.join(d, "drv")
    sdl = subprocess.run(["pkg-config", "--cflags", "--libs", "sdl2",
                          "SDL2_image"], capture_output=True, text=True)
    cmd = (["gcc", "-O2", "-std=c11", "-I" + os.path.join(ROOT, "src"),
            "-o", exe, src,
            os.path.join(ROOT, "src", "burnout3_props.c"),
            os.path.join(ROOT, "src", "burnout3_vehicle_sim.c"),
            os.path.join(ROOT, "src", "burnout3_collision.c")]
           + sdl.stdout.split() + ["-lGL", "-lm"])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr)
        raise SystemExit("driver build failed")
    return exe


class Driver:
    def __init__(self):
        self.exe = build_driver()
        self.p = subprocess.Popen([self.exe], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)

    def run(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        out = {}
        while True:
            ln = self.p.stdout.readline()
            if not ln:
                raise SystemExit("driver died")
            ln = ln.strip()
            if ln == "END":
                return out
            parts = ln.split()
            out[parts[0]] = [float(x) for x in parts[1:]]


# ---------------------------------------------------------------------------
PASS = 0
FAIL = 0


def chk(name, got, want, tol=2e-4, rel=2e-4):
    global PASS, FAIL
    if not isinstance(got, (list, tuple)):
        got, want = [got], [want]
    ok = len(got) == len(want)
    if ok:
        for a, b in zip(got, want):
            if math.isnan(a) or math.isnan(b):
                ok = False
                break
            if abs(a - b) > tol + rel * max(abs(a), abs(b)):
                ok = False
                break
    if ok:
        PASS += 1
    else:
        FAIL += 1
        g = " ".join("%.6g" % x for x in got)
        w = " ".join("%.6g" % x for x in want)
        print("  FAIL %-34s got=[%s] want=[%s]" % (name, g, w))


def fmt(vals):
    return " ".join("%.9g" % v for v in vals)


def rb_args(frame, vel, dirv, omega, angmom, iib, iiw):
    a = []
    for r in frame:
        a += r
    a += vel + dirv + omega + angmom
    for r in iib:
        a += r
    for r in iiw:
        a += r
    return a


# --- the prop models the shipped tracks actually carry ----------------------
# (bbox half extents in metres; the cone is US_C3_V1's model 0 shape)
CASES = [
    dict(name="cone",     bbmax=[0.21, 0.72, 0.21, 0.0],
                          bbmin=[-0.21, 0.0, -0.21, 0.0]),
    dict(name="board",    bbmax=[1.10, 0.95, 0.12, 0.0],
                          bbmin=[-1.10, 0.0, -0.12, 0.0]),
    dict(name="post",     bbmax=[0.09, 1.05, 0.09, 0.0],
                          bbmin=[-0.09, 0.0, -0.09, 0.0]),
    dict(name="bigbox",   bbmax=[0.90, 0.90, 1.40, 0.0],
                          bbmin=[-0.90, -0.10, -1.40, 0.0]),
]


def sec_setup(S, D):
    print("[setup] FUN_0011A020 -> FUN_00109BB0/FUN_00109190/FUN_000FFC80")
    for ci, c in enumerate(CASES):
        for yaw in (0.0, 0.7):
            frame = frame_from(yaw, [10.0 + ci, 0.0, -30.0])
            rs, ri = 0x12345678, 0x9E3779B9
            # ---- real code ------------------------------------------------
            S.uc.mem_write(BODY, b'\0' * BODY_SZ)
            S.wmat(FRAME, frame)
            S.wv(BBOX, c['bbmax'])
            S.wv(BBOX + 0x10, c['bbmin'])
            S.uc.mem_write(G_RNG_S, struct.pack('<I', rs))
            S.uc.mem_write(G_RNG_I, struct.pack('<I', ri))
            S.wf(G_CLOCK, 0.0)
            # FUN_0011A020(eax = body, ebx = bbox record, edi = matrix,
            #              [ebp+8] = owner)
            S.call(F_1A020, eax=BODY, ebx=BBOX, edi=FRAME, esi=BODY,
                   stack_args=[0x1234])
            r_mass = S.rf(BODY + 0x1F0)
            r_com = S.rf(BODY + 0x1F4)
            r_rad = S.rf(BODY + 0x1CC)
            r_vel = S.rv(BODY + 0xB0)
            r_dir = S.rv(BODY + 0xC0)
            r_iib = [S.rv(BODY + 0x10 + i * 0x10) for i in range(3)]
            r_iiw = [S.rv(BODY + 0x40 + i * 0x10) for i in range(3)]
            r_lru = S.rf(BODY + 0x224)
            # ---- port -----------------------------------------------------
            args = []
            for r in frame:
                args += r
            args += c['bbmax'] + c['bbmin']
            got = D.run("setup " + fmt(args) + " %u %u" % (rs, ri))
            tag = "%s/yaw%.1f" % (c['name'], yaw)
            chk("mass " + tag, got['mass'], [r_mass])
            chk("com " + tag, got['com'], [r_com])
            chk("radius " + tag, got['radius'], [r_rad])
            chk("launch vel " + tag, got['vel'][:3], r_vel[:3])
            chk("launch speed " + tag, got['vel'][3:4], r_vel[3:4])
            chk("launch dir " + tag, got['dir'][:3], r_dir[:3])
            for i in range(3):
                chk("iib%d %s" % (i, tag), got['iib%d' % i][:3], r_iib[i][:3],
                    rel=1e-4)
                chk("iiw%d %s" % (i, tag), got['iiw%d' % i][:3], r_iiw[i][:3],
                    rel=1e-4)
            chk("lru key " + tag, [r_lru], [10.0])
            chk("launch y >= 0 " + tag, [1.0 if r_vel[1] >= 0.0 else 0.0],
                [1.0])


def sec_step(S, D):
    print("[step]  FUN_0011A330 (drag pair + FUN_00109560 state-6 gravity)")
    states = [
        dict(vel=[6.0, 3.0, -2.0], omega=[3.0, 1.0, -4.0], nsteps=1),
        dict(vel=[18.0, 4.5, 1.0], omega=[12.0, -6.0, 2.0], nsteps=1),
        dict(vel=[0.4, 0.0, 0.1], omega=[0.2, 0.0, 0.1], nsteps=1),
        dict(vel=[22.0, 6.0, -9.0], omega=[9.0, 2.0, -3.0], nsteps=20),
    ]
    dt = 1.0 / 60.0
    for ci, c in enumerate(CASES):
        mass = max(100.0, (c['bbmax'][0] - c['bbmin'][0]) *
                          (c['bbmax'][2] - c['bbmin'][2]) * 200.0)
        com = (c['bbmax'][1] + c['bbmin'][1]) * 0.5
        d = diag_inertia(c['bbmax'], c['bbmin'], mass)
        for si, st in enumerate(states):
            frame = frame_from(0.4 * si, [3.0, 5.0, -12.0], pitch=0.15 * si)
            iiw = world_inertia(frame, d)
            iib = [[d[0], 0, 0, 0], [0, d[1], 0, 0], [0, 0, d[2], 0]]
            angmom = [st['omega'][k] / d[k] for k in range(3)]
            seed_body(S, BODY, FRAME, frame,
                      dict(vel=st['vel'], omega=st['omega'], angmom=angmom,
                           inv_inertia=iib, inv_inertia_world=iiw,
                           mass=mass, com=com))
            for _ in range(st['nsteps']):
                S.call(F_1A330, ecx=BODY, stack_args=[struct.unpack(
                    '<I', f2b(dt))[0]])
            r_frame = S.rmat(FRAME)
            r_vel = S.rv(BODY + 0xB0)
            r_om = S.rv(BODY + 0xD0)
            r_am = S.rv(BODY + 0xE0)

            vel = list(st['vel'])
            spd = math.sqrt(sum(v * v for v in vel))
            dirv = ([v / spd for v in vel] + [0.0]) if spd > 1e-9 else frame[2]
            args = rb_args(frame, vel + [spd], dirv,
                           list(st['omega']) + [0.0], angmom + [0.0], iib, iiw)
            got = D.run("step " + fmt(args) +
                        " %.9g %.9g %.9g %d" % (mass, com, dt, st['nsteps']))
            tag = "%s/s%d" % (c['name'], si)
            for r in range(4):
                chk("frame%d %s" % (r, tag), got['frame%d' % r][:3],
                    r_frame[r][:3], tol=5e-4)
            chk("vel " + tag, got['vel'][:3], r_vel[:3])
            chk("speed " + tag, got['vel'][3:4], r_vel[3:4])
            chk("omega " + tag, got['omega'][:3], r_om[:3])
            chk("angmom " + tag, got['angmom'][:3], r_am[:3])


def sec_contact(S, D):
    print("[contact] FUN_00113960 arm: FUN_001066A0 / -0.9 bend / FUN_0010F8D0"
          " / FUN_00106500")
    car_mass = 1200.0
    car_d = [1.0 / 900.0, 1.0 / 1800.0, 1.0 / 1600.0]
    speeds = [8.94, 26.8, 44.7, 67.0]        # 20 / 60 / 100 / 150 mph
    for ci, c in enumerate(CASES):
        pmass = max(100.0, (c['bbmax'][0] - c['bbmin'][0]) *
                           (c['bbmax'][2] - c['bbmin'][2]) * 200.0)
        pd = diag_inertia(c['bbmax'], c['bbmin'], pmass)
        for sp in speeds:
            pframe = frame_from(0.0, [0.0, 0.0, -2.0])
            cframe = frame_from(0.0, [0.0, 0.55, -6.0])
            piiw = world_inertia(pframe, pd)
            ciiw = world_inertia(cframe, car_d)
            piib = [[pd[0], 0, 0, 0], [0, pd[1], 0, 0], [0, 0, pd[2], 0]]
            ciib = [[car_d[0], 0, 0, 0], [0, car_d[1], 0, 0],
                    [0, 0, car_d[2], 0]]
            # the car drives along +z (frame row 2 = at = (0,0,1))
            cvel = [0.0, 0.0, sp]
            point = [0.0, 0.30, -2.20, 0.0]
            normal = [0.0, 0.0, 1.0, 0.0]      # car -> prop

            # ---- real code ------------------------------------------------
            seed_body(S, BODY, FRAME, pframe,
                      dict(vel=[0, 0, 0], omega=[0, 0, 0], angmom=[0, 0, 0],
                           inv_inertia=piib, inv_inertia_world=piiw,
                           mass=pmass, com=0.0))
            seed_body(S, CAR, CARFRAME, cframe,
                      dict(vel=cvel, omega=[0.0, 0.35, 0.0],
                           angmom=[0.0, 0.35 / car_d[1], 0.0],
                           inv_inertia=ciib, inv_inertia_world=ciiw,
                           mass=car_mass, com=0.0))
            S.wv(SCRATCH + 0x00, point)                 # contact point
            # FUN_001066A0(ecx = body, eax = out, [esp+4] = point)
            S.call(F_066A0, ecx=CAR, eax=SCRATCH + 0x10,
                   stack_args=[SCRATCH + 0x00])
            S.call(F_066A0, ecx=BODY, eax=SCRATCH + 0x20,
                   stack_args=[SCRATCH + 0x00])
            vpa = S.rv(SCRATCH + 0x10)
            vpb = S.rv(SCRATCH + 0x20)
            vrel = [vpb[k] - vpa[k] for k in range(4)]
            S.wv(SCRATCH + 0x30, vrel)
            n = list(normal)
            if sum(v * v for v in vrel[:3]) >= 2.3283064365386963e-10:
                S.wv(SCRATCH + 0x40, vrel)
                S.call(F_11640, eax=SCRATCH + 0x40)      # normalise v_rel
                u = S.rv(SCRATCH + 0x40)
                n = [n[k] + u[k] * (-0.9) for k in range(3)] + [0.0]
                S.wv(SCRATCH + 0x50, n)
                S.call(F_11640, eax=SCRATCH + 0x50)
                n = S.rv(SCRATCH + 0x50)
            S.wv(SCRATCH + 0x50, n)
            # FUN_0010F8D0(ecx = bodyA(car), eax = bodyB(prop), args)
            e_bits = 0
            S.call(F_0F8D0, ecx=CAR, eax=BODY,
                   stack_args=[SCRATCH + 0x00, SCRATCH + 0x00, SCRATCH + 0x30,
                               SCRATCH + 0x50, e_bits, SCRATCH + 0x60])
            imp = S.rv(SCRATCH + 0x60)
            j = math.sqrt(sum(x * x for x in imp[:3]))
            # kindA == 2 -> the prop takes -J at the contact point
            S.wv(SCRATCH + 0x70, [-imp[k] for k in range(4)])
            S.call(F_06500, ecx=SCRATCH + 0x70, eax=BODY,
                   stack_args=[SCRATCH + 0x00])
            r_impf = S.rv(BODY + 0x110)
            r_impt = S.rv(BODY + 0x120)
            r_carf = S.rv(CAR + 0x110)

            # ---- port -----------------------------------------------------
            pargs = rb_args(pframe, [0, 0, 0, 0], pframe[2], [0, 0, 0, 0],
                            [0, 0, 0, 0], piib, piiw)
            cspd = math.sqrt(sum(v * v for v in cvel))
            cargs = rb_args(cframe, cvel + [cspd],
                            [v / cspd for v in cvel] + [0.0],
                            [0.0, 0.35, 0.0, 0.0],
                            [0.0, 0.35 / car_d[1], 0.0, 0.0], ciib, ciiw)
            got = D.run("contact " + fmt(pargs) + " " + fmt(cargs) +
                        " %.9g %.9g " % (pmass, car_mass) +
                        fmt(point) + " " + fmt(normal))
            tag = "%s/%.0fmph" % (c['name'], sp * 2.2369363)
            chk("j " + tag, got['j'], [j], rel=1e-4)
            chk("nbent " + tag, got['nbent'][:3], n[:3])
            chk("imp " + tag, got['imp'][:3], imp[:3], rel=1e-4)
            chk("prop +0x110 " + tag, got['impf'][:3], r_impf[:3], rel=1e-4)
            chk("prop +0x120 " + tag, got['impt'][:3], r_impt[:3], rel=1e-4)
            chk("car +0x110 untouched " + tag, r_carf[:3], [0.0, 0.0, 0.0])
            # the launch cap verdict: dv the prop actually receives
            dv = math.sqrt(sum(x * x for x in r_impf[:3])) / pmass
            if abs(sp - 67.0) < 0.1 and c['name'] == 'cone':
                chk("150 mph cone dv < 60 m/s", [1.0 if dv < 60.0 else 0.0],
                    [1.0])




# ---------------------------------------------------------------------------
# --flight: not a test, a REPORT.  Knock a cone at 60 mph and print the flight
# under the recovered law next to the GLUE law it replaced, so the change in
# feel is a number and not a vibe.
# ---------------------------------------------------------------------------
def report_flight(S, D):
    c = CASES[0]                                     # the cone
    pmass = max(100.0, (c['bbmax'][0] - c['bbmin'][0]) *
                       (c['bbmax'][2] - c['bbmin'][2]) * 200.0)
    com = (c['bbmax'][1] + c['bbmin'][1]) * 0.5
    pd = diag_inertia(c['bbmax'], c['bbmin'], pmass)
    piib = [[pd[0], 0, 0, 0], [0, pd[1], 0, 0], [0, 0, pd[2], 0]]
    car_d = [1.0 / 900.0, 1.0 / 1800.0, 1.0 / 1600.0]
    ciib = [[car_d[0], 0, 0, 0], [0, car_d[1], 0, 0], [0, 0, car_d[2], 0]]
    print("cone: mass %.0f kg  com %.3f m  Iinv diag %.4g %.4g %.4g"
          % (pmass, com, pd[0], pd[1], pd[2]))
    print("%8s | %-34s | %-22s" % ("closing", "RECOVERED (FUN_00113960)",
                                   "OLD GLUE"))
    for mph in (20.0, 60.0, 100.0, 150.0):
        sp = mph / 2.2369363
        pframe = frame_from(0.0, [0.0, 0.0, -2.0])
        cframe = frame_from(0.0, [0.0, 0.55, -6.0])
        piiw = world_inertia(pframe, pd)
        ciiw = world_inertia(cframe, car_d)
        cvel = [0.0, 0.0, sp]
        point = [0.0, 0.30, -2.20, 0.0]
        normal = [0.0, 0.0, 1.0, 0.0]
        pargs = rb_args(pframe, [0, 0, 0, 0], pframe[2], [0, 0, 0, 0],
                        [0, 0, 0, 0], piib, piiw)
        cargs = rb_args(cframe, cvel + [sp], [0.0, 0.0, 1.0, 0.0],
                        [0, 0, 0, 0], [0, 0, 0, 0], ciib, ciiw)
        got = D.run("contact " + fmt(pargs) + " " + fmt(cargs) +
                    " %.9g %.9g " % (pmass, 1200.0) +
                    fmt(point) + " " + fmt(normal))
        dv = math.sqrt(sum(x * x for x in got['impf'][:3])) / pmass
        # the law this replaced: j = m*(1+0.35)*vn, +0.45*vn of lift, capped
        # at 30 m/s (burnout3_props.c before this wave)
        old = 1.35 * sp
        old_y = 0.45 * sp
        old_tot = math.sqrt(old * old + old_y * old_y)
        capped = min(old_tot, 30.0)
        print("%6.0f mph | dv %6.2f m/s (j %7.0f N.s), no cap  | dv %6.2f -> "
              "cap %5.2f" % (mph, dv, got['j'][0], old_tot, capped))
    # the drag + gravity-at-com flight itself
    print("\nflight of a 60 mph knock (recovered law, 1/60 s steps):")
    sp = 60.0 / 2.2369363
    pframe = frame_from(0.0, [0.0, 0.0, -2.0])
    piiw = world_inertia(pframe, pd)
    args = rb_args(pframe, [0, 0, 0, 0], pframe[2], [0, 0, 0, 0],
                   [0, 0, 0, 0], piib, piiw)
    cframe = frame_from(0.0, [0.0, 0.55, -6.0])
    ciiw = world_inertia(cframe, car_d)
    cargs = rb_args(cframe, [0.0, 0.0, sp, sp], [0.0, 0.0, 1.0, 0.0],
                    [0, 0, 0, 0], [0, 0, 0, 0], ciib, ciiw)
    got = D.run("contact " + fmt(args) + " " + fmt(cargs) +
                " %.9g %.9g " % (pmass, 1200.0) +
                fmt([0.0, 0.30, -2.20, 0.0]) + " " + fmt([0, 0, 1, 0]))
    vel = [got['impf'][k] / pmass for k in range(3)]
    am = got['impt'][:3]
    om = [am[k] * pd[k] for k in range(3)]
    spd = math.sqrt(sum(v * v for v in vel))
    d = [v / spd for v in vel] if spd > 1e-9 else [0, 0, 1]
    st = rb_args(pframe, vel + [spd], d + [0.0], om + [0.0], am + [0.0],
                 piib, piiw)
    for n in (1, 6, 15, 30, 60, 120):
        r = D.run("step " + fmt(st) + " %.9g %.9g %.9g %d"
                  % (pmass, com, 1.0 / 60.0, n))
        print("  t=%5.2fs  pos (%6.2f %6.2f %6.2f)  |v| %5.2f  |w| %5.2f"
              % (n / 60.0, r['frame3'][0], r['frame3'][1], r['frame3'][2],
                 r['vel'][3],
                 math.sqrt(sum(x * x for x in r['omega'][:3]))))

SECTIONS = {"setup": sec_setup, "step": sec_step, "contact": sec_contact}


def main():
    args = sys.argv[1:]
    flight = "--flight" in args
    args = [a for a in args if a != "--flight"]
    want = args or ([] if flight else list(SECTIONS))
    S = Session()
    D = Driver()
    if flight:
        report_flight(S, D)
        if not want:
            return 0
    for name in want:
        if name not in SECTIONS:
            print("unknown section", name)
            return 2
        SECTIONS[name](S, D)
    print("\n%d/%d passed" % (PASS, PASS + FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
