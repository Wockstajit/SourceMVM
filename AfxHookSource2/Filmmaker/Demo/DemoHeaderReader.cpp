#include "DemoHeaderReader.h"

#include "../Platform/ProtobufWire.h"

#include <fstream>
#include <filesystem>
#include <vector>
#include <cstring>

namespace Filmmaker {

namespace {

constexpr unsigned char k_DemoMagic[8] = { 'P','B','D','E','M','S','2','\0' };

// Demo packet command ids (low 6 bits; bit 0x40 = compressed).
constexpr uint32_t k_DemCompressedFlag = 0x40;
constexpr uint32_t k_DemFileHeader = 1;
constexpr uint32_t k_DemFileInfo = 2;

// Reads (cmd, tick, size) framing varints from a reader and returns the payload
// view. Returns false on error/short buffer.
bool ReadPacketFraming(ProtobufReader& r, uint32_t& cmd, const uint8_t*& payload, size_t& payloadLen) {
	cmd = (uint32_t)r.ReadVarint();
	r.ReadVarint(); // tick (unused)
	const uint64_t size = r.ReadVarint();
	if (r.HasError())
		return false;
	if (size > r.Remaining())
		return false;
	payload = r.Cursor();
	payloadLen = (size_t)size;
	return true;
}

// Extracts map_name (5), network_protocol (2) and build_num (13) from a CDemoFileHeader
// payload. One pass: the protocol/build are what DemoRepair classifies on, and pulling them
// here keeps that classification free -- this packet is read for the map regardless.
void ParseFileHeader(const uint8_t* data, size_t len, std::string& outMap,
		int& outProtocol, int& outBuild) {
	ProtobufReader r(data, len);
	uint32_t field; PbWire wire;
	while (r.ReadTag(field, wire)) {
		if (field == 5 && wire == PbWire::LengthDelimited) {
			const uint8_t* p; size_t l;
			if (r.ReadBytes(p, l)) { outMap.assign((const char*)p, l); continue; }
			return;
		}
		if (field == 2 && wire == PbWire::Varint) { outProtocol = (int)r.ReadVarint(); continue; }
		if (field == 13 && wire == PbWire::Varint) { outBuild = (int)r.ReadVarint(); continue; }
		r.SkipField(wire);
	}
}

// Extracts playback_time (field 1, float) and playback_ticks (field 2, varint)
// from a CDemoFileInfo payload.
void ParseFileInfo(const uint8_t* data, size_t len, int& outDurationSeconds, int& outPlaybackTicks) {
	outDurationSeconds = 0;
	outPlaybackTicks = 0;
	ProtobufReader r(data, len);
	uint32_t field; PbWire wire;
	while (r.ReadTag(field, wire)) {
		if (field == 1 && wire == PbWire::Fixed32) {
			uint32_t bits = r.ReadFixed32();
			float seconds;
			std::memcpy(&seconds, &bits, sizeof(seconds));
			if (seconds >= 0.0f && seconds <= 1.0e7f)
				outDurationSeconds = (int)seconds;
			continue;
		}
		if (field == 2 && wire == PbWire::Varint) {
			uint64_t ticks = r.ReadVarint();
			if (ticks <= 0x7fffffffull)
				outPlaybackTicks = (int)ticks;
			continue;
		}
		r.SkipField(wire);
	}
}

} // namespace

DemoHeaderInfo ReadDemoHeader(const std::wstring& path) {
	DemoHeaderInfo info;

	std::error_code ec;
	std::ifstream f(std::filesystem::path(path), std::ios::binary);
	if (!f.is_open())
		return info;

	// Read the fixed 16-byte header (magic + two int32 offsets).
	unsigned char head[16];
	f.read((char*)head, sizeof(head));
	if (f.gcount() != (std::streamsize)sizeof(head))
		return info;
	if (0 != std::memcmp(head, k_DemoMagic, sizeof(k_DemoMagic)))
		return info;

	int32_t fileInfoOffset = 0;
	std::memcpy(&fileInfoOffset, head + 8, sizeof(fileInfoOffset));

	// The DEM_FileHeader is the first packet and is small + uncompressed; read a
	// generous chunk to cover its framing + payload.
	std::vector<uint8_t> buf(64 * 1024);
	f.read((char*)buf.data(), (std::streamsize)buf.size());
	const size_t got = (size_t)f.gcount();
	if (got > 0) {
		ProtobufReader r(buf.data(), got);
		uint32_t cmd; const uint8_t* payload; size_t payloadLen;
		if (ReadPacketFraming(r, cmd, payload, payloadLen)
			&& (cmd & ~k_DemCompressedFlag) == k_DemFileHeader
			&& 0 == (cmd & k_DemCompressedFlag)) {
			ParseFileHeader(payload, payloadLen, info.map, info.networkProtocol, info.buildNum);
		}
	}

	// Duration lives in the DEM_FileInfo packet at the offset from the header.
	if (fileInfoOffset > 16) {
		f.clear();
		f.seekg((std::streamoff)fileInfoOffset, std::ios::beg);
		unsigned char infoBuf[512];
		f.read((char*)infoBuf, sizeof(infoBuf));
		const size_t infoGot = (size_t)f.gcount();
		if (infoGot > 0) {
			ProtobufReader r(infoBuf, infoGot);
			uint32_t cmd; const uint8_t* payload; size_t payloadLen;
			if (ReadPacketFraming(r, cmd, payload, payloadLen)
				&& (cmd & ~k_DemCompressedFlag) == k_DemFileInfo
				&& 0 == (cmd & k_DemCompressedFlag)) {
				ParseFileInfo(payload, payloadLen, info.durationSeconds, info.playbackTicks);
			}
		}
	}

	info.ok = true;
	return info;
}

} // namespace Filmmaker
