#pragma once

#include "CamMarkers.h"
#include "../Demo/DemoProjectStoreMath.h" // ProjectGame

#include <string>
#include <vector>

namespace Filmmaker {

// What path shake (CamPathShake.cpp, CamPathShakeCommand.cpp) asks the game it runs in: the camera
// path its keys belong to, and where the shakes are saved. The store, its snapshot and RageShake's
// evaluation are the same in every game. CS2 answers in CamPathShakeHostSource2.cpp, CS:S in
// AfxHookSource/Filmmaker/SourceCameraPath.cpp.

// The camera path's keys. Main thread only.
const std::vector<CamMarker>& PathShakeHost_Markers();
// The tick the camera path drives the view at: while it plays or scrubs, or a render evaluates it.
// False when the path does not have the view.
bool PathShakeHost_PathTick(double& outTick);
// The tick a looked-through camera object's keys are applied at (CS2's scene keys).
double PathShakeHost_ObjectTick();
// The playing demo, the key its project (and so its shakes) is saved under; empty without a demo.
std::wstring PathShakeHost_DemoPath();
// Which game's project store the shakes go to.
ProjectGame PathShakeHost_Game();
// Seconds per demo tick.
double PathShakeHost_IntervalPerTick();

} // namespace Filmmaker
