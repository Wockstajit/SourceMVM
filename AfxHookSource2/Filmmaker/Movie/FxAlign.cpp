#include "FxAlign.h"

#include "FollowCameraMath.h"        // FollowVec3/FollowAngles + rotate/distance helpers
#include "FollowTargetProviders.h"   // EntityAt, EntityClass, ResolveAttachTransform
#include "ParticleFx.h"
#include "../Cosmetics/CosmeticModelSwap.h" // ReadActiveViewmodelWeaponState (viewmodel weapon entity)
#include "../Debug/MvmDebugLog.h"  // MvmDebugLog_* breadcrumbs
#include "../../ClientEntitySystem.h"       // AfxGetSpectatedPawnIndex, CEntityInstance
#include "../../hlaeFolder.h"               // GetHlaeRoamingAppDataFolderW
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h" // SOURCESDK::Vector/Quaternion
#include "../../../Shared/AfxConsole.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Filmmaker {

namespace {

// ============================== state ==============================================

struct PendingSample {
	std::string raw;      // lowercased vanilla resource path
	std::string target;   // lowercased swap-target resource path
	void* instance = nullptr; // engine create return (unvalidated; SEH-read only)
	int demoTick = -1;
	unsigned long long wallMs = 0;
};

struct Agg {
	int count = 0;
	int pass = 0;
	int cpScan = 0;       // samples measured off the live collection (ground truth)
	double sumDist = 0.0;
	double maxDist = 0.0;
};

std::atomic<bool> g_enabled{ false };
std::mutex g_mx;                       // guards everything below
std::vector<PendingSample> g_pending;  // hook -> pump handoff
unsigned long long g_droppedPending = 0;
std::map<std::string, Agg> g_aggs;     // "<class>|<effect>" -> aggregate
unsigned long long g_droppedGate = 0;  // third-person outside the watched fire window
unsigned long long g_droppedNoPawn = 0;// no spectated pawn at resolve time
int g_samples = 0;
int g_cpScanSamples = 0;
int g_attachSamples = 0;               // samples whose reference was a real muzzle attachment
double g_threshold = 2.5;              // Source units; see PrintStatus for rationale
// Barrel-smoke raws (weapon_muzzle_smoke*) carry no weapon class; attribute them to the
// most recent classified muzzle flash within the same burst (demo-tick window).
std::string g_lastFlashClass;
int g_lastFlashTick = -0x40000000;
constexpr int kFlashClassWindowTicks = 64;
constexpr size_t kPendingCap = 128;
constexpr size_t kScanBytes = 0xE00;   // how deep into the collection the CP scan looks
constexpr double kScanRangeUnits = 400.0; // float3 farther than this from the muzzle = not ours
// Mean of the Modern packs' authored C_INIT_PositionOffset (postprocess_modern.py
// _CS2_MODERN_ROPE_TRAIL_OFFSET_BLOCK: min [0,0,-0.5], max [0.5,0,0]) -- the config-offset
// fallback measures this authored offset only, so keep it in sync with the postprocess.
constexpr double kModernCfgOffset[3] = { 0.25, 0.0, -0.25 };

// ============================== classification =====================================

// Muzzle-FX effect kind off the swap target, or null for targets the probe ignores.
// Classification is by name substring, pack-agnostic.
const char* EffectKindFor(const char* target) {
	if (!std::strstr(target, "/modern/") && !std::strstr(target, "/insurgency/"))
		return nullptr;
	if (std::strstr(target, "mvm_grenade_trail"))
		return nullptr; // grenade flight trail: not muzzle-anchored
	if (std::strstr(target, "barrel_smoke"))
		return "barrelsmoke";
	if (std::strstr(target, "muzzle_smoke"))
		return "barrelsmoke";
	if (std::strstr(target, "muzzleflash") || std::strstr(target, "muzzle_flash"))
		return "muzzleflash"; // Modern muzzleflash_*/mvm_muzzleflash_sniper_*, Insurgency muzzleflash_<weapon>_1p/_3rd
	return nullptr;
}

// GMod per-shot class flashes carry barrel_smoke as a child, so it never gets its own
// swap target. When the parent flash is measured, mirror the muzzle distance into
// barrelsmoke.
bool HasPerShotBarrelSmokeChild(const char* target) {
	if (!target || !std::strstr(target, "muzzleflash_"))
		return false;
	if (std::strstr(target, "muzzleflash_suppressed"))
		return false;
	if (std::strstr(target, "mvm_muzzleflash_sniper"))
		return false;
	static const char* kClassFlashes[] = {
		"muzzleflash_ar.vpcf",
		"muzzleflash_smg.vpcf",
		"muzzleflash_shotgun.vpcf",
		"muzzleflash_pistol.vpcf",
		"muzzleflash_pistol_deagle.vpcf",
		"muzzleflash_lmg.vpcf",
		"muzzleflash_dmr.vpcf",
		nullptr,
	};
	for (const char** flash = kClassFlashes; *flash; ++flash) {
		if (std::strstr(target, *flash))
			return true;
	}
	return false;
}

void RecordDerivedAlignSample(const std::string& wclass, const char* effect, double dist,
	bool pass, const char* method, bool attachment) {
	std::lock_guard<std::mutex> lock(g_mx);
	Agg& a = g_aggs[wclass + "|" + effect];
	++a.count;
	if (pass)
		++a.pass;
	if (0 == std::strcmp(method, "cp-scan"))
		++a.cpScan;
	a.sumDist += dist;
	if (dist > a.maxDist)
		a.maxDist = dist;
	++g_samples;
	if (0 == std::strcmp(method, "cp-scan"))
		++g_cpScanSamples;
	if (attachment)
		++g_attachSamples;
}

// First-person (viewmodel) weapon FX path: _fps/_fp suffixed systems. Only these are the
// spectated player's own viewmodel effects -- third-person flashes of every OTHER player
// in the demo also reach the hook, and measuring those against the spectated pawn's
// muzzle is noise (live run 2026-07-04: 90% of unfiltered samples were third-person
// raws resolving to "weapon-origin"). Mirrors ParticleFx.cpp's IsFirstPersonWeaponFxPath.
bool IsFpPath(const char* low) {
	if (!low || !low[0])
		return false;
	const char* fp = std::strstr(low, "_fp");
	if (!fp)
		return false;
	if (fp[3] == 's' || fp[3] == 0 || fp[3] == '.' || fp[3] == '_')
		return true;
	return false;
}

// Weapon class off the VANILLA resource path (matches the buckets kVariantWeaponFx
// encodes). Substring order matters: the more specific names first.
const char* WeaponClassFromRaw(const char* raw) {
	if (std::strstr(raw, "snip_ar")) return "autosniper";
	if (std::strstr(raw, "muzzleflash_snip")) return "awp";
	if (std::strstr(raw, "riffle_lrg")) return "autosniper";
	if (std::strstr(raw, "muzsilenced_subm")) return "smg_silenced";
	if (std::strstr(raw, "muzsilenced_rif")) return "rifle_silenced";
	if (std::strstr(raw, "subm")) return "smg";
	if (std::strstr(raw, "mach")) return "lmg";
	if (std::strstr(raw, "shot")) return "shotgun";
	if (std::strstr(raw, "deagle")) return "deagle";
	if (std::strstr(raw, "revolver")) return "revolver";
	if (std::strstr(raw, "pist")) return "pistol";
	if (std::strstr(raw, "ak47") || std::strstr(raw, "riffle") || std::strstr(raw, "aug")
		|| std::strstr(raw, "_sg_") || std::strstr(raw, "muzzleflash_basic"))
		return "assaultrifle";
	return nullptr; // e.g. weapon_muzzle_smoke* -- classless, attributed to the last flash
}

// ============================== guarded reads =======================================
// Separate C-only functions: MSVC forbids __try where C++ unwinding objects live.

bool SehCopyBytes(const void* src, void* dst, size_t n) {
	__try {
		std::memcpy(dst, src, n);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// SEH-guarded named-attachment read. The FIRST-PERSON viewmodel weapon is a client-only
// entity (C_CS2HudModelWeapon) that can be mid-reconstruction while a particle swap fires
// during live (unpaused) playback; the unguarded ResolveAttachTransform faulted CS2 ~8s into
// the first capture run. Only POD locals live across the __try so no C++ unwinding is required.
bool SehReadAttachment(CEntityInstance* ent, const char* name, FollowVec3& pos, FollowAngles& ang) {
	if (!ent || !name)
		return false;
	SOURCESDK::Vector origin;
	SOURCESDK::Quaternion q;
	bool ok = false;
	__try {
		const unsigned char idx = ent->LookupAttachment(name);
		if (idx)
			ok = ent->GetAttachment(idx, origin, q);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!ok)
		return false;
	pos = FollowVec3{ origin.x, origin.y, origin.z };
	ang = FollowQuatToAngles(q.x, q.y, q.z, q.w);
	return true;
}

// ============================== muzzle reference ====================================

struct MuzzleRef {
	bool valid = false;
	bool attachment = false; // true = real model attachment (high confidence)
	FollowVec3 pos;
	FollowAngles ang;
	FollowVec3 eye;          // spectated pawn eye (scan sanity anchor)
	std::string source;      // "viewmodel/muzzle", "weapon/muzzle_flash", "weapon-origin", ...
	std::string weaponClass; // entity class of the held weapon (context only)
	std::string vmDetail;    // why/how the viewmodel entity resolved (diagnostic)
};

MuzzleRef ResolveMuzzle() {
	MuzzleRef out;
	// Spectated pawn first; in a locally-recorded demo the shooter IS the local viewer
	// (no observer target at all -- live run 2026-07-04: 755/944 samples dropped
	// "no-pawn" before this fallback), the same fallback order cosmetics uses.
	const int pawnIdx = AfxGetSpectatedPawnIndex();
	CEntityInstance* pawn = pawnIdx >= 0 ? EntityAt(pawnIdx) : nullptr;
	if (!pawn || !pawn->IsPlayerPawn())
		pawn = AfxGetLocalViewerPawn();
	if (!pawn || !pawn->IsPlayerPawn()) {
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("fx.align", "no-pawn: spectatedIdx=%d spectatedEnt=%d localPawn=%d",
				pawnIdx, pawnIdx >= 0 && EntityAt(pawnIdx) ? 1 : 0, pawn ? 1 : 0);
		return out;
	}
	float eye[3] = {};
	pawn->GetRenderEyeOrigin(eye);
	out.eye = FollowVec3{ eye[0], eye[1], eye[2] };

	int wi = -1;
	{
		const auto h = pawn->GetActiveWeaponHandle();
		if (h.IsValid())
			wi = h.GetEntryIndex();
	}
	CEntityInstance* weapon = wi >= 0 ? EntityAt(wi) : nullptr;
	out.weaponClass = EntityClass(weapon);

	// The Modern assets are m_bViewModelEffect: the FIRST-PERSON viewmodel weapon entity (the
	// weapon-like child of the pawn's HudModelArms) is the reference that matches what is on
	// screen. ReadActiveViewmodelWeaponState returned -1 on every demo POV pawn (its class/paint
	// match fails), so 0/997 samples ever measured against the viewmodel muzzle -- every FP effect
	// was scored against the WORLD muzzle (~5-17u off). ResolveViewmodelWeaponEntityIndex resolves
	// the same arms child WITHOUT the class match. World weapon stays the fallback.
	CEntityInstance* vm = nullptr;
	int vmIndex = -1;
	{
		char dbg[192] = {};
		void* vmEnt = nullptr;
		vmIndex = ResolveViewmodelWeaponEntityIndex((unsigned char*)pawn,
			out.weaponClass.empty() ? nullptr : out.weaponClass.c_str(), dbg, sizeof(dbg), &vmEnt);
		vm = (CEntityInstance*)vmEnt; // client-only entity: use the pointer, NOT EntityAt(vmIndex)
		char d[288];
		std::snprintf(d, sizeof(d), "wclass=%s worldWi=%d vmIdx=%d vmEnt=%d %s",
			out.weaponClass.empty() ? "-" : out.weaponClass.c_str(), wi, vmIndex, vm ? 1 : 0, dbg);
		out.vmDetail = d;
	}

	const struct { CEntityInstance* ent; int idx; const char* tag; } cands[] = {
		{ vm, vmIndex, "viewmodel" },
		{ weapon, wi, "weapon" },
	};
	const char* attNames[] = { "muzzle", "muzzle_flash" };
	for (const auto& c : cands) {
		if (!c.ent)
			continue;
		for (const char* att : attNames) {
			FollowVec3 apos;
			FollowAngles aang;
			if (SehReadAttachment(c.ent, att, apos, aang)) {
				out.valid = true;
				out.attachment = true;
				out.pos = apos;
				out.ang = aang;
				out.source = std::string(c.tag) + "/" + att;
				return out;
			}
		}
	}
	// No muzzle attachment on either entity: weapon origin oriented by the pawn's eyes
	// (low confidence -- flagged via source + attachment=false).
	if (weapon) {
		FollowVec3 origin;
		if (ReadOrigin(weapon, origin)) {
			float a[3] = {};
			pawn->GetRenderEyeAngles(a);
			out.valid = true;
			out.pos = origin;
			out.ang = FollowAngles{ a[0], a[1], a[2] };
			out.source = "weapon-origin";
			return out;
		}
	}
	float a[3] = {};
	pawn->GetRenderEyeAngles(a);
	out.valid = true;
	out.pos = out.eye;
	out.ang = FollowAngles{ a[0], a[1], a[2] };
	out.source = "pawn-eye";
	return out;
}

// ============================== spawn point =========================================

// Scan the created collection's first kScanBytes for the float3 nearest the muzzle
// (control points / emitter origins live in that header region). Returns true and fills
// outSpawn on a hit within kScanRangeUnits. Freed/garbage memory degrades to "no hit"
// via the SEH copy + the range filter, never a crash.
bool ScanInstanceForSpawn(void* instance, const FollowVec3& muzzle, FollowVec3& outSpawn) {
	if (!instance)
		return false;
	unsigned char buf[kScanBytes];
	size_t got = 0;
	// A partial page fault fails the whole memcpy; fall back to smaller windows.
	for (size_t want : { kScanBytes, (size_t)0x400, (size_t)0x100 }) {
		if (SehCopyBytes(instance, buf, want)) {
			got = want;
			break;
		}
	}
	if (got < 12)
		return false;
	double bestDist = kScanRangeUnits;
	bool found = false;
	for (size_t off = 0; off + 12 <= got; off += 4) {
		float v[3];
		std::memcpy(v, buf + off, 12);
		if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
			continue;
		if (std::fabs(v[0]) > 100000.f || std::fabs(v[1]) > 100000.f || std::fabs(v[2]) > 100000.f)
			continue;
		const FollowVec3 p{ v[0], v[1], v[2] };
		const double d = FollowDistance(p, muzzle);
		if (d < bestDist) {
			bestDist = d;
			outSpawn = p;
			found = true;
		}
	}
	return found;
}

// ============================== NDJSON sink =========================================

std::wstring AlignLogPath() {
	std::wstring path = GetHlaeRoamingAppDataFolderW();
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	path += L"fx_align.jsonl";
	return path;
}

void AppendJsonEscaped(std::string& out, const char* s) {
	for (; s && *s; ++s) {
		const char c = *s;
		if (c == '\\' || c == '"')
			out.push_back('\\');
		out.push_back(c);
	}
}

void WriteSampleLine(const char* wclass, const char* effect, const PendingSample& p,
	const MuzzleRef& m, const FollowVec3& spawn, double dist, const FollowVec3& local,
	const char* method, const char* pov, bool pass) {
	std::ofstream f{ std::filesystem::path(AlignLogPath()), std::ios::app };
	if (!f)
		return;
	std::string raw, target, source, vmDetail;
	AppendJsonEscaped(raw, p.raw.c_str());
	AppendJsonEscaped(target, p.target.c_str());
	AppendJsonEscaped(source, m.source.c_str());
	AppendJsonEscaped(vmDetail, m.vmDetail.c_str());
	char buf[1152];
	std::snprintf(buf, sizeof(buf),
		"{\"weapon_class\":\"%s\",\"effect\":\"%s\",\"raw\":\"%s\",\"target\":\"%s\","
		"\"demo_tick\":%d,\"muzzle_world\":[%.2f,%.2f,%.2f],\"spawn_world\":[%.2f,%.2f,%.2f],"
		"\"distance_units\":%.3f,\"local_offset\":[%.3f,%.3f,%.3f],"
		"\"config_offset\":[%.2f,%.2f,%.2f],\"method\":\"%s\",\"muzzle_source\":\"%s\","
		"\"muzzle_attachment\":%s,\"pov\":\"%s\",\"vm_debug\":\"%s\",\"pass\":%s}\n",
		wclass, effect, raw.c_str(), target.c_str(), p.demoTick,
		m.pos.x, m.pos.y, m.pos.z, spawn.x, spawn.y, spawn.z,
		dist, local.x, local.y, local.z,
		kModernCfgOffset[0], kModernCfgOffset[1], kModernCfgOffset[2],
		method, source.c_str(), m.attachment ? "true" : "false", pov, vmDetail.c_str(),
		pass ? "true" : "false");
	f << buf;
}

// ============================== per-sample processing ===============================

void ProcessSample(const PendingSample& p) {
	const char* effect = EffectKindFor(p.target.c_str());
	if (!effect)
		return;
	// Spectated-player only. First-person raws (_fps/_fp) are always theirs; THIRD-person
	// raws are accepted only inside their weapon_fire window (the FxDebugHud gate) --
	// other players' shots elsewhere in the demo would otherwise flood the metric with
	// distances to the wrong player's muzzle (live run 2026-07-04). Barrel smoke raws
	// (weapon_muzzle_smoke*) are classless AND markerless, so they are additionally
	// gated around the spectated player's own last accepted flash below.
	const bool isSmoke = 0 == std::strcmp(effect, "barrelsmoke");
	const bool fp = IsFpPath(p.raw.c_str());
	if (!isSmoke && !fp && !ParticleFx_WatchedFireWindow(p.demoTick)) {
		std::lock_guard<std::mutex> lock(g_mx);
		++g_droppedGate;
		return;
	}
	const char* wclass = WeaponClassFromRaw(p.raw.c_str());
	{
		std::lock_guard<std::mutex> lock(g_mx);
		if (wclass && !isSmoke) {
			g_lastFlashClass = wclass;
			g_lastFlashTick = p.demoTick;
		} else if (!wclass) {
			const bool inWindow = !g_lastFlashClass.empty() && p.demoTick >= 0
				&& p.demoTick - g_lastFlashTick >= -kFlashClassWindowTicks
				&& p.demoTick - g_lastFlashTick <= kFlashClassWindowTicks;
			if (isSmoke && !inWindow)
				return; // can't attribute this smoke to the POV player: drop it
			if (inWindow)
				wclass = g_lastFlashClass.c_str();
		}
	}
	// g_lastFlashClass is only mutated on this (main) thread; the c_str stays valid here.
	std::string wclassCopy = wclass ? wclass : "unknown";

	MuzzleRef m = ResolveMuzzle();
	if (!m.valid) {
		std::lock_guard<std::mutex> lock(g_mx);
		++g_droppedNoPawn; // no spectated pawn (menu, freecam over nothing)
		return;
	}

	FollowVec3 spawn;
	const char* method;
	if (ScanInstanceForSpawn(p.instance, m.pos, spawn)) {
		method = "cp-scan";
	} else {
		// Fallback: the authored vpcf offset rotated into world space. This measures the
		// CONFIGURED offset only (it cannot see a wrong engine attachment binding).
		const FollowVec3 cfg{ kModernCfgOffset[0], kModernCfgOffset[1], kModernCfgOffset[2] };
		const FollowVec3 world = FollowRotateVector(cfg, m.ang);
		spawn = FollowVec3{ m.pos.x + world.x, m.pos.y + world.y, m.pos.z + world.z };
		method = "config-offset";
	}
	const double dist = FollowDistance(spawn, m.pos);
	const FollowVec3 delta{ spawn.x - m.pos.x, spawn.y - m.pos.y, spawn.z - m.pos.z };
	const FollowVec3 local = FollowInverseRotateVector(delta, m.ang);

	bool pass;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		pass = dist <= g_threshold;
		Agg& a = g_aggs[wclassCopy + "|" + effect];
		++a.count;
		if (pass)
			++a.pass;
		if (0 == std::strcmp(method, "cp-scan"))
			++a.cpScan;
		a.sumDist += dist;
		if (dist > a.maxDist)
			a.maxDist = dist;
		++g_samples;
		if (0 == std::strcmp(method, "cp-scan"))
			++g_cpScanSamples;
		if (m.attachment)
			++g_attachSamples;
	}
	WriteSampleLine(wclassCopy.c_str(), effect, p, m, spawn, dist, local, method,
		fp ? "fp" : "world", pass);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("fx.align", "%s %s d=%.2fu (%s, %s, %s) %s", wclassCopy.c_str(), effect,
			dist, method, m.source.c_str(), fp ? "fp" : "world", pass ? "pass" : "FAIL");
	if (0 == std::strcmp(effect, "muzzleflash") && HasPerShotBarrelSmokeChild(p.target.c_str())) {
		RecordDerivedAlignSample(wclassCopy, "barrelsmoke", dist, pass, method, m.attachment);
	}
}

void PrintStatus(const char* cmd) {
	int samples, cpScan, attach;
	double threshold;
	size_t pending;
	unsigned long long dropped;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		samples = g_samples;
		cpScan = g_cpScanSamples;
		attach = g_attachSamples;
		threshold = g_threshold;
		pending = g_pending.size();
		dropped = g_droppedPending;
	}
	unsigned long long droppedGate, droppedNoPawn;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		droppedGate = g_droppedGate;
		droppedNoPawn = g_droppedNoPawn;
	}
	advancedfx::Message(
		"fx align: %s. %d sample(s) (%d cp-scan / %d config-offset; %d on a real muzzle "
		"attachment), threshold %.2f units, %zu pending, %llu dropped (queue), "
		"%llu gated (not watched player), %llu no-pawn.\n"
		"  %s fx align on|off - measure Modern muzzle FX spawn distance (Source units).\n"
		"  %s fx align report - per weapon-class/effect table.  clear - reset + truncate log.\n"
		"  %s fx align threshold <units> - pass distance (default 2.5).\n"
		"  Samples: %%APPDATA%%\\HLAE\\fx_align.jsonl\n",
		FxAlign_Enabled() ? "ON" : "off", samples, cpScan, samples - cpScan, attach,
		threshold, pending, dropped, droppedGate, droppedNoPawn, cmd, cmd, cmd);
}

// One-shot evidence dump: where does the WORLD weapon muzzle sit vs the resolved FIRST-PERSON
// viewmodel weapon muzzle for the currently spectated player? Lets me confirm the viewmodel
// resolution works (and by how much the two differ) without waiting for a fired shot.
void DumpVmProbe() {
	const int pawnIdx = AfxGetSpectatedPawnIndex();
	CEntityInstance* pawn = pawnIdx >= 0 ? EntityAt(pawnIdx) : nullptr;
	if (!pawn || !pawn->IsPlayerPawn())
		pawn = AfxGetLocalViewerPawn();
	if (!pawn || !pawn->IsPlayerPawn()) {
		advancedfx::Message("fx align vmprobe: no spectated/local player pawn.\n");
		return;
	}
	float eye[3] = {};
	pawn->GetRenderEyeOrigin(eye);
	const FollowVec3 eyeV{ eye[0], eye[1], eye[2] };
	int wi = -1;
	{ const auto h = pawn->GetActiveWeaponHandle(); if (h.IsValid()) wi = h.GetEntryIndex(); }
	CEntityInstance* weapon = wi >= 0 ? EntityAt(wi) : nullptr;
	std::string wclass = EntityClass(weapon);

	auto reportAtt = [&](const char* tag, CEntityInstance* ent, int idx) -> bool {
		if (!ent) { advancedfx::Message("  %-10s: <none>\n", tag); return false; }
		for (const char* att : { "muzzle_flash", "muzzle" }) {
			FollowVec3 apos;
			FollowAngles aang;
			if (SehReadAttachment(ent, att, apos, aang)) {
				advancedfx::Message("  %-10s idx=%d cls=%s att=%s pos=(%.1f,%.1f,%.1f) eyeDist=%.1fu\n",
					tag, idx, EntityClass(ent).c_str(), att, apos.x, apos.y, apos.z,
					FollowDistance(apos, eyeV));
				return true;
			}
		}
		advancedfx::Message("  %-10s idx=%d cls=%s att=<no muzzle attachment>\n",
			tag, idx, EntityClass(ent).c_str());
		return false;
	};

	char dbg[192] = {};
	void* vmEnt = nullptr;
	const int vmIdx = ResolveViewmodelWeaponEntityIndex((unsigned char*)pawn,
		wclass.empty() ? nullptr : wclass.c_str(), dbg, sizeof(dbg), &vmEnt);
	CEntityInstance* vm = (CEntityInstance*)vmEnt;

	advancedfx::Message("fx align vmprobe: pawn=%d eye=(%.1f,%.1f,%.1f) wclass=%s\n  vm-resolve: %s\n",
		pawnIdx, eye[0], eye[1], eye[2], wclass.empty() ? "-" : wclass.c_str(), dbg);
	reportAtt("world", weapon, wi);
	const bool vmMuzzle = reportAtt("viewmodel", vm, vmIdx);
	if (!vmMuzzle && !vm)
		advancedfx::Message("  (no viewmodel weapon entity resolved -- FP effects will score vs the WORLD muzzle)\n");
	if (!vmMuzzle && vm) {
		// The viewmodel entity resolved but has no muzzle/muzzle_flash point under those names --
		// enumerate what it DOES expose so we know the right attachment name.
		std::vector<FollowAttachPoint> pts = AvailableAttachPoints(vm, FollowTargetType::Weapon, true, false);
		std::string ids;
		for (const auto& p : pts) { if (!ids.empty()) ids += ","; ids += p.id; if (p.valid) ids += "*"; }
		advancedfx::Message("  viewmodel attach points: [%s]\n", ids.empty() ? "(none)" : ids.c_str());
	}

	MuzzleRef m = ResolveMuzzle();
	advancedfx::Message("  ResolveMuzzle -> source=%s pos=(%.1f,%.1f,%.1f) attachment=%s\n",
		m.source.c_str(), m.pos.x, m.pos.y, m.pos.z, m.attachment ? "true" : "false");
}

} // namespace

// ============================== public API ==========================================

bool FxAlign_Enabled() {
	return g_enabled.load(std::memory_order_relaxed);
}

void FxAlign_OnSwapCreate(const char* rawLow, const char* targetLow, void* instance, int demoTick) {
	if (!rawLow || !targetLow)
		return;
	// Cheap pre-filter on the hook thread; full classification happens on the pump.
	if (!EffectKindFor(targetLow))
		return;
	std::lock_guard<std::mutex> lock(g_mx);
	if (g_pending.size() >= kPendingCap) {
		++g_droppedPending;
		return;
	}
	PendingSample p;
	p.raw = rawLow;
	p.target = targetLow;
	p.instance = instance;
	p.demoTick = demoTick;
	p.wallMs = GetTickCount64();
	g_pending.push_back(std::move(p));
}

void FxAlign_PumpMainThread() {
	if (!FxAlign_Enabled())
		return;
	// Give the engine's caller one beat to set the new collection's control points, but
	// don't let flash-lifetime instances go stale: process anything >= 10ms old.
	const unsigned long long now = GetTickCount64();
	std::vector<PendingSample> ready;
	{
		std::lock_guard<std::mutex> lock(g_mx);
		for (size_t i = 0; i < g_pending.size();) {
			if (now - g_pending[i].wallMs >= 10) {
				ready.push_back(std::move(g_pending[i]));
				g_pending.erase(g_pending.begin() + i);
				if (ready.size() >= 16)
					break; // rate-cap per frame
			} else {
				++i;
			}
		}
	}
	for (const PendingSample& p : ready)
		ProcessSample(p);
}

void FxAlign_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = (argc >= 4) ? args->ArgV(3) : "";
	if (0 == _stricmp(sub, "on") || 0 == _stricmp(sub, "off")) {
		const bool on = 0 == _stricmp(sub, "on");
		g_enabled.store(on, std::memory_order_relaxed);
		if (on)
			ParticleFxRef().EnsureInstalled();
		advancedfx::Message("fx align: %s.%s\n", on ? "ON" : "off",
			on ? " Measuring Modern muzzle FX spawn distance; samples -> %APPDATA%\\HLAE\\fx_align.jsonl" : "");
		return;
	}
	if (0 == _stricmp(sub, "clear")) {
		{
			std::lock_guard<std::mutex> lock(g_mx);
			g_pending.clear();
			g_aggs.clear();
			g_samples = 0;
			g_cpScanSamples = 0;
			g_attachSamples = 0;
			g_droppedPending = 0;
			g_droppedGate = 0;
			g_droppedNoPawn = 0;
			g_lastFlashClass.clear();
			g_lastFlashTick = -0x40000000;
		}
		std::ofstream f{ std::filesystem::path(AlignLogPath()), std::ios::trunc };
		advancedfx::Message("fx align: cleared (aggregates + fx_align.jsonl).\n");
		return;
	}
	if (0 == _stricmp(sub, "threshold")) {
		if (argc < 5) {
			std::lock_guard<std::mutex> lock(g_mx);
			advancedfx::Message("fx align: threshold is %.2f units.\n", g_threshold);
			return;
		}
		const double t = atof(args->ArgV(4));
		if (t <= 0.0) {
			advancedfx::Warning("fx align: threshold must be > 0.\n");
			return;
		}
		std::lock_guard<std::mutex> lock(g_mx);
		g_threshold = t;
		advancedfx::Message("fx align: threshold = %.2f units (affects new samples).\n", t);
		return;
	}
	if (0 == _stricmp(sub, "vmprobe")) {
		DumpVmProbe();
		return;
	}
	if (0 == _stricmp(sub, "report")) {
		std::lock_guard<std::mutex> lock(g_mx);
		advancedfx::Message("fx align: %d sample(s), threshold %.2f units, %d cp-scan / %d "
			"config-offset, %d on a real muzzle attachment.\n",
			g_samples, g_threshold, g_cpScanSamples, g_samples - g_cpScanSamples,
			g_attachSamples);
		advancedfx::Message("  %-14s %-12s %5s %8s %8s %6s %7s\n",
			"class", "effect", "n", "mean_u", "max_u", "scan", "pass");
		bool anyFail = false;
		for (const auto& [key, a] : g_aggs) {
			const size_t bar = key.find('|');
			const std::string cls = key.substr(0, bar);
			const std::string eff = key.substr(bar + 1);
			const bool ok = a.pass == a.count;
			if (!ok)
				anyFail = true;
			advancedfx::Message("  %-14s %-12s %5d %8.2f %8.2f %6d %4d/%-4d%s\n",
				cls.c_str(), eff.c_str(), a.count, a.count ? a.sumDist / a.count : 0.0,
				a.maxDist, a.cpScan, a.pass, a.count, ok ? "" : "  <-- FAIL");
		}
		advancedfx::Message("fx align: RESULT %s\n", g_aggs.empty() ? "NO-SAMPLES" : (anyFail ? "FAIL" : "PASS"));
		return;
	}
	PrintStatus(cmd);
}

} // namespace Filmmaker
