#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "RenderPlayerMatte.h"

#include "../Filmmaker.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/ModelPoseAccess.h"
#include "../Movie/RagdollBodies.h"
#include "../Cosmetics/CustomizeTargetMath.h"
#include "../Platform/JsonBuilder.h"
#include "../WorldDecals/WorldDecals.h"
#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"
#include "../../../Shared/AfxConsole.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace Filmmaker {
namespace {

struct RosterPlayer {
	int controller = -1;
	int pawn = -1;
	bool alive = false;
	int team = 0;
	int slot = -1;
	uint64_t steam = 0;
	std::string name;
};

std::mutex g_snapMutex;
PlayerMatte::RenderSnapshot g_snapshot;

int g_selectedController = -1;
std::string g_selectedName;
int g_selectedTeam = 0;
PlayerMatte::HighlightSettings g_highlight;
std::vector<RosterPlayer> g_roster;
int g_spectatedController = -1;

CEntityInstance* SafeEnt(int index) {
	if (index < 0 || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex || index > GetHighestEntityIndex())
		return nullptr;
	__try { return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index); }
	__except (1) { return nullptr; }
}

int SafeHandleIndexAt(CEntityInstance* ent, ptrdiff_t offset) {
	if (!ent || offset == 0) return -1;
	unsigned int raw = 0;
	__try { raw = *(unsigned int*)((unsigned char*)ent + offset); } __except (1) { return -1; }
	const SOURCESDK::CS2::CBaseHandle h = SOURCESDK::CS2::CEntityHandle::CEntityHandle(raw);
	return h.IsValid() ? h.GetEntryIndex() : -1;
}

bool SafeIsBodyPawn(CEntityInstance* ent) {
	if (!ent) return false;
	bool pawn = false;
	const char* cls = nullptr;
	__try { pawn = ent->IsPlayerPawn(); cls = ent->GetClientClassName(); } __except (1) { return false; }
	return pawn && !(cls && std::strstr(cls, "Observer"));
}

bool SafeIsAlivePawn(CEntityInstance* ent) {
	if (!SafeIsBodyPawn(ent)) return false;
	unsigned int hp = 0;
	__try { hp = ent->GetHealth(); } __except (1) { return false; }
	return hp > 0;
}

bool SafeIsController(CEntityInstance* ent) {
	__try { return ent && ent->IsPlayerController(); } __except (1) { return false; }
}

int SafeTeam(CEntityInstance* ent) {
	__try { return ent ? ent->GetTeam() : 0; } __except (1) { return 0; }
}

const char* SafeControllerName(CEntityInstance* ent, uint64_t* steamOut) {
	*steamOut = 0;
	__try {
		const char* name = ent->GetSanitizedPlayerName();
		if (!name || !*name) name = ent->GetPlayerName();
		*steamOut = ent->GetSteamId();
		return name;
	} __except (1) { return nullptr; }
}

bool SafeOrigin(CEntityInstance* ent, float out[3]) {
	if (!ent) return false;
	float x = 0, y = 0, z = 0;
	__try { ent->GetOrigin(x, y, z); } __except (1) { return false; }
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
	out[0] = x; out[1] = y; out[2] = z;
	return true;
}

int ResolveBodyPawn(int controller) {
	CEntityInstance* ctrl = SafeEnt(controller);
	const int viaCtrl = SafeHandleIndexAt(ctrl, g_clientDllOffsets.CCSPlayerController.m_hPlayerPawn);
	if (viaCtrl >= 0 && SafeIsBodyPawn(SafeEnt(viaCtrl))) return viaCtrl;
	const ptrdiff_t origOff = g_clientDllOffsets.C_CSPlayerPawn.m_hOriginalController;
	if (origOff == 0) return -1;
	for (int i = 0, n = GetHighestEntityIndex(); i <= n; ++i) {
		CEntityInstance* e = SafeEnt(i);
		if (SafeIsBodyPawn(e) && SafeHandleIndexAt(e, origOff) == controller) return i;
	}
	return -1;
}

// Samples up to 16 evenly distributed bone positions from the pawn's (or its ragdoll's) world
// bone array (CModelState + 0x80, 8 floats per bone: xyz + scale + quat).
int ReadPawnBoneSpheres(CEntityInstance* pawn, const float feetOrigin[3],
	float outBones[PlayerMatte::kMaxTargetBones][4]) {
	if (!pawn) return 0;
	CEntityInstance* carriers[2] = { pawn, RagdollBodies::ClientRagdollOf(pawn) };
	if (!SafeIsAlivePawn(pawn)) std::swap(carriers[0], carriers[1]);
	for (CEntityInstance* carrier : carriers) {
		if (!carrier) continue;
		unsigned char* modelState = ModelPoseAccess::ModelStateOf(carrier);
		if (!modelState) continue;
		int count = 0;
		__try {
			const float* world = *(const float**)(modelState + 0x80);
			if ((uintptr_t)world < 0x10000) continue;
			// Sample core skeleton bones (indices 0..47 stepped by 3) that sit within a human body
			// reach of the pawn's origin so invalid/unbuilt poses cleanly fall back to the capsule.
			for (int b = 0; b < 48 && count < PlayerMatte::kMaxTargetBones; b += 3) {
				const float* bone = world + b * 8;
				const float bx = bone[0], by = bone[1], bz = bone[2];
				if (!std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(bz)) continue;
				const float dx = bx - feetOrigin[0];
				const float dy = by - feetOrigin[1];
				const float dz = bz - feetOrigin[2];
				if (dx * dx + dy * dy + dz * dz > 140.0f * 140.0f) continue;
				outBones[count][0] = bx;
				outBones[count][1] = by;
				outBones[count][2] = bz;
				outBones[count][3] = 17.0f; // includes vest/limbs/held weapon near torso/hands
				++count;
			}
		} __except (1) {
			count = 0;
		}
		if (count >= 4) return count;
	}
	return 0;
}

std::string LowerStr(const std::string& s) {
	std::string out(s);
	for (char& c : out) c = (char)std::tolower((unsigned char)c);
	return out;
}

void RefreshRoster() {
	g_roster.clear();
	g_spectatedController = -1;
	namespace TM = CustomizeTargetMath;
	const std::vector<TM::DemoPlayer> rows = TM::ParseDemoPlayers(CameraTimeline_NativeDemoPlayers());
	if (!rows.empty()) {
		for (const TM::DemoPlayer& row : rows) {
			CEntityInstance* ctrl = SafeEnt(row.controller);
			if (!SafeIsController(ctrl)) continue;
			RosterPlayer p;
			p.controller = row.controller;
			p.alive = row.alive;
			p.team = row.team;
			p.slot = row.slot;
			p.pawn = row.alive ? row.pawn : ResolveBodyPawn(row.controller);
			const char* nm = SafeControllerName(ctrl, &p.steam);
			p.name = (nm && *nm) ? nm : ("Player " + std::to_string(p.controller));
			g_roster.push_back(p);
		}
		const int specPawn = CameraTimeline_NativeSpectatedPawn();
		if (specPawn >= 0) {
			for (const RosterPlayer& p : g_roster) {
				if (p.alive && p.pawn == specPawn) { g_spectatedController = p.controller; break; }
			}
		}
	} else {
		for (int i = 0, n = GetHighestEntityIndex(); i <= n; ++i) {
			CEntityInstance* ctrl = SafeEnt(i);
			if (!SafeIsController(ctrl)) continue;
			const int team = SafeTeam(ctrl);
			if (team != 2 && team != 3) continue;
			RosterPlayer p;
			p.controller = i;
			p.team = team;
			p.pawn = ResolveBodyPawn(i);
			p.alive = SafeIsAlivePawn(SafeEnt(p.pawn));
			const char* nm = SafeControllerName(ctrl, &p.steam);
			p.name = (nm && *nm) ? nm : ("Player " + std::to_string(i));
			g_roster.push_back(p);
		}
	}
	std::stable_sort(g_roster.begin(), g_roster.end(), [](const RosterPlayer& a, const RosterPlayer& b) {
		if (a.team != b.team) return a.team < b.team;
		return LowerStr(a.name) < LowerStr(b.name);
	});
}

const RosterPlayer* FindByController(int controller) {
	for (const RosterPlayer& p : g_roster)
		if (p.controller == controller) return &p;
	return nullptr;
}

} // namespace

void RenderPlayerMatte_OnLevelInit() {
	g_selectedController = -1;
	g_selectedName.clear();
	g_selectedTeam = 0;
	g_roster.clear();
	g_spectatedController = -1;
	std::lock_guard<std::mutex> lock(g_snapMutex);
	g_snapshot = {};
	g_snapshot.highlight = g_highlight;
}

void RenderPlayerMatte_RunMainThreadFrame() {
	static ULONGLONG s_lastRosterMs = 0;
	const ULONGLONG nowMs = GetTickCount64();
	if (nowMs - s_lastRosterMs >= 250 || g_selectedController >= 0) {
		if (nowMs - s_lastRosterMs >= 250) {
			s_lastRosterMs = nowMs;
			RefreshRoster();
		}
	}
	const bool needCapsules = g_selectedController >= 0 || g_highlight.enabled || WorldDecals::AnyEnabled();
	if (!needCapsules)
		return;

	PlayerMatte::RenderSnapshot next;
	next.highlight = g_highlight;

	if (g_selectedController >= 0) {
		const RosterPlayer* picked = FindByController(g_selectedController);
		if (picked) {
			g_selectedName = picked->name;
			g_selectedTeam = picked->team;
			CEntityInstance* targetPawn = SafeEnt(picked->pawn);
			float origin[3] = {};
			if (targetPawn && SafeOrigin(targetPawn, origin)) {
				next.hasTarget = true;
				next.controller = picked->controller;
				next.pawn = picked->pawn;
				next.targetCapsule[0] = origin[0];
				next.targetCapsule[1] = origin[1];
				next.targetCapsule[2] = origin[2];
				next.targetCapsule[3] = picked->alive ? 74.0f : 32.0f;
				next.targetBoneCount = ReadPawnBoneSpheres(targetPawn, origin, next.targetBones);
				PlayerMatte::ComputeTargetBounds(next);
			}
		}
	}

	if (next.hasTarget || WorldDecals::AnyEnabled()) {
		for (const RosterPlayer& other : g_roster) {
			if ((next.hasTarget && other.controller == next.controller) || other.pawn < 0) continue;
			if (next.otherCount >= PlayerMatte::kMaxOtherPlayers) break;
			CEntityInstance* otherPawn = SafeEnt(other.pawn);
			float oOrigin[3] = {};
			if (!otherPawn || !SafeOrigin(otherPawn, oOrigin)) continue;
			next.otherCapsules[next.otherCount][0] = oOrigin[0];
			next.otherCapsules[next.otherCount][1] = oOrigin[1];
			next.otherCapsules[next.otherCount][2] = oOrigin[2];
			next.otherCapsules[next.otherCount][3] = other.alive ? 74.0f : 32.0f;
			++next.otherCount;
		}
	}

	std::lock_guard<std::mutex> lock(g_snapMutex);
	g_snapshot = next;
}

bool RenderPlayerMatte_PickController(int controller) {
	RefreshRoster();
	const RosterPlayer* p = FindByController(controller);
	if (!p) return false;
	g_selectedController = p->controller;
	g_selectedName = p->name;
	g_selectedTeam = p->team;
	MvmDebugLog_LinefAlways("render.playermatte", "picked controller=%d pawn=%d name=\"%s\"",
		p->controller, p->pawn, p->name.c_str());
	return true;
}

bool RenderPlayerMatte_PickSpectated() {
	RefreshRoster();
	if (g_spectatedController < 0) return false;
	return RenderPlayerMatte_PickController(g_spectatedController);
}

void RenderPlayerMatte_Clear() {
	g_selectedController = -1;
	g_selectedName.clear();
	g_selectedTeam = 0;
	std::lock_guard<std::mutex> lock(g_snapMutex);
	g_snapshot.hasTarget = false;
	g_snapshot.controller = -1;
	g_snapshot.pawn = -1;
	g_snapshot.targetBoneCount = 0;
}

int RenderPlayerMatte_SelectedController() { return g_selectedController; }
std::string RenderPlayerMatte_SelectedName() { return g_selectedName; }
bool RenderPlayerMatte_HasTarget() { return g_selectedController >= 0; }

void RenderPlayerMatte_SetHighlightEnabled(bool on) { g_highlight.enabled = on; }
bool RenderPlayerMatte_HighlightEnabled() { return g_highlight.enabled; }
void RenderPlayerMatte_SetHighlightDim(float v) { g_highlight.dimWorld = PlayerMatte::Clamp(v, 0.0f, 1.0f); }
void RenderPlayerMatte_SetHighlightDesat(float v) { g_highlight.desaturateWorld = PlayerMatte::Clamp(v, 0.0f, 1.0f); }
void RenderPlayerMatte_SetHighlightOutline(float v) { g_highlight.outlineStrength = PlayerMatte::Clamp(v, 0.0f, 1.0f); }
void RenderPlayerMatte_SetHighlightTint(float r, float g, float b) {
	g_highlight.tint[0] = PlayerMatte::Clamp(r, 0.0f, 1.0f);
	g_highlight.tint[1] = PlayerMatte::Clamp(g, 0.0f, 1.0f);
	g_highlight.tint[2] = PlayerMatte::Clamp(b, 0.0f, 1.0f);
}

PlayerMatte::RenderSnapshot RenderPlayerMatte_Snapshot() {
	std::lock_guard<std::mutex> lock(g_snapMutex);
	return g_snapshot;
}

void RenderPlayerMatte_AppendStateJson(std::ostringstream& out) {
	out << ",\"playerMatte\":{"
		<< "\"controller\":" << g_selectedController
		<< ",\"name\":\"" << JsonBuilder::Escape(g_selectedName) << "\""
		<< ",\"team\":" << g_selectedTeam
		<< ",\"spectated\":" << g_spectatedController
		<< ",\"highlight\":" << (g_highlight.enabled ? "true" : "false")
		<< ",\"dimWorld\":" << g_highlight.dimWorld
		<< ",\"desatWorld\":" << g_highlight.desaturateWorld
		<< ",\"outline\":" << g_highlight.outlineStrength
		<< ",\"tint\":[" << g_highlight.tint[0] << "," << g_highlight.tint[1] << "," << g_highlight.tint[2] << "]"
		<< ",\"players\":[";
	for (size_t i = 0; i < g_roster.size(); ++i) {
		const RosterPlayer& p = g_roster[i];
		if (i) out << ",";
		out << "{\"controller\":" << p.controller
			<< ",\"pawn\":" << p.pawn
			<< ",\"alive\":" << (p.alive ? "true" : "false")
			<< ",\"team\":" << p.team
			<< ",\"name\":\"" << JsonBuilder::Escape(p.name) << "\"}";
	}
	out << "]}";
}

void RenderPlayerMatte_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Message(
			"%s render player pick <controller> | spectated | clear\n"
			"%s render player highlight on|off|toggle | dim <0..1> | desat <0..1> | outline <0..1> | tint <r> <g> <b>\n"
			"%s render player roster | status\n", cmd, cmd, cmd);
		return;
	}
	const char* sub = args->ArgV(3);
	if (0 == _stricmp(sub, "pick") && argc >= 5) {
		const int ctrl = std::atoi(args->ArgV(4));
		if (!RenderPlayerMatte_PickController(ctrl))
			advancedfx::Warning("%s render player: controller %d not in roster.\n", cmd, ctrl);
		return;
	}
	if (0 == _stricmp(sub, "spectated")) {
		if (!RenderPlayerMatte_PickSpectated())
			advancedfx::Warning("%s render player: no spectated player right now.\n", cmd);
		return;
	}
	if (0 == _stricmp(sub, "clear") || 0 == _stricmp(sub, "off")) {
		RenderPlayerMatte_Clear();
		return;
	}
	if (0 == _stricmp(sub, "highlight") && argc >= 5) {
		const char* a = args->ArgV(4);
		if (0 == _stricmp(a, "on") || 0 == std::strcmp(a, "1")) RenderPlayerMatte_SetHighlightEnabled(true);
		else if (0 == _stricmp(a, "off") || 0 == std::strcmp(a, "0")) RenderPlayerMatte_SetHighlightEnabled(false);
		else if (0 == _stricmp(a, "toggle")) RenderPlayerMatte_SetHighlightEnabled(!RenderPlayerMatte_HighlightEnabled());
		else if (0 == _stricmp(a, "dim") && argc >= 6) RenderPlayerMatte_SetHighlightDim((float)std::atof(args->ArgV(5)));
		else if (0 == _stricmp(a, "desat") && argc >= 6) RenderPlayerMatte_SetHighlightDesat((float)std::atof(args->ArgV(5)));
		else if (0 == _stricmp(a, "outline") && argc >= 6) RenderPlayerMatte_SetHighlightOutline((float)std::atof(args->ArgV(5)));
		else if (0 == _stricmp(a, "tint") && argc >= 8) {
			RenderPlayerMatte_SetHighlightTint(
				(float)std::atof(args->ArgV(5)),
				(float)std::atof(args->ArgV(6)),
				(float)std::atof(args->ArgV(7)));
		}
		return;
	}
	if (0 == _stricmp(sub, "roster")) {
		RefreshRoster();
		for (const RosterPlayer& p : g_roster) {
			advancedfx::Message("  ctrl=%d pawn=%d team=%s alive=%d name=\"%s\"\n",
				p.controller, p.pawn, p.team == 2 ? "T" : (p.team == 3 ? "CT" : "?"),
				p.alive ? 1 : 0, p.name.c_str());
		}
		return;
	}
	if (0 == _stricmp(sub, "status")) {
		advancedfx::Message("playerMatte: controller=%d name=\"%s\" highlight=%d dim=%.2f desat=%.2f outline=%.2f\n",
			g_selectedController, g_selectedName.c_str(), g_highlight.enabled ? 1 : 0,
			g_highlight.dimWorld, g_highlight.desaturateWorld, g_highlight.outlineStrength);
		return;
	}
	advancedfx::Warning("%s render player: unknown subcommand '%s'.\n", cmd, sub);
}

} // namespace Filmmaker
