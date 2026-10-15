#pragma once

// Every version-coupled address AfxHookDeadlock resolves, in one place.
//
// The Deadlock analogue of AfxHookSource2/addresses.h, and quarantined for the same reason: one
// Deadlock update relocates code and breaks byte-pattern scans, and having every scan in a single
// translation unit means the damage is visible in one file instead of scattered across forty.
//
// Docs/deadlock/signatures.md records how each pattern was derived, which anchor it hangs off, and
// what the RVA was on the build it was derived against. Re-derive from the anchor rather than
// guessing; the offline verifier
//
//     py -3 Automation/verify/verify-deadlock-signatures.py
//
// classifies every pattern here as OK / MULTI / BROKEN without launching the game.

#include <windows.h>
#include <cstdint>

namespace Deadlock {

// ---------------------------------------------------------------------------
// Resolution. Each Init* is called once, from the module dispatcher, the first time that module
// shows up in a LoadLibrary call. They return false when a REQUIRED address did not resolve, and
// the caller reports that through `mirv_filmmaker health` rather than popping a message box per
// failure the way the CS2 hook does.
// ---------------------------------------------------------------------------

bool Addresses_InitTier0(HMODULE tier0Dll);
bool Addresses_InitEngine2(HMODULE engine2Dll);
bool Addresses_InitPanorama(HMODULE panoramaDll);
bool Addresses_InitClient(HMODULE clientDll);
bool Addresses_InitSceneSystem(HMODULE sceneSystemDll);

// ---------------------------------------------------------------------------
// Module handles, captured by the dispatcher. Null until that module loads.
// ---------------------------------------------------------------------------

extern HMODULE g_H_Tier0;
extern HMODULE g_H_Engine2;
extern HMODULE g_H_Client;
extern HMODULE g_H_Panorama;
extern HMODULE g_H_SchemaSystem;
extern HMODULE g_H_ResourceSystem;
extern HMODULE g_H_FileSystemStdio;
extern HMODULE g_H_SceneSystem;
extern HMODULE g_H_MaterialSystem2;
extern HMODULE g_H_RenderSystemDx11;

// ---------------------------------------------------------------------------
// Panorama. panorama.dll is shared Source 2 code, so RunScript is the one address here that is
// genuinely a panorama-side scan; the two client-side globals are client code.
// ---------------------------------------------------------------------------

// CUIEngine::RunScript(engine, contextPanel, source, originFile, line).
//
// Derived rather than inherited: Deadlock's prologue saves rbx into r8's home slot and pushes
// rbp,rsi,rdi,r12,r13,r14,r15 where CS2 pushes rbp,rbx,rsi,rdi,r12,r14,r15, so the CS2 pattern
// misses entirely. Anchored on the "CUIEngine::RunScript (compile+run)" profiling scope; the
// neighbouring function owning "(pre-compiled)" is NOT the one to call.
extern void* g_Panorama_RunScript;

// CUIEngine** and the HUD root CPanel2D**, both client.dll globals.
extern unsigned char** g_pUIEngine;
extern unsigned char** g_pHudPanel;

// ---------------------------------------------------------------------------
// Client view setup. The function the camera override is installed inside.
// ---------------------------------------------------------------------------

extern unsigned char** g_DemoPlayerPointer;
extern size_t g_DemoPlayerVtable;
extern unsigned char** g_GlobalVars;

extern size_t g_ClientViewSetupFn;
// Measured CSource2Client frame-stage dispatcher; drives the main/UI pump independently.
extern size_t g_ClientFrameStageFn;

// Build 6774: CViewRender slot17 calls ClientModeCitadelNormal slot15 with the view
// embedded at +0x10. Require both RTTI identities, the setup function, and that caller.
extern size_t g_ViewVtable;
extern size_t g_ClientModeVtable;
extern size_t g_ViewSetupCallerReturn;

// ---------------------------------------------------------------------------
// A one-line-per-address status dump for `mirv_filmmaker health`.
// ---------------------------------------------------------------------------

struct AddressStatus {
	const char* name;
	const char* module;
	bool required;
	bool resolved;
	size_t rva;         // 0 when unresolved
	size_t expectedRva; // what it was on the build it was derived against, 0 when not recorded
};

// Registers an address resolved by a module OUTSIDE this file, so it still shows up in health.
//
// The rule this file states -- every version-coupled address in one place -- is about VISIBILITY,
// not about which translation unit runs the scan. DeadlockEntities owns the entity-list patterns
// because it owns the only code that dereferences them; it reports them here so a broken scan is
// still one line in `mirv_filmmaker health` rather than a feature that silently does nothing.
void Addresses_RecordExternal(const char* name, const char* module, bool required,
                              size_t absolute, HMODULE base, size_t expectedRva);

// Fills `out` with up to `max` entries and returns how many were written.
int Addresses_Status(AddressStatus* out, int max);

// True when every REQUIRED address resolved.
bool Addresses_AllRequiredResolved();

} // namespace Deadlock
