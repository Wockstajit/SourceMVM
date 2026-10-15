#pragma once

// Deadlock's camera-input host.
//
// The CS2 hook keeps this in main.cpp: a `MirvInputEx` implementing IMirvInputDependencies, plus
// the twenty CameraBridge_* functions the Filmmaker layer calls to drive and read the free camera.
// Neither piece is CS2-specific in substance -- Shared/MirvInput.cpp is pure Win32 and maths with
// no engine types at all -- but MirvInputEx's suspend predicate ORs together CS2's own UI-wants-
// cursor flags.
//
// So this is the Deadlock counterpart: same MirvInput underneath, Deadlock's own suspend rules on
// top, and the CameraBridge_* definitions the shared Filmmaker files link against.
//
// The suspend predicate is the one seam that matters. Get it wrong and the symptom is the recorded
// one: sliders work while buttons are dead, because an unsuspended MirvInput CONSUMES the mouse
// event before Panorama ever sees it.

#include "DeadlockViewOwnerMath.h"

class MirvInput;

namespace Deadlock {

// Constructs the MirvInput instance and registers `mirv_input`. Call once from DllMain.
void CameraRuntime_Initialize();

// Uses the main pump's debounced replay state, including when UI is disabled.
// On confirmed unload, release follow/freecam and discard queued camera placements.
// Instantaneous write permission still requires a valid demo tick in ResolveOwner.
void CameraRuntime_RunFrame(bool demoLoaded);

MirvInput* CameraRuntime_Input();

// THE free-camera switch. `mirv_filmmaker freecam on|off|toggle`, `mirv_input camera|end` and
// the CameraBridge setter all land here, so there is one implementation and one log line
// (`deadlock.freecam on|off src=<caller>`, written on a change). Returns false when the camera
// runtime is not up yet.
bool CameraRuntime_SetFreeCam(bool enable, const char* src);
bool CameraRuntime_FreeCam();

// THE SAME PREDICATE DeadlockMirvInputHost::GetSuspendMirvInput() answers for MirvInput itself
// (Ui_ConsoleVisible() || MouseCapture_IsFree() || Ui_AnyPanelWantsCursor()), exposed so
// DeadlockInput.cpp's own key-edge tracking can detect the same transition MirvInput reacts to.
// Single source of truth: change one, the other follows, by construction rather than by two
// authors remembering to keep two copies in step.
bool CameraRuntime_MirvInputSuspended();

// WHO OWNS THE VIEW THIS FRAME (DeadlockViewOwnerMath.h). The view hook's claim, the `camera`
// report and the pose consumer all ask this one function, so they cannot disagree.
ViewOwner CameraRuntime_ResolveOwner();

// The owner as of the LAST time the view hook actually resolved one (DeadlockViewSetup.cpp's
// ApplyOverride, once per render frame) -- not a fresh recomputation. CameraRuntime_OwnerName()
// below reads this rather than calling CameraRuntime_ResolveOwner() again, because a fresh call
// made from a console/netcon command runs on a different cadence than the render hook: Follow's
// (and Shot's/Graph's) pushed-pose freshness is checked against `Frame_MainCount()`, and a
// recomputation from outside the render hook's own per-frame timing can read that counter one tick
// off from when the pose was actually applied -- correct in the applied write, wrong in the report
// asked a moment later. Measured live (P5): `mirv_filmmaker view`/`camera` consistently reported
// "the game" while the follow camera's pose was demonstrably being applied every frame (matching
// origin/fov values, tracking angles) -- a reporting bug, not a functional one, fixed by recording
// rather than re-deriving.
void CameraRuntime_RecordAppliedOwner(ViewOwner owner);
ViewOwner CameraRuntime_LastAppliedOwner();
const char* CameraRuntime_OwnerName();

// Push an absolute pose on behalf of one owner, stamped with the current main-pump count. Only
// the resolved owner's slot is applied, and only while fresh; a pose from any other owner, or
// from an earlier pump, is dropped instead of being applied late.
void CameraRuntime_PushOwnedPose(ViewOwner owner, double x, double y, double z,
                                 double pitch, double yaw, double roll, double fov);
bool CameraRuntime_HasFreshPose(ViewOwner owner);

// Feeds the current on-screen size, used for the inverse FOV scaling MirvInput needs.
void CameraRuntime_SetViewportSize(int width, int height);

// The viewport the engine last reported, in device pixels. The render workspace shows it as the
// capture resolution, because a back-buffer grab has no resolution of its own -- the window's size
// IS the output size, and a resolution picker there would be a control that does nothing.
void CameraRuntime_GetViewportSize(int& width, int& height);

// Records the pose the engine produced this frame, before any override. This is what "reset view"
// snaps back to and what the marker system snapshots.
void CameraRuntime_SetGameCamera(const double origin[3], const double angles[3], double fov);

// Records the pose actually used this frame, after the override.
void CameraRuntime_SetLastCamera(const double origin[3], const double angles[3], double fov);

void CameraRuntime_GetLastCamera(double outOrigin[3], double outAngles[3], double& outFov);

// The CameraBridge's untagged push: a ONE-SHOT placement for the free camera (go to a hero, a
// reposition). Camera features that drive the view every frame -- the shot, the graph, the follow
// camera -- push through CameraRuntime_PushOwnedPose instead, so the owner resolver decides which
// of them is applied and a pose from a frame nobody claimed is dropped rather than applied late.
void CameraRuntime_PushPose(double x, double y, double z,
                            double pitch, double yaw, double roll, double fov);

// The OS-side cursor probe the graph editor drags with. Panorama's in-game HUD context has no
// mouse-move event, so this is the only source of continuous motion; `seq` increments on every
// move so JavaScript can distinguish "still dragging" from "same pixel twice".
void CameraRuntime_GetUiCursor(int& x, int& y, bool& lmb, bool& rmb, bool& shift, unsigned& seq);
void CameraRuntime_SetUiCursorSample(int x, int y, bool lmb, bool rmb);

// In-world camera-path keyframe gizmos. Kept behind an explicit suppression flag as well as the
// user's own toggle, because the gizmos draw into the scene target before the UI pass and would
// otherwise land in every recorded frame; a render job suppresses them for its lifetime without
// disturbing the toggle, so they come back by themselves when the job ends.
bool CameraRuntime_PathDrawEnabled();
void CameraRuntime_SetPathDraw(bool enabled);
void CameraRuntime_SetPathDrawSuppressed(bool suppressed);

// The whole override stack for one frame, applied in place. Returns true when anything changed
// the pose, which is what the view hook uses to decide whether to write back.
bool CameraRuntime_Override(float deltaSeconds,
                            float& x, float& y, float& z,
                            float& pitch, float& yaw, float& roll, float& fov);

void CameraRuntime_OnFocus(bool hasFocus);

} // namespace Deadlock
