#pragma once
// Owns the spatializer voice pools and renders the master mix. The audio
// callback (or the offline render test) calls render(); the game thread calls
// updateParams() once per frame and starts/stops sounds through placeLoop /
// stopLoop / playOneShot. *What* plays and *when* is decided one level up by
// SoundDirector -- this class only knows buffers, positions, and voices.

#include "Source.h"
#include "EngineTuning.h"
#include "Occlusion.h"
#include <glm/vec3.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

struct ListenerPose {
    glm::vec3 pos{0.0f};
    float yaw = 0.0f;    // radians, engine convention (see makeParams)
    float pitch = 0.0f;
};

class AudioWorld {
public:
    static constexpr int kMaxBlock = 512;
    // Each voice is a full prepared XYZPanEngine (~33 MB -- see memory notes),
    // so the pools are small and fixed. Loops: enough for the default scene's
    // 6 emitters + 2 bugs plus dev headroom. One-shots: footsteps + impacts +
    // idle chirps share this pool; a steal-free trigger just drops the sound.
    static constexpr int kLoopVoices = 12;
    static constexpr int kOneShotVoices = 6;
    static constexpr float kSphereRadius = 35.0f;   // audible radius, meters
    static constexpr float kPerSourceGain = 0.5f;

    // Prepare every voice engine. Must complete before the device starts.
    void init(double sampleRate, const ListenerPose& initialListener);

    // ---- Game thread ----
    void updateParams(const ListenerPose& listener);

    // Scene geometry that muffles blocked sounds (see Occlusion.h). Refresh
    // whenever objects are added/moved (per frame is fine; it's a handful of
    // shapes). Voices triggered or updated afterwards get occlusion applied.
    void setOccluders(std::vector<Occluder> occluders) { occluders_ = std::move(occluders); }

    // Per-voice sound character (from the SoundRule that triggered it).
    struct VoiceConfig {
        float gain = 1.0f;            // final gain (jitter already applied)
        float audibleRadius = 35.0f;  // engine sphere of influence, meters
        float verbWet = 0.0f;
        bool doppler = true;
        float dopplerScale = 1.0f;    // >1 exaggerates motion pitch shift
    };

    // Start a looping sound on a free loop voice. Returns the voice index
    // (stable handle for stopLoop/setLoopPos) or -1 if the pool is exhausted.
    int placeLoop(const std::vector<float>* buf, size_t startOffset,
                  const glm::vec3& pos, const VoiceConfig& cfg,
                  const ListenerPose& listener);
    // Fade out and free a loop voice returned by placeLoop.
    void stopLoop(int voiceIdx);
    // Move a playing loop (moving emitters: bugs). Game thread; the audio
    // thread only sees positions through the per-frame param snapshot.
    void setLoopPos(int voiceIdx, const glm::vec3& pos);

    // Fire a one-shot at a world position. Returns false if no voice is free.
    bool playOneShot(const std::vector<float>* buf, const glm::vec3& pos,
                     double rate, const VoiceConfig& cfg, const ListenerPose& listener);

    int activeVoices() const;
    // Average audio-callback CPU fraction since the last call (0..1).
    double consumeCpuLoad();

    // Live engine tuning (dev menu). Game thread only; edits flow into every
    // voice through makeParams() on the next updateParams()/trigger.
    EngineTuning& tuning() { return tuning_; }
    const EngineTuning& tuning() const { return tuning_; }

    // ---- Audio thread ----
    // Renders `frames` of interleaved stereo into `out`. Processes internally
    // in chunks of <= kMaxBlock. No allocation, no locks.
    void render(float* interleavedOut, int frames);

    double sampleRate() const { return sampleRate_; }

private:
    xyzpan::EngineParams makeParams(const Source& src, const glm::vec3& srcPos,
                                    const ListenerPose& listener) const;
    // Compute + publish occlusion for one voice (dev-panel EP_Occl* knobs).
    void applyOcclusion(Source& src, const glm::vec3& srcPos,
                        const ListenerPose& listener) const;

    double sampleRate_ = 48000.0;
    EngineTuning tuning_;
    std::vector<Occluder> occluders_;   // game thread only

    std::vector<std::unique_ptr<Source>> loops_;
    std::vector<std::unique_ptr<Source>> oneShots_;

    // Audio-thread scratch (preallocated)
    std::vector<float> mixL_, mixR_, scratchIn_, srcL_, srcR_;

    // Startup fade-in: masks the engines' distance-delay smoothers settling
    // from their reset values over the first ~0.5 s.
    float masterGain_ = 0.0f;
    float masterGainInc_ = 0.0f;

    // CPU instrumentation (audio thread writes, game thread reads)
    std::atomic<uint64_t> renderNanos_{0};
    std::atomic<uint64_t> renderedFrames_{0};
};
