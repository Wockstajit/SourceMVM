#include "DeadlockEngine.h"

#include "DeadlockAddresses.h"
#include "DeadlockDemo.h"
#include <cmath>

#include "../Globals.h"
#include "../../Shared/AfxConsole.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"

#include <windows.h>
#include <cstring>
#include <filesystem>

namespace Deadlock {

namespace {

void* g_pEngineToClient = nullptr;
void* g_pCvarIface = nullptr;

double g_tickInterval = 1.0 / 64.0;
bool g_tickIntervalConfirmed = false;

bool g_hadFault = false;

namespace fs = std::filesystem;

bool ExistingDemoPath(fs::path candidate, std::wstring& out) {
	if (!candidate.has_extension()) candidate += L".dem";
	std::error_code ec;
	if (!fs::is_regular_file(candidate, ec)) return false;
	fs::path absolute = fs::absolute(candidate, ec);
	if (ec) absolute = candidate;
	out = absolute.lexically_normal().wstring();
	return true;
}

std::wstring ResolveDemoPath(const std::wstring& argument) {
	if (argument.empty()) return {};
	std::wstring resolved;
	const fs::path input(argument);
	if (input.is_absolute() && ExistingDemoPath(input, resolved)) return resolved;

	// Deadlock runs from game/bin/win64, while `playdemo replays/foo` is resolved below
	// game/citadel. Find the game ancestor instead of assuming which Steam library owns it.
	wchar_t module[32768] = {};
	const DWORD length = GetModuleFileNameW(nullptr, module, (DWORD)_countof(module));
	if (length && length < _countof(module)) {
		fs::path dir = fs::path(std::wstring(module, length)).parent_path();
		while (!dir.empty()) {
			if (0 == _wcsicmp(dir.filename().c_str(), L"game")) {
				if (ExistingDemoPath(dir / L"citadel" / input, resolved)) return resolved;
				if (ExistingDemoPath(dir / input, resolved)) return resolved;
				break;
			}
			const fs::path parent = dir.parent_path();
			if (parent == dir) break;
			dir = parent;
		}
	}

	if (ExistingDemoPath(input, resolved)) return resolved;
	return {}; // an unresolved filename must never become a project save destination
}

int g_slots[(int)EngineSlot::Count] = {
	51, // ExecuteClientCmd: build 6774 CEngineClient, body matched and echo verified
	65, // GetLevelNameShort: preserve the bare map name returned by the old slot
	43, // GetDemoFilePath: CEngineClient -> demo player slot1 -> CDemoFile slot3
	3,  // DemoGetTick        VERIFIED from vtable accessor machine code
	11, // DemoIsPlaying      VERIFIED from vtable accessor machine code
	12, // DemoIsPaused       VERIFIED from vtable accessor machine code
	14, // DemoIsSeeking      VERIFIED from vtable accessor machine code
	2,  // DemoGetStartTick   VERIFIED from vtable accessor machine code
	17, // DemoGetPlaybackScale VERIFIED from vtable accessor machine code
};

const char* const g_slotNames[(int)EngineSlot::Count] = {
	"executeclientcmd",
	"getlevelname",
	"getdemofilepath",
	"demogettick",
	"demoisplaying",
	"demoispaused",
	"demoisseeking",
	"demogetstarttick",
	"demogetplaybackscale",
};

inline void** VTable(void* obj) {
	if (!obj) return nullptr;
	return *(void***)obj;
}

// A guarded call is the difference between "that slot index was wrong" and "the game vanished".
// Slot indices here are hypotheses by construction, so every call through one is wrapped.
#define DL_GUARD_BEGIN __try {
#define DL_GUARD_END(neutral) } __except (EXCEPTION_EXECUTE_HANDLER) { \
		g_hadFault = true; \
		return neutral; \
	}

typedef bool(__fastcall* Fn_Bool_This)(void*);
typedef int(__fastcall* Fn_Int_This)(void*);
typedef void*(__fastcall* Fn_Ptr_This)(void*);
typedef const char*(__fastcall* Fn_Str_This)(void*);
typedef void(__fastcall* Fn_ExecCmd)(void*, int, const char*, bool);

void* CallPtrSlot(void* obj, int slot) {
	void** vt = VTable(obj);
	if (!vt || slot < 0) return nullptr;
	DL_GUARD_BEGIN
		return ((Fn_Ptr_This)vt[slot])(obj);
	DL_GUARD_END(nullptr)
}

bool CallBoolSlot(void* obj, int slot) {
	void** vt = VTable(obj);
	if (!vt || slot < 0) return false;
	DL_GUARD_BEGIN
		return ((Fn_Bool_This)vt[slot])(obj);
	DL_GUARD_END(false)
}

int CallIntSlot(void* obj, int slot) {
	void** vt = VTable(obj);
	if (!vt || slot < 0) return -1;
	DL_GUARD_BEGIN
		return ((Fn_Int_This)vt[slot])(obj);
	DL_GUARD_END(-1)
}

const char* CallStrSlot(void* obj, int slot) {
	void** vt = VTable(obj);
	if (!vt || slot < 0) return nullptr;
	DL_GUARD_BEGIN
		return ((Fn_Str_This)vt[slot])(obj);
	DL_GUARD_END(nullptr)
}

// Length of a possibly-bogus char*, bounded and guarded. Kept separate from the std::string
// construction below because MSVC will not compile SEH in a frame that needs object unwinding
// (C2712), which is exactly the kind of thing that turns a safety net into a build error.
size_t SafeLength(const char* p, size_t limit) {
	if (!p) return 0;
	DL_GUARD_BEGIN
		size_t n = 0;
		while (n < limit && p[n] != 0) ++n;
		return n;
	DL_GUARD_END(0)
}

// Copies a possibly-bogus char* into a std::string without trusting its length. A wrong slot can
// hand back anything.
std::string SafeString(const char* p, size_t limit = 1024) {
	size_t n = SafeLength(p, limit);
	if (!n) return std::string();
	return std::string(p, n);
}

} // namespace

bool Engine_Connect(void* appSystemFactory) {
	typedef void*(*CreateInterfaceFn)(const char*, int*);
	CreateInterfaceFn factory = (CreateInterfaceFn)appSystemFactory;
	if (!factory) return false;

	// Byte-identical to CS2's version strings. Verified against the installed Deadlock build by
	// Automation/verify/verify-deadlock-signatures.py, which is why there is nothing to derive
	// here: the whole interface-acquisition layer transfers unchanged.
	g_pEngineToClient = factory("Source2EngineToClient001", nullptr);
	g_pCvarIface = factory("VEngineCvar007", nullptr);

	if (!g_pEngineToClient)
		advancedfx::Warning("AfxHookDeadlock: Source2EngineToClient001 not available.\n");

	return g_pEngineToClient != nullptr;
}

bool Engine_HasEngineToClient() { return g_pEngineToClient != nullptr; }

bool Engine_IsPlayingDemo() {
	return Demo_IsPlaying();
}

bool Engine_IsDemoPaused() { return Demo_IsPaused(); }
int Engine_DemoTick() { return Demo_Tick(); }
double Engine_IntervalPerTick() {
    const double measured = Demo_IntervalPerTick();
    return measured > 0 ? measured : g_tickInterval;
}

void Engine_SetIntervalPerTick(double seconds) {
	if (!std::isfinite(seconds) || seconds < 0.001 || seconds > 0.1) return;
	g_tickInterval = seconds;
	g_tickIntervalConfirmed = true;
}

bool Engine_TickIntervalConfirmed() { return Demo_IntervalPerTick() > 0 || g_tickIntervalConfirmed; }

double Engine_DemoTime() { return Demo_Time(); }

namespace {
bool CopyDemoFileName(char* out, size_t capacity) {
	DL_GUARD_BEGIN
		void** vt = VTable(g_pEngineToClient);
		const int slot = g_slots[(int)EngineSlot::GetDemoFilePath];
		if (!vt || slot < 0) return false;
		const char* name = ((Fn_Str_This)vt[slot])(g_pEngineToClient);
		if (!name) return false;
		for (size_t n = 0; n < capacity; ++n) {
			out[n] = name[n];
			if (!out[n]) return n != 0;
		}
		return false; // refuse an unterminated or oversized engine string
	DL_GUARD_END(false)
}
}

std::string Engine_DemoFilePath() {
	// Build 6774: slot 43 gets CDemoFile through player slot 1 and returns its
	// filename through file slot 3. Slot 44 returns the recorder's name instead.
	// Never fall back to +playdemo: it becomes stale after an in-process replay switch.
	static std::string lastName, resolvedPath;
	char name[4096];
	if (!Demo_IsPlaying() || !CopyDemoFileName(name, sizeof(name))) {
		lastName.clear(); resolvedPath.clear();
		return {};
	}
	if (lastName == name && !resolvedPath.empty()) return resolvedPath;
	lastName = name;
	resolvedPath.clear();
	const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0);
	if (chars <= 1) return {};
	std::wstring wide((size_t)chars, L'\0');
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide.data(), chars)) return {};
	wide.resize((size_t)chars - 1);
	const std::wstring path = ResolveDemoPath(wide);
	if (path.empty()) return {};
	const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(),
		nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) return {};
	resolvedPath.resize((size_t)bytes);
	WideCharToMultiByte(CP_UTF8, 0, path.c_str(), (int)path.size(),
		resolvedPath.data(), bytes, nullptr, nullptr);
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_LinefAlways("deadlock.demo.path", "engine=%s resolved=%s",
			name, resolvedPath.c_str());
	return resolvedPath;
}

std::string Engine_LevelName() {
	if (!g_pEngineToClient) return std::string();
	return SafeString(CallStrSlot(g_pEngineToClient, g_slots[(int)EngineSlot::GetLevelName]));
}

bool Engine_ExecuteClientCmd(const char* command) {
	if (!g_pEngineToClient || !command) return false;
	void** vt = VTable(g_pEngineToClient);
	int slot = g_slots[(int)EngineSlot::ExecuteClientCmd];
	if (!vt || slot < 0) return false;
	DL_GUARD_BEGIN
		((Fn_ExecCmd)vt[slot])(g_pEngineToClient, 0, command, false);
		return true;
	DL_GUARD_END(false)
}

int Engine_GetSlot(EngineSlot slot) {
	int i = (int)slot;
	if (i < 0 || i >= (int)EngineSlot::Count) return -1;
	return g_slots[i];
}

void Engine_SetSlot(EngineSlot slot, int index) {
	int i = (int)slot;
	if (i < 0 || i >= (int)EngineSlot::Count) return;
	g_slots[i] = index;
	advancedfx::Message("AfxHookDeadlock: engine slot %s = %d\n", g_slotNames[i], index);
}

const char* Engine_SlotName(EngineSlot slot) {
	int i = (int)slot;
	if (i < 0 || i >= (int)EngineSlot::Count) return "?";
	return g_slotNames[i];
}

bool Engine_SlotFromName(const char* name, EngineSlot* out) {
	if (!name || !out) return false;
	for (int i = 0; i < (int)EngineSlot::Count; ++i) {
		if (0 == _stricmp(name, g_slotNames[i])) {
			*out = (EngineSlot)i;
			return true;
		}
	}
	return false;
}

bool Engine_HadFault() { return g_hadFault; }
void Engine_ClearFault() { g_hadFault = false; }

} // namespace Deadlock
