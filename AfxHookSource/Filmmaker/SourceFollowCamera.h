#pragma once
// CS2's follow camera (AfxHookSource2/Filmmaker/Movie/FollowCamera.h) in CS:S. This file answers its
// host (FollowCameraHost.h) from the entity adapter (SourceEngine.h) and runs it from the hook's
// frame and view. The console surface is CS2's own: mirv_filmmaker follow ... Main thread.

namespace SourceFilmmaker {

// Once a frame at render start, after the TIMELINE dock: the camera's frame while the Camera Editor
// is open. Closing the editor stops Live, as in CS2.
void FollowCamera_Frame();
// From the view hook, after HLAE's own overrides: Lock-on aims at the target where it is drawn this
// frame. True when the follow camera set the view.
bool FollowCamera_OnView(float& x, float& y, float& z, float& pitch, float& yaw, float& roll, float& fov);
// Live: the follow camera has the view, so the path's playback, AUTO and the dock's drive stand aside.
bool FollowCamera_OwnsView();
void FollowCamera_PrintHealth();

} // namespace SourceFilmmaker
