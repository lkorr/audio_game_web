#pragma once
// Shared surface between the WASM C++ game and the JS glue.
//
// Everything here is called across the WASM boundary:
//   - the audio worklet calls wasm_render() on the audio thread
//   - main.js calls the input_* and lifecycle functions on the main thread
//   - the C++ side calls the JS-provided decode hooks (declared in asset_bridge)
//
// All functions are extern "C" so Emscripten exports them by unmangled name.

#include <cstdint>

extern "C" {

// ---- Audio (called on the audio-worklet thread) ----------------------------
// Fill `out` with `frames` interleaved stereo float samples (L,R,L,R,...).
// Safe to call before the world exists: writes silence and returns.
// Invoked internally by the worklet render callback (WebAudioDevice.cpp).
void wasm_render(float* out, int frames);

// Audio bring-up, called from main.js inside the user gesture, in order:
//   wasm_create_audio() -> AudioContext created in C; returns its sample rate
//                          (feed to wasm_init), or 0 on failure.
//   wasm_start_audio()  -> boots the worklet thread + node (async).
//   wasm_resume_audio() -> resumes the context so sound flows.
double wasm_create_audio();
void   wasm_start_audio();
void   wasm_resume_audio();

// ---- Lifecycle (called from main.js) ---------------------------------------
// Boot the game: synthesize/scan sounds, load the scene, prepare voices.
// `sampleRate` is the real AudioContext rate (often 44100, sometimes 48000).
// Must be called once, after assets are preloaded into MEMFS, before ticking.
void wasm_init(double sampleRate);

// Advance one frame. `dtSeconds` is wall-clock delta (clamped internally).
// Runs the full game step and renders the WebGL frame.
void wasm_tick(double dtSeconds);

// Current audio CPU load (0..1+) for an on-page readout. -1 if not started.
double wasm_cpu_load();

// ---- Input (called from main.js) -------------------------------------------
// Continuous movement keys are edge-tracked here (down=1/up=0); wasm_tick
// samples the latched state, matching SDL_GetKeyboardState in the native loop.
// `sdlKeycode` is the SDL3 SDLK_* value (main.js maps DOM codes -> these).
void input_key(uint32_t sdlKeycode, int down, int repeat);
void input_mouse(float dx, float dy);          // relative motion, pixels
void input_wheel(float dy);                    // wheel notches
void input_click(int button);                  // 0=left,1=middle,2=right
void input_mouse_button(int button, int down); // held state (for drag-paint)
void input_pointerlock(int locked);            // pointer-lock state changed
void input_ping();                             // echo probe (E / left-click)

// Top-level game keys that main.cpp handled inline (Tab/F1/F2/F3 + ESC).
// Kept separate from DevMode so the dispatch order matches native exactly.
void input_toggle_dev();       // F2
void input_toggle_stats();     // F1
void input_toggle_visual();    // F3
void input_toggle_fullbright();// Tab

// ---- Canvas resize (called from main.js on ResizeObserver) -----------------
void wasm_resize(int widthPx, int heightPx);

} // extern "C"
