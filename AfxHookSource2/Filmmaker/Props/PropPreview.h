#pragma once

#include <string>

// PROPS PREVIEW: the CS2 ASSETS page shows the model being browsed, turning, in a native
// MapPlayerPreviewPanel (the death-animation picker's panel). Native preview scenes only spawn a
// model while the demo PLAYS, and a dressed shot is usually paused, so the panel is given a marker
// agent (kPropPreviewMarkerModel), which its pooled preview pawn wears paused or not, and this module
// swaps that pawn's model to the asset (client-side SetModel). It then scales the pawn so the
// asset's bounds fill the frame (measured on a throwaway prop_dynamic: the pawn's own bounds are the
// player hull), keeps it drawing while paused, and turns it by writing its scene node's transform in
// the late frame -- the panel has no rotation call, and a static model has no bones to pose.

namespace Filmmaker {

// The agent the CS2 ASSETS panel is created with. Rarely worn in demos, so its preview pawn is
// unambiguous; ConfigHudPropsJs.h passes the same path.
constexpr const char* kPropPreviewMarkerModel = "agents/models/tm_jungle_raider/tm_jungle_raider_variantb2.vmdl";

// The model the panel is showing ("" stops). Main thread.
void PropPreview_Set(const std::string& model);
const std::string& PropPreview_Model();
// A model is being previewed: arms the paused-demo render gates for preview panels (Filmmaker.cpp),
// as the death-animation preview does.
bool PropPreview_Active();
// Main-thread pump: measures the asset, finds the panel's pawn, puts the asset on it, keeps its fit.
void PropPreview_RunFrame();
// Late main-thread pump (after the client's FRAME_RENDER_PASS, like DeathAnimPreview): turns it.
void PropPreview_RunLateFrame();
// Preview fields for the props state (inserted into its object).
void PropPreview_AppendJson(std::string& out);

} // namespace Filmmaker
