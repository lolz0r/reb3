/* Android-only glue for the Burnout 3 RE harness.  See b3_android.c.
 * Included from src/burnout3_full.c inside #ifdef __ANDROID__ only; the
 * desktop build never sees this header (it is not on the Makefile's -I). */
#ifndef B3_ANDROID_H
#define B3_ANDROID_H

#ifdef __cplusplus
extern "C" {
#endif

/* chdir into the extracted asset tree, pump stdio to logcat, set the
 * phone-appropriate env defaults.  Call FIRST, before any asset path is
 * touched -- i.e. at the top of main(). */
void b3_android_boot(void);

/* initialize_gl4es().  Call right after SDL_GL_CreateContext() succeeds and
 * before the first gl* call. */
void b3_android_gl_init(void);

/* Absolute path of the extracted asset root (NULL before b3_android_boot). */
const char* b3_android_asset_root(void);

/* ---- touch + tilt controls (b3_touch.c) --------------------------------
 * Feed every SDL_Event to b3_touch_event(); read the merged state once per
 * frame with b3_touch_state(); draw the overlay last, just before the swap.
 * The event parameter is void* so this header needs no SDL include. */
void b3_touch_event(const void* sdl_event);
/* steer in [-1,1] from the accelerometer (0 while flat/deadzone),
 * gas/brake in [0,1], boost 0/1 from the on-screen buttons. */
void b3_touch_state(float* steer, float* gas, float* brake, int* boost);
/* Semi-transparent button overlay; pass the CURRENT window pixel size. */
void b3_touch_draw(int win_w, int win_h);

#ifdef __cplusplus
}
#endif
#endif /* B3_ANDROID_H */
