// Behaviour tests for the project.mvm container format in
// AfxHookSource2/Filmmaker/Demo/DemoProjectFileMath.h.
#include "../../AfxHookSource2/Filmmaker/Demo/DemoProjectFileMath.h"

#include <cstring>
#include <iostream>
#include <string>

using namespace Filmmaker;

namespace {

int g_failed = 0;

void Expect(bool ok, const char* label) {
	if (ok) return;
	std::cerr << "FAILED " << label << "\n";
	++g_failed;
}

void ExpectString(const std::string& actual, const std::string& expected, const char* label) {
	if (actual == expected) return;
	std::cerr << "FAILED " << label << ": got \"" << actual << "\", expected \"" << expected << "\"\n";
	++g_failed;
}

// Puts back a field in an encoded image (the section table starts at byte 32).
void PatchU32(std::string& bytes, size_t at, uint32_t v) {
	std::memcpy(&bytes[at], &v, 4);
}

ProjectFile SampleFile(ProjectGame game) {
	ProjectFile f;
	f.game = game;
	SetProjectSection(f, kProjectFileInfoTag, "{\"game\":\"cs2\",\"demoPath\":\"C:/a.dem\"}");
	SetProjectSection(f, ProjectSectionTag(ProjectSection::Campath), "{\"version\":2,\"markers\":[]}");
	SetProjectSection(f, ProjectSectionTag(ProjectSection::Loop), "{\"version\":1,\"in\":1,\"out\":2}");
	return f;
}

bool Decodes(const std::string& bytes, ProjectGame game, ProjectFile& out) {
	std::string error;
	return DecodeProjectFile(bytes.data(), bytes.size(), game, out, error);
}

bool Rejected(const std::string& bytes, ProjectGame game, const char* label) {
	ProjectFile out;
	std::string error;
	const bool ok = DecodeProjectFile(bytes.data(), bytes.size(), game, out, error);
	Expect(!ok && !error.empty(), label);
	return !ok;
}

} // namespace

int main() {
	Expect(ProjectFileGameFlag(ProjectGame::Cs2) == kProjectFileGameCs2, "cs2 game flag");
	Expect(ProjectFileGameFlag(ProjectGame::Deadlock) == kProjectFileGameDeadlock, "deadlock flag");
	Expect(ProjectFileGameFlag(ProjectGame::Css) == kProjectFileGameCss, "css flag");
	ExpectString(ProjectSectionTag(ProjectSection::Campath), "campath", "campath tag");
	ExpectString(ProjectSectionTag(ProjectSection::Camshake), "camshake", "camshake tag");
	ExpectString(ProjectSectionTag(ProjectSection::Loop), "loop", "loop tag");
	ExpectString(ProjectSectionTag(ProjectSection::SceneKeys), "scenekeys", "scenekeys tag");
	ExpectString(ProjectSectionTag(ProjectSection::Cosmetics), "cosmetics", "cosmetics tag");
	ExpectString(ProjectSectionTag(ProjectSection::Environment), "environment", "environment tag");
	ExpectString(kProjectFileInfoTag, "project", "info tag");

	// The zlib CRC-32 reference value for "123456789" is 0xCBF43926.
	Expect(ProjectFileCrc32("123456789", 9) == 0xCBF43926u, "crc32 reference vector");

	// Round-trip, both games.
	for (ProjectGame game : {ProjectGame::Cs2, ProjectGame::Deadlock}) {
		const ProjectFile in = SampleFile(game);
		const std::string bytes = EncodeProjectFile(in);
		Expect(bytes.size() >= kProjectFileHeaderBytes + 3 * kProjectFileEntryBytes, "encoded size");
		Expect(!std::memcmp(bytes.data(), kProjectFileMagic, 8), "encoded magic");
		Expect(ReadU32(reinterpret_cast<const uint8_t*>(bytes.data()) + 12) == ProjectFileGameFlag(game),
			"encoded game flag");
		ProjectFile out;
		std::string error;
		Expect(DecodeProjectFile(bytes.data(), bytes.size(), game, out, error), "round trip");
		Expect(out.game == game, "round-trip game");
		Expect(out.sections.size() == in.sections.size(), "round-trip section count");
		for (size_t i = 0; i < in.sections.size(); ++i) {
			ExpectString(out.sections[i].tag, in.sections[i].tag, "round-trip tag");
			ExpectString(out.sections[i].payload, in.sections[i].payload, "round-trip payload");
		}
	}

	const std::string good = EncodeProjectFile(SampleFile(ProjectGame::Cs2));

	// Empty and truncated files.
	Expect(Rejected("", ProjectGame::Cs2, "empty file"), "empty file rejected");
	Expect(Rejected(good.substr(0, 20), ProjectGame::Cs2, "truncated header"), "truncated header");
	Expect(Rejected(good.substr(0, good.size() - 4), ProjectGame::Cs2, "truncated payload"),
		"truncated payload");

	// Wrong magic and wrong version.
	{
		std::string bad = good; bad[0] = 'X';
		Expect(Rejected(bad, ProjectGame::Cs2, "bad magic"), "bad magic rejected");
		bad = good; PatchU32(bad, 8, 2);
		Expect(Rejected(bad, ProjectGame::Cs2, "version 2"), "version 2 rejected");
	}

	// Game flag: unknown value, game mismatch, reserved flag bits, reserved fields.
	{
		std::string bad = good; PatchU32(bad, 12, 7);
		Expect(Rejected(bad, ProjectGame::Cs2, "unknown game flag"), "unknown game flag");
		bad = good; PatchU32(bad, 12, kProjectFileGameCss);
		{
			ProjectFile out;
			Expect(!Decodes(bad, ProjectGame::Cs2, out), "a CS:S project does not open as CS2's");
			Expect(Decodes(bad, ProjectGame::Css, out), "a CS:S project opens for CS:S");
		}
		bad = good; PatchU32(bad, 12, kProjectFileGameDeadlock);
		Expect(Rejected(bad, ProjectGame::Cs2, "game mismatch"), "game mismatch");
		{
			ProjectFile out;
			Expect(Decodes(bad, ProjectGame::Deadlock, out), "deadlock copy opens for deadlock");
		}
		bad = good; PatchU32(bad, 12, kProjectFileGameCs2 | 0x10u);
		Expect(Rejected(bad, ProjectGame::Cs2, "reserved flag bit"), "reserved flag bit");
		bad = good; PatchU32(bad, 20, 1);
		Expect(Rejected(bad, ProjectGame::Cs2, "reserved0 set"), "reserved0");
		bad = good; bad[24] = 1;
		Expect(Rejected(bad, ProjectGame::Cs2, "reserved1 set"), "reserved1");
	}

	// Section count over the cap.
	{
		std::string bad = good; PatchU32(bad, 16, kProjectFileMaxSections + 1);
		Expect(Rejected(bad, ProjectGame::Cs2, "sectionCount > 64"), "sectionCount cap");
	}

	// Section table entries sit at 32 + i*32: tag[16] encoding crc offset size.
	{
		// Encoding != 1.
		std::string bad = good; PatchU32(bad, 32 + 16, 7);
		Expect(Rejected(bad, ProjectGame::Cs2, "encoding != 1"), "encoding rejected");
		// Payload beyond the file.
		bad = good; PatchU32(bad, 32 + 24, (uint32_t)good.size() + 64);
		Expect(Rejected(bad, ProjectGame::Cs2, "offset out of bounds"), "offset bounds");
		bad = good; PatchU32(bad, 32 + 28, (uint32_t)good.size());
		Expect(Rejected(bad, ProjectGame::Cs2, "size out of bounds"), "size bounds");
		// Offset inside the table.
		bad = good; PatchU32(bad, 32 + 24, 40);
		Expect(Rejected(bad, ProjectGame::Cs2, "offset inside table"), "offset under table");
		// Oversized payload field.
		bad = good; PatchU32(bad, 32 + 28, kProjectFileMaxPayload + 1);
		Expect(Rejected(bad, ProjectGame::Cs2, "payload over cap"), "payload cap");
		// CRC mismatch (flip a byte in the first payload, keep the stored crc).
		bad = good;
		const uint32_t off = ReadU32(reinterpret_cast<const uint8_t*>(bad.data()) + 32 + 24);
		bad[off] = char(bad[off] ^ 0xFF);
		Expect(Rejected(bad, ProjectGame::Cs2, "crc mismatch"), "crc mismatch");
		// Duplicate tag: copy entry 0's tag over entry 1's.
		bad = good;
		std::memcpy(&bad[32 + kProjectFileEntryBytes], &bad[32], 16);
		Expect(Rejected(bad, ProjectGame::Cs2, "duplicate tag"), "duplicate tag");
		// Overlapping payloads: point entry 1 at entry 0's range.
		bad = good;
		const uint32_t off0 = ReadU32(reinterpret_cast<const uint8_t*>(bad.data()) + 32 + 24);
		const uint32_t size0 = ReadU32(reinterpret_cast<const uint8_t*>(bad.data()) + 32 + 28);
		PatchU32(bad, 32 + 32 + 24, off0);
		PatchU32(bad, 32 + 32 + 28, size0);
		PatchU32(bad, 32 + 32 + 20, ReadU32(reinterpret_cast<const uint8_t*>(good.data()) + 32 + 20));
		Expect(Rejected(bad, ProjectGame::Cs2, "overlapping payloads"), "overlap rejected");
	}

	// SetProjectSection: empty payload removes, new tag appends, existing tag replaces.
	{
		ProjectFile f = SampleFile(ProjectGame::Cs2);
		SetProjectSection(f, "loop", "");
		Expect(FindProjectSection(f, "loop") == nullptr, "empty payload removes section");
		Expect(f.sections.size() == 2, "removed section count");
		const std::string bytes = EncodeProjectFile(f);
		ProjectFile out;
		Expect(Decodes(bytes, ProjectGame::Cs2, out), "decode after removal");
		Expect(FindProjectSection(out, "loop") == nullptr, "removed section stays gone");
		Expect(FindProjectSection(out, "campath") != nullptr, "other sections kept");

		SetProjectSection(f, "loop", "{\"in\":3}");
		Expect(FindProjectSection(f, "loop") != nullptr, "section re-added");
		SetProjectSection(f, "campath", "{\"v\":9}");
		Expect(FindProjectSection(f, "campath")->payload == "{\"v\":9}", "payload replaced");
		Expect(f.sections.size() == 3, "no duplicate after replace");
	}

	if (g_failed) {
		std::cerr << g_failed << " check(s) failed\n";
		return 1;
	}
	std::cout << "DemoProjectFileMathTests passed\n";
	return 0;
}
