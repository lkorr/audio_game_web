#pragma once
// Hardcoded world: a ~40x40 m glade. Engine/game coordinate convention:
// X = right, Y = forward (north), Z = up. Ground at z = 0. World meters.
//
// Every placeable object carries a SoundProfile: a fixed array of SoundRules,
// one per SoundSlot. Slots are *when* a sound plays; the rule is *what* plays
// and how. Adding a new behavior (combat, reactions to other entities, ...) is
// an enum entry here plus a handler in SoundDirector -- the rule struct
// already carries the generic knobs (radius, cooldown, intervals, jitter).

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// Ground materials are indices into Scene::materials (dynamic; dev mode can
// add entries). 0 = grass, 1 = stone in the built-in scene.

// When a rule fires:
//   Loop        continuous loop while enabled (ambience: water, wind, drones)
//   IdleRandom  one-shot every rand[minInterval, maxInterval] seconds
//   Proximity   one-shot when the player enters `radius` (re-arms on leaving,
//               rate-limited by `cooldown`)
//   Impact      one-shot when the player bumps the object's collider,
//               rate-limited by `cooldown`
enum class SoundSlot : int { Loop = 0, IdleRandom, Proximity, Impact, kCount };
constexpr int kSoundSlotCount = static_cast<int>(SoundSlot::kCount);

const char* soundSlotName(SoundSlot s);

struct SoundRule {
    int soundSet = -1;       // SoundLibrary set id, -1 = unassigned
    bool enabled = true;
    float gain = 0.8f;
    // Engine sphere-of-influence radius for this sound, meters: distance gain
    // hits the floor at this range, so it is effectively the sound's size /
    // carry. Per object per slot.
    float audibleRadius = 35.0f;
    float verbWet = 0.0f;
    bool doppler = true;
    // Doppler exaggeration: scales the propagation-delay slope, so pitch
    // shift from motion is multiplied by this. 1 = physical.
    float dopplerScale = 1.0f;
    float minInterval = 6.0f;   // IdleRandom
    float maxInterval = 16.0f;  // IdleRandom
    float radius = 2.5f;        // Proximity trigger distance, meters
    float cooldown = 0.8f;      // Proximity/Impact re-trigger guard, seconds
    float pitchJitter = 0.04f;  // one-shot playback rate spread (+-)
    float gainJitterDb = 1.5f;  // one-shot level spread (+- dB)

    bool wantsSound() const { return enabled && soundSet >= 0; }
};

struct SoundProfile {
    SoundRule rules[kSoundSlotCount];

    SoundRule& rule(SoundSlot s) { return rules[static_cast<int>(s)]; }
    const SoundRule& rule(SoundSlot s) const { return rules[static_cast<int>(s)]; }
};

struct Tree {
    glm::vec2 pos{0.0f};  // trunk center
    float trunkRadius = 0.4f;
    float trunkHeight = 2.0f;
    float canopyRadius = 1.6f;
    float canopyHeight = 2.6f;   // cone height above the trunk
    float flash = 0.0f;          // bump flash intensity, game thread
    glm::vec3 flashPos{0.0f};    // world-space contact point of the last bump
    // Reveal-on-sound glow, 0..1, game thread. Driven by SoundDirector from
    // loop activity + proximity (+ one-shot pulses), decayed in the frame loop.
    // The renderer reads it so objects emerge from the dark when they sound.
    // Also a general "this object is currently audible" signal for game logic.
    float reveal = 0.0f;
    uint32_t id = 0;             // stable object id (SoundDirector state key)
    SoundProfile sounds;

    glm::vec3 soundPos() const { return { pos.x, pos.y, 1.4f }; }
};

struct Boulder {
    glm::vec3 center{0.0f};  // AABB center
    glm::vec3 half{0.5f};    // AABB half extents
    float flash = 0.0f;
    glm::vec3 flashPos{0.0f};
    float reveal = 0.0f;     // reveal-on-sound glow (see Tree::reveal)
    uint32_t id = 0;
    SoundProfile sounds;

    glm::vec3 soundPos() const { return center; }
};

struct Emitter {
    glm::vec3 pos{0.0f};
    float reveal = 0.0f;     // reveal-on-sound glow (see Tree::reveal)
    uint32_t id = 0;
    SoundProfile sounds;

    // Music sanctuary: while this emitter's Loop is playing and the flag is set,
    // the Stalker will not enter within `musicRadius` meters of it -- the player
    // hears the music in the dark and reads it as a safe pocket. Only meaningful
    // for emitters carrying a Loop rule. See Stalker.cpp.
    bool isMusic = false;
    float musicRadius = 8.0f;

    // Playable-slice goal marker: reaching within a few meters wins the slice.
    // Inert unless a SliceGame is armed (see SliceGame.h).
    bool isGoal = false;

    glm::vec3 soundPos() const { return pos; }
};

// Flying ambient critter: wanders around `home` at roughly head height on a
// deterministic multi-sine path (updateBugs). Loop voices follow `pos` every
// frame, so doppler tracks the flight. Default sound: a small, fast saw
// drone with exaggerated doppler.
struct Bug {
    glm::vec3 home{0.0f};       // wander center (z = nominal flight height)
    float wanderRadius = 4.0f;  // horizontal wander extent, meters
    glm::vec3 pos{0.0f};        // current position (runtime)
    float t = 0.0f;             // flight time (runtime)
    float reveal = 0.0f;        // reveal-on-sound glow (see Tree::reveal)
    uint32_t id = 0;
    SoundProfile sounds;

    glm::vec3 soundPos() const { return pos; }
};

// A predator that homes on sound the player makes. It has no static position:
// it patrols, then investigates/chases the last loud thing it heard (footsteps
// -- louder on stone, loudest sprinting -- pings, impacts), its audibility to
// that sound weighted by the same occlusion the player experiences, so putting
// geometry between you and it breaks its trail. The player tracks it purely by
// its own moving loop drone (Doppler as it moves, muffling as it rounds a
// boulder). Active music emitters repel it, forming safety zones. See
// Stalker.cpp for the update; this is data only.
struct Stalker {
    // Patrol      lazy roam around the fixed home anchor (no cue) -- "roaming"
    // Investigate homing on the last-heard spot at a brisk pace
    // Chase       homing fast on a close, loud cue -- "hunting"
    // Attack      committed lunge: locks a spot and sprints THROUGH it at ~4x,
    //             not tracking mid-flight (dodge it), then Confused
    // Confused    a brief stunned pause (after a lunge, or a cue vanishing) before
    //             it gathers itself and starts to Search
    // Search      lost the trail: sweep the area AROUND lastHeard for a while,
    //             re-locking if it hears the player again, then time out to Patrol
    enum class State : int { Patrol, Investigate, Chase, Attack, Confused, Search };

    glm::vec3 pos{0.0f};        // current position (z = drone height)
    glm::vec3 home{0.0f};       // patrol anchor
    float patrolRadius = 14.0f; // how far it wanders while patrolling
    float heading = 0.0f;       // current facing (radians), for patrol drift

    State state = State::Patrol;
    State prevState = State::Patrol; // last frame's state, for on-entry sound cues
    glm::vec3 lastHeard{0.0f};  // world pos of the strongest recent sound
    float interest = 0.0f;      // loudness of that sound as heard here, 0..~1
    float alertTimer = 0.0f;    // time left before giving up and returning
    float searchTimer = 0.0f;   // time left sweeping around lastHeard (Search)
    glm::vec3 searchTarget{0.0f}; // current point being swept toward (Search)
    float stepAccum = 0.0f;     // travel since last footstep one-shot (runtime)

    // Attack lunge: on entry it locks lungeTarget = the point a few meters PAST
    // the player and drives straight to it at high speed, ignoring new cues until
    // it arrives; then it pauses (Confused) and searches. Fully committed.
    glm::vec3 lungeTarget{0.0f}; // fixed overshoot point for the current lunge
    float stateTimer = 0.0f;     // generic timer for timed states (Confused pause)
    float cueTimer = 0.0f;       // spacing between repeated in-state vocal cues

    float reveal = 0.0f;        // reveal-on-sound glow (see Tree::reveal)
    uint32_t id = 0;
    SoundProfile sounds;        // Loop rule = the predator drone; SoundDirector
                                // owns/follows the voice like any moving object

    glm::vec3 soundPos() const { return pos; }
};

// Footstep sound assignment for one ground material.
struct FloorMaterial {
    std::string name;
    int soundSet = -1;          // variants picked at random per step
    float gain = 0.7f;
    float pitchJitter = 0.05f;
    float gainJitterDb = 2.0f;
};

struct Scene {
    static constexpr float kHalfExtent = 45.0f;     // world is 90x90 m
    static constexpr float kCreekY = 8.0f;          // creek centerline: y = 8
    static constexpr float kCreekHalfWidth = 1.0f;  // 2 m wide band
    static constexpr float kStoneHalfWidth = 3.0f;  // stone ground near the creek

    // Floor material grid: kGridN x kGridN cells of kCellSize m, material
    // index per cell. Painted in dev mode (brush / rect paving); footsteps
    // and the floor inspect target read it through materialAt().
    static constexpr float kCellSize = 0.5f;
    static constexpr int kGridN = static_cast<int>(2.0f * kHalfExtent / kCellSize);
    static constexpr float kCellArea = kCellSize * kCellSize;

    std::vector<Tree> trees;
    std::vector<Boulder> boulders;
    std::vector<Emitter> emitters;
    std::vector<Bug> bugs;
    std::vector<Stalker> stalkers;
    std::vector<FloorMaterial> materials;   // indexed by floor material id
    std::vector<uint8_t> floor;             // kGridN*kGridN material indices

    uint32_t nextId = 1;
    uint32_t takeId() { return nextId++; }

    Scene() : floor(static_cast<size_t>(kGridN) * kGridN, 0) {}

    static int cellIndexOf(float v) {
        const int i = static_cast<int>(std::floor((v + kHalfExtent) / kCellSize));
        return std::clamp(i, 0, kGridN - 1);
    }

    uint8_t& cell(int ix, int iy) { return floor[static_cast<size_t>(iy) * kGridN + ix]; }
    uint8_t cell(int ix, int iy) const { return floor[static_cast<size_t>(iy) * kGridN + ix]; }

    int materialAt(float x, float y) const {
        const int m = cell(cellIndexOf(x), cellIndexOf(y));
        return m < static_cast<int>(materials.size()) ? m : 0;
    }

    // Paint every cell whose center is within `radius` of (x, y).
    void paintCircle(float x, float y, float radius, int mat);
    // Paint every cell touched by the rect spanned by two corners.
    void paintRect(glm::vec2 a, glm::vec2 b, int mat);
    // Painted surface area of one material, m^2.
    float areaOf(int mat) const;
};

class SoundLibrary;
Scene buildScene(const SoundLibrary& lib);

// The playable-slice level: a dark corridor of boulder walls from a start pocket
// to a goal, punctuated by music-emitter sanctuaries that repel the Stalker. One
// Stalker patrols the gaps. See SliceGame.h for the win/lose loop.
Scene buildSliceScene(const SoundLibrary& lib);

// Advance bug flight paths. Deterministic (pure functions of per-bug time),
// so the offline render test reproduces exactly.
void updateBugs(Scene& scene, float dt);

// Default sound profile for a new bug (dev placement + buildScene).
SoundRule makeBugLoopRule(const SoundLibrary& lib);
