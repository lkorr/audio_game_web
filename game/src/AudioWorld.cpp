#include "AudioWorld.h"
#include "xyzpan/Constants.h"
#include <glm/geometric.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>

#if defined(_MSC_VER) || defined(__SSE2__)
#include <immintrin.h>
#include <pmmintrin.h>
#define HAVE_SSE_DENORMAL_CONTROL 1
#endif

xyzpan::EngineParams AudioWorld::makeParams(const Source& src, const glm::vec3& srcPos,
                                            const ListenerPose& listener) const {
    xyzpan::EngineParams p;  // defaults already neutralize LFOs/orbits/test tone/stereo width
    tuning_.apply(p);        // dev-menu engine tuning (defaults are a no-op)

    // World meters throughout. Verified against Engine.cpp: the engine
    // subtracts listenerX/Y/Z from x/y/z directly, and the distance gain /
    // air absorption / near-field paths normalize by sphereRadius, so meters
    // are scale-safe there. Binaural azimuth/rear/elevation cues use
    // distance-difference virtual ears at +-0.087 units -- in meters that is
    // a physical head radius, so meters are correct there too.
    p.x = srcPos.x;
    p.y = srcPos.y;
    p.z = srcPos.z;
    p.listenerX = listener.pos.x;
    p.listenerY = listener.pos.y;
    p.listenerZ = listener.pos.z;
    p.listenerYaw = listener.yaw;
    p.listenerPitch = listener.pitch;
    p.listenerRoll = 0.0f;

    // Per-source sphere of influence: the sound's audible size. Distance
    // gain reaches the floor at this range.
    p.sphereRadius = src.audibleRadius;

    // Doppler/propagation-delay workaround (the one place meters break):
    // Engine.cpp computes the delay fraction as clamp(dist / sqrt(3), 0, 1)
    // (kInvSqrt3 -- a [-1,1]-cube assumption), which saturates at 1.73 m.
    // Since delay = frac * distDelayMaxMs and distDelayMaxMs is a per-block
    // param, we drive distDelayMaxMs = max(dist, sqrt(3)) * 1000 / 343 so the
    // smoothed delay equals dist/343 s exactly at every distance: physically
    // correct propagation delay and doppler, params-only. dopplerScale
    // multiplies the slope, exaggerating the motion pitch shift (bugs).
    const float dist = glm::length(srcPos - listener.pos);
    p.distDelayMaxMs = std::min(450.0f,
        std::max(dist * src.dopplerScale, xyzpan::kSqrt3) * (1000.0f / 343.0f));
    p.dopplerEnabled = src.dopplerEnabled;

    // Reverb character (size/decay/damping) and ER on/off come from tuning;
    // wet level stays per-source (sound rule).
    p.verbWet = src.verbWet;

    // Floor bounce: driven by the source's height above the ground beneath it
    // (engine LOCAL MOD), not the elevation angle to the listener. Ground is
    // the z=0 plane in this scene; full bounce at ground level, fading to
    // none at EP_FloorFadeHeight meters up.
    const float floorFade = std::max(0.5f, tuning_.v[EP_FloorFadeHeight]);
    const float srcHeight = std::max(0.0f, srcPos.z);
    p.floorBounceFactor = std::clamp(1.0f - srcHeight / floorFade, 0.0f, 1.0f);

    p.swarmNodeCount = 1;        // only node 0 processes
    // lfo*Depth, stereoOrbit*Depth, stereoWidth, testToneEnabled: defaults are
    // already 0 / false. Tempo-sync fields stay at defaults.

    return p;
}

void AudioWorld::applyOcclusion(Source& src, const glm::vec3& srcPos,
                                const ListenerPose& listener) const {
    OcclusionResult occ;   // open when disabled
    if (tuning_.v[EP_OcclEnabled] > 0.5f) {
        OcclusionTuning ot;
        ot.attenScale = tuning_.v[EP_OcclAttenScale];
        ot.maxAttenDb = tuning_.v[EP_OcclMaxAttenDb];
        ot.cutoffScale = tuning_.v[EP_OcclCutoffScale];
        ot.minCutoffHz = tuning_.v[EP_OcclMinCutoffHz];
        ot.refFreqHz = tuning_.v[EP_OcclRefFreqHz];
        ot.fresnelHz = tuning_.v[EP_OcclFresnelHz];
        ot.thickness = tuning_.v[EP_OcclThickness] > 0.5f;
        occ = computeOcclusion(listener.pos, srcPos, occluders_, ot);
    }
    src.setOcclusion(occ.cutoffHz, occ.gain, tuning_.v[EP_OcclSmoothMs]);
}

void AudioWorld::init(double sampleRate, const ListenerPose& initialListener) {
    sampleRate_ = sampleRate;

    mixL_.resize(kMaxBlock);
    mixR_.resize(kMaxBlock);
    scratchIn_.resize(kMaxBlock);
    srcL_.resize(kMaxBlock);
    srcR_.resize(kMaxBlock);

    // Startup fade-in over ~0.8 s.
    masterGain_ = 0.0f;
    masterGainInc_ = static_cast<float>(1.0 / (0.8 * sampleRate));

    // All voices prepared (engine built, no allocation later) but Free --
    // renderAdd() skips them until triggered.
    auto makeVoice = [&](bool oneShot) {
        auto src = std::make_unique<Source>();
        src->worldPos = initialListener.pos;
        src->emitGain = 1.0f;
        src->verbWet = 0.0f;
        src->dopplerEnabled = !oneShot;
        src->isOneShot = oneShot;
        src->prepare(sampleRate, kMaxBlock,
                     makeParams(*src, initialListener.pos, initialListener));
        return src;
    };
    for (int i = 0; i < kLoopVoices; ++i) loops_.push_back(makeVoice(false));
    for (int i = 0; i < kOneShotVoices; ++i) oneShots_.push_back(makeVoice(true));
}

void AudioWorld::updateParams(const ListenerPose& listener) {
    auto refresh = [&](Source& src) {
        src.params.write(makeParams(src, src.worldPos, listener));
        applyOcclusion(src, src.worldPos, listener);
    };
    for (auto& src : loops_)
        if (src->isActive()) refresh(*src);
    for (auto& src : oneShots_)
        if (src->isActive()) refresh(*src);
}

int AudioWorld::placeLoop(const std::vector<float>* buf, size_t startOffset,
                          const glm::vec3& pos, const VoiceConfig& cfg,
                          const ListenerPose& listener) {
    if (buf == nullptr || buf->empty()) return -1;
    for (size_t i = 0; i < loops_.size(); ++i) {
        Source* src = loops_[i].get();
        if (src->isActive()) continue;
        // Publish config before the release-store inside triggerLoop().
        src->worldPos = pos;
        src->emitGain = cfg.gain;
        src->audibleRadius = cfg.audibleRadius;
        src->verbWet = cfg.verbWet;
        src->dopplerEnabled = cfg.doppler;
        src->dopplerScale = cfg.dopplerScale;
        src->params.write(makeParams(*src, pos, listener));
        applyOcclusion(*src, pos, listener);
        if (src->triggerLoop(buf, startOffset))
            return static_cast<int>(i);
    }
    return -1;
}

void AudioWorld::stopLoop(int voiceIdx) {
    if (voiceIdx >= 0 && voiceIdx < static_cast<int>(loops_.size()))
        loops_[static_cast<size_t>(voiceIdx)]->requestStop();
}

void AudioWorld::setLoopPos(int voiceIdx, const glm::vec3& pos) {
    if (voiceIdx >= 0 && voiceIdx < static_cast<int>(loops_.size()))
        loops_[static_cast<size_t>(voiceIdx)]->worldPos = pos;
    // updateParams() publishes the new position to the audio thread.
}

bool AudioWorld::playOneShot(const std::vector<float>* buf, const glm::vec3& pos,
                             double rate, const VoiceConfig& cfg,
                             const ListenerPose& listener) {
    if (buf == nullptr || buf->empty()) return false;
    for (auto& voice : oneShots_) {
        if (voice->isActive()) continue;
        voice->worldPos = pos;
        voice->audibleRadius = cfg.audibleRadius;
        voice->verbWet = cfg.verbWet;
        voice->dopplerEnabled = cfg.doppler;
        voice->dopplerScale = cfg.dopplerScale;
        voice->params.write(makeParams(*voice, pos, listener));
        applyOcclusion(*voice, pos, listener);
        return voice->trigger(buf, cfg.gain, rate);
    }
    return false;
}

int AudioWorld::activeVoices() const {
    int n = 0;
    for (const auto& src : loops_)
        if (src->isActive()) ++n;
    for (const auto& src : oneShots_)
        if (src->isActive()) ++n;
    return n;
}

double AudioWorld::consumeCpuLoad() {
    const uint64_t ns = renderNanos_.exchange(0, std::memory_order_relaxed);
    const uint64_t frames = renderedFrames_.exchange(0, std::memory_order_relaxed);
    if (frames == 0) return 0.0;
    const double budget = static_cast<double>(frames) / sampleRate_ * 1e9;
    return static_cast<double>(ns) / budget;
}

void AudioWorld::render(float* interleavedOut, int frames) {
    // Audio-thread contract: no allocation, no locks, no I/O below this line.
#ifdef HAVE_SSE_DENORMAL_CONTROL
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
#ifndef __EMSCRIPTEN__
    // CPU-load instrumentation. Disabled on web: this runs on the Emscripten
    // audio-worklet thread, where std::chrono::steady_clock::now() is
    // unsupported and aborts (__libcpp_verbose_abort). The CPU readout is a dev
    // stat only; consumeCpuLoad() reports 0 on web.
    const auto t0 = std::chrono::steady_clock::now();
#endif

    int done = 0;
    while (done < frames) {
        const int n = std::min(frames - done, kMaxBlock);
        float* mixL = mixL_.data();
        float* mixR = mixR_.data();
        std::fill(mixL, mixL + n, 0.0f);
        std::fill(mixR, mixR + n, 0.0f);

        for (auto& src : loops_)
            src->renderAdd(mixL, mixR, scratchIn_.data(), srcL_.data(), srcR_.data(), n, kPerSourceGain);
        for (auto& src : oneShots_)
            src->renderAdd(mixL, mixR, scratchIn_.data(), srcL_.data(), srcR_.data(), n, kPerSourceGain);

        // Master: fade-in, soft clip, interleave.
        float* out = interleavedOut + static_cast<size_t>(done) * 2;
        for (int i = 0; i < n; ++i) {
            masterGain_ = std::min(1.0f, masterGain_ + masterGainInc_);
            out[2 * i] = std::tanh(mixL[i] * masterGain_);
            out[2 * i + 1] = std::tanh(mixR[i] * masterGain_);
        }
        done += n;
    }

#ifndef __EMSCRIPTEN__
    const auto t1 = std::chrono::steady_clock::now();
    renderNanos_.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
        std::memory_order_relaxed);
    renderedFrames_.fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
#endif
}
