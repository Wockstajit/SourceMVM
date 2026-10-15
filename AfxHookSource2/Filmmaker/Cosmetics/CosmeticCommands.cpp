// Console command surface for the SteamID-keyed cosmetic override system: "mirv_filmmaker
// cosmetics <subcommand> ...". Mirrors the existing DoCosmetics() style in FilmmakerCommand.cpp
// (advancedfx::Message/Warning, args->ArgV(i)) but drives CosmeticOverrideSystem (CosmeticsRef())
// instead of the legacy pawn-indexed MirvCosmetics backend.
//
// Kept in its own translation unit per the Cosmetics/ folder layout (CosmeticOverrideSystem.cpp /
// CosmeticProfile.cpp / CosmeticCatalog.cpp already split this way).

#include "CosmeticOverrideSystem.h"
#include "CosmeticCatalog.h"
#include "CosmeticDirectComposite.h"
#include "../Debug/MvmDebugLog.h"
#include "CosmeticModelSwap.h"
#include "CosmeticAnimFix.h"
#include "CosmeticEntityAccess.h" // CosmeticEnumeratePlayers: the "everyone" actions

#include "../../../Shared/AfxConsole.h"

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <chrono>

namespace Filmmaker {

namespace {

// Case-insensitive token compare, matching the _stricmp idiom used throughout FilmmakerCommand.cpp.
bool TokenIs(const char* token, const char* literal) {
	return token && 0 == _stricmp(token, literal);
}

// Resolves a steamId argument that is either the literal "current" (the player the local viewer is
// currently spectating) or a decimal SteamID64. Returns 0 and prints a Warning on failure.
uint64_t ResolveSteamIdArg(const char* cmd, const char* token) {
	if (!token || !*token) {
		advancedfx::Warning("%s cosmetics: missing steamId.\n", cmd);
		return 0;
	}
	if (TokenIs(token, "current")) {
		uint64_t id = CosmeticsRef().CurrentSpectatedSteamId();
		if (id == 0)
			advancedfx::Warning("%s cosmetics: no spectated player to resolve 'current' to.\n", cmd);
		return id;
	}
	uint64_t id = _strtoui64(token, nullptr, 10);
	if (id == 0)
		advancedfx::Warning("%s cosmetics: invalid steamId '%s'.\n", cmd, token);
	return id;
}

// Weapon finish profiles follow the gun's original econ owner. Knife definitions (including the
// weapon-command aliases) instead use the spectated player's single knife slot. Numeric IDs keep
// their explicit meaning for both picks and clears.
uint64_t ResolveWeaponSteamIdArg(const char* cmd, const char* token, int defIndex) {
	if (!TokenIs(token, "current") || CosmeticCatalog::IsKnifeDef(defIndex))
		return ResolveSteamIdArg(cmd, token);
	uint64_t id = CosmeticsRef().CurrentActiveWeaponOwnerSteamId();
	if (id == 0)
		advancedfx::Warning("%s cosmetics: no active weapon on the spectated player to resolve 'current' to.\n", cmd);
	return id;
}

// After a successful profile mutation: arm this demo session, and (when scheduleTickNudge) briefly
// auto-play + re-pause so the engine actually renders the frames needed for the change to visually
// take -- weapon paint (the first-person viewmodel mirror) and agent/glove/knife/body swaps all need
// this; only bare composite-refresh-only paths should pass false.
void AfterMutation(const char* cmd, bool scheduleTickNudge) {
	CosmeticsRef().Arm();
	if (scheduleTickNudge)
		CosmeticsRef().RequestApplyNudge();
	else
		CosmeticsRef().RequestCompositeRefresh();
	if (!CosmeticsRef().OffsetsAvailable())
		advancedfx::Warning("%s cosmetics: econ offsets did not resolve; cosmetics will not render.\n", cmd);
}

void PrintUsage(const char* cmd) {
	advancedfx::Warning("usage: %s cosmetics <subcommand> ...\n", cmd);
	advancedfx::Message("  %s cosmetics recorded [fullHandle]   (read-only: compact journal enumeration; filtered raw originals and owned write intents)\n", cmd);
	advancedfx::Message(
		"  %s cosmetics enabled [0|1]\n"
		"  %s cosmetics status\n"
		"  %s cosmetics debug [0|1]\n"
		"  %s cosmetics clear\n"
		"  %s cosmetics clearPlayer <steamId|current>\n"
		"  %s cosmetics clearWeapon <steamId|current> <defIndex>\n"
		"  %s cosmetics target current\n"
		"  %s cosmetics player <steamId|current> weapon <defIndex> [paint=N] [wear=F] [seed=N] [stattrak=N]\n"
		"  %s cosmetics player <steamId|current> knife <defIndex> [paint=N] [wear=F] [seed=N]\n"
		"  %s cosmetics player <steamId|current> gloves <defIndex> [paint=N] [wear=F] [seed=N]\n"
		"  %s cosmetics player <steamId|current> agent <modelPath|default>\n"
		"  %s cosmetics allplayers agent <modelPath|default>   (every player on that model team)\n"
			"  %s cosmetics diag   (dump the spectated player's live weapon econ state)\n"
			"  %s cosmetics visualdiag   (read-only: full visual-cache state + flag offsets)\n"
			"  %s cosmetics spectate <steamId|current>   (first-person spectate by steam; skips dead)\n"
			"  %s cosmetics skinlog   (log the spectated player's full LIVE skin state to mvm_debug; auto-logs on switch/seek/weapon/loadout change)\n"
			"  %s cosmetics rebuild once   (re-assert enabled rebuildflags on matched weapons now)\n"
			"  %s cosmetics rebuild auto [0|1]   (per-frame writing of enabled rebuildflags on change)\n"
			"  %s cosmetics rebuildflags [<name> 0|1 | all 0|1]   (toggle individual stale-mark writes)\n"
			"  %s cosmetics paintkitbridge [0|1|auto|force <paint>]   (global deploy-time bridge)\n"
			"  %s cosmetics composite [once [ownerOffsetHex] | stages <mask>]   (paused deployed weapon)\n"
			"  %s cosmetics autocomposite [0|1]   (paused deployed-weapon refresh; default 1)\n"
			"  %s cosmetics modelswap [0|1 | knife 0|1]   (model swaps incl. knife type; both default on)\n"
			"  %s cosmetics animreset [0|1 | offsets <instHex> <varsHex>]   (experimental: null animgraph vars after a knife SetModel)\n"
			"  %s cosmetics precache [0|1]   (blocking-load the knife/agent model before SetModel; default on)\n"
			"  %s cosmetics animfix [0|1]   (detour client.dll anim builder; empty-list for null out-param; THE knife-crash fix; default on)\n"
			"  %s cosmetics ticknudge [on|off|<ticks>]   (briefly play the demo after a change so body swaps re-render)\n"
			"  %s cosmetics debuglog [start|stop]   (timestamped debug log; alias: mvm_debug start|stop)\n"
			"  %s cosmetics uilog <text>   (echo a verbatim UI label to the console + debug log; sent by the Customize panel)\n"
			"  %s cosmetics mesh [auto|modern|legacy | masks <m> <l>]   (legacy-vs-CS2 weapon mesh selection)\n"
			"  %s cosmetics recompose [0|1]   (force material re-composite after writes)\n"
			"  %s cosmetics vtidx <comp> <sec>   (tune UpdateComposite vtable indices, -1=skip)\n"
			"  %s cosmetics vtprobe <idx>   (bisect OnDataChanged: call weapon vtable[idx](this,0) now)\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

// Parses ORDER-INDEPENDENT key/value tokens of the form "key=value" (or "key value", to be lenient)
// starting at argv index `from`. Recognized keys: paint, wear, seed, stattrak. Unknown keys are
// ignored. Values keep their caller-supplied defaults when the key is absent.
void ParseItemKeyValueArgs(int argc, advancedfx::ICommandArgs* args, int from,
	int* paintKit, float* wear, int* seed, int* statTrak) {
	for (int i = from; i < argc; ++i) {
		const char* tok = args->ArgV(i);
		if (!tok || !*tok)
			continue;
		const char* eq = std::strchr(tok, '=');
		std::string key;
		std::string value;
		if (eq) {
			key.assign(tok, eq - tok);
			value.assign(eq + 1);
		} else {
			// Lenient "key value" form: consume the next token as the value if present.
			key.assign(tok);
			if (i + 1 < argc) {
				value.assign(args->ArgV(i + 1));
				++i;
			}
		}
		if (key.empty() || value.empty())
			continue;
		if (0 == _stricmp(key.c_str(), "paint")) {
			if (paintKit) *paintKit = atoi(value.c_str());
		} else if (0 == _stricmp(key.c_str(), "wear")) {
			if (wear) *wear = (float)atof(value.c_str());
		} else if (0 == _stricmp(key.c_str(), "seed")) {
			if (seed) *seed = atoi(value.c_str());
		} else if (0 == _stricmp(key.c_str(), "stattrak")) {
			if (statTrak) *statTrak = atoi(value.c_str());
		}
	}
}

void DoEnabled(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Message("%s cosmetics enabled = %d.\n", cmd, CosmeticsRef().Enabled() ? 1 : 0);
		return;
	}
	bool enabled = 0 != atoi(args->ArgV(3));
	CosmeticsRef().SetEnabled(enabled);
	advancedfx::Message("%s cosmetics enabled = %d.\n", cmd, CosmeticsRef().Enabled() ? 1 : 0);
}

void DoDebug(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Message("%s cosmetics debug = %d.\n", cmd, CosmeticsRef().Debug() ? 1 : 0);
		return;
	}
	CosmeticsRef().SetDebug(0 != atoi(args->ArgV(3)));
	advancedfx::Message("%s cosmetics debug = %d.\n", cmd, CosmeticsRef().Debug() ? 1 : 0);
}

void DoClear(const char* cmd) {
	CosmeticsRef().ClearAll();
	CosmeticsRef().RequestApplyNudge();
	advancedfx::Message("%s cosmetics: cleared all profiles.\n", cmd);
}

void DoClearPlayer(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Warning("usage: %s cosmetics clearPlayer <steamId|current>\n", cmd);
		return;
	}
	uint64_t id = ResolveSteamIdArg(cmd, args->ArgV(3));
	if (id == 0)
		return;
	CosmeticsRef().ClearPlayer(id);
	CosmeticsRef().RequestApplyNudge();
	advancedfx::Message("%s cosmetics: cleared profile for steamId %llu.\n", cmd, (unsigned long long)id);
}

void DoClearWeapon(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 5) {
		advancedfx::Warning("usage: %s cosmetics clearWeapon <steamId|current> <defIndex>\n", cmd);
		return;
	}
	int defIndex = atoi(args->ArgV(4));
	uint64_t id = ResolveWeaponSteamIdArg(cmd, args->ArgV(3), defIndex);
	if (id == 0)
		return;
	CosmeticsRef().ClearWeapon(id, defIndex);
	CosmeticsRef().RequestApplyNudge();
	advancedfx::Message("%s cosmetics: cleared weapon def=%d for steamId %llu.\n",
		cmd, defIndex, (unsigned long long)id);
}

void DoTarget(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 4) ? args->ArgV(3) : "";
	if (!TokenIs(sub, "current")) {
		advancedfx::Warning("usage: %s cosmetics target current\n", cmd);
		return;
	}
	uint64_t id = CosmeticsRef().CurrentSpectatedSteamId();
	if (id == 0) {
		advancedfx::Warning("%s cosmetics target: no spectated player.\n", cmd);
		return;
	}
	std::string name = CosmeticsRef().NameForSteamId(id);
	advancedfx::Message("%s cosmetics target: steamId=%llu name='%s'.\n",
		cmd, (unsigned long long)id, name.c_str());
}

void DoPlayer(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 6) {
		advancedfx::Warning(
			"usage: %s cosmetics player <steamId|current> <weapon|knife|gloves|agent> <defIndex|model> "
			"[paint=N] [wear=F] [seed=N] [stattrak=N]\n", cmd);
		return;
	}
	const char* slot = args->ArgV(4);
	const char* steamToken = args->ArgV(3);

	// Weapon-entity overrides are matched by ApplyMatchedWeapons against the WEAPON's actual econ
	// owner (OriginalOwnerXuid), which differs from the spectated player's own SteamID when they are
	// holding a picked-up weapon. "current" must resolve to the HELD WEAPON's owner for this slot, or
	// the override is filed under the wrong key and silently never matches -- the picked-up weapon
	// keeps showing its default (no-skin) render regardless of what gets applied to "current" the
	// player. Firearm definitions in the "weapon" slot follow that econ owner; knife definitions
	// (including weapon aliases), gloves and agents stay keyed to the spectated player.
	uint64_t id = TokenIs(slot, "weapon")
		? ResolveWeaponSteamIdArg(cmd, steamToken, atoi(args->ArgV(5)))
		: ResolveSteamIdArg(cmd, steamToken);
	if (id == 0)
		return;

	if (TokenIs(slot, "weapon")) {
		int defIndex = atoi(args->ArgV(5));
		int paintKit = 0;
		float wear = 0.0f;
		int seed = 0;
		int statTrak = -1;
		ParseItemKeyValueArgs(argc, args, 6, &paintKit, &wear, &seed, &statTrak);
		// UI skin-click diagnostics: log what the UI requested + the game's live read of the weapon
		// BEFORE the override, apply it NOW (so the skin changes immediately while the weapon is up),
		// then log the live read AFTER. RebuildOnce is the same immediate-apply the user sees; the crash
		// being chased happens later on a quick weapon SWITCH (entity recreation), not here.
		advancedfx::Message("%s cosmetics uiclick request: slot=weapon def=%d paint=%d wear=%.4f seed=%d stattrak=%d (what the UI sent).\n",
			cmd, defIndex, paintKit, wear, seed, statTrak);
		Cosmetics_LogWeaponPick(id, defIndex, paintKit, wear, seed, statTrak);
		if (CosmeticsRef().DeferWeaponPickWhilePlaying(id, defIndex, paintKit, wear, seed, statTrak)) {
			CosmeticsRef().Arm();
			advancedfx::Message("%s cosmetics: weapon pick def=%d paint=%d made during playback -- pausing to apply it, "
				"playback resumes automatically.\n", cmd, defIndex, paintKit);
			return;
		}
		Cosmetics_LogWeaponSnapshot(cmd, "before", id, defIndex);
		CosmeticsRef().SetWeapon(id, defIndex, paintKit, wear, seed, statTrak);
		advancedfx::Message("%s cosmetics: weapon steamId=%llu def=%d paint=%d wear=%.4f seed=%d stattrak=%d.\n",
			cmd, (unsigned long long)id, defIndex, paintKit, wear, seed, statTrak);
		// scheduleTickNudge=true: the first-person viewmodel mirror (RefreshViewmodelWeapons) needs a
		// few actually-rendered frames for its SetMeshGroupMask/PostDataUpdate to visually take -- while
		// paused, RebuildOnce()'s immediate re-apply below writes the data but the composited material
		// doesn't visibly refresh until real frames run. Confirmed live: applying a paint while paused
		// showed no change at all until a manual tick-skip advanced a few frames, after which it rendered
		// correctly. The nudge (auto play ~10 ticks, then re-pause) is the existing, already-proven fix
		// for this exact class of "needs real frames" problem (used by agent/glove/knife swaps below).
		AfterMutation(cmd, true);
		MvmCrashWatch_Arm(-1, "weapon-paint-apply");
		CosmeticsRef().RebuildOnce(); // apply the override to live weapons NOW so the change shows immediately
		Cosmetics_LogWeaponSnapshot(cmd, "after", id, defIndex);
	} else if (TokenIs(slot, "knife")) {
		int defIndex = atoi(args->ArgV(5));
		int paintKit = 0;
		float wear = 0.0f;
		int seed = 0;
		int statTrak = -1; // unused for knives, parsed for symmetry/leniency only
		ParseItemKeyValueArgs(argc, args, 6, &paintKit, &wear, &seed, &statTrak);
		advancedfx::Message("%s cosmetics uiclick request: slot=knife def=%d paint=%d wear=%.4f seed=%d (what the UI sent).\n",
			cmd, defIndex, paintKit, wear, seed);
		// Snapshot the active knife (targetDef=0, since a type swap rewrites the live def). Apply NOW so
		// the knife changes immediately while it is up; the crash being chased is the later re-swap onto
		// the entity the engine recreates during a quick weapon switch (instrumented in the swap path).
		// Same as weapons: a type swap + composite made on live frames leaves the new knife unpainted, so
		// a pick during playback pauses, commits on the paused frame and resumes.
		if (CosmeticsRef().DeferWeaponPickWhilePlaying(id, defIndex, paintKit, wear, seed, -1, /*knife=*/true)) {
			CosmeticsRef().Arm();
			advancedfx::Message("%s cosmetics: knife pick def=%d paint=%d made during playback -- pausing to apply it, "
				"playback resumes automatically.\n", cmd, defIndex, paintKit);
			return;
		}
		Cosmetics_LogWeaponSnapshot(cmd, "before", id, 0);
		CosmeticsRef().SetKnife(id, defIndex, paintKit, wear, seed);
		advancedfx::Message("%s cosmetics: knife steamId=%llu def=%d paint=%d wear=%.4f seed=%d.\n",
			cmd, (unsigned long long)id, defIndex, paintKit, wear, seed);
		AfterMutation(cmd, true);
		CosmeticsRef().RebuildOnce(); // apply NOW so the knife change shows immediately
		Cosmetics_LogWeaponSnapshot(cmd, "after", id, 0);
	} else if (TokenIs(slot, "gloves")) {
		int defIndex = atoi(args->ArgV(5));
		int paintKit = 0;
		float wear = 0.0f;
		int seed = 0;
		int statTrak = -1; // unused for gloves, parsed for symmetry/leniency only
		ParseItemKeyValueArgs(argc, args, 6, &paintKit, &wear, &seed, &statTrak);
		// Gloves are a PAWN body-level item (not an econ weapon entity), so the weapon snapshot does
		// not apply; the verbatim name still comes through the JS "uilog" line and the apply happens via
		// the nudge in AfterMutation.
		advancedfx::Message("%s cosmetics uiclick request: slot=gloves def=%d paint=%d wear=%.4f seed=%d (what the UI sent).\n",
			cmd, defIndex, paintKit, wear, seed);
		Cosmetics_LogGlovePick(id, defIndex, paintKit, wear, seed);
		CosmeticsRef().SetGloves(id, defIndex, paintKit, wear, seed);
		if (MvmDebugLog_Active()) {
			char data[256];
			std::snprintf(data, sizeof(data),
				"\"steamId\":%llu,\"wantDef\":%d,\"wantPaint\":%d,\"wantWear\":%.4f,\"wantSeed\":%d",
				(unsigned long long)id, defIndex, paintKit, wear, seed);
			MvmAgentLog("H2", "CosmeticCommands.cpp:SetGloves", "profile_set", data);
		}
		advancedfx::Message("%s cosmetics: gloves steamId=%llu def=%d paint=%d wear=%.4f seed=%d.\n",
			cmd, (unsigned long long)id, defIndex, paintKit, wear, seed);
		AfterMutation(cmd, true);
	} else if (TokenIs(slot, "agent")) {
		const char* model = args->ArgV(5);
		if (0 != _stricmp(model, "default") && !IsValidAgentModelPath(model)) {
			advancedfx::Warning("%s cosmetics: rejected invalid agent model path '%s'.\n", cmd, model);
			return;
		}
		CosmeticsRef().SetAgent(id, model, 0);
		advancedfx::Message("%s cosmetics: agent steamId=%llu model='%s'.\n", cmd, (unsigned long long)id, model);
		AfterMutation(cmd, true);
	} else {
		advancedfx::Warning("%s cosmetics player: unknown slot '%s' (expected weapon|knife|gloves|agent).\n", cmd, slot);
	}
}

// Team of an agent model, read from its path. CS2 ships agents under agents/models/<family>/...,
// where the family prefix is "ctm_" for CT and "tm_" for T. Order matters: "ctm_" ends with "tm_",
// so a naive contains() check calls every CT agent a T agent.
int AgentModelTeam(const char* modelPath) {
	if (!modelPath) return 0;
	const char* p = std::strstr(modelPath, "agents/models/");
	p = p ? p + 14 : modelPath;
	if (0 == _strnicmp(p, "ctm_", 4)) return 3; // CT
	if (0 == _strnicmp(p, "tm_", 3)) return 2;  // T
	return 0; // custom or unrecognised: apply to everyone rather than guess a team
}

// "cosmetics allplayers agent <modelPath|default>" -- the report's "change every player's Agent at
// once", which until now was one command per player, read off the scoreboard by hand.
//
// Deliberately team-aware: a T model on a CT player is not what "change everyone" means, and the
// swap either looks wrong or is refused downstream. A model whose team can be read from its path is
// applied ONLY to that team, and the skipped players are reported rather than silently ignored.
// "default" clears the override for everyone, both teams.
// Agent pool for the randomiser. The agent catalog is compiled into the Panorama UI, not into
// the DLL (see Cosmetics/CosmeticCatalog.h), so rather than duplicate 80-odd model paths in C++
// and let the two lists drift, the UI registers the pool it is already showing and the backend --
// which is the side that can enumerate players -- does the per-player pick.
std::vector<std::string> g_agentPool;

// Deterministic per-call sequence, seeded once from the clock. Filming wants a different roll
// each time the user presses the button, but not a different one every frame, so the state is
// advanced explicitly and never re-seeded mid-batch.
uint64_t NextRandom() {
	static uint64_t s = 0;
	if (s == 0) s = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() | 1ull;
	s ^= s << 13; s ^= s >> 7; s ^= s << 17;
	return s;
}

void DoAgentPool(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* action = (argc >= 4) ? args->ArgV(3) : "";
	if (TokenIs(action, "clear")) {
		g_agentPool.clear();
		return; // silent: the UI clears then refills on every randomise click
	}
	if (!TokenIs(action, "add")) {
		advancedfx::Warning("usage: %s cosmetics agentpool clear | add <modelPath> [<modelPath> ...]\n", cmd);
		return;
	}
	int added = 0;
	for (int i = 4; i < argc; ++i) {
		const char* model = args->ArgV(i);
		if (!model || !*model || !IsValidAgentModelPath(model)) continue;
		g_agentPool.push_back(model);
		++added;
	}
	if (added == 0)
		advancedfx::Warning("%s cosmetics agentpool: no valid model paths in that batch.\n", cmd);
}

void DoAllPlayers(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 5) {
		advancedfx::Warning("usage: %s cosmetics allplayers agent <modelPath|default>\n", cmd);
		return;
	}
	const char* slot = args->ArgV(3);
	if (!TokenIs(slot, "agent")) {
		advancedfx::Warning("%s cosmetics allplayers: only 'agent' is supported (got '%s').\n", cmd, slot);
		return;
	}
	const char* model = args->ArgV(4);
	const bool clearing = (0 == _stricmp(model, "default"));
	const bool randomise = (0 == _stricmp(model, "random"));

	// "random" gives every player their OWN agent instead of one model for the whole server --
	// what you actually want when the point is to stop ten players looking identical. The pick is
	// still per-team, drawn only from pool entries whose path says they belong to that player's
	// side, so a T never ends up in a CT model.
	if (randomise) {
		if (g_agentPool.empty()) {
			advancedfx::Warning("%s cosmetics allplayers agent random: the agent pool is empty "
				"(the UI registers it with 'cosmetics agentpool add ...').\n", cmd);
			return;
		}
		CosmeticPlayerRef rp[64];
		int rcount = 0;
		CosmeticEnumeratePlayers(rp, 64, &rcount);
		if (rcount <= 0) {
			advancedfx::Warning("%s cosmetics allplayers: no players resolved (is a demo playing?).\n", cmd);
			return;
		}
		int rapplied = 0, rskipped = 0;
		for (int i = 0; i < rcount; ++i) {
			// Collect this player's side of the pool, then pick one. Building the candidate list
			// per player (rather than picking blind and retrying) means a lopsided pool cannot
			// spin, and a side with no entries at all is reported instead of silently skipped.
			std::vector<const std::string*> mine;
			for (size_t k = 0; k < g_agentPool.size(); ++k) {
				const int t = AgentModelTeam(g_agentPool[k].c_str());
				if (t == 0 || t == rp[i].team) mine.push_back(&g_agentPool[k]);
			}
			if (mine.empty()) { ++rskipped; continue; }
			const std::string* pick = mine[(size_t)(NextRandom() % (uint64_t)mine.size())];
			if (CosmeticsRef().SetPlayerAgent(rp[i], pick->c_str())) ++rapplied;
			else ++rskipped;
		}
		advancedfx::Message("%s cosmetics: random agents applied to %d player%s%s (pool=%d).\n",
			cmd, rapplied, rapplied == 1 ? "" : "s",
			rskipped ? " (some had no candidate for their team)" : "", (int)g_agentPool.size());
		if (rapplied > 0)
			AfterMutation(cmd, true);
		return;
	}

	if (!clearing && !IsValidAgentModelPath(model)) {
		advancedfx::Warning("%s cosmetics allplayers: rejected invalid agent model path '%s'.\n", cmd, model);
		return;
	}

	CosmeticPlayerRef players[64];
	int count = 0;
	CosmeticEnumeratePlayers(players, 64, &count);
	if (count <= 0) {
		advancedfx::Warning("%s cosmetics allplayers: no players resolved (is a demo playing?).\n", cmd);
		return;
	}

	const int wantTeam = clearing ? 0 : AgentModelTeam(model);
	int applied = 0, skipped = 0;
	for (int i = 0; i < count; ++i) {
		if (wantTeam != 0 && players[i].team != wantTeam) { ++skipped; continue; }
		if (CosmeticsRef().SetPlayerAgent(players[i], model)) ++applied;
	}

	advancedfx::Message("%s cosmetics: agent '%s' applied to %d player%s%s.\n",
		cmd, model, applied, applied == 1 ? "" : "s",
		skipped ? (wantTeam == 3 ? " (T players skipped: this is a CT model)"
			: " (CT players skipped: this is a T model)") : "");
	if (applied > 0)
		AfterMutation(cmd, true); // one nudge for the whole batch, not one per player
}


// Visual-cache rebuild (the refresh path). "rebuild once" re-asserts the visuals-stale field writes
// on every matched weapon entity right now (decoupled from the per-frame change detection) so the
// user can manually retrigger a refresh attempt for a screenshot; "rebuild auto 0|1" gates the
// per-frame stale-marking. See CosmeticOverrideSystem.h / RebuildOnce / SetRebuildAuto.
void PrintRebuildFlags(const char* cmd) {
	const CosmeticRebuildFlags& f = CosmeticsRef().GetRebuildFlags();
	advancedfx::Message("%s cosmetics rebuildflags: visualsdata=%d clearugc=%d reloadevent=%d "
		"initialized=%d attrinit=%d imagecache=%d attrparity=%d\n",
		cmd, f.visualsDataSet ? 1 : 0, f.clearUgc ? 1 : 0, f.reloadEvent ? 1 : 0,
		f.initialized ? 1 : 0, f.attrInit ? 1 : 0, f.imageCache ? 1 : 0, f.attrParity ? 1 : 0);
}

void DoRebuild(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* mode = (argc >= 4) ? args->ArgV(3) : "";
	if (TokenIs(mode, "once")) {
		int n = CosmeticsRef().RebuildOnce();
		advancedfx::Message("%s cosmetics: rebuild once -> touched %d matched weapon entit%s%s.\n",
			cmd, n, (n == 1 ? "y" : "ies"),
			CosmeticsRef().Recompose() ? " (+ fired recompose vcall, armed)" : "");
		PrintRebuildFlags(cmd);
		if (n == 0)
			advancedfx::Warning("%s cosmetics: rebuild once touched nothing -- needs a demo playing, a "
				"stored profile for the spectated owner, and resolved offsets.\n", cmd);
	} else if (TokenIs(mode, "auto")) {
		if (argc >= 5)
			CosmeticsRef().SetRebuildAuto(0 != atoi(args->ArgV(4)));
		advancedfx::Message("%s cosmetics: rebuild auto = %d (per-frame writing of the ENABLED rebuildflags on change).\n",
			cmd, CosmeticsRef().RebuildAuto() ? 1 : 0);
	} else {
		advancedfx::Warning("usage: %s cosmetics rebuild once | auto 0|1\n", cmd);
	}
}

// Toggle the individual visuals-stale field writes (all default OFF -> HUD-safe). The "clearugc" and
// "initialized" writes are the weapon-vanishes-from-the-switch-bar suspects; enable one at a time and
// "rebuild once" to bisect which breaks the HUD vs. which (if any) triggers a visual rebuild.
void DoRebuildFlags(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 5) {
		const char* name = args->ArgV(3);
		bool on = 0 != atoi(args->ArgV(4));
		if (TokenIs(name, "all")) {
			CosmeticsRef().SetAllRebuildFlags(on);
		} else if (!CosmeticsRef().SetRebuildFlag(name, on)) {
			advancedfx::Warning("%s cosmetics: unknown rebuild flag '%s' "
				"(visualsdata|clearugc|reloadevent|initialized|attrinit|imagecache|attrparity|all).\n", cmd, name);
			return;
		}
	} else if (argc == 4) {
		advancedfx::Warning("usage: %s cosmetics rebuildflags <name> 0|1   (or 'all 0|1'); no value = list\n", cmd);
		return;
	}
	PrintRebuildFlags(cmd);
	advancedfx::Message("  (all OFF = HUD-safe default; clearugc/initialized blank the HUD weapon icon)\n");
}

void DoPaintkitBridge(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4) {
		const char* mode = args->ArgV(3);
		if (TokenIs(mode, "force")) {
			if (argc < 5) {
				advancedfx::Warning("usage: %s cosmetics paintkitbridge force <paintKit>\n", cmd);
				return;
			}
			int paintKit = atoi(args->ArgV(4));
			if (paintKit <= 0) {
				advancedfx::Warning("%s cosmetics paintkitbridge: force paintKit must be > 0; use 'auto' or '0' to clear.\n", cmd);
				return;
			}
			CosmeticsRef().SetPaintkitBridgeForcedValue(paintKit);
			CosmeticsRef().SetPaintkitBridge(true);
		} else if (TokenIs(mode, "auto")) {
			CosmeticsRef().SetPaintkitBridgeForcedValue(-1);
			CosmeticsRef().SetPaintkitBridge(true);
		} else {
			CosmeticsRef().SetPaintkitBridge(0 != atoi(mode));
		}
	}

	advancedfx::Message("%s cosmetics: paintkitbridge=%d cvarFound=%d value=%d forced=%d\n",
		cmd,
		CosmeticsRef().PaintkitBridge() ? 1 : 0,
		CosmeticsRef().PaintkitBridgeCvarFound() ? 1 : 0,
		CosmeticsRef().PaintkitBridgeLastValue(),
		CosmeticsRef().PaintkitBridgeForcedValue());
	advancedfx::Message("  experimental: uses global cl_paintkit_override, affects only next weapon deploy/create, "
		"and is not per-player. Disable to restore the previous cvar value.\n");
}

// The direct-composite path is constrained in the apply loop to paused demos and the deployed weapon.
void DoAutoComposite(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4)
		CosmeticsRef().SetAutoComposite(0 != atoi(args->ArgV(3)));
	advancedfx::Message("%s cosmetics: autocomposite = %d (paused, deployed weapon only).\n",
		cmd, CosmeticsRef().AutoComposite() ? 1 : 0);
}

// Master toggle for the client-side model-swap path (agent / gloves / legacy mesh / knife type). Knife
// TYPE swaps are separately toggleable because they mutate weapon subclass/model state and have been
// crash-prone on weapon switch in demo playback; both default ON, disable with "modelswap knife 0" if a
// demo's weapon switches destabilize.
// Tick-nudge: after a cosmetic change, re-seek the demo so the engine recreates entities and runs
// real frames -- the user-requested lever that makes model/glove/agent/mesh swaps pop without a
// manual scrub. The direct composite chain refreshes the weapon SKIN in place; body/model swaps
// (agent/glove/knife type/mesh) need real rendered frames, so this briefly resumes then re-pauses.
void DoTickNudge(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4) {
		const char* a = args->ArgV(3);
		if (TokenIs(a, "on")) CosmeticsRef().SetTickNudge(true);
		else if (TokenIs(a, "off")) CosmeticsRef().SetTickNudge(false);
		else { CosmeticsRef().SetTickNudgeTicks(atoi(a)); CosmeticsRef().SetTickNudge(true); }
	}
	advancedfx::Message("%s cosmetics: ticknudge=%d ticks=%d totalNudges=%llu (after a change, briefly "
		"play the demo so agent/glove/knife/mesh swaps re-render).\n", cmd, CosmeticsRef().TickNudge() ? 1 : 0,
		CosmeticsRef().TickNudgeTicks(), (unsigned long long)CosmeticsRef().TotalNudges());
}

// REMOVED: `cosmetics previewnudge`. It briefly resumed and re-paused the demo so the Customize 3D
// preview panel would composite and its animgraph would advance. That was a workaround with a real
// cost -- it moved playback underneath a UI that said paused -- and it is now unnecessary: the
// preview's animgraph is advanced directly by Filmmaker/Panorama/PreviewAnimTick.cpp, which grants a
// wall-clock tick delta to the preview pawns only and never touches the demo clock.

void DoModelSwap(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4) {
		if (TokenIs(args->ArgV(3), "knife")) {
			if (argc >= 5)
				CosmeticsRef().SetKnifeModelSwap(0 != atoi(args->ArgV(4)));
		} else {
			CosmeticsRef().SetModelSwap(0 != atoi(args->ArgV(3)));
		}
	}
	advancedfx::Message("%s cosmetics: modelswap = %d knifeType=%d (agent/gloves/legacy mesh + knife type; both default on). resolved=%d.\n",
		cmd, CosmeticsRef().ModelSwap() ? 1 : 0, CosmeticsRef().KnifeModelSwap() ? 1 : 0,
		CosmeticsRef().ModelSwapResolved() ? 1 : 0);
	if (!CosmeticsRef().ModelSwapResolved())
		advancedfx::Warning("%s cosmetics: model-swap client.dll functions did NOT all resolve; knife/agent/glove "
			"swaps will be skipped (paint-only still works).\n", cmd);
}

// Experimental animgraph reset (approach #1): after a knife SetModel, null the entity's
// CAnimationGraphInstance->pAnimGraphNetworkedVariables so the graph rebuilds for the new model. The
// reference offsets (0xD08/0x2E0) are for the local viewmodel; the demo world weapon differs, so they are
// live-tunable here. Watch the "knife.swap step=animreset.world" breadcrumb: inst=0 means the instance
// offset is wrong for the world weapon (tune with "animreset offsets <instHex> <varsHex>").
void DoAnimReset(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4) {
		if (TokenIs(args->ArgV(3), "offsets")) {
			if (argc < 6) {
				advancedfx::Warning("usage: %s cosmetics animreset offsets <instOffsetHex> <varsOffsetHex>\n", cmd);
				return;
			}
			uint32_t instOff = (uint32_t)strtoul(args->ArgV(4), nullptr, 0);
			uint32_t varsOff = (uint32_t)strtoul(args->ArgV(5), nullptr, 0);
			SetAnimGraphResetOffsets(instOff, varsOff);
		} else {
			SetAnimGraphReset(0 != atoi(args->ArgV(3)));
		}
	}
	uint32_t instOff = 0, varsOff = 0;
	GetAnimGraphResetOffsets(&instOff, &varsOff);
	advancedfx::Message("%s cosmetics: animreset=%d instOff=0x%x varsOff=0x%x "
		"(experimental: null entity animgraph networked-vars after a knife SetModel; see knife.swap animreset breadcrumb).\n",
		cmd, AnimGraphReset() ? 1 : 0, instOff, varsOff);
}

// Approach #2 (root-cause knife-crash fix): blocking-precache the target model before SetModel so the
// engine's async anim pass finds non-null per-model anim data instead of crashing (see
// Docs/cosmetics/cosmetics-cs2-methodology-notes.md §11). Default ON. Watch the "knife.swap step=precache" breadcrumb:
// fired=1 means the blocking load ran; a clean swap with NO "crash.veh client.dll" line afterward = fixed.
void DoPrecache(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4)
		SetPrecacheModels(0 != atoi(args->ArgV(3)));
	advancedfx::Message("%s cosmetics: precache = %d (blocking-load the knife/agent model before SetModel; "
		"the root-cause fix for the knife-type-swap crash).\n", cmd, PrecacheModels() ? 1 : 0);
}

// Approach #3 (the working knife-crash fix): a client.dll detour that substitutes an EMPTY sequence list
// when the engine's anim builder returns a null out-param for an unloaded model (see CosmeticAnimFix.h).
// Default ON; installs lazily on the first knife swap. "animfix 0" makes it a pass-through so the crash
// reproduces (A/B). Reports install state + how many null out-params it has neutralized.
void DoAnimFix(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	EnsureAnimCrashFixInstalled(); // install now so 'animfix' alone reports a meaningful state
	if (argc >= 4)
		SetAnimCrashFix(0 != atoi(args->ArgV(3)));
	advancedfx::Message("%s cosmetics: animfix=%d installed=%d fills=%llu "
		"(detours client.dll anim builder; substitutes an empty seq-list for a null out-param -> "
		"knife-type swaps to unloaded models no longer crash).\n",
		cmd, AnimCrashFix() ? 1 : 0, AnimCrashFixInstalled() ? 1 : 0,
		(unsigned long long)AnimCrashFixFills());
	if (!AnimCrashFixInstalled())
		advancedfx::Warning("%s cosmetics: animfix could NOT install (pattern miss); knife-type swaps to "
			"unloaded models will still crash.\n", cmd);
}

// Legacy-vs-CS2 mesh-group selection tuning. "mesh" with no args prints the state; "mesh auto|modern|legacy"
// sets the mode; "mesh masks <modern> <legacy>" sets the bit values (defaults 1/2). See SetMeshMasks.
void DoMesh(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4) {
		const char* mode = args->ArgV(3);
		if (TokenIs(mode, "lookup")) {
			// "mesh lookup <paintKit> [...]": what the econ schema says about each finish.
			for (int i = 4; i < argc; ++i) {
				const int pk = atoi(args->ArgV(i));
				const int legacy = PaintKitLegacyModel(pk);
				advancedfx::Message("%s cosmetics: mesh lookup paint=%d -> %s\n", cmd, pk,
					legacy == 1 ? "legacy" : (legacy == 0 ? "modern (CS2)" : "unknown"));
			}
		} else if (TokenIs(mode, "auto")) CosmeticsRef().SetMeshLegacyMode(-2);
		else if (TokenIs(mode, "modern")) CosmeticsRef().SetMeshLegacyMode(-1);
		else if (TokenIs(mode, "legacy")) CosmeticsRef().SetMeshLegacyMode(1);
		else if (TokenIs(mode, "masks")) {
			if (argc < 6) {
				advancedfx::Warning("usage: %s cosmetics mesh masks <modernMask> <legacyMask>\n", cmd);
				return;
			}
			CosmeticsRef().SetMeshMasks((uint64_t)_strtoui64(args->ArgV(4), nullptr, 0),
				(uint64_t)_strtoui64(args->ArgV(5), nullptr, 0));
		} else {
			advancedfx::Warning("usage: %s cosmetics mesh [auto|modern|legacy | masks <modern> <legacy> | lookup <paintKit>...]\n", cmd);
			return;
		}
	}
	const char* modeStr = CosmeticsRef().MeshLegacyMode() == -2 ? "auto"
		: CosmeticsRef().MeshLegacyMode() == -1 ? "modern" : "legacy";
	advancedfx::Message("%s cosmetics: mesh mode=%s maskModern=%llu maskLegacy=%llu "
		"(auto reads the paint kit's IsUseLegacyModel from the econ schema).\n",
		cmd, modeStr, (unsigned long long)CosmeticsRef().MaskModern(),
		(unsigned long long)CosmeticsRef().MaskLegacy());
	char layout[320];
	DescribePaintKitLayout(layout, sizeof(layout));
	advancedfx::Message("%s cosmetics: mesh paintkit layout: %s\n", cmd, layout);
}

void DoComposite(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* mode = (argc >= 4) ? args->ArgV(3) : "";
	if (TokenIs(mode, "stages")) {
		if (argc >= 5)
			SetDirectCompositeStageMask((uint32_t)strtoul(args->ArgV(4), nullptr, 0));
		advancedfx::Message("%s cosmetics: composite stages=0x%02x "
			"(1=attrs 2=owner 4=set 8=skin; 15=all-safe).\n",
			cmd, GetDirectCompositeStageMask());
		return;
	}
	if (!TokenIs(mode, "once")) {
		advancedfx::Warning("usage: %s cosmetics composite [once [ownerOffsetHex] | stages <mask>]\n", cmd);
		return;
	}
	if (argc >= 5) {
		char* end = nullptr;
		long long offset = _strtoi64(args->ArgV(4), &end, 0);
		if (end == args->ArgV(4) || offset <= 0) {
			advancedfx::Warning("%s cosmetics composite: invalid owner offset '%s' (example: 0x608).\n",
				cmd, args->ArgV(4));
			return;
		}
		CosmeticsRef().SetCompositeOwnerOffset((ptrdiff_t)offset);
	}

	int n = CosmeticsRef().CompositeOnce();
	const CosmeticFrameStats& s = CosmeticsRef().LastFrameStats();
	advancedfx::Message("%s cosmetics: composite once -> touched %d matched weapon entit%s, "
		"direct calls=%d faulted=%d (direct refresh requires a paused, deployed weapon).\n",
		cmd, n, (n == 1 ? "y" : "ies"), s.directCompositeCalls, s.directCompositeFaulted);
	if (n == 0)
		advancedfx::Warning("%s cosmetics: composite once touched nothing -- needs a demo playing, a "
			"stored profile for the spectated owner, and resolved offsets.\n", cmd);
}

// Forced material re-composite tuning (the refresh path). OFF by default; the correct CS2
// UpdateComposite vtable index is build-specific, so it is live-tunable here. See
// CosmeticOverrideSystem.h / SetRecompose.
void DoRecompose(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4)
		CosmeticsRef().SetRecompose(0 != atoi(args->ArgV(3)));
	advancedfx::Message("%s cosmetics: recompose=%d faulted=%d.\n", cmd,
		CosmeticsRef().Recompose() ? 1 : 0, CosmeticsRef().RecomposeFaulted() ? 1 : 0);
}

void DoVtIdx(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 5) {
		advancedfx::Warning("usage: %s cosmetics vtidx <compositeIdx> <secIdx>  (-1 = skip a call)\n", cmd);
		return;
	}
	CosmeticsRef().SetVtIdx(atoi(args->ArgV(3)), atoi(args->ArgV(4)));
	int comp = 0, sec = 0;
	CosmeticsRef().GetVtIdx(&comp, &sec);
	advancedfx::Message("%s cosmetics: vtidx comp=%d sec=%d (recompose re-armed).\n", cmd, comp, sec);
}

// Opt-in legacy fallback-id mode: forces C_EconItemView::m_iItemIDHigh = -1. Default OFF -- it breaks
// the UI inventory read and is unnecessary now that the networked attributes are overwritten directly.
void DoFallback(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4)
		CosmeticsRef().SetFallbackId(0 != atoi(args->ArgV(3)));
	advancedfx::Message("%s cosmetics: fallback (itemIDHigh=-1) = %d.\n", cmd, CosmeticsRef().FallbackId() ? 1 : 0);
}

// Sets the argument passed to the recompose vtable call. 1 = default; 0 lets us test
// OnDataChanged(DATA_UPDATE_CREATED), the candidate client-side skin-rebuild trigger.
void DoVtArg(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 4)
		CosmeticsRef().SetVtArg(atoi(args->ArgV(3)));
	advancedfx::Message("%s cosmetics: vtarg = %d (vtable call arg; 0 = DATA_UPDATE_CREATED).\n",
		cmd, CosmeticsRef().VtArg());
}

// One-shot probe of a single candidate vtable index, for bisecting which slot is the weapon's
// OnDataChanged (the real client-side skin-rebuild trigger -- see the 2026-06-28 binary-analysis
// section of Docs/archive/cosmetics-recompose-research.md). Static analysis of the live client.dll narrowed
// OnDataChanged to a small set of DataUpdateType_t-taking virtuals on the C_CSWeaponBase vtable
// (candidates: 4, 11, 15, 18, 70, 108, 110, 124, 126); this command tries one of them per call so a
// screenshot can attribute any visual rebuild to a specific index. It sets the visuals-stale marks
// (m_bVisualsDataSet=false + m_nCustomEconReloadEventId bump) BEFORE the call -- so OnDataChanged
// sees the visuals as stale -- then SEH-guarded-calls weapon->vtable[idx](this, 0) on every matched
// weapon. arg 0 == DATA_UPDATE_CREATED (the leak's branch that unconditionally rebuilds the skin).
// A wrong index that access-violates is caught (faulted=1) instead of crashing the game.
void DoVtProbe(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 4) {
		advancedfx::Warning("usage: %s cosmetics vtprobe <vtableIndex>   (calls weapon vtable[idx](this,0) = "
			"OnDataChanged(DATA_UPDATE_CREATED) after econ write + visuals-stale marks; candidates "
			"4,11,15,18,70,108,110,124,126)\n", cmd);
		return;
	}
	int idx = atoi(args->ArgV(3));
	CosmeticOverrideSystem& sys = CosmeticsRef();
	sys.SetRebuildFlag("visualsdata", true);  // m_bVisualsDataSet = false (mark visuals stale)
	sys.SetRebuildFlag("reloadevent", true);  // bump m_nCustomEconReloadEventId (the networked reload token)
	sys.SetVtArg(0);                           // DATA_UPDATE_CREATED
	sys.SetVtIdx(idx, -1);                     // probe this index only (secondary skipped); re-arms recompose
	int n = sys.RebuildOnce();                 // writes the stale marks, then fires weapon->vtable[idx](this,0)
	advancedfx::Message("%s cosmetics: vtprobe idx=%d -> touched %d weapon(s), recompose=%d faulted=%d "
		"(arg=0=DATA_UPDATE_CREATED; stale marks: visualsdata+reloadevent).\n",
		cmd, idx, n, sys.Recompose() ? 1 : 0, sys.RecomposeFaulted() ? 1 : 0);
	if (n == 0)
		advancedfx::Warning("%s cosmetics: vtprobe touched nothing -- need a demo playing, a stored profile for "
			"the spectated owner, and resolved offsets.\n", cmd);
	if (sys.RecomposeFaulted())
		advancedfx::Warning("%s cosmetics: vtprobe idx=%d FAULTED (access violation, caught) -- not a callable "
			"(this,int) virtual; try the next candidate index.\n", cmd, idx);
}

// Self-service debug log: start writes a timestamped file under %APPDATA%\HLAE\debuglogs\ that the
// cosmetics apply loop fills with per-event + heartbeat lines (wall time + demo tick + frame); stop
// closes it and prints the folder/file path so the user can paste it into File Explorer and share it.
void DoDebugLog(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* mode = (argc >= 4) ? args->ArgV(3) : "";
	if (TokenIs(mode, "start")) {
		std::string file;
		if (MvmDebugLog_Start(&file)) {
			const bool consoleCapture = MvmConsoleCapture_Start();
			MvmDebugLog_Linef("command", "%s cosmetics debuglog start", cmd);
			MvmDebugLog_Linef("system", "fullConsoleCapture=%d", consoleCapture ? 1 : 0);
			advancedfx::Message("%s cosmetics: debug log STARTED. file: %s ('%s cosmetics debuglog stop' or 'mvm_debug stop' when done).\n", cmd, file.c_str(), cmd);
		} else
			advancedfx::Warning("%s cosmetics: debug log already running (or file could not be opened).\n", cmd);
	} else if (TokenIs(mode, "stop")) {
		std::string folder, file;
		MvmDebugLogStats stats;
		MvmConsoleCapture_Stop();
		if (MvmDebugLog_Stop(&folder, &file, &stats)) {
			advancedfx::Message("%s cosmetics: debug log STOPPED. captured %llu events; combined %llu repeats.\n",
				cmd, (unsigned long long)stats.eventsReceived,
				(unsigned long long)stats.repeatsCombined);
			advancedfx::Message("  file:   %s\n", file.c_str());
			advancedfx::Message("  folder: %s\n", folder.c_str());
		} else {
			advancedfx::Warning("%s cosmetics: no debug log is running.\n", cmd);
		}
	} else {
		const MvmDebugLogStats stats = MvmDebugLog_GetStats();
		advancedfx::Message("%s cosmetics: debuglog is %s (events=%llu unique=%llu repeats_combined=%llu). "
			"Use '%s cosmetics debuglog start|stop' (or 'mvm_debug start|stop|status').\n",
			cmd, MvmDebugLog_Active() ? "RUNNING" : "stopped",
			(unsigned long long)stats.eventsReceived,
			(unsigned long long)stats.uniqueEventsWritten,
			(unsigned long long)stats.repeatsCombined, cmd);
	}
}

// Verbatim UI passthrough: the Customize panel sends the EXACT skin label the user clicked (e.g.
// "AK-47 | Redline", "Skeleton Knife") so the human-readable name appears in the console + MVM debug
// log right next to the before/after weapon snapshot. The label arrives as argv[3..] (a quoted label
// is one token; an unquoted one is several) -- joined back with single spaces. Pure logging, no state.
void DoUiLog(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	std::string text;
	for (int i = 3; i < argc; ++i) {
		const char* tok = args->ArgV(i);
		if (!tok || !*tok)
			continue;
		if (!text.empty())
			text += ' ';
		text += tok;
	}
	if (text.empty())
		text = "(empty)";
	advancedfx::Message("%s cosmetics uiclick label: %s\n", cmd, text.c_str());
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.uiclick", "label %s", text.c_str());
	Cosmetics_StorePendingUiGloveLabel(text.c_str());
}

} // namespace

void Cosmetics_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 3) ? args->ArgV(2) : "";

	if (TokenIs(sub, "enabled")) {
		DoEnabled(argc, args, cmd);
	} else if (TokenIs(sub, "status")) {
		Cosmetics_PrintStatus(cmd);
	} else if (TokenIs(sub, "recorded")) {
		CosmeticsRef().PrintRecordedWeapons(argc >= 4 ? (uint32_t)strtoul(args->ArgV(3), nullptr, 0) : ~0u);
	} else if (TokenIs(sub, "debug")) {
		DoDebug(argc, args, cmd);
	} else if (TokenIs(sub, "clear")) {
		DoClear(cmd);
	} else if (TokenIs(sub, "clearPlayer")) {
		DoClearPlayer(argc, args, cmd);
	} else if (TokenIs(sub, "clearWeapon")) {
		DoClearWeapon(argc, args, cmd);
	} else if (TokenIs(sub, "target")) {
		DoTarget(argc, args, cmd);
	} else if (TokenIs(sub, "player")) {
		DoPlayer(argc, args, cmd);
	} else if (TokenIs(sub, "agentpool")) {
		DoAgentPool(argc, args, cmd);
	} else if (TokenIs(sub, "allplayers")) {
		DoAllPlayers(argc, args, cmd);
	} else if (TokenIs(sub, "diag")) {
		Cosmetics_PrintSpectatedDebug(cmd);
	} else if (TokenIs(sub, "visualdiag")) {
		Cosmetics_PrintVisualDiag(cmd);
	} else if (TokenIs(sub, "spectate")) {
		if (argc < 4) {
			advancedfx::Warning("%s cosmetics spectate: usage: spectate <steamId|current>\n", cmd);
			return;
		}
		uint64_t id = ResolveSteamIdArg(cmd, args->ArgV(3));
		if (id != 0)
			Cosmetics_SpectateSteamId(cmd, id);
	} else if (TokenIs(sub, "skinlog")) {
		// Force one LIVE per-player skin-state snapshot into the mvm_debug log right now. (The same
		// snapshot fires automatically on player switch / seek / weapon / loadout change while
		// mvm_debug is running -- see Cosmetics_TickSkinStateLog.)
		Cosmetics_LogLiveSkinState("manual");
		advancedfx::Message("%s cosmetics: live skin-state snapshot written to the mvm_debug log%s.\n",
			cmd, MvmDebugLog_Active() ? "" : " (start it first: 'mvm_debug start')");
	} else if (TokenIs(sub, "rebuild")) {
		DoRebuild(argc, args, cmd);
	} else if (TokenIs(sub, "rebuildflags")) {
		DoRebuildFlags(argc, args, cmd);
	} else if (TokenIs(sub, "paintkitbridge")) {
		DoPaintkitBridge(argc, args, cmd);
	} else if (TokenIs(sub, "composite")) {
		DoComposite(argc, args, cmd);
	} else if (TokenIs(sub, "autocomposite")) {
		DoAutoComposite(argc, args, cmd);
	} else if (TokenIs(sub, "modelswap")) {
		DoModelSwap(argc, args, cmd);
	} else if (TokenIs(sub, "animreset")) {
		DoAnimReset(argc, args, cmd);
	} else if (TokenIs(sub, "precache")) {
		DoPrecache(argc, args, cmd);
	} else if (TokenIs(sub, "animfix")) {
		DoAnimFix(argc, args, cmd);
	} else if (TokenIs(sub, "ticknudge")) {
		DoTickNudge(argc, args, cmd);
	} else if (TokenIs(sub, "debuglog")) {
		DoDebugLog(argc, args, cmd);
	} else if (TokenIs(sub, "uilog")) {
		DoUiLog(argc, args, cmd);
	} else if (TokenIs(sub, "mesh")) {
		DoMesh(argc, args, cmd);
	} else if (TokenIs(sub, "recompose")) {
		DoRecompose(argc, args, cmd);
	} else if (TokenIs(sub, "vtidx")) {
		DoVtIdx(argc, args, cmd);
	} else if (TokenIs(sub, "fallback")) {
		DoFallback(argc, args, cmd);
	} else if (TokenIs(sub, "vtarg")) {
		DoVtArg(argc, args, cmd);
	} else if (TokenIs(sub, "vtprobe")) {
		DoVtProbe(argc, args, cmd);
	} else {
		PrintUsage(cmd);
	}
}

} // namespace Filmmaker
