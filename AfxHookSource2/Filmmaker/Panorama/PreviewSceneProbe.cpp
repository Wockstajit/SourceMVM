// Read-only probe for the Customize modal's player-preview scene. Rationale, the disproven
// "no handle exists" theory, and the live-confirmed entity class: see PreviewSceneProbe.h.

#include "PreviewSceneProbe.h"


#include <string>

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (the mandatory runtime-diagnosis channel)
#include "../Cosmetics/CosmeticModelSwap.h" // PostDataUpdate (native scene-node renderable refresh)
#include "../../ClientEntitySystem.h"      // CEntityInstance, g_pEntityList, g_GetEntityFromIndex
#include "../../SchemaSystem.h"            // g_clientDllOffsets (ModelChain, for the model-path read)
#include "../../MirvTime.h"                // g_MirvTime.GetCurrentDemoTick
#include "../../WrpConsole.h"              // CON_COMMAND (mvm_previewprobe)
#include "../../../Shared/AfxConsole.h"    // advancedfx::Message / advancedfx::Warning

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h" // ISource2EngineToClient (paused)

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// Same engine pointer CosmeticDemoSync.cpp uses to read the demo's paused state.
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {
namespace {

// Full handle range. Client-only entities start at 0x4000.
constexpr int kMaxHandleIndex = 0x8000;
constexpr int kClientOnlyBase = 0x4000;

// The preview pawn's class name, confirmed live (see header).
constexpr const char* kPreviewClass = "previewplayer";

// Animation-advance proxies, tried in order; the first that resolves is sampled every pump. An idle /
// breathing animgraph moves these attachment bones while the entity's ROOT origin stays put, so the
// pair "attachment moves / origin constant" is what distinguishes "animating" from "frozen".
// LookupAttachment returns 0 on failure and GetAttachment rejects index 0.
const char* const kAttachmentCandidates[] = {
	"eyes", "facemask", "head", "clip_limit", "primary", "weapon_hand_R",
};

int      g_watchIndex = -1;   // -1 == probe disarmed
int      g_throttle = 4;      // sample every Nth pump frame (never every frame -- the log flushes per line)
uint64_t g_pumpFrame = 0;
uint8_t  g_attachIdx = 0;
char     g_attachName[32] = {};
int      g_resolveFailStreak = 0;

// Full-range resolve. SEH-guarded because a map transition can leave the list mid-rebuild.
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

// Own copy of the model-path walk (the module stays standalone rather than pulling in cosmetics
// internals). POD-only __try body, as required. out[0]='\0' on any failure.
void ReadModelPath(CEntityInstance* ent, char* out, size_t outSize) {
	if (out && outSize) out[0] = '\0';
	if (!ent || !out || outSize == 0) return;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_ModelName == 0)
		return;
	unsigned char* p = (unsigned char*)ent;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp <= 0x10000) return;
		unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
		const char* name = *(const char**)(modelState + o.ModelChain.m_ModelName);
		if ((uintptr_t)name <= 0x10000) return;
		size_t i = 0;
		for (; name[i] && i + 1 < outSize; ++i) out[i] = name[i];
		out[i] = '\0';
	} __except (1) {
		if (outSize) out[0] = '\0';
	}
}

// The LIVE geometry selection (CModelState::m_MeshGroupMask). A mask of 0 means "no mesh group
// selected": the model is in the scene, posed, and animating, but draws nothing. That is exactly the
// shape of the "held weapon renders, agent body does not" symptom, so it is read before anything
// deeper. Same walk as ReadModelPath, one field further along.
bool ReadMeshGroupMask(CEntityInstance* ent, uint64_t* out) {
	if (out) *out = 0;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!ent || !out || o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_MeshGroupMask == 0)
		return false;
	unsigned char* p = (unsigned char*)ent;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp <= 0x10000) return false;
		unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
		*out = *(uint64_t*)(modelState + o.ModelChain.m_MeshGroupMask);
		return true;
	} __except (1) {
		return false;
	}
}

// Diagnostic lever for the read above: if a paused-created preview pawn carries mask 0, writing a
// real mask (and refreshing the renderable) has to make it draw. Direct field write + the native
// PostDataUpdate, mirroring what Cosmetics/CosmeticModelSwap.cpp does for weapon mesh selection.
bool WriteMeshGroupMask(CEntityInstance* ent, uint64_t mask) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!ent || mask == 0 || o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_MeshGroupMask == 0)
		return false;
	unsigned char* p = (unsigned char*)ent;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp <= 0x10000) return false;
		unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
		*(uint64_t*)(modelState + o.ModelChain.m_MeshGroupMask) = mask;
		return true;
	} __except (1) {
		return false;
	}
}

void* SceneNodeOf(CEntityInstance* ent) {
	const ptrdiff_t off = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
	if (!ent || off == 0) return nullptr;
	__try {
		void* n = *(void**)((unsigned char*)ent + off);
		return ((uintptr_t)n > 0x10000) ? n : nullptr;
	} __except (1) { return nullptr; }
}

// SEH wrappers kept as free functions taking REFERENCES: __try may not appear in a function that
// holds objects requiring unwinding (MSVC C2712), which a local SOURCESDK::Vector/Quaternion would
// be. Same split CosmeticEntityAccess.cpp uses for the same reason.
bool SafeGetOrigin(CEntityInstance* ent, float& x, float& y, float& z) {
	x = y = z = 0;
	if (!ent) return false;
	__try { ent->GetOrigin(x, y, z); return true; } __except (1) { return false; }
}

uint8_t SafeLookupAttachment(CEntityInstance* ent, const char* name) {
	if (!ent || !name || !*name) return 0;
	__try { return ent->LookupAttachment(name); } __except (1) { return 0; }
}

bool SafeGetAttachment(CEntityInstance* ent, uint8_t idx,
	SOURCESDK::Vector& origin, SOURCESDK::Quaternion& angles) {
	if (!ent || idx == 0) return false;
	__try { return ent->GetAttachment(idx, origin, angles); } __except (1) { return false; }
}

bool DemoPaused() {
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	return demo ? demo->IsDemoPaused() : false;
}

// Binds g_attachIdx/g_attachName to the first candidate this entity actually has.
void BindAttachment(CEntityInstance* ent) {
	g_attachIdx = 0;
	g_attachName[0] = '\0';
	if (!ent) return;
	for (const char* cand : kAttachmentCandidates) {
		const uint8_t idx = SafeLookupAttachment(ent, cand);
		if (idx != 0) {
			g_attachIdx = idx;
			std::snprintf(g_attachName, sizeof(g_attachName), "%s", cand);
			return;
		}
	}
}

// ---- animation-state discovery -------------------------------------------------------------
// The global client clock cannot be used to drive the preview (proved: writes at both main-thread
// stages are re-applied by the client -- see PreviewAnimClock.h). So find the PER-PAWN animation
// state instead: snapshot the pawn's memory, let the demo advance, snapshot again, and report the
// float fields that moved. A field that advances smoothly with the demo tick is the animation clock
// this pawn actually reads, and that is what a per-pawn advance has to write.
constexpr size_t kDiffEntityBytes = 0x2000; // entity base window
constexpr size_t kDiffBodyBytes = 0x1000;   // body-component window (skeleton/animgraph state)

unsigned char g_snapEnt[kDiffEntityBytes];
unsigned char g_snapBody[kDiffBodyBytes];
bool  g_haveSnap = false;
int   g_diffIdx = -1;
void* g_diffBody = nullptr;

// POD-only SEH copy.
bool SafeCopy(void* dst, const void* src, size_t n) {
	if (!dst || !src) return false;
	__try { memcpy(dst, src, n); return true; } __except (1) { return false; }
}

// Follows entity -> m_CBodyComponent (the same chain ReadModelPath uses).
void* BodyComponentOf(CEntityInstance* ent) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!ent || o.ModelChain.m_CBodyComponent == 0) return nullptr;
	unsigned char* p = (unsigned char*)ent;
	__try {
		unsigned char* bc = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		return ((uintptr_t)bc > 0x10000) ? bc : nullptr;
	} __except (1) { return nullptr; }
}

bool FloatPlausibleClock(float a, float b) {
	// Advancing, by a small positive step, staying in a sane magnitude. Excludes NaN/inf and the
	// huge coordinate/matrix values that dominate an entity's memory.
	if (!(a == a) || !(b == b)) return false;           // NaN
	const float d = b - a;
	if (!(d > 0.0f) || !(d < 2.0f)) return false;
	if (!(a > -1.0f && a < 100000.0f)) return false;
	if (!(b > -1.0f && b < 100000.0f)) return false;
	return true;
}

void DiffSnapshot(int index) {
	CEntityInstance* ent = WideResolve(index);
	if (!ent) { advancedfx::Warning("mvm_previewprobe diff: idx %d does not resolve.\n", index); return; }
	void* body = BodyComponentOf(ent);
	memset(g_snapEnt, 0, sizeof(g_snapEnt));
	memset(g_snapBody, 0, sizeof(g_snapBody));
	const bool okEnt = SafeCopy(g_snapEnt, ent, kDiffEntityBytes);
	const bool okBody = body ? SafeCopy(g_snapBody, body, kDiffBodyBytes) : false;
	g_diffIdx = index;
	g_diffBody = body;
	g_haveSnap = okEnt;
	advancedfx::Message("mvm_previewprobe diff: snapshot A idx=%d ent=%p(%d) body=%p(%d).\n"
		"  Let the demo PLAY briefly, then run `mvm_previewprobe diff report`.\n",
		index, (void*)ent, okEnt ? 1 : 0, body, okBody ? 1 : 0);
}

void DiffReport() {
	if (!g_haveSnap || g_diffIdx < 0) {
		advancedfx::Warning("mvm_previewprobe diff: no snapshot -- run `diff <index>` first.\n");
		return;
	}
	CEntityInstance* ent = WideResolve(g_diffIdx);
	if (!ent) { advancedfx::Warning("mvm_previewprobe diff: idx %d no longer resolves.\n", g_diffIdx); return; }

	static unsigned char nowEnt[kDiffEntityBytes];
	static unsigned char nowBody[kDiffBodyBytes];
	const bool okEnt = SafeCopy(nowEnt, ent, kDiffEntityBytes);
	void* body = BodyComponentOf(ent);
	const bool sameBody = (body == g_diffBody) && body;
	const bool okBody = sameBody ? SafeCopy(nowBody, body, kDiffBodyBytes) : false;

	int hitsEnt = 0, hitsBody = 0;
	if (okEnt) {
		for (size_t off = 0; off + 4 <= kDiffEntityBytes && hitsEnt < 24; off += 4) {
			float a, b;
			memcpy(&a, g_snapEnt + off, 4);
			memcpy(&b, nowEnt + off, 4);
			if (!FloatPlausibleClock(a, b)) continue;
			++hitsEnt;
			advancedfx::Message("  ENT +0x%04X  %.5f -> %.5f  (d=%.5f)\n", (unsigned)off, a, b, b - a);
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("preview.diff", "ENT off=0x%04X a=%.5f b=%.5f d=%.5f",
					(unsigned)off, a, b, b - a);
		}
	}
	if (okBody) {
		for (size_t off = 0; off + 4 <= kDiffBodyBytes && hitsBody < 24; off += 4) {
			float a, b;
			memcpy(&a, g_snapBody + off, 4);
			memcpy(&b, nowBody + off, 4);
			if (!FloatPlausibleClock(a, b)) continue;
			++hitsBody;
			advancedfx::Message("  BODY +0x%04X  %.5f -> %.5f  (d=%.5f)\n", (unsigned)off, a, b, b - a);
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("preview.diff", "BODY off=0x%04X a=%.5f b=%.5f d=%.5f",
					(unsigned)off, a, b, b - a);
		}
	}
	advancedfx::Message("mvm_previewprobe diff report: idx=%d entHits=%d bodyHits=%d (bodySame=%d)\n",
		g_diffIdx, hitsEnt, hitsBody, sameBody ? 1 : 0);
}

// ---- A/B/C render-state snapshot -------------------------------------------------------------
// `diff` above answers "what advances with the clock". This answers a different question: "what does
// the fix change". A preview pawn created while the demo is PAUSED is posed and animating but its
// body is never drawn; letting the demo play for a moment fixes it permanently. So snapshot the same
// pawn three times -- A (broken), B (broken, later) and C (after the fix) -- and report the bytes
// where A != C but A == B. The A == B term subtracts everything the animation churns on its own,
// which is the only reason a raw before/after diff of a live, animating entity is readable at all.
constexpr size_t kSnapEntBytes = 0x2000;
constexpr size_t kSnapBodyBytes = 0x1000;
constexpr size_t kSnapNodeBytes = 0x400;

struct Snap {
	bool ok = false;
	int idx = -1;
	void* ent = nullptr;
	void* body = nullptr;
	void* node = nullptr;
	uint64_t meshMask = 0;
	unsigned char e[kSnapEntBytes];
	unsigned char b[kSnapBodyBytes];
	unsigned char n[kSnapNodeBytes];
};

Snap g_snapA, g_snapB, g_snapC;

bool TakeSnap(Snap& s, int idx) {
	CEntityInstance* ent = WideResolve(idx);
	if (!ent) return false;
	memset(s.e, 0, sizeof(s.e));
	memset(s.b, 0, sizeof(s.b));
	memset(s.n, 0, sizeof(s.n));
	s.idx = idx;
	s.ent = ent;
	s.body = BodyComponentOf(ent);
	s.node = SceneNodeOf(ent);
	s.meshMask = 0;
	ReadMeshGroupMask(ent, &s.meshMask);
	const bool okEnt = SafeCopy(s.e, ent, kSnapEntBytes);
	if (s.body) SafeCopy(s.b, s.body, kSnapBodyBytes);
	if (s.node) SafeCopy(s.n, s.node, kSnapNodeBytes);
	s.ok = okEnt;
	return okEnt;
}

void SnapReportWindow(const char* tag, const unsigned char* a, const unsigned char* b,
	const unsigned char* c, size_t bytes, int maxHits, int* outHits) {
	int hits = 0;
	for (size_t off = 0; off + 4 <= bytes; off += 4) {
		uint32_t va, vb, vc;
		memcpy(&va, a + off, 4);
		memcpy(&vb, b + off, 4);
		memcpy(&vc, c + off, 4);
		if (va == vc || va != vb) continue; // unchanged by the fix, or churned by animation anyway
		++hits;
		if (hits <= maxHits) {
			float fa, fc;
			memcpy(&fa, &va, 4);
			memcpy(&fc, &vc, 4);
			advancedfx::Message("  %s +0x%04X  %08X -> %08X   (%.4f -> %.4f)\n",
				tag, (unsigned)off, va, vc, fa, fc);
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("preview.snap", "%s off=0x%04X a=%08X c=%08X fa=%.4f fc=%.4f",
					tag, (unsigned)off, va, vc, fa, fc);
		}
	}
	*outHits = hits;
}

void SnapReport(int maxHits) {
	if (!g_snapA.ok || !g_snapB.ok || !g_snapC.ok) {
		advancedfx::Warning("mvm_previewprobe snap: need all three snapshots"
			" (snap a <idx> -> wait -> snap b -> fix -> snap c).\n");
		return;
	}
	if (g_snapA.idx != g_snapB.idx || g_snapA.idx != g_snapC.idx
		|| g_snapA.ent != g_snapB.ent || g_snapA.ent != g_snapC.ent) {
		advancedfx::Warning("mvm_previewprobe snap: the pawn changed between snapshots"
			" (A idx=%d ent=%p, B idx=%d ent=%p, C idx=%d ent=%p) -- retake them.\n",
			g_snapA.idx, g_snapA.ent, g_snapB.idx, g_snapB.ent, g_snapC.idx, g_snapC.ent);
		return;
	}
	int hEnt = 0, hBody = 0, hNode = 0;
	SnapReportWindow("ENT", g_snapA.e, g_snapB.e, g_snapC.e, kSnapEntBytes, maxHits, &hEnt);
	if (g_snapA.body && g_snapA.body == g_snapB.body && g_snapA.body == g_snapC.body)
		SnapReportWindow("BODY", g_snapA.b, g_snapB.b, g_snapC.b, kSnapBodyBytes, maxHits, &hBody);
	if (g_snapA.node && g_snapA.node == g_snapB.node && g_snapA.node == g_snapC.node)
		SnapReportWindow("NODE", g_snapA.n, g_snapB.n, g_snapC.n, kSnapNodeBytes, maxHits, &hNode);
	advancedfx::Message(
		"mvm_previewprobe snap report: idx=%d ent=%p entHits=%d bodyHits=%d nodeHits=%d"
		" meshMask A=0x%llx B=0x%llx C=0x%llx\n",
		g_snapA.idx, g_snapA.ent, hEnt, hBody, hNode,
		(unsigned long long)g_snapA.meshMask, (unsigned long long)g_snapB.meshMask,
		(unsigned long long)g_snapC.meshMask);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("preview.snap",
			"REPORT idx=%d ent=%p entHits=%d bodyHits=%d nodeHits=%d maskA=0x%llx maskB=0x%llx maskC=0x%llx",
			g_snapA.idx, g_snapA.ent, hEnt, hBody, hNode,
			(unsigned long long)g_snapA.meshMask, (unsigned long long)g_snapB.meshMask,
			(unsigned long long)g_snapC.meshMask);
}

// ---- raw window read / write levers ----------------------------------------------------------
// `snap report` names the offsets the fix changes; these turn each candidate into a live experiment.
// `read` also reads the SAME offsets off a normally-rendering pawn, which is what distinguishes
// "this field is how a drawn entity looks" from "this field is noise".
enum class Window { Ent, Body, Node };

bool ParseWindow(const char* s, Window* out) {
	if (!s || !*s) return false;
	if (0 == _stricmp(s, "e") || 0 == _stricmp(s, "ent")) { *out = Window::Ent; return true; }
	if (0 == _stricmp(s, "b") || 0 == _stricmp(s, "body")) { *out = Window::Body; return true; }
	if (0 == _stricmp(s, "n") || 0 == _stricmp(s, "node")) { *out = Window::Node; return true; }
	return false;
}

unsigned char* WindowBase(CEntityInstance* ent, Window w) {
	switch (w) {
	case Window::Ent: return (unsigned char*)ent;
	case Window::Body: return (unsigned char*)BodyComponentOf(ent);
	case Window::Node: return (unsigned char*)SceneNodeOf(ent);
	}
	return nullptr;
}

const char* WindowName(Window w) {
	return (w == Window::Ent) ? "ENT" : (w == Window::Body) ? "BODY" : "NODE";
}

bool SafeReadU32(unsigned char* base, size_t off, uint32_t* out) {
	*out = 0;
	if (!base) return false;
	__try { *out = *(uint32_t*)(base + off); return true; } __except (1) { return false; }
}

bool SafeWriteU32(unsigned char* base, size_t off, uint32_t value) {
	if (!base) return false;
	__try { *(uint32_t*)(base + off) = value; return true; } __except (1) { return false; }
}

// One dword re-asserted every pump frame while the demo is paused. A candidate that only works when
// held (a per-tick "already updated" stamp, say) is invisible to a one-shot poke, so both exist.
int      g_holdIdx = -1;
Window   g_holdWindow = Window::Ent;
size_t   g_holdOff = 0;
uint32_t g_holdValue = 0;
bool     g_holdRelativeTick = false; // value is (currentDemoTick + (int32)g_holdValue)
uint64_t g_holdWrites = 0;

// ---- per-pawn field driver -------------------------------------------------------------------
// Advances one float on the preview pawn every pump frame WHILE PAUSED. This is how a candidate
// found by `diff` gets tested: if driving it makes the character move, that field is the animation
// state the pose is built from, and a per-pawn advance is the fix (no global clock involved).
int    g_driveIdx = -1;
size_t g_driveOff = 0;
float  g_driveRate = 1.0f;   // units per second
bool   g_driveWrap = true;   // cycle semantics: wrap past 1.0
int64_t g_driveLast = 0;
uint64_t g_driveWrites = 0;

bool SafeAddFloat(void* base, size_t off, float add, bool wrap, float* outBefore, float* outAfter) {
	if (!base) return false;
	__try {
		float* p = (float*)((unsigned char*)base + off);
		const float a = *p;
		if (!(a == a)) return false; // NaN guard: never write over garbage
		float b = a + add;
		if (wrap && b > 1.0f) b -= 1.0f;
		*p = b;
		*outBefore = a; *outAfter = b;
		return true;
	} __except (1) { return false; }
}

double DriveWallSeconds() {
	LARGE_INTEGER freq, now;
	if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return 0.0;
	if (!QueryPerformanceCounter(&now)) return 0.0;
	const int64_t prev = g_driveLast;
	g_driveLast = now.QuadPart;
	if (prev == 0) return 0.0;
	const double dt = (double)(now.QuadPart - prev) / (double)freq.QuadPart;
	return (dt > 0.25) ? 0.25 : dt;
}

// Scans the full range and reports every preview-scene entity. Console gets the preview pawns (few);
// the log gets the same plus counts.
void Scan() {
	int resolved = 0, clientOnly = 0, previewPawns = 0, highest = -1;
	advancedfx::Message("mvm_previewprobe scan: class=*%s* over 0..0x%X\n", kPreviewClass, kMaxHandleIndex);
	for (int i = 0; i < kMaxHandleIndex; ++i) {
		CEntityInstance* e = WideResolve(i);
		if (!e) continue;
		++resolved;
		highest = i;
		if (i >= kClientOnlyBase) ++clientOnly;
		const char* cls = SafeClassName(e);
		const char* ccls = SafeClientClassName(e);
		if (!ContainsNoCase(cls, kPreviewClass) && !ContainsNoCase(ccls, kPreviewClass))
			continue;
		++previewPawns;
		char model[512];
		ReadModelPath(e, model, sizeof(model));
		advancedfx::Message("  idx=%d class=%s clientclass=%s model=%s\n",
			i, cls ? cls : "?", ccls ? ccls : "?", model[0] ? model : "-");
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.scan", "PAWN idx=%d client-only=%d class=%s clientclass=%s model=%s",
				i, i >= kClientOnlyBase ? 1 : 0, cls ? cls : "?", ccls ? ccls : "?", model[0] ? model : "-");
	}
	advancedfx::Message("mvm_previewprobe scan: resolved=%d clientOnly=%d previewPawns=%d highestIdx=%d\n",
		resolved, clientOnly, previewPawns, highest);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("preview.scan", "SUMMARY resolved=%d clientOnly=%d previewPawns=%d highestIdx=%d paused=%d",
			resolved, clientOnly, previewPawns, highest, DemoPaused() ? 1 : 0);
}

// First preview pawn in the client-only range, or -1.
int FindFirstPreviewPawn() {
	for (int i = kClientOnlyBase; i < kMaxHandleIndex; ++i) {
		CEntityInstance* e = WideResolve(i);
		if (!e) continue;
		if (ContainsNoCase(SafeClassName(e), kPreviewClass)
			|| ContainsNoCase(SafeClientClassName(e), kPreviewClass))
			return i;
	}
	return -1;
}


// ---- bones: read-only map of a preview pawn's skeleton (for the death-animation model preview) ----
// CModelState keeps the posed bones behind a pointer and the CModel keeps the bone NAMES in a
// pointer-to-char* table; neither is in the schema, so this walks both and reports every candidate
// instead of trusting one fixed offset. Nothing is written.
bool SafeQword(const void* p, uint64_t& out) {
	out = 0;
	if ((uintptr_t)p < 0x10000) return false;
	__try { out = *(const uint64_t*)p; return true; } __except (1) { return false; }
}
bool SafeAscii(const char* s, char* out, size_t n) {
	if (out && n) out[0] = '\0';
	if ((uintptr_t)s < 0x10000 || !out || n < 2) return false;
	__try {
		size_t i = 0;
		for (; i + 1 < n; ++i) {
			const char c = s[i];
			if (!c) break;
			if (c < 0x20 || c > 0x7e) { out[0] = '\0'; return false; }
			out[i] = c;
		}
		out[i] = '\0';
		return i >= 2 && s[i] == '\0';
	} __except (1) { out[0] = '\0'; return false; }
}
bool SafeFloats(const void* p, float* out, int n) {
	if ((uintptr_t)p < 0x10000) return false;
	__try { memcpy(out, p, sizeof(float) * n); return true; } __except (1) { return false; }
}
bool Plausible(float v) { return v == v && v > -100000.f && v < 100000.f; }


// Parent table: an int16 or int32 array in the CModel reading -1,0,1,2,3 (root_motion, pelvis,
// spine_0, spine_1, spine_2 -- the order the name table shows).
void ScanParents(uint64_t model) {
	for (size_t off = 0; off < 0x600; off += 8) {
		uint64_t t = 0;
		if (!SafeQword((void*)(model + off), t) || t < 0x10000) continue;
		short s16[6] = {}; int s32[6] = {};
		if (SafeFloats((void*)t, (float*)s16, 3) && s16[0] == -1 && s16[1] == 0 && s16[2] == 1 && s16[3] == 2 && s16[4] == 3)
			advancedfx::Message("  parents(int16)? model+0x%03llx -> %p\n", (unsigned long long)off, (void*)t);
		if (SafeFloats((void*)t, (float*)s32, 6) && s32[0] == -1 && s32[1] == 0 && s32[2] == 1 && s32[3] == 2 && s32[4] == 3)
			advancedfx::Message("  parents(int32)? model+0x%03llx -> %p\n", (unsigned long long)off, (void*)t);
	}
}

void BonesDump(int idx, int showBones) {
	CEntityInstance* ent = WideResolve(idx);
	if (!ent) { advancedfx::Warning("mvm_previewprobe bones: idx %d does not resolve.\n", idx); return; }
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	unsigned char* body = (unsigned char*)BodyComponentOf(ent);
	if (!body || !o.ModelChain.m_skeletonInstance || !o.ModelChain.m_modelState) {
		advancedfx::Warning("mvm_previewprobe bones: no body component / skeleton offsets.\n"); return;
	}
	unsigned char* skel = body + o.ModelChain.m_skeletonInstance;
	unsigned char* ms = skel + o.ModelChain.m_modelState;
	float ox = 0, oy = 0, oz = 0;
	SafeGetOrigin(ent, ox, oy, oz);
	char model[256]; ReadModelPath(ent, model, sizeof(model));
	advancedfx::Message("bones idx=%d class=%s model=%s origin=(%.1f %.1f %.1f)\n  skel=%p (+0x%llx) modelState=%p (+0x%llx) modelName@+0x%llx\n",
		idx, SafeClassName(ent), model, ox, oy, oz, skel, (unsigned long long)o.ModelChain.m_skeletonInstance,
		ms, (unsigned long long)o.ModelChain.m_modelState, (unsigned long long)o.ModelChain.m_ModelName);

	// 1. Bone array: any pointer in the first 0x100 of CModelState whose 32-byte records read as
	//    positions near the pawn (world space) or near zero (model space), with unit quaternions.
	for (size_t off = 0; off < 0x100; off += 8) {
		uint64_t q = 0;
		if (!SafeQword(ms + off, q) || q < 0x10000 || q > 0x7fffffffffffull) continue;
		int ok = 0, nearOrigin = 0;
		for (int b = 0; b < 8; ++b) {
			float r[8];
			if (!SafeFloats((void*)(q + b * 32), r, 8)) break;
			const float ql = r[4] * r[4] + r[5] * r[5] + r[6] * r[6] + r[7] * r[7];
			if (!Plausible(r[0]) || !Plausible(r[1]) || !Plausible(r[2]) || ql < 0.95f || ql > 1.05f) break;
			++ok;
			const float dx = r[0] - ox, dy = r[1] - oy, dz = r[2] - oz;
			if (dx * dx + dy * dy + dz * dz < 150.f * 150.f) ++nearOrigin;
		}
		if (ok < 4) continue;
		advancedfx::Message("  bonearray? modelState+0x%02llx -> %p  records=%d nearOrigin=%d\n",
			(unsigned long long)off, (void*)q, ok, nearOrigin);
		for (int b = 0; b < showBones; ++b) {
			float r[8];
			if (!SafeFloats((void*)(q + b * 32), r, 8)) break;
			advancedfx::Message("    [%2d] p=(%8.2f %8.2f %8.2f) s=%.3f q=(%.3f %.3f %.3f %.3f)\n",
				b, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
		}
	}

	// 2. Bone names: the CModel behind CModelState's model handle (just before m_ModelName), then any
	//    pointer in it to a table of char* whose entries read as identifiers.
	for (ptrdiff_t hoff = -0x20; hoff <= 0x20; hoff += 8) {
		uint64_t h = 0, model = 0;
		if (!SafeQword(ms + o.ModelChain.m_ModelName + hoff, h) || h < 0x10000) continue;
		if (!SafeQword((void*)h, model) || model < 0x10000) continue;
		for (size_t off = 0; off < 0x600; off += 8) {
			uint64_t table = 0, s0 = 0, s1 = 0;
			if (!SafeQword((void*)(model + off), table) || table < 0x10000) continue;
			if (!SafeQword((void*)table, s0) || !SafeQword((void*)(table + 8), s1)) continue;
			char n0[48], n1[48];
			if (!SafeAscii((const char*)s0, n0, sizeof(n0)) || !SafeAscii((const char*)s1, n1, sizeof(n1))) continue;
			int count = 0;
			std::string list;
			for (; count < 256; ++count) {
				uint64_t si = 0; char ni[48];
				if (!SafeQword((void*)(table + 8 * count), si) || !SafeAscii((const char*)si, ni, sizeof(ni))) break;
				if (count < 24) { list += ni; list += ' '; }
			}
			advancedfx::Message("  names? handle@modelName%+lld model=%p +0x%03llx -> %d names: %s\n",
				(long long)hoff, (void*)model, (unsigned long long)off, count, list.c_str());
		}
		if (hoff == -8) ScanParents(model);
	}
}

} // namespace

void PreviewSceneProbe_RunFrame() {
	// Held dword (independent of everything else): re-assert one candidate field every frame while the
	// demo is paused, so a per-tick "already updated" stamp can be invalidated and its consumer re-run.
	if (g_holdIdx >= 0 && DemoPaused()) {
		if (CEntityInstance* he = WideResolve(g_holdIdx)) {
			unsigned char* base = WindowBase(he, g_holdWindow);
			uint32_t value = g_holdValue;
			if (g_holdRelativeTick) {
				int tick = 0;
				if (g_MirvTime.GetCurrentDemoTick(tick))
					value = (uint32_t)(tick + (int32_t)g_holdValue);
			}
			if (SafeWriteU32(base, g_holdOff, value)) {
				++g_holdWrites;
				if (MvmDebugLog_Active() && (g_holdWrites % 64) == 1)
					MvmDebugLog_LinefAlways("preview.hold",
						"idx=%d %s+0x%04X = 0x%08X writes=%llu relTick=%d",
						g_holdIdx, WindowName(g_holdWindow), (unsigned)g_holdOff, value,
						(unsigned long long)g_holdWrites, g_holdRelativeTick ? 1 : 0);
			}
		}
	}

	// Field driver (independent of the watch): advance one float per frame while the demo is paused.
	if (g_driveIdx >= 0) {
		if (!DemoPaused()) {
			g_driveLast = 0; // engine advances it itself while playing
		} else if (CEntityInstance* de = WideResolve(g_driveIdx)) {
			const double dt = DriveWallSeconds();
			if (dt > 0.0) {
				float a = 0, b = 0;
				if (SafeAddFloat(de, g_driveOff, (float)(dt * (double)g_driveRate), g_driveWrap, &a, &b)) {
					++g_driveWrites;
					if (MvmDebugLog_Active() && (g_driveWrites % 32) == 0)
						MvmDebugLog_LinefAlways("preview.drive",
							"idx=%d off=0x%04X writes=%llu %.5f -> %.5f rate=%.3f",
							g_driveIdx, (unsigned)g_driveOff, (unsigned long long)g_driveWrites, a, b, g_driveRate);
				}
			}
		}
	}

	if (g_watchIndex < 0)
		return;
	++g_pumpFrame;
	if (g_throttle > 1 && (g_pumpFrame % (uint64_t)g_throttle) != 0)
		return;

	CEntityInstance* ent = WideResolve(g_watchIndex);
	if (!ent) {
		// The preview panel is rebuilt on model/item change, so the index can go away. Report it once
		// per streak rather than every sample.
		if (++g_resolveFailStreak == 1 && MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.watch", "UNRESOLVED idx=%d frame=%llu -- panel rebuilt? re-run `mvm_previewprobe scan`",
				g_watchIndex, (unsigned long long)g_pumpFrame);
		return;
	}
	g_resolveFailStreak = 0;

	if (g_attachIdx == 0)
		BindAttachment(ent);

	float ox = 0, oy = 0, oz = 0;
	SafeGetOrigin(ent, ox, oy, oz);

	SOURCESDK::Vector aOrigin; aOrigin.x = aOrigin.y = aOrigin.z = 0;
	SOURCESDK::Quaternion aAngles; aAngles.x = aAngles.y = aAngles.z = aAngles.w = 0;
	const bool haveAttach = SafeGetAttachment(ent, g_attachIdx, aOrigin, aAngles);

	int tick = 0;
	const bool haveTick = g_MirvTime.GetCurrentDemoTick(tick);

	// MvmDebugLog_LinefAlways, NOT _Linef: a FROZEN animgraph emits a byte-identical payload every
	// sample, and the deduplicating logger would collapse exactly the evidence this probe exists to
	// capture ("the value did not change while paused"). frame= also keeps each line distinguishable.
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("preview.watch",
			"idx=%d frame=%llu tick=%d paused=%d attach=%s(%u) ok=%d apos=%.5f,%.5f,%.5f arot=%.5f,%.5f,%.5f,%.5f origin=%.3f,%.3f,%.3f",
			g_watchIndex, (unsigned long long)g_pumpFrame, haveTick ? tick : -1, DemoPaused() ? 1 : 0,
			g_attachName[0] ? g_attachName : "-", (unsigned)g_attachIdx, haveAttach ? 1 : 0,
			aOrigin.x, aOrigin.y, aOrigin.z, aAngles.x, aAngles.y, aAngles.z, aAngles.w,
			ox, oy, oz);
}

void PreviewSceneProbe_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 2) ? args->ArgV(1) : "status";

	if (0 == _stricmp(arg, "scan")) {
		Scan();
		return;
	}

	// bones [idx|first] [show]: read-only skeleton map (bone array + bone-name table candidates).
	if (0 == _stricmp(arg, "bones")) {
		int idx = (argc >= 3 && _stricmp(args->ArgV(2), "first")) ? atoi(args->ArgV(2)) : FindFirstPreviewPawn();
		const int show = (argc >= 4) ? atoi(args->ArgV(3)) : 6;
		if (idx < 0) { advancedfx::Warning("mvm_previewprobe bones: no preview pawn.\n"); return; }
		BonesDump(idx, show);
		return;
	}

	if (0 == _stricmp(arg, "refresh")) {
		if (argc < 3) {
			advancedfx::Warning("usage: %s refresh <previewEntityIndex>\n", cmd);
			return;
		}
		const int idx = std::atoi(args->ArgV(2));
		CEntityInstance* ent = WideResolve(idx);
		if (!ent || !ContainsNoCase(SafeClassName(ent), kPreviewClass)) {
			advancedfx::Warning("%s: idx %d is not a live *%s* entity.\n",
				cmd, idx, kPreviewClass);
			return;
		}
		void* node = nullptr;
		const ptrdiff_t nodeOffset = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
		if (nodeOffset) {
			__try { node = *(void**)((unsigned char*)ent + nodeOffset); }
			__except (1) { node = nullptr; }
		}
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.refresh",
				"BEGIN idx=%d ent=%p nodeOffset=0x%llx node=%p demoWrite=0 frameWrite=0",
				idx, (void*)ent, (unsigned long long)nodeOffset, node);
		const bool ok = PostDataUpdate((unsigned char*)ent);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.refresh",
				"END idx=%d ent=%p node=%p postDataUpdate=%d demoWrite=0 frameWrite=0",
				idx, (void*)ent, node, ok ? 1 : 0);
		advancedfx::Message(
			"%s: refresh idx=%d ent=%p nodeOffset=0x%llx node=%p postDataUpdate=%d"
			" (demoWrite=0 frameWrite=0).\n",
			cmd, idx, (void*)ent, (unsigned long long)nodeOffset, node, ok ? 1 : 0);
		return;
	}

	// Render-state readout for every preview pawn (or one). meshMask is the field of interest: 0 means
	// the model draws no geometry at all, which is what "the pawn is posed and animating but invisible"
	// looks like from the renderer's side.
	if (0 == _stricmp(arg, "rs")) {
		const int only = (argc >= 3) ? std::atoi(args->ArgV(2)) : -1;
		advancedfx::Message("mvm_previewprobe rs: paused=%d maskOffsets(body=0x%llx skel=0x%llx"
			" state=0x%llx mask=0x%llx)\n",
			DemoPaused() ? 1 : 0,
			(unsigned long long)g_clientDllOffsets.ModelChain.m_CBodyComponent,
			(unsigned long long)g_clientDllOffsets.ModelChain.m_skeletonInstance,
			(unsigned long long)g_clientDllOffsets.ModelChain.m_modelState,
			(unsigned long long)g_clientDllOffsets.ModelChain.m_MeshGroupMask);
		for (int i = kClientOnlyBase; i < kMaxHandleIndex; ++i) {
			if (only >= 0 && i != only) continue;
			CEntityInstance* e = WideResolve(i);
			if (!e) continue;
			if (only < 0 && !ContainsNoCase(SafeClassName(e), kPreviewClass)
				&& !ContainsNoCase(SafeClientClassName(e), kPreviewClass))
				continue;
			char model[512];
			ReadModelPath(e, model, sizeof(model));
			uint64_t mask = 0;
			const bool maskOk = ReadMeshGroupMask(e, &mask);
			float ox = 0, oy = 0, oz = 0;
			SafeGetOrigin(e, ox, oy, oz);
			advancedfx::Message("  idx=%d ent=%p body=%p node=%p meshMask=0x%llx(ok=%d)"
				" origin=%.2f,%.2f,%.2f model=%s\n",
				i, (void*)e, BodyComponentOf(e), SceneNodeOf(e),
				(unsigned long long)mask, maskOk ? 1 : 0, ox, oy, oz, model[0] ? model : "-");
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("preview.rs",
					"idx=%d ent=%p body=%p node=%p meshMask=0x%llx ok=%d paused=%d origin=%.2f,%.2f,%.2f model=%s",
					i, (void*)e, BodyComponentOf(e), SceneNodeOf(e), (unsigned long long)mask,
					maskOk ? 1 : 0, DemoPaused() ? 1 : 0, ox, oy, oz, model[0] ? model : "-");
		}
		return;
	}

	// Write lever for the read above -- only meaningful if `rs` shows a zero/wrong mask.
	if (0 == _stricmp(arg, "mask")) {
		if (argc < 4) {
			advancedfx::Warning("usage: %s mask <index> <hexMask>\n", cmd);
			return;
		}
		const int idx = std::atoi(args->ArgV(2));
		const uint64_t want = (uint64_t)_strtoui64(args->ArgV(3), nullptr, 16);
		CEntityInstance* ent = WideResolve(idx);
		if (!ent) { advancedfx::Warning("%s: idx %d does not resolve.\n", cmd, idx); return; }
		uint64_t before = 0, after = 0;
		ReadMeshGroupMask(ent, &before);
		const bool wrote = WriteMeshGroupMask(ent, want);
		const bool refreshed = PostDataUpdate((unsigned char*)ent);
		ReadMeshGroupMask(ent, &after);
		advancedfx::Message("%s: mask idx=%d 0x%llx -> 0x%llx (wrote=%d postDataUpdate=%d now=0x%llx)\n",
			cmd, idx, (unsigned long long)before, (unsigned long long)want,
			wrote ? 1 : 0, refreshed ? 1 : 0, (unsigned long long)after);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.rs", "MASKWRITE idx=%d before=0x%llx want=0x%llx now=0x%llx wrote=%d pdu=%d",
				idx, (unsigned long long)before, (unsigned long long)want,
				(unsigned long long)after, wrote ? 1 : 0, refreshed ? 1 : 0);
		return;
	}

	// Three-point snapshot: see the SnapReport comment. a <idx> -> (wait) b -> (fix) c -> report.
	if (0 == _stricmp(arg, "snap")) {
		const char* which = (argc >= 3) ? args->ArgV(2) : "";
		if (0 == _stricmp(which, "report")) {
			SnapReport((argc >= 4) ? std::atoi(args->ArgV(3)) : 48);
			return;
		}
		Snap* target = nullptr;
		if (0 == _stricmp(which, "a")) target = &g_snapA;
		else if (0 == _stricmp(which, "b")) target = &g_snapB;
		else if (0 == _stricmp(which, "c")) target = &g_snapC;
		if (!target) {
			advancedfx::Warning("usage: %s snap a <index> | %s snap b | %s snap c | %s snap report [max]\n",
				cmd, cmd, cmd, cmd);
			return;
		}
		int idx = (argc >= 4) ? std::atoi(args->ArgV(3)) : g_snapA.idx;
		if (idx < 0) { advancedfx::Warning("%s: snap a needs an index first.\n", cmd); return; }
		const bool ok = TakeSnap(*target, idx);
		advancedfx::Message("%s: snap %s idx=%d ok=%d ent=%p body=%p node=%p meshMask=0x%llx paused=%d\n",
			cmd, which, idx, ok ? 1 : 0, target->ent, target->body, target->node,
			(unsigned long long)target->meshMask, DemoPaused() ? 1 : 0);
		return;
	}

	if (0 == _stricmp(arg, "read")) {
		Window w = Window::Ent;
		if (argc < 5 || !ParseWindow(args->ArgV(3), &w)) {
			advancedfx::Warning("usage: %s read <index> <ent|body|node> <hexOffset> [dwords]\n", cmd);
			return;
		}
		const int idx = std::atoi(args->ArgV(2));
		const size_t off = (size_t)strtoul(args->ArgV(4), nullptr, 16);
		int count = (argc >= 6) ? std::atoi(args->ArgV(5)) : 1;
		if (count < 1) count = 1;
		if (count > 16) count = 16;
		CEntityInstance* ent = WideResolve(idx);
		unsigned char* base = ent ? WindowBase(ent, w) : nullptr;
		if (!base) { advancedfx::Warning("%s: idx %d window %s unavailable.\n", cmd, idx, args->ArgV(3)); return; }
		for (int i = 0; i < count; ++i) {
			uint32_t v = 0;
			const bool ok = SafeReadU32(base, off + (size_t)i * 4, &v);
			float f; memcpy(&f, &v, 4);
			advancedfx::Message("  %s+0x%04X = %08X (%u / %.4f) ok=%d\n",
				WindowName(w), (unsigned)(off + (size_t)i * 4), v, v, f, ok ? 1 : 0);
		}
		return;
	}

	if (0 == _stricmp(arg, "poke")) {
		Window w = Window::Ent;
		if (argc < 6 || !ParseWindow(args->ArgV(3), &w)) {
			advancedfx::Warning("usage: %s poke <index> <ent|body|node> <hexOffset> <hexValue>\n", cmd);
			return;
		}
		const int idx = std::atoi(args->ArgV(2));
		const size_t off = (size_t)strtoul(args->ArgV(4), nullptr, 16);
		const uint32_t value = (uint32_t)strtoul(args->ArgV(5), nullptr, 16);
		CEntityInstance* ent = WideResolve(idx);
		unsigned char* base = ent ? WindowBase(ent, w) : nullptr;
		if (!base) { advancedfx::Warning("%s: idx %d window %s unavailable.\n", cmd, idx, args->ArgV(3)); return; }
		uint32_t before = 0;
		SafeReadU32(base, off, &before);
		const bool ok = SafeWriteU32(base, off, value);
		advancedfx::Message("%s: poke idx=%d %s+0x%04X %08X -> %08X ok=%d\n",
			cmd, idx, WindowName(w), (unsigned)off, before, value, ok ? 1 : 0);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.poke", "idx=%d %s+0x%04X before=%08X value=%08X ok=%d",
				idx, WindowName(w), (unsigned)off, before, value, ok ? 1 : 0);
		return;
	}

	if (0 == _stricmp(arg, "hold")) {
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "off")) {
			advancedfx::Message("%s: hold off (writes=%llu).\n", cmd, (unsigned long long)g_holdWrites);
			g_holdIdx = -1;
			return;
		}
		Window w = Window::Ent;
		if (argc < 6 || !ParseWindow(args->ArgV(3), &w)) {
			advancedfx::Warning("usage: %s hold <index> <ent|body|node> <hexOffset> <hexValue|tick+N|tick-N>"
				"  |  %s hold off\n", cmd, cmd);
			return;
		}
		const int idx = std::atoi(args->ArgV(2));
		if (!WideResolve(idx)) { advancedfx::Warning("%s: idx %d does not resolve.\n", cmd, idx); return; }
		const char* valueArg = args->ArgV(5);
		g_holdRelativeTick = (0 == _strnicmp(valueArg, "tick", 4));
		g_holdValue = g_holdRelativeTick
			? (uint32_t)(int32_t)std::atoi(valueArg + 4)
			: (uint32_t)strtoul(valueArg, nullptr, 16);
		g_holdIdx = idx;
		g_holdWindow = w;
		g_holdOff = (size_t)strtoul(args->ArgV(4), nullptr, 16);
		g_holdWrites = 0;
		advancedfx::Message("%s: holding idx=%d %s+0x%04X = %s%08X every frame WHILE PAUSED.\n",
			cmd, idx, WindowName(w), (unsigned)g_holdOff,
			g_holdRelativeTick ? "tick+" : "", g_holdValue);
		return;
	}

	if (0 == _stricmp(arg, "drive")) {
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "off")) {
			g_driveIdx = -1; g_driveLast = 0;
			advancedfx::Message("%s: drive off (writes=%llu).\n", cmd, (unsigned long long)g_driveWrites);
			return;
		}
		if (argc < 4) {
			advancedfx::Warning("usage: %s drive <index> <hexOffset> [ratePerSec] [nowrap]  |  %s drive off\n", cmd, cmd);
			return;
		}
		const int idx = std::atoi(args->ArgV(2));
		const size_t off = (size_t)strtoul(args->ArgV(3), nullptr, 16);
		g_driveRate = (argc >= 5) ? (float)atof(args->ArgV(4)) : 1.0f;
		g_driveWrap = !(argc >= 6 && 0 == _stricmp(args->ArgV(5), "nowrap"));
		if (!WideResolve(idx)) { advancedfx::Warning("%s: idx %d does not resolve.\n", cmd, idx); return; }
		g_driveIdx = idx; g_driveOff = off; g_driveLast = 0; g_driveWrites = 0;
		advancedfx::Message("%s: driving idx=%d off=0x%04X rate=%.3f/s wrap=%d (while PAUSED only).\n",
			cmd, idx, (unsigned)off, g_driveRate, g_driveWrap ? 1 : 0);
		return;
	}

	if (0 == _stricmp(arg, "diff")) {
		if (argc >= 3 && 0 == _stricmp(args->ArgV(2), "report")) { DiffReport(); return; }
		if (argc < 3) { advancedfx::Warning("usage: %s diff <index> | %s diff report\n", cmd, cmd); return; }
		DiffSnapshot(std::atoi(args->ArgV(2)));
		return;
	}

	if (0 == _stricmp(arg, "watch")) {
		int idx = -1;
		if (argc >= 3 && 0 != _stricmp(args->ArgV(2), "auto")) {
			idx = std::atoi(args->ArgV(2));
		} else {
			idx = FindFirstPreviewPawn();
			if (idx < 0) {
				advancedfx::Warning("%s: no *%s* entity found -- open the Customize modal first, then `%s scan`.\n",
					cmd, kPreviewClass, cmd);
				return;
			}
		}
		CEntityInstance* e = WideResolve(idx);
		if (!e) {
			advancedfx::Warning("%s: index %d does not resolve.\n", cmd, idx);
			return;
		}
		if (argc >= 4) {
			const int t = std::atoi(args->ArgV(3));
			g_throttle = (t >= 1) ? t : 1;
		}
		g_watchIndex = idx;
		g_pumpFrame = 0;
		g_resolveFailStreak = 0;
		BindAttachment(e);
		char model[512];
		ReadModelPath(e, model, sizeof(model));
		const char* cls = SafeClassName(e);
		advancedfx::Message("%s: watching idx=%d class=%s model=%s attach=%s(%u) every %d frame(s).\n",
			cmd, idx, cls ? cls : "?", model[0] ? model : "-",
			g_attachName[0] ? g_attachName : "NONE-RESOLVED", (unsigned)g_attachIdx, g_throttle);
		if (g_attachIdx == 0)
			advancedfx::Warning("%s: no candidate attachment resolved on this model -- the sample will only carry"
				" the root origin, which an idle animation does not move. Escalate to a bone read.\n", cmd);
		if (!MvmDebugLog_Active())
			advancedfx::Warning("%s: mvm_debug is NOT running -- samples go nowhere. Run `mvm_debug start`.\n", cmd);
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("preview.watch", "ARM idx=%d class=%s model=%s attach=%s(%u) throttle=%d",
				idx, cls ? cls : "?", model[0] ? model : "-",
				g_attachName[0] ? g_attachName : "-", (unsigned)g_attachIdx, g_throttle);
		return;
	}

	if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "stop")) {
		if (MvmDebugLog_Active() && g_watchIndex >= 0)
			MvmDebugLog_LinefAlways("preview.watch", "DISARM idx=%d samples=%llu",
				g_watchIndex, (unsigned long long)g_pumpFrame);
		g_watchIndex = -1;
		advancedfx::Message("%s: watch off.\n", cmd);
		return;
	}

	if (0 == _stricmp(arg, "status")) {
		advancedfx::Message("%s: watch=%s idx=%d attach=%s(%u) throttle=%d pumpFrames=%llu mvm_debug=%s demoPaused=%d\n",
			cmd, g_watchIndex >= 0 ? "ON" : "off", g_watchIndex,
			g_attachName[0] ? g_attachName : "-", (unsigned)g_attachIdx, g_throttle,
			(unsigned long long)g_pumpFrame, MvmDebugLog_Active() ? "on" : "off", DemoPaused() ? 1 : 0);
		return;
	}

	advancedfx::Message(
		"%s read <index> <ent|body|node> <hexOff> [dwords] - raw dword read.\n"
		"%s poke <index> <ent|body|node> <hexOff> <hexVal> - one-shot raw dword write.\n"
		"%s hold <index> <ent|body|node> <hexOff> <hexVal|tick+N> | hold off - re-assert it per frame.\n"
		"%s rs [<index>] - render state of the preview pawns (mesh-group mask, node, origin, model).\n"
		"%s mask <index> <hexMask> - write a mesh-group mask + refresh the renderable (lever for rs).\n"
		"%s snap a <index>|b|c|report [max] - what the fix changed, minus what animation churns.\n"
		"%s refresh <index> - call the preview pawn scene node's native PostDataUpdate once.\n"
		"%s scan - full-range (0..0x8000) list of preview-scene pawns (index/class/model).\n"
		"%s watch [<index>|auto] [everyNframes] - sample the animation-advance proxy into mvm_debug.\n"
		"%s off - stop sampling.\n"
		"%s status - current state.\n"
		"Usage: `mvm_debug start`, open the Customize modal, `%s scan`, `%s watch auto`, then compare\n"
		"the preview.watch attachment origin across a PLAYING and a PAUSED stretch.\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker

CON_COMMAND(mvm_previewprobe, "Read-only probe: does the Customize player preview animate while the demo is paused?") {
	Filmmaker::PreviewSceneProbe_RunCommand(args->ArgC(), args, args->ArgV(0));
}
