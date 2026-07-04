#pragma once
// Web replacement for AudioDevice. Same surface the game code uses, but instead
// of opening a miniaudio playback device it publishes the AudioWorld* to a
// global the AudioWorklet reaches through wasm_render(). No device thread is
// owned here -- the browser's audio render quantum drives everything.
//
// Sample rate is NOT chosen here: the browser's AudioContext decides it (often
// 44100). main.js reads context.sampleRate and passes it to wasm_init(), which
// constructs the world at that rate; WebAudioDevice::init() just records it so
// sampleRate() answers consistently.

class AudioWorld;

class WebAudioDevice {
public:
    WebAudioDevice() = default;

    WebAudioDevice(const WebAudioDevice&) = delete;
    WebAudioDevice& operator=(const WebAudioDevice&) = delete;

    // Records the (browser-decided) rate. Returns true always.
    bool init(unsigned actualRate);

    double sampleRate() const { return rate_; }

    // Publishes `world` to the worklet-visible global. Callbacks (wasm_render)
    // become live immediately; a fade-in in AudioWorld masks the ramp-up.
    bool start(AudioWorld* world);

    // Detaches the world from the worklet (renders silence thereafter).
    void stop();

private:
    double rate_ = 48000.0;
};
