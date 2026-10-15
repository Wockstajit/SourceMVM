// Paused-demo visibility for the Customize player-preview pawns. Root cause, the measurement that
// found it, and the scope rule: see PreviewRenderFlag.h.

#include "PreviewRenderFlag.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (the mandatory runtime-diagnosis channel)
#include "../Cosmetics/CosmeticModelSwap.h" // PawnGloveEntityIndex (the preview's hands)
#include "../../ClientEntitySystem.h"      // CEntityInstance, g_pEntityList, g_GetEntityFromIndex
#include "../../SchemaSystem.h"            // g_clientDllOffsets (the two interpolation fields)
#include "../../WrpConsole.h"              // CON_COMMAND (mvm_previewvis)
#include "../../../Shared/AfxConsole.h"    // advancedfx::Message / Warning

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h" // ISource2EngineToClient (paused)

#include <cstdint>
#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {
namespace {

// Client-only entities start at 0x4000; every preview pawn observed lives above it, so the periodic
// rescan never walks the networked range. Same bounds PreviewAnimTick uses.
constexpr int kMaxHandleIndex = 0x8000;
constexpr int kClientOnlyBase = 0x4000;
constexpr const char* kPreviewClass = "previewplayer";
constexpr int kMaxPreviewPawns = 16;

// The set is stable while the modal is open, but the panel is rebuilt on any model/item change --
// which swaps the pawn -- so it is refreshed periodically rather than once.
constexpr uint64_t kRescanFrames = 30;

int      g_pawnIdx[kMaxPreviewPawns];
int      g_pawnCount = 0;
// Each preview pawn's C_WorldModelGloves -- the character's HANDS (agents have none of their own). It is
// spawned by the pawn think after the panel equips the gloves, i.e. while paused it is born
// un-interpolated exactly like the pawn and is skipped by the renderer until playback resumes.
int      g_gloveIdx[kMaxPreviewPawns];
int      g_gloveCount = 0;
uint64_t g_frame = 0;
uint64_t g_lastScanFrame = 0;

int      g_mode = -1;      // -1 auto (follow the modal), 0 forced off, 1 forced on
bool     g_armed = false;
bool     g_modalOpen = false;
uint64_t g_writes = 0;     // flag flips we performed (not per-frame no-ops)
uint64_t g_clears = 0;
int      g_lastInterpolated = -1; // diagnostics: state of the last pawn we looked at
int      g_lastOverride = -1;

CEntityInstance* WideResolve(int index) {
	if (index < 0 || index >= kMaxHandleIndex || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	__try { return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index); }
	__except (1) { return nullptr; }
}

const char* SafeClassName(CEntityInstance* e) {
	if (!e) return nullptr;
	__try { return e->GetClassName(); } __except (1) { return nullptr; }
}

const char* SafeClientClassName(CEntityInstance* e) {
	if (!e) return nullptr;
	__try { return e->GetClientClassName(); } __except (1) { return nullptr; }
}

bool ContainsNoCase(const char* haystack, const char* needle) {
	if (!haystack || !needle || !*needle) return false;
	const size_t n = std::strlen(needle);
	for (const char* p = haystack; *p; ++p)
		if (0 == _strnicmp(p, needle, n)) return true;
	return false;
}

bool IsPreviewPawn(CEntityInstance* e) {
	return ContainsNoCase(SafeClassName(e), kPreviewClass)
		|| ContainsNoCase(SafeClientClassName(e), kPreviewClass);
}

bool Available() {
	return g_clientDllOffsets.C_BaseEntity.m_bRenderEvenWhenNotSuccessfullyInterpolated != 0;
}

bool ReadByte(CEntityInstance* ent, ptrdiff_t off, uint8_t* out) {
	*out = 0;
	if (!ent || off == 0) return false;
	__try { *out = *((uint8_t*)ent + off); return true; } __except (1) { return false; }
}

// Only ever writes when the byte differs, so a steady state costs one compare per pawn per frame.
bool WriteByteIfDifferent(CEntityInstance* ent, ptrdiff_t off, uint8_t value, bool* outChanged) {
	*outChanged = false;
	if (!ent || off == 0) return false;
	__try {
		uint8_t* p = (uint8_t*)ent + off;
		if (*p == value) return true;
		*p = value;
		*outChanged = true;
		return true;
	} __except (1) { return false; }
}

void Rescan() {
	g_pawnCount = 0;
	for (int i = kClientOnlyBase; i < kMaxHandleIndex && g_pawnCount < kMaxPreviewPawns; ++i) {
		CEntityInstance* e = WideResolve(i);
		if (e && IsPreviewPawn(e))
			g_pawnIdx[g_pawnCount++] = i;
	}
	g_gloveCount = 0;
	for (int i = 0; i < g_pawnCount && g_gloveCount < kMaxPreviewPawns; ++i) {
		const int glove = PawnGloveEntityIndex((unsigned char*)WideResolve(g_pawnIdx[i]));
		if (glove > 0)
			g_gloveIdx[g_gloveCount++] = glove;
	}
	g_lastScanFrame = g_frame;
}

// value 1 = arm the override, 0 = release it.
void ApplyToPawns(uint8_t value) {
	const ptrdiff_t offOverride = g_clientDllOffsets.C_BaseEntity.m_bRenderEvenWhenNotSuccessfullyInterpolated;
	const ptrdiff_t offInterp = g_clientDllOffsets.C_BaseEntity.m_bHasSuccessfullyInterpolated;
	for (int i = 0; i < g_pawnCount; ++i) {
		CEntityInstance* e = WideResolve(g_pawnIdx[i]);
		if (!e || !IsPreviewPawn(e))
			continue; // index recycled since the scan -- never write through a stale slot
		uint8_t interpolated = 0;
		if (offInterp) ReadByte(e, offInterp, &interpolated);
		bool changed = false;
		if (!WriteByteIfDifferent(e, offOverride, value, &changed))
			continue;
		g_lastInterpolated = (int)interpolated;
		g_lastOverride = (int)value;
		if (!changed)
			continue;
		if (value) ++g_writes; else ++g_clears;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.vis",
				"%s idx=%d ent=%p hasInterpolated=%u override=%u writes=%llu clears=%llu"
				" offInterp=0x%llx offOverride=0x%llx demoWrite=0 tickWrite=0",
				value ? "ARM" : "RELEASE", g_pawnIdx[i], (void*)e, (unsigned)interpolated,
				(unsigned)value, (unsigned long long)g_writes, (unsigned long long)g_clears,
				(unsigned long long)offInterp, (unsigned long long)offOverride);
	}
	for (int i = 0; i < g_gloveCount; ++i) {
		CEntityInstance* e = WideResolve(g_gloveIdx[i]);
		if (!e || !ContainsNoCase(SafeClassName(e), "gloves"))
			continue; // recycled index
		bool changed = false;
		if (WriteByteIfDifferent(e, offOverride, value, &changed) && changed && MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.vis", "%s gloves idx=%d ent=%p", value ? "ARM" : "RELEASE",
				g_gloveIdx[i], (void*)e);
	}
}

void SetArmed(bool armed) {
	if (g_armed == armed) return;
	g_armed = armed;
	if (!armed) {
		// Release the override on whatever preview pawns still exist. They are UI-only entities and
		// are usually destroyed with the panel, but leaving a forced render flag behind would be a
		// state change the modal never undid.
		Rescan();
		ApplyToPawns(0);
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("preview.vis", "%s mode=%d modal=%d available=%d pawns=%d",
			armed ? "ARM" : "DISARM", g_mode, g_modalOpen ? 1 : 0, Available() ? 1 : 0, g_pawnCount);
}

bool DemoPaused() {
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	return demo ? demo->IsDemoPaused() : false;
}

} // namespace

void PreviewRenderFlag_RunFrame(bool customizeModalOpen) {
	++g_frame;
	g_modalOpen = customizeModalOpen;
	SetArmed((g_mode < 0) ? g_modalOpen : (g_mode == 1));
	if (!g_armed || !Available())
		return;
	if (g_frame - g_lastScanFrame >= kRescanFrames || g_pawnCount == 0)
		Rescan();
	if (g_pawnCount == 0)
		return;
	// Held every frame rather than once on open: the panel is rebuilt on any model/item change, and
	// each rebuild spawns a fresh pawn that starts out un-interpolated all over again.
	ApplyToPawns(1);
}

void PreviewRenderFlag_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 2) ? args->ArgV(1) : "status";

	if (0 == _stricmp(arg, "auto")) {
		g_mode = -1;
		advancedfx::Message("%s: auto -- armed whenever the Customize modal is open (default).\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "1")) {
		g_mode = 1;
		advancedfx::Message("%s: forced ON (overrides the modal).\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "0")) {
		g_mode = 0;
		advancedfx::Message("%s: forced off -- a preview pawn spawned while the demo is paused will"
			" not be drawn.\n", cmd);
		return;
	}
	if (0 == _stricmp(arg, "scan")) {
		Rescan();
		advancedfx::Message("%s: preview pawns=%d\n", cmd, g_pawnCount);
		const ptrdiff_t offInterp = g_clientDllOffsets.C_BaseEntity.m_bHasSuccessfullyInterpolated;
		const ptrdiff_t offOverride = g_clientDllOffsets.C_BaseEntity.m_bRenderEvenWhenNotSuccessfullyInterpolated;
		for (int i = 0; i < g_pawnCount; ++i) {
			CEntityInstance* e = WideResolve(g_pawnIdx[i]);
			uint8_t interpolated = 0, override_ = 0;
			ReadByte(e, offInterp, &interpolated);
			ReadByte(e, offOverride, &override_);
			advancedfx::Message("   idx=%d ent=%p hasInterpolated=%u renderEvenWhenNot=%u\n",
				g_pawnIdx[i], (void*)e, (unsigned)interpolated, (unsigned)override_);
		}
		return;
	}
	if (0 == _stricmp(arg, "status")) {
		advancedfx::Message(
			"%s: %s mode=%s modalOpen=%d available=%d paused=%d pawns=%d\n"
			"  offsets hasInterpolated=0x%llx renderEvenWhenNot=0x%llx\n"
			"  arms=%llu releases=%llu lastPawn(hasInterpolated=%d override=%d) mvm_debug=%s\n",
			cmd, g_armed ? "ARMED" : "idle",
			(g_mode < 0) ? "auto" : (g_mode == 1 ? "forced-on" : "forced-off"),
			g_modalOpen ? 1 : 0, Available() ? 1 : 0, DemoPaused() ? 1 : 0, g_pawnCount,
			(unsigned long long)g_clientDllOffsets.C_BaseEntity.m_bHasSuccessfullyInterpolated,
			(unsigned long long)g_clientDllOffsets.C_BaseEntity.m_bRenderEvenWhenNotSuccessfullyInterpolated,
			(unsigned long long)g_writes, (unsigned long long)g_clears,
			g_lastInterpolated, g_lastOverride,
			MvmDebugLog_Active() ? "on" : "off");
		if (!Available())
			advancedfx::Warning("%s: C_BaseEntity::m_bRenderEvenWhenNotSuccessfullyInterpolated did not"
				" resolve -- the preview pawn will stay invisible while the demo is paused."
				" See Docs/agent/cs2-update-recovery.md.\n", cmd);
		return;
	}

	advancedfx::Message(
		"%s auto   - arm whenever the Customize modal is open (DEFAULT; no command needed).\n"
		"%s on|off - override auto. `off` restores the stock behaviour (invisible while paused).\n"
		"%s scan   - list the preview pawns and their interpolation/override flags.\n"
		"%s status - full state.\n",
		cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker

CON_COMMAND(mvm_previewvis,
	"Draw the Customize player-preview pawns even while the demo is paused (interpolation override).") {
	Filmmaker::PreviewRenderFlag_RunCommand(args->ArgC(), args, args->ArgV(0));
}
