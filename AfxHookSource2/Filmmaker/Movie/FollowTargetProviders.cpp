#include "FollowTargetProviders.h"

#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"
#include "../../MirvTime.h"
#include "../Debug/MvmDebugLog.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>

namespace Filmmaker {

CEntityInstance* EntityAt(int index) {
	if (index < 0 || index > GetHighestEntityIndex() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });
	return value;
}

// The DESIGNER class name ("weapon_ak47"), as opposed to EntityClass below, which prefers the
// CLIENT class.
//
// Measured on a live demo 2026-08-27: almost every CS2 gun reports the client class
// "C_CSWeaponBaseGun" -- shared by the AK, M4, AWP and the rest -- so a name derived from it
// read "CSWeaponBaseGun" for most of the weapon list. Only the odd one out (C_WeaponElite,
// C_Knife, C_HEGrenade) carried anything specific. The designer name is per-weapon for all of
// them, so it is what naming and classification must use.
std::string DesignerClass(CEntityInstance* entity) {
	if (!entity) return "";
	const char* cls = entity->GetClassName();
	return cls ? cls : "";
}

std::string EntityClass(CEntityInstance* entity) {
	if (!entity) return "";
	const char* client = entity->GetClientClassName();
	if (client && *client) return client;
	const char* cls = entity->GetClassName();
	return cls ? cls : "";
}

bool ReadOrigin(CEntityInstance* entity, FollowVec3& out) {
	if (!entity) return false;
	float x = FLT_MAX, y = FLT_MAX, z = FLT_MAX;
	entity->GetOrigin(x, y, z);
	out = FollowVec3{ x, y, z };
	return IsFiniteOrigin(out);
}

// Read the same world orientation as the scene node. Direction of travel is unrelated
// to an entity's rotation (especially strafing, stairs and drawing another weapon).
static bool ReadSceneAngles(CEntityInstance* entity, FollowAngles& out) {
	const auto nodeOff = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
	const auto angleOff = g_clientDllOffsets.CGameSceneNode.m_angAbsRotation;
	if (!entity || !nodeOff || !angleOff) return false;
	unsigned char* node = nullptr;
	float angles[3] = {};
	__try {
		node = *reinterpret_cast<unsigned char**>(reinterpret_cast<unsigned char*>(entity) + nodeOff);
		if (!node) return false;
		const auto a = reinterpret_cast<const float*>(node + angleOff);
		for (int i = 0; i < 3; ++i) angles[i] = a[i];
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!std::isfinite(angles[0]) || !std::isfinite(angles[1]) || !std::isfinite(angles[2])) return false;
	out = {angles[0], angles[1], angles[2]};
	static unsigned long long lastLog = 0;
	if (MvmDebugLog_Active() && GetTickCount64() - lastLog >= 500) {
		lastLog = GetTickCount64();
		MvmDebugLog_Linef("follow.mount.scene", "entity=%p node=%p nodeOff=%td anglesOff=%td angles=(%.6f %.6f %.6f)",
			entity, node, nodeOff, angleOff, out.pitch, out.yaw, out.roll);
	}
	return true;
}

// Grenade of any kind, in hand OR in flight. Split out of IsGrenadeClass so the two states
// can be told apart: a grenade the player is still HOLDING is a weapon entity (C_HEGrenade /
// weapon_flashbang), while the thrown one carries a "projectile" suffix. Both the client and
// the designer spelling agree on that suffix, so it is what separates them regardless of which
// name the entity happens to report.
bool IsGrenadeFamily(const std::string& className) {
	const std::string s = Lower(className);
	return s.find("grenade") != std::string::npos || s.find("flashbang") != std::string::npos
		|| s.find("molotov") != std::string::npos || s.find("incendiary") != std::string::npos
		|| s.find("decoy") != std::string::npos;
}

// A grenade in flight -- the thrown projectile, which is its own follow target type.
bool IsGrenadeClass(const std::string& className) {
	return Lower(className).find("projectile") != std::string::npos && IsGrenadeFamily(className);
}

bool IsBombClass(const std::string& className) {
	const std::string s = Lower(className);
	return s == "c_c4" || s.find("plantedc4") != std::string::npos
		|| s.find("weapon_c4") != std::string::npos;
}

bool IsWeaponClass(const std::string& className) {
	const std::string s = Lower(className);
	if (IsBombClass(s) || IsGrenadeClass(s)) return false; // planted C4 and thrown nades are their own kinds
	if (s.find("basegrenade") != std::string::npos) return false; // a thrown smoke / HE's client class (ThrownGrenadeClass)
	// Knives and still-held grenades are weapon entities like any other, but their class names
	// carry neither "weapon" (the client classes are C_Knife / C_HEGrenade) nor "projectile".
	// Without these two they fail every test here AND in IsGrenadeClass, so they could never be
	// offered as a weapon target at all -- you could not attach a camera to a knife.
	return s.find("weapon") != std::string::npos
		|| s.find("knife") != std::string::npos || s.find("bayonet") != std::string::npos
		|| IsGrenadeFamily(s)
		|| s.find("ak47") != std::string::npos || s.find("deagle") != std::string::npos;
}

bool IsDroppedWeapon(CEntityInstance* entity, const std::string& className) {
	return entity && IsWeaponClass(className) && !entity->GetOwnerEntityHandle().IsValid();
}

bool IsWeaponOrBomb(const std::string& className) {
	return IsWeaponClass(className) || IsBombClass(className);
}

std::string GrenadeTypeName(const std::string& className) {
	const std::string s = Lower(className);
	if (s.find("flash") != std::string::npos) return "Flashbang";
	if (s.find("smoke") != std::string::npos) return "Smoke";
	if (s.find("hegrenade") != std::string::npos || s.find("frag") != std::string::npos) return "HE Grenade";
	if (s.find("molotov") != std::string::npos) return "Molotov";
	if (s.find("incendiary") != std::string::npos) return "Incendiary";
	if (s.find("decoy") != std::string::npos) return "Decoy";
	return "Grenade";
}

// Friendly weapon name from an engine class (C_AK47, C_WeaponM4A1, ...) so the UI
// reads "AK-47" / "M4A4" / "M4A1-S" instead of the raw class. Order matters: test the
// more specific variants (silencer) before the base token they contain.
std::string PrettyWeaponName(const std::string& className) {
	const std::string s = Lower(className);
	// Held grenades reach this function now that IsWeaponClass accepts them; the table below has
	// no grenade entries, so without this they would fall through to the raw class name.
	if (IsGrenadeFamily(s)) return GrenadeTypeName(s);
	auto has = [&](const char* t) { return s.find(t) != std::string::npos; };
	if (has("m4a1_silencer") || has("m4a1s")) return "M4A1-S";
	if (has("m4a1") || has("m4a4"))           return "M4A4";
	if (has("usp"))                            return "USP-S";
	if (has("hkp2000") || has("p2000"))        return "P2000";
	if (has("ak47"))                           return "AK-47";
	if (has("awp"))                            return "AWP";
	if (has("ssg08"))                          return "SSG 08";
	if (has("scar20"))                         return "SCAR-20";
	if (has("g3sg1"))                          return "G3SG1";
	if (has("sg556") || has("sg553"))          return "SG 553";
	if (has("aug"))                            return "AUG";
	if (has("galil"))                          return "Galil AR";
	if (has("famas"))                          return "FAMAS";
	if (has("deagle") || has("deserteagle"))   return "Desert Eagle";
	if (has("revolver"))                       return "R8 Revolver";
	if (has("elite"))                          return "Dual Berettas";
	if (has("fiveseven"))                      return "Five-SeveN";
	if (has("glock"))                          return "Glock-18";
	if (has("p250"))                           return "P250";
	if (has("cz75"))                           return "CZ75-Auto";
	if (has("tec9"))                           return "Tec-9";
	if (has("p90"))                            return "P90";
	if (has("bizon"))                          return "PP-Bizon";
	if (has("mac10"))                          return "MAC-10";
	if (has("mp5sd"))                          return "MP5-SD";
	if (has("mp7"))                            return "MP7";
	if (has("mp9"))                            return "MP9";
	if (has("ump"))                            return "UMP-45";
	if (has("nova"))                           return "Nova";
	if (has("xm1014"))                         return "XM1014";
	if (has("sawedoff"))                       return "Sawed-Off";
	if (has("mag7"))                           return "MAG-7";
	if (has("m249"))                           return "M249";
	if (has("negev"))                          return "Negev";
	if (has("taser") || has("zeus"))           return "Zeus x27";
	if (has("knife") || has("bayonet"))        return "Knife";
	if (has("c4"))                             return "C4";
	// Fallback: drop the engine prefix and show whatever class remains.
	std::string base = className;
	if (s.rfind("c_weapon", 0) == 0)      base = className.substr(8);
	else if (s.rfind("c_", 0) == 0)       base = className.substr(2);
	return base.empty() ? "Weapon" : base;
}

// Human-readable name for any followed entity: the player's name for a pawn, a friendly
// weapon/grenade name otherwise. Used for the "Target:" label so it reads "jOAO" or
// "AK-47" instead of "CSPlayerPawn" / "C_AK47".
std::string EntityFriendlyName(CEntityInstance* entity) {
	if (!entity) return "";
	if (entity->IsPlayerPawn()) {
		const auto ch = entity->GetPlayerControllerHandle();
		CEntityInstance* controller = ch.IsValid() ? EntityAt(ch.GetEntryIndex()) : nullptr;
		const char* name = controller ? controller->GetPlayerName() : nullptr;
		return (name && *name) ? name : "Player";
	}
	const std::string cls = EntityClass(entity);
	if (cls.empty()) return "Entity";
	if (IsBombClass(cls)) return "C4";
	const std::string thrown = ThrownGrenadeClass(entity);
	if (!thrown.empty()) return GrenadeTypeName(thrown);
	if (IsWeaponClass(cls)) return PrettyWeaponName(cls);
	return cls;
}

std::string ThrownGrenadeClass(CEntityInstance* entity) {
	if (!entity) return "";
	const std::string designer = DesignerClass(entity);
	if (IsGrenadeClass(designer)) return designer;
	const std::string client = EntityClass(entity);
	return IsGrenadeClass(client) ? client : "";
}

namespace {
// The networked launch: where the grenade left the hand, its velocity, and how often it has
// bounced since. A seek restores these (the client-side trail time it does not).
struct GrenadeLaunch { float start[3] = {}, velocity[3] = {}; int bounces = 0; double groundSpeed = 0; };
bool ReadGrenadeLaunch(CEntityInstance* entity, GrenadeLaunch& l) {
	const auto& o = g_clientDllOffsets.C_BaseCSGrenadeProjectile;
	if (!entity || !o.m_vInitialPosition || !o.m_vInitialVelocity || !o.m_nBounces) return false;
	__try {
		const unsigned char* base = reinterpret_cast<const unsigned char*>(entity);
		std::memcpy(l.start, base + o.m_vInitialPosition, sizeof(l.start));
		std::memcpy(l.velocity, base + o.m_vInitialVelocity, sizeof(l.velocity));
		l.bounces = *reinterpret_cast<const int*>(base + o.m_nBounces);
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	l.groundSpeed = std::hypot(double(l.velocity[0]), double(l.velocity[1]));
	// Thrown nearly straight up (or unset): the ground distance cannot time the flight.
	return std::isfinite(l.groundSpeed) && l.groundSpeed > 50.0;
}
// Seconds to cross the ground from the launch point to `at` at the launch speed. Until the first
// bounce that is the flight time exactly (gravity bends only the height); after a bounce the
// grenade has slowed, so it is the least time it can have flown.
double GroundSeconds(const GrenadeLaunch& l, double x, double y) {
	const double t = std::hypot(x - l.start[0], y - l.start[1]) / l.groundSpeed;
	return std::isfinite(t) && t < 30.0 ? t : -1;
}
// A popped smoke's networked pop tick (a server tick) and where it popped. False before it pops.
bool ReadSmokePop(CEntityInstance* entity, int& popped, float where[3]) {
	const auto& smoke = g_clientDllOffsets.C_SmokeGrenadeProjectile;
	if (!smoke.m_nSmokeEffectTickBegin || !smoke.m_vSmokeDetonationPos) return false;
	__try {
		const unsigned char* base = reinterpret_cast<const unsigned char*>(entity);
		popped = *reinterpret_cast<const int*>(base + smoke.m_nSmokeEffectTickBegin);
		std::memcpy(where, base + smoke.m_vSmokeDetonationPos, 3 * sizeof(float));
	} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	return popped > 0;
}
} // namespace

int GrenadeThrowTick(CEntityInstance* entity) {
	int currentTick = 0;
	g_MirvTime.GetCurrentDemoTick(currentTick);
	float interval = g_MirvTime.interval_per_tick_get();
	if (!(interval > 0.00001f)) interval = 1.0f / 64.0f;
	const float now = g_MirvTime.curtime_get();
	GrenadeLaunch launch;
	FollowVec3 at{};
	if (ReadGrenadeLaunch(entity, launch) && ReadOrigin(entity, at)) {
		// In the air before its first bounce: exact.
		if (launch.bounces == 0)
			if (const double t = GroundSeconds(launch, at.x, at.y); t >= 0) return currentTick - int(std::llround(t / interval));
		// A smoke that has popped: the networked tick it popped at, less the least time it took to
		// get from the hand to where it popped. Stable across seeks; early by its rolling time.
		int popped = 0;
		float where[3] = {};
		if (std::isfinite(now) && now > 0.f && ThrownGrenadeClass(entity).find("smoke") != std::string::npos
			&& ReadSmokePop(entity, popped, where)) {
			const double t = GroundSeconds(launch, where[0], where[1]);
			// The pop tick is a server tick; the server tick now is game time / interval.
			const double estimated = currentTick + (popped - now / interval) - t / interval;
			if (t >= 0 && std::isfinite(estimated) && estimated <= currentTick && currentTick - estimated < 10000.0)
				return int(std::llround(estimated));
		}
	}
	const float created = entity ? entity->GetGrenadeTrajectoryCreationTime() : 0.0f;
	// Both are server/game time, so their difference is the age; carried onto the demo tick, the
	// server-start offset cancels out.
	if (std::isfinite(created) && created > 0.f && std::isfinite(now) && now > 0.f) {
		const double estimated = currentTick + (created - now) / interval;
		if (std::isfinite(estimated) && std::fabs(estimated - currentTick) < 10000.0) return int(std::llround(estimated));
	}
	return currentTick;
}

namespace {
std::vector<GrenadeSighting> g_grenadeSightings;
int g_grenadeIndexFrame = -1;

void GrenadeOwner(CEntityInstance* entity, GrenadeSighting& s) {
	s.throwerIndex = -1; s.thrower.clear(); s.team = 0;
	const auto ownerHandle = entity->GetOwnerEntityHandle();
	if (!ownerHandle.IsValid()) return;
	s.throwerIndex = ownerHandle.GetEntryIndex();
	CEntityInstance* owner = EntityAt(s.throwerIndex);
	if (!owner) return;
	s.team = owner->GetTeam();
	CEntityInstance* controller = owner;
	if (owner->IsPlayerPawn()) {
		const auto ch = owner->GetPlayerControllerHandle();
		controller = ch.IsValid() ? EntityAt(ch.GetEntryIndex()) : nullptr;
	}
	if (controller && controller->IsPlayerController()) {
		const char* name = controller->GetPlayerName();
		if (name && *name) s.thrower = name;
	}
}
} // namespace

const std::vector<GrenadeSighting>& GrenadeIndex_Update() {
	const int frame = g_MirvTime.framecount_get();
	if (frame == g_grenadeIndexFrame) return g_grenadeSightings;
	g_grenadeIndexFrame = frame;
	for (auto& s : g_grenadeSightings) { s.active = false; s.entityIndex = -1; }
	if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return g_grenadeSightings;
	int currentTick = 0;
	g_MirvTime.GetCurrentDemoTick(currentTick);
	for (int i = 0, n = GetHighestEntityIndex(); i <= n; ++i) {
		CEntityInstance* entity = EntityAt(i);
		if (!entity) continue;
		const std::string cls = ThrownGrenadeClass(entity);
		if (cls.empty()) continue;
		const uint64_t handle = EntityHandleValue(entity);
		if (!handle) continue;
		GrenadeSighting* s = nullptr;
		for (auto& e : g_grenadeSightings) if (e.handle == handle) { s = &e; break; }
		if (!s) { g_grenadeSightings.emplace_back(); s = &g_grenadeSightings.back(); s->handle = handle; }
		s->entityIndex = i;
		s->className = cls;
		s->type = GrenadeTypeName(cls);
		// A throw tick read in flight (from the launch) is exact. After the first bounce only the
		// trail time is left, which a seek resets to the landing tick -- always later than the
		// throw -- so a grenade keeps the earliest tick it was ever dated with.
		const int tick = GrenadeThrowTick(entity);
		s->throwTick = s->throwTick < 0 ? tick : (std::min)(s->throwTick, tick);
		s->active = true;
		GrenadeOwner(entity, *s);
	}
	g_grenadeSightings.erase(std::remove_if(g_grenadeSightings.begin(), g_grenadeSightings.end(), [&](const GrenadeSighting& s) {
		return s.throwTick < 0 || std::abs(s.throwTick - currentTick) > kGrenadeForgetTicks;
	}), g_grenadeSightings.end());
	return g_grenadeSightings;
}

void GrenadeIndex_Reset() {
	g_grenadeSightings.clear();
	g_grenadeIndexFrame = -1;
}

uint64_t EntityHandleValue(CEntityInstance* entity) {
	if (!entity) return 0;
	const auto h = entity->GetHandle();
	return h.IsValid() ? (uint64_t)(uint32_t)h.ToInt() : 0;
}

void AddAttachmentIfValid(std::vector<FollowAttachPoint>& out, CEntityInstance* entity,
	const char* id, const char* label, const char* kind) {
	if (!entity || !id || !*id) return;
	const uint8_t idx = entity->LookupAttachment(id);
	if (idx != 0)
		out.push_back(MakeAttachPoint(id, label, kind, true, idx, "model-attachment"));
}

static const char* AttachmentAlias(const std::string& name) {
	if (name == "muzzle") return "muzzle_flash";
	if (name == "muzzle_flash") return "muzzle";
	if (name == "shell_eject") return "eject";
	if (name == "eject") return "shell_eject";
	return nullptr;
}

std::vector<FollowAttachPoint> AvailableAttachPoints(CEntityInstance* entity, FollowTargetType type, bool weapon, bool bomb) {
	std::vector<FollowAttachPoint> out;
	if (!entity) return out;
	out.push_back(MakeAttachPoint("entity", "Entity", "entity", true, 0, "entity-origin"));

	if (type == FollowTargetType::Player) {
		out.push_back(MakeAttachPoint("eyes", "Eyes", "virtual", true, -1, "player-render-eye"));
		out.push_back(MakeAttachPoint("head", "Head", "body-mount", entity->LookupAttachment("clip_limit") != 0, entity->LookupAttachment("clip_limit"), "bone-clip_limit"));
		out.push_back(MakeAttachPoint("chest", "Chest", "body-mount", entity->LookupAttachment("c4") != 0, entity->LookupAttachment("c4"), "bone-c4"));
		out.push_back(MakeAttachPoint("pelvis", "Pelvis", "body-mount", entity->LookupAttachment("grenade0") != 0, entity->LookupAttachment("grenade0"), "bone-grenade0"));
		out.push_back(MakeAttachPoint("feet", "Feet", "virtual", true, -1, "player-virtual"));
		AddAttachmentIfValid(out, entity, "weapon_hand_R", "Right hand", "attachment");
		AddAttachmentIfValid(out, entity, "weapon_hand_L", "Left hand", "attachment");
		return out;
	}

	if (type == FollowTargetType::Grenade || bomb)
		return out;

	if (weapon) {
		AddAttachmentIfValid(out, entity, "muzzle", "Muzzle", "attachment");
		if (!entity->LookupAttachment("muzzle") && entity->LookupAttachment("muzzle_flash"))
			out.push_back(MakeAttachPoint("muzzle", "Muzzle", "attachment", true, entity->LookupAttachment("muzzle_flash"), "alias-muzzle_flash"));
		AddAttachmentIfValid(out, entity, "muzzle_flash", "Muzzle flash", "attachment");
		AddAttachmentIfValid(out, entity, "shell_eject", "Shell eject", "attachment");
		AddAttachmentIfValid(out, entity, "eject", "Eject", "attachment");
		AddAttachmentIfValid(out, entity, "magazine", "Magazine", "attachment");
		AddAttachmentIfValid(out, entity, "mag", "Magazine", "attachment");
		AddAttachmentIfValid(out, entity, "clip", "Magazine", "attachment");
		AddAttachmentIfValid(out, entity, "bolt", "Bolt", "attachment");
		AddAttachmentIfValid(out, entity, "slide", "Slide", "attachment");
	}
	return out;
}



bool ResolvePlayerVirtualPoint(CEntityInstance* pawn, const std::string& id,
	AttachTransformResult& out) {
	if (!pawn || !pawn->IsPlayerPawn()) return false;
	if (pawn->GetHealth() == 0) return false;
	FollowVec3 origin;
	if (!ReadOrigin(pawn, origin)) return false;
	float eye[3] = {};
	float a[3] = {};
	pawn->GetRenderEyeOrigin(eye);
	pawn->GetRenderEyeAngles(a);
	const FollowVec3 eyes{ eye[0], eye[1], eye[2] };
	const FollowAngles eyeAngles{ a[0], a[1], a[2] };
	out.valid = true;
	out.oriented = true;
	out.attachKind = "virtual";
	out.attachIndex = -1;
	out.source = id == "eyes" ? "player-render-eye" : "player-virtual";
	out.ang = eyeAngles;
	if (id == "eyes") {
		out.pos = eyes;
	} else if (id == "head") {
		out.pos = FollowVec3{ eyes.x, eyes.y, eyes.z - 3.0 };
	} else if (id == "chest") {
		out.pos = FollowVec3{
			origin.x + (eyes.x - origin.x) * 0.55,
			origin.y + (eyes.y - origin.y) * 0.55,
			origin.z + (eyes.z - origin.z) * 0.55
		};
		out.ang.pitch = 0.0;
	} else if (id == "pelvis") {
		out.pos = FollowVec3{
			origin.x + (eyes.x - origin.x) * 0.25,
			origin.y + (eyes.y - origin.y) * 0.25,
			origin.z + (eyes.z - origin.z) * 0.25
		};
		out.ang.pitch = 0.0;
	} else if (id == "feet") {
		out.pos = origin;
		out.ang.pitch = 0.0;
	} else {
		return false;
	}
	return true;
}

AttachTransformResult ResolveAttachTransform(CEntityInstance* entity, int entityIndex,
	FollowTargetType type, const std::string& requestedAttachId, FollowPlayerPose* playerPose, bool rigid) {
	AttachTransformResult out;
	out.entityIndex = entityIndex;
	out.entityHandle = EntityHandleValue(entity);
	out.entityType = FollowTargetTypeName(type);
	out.className = EntityClass(entity);
	out.attachId = requestedAttachId.empty() ? "entity" : requestedAttachId;
	if (!entity) {
		out.source = "missing-entity";
		return out;
	}

	const bool deadPlayer = type == FollowTargetType::Player && entity->IsPlayerPawn() && entity->GetHealth() == 0;
	if (type == FollowTargetType::Player && entity->IsPlayerPawn()) {
		// Stock player attachments verified in the model data: clip_limit -> head_0,
		// c4 -> spine_3, grenade0 -> spine_0. These survive the transition to ragdoll;
		// render-eye and entity-origin stay at the last living pose or become zero.
		const char* anchor = (out.attachId == "eyes" || out.attachId == "head") ? "clip_limit"
			: out.attachId == "chest" ? "c4"
			: (out.attachId == "pelvis" || out.attachId == "feet" || out.attachId == "entity") ? "grenade0" : nullptr;
		if (anchor) {
			const bool livingPoint = out.attachId == "entity"
				? (!deadPlayer && (out.valid = ReadOrigin(entity, out.pos)))
				: ResolvePlayerVirtualPoint(entity, out.attachId, out);
			const uint8_t index = entity->LookupAttachment(anchor);
			SOURCESDK::Vector pos;
			SOURCESDK::Quaternion q;
			if (playerPose && (playerPose->handle != out.entityHandle || playerPose->point != out.attachId)) {
				*playerPose = FollowPlayerPose{};
				playerPose->handle = out.entityHandle;
				playerPose->point = out.attachId;
			}
			if (index && entity->GetAttachment(index, pos, q)) {
				const FollowVec3 base{pos.x, pos.y, pos.z};
				const FollowAngles angles = FollowQuatToAngles(q.x, q.y, q.z, q.w);
				if (IsFiniteOrigin(base)) {
					// Attached body mounts follow actual bones while alive as well as dead.
					// Rebuilding "head" from render-eye every frame moves its pivot relative to
					// the skull during look/duck/stair animations. Eyes remains an explicit view mount.
					if (rigid && (out.attachId == "head" || out.attachId == "chest" || out.attachId == "pelvis")) {
						out.pos = base;
						out.ang = angles;
						out.valid = out.oriented = true;
						out.attachKind = "body-mount";
						out.attachIndex = index;
						out.source = std::string(deadPlayer ? "ragdoll-" : "bone-") + anchor;
						return out;
					}
					if (livingPoint && playerPose) {
						playerPose->localPosition = FollowInverseRotateVector({out.pos.x-base.x, out.pos.y-base.y, out.pos.z-base.z}, angles);
						playerPose->localForward = FollowInverseRotateVector(FollowRotateVector({1,0,0}, out.ang), angles);
						playerPose->localUp = FollowInverseRotateVector(FollowRotateVector({0,0,1}, out.ang), angles);
						playerPose->calibrated = true;
					} else if (deadPlayer) {
						out.pos = base;
						out.ang = angles;
						if (playerPose && playerPose->calibrated) {
							const auto offset = FollowRotateVector(playerPose->localPosition, angles);
							out.pos = {base.x+offset.x, base.y+offset.y, base.z+offset.z};
							out.ang = FollowAnglesFromBasis(FollowRotateVector(playerPose->localForward, angles),
								FollowRotateVector(playerPose->localUp, angles));
						}
						out.valid = true;
						out.oriented = true;
						out.attachKind = "ragdoll-mount";
						out.attachIndex = index;
						out.source = std::string("ragdoll-") + anchor;
						return out;
					}
				}
			}
			if (deadPlayer) { out.source = "missing-ragdoll-mount"; return out; }
			if (livingPoint) {
				if (rigid && (out.attachId == "head" || out.attachId == "chest" || out.attachId == "pelvis")) {
					out.valid = false;
					out.source = "missing-body-mount";
				} else if (rigid && out.attachId == "entity") {
					out.oriented = ReadSceneAngles(entity, out.ang);
				}
				return out;
			}
		}
	}

	if (out.attachId != "entity") {
		uint8_t idx = entity->LookupAttachment(out.attachId.c_str());
		const char* alias = AttachmentAlias(out.attachId);
		bool aliased = false;
		if (!idx && type == FollowTargetType::Weapon && alias) {
			idx = entity->LookupAttachment(alias);
			aliased = idx != 0;
		}
		SOURCESDK::Vector origin;
		SOURCESDK::Quaternion q;
		if (idx && entity->GetAttachment(idx, origin, q)) {
			out.valid = true;
			out.oriented = true;
			out.attachKind = "attachment";
			out.attachIndex = idx;
			out.source = aliased ? std::string("alias-") + alias : "model-attachment";
			out.pos = FollowVec3{ origin.x, origin.y, origin.z };
			out.ang = FollowQuatToAngles(q.x, q.y, q.z, q.w);
			return out;
		}
		// A knife has no muzzle. While it is drawn, ride the grip rather than a root at
		// the pawn's feet, and retry the requested gun attachment on every weapon switch.
		if (rigid && type == FollowTargetType::Weapon) {
			const auto ownerHandle = entity->GetOwnerEntityHandle();
			auto* owner = ownerHandle.IsValid() ? EntityAt(ownerHandle.GetEntryIndex()) : nullptr;
			if (owner && owner->IsPlayerPawn() && owner->GetActiveWeaponHandle().ToInt() == entity->GetHandle().ToInt()) {
				const auto hand = ResolveAttachTransform(owner, ownerHandle.GetEntryIndex(), FollowTargetType::Player, "weapon_hand_R", nullptr, true);
				out.pos = hand.pos; out.ang = hand.ang;
				out.valid = hand.valid; out.oriented = hand.oriented;
				out.attachKind = "hand-fallback";
				out.attachIndex = hand.attachIndex;
				out.source = "fallback-held-hand";
				return out;
			}
		}
	}

	out.attachKind = "entity";
	out.attachIndex = 0;
	out.source = out.attachId == "entity" ? "entity-origin" : "fallback-entity-origin";
	// A missing hand is not the pawn's feet. Keep the last camera pose until that
	// bone returns. Weapons can use their own oriented root as a temporary fallback.
	out.valid = !deadPlayer && !(rigid && type == FollowTargetType::Player && out.attachId != "entity")
		&& ReadOrigin(entity, out.pos);
	out.oriented = out.valid && ReadSceneAngles(entity, out.ang);
	if (out.oriented) out.source = out.attachId == "entity" ? "entity-transform" : "fallback-entity-transform";
	return out;
}

} // namespace Filmmaker
