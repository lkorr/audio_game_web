// WebAudioDevice: drives audio through Emscripten's Wasm Audio Worklets.
//
// Why the C audio-worklet API (not a hand-written JS AudioWorkletProcessor):
// Emscripten's -sAUDIO_WORKLET runs a C callback ON the audio thread, sharing
// THIS module's linear memory. That means AudioWorld::render() executes on the
// real-time audio thread reading the same ParamBuffer the main thread writes --
// the exact native contract (miniaudio callback -> render), with no manual
// SharedArrayBuffer marshalling. Single audio thread => no cross-origin
// isolation / COOP-COEP headers required (the plan's "plug into any static
// host" decision).
//
// Lifecycle (context created in C -- the Emscripten-recommended path):
//   main.js: on user gesture -> wasm_create_audio() creates the AudioContext
//            in C and returns its sampleRate (drives the whole engine)
//   main.js: -> wasm_init(sampleRate) builds the world at that rate
//   main.js: -> wasm_start_audio() boots the worklet thread + node
//   main.js: -> wasm_resume_audio() resumes the context (allowed: still in the
//            gesture's async continuation)
//   audio thread: processAudio() -> world->render()

#include "WebAudioDevice.h"
#include "web_bridge.h"
#include "AudioWorld.h"

#include <emscripten/webaudio.h>

#include <cstring>
#include <cstdio>
#include <vector>

namespace {
AudioWorld* g_world = nullptr;

// Stack for the audio worklet thread (required by the API). AudioWorld::render
// -> Source::renderAdd -> XYZPanEngine::process uses substantial stack scratch
// (per-block filter/ER/reverb locals), so 16 KB overflows and aborts inside
// process(). 512 KB is generous headroom. Must be aligned; static lifetime.
alignas(16) uint8_t g_audioThreadStack[512 * 1024];

EMSCRIPTEN_WEBAUDIO_T g_ctx = 0;
bool g_audioStarted = false;

// The audio-thread render callback. Runs per render quantum (128 frames) on the
// dedicated audio thread. Emscripten's output is PLANAR (channel-major:
// data[ch*frames + i]); AudioWorld renders INTERLEAVED, so we render into a
// scratch and split into the planar channels.
bool processAudio(int numInputs, const AudioSampleFrame* /*inputs*/,
                  int numOutputs, AudioSampleFrame* outputs,
                  int /*numParams*/, const AudioParamFrame* /*params*/,
                  void* /*userData*/) {
    if (numOutputs < 1) return true;
    AudioSampleFrame& out = outputs[0];
    const int frames = out.samplesPerChannel;          // 128
    const int channels = out.numberOfChannels;          // 2 (stereo)

    if (g_world == nullptr || channels < 2) {
        // Silence: Emscripten pre-zeros output buffers, so just keep the node alive.
        return true;
    }

    // Interleaved scratch (2 ch). 128-frame quantum; sized once, reused.
    static std::vector<float> interleaved(2 * 256, 0.0f);
    if (static_cast<int>(interleaved.size()) < frames * 2)
        interleaved.assign(static_cast<size_t>(frames) * 2, 0.0f);

    g_world->render(interleaved.data(), frames);

    // De-interleave into the planar output channels. Emscripten's AudioSample
    // frame layout is planar: out.data[ch*frames + i].
    float* dst = out.data;
    for (int i = 0; i < frames; ++i) {
        dst[0 * frames + i] = interleaved[static_cast<size_t>(i) * 2 + 0];
        dst[1 * frames + i] = interleaved[static_cast<size_t>(i) * 2 + 1];
    }
    return true;   // keep processing
}

// Called once the worklet processor is created: build the node and connect it
// to the context destination, then start playback.
void audioWorkletProcessorCreated(EMSCRIPTEN_WEBAUDIO_T ctx, bool success,
                                  void* /*userData*/) {
    if (!success) {
        std::fprintf(stderr, "[web-audio] worklet processor creation failed\n");
        return;
    }
    int outputChannelCounts[1] = { 2 };
    EmscriptenAudioWorkletNodeCreateOptions options = {};
    options.numberOfInputs = 0;
    options.numberOfOutputs = 1;
    options.outputChannelCounts = outputChannelCounts;

    EMSCRIPTEN_WEBAUDIO_T node =
        emscripten_create_wasm_audio_worklet_node(ctx, "audiogame-renderer",
                                                  &options, &processAudio, nullptr);
    // Connect node output 0 to the context destination (input 0).
    emscripten_audio_node_connect(node, ctx, 0, 0);
    g_audioStarted = true;
    std::printf("[web-audio] worklet connected; audio live.\n");
}

// Called once the audio thread is initialized: register our processor.
void audioThreadInitialized(EMSCRIPTEN_WEBAUDIO_T ctx, bool success, void* /*userData*/) {
    if (!success) {
        std::fprintf(stderr, "[web-audio] audio worklet thread init failed\n");
        return;
    }
    WebAudioWorkletProcessorCreateOptions opts = {};
    opts.name = "audiogame-renderer";
    opts.numAudioParams = 0;
    emscripten_create_wasm_audio_worklet_processor_async(
        ctx, &opts, &audioWorkletProcessorCreated, nullptr);
}
} // namespace

bool WebAudioDevice::init(unsigned actualRate) {
    rate_ = (actualRate > 0) ? static_cast<double>(actualRate) : 48000.0;
    return true;
}

bool WebAudioDevice::start(AudioWorld* world) {
    g_world = world;
    return true;
}

void WebAudioDevice::stop() {
    g_world = nullptr;
}

extern "C" {

// Create the AudioContext in C (from the JS gesture) and report its rate so
// the engine can be built to match. Returns 0 on failure.
double wasm_create_audio() {
    if (g_ctx == 0)
        g_ctx = emscripten_create_audio_context(nullptr);
    if (g_ctx == 0) return 0.0;
    return emscripten_audio_context_sample_rate(g_ctx);
}

// Boot the worklet thread + node on the context created above.
void wasm_start_audio() {
    if (g_audioStarted || g_ctx == 0) return;
    emscripten_start_wasm_audio_worklet_thread_async(
        g_ctx, g_audioThreadStack, sizeof(g_audioThreadStack),
        &audioThreadInitialized, nullptr);
}

// Resume the context (call from within the gesture continuation).
void wasm_resume_audio() {
    if (g_ctx == 0) return;
    if (emscripten_audio_context_state(g_ctx) != AUDIO_CONTEXT_STATE_RUNNING)
        emscripten_resume_audio_context_sync(g_ctx);
}

void wasm_render(float* out, int frames) {
    if (g_world == nullptr || frames <= 0) {
        if (out != nullptr && frames > 0)
            std::memset(out, 0, sizeof(float) * 2 * static_cast<size_t>(frames));
        return;
    }
    g_world->render(out, frames);
}

double wasm_cpu_load() {
    return g_world != nullptr ? g_world->consumeCpuLoad() : -1.0;
}

} // extern "C"
