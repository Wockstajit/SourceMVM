#include "EasterEggWorld.h"
#include "EasterEggLightingMath.h"

#include "../World/LightFx.h"
#include "../Debug/MvmDebugLog.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"
#include "../Platform/AtomicFile.h"
#include "../../hlaeFolder.h"

#include <Windows.h>

#include <cmath>
#include <fstream>
#include <string>

// Disco Inferno and Wes Anderson both repaint the scene's lights through LightFx, whose values
// are the user's own saved lighting (FilmmakerSettingsStore snapshots them). So before either
// egg touches them, the user's values are stashed -- in memory AND on disk -- and put back the
// moment the egg turns off. The disk copy covers the game closing mid-disco: on the next
// session's first frame a valid leftover stash is restored. Activation requires the disk copy
// to be published successfully before any light state or effect ownership changes.

namespace Filmmaker {
namespace {

struct Stash : EasterEggLightingState {
	bool have = false;
};

Stash g_stash;
bool g_disco = false, g_wes = false;
bool g_checkedLeftover = false;
const char* g_recovery = "pending";
DWORD g_stashWriteError = ERROR_SUCCESS;
unsigned long long g_discoStartMs = 0;
int g_lastBeat = -1;

// The party palette, one colour per beat.
const int kDisco[][3] = {
	{ 255, 40, 200 }, { 40, 230, 255 }, { 255, 230, 40 }, { 120, 255, 60 },
	{ 255, 120, 20 }, { 160, 70, 255 }, { 255, 60, 90 }, { 60, 120, 255 },
};
constexpr int kDiscoCount = (int)(sizeof(kDisco) / sizeof(kDisco[0]));
constexpr double kBpm = 120.0;
constexpr double kSpinDegPerSec = 36.0; // the sun circles the map: shadows sweep like a mirror ball

std::wstring StashPath() {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
	path += L"easter_eggs_light_stash.json";
	return path;
}

bool WriteStashFile() {
	JsonBuilder j;
	j.BeginObject();
	j.IntField("r", g_stash.r); j.IntField("g", g_stash.g); j.IntField("b", g_stash.b);
	j.DoubleField("amount", g_stash.amount);
	j.BoolField("uniform", g_stash.uniform);
	j.DoubleField("yaw", g_stash.yaw);
	j.EndObject();
	g_stashWriteError = WriteFileAtomic(StashPath(), j.Str());
	if (g_stashWriteError != ERROR_SUCCESS) {
		MvmDebugLog_LinefAlways("egg.light", "activation refused: stash write error=%lu", g_stashWriteError);
		return false;
	}
	return true;
}

bool TakeStash() {
	if (g_stash.have) return true;
	LightFx_GetTint(&g_stash.r, &g_stash.g, &g_stash.b);
	g_stash.amount = LightFx_Amount();
	g_stash.uniform = LightFx_Uniform();
	g_stash.yaw = LightFx_SunYaw();
	if (!WriteStashFile()) return false;
	g_stash.have = true;
	MvmDebugLog_LinefAlways("egg.light", "stash tint=%d,%d,%d amount=%.2f uniform=%d yaw=%.1f",
		g_stash.r, g_stash.g, g_stash.b, g_stash.amount, g_stash.uniform ? 1 : 0, g_stash.yaw);
	return true;
}

void RestoreStash() {
	if (!g_stash.have) return;
	LightFx_SetTint(g_stash.r, g_stash.g, g_stash.b);
	LightFx_SetAmount(g_stash.amount);
	LightFx_SetUniform(g_stash.uniform);
	LightFx_SetSunYaw(g_stash.yaw);
	g_stash.have = false;
	DeleteFileW(StashPath().c_str());
	MvmDebugLog_LinefAlways("egg.light", "restored tint=%d,%d,%d amount=%.2f uniform=%d yaw=%.1f",
		g_stash.r, g_stash.g, g_stash.b, g_stash.amount, g_stash.uniform ? 1 : 0, g_stash.yaw);
}

// A stash left on disk means the last session ended mid-egg. Put the user's lighting back.
void RestoreLeftover() {
	g_checkedLeftover = true;
	std::ifstream f(StashPath(), std::ios::binary | std::ios::ate);
	if (!f) {
		g_recovery = GetFileAttributesW(StashPath().c_str()) == INVALID_FILE_ATTRIBUTES
			&& GetLastError() == ERROR_FILE_NOT_FOUND ? "none" : "read-failed";
		return;
	}
	// A normal stash is under 200 bytes. Bound input before allocation/parsing.
	const auto length = f.tellg();
	if (length <= 0 || length > 4096) { g_recovery = "invalid"; return; }
	std::string text((size_t)length, '\0');
	f.seekg(0);
	if (!f.read(&text[0], (std::streamsize)text.size())) { g_recovery = "read-failed"; return; }
	f.close();
	JsonValue v;
	EasterEggLightingState recovered;
	if (!JsonParse(text, v) || !EasterEggLightingMath::ReadRecovery(v, recovered)) {
		// Preserve invalid evidence and the settings already loaded from the user's preferences.
		g_recovery = "invalid";
		return;
	}
	static_cast<EasterEggLightingState&>(g_stash) = recovered;
	g_stash.have = true;
	RestoreStash();
	g_recovery = "restored";
}

void ApplyWes() {
	// A warm, slightly peachy daylight: Wes Anderson's whole palette is the sun at 4pm.
	LightFx_SetUniform(false);
	LightFx_SetTint(255, 206, 160);
	LightFx_SetAmount(0.6f);
}

} // namespace

const char* EasterEggLighting_RecoveryStatus() {
	MvmDebugLog_LinefAlways("egg.light", "recovery=%s stashActive=%d disco=%d wes=%d writeError=%lu",
		g_recovery, g_stash.have ? 1 : 0, g_disco ? 1 : 0, g_wes ? 1 : 0, g_stashWriteError);
	return g_recovery;
}

EasterEggLightingState EasterEggLighting_UserState() {
	if (g_stash.have) return g_stash;
	EasterEggLightingState state;
	LightFx_GetTint(&state.r, &state.g, &state.b);
	state.amount = LightFx_Amount();
	state.uniform = LightFx_Uniform();
	state.yaw = LightFx_SunYaw();
	return state;
}

bool EasterEggLighting_SetDisco(bool on) {
	if (on == g_disco) return true;
	if (on) {
		if (!TakeStash()) return false;
		g_disco = true;
		g_wes = false;
		g_discoStartMs = GetTickCount64();
		g_lastBeat = -1;
		LightFx_SetUniform(true);
		LightFx_SetAmount(1.0f);
	} else {
		g_disco = false;
		if (g_wes) { LightFx_SetSunYaw(g_stash.yaw); ApplyWes(); }
		else RestoreStash();
	}
	return true;
}

bool EasterEggLighting_SetWes(bool on) {
	if (on == g_wes) return true;
	if (on) {
		if (!TakeStash()) return false;
		g_wes = true;
		g_disco = false;
		LightFx_SetSunYaw(g_stash.yaw);
		ApplyWes();
	} else {
		g_wes = false;
		if (!g_disco) RestoreStash();
	}
	return true;
}

void EasterEggLighting_RunFrame() {
	if (!g_checkedLeftover && !g_disco && !g_wes) RestoreLeftover();
	if (!g_disco) return;
	const double t = (GetTickCount64() - g_discoStartMs) / 1000.0;
	const int beat = (int)(t * kBpm / 60.0);
	if (beat != g_lastBeat) {
		g_lastBeat = beat;
		const int* c = kDisco[beat % kDiscoCount];
		LightFx_SetTint(c[0], c[1], c[2]);
		if (MvmDebugLog_Active() && beat % 8 == 0)
			MvmDebugLog_Linef("egg.disco", "beat=%d tint=%d,%d,%d lightsSeen=%llu", beat, c[0], c[1], c[2], LightFx_LightsSeen());
	}
	LightFx_SetSunYaw((float)std::fmod(g_stash.yaw + t * kSpinDegPerSec, 360.0));
}

} // namespace Filmmaker
