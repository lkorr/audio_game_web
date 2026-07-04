#include "SceneIO.h"
#include "SoundLibrary.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

const SoundSlot kAllSlots[] = {
    SoundSlot::Loop, SoundSlot::IdleRandom, SoundSlot::Proximity, SoundSlot::Impact,
};

SoundSlot slotByName(const std::string& name) {
    for (SoundSlot s : kAllSlots)
        if (name == soundSlotName(s)) return s;
    return SoundSlot::kCount;
}

void writeRule(std::ofstream& f, SoundSlot slot, const SoundRule& r, const SoundLibrary& lib) {
    if (r.soundSet < 0) return;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "rule %s sound=%s enabled=%d gain=%.3f audradius=%.1f verb=%.3f doppler=%d "
                  "dopscale=%.2f min=%.2f max=%.2f radius=%.2f cooldown=%.2f "
                  "pitchjit=%.3f gainjit=%.2f\n",
                  soundSlotName(slot), lib.nameOf(r.soundSet), r.enabled ? 1 : 0,
                  r.gain, r.audibleRadius, r.verbWet, r.doppler ? 1 : 0,
                  r.dopplerScale, r.minInterval, r.maxInterval, r.radius, r.cooldown,
                  r.pitchJitter, r.gainJitterDb);
    f << buf;
}

void writeProfile(std::ofstream& f, const SoundProfile& p, const SoundLibrary& lib) {
    for (SoundSlot s : kAllSlots)
        writeRule(f, s, p.rule(s), lib);
}

// No-throw float parse (malformed values read as 0 instead of crashing).
float toF(const std::string& v) { return std::strtof(v.c_str(), nullptr); }

// "key=value" -> true and fills key/value.
bool splitKV(const std::string& tok, std::string& key, std::string& val) {
    const size_t eq = tok.find('=');
    if (eq == std::string::npos) return false;
    key = tok.substr(0, eq);
    val = tok.substr(eq + 1);
    return true;
}

void applyRuleToken(SoundRule& r, const std::string& key, const std::string& val,
                    const SoundLibrary& lib, int lineNo) {
    if (key == "sound") {
        r.soundSet = lib.find(val);
        if (r.soundSet < 0)
            std::printf("[scene] line %d: unknown sound '%s' (left unassigned)\n",
                        lineNo, val.c_str());
    }
    else if (key == "enabled") r.enabled = val != "0";
    else if (key == "gain") r.gain = toF(val);
    else if (key == "audradius") r.audibleRadius = toF(val);
    else if (key == "verb") r.verbWet = toF(val);
    else if (key == "doppler") r.doppler = val != "0";
    else if (key == "dopscale") r.dopplerScale = toF(val);
    else if (key == "min") r.minInterval = toF(val);
    else if (key == "max") r.maxInterval = toF(val);
    else if (key == "radius") r.radius = toF(val);
    else if (key == "cooldown") r.cooldown = toF(val);
    else if (key == "pitchjit") r.pitchJitter = toF(val);
    else if (key == "gainjit") r.gainJitterDb = toF(val);
    else std::printf("[scene] line %d: unknown rule key '%s'\n", lineNo, key.c_str());
}

} // namespace

bool saveScene(const Scene& scene, const SoundLibrary& lib,
               const EngineTuning& tuning, const std::string& path) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;

    f << "# audio_game scene v1\n";
    for (int i = 0; i < EP_Count; ++i) {
        if (tuning.isDefault(i)) continue;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "engine %s=%.5g\n",
                      kEngineParams[i].key, tuning.v[i]);
        f << buf;
    }
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const FloorMaterial& m = scene.materials[i];
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "material %zu name=%s sound=%s gain=%.3f pitchjit=%.3f gainjit=%.2f\n",
                      i, m.name.c_str(), lib.nameOf(m.soundSet), m.gain,
                      m.pitchJitter, m.gainJitterDb);
        f << buf;
    }
    // Floor grid, run-length encoded per row ("mat*count"); all-default rows
    // are skipped so an unpainted world stays compact. Row 0 is always
    // written: it marks the file as grid-aware, so the loader's legacy
    // creek-band fallback (for pre-grid files) stays off.
    for (int j = 0; j < Scene::kGridN; ++j) {
        bool any = j == 0;
        for (int i = 0; i < Scene::kGridN && !any; ++i)
            if (scene.cell(i, j) != 0) any = true;
        if (!any) continue;
        f << "floorrow " << j;
        int i = 0;
        while (i < Scene::kGridN) {
            const uint8_t m = scene.cell(i, j);
            int run = 1;
            while (i + run < Scene::kGridN && scene.cell(i + run, j) == m) ++run;
            f << ' ' << static_cast<int>(m) << '*' << run;
            i += run;
        }
        f << '\n';
    }
    for (const Tree& t : scene.trees) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "tree %.3f %.3f trunkr=%.2f trunkh=%.2f canopyr=%.2f canopyh=%.2f\n",
                      t.pos.x, t.pos.y, t.trunkRadius, t.trunkHeight,
                      t.canopyRadius, t.canopyHeight);
        f << buf;
        writeProfile(f, t.sounds, lib);
    }
    for (const Boulder& b : scene.boulders) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "boulder %.3f %.3f %.3f %.3f %.3f %.3f\n",
                      b.center.x, b.center.y, b.center.z, b.half.x, b.half.y, b.half.z);
        f << buf;
        writeProfile(f, b.sounds, lib);
    }
    for (const Emitter& e : scene.emitters) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "emitter %.3f %.3f %.3f music=%d musicradius=%.2f goal=%d\n",
                      e.pos.x, e.pos.y, e.pos.z, e.isMusic ? 1 : 0, e.musicRadius,
                      e.isGoal ? 1 : 0);
        f << buf;
        writeProfile(f, e.sounds, lib);
    }
    for (const Bug& b : scene.bugs) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bug %.3f %.3f %.3f %.2f\n",
                      b.home.x, b.home.y, b.home.z, b.wanderRadius);
        f << buf;
        writeProfile(f, b.sounds, lib);
    }
    for (const Stalker& st : scene.stalkers) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "stalker %.3f %.3f %.3f patrol=%.2f\n",
                      st.home.x, st.home.y, st.home.z, st.patrolRadius);
        f << buf;
        writeProfile(f, st.sounds, lib);
    }
    return f.good();
}

bool loadScene(Scene& scene, const SoundLibrary& lib,
               EngineTuning& tuning, const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    EngineTuning outTuning;  // defaults; file overrides
    Scene out;
    // Default material table; `material` lines override assignments.
    out.materials.push_back({ "grass", lib.find("grass"), 0.7f, 0.05f, 2.0f });
    out.materials.push_back({ "stone", lib.find("stone"), 0.7f, 0.05f, 2.0f });

    SoundProfile* current = nullptr;
    bool sawFloorRow = false;
    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        std::istringstream ss(line);
        std::string word;
        if (!(ss >> word) || word[0] == '#') continue;

        if (word == "engine") {
            std::string tok, key, val;
            while (ss >> tok)
                if (splitKV(tok, key, val) && !outTuning.setByKey(key.c_str(), toF(val)))
                    std::printf("[scene] line %d: unknown engine param '%s'\n",
                                lineNo, key.c_str());
        } else if (word == "material") {
            size_t idx = 0;
            ss >> idx;
            if (idx > 255) {
                std::printf("[scene] line %d: material index %zu out of range\n", lineNo, idx);
                continue;
            }
            // Saved worlds may carry more materials than the default two.
            while (idx >= out.materials.size())
                out.materials.push_back({ "mat" + std::to_string(out.materials.size()),
                                          -1, 0.7f, 0.05f, 2.0f });
            FloorMaterial& m = out.materials[idx];
            std::string tok, key, val;
            while (ss >> tok) {
                if (!splitKV(tok, key, val)) continue;
                if (key == "name") m.name = val;
                else if (key == "sound") {
                    m.soundSet = lib.find(val);
                    if (m.soundSet < 0)
                        std::printf("[scene] line %d: unknown sound '%s'\n", lineNo, val.c_str());
                }
                else if (key == "gain") m.gain = toF(val);
                else if (key == "pitchjit") m.pitchJitter = toF(val);
                else if (key == "gainjit") m.gainJitterDb = toF(val);
            }
        } else if (word == "floorrow") {
            sawFloorRow = true;
            int j = -1;
            ss >> j;
            if (j < 0 || j >= Scene::kGridN) {
                std::printf("[scene] line %d: floorrow %d out of range\n", lineNo, j);
                continue;
            }
            int i = 0;
            std::string tok;
            while (ss >> tok && i < Scene::kGridN) {
                const size_t star = tok.find('*');
                if (star == std::string::npos) continue;
                const int mat = std::atoi(tok.c_str());
                const int run = std::atoi(tok.c_str() + star + 1);
                for (int k = 0; k < run && i < Scene::kGridN; ++k, ++i)
                    out.cell(i, j) = static_cast<uint8_t>(std::clamp(mat, 0, 255));
            }
        } else if (word == "tree") {
            Tree t;
            ss >> t.pos.x >> t.pos.y;
            std::string tok, key, val;
            while (ss >> tok) {
                if (!splitKV(tok, key, val)) continue;
                if (key == "trunkr") t.trunkRadius = toF(val);
                else if (key == "trunkh") t.trunkHeight = toF(val);
                else if (key == "canopyr") t.canopyRadius = toF(val);
                else if (key == "canopyh") t.canopyHeight = toF(val);
            }
            t.id = out.takeId();
            out.trees.push_back(t);
            current = &out.trees.back().sounds;
        } else if (word == "boulder") {
            Boulder b;
            ss >> b.center.x >> b.center.y >> b.center.z >> b.half.x >> b.half.y >> b.half.z;
            b.id = out.takeId();
            out.boulders.push_back(b);
            current = &out.boulders.back().sounds;
        } else if (word == "emitter") {
            Emitter e;
            ss >> e.pos.x >> e.pos.y >> e.pos.z;
            std::string tok, key, val;
            while (ss >> tok) {
                if (!splitKV(tok, key, val)) continue;
                if (key == "music") e.isMusic = val != "0";
                else if (key == "musicradius") e.musicRadius = toF(val);
                else if (key == "goal") e.isGoal = val != "0";
            }
            e.id = out.takeId();
            out.emitters.push_back(e);
            current = &out.emitters.back().sounds;
        } else if (word == "bug") {
            Bug b;
            ss >> b.home.x >> b.home.y >> b.home.z >> b.wanderRadius;
            if (b.wanderRadius <= 0.0f) b.wanderRadius = 4.0f;  // omitted field
            b.pos = b.home;
            b.id = out.takeId();
            out.bugs.push_back(b);
            current = &out.bugs.back().sounds;
        } else if (word == "stalker") {
            Stalker st;
            ss >> st.home.x >> st.home.y >> st.home.z;
            std::string tok, key, val;
            while (ss >> tok) {
                if (!splitKV(tok, key, val)) continue;
                if (key == "patrol") st.patrolRadius = toF(val);
            }
            st.pos = st.home;
            st.id = out.takeId();
            out.stalkers.push_back(st);
            current = &out.stalkers.back().sounds;
        } else if (word == "rule") {
            std::string slotName;
            ss >> slotName;
            const SoundSlot slot = slotByName(slotName);
            if (current == nullptr || slot == SoundSlot::kCount) {
                std::printf("[scene] line %d: stray/unknown rule '%s'\n", lineNo, slotName.c_str());
                continue;
            }
            SoundRule& r = current->rule(slot);
            std::string tok, key, val;
            while (ss >> tok)
                if (splitKV(tok, key, val))
                    applyRuleToken(r, key, val, lib, lineNo);
        } else {
            std::printf("[scene] line %d: unknown directive '%s'\n", lineNo, word.c_str());
        }
    }

    // Files saved before the floor grid existed carry no floorrow lines; give
    // them the stone creek band the old hardcoded materialAt() implied.
    if (!sawFloorRow)
        out.paintRect({ -Scene::kHalfExtent, Scene::kCreekY - Scene::kStoneHalfWidth },
                      { Scene::kHalfExtent, Scene::kCreekY + Scene::kStoneHalfWidth }, 1);

    scene = std::move(out);
    tuning = outTuning;
    std::printf("[scene] loaded %s: %zu trees, %zu boulders, %zu emitters\n",
                path.c_str(), scene.trees.size(), scene.boulders.size(),
                scene.emitters.size());
    return true;
}
