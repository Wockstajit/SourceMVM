#pragma once

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Read-only probe for the Customize modal's MapPlayerPreviewPanel scene (the `ui/buy_menu` spawn
// group created in CameraEditorCustomizePreviewJs.h). It answers exactly one question, the one the
// "idle animation is frozen while the demo is paused" investigation is blocked on:
//
//     does the preview character's animation advance on a clock the demo pause gates, or not?
//
// This is INSTRUMENTATION ONLY. It never writes game memory and never installs a detour. Per
// CLAUDE.md's mandatory rule it exists so the detour target is chosen from a log instead of a guess
// -- approach 1 ("preview entities are ticked by a separate path; find the gate") vs approach 2
// ("shared path; feed the preview scene a wall-clock dt while the demo is paused") in
// Docs/panorama/panorama-player-preview-summary.md. The two are different functions, so guessing is a wasted
// detour.
//
// Standalone module (CLAUDE.md: never couple a new thing into an existing thing) -- it owns its own
// console command (`mvm_previewprobe`), its own per-frame pump, and its own entity resolver.
//
// Why its own resolver: the preview character is a CLIENT-ONLY entity at an index above 0x4000.
// GetHighestEntityIndex() is a hardcoded `return 2048` stub (ClientEntitySystem.cpp), so every
// helper that clamps to it -- Filmmaker::CosmeticEntFromIndex and CosmeticDebugRead::EntFromIndex --
// is blind to the whole client-only range. The engine accessor itself is NOT so bounded; the working
// pattern is modelswap::EntFromIndex (full range, guards only
// index >= 0x8000), which is what this reuses.
//
// Confirmed live 2026-07-29 (CS2 build 2000879), demo playing: the preview pawn is class
// `csgo_previewplayer`, clientclass `C_CSPlayerPawn`, resolvable at client-only indices (16389 /
// 16395 / 16897 on that run). Opening the Customize modal added 74 entities to the scan.

// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame. No-op unless `watch` is armed.
void PreviewSceneProbe_RunFrame();

// `mvm_previewprobe` implementation (registered in the .cpp).
void PreviewSceneProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
