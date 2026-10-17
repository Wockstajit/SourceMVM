#include "FollowCamera.h"
#include "FollowTargetProviders.h"

#include "CameraBridge.h"
#include "CameraPath.h"
#include "../Filmmaker.h"
#include "../Panorama/CameraTimelineHud.h"
#include "../Panorama/GraphEditorExperimentHud.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../Demo/DemoInfoHelper.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_map>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

constexpr double kMediumDistance = 1800.0;
constexpr double kFarDistance = 3500.0;
constexpr double kVeryFarDistance = 7000.0;
constexpr double kTeleportDistance = 1800.0;
constexpr double kDefaultFov = 90.0;
constexpr double kDefaultLockOnLookSmoothing = 0.10;
constexpr double kDefaultLockOnPositionSmoothing = 0.04;
constexpr double kDefaultLockOnPrediction = 0.0;
constexpr double kDefaultLockOnDeadzone = 0.0;
constexpr double kDefaultLockOnMaxTurnSpeed = 720.0;

FollowVec3 DefaultLockOnOffset() {
	return FollowVec3{ 0.0, 0.0, 0.0 };
}

FollowVec3 DefaultAttachOffset() {
	return FollowVec3{ 72.0, 0.0, 8.0 };
}

FollowAngles DefaultRotationOffset() {
	return FollowAngles{ 0.0, 0.0, 0.0 };
}

} // namespace

FollowCamera& FollowCameraRef() {
	static FollowCamera instance;
	return instance;
}

double FollowCamera::WallDt() {
	LARGE_INTEGER frequency, now;
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&now);
	if (!m_lastQpc) m_lastQpc = now.QuadPart;
	double dt = (double)(now.QuadPart - m_lastQpc) / (double)frequency.QuadPart;
	m_lastQpc = now.QuadPart;
	if (dt < 0.0) dt = 0.0;
	if (dt > 0.1) dt = 0.1;
	return dt;
}

void FollowCamera::ResetMotion() {
	m_motionInitialized = false;
	m_lockOnClock = {};
	m_lockOnHaveTarget = false;
	m_haveLastRawTarget = false;
	m_retargeting = false;
	m_retargetElapsed = 0.0;
	m_deathRetargetElapsed = 0.0;
	m_lastFrameTick = -1;
	m_lastQpc = 0;
	m_attachDebug = AttachDebugSnapshot();
	m_attachRebaseValid = false;
	m_attachBaseOriented = false;
	m_attachOffsetIsLocal = false;
	m_havePrevAttachFresh = false;
	m_havePrevDebug = false;
}

void FollowCamera::ResetFramingState() {
	m_state.offset = (m_state.mode == FollowMode::Attach) ? DefaultAttachOffset() : DefaultLockOnOffset();
	m_state.rotationOffset = DefaultRotationOffset();
	m_state.fov = kDefaultFov;
}

void FollowCamera::ApplyAttachTrackingDefaults() {
	m_state.lookSmoothing = 0.0;
	m_state.positionSmoothing = 0.0;
	m_state.prediction = 0.0;
	m_state.deadzone = 0.0;
	m_state.maxTurnSpeed = 0.0;
	m_renderTimeSample = true;
}

void FollowCamera::ResetTrackingState() {
	m_state.lookSmoothing = kDefaultLockOnLookSmoothing;
	m_state.positionSmoothing = kDefaultLockOnPositionSmoothing;
	m_state.prediction = kDefaultLockOnPrediction;
	m_state.deadzone = kDefaultLockOnDeadzone;
	m_state.maxTurnSpeed = kDefaultLockOnMaxTurnSpeed;
	m_state.autoDisableOnDeath = true;
	m_state.deathHoldSeconds = 3.0;
	m_state.switchToDroppedWeaponOnDeath = false;
	m_state.switchToDroppedBombOnDeath = false;
	m_state.holdLastKnownPosition = true;
	if (m_state.mode == FollowMode::Attach)
		ApplyAttachTrackingDefaults();
}

void FollowCamera::PlaceCamera() {
	const bool replaced = m_state.hasCamera;
	double pos[3] = {}, ang[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(pos, ang, fov);
	m_state.cameraPosition = FollowVec3{ pos[0], pos[1], pos[2] };
	m_state.cameraAngles = FollowAngles{ ang[0], ang[1], ang[2] };
	m_state.fov = fov;
	m_state.hasCamera = true;
	m_repositioning = false;
	ResetMotion();
	// Include the tick so each Place produces a DISTINCT status line -- the on-screen text
	// changes every time, making a registered click visibly distinguishable from a no-op.
	int placeTick = 0; g_MirvTime.GetCurrentDemoTick(placeTick);
	m_lastMessage = (replaced ? "Follow Camera replaced @ tick " : "Follow Camera placed @ tick ")
		+ std::to_string(placeTick) + ".";
	advancedfx::Message("[followcam] single camera %s pos=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) fov=%.1f.\n",
		replaced ? "replaced" : "placed",
		pos[0], pos[1], pos[2], ang[0], ang[1], ang[2], fov);
}

void FollowCamera::BeginReposition() {
	if (m_state.mode == FollowMode::Attach) {
		const bool haveTarget = m_state.targetEntityIndex >= 0
			|| (m_state.targetType == FollowTargetType::Weapon && m_state.weaponPlayerIndex >= 0)
			|| (m_state.targetType == FollowTargetType::Grenade && m_state.targetHandle);
		if (!haveTarget) {
			advancedfx::Warning("[followcam] attach reposition refused: select a target first.\n");
			m_lastMessage = "Select a target before repositioning attach.";
			return;
		}
	}
	m_resumePreviewAfterReposition = m_state.enabled && m_state.mode == FollowMode::Attach;
	StopPreview("reposition started");
	m_repositioning = true;
	CameraBridge_SetFreeCamEnabled(true);
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(false);
	if (m_state.mode == FollowMode::Attach) {
		m_lastMessage = "Move freecam, then left-click to save attach offset/rotation/FOV.";
		advancedfx::Message("[followcam] repositioning attach mount: move + left-click to save relative pose (Esc cancels).\n");
	} else {
		advancedfx::Message("[followcam] repositioning single camera: move + left-click to place (Esc cancels).\n");
	}
}

void FollowCamera::PlaceReposition() {
	if (!m_repositioning) return;
	if (m_state.mode == FollowMode::Attach) {
		const bool resumePreview = m_resumePreviewAfterReposition;
		if (!CaptureAttachPoseFromCurrentView())
			return;
		m_repositioning = false;
		m_resumePreviewAfterReposition = false;
		if (CameraEditor_Active())
			CameraEditor_SetCursorMode(true);
		if (resumePreview)
			Preview();
		advancedfx::Message("[followcam] attach reposition complete; editor mouse restored.\n");
		return;
	}
	PlaceCamera();
	m_resumePreviewAfterReposition = false;
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(true);
	advancedfx::Message("[followcam] reposition complete; editor mouse restored.\n");
}

void FollowCamera::CancelReposition() {
	if (!m_repositioning) return;
	m_repositioning = false;
	m_resumePreviewAfterReposition = false;
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(true);
	advancedfx::Message("[followcam] reposition cancelled; editor mouse restored.\n");
}

bool FollowCamera::CaptureAttachPoseFromCurrentView() {
	if (m_state.mode != FollowMode::Attach) {
		advancedfx::Warning("[followcam] attach capture refused: switch to Attach mode first.\n");
		m_lastMessage = "Switch to Attach mode before saving a relative mount.";
		return false;
	}

	auto provider = MakeProvider();
	if (!provider || !provider->IsValid()) {
		advancedfx::Warning("[followcam] attach capture refused: selected target is not valid.\n");
		m_lastMessage = "Selected attach target is not valid.";
		return false;
	}

	FollowVec3 target;
	FollowAngles baseAngles;
	const bool oriented = provider->GetWorldTransform(target, baseAngles);
	if (!IsFiniteOrigin(target)) {
		advancedfx::Warning("[followcam] attach capture refused: target transform is invalid.\n");
		m_lastMessage = "Selected attach target transform is invalid.";
		return false;
	}

	double pos[3] = {}, ang[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(pos, ang, fov);
	const FollowVec3 camPos{ pos[0], pos[1], pos[2] };
	const FollowAngles camAngles{ ang[0], ang[1], ang[2] };
	if (!oriented)
		baseAngles = camAngles;

	const FollowVec3 worldOffset{
		camPos.x - target.x,
		camPos.y - target.y,
		camPos.z - target.z
	};
	m_state.offset = FollowInverseRotateVector(worldOffset, baseAngles);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("follow.mount.capture",
			"entity=%d attach=%s oriented=%d base=(%.6f %.6f %.6f) camera=(%.6f %.6f %.6f) localPos=(%.6f %.6f %.6f)",
			provider->EntityIndex(), m_state.attachmentName.c_str(), oriented,
			baseAngles.pitch, baseAngles.yaw, baseAngles.roll, camAngles.pitch, camAngles.yaw, camAngles.roll,
			m_state.offset.x, m_state.offset.y, m_state.offset.z);
	m_state.rotationOffset = FollowRelativeRotation(camAngles, baseAngles);
	m_state.fov = std::clamp(fov, 1.0, 170.0);
	m_state.cameraPosition = camPos;
	m_state.cameraAngles = baseAngles;
	m_baseAngles = baseAngles;
	ResetMotion();
	m_lastMessage = "Attach mount saved relative to '" + m_state.attachmentName + "'.";
	advancedfx::Message(
		"[followcam] attach mount saved: attach=%s offset=(%.2f %.2f %.2f) rotation=(%.2f %.2f %.2f) fov=%.1f.\n",
		m_state.attachmentName.c_str(),
		m_state.offset.x, m_state.offset.y, m_state.offset.z,
		m_state.rotationOffset.pitch, m_state.rotationOffset.yaw, m_state.rotationOffset.roll,
		m_state.fov);
	return true;
}

void FollowCamera::ClearCamera() {
	CancelReposition();
	StopPreview();
	ResetTrackingState();
	ResetFramingState();
	ResetMotion();
	m_state.hasCamera = false;
	m_state.targetEntityIndex = -1;
	m_state.targetHandle = 0;
	m_selectedGrenadeHandle = 0;
	m_grenadeTrackPending = false;
	m_haveLastKnownTarget = false;
	m_lastMessage = "Follow Camera cleared.";
	CameraBridge_SetFollowCameraMarker(false, 0, 0, 0, 0, 0, 0, 90);
	advancedfx::Message("[followcam] camera cleared.\n");
}

void FollowCamera::Preview() {
	if (m_state.mode == FollowMode::LockOn && !m_state.hasCamera) {
		advancedfx::Warning("[followcam] preview refused: place the follow camera first (lock-on mode).\n");
		m_lastMessage = "Place the camera first, or switch to Attach mode.";
		return;
	}
	const bool haveTarget = m_state.targetEntityIndex >= 0
		|| (m_state.targetType == FollowTargetType::Weapon && m_state.weaponPlayerIndex >= 0);
	if (!haveTarget) {
		advancedfx::Warning("[followcam] preview refused: select a target first.\n");
		m_lastMessage = "Select a target first.";
		return;
	}
	CameraPathRef().StopPreview();
	CameraPathRef().StopScrub();
	GraphEditorExperimentHudRef().SetDrive(false);
	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetPathDrawEnabled(false);
	m_state.enabled = true;
	ResetMotion();
	advancedfx::Message("[followcam] preview started: type=%s entity=%d.\n",
		FollowTargetTypeName(m_state.targetType), m_state.targetEntityIndex);
}

void FollowCamera::StopPreview(const char* reason) {
	if (m_state.enabled && reason && *reason)
		advancedfx::Message("[followcam] preview stopped: %s.\n", reason);
	m_state.enabled = false;
	m_attachRebaseValid = false;     // don't let a stale attach pose drive the view after stop
	m_havePrevAttachFresh = false;
	m_grenadeTrackPending = false;
	m_grenadeSeekObserved = false;
	m_grenadeResumeIssued = false;
	if (reason && *reason)
		m_lastMessage = std::string("Tracking stopped: ") + reason + ".";
	if (CameraEditor_Active() && CameraTimelineHudRef().Cursor())
		GraphEditorExperimentHudRef().SetDrive(true);
	ResetMotion();
}

bool FollowCamera::ViewSetupAttachOverride(float curTime,
	double& x, double& y, double& z, double& pitch, double& yaw, double& roll, double& fov) {
	(void)curTime;
	if (!m_attachRebaseValid || !m_state.enabled || m_state.mode != FollowMode::Attach)
		return false;
	// Sample the target HERE (view setup = mid-render), where entities are interpolated, instead of
	// the tick-stepped value RunFrame read at FrameStageNotify.
	auto provider = MakeProvider();
	if (!provider || !provider->IsValid()) return false;
	FollowVec3 freshTarget; FollowAngles freshAng;
	const bool freshOriented = provider->GetWorldTransform(freshTarget, freshAng);
	if (!IsFiniteOrigin(freshTarget))
		return false;
	// Diagnostic: freshDelta is the frame-to-frame movement of the render-time sample. If the
	// render-time sample is interpolated this is a small, smoothly-varying value EVERY frame; if it
	// were still stepped it would read 0.000 then jump (the FrameStageNotify signature).
	const double freshDelta = m_havePrevAttachFresh ? FollowDistance(m_prevAttachFresh, freshTarget) : 0.0;
	const double steppedVsFresh = FollowDistance(m_attachSteppedTarget, freshTarget);
	m_prevAttachFresh = freshTarget; m_havePrevAttachFresh = true;
	const bool sampleRenderDebug = ((m_debugViewFrame++ % 15) == 0);
	if (MvmDebugLog_Active() && sampleRenderDebug)
		MvmDebugLog_Linef("follow.attach.render",
			"entity=%d oriented=%d stepped=(%.3f,%.3f,%.3f) fresh=(%.3f,%.3f,%.3f) freshDelta=%.4f steppedVsFresh=%.4f apply=%d",
			provider->EntityIndex(), freshOriented ? 1 : 0,
			m_attachSteppedTarget.x, m_attachSteppedTarget.y, m_attachSteppedTarget.z,
			freshTarget.x, freshTarget.y, freshTarget.z, freshDelta, steppedVsFresh,
			m_renderTimeSample ? 1 : 0);
	if (m_debug && sampleRenderDebug) {
		advancedfx::Message("[followcam][rtsample] fresh=(%.2f %.2f %.2f) freshDelta=%.3f steppedVsFresh=%.3f apply=%d.\n",
			freshTarget.x, freshTarget.y, freshTarget.z, freshDelta, steppedVsFresh,
			m_renderTimeSample ? 1 : 0);
	}
	if (!m_renderTimeSample) return false;
	// Rebase the smoothed camera onto the interpolated target. For oriented attach points the
	// offset is stored in target-local axes, so it rotates with the same render-time bone pose the
	// visible model uses. A world-space offset from the stepped pose makes hand/weapon mounts look
	// stable in screenshots but swim/jitter in motion as the bone orientation interpolates.
	const FollowVec3 freshOffset = freshOriented && m_attachOffsetIsLocal
		? FollowRotateVector(m_attachOffsetFromTarget, freshAng)
		: m_attachOffsetFromTarget;
	x = freshTarget.x + freshOffset.x;
	y = freshTarget.y + freshOffset.y;
	z = freshTarget.z + freshOffset.z;
	const FollowAngles freshOutput = freshOriented && m_attachBaseOriented
		? FollowComposeRotation(freshAng, m_attachAngleOffsetFromBase)
		: m_outputAngles;
	pitch = freshOutput.pitch;
	yaw = freshOutput.yaw;
	roll = freshOutput.roll;
	fov = m_state.fov;
	if (MvmDebugLog_Active() && sampleRenderDebug) {
		int tick = 0; g_MirvTime.GetCurrentDemoTick(tick);
		const auto localPosition = FollowInverseRotateVector({x-freshTarget.x, y-freshTarget.y, z-freshTarget.z}, freshAng);
		const auto localForward = FollowInverseRotateVector(FollowRotateVector({1,0,0}, freshOutput), freshAng);
		const auto localUp = FollowInverseRotateVector(FollowRotateVector({0,0,1}, freshOutput), freshAng);
		const auto entity = EntityAt(provider->EntityIndex());
		MvmDebugLog_Linef("follow.mount.render",
			"tick=%d entity=%d ptr=%p handle=%llu class=%s attach=%s oriented=%d base=(%.6f %.6f %.6f) camera=(%.6f %.6f %.6f) localPos=(%.6f %.6f %.6f) localForward=(%.6f %.6f %.6f) localUp=(%.6f %.6f %.6f)",
			tick, provider->EntityIndex(), entity, (unsigned long long)EntityHandleValue(entity), EntityClass(entity).c_str(),
			m_state.attachmentName.c_str(), freshOriented, freshAng.pitch, freshAng.yaw, freshAng.roll,
			pitch, yaw, roll, localPosition.x, localPosition.y, localPosition.z,
			localForward.x, localForward.y, localForward.z, localUp.x, localUp.y, localUp.z);
	}
	return true;
}

void FollowCamera::SetTargetType(FollowTargetType type) {
	if (m_state.targetType == type) return;
	m_state.targetType = type;
	m_state.targetEntityIndex = -1;
	m_state.weaponPlayerIndex = type == FollowTargetType::Weapon ? m_lastPlayerIndex : -1;
	m_state.targetHandle = 0;
	m_grenadeTrackPending = false;
	m_selectedEvent = -1;
	// Start from safe defaults. Candidate selection upgrades these to validated
	// model/player points (for example muzzle) only when the selected target exposes them.
	if (type == FollowTargetType::Weapon) m_state.attachmentName = "entity";
	else if (type == FollowTargetType::Grenade) m_state.attachmentName = "entity";
	else if (type == FollowTargetType::Player) m_state.attachmentName = "head";
	ResetMotion();
}

void FollowCamera::SetMode(FollowMode mode) {
	if (m_state.mode == mode) return;
	m_modeSettings[(int)m_state.mode] = static_cast<const FollowCameraSettings&>(m_state);
	m_haveModeSettings[(int)m_state.mode] = true;
	m_state.mode = mode;
	if (m_haveModeSettings[(int)mode]) {
		static_cast<FollowCameraSettings&>(m_state) = m_modeSettings[(int)mode];
	} else {
		static_cast<FollowCameraSettings&>(m_state) = FollowCameraSettings{};
		ResetFramingState();
		ResetTrackingState();
		if (m_state.targetType != FollowTargetType::Player) m_state.attachmentName = "entity";
	}
	if (mode == FollowMode::Attach) ApplyAttachTrackingDefaults();
	m_lastMessage = mode == FollowMode::Attach ? "Attached settings restored." : "Lock-on settings restored.";
	ResetMotion();
}

void FollowCamera::SetWeaponSource(WeaponSource source) {
	if (m_state.weaponSource == source) return;
	m_state.weaponSource = source;
	ResetMotion();
}

bool FollowCamera::SetWeaponPlayer(int pawnIndex) {
	bool found = false;
	for (const auto& player : Players())
		if (player.entityIndex == pawnIndex) { found = true; break; }
	if (!found) { m_lastMessage = "That player is no longer available."; return false; }
	m_lastPlayerIndex = pawnIndex;
	m_state.weaponPlayerIndex = pawnIndex;
	m_state.targetEntityIndex = -1;
	m_state.targetHandle = 0;
	if (m_state.weaponSource == WeaponSource::Dropped) m_state.weaponSource = WeaponSource::Auto;
	auto provider = MakeProvider();
	if (provider && provider->IsValid()) {
		m_state.targetEntityIndex = provider->EntityIndex();
		if (CEntityInstance* entity = EntityAt(m_state.targetEntityIndex)) {
			const auto handle = entity->GetHandle();
			m_state.targetHandle = handle.IsValid() ? (uint64_t)(uint32_t)handle.ToInt() : 0;
		}
	}
	m_lastMessage = "Weapon target scoped to the selected player.";
	ResetMotion();
	return true;
}

bool FollowCamera::GoToTarget() {
	auto provider = MakeProvider();
	if (!provider || !provider->IsValid()) {
		m_lastMessage = "Select an available target first.";
		return false;
	}
	const FollowVec3 target = provider->GetWorldPosition();
	if (!IsFiniteOrigin(target)) return false;
	const FollowVec3 camera{ target.x + 96.0, target.y, target.z + 36.0 };
	const FollowAngles look = FollowLookAt(camera, target);
	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetCameraPose(camera.x, camera.y, camera.z,
		look.pitch, look.yaw, 0.0, m_state.fov);
	m_lastMessage = "Moved the camera to the selected target.";
	return true;
}

std::unique_ptr<IFollowTargetProvider> FollowCamera::MakeProvider() const {
	const bool attach = m_state.mode == FollowMode::Attach;
	switch (m_state.targetType) {
	case FollowTargetType::Player:
		if (attach)
			return std::make_unique<AttachmentTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Player, m_state.attachmentName, &m_playerPose);
		return std::make_unique<PlayerTargetProvider>(m_state.targetEntityIndex, &m_playerPose);
	case FollowTargetType::Grenade:
		if (attach)
			return std::make_unique<AttachmentTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Grenade, m_state.attachmentName);
		return std::make_unique<EntityTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Grenade);
	case FollowTargetType::Weapon: {
		const bool held = (m_state.weaponSource == WeaponSource::Held)
			|| (m_state.weaponSource == WeaponSource::Auto && m_state.weaponPlayerIndex >= 0);
		if (held && m_state.weaponPlayerIndex >= 0)
			return std::make_unique<HeldWeaponProvider>(m_state.weaponPlayerIndex,
				attach ? m_state.attachmentName : std::string());
		if (attach && !m_state.attachmentName.empty())
			return std::make_unique<AttachmentTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Weapon, m_state.attachmentName);
		return std::make_unique<EntityTargetProvider>(m_state.targetEntityIndex, FollowTargetType::Weapon);
	}
	default:
		return std::make_unique<PlayerTargetProvider>(m_state.targetEntityIndex);
	}
}

void FollowCamera::PublishTargetHighlight() {
	// EDITOR-ONLY helper. It is already kept out of every captured stream (the drawer's
	// SuppressForCapture flag) and out of the game when the editor is closed (RunFrame's
	// cameraEditorActive gate). The remaining case is LIVE MODE: once the follow preview is
	// actually driving the camera the user is watching the shot itself, and a selection box
	// floating around the subject is exactly what they are trying to look past. Same rule the
	// other editor helpers follow -- visible while you are choosing a target, gone once it is
	// flying.
	if (m_state.enabled) {
		m_wantTargetHighlight = false;
		CameraBridge_SetFollowTargetBox(false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
		return;
	}
	// It answers "which one did I just pick?", so it shows for a few seconds after a target is
	// chosen (Select Nearest, a pick from the list) and then goes. Left on, it stayed around the
	// subject for the rest of the session (user report 2026-10-17). Picking again shows it again.
	constexpr unsigned long long kTargetHighlightMs = 5000;
	const unsigned long long now = GetTickCount64();
	if (m_state.targetEntityIndex != m_highlightTarget || m_state.targetType != m_highlightType) {
		m_highlightTarget = m_state.targetEntityIndex;
		m_highlightType = m_state.targetType;
		m_highlightSinceMs = now;
	}
	CEntityInstance* entity = m_state.targetEntityIndex >= 0 ? EntityAt(m_state.targetEntityIndex) : nullptr;
	FollowVec3 origin;
	if (!entity || !ReadOrigin(entity, origin) || now - m_highlightSinceMs >= kTargetHighlightMs) {
		m_wantTargetHighlight = false;
		CameraBridge_SetFollowTargetBox(false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
		return;
	}
	// A pawn's origin is at the feet, so the box just grows upward over the body. A weapon,
	// grenade or the C4 is a small object roughly centred on its origin, so drop the base by
	// half its height to wrap it instead of standing a column on top of it.
	const bool pawn = entity->IsPlayerPawn();
	const float half = pawn ? 18.0f : 9.0f;
	const float height = pawn ? 72.0f : 18.0f;
	const float baseZ = (float)origin.z - (pawn ? 0.0f : height * 0.5f);
	m_wantTargetHighlight = true;
	CameraBridge_SetFollowTargetBox(true, (float)origin.x, (float)origin.y, baseZ, half, height);
}

void FollowCamera::RunFrame(bool cameraEditorActive) {
	if (!cameraEditorActive) {
		CameraBridge_SetFollowCameraMarker(false, 0, 0, 0, 0, 0, 0, 90);
		m_wantTargetHighlight = false;
		CameraBridge_SetFollowTargetBox(false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
		if (m_state.enabled) StopPreview("camera editor closed");
		return;
	}
	PublishTargetHighlight();
	if (m_state.hasCamera && m_state.mode == FollowMode::LockOn && !m_state.enabled) {
		CameraBridge_SetFollowCameraMarker(true,
			m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
			m_state.cameraAngles.pitch, m_state.cameraAngles.yaw, m_state.cameraAngles.roll, m_state.fov);
	} else {
		CameraBridge_SetFollowCameraMarker(false, 0, 0, 0, 0, 0, 0, 90);
	}
	// Attach mode rides the target and needs no placed camera; lock-on requires one.
	if (!m_state.enabled)
		return;
	if (m_state.mode == FollowMode::LockOn && !m_state.hasCamera)
		return;

	double dt = WallDt();
	// Is the demo actually advancing this frame? When it's paused/frozen the entity still
	// micro-jitters from interpolation; dividing that by the (tiny) wall-clock dt yields a
	// huge bogus velocity that drives the "face travel" path and makes an attached camera
	// spin/jitter in place. Detect a frozen tick so we can zero velocity below.
	int frameTick = 0; g_MirvTime.GetCurrentDemoTick(frameTick);
	const int previousFrameTick = m_lastFrameTick;
	const double deathDt = FollowPlaybackDelta(previousFrameTick, frameTick, g_MirvTime.interval_per_tick_get());
	if (FollowTickDiscontinuity(previousFrameTick, frameTick)) {
		MvmDebugLog_Linef("follow.clock", "reset previous=%d current=%d elapsed=%.3f", previousFrameTick, frameTick, m_deathRetargetElapsed);
		ResetMotion();
		m_playerPose = FollowPlayerPose{};
		m_haveLastKnownTarget = false;
	}
	const bool demoAdvancing = (previousFrameTick >= 0 && frameTick != previousFrameTick);
	if (demoAdvancing) {
		float interval = g_MirvTime.interval_per_tick_get();
		if (!(interval > 0.00001f)) interval = 1.0f / 64.0f;
		const int tickDelta = std::abs(frameTick - previousFrameTick);
		if (tickDelta > 0 && tickDelta < 128)
			dt = std::clamp((double)tickDelta * (double)interval, 0.0001, 0.1);
	}
	m_lastFrameTick = FollowTickDiscontinuity(previousFrameTick, frameTick)
		? frameTick : (std::max)(previousFrameTick, frameTick);
	if (m_state.targetType == FollowTargetType::Grenade && m_grenadeTrackPending) {
		UpdateGrenadeCache();
		int currentTick = 0;
		g_MirvTime.GetCurrentDemoTick(currentTick);

		if (!m_grenadeSeekObserved) {
			if (currentTick >= m_grenadeSeekTick - 3 && currentTick <= m_grenadeThrowTick - 8)
				m_grenadeSeekObserved = true;
			CameraBridge_SetFreeCamEnabled(true);
			CameraBridge_SetCameraPose(
				m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
				m_state.cameraAngles.pitch, m_state.cameraAngles.yaw, m_state.cameraAngles.roll, m_state.fov);
			if (!m_grenadeSeekObserved)
				return;
		}

		if (!m_grenadeResumeIssued && g_pEngineToClient) {
			g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
			m_grenadeResumeIssued = true;
			m_lastMessage = "Waiting for the selected grenade to be thrown.";
		}

		if (currentTick >= m_grenadeThrowTick - 2 && ResolvePendingGrenade(currentTick)) {
			// Continue into the normal provider path this frame.
		} else if (currentTick > m_grenadeThrowTick + 96) {
			m_lastMessage = "Could not reacquire the selected grenade after its throw tick.";
			advancedfx::Warning("[followcam] grenade track failed: handle %llu did not appear near tick %d.\n",
				(unsigned long long)m_selectedGrenadeHandle, m_grenadeThrowTick);
			StopPreview("grenade could not be reacquired after seek");
			return;
		} else {
			CameraBridge_SetFreeCamEnabled(true);
			CameraBridge_SetCameraPose(
				m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
				m_state.cameraAngles.pitch, m_state.cameraAngles.yaw, m_state.cameraAngles.roll, m_state.fov);
			return;
		}
	}

	auto provider = MakeProvider();
	if (!provider) return;

	// The requested mount belongs to the shot. A temporary missing attachment on another
	// weapon must never overwrite it: the provider retries it when that weapon returns.

	if (m_state.targetType == FollowTargetType::Player) {
		CEntityInstance* pawn = EntityAt(m_state.targetEntityIndex);
		if (pawn && pawn->IsPlayerPawn() && pawn->GetHealth() > 0) {
			m_deathRetargetElapsed = 0.0;
			auto weapon = pawn->GetActiveWeaponHandle();
			if (weapon.IsValid()) m_lastPlayerWeaponIndex = weapon.GetEntryIndex();
			m_lastPlayerBombIndex = -1;
			const int pawnIndex = pawn->GetHandle().IsValid() ? pawn->GetHandle().GetEntryIndex() : m_state.targetEntityIndex;
			for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
				CEntityInstance* entity = EntityAt(i);
				if (!entity || !IsBombClass(EntityClass(entity))) continue;
				auto owner = entity->GetOwnerEntityHandle();
				if (owner.IsValid() && owner.GetEntryIndex() == pawnIndex) {
					m_lastPlayerBombIndex = i;
					break;
				}
			}
		}
	}

	const bool targetDead = provider->GetStatus() == FollowTargetStatus::Dead;
	if (targetDead) {
		m_deathRetargetElapsed += deathDt;
		const bool wantsRetarget = m_state.switchToDroppedWeaponOnDeath || m_state.switchToDroppedBombOnDeath;
		const bool holdExpired = m_deathRetargetElapsed >= m_state.deathHoldSeconds;
		bool retargeted = false;
		if (wantsRetarget && holdExpired) {
			retargeted = TryDeathRetarget();
			if (retargeted) provider = MakeProvider();
		}
		// Follow the moving corpse during the hold, then retarget or release. Allow the
		// dropped entity a short grace when it has not appeared by the end of the hold.
		const bool stop = !retargeted && m_state.autoDisableOnDeath && holdExpired
			&& (!wantsRetarget || m_deathRetargetElapsed >= m_state.deathHoldSeconds + 0.75);
		if (MvmDebugLog_Active() && (stop || retargeted || (m_deathDebugFrame++ % 20) == 0))
			MvmDebugLog_Linef("follow.deathhold",
				"mode=%d entity=%d tick=%d valid=%d elapsed=%.3f hold=%.3f autoDead=%d retarget=%d stop=%d",
				(int)m_state.mode, m_state.targetEntityIndex, frameTick, provider && provider->IsValid(),
				m_deathRetargetElapsed, m_state.deathHoldSeconds, m_state.autoDisableOnDeath,
				retargeted, stop);
		if (stop) { StopPreview("death hold finished"); return; }
		if (retargeted) m_deathRetargetElapsed = 0.0;
	} else {
		m_deathRetargetElapsed = 0.0;
	}
	if (!provider) return;
	if (!provider->IsValid() && (!m_state.holdLastKnownPosition || !m_haveLastKnownTarget)) {
		StopPreview(targetDead ? "ragdoll no longer available" : "target disappeared");
		return;
	}

	FollowVec3 rawTarget = m_haveLastKnownTarget ? m_lastKnownTarget : FollowVec3{};
	FollowAngles rawAngles{};
	bool rawOriented = false;
	const bool validNow = provider->IsValid();
	if (!validNow && m_state.mode == FollowMode::Attach) {
		// Hold the actual last camera pose, not a stale target combined with a new frame.
		m_attachRebaseValid = false;
		return;
	}
	if (validNow) {
		rawOriented = provider->GetWorldTransform(rawTarget, rawAngles);
		if (!IsFiniteOrigin(rawTarget)) {
			StopPreview("target position invalid");
			return;
		}
		if (m_state.mode == FollowMode::Attach) {
		if (!demoAdvancing) {
			// Frozen demo: hold still. No velocity (so no spurious prediction / face-travel).
			m_targetVelocity = {};
		} else if (m_haveLastRawTarget && dt > 0.0001) {
			const double distance = FollowDistance(m_lastRawTarget, rawTarget);
			if (distance > kTeleportDistance) {
				m_motionInitialized = false;
				m_targetVelocity = {};
			} else {
				m_targetVelocity = FollowVec3{
					(rawTarget.x - m_lastRawTarget.x) / dt,
					(rawTarget.y - m_lastRawTarget.y) / dt,
					(rawTarget.z - m_lastRawTarget.z) / dt
				};
			}
		}
		m_lastRawTarget = rawTarget;
		m_haveLastRawTarget = true;
		}
		m_lastKnownTarget = rawTarget;
		m_haveLastKnownTarget = true;
	}
	if (MvmDebugLog_Active() && (m_deathDebugFrame++ % 20) == 0) {
		const auto sampled = ResolveAttachTransform(EntityAt(provider->EntityIndex()), provider->EntityIndex(),
			m_state.targetType, m_state.mode == FollowMode::Attach ? m_state.attachmentName : "eyes", &m_playerPose, m_state.mode == FollowMode::Attach);
		MvmDebugLog_Linef("follow.sample", "mode=%d entity=%d tick=%d dead=%d valid=%d source=%s calibrated=%d raw=(%.3f %.3f %.3f) elapsed=%.3f",
			(int)m_state.mode, provider->EntityIndex(), frameTick, targetDead, validNow,
			sampled.source.c_str(), m_playerPose.calibrated, rawTarget.x, rawTarget.y, rawTarget.z, m_deathRetargetElapsed);
	}

	// ============================ ATTACH (ride-along) =========================
	// The camera rides the target: its position is the target/attachment point plus a
	// LOCAL offset and a composed local rotation. Translation never changes the mount axes.
	if (m_state.mode == FollowMode::Attach) {
		FollowAngles baseAng = rawAngles;
		if (!rawOriented) {
			baseAng = m_motionInitialized ? m_baseAngles : m_state.cameraAngles;
		}
		m_baseAngles = baseAng; // remember the trim-free base for the next stationary hold
		const FollowVec3 worldOffset = FollowRotateVector(m_state.offset, baseAng);
		const FollowVec3 desiredPos{
			rawTarget.x + worldOffset.x + m_targetVelocity.x * m_state.prediction,
			rawTarget.y + worldOffset.y + m_targetVelocity.y * m_state.prediction,
			rawTarget.z + worldOffset.z + m_targetVelocity.z * m_state.prediction
		};
		const FollowAngles desiredAng = FollowComposeRotation(baseAng, m_state.rotationOffset);
		if (!m_motionInitialized) {
			m_smoothedCamPos = desiredPos;
			m_outputAngles = desiredAng;
			m_motionInitialized = true;
		} else {
			m_smoothedCamPos = FollowSmoothPosition(
				m_smoothedCamPos, desiredPos, dt, m_state.positionSmoothing);
			m_outputAngles = FollowSmoothAngles(m_outputAngles, desiredAng, dt,
				m_state.lookSmoothing, m_state.deadzone, m_state.maxTurnSpeed);
			m_outputAngles.roll = desiredAng.roll; // smoothing covers pitch/yaw only
		}
		m_lastCamPos = m_smoothedCamPos;
		m_lastCamAng = m_outputAngles;
		m_lastTargetPos = rawTarget;
		// Stash the rebase basis for ViewSetupAttachOverride: the smoothed camera's offset from the
		// (tick-stepped) target. When the mount has orientation, store it in local axes so render-time
		// rebase rotates it with the fresh bone/player orientation instead of carrying a stale
		// world-space offset across the interpolation boundary.
		const FollowVec3 attachWorldOffset{
			m_smoothedCamPos.x - rawTarget.x,
			m_smoothedCamPos.y - rawTarget.y,
			m_smoothedCamPos.z - rawTarget.z };
		m_attachOffsetFromTarget = rawOriented
			? FollowInverseRotateVector(attachWorldOffset, baseAng)
			: attachWorldOffset;
		m_attachOffsetIsLocal = rawOriented;
		m_attachBaseAngles = baseAng;
		m_attachAngleOffsetFromBase = FollowRelativeRotation(m_outputAngles, baseAng);
		m_attachBaseOriented = rawOriented;
		m_attachSteppedTarget = rawTarget;
		m_attachRebaseValid = true;
		const double pitchErr = FollowWrapDegrees(desiredAng.pitch - m_outputAngles.pitch);
		const double yawErr = FollowWrapDegrees(desiredAng.yaw - m_outputAngles.yaw);
		AttachTransformResult resolved = ResolveAttachTransform(
			EntityAt(provider->EntityIndex()), provider->EntityIndex(), m_state.targetType, m_state.attachmentName, &m_playerPose, true);
		m_attachDebug.active = true;
		m_attachDebug.valid = validNow && resolved.valid;
		m_attachDebug.oriented = rawOriented;
		m_attachDebug.entityIndex = provider->EntityIndex();
		m_attachDebug.entityHandle = resolved.entityHandle;
		m_attachDebug.entityType = FollowTargetTypeName(m_state.targetType);
		m_attachDebug.className = resolved.className;
		m_attachDebug.modelName = resolved.modelName;
		m_attachDebug.attachId = m_state.attachmentName;
		m_attachDebug.attachKind = resolved.attachKind;
		m_attachDebug.attachIndex = resolved.attachIndex;
		m_attachDebug.source = resolved.source;
		m_attachDebug.rawTarget = rawTarget;
		m_attachDebug.rawAngles = rawAngles;
		m_attachDebug.smoothedTarget = rawTarget;
		m_attachDebug.smoothedAngles = m_outputAngles;
		m_attachDebug.cameraPosition = m_smoothedCamPos;
		m_attachDebug.cameraAngles = m_outputAngles;
		m_attachDebug.distance = FollowDistance(m_smoothedCamPos, rawTarget);
		m_attachDebug.cameraDelta = m_motionInitialized && m_havePrevDebug ? FollowDistance(m_prevDebugCamPos, m_smoothedCamPos) : 0.0;
		m_attachDebug.targetDelta = m_havePrevDebug ? FollowDistance(m_prevDebugTargetPos, rawTarget) : 0.0;
		m_attachDebug.angleDelta = m_havePrevDebug
			? std::sqrt(std::pow(FollowWrapDegrees(m_outputAngles.pitch - m_prevDebugCamAng.pitch), 2.0)
				+ std::pow(FollowWrapDegrees(m_outputAngles.yaw - m_prevDebugCamAng.yaw), 2.0))
			: 0.0;
		m_attachDebug.aimError = std::sqrt(pitchErr * pitchErr + yawErr * yawErr);
		m_attachDebug.jitter = (m_attachDebug.cameraDelta > m_attachDebug.targetDelta + 0.01)
			? (m_attachDebug.cameraDelta - m_attachDebug.targetDelta) : 0.0;
		m_attachDebug.smoothing = m_state.positionSmoothing;
		m_attachDebug.previewTick = frameTick;
		m_attachDebug.demoTick = frameTick;
		m_prevDebugCamPos = m_smoothedCamPos;
		m_prevDebugTargetPos = rawTarget;
		m_prevDebugCamAng = m_outputAngles;
		m_havePrevDebug = true;
		const bool sampleAttachDebug = ((m_debugFrame++ % 15) == 0);
		if (MvmDebugLog_Active() && sampleAttachDebug)
			MvmDebugLog_Linef("follow.attach.frame",
				"tick=%d entity=%d owner=%d source=%d oriented=%d raw=(%.3f,%.3f,%.3f) desired=(%.3f,%.3f,%.3f) output=(%.3f,%.3f,%.3f) camDelta=%.4f targetDelta=%.4f jitter=%.4f",
				frameTick, provider->EntityIndex(), m_state.weaponPlayerIndex, (int)m_state.weaponSource,
				rawOriented ? 1 : 0, rawTarget.x, rawTarget.y, rawTarget.z,
				desiredPos.x, desiredPos.y, desiredPos.z,
				m_smoothedCamPos.x, m_smoothedCamPos.y, m_smoothedCamPos.z,
				m_attachDebug.cameraDelta, m_attachDebug.targetDelta, m_attachDebug.jitter);
		CameraBridge_SetFreeCamEnabled(true);
		CameraBridge_SetCameraPose(
			m_smoothedCamPos.x, m_smoothedCamPos.y, m_smoothedCamPos.z,
			m_outputAngles.pitch, m_outputAngles.yaw, m_outputAngles.roll, m_state.fov);
		if (m_debug && sampleAttachDebug) {
			advancedfx::Message(
				"[followcam][attachdebug] type=%s entity=%d handle=%llu attach=%s kind=%s valid=%d source=%s "
				"target=(%.1f %.1f %.1f) cam=(%.1f %.1f %.1f) ang=(%.1f %.1f %.1f) "
				"dist=%.1f camDelta=%.2f targetDelta=%.2f angleDelta=%.2f mountError=%.2f jitter=%.2f tick=%d fov=%.1f.\n",
				FollowTargetTypeName(m_state.targetType), m_attachDebug.entityIndex,
				(unsigned long long)m_attachDebug.entityHandle, m_attachDebug.attachId.c_str(),
				m_attachDebug.attachKind.c_str(), m_attachDebug.valid ? 1 : 0, m_attachDebug.source.c_str(),
				rawTarget.x, rawTarget.y, rawTarget.z,
				m_smoothedCamPos.x, m_smoothedCamPos.y, m_smoothedCamPos.z,
				m_outputAngles.pitch, m_outputAngles.yaw, m_outputAngles.roll,
				m_attachDebug.distance, m_attachDebug.cameraDelta, m_attachDebug.targetDelta,
				m_attachDebug.angleDelta, m_attachDebug.aimError, m_attachDebug.jitter,
				frameTick, m_state.fov);
		}
		return;
	}

	// Lock-on is solved at view setup against the same interpolated pose as the model.
	// Keep this earlier sample only for the diagnostic comparison; never advance its filters here.
	m_lastTargetPos = rawTarget;
}

bool FollowCamera::ViewSetupLockOnOverride(float curTime, double wallDt,
	double& x, double& y, double& z, double& pitch, double& yaw, double& roll, double& fov) {
	if (!m_state.enabled || m_state.mode != FollowMode::LockOn || !m_state.hasCamera || m_grenadeTrackPending)
		return false;
	bool paused = false;
	if (g_pEngineToClient) {
		if (auto demo = g_pEngineToClient->GetDemoFile()) paused = demo->IsDemoPaused();
	}
	const auto clock = FollowAdvanceRenderClock(m_lockOnClock, g_MirvTime.framecount_get(), curTime, wallDt, paused);
	if (!clock.newFrame && m_motionInitialized) {
		x = m_lastCamPos.x; y = m_lastCamPos.y; z = m_lastCamPos.z;
		pitch = m_lastCamAng.pitch; yaw = m_lastCamAng.yaw; roll = m_lastCamAng.roll; fov = m_state.fov;
		return true;
	}
	auto provider = MakeProvider();
	if (!provider) return false;
	const bool validNow = provider->IsValid();
	if (!validNow && (!m_state.holdLastKnownPosition || !m_haveLastKnownTarget)) return false;
	FollowVec3 rawTarget = m_lastKnownTarget;
	FollowAngles ignored;
	if (validNow) provider->GetWorldTransform(rawTarget, ignored);
	if (!IsFiniteOrigin(rawTarget)) return false;
	const auto handle = validNow ? EntityHandleValue(EntityAt(provider->EntityIndex())) : 0;
	const bool continuous = validNow && m_lockOnHaveTarget && handle == m_lockOnPreviousHandle;
	const double distance = continuous ? FollowDistance(rawTarget, m_lockOnPreviousTarget) : 0.0;
	m_targetVelocity = {};
	if (continuous && distance > kTeleportDistance) m_motionInitialized = false;
	else if (continuous && clock.playback > 0.000001) {
		m_targetVelocity = {(rawTarget.x-m_lockOnPreviousTarget.x)/clock.playback,
			(rawTarget.y-m_lockOnPreviousTarget.y)/clock.playback, (rawTarget.z-m_lockOnPreviousTarget.z)/clock.playback};
	}
	m_lockOnPreviousTarget = rawTarget;
	m_lockOnPreviousHandle = handle;
	m_lockOnHaveTarget = validNow;
	if (validNow) { m_lastKnownTarget = rawTarget; m_haveLastKnownTarget = true; }
	const double dt = clock.smoothing;
	// ============================ LOCK-ON (turret) ===========================
	FollowVec3 desiredTarget{
		rawTarget.x + m_state.offset.x + m_targetVelocity.x * m_state.prediction,
		rawTarget.y + m_state.offset.y + m_targetVelocity.y * m_state.prediction,
		rawTarget.z + m_state.offset.z + m_targetVelocity.z * m_state.prediction
	};

	if (m_retargeting && m_state.retargetBlendTime > 0.0001) {
		m_retargetElapsed += dt;
		const double t = (std::min)(1.0, m_retargetElapsed / m_state.retargetBlendTime);
		const double smooth = t * t * (3.0 - 2.0 * t);
		desiredTarget = FollowVec3{
			m_retargetFrom.x + (desiredTarget.x - m_retargetFrom.x) * smooth,
			m_retargetFrom.y + (desiredTarget.y - m_retargetFrom.y) * smooth,
			m_retargetFrom.z + (desiredTarget.z - m_retargetFrom.z) * smooth
		};
		if (t >= 1.0) m_retargeting = false;
	}

	if (!m_motionInitialized) {
		m_smoothedTarget = desiredTarget;
		m_outputAngles = m_state.cameraAngles;
		m_motionInitialized = true;
	} else {
		m_smoothedTarget = m_state.positionSmoothing <= 0.0001 ? desiredTarget : FollowSmoothPosition(
			m_smoothedTarget, desiredTarget, dt, m_state.positionSmoothing);
	}

	const FollowAngles desiredAngles = FollowLookAt(m_state.cameraPosition, m_smoothedTarget);
	m_outputAngles = (m_state.lookSmoothing <= 0.0001 && m_state.maxTurnSpeed <= 0.0 && m_state.deadzone <= 0.0)
		? desiredAngles : FollowSmoothAngles(
		m_outputAngles, desiredAngles, dt, m_state.lookSmoothing,
		m_state.deadzone, m_state.maxTurnSpeed);

	// Rotation sliders trim the aim (pitch/yaw) and set the roll on top of the solve.
	const FollowAngles aimedAngles{
		FollowWrapDegrees(m_outputAngles.pitch + m_state.rotationOffset.pitch),
		FollowWrapDegrees(m_outputAngles.yaw + m_state.rotationOffset.yaw),
		FollowWrapDegrees(m_state.cameraAngles.roll + m_state.rotationOffset.roll)
	};
	m_lastCamPos = m_state.cameraPosition;
	m_lastCamAng = aimedAngles;

	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetCameraPose(
		m_state.cameraPosition.x, m_state.cameraPosition.y, m_state.cameraPosition.z,
		aimedAngles.pitch, aimedAngles.yaw, aimedAngles.roll, m_state.fov);

	if (m_debug && ((m_debugFrame++ % 15) == 0)) {
		advancedfx::Message(
			"[followcam][tick] type=%s entity=%d valid=%d name=%s dt=%.4f "
			"target=(%.1f %.1f %.1f) predicted=(%.1f %.1f %.1f) "
			"angle=(%.2f %.2f) desired=(%.2f %.2f) distance=%.1f freecam=%d.\n",
			FollowTargetTypeName(m_state.targetType), m_state.targetEntityIndex, validNow ? 1 : 0,
			provider->GetDisplayName().c_str(), dt,
			rawTarget.x, rawTarget.y, rawTarget.z,
			m_smoothedTarget.x, m_smoothedTarget.y, m_smoothedTarget.z,
			m_outputAngles.pitch, m_outputAngles.yaw, desiredAngles.pitch, desiredAngles.yaw,
			FollowDistance(m_state.cameraPosition, rawTarget),
			CameraBridge_GetFreeCamEnabled() ? 1 : 0);
	}

	x = m_lastCamPos.x; y = m_lastCamPos.y; z = m_lastCamPos.z;
	pitch = m_lastCamAng.pitch; yaw = m_lastCamAng.yaw; roll = m_lastCamAng.roll; fov = m_state.fov;
	if (MvmDebugLog_Active() && (m_lockOnProbeFrame++ % 2) == 0) {
		const auto aim = FollowLookAt(m_state.cameraPosition, rawTarget);
		int tick = 0; g_MirvTime.GetCurrentDemoTick(tick);
		MvmDebugLog_Linef("follow.lockon.render",
			"tick=%d entity=%d time=%.6f dt=%.6f playbackDt=%.6f paused=%d fresh=(%.6f %.6f %.6f) stepped=(%.6f %.6f %.6f) camera=(%.6f %.6f %.6f) aim=(%.6f %.6f) output=(%.6f %.6f) error=(%.6f %.6f) look=%.4f position=%.4f velocity=(%.6f %.6f %.6f)",
			tick, provider->EntityIndex(), curTime, dt, clock.playback, paused, rawTarget.x, rawTarget.y, rawTarget.z,
			m_lastTargetPos.x, m_lastTargetPos.y, m_lastTargetPos.z, x, y, z,
			aim.pitch, aim.yaw, pitch, yaw, FollowWrapDegrees(pitch-aim.pitch), FollowWrapDegrees(yaw-aim.yaw),
			m_state.lookSmoothing, m_state.positionSmoothing, m_targetVelocity.x, m_targetVelocity.y, m_targetVelocity.z);
	}
	return true;
}

void FollowCamera::SetOffset(double x, double y, double z) {
	m_state.offset = FollowVec3{ x, y, z };
}

void FollowCamera::SetOffsetAxis(int axis, double value) {
	if (axis == 0) m_state.offset.x = value;
	else if (axis == 1) m_state.offset.y = value;
	else if (axis == 2) m_state.offset.z = value;
}

void FollowCamera::SetRotationOffset(double pitch, double yaw, double roll) {
	m_state.rotationOffset = FollowAngles{ pitch, yaw, roll };
}

void FollowCamera::SetRotationAxis(int axis, double value) {
	if (axis == 0) m_state.rotationOffset.pitch = value;
	else if (axis == 1) m_state.rotationOffset.yaw = value;
	else if (axis == 2) m_state.rotationOffset.roll = value;
}

void FollowCamera::SetFov(double value) {
	m_state.fov = std::clamp(value, 1.0, 170.0);
}

void FollowCamera::ResetFraming() {
	ResetFramingState();
	ResetMotion();
	m_lastMessage = (m_state.mode == FollowMode::Attach)
		? "Attach framing reset to defaults."
		: "Framing reset to defaults.";
	advancedfx::Message("[followcam] %s framing reset to defaults.\n",
		m_state.mode == FollowMode::Attach ? "attach" : "lock-on");
}

void FollowCamera::SetLookSmoothing(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.lookSmoothing = 0.0;
		return;
	}
	m_state.lookSmoothing = std::clamp(value, 0.0, 5.0);
}
void FollowCamera::SetPositionSmoothing(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.positionSmoothing = 0.0;
		return;
	}
	m_state.positionSmoothing = std::clamp(value, 0.0, 5.0);
}
void FollowCamera::SetPrediction(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.prediction = 0.0;
		return;
	}
	m_state.prediction = std::clamp(value, 0.0, 2.0);
}
void FollowCamera::SetDeadzone(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.deadzone = 0.0;
		return;
	}
	m_state.deadzone = std::clamp(value, 0.0, 45.0);
}
void FollowCamera::SetMaxTurnSpeed(double value) {
	if (m_state.mode == FollowMode::Attach) {
		m_state.maxTurnSpeed = 0.0;
		return;
	}
	m_state.maxTurnSpeed = std::clamp(value, 0.0, 4000.0);
}

void FollowCamera::ResetTracking() {
	ResetTrackingState();
	ResetMotion();
	m_lastMessage = (m_state.mode == FollowMode::Attach)
		? "Attach tracking reset to stable defaults."
		: "Advanced tracking reset to defaults.";
	advancedfx::Message("[followcam] %s tracking reset to defaults.\n",
		m_state.mode == FollowMode::Attach ? "attach" : "lock-on");
}

void FollowCamera::SetAttachmentName(const std::string& value) {
	if (!value.empty()) {
		m_state.attachmentName = value.substr(0, 64);
		ResetMotion();
	}
}

void FollowCamera::PrintStatus() const {
	advancedfx::Message(
		"[followcam] enabled=%d hasCamera=%d type=%s entity=%d fov=%.1f "
		"offset=(%.1f %.1f %.1f) look=%.3f pos=%.3f prediction=%.3f "
		"deadzone=%.2f maxTurn=%.1f autoDead=%d deathHold=%.1fs switchWeapon=%d switchBomb=%d hold=%d debug=%d rtsample=%d.\n",
		m_state.enabled ? 1 : 0, m_state.hasCamera ? 1 : 0,
		FollowTargetTypeName(m_state.targetType), m_state.targetEntityIndex, m_state.fov,
		m_state.offset.x, m_state.offset.y, m_state.offset.z,
		m_state.lookSmoothing, m_state.positionSmoothing, m_state.prediction,
		m_state.deadzone, m_state.maxTurnSpeed,
		m_state.autoDisableOnDeath ? 1 : 0, m_state.deathHoldSeconds,
		m_state.switchToDroppedWeaponOnDeath ? 1 : 0,
		m_state.switchToDroppedBombOnDeath ? 1 : 0, m_state.holdLastKnownPosition ? 1 : 0,
		m_debug ? 1 : 0, m_renderTimeSample ? 1 : 0);
}

void FollowCamera::PrintCamPose() const {
	advancedfx::Message(
		"[followcam][campose] enabled=%d mode=%s type=%s pos=(%.2f %.2f %.2f) "
		"ang=(%.2f %.2f %.2f) fov=%.1f target=(%.2f %.2f %.2f).\n",
		m_state.enabled ? 1 : 0,
		m_state.mode == FollowMode::Attach ? "attach" : "lockon",
		FollowTargetTypeName(m_state.targetType),
		m_lastCamPos.x, m_lastCamPos.y, m_lastCamPos.z,
		m_lastCamAng.pitch, m_lastCamAng.yaw, m_lastCamAng.roll, m_state.fov,
		m_lastTargetPos.x, m_lastTargetPos.y, m_lastTargetPos.z);
}

} // namespace Filmmaker
