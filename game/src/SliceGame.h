#pragma once
// The playable-slice game loop: objective, win/lose, respawn. Thin layer over
// the existing systems so both the native and web frame loops share one brain.
//
// The loop: cross the dark from sanctuary to sanctuary (active music emitters
// that repel the Stalker) and reach the goal emitter. If the Stalker catches
// you, you respawn at the last sanctuary you were safe inside. Reaching the goal
// wins. All state is in-session (matches the engine: only sound state persists).
//
// Only runs when the scene actually declares a goal (an emitter flagged
// isGoal). On the sandbox/built-in scene it is inert, so nothing changes there.

#include "Scene.h"
#include "SoundDirector.h"
#include "AudioWorld.h"
#include <glm/vec3.hpp>
#include <string>

class Player;
class SoundLibrary;

class SliceGame {
public:
    enum class Outcome { Playing, Won, Caught };

    // Scan the scene for a goal emitter + sanctuaries; arm the slice if found.
    // Safe to call every load; a scene without a goal leaves the slice disabled.
    // `lib` is used to resolve the win/lose stinger sound sets.
    void init(const Scene& scene, const glm::vec3& startPos, const SoundLibrary& lib);

    bool active() const { return active_; }

    // Once per frame after movement + stalker update. May teleport the player
    // (on catch) and fire win/lose stingers through the director. `sprint` is
    // unused today but kept for future scoring. Returns the current outcome.
    Outcome update(float dt, Scene& scene, Player& player,
                   SoundDirector& director, const ListenerPose& listener);

    // One-line HUD string for the overlay (objective while playing, result
    // otherwise). Empty when the slice is inactive.
    const std::string& hud() const { return hud_; }

private:
    bool active_ = false;
    Outcome outcome_ = Outcome::Playing;
    glm::vec3 goalPos_{0.0f};   // win radius is tune::kGoalRadius (live-tunable)
    glm::vec3 respawn_{0.0f};   // last safe sanctuary center (or start)
    float outcomeTimer_ = 0.0f; // shows the result banner, then resets on caught
    std::string hud_;

    int stingWin_ = -1;
    int stingFail_ = -1;
};
