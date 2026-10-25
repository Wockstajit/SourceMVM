#include "CustomizeTarget.h"

#include "CustomizeTargetMath.h"
#include "CosmeticOverrideSystem.h" // CosmeticsRef().InDemoContext()
#include "CosmeticUiQueries.h"      // BuildCustomizeTargetJson, CustomizeJsonEscape

#include "../Filmmaker.h"           // CameraTimeline_Native*: CS2's own demo bar state
#include "../Movie/CameraBridge.h"
#include "../Movie/MovieMode.h"
#include "../Debug/MvmDebugLog.h"
#include "../../MirvTime.h"         // g_MirvTime.GetCurrentDemoTick: a seek ends every saved life

#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"

#include "../../../Shared/AfxConsole.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace Filmmaker {

namespace {

namespace TM = CustomizeTargetMath;

struct Player {
	int controller = -1;
	int pawn = -1;      // the living player pawn, or the body once dead; -1 when no body is found
	bool alive = false;
	int team = 0;
	int slot = -1;
	uint64_t steam = 0;
	std::string name;
	std::string key;    // steam:<id>, or ctrl:<controller> for a bot
};

TM::TargetState g_state;
TM::Target g_target;
int g_spectated = -1;
std::vector<Player> g_players;
// Player key -> loadout JSON. g_loadouts is what the modal shows; g_lastAlive keeps the last read
// taken while the player was alive, because death drops their weapons.
std::map<std::string, std::string> g_loadouts;
std::map<std::string, std::string> g_lastAlive;
int g_loadoutAge = 0;
const char* const kEmptyJson = "{\"target\":-1,\"source\":\"none\",\"spectated\":-1,\"players\":[]}";
std::string g_json = kEmptyJson;
// A dead pick pins the free cam on the body's eye for a few frames: the pushed pose is consumed once
// per frame and the free cam adopts it (CameraPath::HoldEditorAtMarker's pattern).
int g_poseFrames = 0;
double g_pose[7] = {};
// When g_lastAlive may save. A seek back lands before the lives those copies came from, so they go.
// Right after any jump the pawns and their weapons are still arriving, so nothing is saved until
// they settle. A forward seek keeps the copies: they are the last loadout seen alive.
int g_lastTick = -1;
int g_seekSettleFrames = 0;
constexpr int kSeekTicks = 64;         // the jump CosmeticDemoSync also treats as a seek
constexpr int kSeekSettleFrames = 32;  // pawns and their weapons arrive over a few frames

constexpr int kLoadoutFramesActive = 8;  // ~8 reads a second while CONFIG or the editor is open
constexpr int kLoadoutFramesIdle = 32;   // still caught before a death, so a dead player keeps theirs

CEntityInstance* Ent(int index) {
	if (index < 0 || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex || index > GetHighestEntityIndex())
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

int HandleIndexAt(CEntityInstance* ent, ptrdiff_t offset) {
	if (!ent || offset == 0) return -1;
	unsigned int raw = 0;
	__try { raw = *(unsigned int*)((unsigned char*)ent + offset); } __except (1) { return -1; }
	const SOURCESDK::CS2::CBaseHandle h = SOURCESDK::CS2::CEntityHandle::CEntityHandle(raw);
	return h.IsValid() ? h.GetEntryIndex() : -1;
}

// A player pawn that is a body, not the observer pawn a dead player watches through.
bool IsBodyPawn(CEntityInstance* ent) {
	if (!ent) return false;
	bool pawn = false;
	const char* cls = nullptr;
	__try { pawn = ent->IsPlayerPawn(); cls = ent->GetClientClassName(); } __except (1) { return false; }
	return pawn && !(cls && std::strstr(cls, "Observer"));
}

bool IsAlivePawn(CEntityInstance* ent) {
	if (!IsBodyPawn(ent)) return false;
	unsigned int health = 0;
	__try { health = ent->GetHealth(); } __except (1) { return false; }
	return health > 0;
}

// The body a controller's player lives in or left behind. m_hPlayerPawn keeps naming it after the
// controller's m_hPawn has moved to an observer pawn; a body whose m_hOriginalController is this
// controller is the fallback (both resolve on the 2026-10 build; either may be renamed later).
int BodyPawn(int controller) {
	CEntityInstance* ctrl = Ent(controller);
	const int viaController = HandleIndexAt(ctrl, g_clientDllOffsets.CCSPlayerController.m_hPlayerPawn);
	if (viaController >= 0 && IsBodyPawn(Ent(viaController))) return viaController;
	const ptrdiff_t original = g_clientDllOffsets.C_CSPlayerPawn.m_hOriginalController;
	if (original == 0) return -1;
	for (int i = 0, n = GetHighestEntityIndex(); i <= n; ++i) {
		CEntityInstance* e = Ent(i);
		if (IsBodyPawn(e) && HandleIndexAt(e, original) == controller) return i;
	}
	return -1;
}

// Guarded POD reads (no destructible locals may share a function with __try).
bool IsControllerEnt(CEntityInstance* e) {
	__try { return e && e->IsPlayerController(); } __except (1) { return false; }
}

int ControllerTeam(CEntityInstance* e) {
	__try { return e ? e->GetTeam() : 0; } __except (1) { return 0; }
}

const char* ControllerName(CEntityInstance* e, uint64_t* steam) {
	*steam = 0;
	__try {
		const char* name = e->GetSanitizedPlayerName();
		if (!name || !*name) name = e->GetPlayerName();
		*steam = e->GetSteamId();
		return name;
	} __except (1) { return nullptr; }
}

void ReadController(CEntityInstance* ctrl, Player& p) {
	const char* name = ControllerName(ctrl, &p.steam);
	p.name = (name && *name) ? name : ("Player " + std::to_string(p.controller));
	p.key = p.steam ? ("steam:" + std::to_string(p.steam)) : ("ctrl:" + std::to_string(p.controller));
}

std::string Lower(const std::string& s) {
	std::string out(s);
	for (char& c : out) c = (char)std::tolower((unsigned char)c);
	return out;
}

// Every T and CT player, from CS2's own demo bar. The entity scan is the fallback for a demo whose
// bar does not answer: every T/CT controller, alive by its body's health.
void BuildRoster() {
	g_players.clear();
	const std::vector<TM::DemoPlayer> rows = TM::ParseDemoPlayers(CameraTimeline_NativeDemoPlayers());
	if (!rows.empty()) {
		for (const TM::DemoPlayer& row : rows) {
			CEntityInstance* ctrl = Ent(row.controller);
			if (!IsControllerEnt(ctrl)) continue;
			Player p;
			p.controller = row.controller; p.alive = row.alive; p.team = row.team; p.slot = row.slot;
			p.pawn = row.alive ? row.pawn : BodyPawn(row.controller);
			ReadController(ctrl, p);
			g_players.push_back(p);
		}
	} else {
		for (int i = 0, n = GetHighestEntityIndex(); i <= n; ++i) {
			CEntityInstance* ctrl = Ent(i);
			if (!IsControllerEnt(ctrl)) continue;
			const int team = ControllerTeam(ctrl);
			if (team != 2 && team != 3) continue;
			Player p;
			p.controller = i; p.team = team;
			p.pawn = BodyPawn(i);
			p.alive = IsAlivePawn(Ent(p.pawn));
			ReadController(ctrl, p);
			g_players.push_back(p);
		}
	}
	std::stable_sort(g_players.begin(), g_players.end(), [](const Player& a, const Player& b) {
		if (a.team != b.team) return a.team < b.team;
		return Lower(a.name) < Lower(b.name);
	});
}

const Player* Find(int controller) {
	for (const Player& p : g_players)
		if (p.controller == controller) return &p;
	return nullptr;
}

// The controller the view looks through, or -1. Our free cam is nobody's eyes, whatever CS2's own
// observer state still says underneath it.
int SpectatedController(int* eyePawnOut) {
	*eyePawnOut = -1;
	if (CameraBridge_GetFreeCamEnabled()) return -1;
	if (!CameraTimeline_NativeDemoPlayers().empty()) {
		const int pawn = CameraTimeline_NativeSpectatedPawn();
		for (const Player& p : g_players)
			if (p.alive && pawn >= 0 && p.pawn == pawn) return p.controller;
		return -1;
	}
	// No demo bar: the render-eye match, and only onto a LIVING roster player. It also lands on dead
	// bodies and on observer pawns sitting at the spectated eye, which must never become the target.
	const int eye = AfxGetSpectatedPawnIndexHeld();
	*eyePawnOut = eye;
	for (const Player& p : g_players)
		if (p.alive && eye >= 0 && p.pawn == eye) return p.controller;
	return -1;
}

void RefreshLoadouts() {
	g_loadouts.clear();
	for (const Player& p : g_players) {
		std::string json = "null";
		// Health as well as CS2's alive flag: the flag can trail the death by a frame, and a read in
		// that frame has the guns already dropped -- it would replace the last real loadout.
		if (p.alive && p.pawn >= 0 && IsAlivePawn(Ent(p.pawn))) {
			json = BuildCustomizeTargetJson(p.pawn);
			// A copy needs a gun. In the demo's first frames the pawns have health but only a default
			// knife, not always their own team's, and a seek past their death made that the loadout
			// every dead player showed.
			const bool hasGun = json.find("\"primary\":{") != std::string::npos
				|| json.find("\"secondary\":{") != std::string::npos;
			if (hasGun && g_seekSettleFrames == 0) g_lastAlive[p.key] = json;
		} else {
			auto it = g_lastAlive.find(p.key);
			if (it != g_lastAlive.end()) json = it->second;
			else if (p.pawn >= 0) json = BuildCustomizeTargetJson(p.pawn); // the body: gloves and agent
		}
		g_loadouts[p.key] = json;
	}
}

void BuildJson() {
	std::ostringstream o;
	o << "{\"target\":" << g_target.controller
	  << ",\"source\":\"" << TM::SourceName(g_target.source) << "\""
	  << ",\"spectated\":" << g_spectated
	  << ",\"players\":[";
	bool first = true;
	for (const Player& p : g_players) {
		if (!first) o << ',';
		first = false;
		auto it = g_loadouts.find(p.key);
		const bool lastSeen = !p.alive && g_lastAlive.count(p.key) != 0;
		o << "{\"controller\":" << p.controller
		  << ",\"pawn\":" << p.pawn
		  << ",\"alive\":" << (p.alive ? "true" : "false")
		  << ",\"team\":" << p.team
		  << ",\"slot\":" << p.slot
		  << ",\"name\":\"" << CustomizeJsonEscape(p.name.c_str()) << "\""
		  << ",\"key\":\"" << p.key << "\""
		  << ",\"steamId\":\"" << (p.steam ? std::to_string(p.steam) : std::string()) << "\""
		  << ",\"lastSeen\":" << (lastSeen ? "true" : "false")
		  << ",\"loadout\":" << (it != g_loadouts.end() ? it->second : std::string("null"))
		  << '}';
	}
	o << "]}";
	g_json = o.str();
}

// A dead player's last view: the body's render eye, which keeps the eye position and angles it died
// with. Read through the pawn's virtuals, so guarded.
bool ReadEye(int pawn, double pose[7]) {
	CEntityInstance* e = Ent(pawn);
	if (!IsBodyPawn(e)) return false;
	float o[3] = {}, a[3] = {};
	__try { e->GetRenderEyeOrigin(o); e->GetRenderEyeAngles(a); } __except (1) { return false; }
	if (!(o[0] == o[0]) || (o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f)) return false;
	const float fov = CameraBridge_GetGameCameraFov();
	pose[0] = o[0]; pose[1] = o[1]; pose[2] = o[2];
	pose[3] = a[0]; pose[4] = a[1]; pose[5] = 0.0;
	pose[6] = (fov > 1.0f && fov < 179.0f) ? fov : 90.0;
	return true;
}

void ResetAll() {
	TM::ClearPick(g_state);
	g_state.last = -1;
	g_target = TM::Target();
	g_spectated = -1;
	g_players.clear();
	g_loadouts.clear();
	g_lastAlive.clear();
	g_loadoutAge = 0;
	g_poseFrames = 0;
	g_lastTick = -1;
	g_seekSettleFrames = 0;
	g_json = kEmptyJson;
}

void TrackSeeks() {
	int tick = -1;
	if (!g_MirvTime.GetCurrentDemoTick(tick)) return;
	if (g_lastTick >= 0 && std::abs(tick - g_lastTick) > kSeekTicks) {
		const bool back = tick < g_lastTick;
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("customize.seek", "tick %d -> %d: %s %d saved last-alive loadouts", g_lastTick, tick,
				back ? "dropped" : "kept", (int)g_lastAlive.size());
		if (back) g_lastAlive.clear();
		g_seekSettleFrames = kSeekSettleFrames;
	} else if (g_seekSettleFrames > 0) {
		--g_seekSettleFrames;
	}
	g_lastTick = tick;
}

} // namespace

void CustomizeTarget_RunFrame(bool loadouts) {
	if (!CosmeticsRef().InDemoContext()) {
		if (!g_players.empty() || g_state.pick >= 0 || g_state.last >= 0) ResetAll();
		return;
	}
	if (g_poseFrames > 0) {
		--g_poseFrames;
		CameraBridge_SetCameraPose(g_pose[0], g_pose[1], g_pose[2], g_pose[3], g_pose[4], g_pose[5], g_pose[6]);
	}

	TrackSeeks();
	BuildRoster();
	int eyePawn = -1;
	g_spectated = SpectatedController(&eyePawn);
	const TM::Target prev = g_target;
	g_target = TM::Resolve(g_state, g_spectated, [](int c) { return Find(c) != nullptr; });

	const bool changed = prev.controller != g_target.controller || prev.source != g_target.source;
	if (changed || --g_loadoutAge <= 0) {
		RefreshLoadouts();
		g_loadoutAge = loadouts ? kLoadoutFramesActive : kLoadoutFramesIdle;
	}
	if (changed && MvmDebugLog_Active()) {
		const Player* t = Find(g_target.controller);
		MvmDebugLog_Linef("customize.target",
			"controller=%d (was %d) source=%s name='%s' alive=%d pawn=%d spectated=%d nativePawn=%d nativeMode=%d eyePawn=%d freecam=%d players=%d",
			g_target.controller, prev.controller, TM::SourceName(g_target.source), t ? t->name.c_str() : "",
			t && t->alive ? 1 : 0, t ? t->pawn : -1, g_spectated, CameraTimeline_NativeSpectatedPawn(),
			CameraTimeline_NativeObserverMode(), eyePawn, CameraBridge_GetFreeCamEnabled() ? 1 : 0, (int)g_players.size());
	}
	BuildJson();
}

void CustomizeTarget_LevelInit() { ResetAll(); }

const std::string& CustomizeTarget_StateJson() { return g_json; }

bool CustomizeTarget_Available() { return !g_players.empty(); }

bool CustomizeTarget_Pick(int controller) {
	if (!CosmeticsRef().InDemoContext()) return false;
	BuildRoster();
	const Player* p = Find(controller);
	if (!p) {
		MvmDebugLog_Linef("customize.pick", "REFUSED controller=%d (not a T/CT player of this demo, %d players)",
			controller, (int)g_players.size());
		return false;
	}
	TM::Pick(g_state, controller, p->alive);
	bool view = false;
	if (p->alive) {
		// CS2's own first person on them: our free cam off and in-eye (MovieMode), then the native
		// spectate the player list uses (CameraTimelineHud runs it on its next frame).
		g_poseFrames = 0;
		MovieModeRef().SetMode(MovieMode::Mode::Default);
		CameraTimeline_RequestNativeSpectate(controller);
		view = true;
	} else if (p->pawn >= 0 && ReadEye(p->pawn, g_pose)) {
		CameraBridge_SetFreeCamEnabled(true);
		g_poseFrames = 8;
		view = true;
	}
	g_loadoutAge = 0; // the modal reseeds from this player's loadout on the next frame
	MvmDebugLog_Linef("customize.pick", "controller=%d name='%s' alive=%d pawn=%d view=%s eye=%.1f %.1f %.1f ang=%.1f %.1f",
		controller, p->name.c_str(), p->alive ? 1 : 0, p->pawn,
		!view ? "unchanged (no body found)" : (p->alive ? "first person (CS2 spectate)" : "first person (free cam on the body)"),
		p->alive ? 0.0 : g_pose[0], p->alive ? 0.0 : g_pose[1], p->alive ? 0.0 : g_pose[2],
		p->alive ? 0.0 : g_pose[3], p->alive ? 0.0 : g_pose[4]);
	return true;
}

void CustomizeTarget_RunPickCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* a = (argc >= 4) ? args->ArgV(3) : "";
	if (0 == _stricmp(a, "clear")) {
		TM::ClearPick(g_state);
		advancedfx::Message("%s cosmetics pick: cleared; Customize follows the spectated player again.\n", cmd);
		return;
	}
	char* end = nullptr;
	const long controller = std::strtol(a, &end, 10);
	if (!*a || !end || *end) {
		advancedfx::Warning("usage: %s cosmetics pick <controller index|clear>   (see: %s cosmetics roster)\n", cmd, cmd);
		return;
	}
	if (!CustomizeTarget_Pick((int)controller))
		advancedfx::Warning("%s cosmetics pick: %ld is not a T or CT player of this demo.\n", cmd, controller);
	else
		advancedfx::Message("%s cosmetics pick: controller %ld.\n", cmd, controller);
}

void CustomizeTarget_PrintRoster(const char* cmd) {
	advancedfx::Message("%s cosmetics roster: target=%d source=%s spectated=%d (native pawn %d, observer mode %d, free cam %s)\n",
		cmd, g_target.controller, TM::SourceName(g_target.source), g_spectated,
		CameraTimeline_NativeSpectatedPawn(), CameraTimeline_NativeObserverMode(),
		CameraBridge_GetFreeCamEnabled() ? "on" : "off");
	for (const Player& p : g_players) {
		advancedfx::Message("  controller %d  %s  %-5s  pawn %d  slot %d  %s%s  %s\n",
			p.controller, p.team == 2 ? "T " : "CT", p.alive ? "alive" : "dead", p.pawn, p.slot, p.key.c_str(),
			(!p.alive && g_lastAlive.count(p.key)) ? " (last seen loadout)" : "", p.name.c_str());
	}
}

} // namespace Filmmaker
