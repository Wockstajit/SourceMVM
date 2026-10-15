#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "PropPreview.h"
#include "ClientEntitySpawn.h"
#include "../Debug/MvmDebugLog.h"
#include "../Cosmetics/CosmeticModelSwap.h"
#include "../World/WorldSchema.h"
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace Filmmaker {

namespace {

// The panel's camera (cam_loadoutmenu_ct) frames a standing agent: a model this many units across
// fills the box the way an agent does.
constexpr float kFitSize = 64.0f;
// The camera aims at a standing agent's middle, this high above its feet.
constexpr float kFrameHeight = 38.0f;
// One turn this often: slow enough to read the model, quick enough to see every side.
constexpr double kTurnSeconds = 8.0;
// Where the measuring copy is spawned: far under any map, never on screen, and INSIDE the network
// cell grid (32 cells of 1024 units, -16384..16384). -30000 put it in cell -13: CS2 logged "outside
// of cell bounds" for every model browsed, and a client.dll null read on a worker thread followed
// the next unpause (user crash report 2026-10-12).
const Environment::Vec3 kMeasureAt{0, 0, -15000};


std::string g_model;
unsigned char* g_entity = nullptr;
int g_index = -1;
bool g_measured = false;
Environment::Vec3 g_mins, g_maxs;
float g_size = 0, g_scale = 1;
ULONGLONG g_lastScan = 0, g_lastApply = 0;

// The pawn's scene-node pose before the first turn, so each frame turns the REST pose (the writes
// never accumulate). A static model has no bones to pose: the node carries the whole model.
struct NodeRest { unsigned char* node = nullptr; float x[8] = {}; float angles[3] = {}; };
NodeRest g_rest;
ULONGLONG g_turnStart = 0;
unsigned long long g_turnWrites = 0;

unsigned char* EntityAt(int index) {
	if (index < 0 || index >= 0x8000 || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return nullptr;
	__try { return static_cast<unsigned char*>(g_GetEntityFromIndex(*g_pEntityList, index)); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// CModelState::m_ModelName of an entity, via the same chain the cosmetics diagnostics read.
bool ModelName(unsigned char* entity, char* out, size_t size) {
	const auto& o = g_clientDllOffsets.ModelChain;
	if (!entity || !o.m_CBodyComponent || !o.m_skeletonInstance || !o.m_modelState || !o.m_ModelName) return false;
	__try {
		unsigned char* body = *reinterpret_cast<unsigned char**>(entity + o.m_CBodyComponent);
		if (reinterpret_cast<uintptr_t>(body) < 0x10000) return false;
		const char* name = *reinterpret_cast<const char**>(body + o.m_skeletonInstance + o.m_modelState + o.m_ModelName);
		if (reinterpret_cast<uintptr_t>(name) < 0x10000) return false;
		size_t i = 0;
		for (; name[i] && i + 1 < size; ++i) out[i] = name[i];
		out[i] = 0;
		return i > 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool IsPreviewPawn(unsigned char* entity) {
	__try {
		const char* cls = reinterpret_cast<CEntityInstance*>(entity)->GetClassName();
		return cls && std::strstr(cls, "previewplayer") != nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The panel's pawn: the preview pawn wearing the marker agent (about to receive the asset) or
// already wearing the asset.
unsigned char* FindPanelPawn(int& outIndex) {
	for (int i = 0x4000; i < 0x8000; ++i) {
		unsigned char* e = EntityAt(i);
		char name[260];
		if (!e || !IsPreviewPawn(e) || !ModelName(e, name, sizeof(name))) continue;
		if (!_stricmp(name, kPropPreviewMarkerModel) || !_stricmp(name, g_model.c_str())) { outIndex = i; return e; }
	}
	return nullptr;
}

// The pawn's own bounds are the player hull, not the model's: a throwaway prop_dynamic of the
// model, spawned out of sight, reports the model's real bounds.
void Measure() {
	g_measured = true;
	unsigned char* probe = Props::SpawnClientEntity("prop_dynamic", "mvm_prop_measure", g_model.c_str(), kMeasureAt, {}, 1.0f);
	if (!probe) return;
	if (Props::ClientEntityBounds(probe, g_mins, g_maxs))
		g_size = std::max({g_maxs.x - g_mins.x, g_maxs.y - g_mins.y, g_maxs.z - g_mins.z});
	Props::RemoveClientEntity(probe);
	g_scale = g_size > 0.5f ? std::clamp(kFitSize / g_size, 0.002f, 20.0f) : 1.0f;
	MvmDebugLog_LinefAlways("props.preview", "measure %s mins=(%.1f %.1f %.1f) maxs=(%.1f %.1f %.1f) size=%.1f scale=%.4f",
		g_model.c_str(), g_mins.x, g_mins.y, g_mins.z, g_maxs.x, g_maxs.y, g_maxs.z, g_size, g_scale);
}

// The fit scale and the paused-demo render override (a pawn spawned during a pause never
// interpolates, so the renderer would skip it -- see Panorama/PreviewRenderFlag.h).
void Fit(unsigned char* e, std::ptrdiff_t nodeScale) {
	__try {
		if (const auto at = g_clientDllOffsets.C_BaseEntity.m_bRenderEvenWhenNotSuccessfullyInterpolated)
			e[at] = 1;
		if (nodeScale && g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode) {
			unsigned char* node = *reinterpret_cast<unsigned char**>(e + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
			if (reinterpret_cast<uintptr_t>(node) > 0x10000)
				*reinterpret_cast<float*>(node + nodeScale) = g_scale;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {}
}

unsigned char* SceneNode(unsigned char* e) {
	__try {
		unsigned char* node = *reinterpret_cast<unsigned char**>(e + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
		return reinterpret_cast<uintptr_t>(node) > 0x10000 ? node : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool CaptureRest(unsigned char* node) {
	const auto& o = g_clientDllOffsets.CGameSceneNode;
	__try {
		std::memcpy(g_rest.x, node + o.m_nodeToWorld, sizeof(g_rest.x));
		std::memcpy(g_rest.angles, node + o.m_angRotation, sizeof(g_rest.angles));
		g_rest.node = node;
		return std::isfinite(g_rest.x[3]) && g_rest.x[3] > 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The node turned `yaw` radians about the vertical axis through the centre of the model's bounds.
// The derived world transform and the origin/angle sources it is rebuilt from are written together
// (see RagdollWeapon's late write: the lazy rebuild copies the sources over the transform).
bool WriteTurn(unsigned char* node, float yaw) {
	const auto& o = g_clientDllOffsets.CGameSceneNode;
	const float hs = std::sin(yaw * 0.5f), hc = std::cos(yaw * 0.5f);
	const float* q0 = g_rest.x + 4;
	// q = (0, 0, hs, hc) * q0
	const float q[4] = {hc * q0[0] - hs * q0[1], hc * q0[1] + hs * q0[0], hc * q0[2] + hs * q0[3], hc * q0[3] - hs * q0[2]};
	auto rotate = [](const float* r, const float v[3], float out[3]) {
		const float tx = 2 * (r[1] * v[2] - r[2] * v[1]), ty = 2 * (r[2] * v[0] - r[0] * v[2]), tz = 2 * (r[0] * v[1] - r[1] * v[0]);
		out[0] = v[0] + r[3] * tx + (r[1] * tz - r[2] * ty);
		out[1] = v[1] + r[3] * ty + (r[2] * tx - r[0] * tz);
		out[2] = v[2] + r[3] * tz + (r[0] * ty - r[1] * tx);
	};
	// The bounds centre goes where the camera looks: kFrameHeight above the pawn's feet.
	const float c[3] = {(g_mins.x + g_maxs.x) * 0.5f, (g_mins.y + g_maxs.y) * 0.5f, (g_mins.z + g_maxs.z) * 0.5f};
	float b[3];
	rotate(q, c, b);
	const float scale = g_scale; // the fit (the captured rest may predate it)
	float x[8] = {g_rest.x[0] - scale * b[0], g_rest.x[1] - scale * b[1], g_rest.x[2] + kFrameHeight - scale * b[2],
		scale, q[0], q[1], q[2], q[3]};
	const float angles[3] = {g_rest.angles[0], g_rest.angles[1] + yaw * 57.29577951308232f, g_rest.angles[2]};
	__try {
		std::memcpy(node + o.m_nodeToWorld, x, sizeof(x));
		std::memcpy(node + o.m_vecOrigin, x, 3 * sizeof(float));
		std::memcpy(node + o.m_angRotation, angles, sizeof(angles));
		std::memcpy(node + o.m_angAbsRotation, angles, sizeof(angles));
		if (!*reinterpret_cast<unsigned char**>(node + o.m_pParent)) {
			std::memcpy(node + o.m_vecWrappedLocalOrigin, x, 3 * sizeof(float));
			std::memcpy(node + o.m_angWrappedLocalRotation, angles, sizeof(angles));
		}
		if (o.m_vecAbsOrigin) std::memcpy(node + o.m_vecAbsOrigin, x, 3 * sizeof(float));
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

void PropPreview_Set(const std::string& model) {
	if (model == g_model) return;
	g_model = model;
	// The panel is reused for every model, so its pawn (and the rest pose the turn is written from)
	// stays: RunFrame just puts the new asset on it. Stopping lets both go.
	if (model.empty()) {
		g_entity = nullptr;
		g_index = -1;
		g_rest = {};
	}
	g_measured = false;
	g_mins = g_maxs = {};
	g_size = 0;
	g_scale = 1;
	g_lastScan = g_lastApply = 0;
}

const std::string& PropPreview_Model() { return g_model; }
bool PropPreview_Active() { return !g_model.empty(); }

void PropPreview_RunFrame() {
	if (g_model.empty()) return;
	if (!g_measured) Measure();
	const ULONGLONG now = GetTickCount64();
	if (!g_entity || EntityAt(g_index) != g_entity) {
		g_entity = nullptr;
		if (now - g_lastScan < 200) return;
		g_lastScan = now;
		g_entity = FindPanelPawn(g_index);
		if (!g_entity) return;
		MvmDebugLog_LinefAlways("props.preview", "pawn index=%d for %s", g_index, g_model.c_str());
	}
	// The panel may put its own model back (when it refreshes); the asset goes back on.
	char name[260];
	if ((!ModelName(g_entity, name, sizeof(name)) || _stricmp(name, g_model.c_str())) && now - g_lastApply > 500) {
		g_lastApply = now;
		const bool ok = ApplyPreviewModel(g_entity, g_model.c_str());
		MvmDebugLog_LinefAlways("props.preview", "set model index=%d %s ok=%d", g_index, g_model.c_str(), ok ? 1 : 0);
	}
	Fit(g_entity, WorldSchema_Offset(kWF_Node_Scale));
}

void PropPreview_RunLateFrame() {
	const auto& o = g_clientDllOffsets.CGameSceneNode;
	if (g_model.empty() || !g_entity || EntityAt(g_index) != g_entity || !o.m_nodeToWorld || !o.m_vecOrigin ||
	    !o.m_angRotation || !o.m_angAbsRotation || !o.m_vecWrappedLocalOrigin || !o.m_angWrappedLocalRotation || !o.m_pParent)
		return;
	unsigned char* node = SceneNode(g_entity);
	if (!node) return;
	if (g_rest.node != node) {
		if (!CaptureRest(node)) { g_rest = {}; return; }
		if (!g_turnStart) g_turnStart = GetTickCount64();
		MvmDebugLog_LinefAlways("props.preview", "turn rest pos=(%.1f %.1f %.1f) scale=%.3f yaw=%.1f",
			g_rest.x[0], g_rest.x[1], g_rest.x[2], g_rest.x[3], g_rest.angles[1]);
	}
	// Wall clock, not demo time: it turns the same whether the demo plays or is paused.
	const double t = double(GetTickCount64() - g_turnStart) / 1000.0;
	if (WriteTurn(node, float(std::fmod(t / kTurnSeconds, 1.0) * 6.283185307179586))) ++g_turnWrites;
}

void PropPreview_AppendJson(std::string& out) {
	char buf[160];
	std::snprintf(buf, sizeof(buf), ",\"previewFound\":%s,\"previewSize\":%.1f,\"previewTurns\":%llu",
		g_entity ? "true" : "false", g_size, g_turnWrites);
	out += buf;
}

} // namespace Filmmaker
