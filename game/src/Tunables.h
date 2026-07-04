#pragma once
// Live-tweakable gameplay/visual tunables in ONE place.
//
// Every knob below is a runtime global (not a constexpr), seeded with the value
// that used to be hardcoded. A single text file -- tunables.txt, `key = value`
// per line -- can override any of them, and on the NATIVE build the running game
// re-reads that file whenever it changes on disk (Tunables::pollReload, called
// each frame), so you can edit on a second monitor and hear/see the change live
// without a rebuild or restart.
//
// The audio-engine parameters are NOT duplicated here: they already live in the
// keyed kEngineParams table (EngineTuning) and are applied every frame. The same
// tunables.txt understands those keys too (prefix `engine `), so this really is
// one file for everything -- see Tunables::apply(EngineTuning&).
//
// WEB build: there is no live filesystem to re-read while playing, so the file is
// baked in at build time (loaded once at startup if present). Live tweaking is a
// native-only convenience; ship the dialed-in values to web by rebuilding.

#include <string>

class EngineTuning;

namespace tune {

// ---- Stalker AI (was constexpr in Stalker.cpp) ----
extern float kHearThreshold;    // min perceived loudness to react to
extern float kChaseThreshold;   // perceived loudness that means "close" (-> Chase)
extern float kAlertHold;        // seconds to pursue after last hearing
extern float kPatrolSpeed;      // m/s wandering
extern float kInvestigateSpeed; // m/s homing on a cue
extern float kSearchSpeed;      // m/s sweeping the area
extern float kChaseSpeed;       // m/s chasing (keep < player sprint to stay escapable)
extern float kArrive;           // within this of a cue = arrived (m)
extern float kMusicPush;        // outward accel scale at a sanctuary edge
extern float kSearchHold;       // seconds spent searching the area
extern float kSearchRadius;     // how far around lastHeard it sweeps (m)
extern float kLungeRange;       // Chase/Investigate within this of the cue -> Attack (m)
extern float kLungeOvershoot;   // metres past the locked spot the lunge drives to
extern float kLungeSpeedMult;   // lunge speed = chase speed * this (~4x)
extern float kLungeArrive;      // within this of lungeTarget = lunge done (m)
extern float kLungeMaxTime;     // hard cap on one lunge, seconds (blocked path)
extern float kConfusedHold;     // stunned pause after a lunge, seconds
extern float kCueInterval;      // seconds between repeated in-state mood calls
extern float kMaskScale;        // ambient weight in the masking SNR gate
extern float kMaskRadius;       // ignore ambient sources beyond this (m, near-field)
extern float kHearRadius;       // hard cutoff: ignore player sounds beyond this (m)
extern float kHearWindow;       // seconds a heard cue stays "the" cue
extern float kStepDistance;     // travel between stalker footstep one-shots (m)
extern float kChaseStepGain;    // stalker footstep gain when Chasing/Attacking
extern float kPatrolStepGain;   // stalker footstep gain when prowling
extern float kStepSilentSpeed;  // m/s below which the stalker makes no footsteps
extern float kStalkerSkin;      // stalker "radius" for boulder avoidance (m)

// ---- Slice / game rules (was constexpr in SliceGame.cpp + a member default) ----
extern float kCatchDist;        // stalker within this of the eye = caught (m)
extern float kBannerHold;       // seconds the outcome banner shows
extern float kGoalRadius;       // within this of the goal emitter = win (m)

// ---- Player (was static constexpr in Player.h) ----
extern float kPlayerEyeHeight;  // camera height (m)
extern float kPlayerRadius;     // player collision radius (m)
extern float kPlayerWalkSpeed;  // m/s
extern float kPlayerSprintSpeed;// m/s
extern float kPlayerStepDistance;// travel between the player's own footsteps (m)
extern float kPlayerMouseSens;  // radians per pixel

// ---- Ping / player-made sound (SoundDirector + Stalker.h) ----
extern float kPingCooldown;     // seconds between pings
extern float kPingLoudness;     // how loud a ping is to the stalker (0..1)
extern float kImpactLoudness;   // how loud a bump/impact is to the stalker (0..1)

// ---- Visibility / darkness (was constexpr in Renderer.cpp draw()) ----
extern float kLitPoolRadius;    // radius of the lit ground disc around the feet (m)
extern float kRevealRadius;     // objects visible within this range in blind mode (m)

// Load `path` (key = value lines) into the globals, overriding defaults. Missing
// file or unknown keys are ignored (unknown keys are counted; see loadReport).
// Returns true if the file existed and was read.
bool load(const std::string& path);

// Also pour any `engine <key>=<value>` lines from the file into an EngineTuning,
// so the one file drives the audio engine params as well. Call after load().
void applyEngine(const std::string& path, EngineTuning& eng);

// Poll `path`'s modification time; if it changed since the last call, reload the
// globals (and, if `eng` is non-null, the engine params too). Cheap to call every
// frame. Native live-tweak entry point. Returns true if a reload happened.
bool pollReload(const std::string& path, EngineTuning* eng = nullptr);

// Write a tunables.txt to `path` containing every key with its CURRENT value and
// a header comment. Used to generate the starter file so you never hand-author
// the key list. Returns false on write failure.
bool writeTemplate(const std::string& path);

} // namespace tune
