# XYZPan Architecture

XYZPan is a 3D spatial audio panner plugin. It takes a mono or stereo input and positions it in a virtual 3D space around the listener using binaural processing (HRTF-inspired ITD/ILD/spectral cues), distance modeling, and a reverb tail. Output is always stereo headphone-binaural.

**Formats:** VST3, Standalone
**Stack:** C++20, JUCE 8.0.12, OpenGL 3.2 Core, GLM 1.0.1
**Platform:** Windows (v1)

---

## Directory Structure

```
xyzpan/
├── engine/                    # Pure C++ DSP — zero JUCE dependency
│   ├── include/xyzpan/
│   │   ├── Engine.h           # Main engine API
│   │   ├── Types.h            # EngineParams, SphericalCoord, enums
│   │   ├── Constants.h        # All tuning constants
│   │   ├── Coordinates.h      # XYZ → spherical conversion
│   │   ├── DSPStateBridge.h   # Lock-free audio→UI telemetry bridge
│   │   └── dsp/
│   │       ├── LFO.h                # 6-waveform phase-accumulator LFO
│   │       ├── FractionalDelayLine.h # Cubic Hermite interpolated delay
│   │       ├── SVFLowPass.h          # TPT SVF lowpass (head/rear shadow)
│   │       ├── SVFFilter.h           # TPT SVF LP/HP/BP/Notch (chest bounce)
│   │       ├── BiquadFilter.h        # Audio EQ Cookbook biquad (pinna EQ)
│   │       ├── FeedbackCombFilter.h  # IIR feedback comb (depth cues)
│   │       ├── OnePoleSmooth.h       # Parameter smoother (ms-based)
│   │       ├── OnePoleLP.h           # 6 dB/oct lowpass (Hz-based)
│   │       ├── FDNReverb.h           # 4-delay feedback delay network
│   │       └── SineLUT.h             # 2048-entry sine table + linear interp
│   └── src/
│       ├── Engine.cpp         # ~1500 lines — full signal chain
│       ├── LFO.cpp            # LFO tick/reset/smoothing
│       ├── FDNReverb.cpp      # FDN prepare/process/decay/damping
│       └── Coordinates.cpp    # toSpherical(), computeDistance()
│
├── plugin/                    # JUCE integration layer
│   ├── PluginProcessor.h/cpp  # AudioProcessor, APVTS, param→engine bridge
│   ├── PluginEditor.h/cpp     # Editor layout, component wiring, attachments
│   ├── ParamIDs.h             # All parameter ID strings (constexpr)
│   ├── ParamLayout.h/cpp      # createParameterLayout() — ~80 parameters
│   └── CMakeLists.txt
│
├── ui/                        # OpenGL renderer + custom components
│   ├── XYZPanGLView.h/cpp     # 3D scene: room, grid, nodes, trails, interaction
│   ├── Camera.h/cpp           # Orbit camera with snap presets
│   ├── Mesh.h/cpp             # Geometry builders (sphere, cone, room, grid)
│   ├── Shaders.h              # GLSL 150 shaders (line, sphere, trail)
│   ├── PositionBridge.h       # Lock-free double-buffer (audio→GL)
│   ├── AlchemyLookAndFeel.h/cpp # Dark gold/bronze theme
│   ├── LFOStrip.h/cpp         # Per-LFO control group (shape+display+knobs)
│   ├── LFOWaveformDisplay.h/cpp # Live scrolling waveform at 30fps
│   ├── LFOShapeSelector.h/cpp # 6-button waveform shape picker
│   ├── DevPanelComponent.h/cpp # Scrollable dev tuning overlay
│   └── CMakeLists.txt
│
├── tests/
│   ├── engine/                # Engine-only tests (no JUCE linked)
│   │   ├── TestCoordinates.cpp
│   │   ├── TestBinauralPanning.cpp
│   │   ├── TestDepthAndElevation.cpp
│   │   ├── TestDistanceProcessing.cpp
│   │   ├── TestCreativeTools.cpp
│   │   ├── TestSampleRateAdaptation.cpp
│   │   ├── TestPerformance.cpp
│   │   └── TestSineLUT.cpp
│   └── plugin/                # Plugin tests (JUCE required)
│       ├── TestParameterLayout.cpp
│       ├── TestPositionBridge.cpp
│       └── TestPresets.cpp
│
├── cmake/CPM.cmake            # CMake Package Manager
└── CMakeLists.txt             # Top-level: C++20, CPM deps, subdirectories
```

---

## Layer Architecture

```
┌─────────────────────────────────────────────┐
│  Plugin (JUCE AudioProcessor + Editor)      │
│  - APVTS parameter system                   │
│  - processBlock: reads params → engine      │
│  - Lock-free bridges for UI readback        │
├──────────────┬──────────────────────────────┤
│  UI          │  Engine                      │
│  - OpenGL 3D │  - Pure C++ DSP              │
│  - Alchemy   │  - No JUCE headers           │
│    theme     │  - EngineParams struct in     │
│  - LFO/dev   │  - Audio buffers out         │
│    panels    │                              │
└──────────────┴──────────────────────────────┘
```

The engine is a standalone static library with **zero JUCE dependency**. It accepts a plain `EngineParams` struct and raw float buffers. This separation means:
- Engine tests compile and run without JUCE
- The DSP can be reused outside JUCE
- No `juce::` types cross the engine boundary

The plugin layer bridges JUCE's APVTS parameter system to the engine. The UI layer handles OpenGL rendering and custom components, reading position/phase data from the audio thread via lock-free bridges.

---

## Engine — DSP Layer

### API

```cpp
void prepare(double sampleRate, int maxBlockSize);
void setParams(const EngineParams& params);  // once per block
void process(const float* const* inputs, int numInputChannels,
             float* outL, float* outR,
             float* auxL, float* auxR,    // nullable
             int numSamples);
void reset();
```

All buffers are pre-allocated in `prepare()`. No heap allocation in `process()`.

### Signal Chain

Per-block preamble — all transcendental math (`cos`, `sin`, `pow`, `tan`, `exp`) runs here via `setCoefficients()` calls. Per-sample processing uses only arithmetic and table lookups.

Per-sample flow:

1. **Position LFO modulation** — `lfoX/Y/Z.tick()` advance per sample. `modPos = base + lfo * depth`. Zero-LFO fast path skips `sqrt`/trig when all depths are zero.

2. **Stereo width decision** — when `width > 0.001` and stereo input exists, the stereo path runs. Otherwise mono.

3. **Stereo path** (when active):
   - Compute L/R node positions: half-spread perpendicular to listener→source in XY plane
   - Apply orbit LFO rotations in XY/XZ/YZ planes (trig via SineLUT, not `std::cos/sin`)
   - Run full binaural + distance pipeline independently for L input→L-node and R input→R-node
   - Sum: `outL = distL.left + distR.left`, `outR = distL.right + distR.right`

4. **Binaural pipeline** (`processBinauralForSource()`):
   - Vertical mono cylinder — smoothstep blend to mono near Z-axis
   - 10-comb series bank — wet scales with rear-ness (Y < 0)
   - Pinna EQ chain — presenceShelf → earCanalPeak → P1 → N1 → N2 → pinnaShelf
   - ITD — fractional delay, max ~0.66 ms, signed by azimuth
   - ILD — far-ear gain attenuation, up to -8 dB
   - Near-field ILD — low-shelf boost on ipsilateral ear at close range
   - Head shadow — SVF lowpass on far ear, 16 kHz → 1.2 kHz
   - Rear shadow — SVF lowpass on both ears, 22 kHz → 4 kHz

5. **Chest bounce** (shared, post-binaural) — 4x SVF HP cascade → one-pole LP → fractional delay (0–2 ms). Z-modulated gain.

6. **Floor bounce** (shared, post-binaural) — stereo delay lines (0–20 ms), one-pole LP at 5 kHz. Z-modulated gain.

7. **Distance processing**:
   - **Gain:** inverse-square law. Up to +6 dB boost at close range, -72 dB at sphere boundary.
   - **Delay:** fractional delay line up to 300 ms, smoothed at 150 ms time constant.
   - **Doppler:** rate-of-change clamped to ±15% pitch shift.
   - **Air absorption:** two cascaded one-pole LPs (12 dB/oct effective). HF rolloff increases with distance.

8. **FDN Reverb** — 4-delay feedback delay network:
   - Householder feedback matrix (energy-preserving)
   - One-pole damping per loop
   - Distance-scaled pre-delay
   - T60 up to 5 seconds

9. **Output clamp** — `[-2, +2]`

### DSP Building Blocks

| Class | Algorithm | Used For |
|-------|-----------|----------|
| `SVFLowPass` | Cytomic TPT SVF, LP-only | Head shadow, rear shadow |
| `SVFFilter` | TPT SVF, LP/HP/BP/Notch | Chest bounce 4x HP cascade |
| `BiquadFilter` | Audio EQ Cookbook, Direct Form II | Pinna EQ (peaking, shelf), near-field ILD |
| `FeedbackCombFilter` | IIR feedback comb, integer delay | 10-filter depth/front-back coloration bank |
| `FractionalDelayLine` | Cubic Hermite (Catmull-Rom) interpolation, power-of-2 ring buffer | ITD, distance delay, chest/floor bounce, pre-delay |
| `OnePoleLP` | `y = (1-a)*x + a*y`, `a = exp(-2πf/sr)` | Air absorption, chest/floor LP |
| `OnePoleSmooth` | Same topology, ms-parameterized | All parameter transitions |
| `FDNReverb` | 4-delay FDN, Householder matrix, one-pole damping | Reverb tail |
| `SineLUT` | 2048-entry table, linear interpolation | LFO sine waveform, orbit rotation trig |

### LFO System

6 waveforms: Sine (via SineLUT), Triangle, Saw, RampDown, Square, Sample & Hold (xorshift32 RNG).

Phase accumulator wraps at 1.0. Output range [-1, 1]. Optional one-pole output smoother (0–300 ms). Tempo sync via host BPM with 11 beat divisions (1/16 to 4 bars). Global speed multiplier (0–2×). Phase offset applied without discontinuity.

6 LFO instances: 3 position axes (X, Y, Z) + 3 stereo orbit planes (XY, XZ, YZ).

### Coordinate System

Y-forward convention: `azimuth = atan2(X, Y)`, `elevation = atan2(Z, sqrt(X² + Y²))`. Distance floored at 0.1. LFO modulation can push coordinates beyond [-1, 1].

---

## Plugin Layer

### Parameter System

~80 parameters registered via `createParameterLayout()`, stored as `AudioProcessorValueTreeState` with state tag `"XYZPanState"`. All parameter IDs defined as `constexpr const char*` in `ParamIDs.h`.

**Parameter groups:**

| Group | Examples | Notes |
|-------|----------|-------|
| Spatial position | `x`, `y`, `z`, `r` | Y defaults to 1.0 (front) |
| XYZ LFOs | `lfo_x_rate`, `_depth`, `_phase`, `_waveform`, `_smooth`, `_beat_div` | Per-axis, 6 waveforms |
| Stereo | `stereo_width`, `stereo_face_listener`, `stereo_orbit_phase/offset` | Width 0 = mono fallback |
| Orbit LFOs | `stereo_orbit_xy_*`, `_xz_*`, `_yz_*` | Per-plane, independent sync |
| Reverb | `verb_size`, `verb_decay`, `verb_damping`, `verb_wet` | Exposed in bottom row |
| Binaural tuning | `itd_max_ms`, `head_shadow_hz`, `ild_max_db`, ... | Dev panel only |
| Distance tuning | `dist_delay_max_ms`, `air_abs_max_hz`, ... | Dev panel only |
| Comb bank | `comb_delay_0..9`, `comb_fb_0..9` | Dev panel only |

**How parameters reach the engine:**

Every `processBlock`:
1. All `std::atomic<float>*` raw parameter pointers (bound at construction) are `.load()`-ed into a local `EngineParams` struct — no mutex, no APVTS listener on the audio thread.
2. The `r` (radius) parameter is smoothed via `OnePoleSmooth` then multiplied into `x/y/z`.
3. Waveform floats → `int` via `std::round()`. Beat div choice index → float multiplier via lookup table. Bools → `>= 0.5f`.
4. Host BPM read from playhead.
5. `engine.setParams(params)` then `engine.process(...)`.

### Thread-Safe Bridges

| Bridge | Pattern | Writer | Reader | Data |
|--------|---------|--------|--------|------|
| `PositionBridge` | Lock-free double-buffer | Audio thread (end of `processBlock`) | GL thread (`renderOpenGL`) | x, y, z, distance, L/R node positions, stereoWidth, sphereRadius |
| `DSPStateBridge` | Lock-free double-buffer | Audio thread | Message thread (DevPanel timer) | ITD, shadow cutoff, ILD gain, comb wet, distance delay/gain, air cutoff |
| LFO phase atomics | 12× `std::atomic<float>`, relaxed | Audio thread | Message thread (LFOWaveformDisplay timer) | Phase + S&H value per LFO |

Write protocol: write to `buf_[1 - writeIdx]`, then `writeIdx.store(newIdx, release)`.
Read protocol: read `buf_[writeIdx.load(acquire)]` — always gets last complete snapshot.

### Bus Layout

Stereo input (accepts mono), stereo output. Tail length 5.37 seconds (300 ms distance delay + 20 ms floor bounce + 5000 ms reverb T60 + 50 ms pre-delay).

### State Persistence

Standard APVTS XML serialization via `copyState()` / `replaceState()`. No custom versioning.

---

## UI Layer

### OpenGL 3D View (`XYZPanGLView`)

GL 3.2 Core profile. Continuous repainting. All geometry uploaded once in `newOpenGLContextCreated()`.

**Coordinate mapping:** XYZPan X → GL X, XYZPan Y → GL -Z, XYZPan Z → GL +Y.

**Rendered each frame (in order):**
1. Bronze room wireframe (scaled by R parameter)
2. Dark-earth floor grid
3. Listener sphere at origin (warm gold) + forward cone + ear ellipsoids
4. Semi-transparent audible radius sphere (8% opacity)
5. Source node — bright gold in mono, ghost at 10% when stereo active
6. L node (pink `#FF6B9D`) + R node (blue `#6B9DFF`) when stereo active
7. Fading position trails (48-point circular buffer, 1.6s lifetime)

**Shaders** (GLSL 150, inline in `Shaders.h`):

| Shader | Used For |
|--------|----------|
| Line | Room wireframe, floor grid |
| Sphere | All solid objects (diffuse + ambient lighting) |
| Trail | Fading position trails with per-vertex alpha |

**Interaction:**
- Drag source node: hit-test against projected position (12px radius), drag delta computed from camera view matrix columns. Writes to APVTS via `MessageManager::callAsync`.
- Drag background: orbit camera rotation.
- Scroll: zoom camera distance [1.0, 10.0].
- Snap buttons: TopDown, Side, Front presets + free Orbit.

### Alchemy Theme (`AlchemyLookAndFeel`)

Dark warm palette extending `LookAndFeel_V4`:

| Color | Hex | Role |
|-------|-----|------|
| Background | `#1A1108` | Window, GL clear |
| Dark Iron | `#2A1A08` | Knob fill, slider track |
| Bronze | `#8B5E2E` | Borders, wireframe |
| Warm Gold | `#C8A86B` | Arcs, waveform lines |
| Parchment | `#D4B483` | Text, labels |
| Bright Gold | `#E8C46A` | Source node, highlights |
| Stereo L | `#FF6B9D` | Pink — L node |
| Stereo R | `#6B9DFF` | Blue — R node |

Custom drawing for rotary sliders (arc + thumb dot), linear sliders (hero variant with glow), buttons (dark iron fill, bronze border), and labels.

### Custom Components

**LFOStrip** — Self-contained control group per LFO: shape selector → waveform display → depth/smooth knobs → sync toggle + rate/phase knobs. Rate knob swaps to a beat-division combo box when sync is enabled. Two constructor forms for axis LFOs (shared sync) and orbit LFOs (per-plane sync).

**LFOWaveformDisplay** — 30fps timer-driven scrolling waveform. Reads real LFO phase from audio-thread atomics (drift-free). Draws ~2 cycles with one-pole smoothing visualization and a bright-gold dot at current output position.

**LFOShapeSelector** — Row of 6 mini buttons, each drawing its waveform polyline. Bound to APVTS parameter; responds to host automation.

**DevPanelComponent** — Scrollable overlay with collapsible sections for all dev-tuning parameters. Drag-to-resize left edge. Timer-driven live DSP readout from `DSPStateBridge`.

---

## Build System

### Dependencies (via CPM)

| Package | Version | Purpose |
|---------|---------|---------|
| JUCE | 8.0.12 | Audio plugin framework, GUI, OpenGL |
| GLM | 1.0.1 | Math library (view/projection matrices) |
| Catch2 | 3.7.1 | Testing framework |

### CMake Targets

| Target | Type | Links To | Description |
|--------|------|----------|-------------|
| `xyzpan_engine` | Static lib | (nothing — pure C++) | DSP engine. MSVC: `/O2 /arch:AVX2 /fp:fast` |
| `xyzpan_ui` | Static lib | juce_opengl, juce_audio_processors, juce_gui_basics, glm | OpenGL view, theme, components |
| `XYZPan` | Plugin | xyzpan_engine, xyzpan_ui, juce_audio_utils, juce_dsp, juce_opengl | VST3 + Standalone |
| `XYZPanTests` | Executable | xyzpan_engine, Catch2 | Engine tests — no JUCE linked |
| `XYZPanPluginTests` | Executable | XYZPan_SharedCode, xyzpan_ui, xyzpan_engine, Catch2 | Plugin tests — JUCE required |

Notable flags: `JUCE_DIRECT2D=0` (prevents D2D/OpenGL conflict on Windows NVIDIA).

---

## Data Flow

```
  UI Thread                    Audio Thread                    GL Thread
  ─────────                    ────────────                    ─────────
  APVTS SliderAttachment       processBlock():
  writes parameter value  ───→   atomic<float>* .load()
                                 ↓
                                 build EngineParams struct
                                 ↓
                                 engine.setParams(params)
                                 engine.process(in, out)
                                 ↓
                                 positionBridge.write(snap) ──────→ positionBridge.read()
                                 dspStateBridge.write(dsp) ──→       → node positions
                                 lfoPhase*.store(phase)    ──→       → sphere rendering
                                       │                             → trail updates
                                       │
  DevPanel timer reads ←───────────────┤
  DSP telemetry                        │
                                       │
  LFOWaveformDisplay timer ←───────────┘
  reads LFO phases
```

Parameters flow UI → audio thread via APVTS atomic loads (no listener, no mutex).
Position/phase data flows audio thread → UI via lock-free double-buffers and relaxed atomics.
APVTS parameter writes from drag interaction go through `MessageManager::callAsync` (GL thread → message thread → APVTS).

---

## Testing

### Engine Tests (`XYZPanTests`)

No JUCE dependency. Tests run against the pure C++ engine.

| Test File | Coverage |
|-----------|----------|
| `TestCoordinates.cpp` | Spherical conversion, azimuth/elevation/distance |
| `TestBinauralPanning.cpp` | ITD, ILD, head shadow, binaural pipeline |
| `TestDepthAndElevation.cpp` | Comb bank, pinna EQ, elevation filters |
| `TestDistanceProcessing.cpp` | Inverse-square gain, delay/Doppler, air absorption |
| `TestCreativeTools.cpp` | LFO, FDN reverb, stereo split |
| `TestSampleRateAdaptation.cpp` | Behavior at 44.1k, 48k, 96k Hz |
| `TestPerformance.cpp` | Worst-case render time benchmarks |
| `TestSineLUT.cpp` | LUT accuracy, zero-LFO fast path |

### Plugin Tests (`XYZPanPluginTests`)

Requires JUCE. Tests the integration layer.

| Test File | Coverage |
|-----------|----------|
| `TestParameterLayout.cpp` | APVTS IDs, ranges, defaults |
| `TestPositionBridge.cpp` | Lock-free bridge correctness |
| `TestPresets.cpp` | State save/load round-trip |

Framework: Catch2 v3.7.1 with CTest discovery. Run with `ctest --test-dir build`.
