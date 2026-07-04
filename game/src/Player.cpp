#include "Player.h"
#include <glm/geometric.hpp>
#include <algorithm>
#include <cmath>

glm::vec3 Player::forward() const {
    const float cp = std::cos(pitch);
    return { -std::sin(yaw) * cp, std::cos(yaw) * cp, std::sin(pitch) };
}

namespace {

// Push a circle (player capsule footprint) out of a circle collider.
// On hit, `contact` receives the touch point on the collider surface (2D).
bool resolveCircle(glm::vec2& p, float r, const glm::vec2& c, float cr, glm::vec2& contact) {
    const glm::vec2 d = p - c;
    const float dist2 = glm::dot(d, d);
    const float minDist = r + cr;
    if (dist2 >= minDist * minDist) return false;
    const float dist = std::sqrt(std::max(dist2, 1e-12f));
    const glm::vec2 n = (dist > 1e-6f) ? d / dist : glm::vec2(1.0f, 0.0f);
    p = c + n * minDist;
    contact = c + n * cr;
    return true;
}

// Push a circle out of an axis-aligned rectangle.
// On hit, `contact` receives the closest point on the rect boundary (2D).
bool resolveRect(glm::vec2& p, float r, const glm::vec2& center, const glm::vec2& half,
                 glm::vec2& contact) {
    const glm::vec2 local = p - center;
    const glm::vec2 clamped{ std::clamp(local.x, -half.x, half.x),
                             std::clamp(local.y, -half.y, half.y) };
    const glm::vec2 d = local - clamped;
    const float dist2 = glm::dot(d, d);
    if (dist2 >= r * r) return false;
    contact = center + clamped;
    if (dist2 > 1e-12f) {
        const float dist = std::sqrt(dist2);
        p = center + clamped + (d / dist) * r;
    } else {
        // Center inside the rect: push out along the shallowest axis.
        const float px = half.x - std::abs(local.x);
        const float py = half.y - std::abs(local.y);
        if (px < py)
            p.x = center.x + (local.x >= 0.0f ? half.x + r : -half.x - r);
        else
            p.y = center.y + (local.y >= 0.0f ? half.y + r : -half.y - r);
        contact = { std::clamp(p.x, center.x - half.x, center.x + half.x),
                    std::clamp(p.y, center.y - half.y, center.y + half.y) };
    }
    return true;
}

} // namespace

void Player::update(float dt, const PlayerInput& input, Scene& scene,
                    std::vector<FootstepEvent>& stepsOut,
                    std::vector<BumpEvent>& bumpsOut) {
    yaw -= input.mouseDX * kMouseSens();
    pitch = std::clamp(pitch - input.mouseDY * kMouseSens(), -1.45f, 1.45f);

    const glm::vec2 fwd{ -std::sin(yaw), std::cos(yaw) };
    const glm::vec2 right{ fwd.y, -fwd.x };

    glm::vec2 wish{0.0f};
    if (input.forward) wish += fwd;
    if (input.back) wish -= fwd;
    if (input.right) wish += right;
    if (input.left) wish -= right;

    const glm::vec2 before = pos;
    const float speed = input.sprint ? kSprintSpeed() : kWalkSpeed();
    if (glm::dot(wish, wish) > 1e-6f) {
        wish = glm::normalize(wish);
        pos += wish * speed * dt;
    }

    // Fly mode: vertical movement, no collision, no footsteps.
    if (flying) {
        const float vert = (input.up ? 1.0f : 0.0f) - (input.down ? 1.0f : 0.0f);
        eyeZ = std::clamp(eyeZ + vert * speed * dt, 0.4f, 30.0f);
        const float lim = Scene::kHalfExtent - kRadius();
        pos.x = std::clamp(pos.x, -lim, lim);
        pos.y = std::clamp(pos.y, -lim, lim);
        return;
    }
    eyeZ = kEyeHeight();

    // Collision: a few resolve iterations give a stable slide response.
    // Contact points drive the localized bump flash (touch-height z: roughly
    // where an outstretched hand would brush the surface).
    constexpr float kTouchHeight = 1.1f;
    auto addBump = [&bumpsOut](uint32_t id, const glm::vec3& at) {
        // One event per object per frame (the resolve loop can hit it thrice).
        for (const BumpEvent& e : bumpsOut)
            if (e.objectId == id) return;
        bumpsOut.push_back({ id, at });
    };
    for (int iter = 0; iter < 3; ++iter) {
        bool any = false;
        glm::vec2 contact;
        for (Tree& t : scene.trees)
            if (resolveCircle(pos, kRadius(), t.pos, t.trunkRadius, contact)) {
                t.flash = 1.0f;
                t.flashPos = { contact.x, contact.y,
                               std::min(kTouchHeight, t.trunkHeight) };
                addBump(t.id, t.flashPos);
                any = true;
            }
        for (Boulder& b : scene.boulders)
            if (resolveRect(pos, kRadius(), { b.center.x, b.center.y },
                            { b.half.x, b.half.y }, contact)) {
                b.flash = 1.0f;
                b.flashPos = { contact.x, contact.y,
                               std::clamp(kTouchHeight, b.center.z - b.half.z,
                                          b.center.z + b.half.z) };
                addBump(b.id, b.flashPos);
                any = true;
            }
        if (!any) break;
    }

    // Keep inside the glade.
    const float lim = Scene::kHalfExtent - kRadius();
    pos.x = std::clamp(pos.x, -lim, lim);
    pos.y = std::clamp(pos.y, -lim, lim);

    // Footsteps from actual (post-collision) horizontal travel.
    stepAccum_ += glm::length(pos - before);
    while (stepAccum_ >= kStepDistance()) {
        stepAccum_ -= kStepDistance();
        stepsOut.push_back({ { pos.x, pos.y, 0.0f }, scene.materialAt(pos.x, pos.y) });
    }
}
