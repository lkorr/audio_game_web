#pragma once
// Live-tunable xyzpan engine parameters -- the game-side incarnation of the
// VST's dev panel. One table (kEngineParams) is the single source of truth:
// the dev menu renders and edits it, SceneIO persists it (by key, non-default
// values only), and EngineTuning::apply() pours it into EngineParams inside
// AudioWorld::makeParams() every frame, so edits are audible immediately on
// every voice.
//
// Defaults are chosen to reproduce the game's current sound exactly (engine
// defaults, except the reverb/ER values makeParams used to hardcode), so an
// untouched tuning is a no-op.
//
// Per-source fields (position, listener pose, verbWet, dopplerEnabled, the
// distDelayMaxMs doppler workaround) stay in makeParams and are NOT here.

#include <cstdint>

namespace xyzpan { struct EngineParams; }

// Indexes into EngineTuning::v and kEngineParams. Order defines menu order.
enum EngineParamId : int {
    // Left / right (azimuth)
    EP_Binaural = 0, EP_MaxITD, EP_ILDMaxDb, EP_HeadShadowMinHz,
    EP_HeadShadowOpenHz, EP_AzimuthEarOffset,
    // Front / behind
    EP_RearShadowMinHz, EP_RearEarOffset, EP_CombWetMax,
    EP_PresenceShelfHz, EP_PresenceShelfDb,
    EP_EarCanalHz, EP_EarCanalQ, EP_EarCanalDb,
    // Above / below (pinna)
    EP_ElevEarOffset, EP_PinnaN1MinHz, EP_PinnaN1MaxHz, EP_PinnaN1Q,
    EP_PinnaN2OffsetHz, EP_PinnaN2GainDb, EP_PinnaN2Q,
    EP_PinnaP1Hz, EP_PinnaP1GainDb, EP_PinnaP1Q, EP_PinnaShelfHz,
    EP_ConchaHz, EP_ConchaQ, EP_ConchaDb,
    EP_UpperPinnaHz, EP_UpperPinnaQ, EP_UpperPinnaMinDb, EP_UpperPinnaMaxDb,
    EP_TragusHz, EP_TragusQ, EP_TragusDb,
    // Body / floor bounce. EP_FloorFadeHeight is game-side: it maps source
    // height above ground to EngineParams::floorBounceFactor in makeParams
    // (bounce is full at ground level, gone at this height).
    EP_ChestDelayMs, EP_ChestGainDb, EP_ChestHPFHz, EP_ChestLPFHz,
    EP_FloorDelayMs, EP_FloorGainDb, EP_FloorAbsHz, EP_FloorFadeHeight,
    EP_ShoulderHz, EP_ShoulderQ, EP_ShoulderDb,
    // Distance / air. (No global audible radius here: sphereRadius is a
    // per-object SoundRule field -- the sound's size -- edited in the object
    // menu, not the engine panel.)
    EP_DistGainFloorDb, EP_DistGainMax, EP_DistCurveSteep,
    EP_AirAbsMinHz, EP_AirAbsMaxHz, EP_AirAbs2MinHz, EP_AirAbs2MaxHz,
    EP_NearFieldLFHz, EP_NearFieldLFDb, EP_DistSmoothMs,
    // Reverb / early reflections
    EP_VerbSize, EP_VerbDecay, EP_VerbDamping, EP_VerbPreDelayMs,
    EP_VerbModDepth, EP_VerbDiffusion,
    EP_EREnabled, EP_ERRoomSize, EP_ERDamping, EP_ERLevel, EP_ERVerbSend,
    // Smoothing
    EP_SmoothITDMs, EP_SmoothFilterMs, EP_SmoothGainMs,
    // Bypass toggles
    EP_BypassITD, EP_BypassHeadShadow, EP_BypassILD, EP_BypassNearField,
    EP_BypassRearShadow, EP_BypassPinnaEQ, EP_BypassExpandedPinna,
    EP_BypassComb, EP_BypassChest, EP_BypassFloor, EP_BypassDistGain,
    EP_BypassDoppler, EP_BypassAirAbs, EP_BypassReverb, EP_BypassER,
    // Occlusion. Game-side like EP_FloorFadeHeight: not poured into
    // EngineParams; AudioWorld reads these into an OcclusionTuning for
    // computeOcclusion() and the Source-side filter smoothing.
    EP_OcclEnabled, EP_OcclAttenScale, EP_OcclMaxAttenDb, EP_OcclCutoffScale,
    EP_OcclMinCutoffHz, EP_OcclRefFreqHz, EP_OcclFresnelHz, EP_OcclThickness,
    EP_OcclSmoothMs,

    EP_Count,
};

struct EngineParamDef {
    int category;        // index into kEngineCategories
    const char* key;     // scene.txt key ("engine <key>=<value>")
    const char* label;   // overlay label
    float def, min, max, step;
    bool toggle;         // value is 0/1, rendered ON/OFF
};

extern const EngineParamDef kEngineParams[EP_Count];
extern const char* const kEngineCategories[];
extern const int kEngineCategoryCount;

struct EngineTuning {
    float v[EP_Count];

    EngineTuning();                 // table defaults
    void apply(xyzpan::EngineParams& p) const;

    bool isDefault(int id) const;
    void reset(int id);
    // Step the value by dir steps (toggle: flip), clamped to [min, max].
    void adjust(int id, int dir);
    // Set by scene.txt key; false if the key is unknown.
    bool setByKey(const char* key, float value);
    // Number of non-default params in a category (menu annotation).
    int changedInCategory(int category) const;
};
