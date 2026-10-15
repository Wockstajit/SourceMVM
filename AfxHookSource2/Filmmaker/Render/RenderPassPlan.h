#pragma once

// Multipass render plan: WHAT a render job captures, as data. Engine-free and header-only so the
// scheduling rules are unit-tested (Automation/tests/render-pass-plan-tests.cpp).
//
// A pass is a (PassType, VisibleElements) pair. The two axes are independent on purpose: any
// output type can be combined with any visibility group, and adding a new type or group adds one
// enum value and one table row -- the scheduler below never enumerates combinations.
//
// HOW PASSES STAY IN SYNC. Every pass of logical frame N renders inside the SAME engine frame: the
// recorder renders the main view once, then re-runs the engine's client render output for each
// extra render (HLAE's extra-pass loop, RenderServiceHooks.cpp) before the engine advances. The
// simulation therefore advances exactly once per logical frame (host_framerate = fps), and every
// pass reads the identical client state -- player transforms, animation, ragdolls, particles,
// camera. Measured 2026-09-26: an extra pass with no changes is pixel-identical to the main pass of
// the same frame (0.00% of pixels differ), while consecutive frames differ by 0.4-1.6%.
//
// PHYSICAL RENDERS. Passes that need the same scene share one render (a RenderKey):
//   Main       -- the normal view. Everything is captured here, at no extra cost.
//   WorldOnly  -- players, gear and the viewmodel dropped at the scene draw hook. Also rendered,
//                 output-less, as the depth reference of Players Only.
//   NoGun      -- first-person viewmodel hidden. World + Players, Gun Only and Players Only.
// Players Only is NoGun masked to what is CLOSER than WorldOnly at each pixel. It used to be its own
// render with every non-player material repainted chroma green, which failed both ways: the engine
// replays static props from command lists recorded in another render, so props kept their real
// colours in the plate (wires, signs, crates read as "player"), and lists recorded during the green
// render were replayed by the next frame's Main, washing out or blacking out the Beauty plate
// (measured 2026-10-09, de_mirage: 17 of 19 Beauty frames wrong). A depth comparison changes no
// material, so no render can leak into another.
// Gun Only has no render of its own: it is the per-pixel difference between Main and NoGun of
// the same logical frame, which isolates exactly the viewmodel AND everything it causes (muzzle
// flash, first-person particles), with its true colour from Main. That is only valid because both
// renders share one simulation state.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Filmmaker {

enum class PassType : int {
	Beauty = 0,
	Depth,
	Normal,
	Key,     // the isolated subject in its own colour over the background colour (green screen)
	Count
};

enum class VisibleElements : int {
	Everything = 0,
	WorldAndPlayers, // no first-person weapon
	OnlyWorld,
	OnlyPlayers,
	OnlyWeapon,
	Count
};

// The physical render a pass is taken from. Order is the render order within a logical frame.
enum class RenderKey : int {
	Main = 0,
	WorldOnly, // before NoGun: Players Only, taken from NoGun, compares against its depth
	NoGun,
	Count
};

// What the pass compositor does with the render before the recorder copies it.
enum class PassMask : int {
	None = 0,     // the whole render is the pass
	ColorKey,     // players: by depth against WorldOnly in a job; by the chroma key in the live view
	GunDiff       // subject = pixels where Main differs from NoGun (gun pass)
};

struct PassConfig {
	PassType type = PassType::Beauty;
	VisibleElements elements = VisibleElements::Everything;

	bool operator==(const PassConfig& o) const { return type == o.type && elements == o.elements; }
	bool operator!=(const PassConfig& o) const { return !(*this == o); }
};

inline const char* PassTypeToken(PassType t) {
	switch (t) {
	case PassType::Beauty: return "beauty";
	case PassType::Depth: return "depth";
	case PassType::Normal: return "normal";
	case PassType::Key: return "key";
	default: return "?";
	}
}

inline const char* PassTypeLabel(PassType t) {
	switch (t) {
	case PassType::Beauty: return "Beauty";
	case PassType::Depth: return "Depth";
	case PassType::Normal: return "Normal";
	case PassType::Key: return "Key";
	default: return "?";
	}
}

inline const char* VisibleElementsToken(VisibleElements e) {
	switch (e) {
	case VisibleElements::Everything: return "all";
	case VisibleElements::WorldAndPlayers: return "worldplayers";
	case VisibleElements::OnlyWorld: return "world";
	case VisibleElements::OnlyPlayers: return "players";
	case VisibleElements::OnlyWeapon: return "gun";
	default: return "?";
	}
}

inline const char* VisibleElementsLabel(VisibleElements e) {
	switch (e) {
	case VisibleElements::Everything: return "Everything";
	case VisibleElements::WorldAndPlayers: return "World + Players (no gun)";
	case VisibleElements::OnlyWorld: return "World only";
	case VisibleElements::OnlyPlayers: return "Players only";
	case VisibleElements::OnlyWeapon: return "Gun only";
	default: return "?";
	}
}

inline bool PassTypeFromToken(const char* s, PassType& out) {
	if (!s) return false;
	for (int i = 0; i < (int)PassType::Count; ++i)
		if (0 == std::strcmp(s, PassTypeToken((PassType)i))) { out = (PassType)i; return true; }
	return false;
}

inline bool VisibleElementsFromToken(const char* s, VisibleElements& out) {
	if (!s) return false;
	for (int i = 0; i < (int)VisibleElements::Count; ++i)
		if (0 == std::strcmp(s, VisibleElementsToken((VisibleElements)i))) { out = (VisibleElements)i; return true; }
	return false;
}

inline RenderKey RenderKeyFor(VisibleElements e) {
	switch (e) {
	case VisibleElements::WorldAndPlayers:
	case VisibleElements::OnlyWeapon:
	case VisibleElements::OnlyPlayers: return RenderKey::NoGun;
	case VisibleElements::OnlyWorld: return RenderKey::WorldOnly;
	case VisibleElements::Everything:
	default: return RenderKey::Main;
	}
}

inline PassMask PassMaskFor(VisibleElements e) {
	switch (e) {
	case VisibleElements::OnlyPlayers: return PassMask::ColorKey;
	case VisibleElements::OnlyWeapon: return PassMask::GunDiff;
	default: return PassMask::None;
	}
}

// Whether the pass exists. The isolated groups (Players only, Gun only) have a background to key
// out, so their colour plate IS the Key; the groups that fill the frame (Everything, World + players,
// World only) have nothing to key, so theirs is Beauty. Depth and Normal exist for every group.
inline bool PassIsIsolated(VisibleElements e) {
	return e == VisibleElements::OnlyPlayers || e == VisibleElements::OnlyWeapon;
}
inline bool PassAllowed(const PassConfig& p) {
	if (p.type == PassType::Key) return PassIsIsolated(p.elements);
	if (p.type == PassType::Beauty) return !PassIsIsolated(p.elements);
	return true;
}

// True when the recorder can hand the render straight to the encoder. Everything else goes
// through the pass compositor first.
inline bool PassIsDirectCapture(const PassConfig& p) {
	return p.type == PassType::Beauty && PassMaskFor(p.elements) == PassMask::None;
}

// Output folder / recorder stream name. Stable and unique per pass; Beauty + Everything keeps the
// plain `beauty` name every existing take uses.
// What a pass's file is called inside its type folder: <take>\beauty\world.mp4.
inline const char* PassFileStem(VisibleElements e) {
	switch (e) {
	case VisibleElements::Everything: return "everything";
	case VisibleElements::WorldAndPlayers: return "world_players";
	case VisibleElements::OnlyWorld: return "world";
	case VisibleElements::OnlyPlayers: return "players";
	case VisibleElements::OnlyWeapon: return "gun";
	default: return "pass";
	}
}

// The pass's folder under the take. A video is one file, so every variant of a type shares the
// type's folder (beauty\everything.mp4, beauty\world.mp4). An image sequence is thousands of
// frames, so each variant keeps its own folder inside it (beauty\world\frame_000001.png).
inline std::string PassOutputFolder(const PassConfig& p, bool imageSequence) {
	std::string folder = PassTypeToken(p.type);
	if (imageSequence) {
		folder += "\\";
		folder += PassFileStem(p.elements);
	}
	return folder;
}

inline std::string PassStreamName(const PassConfig& p) {
	std::string s = PassTypeToken(p.type);
	if (p.elements != VisibleElements::Everything) {
		s += "_";
		s += VisibleElementsToken(p.elements);
	}
	return s;
}

// The pass list of one job, normalised: duplicates dropped, order kept (the recorder's own order
// is the render order, derived below, so list order is only presentation).
inline std::vector<PassConfig> NormalizePasses(const std::vector<PassConfig>& in) {
	std::vector<PassConfig> out;
	for (const PassConfig& p : in) {
		if ((int)p.type < 0 || p.type >= PassType::Count) continue;
		if ((int)p.elements < 0 || p.elements >= VisibleElements::Count) continue;
		if (!PassAllowed(p)) continue;
		bool dup = false;
		for (const PassConfig& q : out) dup |= q == p;
		if (!dup) out.push_back(p);
	}
	return out;
}

struct PassPlan {
	std::vector<PassConfig> passes;       // normalised
	bool renders[(int)RenderKey::Count] = {}; // which physical renders a logical frame needs
	bool needsMainReference = false;      // Gun Only: keep Main's colour + depth for the NoGun render
	bool needsWorldReference = false;     // Players Only: keep WorldOnly's depth for the NoGun render

	// Physical renders per logical frame. Main always renders (it is the view the user sees and the
	// scene every extra render is re-run from), even when no pass captures it.
	int PhysicalRendersPerFrame() const {
		int n = 0;
		for (int i = 0; i < (int)RenderKey::Count; ++i) n += renders[i] ? 1 : 0;
		return n;
	}
	int ExtraRendersPerFrame() const { return PhysicalRendersPerFrame() - 1; }
	int StreamCount() const { return (int)passes.size(); }
};

inline PassPlan BuildPassPlan(const std::vector<PassConfig>& requested) {
	PassPlan plan;
	plan.passes = NormalizePasses(requested);
	plan.renders[(int)RenderKey::Main] = true;
	for (const PassConfig& p : plan.passes) {
		plan.renders[(int)RenderKeyFor(p.elements)] = true;
		if (PassMaskFor(p.elements) == PassMask::GunDiff) plan.needsMainReference = true;
		if (p.elements == VisibleElements::OnlyPlayers) {
			plan.needsWorldReference = true;
			plan.renders[(int)RenderKey::WorldOnly] = true;
		}
	}
	return plan;
}

// Progress is counted in LOGICAL frames (one per output frame, the same in every stream); the
// physical render count is reported alongside, because a four-pass job is honestly ~4x the work.
struct PassProgress {
	int logicalDone = 0;
	int logicalTotal = 0;
	int64_t physicalDone = 0;
	int64_t physicalTotal = 0;
	double percent = 0.0;
};

inline PassProgress ComputePassProgress(const PassPlan& plan, int logicalDone, int logicalTotal) {
	PassProgress p;
	p.logicalDone = logicalDone < 0 ? 0 : logicalDone;
	p.logicalTotal = logicalTotal < 0 ? 0 : logicalTotal;
	const int per = plan.PhysicalRendersPerFrame();
	p.physicalDone = (int64_t)p.logicalDone * per;
	p.physicalTotal = (int64_t)p.logicalTotal * per;
	p.percent = p.logicalTotal > 0 ? 100.0 * (double)p.logicalDone / (double)p.logicalTotal : 0.0;
	return p;
}

// Exact demo position of output frame `frame`: start + frame * tickRate / fps, computed from the
// integer product so frame 100000 at 60 fps is as exact as frame 1 (no accumulated 1/60 error).
// tickRate is integral for CS2 demos (64); a fractional rate falls back to double arithmetic.
inline double FrameTickAt(int startTick, int64_t frame, int fps, double tickRate) {
	if (fps <= 0) return (double)startTick;
	const int64_t whole = (int64_t)tickRate;
	if ((double)whole == tickRate) {
		const int64_t num = frame * whole;
		const int64_t q = num / fps;
		const int64_t r = num % fps;
		return (double)startTick + (double)q + (double)r / (double)fps;
	}
	return (double)startTick + (double)frame * tickRate / (double)fps;
}

// Output frame interval in seconds (1/fps, not an integer millisecond count).
inline double FrameIntervalSeconds(int fps) {
	return fps > 0 ? 1.0 / (double)fps : 0.0;
}

// Gray written by the Depth pass for a camera depth (distance along the view axis, world units):
// white at `nearUnits`, black at `farUnits` and beyond, linear between. The distance itself comes
// from unprojecting CS2's raw depth (afx_render_pass_ps_5_0.fxc).
inline double DepthGray(double distance, double nearUnits, double farUnits) {
	if (!(farUnits > nearUnits)) return 0.0;
	double g = 1.0 - (distance - nearUnits) / (farUnits - nearUnits);
	if (g < 0.0) g = 0.0;
	if (g > 1.0) g = 1.0;
	return g;
}

} // namespace Filmmaker
