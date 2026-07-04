#pragma once
// Stub <SDL3/SDL.h> for the web build.
//
// DevMode.cpp does `#include <SDL3/SDL.h>` but only uses the SDLK_* keycode
// enum from it (verified: no other SDL symbols). SDL is NOT linked into the
// WASM build, so we put this directory on DevMode's include path (via
// CMakeLists) so that include resolves here instead of to a missing real SDL,
// and forward to the keycode-only shim.
#include "sdl_keys_compat.h"
