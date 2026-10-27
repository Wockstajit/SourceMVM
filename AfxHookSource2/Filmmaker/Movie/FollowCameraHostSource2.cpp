// CS2's answers to FollowCameraHost.h.

#include "FollowCameraHost.h"
#include "FollowTargetProviders.h"

#include "CameraPath.h"
#include "../Filmmaker.h"
#include "../Debug/MvmDebugLog.h"
#include "../Demo/DemoInfoHelper.h"
#include "../Panorama/CameraTimelineHud.h"
#include "../Panorama/GraphEditorExperimentHud.h"
#include "../Platform/TextEncoding.h"
#include "../../ClientEntitySystem.h"
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <sstream>
#include <unordered_map>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// A weapon's owner, for the Weapon list (FollowHost_FindTargets below).
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

int FollowHost_DemoTick() {
	int tick = 0;
	g_MirvTime.GetCurrentDemoTick(tick);
	return tick;
}

double FollowHost_IntervalPerTick() { return g_MirvTime.interval_per_tick_get(); }

int FollowHost_FrameCount() { return g_MirvTime.framecount_get(); }

bool FollowHost_DemoPaused() {
	if (g_pEngineToClient) {
		if (auto demo = g_pEngineToClient->GetDemoFile()) return demo->IsDemoPaused();
	}
	return false;
}

void FollowHost_ResumeDemo() {
	if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
}

void FollowHost_SeekDemo(int tick) {
	if (!g_pEngineToClient) return;
	std::ostringstream command;
	command << "demo_gototick " << tick;
	g_pEngineToClient->ExecuteClientCmd(0, command.str().c_str(), true);
}

std::string FollowHost_DemoPath() {
	const char* p = g_pEngineToClient ? g_pEngineToClient->GetDemoFilePath() : nullptr;
	return p ? p : "";
}

// The .fmjson v5 pre-scan: ReadDemoInfoViaHelper reuses or refreshes the "<demo>.fmjson" cache.
bool FollowHost_ReadDemoEvents(const std::string& path, std::atomic<bool>* cancel,
	std::vector<FollowEventRecord>& events, std::string& helperPath, std::string& error) {
	DemoHelperResult result = ReadDemoInfoViaHelper(Utf8ToWide(path), {}, cancel);
	if (cancel && cancel->load()) return false;
	std::unordered_map<uint32_t, std::string> names;
	for (const auto& pl : result.players) names[pl.accountId] = pl.name;
	events.clear();
	events.reserve(result.events.size());
	for (const auto& e : result.events) {
		FollowEventRecord f;
		f.tick = e.tick; f.type = e.type; f.item = e.item; f.accountId = e.accountId;
		auto it = names.find(e.accountId);
		f.ownerName = (it != names.end()) ? it->second : std::string();
		events.push_back(std::move(f));
	}
	helperPath = result.helperPath;
	error = result.error;
	return result.ok;
}

bool FollowHost_EditorActive() { return CameraEditor_Active(); }

void FollowHost_SetEditorCursor(bool ui) { CameraEditor_SetCursorMode(ui); }

void FollowHost_TakeViewFromOtherCameras() {
	CameraPathRef().StopPreview();
	CameraPathRef().StopScrub();
	GraphEditorExperimentHudRef().SetDrive(false);
}

void FollowHost_ReturnViewToTimeline() {
	if (CameraEditor_Active() && CameraTimelineHudRef().Cursor())
		GraphEditorExperimentHudRef().SetDrive(true);
}

std::unique_ptr<IFollowTargetProvider> FollowHost_MakeProvider(const FollowCameraState& state, FollowPlayerPose* playerPose) {
	const bool attach = state.mode == FollowMode::Attach;
	switch (state.targetType) {
	case FollowTargetType::Player:
		if (attach)
			return std::make_unique<AttachmentTargetProvider>(state.targetEntityIndex, FollowTargetType::Player, state.attachmentName, playerPose);
		return std::make_unique<PlayerTargetProvider>(state.targetEntityIndex, playerPose);
	case FollowTargetType::Grenade:
		if (attach)
			return std::make_unique<AttachmentTargetProvider>(state.targetEntityIndex, FollowTargetType::Grenade, state.attachmentName);
		return std::make_unique<EntityTargetProvider>(state.targetEntityIndex, FollowTargetType::Grenade);
	case FollowTargetType::Weapon: {
		const bool held = (state.weaponSource == WeaponSource::Held)
			|| (state.weaponSource == WeaponSource::Auto && state.weaponPlayerIndex >= 0);
		if (held && state.weaponPlayerIndex >= 0)
			return std::make_unique<HeldWeaponProvider>(state.weaponPlayerIndex,
				attach ? state.attachmentName : std::string());
		if (attach && !state.attachmentName.empty())
			return std::make_unique<AttachmentTargetProvider>(state.targetEntityIndex, FollowTargetType::Weapon, state.attachmentName);
		return std::make_unique<EntityTargetProvider>(state.targetEntityIndex, FollowTargetType::Weapon);
	}
	default:
		return std::make_unique<PlayerTargetProvider>(state.targetEntityIndex);
	}
}

std::vector<FollowTargetCandidate> FollowHost_FindTargets(FollowTargetType type, bool attachPoints, const FollowVec3& from) {
	std::vector<FollowTargetCandidate> out;
	if (type == FollowTargetType::Grenade || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return out;

	for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
		CEntityInstance* entity = EntityAt(i);
		if (!entity) continue;

		FollowTargetCandidate candidate;
		CEntityInstance* target = entity;
		if (type == FollowTargetType::Player) {
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
			if (attachPoints) {
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
			if (attachPoints) {
				candidate.attachPoints = AvailableAttachPoints(entity, FollowTargetType::Weapon, candidate.isWeapon, candidate.isBomb);
				candidate.attachments = AttachPointIds(candidate.attachPoints);
			}
		}

		FollowVec3 position;
		if (type == FollowTargetType::Player) {
			float eye[3] = {};
			target->GetRenderEyeOrigin(eye);
			position = FollowVec3{ eye[0], eye[1], eye[2] };
		} else if (!ReadOrigin(target, position)) {
			continue;
		}
		candidate.distance = FollowDistance(from, position);
		const auto handle = target->GetHandle();
		candidate.handle = handle.IsValid() ? (uint64_t)(uint32_t)handle.ToInt() : 0;
		out.push_back(candidate);
	}
	return out;
}

int FollowHost_SpectatedPlayer() { return AfxGetSpectatedPawnIndex(); }

int FollowHost_PlayerUserId(int entityIndex) {
	CEntityInstance* pawn = EntityAt(entityIndex);
	const auto controller = pawn ? pawn->GetPlayerControllerHandle() : SOURCESDK::CS2::CBaseHandle();
	return controller.IsValid() ? controller.GetEntryIndex() - 1 : -1;
}

const std::vector<GrenadeSighting>& FollowHost_GrenadeSightings() { return GrenadeIndex_Update(); }

std::vector<FollowAttachPoint> FollowHost_AttachPoints(int entityIndex, FollowTargetType type) {
	CEntityInstance* entity = EntityAt(entityIndex);
	bool weapon = false, bomb = false;
	if (entity) {
		const std::string cls = EntityClass(entity);
		weapon = IsWeaponClass(cls);
		bomb = IsBombClass(cls);
	}
	return AvailableAttachPoints(entity, type, weapon, bomb);
}

uint64_t FollowHost_EntityHandle(int entityIndex) {
	return entityIndex >= 0 ? EntityHandleValue(EntityAt(entityIndex)) : 0;
}

bool FollowHost_EntityOrigin(int entityIndex, FollowVec3& origin, bool& isPlayer) {
	CEntityInstance* entity = entityIndex >= 0 ? EntityAt(entityIndex) : nullptr;
	if (!entity || !ReadOrigin(entity, origin)) return false;
	isPlayer = entity->IsPlayerPawn();
	return true;
}

bool FollowHost_LivePlayerItems(int entityIndex, int& activeWeaponIndex, int& bombIndex) {
	CEntityInstance* pawn = EntityAt(entityIndex);
	if (!pawn || !pawn->IsPlayerPawn() || pawn->GetHealth() <= 0) return false;
	activeWeaponIndex = -1;
	auto weapon = pawn->GetActiveWeaponHandle();
	if (weapon.IsValid()) activeWeaponIndex = weapon.GetEntryIndex();
	bombIndex = -1;
	const int pawnIndex = pawn->GetHandle().IsValid() ? pawn->GetHandle().GetEntryIndex() : entityIndex;
	for (int i = 0; i <= GetHighestEntityIndex(); ++i) {
		CEntityInstance* entity = EntityAt(i);
		if (!entity || !IsBombClass(EntityClass(entity))) continue;
		auto owner = entity->GetOwnerEntityHandle();
		if (owner.IsValid() && owner.GetEntryIndex() == pawnIndex) {
			bombIndex = i;
			break;
		}
	}
	return true;
}

FollowMountDescription FollowHost_DescribeMount(int entityIndex, FollowTargetType type, const std::string& mount,
	FollowPlayerPose* playerPose, bool rigid) {
	const AttachTransformResult r = ResolveAttachTransform(EntityAt(entityIndex), entityIndex, type, mount, playerPose, rigid);
	FollowMountDescription d;
	d.valid = r.valid;
	d.entityHandle = r.entityHandle;
	d.className = r.className;
	d.modelName = r.modelName;
	d.attachKind = r.attachKind;
	d.attachIndex = r.attachIndex;
	d.source = r.source;
	return d;
}

void FollowHost_PrintTargetProbe(int entityIndex) {
	auto* pawn = EntityAt(entityIndex);
	const char* points[] = { "entity", "eyes", "head", "chest", "pelvis", "feet",
		"forward", "facemask", "primary", "c4", "pistol", "weapon_hand_R", "weapon_hand_L", "leg_L", "leg_R" };
	for (const char* point : points) {
		const auto resolved = ResolveAttachTransform(pawn, entityIndex, FollowTargetType::Player, point);
		const unsigned index = pawn ? pawn->LookupAttachment(point) : 0;
		advancedfx::Message("[followcam][probe] entity=%d health=%u point=%s index=%u source=%s pos=(%.3f %.3f %.3f)\n",
			entityIndex, pawn ? pawn->GetHealth() : 0, point, index, resolved.source.c_str(), resolved.pos.x, resolved.pos.y, resolved.pos.z);
		MvmDebugLog_Linef("follow.targetprobe", "entity=%d ptr=%p health=%u point=%s index=%u source=%s valid=%d pos=(%.3f %.3f %.3f)",
			entityIndex, pawn, pawn ? pawn->GetHealth() : 0, point, index, resolved.source.c_str(), resolved.valid,
			resolved.pos.x, resolved.pos.y, resolved.pos.z);
	}
}

} // namespace Filmmaker
