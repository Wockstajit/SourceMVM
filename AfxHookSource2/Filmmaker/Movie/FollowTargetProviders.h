#pragma once

#include "FollowCamera.h"
#include "../../ClientEntitySystem.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace Filmmaker {

// Entity/naming/attachment-resolution helpers shared by the target providers below and by
// FollowCamera's candidate discovery + state-JSON building (FollowTargetDiscovery.cpp,
// FollowCameraStateJson.cpp). Moved out of FollowCamera.cpp's anonymous namespace -- same
// implementations, now with external linkage so other Follow*.cpp files can call them.

CEntityInstance* EntityAt(int index);
std::string Lower(std::string value);
// Designer class name ("weapon_ak47"). Prefer this for weapon naming: most CS2 guns share the
// client class "C_CSWeaponBaseGun".
std::string DesignerClass(CEntityInstance* entity);
std::string EntityClass(CEntityInstance* entity);
bool IsFiniteOrigin(const FollowVec3& p);
bool ReadOrigin(CEntityInstance* entity, FollowVec3& out);
// Any grenade, held or thrown; IsGrenadeClass is the thrown-projectile subset.
bool IsGrenadeFamily(const std::string& className);
bool IsGrenadeClass(const std::string& className);
bool IsBombClass(const std::string& className);
bool IsWeaponClass(const std::string& className);
bool IsDroppedWeapon(CEntityInstance* entity, const std::string& className);
bool IsWeaponOrBomb(const std::string& className);
std::string GrenadeTypeName(const std::string& className);

// ---- thrown grenades: ONE index for every picker (the Follow Camera's Grenade target, the scene
// objects' "attach to a grenade") -------------------------------------------------------------
// A thrown projectile, by either of its names. The designer name ("smokegrenade_projectile")
// names every kind; the CLIENT class of a thrown smoke and HE is the generic C_BaseGrenade, with
// no "projectile" in it -- testing only that (EntityClass) is why smokes and HEs never appeared
// in either picker, even one in the air in front of the camera (2026-10-18). "" when not one.
std::string ThrownGrenadeClass(CEntityInstance* entity);
// The demo tick a projectile was thrown: its age in game time carried onto the demo tick.
int GrenadeThrowTick(CEntityInstance* entity);
struct GrenadeSighting {
	uint64_t handle = 0;
	int entityIndex = -1;       // where it is now, while active
	std::string className, type, thrower;
	int throwerIndex = -1, team = 0, throwTick = -1;
	bool active = false;        // in the world at this tick
};
// The pickers list grenades thrown this far either side of the playhead: the ones in the air now
// and the ones seen earlier this session. Wide on purpose -- a picker that only knew the last few
// seconds hid the grenade the user was looking for.
constexpr int kGrenadeBrowseTicks = 64 * 60;
// Sightings further than this from the playhead are forgotten.
constexpr int kGrenadeForgetTicks = 64 * 300;
// Main thread. Scans the entities once per frame however many callers ask, remembers every
// projectile it has seen (by handle), and returns the index.
const std::vector<GrenadeSighting>& GrenadeIndex_Update();
void GrenadeIndex_Reset(); // a new map or demo: nothing seen yet
std::string PrettyWeaponName(const std::string& className);
std::string EntityFriendlyName(CEntityInstance* entity);
uint64_t EntityHandleValue(CEntityInstance* entity);
FollowAttachPoint MakeAttachPoint(const char* id, const char* label, const char* kind,
	bool valid, int index, const char* source);
void AddAttachmentIfValid(std::vector<FollowAttachPoint>& out, CEntityInstance* entity,
	const char* id, const char* label, const char* kind);
std::vector<FollowAttachPoint> AvailableAttachPoints(CEntityInstance* entity, FollowTargetType type, bool weapon, bool bomb);
std::vector<std::string> AttachPointIds(const std::vector<FollowAttachPoint>& points);

struct AttachTransformResult {
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
	FollowVec3 pos;
	FollowAngles ang;
};

bool ResolvePlayerVirtualPoint(CEntityInstance* pawn, const std::string& id, AttachTransformResult& out);
AttachTransformResult ResolveAttachTransform(CEntityInstance* entity, int entityIndex,
	FollowTargetType type, const std::string& requestedAttachId, FollowPlayerPose* playerPose = nullptr, bool rigid = false);

std::string JsonEscape(const std::string& value);
void AppendAttachPointJson(std::ostringstream& o, const FollowAttachPoint& point);
const char* TeamName(int team);

class PlayerTargetProvider final : public IFollowTargetProvider {
public:
	explicit PlayerTargetProvider(int index, FollowPlayerPose* pose = nullptr) : m_index(index), m_pose(pose) {}
	bool IsValid() override {
		CEntityInstance* pawn = EntityAt(m_index);
		return pawn && pawn->IsPlayerPawn()
			&& ResolveAttachTransform(pawn, m_index, FollowTargetType::Player, "eyes", m_pose).valid;
	}
	FollowVec3 GetWorldPosition() override {
		FollowVec3 pos; FollowAngles ang;
		GetWorldTransform(pos, ang);
		return pos;
	}
	bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang) override {
		const auto resolved = ResolveAttachTransform(EntityAt(m_index), m_index, FollowTargetType::Player, "eyes", m_pose);
		pos = resolved.pos;
		ang = resolved.ang;
		return resolved.valid && resolved.oriented;
	}
	std::string GetDisplayName() override {
		CEntityInstance* pawn = EntityAt(m_index);
		if (!pawn) return "Missing player";
		auto controllerHandle = pawn->GetPlayerControllerHandle();
		CEntityInstance* controller = controllerHandle.IsValid() ? EntityAt(controllerHandle.GetEntryIndex()) : nullptr;
		const char* name = controller ? controller->GetPlayerName() : nullptr;
		return name && *name ? name : "Player";
	}
	FollowTargetStatus GetStatus() override {
		CEntityInstance* pawn = EntityAt(m_index);
		if (!pawn || !pawn->IsPlayerPawn()) return FollowTargetStatus::Missing;
		return pawn->GetHealth() > 0 ? FollowTargetStatus::Alive : FollowTargetStatus::Dead;
	}
	int EntityIndex() const override { return m_index; }
private:
	int m_index;
	FollowPlayerPose* m_pose;
};

class EntityTargetProvider : public IFollowTargetProvider {
public:
	EntityTargetProvider(int index, FollowTargetType type) : m_index(index), m_type(type) {}
	bool IsValid() override {
		CEntityInstance* entity = EntityAt(m_index);
		if (!entity) return false;
		const std::string cls = EntityClass(entity);
		switch (m_type) {
		case FollowTargetType::Grenade: return !ThrownGrenadeClass(entity).empty();
		case FollowTargetType::Weapon: return IsWeaponOrBomb(cls);
		default: return true;
		}
	}
	FollowVec3 GetWorldPosition() override {
		FollowVec3 out;
		ReadOrigin(EntityAt(m_index), out);
		return out;
	}
	std::string GetDisplayName() override {
		const std::string cls = EntityClass(EntityAt(m_index));
		if (cls.empty()) return "Entity";
		if (m_type == FollowTargetType::Grenade) return GrenadeTypeName(ThrownGrenadeClass(EntityAt(m_index)));
		return PrettyWeaponName(cls);
	}
	FollowTargetStatus GetStatus() override {
		return IsValid() ? FollowTargetStatus::Active : FollowTargetStatus::Missing;
	}
	int EntityIndex() const override { return m_index; }
protected:
	int m_index;
	FollowTargetType m_type;
};

class AttachmentTargetProvider final : public EntityTargetProvider {
public:
	AttachmentTargetProvider(int index, FollowTargetType type, const std::string& attachment, FollowPlayerPose* pose = nullptr)
		: EntityTargetProvider(index, type), m_attachment(attachment), m_pose(pose) {}
	// A body mount is valid only while its bone exists. Missing weapon points temporarily
	// use the held grip or dropped root without replacing the requested attachment.
	// Death policy is evaluated separately from transform validity: a moving corpse remains
	// a valid target until its skeleton disappears.
	bool IsValid() override {
		CEntityInstance* entity = EntityAt(m_index);
		if (!entity) return false;
		if (m_type == FollowTargetType::Player)
			return entity->IsPlayerPawn()
				&& ResolveAttachTransform(entity, m_index, m_type, m_attachment, m_pose, true).valid;
		return ResolveAttachTransform(entity, m_index, m_type, m_attachment, m_pose, true).valid;
	}
	FollowTargetStatus GetStatus() override {
		if (m_type == FollowTargetType::Player) {
			CEntityInstance* entity = EntityAt(m_index);
			if (!entity || !entity->IsPlayerPawn()) return FollowTargetStatus::Missing;
			return entity->GetHealth() > 0 ? FollowTargetStatus::Active : FollowTargetStatus::Dead;
		}
		return EntityTargetProvider::GetStatus();
	}
	FollowVec3 GetWorldPosition() override {
		FollowVec3 pos; FollowAngles ang;
		GetWorldTransform(pos, ang);
		return pos;
	}
	bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang) override {
		AttachTransformResult resolved = ResolveAttachTransform(EntityAt(m_index), m_index, m_type, m_attachment, m_pose, true);
		pos = resolved.pos;
		ang = resolved.ang;
		return resolved.valid && resolved.oriented;
	}
	// Just the entity name (player or weapon); the attach point is shown separately in the
	// "Attach pt" row, so appending "/ head" here only made the Target label noisy.
	std::string GetDisplayName() override {
		return EntityFriendlyName(EntityAt(m_index));
	}
private:
	std::string m_attachment;
	FollowPlayerPose* m_pose;
};

// Follows a player's CURRENTLY HELD weapon: re-resolves the active weapon each frame
// so the camera tracks through weapon switches. With an attachment name it reads that
// point (muzzle/etc.) on the live weapon; otherwise the weapon's origin.
class HeldWeaponProvider final : public IFollowTargetProvider {
public:
	HeldWeaponProvider(int playerIndex, std::string attachment)
		: m_playerIndex(playerIndex), m_attachment(std::move(attachment)) {}
	int ActiveWeaponIndex() const {
		CEntityInstance* pawn = EntityAt(m_playerIndex);
		if (!pawn || !pawn->IsPlayerPawn()) return -1;
		const auto weapon = pawn->GetActiveWeaponHandle();
		return weapon.IsValid() ? weapon.GetEntryIndex() : -1;
	}
	bool IsValid() override {
		const int wi = ActiveWeaponIndex();
		if (wi < 0) return false;
		CEntityInstance* weapon = EntityAt(wi);
		if (!weapon) return false;
		// The resolver retries the requested attachment and its native alias on each weapon.
		return ResolveAttachTransform(weapon, wi, FollowTargetType::Weapon, m_attachment, nullptr, true).valid;
	}
	FollowVec3 GetWorldPosition() override {
		FollowVec3 pos; FollowAngles ang;
		GetWorldTransform(pos, ang);
		return pos;
	}
	bool GetWorldTransform(FollowVec3& pos, FollowAngles& ang) override {
		pos = {};
		const int wi = ActiveWeaponIndex();
		AttachTransformResult resolved = ResolveAttachTransform(EntityAt(wi), wi, FollowTargetType::Weapon, m_attachment, nullptr, true);
		pos = resolved.pos;
		ang = resolved.ang;
		return resolved.valid && resolved.oriented;
	}
	std::string GetDisplayName() override {
		CEntityInstance* weapon = EntityAt(ActiveWeaponIndex());
		const std::string designer = DesignerClass(weapon);
		const std::string cls = designer.empty() ? EntityClass(weapon) : designer;
		std::string base = cls.empty() ? "Held weapon" : PrettyWeaponName(cls);
		return m_attachment.empty() ? base : (base + " / " + m_attachment);
	}
	FollowTargetStatus GetStatus() override {
		CEntityInstance* pawn = EntityAt(m_playerIndex);
		if (!pawn || !pawn->IsPlayerPawn()) return FollowTargetStatus::Missing;
		if (pawn->GetHealth() <= 0) return FollowTargetStatus::Dead;
		return IsValid() ? FollowTargetStatus::Active : FollowTargetStatus::Missing;
	}
	int EntityIndex() const override { return ActiveWeaponIndex(); }
private:
	int m_playerIndex;
	std::string m_attachment;
};


} // namespace Filmmaker
