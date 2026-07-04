#pragma once
// miniaudio playback device wrapper. Owns the data callback, which forwards
// to AudioWorld::render(). Init order matters: init() opens the device (so
// the real sample rate is known), the caller then builds the AudioWorld, and
// start() begins callbacks.

class AudioWorld;

class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Opens the playback device (f32 stereo, requesting `preferredRate`).
    // Callbacks do not run until start(). Returns false on failure.
    bool init(unsigned preferredRate);

    // Actual device sample rate (valid after init()).
    double sampleRate() const;

    bool start(AudioWorld* world);
    void stop();

    struct Impl;  // public so the miniaudio data callback can reach it

private:
    Impl* impl_;
};
