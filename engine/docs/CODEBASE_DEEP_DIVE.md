# XYZPan Codebase Deep Dive

A C++ walkthrough of the XYZPan 3D spatial audio panner, organized by the topics most likely to come up in an Apple Core Audio programmer interview. Each section explains the C++ concept, shows the real code that implements it, and highlights what to emphasize in discussion.

---

## Table of Contents

1. [Real-Time Audio Safety](#1-real-time-audio-safety)
2. [Lock-Free Inter-Thread Communication](#2-lock-free-inter-thread-communication)
3. [Memory Management and RAII](#3-memory-management-and-raii)
4. [The Audio Processing Pipeline](#4-the-audio-processing-pipeline)
5. [Ring Buffers and Delay Lines](#5-ring-buffers-and-delay-lines)
6. [Filter Design and Implementation](#6-filter-design-and-implementation)
7. [Parameter Smoothing](#7-parameter-smoothing)
8. [Performance Optimization](#8-performance-optimization)
9. [C++ Language Features Used Throughout](#9-c-language-features-used-throughout)
10. [Multi-Instance Architecture](#10-multi-instance-architecture)
11. [OpenGL and the Render Thread](#11-opengl-and-the-render-thread)

---

## 1. Real-Time Audio Safety

**Why this matters for Apple:** Core Audio enforces hard real-time deadlines. The audio callback must return within a fixed time budget (e.g., 5.8ms at 44.1kHz/256 samples). Miss it and you get audible glitches. Apple interviewers will probe whether you understand what operations are forbidden on the audio thread.

### The Three Rules

The audio thread must **never**:
1. **Allocate or free heap memory** (`new`, `delete`, `malloc`, `free`, `std::vector::push_back` that triggers reallocation)
2. **Acquire a lock** (mutex, semaphore, spinlock) — another thread might hold it, causing unbounded wait
3. **Do I/O** (file reads, network, logging)

### How XYZPan Enforces This

**Rule 1 — No allocations:** Every buffer is pre-allocated in `prepare()`, which runs on the message thread before audio starts. The `process()` function uses only pre-existing memory.

```cpp
// Engine.h — monoBuffer is a member variable, allocated once
std::vector<float> monoBuffer;  // pre-allocated in prepare() to maxBlockSize

// Engine.cpp:55 — prepare() is called before audio starts
void XYZPanEngine::prepare(double inSampleRate, int inMaxBlockSize, ...) {
    monoBuffer.resize(static_cast<size_t>(inMaxBlockSize), 0.0f);
    // ...all delay lines, filter states also allocated here
}
```

The keyword `resize()` does allocate, but it only runs in `prepare()`, never in `process()`. Once `process()` starts, `monoBuffer` is just an array of floats sitting in memory. Accessing `monoBuffer[i]` is a pointer dereference — no allocation.

**Rule 2 — No locks:** Parameters flow from UI to audio thread via `std::atomic<float>`. The audio thread reads them with `.load()`, which is a single CPU instruction — it never blocks.

```cpp
// PluginProcessor.cpp — reading parameters on the audio thread
params.x = xSmooth_.current() * r;  // xSmooth_ was fed from xParam->load()
```

The `.load()` call on an atomic is guaranteed not to block. There is no mutex involved.

**Rule 3 — No I/O:** The engine is pure computation. It takes float arrays in, writes float arrays out. No file access, no logging, no string formatting.

### What `ScopedNoDenormals` Does

```cpp
// PluginProcessor.cpp — first line of processBlock
juce::ScopedNoDenormals noDenormals;
```

Denormalized floats are extremely small numbers (below ~1.2e-38) that CPUs process orders of magnitude slower than normal floats. In audio, filter states often decay toward zero and hit denormal range. This RAII guard sets a CPU flag (FTZ/DAZ — Flush To Zero / Denormals Are Zero) that forces those tiny values to zero instead. When the guard destructs at the end of `processBlock`, it restores the previous flag.

**Interview talking point:** "We set FTZ/DAZ at the top of the audio callback because IIR filter states decay toward denormal range during silence, which can cause 10-100x CPU spikes on x86."

---

## 2. Lock-Free Inter-Thread Communication

**Why this matters for Apple:** Core Audio is inherently multi-threaded — the audio render thread is separate from the main/UI thread. Apple engineers need to know you can move data between threads safely without locks.

### The Three-Thread Model

XYZPan has three threads that must communicate without blocking each other:

| Thread | Runs | Reads | Writes |
|--------|------|-------|--------|
| **Audio** | `processBlock()` at ~86Hz (512 samples @ 44.1kHz) | Atomic parameters | PositionBridge, SourceExportBuffer |
| **Message** (UI) | Event-driven, user interactions | APVTS ValueTree | Atomic parameters |
| **OpenGL** | `renderOpenGL()` at ~60fps | PositionBridge | Frame buffer |

### Lock-Free Double Buffer (PositionBridge)

This is the most important lock-free pattern in the codebase. The audio thread needs to tell the GL thread where the sound source is, but they can't share a mutex.

```cpp
// ui/PositionBridge.h

class PositionBridge {
public:
    // Called from audio thread after engine.process()
    void write(const SourcePositionSnapshot& pos) {
        const int idx = 1 - writeIdx_.load(std::memory_order_relaxed);
        buf_[idx] = pos;
        writeIdx_.store(idx, std::memory_order_release);
    }

    // Called from GL thread in renderOpenGL()
    [[nodiscard]] SourcePositionSnapshot read() const {
        return buf_[writeIdx_.load(std::memory_order_acquire)];
    }

private:
    SourcePositionSnapshot buf_[2];
    std::atomic<int> writeIdx_{0};
};
```

**How it works, step by step:**

1. There are two copies of the data (`buf_[0]` and `buf_[1]`).
2. `writeIdx_` always points to the buffer that was **most recently finished** writing.
3. When the audio thread wants to write, it computes `1 - writeIdx_` to get the **other** buffer (the one the reader isn't using). It writes there, then flips `writeIdx_`.
4. When the GL thread wants to read, it reads from `buf_[writeIdx_]` — the most recently completed write.

**Why this is safe:** The writer and reader are always accessing **different** buffers. There's no moment where both touch the same memory simultaneously.

### Memory Ordering — What `acquire` and `release` Mean

This is a concept you'll almost certainly be asked about.

```cpp
writeIdx_.store(idx, std::memory_order_release);   // writer
writeIdx_.load(std::memory_order_acquire);          // reader
```

**`memory_order_release` (writer side):** "All my writes to `buf_[idx]` above this line must be visible to other threads BEFORE this store becomes visible." It's a one-way fence — it prevents the compiler and CPU from reordering the data writes to happen after the index update.

**`memory_order_acquire` (reader side):** "After I load this index, I'm guaranteed to see all the writes that happened before the corresponding release store." It prevents the compiler from speculatively reading `buf_` data before the index load completes.

**`memory_order_relaxed`:** "I don't need ordering guarantees for this specific load." Used on line 2 of `write()` because the old value of `writeIdx_` is just used to compute which buffer to write to — it doesn't need to synchronize with any other thread's writes.

**Interview talking point:** "Release-acquire pairs create a happens-before relationship. The release store publishes the data, and the acquire load subscribes to it. Everything the writer did before the release is visible to the reader after the acquire."

### Atomic Parameter Access

```cpp
// PluginProcessor.h — raw pointers to JUCE's internal atomics
std::atomic<float>* xParam;  // points into APVTS-managed memory

// Constructor — cache the pointer once
xParam = apvts.getRawParameterValue(ParamID::X);

// processBlock — read on audio thread
float x = xParam->load();  // defaults to memory_order_seq_cst
```

`std::atomic<float>*` is a **raw pointer** (not a smart pointer) to an atomic float. The pointer itself is not atomic — it's set once in the constructor and never changes. What's atomic is the float it points to. The UI thread writes the float; the audio thread reads it. Both happen safely because `std::atomic` guarantees no torn reads (you'll never see half of a write).

**Why cache raw pointers?** Looking up a parameter by string ID in JUCE's ValueTree involves string comparison and tree traversal — too expensive for the audio thread. By caching the pointer in the constructor, the audio thread just dereferences a pointer (one CPU instruction) instead of searching a tree.

---

## 3. Memory Management and RAII

**Why this matters for Apple:** C++ memory management bugs (leaks, dangling pointers, use-after-free) are a major source of real-world crashes. Apple expects you to understand ownership semantics.

### RAII (Resource Acquisition Is Initialization)

RAII means: acquire a resource in a constructor, release it in the destructor. Since C++ guarantees destructors run when objects go out of scope, cleanup is automatic.

**`ScopedNoDenormals` is pure RAII:**
```cpp
{
    juce::ScopedNoDenormals noDenormals;  // constructor sets FTZ/DAZ flags
    // ... do audio processing ...
}   // destructor restores previous flags — even if an exception was thrown
```

The entire engine is RAII by design: `XYZPanEngine` owns its delay lines, filters, and smoothers as member variables. When the engine is destroyed, all members are destroyed automatically — no explicit cleanup code needed.

### Ownership Types in the Codebase

**1. Value members (most common — the default):**
```cpp
// Engine.h
BinauralPipeline src_;        // owned by value — destroyed when Engine is destroyed
dsp::FDNReverb   reverb_;     // same
dsp::LFO         lfoX_;       // same
```

When a class holds another class "by value" (not via pointer), the outer class's destructor automatically calls the inner class's destructor. No `delete` needed. This is the safest ownership model and the one used for everything in the engine.

**2. `std::vector` (owned dynamic arrays):**
```cpp
// Engine.h
std::vector<float> monoBuffer;
```

`std::vector` is a dynamically-sized array that manages its own heap memory. When the vector is destroyed, it frees its memory. When you call `.resize()` or `.assign()`, it handles reallocation internally. You never call `new[]` or `delete[]` yourself.

Internally, `std::vector` holds three pointers: `begin`, `end`, and `capacity`. When you access `monoBuffer[i]`, it's equivalent to `*(begin + i)` — a pointer add and dereference, same cost as a raw C array.

**3. Raw pointers (non-owning references):**
```cpp
// PluginProcessor.h
std::atomic<float>* xParam;  // does NOT own the memory; APVTS owns it
```

A raw pointer in C++ is just a memory address. It doesn't manage the lifetime of what it points to. Here, JUCE's APVTS owns the atomic floats; the processor just holds a pointer to look them up quickly. The processor must not delete this pointer — it doesn't own the memory.

**Interview talking point:** "Raw pointers are fine for non-owning references where the lifetime is guaranteed by the architecture — like cached parameter pointers where the APVTS outlives the processor."

**4. `std::shared_ptr` (shared ownership):**
```cpp
// PluginProcessor.h
std::shared_ptr<std::atomic<bool>> receivingBroadcast_;
```

`shared_ptr` uses reference counting: multiple owners can hold a copy, and the resource is freed when the last copy is destroyed. Here, the broadcast flag is shared between the processor and the listener hub so either can check/set it even if the other is being destroyed.

**Reference counting means:** An internal counter tracks how many `shared_ptr` instances point to the object. `shared_ptr` copy increments the count; destruction decrements it. When count hits zero, the object is deleted. This is thread-safe (the counter is atomic), but has overhead — each copy/destruction involves an atomic increment/decrement.

**5. `SharedResourcePointer` (JUCE's process-wide singleton):**
```cpp
// PluginProcessor.h
juce::SharedResourcePointer<SharedListenerHub> listenerHub_;
```

This is JUCE's pattern for objects shared across all plugin instances in a process. The first instance to create it allocates the hub; the last to be destroyed frees it. Internally it's similar to `shared_ptr` with a static instance map keyed by type.

### The `= delete` Pattern

```cpp
// Engine.h
XYZPanEngine(const XYZPanEngine&) = delete;
XYZPanEngine& operator=(const XYZPanEngine&) = delete;
```

`= delete` tells the compiler: "Do not generate this function. If anyone tries to copy this object, give a compile error."

Why delete copy operations? The engine owns delay line buffers, filter states, and ring buffer write positions. If you copied an engine, both copies would process audio independently but could diverge in confusing ways. More importantly, copying large buffers is expensive and never what you want on the audio thread. By deleting the copy constructor and copy assignment operator, you get a compile-time guarantee that nobody accidentally copies the engine.

---

## 4. The Audio Processing Pipeline

**Why this matters for Apple:** Understanding signal flow and how to structure a real-time audio pipeline is core to the role.

### Architecture: Engine vs. Plugin

XYZPan separates the DSP engine from the JUCE plugin wrapper:

```
Plugin Layer (JUCE)              Engine Layer (pure C++)
┌────────────────────┐           ┌────────────────────┐
│ PluginProcessor    │           │ XYZPanEngine        │
│  - APVTS params    │──params──▶│  - BinauralPipeline │
│  - processBlock()  │           │  - DistancePipeline │
│  - state save/load │◀─audio───│  - ChestPipeline    │
│  - preset manager  │           │  - FloorPipeline    │
└────────────────────┘           │  - ERPipeline       │
                                 │  - FDNReverb        │
                                 │  - LFOs             │
                                 └────────────────────┘
```

The engine has **zero JUCE dependency**. It takes raw float pointers and a parameter struct. This design means:
- The engine can be unit-tested without a plugin host
- Porting to a non-JUCE framework requires only rewriting the wrapper
- The engine compiles on any platform with a C++20 compiler

### The `process()` Function Structure

```cpp
// Engine.cpp:252
void XYZPanEngine::process(const float* const* inputs, int numInputChannels,
                            float* outL, float* outR,
                            float* auxL, float* auxR,
                            int numSamples)
```

**The signature explained:**
- `const float* const* inputs` — "pointer to const-pointer to const-float." This is a C-style array of channel pointers (standard audio API convention). `inputs[0]` is the left channel, `inputs[1]` is the right. The `const` on the floats means the engine won't modify the input buffers. The `const` on the outer pointer means it won't reassign the channel pointers.
- `float* outL, float* outR` — pre-allocated output buffers the engine writes into.
- `int numSamples` — how many samples to process this block.

**The two-phase structure:**

```
┌──────────────────────────────────────────────────────────────┐
│  BLOCK PREAMBLE (runs once per block, ~64-512 samples)      │
│  - Read parameters                                          │
│  - Update filter coefficients (calls cos, sin, pow, exp)    │
│  - Pre-compute distance-dependent targets                   │
│  - Smooth listener rotation angles                          │
│  - Pre-converge block-constant smoothers O(1)               │
├──────────────────────────────────────────────────────────────┤
│  PER-SAMPLE LOOP (runs numSamples times)                    │
│  for (int i = 0; i < numSamples; ++i) {                     │
│    - LFO tick + position modulation                         │
│    - Distance calculation (fastSqrt)                        │
│    - Head rotation (matrix multiply)                        │
│    - Binaural processing (ITD, ILD, head shadow)            │
│    - Chest/floor bounce                                     │
│    - Distance gain + air absorption                         │
│    - Early reflections                                      │
│    - Reverb                                                 │
│  }                                                          │
└──────────────────────────────────────────────────────────────┘
```

**Why two phases?** Functions like `std::cos()`, `std::sin()`, `std::pow()`, and `std::exp()` are transcendentals — they take 20-100 CPU cycles each. If you compute filter coefficients inside the per-sample loop, you burn hundreds of cycles per sample on math that barely changes. By computing once per block (typically 256-512 samples), you amortize that cost.

```cpp
// Engine.cpp:461-474 — block preamble precomputes dB-to-linear
const float chestGainLin = std::pow(10.0f, currentParams.chestGainDb / 20.0f);
// ...later in the per-sample loop, chestGainLin is just a float multiply
```

**Interview talking point:** "I separate block-rate coefficient computation from per-sample processing. Transcendentals run once per block; the inner loop uses only adds, multiplies, and table lookups."

### Input Handling

```cpp
// Engine.cpp:270-278
const float* inputL = inputs[0];
const float* inputR = (numInputChannels >= 2 && inputs[1] != nullptr) ? inputs[1] : nullptr;

if (inputR != nullptr) {
    for (int i = 0; i < numSamples; ++i)
        monoBuffer[static_cast<size_t>(i)] = 0.5f * (inputL[i] + inputR[i]);
}
```

The plugin accepts mono or stereo input. The engine sums stereo to mono for the main binaural pipeline (you can't pan a stereo signal to a point in 3D space — it must be mono first). The stereo width feature splits L/R input to separate spatial positions.

`static_cast<size_t>(i)` converts `int` to `size_t` (unsigned). `std::vector::operator[]` takes `size_t`. Without the cast, the compiler warns about signed/unsigned mismatch. This cast is safe because `i` is always non-negative inside the loop.

---

## 5. Ring Buffers and Delay Lines

**Why this matters for Apple:** Ring buffers are the fundamental data structure of real-time audio. You'll almost certainly be asked to implement or explain one.

### The Power-of-2 Bitmask Trick

```cpp
// FractionalDelayLine.h:29-35
void prepare(int capacitySamples) {
    int n = 1;
    while (n < capacitySamples + 4) n <<= 1;  // round up to power of 2
    mask_ = n - 1;
    buf_.assign(static_cast<size_t>(n), 0.0f);
    writePos_ = 0;
}
```

**Line by line:**
- `n <<= 1` is bit-shift left by 1, equivalent to `n *= 2`. The loop finds the smallest power of 2 that fits the requested capacity plus 4 (Hermite needs 4 taps).
- `mask_ = n - 1` creates a bitmask. For n=1024, mask=1023=0b1111111111.
- `buf_.assign(n, 0.0f)` resizes the vector to `n` elements and fills with zeros.

**Why power of 2?** Because wrapping a circular buffer normally requires modulo: `index % size`. Modulo is a division, which takes 20-40 CPU cycles on x86. But if the size is a power of 2, modulo is equivalent to a bitwise AND with (size-1):

```
index % 1024  ==  index & 1023  ==  index & 0x3FF
```

Bitwise AND takes 1 cycle. This is 20-40x faster, and the delay line's `read()` function runs on every single sample.

### Writing to the Ring Buffer

```cpp
// FractionalDelayLine.h:44-47
void push(float sample) {
    buf_[static_cast<size_t>(writePos_ & mask_)] = sample;
    ++writePos_;
}
```

`writePos_` is a plain `int` that increments forever (it never wraps manually). The `& mask_` operation handles wrapping. When `writePos_` hits 1024 on a 1024-element buffer, `1024 & 1023 = 0`, so it wraps back to index 0. This works even when `writePos_` overflows (integer overflow on unsigned types wraps predictably, and on signed types this would technically be undefined behavior, but in practice works on all modern hardware and compilers).

### Hermite Interpolation (Reading at Fractional Positions)

When you need a delay of 5.7 samples (for ITD simulation), you can't just read `buf_[5]` or `buf_[6]`. You need to interpolate between samples. Linear interpolation (2 taps) creates audible artifacts when the delay is modulated (doppler). Hermite/Catmull-Rom (4 taps) produces much smoother results:

```cpp
// FractionalDelayLine.h:76-96
float readHermite(float delayInSamples) const {
    int d   = static_cast<int>(delayInSamples);           // integer part
    float t = delayInSamples - static_cast<float>(d);     // fractional part

    // Invert fractional part for correct interpolation direction
    if (t > 0.0f) { t = 1.0f - t; d += 1; }

    int base = writePos_ - 1 - d;  // index of the nearest sample

    // Read 4 adjacent samples (A, B, C, D)
    float A = buf_[static_cast<size_t>((base - 1) & mask_)];
    float B = buf_[static_cast<size_t>((base    ) & mask_)];
    float C = buf_[static_cast<size_t>((base + 1) & mask_)];
    float D = buf_[static_cast<size_t>((base + 2) & mask_)];

    // Hermite polynomial coefficients
    float a = -0.5f*A + 1.5f*B - 1.5f*C + 0.5f*D;
    float b =        A - 2.5f*B + 2.0f*C - 0.5f*D;
    float c = -0.5f*A           + 0.5f*C;

    // Horner's method: evaluate a*t^3 + b*t^2 + c*t + B
    return ((a * t + b) * t + c) * t + B;
}
```

**The Horner's method evaluation** `((a*t + b)*t + c)*t + B` computes a cubic polynomial with only 3 multiplies and 3 adds, instead of computing `t*t`, `t*t*t` separately (which would take 5 multiplies). This is a standard optimization the interviewer will recognize.

**The bitmask handles negative indices safely:** When `base - 1` is negative (say, -3), the bitwise AND with mask (e.g., 1023) wraps it to 1021. This is because `-3 & 1023 = 1021` in two's complement arithmetic. No branch needed for wrapping.

**Interview talking point:** "I use 4-tap Hermite interpolation for modulated delay paths like ITD and doppler because linear interpolation creates audible low-pass filtering and pitch artifacts during modulation. For static delays like reverb output taps, linear interpolation is fine and cheaper."

### The Linear Interpolation Alternative

```cpp
// FractionalDelayLine.h:60-72
float readLinear(float delayInSamples) const {
    int d   = static_cast<int>(delayInSamples);
    float t = delayInSamples - static_cast<float>(d);
    if (t > 0.0f) { t = 1.0f - t; d += 1; }
    int base = writePos_ - 1 - d;

    float A = buf_[static_cast<size_t>((base    ) & mask_)];
    float B = buf_[static_cast<size_t>((base + 1) & mask_)];
    return A + t * (B - A);  // 1 multiply, 2 adds
}
```

Only 2 taps, ~3 FLOPs vs ~11 for Hermite. Used for fixed-delay paths (reverb allpass pre-delay, output taps) where the delay doesn't change per sample, so modulation artifacts aren't a concern.

---

## 6. Filter Design and Implementation

**Why this matters for Apple:** Filters are the building block of all audio processing. Expect questions about filter topologies, stability, and why you'd choose one over another.

### State Variable Filter (SVF) — The Modulation-Safe Filter

```cpp
// SVFLowPass.h:50-60
void setCoefficients(float cutoffHz, float sampleRate, float Q = 0.7071f) {
    float safeHz = std::min(cutoffHz, 0.45f * sampleRate);   // clamp near Nyquist
    if (std::abs(safeHz - lastCutoff_) < 2.0f) return;       // skip if change < 2Hz
    lastCutoff_ = safeHz;
    float g = SineLUT::fastTan(3.14159265f * safeHz / sampleRate);  // frequency warping
    float k = 1.0f / Q;
    a1_ = 1.0f / (1.0f + g * (g + k));
    a2_ = g * a1_;
    a3_ = g * a2_;
}

float process(float v0) {
    float v3 = v0 - ic2eq_;
    float v1 = a1_ * ic1eq_ + a2_ * v3;
    float v2 = ic2eq_ + a2_ * ic1eq_ + a3_ * v3;
    ic1eq_ = 2.0f * v1 - ic1eq_;
    ic2eq_ = 2.0f * v2 - ic2eq_;
    return v2;  // LP output
}
```

**Why SVF instead of biquad for modulated cutoffs?**

Traditional biquad filters (Direct Form I/II) store their state as delay elements (`z1_`, `z2_`). When you change coefficients abruptly, the state from the old coefficients is mismatched with the new ones, causing clicks and potentially instability.

The SVF in TPT (Topology-Preserving Transform) form stores state as integrator outputs (`ic1eq_`, `ic2eq_`) which have physical meaning (they represent capacitor voltages in the analog prototype). When coefficients change, the state remains valid because the topology is preserved. You can change the cutoff every single sample without clicks or instability.

**The 0.45 * sampleRate clamp:** The `tan()` function in the frequency warping formula approaches infinity as the cutoff approaches Nyquist (sampleRate/2). Clamping at 0.45 * sampleRate (90% of Nyquist) keeps `g` finite and the filter stable.

**The 2Hz delta check:** `if (std::abs(safeHz - lastCutoff_) < 2.0f) return;` — If the cutoff hasn't changed by more than 2Hz, skip the coefficient recalculation. The `fastTan()` call, while cheap, is still unnecessary when the cutoff is essentially unchanged. A 2Hz change at any audio frequency is imperceptible.

**Where used:** Head shadow (azimuth-dependent lowpass, simulates the head blocking high frequencies from one ear) and rear shadow (elevation-dependent).

### Biquad Filter — Per-Block Coefficient Updates with Smoothing

```cpp
// BiquadFilter.h:105-123
void setCoefficientsSmoothed(BiquadType type, float freqHz, float sampleRate,
                             float Q, float gainDb, int blockSize) {
    // Save current coefficients as "old"
    old_b0_ = b0_; old_b1_ = b1_; old_b2_ = b2_;
    old_a1_ = a1_; old_a2_ = a2_;

    // Compute new targets
    setCoefficients(type, freqHz, sampleRate, Q, gainDb);

    // Store new as targets, restore old as starting point
    new_b0_ = b0_; new_b1_ = b1_; new_b2_ = b2_;
    new_a1_ = a1_; new_a2_ = a2_;
    b0_ = old_b0_; b1_ = old_b1_; b2_ = old_b2_;
    a1_ = old_a1_; a2_ = old_a2_;

    smoothSamplesRemaining_ = blockSize;
    smoothInc_ = 1.0f / static_cast<float>(blockSize);
    smoothT_ = 0.0f;
}
```

This does something clever: it computes the old and new coefficients, then **linearly interpolates** between them across the block in `process()`:

```cpp
// BiquadFilter.h:130-143
float process(float x) {
    if (smoothSamplesRemaining_ > 0) {
        smoothT_ += smoothInc_;
        b0_ = old_b0_ + (new_b0_ - old_b0_) * smoothT_;  // lerp
        b1_ = old_b1_ + (new_b1_ - old_b1_) * smoothT_;
        b2_ = old_b2_ + (new_b2_ - old_b2_) * smoothT_;
        a1_ = old_a1_ + (new_a1_ - old_a1_) * smoothT_;
        a2_ = old_a2_ + (new_a2_ - old_a2_) * smoothT_;
        --smoothSamplesRemaining_;
    }
    float y = b0_ * x + z1_;
    z1_     = b1_ * x - a1_ * y + z2_;
    z2_     = b2_ * x - a2_ * y;
    return y;
}
```

**Why not use SVF for everything?** SVF only gives you lowpass/highpass/bandpass. The biquad supports PeakingEQ, HighShelf, and LowShelf — needed for pinna modeling and presence EQ. The tradeoff is that biquads need coefficient smoothing to avoid clicks, while SVFs don't.

**Where used:** Pinna notches/peaks (elevation-dependent EQ), presence shelf (front/back cue), ear canal resonance peak. These are the filters that make sounds "above" or "behind" you distinguishable.

### Feedback Comb Filter — Stability by Design

```cpp
// FeedbackCombFilter.h:55-57
void setFeedback(float g) {
    feedback_ = std::clamp(g, -0.95f, 0.95f);
}
```

`std::clamp(value, min, max)` returns `min` if value < min, `max` if value > max, otherwise value. It's equivalent to `std::min(std::max(value, min), max)` but more readable.

The hard clamp at ±0.95 is a **stability invariant**. A feedback comb filter has the transfer function `H(z) = 1/(1 - g*z^-M)`. When `|g| >= 1`, the filter diverges to infinity — the output grows without bound, producing a harsh digital explosion. By clamping inside `setFeedback()`, the filter is *structurally incapable* of instability, regardless of what the caller passes in.

**Interview talking point:** "Safety constraints belong inside the component, not at the call site. If stability depends on a range check, the component should enforce it — the caller shouldn't have to remember."

---

## 7. Parameter Smoothing

**Why this matters for Apple:** Parameter smoothing is essential for any audio plugin. Abrupt parameter changes cause clicks. Interviewers want to know you understand the tradeoff between smoothing speed and responsiveness.

### The One-Pole Exponential Smoother

```cpp
// OnePoleSmooth.h:36-58
void prepare(float smoothingMs, float sampleRate) {
    a_ = std::exp(-6.28318530f / (smoothingMs * 0.001f * sampleRate));
    b_ = 1.0f - a_;
}

float process(float target) {
    z_ = target * b_ + z_ * a_;
    return z_;
}

void converge(float target, int numSamples) {
    z_ = target + (z_ - target) * std::pow(a_, static_cast<float>(numSamples));
}
```

**What this is:** A digital RC circuit. When you set a new target, the output exponentially approaches it. `a_` is the pole (how much of the old value to keep), `b_ = 1-a` is how much of the new target to add. Higher `a_` = slower smoothing.

**The `converge()` trick:** Instead of calling `process()` N times in a loop (O(N)), this analytically computes where the smoother would be after N iterations in O(1). The math: after N iterations, the error from target decays by `a^N`. So: `z = target + (z - target) * a^N`.

This is used in `processBlock` for parameters that only need block-rate smoothing (not per-sample):

```cpp
// PluginProcessor.cpp
xSmooth_.converge(xParam->load(), buffer.getNumSamples());
const float smoothedX = xSmooth_.current();
```

One call per block, O(1), instead of looping through all samples. The `std::pow()` call here is acceptable because it's in the block preamble, not the per-sample loop.

### Different Smoothing Strategies for Different Purposes

| Smoother | Time Constant | Strategy | Why |
|----------|--------------|----------|-----|
| Position XYZ | 5ms | Block-rate `converge()` | Fast enough to track mouse drag, slow enough to prevent zipper |
| R scale | 20ms | Block-rate `converge()` | Slower to prevent automation clicks on size changes |
| LFO depth | 20ms | Per-sample `process()` | Per-sample avoids staircase artifacts when knob is dragged |
| Biquad coefficients | 1 block | Per-sample linear interpolation | Prevents clicks at block boundaries |
| SVF cutoff | Instant | No smoothing needed | TPT topology handles instant changes safely |
| Listener rotation | Variable | Angular (cos/sin pair) IIR | Handles 360-to-0 wrapping correctly |

### Angular Smoothing (Listener Rotation)

Regular linear smoothing breaks at angle wraparound. If the listener rotates from 359 to 1 degrees, linear interpolation goes through 180 — wrong direction. XYZPan solves this by smoothing in the **sin/cos domain**:

```cpp
// Engine.h:206-208
float yawSmCos_ = 1.0f, yawSmSin_ = 0.0f;  // unit-circle representation
```

Instead of smoothing the angle directly, it smooths `cos(angle)` and `sin(angle)` independently. Then reconstructs the angle with `atan2`. The sin/cos representation naturally takes the shortest path around the circle.

---

## 8. Performance Optimization

**Why this matters for Apple:** Audio processing has a hard real-time budget. Apple engineers need to know you can write efficient DSP code.

### Sine/Cosine Lookup Table

```cpp
// SineLUT.h:9-19
class SineLUT {
public:
    static constexpr int kSize = 2048;

    static float lookup(float phase01) {
        const float idx = phase01 * kSize;
        const int i0 = static_cast<int>(idx) & (kSize - 1);
        const int i1 = (i0 + 1) & (kSize - 1);
        const float frac = idx - static_cast<float>(static_cast<int>(idx));
        return table_[i0] + frac * (table_[i1] - table_[i0]);  // linear interpolation
    }
```

**What `static constexpr` means:** `static` means the value belongs to the class, not any instance. `constexpr` means it's computed at compile time. `static constexpr int kSize = 2048` is a compile-time constant — the compiler replaces every use of `kSize` with 2048. No memory read needed at runtime.

**The table initialization:**
```cpp
// SineLUT.h:55-60
static inline const std::array<float, kSize> table_ = []() {
    std::array<float, kSize> t{};
    for (int i = 0; i < kSize; ++i)
        t[i] = std::sin(2.0f * 3.14159265358979f * static_cast<float>(i) / kSize);
    return t;
}();
```

This is an **immediately invoked lambda expression (IILE)**. The `[]() { ... }()` syntax defines a lambda function and immediately calls it. The result initializes `table_`. This runs once at program startup, before any audio processing. `static inline` ensures there's only one copy of the table across all translation units.

2048 floats = 8KB — fits in L1 cache on all modern CPUs. Each lookup needs 1 multiply, 1 integer truncation, 2 table reads, and 1 lerp. Much faster than `std::sin()` which takes 20-80 cycles.

### Fast Tangent Approximation

```cpp
// SineLUT.h:48-52
static float fastTan(float x) {
    const float x2 = x * x;
    return x * (1.0f + x2 * (1.0f / 3.0f + x2 * (2.0f / 15.0f)))
             / (1.0f - x2 * (1.0f / 3.0f));
}
```

This is a **Pade [3/2] rational approximation** — a ratio of polynomials. It's more accurate than a Taylor series of the same order because it models the pole at pi/2 better. Accurate to ~0.002% for inputs up to 1.414 (which is pi*0.45 — the SVF's Nyquist clamp). ~3-5x faster than `std::tan()`.

**Used by:** SVF coefficient computation (`g = tan(pi * f / sr)`), which runs every time the cutoff changes. Since the SVF cutoff can change per-sample (head shadow tracks source position), this needs to be fast.

### Fast Square Root

```cpp
// FastMath.h:14-22
static inline float fastSqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    float y = x;
    int i;
    std::memcpy(&i, &y, sizeof(i));      // reinterpret float bits as int
    i = 0x1fbd1df5 + (i >> 1);           // bit-hack initial guess
    std::memcpy(&y, &i, sizeof(y));      // reinterpret back to float
    return 0.5f * (y + x / y);           // one Newton-Raphson iteration
}
```

**How this works:**

IEEE 754 floats store the exponent in bits 23-30. Shifting right by 1 (`>> 1`) approximately halves the exponent, which approximates square root. The magic constant `0x1fbd1df5` corrects the mantissa. This gives a rough estimate (~5% error), then one Newton-Raphson iteration (`0.5 * (y + x/y)`) refines it to ~0.1% error.

**Why `std::memcpy` instead of pointer cast?** In C++, `*(int*)&y` (type-punning via pointer cast) is **undefined behavior** due to strict aliasing rules. The compiler assumes pointers of different types don't alias, and may optimize away the read. `std::memcpy` is the defined way to reinterpret bits — the compiler recognizes this pattern and optimizes it to a single register move (no actual memory copy).

**Used by:** Distance calculations in the per-sample loop. Every sample computes `sqrt(x^2 + y^2 + z^2)` for source distance. `std::sqrt` would be ~15 cycles; this is ~5 cycles.

### Block-Constant Caching

```cpp
// Engine.cpp — block preamble (runs once)
const float chestGainLin = std::pow(10.0f, currentParams.chestGainDb / 20.0f);

// Engine.cpp — per-sample loop (runs numSamples times)
// Uses chestGainLin directly — no std::pow call
float chestOut = chestSample * chestGainLin;
```

`const float` in a local scope means the variable is immutable after initialization. The compiler can keep it in a register for the entire loop instead of reloading from memory.

**Interview talking point:** "I hoist all transcendental function calls out of the per-sample loop. Inside the loop, only arithmetic operations and table lookups."

---

## 9. C++ Language Features Used Throughout

### Namespaces

```cpp
namespace xyzpan::dsp {  // nested namespace (C++17)
    class FractionalDelayLine { ... };
}
```

`xyzpan::dsp` is a **nested namespace**. Without namespaces, every class name is global — two libraries with a `Filter` class would clash. Namespaces scope names: `xyzpan::dsp::FractionalDelayLine` won't conflict with any other library's `FractionalDelayLine`.

The `::` is the **scope resolution operator**. `xyzpan::dsp::FractionalDelayLine` means "the FractionalDelayLine inside dsp, inside xyzpan."

### `#pragma once`

```cpp
#pragma once  // first line of every header
```

Prevents the header from being included twice in the same translation unit. Without this, if two .cpp files both include `FractionalDelayLine.h`, you'd get duplicate class definitions. `#pragma once` is technically non-standard but supported by every compiler and is simpler than the traditional `#ifndef`/`#define` include guard pattern.

### Const Correctness

```cpp
// FractionalDelayLine.h:52
float read(float delayInSamples) const { ... }
```

The `const` after the parameter list means "this method does not modify the object." It's a compile-time contract. If you accidentally write to a member variable inside a `const` method, the compiler rejects it.

```cpp
// PositionBridge.h:48
[[nodiscard]] SourcePositionSnapshot read() const { ... }
```

`[[nodiscard]]` is a C++17 attribute that warns if the return value is discarded. Calling `bridge.read();` without using the result is almost certainly a bug — the compiler will flag it.

### Enum Classes (Scoped Enumerations)

```cpp
// BiquadFilter.h:24
enum class BiquadType { PeakingEQ, HighShelf, LowShelf };

// LFO.h:7
enum class LFOWaveform { Sine = 0, Triangle, Saw, RampDown, Square, SampleHold };
```

`enum class` (C++11) creates a **scoped, strongly-typed enumeration**. Unlike plain `enum`:
- Values are scoped: you must write `BiquadType::PeakingEQ`, not just `PeakingEQ`
- They don't implicitly convert to integers: `if (type == 0)` won't compile — you must compare to `BiquadType::PeakingEQ`
- They can't accidentally collide with values from other enums

### Inline and Static Inline

```cpp
// FastMath.h:14
static inline float fastSqrt(float x) { ... }
```

`inline` suggests the compiler replace calls with the function body (inlining). For small functions called in tight loops, this eliminates function call overhead (pushing/popping the stack, jumping to the function address). `static` in a free function means "internal linkage" — each translation unit gets its own copy, avoiding linker errors if the header is included by multiple .cpp files.

For the inner-loop functions like `fastSqrt()`, `process()`, and `read()`, the compiler almost certainly inlines them (they're defined in headers, small, and performance-critical). The `inline` keyword is more of a hint — modern compilers make their own inlining decisions.

### `static_cast` vs C-Style Casts

```cpp
static_cast<size_t>(writePos_ & mask_)   // C++ style
(size_t)(writePos_ & mask_)              // C style — NOT used
```

`static_cast` is explicit about what kind of conversion you want. It only allows "sensible" conversions (int to float, int to size_t, derived-to-base). A C-style cast `(size_t)x` will try multiple cast types silently, including dangerous ones like `reinterpret_cast`. Using `static_cast` makes intent clear and catches bugs at compile time.

### `std::clamp`, `std::min`, `std::max`

```cpp
std::clamp(g, -0.95f, 0.95f);        // ensure value is in [-0.95, 0.95]
std::min(cutoffHz, 0.45f * sampleRate); // cap cutoff below Nyquist
std::max(rawModDist, kMinDistance);      // enforce minimum distance
```

These are `<algorithm>` utilities. `std::clamp` (C++17) combines min and max into one call. They're branchless on most compilers (using conditional moves), making them efficient in tight loops.

### Deleted Special Member Functions

```cpp
// Engine.h:49-50
XYZPanEngine(const XYZPanEngine&) = delete;
XYZPanEngine& operator=(const XYZPanEngine&) = delete;
```

C++ auto-generates copy constructors and copy assignment operators by default. `= delete` explicitly forbids them. The engine contains unique state (delay line write positions, filter memory) that shouldn't be duplicated. This is a compile-time safety net — any attempt to copy the engine fails at compilation, not at runtime.

### Default Constructors/Destructors

```cpp
// Engine.h:45-46
XYZPanEngine() = default;
~XYZPanEngine() = default;
```

`= default` tells the compiler to generate the default version. For the constructor, this means "zero-initialize all members that have default member initializers, and default-construct all class members." For the destructor, it means "destroy all members in reverse declaration order." Since all members are either value types or vectors (which clean up after themselves via RAII), no custom destructor logic is needed.

---

## 10. Multi-Instance Architecture

**Why this matters for Apple:** Apple Spatial Audio involves coordination between multiple audio processing nodes. Understanding multi-instance synchronization is directly relevant.

### SharedListenerHub — Process-Wide Singleton

When multiple XYZPan instances run in the same DAW, they can link their listener positions so all instances share the same virtual head orientation. This is managed by `SharedListenerHub`:

```cpp
// SharedListenerHub.h
class SharedListenerHub {
    mutable juce::SpinLock spinLock_;
    std::vector<Listener*> linked_;
    Listener* pilot_ = nullptr;

    // Lock-free shared atomics
    std::atomic<float> sharedYaw{0.0f};
    std::atomic<float> sharedPitch{0.0f};
    std::atomic<float> sharedRoll{0.0f};
};
```

**The `mutable` keyword:** `mutable juce::SpinLock spinLock_` means the lock can be acquired even in `const` methods. Without `mutable`, a `const` method couldn't lock the spinlock (locking mutates the lock's internal state). This is appropriate because locking is an implementation detail of thread safety, not a logical mutation of the object.

### The Pilot Pattern

Only one instance (the "pilot") broadcasts orientation changes. Others receive.

```
Instance A (pilot) ──broadcast──▶ SharedListenerHub
                                       │
Instance B (listener) ◀──callback──────┤
Instance C (listener) ◀──callback──────┘
```

The `receivingBroadcast_` flag prevents infinite loops:

```cpp
// PluginProcessor.cpp — receiving a broadcast
void listenerOrientationChanged(float yaw, float pitch, float roll, bool headFollows) {
    receivingBroadcast_->store(true);   // "I'm receiving, don't re-broadcast"
    // ... update local parameters ...
    receivingBroadcast_->store(false);
}

// PluginProcessor.cpp — parameter change handler
void parameterChanged(const juce::String& id, float value) {
    if (receivingBroadcast_->load()) return;  // skip — this change came from a broadcast
    // ... broadcast to other instances ...
}
```

Without this guard, changing the yaw on instance A would broadcast to B, which would update its parameter, which would trigger `parameterChanged`, which would broadcast back to A, creating an infinite loop.

### Thread Safety of Hub Operations

The hub uses a spinlock for its linked-instance list but does all callbacks **outside** the lock:

```cpp
// Pseudocode pattern used throughout SharedListenerHub
void broadcastOrientation(...) {
    std::vector<Listener*> snapshot;
    {
        juce::SpinLock::ScopedLockType lock(spinLock_);
        snapshot = linked_;  // copy the list under lock
    }
    // Callbacks run OUTSIDE the lock
    for (auto* listener : snapshot) {
        if (listener->hubAlive_.load())
            listener->listenerOrientationChanged(yaw, pitch, roll, headFollows);
    }
}
```

**Why snapshot-then-iterate?** The callbacks might call back into the hub (e.g., to check pilot status). If the lock were held during callbacks, this would deadlock — the callback tries to acquire the lock that's already held by the same thread.

The `hubAlive_` flag is an `std::atomic<bool>` that each listener sets to `false` before detaching. This prevents calling virtual methods on a half-destroyed object — a common source of crashes in multi-instance plugins.

---

## 11. OpenGL and the Render Thread

**Why this matters for Apple:** Apple's Metal/GL rendering and audio rendering share similar real-time constraints and thread isolation patterns.

### Thread Isolation

The GL thread never touches audio state directly. All data flows through the `PositionBridge` double buffer (covered in Section 2). The GL thread calls `bridge.read()` to get the latest snapshot of source position, listener orientation, and RMS levels.

### GLSL Shaders

The rendering uses OpenGL 3.2 Core Profile with GLSL `#version 150 core`. Key shader patterns:

```glsl
// Sphere shader — diffuse + ambient lighting
float diff = max(dot(worldNormal, lightDir), 0.0);
float light = ambient + (1.0 - ambient) * diff;
// Premultiplied alpha for blend modes
outColor = vec4(rgb * a, a);
```

**Premultiplied alpha** multiplies RGB by alpha before blending. This avoids dark fringe artifacts when blending transparent objects and is required by some compositors. It's the same technique Apple uses in Core Animation.

### Procedural Shaders

The sky and ground are procedurally generated in fragment shaders (no texture files):

- **Perlin noise** for terrain height and surface detail
- **FBM (Fractional Brownian Motion)** — layered noise at decreasing scales
- **Voronoi** — cell-based patterns
- **Hash functions** for deterministic pseudorandom noise in shaders

All noise is computed from math (sin/fract/dot), not texture lookups. This means zero texture memory and infinite resolution.

---

## Quick Reference: Key Files

| What | File | What to Know |
|------|------|-------------|
| Engine entry point | `engine/src/Engine.cpp:252` | `process()` — block preamble + per-sample loop |
| Engine header | `engine/include/xyzpan/Engine.h` | Pipeline composition, all member types |
| Ring buffer + Hermite | `engine/include/xyzpan/dsp/FractionalDelayLine.h` | Power-of-2 bitmask, 4-tap interpolation |
| SVF filter (modulation-safe) | `engine/include/xyzpan/dsp/SVFLowPass.h` | TPT topology, per-sample safe |
| Biquad filter (EQ) | `engine/include/xyzpan/dsp/BiquadFilter.h` | Coefficient smoothing, Direct Form II |
| Parameter smoother | `engine/include/xyzpan/dsp/OnePoleSmooth.h` | O(1) `converge()`, exponential IIR |
| Fast math | `engine/include/xyzpan/dsp/FastMath.h` | Bit-hack sqrt, Newton-Raphson |
| Sine LUT + fast tan | `engine/include/xyzpan/dsp/SineLUT.h` | 2048-entry LUT, Pade approximation |
| Comb filter | `engine/include/xyzpan/dsp/FeedbackCombFilter.h` | Stability clamp, bitmask ring buffer |
| Lock-free double buffer | `ui/PositionBridge.h` | acquire/release semantics |
| Plugin processor | `plugin/PluginProcessor.h` | Atomic parameter caching, APVTS |
| Linked instances | `plugin/SharedListenerHub.h` | Spinlock, pilot pattern, broadcast guard |
| Shaders | `ui/Shaders.h` | GLSL 150 core, procedural generation |

---

## Interview Framing Suggestions

When discussing XYZPan in an interview, frame your answers around **decisions and tradeoffs**, not just descriptions:

- "I chose SVF over biquad for the head shadow filter because the cutoff modulates per-sample as the source moves, and SVFs remain stable under modulation while biquads need coefficient smoothing."

- "The ring buffer uses power-of-2 sizing so wrap-around is a single AND instruction instead of a modulo division — that matters when you're doing 4 buffer reads per sample for Hermite interpolation."

- "I use a lock-free double buffer for audio-to-GL communication because the audio thread can't afford to wait for the GL thread's vsync, and a mutex would introduce priority inversion."

- "All parameter reads on the audio thread go through cached atomic pointers rather than APVTS tree lookups, because tree traversal involves string comparison which is both slow and involves potential allocation."

- "The engine has zero JUCE dependency — it's pure C++ with raw float pointers. This means it can be tested in isolation, ported to AudioUnit, or integrated into a non-JUCE host without rewriting DSP code."
