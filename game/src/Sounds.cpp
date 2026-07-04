#include "Sounds.h"
#include "SoundLibrary.h"
#include <cmath>
#include <random>
#include <algorithm>
#include <vector>

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Simple one-pole lowpass (same topology the engine uses for air absorption).
struct OnePoleLP {
    float a = 0.0f, y = 0.0f;
    void setCutoff(float hz, float sr) {
        a = std::exp(-kTwoPi * std::max(10.0f, hz) / sr);
    }
    float process(float x) { y = (1.0f - a) * x + a * y; return y; }
};

// RBJ biquad bandpass (constant skirt gain), used for the wind whistle band
// and the water sparkle layer.
struct Bandpass {
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    void set(float hz, float q, float sr) {
        const float w0 = kTwoPi * hz / sr;
        const float alpha = std::sin(w0) / (2.0f * q);
        const float a0 = 1.0f + alpha;
        b0 = alpha / a0;
        b1 = 0.0f;
        b2 = -alpha / a0;
        a1 = -2.0f * std::cos(w0) / a0;
        a2 = (1.0f - alpha) / a0;
    }
    float process(float x) {
        const float out = b0 * x + z1;
        z1 = b1 * x - a1 * out + z2;
        z2 = b2 * x - a2 * out;
        return out;
    }
};

float frand(std::mt19937& rng, float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}

void normalize(std::vector<float>& buf, float peakTarget) {
    float peak = 0.0f;
    for (float v : buf) peak = std::max(peak, std::abs(v));
    if (peak > 1e-9f) {
        const float g = peakTarget / peak;
        for (float& v : buf) v *= g;
    }
}

// Crossfade the final fadeLen samples into the head so the loop point is
// seamless, then drop the tail.
void makeSeamless(std::vector<float>& buf, int fadeLen) {
    if (static_cast<int>(buf.size()) <= fadeLen * 2) return;
    const int n = static_cast<int>(buf.size()) - fadeLen;
    for (int i = 0; i < fadeLen; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(fadeLen);
        buf[static_cast<size_t>(i)] = buf[static_cast<size_t>(i)] * t
                                    + buf[static_cast<size_t>(n + i)] * (1.0f - t);
    }
    buf.resize(static_cast<size_t>(n));
}

std::vector<float> synthWater(float sr, unsigned seed) {
    std::mt19937 rng(seed);
    const int n = static_cast<int>(sr * 8.5f);  // extra 0.5 s consumed by loop crossfade
    std::vector<float> buf(static_cast<size_t>(n));
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    OnePoleLP body;     // burble body
    OnePoleLP sparkLP;  // tames the sparkle band
    Bandpass spark;
    spark.set(4000.0f, 1.2f, sr);
    sparkLP.setCutoff(7000.0f, sr);

    // Slow LFOs for amplitude + cutoff motion (random phases per seed)
    const float r1 = frand(rng, 0.11f, 0.16f), p1 = frand(rng, 0.0f, kTwoPi);
    const float r2 = frand(rng, 0.27f, 0.37f), p2 = frand(rng, 0.0f, kTwoPi);
    const float r3 = frand(rng, 0.05f, 0.08f), p3 = frand(rng, 0.0f, kTwoPi);

    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        const float l1 = std::sin(kTwoPi * r1 * t + p1);
        const float l2 = std::sin(kTwoPi * r2 * t + p2);
        const float l3 = std::sin(kTwoPi * r3 * t + p3);

        // Cutoff wanders 900-2100 Hz; refresh coefficient every 64 samples.
        if ((i & 63) == 0) {
            const float cutoff = 1500.0f + 450.0f * l1 + 150.0f * l2;
            body.setCutoff(cutoff, sr);
        }
        const float amp = 0.75f + 0.18f * l2 + 0.07f * l3;
        const float burble = body.process(noise(rng)) * amp;

        const float sparkAmp = 0.10f + 0.05f * l3;
        const float sparkle = sparkLP.process(spark.process(noise(rng))) * sparkAmp;

        buf[static_cast<size_t>(i)] = burble + sparkle;
    }
    makeSeamless(buf, static_cast<int>(sr * 0.5f));
    normalize(buf, 0.9f);
    return buf;
}

std::vector<float> synthWind(float sr, unsigned seed) {
    std::mt19937 rng(seed);
    const int n = static_cast<int>(sr * 12.5f);
    std::vector<float> buf(static_cast<size_t>(n));
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    Bandpass band;
    float cutoff = 500.0f, cutoffTarget = 500.0f;
    float gain = 0.7f, gainTarget = 0.7f;
    band.set(cutoff, 1.8f, sr);

    for (int i = 0; i < n; ++i) {
        // Random-walk targets, gentle slew, coefficient refresh every 128 samples.
        if ((i & 2047) == 0) {
            cutoffTarget = std::clamp(cutoffTarget + frand(rng, -120.0f, 120.0f), 300.0f, 800.0f);
            gainTarget = std::clamp(gainTarget + frand(rng, -0.15f, 0.15f), 0.35f, 1.0f);
        }
        cutoff += (cutoffTarget - cutoff) * 0.0004f;
        gain += (gainTarget - gain) * 0.0004f;
        if ((i & 127) == 0) band.set(cutoff, 1.8f, sr);
        buf[static_cast<size_t>(i)] = band.process(noise(rng)) * gain;
    }
    makeSeamless(buf, static_cast<int>(sr * 0.5f));
    normalize(buf, 0.9f);
    return buf;
}

// One chirp: sine sweep with sharp attack/decay envelope and light vibrato.
void addChirp(std::vector<float>& buf, float sr, int start,
              float f0, float f1, float durS, float amp, float vibHz, float vibDepth) {
    const int len = static_cast<int>(durS * sr);
    float phase = 0.0f;
    for (int i = 0; i < len; ++i) {
        const int idx = start + i;
        if (idx >= static_cast<int>(buf.size())) break;
        const float t = static_cast<float>(i) / static_cast<float>(len);
        const float freq = f0 + (f1 - f0) * t
                         + vibDepth * std::sin(kTwoPi * vibHz * static_cast<float>(i) / sr);
        phase += freq / sr;
        // Fast attack (~10% of length), exponential-ish decay
        const float att = std::min(1.0f, t * 10.0f);
        const float dec = std::exp(-3.5f * t);
        buf[static_cast<size_t>(idx)] += std::sin(kTwoPi * phase) * amp * att * dec;
    }
}

std::vector<float> synthBird(float sr, unsigned seed) {
    std::mt19937 rng(seed);
    const float durS = frand(rng, 45.0f, 60.0f);
    const int n = static_cast<int>(durS * sr);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);

    // Phrases at randomized intervals; randomness baked into the buffer so no
    // runtime scheduling is needed. Leave the first/last second silent so the
    // loop point is trivially seamless.
    float t = frand(rng, 1.0f, 3.0f);
    while (t < durS - 2.0f) {
        const int chirps = std::uniform_int_distribution<int>(2, 5)(rng);
        float ct = t;
        for (int c = 0; c < chirps; ++c) {
            const float f0 = frand(rng, 3000.0f, 7000.0f);
            const float f1 = std::clamp(f0 + frand(rng, -2500.0f, 2500.0f), 3000.0f, 8000.0f);
            const float dur = frand(rng, 0.03f, 0.12f);
            addChirp(buf, sr, static_cast<int>(ct * sr), f0, f1, dur,
                     frand(rng, 0.5f, 0.9f), frand(rng, 25.0f, 60.0f), frand(rng, 0.0f, 220.0f));
            ct += dur + frand(rng, 0.02f, 0.12f);
        }
        t = ct + frand(rng, 1.5f, 6.0f);
    }
    normalize(buf, 0.9f);
    return buf;
}

std::vector<float> synthGrassStep(float sr, std::mt19937& rng) {
    const float durS = frand(rng, 0.16f, 0.22f);
    const int n = static_cast<int>(durS * sr);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    OnePoleLP lp;
    lp.setCutoff(frand(rng, 2500.0f, 3500.0f), sr);
    const float decay = frand(rng, 18.0f, 26.0f);
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        const float att = std::min(1.0f, t * 500.0f);  // ~2 ms attack
        buf[static_cast<size_t>(i)] = lp.process(noise(rng)) * att * std::exp(-decay * t);
    }
    // A few tiny secondary crunch bursts
    const int crunches = std::uniform_int_distribution<int>(2, 4)(rng);
    for (int c = 0; c < crunches; ++c) {
        const int start = static_cast<int>(frand(rng, 0.025f, 0.12f) * sr);
        const int len = static_cast<int>(frand(rng, 0.004f, 0.010f) * sr);
        OnePoleLP clp;
        clp.setCutoff(frand(rng, 3000.0f, 5000.0f), sr);
        const float amp = frand(rng, 0.25f, 0.5f);
        for (int i = 0; i < len && start + i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(len);
            buf[static_cast<size_t>(start + i)] += clp.process(noise(rng)) * amp * (1.0f - t);
        }
    }
    normalize(buf, 0.9f);
    return buf;
}

std::vector<float> synthStoneStep(float sr, std::mt19937& rng) {
    const float durS = frand(rng, 0.10f, 0.15f);
    const int n = static_cast<int>(durS * sr);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    // Sharp, bright click layer
    OnePoleLP lp;
    lp.setCutoff(frand(rng, 5000.0f, 7000.0f), sr);
    const float decay = frand(rng, 35.0f, 50.0f);
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        const float att = std::min(1.0f, t * 1000.0f);  // ~1 ms attack
        buf[static_cast<size_t>(i)] = lp.process(noise(rng)) * att * std::exp(-decay * t);
    }
    // Pitch-down thump underneath (~120 -> 60 Hz over 60 ms)
    const float thumpAmp = frand(rng, 0.4f, 0.6f);
    const float fStart = frand(rng, 110.0f, 140.0f);
    float phase = 0.0f;
    const int thumpLen = std::min(n, static_cast<int>(0.06f * sr));
    for (int i = 0; i < thumpLen; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(thumpLen);
        const float freq = fStart * (1.0f - 0.5f * t);
        phase += freq / sr;
        buf[static_cast<size_t>(i)] += std::sin(kTwoPi * phase) * thumpAmp * std::exp(-6.0f * t);
    }
    normalize(buf, 0.9f);
    return buf;
}

// Band-limited saw tone loop (bug drone). Additive synthesis with harmonics
// up to Nyquist; loop length is an exact integer number of periods so the
// seam is click-free without crossfading.
std::vector<float> synthSaw(float sr, float f0) {
    const int periods = std::max(1, static_cast<int>(2.0f * f0));  // ~2 s
    const int n = static_cast<int>(std::round(static_cast<float>(periods) * sr / f0));
    const float fExact = static_cast<float>(periods) * sr / static_cast<float>(n);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);

    const int harmonics = static_cast<int>(0.45f * sr / fExact);
    for (int k = 1; k <= harmonics; ++k) {
        const float amp = 1.0f / static_cast<float>(k);
        const float w = kTwoPi * fExact * static_cast<float>(k) / sr;
        float phase = 0.0f;
        for (int i = 0; i < n; ++i) {
            buf[static_cast<size_t>(i)] += std::sin(phase) * amp;
            phase += w;
            if (phase > kTwoPi) phase -= kTwoPi;
        }
    }
    normalize(buf, 0.9f);
    return buf;
}

// Hollow wood knock (tree trunk bump): two damped resonant modes excited by a
// short noise burst, plus the burst itself for the attack transient.
std::vector<float> synthKnock(float sr, std::mt19937& rng) {
    const float durS = frand(rng, 0.09f, 0.14f);
    const int n = static_cast<int>(durS * sr);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    const float f1 = frand(rng, 420.0f, 560.0f);
    const float f2 = f1 * frand(rng, 1.9f, 2.4f);
    Bandpass m1, m2;
    m1.set(f1, 9.0f, sr);
    m2.set(f2, 11.0f, sr);

    OnePoleLP burstLP;
    burstLP.setCutoff(frand(rng, 2500.0f, 4000.0f), sr);
    const int burstLen = static_cast<int>(0.006f * sr);

    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        const float exc = (i < burstLen) ? burstLP.process(noise(rng)) : 0.0f;
        buf[static_cast<size_t>(i)] = exc * 0.8f
            + m1.process(exc) * 5.0f * std::exp(-38.0f * t)
            + m2.process(exc) * 3.0f * std::exp(-55.0f * t);
    }
    normalize(buf, 0.9f);
    return buf;
}

// Player probe "ping": a short bright bell-like transient the player fires to
// read the space by its acoustic return. Two clean high modes (a fifth apart)
// with a fast noise-burst attack, decaying quickly so the tail you hear back is
// the room's reverb, not the ping itself. Dry and distinctive so its echoes
// stand out against ambience.
std::vector<float> synthPing(float sr, unsigned seed) {
    std::mt19937 rng(seed);
    const float durS = 0.18f;
    const int n = static_cast<int>(durS * sr);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    const float f1 = 1320.0f;
    const float f2 = f1 * 1.5f;   // a fifth up: rings clearly, easy to track
    Bandpass m1, m2;
    m1.set(f1, 18.0f, sr);
    m2.set(f2, 22.0f, sr);

    OnePoleLP burstLP;
    burstLP.setCutoff(6000.0f, sr);
    const int burstLen = static_cast<int>(0.003f * sr);

    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        const float exc = (i < burstLen) ? burstLP.process(noise(rng)) : 0.0f;
        buf[static_cast<size_t>(i)] = exc * 0.5f
            + m1.process(exc) * 6.0f * std::exp(-22.0f * t)
            + m2.process(exc) * 4.0f * std::exp(-30.0f * t);
    }
    normalize(buf, 0.9f);
    return buf;
}

// A warm looping music pad for the sanctuaries: a sustained major chord with
// slow beating between slightly detuned partials and a gentle amplitude sway,
// so it reads as safe, inviting, and unmistakably "music" against the ambience.
// Loop length is a whole number of the slowest beat period for a clean seam.
std::vector<float> synthMusicPad(float sr) {
    const float durS = 8.0f;
    const int n = static_cast<int>(durS * sr);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);

    // C major triad + octave, each voice a pair detuned by ~0.5 Hz to beat.
    const float roots[4] = { 261.63f, 329.63f, 392.00f, 523.25f };  // C4 E4 G4 C5
    for (float f : roots) {
        for (int s = 0; s < 2; ++s) {
            const float fd = f * (s == 0 ? 1.0f : 1.0015f);  // slight detune
            float phase = 0.0f;
            for (int i = 0; i < n; ++i) {
                phase += kTwoPi * fd / sr;
                buf[static_cast<size_t>(i)] += std::sin(phase) * 0.12f;
            }
        }
    }
    // Slow amplitude sway (breathing), 0.15 Hz.
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        buf[static_cast<size_t>(i)] *= 0.75f + 0.25f * std::sin(kTwoPi * 0.15f * t);
    }
    makeSeamless(buf, static_cast<int>(sr * 0.5f));
    normalize(buf, 0.9f);
    return buf;
}

// A short arpeggiated chord stinger: three sine partials struck in sequence,
// each ringing with a soft exponential decay. `rising` plays low->high (a warm
// major resolution for the win); false plays high->low over a diminished voicing
// (an unresolved fall for the fail). Dry and clear so it reads over ambience.
std::vector<float> synthSting(float sr, bool rising) {
    const float durS = 1.6f;
    const int n = static_cast<int>(durS * sr);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);

    // Major triad up (win) vs a falling tritone-laden voicing down (fail).
    const float winF[3]  = { 392.0f, 494.0f, 587.0f };   // G4 B4 D5
    const float failF[3] = { 466.0f, 330.0f, 233.0f };   // Bb4 E4 Bb3 (tritone drop)
    const float* fr = rising ? winF : failF;

    for (int v = 0; v < 3; ++v) {
        const int start = static_cast<int>(static_cast<float>(v) * 0.16f * sr);
        const float f = fr[v];
        float phase = 0.0f;
        for (int i = start; i < n; ++i) {
            const float t = static_cast<float>(i - start) / sr;
            phase += kTwoPi * f / sr;
            const float env = std::exp(-2.2f * t) * std::min(1.0f, t * 60.0f);
            buf[static_cast<size_t>(i)] += std::sin(phase) * env * 0.4f;
        }
    }
    normalize(buf, 0.9f);
    return buf;
}

// Heavy dull thud (boulder bump): pitch-dropping low sine plus a lowpassed
// noise scuff.
std::vector<float> synthThud(float sr, std::mt19937& rng) {
    const float durS = frand(rng, 0.12f, 0.18f);
    const int n = static_cast<int>(durS * sr);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    const float fStart = frand(rng, 80.0f, 110.0f);
    float phase = 0.0f;
    OnePoleLP scuff;
    scuff.setCutoff(frand(rng, 700.0f, 1100.0f), sr);
    const float scuffAmp = frand(rng, 0.25f, 0.4f);

    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        const float freq = fStart * (1.0f - 0.45f * std::min(1.0f, t * 12.0f));
        phase += freq / sr;
        const float att = std::min(1.0f, t * 700.0f);
        buf[static_cast<size_t>(i)] =
            std::sin(kTwoPi * phase) * att * std::exp(-22.0f * t)
            + scuff.process(noise(rng)) * scuffAmp * att * std::exp(-35.0f * t);
    }
    normalize(buf, 0.9f);
    return buf;
}

// Stalker footstep: a heavy, low, menacing tread -- distinct from the player's
// bright grass/stone steps so you can tell the predator's gait from your own in
// the dark. A deep pitch-down thump (lower and longer than synthStoneStep's) for
// the weight of the foot landing, plus a dark low-passed noise scuff for the drag
// of a big body. Deliberately dull (no bright click layer) so it reads as *large*.
std::vector<float> synthStalkerStep(float sr, std::mt19937& rng) {
    const float durS = frand(rng, 0.18f, 0.24f);
    const int n = static_cast<int>(durS * sr);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    // Deep pitch-down thump (~70 -> 35 Hz over ~90 ms): the foot's impact.
    const float thumpAmp = frand(rng, 0.7f, 0.9f);
    const float fStart = frand(rng, 60.0f, 78.0f);
    float phase = 0.0f;
    const int thumpLen = std::min(n, static_cast<int>(0.09f * sr));
    for (int i = 0; i < thumpLen; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(thumpLen);
        const float freq = fStart * (1.0f - 0.5f * t);
        phase += freq / sr;
        buf[static_cast<size_t>(i)] += std::sin(kTwoPi * phase) * thumpAmp * std::exp(-5.0f * t);
    }
    // Dark scuff: heavily low-passed noise, dragged out under the thump.
    OnePoleLP scuff;
    scuff.setCutoff(frand(rng, 350.0f, 550.0f), sr);
    const float scuffAmp = frand(rng, 0.25f, 0.4f);
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        const float att = std::min(1.0f, t * 400.0f);   // ~2.5 ms attack
        buf[static_cast<size_t>(i)] += scuff.process(noise(rng)) * scuffAmp * att
                                       * std::exp(-14.0f * t);
    }
    normalize(buf, 0.9f);
    return buf;
}

// Stalker "call" vocalizations: short one-shots that voice the predator's MOOD so
// the player reads its AI state in the dark (fired on state entry + periodically
// while in-state by SoundDirector::onStalkerCall). Each `mood` is a distinct
// timbre/contour built from a couple of detuned sines + a formant-ish bandpass on
// noise (a throat). Kept dark and animal, a family with the drone/step. `mood`:
//   0 roam      low slow breathy exhale (calm, incurious)
//   1 hunt      rising layered growl (urgent, closing)
//   2 search    mid two-note questioning warble (probing)
//   3 confused  wavering falling tone (disoriented, after a whiffed lunge)
// The lunge screech is its own louder synth (synthStalkerLunge) below.
std::vector<float> synthStalkerCall(float sr, int mood, unsigned seed) {
    std::mt19937 rng(seed);
    const float durS = (mood == 0) ? 0.85f : (mood == 2) ? 0.7f : 0.6f;
    const int n = static_cast<int>(durS * sr);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    // Base pitch + contour per mood (Hz at t=0 -> Hz at t=1, linear glide).
    float f0 = 90.0f, f1 = 90.0f, growl = 0.0f;
    switch (mood) {
        case 0: f0 = 85.0f;  f1 = 70.0f;  growl = 0.15f; break; // roam: settle down
        case 1: f0 = 120.0f; f1 = 180.0f; growl = 0.55f; break; // hunt: rising growl
        case 2: f0 = 130.0f; f1 = 150.0f; growl = 0.25f; break; // search: light warble
        default:f0 = 160.0f; f1 = 95.0f;  growl = 0.35f; break; // confused: falling
    }

    // Throat noise routed through a moving bandpass (a crude formant) for body.
    Bandpass throat;
    OnePoleLP amNoise;
    amNoise.setCutoff(28.0f, sr);   // slow amplitude wobble for the growl
    float ph1 = 0.0f, ph2 = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        float f = f0 + (f1 - f0) * t;
        // search: a distinct two-note lilt; confused: a slow pitch waver.
        if (mood == 2) f += (t > 0.5f ? 22.0f : 0.0f);
        if (mood == 3) f += 12.0f * std::sin(kTwoPi * 3.5f * t);
        ph1 += kTwoPi * f / sr;
        ph2 += kTwoPi * f * 1.5f / sr;             // a fifth above, detuned body
        // Growl = slow AM on the tone from lowpassed noise.
        const float am = 1.0f + growl * amNoise.process(noise(rng));
        throat.set(std::clamp(f * 6.0f, 200.0f, 2200.0f), 4.0f, sr);
        const float breath = throat.process(noise(rng)) * 0.4f;
        // ADSR-ish: soft attack, sustain, gentle release.
        const float env = std::min(1.0f, t * 8.0f) * std::min(1.0f, (1.0f - t) * 6.0f);
        buf[static_cast<size_t>(i)] =
            (std::sin(ph1) * 0.6f + std::sin(ph2) * 0.25f) * am * env
            + breath * env;
    }
    normalize(buf, 0.85f);
    return buf;
}

// Stalker lunge screech: the loud, sharp TELL of a committed attack -- a fast
// rising shriek (formant sweep up) with a hard noisy edge, so the player hears it
// coming and has a beat to sidestep the charge. Brighter and louder than the
// mood calls; it should cut over the drone.
std::vector<float> synthStalkerLunge(float sr, unsigned seed) {
    std::mt19937 rng(seed);
    const float durS = 0.55f;
    const int n = static_cast<int>(durS * sr);
    std::vector<float> buf(static_cast<size_t>(n), 0.0f);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    Bandpass shriek;
    float ph = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        // Fast exponential rise 220 -> ~900 Hz: the intake-and-strike.
        const float f = 220.0f * std::pow(4.1f, t);
        ph += kTwoPi * f / sr;
        shriek.set(std::clamp(f * 2.5f, 300.0f, 6000.0f), 3.0f, sr);
        const float edge = shriek.process(noise(rng)) * 0.6f;
        // Sharp attack, sustained through the rise, quick cut at the top.
        const float env = std::min(1.0f, t * 40.0f) * (t < 0.85f ? 1.0f : (1.0f - t) / 0.15f);
        buf[static_cast<size_t>(i)] =
            (std::sin(ph) * 0.5f + std::sin(ph * 2.0f) * 0.2f + edge) * env;
    }
    normalize(buf, 0.95f);
    return buf;
}

} // namespace

void registerBuiltinSounds(SoundLibrary& lib, double sampleRate) {
    const float sr = static_cast<float>(sampleRate);

    lib.addVariant(lib.getOrAddSet("water", true), synthWater(sr, 0xA1CE5EEDu));
    lib.addVariant(lib.getOrAddSet("wind", true), synthWind(sr, 0xB1077EEDu));

    const int bird = lib.getOrAddSet("bird", true);
    lib.addVariant(bird, synthBird(sr, 101u));
    lib.addVariant(bird, synthBird(sr, 202u));
    lib.addVariant(bird, synthBird(sr, 303u));

    std::mt19937 stepRng(0x57E9u);
    const int grass = lib.getOrAddSet("grass", true);
    const int stone = lib.getOrAddSet("stone", true);
    for (int i = 0; i < 8; ++i) lib.addVariant(grass, synthGrassStep(sr, stepRng));
    for (int i = 0; i < 8; ++i) lib.addVariant(stone, synthStoneStep(sr, stepRng));

    std::mt19937 hitRng(0x817u);
    const int knock = lib.getOrAddSet("knock", true);
    const int thud = lib.getOrAddSet("thud", true);
    for (int i = 0; i < 4; ++i) lib.addVariant(knock, synthKnock(sr, hitRng));
    for (int i = 0; i < 4; ++i) lib.addVariant(thud, synthThud(sr, hitRng));

    // Saw drone, 3 pitches (bugs sharing the set pick variants by object id).
    const int saw = lib.getOrAddSet("saw", true);
    lib.addVariant(saw, synthSaw(sr, 196.0f));
    lib.addVariant(saw, synthSaw(sr, 247.0f));
    lib.addVariant(saw, synthSaw(sr, 311.0f));

    // Player echo-probe ping (single deterministic variant: it is a reference
    // tone the player learns, so no per-fire timbre jitter).
    lib.addVariant(lib.getOrAddSet("ping", true), synthPing(sr, 0x9143u));

    // A low, slow-moving predator drone for the Stalker. A detuned saw pair an
    // octave below the bugs: menacing, carries far, and its motion is legible
    // through Doppler + occlusion.
    const int stalker = lib.getOrAddSet("stalker", true);
    lib.addVariant(stalker, synthSaw(sr, 55.0f));
    lib.addVariant(stalker, synthSaw(sr, 82.5f));

    // Stalker footsteps: heavy low tread the player hears as the predator's gait
    // (fired from updateStalkers as it moves; louder/faster when Chasing).
    std::mt19937 stalkerStepRng(0x57A1C5u);
    const int stalkerStep = lib.getOrAddSet("stalker_step", true);
    for (int i = 0; i < 6; ++i) lib.addVariant(stalkerStep, synthStalkerStep(sr, stalkerStepRng));

    // Stalker mood calls: one variant per AI mood (index = mood: 0 roam, 1 hunt,
    // 2 search, 3 confused). Fired by SoundDirector::onStalkerCall so the player
    // reads the predator's state by ear. Plus a loud lunge screech (the attack tell).
    const int stalkerCall = lib.getOrAddSet("stalker_call", true);
    for (int mood = 0; mood < 4; ++mood)
        lib.addVariant(stalkerCall, synthStalkerCall(sr, mood, 0x5CA11u + static_cast<unsigned>(mood) * 7u));
    lib.addVariant(lib.getOrAddSet("stalker_lunge", true), synthStalkerLunge(sr, 0x1D6E60u));

    // Win/lose stingers for the playable slice.
    lib.addVariant(lib.getOrAddSet("sting_win", true), synthSting(sr, true));
    lib.addVariant(lib.getOrAddSet("sting_fail", true), synthSting(sr, false));

    // Sanctuary music pad (repels the Stalker; a beacon of safety by ear).
    lib.addVariant(lib.getOrAddSet("music", true), synthMusicPad(sr));
}
