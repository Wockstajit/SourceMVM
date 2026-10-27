#include "SourceDemoFile.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace SourceFilmmaker {
namespace DemoFile {

namespace {

// demofile.h: the header is kHeaderBytes, every frame starts with a command byte and a tick.
const size_t kPathLength = 260;
// democmdinfo_t with one split screen slot: flags, then origin, angles and local angles twice.
const size_t kCommandInfoSize = 76;

enum Command { kSignon = 1, kPacket = 2, kSyncTick = 3, kConsoleCmd = 4, kUserCmd = 5, kDataTables = 6, kStop = 7, kStringTables = 8 };

// Network message numbers and field widths of protocol 24 (netmessages.h, const.h).
const int kMessageTypeBits = 6;
const int kEventIndexBits = 9;   // MAX_EVENT_BITS
const int kEdictBits = 11;       // MAX_EDICT_BITS
const int kModelIndexBits = 11;  // SP_MODEL_INDEX_BITS
const int kDecalIndexBits = 9;   // MAX_DECAL_INDEX_BITS
const int kSoundIndexBits = 14;  // MAX_SOUND_INDEX_BITS
const int kCoordIntegerBits = 14;
const int kCoordFractionBits = 5;

// Little-endian bit reader over one packet. Reads past the end set overrun and return zeros, so the
// caller checks once per message instead of on every read.
class BitReader {
public:
	BitReader(const unsigned char* data, size_t bytes) : m_data(data), m_bits(bytes * 8) {}

	size_t Left() const { return m_pos < m_bits ? m_bits - m_pos : 0; }
	bool Overrun() const { return m_overrun; }

	uint32_t Bits(int count) {
		uint32_t value = 0;
		if (count <= 0) return 0;
		if (m_pos + (size_t)count > m_bits) {
			m_overrun = true;
			m_pos = m_bits;
			return 0;
		}
		for (int i = 0; i < count; ++i, ++m_pos)
			value |= (uint32_t)((m_data[m_pos >> 3] >> (m_pos & 7)) & 1) << i;
		return value;
	}
	int32_t Signed(int count) {
		const uint32_t raw = Bits(count);
		if (count < 32 && (raw & (1u << (count - 1)))) return (int32_t)(raw | (~0u << count));
		return (int32_t)raw;
	}
	bool Bit() { return Bits(1) != 0; }
	float Float() {
		const uint32_t raw = Bits(32);
		float value;
		std::memcpy(&value, &raw, sizeof(value));
		return value;
	}
	std::string String() {
		std::string out;
		while (!m_overrun) {
			const char c = (char)Bits(8);
			if (!c) break;
			out.push_back(c);
		}
		return out;
	}
	uint32_t VarInt() {
		uint32_t result = 0;
		for (int i = 0; i < 5; ++i) {
			const uint32_t b = Bits(8);
			result |= (b & 0x7F) << (7 * i);
			if (!(b & 0x80)) break;
		}
		return result;
	}
	void Skip(size_t count) {
		if (m_pos + count > m_bits) {
			m_overrun = true;
			m_pos = m_bits;
			return;
		}
		m_pos += count;
	}
	// One component of bf_read::ReadBitVec3Coord.
	void SkipCoord() {
		const bool integer = Bit(), fraction = Bit();
		if (!integer && !fraction) return;
		Bit(); // sign
		if (integer) Bits(kCoordIntegerBits);
		if (fraction) Bits(kCoordFractionBits);
	}
	size_t Position() const { return m_pos; }

private:
	const unsigned char* m_data;
	size_t m_bits;
	size_t m_pos = 0;
	bool m_overrun = false;
};

int Log2(unsigned value) {
	int result = 0;
	while (value >>= 1) ++result;
	return result;
}

int32_t ReadInt32(const unsigned char* p) {
	int32_t value;
	std::memcpy(&value, p, sizeof(value));
	return value;
}

std::string ReadPath(const unsigned char* p) {
	size_t n = 0;
	while (n < kPathLength && p[n]) ++n;
	return std::string((const char*)p, n);
}

struct Descriptor {
	std::string name;
	std::vector<std::pair<int, std::string>> keys; // type, name
	bool wanted = false;
};

class Scanner {
public:
	Scanner(Scan& scan, const std::vector<std::string>& wanted) : m_scan(scan), m_wanted(wanted) {}

	bool Packet(const unsigned char* data, size_t bytes, int tick) {
		BitReader b(data, bytes);
		while (b.Left() >= (size_t)kMessageTypeBits) {
			const int type = (int)b.Bits(kMessageTypeBits);
			if (!Message(b, type, tick)) return false;
			if (b.Overrun()) return Fail("message %d runs past the end of its packet at tick %d", type, tick);
			++m_scan.messages;
		}
		return true;
	}

	bool Fail(const char* format, int a, int b) {
		char text[160];
		std::snprintf(text, sizeof(text), format, a, b);
		m_scan.error = text;
		return false;
	}

private:
	bool Message(BitReader& b, int type, int tick) {
		switch (type) {
		case 0: return true;                                   // net_NOP
		case 1: b.String(); return true;                       // net_Disconnect
		case 2: b.Bits(32); b.String(); b.Bit(); return true;  // net_File
		case 3: b.Bits(32); b.Bits(16); b.Bits(16); return true; // net_Tick
		case 4: b.String(); return true;                       // net_StringCmd
		case 5: {                                              // net_SetConVar
			const int count = (int)b.Bits(8);
			for (int i = 0; i < count && !b.Overrun(); ++i) { b.String(); b.String(); }
			return true;
		}
		case 6: b.Bits(8); b.Bits(32); return true;            // net_SignonState
		case 7: b.String(); return true;                       // svc_Print
		case 8:                                                // svc_ServerInfo
			b.Bits(16); b.Bits(32); b.Bit(); b.Bit(); b.Bits(32); b.Bits(16);
			b.Skip(128);                                       // map MD5
			b.Bits(8); b.Bits(8); b.Bits(32); b.Bits(8);
			b.String(); b.String(); b.String(); b.String();
			b.Bit();                                           // replay
			return true;
		case 9: b.Bit(); b.Skip(b.Bits(16)); return true;      // svc_SendTable
		case 10: {                                             // svc_ClassInfo
			const unsigned count = b.Bits(16);
			if (!b.Bit()) {
				const int bits = Log2(count) + 1;
				for (unsigned i = 0; i < count && !b.Overrun(); ++i) { b.Bits(bits); b.String(); b.String(); }
			}
			return true;
		}
		case 11: b.Bit(); return true;                         // svc_SetPause
		case 12: {                                             // svc_CreateStringTable
			b.String();
			const unsigned maxEntries = b.Bits(16);
			b.Bits(Log2(maxEntries) + 1);
			const uint32_t length = b.VarInt();
			if (b.Bit()) { b.Bits(12); b.Bits(4); }
			b.Bit();                                           // compressed
			b.Skip(length);
			return true;
		}
		case 13:                                               // svc_UpdateStringTable
			b.Bits(5);
			if (b.Bit()) b.Bits(16);
			b.Skip(b.Bits(20));
			return true;
		case 14:                                               // svc_VoiceInit
			b.String();
			if (b.Bits(8) == 255) b.Bits(16);
			return true;
		case 15: b.Bits(8); b.Bits(8); b.Skip(b.Bits(16)); return true; // svc_VoiceData
		case 17:                                               // svc_Sounds
			if (b.Bit()) b.Skip(b.Bits(8));
			else { b.Bits(8); b.Skip(b.Bits(16)); }
			return true;
		case 18: b.Bits(kEdictBits); return true;              // svc_SetView
		case 19: b.Bit(); b.Bits(16); b.Bits(16); b.Bits(16); return true; // svc_FixAngle
		case 20: b.Bits(16); b.Bits(16); b.Bits(16); return true;          // svc_CrosshairAngle
		case 21: {                                             // svc_BSPDecal
			const bool x = b.Bit(), y = b.Bit(), z = b.Bit();
			if (x) b.SkipCoord();
			if (y) b.SkipCoord();
			if (z) b.SkipCoord();
			b.Bits(kDecalIndexBits);
			if (b.Bit()) { b.Bits(kEdictBits); b.Bits(kModelIndexBits); }
			b.Bit();
			return true;
		}
		case 23: b.Bits(8); b.Skip(b.Bits(11)); return true;   // svc_UserMessage
		case 24: b.Bits(kEdictBits); b.Bits(9); b.Skip(b.Bits(11)); return true; // svc_EntityMessage
		case 25: return GameEvent(b, tick);                    // svc_GameEvent
		case 26: {                                             // svc_PacketEntities
			b.Bits(kEdictBits);
			if (b.Bit()) b.Bits(32);
			b.Bit();
			b.Bits(kEdictBits);
			const uint32_t length = b.Bits(20);
			b.Bit();
			b.Skip(length);
			return true;
		}
		case 27: b.Bits(8); b.Skip(b.VarInt()); return true;   // svc_TempEntities
		case 28: b.Bits(kSoundIndexBits); return true;         // svc_Prefetch
		case 29: b.Bits(16); b.Skip((size_t)b.Bits(16) * 8); return true; // svc_Menu
		case 30: return EventList(b);                          // svc_GameEventList
		case 31: b.Bits(32); b.String(); return true;          // svc_GetCvarValue
		case 32: b.Skip((size_t)b.Bits(32) * 8); return true;  // svc_CmdKeyValues
		default:
			return Fail("unknown network message %d at tick %d", type, tick);
		}
	}

	bool EventList(BitReader& b) {
		const unsigned count = b.Bits(kEventIndexBits);
		const size_t length = b.Bits(20);
		const size_t end = b.Position() + length;
		for (unsigned i = 0; i < count && !b.Overrun(); ++i) {
			const int id = (int)b.Bits(kEventIndexBits);
			Descriptor descriptor;
			descriptor.name = b.String();
			for (;;) {
				const int keyType = (int)b.Bits(3);
				if (!keyType || b.Overrun()) break;
				descriptor.keys.emplace_back(keyType, b.String());
			}
			descriptor.wanted = m_wanted.empty();
			for (const std::string& name : m_wanted) descriptor.wanted |= name == descriptor.name;
			m_descriptors[id] = std::move(descriptor);
		}
		if (!b.Overrun() && b.Position() != end) return Fail("game event list size mismatch (%d of %d descriptors)", (int)m_descriptors.size(), (int)count);
		return true;
	}

	bool GameEvent(BitReader& b, int tick) {
		const size_t length = b.Bits(11);
		const size_t start = b.Position();
		const int id = (int)b.Bits(kEventIndexBits);
		auto it = m_descriptors.find(id);
		if (it == m_descriptors.end() || !it->second.wanted) {
			b.Skip(length >= (size_t)kEventIndexBits ? length - kEventIndexBits : 0);
			return true;
		}
		Event event;
		event.tick = tick;
		event.name = it->second.name;
		for (const auto& key : it->second.keys) {
			Field field;
			field.key = key.second;
			switch (key.first) {
			case 1: field.type = Field::Type::String; field.text = b.String(); break;
			case 2: field.type = Field::Type::Float; field.number = b.Float(); break;
			case 3: field.value = b.Signed(32); break;
			case 4: field.value = b.Signed(16); break;
			case 5: field.value = (int)b.Bits(8); break;
			case 6: field.type = Field::Type::Bool; field.value = b.Bit() ? 1 : 0; break;
			default: return Fail("game event field type %d at tick %d", key.first, tick);
			}
			event.fields.push_back(std::move(field));
		}
		if (!b.Overrun() && b.Position() - start != length)
			return Fail("game event size mismatch at tick %d (%d bits)", tick, (int)length);
		m_scan.events.push_back(std::move(event));
		return true;
	}

	Scan& m_scan;
	const std::vector<std::string>& m_wanted;
	std::unordered_map<int, Descriptor> m_descriptors;
};

} // namespace

const Field* Event::Find(const char* key) const {
	for (const Field& field : fields)
		if (field.key == key) return &field;
	return nullptr;
}

int Event::Int(const char* key, int fallback) const {
	const Field* field = Find(key);
	return field && field->type != Field::Type::String && field->type != Field::Type::Float ? field->value : fallback;
}

std::string Event::Text(const char* key) const {
	const Field* field = Find(key);
	return field && field->type == Field::Type::String ? field->text : std::string();
}

bool ReadHeader(const unsigned char* data, size_t size, Header& h) {
	if (!data || size < kHeaderBytes || std::memcmp(data, "HL2DEMO\0", 8) != 0) return false;
	h.demoProtocol = ReadInt32(data + 8);
	h.networkProtocol = ReadInt32(data + 12);
	h.serverName = ReadPath(data + 16);
	h.clientName = ReadPath(data + 16 + kPathLength);
	h.mapName = ReadPath(data + 16 + 2 * kPathLength);
	h.gameDirectory = ReadPath(data + 16 + 3 * kPathLength);
	std::memcpy(&h.playbackTime, data + 1056, sizeof(float));
	h.playbackTicks = ReadInt32(data + 1060);
	h.playbackFrames = ReadInt32(data + 1064);
	return true;
}

Scan ScanDemo(const unsigned char* data, size_t size, const std::vector<std::string>& wanted) {
	Scan scan;
	if (!ReadHeader(data, size, scan.header)) {
		scan.error = "not a Source 1 demo (no HL2DEMO header)";
		return scan;
	}
	const Header& h = scan.header;
	if (h.demoProtocol != 3 || h.networkProtocol != 24) {
		char text[96];
		std::snprintf(text, sizeof(text), "demo protocol %d, network protocol %d (only 3 and 24 are read)", h.demoProtocol, h.networkProtocol);
		scan.error = text;
		return scan;
	}

	Scanner scanner(scan, wanted);
	auto stop = [&](const char* why) { scan.error = why; return scan; };
	size_t at = kHeaderBytes;
	auto need = [&](size_t bytes) { return at + bytes <= size; };
	auto block = [&](size_t& length) -> bool {
		if (!need(4)) return false;
		const int32_t n = ReadInt32(data + at);
		at += 4;
		if (n < 0 || !need((size_t)n)) return false;
		length = (size_t)n;
		return true;
	};
	for (;;) {
		if (!need(5)) break; // a demo cut short ends without dem_stop; keep what was read
		const int command = data[at];
		const int tick = ReadInt32(data + at + 1);
		at += 5;
		++scan.frames;
		size_t length = 0;
		switch (command) {
		case kSignon:
		case kPacket:
			if (!need(kCommandInfoSize + 8)) return stop("truncated packet frame");
			at += kCommandInfoSize + 8;
			if (!block(length)) return stop("truncated packet frame");
			if (!scanner.Packet(data + at, length, command == kPacket ? tick : 0)) return scan;
			at += length;
			break;
		case kSyncTick:
			break;
		case kConsoleCmd:
		case kDataTables:
		case kStringTables:
			if (!block(length)) return stop("truncated frame");
			at += length;
			break;
		case kUserCmd:
			if (!need(4)) return stop("truncated user command");
			at += 4;
			if (!block(length)) return stop("truncated user command");
			at += length;
			break;
		case kStop:
			scan.ok = true;
			return scan;
		default:
			scanner.Fail("unknown demo command %d at tick %d", command, tick);
			return scan;
		}
	}
	scan.ok = true;
	return scan;
}

Scan ScanDemoFile(const std::string& path, const std::vector<std::string>& wanted) {
	Scan scan;
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) {
		scan.error = "cannot open " + path;
		return scan;
	}
	std::vector<unsigned char> data;
	unsigned char chunk[1 << 16];
	size_t n;
	while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) data.insert(data.end(), chunk, chunk + n);
	std::fclose(f);
	return ScanDemo(data.data(), data.size(), wanted);
}

} // namespace DemoFile
} // namespace SourceFilmmaker
