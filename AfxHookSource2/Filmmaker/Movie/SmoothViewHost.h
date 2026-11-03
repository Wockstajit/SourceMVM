#pragma once

// What Smooth View (SmoothView.cpp) reads from the game it runs in, so one implementation builds into
// CS2's hook and the Source 1 hook (GAME_SOURCE1), as MvmDebugLogHost.h does for the debug log. Each
// hook defines these once: SmoothViewHostSource2.cpp for CS2, AfxHookSource/Filmmaker/SourceSmoothView.cpp
// for Source 1.

namespace Filmmaker {

// The viewed player's real velocity (units/s) and whether it stands on the ground, for the camera at
// x y z. False when the game cannot say; Smooth View then measures the camera's own movement.
bool SmoothViewHost_PawnMotion(double x, double y, double z, double velocity[3], bool* onGround);

// The render frame's real duration in seconds and its number. Smooth View's breathing and smoothing
// step once per rendered frame.
void SmoothViewHost_RenderFrame(double& frameTime, int& frameCount);

} // namespace Filmmaker
