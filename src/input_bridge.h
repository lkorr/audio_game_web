#pragma once
// Web input state shared between input_bridge.cpp (which JS calls) and
// web_main.cpp (which drains it each tick). Mirrors how the native loop split
// input: SDL_GetKeyboardState gave a latched movement snapshot, while discrete
// key/wheel/click events were dispatched to DevMode in event order.
//
// The browser is single-threaded for input + game loop (both on the main JS
// thread), so no locking is needed here -- input_* callbacks and wasm_tick
// never overlap.

#include "Player.h"        // PlayerInput
#include <cstdint>
#include <vector>

// One discrete UI event, queued by JS callbacks and drained by web_main in the
// same order the native SDL_PollEvent loop would have seen them.
struct WebEvent {
    enum Type { Key, Wheel, Click, Ping,
                ToggleDev, ToggleStats, ToggleVisual, ToggleFullbright } type;
    uint32_t keycode = 0;   // Key: SDL3 SDLK_* value
    int repeat = 0;         // Key: 1 if an auto-repeat
    float wheelDy = 0.0f;   // Wheel
    int button = 0;         // Click: 0=L 1=M 2=R
};

struct WebInput {
    // Latched movement state (WASD/shift/space/ctrl), sampled each tick.
    bool forward = false, back = false, left = false, right = false, sprint = false;
    bool up = false, down = false;

    // Accumulated relative mouse motion since the last tick (reset on drain).
    float mouseDX = 0.0f, mouseDY = 0.0f;

    // Whether the pointer is locked (mouse-look active). Mirrors native's
    // "mouseCaptured": look only applies while locked.
    bool pointerLocked = false;

    // Live mouse-button state (bit 0 = left, 1 = middle, 2 = right). DevMode's
    // drag-paint polls this via the SDL_GetMouseState stub each frame.
    unsigned mouseButtons = 0;

    // Discrete UI events awaiting dispatch.
    std::vector<WebEvent> events;

    // Fill a PlayerInput from the latched state + consume mouse deltas.
    PlayerInput sampleMovement();
};

// The single instance the callbacks feed and web_main drains.
WebInput& webInput();
