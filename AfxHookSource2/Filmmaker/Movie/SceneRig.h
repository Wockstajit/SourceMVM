#pragma once

// Scene object rigs (Docs/features/scene-object-rigs.md): placed lights, meshes and cameras
// attached to players, weapons, grenades and other scene objects, and their pose keys on the
// TIMELINE dock. The engine-free maths are SceneRigMath.h; the keys and attachments live in the
// scene-key track (SceneKeys.cpp), so they share its undo history and the demo's project.mvm.
//
// Threading: main thread only (FrameStageNotify, the view-setup hook and console commands). The
// engine-thread frame capture reads what this publishes through SceneRigTransient.h.

#include <sstream>
#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// After SceneKeys_RunFrame: sample every object's pose channels at the applied tick, resolve
// its parents against the live entities and publish where each rigged object is.
void SceneRig_RunMainThreadFrame();

// View setup (main.cpp, after the Follow Camera): re-sample the entity parents found this frame,
// so attached objects ride the interpolated pose the models are drawn at, and republish. When a
// camera object is looked through, writes its view and returns true (`attached` says whether it
// rides something, for the shake stack).
bool SceneRig_ViewSetup(double& x, double& y, double& z, double& pitch, double& yaw, double& roll,
	double& fov, bool& attached);

void SceneRig_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
void SceneRig_AppendStateJson(std::ostringstream& out);

// ---- for the Environment's own edits (may run under its lock: these never call back into it) --

// Whether the rig owns this object's pose: it is attached, or its pose is keyed.
bool SceneRig_Rigged(int id);
// One local pose channel (Rig::PoseChannel) of a rigged object: keys the pose at the playhead
// when it is animated, else sets its attach offset. False when the rig does not own the object.
bool SceneRig_SetLocal(int id, int channel, double value);
// The gizmo or a placement moved a rigged object to this world position (and these angles when
// `angles` is given); stored the same way as SetLocal.
bool SceneRig_SetWorld(int id, const double position[3], const double* angles);
// A gizmo drag on a rigged object is one undo step: begin, any number of SetWorld, end.
void SceneRig_BeginGesture();
void SceneRig_EndGesture(bool keep);

// ---- for the TIMELINE dock (SceneKeys.cpp) ---------------------------------------------------

// The value a pose channel has with no keys: the attach offset, or the scene's authored pose.
bool SceneRig_RestChannel(int id, int channel, double& out);
// The object's name for its dock layer ("Spot light 3"); "Object 7 (removed)" once it is gone.
std::string SceneRig_ObjectName(int id);
// Whether the object is in the scene now. A removed object's layer leaves the dock at once; its
// keys stay in the track, so undoing the removal brings the layer back with them.
bool SceneRig_ObjectExists(int id);
// The scene's cameras and lights: each has its TIMELINE layer, keyed or not, so it can be animated
// from there (its switch, or a key placed on a row).
std::vector<int> SceneRig_TimelineObjects();
// The channels the object's layer shows, in row order (Rig::ChannelsFor its kind).
std::vector<int> SceneRig_DockChannels(int id);
// The demo changed or a practice session began: forget held poses and the looked-through camera.
void SceneRig_Reset();

// The camera object being looked through, 0 for none (its shake track shakes the view).
int SceneRig_LookThrough();

} // namespace Filmmaker
