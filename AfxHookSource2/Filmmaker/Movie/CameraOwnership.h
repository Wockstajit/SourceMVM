#pragma once

// Who claimed the view pose this frame, and what that means downstream.
//
// WHY THIS EXISTS. The view-setup trampoline runs a stack of override cameras (camera path, cam
// import, free cam, attach, the Rust hook). Several of them can be active at once,
// so "did something override the pose" is not enough -- the shake stack layers on top of POV and
// attach but must leave a hand-flown or hand-authored camera alone, and the true-aim crosshair
// only has a meaning while the view is still the game's own POV.
//
// This used to be six loose booleans in the trampoline with the precedence encoded implicitly in
// the ORDER of an if/else chain, several hundred lines away from where the flags were set. That
// ordering is load-bearing and non-obvious (see Resolve() below), and every new camera feature
// had to rediscover it. Naming it here makes the rule reviewable in one place.

#include "CameraShakeTypes.h" // ShakeCamMode

namespace Filmmaker {

// Set by each override stage in the trampoline as it claims the pose. More than one can be true
// in the same frame -- that is the whole reason Resolve() has to impose an order.
struct CameraOwnership {
	bool path = false;        // baked camera path
	bool import = false;      // cam import (CamIO)
	bool freeCam = false;     // mirv free cam / MirvInput override
	bool attach = false;      // FollowCamera attach preview
	bool other = false;       // the Rust view hook, or anything else

	bool Any() const { return path || import || freeCam || attach || other; }

	// Collapses the flags to the single mode the shake stack gates on.
	//
	// THE ORDER IS LOAD-BEARING, in this exact priority:
	//
	//   1. path / import  -- fully hand-authored. Outranks everything; a baked path must never
	//                        be shaken, even if it was started from an attach preview.
	//   2. attach         -- MUST outrank freeCam. FollowCamera attach preview latches the free
	//                        cam, so `attach` and `freeCam` are BOTH set during an
	//                        attach preview. If freeCam won, the shake would switch itself off
	//                        on exactly the head cam it exists to ride.
	//   3. freeCam        -- hand-flown, never shaken.
	//   4. other          -- unknown owner, treated conservatively.
	//   5. (none)         -- plain POV spectate, including CS2's own chase and free cameras.
	ShakeCamMode Resolve() const {
		if (path || import) return ShakeCamMode::CameraPath;
		if (attach)         return ShakeCamMode::Attach;
		if (freeCam)        return ShakeCamMode::FreeCam;
		if (other)          return ShakeCamMode::Other;
		return ShakeCamMode::Pov;
	}
};

} // namespace Filmmaker
