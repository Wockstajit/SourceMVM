#include "../../AfxHookSource2/Filmmaker/Render/RenderPassPlan.h"

#include <cmath>
#include <iostream>

using namespace Filmmaker;

namespace {

int g_failed = 0;

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

PassConfig P(PassType t, VisibleElements e) { PassConfig p; p.type = t; p.elements = e; return p; }

void TestTokens() {
	for (int t = 0; t < (int)PassType::Count; ++t) {
		PassType back;
		CHECK(PassTypeFromToken(PassTypeToken((PassType)t), back) && back == (PassType)t);
	}
	for (int e = 0; e < (int)VisibleElements::Count; ++e) {
		VisibleElements back;
		CHECK(VisibleElementsFromToken(VisibleElementsToken((VisibleElements)e), back) && back == (VisibleElements)e);
	}
	PassType t;
	CHECK(!PassTypeFromToken("clay", t));
	CHECK(!PassTypeFromToken(nullptr, t));
}

void TestStreamNamesUnique() {
	// Every combination gets its own folder, and Beauty + Everything keeps the historic name.
	CHECK(PassStreamName(P(PassType::Beauty, VisibleElements::Everything)) == "beauty");
	CHECK(PassStreamName(P(PassType::Depth, VisibleElements::OnlyPlayers)) == "depth_players");
	std::vector<std::string> names;
	for (int t = 0; t < (int)PassType::Count; ++t)
		for (int e = 0; e < (int)VisibleElements::Count; ++e) {
			const std::string n = PassStreamName(P((PassType)t, (VisibleElements)e));
			for (const std::string& m : names) CHECK(m != n);
			names.push_back(n);
		}
	CHECK(names.size() == 20);
}

void TestOutputLayout() {
	// Videos share the type's folder and are named for the group; sequences get a folder each.
	CHECK(PassOutputFolder(P(PassType::Beauty, VisibleElements::OnlyWorld), false) == "beauty");
	CHECK(std::string(PassFileStem(VisibleElements::OnlyWorld)) == "world");
	CHECK(std::string(PassFileStem(VisibleElements::Everything)) == "everything");
	CHECK(std::string(PassFileStem(VisibleElements::WorldAndPlayers)) == "world_players");
	CHECK(PassOutputFolder(P(PassType::Depth, VisibleElements::OnlyPlayers), true) == "depth\\players");
	// No two groups of one type may share a file name.
	std::vector<std::string> stems;
	for (int e = 0; e < (int)VisibleElements::Count; ++e) {
		const std::string s = PassFileStem((VisibleElements)e);
		for (const std::string& m : stems) CHECK(m != s);
		stems.push_back(s);
	}
}

void TestRenderGrouping() {
	// Beauty + Depth + Normal of everything: one physical render.
	PassPlan a = BuildPassPlan({P(PassType::Beauty, VisibleElements::Everything),
		P(PassType::Depth, VisibleElements::Everything), P(PassType::Normal, VisibleElements::Everything)});
	CHECK(a.PhysicalRendersPerFrame() == 1);
	CHECK(a.StreamCount() == 3);
	CHECK(!a.needsMainReference);

	// Gun only shares the no-gun render and needs Main kept as its reference.
	PassPlan b = BuildPassPlan({P(PassType::Key, VisibleElements::OnlyWeapon),
		P(PassType::Beauty, VisibleElements::WorldAndPlayers)});
	CHECK(b.PhysicalRendersPerFrame() == 2);
	CHECK(b.renders[(int)RenderKey::NoGun]);
	CHECK(b.needsMainReference);

	// Every combination needs at most three renders; Key exists for two groups only.
	std::vector<PassConfig> all;
	for (int t = 0; t < (int)PassType::Count; ++t)
		for (int e = 0; e < (int)VisibleElements::Count; ++e) all.push_back(P((PassType)t, (VisibleElements)e));
	PassPlan c = BuildPassPlan(all);
	CHECK(c.PhysicalRendersPerFrame() == 3);
	CHECK(c.StreamCount() == 15); // Beauty and Key are exclusive per group
	CHECK(PassAllowed(P(PassType::Key, VisibleElements::OnlyPlayers)));
	CHECK(PassAllowed(P(PassType::Key, VisibleElements::OnlyWeapon)));
	CHECK(!PassAllowed(P(PassType::Key, VisibleElements::Everything)));
	CHECK(!PassAllowed(P(PassType::Key, VisibleElements::WorldAndPlayers)));
	CHECK(!PassAllowed(P(PassType::Key, VisibleElements::OnlyWorld)));
	CHECK(!PassAllowed(P(PassType::Beauty, VisibleElements::OnlyPlayers)));
	CHECK(!PassAllowed(P(PassType::Beauty, VisibleElements::OnlyWeapon)));
	CHECK(PassAllowed(P(PassType::Depth, VisibleElements::OnlyPlayers)));

	// Players only is the no-gun render masked against World only: both render, nothing else.
	PassPlan e = BuildPassPlan({P(PassType::Key, VisibleElements::OnlyPlayers)});
	CHECK(e.renders[(int)RenderKey::NoGun] && e.renders[(int)RenderKey::WorldOnly]);
	CHECK(e.needsWorldReference && !e.needsMainReference);
	CHECK(e.PhysicalRendersPerFrame() == 3);
	CHECK((int)RenderKey::WorldOnly < (int)RenderKey::NoGun); // the reference renders first

	// Main renders even when nothing captures it.
	PassPlan d = BuildPassPlan({P(PassType::Depth, VisibleElements::OnlyWorld)});
	CHECK(d.renders[(int)RenderKey::Main]);
	CHECK(d.PhysicalRendersPerFrame() == 2);
	CHECK(d.ExtraRendersPerFrame() == 1);
}

void TestNormalize() {
	PassPlan p = BuildPassPlan({P(PassType::Beauty, VisibleElements::Everything),
		P(PassType::Beauty, VisibleElements::Everything), P((PassType)9, VisibleElements::Everything)});
	CHECK(p.StreamCount() == 1);
}

void TestMasks() {
	CHECK(PassIsDirectCapture(P(PassType::Beauty, VisibleElements::Everything)));
	CHECK(PassIsDirectCapture(P(PassType::Beauty, VisibleElements::OnlyWorld)));
	CHECK(!PassIsDirectCapture(P(PassType::Key, VisibleElements::OnlyWeapon)));
	CHECK(!PassIsDirectCapture(P(PassType::Key, VisibleElements::OnlyPlayers))); // keyed onto the chosen colour
	CHECK(!PassIsDirectCapture(P(PassType::Depth, VisibleElements::Everything)));
	CHECK(PassStreamName(P(PassType::Key, VisibleElements::OnlyPlayers)) == "key_players");
	CHECK(PassMaskFor(VisibleElements::OnlyWeapon) == PassMask::GunDiff);
	CHECK(PassMaskFor(VisibleElements::OnlyPlayers) == PassMask::ColorKey);
}

void TestProgress() {
	// 3 renders over 1000 logical frames = 3000 physical renders, 1000 frames per output.
	PassPlan p = BuildPassPlan({P(PassType::Beauty, VisibleElements::Everything),
		P(PassType::Key, VisibleElements::OnlyPlayers), P(PassType::Depth, VisibleElements::OnlyWorld),
		P(PassType::Key, VisibleElements::OnlyWeapon)});
	CHECK(p.PhysicalRendersPerFrame() == 3);
	PassProgress g = ComputePassProgress(p, 250, 1000);
	CHECK(g.physicalDone == 750 && g.physicalTotal == 3000);
	CHECK(std::fabs(g.percent - 25.0) < 1e-9);
}

void TestTiming() {
	// 60 fps is 16.666... ms, not 16 ms.
	CHECK(std::fabs(FrameIntervalSeconds(60) - 1.0 / 60.0) < 1e-15);
	// No drift: frame 600000 at 60 fps and 64 ticks/s lands exactly 640000 ticks later.
	CHECK(FrameTickAt(100, 600000, 60, 64.0) == 100.0 + 640000.0);
	CHECK(std::fabs(FrameTickAt(0, 1, 60, 64.0) - 64.0 / 60.0) < 1e-12);
	for (int fps : {24, 30, 60, 120, 144, 240}) {
		const double t = FrameTickAt(0, (int64_t)fps * 3600, fps, 64.0); // one hour
		CHECK(t == 64.0 * 3600.0);
	}
}

void TestDepth() {
	CHECK(DepthGray(8.0, 8.0, 4096.0) == 1.0);
	CHECK(DepthGray(5000.0, 8.0, 4096.0) == 0.0);
	CHECK(std::fabs(DepthGray(2052.0, 8.0, 4096.0) - 0.5) < 1e-9);
	CHECK(DepthGray(10.0, 5.0, 5.0) == 0.0);
}

} // namespace

int main() {
	TestTokens();
	TestStreamNamesUnique();
	TestOutputLayout();
	TestRenderGrouping();
	TestNormalize();
	TestMasks();
	TestProgress();
	TestTiming();
	TestDepth();
	if (g_failed) {
		std::cerr << g_failed << " check(s) failed\n";
		return 1;
	}
	std::cout << "render pass plan tests passed\n";
	return 0;
}
