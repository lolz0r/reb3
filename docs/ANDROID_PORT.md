# Android port of the reB3 harness

One checkout, two targets. `make` still produces the desktop binary exactly as
before; `android/` produces an arm64 APK from **the same `src/*.c`**. There is
no Android branch and no forked copy of the harness: every platform difference
lives either inside `#ifdef __ANDROID__` in `src/`, or in a file under
`android/` that the Makefile never sees.

---

## 1. Build both

### Desktop (unchanged)

```sh
make -j4                       # -> ./burnout3 and build/dump_traj
python3 tools/validate_port.py # -> 127/127 cases match the real code
```

Requirements are as they always were: `pkg-config sdl2 SDL2_image`, `-lGL`.
Nothing under `android/` is on the Makefile's include or link path.

### Android

```sh
# one-time: clone the three third-party libs into android/third_party/
./android/fetch_deps.sh

# one-time (or after any extractor rerun): build the asset payload
./android/pack_assets.sh US_C3_V1

# build the APK
cd android
JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
ANDROID_HOME=$HOME/Android/Sdk \
PATH=$JAVA_HOME/bin:$PATH \
./gradlew assembleDebug
# -> android/app/build/outputs/apk/debug/app-debug.apk
```

`android/local.properties` (gitignored) carries `sdk.dir=$HOME/Android/Sdk`;
`ANDROID_HOME` works instead if you would rather not have the file.

Install with `adb install -r android/app/build/outputs/apk/debug/app-debug.apk`.

---

## 2. Toolchain actually used

| Piece | Version | Where it came from |
|---|---|---|
| Android Studio | `AI-261.26222.65.2613.16025427` | `~/Downloads/android-studio-quail3-patch1-linux/` — **used only as an inventory source**; the GUI was never launched, and it ships no SDK and no `sdkmanager` |
| Studio JBR | OpenJDK 25.0.2 | present at `android-studio/jbr`, **not used** — Gradle 8.14.3 does not support Java 25 |
| JDK for Gradle | OpenJDK **21.0.11** | system, `/usr/lib/jvm/java-21-openjdk-amd64` |
| Android SDK | installed fresh into `~/Android/Sdk` | `cmdline-tools;latest` (`commandlinetools-linux-15859902`), licences accepted headless via `sdkmanager --licenses` |
| platform-tools | 37.0.1 | sdkmanager |
| platforms | android-35 | sdkmanager |
| build-tools | 35.0.0 | sdkmanager |
| NDK | **27.2.12479018** (r27c) | sdkmanager |
| CMake | 3.22.1 (+ bundled ninja) | sdkmanager |
| Gradle | 8.14.3 (wrapper) | `services.gradle.org` |
| Android Gradle Plugin | 8.13.2 | `dl.google.com/dl/android/maven2` |

Network was available throughout (`dl.google.com`, `github.com`,
`repo.maven.apache.org` all reachable), so nothing had to be worked around.

### Third-party native libraries (`android/fetch_deps.sh`, all gitignored)

| Library | Pin | Role |
|---|---|---|
| [SDL2](https://github.com/libsdl-org/SDL) | `release-2.32.10` | window / GLES context / audio callback / controllers / touch. Also supplies the `SDLActivity` Java layer we subclass. |
| [SDL2_image](https://github.com/libsdl-org/SDL_image) | `release-2.8.8` | `IMG_Load` for the PNG art. Built **stb-backend, PNG only** — no libpng, no zlib, no submodule checkout. Every other decoder (AVIF/JPG/TIF/WEBP/…) is switched off. |
| [gl4es](https://github.com/ptitSeb/gl4es) | commit `81547d9` | the whole reason this port is possible: desktop GL 2.1 **compatibility** — `glBegin`/`glEnd`, display lists, `GL_QUADS`, `glPushAttrib`, `glTexEnv`, plus GLSL 1.10/1.20 → ESSL 1.00 translation — implemented on GLES 2.0. |

gl4es build flags (set in `android/app/src/main/cpp/CMakeLists.txt`):
`STATICLIB=ON NO_INIT_CONSTRUCTOR=ON USE_ANDROID_LOG=ON NOX11=ON`, and the NDK
toolchain's own `ANDROID=ON` turns on gl4es' `-DNOX11 -DNO_GBM -DDEFAULT_ES=2`.
The loader stays enabled (`NOEGL=OFF`, `NO_LOADER=OFF`) so gl4es `dlopen`s
`libEGL`/`libGLESv2` itself — that is what keeps `libmain.so` free of any
`libGLESv2` `DT_NEEDED`, and therefore free of the `glBindTexture`-style symbol
collision that direct linking would create.

Verified after linking: `libmain.so` has **zero undefined `gl*` symbols** and
`NEEDED` lists only `libSDL2_image libSDL2 libandroid liblog libdl libm libc`.
`glBegin`, `glNewList`, `glCallList`, `glPushAttrib` all resolve to gl4es.

---

## 3. Project layout

```
android/
  fetch_deps.sh              clone SDL2 / SDL2_image / gl4es at pinned tags
  pack_assets.sh             build/  ->  app/src/main/assets/burnout3_assets.zip
  settings.gradle build.gradle gradle.properties gradlew gradle/wrapper/
  local.properties           sdk.dir (gitignored)
  third_party/               the three clones (gitignored)
  app/
    build.gradle             minSdk 24, targetSdk/compileSdk 35, arm64-v8a only
    src/main/AndroidManifest.xml
    src/main/java/org/libsdl/app/*.java     verbatim from SDL2's android-project
    src/main/java/com/b3re/harness/MainActivity.java   extends SDLActivity
    src/main/cpp/CMakeLists.txt             the Android counterpart of the Makefile
    src/main/cpp/b3_android.{c,h}           Android-only glue
    src/main/assets/burnout3_assets.zip     generated, gitignored
```

`app/src/main/cpp/CMakeLists.txt` compiles the **same 19 translation units** the
Makefile's `SRCS` lists — keep the two lists in lockstep when a module is added.
`tools/` and the validators stay host-only and are not in the Android build.

### Include shimming, done without touching the sources

Two things would normally force edits across eight files, and neither did:

* `#include <GL/gl.h>` — resolved by putting `third_party/gl4es/include` on the
  Android target's include path. gl4es ships a genuine desktop `GL/gl.h`.
* `#include <SDL2/SDL.h>` / `<SDL2/SDL_image.h>` — the Debian/pkg-config layout;
  upstream installs those headers flat. CMake generates a directory of one-line
  forwarding headers (`compat_include/SDL2/SDL.h` → `#include <SDL.h>`) and puts
  it first on the include path.

---

## 4. Every `#ifdef __ANDROID__` in `src/` (95 inserted lines, 4 files, 0 deletions)

| File | What and why |
|---|---|
| `burnout3_full.c` (include block) | pulls in `b3_android.h` |
| `burnout3_full.c` `main()` head | `b3_android_boot()` — `chdir()` into the extracted asset tree so every relative `"build/…"` path below resolves unchanged, plus stdio→logcat and `B3_MSAA=0` / `B3_TRACK=US_C3_V1` defaults (set with overwrite=0, so a manifest `SDL_ENV` override still wins) |
| `burnout3_full.c` GL attributes | request an **ES 2.0** profile instead of desktop 2.1; gl4es supplies the 2.1 compatibility surface on top |
| `burnout3_full.c` after context creation | `b3_android_gl_init()` (gl4es has no init constructor), then `SDL_GL_GetDrawableSize()` — SDL ignores the requested window size on Android, so `render_init` must be told the real surface size |
| `burnout3_full.c` `process_input()` | touch fallback: maps screen zones onto the existing `g_keys[]` array (left third → steer, right third → throttle / boost, bottom-centre → brake). Multi-touch aware, one finger per zone. No gameplay code knows a touchscreen exists, and an attached controller keeps working through the untouched `pad_axis()` path. |
| `burnout3_postfx.c`, `burnout3_carfx.c`, `burnout3_trackmesh.c` | `#define SDL_GL_GetProcAddress gl4es_GetProcAddress`. **This one is load-bearing.** All three resolve the GL 2.0 shader entry points by name; on Android `SDL_GL_GetProcAddress` returns the *raw GLES2 driver* symbols, which cannot compile these files' GLSL 1.10 sources and, worse, sit outside the program/uniform state gl4es maintains for its fixed-function emulation — the next immediate-mode draw would silently unbind the program. Routing the lookup through gl4es fixes both. |

Nothing was changed in `burnout3_vehicle_sim.c`, `burnout3_crash.c` or
`burnout3_props.c` (sibling-owned), and no shim there turned out to be needed.

The `src/` diff is **pure addition** — 95 `+` lines, zero `-` lines — and a
mechanical check confirms every added line sits between an `#ifdef __ANDROID__`
and its `#endif`. With `__ANDROID__` undefined the preprocessor output is
therefore identical to before the port.

---

## 5. Assets

`build/` is **6.7 GiB**: 4.3 GiB of it `audio/`, 722 MiB `music/`, 423 MiB
`tracks/`, 134 MiB `cars/`, and several GiB of `debug_dump_*.bmp` captures.
That cannot ship, so `android/pack_assets.sh` builds the subset the harness
actually opens for **one** track and zips it rooted at `build/…`:

```
staged (uncompressed)   239 MiB
burnout3_assets.zip     143 MiB      (stored, not re-compressed, in the APK)
```

| Included | Size | Note |
|---|---|---|
| `build/cars/` | 133 MiB | whole roster + traffic meshes/textures; the 8 grid slots and the `.bgd` traffic set between them reach most of it |
| `build/music/track_00..01.wav` | 39 MiB | `B3_MUSIC_TRACKS=N` knob (default 2 of 44) |
| `build/audio/` | 24 MiB | the four SFX banks `awd_crashmod` / `awd_single` / `awd_generic` / `awd_<TRACK>`, plus **only** the `eng_*.wav` rpm loops from every `awd_pveh_*_high/` |
| `build/track.obj` | 16 MiB | taken from `tracks/<TRACK>/track.obj` |
| `build/textures/`, `build/frontend/` | 12 MiB each | |
| `build/tracks/<TRACK>/` | 3.1 MiB | only `envmap.png`, `light_probes.bin`, `props.bin` — the three files read at runtime |
| `build/collision.bin` | 2.4 MiB | from `tracks/<TRACK>/collision.bin` |
| `build/postfx/<TRACK>_*` | 236 KiB | clouds / gradients / env for this track only |
| `Globalus.bin`, `mixer.cfg`, `carfx/`, `particlefx/`, `boostfx/` | < 400 KiB | |

**Cut** (recoverable by sideloading into the app's internal storage, or by
widening the script): every other track's `tracks/`, `audio/` and `postfx/`
data; `audio/EATrax0` + `EATrax1` (1.6 GiB); `audio/Movie` (291 MiB); the `DJ*`
banks; the `awd_pveh_*_low` variants and the `ex_*`/`gear_*` waves inside the
`_high` dirs; `audio/rws_crash*` (147 MiB, re-enable with
`B3_PACK_CRASH_AUDIO=1`); EA TRAX tracks 2–43; all `debug_dump_*`.

`route.bin` / `grid.bin` / `traffic.bin` are deliberately absent: they are
extractor outputs already baked into `src/burnout3_track_paths.h`,
`burnout3_start_grid.h` and `burnout3_traffic_data.h` at compile time, and
nothing opens them at runtime.

### Refreshing assets after an extractor rerun

```sh
python3 tools/extract_track.py …      # or any other tools/extract_*.py
./android/pack_assets.sh US_C3_V1     # rebuilds the zip from build/
cd android && ./gradlew assembleDebug
```

`pack_assets.sh` writes `build/ASSET_STAMP` into the zip (track id, timestamp,
and a sha256 over the packed file list + sizes). `MainActivity` compares the
stamp inside the APK with the one already on disk and re-extracts the whole
tree when they differ — so a repack genuinely lands on the device instead of
being masked by a stale extraction. No file is ever hand-copied.

Packing a different track is just `./android/pack_assets.sh EU_C2_V1`; the
script promotes that track's `track.obj` / `collision.bin` / `textures/` into
the top-level names the harness hard-codes, so the payload stays self-consistent
even when the checkout currently has some other track installed. Remember the
matching `B3_TRACK` default lives in `b3_android_boot()`.

---

## 6. Runtime shape on device

1. `MainActivity.onCreate` extracts `assets/burnout3_assets.zip` into
   `getFilesDir()` (zip-slip guarded), then hands over to `SDLActivity`.
2. SDL loads `libSDL2.so` then `libmain.so`; `libSDL2_image.so` follows as a
   `DT_NEEDED`. `SDL_main.h` has already renamed the harness' `main` to
   `SDL_main`, so no entry-point shim was needed.
3. `b3_android_boot()` `chdir`s to that same directory and starts a
   `pipe()`+thread pump that turns the harness' `printf` ledger into
   `logcat -s Burnout3` output.
4. `b3_android_gl_init()` calls `initialize_gl4es()` on the current ES
   context, **then restores the EGL binding with raw EGL**: gl4es' hardware
   probe (`glx/hardext.c GetHardwareExtensions`) makes its own pbuffer
   context current and exits via `eglMakeCurrent(dpy, 0, 0, EGL_NO_CONTEXT)`,
   leaving the thread with no current context while SDL believes its own is
   still bound. Without the rebind every raw GLES call is the Android
   trampoline's silent no-op (`glCreateShader()==0`, `glGetError()==0`), the
   first FPE program "fails" and gl4es null-derefs (`realize_glenv`,
   fpe.c:1138).
5. Everything after that is the unmodified harness.

Orientation is locked to landscape twice: the manifest says
`sensorLandscape`, **and** `b3_android_boot()` sets `SDL_HINT_ORIENTATIONS`
to `"LandscapeLeft LandscapeRight"` — SDL's window glue calls
`setRequestedOrientation()` itself and, for a resizable window with no hint,
requests `FULL_USER`, which follows the sensor straight back into portrait.
MSAA defaults off (tile GPUs pay for the implicit resolves the postfx
framebuffer reads force). `B3_POSTFX_PRESENT` / `B3_POSTFX_BLUR` default off
on Android: the `glCopyTexImage2D` back-buffer grab produced a solid white
frame on the Pixel's Mali; needs a proper FBO-based grab before re-enabling.

### Touch + tilt controls (`android/app/src/main/cpp/b3_touch.c`)

* **Tilt to steer** — the SDL accelerometer joystick, device-Y axis, sign
  corrected per `SDL_GetDisplayOrientation()` so both sensorLandscape
  rotations steer the same way. 0.10 g deadzone, full lock at
  `B3_TILT_LOCK_G` (default 0.42 g ≈ 25°), one-pole low-pass;
  `B3_TILT_SIGN=-1` flips a device that reports oddly.
* **Buttons** — three semi-transparent discs, height-relative geometry, hit
  area 1.35× the drawn disc, multi-touch with slide-off release: BRAKE
  (red, octagon icon) on the left edge, GAS (green, triangle) on the right
  edge, BOOST (orange, double chevron) above GAS.
* `burnout3_full.c` integration (all `#ifdef __ANDROID__`): events feed
  `b3_touch_event()`, the input gather merges `b3_touch_state()` exactly
  like the pad (analog steer wins only when non-zero, buttons OR in), and
  `b3_touch_draw()` runs dead last before the swap so T-dump captures stay
  pure game rendering.

---

## 7. Verified / not verified

**Verified**

* `make -j4` clean build green, `tools/validate_port.py` **127/127**, from the
  same tree state that produces the APK.
* `./gradlew assembleDebug` green. `app-debug.apk` = 150,497,497 bytes.
  `aapt2 dump badging`: `com.b3re.harness`, minSdk 24, targetSdk 35,
  `uses-gl-es 0x20000`, launchable activity `com.b3re.harness.MainActivity`.
  Payload: `lib/arm64-v8a/{libmain.so 1.8 MB, libSDL2.so 1.5 MB,
  libSDL2_image.so 86 KB}` + `assets/burnout3_assets.zip 148.9 MB` (stored).
* Link-level GL correctness (no `libGLESv2` `DT_NEEDED`, no undefined `gl*`).

**Not verified** — no device or emulator was attached, and per the brief none
was started. Everything below is untested on hardware:

* first-run extraction time and free-space behaviour (~239 MiB written);
* gl4es' translation of the four GLSL programs (postfx gamma LUT, postfx blur
  accumulate/present composite, trackmesh fog floor, carfx shine) — the first
  thing to check in `logcat -s Burnout3`, which will print the compiler log on
  failure and fall back to fixed function;
* display-list and `GL_QUADS` throughput with this vertex volume on a tile GPU;
* the touch zone layout's ergonomics.

## 8. Remaining work

1. **Run it on hardware** and triage the logcat. That is the whole next step.
2. **Release build**: `assembleRelease` currently has no signing config —
   add a keystore and `signingConfigs`. Also consider `abiFilters` +
   `armeabi-v7a` and an app bundle, at which point the 143 MiB asset zip must
   move to Play Asset Delivery (150 MB APK is fine to sideload, not to publish).
3. **Asset diet**: `build/cars/` is 133 MiB of the 143 MiB payload. Restricting
   it to the 8 roster slots plus the `.bgd` traffic set would likely drop the
   APK under 60 MiB; it needs the roster/traffic tables read out of
   `burnout3_vehicle_data.h` + `burnout3_traffic_data.h` by the packer.
4. **On-device asset sideload** so the full `build/` tree (or another track) can
   be pushed with `adb push` without a rebuild — the extractor already skips
   when the stamp matches, so a manual stamp write is all it takes.
5. **Touch UI**: the zone map is invisible. Draw it, or add tilt steering.
6. **Audio latency**: SDL's OpenSLES backend with 1024-frame buffers at 44.1 kHz
   mono; check for underruns and consider bumping to AAudio (SDL hint
   `SDL_HINT_AUDIODRIVER=aaudio`).
7. **Pause/resume**: SDL destroys the GL context on background; gl4es state and
   every display list / texture the harness uploaded at init would need
   rebuilding. Currently untested — expect a black screen after a task switch
   until `SDL_HINT_ANDROID_BLOCK_ON_PAUSE` behaviour is confirmed.
8. **Keep the two source lists in lockstep**: a module added to the Makefile's
   `SRCS` must also be added to `android/app/src/main/cpp/CMakeLists.txt`.
