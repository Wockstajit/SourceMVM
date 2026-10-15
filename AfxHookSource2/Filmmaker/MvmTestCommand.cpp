#include "MvmTest.h"

#include "Panorama/ConfigHud.h"
#include "Movie/ParticleFx.h"

#include "../WrpConsole.h"
#include "../../Shared/AfxConsole.h"

#include "../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <atomic>
#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

std::atomic<bool> g_mvmTestActive{ false };

bool PlayingDemo() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

void ExecClientCmd(const char* cmd) {
	if (g_pEngineToClient)
		g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
}

void ExecPracticeSetup(const char* map) {
	ExecClientCmd("mirv_cvar_unlock_sv_cheats");
	ExecClientCmd("sv_cheats 1");
	if (map && map[0]) {
		char buf[128];
		std::snprintf(buf, sizeof(buf), "map %s", map);
		ExecClientCmd(buf);
	}
	ExecClientCmd("bot_kick");
	ExecClientCmd("bot_quota 4");
	ExecClientCmd("bot_add");
	ExecClientCmd("mp_freezetime 0");
	ExecClientCmd("mp_warmup_end");
	ExecClientCmd("mp_respawn_on_death_ct 1");
	ExecClientCmd("mp_respawn_on_death_t 1");
	ExecClientCmd("mp_buy_anywhere 1");
	ExecClientCmd("mp_buytime 99999");
}

void ArmIfNeeded() {
	if (MvmTest_Active())
		return;
	MvmTest_SetActive(true);
	ExecClientCmd("mirv_cvar_unlock_sv_cheats");
	ExecClientCmd("sv_cheats 1");
}

} // namespace

bool MvmTest_Active() { return g_mvmTestActive.load(std::memory_order_relaxed); }

void MvmTest_SetActive(bool on) { g_mvmTestActive.store(on, std::memory_order_relaxed); }

bool MvmTest_LevelLoaded() {
	if (!g_pEngineToClient)
		return false;
	const char* lvl = g_pEngineToClient->GetLevelNameShort();
	return lvl && lvl[0] != '\0';
}

bool MvmTest_CanUseMenu() {
	return MvmTest_Active() && MvmTest_LevelLoaded() && !PlayingDemo();
}

void MvmTest_EnsureArmed() { ArmIfNeeded(); }

bool MvmTest_HandleInsertKey() {
	if (PlayingDemo() || !MvmTest_LevelLoaded())
		return false;
	ArmIfNeeded();
	if (!ConfigHudRef().Enabled())
		ConfigHudRef().SetEnabled(true);
	else
		ConfigHudRef().SetEnabled(false);
	return true;
}

void MvmTest_RunCommand(int argc, advancedfx::ICommandArgs* args) {
	const char* sub = (argc >= 2) ? args->ArgV(1) : "status";

	if (0 == _stricmp(sub, "start")) {
		const char* map = (argc >= 3) ? args->ArgV(2) : nullptr;
		// Private-match only: never arm the FX test harness onto demo playback. Starting
		// with an explicit map is fine (the `map` command leaves the demo), but arming on
		// the CURRENT level while a demo is playing would attach to the demo -- refuse it.
		if ((!map || !map[0]) && PlayingDemo()) {
			advancedfx::Warning("mvm_test: cannot start during demo playback (private-match only). "
				"Pass a map to leave the demo (mvm_test start de_dust2), or use the demo Config panel.\n");
			return;
		}
		MvmTest_SetActive(true);
		if (map && map[0]) {
			ExecPracticeSetup(map);
			advancedfx::Message("mvm_test: started on map '%s'. Camera + Effects menu opening (Insert toggles).\n", map);
		} else if (MvmTest_LevelLoaded()) {
			ExecClientCmd("mirv_cvar_unlock_sv_cheats");
			ExecClientCmd("sv_cheats 1");
			advancedfx::Message("mvm_test: armed on current map. Camera + Effects menu opening (Insert toggles).\n");
		} else {
			ExecPracticeSetup("de_dust2");
			advancedfx::Message("mvm_test: loading de_dust2 with bots. Camera + Effects menu opening (Insert toggles).\n");
		}
		ConfigHudRef().SetEnabled(true);
		return;
	}

	if (0 == _stricmp(sub, "stop")) {
		MvmTest_SetActive(false);
		ConfigHudRef().SetEnabled(false);
		advancedfx::Message("mvm_test: stopped.\n");
		return;
	}

	if (0 == _stricmp(sub, "menu")) {
		ConfigHud_RunCommand(argc, args, "mvm_test");
		return;
	}

	if (0 == _stricmp(sub, "status")) {
		const char* lvl = g_pEngineToClient ? g_pEngineToClient->GetLevelNameShort() : nullptr;
		ParticleFx& fx = ParticleFxRef();
		advancedfx::Message(
			"mvm_test: active=%s map='%s' menu=%s fx_master=%s fx_hook=%s\n"
			"Usage: mvm_test start [map] | stop | menu on|off|toggle | status\n",
			MvmTest_Active() ? "yes" : "no",
			lvl ? lvl : "(none)",
			ConfigHud_Enabled() ? "open" : "closed",
			fx.Enabled() ? "on" : "off",
			fx.Installed() ? "armed" : "not armed");
		return;
	}

	advancedfx::Warning("mvm_test: unknown subcommand '%s'. Usage: mvm_test start [map] | stop | menu | status\n", sub);
}

} // namespace Filmmaker

CON_COMMAND(mvm_test, "Offline live-match Camera + Effects testing (mvm_test start [map] | stop | menu | status).") {
	const int argc = args->ArgC();
	Filmmaker::MvmTest_RunCommand(argc, args);
}
