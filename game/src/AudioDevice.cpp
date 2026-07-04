// The one translation unit that compiles the miniaudio implementation.
// NOMINMAX: miniaudio includes windows.h, whose min/max macros would break
// std::min/std::max in the engine headers included below.
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif
// Decoding stays ON: SoundLibrary uses ma_decode_file for wav/flac/mp3 assets.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#include <miniaudio.h>

#include "AudioDevice.h"
#include "AudioWorld.h"
#include <cstring>

struct AudioDevice::Impl {
    ma_device device{};
    AudioWorld* world = nullptr;
    bool initialized = false;
    bool started = false;
};

namespace {

void dataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frameCount) {
    auto* impl = static_cast<AudioDevice::Impl*>(device->pUserData);
    float* out = static_cast<float*>(output);
    if (impl == nullptr || impl->world == nullptr) {
        std::memset(out, 0, sizeof(float) * 2 * frameCount);
        return;
    }
    impl->world->render(out, static_cast<int>(frameCount));
}

} // namespace

AudioDevice::AudioDevice() : impl_(new Impl) {}

AudioDevice::~AudioDevice() {
    stop();
    if (impl_->initialized)
        ma_device_uninit(&impl_->device);
    delete impl_;
}

bool AudioDevice::init(unsigned preferredRate) {
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = preferredRate;
    config.dataCallback = dataCallback;
    config.pUserData = impl_;

    if (ma_device_init(nullptr, &config, &impl_->device) != MA_SUCCESS)
        return false;
    impl_->initialized = true;
    return true;
}

double AudioDevice::sampleRate() const {
    return impl_->initialized ? static_cast<double>(impl_->device.sampleRate) : 48000.0;
}

bool AudioDevice::start(AudioWorld* world) {
    if (!impl_->initialized) return false;
    impl_->world = world;
    if (ma_device_start(&impl_->device) != MA_SUCCESS) return false;
    impl_->started = true;
    return true;
}

void AudioDevice::stop() {
    if (impl_->initialized && impl_->started) {
        ma_device_stop(&impl_->device);
        impl_->started = false;
    }
}
