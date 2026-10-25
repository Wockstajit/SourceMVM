#pragma once

// The project.mvm container: one binary file per demo project folder that carries every
// authored JSON payload ("project", "campath", "camshake", "loop", "scenekeys", "cosmetics") as a
// tagged, CRC-checked section. Disk IO lives in DemoProjectStore.cpp; this header is
// engine-free and unit-tested by Automation/tests/demo-project-file-math-tests.cpp.
//
// Layout, little-endian:
//   Header, 32 bytes: u8 magic[8] = {'S','R','C','M','V','M',0x1A,0}; u32 version = 1;
//       u32 flags (bits 0-3 = game, every other bit 0); u32 sectionCount (<= 64);
//       u32 reserved0 = 0; u64 reserved1 = 0.
//   Section table: sectionCount entries of 32 bytes: char tag[16] (ASCII, NUL-padded);
//       u32 encoding (1 = UTF-8 JSON, the only valid value); u32 crc32 (zlib CRC-32 of
//       the payload); u32 payloadOffset (from file start); u32 payloadSize (<= 256 MiB).
//   Payloads follow the table, contiguous, in table order. Duplicate tags are invalid.

#include "DemoProjectStoreMath.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Filmmaker {

// The authored payload kinds a demo project folder carries. Deadlock uses the same
// campath/camshake/loop tags as CS2 -- the header game flag is what distinguishes them.
enum class ProjectSection {
	Campath,
	Camshake,
	Loop,
	SceneKeys,
	Cosmetics, // CS2 skin, knife, glove and agent picks per player (Cosmetics/CosmeticProfile.h)
	Environment, // CS2: the other map the demo plays on and its alignment (AltEnvironment/AltEnvironment.h)
};

inline const char* ProjectSectionTag(ProjectSection section) {
	switch (section) {
	case ProjectSection::Campath: return "campath";
	case ProjectSection::Camshake: return "camshake";
	case ProjectSection::Loop: return "loop";
	case ProjectSection::SceneKeys: return "scenekeys";
	case ProjectSection::Cosmetics: return "cosmetics";
	case ProjectSection::Environment: return "environment";
	}
	return "section";
}

constexpr uint8_t kProjectFileMagic[8] = {'S', 'R', 'C', 'M', 'V', 'M', 0x1A, 0};
constexpr uint32_t kProjectFileVersion = 1;
constexpr uint32_t kProjectFileGameCs2 = 1;
constexpr uint32_t kProjectFileGameDeadlock = 2;
constexpr uint32_t kProjectFileMaxSections = 64;
constexpr uint32_t kProjectFileEncodingJson = 1;
constexpr uint32_t kProjectFileMaxPayload = 256u * 1024u * 1024u;
constexpr size_t kProjectFileHeaderBytes = 32;
constexpr size_t kProjectFileEntryBytes = 32;

// The "project" section, present whenever the file is written:
// {"game":"cs2"|"deadlock","demoPath":"<utf8>"}.
constexpr char kProjectFileInfoTag[] = "project";

// The container's file name inside a demo's project folder.
constexpr wchar_t kProjectFileName[] = L"project.mvm";

inline uint32_t ProjectFileGameFlag(ProjectGame game) {
	return game == ProjectGame::Deadlock ? kProjectFileGameDeadlock : kProjectFileGameCs2;
}

// The whole decoded container. Sections stay in file order.
struct ProjectFileSection {
	std::string tag;
	std::string payload;
};

struct ProjectFile {
	ProjectGame game = ProjectGame::Cs2;
	std::vector<ProjectFileSection> sections;
};

inline const ProjectFileSection* FindProjectSection(const ProjectFile& file, const char* tag) {
	for (const ProjectFileSection& section : file.sections)
		if (section.tag == tag) return &section;
	return nullptr;
}

// Replaces a section's payload; an empty payload removes the section.
inline void SetProjectSection(ProjectFile& file, const char* tag, const std::string& payload) {
	for (auto it = file.sections.begin(); it != file.sections.end(); ++it) {
		if (it->tag == tag) {
			if (payload.empty()) file.sections.erase(it);
			else it->payload = payload;
			return;
		}
	}
	if (!payload.empty()) file.sections.push_back({tag, payload});
}

// Standard IEEE/zlib CRC-32.
inline uint32_t ProjectFileCrc32(const void* data, size_t size) {
	static const std::array<uint32_t, 256> table = [] {
		std::array<uint32_t, 256> t{};
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			t[i] = c;
		}
		return t;
	}();
	uint32_t crc = 0xFFFFFFFFu;
	const uint8_t* bytes = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < size; ++i)
		crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

inline uint32_t ReadU32(const uint8_t* at) {
	uint32_t v;
	std::memcpy(&v, at, 4);
	return v;
}

inline uint64_t ReadU64(const uint8_t* at) {
	uint64_t v;
	std::memcpy(&v, at, 8);
	return v;
}

inline void WriteU32(uint8_t* at, uint32_t v) {
	std::memcpy(at, &v, 4);
}

inline void WriteU64(uint8_t* at, uint64_t v) {
	std::memcpy(at, &v, 8);
}

// Reads a NUL-padded tag field. False when it is empty, not printable ASCII, or carries
// non-zero bytes after the terminator (the canonical form EncodeProjectFile writes).
inline bool ReadProjectFileTag(const uint8_t* field, std::string& tag) {
	size_t len = 0;
	while (len < 16 && field[len]) ++len;
	if (len == 0 || len == 16) return false;
	for (size_t i = 0; i < len; ++i)
		if (field[i] < 0x21 || field[i] > 0x7E) return false;
	for (size_t i = len; i < 16; ++i)
		if (field[i] != 0) return false;
	tag.assign(reinterpret_cast<const char*>(field), len);
	return true;
}

// Decodes a project.mvm image. `expectedGame` is enforced against the header's game flag,
// so a Deadlock file is never opened as CS2. False and `error` says why on any violation.
inline bool DecodeProjectFile(const void* data, size_t size, ProjectGame expectedGame,
	ProjectFile& out, std::string& error) {
	const uint8_t* bytes = static_cast<const uint8_t*>(data);
	out = {};
	if (size < kProjectFileHeaderBytes) { error = "truncated header"; return false; }
	if (std::memcmp(bytes, kProjectFileMagic, 8)) { error = "not a project.mvm"; return false; }
	const uint32_t version = ReadU32(bytes + 8);
	if (version != kProjectFileVersion) { error = "unsupported project.mvm version"; return false; }
	const uint32_t flags = ReadU32(bytes + 12);
	const uint32_t count = ReadU32(bytes + 16);
	if (ReadU32(bytes + 20) != 0 || ReadU64(bytes + 24) != 0) {
		error = "reserved header fields not zero"; return false;
	}
	if (flags >> 4) { error = "unknown flag bits set"; return false; }
	const uint32_t game = flags & 0xFu;
	if (game != kProjectFileGameCs2 && game != kProjectFileGameDeadlock) {
		error = "unknown project game"; return false;
	}
	if (game != ProjectFileGameFlag(expectedGame)) { error = "project is for another game"; return false; }
	if (count > kProjectFileMaxSections) { error = "too many sections"; return false; }
	const size_t tableEnd = kProjectFileHeaderBytes + (size_t)count * kProjectFileEntryBytes;
	if (size < tableEnd) { error = "truncated section table"; return false; }

	ProjectFile parsed;
	parsed.game = expectedGame;
	std::map<std::string, bool> seen;
	for (uint32_t i = 0; i < count; ++i) {
		const uint8_t* e = bytes + kProjectFileHeaderBytes + i * kProjectFileEntryBytes;
		std::string tag;
		if (!ReadProjectFileTag(e, tag)) { error = "bad section tag"; return false; }
		if (seen.count(tag)) { error = "duplicate section tag"; return false; }
		seen[tag] = true;
		const uint32_t encoding = ReadU32(e + 16);
		const uint32_t crc = ReadU32(e + 20);
		const uint32_t offset = ReadU32(e + 24);
		const uint32_t payloadSize = ReadU32(e + 28);
		if (encoding != kProjectFileEncodingJson) { error = "unknown section encoding"; return false; }
		if (payloadSize > kProjectFileMaxPayload) { error = "section too large"; return false; }
		if (offset < tableEnd || (size_t)offset > size || (size_t)payloadSize > size - offset) {
			error = "section payload out of bounds"; return false;
		}
		if (payloadSize && ProjectFileCrc32(bytes + offset, payloadSize) != crc) {
			error = "section crc mismatch"; return false;
		}
		parsed.sections.push_back({tag, std::string(reinterpret_cast<const char*>(bytes + offset), payloadSize)});
	}
	// Payloads must not overlap (they are written contiguously, in table order).
	for (size_t i = 0; i < parsed.sections.size(); ++i) {
		const uint8_t* e = bytes + kProjectFileHeaderBytes + i * kProjectFileEntryBytes;
		const uint32_t begin = ReadU32(e + 24), end = begin + ReadU32(e + 28);
		for (size_t j = i + 1; j < parsed.sections.size(); ++j) {
			const uint8_t* f = bytes + kProjectFileHeaderBytes + j * kProjectFileEntryBytes;
			const uint32_t fbegin = ReadU32(f + 24), fend = fbegin + ReadU32(f + 28);
			if (begin < fend && fbegin < end) { error = "overlapping section payloads"; return false; }
		}
	}
	out = std::move(parsed);
	return true;
}

// Encodes a project.mvm image: header, table, then payloads contiguous in section order.
// Empty payloads are dropped (a removed section writes nothing).
inline std::string EncodeProjectFile(const ProjectFile& file) {
	std::vector<const ProjectFileSection*> live;
	for (const ProjectFileSection& s : file.sections)
		if (!s.payload.empty() && s.payload.size() <= kProjectFileMaxPayload) live.push_back(&s);
	const uint32_t count = (uint32_t)live.size();
	const size_t tableEnd = kProjectFileHeaderBytes + (size_t)count * kProjectFileEntryBytes;
	size_t total = tableEnd;
	for (const ProjectFileSection* s : live) total += s->payload.size();

	std::string out(total, '\0');
	uint8_t* bytes = reinterpret_cast<uint8_t*>(&out[0]);
	std::memcpy(bytes, kProjectFileMagic, 8);
	WriteU32(bytes + 8, kProjectFileVersion);
	WriteU32(bytes + 12, ProjectFileGameFlag(file.game));
	WriteU32(bytes + 16, count);
	WriteU32(bytes + 20, 0);
	WriteU64(bytes + 24, 0);

	size_t offset = tableEnd;
	for (uint32_t i = 0; i < count; ++i) {
		const ProjectFileSection& s = *live[i];
		uint8_t* e = bytes + kProjectFileHeaderBytes + i * kProjectFileEntryBytes;
		std::memset(e, 0, kProjectFileEntryBytes);
		std::memcpy(e, s.tag.data(), s.tag.size() < 15 ? s.tag.size() : 15);
		WriteU32(e + 16, kProjectFileEncodingJson);
		WriteU32(e + 20, ProjectFileCrc32(s.payload.data(), s.payload.size()));
		WriteU32(e + 24, (uint32_t)offset);
		WriteU32(e + 28, (uint32_t)s.payload.size());
		std::memcpy(bytes + offset, s.payload.data(), s.payload.size());
		offset += s.payload.size();
	}
	return out;
}

} // namespace Filmmaker
