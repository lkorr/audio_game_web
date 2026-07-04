// The one translation unit that compiles the miniaudio IMPLEMENTATION for the
// web build -- decoder only, no device backend.
//
// On native, AudioDevice.cpp was the TU that defined MINIAUDIO_IMPLEMENTATION
// (it needed both the decoder AND the playback device). On web the device is
// the browser's AudioWorklet (see WebAudioDevice.cpp), so we disable every
// device backend here and keep ONLY the wav/flac/mp3 decoders. That lets
// SoundLibrary.cpp's existing ma_decode_file() path run UNCHANGED against the
// files Emscripten preloaded into MEMFS (--preload-file assets@/assets) --
// std::filesystem::directory_iterator and fopen both work in MEMFS.
//
// Result: no source changes to SoundLibrary.cpp or Sounds.cpp. The custom
// sound-design goal ("tons of wav/mp3 files") is served by simply dropping
// more files into webgame/assets/audio/ before building.

// Decode only: turn off the device/backends and the encoder.
#define MA_NO_DEVICE_IO
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_THREADING
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
