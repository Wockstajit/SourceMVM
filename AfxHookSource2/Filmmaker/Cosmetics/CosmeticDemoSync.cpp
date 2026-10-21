// Cosmetic demo synchronization: demo-seek detection and the post-apply "tick nudge"
// state machine. A cosmetic change while the demo is PAUSED does not re-evaluate
// renderables on its own; the engine-native fix is to briefly resume, let ~10 ticks
// render, and re-pause (MaybeFireTickNudge). Seeks invalidate every per-pawn apply gate
// and start a settle window sized to the jump (OnDemoSeekDetected) so the override
// system does not fight the engine while the entity list is being rebuilt.

#include <Windows.h> // GetTickCount64 (theme composite batching)

#include "CosmeticOverrideSystem.h"

#include "../Debug/MvmDebugLog.h"
#include "../Movie/DemoScrub.h"               // a scrub owns the play state: no seek hold
#include "../Render/RenderSessionDriver.h"    // a render holds its own landing: no seek hold

#include "../../MirvTime.h" // g_MirvTime.GetCurrentDemoTick for the tick nudge

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h" // ISource2EngineToClient (demo seek)

// Engine pointer (same one CameraPath/MovieMode use) for the demo re-seek that forces a renderable
// rebuild after a cosmetic change (the user-requested "nudge a few ticks" lever).
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// One-shot composite burst after a skin apply (avoids the old 150-frame hammer during playback).
constexpr int kCompositeBurstShots = 2;

// Seek refresh (SeekRefreshPending / phase 3). A composite's material work drains asynchronously
// after the call, so the refresh counts as done only after this many quiet frames -- the same
// allowance phase 2 gives the live-pick composite before it resumes.
constexpr uint64_t kSeekRefreshDrainFrames = 24;
// The longest a seek hold keeps the demo paused. A stacked seek settle (128) plus the mesh switch,
// composite and drain fits several times over; past this the demo plays on regardless.
constexpr uint64_t kSeekHoldMaxFrames = 600;
// How long the hold waits for its demo_pause to take before it gives up and lets playback run.
constexpr uint64_t kSeekHoldPauseWaitFrames = 30;

// demo_pause toggles. Calling it while the demo is already paused starts playback.
bool DemoIsPaused() {
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	return demo && demo->IsDemoPaused();
}

} // namespace

void CosmeticOverrideSystem::RequestCompositeRefresh() {
	m_compositeBurstRemaining = kCompositeBurstShots;
}

void CosmeticOverrideSystem::AbortActiveTickNudge(const char* reason) {
	m_pendingNudge = false;
	m_pendingNudgeOwedOnly = false;
	m_owedPauseIdx.clear();
	m_postNudgeMeshRefreshPending = false;
	if (m_nudgePhase == 2) {
		// A queued pick still needs this pause after a seek. Its eventual commit owns
		// resuming playback; resuming here can race that commit and force a live timeout.
		const bool resume = m_livePicks.empty();
		if (resume && g_pEngineToClient)
			g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
		m_nudgePhase = 0;
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.nudge", "ABORT live-composite %s resume=%d queued=%zu frame=%llu",
				reason ? reason : "?", resume ? 1 : 0, m_livePicks.size(), (unsigned long long)m_frameCounter);
		return;
	}
	if (m_nudgePhase != 1)
		return;
	// The play-out may not have started (a seek aborted it on the same frame as demo_resume).
	// demo_pause toggles, so sending it while still paused would start the demo and leave it running.
	if (m_nudgeWasPaused && m_nudgeSawResume && !DemoIsPaused() && g_pEngineToClient)
		g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true);
	m_nudgePhase = 0;
	m_nudgeWasPaused = false;
	m_nudgeSawResume = false;
	m_nudgePauseIssued = false;
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.nudge", "ABORT %s frame=%llu", reason ? reason : "?",
			(unsigned long long)m_frameCounter);
}

bool CosmeticOverrideSystem::SeekRefreshPending() const {
	if (!m_store.Enabled() || !OffsetsAvailable() || m_store.Empty())
		return false;
	// Entities still rebuilding, the post-seek composite not yet paid, its burst still firing, or the
	// mesh selections not yet allowed (they precede their own composite).
	if (m_seekSettleFrames > 0 || m_seekCompositePending || m_compositeBurstRemaining > 0 || !m_meshSwapStable)
		return true;
	// A deployed weapon is still waiting for its composite (a mesh switch settling, or owed).
	if (m_weaponMaterialBusy)
		return true;
	return m_lastDirectCompositeFrame != 0 && m_frameCounter - m_lastDirectCompositeFrame < kSeekRefreshDrainFrames;
}

void CosmeticOverrideSystem::OnDemoSeekDetected(int tickJump) {
	// A seek stacked onto a running hold (round-skip spam) keeps that hold: its settle restarts below
	// and the hold simply lasts until the newest landing is redrawn.
	const bool holding = m_nudgePhase == 3;
	if (!holding)
		AbortActiveTickNudge("seek");
	// Short seeks may preserve themed pawns. Their full identities and live owned-model observations
	// decide whether to retain the original; no saved pointer is dereferenced across the seek.
	// A short seek may preserve the live weapon and its in-flight composite. Reconcile each field
	// against owned write intents after settling; full identities retire recreated weapons safely.
	for (auto& entry : m_recordedWeapons) {
		entry.second.restored = false;
		entry.second.meshSelected = false;
		entry.second.dueFrame = 0;
	}
	m_weaponMaterialBusy = false;
	InvalidateAgentStates();
	m_gloveState.clear();
	m_lastGloveSpectatedSid = 0;
	m_knifeSwapState.clear();
	m_weaponMeshState.clear();
	// The apply gate stays closed while the seek rebuilds entities. Invalidate the previous frame's
	// late queue immediately so RunMainThreadLateFrame can never touch stale pre-seek pointers, and
	// force the recreated weapons through a fresh native renderer selection after the settle window.
	m_lateMeshReassert.clear();
	m_meshSwapStable = false;
	m_resumeWorldMeshReselectFrames = 0;
	m_framesSinceSeek = 0;
	m_lastCompositeTick = -1;
	m_compositeBurstRemaining = 0;
	// The zeroed burst never re-armed, so a scrub-then-play kept the demo's original material
	// until the user happened to change a setting. Mark the refresh owed; RunFrame pays it on
	// the first paused frame after the settle window (composite is paused-only by hard rule).
	if (!m_store.Empty())
		m_seekCompositePending = true;
	m_lastCompositeFrameByIdx.clear();
	m_compositeOwedIdx.clear(); // entities are rebuilt; the seek's own composite pass covers them
	m_meshFollowupCompositeDue.clear();
	int settle = 32;
	if (tickJump > 128) settle = 48;
	if (tickJump > 512) settle = 64;
	if (tickJump > 2048) settle = 96;
	if (tickJump > 8192) settle = 128;
	// Stack / extend when seeks arrive back-to-back (round skip spam).
	if (m_seekSettleFrames > 0)
		m_seekSettleFrames = (settle > m_seekSettleFrames) ? settle : (m_seekSettleFrames + settle / 2);
	else
		m_seekSettleFrames = settle;
	m_vmMirrorSettleFrames = m_seekSettleFrames * 4;
	if (m_vmMirrorSettleFrames < 96) m_vmMirrorSettleFrames = 96;
	if (MvmDebugLog_Active()) {
		MvmDebugLog_Linef("cosmetics.seek", "jump=%d settleFrames=%d vmSettle=%d", tickJump, settle, m_vmMirrorSettleFrames);
		MvmCrashWatch_Arm(-1, "demo-seek"); // widen crash.veh window across seek/settle for round-skip repros
	}

	// HOLD A SEEK THAT LANDED WHILE PLAYING. m_seekCompositePending is paid on a paused frame only, so
	// with nothing to pause it the skins stayed off: the tick loop jumped back to In and played the
	// demo's own skins every pass (user report 2026-10-21, "it goes back to the beginning, some skins
	// are just gone even though they are selected"). Pause on the landing, redraw, resume.
	// Not for a scrub (it owns the play state until it lands) or a render job (it holds its own paused
	// landing and waits on SeekRefreshPending), and not with the nudge or the composite switched off.
	if (holding) {
		m_seekHoldStartFrame = m_frameCounter;
	} else if (m_seekCompositePending && m_demoPlayingLastFrame && m_tickNudge && m_autoComposite
		&& !DemoScrub_Active() && !RenderSessionDriver_IsActive()) {
		m_nudgePhase = 3;
		m_seekHoldStartFrame = m_frameCounter;
		m_seekHoldPauseIssued = false;
		m_seekHoldPaused = false;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("cosmetics.seekhold", "START jump=%d frame=%llu", tickJump,
				(unsigned long long)m_frameCounter);
	}
}

void CosmeticOverrideSystem::DetectDemoSeekEarly() {
	int curTick = 0;
	if (!g_MirvTime.GetCurrentDemoTick(curTick))
		return;
	if (m_lastDemoTickObserved >= 0) {
		int d = curTick - m_lastDemoTickObserved;
		if (d < 0) d = -d;
		if (d > 64)
			OnDemoSeekDetected(d);
	}
	m_lastDemoTickObserved = curTick;
	m_demoPlayingLastFrame = !DemoIsPaused(); // read by the NEXT detection: was the seek made while playing?
}

void CosmeticOverrideSystem::RequestApplyNudge() {
	RequestCompositeRefresh();
	// A seek hold is already pausing for a composite pass and resumes into live frames afterwards;
	// the burst armed above extends it (SeekRefreshPending). A second pause after it would be noise.
	if (m_nudgePhase == 3) {
		m_pendingNudge = false;
		m_pendingNudgeOwedOnly = false;
		return;
	}
	// The pause already in flight serves this edit (including a queued pick cancellation).
	// Leaving another nudge pending would pause again after this one returns playback.
	if (m_nudgePhase == 2) {
		m_pendingNudge = false;
		m_pendingNudgeOwedOnly = false;
		// Preserve its mesh marker: immediate RebuildOnce already composites a paused pick.
		// Forcing it again would bypass the burst cooldown in the same engine frame.
		m_pauseCompositeDoneFrame = 0;
		m_nudgePlayStartFrame = m_frameCounter;
		return;
	}
	// Mark a nudge pending and (re)set the debounce anchor to now. A burst of edits (e.g. dragging
	// the wear slider) keeps pushing the anchor forward, so exactly one seek fires after the user
	// stops -- never one seek per intermediate value.
	m_pendingNudge = true;
	m_pendingNudgeOwedOnly = false;
	m_nudgeRequestFrame = m_frameCounter;
}

void CosmeticOverrideSystem::RequestOwedCompositePause(int entityIndex) {
	// Callers can repeat this every frame for the same gun (a settled switch waits for its pause).
	// Only a NEW gun may restart the debounce, or the repeats starve it and the pause never fires.
	// The current pause already serves these requests. Hand them to its apply pass
	// directly, without leaving a second pause queued after it completes.
	if (m_nudgePhase == 2 || m_nudgePhase == 3) {
		m_compositeOwedIdx.insert(entityIndex);
		return;
	}
	const bool fresh = m_owedPauseIdx.insert(entityIndex).second;
	// A theme dresses the whole server: someone draws a new gun every few seconds, and pausing the
	// demo for each one made playback unusable. Theme composites are BATCHED: parked here and paid
	// by one short pause at most every kThemeOwedPauseGapMs (MaybeFireTickNudge), or at once when
	// the user pauses.
	if (m_theme && GetTickCount64() - m_lastOwedPauseMs < kThemeOwedPauseGapMs)
		return;
	if (!m_pendingNudge) {
		m_pendingNudge = true;
		m_pendingNudgeOwedOnly = true;
		m_nudgeRequestFrame = m_frameCounter;
	} else if (fresh) {
		m_nudgeRequestFrame = m_frameCounter;
	}
}

bool CosmeticOverrideSystem::DeferWeaponPickWhilePlaying(uint64_t steamId, int defIndex, int paintKit,
	float wear, int seed, int statTrak, bool knife) {
	if (!InDemoContext() || !g_pEngineToClient || m_nudgePhase == 1)
		return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	if (!demo || (demo->IsDemoPaused() && m_livePicks.empty()))
		return false;
	if (m_livePicks.empty()) {
		// Own the pause's completion as soon as it is requested. Clearing the final pick must
		// still return playback even though CommitLivePicks then has nothing to commit.
		m_pendingNudge = false;
		m_pendingNudgeOwedOnly = false;
		m_postNudgeMeshRefreshPending = false;
		m_pauseCompositeDoneFrame = 0;
		m_nudgePlayStartFrame = m_frameCounter;
		m_nudgePhase = 2;
		// Work waiting for a pause rides this one instead of scheduling another on resume.
		for (int idx : m_owedPauseIdx) m_compositeOwedIdx.insert(idx);
		m_owedPauseIdx.clear();
		g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true);
	}
	// Once a live batch exists, later picks join it even if the pause already took effect.
	// Applying a later pick immediately would let the older queued pick overwrite it.
	m_livePicks.push_back({ steamId, defIndex, paintKit, wear, seed, statTrak, knife });
	m_livePickRequestFrame = m_frameCounter;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.livepick", "DEFER def=%d paint=%d steam=%llu queued=%d frame=%llu",
			defIndex, paintKit, (unsigned long long)steamId, (int)m_livePicks.size(),
			(unsigned long long)m_frameCounter);
	return true;
}

void CosmeticOverrideSystem::CommitLivePicks() {
	if (m_livePicks.empty())
		return;
	bool paused = false;
	if (g_pEngineToClient)
		if (auto* demo = g_pEngineToClient->GetDemoFile()) paused = demo->IsDemoPaused();
	const bool timedOut = m_frameCounter - m_livePickRequestFrame > 120;
	if (!paused && !timedOut)
		return;
	for (const PendingLivePick& pick : m_livePicks)
		if (pick.knife)
			SetKnife(pick.steamId, pick.defIndex, pick.paintKit, pick.wear, pick.seed);
		else
			SetWeapon(pick.steamId, pick.defIndex, pick.paintKit, pick.wear, pick.seed, pick.statTrak);
	const int committed = (int)m_livePicks.size();
	m_livePicks.clear();
	m_livePickRequestFrame = 0;
	if (!paused) {
		m_nudgePhase = 0;
		// The pause never took (e.g. playback was stopped meanwhile): fall back to the nudge path.
		RequestApplyNudge();
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("cosmetics.livepick", "COMMIT without pause (timed out) picks=%d", committed);
		return;
	}
	// This paused frame applies attributes, the native mesh selection and the direct composite together
	// (RunFrame, right after this); MaybeFireTickNudge phase 2 then settles the material and resumes.
	m_weaponMeshState.clear();
	m_postNudgeMeshRefreshPending = true;
	m_pauseCompositeDoneFrame = 0;
	m_nudgePlayStartFrame = m_frameCounter;
	m_nudgePhase = 2;
	RequestCompositeRefresh();
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.livepick", "COMMIT on paused frame picks=%d frame=%llu",
			committed, (unsigned long long)m_frameCounter);
}

void CosmeticOverrideSystem::MaybeFireTickNudge() {
	// Phase 3: a seek landed while the demo played (OnDemoSeekDetected). Pause once on the landing,
	// let the paused frames pay the post-seek composite, resume when SeekRefreshPending() clears.
	// demo_pause toggles, so it goes out only while the demo is seen playing, and only once.
	if (m_nudgePhase == 3) {
		const bool paused = DemoIsPaused();
		const uint64_t held = m_frameCounter - m_seekHoldStartFrame;
		if (!m_seekHoldPaused) {
			if (paused) {
				m_seekHoldPaused = true; // ours, or the seek's own one-frame pause: either way we resume it
			} else if (!m_seekHoldPauseIssued) {
				if (g_pEngineToClient)
					g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true);
				m_seekHoldPauseIssued = true;
			} else if (held > kSeekHoldPauseWaitFrames) {
				// The pause never took. Leave playback alone; the next pause pays the composite as before.
				m_nudgePhase = 0;
				if (MvmDebugLog_Active())
					MvmDebugLog_LinefAlways("cosmetics.seekhold", "GIVE UP pause not observed frames=%llu",
						(unsigned long long)held);
			}
			return;
		}
		if (!paused) {
			// Someone else resumed during the hold (the user, or a camera path's own resume): theirs to own.
			m_nudgePhase = 0;
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("cosmetics.seekhold", "RELEASED externally frames=%llu refreshPending=%d",
					(unsigned long long)held, SeekRefreshPending() ? 1 : 0);
			return;
		}
		const bool refreshed = !SeekRefreshPending();
		const bool timedOut = held > kSeekHoldMaxFrames;
		if (refreshed || timedOut) {
			if (g_pEngineToClient)
				g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
			m_nudgePhase = 0;
			++m_totalNudges;
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("cosmetics.seekhold", "DONE resumed refreshed=%d timedOut=%d frames=%llu",
					refreshed ? 1 : 0, timedOut ? 1 : 0, (unsigned long long)held);
		}
		return;
	}
	// Phase 2: a pick made during PLAYBACK paused the demo for one composite. RunFrame consumes
	// m_postNudgeMeshRefreshPending on the first frame it observes the pause (mesh write + the proven
	// paused-only direct composite). Hold the pause a few more frames so the async material work of
	// that composite drains, then hand playback back. A safety cap never leaves the demo paused.
	if (m_nudgePhase == 2) {
		constexpr uint64_t kSettleFrames = 24; // covers the post-switch composite settle (8) + material drain
		bool paused = false;
		if (g_pEngineToClient)
			if (auto* demo = g_pEngineToClient->GetDemoFile()) paused = demo->IsDemoPaused();
		if (paused && !m_postNudgeMeshRefreshPending && m_pauseCompositeDoneFrame == 0)
			m_pauseCompositeDoneFrame = m_frameCounter;
		const bool settled = !m_weaponMaterialBusy && m_pauseCompositeDoneFrame != 0
			&& m_frameCounter - m_pauseCompositeDoneFrame >= kSettleFrames;
		const bool timedOut = (m_frameCounter - m_nudgePlayStartFrame) > 240;
		if (settled || timedOut) {
			if (g_pEngineToClient)
				g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
			m_postNudgeMeshRefreshPending = false;
			m_nudgePhase = 0;
			++m_totalNudges;
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("cosmetics.nudge", "DONE live-composite resumed settled=%d timedOut=%d frames=%llu",
					settled ? 1 : 0, timedOut ? 1 : 0, (unsigned long long)(m_frameCounter - m_nudgePlayStartFrame));
		}
		return;
	}
	// Phase 1: a nudge is currently playing out (we issued demo_resume) -- re-pause once enough ticks
	// have rendered, or a frame safety cap trips so the demo is never left stuck playing.
	// demo_pause is a toggle. It is sent only after demo_resume has been observed, and only while
	// the demo is actually playing. Sending it earlier (resume never applied, or a seek aborted the
	// nudge on a still-paused demo) starts playback and then this state machine forgets it.
	if (m_nudgePhase == 1) {
		int tick = 0;
		const bool haveTick = g_MirvTime.GetCurrentDemoTick(tick);
		const bool paused = DemoIsPaused();
		if (!paused) m_nudgeSawResume = true;
		const bool enough = m_nudgeSawResume && haveTick && (tick - m_nudgeStartTick >= m_tickNudgeTicks);
		const bool timedOut = (m_frameCounter - m_nudgePlayStartFrame) > 240; // ~2-4s safety
		if (!m_nudgeSawResume) {
			if (!timedOut) return;
			m_postNudgeMeshRefreshPending = m_nudgeWasPaused;
			m_nudgePhase = 0;
			++m_totalNudges;
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("cosmetics.nudge", "DONE resume never started; staying paused frame=%llu",
					(unsigned long long)m_frameCounter);
			return;
		}
		if (!(enough || timedOut || m_nudgePauseIssued)) return;
		if (!m_nudgePauseIssued) {
			// Re-assert the override onto whatever entities the play-out left us with. Do NOT re-arm a
			// composite burst here: demo_pause is queued from a frame that can still report "playing",
			// and UpdateComposite on that frame hangs CS2. The burst is consumed during the play-out.
			InvalidateAgentStates();
			m_gloveState.clear();
			m_lastGloveSpectatedSid = 0;
			m_knifeSwapState.clear();
			m_weaponMeshState.clear();
			m_compositeBurstRemaining = 0;
			if (m_nudgeWasPaused && !paused && g_pEngineToClient)
				g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true);
			m_nudgePauseIssued = true;
			if (MvmDebugLog_Active())
				MvmDebugLog_Linef("cosmetics.nudge",
					"play-out done playedTicks=%d enough=%d timedOut=%d paused=%d",
					haveTick ? tick - m_nudgeStartTick : -1, enough ? 1 : 0, timedOut ? 1 : 0, paused ? 1 : 0);
		}
		if (m_nudgeWasPaused && !paused) return; // wait until the one pause is observed
		// Defer the final mesh-group write until RunFrame observes the paused state, then composite
		// once onto that mesh. Repeating SetMeshGroupMask while paused leaves the weapon gray.
		m_postNudgeMeshRefreshPending = m_nudgeWasPaused;
		m_nudgePhase = 0;
		m_nudgePauseIssued = false;
		++m_totalNudges;
		return;
	}

	if (!m_pendingNudge) {
		// Owed composites parked by a theme: pay them on the user's own pause, or start one batched
		// pause once the gap since the last one has elapsed.
		if (!m_owedPauseIdx.empty() && m_nudgePhase == 0 && g_pEngineToClient) {
			auto* demo = g_pEngineToClient->GetDemoFile();
			if (demo && demo->IsDemoPaused()) {
				for (int idx : m_owedPauseIdx) m_compositeOwedIdx.insert(idx);
				m_owedPauseIdx.clear();
			} else if (GetTickCount64() - m_lastOwedPauseMs >= kThemeOwedPauseGapMs) {
				m_pendingNudge = true;
				m_pendingNudgeOwedOnly = true;
				m_nudgeRequestFrame = m_frameCounter;
			}
		}
		return;
	}
	if (m_pendingNudgeOwedOnly && m_owedPauseIdx.empty() && m_compositeOwedIdx.empty()) {
		m_pendingNudge = false;
		m_pendingNudgeOwedOnly = false;
		return;
	}
	// Throttled (not per-frame): framesSinceReq changes every call so MvmDebugLog_Linef's dedup can't
	// collapse it: this diagnosed the m_seekSettleFrames-stuck-forever bug above via 2000+ lines/3s.
	if (MvmDebugLog_Active() && (m_frameCounter % 16) == 0)
		MvmDebugLog_Linef("cosmetics.nudge.gate",
			"pending=1 seekSettle=%d tickNudge=%d framesSinceReq=%llu inDemo=%d",
			m_seekSettleFrames, m_tickNudge ? 1 : 0,
			(unsigned long long)(m_frameCounter - m_nudgeRequestFrame), InDemoContext() ? 1 : 0);
	if (m_seekSettleFrames > 0)
		return; // defer until entity list settles after a seek
	if (!m_tickNudge) { m_pendingNudge = false; return; } // disabled: drop the request
	// Debounce: wait for a short quiet period after the last change so rapid re-picks coalesce.
	const uint64_t kDebounceFrames = 8;
	if (m_frameCounter - m_nudgeRequestFrame < kDebounceFrames)
		return;
	if (!InDemoContext())
		return; // keep pending; fire once a demo is actually playing
	int tick = 0;
	if (!g_MirvTime.GetCurrentDemoTick(tick)) {
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.nudge.gate", "tick unreadable, deferring");
		return; // tick not readable yet (mid-seek) -- retry next frame
	}
	m_pendingNudge = false;

	bool paused = true;
	if (g_pEngineToClient) {
		if (auto* demo = g_pEngineToClient->GetDemoFile())
			paused = demo->IsDemoPaused();
	}
	if (m_pendingNudgeOwedOnly) {
		// Only composites are owed (weapons drawn after their skin was applied). Hand them back to the
		// owed set, which the paused apply pays per weapon. Do NOT forget the mesh selections, agents
		// or gloves like a profile change does: that re-switched every overridden gun, which re-owed
		// a composite on every holstered one, so with a whole server dressed (UwU) the demo paused
		// for a new composite about once a second forever.
		m_pendingNudgeOwedOnly = false;
		for (int idx : m_owedPauseIdx) m_compositeOwedIdx.insert(idx);
		const int owed = (int)m_owedPauseIdx.size();
		m_owedPauseIdx.clear();
		if (paused)
			return; // the user paused meanwhile: the paused frames pay them without our help
		m_lastOwedPauseMs = GetTickCount64();
		m_postNudgeMeshRefreshPending = false;
		m_pauseCompositeDoneFrame = 0;
		m_nudgePlayStartFrame = m_frameCounter;
		m_nudgePhase = 2;
		if (g_pEngineToClient)
			g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true);
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.nudge", "START owed-composite pause weapons=%d frame=%llu", owed,
				(unsigned long long)m_frameCounter);
		return;
	}
	if (!m_owedPauseIdx.empty()) {
		// A full nudge re-selects and composites everything; the owed ones ride along.
		for (int idx : m_owedPauseIdx) m_compositeOwedIdx.insert(idx);
		m_owedPauseIdx.clear();
	}
	if (!paused) {
		// A pick made while the demo PLAYS. Live frames re-evaluate agents and gloves, but a weapon
		// skin that needs the other mesh (legacy CS:GO <-> modern CS2, e.g. Glock Moonrise -> AXIA)
		// gets its mesh switched and never its texture: the direct composite is paused-only (it hung
		// CS2 during live packets in July 2026), so the gun kept the old look until a seek (measured
		// 2026-09-28). Pause, let one paused frame composite onto the new mesh, then resume.
		InvalidateAgentStates();
		m_gloveState.clear();
		m_lastGloveSpectatedSid = 0;
		m_weaponMeshState.clear();          // re-run the native mesh selection onto the paused frame
		m_postNudgeMeshRefreshPending = true; // RunFrame: mesh write + composite on the paused frame
		m_pauseCompositeDoneFrame = 0;
		m_nudgePlayStartFrame = m_frameCounter;
		m_nudgePhase = 2;
		if (g_pEngineToClient)
			g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true);
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("cosmetics.nudge", "START live-composite pause frame=%llu", (unsigned long long)m_frameCounter);
		return;
	}

	// Paused: briefly resume so the engine renders real frames (the only thing that makes a body
	// model / glove / mesh swap re-evaluate), then re-pause after ~m_tickNudgeTicks ticks -- exactly
	// "let the game play ~10 ticks" done automatically. Clear the per-pawn apply gates so the
	// override re-fires onto any entities the play-out recreates.
	InvalidateAgentStates();
	m_gloveState.clear();
	m_lastGloveSpectatedSid = 0;
	m_nudgeWasPaused = true;
	m_nudgeSawResume = false;
	m_nudgePauseIssued = false;
	m_nudgeStartTick = tick;
	m_nudgePlayStartFrame = m_frameCounter;
	m_nudgePhase = 1;
	if (g_pEngineToClient)
		g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("cosmetics.nudge", "START resume plannedTicks=%d", m_tickNudgeTicks);
}

} // namespace Filmmaker
