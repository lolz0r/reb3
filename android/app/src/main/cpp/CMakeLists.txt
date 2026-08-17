# ---------------------------------------------------------------------------
# Burnout 3: Takedown RE harness -- Android (arm64) native build.
#
# This file is the Android counterpart of the repo-root Makefile.  It compiles
# THE SAME src/*.c translation units the desktop build compiles (SRCS below is
# kept in lockstep with the Makefile's SRCS), plus three third-party libraries
# that stand in for the desktop's system SDL2/SDL2_image/libGL:
#
#   SDL2       (libsdl-org/SDL, release-2.32.10) -- window/GL/audio/input
#   SDL2_image (libsdl-org/SDL_image, release-2.8.8, stb backend) -- IMG_Load
#   gl4es      (ptitSeb/gl4es) -- desktop GL 2.1 COMPATIBILITY on top of GLES2
#              (immediate mode, display lists, GL_QUADS, glPushAttrib, and the
#              GLSL 1.20 -> ESSL 1.00 shader translation the postfx passes need)
#
# Nothing here is referenced by the desktop build; `make` at the repo root is
# unaffected.  All Android-only divergence inside src/*.c is #ifdef __ANDROID__.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.22)
project(burnout3_android C)

# Android 15+ 16 KB page-size compatibility: must be set BEFORE the SDL
# subprojects are configured so every shared lib links with it.
set(CMAKE_SHARED_LINKER_FLAGS
    "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")

if(NOT DEFINED B3_ROOT)
    get_filename_component(B3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
endif()
set(B3_SRC "${B3_ROOT}/src")
set(B3_TP  "${B3_ROOT}/android/third_party")

foreach(dep SDL2 SDL2_image gl4es)
    if(NOT EXISTS "${B3_TP}/${dep}/CMakeLists.txt")
        message(FATAL_ERROR
            "Missing ${B3_TP}/${dep}. Run android/fetch_deps.sh first.")
    endif()
endforeach()

# --------------------------------------------------------------------------
# 1. SDL2
# --------------------------------------------------------------------------
set(SDL_SHARED ON  CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
add_subdirectory("${B3_TP}/SDL2" sdl2-build)

# --------------------------------------------------------------------------
# 2. SDL2_image -- PNG only, via the vendored stb_image backend (no libpng,
#    no submodules).  The harness calls IMG_Init(IMG_INIT_PNG) and IMG_Load
#    on .png art exclusively.
# --------------------------------------------------------------------------
set(SDL2IMAGE_INSTALL     OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_SAMPLES     OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_TESTS       OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_VENDORED    OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_BACKEND_STB ON  CACHE BOOL "" FORCE)
set(SDL2IMAGE_PNG         ON  CACHE BOOL "" FORCE)
set(SDL2IMAGE_PNG_SAVE    OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_JPG         OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_JPG_SAVE    OFF CACHE BOOL "" FORCE)
foreach(fmt AVIF BMP GIF JXL LBM PCX PNM QOI SVG TGA TIF WEBP XCF XPM XV)
    set(SDL2IMAGE_${fmt} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${B3_TP}/SDL2_image" sdl2image-build)

# --------------------------------------------------------------------------
# 3. gl4es -- static, no init constructor (we call initialize_gl4es() once the
#    SDL GLES2 context is current), Android logging.  ANDROID is already set
#    by the NDK toolchain, which turns on -DNOX11 -DNO_GBM -DDEFAULT_ES=2
#    inside gl4es' own CMakeLists.
# --------------------------------------------------------------------------
set(STATICLIB           ON CACHE BOOL "" FORCE)
set(NO_INIT_CONSTRUCTOR ON CACHE BOOL "" FORCE)
set(USE_ANDROID_LOG     ON CACHE BOOL "" FORCE)
set(NOX11               ON CACHE BOOL "" FORCE)
add_subdirectory("${B3_TP}/gl4es" gl4es-build)
target_compile_options(GL PRIVATE -Wno-everything)
# gl4es hard-codes ARCHIVE_OUTPUT_DIRECTORY to ${CMAKE_SOURCE_DIR}/lib, which
# is *this* directory; keep libGL.a in the build tree instead.
set_target_properties(GL PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gl4es-lib")

# --------------------------------------------------------------------------
# 4. Compatibility include shim.
#    The harness sources say `#include <SDL2/SDL.h>` / `<SDL2/SDL_image.h>`
#    (the Debian/pkg-config layout).  Upstream SDL2 installs those headers
#    flat, so generate a directory of one-line forwarding headers rather than
#    touching 8 translation units.  <GL/gl.h> resolves to gl4es' own header.
# --------------------------------------------------------------------------
set(B3_COMPAT_INC "${CMAKE_CURRENT_BINARY_DIR}/compat_include")
file(MAKE_DIRECTORY "${B3_COMPAT_INC}/SDL2")
file(GLOB B3_SDL_HDRS "${B3_TP}/SDL2/include/*.h")
list(APPEND B3_SDL_HDRS "${B3_TP}/SDL2_image/include/SDL_image.h")
foreach(hdr ${B3_SDL_HDRS})
    get_filename_component(hname "${hdr}" NAME)
    file(WRITE "${B3_COMPAT_INC}/SDL2/${hname}" "#include <${hname}>\n")
endforeach()

# --------------------------------------------------------------------------
# 5. The harness itself.  SRCS mirrors the Makefile's SRCS exactly; the extra
#    file is the Android-only bootstrap (asset chdir + gl4es bring-up).
#    HOST-ONLY code (tools/, the validators) is deliberately absent.
# --------------------------------------------------------------------------
set(B3_SRCS
    ${B3_SRC}/burnout3_full.c
    ${B3_SRC}/burnout3_vehicle_sim.c
    ${B3_SRC}/burnout3_trackmesh.c
    ${B3_SRC}/burnout3_hud.c
    ${B3_SRC}/burnout3_collision.c
    ${B3_SRC}/burnout3_crash.c
    ${B3_SRC}/burnout3_carcol.c
    ${B3_SRC}/burnout3_takedown.c
    ${B3_SRC}/burnout3_score_events.c
    ${B3_SRC}/burnout3_td_rules.c
    ${B3_SRC}/burnout3_sfx.c
    ${B3_SRC}/burnout3_ai.c
    ${B3_SRC}/burnout3_carfx.c
    ${B3_SRC}/burnout3_postfx.c
    ${B3_SRC}/burnout3_music.c
    ${B3_SRC}/burnout3_boostfx.c
    ${B3_SRC}/burnout3_particlefx.c
    ${B3_SRC}/burnout3_panels.c
    ${B3_SRC}/burnout3_props.c
    ${B3_SRC}/burnout3_traffic_reservations.c
    ${B3_SRC}/burnout3_traffic_pool.c
)

add_library(main SHARED
    ${B3_SRCS}
    ${CMAKE_CURRENT_SOURCE_DIR}/b3_android.c
    ${CMAKE_CURRENT_SOURCE_DIR}/b3_touch.c
)

target_include_directories(main PRIVATE
    ${B3_COMPAT_INC}            # <SDL2/...> forwarders  (must precede SDL's)
    ${B3_TP}/gl4es/include      # <GL/gl.h> -> gl4es
    ${B3_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(main PRIVATE _GNU_SOURCE)
target_compile_options(main PRIVATE
    -std=c11 -O2 -Wall -Wextra
    -Wno-unused-parameter -Wno-missing-field-initializers
)

target_link_libraries(main
    GL                # gl4es first: our gl* calls must bind to it, not GLESv2
    SDL2_image
    SDL2
    android log dl m
)
