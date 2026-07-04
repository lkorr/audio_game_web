#include "Tunables.h"
#include "EngineTuning.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

namespace tune {

// ---- Definitions + defaults (the values that used to be hardcoded) ----
float kHearThreshold = 0.06f;
float kChaseThreshold = 0.30f;
float kAlertHold = 6.0f;
float kPatrolSpeed = 1.1f;
float kInvestigateSpeed = 1.9f;
float kSearchSpeed = 1.5f;
float kChaseSpeed = 2.7f;
float kArrive = 1.0f;
float kMusicPush = 3.0f;
float kSearchHold = 10.0f;
float kSearchRadius = 8.0f;
float kLungeRange = 5.0f;
float kLungeOvershoot = 3.0f;
float kLungeSpeedMult = 4.0f;
float kLungeArrive = 0.8f;
float kLungeMaxTime = 1.2f;
float kConfusedHold = 1.6f;
float kCueInterval = 3.5f;
float kMaskScale = 2.5f;
float kMaskRadius = 14.0f;
float kHearRadius = 28.0f;
float kHearWindow = 3.0f;
float kStepDistance = 0.8f;
float kChaseStepGain = 0.9f;
float kPatrolStepGain = 0.7f;
float kStepSilentSpeed = 0.15f;
float kStalkerSkin = 0.6f;

float kCatchDist = 1.6f;
float kBannerHold = 2.5f;
float kGoalRadius = 3.0f;

float kPlayerEyeHeight = 1.7f;
float kPlayerRadius = 0.35f;
float kPlayerWalkSpeed = 1.6f;
float kPlayerSprintSpeed = 3.0f;
float kPlayerStepDistance = 0.75f;
float kPlayerMouseSens = 0.0026f;

float kPingCooldown = 1.2f;
float kPingLoudness = 1.0f;
float kImpactLoudness = 0.7f;

float kLitPoolRadius = 1.4f;
float kRevealRadius = 2.0f;

namespace {

// The single source of truth binding a text key to its storage. load() and
// writeTemplate() both walk this, so adding a knob is one line here + the extern.
struct Entry { const char* key; float* value; const char* comment; };

const Entry kEntries[] = {
    // -- Stalker AI --
    { "stalker.hearThreshold",   &kHearThreshold,   "min perceived loudness to react to" },
    { "stalker.chaseThreshold",  &kChaseThreshold,  "perceived loudness meaning 'close' -> Chase" },
    { "stalker.alertHold",       &kAlertHold,       "seconds to pursue after last hearing" },
    { "stalker.patrolSpeed",     &kPatrolSpeed,     "m/s wandering" },
    { "stalker.investigateSpeed",&kInvestigateSpeed,"m/s homing on a cue" },
    { "stalker.searchSpeed",     &kSearchSpeed,     "m/s sweeping the area" },
    { "stalker.chaseSpeed",      &kChaseSpeed,      "m/s chasing (keep < player sprint)" },
    { "stalker.arrive",          &kArrive,          "within this of a cue = arrived (m)" },
    { "stalker.musicPush",       &kMusicPush,       "outward accel scale at a sanctuary edge" },
    { "stalker.searchHold",      &kSearchHold,      "seconds spent searching the area" },
    { "stalker.searchRadius",    &kSearchRadius,    "how far around lastHeard it sweeps (m)" },
    { "stalker.lungeRange",      &kLungeRange,      "within this of the cue -> Attack (m)" },
    { "stalker.lungeOvershoot",  &kLungeOvershoot,  "metres past the locked spot the lunge drives" },
    { "stalker.lungeSpeedMult",  &kLungeSpeedMult,  "lunge speed = chase speed * this" },
    { "stalker.lungeArrive",     &kLungeArrive,     "within this of lungeTarget = done (m)" },
    { "stalker.lungeMaxTime",    &kLungeMaxTime,    "hard cap on one lunge, seconds" },
    { "stalker.confusedHold",    &kConfusedHold,    "stunned pause after a lunge, seconds" },
    { "stalker.cueInterval",     &kCueInterval,     "seconds between repeated mood calls" },
    { "stalker.maskScale",       &kMaskScale,       "ambient weight in the masking gate" },
    { "stalker.maskRadius",      &kMaskRadius,      "ignore ambient sources beyond this (m)" },
    { "stalker.hearRadius",      &kHearRadius,      "ignore player sounds beyond this (m)" },
    { "stalker.hearWindow",      &kHearWindow,      "seconds a heard cue stays 'the' cue" },
    { "stalker.stepDistance",    &kStepDistance,    "travel between stalker footsteps (m)" },
    { "stalker.chaseStepGain",   &kChaseStepGain,   "footstep gain when Chasing/Attacking" },
    { "stalker.patrolStepGain",  &kPatrolStepGain,  "footstep gain when prowling" },
    { "stalker.stepSilentSpeed", &kStepSilentSpeed, "m/s below which it makes no footsteps" },
    { "stalker.skin",            &kStalkerSkin,     "stalker radius for boulder avoidance (m)" },
    // -- Slice / game rules --
    { "slice.catchDist",         &kCatchDist,       "stalker within this of the eye = caught (m)" },
    { "slice.bannerHold",        &kBannerHold,      "seconds the outcome banner shows" },
    { "slice.goalRadius",        &kGoalRadius,      "within this of the goal = win (m)" },
    // -- Player --
    { "player.eyeHeight",        &kPlayerEyeHeight, "camera height (m)" },
    { "player.radius",           &kPlayerRadius,    "player collision radius (m)" },
    { "player.walkSpeed",        &kPlayerWalkSpeed, "m/s" },
    { "player.sprintSpeed",      &kPlayerSprintSpeed,"m/s" },
    { "player.stepDistance",     &kPlayerStepDistance,"travel between player footsteps (m)" },
    { "player.mouseSens",        &kPlayerMouseSens, "radians per pixel" },
    // -- Ping / player sound --
    { "ping.cooldown",           &kPingCooldown,    "seconds between pings" },
    { "ping.loudness",           &kPingLoudness,    "how loud a ping is to the stalker (0..1)" },
    { "ping.impactLoudness",     &kImpactLoudness,  "how loud a bump is to the stalker (0..1)" },
    // -- Visibility / darkness --
    { "view.litPoolRadius",      &kLitPoolRadius,   "radius of the lit ground disc (m)" },
    { "view.revealRadius",       &kRevealRadius,    "objects visible within this in blind mode (m)" },
};
constexpr int kEntryCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));

// Trim ASCII whitespace from both ends (in place on a std::string view range).
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
    return s.substr(a, b - a);
}

float* findEntry(const std::string& key) {
    for (const Entry& e : kEntries)
        if (key == e.key) return e.value;
    return nullptr;
}

// mtime of `path` in seconds since epoch, or 0 if it does not exist.
long long fileMTime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<long long>(st.st_mtime);
}

long long g_lastMTime = -1;   // -1 = never loaded

} // namespace

bool load(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;

    char line[512];
    int applied = 0, unknown = 0;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        // `engine <key>=<value>` lines are handled in applyEngine(), skip here.
        if (s.rfind("engine ", 0) == 0) continue;

        // Accept `key = value` or `key value`.
        size_t eq = s.find_first_of("=");
        std::string key, val;
        if (eq != std::string::npos) {
            key = trim(s.substr(0, eq));
            val = trim(s.substr(eq + 1));
        } else {
            size_t sp = s.find_first_of(" \t");
            if (sp == std::string::npos) continue;
            key = trim(s.substr(0, sp));
            val = trim(s.substr(sp + 1));
        }
        float* slot = findEntry(key);
        if (slot == nullptr) { ++unknown; continue; }
        *slot = static_cast<float>(std::atof(val.c_str()));
        ++applied;
    }
    std::fclose(f);
    std::printf("[tunables] loaded %s (%d set, %d unknown)\n", path.c_str(), applied, unknown);
    return true;
}

void applyEngine(const std::string& path, EngineTuning& eng) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return;
    char line[512];
    int applied = 0;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.rfind("engine ", 0) != 0) continue;
        s = trim(s.substr(7));                 // drop "engine "
        size_t eq = s.find_first_of("=");
        if (eq == std::string::npos) continue;
        std::string key = trim(s.substr(0, eq));
        float v = static_cast<float>(std::atof(trim(s.substr(eq + 1)).c_str()));
        if (eng.setByKey(key.c_str(), v)) ++applied;
    }
    std::fclose(f);
    if (applied > 0)
        std::printf("[tunables] applied %d engine params from %s\n", applied, path.c_str());
}

bool pollReload(const std::string& path, EngineTuning* eng) {
    const long long m = fileMTime(path);
    if (m == 0) return false;               // no file
    if (m == g_lastMTime) return false;     // unchanged
    g_lastMTime = m;
    load(path);
    if (eng != nullptr) applyEngine(path, *eng);
    return true;
}

bool writeTemplate(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    std::fprintf(f,
        "# Live tunables. Edit and save while the NATIVE game runs (--slice) and it\n"
        "# reloads within a frame -- no rebuild. `key = value`, one per line; '#'\n"
        "# comments. Unknown keys are ignored. Engine params: `engine <key>=<value>`\n"
        "# (see the F2 dev menu / EngineTuning for keys). Delete a line to restore\n"
        "# its built-in default.\n\n");
    std::string lastPrefix;
    for (const Entry& e : kEntries) {
        // Blank line + banner between groups (split on the key prefix before '.').
        const char* dot = std::strchr(e.key, '.');
        std::string prefix = dot ? std::string(e.key, dot) : std::string(e.key);
        if (prefix != lastPrefix) {
            std::fprintf(f, "\n# --- %s ---\n", prefix.c_str());
            lastPrefix = prefix;
        }
        std::fprintf(f, "%-26s = %-10.5g  # %s\n", e.key, *e.value, e.comment);
    }
    std::fclose(f);
    std::printf("[tunables] wrote template %s (%d keys)\n", path.c_str(), kEntryCount);
    return true;
}

} // namespace tune
