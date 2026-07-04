#include "SoundDirector.h"
#include <glm/geometric.hpp>
#include <algorithm>
#include <cmath>

namespace {

float frand(std::mt19937& rng, float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}

AudioWorld::VoiceConfig configFor(const SoundRule& r, float gain) {
    AudioWorld::VoiceConfig cfg;
    cfg.gain = gain;
    cfg.audibleRadius = r.audibleRadius;
    cfg.verbWet = r.verbWet;
    cfg.doppler = r.doppler;
    cfg.dopplerScale = r.dopplerScale;
    return cfg;
}

} // namespace

void SoundDirector::init(AudioWorld* world, SoundLibrary* lib) {
    world_ = world;
    lib_ = lib;
}

bool SoundDirector::fireOneShot(const SoundRule& rule, const glm::vec3& pos,
                                const ListenerPose& listener) {
    const std::vector<float>* buf = lib_->randomVariant(rule.soundSet, rng_);
    if (buf == nullptr) return false;
    const float db = frand(rng_, -rule.gainJitterDb, rule.gainJitterDb);
    const float gain = rule.gain * std::pow(10.0f, db / 20.0f);
    const double rate = 1.0 + frand(rng_, -rule.pitchJitter, rule.pitchJitter);
    return world_->playOneShot(buf, pos, rate, configFor(rule, gain), listener);
}

void SoundDirector::audition(const SoundRule& rule, const glm::vec3& pos,
                             const ListenerPose& listener) {
    if (rule.soundSet >= 0) fireOneShot(rule, pos, listener);
}

void SoundDirector::updateObject(float dt, uint32_t id, SoundProfile& prof,
                                 const glm::vec3& pos, const ListenerPose& listener,
                                 float* revealOut) {
    ObjState& st = states_[id];
    st.seen = true;
    st.proxCooldown = std::max(0.0f, st.proxCooldown - dt);
    st.impactCooldown = std::max(0.0f, st.impactCooldown - dt);

    const float dist = glm::length(pos - listener.pos);
    // Steady reveal target: while a loop is audible, glow scales with nearness
    // (louder = closer = brighter). One-shot fires below pulse it to 1.0.
    float revealTarget = 0.0f;

    // ---- Loop slot ----
    {
        const SoundRule& r = prof.rule(SoundSlot::Loop);
        // Range gate: only hold a loop voice for sources the listener can
        // actually hear. On a big map there are more loop sources than voices
        // (kLoopVoices), so distant loops must release their voice or nearby
        // sources -- including bugs -- get starved and go silent. Hysteresis
        // (keep until 1.3x the audible radius) avoids thrash at the boundary.
        const float keepRadius = r.audibleRadius * (st.loopVoice >= 0 ? 1.3f : 1.0f);
        const bool inRange = dist <= keepRadius;
        const bool want = r.wantsSound() && lib_->valid(r.soundSet) && inRange;
        const bool configChanged = st.loopVoice >= 0 &&
            (st.loopSet != r.soundSet || st.loopGain != r.gain ||
             st.loopRadius != r.audibleRadius ||
             st.loopVerb != r.verbWet || st.loopDoppler != r.doppler ||
             st.loopDopplerScale != r.dopplerScale);
        if (st.loopVoice >= 0 && (!want || configChanged)) {
            world_->stopLoop(st.loopVoice);
            st.loopVoice = -1;
            // On config change the restart lands next frame, once the faded
            // voice has freed itself.
        } else if (want && st.loopVoice < 0) {
            // Stable per-object variant + start phase so objects sharing a
            // set stay decorrelated.
            const SoundSet& set = lib_->set(r.soundSet);
            const std::vector<float>* buf =
                lib_->variant(r.soundSet, id % set.variants.size());
            const size_t offset = static_cast<size_t>(id) * 7919u * 257u;
            st.loopVoice = world_->placeLoop(buf, offset, pos,
                                             configFor(r, r.gain), listener);
            if (st.loopVoice >= 0) {
                st.loopSet = r.soundSet;
                st.loopGain = r.gain;
                st.loopRadius = r.audibleRadius;
                st.loopVerb = r.verbWet;
                st.loopDoppler = r.doppler;
                st.loopDopplerScale = r.dopplerScale;
            }
        } else if (st.loopVoice >= 0) {
            // Follow moving objects (bugs): publish the current position.
            world_->setLoopPos(st.loopVoice, pos);
        }
        // Steady glow while the loop plays, scaled by nearness within its carry.
        if (st.loopVoice >= 0 && st.loopRadius > 0.0f)
            revealTarget = std::clamp(1.0f - dist / st.loopRadius, 0.0f, 1.0f);
    }

    // ---- IdleRandom slot ----
    {
        const SoundRule& r = prof.rule(SoundSlot::IdleRandom);
        if (r.wantsSound()) {
            if (st.idleTimer < 0.0f)
                st.idleTimer = frand(rng_, 0.0f, std::max(r.minInterval, 0.1f));
            st.idleTimer -= dt;
            if (st.idleTimer <= 0.0f) {
                // Skip when out of this sound's own audible range.
                if (dist <= r.audibleRadius) {
                    fireOneShot(r, pos, listener);
                    revealTarget = 1.0f;   // blip of light synced to the chirp
                }
                st.idleTimer = frand(rng_, std::max(r.minInterval, 0.1f),
                                     std::max(r.maxInterval, r.minInterval + 0.1f));
            }
        } else {
            st.idleTimer = -1.0f;
        }
    }

    // ---- Proximity slot ----
    {
        const SoundRule& r = prof.rule(SoundSlot::Proximity);
        if (r.wantsSound()) {
            if (!st.inProximity && dist <= r.radius) {
                st.inProximity = true;
                if (st.proxCooldown <= 0.0f) {
                    fireOneShot(r, pos, listener);
                    st.proxCooldown = r.cooldown;
                    revealTarget = 1.0f;   // blip on the proximity trigger
                }
            } else if (st.inProximity && dist > r.radius * 1.2f) {
                st.inProximity = false;   // hysteresis re-arm
            }
        } else {
            st.inProximity = false;
        }
    }

    // Rise instantly to the target, decay is applied in the frame loop.
    if (revealOut != nullptr) *revealOut = std::max(*revealOut, revealTarget);
}

void SoundDirector::update(float dt, Scene& scene, const ListenerPose& listener) {
    for (auto& [id, st] : states_) st.seen = false;

    // Bugs first: they are close-range moving sources (radius ~6 m), so give
    // them loop-voice priority over the many far-carrying static emitters --
    // otherwise a bug right next to you can lose the voice race and go silent.
    for (Bug& b : scene.bugs)
        updateObject(dt, b.id, b.sounds, b.soundPos(), listener, &b.reveal);
    for (Tree& t : scene.trees)
        updateObject(dt, t.id, t.sounds, t.soundPos(), listener, &t.reveal);
    for (Boulder& b : scene.boulders)
        updateObject(dt, b.id, b.sounds, b.soundPos(), listener, &b.reveal);
    for (Emitter& e : scene.emitters)
        updateObject(dt, e.id, e.sounds, e.soundPos(), listener, &e.reveal);
    // Stalkers move every frame (like bugs): their loop voice follows pos, so
    // the player hears the predator drone Doppler and occlude as it hunts.
    for (Stalker& s : scene.stalkers)
        updateObject(dt, s.id, s.sounds, s.soundPos(), listener, &s.reveal);

    // Sweep: free loop voices of objects deleted since last frame.
    for (auto it = states_.begin(); it != states_.end();) {
        if (!it->second.seen) {
            if (it->second.loopVoice >= 0) world_->stopLoop(it->second.loopVoice);
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
}

void SoundDirector::onFootstep(const Scene& scene, int mat, const glm::vec3& pos,
                               const ListenerPose& listener) {
    const size_t mi = static_cast<size_t>(mat);
    if (mat < 0 || mi >= scene.materials.size()) return;
    const FloorMaterial& m = scene.materials[mi];

    const std::vector<float>* buf = lib_->randomVariant(m.soundSet, rng_);
    if (buf == nullptr) return;
    const float db = frand(rng_, -m.gainJitterDb, m.gainJitterDb);
    const float gain = m.gain * std::pow(10.0f, db / 20.0f);
    const double rate = 1.0 + frand(rng_, -m.pitchJitter, m.pitchJitter);
    // Steps are at the feet: default radius, dry, no propagation delay.
    AudioWorld::VoiceConfig cfg;
    cfg.gain = gain;
    cfg.doppler = false;
    world_->playOneShot(buf, pos, rate, cfg, listener);
}

void SoundDirector::onStalkerStep(const glm::vec3& pos, float gain,
                                  const ListenerPose& listener) {
    if (stalkerStepSet_ < 0) stalkerStepSet_ = lib_->find("stalker_step");
    const std::vector<float>* buf = lib_->randomVariant(stalkerStepSet_, rng_);
    if (buf == nullptr) return;
    // A little pitch/gain jitter so the gait does not sound machine-regular.
    const float db = frand(rng_, -1.5f, 1.5f);
    const float g = gain * std::pow(10.0f, db / 20.0f);
    const double rate = 1.0 + frand(rng_, -0.05f, 0.05f);
    // At the predator's feet: doppler off, dry (it is not a room probe). Carries
    // as far as the drone (30 m) so you can track the hunter by its gait across
    // the map, not just when it is nearly on top of you.
    AudioWorld::VoiceConfig cfg;
    cfg.gain = g;
    cfg.audibleRadius = 32.0f;
    cfg.doppler = false;
    world_->playOneShot(buf, pos, rate, cfg, listener);
}

void SoundDirector::onStalkerCall(const glm::vec3& pos, int mood, float gain,
                                  const ListenerPose& listener) {
    if (stalkerCallSet_ < 0) stalkerCallSet_ = lib_->find("stalker_call");
    // mood indexes a specific variant (0 roam, 1 hunt, 2 search, 3 confused) so
    // each AI state has its own recognizable voice -- NOT a random pick.
    const std::vector<float>* buf = lib_->variant(stalkerCallSet_, static_cast<size_t>(mood));
    if (buf == nullptr) return;
    const float db = frand(rng_, -1.0f, 1.0f);
    const float g = gain * std::pow(10.0f, db / 20.0f);
    const double rate = 1.0 + frand(rng_, -0.03f, 0.03f);
    // Carries far (the player should read the mood across the map) and takes a
    // little room reverb; doppler on so a moving call still shifts with motion.
    AudioWorld::VoiceConfig cfg;
    cfg.gain = g;
    cfg.audibleRadius = 34.0f;
    cfg.verbWet = 0.2f;
    cfg.doppler = true;
    world_->playOneShot(buf, pos, rate, cfg, listener);
}

void SoundDirector::onStalkerLunge(const glm::vec3& pos, const ListenerPose& listener) {
    if (stalkerLungeSet_ < 0) stalkerLungeSet_ = lib_->find("stalker_lunge");
    const std::vector<float>* buf = lib_->randomVariant(stalkerLungeSet_, rng_);
    if (buf == nullptr) return;
    const double rate = 1.0 + frand(rng_, -0.04f, 0.04f);
    // The loud attack tell: cuts over the drone, carries far, slight reverb edge.
    AudioWorld::VoiceConfig cfg;
    cfg.gain = 1.0f;
    cfg.audibleRadius = 38.0f;
    cfg.verbWet = 0.25f;
    cfg.doppler = true;
    world_->playOneShot(buf, pos, rate, cfg, listener);
}

bool SoundDirector::onPing(const ListenerPose& listener) {
    if (pingCooldown_ > 0.0f) return false;
    if (pingSet_ < 0) pingSet_ = lib_->find("ping");
    const std::vector<float>* buf = lib_->randomVariant(pingSet_, rng_);
    if (buf == nullptr) return false;

    // Emitted from the listener position: its value is the reverb/echo that
    // comes back, so where it "is" matters less than what the room does to it.
    const glm::vec3 pos = listener.pos;
    AudioWorld::VoiceConfig cfg;
    cfg.gain = 0.9f;
    cfg.audibleRadius = 40.0f;   // carries: it is a loud, deliberate probe
    cfg.verbWet = 0.85f;         // the point of the ping is its reverb return
    cfg.doppler = false;
    if (!world_->playOneShot(buf, pos, 1.0, cfg, listener)) return false;
    pingCooldown_ = kPingCooldown;
    return true;
}

void SoundDirector::onImpact(Scene& scene, uint32_t objectId, const glm::vec3& contactPos,
                             const ListenerPose& listener) {
    SoundProfile* prof = nullptr;
    float* reveal = nullptr;
    for (Tree& t : scene.trees)
        if (t.id == objectId) { prof = &t.sounds; reveal = &t.reveal; break; }
    if (prof == nullptr)
        for (Boulder& b : scene.boulders)
            if (b.id == objectId) { prof = &b.sounds; reveal = &b.reveal; break; }
    if (prof == nullptr) return;

    const SoundRule& r = prof->rule(SoundSlot::Impact);
    if (!r.wantsSound()) return;

    ObjState& st = states_[objectId];
    if (st.impactCooldown > 0.0f) return;
    if (fireOneShot(r, contactPos, listener)) {
        st.impactCooldown = r.cooldown;
        if (reveal != nullptr) *reveal = 1.0f;   // flash the object on impact
    }
}
