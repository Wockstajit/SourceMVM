#include "DeadlockPreUiHook.h"

#include "DeadlockViewportBlit.h"
#include "DeadlockOverlayDrawer.h"

#include "../../Shared/AfxConsole.h"
#include "../../Shared/AfxDetours.h"
#include "../../Shared/binutils.h"

#include <cstdarg>
#include <cstring>
#include <set>
#include <string>

namespace Deadlock {

namespace {

bool g_installed = false;
PreUiCallback_t g_callback = nullptr;

unsigned long long g_contexts = 0;
unsigned long long g_hudPasses = 0;
unsigned long long g_callbacks = 0;

// Derived offline; see DeadlockPreUiHook.h and derive-deadlock-preui-hook.py.
//
// 137, not CS2's 142. The two vtables are 173 and 178 slots, and the same function sits at a
// different index in each -- which is exactly why this was cross-matched rather than copied.
const size_t kQueueCallbackSlot = 137;

// The render pass whose setup marks the boundary. CS2 keys on "CSGOHud".
//
// MEASURED, and the obvious guess was wrong. "CitadelHud" is a panel class in client.dll and it
// never matched: what the engine passes here is the render CONTEXT's name, which is a different
// vocabulary. Dumped in game with `mirv_filmmaker preui names`, the HUD's is:
//
//     #%s/SetupLightsAndViewConstants  arg0=CitadelHudRoot
//
// The same dump lists CitadelDashboardRoot, CitadelHudDBOverlay, CitadelHudPlayOfTheGameOverlay,
// CitadelLoadingScreen, PanoramaEngineConsole and CPanoramaWorldPanel as sibling Panorama passes.
// Only the HUD root is the world/UI boundary; the overlays are drawn after it and keying on one of
// those would put the blit in the wrong place.
const char* const kHudPassName = "CitadelHudRoot";
const char* const kSetupLightsFmt = "#%s/SetupLightsAndViewConstants";

// The engine's render-thread callback interface: one virtual, called on the render thread.
//
// Allocated with `new` per queue, exactly as the CS2 side does it -- the engine owns it from the
// moment it is queued. It deliberately carries no state: a callback that outlives the frame that
// made it must not reach back into anything that could have gone away.
class PreUiCallbackObject {
public:
	virtual void OnCallback() {
		++g_callbacks;
		if (PreUiCallback_t fn = g_callback) {
			__try {
				fn();
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				// A fault here is on the render thread inside the engine's own call. Swallowing it
				// costs one frame of the preview; letting it out takes the game down.
			}
		}
	}
};

// Queue a callback on the context's software command list.
//
// `param_1` points at the CRenderContextPtr; its first field is the software command list, whose
// vtable carries QueueCallback. Guarded because a wrong slot calls an unrelated virtual with a
// pointer argument, and the whole point of the stand-down below is to notice that once rather than
// every frame for the rest of the session.
bool g_queueStoodDown = false;

void QueuePreUiCallback(void* param_1) {
	if (g_queueStoodDown || !param_1) return;
	__try {
		void* commandList = *(void**)param_1;
		if (!commandList) return;
		void** vtable = *(void***)commandList;
		if (!vtable) return;
		void* entry = vtable[kQueueCallbackSlot];
		if (!entry) return;
		typedef void(__fastcall * QueueCallback_t)(void* commandList, void* callback);
		((QueueCallback_t)entry)(commandList, new PreUiCallbackObject());
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_queueStoodDown = true;
		advancedfx::Warning(
			"AfxHookDeadlock: queuing the pre-UI callback faulted, so the vtable slot (%zu) is\n"
"wrong for this build. The scaled preview is disabled for this session. Re-run\n"
"Automation/verify/derive-deadlock-preui-hook.py to re-derive it.\n",
			kQueueCallbackSlot);
	}
}

// EVERY DISTINCT PASS NAME SEEN, for deriving the one that matters.
//
// The HUD pass name cannot be inferred from CS2's spelling -- "CitadelHud" was the obvious guess
// from client.dll's strings and it never matched, because the name the engine passes here is the
// render-context's name, not the panel class's. So the names are collected and dumped instead:
// `mirv_filmmaker preui names`. Bounded, because this runs thousands of times a second.
std::set<std::string>& SeenNames() {
	static std::set<std::string> names;
	return names;
}
bool g_collectNames = false;

// Copy a possibly-bad pointer into a fixed buffer. SEH cannot live in a function that also
// constructs objects ("cannot use __try in functions that require object unwinding"), so the
// dangerous read is isolated here and the std::string work happens in the caller.
bool CopyCString(const char* src, char* dst, size_t cap) {
	__try {
		size_t i = 0;
		for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
		dst[i] = 0;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		dst[0] = 0;
		return false;
	}
}

void NoteName(const char* fmt, const char* name) {
	if (!g_collectNames || !name) return;
	std::set<std::string>& seen = SeenNames();
	if (seen.size() > 400) return;
	char buf[192];
	if (!CopyCString(name, buf, sizeof(buf))) return;
	seen.insert(std::string(fmt ? fmt : "?") + "  arg0=" + buf);
}

// Does this pass's format string and first argument mark the HUD's setup?
//
// Both overloads are varargs, and the first vararg is only a string when the format says so --
// reading it otherwise would walk whatever happened to be in the register. So the format is
// compared first, and only then is the argument touched.
bool NameEquals(const char* a, const char* b) {
	__try {
		return 0 == std::strcmp(a, b);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// Inspect one pass. Returns whether it is the HUD's, and -- while collecting -- records what it
// saw, which is how the HUD's real name gets found rather than guessed a second time.
//
// EVERY format is recorded, not just the one being matched against. The first guess assumed both
// the format string AND the name; recording only the calls that already match the format would
// have hidden a HUD pass announced under a different format, which is exactly the kind of blind
// spot that makes a diagnostic agree with the bug.
bool IsHudPass(const char* fmt, va_list args) {
	if (!fmt) return false;
	const bool setupLights = NameEquals(fmt, kSetupLightsFmt);
	// The first vararg is only a string when the format says so; reading it otherwise walks
	// whatever happened to be in the register.
	const bool hasStringArg = setupLights || nullptr != std::strstr(fmt, "%s");
	if (!hasStringArg) {
		NoteName(fmt, "(no string argument)");
		return false;
	}
	const char* name = va_arg(args, const char*);
	if (!name) return false;
	NoteName(fmt, name);
	return setupLights && NameEquals(name, kHudPassName);
}

// ---- the two overloads ---------------------------------------------------------------------
//
// The varargs are NOT forwarded. Passing a va_list through to a `...` function needs an assembly
// trampoline, and the CS2 side does not bother either: the format string only names the pass for
// the engine's own profiling scopes, so replacing it costs a label in a profiler nobody is reading
// and buys a detour that is ten lines instead of a hand-written thunk.
const char* const kReplacementFmt =
	"Hooked by SOURCE:MVM (AfxHookDeadlock). If you are reading this in a profiler, please report it.";

typedef unsigned char*(__fastcall* CreateRenderContextPtr1_t)(
	unsigned char* param_1, unsigned char param_2, void* pDevice, void* param_4,
	const char* fmt, ...);
CreateRenderContextPtr1_t g_old1 = nullptr;

unsigned char* __fastcall New_CreateRenderContextPtr1(
	unsigned char* param_1, unsigned char param_2, void* pDevice, void* param_4,
	const char* fmt, ...) {
	unsigned char* result = g_old1(param_1, param_2, pDevice, param_4, kReplacementFmt);
	++g_contexts;
	if (fmt) {
		va_list args;
		va_start(args, fmt);
		const bool hud = IsHudPass(fmt, args);
		va_end(args);
		// hudPasses counts the NAME MATCHING, not the blit wanting the frame. Counting both
		// together made the triage line accuse the pass name of being wrong for this build every
		// time the workspace happened to be shut -- a diagnostic that sends the next investigation
		// to re-derive a value that was never wrong.
		if (hud) {
			++g_hudPasses;
			if (ViewportBlit_Active()) QueuePreUiCallback(param_1);
		}
	}
	return result;
}

typedef unsigned char*(__fastcall* CreateRenderContextPtr2_t)(
	unsigned char* param_1, unsigned char param_2, void* pDevice, const char* fmt, ...);
CreateRenderContextPtr2_t g_old2 = nullptr;

unsigned char* __fastcall New_CreateRenderContextPtr2(
	unsigned char* param_1, unsigned char param_2, void* pDevice, const char* fmt, ...) {
	unsigned char* result = g_old2(param_1, param_2, pDevice, kReplacementFmt);
	++g_contexts;
	if (fmt) {
		va_list args;
		va_start(args, fmt);
		const bool hud = IsHudPass(fmt, args);
		va_end(args);
		if (hud) {
			++g_hudPasses;
			if (ViewportBlit_Active()) QueuePreUiCallback(param_1);
		}
	}
	return result;
}

// Patterns derived and uniqueness-checked offline. Pattern 1 is CS2's with ONE byte wildcarded:
// the builds call different vtable displacements there, which is an ordinary consequence of the
// interface growing between them.
const char* const kPattern1 =
	"40 53 56 57 48 83 ec 20 49 8b 00 49 8b d8 48 8b f9 45 33 c0 48 8b cb 49 8b f1 ff 90 ?? 01 00 00";
const char* const kPattern2 =
	"40 55 53 57 48 8d 6c 24 ?? 48 81 ec ?? ?? ?? ?? 80 65 ?? ?? 33 c0";

// Scan .text once and refuse an ambiguous match. A pattern that hits twice is worse than one that
// hits never: it detours whichever copy the scan reached first, and which one that is can change
// with a rebuild.
void* FindUnique(Afx::BinUtils::MemRange textRange, const char* pattern, const char* label) {
	Afx::BinUtils::MemRange hit = Afx::BinUtils::FindPatternString(textRange, pattern);
	if (hit.IsEmpty()) {
		advancedfx::Warning("AfxHookDeadlock: pre-UI pattern '%s' did not resolve.\n", label);
		return nullptr;
	}
	// Look again past the first hit; a second one means the pattern is not specific enough.
	Afx::BinUtils::MemRange rest(hit.Start + 1, textRange.End);
	if (!Afx::BinUtils::FindPatternString(rest, pattern).IsEmpty()) {
		advancedfx::Warning(
			"AfxHookDeadlock: pre-UI pattern '%s' matched more than once; refusing to detour a\n"
"function picked by scan order. Re-derive it before enabling the scaled preview.\n",
			label);
		return nullptr;
	}
	return (void*)hit.Start;
}

// The pre-UI slot's single callback, combining both consumers in the order that makes the second
// one correct -- see PreUiHook_Install's own comment on why.
void RunOverlayThenBlit() {
	OverlayDrawer_RunPreUi();
	ViewportBlit_RunPreUi();
}

} // namespace

bool PreUiHook_Install(HMODULE sceneSystemDll) {
	if (g_installed) return true;
	if (!sceneSystemDll) return false;

	Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
	for (Afx::BinUtils::ImageSectionsReader sections(sceneSystemDll);
	     !sections.Eof(); sections.Next()) {
		PIMAGE_SECTION_HEADER header = sections.Get();
		if (header && 0 == std::strncmp((const char*)header->Name, ".text", 5)) {
			textRange = sections.GetMemRange();
			break;
		}
	}
	if (textRange.IsEmpty()) {
		advancedfx::Warning("AfxHookDeadlock: scenesystem.dll has no .text range to scan.\n");
		return false;
	}

	void* fn1 = FindUnique(textRange, kPattern1, "CreateRenderContextPtr1");
	void* fn2 = FindUnique(textRange, kPattern2, "CreateRenderContextPtr2");
	// BOTH or neither. Only one of the two overloads is the HUD's on a given frame, and which one
	// is not something to find out by shipping half the hook.
	if (!fn1 || !fn2) return false;

	g_old1 = (CreateRenderContextPtr1_t)fn1;
	g_old2 = (CreateRenderContextPtr2_t)fn2;

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_old1, New_CreateRenderContextPtr1);
	DetourAttach(&(PVOID&)g_old2, New_CreateRenderContextPtr2);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_old1 = nullptr;
		g_old2 = nullptr;
		advancedfx::Warning("AfxHookDeadlock: could not detour the pre-UI render-context calls.\n");
		return false;
	}

	g_installed = true;
	// ONE callback slot (g_callback below is a single function pointer, not a list), so both
	// pre-UI D3D consumers run from one combining function, in the order that makes the second
	// one correct: the overlay draws hero boxes INTO the just-finished full-screen world render
	// first, then the blit snapshots-clears-redraws THAT combined image scaled into the preview
	// rect -- drawing the boxes after the blit would have the blit's own clear erase them the very
	// frame they were drawn, and the blit's snapshot would then scale a world with no boxes in it.
	PreUiHook_SetCallback(&RunOverlayThenBlit);
	advancedfx::Message(
		"AfxHookDeadlock: pre-UI hook installed (scenesystem +0x%zx, +0x%zx; QueueCallback slot %zu).\n",
		(size_t)fn1 - (size_t)sceneSystemDll, (size_t)fn2 - (size_t)sceneSystemDll,
		kQueueCallbackSlot);
	return true;
}

bool PreUiHook_Installed() { return g_installed; }

void PreUiHook_SetCallback(PreUiCallback_t callback) { g_callback = callback; }

void PreUiHook_Counters(unsigned long long& contexts,
                        unsigned long long& hudPasses,
                        unsigned long long& callbacks) {
	contexts = g_contexts;
	hudPasses = g_hudPasses;
	callbacks = g_callbacks;
}

void PreUiHook_SetCollectNames(bool on) {
	g_collectNames = on;
	if (!on) SeenNames().clear();
}

void PreUiHook_DumpNames() {
	std::set<std::string>& seen = SeenNames();
	advancedfx::Message("  pre-ui pass names seen (%zu):\n", seen.size());
	for (const std::string& s : seen) advancedfx::Message("    %s\n", s.c_str());
	if (seen.empty())
		advancedfx::Message("    none yet -- 'mirv_filmmaker preui names on', wait a frame, ask again.\n");
}

void PreUiHook_Report() {
	advancedfx::Message(
		"  pre-ui:    %s  contexts=%llu hudPasses=%llu callbacks=%llu%s\n",
		g_installed ? "hooked" : "NOT hooked", g_contexts, g_hudPasses, g_callbacks,
		g_queueStoodDown ? "  QUEUE STOOD DOWN (wrong vtable slot)" : "");
	// The three counters fail in a fixed order, so say which stage is the one that broke rather
	// than leaving three numbers to be interpreted.
	if (g_installed && g_contexts == 0)
		advancedfx::Warning(
			"  the detour is installed but the engine never called it -- the pattern found the\n"
"  wrong function.\n");
	else if (g_contexts > 0 && g_hudPasses == 0)
		advancedfx::Warning(
			"  render contexts are being created but none is named '%s' -- the HUD pass name is\n"
"  wrong for this build. Re-run derive-deadlock-preui-hook.py.\n", kHudPassName);
	else if (g_hudPasses > 0 && g_callbacks == 0 && ViewportBlit_Active())
		advancedfx::Warning(
			"  the HUD pass is recognised but the queued callback never ran -- the QueueCallback\n"
"  vtable slot (%zu) is wrong for this build.\n", kQueueCallbackSlot);
}

} // namespace Deadlock
