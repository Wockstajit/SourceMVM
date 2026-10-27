#pragma once

// CS2's camera shake layers in CS:S: Handheld, Weapon Recoil and Weapon Bob
// (AfxHookSource2/Filmmaker/Movie/CameraShakeStack.cpp, compiled into both hooks), and this hook's
// answers to their host (CameraShakeHost.h).

namespace SourceFilmmaker {

// Installs the shot listener (the profile's shot temp entity). Safe to call every frame; it retries
// until the client classes are there.
void CameraShake_Init();

// Main thread, once a frame: the stack's housekeeping (a closed demo drops the springs).
void CameraShake_Frame();

// The view hook, after every camera override: the summed shake rides on the view. followOwns: the
// follow camera has the view; cameraOverridden: HLAE's free camera or a camera path has it.
void CameraShake_OnView(float& x, float& y, float& z, float& pitch, float& yaw, float& roll, float& fov,
	bool followOwns, bool cameraOverridden);

// One line for mirv_filmmaker health.
void CameraShake_PrintHealth();

} // namespace SourceFilmmaker
