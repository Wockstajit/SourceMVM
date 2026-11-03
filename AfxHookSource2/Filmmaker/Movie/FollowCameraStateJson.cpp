#include "FollowCamera.h"

#include "FollowCameraHost.h"

#include <Windows.h>
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace Filmmaker {

namespace {
// Distance thresholds for the JSON "distanceLevel" hint (0..3), used only here.
constexpr double kMediumDistance = 1800.0;
constexpr double kFarDistance = 3500.0;
constexpr double kVeryFarDistance = 7000.0;
} // namespace

std::string JsonEscape(const std::string& value) {
	std::ostringstream out;
	for (unsigned char c : value) {
		switch (c) {
		case '\\': out << "\\\\"; break;
		case '"': out << "\\\""; break;
		case '\n': out << "\\n"; break;
		case '\r': out << "\\r"; break;
		case '\t': out << "\\t"; break;
		default:
			if (c < 0x20) out << '?';
			else out << (char)c;
		}
	}
	return out.str();
}

void AppendAttachPointJson(std::ostringstream& o, const FollowAttachPoint& point) {
	o << "{\"id\":\"" << JsonEscape(point.id) << "\""
		<< ",\"label\":\"" << JsonEscape(point.label) << "\""
		<< ",\"kind\":\"" << JsonEscape(point.kind) << "\""
		<< ",\"valid\":" << (point.valid ? "true" : "false")
		<< ",\"index\":" << point.index
		<< ",\"source\":\"" << JsonEscape(point.source) << "\"}";
}

const char* TeamName(int team) {
	return team == 2 ? "T" : (team == 3 ? "CT" : "-");
}

std::string FollowCamera::BuildStateJson() const {
	if (m_state.targetType == FollowTargetType::Weapon)
		EnsureEventsLoaded(); // kick off / refresh the drop-event index in the background
	std::unique_ptr<IFollowTargetProvider> provider = MakeProvider();
	const bool targetValid = provider && provider->IsValid();
	const std::string targetName = provider ? provider->GetDisplayName() : "";
	std::vector<FollowAttachPoint> selectedAttachPoints;
	if (provider && m_state.mode == FollowMode::Attach)
		selectedAttachPoints = FollowHost_AttachPoints(provider->EntityIndex(), m_state.targetType);
	double targetDistance = 0.0;
	if (provider && targetValid)
		targetDistance = FollowDistance(m_state.cameraPosition, provider->GetWorldPosition());

	std::ostringstream o;
	o << std::fixed << std::setprecision(2);
	o << "{";
	o << "\"enabled\":" << (m_state.enabled ? "true" : "false");
	o << ",\"hasCamera\":" << (m_state.hasCamera ? "true" : "false");
	o << ",\"singleCamera\":true";
	o << ",\"repositioning\":" << (m_repositioning ? "true" : "false");
	o << ",\"type\":" << (int)m_state.targetType;
	o << ",\"typeName\":\"" << FollowTargetTypeName(m_state.targetType) << "\"";
	o << ",\"mode\":" << (int)m_state.mode;
	o << ",\"weaponSource\":" << (int)m_state.weaponSource;
	o << ",\"weaponPlayerIndex\":" << m_state.weaponPlayerIndex;
	// Who the weapon list is scoped to (-1 = nobody picked yet, so the list is scene-wide).
	o << ",\"weaponFilterPlayer\":" << m_lastPlayerIndex;
	o << ",\"targetIndex\":" << m_state.targetEntityIndex;
	o << ",\"targetHandle\":" << m_state.targetHandle;
	o << ",\"targetValid\":" << (targetValid ? "true" : "false");
	o << ",\"targetDead\":" << (provider && provider->GetStatus() == FollowTargetStatus::Dead ? "true" : "false");
	o << ",\"deathElapsed\":" << m_deathRetargetElapsed;
	o << ",\"targetName\":\"" << JsonEscape(targetName) << "\"";
	o << ",\"targetDistance\":" << targetDistance;
	o << ",\"distanceLevel\":" << (targetDistance >= kVeryFarDistance ? 3 : targetDistance >= kFarDistance ? 2 : targetDistance >= kMediumDistance ? 1 : 0);
	o << ",\"offset\":[" << m_state.offset.x << "," << m_state.offset.y << "," << m_state.offset.z << "]";
	o << ",\"rotation\":[" << m_state.rotationOffset.pitch << "," << m_state.rotationOffset.yaw << "," << m_state.rotationOffset.roll << "]";
	o << ",\"fov\":" << m_state.fov;
	o << ",\"look\":" << m_state.lookSmoothing;
	o << ",\"position\":" << m_state.positionSmoothing;
	o << ",\"prediction\":" << m_state.prediction;
	o << ",\"deadzone\":" << m_state.deadzone;
	o << ",\"maxTurn\":" << m_state.maxTurnSpeed;
	o << ",\"autoDead\":" << (m_state.autoDisableOnDeath ? "true" : "false");
	o << ",\"deathHold\":" << m_state.deathHoldSeconds;
	o << ",\"switchWeapon\":" << (m_state.switchToDroppedWeaponOnDeath ? "true" : "false");
	o << ",\"switchBomb\":" << (m_state.switchToDroppedBombOnDeath ? "true" : "false");
	o << ",\"holdLast\":" << (m_state.holdLastKnownPosition ? "true" : "false");
	o << ",\"attachment\":\"" << JsonEscape(m_state.attachmentName) << "\"";
	o << ",\"attachPoints\":[";
	for (size_t ai = 0; ai < selectedAttachPoints.size(); ++ai) {
		if (ai) o << ",";
		AppendAttachPointJson(o, selectedAttachPoints[ai]);
	}
	o << "]";
	o << ",\"message\":\"" << JsonEscape(m_lastMessage) << "\"";
	o << ",\"grenadePending\":" << (m_grenadeTrackPending ? "true" : "false");
	o << ",\"grenadeThrowTick\":" << m_grenadeThrowTick;
	o << ",\"grenadeSeekTick\":" << m_grenadeSeekTick;
	o << ",\"debug\":" << (m_debug ? "true" : "false");
	o << ",\"renderTimeSample\":" << (m_renderTimeSample ? "true" : "false");
	o << ",\"attachDebug\":{";
	o << "\"active\":" << (m_attachDebug.active ? "true" : "false");
	o << ",\"valid\":" << (m_attachDebug.valid ? "true" : "false");
	o << ",\"oriented\":" << (m_attachDebug.oriented ? "true" : "false");
	o << ",\"entityIndex\":" << m_attachDebug.entityIndex;
	o << ",\"entityHandle\":" << m_attachDebug.entityHandle;
	o << ",\"entityType\":\"" << JsonEscape(m_attachDebug.entityType) << "\"";
	o << ",\"className\":\"" << JsonEscape(m_attachDebug.className) << "\"";
	o << ",\"modelName\":\"" << JsonEscape(m_attachDebug.modelName) << "\"";
	o << ",\"attachId\":\"" << JsonEscape(m_attachDebug.attachId) << "\"";
	o << ",\"attachKind\":\"" << JsonEscape(m_attachDebug.attachKind) << "\"";
	o << ",\"attachIndex\":" << m_attachDebug.attachIndex;
	o << ",\"source\":\"" << JsonEscape(m_attachDebug.source) << "\"";
	o << ",\"rawTarget\":[" << m_attachDebug.rawTarget.x << "," << m_attachDebug.rawTarget.y << "," << m_attachDebug.rawTarget.z << "]";
	o << ",\"rawAngles\":[" << m_attachDebug.rawAngles.pitch << "," << m_attachDebug.rawAngles.yaw << "," << m_attachDebug.rawAngles.roll << "]";
	o << ",\"smoothedTarget\":[" << m_attachDebug.smoothedTarget.x << "," << m_attachDebug.smoothedTarget.y << "," << m_attachDebug.smoothedTarget.z << "]";
	o << ",\"smoothedAngles\":[" << m_attachDebug.smoothedAngles.pitch << "," << m_attachDebug.smoothedAngles.yaw << "," << m_attachDebug.smoothedAngles.roll << "]";
	o << ",\"camera\":[" << m_attachDebug.cameraPosition.x << "," << m_attachDebug.cameraPosition.y << "," << m_attachDebug.cameraPosition.z << "]";
	o << ",\"cameraAngles\":[" << m_attachDebug.cameraAngles.pitch << "," << m_attachDebug.cameraAngles.yaw << "," << m_attachDebug.cameraAngles.roll << "]";
	o << ",\"distance\":" << m_attachDebug.distance;
	o << ",\"cameraDelta\":" << m_attachDebug.cameraDelta;
	o << ",\"targetDelta\":" << m_attachDebug.targetDelta;
	o << ",\"angleDelta\":" << m_attachDebug.angleDelta;
	o << ",\"aimError\":" << m_attachDebug.aimError;
	o << ",\"mountAngleError\":" << m_attachDebug.aimError;
	o << ",\"jitter\":" << m_attachDebug.jitter;
	o << ",\"smoothing\":" << m_attachDebug.smoothing;
	o << ",\"previewTick\":" << m_attachDebug.previewTick;
	o << ",\"demoTick\":" << m_attachDebug.demoTick;
	o << "}";
	o << ",\"players\":[";
	auto players = Players();
	for (size_t pi = 0; pi < players.size(); ++pi) {
		if (pi) o << ",";
		const auto& p = players[pi];
		o << "{\"index\":" << p.entityIndex
			<< ",\"userId\":" << FollowHost_PlayerUserId(p.entityIndex)
			<< ",\"name\":\"" << JsonEscape(p.name) << "\""
			<< ",\"team\":\"" << TeamName(p.team) << "\""
			<< ",\"alive\":" << (p.alive ? "true" : "false") << "}";
	}
	o << "]";
	o << ",\"candidates\":[";
	auto candidates = Candidates();
	const size_t limit = (std::min<size_t>)(candidates.size(), 24);
	for (size_t i = 0; i < limit; ++i) {
		if (i) o << ",";
		const auto& c = candidates[i];
		o << "{\"index\":" << c.entityIndex
			<< ",\"handle\":" << c.handle
			<< ",\"name\":\"" << JsonEscape(c.name) << "\""
			<< ",\"className\":\"" << JsonEscape(c.className) << "\""
			<< ",\"team\":\"" << TeamName(c.team) << "\""
			<< ",\"alive\":" << (c.alive ? "true" : "false")
			<< ",\"isWeapon\":" << (c.isWeapon ? "true" : "false")
			<< ",\"isBomb\":" << (c.isBomb ? "true" : "false")
			<< ",\"dropped\":" << (c.dropped ? "true" : "false")
			<< ",\"held\":" << (c.held ? "true" : "false")
			<< ",\"current\":" << (c.current ? "true" : "false")
			<< ",\"ownerIndex\":" << c.ownerIndex
			<< ",\"ownerName\":\"" << JsonEscape(c.ownerName) << "\""
			<< ",\"status\":\"" << JsonEscape(c.status) << "\""
			<< ",\"grenadeType\":\"" << JsonEscape(c.grenadeType) << "\""
			<< ",\"throwTick\":" << c.throwTick
			<< ",\"tickDelta\":" << c.tickDelta
			<< ",\"distance\":" << c.distance
			<< ",\"distanceLevel\":" << (c.distance >= kVeryFarDistance ? 3 : c.distance >= kFarDistance ? 2 : c.distance >= kMediumDistance ? 1 : 0)
			<< ",\"attachments\":[";
		for (size_t ai = 0; ai < c.attachments.size(); ++ai) {
			if (ai) o << ",";
			o << "\"" << JsonEscape(c.attachments[ai]) << "\"";
		}
		o << "],\"attachPoints\":[";
		for (size_t pi = 0; pi < c.attachPoints.size(); ++pi) {
			if (pi) o << ",";
			AppendAttachPointJson(o, c.attachPoints[pi]);
		}
		o << "]}";
	}
	o << "]"; // close candidates array

	// Recorded loadout events (weapon/C4 drops + pickups) from the .fmjson v5 pre-scan.
	const int evStatus = m_eventStatus.load();
	unsigned long long eventScanElapsedMs = 0;
	std::string eventDemoPath;
	std::string eventHelperPath;
	std::string eventError;
	size_t eventCount = 0;
	{
		std::lock_guard<std::mutex> lock(m_eventMutex);
		if (m_eventScanStartMs && evStatus == (int)EventStatus::Loading)
			eventScanElapsedMs = GetTickCount64() - m_eventScanStartMs;
		eventDemoPath = m_eventDemoPath.empty() ? m_eventLoadingPath : m_eventDemoPath;
		eventHelperPath = m_eventHelperPath;
		eventError = m_eventError;
		eventCount = m_events.size();
	}
	o << ",\"eventStatus\":" << evStatus;       // 0 idle 1 loading 2 ready 3 failed
	o << ",\"eventScanElapsedMs\":" << eventScanElapsedMs;
	o << ",\"eventDemoPath\":\"" << JsonEscape(eventDemoPath) << "\"";
	o << ",\"eventHelperPath\":\"" << JsonEscape(eventHelperPath) << "\"";
	o << ",\"eventError\":\"" << JsonEscape(eventError) << "\"";
	o << ",\"eventCount\":" << eventCount;
	o << ",\"selectedEvent\":" << m_selectedEvent;
	o << ",\"events\":[";
	if (m_state.targetType == FollowTargetType::Weapon) {
		auto events = EventsSnapshot();
		const int curTick = FollowHost_DemoTick();
		// Show the events nearest the playhead (a long demo has too many to ship every
		// frame); keep the original index so eventselect maps back to the full list.
		std::vector<size_t> order(events.size());
		for (size_t i = 0; i < events.size(); ++i) order[i] = i;
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
			return std::abs(events[a].tick - curTick) < std::abs(events[b].tick - curTick);
		});
		if (order.size() > 60) order.resize(60);
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
			return events[a].tick < events[b].tick;
		});
		for (size_t k = 0; k < order.size(); ++k) {
			if (k) o << ",";
			const size_t i = order[k];
			const auto& e = events[i];
			o << "{\"index\":" << i
				<< ",\"tick\":" << e.tick
				<< ",\"type\":\"" << JsonEscape(e.type) << "\""
				<< ",\"item\":\"" << JsonEscape(e.item) << "\""
				<< ",\"accountId\":" << e.accountId
				<< ",\"ownerName\":\"" << JsonEscape(e.ownerName) << "\"}";
		}
	}
	o << "]}";
	return o.str();
}

} // namespace Filmmaker
