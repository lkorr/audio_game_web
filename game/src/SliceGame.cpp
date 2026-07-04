#include "SliceGame.h"
#include "Stalker.h"
#include "Player.h"
#include "SoundLibrary.h"
#include <glm/geometric.hpp>
#include <algorithm>

namespace {
constexpr float kCatchDist = 1.6f;   // stalker within this of the eye = caught
constexpr float kBannerHold = 2.5f;  // seconds the outcome banner shows

// Fire a stinger set as a wet one-shot at the listener, through the current
// room reverb. Uses SoundDirector::audition (random variant + jitter + play).
void fireStinger(SoundDirector& director, int set, const glm::vec3& pos,
                 const ListenerPose& listener) {
    if (set < 0) return;
    SoundRule r;
    r.soundSet = set;
    r.gain = 0.85f;
    r.audibleRadius = 60.0f;   // it is a UI cue: audible wherever you are
    r.verbWet = 0.4f;          // let the room color it
    r.doppler = false;
    r.pitchJitter = 0.0f;
    r.gainJitterDb = 0.0f;
    director.audition(r, pos, listener);
}
} // namespace

void SliceGame::init(const Scene& scene, const glm::vec3& startPos,
                     const SoundLibrary& lib) {
    active_ = false;
    outcome_ = Outcome::Playing;
    respawn_ = startPos;
    stingWin_ = lib.find("sting_win");
    stingFail_ = lib.find("sting_fail");
    for (const Emitter& e : scene.emitters) {
        if (e.isGoal) {
            active_ = true;
            goalPos_ = e.pos;
        }
    }
    hud_ = active_ ? "REACH THE GOAL. STAY IN THE MUSIC. E = LISTEN."
                   : std::string();
}

SliceGame::Outcome SliceGame::update(float dt, Scene& scene, Player& player,
                                     SoundDirector& director,
                                     const ListenerPose& listener) {
    if (!active_) return Outcome::Playing;

    // While standing inside an ACTIVE music sanctuary, remember it as the
    // respawn point -- the player earns checkpoints by reaching safety.
    for (const Emitter& e : scene.emitters) {
        if (!e.isMusic) continue;
        if (!e.sounds.rule(SoundSlot::Loop).wantsSound()) continue;
        glm::vec3 d = listener.pos - e.pos; d.z = 0.0f;
        if (glm::length(d) <= e.musicRadius * 0.8f)
            respawn_ = e.pos;
    }

    // Banner countdown after a resolved outcome. Win stays won; caught resets
    // to play after the banner so the run continues from the last sanctuary.
    if (outcome_ != Outcome::Playing) {
        outcomeTimer_ = std::max(0.0f, outcomeTimer_ - dt);
        if (outcome_ == Outcome::Caught && outcomeTimer_ <= 0.0f) {
            outcome_ = Outcome::Playing;
            hud_ = "REACH THE GOAL. STAY IN THE MUSIC. E = LISTEN.";
        }
        return outcome_;
    }

    // Win: within range of the goal emitter.
    glm::vec3 toGoal = listener.pos - goalPos_; toGoal.z = 0.0f;
    if (glm::length(toGoal) <= goalRadius_) {
        outcome_ = Outcome::Won;
        outcomeTimer_ = kBannerHold;
        hud_ = "YOU REACHED THE GOAL. THE DARK IS QUIET NOW.";
        fireStinger(director, stingWin_, listener.pos, listener);
        return outcome_;
    }

    // Lose: a stalker got you. Respawn at the last safe sanctuary, keep playing.
    if (nearestStalkerDist(scene, listener.pos) <= kCatchDist) {
        outcome_ = Outcome::Caught;
        outcomeTimer_ = kBannerHold;
        hud_ = "IT FOUND YOU. BACK TO THE MUSIC.";
        fireStinger(director, stingFail_, listener.pos, listener);
        player.pos = { respawn_.x, respawn_.y };
        // Calm every stalker so the respawn is survivable.
        for (Stalker& s : scene.stalkers) {
            s.state = Stalker::State::Patrol;
            s.interest = 0.0f;
            s.alertTimer = 0.0f;
        }
        return outcome_;
    }

    return Outcome::Playing;
}
