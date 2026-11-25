#define NOMINMAX
#include "EffectNodes.h"

#include "../Platform/JsonBuilder.h"
#include "../../../Shared/AfxConsole.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Filmmaker::EffectNodes {
namespace {

std::mutex g_mutex;
NodeGraph g_graph = BuildPreset("color_subject_desat_world", false);
CompiledPlan g_plan;
std::atomic<bool> g_active{ false };
std::atomic<bool> g_needsWorldDepth{ false };
bool g_graphCanvasOpen = false;

void RecompileLocked() {
	g_plan = CompileGraph(g_graph);
	g_active.store(g_plan.enabled && !g_plan.steps.empty(), std::memory_order_release);
	g_needsWorldDepth.store(g_plan.enabled && g_plan.needsWorldDepthRef, std::memory_order_release);
}

Node* FindNodeLocked(int id) {
	for (Node& n : g_graph.nodes)
		if (n.id == id) return &n;
	return nullptr;
}

} // namespace

bool Active() {
	return g_active.load(std::memory_order_acquire);
}

bool NeedsWorldDepthRef() {
	return g_needsWorldDepth.load(std::memory_order_acquire);
}

CompiledPlan SnapshotPlan() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_plan;
}

void LoadPreset(const std::string& presetName, bool enable) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_graph = BuildPreset(presetName, enable);
	RecompileLocked();
}

void SetEnabled(bool on) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_graph.enabled = on;
	RecompileLocked();
}

void Reset() {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_graph = BuildPreset("color_subject_desat_world", false);
	g_graphCanvasOpen = false;
	RecompileLocked();
}

void AppendStateJson(std::ostringstream& out) {
	std::lock_guard<std::mutex> lock(g_mutex);
	out << ",\"effectNodes\":{"
		<< "\"on\":" << (g_graph.enabled ? "true" : "false")
		<< ",\"canvasOpen\":" << (g_graphCanvasOpen ? "true" : "false")
		<< ",\"preset\":\"" << JsonBuilder::Escape(g_graph.presetName) << "\""
		<< ",\"selected\":" << g_graph.selectedNodeId
		<< ",\"steps\":" << g_plan.steps.size()
		<< ",\"nodes\":[";
	for (size_t i = 0; i < g_graph.nodes.size(); ++i) {
		const Node& n = g_graph.nodes[i];
		if (i) out << ",";
		out << "{\"id\":" << n.id
			<< ",\"type\":\"" << NodeTypeToken(n.type) << "\""
			<< ",\"label\":\"" << JsonBuilder::Escape(n.label) << "\""
			<< ",\"mask\":" << (IsMaskNode(n.type) ? "true" : "false")
			<< ",\"x\":" << (int)n.x << ",\"y\":" << (int)n.y
			<< ",\"enabled\":" << (n.enabled ? "true" : "false")
			<< ",\"p\":[" << n.p[0] << "," << n.p[1] << "," << n.p[2] << "," << n.p[3]
			<< "," << n.p[4] << "," << n.p[5] << "," << n.p[6] << "," << n.p[7] << "]}";
	}
	out << "],\"edges\":[";
	for (size_t i = 0; i < g_graph.edges.size(); ++i) {
		const Edge& e = g_graph.edges[i];
		if (i) out << ",";
		out << "{\"id\":" << e.id
			<< ",\"from\":" << e.fromNode << ",\"fromPort\":" << e.fromPort
			<< ",\"to\":" << e.toNode << ",\"toPort\":" << e.toPort << "}";
	}
	out << "]}";
}

void RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 3) {
		advancedfx::Message(
			"%s nodes on|off|toggle|reset\n"
			"%s nodes preset <color_subject_desat_world|glow_through_fog|player_neon_outline|sky_grade_heat_haze|depth_spotlight>\n"
			"%s nodes canvas open|close|toggle\n"
			"%s nodes add <depth_range|sky_mask|player_matte|fog_mask|luma_key|mask_combine|grade|glow|outline|distortion> [x y]\n"
			"%s nodes select <id> | remove <id> | enable <id> <0|1> | move <id> <x> <y> | param <id> <idx 0..7> <val>\n"
			"%s nodes connect <fromId> <toId> [toPort 0|1] | disconnect <edgeId>\n",
			cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}
	const char* sub = args->ArgV(2);
	if (0 == _stricmp(sub, "on")) { SetEnabled(true); return; }
	if (0 == _stricmp(sub, "off")) { SetEnabled(false); return; }
	if (0 == _stricmp(sub, "toggle")) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_graph.enabled = !g_graph.enabled;
		RecompileLocked();
		return;
	}
	if (0 == _stricmp(sub, "reset")) { Reset(); return; }
	if (0 == _stricmp(sub, "preset") && argc >= 4) {
		LoadPreset(args->ArgV(3), true);
		return;
	}
	if (0 == _stricmp(sub, "canvas") && argc >= 4) {
		std::lock_guard<std::mutex> lock(g_mutex);
		const char* a = args->ArgV(3);
		if (0 == _stricmp(a, "open") || 0 == _stricmp(a, "on")) g_graphCanvasOpen = true;
		else if (0 == _stricmp(a, "close") || 0 == _stricmp(a, "off")) g_graphCanvasOpen = false;
		else if (0 == _stricmp(a, "toggle")) g_graphCanvasOpen = !g_graphCanvasOpen;
		advancedfx::Message("%s nodes canvas: %s\n", cmd, g_graphCanvasOpen
			? "open. It is drawn over the preview while CONFIG is open on the NODES tab." : "closed");
		return;
	}
	if (0 == _stricmp(sub, "add") && argc >= 4) {
		NodeType t = NodeType::ColorGrade;
		if (!NodeTypeFromToken(args->ArgV(3), t) || t == NodeType::Output) {
			advancedfx::Warning("%s nodes: unknown node type '%s'.\n", cmd, args->ArgV(3));
			return;
		}
		std::lock_guard<std::mutex> lock(g_mutex);
		const float x = argc >= 6 ? (float)std::atof(args->ArgV(4)) : (180.0f + (float)((g_graph.nodes.size() * 48) % 360));
		const float y = argc >= 6 ? (float)std::atof(args->ArgV(5)) : (90.0f + (float)((g_graph.nodes.size() * 38) % 240));
		Node n = MakeDefaultNode(g_graph.nextNodeId++, t, x, y);
		const int newId = n.id;
		g_graph.nodes.push_back(n);
		g_graph.selectedNodeId = newId;
		// Auto-splice an Effect node right before Output so newly added nodes show an immediate effect!
		if (IsEffectNode(t)) {
			int outId = -1;
			for (const Node& q : g_graph.nodes) if (q.type == NodeType::Output) { outId = q.id; break; }
			if (outId > 0) {
				int prevFrom = -1;
				for (Edge& e : g_graph.edges) {
					if (e.toNode == outId && e.toPort == 0) {
						prevFrom = e.fromNode;
						e.fromNode = newId;
						break;
					}
				}
				if (prevFrom > 0)
					g_graph.edges.push_back(Edge{ g_graph.nextEdgeId++, prevFrom, 0, newId, 0 });
				else
					g_graph.edges.push_back(Edge{ g_graph.nextEdgeId++, newId, 0, outId, 0 });
			}
		}
		RecompileLocked();
		return;
	}
	if (0 == _stricmp(sub, "select") && argc >= 4) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_graph.selectedNodeId = std::atoi(args->ArgV(3));
		return;
	}
	if (0 == _stricmp(sub, "remove") && argc >= 4) {
		const int id = std::atoi(args->ArgV(3));
		std::lock_guard<std::mutex> lock(g_mutex);
		Node* target = FindNodeLocked(id);
		if (!target || target->type == NodeType::Output) return;
		// Bypass color chain smoothly when removing a middle effect node.
		int upstreamColor = -1, downstreamColor = -1;
		for (const Edge& e : g_graph.edges) {
			if (e.toNode == id && e.toPort == 0) upstreamColor = e.fromNode;
			if (e.fromNode == id && e.toPort == 0) downstreamColor = e.toNode;
		}
		g_graph.edges.erase(std::remove_if(g_graph.edges.begin(), g_graph.edges.end(),
			[id](const Edge& e) { return e.fromNode == id || e.toNode == id; }), g_graph.edges.end());
		if (upstreamColor > 0 && downstreamColor > 0 && upstreamColor != downstreamColor)
			g_graph.edges.push_back(Edge{ g_graph.nextEdgeId++, upstreamColor, 0, downstreamColor, 0 });
		g_graph.nodes.erase(std::remove_if(g_graph.nodes.begin(), g_graph.nodes.end(),
			[id](const Node& n) { return n.id == id; }), g_graph.nodes.end());
		if (g_graph.selectedNodeId == id)
			g_graph.selectedNodeId = g_graph.nodes.empty() ? -1 : g_graph.nodes.front().id;
		RecompileLocked();
		return;
	}
	if (0 == _stricmp(sub, "enable") && argc >= 5) {
		const int id = std::atoi(args->ArgV(3));
		const bool on = std::atoi(args->ArgV(4)) != 0;
		std::lock_guard<std::mutex> lock(g_mutex);
		if (Node* n = FindNodeLocked(id)) { n->enabled = on; RecompileLocked(); }
		return;
	}
	if (0 == _stricmp(sub, "move") && argc >= 6) {
		const int id = std::atoi(args->ArgV(3));
		std::lock_guard<std::mutex> lock(g_mutex);
		if (Node* n = FindNodeLocked(id)) {
			n->x = std::clamp((float)std::atof(args->ArgV(4)), 0.0f, 1600.0f);
			n->y = std::clamp((float)std::atof(args->ArgV(5)), 0.0f, 900.0f);
		}
		return;
	}
	if (0 == _stricmp(sub, "param") && argc >= 6) {
		const int id = std::atoi(args->ArgV(3));
		const int idx = std::atoi(args->ArgV(4));
		const float val = (float)std::atof(args->ArgV(5));
		std::lock_guard<std::mutex> lock(g_mutex);
		if (idx >= 0 && idx < 8) {
			if (Node* n = FindNodeLocked(id)) { n->p[idx] = val; RecompileLocked(); }
		}
		return;
	}
	if (0 == _stricmp(sub, "connect") && argc >= 5) {
		const int fromId = std::atoi(args->ArgV(3));
		const int toId = std::atoi(args->ArgV(4));
		const int toPort = argc >= 6 ? std::clamp(std::atoi(args->ArgV(5)), 0, 1) : 0;
		if (fromId == toId) return;
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!FindNodeLocked(fromId) || !FindNodeLocked(toId)) return;
		// Replace any existing wire feeding (toId, toPort).
		g_graph.edges.erase(std::remove_if(g_graph.edges.begin(), g_graph.edges.end(),
			[toId, toPort](const Edge& e) { return e.toNode == toId && e.toPort == toPort; }), g_graph.edges.end());
		g_graph.edges.push_back(Edge{ g_graph.nextEdgeId++, fromId, 0, toId, toPort });
		RecompileLocked();
		return;
	}
	if (0 == _stricmp(sub, "disconnect") && argc >= 4) {
		const int eid = std::atoi(args->ArgV(3));
		std::lock_guard<std::mutex> lock(g_mutex);
		g_graph.edges.erase(std::remove_if(g_graph.edges.begin(), g_graph.edges.end(),
			[eid](const Edge& e) { return e.id == eid; }), g_graph.edges.end());
		RecompileLocked();
		return;
	}
	advancedfx::Warning("%s nodes: unknown subcommand '%s'.\n", cmd, sub);
}

} // namespace Filmmaker::EffectNodes
