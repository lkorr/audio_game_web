cmake_minimum_required(VERSION 3.25)
project(audio_game LANGUAGES C CXX)

# This repo is fully self-contained (engine/, game/, cmake/ vendored in) and
# builds TWO ways from this one file:
#
#   Native (desktop, live-tweak):
#     cmake -B build -DCMAKE_BUILD_TYPE=Release
#     cmake --build build --config Release --target audio_game
#     ./build/game/Release/audio_game --slice      # edit tunables.txt live
#
#   Web (WebAssembly, via Emscripten):
#     ./build.sh                                    # emcmake wrapper
#
# The two share the engine + portable game/src sources and swap only the
# platform glue (native game/src/{main,AudioDevice,GLLoader}.cpp vs web src/*).
# When configured WITHOUT Emscripten we build the native exe; WITH emcmake we
# build the WASM module.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The vendored shared sources live under this repo. REPO_ROOT can still point at
# an external checkout (-DREPO_ROOT=<path>) but defaults to here.
if(NOT DEFINED REPO_ROOT)
    set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
endif()
get_filename_component(REPO_ROOT ${REPO_ROOT} ABSOLUTE)
set(GAME_SRC  ${REPO_ROOT}/game/src)

# ============================ NATIVE BUILD ==================================
# Desktop build (SDL3 window/audio). This is the build you live-tweak with:
# run `audio_game --slice`, then edit tunables.txt on your other monitor.
if(NOT EMSCRIPTEN)
    message(STATUS "audio_game: NATIVE build")
    include(${REPO_ROOT}/cmake/CPM.cmake)

    CPMAddPackage(
        NAME SDL3
        GITHUB_REPOSITORY libsdl-org/SDL
        GIT_TAG release-3.2.14
        OPTIONS
            "SDL_SHARED OFF"
            "SDL_STATIC ON"
            "SDL_TEST_LIBRARY OFF"
    )
    CPMAddPackage(
        NAME glm
        GITHUB_REPOSITORY g-truc/glm
        GIT_TAG 1.0.1
    )
    CPMAddPackage(
        NAME miniaudio
        GITHUB_REPOSITORY mackron/miniaudio
        GIT_TAG 0.11.21
        DOWNLOAD_ONLY YES
    )

    add_subdirectory(engine)
    add_subdirectory(game)
    return()   # native target is defined in game/CMakeLists.txt; we're done
endif()

# ============================= WEB BUILD ====================================
message(STATUS "audio_game: WEB (Emscripten) build: REPO_ROOT = ${REPO_ROOT}")

# --- Dependencies (header-only; fetched like the native root CMake) ----------
include(${REPO_ROOT}/cmake/CPM.cmake)

CPMAddPackage(
    NAME glm
    GITHUB_REPOSITORY g-truc/glm
    GIT_TAG 1.0.1
)
CPMAddPackage(
    NAME miniaudio
    GITHUB_REPOSITORY mackron/miniaudio
    GIT_TAG 0.11.21
    DOWNLOAD_ONLY YES
)

# --- Threading features (Wasm Audio Worklet requirement) ---------------------
# AUDIO_WORKLET runs the render callback on a real audio thread and is built on
# Wasm SharedArrayBuffer, so EVERY object -- including the engine subdirectory
# -- must be compiled with atomics + bulk-memory. Applying these globally (via
# add_compile_options, before add_subdirectory) guarantees the engine .a and the
# executable agree; a mismatch is the "--shared-memory is disallowed" link error.
# NOTE: this build therefore requires cross-origin isolation (COOP/COEP) to run
# in a browser -- see README ("Serving with the required headers").
#
# AUDIO_WORKLET requires WASM_WORKERS (Emscripten enforces this). WASM_WORKERS
# uses a lightweight threading runtime whose libc++ variant (-ww) has no
# pthread. C++ thread-safe static init (function-local statics -- e.g. in the
# engine's Engine.cpp) would otherwise emit calls to pthread_mutex_* / call_once
# and fail to link, so we disable thread-safe-statics. Safe here: those statics
# initialize from constants, and the main-thread warm-up render (wasm_init)
# initializes them before the worklet thread ever runs.
add_compile_options(-sWASM_WORKERS=1 -fno-threadsafe-statics)

# --- Engine (portable DSP, compiled unchanged; Emscripten flags handled in
#     engine/CMakeLists.txt via the EMSCRIPTEN branch) --------------------------
add_subdirectory(${REPO_ROOT}/engine ${CMAKE_BINARY_DIR}/engine_build)

# --- The web executable ------------------------------------------------------
add_executable(audiogame
    # New web glue
    src/web_main.cpp
    src/WebAudioDevice.cpp
    src/WebGLLoader.cpp
    src/input_bridge.cpp
    src/asset_bridge.cpp        # miniaudio decoder implementation (decode-only)
    src/pthread_stubs.c         # no-op pthread syms libc++-ww references (see file)
    # Portable game sources (identical to native, minus main/AudioDevice/GLLoader)
    ${GAME_SRC}/AudioWorld.cpp
    ${GAME_SRC}/DevMode.cpp
    ${GAME_SRC}/EngineTuning.cpp
    ${GAME_SRC}/Occlusion.cpp
    ${GAME_SRC}/SceneIO.cpp
    ${GAME_SRC}/SoundDirector.cpp
    ${GAME_SRC}/SoundLibrary.cpp
    ${GAME_SRC}/Source.cpp
    ${GAME_SRC}/Sounds.cpp
    ${GAME_SRC}/Player.cpp
    ${GAME_SRC}/Scene.cpp
    ${GAME_SRC}/Stalker.cpp
    ${GAME_SRC}/SliceGame.cpp
    ${GAME_SRC}/Tunables.cpp
    ${GAME_SRC}/Renderer.cpp
)

target_link_libraries(audiogame PRIVATE xyzpan_engine glm::glm)

target_include_directories(audiogame PRIVATE
    ${GAME_SRC}                 # game headers (AudioWorld.h, Scene.h, ...)
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${miniaudio_SOURCE_DIR}
)

# DevMode.cpp includes <SDL3/SDL.h> purely for the SDLK_* enum. SDL is not
# linked into WASM, so we resolve that include to a stub (src/sdl_stub/SDL3/
# SDL.h -> sdl_keys_compat.h) by putting the stub dir on the include path ahead
# of any real SDL. Applied to the whole target (harmless: only DevMode.cpp
# includes SDL, and the stub only defines SDLK_* macros).
target_include_directories(audiogame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/sdl_stub)

target_compile_options(audiogame PRIVATE -O3 -msimd128)

# --- Link / Emscripten settings ----------------------------------------------
# AUDIO_WORKLET audio (real audio thread via -pthread/SharedArrayBuffer),
# WebGL2/GLES3, MEMFS assets preloaded, exported entry points for main.js.
# Requires COOP/COEP headers at serve time (see README).
set(EXPORTED_FUNCS "[\
_main,\
_wasm_init,_wasm_tick,_wasm_resize,\
_wasm_create_audio,_wasm_start_audio,_wasm_resume_audio,_wasm_cpu_load,\
_input_key,_input_mouse,_input_wheel,_input_click,_input_mouse_button,_input_pointerlock,\
_input_ping,\
_input_toggle_dev,_input_toggle_stats,_input_toggle_visual,_input_toggle_fullbright,\
_malloc,_free]")

target_link_options(audiogame PRIVATE
    "-O3"
    "-sENVIRONMENT=web,worker"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createAudioGame"
    "-sEXPORTED_FUNCTIONS=${EXPORTED_FUNCS}"
    "-sEXPORTED_RUNTIME_METHODS=[ccall,cwrap,HEAPF32]"
    # NO memory growth: with -pthread the heap is a SharedArrayBuffer, and a
    # GROWABLE SharedArrayBuffer produces resizable-ArrayBuffer views that
    # WebGL's bufferData/texImage REJECT ("provided ArrayBuffer value must not
    # be resizable"). A fixed heap keeps the GL upload views valid. 768 MB is
    # sized for the ~16-voice scene (each XYZPanEngine ~33 MB) with headroom.
    "-sINITIAL_MEMORY=805306368"      # 768 MB fixed, non-growable
    "-sMAX_WEBGL_VERSION=2"
    "-sMIN_WEBGL_VERSION=2"
    "-sFULL_ES3=1"
    "-sAUDIO_WORKLET=1"
    "-sWASM_WORKERS=1"
    "-sSTACK_SIZE=1048576"
    "--preload-file" "${CMAKE_CURRENT_SOURCE_DIR}/assets@/assets"
)

# Emit into dist/ with a stable base name so index.html can reference it.
set_target_properties(audiogame PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/dist
    OUTPUT_NAME audiogame)
