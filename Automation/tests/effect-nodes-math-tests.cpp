#include "../../AfxHookSource2/Filmmaker/EffectNodes/EffectNodesMath.h"

#include <iostream>

using namespace Filmmaker::EffectNodes;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

void TestStarterPresetsCompileCleanly() {
	// Color subject, desaturate world -> must require world depth ref for PlayerMatte and emit masks before effects.
	NodeGraph g1 = BuildPreset("color_subject_desat_world", true);
	CompiledPlan p1 = CompileGraph(g1);
	CHECK(p1.enabled);
	CHECK(p1.needsWorldDepthRef);
	CHECK(p1.steps.size() == 4);
	// Mask nodes must compile before the effect nodes that consume them.
	bool seenEffect = false;
	for (const auto& st : p1.steps) {
		if (IsEffectNode(st.type)) {
			CHECK(st.maskSlotA >= 0);
			seenEffect = true;
		}
	}
	CHECK(seenEffect);

	// Glow through the fog -> uses FogMask (no world depth ref needed) + Glow + ColorGrade.
	NodeGraph g2 = BuildPreset("glow_through_fog", true);
	CompiledPlan p2 = CompileGraph(g2);
	CHECK(p2.enabled);
	CHECK(!p2.needsWorldDepthRef);
	CHECK(p2.steps.size() == 3);
	CHECK(p2.steps[0].type == NodeType::FogMask);
	CHECK(p2.steps[1].type == NodeType::Glow);
	CHECK(p2.steps[1].maskSlotA == p2.steps[0].outMaskSlot);
}

// The two player presets act on the picked player, and every effect of theirs is held by that mask: an
// unmasked Glow bloomed the whole frame and whited out a bright scene.
void TestPlayerPresetsFollowThePick() {
	for (const char* name : { "color_subject_desat_world", "player_neon_outline" }) {
		const CompiledPlan p = CompileGraph(BuildPreset(name, true));
		CHECK(p.needsWorldDepthRef);
		int mattes = 0, effects = 0;
		for (const auto& st : p.steps) {
			if (st.type == NodeType::PlayerMatte) { ++mattes; CHECK(st.p[0] > 0.5f); }
			if (IsEffectNode(st.type)) { ++effects; CHECK(st.maskSlotA >= 0); }
		}
		CHECK(mattes >= 1);
		CHECK(effects == 2);
	}
	// The subject and the world are the same mask, one of them inverted.
	const CompiledPlan p = CompileGraph(BuildPreset("color_subject_desat_world", true));
	int inverted = 0;
	for (const auto& st : p.steps) if (st.type == NodeType::PlayerMatte && st.p[2] > 0.5f) ++inverted;
	CHECK(inverted == 1);
}

void TestCycleSafety() {
	NodeGraph g = BuildPreset("depth_spotlight", true);
	// Introduce an artificial cycle between two nodes: compiler must terminate cleanly without hanging.
	g.edges.push_back(Edge{ 99, g.nodes[1].id, 0, g.nodes[0].id, 0 });
	CompiledPlan p = CompileGraph(g);
	CHECK(p.enabled);
	CHECK(p.steps.size() <= kMaxCompiledSteps);
}

void TestDisabledGraphProducesNoSteps() {
	NodeGraph g = BuildPreset("player_neon_outline", false);
	CompiledPlan p = CompileGraph(g);
	CHECK(!p.enabled);
	CHECK(p.steps.empty());
}


// Runs the plan on paper: every slot a step reads must still hold the mask node it was compiled to read.
bool SlotsHoldTheirMasks(const CompiledPlan& p) {
	int holds[kMaskSlots] = { -1, -1, -1, -1 };
	for (const auto& st : p.steps) {
		if (st.maskNodeA > 0 && (st.maskSlotA < 0 || holds[st.maskSlotA] != st.maskNodeA)) return false;
		if (st.maskNodeB > 0 && (st.maskSlotB < 0 || holds[st.maskSlotB] != st.maskNodeB)) return false;
		if (IsMaskNode(st.type)) { if (st.outMaskSlot < 0 || st.outMaskSlot >= kMaskSlots) return false; holds[st.outMaskSlot] = st.nodeId; }
	}
	return true;
}

int AddNode(NodeGraph& g, NodeType t) { g.nodes.push_back(MakeDefaultNode(g.nextNodeId++, t, 0, 0)); return g.nodes.back().id; }
void Link(NodeGraph& g, int from, int to, int port) { g.edges.push_back(Edge{ g.nextEdgeId++, from, 0, to, port }); }

void TestSharedMaskOutlivesLaterMasks() {
	// One sky mask feeds the first and the last of six effects; four other masks are made in between.
	// Round-robin slots overwrote the sky mask before the last effect read it.
	NodeGraph g; g.enabled = true;
	const int sky = AddNode(g, NodeType::SkyMask);
	int prev = -1;
	for (int i = 0; i < 6; ++i) {
		const int fx = AddNode(g, NodeType::ColorGrade);
		if (prev > 0) Link(g, prev, fx, 0);
		const int mask = (i == 0 || i == 5) ? sky : AddNode(g, NodeType::DepthRange);
		Link(g, mask, fx, 1);
		prev = fx;
	}
	const int out = AddNode(g, NodeType::Output);
	Link(g, prev, out, 0);
	CompiledPlan p = CompileGraph(g);
	CHECK(p.droppedForMaskSlots == 0);
	CHECK(p.steps.size() == 11);
	CHECK(SlotsHoldTheirMasks(p));
}

void TestDeepCombineTreeKeepsLiveBranches() {
	// combine(combine(a,b), combine(combine(combine(c,d),e),f)): the first branch's result waits while
	// the second builds. Eleven masks wrap round-robin slots onto it; at most three are live at once.
	NodeGraph g; g.enabled = true;
	auto leaf = [&] { return AddNode(g, NodeType::DepthRange); };
	auto combine = [&](int a, int b) { const int c = AddNode(g, NodeType::MaskCombine); Link(g, a, c, 0); Link(g, b, c, 1); return c; };
	const int left = combine(leaf(), leaf());
	const int right = combine(combine(combine(leaf(), leaf()), leaf()), leaf());
	const int root = combine(left, right);
	const int fx = AddNode(g, NodeType::Glow); Link(g, root, fx, 1);
	const int out = AddNode(g, NodeType::Output); Link(g, fx, out, 0);
	CompiledPlan p = CompileGraph(g);
	CHECK(p.droppedForMaskSlots == 0);
	CHECK(p.steps.size() == 12);
	CHECK(SlotsHoldTheirMasks(p));
}

void TestTooManyLiveMasksAreDroppedNotOverwritten() {
	// Five masks all read by one five-input chain at the end: the fifth cannot get a slot.
	NodeGraph g; g.enabled = true;
	int masks[5];
	for (int& m : masks) m = AddNode(g, NodeType::DepthRange);
	int acc = masks[4];
	for (int i = 3; i >= 0; --i) { const int c = AddNode(g, NodeType::MaskCombine); Link(g, masks[i], c, 0); Link(g, acc, c, 1); acc = c; }
	// combine(m0, combine(m1, combine(m2, combine(m3, m4)))) visits m0..m3 first: all four stay live
	// until m4 is made, so m4 (and the combines that need it) are dropped.
	const int fx = AddNode(g, NodeType::Glow); Link(g, acc, fx, 1);
	const int out = AddNode(g, NodeType::Output); Link(g, fx, out, 0);
	CompiledPlan p = CompileGraph(g);
	CHECK(p.droppedForMaskSlots > 0);
	CHECK(SlotsHoldTheirMasks(p));
}
} // namespace

int main() {
	TestStarterPresetsCompileCleanly();
	TestPlayerPresetsFollowThePick();
	TestCycleSafety();
	TestSharedMaskOutlivesLaterMasks();
	TestDeepCombineTreeKeepsLiveBranches();
	TestTooManyLiveMasksAreDroppedNotOverwritten();
	TestDisabledGraphProducesNoSteps();
	if (g_failed) {
		std::cerr << g_failed << " EffectNodesMathTests failed\n";
		return 1;
	}
	return 0;
}
