#include "WorldTransient.h"
#include "NightMode.h"
#include "AutoExposure.h"

#include "WorldSchema.h"
#include "WorldEntities.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Linef
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"            // g_clientDllOffsets (camera-services chain, diagnostic)
#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// --------------------------------------------------------------------------------------------
// MEASURED 2026-08-04, Mirage, repaired match demo, paused at tick 30555 (identical camera for
// every capture) -- see Docs/effects/night-mode.md 3.
//
// What does NOT work, despite being what every reference port does:
//   * Writing C_PostProcessingVolume::m_flMinExposure / m_flMaxExposure (+ m_bExposureControl).
//     The writes LAND -- the mvm_debug log shows the volume snapshotted at min=0.8 max=1.0
//     ctrl=1 and rewritten every frame -- and the image does not change AT ALL. The map has a
//     single volume (entity 82) and the spectator camera reports as inside NO volume
//     (m_hActivePostProcessingVolume == invalid), so the tonemapper never consults it.
//   * mat_tonemap_force_min / mat_tonemap_force_max. Confirmed set to 0.1/0.1 by reading them
//     back; zero visual change. These read as Source-1-era leftovers ("Old default was 3.0" in
//     their own help text) that the CS2 renderer no longer consults.
//
// What DOES work:
//   * mat_tonemap_force_scale. 0 = off (engine auto-exposure). 0.10 = night, 0.35 = dusk,
//     1.0 = roughly neutral, higher = brighter. Verified by screenshot at a fixed camera/tick.
//
// So this feature is driven ENTIRELY by one convar and needs no schema fields at all, which
// also makes it the most update-resilient thing in the World module: a convar rename is the
// only way it can break, and `world health` will say so.
// --------------------------------------------------------------------------------------------
constexpr const char* kScaleConvar = "mat_tonemap_force_scale";

// Re-assert cadence. Convars persist, but a demo/map change or a config exec can reset them,
// and a silently-reverted look mid-edit is worse than a redundant command every couple of
// seconds. Same reasoning (and interval) as the spec_mode re-assert in Filmmaker.cpp.
constexpr unsigned long long kReassertMs = 2000;

float g_exposure = kExposureAuto;
// `previous` while adaptation owns the convar, so leaving it re-applies the plain state.
constexpr float kAdaptingSentinel = -2.0f;
unsigned long long g_lastAssertMs = 0;
bool g_cheatsUnlocked = false;

// Diagnostics only. The post-processing volume plays no part in the applied effect any more;
// these are kept because `world health` showing "1 volume, camera inside -1" is precisely the
// evidence that explains why the volume approach was abandoned.
const char* const kVolumeClasses[] = {
	"post_processing_volume",
	"C_PostProcessingVolume",
};
int g_volumeCount = 0;
int g_activeVolumeIndex = -1;
int g_scanCountdown = 0;
constexpr int kRescanIntervalFrames = 30;

void ExecCmd(const char* command) {
	if (!g_pEngineToClient || !command) return;
	g_pEngineToClient->ExecuteClientCmd(0, command, true);
}

// mat_tonemap_force_scale is cheat-flagged. Offline demo/movie work only, so unlocking is
// appropriate here -- this DLL is never wired for VAC-protected servers.
void EnsureCheatsUnlocked() {
	if (g_cheatsUnlocked) return;
	g_cheatsUnlocked = true;
	ExecCmd("mirv_cvar_unlock_sv_cheats");
	ExecCmd("sv_cheats 1");
	MvmDebugLog_Linef("world.exposure", "cheats unlocked for %s", kScaleConvar);
}

void ApplyScale(float value) {
	char buf[96];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s %.4f", kScaleConvar, value);
	ExecCmd(buf);
	g_lastAssertMs = GetTickCount64();
}

// Diagnostic refresh: how many post-processing volumes exist and which one the camera is in.
void UpdateVolumeDiagnostics() {
	g_volumeCount = WorldEntity_Count(kVolumeClasses,
		sizeof(kVolumeClasses) / sizeof(kVolumeClasses[0]));

	g_activeVolumeIndex = -1;
	const std::ptrdiff_t offActive = WorldSchema_Offset(kWF_CamSvc_ActivePpv);
	const std::ptrdiff_t offCamSvc = g_clientDllOffsets.C_BasePlayerPawn.m_pCameraServices;
	if (0 == offActive || 0 == offCamSvc) return;
	if (!g_pEntityList || !g_GetEntityFromIndex) return;

	const int pawnIndex = AfxGetSpectatedPawnIndex();
	if (pawnIndex < 0) return;

	__try {
		auto pawn = (unsigned char*)g_GetEntityFromIndex(*g_pEntityList, pawnIndex);
		if (!pawn) return;
		auto camSvc = *(unsigned char**)(pawn + offCamSvc);
		if (!camSvc) return;
		const unsigned int handle = *(unsigned int*)(camSvc + offActive);
		if (0xFFFFFFFFu == handle) return;
		g_activeVolumeIndex = (int)(handle & 0x7FFFu);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_activeVolumeIndex = -1;
	}
}

} // namespace

bool NightMode_Available(std::string* outReason) {
	// No schema dependency at all -- see the measurement block above. The only requirement is a
	// live engine to push the convar through.
	if (!g_pEngineToClient) {
		if (outReason) *outReason = "engine-to-client interface not resolved";
		return false;
	}
	if (outReason) outReason->clear();
	return true;
}

float NightMode_Exposure() { return g_exposure; }
bool  NightMode_Enabled()  { return g_exposure != kExposureAuto; }
int   NightMode_VolumeCount() { return g_volumeCount; }
int   NightMode_ActiveVolumeIndex() { return g_activeVolumeIndex; }

void NightMode_RestoreAll() {
	g_exposure = kExposureAuto;
	ApplyScale(0.0f); // 0 = off; engine auto-exposure resumes
	MvmDebugLog_Linef("world.exposure", "restore (%s 0)", kScaleConvar);
}

void NightMode_SetExposure(float exposure) {
	if (exposure == kExposureAuto) {
		NightMode_RestoreAll();
		return;
	}

	std::string reason;
	if (!NightMode_Available(&reason)) {
		advancedfx::Warning("mirv_filmmaker world exposure: unavailable (%s).\n", reason.c_str());
		return;
	}

	// Hand-rolled clamp: windows.h defines min/max as macros, which mangles std::min/std::max.
	float clamped = exposure;
	if (clamped < kExposureMin) clamped = kExposureMin;
	else if (clamped > kExposureMax) clamped = kExposureMax;

	EnsureCheatsUnlocked();
	g_exposure = clamped;
	ApplyScale(clamped);
	MvmDebugLog_Linef("world.exposure", "set %s=%.4f", kScaleConvar, clamped);
}

void NightMode_RunFrame() {
	// Diagnostics stay fresh even while idle -- `world health` is most useful when it can be run
	// at any moment, not only while the feature is on.
	if (--g_scanCountdown <= 0) {
		g_scanCountdown = kRescanIntervalFrames;
		UpdateVolumeDiagnostics();
	}

	const float effective = float(WorldTransient::Number("exposure", g_exposure));
	static float previous = kExposureAuto;
	// Eye adaptation drives the same convar while exposure is on auto; a pinned value wins.
	// AutoExposure_MainThread runs every frame regardless so its clock stays continuous.
	float adapted = 0.f;
	const bool adapting = AutoExposure_MainThread(adapted) && effective == kExposureAuto;
	if (adapting) {
		static float applied = 0.f;
		const unsigned long long now = GetTickCount64();
		EnsureCheatsUnlocked();
		if (previous != kAdaptingSentinel || !(applied > 0) || std::fabs(std::log2(adapted / applied)) > .003f ||
			now - g_lastAssertMs >= kReassertMs) { ApplyScale(adapted); applied = adapted; }
		previous = kAdaptingSentinel;
		return;
	}
	if (effective != previous) { EnsureCheatsUnlocked(); ApplyScale(effective == kExposureAuto ? 0.f : effective); previous = effective; }
	if (effective == kExposureAuto) return;

	const unsigned long long now = GetTickCount64();
	if (now - g_lastAssertMs >= kReassertMs) ApplyScale(effective);
}

void NightMode_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker world exposure [<value>|auto] [quiet]
	if (argc < 4) {
		std::string reason;
		const bool ok = NightMode_Available(&reason);
		advancedfx::Message("mirv_filmmaker world exposure: %s\n",
			NightMode_Enabled() ? "pinned" : "auto");
		if (ok) advancedfx::Message("  available : yes (via %s)\n", kScaleConvar);
		else    advancedfx::Message("  available : NO - %s\n", reason.c_str());
		if (NightMode_Enabled())
			advancedfx::Message("  value     : %.3f\n", NightMode_Exposure());
		advancedfx::Message("  volumes   : %d post-process volume(s), camera inside index %d\n",
			NightMode_VolumeCount(), NightMode_ActiveVolumeIndex());
		float metered = 0.f;
		const bool haveMeter = AutoExposure_MeteredLog2(metered);
		advancedfx::Message("  adapt     : %s  speed %.2f stops/s  compensation %+.2f stops  scale %.3f  metered log2 %s\n",
			AutoExposure_Enabled() ? "on" : "off", AutoExposure_Speed(), AutoExposure_Compensation(),
			AutoExposure_CurrentScale(), haveMeter ? std::to_string(metered).c_str() : "-");
		advancedfx::Message(
			"Usage:\n"
			"  mirv_filmmaker world exposure <%.2f..%.2f> - pin exposure (0.10 night, 0.35 dusk, 1.0 neutral)\n"
			"  mirv_filmmaker world exposure auto         - restore the engine's auto-exposure\n"
			"  mirv_filmmaker world exposure adapt on|off|toggle - eye adaptation while exposure is auto\n"
			"  mirv_filmmaker world exposure speed <%.1f..%.0f>  - adaptation speed, stops per second\n"
			"  mirv_filmmaker world exposure compensation <-4..4> - brighten/darken the adapted exposure, stops\n",
			kExposureMin, kExposureMax, kAutoExposureSpeedMin, kAutoExposureSpeedMax);
		return;
	}

	const char* v = args->ArgV(3);
	const bool quiet = (argc >= 5) && (0 == _stricmp(args->ArgV(4), "quiet"));

	if (0 == _stricmp(v, "adapt")) {
		const char* mode = argc >= 5 ? args->ArgV(4) : "toggle";
		const bool on = (0 == _stricmp(mode, "on") || 0 == _stricmp(mode, "1")) ? true
			: (0 == _stricmp(mode, "off") || 0 == _stricmp(mode, "0")) ? false : !AutoExposure_Enabled();
		AutoExposure_SetEnabled(on);
		MvmDebugLog_Linef("world.exposure", "adapt=%d", int(on));
		if (!(argc >= 6 && 0 == _stricmp(args->ArgV(5), "quiet")))
			advancedfx::Message("mirv_filmmaker: eye adaptation %s.\n", on ? "on" : "off");
		return;
	}
	if (0 == _stricmp(v, "speed") || 0 == _stricmp(v, "compensation")) {
		if (argc < 5) { advancedfx::Message("mirv_filmmaker world exposure %s <value>\n", v); return; }
		const float value = float(atof(args->ArgV(4)));
		if (0 == _stricmp(v, "speed")) AutoExposure_SetSpeed(value); else AutoExposure_SetCompensation(value);
		if (!(argc >= 6 && 0 == _stricmp(args->ArgV(5), "quiet")))
			advancedfx::Message("mirv_filmmaker: adaptation speed %.2f stops/s, compensation %+.2f stops.\n",
				AutoExposure_Speed(), AutoExposure_Compensation());
		return;
	}

	if (0 == _stricmp(v, "auto") || 0 == _stricmp(v, "off") || 0 == _stricmp(v, "default")) {
		NightMode_SetExposure(kExposureAuto);
		if (!quiet) advancedfx::Message("mirv_filmmaker: exposure = auto (engine auto-exposure restored).\n");
		return;
	}

	NightMode_SetExposure((float)atof(v));
	if (!quiet && NightMode_Enabled())
		advancedfx::Message("mirv_filmmaker: exposure = %.3f.\n", NightMode_Exposure());
}

} // namespace Filmmaker
