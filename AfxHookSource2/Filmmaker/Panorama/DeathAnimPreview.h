#pragma once

// PER-PLAYER DEATH ANIMATIONS, 3D preview: plays a death clip on the Config picker's player-model
// preview (a MapPlayerPreviewPanel, so the real agent of the selected player) instead of a stick
// figure.
//
// How: the preview pawn is an ordinary client pawn (class csgo_previewplayer). Its PARENT-LOCAL
// bone transforms live behind CModelState+0x18 (and a copy at +0x48); the engine builds the world
// bones it draws from them AFTER the late main-thread pump (measured 2026-09-25: a late write to the
// world array at +0x80 never reached the screen, the same write to +0x18 moved the rendered body).
// So every late frame this samples the clip, drives the fifteen ragdoll bones, carries every other
// bone along with its nearest driven ancestor (DeathAnimationsMath::FollowDriven), converts back to
// parent-local and writes the pose.
//
// Bone names come from the pawn's CModel (char* table at +0x168, int16 parents at +0x180) -- neither
// is in the schema, so both are validated before use and the preview simply does nothing if a CS2
// update moves them (`deathanim view status` says why).

namespace Filmmaker {

// The clip the picker is previewing, or null/"" / "off" to stop. Restarts the clip from frame 0.
void DeathAnimPreview_Set(const char* clip);

// True while a clip is being previewed. Also arms the paused-demo render gates for the preview panel
// (the same ones the Customize modal uses), so it draws while the demo is paused.
bool DeathAnimPreview_Active();

// Late main-thread pump (after the client's FRAME_RENDER_PASS handler; see Filmmaker.h).
// `configOpen` false stops the preview: it only exists inside the Config panel's picker, and a
// preview left running would keep posing the Customize modal's character later.
void DeathAnimPreview_RunLateFrame(bool configOpen);

// One-line status for `mirv_filmmaker deathanim view status`.
void DeathAnimPreview_PrintStatus();

} // namespace Filmmaker
