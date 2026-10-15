#include "EasterEggGunMute.h"

#include "../../../Shared/binutils.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/Detours/src/detours.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace Filmmaker {
namespace {

// client.dll DoStartSoundEvent (found 2026-10-10 from its "DoStartSoundEvent(%s)" debug print). Both
// CMsgSosStartSoundEvent (demo / network sounds) and the client StartSoundEvent(name) path call it:
//   (ctx, outGuid, a3, a4, soundeventHash, sourceEntityIndex, seed, packedParams, packedSize, startTime)
// It writes a 16-byte guid struct to outGuid and returns it. The name of a hash comes from the
// sound op system at [rip+...] (+8 interface, vtable slot 2).
typedef void* (__fastcall* DoStartSoundEvent_t)(void* ctx, void* outGuid, void* a3, int a4, unsigned int hash,
	unsigned int entity, short seed, void* packed, int packedSize, void* startTime);

// Prologue through the `mov rcx, [rip+soundOpSystem]` at +71 (rel32 at +74, next instruction at +78).
const char* const kPattern =
	"48 8B C4 55 53 56 57 41 54 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 44 8B A5 ?? ?? ?? ?? 33 DB "
	"4C 89 68 08 48 8B F2 89 1A 41 8B F9 C7 42 04 FF FF FF FF 89 5A 08 4C 89 70 10 4C 89 78 18 33 C0 "
	"48 89 42 0C 41 8B D4 48 8B 0D";
constexpr size_t kGlobalRel32 = 74, kGlobalNext = 78;

DoStartSoundEvent_t g_orig = nullptr;
void** g_soundOpSystem = nullptr; // address of the global holding the sound op system pointer
bool g_installed = false, g_tried = false;
std::atomic<bool> g_enabled{ false };
std::atomic<unsigned long long> g_seen{ 0 }, g_dropped{ 0 };
std::mutex g_namesMx;
std::set<std::string> g_loggedNames;

// The event's name, copied (lower-cased, at most 127 chars) into `out`. Everything that touches game
// memory is fault-guarded and allocation-free: this runs for EVERY sound the game starts.
bool NameOf(unsigned int hash, char (&out)[128]) {
	out[0] = 0;
	__try {
		char* sys = g_soundOpSystem ? (char*)*g_soundOpSystem : nullptr;
		if (!sys) return false;
		void* iface = sys + 8;
		void** vtbl = *(void***)iface;
		typedef const char* (__fastcall* Name_t)(void*, unsigned int);
		const char* name = ((Name_t)vtbl[2])(iface, hash);
		if (!name) return false;
		int i = 0;
		for (; i < 127 && name[i]; ++i) out[i] = (char)tolower((unsigned char)name[i]);
		out[i] = 0;
		return i > 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		out[0] = 0;
		return false;
	}
}

// A gun going off: CS2 names those "Weapon_<gun>.Single", "...SingleDistant", "...Silenced" and so on.
// Reloads, draws, bolts, knives and grenades are left alone. `n` is lower-case.
bool IsGunshot(const char* n) {
	if (!n || std::strncmp(n, "weapon", 6) != 0) return false;
	static const char* const kNot[] = { "knife", "grenade", "flash", "smoke", "molotov", "incgrenade", "decoy",
		"c4", "taser", "reload", "draw", "deploy", "clip", "bolt", "pump", "slide", "zoom", "empty", "inspect",
		"pickup", "drop", "land", "hit", "bullet", "shell", "casing", "impact", "hammer", "unsil" };
	for (const char* k : kNot) if (std::strstr(n, k)) return false;
	static const char* const kShot[] = { "single", "silenced", "fire", "shot", "distant", "tail", "burst", "auto" };
	for (const char* k : kShot) if (std::strstr(n, k)) return true;
	return false;
}

void* __fastcall Hook(void* ctx, void* outGuid, void* a3, int a4, unsigned int hash, unsigned int entity, short seed,
	void* packed, int packedSize, void* startTime) {
	if (g_enabled.load(std::memory_order_relaxed)) {
		g_seen.fetch_add(1, std::memory_order_relaxed);
		char name[128];
		const bool named = NameOf(hash, name);
		const bool drop = named && IsGunshot(name);
		if (named && MvmDebugLog_Active() && name[0] == 'w') {
			// Every distinct weapon sound name once, so the filter can be checked against real names.
			bool fresh = false;
			try {
				const std::string key(name);
				std::lock_guard<std::mutex> lock(g_namesMx);
				if (g_loggedNames.size() < 400) fresh = g_loggedNames.insert(key).second;
			} catch (...) {}
			if (fresh) MvmDebugLog_LinefAlways("egg.gunmute.name", "%s %s", drop ? "MUTED" : "kept ", name);
		}
		if (drop) {
			g_dropped.fetch_add(1, std::memory_order_relaxed);
			// What the original writes before starting anything: an empty (never-started) guid.
			unsigned char* g = (unsigned char*)outGuid;
			*(unsigned int*)(g + 0) = 0;
			*(int*)(g + 4) = -1;
			*(unsigned int*)(g + 8) = 0;
			*(unsigned long long*)(g + 12) = 0;
			return outGuid;
		}
	}
	return g_orig(ctx, outGuid, a3, a4, hash, entity, seed, packed, packedSize, startTime);
}

} // namespace

void EasterEggGunMute_Ensure() {
	if (g_tried) return;
	g_tried = true;
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client) { g_tried = false; return; } // not loaded yet: try again later
	Afx::BinUtils::ImageSectionsReader sections(client);
	if (sections.Eof()) return;
	const Afx::BinUtils::MemRange r = Afx::BinUtils::FindPatternString(sections.GetMemRange(), kPattern);
	if (r.IsEmpty()) {
		advancedfx::Warning("uwu: DoStartSoundEvent not found in client.dll; game gunshots stay audible (a CS2 update moved it).\n");
		MvmDebugLog_LinefAlways("egg.gunmute", "pattern NOT FOUND: game gunshots stay audible");
		return;
	}
	const unsigned char* fn = (const unsigned char*)r.Start;
	const int rel = *(const int*)(fn + kGlobalRel32);
	g_soundOpSystem = (void**)(fn + kGlobalNext + rel);
	g_orig = (DoStartSoundEvent_t)r.Start;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_orig, Hook);
	g_installed = NO_ERROR == DetourTransactionCommit();
	MvmDebugLog_LinefAlways("egg.gunmute", "DoStartSoundEvent %s at client+0x%llx",
		g_installed ? "detoured" : "DETOUR FAILED", (unsigned long long)(r.Start - (size_t)client));
}

void EasterEggGunMute_SetEnabled(bool on) { g_enabled.store(on); }

void EasterEggGunMute_Stats(bool& installed, unsigned long long& seen, unsigned long long& dropped) {
	installed = g_installed; seen = g_seen.load(); dropped = g_dropped.load();
}

} // namespace Filmmaker
