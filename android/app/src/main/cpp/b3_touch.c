/* ==========================================================================
 * b3_touch.c -- Android touch buttons + tilt steering for the B3 RE harness.
 *
 * Android-only (compiled from android/app/src/main/cpp, never by the repo
 * Makefile).  Three semi-transparent buttons and accelerometer steering:
 *
 *   left edge   BRAKE   (octagon icon)
 *   right edge  GAS     (triangle icon), BOOST above it (double chevron)
 *   tilt        steering wheel style -- roll the phone about the axis
 *               perpendicular to the screen
 *
 * The buttons are ANALOG-shaped but report 0/1 (retail's digital A-button
 * throttle); steering is analog like a pad stick.  All geometry lives in
 * height-relative units so any landscape aspect gets the same physical
 * button size, and the hit area is ~35% larger than the drawn disc.
 *
 * Tilt: SDL exposes the accelerometer as a joystick ("Android
 * Accelerometer", 3 axes, +/-32767 == +/-1 g), already rotated into
 * SCREEN coordinates by SDLSurface.onSensorChanged() -- axis 0 is the
 * displayed horizontal (0 at rest, positive on clockwise roll, same sign
 * in both sensorLandscape rotations), axis 1 the at-rest +1 g vertical.
 * Steering therefore reads axis 0 and needs no rotation handling here.
 * B3_TILT_SIGN=-1 flips it if a device reports oddly; B3_TILT_LOCK_G sets
 * the full-lock tilt (default 0.42 g ~ 25 degrees).
 * ========================================================================== */
#include "b3_android.h"

#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- layout (x,y = disc centre in [0,1] of the window; r in units of
 *      window HEIGHT).  y grows DOWN to match SDL touch coordinates. ---- */
#define BTN_GAS    0
#define BTN_BRAKE  1
#define BTN_BOOST  2
#define BTN_N      3

typedef struct { float x, y, r; } btn_geo_t;

static const btn_geo_t BTN[BTN_N] = {
    { 0.925f, 0.640f, 0.095f },   /* GAS   -- right thumb rest        */
    { 0.075f, 0.640f, 0.095f },   /* BRAKE -- left thumb rest         */
    { 0.925f, 0.330f, 0.070f },   /* BOOST -- slide the thumb up      */
};
#define HIT_SCALE 1.35f

/* ---- state ------------------------------------------------------------- */
static int          g_down[BTN_N];                /* held this instant     */
static SDL_FingerID g_finger_of[BTN_N];
static float        g_aspect = 16.0f / 9.0f;      /* w/h, updated per draw */

static SDL_Joystick* g_accel     = NULL;
static int           g_accel_try = 0;
static float         g_tilt_lp   = 0.0f;          /* low-passed steer      */

static int btn_hit(int b, float fx, float fy)
{
    /* touch coords are normalised per axis; convert dx to height units */
    float dx = (fx - BTN[b].x) * g_aspect;
    float dy = (fy - BTN[b].y);
    float r  = BTN[b].r * HIT_SCALE;
    return dx * dx + dy * dy <= r * r;
}

void b3_touch_event(const void* ev)
{
    const SDL_Event* e = (const SDL_Event*)ev;
    if (e->type != SDL_FINGERDOWN && e->type != SDL_FINGERUP
        && e->type != SDL_FINGERMOTION)
        return;

    /* this finger first releases whatever it held ... */
    for (int b = 0; b < BTN_N; b++)
        if (g_down[b] && g_finger_of[b] == e->tfinger.fingerId)
            g_down[b] = 0;

    /* ... and re-claims whatever it is on now (motion can slide between) */
    if (e->type != SDL_FINGERUP) {
        for (int b = 0; b < BTN_N; b++) {
            if (btn_hit(b, e->tfinger.x, e->tfinger.y)) {
                g_down[b]      = 1;
                g_finger_of[b] = e->tfinger.fingerId;
            }
        }
    }
}

/* ---- tilt -------------------------------------------------------------- */

static void accel_open_once(void)
{
    if (g_accel || g_accel_try) return;
    g_accel_try = 1;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        const char* n = SDL_JoystickNameForIndex(i);
        if (n && strstr(n, "Accelerometer")) {
            g_accel = SDL_JoystickOpen(i);
            break;
        }
    }
    if (!g_accel)
        SDL_Log("[b3_touch] no accelerometer joystick -- tilt steer off");
}

static float tilt_steer(void)
{
    static float lock_g = 0.0f;
    static int   sign_env = 0;
    if (lock_g == 0.0f) {
        const char* e = getenv("B3_TILT_LOCK_G");
        lock_g = e ? (float)atof(e) : 0.42f;
        if (lock_g < 0.1f) lock_g = 0.42f;
        e = getenv("B3_TILT_SIGN");
        sign_env = (e && atoi(e) < 0) ? -1 : 1;
    }

    accel_open_once();
    if (!g_accel) return 0.0f;

    /* SDL's Java glue (SDLSurface.onSensorChanged) has ALREADY rotated the
     * accelerometer into SCREEN coordinates before onNativeAccel(), per
     * display rotation -- so axis 0 is "along the displayed horizontal",
     * reads 0 at rest in landscape, goes positive when the phone is rolled
     * clockwise, and keeps that sign in BOTH sensorLandscape rotations.
     * (Axis 1 is the at-rest +1 g axis -- reading THAT pegged the steering
     * and drove the car in circles on the first device test.) */
    SDL_JoystickUpdate();
    float gx = SDL_JoystickGetAxis(g_accel, 0) / 32767.0f;

    float s = (float)sign_env * gx / lock_g;

    /* deadzone then re-scale so full lock stays reachable */
    const float dz = 0.10f;
    if (fabsf(s) < dz) s = 0.0f;
    else s = (s - (s > 0 ? dz : -dz)) / (1.0f - dz);
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;

    /* one-pole low-pass -- accelerometers are noisy at 60 Hz */
    g_tilt_lp += 0.35f * (s - g_tilt_lp);
    return g_tilt_lp;
}

void b3_touch_state(float* steer, float* gas, float* brake, int* boost)
{
    if (steer) *steer = tilt_steer();
    if (gas)   *gas   = g_down[BTN_GAS]   ? 1.0f : 0.0f;
    if (brake) *brake = g_down[BTN_BRAKE] ? 1.0f : 0.0f;
    if (boost) *boost = g_down[BTN_BOOST];
}

/* ---- overlay ----------------------------------------------------------- */

static void draw_disc(float cx, float cy, float r, float rr, float gg,
                      float bb, float a)
{
    glColor4f(rr, gg, bb, a);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= 28; i++) {
        float t = (float)i * (float)(2.0 * M_PI / 28.0);
        glVertex2f(cx + cosf(t) * r, cy + sinf(t) * r);
    }
    glEnd();
}

static void draw_ring(float cx, float cy, float r, float a)
{
    glColor4f(1.0f, 1.0f, 1.0f, a);
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 28; i++) {
        float t = (float)i * (float)(2.0 * M_PI / 28.0);
        glVertex2f(cx + cosf(t) * r, cy + sinf(t) * r);
    }
    glEnd();
}

void b3_touch_draw(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    g_aspect = (float)w / (float)h;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, 1.0, 1.0, 0.0, -1.0, 1.0);   /* y down like touch coords */
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    /* height-relative geometry: undo the aspect on X inside the drawers by
     * scaling the projection instead -- draw in (x*aspect, y) space. */
    glScalef(1.0f / g_aspect, 1.0f, 1.0f);

    GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean tex   = glIsEnabled(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (int b = 0; b < BTN_N; b++) {
        float cx = BTN[b].x * g_aspect;   /* into height units */
        float cy = BTN[b].y;
        float r  = BTN[b].r;
        int   on = g_down[b];

        float tint_r = 1.0f, tint_g = 1.0f, tint_b = 1.0f;
        if (b == BTN_GAS)   { tint_r = 0.35f; tint_g = 1.0f;  tint_b = 0.45f; }
        if (b == BTN_BRAKE) { tint_r = 1.0f;  tint_g = 0.35f; tint_b = 0.35f; }
        if (b == BTN_BOOST) { tint_r = 1.0f;  tint_g = 0.65f; tint_b = 0.15f; }

        draw_disc(cx, cy, r, tint_r, tint_g, tint_b, on ? 0.42f : 0.16f);
        draw_ring(cx, cy, r, on ? 0.65f : 0.30f);

        /* icon, white */
        glColor4f(1.0f, 1.0f, 1.0f, on ? 0.85f : 0.42f);
        float s = r * 0.45f;
        if (b == BTN_GAS) {                    /* up triangle */
            glBegin(GL_TRIANGLES);
            glVertex2f(cx,     cy - s);
            glVertex2f(cx - s, cy + s * 0.8f);
            glVertex2f(cx + s, cy + s * 0.8f);
            glEnd();
        } else if (b == BTN_BRAKE) {           /* octagon (stop) */
            glBegin(GL_TRIANGLE_FAN);
            glVertex2f(cx, cy);
            for (int i = 0; i <= 8; i++) {
                float t = (float)(i * (2.0 * M_PI / 8.0) + M_PI / 8.0);
                glVertex2f(cx + cosf(t) * s, cy + sinf(t) * s);
            }
            glEnd();
        } else {                               /* boost: double chevron up */
            glBegin(GL_TRIANGLES);
            for (int k = 0; k < 2; k++) {
                float oy = (k ? 0.55f : -0.35f) * s;
                glVertex2f(cx,     cy - s * 0.75f + oy);
                glVertex2f(cx - s, cy + s * 0.15f + oy);
                glVertex2f(cx + s, cy + s * 0.15f + oy);
            }
            glEnd();
        }
    }

    if (tex)   glEnable(GL_TEXTURE_2D);
    if (depth) glEnable(GL_DEPTH_TEST);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}
