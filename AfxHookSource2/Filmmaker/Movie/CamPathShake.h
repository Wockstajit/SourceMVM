#pragma once

// CamPathShake: GTA V / Rockstar Editor style per-keyframe camera shake for the camera timeline.
//
// Right-click a keyframe diamond -> pick a style, an intensity (1-10) and a speed (10-100), the
// way a Rockstar Editor camera marker carries its shake. The setting rides on the keyframe; the
// shake itself is evaluated by RageShake (an oscillator/envelope model) and ADDED to the
// camera-path pose on the way out.
//
// ---------------------------------------------------------------------------------------------
// THE LAYER IS NON-DESTRUCTIVE, AND THAT IS ENFORCED STRUCTURALLY, NOT BY CONVENTION.
//
// Nothing here can reach the camera graph:
//   * The settings live in THIS module's own map and THIS module's own project.mvm section
//     ("camshake"), never in CamMarker, never in PathSettings, never in the
//     campath section. The graph editor bakes CamMarker vectors back over the path
//     (CameraPath::ReplaceMarkersFromGraph) -- anything stored on a marker would be a graph
//     channel by definition, and would be rewritten by a curve drag.
//   * Apply() takes the evaluated pose BY VALUE-ADD at the point of push and never writes back.
//     Turn the shake off and the pose is bit-identical to the authored curve.
// The graph stays exactly what it is: authored camera positioning and movement.
//
// Keyframe identity is the keyframe's TICK, not its index. An index would be silently wrong the
// moment a keyframe is inserted ahead of it (CamMarkers::Add inserts in tick order and renumbers)
// or the graph rebakes the list. Retimes are followed by RunFrame, which watches the tick list.
//
// THREADING -- this bit is load-bearing, it crashed the game once.
//
// The store (a std::map keyed against CameraPath's marker std::vector) is MAIN THREAD ONLY. The
// view-setup trampoline that applies the shake is NOT guaranteed to be the main thread, so it
// must never touch either container: on demo load CameraPath::RunFrame wipes and reloads the
// marker vector, and a view thread walking it mid-reallocation takes the process down.
//
// The main thread therefore publishes a flat snapshot once per frame (CamPathShake_RunFrame) and
// the view side reads only that, under a mutex held just long enough to copy. See PublishSnapshot
// in the .cpp for the full note.

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// A STOP key: the shake running into it ends there (it fades over the handover). Saved as "STOP".
constexpr int kCamPathShakeStop = -2;

// One keyframe's shake, in the Rockstar Editor's own units: a style, 1-10, 10-100.
struct CamPathShakeSetting {
	int style = -1;      // index into the RageShake style table; -1 = no key; kCamPathShakeStop = stop
	int intensity = 5;   // 1..10  (5 = the style's authored strength, 10 = double)
	int speed = 50;      // 10..100 (50 = the style's authored frequency, 100 = double)

	bool Active() const { return style >= 0 && intensity > 0; }
	bool Stop() const { return style == kCamPathShakeStop; }
	bool IsKey() const { return Active() || Stop(); }

	// Needed so the retime/prune pass can compare whole maps and only mark the sidecar dirty
	// when something actually moved -- otherwise every frame would rewrite the file.
	bool operator==(const CamPathShakeSetting& o) const {
		return style == o.style && intensity == o.intensity && speed == o.speed;
	}
	bool operator!=(const CamPathShakeSetting& o) const { return !(*this == o); }
};

// Main-thread pump: follows the active demo (load/save its project.mvm section), follows keyframe retimes,
// and drops settings whose keyframe no longer exists. Called from Filmmaker::RunMainThreadFrame.
void CamPathShake_RunFrame();

// --- shake tracks --------------------------------------------------------------------------
//
// One model for two owners (user request 2026-10-17, "share the same thing"): owner 0 is the
// camera path, whose keys sit on its camera keys (they follow retimes, and go when their camera
// key does); owner N is camera object N of the Environment scene (scene-object-rigs.md), whose keys
// sit on any tick and apply while it is looked through. A key runs until the next key of its track
// -- the same style ramps to it, any other hands over, a STOP ends it -- or to the end of the
// shot. Keys are addressed by TICK here; the console also takes the path's marker index.
CamPathShakeSetting CamPathShake_GetAt(int owner, int tick);
// style -1 removes the key; kCamPathShakeStop writes a stop. A path key must sit on a camera key.
bool CamPathShake_SetAt(int owner, int tick, const CamPathShakeSetting& s);
void CamPathShake_ClearTrack(int owner);
int  CamPathShake_TrackKeys(int owner);
// The dock's state: `,"pathKeys":[ticks],"shakes":[{i,t,s,l,n,p,o}],"objShakes":{"<id>":[{t,s,l,n,p,o}]},
// "shakeEssentials":n,"shakeStyles":[[style,label]]` (s = -2 and l = "Stop" for a stop key).
void CamPathShake_AppendDockJson(std::ostringstream& o);

// --- per-keyframe access to the path's track (by MARKER INDEX, the legacy bar and the console) ---
CamPathShakeSetting CamPathShake_Get(int markerIndex);
void CamPathShake_Set(int markerIndex, const CamPathShakeSetting& s);
void CamPathShake_Clear(int markerIndex);
void CamPathShake_ClearAll();
// A practice session begins or ends: drop the shakes in memory without saving them and read the
// playing demo's project again on the next frame (CameraPath::ReloadProject).
void CamPathShake_ReloadProject();
int  CamPathShake_Count();          // how many keyframes currently carry a shake
// Copy one keyframe's shake onto EVERY keyframe. Returns how many were written. The usual way to
// shake a whole shot: dial one keyframe in, then push it across instead of repeating the edit.
int  CamPathShake_ApplyToAll(int fromMarkerIndex);

// --- the layer ---
//
// WHERE THIS RUNS, AND WHY IT MATTERS
//
// The shake is a FRAME SHAKER, not a path edit: it jogs the final camera frame the director
// already computed from the marker path; the path itself is never touched. It
// is called from the view-setup trampoline (main.cpp), after the whole camera-ownership stack has
// produced the authored pose and immediately before the write-back, in the same slot
// CameraShake_Apply occupies.
//
// It deliberately does NOT run inside the camera-path push (CamPlayback). Adding the offset there
// would make the shaken pose the path's OWN published pose, and everything that reads the camera
// back would then see it: placing or repositioning a marker would bake the shake into the
// keyframe permanently, the graph editor's per-channel defaults would drift, and the
// spectated-pawn match in main.cpp keys off an unshaken camera. Shaking the view keeps the
// authored path clean by construction -- turn the shake off and the move is bit-identical.
// Gates itself on CameraPath::CurrentPathTick() -- i.e. "is the dolly driving the view right
// now". Do NOT re-gate this on ShakeCamMode: a filmmaker dolly shot resolves to FreeCam, not
// CameraPath (only the legacy HLAE campath sets that), and gating on it disables the feature
// outright. The full explanation is at the top of CamPathShake_ApplyToView in the .cpp.
// `sceneCamera` is the camera object being looked through (0 for none): then that object's track
// shakes the view at the scene keys' tick, and the path's never does (it used to leak onto it).
bool CamPathShake_ApplyToView(
	float& x, float& y, float& z,
	float& pitch, float& yaw, float& roll, float& fov, int sceneCamera);

// The raw evaluator, kept public for tests/diagnostics. `lookupTick` is a position along the
// camera path in demo ticks. Deterministic: the same tick always produces the same offset, so
// scrubbing, previewing and rendering agree and a re-render is frame-identical.
bool CamPathShake_Apply(double lookupTick,
	double& x, double& y, double& z,
	double& pitch, double& yaw, double& roll, double& fov);

// Master switch (the layer is on by default; this is for A/B-ing a shot).
bool CamPathShake_Enabled();
void CamPathShake_SetEnabled(bool on);

// Main thread. Changes whenever the shake the view applies changes (a key's style, intensity or
// speed, the master switch, a retime); 0 while no shake applies. The scrub frame cache mixes it
// into its picture revision.
uint64_t CamPathShake_Revision();

// Console entry: "mirv_filmmaker pathshake ...". Deliberately NOT "camshake", which is the
// CameraShakeStack (Handheld / Recoil / Weapon Recoil) diagnostics command.
void CamPathShake_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

// Timeline state JSON: the per-keyframe settings array the Panorama right-click menu reads.
// Emits `[{"i":<markerIndex>,"s":<style>,"n":<intensity>,"p":<speed>}, ...]` (no outer key).
std::string CamPathShake_StateJsonArray();

} // namespace Filmmaker
