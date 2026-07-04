#pragma once
// One sound emitter: a full XYZPanEngine instance, a mono sample buffer, and a
// lock-free parameter handoff. Loop sources play until stopped; one-shot
// sources (footstep pool voices) free themselves when their buffer ends. Both
// are controlled from the game thread via an atomic state machine:
//
//   Free ──trigger()/triggerLoop()──> Triggered ──(audio thread adopts)──> Playing
//   Playing ──requestStop()──> StopRequested ──(audio thread clears)──> Free
//   Playing (one-shot, buffer + tail done) ──(audio thread)──> Free
//
// The game thread writes pending* fields (and worldPos/emitGain/params) before
// the release-store to Triggered; the audio thread's acquire-load makes them
// visible. Voices in Free state are never processed (engine smoothers freeze).

#include "ParamBuffer.h"
#include "xyzpan/Engine.h"
#include <glm/vec3.hpp>
#include <atomic>
#include <cstddef>
#include <vector>

enum class VoiceState : int { Free = 0, Triggered = 1, Playing = 2, StopRequested = 3 };

class Source {
public:
    // ---- Game-thread configuration (set before the device starts, or before
    //      the release-store that activates the voice) ----
    glm::vec3 worldPos{0.0f};            // emitter position, world meters
    float emitGain = 1.0f;               // pre-engine input gain
    float audibleRadius = 35.0f;         // engine sphereRadius, meters
    float verbWet = 0.0f;
    bool dopplerEnabled = true;
    float dopplerScale = 1.0f;           // propagation-delay slope multiplier
    bool isOneShot = false;

    ParamBuffer params;

    void prepare(double sampleRate, int maxBlockSize,
                 const xyzpan::EngineParams& initialParams);

    // Loop activation, game thread. Caller must
    // have already written worldPos/emitGain/verbWet and fresh params.
    // Returns false if the voice is not Free.
    bool triggerLoop(const std::vector<float>* buf, size_t startOffset);

    // One-shot trigger, game thread. Caller must have already written fresh
    // params (with the foot position) into `params`. Returns false if the
    // voice is busy.
    bool trigger(const std::vector<float>* buf, float gain, double rate);

    // Occlusion targets, game thread (computed by AudioWorld::updateParams
    // from scene geometry). The audio thread smooths toward these over
    // smoothMs and applies a low-pass + gain to the voice input before the
    // engine (xyzpan itself is off-limits), so the muffling also darkens the
    // voice's reverb send -- correct for occlusion.
    void setOcclusion(float cutoffHz, float gain, float smoothMs) {
        occlCutoffTarget_.store(cutoffHz, std::memory_order_relaxed);
        occlGainTarget_.store(gain, std::memory_order_relaxed);
        occlSmoothMs_.store(smoothMs, std::memory_order_relaxed);
    }

    // Ask the audio thread to silence and free this voice (next block).
    // Game thread. Safe on any state; a Triggered-but-not-adopted voice is
    // simply cancelled.
    void requestStop() {
        state_.store(static_cast<int>(VoiceState::StopRequested), std::memory_order_release);
    }

    bool isActive() const {
        return state_.load(std::memory_order_relaxed) != static_cast<int>(VoiceState::Free);
    }

    // Audio thread: render `n` samples and accumulate into mixL/mixR with
    // `mixGain`. scratchIn/L/R are caller-provided blocks of >= n floats.
    void renderAdd(float* mixL, float* mixR,
                   float* scratchIn, float* scratchL, float* scratchR,
                   int n, float mixGain);

private:
    xyzpan::XYZPanEngine engine_;
    const std::vector<float>* buffer_ = nullptr;
    double sampleRate_ = 48000.0;

    // Occlusion handoff (game thread stores, audio thread loads; relaxed is
    // enough -- a frame of staleness is absorbed by the smoothing).
    std::atomic<float> occlCutoffTarget_{20000.0f};
    std::atomic<float> occlGainTarget_{1.0f};
    std::atomic<float> occlSmoothMs_{100.0f};

    // Audio-thread occlusion state: smoothed cutoff/gain plus an RBJ biquad
    // low-pass (transposed direct form II). Snapped to targets on trigger.
    float occlCutoff_ = 20000.0f;
    float occlGain_ = 1.0f;
    float occlZ1_ = 0.0f, occlZ2_ = 0.0f;
    void applyOcclusion(float* x, int n);

    // Audio-thread playback state
    double playhead_ = 0.0;
    double rate_ = 1.0;
    float voiceGain_ = 1.0f;
    int tailLeft_ = 0;                   // post-buffer flush samples (one-shots)

    // Activation handoff: game thread writes pending* then flips state_ to
    // Triggered (release); audio thread consumes (acquire).
    std::atomic<int> state_{static_cast<int>(VoiceState::Free)};
    const std::vector<float>* pendingBuffer_ = nullptr;
    float pendingGain_ = 1.0f;
    double pendingRate_ = 1.0;
    std::size_t pendingOffset_ = 0;      // loop start phase (loops only)
};
