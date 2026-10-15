#pragma once

// The shot: markers, the path, and the one playhead over them.
//
// The camera timeline and the graph editor are two VIEWS OF THE SAME SHOT, so "where is the
// playhead" has exactly one correct answer and one owner. The CS2 side learned this the hard way:
// the timeline and the graph each kept their own scrub state, both falling back to the demo tick
// when idle, which made them look synchronised right up until either one was dragged. Measured
// live at the time: after dragging the graph ruler to tick 5000 the graph reported 5000 while the
// timeline still reported 17801, because neither drag seeks the world (a seek per dragged tick is
// far too expensive) and the panel that did not drag kept reading the stale demo tick.
//
// The contract, carried over intact:
//
//   * Not scrubbing -> the playhead IS the demo tick. Both views agree for free.
//   * Scrubbing     -> the playhead is the dragged tick. The world has not moved; the visible
//                      view previews the camera there.
//   * Release       -> ends the scrub AND seeks the demo to that tick, so the idle fallback is
//                      immediately correct again. A release that does not seek recreates the bug.
//
// Marker data and path evaluation are the shared, engine-free units (CamMarkers, CamPathEval,
// Shared/CamPath). The clock is Deadlock's own, because the CS2 playback clock reaches the engine
// through MirvTime and its hardcoded 1/64 tick interval, which is wrong here.

#include <string>

namespace advancedfx { class ICommandArgs; }
class IMirvCampath_Time;
class IMirvCampath_Camera;

namespace Filmmaker {

struct CamMarker;

// --- markers ---------------------------------------------------------------------------------

// Snapshots the live camera into a new marker at the current tick.
int Shot_AddMarkerHere();

// Re-points the SELECTED marker at the live camera, keeping its tick. Framing and timing are
// separate edits; moving a camera must not silently re-time the shot.
bool Shot_UpdateSelectedFromCamera();

bool Shot_DeleteSelected();
void Shot_Clear();
int Shot_MarkerCount();
int Shot_Selected();
void Shot_Select(int index);

// Marker `index`, or nullptr when out of range.
const CamMarker* Shot_Marker(int index);

int Shot_FirstTick();
int Shot_LastTick();

// --- playhead --------------------------------------------------------------------------------

void Shot_BeginScrub(double tick);

// Ends the scrub and seeks the demo to the final tick. Both halves, always: see the header note.
void Shot_EndScrub();

bool Shot_Scrubbing();

// The dragged tick while scrubbing, otherwise the live demo tick.
double Shot_PlayheadTick();

// --- playback --------------------------------------------------------------------------------

// Starts driving the camera along the path from the current playhead. Space with the timeline
// open plays the path; Space with it closed is a plain demo pause, which is why this is a distinct
// verb rather than a demo_resume.
void Shot_Play();
void Shot_Pause();
bool Shot_Playing();

// Runs before command dispatch, even with UI disabled. True on project transition;
// callers must release camera ownership then. Uses the shared debounced unload signal.
bool Shot_UpdateReplay(bool demoLoaded);

// Called every main-thread frame. Pushes a pose when the path owns the view.
void Shot_RunFrame();

// True while the path is claiming the camera, so nothing else writes over it.
bool Shot_OwnsView();

// The authored tick currently being evaluated while the shot owns the view. Unlike the demo tick,
// this follows constant-speed and per-segment timing, so a per-keyframe effect stays attached to
// the camera move rather than drifting against it.
bool Shot_CurrentPathTick(double& outTick);

// --- persistence -----------------------------------------------------------------------------

// The "campath" section of source_mvm/demos/deadlock/<demo>/project.mvm.
bool Shot_Save();
bool Shot_Load();

void Shot_Report();

// HLAE's command parser edits the same markers as the editor, never its scratch spline.
void Shot_CampathCommand(advancedfx::ICommandArgs* args, IMirvCampath_Time* time, IMirvCampath_Camera* camera);
bool Shot_CommandPathActive();
bool Shot_ApplyCommandPath(float& x, float& y, float& z, float& pitch, float& yaw, float& roll, float& fov);

// --- the selected marker's own values ---------------------------------------------------------
//
// FOV, roll and ease belong to ONE marker and are keyed with it, so these write the selection
// rather than a path-wide setting. Each returns false when nothing is selected, so a control that
// fires with an empty shot says so instead of silently writing marker zero.
bool Shot_SetSelectedFov(double fov);
bool Shot_SetSelectedRoll(double roll);
bool Shot_SetMarkerEase(int index, int ease); // edits a context-menu key without changing selection
bool Shot_SetSelectedEase(int ease);          // CamMarkers' Ease enum: 0 none, 1 in, 2 out, 3 in/out

// The selected marker's OUTGOING segment speed (Per-Segment mode only; the last marker has no
// outgoing segment, so its value is never read). Clamped to CS2's [0.2, 1.0] range.
bool Shot_SetSelectedSpeedMul(double mul);

// Steps the selection, wrapping. The PATH card's stepper and the timeline both call this rather
// than each working out the next index and disagreeing at the ends. Goes through
// Shot_SelectForEditor, so stepping also takes you to the camera.
int Shot_SelectStep(int delta);

// CS2's CameraPath::SelectForEditor: select the marker AND go to it -- free cam on, demo paused,
// seek to the marker's tick, push its pose. Selecting a camera you cannot see is what made the
// editor's stepper read as doing nothing. Returns false with no such marker.
bool Shot_SelectForEditor(int index);

// CS2's "Tick -> Here" (CameraPath::MoveKey): retime the SELECTED marker to the demo's current
// tick, clamped strictly between its neighbours (DeadlockShotRulesMath.h ClampRetimeTick). The
// pose is untouched -- `marker update` is the pose overwrite. False with nothing selected.
bool Shot_RetimeSelectedToCurrentTick();

// The marker under the crosshair this frame (-1 for none), decided by the in-world drawer's feed
// (DeadlockWorkspaces.cpp) from the same projection it draws with. F selects it, L deletes it --
// CS2's own aimed-marker hotkeys (MovieMode.cpp OnKey).
void Shot_SetHoveredMarker(int index);
int Shot_HoveredMarker();

// The path sampled along its curve for the in-world drawer: up to maxPoints (x,y,z) triples into
// outXyz (3 * maxPoints doubles). Returns the number of points written; 0 with fewer than two
// markers, since one marker is a point, not a path.
int Shot_SamplePath(double* outXyz, int maxPoints);

// --- path-wide settings -----------------------------------------------------------------------

void Shot_SetSpeedMode(int mode);             // PathSettings: 0 manual, 1 constant, 2 per-segment
int Shot_SpeedMode();

// Global Constant-mode speed multiplier, same [0.2, 1.0] range and 0.2/0.5/0.8/1.0 ladder as CS2
// (CameraPath::SetConstSpeed/CycleConstSpeed). Used for the whole path when speedMode == constant.
void Shot_SetConstSpeed(double mul);
void Shot_CycleConstSpeed();
double Shot_ConstSpeed();

// World drawing and the tick hold. These are VIEW options rather than shot data -- they change
// nothing that is saved with the path -- so they live beside it rather than in PathSettings.
void Shot_SetDrawPath(bool on);
bool Shot_DrawPath();
void Shot_SetDrawMarkers(bool on);
bool Shot_DrawMarkers();
void Shot_SetFreeze(bool on);
bool Shot_Freeze();

// The `cam` block of the state feed: the selected marker's values plus the path-wide settings.
void Shot_AppendCameraStateJson(std::string& out);


} // namespace Filmmaker
