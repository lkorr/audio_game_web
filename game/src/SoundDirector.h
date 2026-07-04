#pragma once
// Game-thread brain that turns scene SoundProfiles into AudioWorld voice
// calls. Each frame, update() walks every object (trees, boulders, emitters),
// keeps per-object state keyed by stable object id, and evaluates the slots:
//
//   Loop        starts/stops/restarts a loop voice to match the rule; any
//               config edit (dev mode) is detected by comparing the applied
//               config and applies live via restart
//   IdleRandom  counts down a randomized timer, fires a one-shot at the object
//   Proximity   edge-triggers a one-shot when the player enters the radius
//               (hysteresis re-arm at 1.2x radius + cooldown)
//   Impact      fired externally by onImpact() from player bump events
//
// Footsteps route through here too (onFootstep) using the scene's per-material
// FloorMaterial table.
//
// Extending: new behaviors (combat, entity-reacts-to-entity, scripted cues)
// add a SoundSlot, a field or two on SoundRule if needed, and either a block
// in updateObject() (polled conditions, like Proximity) or an onXxx() entry
// point (event conditions, like Impact).

#include "Scene.h"
#include "AudioWorld.h"
#include "SoundLibrary.h"
#include <glm/vec3.hpp>
#include <random>
#include <unordered_map>

class SoundDirector {
public:
    void init(AudioWorld* world, SoundLibrary* lib);

    // Once per frame, after player movement. Manages loops, idle timers,
    // proximity triggers, cooldowns; sweeps state of deleted objects.
    void update(float dt, Scene& scene, const ListenerPose& listener);

    // Footstep event (player or render test). `mat` indexes Scene::materials.
    void onFootstep(const Scene& scene, int mat, const glm::vec3& pos,
                    const ListenerPose& listener);

    // Stalker footstep one-shot (fired from updateStalkers as the predator moves).
    // `gain` carries the state's loudness -- heavier/louder when Chasing, quieter
    // when Patrolling -- so the player reads its gait. Doppler off: it is at the
    // predator's feet, its motion already legible through the drone.
    void onStalkerStep(const glm::vec3& pos, float gain, const ListenerPose& listener);

    // Stalker mood vocalization (fired from updateStalkers on state entry + on a
    // timer while in-state). `mood` selects the variant of the `stalker_call` set:
    // 0 roam, 1 hunt, 2 search, 3 confused -- so the player reads the AI's state by
    // ear. `gain` lets closer/urgent moods read louder.
    void onStalkerCall(const glm::vec3& pos, int mood, float gain,
                       const ListenerPose& listener);

    // Stalker lunge screech: the loud tell that a committed attack has begun.
    void onStalkerLunge(const glm::vec3& pos, const ListenerPose& listener);

    // Player bumped object `objectId` at `contactPos`.
    void onImpact(Scene& scene, uint32_t objectId, const glm::vec3& contactPos,
                  const ListenerPose& listener);

    // Player echo-probe ping: a bright one-shot at the listener with a heavy
    // reverb send, so the space answers by its tail (pure acoustic return). Rate
    // limited; returns true if it actually fired (a probe the Stalker can hear).
    // Advance the cooldown by calling tickPing(dt) each frame.
    bool onPing(const ListenerPose& listener);
    void tickPing(float dt) { pingCooldown_ = std::max(0.0f, pingCooldown_ - dt); }
    bool pingReady() const { return pingCooldown_ <= 0.0f; }

    // Dev mode: play a rule's sound once at a position (slot audition).
    void audition(const SoundRule& rule, const glm::vec3& pos, const ListenerPose& listener);

private:
    struct ObjState {
        int loopVoice = -1;
        // Loop config as applied to the voice; a mismatch with the rule
        // triggers a restart.
        int loopSet = -1;
        float loopGain = 0.0f, loopRadius = 0.0f, loopVerb = 0.0f;
        float loopDopplerScale = 1.0f;
        bool loopDoppler = false;
        float idleTimer = -1.0f;        // <0: not scheduled yet
        bool inProximity = false;
        float proxCooldown = 0.0f;
        float impactCooldown = 0.0f;
        bool seen = false;              // mark/sweep flag
    };

    // `revealOut` (may be null) receives this object's reveal-on-sound target
    // for the renderer: proximity-scaled while its loop is audible, pulsed to
    // 1.0 on one-shot fires. The frame loop decays the stored value toward it.
    void updateObject(float dt, uint32_t id, SoundProfile& prof, const glm::vec3& pos,
                      const ListenerPose& listener, float* revealOut = nullptr);
    // Random variant + pitch/gain jitter, then AudioWorld::playOneShot.
    bool fireOneShot(const SoundRule& rule, const glm::vec3& pos, const ListenerPose& listener);

    AudioWorld* world_ = nullptr;
    SoundLibrary* lib_ = nullptr;
    std::unordered_map<uint32_t, ObjState> states_;
    std::mt19937 rng_{0x50D17u};

    int pingSet_ = -1;          // resolved lazily on first ping
    int stalkerStepSet_ = -1;   // resolved lazily on first stalker step
    int stalkerCallSet_ = -1;   // resolved lazily on first stalker mood call
    int stalkerLungeSet_ = -1;  // resolved lazily on first stalker lunge
    float pingCooldown_ = 0.0f;
    // Ping cooldown is the live-tunable tune::kPingCooldown (tunables.txt).
};
