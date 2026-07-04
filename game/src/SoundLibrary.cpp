#include "SoundLibrary.h"
#include "Sounds.h"

#include <miniaudio.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

bool isAudioFile(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav" || ext == ".flac" || ext == ".mp3";
}

// "Thud Heavy_03.wav" -> set name "thud_heavy" (lowercase, spaces -> '_',
// trailing digits and separators stripped to group variants).
std::string setNameFor(const fs::path& p) {
    std::string s = p.stem().string();
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == ' ') c = '_';
    }
    size_t end = s.size();
    while (end > 1 && std::isdigit(static_cast<unsigned char>(s[end - 1]))) --end;
    while (end > 1 && (s[end - 1] == '_' || s[end - 1] == '-' || s[end - 1] == '.')) --end;
    s.resize(end);
    return s.empty() ? "unnamed" : s;
}

} // namespace

void SoundLibrary::init(double sampleRate, std::string audioDir) {
    sampleRate_ = sampleRate;
    dir_ = std::move(audioDir);
    registerBuiltinSounds(*this, sampleRate_);
    const int n = rescan();
    std::printf("[sounds] %d sets (%d file variants from %s)\n", count(), n, dir_.c_str());
}

int SoundLibrary::find(const std::string& name) const {
    for (int i = 0; i < count(); ++i)
        if (sets_[static_cast<size_t>(i)]->name == name) return i;
    return -1;
}

const char* SoundLibrary::nameOf(int id) const {
    if (id < 0 || id >= count()) return "none";
    return sets_[static_cast<size_t>(id)]->name.c_str();
}

int SoundLibrary::getOrAddSet(const std::string& name, bool builtin) {
    const int existing = find(name);
    if (existing >= 0) return existing;
    auto s = std::make_unique<SoundSet>();
    s->name = name;
    s->builtin = builtin;
    sets_.push_back(std::move(s));
    return count() - 1;
}

void SoundLibrary::addVariant(int id, std::vector<float> samples) {
    sets_[static_cast<size_t>(id)]->variants.push_back(
        std::make_unique<std::vector<float>>(std::move(samples)));
}

const std::vector<float>* SoundLibrary::variant(int id, size_t v) const {
    if (!valid(id)) return nullptr;
    const SoundSet& s = set(id);
    return s.variants[v % s.variants.size()].get();
}

const std::vector<float>* SoundLibrary::randomVariant(int id, std::mt19937& rng) const {
    if (!valid(id)) return nullptr;
    const SoundSet& s = set(id);
    const size_t v = std::uniform_int_distribution<size_t>(0, s.variants.size() - 1)(rng);
    return s.variants[v].get();
}

int SoundLibrary::rescan() {
    std::error_code ec;
    if (dir_.empty() || !fs::is_directory(dir_, ec)) return 0;

    // Sorted load order keeps set ids deterministic across runs.
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir_, ec))
        if (entry.is_regular_file(ec) && isAudioFile(entry.path()))
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    int loaded = 0;
    for (const fs::path& p : files) {
        const std::string fname = p.filename().string();
        if (loadedFiles_.count(fname)) continue;

        ma_decoder_config cfg = ma_decoder_config_init(
            ma_format_f32, 1, static_cast<ma_uint32>(sampleRate_));
        ma_uint64 frameCount = 0;
        void* pcm = nullptr;
        if (ma_decode_file(p.string().c_str(), &cfg, &frameCount, &pcm) != MA_SUCCESS) {
            std::printf("[sounds] failed to decode %s\n", fname.c_str());
            loadedFiles_.insert(fname);  // don't retry every rescan
            continue;
        }
        std::vector<float> samples(static_cast<const float*>(pcm),
                                   static_cast<const float*>(pcm) + frameCount);
        ma_free(pcm, nullptr);

        const int id = getOrAddSet(setNameFor(p), false);
        addVariant(id, std::move(samples));
        loadedFiles_.insert(fname);
        ++loaded;
        std::printf("[sounds] loaded %s -> set '%s' (variant %d, %.2f s)\n",
                    fname.c_str(), nameOf(id),
                    static_cast<int>(set(id).variants.size()),
                    static_cast<double>(frameCount) / sampleRate_);
    }
    return loaded;
}
