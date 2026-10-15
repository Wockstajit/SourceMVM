#include "SmokeFx.h"
#include "SmokeAppearance.h"

#include "WorldSchema.h"
#include "WorldEntities.h"

#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"
#include "../../MirvColors.h"   // the colour state this mirrors (single source of truth)
#include "../../MirvTime.h"     // GetCurrentDemoTick, for the re-trigger
#include "../../../Shared/AfxConsole.h"

#include <Windows.h> // GetTickCount64 (probe throttle)
#include <cstring>

namespace Filmmaker {

namespace {

// Measured 2026-08-04: a smoke projectile reports GetClassName() == "smokegrenade_projectile"
// and GetClientClassName() == "C_BaseGrenade". Both spellings listed for the same reason as
// everywhere else in this module -- see WorldEntities.h.
const char* const kSmokeClasses[] = {
	"smokegrenade_projectile",
	"C_SmokeGrenadeProjectile",
};

bool g_live = true;
int  g_projectileCount = 0;

// Per-entity-slot throttle for the diagnostic probe below. Indexed by (entity index & 0xFF),
// which is plenty: only a handful of smokes exist at once and a collision just means one of
// them logs slightly less often.
unsigned long long g_lastProbeMs[256] = { 0 };

// Writes three floats. Kept free of C++ objects so __try is legal (MSVC restriction).
bool WriteColorRaw(unsigned char* base, std::ptrdiff_t off, float r, float g, float b) {
	__try {
		float* v = (float*)(base + off);
		v[0] = r; v[1] = g; v[2] = b;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return true;
}

bool ReadColorRaw(unsigned char* base, std::ptrdiff_t off, float* r, float* g, float* b) {
	__try {
		const float* v = (const float*)(base + off);
		*r = v[0]; *g = v[1]; *b = v[2];
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return true;
}

// Reads any int field defensively (used for the detonation/lifecycle fields below).
bool ReadIntRaw(unsigned char* base, std::ptrdiff_t off, int* out) {
	__try {
		*out = *(const int*)(base + off);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return true;
}

bool WriteIntRaw(unsigned char* base, std::ptrdiff_t off, int value) {
	__try {
		*(int*)(base + off) = value;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return true;
}

// --------------------------------------------------------------------------------------------
// VOXEL VOLUME RE-TINT
//
// The problem this solves (reported and reproduced): a smoke set to red renders red while it is
// dense, then washes back towards the game's own grey as it thins out and expires -- instead of
// staying red and simply getting fainter.
//
// WHY. The mvm_debug probe above proves m_vSmokeColor keeps our value for the whole lifetime
// (`pre=(255.0,0.0,0.0)` on every sample, never reverted). So the entity field is not the
// problem. CS2 smoke renders from a 32x32x32 RGBA voxel volume hanging off the projectile --
// each voxel carries its OWN rgb + alpha (see reference/coding help/smoke/745091-smoke-voxels).
// The tint is baked into those voxels when they are generated; voxels regenerated during the
// dissipation phase come back with the stock colour.
//
// THE FIX. Re-write the RGB of every occupied voxel each frame and LEAVE ALPHA ALONE. Alpha is
// the density, so the smoke still thins and fades exactly as it should -- it just fades as a
// dimmer red rather than reverting to grey. That is precisely the requested behaviour.
//
// THE RISK, AND WHAT IT ALREADY COST. This is the only code here that writes a buffer the schema
// cannot locate for us. The first version assumed the volume was exactly 32^3*4 bytes and wrote
// that much -- and CRASHED CS2 (dumps 2026-08-04 11:05 / 11:07). Note that a __try guard does
// NOT protect against this: overrunning a smaller heap allocation lands on other perfectly valid
// committed memory, so nothing faults; it corrupts the heap and the game dies later somewhere
// unrelated. Four guards now, and it is OPT-IN:
//   1. every pointer bounds-checked (non-null, above 0x10000),
//   2. the frame index sanity-checked,
//   3. the volume's own `center` must match the schema-resolved m_vSmokeDetonationPos,
//   4. VirtualQuery must confirm the FULL volume fits in one writable committed region.
// Any failure means we do not write at all. Fails CLOSED: stock smoke behaviour.
//
// Layout INSIDE the volume struct, expressed RELATIVE TO ITS CENTRE FIELD. The reference gives
// these as absolute entity offsets (wrap 0x14A8 + state 0x70 / rgba 0xE0 / centre 0xE8 / frame
// 0x100); measured 2026-08-04 those absolute offsets are already WRONG on the current build --
// the chain validated as INVALID and correctly refused to write.
//
// So rather than re-hardcoding a number that will rot again in a month, the centre is FOUND at
// runtime by scanning the entity for a vector matching the schema-resolved detonation position,
// and everything else is read relative to it. The struct's internal layout is far more stable
// than its offset within the entity, so this should survive updates that would break a
// hardcoded chain -- and if it ever does not, it still fails closed.
constexpr std::ptrdiff_t kVoxRgbaFromCenter  = -0x8;  // 0xE0 - 0xE8
constexpr std::ptrdiff_t kVoxStateFromCenter = -0x78; // 0x70 - 0xE8
constexpr std::ptrdiff_t kVoxFrameFromCenter = +0x18; // 0x100 - 0xE8

// Where to look for the centre field. The old build put it at 0x14A8+0xE8 = 0x1590; the window
// is generous around that and 4-byte aligned.
constexpr std::ptrdiff_t kVoxScanBegin = 0x1200;
constexpr std::ptrdiff_t kVoxScanEnd   = 0x2600;

constexpr int    kVoxDim   = 32;
constexpr size_t kVoxCount = (size_t)kVoxDim * kVoxDim * kVoxDim; // 32768
constexpr size_t kVoxBytes = kVoxCount * 4;                        // 131072

// DEFAULT OFF, DELIBERATELY. This is the only code in the World module that writes a raw,
// non-schema-located buffer, and an early version CRASHED CS2 (dumps at 2026-08-04 11:05/11:07):
// it assumed the volume was exactly 32^3*4 = 131072 bytes and wrote that much, which corrupts
// the heap whenever the real allocation is smaller. The write is now bounded by an actual
// VirtualQuery of the containing region and refuses to run unless the full volume genuinely
// fits -- but it stays opt-in until it has been proven over a full smoke lifetime.
//   mirv_filmmaker world smoke voxel on
bool g_voxelTint = false;
int  g_voxelTinted = 0;      // volumes re-tinted on the last frame
bool g_voxelChainOk = false; // last validation result
bool g_voxelChainTested = false;
std::ptrdiff_t g_voxCenterOff = 0; // discovered once, then reused

size_t WritableBytesAt(const unsigned char* p); // defined below, used by the checked variant

// Validates a candidate centre offset and returns the rgba buffer. POD-only so __try is legal.
unsigned char* TryVoxelCenter(unsigned char* ent, std::ptrdiff_t centerOff,
	float detX, float detY, float detZ) {
	__try {
		unsigned char* c = ent + centerOff;
		const float cx = *(const float*)(c + 0);
		const float cy = *(const float*)(c + 4);
		const float cz = *(const float*)(c + 8);
		if (cx == 0.0f && cy == 0.0f && cz == 0.0f) return nullptr;
		const float dx = cx - detX, dy = cy - detY, dz = cz - detZ;
		if ((dx * dx + dy * dy + dz * dz) > (96.0f * 96.0f)) return nullptr;

		void* state = *(void**)(c + kVoxStateFromCenter);
		unsigned char* rgba = *(unsigned char**)(c + kVoxRgbaFromCenter);
		const int frame = *(const int*)(c + kVoxFrameFromCenter);
		if (!state || !rgba) return nullptr;
		if ((unsigned long long)state < 0x10000ull) return nullptr;
		if ((unsigned long long)rgba < 0x10000ull) return nullptr;
		if (frame < 0 || frame > 1024) return nullptr;

		return rgba;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// Wraps the candidate check with the region-size test, so the SCAN can never latch onto a
// plausible-looking centre whose buffer is too small to be the real volume.
unsigned char* TryVoxelCenterChecked(unsigned char* ent, std::ptrdiff_t centerOff,
	float detX, float detY, float detZ) {
	unsigned char* rgba = TryVoxelCenter(ent, centerOff, detX, detY, detZ);
	if (!rgba) return nullptr;
	if (WritableBytesAt(rgba) < kVoxBytes) return nullptr;
	return rgba;
}

// Finds the volume centre by scanning for the detonation position. Result cached in
// g_voxCenterOff, so the scan runs once per session rather than per frame.
unsigned char* ResolveVoxelRgba(unsigned char* ent, float detX, float detY, float detZ) {
	if (0 != g_voxCenterOff) {
		unsigned char* rgba = TryVoxelCenterChecked(ent, g_voxCenterOff, detX, detY, detZ);
		if (rgba) return rgba;
		// Cached offset stopped working (different entity layout / bad cache) -- rescan.
		g_voxCenterOff = 0;
	}

	const std::ptrdiff_t skip = WorldSchema_Offset(kWF_Smoke_DetonationPos);
	for (std::ptrdiff_t off = kVoxScanBegin; off < kVoxScanEnd; off += 4) {
		if (off == skip) continue; // that is the schema field itself, not the volume's copy
		unsigned char* rgba = TryVoxelCenterChecked(ent, off, detX, detY, detZ);
		if (rgba) {
			g_voxCenterOff = off;
			MvmDebugLog_Linef("world.smoke", "voxel centre found at +0x%zX (rgba +0x%zX)",
				(size_t)off, (size_t)(off + kVoxRgbaFromCenter));
			return rgba;
		}
	}
	return nullptr;
}

// How many bytes are genuinely writable starting at `p`, according to the OS.
//
// THIS IS THE FIX FOR THE CRASH. The previous version assumed the volume was exactly
// 32^3 * 4 bytes and wrote that much. A __try guard does NOT save you there: writing past the
// end of a smaller heap allocation usually lands on other VALID committed memory, so nothing
// faults -- it silently corrupts the heap and CS2 dies later somewhere unrelated. Reading the
// real region size is the only way to make this safe.
size_t WritableBytesAt(const unsigned char* p) {
	MEMORY_BASIC_INFORMATION mbi;
	if (0 == VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return 0;
	if (mbi.State != MEM_COMMIT) return 0;
	const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
		| PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	if (0 == (mbi.Protect & writable)) return 0;
	if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;

	const unsigned char* regionEnd =
		(const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
	if (regionEnd <= p) return 0;
	return (size_t)(regionEnd - p);
}

// DISABLED. Kept for the research record; see SmokeFx.h and Docs/effects/smoke.md.
//
// This crashed CS2 TWICE (dumps 2026-08-04 11:05, 11:07, then again at 11:24 *after* the
// "fix"). The second attempt bounded the write with VirtualQuery, and that reasoning was
// simply WRONG: a committed region is typically a whole heap segment holding many separate
// allocations, so "this region has 128KB left" says nothing about whether THIS allocation is
// 128KB. The write kept stomping neighbouring allocations, just more confidently.
//
// The honest position: the real size and ownership of this buffer are not known, and there is
// no safe way to write it without knowing them. Guessing has cost two crashes. Re-enable only
// after the volume's true extent has been established (its allocation size, not its region's),
// e.g. by reversing the code that allocates it rather than by inferring from the voxel-count
// assumption in reference/coding help/smoke/745091.
constexpr bool kVoxelWriteProven = false;

bool TintVoxels(unsigned char* rgba, unsigned char r, unsigned char g, unsigned char b) {
	if (!kVoxelWriteProven) return false;
	if (WritableBytesAt(rgba) < kVoxBytes) return false;
	__try {
		for (size_t i = 0; i < kVoxCount; ++i) {
			unsigned char* v = rgba + i * 4;
			if (0 == v[3]) continue; // empty voxel -- leave it empty
			v[0] = r; v[1] = g; v[2] = b;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return true;
}

bool ReadVec3Raw(unsigned char* base, std::ptrdiff_t off, float* x, float* y, float* z) {
	__try {
		const float* v = (const float*)(base + off);
		*x = v[0]; *y = v[1]; *z = v[2];
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	return true;
}

int TeamOf(CEntityInstance* ent) {
	if (!ent) return 0;
	__try {
		return ent->GetTeam();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

} // namespace

bool SmokeFx_Available(std::string* outReason) {
	if (0 == g_clientDllOffsets.C_SmokeGrenadeProjectile.m_vSmokeColor) {
		if (outReason) *outReason = "missing schema field C_SmokeGrenadeProjectile::m_vSmokeColor";
		return false;
	}
	if (outReason) outReason->clear();
	return true;
}

void SmokeFx_SetLive(bool on) {
	if (g_live == on) return;
	g_live = on;
	MvmDebugLog_Linef("world.smoke", "live=%d", on ? 1 : 0);
}

bool SmokeFx_Live() { return g_live; }
int  SmokeFx_ProjectileCount() { return g_projectileCount; }

void SmokeFx_RunFrame() {
	g_projectileCount = 0;
	g_voxelTinted = 0;
	if (!g_live) return;

	const std::ptrdiff_t off = g_clientDllOffsets.C_SmokeGrenadeProjectile.m_vSmokeColor;
	if (0 == off) return;
	if (!g_pEntityList || !g_GetEntityFromIndex) return;

	const char* tName  = MirvColors_GetSmokeColorName(false);
	const char* ctName = MirvColors_GetSmokeColorName(true);
	const bool tOn  = tName  && 0 != _stricmp(tName,  "default");
	const bool ctOn = ctName && 0 != _stricmp(ctName, "default");

	unsigned char tr = 0, tg = 0, tb = 0, cr = 0, cg = 0, cb = 0;
	MirvColors_GetSmokeColorRgb(false, &tr, &tg, &tb);
	MirvColors_GetSmokeColorRgb(true,  &cr, &cg, &cb);

	// AUTO RE-TRIGGER. CS2 bakes a smoke's colour into its voxel volume at spawn, so a colour
	// change is invisible on smoke that is already out unless the volume is restarted. Doing
	// that automatically the moment the colour changes is the only sane behaviour -- making the
	// operator press a second "now actually apply it" button is busywork, and it is exactly the
	// kind of implementation detail leaking into the UI that this panel should hide.
	//
	// This sits BEFORE the "nothing overridden" early-out on purpose: switching a team back to
	// `default` is also a change, and existing smoke has to be restored then too.
	{
		static bool s_init = false;
		static bool s_tOn = false, s_ctOn = false;
		static unsigned char s_t[3] = { 0, 0, 0 }, s_ct[3] = { 0, 0, 0 };
		const bool changed = !s_init
			|| tOn != s_tOn || ctOn != s_ctOn
			|| tr != s_t[0] || tg != s_t[1] || tb != s_t[2]
			|| cr != s_ct[0] || cg != s_ct[1] || cb != s_ct[2];
		if (changed) {
			const bool first = !s_init;
			s_init = true;
			s_tOn = tOn; s_ctOn = ctOn;
			s_t[0] = tr; s_t[1] = tg; s_t[2] = tb;
			s_ct[0] = cr; s_ct[1] = cg; s_ct[2] = cb;
			// Skip the very first frame: that is just this module learning the current state,
			// not the operator changing anything, and re-triggering every smoke on load would
			// restart volumes nobody asked to restart.
			if (!first) SmokeFx_Reapply();
		}
	}

	// Nothing to assert unless at least one team has an override -- otherwise this would fight
	// the game for control of the default colour every frame for no reason.
	if (!tOn && !ctOn) return;

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
		if (!WorldEntity_Matches(ent, kSmokeClasses,
				sizeof(kSmokeClasses) / sizeof(kSmokeClasses[0])))
			continue;
		++g_projectileCount;

		const int team = TeamOf(ent);
		auto base = (unsigned char*)ent;

		// PRE-WRITE READ. This is the whole diagnostic: if what we wrote last frame is still
		// here, our write persists and any wrong colour is a SAMPLING problem (the volume read
		// the value once at spawn). If it has reverted, something -- the demo network update or
		// the game's own smoke-props code -- is overwriting us and we are losing a race.
		float pr = -1.f, pg = -1.f, pb = -1.f;
		const bool readOk = ReadColorRaw(base, off, &pr, &pg, &pb);

		// Same 0-255 scale MirvColors' creation-time hook writes, so the two paths cannot
		// disagree about what "red" means.
		bool wrote = false;
		const bool useCt = (team == 3);
		const bool teamOn = useCt ? ctOn : tOn;
		const unsigned char wr = useCt ? cr : tr;
		const unsigned char wg = useCt ? cg : tg;
		const unsigned char wb = useCt ? cb : tb;
		if (teamOn)
			wrote = WriteColorRaw(base, off, (float)wr, (float)wg, (float)wb);

		// Keep the voxel volume tinted for the whole lifetime, so the smoke fades as a dimmer
		// version of the chosen colour instead of washing back to stock grey. See the block at
		// the top of this file for why the entity field alone is not enough.
		if (g_voxelTint && teamOn) {
			const std::ptrdiff_t offDet = WorldSchema_Offset(kWF_Smoke_DetonationPos);
			if (0 != offDet) {
				float dx = 0.f, dy = 0.f, dz = 0.f;
				if (ReadVec3Raw(base, offDet, &dx, &dy, &dz)) {
					unsigned char* rgba = ResolveVoxelRgba(base, dx, dy, dz);
					g_voxelChainTested = true;
					g_voxelChainOk = (rgba != nullptr);
					if (rgba && TintVoxels(rgba, wr, wg, wb)) ++g_voxelTinted;
				}
			}
		}

		// Throttled to ~1/sec per entity index so this never floods (each log line flushes).
		if (readOk && MvmDebugLog_Active()) {
			const unsigned long long now = GetTickCount64();
			if (now - g_lastProbeMs[i & 0xFF] >= 1000) {
				g_lastProbeMs[i & 0xFF] = now;
				int tickBegin = -1, didEffect = -1;
				const std::ptrdiff_t offTick = WorldSchema_Offset(kWF_Smoke_EffectTickBegin);
				const std::ptrdiff_t offDid  = WorldSchema_Offset(kWF_Smoke_DidSmokeEffect);
				if (offTick) ReadIntRaw(base, offTick, &tickBegin);
				if (offDid)  ReadIntRaw(base, offDid, &didEffect);
				MvmDebugLog_Linef("world.smoke",
					"idx=%d team=%d pre=(%.1f,%.1f,%.1f) wrote=%d tickBegin=%d did=%d",
					i, team, pr, pg, pb, wrote ? 1 : 0, tickBegin, didEffect & 0xFF);
			}
		}
	}

	static int s_lastCount = -1;
	if (g_projectileCount != s_lastCount) {
		s_lastCount = g_projectileCount;
		MvmDebugLog_Linef("world.smoke", "live rewrite projectiles=%d t=%d ct=%d",
			g_projectileCount, tOn ? 1 : 0, ctOn ? 1 : 0);
	}
}

int SmokeFx_Reapply() {
	const std::ptrdiff_t offColor = g_clientDllOffsets.C_SmokeGrenadeProjectile.m_vSmokeColor;
	const std::ptrdiff_t offTick  = WorldSchema_Offset(kWF_Smoke_EffectTickBegin);
	if (0 == offColor || 0 == offTick) return 0;
	if (!g_pEntityList || !g_GetEntityFromIndex) return 0;

	int tick = 0;
	if (!g_MirvTime.GetCurrentDemoTick(tick)) return 0;

	// Resolve BOTH teams to a concrete colour: the override when set, otherwise the game's own
	// default. Always writing a colour is what makes switching back to `default` actually
	// restore deployed smoke -- writing nothing would rebuild the volume still carrying the
	// override we put in the field earlier.
	unsigned char tr = 0, tg = 0, tb = 0, cr = 0, cg = 0, cb = 0;
	if (!MirvColors_GetSmokeColorRgb(false, &tr, &tg, &tb))
		MirvColors_GetSmokeDefaultRgb(false, &tr, &tg, &tb);
	if (!MirvColors_GetSmokeColorRgb(true, &cr, &cg, &cb))
		MirvColors_GetSmokeDefaultRgb(true, &cr, &cg, &cb);

	int retriggered = 0;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, i);
		if (!WorldEntity_Matches(ent, kSmokeClasses,
				sizeof(kSmokeClasses) / sizeof(kSmokeClasses[0])))
			continue;

		auto base = (unsigned char*)ent;
		const int team = TeamOf(ent);

		// Colour FIRST, so the rebuilt volume reads the value we want rather than the old one.
		if (team == 3) WriteColorRaw(base, offColor, (float)cr, (float)cg, (float)cb);
		else           WriteColorRaw(base, offColor, (float)tr, (float)tg, (float)tb);

		if (WriteIntRaw(base, offTick, tick)) ++retriggered;
	}

	MvmDebugLog_Linef("world.smoke", "reapply tick=%d retriggered=%d", tick, retriggered);
	return retriggered;
}

void SmokeFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker world smoke [live on|off]
	const char* sub = (argc >= 4) ? args->ArgV(3) : "";
	if (0 == _stricmp(sub, "appearance")) { SmokeAppearance::RunCommand(argc, args); return; }
	if (0 == _stricmp(sub, "reapply")) {
		const int n = SmokeFx_Reapply();
		if (n > 0)
			advancedfx::Message("mirv_filmmaker: re-triggered %d smoke volume(s) with the current colour.\n", n);
		else
			advancedfx::Message("mirv_filmmaker: no deployed smoke to re-trigger.\n");
		return;
	}
	if (0 == _stricmp(sub, "voxel")) {
		advancedfx::Warning(
			"mirv_filmmaker: smoke voxel re-tint is DISABLED IN CODE and will not turn on.\n"
			"  It crashed CS2 twice (2026-08-04). Writing the voxel volume needs its true\n"
			"  allocation size, which is not known -- VirtualQuery bounds the heap REGION, not\n"
			"  the allocation, so the write was still corrupting neighbouring memory.\n"
			"  See Docs/effects/smoke.md before re-enabling.\n");
		return;
	}
	if (0 == _stricmp(sub, "on") || 0 == _stricmp(sub, "off") || 0 == _stricmp(sub, "toggle")) {
		const bool on = (0 == _stricmp(sub, "toggle")) ? !g_live : (0 == _stricmp(sub, "on"));
		SmokeFx_SetLive(on);
		advancedfx::Message("mirv_filmmaker: smoke live re-write = %s.\n", on ? "on" : "off");
		return;
	}

	std::string reason;
	const bool ok = SmokeFx_Available(&reason);
	advancedfx::Message("mirv_filmmaker world smoke:\n");
	advancedfx::Message("  live re-write : %s\n", g_live ? "on" : "off");
	if (ok) advancedfx::Message("  available     : yes\n");
	else    advancedfx::Message("  available     : NO - %s\n", reason.c_str());
	advancedfx::Message("  projectiles   : %d live smoke grenade(s)\n", g_projectileCount);
	advancedfx::Message("  voxel re-tint : %s (%d volume(s) tinted last frame)\n",
		g_voxelTint ? "on" : "off", g_voxelTinted);
	if (g_voxelChainTested)
		advancedfx::Message("  voxel chain   : %s\n", g_voxelChainOk
			? "VALID (volume centre matches detonation pos)"
			: "INVALID - offsets shifted, re-tint disabled (fails closed)");
	advancedfx::Message("  colours       : T=%s CT=%s (set with `mirv_colors smokes t|ct <colour>`)\n",
		MirvColors_GetSmokeColorName(false), MirvColors_GetSmokeColorName(true));
	advancedfx::Message(
		"Usage:\n"
		"  mirv_filmmaker world smoke reapply      - re-trigger deployed smoke so it takes the current colour\n"
		"  mirv_filmmaker world smoke voxel on|off - keep the voxel volume tinted, so smoke fades as a\n"
		"                                            dimmer version of the colour instead of stock grey\n"
		"  mirv_filmmaker world smoke on|off       - per-frame entity-field colour write\n"
		"MEASURED: the entity colour field is NOT overwritten (probe shows it holding). The wash-back\n"
		"during dissipation comes from the voxel volume, which `voxel` re-tints.\n");
}

} // namespace Filmmaker
