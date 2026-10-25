#include "DemoWorldSwap.h"

#include "../Platform/SnappyDecode.h"

#include <cstring>
#include <fstream>
#include <istream>
#include <ostream>
#include <system_error>
#include <vector>

namespace Filmmaker {

namespace {

constexpr char kMagic[8] = { 'P', 'B', 'D', 'E', 'M', 'S', '2', '\0' };
constexpr uint64_t kCompressed = 64;
enum : uint64_t { kFileHeader = 1, kPacket = 7, kSignonPacket = 8, kFullPacket = 13, kSpawnGroups = 15 };
enum : uint32_t { kMsgSpawnGroupLoad = 8, kMsgServerInfo = 40, kMsgClearAllStringTables = 51 };
bool Rewritten(uint32_t type) {
	return type == kMsgSpawnGroupLoad || type == kMsgServerInfo || type == kMsgClearAllStringTables;
}

// ---- protobuf: a message as its raw fields, so untouched fields keep their exact bytes -----------

struct Field {
	uint32_t number = 0, wire = 0;
	size_t begin = 0, end = 0;          // the whole field (tag included) in the source
	size_t valueBegin = 0, valueEnd = 0; // a length-delimited field's bytes
};

bool ReadVarint(const uint8_t* p, size_t size, size_t& i, uint64_t& v) {
	v = 0;
	for (int shift = 0; shift < 64; shift += 7) {
		if (i >= size) return false;
		const uint8_t c = p[i++];
		v |= uint64_t(c & 0x7f) << shift;
		if (!(c & 0x80)) return true;
	}
	return false;
}

bool ParseFields(const uint8_t* p, size_t size, std::vector<Field>& out) {
	out.clear();
	size_t i = 0;
	while (i < size) {
		Field f;
		f.begin = i;
		uint64_t tag = 0;
		if (!ReadVarint(p, size, i, tag)) return false;
		f.number = uint32_t(tag >> 3);
		f.wire = uint32_t(tag & 7);
		uint64_t v = 0;
		switch (f.wire) {
		case 0: if (!ReadVarint(p, size, i, v)) return false; break;
		case 1: if (size - i < 8) return false; i += 8; break;
		case 5: if (size - i < 4) return false; i += 4; break;
		case 2:
			if (!ReadVarint(p, size, i, v) || v > size - i) return false;
			f.valueBegin = i;
			f.valueEnd = i + size_t(v);
			i += size_t(v);
			break;
		default: return false;
		}
		if (f.number == 0) return false;
		f.end = i;
		out.push_back(f);
	}
	return true;
}

void PutVarint(std::string& s, uint64_t v) {
	do {
		uint8_t b = uint8_t(v & 0x7f);
		v >>= 7;
		if (v) b |= 0x80;
		s.push_back(char(b));
	} while (v);
}
std::string BytesField(uint32_t number, const std::string& value) {
	std::string s;
	PutVarint(s, (uint64_t(number) << 3) | 2);
	PutVarint(s, value.size());
	return s + value;
}
std::string FloatField(uint32_t number, float value) {
	std::string s;
	PutVarint(s, (uint64_t(number) << 3) | 5);
	char b[4];
	std::memcpy(b, &value, 4);
	return s.append(b, 4);
}
std::string Vector3(double x, double y, double z) {
	return FloatField(1, float(x)) + FloatField(2, float(y)) + FloatField(3, float(z));
}

// Edits to one message: field number -> its new encoded field ("" drops it).
struct Edit {
	uint32_t number;
	std::string encoded;
};
// Re-encodes `src` with each edited field replacing its first occurrence (or appended when
// absent); every other field is copied as it was.
std::string ApplyEdits(const uint8_t* src, const std::vector<Field>& fields, const std::vector<Edit>& edits) {
	std::string out;
	std::vector<bool> placed(edits.size(), false);
	for (const Field& f : fields) {
		bool edited = false;
		for (size_t k = 0; k < edits.size(); ++k)
			if (edits[k].number == f.number) {
				if (!placed[k]) out += edits[k].encoded, placed[k] = true;
				edited = true;
			}
		if (!edited) out.append(reinterpret_cast<const char*>(src + f.begin), f.end - f.begin);
	}
	for (size_t k = 0; k < edits.size(); ++k)
		if (!placed[k]) out += edits[k].encoded;
	return out;
}

const Field* Find(const std::vector<Field>& fields, uint32_t number) {
	for (const Field& f : fields)
		if (f.number == number) return &f;
	return nullptr;
}
std::string StringOf(const uint8_t* src, const Field* f) {
	return f && f->wire == 2 ? std::string(reinterpret_cast<const char*>(src + f->valueBegin), f->valueEnd - f->valueBegin) : std::string();
}
uint64_t VarintOf(const uint8_t* src, const Field* f) {
	if (!f || f->wire != 0) return 0;
	size_t i = f->begin;
	uint64_t tag = 0, v = 0;
	ReadVarint(src, f->end, i, tag);
	ReadVarint(src, f->end, i, v);
	return v;
}

// ---- the netmessage bit stream: ubitvar type, varuint32 size, payload, LSB first ----------------

struct BitReader {
	const uint8_t* p;
	size_t bits, pos = 0;
	BitReader(const uint8_t* data, size_t size) : p(data), bits(size * 8) {}
	size_t Left() const { return bits - pos; }
	uint32_t Read(int n) {
		uint32_t v = 0;
		for (int k = 0; k < n; ++k, ++pos) v |= uint32_t((p[pos >> 3] >> (pos & 7)) & 1) << k;
		return v;
	}
	bool UBitVar(uint32_t& v) {
		if (Left() < 6) return false;
		v = Read(6);
		const int ext = (v & 0x30) == 0x10 ? 4 : (v & 0x30) == 0x20 ? 8 : (v & 0x30) == 0x30 ? 28 : 0;
		if (!ext) return true;
		if (Left() < size_t(ext)) return false;
		v = (v & 15) | (Read(ext) << 4);
		return true;
	}
	bool VarUInt32(uint32_t& v) {
		v = 0;
		for (int k = 0; k < 5; ++k) {
			if (Left() < 8) return false;
			const uint32_t b = Read(8);
			v |= (b & 0x7f) << (7 * k);
			if (!(b & 0x80)) return true;
		}
		return false;
	}
};

struct BitWriter {
	std::vector<uint8_t> out;
	size_t pos = 0;
	void Write(uint32_t v, int n) {
		for (int k = 0; k < n; ++k, ++pos) {
			if ((pos >> 3) >= out.size()) out.push_back(0);
			if ((v >> k) & 1) out[pos >> 3] |= uint8_t(1u << (pos & 7));
		}
	}
	void UBitVar(uint32_t v) {
		if (v < 16) Write(v, 6);
		else if (v < 256) Write((v & 15) | 16, 6), Write(v >> 4, 4);
		else if (v < 4096) Write((v & 15) | 32, 6), Write(v >> 4, 8);
		else Write((v & 15) | 48, 6), Write(v >> 4, 28);
	}
	void VarUInt32(uint32_t v) {
		do {
			uint32_t b = v & 0x7f;
			v >>= 7;
			if (v) b |= 0x80;
			Write(b, 8);
		} while (v);
	}
	void Copy(const uint8_t* src, size_t from, size_t count) {
		for (size_t k = 0; k < count; ++k) Write((src[(from + k) >> 3] >> ((from + k) & 7)) & 1, 1);
	}
};

struct Swapper {
	const DemoWorldSwap& spec;
	DemoWorldSwapResult& result;
	uint64_t mainHandle = 0;

	// A rewritten SpawnGroup_Load / ServerInfo payload, or "" to keep the message as it was.
	std::string RewriteMessage(uint32_t type, const uint8_t* body, size_t size) {
		std::vector<Field> fields;
		if (!ParseFields(body, size, fields)) return std::string();
		if (type == kMsgServerInfo) {
			++result.serverInfos;
			std::vector<Edit> edits = { { 15, BytesField(15, spec.map) }, { 18, BytesField(18, spec.addon) } };
			// game_session_config (19) carries the map again as s1_mapname (11).
			const Field* session = Find(fields, 19);
			std::vector<Field> inner;
			if (session && session->wire == 2 &&
				ParseFields(body + session->valueBegin, session->valueEnd - session->valueBegin, inner) && Find(inner, 11)) {
				const std::string config = ApplyEdits(body + session->valueBegin, inner, { { 11, BytesField(11, spec.map) } });
				edits.push_back({ 19, BytesField(19, config) });
			}
			return ApplyEdits(body, fields, edits);
		}
		if (type == kMsgClearAllStringTables) {
			// CSVCMsg_ClearAllStringTables.mapname (1): the level name the client reports.
			if (StringOf(body, Find(fields, 1)).empty()) return std::string();
			++result.levelNames;
			return ApplyEdits(body, fields, { { 1, BytesField(1, spec.map) } });
		}
		const std::string world = StringOf(body, Find(fields, 1));
		const uint64_t handle = VarintOf(body, Find(fields, 4));
		const uint64_t parent = VarintOf(body, Find(fields, 18));
		const std::string group = StringOf(body, Find(fields, 20));
		const auto& o = spec.offset;
		const std::vector<Edit> place = { { 6, BytesField(6, Vector3(o.pos.x, o.pos.y, o.pos.z)) }, { 7, BytesField(7, Vector3(0, o.yaw, 0)) } };
		if (parent == 0 && !world.empty() && world == spec.sourceMap) {
			mainHandle = handle;
			++result.worlds;
			std::vector<Edit> edits = place;
			edits.insert(edits.begin(), { 1, BytesField(1, spec.map) });
			return ApplyEdits(body, fields, edits);
		}
		if (group.rfind("skyboxWorldGroup", 0) == 0 && (mainHandle == 0 || parent == mainHandle)) {
			++result.skyboxes;
			std::vector<Edit> edits = place;
			if (!spec.skybox.empty()) edits.insert(edits.begin(), { 1, BytesField(1, spec.skybox) });
			return ApplyEdits(body, fields, edits);
		}
		return std::string();
	}

	// Whether a packed message stream holds a message this rewrites: headers only, so the
	// packets that make up nearly all of a demo are passed over without copying a bit.
	static bool Mentions(const uint8_t* data, size_t size) {
		BitReader r(data, size);
		while (r.Left() >= 14) {
			uint32_t type = 0, length = 0;
			if (!r.UBitVar(type) || !r.VarUInt32(length) || r.Left() < size_t(length) * 8) return false;
			if (Rewritten(type)) return true;
			r.pos += size_t(length) * 8;
		}
		return false;
	}

	// Rewrites a packed message stream; false when nothing in it changed.
	bool RewriteStream(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
		if (!Mentions(data, size)) return false;
		BitReader r(data, size);
		BitWriter w;
		bool changed = false;
		while (r.Left() >= 14) {
			const size_t start = r.pos;
			uint32_t type = 0, length = 0;
			if (!r.UBitVar(type) || !r.VarUInt32(length) || r.Left() < size_t(length) * 8) {
				r.pos = start;
				break;
			}
			const size_t bodyBit = r.pos;
			r.pos += size_t(length) * 8;
			if (Rewritten(type)) {
				std::vector<uint8_t> body(length);
				for (size_t k = 0; k < length; ++k) {
					BitReader b(data, size);
					b.pos = bodyBit + k * 8;
					body[k] = uint8_t(b.Read(8));
				}
				const std::string next = RewriteMessage(type, body.data(), body.size());
				if (!next.empty()) {
					w.UBitVar(type);
					w.VarUInt32(uint32_t(next.size()));
					for (unsigned char c : next) w.Write(c, 8);
					changed = true;
					continue;
				}
			}
			w.Copy(data, start, r.pos - start);
		}
		if (!changed) return false;
		w.Copy(data, r.pos, r.Left()); // trailing padding
		out = std::move(w.out);
		return true;
	}

	// One frame's (decompressed) payload; false when it stays as it was.
	bool RewriteFrame(uint64_t base, const uint8_t* p, size_t size, std::string& out) {
		std::vector<Field> fields;
		if (!ParseFields(p, size, fields)) return false;
		if (base == kFileHeader) {
			result.headerMap = StringOf(p, Find(fields, 5));
			++result.headers;
			out = ApplyEdits(p, fields, { { 5, BytesField(5, spec.map) }, { 10, BytesField(10, spec.addon) } });
			return true;
		}
		if (base == kPacket || base == kSignonPacket) {
			const Field* data = Find(fields, 3);
			std::vector<uint8_t> stream;
			if (!data || data->wire != 2 || !RewriteStream(p + data->valueBegin, data->valueEnd - data->valueBegin, stream)) return false;
			out = ApplyEdits(p, fields, { { 3, BytesField(3, std::string(stream.begin(), stream.end())) } });
			return true;
		}
		if (base == kFullPacket) {
			const Field* packet = Find(fields, 2);
			std::string inner;
			if (!packet || packet->wire != 2 || !RewriteFrame(kPacket, p + packet->valueBegin, packet->valueEnd - packet->valueBegin, inner)) return false;
			out = ApplyEdits(p, fields, { { 2, BytesField(2, inner) } });
			return true;
		}
		if (base == kSpawnGroups) {
			// CDemoSpawnGroups.msgs (3): each entry is its own packed message stream.
			std::string rebuilt;
			bool changed = false;
			for (const Field& f : fields) {
				std::vector<uint8_t> stream;
				if (f.number == 3 && f.wire == 2 && RewriteStream(p + f.valueBegin, f.valueEnd - f.valueBegin, stream)) {
					rebuilt += BytesField(3, std::string(stream.begin(), stream.end()));
					changed = true;
				} else {
					rebuilt.append(reinterpret_cast<const char*>(p + f.begin), f.end - f.begin);
				}
			}
			if (changed) out = std::move(rebuilt);
			return changed;
		}
		return false;
	}
};

bool ReadStreamVarint(std::istream& in, uint64_t& v, uint64_t& consumed) {
	v = 0;
	for (int shift = 0; shift < 64; shift += 7) {
		const int c = in.get();
		if (c == EOF) return false;
		++consumed;
		v |= uint64_t(c & 0x7f) << shift;
		if (!(c & 0x80)) return true;
	}
	return false;
}

void WriteStreamVarint(std::ostream& out, uint64_t v, uint64_t& written) {
	std::string s;
	PutVarint(s, v);
	out.write(s.data(), std::streamsize(s.size()));
	written += s.size();
}

} // namespace

DemoWorldSwapResult SwapDemoWorld(std::istream& in, std::ostream& out, const DemoWorldSwap& spec,
		const std::atomic<bool>* cancel, const std::function<void(double)>& progress) {
	DemoWorldSwapResult result;
	if (spec.map.empty() || spec.sourceMap.empty()) {
		result.error = "no map to swap";
		return result;
	}
	in.seekg(0, std::ios::end);
	const uint64_t total = uint64_t(in.tellg());
	in.seekg(0, std::ios::beg);
	char header[16];
	if (!in.read(header, 16) || std::memcmp(header, kMagic, 8) != 0) {
		result.error = "not a CS2 demo (PBDEMS2)";
		return result;
	}
	int32_t fileInfo = 0, spawnGroups = 0;
	std::memcpy(&fileInfo, header + 8, 4);
	std::memcpy(&spawnGroups, header + 12, 4);
	int32_t newFileInfo = fileInfo, newSpawnGroups = spawnGroups;
	out.write(header, 16);
	uint64_t inPos = 16, outPos = 16, lastReport = 0;

	Swapper swapper{ spec, result };
	std::vector<uint8_t> payload, plain;
	std::string rewritten;
	while (true) {
		if (cancel && cancel->load()) {
			result.error = "cancelled";
			return result;
		}
		const uint64_t frameStart = inPos;
		uint64_t cmd = 0, tick = 0, size = 0, framing = 0;
		if (!ReadStreamVarint(in, cmd, framing)) break; // clean end of file
		if (!ReadStreamVarint(in, tick, framing) || !ReadStreamVarint(in, size, framing) || size > total - inPos - framing) {
			result.error = "truncated frame at byte " + std::to_string(frameStart);
			return result;
		}
		inPos += framing;
		payload.resize(size_t(size));
		if (size && !in.read(reinterpret_cast<char*>(payload.data()), std::streamsize(size))) {
			result.error = "short read at byte " + std::to_string(frameStart);
			return result;
		}
		inPos += size;
		++result.frames;
		if (int64_t(frameStart) == fileInfo) newFileInfo = int32_t(outPos);
		if (int64_t(frameStart) == spawnGroups) newSpawnGroups = int32_t(outPos);

		const uint64_t base = cmd & ~kCompressed;
		bool replaced = false;
		if (base == kFileHeader || base == kPacket || base == kSignonPacket || base == kFullPacket || base == kSpawnGroups) {
			const uint8_t* p = payload.data();
			size_t n = payload.size();
			bool readable = true;
			if (cmd & kCompressed) {
				readable = SnappyDecode(payload.data(), payload.size(), plain);
				p = plain.data();
				n = plain.size();
			}
			if (readable && swapper.RewriteFrame(base, p, n, rewritten)) {
				WriteStreamVarint(out, base, outPos);
				WriteStreamVarint(out, tick, outPos);
				WriteStreamVarint(out, rewritten.size(), outPos);
				out.write(rewritten.data(), std::streamsize(rewritten.size()));
				outPos += rewritten.size();
				replaced = true;
			}
		}
		if (!replaced) {
			WriteStreamVarint(out, cmd, outPos);
			WriteStreamVarint(out, tick, outPos);
			WriteStreamVarint(out, size, outPos);
			if (size) out.write(reinterpret_cast<const char*>(payload.data()), std::streamsize(size));
			outPos += size;
		}
		if (!out) {
			result.error = "write failed";
			return result;
		}
		if (progress && inPos - lastReport >= (8u << 20)) {
			lastReport = inPos;
			progress(total ? double(inPos) / double(total) : 0.0);
		}
	}
	if (result.headers == 0 || result.worlds == 0) {
		result.error = result.headers == 0 ? "the demo has no file header"
			: "the demo never loads the world \"" + spec.sourceMap + "\"";
		return result;
	}
	if (outPos > uint64_t(INT32_MAX)) {
		result.error = "the copy is larger than a demo can be";
		return result;
	}
	out.seekp(8);
	out.write(reinterpret_cast<const char*>(&newFileInfo), 4);
	out.write(reinterpret_cast<const char*>(&newSpawnGroups), 4);
	out.flush();
	if (!out) {
		result.error = "write failed";
		return result;
	}
	result.bytesIn = inPos;
	result.bytesOut = outPos;
	result.ok = true;
	if (progress) progress(1.0);
	return result;
}

DemoWorldSwapResult SwapDemoWorldFile(const std::filesystem::path& inPath, const std::filesystem::path& outPath,
		const DemoWorldSwap& spec, const std::atomic<bool>* cancel, const std::function<void(double)>& progress) {
	DemoWorldSwapResult result;
	std::filesystem::path part = outPath;
	part += L".part";
	{
		std::vector<char> inBuffer(1 << 20), outBuffer(1 << 20);
		std::ifstream in;
		in.rdbuf()->pubsetbuf(inBuffer.data(), std::streamsize(inBuffer.size()));
		in.open(inPath, std::ios::binary);
		if (!in) {
			result.error = "cannot open the demo";
			return result;
		}
		std::ofstream out;
		out.rdbuf()->pubsetbuf(outBuffer.data(), std::streamsize(outBuffer.size()));
		out.open(part, std::ios::binary | std::ios::trunc);
		if (!out) {
			result.error = "cannot write the copy";
			return result;
		}
		result = SwapDemoWorld(in, out, spec, cancel, progress);
	}
	std::error_code ec;
	if (!result.ok) {
		std::filesystem::remove(part, ec);
		return result;
	}
	// The slot's previous copy goes first: the slot written is never the one playing, and a rename
	// that has to replace an existing file is not something every Windows library does.
	std::filesystem::remove(outPath, ec);
	std::filesystem::rename(part, outPath, ec);
	if (ec) {
		std::filesystem::remove(part, ec);
		result.ok = false;
		result.error = "cannot replace the previous copy (is it playing?)";
	}
	return result;
}

} // namespace Filmmaker
