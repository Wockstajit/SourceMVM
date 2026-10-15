#pragma once

// Narrow bridge between the movie director (MovieMode) / HUD (MovieHud) and the
// camera-input system, which lives in main.cpp (g_MirvInputEx / MirvInput).
//
// These functions are DEFINED in main.cpp (where g_MirvInputEx is visible) and
// declared here so the Filmmaker modules can drive/read the free camera without
// seeing the MirvInputEx internals. This keeps camera ownership in the camera
// system and lets the UI/director layers stay decoupled (architecture rule:
// Panorama displays state + sends commands; the camera system owns movement).

namespace Filmmaker {

// Free-cam (MirvInput camera control mode) on/off.
bool  CameraBridge_GetFreeCamEnabled();
void  CameraBridge_SetFreeCamEnabled(bool enable);

// Third-person orbit input. Captures the mouse through MirvInput without enabling the HLAE
// free camera. Deltas are raw cursor PIXELS, single-sourced from the once-per-frame
// cursor-vs-center measurement in main.cpp (see MirvInput::SupplyThirdPersonOrbitPixels);
// the consumer (ThirdPersonCamera) applies its own degrees-per-pixel sensitivity.
bool  CameraBridge_GetThirdPersonOrbitEnabled();
void  CameraBridge_SetThirdPersonOrbitEnabled(bool enable);
bool  CameraBridge_ConsumeThirdPersonOrbit(double& dYawPixels, double& dPitchPixels);

// Free-cam speed multiplier; dir > 0 increases, dir < 0 decreases (one 0.5x step,
// FreeCamStepMath.h).
float CameraBridge_GetFreeCamSpeed();
void  CameraBridge_AdjustFreeCamSpeed(int dir);

// Free-cam FOV (zoom), one degree per call. dir > 0 = scroll up = zoom IN (lower FOV);
// dir < 0 = scroll down = zoom OUT (higher FOV). The new FOV persists frame-over-frame.
void  CameraBridge_AdjustFreeCamFov(int dir);

// The last speed/FOV wheel step: seq counts every step, kind 1 = speed, 2 = FOV, dir its sign.
// Drives the speed/FOV toast (MovieHud) while Config or the Camera Editor is open.
void  CameraBridge_LastFreeCamAdjust(int& seq, int& kind, int& dir);

// Temporarily scale free-cam keyboard+mouse sensitivity for fine/slow movement
// (held while Shift is down). Saves/restores the base sensitivity internally and
// is idempotent, so repeated true/true or false/false calls are safe.
void  CameraBridge_SetFreeCamSlow(bool slow);

// Current (post-override) camera FOV, for the HUD readout.
float CameraBridge_GetGameCameraFov();

// Read the current (post-override) camera pose. Used by the camera-marker system
// to snapshot the free cam when placing / repositioning a marker. Angles are
// pitch, yaw, roll (degrees) to match the engine view-angle order.
void CameraBridge_GetCurrentCamera(double outOrigin[3], double outAngles[3], double& outFov);

// Push an ABSOLUTE camera pose for camera-path playback. Applied on the next
// camera override (MirvInput::Override) and consumed once, so it must be called
// every frame while a path is playing. Works whether or not free cam is on.
void CameraBridge_SetCameraPose(double x, double y, double z,
                                double pitch, double yaw, double roll, double fov);

// Enable/disable the in-world camera-path keyframe gizmos (g_CampathDrawer).
void CameraBridge_SetPathDrawEnabled(bool enable);

// Suppress the in-world gizmos for the lifetime of a capture WITHOUT touching the user's own
// CameraBridge_SetPathDrawEnabled choice, so they return by themselves when the job ends. The
// gizmos are drawn into the scene target before UI, so they would otherwise appear in every
// recorded stream. Set true when a render job starts, false on every exit path (success,
// cancel, failure) -- the restore is idempotent.
void CameraBridge_SetPathDrawSuppressed(bool suppressed);

// Draw/hide the single follow-camera marker. This marker is not part of the
// path-camera keyframe model.
void CameraBridge_SetFollowCameraMarker(bool enabled,
                                        double x, double y, double z,
                                        double pitch, double yaw, double roll, double fov);

// Draw/hide the in-world box around the entity the Follow inspector has selected. The box sits
// on (x,y,z), rises "height" and is "halfWidth" wide either way.
void CameraBridge_SetFollowTargetBox(bool enabled,
                                     float x, float y, float z,
                                     float halfWidth, float height);

// UI cursor probe for the EXPERIMENTAL graph editor. Returns the latest client-area cursor
// position + left/right-button state captured in the WndProc (main.cpp), plus the live Shift
// state (read on the main thread). 'seq' increments on every mouse-move so the JS can detect
// motion even when the coordinates repeat. This is what lets the graph editor implement
// After-Effects-style continuous dragging (Panorama itself has no mouse-move event). 'rmb' drives
// the right-click ease menu; 'shift' drives axis-lock dragging. Coordinates are window-client
// pixels (divide by the panel uiscale to map into Panorama layout space).
void CameraBridge_GetUiCursor(int& x, int& y, bool& lmb, bool& rmb, bool& shift, unsigned& seq);

// Live Ctrl state, read the same way as GetUiCursor's Shift (GetAsyncKeyState, main thread). A
// separate call rather than a new GetUiCursor out-param: that signature has six call sites across
// five files, and only the graph editor's timeline zoom (Ctrl+wheel) needs Ctrl.
bool CameraBridge_GetCtrlDown();

// Live Alt state. Alt is the graph editor's snap-bypass modifier: held during a scrub or a key
// drag it places the value at fine (unsnapped) ticks. Shift is already axis-lock/straighten in
// that editor and Ctrl is the wheel zoom, so Alt is the free modifier.
bool CameraBridge_GetAltDown();

// Style the in-world keyframe gizmos for the camera-marker system: paint them in
// the global Freeze (blue) / Live (gold) colour with a glow, and draw the aimed-at
// marker (highlight index, -1 = none) white. When enabled, also forces the
// keyframe-camera gizmos + index labels on. Pushed every frame while editing.
void CameraBridge_SetMarkerStyle(bool enabled, bool freeze, int highlightIndex);

} // namespace Filmmaker
