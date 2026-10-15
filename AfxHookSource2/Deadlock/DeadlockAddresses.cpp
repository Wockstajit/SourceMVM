#include "DeadlockAddresses.h"

#include "../Globals.h"
#include "../../Shared/AfxConsole.h"
#include "../../Shared/binutils.h"

#include <cstring>
#include <vector>

namespace Deadlock {

HMODULE g_H_Tier0 = nullptr;
HMODULE g_H_Engine2 = nullptr;
HMODULE g_H_Client = nullptr;
HMODULE g_H_Panorama = nullptr;
HMODULE g_H_SchemaSystem = nullptr;
HMODULE g_H_ResourceSystem = nullptr;
HMODULE g_H_FileSystemStdio = nullptr;
HMODULE g_H_SceneSystem = nullptr;
HMODULE g_H_MaterialSystem2 = nullptr;
HMODULE g_H_RenderSystemDx11 = nullptr;

void* g_Panorama_RunScript = nullptr;
unsigned char** g_pUIEngine = nullptr;
unsigned char** g_pHudPanel = nullptr;
unsigned char** g_DemoPlayerPointer = nullptr;
size_t g_DemoPlayerVtable = 0;
unsigned char** g_GlobalVars = nullptr;
size_t g_ClientViewSetupFn = 0;
size_t g_ClientFrameStageFn = 0;
size_t g_ViewVtable = 0;
size_t g_ClientModeVtable = 0;
size_t g_ViewSetupCallerReturn = 0;

namespace {

// One row per address, so health can report the whole set without each Init* having to remember
// to register anything.
struct Entry {
	const char* name;
	const char* module;
	bool required;
	bool resolved;
	size_t rva;
	size_t expectedRva;
};

std::vector<Entry>& Entries() {
	static std::vector<Entry> entries;
	return entries;
}

void Record(const char* name, const char* module, bool required, size_t absolute,
            HMODULE base, size_t expectedRva) {
	Entry e;
	e.name = name;
	e.module = module;
	e.required = required;
	e.resolved = absolute != 0;
	e.rva = (absolute && base) ? (absolute - (size_t)base) : 0;
	e.expectedRva = expectedRva;
	Entries().push_back(e);

	if (!e.resolved) {
		advancedfx::Warning("AfxHookDeadlock: %s (%s) did not resolve.\n", name, module);
		return;
	}
	if (expectedRva && e.rva != expectedRva) {
		// Not an error. A unique match at a new RVA is exactly what a game update looks like, and
		// the point of recording the old one is to make that visible instead of invisible.
		advancedfx::Message("AfxHookDeadlock: %s moved to rva 0x%zx (was 0x%zx).\n",
		                    name, e.rva, expectedRva);
	}
}

// Reads a RIP-relative disp32 at `at` and resolves it to the absolute target. `insnEnd` is where
// the referencing instruction ends, which is what the displacement is relative to.
unsigned char** RipTarget(size_t at, size_t insnEnd) {
	if (!at) return nullptr;
	int32_t disp = 0;
	std::memcpy(&disp, (void*)at, sizeof(disp));
	return (unsigned char**)(insnEnd + disp);
}

} // namespace

bool Addresses_InitTier0(HMODULE tier0Dll) {
	g_H_Tier0 = tier0Dll;
	// Nothing pattern-scanned in tier0: everything the hook wants from it is an export, resolved
	// by name in DeadlockMain.cpp. Kept as a hook point so a future tier0 scan has a home.
	return true;
}

bool Addresses_InitEngine2(HMODULE engine2Dll) {
	g_H_Engine2 = engine2Dll;
	{
		size_t hit = getAddress(engine2Dll, "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 58 84 C0 0F 85 ?? ?? ?? ?? 83 EF 03");
		g_DemoPlayerPointer = (hit ? RipTarget(hit + 3, hit + 7) : nullptr);
		Record("demo_player_pointer", "engine2", true, (size_t)g_DemoPlayerPointer, engine2Dll, 0x61b618);
	}
	{
		size_t hit = getAddress(engine2Dll, "48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 41 08");
		g_DemoPlayerVtable = (size_t)(hit ? RipTarget(hit + 3, hit + 7) : nullptr);
		Record("demo_player_vtable", "engine2", true, (size_t)g_DemoPlayerVtable, engine2Dll, 0x535730);
	}
	// The demo interface is reached through Source2EngineToClient001, not through a scan, so there
	// is nothing to resolve here yet. The vtable slots live in DeadlockEngine.cpp.
	return true;
}

bool Addresses_InitPanorama(HMODULE panoramaDll) {
	g_H_Panorama = panoramaDll;

	// Build 6774: compile+run profiling string, with the entry recovered through .pdata.
	// Keep the trailing source-register move to distinguish the script entry from other frames.
	g_Panorama_RunScript = (void*)getAddress(panoramaDll,
		"4C 89 4C 24 ?? 4C 89 44 24 ?? 48 89 54 24 ?? 55 53 56 57 41 54 41 56 41 57 "
		"48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? "
		"48 C7 44 24 ?? ?? ?? ?? ?? 48 89 44 24 ?? 4D 8B E0");
	Record("panorama_RunScript", "panorama", true,
	       (size_t)g_Panorama_RunScript, panoramaDll, 0xB4450);

	return g_Panorama_RunScript != nullptr;
}

void Addresses_RecordExternal(const char* name, const char* module, bool required,
                              size_t absolute, HMODULE base, size_t expectedRva) {
	Record(name, module, required, absolute, base, expectedRva);
}

bool Addresses_InitClient(HMODULE clientDll) {
	g_H_Client = clientDll;
	{
		// Build 6774 frame stage reads curtime at +0x30; the view matrix path also reads
		// framecount at +4 from the same global (RVA 0x3310df0).
		size_t hit = getAddress(clientDll, "48 8B 0D ?? ?? ?? ?? F3 0F 10 49 30 B9 FF FF FF FF E8 ?? ?? ?? ??");
		g_GlobalVars = (hit ? RipTarget(hit + 3, hit + 7) : nullptr);
		Record("client_global_vars", "client", true, (size_t)g_GlobalVars, clientDll, 0x3310df0);
	}

	// CUIEngine global. The CS2 pattern hits Deadlock unchanged -- this is compiler-shape code
	// over the shared Panorama client, in the function that references
	// "CreatePanelWithCurrentContext". The store is `mov [rip+d32], rcx`, so the displacement is
	// at +7 and the instruction ends at +11.
	{
		size_t at = getAddress(clientDll, "48 89 78 ?? 48 89 0D ?? ?? ?? ??");
		Record("client_uiengine_global", "client", true, at, clientDll, 0x1EE90B0);
		if (at) g_pUIEngine = RipTarget(at + 7, at + 11);
	}

	// HUD root panel global. Deadlock's HUD root layout is layout/hud.xml, one level shallower
	// than CS2's layout/hud/hud.xml, and its panel class is CCitadel_Hud_Main. The pattern anchors
	// on `mov [rip+d32], r14` immediately followed by the lea of that path, so the displacement is
	// at +3 and the instruction ends at +7.
	{
		size_t at = getAddress(clientDll,
			"4C 89 35 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 49 8B 4E 08 45 33 C0");
		Record("client_hud_panel_global", "client", true, at, clientDll, 0x1B195B6);
		if (at) g_pHudPanel = RipTarget(at + 3, at + 7);
	}

	// Build 6774: CSource2Client slot 36 dispatches frame stages; stage 12 updates the view.
	// Confirmed by read-only view stacks on the initialization thread, then its jump table.
	g_ClientFrameStageFn = getAddress(clientDll,
		"48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 40 48 8B F1 33 FF "
		"48 8B 89 30 02 00 00 8B DA 48 85 C9");
	Record("client_frame_stage", "client", true, g_ClientFrameStageFn, clientDll, 0x1886570);

	// The per-frame view setup. Unique in the installed client.dll and verified by the main-view
	// call chain below.
	{
		g_ClientViewSetupFn = getAddress(clientDll,
			"48 89 5C 24 08 48 89 6C 24 20 56 57 41 56 48 83 EC 60 "
			"C7 82 FC 04 00 00 01 00 00 00 48 8D 0D ?? ?? ?? ??");
		Record("client_view_setup", "client", true, g_ClientViewSetupFn, clientDll, 0x5D7120);
	}

	// The view class vtable, from RTTI. Deadlock keeps CS2's `.?AVCViewRender@@` name.
	g_ViewVtable = (size_t)Afx::BinUtils::FindClassVtable(clientDll, ".?AVCViewRender@@", 0, 0);
	Record("client_view_vtable", "client", true, g_ViewVtable, clientDll, 0x2349418);

	// Build 6774: CViewRender::slot17 calls ClientModeCitadelNormal::slot15 with
	// (mode, viewRender+0x10), then publishes the pose before slot4 builds matrices.
	g_ClientModeVtable = (size_t)Afx::BinUtils::FindClassVtable(clientDll, ".?AVClientModeCitadelNormal@@", 0, 0);
	Record("client_mode_vtable", "client", true, g_ClientModeVtable, clientDll, 0x26bf000);
	if (g_ViewVtable && g_ClientModeVtable && g_ClientViewSetupFn
	    && ((size_t*)g_ClientModeVtable)[15] == g_ClientViewSetupFn) {
		const size_t slot17 = ((size_t*)g_ViewVtable)[17];
		const size_t at = getAddress(clientDll,
			"48 8B C8 48 8B 10 4C 8B 42 78 48 8D 56 10 41 FF D0");
		if (at && slot17 && at >= slot17 && at + 17 <= slot17 + 0x80)
			g_ViewSetupCallerReturn = at + 17;
	}
	Record("client_view_caller", "client", true,
	       g_ViewSetupCallerReturn, clientDll, 0x192dbb4);

	return g_pUIEngine != nullptr && g_pHudPanel != nullptr && g_ClientViewSetupFn != 0
	    && g_ViewVtable != 0 && g_ClientModeVtable != 0 && g_ViewSetupCallerReturn != 0;
}

bool Addresses_InitSceneSystem(HMODULE sceneSystemDll) {
	g_H_SceneSystem = sceneSystemDll;
	return true;
}

int Addresses_Status(AddressStatus* out, int max) {
	int n = 0;
	for (const Entry& e : Entries()) {
		if (n >= max) break;
		out[n].name = e.name;
		out[n].module = e.module;
		out[n].required = e.required;
		out[n].resolved = e.resolved;
		out[n].rva = e.rva;
		out[n].expectedRva = e.expectedRva;
		++n;
	}
	return n;
}

bool Addresses_AllRequiredResolved() {
	for (const Entry& e : Entries()) {
		if (e.required && !e.resolved) return false;
	}
	return true;
}

} // namespace Deadlock
