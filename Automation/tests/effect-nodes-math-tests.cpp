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

} // namespace

int main() {
	TestStarterPresetsCompileCleanly();
	TestCycleSafety();
	TestDisabledGraphProducesNoSteps();
	if (g_failed) {
		std::cerr << g_failed << " EffectNodesMathTests failed\n";
		return 1;
	}
	return 0;
}
