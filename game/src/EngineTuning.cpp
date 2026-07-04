#include "EngineTuning.h"
#include "xyzpan/Types.h"
#include <algorithm>
#include <cmath>
#include <cstring>

const char* const kEngineCategories[] = {
    "LEFT / RIGHT",
    "FRONT / BEHIND",
    "ABOVE / BELOW (PINNA)",
    "BODY / FLOOR BOUNCE",
    "DISTANCE / AIR",
    "REVERB / EARLY REFL",
    "SMOOTHING",
    "BYPASS TOGGLES",
    "OCCLUSION",
};
const int kEngineCategoryCount = 9;

// Defaults mirror xyzpan/Constants.h (engine behavior unchanged when
// untouched), except verb size/decay/damping and erEnabled which reproduce
// the values makeParams previously hardcoded for the outdoor scene, and
// sphereRadius which is the game's audible radius.
const EngineParamDef kEngineParams[EP_Count] = {
    // cat key                      label                    def      min     max      step   toggle
    { 0, "binaural",              "BINAURAL (ITD)",        1.0f,    0,      1,       1,     true },
    { 0, "itd_max_ms",            "MAX ITD MS",            0.72f,   0,      5,       0.02f, false },
    { 0, "ild_max_db",            "ILD MAX DB",            4.5f,    0,      24,      0.25f, false },
    { 0, "head_shadow_min_hz",    "HEAD SHADOW MIN HZ",    2250,    200,    16000,   250,   false },
    { 0, "head_shadow_open_hz",   "HEAD SHADOW OPEN HZ",   16000,   4000,   22000,   500,   false },
    { 0, "azimuth_ear_offset",    "EAR OFFSET L/R",        0.087f,  0,      0.5f,    0.005f,false },

    { 1, "rear_shadow_min_hz",    "REAR SHADOW MIN HZ",    20000,   500,    22000,   250,   false },
    { 1, "rear_ear_offset",       "EAR OFFSET F/B",        0.087f,  0,      0.5f,    0.005f,false },
    { 1, "comb_wet_max",          "REAR COMB WET MAX",     0.15f,   0,      1,       0.01f, false },
    { 1, "presence_shelf_hz",     "PRESENCE SHELF HZ",     3000,    500,    8000,    100,   false },
    { 1, "presence_shelf_db",     "PRESENCE SHELF DB",     1.0f,    0,      6,       0.25f, false },
    { 1, "ear_canal_hz",          "EAR CANAL HZ",          2700,    1000,   5000,    100,   false },
    { 1, "ear_canal_q",           "EAR CANAL Q",           2.0f,    0.3f,   8,       0.1f,  false },
    { 1, "ear_canal_db",          "EAR CANAL MAX DB",      4.0f,    0,      12,      0.25f, false },

    { 2, "elev_ear_offset",       "EAR OFFSET U/D",        0.087f,  0,      0.5f,    0.005f,false },
    { 2, "pinna_n1_min_hz",       "PINNA N1 MIN HZ",       6500,    2000,   12000,   100,   false },
    { 2, "pinna_n1_max_hz",       "PINNA N1 MAX HZ",       10000,   4000,   16000,   100,   false },
    { 2, "pinna_n1_q",            "PINNA N1 Q",            2.0f,    0.3f,   8,       0.1f,  false },
    { 2, "pinna_n2_offset_hz",    "PINNA N2 OFFSET HZ",    3000,    0,      8000,    100,   false },
    { 2, "pinna_n2_gain_db",      "PINNA N2 GAIN DB",      -8,      -24,    0,       0.5f,  false },
    { 2, "pinna_n2_q",            "PINNA N2 Q",            2.0f,    0.3f,   8,       0.1f,  false },
    { 2, "pinna_p1_hz",           "PINNA P1 HZ",           5000,    1000,   12000,   100,   false },
    { 2, "pinna_p1_gain_db",      "PINNA P1 GAIN DB",      2.8f,    0,      12,      0.2f,  false },
    { 2, "pinna_p1_q",            "PINNA P1 Q",            1.5f,    0.3f,   8,       0.1f,  false },
    { 2, "pinna_shelf_hz",        "PINNA SHELF HZ",        4000,    1000,   12000,   100,   false },
    { 2, "concha_hz",             "CONCHA NOTCH HZ",       4000,    1000,   8000,    100,   false },
    { 2, "concha_q",              "CONCHA NOTCH Q",        3.0f,    0.3f,   8,       0.1f,  false },
    { 2, "concha_db",             "CONCHA NOTCH DB",       -8,      -24,    0,       0.5f,  false },
    { 2, "upper_pinna_hz",        "UPPER PINNA HZ",        12000,   6000,   16000,   200,   false },
    { 2, "upper_pinna_q",         "UPPER PINNA Q",         2.0f,    0.3f,   8,       0.1f,  false },
    { 2, "upper_pinna_min_db",    "UPPER PINNA MIN DB",    -4,      -12,    0,       0.5f,  false },
    { 2, "upper_pinna_max_db",    "UPPER PINNA MAX DB",    3,       0,      12,      0.5f,  false },
    { 2, "tragus_hz",             "TRAGUS NOTCH HZ",       8500,    4000,   14000,   100,   false },
    { 2, "tragus_q",              "TRAGUS NOTCH Q",        3.5f,    0.3f,   8,       0.1f,  false },
    { 2, "tragus_db",             "TRAGUS NOTCH DB",       -5,      -24,    0,       0.5f,  false },

    { 3, "chest_delay_ms",        "CHEST DELAY MAX MS",    2,       0,      10,      0.1f,  false },
    { 3, "chest_gain_db",         "CHEST GAIN DB",         -8,      -40,    0,       0.5f,  false },
    { 3, "chest_hpf_hz",          "CHEST HPF HZ",          700,     100,    4000,    50,    false },
    { 3, "chest_lpf_hz",          "CHEST LPF HZ",          1000,    200,    8000,    50,    false },
    { 3, "floor_delay_ms",        "FLOOR DELAY MAX MS",    20,      0,      50,      0.5f,  false },
    { 3, "floor_gain_db",         "FLOOR GAIN DB",         -5,      -40,    0,       0.5f,  false },
    { 3, "floor_abs_hz",          "FLOOR ABSORB HZ",       2000,    500,    16000,   100,   false },
    { 3, "floor_fade_m",          "FLOOR FADE HEIGHT M",   4,       0.5f,   20,      0.25f, false },
    { 3, "shoulder_hz",           "SHOULDER PEAK HZ",      1500,    500,    4000,    50,    false },
    { 3, "shoulder_q",            "SHOULDER PEAK Q",       1.0f,    0.3f,   8,       0.1f,  false },
    { 3, "shoulder_db",           "SHOULDER PEAK DB",      2,       0,      12,      0.25f, false },

    { 4, "dist_floor_db",         "GAIN AT EDGE DB",       -72,     -96,    -12,     1,     false },
    { 4, "dist_gain_max",         "CLOSE BOOST MAX X",     2,       1,      4,       0.05f, false },
    { 4, "dist_curve_steep",      "CURVE STEEPNESS",       0,       0,      1,       0.02f, false },
    { 4, "air_abs_min_hz",        "AIR ABS FAR HZ",        8000,    1000,   22000,   250,   false },
    { 4, "air_abs_max_hz",        "AIR ABS NEAR HZ",       22000,   4000,   22000,   250,   false },
    { 4, "air_abs2_min_hz",       "AIR ABS2 FAR HZ",       12000,   1000,   22000,   250,   false },
    { 4, "air_abs2_max_hz",       "AIR ABS2 NEAR HZ",      22000,   4000,   22000,   250,   false },
    { 4, "near_lf_hz",            "NEARFIELD LF HZ",       200,     50,     1000,    10,    false },
    { 4, "near_lf_db",            "NEARFIELD LF MAX DB",   6,       0,      12,      0.25f, false },
    { 4, "dist_smooth_ms",        "DOPPLER SMOOTH MS",     150,     1,      500,     5,     false },

    { 5, "verb_size",             "SIZE",                  0.4f,    0,      1,       0.02f, false },
    { 5, "verb_decay",            "DECAY",                 0.25f,   0,      1,       0.02f, false },
    { 5, "verb_damping",          "DAMPING",               0.6f,    0,      1,       0.02f, false },
    { 5, "verb_predelay_ms",      "PREDELAY MAX MS",       50,      0,      50,      1,     false },
    { 5, "verb_mod",              "MOD DEPTH",             0.5f,    0,      1,       0.02f, false },
    { 5, "verb_diffusion",        "DIFFUSION",             0.7f,    0,      1,       0.02f, false },
    { 5, "er_enabled",            "EARLY REFLECTIONS",     0,       0,      1,       1,     true },
    { 5, "er_room_m",             "ER ROOM SIZE M",        5,       1,      30,      0.5f,  false },
    { 5, "er_damping",            "ER DAMPING",            0.903f,  0,      1,       0.01f, false },
    { 5, "er_level",              "ER LEVEL",              0.5f,    0,      1,       0.02f, false },
    { 5, "er_verb_send",          "ER VERB SEND",          0.7f,    0,      1,       0.02f, false },

    { 6, "smooth_itd_ms",         "ITD SMOOTH MS",         8,       0.5f,   100,     0.5f,  false },
    { 6, "smooth_filter_ms",      "FILTER SMOOTH MS",      5,       0.5f,   100,     0.5f,  false },
    { 6, "smooth_gain_ms",        "GAIN SMOOTH MS",        5,       0.5f,   100,     0.5f,  false },

    { 7, "bypass_itd",            "BYPASS ITD",            0, 0, 1, 1, true },
    { 7, "bypass_head_shadow",    "BYPASS HEAD SHADOW",    0, 0, 1, 1, true },
    { 7, "bypass_ild",            "BYPASS ILD",            0, 0, 1, 1, true },
    { 7, "bypass_near_field",     "BYPASS NEAR FIELD",     0, 0, 1, 1, true },
    { 7, "bypass_rear_shadow",    "BYPASS REAR SHADOW",    0, 0, 1, 1, true },
    { 7, "bypass_pinna_eq",       "BYPASS PINNA EQ",       0, 0, 1, 1, true },
    { 7, "bypass_expanded_pinna", "BYPASS EXP. PINNA",     0, 0, 1, 1, true },
    { 7, "bypass_comb",           "BYPASS REAR COMB",      0, 0, 1, 1, true },
    { 7, "bypass_chest",          "BYPASS CHEST BOUNCE",   0, 0, 1, 1, true },
    { 7, "bypass_floor",          "BYPASS FLOOR BOUNCE",   0, 0, 1, 1, true },
    { 7, "bypass_dist_gain",      "BYPASS DISTANCE GAIN",  0, 0, 1, 1, true },
    { 7, "bypass_doppler",        "BYPASS DOPPLER",        0, 0, 1, 1, true },
    { 7, "bypass_air_abs",        "BYPASS AIR ABSORB",     0, 0, 1, 1, true },
    { 7, "bypass_reverb",         "BYPASS REVERB",         0, 0, 1, 1, true },
    { 7, "bypass_er",             "BYPASS EARLY REFL",     0, 0, 1, 1, true },

    { 8, "occl_enabled",          "OCCLUSION",             1,       0,      1,       1,     true },
    { 8, "occl_atten_scale",      "ATTEN SCALE X",         1.0f,    0,      3,       0.05f, false },
    { 8, "occl_max_atten_db",     "MAX ATTEN DB",          25,      0,      60,      1,     false },
    { 8, "occl_cutoff_scale",     "CUTOFF SCALE X",        1.0f,    0.1f,   8,       0.05f, false },
    { 8, "occl_min_cutoff_hz",    "MIN CUTOFF HZ",         300,     100,    8000,    50,    false },
    { 8, "occl_ref_freq_hz",      "GAIN REF FREQ HZ",      250,     50,     2000,    25,    false },
    { 8, "occl_fresnel_hz",       "FRESNEL ZONE HZ",       6000,    500,    16000,   250,   false },
    { 8, "occl_thickness",        "WIDTH (C3) FACTOR",     1,       0,      1,       1,     true },
    { 8, "occl_smooth_ms",        "SMOOTH MS",             100,     10,     1000,    10,    false },
};

EngineTuning::EngineTuning() {
    for (int i = 0; i < EP_Count; ++i) v[i] = kEngineParams[i].def;
}

bool EngineTuning::isDefault(int id) const {
    return std::abs(v[id] - kEngineParams[id].def) < 1e-5f;
}

void EngineTuning::reset(int id) { v[id] = kEngineParams[id].def; }

void EngineTuning::adjust(int id, int dir) {
    const EngineParamDef& d = kEngineParams[id];
    if (d.toggle)
        v[id] = (v[id] > 0.5f) ? 0.0f : 1.0f;
    else
        v[id] = std::clamp(v[id] + d.step * static_cast<float>(dir), d.min, d.max);
}

bool EngineTuning::setByKey(const char* key, float value) {
    for (int i = 0; i < EP_Count; ++i)
        if (std::strcmp(kEngineParams[i].key, key) == 0) {
            v[i] = std::clamp(value, kEngineParams[i].min, kEngineParams[i].max);
            return true;
        }
    return false;
}

int EngineTuning::changedInCategory(int category) const {
    int n = 0;
    for (int i = 0; i < EP_Count; ++i)
        if (kEngineParams[i].category == category && !isDefault(i)) ++n;
    return n;
}

void EngineTuning::apply(xyzpan::EngineParams& p) const {
    auto on = [this](int id) { return v[id] > 0.5f; };

    p.binauralEnabled      = on(EP_Binaural);
    p.maxITD_ms            = v[EP_MaxITD];
    p.ildMaxDb             = v[EP_ILDMaxDb];
    p.headShadowMinHz      = v[EP_HeadShadowMinHz];
    p.headShadowFullOpenHz = v[EP_HeadShadowOpenHz];
    p.azimuthEarOffset     = v[EP_AzimuthEarOffset];

    p.rearShadowMinHz      = v[EP_RearShadowMinHz];
    p.rearEarOffset        = v[EP_RearEarOffset];
    p.combWetMax           = v[EP_CombWetMax];
    p.presenceShelfFreqHz  = v[EP_PresenceShelfHz];
    p.presenceShelfMaxDb   = v[EP_PresenceShelfDb];
    p.earCanalFreqHz       = v[EP_EarCanalHz];
    p.earCanalQ            = v[EP_EarCanalQ];
    p.earCanalMaxDb        = v[EP_EarCanalDb];

    p.elevEarOffset        = v[EP_ElevEarOffset];
    p.pinnaN1MinHz         = v[EP_PinnaN1MinHz];
    p.pinnaN1MaxHz         = v[EP_PinnaN1MaxHz];
    p.pinnaNotchQ          = v[EP_PinnaN1Q];
    p.pinnaN2OffsetHz      = v[EP_PinnaN2OffsetHz];
    p.pinnaN2GainDb        = v[EP_PinnaN2GainDb];
    p.pinnaN2Q             = v[EP_PinnaN2Q];
    p.pinnaP1FreqHz        = v[EP_PinnaP1Hz];
    p.pinnaP1GainDb        = v[EP_PinnaP1GainDb];
    p.pinnaP1Q             = v[EP_PinnaP1Q];
    p.pinnaShelfFreqHz     = v[EP_PinnaShelfHz];
    p.conchaNotchFreqHz    = v[EP_ConchaHz];
    p.conchaNotchQ         = v[EP_ConchaQ];
    p.conchaNotchMaxDb     = v[EP_ConchaDb];
    p.upperPinnaFreqHz     = v[EP_UpperPinnaHz];
    p.upperPinnaQ          = v[EP_UpperPinnaQ];
    p.upperPinnaMinDb      = v[EP_UpperPinnaMinDb];
    p.upperPinnaMaxDb      = v[EP_UpperPinnaMaxDb];
    p.tragusNotchFreqHz    = v[EP_TragusHz];
    p.tragusNotchQ         = v[EP_TragusQ];
    p.tragusNotchMaxDb     = v[EP_TragusDb];

    p.chestDelayMaxMs      = v[EP_ChestDelayMs];
    p.chestGainDb          = v[EP_ChestGainDb];
    p.chestHPFHz           = v[EP_ChestHPFHz];
    p.chestLPHz            = v[EP_ChestLPFHz];
    p.floorDelayMaxMs      = v[EP_FloorDelayMs];
    p.floorGainDb          = v[EP_FloorGainDb];
    p.floorAbsHz           = v[EP_FloorAbsHz];
    p.shoulderPeakFreqHz   = v[EP_ShoulderHz];
    p.shoulderPeakQ        = v[EP_ShoulderQ];
    p.shoulderPeakMaxDb    = v[EP_ShoulderDb];

    // sphereRadius is per-source (SoundRule::audibleRadius), set in makeParams.
    p.distGainFloorDb      = v[EP_DistGainFloorDb];
    p.distGainMax          = v[EP_DistGainMax];
    p.distCurveSteep       = v[EP_DistCurveSteep];
    p.airAbsMinHz          = v[EP_AirAbsMinHz];
    p.airAbsMaxHz          = v[EP_AirAbsMaxHz];
    p.airAbs2MinHz         = v[EP_AirAbs2MinHz];
    p.airAbs2MaxHz         = v[EP_AirAbs2MaxHz];
    p.nearFieldLFHz        = v[EP_NearFieldLFHz];
    p.nearFieldLFMaxDb     = v[EP_NearFieldLFDb];
    p.distSmoothMs         = v[EP_DistSmoothMs];

    p.verbSize             = v[EP_VerbSize];
    p.verbDecay            = v[EP_VerbDecay];
    p.verbDamping          = v[EP_VerbDamping];
    p.verbPreDelayMax      = v[EP_VerbPreDelayMs];
    p.verbModDepth         = v[EP_VerbModDepth];
    p.verbDiffusion        = v[EP_VerbDiffusion];
    p.erEnabled            = on(EP_EREnabled);
    p.erRoomSize           = v[EP_ERRoomSize];
    p.erDamping            = v[EP_ERDamping];
    p.erLevel              = v[EP_ERLevel];
    p.erReverbSend         = v[EP_ERVerbSend];

    p.smoothMs_ITD         = v[EP_SmoothITDMs];
    p.smoothMs_Filter      = v[EP_SmoothFilterMs];
    p.smoothMs_Gain        = v[EP_SmoothGainMs];

    p.bypassITD            = on(EP_BypassITD);
    p.bypassHeadShadow     = on(EP_BypassHeadShadow);
    p.bypassILD            = on(EP_BypassILD);
    p.bypassNearField      = on(EP_BypassNearField);
    p.bypassRearShadow     = on(EP_BypassRearShadow);
    p.bypassPinnaEQ        = on(EP_BypassPinnaEQ);
    p.bypassExpandedPinna  = on(EP_BypassExpandedPinna);
    p.bypassComb           = on(EP_BypassComb);
    p.bypassChest          = on(EP_BypassChest);
    p.bypassFloor          = on(EP_BypassFloor);
    p.bypassDistGain       = on(EP_BypassDistGain);
    p.bypassDoppler        = on(EP_BypassDoppler);
    p.bypassAirAbs         = on(EP_BypassAirAbs);
    p.bypassReverb         = on(EP_BypassReverb);
    p.bypassER             = on(EP_BypassER);

    // EP_Occl* are game-side (occlusion happens before the engine); AudioWorld
    // reads them directly. Nothing to pour into EngineParams.
}
