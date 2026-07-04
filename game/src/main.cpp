// audio_game -- audio-first walking simulator, first playable.
//
// Modes:
//   (default)        windowed game: SDL3 + GL 3.3 wireframe + live audio
//   --render-test    offline: scripted walk rendered through the full mix
//                    path to render_test.wav, stats printed, no window/device
//   --auto-exit N    windowed game that quits cleanly after N seconds

#include "AudioDevice.h"
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
#include "Tunables.h"

#include <SDL3/SDL.h>
#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// Audio assets live in <project>/assets/audio; the exe usually runs from the
// project root or a few levels down inside build/, so walk up a little.
// scene.txt sits next to the assets folder.
std::string findAudioDir() {
    const char* candidates[] = {
        "assets/audio", "../assets/audio", "../../assets/audio", "../../../assets/audio",
    };
    std::error_code ec;
    for (const char* c : candidates)
        if (std::filesystem::is_directory(c, ec)) return c;
    return "assets/audio";
}

std::string scenePathFor(const std::string& audioDir) {
    const std::filesystem::path assets = std::filesystem::path(audioDir).parent_path();
    if (assets.empty()) return "scene.txt";
    return (assets.parent_path() / "scene.txt").string();
}

// ---------------------------------------------------------------------------
// BMP writer (24-bit, for --screenshot; rows arrive bottom-up from
// glReadPixels which matches BMP's native order)
// ---------------------------------------------------------------------------
bool writeBmp24(const char* path, const std::vector<unsigned char>& rgb, int w, int h) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const int rowBytes = (w * 3 + 3) & ~3;
    const uint32_t dataSize = static_cast<uint32_t>(rowBytes * h);
    const uint32_t fileSize = 54 + dataSize;
    unsigned char hdr[54] = { 'B', 'M' };
    auto put32 = [&](int off, uint32_t v) {
        hdr[off] = static_cast<unsigned char>(v);
        hdr[off + 1] = static_cast<unsigned char>(v >> 8);
        hdr[off + 2] = static_cast<unsigned char>(v >> 16);
        hdr[off + 3] = static_cast<unsigned char>(v >> 24);
    };
    put32(2, fileSize); put32(10, 54); put32(14, 40);
    put32(18, static_cast<uint32_t>(w)); put32(22, static_cast<uint32_t>(h));
    hdr[26] = 1; hdr[28] = 24;
    put32(34, dataSize);
    std::fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> row(static_cast<size_t>(rowBytes), 0);
    for (int yRow = 0; yRow < h; ++yRow) {
        const unsigned char* src = rgb.data() + static_cast<size_t>(yRow) * w * 3;
        for (int xCol = 0; xCol < w; ++xCol) {       // RGB -> BGR
            row[static_cast<size_t>(xCol) * 3 + 0] = src[xCol * 3 + 2];
            row[static_cast<size_t>(xCol) * 3 + 1] = src[xCol * 3 + 1];
            row[static_cast<size_t>(xCol) * 3 + 2] = src[xCol * 3 + 0];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// WAV writer (16-bit PCM stereo)
// ---------------------------------------------------------------------------
bool writeWav16(const char* path, const std::vector<float>& interleaved, int sampleRate) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;

    const uint32_t numSamples = static_cast<uint32_t>(interleaved.size());
    const uint32_t dataBytes = numSamples * 2;
    const uint16_t channels = 2, bits = 16;
    const uint32_t byteRate = static_cast<uint32_t>(sampleRate) * channels * (bits / 8);
    const uint16_t blockAlign = channels * (bits / 8);
    const uint32_t riffSize = 36 + dataBytes;

    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f); w32(riffSize); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(channels);
    w32(static_cast<uint32_t>(sampleRate)); w32(byteRate); w16(blockAlign); w16(bits);
    std::fwrite("data", 1, 4, f); w32(dataBytes);

    for (float v : interleaved) {
        const float c = std::clamp(v, -1.0f, 1.0f);
        const int16_t s = static_cast<int16_t>(std::lround(c * 32767.0f));
        std::fwrite(&s, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// --render-test: scripted 10 s listener walk, offline render, verification
// ---------------------------------------------------------------------------
int runRenderTest() {
    constexpr double kSr = 48000.0;
    constexpr double kDurS = 10.0;
    constexpr int kTickHz = 60;
    const int tickFrames = static_cast<int>(kSr) / kTickHz;          // 800
    const int totalFrames = static_cast<int>(kSr * kDurS);

    std::printf("[render-test] synthesizing sounds at %.0f Hz...\n", kSr);
    SoundLibrary lib;
    lib.init(kSr, findAudioDir());

    // Always the hardcoded scene (never scene.txt): the test is a regression
    // baseline.
    Scene scene = buildScene(lib);

    // Walk script: start 15 m south of the creek (y = 8) at (0, -7), walk
    // straight north at 1.5 m/s for 10 s, ending at the water. The path
    // passes LEFT of tree 0 / bird emitter 0 at (1.5, -2): the source swings
    // front-right -> right -> rear-right, so channel asymmetry in the WAV
    // verifies azimuth movement. Head sweeps +-30 degrees (5 s period).
    auto listenerAt = [](double t) {
        ListenerPose lp;
        lp.pos = { 0.0f, static_cast<float>(-7.0 + 1.5 * t), Player::kEyeHeight() };
        lp.yaw = 0.5236f * std::sin(2.0f * 3.14159265f * static_cast<float>(t) / 5.0f);
        lp.pitch = 0.0f;
        return lp;
    };

    AudioWorld world;
    world.init(kSr, listenerAt(0.0));

    SoundDirector director;
    director.init(&world, &lib);

    std::vector<float> out(static_cast<size_t>(totalFrames) * 2, 0.0f);

    double stepAccum = 0.0;
    glm::vec3 prevPos = listenerAt(0.0).pos;
    double cpuMax = 0.0, cpuSum = 0.0;
    int cpuTicks = 0;

    int frame = 0;
    while (frame < totalFrames) {
        const double t = static_cast<double>(frame) / kSr;
        const ListenerPose lp = listenerAt(t);

        // Footstep cadence from horizontal travel, same rule as the game.
        stepAccum += glm::length(glm::vec2(lp.pos) - glm::vec2(prevPos));
        prevPos = lp.pos;
        if (stepAccum >= Player::kStepDistance()) {
            stepAccum -= Player::kStepDistance();
            director.onFootstep(scene, scene.materialAt(lp.pos.x, lp.pos.y),
                                { lp.pos.x, lp.pos.y, 0.0f }, lp);
        }

        updateBugs(scene, 1.0f / kTickHz);
        std::vector<Occluder> occ = buildOccluders(scene);
        updateStalkers(scene, 1.0f / kTickHz, occ);   // no-op for the baseline scene
                                                       // (no stalkers -> no steps)
        director.update(1.0f / kTickHz, scene, lp);
        world.setOccluders(std::move(occ));
        world.updateParams(lp);

        const int n = std::min(tickFrames, totalFrames - frame);
        world.render(out.data() + static_cast<size_t>(frame) * 2, n);

        const double cpu = world.consumeCpuLoad();
        cpuMax = std::max(cpuMax, cpu);
        cpuSum += cpu;
        ++cpuTicks;
        frame += n;
    }

    // ---- Stats ----
    float peak = 0.0f;
    double sumSq = 0.0;
    int nanCount = 0;
    for (float v : out) {
        if (!std::isfinite(v)) { ++nanCount; continue; }
        peak = std::max(peak, std::abs(v));
        sumSq += static_cast<double>(v) * v;
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(out.size()));

    std::printf("[render-test] peak: %.4f  rms: %.4f  NaN/inf: %d  voices: %d\n",
                peak, rms, nanCount, world.activeVoices());
    std::printf("[render-test] audio CPU per block: avg %.2f%%  max %.2f%%\n",
                100.0 * cpuSum / std::max(cpuTicks, 1), 100.0 * cpuMax);

    // Per-2s window L/R RMS: verifies azimuth movement via channel asymmetry.
    // Full band is dominated by the (roughly symmetric) water; the >2.5 kHz
    // band isolates the birdsong chirps from the tree the path passes left of.
    std::printf("[render-test] L/R RMS per 2 s window (bird/tree passes on the right ~t=3-4s):\n");
    {
        const double hpA = std::exp(-2.0 * 3.14159265358979 * 2500.0 / kSr);
        double lpL = 0.0, lpR = 0.0;  // lowpass state; highpass = x - lp
        for (int w = 0; w < 5; ++w) {
            const size_t a = static_cast<size_t>(w) * 2 * static_cast<size_t>(kSr) * 2;
            const size_t b = std::min(out.size(), a + 2 * static_cast<size_t>(kSr) * 2);
            double sl = 0.0, srr = 0.0, hl = 0.0, hr = 0.0;
            for (size_t i = a; i + 1 < b; i += 2) {
                const double l = out[i], r = out[i + 1];
                sl += l * l;
                srr += r * r;
                lpL = (1.0 - hpA) * l + hpA * lpL;
                lpR = (1.0 - hpA) * r + hpA * lpR;
                const double hpl = l - lpL, hpr = r - lpR;
                hl += hpl * hpl;
                hr += hpr * hpr;
            }
            const double cnt = static_cast<double>((b - a) / 2);
            std::printf("  %ds-%ds:  full L %.4f R %.4f  |  >2.5kHz L %.5f R %.5f\n",
                        w * 2, w * 2 + 2, std::sqrt(sl / cnt), std::sqrt(srr / cnt),
                        std::sqrt(hl / cnt), std::sqrt(hr / cnt));
        }
    }

    if (!writeWav16("render_test.wav", out, static_cast<int>(kSr))) {
        std::fprintf(stderr, "[render-test] FAIL: could not write render_test.wav\n");
        return 1;
    }
    std::printf("[render-test] wrote render_test.wav (%.1f s stereo 16-bit)\n", kDurS);

    if (nanCount > 0) { std::fprintf(stderr, "[render-test] FAIL: NaN/inf in output\n"); return 2; }
    if (rms < 1e-4) { std::fprintf(stderr, "[render-test] FAIL: output is silent\n"); return 3; }
    if (peak >= 1.0f) { std::fprintf(stderr, "[render-test] FAIL: peak >= 1.0\n"); return 4; }
    std::printf("[render-test] PASS\n");
    return 0;
}

// ---------------------------------------------------------------------------
// --dump-scene: write the scene (scene.txt if present, else built-in) back to
// scene.txt. Bootstraps a scene file for hand editing and round-trip checks.
// ---------------------------------------------------------------------------
int runDumpScene() {
    SoundLibrary lib;
    lib.init(48000.0, findAudioDir());
    const std::string path = scenePathFor(lib.dir());
    Scene scene;
    EngineTuning tuning;
    if (!loadScene(scene, lib, tuning, path))
        scene = buildScene(lib);
    if (!saveScene(scene, lib, tuning, path)) {
        std::fprintf(stderr, "[dump-scene] FAILED to write %s\n", path.c_str());
        return 1;
    }
    std::printf("[dump-scene] wrote %s\n", path.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Windowed game
// ---------------------------------------------------------------------------
struct GameOptions {
    double autoExitSeconds = 0.0;
    bool startInDevMode = false;     // --dev
    bool forceSlice = false;         // --slice: the playable slice, ignore scene.txt
    bool fontTest = false;           // --font-test: overlay the whole charset
    std::string screenshotPath;      // --screenshot: BMP of the final frame
    float lookYaw = 0.0f;            // --look: initial view direction (debug)
    float lookPitch = 0.0f;
    std::vector<std::string> keys;   // --keys: injected key-downs (debug),
                                     // one every 0.4 s starting at 0.8 s
};

// Debug key injection: name -> SDL keycode.
SDL_Keycode keycodeFor(const std::string& name) {
    if (name == "up") return SDLK_UP;
    if (name == "down") return SDLK_DOWN;
    if (name == "left") return SDLK_LEFT;
    if (name == "right") return SDLK_RIGHT;
    if (name == "enter") return SDLK_RETURN;
    if (name == "esc") return SDLK_ESCAPE;
    if (name == "f1") return SDLK_F1;
    if (name == "f2") return SDLK_F2;
    if (name == "f3") return SDLK_F3;
    if (name.size() == 1) return static_cast<SDL_Keycode>(name[0]);
    return SDLK_UNKNOWN;
}

int runGame(const GameOptions& opt) {
    const double autoExitSeconds = opt.autoExitSeconds;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow("audio_game", 1280, 720,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext glctx = SDL_GL_CreateContext(window);
    if (glctx == nullptr || !SDL_GL_MakeCurrent(window, glctx)) {
        std::fprintf(stderr, "GL context failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    if (!loadGLFunctions()) {
        std::fprintf(stderr, "Failed to load GL 3.3 functions\n");
        SDL_Quit();
        return 1;
    }
    std::printf("GL_VERSION: %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    // Audio: open the device first so synthesis and engine prepare() use the
    // real device rate; callbacks only start after everything is built.
    AudioDevice device;
    if (!device.init(48000)) {
        std::fprintf(stderr, "Audio device init failed\n");
        SDL_Quit();
        return 1;
    }
    const double sr = device.sampleRate();
    std::printf("Audio device: %.0f Hz stereo\n", sr);
    std::printf("Synthesizing sounds...\n");

    SoundLibrary lib;
    lib.init(sr, findAudioDir());

    const std::string scenePath = scenePathFor(lib.dir());
    Scene scene;
    EngineTuning loadedTuning;
    if (opt.forceSlice) {
        scene = buildSliceScene(lib);
        std::printf("--slice: playing the built-in slice (scene.txt ignored)\n");
    } else if (!loadScene(scene, lib, loadedTuning, scenePath)) {
        scene = buildSliceScene(lib);
        std::printf("No %s -- using the built-in playable slice (C in dev mode saves it)\n",
                    scenePath.c_str());
    }

    // Live tunables: one fixed file at the REPO ROOT (path baked in at build time
    // via AUDIO_GAME_REPO_ROOT), so it is in the same easy-to-find place no matter
    // which directory you launch the exe from. If it doesn't exist yet, write a
    // template seeded with the current defaults so you have the full key list to
    // edit. Load it now (over the scene's engine params); the frame loop re-reads
    // it whenever it changes on disk -- edit while playing, no rebuild.
#ifdef AUDIO_GAME_REPO_ROOT
    const std::string kTunablesPath = std::string(AUDIO_GAME_REPO_ROOT) + "/tunables.txt";
#else
    const std::string kTunablesPath = "tunables.txt";   // fallback: working dir
#endif
    if (!tune::load(kTunablesPath))
        tune::writeTemplate(kTunablesPath);   // first run: drop a starter file
    tune::applyEngine(kTunablesPath, loadedTuning);
    std::printf("[tunables] file: %s\n", kTunablesPath.c_str());

    Player player;
    player.yaw = opt.lookYaw;
    player.pitch = opt.lookPitch;
    player.pos = { 0.0f, -30.0f };   // start in the south riverside sanctuary
    ListenerPose listener{ player.eyePos(), player.yaw, player.pitch };

    AudioWorld world;
    world.tuning() = loadedTuning;   // before init: prepare() snapshots params
    world.init(sr, listener);

    SoundDirector director;
    director.init(&world, &lib);

    // Warm-up block on the game thread: XYZPanEngine::process() initializes
    // two function-local statics on first call (Engine.cpp:546/549), which
    // under MSVC's thread-safe-init runs a guarded one-time init. Doing one
    // render here keeps the first real audio callback lock-free. Costs ~10 ms
    // of the 800 ms startup fade.
    {
        std::vector<float> warmup(static_cast<size_t>(AudioWorld::kMaxBlock) * 2);
        world.render(warmup.data(), AudioWorld::kMaxBlock);
    }

    if (!device.start(&world)) {
        std::fprintf(stderr, "Audio device start failed\n");
        SDL_Quit();
        return 1;
    }

    Renderer renderer;
    if (!renderer.init(scene)) {
        std::fprintf(stderr, "Renderer init failed\n");
        // world outlives this scope's return only until its destructor runs --
        // and it was constructed after device, so it dies first. Stop callbacks
        // before tearing it down.
        device.stop();
        SDL_Quit();
        return 1;
    }

    std::printf("Controls: WASD move, mouse look, Shift sprint, E echo-ping, ESC release mouse / quit\n"
                "Dev panel: F1 stats toggle, F2 dev menu (place/delete/select objects,\n"
                "           edit sound profiles, fly, save scene),\n"
                "           F3 blind mode (lit <-> near-total darkness), Tab full-bright override\n");

    DevMode dev;
    dev.init(&lib, &director, &world.tuning(), scenePath);
    if (opt.startInDevMode) dev.toggle(player);

    SliceGame slice;
    slice.init(scene, player.eyePos(), lib);

    std::vector<OverlayRect> overlayRects;
    std::vector<OverlayText> overlayTexts;

    bool mouseCaptured = SDL_SetWindowRelativeMouseMode(window, true);
    bool fullBright = false;   // Tab: debug override on top of the chosen mode
    bool blindMode = false;    // F3: near-total darkness (audio-first look)
    bool showStats = false;
    bool running = true;

    float groundGlowBoost = 0.0f;
    bool pingRequested = false;
    std::vector<FootstepEvent> steps;
    std::vector<BumpEvent> bumps;

    auto t0 = std::chrono::steady_clock::now();
    auto prev = t0;
    double statTimer = 0.0;
    int statFrames = 0;
    size_t nextKey = 0;

    while (running) {
        PlayerInput input;

        // Live tunables: re-read tunables.txt if it changed on disk (cheap mtime
        // check). AI / slice / player / view knobs are globals read in place;
        // engine params are poured into world.tuning() (applied every frame in
        // makeParams). Edit the file on a second monitor and it takes effect here.
        tune::pollReload(kTunablesPath, &world.tuning());

        // Debug key injection (--keys), one every 0.4 s starting at 0.8 s.
        {
            const double now = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            if (nextKey < opt.keys.size() && now >= 0.8 + 0.4 * static_cast<double>(nextKey)) {
                SDL_Event kev{};
                kev.type = SDL_EVENT_KEY_DOWN;
                kev.key.key = keycodeFor(opt.keys[nextKey]);
                SDL_PushEvent(&kev);
                ++nextKey;
            }
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    // Dev mode gets first refusal (incl. repeats for held
                    // arrows, and ESC to close its menu instead of quitting).
                    if (dev.enabled &&
                        dev.handleKey(ev.key.key, ev.key.repeat, scene, player,
                                      listener, renderer))
                        break;
                    if (ev.key.repeat) break;
                    if (ev.key.key == SDLK_E) {
                        pingRequested = true;   // fired in the loop with fresh occluders
                    } else if (ev.key.key == SDLK_ESCAPE) {
                        if (mouseCaptured) {
                            SDL_SetWindowRelativeMouseMode(window, false);
                            mouseCaptured = false;
                        } else {
                            running = false;
                        }
                    } else if (ev.key.key == SDLK_TAB) {
                        fullBright = !fullBright;
                        std::printf("[dev] full-bright debug: %s\n", fullBright ? "ON" : "off");
                    } else if (ev.key.key == SDLK_F1) {
                        showStats = !showStats;
                    } else if (ev.key.key == SDLK_F2) {
                        dev.toggle(player);
                    } else if (ev.key.key == SDLK_F3) {
                        blindMode = !blindMode;
                        std::printf("[dev] visual mode: %s\n",
                                    blindMode ? "blind (near-total darkness; navigate by ear)"
                                              : "lit (solid objects, full map)");
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    dev.handleWheel(ev.wheel.y, scene, listener, renderer);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (!mouseCaptured) {
                        SDL_SetWindowRelativeMouseMode(window, true);
                        mouseCaptured = true;
                    } else if (ev.button.button == SDL_BUTTON_LEFT) {
                        dev.handleClick(scene, player, renderer);
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (mouseCaptured) {
                        input.mouseDX += ev.motion.xrel;
                        input.mouseDY += ev.motion.yrel;
                    }
                    break;
                default:
                    break;
            }
        }

        const bool* keys = SDL_GetKeyboardState(nullptr);
        input.forward = keys[SDL_SCANCODE_W];
        input.back = keys[SDL_SCANCODE_S];
        input.left = keys[SDL_SCANCODE_A];
        input.right = keys[SDL_SCANCODE_D];
        input.sprint = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
        input.up = keys[SDL_SCANCODE_SPACE];     // fly mode only
        input.down = keys[SDL_SCANCODE_LCTRL];

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(0.1f, std::chrono::duration<float>(now - prev).count());
        prev = now;
        const double elapsed = std::chrono::duration<double>(now - t0).count();

        steps.clear();
        bumps.clear();
        player.update(dt, input, scene, steps, bumps);
        updateBugs(scene, dt);

        // Occluders rebuilt per frame (dev-mode placement occludes immediately);
        // shared by the audio path AND the stalker's hearing/steering so the
        // predator hears the world exactly as the player does.
        std::vector<Occluder> occluders = buildOccluders(scene);

        listener = { player.eyePos(), player.yaw, player.pitch };
        for (const FootstepEvent& s : steps) {
            director.onFootstep(scene, s.material, s.pos, listener);
            hearSoundStalkers(scene, s.pos, footstepLoudness(scene, s.material, input.sprint),
                              occluders);
            groundGlowBoost = 1.0f;
        }
        for (const BumpEvent& b : bumps) {
            director.onImpact(scene, b.objectId, b.pos, listener);
            hearSoundStalkers(scene, b.pos, tune::kImpactLoudness, occluders);  // impacts are loud
        }
        director.tickPing(dt);
        if (pingRequested) {
            pingRequested = false;
            // Only a ping that actually fires (past its cooldown) is a beacon.
            if (director.onPing(listener))
                hearSoundStalkers(scene, listener.pos, kPingLoudness, occluders);
        }
        updateStalkers(scene, dt, occluders, &director, &listener);
        // Slice brain: win/lose/respawn. May teleport the player, so run it
        // before we publish the listener pose to the audio engine.
        slice.update(dt, scene, player, director, listener);
        listener = { player.eyePos(), player.yaw, player.pitch };  // respawn moved us
        director.update(dt, scene, listener);
        world.setOccluders(std::move(occluders));
        world.updateParams(listener);

        // Decay bump flashes (~400 ms) and the footstep glow pulse. Reveal-on-
        // sound decays slower so one-shot blips fade smoothly; director.update()
        // re-raises it to the steady loop target first, so a playing loop stays
        // pinned and only the tail after a pulse fades.
        const float flashDecay = std::exp(-dt * 7.0f);
        const float revealDecay = std::exp(-dt * 3.0f);
        for (Tree& t : scene.trees) { t.flash *= flashDecay; t.reveal *= revealDecay; }
        for (Boulder& b : scene.boulders) { b.flash *= flashDecay; b.reveal *= revealDecay; }
        for (Emitter& e : scene.emitters) e.reveal *= revealDecay;
        for (Bug& b : scene.bugs) b.reveal *= revealDecay;
        for (Stalker& s : scene.stalkers) s.reveal *= revealDecay;
        groundGlowBoost *= std::exp(-dt * 6.0f);

        // Dev inspect pick + highlight (after decay so the highlight holds);
        // in paint mode this also drag-paints under a held left button.
        dev.frameUpdate(scene, player, renderer);

        const bool lastFrame = autoExitSeconds > 0.0 && elapsed >= autoExitSeconds;

        int fbw = 0, fbh = 0;
        SDL_GetWindowSizeInPixels(window, &fbw, &fbh);
        const VisualMode mode = fullBright ? VisualMode::FullBright
                              : blindMode ? VisualMode::Blind
                                          : VisualMode::Lit;
        DevPreview previewData;
        const bool hasPreview = dev.preview(player, previewData);
        renderer.draw(scene, listener.pos, listener.yaw, listener.pitch,
                      fbw, fbh, static_cast<float>(elapsed), groundGlowBoost, mode,
                      hasPreview ? &previewData : nullptr);

        overlayRects.clear();
        overlayTexts.clear();
        if (opt.fontTest) {
            overlayRects.push_back({ 20.0f, 20.0f, 920.0f, 130.0f, { 0, 0, 0, 0.8f } });
            overlayTexts.push_back({ 30.0f, 30.0f, 3.0f, { 1, 1, 1, 1 },
                                     " !\"#$%&'()*+,-./0123456789:;<=>?" });
            overlayTexts.push_back({ 30.0f, 60.0f, 3.0f, { 1, 1, 1, 1 },
                                     "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_" });
            overlayTexts.push_back({ 30.0f, 90.0f, 3.0f, { 0.55f, 0.85f, 1.0f, 1 },
                                     "THE QUICK BROWN FOX JUMPS OVER 13 LAZY DOGS." });
            overlayTexts.push_back({ 30.0f, 120.0f, 2.0f, { 1.0f, 0.92f, 0.35f, 1 },
                                     "GAIN 0.80  RADIUS 2.5 M  [ WATER ]  <ON/OFF>" });
        }
        // Slice HUD: one centered line at the top (objective / outcome). Hidden
        // in dev mode so it does not fight the editor UI.
        if (slice.active() && !dev.enabled && !slice.hud().empty()) {
            const float scale = 2.0f;
            const float tw = Renderer::textWidth(slice.hud(), scale);
            overlayTexts.push_back({ (fbw - tw) * 0.5f, 24.0f, scale,
                                     { 0.85f, 0.9f, 1.0f, 1.0f }, slice.hud() });
        }
        dev.buildOverlay(scene, fbw, fbh, overlayRects, overlayTexts);
        renderer.drawOverlay(overlayRects, overlayTexts, fbw, fbh);

        if (lastFrame && !opt.screenshotPath.empty()) {
            std::vector<unsigned char> rgb(static_cast<size_t>(fbw) * fbh * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, fbw, fbh, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
            if (writeBmp24(opt.screenshotPath.c_str(), rgb, fbw, fbh))
                std::printf("[screenshot] wrote %s (%dx%d)\n",
                            opt.screenshotPath.c_str(), fbw, fbh);
            else
                std::fprintf(stderr, "[screenshot] FAILED: %s\n", opt.screenshotPath.c_str());
        }

        SDL_GL_SwapWindow(window);

        ++statFrames;
        statTimer += dt;
        if (statTimer >= 1.0) {
            if (showStats) {
                std::printf("fps %5.1f | voices %2d | audio cpu %5.2f%%\n",
                            statFrames / statTimer, world.activeVoices(),
                            100.0 * world.consumeCpuLoad());
            } else {
                world.consumeCpuLoad();  // keep counters from accumulating
            }
            statTimer = 0.0;
            statFrames = 0;
        }

        if (autoExitSeconds > 0.0 && elapsed >= autoExitSeconds)
            running = false;
    }

    device.stop();
    renderer.shutdown();
    SDL_GL_DestroyContext(glctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool renderTest = false;
    bool dumpScene = false;
    GameOptions opt;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--render-test") == 0) {
            renderTest = true;
        } else if (std::strcmp(argv[i], "--dump-scene") == 0) {
            dumpScene = true;
        } else if (std::strcmp(argv[i], "--dev") == 0) {
            opt.startInDevMode = true;
        } else if (std::strcmp(argv[i], "--slice") == 0) {
            opt.forceSlice = true;
        } else if (std::strcmp(argv[i], "--font-test") == 0) {
            opt.fontTest = true;
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            opt.screenshotPath = argv[++i];
        } else if (std::strcmp(argv[i], "--look") == 0 && i + 2 < argc) {
            opt.lookYaw = static_cast<float>(std::atof(argv[++i]));
            opt.lookPitch = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            std::string list = argv[++i];
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                const size_t end = (comma == std::string::npos) ? list.size() : comma;
                if (end > start) opt.keys.push_back(list.substr(start, end - start));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (std::strcmp(argv[i], "--auto-exit") == 0 && i + 1 < argc) {
            opt.autoExitSeconds = std::atof(argv[++i]);
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            std::fprintf(stderr,
                         "Usage: audio_game [--render-test] [--dump-scene] [--auto-exit N]\n"
                         "                  [--dev] [--slice] [--font-test] [--screenshot out.bmp]\n");
            return 64;
        }
    }

    if (renderTest)
        return runRenderTest();
    if (dumpScene)
        return runDumpScene();
    return runGame(opt);
}
