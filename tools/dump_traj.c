// Trajectory driver for the full-pipeline differential test.
//
// Runs b3_vehicle_step_full() (the C port of FUN_0011BE50's main path +
// FUN_0011ECF0) from the same COMPCAR1 initial state the Unicorn session
// (tools/emulate_pipeline.py) seeds, over the same scenario input tapes,
// on the same flat-plane ground, and prints one JSON object per frame.
// validate_port.py compares this against the real-code trajectory.
//
// Build: cc -O2 -Isrc -o build/dump_traj tools/dump_traj.c \
//        src/burnout3_vehicle_sim.c -lm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "burnout3_vehicle_sim.h"
#include "burnout3_car_physics.h"
// The relocated FUN_0011AEF0 slot in b3_vehicle_step_full's substep calls
// through B3VehicleFull.chassis_resolve, and the response it points at
// (b3_crash_response) lives in burnout3_crash.c.  The Makefile rule for this
// driver links burnout3_vehicle_sim.c only -- deliberately, so that every
// other consumer of the sim keeps its dependency set -- so the response is
// pulled in here as a second translation unit.  Nothing else links this
// file, so there is no duplicate definition anywhere.
#include "burnout3_crash.c"
// PHYS-LEDGER-4 / PH-05: the flying-part seeding law (FUN_00109BB0 and
// FUN_001069C0's box centre) lives in burnout3_panels.c, which the Makefile
// rule for this driver now links as a THIRD translation unit (it cannot be
// textually included: it has its own copies of the static mirror_* helpers).
#include "burnout3_panels.h"

// flat plane at y=0, surface type 0 (mirrors the seeded soup)
int b3_ground_probe(float x, float y, float z,
                    float* out_height, float out_normal[3]) {
    (void)x; (void)y; (void)z;
    *out_height = 0.0f;
    out_normal[0] = 0.0f;
    out_normal[1] = 1.0f;
    out_normal[2] = 0.0f;
    return 0;
}

typedef struct { int frames; float th, br, st; int boost; } Phase;

static const Phase SC_ACCEL[]  = {{60, 0, 0, 0, 0}, {240, 1, 0, 0, 0}, {0}};
static const Phase SC_CORNER[] = {{60, 0, 0, 0, 0}, {180, 1, 0, 0, 0},
                                  {120, 0.6f, 0, 0.5f, 0}, {0}};
static const Phase SC_BRAKE[]  = {{60, 0, 0, 0, 0}, {180, 1, 0, 0, 0},
                                  {150, 0, 1, 0, 0}, {0}};

static void emit(const B3VehicleFull* v) {
    printf("{\"pos\":[%.9g,%.9g,%.9g],\"vel\":[%.9g,%.9g,%.9g],"
           "\"speed\":%.9g,\"dir\":[%.9g,%.9g,%.9g],"
           "\"omega\":[%.9g,%.9g,%.9g],\"angmom\":[%.9g,%.9g,%.9g],"
           "\"at\":[%.9g,%.9g,%.9g],\"up\":[%.9g,%.9g,%.9g],"
           "\"rpm\":%.9g,\"gear\":%d,\"torque\":%.9g,"
           "\"steer_deg\":%.9g,\"drift\":%d,\"slide\":%.9g,"
           "\"airborne\":%d,\"c212\":%d,\"cstate\":%d,\"impact\":%.9g,"
           "\"cfire\":%d,\"wheels\":[",
           v->rb.frame[3][0], v->rb.frame[3][1], v->rb.frame[3][2],
           v->rb.vel[0], v->rb.vel[1], v->rb.vel[2],
           v->rb.vel[3],
           v->rb.dir[0], v->rb.dir[1], v->rb.dir[2],
           v->rb.omega[0], v->rb.omega[1], v->rb.omega[2],
           v->rb.angmom[0], v->rb.angmom[1], v->rb.angmom[2],
           v->rb.frame[2][0], v->rb.frame[2][1], v->rb.frame[2][2],
           v->rb.frame[1][0], v->rb.frame[1][1], v->rb.frame[1][2],
           v->trans.omega * 9.549296f, v->trans.gear,
           v->drive_torque_1520, v->steer_deg_1164,
           v->drift_state_1524, v->slide_1440, v->f1168,
           v->contact_212, v->contact_state_198, v->impact_194,
           v->crash_fired);
    for (int i = 0; i < 4; i++)
        printf("%s{\"cur\":%.9g,\"prev\":%.9g,\"omega\":%.9g,"
               "\"spin\":%.9g,\"contact\":%d}", i ? "," : "",
               v->wheel[i].cur_len, v->wheel[i].prev_len,
               v->wheel[i].omega, v->wheel[i].spin, v->wheel[i].contact);
    printf("]}\n");
}

// ---- checkpoint mode: reseed the full struct from an emulation state ----
// file of "key value" lines (emulate_pipeline.Pipeline.export_state), then
// step N frames with fixed inputs. Usage:
//   dump_traj --state <file> <frames> <throttle> <brake> <steer> <boost>
static float g_kv_val(const char* buf, const char* key, float dflt) {
    const char* p = buf;
    size_t kl = strlen(key);
    while (p) {
        if (!strncmp(p, key, kl) && p[kl] == ' ')
            return (float)atof(p + kl + 1);
        p = strchr(p, '\n');
        if (p) p++;
    }
    return dflt;
}

static void load_state(B3VehicleFull* v, const char* path) {
    static char buf[65536];
    FILE* f = fopen(path, "r");
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    char key[64];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            snprintf(key, sizeof key, "frame_%d_%d", r, c);
            v->rb.frame[r][c] = g_kv_val(buf, key, v->rb.frame[r][c]);
        }
    v->rb.vel[0] = g_kv_val(buf, "vel_x", 0);
    v->rb.vel[1] = g_kv_val(buf, "vel_y", 0);
    v->rb.vel[2] = g_kv_val(buf, "vel_z", 0);
    v->rb.vel[3] = g_kv_val(buf, "speed", 0);
    for (int i = 0; i < 3; i++) {
        snprintf(key, sizeof key, "dir_%d", i);
        v->rb.dir[i] = g_kv_val(buf, key, 0);
        snprintf(key, sizeof key, "omega_%d", i);
        v->rb.omega[i] = g_kv_val(buf, key, 0);
        snprintf(key, sizeof key, "angmom_%d", i);
        v->rb.angmom[i] = g_kv_val(buf, key, 0);
        snprintf(key, sizeof key, "defl_%d", i);
        v->rb.deflection[i] = g_kv_val(buf, key, 0);
    }
    for (int i = 0; i < 4; i++) {
        B3WheelSim* w = &v->wheel[i];
        static const char* nm[4] = {"wp", "cp", "pp", "pc"};
        float* dst[4] = {w->world_pos, w->contact_pt, w->prev_pos,
                         w->prev_contact};
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 3; k++) {
                snprintf(key, sizeof key, "w%d_%s_%d", i, nm[j], k);
                dst[j][k] = g_kv_val(buf, key, 0);
            }
        for (int k = 0; k < 3; k++) {
            snprintf(key, sizeof key, "w%d_n_%d", i, k);
            w->normal[k] = g_kv_val(buf, key, k == 1 ? 1.0f : 0.0f);
        }
        snprintf(key, sizeof key, "w%d_torque", i);
        w->torque = g_kv_val(buf, key, 0);
        snprintf(key, sizeof key, "w%d_spin", i);
        w->spin = g_kv_val(buf, key, 0);
        snprintf(key, sizeof key, "w%d_omega", i);
        w->omega = g_kv_val(buf, key, 0);
        snprintf(key, sizeof key, "w%d_prev", i);
        w->prev_len = g_kv_val(buf, key, w->prev_len);
        snprintf(key, sizeof key, "w%d_cur", i);
        w->cur_len = g_kv_val(buf, key, w->cur_len);
        snprintf(key, sizeof key, "w%d_contact", i);
        w->contact = (unsigned char)g_kv_val(buf, key, 0);
        w->frame_y = w->prev_len;
    }
    B3EngineTransmission* t = &v->trans;
    t->omega = g_kv_val(buf, "t_omega", t->omega);
    t->shift_timer = g_kv_val(buf, "t_timer", 0);
    t->shifting = (int)g_kv_val(buf, "t_shifting", 0);
    t->no_upshift = (int)g_kv_val(buf, "t_noupshift", 0);
    t->rev_limit_rpm = g_kv_val(buf, "t_limit", t->rev_limit_rpm);
    t->rng_state = (unsigned)(double)atof(strstr(buf, "t_rng_a ") + 8);
    t->rng_inc = (unsigned)(double)atof(strstr(buf, "t_rng_c ") + 8);
    t->gear = (int)g_kv_val(buf, "t_gear", 0);
    t->upshift_block = g_kv_val(buf, "t_upblk", 0);
    t->downshift_block = g_kv_val(buf, "t_dnblk", 0);
    v->thr_prev_141C = g_kv_val(buf, "thr_prev", 0);
    v->brake_prev_1420 = g_kv_val(buf, "brake_prev", 0);
    v->steer_prev_1424 = g_kv_val(buf, "steer_prev", 0);
    v->drift_time_142C = g_kv_val(buf, "drift_time", 0);
    v->slide_prev_1430 = g_kv_val(buf, "slide_prev", v->slide_prev_1430);
    v->drift_timer_1438 = g_kv_val(buf, "drift_timer", 0);
    v->airtime_143C = g_kv_val(buf, "airtime", 0);
    v->slide_1440 = g_kv_val(buf, "slide", v->slide_1440);
    v->drift_state_1524 = (int)g_kv_val(buf, "drift_state", 0);
    v->f1168 = (unsigned char)g_kv_val(buf, "f1168", 0);
    v->timer_152C = g_kv_val(buf, "timer_152C", -1.0f);
    v->clock = g_kv_val(buf, "clock", 0);
    v->grip_scalar = g_kv_val(buf, "grip", 1.2f);
    // control-state bytes + the out-of-control clocks (FUN_0011ECF0's
    // aggressive-driving-reaction envelope)
    v->class_215 = (unsigned char)g_kv_val(buf, "class_215", v->class_215);
    v->contact_212 = (unsigned char)g_kv_val(buf, "contact_212", 0);
    v->hit_side_153C = (unsigned char)g_kv_val(buf, "hit_side_153C", 0);
    v->flag_b_1446 = (unsigned char)g_kv_val(buf, "flag_b_1446", 0);
    v->ooc_slam_1598 = g_kv_val(buf, "ooc_slam_1598", -1.0f);
    v->ooc_wall_1690 = g_kv_val(buf, "ooc_wall_1690", -1.0f);
    v->launch_time_1350 = g_kv_val(buf, "launch_1350", -100.0f);
    // rebuild the derived matrices the pipeline expects at a frame
    // boundary: inverse frame + world inverse inertia (Rt . I0 . R)
    {
        float(*m)[4] = v->rb.frame;
        float(*inv)[4] = v->rb.inv_frame;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) inv[r][c] = m[r][c];
        float t2;
        t2 = inv[0][1]; inv[0][1] = inv[1][0]; inv[1][0] = t2;
        t2 = inv[0][2]; inv[0][2] = inv[2][0]; inv[2][0] = t2;
        t2 = inv[1][2]; inv[1][2] = inv[2][1]; inv[2][1] = t2;
        float p[4];
        for (int j = 0; j < 4; j++)
            p[j] = m[3][0] * inv[0][j] + m[3][1] * inv[1][j]
                 + m[3][2] * inv[2][j];
        for (int j = 0; j < 4; j++) inv[3][j] = -p[j];
        float Rt[3][4], tmp[3][4];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) Rt[i][j] = m[j][i];
            Rt[i][3] = 0.0f;
        }
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++)
                tmp[i][j] = Rt[i][0] * v->rb.inv_inertia_body[0][j]
                          + Rt[i][1] * v->rb.inv_inertia_body[1][j]
                          + Rt[i][2] * v->rb.inv_inertia_body[2][j];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++)
                v->rb.inv_inertia_world[i][j] =
                    m[i][0] * tmp[0][j] + m[i][1] * tmp[1][j]
                    + m[i][2] * tmp[2][j];
    }
}

// ---- world-contact mode -------------------------------------------------
// `dump_traj --wcontact <file>` reads a flat list of floats/ints describing a
// rigid body plus one narrow-phase contact, runs the REAL C port
// b3_rigid_body_world_contact() (FUN_00109EA0's post-narrow-phase half) and
// prints the mutated body.  validate_port.py drives the identical state
// through the real x86 under Unicorn and compares.
//
// Input order (whitespace separated):
//   frame[16] vel[4] omega[4] angmom[4] force[4] torque[4] impf[4] impt[4]
//   defl[4] invI[12] mass restitution cls attach hit point[4] normal[4]
//   pushout[4]
static int wcontact_mode(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "wcontact: cannot open %s\n", path); return 1; }
    double d;
    float buf[128];
    int n = 0;
    while (n < 128 && fscanf(f, "%lf", &d) == 1) buf[n++] = (float)d;
    fclose(f);
    if (n < 77) { fprintf(stderr, "wcontact: got %d floats, need 77\n", n);
                  return 1; }
    B3RigidBody rb;
    memset(&rb, 0, sizeof rb);
    int k = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) rb.frame[r][c] = buf[k++];
    for (int i = 0; i < 4; i++) rb.vel[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.omega[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.angmom[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.force_acc[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.torque_acc[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.imp_force[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.imp_torque[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.deflection[i] = buf[k++];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) rb.inv_inertia_world[r][c] = buf[k++];
    const float mass = buf[k++];
    const float rest = buf[k++];
    const int cls = (int)buf[k++];
    const int attach = (int)buf[k++];
    const int hit = (int)buf[k++];
    B3WorldContact ct;
    for (int i = 0; i < 4; i++) ct.point[i] = buf[k++];
    for (int i = 0; i < 4; i++) ct.normal[i] = buf[k++];
    for (int i = 0; i < 4; i++) ct.pushout[i] = buf[k++];
    B3WorldContactResult res;
    b3_rigid_body_world_contact(&rb, mass, cls, attach, rest,
                                hit ? &ct : NULL, &res);
    printf("{\"vel\":[%.9g,%.9g,%.9g,%.9g],"
           "\"omega\":[%.9g,%.9g,%.9g,%.9g],"
           "\"angmom\":[%.9g,%.9g,%.9g,%.9g],"
           "\"force\":[%.9g,%.9g,%.9g,%.9g],"
           "\"torque\":[%.9g,%.9g,%.9g,%.9g],"
           "\"impf\":[%.9g,%.9g,%.9g,%.9g],"
           "\"impt\":[%.9g,%.9g,%.9g,%.9g],"
           "\"defl\":[%.9g,%.9g,%.9g,%.9g],"
           "\"pushout\":[%.9g,%.9g,%.9g,%.9g],"
           "\"impact\":%.9g,\"valid\":%d,\"sleep\":%d,"
           "\"impulsed\":%d,\"hit\":%d}\n",
           rb.vel[0], rb.vel[1], rb.vel[2], rb.vel[3],
           rb.omega[0], rb.omega[1], rb.omega[2], rb.omega[3],
           rb.angmom[0], rb.angmom[1], rb.angmom[2], rb.angmom[3],
           rb.force_acc[0], rb.force_acc[1], rb.force_acc[2], rb.force_acc[3],
           rb.torque_acc[0], rb.torque_acc[1], rb.torque_acc[2],
           rb.torque_acc[3],
           rb.imp_force[0], rb.imp_force[1], rb.imp_force[2], rb.imp_force[3],
           rb.imp_torque[0], rb.imp_torque[1], rb.imp_torque[2],
           rb.imp_torque[3],
           rb.deflection[0], rb.deflection[1], rb.deflection[2],
           rb.deflection[3],
           res.pushout[0], res.pushout[1], res.pushout[2], res.pushout[3],
           res.impact, res.valid, res.sleep, res.impulsed, res.grounded);
    return 0;
}

// ---- class-7 (panel / debris) update mode -------------------------------
// `dump_traj --class7 <file>` runs the REAL C port
// b3_rigid_body_class7_update() (FUN_00106D00) over one step.
// Input order: frame[16] vel[4] dir[4] omega[4] angmom[4] force[4] torque[4]
//   impf[4] impt[4] defl[4] invI_body[12] invI_world[12] normal[4]
//   mass com dt suppress unit attach grounded
static int class7_mode(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "class7: cannot open %s\n", path); return 1; }
    double d;
    float buf[160];
    int n = 0;
    while (n < 160 && fscanf(f, "%lf", &d) == 1) buf[n++] = (float)d;
    fclose(f);
    if (n < 87) { fprintf(stderr, "class7: got %d floats, need 87\n", n);
                   return 1; }
    B3RigidBody rb;
    memset(&rb, 0, sizeof rb);
    int k = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) rb.frame[r][c] = buf[k++];
    for (int i = 0; i < 4; i++) rb.vel[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.dir[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.omega[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.angmom[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.force_acc[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.torque_acc[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.imp_force[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.imp_torque[i] = buf[k++];
    for (int i = 0; i < 4; i++) rb.deflection[i] = buf[k++];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) rb.inv_inertia_body[r][c] = buf[k++];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) rb.inv_inertia_world[r][c] = buf[k++];
    B3Class7State st;
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 4; i++) st.normal[i] = buf[k++];
    const float mass = buf[k++];
    const float com = buf[k++];
    const float dt = buf[k++];
    st.suppress_4d0 = (int)buf[k++];
    st.unit_216 = (int)buf[k++];
    st.attach_2ba = (int)buf[k++];
    st.grounded_212 = (int)buf[k++];
    b3_rigid_body_class7_update(&rb, mass, com, &st, dt);
    printf("{\"vel\":[%.9g,%.9g,%.9g,%.9g],"
           "\"dir\":[%.9g,%.9g,%.9g,%.9g],"
           "\"omega\":[%.9g,%.9g,%.9g,%.9g],"
           "\"angmom\":[%.9g,%.9g,%.9g,%.9g],"
           "\"force\":[%.9g,%.9g,%.9g,%.9g],"
           "\"torque\":[%.9g,%.9g,%.9g,%.9g],"
           "\"impf\":[%.9g,%.9g,%.9g,%.9g],"
           "\"impt\":[%.9g,%.9g,%.9g,%.9g],"
           "\"defl\":[%.9g,%.9g,%.9g,%.9g],"
           "\"frame\":[[%.9g,%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g,%.9g],"
           "[%.9g,%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g,%.9g]],"
           "\"suppress\":%d}\n",
           rb.vel[0], rb.vel[1], rb.vel[2], rb.vel[3],
           rb.dir[0], rb.dir[1], rb.dir[2], rb.dir[3],
           rb.omega[0], rb.omega[1], rb.omega[2], rb.omega[3],
           rb.angmom[0], rb.angmom[1], rb.angmom[2], rb.angmom[3],
           rb.force_acc[0], rb.force_acc[1], rb.force_acc[2], rb.force_acc[3],
           rb.torque_acc[0], rb.torque_acc[1], rb.torque_acc[2],
           rb.torque_acc[3],
           rb.imp_force[0], rb.imp_force[1], rb.imp_force[2], rb.imp_force[3],
           rb.imp_torque[0], rb.imp_torque[1], rb.imp_torque[2],
           rb.imp_torque[3],
           rb.deflection[0], rb.deflection[1], rb.deflection[2],
           rb.deflection[3],
           rb.frame[0][0], rb.frame[0][1], rb.frame[0][2], rb.frame[0][3],
           rb.frame[1][0], rb.frame[1][1], rb.frame[1][2], rb.frame[1][3],
           rb.frame[2][0], rb.frame[2][1], rb.frame[2][2], rb.frame[2][3],
           rb.frame[3][0], rb.frame[3][1], rb.frame[3][2], rb.frame[3][3],
           st.suppress_4d0);
    return 0;
}

// ---- the FUN_0011AEF0 substep slot --------------------------------------
// `--soup <file>` (used with `--state`) hands the pipeline the polygon soup
// FUN_0011BC60 would have collected into veh+0x200, and installs the
// relocated chassis-contact resolve at its retail call site.
//
// File format, one polygon per line, whitespace separated:
//     p0x p0y p0z  p1x p1y p1z  p2x p2y p2z  nx ny nz  surface_u16
// Everything is GAME space, which is the pipeline's own space.
#define SOUP_MAX 64
static B3CrashPoly g_soup_poly[SOUP_MAX];
static unsigned short g_soup_flag[SOUP_MAX];
static int g_soup_n = 0;

// FUN_0011BC60 @0x0011BF43: refill veh+0x200 once per frame, before the
// substep loop.  This world is static, so the "collection" is a rebind.
static int soup_freeze(void* user, B3VehicleFull* v) {
    (void)user;
    v->soup.count = g_soup_n;
    v->soup.polys = g_soup_poly;
    v->soup.flags = g_soup_flag;
    return g_soup_n;
}

static void load_soup(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "soup: cannot open %s\n", path); exit(1); }
    double q[13];
    while (g_soup_n < SOUP_MAX) {
        int got = 0;
        for (; got < 13; got++)
            if (fscanf(f, "%lf", &q[got]) != 1) break;
        if (got < 13) break;
        B3CrashPoly* p = &g_soup_poly[g_soup_n];
        for (int k = 0; k < 3; k++) {
            p->p0[k] = (float)q[k];
            p->p1[k] = (float)q[3 + k];
            p->p2[k] = (float)q[6 + k];
            p->n[k]  = (float)q[9 + k];
        }
        p->p0[3] = p->p1[3] = p->p2[3] = p->n[3] = 0.0f;
        g_soup_flag[g_soup_n] = (unsigned short)q[12];
        g_soup_n++;
    }
    fclose(f);
}

// The B3CrashVehicle inputs that live outside the rigid body, seeded to the
// values the Unicorn session carries (probe: veh+0x13A8 = 1.0,
// veh+0x1534 = 1.0, veh+0x1353 = 0, veh+0x153E = 0, veh+0x1434 = 0,
// racecar+0x1920 = 0 -> the class-0 arm, and FUN_00017310 reads
// [0x004D5370] == 0 so the crash-party thresholds are OFF).
static float g_authority = 1.0f;   // veh+0x1534, `--authority`
// veh+0x1353, `--flags1353`.  Bit 3 is the CRASH-ENTRY VETO the authority
// ladder raises (FUN_00105BD0 @0x00105F95 `OR byte [ESI+0x1353],0x18`) and
// FUN_0011AEF0 reads @0x0011B94D `TEST byte [EDI+0x1353],8`; bits 0 and 2
// disable the whole resolve @0x0011AF0C.
static unsigned char g_flags1353 = 0;

static void install_chassis_contact(B3VehicleFull* v) {
    v->soup_freeze        = soup_freeze;
    v->chassis_resolve    = b3_vehicle_chassis_contact;
    v->surface_grip_13A8  = 1.0f;
    v->authority_1534     = g_authority;
    v->drift_dir_1434     = 0.0f;
    v->flags_1353         = g_flags1353;
    v->no_scrub_153E      = 0;
    v->landed_211         = 0;
    v->racecar_class_1920 = 0;
    v->is_class0          = 1;
    v->party_mode         = 0;
}

// PHYS-LEDGER-4 / PH-05.  The flying-part SEEDING differential: the file
// holds  mass  bbmax.xyz  bbmin.xyz  axis, and this prints the inverse
// inertia diagonal FUN_00109BB0 would install plus the recentred box.
static int pieceseed_mode(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "pieceseed: cannot open %s\n", path); return 1; }
    double d;
    float b[8];
    int n = 0;
    while (n < 8 && fscanf(f, "%lf", &d) == 1) b[n++] = (float)d;
    fclose(f);
    if (n < 8) { fprintf(stderr, "pieceseed: need 8 numbers\n"); return 1; }
    const float mass = b[0];
    const float bbmax[3] = { b[1], b[2], b[3] };
    const float bbmin[3] = { b[4], b[5], b[6] };
    float diag[3], rmax[3], rmin[3];
    b3_piece_inertia(mass, bbmax, bbmin, diag);
    b3_piece_recentre(bbmax, bbmin, (int)b[7], rmax, rmin);
    printf("{\"diag\":[%.9g,%.9g,%.9g],"
           "\"bbmax\":[%.9g,%.9g,%.9g],\"bbmin\":[%.9g,%.9g,%.9g]}\n",
           diag[0], diag[1], diag[2],
           rmax[0], rmax[1], rmax[2], rmin[0], rmin[1], rmin[2]);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--wcontact"))
        return wcontact_mode(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "--class7"))
        return class7_mode(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "--pieceseed"))
        return pieceseed_mode(argv[2]);
    const Phase* sc = SC_ACCEL;
    const char* state_file = NULL;
    const char* soup_file = NULL;
    int win_frames = 0;
    float win_th = 0, win_br = 0, win_st = 0;
    int win_bo = 0;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--soup")) {
            soup_file = argv[i + 1];
            for (int j = i + 2; j + 1 < argc; j += 2) {
                if (!strcmp(argv[j], "--authority"))
                    g_authority = (float)atof(argv[j + 1]);
                else if (!strcmp(argv[j], "--flags1353"))
                    g_flags1353 = (unsigned char)strtoul(argv[j + 1], 0, 0);
                else
                    break;
            }
            argc = i;               // trailing options; hide them from below
            break;
        }
    if (argc >= 7 && !strcmp(argv[1], "--state")) {
        state_file = argv[2];
        win_frames = atoi(argv[3]);
        win_th = (float)atof(argv[4]);
        win_br = (float)atof(argv[5]);
        win_st = (float)atof(argv[6]);
        win_bo = argc > 7 ? atoi(argv[7]) : 0;
    } else if (argc > 1) {
        if (!strcmp(argv[1], "corner")) sc = SC_CORNER;
        else if (!strcmp(argv[1], "brake")) sc = SC_BRAKE;
    }
    B3PhysicsConfig cfg;
    b3_physics_defaults(&cfg);
    for (int i = 0; i < 64; i++)
        b3_config_set_by_offset(&cfg, B3_CARPARAMS_COMPCAR1[i].offset,
                                B3_CARPARAMS_COMPCAR1[i].value);

    // Car1.bgv real geometry (same values emulate_pipeline.py seeds)
    static const float wheels_xz[4][2] = {
        {-0.7600f, 1.2379f}, {0.7600f, 1.2379f},
        {-0.7600f, -1.3067f}, {0.7600f, -1.3067f}};
    static const float half_ext[4] = {1.0157f, 1.1222f, 2.0636f, 0.0f};
    static const float center_off[4] = {-1.0157f, -0.1505f, -2.0866f,
                                        2.0636f};
    static const float inv_inertia[3] = {0.0008f, 0.0011f, 0.0013f};
    static const float pos[3] = {0.0f, 0.31f, 0.0f};

    B3VehicleFull v;
    b3_vehicle_full_init(&v, &cfg, wheels_xz, 0.3117f, half_ext,
                         center_off, inv_inertia, pos, 0.0f);
    const float dt = 1.0f / 60.0f;
    if (soup_file) {
        load_soup(soup_file);
        install_chassis_contact(&v);
    }
    if (state_file) {
        load_state(&v, state_file);
        for (int f = 0; f < win_frames; f++) {
            b3_vehicle_step_full(&v, win_th, win_br, win_st, win_bo, dt);
            emit(&v);
        }
        return 0;
    }
    for (const Phase* p = sc; p->frames; p++)
        for (int f = 0; f < p->frames; f++) {
            b3_vehicle_step_full(&v, p->th, p->br, p->st, p->boost, dt);
            emit(&v);
        }
    return 0;
}
