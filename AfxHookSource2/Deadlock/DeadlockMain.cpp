// AfxHookDeadlock: entry point and module dispatch.
//
// The shape is inherited from the CS2 hook because it is the right shape for a Source 2 game:
// rewrite the host executable's import of LoadLibrary, then catch each engine module as it loads
// and install that module's hooks exactly once. Deadlock's module NAMES are identical to CS2's --
// tier0, engine2, scenesystem, materialsystem2, rendersystemdx11, panorama, schemasystem -- so the
// suffix matching transfers unchanged. The one literal that differs is the client, which lives
// under Deadlock's mod directory: citadel/bin/win64/client.dll, not csgo/bin/win64/client.dll.
//
// CreateInterface is intercepted through a hooked GetProcAddress on tier0, the same way, and this
// is where the port got its biggest free ride: every interface version string in Deadlock is
// byte-identical to CS2's, verified against the installed build by
// Automation/verify/verify-deadlock-signatures.py. Source2Client002, Source2EngineToClient001,
// ResourceSystem013, VFileSystem017. Nothing to re-derive.

#include "DeadlockAddresses.h"
#include "DeadlockEntities.h"
#include "DeadlockBoot.h"
#include "DeadlockCameraRuntime.h"
#include "DeadlockDebug.h"
#include "DeadlockEngine.h"
#include "DeadlockFrame.h"
#include "DeadlockNetcon.h"
#include "DeadlockPresentHook.h"
#include "DeadlockPreUiHook.h"
#include "DeadlockUi.h"
#include "DeadlockViewSetup.h"
#include "../Filmmaker/Debug/MvmDebugLog.h"

#include "../ConsoleCapture.h"
#include "DeadlockUiCommandBridge.h"
#include "../Globals.h"
#include "../WrpConsole.h"
#include "../../Shared/AfxCommandLine.h"
#include "../../Shared/AfxConsole.h"
#include "../../Shared/AfxDetours.h"
#include "../../Shared/StringTools.h"

// Defined in DeadlockMirvCommands.cpp. Declared here rather than in a header because it is called
// from exactly one place, the client's Connect, and must be called there and nowhere else.
namespace Deadlock { void MirvCommands_InstallRegistrar(); }
#include "../Filmmaker/MouseCapture.h"

#include <windows.h>
#include <cstdint>

// ---------------------------------------------------------------------------
// Globals the shared and Deadlock-side code expects.
// ---------------------------------------------------------------------------

advancedfx::CCommandLine* g_CommandLine = nullptr;

namespace {

HMODULE g_hSelf = nullptr;

bool g_disableViewHook = false;
bool g_disableViewBody = false;
bool g_disablePresentHook = false;
bool g_disableUi = false;

// Connect/Init establish the interface. The frame-stage slot is additionally checked against
// the current binary signature before any pointer is replaced; an interface name alone is not
// an ABI guarantee. Build 6774's stage 12 was measured through the read-only view probe and its
// dispatcher jump table. Panorama work stays on that same initialization thread.
const int kClientConnectSlot = 0;
const int kClientInitSlot = 3;

typedef int(__fastcall* ClientConnect_t)(void* thisPtr, void* appSystemFactory);
typedef int(__fastcall* ClientInit_t)(void* thisPtr);

ClientConnect_t g_oldClientConnect = nullptr;
ClientInit_t g_oldClientInit = nullptr;

using ClientFrameStageFn = void(__fastcall*)(void*, int);
ClientFrameStageFn g_oldClientFrameStage = nullptr;
DWORD g_clientInitThread = 0;

void __fastcall New_ClientFrameStage(void* thisPtr, int stage) {
	g_oldClientFrameStage(thisPtr, stage);
	if (stage != 12) return;
	if (GetCurrentThreadId() != g_clientInitThread) {
		static volatile LONG warned = 0;
		if (InterlockedCompareExchange(&warned, 1, 0) == 0) {
			DL_BOOT("main pump: frame-stage thread mismatch; refusing UI work");
		}
		return;
	}
	Deadlock::Frame_MainThread();
	if (Filmmaker::MvmDebugLog_Active() && Deadlock::Frame_MainCount() % 256 == 1)
		Filmmaker::MvmDebugLog_Linef("deadlock.main.frame", "stage=%d thread=%lu frames=%llu",
			stage, GetCurrentThreadId(), Deadlock::Frame_MainCount());
}

bool InstallClientFrameStage(void* iface) {
	if (g_oldClientFrameStage) return true;
	if (!iface || !Deadlock::g_ClientFrameStageFn) return false;
	void** table = *(void***)iface;
	if (!table || (size_t)table[36] != Deadlock::g_ClientFrameStageFn) return false;
	g_clientInitThread = GetCurrentThreadId();
	return AfxDetourPtr((PVOID*)&table[36], New_ClientFrameStage, (PVOID*)&g_oldClientFrameStage);
}

int __fastcall New_ClientConnect(void* thisPtr, void* appSystemFactory) {
	static bool first = true;
	if (first) {
		first = false;
		DL_BOOT("client Connect");
		Deadlock::Engine_Connect(appSystemFactory);

		typedef void*(*CreateInterfaceFn)(const char*, int*);
		CreateInterfaceFn factory = (CreateInterfaceFn)appSystemFactory;
		if (factory) {
			// The cvar interface is what console commands register into, so nothing works before
			// this. Same version string as CS2.
			SOURCESDK::CS2::g_pCVar = SOURCESDK::CS2::cvar =
				(SOURCESDK::CS2::ICvar*)factory("VEngineCvar007", nullptr);

			// Interface names do not establish ABI compatibility. Deadlock previously used 48
			// slots and now uses 46; install its independently derived registrar before any
			// command attempts registration.
			// See DeadlockIcvarSlots.h and Automation/verify/derive-deadlock-icvar-slots.py.
			Deadlock::MirvCommands_InstallRegistrar();
		}
	}
	return g_oldClientConnect ? g_oldClientConnect(thisPtr, appSystemFactory) : 0;
}

int __fastcall New_ClientInit(void* thisPtr) {
	DL_BOOT("client Init: calling through");
	const int result = g_oldClientInit ? g_oldClientInit(thisPtr) : 0;
	DL_BOOT("client Init: original returned");

	// Permit mvm_debug evidence during registration/startup, before F7 or netcon can run.
	if (g_CommandLine && g_CommandLine->FindParam(L"-mvmDebug")) {
		Deadlock::Debug_Install();
		advancedfx::CFakeCommandArgs debugArgs("mvm_debug");
		debugArgs.AddArg("start");
		Deadlock::Debug_Command(&debugArgs);
	}

	// Commands can only be registered once the cvar interface exists, which Connect above gave us.
	WrpRegisterCommands();

	DL_BOOT("client Init: commands registered");
	const bool clientAddressesOk = Deadlock::Addresses_InitClient(Deadlock::g_H_Client);
	DL_BOOT(clientAddressesOk ? "client Init: client addresses resolved"
	                         : "client Init: required client addresses did NOT resolve");
	if (g_CommandLine && (g_CommandLine->FindParam(L"-mvmNoMainFrame") ||
	                     g_CommandLine->FindParam(L"-mvmViewProbe"))) {
		DL_BOOT("client Init: main frame hook SKIPPED (diagnostic switch)");
	} else {
		const bool frameOk = InstallClientFrameStage(thisPtr);
		DL_BOOT(frameOk ? "client Init: main frame hook installed"
		               : "client Init: main frame hook FAILED; main pump unavailable");
	}
	// The entity list. Its own module and its own scan: losing it costs the follow camera and
	// nothing else, so it must not be able to fail the client address block.
	// -mvmNoEntities keeps the whole entity module cold, for A/B frame-rate runs and for isolating a
	// crash to it. Without it the list resolves here; nothing reads entities per frame until a feature
	// asks.
	if (g_CommandLine && g_CommandLine->FindParam(L"-mvmNoEntities")) {
		DL_BOOT("client Init: entity integration SKIPPED (-mvmNoEntities)");
	} else {
		const bool entitiesOk = Deadlock::Entities_InitClient(Deadlock::g_H_Client);
		DL_BOOT(entitiesOk ? "client Init: entity list resolved" : "client Init: entity list did NOT resolve");
	}
	if (g_CommandLine && g_CommandLine->FindParam(L"-mvmViewProbe")) {
		const bool probeOk = Deadlock::ViewSetup_InstallProbe();
		DL_BOOT(probeOk ? "client Init: read-only view probe installed; main pump unavailable"
		               : "client Init: read-only view probe FAILED");
	} else if (!g_disableViewHook) {
		const bool viewOk = Deadlock::ViewSetup_Install();
		if (viewOk && g_disableViewBody) Deadlock::ViewSetup_SetEnabled(false);
		DL_BOOT(!viewOk ? "client Init: view setup FAILED; camera unavailable"
			: g_disableViewBody ? "client Init: view detour installed, body OFF"
			: "client Init: view setup installed");
	} else {
		DL_BOOT("client Init: view setup SKIPPED (-mvmNoViewDetour)");
	}

	// Deliberately here rather than at module-load time: see the note in the dispatcher.
	if (!g_disablePresentHook) {
		const bool presentOk = Deadlock::PresentHook_Install(Deadlock::g_H_RenderSystemDx11);
		DL_BOOT(presentOk ? "client Init: present hook installed"
		                  : "client Init: present hook FAILED");
	} else {
		DL_BOOT("client Init: present hook SKIPPED (-mvmNoPresentHook)");
	}

	// The panel's only route to the console. Started here, beside netcon, because it uses the same
	// tier0 logging machinery and has the same prerequisite: the console has to exist first.
	if (Deadlock::UiCommandBridge_Start())
		DL_BOOT("client Init: ui command bridge listening");
	else
		DL_BOOT("client Init: ui command bridge FAILED; every panel button will be inert");

	// Netcon comes up here rather than in DllMain: a listener that answers before the console
	// exists would accept commands it cannot execute, and automation waits on our banner as proof
	// the DLL is actually live rather than merely that a port is open.
	unsigned short port = 0;
	if (g_CommandLine) {
		if (int idx = g_CommandLine->FindParam(L"-mvmnetconport")) {
			if (idx + 1 < g_CommandLine->GetArgC())
				port = (unsigned short)wcstoul(g_CommandLine->GetArgV(idx + 1), nullptr, 10);
		}
	}
	// Report what actually happened, not that we tried. An unconditional "started" line is how
	// a failed bind reads as a working listener for launch after launch.
	if (Deadlock::Netcon_Start(port))
		DL_BOOT("client Init: netcon LISTENING; hook is live");
	else
		DL_BOOT("client Init: netcon FAILED TO BIND; hook live but unreachable");

	// The exit trace goes in here with the other hooks, off the loader lock. Deadlock ends a bad
	// frame by exiting cleanly rather than crashing, so a detour on ExitProcess is the only thing
	// that can say who decided to leave.
	Deadlock::Debug_Install();

	advancedfx::Message(
		"AfxHookDeadlock loaded. 'mirv_filmmaker health' for status, "
		"'mirv_filmmaker workspace camera' to open the editor.\n");
	return result;
}

void HookClientInterface(void* iface) {
	if (!iface) return;
	void** vtable = *(void***)iface;
	if (!vtable) return;
	AfxDetourPtr((PVOID*)&(vtable[kClientConnectSlot]), New_ClientConnect,
	             (PVOID*)&g_oldClientConnect);
	AfxDetourPtr((PVOID*)&(vtable[kClientInitSlot]), New_ClientInit, (PVOID*)&g_oldClientInit);
}

typedef void*(*CreateInterfaceFn_t)(const char*, int*);
CreateInterfaceFn_t g_oldClientCreateInterface = nullptr;

void* New_ClientCreateInterface(const char* name, int* returnCode) {
	static bool first = true;
	if (first) DL_BOOT("client CreateInterface intercepted");
	void* result = g_oldClientCreateInterface ? g_oldClientCreateInterface(name, returnCode)
	                                          : nullptr;
	if (first && g_oldClientCreateInterface) {
		first = false;
		if (void* iface = g_oldClientCreateInterface("Source2Client002", nullptr))
			HookClientInterface(iface);
		else
			advancedfx::Warning(
				"AfxHookDeadlock: Source2Client002 not available; no UI and no camera.\n");
	}
	return result;
}

FARPROC WINAPI New_Tier0_GetProcAddress(HMODULE module, LPCSTR procName) {
	FARPROC result = GetProcAddress(module, procName);
	if (!result || !HIWORD(procName)) return result;

	if (0 == lstrcmpA(procName, "GetProcAddress"))
		return (FARPROC)&New_Tier0_GetProcAddress;

	if (module == Deadlock::g_H_Client && 0 == lstrcmpA(procName, "CreateInterface")) {
		g_oldClientCreateInterface = (CreateInterfaceFn_t)result;
		return (FARPROC)&New_ClientCreateInterface;
	}

	return result;
}

HMODULE WINAPI New_LoadLibraryA(LPCSTR name);
HMODULE WINAPI New_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags);
HMODULE WINAPI New_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags);

CAfxImportFuncHook<FARPROC(WINAPI*)(HMODULE, LPCSTR)>
	g_Import_tier0_GetProcAddress("GetProcAddress", &New_Tier0_GetProcAddress);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR)>
	g_Import_tier0_LoadLibraryA("LoadLibraryA", &New_LoadLibraryA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)>
	g_Import_tier0_LoadLibraryExA("LoadLibraryExA", &New_LoadLibraryExA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD)>
	g_Import_tier0_LoadLibraryExW("LoadLibraryExW", &New_LoadLibraryExW);

CAfxImportDllHook g_Import_tier0_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_tier0_GetProcAddress,
	&g_Import_tier0_LoadLibraryA,
	&g_Import_tier0_LoadLibraryExA,
	&g_Import_tier0_LoadLibraryExW }));

CAfxImportsHook g_Import_tier0(CAfxImportsHooks({ &g_Import_tier0_KERNEL32 }));

CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR)>
	g_Import_process_LoadLibraryA("LoadLibraryA", &New_LoadLibraryA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD)>
	g_Import_process_LoadLibraryExA("LoadLibraryExA", &New_LoadLibraryExA);
CAfxImportFuncHook<HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD)>
	g_Import_process_LoadLibraryExW("LoadLibraryExW", &New_LoadLibraryExW);

CAfxImportDllHook g_Import_process_KERNEL32("KERNEL32.dll", CAfxImportDllHooks({
	&g_Import_process_LoadLibraryA,
	&g_Import_process_LoadLibraryExA,
	&g_Import_process_LoadLibraryExW }));

CAfxImportsHook g_Import_process(CAfxImportsHooks({ &g_Import_process_KERNEL32 }));

// Each branch fires once. Suffix matching on the path the game passed to LoadLibrary is what makes
// this robust against absolute-vs-relative and case differences.
void OnModuleLoaded(HMODULE module, LPCWSTR path) {
	if (!module || !path) return;

	static bool firstTier0 = true;
	static bool firstEngine2 = true;
	static bool firstClient = true;
	static bool firstPanorama = true;
	static bool firstSchemaSystem = true;
	static bool firstResourceSystem = true;
	static bool firstFileSystem = true;
	static bool firstSceneSystem = true;
	static bool firstMaterialSystem = true;
	static bool firstRenderSystem = true;

	if (firstTier0 && StringEndsWithW(path, L"tier0.dll")) {
		firstTier0 = false;
		DL_BOOT("module: tier0");
		g_Import_tier0.Apply(module);
		Deadlock::Addresses_InitTier0(module);

		SOURCESDK::CS2::g_pMemAlloc =
			*(SOURCESDK::CS2::IMemAlloc**)GetProcAddress(module, "g_pMemAlloc");

		// Route our own console output through tier0's, so netcon and the game console see the
		// same stream.
		if ((Tier0_Message = (advancedfx::Con_Printf_t)GetProcAddress(module, "Msg")))
			advancedfx::Message = My_Console_Message;
		if ((Tier0_Warning = (advancedfx::Con_Printf_t)GetProcAddress(module, "Warning")))
			advancedfx::Warning = My_Console_Warning;
		return;
	}

	if (firstEngine2 && StringEndsWithW(path, L"engine2.dll")) {
		firstEngine2 = false;
		DL_BOOT("module: engine2");
		Deadlock::Addresses_InitEngine2(module);
		return;
	}

	// The one literal that had to change from the CS2 hook. Deadlock's mod directory is citadel,
	// so its client is citadel/bin/win64/client.dll -- matching bare "client.dll" would also catch
	// unrelated modules on some systems, which is why the CS2 hook matches the game-dir path too.
	if (firstClient && StringEndsWithW(path, L"citadel\\bin\\win64\\client.dll")) {
		firstClient = false;
		Deadlock::g_H_Client = module;
		return;
	}

	if (firstPanorama && StringEndsWithW(path, L"panorama.dll")) {
		firstPanorama = false;
		DL_BOOT("module: panorama (scanning RunScript)");
		const bool panoramaOk = Deadlock::Addresses_InitPanorama(module);
		DL_BOOT(panoramaOk ? "module: panorama addresses resolved"
		                   : "module: required panorama addresses did NOT resolve");
		return;
	}

	if (firstSchemaSystem && StringEndsWithW(path, L"schemasystem.dll")) {
		firstSchemaSystem = false;
		Deadlock::g_H_SchemaSystem = module;
		return;
	}

	if (firstResourceSystem && StringEndsWithW(path, L"resourcesystem.dll")) {
		firstResourceSystem = false;
		Deadlock::g_H_ResourceSystem = module;
		return;
	}

	if (firstFileSystem && StringEndsWithW(path, L"filesystem_stdio.dll")) {
		firstFileSystem = false;
		Deadlock::g_H_FileSystemStdio = module;
		return;
	}

	if (firstSceneSystem && StringEndsWithW(path, L"scenesystem.dll")) {
		firstSceneSystem = false;
		Deadlock::Addresses_InitSceneSystem(module);
		// The pre-UI hook now restores only the D3D viewport for Panorama. Its old custom compositor
		// remains disabled because binding a custom vertex buffer from this boundary overflowed
		// Deadlock's 16-bit vertex-buffer tracking list.
		const bool preUiOk = Deadlock::PreUiHook_Install(module);
		DL_BOOT(preUiOk ? "module: scenesystem; pre-ui hook installed"
		                : "module: scenesystem; pre-ui hook FAILED");
		return;
	}

	if (firstMaterialSystem && StringEndsWithW(path, L"materialsystem2.dll")) {
		firstMaterialSystem = false;
		Deadlock::g_H_MaterialSystem2 = module;
		return;
	}

	if (firstRenderSystem && StringEndsWithW(path, L"rendersystemdx11.dll")) {
		firstRenderSystem = false;
		DL_BOOT("module: rendersystemdx11");
		Deadlock::g_H_RenderSystemDx11 = module;
		// NOTE the absence of PresentHook_Install here, and do not put it back.
		//
		// This function runs inside LoadLibraryExW, which means it runs UNDER THE LOADER LOCK.
		// Installing the present hook creates a D3D11 device and registers a window class to read
		// one vtable entry, and doing that from under the loader lock takes the process down before
		// it draws a frame -- silently, with no dump and no log, which is indistinguishable from
		// the game simply refusing to start. It is installed from the client's Init instead, where
		// the render system is fully up and nothing is holding the loader.
		return;
	}

	// Vulkan instead of DX11 is not a degraded mode, it is no mode: every render hook here is a
	// D3D11 hook, so the whole render pump would silently never run. Say so rather than looking
	// broken later.
	if (StringEndsWithW(path, L"rendersystemvulkan.dll")) {
		advancedfx::Warning(
			"AfxHookDeadlock: Deadlock is loading the VULKAN renderer. Every render hook in this\n"
			"DLL is a D3D11 hook, so the render pump will never run and the editor will not draw.\n"
			"Launch Deadlock without -vulkan.\n");
		return;
	}
}

HMODULE WINAPI New_LoadLibraryA(LPCSTR name) {
	HMODULE result = LoadLibraryA(name);
	if (result && name) {
		WCHAR wide[MAX_PATH * 2];
		if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2))
			OnModuleLoaded(result, wide);
	}
	return result;
}

HMODULE WINAPI New_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
	HMODULE result = LoadLibraryExA(name, file, flags);
	if (result && name) {
		WCHAR wide[MAX_PATH * 2];
		if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH * 2))
			OnModuleLoaded(result, wide);
	}
	return result;
}

HMODULE WINAPI New_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
	HMODULE result = LoadLibraryExW(name, file, flags);
	OnModuleLoaded(result, name);
	return result;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
	switch (reason) {
	case DLL_PROCESS_ATTACH: {
		Deadlock::Boot_Begin();
		DL_BOOT("DllMain PROCESS_ATTACH");
		g_hSelf = (HMODULE)instance;
		DisableThreadLibraryCalls((HMODULE)instance);

		// BEFORE anything can print. ConsoleCapture's My_Console_Message dereferences
		// g_ConsolePrinter unconditionally, and the moment tier0 loads we point
		// advancedfx::Message at it -- so a null printer turns the first status line into an
		// access violation inside LoadLibrary, under the loader lock. The game then dies with no
		// window, no dump and no log, which reads as "the hook does not work" rather than as a
		// one-line omission. This is that one line.
		g_ConsolePrinter = new CConsolePrinter();
		DL_BOOT("console printer allocated");

		g_CommandLine = new advancedfx::CCommandLine();
		DL_BOOT("command line parsed");

		// FOUND LIVE 2026-09-14 while chasing "free cam won't move (mouse-look still works)": this
		// click-to-capture scheme is CS2-only. It stays armed (MouseCapture_IsFree()==true, which
		// alone makes GetSuspendMirvInput() true -- see DeadlockCameraRuntime.cpp) until
		// MouseCapture_OnGameClick() fires, and that call exists only in CS2's WindowInputHooks.cpp,
		// which is not part of this target (grep confirms zero references to MouseCapture_OnGameClick/
		// OnActivate/RunCommand anywhere under AfxHookSource2/Deadlock or AfxHookSource2/Filmmaker/
		// Deadlock). So with the CS2 default (clickToCapture=true), a Deadlock session would arm
		// "free" at boot and NEVER be able to clear it -- MirvInput permanently suspended for this
		// reason alone, regardless of anything else. Mouse-look still partly working in practice
		// traces to Supply_SetCursorPos's baseline re-sync having no suspend check of its own (see
		// the comment at CameraRuntime_MirvInputSuspended's declaration); WASD has no such recovery,
		// which is what actually got reported.
		//
		// Deadlock does not need this concept at all: its own native cursor-mode toggle (J), tracked
		// through Input_UiMouse()/Ui_AnyPanelWantsCursor(), already answers "does a UI element own
		// the cursor right now" -- the exact question click-to-capture exists to answer for CS2.
		// clickToCapture=false here (never -mvmGrabMouse-gated) keeps MouseCapture_IsFree() a
		// permanent, harmless `false`, leaving Ui_AnyPanelWantsCursor() as the one real, functioning
		// suspend source.
		Filmmaker::MouseCapture_Initialize(false);
		DL_BOOT("mouse capture initialised");

		if (!g_CommandLine->FindParam(L"-insecure")) {
			MessageBoxA(0,
				"Add -insecure to Deadlock's launch options. AfxHookDeadlock refuses to load "
				"without it.",
				"AfxHookDeadlock", MB_OK | MB_ICONERROR);
			HANDLE self = OpenProcess(PROCESS_TERMINATE, TRUE, GetCurrentProcessId());
			TerminateProcess(self, 0);
			CloseHandle(self);
			return FALSE;
		}

		// -mvmInert: return from DllMain having done NOTHING but load. No import rewrite, so no
		// module dispatch, so no hooks at all. The DLL is present in the process and inert.
		//
		// This exists because the Windows event log showed no crash: Deadlock was EXITING CLEANLY
		// a second after the hook came up. A clean exit is a decision, not a fault, and the first
		// thing worth knowing is whether the game objects to the DLL being there at all or to
		// something it does. One launch answers that; without it, every subsystem is a suspect.
		if (g_CommandLine->FindParam(L"-mvmInert")) {
			DL_BOOT("-mvmInert: doing nothing, returning from DllMain");
			break;
		}

		// The root of the whole hook graph: rewriting the host executable's LoadLibrary imports is
		// what lets every module below be caught at load time rather than raced for afterwards.
		DL_BOOT("applying exe import hooks");
		g_Import_process.Apply(GetModuleHandle(NULL));
		DL_BOOT("exe import hooks applied");

		// Modules already mapped by the time we were injected. The launcher creates the process
		// suspended and injects before the first instruction runs, so normally this finds only
		// tier0 -- but a manual attach to a running game finds everything, and skipping this would
		// make that path silently do nothing.
		{
			static const wchar_t* const kAlready[] = {
				L"tier0.dll", L"engine2.dll", L"panorama.dll", L"schemasystem.dll",
				L"resourcesystem.dll", L"filesystem_stdio.dll", L"scenesystem.dll",
				L"materialsystem2.dll", L"rendersystemdx11.dll",
			};
			// Same loader-lock caution applies transitively: OnModuleLoaded must stay cheap,
			// because DllMain is itself running under the loader lock when we are injected into a
			// suspended process.
			for (const wchar_t* name : kAlready) {
				if (HMODULE m = GetModuleHandleW(name)) OnModuleLoaded(m, name);
			}
			if (HMODULE m = GetModuleHandleW(L"client.dll"))
				OnModuleLoaded(m, L"citadel\\bin\\win64\\client.dll");
		}

		Deadlock::CameraRuntime_Initialize();

		// Bisect switches, read here so they apply before anything installs. Every hook in this
		// DLL starts life as a hypothesis about a function signature or a struct layout, and the
		// failure mode is a process that dies before it draws a frame -- no window, no dump, no
		// log. Being able to turn one hook off per launch is the difference between localising
		// that in two launches and guessing at it for an afternoon.
		// -mvmNoViewHook disables the camera BODY but still installs the detour, because the
		// detour is also what drives the main-thread pump -- and a flag that silently takes
		// netcon down with the camera makes every bisect ambiguous. That cost a launch to
		// notice: netcon accepted commands and never executed one, because nothing was pumping.
		if (g_CommandLine->FindParam(L"-mvmNoViewHook")) {
			g_disableViewBody = true;
			DL_BOOT("-mvmNoViewHook: camera body off, detour still installed");
		}
		// The harder switch, for when the detour itself is the suspect. This DOES take the main
		// pump with it, so netcon will connect and never answer.
		if (g_CommandLine->FindParam(L"-mvmNoViewDetour")) {
			g_disableViewHook = true;
			DL_BOOT("-mvmNoViewDetour: no detour at all (netcon will not answer)");
		}
		if (g_CommandLine->FindParam(L"-mvmNoPresentHook")) {
			g_disablePresentHook = true;
			DL_BOOT("-mvmNoPresentHook: the present hook will not be installed");
		}
		if (g_CommandLine->FindParam(L"-mvmNoBootTrace")) {
			DL_BOOT("-mvmNoBootTrace: boot trace off from here");
			Deadlock::Boot_Enabled() = false;
		}
		if (g_CommandLine->FindParam(L"-mvmNoUi")) {
			g_disableUi = true;
			Deadlock::Frame_SetUiDisabled(true);
			DL_BOOT("-mvmNoUi: the Panorama panels will not be built");
		}

		DL_BOOT("camera runtime up; DllMain done");
		break;
	}

	case DLL_PROCESS_DETACH:
		Deadlock::Debug_OnProcessDetach();
		Deadlock::Netcon_Stop();
		break;
	}
	return TRUE;
}
