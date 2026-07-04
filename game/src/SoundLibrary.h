#pragma once
// Named registry of sounds. A SoundSet is one logical sound with one or more
// mono variant buffers (footsteps/impacts pick a random variant per trigger;
// loops pick a stable variant per object so emitters sharing a set stay
// decorrelated). Sets come from two places:
//
//   builtins  -- procedurally synthesized at startup (Sounds.cpp)
//   files     -- decoded from the audio asset folder (wav/flac/mp3), resampled
//                and downmixed to mono at the device rate
//
// File naming: "thud.wav" makes set "thud"; trailing digits group variants, so
// "thud_01.wav" + "thud_02.wav" make one set "thud" with two variants. A file
// set whose name matches an existing set appends variants to it (this is how
// custom footstep variants extend a material set). Names are lowercased and
// spaces become '_'.
//
// Audio-thread safety: the registry is append-only and variant buffers live
// behind unique_ptr, so a const std::vector<float>* handed to a voice stays
// valid for the lifetime of the library. rescan() may be called from the game
// thread while audio plays.

#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

struct SoundSet {
    std::string name;
    bool builtin = false;
    std::vector<std::unique_ptr<std::vector<float>>> variants;
};

class SoundLibrary {
public:
    // Synthesizes the builtins, then scans `audioDir` (missing dir is fine).
    void init(double sampleRate, std::string audioDir);

    // Loads any new files in the audio dir. Returns the number of new variant
    // buffers loaded. Never removes or reallocates existing buffers.
    int rescan();

    int count() const { return static_cast<int>(sets_.size()); }
    const SoundSet& set(int id) const { return *sets_[static_cast<size_t>(id)]; }
    bool valid(int id) const { return id >= 0 && id < count() && !set(id).variants.empty(); }

    // -1 if no set has that name.
    int find(const std::string& name) const;
    // "none" for -1 / out of range.
    const char* nameOf(int id) const;

    // Existing set id for `name`, or a fresh empty one.
    int getOrAddSet(const std::string& name, bool builtin);
    void addVariant(int id, std::vector<float> samples);

    // nullptr when the id/set is empty.
    const std::vector<float>* variant(int id, size_t v) const;
    const std::vector<float>* randomVariant(int id, std::mt19937& rng) const;

    const std::string& dir() const { return dir_; }

private:
    double sampleRate_ = 48000.0;
    std::string dir_;
    std::vector<std::unique_ptr<SoundSet>> sets_;
    std::set<std::string> loadedFiles_;  // filenames already decoded
};
