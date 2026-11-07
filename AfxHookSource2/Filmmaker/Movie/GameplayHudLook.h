#pragma once

// GAMEPLAY HUD LOOK: the "In-game" HUD view shows the watched player's HUD the way they saw it
// while playing, not CS2's spectator version of it (issue #116).
//
// Watching a demo makes the viewer a spectator, and CS2 draws its HUD differently for spectators:
// a square radar with every player on it, and the viewer's own crosshair. A frag movie wants the
// player's screen: the round radar, only the enemies their team had spotted, and their crosshair.
// CS2 has a console variable for each of those differences, so the In-game view sets them while it
// is on screen and puts the viewer's own values back the moment it is not. The spectator panels
// (name bar, avatar, vignette) are hidden by the same view in the timeline JS (applyHudView).
//
// No setting of its own: the In-game view is the switch, in the editor, CONFIG and the render
// job's GAME HUD alike (all three resolve to CameraEditorHud::GetHudView). Values are written on
// the on and off edges only, never per frame, so the console is not spammed and nothing fights a
// value changed by hand while the view is up.

namespace Filmmaker {

// Main thread, once per frame, after every HUD panel ran. `inGameViewShown` is true while the
// In-game view is actually on screen: a demo is loaded, the Camera Editor or CONFIG is up, the
// view resolves to In-game, and the F8 blackout is off.
void GameplayHudLook_RunFrame(bool inGameViewShown);

// True while the player-look values are applied (for health and the debug log).
bool GameplayHudLook_Applied();

} // namespace Filmmaker
