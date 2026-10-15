#pragma once

// WHO OWNS THE VIEW THIS FRAME -- one answer, engine-free, shared by everything that asks.
//
// Before this existed the question had four different answers in four places. The view hook's claim
// (DeadlockViewSetup.cpp ApplyOverride) knew about the free camera, the shot and the graph but not
// the follow camera, so a follow pose could never reach the screen. The `camera` report named
// owners in its own order and could name the follow camera while nothing wrote. And the camera
// features shared ONE pending-pose slot, so whichever of the shot and the graph pushed last in a
// frame won -- the graph, because it runs after the shot -- which is the opposite of the documented
// precedence. A pose pushed while nothing claimed the view also stayed pending and jumped in later.
//
// PRECEDENCE, highest first:
//
//   Shot       a camera path that is arming, playing, being scrubbed, or holding its pose while a
//              scrub release settles. An explicit, timed camera move; nothing interrupts it.
//   Graph      the graph editor while it drives the camera (the user is watching a curve edit).
//   Follow     the follow / attached camera while its preview is live.
//   FreeCam    HLAE's free camera.
//   ShotHold   the paused, parked path pose: the editor is open, the replay is paused, the path is
//              evaluable, and the playhead is inside it. BELOW the free camera on purpose, so going
//              to a player or flying off to place a marker is never frozen by a held pose.
//   Game       nothing claims the view; the replay's own camera is used and nothing is written.
//
// Console camera overrides (`mirv_fov`, `mirv_camio import`, `mirv_campath enabled`) are not an owner:
// they are applied on top of whichever owner produced the pose, as they always were.
//
// FRESHNESS. Each feature pushes into its own slot, stamped with the main-pump counter at the time.
// The main pump runs after the view write, so a pose pushed in pump N is consumed by the view of the
// NEXT frame, during which the counter still reads N. A slot is fresh only when its stamp equals the
// current counter; anything older is dropped rather than applied late.

namespace Deadlock {

enum class ViewOwner : int {
	Game = 0,
	ShotHold,
	FreeCam,
	Follow,
	Graph,
	Shot,
};

struct ViewOwnerInputs {
	bool replayAvailable = false; // a valid demo tick; the hideout is never a camera owner
	bool shotActive = false;  // arming, playing, scrubbing, or settling a scrub release
	bool graphDriving = false;
	bool followLive = false;
	bool freeCam = false;
	bool shotHold = false;
};

inline ViewOwner ResolveViewOwner(const ViewOwnerInputs& in) {
	if (!in.replayAvailable) return ViewOwner::Game;
	if (in.shotActive) return ViewOwner::Shot;
	if (in.graphDriving) return ViewOwner::Graph;
	if (in.followLive) return ViewOwner::Follow;
	if (in.freeCam) return ViewOwner::FreeCam;
	if (in.shotHold) return ViewOwner::ShotHold;
	return ViewOwner::Game;
}

// The names the `camera` report prints. Automation/verify/verify-deadlock-cameras.ps1 matches on
// "camera path" and "follow camera", so they are part of the contract.
inline const char* ViewOwnerName(ViewOwner owner) {
	switch (owner) {
	case ViewOwner::Shot: return "camera path";
	case ViewOwner::Graph: return "graph";
	case ViewOwner::Follow: return "follow camera";
	case ViewOwner::FreeCam: return "free camera";
	case ViewOwner::ShotHold: return "camera path (held)";
	case ViewOwner::Game: return "the game";
	}
	return "the game";
}

// True when the owner writes the view. The game camera is recorded, never written.
inline bool ViewOwnerWrites(ViewOwner owner) {
	return owner != ViewOwner::Game;
}

struct OwnedPose {
	bool have = false;
	unsigned long long stamp = 0;
	double x = 0.0, y = 0.0, z = 0.0;
	double pitch = 0.0, yaw = 0.0, roll = 0.0, fov = 90.0;
};

inline bool OwnedPoseFresh(const OwnedPose& pose, unsigned long long mainPumpCount) {
	return pose.have && pose.stamp == mainPumpCount;
}

} // namespace Deadlock
