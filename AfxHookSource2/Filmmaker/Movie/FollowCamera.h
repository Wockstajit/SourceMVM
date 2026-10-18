#pragma once

#include "FollowCameraMath.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Logical target groups. C4 and dropped/held weapons all live under Weapon and are
// distinguished by class checks + WeaponSource. Attachment is now a MODE, not a type.
enum class FollowTargetType {
	Player = 0,
	Grenade = 1,
	Weapon = 2
};

// Turret (camera placed at a fixed point, only rotates to track the target) vs rigid
// ride-along (camera position + orientation are derived from the target every frame,
// with a local offset / rotation trim / FOV applied).
enum class FollowMode {
	LockOn = 0,
	Attach = 1
};

// For Weapon targets: follow the player's active (held) weapon, the dropped weapon
// entity on the ground, or auto-pick based on the selected candidate's owner.
enum class WeaponSource {
	Auto = 0,
	Held = 1,
	Dropped = 2
};

enum class FollowTargetStatus {
	Missing = 0,
	Alive = 1,
	Dead = 2,
	Active = 3
};

struct FollowAttachPoint {
	std::string id;
	std::string label;
	std::string kind;
	bool valid = false;
	int index = -1;
	std::string source;
};

struct FollowTargetCandidate {
	int entityIndex = -1;
	uint64_t handle = 0;
	std::string name;
	std::string className;
	int team = 0;
	bool alive = false;
	double distance = 0.0;
	bool isWeapon = false;
	bool isBomb = false;
	bool dropped = false;
	bool held = false;
	bool current = false;
	int ownerIndex = -1;
	std::string ownerName;
	std::string status;
	std::string grenadeType;
	int throwTick = -1;
	int tickDelta = 0;
	std::vector<std::string> attachments;
	std::vector<FollowAttachPoint> attachPoints;
};

// A recorded loadout event from the demo pre-scan (.fmjson v5): weapon/C4 drops and
// pickups. Surfaced as the Events list so Preview Tick can jump to the moment.
struct FollowEventRecord {
	int tick = -1;
	std::string type;       // "weapon_drop" | "bomb_dropped" | "bomb_pickup" | "item_pickup"
	std::string item;       // e.g. "weapon_ak47", "c4"
	uint32_t accountId = 0;  // player involved (Steam account id; 0 = unknown)
	std::string ownerName;   // resolved from the scoreboard when available
};

// Framing and tracking belong to a mode; target selection and camera placement are shared.
struct FollowCameraSettings {
	double fov = 90.0;
	FollowVec3 offset;
	FollowAngles rotationOffset; // attach: applied on top of the attachment orientation;
	                             // lock-on: aim trim added after the look-at solve.
	double lookSmoothing = 0.10;
	double positionSmoothing = 0.04;
	double prediction = 0.0;
	double deadzone = 0.0;
	double maxTurnSpeed = 720.0;
	double blendInTime = 0.10;
	double retargetBlendTime = 0.30;
	bool autoDisableOnDeath = true;
	// Playback seconds following the corpse before stopping or retargeting. With auto-disable
	// and both retarget options off, follow the corpse for as long as it is available.
	double deathHoldSeconds = 3.0;
	bool switchToDroppedWeaponOnDeath = false;
	bool switchToDroppedBombOnDeath = false;
	bool holdLastKnownPosition = true;
	std::string attachmentName = "head";
};

struct FollowCameraState : FollowCameraSettings {
	bool enabled = false;
	bool hasCamera = false;
	FollowVec3 cameraPosition;
	FollowAngles cameraAngles;
	FollowTargetType targetType = FollowTargetType::Player;
	FollowMode mode = FollowMode::LockOn;
	WeaponSource weaponSource = WeaponSource::Auto;
	int targetEntityIndex = -1;
	int weaponPlayerIndex = -1;
	uint64_t targetHandle = 0;
};

// Calibrate virtual player mounts against the living skeleton, then carry that transform
// with the same body attachment through the ragdoll. Never use a dead pawn's render eye.
struct FollowPlayerPose {
	uint64_t handle = 0;
	std::string point;
	bool calibrated = false;
	FollowVec3 localPosition, localForward, localUp;
};

class IFollowTargetProvider {
public:
	virtual ~IFollowTargetProvider() = default;
	virtual bool IsValid() = 0;
	virtual FollowVec3 GetWorldPosition() = 0;
	// Rigid-attach transform: fills pos (+ ang when oriented) for ride-along mode.
	// Returns true when an orientation is available (attachment quat / render angles),
	// false when only a position is known (caller then keeps the placed orientation).
	virtual bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang) {
		pos = GetWorldPosition();
		(void)ang;
		return false;
	}
	virtual std::string GetDisplayName() = 0;
	virtual FollowTargetStatus GetStatus() = 0;
	virtual int EntityIndex() const = 0;
};

class FollowCamera {
public:
	void RunFrame(bool cameraEditorActive);
	// Publishes the in-world box around the selected target. Runs whether or not the follow
	// camera is active -- browsing candidates is exactly when you need to see which entity a
	// row refers to.
	void PublishTargetHighlight();
	// True while that box is being published. Read by CameraPath::EnsureDrawState, which owns the
	// master gizmo flag the box is drawn under.
	bool WantsTargetHighlight() const { return m_wantTargetHighlight; }

	// Cancels and joins the background loadout-event loader so a DLL unload can't leave
	// a detached worker writing into freed singleton state. Called from Filmmaker::Shutdown.
	void Shutdown();

	void PlaceCamera();
	void BeginReposition();
	void PlaceReposition();
	void CancelReposition();
	bool CaptureAttachPoseFromCurrentView();
	void ClearCamera();
	void Preview();
	void StopPreview(const char* reason = nullptr);
	void SetTargetType(FollowTargetType type);
	void SetMode(FollowMode mode);
	void SetWeaponSource(WeaponSource source);
	bool SetWeaponPlayer(int pawnIndex);
	bool GoToTarget();
	bool SelectNearest();
	bool SelectEntity(int entityIndex);
	bool SelectHandle(uint64_t handle);
	bool TrackSelectedGrenade();
	bool SelectEvent(int eventIndex);   // pick a recorded drop/pickup event
	bool PreviewTick();                 // jump the demo to just before the selected event

	void SetOffset(double x, double y, double z);
	void SetOffsetAxis(int axis, double value); // 0=x 1=y 2=z (slider granularity)
	void SetRotationOffset(double pitch, double yaw, double roll);
	void SetRotationAxis(int axis, double value); // 0=pitch 1=yaw 2=roll
	void SetFov(double value);
	void ResetFraming();
	void SetLookSmoothing(double value);
	void SetPositionSmoothing(double value);
	void SetPrediction(double value);
	void SetDeadzone(double value);
	void SetMaxTurnSpeed(double value);
	void ResetTracking();
	void SetAttachmentName(const std::string& value);
	void SetAutoDisableOnDeath(bool value) { m_state.autoDisableOnDeath = value; }
	void SetDeathHoldSeconds(double value) { if (std::isfinite(value)) m_state.deathHoldSeconds = value < 0.0 ? 0.0 : value; }
	void SetSwitchToDroppedWeapon(bool value) { m_state.switchToDroppedWeaponOnDeath = value; }
	void SetSwitchToDroppedBomb(bool value) { m_state.switchToDroppedBombOnDeath = value; }
	void SetHoldLastKnown(bool value) { m_state.holdLastKnownPosition = value; }
	void SetDebug(bool value) { m_debug = value; }
	void SetRenderTimeSample(bool value) { m_renderTimeSample = value; }
	bool RenderTimeSample() const { return m_renderTimeSample; }

	// Called from the view-setup trampoline (runs DURING rendering, after the client interpolates
	// entities). When an attach preview is active it RE-SAMPLES the target at render time -- the
	// interpolated pose the model is actually drawn at, not the tick-stepped pose RunFrame read at
	// FrameStageNotify -- and rebases the camera onto it, which removes the ride-along jitter.
	// Returns true (and writes the view) only when m_renderTimeSample is on and a target is live.
	bool ViewSetupAttachOverride(float curTime,
		double& x, double& y, double& z, double& pitch, double& yaw, double& roll, double& fov);
	// Lock-on aims and smooths against the interpolated target once per rendered frame.
	bool ViewSetupLockOnOverride(float curTime, double wallDt,
		double& x, double& y, double& z, double& pitch, double& yaw, double& roll, double& fov);

	// Attach mode rides the target and needs no placed camera; lock-on requires Place.
	bool OwnsView() const { return m_state.enabled && (m_state.hasCamera || m_state.mode == FollowMode::Attach); }
	bool Repositioning() const { return m_repositioning; }
	bool Debug() const { return m_debug; }
	const FollowCameraState& State() const { return m_state; }
	const std::string& LastMessage() const { return m_lastMessage; }
	std::vector<FollowTargetCandidate> Candidates() const;
	std::vector<FollowTargetCandidate> Players() const;
	std::string BuildStateJson() const;
	void PrintStatus() const;
	void PrintCamPose() const;   // instrumentation: dump computed pose + target world pos

private:
	std::unique_ptr<IFollowTargetProvider> MakeProvider() const;
	bool SelectCandidate(const FollowTargetCandidate& candidate);
	bool TryDeathRetarget();
	bool ResolvePendingGrenade(int currentTick);
	void UpdateGrenadeCache() const;
	void ResetMotion();
	void ResetFramingState();
	void ResetTrackingState();
	void ApplyAttachTrackingDefaults();
	double WallDt();

	// Background load of the current demo's loadout events (.fmjson v5). Kicks off a
	// detached worker the first time the events are needed for a given demo path.
	void EnsureEventsLoaded() const;
	std::vector<FollowEventRecord> EventsSnapshot() const;

	struct GrenadeRecord {
		uint64_t handle = 0;
		int entityIndex = -1;
		std::string className;
		std::string grenadeType;
		std::string throwerName;
		int throwerIndex = -1;
		int team = 0;
		int throwTick = -1;
		bool active = false;
	};

	FollowCameraState m_state;
	mutable FollowPlayerPose m_playerPose;
	FollowCameraSettings m_modeSettings[2];
	bool m_haveModeSettings[2] = { false, false };
	FollowAngles m_outputAngles;
	FollowAngles m_baseAngles;   // attach: orientation WITHOUT the rotation trim, so holding it
	                             // while stationary doesn't re-add the trim each frame (spin fix)
	int m_lastFrameTick = -1;    // last demo tick seen in RunFrame; used to detect a paused demo
	bool m_wantTargetHighlight = false; // a target box is currently being published
	// The box shows for kTargetHighlightMs after a target is picked (FollowCamera.cpp), then goes.
	int m_highlightTarget = -2;
	FollowTargetType m_highlightType = FollowTargetType::Player;
	unsigned long long m_highlightSinceMs = 0;
	// Last player picked in Player mode. Scopes the WEAPON list to that player's weapons, which
	// is the report's workflow (choose a player, then their weapon) and the only owner signal
	// that survives free cam -- AfxGetSpectatedPawnIndex returns -1 there by design.
	int m_lastPlayerIndex = -1;
	FollowVec3 m_smoothedTarget;
	FollowVec3 m_smoothedCamPos; // attach mode: smoothed camera position

	// --- attach render-time rebase (jitter fix) ---
	bool m_renderTimeSample = true;        // re-sample attach target at view setup (toggle: follow rtsample)
	bool m_attachRebaseValid = false;      // RunFrame produced a usable attach pose this frame
	FollowVec3 m_attachOffsetFromTarget;   // smoothedCam - steppedTarget; local-space when oriented
	bool m_attachOffsetIsLocal = false;    // true when the offset should rotate with render-time base
	FollowAngles m_attachBaseAngles;       // trim-free base used by RunFrame
	FollowAngles m_attachAngleOffsetFromBase; // inverse(base) * output rotation, preserved across rebase
	bool m_attachBaseOriented = false;     // angle rebase is valid only when RunFrame had orientation
	FollowVec3 m_attachSteppedTarget;      // tick-stepped target RunFrame used (diagnostics)
	FollowVec3 m_prevAttachFresh;          // previous render-time sample (frame-to-frame smoothness check)
	bool m_havePrevAttachFresh = false;
	unsigned m_debugViewFrame = 0;
	unsigned m_lockOnProbeFrame = 0;
	FollowRenderClock m_lockOnClock;
	FollowVec3 m_lockOnPreviousTarget;
	uint64_t m_lockOnPreviousHandle = 0;
	bool m_lockOnHaveTarget = false;
	FollowVec3 m_lastRawTarget;
	FollowVec3 m_lastKnownTarget;
	FollowVec3 m_targetVelocity;
	FollowVec3 m_retargetFrom;
	bool m_motionInitialized = false;
	bool m_haveLastRawTarget = false;
	bool m_haveLastKnownTarget = false;
	bool m_retargeting = false;
	double m_retargetElapsed = 0.0;
	double m_deathRetargetElapsed = 0.0;
	unsigned m_deathDebugFrame = 0;
	int m_lastPlayerWeaponIndex = -1;
	int m_lastPlayerBombIndex = -1;
	long long m_lastQpc = 0;
	bool m_debug = false;
	bool m_repositioning = false;
	bool m_resumePreviewAfterReposition = false;
	uint64_t m_selectedGrenadeHandle = 0;
	bool m_grenadeTrackPending = false;
	bool m_grenadeSeekObserved = false;
	bool m_grenadeResumeIssued = false;
	int m_grenadeThrowTick = -1;
	int m_grenadeSeekTick = -1;
	std::string m_lastMessage;
	mutable std::unordered_map<uint64_t, GrenadeRecord> m_grenadeCache;
	unsigned m_debugFrame = 0;

	struct AttachDebugSnapshot {
		bool active = false;
		bool valid = false;
		bool oriented = false;
		int entityIndex = -1;
		uint64_t entityHandle = 0;
		std::string entityType;
		std::string className;
		std::string modelName;
		std::string attachId;
		std::string attachKind;
		int attachIndex = -1;
		std::string source;
		FollowVec3 rawTarget;
		FollowAngles rawAngles;
		FollowVec3 smoothedTarget;
		FollowAngles smoothedAngles;
		FollowVec3 cameraPosition;
		FollowAngles cameraAngles;
		double distance = 0.0;
		double cameraDelta = 0.0;
		double targetDelta = 0.0;
		double angleDelta = 0.0;
		double aimError = 0.0;
		double jitter = 0.0;
		double smoothing = 0.0;
		int previewTick = 0;
		int demoTick = 0;
	};
	AttachDebugSnapshot m_attachDebug;
	FollowVec3 m_prevDebugCamPos;
	FollowVec3 m_prevDebugTargetPos;
	FollowAngles m_prevDebugCamAng;
	bool m_havePrevDebug = false;

	// Selected recorded event + last computed pose (instrumentation readout).
	int m_selectedEvent = -1;
	FollowVec3 m_lastCamPos;
	FollowAngles m_lastCamAng;
	FollowVec3 m_lastTargetPos;

	// Demo loadout-event index, loaded lazily on a background thread.
	enum class EventStatus { Idle = 0, Loading = 1, Ready = 2, Failed = 3 };
	mutable std::mutex m_eventMutex;
	mutable std::string m_eventDemoPath;     // demo path the loaded events belong to
	mutable std::string m_eventLoadingPath;  // path of the in-flight background load
	mutable std::atomic<int> m_eventStatus{ (int)EventStatus::Idle };
	mutable std::vector<FollowEventRecord> m_events;
	mutable unsigned long long m_eventScanStartMs = 0;
	mutable std::string m_eventError;
	mutable std::string m_eventHelperPath;
	// The background loader is joinable (not detached) so it can be cancelled + joined on
	// shutdown; m_eventCancel aborts the in-flight helper process within ~50ms.
	mutable std::thread m_eventThread;
	mutable std::atomic<bool> m_eventCancel{ false };
};

FollowCamera& FollowCameraRef();
const char* FollowTargetTypeName(FollowTargetType type);

// Console command entry: handles "mirv_filmmaker follow ...". argc/args/cmd are forwarded
// from FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(1) == "follow").
void Follow_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker
