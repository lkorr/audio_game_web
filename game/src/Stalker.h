#pragma once
// The Stalker: a predator that hunts the player by sound (game thread). Two
// entry points, both pure game logic (no audio voices touched here -- its drone
// is a normal moving loop owned by SoundDirector):
//
//   hearSoundStalkers()  fed by SoundDirector whenever the player makes a sound
//                        (footstep/ping/impact). The sound's loudness AT the
//                        stalker is weighted by distance and by the SAME
//                        occlusion the player hears, so geometry between you and
//                        it hides you. A loud-enough sound sets lastHeard and
//                        flips the stalker to Investigate/Chase.
//
//   updateStalkers()     per-frame steering: drift on patrol, home on lastHeard
//                        when alerted, give up after a timeout. Blocked by
//                        occluder AABBs (no clipping through boulders) and
//                        repelled by ACTIVE music emitters (musicRadius) -- the
//                        sanctuaries the player routes between.
//
// Deliberately simple (no pathfinding): drift toward the last loud sound. In
// the dark, tracked only by its drone, that is already tense.

#include "Scene.h"
#include "Occlusion.h"
#include "AudioWorld.h"   // ListenerPose
#include <glm/vec3.hpp>
#include <vector>

class SoundDirector;

// Report one player-made sound to every stalker. `loudness` is the source
// strength at the emitter (~0..1: grass step quiet, stone louder, sprint
// louder still, ping/impact loud). `occluders` is the current frame's list
// (buildOccluders) so the stalker hears through the world, not around it.
void hearSoundStalkers(Scene& scene, const glm::vec3& soundPos, float loudness,
                       const std::vector<Occluder>& occluders);

// Advance all stalkers: steering, music repulsion, occluder blocking. Call once
// per frame after SoundDirector-fed hearing, alongside updateBugs. When
// `director`/`listener` are supplied, each moving stalker emits footstep
// one-shots through the director (its gait: louder/faster Chasing, slower on
// Patrol, silent when effectively stationary). Pass null to skip footsteps.
void updateStalkers(Scene& scene, float dt, const std::vector<Occluder>& occluders,
                    SoundDirector* director = nullptr,
                    const ListenerPose* listener = nullptr);

// Distance from the nearest stalker to `p` (meters); +inf if there are none.
// The game loop uses this for the catch/lose check.
float nearestStalkerDist(const Scene& scene, const glm::vec3& p);

// Perceived source loudness (0..1) of a footstep on `material`, louder when
// sprinting. The game loop passes the result to hearSoundStalkers so quiet
// ground (grass) and a careful pace keep you hidden.
float footstepLoudness(const Scene& scene, int material, bool sprinting);

// Loudness of the player's echo ping -- a loud, deliberate beacon.
constexpr float kPingLoudness = 1.0f;

// Default drone profile for a new stalker (placement + buildScene).
class SoundLibrary;
SoundRule makeStalkerLoopRule(const SoundLibrary& lib);
