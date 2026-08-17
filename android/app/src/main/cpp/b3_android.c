/* ==========================================================================
 * b3_android.c -- Android-only bootstrap for the Burnout 3 RE harness.
 *
 * This file exists ONLY in the Android build (android/app/src/main/cpp); it
 * is never compiled by the repo-root Makefile.  Everything it does is the
 * platform glue the desktop gets for free:
 *
 *   1. chdir() into the extracted asset tree, so every "build/..." relative
 *      path in src/*.c resolves unchanged.  MainActivity.java has already
 *      unpacked assets/burnout3_assets.zip into the app's internal storage
 *      and dropped a marker file there.
 *   2. Pump stdout/stderr into logcat -- the harness prints its whole
 *      recovery ledger through printf and Android throws that away.
 *   3. Bring gl4es up once SDL's GLES2 context is current.  gl4es is built
 *      with NO_INIT_CONSTRUCTOR, so initialize_gl4es() must be called by
 *      hand, after the context exists and before the first gl* call.
 *
 * Declarations live in b3_android.h, which src/burnout3_full.c includes from
 * inside #ifdef __ANDROID__.
 * ========================================================================== */
#include "b3_android.h"

#include <SDL2/SDL.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gl4esinit.h"

#define TAG "Burnout3"

/* -- 1. asset root ------------------------------------------------------- */

static char g_root[1024];

const char* b3_android_asset_root(void)
{
    return g_root[0] ? g_root : NULL;
}

/* -- 2. stdio -> logcat -------------------------------------------------- */

static int  g_pipe[2] = { -1, -1 };
static pthread_t g_log_thread;

static void* log_pump(void* unused)
{
    char buf[512];
    size_t used = 0;
    (void)unused;
    for (;;) {
        ssize_t n = read(g_pipe[0], buf + used, sizeof buf - used - 1);
        if (n <= 0) break;
        used += (size_t)n;
        buf[used] = '\0';
        char* line = buf;
        char* nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            __android_log_write(ANDROID_LOG_INFO, TAG, line);
            line = nl + 1;
        }
        used = strlen(line);
        memmove(buf, line, used + 1);
        if (used == sizeof buf - 1) {          /* absurdly long line: flush */
            __android_log_write(ANDROID_LOG_INFO, TAG, buf);
            used = 0;
        }
    }
    return NULL;
}

static void start_log_pump(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pipe(g_pipe) != 0) return;
    dup2(g_pipe[1], STDOUT_FILENO);
    dup2(g_pipe[1], STDERR_FILENO);
    pthread_create(&g_log_thread, NULL, log_pump, NULL);
    pthread_detach(g_log_thread);
}

/* -- 3. boot ------------------------------------------------------------- */

void b3_android_boot(void)
{
    start_log_pump();

    const char* path = SDL_AndroidGetInternalStoragePath();
    if (!path) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
                            "no internal storage path: %s", SDL_GetError());
        return;
    }
    snprintf(g_root, sizeof g_root, "%s", path);
    if (chdir(g_root) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
                            "chdir(%s) failed", g_root);
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "asset root: %s", g_root);

    /* Sanity: the extractor should have produced build/ right here. */
    FILE* f = fopen("build/collision.bin", "rb");
    if (f) {
        fclose(f);
    } else {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "build/collision.bin missing under %s -- asset extraction failed",
            g_root);
    }

    /* Defaults that only make sense on a phone/tablet:
     *  - MSAA off: gl4es on GLES2 pays for it twice and tile GPUs hate the
     *    implicit resolves the postfx framebuffer reads force.
     *  - the window is whatever surface Android hands us; B3_RES is only a
     *    desktop convenience, so leave it alone if the user set it via
     *    SDL_ENV meta-data in the manifest.
     * setenv(...,0) => never clobber an explicitly provided value. */
    setenv("B3_MSAA", "0", 0);
    setenv("B3_TRACK", "US_C3_V1", 0);

    /* Landscape only.  The manifest already says sensorLandscape, but SDL's
     * window-creation glue calls setRequestedOrientation() itself and, for a
     * RESIZABLE window with no SDL_HINT_ORIENTATIONS, requests FULL_USER --
     * which follows the sensor straight back into portrait.  The hint is the
     * supported way to keep SDLActivity.setOrientationBis() in landscape. */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

    /* Tilt steering (b3_touch.c) reads the accelerometer through the SDL
     * joystick API; make sure the exposure is on regardless of SDL's
     * compiled default. */
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "1");

    /* Bring-up bisect: the present composite + radial blur grab the back
     * buffer with glCopyTexImage2D through gl4es; suspected source of the
     * all-white world on the first rendering run.  Default them OFF on
     * Android until confirmed good (setenv(...,0) stays overridable). */
    setenv("B3_POSTFX_PRESENT", "0", 0);
    setenv("B3_POSTFX_BLUR", "0", 0);

    /* When a shader fails to build, make gl4es print the (converted) source
     * and the driver's real info log instead of one garbage line -- on the
     * first Pixel bring-up every FPE program failed and the stock message
     * printed an uninitialised buffer.  Costs nothing when nothing fails. */
    setenv("LIBGL_LOGSHADERERROR", "1", 0);
}

/* -- 4. gl4es ------------------------------------------------------------ */

void b3_android_gl_init(void)
{
    /* NOEGL is OFF, so gl4es dlopen()s libEGL/libGLESv2 itself; it still
     * wants a GetProcAddress for extension probing, and SDL's is already
     * bound to the context SDL_GL_CreateContext() just made current.
     *
     * CRITICAL: gl4es' hardware probe (glx/hardext.c GetHardwareExtensions)
     * creates its OWN pbuffer surface + context, eglMakeCurrent()s it to
     * read the driver's caps, and then unbinds with eglMakeCurrent(dpy, 0,
     * 0, EGL_NO_CONTEXT) -- which leaves the calling thread with NO current
     * context at all.  SDL still believes its context is current, so from
     * then on every real GLES call is the Android trampoline's silent no-op
     * (glCreateShader()==0 with glGetError()==0), the first FPE program
     * "fails" to compile, and gl4es crashes on the resulting NULL glprogram
     * (realize_glenv, fpe.c:1138).  Save the EGL binding around the init
     * and restore it with raw EGL -- SDL_GL_MakeCurrent() can't be trusted
     * here precisely because SDL thinks nothing changed. */
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    void* (*get_dpy)(void)     = egl ? dlsym(egl, "eglGetCurrentDisplay") : NULL;
    void* (*get_ctx)(void)     = egl ? dlsym(egl, "eglGetCurrentContext") : NULL;
    void* (*get_surf)(int)     = egl ? dlsym(egl, "eglGetCurrentSurface") : NULL;
    unsigned (*make_cur)(void*, void*, void*, void*) =
                                 egl ? dlsym(egl, "eglMakeCurrent") : NULL;
    void* dpy  = get_dpy  ? get_dpy()  : NULL;
    void* ctx  = get_ctx  ? get_ctx()  : NULL;
    void* draw = get_surf ? get_surf(0x3059 /*EGL_DRAW*/) : NULL;
    void* read = get_surf ? get_surf(0x305A /*EGL_READ*/) : NULL;

    set_getprocaddress((void* (*)(const char*))SDL_GL_GetProcAddress);
    initialize_gl4es();

    if (make_cur && dpy && ctx) {
        unsigned ok = make_cur(dpy, draw, read, ctx);
        __android_log_print(ANDROID_LOG_INFO, TAG,
            "gl4es initialised (SDL context rebound: %s)",
            ok ? "ok" : "FAILED");
    } else {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "gl4es initialised (no EGL binding captured -- raw GLES calls "
            "may be dead)");
    }
}
