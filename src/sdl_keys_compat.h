#pragma once
// SDL3 keycode compatibility shim for the web build.
//
// DevMode.cpp is compiled UNCHANGED into the WASM module, but it includes
// <SDL3/SDL.h> purely for the SDLK_* keycode enum it switch()es on -- it makes
// no other SDL calls. Rather than link SDL into the browser build, we force
// this tiny header in front of DevMode.cpp (via `-include sdl_keys_compat.h`
// in CMakeLists) and stub out the SDL include, so the SDLK_* constants resolve
// to their real SDL3 values with no SDL dependency.
//
// Values are from SDL3's SDL_keycode.h and are ABI-stable public constants:
//   - printable keys == their ASCII/UTF-8 codepoint (letters are LOWERCASE)
//   - named keys == SDLK_SCANCODE_MASK | <scancode>
// The input bridge (input_bridge.cpp) maps DOM KeyboardEvent.code strings to
// exactly these values before calling DevMode::handleKey, so the switch cases
// match what the browser sends.

#include <cstdint>

// Neutralize the real SDL header if something includes it after us.
#define SDL_h_
#define SDL_keycode_h_

using SDL_Keycode = uint32_t;

// Scancode mask for non-printable keys (SDL3: 1<<30).
#ifndef SDLK_SCANCODE_MASK
#define SDLK_SCANCODE_MASK (1u << 30)
#endif

// --- SDL3 scancodes used by the named keys below (from SDL_scancode.h) ------
#define SDL_SCANCODE_RETURN 40
#define SDL_SCANCODE_ESCAPE 41
#define SDL_SCANCODE_RIGHT  79
#define SDL_SCANCODE_LEFT   80
#define SDL_SCANCODE_DOWN   81
#define SDL_SCANCODE_UP     82

// --- Printable keys: keycode == ASCII (letters lowercase) -------------------
#define SDLK_1 0x31
#define SDLK_2 0x32
#define SDLK_3 0x33
#define SDLK_4 0x34
#define SDLK_B 0x62
#define SDLK_C 0x63
#define SDLK_E 0x65
#define SDLK_F 0x66
#define SDLK_G 0x67
#define SDLK_K 0x6b
#define SDLK_L 0x6c
#define SDLK_N 0x6e
#define SDLK_O 0x6f
#define SDLK_P 0x70
#define SDLK_Q 0x71
#define SDLK_R 0x72
#define SDLK_S 0x73
#define SDLK_T 0x74
#define SDLK_U 0x75
#define SDLK_V 0x76
#define SDLK_X 0x78
#define SDLK_LEFTBRACKET  0x5b   // '['
#define SDLK_RIGHTBRACKET 0x5d   // ']'

// --- Named keys: SDLK_SCANCODE_MASK | scancode ------------------------------
#define SDLK_RETURN (SDL_SCANCODE_RETURN | SDLK_SCANCODE_MASK)
#define SDLK_ESCAPE (SDL_SCANCODE_ESCAPE | SDLK_SCANCODE_MASK)
#define SDLK_RIGHT  (SDL_SCANCODE_RIGHT  | SDLK_SCANCODE_MASK)
#define SDLK_LEFT   (SDL_SCANCODE_LEFT   | SDLK_SCANCODE_MASK)
#define SDLK_DOWN   (SDL_SCANCODE_DOWN   | SDLK_SCANCODE_MASK)
#define SDLK_UP     (SDL_SCANCODE_UP     | SDLK_SCANCODE_MASK)

// --- Mouse state (DevMode drag-paint polls SDL_GetMouseState) ----------------
// SDL3: SDL_GetMouseState returns a button-flags bitmask; SDL_BUTTON_LMASK is
// bit 0 (left). We back this with the live button state the input bridge tracks
// (implemented in input_bridge.cpp). The float* out params (x,y) are unused by
// DevMode -- it passes nullptr -- so they're ignored.
using SDL_MouseButtonFlags = uint32_t;
#define SDL_BUTTON_LMASK 0x1u
#define SDL_BUTTON_MMASK 0x2u
#define SDL_BUTTON_RMASK 0x4u

// Defined in input_bridge.cpp.
SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y);
