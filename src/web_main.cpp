// Web entry point. Mirrors runGame() from the native main.cpp, but:
//   - no SDL: the WebGL2 context comes from Emscripten's HTML5 API, bound to
//     the <canvas id="canvas"> in index.html
//   - no while-loop / vsync: main.js drives frames by calling wasm_tick(dt)
//     from requestAnimationFrame
//   - no miniaudio device: WebAudioDevice publishes the world to the worklet
//   - input arrives through input_bridge (fed by DOM events in main.js)
//
// The per-frame sequence inside wasm_tick is a line-for-line copy of the native
// loop body (player.update -> updateBugs -> listener -> footsteps/impacts ->
// director.update -> setOccluders -> updateParams -> flash decay -> dev pick ->
// renderer.draw -> overlay). Assets are at fixed MEMFS paths (preloaded).

#include "AudioWorld.h"
#include "DevMode.h"
#include "GLLoader.h"
#include "Occlusion.h"
#include "Player.h"
#include "Renderer.h"
#include "Scene.h"
#include "SceneIO.h"
#include "SoundDirector.h"
#include "SoundLibrary.h"
#include "Sounds.h"
#include "Stalker.h"
#include "SliceGame.h"

#include "WebAudioDevice.h"
#include "input_bridge.h"
#include "web_bridge.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// Fixed MEMFS paths (Emscripten --preload-file assets@/assets). The native
// directory-walk (findAudioDir/scenePathFor) is unnecessary in the virtual FS.
namespace {
const char* kAudioDir  = "assets/audio";
const char* kScenePath = "assets/scene.txt";

// All game state lives here so wasm_tick (called from JS each frame) can reach
// it. Held in a single struct constructed by wasm_init; the raw pointer is
// leaked intentionally (the page owns it for its whole lifetime).
struct Game {
    WebAudioDevice device;
    SoundLibrary   lib;
    Scene          scene;
    Player         player;
    AudioWorld     world;
    SoundDirector  director;
    Renderer       renderer;
    DevMode        dev;
    SliceGame      slice;
    ListenerPose   listener;

    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl = 0;

    // View state that native tracked as locals in runGame.
    bool  fullBright = false;
    bool  blindMode  = false;   // F3: near-total darkness (audio-first look)
    bool  showStats  = false;
    bool  pingRequested = false;
    float groundGlowBoost = 0.0f;
    float elapsed = 0.0f;

    int fbw = 1280, fbh = 720;

    // Reused per-frame scratch (avoid per-frame allocation like native).
    std::vector<FootstepEvent> steps;
    std::vector<BumpEvent>     bumps;
    std::vector<OverlayRect>   overlayRects;
    std::vector<OverlayText>   overlayTexts;
};

Game* g = nullptr;

// Dispatch the queued discrete UI events in order, exactly as the native
// SDL_PollEvent switch did (dev gets first refusal on keys; top-level toggles
// otherwise). Movement keys were already latched in input_bridge.
void drainEvents() {
    WebInput& in = webInput();
    for (const WebEvent& ev : in.events) {
        switch (ev.type) {
            case WebEvent::Key:
                // Dev mode consumes first (menus, ESC-closes-menu, arrow repeats).
                if (g->dev.enabled &&
                    g->dev.handleKey(static_cast<int>(ev.keycode), ev.repeat != 0,
                                     g->scene, g->player, g->listener, g->renderer))
                    break;
                // Top-level keys not handled by dev are delivered as explicit
                // Toggle* events from main.js (F1/F2/F3/Tab), so nothing else
                // to do here for a plain key-down.
                break;
            case WebEvent::Wheel:
                g->dev.handleWheel(ev.wheelDy, g->scene, g->listener, g->renderer);
                break;
            case WebEvent::Click:
                if (ev.button == 0) {   // left
                    if (g->dev.enabled) g->dev.handleClick(g->scene, g->player, g->renderer);
                    else g->pingRequested = true;   // echo probe in play
                }
                break;
            case WebEvent::Ping:
                if (!g->dev.enabled) g->pingRequested = true;
                break;
            case WebEvent::ToggleDev:        g->dev.toggle(g->player); break;
            case WebEvent::ToggleStats:      g->showStats  = !g->showStats;  break;
            case WebEvent::ToggleVisual:     g->blindMode  = !g->blindMode;  break;
            case WebEvent::ToggleFullbright: g->fullBright = !g->fullBright; break;
        }
    }
    in.events.clear();
}

} // namespace

extern "C" {

void wasm_init(double sampleRate) {
    if (g != nullptr) return;   // idempotent
    g = new Game();

    // --- WebGL2 context on the page canvas ---
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.majorVersion = 2;      // WebGL2 == GLES3
    attr.minorVersion = 0;
    attr.alpha = false;
    attr.depth = false;         // renderer uses additive lines, no depth buffer
    attr.stencil = false;
    attr.antialias = true;
    g->gl = emscripten_webgl_create_context("#canvas", &attr);
    if (g->gl <= 0) {
        std::fprintf(stderr, "[web] WebGL2 context creation failed (need a WebGL2-capable browser)\n");
    }
    emscripten_webgl_make_context_current(g->gl);
    if (!loadGLFunctions())
        std::fprintf(stderr, "[web] some GL entry points missing\n");
    std::printf("[web] GL_VERSION: %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    // --- Audio: browser owns the rate; record it, build everything at it. ---
    g->device.init(static_cast<unsigned>(sampleRate));
    const double sr = g->device.sampleRate();
    std::printf("[web] audio rate: %.0f Hz\n", sr);

    std::printf("[web] synthesizing + scanning sounds...\n");
    g->lib.init(sr, kAudioDir);

    // Scene: preloaded scene.txt, else the built-in playable slice.
    EngineTuning loadedTuning;
    if (!loadScene(g->scene, g->lib, loadedTuning, kScenePath)) {
        g->scene = buildSliceScene(g->lib);
        std::printf("[web] no %s -- using built-in playable slice\n", kScenePath);
    }

    // Live tunables, baked in: if assets/tunables.txt was bundled at build time,
    // apply it once here (over the scene's engine params). No live reload on web
    // (no re-readable filesystem); dial values in on native, then rebuild for web.
    if (tune::load("assets/tunables.txt"))
        tune::applyEngine("assets/tunables.txt", loadedTuning);

    g->player.yaw = 0.0f;
    g->player.pitch = 0.0f;
    g->player.pos = { 0.0f, -30.0f };   // start in the south riverside sanctuary
    g->listener = { g->player.eyePos(), g->player.yaw, g->player.pitch };

    g->world.tuning() = loadedTuning;   // before init(): prepare() snapshots params
    g->world.init(sr, g->listener);

    g->director.init(&g->world, &g->lib);

    // Warm-up render on this (main) thread: initializes the engine's
    // function-local statics before the worklet's first callback, keeping that
    // callback lock-free (same rationale as native).
    {
        std::vector<float> warmup(static_cast<size_t>(AudioWorld::kMaxBlock) * 2);
        g->world.render(warmup.data(), AudioWorld::kMaxBlock);
    }

    // Publish the world to the worklet-facing global. The worklet node is only
    // connected by main.js AFTER wasm_init returns, so callbacks start clean.
    g->device.start(&g->world);

    if (!g->renderer.init(g->scene))
        std::fprintf(stderr, "[web] renderer init failed\n");

    g->dev.init(&g->lib, &g->director, &g->world.tuning(), kScenePath);

    g->slice.init(g->scene, g->player.eyePos(), g->lib);

    std::printf("[web] ready.\n");
}

void wasm_resize(int widthPx, int heightPx) {
    if (g == nullptr) return;
    g->fbw = widthPx > 0 ? widthPx : 1;
    g->fbh = heightPx > 0 ? heightPx : 1;
    if (g->gl > 0) {
        emscripten_webgl_make_context_current(g->gl);
        // The drawing-buffer size is set by main.js on the canvas; nothing else
        // needed here (renderer.draw takes fbw/fbh and sets the viewport).
    }
}

void wasm_tick(double dtSeconds) {
    if (g == nullptr) return;
    emscripten_webgl_make_context_current(g->gl);

    const float dt = std::min(0.1f, static_cast<float>(dtSeconds));
    g->elapsed += dt;

    // ---- input ----
    drainEvents();
    PlayerInput input = webInput().sampleMovement();

    // ---- game step (identical order to native runGame loop body) ----
    g->steps.clear();
    g->bumps.clear();
    g->player.update(dt, input, g->scene, g->steps, g->bumps);
    updateBugs(g->scene, dt);

    // One occluder list per frame, shared by the audio path and the stalker's
    // hearing + steering (identical to the native loop).
    std::vector<Occluder> occluders = buildOccluders(g->scene);

    g->listener = { g->player.eyePos(), g->player.yaw, g->player.pitch };
    for (const FootstepEvent& s : g->steps) {
        g->director.onFootstep(g->scene, s.material, s.pos, g->listener);
        hearSoundStalkers(g->scene, s.pos,
                          footstepLoudness(g->scene, s.material, input.sprint), occluders);
        g->groundGlowBoost = 1.0f;
    }
    for (const BumpEvent& b : g->bumps) {
        g->director.onImpact(g->scene, b.objectId, b.pos, g->listener);
        hearSoundStalkers(g->scene, b.pos, tune::kImpactLoudness, occluders);
    }
    g->director.tickPing(dt);
    if (g->pingRequested) {
        g->pingRequested = false;
        if (g->director.onPing(g->listener))
            hearSoundStalkers(g->scene, g->listener.pos, kPingLoudness, occluders);
    }
    updateStalkers(g->scene, dt, occluders, &g->director, &g->listener);
    // Slice brain: win/lose/respawn. May teleport the player, so re-derive the
    // listener before publishing it to the audio engine.
    g->slice.update(dt, g->scene, g->player, g->director, g->listener);
    g->listener = { g->player.eyePos(), g->player.yaw, g->player.pitch };
    g->director.update(dt, g->scene, g->listener);
    g->world.setOccluders(std::move(occluders));
    g->world.updateParams(g->listener);

    // ---- flash / glow decay ----
    // Reveal decays slower than the bump flash so one-shot blips fade smoothly;
    // director.update() (above) re-raises it to the steady loop target first,
    // so a playing loop stays pinned and only the tail after a pulse fades.
    const float flashDecay = std::exp(-dt * 7.0f);
    const float revealDecay = std::exp(-dt * 3.0f);
    for (Tree& t : g->scene.trees) { t.flash *= flashDecay; t.reveal *= revealDecay; }
    for (Boulder& b : g->scene.boulders) { b.flash *= flashDecay; b.reveal *= revealDecay; }
    for (Emitter& e : g->scene.emitters) e.reveal *= revealDecay;
    for (Bug& b : g->scene.bugs) b.reveal *= revealDecay;
    for (Stalker& s : g->scene.stalkers) s.reveal *= revealDecay;
    g->groundGlowBoost *= std::exp(-dt * 6.0f);

    // ---- dev inspect pick / drag-paint ----
    g->dev.frameUpdate(g->scene, g->player, g->renderer);

    // ---- render ----
    const VisualMode mode = g->fullBright ? VisualMode::FullBright
                          : g->blindMode  ? VisualMode::Blind
                                          : VisualMode::Lit;
    DevPreview previewData;
    const bool hasPreview = g->dev.preview(g->player, previewData);
    g->renderer.draw(g->scene, g->listener.pos, g->listener.yaw, g->listener.pitch,
                     g->fbw, g->fbh, g->elapsed, g->groundGlowBoost, mode,
                     hasPreview ? &previewData : nullptr);

    g->overlayRects.clear();
    g->overlayTexts.clear();
    // Slice HUD: one centered top line (objective / outcome), hidden in dev mode.
    if (g->slice.active() && !g->dev.enabled && !g->slice.hud().empty()) {
        const float scale = 2.0f;
        const float tw = Renderer::textWidth(g->slice.hud(), scale);
        g->overlayTexts.push_back({ (g->fbw - tw) * 0.5f, 24.0f, scale,
                                    { 0.85f, 0.9f, 1.0f, 1.0f }, g->slice.hud() });
    }
    g->dev.buildOverlay(g->scene, g->fbw, g->fbh, g->overlayRects, g->overlayTexts);
    g->renderer.drawOverlay(g->overlayRects, g->overlayTexts, g->fbw, g->fbh);
}

} // extern "C"

// main() is required by the Emscripten runtime but does nothing: the page
// drives wasm_init/wasm_tick explicitly once the user gesture unlocks audio.
int main() {
    std::printf("[web] audio_game module loaded; awaiting wasm_init.\n");
    return 0;
}
