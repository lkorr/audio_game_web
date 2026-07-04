#include "Stalker.h"
#include "SoundLibrary.h"
#include "SoundDirector.h"
#include <glm/geometric.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Tuning constants for the hunt. Kept here (not in SoundRule) because they are
// behavior, not sound character.
constexpr float kHearThreshold = 0.06f;  // min perceived loudness to react to
constexpr float kChaseThreshold = 0.30f; // perceived loudness that means "close"
constexpr float kAlertHold = 6.0f;       // seconds to pursue after last hearing
constexpr float kPatrolSpeed = 1.1f;     // m/s wandering
constexpr float kInvestigateSpeed = 1.9f;
constexpr float kSearchSpeed = 1.5f;     // brisk sweep while hunting the area
constexpr float kChaseSpeed = 2.7f;      // still < player sprint (3.0): escapable
constexpr float kArrive = 1.0f;          // within this of lastHeard = arrived
constexpr float kMusicPush = 3.0f;       // outward accel scale at a sanctuary edge

// Search phase: after the trail goes cold the stalker sweeps the area around the
// last-heard spot for kSearchHold seconds before giving up to Patrol. It picks a
// fresh random-ish point within kSearchRadius each time it arrives at one.
constexpr float kSearchHold = 10.0f;     // seconds spent hunting the area
constexpr float kSearchRadius = 8.0f;    // how far around lastHeard it sweeps

// Attack lunge: while Chasing, if the player's last-heard spot is within
// kLungeRange it commits to a lunge -- locks a point kLungeOvershoot metres PAST
// that spot and sprints straight to it at kChaseSpeed*kLungeSpeedMult, ignoring
// new cues (fully committed: sidestep it). On arrival it is Confused for
// kConfusedHold seconds (a stunned pause), then drops into Search. kLungeMaxTime
// is a safety cap so a blocked lunge can't run forever.
constexpr float kLungeRange = 5.0f;        // Chase within this of the cue -> lunge
constexpr float kLungeOvershoot = 3.0f;    // metres past the locked spot
constexpr float kLungeSpeedMult = 4.0f;    // lunge speed = chase speed * this (~4x)
constexpr float kLungeArrive = 0.8f;       // within this of lungeTarget = done
constexpr float kLungeMaxTime = 1.2f;      // hard cap on one lunge (blocked path)
constexpr float kConfusedHold = 1.6f;      // stunned pause after a lunge, seconds

// Mood vocalizations: the interval between repeated in-state calls (a call also
// fires immediately on entering a state). Roomy so it punctuates, not chatters.
constexpr float kCueInterval = 3.5f;

// Ambient masking: loud continuous emitters (the stream, cavern drips, wind)
// drown out the player's footsteps near them. This is a LOCAL signal-to-noise
// gate, both terms measured at the player: the player's own sound strength vs.
// the ambient level at the same spot. The stalker's perceived loudness is scaled
// by  signal / (signal + kMaskScale * ambient)  -- 1.0 in quiet, falling toward 0
// only when a loud source is right on top of you. A multiplicative gate (not a
// subtraction of a far-field number) keeps loud sounds mostly audible unless you
// are genuinely hugging a source, so the stalker isn't deaf out in the open.
constexpr float kMaskScale = 2.5f;       // ambient weight in the SNR gate
constexpr float kMaskRadius = 14.0f;     // ignore ambient sources beyond this (near-field only)

// Hearing window. A hard distance cutoff comes first (cheap early-out before the
// occlusion/distance weighting) so sounds far across the map never move a
// stalker. Within that radius it commits to the LOUDEST sound of the last
// `kHearWindow` seconds -- a fresh cue only wins if it is louder OR the current
// cue has expired, so it locks onto the strongest recent thing instead of
// twitching to every faint step.
constexpr float kHearRadius = 28.0f;     // hard cutoff: ignore sounds beyond this
constexpr float kHearWindow = 3.0f;      // seconds a heard cue stays "the" cue

// Stalker footsteps: distance between one-shots (shorter when moving fast),
// per-state loudness, and the speed below which it is effectively stationary and
// stays silent.
constexpr float kStepDistance = 0.8f;    // travel between footstep one-shots (m):
                                         // short so the gait reads as a rhythm,
                                         // not a lonely thud every second-plus
constexpr float kChaseStepGain = 0.9f;   // heavy, close: the gait you dread
constexpr float kPatrolStepGain = 0.7f;  // still clearly audible while it prowls
constexpr float kStepSilentSpeed = 0.15f; // m/s below this: no footsteps

// Loudness of a sound as perceived at `at`: source strength attenuated by
// inverse-distance falloff (with a 1 m floor) and by geometric occlusion. This
// is the whole stealth model -- move quietly, or keep a boulder between you and
// the hunter.
float perceivedLoudness(const glm::vec3& at, const glm::vec3& soundPos,
                        float loudness, const std::vector<Occluder>& occluders) {
    const float d = glm::length(at - soundPos);
    const float distFall = 1.0f / std::max(1.0f, d);
    const OcclusionResult occ = computeOcclusion(at, soundPos, occluders);
    return loudness * distFall * occ.gain;
}

// Ambient sound level AT the player's own position `at`: the summed near-field
// loudness of every ACTIVE non-music Loop emitter close to it -- the stream,
// cavern drips, wind. This is the "noise" side of the masking SNR gate; it is
// deliberately measured at the player (not at the stalker) so it is only large
// when the player is genuinely close to a loud source. Music sanctuaries are
// excluded (a separate mechanic; safety there should not also blind the stalker).
float ambientLevelAt(const Scene& scene, const glm::vec3& at,
                     const std::vector<Occluder>& occluders) {
    float ambient = 0.0f;
    for (const Emitter& e : scene.emitters) {
        if (e.isMusic) continue;
        const SoundRule& loop = e.sounds.rule(SoundSlot::Loop);
        if (!loop.wantsSound()) continue;
        if (glm::length(at - e.pos) > kMaskRadius) continue;
        ambient += perceivedLoudness(at, e.pos, loop.gain, occluders);
    }
    return ambient;
}

// Nearest point the stalker is allowed to occupy given the AABB occluders:
// if `p` lands inside a boulder (expanded by a small skin), push it out along
// the shallowest axis. Cylinders (tree trunks) are thin; we let the stalker
// brush past those rather than solve full capsule-vs-cylinder here.
glm::vec3 avoidOccluders(const glm::vec3& p, const std::vector<Occluder>& occluders) {
    constexpr float kSkin = 0.6f;   // stalker "radius"
    glm::vec3 out = p;
    for (const Occluder& o : occluders) {
        if (o.shape != Occluder::Shape::Box) continue;
        const glm::vec3 mn = o.center - o.half - glm::vec3(kSkin);
        const glm::vec3 mx = o.center + o.half + glm::vec3(kSkin);
        if (out.x < mn.x || out.x > mx.x || out.y < mn.y || out.y > mx.y) continue;
        // Inside the expanded box in XY: eject along the axis with least overlap.
        const float pushL = out.x - mn.x, pushR = mx.x - out.x;
        const float pushD = out.y - mn.y, pushU = mx.y - out.y;
        const float minX = std::min(pushL, pushR);
        const float minY = std::min(pushD, pushU);
        if (minX < minY) out.x = (pushL < pushR) ? mn.x : mx.x;
        else             out.y = (pushD < pushU) ? mn.y : mx.y;
    }
    return out;
}

// Sum of outward push from every ACTIVE music sanctuary the stalker is inside.
// A music emitter only repels while its Loop rule is enabled and assigned; the
// push ramps up from the radius edge to the center so the boundary is soft but
// the interior is firmly forbidden.
glm::vec3 musicRepulsion(const Scene& scene, const glm::vec3& p) {
    glm::vec3 push{0.0f};
    for (const Emitter& e : scene.emitters) {
        if (!e.isMusic) continue;
        if (!e.sounds.rule(SoundSlot::Loop).wantsSound()) continue;
        glm::vec3 d = p - e.pos;
        d.z = 0.0f;
        const float dist = glm::length(d);
        if (dist >= e.musicRadius) continue;
        const float t = 1.0f - dist / std::max(0.01f, e.musicRadius);  // 0 edge..1 center
        const glm::vec3 dir = dist > 1e-3f ? d / dist
                                           : glm::vec3(1.0f, 0.0f, 0.0f);
        push += dir * (t * kMusicPush);
    }
    return push;
}

float speedFor(Stalker::State s) {
    switch (s) {
        case Stalker::State::Attack:      return kChaseSpeed * kLungeSpeedMult;
        case Stalker::State::Chase:       return kChaseSpeed;
        case Stalker::State::Investigate: return kInvestigateSpeed;
        case Stalker::State::Search:      return kSearchSpeed;
        case Stalker::State::Confused:    return 0.0f;   // stunned, rooted
        default:                          return kPatrolSpeed;
    }
}

// Map an AI state to a mood-call variant index (see synthStalkerCall / the
// stalker_call set: 0 roam, 1 hunt, 2 search, 3 confused). Investigate shares the
// "hunt" voice (it is actively closing); Attack has its own lunge screech, not a
// mood call, so it returns -1 (no periodic call while lunging).
int moodFor(Stalker::State s) {
    switch (s) {
        case Stalker::State::Patrol:      return 0;   // roam
        case Stalker::State::Chase:       return 1;   // hunt
        case Stalker::State::Investigate: return 1;   // hunt (closing)
        case Stalker::State::Search:      return 2;   // search
        case Stalker::State::Confused:    return 3;   // confused
        default:                          return -1;  // Attack: no mood call
    }
}

} // namespace

SoundRule makeStalkerLoopRule(const SoundLibrary& lib) {
    SoundRule r;
    r.soundSet = lib.find("stalker");
    r.gain = 0.6f;
    r.audibleRadius = 30.0f;   // carries far: you should hear it coming
    r.verbWet = 0.25f;
    r.doppler = true;
    r.dopplerScale = 1.5f;     // motion is legible but not cartoonish
    return r;
}

void hearSoundStalkers(Scene& scene, const glm::vec3& soundPos, float loudness,
                       const std::vector<Occluder>& occluders) {
    // Masking is a property of WHERE the sound was made, not of any one stalker,
    // so compute it once. The gate is the local signal-to-noise at the player:
    // `loudness` is the player's own source strength; `ambient` is the loudness of
    // loud sources right next to the player. maskGain -> 1 in quiet, -> 0 only when
    // a source is right on top of you. Both terms are near-field (measured at the
    // player), so a distant stalker is NOT made deaf -- only proximity to a loud
    // source hides you.
    const float ambient = ambientLevelAt(scene, soundPos, occluders);
    const float maskGain = loudness / std::max(1e-4f, loudness + kMaskScale * ambient);

    for (Stalker& s : scene.stalkers) {
        // A committed lunge (Attack) ignores everything -- it charges its locked
        // point regardless. Confused is a stun: it cannot re-lock until it clears.
        if (s.state == Stalker::State::Attack ||
            s.state == Stalker::State::Confused) continue;

        // Hard radius cutoff first: sounds outside the hearing radius never move
        // the stalker, no matter how loud. Cheap early-out before the (costlier)
        // occlusion/distance weighting.
        if (glm::length(s.pos - soundPos) > kHearRadius) continue;

        // Perceived loudness at the stalker, scaled by the masking gate: standing
        // beside the stream your footsteps are drowned out and the stalker never
        // gets a cue loud enough to commit. Travel along the stream / past noisy
        // sources to shed it; out in the open a ping or sprint still gives you away.
        float perceived = perceivedLoudness(s.pos, soundPos, loudness, occluders) * maskGain;
        if (perceived < kHearThreshold) continue;

        // Recency window: the current cue counts as "fresh" for the first
        // kHearWindow seconds after it was heard (alertTimer counts down from
        // kAlertHold). A new sound overrides only if it is LOUDER than the fresh
        // cue, or the window has elapsed -- so it commits to the strongest recent
        // thing rather than twitching to every faint step.
        const bool cueFresh = s.alertTimer > (kAlertHold - kHearWindow) &&
                              s.state != Stalker::State::Patrol;
        if (cueFresh && perceived < s.interest) continue;

        s.lastHeard = soundPos;
        s.interest = perceived;
        s.alertTimer = kAlertHold;
        s.state = (perceived >= kChaseThreshold) ? Stalker::State::Chase
                                                 : Stalker::State::Investigate;
    }
}

void updateStalkers(Scene& scene, float dt, const std::vector<Occluder>& occluders,
                    SoundDirector* director, const ListenerPose* listener) {
    const float lim = Scene::kHalfExtent - 0.5f;
    for (Stalker& s : scene.stalkers) {
        // Interest fades and the alert clock ticks down. When the pursuit clock
        // runs out (the cue has gone stale) it does NOT snap straight home:
        // Investigate/Chase drop into Search -- a timed sweep of the area around
        // the last-heard spot -- and only Search itself finally times out to
        // Patrol. Patrol has no cue and ignores both clocks.
        s.alertTimer = std::max(0.0f, s.alertTimer - dt);
        s.interest *= std::exp(-dt * 0.4f);
        s.stateTimer = std::max(0.0f, s.stateTimer - dt);
        if (s.state == Stalker::State::Chase ||
            s.state == Stalker::State::Investigate) {
            // If close enough to a live cue, COMMIT to a lunge: lock a point a few
            // metres past the last-heard spot and drive at it (see below). It does
            // not track the player once committed -- sidestep the charge. Reachable
            // from either homing state: what matters is that it got near you with a
            // fresh trail, not the loudness label.
            glm::vec3 toCue = s.lastHeard - s.pos; toCue.z = 0.0f;
            const float cueDist = glm::length(toCue);
            if (s.alertTimer > 0.0f && cueDist <= kLungeRange && cueDist > 1e-3f) {
                s.state = Stalker::State::Attack;
                const glm::vec3 dir = toCue / cueDist;
                s.lungeTarget = s.lastHeard + dir * kLungeOvershoot;
                s.lungeTarget.z = 1.2f;
                s.stateTimer = kLungeMaxTime;   // safety cap
            } else if (s.alertTimer <= 0.0f) {
                // Trail cold before it got close: sweep the area.
                s.state = Stalker::State::Search;
                s.searchTimer = kSearchHold;
                s.searchTarget = s.lastHeard;
                s.interest = 0.0f;
            }
        } else if (s.state == Stalker::State::Confused) {
            // Stunned pause after the lunge; when it clears, start searching where
            // it last had you (it just overshot, so the trail is nearby).
            if (s.stateTimer <= 0.0f) {
                s.state = Stalker::State::Search;
                s.searchTimer = kSearchHold;
                s.searchTarget = s.lastHeard;
                s.interest = 0.0f;
            }
        } else if (s.state == Stalker::State::Search) {
            s.searchTimer = std::max(0.0f, s.searchTimer - dt);
            if (s.searchTimer <= 0.0f)
                s.state = Stalker::State::Patrol;   // gave up: back to patrol
        }
        // Attack is time/arrival driven in the movement block below; no timeout here
        // beyond the kLungeMaxTime safety cap it shares via stateTimer.

        // Music sanctuaries cleanly force Patrol: if it has been pushed inside an
        // active sanctuary it cannot reach the player through it, so it gives up
        // the trail and heads home rather than jittering at the boundary. Interrupts
        // a lunge too (the sanctuary is a hard no-go).
        const glm::vec3 push = musicRepulsion(scene, s.pos);
        if (s.state != Stalker::State::Patrol && glm::length(push) > 1e-3f) {
            s.state = Stalker::State::Patrol;
            s.interest = 0.0f;
            s.alertTimer = 0.0f;
            s.searchTimer = 0.0f;
            s.stateTimer = 0.0f;
        }

        // Desired heading vector.
        glm::vec3 target;
        if (s.state == Stalker::State::Attack) {
            target = s.lungeTarget;         // fixed: committed straight-line charge
        } else if (s.state == Stalker::State::Confused) {
            target = s.pos;                 // rooted in place, stunned
        } else if (s.state == Stalker::State::Patrol) {
            // Lazy roaming around the FIXED home anchor: a slow drifting turn,
            // easing back toward home as it nears the patrol radius (a smooth
            // blend, not a hard steer-inward snap, so the path stays lazy).
            s.heading += 0.5f * dt;   // gentle constant turn
            glm::vec3 dir(std::cos(s.heading), std::sin(s.heading), 0.0f);
            const glm::vec3 fromHome = s.pos - s.home;
            const float homeDist = glm::length(fromHome);
            if (homeDist > 1e-3f) {
                // Weight toward home grows from 0 at center to 1 at the radius
                // (and beyond), so it curves back gradually instead of orbiting.
                const float pull = std::clamp(homeDist / std::max(0.01f, s.patrolRadius),
                                              0.0f, 1.0f);
                const glm::vec3 homeward = -fromHome / homeDist;
                dir = glm::normalize(dir * (1.0f - pull) + homeward * pull);
            }
            target = s.pos + dir;
        } else if (s.state == Stalker::State::Search) {
            // Sweep the area around the last-heard spot: head for searchTarget;
            // when it arrives, pick a fresh point within kSearchRadius of
            // lastHeard. The point is derived deterministically from the search
            // clock + id (golden-angle spin), so the render test stays exact --
            // no RNG. The result reads as it prowling around where you were.
            glm::vec3 toPt = s.searchTarget - s.pos; toPt.z = 0.0f;
            if (glm::length(toPt) <= kArrive) {
                const float a = (s.searchTimer + static_cast<float>(s.id)) * 2.39996f;
                const float r = kSearchRadius * (0.4f + 0.6f *
                                 std::abs(std::sin(s.searchTimer * 1.3f)));
                s.searchTarget = s.lastHeard +
                                 glm::vec3(r * std::cos(a), r * std::sin(a), 0.0f);
            }
            target = s.searchTarget;
        } else {
            target = s.lastHeard;
        }

        glm::vec3 toTarget = target - s.pos;
        toTarget.z = 0.0f;
        const float dist = glm::length(toTarget);
        glm::vec3 dir = dist > 1e-3f ? toTarget / dist : glm::vec3(0.0f);

        // Music sanctuaries bend an ongoing approach away: the outward push is
        // added on top of the seek so nearing a protected pocket curves the path
        // around it (Patrol has already been forced above if it got inside). A
        // committed lunge ignores the push -- it is a dead-straight charge.
        glm::vec3 move = (s.state == Stalker::State::Attack) ? dir : dir + push;
        if (glm::length(move) > 1e-3f) move = glm::normalize(move);

        // Arrival handling per state:
        //  - Attack reaches its locked overshoot point (or hits the safety cap) ->
        //    Confused (a stunned pause), then it will Search.
        //  - Investigate/Chase reach the last-heard spot but you are gone -> Search.
        //  - Search never "arrives" (it repoints around lastHeard); Patrol drifts;
        //    Confused is rooted (speed 0).
        const glm::vec3 prevPos = s.pos;
        if (s.state == Stalker::State::Attack &&
            (dist <= kLungeArrive || s.stateTimer <= 0.0f)) {
            // Lunge spent: overshot the spot (or blocked). Recover, disoriented.
            s.state = Stalker::State::Confused;
            s.stateTimer = kConfusedHold;
            s.interest = 0.0f;
        } else if ((s.state == Stalker::State::Investigate ||
                    s.state == Stalker::State::Chase) && dist <= kArrive) {
            // Reached where it last heard you, but you are gone: prowl the area.
            s.state = Stalker::State::Search;
            s.searchTimer = kSearchHold;
            s.searchTarget = s.lastHeard;
            s.interest = 0.0f;
        } else {
            const float step = speedFor(s.state) * dt;
            glm::vec3 next = s.pos + move * step;
            next.x = std::clamp(next.x, -lim, lim);
            next.y = std::clamp(next.y, -lim, lim);
            next = avoidOccluders(next, occluders);
            s.pos = next;
        }

        // Keep the drone at a steady stalking height.
        s.pos.z = 1.2f;

        // Footsteps: accumulate horizontal travel and fire a one-shot each time
        // it passes a step distance. Silent when effectively stationary (arrived,
        // or barely moving), louder/faster when Chasing since Chase covers ground
        // fastest so kStepDistance is reached sooner.
        if (director != nullptr && listener != nullptr) {
            glm::vec2 delta(s.pos.x - prevPos.x, s.pos.y - prevPos.y);
            const float travel = glm::length(delta);
            if (travel > kStepSilentSpeed * dt) {
                s.stepAccum += travel;
                if (s.stepAccum >= kStepDistance) {
                    s.stepAccum -= kStepDistance;
                    // Chase and the Attack lunge are the heavy, urgent gait.
                    const bool heavy = s.state == Stalker::State::Chase ||
                                       s.state == Stalker::State::Attack;
                    director->onStalkerStep(s.pos, heavy ? kChaseStepGain : kPatrolStepGain,
                                            *listener);
                }
            } else {
                s.stepAccum = 0.0f;   // stationary: reset so it does not creep
            }
        }

        // Mood vocalizations: voice the AI's state so the player reads it by ear.
        //  - Entering Attack fires the loud lunge screech (the tell to dodge).
        //  - Every other state change fires that state's mood call immediately.
        //  - While in a state, it re-calls on a roomy interval so a long hunt /
        //    search / patrol keeps speaking (Confused is short, so entry is enough).
        if (director != nullptr && listener != nullptr) {
            const bool entered = s.state != s.prevState;
            if (entered && s.state == Stalker::State::Attack) {
                director->onStalkerLunge(s.pos, *listener);
                s.cueTimer = kCueInterval;
            } else {
                const int mood = moodFor(s.state);
                if (mood >= 0) {
                    s.cueTimer -= dt;
                    if (entered || s.cueTimer <= 0.0f) {
                        // Hunt reads loudest; roam softest; others in between.
                        const float g = (mood == 1) ? 0.95f : (mood == 0) ? 0.6f : 0.8f;
                        director->onStalkerCall(s.pos, mood, g, *listener);
                        s.cueTimer = kCueInterval;
                    }
                }
            }
        }
        s.prevState = s.state;
    }
}

float footstepLoudness(const Scene& scene, int material, bool sprinting) {
    // Base on the material's own footstep gain (how loud that surface reads),
    // so stone (bright, ringing) draws harder than grass (soft) with no extra
    // table. Sprinting slams the ground: a big multiplier that turns a stealthy
    // grass creep into a beacon.
    float base = 0.25f;
    const size_t mi = static_cast<size_t>(material);
    if (material >= 0 && mi < scene.materials.size())
        base = 0.18f + 0.5f * scene.materials[mi].gain;  // ~0.27 grass .. ~0.53 stone
    return std::clamp(base * (sprinting ? 2.2f : 1.0f), 0.0f, 1.0f);
}

float nearestStalkerDist(const Scene& scene, const glm::vec3& p) {
    float best = std::numeric_limits<float>::infinity();
    for (const Stalker& s : scene.stalkers)
        best = std::min(best, glm::length(s.pos - p));
    return best;
}
