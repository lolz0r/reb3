#!/usr/bin/env python3
"""
Differential acceptance suite for src/burnout3_carcol.c.

Every case seeds two vehicles, runs the REAL x86 under Unicorn
(tools/emulate_carcol.py) and the compiled C port from identical state, and
asserts the two agree field for field.

  narrow phase      FUN_0010A9D0 -> FUN_0010AC20   contact point / normal /
                                                   per-body separation
  broad phase       FUN_00114270                   world AABB of the box
  impulse           FUN_0010F8D0                   two-body contact impulse
  force routing     FUN_001205E0                   +0xF0 vs +0xF0/+0x100
  racer vs racer    FUN_001121F0                   full response + slam class
  car vs wreck      FUN_00113960                   response + crash threshold

Usage: python3 tools/validate_carcol.py [section]
"""
import math
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import importlib.util
_spec = importlib.util.spec_from_file_location(
    "ec", os.path.join(HERE, "emulate_carcol.py"))
ec = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ec)

# ---------------------------------------------------------------------------
# the C driver -- compiled against src/burnout3_carcol.c
# ---------------------------------------------------------------------------
DRIVER = r'''
/* Differential driver for burnout3_carcol.c (built by validate_carcol.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "burnout3_carcol.h"

static B3RigidBody rbs[2];
static B3CarBody   bodies[2];
static B3CarHull   hulls[2];

static float rf(void) { double d; if (scanf("%lf", &d) != 1) exit(2); return (float)d; }
static int   ri(void) { int i; if (scanf("%d", &i) != 1) exit(2); return i; }

static void pv(const char* k, const float v[4]) {
    printf("%s %.9g %.9g %.9g %.9g\n", k, v[0], v[1], v[2], v[3]);
}
static void pf(const char* k, float v) { printf("%s %.9g\n", k, v); }

static void read_body(int i) {
    char path[512];
    B3CarBody* b = &bodies[i];
    memset(b, 0, sizeof(*b));
    memset(&rbs[i], 0, sizeof(rbs[i]));
    b->rb = &rbs[i];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) rbs[i].frame[r][c] = rf();
    for (int c = 0; c < 4; c++) b->bbmax[c] = rf();
    for (int c = 0; c < 4; c++) b->bbmin[c] = rf();
    b->mass = rf();
    for (int c = 0; c < 3; c++) rbs[i].vel[c] = rf();
    rbs[i].vel[3] = sqrtf(rbs[i].vel[0]*rbs[i].vel[0] + rbs[i].vel[1]*rbs[i].vel[1]
                        + rbs[i].vel[2]*rbs[i].vel[2]);
    for (int c = 0; c < 3; c++) rbs[i].omega[c] = rf();
    rbs[i].omega[3] = 0.0f;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++)
        rbs[i].inv_inertia_world[r][c] = rf();
    b->type        = (unsigned char)ri();
    b->crashed     = (unsigned char)ri();
    b->grounded    = (unsigned char)ri();
    b->asleep      = (unsigned char)ri();
    b->drift_state = ri();
    b->yaw_input   = rf();
    if (scanf("%511s", path) != 1) exit(2);
    if (!b3_carcol_hull_load(path, &hulls[i])) { fprintf(stderr, "hull %s\n", path); exit(3); }
    b->hull = &hulls[i];
    /* inverse frame, same construction as the integrator */
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
        rbs[i].inv_frame[r][c] = rbs[i].frame[r][c];
    {
        float t, (*m)[4] = rbs[i].inv_frame, p[4];
        t = m[0][1]; m[0][1] = m[1][0]; m[1][0] = t;
        t = m[0][2]; m[0][2] = m[2][0]; m[2][0] = t;
        t = m[1][2]; m[1][2] = m[2][1]; m[2][1] = t;
        for (int c = 0; c < 4; c++)
            p[c] = m[3][0]*m[0][c] + m[3][1]*m[1][c] + m[3][2]*m[2][c];
        for (int c = 0; c < 4; c++) m[3][c] = -p[c];
    }
}

static void dump_body(const char* tag, int i) {
    char k[64];
    snprintf(k, sizeof k, "%s.force",      tag); pv(k, rbs[i].force_acc);
    snprintf(k, sizeof k, "%s.torque",     tag); pv(k, rbs[i].torque_acc);
    snprintf(k, sizeof k, "%s.imp_force",  tag); pv(k, rbs[i].imp_force);
    snprintf(k, sizeof k, "%s.imp_torque", tag); pv(k, rbs[i].imp_torque);
    snprintf(k, sizeof k, "%s.deflection", tag); pv(k, rbs[i].deflection);
    snprintf(k, sizeof k, "%s.contact_pt", tag); pv(k, bodies[i].contact_pt);
    printf("%s.touched %d\n", tag, bodies[i].touched);
    printf("%s.hit_side %d\n", tag, bodies[i].hit_side);
}

int main(void) {
    int mode = ri();
    B3CarContact ct;
    if (mode == 3) {                       /* FUN_0010F8D0 in isolation */
        float pt3[4], pt1[4], vrel[4], n[4], out[4], e, m1, m3;
        read_body(0); read_body(1);
        for (int c = 0; c < 4; c++) pt3[c] = rf();
        for (int c = 0; c < 4; c++) pt1[c] = rf();
        for (int c = 0; c < 4; c++) vrel[c] = rf();
        for (int c = 0; c < 4; c++) n[c] = rf();
        e = rf();
        m1 = bodies[1].mass; m3 = bodies[0].mass;
        float j = b3_carcol_mutual_impulse(bodies[1].rb, m1, bodies[0].rb, m3,
                                           pt3, pt1, vrel, n, e, out);
        pv("imp", out); pf("j", j);
        return 0;
    }
    if (mode == 4) {                       /* FUN_001205E0 */
        float f[4], p[4];
        read_body(0); read_body(1);
        for (int c = 0; c < 4; c++) f[c] = rf();
        for (int c = 0; c < 4; c++) p[c] = rf();
        b3_carcol_apply_force(bodies[0].rb, bodies[0].drift_state, f, p);
        dump_body("a", 0);
        return 0;
    }
    read_body(0); read_body(1);
    if (mode == 5) {                       /* FUN_00114270 world AABB */
        float lo[3], hi[3], l4[4], h4[4];
        b3_carcol_world_aabb(&bodies[0], lo, hi);
        for (int c = 0; c < 3; c++) { l4[c] = lo[c]; h4[c] = hi[c]; }
        l4[3] = h4[3] = 0.0f;
        pv("lo", l4); pv("hi", h4);
        return 0;
    }
    if (mode == 0) {
        int hit = b3_carcol_contact(&bodies[0], &bodies[1], &ct);
        printf("hit %d\n", hit);
        if (hit) { pv("point", ct.point); pv("normal", ct.normal);
                   pv("pen_a", ct.pen_a); pv("pen_b", ct.pen_b); }
        return 0;
    }
    int hit = (mode == 1) ? b3_carcol_resolve_alive(&bodies[0], &bodies[1], &ct)
                          : b3_carcol_resolve_wreck(&bodies[0], &bodies[1], &ct);
    printf("hit %d\n", hit);
    if (hit) {
        pv("point", ct.point); pv("normal", ct.normal);
        pf("impact", ct.impact);
        pf("vn_mph", ct.vn_mph);
        printf("slam %d\n", ct.slam_class);
        printf("crash_a %d\n", ct.crash_a);
        printf("crash_b %d\n", ct.crash_b);
        printf("event %d\n", ct.event);
        printf("attacker_is_b %d\n", ct.attacker_is_b);
        pf("strength", ct.strength);
        dump_body("a", 0); dump_body("b", 1);
    }
    return 0;
}
'''

# ---------------------------------------------------------------------------
BUILD = None


def build_driver():
    global BUILD
    d = tempfile.mkdtemp(prefix="carcol_")
    src = os.path.join(d, "carcol_drv.c")
    open(src, "w").write(DRIVER)
    exe = os.path.join(d, "carcol_drv")
    cmd = ["gcc", "-std=c11", "-O2", "-I", os.path.join(ROOT, "src"),
           "-o", exe, src, os.path.join(ROOT, "src", "burnout3_carcol.c"),
           "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout); print(r.stderr)
        raise SystemExit("driver build failed")
    BUILD = exe
    return exe


def run_driver(payload):
    r = subprocess.run([BUILD], input=payload, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("driver failed (%d): %s" % (r.returncode, r.stderr))
    out = {}
    for line in r.stdout.splitlines():
        p = line.split()
        if not p:
            continue
        out[p[0]] = [float(x) for x in p[1:]] if len(p) > 2 else float(p[1])
    return out


def body_payload(st, hullpath):
    v = []
    for row in st['frame']:
        v += list(row)
    v += list(st['bbmax']) + list(st['bbmin']) + [st['mass']]
    v += list(st.get('vel', [0, 0, 0]))
    v += list(st.get('omega', [0, 0, 0]))
    ii = st.get('inv_inertia', DEFAULT_II)
    for row in ii:
        v += list(row)
    s = " ".join("%.9g" % x for x in v)
    s += " %d %d %d %d %d %.9g %s" % (
        st.get('type', 0), st.get('crashed', 0), st.get('grounded', 0),
        st.get('asleep', 0), st.get('drift', 0), st.get('yaw_input', 0.0),
        hullpath)
    return s


DEFAULT_II = [[1.0 / 900, 0, 0, 0], [0, 1.0 / 1800, 0, 0], [0, 0, 1.0 / 1600, 0]]

PASS = 0
FAIL = 0
SECTION = None


def chk(name, got, want, tol=1e-3, rel=1e-3):
    global PASS, FAIL
    ok = True
    if isinstance(want, (list, tuple)):
        if not isinstance(got, (list, tuple)) or len(got) < len(want):
            ok = False
        else:
            for a, b in zip(got, want):
                if abs(a - b) > tol + rel * max(abs(a), abs(b)):
                    ok = False
    else:
        a, b = float(got), float(want)
        if abs(a - b) > tol + rel * max(abs(a), abs(b)):
            ok = False
    if ok:
        PASS += 1
    else:
        FAIL += 1
        print("  FAIL %-28s got=%s want=%s" % (name, got, want))
    return ok


def chk_eq(name, got, want):
    """Exact equality -- for image bytes and integer verdicts."""
    global PASS, FAIL
    if got == want:
        PASS += 1
        return True
    FAIL += 1
    print("  FAIL %-28s got=%s want=%s" % (name, got, want))
    return False


def hull_path(cls, car):
    return os.path.join(ROOT, "build", "cars", "%s_%s.hull" % (cls, car))


def car(cls, car_id, **kw):
    bmax, bmin = ec.bbox(cls, car_id)
    st = dict(hull=ec.load_hull(cls, car_id), bbmax=bmax, bbmin=bmin,
              _cls=cls, _car=car_id)
    st.update(kw)
    return st


# ---------------------------------------------------------------------------
CASES_NARROW = [
    # name, A, B
    ("side-by-side",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 20]),
     dict(pos=(1.4, 0, 0.5), yaw=0.0, mass=900.0, vel=[0, 0, 18])),
    ("nose-to-tail",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 30]),
     dict(pos=(0.05, 0, 3.6), yaw=0.0, mass=900.0, vel=[0, 0, 20])),
    ("angled-side",
     dict(pos=(0, 0, 0), yaw=0.20, mass=800.0, vel=[3, 0, 25]),
     dict(pos=(1.7, 0, 1.1), yaw=-0.15, mass=1100.0, vel=[-2, 0, 22])),
    ("t-bone",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 26]),
     dict(pos=(1.9, 0.05, 1.2), yaw=1.4, mass=1000.0, vel=[-9, 0, 4])),
    ("deep-overlap",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 10]),
     dict(pos=(0.9, 0.02, 0.3), yaw=0.05, mass=900.0, vel=[0, 0, 12])),
    ("rear-into-nose",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 5]),
     dict(pos=(-0.02, 0, -3.7), yaw=0.0, mass=1400.0, vel=[0, 0, 30])),
    ("pitched",
     dict(pos=(0, 0.15, 0), yaw=0.1, pitch=0.12, mass=800.0, vel=[1, 0, 24]),
     dict(pos=(1.6, 0, 0.9), yaw=0.0, mass=900.0, vel=[0, 0, 20])),
    ("no-contact",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 20]),
     dict(pos=(6.0, 0, 0), yaw=0.0, mass=900.0, vel=[0, 0, 20])),
]


def make_state(spec, cls="COMP", cid="Car1"):
    st = car(cls, cid)
    st['frame'] = ec.frame_from(spec.get('yaw', 0.0), spec['pos'],
                                spec.get('pitch', 0.0))
    for k in ('mass', 'vel', 'omega', 'type', 'crashed', 'grounded',
              'asleep', 'drift', 'yaw_input', 'inv_inertia'):
        if k in spec:
            st[k] = spec[k]
    st.setdefault('mass', 800.0)
    return st


def section(name):
    global SECTION
    SECTION = name
    print("\n== %s" % name)


# ---------------------------------------------------------------------------
def run_narrow():
    section("narrow phase (FUN_0010A9D0 -> FUN_0010AC20)")
    for name, sa, sb in CASES_NARROW:
        A = make_state(sa)
        B = make_state(sb, "SUPR", "Car1") if "t-bone" in name else make_state(sb)
        s = ec.Session()
        s.seed(0, A); s.seed(1, B)
        g = s.narrow(0)
        payload = "0\n%s\n%s\n" % (body_payload(A, hull_path(A['_cls'], A['_car'])),
                                   body_payload(B, hull_path(B['_cls'], B['_car'])))
        c = run_driver(payload)
        print(" case %s  (game hit=%d valid=%d)" % (name, g['hit'], g['valid']))
        chk(name + ".hit", c['hit'], 1 if g['valid'] else 0, tol=0)
        if not g['valid']:
            continue
        chk(name + ".point", c['point'], g['point'], tol=1e-4)
        chk(name + ".normal", c['normal'], g['normal'], tol=1e-4)
        dA = [g['posA'][i] - A['frame'][3][i] for i in range(4)]
        dB = [g['posB'][i] - B['frame'][3][i] for i in range(4)]
        chk(name + ".pen_a", c['pen_a'], dA, tol=1e-4)
        chk(name + ".pen_b", c['pen_b'], dB, tol=1e-4)


def run_aabb():
    section("broad phase world AABB (FUN_00114270)")
    specs = [
        ("axis-aligned", dict(pos=(10, 1, -4), yaw=0.0)),
        ("yawed", dict(pos=(-3, 0.5, 7), yaw=0.9)),
        ("yaw+pitch", dict(pos=(2, 2, 2), yaw=-1.3, pitch=0.25)),
    ]
    for name, sp in specs:
        A = make_state(sp)
        B = make_state(dict(pos=(50, 0, 50), yaw=0.0))
        s = ec.Session(); s.seed(0, A); s.seed(1, B)
        g = s.aabb(0)
        payload = "5\n%s\n%s\n" % (body_payload(A, hull_path(A['_cls'], A['_car'])),
                                   body_payload(B, hull_path(B['_cls'], B['_car'])))
        c = run_driver(payload)
        chk(name + ".lo", c['lo'][:3], g['lo'][:3], tol=1e-4)
        chk(name + ".hi", c['hi'][:3], g['hi'][:3], tol=1e-4)


def run_impulse():
    section("two-body impulse (FUN_0010F8D0)")
    cases = [
        ("head-on", [0.5, 0.4, 1.0, 1], [0.5, 0.4, 1.0, 1],
         [-12.0, 0.0, -3.0, 0], [1.0, 0.0, 0.0, 0], 0.1),
        ("offset-lever", [1.0, 0.2, -1.6, 1], [1.0, 0.2, -1.6, 1],
         [4.0, -1.0, 9.0, 0], [0.0, 0.0, 1.0, 0], 0.1),
        ("e0", [0.2, 0.6, 0.3, 1], [0.2, 0.6, 0.3, 1],
         [7.0, 2.0, -1.0, 0], [0.6, 0.0, 0.8, 0], 0.0),
        ("diag-normal", [-0.9, 0.1, 2.0, 1], [-0.9, 0.1, 2.0, 1],
         [-3.0, 0.5, 6.0, 0], [0.57735, 0.57735, 0.57735, 0], 0.35),
    ]
    A = make_state(dict(pos=(0, 0, 0), yaw=0.3, mass=800.0,
                        omega=[0.1, 0.8, -0.2]))
    B = make_state(dict(pos=(1.8, 0, 0.6), yaw=-0.2, mass=1300.0,
                        omega=[-0.3, 1.2, 0.05]))
    s = ec.Session(); s.seed(0, A); s.seed(1, B)
    for name, pt3, pt1, vrel, n, e in cases:
        g = s.impulse(pt3, pt1, vrel, n, e)
        payload = ("3\n%s\n%s\n%s %s %s %s %.9g\n"
                   % (body_payload(A, hull_path(A['_cls'], A['_car'])),
                      body_payload(B, hull_path(B['_cls'], B['_car'])),
                      " ".join("%.9g" % x for x in pt3),
                      " ".join("%.9g" % x for x in pt1),
                      " ".join("%.9g" % x for x in vrel),
                      " ".join("%.9g" % x for x in n), e))
        c = run_driver(payload)
        chk("impulse." + name, c['imp'], g, tol=1e-3, rel=1e-5)


def run_force():
    section("force routing (FUN_001205E0)")
    cases = [
        ("drift-linear", dict(drift=1, omega=[0, 0.5, 0])),
        ("slow-yaw-at-point", dict(drift=0, omega=[0, 0.5, 0])),
        ("fast-yaw-same-sign", dict(drift=0, omega=[0, 6.0, 0])),
        ("fast-yaw-opposite", dict(drift=0, omega=[0, -6.0, 0])),
    ]
    force = [1500.0, 0.0, -400.0, 0.0]
    point = [1.1, 0.3, 1.9, 1.0]
    for name, extra in cases:
        A = make_state(dict(pos=(0, 0, 0), yaw=0.25, mass=800.0, **extra))
        B = make_state(dict(pos=(40, 0, 0), yaw=0.0, mass=900.0))
        s = ec.Session(); s.seed(0, A); s.seed(1, B)
        g = s.apply_force(0, force, point)
        payload = ("4\n%s\n%s\n%s\n%s\n"
                   % (body_payload(A, hull_path(A['_cls'], A['_car'])),
                      body_payload(B, hull_path(B['_cls'], B['_car'])),
                      " ".join("%.9g" % x for x in force),
                      " ".join("%.9g" % x for x in point)))
        c = run_driver(payload)
        chk(name + ".force", c['a.force'], g['force'], tol=1e-3, rel=1e-6)
        chk(name + ".torque", c['a.torque'], g['torque'], tol=1e-2, rel=1e-6)


RESPONSE_CASES = [
    # name, A spec, B spec, B car
    # --- head-on into ONCOMING traffic: the crash-parity boundary -------
    # Retail gates an alive-vs-alive car crash on
    #   |dot(vrel, n)| * 2.236936 ([0x0038994c]) > 150.0 ([0x003ebe4c])
    # (COMISS/JBE @0x0011281e/@0x0011283e in FUN_001121F0).  Nose-to-nose,
    # vn IS the closing speed, so these three pin the threshold from both
    # sides -- and the type-2 case proves retail gives a TRAFFIC body no
    # lower bar than a racer, which is why hitting oncoming traffic below
    # ~160 mph closing legitimately does not crash.
    # --- the TRAFFIC wreck threshold, 2500 vs 5000 ---------------------
    # FUN_00113960 wrecks the racer on `impact > 5000` ([0x003EBE50]),
    # dropping to 2500 ([0x003EBE54]) when an UN-crashed non-traffic car
    # hits a CRASHED traffic car.  These two sit in the discriminating
    # band (impact ~3223): identical geometry, and only the B.type=2 case
    # may crash the racer.  This is retail's cheap cascade -- once a
    # traffic car is wrecked, 15 mph of closing is enough to wreck you.
    ("wreck-vs-crashed-traffic",   # B = TRAFFIC, crashed -> thresh 2500
     dict(pos=(0, 0, 0),   yaw=0.0,       mass=900.0,  vel=[0, 0,  6.71]),
     dict(pos=(0, 0, 3.9), yaw=3.14159265, mass=1200.0, vel=[0, 0, -3.13],
          type=2, crashed=1), None),
    ("wreck-vs-crashed-racer",     # B = RACER, crashed   -> thresh 5000
     dict(pos=(0, 0, 0),   yaw=0.0,       mass=900.0,  vel=[0, 0,  6.71]),
     dict(pos=(0, 0, 3.9), yaw=3.14159265, mass=1200.0, vel=[0, 0, -3.13],
          type=0, crashed=1), None),
    ("headon-under-150",           # 120 + 30 = 150 mph closing -> NO crash
     dict(pos=(0, 0, 0),   yaw=0.0,       mass=900.0,  vel=[0, 0,  53.64]),
     dict(pos=(0, 0, 3.9), yaw=3.14159265, mass=1200.0, vel=[0, 0, -13.41]), None),
    ("headon-over-150",            # 130 + 30 = 160 mph closing -> crash
     dict(pos=(0, 0, 0),   yaw=0.0,       mass=900.0,  vel=[0, 0,  58.11]),
     dict(pos=(0, 0, 3.9), yaw=3.14159265, mass=1200.0, vel=[0, 0, -13.41]), None),
    ("headon-traffic-type2",       # same, B typed as TRAFFIC -> same bar
     dict(pos=(0, 0, 0),   yaw=0.0,       mass=900.0,  vel=[0, 0,  58.11]),
     dict(pos=(0, 0, 3.9), yaw=3.14159265, mass=1200.0, vel=[0, 0, -13.41],
          type=2), None),
    ("lateral-rub",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0.6, 0, 24.0]),
     dict(pos=(1.45, 0, 0.4), yaw=0.0, mass=900.0, vel=[-0.6, 0, 23.0]), None),
    ("hard-side-swipe",
     dict(pos=(0, 0, 0), yaw=0.12, mass=800.0, vel=[8.0, 0, 30.0],
          omega=[0, 0.4, 0], yaw_input=0.3),
     dict(pos=(1.6, 0, 0.6), yaw=-0.05, mass=1200.0, vel=[-4.0, 0, 26.0],
          omega=[0, -0.2, 0], yaw_input=-0.1), None),
    ("nose-into-tail-hi",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 42.0]),
     dict(pos=(0.0, 0, 4.02), yaw=0.0, mass=1100.0, vel=[0, 0, 20.0]), None),
    ("nose-into-tail-lo",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 24.0]),
     dict(pos=(0.0, 0, 4.02), yaw=0.0, mass=1100.0, vel=[0, 0, 20.0]), None),
    # The rear-end branch clamps BOTH longitudinal parameters: the attacker's
    # contact must sit at/past its own bbmax.z and the victim's at/behind its
    # bbmin.z.  COMP/Car4's hull nose overhangs its box by 0.197 and
    # SUPR/Car10's tail by 0.373, so that pair reaches it.
    ("rear-end-slam",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 46.0],
          _car=("COMP", "Car4")),
     dict(pos=(0.0, 0, 4.35), yaw=0.0, mass=1100.0, vel=[0, 0, 20.0]),
     ("SUPR", "Car10")),
    ("rear-end-light",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 33.0],
          _car=("COMP", "Car4")),
     dict(pos=(0.0, 0, 4.35), yaw=0.0, mass=1100.0, vel=[0, 0, 20.0]),
     ("SUPR", "Car10")),
    ("rear-ended-by-b",
     dict(pos=(0, 0, 0), yaw=0.0, mass=1100.0, vel=[0, 0, 20.0],
          _car=("SUPR", "Car10")),
     dict(pos=(0.0, 0, -4.35), yaw=0.0, mass=800.0, vel=[0, 0, 46.0],
          _car=("COMP", "Car4")), None),
    ("t-bone",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 34.0]),
     dict(pos=(1.85, 0.02, 1.4), yaw=1.35, mass=1300.0, vel=[-14.0, 0, 3.0]),
     ("SUPR", "Car1")),
    ("grounded-a",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[2.0, 0, 20.0], grounded=1),
     dict(pos=(1.5, 0, 0.2), yaw=0.0, mass=900.0, vel=[-1.0, 0, 20.0]), None),
    ("grounded-b",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[2.0, 0, 20.0]),
     dict(pos=(1.5, 0, 0.2), yaw=0.0, mass=900.0, vel=[-1.0, 0, 20.0],
          grounded=1), None),
    ("drifting-a",
     dict(pos=(0, 0, 0), yaw=0.1, mass=800.0, vel=[5.0, 0, 28.0], drift=1,
          omega=[0, 1.5, 0]),
     dict(pos=(1.7, 0, 0.5), yaw=0.0, mass=1000.0, vel=[0, 0, 25.0]), None),
    ("heavy-vs-light",
     dict(pos=(0, 0, 0), yaw=0.0, mass=600.0, vel=[6.0, 0, 30.0]),
     dict(pos=(1.55, 0, 0.3), yaw=0.0, mass=2200.0, vel=[-2.0, 0, 28.0]), None),
    ("crash-speed",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[45.0, 0, 20.0]),
     dict(pos=(1.5, 0, 0.2), yaw=0.0, mass=900.0, vel=[-45.0, 0, 20.0]), None),
]


def run_response(mode, label, mkB=None):
    section(label)
    for name, sa, sb, other in RESPONSE_CASES:
        A = make_state(sa, *(sa.get('_car') or ("COMP", "Car1")))
        B = make_state(sb, *(sb.get('_car') or other or ("COMP", "Car1")))
        if mkB:
            mkB(A, B)
        s = ec.Session(); s.seed(0, A); s.seed(1, B)
        g = s.resolve_alive() if mode == 1 else s.resolve_wreck()
        payload = ("%d\n%s\n%s\n" % (mode,
                   body_payload(A, hull_path(A['_cls'], A['_car'])),
                   body_payload(B, hull_path(B['_cls'], B['_car']))))
        c = run_driver(payload)
        chk(name + ".hit", c['hit'], g['hit'], tol=0)
        if not g['hit'] or not c['hit']:
            print("  %-18s no contact" % name)
            continue
        print("  %-18s vn=%6.1f mph impact=%9.1f slam=%d crash=%d%d events=%s"
              % (name, c['vn_mph'], c['impact'], int(c['slam']),
                 int(g['crash_a']), int(g['crash_b']),
                 [(t, 'B' if a == ec.VEH_B else 'A', round(st, 3))
                  for t, a, v, st in g['slams']]))
        chk(name + ".point", c['point'], g['point'], tol=1e-4)
        chk(name + ".normal", c['normal'], g['normal'], tol=1e-4)
        chk(name + ".impact", c['impact'], g['impact'], tol=1e-2, rel=1e-5)
        chk(name + ".slam", c['slam'], g['slam'], tol=0)
        chk(name + ".crash_a", c['crash_a'], g['crash_a'], tol=0)
        chk(name + ".crash_b", c['crash_b'], g['crash_b'], tol=0)
        sl = g['slams'][-1] if g['slams'] else (0, 0, 0, 0.0)
        chk(name + ".event", c['event'], sl[0], tol=0)
        chk(name + ".attacker_is_b", c['attacker_is_b'],
            1 if sl[1] == ec.VEH_B else 0, tol=0)
        chk(name + ".strength", c['strength'], sl[3], tol=1e-5, rel=1e-5)
        for tag, gd in (("a", g['a']), ("b", g['b'])):
            chk("%s.%s.deflection" % (name, tag), c[tag + '.deflection'],
                gd['deflection'], tol=1e-4)
            chk("%s.%s.imp_force" % (name, tag), c[tag + '.imp_force'],
                gd['imp_force'], tol=1e-2, rel=1e-5)
            chk("%s.%s.imp_torque" % (name, tag), c[tag + '.imp_torque'],
                gd['imp_torque'], tol=1e-2, rel=1e-5)
            chk("%s.%s.force" % (name, tag), c[tag + '.force'],
                gd['force'], tol=1e-2, rel=1e-5)
            chk("%s.%s.torque" % (name, tag), c[tag + '.torque'],
                gd['torque'], tol=1e-1, rel=1e-5)
            chk("%s.%s.contact_pt" % (name, tag), c[tag + '.contact_pt'],
                gd['contact_pt'], tol=1e-4)
            chk("%s.%s.touched" % (name, tag), c[tag + '.touched'],
                gd['touched'], tol=0)
            chk("%s.%s.hit_side" % (name, tag), c[tag + '.hit_side'],
                gd['hit_side'], tol=0)


def wreck_setup(A, B):
    B['crashed'] = 1


# ---------------------------------------------------------------------------
# THE RACING GATHER'S TWO RUNTIME FILTERS -- FUN_0011BBE0, over the REAL
# build/collision.bin.  Not car-vs-car, but the same collision boundary:
# b3_sweep_sphere_ex is what feeds FUN_0011AEF0's wall trigger and
# FUN_00112E70's object trigger with a contact.
# ---------------------------------------------------------------------------
GATHER_DRIVER = r'''
#include <stdio.h>
#include <string.h>
#include "burnout3_collision.h"

/* stdin: "sweep x y z r nymax usev vx vy vz" -> "res hit type nx ny nz" */
int main(int argc, char** argv) {
    if (b3_collision_load(argc > 1 ? argv[1] : "build/collision.bin") <= 0) {
        fprintf(stderr, "no collision.bin\n");
        return 1;
    }
    char cmd[32];
    while (scanf("%31s", cmd) == 1) {
        if (!strcmp(cmd, "sweep")) {
            float x, y, z, r, ny, vx, vy, vz, q[3], n[3];
            unsigned short ty = 0;
            int usev;
            if (scanf("%f %f %f %f %f %d %f %f %f",
                      &x, &y, &z, &r, &ny, &usev, &vx, &vy, &vz) != 9) break;
            float p[3] = {x, y, z};
            float v[3] = {vx, vy, vz};
            int hit = b3_sweep_sphere_ex(p, p, r, ny, usev ? v : NULL,
                                         q, n, &ty);
            printf("res %d %u %.5f %.5f %.5f\n", hit, (unsigned)ty,
                   hit ? n[0] : 0.0f, hit ? n[1] : 0.0f, hit ? n[2] : 0.0f);
        } else if (!strcmp(cmd, "quit")) {
            break;
        }
        fflush(stdout);
    }
    return 0;
}
'''


def _elf_reader():
    elf = open(os.path.join(ROOT, "build", "burnout3.elf"), "rb").read()
    ph_off = struct.unpack_from('<I', elf, 0x1C)[0]
    ph_num = struct.unpack_from('<H', elf, 0x2C)[0]
    segs = [s for s in (struct.unpack_from('<IIIIIIII', elf, ph_off + i * 32)
                        for i in range(ph_num)) if s[0] == 1]

    def rd(va, n):
        for t, o, v, _p, f, _m, _fl, _a in segs:
            if v <= va < v + f:
                return elf[o + (va - v):o + (va - v) + n]
        return b'\0' * n
    return rd


def run_gather():
    """FUN_0011BBE0's two runtime filters against the real collision world."""
    section("racing gather runtime filters (FUN_0011BBE0)")
    rd = _elf_reader()
    # ---- the predicate, straight out of the instruction bytes -----------
    chk("0x003B1684 gather velocity limit = 0.5",
        struct.unpack('<f', rd(0x003B1684, 4))[0], 0.5, 1e-9)
    chk("0x0039B264 gather normal.y floor = -0.7",
        struct.unpack('<f', rd(0x0039B264, 4))[0], -0.7, 1e-6)
    chk_eq("0x0011BBFE CMP low, 0x23", rd(0x0011BBFE, 3), b'\x83\xf8\x23')
    chk_eq("0x0011BC03 CMP low, 0x22", rd(0x0011BC03, 3), b'\x83\xf8\x22')
    chk_eq("0x0011BC08 TEST ch, 0x10 (type & 0x1000)",
           rd(0x0011BC08, 3), b'\xf6\xc5\x10')
    chk_eq("0x0011BC0D CMP low, 0x15", rd(0x0011BC0D, 3), b'\x83\xf8\x15')
    chk_eq("0x0011BC15 CMP low, 0x20", rd(0x0011BC15, 3), b'\x83\xf8\x20')
    chk_eq("0x0011BC1A reads veh+0xB0 (the car's OWN velocity)",
           rd(0x0011BC1A, 7), b'\x0f\x28\x86\xb0\x00\x00\x00')
    chk_eq("0x0011BC21 takes the record's normal at +0x10",
           rd(0x0011BC21, 3), b'\x8d\x47\x10')
    chk_eq("0x0011BC2D CALL FUN_00013C60 (the dot)", rd(0x0011BC2D, 1),
           b'\xe8')
    chk_eq("0x0011BC32 COMISS the dot with [0x003B1684]",
           rd(0x0011BC32, 7), b'\x0f\x2f\x05\x84\x16\x3b\x00')
    chk_eq("0x0011BC3B loads [0x0039B264]", rd(0x0011BC3B, 8),
           b'\xf3\x0f\x10\x05\x64\xb2\x39\x00')
    chk_eq("0x0011BC43 COMISS it against normal.y (record+0x14)",
           rd(0x0011BC43, 4), b'\x0f\x2f\x47\x14')
    chk_eq("0x0011BC4B CALL FUN_0010A8E0 (append to the soup)",
           rd(0x0011BC4B, 1), b'\xe8')

    binpath = os.path.join(ROOT, "build", "collision.bin")
    if not os.path.exists(binpath):
        print("  (build/collision.bin missing -- behaviour cases skipped)")
        return
    d = tempfile.mkdtemp(prefix="gather_")
    src = os.path.join(d, "gather_drv.c")
    open(src, "w").write(GATHER_DRIVER)
    exe = os.path.join(d, "gather_drv")
    r = subprocess.run(
        ["gcc", "-std=c11", "-O2", "-I", os.path.join(ROOT, "src"),
         "-o", exe, src, os.path.join(ROOT, "src", "burnout3_collision.c"),
         "-lm"], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout); print(r.stderr)
        raise SystemExit("gather driver build failed")

    data = open(binpath, "rb").read()
    ntri = struct.unpack_from('<I', data, 8)[0]

    def tri(i):
        o = 0x28 + i * 40
        v = struct.unpack_from('<9f', data, o)
        ty = struct.unpack_from('<H', data, o + 36)[0]
        p0 = (v[0], v[1], -v[2])          # loader: negate z, swap v1/v2
        p1 = (v[6], v[7], -v[8])
        p2 = (v[3], v[4], -v[5])
        e1 = [p1[k] - p0[k] for k in range(3)]
        e2 = [p2[k] - p0[k] for k in range(3)]
        n = [e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2],
             e1[0]*e2[1] - e1[1]*e2[0]]
        ln = math.sqrt(sum(c * c for c in n)) or 1.0
        n = [c / ln for c in n]
        c = [(p0[k] + p1[k] + p2[k]) / 3.0 for k in range(3)]
        return ty, n, c

    def sweeps(rows):
        payload = "".join(
            "sweep %.5f %.5f %.5f %.3f 1.1 %d %.5f %.5f %.5f\n"
            % (p[0], p[1], p[2], rad, usev, v[0], v[1], v[2])
            for p, rad, usev, v in rows) + "quit\n"
        o = subprocess.run([exe, binpath], input=payload,
                           capture_output=True, text=True)
        return [l.split() for l in o.stdout.splitlines()
                if l.startswith("res ")]

    # Pick ISOLATED representatives: a candidate only qualifies if a
    # velocity-free probe 0.35 in front of its face wins with ITS OWN type
    # (otherwise a neighbouring poly answers and the filter is untestable).
    RAD = 0.5
    OFF = 0.35

    def pick(pred, limit=400):
        cands = []
        for i in range(ntri):
            ty, n, c = tri(i)
            if abs(n[1]) < 0.2 and pred(ty & 0xFF):
                cands.append((ty, n, c))
                if len(cands) >= limit:
                    break
        if not cands:
            return None
        rows = [([c[k] + n[k] * OFF for k in range(3)], RAD, 0, (0, 0, 0))
                for ty, n, c in cands]
        got = sweeps(rows)
        for k, (ty, n, c) in enumerate(cands):
            if k < len(got) and int(got[k][1]) == 1 and int(got[k][2]) == ty:
                return (ty, n, c)
        return None

    struct_i = pick(lambda lo: 0x15 <= lo < 0x20)
    chevron_i = pick(lambda lo: lo == 0x20)
    plain_i = pick(lambda lo: lo < 0x15)

    lines, cases = [], []
    rows = []

    def q(c, n, usev, v):
        # sit the sphere just in FRONT of the face so the one-sided test passes
        rows.append(([c[k] + n[k] * OFF for k in range(3)], RAD, usev, v))

    # A dense soup means "the face was dropped" shows up as "it no longer
    # WINS the sweep", not necessarily as "nothing was hit" -- a neighbour
    # may answer instead.  `is_ty` asserts the face won, `not_ty` that it did
    # not.
    if chevron_i:
        ty, n, c = chevron_i
        q(c, n, 0, (0, 0, 0))
        cases.append(("chevron board (0x20) still collides", "is_ty", ty))
        q(c, n, 1, [-n[k] * 20.0 for k in range(3)])
        cases.append(("chevron board, driving INTO it: collides",
                      "is_ty", ty))
        q(c, n, 1, [n[k] * 20.0 for k in range(3)])
        cases.append(("chevron board, separating at 20 m/s: dropped",
                      "not_ty", ty))
    if struct_i:
        ty, n, c = struct_i
        lo = ty & 0xFF
        q(c, n, 1, [-n[k] * 20.0 for k in range(3)])
        cases.append(("armco 0x%02X, driving INTO it: collides" % lo,
                      "is_ty", ty))
        q(c, n, 1, [n[k] * 20.0 for k in range(3)])
        cases.append(("armco 0x%02X, separating at 20 m/s: dropped (no snag)"
                      % lo, "not_ty", ty))
        q(c, n, 1, [n[k] * 0.4 for k in range(3)])
        cases.append(("armco, separating at only 0.4 m/s (< 0.5): kept",
                      "is_ty", ty))
    if plain_i:
        ty, n, c = plain_i
        q(c, n, 1, [n[k] * 20.0 for k in range(3)])
        cases.append(("a non-structure surface 0x%02X ignores filter (a)"
                      % (ty & 0xFF), "is_ty", ty))
    res = sweeps(rows)
    for k, (name, kind, ty) in enumerate(cases):
        if k >= len(res):
            chk_eq(name, "missing", "a res line")
            continue
        hit, got_ty = int(res[k][1]), int(res[k][2])
        if kind == "is_ty":
            chk_eq(name, (hit, got_ty), (1, ty))
        else:
            chk_eq(name, hit and got_ty == ty, False)

    # filter (b): a downward-facing face may never block, whatever
    # wall_ny_max the caller passes.
    down = [i for i in range(ntri) if tri(i)[1][1] < -0.7]
    if down:
        rows = []
        for i in down[:40]:
            ty, n, c = tri(i)
            q(c, n, 0, (0, 0, 0))
        res = sweeps(rows)
        hits = [x for x in res if int(x[1]) == 1 and int(x[2]) == 0]
        chk_eq("normal.y < -0.7 faces never win a sweep (%d sampled of %d)"
               % (len(res), len(down)), len(hits), 0)


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    build_driver()
    if which in ("all", "gather"):   run_gather()
    if which in ("all", "narrow"):   run_narrow()
    if which in ("all", "aabb"):     run_aabb()
    if which in ("all", "impulse"):  run_impulse()
    if which in ("all", "force"):    run_force()
    if which in ("all", "alive"):    run_response(1, "racer vs racer (FUN_001121F0)")
    if which in ("all", "wreck"):    run_response(2, "car vs wreck (FUN_00113960)",
                                                  wreck_setup)
    print("\n%d/%d passed" % (PASS, PASS + FAIL))
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
