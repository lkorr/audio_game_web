// Exported input_* entry points main.js calls, plus the WebInput drain helper.
// See input_bridge.h for the split-responsibility rationale.

#include "input_bridge.h"
#include "web_bridge.h"
#include "sdl_keys_compat.h"   // SDLK_* values + SDL_GetMouseState decl

namespace {
WebInput g_input;

// Map an SDL3 keycode to a latched movement flag; returns a pointer to the
// bool in g_input, or nullptr if this key is not a movement key. Movement uses
// the same physical keys as native (WASD + shift + space/ctrl); main.js sends
// the letter keycodes (lowercase ASCII) and the modifier codes below.
bool* movementFlagFor(uint32_t kc) {
    switch (kc) {
        case 0x77 /*w*/: return &g_input.forward;
        case 0x73 /*s*/: return &g_input.back;
        case 0x61 /*a*/: return &g_input.left;
        case 0x64 /*d*/: return &g_input.right;
        // Left/Right Shift, Space, Left Ctrl -- SDL3 scancode-masked keycodes.
        case (225u | SDLK_SCANCODE_MASK): // LSHIFT
        case (229u | SDLK_SCANCODE_MASK): // RSHIFT
            return &g_input.sprint;
        case 0x20 /*space*/: return &g_input.up;
        case (224u | SDLK_SCANCODE_MASK): // LCTRL
            return &g_input.down;
        default: return nullptr;
    }
}
} // namespace

WebInput& webInput() { return g_input; }

// SDL mouse-state shim for DevMode's drag-paint poll (declared in the stub
// header). DevMode passes nullptr for x/y and only tests SDL_BUTTON_LMASK.
SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    return g_input.mouseButtons;
}

PlayerInput WebInput::sampleMovement() {
    PlayerInput in;
    in.forward = forward; in.back = back; in.left = left; in.right = right;
    in.sprint = sprint;   in.up = up;     in.down = down;
    if (pointerLocked) { in.mouseDX = mouseDX; in.mouseDY = mouseDY; }
    mouseDX = 0.0f; mouseDY = 0.0f;   // consumed
    return in;
}

extern "C" {

void input_key(uint32_t sdlKeycode, int down, int repeat) {
    // Latch movement keys (both down and up matter).
    if (bool* flag = movementFlagFor(sdlKeycode)) {
        *flag = (down != 0);
        // Movement keys are not queued as discrete events -- native read them
        // via the keyboard-state snapshot, not the event stream.
        return;
    }
    // Only key-DOWN produces a discrete UI event (matches native, which acted
    // on SDL_EVENT_KEY_DOWN). Repeats are forwarded so held arrows keep
    // adjusting menu rows (native passed ev.key.repeat through to DevMode).
    if (down)
        g_input.events.push_back({ WebEvent::Key, sdlKeycode, repeat, 0.0f, 0 });
}

void input_mouse(float dx, float dy) {
    g_input.mouseDX += dx;
    g_input.mouseDY += dy;
}

void input_wheel(float dy) {
    g_input.events.push_back({ WebEvent::Wheel, 0, 0, dy, 0 });
}

void input_click(int button) {
    g_input.events.push_back({ WebEvent::Click, 0, 0, 0.0f, button });
}

void input_mouse_button(int button, int down) {
    // Track held state for the DevMode drag-paint poll (SDL_GetMouseState stub).
    const unsigned bit = 1u << (button & 3);
    if (down) g_input.mouseButtons |= bit;
    else      g_input.mouseButtons &= ~bit;
    // A left-button press is also the discrete "click" dev action (native fired
    // its click on SDL_EVENT_MOUSE_BUTTON_DOWN).
    if (down && button == 0)
        g_input.events.push_back({ WebEvent::Click, 0, 0, 0.0f, 0 });
}

void input_pointerlock(int locked) {
    g_input.pointerLocked = (locked != 0);
}

void input_ping()              { g_input.events.push_back({ WebEvent::Ping,             0,0,0,0 }); }
void input_toggle_dev()        { g_input.events.push_back({ WebEvent::ToggleDev,        0,0,0,0 }); }
void input_toggle_stats()      { g_input.events.push_back({ WebEvent::ToggleStats,      0,0,0,0 }); }
void input_toggle_visual()     { g_input.events.push_back({ WebEvent::ToggleVisual,     0,0,0,0 }); }
void input_toggle_fullbright() { g_input.events.push_back({ WebEvent::ToggleFullbright, 0,0,0,0 }); }

} // extern "C"
