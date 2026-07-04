#pragma once
// Scene persistence: a simple line-based text format so dev-mode edits
// survive restarts and diffs stay readable. Sounds are stored by set NAME
// (not id), so the file survives library reordering; unknown names load as
// unassigned with a console warning.
//
//   # audio_game scene v1
//   material 0 name=grass sound=grass gain=0.70 pitchjit=0.050 gainjit=2.00
//   floorrow 50 0*12 1*20 0*48
//   tree 1.50 -2.00 trunkr=0.40 trunkh=2.00 canopyr=1.60 canopyh=2.60
//   rule impact sound=knock enabled=1 gain=0.55 ...
//   boulder 3.00 -10.00 0.50 0.60 0.50 0.45
//   emitter -7.00 8.00 0.10
//   rule loop sound=water ...
//
// `rule` lines attach to the most recent object line. Only slots with an
// assigned sound are written. `floorrow` lines are the painted floor grid,
// run-length encoded as material*count per row; all-default rows are omitted.
// The same format backs the dev-mode WORLDS browser (worlds/*.txt slots).

// Engine tuning (dev panel) rides along as "engine <key>=<value>" lines,
// non-default values only.

#include "Scene.h"
#include "EngineTuning.h"
#include <string>

class SoundLibrary;

bool saveScene(const Scene& scene, const SoundLibrary& lib,
               const EngineTuning& tuning, const std::string& path);

// Returns false (scene/tuning untouched) if the file is missing or unreadable.
bool loadScene(Scene& scene, const SoundLibrary& lib,
               EngineTuning& tuning, const std::string& path);
