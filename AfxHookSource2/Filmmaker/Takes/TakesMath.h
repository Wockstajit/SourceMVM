#pragma once

// BOT TAKES, the engine-free half (see Docs/features/bot-takes.md).
//
// A take is a recording of what one player did in a private match: every user command the server
// ran for them, tick by tick, next to where that command left them. Playing a take hands those
// commands to a real bot, so it walks, jumps, shoots and throws through the game's own code.
//
// Commands are stored as CSGOUserCmdPB wire bytes. The wire format is stable across game updates
// (the field numbers below are read from the descriptors inside server.dll), while the C++ layout
// of the generated message is not, so a saved take survives an update that moves those fields.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace Filmmaker::Takes {

// -- Protobuf wire format --------------------------------------------------------------------------

enum WireType : uint32_t { WireVarint = 0, WireFixed64 = 1, WireBytes = 2, WireFixed32 = 5 };

inline bool ReadVarint(const uint8_t *&p, const uint8_t *end, uint64_t &out) {
	out = 0;
	for (int shift = 0; shift < 70 && p < end; shift += 7) {
		const uint8_t c = *p++;
		out |= uint64_t(c & 0x7f) << shift;
		if (!(c & 0x80)) return true;
	}
	return false;
}

inline void WriteVarint(std::vector<uint8_t> &out, uint64_t v) {
	while (v >= 0x80) {
		out.push_back(uint8_t(v) | 0x80);
		v >>= 7;
	}
	out.push_back(uint8_t(v));
}

inline void WriteTag(std::vector<uint8_t> &out, uint32_t field, WireType type) {
	WriteVarint(out, (uint64_t(field) << 3) | type);
}

// An int32 field: protobuf encodes negative values as ten-byte varints.
inline void WriteInt32Field(std::vector<uint8_t> &out, uint32_t field, int32_t v) {
	WriteTag(out, field, WireVarint);
	WriteVarint(out, uint64_t(int64_t(v)));
}

inline void WriteUint32Field(std::vector<uint8_t> &out, uint32_t field, uint32_t v) {
	WriteTag(out, field, WireVarint);
	WriteVarint(out, v);
}

inline void WriteFloatField(std::vector<uint8_t> &out, uint32_t field, float v) {
	WriteTag(out, field, WireFixed32);
	const size_t at = out.size();
	out.resize(at + 4);
	std::memcpy(out.data() + at, &v, 4);
}

inline void WriteBytesField(std::vector<uint8_t> &out, uint32_t field, const uint8_t *data, size_t size) {
	WriteTag(out, field, WireBytes);
	WriteVarint(out, size);
	out.insert(out.end(), data, data + size);
}

struct WireField {
	uint32_t number = 0;
	uint32_t type = 0;
	uint64_t varint = 0;           // WireVarint
	uint32_t fixed32 = 0;          // WireFixed32
	const uint8_t *data = nullptr; // WireBytes payload
	size_t size = 0;               // WireBytes payload size
	const uint8_t *rawBegin = nullptr; // the whole field, tag included
	const uint8_t *rawEnd = nullptr;
};

// Calls fn(field) for each field. False when the bytes are not valid wire data.
template <class Fn> inline bool ForEachField(const uint8_t *data, size_t size, Fn &&fn) {
	const uint8_t *p = data, *end = data + size;
	while (p < end) {
		WireField f;
		f.rawBegin = p;
		uint64_t tag = 0;
		if (!ReadVarint(p, end, tag)) return false;
		f.number = uint32_t(tag >> 3);
		f.type = uint32_t(tag & 7);
		switch (f.type) {
		case WireVarint:
			if (!ReadVarint(p, end, f.varint)) return false;
			break;
		case WireFixed64:
			if (end - p < 8) return false;
			p += 8;
			break;
		case WireBytes: {
			uint64_t n = 0;
			if (!ReadVarint(p, end, n) || n > uint64_t(end - p)) return false;
			f.data = p;
			f.size = size_t(n);
			p += n;
			break;
		}
		case WireFixed32:
			if (end - p < 4) return false;
			std::memcpy(&f.fixed32, p, 4);
			p += 4;
			break;
		default:
			return false;
		}
		f.rawEnd = p;
		fn(f);
	}
	return true;
}

inline float FixedToFloat(uint32_t v) {
	float out;
	std::memcpy(&out, &v, 4);
	return out;
}

// Field numbers, from usercmd.proto and cs_usercmd.proto as embedded in server.dll.
namespace Field {
// CSGOUserCmdPB
constexpr uint32_t kBase = 1, kInputHistory = 2, kPredictBodyFx = 11, kPredictHeadFx = 12, kPredictRagdolls = 13;
// CBaseUserCmdPB
constexpr uint32_t kLegacyCommandNumber = 1, kClientTick = 2, kButtons = 3, kViewAngles = 4, kForward = 5, kLeft = 6,
                   kUp = 7, kWeaponSelect = 9, kPawnHandle = 14, kPredictionOffset = 17, kMoveCrc = 19,
                   kConsumedAngleChanges = 20, kExecutionNotes = 22;
// CInButtonStatePB
constexpr uint32_t kButtonState1 = 1;
// CSGOInputHistoryEntryPB
constexpr uint32_t kHistoryTargetEnt = 65, kHistoryShootPos = 66, kHistoryTargetHead = 67, kHistoryTargetAbs = 68,
                   kHistoryTargetAng = 69;
} // namespace Field

// The parts of a recorded command the rest of the feature reads.
struct CmdSummary {
	bool valid = false;
	int32_t clientTick = 0;
	int32_t weaponSelect = 0;
	uint64_t buttons = 0;
	float pitch = 0, yaw = 0, roll = 0;
	float forward = 0, left = 0, up = 0;
	int subtickMoves = 0;
	int historyEntries = 0;
};

inline CmdSummary SummarizeCmd(const uint8_t *data, size_t size) {
	CmdSummary s;
	s.valid = ForEachField(data, size, [&](const WireField &f) {
		if (f.number == Field::kInputHistory && f.type == WireBytes) {
			++s.historyEntries;
			return;
		}
		if (f.number != Field::kBase || f.type != WireBytes) return;
		ForEachField(f.data, f.size, [&](const WireField &b) {
			switch (b.number) {
			case Field::kClientTick: s.clientTick = int32_t(b.varint); break;
			case Field::kWeaponSelect: s.weaponSelect = int32_t(b.varint); break;
			case Field::kForward: s.forward = FixedToFloat(b.fixed32); break;
			case Field::kLeft: s.left = FixedToFloat(b.fixed32); break;
			case Field::kUp: s.up = FixedToFloat(b.fixed32); break;
			case 18: ++s.subtickMoves; break;
			case Field::kButtons:
				ForEachField(b.data, b.size, [&](const WireField &bs) {
					if (bs.number == Field::kButtonState1) s.buttons = bs.varint;
				});
				break;
			case Field::kViewAngles:
				ForEachField(b.data, b.size, [&](const WireField &a) {
					if (a.number == 1) s.pitch = FixedToFloat(a.fixed32);
					if (a.number == 2) s.yaw = FixedToFloat(a.fixed32);
					if (a.number == 3) s.roll = FixedToFloat(a.fixed32);
				});
				break;
			default: break;
			}
		});
	});
	return s;
}

// What a recorded command needs changed before it can drive a bot instead of the player.
struct CmdRewrite {
	int32_t clientTick = 0;
	uint32_t pawnHandle = 0;
	int32_t weaponSelect = 0;     // the entity index of the BOT's weapon; 0 sends no switch
	bool hasEye = false;          // shots leave from here instead of the recorded eye
	float eye[3] = {0, 0, 0};
	bool stripAttack = false;     // "bot_dont_shoot" for one take: the buttons survive, attacks do not
};

constexpr uint64_t kInAttack = 1ull << 0, kInAttack2 = 1ull << 11;

inline void WriteVectorMessage(std::vector<uint8_t> &out, uint32_t field, const float v[3]) {
	std::vector<uint8_t> inner;
	WriteFloatField(inner, 1, v[0]);
	WriteFloatField(inner, 2, v[1]);
	WriteFloatField(inner, 3, v[2]);
	WriteBytesField(out, field, inner.data(), inner.size());
}

inline std::vector<uint8_t> RewriteHistoryEntry(const uint8_t *data, size_t size, const CmdRewrite &r) {
	std::vector<uint8_t> out;
	out.reserve(size + 16);
	ForEachField(data, size, [&](const WireField &f) {
		switch (f.number) {
		// Aimed at whatever the player's client had under the crosshair: entity indexes and poses
		// from the recording session, which mean nothing to this bot.
		case Field::kHistoryTargetEnt:
		case Field::kHistoryTargetHead:
		case Field::kHistoryTargetAbs:
		case Field::kHistoryTargetAng:
			return;
		case Field::kHistoryShootPos:
			if (r.hasEye) return; // written below
			break;
		default: break;
		}
		out.insert(out.end(), f.rawBegin, f.rawEnd);
	});
	if (r.hasEye) WriteVectorMessage(out, Field::kHistoryShootPos, r.eye);
	return out;
}

inline std::vector<uint8_t> RewriteBase(const uint8_t *data, size_t size, const CmdRewrite &r) {
	std::vector<uint8_t> out;
	out.reserve(size + 16);
	ForEachField(data, size, [&](const WireField &f) {
		switch (f.number) {
		// Replaced below, or client bookkeeping about the player's connection that a bot never had.
		case Field::kLegacyCommandNumber:
		case Field::kClientTick:
		case Field::kWeaponSelect:
		case Field::kPawnHandle:
		case Field::kPredictionOffset:
		case Field::kMoveCrc:
		case Field::kConsumedAngleChanges:
		case Field::kExecutionNotes:
			return;
		case Field::kButtons:
			if (r.stripAttack) {
				std::vector<uint8_t> inner;
				ForEachField(f.data, f.size, [&](const WireField &b) {
					if (b.number == Field::kButtonState1 && b.type == WireVarint) {
						WriteTag(inner, b.number, WireVarint);
						WriteVarint(inner, b.varint & ~(kInAttack | kInAttack2));
					} else {
						inner.insert(inner.end(), b.rawBegin, b.rawEnd);
					}
				});
				WriteBytesField(out, f.number, inner.data(), inner.size());
				return;
			}
			break;
		default: break;
		}
		out.insert(out.end(), f.rawBegin, f.rawEnd);
	});
	WriteInt32Field(out, Field::kLegacyCommandNumber, r.clientTick);
	WriteInt32Field(out, Field::kClientTick, r.clientTick);
	if (r.pawnHandle) WriteUint32Field(out, Field::kPawnHandle, r.pawnHandle);
	if (r.weaponSelect) WriteInt32Field(out, Field::kWeaponSelect, r.weaponSelect);
	return out;
}

// A recorded command, rewritten for the bot that plays it. Empty when the input is not valid.
inline std::vector<uint8_t> RewriteCmdForBot(const uint8_t *data, size_t size, const CmdRewrite &r) {
	std::vector<uint8_t> out;
	out.reserve(size + 32);
	const bool ok = ForEachField(data, size, [&](const WireField &f) {
		switch (f.number) {
		case Field::kBase:
			if (f.type == WireBytes) {
				const std::vector<uint8_t> base = RewriteBase(f.data, f.size, r);
				WriteBytesField(out, f.number, base.data(), base.size());
				return;
			}
			break;
		case Field::kInputHistory:
			if (f.type == WireBytes) {
				const std::vector<uint8_t> entry = RewriteHistoryEntry(f.data, f.size, r);
				WriteBytesField(out, f.number, entry.data(), entry.size());
				return;
			}
			break;
		case Field::kPredictBodyFx:
		case Field::kPredictHeadFx:
		case Field::kPredictRagdolls:
			return; // the player's client guessing at blood and ragdolls
		default: break;
		}
		out.insert(out.end(), f.rawBegin, f.rawEnd);
	});
	if (!ok) out.clear();
	return out;
}

// A bot standing still, looking where it was last told to.
inline std::vector<uint8_t> BuildIdleCmd(int32_t clientTick, uint32_t pawnHandle, float pitch, float yaw) {
	std::vector<uint8_t> angles;
	WriteFloatField(angles, 1, pitch);
	WriteFloatField(angles, 2, yaw);
	WriteFloatField(angles, 3, 0.0f);
	std::vector<uint8_t> buttons;
	WriteTag(buttons, Field::kButtonState1, WireVarint);
	WriteVarint(buttons, 0);
	std::vector<uint8_t> base;
	WriteInt32Field(base, Field::kLegacyCommandNumber, clientTick);
	WriteInt32Field(base, Field::kClientTick, clientTick);
	WriteBytesField(base, Field::kButtons, buttons.data(), buttons.size());
	WriteBytesField(base, Field::kViewAngles, angles.data(), angles.size());
	if (pawnHandle) WriteUint32Field(base, Field::kPawnHandle, pawnHandle);
	std::vector<uint8_t> out;
	WriteBytesField(out, Field::kBase, base.data(), base.size());
	return out;
}

// -- A take ----------------------------------------------------------------------------------------

constexpr uint16_t kNoName = 0xffff;

struct TickSample {
	float pos[3] = {0, 0, 0};   // where the command left the player
	float vel[3] = {0, 0, 0};
	float pitch = 0, yaw = 0;
	uint16_t activeWeapon = kNoName; // index into Track::names
	uint16_t flags = 0;              // kTickAlive
	uint32_t cmdOffset = 0, cmdSize = 0; // into Track::cmds
};
constexpr uint16_t kTickAlive = 1;
constexpr uint16_t kTickJumped = 2; // the player was moved by something no input explains (setpos, a trigger)

// More than this tick's own speed explains. Horizontally that is the speed either side of the tick,
// with room for subtick timing; vertically it also allows the 18-unit stair step movement snaps up.
inline bool MovedWithoutInput(const float before[3], const float beforeVel[3], const float after[3], const float afterVel[3],
                              float tickRate) {
	const float dt = 1.0f / (tickRate > 0 ? tickRate : 64.0f);
	const float dx = after[0] - before[0], dy = after[1] - before[1], dz = after[2] - before[2];
	const float v0 = std::sqrt(beforeVel[0] * beforeVel[0] + beforeVel[1] * beforeVel[1]);
	const float v1 = std::sqrt(afterVel[0] * afterVel[0] + afterVel[1] * afterVel[1]);
	const float flat = (std::max)(v0, v1) * dt * 1.5f + 4.0f;
	const float rise = (std::max)(std::fabs(beforeVel[2]), std::fabs(afterVel[2])) * dt * 1.5f + 24.0f;
	return dx * dx + dy * dy > flat * flat || std::fabs(dz) > rise;
}

struct InventoryEvent {
	uint32_t tick = 0;
	uint16_t name = kNoName;
	uint8_t added = 1;
};

struct WeaponSwap {
	std::string from, to; // weapon_ak47 -> weapon_m4a1
};

struct Track {
	std::string name;
	uint8_t team = 3; // 2 T, 3 CT
	bool enabled = true;
	bool holdFire = false;   // plays everything but never fires
	int32_t delayTicks = 0;  // starts this many ticks into the scene
	int32_t health = 100;
	int32_t armor = 0;
	uint8_t helmet = 0;

	std::vector<std::string> names;        // weapon classes used by this take
	std::vector<uint16_t> startWeapons;    // what it holds on tick 0
	std::vector<InventoryEvent> inventory; // what it picks up, buys, throws or drops after that
	std::vector<TickSample> ticks;
	std::vector<uint8_t> cmds;             // every tick's CSGOUserCmdPB

	// Changes made after the take was recorded: the performance stays, the player changes.
	std::string agentModel;              // characters/models/.../....vmdl; "" keeps the bot's own
	std::vector<WeaponSwap> weaponSwaps;
};

inline uint16_t InternName(std::vector<std::string> &names, const std::string &name) {
	for (size_t i = 0; i < names.size(); ++i)
		if (names[i] == name) return uint16_t(i);
	if (names.size() >= kNoName) return kNoName;
	names.push_back(name);
	return uint16_t(names.size() - 1);
}

inline const std::string &SwappedWeapon(const Track &t, const std::string &recorded) {
	for (const WeaponSwap &s : t.weaponSwaps)
		if (s.from == recorded) return s.to;
	return recorded;
}

// Something placed in the scene. `solid` makes it an obstacle for players and bots alike.
struct SceneObject {
	std::string name;  // unique; becomes the entity's targetname
	std::string model; // models/.../....vmdl
	float pos[3] = {0, 0, 0};
	float ang[3] = {0, 0, 0};
	float scale = 1.0f;
	bool solid = true;
};

struct Scene {
	std::string map;
	float tickRate = 64.0f;
	std::vector<Track> tracks;
	std::vector<SceneObject> objects;
};

// -- The clock ---------------------------------------------------------------------------------------

// The tick a scene is over: its longest enabled take, plus any delay that take starts with.
inline int32_t SceneLengthTicks(const Scene &s) {
	int32_t length = 0;
	for (const Track &t : s.tracks)
		if (t.enabled) length = (std::max)(length, t.delayTicks + int32_t(t.ticks.size()));
	return length;
}

enum class FrameKind { BeforeStart, Playing, Finished };

struct TrackFrame {
	FrameKind kind = FrameKind::BeforeStart;
	int32_t index = 0; // Playing: index into Track::ticks
};

inline TrackFrame FrameForSceneTick(const Track &t, int32_t sceneTick) {
	TrackFrame f;
	const int32_t local = sceneTick - t.delayTicks;
	if (local < 0) return f;
	if (local >= int32_t(t.ticks.size())) {
		f.kind = FrameKind::Finished;
		f.index = int32_t(t.ticks.size()) - 1;
		return f;
	}
	f.kind = FrameKind::Playing;
	f.index = local;
	return f;
}

inline float Distance3(const float a[3], const float b[3]) {
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The game's movement is deterministic, but a bot's start is never perfectly the player's (subtick
// timing, what it is carrying, something moved into the way). A bot this far off its take is put
// back. Zero turns correction off, so placed objects can knock it off the recorded path.
inline bool ShouldCorrectDrift(float distance, float threshold) { return threshold > 0.0f && distance > threshold; }

// Names the next take "Take 3" unless that name is already used.
inline std::string NextTakeName(const std::vector<Track> &tracks) {
	for (int n = int(tracks.size()) + 1;; ++n) {
		const std::string name = "Take " + std::to_string(n);
		bool used = false;
		for (const Track &t : tracks) used = used || t.name == name;
		if (!used) return name;
	}
}

// Keeps what a file name may carry; a take name typed in the panel can hold anything.
// A bot has no SteamID, and Customize keeps its per-player skins by SteamID, so in a demo it could not
// touch a bot's guns. A performer gets one made up from its take's name, the same on every pass and in
// every demo, in a range of account ids Steam does not hand out.
inline uint64_t PerformerSteamId(const std::string &takeName) {
	uint32_t hash = 2166136261u;
	for (const char c : takeName) hash = (hash ^ uint8_t(c)) * 16777619u;
	return 0x01100001FF000000ull | (hash & 0x00FFFFFFu);
}

inline std::string SafeFileName(const std::string &name) {
	std::string out;
	for (char c : name) {
		const unsigned char u = static_cast<unsigned char>(c);
		const bool ok = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u == '-' || u == ' ';
		out.push_back(ok ? c : '_');
	}
	while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
	if (out.empty()) out = "scene";
	return out.substr(0, 64);
}

// -- The file ----------------------------------------------------------------------------------------

constexpr uint32_t kSceneMagic = 0x4b545653; // "SVTK"
constexpr uint32_t kSceneVersion = 2;

class ByteWriter {
public:
	std::vector<uint8_t> bytes;
	template <class T> void Pod(const T &v) {
		const size_t at = bytes.size();
		bytes.resize(at + sizeof(T));
		std::memcpy(bytes.data() + at, &v, sizeof(T));
	}
	void Str(const std::string &s) {
		Pod(uint32_t(s.size()));
		bytes.insert(bytes.end(), s.begin(), s.end());
	}
	void Raw(const void *data, size_t size) {
		const uint8_t *p = static_cast<const uint8_t *>(data);
		bytes.insert(bytes.end(), p, p + size);
	}
};

class ByteReader {
public:
	ByteReader(const uint8_t *data, size_t size) : p_(data), end_(data + size) {}
	bool ok = true;
	template <class T> T Pod() {
		T v{};
		if (size_t(end_ - p_) < sizeof(T)) {
			ok = false;
			return v;
		}
		std::memcpy(&v, p_, sizeof(T));
		p_ += sizeof(T);
		return v;
	}
	std::string Str() {
		const uint32_t n = Pod<uint32_t>();
		if (!ok || n > size_t(end_ - p_)) {
			ok = false;
			return {};
		}
		std::string s(reinterpret_cast<const char *>(p_), n);
		p_ += n;
		return s;
	}
	bool Raw(void *out, size_t size) {
		if (size_t(end_ - p_) < size) return ok = false;
		std::memcpy(out, p_, size);
		p_ += size;
		return true;
	}
	size_t Remaining() const { return size_t(end_ - p_); }

private:
	const uint8_t *p_, *end_;
};

inline std::vector<uint8_t> SerializeScene(const Scene &s) {
	ByteWriter w;
	w.Pod(kSceneMagic);
	w.Pod(kSceneVersion);
	w.Str(s.map);
	w.Pod(s.tickRate);
	w.Pod(uint32_t(s.tracks.size()));
	for (const Track &t : s.tracks) {
		w.Str(t.name);
		w.Pod(t.team);
		w.Pod(uint8_t(t.enabled));
		w.Pod(uint8_t(t.holdFire));
		w.Pod(t.delayTicks);
		w.Pod(t.health);
		w.Pod(t.armor);
		w.Pod(t.helmet);
		w.Pod(uint32_t(t.names.size()));
		for (const std::string &n : t.names) w.Str(n);
		w.Pod(uint32_t(t.startWeapons.size()));
		for (uint16_t n : t.startWeapons) w.Pod(n);
		w.Pod(uint32_t(t.inventory.size()));
		for (const InventoryEvent &e : t.inventory) {
			w.Pod(e.tick);
			w.Pod(e.name);
			w.Pod(e.added);
		}
		w.Pod(uint32_t(t.ticks.size()));
		if (!t.ticks.empty()) w.Raw(t.ticks.data(), t.ticks.size() * sizeof(TickSample));
		w.Pod(uint32_t(t.cmds.size()));
		if (!t.cmds.empty()) w.Raw(t.cmds.data(), t.cmds.size());
		w.Str(t.agentModel);
		w.Pod(uint32_t(t.weaponSwaps.size()));
		for (const WeaponSwap &x : t.weaponSwaps) {
			w.Str(x.from);
			w.Str(x.to);
		}
	}
	w.Pod(uint32_t(s.objects.size()));
	for (const SceneObject &o : s.objects) {
		w.Str(o.name);
		w.Str(o.model);
		w.Raw(o.pos, sizeof(o.pos));
		w.Raw(o.ang, sizeof(o.ang));
		w.Pod(o.scale);
		w.Pod(uint8_t(o.solid));
	}
	return std::move(w.bytes);
}

// False on anything but a complete scene this version wrote; `out` is then left as it was.
inline bool ParseScene(const uint8_t *data, size_t size, Scene &out) {
	ByteReader r(data, size);
	if (r.Pod<uint32_t>() != kSceneMagic || r.Pod<uint32_t>() != kSceneVersion) return false;
	Scene s;
	s.map = r.Str();
	s.tickRate = r.Pod<float>();
	const uint32_t trackCount = r.Pod<uint32_t>();
	if (!r.ok || trackCount > 256) return false;
	for (uint32_t i = 0; i < trackCount && r.ok; ++i) {
		Track t;
		t.name = r.Str();
		t.team = r.Pod<uint8_t>();
		t.enabled = r.Pod<uint8_t>() != 0;
		t.holdFire = r.Pod<uint8_t>() != 0;
		t.delayTicks = r.Pod<int32_t>();
		t.health = r.Pod<int32_t>();
		t.armor = r.Pod<int32_t>();
		t.helmet = r.Pod<uint8_t>();
		const uint32_t names = r.Pod<uint32_t>();
		if (!r.ok || names > r.Remaining()) return false;
		for (uint32_t n = 0; n < names && r.ok; ++n) t.names.push_back(r.Str());
		const uint32_t startWeapons = r.Pod<uint32_t>();
		if (!r.ok || startWeapons > r.Remaining()) return false;
		for (uint32_t n = 0; n < startWeapons && r.ok; ++n) t.startWeapons.push_back(r.Pod<uint16_t>());
		const uint32_t inventory = r.Pod<uint32_t>();
		if (!r.ok || inventory > r.Remaining()) return false;
		for (uint32_t n = 0; n < inventory && r.ok; ++n) {
			InventoryEvent e;
			e.tick = r.Pod<uint32_t>();
			e.name = r.Pod<uint16_t>();
			e.added = r.Pod<uint8_t>();
			t.inventory.push_back(e);
		}
		const uint32_t ticks = r.Pod<uint32_t>();
		if (!r.ok || ticks > r.Remaining() / sizeof(TickSample)) return false;
		t.ticks.resize(ticks);
		if (ticks && !r.Raw(t.ticks.data(), ticks * sizeof(TickSample))) return false;
		const uint32_t cmds = r.Pod<uint32_t>();
		if (!r.ok || cmds > r.Remaining()) return false;
		t.cmds.resize(cmds);
		if (cmds && !r.Raw(t.cmds.data(), cmds)) return false;
		for (const TickSample &tick : t.ticks)
			if (uint64_t(tick.cmdOffset) + tick.cmdSize > cmds) return false;
		t.agentModel = r.Str();
		const uint32_t swaps = r.Pod<uint32_t>();
		if (!r.ok || swaps > r.Remaining()) return false;
		for (uint32_t n = 0; n < swaps && r.ok; ++n) {
			WeaponSwap x;
			x.from = r.Str();
			x.to = r.Str();
			t.weaponSwaps.push_back(x);
		}
		s.tracks.push_back(std::move(t));
	}
	const uint32_t objects = r.Pod<uint32_t>();
	if (!r.ok || objects > r.Remaining()) return false;
	for (uint32_t i = 0; i < objects && r.ok; ++i) {
		SceneObject o;
		o.name = r.Str();
		o.model = r.Str();
		r.Raw(o.pos, sizeof(o.pos));
		r.Raw(o.ang, sizeof(o.ang));
		o.scale = r.Pod<float>();
		o.solid = r.Pod<uint8_t>() != 0;
		s.objects.push_back(o);
	}
	if (!r.ok || r.Remaining() != 0) return false;
	out = std::move(s);
	return true;
}

} // namespace Filmmaker::Takes
