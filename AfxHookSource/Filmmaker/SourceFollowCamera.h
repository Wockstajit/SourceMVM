#pragma once
// CS2's follow camera (AfxHookSource2/Filmmaker/Movie/FollowCamera.h) in CS:S. This file answers its
// host (FollowCameraHost.h) from the entity adapter (SourceEngine.h) and runs it from the hook's
// frame and view. The console surface is CS2's own: mirv_filmmaker follow ... Main thread.

#include <string>
#include <vector>

namespace SourceFilmmaker {

namespace Ui { struct FollowInspectorModel; struct PathOverlayModel; }

// The FOLLOW CAMERA tab's model: the follow camera's state JSON, the payload CS2's inspector reads.
void FollowCamera_InspectorModel(Ui::FollowInspectorModel& out);
// Runs the tab's commands (mirv_filmmaker follow ...) through the console, as CS2's inspector does,
// and logs each to mvm_debug ("follow.ui").
void FollowCamera_RunCommands(const std::vector<std::string>& commands);

// Once a frame at render start, after the TIMELINE dock: the camera's frame while the Camera Editor
// is open. Closing the editor stops Live, as in CS2.
void FollowCamera_Frame();
// From the view hook, after HLAE's own overrides: Lock-on aims at the target where it is drawn this
// frame. True when the follow camera set the view.
bool FollowCamera_OnView(float& x, float& y, float& z, float& pitch, float& yaw, float& roll, float& fov);
// Live: the follow camera has the view, so the path's playback, AUTO and the dock's drive stand aside.
bool FollowCamera_OwnsView();
// Adds the in-world aids to the path overlay for a width x height screen: the placed Lock-on camera
// while it is not live, and the box around a target for five seconds after it is picked. False when
// neither shows.
bool FollowCamera_Overlay(Ui::PathOverlayModel& out, float width, float height);
// Reposition (CS2's): the free cam is flown to the new place; a left click while the game has the
// mouse saves it (Lock-on's camera, or Attached's pose on its mount), Esc cancels. OnKey is true when
// the key was the reposition's.
bool FollowCamera_Repositioning();
void FollowCamera_PlaceReposition();
bool FollowCamera_OnKey(int vk, bool down);
void FollowCamera_PrintHealth();

} // namespace SourceFilmmaker
