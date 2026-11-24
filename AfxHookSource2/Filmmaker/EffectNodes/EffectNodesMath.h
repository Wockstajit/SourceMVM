#pragma once

// Pure engine-free DAG graph model, presets, and topological compiler for Effect Nodes.
// Connects depth ranges, sky masks, fog masks, and player mattes (all players or picked player)
// into color grades, glows, outlines, and distortions.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Filmmaker::EffectNodes {

inline constexpr int kMaxCompiledSteps = 16;
inline constexpr int kMaskSlots = 4;

enum class NodeType : int {
	// Mask producers
	DepthRange = 0,
	SkyMask = 1,
	PlayerMatte = 2,
	FogMask = 3,
	LumaKey = 4,
	MaskCombine = 5,
	// Effect processors
	ColorGrade = 6,
	Glow = 7,
	Outline = 8,
	Distortion = 9,
	// Final output
	Output = 10,
	Count
};

inline bool IsMaskNode(NodeType t) {
	return (int)t >= (int)NodeType::DepthRange && (int)t <= (int)NodeType::MaskCombine;
}

inline bool IsEffectNode(NodeType t) {
	return (int)t >= (int)NodeType::ColorGrade && (int)t <= (int)NodeType::Distortion;
}

inline const char* NodeTypeToken(NodeType t) {
	switch (t) {
	case NodeType::DepthRange: return "depth_range";
	case NodeType::SkyMask: return "sky_mask";
	case NodeType::PlayerMatte: return "player_matte";
	case NodeType::FogMask: return "fog_mask";
	case NodeType::LumaKey: return "luma_key";
	case NodeType::MaskCombine: return "mask_combine";
	case NodeType::ColorGrade: return "grade";
	case NodeType::Glow: return "glow";
	case NodeType::Outline: return "outline";
	case NodeType::Distortion: return "distortion";
	case NodeType::Output: return "output";
	default: return "grade";
	}
}

inline const char* NodeTypeLabel(NodeType t) {
	switch (t) {
	case NodeType::DepthRange: return "Depth Range";
	case NodeType::SkyMask: return "Sky Mask";
	case NodeType::PlayerMatte: return "Player Matte";
	case NodeType::FogMask: return "Fog Mask";
	case NodeType::LumaKey: return "Luma Key";
	case NodeType::MaskCombine: return "Combine Mask";
	case NodeType::ColorGrade: return "Color Grade";
	case NodeType::Glow: return "Glow";
	case NodeType::Outline: return "Outline";
	case NodeType::Distortion: return "Distortion";
	case NodeType::Output: return "Output";
	default: return "Node";
	}
}

inline bool NodeTypeFromToken(const char* s, NodeType& out) {
	if (!s) return false;
	for (int i = 0; i < (int)NodeType::Count; ++i)
		if (0 == _stricmp(s, NodeTypeToken((NodeType)i))) { out = (NodeType)i; return true; }
	return false;
}

struct Node {
	int id = 0;
	NodeType type = NodeType::ColorGrade;
	std::string label;
	float x = 120.0f, y = 120.0f; // canvas coordinates for the interactive graph editor
	bool enabled = true;
	// Up to 8 numeric parameters per node (meaning defined by NodeType):
	// DepthRange:  [0]=near(100), [1]=far(1400), [2]=softness(150), [3]=invert(0)
	// SkyMask:     [0]=cutoff(18000), [1]=softness(500), [2]=invert(0)
	// PlayerMatte: [0]=selectedOnly(0=all,1=selected), [1]=feather(1.0), [2]=invert(0)
	// FogMask:     [0]=start(300), [1]=end(2600), [2]=density(1.0), [3]=invert(0)
	// LumaKey:     [0]=threshold(0.65), [1]=softness(0.15), [2]=invert(0)
	// MaskCombine: [0]=op(0=union,1=intersect,2=subtract,3=invert)
	// ColorGrade:  [0]=saturation(1), [1]=contrast(1), [2]=exposure(0), [3]=temp(0),
	//              [4]=tintR(1), [5]=tintG(1), [6]=tintB(1), [7]=mix(1)
	// Glow:        [0]=threshold(0.55), [1]=intensity(1.0), [2]=radius(12), [3]=tintR(1),
	//              [4]=tintG(0.9), [5]=tintB(0.75), [6]=mix(1)
	// Outline:     [0]=thickness(2.5), [1]=threshold(0.25), [2]=colorR(1), [3]=colorG(0.78),
	//              [4]=colorB(0.22), [5]=opacity(0.95)
	// Distortion:  [0]=mode(0=ripple,1=heathaze,2=radial,3=chromatic), [1]=strength(0.35),
	//              [2]=scale(14), [3]=speed(2.0), [4]=chromatic(0.35), [5]=mix(1)
	// Output:      [0]=masterMix(1.0)
	float p[8] = {};
};

struct Edge {
	int id = 0;
	int fromNode = 0;
	int fromPort = 0; // 0 = primary output (color or mask)
	int toNode = 0;
	int toPort = 0;   // for Effect: 0 = color in, 1 = mask in; for MaskCombine: 0 = mask A, 1 = mask B
};

struct NodeGraph {
	bool enabled = false;
	std::string presetName;
	int selectedNodeId = -1;
	int nextNodeId = 1;
	int nextEdgeId = 1;
	std::vector<Node> nodes;
	std::vector<Edge> edges;
};

inline Node MakeDefaultNode(int id, NodeType type, float x, float y) {
	Node n;
	n.id = id;
	n.type = type;
	n.label = NodeTypeLabel(type);
	n.x = x;
	n.y = y;
	switch (type) {
	case NodeType::DepthRange:
		n.p[0] = 100.0f; n.p[1] = 1400.0f; n.p[2] = 150.0f; n.p[3] = 0.0f;
		break;
	case NodeType::SkyMask:
		n.p[0] = 18000.0f; n.p[1] = 500.0f; n.p[2] = 0.0f;
		break;
	case NodeType::PlayerMatte:
		n.p[0] = 0.0f; n.p[1] = 1.0f; n.p[2] = 0.0f;
		break;
	case NodeType::FogMask:
		n.p[0] = 320.0f; n.p[1] = 2600.0f; n.p[2] = 1.0f; n.p[3] = 0.0f;
		break;
	case NodeType::LumaKey:
		n.p[0] = 0.65f; n.p[1] = 0.15f; n.p[2] = 0.0f;
		break;
	case NodeType::MaskCombine:
		n.p[0] = 0.0f;
		break;
	case NodeType::ColorGrade:
		n.p[0] = 1.0f; n.p[1] = 1.0f; n.p[2] = 0.0f; n.p[3] = 0.0f;
		n.p[4] = 1.0f; n.p[5] = 1.0f; n.p[6] = 1.0f; n.p[7] = 1.0f;
		break;
	case NodeType::Glow:
		n.p[0] = 0.55f; n.p[1] = 1.0f; n.p[2] = 12.0f;
		n.p[3] = 1.0f; n.p[4] = 0.90f; n.p[5] = 0.72f; n.p[6] = 1.0f;
		break;
	case NodeType::Outline:
		n.p[0] = 2.5f; n.p[1] = 0.25f;
		n.p[2] = 1.0f; n.p[3] = 0.78f; n.p[4] = 0.22f; n.p[5] = 0.95f;
		break;
	case NodeType::Distortion:
		n.p[0] = 1.0f; n.p[1] = 0.35f; n.p[2] = 14.0f; n.p[3] = 2.0f; n.p[4] = 0.35f; n.p[5] = 1.0f;
		break;
	case NodeType::Output:
		n.p[0] = 1.0f;
		break;
	default: break;
	}
	return n;
}

struct CompiledStep {
	int nodeId = 0;
	NodeType type = NodeType::ColorGrade;
	int outMaskSlot = -1; // 0..3 when IsMaskNode
	int maskSlotA = -1;   // bound mask slot for Effect or MaskCombine A
	int maskSlotB = -1;   // bound mask slot for MaskCombine B
	float p[8] = {};
	// The mask nodes this step reads (MaskCombine A/B, an Effect's mask), before slots are assigned.
	int maskNodeA = -1;
	int maskNodeB = -1;
};

struct CompiledPlan {
	bool enabled = false;
	bool needsWorldDepthRef = false;
	float masterMix = 1.0f;
	// Steps dropped because more than kMaskSlots masks would have to be alive at once.
	int droppedForMaskSlots = 0;
	std::vector<CompiledStep> steps;
};

// Gives every mask step a slot that no mask still waiting for a reader holds. A slot is free again
// only after the last step that reads its mask has run, so a mask feeding several effects, or the
// first branch of a deep MaskCombine tree, is never overwritten before it is used. A step whose
// mask would need a fifth live slot is dropped (with everything that reads it) instead of
// overwriting a live mask, and counted in droppedForMaskSlots.
inline void AssignMaskSlots(CompiledPlan& plan) {
	std::map<int, size_t> lastRead; // mask node id -> index of its last reader
	for (size_t i = 0; i < plan.steps.size(); ++i) {
		const CompiledStep& st = plan.steps[i];
		if (st.maskNodeA > 0) lastRead[st.maskNodeA] = i;
		if (st.maskNodeB > 0) lastRead[st.maskNodeB] = i;
	}
	std::map<int, int> slotOf; // mask node id -> slot
	int owner[kMaskSlots];
	for (int& o : owner) o = -1;
	std::vector<CompiledStep> kept;
	for (size_t i = 0; i < plan.steps.size(); ++i) {
		CompiledStep st = plan.steps[i];
		const auto bound = [&](int node) { auto it = slotOf.find(node); return it == slotOf.end() ? -1 : it->second; };
		const bool needsA = st.maskNodeA > 0, needsB = st.maskNodeB > 0;
		st.maskSlotA = needsA ? bound(st.maskNodeA) : -1;
		st.maskSlotB = needsB ? bound(st.maskNodeB) : -1;
		const bool inputsLost = (needsA && st.maskSlotA < 0) || (needsB && st.maskSlotB < 0);
		if (IsMaskNode(st.type)) {
			int slot = -1;
			if (!inputsLost)
				for (int s = 0; s < kMaskSlots && slot < 0; ++s) if (owner[s] < 0) slot = s;
			if (slot < 0) { ++plan.droppedForMaskSlots; }
			else {
				st.outMaskSlot = slot;
				owner[slot] = st.nodeId;
				slotOf[st.nodeId] = slot;
			}
		} else if (inputsLost && IsEffectNode(st.type)) {
			// Its mask was dropped: apply the effect unmasked would change the look, so skip it.
			++plan.droppedForMaskSlots;
		}
		const bool keep = IsMaskNode(st.type) ? st.outMaskSlot >= 0 : !(inputsLost && IsEffectNode(st.type));
		if (keep) kept.push_back(st);
		// Free the slots of masks this step was the last reader of (after the step, so a MaskCombine
		// never writes into a slot it is still reading).
		for (int node : { st.maskNodeA, st.maskNodeB }) {
			auto lr = lastRead.find(node);
			if (node > 0 && lr != lastRead.end() && lr->second == i) {
				const int s = bound(node);
				if (s >= 0 && owner[s] == node) owner[s] = -1;
			}
		}
		// A mask nothing reads frees its slot at once.
		if (IsMaskNode(st.type) && st.outMaskSlot >= 0 && !lastRead.count(st.nodeId)) owner[st.outMaskSlot] = -1;
	}
	plan.steps.swap(kept);
}

// Cycle-safe topological compiler: walks backward from the Output node, emits mask evaluations
// before the effects that consume them, then gives masks slots 0..3 by lifetime (AssignMaskSlots).
inline CompiledPlan CompileGraph(const NodeGraph& g) {
	CompiledPlan plan;
	plan.enabled = g.enabled;
	if (!g.enabled) return plan;

	std::map<int, const Node*> byId;
	const Node* outputNode = nullptr;
	for (const Node& n : g.nodes) {
		byId[n.id] = &n;
		if (n.type == NodeType::Output) outputNode = &n;
	}
	if (!outputNode) return plan;
	plan.masterMix = std::clamp(outputNode->p[0], 0.0f, 1.0f);

	auto findInput = [&](int toNode, int toPort) -> int {
		for (const Edge& e : g.edges)
			if (e.toNode == toNode && e.toPort == toPort) return e.fromNode;
		return -1;
	};

	std::set<int> visiting, visited;
	std::set<int> maskNodes; // mask nodes already emitted

	auto visit = [&](auto&& self, int id) -> void {
		if (id <= 0 || visited.count(id) || visiting.count(id)) return;
		auto it = byId.find(id);
		if (it == byId.end()) return;
		const Node* n = it->second;
		visiting.insert(id);

		// Visit upstream color/mask dependencies first.
		const int in0 = findInput(id, 0);
		const int in1 = findInput(id, 1);
		if (in0 > 0) self(self, in0);
		if (in1 > 0) self(self, in1);

		visiting.erase(id);
		visited.insert(id);

		if (n->type == NodeType::Output || !n->enabled) return;
		if ((int)plan.steps.size() >= kMaxCompiledSteps) return;

		CompiledStep step;
		step.nodeId = n->id;
		step.type = n->type;
		std::memcpy(step.p, n->p, sizeof(step.p));

		if (IsMaskNode(n->type)) {
			maskNodes.insert(n->id);
			if (n->type == NodeType::MaskCombine) {
				if (maskNodes.count(in0)) step.maskNodeA = in0;
				if (maskNodes.count(in1)) step.maskNodeB = in1;
			}
			if (n->type == NodeType::PlayerMatte) plan.needsWorldDepthRef = true;
		} else if (IsEffectNode(n->type)) {
			// Port 1 is the mask input on every Effect node.
			if (maskNodes.count(in1)) step.maskNodeA = in1;
			else if (maskNodes.count(in0)) step.maskNodeA = in0;
		}
		plan.steps.push_back(step);
	};

	visit(visit, outputNode->id);
	AssignMaskSlots(plan);
	return plan;
}

// Built-in starter presets matching the user's examples and ready to tweak in the graph editor!
inline NodeGraph BuildPreset(const std::string& name, bool enableNow = true) {
	NodeGraph g;
	g.enabled = enableNow;
	g.presetName = name;

	auto addNode = [&](NodeType t, float x, float y, const char* customLabel = nullptr) -> Node& {
		Node n = MakeDefaultNode(g.nextNodeId++, t, x, y);
		if (customLabel) n.label = customLabel;
		g.nodes.push_back(n);
		return g.nodes.back();
	};
	auto link = [&](int from, int to, int toPort = 0) {
		g.edges.push_back(Edge{ g.nextEdgeId++, from, 0, to, toPort });
	};

	if (name == "color_subject_desat_world") {
		Node& pmWorld = addNode(NodeType::PlayerMatte, 40, 60, "World (Inv Player)");
		pmWorld.p[0] = 0.0f; pmWorld.p[2] = 1.0f; // inverted player matte -> selects world
		const int pmWorldId = pmWorld.id;

		Node& pmSubj = addNode(NodeType::PlayerMatte, 40, 210, "Subject Matte");
		pmSubj.p[0] = 0.0f; pmSubj.p[2] = 0.0f;
		const int pmSubjId = pmSubj.id;

		Node& desat = addNode(NodeType::ColorGrade, 270, 60, "Desaturate World");
		desat.p[0] = 0.05f; desat.p[1] = 1.15f; desat.p[2] = -0.18f; desat.p[7] = 1.0f;
		const int desatId = desat.id;

		Node& pop = addNode(NodeType::ColorGrade, 500, 140, "Color Subject");
		pop.p[0] = 1.38f; pop.p[1] = 1.08f; pop.p[2] = 0.08f; pop.p[3] = 0.12f; pop.p[7] = 1.0f;
		const int popId = pop.id;

		Node& out = addNode(NodeType::Output, 730, 140, "Final Output");
		link(pmWorldId, desatId, 1);
		link(desatId, popId, 0);
		link(pmSubjId, popId, 1);
		link(popId, out.id, 0);
		g.selectedNodeId = desatId;
	} else if (name == "glow_through_fog") {
		Node& fog = addNode(NodeType::FogMask, 40, 80, "Fog Distance Mask");
		fog.p[0] = 280.0f; fog.p[1] = 2400.0f; fog.p[2] = 1.15f;
		const int fogId = fog.id;

		Node& glow = addNode(NodeType::Glow, 280, 80, "Fog Bloom");
		glow.p[0] = 0.42f; glow.p[1] = 1.45f; glow.p[2] = 16.0f;
		glow.p[3] = 1.0f; glow.p[4] = 0.86f; glow.p[5] = 0.65f; glow.p[6] = 1.0f;
		const int glowId = glow.id;

		Node& grade = addNode(NodeType::ColorGrade, 510, 120, "Atmospheric Grade");
		grade.p[0] = 1.10f; grade.p[1] = 1.08f; grade.p[2] = 0.05f; grade.p[3] = 0.18f; grade.p[7] = 0.85f;
		const int gradeId = grade.id;

		Node& out = addNode(NodeType::Output, 740, 120, "Final Output");
		link(fogId, glowId, 1);
		link(glowId, gradeId, 0);
		link(fogId, gradeId, 1);
		link(gradeId, out.id, 0);
		g.selectedNodeId = glowId;
	} else if (name == "player_neon_outline") {
		Node& pm = addNode(NodeType::PlayerMatte, 40, 110, "Player Matte");
		pm.p[0] = 0.0f; pm.p[2] = 0.0f;
		const int pmId = pm.id;

		Node& outline = addNode(NodeType::Outline, 280, 110, "Neon Silhouette");
		outline.p[0] = 3.0f; outline.p[1] = 0.2f;
		outline.p[2] = 1.0f; outline.p[3] = 0.72f; outline.p[4] = 0.18f; outline.p[5] = 1.0f;
		const int outlId = outline.id;

		Node& glow = addNode(NodeType::Glow, 510, 110, "Edge Glow");
		glow.p[0] = 0.60f; glow.p[1] = 1.25f; glow.p[2] = 10.0f;
		glow.p[3] = 1.0f; glow.p[4] = 0.76f; glow.p[5] = 0.28f; glow.p[6] = 0.9f;
		const int glowId = glow.id;

		Node& out = addNode(NodeType::Output, 740, 110, "Final Output");
		link(pmId, outlId, 1);
		link(outlId, glowId, 0);
		link(glowId, out.id, 0);
		g.selectedNodeId = outlId;
	} else if (name == "sky_grade_heat_haze") {
		Node& sky = addNode(NodeType::SkyMask, 40, 60, "Sky Mask");
		const int skyId = sky.id;

		Node& depth = addNode(NodeType::DepthRange, 40, 210, "Far Distance");
		depth.p[0] = 550.0f; depth.p[1] = 3200.0f; depth.p[2] = 260.0f;
		const int depthId = depth.id;

		Node& skyGrade = addNode(NodeType::ColorGrade, 280, 60, "Dramatic Sky");
		skyGrade.p[0] = 1.45f; skyGrade.p[1] = 1.18f; skyGrade.p[2] = -0.10f;
		skyGrade.p[4] = 1.15f; skyGrade.p[5] = 0.82f; skyGrade.p[6] = 0.65f; skyGrade.p[7] = 1.0f;
		const int skyGradeId = skyGrade.id;

		Node& haze = addNode(NodeType::Distortion, 520, 140, "Heat Haze");
		haze.p[0] = 1.0f; haze.p[1] = 0.35f; haze.p[2] = 16.0f; haze.p[3] = 2.4f; haze.p[4] = 0.25f; haze.p[5] = 0.85f;
		const int hazeId = haze.id;

		Node& out = addNode(NodeType::Output, 750, 140, "Final Output");
		link(skyId, skyGradeId, 1);
		link(skyGradeId, hazeId, 0);
		link(depthId, hazeId, 1);
		link(hazeId, out.id, 0);
		g.selectedNodeId = skyGradeId;
	} else {
		// depth_spotlight (default)
		g.presetName = "depth_spotlight";
		Node& depth = addNode(NodeType::DepthRange, 40, 100, "Outside Subject Band");
		depth.p[0] = 120.0f; depth.p[1] = 750.0f; depth.p[2] = 160.0f; depth.p[3] = 1.0f;
		const int depthId = depth.id;

		Node& bgGrade = addNode(NodeType::ColorGrade, 290, 100, "Darken Surroundings");
		bgGrade.p[0] = 0.35f; bgGrade.p[1] = 1.15f; bgGrade.p[2] = -0.55f; bgGrade.p[7] = 0.9f;
		const int bgId = bgGrade.id;

		Node& out = addNode(NodeType::Output, 560, 100, "Final Output");
		link(depthId, bgId, 1);
		link(bgId, out.id, 0);
		g.selectedNodeId = bgId;
	}
	return g;
}

} // namespace Filmmaker::EffectNodes
