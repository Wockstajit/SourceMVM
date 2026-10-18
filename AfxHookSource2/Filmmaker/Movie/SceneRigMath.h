#pragma once

// Engine-free scene object rigs (Docs/features/scene-object-rigs.md): what a placed scene object
// is attached to, the pose it keeps in its parent's frame, and how a chain of attachments resolves
// to a world pose. The live entity reads are SceneRig.cpp's; everything here takes them as inputs.
//
// Frames. A local pose is x forward, y right, z up of its parent, with pitch / yaw / roll composed
// on the parent's rotation: the Follow Camera's attach offset (FollowRotateVector,
// FollowComposeRotation), so a light on a head and a camera on a head agree on what "20 forward"
// means. An unattached object's local pose IS its world pose.
//
// Unit-tested in Automation/tests/scene-rig-math-tests.cpp.

#include "FollowCameraMath.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/JsonParser.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Filmmaker::Rig {

struct Pose {
	FollowVec3 pos;
	FollowAngles ang;
};

inline double WrapDegrees(double deg) {
	if (!std::isfinite(deg)) return 0.0;
	deg = std::fmod(deg, 360.0);
	if (deg > 180.0) deg -= 360.0;
	if (deg <= -180.0) deg += 360.0;
	return deg;
}

inline FollowAngles WrapAngles(const FollowAngles& a) {
	return {WrapDegrees(a.pitch), WrapDegrees(a.yaw), WrapDegrees(a.roll)};
}

// World pose of a child riding `parent` at `local`.
inline Pose Compose(const Pose& parent, const Pose& local) {
	const FollowVec3 d = FollowRotateVector(local.pos, parent.ang);
	Pose out;
	out.pos = {parent.pos.x + d.x, parent.pos.y + d.y, parent.pos.z + d.z};
	out.ang = WrapAngles(FollowComposeRotation(parent.ang, local.ang));
	return out;
}

// The local pose that puts a child of `parent` at `world` (Compose's inverse).
inline Pose Decompose(const Pose& world, const Pose& parent) {
	Pose out;
	out.pos = FollowInverseRotateVector(
		{world.pos.x - parent.pos.x, world.pos.y - parent.pos.y, world.pos.z - parent.pos.z}, parent.ang);
	out.ang = WrapAngles(FollowRelativeRotation(world.ang, parent.ang));
	return out;
}

// ---- what an object is attached to ----------------------------------------------------------

enum class ParentKind : int {
	None = 0,
	Player,   // a player pawn, at a body mount (head, eyes, chest, pelvis, feet, hands)
	Held,     // whatever weapon that player holds this frame, at a weapon mount
	Dropped,  // a weapon or the C4 lying in the world
	Grenade,  // a thrown projectile
	Object,   // another placed scene object
};

enum class Missing : int {
	Hide = 0, // not drawn while the parent is missing
	Hold,     // stays at the last pose it had while the parent was there
};

inline const char* ParentKindName(ParentKind k) {
	switch (k) {
	case ParentKind::Player: return "player";
	case ParentKind::Held: return "held";
	case ParentKind::Dropped: return "dropped";
	case ParentKind::Grenade: return "grenade";
	case ParentKind::Object: return "object";
	default: return "none";
	}
}

inline bool ParseParentKind(const std::string& s, ParentKind& out) {
	for (int i = 0; i <= int(ParentKind::Object); ++i)
		if (s == ParentKindName(ParentKind(i))) { out = ParentKind(i); return true; }
	return false;
}

// Identities are re-resolved every frame against the live entities, never cached across ticks:
// a pawn is re-created every round, a weapon is a new entity once dropped, and a grenade only
// exists between its throw and its end. Each kind keeps what survives that.
struct Attachment {
	ParentKind parent = ParentKind::None;
	// Player / Held: Steam id first (0 = unknown, bots), the scoreboard name second.
	uint64_t steamId = 0;
	std::string playerName;
	// Grenade: its entity handle in the demo first (the demo records it, so a seek gives the same
	// grenade the same handle; 0 = unknown), then its type and the tick it was thrown (matched
	// within kGrenadeTickSlack), the thrower breaking ties between two of a kind.
	uint64_t grenadeHandle = 0;
	std::string grenadeType;
	int throwTick = -1;
	std::string thrower;
	// Dropped: the entity index and designer class it had when picked. The weakest identity: valid
	// while that index still holds that class with no owner.
	int entityIndex = -1;
	std::string entityClass;
	// Object: the parent scene object's id.
	int object = 0;
	std::string mount;            // the parent's attach point ("" = its origin)
	Missing missing = Missing::Hide;
	Pose offset;                  // the rest pose in the parent's frame (pose keys override it)

	bool operator==(const Attachment& b) const;
	bool operator!=(const Attachment& b) const { return !(*this == b); }
};

constexpr int kGrenadeTickSlack = 4;
constexpr int kMaxChainDepth = 8;
constexpr int kMaxObjectId = 1000000;

inline bool SamePose(const Pose& a, const Pose& b) {
	return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.pos.z == b.pos.z &&
		a.ang.pitch == b.ang.pitch && a.ang.yaw == b.ang.yaw && a.ang.roll == b.ang.roll;
}

inline bool Attachment::operator==(const Attachment& b) const {
	return parent == b.parent && steamId == b.steamId && playerName == b.playerName &&
		grenadeHandle == b.grenadeHandle && grenadeType == b.grenadeType && throwTick == b.throwTick && thrower == b.thrower &&
		entityIndex == b.entityIndex && entityClass == b.entityClass && object == b.object &&
		mount == b.mount && missing == b.missing && SamePose(offset, b.offset);
}

inline const char* DefaultMount(ParentKind k) {
	switch (k) {
	case ParentKind::Player: return "head";
	case ParentKind::Held: return "muzzle";
	default: return "entity";
	}
}

// Would making `child` ride scene object `parent` close a loop (or nest deeper than the limit)?
inline bool WouldCycle(const std::map<int, Attachment>& attach, int child, int parent) {
	int at = parent;
	for (int depth = 0; depth <= kMaxChainDepth; ++depth) {
		if (at == child) return true;
		const auto a = attach.find(at);
		if (a == attach.end() || a->second.parent != ParentKind::Object) return false;
		at = a->second.object;
	}
	return true;
}

// ---- resolving a chain ----------------------------------------------------------------------

// A parent outside the scene (player, weapon, grenade) as SceneRig.cpp read it this frame.
struct ExternalPose {
	bool valid = false;
	bool oriented = false; // false: a position only; the child keeps world-aligned axes
	Pose pose;
};

struct Resolved {
	bool valid = false;   // false: some parent up the chain is missing (or the chain is broken)
	Pose world;
	Pose parent;          // the immediate parent's world pose (its frame), valid with `valid`
	bool attached = false;
	int depth = 0;        // how many attachments were followed
};

// local(id, pose): the object's local pose this frame; false when it does not exist or is off.
// attachment(id): what it rides, nullptr when it sits in world space.
// external(attachment): the live pose of a player / weapon / grenade parent.
using LocalFn = std::function<bool(int, Pose&)>;
using AttachFn = std::function<const Attachment*(int)>;
using ExternalFn = std::function<ExternalPose(int, const Attachment&)>;

inline Resolved ResolveWorld(int id, const LocalFn& local, const AttachFn& attachment,
	const ExternalFn& external, int depth = 0) {
	Resolved r;
	Pose mine;
	if (depth > kMaxChainDepth || !local(id, mine)) return r;
	const Attachment* a = attachment(id);
	if (!a || a->parent == ParentKind::None) {
		r.valid = true; r.world = mine; r.depth = depth;
		return r;
	}
	r.attached = true;
	Pose parent;
	if (a->parent == ParentKind::Object) {
		if (a->object == id) return r;
		const Resolved up = ResolveWorld(a->object, local, attachment, external, depth + 1);
		if (!up.valid) return r;
		parent = up.world;
		r.depth = up.depth;
	} else {
		const ExternalPose e = external(id, *a);
		if (!e.valid) return r;
		parent = e.pose;
		if (!e.oriented) parent.ang = {};
		r.depth = depth + 1;
	}
	r.valid = true;
	r.parent = parent;
	r.world = Compose(parent, mine);
	return r;
}

// The Missing policy over one object's frames: returns whether it is drawn, and where.
struct HoldState {
	bool have = false;
	Pose last;
};

inline bool ApplyMissing(const Resolved& r, Missing policy, HoldState& hold, Pose& out) {
	if (r.valid) { hold.have = true; hold.last = r.world; out = r.world; return true; }
	if (policy == Missing::Hold && hold.have) { out = hold.last; return true; }
	return false;
}

// ---- pose channels --------------------------------------------------------------------------
// Each object animates through scene-key channels "obj.<id>.<key>" (SceneKeyframeTrackMath.h's
// ObjectSpec). The dock addresses a channel by number; object channels live above the static
// catalog so adding or removing an object never renumbers another one under a drag.

enum PoseChannel : int { X = 0, Y, Z, Pitch, Yaw, Roll, Fov, kPoseChannels };
// A light's settings, keyable beside its pose: brightness, range, beam angle, emitter size and an
// area light's panel. They follow the pose channels in an object's channel numbers.
enum LightChannel : int { Intensity = kPoseChannels, Range, Cone, Radius, Width, Height, kObjectChannels };
constexpr int kLightChannels = kObjectChannels - kPoseChannels;
constexpr int kObjectChannelBase = 1000000;
constexpr int kChannelStride = 16; // channel numbers per object (room past kObjectChannels)

inline const char* ChannelKey(int c) {
	static const char* keys[] = {"x", "y", "z", "pitch", "yaw", "roll", "fov", "intensity", "range", "cone", "radius", "width", "height"};
	return c >= 0 && c < kObjectChannels ? keys[c] : "";
}

inline int ChannelOfKey(const std::string& key) {
	for (int c = 0; c < kObjectChannels; ++c) if (key == ChannelKey(c)) return c;
	return -1;
}

inline bool IsLightKind(const std::string& kind) { return kind == "point" || kind == "spot" || kind == "area"; }

// The channels an object of `kind` has on the TIMELINE, in row order: a camera's pose and field
// of view; a light's pose and its settings (a beam angle for spot and area lights, a panel for an
// area light, an emitter size for the others); anything else its pose.
inline std::vector<int> ChannelsFor(const std::string& kind) {
	std::vector<int> out = {X, Y, Z, Pitch, Yaw, Roll};
	if (kind == "camera") out.push_back(Fov);
	else if (IsLightKind(kind)) {
		out.push_back(Intensity);
		out.push_back(Range);
		if (kind != "point") out.push_back(Cone);
		if (kind == "area") { out.push_back(Width); out.push_back(Height); }
		else out.push_back(Radius);
	}
	return out;
}

inline std::string ChannelId(int objectId, int c) {
	return "obj." + std::to_string(objectId) + "." + ChannelKey(c);
}

inline bool ParseObjectId(const std::string& s, int& out) {
	if (s.empty() || s.size() > 7 || s[0] == '0') return false;
	int n = 0;
	for (char ch : s) { if (ch < '0' || ch > '9') return false; n = n * 10 + (ch - '0'); }
	if (n < 1 || n > kMaxObjectId) return false;
	out = n;
	return true;
}

inline bool ParseChannelId(const std::string& id, int& objectId, int& c) {
	if (id.rfind("obj.", 0) != 0) return false;
	const size_t dot = id.find('.', 4);
	if (dot == std::string::npos) return false;
	if (!ParseObjectId(id.substr(4, dot - 4), objectId)) return false;
	c = ChannelOfKey(id.substr(dot + 1));
	return c >= 0;
}

inline int ChannelIndex(int objectId, int c) { return kObjectChannelBase + objectId * kChannelStride + c; }

inline bool ParseChannelIndex(int index, int& objectId, int& c) {
	if (index < kObjectChannelBase) return false;
	const int rel = index - kObjectChannelBase;
	objectId = rel / kChannelStride; c = rel % kChannelStride;
	return objectId >= 1 && objectId <= kMaxObjectId && c < kObjectChannels;
}

inline void PoseToChannels(const Pose& p, double fov, double out[kPoseChannels]) {
	out[X] = p.pos.x; out[Y] = p.pos.y; out[Z] = p.pos.z;
	out[Pitch] = WrapDegrees(p.ang.pitch); out[Yaw] = WrapDegrees(p.ang.yaw); out[Roll] = WrapDegrees(p.ang.roll);
	out[Fov] = fov;
}

inline Pose ChannelsToPose(const double v[kPoseChannels]) {
	Pose p;
	p.pos = {v[X], v[Y], v[Z]};
	p.ang = {v[Pitch], v[Yaw], v[Roll]};
	return p;
}

// ---- serialization (the "attach" member of the scenekeys section) -----------------------------

inline void WriteAttachments(JsonBuilder& j, const std::map<int, Attachment>& attach) {
	j.BeginObject();
	for (const auto& e : attach) {
		const Attachment& a = e.second;
		j.Key(std::to_string(e.first).c_str());
		j.BeginObject();
		j.StringField("parent", ParentKindName(a.parent));
		switch (a.parent) {
		case ParentKind::Player:
		case ParentKind::Held:
			j.StringField("steamId", std::to_string(a.steamId));
			j.StringField("name", a.playerName);
			break;
		case ParentKind::Grenade:
			j.StringField("type", a.grenadeType);
			j.StringField("handle", std::to_string(a.grenadeHandle));
			j.IntField("throwTick", a.throwTick);
			j.StringField("thrower", a.thrower);
			break;
		case ParentKind::Dropped:
			j.IntField("entity", a.entityIndex);
			j.StringField("class", a.entityClass);
			break;
		case ParentKind::Object:
			j.IntField("object", a.object);
			break;
		default: break;
		}
		j.StringField("mount", a.mount);
		j.StringField("missing", a.missing == Missing::Hold ? "hold" : "hide");
		j.Key("offset"); j.BeginArray();
		for (double n : {a.offset.pos.x, a.offset.pos.y, a.offset.pos.z, a.offset.ang.pitch, a.offset.ang.yaw, a.offset.ang.roll})
			j.ValueDouble(n);
		j.EndArray();
		j.EndObject();
	}
	j.EndObject();
}

inline bool PlainText(const std::string& s, size_t max) {
	if (s.size() > max) return false;
	for (unsigned char c : s) if (c < 0x20) return false;
	return true;
}

inline bool MountName(const std::string& s) {
	if (s.size() > 64) return false;
	for (unsigned char c : s) if (!(std::isalnum(c) || c == '_')) return false;
	return true;
}

// Validates everything; `out` is replaced only on success.
inline bool ReadAttachments(const JsonValue& v, std::map<int, Attachment>& out, std::string& error) {
	if (v.type != JsonValue::Type::Object) { error = "Invalid attach map"; return false; }
	std::map<int, Attachment> pending;
	auto str = [](const JsonValue& o, const char* k, std::string& s) {
		const JsonValue* p = o.Find(k); if (!p || p->type != JsonValue::Type::String) return false; s = p->str; return true;
	};
	auto num = [](const JsonValue& o, const char* k, double& n) {
		const JsonValue* p = o.Find(k); if (!p || p->type != JsonValue::Type::Number || !std::isfinite(p->number)) return false; n = p->number; return true;
	};
	for (const auto& e : v.obj) {
		int id = 0;
		if (!ParseObjectId(e.first, id) || pending.count(id) || e.second.type != JsonValue::Type::Object) { error = "Invalid attached object id"; return false; }
		const JsonValue& o = e.second;
		Attachment a;
		std::string kind, missing;
		if (!str(o, "parent", kind) || !ParseParentKind(kind, a.parent) || a.parent == ParentKind::None) { error = "Invalid attachment parent"; return false; }
		if (a.parent == ParentKind::Player || a.parent == ParentKind::Held) {
			std::string steam;
			if (!str(o, "steamId", steam) || !str(o, "name", a.playerName) || steam.empty() || steam.size() > 20 || !PlainText(a.playerName, 128)) { error = "Invalid attached player"; return false; }
			for (char c : steam) if (c < '0' || c > '9') { error = "Invalid attached player"; return false; }
			a.steamId = std::strtoull(steam.c_str(), nullptr, 10);
			if (!a.steamId && a.playerName.empty()) { error = "Attached player has no identity"; return false; }
		} else if (a.parent == ParentKind::Grenade) {
			double tick = -1;
			if (!str(o, "type", a.grenadeType) || !num(o, "throwTick", tick) || !str(o, "thrower", a.thrower) ||
				!PlainText(a.grenadeType, 32) || a.grenadeType.empty() || !PlainText(a.thrower, 128) ||
				tick < 0 || tick > 2147483647.0 || tick != std::floor(tick)) { error = "Invalid attached grenade"; return false; }
			a.throwTick = int(tick);
			std::string handle;
			if (str(o, "handle", handle)) {
				if (handle.empty() || handle.size() > 20) { error = "Invalid attached grenade"; return false; }
				for (char c : handle) if (c < '0' || c > '9') { error = "Invalid attached grenade"; return false; }
				a.grenadeHandle = std::strtoull(handle.c_str(), nullptr, 10);
			}
		} else if (a.parent == ParentKind::Dropped) {
			double index = -1;
			if (!num(o, "entity", index) || !str(o, "class", a.entityClass) || index < 0 || index > 32767 ||
				index != std::floor(index) || !PlainText(a.entityClass, 64) || a.entityClass.empty()) { error = "Invalid attached weapon"; return false; }
			a.entityIndex = int(index);
		} else {
			double object = 0;
			if (!num(o, "object", object) || object < 1 || object > kMaxObjectId || object != std::floor(object) || int(object) == id) { error = "Invalid parent object"; return false; }
			a.object = int(object);
		}
		if (!str(o, "mount", a.mount) || !MountName(a.mount)) { error = "Invalid attach mount"; return false; }
		if (!str(o, "missing", missing) || (missing != "hide" && missing != "hold")) { error = "Invalid missing policy"; return false; }
		a.missing = missing == "hold" ? Missing::Hold : Missing::Hide;
		const JsonValue* off = o.Find("offset");
		if (!off || off->type != JsonValue::Type::Array || off->arr.size() != 6) { error = "Invalid attach offset"; return false; }
		double n[6];
		for (int i = 0; i < 6; ++i) {
			if (off->arr[i].type != JsonValue::Type::Number || !std::isfinite(off->arr[i].number)) { error = "Invalid attach offset"; return false; }
			n[i] = off->arr[i].number;
			if ((i < 3 && std::fabs(n[i]) > 32768) || (i >= 3 && std::fabs(n[i]) > 360)) { error = "Attach offset out of range"; return false; }
		}
		a.offset.pos = {n[0], n[1], n[2]};
		a.offset.ang = {n[3], n[4], n[5]};
		pending[id] = a;
	}
	for (const auto& e : pending)
		if (e.second.parent == ParentKind::Object && WouldCycle(pending, e.first, e.second.object)) { error = "Attachment loop"; return false; }
	out = std::move(pending);
	return true;
}

} // namespace Filmmaker::Rig
