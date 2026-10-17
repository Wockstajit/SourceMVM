#include "FollowTargetDiscovery.h"

#include "CameraBridge.h"
#include "CameraPath.h"
#include "FollowTargetProviders.h"
#include "../Panorama/GraphEditorExperimentHud.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <algorithm>
#include <cmath>
#include <sstream>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Only used by UpdateGrenadeCache/Candidates below -- kept file-local (not exposed via
// FollowTargetProviders.h, which the provider classes don't call this from).
void ResolveOwner(CEntityInstance* entity, int& ownerIndex, std::string& ownerName, int& ownerTeam) {
	ownerIndex = -1;
	ownerName.clear();
	ownerTeam = 0;
	if (!entity) return;
	const auto ownerHandle = entity->GetOwnerEntityHandle();
	if (!ownerHandle.IsValid()) return;
	ownerIndex = ownerHandle.GetEntryIndex();
	CEntityInstance* owner = EntityAt(ownerIndex);
	if (!owner) return;
	ownerTeam = owner->GetTeam();
	CEntityInstance* controller = owner;
	if (owner->IsPlayerPawn()) {
		const auto controllerHandle = owner->GetPlayerControllerHandle();
		controller = controllerHandle.IsValid() ? EntityAt(controllerHandle.GetEntryIndex()) : nullptr;
	}
	if (controller && controller->IsPlayerController()) {
		const char* name = controller->GetPlayerName();
		if (name && *name) ownerName = name;
	}
	if (ownerName.empty())
		ownerName = owner->IsPlayerPawn() ? "Player pawn" : "Entity";
}

} // namespace

void FollowCamera::UpdateGrenadeCache() const {
	for (auto& pair : m_grenadeCache)
		pair.second.active = false;

	int currentTick = 0;
	g_MirvTime.GetCurrentDemoTick(currentTick);
	float interval = g_MirvTime.interval_per_tick_get();
	if (!(interval > 0.00001f)) interval = 1.0f / 64.0f;
	const float currentGameTime = g_MirvTime.curtime_get();

	for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
		CEntityInstance* entity = EntityAt(i);
		if (!entity) continue;
		const std::string cls = EntityClass(entity);
		if (!IsGrenadeClass(cls)) continue;
		const auto entityHandle = entity->GetHandle();
		if (!entityHandle.IsValid()) continue;
		const uint64_t handle = (uint64_t)(uint32_t)entityHandle.ToInt();

		int throwTick = currentTick;
		const float creationTime = entity->GetGrenadeTrajectoryCreationTime();
		if (std::isfinite(creationTime) && creationTime > 0.0f
			&& std::isfinite(currentGameTime) && currentGameTime > 0.0f) {
			// Both values are server/game time. Convert their relative age onto the
			// current playback/demo tick, avoiding the server-start tick offset.
			const double estimated = currentTick + (creationTime - currentGameTime) / interval;
			if (std::isfinite(estimated) && std::fabs(estimated - currentTick) < 10000.0)
				throwTick = (int)std::llround(estimated);
		}

		auto& record = m_grenadeCache[handle];
		record.handle = handle;
		record.entityIndex = i;
		record.className = cls;
		record.grenadeType = GrenadeTypeName(cls);
		record.throwTick = throwTick;
		record.active = true;
		ResolveOwner(entity, record.throwerIndex, record.throwerName, record.team);
	}

	for (auto it = m_grenadeCache.begin(); it != m_grenadeCache.end();) {
		if (it->second.throwTick >= 0 && std::abs(it->second.throwTick - currentTick) > 5000)
			it = m_grenadeCache.erase(it);
		else
			++it;
	}
}

std::vector<FollowTargetCandidate> FollowCamera::Candidates() const {
	std::vector<FollowTargetCandidate> out;
	if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return out;

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
		UpdateGrenadeCache();
		int currentTick = 0;
		g_MirvTime.GetCurrentDemoTick(currentTick);
		for (const auto& pair : m_grenadeCache) {
			const GrenadeRecord& record = pair.second;
			// Browse window: grenades thrown within ~1000 ticks either side of the playhead
			// (recently detected, or about to exist once scrubbed into range).
			if (record.throwTick < 0 || std::abs(record.throwTick - currentTick) > 1000)
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
				CEntityInstance* grenade = EntityAt(record.entityIndex);
				candidate.attachPoints = AvailableAttachPoints(grenade, FollowTargetType::Grenade, false, false);
				candidate.attachments = AttachPointIds(candidate.attachPoints);
				FollowVec3 position;
				if (ReadOrigin(grenade, position))
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

	for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
		CEntityInstance* entity = EntityAt(i);
		if (!entity) continue;

		FollowTargetCandidate candidate;
		CEntityInstance* target = entity;
		if (m_state.targetType == FollowTargetType::Player) {
			if (!entity->IsPlayerController()) continue;
			const auto pawnHandle = entity->GetPlayerPawnHandle();
			if (!pawnHandle.IsValid()) continue;
			target = EntityAt(pawnHandle.GetEntryIndex());
			if (!target || !target->IsPlayerPawn()) continue;
			candidate.entityIndex = pawnHandle.GetEntryIndex();
			const char* playerName = entity->GetPlayerName();
			candidate.name = playerName && *playerName ? playerName : "Player";
			candidate.team = target->GetTeam();
			candidate.alive = target->GetHealth() > 0;
			candidate.status = candidate.alive ? "alive" : "dead";
			if (m_state.mode == FollowMode::Attach) {
				candidate.attachPoints = AvailableAttachPoints(target, FollowTargetType::Player, false, false);
				candidate.attachments = AttachPointIds(candidate.attachPoints);
			}
		} else { // Weapon: dropped + held weapons and the C4
			// Classify and name off the DESIGNER class ("weapon_ak47"): the client class is
			// "C_CSWeaponBaseGun" for nearly every gun, which named most of this list
			// "CSWeaponBaseGun". Falls back to the client class when the designer name is missing.
			const std::string designer = DesignerClass(entity);
			const std::string cls = designer.empty() ? EntityClass(entity) : designer;
			if (!IsWeaponOrBomb(cls)) continue;
			candidate.entityIndex = i;
			candidate.className = cls;
			candidate.isWeapon = IsWeaponClass(cls);
			candidate.isBomb = IsBombClass(cls);
			ResolveOwner(entity, candidate.ownerIndex, candidate.ownerName, candidate.team);
			candidate.held = candidate.ownerIndex >= 0;
			candidate.dropped = !candidate.held;
			if (candidate.held) {
				if (CEntityInstance* ownerPawn = EntityAt(candidate.ownerIndex)) {
					const auto active = ownerPawn->GetActiveWeaponHandle();
					candidate.current = active.IsValid() && active.GetEntryIndex() == i;
				}
			}
			candidate.name = candidate.isBomb ? "C4" : (cls.empty() ? "Weapon" : PrettyWeaponName(cls));
			candidate.alive = true;
			if (candidate.held)
				candidate.status = std::string(candidate.current ? "current · " : "inventory · ")
					+ (candidate.isBomb ? "C4 carried by " : "held by ")
					+ (candidate.ownerName.empty() ? "player" : candidate.ownerName);
			else
				candidate.status = candidate.isBomb ? "C4 on the ground" : "dropped on floor";
			if (m_state.mode == FollowMode::Attach) {
				candidate.attachPoints = AvailableAttachPoints(entity, FollowTargetType::Weapon, candidate.isWeapon, candidate.isBomb);
				candidate.attachments = AttachPointIds(candidate.attachPoints);
			}
		}

		FollowVec3 position;
		if (m_state.targetType == FollowTargetType::Player) {
			float eye[3] = {};
			target->GetRenderEyeOrigin(eye);
			position = FollowVec3{ eye[0], eye[1], eye[2] };
		} else if (!ReadOrigin(target, position)) {
			continue;
		}
		candidate.distance = FollowDistance(reference, position);
		const auto handle = target->GetHandle();
		candidate.handle = handle.IsValid() ? (uint64_t)(uint32_t)handle.ToInt() : 0;
		out.push_back(candidate);
	}

	// Weapon list: keep it to the player being watched. The scan above walks the WHOLE entity
	// list, so on a full server it returns every gun in the round -- dozens of entries, capped at
	// 24 for the UI, in which the one weapon you actually want (the gun in the hands of the guy on
	// screen) is just one row among strangers'. Dropped weapons and the C4 have no owner and are
	// deliberately kept: they belong to nobody, so they are never the clutter being complained
	// about, and they are the whole point of the "Dropped" source mode.
	//
	// The filter is skipped when the spectated pawn cannot be resolved, and when it owns nothing
	// in this list, so it can only ever remove rows that a still-populated list makes redundant --
	// no weapon becomes unreachable.
	if (m_state.targetType == FollowTargetType::Weapon) {
		// WHICH player scopes the list.
		//
		// The first version keyed this on AfxGetSpectatedPawnIndex() alone, and measuring it on a
		// live demo showed the filter silently never applying: that helper returns -1 in third
		// person and free cam BY DESIGN (there is no single spectated player then) -- and free cam
		// is exactly the mode the camera editor puts you in. The guard below then skipped the
		// filter every time, so the list stayed scene-wide.
		//
		// The player you last picked in Player mode is the better answer anyway: it is the
		// report's own workflow (choose a player, then choose their weapon), it survives free cam,
		// and it is an explicit choice rather than an inference. The spectated pawn stays as the
		// fallback for when no player has been picked yet.
		int watched = m_lastPlayerIndex;
		if (watched < 0) watched = AfxGetSpectatedPawnIndex();
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
	std::vector<FollowTargetCandidate> out;
	if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return out;
	double pos[3] = {}, ang[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(pos, ang, fov);
	const FollowVec3 reference{ pos[0], pos[1], pos[2] };
	for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
		CEntityInstance* controller = EntityAt(i);
		if (!controller || !controller->IsPlayerController()) continue;
		const auto pawnHandle = controller->GetPlayerPawnHandle();
		if (!pawnHandle.IsValid()) continue;
		CEntityInstance* pawn = EntityAt(pawnHandle.GetEntryIndex());
		if (!pawn || !pawn->IsPlayerPawn()) continue;
		FollowTargetCandidate c;
		c.entityIndex = pawnHandle.GetEntryIndex();
		const char* name = controller->GetPlayerName();
		c.name = name && *name ? name : "Player";
		c.team = pawn->GetTeam(); c.alive = pawn->GetHealth() > 0;
		float eye[3] = {}; pawn->GetRenderEyeOrigin(eye);
		c.distance = FollowDistance(reference, FollowVec3{ eye[0], eye[1], eye[2] });
		out.push_back(std::move(c));
	}
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
	CameraPathRef().StopPreview();
	CameraPathRef().StopScrub();
	GraphEditorExperimentHudRef().SetDrive(false);
	CameraBridge_SetPathDrawEnabled(false);
	CameraBridge_SetFreeCamEnabled(true);

	if (g_pEngineToClient) {
		std::ostringstream command;
		command << "demo_gototick " << m_grenadeSeekTick;
		g_pEngineToClient->ExecuteClientCmd(0, command.str().c_str(), true);
	}
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
