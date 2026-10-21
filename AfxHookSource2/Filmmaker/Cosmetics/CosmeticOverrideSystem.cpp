#include "CosmeticOverrideSystem.h"

#include "CosmeticCatalog.h"
#include "CosmeticModelSwap.h"
#include "CosmeticDirectComposite.h"
#include "../Debug/MvmDebugLog.h"
#include "CosmeticEntityAccess.h" // SEH-guarded entity reads/writes (was this file's anon namespace)

#include "../Demo/PlayingDemoPath.h"
#include "../../hlaeFolder.h"          // the retired global profile file, removed at startup

#include "../../ClientEntitySystem.h" // CEntityInstance, entity-list globals, CBaseHandle, AfxGetLocalObserverState
#include "../../SchemaSystem.h"        // g_clientDllOffsets, g_cosmeticsOffsetsOk

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "../../../Shared/binutils.h"
#include "../../MirvTime.h"               // g_MirvTime.GetCurrentDemoTick for the tick nudge
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h" // ISource2EngineToClient (demo seek)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

// Engine pointer (same one CameraPath/MovieMode use) for the demo re-seek that forces a renderable
// rebuild after a cosmetic change (the user-requested "nudge a few ticks" lever).
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Min frames between clobber-driven re-composites during demo playback (demo packets reset attrs often).
constexpr uint64_t kClobberCompositeCooldownFrames = 64;

// Paused frames between a native mesh-group switch and the one composite that textures the new group.
constexpr uint64_t kMeshSwitchSettleFrames = 8;

// Uninterrupted settled frames after a seek before knife-type swaps and native mesh switches may run.
constexpr int kKnifeSwapStableFrames = 8;

// Quiet frames after the last profile edit before the demo's project is written (~0.5s at 60 fps):
// one write for a burst of picks, never one per edit.
constexpr uint64_t kProfileSaveQuietFrames = 30;

bool DemoIsPaused() {
	if (!g_pEngineToClient)
		return false;
	if (auto* demo = g_pEngineToClient->GetDemoFile())
		return demo->IsDemoPaused();
	return false;
}

} // namespace

CosmeticOverrideSystem& CosmeticsRef() {
	static CosmeticOverrideSystem s;
	return s;
}

void Cosmetics_RunFrame() {
	// Per-player LIVE skin-state snapshot into the mvm_debug log. Runs BEFORE the apply (and is not
	// gated on cosmetics being enabled) so the log shows what the game is actually rendering for the
	// spectated player -- to compare against any override -- on every player switch / seek / weapon /
	// loadout change. No-op unless mvm_debug is active. (CosmeticDebug.cpp)
	Cosmetics_TickSkinStateLog();
	CosmeticsRef().RunFrame();
}

void CosmeticOverrideSystem::Init() {
	if (m_initialized)
		return;
	// Picks belong to a demo: they load when that demo opens (LoadDemoProfiles), never at startup.
	m_store.ClearAll();
	m_store.SetEnabled(true);
	m_armed = true;
	// Older builds kept one global %APPDATA%\HLAE\cosmetic_profiles.json that nothing reads any more.
	{
		std::error_code ec;
		std::filesystem::remove(std::filesystem::path(GetHlaeRoamingAppDataFolderW()) / L"cosmetic_profiles.json", ec);
	}
	m_initialized = true;
}

bool CosmeticOverrideSystem::CompositedMaterialsBuilt() const {
	for (const MaterialWatch& m : m_materialWatch) {
		const SOURCESDK::CS2::CBaseHandle handle(m.handle);
		CEntityInstance* ent = handle.IsValid() ? CosmeticEntFromIndex(handle.GetEntryIndex()) : nullptr;
		// Recycled or removed since its composite: nothing of ours is left to wait for.
		if (!ent || (uintptr_t)ent != m.entity || (uint32_t)ent->GetHandle().ToInt() != m.handle)
			continue;
		int built = -1, pending = -1;
		if (!ReadCompositeOwnerCounts((unsigned char*)ent, &built, &pending))
			return false;
		if (pending > 0 || built <= 0)
			return false;
	}
	return true;
}

void CosmeticOverrideSystem::Shutdown() {
	ResetSessionOverrides("shutdown");
}

void CosmeticOverrideSystem::ResetSessionOverrides(const char* reason) {
	// The outgoing demo keeps its picks: write them before the store is cleared.
	FlushProfiles();
	// Deferred picks belong to the demo that requested them.
	if (!m_livePicks.empty() && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.livepick", "CANCEL session reason=%s removed=%zu",
			reason ? reason : "unknown", m_livePicks.size());
	m_livePicks.clear();
	m_livePickRequestFrame = 0;
	// Session state must be discarded before the egg's deferred OFF callback. Restoring its
	// saved profiles here would resurrect the preceding demo's choices in the new one.
	if ((m_theme || !m_themeSavedProfiles.empty() || !m_themeSavedBots.empty()) && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.theme", "discard session reason=%s active=%d profiles=%zu bots=%zu",
			reason ? reason : "unknown", m_theme ? 1 : 0, m_themeSavedProfiles.size(), m_themeSavedBots.size());
	m_theme = false;
	m_themeSavedProfiles.clear();
	m_themeSavedBots.clear();
	ForgetThemeEntities(reason);
	RestorePaintkitBridgeCvar();
	m_store.ClearAll();
	m_store.SetEnabled(true);
	m_armed = true;
	m_agentState.clear();
	m_botAgents.clear();
	m_gloveState.clear();
	m_lastGloveSpectatedSid = 0;
	m_knifeSwapState.clear();
	m_weaponMeshState.clear();
	m_compositeOwedIdx.clear();
	m_haveDemoPausedState = false;
	m_lastDemoPausedState = false;
	m_resumeWorldMeshReselectFrames = 0;
	m_pendingNudge = false;
	m_pendingNudgeOwedOnly = false;
	m_owedPauseIdx.clear();
	m_nudgePhase = 0;
	m_nudgeWasPaused = false;
	m_postNudgeMeshRefreshPending = false;
	m_framesSinceSeek = 0;     // re-arm the knife-swap stability window on demo load/close
	m_lastDemoTickObserved = -1;
	m_demoPlayingLastFrame = false;
	m_lastDirectCompositeFrame = 0;
	m_materialWatch.clear();
	m_seekSettleFrames = 0;
	m_vmMirrorSettleFrames = 0;
	m_lastCompositeTick = -1;
	m_compositeBurstRemaining = 0;
	m_seekCompositePending = false;
	m_lastSpectatedPawnIndex = -1;
	m_lastCompositeFrameByIdx.clear();
	m_projectDemoPath.clear(); // LoadDemoProfiles binds the next demo
	m_profilesDirty = false;
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.session", "cleared runtime overrides: reason=%s enabled=1 profiles=0 armed=1",
			reason ? reason : "unknown");
}

bool CosmeticOverrideSystem::OffsetsAvailable() const {
	return g_cosmeticsOffsetsOk;
}

bool CosmeticOverrideSystem::InDemoContext() const {
	return !ResolvePlayingDemoPath().empty();
}

void CosmeticOverrideSystem::SetEnabled(bool e) {
	(void)e;
	// Filmmaker demo workflow: cosmetics stay enabled for the whole session.
	m_store.SetEnabled(true);
	m_armed = true;
}

void CosmeticOverrideSystem::Save() {
	m_profilesDirty = true;
	FlushProfiles();
}

void CosmeticOverrideSystem::SetRecompose(bool e) {
	m_recompose = e;
	if (e) m_recomposeFaulted = false;
}

void CosmeticOverrideSystem::GetVtIdx(int* comp, int* sec) const {
	if (comp) *comp = m_vtComposite;
	if (sec) *sec = m_vtCompositeSec;
}

void CosmeticOverrideSystem::SetVtIdx(int comp, int sec) {
	m_vtComposite = comp;
	m_vtCompositeSec = sec;
	m_recompose = true;        // re-arm so the new indices get tried next frame
	m_recomposeFaulted = false;
}

void CosmeticOverrideSystem::SetWeapon(uint64_t steamId, int defIndex, int paintKit, float wear, int seed, int statTrak) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);

	// Knives have their own single slot + model/type swap; route a knife def there. Every other weapon
	// def gets its OWN entry in the per-weapon map, so each weapon type the player carries keeps an
	// independent skin (the "any weapon, per player" model). A defIndex of 0 is rejected -- a per-weapon
	// override requires an explicit weapon (the old "0 = any in slot" wildcard is gone).
	if (CosmeticCatalog::IsKnifeDef(defIndex)) {
		SetKnife(steamId, defIndex, paintKit, wear, seed);
		return;
	}
	if (defIndex <= 0)
		return;

	CosmeticItem& item = profile.weapons[defIndex];
	item.set = true;
	item.defIndex = defIndex;
	item.paintKit = paintKit;
	item.wear = wear;
	item.seed = seed;
	item.statTrak = statTrak;
	// statTrak is left as-supplied for weapons (unlike SetKnife/SetGloves which force -1).
	MarkProfilesChanged();

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;
}

void CosmeticOverrideSystem::ClearWeapon(uint64_t steamId, int defIndex) {
	const bool knife = CosmeticCatalog::IsKnifeDef(defIndex);
	const size_t queuedBefore = m_livePicks.size();
	m_livePicks.erase(std::remove_if(m_livePicks.begin(), m_livePicks.end(),
		[steamId, defIndex, knife](const PendingLivePick& pick) {
			const bool pickKnife = pick.knife || CosmeticCatalog::IsKnifeDef(pick.defIndex);
			return pick.steamId == steamId && (knife ? pickKnife : !pickKnife && pick.defIndex == defIndex);
		}), m_livePicks.end());
	if (m_livePicks.empty()) m_livePickRequestFrame = 0;
	if (queuedBefore != m_livePicks.size() && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.livepick", "CANCEL weapon steam=%llu def=%d removed=%zu remaining=%zu",
			(unsigned long long)steamId, defIndex, queuedBefore - m_livePicks.size(), m_livePicks.size());
	ForgetWeaponMeshSelections();
	if (!knife) MarkRecordedWeaponRestores(steamId, defIndex);
	CosmeticProfile* profile = m_store.Find(steamId);
	if (!profile)
		return;
	if (CosmeticCatalog::IsKnifeDef(defIndex)) {
		profile->knife = CosmeticItem{};
	} else {
		profile->weapons.erase(defIndex);
	}
	if (profile->Empty())
		m_store.ClearPlayer(steamId);
	MarkProfilesChanged();
}

void CosmeticOverrideSystem::SetKnife(uint64_t steamId, int defIndex, int paintKit, float wear, int seed) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);
	profile.knife.set = true;
	profile.knife.defIndex = defIndex;
	profile.knife.paintKit = paintKit;
	profile.knife.wear = wear;
	profile.knife.seed = seed;
	profile.knife.statTrak = -1;
	MarkProfilesChanged();

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;
}

void CosmeticOverrideSystem::SetGloves(uint64_t steamId, int defIndex, int paintKit, float wear, int seed) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);
	profile.gloves.set = true;
	profile.gloves.defIndex = defIndex;
	profile.gloves.paintKit = paintKit;
	profile.gloves.wear = wear;
	profile.gloves.seed = seed;
	profile.gloves.statTrak = -1;
	MarkProfilesChanged();

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;

	// Arm an immediate multi-frame burst even when observer-services returns target -1 (POV demos).
	GloveApplyState& st = m_gloveState[steamId];
	st.frames = 4;
	st.pawnStable = 4;
	m_gloveBypassSid = steamId;
	m_gloveBypassFrames = 90;
}

void CosmeticOverrideSystem::SetAgent(uint64_t steamId, const char* model, int defIndex) {
	CosmeticProfile& profile = m_store.GetOrCreate(steamId);
	if (!model || !*model || 0 == _stricmp(model, "default")) {
		profile.agent.set = false;
		profile.agent.model.clear();
	} else {
		profile.agent.set = true;
		profile.agent.model = model;
		profile.agent.defIndex = defIndex;
	}
	MarkProfilesChanged();

	std::string name = NameForSteamId(steamId);
	if (!name.empty())
		profile.name = name;
}

void CosmeticOverrideSystem::InvalidateAgentStates() {
	m_agentState.clear();
	for (auto& entry : m_botAgents) entry.second.applied = {};
}

void CosmeticOverrideSystem::ForgetThemeEntities(const char* reason) {
	// No native reads or restoration: these pointers belonged to the preceding entity graph.
	if ((!m_themeOriginalModels.empty() || !m_recordedWeapons.empty() || !m_lateMeshReassert.empty()) && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.theme", "forget entities reason=%s models=%zu weapons=%zu late=%zu",
			reason ? reason : "unknown", m_themeOriginalModels.size(), m_recordedWeapons.size(), m_lateMeshReassert.size());
	m_themeOriginalModels.clear();
	m_recordedWeapons.clear();
	m_weaponMaterialBusy = false;
	m_lateMeshReassert.clear();
}

void CosmeticOverrideSystem::BeginTheme() {
	if (m_theme) return;
	m_theme = true;
	for (auto& entry : m_recordedWeapons) {
		entry.second.pending = false;
		entry.second.restored = false;
		entry.second.meshSelected = false;
		entry.second.dueFrame = 0;
	}
	m_themeSavedProfiles = m_store.All();
	m_themeSavedBots = m_botAgents;
	// Cancel followed immediately by another theme must retain any un-restored survivor's original.
	for (auto& entry : m_themeOriginalModels) { entry.second.pending = false; entry.second.restoring = false; entry.second.dueFrame = 0; }
	m_armed = true;
}

void CosmeticOverrideSystem::EndTheme() {
	if (!m_theme) return;
	m_theme = false;
	MarkRecordedWeaponRestores();
	ForgetWeaponMeshSelections();
	m_store.ReplaceAll(m_themeSavedProfiles);
	m_botAgents = m_themeSavedBots;
	InvalidateAgentStates();
	for (auto& entry : m_themeOriginalModels) { entry.second.pending = true; entry.second.restoring = false; entry.second.dueFrame = 0; }
	m_themeSavedProfiles.clear();
	m_themeSavedBots.clear();
	// The same pump runs before the empty-profile gate and defers writes through seek reconstruction.
	RunThemePawnRestores();
}

bool CosmeticOverrideSystem::SetPlayerAgent(const CosmeticPlayerRef& player, const char* model) {
	const SOURCESDK::CS2::CBaseHandle handle(player.controllerHandle);
	CEntityInstance* controller = handle.IsValid() ? CosmeticEntFromIndex(handle.GetEntryIndex()) : nullptr;
	if (!controller || !controller->IsPlayerController()
		|| controller->GetHandle().ToInt() != player.controllerHandle
		|| controller->GetSteamId() != player.steamId)
		return false;
	if (player.steamId != 0) {
		SetAgent(player.steamId, model, 0);
	} else if (!model || !*model || 0 == _stricmp(model, "default")) {
		m_botAgents.erase(player.controllerHandle);
	} else {
		auto& entry = m_botAgents[player.controllerHandle];
		entry.agent.set = true;
		entry.agent.model = model;
		entry.applied = {};
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.agent.pick", "controller=%u steam=%llu team=%d model='%s'",
			player.controllerHandle, (unsigned long long)player.steamId, player.team, model ? model : "default");
	return true;
}

void CosmeticOverrideSystem::ClearPlayer(uint64_t steamId) {
	const size_t queuedBefore = m_livePicks.size();
	m_livePicks.erase(std::remove_if(m_livePicks.begin(), m_livePicks.end(),
		[steamId](const PendingLivePick& pick) { return pick.steamId == steamId; }), m_livePicks.end());
	if (m_livePicks.empty()) m_livePickRequestFrame = 0;
	if (queuedBefore != m_livePicks.size() && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.livepick", "CANCEL player steam=%llu removed=%zu remaining=%zu",
			(unsigned long long)steamId, queuedBefore - m_livePicks.size(), m_livePicks.size());
	ForgetWeaponMeshSelections();
	m_store.ClearPlayer(steamId);
	MarkProfilesChanged();
	MarkRecordedWeaponRestores(steamId);
}

// A cleared override leaves the entity on whatever group we last selected while the per-entity memo
// still says "done": re-picking a finish with the same mask afterwards skipped the native switch.
void CosmeticOverrideSystem::ForgetWeaponMeshSelections() {
	m_lateMeshReassert.clear();
	m_weaponMeshState.clear();
	m_meshFollowupCompositeDue.clear();
}

void CosmeticOverrideSystem::ClearAll() {
	// Do not let the next paused frame recreate a profile that was just cleared.
	if (!m_livePicks.empty() && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.livepick", "CANCEL all removed=%zu", m_livePicks.size());
	m_livePicks.clear();
	m_livePickRequestFrame = 0;
	ForgetWeaponMeshSelections();
	m_store.ClearAll();
	MarkProfilesChanged();
	m_botAgents.clear();
	m_agentState.clear();
	MarkRecordedWeaponRestores();
}

int CosmeticOverrideSystem::CurrentSpectatedPawnIndex() const {
	// Robust spectated-pawn resolution: engine observer-services first, then a render-eye match
	// against the game camera. The observer-services path alone reads target -1 in POV/GOTV demos
	// (so 'current' could never resolve there); the eye-match fallback fixes that. See
	// AfxGetSpectatedPawnIndex in ClientEntitySystem.cpp.
	auto validPawnIndex = [](int index) {
		if (index < 0 || index > GetHighestEntityIndex())
			return false;
		CEntityInstance* pawn = CosmeticEntFromIndex(index);
		return pawn && pawn->IsPlayerPawn();
	};

	const int resolved = AfxGetSpectatedPawnIndex();
	if (validPawnIndex(resolved)) {
		m_lastSpectatedPawnIndex = resolved;
		return resolved;
	}

	// POV demos on the current client commonly expose obsMode=0 / target=-1 even though the split-
	// screen controller still owns the pawn whose first-person view is being rendered. Render-eye
	// matching also (correctly) drops when the filmmaker camera is moved away from that eye. Cosmetic
	// targeting must remain attached to that viewer pawn across the camera move instead of reporting
	// "no-spectate" and suspending world/viewmodel maintenance.
	CEntityInstance* localViewer = AfxGetLocalViewerPawn();
	if (localViewer && localViewer->IsPlayerPawn()) {
		const SOURCESDK::CS2::CBaseHandle handle = localViewer->GetHandle();
		if (handle.IsValid() && validPawnIndex(handle.GetEntryIndex())) {
			m_lastSpectatedPawnIndex = handle.GetEntryIndex();
			return m_lastSpectatedPawnIndex;
		}
	}

	// Both live signals can be null for a handful of frames while the demo/viewmodel camera is
	// rebuilding. Keep the last target only while that exact entity index still resolves to a pawn;
	// entity deletion or demo-session reset invalidates it immediately.
	if (validPawnIndex(m_lastSpectatedPawnIndex))
		return m_lastSpectatedPawnIndex;
	m_lastSpectatedPawnIndex = -1;
	return -1;
}

uint8_t CosmeticOverrideSystem::CurrentObserverMode() const {
	return AfxGetLocalObserverState(nullptr);
}

uint64_t CosmeticOverrideSystem::CurrentSpectatedSteamId() const {
	int pawnIndex = CurrentSpectatedPawnIndex();
	if (pawnIndex < 0)
		return 0;

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = CosmeticEntFromIndex(i);
		if (!ent || !ent->IsPlayerController())
			continue;
		if (ent->GetPlayerPawnHandle().GetEntryIndex() == pawnIndex)
			return ent->GetSteamId();
	}
	return 0;
}

std::string CosmeticOverrideSystem::NameForSteamId(uint64_t steamId) const {
	if (steamId == 0)
		return std::string();

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = CosmeticEntFromIndex(i);
		if (!ent || !ent->IsPlayerController())
			continue;
		if (ent->GetSteamId() != steamId)
			continue;
		const char* sanitized = ent->GetSanitizedPlayerName();
		if (sanitized && *sanitized)
			return std::string(sanitized);
		const char* raw = ent->GetPlayerName();
		if (raw && *raw)
			return std::string(raw);
		return std::string();
	}
	return std::string();
}

void CosmeticOverrideSystem::AdvanceFrameClocks() {
	++m_frameCounter;
	if (m_vmMirrorSettleFrames > 0) --m_vmMirrorSettleFrames;
	// Decay unconditionally (regardless of the enabled/armed/profiles/demo gate): this used to live
	// only in the SEEK-SAFETY GATE, which the empty-profile early-return skips entirely -- so with
	// zero cosmetic profiles set (a fresh session, or the Customize preview one-shot previewnudge
	// with no profile yet), a seek anywhere in the session left this stuck at its post-seek value
	// FOREVER, and MaybeFireTickNudge() permanently deferred every pending nudge as a result
	// (confirmed live: seekSettle stuck at 152 for 2000+ frames, previewnudge silently doing nothing).
	if (m_seekSettleFrames > 0) --m_seekSettleFrames;

	// Cosmetic state is scoped to one uninterrupted demo run: clear ONLY when the demo PATH changes
	// (a different demo loaded, or the demo closed). Overrides are keyed by owner XUID and the apply
	// loop re-discovers entities every frame, so they intentionally PERSIST across scrubs in BOTH
	// directions -- including a scrub all the way back to tick 0. An old "demo tick reset" path wiped
	// the store on any back-to-start scrub, which silently desynced the Customize UI (it re-read the
	// now-default loadout) from the still-applied overrides in the game; that wipe was removed. A
	// genuine reload loads a fresh path and is still caught here; use "cosmetics clear" on demand.
	{
		// An EMPTY path is not proof the demo closed. IsPlayingDemo() blips false for a few frames
		// during every demo_gototick -- and a timeline scrub IS demo_gototick -- so treating the first
		// empty frame as a close DELETED the user's picks on every scrub. The store was runtime-only
		// then, so the skin was gone for good: the reported "scrub back, weapon reverts to its default
		// skin" bug. Picks now reload from the demo's project, but a reset mid-scrub would still strip
		// every applied skin until the next paused composite.
		// Debounce it the same way, and for the same reason, as MovieMode::ApplyXrayDefault's
		// kXrayDemoGoneFrames.
		//
		// A genuinely DIFFERENT demo is still caught immediately, because it resolves to a NON-empty
		// path that differs from m_lastDemoPath: the debounce only delays the "closed" verdict, it
		// never lets one demo's overrides leak into another.
		constexpr int kDemoGoneFrames = 120;
		std::wstring curDemo = ResolvePlayingDemoPath();
		if (curDemo.empty()) {
			if (!m_lastDemoPath.empty()
				&& m_demoGoneFrames < kDemoGoneFrames
				&& ++m_demoGoneFrames >= kDemoGoneFrames) {
				m_lastDemoPath.clear();
				ResetSessionOverrides("demo closed");
			}
		} else {
			m_demoGoneFrames = 0; // a path resolved: the blip (if there was one) is over
			if (curDemo != m_lastDemoPath) {
				m_lastDemoPath = curDemo;
				ResetSessionOverrides("demo path changed");
				LoadDemoProfiles();
			}
		}
	}
	if (m_profilesDirty && m_frameCounter - m_profilesChangedFrame >= kProfileSaveQuietFrames)
		FlushProfiles();
	DetectDemoSeekEarly();
	MaybeFireTickNudge();
	CommitLivePicks();

	// Belt-and-suspenders: enabled cosmetics must always be armed in demo context so the apply loop
	// is not blocked until the first UI mutation (log evidence: armed=0 blocked ~10s of spectating).
	if (m_store.Enabled() && InDemoContext() && !m_armed) {
		m_armed = true;
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.armed", "auto-armed in demo context frame=%llu", (unsigned long long)m_frameCounter);
	}
	if (MvmDebugLog_Active() && (m_frameCounter % 512) == 0) {
		MvmDebugLog_Linef("cosmetics.heartbeat",
			"armed=%d enabled=%d profiles=%zu inDemo=%d totalNudges=%llu",
			m_armed ? 1 : 0, m_store.Enabled() ? 1 : 0,
			m_store.All().size(), InDemoContext() ? 1 : 0, (unsigned long long)m_totalNudges);
	}
}

bool CosmeticOverrideSystem::ApplyGateOpen() {
	// Cheap no-op gate: disabled, NOT armed this demo, offsets unresolved, no profiles, or no demo.
	if (!m_store.Enabled() || !m_armed || !g_cosmeticsOffsetsOk || (m_store.Empty() && m_botAgents.empty()) || !InDemoContext())
		return false;
	// SEEK-SAFETY GATE: skip apply while entities rebuild after a demo seek (DetectDemoSeekEarly).
	// The decrement happens in AdvanceFrameClocks; this only decides whether to apply this frame.
	if (m_seekSettleFrames > 0)
		return false;
	return true;
}

void CosmeticOverrideSystem::ResetApplyStats() {
	m_lastStats.entitiesScanned = 0;
	m_lastStats.entitiesMatched = 0;
	m_lastStats.entitiesPatched = 0;
	m_lastStats.entitiesReverted = 0;
	m_lastStats.attrListsRead = 0;
	m_lastStats.attrValuesWritten = 0;
	m_lastStats.attrValuesChanged = 0;
	m_lastStats.attrListsEmpty = 0;
	m_lastStats.namedSetterApplied = 0;
	m_lastStats.directCompositeCalls = 0;
	m_lastStats.directCompositeResolved = 0;
	m_lastStats.directCompositeFaulted = 0;
}

void CosmeticOverrideSystem::ResetFrameStats() {
	++m_lastStats.frame;
	ResetApplyStats();
	m_lastStats.paintkitBridgeValue = 0;
	m_lastStats.modelSwapResolved = 0;
	m_lastStats.knifeModelsApplied = 0;
	m_lastStats.weaponMeshFixed = 0;
	m_lastStats.pawnsScanned = 0;
	m_lastStats.glovesApplied = 0;
	m_lastStats.agentsApplied = 0;
}

void CosmeticOverrideSystem::LogApplyEvent() const {
	if (!MvmDebugLog_Active()) return;
	const CosmeticFrameStats& s = m_lastStats;
	const bool event = s.entitiesMatched > 0 && (s.attrValuesChanged > 0 || s.knifeModelsApplied > 0 ||
		s.glovesApplied > 0 || s.agentsApplied > 0 || s.weaponMeshFixed > 0);
	if (!event) return;
	MvmDebugLog_Linef("cosmetics.apply", "matched=%d patched=%d attrChanged=%d "
		"knifeSwap=%d weaponMesh=%d gloves=%d agents=%d skinAttrsWritten=%d",
		s.entitiesMatched, s.entitiesPatched,
		s.attrValuesChanged, s.knifeModelsApplied, s.weaponMeshFixed, s.glovesApplied,
		s.agentsApplied, s.attrValuesWritten);
}

void CosmeticOverrideSystem::RunFrame() {
	// A pass that closes the apply gate must also discard the preceding pass's
	// pointers. Cancellation/unload otherwise kept replaying the old theme masks.
	m_lateMeshReassert.clear();
	AdvanceFrameClocks();
	// Post-seek stability clock. It advances on every settled frame, not only while the apply gate is
	// open: it used to stall while no override existed yet, so the FIRST pick after a seek saw
	// m_meshSwapStable=false, composited onto the old mesh group before switching, and that in-flight
	// build swallowed the correct one (measured 2026-10-09: first legacy->CS2 pick stayed untextured).
	if (m_seekSettleFrames == 0 && m_framesSinceSeek < 0x40000000) ++m_framesSinceSeek;
	m_meshSwapStable = m_framesSinceSeek >= kKnifeSwapStableFrames;
	RunRecordedWeaponRestores();
	RunThemePawnRestores();


	if (!ApplyGateOpen()) {
		// Clearing the final profile leaves no mesh pass to finish this pause.
		// Theme restoration ran above; deferred picks committed before the gate.
		if (m_store.Empty() && m_botAgents.empty() && DemoIsPaused())
			m_postNudgeMeshRefreshPending = false;
		UpdatePaintkitBridge();
		return;
	}

	// Knife-swap stability window. The seek settle is enough for the SEH-guarded POD attribute
	// writes + the composite refresh, but NOT for the knife model-type swap: its SetModel +
	// UpdateSubclass re-derive the weapon VData/animation set, and if that lands on an entity the
	// engine is still reconstructing after a seek the animation state is left inconsistent and faults
	// a few SECONDS later (outside the SEH guard) -- the "scrub, knife redeploys, then it crashes"
	// report. So the knife swap waits for a run of uninterrupted frames since the last seek; rapid
	// stacked scrubs keep resetting m_framesSinceSeek, so it can never fire mid-reconstruction.
	// Frame-based (not tick-based) so it also advances while paused, where the demo tick is frozen.
	// Was 64 frames purely to avoid the unloaded-model anim CRASH on a half-rebuilt entity;
	// CosmeticAnimFix now neutralizes that crash, so the window is tightened to a few frames --
	// which closes the visible window where the knife reverted to its ORIGINAL model (skin already
	// re-applied) right after a seek.
	const bool knifeSwapStable = m_framesSinceSeek >= kKnifeSwapStableFrames;

	// The native CGameSceneNode::SetMeshGroupMask needs the same post-seek settle as the knife swap.
	// It dereferences renderer-side state the engine is still rebuilding during a seek: firing it
	// mid-reconstruction faulted inside the callee at client.dll+0xa73cf6 (the function the setter
	// calls right after its equal-value early-out), reproduced twice on long scrubs. The direct field
	// write it replaced was inert, so this exposure is new and has to be gated.

	// The documented working recipe composites the deployed weapon while the demo is PAUSED, then
	// lets the tick nudge advance the renderer. Never run the chain during live demo packets: the
	// July 2026 crash was reproduced when a preserved retry burst fired after demo_resume. Consume
	// burst attempts while paused so none survive into the nudge; the per-entity cooldown prevents
	// an immediate apply plus its queued burst from calling the chain twice.
	const bool demoPaused = DemoIsPaused();
	if (m_haveDemoPausedState && m_lastDemoPausedState && !demoPaused) {
		// A live packet can restore the third-person renderer's cached mesh selection without
		// changing CModelState::m_MeshGroupMask. The late pass must therefore use the native setter,
		// not merely rewrite the field, across the first few rendered playback frames.
		m_resumeWorldMeshReselectFrames = 3;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("cosmetics.weapon.worldresume",
				"paused->playing: armed native world mesh reselect for %d late frames",
				m_resumeWorldMeshReselectFrames);
	}
	m_haveDemoPausedState = true;
	m_lastDemoPausedState = demoPaused;
	// Pay the composite refresh a seek owes us. Only on a PAUSED frame past the settle window
	// (ApplyGateOpen filtered that): the burst decrements unconditionally below, so arming it
	// during live playback would drain it with zero composites fired. And only once the native mesh
	// switches may run (m_meshSwapStable), so a gun that changes legacy/CS2 mesh composites ONCE, on
	// its new mesh: composited first and switched eight frames later, it needed a second composite
	// that the overlap cooldown held back another 64 frames (a 136-frame seek hold on a 300-tick
	// loop jump, measured 2026-10-21; 87 frames with this gate).
	if (m_seekCompositePending && demoPaused && m_meshSwapStable) {
		m_seekCompositePending = false;
		RequestCompositeRefresh();
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.seek", "post-seek composite burst re-armed on paused frame=%llu",
				(unsigned long long)m_frameCounter);
	}
	// Not in the few frames between a seek's settle and m_meshSwapStable: a composite there lands on
	// the mesh group about to be left, and the seek burst above covers every deployed gun right after.
	const bool allowDirectComposite = m_autoComposite && demoPaused && m_meshSwapStable;
	const bool finalizePostNudgeMesh = demoPaused && m_meshSwapStable && m_postNudgeMeshRefreshPending; // see above
	const bool burstPending = m_compositeBurstRemaining > 0;
	if (burstPending)
		--m_compositeBurstRemaining;
	const bool burstComposite = allowDirectComposite && burstPending;

	ResetFrameStats();

	// Per-frame: apply attributes, optional stale flags, and model/mesh state. While paused, a
	// mutation may run the proven-safe direct-composite chain; live playback relies on the native
	// rebuild with cl_paintkit_override in effect.
	ApplyMatchedWeapons(/*forceStale=*/false, /*fireRebuildCall=*/m_recompose, /*fireDirectComposite=*/allowDirectComposite,
		/*allowKnifeSwap=*/knifeSwapStable, /*periodicComposite=*/burstComposite);
	if (finalizePostNudgeMesh)
		m_postNudgeMeshRefreshPending = false;

	// Gloves + agent (pawn-level model swap), keyed by owner SteamID like the weapon loop.
	m_gloveApplyBudget = 1;
	m_constructPaintBudget = 1;
	ApplyPawnCosmetics();

	// Cumulative (never-reset) apply counters -- so a one-shot/gated apply stays observable in status.
	m_totalKnife += m_lastStats.knifeModelsApplied;
	m_totalWeaponMesh += m_lastStats.weaponMeshFixed;
	m_totalGloves += m_lastStats.glovesApplied;
	m_totalAgent += m_lastStats.agentsApplied;

	LogApplyEvent();
	UpdatePaintkitBridge();
}

// Manual, on-demand rebuild: re-asserts the visuals-stale field writes (forceStale) on every matched
// weapon entity even when nothing changed, and fires the recompose vcall once if armed. Decoupled
// from the per-frame change detection so the user can retrigger a refresh attempt for a screenshot.
// Does NOT require the apply loop to be enabled -- it is an explicit action -- but still hard-gated on
// offsets resolving, a demo playing, and at least one stored profile. Returns matched entity count.
int CosmeticOverrideSystem::RebuildOnce() {
	if (!g_cosmeticsOffsetsOk || !InDemoContext() || m_store.Empty())
		return 0;
	MvmCrashWatch_Arm(-1, "weapon-rebuild");
	ResetApplyStats();
	return ApplyMatchedWeapons(/*forceStale=*/true, /*fireRebuildCall=*/m_recompose,
		/*fireDirectComposite=*/m_autoComposite && DemoIsPaused(),
		/*allowKnifeSwap=*/true, /*periodicComposite=*/false);
}

int CosmeticOverrideSystem::CompositeOnce() {
	if (!g_cosmeticsOffsetsOk || !InDemoContext() || m_store.Empty())
		return 0;
	const bool paused = DemoIsPaused();
	if (!paused)
		RequestApplyNudge();
	ResetApplyStats();
	return ApplyMatchedWeapons(/*forceStale=*/true, /*fireRebuildCall=*/false, /*fireDirectComposite=*/paused,
		/*allowKnifeSwap=*/true, /*periodicComposite=*/false);
}

bool CosmeticOverrideSystem::SetRebuildFlag(const char* name, bool on) {
	if (!name) return false;
	if (0 == _stricmp(name, "visualsdata") || 0 == _stricmp(name, "visualsdataset")) m_rebuildFlags.visualsDataSet = on;
	else if (0 == _stricmp(name, "clearugc")) m_rebuildFlags.clearUgc = on;
	else if (0 == _stricmp(name, "reloadevent")) m_rebuildFlags.reloadEvent = on;
	else if (0 == _stricmp(name, "initialized") || 0 == _stricmp(name, "init")) m_rebuildFlags.initialized = on;
	else if (0 == _stricmp(name, "attrinit")) m_rebuildFlags.attrInit = on;
	else if (0 == _stricmp(name, "imagecache")) m_rebuildFlags.imageCache = on;
	else if (0 == _stricmp(name, "attrparity") || 0 == _stricmp(name, "parity")) m_rebuildFlags.attrParity = on;
	else return false;
	return true;
}

void CosmeticOverrideSystem::SetAllRebuildFlags(bool on) {
	m_rebuildFlags.visualsDataSet = on;
	m_rebuildFlags.clearUgc = on;
	m_rebuildFlags.reloadEvent = on;
	m_rebuildFlags.initialized = on;
	m_rebuildFlags.attrInit = on;
	m_rebuildFlags.imageCache = on;
	m_rebuildFlags.attrParity = on;
}

uint64_t CosmeticOverrideSystem::CurrentActiveWeaponOwnerSteamId() const {
	if (!g_cosmeticsOffsetsOk || !InDemoContext())
		return 0;

	const int pawnIndex = CurrentSpectatedPawnIndex();
	if (pawnIndex < 0)
		return 0;

	CEntityInstance* pawn = CosmeticEntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return 0;

	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	CEntityInstance* weapon = wh.IsValid() ? CosmeticEntFromIndex(wh.GetEntryIndex()) : nullptr;
	if (!weapon || !CosmeticLooksLikeWeaponEntity(weapon))
		return 0;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	WeaponEconRead econ;
	if (!CosmeticTryReadWeaponEconInfo((unsigned char*)weapon,
			o.C_EconEntity.m_OriginalOwnerXuidLow, o.C_EconEntity.m_OriginalOwnerXuidHigh,
			o.C_EconEntity.m_AttributeManager, o.C_AttributeContainer.m_Item,
			o.C_EconItemView.m_iItemDefinitionIndex, &econ))
		return 0;

	return econ.xuid != 0 ? econ.xuid : CurrentSpectatedSteamId();
}

int CosmeticOverrideSystem::ResolveSpectatedPaintkitOverride() const {
	if (m_paintkitBridgeForcedValue > 0)
		return m_paintkitBridgeForcedValue;

	if (!m_store.Enabled() || !g_cosmeticsOffsetsOk || m_store.Empty() || !InDemoContext())
		return 0;

	const int pawnIndex = CurrentSpectatedPawnIndex();
	if (pawnIndex < 0)
		return 0;

	CEntityInstance* pawn = CosmeticEntFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return 0;

	SOURCESDK::CS2::CBaseHandle wh = pawn->GetActiveWeaponHandle();
	CEntityInstance* weapon = wh.IsValid() ? CosmeticEntFromIndex(wh.GetEntryIndex()) : nullptr;
	if (!weapon || !CosmeticLooksLikeWeaponEntity(weapon))
		return 0;

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	WeaponEconRead econ;
	if (!CosmeticTryReadWeaponEconInfo((unsigned char*)weapon,
			o.C_EconEntity.m_OriginalOwnerXuidLow, o.C_EconEntity.m_OriginalOwnerXuidHigh,
			o.C_EconEntity.m_AttributeManager, o.C_AttributeContainer.m_Item,
			o.C_EconItemView.m_iItemDefinitionIndex, &econ))
		return 0;

	uint64_t profileXuid = econ.xuid;
	if (profileXuid == 0) {
		// Synthetic/scripted pickups in hard-demo fixtures have no OriginalOwnerXuid. Only use the
		// spectated holder as the fallback for the active weapon; never paint arbitrary world drops.
		SOURCESDK::CS2::CBaseHandle aw = pawn->GetActiveWeaponHandle();
		if (!aw.IsValid() || aw.GetEntryIndex() != wh.GetEntryIndex())
			return 0;
		profileXuid = CurrentSpectatedSteamId();
	}

	const CosmeticProfile* prof = m_store.Find(profileXuid);
	if (!prof)
		return 0;

	const CosmeticItem* item = nullptr;
	if (CosmeticCatalog::IsKnifeDef(econ.liveDef)) {
		if (prof->knife.set)
			item = &prof->knife;
	} else {
		const CosmeticItem* cand = prof->FindWeapon(econ.liveDef);
		if (cand && cand->set)
			item = cand;
	}

	if (!item || item->paintKit <= 0)
		return 0;
	return item->paintKit;
}

// Runs after the engine's own FrameStageNotify handler, so this is the last mesh-group write before
// the draw. Deliberately minimal: no entity walk, no econ writes, no composite -- just replay the
// masks the apply pass already decided on, against the entity pointers it already validated.
void CosmeticOverrideSystem::ReassertMeshMasksLate() {
	// m_meshSwapStable: never re-assert into a seek that is still settling (see where it is set).
	if (!InDemoContext() || !Enabled() || !m_modelSwap || !m_meshSwapStable || m_lateMeshReassert.empty()) {
		m_lateMeshReassert.clear();
		return;
	}
	const bool nativeReselect = m_resumeWorldMeshReselectFrames > 0;
	for (size_t n = 0; n < m_lateMeshReassert.size(); ++n) {
		const auto& queued = m_lateMeshReassert[n];
		const SOURCESDK::CS2::CBaseHandle handle(queued.handle);
		CEntityInstance* live = handle.IsValid() ? CosmeticEntFromIndex(handle.GetEntryIndex()) : nullptr;
		if (!live || (uintptr_t)live != queued.entity || live->GetHandle().ToInt() != queued.handle) {
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("cosmetics.weapon.late", "discard stale handle=%u entity=%p", queued.handle, (void*)queued.entity);
			continue;
		}
		unsigned char* w = (unsigned char*)live;
		const uint64_t mask = queued.mask;
		if (mask == 0) continue;
		// Stable frames only need the non-invalidating field hold. Immediately after playback resumes,
		// however, CS2 can reset the WORLD renderable's cached group while leaving this field unchanged;
		// briefly re-run the native setter after CS2's handler so third-person geometry stays aligned.
		// What the engine left in the field this frame. A value other than ours means CS2 re-applied the
		// demo's recorded group through its NATIVE setter, which is what the third-person renderer
		// follows; a field write alone then leaves the world model drawing the recorded (modern) mesh
		// under a legacy finish -- the bare-gun report (2026-10-13).
		const uint64_t before = ReadEntityMeshGroupMask(live);
		const bool clobbered = before != mask;
		if (MvmDebugLog_Active()) {
			static ULONGLONG lastLog = 0; const ULONGLONG now = GetTickCount64();
			if (clobbered || now - lastLog >= 2000) { lastLog = now;
				MvmDebugLog_LinefAlways("cosmetics.weapon.late", "entity=%p field=%llu want=%llu clobbered=%d nativeReselect=%d",
					(void*)w, (unsigned long long)before, (unsigned long long)mask, clobbered ? 1 : 0, nativeReselect ? 1 : 0); }
		}
		const bool applied = (nativeReselect || clobbered)
			? ReselectWeaponMeshMask(w, mask)
			: HoldWeaponMeshMaskDirect(w, mask);
		if (applied) {
			++m_lastStats.weaponMeshFixed;
			if (nativeReselect && MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("cosmetics.weapon.worldresume",
					"native late reselect frameRemaining=%d entity=%p mask=%llu",
					m_resumeWorldMeshReselectFrames, (void*)w, (unsigned long long)mask);
		}
	}
	if (nativeReselect)
		--m_resumeWorldMeshReselectFrames;
	m_lateMeshReassert.clear(); // consume once, even if the next main apply pass is skipped
}

int CosmeticOverrideSystem::ApplyMatchedWeapons(bool forceStale, bool fireRebuildCall, bool fireDirectComposite,
	bool allowKnifeSwap, bool periodicComposite) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ent = CosmeticEntFromIndex(i);
		if (!ent)
			continue;
		if (!CosmeticLooksLikeWeaponEntity(ent))
			continue; // gloves/agent are not econ weapon entities; handled separately below

		unsigned char* w = (unsigned char*)ent;

		// SEH-guarded read of owner XUID + embedded C_EconItemView pointer + live item-definition
		// index. A "weapon_" entity that is not actually a C_EconEntity is skipped here instead of
		// crashing. Dropped weapons keep OriginalOwnerXuid, so a dropped gun still carries its
		// owner's override.
		WeaponEconRead econ;
		if (!CosmeticTryReadWeaponEconInfo(w,
				o.C_EconEntity.m_OriginalOwnerXuidLow, o.C_EconEntity.m_OriginalOwnerXuidHigh,
				o.C_EconEntity.m_AttributeManager, o.C_AttributeContainer.m_Item,
				o.C_EconItemView.m_iItemDefinitionIndex, &econ))
			continue;

		uint64_t xuid = econ.xuid;
		if (xuid == 0) {
			// Hard-demo scripted pickups carry no original-owner XUID. Match only the weapon currently
			// held by the spectated pawn, using that holder's SteamID as the session profile key.
			const int pawnIndex = CurrentSpectatedPawnIndex();
			CEntityInstance* pawn = pawnIndex >= 0 ? CosmeticEntFromIndex(pawnIndex) : nullptr;
			if (!pawn || !pawn->IsPlayerPawn())
				continue;
			SOURCESDK::CS2::CBaseHandle aw = pawn->GetActiveWeaponHandle();
			if (!aw.IsValid() || aw.GetEntryIndex() != i)
				continue;
			xuid = CurrentSpectatedSteamId();
			if (xuid == 0)
				continue;
		}

		CosmeticProfile* prof = m_store.Find(xuid);
		if (!prof)
			continue;

		++m_lastStats.entitiesScanned;

		unsigned char* itemView = econ.itemView;
		int liveDef = econ.liveDef;

		CosmeticItem* item = nullptr;
		int knifeDefOverride = 0;

		if (CosmeticCatalog::IsKnifeDef(liveDef)) {
			if (prof->knife.set) {
				item = &prof->knife;
				// Desired knife TYPE swap target, taken from the PROFILE -- NOT gated on
				// (item->defIndex != liveDef). CosmeticApplyWrite writes m_iItemDefinitionIndex = target,
				// so the frame after the first write liveDef == target and the old gate computed 0; since
				// a profile change/seek also re-arms the 64-frame stability window, the "def differs" frame
				// and the "stable" frame never coincided and the model swap NEVER fired (confirmed in
				// mvm_debug: liveDef flipped 525->503 but liveModel stayed knife_skeleton). The per-entity
				// m_knifeSwapState throttle below still fires SetModel only ONCE per activation, so driving
				// this purely from the profile is safe.
				if (m_knifeModelSwap && item->defIndex > 0)
					knifeDefOverride = item->defIndex;
			}
		} else {
			// Per-weapon override: look the held weapon's own def up in the player's weapons map. This
			// is exactly "this player's M4 -> this M4 skin", independent per weapon type, so every gun
			// the player carries keeps its own skin.
			CosmeticItem* cand = prof->FindWeapon(liveDef);
			if (cand && cand->set)
				item = cand;
		}

		if (!item)
			continue;

		// Every firearm edit needs a recorded baseline, including ordinary Customize Reset.
		// Refuse incomplete reads before mutation; a surviving seek reconciles owned fields separately.
		CosmeticRecordedWeapon* recorded = nullptr;
		if (!CosmeticCatalog::IsKnifeDef(liveDef)) {
			recorded = ObserveRecordedWeapon(ent, xuid, liveDef);
			if (!recorded) continue;
			recorded->AttemptNetwork(item->paintKit, item->wear, item->seed, item->statTrak);
		}
		++m_lastStats.entitiesMatched;

		// THE real skin write (Phase 2): overwrite the networked dynamic attributes (def 6/7/8/80),
		// where a demo weapon's actual skin lives. The fallback-field CosmeticApplyWrite below is now
		// a harmless backup (those fields are ignored while networked attributes are present).
		int attrStride = (int)o.CEconItemAttribute.m_size;
		int attrMinStride = (int)o.CEconItemAttribute.m_flValue + (int)sizeof(float);
		if (attrStride < attrMinStride) attrStride = attrMinStride;
		AttrWriteResult attr;
		CosmeticWriteNetworkedSkinAttributes(itemView,
			o.C_EconItemView.m_NetworkedDynamicAttributes, o.C_AttributeList.m_Attributes, attrStride,
			o.CEconItemAttribute.m_iAttributeDefinitionIndex, o.CEconItemAttribute.m_flValue,
			item->paintKit, item->seed, item->wear, item->statTrak, &attr);
		if (attr.ok) {
			++m_lastStats.attrListsRead;
			m_lastStats.attrValuesWritten += attr.written;
			if (attr.changed)
				++m_lastStats.attrValuesChanged;
			if (attr.written == 0)
				++m_lastStats.attrListsEmpty;
		}

		// DEFAULT-ITEM PAINT (the "pistol went to default no-skin" fix): a default/vanilla demo weapon
		// has NO networked paint attribute (attr.hasPaint == false), so the overwrite path can't paint it
		// and the named-setter alone did not render (confirmed: a default Deagle stayed default). For
		// these items, force the legacy fallback path (m_iItemIDHigh = -1) so the client composites from
		// the m_nFallbackPaintKit/Wear/Seed fields we write -- the proven nSkinz mechanism. Scoped to
		// no-paint-attr items with an actual paint pick, so painted demo weapons (the AK) keep their HUD
		// identity via the overwrite path. (Trade-off: the HUD weapon icon for a re-skinned DEFAULT gun
		// may blank, which is acceptable for movie work and only affects guns that were vanilla in the demo.)
		bool useFallbackId = m_fallbackId || (!attr.hasPaint && item->paintKit > 0);

		if (recorded) {
			recorded->AttemptFallback(item->paintKit, item->wear, item->seed, item->statTrak, (uint32_t)xuid, useFallbackId);
			// The normal apply path also repairs a previous fallback item ID high to zero.
			if (!useFallbackId && recorded->observedItemHigh == ~0u) recorded->rawOwned[0].Attempt(0);
		}
		ApplyResult result = CosmeticApplyWrite(
			w,
			itemView,
			w + o.C_EconEntity.m_AttributeManager,
			o.C_EconItemView.m_iItemIDHigh,
			o.C_EconEntity.m_nFallbackPaintKit,
			o.C_EconEntity.m_flFallbackWear,
			o.C_EconEntity.m_nFallbackSeed,
			o.C_EconEntity.m_nFallbackStatTrak,
			o.C_EconItemView.m_iItemDefinitionIndex,
			o.C_EconItemView.m_iItemIDLow,
			o.C_EconItemView.m_iAccountID,
			o.C_EconItemView.m_bInitialized,
			o.C_EconItemView.m_bInitializedTags,
			o.C_EconEntity.m_bAttributesInitialized,
			o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache,
			o.C_EconItemView.m_bInventoryImageRgbaRequested,
			o.C_EconItemView.m_bInventoryImageTriedCache,
			o.C_EconItemView.m_szCurrentLoadCachedFileName,
			o.CAttributeManager.m_iReapplyProvisionParity,
			o.C_CSWeaponBase.m_bVisualsDataSet,
			o.C_CSWeaponBase.m_bClearWeaponIdentifyingUGC,
			o.C_CSWeaponBase.m_nCustomEconReloadEventId,
			(int32_t)item->paintKit,
			item->wear,
			(int32_t)item->seed,
			(int32_t)item->statTrak,
			(uint32_t)xuid,
			knifeDefOverride,
			useFallbackId,
			m_rebuildAuto,
			forceStale,
			m_rebuildFlags);

		if (result.patched)
			++m_lastStats.entitiesPatched;
		if (result.reverted)
			++m_lastStats.entitiesReverted;

		// Whether attrs/mesh need touching this frame (may be every tick during playback).
		bool refreshWarranted = result.needComposite || attr.changed || forceStale || periodicComposite;
		// Direct composite + FP mirror: debounced -- NOT on every attr.changed (demo packet clobber).
		// A UI pick can run an immediate apply and leave a burst pending for the per-frame pump in the
		// same engine frame. Treat that burst like any other retry: firing UpdateComposite twice onto
		// the same entity before the engine's async material work drains corrupts its vector state and
		// later aborts in UtlVectorMemory_FailedAllocation (count=INT_MAX). Only an actual new apply or
		// explicit force bypasses the per-entity cooldown.
		bool compositeWarranted = result.needComposite || forceStale || m_postNudgeMeshRefreshPending;
		if (!compositeWarranted && (attr.changed || periodicComposite)) {
			uint64_t last = 0;
			auto it = m_lastCompositeFrameByIdx.find(i);
			if (it != m_lastCompositeFrameByIdx.end())
				last = it->second;
			if (m_frameCounter >= last + kClobberCompositeCooldownFrames)
				compositeWarranted = true;
		}

		// MISSING-ATTRIBUTE FALLBACK (the attrWritten=0 fix): when the item has no networked paint
		// attribute to overwrite (attr.hasPaint == false -- default/vanilla weapons, picked-up or
		// viewmodel entities), apply the skin through the engine named-setter so it still paints. The
		// composite below renders it. Idempotent, so it is safe even when the composite path also calls
		// the setter; gated to the refresh cadence so it is not run redundantly every frame.
		bool namedSetter = false;
		if (item->paintKit > 0 && !attr.hasPaint && refreshWarranted) {
			if (recorded) recorded->AttemptLocal(item->paintKit, item->wear, item->seed, item->statTrak, true);
			namedSetter = FireNamedSkinAttributes(itemView, item->paintKit, item->wear, item->seed, item->statTrak);
			if (namedSetter)
				++m_lastStats.namedSetterApplied;
		}

		// Optional forced re-composite (OFF by default; see SetRecompose). Writing the fallback
		// fields + invalidating the init flags is not always enough to make a demo entity visually
		// rebuild its skin material -- this calls the weapon's UpdateComposite vtable method when a
		// (re)composite is warranted. SEH-guarded: a wrong index disables recompose, never crashes.
		// fireRebuildCall lets RunFrame keep this opt-in (only when m_recompose is armed) while the
		// manual "rebuild once" path forces a call attempt on every matched entity (forceStale).
		if (fireRebuildCall && (result.needComposite || attr.changed || forceStale)) {
			bool ok = true;
			if (m_vtComposite >= 0) ok = CosmeticSafeVCall(ent, m_vtComposite, m_vtArg);
			if (ok && m_vtCompositeSec >= 0) ok = CosmeticSafeVCall(ent, m_vtCompositeSec, m_vtArg);
			if (!ok) { m_recompose = false; m_recomposeFaulted = true; }
		}

		// Experimental Andromeda-style direct refresh (OFF except "cosmetics composite once"). This is
		// distinct from the vtable-probe path above: it calls the named client.dll functions resolved
		// by byte signatures, using the researched weapon+ownerOffset composite owner pointer. The
		// call is SEH-guarded and reports resolved/called/faulted counters.
		// MODEL SWAP (knife TYPE swap / legacy-vs-CS2 weapon mesh) -- run BEFORE the composite below so
		// the skin re-composites onto the NEW model/mesh. Change-gated like the composite; all calls are
		// SEH-guarded inside CosmeticModelSwap. The def index was already written by CosmeticApplyWrite
		// (knifeDefOverride), so GetStaticData resolves the target knife model. Resolve the original
		// owner's pawn so the first-person HUD weapon receives the same model/mesh refresh.
		// ownerPawn drives ONLY the first-person viewmodel refresh. Gate it to the case where THIS
		// weapon is the pawn's ACTIVE (deployed) weapon: a holstered weapon has no first-person
		// viewmodel, and walking the HUD-arms children while a DIFFERENT weapon is mid-deploy (e.g.
		// switching knife->AK during live playback, where the knife swap re-fires on entity recreation)
		// reaches the other weapon's half-built viewmodel and CRASHES on its next animation. The WORLD
		// (third-person) model/mesh swap below still runs for every matched weapon -- only the
		// first-person viewmodel mirror is gated to the active weapon.
		// Resolve the HOLDING pawn + whether THIS entity is its ACTIVE (deployed) weapon, EVERY matched
		// frame (not gated on a change). The knife swap now fires when the knife BECOMES active even if
		// the profile was changed earlier (while holstered, or during a seek) -- a standing condition, not
		// a one-frame event -- and the diagnostic log reports active-ness. ownerPawn != null <=> active.
		//
		// Resolve via the weapon ENTITY's own live m_hOwnerEntity (who is actually carrying it right
		// now), NOT via CosmeticPawnForSteamId(xuid) -- xuid here is the econ/skin OWNER, a DIFFERENT player than
		// the holder for a picked-up weapon. Looking up the econ owner's pawn and checking ITS active
		// weapon handle against `i` can never succeed for a pickup (the econ owner isn't the one holding
		// this entity -- the picker-upper is), which silently disabled the first-person viewmodel mirror
		// (RefreshViewmodelWeapons, gated on ownerPawn below) for EVERY picked-up weapon regardless of
		// what skin was applied. Confirmed live: reskinning a teammate's picked-up AWP via the Customize
		// UI wrote the new paint attribute every click (uiclick log showed netPaint changing) but the
		// first-person view never updated, because ownerPawn was always null for that entity.
		unsigned char* ownerPawn = nullptr;
		{
			SOURCESDK::CS2::CBaseHandle oh = ent->GetOwnerEntityHandle();
			CEntityInstance* holder = oh.IsValid() ? CosmeticEntFromIndex(oh.GetEntryIndex()) : nullptr;
			unsigned char* pawn = (holder && holder->IsPlayerPawn()) ? (unsigned char*)holder : nullptr;
			if (pawn) {
				SOURCESDK::CS2::CBaseHandle aw = ((CEntityInstance*)pawn)->GetActiveWeaponHandle();
				if (aw.IsValid() && aw.GetEntryIndex() == i)
					ownerPawn = pawn;
			}
		}
		bool knifeFired = false; // set true if the knife model/type swap actually ran this frame (diag)
		// When a legacy/modern weapon selection is established, this is the demo-recorded mask that
		// must be placed in the field immediately before the engine's frame handler. Its native setter
		// then sees the value it was about to restore and early-outs, preserving our renderer choice.
		// The late pass writes the requested mask back after that handler for diagnostics/next-frame use.
		uint64_t preArmRecordedMeshMask = 0;
		// Mesh-group (legacy CS:GO vs modern CS2 model) diagnostics for the per-weapon log line below.
		// A weapon .vmdl carries BOTH meshes; SetMeshGroupMask selects which one renders, and each paint
		// kit is authored for one. dbgMeshLegacy: 1=legacy / 0=modern / -1=unknown (schema lookup failed)
		// / -2=not evaluated this frame. When it is -1 the resolved mask is 0 and the mesh is LEFT
		// UNTOUCHED -- so a skin authored for the OTHER mesh than the one the weapon currently shows will
		// not render (the user's "the model doesn't switch" symptom).
		int dbgMeshLegacy = -2;
		unsigned long long dbgMeshMask = 0;
		int dbgMeshApplied = 0;
		// The setter-only mesh path is safe while paused and must run after the tick nudge re-pauses:
		// each live demo packet restores the recorded paint's mask, so the last live-frame write is
		// otherwise overwritten. ApplyWeaponMeshMask deliberately excludes ResetAnimGraph,
		// PostDataUpdate, and HUD-arms traversal; those were the unsafe queued-renderer calls.
		if (m_modelSwap) {
			bool isKnife = CosmeticCatalog::IsKnifeDef(liveDef);
			// The non-knife weapon mesh-group fix is a setter-only SetMeshGroupMask operation.
			if (isKnife) {
				// Fire when the knife is the owner's ACTIVE weapon + the post-seek stability window elapsed +
				// its def is still the wrong type. NO attr-change-this-frame requirement (after a seek the
				// change and "stable" never coincide -- that is why an edited knife never swapped). A per-owner
				// throttle keyed on the knife entity index + target def fires it ONCE per activation: the engine
				// reverts the def every tick, but we re-fire only on entity recreation (new index) or a new
				// target, never every frame (the per-frame re-swap was the crash).
				// NOTE: no longer gated on ownerPawn (active). The WORLD (third-person) knife model swap must
				// run for a HOLSTERED knife too -- otherwise, after a seek recreated the entity, the skin
				// paint re-applied (ungated) while the model stayed the ORIGINAL knife, showing a mismatched
				// model+skin (e.g. Butterfly mesh + Bayonet's skin) until the knife was next deployed. The
				// first-person viewmodel mirror inside ApplyKnifeModelSwap auto-skips when pawnForViewmodel
				// is null (holstered), and we re-fire on the holstered->active edge to run it once. Safe now
				// that CosmeticAnimFix neutralizes the unloaded-model anim crash the active-only gate avoided.
				if (m_knifeModelSwap && knifeDefOverride > 0) {
					KnifeSwapState& ks = m_knifeSwapState[xuid];
					bool recreated = (ks.entityIndex != i);
					bool newTarget = (ks.swappedDef != knifeDefOverride);
					bool active = (ownerPawn != nullptr);
					bool becameActive = active && !ks.lastActive; // deploy edge -> run the viewmodel mirror once
					// Self-correcting trigger: re-fire whenever the knife's LIVE world model isn't our target.
					// A demo seek can reconstruct the knife in the SAME entity slot (index unchanged) while
					// resetting its model to the original -- the index/active-edge throttles never trip, so the
					// knife kept its ORIGINAL model with the swapped skin until it was re-deployed. Comparing the
					// actual rendered model path catches that case (and any other silent revert). Settles to
					// false in one frame once SetModel takes, so it does not thrash.
					bool modelMismatch = false;
					{
						char targetModel[260];
						if (ResolveKnifeModelPath(knifeDefOverride, itemView, targetModel, sizeof(targetModel)) && targetModel[0]) {
							char liveKnifeModel[160];
							CosmeticReadEntityModelPath(ent, liveKnifeModel, sizeof(liveKnifeModel));
							modelMismatch = liveKnifeModel[0] && 0 != _stricmp(liveKnifeModel, targetModel);
						}
					}
					if (recreated || newTarget || becameActive || modelMismatch) {
						// Always-flushed re-fire decision. recreated=1 (a NEW entity index for the same owner)
						// is the seek/quick-switch case: the engine destroyed+recreated the knife entity, so
						// the swap re-fires onto the rebuilt entity. framesSinceSeek shows how settled the
						// world is; allowSwap=0 means SUPPRESSED until the (short) post-seek window elapses.
						// active=0 = holstered (world model swap only; viewmodel mirror skipped).
						if (MvmDebugLog_Active())
							MvmDebugLog_LinefAlways("knife.fire",
								"xuid=%llu idx=%d prevIdx=%d liveDef=%d targetDef=%d prevDef=%d recreated=%d newTarget=%d active=%d modelMismatch=%d allowSwap=%d framesSinceSeek=%d -> %s",
								(unsigned long long)xuid, i, ks.entityIndex, liveDef, knifeDefOverride, ks.swappedDef,
								recreated ? 1 : 0, newTarget ? 1 : 0, active ? 1 : 0, modelMismatch ? 1 : 0, allowKnifeSwap ? 1 : 0, m_framesSinceSeek,
								allowKnifeSwap ? "FIRE" : "SUPPRESS-settle");
						if (allowKnifeSwap) {
							uint64_t mask = ResolveMeshMask(item->paintKit, /*knife=*/true,
								m_meshLegacyMode, m_maskModern, m_maskLegacy);
							if (mask == 0) mask = m_maskModern; // a knife model always needs a mesh group
							// ownerPawn null when holstered -> world swap runs, viewmodel mirror is skipped.
							knifeFired = ApplyKnifeModelSwap(w, itemView, ownerPawn, knifeDefOverride, mask, i);
							if (knifeFired) {
								++m_lastStats.knifeModelsApplied;
								// Same rule as the legacy/CS2 mesh switch: a composite in the swap frame lands on the old
								// knife's materials (measured 2026-10-09: M9 Marble Fade picked over a Karambit rendered as a
								// plain blade). Hold composites, then run one on the new model once it settles.
								m_meshFollowupCompositeDue[i] = m_frameCounter + kMeshSwitchSettleFrames;
								// A swap on a PLAYING frame (e.g. the deploy-edge re-fire when the knife is redrawn) drops
								// the composite and no paused frame follows on its own: owe it, which pauses for one pass.
								if (!fireDirectComposite)
									m_compositeOwedIdx.insert(i);
								ks.entityIndex = i;
								ks.swappedDef = knifeDefOverride;
							}
						}
					}
					ks.lastActive = active; // track each matched frame so a holstered->active edge fires once
				}
			} else if (item->paintKit >= 0) {
				// paintKit 0 is the vanilla finish, which renders on the modern mesh: picking "Default"
				// on a gun whose demo skin is legacy must switch it back (ResolveMeshMask maps 0 -> modern).
				// Mesh-group (legacy CS:GO vs modern CS2 model) selection, only actually WRITTEN when the
				// LIVE mesh mask differs from our target, so we correct an engine revert (the user's "the
				// model doesn't switch when I pick an older/newer skin" bug) without thrashing the
				// renderable when it is already right.
				uint64_t mask = ResolveMeshMask(item->paintKit, /*knife=*/false,
					m_meshLegacyMode, m_maskModern, m_maskLegacy);
				dbgMeshMask = mask;
				// Surface the raw legacy decision in the log (only while debugging -- it walks the econ
				// schema). -1 here means the lookup could not classify this paint kit, so the mesh is left
				// untouched and the skin may not render on whatever mesh the weapon currently shows.
				if (MvmDebugLog_Active())
					dbgMeshLegacy = item->paintKit > 0 ? PaintKitLegacyModel(item->paintKit) : 0;
				if (mask != 0) {
					uint64_t liveMask = ReadEntityMeshGroupMask(ent);
					WeaponMeshState& meshState = m_weaponMeshState[i];
					const bool selectionChanged =
						meshState.entity != (uintptr_t)w || meshState.mask != mask;
					// Exactly one native switch per live entity/target. Repeating it after the demo
					// restores its recorded field value continuously invalidates the async material.
					bool needMesh = selectionChanged && m_meshSwapStable;
					if (needMesh) {
						const uint64_t recordedMask = recorded ? recorded->mesh :
							(meshState.entity == (uintptr_t)w && meshState.recordedMask != 0
								? meshState.recordedMask : liveMask);
						if (recorded) recorded->AttemptMesh(mask);
						const bool switched = ApplyWeaponMeshMask(w, mask, ownerPawn, i);
						// The forced scratch/target toggle can partially fail even when the pre-call
						// field matched. Establish the renderer memo only after both native calls succeed.
						if (switched) {
							meshState.entity = (uintptr_t)w;
							meshState.mask = mask;
							meshState.recordedMask = recordedMask;
						}
						if (switched) {
							compositeWarranted = true;
							m_meshFollowupCompositeDue[i] = m_frameCounter + kMeshSwitchSettleFrames;
						}
						++m_lastStats.weaponMeshFixed;
						dbgMeshApplied = 1;
					}
					// Queue only established selections. The late pass is a non-invalidating field hold,
					// never a substitute for the initial native renderer switch.
					if (meshState.entity == (uintptr_t)w && meshState.mask == mask) {
						if (recorded) {
							recorded->AttemptMesh(mask);
							meshState.recordedMask = recorded->mesh;
						}
						m_lateMeshReassert.push_back({(uintptr_t)w, (uint32_t)ent->GetHandle().ToInt(), mask});
						if (meshState.recordedMask != 0 && meshState.recordedMask != mask)
							preArmRecordedMeshMask = meshState.recordedMask;
					}
					if (MvmDebugLog_Active() && (needMesh || dbgMeshLegacy < 0)) {
						uint64_t postMask = ReadEntityMeshGroupMask(ent);
						MvmDebugLog_LinefAlways("cosmetics.weapon.mesh",
							"idx=%d def=%d wantPaint=%d wantLegacy=%d wantMask=%llu preMask=%llu postMask=%llu applied=%d paintChanged=%d",
							i, liveDef, item->paintKit, dbgMeshLegacy,
							(unsigned long long)mask, (unsigned long long)liveMask,
							(unsigned long long)postMask, dbgMeshApplied, attr.changed ? 1 : 0);
					}
				} else if (MvmDebugLog_Active()) {
					MvmDebugLog_LinefAlways("cosmetics.weapon.mesh",
						"SKIP idx=%d def=%d wantPaint=%d legacyLookup=-1 (mesh untouched -- skin may not render)",
						i, liveDef, item->paintKit);
				}
			}
		}

		// Never composite a holstered world weapon. The documented working path targets the actually
		// deployed weapon; an inactive entity can accept the call and corrupt async material state.
		// The composite only runs on a deployed weapon, so a change to a holstered one is owed until
		// it is drawn. While the demo PLAYS the composite cannot run, so ask for the pause-composite-resume
		// pass; while paused it runs right here.
		// A native mesh switch is settling on this entity: hold every composite until it has, then run
		// exactly one on a paused frame with the weapon drawn. A composite in the switch frame lands on
		// the materials of the group being left, and overlapping composites abandon each other.
		{
			auto settle = m_meshFollowupCompositeDue.find(i);
			if (settle != m_meshFollowupCompositeDue.end()) {
				if (m_frameCounter < settle->second || !fireDirectComposite || !ownerPawn) {
					compositeWarranted = false;
					// Holstered: the gun is owed its composite when it is next drawn (paused: right here once
					// settled; playing: the owed path below pauses for it).
					if (!ownerPawn)
						m_compositeOwedIdx.insert(i);
					// Settled on a drawn gun while the demo plays: nothing pays it until a pause, so a
					// theme (everyone dressed at once) asks for its batched one.
					else if (m_theme && !fireDirectComposite && m_frameCounter >= settle->second)
						RequestOwedCompositePause(i);
				} else {
					compositeWarranted = true;
					m_meshFollowupCompositeDue.erase(settle);
					if (MvmDebugLog_Active())
						MvmDebugLog_LinefAlways("cosmetics.composite", "idx=%d settled after mesh switch", i);
				}
			}
		}
		// Composite trace: for a short window after a pick, log the world weapon's and its HUD copies'
		// mesh mask and composite counts, so a first-person gun that misses its skin shows where.
		if (MvmDebugLog_Active()) {
			if (attr.changed || result.needComposite)
				m_compositeTraceUntil[i] = m_frameCounter + 90;
			auto tr = m_compositeTraceUntil.find(i);
			if (tr != m_compositeTraceUntil.end()) {
				if (m_frameCounter > tr->second) m_compositeTraceUntil.erase(tr);
				else if ((m_frameCounter % 3) == 0) LogWeaponCompositeTrace(w, ownerPawn, i, item->paintKit);
			}
		}
		const bool compositeOwed = m_compositeOwedIdx.count(i) != 0;
		if (compositeOwed && ownerPawn && m_meshFollowupCompositeDue.count(i) == 0) compositeWarranted = true;
		if (compositeWarranted && !ownerPawn && (result.needComposite || attr.changed || forceStale))
			m_compositeOwedIdx.insert(i);
		if (compositeOwed && ownerPawn && !fireDirectComposite) {
			m_compositeOwedIdx.erase(i);
			RequestOwedCompositePause(i);
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("cosmetics.nudge", "owed composite idx=%d: weapon drawn while playing, pause-composite requested", i);
		}
		// A surviving seek or a rapid second pick must not overlap the previous native material job.
		// The mesh follow-up used to bypass this clock after seeks because its memo was cleared.
		if (recorded && compositeWarranted && recorded->lastCompositeFrame
			&& m_frameCounter < recorded->lastCompositeFrame + kClobberCompositeCooldownFrames) {
			const uint64_t due = recorded->lastCompositeFrame + kClobberCompositeCooldownFrames;
			auto& followup = m_meshFollowupCompositeDue[i];
			followup = (std::max)(followup, due);
			compositeWarranted = false;
		}
		const bool waitingMaterial = m_meshFollowupCompositeDue.count(i) || m_compositeOwedIdx.count(i);
		if (ownerPawn && m_autoComposite && waitingMaterial && DemoIsPaused()) {
			m_weaponMaterialBusy = true;
			if (m_nudgePhase == 2) m_pauseCompositeDoneFrame = 0;
		}
		if (fireDirectComposite && compositeWarranted && ownerPawn) {
			// The composite is built against the mesh group the field holds right now. Between frames the
			// field carries the demo-recorded group (the pre-arm below), so without this a CS2 finish picked
			// over a legacy demo skin (or the reverse) was baked for the wrong group and the first-person
			// gun rendered bare (measured 2026-10-09 via cosmetics.composite.trace: world mask read 2 at the
			// composite while the target was 1). Non-invalidating field write; the renderer already switched.
			if (dbgMeshMask != 0) {
				// Direct field holds can run before the post-seek native selection settles.
				// Keep their target owned without inventing a scratch-mask write.
				if (recorded) recorded->AttemptMeshTarget(dbgMeshMask);
				HoldWeaponMeshMaskDirect(w, dbgMeshMask);
			}
			if (recorded) {
				recorded->AttemptLocal(item->paintKit, item->wear, item->seed, item->statTrak, false);
				// Even a fault after a partial native call may leave material work in flight.
				recorded->lastCompositeFrame = m_frameCounter;
			}
			DirectCompositeResult direct = FireDirectCompositeRefresh(
				w,
				itemView,
				o.C_EconItemView.m_bRestoreCustomMaterialAfterPrecache,
				m_compositeOwnerOffset,
				(int32_t)item->paintKit,
				item->wear,
				(int32_t)item->seed);
			if (direct.resolved)
				m_lastStats.directCompositeResolved = 1;
			if (direct.called) {
				++m_lastStats.directCompositeCalls;
				m_lastCompositeFrameByIdx[i] = m_frameCounter;
				m_lastDirectCompositeFrame = m_frameCounter; // SeekRefreshPending waits out its material drain
				{
					const uint32_t handle = (uint32_t)ent->GetHandle().ToInt();
					bool watched = false;
					for (const MaterialWatch& m : m_materialWatch)
						watched = watched || (m.entity == (uintptr_t)w && m.handle == handle);
					if (!watched) m_materialWatch.push_back({ (uintptr_t)w, handle });
				}
				m_compositeOwedIdx.erase(i);
				if (m_nudgePhase == 2) m_pauseCompositeDoneFrame = m_frameCounter;
				// Vanilla finish: the world weapon just released its composite, but the first-person HUD model
				// keeps its own copy of the previous skin until a new composite replaces it -- drop it too.
				if (item->paintKit == 0)
					ReleaseViewmodelComposites(ownerPawn, w, i);
				if (MvmDebugLog_Active()) {
					const char* why = result.needComposite ? "apply" : (forceStale ? "force" :
						(periodicComposite ? "burst" : "clobber"));
					MvmDebugLog_Linef("cosmetics.composite", "idx=%d def=%d paint=%d reason=%s",
						i, liveDef, item->paintKit, why);
				}
			}
			if (direct.faulted)
				m_lastStats.directCompositeFaulted = 1;
		}

		// Run after the one-shot composite so that call observes the requested mask, but before the
		// engine's own frame handler. Pre-arming the recorded value makes the engine's native restore
		// setter take its equal-value early-out, so it cannot switch the renderer back or invalidate
		// the material. ReassertMeshMasksLate restores the requested field value after the handler.
		if (preArmRecordedMeshMask != 0)
			HoldWeaponMeshMaskDirect(w, preArmRecordedMeshMask);

		// REMOVED: MirrorWeaponCosmeticsToViewmodel call. Root-caused against the CS2-OFFSETS schema
		// dump (client_dll.json): the first-person HUD-arms weapon is a C_CS2HudModelWeapon, whose parent
		// chain is C_CS2HudModelWeapon -> C_CS2HudModelBase -> C_LateUpdatedAnimating -> CBaseAnimGraph ->
		// C_BaseModelEntity -> C_BaseEntity. C_EconEntity ALSO derives from CBaseAnimGraph but is a
		// SIBLING branch, not an ancestor -- C_CS2HudModelWeapon has NO C_EconEntity fields at all
		// (no m_AttributeManager, no fallback paint/wear/seed, nothing). This function computed
		// "vmWeapon + o.C_EconEntity.m_AttributeManager" and "vmWeapon + m_compositeOwnerOffset" as if it
		// were a real econ weapon, which are both meaningless reads into unrelated C_CS2HudModelWeapon
		// memory. Confirmed live: paintBefore/paintAfter always read -1 (garbage "item view" pointer) and
		// the direct-composite call always faulted (fault=1); re-firing it more often (to fix a separate
		// same-mesh-group repaint bug) triggered a ~154GB allocation attempt and a fatal OOM crash.
		//
		// The actual fix for "first person doesn't show the new skin" is RefreshViewmodelWeapons (called
		// from ApplyWeaponMeshMask above via ownerPawn) -- it only touches SetModel/SetMeshGroupMask/
		// ResetAnimGraph/PostDataUpdate, all on the C_BaseModelEntity/CBaseAnimGraph ancestry that
		// C_CS2HudModelWeapon legitimately shares with real weapons. No econ-attribute write is needed on
		// the HUD-arms child at all -- the composited material is a shared resource keyed by model path +
		// mesh group, already generated correctly by the WORLD weapon's own (correctly-offset) composite
		// call above; the viewmodel just needs to be told to re-pick it up, which the mesh/model mirror
		// already does.

		// Per-matched-weapon diagnostic (deduped per unique payload). The decisive field is
		// attrWritten: a demo weapon's REAL skin lives in m_NetworkedDynamicAttributes (def 6 paint /
		// 7 seed / 8 wear) -- attrWritten=0 means that attribute is NOT present on this item, so the
		// paint cannot be written there (the skin will not render via the networked-attr path no matter
		// what). attrOk=0 means the attribute vector itself could not be read. needComposite reflects
		// whether a value differed this frame (drives the re-composite / model-swap fire).
		if (MvmDebugLog_Active()) {
			// idx + class distinguish the multiple entities one weapon can have (world model vs first-person
			// viewmodel vs a picked-up copy). active=1 means it is the owner's deployed weapon. liveModel is
			// the ACTUAL rendered model path -- a knife swap that did not take shows the original model here.
			// attrWritten=0 = this entity has NO networked paint attribute to write (can't be skinned via the
			// networked-attr path) -- the typical attrWritten=0 entity is the viewmodel / a picked-up weapon.
			const char* cls = ent->GetClassName();
			char liveModel[160];
			CosmeticReadEntityModelPath(ent, liveModel, sizeof(liveModel));
			// LIVE rendered mesh-group mask (post-apply). Compare across frames: if it != our meshMask on
			// a later (meshApplied=0) frame, the engine reverted it; the value on a CORRECTLY-rendering
			// weapon is the real legacy/modern mask to target.
			unsigned long long liveMeshMask = (unsigned long long)ReadEntityMeshGroupMask(ent);
			// writePath shows HOW the skin was written: "overwrite" = an existing networked def-6 paint
			// attr was overwritten; "namedSetter" = no paint attr existed so the engine named-setter
			// fallback applied it (the attrWritten=0 fix); "none" = neither (e.g. paintKit 0 / vanilla).
			const char* writePath = attr.hasPaint ? "overwrite"
				: (useFallbackId ? (namedSetter ? "fallbackId+named" : "fallbackId")
					: (namedSetter ? "namedSetter" : "none"));
			MvmDebugLog_Linef("cosmetics.weapon",
				"idx=%d cls='%s' xuid=%llu liveDef=%d active=%d isKnife=%d knifeSwapDef=%d knifeFired=%d "
				"allowKnife=%d paint=%d wear=%.3f seed=%d stat=%d attrOk=%d attrWritten=%d hasPaint=%d "
				"writePath=%s attrChanged=%d patched=%d needComposite=%d "
				"meshMode=%d meshLegacy=%d meshMask=%llu meshApplied=%d liveMeshMask=%llu liveModel='%s'",
				i, cls ? cls : "?", (unsigned long long)xuid, liveDef, ownerPawn ? 1 : 0,
				CosmeticCatalog::IsKnifeDef(liveDef) ? 1 : 0, knifeDefOverride, knifeFired ? 1 : 0,
				allowKnifeSwap ? 1 : 0, item->paintKit, item->wear, item->seed, item->statTrak,
				attr.ok ? 1 : 0, attr.written, attr.hasPaint ? 1 : 0, writePath, attr.changed ? 1 : 0,
				result.patched ? 1 : 0, result.needComposite ? 1 : 0,
				m_meshLegacyMode, dbgMeshLegacy, dbgMeshMask, dbgMeshApplied, liveMeshMask, liveModel);
		}
	}

	// GLOVES + AGENT are pawn-level (not econ weapon entities) and are applied separately in
	// ApplyPawnCosmetics(), called from RunFrame after this weapon pass. See CosmeticModelSwap.cpp.
	return m_lastStats.entitiesMatched;
}

bool CosmeticOverrideSystem::ModelSwapResolved() const {
	return ResolveModelSwapFns().CoreOk();
}

// Pawn-level cosmetics: walk player controllers, resolve each one's pawn + owner SteamID, and apply
// the agent (player) model swap + glove model for any profiled player. Glove apply is change-gated
// (re-fires for a few frames after a glove/paint change, a respawn, or the engine setting
// m_bNeedToReApplyGloves) so the body group is not rebuilt every frame. Agent swap is hash-gated so
// SetModel only fires when the chosen model changes (or the engine recreated the pawn). All entity
// writes are SEH-guarded inside CosmeticModelSwap.
int CosmeticOverrideSystem::ApplyPawnCosmetics() {
	if (!m_modelSwap) {
		if (MvmDebugLog_Active())
			MvmAgentLog("H1", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics", "modelswap_disabled", "");
		return 0;
	}

	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (MvmDebugLog_Active() && o.C_CSPlayerPawn.m_EconGloves == 0) {
		MvmAgentLog("H4", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics", "econ_gloves_offset_missing",
			"\"m_EconGloves\":0");
	}
	const uint64_t spectatedSteamId = CurrentSpectatedSteamId();
	if (spectatedSteamId != 0)
		m_lastSpectatedSteamId = spectatedSteamId;
	if (m_gloveBypassFrames > 0)
		--m_gloveBypassFrames;
	// Profiled players only get glove writes while spectated; re-arm whenever the spectate target
	// changes so switching back to a player always runs a fresh burst (not only on first UI set).
	if (spectatedSteamId != 0 && spectatedSteamId != m_lastGloveSpectatedSid) {
		if (MvmDebugLog_Active()) {
			char data[128];
			std::snprintf(data, sizeof(data),
				"\"from\":%llu,\"to\":%llu",
				(unsigned long long)m_lastGloveSpectatedSid, (unsigned long long)spectatedSteamId);
			MvmAgentLog("H8", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics",
				"glove_rearm_spectate_change", data);
		}
		m_lastGloveSpectatedSid = spectatedSteamId;
		CosmeticProfile* sp = m_store.Find(spectatedSteamId);
		if (sp && sp->gloves.set && sp->gloves.defIndex > 0) {
			GloveApplyState& gst = m_gloveState[spectatedSteamId];
			gst.frames = 4;
			gst.pawnStable = 0;
		}
	}
	PruneThemePawns();
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ctrl = CosmeticEntFromIndex(i);
		if (!ctrl || !ctrl->IsPlayerController())
			continue;
		uint64_t steamId = ctrl->GetSteamId();
		CosmeticProfile* prof = steamId ? m_store.Find(steamId) : nullptr;
		auto bot = m_botAgents.find(ctrl->GetHandle().ToInt());
		BotAgentOverride* botAgent = !steamId && bot != m_botAgents.end() ? &bot->second : nullptr;
		const CosmeticAgent* agent = prof ? &prof->agent : botAgent ? &botAgent->agent : nullptr;
		if ((!agent || !agent->set) && (!prof || !prof->gloves.set))
			continue;

		SOURCESDK::CS2::CBaseHandle ph = ctrl->GetPlayerPawnHandle();
		CEntityInstance* pawnEnt = ph.IsValid() ? CosmeticEntFromIndex(ph.GetEntryIndex()) : nullptr;
		CosmeticThemePawnIdentity pawnIdentity;
		int pawnHealth = 0;
		if (CosmeticReadThemePawnIdentity(ctrl, pawnEnt, &pawnIdentity, &pawnHealth) != 1)
			continue;
		unsigned char* pawn = (unsigned char*)pawnEnt;
		++m_lastStats.pawnsScanned;

		// AGENT (player model). Gate on both model hash and pawn pointer so a seek/entity recreation
		// always reapplies even when the requested model path did not change.
		// A dead pawn retains the agent model applied while alive. Reasserting SetModel after
		// death restarts ragdoll physics.
		if (pawnHealth > 0 && agent && agent->set && !agent->model.empty()) {
			uint64_t h = 1469598103934665603ULL; // FNV-1a of the model path
			for (char c : agent->model) { h ^= (unsigned char)c; h *= 1099511628211ULL; }
			AgentApplyState& state = botAgent ? botAgent->applied : m_agentState[steamId];
			if (state.hash != h || state.pawn != (uintptr_t)pawn) {
				if ((!m_theme || CaptureThemePawn(ctrl, pawnEnt, agent->model.c_str()))
					&& ApplyAgentModel(pawn, agent->model.c_str())) {
					++m_lastStats.agentsApplied;
					state = { h, (uintptr_t)pawn };
					if (MvmDebugLog_Active()) {
						char actual[256] = {};
						CosmeticReadEntityModelPath(pawnEnt, actual, sizeof(actual));
						MvmDebugLog_Linef("cosmetics.agent.applied",
							"controller=%u steam=%llu pawn=%p modelNameOffset=0x%llx want='%s' actual='%s'",
							ctrl->GetHandle().ToInt(), (unsigned long long)steamId, pawn,
							(unsigned long long)o.ModelChain.m_ModelName, agent->model.c_str(), actual);
					}
				}
			} else if ((m_lastStats.frame % 32) == 0) {
				// Re-assert only when the engine actually reverted the model: an unconditional SetModel
				// rebuilds the pawn's model + anim graph, which with a whole server dressed (UwU) hitched.
				char actual[256] = {};
				CosmeticReadEntityModelPath(pawnEnt, actual, sizeof(actual));
				if ((!actual[0] || 0 != _stricmp(actual, agent->model.c_str()))
					&& (!m_theme || CaptureThemePawn(ctrl, pawnEnt, agent->model.c_str())))
					ApplyAgentModel(pawn, agent->model.c_str());
			}
		}
		if (!prof) continue; // bot overrides affect agents only, never SteamID-owned econ items

		// GLOVES. Change-gated multi-frame apply.
		if (prof->gloves.set && prof->gloves.defIndex > 0 && o.C_CSPlayerPawn.m_EconGloves) {
			GloveApplyState& st = m_gloveState[steamId];
			const bool uiBypass = (m_gloveBypassSid == steamId && m_gloveBypassFrames > 0);
			const bool spectatedMatch = (spectatedSteamId != 0 && steamId == spectatedSteamId);
			const bool stickyBurst = (spectatedSteamId == 0 && m_lastSpectatedSteamId == steamId && st.frames > 0);
			if (!spectatedMatch && !uiBypass && !stickyBurst) {
				if (MvmDebugLog_Active() && (m_lastStats.frame % 64) == 0) {
					char data[200];
					std::snprintf(data, sizeof(data),
						"\"steamId\":%llu,\"spectated\":%llu,\"uiBypass\":%d,\"sticky\":%d,\"framesLeft\":%d",
						(unsigned long long)steamId, (unsigned long long)spectatedSteamId,
						uiBypass ? 1 : 0, stickyBurst ? 1 : 0, st.frames);
					MvmAgentLog("H7", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics",
						"glove_skip_not_spectated", data);
				}
				continue;
			}

			// Track pawn recreation before the team gate -- side switches briefly report team=0 and
			// must still re-arm the burst when the pawn entity is recreated.
			if (st.pawn != (uintptr_t)pawn) {
				const bool armRecreate = spectatedMatch || uiBypass;
				st.pawn = (uintptr_t)pawn;
				st.pawnStable = 0;
				if (armRecreate) {
					st.frames = 4;
					if (MvmDebugLog_Active()) {
						char data[96];
						std::snprintf(data, sizeof(data), "\"steamId\":%llu", (unsigned long long)steamId);
						MvmAgentLog("H8", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics",
							"glove_rearm_pawn_recreate", data);
					}
				} else {
					st.frames = 0;
				}
			}

			uint8_t team = 0;
			if (o.C_BaseEntity.m_iTeamNum) {
				__try { team = *(uint8_t*)(pawn + o.C_BaseEntity.m_iTeamNum); }
				__except (1) { team = 0; }
			}
			// Spectate/player-switch recreates the pawn with team=0 briefly -- defer writes until valid.
			if (team != 2 && team != 3) {
				if (MvmDebugLog_Active() && (m_lastStats.frame % 32) == 0) {
					char data[128];
					std::snprintf(data, sizeof(data),
						"\"steamId\":%llu,\"team\":%u,\"framesLeft\":%d",
						(unsigned long long)steamId, (unsigned)team, st.frames);
					MvmAgentLog("H9", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics",
						"glove_skip_team_unready", data);
				}
				continue;
			}
			if (st.pawnStable < 4) {
				++st.pawnStable;
				continue;
			}

			if (st.frames == 0 && (spectatedMatch || uiBypass)) {
				const int liveDef = CosmeticSafeReadGloveDefIndex(pawn);
				bool engineWantsReapply = false;
				if (o.C_CSPlayerPawn.m_bNeedToReApplyGloves) {
					__try {
						engineWantsReapply = *(bool*)(pawn + o.C_CSPlayerPawn.m_bNeedToReApplyGloves);
					} __except (1) {
						engineWantsReapply = false;
					}
				}
				if (liveDef != prof->gloves.defIndex || engineWantsReapply) {
					st.frames = 4;
					if (MvmDebugLog_Active()) {
						char data[160];
						std::snprintf(data, sizeof(data),
							"\"steamId\":%llu,\"liveDef\":%d,\"wantDef\":%d,\"engineReapply\":%d",
							(unsigned long long)steamId, liveDef, prof->gloves.defIndex,
							engineWantsReapply ? 1 : 0);
						MvmAgentLog("H8", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics",
							"glove_rearm_live_mismatch", data);
					}
				}
			}

			uint32_t wearBits = 0;
			std::memcpy(&wearBits, &prof->gloves.wear, sizeof(wearBits));
			uint64_t sig = 1469598103934665603ULL;
			const uint32_t sigParts[] = { (uint32_t)prof->gloves.defIndex,
				(uint32_t)prof->gloves.paintKit, (uint32_t)prof->gloves.seed, wearBits, (uint32_t)team };
			for (uint32_t part : sigParts) { sig ^= part; sig *= 1099511628211ULL; }
			float spawn = CosmeticSafeReadEntityFloat(pawn, o.C_CSPlayerPawn.m_flLastSpawnTimeIndex, -1.0f);

			if (st.sig != sig || st.lastSpawn != spawn) {
				st.frames = 4; // apply over a few frames (gloves rebuild across frames)
				st.sig = sig;
				st.lastSpawn = spawn;
			}
			if (st.frames > 0) {
				if (m_gloveApplyBudget <= 0)
					continue;
				--m_gloveApplyBudget;
				bool composePaint = (st.frames == 4) && m_constructPaintBudget > 0;
				if (composePaint)
					--m_constructPaintBudget;
				if (MvmDebugLog_Active()) {
					char data[360];
					std::snprintf(data, sizeof(data),
						"\"steamId\":%llu,\"wantDef\":%d,\"wantPaint\":%d,\"framesLeft\":%d,\"pawn\":%llu,"
						"\"team\":%u,\"composePaint\":%d,\"pawnStable\":%d",
						(unsigned long long)steamId, prof->gloves.defIndex, prof->gloves.paintKit, st.frames,
						(unsigned long long)(uintptr_t)pawn, (unsigned)team, composePaint ? 1 : 0, st.pawnStable);
					MvmAgentLog("H6", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics", "glove_apply_fire", data);
				}
				if (ApplyGloveModel(pawn, prof->gloves.defIndex, prof->gloves.paintKit,
						prof->gloves.wear, prof->gloves.seed, (uint32_t)steamId, composePaint)) {
					++m_lastStats.glovesApplied;
					// Paused: the pawn think that rebuilds the third-person glove model does not run.
					if (DemoIsPaused() && UpdatePawnGlovesNow(pawn, /*onlyIfFlagged=*/true)
						&& MvmDebugLog_Active())
						MvmDebugLog_LinefAlways("cosmetics.glove", "paused glove model rebuild steam=%llu def=%d paint=%d",
							(unsigned long long)steamId, prof->gloves.defIndex, prof->gloves.paintKit);
				}
				--st.frames;
			} else if (MvmDebugLog_Active() && (m_lastStats.frame % 128) == 0) {
				char data[192];
				std::snprintf(data, sizeof(data),
					"\"steamId\":%llu,\"wantDef\":%d,\"framesLeft\":0,\"sig\":%llu",
					(unsigned long long)steamId, prof->gloves.defIndex, (unsigned long long)st.sig);
				MvmAgentLog("H6", "CosmeticOverrideSystem.cpp:ApplyPawnCosmetics", "glove_gate_idle", data);
			}
		}
	}

	m_lastStats.modelSwapResolved = ResolveModelSwapFns().CoreOk() ? 1 : 0;
	return m_lastStats.pawnsScanned;
}

} // namespace Filmmaker
