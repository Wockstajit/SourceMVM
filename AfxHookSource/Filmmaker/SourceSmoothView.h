#pragma once

// Smooth View in CS:S: CS2's breathing, strafe tilt and falling and jumping motion on the game's own
// camera. The implementation is CS2's (AfxHookSource2/Filmmaker/Movie/SmoothView.cpp, shared through
// SmoothViewHost.h); this file answers its host questions for Source 1 and runs it from HLAE's view
// hook after HLAE's overrides, only when none of them moved the camera (no mirv_input free cam, no
// campath), as CS2 keeps Smooth View off its own cameras.
//
// Main thread.

namespace advancedfx { class ICommandArgs; }

namespace SourceFilmmaker {

// After HLAE's view overrides (SourceFilmmaker.h OnViewOverride). x y z is the camera, pitch yaw
// roll fov the view to adjust; cameraOverridden when an HLAE override moved the camera.
void SmoothView_OnView(float x, float y, float z, float& pitch, float& yaw, float& roll, float& fov, bool cameraOverridden);

// mirv_filmmaker smoothview on|off|toggle|state|reset|<setting> <value> (CS2's subcommands).
void SmoothView_Command(int argc, advancedfx::ICommandArgs* args);

} // namespace SourceFilmmaker
