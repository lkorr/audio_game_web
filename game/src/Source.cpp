#include "Source.h"
#include <algorithm>
#include <cmath>
#include <cstring>

void Source::prepare(double sampleRate, int maxBlockSize,
                     const xyzpan::EngineParams& initialParams) {
    sampleRate_ = sampleRate;
    engine_.prepare(sampleRate, maxBlockSize, initialParams);
    engine_.snapListenerRotation(initialParams.listenerYaw,
                                 initialParams.listenerPitch,
                                 initialParams.listenerRoll);
    params.write(initialParams);
}

bool Source::triggerLoop(const std::vector<float>* buf, size_t startOffset) {
    if (state_.load(std::memory_order_acquire) != static_cast<int>(VoiceState::Free))
        return false;
    pendingBuffer_ = buf;
    pendingGain_ = 1.0f;
    pendingRate_ = 1.0;
    pendingOffset_ = startOffset;
    state_.store(static_cast<int>(VoiceState::Triggered), std::memory_order_release);
    return true;
}

bool Source::trigger(const std::vector<float>* buf, float gain, double rate) {
    if (state_.load(std::memory_order_acquire) != static_cast<int>(VoiceState::Free))
        return false;
    pendingBuffer_ = buf;
    pendingGain_ = gain;
    pendingRate_ = rate;
    state_.store(static_cast<int>(VoiceState::Triggered), std::memory_order_release);
    return true;
}

void Source::renderAdd(float* mixL, float* mixR,
                       float* scratchIn, float* scratchL, float* scratchR,
                       int n, float mixGain) {
    const int st = state_.load(std::memory_order_acquire);
    if (st == static_cast<int>(VoiceState::Free)) return;
    bool stopping = false;
    if (st == static_cast<int>(VoiceState::StopRequested)) {
        // Render one final block with the input faded to zero, then free.
        // The engine keeps its (stale) tail state; it is not processed again
        // until re-triggered. The dropped tail is at most verbWet-level.
        if (buffer_ == nullptr || buffer_->empty()) {
            state_.store(static_cast<int>(VoiceState::Free), std::memory_order_release);
            return;
        }
        stopping = true;
    }
    if (st == static_cast<int>(VoiceState::Triggered)) {
        buffer_ = pendingBuffer_;
        voiceGain_ = pendingGain_;
        rate_ = pendingRate_;
        if (isOneShot) {
            playhead_ = 0.0;
            // Flush time after the buffer ends: covers the ITD/doppler delay
            // lines and filter ring-out (verbWet is 0 on one-shots).
            tailLeft_ = 4096;
        } else {
            playhead_ = (buffer_ != nullptr && !buffer_->empty())
                ? static_cast<double>(pendingOffset_ % buffer_->size()) : 0.0;
        }
        // Snap occlusion to the trigger-time targets (written before the
        // release-store) so a voice born behind a wall starts muffled instead
        // of sweeping shut over the smoothing time.
        occlCutoff_ = occlCutoffTarget_.load(std::memory_order_relaxed);
        occlGain_ = occlGainTarget_.load(std::memory_order_relaxed);
        occlZ1_ = occlZ2_ = 0.0f;
        state_.store(static_cast<int>(VoiceState::Playing), std::memory_order_relaxed);
    }

    if (buffer_ == nullptr || buffer_->empty()) return;

    xyzpan::EngineParams p;
    params.read(p);
    engine_.setParams(p);

    const std::vector<float>& buf = *buffer_;
    const size_t size = buf.size();
    const float g = emitGain * voiceGain_;
    bool finished = false;

    if (!isOneShot) {
        // Looping playback at unity rate.
        size_t pos = static_cast<size_t>(playhead_);
        for (int i = 0; i < n; ++i) {
            scratchIn[i] = buf[pos] * g;
            if (++pos >= size) pos = 0;
        }
        playhead_ = static_cast<double>(pos);
    } else {
        // One-shot playback with linear-interpolated rate.
        double pos = playhead_;
        for (int i = 0; i < n; ++i) {
            if (pos < static_cast<double>(size - 1)) {
                const size_t i0 = static_cast<size_t>(pos);
                const float frac = static_cast<float>(pos - static_cast<double>(i0));
                scratchIn[i] = (buf[i0] * (1.0f - frac) + buf[i0 + 1] * frac) * g;
                pos += rate_;
            } else {
                scratchIn[i] = 0.0f;
                if (--tailLeft_ <= 0) finished = true;
            }
        }
        playhead_ = pos;
    }

    if (stopping) {
        // Linear fade across the final block — click-free stop.
        const float inc = 1.0f / static_cast<float>(n);
        for (int i = 0; i < n; ++i)
            scratchIn[i] *= 1.0f - inc * static_cast<float>(i + 1);
        finished = true;
    }

    applyOcclusion(scratchIn, n);

    const float* inputs[1] = { scratchIn };
    engine_.process(inputs, 1, scratchL, scratchR, nullptr, nullptr, n);

    for (int i = 0; i < n; ++i) {
        mixL[i] += scratchL[i] * mixGain;
        mixR[i] += scratchR[i] * mixGain;
    }

    if (finished) {
        if (stopping) buffer_ = nullptr;
        state_.store(static_cast<int>(VoiceState::Free), std::memory_order_release);
    }
}

void Source::applyOcclusion(float* x, int n) {
    const float cutoffTarget = occlCutoffTarget_.load(std::memory_order_relaxed);
    const float gainTarget = occlGainTarget_.load(std::memory_order_relaxed);

    // Block-rate smoothing; cutoff moves in log-frequency (perceptually even
    // sweep), gain linearly. Time constant is dev-tunable (occl_smooth_ms).
    const float tau = std::max(1.0f, occlSmoothMs_.load(std::memory_order_relaxed))
                    * 0.001f;
    const float a = 1.0f - std::exp(-static_cast<float>(n)
                                    / (tau * static_cast<float>(sampleRate_)));
    occlCutoff_ *= std::pow(cutoffTarget / occlCutoff_, a);
    const float gainPrev = occlGain_;
    occlGain_ += a * (gainTarget - occlGain_);

    if (occlCutoff_ >= 19500.0f && occlGain_ >= 0.999f) {
        // Open: bypass. The filter is transparent at this cutoff, so zeroed
        // state can't click when occlusion re-engages.
        occlZ1_ = occlZ2_ = 0.0f;
        return;
    }

    // RBJ cookbook low-pass, Butterworth Q. Coefficients once per block; the
    // smoothed cutoff keeps block-to-block steps small.
    const float fc = std::min(occlCutoff_, 0.45f * static_cast<float>(sampleRate_));
    const float w0 = 2.0f * 3.14159265f * fc / static_cast<float>(sampleRate_);
    const float cw = std::cos(w0), sw = std::sin(w0);
    const float alpha = sw / (2.0f * 0.70710678f);
    const float a0inv = 1.0f / (1.0f + alpha);
    const float b1 = (1.0f - cw) * a0inv;
    const float b0 = 0.5f * b1, b2 = b0;
    const float a1 = -2.0f * cw * a0inv;
    const float a2 = (1.0f - alpha) * a0inv;

    // Gain ramps linearly across the block to avoid zipper steps.
    float g = gainPrev;
    const float gInc = (occlGain_ - gainPrev) / static_cast<float>(n);
    float z1 = occlZ1_, z2 = occlZ2_;
    for (int i = 0; i < n; ++i) {
        const float in = x[i];
        const float out = b0 * in + z1;
        z1 = b1 * in - a1 * out + z2;
        z2 = b2 * in - a2 * out;
        g += gInc;
        x[i] = out * g;
    }
    occlZ1_ = z1;
    occlZ2_ = z2;
}
