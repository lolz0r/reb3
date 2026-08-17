#!/usr/bin/env python3
"""
Differential acceptance test for src/burnout3_td_rules.c.

Every case seeds a scenario, runs the REAL retail chain under Unicorn
(tools/emulate_td_rules.py) and the compiled C module from identical state,
and diffs the observable result: the slam stamps, whether the crash entry was
ever reached, the attribution stamps, the commit timing, the DENIED / LUCKY
ESCAPE outcomes and the takedown message selection.

The C side is driven by a small command-line harness compiled from the module
(source embedded below, built into the scratch dir).

Usage:  python3 tools/validate_td_rules.py [section]
"""
import os
import subprocess
import struct
import sys
import tempfile

_here = os.path.dirname(os.path.abspath(__file__))
_root = os.path.dirname(_here)
sys.path.insert(0, _here)

import emulate_td_rules as E                                   # noqa: E402

PASS = 0
FAIL = 0
FAILURES = []


def check(name, got, want, tol=None):
    global PASS, FAIL
    ok = (abs(got - want) <= tol) if (tol is not None) else (got == want)
    if ok:
        PASS += 1
    else:
        FAIL += 1
        FAILURES.append("%-58s got %r want %r" % (name, got, want))


# ==========================================================================
# the C driver
# ==========================================================================
DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "burnout3_td_rules.h"
#include "burnout3_crash.h"

/* burnout3_crash.c is linked in for section 9's wall trigger; its crashed-
 * path solver also references the integrator, which this driver never runs.
 * Stubbed so the link stays down to the two modules under test. */
void b3_rigid_body_integrate(struct B3RigidBody* rb, float mass_kg,
                             float com_height, int in_race, int state6,
                             float dt) {
    (void)rb; (void)mass_kg; (void)com_height;
    (void)in_race; (void)state6; (void)dt;
}

static B3TdRules R;
static float POS[B3_TDR_MAX_CARS][3];
static int use_pos = 0;

static void dump(int s) {
    B3TdCar* c = &R.car[s];
    int i;
    printf("slam_time=%.6f slam_type=%d aggressor=%d aggressor_time=%.6f "
           "slams_made=%d times_slammed=%d td_credited=%d td_by=%d "
           "revenge=%d recover_at=%.6f bp=%d td_count=%d last_victim=%d "
           "denied=%d lucky=%d dbl_window=%.6f dbl_count=%d "
           "spree_window=%.6f spree_count=%d at_count=%d",
           c->slam_time, c->slam_type, c->aggressor, c->aggressor_time,
           c->slams_made, c->times_slammed, c->td_credited, c->td_by,
           c->revenge_flag, c->recover_at, c->bp, c->td_count, c->last_victim,
           c->denied_pending, c->lucky_pending, c->dbl_window, c->dbl_count,
           c->spree_window, c->spree_count, c->aftertouch_count);
    printf(" claim=");
    for (i = 0; i < R.ncars; i++) printf("%.6f,", c->claim[i]);
    printf(" claim_at=");
    for (i = 0; i < R.ncars; i++) printf("%d,", c->claim_aftertouch[i]);
    printf(" claim_ps=");
    for (i = 0; i < R.ncars; i++) printf("%d,", c->claim_psyche[i]);
    printf("\n");
}

int main(void) {
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        char cmd[32];
        if (sscanf(line, "%31s", cmd) != 1) continue;
        if (!strcmp(cmd, "reset")) {
            int n; sscanf(line, "%*s %d", &n);
            b3_td_reset(&R, n); use_pos = 0;
            memset(POS, 0, sizeof(POS));
        } else if (!strcmp(cmd, "car")) {
            int s, cls, g; sscanf(line, "%*s %d %d %d", &s, &cls, &g);
            b3_td_set_car(&R, s, cls, g);
        } else if (!strcmp(cmd, "set")) {
            char f[32]; int s; double v;
            sscanf(line, "%*s %d %31s %lf", &s, f, &v);
            B3TdCar* c = &R.car[s];
            if (!strcmp(f, "crashed")) c->crashed = (int)v;
            else if (!strcmp(f, "race_state")) c->race_state = (int)v;
            else if (!strcmp(f, "speed_ms")) c->speed_ms = (float)v;
            else if (!strcmp(f, "slam_time")) c->slam_time = (float)v;
            else if (!strcmp(f, "aggressor")) c->aggressor = (int)v;
            else if (!strcmp(f, "aggressor_time")) c->aggressor_time = (float)v;
            else if (!strcmp(f, "psyche_armed")) c->psyche_armed = (int)v;
            else if (!strcmp(f, "psyche_target")) c->psyche_target = (int)v;
            else if (!strcmp(f, "td_credited")) c->td_credited = (int)v;
            else if (!strcmp(f, "crash_time")) c->crash_time = (float)v;
            else if (!strcmp(f, "dbl_window")) c->dbl_window = (float)v;
            else if (!strcmp(f, "dbl_count")) c->dbl_count = (int)v;
            else if (!strcmp(f, "spree_window")) c->spree_window = (float)v;
            else if (!strcmp(f, "spree_count")) c->spree_count = (int)v;
            else if (!strcmp(f, "at_count")) c->aftertouch_count = (int)v;
            else { fprintf(stderr, "bad field %s\n", f); exit(2); }
        } else if (!strcmp(cmd, "setidx")) {
            char f[32]; int s, k; double v;
            sscanf(line, "%*s %d %31s %d %lf", &s, f, &k, &v);
            B3TdCar* c = &R.car[s];
            if (!strcmp(f, "claim")) c->claim[k] = (float)v;
            else if (!strcmp(f, "claim_at")) c->claim_aftertouch[k] = (int)v;
            else if (!strcmp(f, "claim_ps")) c->claim_psyche[k] = (int)v;
            else if (!strcmp(f, "contact")) c->contact_time[k] = (float)v;
            else if (!strcmp(f, "tdby")) c->taken_down_by[k] = (int)v;
            else { fprintf(stderr, "bad idx field %s\n", f); exit(2); }
        } else if (!strcmp(cmd, "pos")) {
            int s; double x, y, z;
            sscanf(line, "%*s %d %lf %lf %lf", &s, &x, &y, &z);
            POS[s][0] = (float)x; POS[s][1] = (float)y; POS[s][2] = (float)z;
            use_pos = 1;
        } else if (!strcmp(cmd, "slam")) {
            double t, st; int kind, a, v;
            sscanf(line, "%*s %lf %d %d %d %lf", &t, &kind, &a, &v, &st);
            printf("ret %d\n", b3_td_slam_report(&R, (float)t, kind, a, v,
                                                 (float)st));
        } else if (!strcmp(cmd, "contact")) {
            double t, dt; int a, b, tch;
            sscanf(line, "%*s %lf %lf %d %d %d", &t, &dt, &a, &b, &tch);
            b3_td_contact(&R, (float)t, (float)dt, a, b, tch);
        } else if (!strcmp(cmd, "notify")) {
            double t; int s; sscanf(line, "%*s %lf %d", &t, &s);
            b3_td_contact_notify(&R, (float)t, s);
        } else if (!strcmp(cmd, "crash")) {
            double t; int s, wall, hasobj, vclass, wreck;
            B3TdCause c;
            sscanf(line, "%*s %lf %d %d %d %d %d", &t, &s, &wall, &hasobj,
                   &vclass, &wreck);
            if (wall) b3_td_cause_wall(&c, 0);
            else if (hasobj) b3_td_cause_object(&c, vclass);
            else if (wreck >= 0) b3_td_cause_wreck(&c, wreck);
            else b3_td_cause_none(&c);
            b3_td_on_crash(&R, (float)t, s,
                           (wall || hasobj || wreck >= 0) ? &c : NULL,
                           use_pos ? POS : NULL);
        } else if (!strcmp(cmd, "frame")) {
            double t; int s, i, n; B3TdEvent ev[8];
            sscanf(line, "%*s %lf %d", &t, &s);
            n = b3_td_frame(&R, (float)t, s, ev, 8);
            printf("events %d\n", n);
            for (i = 0; i < n; i++)
                printf("ev %d %d %d %d %d %d %d %d\n", ev[i].kind,
                       ev[i].attacker, ev[i].victim, ev[i].message, ev[i].bp,
                       ev[i].owner, ev[i].aftertouch, ev[i].revenge);
        } else if (!strcmp(cmd, "msg")) {
            int a, v, at, ps;
            sscanf(line, "%*s %d %d %d %d", &a, &v, &at, &ps);
            printf("msg %d\n", b3_td_select_message(&R, a, v, at, ps));
        } else if (!strcmp(cmd, "ooc")) {
            int s; double t; sscanf(line, "%*s %d %lf", &s, &t);
            printf("ooc %d\n", b3_td_out_of_control(&R, s, (float)t));
        } else if (!strcmp(cmd, "auth")) {
            int s; double t; B3TdAuthority a;
            sscanf(line, "%*s %d %lf", &s, &t);
            b3_td_crash_authority_full(&R, s, (float)t, &a);
            printf("auth %.7f %d %d %d\n", a.value, a.crash_ok, a.in_range,
                   a.slammed);
        } else if (!strcmp(cmd, "view")) {
            int s, alt; double d2;
            sscanf(line, "%*s %d %lf %d", &s, &d2, &alt);
            R.car[s].view_dist2 = (float)d2;
            R.car[s].view_radius_alt = alt;
        } else if (!strcmp(cmd, "crashmode")) {
            int m; sscanf(line, "%*s %d", &m); R.crash_mode = m;
        } else if (!strcmp(cmd, "psyche")) {
            int s, v; sscanf(line, "%*s %d %d", &s, &v);
            R.car[s].psyche_armed = v;
        } else if (!strcmp(cmd, "wall")) {
            /* one world contact through the ported FUN_0011AEF0 tail */
            int s; double t, px, py, pz, nx, ny, nz, vx, vy, vz, mass, spd;
            B3CrashVehicle cv; B3TdWallHit wh; int k;
            sscanf(line, "%*s %d %lf %lf %lf %lf %lf %lf %lf "
                   "%lf %lf %lf %lf %lf",
                   &s, &t, &px, &py, &pz, &nx, &ny, &nz,
                   &vx, &vy, &vz, &mass, &spd);
            memset(&cv, 0, sizeof(cv));
            for (k = 0; k < 4; k++) cv.frame[k][k] = 1.0f;
            /* a ~1200 kg car's inverse inertia, so the impulse's
             * angular term has a realistic weight */
            for (k = 0; k < 3; k++)
                cv.iinv_world[k][k] = 1.0f / 2000.0f;
            cv.vel[0] = (float)vx; cv.vel[1] = (float)vy;
            cv.vel[2] = (float)vz; cv.vel[3] = (float)spd;
            if (cv.vel[3] > 1e-6f)
                for (k = 0; k < 3; k++) cv.dir[k] = cv.vel[k] / cv.vel[3];
            cv.mass = (float)mass;
            cv.bbmax[0] = 1.0f; cv.bbmax[1] = 0.6f; cv.bbmax[2] = 2.2f;
            cv.bbmin[0] = -1.0f; cv.bbmin[1] = -0.6f; cv.bbmin[2] = -2.2f;
            cv.ground_frac = 1.0f;
            cv.surface = 0x0001;
            {
                float pt[4], nn[4];
                pt[0] = (float)px; pt[1] = (float)py; pt[2] = (float)pz;
                pt[3] = 0.0f;
                nn[0] = (float)nx; nn[1] = (float)ny; nn[2] = (float)nz;
                nn[3] = 0.0f;
                b3_td_wall_contact(&R, s, (float)t, &cv, pt, nn, 0.0f);
            }
            b3_td_wall_take(&R, s, &wh);
            printf("wall %d %.7f %.7f %.7f %.7f %.7f\n", wh.fire, wh.dv,
                   wh.headon, wh.dv_thr, wh.headon_thr, wh.authority);
        } else if (!strcmp(cmd, "resp")) {
            /* FUN_0011AEF0's CHASSIS RESPONSE over the same contact -- the
             * two scrubs + the e=0 point impulse (header section 9b).  The
             * frame is the identity at the origin, so a contact offset in x
             * is at a flank and one offset in z at the nose/tail, which is
             * what makes the yaw term visible. */
            double t, px, py, pz, nx, ny, nz, vx, vy, vz, mass, spd, brake;
            B3CrashVehicle cv; B3TdWallResponse rp; int k;
            sscanf(line, "%*s %lf %lf %lf %lf %lf %lf %lf "
                   "%lf %lf %lf %lf %lf %lf",
                   &t, &px, &py, &pz, &nx, &ny, &nz,
                   &vx, &vy, &vz, &mass, &spd, &brake);
            (void)t;
            memset(&cv, 0, sizeof(cv));
            for (k = 0; k < 4; k++) cv.frame[k][k] = 1.0f;
            for (k = 0; k < 3; k++) cv.iinv_world[k][k] = 1.0f / 2000.0f;
            cv.vel[0] = (float)vx; cv.vel[1] = (float)vy;
            cv.vel[2] = (float)vz; cv.vel[3] = (float)spd;
            if (cv.vel[3] > 1e-6f)
                for (k = 0; k < 3; k++) cv.dir[k] = cv.vel[k] / cv.vel[3];
            cv.mass = (float)mass;
            cv.bbmax[0] = 1.0f; cv.bbmax[1] = 0.6f; cv.bbmax[2] = 2.2f;
            cv.bbmin[0] = -1.0f; cv.bbmin[1] = -0.6f; cv.bbmin[2] = -2.2f;
            cv.ground_frac = (float)brake;
            cv.surface = 0x0001;
            {
                float pt[4], nn[4];
                pt[0] = (float)px; pt[1] = (float)py; pt[2] = (float)pz;
                pt[3] = 0.0f;
                nn[0] = (float)nx; nn[1] = (float)ny; nn[2] = (float)nz;
                nn[3] = 0.0f;
                b3_td_wall_response(&cv, pt, nn, &rp);
            }
            printf("resp %d %.7f %.7f %.7f %.7f %.7f %.7f %.7f %.7f %d\n",
                   rp.valid, rp.scrub, rp.impact,
                   rp.imp[0], rp.imp[1], rp.imp[2],
                   rp.ang_imp[0], rp.ang_imp[1], rp.ang_imp[2],
                   rp.at_point);
        } else if (!strcmp(cmd, "objclass")) {
            int ty, dt; sscanf(line, "%*s %d %d", &ty, &dt);
            printf("objclass %d\n", b3_td_object_class(ty, dt));
        } else if (!strcmp(cmd, "objtable")) {
            int cb, ca; sscanf(line, "%*s %d %d", &cb, &ca);
            printf("objtable %d\n", b3_td_object_crashable(cb, ca));
        } else if (!strcmp(cmd, "obj")) {
            /* FUN_00112E70's object trigger over one contact */
            int s, ocls, ccls, immune;
            double t, vx, vy, vz, nx, ny, nz, mass;
            B3TdObjectHit oh;
            sscanf(line, "%*s %d %lf %lf %lf %lf %lf %lf %lf %lf %d %d %d",
                   &s, &t, &vx, &vy, &vz, &nx, &ny, &nz, &mass,
                   &ocls, &ccls, &immune);
            {
                float vr[3], nn[3];
                vr[0] = (float)vx; vr[1] = (float)vy; vr[2] = (float)vz;
                nn[0] = (float)nx; nn[1] = (float)ny; nn[2] = (float)nz;
                b3_td_object_contact(&R, s, (float)t, vr, nn, (float)mass,
                                     ocls, ccls, immune);
            }
            b3_td_object_take(&R, s, &oh);
            printf("obj %d %d %.6f %.6f %.6f %.4f\n", oh.valid, oh.fire,
                   oh.closing_mph, oh.thr, oh.authority, oh.damage);
        } else if (!strcmp(cmd, "dump")) {
            int s; sscanf(line, "%*s %d", &s); dump(s);
        } else if (!strcmp(cmd, "quit")) {
            break;
        }
        fflush(stdout);
    }
    return 0;
}
'''

SCRATCH = os.environ.get(
    'B3_TDR_SCRATCH',
    '/tmp/claude-1000/-home-lolz0r-burnout3/'
    'df62ed7e-f72a-446d-bebd-33653e64dd3d/scratchpad/tdrules')


class CDriver:
    def __init__(self):
        os.makedirs(SCRATCH, exist_ok=True)
        src = os.path.join(SCRATCH, 'td_driver.c')
        exe = os.path.join(SCRATCH, 'td_driver')
        with open(src, 'w') as f:
            f.write(DRIVER)
        cmd = ['gcc', '-O1', '-std=c99', '-Wall', '-I', os.path.join(_root, 'src'),
               src, os.path.join(_root, 'src', 'burnout3_td_rules.c'),
               # section 9's wall trigger calls straight into the ported
               # FUN_0011AEF0 tail
               os.path.join(_root, 'src', 'burnout3_crash.c'),
               '-o', exe, '-lm']
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
            raise SystemExit("driver build failed")
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True,
                                  bufsize=1)

    def cmd(self, s):
        self.p.stdin.write(s + "\n")
        self.p.stdin.flush()

    def ask(self, s):
        self.cmd(s)
        return self.p.stdout.readline().strip()

    def events(self, clock, slot):
        self.cmd("frame %.6f %d" % (clock, slot))
        n = int(self.p.stdout.readline().split()[1])
        out = []
        for _ in range(n):
            parts = self.p.stdout.readline().split()
            out.append(dict(kind=int(parts[1]), attacker=int(parts[2]),
                            victim=int(parts[3]), message=int(parts[4]),
                            bp=int(parts[5]), owner=int(parts[6]),
                            aftertouch=int(parts[7]), revenge=int(parts[8])))
        return out

    def dump(self, slot):
        line = self.ask("dump %d" % slot)
        d = {}
        for tok in line.split():
            k, v = tok.split('=', 1)
            if ',' in v:
                d[k] = [float(x) for x in v.rstrip(',').split(',')]
            elif '.' in v:
                d[k] = float(v)
            else:
                d[k] = int(v)
        return d

    def seed(self, ncars=6, classes=None):
        self.cmd("reset %d" % ncars)
        for i in range(ncars):
            cls = (0 if i == 0 else 1) if classes is None else classes[i]
            self.cmd("car %d %d %d" % (i, cls, i))


# ==========================================================================
def world(ncars=6):
    w = E.World(ncars)
    return w


def rcslot(w, ptr):
    """racecar pointer -> slot, 0 -> -1"""
    if ptr == 0:
        return -1
    return (ptr - E.RC_BASE) // E.RC_STRIDE


# --------------------------------------------------------------------------
def sec1_slam_never_wrecks(C):
    """FUN_001989A0 executed: no path reaches the crash entry FUN_0010DCA0."""
    print("\n-- 1. a slam never wrecks the victim (FUN_001989A0 executed)")
    cases = [
        # (clock, attacker, victim, strength, type, victim_crashed_alt, victim_speed)
        (10.0, 1, 0, 1.0, 0, 0, 60.0),
        (10.0, 1, 0, 0.5, 1, 0, 10.0),
        (25.0, 2, 3, 1.0, 0, 1, 60.0),
        (25.0, 0, 4, 0.2, 1, 0, 5.0),
    ]
    for (t, a, v, s, ty, alt, sp) in cases:
        w = world()
        w.set_clock(t)
        w.wb(w.rc(v) + 0x18FB, alt)
        w.wf(w.pv(v) + 0xBC, sp)
        w.slam(a, v, s, ty)
        names = [c[0] for c in w.calls]
        check("slam(%d->%d) never calls FUN_0010DCA0" % (a, v),
              'CRASH_ENTRY' in names, False)

        real = w.car_state(v)
        ra = w.car_state(a)

        C.seed()
        C.ask("slam %.6f %d %d %d %.6f" % (t, E.B3K_SIDE if ty == 0 else E.B3K_REAR,
                                           a, v, s))
        cv, ca = C.dump(v), C.dump(a)
        check("victim %d slam_time" % v, cv['slam_time'], real['slam_time'], 1e-5)
        check("victim %d aggressor" % v, cv['aggressor'],
              rcslot(w, real['aggressor']))
        check("victim %d aggressor_time" % v, cv['aggressor_time'],
              real['aggressor_time'], 1e-5)
        check("victim %d times_slammed" % v, cv['times_slammed'],
              real['times_slammed'])
        check("victim %d slam_type" % v, cv['slam_type'], real['slam_type'])
        check("attacker %d slams_made" % a, ca['slams_made'], ra['slams_made'])
        check("attacker %d aggressor" % a, ca['aggressor'],
              rcslot(w, ra['aggressor']))
        check("attacker %d aggressor_time" % a, ca['aggressor_time'],
              ra['aggressor_time'], 1e-5)
        check("attacker %d slam_time untouched" % a, ca['slam_time'], -1.0, 1e-6)


def sec2_dispatch(C):
    """FUN_00029F30: which kind reaches the out-of-control stamp."""
    print("\n-- 2. slam-report dispatch by kind (FUN_00029F30 executed)")
    for kind in (1, 2, 3, 4, 5, 6):
        w = world()
        w.set_clock(12.0)
        for i in range(6):
            w.wf(w.pv(i) + 0xBC, 30.0)          # ~67 mph, above the shunt gate
        w.slam_entry(kind, 1, 0, 1.0)
        real = w.car_state(0)
        stamped = real['slam_time'] > 0.0

        C.seed()
        for i in range(6):
            C.cmd("set %d speed_ms 30.0" % i)
        C.ask("slam 12.0 %d 1 0 1.0" % kind)
        cv = C.dump(0)
        check("kind %d out-of-control stamp" % kind,
              cv['slam_time'] > 0.0, stamped)
        check("kind %d slam_time value" % kind, cv['slam_time'],
              real['slam_time'], 1e-5)
        check("kind %d aggressor" % kind, cv['aggressor'],
              rcslot(w, real['aggressor']))
        check("kind %d no crash entry" % kind,
              'CRASH_ENTRY' in [c[0] for c in w.calls], False)


def sec3_gate(C):
    """FUN_00197BE0: the 1.0 s mutual re-slam cooldown."""
    print("\n-- 3. full-slam gate (FUN_00197BE0 executed)")
    cases = [
        # (clock, victim aggressor slot/-1, victim aggr time,
        #  attacker aggressor slot/-1, attacker aggr time, race_state, want)
        (10.0, -1, -1.0, -1, -1.0, 0),
        (10.0, 1, 9.5, -1, -1.0, 0),     # victim was slammed by 1 0.5 s ago
        (10.0, 1, 8.5, -1, -1.0, 0),     # ... 1.5 s ago -> cooldown expired
        (10.0, 2, 9.5, -1, -1.0, 0),     # a different car -> allowed
        (10.0, -1, -1.0, 0, 9.9, 0),     # attacker's aggressor is the victim
        (10.0, -1, -1.0, 2, 9.9, 0),     # a third car -> allowed
        (10.0, -1, -1.0, -1, -1.0, 3),   # race over
    ]
    for (t, va, vat, aa, aat, rs) in cases:
        w = world()
        w.set_clock(t)
        if va >= 0:
            w.wi(w.rc(0) + 0x16BC, w.rc(va))
        w.wf(w.rc(0) + 0x16C0, vat)
        if aa >= 0:
            w.wi(w.rc(1) + 0x16BC, w.rc(aa))
        w.wf(w.rc(1) + 0x16C0, aat)
        w.wi(w.rc(1) + 0x134C, rs)
        ret = w.slam_gate(1, 0, 1.0, 0) & 0xFF
        real = w.car_state(0)

        C.seed()
        C.cmd("set 0 aggressor %d" % va)
        C.cmd("set 0 aggressor_time %.6f" % vat)
        C.cmd("set 1 aggressor %d" % aa)
        C.cmd("set 1 aggressor_time %.6f" % aat)
        C.cmd("set 1 race_state %d" % rs)
        got = int(C.ask("slam %.6f %d 1 0 1.0" % (t, E.B3K_SIDE)).split()[1])
        check("gate va=%d vat=%.1f aa=%d aat=%.1f rs=%d" % (va, vat, aa, aat, rs),
              got, ret)
        cv = C.dump(0)
        check("  -> stamped", cv['slam_time'] > 0.0, real['slam_time'] > 0.0)


def sec4_wall_shunt(C):
    """FUN_00197EA0: kind 2 needs both cars above 40 mph."""
    print("\n-- 4. wall shunt (FUN_00197EA0 executed)")
    for (sa, sv) in ((30.0, 30.0), (30.0, 10.0), (10.0, 30.0), (17.0, 30.0),
                     (18.0, 18.0)):
        w = world()
        w.set_clock(20.0)
        w.wf(w.pv(1) + 0xBC, sa)
        w.wf(w.pv(0) + 0xBC, sv)
        ret = w.wall_shunt(1, 0) & 0xFF
        real = w.car_state(0)

        C.seed()
        C.cmd("set 1 speed_ms %.6f" % sa)
        C.cmd("set 0 speed_ms %.6f" % sv)
        got = int(C.ask("slam 20.0 %d 1 0 1.0" % E.B3K_WALL).split()[1])
        check("shunt %.0f/%.0f m/s accepted" % (sa, sv), got, ret)
        cv = C.dump(0)
        check("shunt %.0f/%.0f aggressor" % (sa, sv), cv['aggressor'],
              rcslot(w, real['aggressor']))
        check("shunt %.0f/%.0f no OOC stamp" % (sa, sv), cv['slam_time'],
              real['slam_time'], 1e-6)


def sec5_attribution(C):
    """FUN_00197430: who gets a claim when car `v` crashes."""
    print("\n-- 5. crash attribution (FUN_00197430 executed)")
    cases = [
        dict(name="contact timer above Min Collide Time",
             contact={1: 0.2}, aggr=None, dist={}, cause=None),
        dict(name="contact timer below Min Collide Time",
             contact={1: 0.05}, aggr=None, dist={}, cause=None),
        dict(name="slammer inside Maximum Crash Wait",
             contact={}, aggr=(1, 9.0), dist={}, cause=None),
        dict(name="slammer outside Maximum Crash Wait",
             contact={}, aggr=(1, 7.0), dist={}, cause=None),
        dict(name="contact but 200 m away",
             contact={1: 0.5}, aggr=None, dist={1: 200.0}, cause=None),
        dict(name="contact 100 m away",
             contact={1: 0.5}, aggr=None, dist={1: 100.0}, cause=None),
        dict(name="aftertouch: a human wreck caused it",
             contact={}, aggr=None, dist={}, cause=('wreck', 1)),
        dict(name="two claimants",
             contact={1: 0.5, 2: 0.5}, aggr=None, dist={}, cause=None),
    ]
    T = 10.0
    for cs in cases:
        w = world()
        w.set_clock(T)
        for i in range(6):
            w.set_pos(i, 0.0, 0.0, 0.0)
        for k, d in cs['dist'].items():
            w.set_pos(k, d, 0.0, 0.0)
        for k, v in cs['contact'].items():
            # score+0x528[k] on the CRASHING car (slot 0) -- FUN_00197430
            # walks its own array, not the opponent's
            w.wf(w.score(0) + 0x528 + 4 * k, v)
        if cs['aggr']:
            w.wi(w.score(0) + 0x5EC, w.rc(cs['aggr'][0]))
            w.wf(w.score(0) + 0x5F0, cs['aggr'][1])
        rec = 0
        if cs['cause'] and cs['cause'][0] == 'wreck':
            wr = cs['cause'][1]
            w.wi(w.rc(wr) + 0x1920, 0)               # human
            w.wb(w.rc(wr) + 0x18FA, 1)               # crashed
            rec = w.cause_record(wreck_rc=w.rc(wr))
        w.attribute(0, rec)
        real = [w.car_state(i) for i in range(6)]

        C.seed()
        for i in range(6):
            C.cmd("pos %d 0 0 0" % i)
        for k, d in cs['dist'].items():
            C.cmd("pos %d %.3f 0 0" % (k, d))
        for k, v in cs['contact'].items():
            C.cmd("setidx 0 contact %d %.6f" % (k, v))
        if cs['aggr']:
            C.cmd("set 0 aggressor %d" % cs['aggr'][0])
            C.cmd("set 0 aggressor_time %.6f" % cs['aggr'][1])
        wreck = -1
        if cs['cause'] and cs['cause'][0] == 'wreck':
            wreck = cs['cause'][1]
            C.cmd("car %d 0 %d" % (wreck, wreck))
            C.cmd("set %d crashed 1" % wreck)
        C.cmd("crash %.6f 0 0 0 0 %d" % (T, wreck))
        for i in range(1, 6):
            cv = C.dump(i)
            check("%s: car %d claim[0]" % (cs['name'], i), cv['claim'][0],
                  real[i]['claim'][0], 1e-5)
            check("%s: car %d claim_at[0]" % (cs['name'], i),
                  int(cv['claim_at'][0]), real[i]['claim_at'][0])


def sec6_claim_scan(C):
    """FUN_00197040: the Race Car Clear Wait deferral."""
    print("\n-- 6. deferred claim scan (FUN_00197040 executed)")
    cases = [
        (10.0, 9.9, 0, "too fresh"),
        (10.0, 9.5, 0, "exactly Race Car Clear Wait"),
        (10.0, 9.0, 0, "old enough"),
        (10.0, 9.0, 1, "attacker crashed"),
    ]
    for (t, stamp, attacker_crashed, name) in cases:
        w = world()
        w.set_clock(t)
        w.wf(w.score(1) + 0x4D8 + 4 * 0, stamp)   # car 1 claims car 0
        w.wb(w.rc(1) + 0x18FA, attacker_crashed)
        w.stub(E.F_COMMIT, 'commit', argbytes=0,
               cb=lambda e, a, r: e.uc.reg_write(E.UC_X86_REG_EAX, 1))
        w.claim_scan(1)
        committed = any(c[0] == 'commit' for c in w.calls)
        cleared = w.rf(w.score(1) + 0x4D8) < 0.0

        C.seed()
        C.cmd("setidx 1 claim 0 %.6f" % stamp)
        C.cmd("set 1 crashed %d" % attacker_crashed)
        evs = C.events(t, 1)
        check("claim scan (%s) commits" % name,
              len([e for e in evs if e['kind'] == 1]) > 0, committed)
        check("claim scan (%s) clears the slot" % name,
              C.dump(1)['claim'][0] < 0.0, cleared)


def sec7_commit(C):
    """FUN_00198E60: dedup, counters, revenge, AI recovery."""
    print("\n-- 7. commit (FUN_00198E60 executed)")
    for (pre_credited, revenge, vcls) in ((0, 0, 1), (1, 0, 1), (0, 1, 1),
                                          (0, 0, 0)):
        w = world()
        w.gamectx(True)
        w.set_clock(30.0)
        w.wi(w.rc(0) + 0x1920, vcls)
        w.wb(w.rc(0) + 0x15D6, pre_credited)
        if revenge:
            # score+0x5B9[victim grid] on the attacker = "they took me down"
            w.wb(w.score(1) + 0x5B9 + 0, 1)
        w.stub(E.F_AWARD, 'award', argbytes=16)
        ret = w.commit(1, 0) & 0xFF
        rv, ra = w.car_state(0), w.car_state(1)

        C.seed(classes=[vcls, 1, 1, 1, 1, 1])
        C.cmd("set 0 td_credited %d" % pre_credited)
        if revenge:
            C.cmd("setidx 1 tdby 0 1")
        C.cmd("setidx 1 claim 0 10.0")
        evs = C.events(30.0, 1)
        cv, ca = C.dump(0), C.dump(1)
        tag = "credited=%d revenge=%d vcls=%d" % (pre_credited, revenge, vcls)
        check("commit %s returns" % tag, 1 if evs else 0, ret)
        check("commit %s victim td_credited" % tag, cv['td_credited'],
              rv['td_credited'])
        check("commit %s victim td_by" % tag, cv['td_by'],
              rcslot(w, rv['td_by']) if rv['td_credited'] else cv['td_by'])
        check("commit %s attacker td_count" % tag, ca['td_count'],
              ra['td_count'])
        check("commit %s victim revenge flag" % tag, cv['revenge'], rv['revenge'])
        check("commit %s AI recovery" % tag, cv['recover_at'],
              rv['recover_at'], 1e-5)


def sec8_messages(C):
    """FUN_001994D0: the takedown message selection."""
    print("\n-- 8. message selection + BP (FUN_001994D0 executed)")
    vclasses = [(1, 0x94), (2, 0x95), (3, 0x96), (4, 0x97), (5, 0x98),
                (7, 0x99), (8, 0x9A), (9, 0x9B), (11, 0x9C), (6, 0x93)]
    for (vc, want) in vclasses:
        w = world()
        w.gamectx(True)
        w.set_clock(40.0)
        w.wb(w.rc(0) + 0x18FA, 1)
        obj = w.traffic_object(vc)
        rec = w.cause_record(has_obj=1, obj=obj)
        w.posts = []
        w.award(1, 0, 0, 0, rec)
        got = w.posts[0]['msg'] if w.posts else -1
        check("vehicle class %d -> msg 0x%02X (real)" % (vc, want), got, want)

        C.seed()
        C.cmd("set 0 crashed 1")
        C.cmd("crash 40.0 0 0 1 %d -1" % vc)
        cmsg = int(C.ask("msg 1 0 0 0").split()[1])
        check("vehicle class %d -> msg (C)" % vc, cmsg, got)

    # wall
    w = world(); w.gamectx(True); w.set_clock(40.0); w.wb(w.rc(0) + 0x18FA, 1)
    rec = w.cause_record(wall=1)
    w.posts = []
    w.award(1, 0, 0, 0, rec)
    real_wall = w.posts[0]['msg']
    check("wall -> 0x9D (real)", real_wall, 0x9D)
    C.seed(); C.cmd("set 0 crashed 1"); C.cmd("crash 40.0 0 1 0 0 -1")
    check("wall -> 0x9D (C)", int(C.ask("msg 1 0 0 0").split()[1]), real_wall)

    # no cause record at all
    w = world(); w.gamectx(True); w.set_clock(40.0); w.wb(w.rc(0) + 0x18FA, 1)
    w.posts = []
    w.award(1, 0, 0, 0, 0)
    check("no cause -> 0x93 (real)", w.posts[0]['msg'], 0x93)
    C.seed(); C.cmd("set 0 crashed 1"); C.cmd("crash 40.0 0 0 0 0 -1")
    check("no cause -> 0x93 (C)", int(C.ask("msg 1 0 0 0").split()[1]), 0x93)

    # psyche out
    w = world(); w.gamectx(True); w.set_clock(40.0); w.wb(w.rc(0) + 0x18FA, 1)
    w.posts = []
    w.award(1, 0, 1, 0, 0)
    check("psyche -> 0xA9 (real)", w.posts[0]['msg'], 0xA9)
    check("psyche BP", w.posts[0]['bp'], 150)
    C.seed(); C.cmd("set 0 crashed 1")
    check("psyche -> 0xA9 (C)", int(C.ask("msg 1 0 0 1").split()[1]), 0xA9)

    # aftertouch ladder
    for n in (0, 1, 2, 3, 4, 5):
        w = world(); w.gamectx(True); w.set_clock(40.0); w.wb(w.rc(0) + 0x18FA, 1)
        w.wi(w.cslot(1) + 0x128, n)
        w.posts = []
        w.award(1, 0, 0, 1, 0)
        want = w.posts[0]['msg']
        check("aftertouch #%d msg (real) 0xAA+min(n,4)" % n, want,
              0xAA + min(n, 4))
        check("aftertouch #%d BP" % n, w.posts[0]['bp'], 1250)
        C.seed(); C.cmd("set 0 crashed 1"); C.cmd("set 1 at_count %d" % n)
        check("aftertouch #%d msg (C)" % n,
              int(C.ask("msg 1 0 1 0").split()[1]), want)

    # BP totals: plain, double, spree, revenge
    for (label, dblc, dblw, sprc, sprw, rev, want_bp) in (
            ("plain", 0, -1.0, 0, -1.0, 0, 150),
            ("double #2", 1, 45.0, 1, 60.0, 0, 150 + 300 + 300),
            ("double #3", 2, 45.0, 2, 60.0, 0, 150 + 500 + 500),
            ("revenge", 0, -1.0, 0, -1.0, 1, 150 + 350)):
        w = world(); w.gamectx(True); w.set_clock(40.0); w.wb(w.rc(0) + 0x18FA, 1)
        w.wi(w.cslot(1) + 0x114, dblc); w.wf(w.cslot(1) + 0x110, dblw)
        w.wi(w.cslot(1) + 0x11C, sprc); w.wf(w.cslot(1) + 0x118, sprw)
        w.wb(w.rc(1) + 0x1689 + 0, rev)
        w.posts = []
        w.award(1, 0, 0, 0, 0)
        real_bp = w.ri(w.rc(1) + 0x111C)
        check("BP %s (real)" % label, real_bp, want_bp)

        C.seed()
        C.cmd("set 0 crashed 1")
        C.cmd("set 1 dbl_count %d" % dblc); C.cmd("set 1 dbl_window %.6f" % dblw)
        C.cmd("set 1 spree_count %d" % sprc)
        C.cmd("set 1 spree_window %.6f" % sprw)
        if rev:
            C.cmd("setidx 1 tdby 0 1")
        C.cmd("setidx 1 claim 0 10.0")
        evs = C.events(40.0, 1)
        check("BP %s (C)" % label, evs[0]['bp'] if evs else -1, real_bp)


def sec9_denied(C):
    """FUN_00197920 arm + FUN_00195CE0 award."""
    print("\n-- 9. TAKEDOWN DENIED / LUCKY ESCAPE")
    arm_cases = [
        (10.0, 9.0, 1, 0, 0, "inside the window"),
        (10.0, 7.0, 1, 0, 0, "outside Maximum Crash Wait"),
        (10.0, 9.0, 0, 0, 0, "no aggressor pointer"),
        (10.0, 9.0, 1, 1, 0, "I am crashed"),
        (10.0, 9.0, 1, 0, 3, "race over"),
        (10.0, -1.0, 1, 0, 0, "never slammed"),
    ]
    for (t, at, has_aggr, crashed, rs, name) in arm_cases:
        w = world()
        w.set_clock(t)
        w.wf(w.score(0) + 0x5F0, at)
        if has_aggr:
            w.wi(w.score(0) + 0x5EC, w.rc(1))
        w.wb(w.rc(0) + 0x18FA, crashed)
        w.wi(w.rc(0) + 0x134C, rs)
        obj = w.traffic_object(1)
        w.deny_arm(0, obj)
        real = w.car_state(0)

        C.seed()
        C.cmd("set 0 aggressor_time %.6f" % at)
        C.cmd("set 0 aggressor %d" % (1 if has_aggr else -1))
        C.cmd("set 0 crashed %d" % crashed)
        C.cmd("set 0 race_state %d" % rs)
        C.cmd("notify %.6f 0" % t)
        cv = C.dump(0)
        check("denied arm (%s) pending" % name, cv['denied'], real['denied'])
        check("denied arm (%s) lucky" % name, cv['lucky'], real['lucky'])

    # award
    for (t, at, pend, lucky, name) in ((10.0, 9.0, 1, 1, "both"),
                                       (12.0, 9.0, 1, 1, "window expired"),
                                       (10.0, 9.0, 0, 0, "not armed")):
        w = world()
        w.gamectx(True)
        w.set_clock(t)
        w.wf(w.score(0) + 0x5F0, at)
        w.wi(w.score(0) + 0x5EC, w.rc(1))
        w.wb(w.score(0) + 0x5E5, pend)
        w.wb(w.score(0) + 0x5E6, lucky)
        # FUN_00199CA0 / FUN_00199BE0 are inline copies of FUN_00199350's
        # accept test: they write the message id straight into the callout
        # slot's priority field (+0x130), so read it back from there.
        w.wi(w.cslot(0) + 0x130, 0)
        w.wi(w.cslot(1) + 0x130, 0)
        w.deny_award(0)
        msgs = sorted(m for m in (w.ri(w.cslot(0) + 0x130),
                                  w.ri(w.cslot(1) + 0x130)) if m)
        bp = w.ri(w.rc(0) + 0x111C)

        C.seed()
        C.cmd("set 0 aggressor_time %.6f" % at)
        C.cmd("set 0 aggressor 1")
        if pend:
            C.cmd("notify %.6f 0" % at)      # arms both flags
        evs = C.events(t, 0)
        cmsgs = sorted(e['message'] for e in evs)
        check("denied award (%s) messages" % name, cmsgs, msgs)
        check("denied award (%s) BP" % name, C.dump(0)['bp'], bp)


def sec10_full_flow(C):
    """The whole recovered flow, end to end, against the real functions."""
    print("\n-- 10. slam -> OOC -> wall crash -> WALL TAKEDOWN (both sides)")
    T0, T1, T2 = 10.0, 10.4, 11.0

    # ---- real ----
    w = world()
    w.set_clock(T0)
    for i in range(6):
        w.set_pos(i, 0.0, 0.0, 0.0)
        w.wf(w.pv(i) + 0xBC, 40.0)
    w.slam_entry(6, 1, 0, 1.0)                       # car 1 rear-ends car 0
    check("flow: no wreck from the slam",
          'CRASH_ENTRY' in [c[0] for c in w.calls], False)
    check("flow: victim out of control", w.car_state(0)['slam_time'], T0, 1e-5)
    # 0.4 s later the victim hits a wall: the cause record says WALL
    w.set_clock(T1)
    rec = w.cause_record(wall=1)
    w.w(w.rc(0) + 0x13D8, w.r(rec, 0x10))            # FUN_00197750's store
    w.attribute(0, rec)
    real_claim = w.car_state(1)['claim'][0]
    check("flow: attacker holds the claim", real_claim, T1, 1e-5)
    w.wb(w.rc(0) + 0x18FA, 1)
    w.set_clock(T2)
    w.gamectx(True)
    w.stub(E.F_AWARD, 'award_capture', argbytes=16,
           cb=lambda e, a, r: w.__setattr__('award_ecx', r['ecx']))
    w.claim_scan(1)
    check("flow: commit passes the victim's stored cause record",
          getattr(w, 'award_ecx', 0), w.rc(0) + 0x13D8)
    check("flow: victim credited", w.car_state(0)['td_credited'], 1)

    # ---- C ----
    C.seed()
    for i in range(6):
        C.cmd("pos %d 0 0 0" % i)
        C.cmd("set %d speed_ms 40.0" % i)
    C.ask("slam %.6f %d 1 0 1.0" % (T0, E.B3K_REAR))
    check("flow(C): victim out of control", C.dump(0)['slam_time'], T0, 1e-5)
    check("flow(C): OOC predicate at +0.9 s",
          int(C.ask("ooc 0 %.6f" % (T0 + 0.9)).split()[1]), 1)
    check("flow(C): OOC predicate at +1.1 s",
          int(C.ask("ooc 0 %.6f" % (T0 + 1.1)).split()[1]), 0)
    C.cmd("crash %.6f 0 1 0 0 -1" % T1)
    check("flow(C): attacker holds the claim", C.dump(1)['claim'][0],
          real_claim, 1e-5)
    evs = C.events(T2, 1)
    check("flow(C): one takedown", len(evs), 1)
    if evs:
        check("flow(C): WALL TAKEDOWN message", evs[0]['message'], 0x9D)
        check("flow(C): takedown BP", evs[0]['bp'], 150)
    check("flow(C): victim credited", C.dump(0)['td_credited'], 1)

    # ---- and the DENIED branch: the victim never crashes -----------------
    C.seed()
    for i in range(6):
        C.cmd("set %d speed_ms 40.0" % i)
    C.ask("slam %.6f %d 1 0 1.0" % (T0, E.B3K_REAR))
    C.cmd("notify %.6f 0" % (T0 + 0.5))              # the victim survives a hit
    evs = C.events(T0 + 0.6, 0)
    kinds = sorted(e['kind'] for e in evs)
    check("flow(C): survived slam -> LUCKY + DENIED", kinds, [2, 3])
    evs2 = C.events(T2, 1)
    check("flow(C): no takedown for the attacker", len(evs2), 0)


def sec11_constants():
    """The tuned parameters and message ids, straight out of the image."""
    print("\n-- 11. constants (image bytes)")
    data = open(os.path.join(_root, 'build', 'burnout3.elf'), 'rb').read()
    ph_off = struct.unpack_from('<I', data, 0x1C)[0]
    ph_num = struct.unpack_from('<H', data, 0x2C)[0]
    segs = []
    for i in range(ph_num):
        t, off, va, _pa, fsz, msz, _f, _a = struct.unpack_from(
            '<IIIIIIII', data, ph_off + i * 32)
        if t == 1:
            segs.append((va, off, fsz))

    def rd(va, n):
        for v, o, f in segs:
            if v <= va < v + f:
                return data[o + (va - v):o + (va - v) + n]
        return b'\0' * n

    def f32(va):
        return struct.unpack('<f', rd(va, 4))[0]

    check("0x003B1944 attribution radius^2", f32(0x003B1944), 25600.0, 1e-3)
    check("0x0038994C mph", f32(0x0038994C), 2.2369363, 1e-6)
    check("0x003B168C 1.0 (re-slam window)", f32(0x003B168C), 1.0, 1e-6)
    check("0x003B16C0 -1.0 (idle clock)", f32(0x003B16C0), -1.0, 1e-6)
    check("DAT_003A4B38 aftertouch ladder", list(rd(0x003A4B38, 5)),
          [0xAA, 0xAB, 0xAC, 0xAD, 0xAE])
    # the vtable slots this module depends on
    for base in (0x003A9280, 0x003A98A0, 0x003A9EA0, 0x003A9528):
        ents = struct.unpack('<32I', rd(base, 128))
        check("vtable %08X +0x64 = FUN_00029F30" % base, ents[25], 0x00029F30)
        check("vtable %08X +0x54 = FUN_000273D0" % base, ents[21], 0x000273D0)
        check("vtable %08X +0x48 is a crash notifier" % base,
              ents[18] in (0x00024940, 0x0002BFE0), True)
    # the control-handover sites the cinematic assumes
    check("0x0018CB6A stores racecar+0x27D8",
          rd(0x0018CB6A, 6), b'\x88\x88\xd8\x27\x00\x00')
    check("0x0018CB94 restores pv+0x1534",
          rd(0x0018CB94, 8), b'\xf3\x0f\x11\x81\x34\x15\x00\x00')
    check("0x0018C53A reads racecar+0x27D8",
          rd(0x0018C53A, 6), b'\x8a\x86\xd8\x27\x00\x00')
    check("0x0018C54A calls the auto-driver FUN_00170820",
          rd(0x0018C54A, 1) + struct.pack('<i', 0x00170820 - 0x0018C54F),
          rd(0x0018C54A, 5))
    check("0x000279F0 clears racecar+0x27D8",
          rd(0x000279F0, 6), b'\x88\x98\xd8\x27\x00\x00')
    # the slam handler contains no call to the crash entry
    body = rd(0x001989A0, 0x520)
    calls = []
    for i in range(len(body) - 5):
        if body[i] == 0xE8:
            rel = struct.unpack_from('<i', body, i + 1)[0]
            calls.append((0x001989A0 + i + 5 + rel) & 0xFFFFFFFF)
    check("FUN_001989A0 never calls FUN_0010DCA0", 0x0010DCA0 in calls, False)
    check("FUN_001989A0 never calls FUN_0010DD20", 0x0010DD20 in calls, False)


def sec12_authority(C):
    """The crash-threshold authority veh+0x1534 and the wall trigger it
    scales -- docs/RE_TD_RULES.md 12.

    The view-distance ladder runs FOR REAL under Unicorn (FUN_00105FC0) and
    is diffed against the C port band by band; the FUN_00105340 override and
    the FUN_0011AEF0 gate are asserted against the image bytes and then
    exercised through the ported trigger."""
    print("\n-- 12. crash-threshold authority + the wall trigger")
    A = E.Authority()
    base = A.rf(E.K_VIEW_R2)
    check("0x0039A854 view radius^2 (125 m)", base, 15625.0, 1e-3)
    check("0x0039A850 alt view radius^2 (140 m)", A.rf(E.K_VIEW_R2_ALT),
          19600.0, 1e-3)
    check("0x003B1A2C authority scale", A.rf(E.K_AUTH_SCALE), 0.97, 1e-6)
    check("0x00384148 authority floor", A.rf(E.K_AUTH_FLOOR), 0.03, 1e-6)
    check("0x003A69BC slam authority (race)", A.rf(E.K_AUTH_SLAM), 0.05, 1e-6)
    check("0x003A69C4 slam authority (crash mode)", A.rf(E.K_AUTH_SLAMCM),
          0.1, 1e-6)
    bands = [A.rf(E.K_LADDER + 4 * i) for i in range(1, 6)]
    for i, want in enumerate([0.0, 0.1, 0.4, 0.5, 1.0]):
        check("DAT_0039A858[%d] ladder band" % (i + 1), bands[i],
              want, 1e-6)

    # ---- the ladder, real code vs the port -------------------------------
    C.cmd("reset 2")
    C.cmd("car 0 1 0")          # class 1 so the ladder path is taken
    C.cmd("car 1 1 1")
    for frac in (0.0, 0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.5,
                 0.75, 1.0, 1.5):
        d2 = frac * base
        want = A.ladder(d2)
        C.cmd("view 0 %.9g 0" % d2)
        got = C.ask("auth 0 0.0").split()
        check("ladder %.2f base: authority" % frac, float(got[1]),
              want['authority'], 2e-6)
        check("ladder %.2f base: crash_ok" % frac, int(got[2]),
              1 if want['crash_ok'] else 0)
        check("ladder %.2f base: in_range" % frac, int(got[3]),
              1 if want['in_range'] else 0)
    # the alternate radius selector veh+0x1550
    for frac in (0.2, 0.5):
        d2 = frac * A.rf(E.K_VIEW_R2_ALT)
        want = A.ladder(d2, alt=1)
        C.cmd("view 0 %.9g 1" % d2)
        check("alt radius %.2f base: authority" % frac,
              float(C.ask("auth 0 0.0").split()[1]), want['authority'], 2e-6)
    C.cmd("view 0 -1 0")

    # ---- the human early-out --------------------------------------------
    C.cmd("reset 2"); C.cmd("car 0 0 0"); C.cmd("car 1 1 1")
    C.cmd("view 0 %.9g 0" % (0.9 * base))     # far, but class 0
    got = C.ask("auth 0 0.0").split()
    check("human early-out ignores the ladder", float(got[1]), 1.0, 1e-6)
    check("human early-out keeps the crash entries on", int(got[2]), 1)

    # ---- FUN_00105340's six-condition override ---------------------------
    # bytes first: the two stored constants and the class compare
    data = open(os.path.join(_root, 'build', 'burnout3.elf'), 'rb').read()
    ph_off = struct.unpack_from('<I', data, 0x1C)[0]
    ph_num = struct.unpack_from('<H', data, 0x2C)[0]
    segs = [s for s in (struct.unpack_from('<IIIIIIII', data,
                                           ph_off + i * 32)
                        for i in range(ph_num)) if s[0] == 1]

    def rd(va, n):
        for t, o, v, _p, f, _m, _fl, _a in segs:
            if v <= va < v + f:
                return data[o + (va - v):o + (va - v) + n]
        return b'\0' * n

    check("0x001056BE loads 0.05 [0x003A69BC]", rd(0x001056BE, 8),
          b'\xf3\x0f\x10\x0d\xbc\x69\x3a\x00')
    check("0x001056C6 stores it to veh+0x1534", rd(0x001056C6, 8),
          b'\xf3\x0f\x11\x8f\x34\x15\x00\x00')
    check("0x001057AA loads 0.1 [0x003A69C4]", rd(0x001057AA, 8),
          b'\xf3\x0f\x10\x0d\xc4\x69\x3a\x00')
    check("0x001056B1 compares attacker+0x1920 with 0", rd(0x001056B1, 7),
          b'\x83\xb8\x20\x19\x00\x00\x00')
    check("0x001056B8 bails when it is not 0 (JNZ)", rd(0x001056B8, 2)[0],
          0x0F)

    # now the behaviour, one condition violated at a time
    def arm(clock=1.0):
        C.cmd("reset 2"); C.cmd("car 0 1 0"); C.cmd("car 1 0 1")
        C.cmd("set 0 slam_time 0.5")
        C.cmd("set 0 aggressor 1")
        C.cmd("set 0 aggressor_time 0.5")
        return clock

    arm(); check("slammed by the human -> 0.05",
                 float(C.ask("auth 0 1.0").split()[1]), 0.05, 1e-6)
    arm(); C.cmd("crashmode 1")
    check("crash mode -> 0.1", float(C.ask("auth 0 1.0").split()[1]),
          0.1, 1e-6)
    C.cmd("crashmode 0")
    arm(); check("outside the OOC window -> no override",
                 float(C.ask("auth 0 1.6").split()[1]), 1.0, 1e-6)
    arm(); C.cmd("set 0 slam_time -1")
    check("never slammed -> no override",
          float(C.ask("auth 0 1.0").split()[1]), 1.0, 1e-6)
    arm(); C.cmd("set 0 aggressor_time -1")
    check("no aggressive contact clock -> no override",
          float(C.ask("auth 0 1.0").split()[1]), 1.0, 1e-6)
    arm(); C.cmd("set 0 aggressor -1")
    check("no aggressor -> no override",
          float(C.ask("auth 0 1.0").split()[1]), 1.0, 1e-6)
    # the attacker-class condition: an AI aggressor does NOT get the assist
    C.cmd("reset 2"); C.cmd("car 0 1 0"); C.cmd("car 1 1 1")
    C.cmd("set 0 slam_time 0.5"); C.cmd("set 0 aggressor 1")
    C.cmd("set 0 aggressor_time 0.5")
    check("an AI aggressor gets no override (attacker+0x1920 != 0)",
          float(C.ask("auth 0 1.0").split()[1]), 1.0, 1e-6)
    # an armed psyche-out pins the floor
    arm(); C.cmd("set 0 slam_time -1"); C.cmd("psyche 0 1")
    got = C.ask("auth 0 1.0").split()
    check("psyche armed -> the 0.03 floor", float(got[1]), 0.03, 1e-6)
    check("psyche armed keeps the crash entries on", int(got[2]), 1)

    # ---- FUN_0011AEF0's gate, driven through the ported trigger ----------
    check("0x0011B95E multiplies authority by 27.5 [0x0039B2FC]",
          rd(0x0011B95E, 8), b'\xf3\x0f\x59\x05\xfc\xb2\x39\x00')
    check("0x0011B979 multiplies authority by 0.707 [0x003B1A20]",
          rd(0x0011B979, 8), b'\xf3\x0f\x59\x05\x20\x1a\x3b\x00')
    check("0x0011B91F crash-party dv scale 10.0 [0x003A7F34]",
          rd(0x0011B91F, 8), b'\xf3\x0f\x59\x05\x34\x7f\x3a\x00')
    check("0x0011B93A crash-party head-on 0.303 [0x0039B308]",
          rd(0x0011B93A, 8), b'\xf3\x0f\x59\x05\x08\xb3\x39\x00')

    # identity frame: at = +z, so a wall normal of -z is dead head-on.
    # mass 1000, unit inverse inertia, contact on the centre line -> the
    # angular term vanishes and dv is the closing speed exactly.
    def wall(slot, clock, vz, nz=-1.0, nx=0.0, pz=2.2):
        f = ("wall %d %g 0 0 %g %g 0 %g 0 0 %g 1000 %g"
             % (slot, clock, pz, nx, nz, vz, abs(vz)))
        return C.ask(f).split()

    C.cmd("reset 2"); C.cmd("car 0 0 0"); C.cmd("car 1 1 1")
    r = wall(0, 0.0, 30.0)
    # FUN_0011AEF0 scrubs the velocity before it measures the impulse:
    # head-on 1 - (1.0 - 0.707)*0.1 = 0.9707, then the 0.99 surface scrub.
    check("player, 30 m/s head-on: dv is the SCRUBBED closing speed",
          float(r[2]), 30.0 * (1.0 - (1.0 - 0.707) * 0.1) * 0.99, 0.02)
    check("player, 30 m/s head-on: threshold is 27.5", float(r[4]), 27.5,
          1e-4)
    check("player, 30 m/s head-on: CRASHES", int(r[1]), 1)
    r = wall(0, 0.0, 40.0)
    check("player, 40 m/s head-on: CRASHES", int(r[1]), 1)
    r = wall(0, 0.0, 25.0)
    check("player, 25 m/s head-on: survives (under 27.5)", int(r[1]), 0)
    # a glancing hit at the same speed fails the head-on test, not the dv one
    r = wall(0, 0.0, 40.0, nz=-0.5, nx=0.866)
    check("player, glancing 40 m/s: headon under 0.707", float(r[3]) < 0.707,
          True)
    check("player, glancing 40 m/s: survives", int(r[1]), 0)

    # the same car, just slammed by the player -> 0.05 authority
    C.cmd("reset 2"); C.cmd("car 0 1 0"); C.cmd("car 1 0 1")
    C.cmd("set 0 slam_time 0.5"); C.cmd("set 0 aggressor 1")
    C.cmd("set 0 aggressor_time 0.5")
    r = wall(0, 1.0, 3.0)
    check("slammed victim: authority 0.05", float(r[6]), 0.05, 1e-6)
    check("slammed victim: dv threshold 1.375", float(r[4]), 1.375, 1e-4)
    check("slammed victim: 3 m/s barrier brush WRECKS it", int(r[1]), 1)
    r = wall(0, 1.0, 40.0, nz=-0.5, nx=0.866)
    check("slammed victim: a glancing hit wrecks it too", int(r[1]), 1)
    # ... and the same brush without the slam does nothing
    C.cmd("reset 2"); C.cmd("car 0 1 0"); C.cmd("car 1 0 1")
    C.cmd("view 0 -1 0")
    r = wall(0, 1.0, 3.0)
    check("un-slammed: the same 3 m/s brush is ignored", int(r[1]), 0)

    # the ladder's far band switches the entry off outright
    C.cmd("reset 2"); C.cmd("car 0 1 0"); C.cmd("car 1 0 1")
    C.cmd("view 0 %.9g 0" % (0.9 * base))
    r = wall(0, 0.0, 60.0)
    check("beyond the ladder's 0.4 band: crash entry disabled", int(r[1]), 0)
    C.cmd("view 0 %.9g 0" % (0.2 * base))
    r = wall(0, 0.0, 20.0)
    check("inside the ramp band: a 20 m/s hit wrecks (thr %.1f)"
          % (0.6767 * 27.5), int(r[1]), 1)


def sec13_ladder_input(C):
    """[S] -> [C]: prove the view-distance ladder's d2 input by executing
    FUN_00105BD0 WHOLE -- DAT_0073A1C0 is the LOCAL PLAYER count and
    DAT_0073A1D0 the inline array of those players' racecars, so in single
    player the ladder measures the distance to THE PLAYER'S CAR."""
    print("\n-- 13. the authority ladder's distance input (FUN_00105BD0)")
    A = E.AuthorityFull()
    base = A.rf(E.K_VIEW_R2)

    # (1) one local player: the loop must `this`-call exactly that record and
    #     store |p_player - veh.frame.pos|^2 at veh+0x1560[0].
    r = A.run((10.0, 0.0, 20.0), [(40.0, 0.0, 60.0)], cls=1)
    check("one local player: one position query", len(r['visited']), 1)
    check("the query's `this` IS DAT_0073A1D0[0]", r['visited'][0],
          E.G_LOCAL_CARS)
    check("veh+0x1560[0] = squared distance to the player's car",
          r['d2'][0], 30.0 * 30.0 + 40.0 * 40.0, 1e-2)

    # (2) two local players: stride 0x27E0, and the NEAREST one wins
    r2 = A.run((0.0, 0.0, 0.0), [(100.0, 0.0, 0.0), (10.0, 0.0, 0.0)], cls=1)
    check("two local players: two queries", len(r2['visited']), 2)
    check("the array stride is 0x27E0",
          r2['visited'][1] - r2['visited'][0], 0x27E0)
    check("both distances stored", [round(x) for x in r2['d2']],
          [10000, 100])
    check("the NEAREST feeds the ladder (10 m -> full authority)",
          r2['authority'], 1.0, 2e-6)

    # (3) the loop bound is DAT_0073A1C0: a second local player that is FAR
    #     cannot be reached when the count is 1
    r3 = A.run((0.0, 0.0, 0.0), [(10.0, 0.0, 0.0)], cls=1)
    check("count 1 stops the loop after one record", len(r3['visited']), 1)

    # (4) the ladder value the whole function produces must equal the value
    #     the C port derives from that same d2 -- band by band.
    C.cmd("reset 2"); C.cmd("car 0 1 0"); C.cmd("car 1 1 1")
    for frac in (0.05, 0.2, 0.3, 0.5, 1.2):
        d = (frac * base) ** 0.5
        got = A.run((0.0, 0.0, 0.0), [(d, 0.0, 0.0)], cls=1)
        C.cmd("view 0 %.9g 0" % (frac * base))
        port = float(C.ask("auth 0 0.0").split()[1])
        check("whole-ladder %.2f base: real %.5f == port" % (frac, port),
              got['authority'], port, 3e-5)
    C.cmd("view 0 -1 0")

    # (5) the visibility test is FUN_001AD4A0 on the SCORED car's position:
    #     a local player that fails it is skipped, so the far one wins.
    r5 = A.run((0.0, 0.0, 0.0), [(10.0, 0.0, 0.0), (100.0, 0.0, 0.0)],
               cls=1, seen={0: 0, 1: 1})
    check("an unseen local player is skipped (100 m wins)",
          r5['authority'], A.ladder(10000.0)['authority'], 3e-5)


def sec14_object_trigger(C):
    """FUN_00112E70's OBJECT/PROP crash trigger and its class table."""
    print("\n-- 14. the object/prop crash trigger (FUN_00112E70)")
    A = E.Authority()

    # ---- the image constants -------------------------------------------
    check("0x003EBE44 object mph scale (normal race)", A.rf(0x003EBE44),
          75.0, 1e-4)
    check("0x003EBE48 object mph scale (crash party)", A.rf(0x003EBE48),
          20.0, 1e-4)
    check("0x0038994C m/s -> mph", A.rf(0x0038994C), 2.2369363, 1e-6)
    check("0x003EBE74 damage constant", A.rf(0x003EBE74), 0.1, 1e-6)
    check("0x003B1E84 the veh+0x152C decay floor", A.rf(0x003B1E84),
          -1e-4, 1e-9)

    # ---- FUN_0010FBC0 executed for real, every handle type --------------
    for ty in range(0, 8):
        for dt in (0, 1):
            got = A.handle_class(ty, dt)
            port = int(C.ask("objclass %d %d" % (ty, dt)).split()[1])
            check("FUN_0010FBC0(type %d, designated %d) = %d" % (ty, dt, got),
                  port, got)

    # ---- DAT_0039AE50, byte for byte, vs the ported table ---------------
    tbl = A.r(0x0039AE50, 49)
    for b in range(7):
        for a in range(7):
            port = int(C.ask("objtable %d %d" % (b, a)).split()[1])
            check("DAT_0039AE50[%d][%d]" % (b, a), port, tbl[b * 7 + a])
    check("row 2 (a type-3 OBJECT) is crashable only for classes 0 and 3",
          [tbl[14 + a] for a in range(7)], [1, 0, 0, 1, 0, 0, 0])
    check("a racecar handle (type 0) maps to class 0",
          int(C.ask("objclass 0 0").split()[1]), 0)
    check("a type-3 prop handle maps to class 2",
          int(C.ask("objclass 3 0").split()[1]), 2)
    check("the designated big-hit traffic vehicle (type 4) maps to class 3",
          int(C.ask("objclass 4 1").split()[1]), 3)
    check("ordinary traffic (type 4) maps to class 5",
          int(C.ask("objclass 4 0").split()[1]), 5)
    check("racecar vs prop IS crashable",
          int(C.ask("objtable 2 0").split()[1]), 1)
    check("the big-hit traffic vehicle vs prop IS crashable",
          int(C.ask("objtable 2 3").split()[1]), 1)
    check("ordinary traffic vs prop is NOT",
          int(C.ask("objtable 2 5").split()[1]), 0)

    # ---- the gate, through the port -------------------------------------
    def obj(slot, t, vms, nx=-1.0, nz=0.0, mass=1200.0, ocls=2, ccls=0,
            immune=0):
        """v_rel = the object's point velocity minus the car's: a static prop
        against a car closing at `vms` along -n.  ocls/ccls are the recovered
        FUN_0010FBC0 classes (2 = a type-3 prop, 0 = a racecar)."""
        return C.ask("obj %d %.6f %.6f 0 %.6f %.6f 0 %.6f %.3f %d %d %d"
                     % (slot, t, -vms * -nx, -vms * -nz, nx, nz, mass,
                        ocls, ccls, immune)).split()

    C.cmd("reset 2"); C.cmd("car 0 0 0"); C.cmd("car 1 1 1")
    mph = 2.2369363
    r = obj(0, 0.0, 40.0)
    check("player at 40 m/s into a prop: closing mph", float(r[3]),
          40.0 * mph, 1e-3)
    check("player at 40 m/s (89.5 mph) into a prop: WRECKS", int(r[2]), 1)
    check("the threshold is authority * 75", float(r[4]), 75.0, 1e-4)
    check("damage = mass*2*mph*0.1*0.5", float(r[6]),
          1200.0 * 2.0 * (40.0 * mph) * 0.1 * 0.5, 1e-1)
    r = obj(0, 0.0, 30.0)
    check("player at 30 m/s (67 mph): survives (under 75)", int(r[2]), 0)

    # a purely GLANCING contact has no normal-direction closing speed, so the
    # object test ignores it exactly as the wall test's head-on gate would
    r = C.ask("obj 0 0.0 %.6f 0 0 -1 0 0 1200 2 0 0" % 0.0).split()
    check("no closing speed along the normal: no object crash", int(r[2]), 0)

    # the two vetoes
    r = obj(0, 0.0, 60.0, immune=1)
    check("veh+0x152C >= 0 (post-spawn immunity): no object crash",
          int(r[1]), 0)
    r = obj(0, 0.0, 60.0, ccls=5)   # ordinary traffic vs a prop
    check("a non-crashable class pair: no object crash", int(r[1]), 0)
    r = obj(0, 0.0, 60.0, ccls=3)   # the DESIGNATED big-hit traffic vehicle
    check("the designated big-hit traffic vehicle DOES object-crash",
          int(r[2]), 1)

    # the ladder's |= 0x18 kills bit 4 as well as bit 3
    C.cmd("reset 2"); C.cmd("car 0 1 0"); C.cmd("car 1 1 1")
    C.cmd("view 0 %.9g 0" % (0.9 * A.rf(E.K_VIEW_R2)))
    r = obj(0, 0.0, 60.0)
    check("crash entries off (veh+0x1353 & 0x10): no object crash",
          int(r[2]), 0)
    C.cmd("view 0 -1 0")

    # a slammed car: 0.05 authority -> 3.75 mph
    C.cmd("reset 2"); C.cmd("car 0 1 0"); C.cmd("car 1 0 1")
    C.cmd("set 0 slam_time 0.5"); C.cmd("set 0 aggressor 1")
    C.cmd("set 0 aggressor_time 0.5")
    r = obj(0, 1.0, 2.0)
    check("slammed victim: object threshold 3.75 mph", float(r[4]), 3.75,
          1e-4)
    check("slammed victim: 2 m/s (4.5 mph) prop tap WRECKS it", int(r[2]), 1)

    # crash party uses the LENGTH of v_rel against a *20 threshold
    C.cmd("reset 2"); C.cmd("car 0 0 0"); C.cmd("car 1 1 1")
    C.cmd("crashmode 1")
    r = C.ask("obj 0 0.0 0 0 -12.0 -1 0 0 1200 2 0 0").split()
    check("crash party: |v_rel| (not the normal part) drives the test",
          float(r[3]), 12.0 * mph, 1e-3)
    check("crash party threshold is authority * 20", float(r[4]), 20.0, 1e-4)
    check("crash party: 12 m/s sideways along a prop WRECKS", int(r[2]), 1)
    C.cmd("crashmode 0")


def sec15_wall_scrape(C):
    """FUN_0011AEF0's chassis response for a NON-crashing scrape: the branch
    structure out of the image, then the ported response's behaviour."""
    print("\n-- 15. the wall-scrape chassis response (FUN_0011AEF0)")
    data = open(os.path.join(_root, 'build', 'burnout3.elf'), 'rb').read()
    ph_off = struct.unpack_from('<I', data, 0x1C)[0]
    ph_num = struct.unpack_from('<H', data, 0x2C)[0]
    segs = [s for s in (struct.unpack_from('<IIIIIIII', data, ph_off + i * 32)
                        for i in range(ph_num)) if s[0] == 1]

    def rd(va, n):
        for t, o, v, _p, f, _m, _fl, _a in segs:
            if v <= va < v + f:
                return data[o + (va - v):o + (va - v) + n]
        return b'\0' * n

    # (a) the response is UNCONDITIONAL on the wall path: the only branch
    #     between the flattened normal and the crash tail is `dv > 0`.
    check("0x0011B764 COMISS dv, [0x003B16E0] (= 0.0)", rd(0x0011B764, 7),
          b'\x0f\x2f\x1d\xe0\x16\x3b\x00')
    check("0x0011B771 JBE straight to the crash tail 0x0011B909",
          rd(0x0011B771, 6), b'\x0f\x86\x92\x01\x00\x00')

    # (b) the deflection push-out scale
    check("0x0011B48F MULSS [0x003B1870] (= 1.5)", rd(0x0011B48F, 8),
          b'\xf3\x0f\x59\x05\x70\x18\x3b\x00')

    # (c) the brake gate that chooses linear-only vs the at-point routing
    check("0x0011B8A2 loads veh+0x1404", rd(0x0011B8A2, 8),
          b'\xf3\x0f\x10\x87\x04\x14\x00\x00')
    check("0x0011B8AA compares it with 0.1 [0x003A69C4]", rd(0x0011B8AA, 7),
          b'\x0f\x2f\x05\xc4\x69\x3a\x00')
    check("0x0011B8C2 JBE -> the FUN_001206D0 at-point path",
          rd(0x0011B8C2, 2), b'\x76\x22')
    check("0x0011B904 CALL FUN_001206D0", rd(0x0011B904, 1), b'\xe8')

    # (d) the -1000 * mass * dir brake is NOT part of a static-wall scrape:
    #     it lives inside the SECOND poly set's wall-count guard.
    check("0x0011B0BA loads -1000.0 [0x003B1744]", rd(0x0011B0BA, 8),
          b'\xf3\x0f\x10\x05\x44\x17\x3b\x00')
    check("0x0011B09F CMP the wall count against its pre-set2 value",
          rd(0x0011B09F, 4), b'\x3b\x4c\x24\x10')
    check("0x0011B0A3 JLE skips the -1000 term", rd(0x0011B0A3, 2),
          b'\x7e\x5b')
    check("the -1000 term writes veh+0xF0 (force_acc), not the impulse",
          rd(0x0011B0F0, 7), b'\x0f\x29\x87\xf0\x00\x00\x00')

    # ---- the ported response -------------------------------------------
    def resp(px, pz, nx, nz, vx, vz, brake=0.0, mass=1200.0):
        spd = (vx * vx + vz * vz) ** 0.5
        return [float(x) for x in C.ask(
            "resp 0 %.6f 0 %.6f %.6f 0 %.6f %.6f 0 %.6f %.3f %.6f %.6f"
            % (px, pz, nx, nz, vx, vz, mass, spd, brake)).split()[1:]]

    # head-on into a wall whose normal is -z, contact on the centre line:
    # no lever arm, so no yaw at all.
    r = resp(0.0, 2.2, 0.0, -1.0, 0.0, 30.0)
    check("head-on centre contact: valid", int(r[0]), 1)
    check("head-on centre contact: NO yaw impulse", abs(r[7]), 0.0, 1e-3)
    check("head-on centre contact: the head-on scrub fires (0.707 < 1.0)",
          r[1], (1.0 - (1.0 - 0.707) * 0.1) * 0.99, 1e-6)

    # the shallow clip the harness used to PIVOT on: the car is travelling
    # mostly along +z with a small +x drift into a wall on its right (normal
    # -x), touching at the FRONT right corner.  Retail's point impulse then
    # has a lever arm and must produce a yaw that turns the nose AWAY.
    r = resp(1.0, 2.2, -1.0, 0.0, 4.0, 40.0)
    check("shallow front-corner clip: valid", int(r[0]), 1)
    check("shallow front-corner clip: a NON-ZERO yaw impulse", abs(r[7]) > 1.0,
          True)
    check("shallow front-corner clip: the impulse pushes off the wall (-x)",
          r[3] < 0.0, True)
    check("shallow front-corner clip: the yaw turns the nose off the wall",
          r[7] < 0.0, True)
    check("shallow clip: only the 0.99 scrub (head-on under 0.707)", r[1],
          0.99, 1e-6)
    check("shallow front-corner clip: taken at the contact POINT",
          int(r[9]), 1)
    # the mirrored corner mirrors the yaw
    r2 = resp(-1.0, 2.2, 1.0, 0.0, -4.0, 40.0)
    check("the mirrored clip mirrors the yaw", r2[7], -r[7], 1e-3)

    # braking suppresses the torque entirely (veh+0x1404 > 0.1)
    rb_ = resp(1.0, 2.2, -1.0, 0.0, 4.0, 40.0, brake=0.5)
    check("braking (veh+0x1404 > 0.1): linear only, no yaw", abs(rb_[7]),
          0.0, 1e-6)
    check("braking: the same linear impulse", rb_[3], r[3], 1e-3)
    check("braking: at_point clear", int(rb_[9]), 0)


# ==========================================================================
def main():
    # slam-kind ids the emulator module exposes for the driver commands
    E.B3K_RUB, E.B3K_WALL = 1, 2
    E.B3K_SIDE_LIGHT, E.B3K_REAR_LIGHT = 3, 4
    E.B3K_SIDE, E.B3K_REAR = 5, 6
    E.UC_X86_REG_EAX = __import__('unicorn.x86_const',
                                  fromlist=['UC_X86_REG_EAX']).UC_X86_REG_EAX

    want = sys.argv[1] if len(sys.argv) > 1 else None
    C = CDriver()
    secs = [('1', sec1_slam_never_wrecks), ('2', sec2_dispatch),
            ('3', sec3_gate), ('4', sec4_wall_shunt),
            ('5', sec5_attribution), ('6', sec6_claim_scan),
            ('7', sec7_commit), ('8', sec8_messages), ('9', sec9_denied),
            ('10', sec10_full_flow)]
    for tag, fn in secs:
        if want in (None, tag):
            fn(C)
    if want in (None, '11'):
        sec11_constants()
    if want in (None, '12'):
        sec12_authority(C)
    if want in (None, '13'):
        sec13_ladder_input(C)
    if want in (None, '14'):
        sec14_object_trigger(C)
    if want in (None, '15'):
        sec15_wall_scrape(C)
    C.cmd("quit")

    print("\n================ %d/%d ================" % (PASS, PASS + FAIL))
    for f in FAILURES:
        print("  FAIL " + f)
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
