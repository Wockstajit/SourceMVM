// HLAE commands hosted on Deadlock's native console. Engine-free command parsers
// share Deadlock's demo clock and camera state; engine calls use verified Deadlock
// interfaces. DeadlockCameraCommands.cpp owns FOV, CAM IO and Panorama commands.
// The campath adapter updates the editor's authoritative markers, not g_CamPath,
// which CamPathEval rebuilds as a chord-length spline. Entity tools remain disabled.

#include "DeadlockCameraRuntime.h"
#include "DeadlockCameraCommands.h"
#include "DeadlockCameraCommandMath.h"
#include "DeadlockDemo.h"
#include "DeadlockEngine.h"

#include "../WrpConsole.h"
#include "DeadlockIcvarSlots.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"
#include "../Filmmaker/Deadlock/DeadlockShot.h"

#include "../../Shared/AfxConsole.h"
#include "../../Shared/CommandSystem.h"
#include "../../Shared/MirvCampath.h"
#include "../../Shared/MirvInput.h"
#include "../../Shared/MirvSkip.h"
#include "../../Shared/StringTools.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <sstream>
#include <list>

// WrpConsole.cpp's "have I drained the queue yet" flag, at global scope there. Read by the
// registration probe below to tell "never ran" apart from "ran and did not take".
extern bool g_bComandsRegisterred;

namespace {

// ---- the demo clock, in the shape Shared/ asks for -------------------------------------------
//
// One adapter, because MirvSkip and MirvCampath both want the same questions
// answered. Every value comes from Deadlock's own demo player rather than from a second clock:
// see DeadlockDemo.cpp and [[demo-clock-backwards-jitter]] for why there is exactly one.
class CDeadlockCampathTime : public IMirvCampath_Time {
public:
	virtual double GetTime() override { return Deadlock::Demo_Time(); }
	virtual double GetCurTime() override { return Deadlock::Demo_Time(); }

	virtual bool GetCurrentDemoTick(int& outTick) override {
		const int tick = Deadlock::Demo_Tick();
		if (tick < 0) return false;
		outTick = tick;
		return true;
	}

	virtual bool GetCurrentDemoTime(double& outDemoTime) override {
		if (Deadlock::Demo_Tick() < 0) return false;
		outDemoTime = Deadlock::Demo_Time();
		return true;
	}

	virtual bool GetDemoTickFromDemoTime(double, double time, int& outTick) override {
		const double interval = Deadlock::Demo_IntervalPerTick();
		return Deadlock::CameraTimeToTick(time, Deadlock::Demo_Time(), Deadlock::Demo_Tick(), interval, outTick);
	}

	virtual bool GetDemoTimeFromClientTime(double curTime, double time, double& outDemoTime) override {
		double current = 0;
		if (!GetCurrentDemoTime(current)) return false;
		outDemoTime = time - (curTime - current);
		return true;
	}

	virtual bool GetDemoTickFromClientTime(double curTime, double targetTime, int& outTick) override {
		double demoTime = 0;
		return GetDemoTimeFromClientTime(curTime, targetTime, demoTime)
			&& GetDemoTickFromDemoTime(curTime, demoTime, outTick);
	}
} g_DeadlockCampathTime;

class CDeadlockCampathCamera : public IMirvCampath_Camera {
	SMirvCameraValue GetCamera() override {
		double p[3], a[3], fov;
		Deadlock::CameraRuntime_GetLastCamera(p, a, fov);
		return SMirvCameraValue(p[0], p[1], p[2], a[0], a[1], a[2], fov);
	}
} g_DeadlockCampathCamera;

// ---- mirv_skip -------------------------------------------------------------------------------

class CDeadlockGotoDemoTick : public IMirvSkip_GotoDemoTick {
	virtual void GotoDemoTick(int tick) override {
		std::ostringstream oss;
		oss << "demo_gototick " << tick;
		Deadlock::Engine_ExecuteClientCmd(oss.str().c_str());
	}
} g_DeadlockGotoDemoTick;

// ---- mirv_cmd --------------------------------------------------------------------------------

class CDeadlockExecuteClientCmd : public IExecuteClientCmdForCommandSystem {
	virtual void ExecuteClientCmd(const char* value) override {
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_LinefAlways("deadlock.cmd.execute", "tick=%d command=%s", Deadlock::Demo_Tick(), value);
		Deadlock::Engine_ExecuteClientCmd(value);
	}
} g_DeadlockExecuteClientCmd;

class CDeadlockGetTick : public IGetTickForCommandSystem {
	virtual float GetTick() override { return (float)Deadlock::Demo_Tick(); }
} g_DeadlockGetTick;

class CDeadlockGetTime : public IGetTimeForCommandSystem {
	virtual float GetTime() override { return (float)Deadlock::Demo_Time(); }
} g_DeadlockGetTime;

CommandSystem g_DeadlockCommandSystem(&g_DeadlockExecuteClientCmd, &g_DeadlockGetTick, &g_DeadlockGetTime);

} // namespace

// ---- registering, through the slot this port actually has ------------------------------------

namespace Deadlock {

void MirvCommands_RunFrame() {
	static bool hadDemo = false;
	const bool hasDemo = Engine_DemoTick() >= 0;
	if (hasDemo != hadDemo) {
		g_DeadlockCommandSystem.OnLevelInitPreEntity();
		hadDemo = hasDemo;
	}
	if (hasDemo) g_DeadlockCommandSystem.OnExecuteCommands();
}

// Installed into WrpConsole's seam from DeadlockMain's client Connect, as soon as the ICvar pointer
// exists and before WrpRegisterCommands drains the queue.
static void RegisterConCommandForDeadlock(SOURCESDK::CS2::CCmd* cmd) {
	// The engine retains the callback interface, so its address must remain stable.
	static std::list<CWrpConCommandCallback> callbacks;
	const auto callback = WrpFindCommandCallback(cmd->GetName());
	if (!callback) return;
	callbacks.emplace_back(callback);
	ConsoleCommandCreation creation;
	creation.name = cmd->GetName();
	creation.help = cmd->GetHelpString();
	creation.flags = (std::uint64_t)cmd->GetFlags();
	creation.callback = &callbacks.back();
	const std::uint64_t handle = Icvar_RegisterConCommand(SOURCESDK::CS2::g_pCVar, creation);
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_LinefAlways("deadlock.console.register", "name=%s slot=%zu handle=0x%llx",
			creation.name, kIcvarSlot_RegisterConCommand, (unsigned long long)handle);
	if ((handle & 0xffff) == 0xffff)
		advancedfx::Warning("AfxHookDeadlock: native console registration failed for %s.\n", creation.name);
}

void MirvCommands_InstallRegistrar() {
	// Keep the game-specific registrar: interface slots can diverge on independent updates.
	g_pfnRegisterConCommandOverride = &RegisterConCommandForDeadlock;
}

} // namespace Deadlock

// ---- is the console registration actually working? -------------------------------------------
//
// WHY THIS EXISTS. Every command in this file registers through `CON_COMMAND`, which ends in
// `ICvar::RegisterConCommand` -- a VTABLE call on an interface whose layout is assumed to match
// CS2's. Deadlock's `VEngineCvar007` answers to the same version string, but a matching string is
// not a matching vtable, and a wrong slot here does not fault: it calls some other virtual, returns,
// and the command simply never appears. That is indistinguishable from "the command was never
// written", and it is invisible over netcon, because netcon dispatches `mirv_deadlock` itself
// (Commands_DispatchLine) and never asks the engine. So the entire port could run for months on a
// console surface the game's own console cannot see -- which is exactly what was reported:
// typing `mirv` in the in-game console offers nothing.
//
// This probe answers the three questions that separate the causes, and it is reachable WITHOUT the
// engine, through `mirv_filmmaker cvarprobe`.
namespace Deadlock {

// The dangerous read, isolated. SEH cannot share a function with anything that needs unwinding,
// and the table is capped well below CS2's real count: this is asking "does reading work at all",
// not enumerating the console.
static void WalkCommandTable(SOURCESDK::CS2::ICvar* cvar, const char* const* wanted, int wantedCount,
                             bool* seen, int& total, int& foundOurs, bool& faulted) {
	__try {
		for (size_t i = 0; i < 4096; ++i) {
			SOURCESDK::CS2::CCmd* cmd = Icvar_GetCmd(cvar, i);
			if (nullptr == cmd) break;
			const SOURCESDK::int64 flags = cmd->GetFlags();
			if (flags == 0x400) break;
			++total;
			const char* name = cmd->GetName();
			if (!name) continue;
			for (int w = 0; w < wantedCount; ++w)
				if (!seen[w] && 0 == std::strcmp(name, wanted[w])) { seen[w] = true; ++foundOurs; }
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		faulted = true;
	}
}

// PURE READS. The vtable of a live interface, resolved to module + RVA, plus the opening bytes of
// each function so a slot can be recognised offline without a debugger attached to the game.
//
// This is the first step of deriving Deadlock's ICvar layout and it cannot fault: it dereferences
// one pointer the engine handed us and then walks an array of code addresses, checking each against
// the module list before reading it.
void MirvCommands_DumpCvarVtable(int slots) {
	SOURCESDK::CS2::ICvar* cvar = SOURCESDK::CS2::g_pCVar;
	if (!cvar) { advancedfx::Warning("cvar vtable: no ICvar pointer.\n"); return; }
	if (slots < 1) slots = 64;
	if (slots > 256) slots = 256;

	void** vtbl = nullptr;
	__try { vtbl = *(void***)cvar; } __except (EXCEPTION_EXECUTE_HANDLER) { vtbl = nullptr; }
	if (!vtbl) { advancedfx::Warning("cvar vtable: the interface pointer did not read back.\n"); return; }

	{
		HMODULE owner = nullptr;
		char name[MAX_PATH] = {};
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       (LPCSTR)vtbl, &owner) && owner) {
			GetModuleFileNameA(owner, name, MAX_PATH);
			const char* leaf = std::strrchr(name, '\\');
			advancedfx::Message("==== ICvar vtable @ %s +0x%zx ====\n",
			                    leaf ? leaf + 1 : name, (size_t)((unsigned char*)vtbl - (unsigned char*)owner));
		} else {
			advancedfx::Message("==== ICvar vtable @ %p (no owning module) ====\n", (void*)vtbl);
		}

		// THE CLASS NAME, off MSVC's RTTI. The slot just before a vtable points at the complete
		// object locator, whose type descriptor carries the decorated name. Knowing the class is
		// what makes the CS2 side findable: the same class in CS2's tier0.dll gives a vtable whose
		// slot order is already documented, and the two can then be cross-matched function by
		// function instead of guessed at.
		char rtti[256] = {};
		__try {
			void* col = *((void**)vtbl - 1);
			if (col && owner) {
				// 64-bit RTTI stores IMAGE-RELATIVE offsets, not pointers.
				const unsigned char* base = (const unsigned char*)owner;
				const unsigned pTypeDescriptor = *(const unsigned*)((const unsigned char*)col + 12);
				if (pTypeDescriptor) {
					const char* name = (const char*)(base + pTypeDescriptor + 16);
					std::strncpy(rtti, name, sizeof(rtti) - 1);
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) { rtti[0] = 0; }
		advancedfx::Message("  class: %s\n", rtti[0] ? rtti : "(no RTTI)");
	}

	for (int i = 0; i < slots; ++i) {
		void* fn = nullptr;
		__try { fn = vtbl[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { advancedfx::Message("  %3d  <unreadable>\n", i); break; }
		if (!fn) { advancedfx::Message("  %3d  null -- end of the table\n", i); break; }

		HMODULE owner = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        (LPCSTR)fn, &owner) || !owner) {
			// Past the end of the table the "entries" stop being code addresses at all, which is
			// how the table's length is read off rather than guessed.
			advancedfx::Message("  %3d  %p  not in any module -- table ends here\n", i, fn);
			break;
		}

		char path[MAX_PATH] = {};
		GetModuleFileNameA(owner, path, MAX_PATH);
		const char* leaf = std::strrchr(path, '\\');
		unsigned char b[12] = {};
		bool readable = true;
		__try { std::memcpy(b, fn, sizeof(b)); } __except (EXCEPTION_EXECUTE_HANDLER) { readable = false; }

		if (readable)
			advancedfx::Message("  %3d  %-14s +0x%08zx  %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
				i, leaf ? leaf + 1 : path, (size_t)((unsigned char*)fn - (unsigned char*)owner),
				b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
		else
			advancedfx::Message("  %3d  %-14s +0x%08zx  <bytes unreadable>\n",
				i, leaf ? leaf + 1 : path, (size_t)((unsigned char*)fn - (unsigned char*)owner));
	}
	advancedfx::Message("==== end ====\n");
}

// ---- deriving CCmd's layout FROM THE ENGINE'S OWN COMMANDS ------------------------------------
//
// Inspecting the creation descriptor and runtime command storage helped isolate registration:
// `CCmd` in the shared SDK header is a hand-written mirror of an internal engine struct, and
// Deadlock's CCvar has clearly moved on from CS2's.
//
// Disassembling the registrar is one way to find the new layout. Reading the engine's own commands
// is a better one, because Deadlock ships hundreds of correctly-built CCmd objects and `GetCmd` --
// the measured GetCmd slot -- hands them over. Print what is actually in them and the layout stops
// being a question: the field holding a pointer to the name is the name field, whatever offset it
// is at, and a field that is identical across every command is structure rather than data.
//
// ALL READS, every one of them guarded. Nothing here calls into the engine except GetCmd.
static bool SafeReadPtr(const void* at, void** out) {
	__try { *out = *(void**)at; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeAscii(const void* p, char* buf, size_t cap) {
	__try {
		const char* s = (const char*)p;
		size_t i = 0;
		for (; i + 1 < cap; ++i) {
			const char c = s[i];
			if (!c) break;
			if (c < 0x20 || (unsigned char)c > 0x7e) return false;   // not a printable C string
			buf[i] = c;
		}
		buf[i] = 0;
		return i > 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static const char* ModuleLeafFor(const void* addr, char* buf, size_t cap, size_t* rvaOut) {
	HMODULE owner = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCSTR)addr, &owner) || !owner)
		return nullptr;
	char path[MAX_PATH] = {};
	GetModuleFileNameA(owner, path, MAX_PATH);
	const char* leaf = std::strrchr(path, '\\');
	std::strncpy(buf, leaf ? leaf + 1 : path, cap - 1);
	buf[cap - 1] = 0;
	if (rvaOut) *rvaOut = (size_t)((const unsigned char*)addr - (const unsigned char*)owner);
	return buf;
}

void MirvCommands_DumpCmdStructs(int count, int qwords) {
	SOURCESDK::CS2::ICvar* cvar = SOURCESDK::CS2::g_pCVar;
	if (!cvar) { advancedfx::Warning("cmd structs: no ICvar pointer.\n"); return; }
	if (count < 1) count = 4;
	if (count > 32) count = 32;
	if (qwords < 4) qwords = 12;
	if (qwords > 32) qwords = 32;

	advancedfx::Message("==== live CCmd objects (GetCmd slot %zu), %d qwords each ====\n",
	                    kIcvarSlot_GetCmd, qwords);
	advancedfx::Message("  CS2 for reference: +0 name  +8 help  +16 flags  +24 callback  +56 next\n");

	int shown = 0;
	for (size_t i = 0; i < 4096 && shown < count; ++i) {
		SOURCESDK::CS2::CCmd* cmd = nullptr;
		__try { cmd = Icvar_GetCmd(cvar, i); } __except (EXCEPTION_EXECUTE_HANDLER) {
			advancedfx::Warning("  GetCmd(%zu) faulted -- slot %zu is not GetCmd after all.\n", i, kIcvarSlot_GetCmd);
			return;
		}
		if (!cmd) break;
		++shown;

		advancedfx::Message("  [%zu] %p\n", i, (void*)cmd);
		for (int q = 0; q < qwords; ++q) {
			void* v = nullptr;
			if (!SafeReadPtr((const unsigned char*)cmd + q * 8, &v)) {
				advancedfx::Message("    +%02d  <unreadable>\n", q * 8);
				break;
			}
			char mod[64] = {}; size_t rva = 0;
			char text[96] = {};
			const char* where = ModuleLeafFor(v, mod, sizeof(mod), &rva);
			const bool isText = SafeAscii(v, text, sizeof(text));
			if (isText)
				advancedfx::Message("    +%02d  %016llx  \"%s\"%s%s\n", q * 8, (unsigned long long)v, text,
				                    where ? "  in " : "", where ? where : "");
			else if (where)
				advancedfx::Message("    +%02d  %016llx  %s+0x%zx\n", q * 8, (unsigned long long)v, where, rva);
			else
				advancedfx::Message("    +%02d  %016llx\n", q * 8, (unsigned long long)v);
		}
	}
	if (0 == shown)
		advancedfx::Warning("  GetCmd returned nothing at all -- the table is empty or the slot is wrong.\n");
	advancedfx::Message("==== end ====\n");
}

void MirvCommands_ProbeRegistration() {
	advancedfx::Message("==== console registration probe ====\n");

	SOURCESDK::CS2::ICvar* cvar = SOURCESDK::CS2::g_pCVar;
	advancedfx::Message("  ICvar pointer:      %s\n", cvar ? "captured" : "NULL -- Connect never handed it over");
	if (!cvar) {
		advancedfx::Message(
			"  Nothing can register without it. It is taken in New_ClientConnect\n"
			"  (DeadlockMain.cpp) from the client's app-system factory as \"VEngineCvar007\".\n");
		return;
	}

	advancedfx::Message("  WrpRegisterCommands: %s\n",
	                    g_bComandsRegisterred ? "ran with a live ICvar" : "has NOT run (or ran with a null ICvar)");

	// WALK THE ENGINE'S OWN COMMAND TABLE and look for names we registered.
	//
	// GUARDED, AND THAT GUARD IS NOT PARANOIA -- this exact walk took Deadlock down the first time
	// it ran. `ICvar::GetCmd` is a vtable slot assumed to be CS2's; if the layout differs, the call
	// lands on some other virtual and what comes back is not a CCmd, so reading its flags is a read
	// through a garbage pointer. A crash here is itself the answer, but it is a terrible way to
	// deliver it.
	int total = 0;
	int foundOurs = 0;
	bool faulted = false;
	const char* wanted[] = { "mirv_filmmaker", "mirv_deadlock", "mirv_input", "mirv_skip", "mirv_cmd",
		"mirv_fov", "mirv_camio", "mirv_campath", "mirv_panorama", "mvm_debug" };
	constexpr int wantedCount = sizeof(wanted) / sizeof(wanted[0]);
	bool seen[wantedCount] = {};

	WalkCommandTable(cvar, wanted, wantedCount, seen, total, foundOurs, faulted);

	if (faulted) {
		advancedfx::Warning("  Command table read faulted after %d entries. Re-check the Deadlock ICvar adapter against this game build.\n", total);
		return;
	}

	advancedfx::Message("  engine command table: %d entries readable\n", total);
	for (int w = 0; w < wantedCount; ++w)
		advancedfx::Message("    %-16s %s\n", wanted[w], seen[w] ? "PRESENT" : "absent");

	if (total <= 0)
		advancedfx::Warning("  Command table is empty or unreadable. Verify GetCmd and engine initialization.\n");
	else if (0 == foundOurs)
		advancedfx::Warning("  None of the expected commands are present. Check registration handles in mvm_debug and the Deadlock ABI adapter.\n");
	else if (foundOurs == wantedCount)
		advancedfx::Message(
			"  Registration is working. A command that is PRESENT here but does not autocomplete\n"
			"  in the console is a console-filtering question, not a registration one.\n");
	else advancedfx::Warning("  Only %d of %d expected commands registered.\n", foundOurs, wantedCount);
}

} // namespace Deadlock

// ---- the commands ----------------------------------------------------------------------------

// THE ONE THAT MATTERS MOST. `mirv_input` is how every HLAE user configures the free camera --
// sensitivity, keyboard speeds, the camera-control mode, mouse behaviour. This target already
// hosts a complete MirvInput (DeadlockCameraRuntime.cpp implements IMirvInputDependencies), so the
// command is a forward to the very same object the free camera flies from. Nothing is duplicated.
CON_COMMAND(mirv_input, "Input mode configuration.") {
	// `camera` and `end` are the free camera's on and off -- the same verb as `mirv_filmmaker freecam`,
	// so both spellings go through one implementation. Everything else is MirvInput's own.
	if (args->ArgC() == 2
		&& (0 == _stricmp(args->ArgV(1), "camera") || 0 == _stricmp(args->ArgV(1), "end"))) {
		if (Deadlock::CameraRuntime_SetFreeCam(0 == _stricmp(args->ArgV(1), "camera"), "mirv_input")) return;
	}
	if (MirvInput* input = Deadlock::CameraRuntime_Input()) {
		input->ConCommand(args);
		return;
	}
	advancedfx::Warning(
		"mirv_input: the camera runtime is not up yet. It is created in the client's Init, so this\n"
		"can only happen before the game has finished starting.\n");
}

CON_COMMAND(mirv_skip, "for skipping through demos (uses demo_gototick)") {
	MirvSkip_ConsoleCommand(args, &g_DeadlockCampathTime, &g_DeadlockGotoDemoTick);
}

CON_COMMAND(mirv_cmd, "Command system (for scheduling commands).") {
	g_DeadlockCommandSystem.Console_Command(args);
}

CON_COMMAND(mirv_campath, "HLAE camera paths, sharing the Deadlock editor's markers.") {
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Command(args);
	Filmmaker::Shot_CampathCommand(args, &g_DeadlockCampathTime, &g_DeadlockCampathCamera);
}

CON_COMMAND(mirv_loadlibrary, "Load a DLL.") {
	if (2 > args->ArgC()) {
		advancedfx::Message("%s <dllPath> - load a DLL into the game.\n", args->ArgV(0));
		return;
	}
	std::wstring wide;
	if (!UTF8StringToWideString(args->ArgV(1), wide)) {
		advancedfx::Warning("mirv_loadlibrary: could not convert the path.\n");
		return;
	}
	if (0 != LoadLibraryW(wide.c_str())) advancedfx::Message("LoadLibraryW OK.\n");
	else advancedfx::Warning("LoadLibraryW FAILED (error %u).\n", (unsigned)GetLastError());
}

// The two cvar commands walk the engine's own command and cvar tables through the SAME ICvar
// pointer the console registration uses, so if `mirv_filmmaker health` can list commands at all,
// these can run. Whether Deadlock HAS a given cvar is a separate, runtime question.
CON_COMMAND(mirv_cvar_unhide_all, "Unlocks cmds and cvars.") {
	if (!SOURCESDK::CS2::g_pCVar) { advancedfx::Warning("mirv_cvar_unhide_all: no cvar interface.\n"); return; }
	const SOURCESDK::int64 mask =
		(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY | SOURCESDK_CS2_FCVAR_HIDDEN);

	int total = 0, changed = 0;
	for (size_t i = 0; i < 65536; ++i) {
		SOURCESDK::CS2::CCmd* cmd = Deadlock::Icvar_GetCmd(SOURCESDK::CS2::g_pCVar, i);
		if (nullptr == cmd) break;
		const SOURCESDK::int64 flags = cmd->GetFlags();
		if (flags == 0x400) break;
		++total;
		if (flags & mask) { cmd->SetFlags(flags & ~mask); ++changed; }
	}
	advancedfx::Message("==== Cmds total: %i (Cmds unhidden: %i) ====\n", total, changed);

	total = changed = 0;
	for (size_t i = 0; i < 65536; ++i) {
		SOURCESDK::CS2::Cvar_s* cvar = Deadlock::Icvar_GetCvar(SOURCESDK::CS2::g_pCVar, i);
		if (nullptr == cvar) break;
		++total;
		if (cvar->m_nFlags & mask) { cvar->m_nFlags &= ~mask; ++changed; }
	}
	advancedfx::Message("==== Cvars total: %i (Cvars unhidden: %i) ====\n", total, changed);
}

CON_COMMAND(mirv_cvar_unlock_sv_cheats, "Unlocks sv_cheats on client (as much as possible).") {
	if (!SOURCESDK::CS2::g_pCVar) { advancedfx::Warning("mirv_cvar_unlock_sv_cheats: no cvar interface.\n"); return; }
	const SOURCESDK::int64 cheat = (SOURCESDK::int64)SOURCESDK_CS2_FCVAR_CHEAT;

	int total = 0, changed = 0;
	for (size_t i = 0; i < 65536; ++i) {
		SOURCESDK::CS2::CCmd* cmd = Deadlock::Icvar_GetCmd(SOURCESDK::CS2::g_pCVar, i);
		if (nullptr == cmd) break;
		const SOURCESDK::int64 flags = cmd->GetFlags();
		if (flags == 0x400) break;
		++total;
		if (flags & cheat) { cmd->SetFlags(flags & ~cheat); ++changed; }
	}
	advancedfx::Message("==== Cmds total: %i (Cmds unlocked: %i) ====\n", total, changed);

	total = changed = 0;
	bool sawSvCheats = false;
	for (size_t i = 0; i < 65536; ++i) {
		SOURCESDK::CS2::Cvar_s* cvar = Deadlock::Icvar_GetCvar(SOURCESDK::CS2::g_pCVar, i);
		if (nullptr == cvar) break;
		++total;
		if (cvar->m_nFlags & cheat) { cvar->m_nFlags &= ~cheat; ++changed; }
		if (cvar->m_pszName && 0 == std::strcmp("sv_cheats", cvar->m_pszName)) {
			sawSvCheats = true;
			cvar->m_nFlags &= ~(SOURCESDK::int64)(SOURCESDK_CS2_FCVAR_REPLICATED | SOURCESDK_CS2_FCVAR_NOTIFY);
			cvar->m_nFlags |= SOURCESDK_CS2_FCVAR_CLIENTDLL;
			// Deadlock's value storage has extra callback fields before it. Use the engine
			// setter rather than writing the CS2 m_Value offset into unrelated storage.
		}
	}
	advancedfx::Message("==== Cvars total: %i (Cvars unlocked: %i) ====\n", total, changed);
	if (!sawSvCheats)
		advancedfx::Warning("  no `sv_cheats` cvar in this build -- the flag clearing still ran.\n");
	else Deadlock::Engine_ExecuteClientCmd("sv_cheats 1");
}
