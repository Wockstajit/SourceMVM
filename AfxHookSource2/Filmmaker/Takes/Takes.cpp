#define NOMINMAX
#include "Takes.h"

#include "TakesHud.h"
#include "TakesMath.h"
#include "TakesServer.h"

#include "../Debug/MvmDebugLog.h"
#include "../Platform/JsonBuilder.h"
#include "../Props/ClientEntitySpawn.h"
#include "../Props/Props.h"
#include "../Config/PracticeSessionFlag.h"
#include "../../WrpConsole.h"
#include "../../hlaeFolder.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient *g_pEngineToClient;

namespace Filmmaker::Takes {
namespace {

enum class Phase {
	Idle,       // nothing plays; bots, if any, stand still
	WaitBots,   // bot_add was sent; waiting for them to join
	WaitSpawn,  // bringing back whoever is dead; the match may also still be leaving warmup
	Running,    // the scene clock runs
	Finished,   // one pass played and looping is off; bots stand where their takes ended
};

const char *PhaseName(Phase p) {
	switch (p) {
	case Phase::Idle: return "idle";
	case Phase::WaitBots: return "adding bots";
	case Phase::WaitSpawn: return "spawning";
	case Phase::Running: return "playing";
	case Phase::Finished: return "finished";
	}
	return "?";
}

// Whether a bot is held to the recorded path, on top of replaying the input.
enum class FollowMode { Auto, On, Off };

struct Actor {
	int track = -1;
	uint32_t controllerHandle = 0xffffffffu;
	void *pawn = nullptr; // refreshed every frame; compared, never dereferenced from the hooks
	bool started = false;
	size_t nextInventory = 0;
	float lastPitch = 0, lastYaw = 0;
	std::vector<std::string> pendingGive;
	std::vector<uint32_t> heldWeapons; // every gun this bot has held this pass; what it dropped is cleared before the next one
	std::string spawnModel;            // what it wore when it joined, for when a chosen model is taken off again
	// How close the performance stays to the recording. Reset each pass; `takes status` prints them.
	float maxDrift = 0;
	float lastDrift = 0;
	int corrections = 0;
	int fed = 0;
	int deaths = 0;
	bool wasAlive = false;
};

struct RecordingState {
	bool active = false;
	bool armed = false; // starts with the scene, once the player and the bots are up
	Track track;
	std::vector<std::string> lastWeapons;
	void *pawn = nullptr;
	int startTick = 0;
	std::vector<uint8_t> scratch;
};

struct State {
	std::recursive_mutex mutex;
	Scene scene;
	Phase phase = Phase::Idle;
	bool loop = false;
	FollowMode follow = FollowMode::Auto;
	bool noSpread = false;     // weapon_accuracy_nospread
	int recoilRandom = 100;    // percent of each weapon's shipped recoil variance
	bool recoilApplied = false;
	bool settingsLoaded = false;
	int housekeepingTick = -1000000;
	bool setupDone = false;
	bool trace = false;         // every bot command to the debug log
	float driftLimit = 0.0f;   // units; off by default, a snap is visible and an unforced bot stays within 0.1 of its take
	int tailTicks = 64;        // the scene holds its last moment this long before looping
	std::vector<Actor> actors;
	RecordingState rec;
	int sceneStartTick = 0;
	int phaseStartTick = 0;
	int lastRespawnTick = -1000000;
	int lastJoinTick = -1000000;
	int passes = 0;
	int botsRequested = 0;
	std::string message;
	std::string demoName;
	bool demoRecording = false;
	bool demoStopAtEnd = false;
	bool demoPending = false;  // starts with the next pass
	std::string lastMap;
} g;

std::filesystem::path SettingsFile() {
	return std::filesystem::path(GetHlaeRoamingAppDataFolderW()) / L"source_mvm" / L"takes" / L"settings.txt";
}

const char *FollowName(FollowMode m) { return m == FollowMode::On ? "on" : m == FollowMode::Off ? "off" : "auto"; }

void SaveSettings() {
	if (PracticeSession_Active()) return;
	std::error_code ec;
	std::filesystem::create_directories(SettingsFile().parent_path(), ec);
	std::ofstream out(SettingsFile(), std::ios::trunc);
	out << "loop " << (g.loop ? 1 : 0) << "\nfollow " << FollowName(g.follow)
	    << "\nnospread " << (g.noSpread ? 1 : 0) << "\nrecoilrandom " << g.recoilRandom << "\n";
}

void LoadSettings() {
	if (g.settingsLoaded) return;
	g.settingsLoaded = true;
	std::ifstream in(SettingsFile());
	std::string key, value;
	while (in >> key >> value) {
		if (key == "loop") g.loop = value != "0";
		else if (key == "follow") g.follow = value == "on" ? FollowMode::On : value == "off" ? FollowMode::Off : FollowMode::Auto;
		else if (key == "nospread") g.noSpread = value != "0";
		else if (key == "recoilrandom") g.recoilRandom = (std::max)(0, (std::min)(100, atoi(value.c_str())));
	}
}

void Exec(const char *cmd) {
	if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
}

void Execf(const char *fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Exec(buf);
}

bool PlayingDemo() {
	if (g_pEngineToClient) {
		if (auto *demo = g_pEngineToClient->GetDemoFile()) return demo->IsPlayingDemo() || demo->IsDemoPaused();
	}
	return false;
}

// The map a match is on. The main menu is a real level with a local server of its own, called <empty>.
std::string CurrentMap() {
	const char *name = g_pEngineToClient ? g_pEngineToClient->GetLevelNameShort() : nullptr;
	if (!name || 0 == strcmp(name, "<empty>")) return "";
	return name;
}

void Say(const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g.message = buf;
	advancedfx::Message("takes: %s\n", buf);
	MvmDebugLog_Linef("takes", "%s", buf);
}

// The rules a recorded scene needs: rounds that never end by themselves, nobody kicked, money for
// whatever the player buys, and damage from everyone so any bot can be the one who dies. Warmup ending
// is the only restart this feature ever causes, and only when the match is still in warmup: a warmup
// brings the dead back by itself, which would undo every kill.
void ApplySetup() {
	if (g.setupDone) return;
	g.setupDone = true;
	static const char *const kSetup[] = {
		"mirv_cvar_unlock_sv_cheats",
		"sv_cheats 1",
		"mp_warmup_end",
		"mp_ignore_round_win_conditions 1",
		"mp_roundtime 60",
		"mp_roundtime_defuse 60",
		"mp_roundtime_hostage 60",
		"mp_freezetime 0",
		"mp_team_intro_time 0",
		"mp_round_restart_delay 0",
		"mp_join_grace_time 30", // its maximum; Server::Respawn relies on it
		"mp_maxrounds 999", // 0 counts every round as the half: the restart that ends warmup would swap the teams
		"mp_timelimit 0",
		"mp_halftime 0",
		"mp_match_can_clinch 0",
		"mp_autokick 0",
		"mp_tkpunish 0",
		"mp_autoteambalance 0",
		"mp_limitteams 0",
		"mp_friendlyfire 1",
		"mp_solid_teammates 0", // a performing bot must not be stopped by the player or another take
		"mp_give_player_c4 0",
		"mp_buy_anywhere 1",
		"mp_buytime 99999",
		"mp_startmoney 65535",
		"mp_maxmoney 65535",
		"mp_afterroundmoney 65535",
		"ammo_grenade_limit_total 6",
		"sv_hibernate_when_empty 0",
		"bot_quota_mode normal",
		"bot_quota 0",
		"bot_join_after_player 0",
		"bot_auto_vacate 0",
		// What a recorded demo needs: nothing held back, and every tick written.
		"tv_delay 0",
		"tv_delaymapchange 0",
		"tv_snapshotrate 64",
		"tv_autorecord 0",
		"tv_advertise_watchable 0",
	};
	for (const char *cmd : kSetup) Exec(cmd);
	Execf("weapon_accuracy_nospread %d", g.noSpread ? 1 : 0);
}

Actor *ActorByPawn(void *pawn) {
	for (Actor &a : g.actors)
		if (a.pawn == pawn) return &a;
	return nullptr;
}

// -- Recording ----------------------------------------------------------------------------------------

void WeaponClasses(void *pawn, std::vector<std::string> &out) {
	std::vector<void *> weapons;
	Server::ListWeapons(pawn, weapons);
	out.clear();
	for (void *w : weapons) out.emplace_back(Server::ClassNameOf(w));
	std::sort(out.begin(), out.end());
}

void BeginRecordingNow(void *hostPawn) {
	RecordingState &r = g.rec;
	r.armed = false;
	r.active = true;
	r.pawn = hostPawn;
	r.startTick = Server::TickCount();
	r.track.team = uint8_t(Server::Team(hostPawn));
	r.track.health = Server::Health(hostPawn);
	r.track.armor = Server::Armor(hostPawn);
	r.track.helmet = Server::HasHelmet(hostPawn) ? 1 : 0;
	WeaponClasses(hostPawn, r.lastWeapons);
	r.track.startWeapons.clear();
	for (const std::string &w : r.lastWeapons) r.track.startWeapons.push_back(InternName(r.track.names, w));
	Say("recording %s", r.track.name.c_str());
}

// The server just ran one of the player's commands.
void RecordCommand(void *pawn, void *cmd) {
	RecordingState &r = g.rec;
	if (!Server::SerializeCmd(cmd, r.scratch)) return;
	Track &t = r.track;
	TickSample s;
	Server::Pose(pawn, s.pos, s.vel);
	const CmdSummary summary = SummarizeCmd(r.scratch.data(), r.scratch.size());
	s.pitch = summary.pitch;
	s.yaw = summary.yaw;
	s.flags = kTickAlive;
	if (void *active = Server::ActiveWeapon(pawn)) s.activeWeapon = InternName(t.names, Server::ClassNameOf(active));
	s.cmdOffset = uint32_t(t.cmds.size());
	s.cmdSize = uint32_t(r.scratch.size());
	t.cmds.insert(t.cmds.end(), r.scratch.begin(), r.scratch.end());
	const uint32_t tick = uint32_t(t.ticks.size());
	if (tick > 0 && MovedWithoutInput(t.ticks.back().pos, t.ticks.back().vel, s.pos, s.vel, g.scene.tickRate)) s.flags |= kTickJumped;
	t.ticks.push_back(s);
	// What the player picked up, bought, threw or dropped since the last command.
	std::vector<std::string> now;
	WeaponClasses(pawn, now);
	if (now != r.lastWeapons) {
		for (const std::string &w : now) {
			const auto was = std::count(r.lastWeapons.begin(), r.lastWeapons.end(), w);
			const auto is = std::count(now.begin(), now.end(), w);
			for (auto i = was; i < is; ++i) t.inventory.push_back({tick, InternName(t.names, w), 1});
		}
		for (const std::string &w : r.lastWeapons) {
			const auto was = std::count(r.lastWeapons.begin(), r.lastWeapons.end(), w);
			const auto is = std::count(now.begin(), now.end(), w);
			if (was > is && (&w == &r.lastWeapons.front() || *(&w - 1) != w))
				for (auto i = is; i < was; ++i) t.inventory.push_back({tick, InternName(t.names, w), 0});
		}
		r.lastWeapons = std::move(now);
	}
}

// -- Saving --------------------------------------------------------------------------------------------

std::filesystem::path SceneFolder(const std::string &map) {
	return std::filesystem::path(GetHlaeRoamingAppDataFolderW()) / L"source_mvm" / L"takes" /
	       std::filesystem::u8path(SafeFileName(map));
}

bool SaveScene(const std::string &name) {
	const std::string map = g.scene.map.empty() ? CurrentMap() : g.scene.map;
	std::error_code ec;
	const std::filesystem::path folder = SceneFolder(map);
	std::filesystem::create_directories(folder, ec);
	const std::filesystem::path file = folder / std::filesystem::u8path(SafeFileName(name) + ".mvmtakes");
	const std::vector<uint8_t> bytes = SerializeScene(g.scene);
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	if (!out) return false;
	out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
	return bool(out);
}

bool LoadScene(const std::string &name, Scene &scene) {
	const std::filesystem::path file = SceneFolder(CurrentMap()) / std::filesystem::u8path(SafeFileName(name) + ".mvmtakes");
	std::ifstream in(file, std::ios::binary);
	if (!in) return false;
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return ParseScene(bytes.data(), bytes.size(), scene);
}

void Autosave() {
	if (!g.scene.tracks.empty() || !g.scene.objects.empty()) {
		SaveScene("_last");
		return;
	}
	// An emptied scene: the autosave goes too, or the next map load brings back what was just deleted.
	// Only once this map's autosave was read (EnsureSceneForMap); before that an empty scene means
	// "not loaded yet", and the file is the user's work.
	const std::string map = CurrentMap();
	if (map.empty() || map != g.lastMap) return;
	std::error_code ec;
	std::filesystem::remove(SceneFolder(map) / std::filesystem::u8path(SafeFileName("_last") + ".mvmtakes"), ec);
}

// -- Placed objects ------------------------------------------------------------------------------------

// Everything this feature puts in the world through ent_create.
struct WorldSpawn {
	std::string key;   // stable: the scene object's name, or solid_<prop id>
	std::string model;
	float pos[3] = {0, 0, 0};
	float ang[3] = {0, 0, 0};
	float scale = 1.0f;
	bool solid = true;
	bool invisible = false; // a collider under a client-side prop, which is what gets drawn
	std::string Signature() const {
		char text[160];
		snprintf(text, sizeof(text), "%.2f %.2f %.2f %.2f %.2f %.2f %.3f %d %d", pos[0], pos[1], pos[2], ang[0], ang[1], ang[2], scale, solid ? 1 : 0, invisible ? 1 : 0);
		return model + " " + text;
	}
};

WorldSpawn FromObject(const SceneObject &o) {
	WorldSpawn w;
	w.key = o.name;
	w.model = o.model;
	std::memcpy(w.pos, o.pos, sizeof(w.pos));
	std::memcpy(w.ang, o.ang, sizeof(w.ang));
	w.scale = o.scale;
	w.solid = o.solid;
	return w;
}

// What each thing is called in the world right now. ent_remove finds its target when the deferred
// kill runs, so a new copy under the old name would die with the old one: every copy gets its own.
struct LiveWorldSpawn {
	std::string liveName;
	std::string signature;
};
std::map<std::string, LiveWorldSpawn> g_live;
int g_spawnGeneration = 0;
// A model's first spawn can land before its load does, and the entity silently never appears. So
// every spawn is checked a moment later and sent again if it is not there.
struct SpawnCheck {
	WorldSpawn spawn;
	int checkAtTick = 0;
	int tries = 0;
};
std::vector<SpawnCheck> g_spawnChecks;
constexpr int kSpawnCheckTicks = 24;
constexpr int kSpawnMaxTries = 8;

void RemoveLive(const std::string &key) {
	const auto it = g_live.find(key);
	if (it == g_live.end()) return;
	Execf("ent_remove %s", it->second.liveName.c_str());
	g_live.erase(it);
	g_spawnChecks.erase(std::remove_if(g_spawnChecks.begin(), g_spawnChecks.end(), [&](const SpawnCheck &c) { return c.spawn.key == key; }),
	                    g_spawnChecks.end());
}

void SendSpawn(const WorldSpawn &w) {
	// Resident first: the client draws the ERROR model for anything the level did not load, and the
	// server has no physics hull for it. Both modules share this one resource system.
	Props::PrecacheModel(w.model.c_str());
	const auto old = g_live.find(w.key);
	if (old != g_live.end()) Execf("ent_remove %s", old->second.liveName.c_str());
	const std::string live = "mvm_" + w.key + "_" + std::to_string(++g_spawnGeneration);
	g_live[w.key] = {live, w.Signature()};
	// ent_create takes its keyvalues the way a map would give them, so there is no spawner to find.
	// Solid 6 is SOLID_VPHYSICS: the model's own physics hull. Render mode is an enum taken by name.
	Execf("ent_create prop_dynamic { \"targetname\" \"%s\" \"model\" \"%s\" \"solid\" \"%d\" "
	      "\"origin\" \"%.3f %.3f %.3f\" \"angles\" \"%.3f %.3f %.3f\" \"modelscale\" \"%.3f\" \"rendermode\" \"%s\" }",
	      live.c_str(), w.model.c_str(), w.solid ? 6 : 0, w.pos[0], w.pos[1], w.pos[2], w.ang[0], w.ang[1], w.ang[2], w.scale, w.invisible ? "kRenderNone" : "kRenderNormal");
}

void ArmSpawnCheck(const WorldSpawn &w, int tries) {
	g_spawnChecks.erase(std::remove_if(g_spawnChecks.begin(), g_spawnChecks.end(), [&](const SpawnCheck &c) { return c.spawn.key == w.key; }),
	                    g_spawnChecks.end());
	g_spawnChecks.push_back({w, Server::TickCount() + kSpawnCheckTicks, tries});
}

void SpawnWorld(const WorldSpawn &w) {
	SendSpawn(w);
	ArmSpawnCheck(w, 0);
}

void PumpSpawnChecks() {
	if (g_spawnChecks.empty()) return;
	const int tick = Server::TickCount();
	for (size_t i = 0; i < g_spawnChecks.size();) {
		const SpawnCheck check = g_spawnChecks[i];
		if (tick < check.checkAtTick && tick >= check.checkAtTick - kSpawnCheckTicks * 2) {
			++i;
			continue;
		}
		g_spawnChecks.erase(g_spawnChecks.begin() + i);
		const auto live = g_live.find(check.spawn.key);
		if (live == g_live.end() || Server::FindByTargetName(live->second.liveName.c_str())) continue;
		if (check.tries + 1 >= kSpawnMaxTries) {
			Say("could not place %s: it did not load", check.spawn.model.c_str());
			continue;
		}
		MvmDebugLog_Linef("takes.object", "%s missing after try %d, spawning again", check.spawn.key.c_str(), check.tries + 1);
		SendSpawn(check.spawn);
		ArmSpawnCheck(check.spawn, check.tries + 1);
	}
}

// Anything of ours a round restart took away comes back, from warmup ending or one the player sent.
// Walks the entity list, so it is only called a few times a second, and only with something placed.
void RestoreMissingWorld() {
	if (g_live.empty()) return;
	for (const auto &entry : g_live) {
		bool pending = false;
		for (const SpawnCheck &c : g_spawnChecks) pending = pending || c.spawn.key == entry.first;
		if (pending || Server::FindByTargetName(entry.second.liveName.c_str())) continue;
		for (const SceneObject &o : g.scene.objects) {
			if (o.name == entry.first) {
				SpawnWorld(FromObject(o));
				return; // g_live changed under the loop
			}
		}
		// A solid prop's collider: dropping the record makes SyncSolidProps send it again.
		const std::string key = entry.first;
		g_live.erase(key);
		return;
	}
}

// The scene's objects that are not in the world yet, such as after loading a saved scene.
void SpawnMissingObjects() {
	for (const SceneObject &o : g.scene.objects)
		if (!g_live.count(o.name)) SpawnWorld(FromObject(o));
}

// Keeps one invisible collider under each solid placed prop, following its pose.
void SyncSolidProps() {
	std::vector<SolidProp> want;
	Props_CollectSolid(want);
	if (!want.empty()) {
		// ent_create is a cheat command; a solid prop can be switched on before any take exists.
		ApplySetup();
	}
	std::vector<std::string> keys;
	for (const SolidProp &prop : want) {
		WorldSpawn w;
		w.key = "solid_" + std::to_string(prop.id);
		w.model = prop.model;
		std::memcpy(w.pos, prop.pos, sizeof(w.pos));
		std::memcpy(w.ang, prop.ang, sizeof(w.ang));
		w.scale = prop.scale;
		w.invisible = true;
		keys.push_back(w.key);
		const auto live = g_live.find(w.key);
		if (live == g_live.end() || live->second.signature != w.Signature()) SpawnWorld(w);
	}
	std::vector<std::string> gone;
	for (const auto &entry : g_live) {
		if (entry.first.rfind("solid_", 0) == 0 && std::find(keys.begin(), keys.end(), entry.first) == keys.end()) gone.push_back(entry.first);
	}
	for (const std::string &key : gone) RemoveLive(key);
}

// Gun randomness: scaled on every weapon the player and the bots hold. The weapon data blocks are
// shared per class, so this only costs a walk over a few weapons now and then.
void ApplyRecoilRandomness() {
	if (g.recoilRandom != 100) g.recoilApplied = true;
	// Once anything was scaled this keeps running at 100% too: each class goes back as it is next held.
	if (!g.recoilApplied) return;
	const float scale = g.recoilRandom / 100.0f;
	auto apply = [&](void *pawn) {
		std::vector<void *> weapons;
		if (pawn) Server::ListWeapons(pawn, weapons);
		for (void *w : weapons) Server::SetRecoilRandomness(w, scale);
	};
	void *host = Server::HostController();
	apply(host ? Server::PawnOf(host) : nullptr);
	for (const Actor &a : g.actors) apply(a.pawn);
}

// -- Playing -------------------------------------------------------------------------------------------

void *ActorPawn(const Actor &a) {
	void *controller = Server::EntityFromHandle(a.controllerHandle);
	return controller ? Server::PawnOf(controller) : nullptr;
}

// Pairs each enabled take with a bot on its team. Returns how many takes still have none.
int AssignBots() {
	// Actors for takes that went away are dropped; their bots go back to the free pool.
	g.actors.erase(std::remove_if(g.actors.begin(), g.actors.end(), [](const Actor &a) {
		return a.track < 0 || a.track >= int(g.scene.tracks.size()) || !g.scene.tracks[a.track].enabled ||
		       !Server::EntityFromHandle(a.controllerHandle);
	}), g.actors.end());
	int missing = 0;
	const int maxClients = Server::MaxClients();
	for (int ti = 0; ti < int(g.scene.tracks.size()); ++ti) {
		const Track &t = g.scene.tracks[ti];
		if (!t.enabled) continue;
		bool assigned = false;
		for (const Actor &a : g.actors) assigned = assigned || a.track == ti;
		if (assigned) continue;
		void *found = nullptr;
		for (int slot = 0; slot < maxClients && !found; ++slot) {
			void *c = Server::ControllerBySlot(slot);
			if (!c || !Server::ControllerIsBot(c) || Server::Team(c) != t.team) continue;
			const uint32_t handle = Server::HandleOf(c);
			bool taken = false;
			for (const Actor &a : g.actors) taken = taken || a.controllerHandle == handle;
			if (!taken) found = c;
		}
		if (!found) {
			++missing;
			continue;
		}
		Actor a;
		a.track = ti;
		a.controllerHandle = Server::HandleOf(found);
		a.pawn = Server::PawnOf(found); // from here on the AI skips it
		g.actors.push_back(a);
		Server::SetPlayerName(found, t.name.c_str());
		MvmDebugLog_Linef("takes.assign", "track=%d name=%s controller=%p", ti, t.name.c_str(), found);
	}
	return missing;
}

int CountTeamBots(int team) {
	int n = 0;
	const int maxClients = Server::MaxClients();
	for (int slot = 0; slot < maxClients; ++slot) {
		void *c = Server::ControllerBySlot(slot);
		if (c && Server::ControllerIsBot(c) && Server::Team(c) == team) ++n;
	}
	return n;
}

void RequestMissingBots() {
	int wantT = 0, wantCt = 0;
	for (const Track &t : g.scene.tracks) {
		if (!t.enabled) continue;
		(t.team == 2 ? wantT : wantCt)++;
	}
	const int haveT = CountTeamBots(2), haveCt = CountTeamBots(3);
	// bot_add raises the quota by itself, so it starts from what is here now.
	Execf("bot_quota %d", haveT + haveCt);
	for (int i = haveT; i < wantT; ++i) Exec("bot_add_t");
	for (int i = haveCt; i < wantCt; ++i) Exec("bot_add_ct");
}

// Only performers stay: an AI bot left over from the match would run through the shot.
void KickExtraBots() {
	const int maxClients = Server::MaxClients();
	for (int slot = 0; slot < maxClients; ++slot) {
		void *c = Server::ControllerBySlot(slot);
		if (!c || !Server::ControllerIsBot(c) || Server::Team(c) < 2) continue; // SourceTV sits on no team
		const uint32_t handle = Server::HandleOf(c);
		bool performing = false;
		for (const Actor &a : g.actors) performing = performing || a.controllerHandle == handle;
		// Never bot_kick, even with a name: it kicks SourceTV along with the bot, and no demo can be
		// recorded after that. A client's slot is its user id.
		if (!performing) Execf("kickid %d", slot);
	}
	// Now exactly the performers: the quota neither adds to them nor picks one of them to kick.
	Execf("bot_quota %d", int(g.actors.size()));
}

// Gives what is not already held. DestroyWeapons leaves a knife, and a pawn cannot carry two: the
// spare would be dropped at its feet.
void GiveMissing(void *pawn, const std::vector<std::string> &weapons, const std::string &active) {
	for (const std::string &w : weapons) {
		if (Server::FindWeapon(pawn, w.c_str())) continue;
		void *item = Server::Give(pawn, w.c_str(), w == active);
		MvmDebugLog_Linef("takes.equip", "pawn=%p weapon=%s item=%p", pawn, w.c_str(), item);
	}
}

// What the take held on its first tick, with this bot's swaps.
void EquipForStart(const Track &t, void *pawn) {
	Server::DestroyWeapons(pawn);
	std::string active;
	if (!t.ticks.empty() && t.ticks[0].activeWeapon < t.names.size()) active = SwappedWeapon(t, t.names[t.ticks[0].activeWeapon]);
	std::vector<std::string> weapons;
	for (uint16_t n : t.startWeapons)
		if (n < t.names.size()) weapons.push_back(SwappedWeapon(t, t.names[n]));
	GiveMissing(pawn, weapons, active);
	if (t.armor > 0) Server::Give(pawn, t.helmet ? "item_assaultsuit" : "item_kevlar", false);
	Server::SetArmor(pawn, t.armor);
}

// Makes this bot look like a person to Customize in a demo: an id on the player, and on each gun.
void StampIdentity(const Track &t, const Actor &a, void *pawn) {
	const uint64_t id = PerformerSteamId(t.name);
	if (void *controller = Server::EntityFromHandle(a.controllerHandle)) Server::SetSteamId(controller, id);
	std::vector<void *> weapons;
	Server::ListWeapons(pawn, weapons);
	for (void *w : weapons) Server::SetOriginalOwner(w, id);
}

void ApplyAgentModel(const Track &t, const Actor &a, void *pawn) {
	const std::string &model = t.agentModel.empty() ? a.spawnModel : t.agentModel;
	if (model.empty() || model == Server::ModelOf(pawn)) return;
	Props::PrecacheModel(model.c_str());
	Server::SetModel(pawn, model.c_str());
}

void StartActor(Actor &a) {
	const Track &t = g.scene.tracks[a.track];
	void *pawn = ActorPawn(a);
	a.pawn = pawn;
	if (!pawn || t.ticks.empty()) return;
	// The game refused to bring it back. Handing a corpse weapons and a position would help nothing.
	if (!Server::Alive(pawn)) {
		Say("%s could not come back, so it sits this one out", t.name.c_str());
		return;
	}
	// Again each pass: a bot that was still joining when it was assigned took its profile's name back.
	if (void *controller = Server::EntityFromHandle(a.controllerHandle)) Server::SetPlayerName(controller, t.name.c_str());
	if (a.spawnModel.empty()) a.spawnModel = Server::ModelOf(pawn);
	const TickSample &first = t.ticks[0];
	const float angles[3] = {first.pitch, first.yaw, 0};
	// At the speed the take starts at. From a standstill, a take started on the move stays a whole
	// acceleration behind the recording for its entire length: about 47 units at a run.
	Server::Teleport(pawn, first.pos, angles, first.vel);
	Server::SetHealth(pawn, t.health > 0 ? t.health : 100);
	ApplyAgentModel(t, a, pawn);
	EquipForStart(t, pawn);
	StampIdentity(t, a, pawn);
	a.started = true;
	a.nextInventory = 0;
	a.lastPitch = first.pitch;
	a.lastYaw = first.yaw;
	a.pendingGive.clear();
	a.maxDrift = 0;
	a.lastDrift = 0;
	a.corrections = 0;
	a.fed = 0;
	a.wasAlive = true;
	MvmDebugLog_Linef("takes.start", "track=%s pawn=%p pos=(%.1f %.1f %.1f) ticks=%d", t.name.c_str(), pawn, first.pos[0], first.pos[1], first.pos[2], int(t.ticks.size()));
}

// What the last pass left in the world: guns the bots dropped, and grenades still in the air or burning.
// A gun a person picked up has an owner again, and stays theirs.
void ClearLeftovers() {
	for (Actor &a : g.actors) {
		for (const uint32_t handle : a.heldWeapons) {
			void *weapon = Server::EntityFromHandle(handle);
			if (weapon && !Server::OwnerOf(weapon)) Server::RemoveEntity(weapon);
		}
		a.heldWeapons.clear();
	}
	static const char *const kThrown[] = {"smokegrenade_projectile", "molotov_projectile", "hegrenade_projectile", "flashbang_projectile", "decoy_projectile", "inferno"};
	for (int i = 0; i < 16384; ++i) {
		void *e = Server::EntityFromIndex(i);
		if (!e) continue;
		const char *name = Server::ClassNameOf(e);
		for (const char *thrown : kThrown)
			if (0 == strcmp(name, thrown)) Server::RemoveEntity(e);
	}
}

// Brings back whoever a pass needs. A dead player comes back where they fell, so they stay in the area
// they were working in. True when someone had to be respawned.
bool BringBackTheDead() {
	bool any = false;
	for (Actor &a : g.actors) {
		void *controller = Server::EntityFromHandle(a.controllerHandle);
		void *pawn = controller ? Server::PawnOf(controller) : nullptr;
		if (!controller || (pawn && Server::Alive(pawn))) continue;
		any = true;
		Server::Respawn(controller);
	}
	void *host = Server::HostController();
	void *hostPawn = host ? Server::PawnOf(host) : nullptr;
	if (host && Server::Team(host) >= 2 && !(hostPawn && Server::Alive(hostPawn))) {
		float at[3], vel[3];
		const bool known = hostPawn && Server::Pose(hostPawn, at, vel);
		any = true;
		if (Server::Respawn(host) && known) {
			const float still[3] = {0, 0, 0};
			Server::Teleport(Server::PawnOf(host), at, nullptr, still);
		}
	}
	return any;
}

// The team menu's switch waits for the next round, and once takes are set up the round does not end. The
// player would sit dead on their old team for the whole of it.
void ApplyPendingTeamSwitch() {
	if (!g.setupDone) return;
	void *host = Server::HostController();
	if (!host) return;
	const int pending = Server::PendingTeam(host);
	if (pending >= 2 && pending != Server::Team(host)) Server::JoinTeam(host, pending);
}

void WaitForSpawns() {
	ApplyPendingTeamSwitch();
	KickExtraBots();
	ClearLeftovers();
	for (Actor &a : g.actors) a.started = false;
	g.phaseStartTick = Server::TickCount();
	g.lastRespawnTick = g.phaseStartTick - 1000;
	g.lastJoinTick = g.phaseStartTick - 1000;
	g.phase = Phase::WaitSpawn;
}

void BeginPass() {
	ApplySetup();
	const int missing = AssignBots();
	if (missing > 0) {
		RequestMissingBots();
		g.phase = Phase::WaitBots;
		g.phaseStartTick = Server::TickCount();
		return;
	}
	WaitForSpawns();
}

void StartScene() {
	++g.passes;
	void *host = Server::HostController();
	void *hostPawn = host ? Server::PawnOf(host) : nullptr;
	for (Actor &a : g.actors) StartActor(a);
	SpawnMissingObjects();
	g.sceneStartTick = Server::TickCount() + 1;
	g.phase = Phase::Running;
	if (g.demoPending) {
		g.demoPending = false;
		g.demoRecording = true;
		g.demoStopAtEnd = true;
		Execf("tv_record %s", g.demoName.c_str());
		Say("recording demo %s", g.demoName.c_str());
	}
	if (g.rec.armed) {
		if (hostPawn && Server::Alive(hostPawn)) BeginRecordingNow(hostPawn);
		else {
			g.rec.armed = false;
			g.phase = Phase::Idle;
			Say("could not start recording: you are not in the round");
			return;
		}
	}
	MvmDebugLog_Linef("takes.pass", "pass %d: %d bots, %.1f seconds", g.passes, int(g.actors.size()), SceneLengthTicks(g.scene) / g.scene.tickRate);
}

void StopDemoIfAny() {
	if (!g.demoRecording) return;
	Exec("tv_stoprecord");
	g.demoRecording = false;
	g.demoStopAtEnd = false;
	Say("demo saved: %s.dem", g.demoName.c_str());
}

bool GotvRunning() {
	const int maxClients = Server::MaxClients();
	for (int slot = 0; slot < maxClients; ++slot) {
		void *c = Server::ControllerBySlot(slot);
		if (c && Server::Team(c) < 2 && 0 == strcmp(Server::PlayerName(c), "SourceTV")) return true;
	}
	return false;
}

void FinishRecording() {
	RecordingState &r = g.rec;
	r.active = false;
	r.armed = false;
	if (r.track.ticks.size() < 8) {
		Say("recording dropped: nothing was captured");
	} else {
		g.scene.map = CurrentMap();
		g.scene.tracks.push_back(std::move(r.track));
		const Track &t = g.scene.tracks.back();
		Say("%s kept: %.1f seconds. Press Play to see it.", t.name.c_str(), t.ticks.size() / g.scene.tickRate);
		Autosave();
	}
	r.track = Track{};
	r.pawn = nullptr;
	// F7 plays it. Any bot that played along stays where it is now.
	g.phase = Phase::Idle;
}

// The bot's command for this tick. Runs on the server's frame.
bool OnBotBuildCmd(void *pawn, void *cmd) {
	std::lock_guard<std::recursive_mutex> lock(g.mutex);
	Actor *a = ActorByPawn(pawn);
	if (!a || a->track < 0 || a->track >= int(g.scene.tracks.size())) return false;
	const Track &t = g.scene.tracks[a->track];
	const int tick = Server::TickCount();
	const uint32_t handle = Server::HandleOf(pawn);
	TrackFrame frame;
	if (g.phase == Phase::Running && a->started) frame = FrameForSceneTick(t, tick - g.sceneStartTick);
	if (frame.kind != FrameKind::Playing) {
		const std::vector<uint8_t> idle = BuildIdleCmd(tick, handle, a->lastPitch, a->lastYaw);
		Server::ParseCmd(cmd, idle.data(), idle.size());
		Server::SetCmdNumber(cmd, tick);
		return true;
	}
	const TickSample &s = t.ticks[frame.index];
	if (frame.index > 0) {
		const TickSample &before = t.ticks[frame.index - 1];
		float pos[3], vel[3];
		if (before.flags & kTickJumped) {
			// The recording itself moved here without walking, so this is the one 1-to-1 teleport.
			const float angles[3] = {before.pitch, before.yaw, 0};
			Server::Teleport(pawn, before.pos, angles, before.vel);
		} else if (Server::Pose(pawn, pos, vel)) {
			const float drift = Distance3(pos, before.pos);
			a->lastDrift = drift;
			a->maxDrift = (std::max)(a->maxDrift, drift);
			// A different gun runs at a different speed, so its take would slowly leave the path it
			// was recorded on. Moved without the teleport flag, the tiny per-tick nudge stays smooth.
			const bool follow = g.follow == FollowMode::On || (g.follow == FollowMode::Auto && !t.weaponSwaps.empty());
			if (follow && drift > 0.01f) {
				Server::SetAbsOrigin(pawn, before.pos);
			} else if (ShouldCorrectDrift(drift, g.driftLimit)) {
				Server::Teleport(pawn, before.pos, nullptr, before.vel);
				++a->corrections;
				MvmDebugLog_Linef("takes.drift", "track=%s frame=%d drift=%.2f", t.name.c_str(), frame.index, drift);
			}
		}
	}
	CmdRewrite rewrite;
	rewrite.clientTick = tick;
	rewrite.pawnHandle = handle;
	rewrite.stripAttack = t.holdFire;
	rewrite.hasEye = Server::EyePosition(pawn, rewrite.eye);
	if (s.activeWeapon < t.names.size()) {
		const std::string &wanted = SwappedWeapon(t, t.names[s.activeWeapon]);
		void *active = Server::ActiveWeapon(pawn);
		if (!active || wanted != Server::ClassNameOf(active)) {
			// weaponselect is an entity index, as in the game's own bot_mimic copy.
			if (void *owned = Server::FindWeapon(pawn, wanted.c_str())) rewrite.weaponSelect = int32_t(Server::HandleOf(owned) & 0x7fff);
		}
	}
	const std::vector<uint8_t> bytes = RewriteCmdForBot(t.cmds.data() + s.cmdOffset, s.cmdSize, rewrite);
	if (bytes.empty() && s.cmdSize) return false;
	Server::ParseCmd(cmd, bytes.data(), bytes.size());
	Server::SetCmdNumber(cmd, tick);
	a->lastPitch = s.pitch;
	a->lastYaw = s.yaw;
	++a->fed;
	return true;
}

// The server ran a command for this pawn.
void OnCommandRan(void *pawn, void *cmd) {
	std::lock_guard<std::recursive_mutex> lock(g.mutex);
	if (g.rec.active && pawn == g.rec.pawn) {
		RecordCommand(pawn, cmd);
		return;
	}
	if (!g.trace || !MvmDebugLog_Active() || g.phase != Phase::Running) return;
	Actor *a = ActorByPawn(pawn);
	if (!a || a->track < 0 || a->track >= int(g.scene.tracks.size())) return;
	const Track &t = g.scene.tracks[a->track];
	const TrackFrame f = FrameForSceneTick(t, Server::TickCount() - g.sceneStartTick);
	if (f.kind != FrameKind::Playing) return;
	const TickSample &s = t.ticks[f.index];
	float pos[3], vel[3];
	if (!Server::Pose(pawn, pos, vel)) return;
	std::vector<uint8_t> ran;
	Server::SerializeCmd(cmd, ran);
	const CmdSummary c = SummarizeCmd(ran.data(), ran.size());
	const CmdSummary r = SummarizeCmd(t.cmds.data() + s.cmdOffset, s.cmdSize);
	MvmDebugLog_LinefAlways("takes.trace",
		"frame=%d bot pos=(%.2f %.2f %.2f) vel=(%.1f %.1f %.1f) take pos=(%.2f %.2f %.2f) vel=(%.1f %.1f %.1f) d=%.2f "
		"btn=%llx/%llx fwd=%.2f/%.2f left=%.2f/%.2f up=%.2f/%.2f yaw=%.2f/%.2f sub=%d/%d hist=%d/%d weapon=%s/%s",
		f.index, pos[0], pos[1], pos[2], vel[0], vel[1], vel[2], s.pos[0], s.pos[1], s.pos[2], s.vel[0], s.vel[1], s.vel[2], Distance3(pos, s.pos),
		(unsigned long long)c.buttons, (unsigned long long)r.buttons, c.forward, r.forward, c.left, r.left, c.up, r.up, c.yaw, r.yaw,
		c.subtickMoves, r.subtickMoves, c.historyEntries, r.historyEntries,
		Server::ClassNameOf(Server::ActiveWeapon(pawn)), s.activeWeapon < t.names.size() ? t.names[s.activeWeapon].c_str() : "-");
}

bool OnSkipBotThink(void *pawn) {
	std::lock_guard<std::recursive_mutex> lock(g.mutex);
	return ActorByPawn(pawn) != nullptr;
}

bool EnsureHooks() {
	if (!Server::Resolve()) {
		Say("not available: %s", Server::ResolveError());
		return false;
	}
	if (Server::HooksInstalled()) return true;
	Server::Hooks hooks;
	hooks.botBuildCmd = OnBotBuildCmd;
	hooks.commandRan = OnCommandRan;
	hooks.skipBotThink = OnSkipBotThink;
	if (!Server::InstallHooks(hooks)) {
		Say("could not hook the server: %s", Server::ResolveError());
		return false;
	}
	return true;
}

// A scene belongs to one map. Arriving on another brings back what was last made there.
void EnsureSceneForMap() {
	const std::string map = CurrentMap();
	if (map.empty() || map == g.lastMap) return;
	const bool changed = !g.lastMap.empty();
	g.lastMap = map;
	g.setupDone = false;
	if (changed) {
		g.phase = Phase::Idle;
		g.rec = RecordingState{};
		g.actors.clear();
		g.scene = Scene{};
		g_live.clear();
		g_spawnChecks.clear();
	}
	if (g.scene.tracks.empty() && g.scene.objects.empty()) {
		Scene saved;
		if (LoadScene("_last", saved)) g.scene = std::move(saved);
	}
	if (g.scene.map.empty()) g.scene.map = map;
}

// Private matches only. A demo has no server here, and anything online has none we own.
bool CanUseHere(bool quiet = false) {
	if (PlayingDemo()) {
		if (!quiet) Say("takes work in a private match, not in demo playback");
		return false;
	}
	if (CurrentMap().empty() || !Server::Active()) {
		if (!quiet) Say("start a private match first");
		return false;
	}
	return true;
}

void DoRecord(const char *name) {
	if (!CanUseHere() || !EnsureHooks()) return;
	EnsureSceneForMap();
	if (g.rec.active) {
		Say("already recording %s", g.rec.track.name.c_str());
		return;
	}
	ApplySetup();
	// Buying is part of a take, and nothing here starts a new round to hand out mp_startmoney.
	if (void *host = Server::HostController()) Server::SetMoney(host, 65535);
	g.rec = RecordingState{};
	g.rec.track.name = (name && *name) ? name : NextTakeName(g.scene.tracks);
	g.scene.map = CurrentMap();
	// The player stays exactly where they are. Takes already recorded start over with this one, so
	// they line up. Bots go to their own start points, which is why the player walks to the area first.
	g.rec.armed = true;
	BeginPass();
}

void DoStopRecording() {
	if (g.rec.active || g.rec.armed) FinishRecording();
}

void DoPlay() {
	if (!CanUseHere() || !EnsureHooks()) return;
	EnsureSceneForMap();
	if (g.scene.tracks.empty()) {
		// A round never ends by itself once takes are set up, so nothing else would revive the player.
		if (g.setupDone) ApplyPendingTeamSwitch();
		if (g.setupDone && BringBackTheDead()) Say("You are back. Press Record for a take.");
		else Say("nothing to play yet: record a take first");
		return;
	}
	g.passes = 0;
	BeginPass();
}

void DoStop() {
	// A "Record demo" that never reached its pass is called off with it, or the next plain Play
	// would start recording a demo nobody asked for.
	g.demoPending = false;
	if (g.rec.active || g.rec.armed) {
		DoStopRecording();
		return;
	}
	g.phase = Phase::Idle;
	StopDemoIfAny();
	Say("Stopped. Bots stay where they are, and Play starts over.");
}

void PumpActors(int frameTick) {
	for (Actor &a : g.actors) {
		void *pawn = ActorPawn(a);
		a.pawn = pawn;
		if (!pawn) continue;
		const bool alive = Server::Alive(pawn);
		if (a.wasAlive && !alive) ++a.deaths;
		a.wasAlive = alive;
		if (!alive || !a.started || a.track < 0 || a.track >= int(g.scene.tracks.size())) continue;
		const Track &t = g.scene.tracks[a.track];
		std::vector<void *> held;
		Server::ListWeapons(pawn, held);
		for (void *w : held) {
			const uint32_t handle = Server::HandleOf(w);
			if (std::find(a.heldWeapons.begin(), a.heldWeapons.end(), handle) == a.heldWeapons.end()) a.heldWeapons.push_back(handle);
		}
		// What the player picked up or bought in this take. Commands do not carry those, so the bot
		// is handed them at the same moment. Thrown grenades leave by themselves.
		const int local = frameTick - t.delayTicks;
		while (a.nextInventory < t.inventory.size() && int(t.inventory[a.nextInventory].tick) <= local) {
			const InventoryEvent &e = t.inventory[a.nextInventory++];
			if (e.added && e.name < t.names.size()) {
				const std::string &weapon = SwappedWeapon(t, t.names[e.name]);
				if (!Server::FindWeapon(pawn, weapon.c_str())) {
					void *item = Server::Give(pawn, weapon.c_str(), false);
					MvmDebugLog_Linef("takes.give", "track=%s tick=%d weapon=%s item=%p", t.name.c_str(), local, weapon.c_str(), item);
					StampIdentity(t, a, pawn);
				}
			}
		}
	}
}

} // namespace

bool Recording() { return g.rec.active; }

std::atomic<int> g_pendingKey{0};
std::atomic<bool> g_keyRepeat{false}; // the WM_KEYDOWN being handled is an auto-repeat (NoteKeyRepeat)

bool HandleKey(unsigned vkey, bool down) {
	constexpr unsigned kF6 = 0x75, kF7 = 0x76;
	if (vkey != kF6 && vkey != kF7) return false;
	if (PlayingDemo() || CurrentMap().empty() || !Server::Active()) return false;
	// One action per press. Windows repeats WM_KEYDOWN while a key is held, and each repeat used to
	// count as a new press: holding F6 started, dropped and restarted the recording over and over.
	// The repeat comes from the key message itself (NoteKeyRepeat), not from a held flag cleared on
	// the release: a release that arrives while the pause menu or the ReShade overlay owns the input
	// never reaches this function, and a flag left set would swallow every later press.
	if (down && !g_keyRepeat.load(std::memory_order_relaxed)) g_pendingKey.store(int(vkey), std::memory_order_relaxed);
	return true;
}

void NoteKeyRepeat(bool repeat) { g_keyRepeat.store(repeat, std::memory_order_relaxed); }

std::string HudLine(bool &recording) {
	std::lock_guard<std::recursive_mutex> lock(g.mutex);
	recording = g.rec.active;
	if (PlayingDemo()) return "";
	char text[96];
	const float rate = g.scene.tickRate > 0 ? g.scene.tickRate : 64.0f;
	auto clock = [](float seconds, char *out, size_t size) {
		const int s = (std::max)(0, int(seconds));
		snprintf(out, size, "%d:%02d", s / 60, s % 60);
	};
	char a[16], b[16];
	if (g.rec.active) {
		clock(g.rec.track.ticks.size() / rate, a, sizeof(a));
		snprintf(text, sizeof(text), "REC %s  F6 to stop", a);
		return text;
	}
	if (g.phase == Phase::WaitBots) return "Adding bots...";
	if (g.phase == Phase::WaitSpawn) return "Getting ready...";
	// Rounds do not end once takes are set up, so being dead would otherwise look like being stuck.
	if (g.setupDone && (g.phase == Phase::Idle || g.phase == Phase::Finished)) {
		void *host = Server::HostController();
		void *pawn = host ? Server::PawnOf(host) : nullptr;
		if (host && Server::Team(host) >= 2 && !(pawn && Server::Alive(pawn)))
			return g.scene.tracks.empty() ? "You are dead. F7 brings you back" : "You are dead. F7 plays and brings you back";
	}
	if (g.phase == Phase::Running) {
		const int length = SceneLengthTicks(g.scene);
		clock((std::min)(Server::TickCount() - g.sceneStartTick, length) / rate, a, sizeof(a));
		clock(length / rate, b, sizeof(b));
		snprintf(text, sizeof(text), g.demoRecording ? "DEMO %s / %s" : "PLAYING %s / %s  F7 to stop", a, b);
		return text;
	}
	return "";
}

void RunMainThreadFrame() {
	RunHudFrame();
	// GOTV is what records every player at full rate, and its master only starts with a map. So it
	// is asked for on the first frame, before any private match or workshop map loads. It costs a
	// spectator slot on a local server and does nothing for demo playback.
	static bool gotvAsked = false;
	if (!gotvAsked && g_pEngineToClient) {
		gotvAsked = true;
		Exec("tv_enable 1");
		Exec("tv_autorecord 0");
	}
	std::lock_guard<std::recursive_mutex> lock(g.mutex);
	const int key = g_pendingKey.exchange(0, std::memory_order_relaxed);
	if (key == 0x75) {
		if (g.rec.active || g.rec.armed) DoStop();
		else DoRecord("");
	} else if (key == 0x76) {
		// Pressed again after a take ended by itself, it plays from the start.
		if (g.rec.active || g.rec.armed || (g.phase != Phase::Idle && g.phase != Phase::Finished)) DoStop();
		else DoPlay();
	}
	// Solid props are the one thing that wants this pump with no take in sight, so they get a cheap
	// test here: a demo or a match without them returns straight away.
	const bool solid = !PlayingDemo() && Props_AnySolid();
	if (g.phase == Phase::Idle && !g.setupDone && !g.rec.active && !g.rec.armed && g_spawnChecks.empty() && !solid && g_live.empty() && !g.recoilApplied && g.recoilRandom == 100) return;
	if (PlayingDemo() || CurrentMap().empty() || !Server::Active()) {
		// The map or the match went away under us.
		g.rec = RecordingState{};
		g.actors.clear();
		g.phase = Phase::Idle;
		g.setupDone = false;
		g.demoRecording = false;
		g.demoPending = false;
		g_spawnChecks.clear();
		g_live.clear();
		return;
	}
	PumpSpawnChecks();
	const int tick = Server::TickCount();
	// A few times a second is plenty for things a person changes by hand.
	if (tick < g.housekeepingTick || tick - g.housekeepingTick >= 20) {
		g.housekeepingTick = tick;
		EnsureSceneForMap();
		ApplyPendingTeamSwitch();
		RestoreMissingWorld();
		SyncSolidProps();
		ApplyRecoilRandomness();
	}
	void *host = Server::HostController();
	void *hostPawn = host ? Server::PawnOf(host) : nullptr;
	if (g.rec.active) {
		// The pawn can be replaced under a recording (a respawn); dying ends the take where it is.
		if (!hostPawn || !Server::Alive(hostPawn)) {
			Say("you died: take ends here");
			FinishRecording();
			return;
		}
		g.rec.pawn = hostPawn;
	}
	switch (g.phase) {
	case Phase::WaitBots: {
		const int missing = AssignBots();
		if (missing == 0) {
			WaitForSpawns();
		} else if (tick - g.phaseStartTick > 64 * 8) {
			Say("could not add %d bot%s (is the server full?)", missing, missing == 1 ? "" : "s");
			g.phase = Phase::Idle;
			g.rec.armed = false;
			g.demoPending = false;
		}
		break;
	}
	case Phase::WaitSpawn: {
		// Still on the team screen, where a match with nobody on a team also sits in freeze time for
		// good. The game can ignore this while it is loading, so it is sent again.
		if (g.rec.armed && host && Server::Team(host) < 2 && tick - g.lastJoinTick >= 64) {
			g.lastJoinTick = tick;
			Exec("jointeam 3");
		}
		// Warmup ending restarts the round, which moves everyone. Nothing starts until that has passed.
		if (Server::MatchSettling()) {
			Server::HurryMatch();
			if (tick - g.phaseStartTick <= 64 * 30) break;
			Say("the match stayed in warmup, starting anyway");
		}
		// A bot from the old match may have been kicked under its assignment.
		if (AssignBots() > 0) {
			RequestMissingBots();
			g.phase = Phase::WaitBots;
			g.phaseStartTick = tick;
			break;
		}
		if (tick - g.lastRespawnTick >= 16 && BringBackTheDead()) g.lastRespawnTick = tick;
		bool ready = tick - g.lastRespawnTick >= 4; // the game hands out its own weapons right after a spawn
		for (const Actor &a : g.actors) {
			void *pawn = ActorPawn(a);
			ready = ready && pawn && Server::Alive(pawn);
		}
		if (g.rec.armed) ready = ready && hostPawn && Server::Alive(hostPawn);
		if (ready || tick - g.phaseStartTick > 64 * 10) StartScene();
		break;
	}
	case Phase::Running: {
		const int sceneTick = tick - g.sceneStartTick;
		PumpActors(sceneTick);
		if (g.rec.active) break; // a take being recorded sets the length
		const int length = SceneLengthTicks(g.scene);
		// A loop or a demo holds the last moment for a beat. A plain pass is over when it is over.
		if (g.demoRecording && g.demoStopAtEnd && sceneTick >= length + g.tailTicks) {
			StopDemoIfAny();
			g.phase = Phase::Finished;
			break;
		}
		if (g.demoRecording) break;
		if (g.loop) {
			if (sceneTick >= length + g.tailTicks) BeginPass();
		} else if (sceneTick >= length) {
			g.phase = Phase::Finished;
			Say("Finished. Bots stay where their takes ended, and Play starts over.");
		}
		break;
	}
	case Phase::Finished:
	case Phase::Idle:
		PumpActors(0);
		break;
	}
}

namespace {

int ParseTrackIndex(const char *text) {
	const int n = atoi(text) - 1;
	return (n >= 0 && n < int(g.scene.tracks.size())) ? n : -1;
}

void PrintStatus() {
	Server::Resolve();
	advancedfx::Message("takes: %s | map '%s' | %d take%s, %d object%s | loop %s, drift limit %.0f\n",
	                    g.rec.active ? "RECORDING" : PhaseName(g.phase), CurrentMap().c_str(), int(g.scene.tracks.size()),
	                    g.scene.tracks.size() == 1 ? "" : "s", int(g.scene.objects.size()), g.scene.objects.size() == 1 ? "" : "s",
	                    g.loop ? "on" : "off", g.driftLimit);
	advancedfx::Message("  match: %s\n", Server::DescribeMatch().c_str());
	if (void *you = Server::HostController()) {
		void *pawn = Server::PawnOf(you);
		advancedfx::Message("  you: team %d, %s, pending %d\n", Server::Team(you), pawn ? (Server::Alive(pawn) ? "alive" : "dead") : "no pawn", Server::PendingTeam(you));
	}
	advancedfx::Message("  server: %s, hooks %s%s%s\n", Server::Active() ? "local" : "none", Server::HooksInstalled() ? "in" : "not in",
	                    *Server::ResolveError() ? ", problem: " : "", Server::ResolveError());
	if (g.rec.active) advancedfx::Message("  recording %s: %d ticks\n", g.rec.track.name.c_str(), int(g.rec.track.ticks.size()));
	if (g.phase == Phase::Running) {
		advancedfx::Message("  pass %d, scene tick %d of %d\n", g.passes, Server::TickCount() - g.sceneStartTick, SceneLengthTicks(g.scene));
	}
	for (size_t i = 0; i < g.scene.tracks.size(); ++i) {
		const Track &t = g.scene.tracks[i];
		const Actor *actor = nullptr;
		for (const Actor &a : g.actors)
			if (a.track == int(i)) actor = &a;
		advancedfx::Message("  %zu | %s | %s | %.1fs | %s%s", i + 1, t.name.c_str(), t.team == 2 ? "T" : "CT", t.ticks.size() / g.scene.tickRate,
		                    t.enabled ? "on" : "off", t.holdFire ? ", holds fire" : "");
		if (actor) {
			float at[3] = {0, 0, 0}, vel[3];
			if (actor->pawn) Server::Pose(actor->pawn, at, vel);
			void *held = actor->pawn ? Server::ActiveWeapon(actor->pawn) : nullptr;
			advancedfx::Message(" | bot %s hp %d at (%.0f %.0f %.0f) holding %s clip %d, fed %d, drift now %.2f max %.2f, corrections %d, deaths %d", actor->pawn ? "in" : "gone",
			                    actor->pawn ? Server::Health(actor->pawn) : 0, at[0], at[1], at[2], held ? Server::ClassNameOf(held) : "nothing", Server::ClipOf(held), actor->fed, actor->lastDrift, actor->maxDrift, actor->corrections, actor->deaths);
		}
		advancedfx::Message("\n");
	}
	for (size_t i = 0; i < g.scene.objects.size(); ++i) {
		const SceneObject &o = g.scene.objects[i];
		advancedfx::Message("  object %zu | %s | %s | %s | (%.0f %.0f %.0f)\n", i + 1, o.name.c_str(), o.model.c_str(), o.solid ? "solid" : "no collision",
		                    o.pos[0], o.pos[1], o.pos[2]);
	}
}

void PrintHelp(const char *cmd) {
	advancedfx::Message(
		"%s takes record [name]  - start recording yourself (bots already recorded play along)\n"
		"%s takes stop           - end the recording, or stop playing\n"
		"%s takes play           - play every take once as bots, from wherever you are; the dead come back\n"
		"%s takes loop on|off    - play again after the last take ends (off by default)\n"
		"%s takes status         - what is happening, and how close each bot stays to its take\n"
		"%s takes delete <n> | enable <n> on|off | clear\n"
		"%s takes team <n> t|ct | name <n> <text> | holdfire <n> on|off | delay <n> <seconds>\n"
		"%s takes model <n> <characters/models/.../x.vmdl>\n"
		"%s takes weapon <n> <weapon_ak47> <weapon_m4a1>  - hold another gun in that take\n"
		"%s takes object add <model.vmdl> [nosolid] [scale] | move <n> x y z | remove <n> | list | clear\n"
		"%s takes follow auto|on|off - hold bots to the recorded path (auto: only takes with a swapped gun)\n"
		"%s takes drift <units>  - snap a bot back when it strays this far (0 = never, the default)\n"
		"%s takes nospread on|off | recoilrandom <0-100>\n"
		"%s takes demo [name]    - play one pass from the start and record it to a demo\n"
		"%s takes save <name> | load <name>\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

void AddObjectAtCrosshair(const char *model, bool solid, float scale) {
	void *host = Server::HostController();
	void *pawn = host ? Server::PawnOf(host) : nullptr;
	float pos[3] = {0, 0, 0}, vel[3];
	if (!pawn || !Server::Pose(pawn, pos, vel)) {
		Say("you need to be alive to place something");
		return;
	}
	SceneObject o;
	o.model = model;
	o.solid = solid;
	o.scale = scale;
	// Far enough ahead that a big model does not swallow the player, on the floor they stand on.
	float pitch = 0, yaw = 0;
	Server::EyeAngles(pawn, pitch, yaw);
	const float rad = yaw * 3.14159265f / 180.0f;
	o.pos[0] = pos[0] + std::cos(rad) * 160.0f;
	o.pos[1] = pos[1] + std::sin(rad) * 160.0f;
	o.pos[2] = pos[2];
	o.ang[1] = yaw + 180.0f;
	char name[64];
	for (int n = int(g.scene.objects.size()) + 1;; ++n) {
		snprintf(name, sizeof(name), "mvm_obj_%d", n);
		bool used = false;
		for (const SceneObject &x : g.scene.objects) used = used || x.name == name;
		if (!used) break;
	}
	o.name = name;
	g.scene.objects.push_back(o);
	g.scene.map = CurrentMap();
	SpawnWorld(FromObject(o));
	Autosave();
	Say("placed %s", o.name.c_str());
}

} // namespace

void RunCommand(int argc, advancedfx::ICommandArgs *args, const char *cmd) {
	std::lock_guard<std::recursive_mutex> lock(g.mutex);
	const char *sub = argc >= 3 ? args->ArgV(2) : "status";
	auto arg = [&](int i) { return i < argc ? args->ArgV(i) : ""; };
	LoadSettings();

	if (0 == _stricmp(sub, "status")) {
		PrintStatus();
	} else if (0 == _stricmp(sub, "help")) {
		PrintHelp(cmd);
	} else if (0 == _stricmp(sub, "record")) {
		DoRecord(arg(3));
	} else if (0 == _stricmp(sub, "stop")) {
		DoStop();
	} else if (0 == _stricmp(sub, "play")) {
		DoPlay();
	} else if (0 == _stricmp(sub, "loop")) {
		g.loop = 0 != _stricmp(arg(3), "off");
		SaveSettings();
		Say("loop %s", g.loop ? "on" : "off");
	} else if (0 == _stricmp(sub, "follow")) {
		const char *mode = arg(3);
		g.follow = 0 == _stricmp(mode, "on") ? FollowMode::On : 0 == _stricmp(mode, "off") ? FollowMode::Off : FollowMode::Auto;
		SaveSettings();
		Say("stay on the path: %s", FollowName(g.follow));
	} else if (0 == _stricmp(sub, "nospread")) {
		g.noSpread = 0 != _stricmp(arg(3), "off");
		SaveSettings();
		Exec("mirv_cvar_unlock_sv_cheats");
		Exec("sv_cheats 1");
		Execf("weapon_accuracy_nospread %d", g.noSpread ? 1 : 0);
		Say("random spread %s", g.noSpread ? "off" : "on");
	} else if (0 == _stricmp(sub, "recoilrandom")) {
		g.recoilRandom = (std::max)(0, (std::min)(100, atoi(arg(3))));
		SaveSettings();
		if (CanUseHere(true)) ApplyRecoilRandomness();
		Say("recoil randomness %d%%", g.recoilRandom);
	} else if (0 == _stricmp(sub, "ents")) {
		// Development check: how many live entities have this in their class name.
		const char *needle = arg(3);
		int count = 0;
		for (int i = 0; i < 16384; ++i) {
			void *e = Server::EntityFromIndex(i);
			if (!e) continue;
			const char *name = Server::ClassNameOf(e);
			if (*needle ? nullptr != strstr(name, needle) : false) {
				++count;
				float pos[3] = {0, 0, 0}, vel[3];
				Server::Pose(e, pos, vel);
				advancedfx::Message("  %d %s (%.0f %.0f %.0f) owner-team %d\n", i, name, pos[0], pos[1], pos[2], Server::Team(e));
			}
		}
		advancedfx::Message("takes: %d live '%s'\n", count, needle);
	} else if (0 == _stricmp(sub, "trace")) {
		g.trace = 0 != _stricmp(arg(3), "off");
		Say("trace %s", g.trace ? "on" : "off");
	} else if (0 == _stricmp(sub, "drift")) {
		g.driftLimit = float(atof(arg(3)));
		Say("drift limit %.1f", g.driftLimit);
	} else if (0 == _stricmp(sub, "clear")) {
		// This map's scene first. A clear typed before the panel or a take ever touched this map
		// would otherwise find nothing loaded, leave the autosave where it is (Autosave only removes
		// it for a map it has read), and the scene would come back when CONFIG opened.
		EnsureSceneForMap();
		g.scene = Scene{};
		g.actors.clear();
		g.phase = Phase::Idle;
		// Before setup these are somebody's ordinary match bots, and rounds can still end: kicking them
		// would end this one.
		if (g.setupDone) KickExtraBots();
		Autosave();
		Say("scene cleared");
	} else if (0 == _stricmp(sub, "delete") || 0 == _stricmp(sub, "enable") || 0 == _stricmp(sub, "team") || 0 == _stricmp(sub, "name") ||
	           0 == _stricmp(sub, "holdfire") || 0 == _stricmp(sub, "delay") || 0 == _stricmp(sub, "model") || 0 == _stricmp(sub, "weapon")) {
		const int n = ParseTrackIndex(arg(3));
		if (n < 0) {
			Say("no take %s", arg(3));
			return;
		}
		Track &t = g.scene.tracks[n];
		if (0 == _stricmp(sub, "delete")) {
			const std::string name = t.name;
			g.scene.tracks.erase(g.scene.tracks.begin() + n);
			for (Actor &a : g.actors) {
				if (a.track == n) a.track = -1;
				else if (a.track > n) --a.track;
			}
			if (g.scene.tracks.empty()) {
				g.phase = Phase::Idle;
				g.actors.clear();
				// Before setup these are somebody's ordinary match bots (see `clear`).
				if (g.setupDone) KickExtraBots();
			}
			Say("%s deleted", name.c_str());
		} else if (0 == _stricmp(sub, "enable")) {
			t.enabled = 0 != _stricmp(arg(4), "off");
		} else if (0 == _stricmp(sub, "team")) {
			t.team = 0 == _stricmp(arg(4), "t") ? 2 : 3;
			for (Actor &a : g.actors)
				if (a.track == n) a.track = -1;
		} else if (0 == _stricmp(sub, "name")) {
			t.name = arg(4);
			for (const Actor &a : g.actors)
				if (a.track == n)
					if (void *c = Server::EntityFromHandle(a.controllerHandle)) Server::SetPlayerName(c, t.name.c_str());
		} else if (0 == _stricmp(sub, "holdfire")) {
			t.holdFire = 0 != _stricmp(arg(4), "off");
		} else if (0 == _stricmp(sub, "delay")) {
			t.delayTicks = int32_t(atof(arg(4)) * g.scene.tickRate);
		} else if (0 == _stricmp(sub, "model")) {
			t.agentModel = 0 == strcmp(arg(4), "-") ? "" : arg(4);
			for (const Actor &a : g.actors)
				if (a.track == n)
					if (void *pawn = ActorPawn(a)) ApplyAgentModel(t, a, pawn);
			Say("%s now wears %s", t.name.c_str(), t.agentModel.empty() ? "the model its bot joined with" : t.agentModel.c_str());
		} else if (0 == _stricmp(sub, "weapon")) {
			const std::string from = arg(4), to = arg(5);
			t.weaponSwaps.erase(std::remove_if(t.weaponSwaps.begin(), t.weaponSwaps.end(), [&](const WeaponSwap &s) { return s.from == from; }),
			                    t.weaponSwaps.end());
			if (!to.empty() && to != "-" && from != to) t.weaponSwaps.push_back({from, to});
			Say("%s: %s plays as %s from the next pass", t.name.c_str(), from.c_str(), to.empty() ? from.c_str() : to.c_str());
		}
		Autosave();
	} else if (0 == _stricmp(sub, "object")) {
		const char *what = arg(3);
		if (0 == _stricmp(what, "add")) {
			if (!CanUseHere()) return;
			ApplySetup();
			bool solid = true;
			float scale = 1.0f;
			for (int i = 5; i < argc; ++i) {
				if (0 == _stricmp(arg(i), "nosolid")) solid = false;
				else if (atof(arg(i)) > 0) scale = float(atof(arg(i)));
			}
			AddObjectAtCrosshair(arg(4), solid, scale);
		} else if (0 == _stricmp(what, "move")) {
			const int n = atoi(arg(4)) - 1;
			if (n < 0 || n >= int(g.scene.objects.size())) {
				Say("no object %s", arg(4));
				return;
			}
			SceneObject &o = g.scene.objects[n];
			o.pos[0] = float(atof(arg(5)));
			o.pos[1] = float(atof(arg(6)));
			o.pos[2] = float(atof(arg(7)));
			if (argc > 8) o.ang[1] = float(atof(arg(8)));
			SpawnWorld(FromObject(o));
			Autosave();
		} else if (0 == _stricmp(what, "remove")) {
			const int n = atoi(arg(4)) - 1;
			if (n < 0 || n >= int(g.scene.objects.size())) {
				Say("no object %s", arg(4));
				return;
			}
			RemoveLive(g.scene.objects[n].name);
			g.scene.objects.erase(g.scene.objects.begin() + n);
			Autosave();
		} else if (0 == _stricmp(what, "clear")) {
			for (const SceneObject &o : g.scene.objects) RemoveLive(o.name);
			g.scene.objects.clear();
			Autosave();
		} else {
			PrintStatus();
		}
	} else if (0 == _stricmp(sub, "demo")) {
		if (!CanUseHere() || !EnsureHooks()) return;
		if (g.scene.tracks.empty()) {
			Say("nothing to play yet: record a take first");
			return;
		}
		if (!GotvRunning()) {
			// tv_enable only counts from the next map load.
			Exec("tv_enable 1");
			Say("demos need GOTV, which starts with a map: load this map again, then record the demo");
			return;
		}
		if (argc > 3) {
			g.demoName = arg(3);
		} else {
			char stamp[32];
			SYSTEMTIME now;
			GetLocalTime(&now);
			snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
			g.demoName = "takes_" + SafeFileName(CurrentMap()) + "_" + stamp;
		}
		g.demoPending = true;
		g.passes = 0;
		BeginPass();
		Say("demo starts with this pass");
	} else if (0 == _stricmp(sub, "save")) {
		const std::string name = argc > 3 ? arg(3) : "scene";
		Say(SaveScene(name) ? "saved %s" : "could not save %s", name.c_str());
	} else if (0 == _stricmp(sub, "load")) {
		const std::string name = argc > 3 ? arg(3) : "_last";
		Scene scene;
		if (!LoadScene(name, scene)) {
			Say("could not load %s on this map", name.c_str());
			return;
		}
		g.scene = std::move(scene);
		g.actors.clear();
		Say("loaded %s: %d takes", name.c_str(), int(g.scene.tracks.size()));
	} else {
		PrintHelp(cmd);
	}
}

void AppendStateJson(std::ostringstream &out) {
	std::lock_guard<std::recursive_mutex> lock(g.mutex);
	LoadSettings();
	const bool available = !PlayingDemo() && !CurrentMap().empty();
	if (available) EnsureSceneForMap();
	const float rate = g.scene.tickRate > 0 ? g.scene.tickRate : 64.0f;
	std::vector<SolidProp> solid;
	Props_CollectSolid(solid);
	out << ",\"takes\":{\"available\":" << (available ? "true" : "false");
	out << ",\"phase\":\"" << PhaseName(g.phase) << "\",\"recording\":" << (g.rec.active ? "true" : "false");
	out << ",\"recSeconds\":" << g.rec.track.ticks.size() / rate;
	out << ",\"sceneSeconds\":" << (g.phase == Phase::Running ? (Server::TickCount() - g.sceneStartTick) / rate : 0.0f);
	out << ",\"lengthSeconds\":" << SceneLengthTicks(g.scene) / rate;
	out << ",\"loop\":" << (g.loop ? "true" : "false");
	out << ",\"follow\":\"" << FollowName(g.follow) << "\"";
	out << ",\"noSpread\":" << (g.noSpread ? "true" : "false") << ",\"recoilRandom\":" << g.recoilRandom;
	out << ",\"demoRecording\":" << (g.demoRecording ? "true" : "false");
	out << ",\"solidNew\":" << (Props_SolidNew() ? "true" : "false") << ",\"solidCount\":" << solid.size();
	out << ",\"message\":\"" << JsonBuilder::Escape(g.message) << "\"";
	out << ",\"tracks\":[";
	for (size_t i = 0; i < g.scene.tracks.size(); ++i) {
		const Track &t = g.scene.tracks[i];
		const Actor *actor = nullptr;
		for (const Actor &a : g.actors)
			if (a.track == int(i)) actor = &a;
		if (i) out << ',';
		out << "{\"name\":\"" << JsonBuilder::Escape(t.name) << "\",\"team\":" << int(t.team) << ",\"seconds\":" << t.ticks.size() / rate;
		out << ",\"enabled\":" << (t.enabled ? "true" : "false") << ",\"holdFire\":" << (t.holdFire ? "true" : "false");
		out << ",\"delay\":" << t.delayTicks / rate << ",\"agent\":\"" << JsonBuilder::Escape(t.agentModel) << "\"";
		out << ",\"hasBot\":" << (actor ? "true" : "false") << ",\"maxDrift\":" << (actor ? actor->maxDrift : 0.0f) << ",\"deaths\":" << (actor ? actor->deaths : 0);
		out << ",\"weapons\":[";
		bool first = true;
		for (const std::string &n : t.names) {
			// Guns only: a knife or a grenade has no swap in this list.
			if (n.find("knife") != std::string::npos || n.find("grenade") != std::string::npos || n == "weapon_molotov" || n == "weapon_incgrenade" ||
			    n == "weapon_flashbang" || n == "weapon_decoy" || n == "weapon_c4" || n == "weapon_taser" || n.rfind("weapon_", 0) != 0)
				continue;
			out << (first ? "" : ",") << '"' << JsonBuilder::Escape(n) << '"';
			first = false;
		}
		out << "],\"swaps\":[";
		for (size_t w = 0; w < t.weaponSwaps.size(); ++w)
			out << (w ? "," : "") << "[\"" << JsonBuilder::Escape(t.weaponSwaps[w].from) << "\",\"" << JsonBuilder::Escape(t.weaponSwaps[w].to) << "\"]";
		out << "]}";
	}
	out << "]}";
}

} // namespace Filmmaker::Takes
