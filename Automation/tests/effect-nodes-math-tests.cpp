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

// The graph editor asks CheckConnect before it wires two nodes: a mask never becomes the image the
// Output shows, an image never becomes a mask, and no wire closes a loop.
void TestConnectRules() {
	NodeGraph g = BuildPreset("glow_through_fog", true);   // fog -> glow(mask), glow -> grade -> out
	int fog = -1, glow = -1, grade = -1, out = -1;
	for (const Node& n : g.nodes) {
		if (n.type == NodeType::FogMask) fog = n.id;
		if (n.type == NodeType::Glow) glow = n.id;
		if (n.type == NodeType::ColorGrade) grade = n.id;
		if (n.type == NodeType::Output) out = n.id;
	}
	CHECK(fog > 0 && glow > 0 && grade > 0 && out > 0);
	CHECK(CheckConnect(g, grade, out, 0) == ConnectCheck::Ok);
	CHECK(CheckConnect(g, fog, out, 0) == ConnectCheck::MaskIntoImage);
	CHECK(CheckConnect(g, grade, out, 1) == ConnectCheck::NoSuchPort);
	CHECK(CheckConnect(g, out, grade, 0) == ConnectCheck::NoSuchPort);
	CHECK(CheckConnect(g, fog, glow, 0) == ConnectCheck::MaskIntoImage);
	CHECK(CheckConnect(g, grade, glow, 1) == ConnectCheck::ImageIntoMask);
	CHECK(CheckConnect(g, glow, fog, 0) == ConnectCheck::NoSuchPort);
	// glow feeds grade, so grade -> glow would loop.
	CHECK(CheckConnect(g, grade, glow, 0) == ConnectCheck::Cycle);
	CHECK(CheckConnect(g, glow, glow, 0) == ConnectCheck::SameNode);
	CHECK(CheckConnect(g, glow, 999, 0) == ConnectCheck::MissingNode);
	// A wire dropped on empty space: masks land on the mask input, images on the image input.
	CHECK(DefaultLinkPort(NodeType::FogMask, NodeType::Glow) == 1);
	CHECK(DefaultLinkPort(NodeType::Glow, NodeType::ColorGrade) == 0);
	CHECK(DefaultLinkPort(NodeType::SkyMask, NodeType::MaskCombine) == 0);
	CHECK(DefaultLinkPort(NodeType::Glow, NodeType::MaskCombine) == -1);
	CHECK(DefaultLinkPort(NodeType::Glow, NodeType::DepthRange) == -1);
}

// Every node type has a token that round-trips and is exactly one of mask, effect or output, and
// Mask Feather reads the mask on its one input and compiles after it.
void TestEveryTypeIsWired() {
	for (int i = 0; i < (int)NodeType::Count; ++i) {
		const NodeType t = (NodeType)i;
		NodeType back = NodeType::Output;
		CHECK(NodeTypeFromToken(NodeTypeToken(t), back) && back == t);
		const int kinds = (IsMaskNode(t) ? 1 : 0) + (IsEffectNode(t) ? 1 : 0) + (t == NodeType::Output ? 1 : 0);
		CHECK(kinds == 1);
	}
	NodeGraph g;
	g.enabled = true;
	g.nodes.push_back(MakeDefaultNode(1, NodeType::RadialMask, 0, 0));
	g.nodes.push_back(MakeDefaultNode(2, NodeType::MaskFeather, 0, 0));
	g.nodes.push_back(MakeDefaultNode(3, NodeType::Vignette, 0, 0));
	g.nodes.push_back(MakeDefaultNode(4, NodeType::Output, 0, 0));
	g.nextNodeId = 5;
	CHECK(CheckConnect(g, 1, 2, 0) == ConnectCheck::Ok);
	CHECK(CheckConnect(g, 1, 2, 1) == ConnectCheck::NoSuchPort);
	CHECK(CheckConnect(g, 3, 2, 0) == ConnectCheck::ImageIntoMask);
	CHECK(DefaultLinkPort(NodeType::RadialMask, NodeType::MaskFeather) == 0);
	g.edges.push_back(Edge{ 1, 1, 0, 2, 0 });
	g.edges.push_back(Edge{ 2, 2, 0, 3, 1 });
	g.edges.push_back(Edge{ 3, 3, 0, 4, 0 });
	CompiledPlan p = CompileGraph(g);
	CHECK(p.steps.size() == 3);
	if (p.steps.size() == 3) {
		CHECK(p.steps[0].type == NodeType::RadialMask);
		CHECK(p.steps[1].type == NodeType::MaskFeather);
		CHECK(p.steps[1].maskSlotA == p.steps[0].outMaskSlot);
		CHECK(p.steps[1].outMaskSlot >= 0 && p.steps[1].outMaskSlot != p.steps[1].maskSlotA);
		CHECK(p.steps[2].type == NodeType::Vignette);
		CHECK(p.steps[2].maskSlotA == p.steps[1].outMaskSlot);
	}
}

// Every look preset compiles to masks before effects, keeps every mask it reads, and ends in an Output.
// Solid Color Players fills every player (not only the picked one), and only the two time effects keep a
// history.
void TestLookPresetsCompile() {
	const char* looks[] = { "solid_color_players", "neon_silhouettes", "sin_city", "comic_cel_shade", "teal_orange",
		"old_film", "vhs_tape", "comic_book", "shimmering_players", "ghost_trails", "painting", "pencil_drawing",
		"miniature", "anamorphic", "dreamy_glow", "night_vision_goggles", "thermal_scope" };
	for (const char* name : looks) {
		NodeGraph g = BuildPreset(name, true);
		int outputs = 0;
		for (const Node& n : g.nodes) if (n.type == NodeType::Output) ++outputs;
		CHECK(outputs == 1);
		const CompiledPlan p = CompileGraph(g);
		if (p.steps.empty() || p.droppedForMaskSlots != 0 || !SlotsHoldTheirMasks(p)) std::cerr << "look " << name << "\n";
		CHECK(!p.steps.empty());
		CHECK(p.droppedForMaskSlots == 0);
		CHECK(SlotsHoldTheirMasks(p));
	}
	const CompiledPlan solid = CompileGraph(BuildPreset("solid_color_players", true));
	CHECK(solid.steps.size() == 2);
	if (solid.steps.size() == 2) {
		CHECK(solid.steps[0].type == NodeType::PlayerMatte && solid.steps[0].p[0] < 0.5f);
		CHECK(solid.steps[1].type == NodeType::Silhouette);
		CHECK(solid.steps[1].maskSlotA == solid.steps[0].outMaskSlot);
	}
	for (int i = 0; i < (int)NodeType::Count; ++i) {
		const NodeType t = (NodeType)i;
		CHECK(NeedsHistory(t) == (t == NodeType::EchoTrails || t == NodeType::StopMotion));
	}
}

// Mask Edge is a mask processor like Feather: one mask in, a mask out, no image in.
void TestMaskEdgeConnects() {
	NodeGraph g; g.enabled = true;
	const int m = AddNode(g, NodeType::PlayerMatte);
	const int edge = AddNode(g, NodeType::MaskEdge);
	const int fx = AddNode(g, NodeType::Silhouette);
	CHECK(CheckConnect(g, m, edge, 0) == ConnectCheck::Ok);
	CHECK(CheckConnect(g, fx, edge, 0) == ConnectCheck::ImageIntoMask);
	CHECK(CheckConnect(g, m, edge, 1) == ConnectCheck::NoSuchPort);
	CHECK(DefaultLinkPort(NodeType::PlayerMatte, NodeType::MaskEdge) == 0);
	Link(g, m, edge, 0);
	Link(g, edge, fx, 1);
	const int out = AddNode(g, NodeType::Output); Link(g, fx, out, 0);
	const CompiledPlan p = CompileGraph(g);
	CHECK(p.steps.size() == 3);
	CHECK(SlotsHoldTheirMasks(p));
}

int main() {
	TestLookPresetsCompile();
	TestMaskEdgeConnects();
	TestStarterPresetsCompileCleanly();
	TestPlayerPresetsFollowThePick();
	TestCycleSafety();
	TestSharedMaskOutlivesLaterMasks();
	TestDeepCombineTreeKeepsLiveBranches();
	TestTooManyLiveMasksAreDroppedNotOverwritten();
	TestDisabledGraphProducesNoSteps();
	TestConnectRules();
	TestEveryTypeIsWired();
	if (g_failed) {
		std::cerr << g_failed << " EffectNodesMathTests failed\n";
		return 1;
	}
	return 0;
}
