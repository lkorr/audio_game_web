#include "DevMode.h"
#include "SoundLibrary.h"
#include "SoundDirector.h"
#include "SceneIO.h"
#include <SDL3/SDL.h>
#include <glm/geometric.hpp>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kMinDist = 1.0f, kMaxDist = 20.0f;
constexpr float kPickRange = 30.0f;          // max ray distance for objects
constexpr float kHighlightFlash = 0.45f;     // steady flash level on the pick

// ---- ray helpers (origin o, normalized dir d) ------------------------------

// Vertical cylinder (axis through c, z in [z0,z1], radius r). Returns hit t.
bool rayCylinder(const glm::vec3& o, const glm::vec3& d, const glm::vec2& c,
                 float r, float z0, float z1, float& tOut) {
    const glm::vec2 oc{ o.x - c.x, o.y - c.y };
    const glm::vec2 dd{ d.x, d.y };
    const float a = glm::dot(dd, dd);
    if (a < 1e-8f) return false;  // looking straight up/down
    const float b = glm::dot(oc, dd);
    const float cc = glm::dot(oc, oc) - r * r;
    const float disc = b * b - a * cc;
    if (disc < 0.0f) return false;
    const float t = (-b - std::sqrt(disc)) / a;
    if (t < 0.0f) return false;
    const float z = o.z + t * d.z;
    if (z < z0 || z > z1) return false;
    tOut = t;
    return true;
}

bool rayAabb(const glm::vec3& o, const glm::vec3& d, const glm::vec3& mn,
             const glm::vec3& mx, float& tOut) {
    float t0 = 0.0f, t1 = 1e9f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-8f) {
            if (o[i] < mn[i] || o[i] > mx[i]) return false;
            continue;
        }
        float ta = (mn[i] - o[i]) / d[i];
        float tb = (mx[i] - o[i]) / d[i];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    tOut = t0;
    return true;
}

bool raySphere(const glm::vec3& o, const glm::vec3& d, const glm::vec3& c,
               float r, float& tOut) {
    const glm::vec3 oc = o - c;
    const float b = glm::dot(oc, d);
    const float disc = b * b - (glm::dot(oc, oc) - r * r);
    if (disc < 0.0f) return false;
    const float t = -b - std::sqrt(disc);
    if (t < 0.0f) return false;
    tOut = t;
    return true;
}

std::string fmt(const char* f, ...) {
    char buf[160];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buf, sizeof(buf), f, args);
    va_end(args);
    return buf;
}

// Engine param value text: ON/OFF for toggles, decimals scaled to the step.
std::string engineValueText(const EngineTuning& t, int id) {
    const EngineParamDef& d = kEngineParams[id];
    if (d.toggle) return t.v[id] > 0.5f ? "ON" : "OFF";
    if (d.step >= 1.0f) return fmt("%.0f", t.v[id]);
    if (d.step >= 0.02f) return fmt("%.2f", t.v[id]);
    return fmt("%.3f", t.v[id]);
}

// Overlay palette
const glm::vec4 kPanel{ 0.02f, 0.03f, 0.06f, 0.78f };
const glm::vec4 kHeader{ 0.55f, 0.85f, 1.0f, 1.0f };
const glm::vec4 kHint{ 0.55f, 0.6f, 0.7f, 1.0f };
const glm::vec4 kText{ 0.85f, 0.88f, 0.95f, 1.0f };
const glm::vec4 kSelected{ 1.0f, 0.92f, 0.35f, 1.0f };
const glm::vec4 kSelectBar{ 0.25f, 0.28f, 0.1f, 0.85f };
const glm::vec4 kDisabled{ 0.45f, 0.4f, 0.4f, 1.0f };
const glm::vec4 kTarget{ 0.5f, 1.0f, 0.6f, 1.0f };

} // namespace

void DevMode::init(SoundLibrary* lib, SoundDirector* director, EngineTuning* tuning,
                   std::string scenePath) {
    lib_ = lib;
    director_ = director;
    tuning_ = tuning;
    scenePath_ = std::move(scenePath);
    placeSound_ = lib_->find("water");
}

void DevMode::toggle(Player& player) {
    enabled = !enabled;
    state_ = State::Inspect;
    target_ = {};
    if (!enabled) {
        player.flying = false;
        std::printf("[dev] dev mode OFF\n");
        return;
    }
    std::printf("[dev] dev mode ON (overlay shows controls)\n");
}

glm::vec3 DevMode::aimPoint(const Player& player) const {
    glm::vec3 p = player.eyePos() + player.forward() * dist_;
    p.x = std::clamp(p.x, -Scene::kHalfExtent, Scene::kHalfExtent);
    p.y = std::clamp(p.y, -Scene::kHalfExtent, Scene::kHalfExtent);
    p.z += heightOffset_;
    return p;
}

bool DevMode::aimGround(const Player& player, glm::vec2& out) const {
    const glm::vec3 o = player.eyePos();
    const glm::vec3 d = player.forward();
    if (d.z >= -1e-4f) return false;   // looking level or up
    const float t = -o.z / d.z;
    out = { std::clamp(o.x + t * d.x, -Scene::kHalfExtent, Scene::kHalfExtent),
            std::clamp(o.y + t * d.y, -Scene::kHalfExtent, Scene::kHalfExtent) };
    return true;
}

DevMode::Target DevMode::pick(const Scene& scene, const Player& player) const {
    const glm::vec3 o = player.eyePos();
    const glm::vec3 d = player.forward();

    Target best;
    best.rayT = kPickRange;

    float t = 0.0f;
    for (const Tree& tr : scene.trees) {
        // Generous radius so the canopy region picks too.
        const float r = std::max(tr.trunkRadius + 0.25f, tr.canopyRadius * 0.6f);
        if (rayCylinder(o, d, tr.pos, r, 0.0f, tr.trunkHeight + tr.canopyHeight, t)
            && t < best.rayT) {
            best = { TargetKind::Tree, tr.id, 0, o + d * t, t };
        }
    }
    for (const Boulder& b : scene.boulders) {
        const glm::vec3 pad{ 0.15f };
        if (rayAabb(o, d, b.center - b.half - pad, b.center + b.half + pad, t)
            && t < best.rayT) {
            best = { TargetKind::Boulder, b.id, 0, o + d * t, t };
        }
    }
    for (const Emitter& e : scene.emitters) {
        if (raySphere(o, d, e.pos, 0.5f, t) && t < best.rayT)
            best = { TargetKind::Emitter, e.id, 0, o + d * t, t };
    }
    for (const Bug& b : scene.bugs) {
        // Generous sphere -- they move.
        if (raySphere(o, d, b.pos, 0.7f, t) && t < best.rayT)
            best = { TargetKind::Bug, b.id, 0, o + d * t, t };
    }

    if (best.kind == TargetKind::None && d.z < -1e-4f) {
        // Floor at the ray-ground intersection.
        t = -o.z / d.z;
        glm::vec3 hit = o + d * t;
        hit.x = std::clamp(hit.x, -Scene::kHalfExtent, Scene::kHalfExtent);
        hit.y = std::clamp(hit.y, -Scene::kHalfExtent, Scene::kHalfExtent);
        best = { TargetKind::Floor, 0, scene.materialAt(hit.x, hit.y), hit, t };
    }
    return best;
}

void DevMode::frameUpdate(Scene& scene, const Player& player, Renderer& renderer) {
    if (!enabled) return;

    if (state_ == State::Inspect)
        target_ = pick(scene, player);

    // PAINT: drag-paint while the left button is held.
    if (state_ == State::Paint) {
        const SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
        if (buttons & SDL_BUTTON_LMASK) paintBrush(scene, player, renderer);
        return;
    }

    // Steady highlight flash on the inspected / menu-edited object. Reuses
    // the bump-flash path: set every frame, decays naturally when the pick
    // moves on.
    const Target& t = (state_ == State::Menu) ? menuTarget_ : target_;
    if (state_ == State::Place || state_ == State::EngineMenu ||
        state_ == State::Worlds) return;
    if (t.kind == TargetKind::Tree) {
        for (Tree& tr : scene.trees)
            if (tr.id == t.id) {
                tr.flash = std::max(tr.flash, kHighlightFlash);
                tr.flashPos = t.hitPos;
            }
    } else if (t.kind == TargetKind::Boulder) {
        for (Boulder& b : scene.boulders)
            if (b.id == t.id) {
                b.flash = std::max(b.flash, kHighlightFlash);
                b.flashPos = t.hitPos;
            }
    }
}

bool DevMode::preview(const Player& player, DevPreview& out) const {
    if (!enabled) return false;
    if (state_ == State::Place) {
        const glm::vec3 p = aimPoint(player);
        switch (type_) {
            case PlaceType::Tree:    out.shape = 0; out.pos = { p.x, p.y, 0.0f }; break;
            case PlaceType::Boulder: out.shape = 1; out.pos = { p.x, p.y, 0.0f }; break;
            default:                 out.shape = 2; out.pos = { p.x, p.y, std::max(p.z, 0.1f) }; break;
        }
        return true;
    }
    // Paint: brush ring at the ground aim point, tinted like the material.
    if (state_ == State::Paint) {
        glm::vec2 g;
        if (!aimGround(player, g)) return false;
        out.shape = 3;
        out.pos = { g.x, g.y, 0.02f };
        out.scale = brushRadius_;
        out.color = Renderer::floorColor(paintMat_);
        out.hasColor = true;
        return true;
    }
    // Inspect/menu: emitters/bugs have no flashable wireframe, so pulse a
    // diamond at the picked one (tracks while inspecting; frozen in menu).
    if (state_ == State::EngineMenu || state_ == State::Worlds) return false;
    const Target& t = (state_ == State::Menu) ? menuTarget_ : target_;
    if (t.kind == TargetKind::Emitter || t.kind == TargetKind::Bug) {
        out.shape = 2;
        out.pos = t.hitPos;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Menu plumbing
// ---------------------------------------------------------------------------

SoundProfile* DevMode::menuProfile(Scene& scene, glm::vec3* posOut) const {
    switch (menuTarget_.kind) {
        case TargetKind::Tree:
            for (Tree& t : scene.trees)
                if (t.id == menuTarget_.id) {
                    if (posOut) *posOut = t.soundPos();
                    return &t.sounds;
                }
            break;
        case TargetKind::Boulder:
            for (Boulder& b : scene.boulders)
                if (b.id == menuTarget_.id) {
                    if (posOut) *posOut = b.soundPos();
                    return &b.sounds;
                }
            break;
        case TargetKind::Emitter:
            for (Emitter& e : scene.emitters)
                if (e.id == menuTarget_.id) {
                    if (posOut) *posOut = e.soundPos();
                    return &e.sounds;
                }
            break;
        case TargetKind::Bug:
            for (Bug& b : scene.bugs)
                if (b.id == menuTarget_.id) {
                    if (posOut) *posOut = b.soundPos();
                    return &b.sounds;
                }
            break;
        default:
            break;
    }
    return nullptr;
}

FloorMaterial* DevMode::menuFloor(Scene& scene) const {
    if (menuTarget_.kind != TargetKind::Floor) return nullptr;
    const size_t i = static_cast<size_t>(menuTarget_.material);
    return i < scene.materials.size() ? &scene.materials[i] : nullptr;
}

Tree* DevMode::menuTree(Scene& scene) const {
    if (menuTarget_.kind != TargetKind::Tree) return nullptr;
    for (Tree& t : scene.trees)
        if (t.id == menuTarget_.id) return &t;
    return nullptr;
}

Boulder* DevMode::menuBoulder(Scene& scene) const {
    if (menuTarget_.kind != TargetKind::Boulder) return nullptr;
    for (Boulder& b : scene.boulders)
        if (b.id == menuTarget_.id) return &b;
    return nullptr;
}

Emitter* DevMode::menuEmitter(Scene& scene) const {
    if (menuTarget_.kind != TargetKind::Emitter) return nullptr;
    for (Emitter& e : scene.emitters)
        if (e.id == menuTarget_.id) return &e;
    return nullptr;
}

Bug* DevMode::menuBug(Scene& scene) const {
    if (menuTarget_.kind != TargetKind::Bug) return nullptr;
    for (Bug& b : scene.bugs)
        if (b.id == menuTarget_.id) return &b;
    return nullptr;
}

void DevMode::openMenu(Scene& scene) {
    if (target_.kind == TargetKind::None) return;
    menuTarget_ = target_;
    state_ = State::Menu;
    menuFromPaint_ = false;
    page_ = MenuPage::Sounds;
    row_ = 0;
    if (menuTarget_.kind != TargetKind::Floor) {
        // Open on the most interesting slot: first one with a sound assigned.
        slot_ = SoundSlot::Loop;
        if (const SoundProfile* profile = menuProfile(scene)) {
            for (int s = 0; s < kSoundSlotCount; ++s)
                if (profile->rules[s].soundSet >= 0) {
                    slot_ = static_cast<SoundSlot>(s);
                    break;
                }
        }
    }
}

void DevMode::closeMenu() {
    state_ = menuFromPaint_ ? State::Paint : State::Inspect;
    menuFromPaint_ = false;
}

std::vector<DevMode::Row> DevMode::menuRows(const Scene& /*scene*/) const {
    if (menuTarget_.kind == TargetKind::Floor)
        return { Row::Sound, Row::Gain, Row::PitchJit, Row::GainJit, Row::Audition };

    if (page_ == MenuPage::Body) {
        switch (menuTarget_.kind) {
            case TargetKind::Tree:
                return { Row::Page, Row::PosX, Row::PosY,
                         Row::SizeA, Row::SizeB, Row::SizeC, Row::SizeD };
            case TargetKind::Boulder:
                return { Row::Page, Row::PosX, Row::PosY, Row::PosZ,
                         Row::SizeA, Row::SizeB, Row::SizeC };
            case TargetKind::Emitter:
                return { Row::Page, Row::PosX, Row::PosY, Row::PosZ };
            case TargetKind::Bug:
                return { Row::Page, Row::PosX, Row::PosY, Row::PosZ, Row::SizeA };
            default:
                return { Row::Page };
        }
    }

    std::vector<Row> rows{ Row::Page, Row::Slot, Row::Sound, Row::Enabled, Row::Gain,
                           Row::AudibleRadius };
    switch (slot_) {
        case SoundSlot::Loop:
            rows.insert(rows.end(), { Row::Verb, Row::Doppler, Row::DopplerScale });
            break;
        case SoundSlot::IdleRandom:
            rows.insert(rows.end(), { Row::MinInterval, Row::MaxInterval,
                                      Row::PitchJit, Row::GainJit, Row::Audition });
            break;
        case SoundSlot::Proximity:
            rows.insert(rows.end(), { Row::Radius, Row::Cooldown,
                                      Row::PitchJit, Row::GainJit, Row::Audition });
            break;
        case SoundSlot::Impact:
            rows.insert(rows.end(), { Row::Cooldown, Row::PitchJit, Row::GainJit,
                                      Row::Audition });
            break;
        default:
            break;
    }
    return rows;
}

std::string DevMode::rowLabel(Row r) const {
    const TargetKind k = menuTarget_.kind;
    switch (r) {
        case Row::Page:          return "PAGE";
        case Row::PosX:          return "POS X";
        case Row::PosY:          return "POS Y";
        case Row::PosZ:
            return k == TargetKind::Boulder ? "CENTER Z"
                 : k == TargetKind::Bug ? "FLY HEIGHT" : "HEIGHT";
        case Row::SizeA:
            return k == TargetKind::Tree ? "TRUNK RADIUS"
                 : k == TargetKind::Boulder ? "HALF X" : "WANDER RADIUS";
        case Row::SizeB:
            return k == TargetKind::Tree ? "TRUNK HEIGHT" : "HALF Y";
        case Row::SizeC:
            return k == TargetKind::Tree ? "CANOPY RADIUS" : "HALF Z";
        case Row::SizeD:         return "CANOPY HEIGHT";
        case Row::Slot:          return "SLOT";
        case Row::Sound:         return "SOUND";
        case Row::Enabled:       return "ENABLED";
        case Row::Gain:          return "GAIN";
        case Row::AudibleRadius: return "AUDIBLE RADIUS";
        case Row::Verb:          return "REVERB";
        case Row::Doppler:       return "DOPPLER";
        case Row::DopplerScale:  return "DOPPLER SCALE";
        case Row::MinInterval:   return "MIN INTERVAL";
        case Row::MaxInterval:   return "MAX INTERVAL";
        case Row::Radius:        return "TRIGGER RADIUS";
        case Row::Cooldown:    return "COOLDOWN";
        case Row::PitchJit:    return "PITCH JITTER";
        case Row::GainJit:     return "GAIN JITTER";
        case Row::Audition:    return "AUDITION";
    }
    return "?";
}

std::string DevMode::rowValue(const Scene& scene, Row r) const {
    // menuProfile/menuFloor return mutable pointers for the edit path; here
    // we only read.
    Scene& s = const_cast<Scene&>(scene);
    if (const FloorMaterial* m = menuFloor(s)) {
        switch (r) {
            case Row::Sound:    return lib_->nameOf(m->soundSet);
            case Row::Gain:     return fmt("%.2f", m->gain);
            case Row::PitchJit: return fmt("%.2f", m->pitchJitter);
            case Row::GainJit:  return fmt("%.1f DB", m->gainJitterDb);
            case Row::Audition: return "(ENTER)";
            default:            return "";
        }
    }
    if (r == Row::Page)
        return page_ == MenuPage::Sounds ? "SOUNDS" : "BODY";

    // BODY-page rows: read the object's physical stats.
    if (r == Row::PosX || r == Row::PosY || r == Row::PosZ ||
        r == Row::SizeA || r == Row::SizeB || r == Row::SizeC || r == Row::SizeD) {
        if (const Tree* t = menuTree(s)) {
            switch (r) {
                case Row::PosX:  return fmt("%.2f M", t->pos.x);
                case Row::PosY:  return fmt("%.2f M", t->pos.y);
                case Row::SizeA: return fmt("%.2f M", t->trunkRadius);
                case Row::SizeB: return fmt("%.2f M", t->trunkHeight);
                case Row::SizeC: return fmt("%.2f M", t->canopyRadius);
                case Row::SizeD: return fmt("%.2f M", t->canopyHeight);
                default: break;
            }
        } else if (const Boulder* b = menuBoulder(s)) {
            switch (r) {
                case Row::PosX:  return fmt("%.2f M", b->center.x);
                case Row::PosY:  return fmt("%.2f M", b->center.y);
                case Row::PosZ:  return fmt("%.2f M", b->center.z);
                case Row::SizeA: return fmt("%.2f M", b->half.x);
                case Row::SizeB: return fmt("%.2f M", b->half.y);
                case Row::SizeC: return fmt("%.2f M", b->half.z);
                default: break;
            }
        } else if (const Emitter* e = menuEmitter(s)) {
            switch (r) {
                case Row::PosX: return fmt("%.2f M", e->pos.x);
                case Row::PosY: return fmt("%.2f M", e->pos.y);
                case Row::PosZ: return fmt("%.2f M", e->pos.z);
                default: break;
            }
        } else if (const Bug* g = menuBug(s)) {
            switch (r) {
                case Row::PosX:  return fmt("%.2f M", g->home.x);
                case Row::PosY:  return fmt("%.2f M", g->home.y);
                case Row::PosZ:  return fmt("%.2f M", g->home.z);
                case Row::SizeA: return fmt("%.1f M", g->wanderRadius);
                default: break;
            }
        }
        return "";
    }

    const SoundProfile* prof = menuProfile(s);
    if (prof == nullptr) return "";
    const SoundRule& rule = prof->rule(slot_);
    switch (r) {
        case Row::Slot: {
            // "LOOP" with assignment markers on the other slots, e.g.
            // "< NEAR >  (LOOP+ IMPACT+)" would be noisy; keep it simple.
            return soundSlotName(slot_);
        }
        case Row::Sound:         return lib_->nameOf(rule.soundSet);
        case Row::Enabled:       return rule.enabled ? "ON" : "OFF";
        case Row::Gain:          return fmt("%.2f", rule.gain);
        case Row::AudibleRadius: return fmt("%.0f M", rule.audibleRadius);
        case Row::Verb:          return fmt("%.2f", rule.verbWet);
        case Row::Doppler:       return rule.doppler ? "ON" : "OFF";
        case Row::DopplerScale:  return fmt("%.2f X", rule.dopplerScale);
        case Row::MinInterval: return fmt("%.1f S", rule.minInterval);
        case Row::MaxInterval: return fmt("%.1f S", rule.maxInterval);
        case Row::Radius:      return fmt("%.1f M", rule.radius);
        case Row::Cooldown:    return fmt("%.2f S", rule.cooldown);
        case Row::PitchJit:    return fmt("%.2f", rule.pitchJitter);
        case Row::GainJit:     return fmt("%.1f DB", rule.gainJitterDb);
        case Row::Audition:    return "(ENTER)";
    }
    return "";
}

void DevMode::adjustRow(Scene& scene, Row r, int dir, const ListenerPose& listener,
                        Renderer& renderer) {
    const int n = lib_->count();
    auto cycleSound = [&](int cur) {
        int v = cur + dir;
        if (v < -1) v = n - 1;
        if (v >= n) v = -1;
        return v;
    };

    if (r == Row::Page) {
        page_ = (page_ == MenuPage::Sounds) ? MenuPage::Body : MenuPage::Sounds;
        row_ = 0;
        return;
    }

    // BODY-page rows: nudge the stat, then re-upload geometry so the
    // wireframe tracks the edit. Loop voices follow soundPos() per frame.
    if (r == Row::PosX || r == Row::PosY || r == Row::PosZ ||
        r == Row::SizeA || r == Row::SizeB || r == Row::SizeC || r == Row::SizeD) {
        const float fd = static_cast<float>(dir);
        const float lim = Scene::kHalfExtent;
        auto nudge = [&](float& v, float step, float lo, float hi) {
            v = std::clamp(v + fd * step, lo, hi);
        };
        if (Tree* t = menuTree(scene)) {
            switch (r) {
                case Row::PosX:  nudge(t->pos.x, 0.25f, -lim, lim); break;
                case Row::PosY:  nudge(t->pos.y, 0.25f, -lim, lim); break;
                case Row::SizeA: nudge(t->trunkRadius, 0.05f, 0.1f, 2.5f); break;
                case Row::SizeB: nudge(t->trunkHeight, 0.25f, 0.5f, 12.0f); break;
                case Row::SizeC: nudge(t->canopyRadius, 0.1f, 0.3f, 6.0f); break;
                case Row::SizeD: nudge(t->canopyHeight, 0.25f, 0.5f, 8.0f); break;
                default: break;
            }
        } else if (Boulder* b = menuBoulder(scene)) {
            switch (r) {
                case Row::PosX:  nudge(b->center.x, 0.25f, -lim, lim); break;
                case Row::PosY:  nudge(b->center.y, 0.25f, -lim, lim); break;
                case Row::PosZ:  nudge(b->center.z, 0.25f, -2.0f, 10.0f); break;
                case Row::SizeA: nudge(b->half.x, 0.05f, 0.1f, 5.0f); break;
                case Row::SizeB: nudge(b->half.y, 0.05f, 0.1f, 5.0f); break;
                case Row::SizeC: nudge(b->half.z, 0.05f, 0.1f, 5.0f); break;
                default: break;
            }
        } else if (Emitter* e = menuEmitter(scene)) {
            switch (r) {
                case Row::PosX: nudge(e->pos.x, 0.25f, -lim, lim); break;
                case Row::PosY: nudge(e->pos.y, 0.25f, -lim, lim); break;
                case Row::PosZ: nudge(e->pos.z, 0.25f, 0.05f, 30.0f); break;
                default: break;
            }
        } else if (Bug* g = menuBug(scene)) {
            switch (r) {
                case Row::PosX:  nudge(g->home.x, 0.25f, -lim, lim); break;
                case Row::PosY:  nudge(g->home.y, 0.25f, -lim, lim); break;
                case Row::PosZ:  nudge(g->home.z, 0.25f, 0.5f, 10.0f); break;
                case Row::SizeA: nudge(g->wanderRadius, 0.5f, 0.5f, 15.0f); break;
                default: break;
            }
        }
        renderer.rebuildScene(scene);
        return;
    }

    if (FloorMaterial* m = menuFloor(scene)) {
        switch (r) {
            case Row::Sound:    m->soundSet = cycleSound(m->soundSet); break;
            case Row::Gain:     m->gain = std::clamp(m->gain + dir * 0.05f, 0.0f, 2.0f); break;
            case Row::PitchJit: m->pitchJitter = std::clamp(m->pitchJitter + dir * 0.01f, 0.0f, 0.5f); break;
            case Row::GainJit:  m->gainJitterDb = std::clamp(m->gainJitterDb + dir * 0.25f, 0.0f, 12.0f); break;
            case Row::Audition: auditionCurrent(scene, listener); break;
            default: break;
        }
        return;
    }

    glm::vec3 pos;
    SoundProfile* prof = menuProfile(scene, &pos);
    if (prof == nullptr) return;
    SoundRule& rule = prof->rule(slot_);
    switch (r) {
        case Row::Slot: {
            int s = static_cast<int>(slot_) + dir;
            if (s < 0) s = kSoundSlotCount - 1;
            if (s >= kSoundSlotCount) s = 0;
            slot_ = static_cast<SoundSlot>(s);
            row_ = 0;  // row list changes shape
            break;
        }
        case Row::Sound:
            rule.soundSet = cycleSound(rule.soundSet);
            // One-shot slots: audition while cycling so browsing is audible.
            if (slot_ != SoundSlot::Loop && rule.soundSet >= 0)
                director_->audition(rule, pos, listener);
            break;
        case Row::Enabled:     rule.enabled = !rule.enabled; break;
        case Row::Gain:        rule.gain = std::clamp(rule.gain + dir * 0.05f, 0.0f, 2.0f); break;
        case Row::AudibleRadius:
            rule.audibleRadius = std::clamp(rule.audibleRadius + dir * 1.0f, 2.0f, 150.0f);
            break;
        case Row::Verb:        rule.verbWet = std::clamp(rule.verbWet + dir * 0.01f, 0.0f, 1.0f); break;
        case Row::Doppler:     rule.doppler = !rule.doppler; break;
        case Row::DopplerScale:
            rule.dopplerScale = std::clamp(rule.dopplerScale + dir * 0.25f, 0.25f, 8.0f);
            break;
        case Row::MinInterval:
            rule.minInterval = std::clamp(rule.minInterval + dir * 0.5f, 0.5f, 600.0f);
            rule.maxInterval = std::max(rule.maxInterval, rule.minInterval);
            break;
        case Row::MaxInterval:
            rule.maxInterval = std::clamp(rule.maxInterval + dir * 0.5f, 0.5f, 600.0f);
            rule.minInterval = std::min(rule.minInterval, rule.maxInterval);
            break;
        case Row::Radius:      rule.radius = std::clamp(rule.radius + dir * 0.5f, 0.5f, 30.0f); break;
        case Row::Cooldown:    rule.cooldown = std::clamp(rule.cooldown + dir * 0.25f, 0.0f, 30.0f); break;
        case Row::PitchJit:    rule.pitchJitter = std::clamp(rule.pitchJitter + dir * 0.01f, 0.0f, 0.5f); break;
        case Row::GainJit:     rule.gainJitterDb = std::clamp(rule.gainJitterDb + dir * 0.25f, 0.0f, 12.0f); break;
        case Row::Audition:    auditionCurrent(scene, listener); break;
    }
}

void DevMode::auditionCurrent(Scene& scene, const ListenerPose& listener) {
    if (const FloorMaterial* m = menuFloor(scene)) {
        SoundRule r;
        r.soundSet = m->soundSet;
        r.gain = m->gain;
        r.doppler = false;
        r.pitchJitter = m->pitchJitter;
        r.gainJitterDb = m->gainJitterDb;
        director_->audition(r, { listener.pos.x, listener.pos.y, 0.0f }, listener);
        return;
    }
    glm::vec3 pos;
    if (SoundProfile* prof = menuProfile(scene, &pos)) {
        if (slot_ == SoundSlot::Loop) return;  // already audible while assigned
        director_->audition(prof->rule(slot_), pos, listener);
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

bool DevMode::handleKey(int key, bool repeat, Scene& scene, Player& player,
                        const ListenerPose& listener, Renderer& renderer) {
    if (!enabled) return false;

    // Keys shared by every state (no repeats).
    if (!repeat) {
        switch (key) {
            case SDLK_V:
                player.flying = !player.flying;
                std::printf("[dev] fly: %s\n", player.flying ? "ON" : "off");
                return true;
            case SDLK_U: {
                const int n = lib_->rescan();
                std::printf("[dev] rescan: %d new file variant(s), %d sets total\n",
                            n, lib_->count());
                return true;
            }
            case SDLK_C:
                if (saveScene(scene, *lib_, *tuning_, scenePath_))
                    std::printf("[dev] scene saved to %s\n", scenePath_.c_str());
                else
                    std::printf("[dev] FAILED to save %s\n", scenePath_.c_str());
                return true;
            case SDLK_P:
                std::printf("[dev] player: pos (%.2f, %.2f, %.2f)  yaw %.3f  pitch %.3f\n",
                            player.pos.x, player.pos.y, player.eyeZ, player.yaw, player.pitch);
                return true;
            default:
                break;
        }
    }

    switch (state_) {
        case State::Inspect:
            if (repeat) return false;
            switch (key) {
                case SDLK_E:
                case SDLK_RETURN:
                    openMenu(scene);
                    return true;
                case SDLK_X:
                    deleteTarget(scene, renderer);
                    return true;
                case SDLK_B:
                    state_ = State::Place;
                    return true;
                case SDLK_T:
                    state_ = State::Paint;
                    hasRectCorner_ = false;
                    paintMat_ = std::clamp(paintMat_, 0,
                                           static_cast<int>(scene.materials.size()) - 1);
                    return true;
                case SDLK_O:
                    state_ = State::Worlds;
                    refreshWorldList();
                    worldMsg_.clear();
                    return true;
                case SDLK_L:
                    loadWorld(scenePath_, scene, renderer);
                    return true;
                case SDLK_G:
                    state_ = State::EngineMenu;
                    engCat_ = -1;
                    engRow_ = 0;
                    return true;
                default:
                    return false;
            }

        case State::Place:
            switch (key) {
                case SDLK_ESCAPE:
                case SDLK_B:
                    if (!repeat) state_ = State::Inspect;
                    return true;
                case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4:
                    type_ = static_cast<PlaceType>(key - SDLK_1);
                    return true;
                case SDLK_R:
                    dist_ = std::clamp(dist_ - 0.5f, kMinDist, kMaxDist);
                    return true;
                case SDLK_F:
                    dist_ = std::clamp(dist_ + 0.5f, kMinDist, kMaxDist);
                    return true;
                case SDLK_Q:
                    heightOffset_ = std::clamp(heightOffset_ - 0.25f, -10.0f, 15.0f);
                    return true;
                case SDLK_E:
                    heightOffset_ = std::clamp(heightOffset_ + 0.25f, -10.0f, 15.0f);
                    return true;
                case SDLK_LEFTBRACKET:
                case SDLK_RIGHTBRACKET: {
                    const int dir = (key == SDLK_LEFTBRACKET) ? -1 : 1;
                    const int n = lib_->count();
                    placeSound_ += dir;
                    if (placeSound_ < 0) placeSound_ = n - 1;
                    if (placeSound_ >= n) placeSound_ = 0;
                    return true;
                }
                case SDLK_RETURN:
                    if (!repeat) place(scene, player, renderer);
                    return true;
                default:
                    return false;
            }

        case State::Paint:
            if (repeat && key != SDLK_R && key != SDLK_F && key != SDLK_RETURN)
                return true;
            return handlePaintKey(key, scene, player, listener, renderer);

        case State::Worlds:
            if (repeat) return true;
            return handleWorldsKey(key, scene, renderer);

        case State::Menu: {
            const std::vector<Row> rows = menuRows(scene);
            if (rows.empty()) { closeMenu(); return false; }
            const int rowCount = static_cast<int>(rows.size());
            row_ = std::clamp(row_, 0, rowCount - 1);
            switch (key) {
                case SDLK_ESCAPE:
                case SDLK_E:
                    if (!repeat) closeMenu();
                    return true;
                case SDLK_UP:
                    row_ = (row_ + rowCount - 1) % rowCount;
                    return true;
                case SDLK_DOWN:
                    row_ = (row_ + 1) % rowCount;
                    return true;
                case SDLK_LEFT:
                    adjustRow(scene, rows[static_cast<size_t>(row_)], -1, listener, renderer);
                    return true;
                case SDLK_RIGHT:
                    adjustRow(scene, rows[static_cast<size_t>(row_)], +1, listener, renderer);
                    return true;
                case SDLK_RETURN: {
                    const Row r = rows[static_cast<size_t>(row_)];
                    if (r == Row::Audition || r == Row::Enabled || r == Row::Doppler ||
                        r == Row::Page)
                        adjustRow(scene, r, +1, listener, renderer);
                    return true;
                }
                case SDLK_B:
                    if (!repeat) auditionCurrent(scene, listener);
                    return true;
                default:
                    // Swallow leftover keys so place/inspect bindings don't
                    // fire under the menu (movement is separate scan-state).
                    return false;
            }
        }

        case State::EngineMenu:
            return handleEngineKey(key);
    }
    return false;
}

std::vector<int> DevMode::engineCategoryParams(int category) const {
    std::vector<int> out;
    for (int i = 0; i < EP_Count; ++i)
        if (kEngineParams[i].category == category) out.push_back(i);
    return out;
}

bool DevMode::handleEngineKey(int key) {
    if (engCat_ < 0) {
        // Category list.
        switch (key) {
            case SDLK_UP:
                engRow_ = (engRow_ + kEngineCategoryCount - 1) % kEngineCategoryCount;
                return true;
            case SDLK_DOWN:
                engRow_ = (engRow_ + 1) % kEngineCategoryCount;
                return true;
            case SDLK_RETURN:
            case SDLK_RIGHT:
                engCat_ = engRow_;
                engRow_ = 0;
                return true;
            case SDLK_ESCAPE:
            case SDLK_G:
                state_ = State::Inspect;
                return true;
            default:
                return false;
        }
    }

    const std::vector<int> params = engineCategoryParams(engCat_);
    const int n = static_cast<int>(params.size());
    engRow_ = std::clamp(engRow_, 0, n - 1);
    const int id = params[static_cast<size_t>(engRow_)];
    switch (key) {
        case SDLK_UP:
            engRow_ = (engRow_ + n - 1) % n;
            return true;
        case SDLK_DOWN:
            engRow_ = (engRow_ + 1) % n;
            return true;
        case SDLK_LEFT:
            tuning_->adjust(id, -1);
            return true;
        case SDLK_RIGHT:
            tuning_->adjust(id, +1);
            return true;
        case SDLK_RETURN:
            if (kEngineParams[id].toggle) tuning_->adjust(id, +1);
            return true;
        case SDLK_X:
            tuning_->reset(id);
            return true;
        case SDLK_ESCAPE:
            engRow_ = engCat_;   // land back on the category we came from
            engCat_ = -1;
            return true;
        case SDLK_G:
            state_ = State::Inspect;
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Paint mode
// ---------------------------------------------------------------------------

void DevMode::paintBrush(Scene& scene, const Player& player, Renderer& renderer) {
    glm::vec2 g;
    if (!aimGround(player, g)) return;
    scene.paintCircle(g.x, g.y, brushRadius_, paintMat_);
    renderer.rebuildFloor(scene);
}

bool DevMode::handlePaintKey(int key, Scene& scene, const Player& player,
                             const ListenerPose& /*listener*/, Renderer& renderer) {
    const int matCount = static_cast<int>(scene.materials.size());
    switch (key) {
        case SDLK_ESCAPE:
        case SDLK_T:
            state_ = State::Inspect;
            hasRectCorner_ = false;
            return true;
        case SDLK_R:
            brushRadius_ = std::clamp(brushRadius_ - 0.5f, 0.5f, 10.0f);
            return true;
        case SDLK_F:
            brushRadius_ = std::clamp(brushRadius_ + 0.5f, 0.5f, 10.0f);
            return true;
        case SDLK_LEFTBRACKET:
        case SDLK_RIGHTBRACKET: {
            const int dir = (key == SDLK_LEFTBRACKET) ? -1 : 1;
            paintMat_ = (paintMat_ + dir + matCount) % matCount;
            return true;
        }
        case SDLK_RETURN:
            paintBrush(scene, player, renderer);
            return true;
        case SDLK_K: {
            // Rect paving: first K marks a corner, second K fills the rect
            // spanned to the current aim point.
            glm::vec2 g;
            if (!aimGround(player, g)) return true;
            if (!hasRectCorner_) {
                rectCorner_ = g;
                hasRectCorner_ = true;
            } else {
                scene.paintRect(rectCorner_, g, paintMat_);
                renderer.rebuildFloor(scene);
                hasRectCorner_ = false;
                const float area = std::abs(g.x - rectCorner_.x) * std::abs(g.y - rectCorner_.y);
                std::printf("[dev] paved %.1f m2 rect with %s\n", area,
                            scene.materials[static_cast<size_t>(paintMat_)].name.c_str());
            }
            return true;
        }
        case SDLK_X: {
            // Eyedropper.
            glm::vec2 g;
            if (aimGround(player, g)) paintMat_ = scene.materialAt(g.x, g.y);
            return true;
        }
        case SDLK_N: {
            // New material, cloned from the selected one; rename by editing
            // the scene file (the overlay has no text input).
            if (matCount >= 255) return true;
            FloorMaterial m = scene.materials[static_cast<size_t>(paintMat_)];
            m.name = "mat" + std::to_string(matCount);
            scene.materials.push_back(std::move(m));
            paintMat_ = matCount;
            std::printf("[dev] added floor material '%s' (edit with E)\n",
                        scene.materials.back().name.c_str());
            return true;
        }
        case SDLK_E:
            // Edit the selected material's footstep sound/gain/jitter.
            menuTarget_ = {};
            menuTarget_.kind = TargetKind::Floor;
            menuTarget_.material = paintMat_;
            state_ = State::Menu;
            menuFromPaint_ = true;
            row_ = 0;
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Worlds (save/load browser)
// ---------------------------------------------------------------------------

std::string DevMode::worldsDir() const {
    return (std::filesystem::path(scenePath_).parent_path() / "worlds").string();
}

void DevMode::refreshWorldList() {
    worldFiles_.clear();
    worldFiles_.push_back(scenePath_);
    std::vector<std::string> extra;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(worldsDir(), ec))
        if (e.path().extension() == ".txt") extra.push_back(e.path().string());
    std::sort(extra.begin(), extra.end());
    worldFiles_.insert(worldFiles_.end(), extra.begin(), extra.end());
    worldRow_ = std::clamp(worldRow_, 0, static_cast<int>(worldFiles_.size()) - 1);
}

void DevMode::loadWorld(const std::string& path, Scene& scene, Renderer& renderer) {
    const std::string name = std::filesystem::path(path).filename().string();
    if (!loadScene(scene, *lib_, *tuning_, path)) {
        worldMsg_ = "LOAD FAILED: " + name;
        std::printf("[dev] %s\n", worldMsg_.c_str());
        return;
    }
    // SoundDirector sweeps the old objects' loop voices next frame; geometry
    // and floor re-upload here.
    target_ = {};
    menuTarget_ = {};
    paintMat_ = std::clamp(paintMat_, 0, static_cast<int>(scene.materials.size()) - 1);
    renderer.rebuildScene(scene);
    worldMsg_ = "LOADED " + name;
    std::printf("[dev] loaded world %s\n", path.c_str());
}

void DevMode::saveWorld(const std::string& path, const Scene& scene) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string name = std::filesystem::path(path).filename().string();
    if (saveScene(scene, *lib_, *tuning_, path)) {
        worldMsg_ = "SAVED " + name;
        std::printf("[dev] saved world %s\n", path.c_str());
    } else {
        worldMsg_ = "SAVE FAILED: " + name;
        std::printf("[dev] %s\n", worldMsg_.c_str());
    }
}

bool DevMode::handleWorldsKey(int key, Scene& scene, Renderer& renderer) {
    const int n = static_cast<int>(worldFiles_.size());
    switch (key) {
        case SDLK_ESCAPE:
        case SDLK_O:
            state_ = State::Inspect;
            return true;
        case SDLK_UP:
            worldRow_ = (worldRow_ + n - 1) % n;
            return true;
        case SDLK_DOWN:
            worldRow_ = (worldRow_ + 1) % n;
            return true;
        case SDLK_RETURN:
            loadWorld(worldFiles_[static_cast<size_t>(worldRow_)], scene, renderer);
            return true;
        case SDLK_S:
            saveWorld(worldFiles_[static_cast<size_t>(worldRow_)], scene);
            return true;
        case SDLK_N: {
            // First free worlds/world_<k>.txt slot.
            const std::filesystem::path dir = worldsDir();
            std::string path;
            for (int k = 1; k < 1000; ++k) {
                path = (dir / ("world_" + std::to_string(k) + ".txt")).string();
                std::error_code ec;
                if (!std::filesystem::exists(path, ec)) break;
            }
            saveWorld(path, scene);
            refreshWorldList();
            for (int i = 0; i < static_cast<int>(worldFiles_.size()); ++i)
                if (worldFiles_[static_cast<size_t>(i)] == path) worldRow_ = i;
            return true;
        }
        default:
            return false;
    }
}

void DevMode::handleWheel(float dy, Scene& scene, const ListenerPose& listener,
                          Renderer& renderer) {
    if (!enabled) return;
    if (state_ == State::Place) {
        dist_ = std::clamp(dist_ + dy * 0.5f, kMinDist, kMaxDist);
    } else if (state_ == State::Paint) {
        brushRadius_ = std::clamp(brushRadius_ + dy * 0.5f, 0.5f, 10.0f);
    } else if (state_ == State::Menu) {
        const std::vector<Row> rows = menuRows(scene);
        if (!rows.empty()) {
            row_ = std::clamp(row_, 0, static_cast<int>(rows.size()) - 1);
            adjustRow(scene, rows[static_cast<size_t>(row_)], dy > 0 ? 1 : -1,
                      listener, renderer);
        }
    } else if (state_ == State::EngineMenu && engCat_ >= 0) {
        const std::vector<int> params = engineCategoryParams(engCat_);
        if (!params.empty()) {
            engRow_ = std::clamp(engRow_, 0, static_cast<int>(params.size()) - 1);
            tuning_->adjust(params[static_cast<size_t>(engRow_)], dy > 0 ? 1 : -1);
        }
    }
}

void DevMode::handleClick(Scene& scene, const Player& player, Renderer& renderer) {
    if (!enabled) return;
    if (state_ == State::Place)
        place(scene, player, renderer);
    else if (state_ == State::Paint)
        paintBrush(scene, player, renderer);
    else if (state_ == State::Inspect)
        openMenu(scene);
}

// ---------------------------------------------------------------------------
// Place / delete
// ---------------------------------------------------------------------------

void DevMode::place(Scene& scene, const Player& player, Renderer& renderer) {
    const glm::vec3 p = aimPoint(player);

    switch (type_) {
        case PlaceType::Tree: {
            Tree t;
            t.pos = { p.x, p.y };
            t.id = scene.takeId();
            t.sounds.rule(SoundSlot::Impact).soundSet = lib_->find("knock");
            t.sounds.rule(SoundSlot::Impact).gain = 0.55f;
            t.sounds.rule(SoundSlot::Impact).doppler = false;
            scene.trees.push_back(std::move(t));
            std::printf("[dev] placed tree #%u at (%.2f, %.2f)\n", scene.trees.back().id, p.x, p.y);
            break;
        }
        case PlaceType::Boulder: {
            Boulder b;
            b.center = { p.x, p.y, 0.45f };
            b.half = { 0.55f, 0.5f, 0.45f };
            b.id = scene.takeId();
            b.sounds.rule(SoundSlot::Impact).soundSet = lib_->find("thud");
            b.sounds.rule(SoundSlot::Impact).gain = 0.6f;
            b.sounds.rule(SoundSlot::Impact).doppler = false;
            scene.boulders.push_back(std::move(b));
            std::printf("[dev] placed boulder #%u at (%.2f, %.2f)\n", scene.boulders.back().id, p.x, p.y);
            break;
        }
        case PlaceType::Emitter: {
            Emitter e;
            e.pos = { p.x, p.y, std::max(p.z, 0.1f) };
            e.id = scene.takeId();
            SoundRule& loop = e.sounds.rule(SoundSlot::Loop);
            loop.soundSet = placeSound_;
            loop.gain = 0.8f;
            loop.verbWet = 0.05f;
            scene.emitters.push_back(e);
            std::printf("[dev] placed emitter #%u (%s) at (%.2f, %.2f, %.2f)\n",
                        e.id, lib_->nameOf(placeSound_), e.pos.x, e.pos.y, e.pos.z);
            break;
        }
        case PlaceType::Bug: {
            Bug b;
            b.home = { p.x, p.y, std::clamp(p.z, 0.8f, 6.0f) };
            b.pos = b.home;
            b.id = scene.takeId();
            b.sounds.rule(SoundSlot::Loop) = makeBugLoopRule(*lib_);
            scene.bugs.push_back(b);
            std::printf("[dev] placed bug #%u home (%.2f, %.2f, %.2f)\n",
                        b.id, b.home.x, b.home.y, b.home.z);
            break;
        }
        default:
            return;
    }
    renderer.rebuildScene(scene);
}

void DevMode::deleteTarget(Scene& scene, Renderer& renderer) {
    // Loop voices of deleted objects are freed by SoundDirector's sweep on
    // the next frame.
    switch (target_.kind) {
        case TargetKind::Tree:
            for (size_t i = 0; i < scene.trees.size(); ++i)
                if (scene.trees[i].id == target_.id) {
                    std::printf("[dev] deleted tree #%u\n", target_.id);
                    scene.trees.erase(scene.trees.begin() + static_cast<ptrdiff_t>(i));
                    break;
                }
            break;
        case TargetKind::Boulder:
            for (size_t i = 0; i < scene.boulders.size(); ++i)
                if (scene.boulders[i].id == target_.id) {
                    std::printf("[dev] deleted boulder #%u\n", target_.id);
                    scene.boulders.erase(scene.boulders.begin() + static_cast<ptrdiff_t>(i));
                    break;
                }
            break;
        case TargetKind::Emitter:
            for (size_t i = 0; i < scene.emitters.size(); ++i)
                if (scene.emitters[i].id == target_.id) {
                    std::printf("[dev] deleted emitter #%u\n", target_.id);
                    scene.emitters.erase(scene.emitters.begin() + static_cast<ptrdiff_t>(i));
                    break;
                }
            break;
        case TargetKind::Bug:
            for (size_t i = 0; i < scene.bugs.size(); ++i)
                if (scene.bugs[i].id == target_.id) {
                    std::printf("[dev] deleted bug #%u\n", target_.id);
                    scene.bugs.erase(scene.bugs.begin() + static_cast<ptrdiff_t>(i));
                    break;
                }
            break;
        default:
            return;
    }
    target_ = {};
    renderer.rebuildScene(scene);
}

// ---------------------------------------------------------------------------
// Overlay
// ---------------------------------------------------------------------------

const char* DevMode::targetName(const Scene& scene, const Target& t, char* buf, size_t n) const {
    switch (t.kind) {
        case TargetKind::Tree:
            std::snprintf(buf, n, "TREE #%u", t.id);
            return buf;
        case TargetKind::Boulder:
            std::snprintf(buf, n, "BOULDER #%u", t.id);
            return buf;
        case TargetKind::Emitter: {
            const char* sound = "none";
            for (const Emitter& e : scene.emitters)
                if (e.id == t.id) sound = lib_->nameOf(e.sounds.rule(SoundSlot::Loop).soundSet);
            std::snprintf(buf, n, "EMITTER #%u (%s)", t.id, sound);
            return buf;
        }
        case TargetKind::Bug: {
            const char* sound = "none";
            for (const Bug& b : scene.bugs)
                if (b.id == t.id) sound = lib_->nameOf(b.sounds.rule(SoundSlot::Loop).soundSet);
            std::snprintf(buf, n, "BUG #%u (%s)", t.id, sound);
            return buf;
        }
        case TargetKind::Floor: {
            const size_t i = static_cast<size_t>(t.material);
            std::snprintf(buf, n, "FLOOR: %s",
                          i < scene.materials.size() ? scene.materials[i].name.c_str() : "?");
            return buf;
        }
        default:
            std::snprintf(buf, n, "NOTHING");
            return buf;
    }
}

void DevMode::buildOverlay(const Scene& scene, int fbWidth, int fbHeight,
                           std::vector<OverlayRect>& rects,
                           std::vector<OverlayText>& texts) const {
    if (!enabled) return;
    const float s = 2.0f;                      // font scale
    const float lh = Renderer::textHeight(s) + 4.0f;  // line height
    const float pad = 10.0f;

    auto panel = [&](float x, float y, float w, float h) {
        rects.push_back({ x, y, w, h, kPanel });
    };
    auto line = [&](float x, float& y, const std::string& str, const glm::vec4& c) {
        texts.push_back({ x, y, s, c, str });
        y += lh;
    };

    // Crosshair (skip when a menu is open -- the pick is frozen anyway).
    if (state_ == State::Inspect || state_ == State::Place) {
        const float cx = static_cast<float>(fbWidth) * 0.5f;
        const float cy = static_cast<float>(fbHeight) * 0.5f;
        const glm::vec4 c{ 0.9f, 0.95f, 1.0f, 0.8f };
        rects.push_back({ cx - 7.0f, cy - 1.0f, 14.0f, 2.0f, c });
        rects.push_back({ cx - 1.0f, cy - 7.0f, 2.0f, 14.0f, c });
    }

    char buf[96];
    float y = pad + 6.0f;
    const float x = pad + 8.0f;

    switch (state_) {
        case State::Inspect: {
            panel(pad, pad, 540.0f, 8.0f * lh + 12.0f);
            line(x, y, "DEV / INSPECT", kHeader);
            line(x, y, fmt("AIM: %s", targetName(scene, target_, buf, sizeof(buf))), kTarget);
            line(x, y, "E/ENTER/LMB EDIT   X DELETE", kHint);
            line(x, y, "B PLACE   T PAINT FLOOR   G ENGINE", kHint);
            line(x, y, "O WORLDS   L RELOAD   C SAVE SCENE", kHint);
            line(x, y, "V FLY   U RESCAN SOUNDS   P PRINT POS", kHint);
            line(x, y, "F1 STATS   F3 VISUAL   F2 EXIT DEV", kHint);
            line(x, y, fmt("SOUNDS: %d SETS", lib_->count()), kHint);
            break;
        }
        case State::Paint: {
            panel(pad, pad, 600.0f, 8.0f * lh + 12.0f);
            line(x, y, "DEV / PAINT FLOOR", kHeader);
            const FloorMaterial& m = scene.materials[static_cast<size_t>(
                std::clamp(paintMat_, 0, static_cast<int>(scene.materials.size()) - 1))];
            // Material swatch in its floor tint next to the name.
            const glm::vec3 tint = Renderer::floorColor(paintMat_);
            rects.push_back({ x, y + 2.0f, 10.0f, 10.0f, { tint.x, tint.y, tint.z, 1.0f } });
            texts.push_back({ x + 16.0f, y, s, kTarget,
                              fmt("MATERIAL: < %s >   ([ ] CYCLE)", m.name.c_str()) });
            y += lh;
            line(x, y, fmt("BRUSH %.1f M (WHEEL OR R/F)   DAB %.1f M2",
                           brushRadius_, kPi * brushRadius_ * brushRadius_), kHint);
            line(x, y, "LMB HOLD PAINT   ENTER DAB", kHint);
            line(x, y, hasRectCorner_
                     ? fmt("K PAVE RECT TO AIM (CORNER %.1f, %.1f SET)",
                           rectCorner_.x, rectCorner_.y)
                     : "K SET RECT CORNER (PAVE BY AREA)", kTarget);
            line(x, y, "X EYEDROP   N NEW MATERIAL   E EDIT MATERIAL", kHint);
            {
                // Coverage per material, m^2 (the glade is 1600 m^2 total).
                std::string cov = "AREA:";
                for (size_t i = 0; i < scene.materials.size() && i < 6; ++i)
                    cov += fmt(" %s %.0f", scene.materials[i].name.c_str(),
                               scene.areaOf(static_cast<int>(i)));
                cov += " M2";
                line(x, y, cov, kHint);
            }
            line(x, y, "ESC/T BACK TO INSPECT", kHint);
            break;
        }
        case State::Worlds: {
            const float w = 560.0f;
            const float h = (static_cast<float>(worldFiles_.size()) + 5.0f) * lh + 24.0f;
            panel(pad, pad, w, h);
            line(x, y, "DEV / WORLDS", kHeader);
            y += 4.0f;
            for (size_t i = 0; i < worldFiles_.size(); ++i) {
                const bool sel = static_cast<int>(i) == worldRow_;
                if (sel)
                    rects.push_back({ pad + 3.0f, y - 3.0f, w - 6.0f, lh, kSelectBar });
                std::string name =
                    std::filesystem::path(worldFiles_[i]).filename().string();
                if (i == 0) name += "  (ACTIVE FILE)";
                texts.push_back({ x, y, s, sel ? kSelected : kText,
                                  (sel ? "> " : "  ") + name });
                y += lh;
            }
            y += 4.0f;
            line(x, y, "ENTER LOAD   S SAVE OVER   N SAVE NEW SLOT", kHint);
            line(x, y, "ESC/O BACK", kHint);
            if (!worldMsg_.empty()) line(x, y, worldMsg_, kTarget);
            break;
        }
        case State::Place: {
            panel(pad, pad, 540.0f, 7.0f * lh + 12.0f);
            line(x, y, "DEV / PLACE", kHeader);
            const char* names[4] = { "TREE", "BOULDER", "EMITTER", "BUG" };
            const int ti = static_cast<int>(type_);
            line(x, y, fmt("TYPE: %s   (1 TREE  2 BOULDER  3 EMITTER  4 BUG)", names[ti]), kTarget);
            if (type_ == PlaceType::Emitter)
                line(x, y, fmt("EMITTER SOUND: %s   ([ ] CYCLE)", lib_->nameOf(placeSound_)), kTarget);
            else
                line(x, y, "", kHint);
            line(x, y, "LMB/ENTER PLACE AT GHOST", kHint);
            line(x, y, fmt("DIST %.1f M (WHEEL OR R/F)   HEIGHT %+.2f M (Q/E)",
                           dist_, heightOffset_), kHint);
            line(x, y, "ESC/B BACK TO INSPECT", kHint);
            line(x, y, "V FLY   C SAVE   F2 EXIT DEV", kHint);
            break;
        }
        case State::Menu: {
            // Resolve target; show a note instead of rows if it vanished.
            Scene& sc = const_cast<Scene&>(scene);
            const bool isFloor = menuTarget_.kind == TargetKind::Floor;
            const bool alive = isFloor ? menuFloor(sc) != nullptr
                                       : menuProfile(sc) != nullptr;
            const std::vector<Row> rows = menuRows(scene);
            const float w = 560.0f;
            const float h = (static_cast<float>(rows.size()) + 4.0f) * lh + 24.0f;
            panel(pad, pad, w, h);
            line(x, y, fmt("EDIT %s", targetName(scene, menuTarget_, buf, sizeof(buf))), kHeader);
            if (!alive) {
                line(x, y, "(TARGET DELETED -- ESC)", kDisabled);
                break;
            }
            y += 4.0f;
            const float valueX = x + 18.0f * 6.0f * s;  // label column ~18 chars
            for (size_t i = 0; i < rows.size(); ++i) {
                const bool sel = static_cast<int>(i) == row_;
                if (sel)
                    rects.push_back({ pad + 3.0f, y - 3.0f, w - 6.0f, lh, kSelectBar });
                const glm::vec4& c = sel ? kSelected : kText;
                texts.push_back({ x, y, s, c, (sel ? "> " : "  ") + rowLabel(rows[i]) });
                std::string v = rowValue(scene, rows[i]);
                if (rows[i] == Row::Slot || rows[i] == Row::Sound || rows[i] == Row::Page)
                    v = "< " + v + " >";
                texts.push_back({ valueX, y, s, c, v });
                y += lh;
            }
            y += 4.0f;
            line(x, y, "UP/DOWN ROW   LEFT/RIGHT (OR WHEEL) ADJUST", kHint);
            line(x, y, "ENTER TOGGLE/AUDITION   B AUDITION   ESC/E CLOSE", kHint);
            break;
        }
        case State::EngineMenu: {
            if (engCat_ < 0) {
                // Category list.
                const float w = 480.0f;
                const float h = (static_cast<float>(kEngineCategoryCount) + 3.0f) * lh + 24.0f;
                panel(pad, pad, w, h);
                line(x, y, "ENGINE PARAMS", kHeader);
                y += 4.0f;
                for (int i = 0; i < kEngineCategoryCount; ++i) {
                    const bool sel = i == engRow_;
                    if (sel)
                        rects.push_back({ pad + 3.0f, y - 3.0f, w - 6.0f, lh, kSelectBar });
                    const int changed = tuning_->changedInCategory(i);
                    std::string label = std::string(sel ? "> " : "  ") + kEngineCategories[i];
                    if (changed > 0) label += fmt("  (%d CHANGED)", changed);
                    texts.push_back({ x, y, s, sel ? kSelected : kText, label });
                    y += lh;
                }
                y += 4.0f;
                line(x, y, "UP/DOWN   ENTER/RIGHT OPEN   ESC/G BACK", kHint);
            } else {
                // Param list for the open category.
                const std::vector<int> params = engineCategoryParams(engCat_);
                const float w = 560.0f;
                const float h = (static_cast<float>(params.size()) + 4.0f) * lh + 24.0f;
                panel(pad, pad, w, h);
                line(x, y, fmt("ENGINE / %s", kEngineCategories[engCat_]), kHeader);
                y += 4.0f;
                const float valueX = x + 22.0f * 6.0f * s;  // label column ~22 chars
                for (size_t i = 0; i < params.size(); ++i) {
                    const int id = params[i];
                    const bool sel = static_cast<int>(i) == engRow_;
                    if (sel)
                        rects.push_back({ pad + 3.0f, y - 3.0f, w - 6.0f, lh, kSelectBar });
                    const glm::vec4& c = sel ? kSelected
                                             : tuning_->isDefault(id) ? kText : kTarget;
                    texts.push_back({ x, y, s, c,
                                      std::string(sel ? "> " : "  ") + kEngineParams[id].label });
                    std::string v = engineValueText(*tuning_, id);
                    if (!tuning_->isDefault(id)) v += " *";
                    texts.push_back({ valueX, y, s, c, v });
                    y += lh;
                }
                y += 4.0f;
                line(x, y, "UP/DOWN   LEFT/RIGHT (OR WHEEL) ADJUST   X RESET", kHint);
                line(x, y, "ESC CATEGORIES   G INSPECT   (* = CHANGED)", kHint);
            }
            break;
        }
    }
}
