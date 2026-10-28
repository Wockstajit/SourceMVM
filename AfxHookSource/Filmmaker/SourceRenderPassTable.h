#pragma once

// Which of CS2's render passes CS:S draws, and how. A pass is a type (Beauty, Depth, Normal, Key) of a
// visible group (Everything, World + players, World only, Players only, Gun only), as CS2's PASSES
// grid lays them out (RenderPassPlan.h, whose enum order the ints here follow). Beauty · Everything
// is the frame HLAE's screen recorder takes; the passes SourceRenderPasses.h draws are the others CS:S
// can make from the view drawn again with model filtering, fog and HLAE's depth surface. Engine-free: the game host, the Render
// workspace and the browser mockup read the same table.

namespace SourceFilmmaker {

const int kPassTypes = 4;    // Beauty, Depth, Normal, Key
const int kPassGroups = 5;   // Everything, World + players, World only, Players only, Gun only

// A pass SourceRenderPasses.h draws in the frame.
enum class ExtraPass { NoGun, Depth, DepthNoGun, KeyGun, World, DepthWorld, KeyPlayers, Normal, NormalNoGun, NormalWorld, DepthPlayers, DepthGun, NormalPlayers, NormalGun };

inline bool PassIsNormal(ExtraPass kind) {
    return kind == ExtraPass::Normal || kind == ExtraPass::NormalNoGun || kind == ExtraPass::NormalWorld || kind == ExtraPass::NormalPlayers || kind == ExtraPass::NormalGun;
}
inline bool PassNeedsDepth(ExtraPass kind) {
    return kind == ExtraPass::Depth || kind == ExtraPass::DepthNoGun || kind == ExtraPass::DepthWorld || kind == ExtraPass::DepthPlayers || kind == ExtraPass::DepthGun || PassIsNormal(kind);
}

inline int PassDepthMask(ExtraPass kind) {
    if (kind == ExtraPass::DepthGun || kind == ExtraPass::NormalGun) return 1;
    if (kind == ExtraPass::DepthPlayers || kind == ExtraPass::NormalPlayers) return 2;
    return 0;
}

enum class PassWay {
	NotAPass,    // CS2 has no such pass: Key is for the isolated groups, Beauty for the others
	Main,        // Beauty · Everything: the game's frame, HLAE's screen recorder
	Extra,       // the view drawn again in the same frame (extra says which)
	Unavailable  // a pass CS2 renders that CS:S does not (why)
};

struct PassRoute {
	PassWay way = PassWay::NotAPass;
	ExtraPass extra = ExtraPass::NoGun;
	const char* why = nullptr;
};

inline PassRoute RoutePass(int type, int group) {
	PassRoute r;
	const bool isolated = group == 3 || group == 4;
	if (type < 0 || type >= kPassTypes || group < 0 || group >= kPassGroups) return r;
	if ((type == 3 && !isolated) || (type == 0 && isolated)) return r;
	if (type == 0 && group == 0) {
		r.way = PassWay::Main;
		return r;
	}
	r.way = PassWay::Extra;
	if (type == 0 && group == 1) r.extra = ExtraPass::NoGun;
	else if (type == 1 && group == 0) r.extra = ExtraPass::Depth;
	else if (type == 1 && group == 1) r.extra = ExtraPass::DepthNoGun;
	else if (type == 3 && group == 4) r.extra = ExtraPass::KeyGun;
    else if (type == 0 && group == 2) r.extra = ExtraPass::World;
    else if (type == 1 && group == 2) r.extra = ExtraPass::DepthWorld;
    else if (type == 3 && group == 3) r.extra = ExtraPass::KeyPlayers;
    else if (type == 2 && group == 0) r.extra = ExtraPass::Normal;
    else if (type == 2 && group == 1) r.extra = ExtraPass::NormalNoGun;
    else if (type == 2 && group == 2) r.extra = ExtraPass::NormalWorld;
    else if (type == 1 && group == 3) r.extra = ExtraPass::DepthPlayers;
    else if (type == 1 && group == 4) r.extra = ExtraPass::DepthGun;
    else if (type == 2 && group == 3) r.extra = ExtraPass::NormalPlayers;
    else if (type == 2 && group == 4) r.extra = ExtraPass::NormalGun;
	else return PassRoute{};
	return r;
}

} // namespace SourceFilmmaker
