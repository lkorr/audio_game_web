#pragma once
// Player movement, capsule collision against scene colliders, footstep logic.
//
// Orientation convention (matches the engine's listener rotation math in
// Engine.cpp): with listenerYaw = theta, a source rotates by azimuth + theta,
// so "in front" is the direction (-sin(theta), cos(theta)). Mouse-right
// therefore *decreases* yaw. Positive pitch = looking up.

#include "Scene.h"
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <vector>

struct PlayerInput {
    bool forward = false, back = false, left = false, right = false, sprint = false;
    bool up = false, down = false;         // fly mode only (Space / LCtrl)
    float mouseDX = 0.0f, mouseDY = 0.0f;  // relative, pixels
};

// One footstep event produced by update().
struct FootstepEvent {
    glm::vec3 pos;
    int material;        // index into Scene::materials
};

// One collider contact produced by update(). Continuous while sliding along
// an object; SoundDirector's per-object impact cooldown rate-limits it.
struct BumpEvent {
    uint32_t objectId;
    glm::vec3 pos;       // world-space contact point
};

class Player {
public:
    static constexpr float kEyeHeight = 1.7f;
    static constexpr float kRadius = 0.35f;
    static constexpr float kWalkSpeed = 1.6f;
    static constexpr float kSprintSpeed = 3.0f;
    static constexpr float kStepDistance = 0.75f;
    static constexpr float kMouseSens = 0.0026f;  // rad per pixel

    glm::vec2 pos{0.0f, -7.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;

    // Dev fly/noclip: Space/LCtrl move the eye vertically, collision and
    // footsteps are skipped. eyeZ snaps back to kEyeHeight on landing (the
    // engine's position smoothers absorb the step).
    bool flying = false;
    float eyeZ = kEyeHeight;

    glm::vec3 eyePos() const { return { pos.x, pos.y, eyeZ }; }
    glm::vec3 forward() const;

    // Moves, collides (slide response, marking bumped objects' flash and
    // appending bump events), and appends footstep events for every
    // kStepDistance of horizontal travel.
    void update(float dt, const PlayerInput& input, Scene& scene,
                std::vector<FootstepEvent>& stepsOut,
                std::vector<BumpEvent>& bumpsOut);

private:
    float stepAccum_ = 0.0f;
};
