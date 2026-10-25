#include "FilmmakerHealth.h"

#include "Filmmaker.h"                      // PumpStatus, Library
#include "Demo/DemoLibrary.h"               // DemoLibrary::Snapshot / IsScanning
#include "Debug/MvmDebugLog.h"     // MvmDebugLog_*
#include "Debug/GpuPassProfiler.h"  // the hook's own GPU passes, while profiling
#include "Cosmetics/CosmeticOverrideSystem.h"
#include "Movie/ParticleFx.h"
#include "Config/EffectsPersistence.h" // SceneActive: the particle hook waits for a demo
#include "../SchemaSystem.h"                // g_clientDllOffsets, g_cosmeticsOffsetsOk
#include "../RenderSystemDX11Hooks.h"       // RenderHookCost_Sample: the detours' own frame cost
#include "Community/BugReport.h"            // BugReport_Record: the always-on recorder
#include "Community/CrashReport.h"          // CrashReport_NoteFrameTime
#include "Environment/EnvironmentScene.h"   // CurrentMapName
#include "Relight/Relight.h"                // HeartbeatText
#include "Platform/SystemUsage.h"           // machine RAM/CPU, the game's RAM/CPU/GPU
#include "Environment/EnvironmentNativeMaterial.h" // NativeMaterialLiveCost
#include "Render/RenderTargetProbe.h"          // RenderOwnedMemory_*: what the hook holds in VRAM
#include "../ViewportScaler.h"                 // AfxViewportScaler::BreakCounts: scaled-preview breaks
#include "Movie/ScrubFrameCache.h"             // ScrubFrameCache_PrintHealth
#include "AltEnvironment/AltEnvironment.h"     // HealthLine: which map the demo plays on
#include <chrono>
#include <cstdio>
#include "../WrpConsole.h"
#include "../../Shared/AfxConsole.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace Filmmaker {

namespace {

struct OffsetProbe {
	const char* name;
	ptrdiff_t value;
	bool required; // required = gated by g_cosmeticsOffsetsOk; optional = silent when 0
};

// The fields whose disappearance is SILENT. Required ones are already summarised by
// g_cosmeticsOffsetsOk; the optional block is the dangerous half -- a rename there disables a
// feature with no message anywhere, so each is listed individually.
std::vector<OffsetProbe> CollectOffsetProbes() {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	return {
		{ "C_EconItemView::m_iItemDefinitionIndex", o.C_EconItemView.m_iItemDefinitionIndex, true },
		{ "C_EconItemView::m_NetworkedDynamicAttributes", o.C_EconItemView.m_NetworkedDynamicAttributes, true },
		{ "C_EconItemView::m_AttributeList", o.C_EconItemView.m_AttributeList, true },
		{ "C_EconEntity::m_nFallbackPaintKit", o.C_EconEntity.m_nFallbackPaintKit, true },
		{ "C_EconEntity::m_AttributeManager", o.C_EconEntity.m_AttributeManager, true },
		{ "C_CSPlayerPawn::m_EconGloves", o.C_CSPlayerPawn.m_EconGloves, false },
		{ "C_CSPlayerPawn::m_bNeedToReApplyGloves", o.C_CSPlayerPawn.m_bNeedToReApplyGloves, false },
		{ "C_CSPlayerPawn::m_hHudModelArms", o.C_CSPlayerPawn.m_hHudModelArms, false },
		{ "C_CSPlayerPawn::m_flFlashMaxAlpha", o.C_CSPlayerPawn.m_flFlashMaxAlpha, false },
		{ "ModelChain::m_CBodyComponent", o.ModelChain.m_CBodyComponent, false },
		{ "ModelChain::m_skeletonInstance", o.ModelChain.m_skeletonInstance, false },
		{ "ModelChain::m_modelState", o.ModelChain.m_modelState, false },
		{ "ModelChain::m_ModelName", o.ModelChain.m_ModelName, false },
		{ "ModelChain::m_MeshGroupMask", o.ModelChain.m_MeshGroupMask, false },
		{ "C_BaseEntity::m_pGameSceneNode", o.C_BaseEntity.m_pGameSceneNode, false },
		{ "C_BaseEntity::m_nSubclassID", o.C_BaseEntity.m_nSubclassID, false },
		{ "CBasePlayerController::m_steamID", o.CBasePlayerController.m_steamID, false },
		{ "CPlayer_WeaponServices::m_hActiveWeapon", o.CPlayer_WeaponServices.m_hActiveWeapon, false },
	};
}

const char* YesNo(bool v) { return v ? "yes" : "NO"; }

} // namespace

void Filmmaker_RunHealthCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
    if(argc>=3&&0==_stricmp(args->ArgV(2),"resetTiming"))RenderFrameTiming_Reset();
	const bool verbose = (argc >= 3) && (0 == _stricmp(args->ArgV(2), "verbose"));

	uint64_t renderCalls = 0, mainCalls = 0, scans = 0;
	PumpStatus(renderCalls, mainCalls, scans);

	const CosmeticOverrideSystem& cosmetics = CosmeticsRef();
	ParticleFx& fx = ParticleFxRef();
	const bool fxInstalled = fx.Installed();

	const std::vector<OffsetProbe> probes = CollectOffsetProbes();
	int reqTotal = 0, reqOk = 0, optTotal = 0, optOk = 0;
	for (const OffsetProbe& p : probes) {
		if (p.required) { ++reqTotal; if (p.value) ++reqOk; }
		else            { ++optTotal; if (p.value) ++optOk; }
	}

	int problems = 0;

	advancedfx::Message("%s health:\n", cmd);

	// -- render pump: the one silent-catastrophic failure ------------------------------------
	advancedfx::Message("  render pump (Before_Present) : %llu\n", (unsigned long long)renderCalls);
	advancedfx::Message("  main pump                    : %llu\n", (unsigned long long)mainCalls);
	{
		// The hook's own share of the frame. If these are small the frame rate is the game's.
        const auto timing=RenderFrameTiming_Sample();
        advancedfx::Message("  frame timing: samples=%u meanMs=%.3f p50Ms=%.3f p95Ms=%.3f p99Ms=%.3f maxMs=%.3f over33=%u\n",
            timing.samples,timing.meanMs,timing.p50Ms,timing.p95Ms,timing.p99Ms,timing.maxMs,timing.over33);
		const RenderHookCost cost = RenderHookCost_Sample();
		advancedfx::Message("  hook cost sampling           : every %u frames, latest sample %llu ms ago\n", cost.sampleStride, cost.sampleAgeMs);
		advancedfx::Message("  hook cost latest sample      : draw detours %.2f ms over %u draws (native draws %.2f ms), present %.2f ms, environment %.2f ms\n",
			cost.drawHookMs, cost.draws, cost.drawNativeMs, cost.presentHookMs, cost.environmentMs);
		advancedfx::Message("  hook cost ~20-sample average  : draw detours %.2f ms, present %.2f ms, environment %.2f ms\n",
			cost.avgDrawHookMs, cost.avgPresentHookMs, cost.avgEnvironmentMs);
		if (Filmmaker::GpuProfile::Enabled()) Filmmaker::GpuProfile::Print(16);
		Filmmaker::RenderOwnedMemory_Print(8);
		// Scaled workspace preview: every break since launch, by reason (VpScaleBreakMath.h). Each
		// one is a numbered vpscale.break line in the mvm_debug log with the state it started in.
		unsigned vpTotal = 0, vpMissed = 0, vpStale = 0, vpStretched = 0, vpOpen = 0;
		AfxViewportScaler::BreakCounts(vpTotal, vpMissed, vpStale, vpStretched, vpOpen);
		advancedfx::Message("  vp scale breaks              : %u (missed blit %u, stale rect %u, stretched %u)%s\n",
			vpTotal, vpMissed, vpStale, vpStretched, vpOpen ? " -- BROKEN NOW" : "");
		ScrubFrameCache_PrintHealth();
	}
	if (renderCalls == 0) {
		++problems;
		advancedfx::Warning(
			"  ^ RENDER PUMP IS DEAD. Filmmaker::RunFrame() never runs: no demo scans, no FX\n"
			"    precache, no campath drawer. Prime suspect after a CS2 update is the\n"
			"    CRenderContextDx11_SoftwareCommandList::QueueCallback VTABLE INDEX in\n"
			"    RenderSystemDX11Hooks.cpp (it moved 140 -> 142 once). No byte-pattern check\n"
			"    can see this. See Docs/agent/cs2-update-recovery.md.\n");
	}

	// -- schema (names, not addresses) --------------------------------------------------------
	advancedfx::Message("  cosmetics offsets ok         : %s  (required %d/%d, optional %d/%d resolved)\n",
		YesNo(g_cosmeticsOffsetsOk), reqOk, reqTotal, optOk, optTotal);
	if (!g_cosmeticsOffsetsOk) {
		++problems;
		advancedfx::Warning(
			"  ^ A REQUIRED econ/schema field did not resolve BY NAME, so the skin changer is\n"
			"    disabled. Valve renamed a class or field. Dump the live schema from\n"
			"    %%TEMP%%\\hlae_schema_econ.txt (written every launch) and fix the names in\n"
			"    SchemaSystem.cpp::initCosmeticsOffsets.\n");
	}
	if (optOk != optTotal) {
		++problems;
		advancedfx::Warning(
			"  ^ %d OPTIONAL schema field(s) unresolved. These fail SILENTLY at their read\n"
			"    sites (guarded only by != 0), disabling glove/agent/mesh reads with no other\n"
			"    signal. Run \"%s health verbose\" for the list.\n", optTotal - optOk, cmd);
	}

	// -- particle FX hook ---------------------------------------------------------------------
	advancedfx::Message("  particle hook installed      : %s   (fx enabled: %s)\n",
		YesNo(fxInstalled), fx.Enabled() ? "yes" : "no");
	if (fx.Enabled() && !fxInstalled && !EffectsPersistence_SceneActive()) {
		advancedfx::Message("  (the hook arms once a demo or test map loads; effects never run at the main menu)\n");
	} else if (fx.Enabled() && !fxInstalled) {
		++problems;
		advancedfx::Warning(
			"  ^ FX is enabled but the particle hook is NOT installed, so every swap falls\n"
			"    through to vanilla. It fails closed on purpose and names the reason -- look\n"
			"    for the \"fx: particle hook\" warning earlier in the console, or the\n"
			"    fx.install line in the mvm_debug log. Causes seen: ParticleSystemMgr003\n"
			"    interface missing (version bumped), vtable slots out of image (slot order\n"
			"    changed), create-stub shape unrecognized.\n");
	}

	// -- cosmetics runtime --------------------------------------------------------------------
	advancedfx::Message("  cosmetics enabled/armed      : %s / %s   (in demo: %s)\n",
		cosmetics.Enabled() ? "yes" : "no", cosmetics.Armed() ? "yes" : "no",
		cosmetics.InDemoContext() ? "yes" : "no");
	advancedfx::Message("  demos in list                : %zu   (scanning: %s)\n",
		Library().Snapshot().size(), Library().IsScanning() ? "yes" : "no");
	advancedfx::Message("  %s\n", AltEnvironment::HealthLine().c_str());

	if (verbose) {
		advancedfx::Message("  schema offsets:\n");
		for (const OffsetProbe& p : probes) {
			advancedfx::Message("    %-46s %s 0x%llx%s\n", p.name,
				p.required ? "[req]" : "[opt]", (unsigned long long)p.value,
				p.value ? "" : "   <-- UNRESOLVED");
		}
	}

	if (problems == 0) {
		advancedfx::Message("  => healthy: every version-coupled subsystem resolved.\n");
	} else {
		advancedfx::Warning("  => %d problem(s). Runbook: Docs/agent/cs2-update-recovery.md\n"
			"     Offline checks: Automation/verify/verify-cs2-signatures.py (byte patterns)\n"
			"                     Automation/verify/verify-cs2-assets.py     (asset names/defs)\n",
			problems);
	}

	// The console cannot carry this to a netcon client, so mirror the whole verdict into the
	// log as one line (see Docs/agent/mvm-debug-instrumentation.md).
	if (MvmDebugLog_Active()) {
		std::ostringstream o;
		o << "problems=" << problems
			<< " renderPump=" << renderCalls << " mainPump=" << mainCalls << " scans=" << scans
			<< " offsetsOk=" << (g_cosmeticsOffsetsOk ? 1 : 0)
			<< " req=" << reqOk << "/" << reqTotal << " opt=" << optOk << "/" << optTotal
			<< " fxInstalled=" << (fxInstalled ? 1 : 0) << " fxEnabled=" << (fx.Enabled() ? 1 : 0)
			<< " cosmeticsEnabled=" << (cosmetics.Enabled() ? 1 : 0)
			<< " armed=" << (cosmetics.Armed() ? 1 : 0)
			<< " inDemo=" << (cosmetics.InDemoContext() ? 1 : 0)
			<< " demos=" << Library().Snapshot().size();
		for (const OffsetProbe& p : probes)
			if (!p.value)
				o << " UNRESOLVED=" << p.name;
		MvmDebugLog_Linef("health", "%s", o.str().c_str());
	}
}

void FilmmakerHealth_Heartbeat() {
	// Every 5 s is ~70 KB an hour: cheap next to the console lines the recorder already keeps.
	static auto s_next = std::chrono::steady_clock::time_point{};
	const auto now = std::chrono::steady_clock::now();
	if (now < s_next) return;
	s_next = now + std::chrono::seconds(5);

	const RenderFrameTiming timing = RenderFrameTiming_Sample();
	const RenderHookCost cost = RenderHookCost_Sample();
	RenderGpuMemory gpu;
	const bool haveGpu = RenderGpuMemory_Sample(gpu);
	std::string map = Environment::CurrentMapName();
	if (map.empty() || map == "<empty>") map = "(menu)";
	const SystemUsage sys = SystemUsage_Latest();
	// The machine, once (it does not change): which CPU and GPU, how much RAM.
	static bool s_machineLogged = false;
	if (!s_machineLogged && !sys.cpuName.empty() && haveGpu) {
		s_machineLogged = true;
		char machine[512];
		snprintf(machine, sizeof(machine), "machine cpu '%s' (%u threads) | ram %llu MB | gpu '%s' | vram budget %llu MB",
			sys.cpuName.c_str(), sys.logicalCpus, sys.ramTotalMb, gpu.adapter, gpu.budgetBytes >> 20);
		BugReport_Record("health", machine);
	}
	const RenderProgress progress = RenderProgress_Sample();
	uint64_t renderCalls = 0, mainCalls = 0, scans = 0;
	PumpStatus(renderCalls, mainCalls, scans);
	float livePrepare = 0, liveAo = 0, liveTotal = 0;
	Filmmaker::Environment::NativeMaterialLiveCost(livePrepare, liveAo, liveTotal);
	unsigned long long ownedBytes = 0; unsigned ownedResources = 0;
	Filmmaker::RenderOwnedMemory_Totals(ownedBytes, ownedResources);
	const RenderShaderCost shaders = RenderShaderCost_Sample();
	const Environment::NativeMaterialDerivation derived = Environment::NativeMaterialDerivationStatus();
	char line[1408];
	snprintf(line, sizeof(line),
		"heartbeat map=%s | frame p50=%.1f p95=%.1f max=%.1f ms over33=%u/%u | hook draw=%.2f (lighting check %.2f redraw %.2f caster %.2f; live %.2f prepare %.2f ao %.2f) present=%.2f env=%.2f ms"
		" | hook samples stride=%u age=%llu ms | render phase=%s age=%llu ms frameAge=%llu ms started=%d pumps=%llu/%llu"
		" | game cpu %.0f%% gpu %.0f%% ram %llu MB (private %llu) vram %llu/%llu MB (hook %llu MB in %u) | system cpu %.0f%% ram free %llu/%llu MB | sampler %.1f ms"
		" | shaders %llu engine %.0f ms hook %.0f ms (max %.1f) derived %llu in %.0f ms (%zu queued) | %s",
		map.c_str(), timing.p50Ms, timing.p95Ms, timing.maxMs, timing.over33, timing.samples,
		cost.avgDrawHookMs, cost.avgDrawBeginMs, cost.avgDrawExtraMs, cost.avgDrawCasterMs, liveTotal, livePrepare, liveAo, cost.avgPresentHookMs, cost.avgEnvironmentMs,
		cost.sampleStride, cost.sampleAgeMs, progress.phase, progress.phaseAgeMs, progress.frameAgeMs, progress.started ? 1 : 0,
		static_cast<unsigned long long>(renderCalls), static_cast<unsigned long long>(mainCalls),
		sys.gameCpuPercent, sys.gameGpuPercent, sys.gameWorkingSetMb, sys.gamePrivateMb,
		haveGpu ? gpu.usedBytes >> 20 : 0ull, haveGpu ? gpu.budgetBytes >> 20 : 0ull, ownedBytes >> 20, ownedResources,
		sys.systemCpuPercent, sys.ramFreeMb, sys.ramTotalMb, sys.sampleMs,
		shaders.shaders, shaders.engineMs, shaders.hookMs, shaders.maxHookMs, derived.done, derived.workMs, derived.queued,
		Relight::HeartbeatText().c_str());
	BugReport_Record("health", line);
	CrashReport_NoteFrameTime(timing.p95Ms, timing.samples);
}

} // namespace Filmmaker
