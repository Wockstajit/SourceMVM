#pragma once

// One question, asked by the WndProc hook before any filmmaker input tap runs: does a foreign
// UI own the screen right now? While it does, EVERY director control stands down -- plain
// scroll must not cycle First/Third/Free, Space must not toggle the demo, clicks must not
// switch players, WASD must not fly the cam, and the K/J/L/F/X/Z/F8 hotkeys must not fire.
// Input passes straight through instead; the foreign UI handles it itself.
//
// Three foreign UIs are tracked (reported 2026-08-31: "everything goes to the game -- when you
// press pause you're actually in the menu"):
//
//  * CS2's developer console, from the engine's own IGameUIService::Con_IsVisible.
//  * CS2's own pause/main menu. Signalled by CS2's own Panorama events (CSGOShowPauseMenu /
//    CSGOHidePauseMenu and the MainMenu pair): a tiny JS subscriber installed in the stable
//    menu context by FilmmakerMenu records the level in a panel attribute, and the menu pump
//    polls it here every frame. Two cheaper signals were tried first and MEASURED WRONG: the
//    CSGOMainMenu render pass also runs during plain demo playback, and the engine's own
//    CCSGOInput::MouseInputEnabled is false for ALL demo spectating, menu or not.
//  * The ReShade overlay. ReShade has no is-open export, so its toggle key (ReShade.ini
//    [INPUT] KeyOverlay, default Home) is mirrored; pressing it twice resyncs a drifted
//    mirror. Armed only when a ReShade module with addon support is actually loaded.

namespace Filmmaker {

// WndProc thread: true while any foreign UI is believed to own input.
bool UiInputGate_Active();

// WndProc thread: feed every WM_KEYDOWN (never consumes) so the ReShade toggle key is tracked.
void UiInputGate_OnKeyDown(unsigned vk, long long lParam);

// Main thread (menu pump), every frame while the menu JS context is alive: the current level
// of CS2's pause/main-menu visibility, as reported by its own Panorama show/hide events.
void UiInputGate_SupplyPauseMenuOpen(bool open);

} // namespace Filmmaker
