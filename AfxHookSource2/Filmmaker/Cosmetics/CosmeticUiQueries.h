#pragma once

#include <string>

namespace Filmmaker {

// Cosmetics-side queries for the Camera Editor's CUSTOMIZE (LOADOUT) modal: live
// weapon/glove/agent econ reads for the spectated pawn, surfaced to Panorama as JSON.
// Lives in Cosmetics/ (moved from Panorama/CameraEditorCustomizeState) because these are
// pure entity/econ reads with no HUD/bridge involvement -- the Panorama side (see
// CameraEditorHud.cpp, which stays focused on Panorama build/teardown + per-frame state
// push) only calls them and pushes the JSON into the modal.

// pawnIndex -> full loadout JSON (weapons/gloves/agent model + active-weapon pickup info) for the
// CUSTOMIZE modal. Returns the literal string "null" if pawnIndex isn't a valid player pawn.
std::string BuildCustomizeTargetJson(int pawnIndex);

// { "<pawnIndex>": <BuildCustomizeTargetJson>, ... } for every currently valid player pawn.
std::string BuildCustomizePlayersJson();

// Zeroes every pawn's flashbang whiteout fields for this frame. Called per main-thread frame by
// CameraEditorHud while the Customize modal is open so the flash overlay never covers the modal.
void CustomizeSuppressFlashTick();

} // namespace Filmmaker
