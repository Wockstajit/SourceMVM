#include "FollowTargetDiscovery.h"

#include "CameraBridge.h"
#include "FollowCameraHost.h"
#include "../../../Shared/AfxConsole.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Filmmaker {

FollowAttachPoint MakeAttachPoint(const char* id, const char* label, const char* kind,
	bool valid, int index, const char* source) {
	FollowAttachPoint out;
	out.id = id ? id : "";
	out.label = label ? label : out.id;
	out.kind = kind ? kind : "";
	out.valid = valid;
	out.index = index;
	out.source = source ? source : "";
	return out;
}

std::vector<std::string> AttachPointIds(const std::vector<FollowAttachPoint>& points) {
	std::vector<std::string> out;
	out.reserve(points.size());
	for (const auto& point : points)
		if (point.valid)
			out.push_back(point.id);
	return out;
}

const char* FollowTargetTypeName(FollowTargetType type) {
	switch (type) {
	case FollowTargetType::Grenade: return "Grenade";
	case FollowTargetType::Weapon: return "Weapon / C4";
	default: return "Player";
	}
}

// The grenades come from the game's sightings (FollowHost_GrenadeSightings). In CS2 that is the
// shared index the scene objects' grenade picker reads too, so both find the same projectiles.
void FollowCamera::UpdateGrenadeCache() const {
	m_grenadeCache.clear();
	for (const GrenadeSighting& s : FollowHost_GrenadeSightings()) {
		GrenadeRecord& record = m_grenadeCache[s.handle];
		record.handle = s.handle;
		record.entityIndex = s.active ? s.entityIndex : -1;
		record.className = s.className;
		record.grenadeType = s.type;
		record.throwerName = s.thrower.empty() ? "Player" : s.thrower;
		record.throwerIndex = s.throwerIndex;
		record.team = s.team;
		record.throwTick = s.throwTick;
		record.active = s.active;
	}
}

std::vector<FollowTargetCandidate> FollowCamera::Candidates() const {
	// Distance is measured from the LIVE view (where the operator is flying / looking), not the
	// placed lock-on camera -- so "Select Nearest" picks the target nearest the current shot and
	// the list readout reflects how close the operator is. Falls back to the placed camera only
	// if the live camera read returns garbage.
	FollowVec3 reference = m_state.cameraPosition;
	{
		double vpos[3] = {}, vang[3] = {}, vfov = 90.0;
		CameraBridge_GetCurrentCamera(vpos, vang, vfov);
		if (std::isfinite(vpos[0]) && std::isfinite(vpos[1]) && std::isfinite(vpos[2]))
			reference = FollowVec3{ vpos[0], vpos[1], vpos[2] };
	}

	if (m_state.targetType == FollowTargetType::Grenade) {
		std::vector<FollowTargetCandidate> out;
		UpdateGrenadeCache();
		const int currentTick = FollowHost_DemoTick();
		for (const auto& pair : m_grenadeCache) {
			const GrenadeRecord& record = pair.second;
			// Browse window: grenades thrown within kGrenadeBrowseTicks either side of the playhead
			// (in the air now, or seen earlier this session).
			if (record.throwTick < 0 || std::abs(record.throwTick - currentTick) > kGrenadeBrowseTicks)
				continue;
			FollowTargetCandidate candidate;
			candidate.entityIndex = record.active ? record.entityIndex : -1;
			candidate.handle = record.handle;
			candidate.name = record.grenadeType;
			candidate.className = record.className;
			candidate.grenadeType = record.grenadeType;
			candidate.ownerIndex = record.throwerIndex;
			candidate.ownerName = record.throwerName;
			candidate.team = record.team;
			candidate.throwTick = record.throwTick;
			candidate.tickDelta = record.throwTick - currentTick;
			candidate.status = record.active ? "active projectile" : "recorded nearby";
			candidate.alive = record.active;
			if (record.active) {
				candidate.attachPoints = FollowHost_AttachPoints(record.entityIndex, FollowTargetType::Grenade);
				candidate.attachments = AttachPointIds(candidate.attachPoints);
				FollowVec3 position;
				bool isPlayer = false;
				if (FollowHost_EntityOrigin(record.entityIndex, position, isPlayer))
					candidate.distance = FollowDistance(reference, position);
			} else {
				candidate.attachPoints.push_back(MakeAttachPoint("entity", "Entity", "entity", true, 0, "recorded-grenade"));
				candidate.attachments = AttachPointIds(candidate.attachPoints);
			}
			out.push_back(std::move(candidate));
		}
		std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
			const int ad = std::abs(a.tickDelta), bd = std::abs(b.tickDelta);
			return ad == bd ? a.throwTick < b.throwTick : ad < bd;
		});
		return out;
	}

	std::vector<FollowTargetCandidate> out =
		FollowHost_FindTargets(m_state.targetType, m_state.mode == FollowMode::Attach, reference);

	// Weapon list: keep it to the player being watched. The scan walks the WHOLE entity list, so
	// on a full server it returns every gun in the round -- dozens of entries, capped at 24 for the
	// UI, in which the one weapon you actually want (the gun in the hands of the guy on screen) is
	// just one row among strangers'. Dropped weapons and the C4 have no owner and are deliberately
	// kept: they belong to nobody, so they are never the clutter being complained about, and they
	// are the whole point of the "Dropped" source mode.
	//
	// The filter is skipped when the spectated pawn cannot be resolved, and when it owns nothing
	// in this list, so it can only ever remove rows that a still-populated list makes redundant --
	// no weapon becomes unreachable.
	if (m_state.targetType == FollowTargetType::Weapon) {
		// WHICH player scopes the list.
		//
		// The first version keyed this on the spectated player alone, and measuring it on a live
		// demo showed the filter silently never applying: there is no single spectated player in
		// third person and free cam BY DESIGN -- and free cam is exactly the mode the camera editor
		// puts you in. The guard below then skipped the filter every time, so the list stayed
		// scene-wide.
		//
		// The player you last picked in Player mode is the better answer anyway: it is the
		// report's own workflow (choose a player, then choose their weapon), it survives free cam,
		// and it is an explicit choice rather than an inference. The spectated pawn stays as the
		// fallback for when no player has been picked yet.
		int watched = m_lastPlayerIndex;
		if (watched < 0) watched = FollowHost_SpectatedPlayer();
		if (watched >= 0)
			out.erase(std::remove_if(out.begin(), out.end(), [this, watched](const FollowTargetCandidate& c) {
				if (m_state.weaponSource == WeaponSource::Held && !c.held) return true;
				if (m_state.weaponSource == WeaponSource::Dropped && !c.dropped) return true;
				return c.held && c.ownerIndex != watched;
			}), out.end());
		else if (m_state.weaponSource != WeaponSource::Auto)
			out.erase(std::remove_if(out.begin(), out.end(), [this](const FollowTargetCandidate& c) {
				return m_state.weaponSource == WeaponSource::Held ? !c.held : !c.dropped;
			}), out.end());
	}

	std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
		// Weapons HELD by a player rank above dropped ones (you almost always want the
		// in-use weapon); within each group, nearest first. Players (held==false for both)
		// fall straight through to the distance compare.
		if (a.held != b.held) return a.held;
		return a.distance < b.distance;
	});
	return out;
}

std::vector<FollowTargetCandidate> FollowCamera::Players() const {
	double pos[3] = {}, ang[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(pos, ang, fov);
	std::vector<FollowTargetCandidate> out =
		FollowHost_FindTargets(FollowTargetType::Player, false, FollowVec3{ pos[0], pos[1], pos[2] });
	std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.distance < b.distance; });
	return out;
}

bool FollowCamera::SelectCandidate(const FollowTargetCandidate& candidate) {
	if (candidate.entityIndex < 0 && !(m_state.targetType == FollowTargetType::Grenade && candidate.handle))
		return false;
	if (m_haveLastKnownTarget) {
		m_retargetFrom = m_lastKnownTarget;
		m_retargeting = true;
		m_retargetElapsed = 0.0;
	}
	m_state.targetEntityIndex = candidate.entityIndex;
	m_state.targetHandle = candidate.handle;
	if (m_state.targetType == FollowTargetType::Grenade)
		m_selectedGrenadeHandle = candidate.handle;
	// Held-weapon tracking re-resolves the player's active weapon each frame; remember
	// the owner so the camera survives weapon switches. Dropped items have no owner.
	if (m_state.targetType == FollowTargetType::Weapon)
		m_state.weaponPlayerIndex = candidate.held ? candidate.ownerIndex : -1;
	// Remember the player so the weapon list can be scoped to them next time. Picking a HELD
	// weapon counts too: its owner is just as explicit a choice of player as Player mode is.
	if (m_state.targetType == FollowTargetType::Player)
		m_lastPlayerIndex = candidate.entityIndex;
	else if (m_state.targetType == FollowTargetType::Weapon && candidate.held)
		m_lastPlayerIndex = candidate.ownerIndex;
	// Keep the requested mount when changing weapons/targets; transient fallback belongs
	// to the resolver, not the saved framing. Returning to the model restores its point.
	m_haveLastRawTarget = false;
	m_highlightTarget = -2; // every pick shows the target box again for a few seconds (PublishTargetHighlight)
	m_lastMessage = "Selected " + candidate.name + ".";
	advancedfx::Message("[followcam] target selected: type=%s entity=%d name=%s distance=%.1f.\n",
		FollowTargetTypeName(m_state.targetType), candidate.entityIndex, candidate.name.c_str(), candidate.distance);
	return true;
}

bool FollowCamera::SelectNearest() {
	auto candidates = Candidates();
	if (candidates.empty()) {
		advancedfx::Warning("[followcam] no %s target found.\n", FollowTargetTypeName(m_state.targetType));
		return false;
	}
	if (m_state.targetType == FollowTargetType::Player) {
		for (const auto& candidate : candidates)
			if (candidate.alive)
				return SelectCandidate(candidate);
	}
	return SelectCandidate(candidates.front());
}

bool FollowCamera::SelectEntity(int entityIndex) {
	auto candidates = Candidates();
	for (const auto& candidate : candidates)
		if (candidate.entityIndex == entityIndex)
			return SelectCandidate(candidate);
	advancedfx::Warning("[followcam] entity %d is not a valid %s target.\n",
		entityIndex, FollowTargetTypeName(m_state.targetType));
	return false;
}

bool FollowCamera::SelectHandle(uint64_t handle) {
	if (!handle) return false;
	if (m_state.targetType != FollowTargetType::Grenade)
		m_state.targetType = FollowTargetType::Grenade;
	UpdateGrenadeCache();
	auto it = m_grenadeCache.find(handle);
	if (it != m_grenadeCache.end()) {
		FollowTargetCandidate candidate;
		candidate.handle = it->second.handle;
		candidate.entityIndex = it->second.active ? it->second.entityIndex : -1;
		candidate.name = it->second.grenadeType;
		candidate.grenadeType = it->second.grenadeType;
		candidate.className = it->second.className;
		candidate.ownerName = it->second.throwerName;
		candidate.ownerIndex = it->second.throwerIndex;
		candidate.team = it->second.team;
		candidate.throwTick = it->second.throwTick;
		candidate.status = it->second.active ? "active projectile" : "recorded nearby";
		return SelectCandidate(candidate);
	}
	advancedfx::Warning("[followcam] grenade handle %llu is no longer in the session index.\n",
		(unsigned long long)handle);
	m_lastMessage = "Selected grenade is no longer in the session index.";
	return false;
}

bool FollowCamera::TrackSelectedGrenade() {
	if (!m_state.hasCamera) {
		advancedfx::Warning("[followcam] grenade tracking refused: place the Follow Camera first.\n");
		m_lastMessage = "Place the Follow Camera before tracking a grenade.";
		return false;
	}
	if (!m_selectedGrenadeHandle) {
		advancedfx::Warning("[followcam] grenade tracking refused: select a grenade first.\n");
		m_lastMessage = "Select a grenade from the nearby list first.";
		return false;
	}
	UpdateGrenadeCache();
	auto it = m_grenadeCache.find(m_selectedGrenadeHandle);
	if (it == m_grenadeCache.end() || it->second.throwTick < 0) {
		advancedfx::Warning("[followcam] grenade tracking refused: throw tick is unavailable.\n");
		m_lastMessage = "The selected grenade has no usable throw tick.";
		return false;
	}

	m_state.targetType = FollowTargetType::Grenade;
	m_state.targetHandle = m_selectedGrenadeHandle;
	m_state.targetEntityIndex = -1;
	m_grenadeThrowTick = it->second.throwTick;
	m_grenadeSeekTick = (std::max)(0, m_grenadeThrowTick - 40);
	m_grenadeTrackPending = true;
	m_grenadeSeekObserved = false;
	m_grenadeResumeIssued = false;
	m_state.enabled = true;
	ResetMotion();
	FollowHost_TakeViewFromOtherCameras();
	CameraBridge_SetPathDrawEnabled(false);
	CameraBridge_SetFreeCamEnabled(true);

	FollowHost_SeekDemo(m_grenadeSeekTick);
	m_lastMessage = "Seeking to tick " + std::to_string(m_grenadeSeekTick)
		+ ", 40 ticks before the throw.";
	advancedfx::Message("[followcam] grenade track armed: %s by %s, throwTick=%d seekTick=%d handle=%llu.\n",
		it->second.grenadeType.c_str(), it->second.throwerName.c_str(),
		m_grenadeThrowTick, m_grenadeSeekTick, (unsigned long long)m_selectedGrenadeHandle);
	return true;
}

bool FollowCamera::ResolvePendingGrenade(int currentTick) {
	auto it = m_grenadeCache.find(m_selectedGrenadeHandle);
	if (it == m_grenadeCache.end() || !it->second.active) {
		const std::string expectedType = it != m_grenadeCache.end() ? it->second.grenadeType : "";
		auto best = m_grenadeCache.end();
		for (auto candidate = m_grenadeCache.begin(); candidate != m_grenadeCache.end(); ++candidate) {
			if (!candidate->second.active) continue;
			if (!expectedType.empty() && candidate->second.grenadeType != expectedType) continue;
			if (std::abs(candidate->second.throwTick - m_grenadeThrowTick) > 4) continue;
			best = candidate;
			break;
		}
		if (best == m_grenadeCache.end())
			return false;
		it = best;
		m_selectedGrenadeHandle = it->first;
	}
	m_state.targetEntityIndex = it->second.entityIndex;
	m_state.targetHandle = it->second.handle;
	m_grenadeTrackPending = false;
	ResetMotion();
	m_lastMessage = "Tracking " + it->second.grenadeType + " from throw tick "
		+ std::to_string(it->second.throwTick) + ".";
	advancedfx::Message("[followcam] grenade acquired at tick %d: entity=%d handle=%llu.\n",
		currentTick, m_state.targetEntityIndex, (unsigned long long)m_state.targetHandle);
	return true;
}

bool FollowCamera::TryDeathRetarget() {
	auto trySwitch = [&](int entityIndex) -> bool {
		if (entityIndex < 0) return false;
		const FollowTargetType oldType = m_state.targetType;
		const WeaponSource oldSrc = m_state.weaponSource;
		m_state.targetType = FollowTargetType::Weapon;
		m_state.weaponSource = WeaponSource::Dropped;
		m_state.weaponPlayerIndex = -1;
		if (SelectEntity(entityIndex)) return true;
		m_state.targetType = oldType;
		m_state.weaponSource = oldSrc;
		return false;
	};
	if (m_state.switchToDroppedBombOnDeath && trySwitch(m_lastPlayerBombIndex)) return true;
	if (m_state.switchToDroppedWeaponOnDeath && trySwitch(m_lastPlayerWeaponIndex)) return true;
	return false;
}

} // namespace Filmmaker
