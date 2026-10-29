// SourceDemoFile, the Source 1 demo reader behind the CS:S demo bar's kill and round markers. The
// demos here are written bit by bit in the test, with the same message layouts a protocol 24 game
// writes, so no game file is needed. Set SOURCEMVM_SOURCE1_TEST_DEMO to a real demo to also scan
// that and print what it holds.

#include "../../AfxHookSource/Filmmaker/SourceDemoFile.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace SourceFilmmaker::DemoFile;

static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++g_failed; } } while (0)

namespace {

class BitWriter {
public:
	void Bits(uint32_t value, int count) {
		for (int i = 0; i < count; ++i, ++m_bits) {
			if ((m_bits >> 3) >= m_data.size()) m_data.push_back(0);
			if (value & (1u << i)) m_data[m_bits >> 3] |= (unsigned char)(1u << (m_bits & 7));
		}
	}
	void Bit(bool b) { Bits(b ? 1 : 0, 1); }
	void String(const char* s) {
		for (; *s; ++s) Bits((unsigned char)*s, 8);
		Bits(0, 8);
	}
	void VarInt(uint32_t value) {
		do {
			uint32_t b = value & 0x7F;
			value >>= 7;
			if (value) b |= 0x80;
			Bits(b, 8);
		} while (value);
	}
	void Zeros(size_t count) { for (size_t i = 0; i < count; ++i) Bit(false); }
	void Append(const BitWriter& other) {
		for (size_t i = 0; i < other.m_bits; ++i) Bit((other.m_data[i >> 3] >> (i & 7)) & 1);
	}
	size_t Size() const { return m_bits; }
	const std::vector<unsigned char>& Data() const { return m_data; }

private:
	std::vector<unsigned char> m_data;
	size_t m_bits = 0;
};

enum EventId { kPlayerDeath = 23, kRoundStart = 40, kPlayerFootstep = 99 };

// svc_GameEventList with player_death (userid, attacker: short; weapon: string; headshot: bool),
// round_start (timelimit: long; objective: string) and player_footstep (userid: short).
void WriteEventList(BitWriter& w) {
	BitWriter body;
	body.Bits(kPlayerDeath, 9); body.String("player_death");
	body.Bits(4, 3); body.String("userid");
	body.Bits(4, 3); body.String("attacker");
	body.Bits(1, 3); body.String("weapon");
	body.Bits(6, 3); body.String("headshot");
	body.Bits(0, 3);
	body.Bits(kRoundStart, 9); body.String("round_start");
	body.Bits(3, 3); body.String("timelimit");
	body.Bits(1, 3); body.String("objective");
	body.Bits(0, 3);
	body.Bits(kPlayerFootstep, 9); body.String("player_footstep");
	body.Bits(4, 3); body.String("userid");
	body.Bits(0, 3);
	w.Bits(30, 6);
	w.Bits(3, 9);
	w.Bits((uint32_t)body.Size(), 20);
	w.Append(body);
}

void WriteGameEvent(BitWriter& w, const BitWriter& body) {
	w.Bits(25, 6);
	w.Bits((uint32_t)body.Size(), 11);
	w.Append(body);
}

void WriteDeath(BitWriter& w, int victim, int attacker, const char* weapon, bool headshot) {
	BitWriter body;
	body.Bits(kPlayerDeath, 9);
	body.Bits((uint32_t)victim, 16);
	body.Bits((uint32_t)attacker, 16);
	body.String(weapon);
	body.Bit(headshot);
	WriteGameEvent(w, body);
}

void WriteRoundStart(BitWriter& w, int timelimit) {
	BitWriter body;
	body.Bits(kRoundStart, 9);
	body.Bits((uint32_t)timelimit, 32);
	body.String("BOMB TARGET");
	WriteGameEvent(w, body);
}

void WriteFootstep(BitWriter& w, int userid) {
	BitWriter body;
	body.Bits(kPlayerFootstep, 9);
	body.Bits((uint32_t)userid, 16);
	WriteGameEvent(w, body);
}

// Messages the reader has to step over by their lengths and layouts.
void WriteNoise(BitWriter& w, int tick) {
	w.Bits(3, 6); w.Bits((uint32_t)tick + 5000, 32); w.Bits(15, 16); w.Bits(0, 16);       // net_Tick
	w.Bits(17, 6); w.Bit(false); w.Bits(2, 8); w.Bits(37, 16); w.Zeros(37);              // svc_Sounds
	w.Bits(26, 6); w.Bits(64, 11); w.Bit(true); w.Bits((uint32_t)(tick - 1), 32);        // svc_PacketEntities
	w.Bit(false); w.Bits(3, 11); w.Bits(300, 20); w.Bit(false); w.Zeros(300);
	w.Bits(27, 6); w.Bits(1, 8); w.VarInt(200); w.Zeros(200);                             // svc_TempEntities
	w.Bits(23, 6); w.Bits(5, 8); w.Bits(40, 11); w.Zeros(40);                             // svc_UserMessage
	w.Bits(21, 6); w.Bit(true); w.Bit(false); w.Bit(true);                                // svc_BSPDecal
	w.Bit(true); w.Bit(true); w.Bit(false); w.Bits(100, 14); w.Bits(3, 5);                // x: integer and fraction
	w.Bit(true); w.Bit(false); w.Bit(true); w.Bits(7, 14);                                // z: integer only
	w.Bits(12, 9); w.Bit(true); w.Bits(3, 11); w.Bits(40, 11); w.Bit(false);
	w.Bits(0, 6);                                                                         // net_NOP
}

void WriteSignon(BitWriter& w) {
	w.Bits(8, 6);                                         // svc_ServerInfo
	w.Bits(24, 16); w.Bits(1, 32); w.Bit(true); w.Bit(false); w.Bits(0xFFFFFFFF, 32); w.Bits(197, 16);
	w.Zeros(128); w.Bits(0, 8); w.Bits(33, 8); w.Bits(0x3C75C28F, 32); w.Bits('w', 8);
	w.String("cstrike"); w.String("de_dust2"); w.String("sky_dust"); w.String("SourceTV"); w.Bit(false);
	w.Bits(12, 6); w.String("userinfo"); w.Bits(256, 16); w.Bits(2, 9);                 // svc_CreateStringTable
	w.VarInt(70); w.Bit(false); w.Bit(false); w.Zeros(70);
	w.Bits(10, 6); w.Bits(197, 16); w.Bit(true);                                         // svc_ClassInfo
	WriteEventList(w);
	w.Bits(6, 6); w.Bits(6, 8); w.Bits(1, 32);                                           // net_SignonState
}

void PutInt(std::vector<unsigned char>& out, int32_t v) {
	unsigned char b[4];
	std::memcpy(b, &v, 4);
	out.insert(out.end(), b, b + 4);
}

void PutPacket(std::vector<unsigned char>& out, int command, int tick, const BitWriter& packet) {
	out.push_back((unsigned char)command);
	PutInt(out, tick);
	out.insert(out.end(), 76 + 8, 0);
	PutInt(out, (int32_t)packet.Data().size());
	out.insert(out.end(), packet.Data().begin(), packet.Data().end());
}

std::vector<unsigned char> Header(int networkProtocol = 24) {
	std::vector<unsigned char> out(1072, 0);
	std::memcpy(out.data(), "HL2DEMO\0", 8);
	int32_t v = 3;
	std::memcpy(&out[8], &v, 4);
	v = networkProtocol;
	std::memcpy(&out[12], &v, 4);
	std::strcpy((char*)&out[16 + 2 * 260], "de_dust2");
	std::strcpy((char*)&out[16 + 3 * 260], "cstrike");
	v = 4410;
	std::memcpy(&out[1060], &v, 4);
	return out;
}

std::vector<unsigned char> TestDemo(bool withStop = true) {
	std::vector<unsigned char> demo = Header();
	BitWriter signon;
	WriteSignon(signon);
	PutPacket(demo, 1, 0, signon);
	demo.push_back(3); PutInt(demo, 0);                       // dem_synctick
	demo.push_back(4); PutInt(demo, 2); PutInt(demo, 4);      // dem_consolecmd
	demo.insert(demo.end(), { 'e', 'c', 'h', 0 });
	BitWriter first;
	WriteNoise(first, 100);
	WriteRoundStart(first, 180);
	WriteFootstep(first, 3);
	PutPacket(demo, 2, 100, first);
	demo.push_back(5); PutInt(demo, 150); PutInt(demo, 9); PutInt(demo, 6); // dem_usercmd
	demo.insert(demo.end(), 6, 0);
	BitWriter second;
	WriteNoise(second, 2000);
	WriteDeath(second, 21, 15, "usp", true);
	WriteNoise(second, 2000);
	WriteDeath(second, 14, 16, "m4a1", false);
	PutPacket(demo, 2, 2000, second);
	if (withStop) { demo.push_back(7); PutInt(demo, 2001); }
	return demo;
}

const std::vector<std::string> kMarkers = { "player_death", "round_start" };

void TestReadsWantedEvents() {
	const std::vector<unsigned char> demo = TestDemo();
	const Scan scan = ScanDemo(demo.data(), demo.size(), kMarkers);
	CHECK(scan.ok);
	if (!scan.ok) std::printf("  error: %s\n", scan.error.c_str());
	CHECK(scan.header.mapName == "de_dust2" && scan.header.gameDirectory == "cstrike");
	CHECK(scan.header.playbackTicks == 4410);
	CHECK(scan.frames == 7);
	CHECK(scan.events.size() == 3);
	if (scan.events.size() != 3) return;
	CHECK(scan.events[0].name == "round_start" && scan.events[0].tick == 100);
	CHECK(scan.events[0].Int("timelimit") == 180 && scan.events[0].Text("objective") == "BOMB TARGET");
	CHECK(scan.events[1].name == "player_death" && scan.events[1].tick == 2000);
	CHECK(scan.events[1].Int("userid") == 21 && scan.events[1].Int("attacker") == 15);
	CHECK(scan.events[1].Text("weapon") == "usp" && scan.events[1].Int("headshot") == 1);
	CHECK(scan.events[2].Int("userid") == 14 && scan.events[2].Int("headshot") == 0);
	CHECK(scan.events[2].Int("missing", -7) == -7);
}

void TestAllEventsWhenNoneNamed() {
	const std::vector<unsigned char> demo = TestDemo();
	const Scan scan = ScanDemo(demo.data(), demo.size(), {});
	CHECK(scan.ok && scan.events.size() == 4); // the footstep too
}

void TestCutShortDemoKeepsWhatWasRead() {
	const std::vector<unsigned char> demo = TestDemo(false);
	const Scan scan = ScanDemo(demo.data(), demo.size(), kMarkers);
	CHECK(scan.ok && scan.events.size() == 3);
	// Cut inside the last packet: an error, with the events before it kept.
	const Scan cut = ScanDemo(demo.data(), demo.size() - 20, kMarkers);
	CHECK(!cut.ok && !cut.error.empty() && cut.events.size() == 1);
}

void TestRejectsOtherFormats() {
	std::vector<unsigned char> demo = TestDemo();
	demo[0] = 'X';
	CHECK(!ScanDemo(demo.data(), demo.size(), kMarkers).ok);
	std::vector<unsigned char> csgo = Header(13);
	CHECK(!ScanDemo(csgo.data(), csgo.size(), kMarkers).ok);
	CHECK(!ScanDemo(nullptr, 0, kMarkers).ok);
}

// The header alone, as the demos browser reads it for every file: an old v34 demo (network protocol
// 7) still has its map and length read, though its body is not scanned.
void TestReadsHeaderAlone() {
	SourceFilmmaker::DemoFile::Header h; // the test's own Header() builds the bytes
	std::vector<unsigned char> demo = Header(7);
	CHECK(ReadHeader(demo.data(), demo.size(), h));
	CHECK(h.demoProtocol == 3 && h.networkProtocol == 7);
	CHECK(h.mapName == "de_dust2" && h.gameDirectory == "cstrike");
	CHECK(h.playbackTicks == 4410);
	CHECK(!ReadHeader(demo.data(), 1071, h));
	demo[0] = 'X';
	CHECK(!ReadHeader(demo.data(), demo.size(), h));
}

void TestUnknownMessageStops() {
	std::vector<unsigned char> demo = Header();
	BitWriter signon;
	WriteSignon(signon);
	PutPacket(demo, 1, 0, signon);
	BitWriter bad;
	WriteRoundStart(bad, 1); // read
	bad.Bits(16, 6);         // svc_HLTV: not sent by these games
	PutPacket(demo, 2, 50, bad);
	const Scan scan = ScanDemo(demo.data(), demo.size(), kMarkers);
	CHECK(!scan.ok && scan.error.find("16") != std::string::npos);
}

void ScanLocalDemo() {
	const char* path = std::getenv("SOURCEMVM_SOURCE1_TEST_DEMO");
	if (!path || !*path) return;
	const Scan scan = ScanDemoFile(path, { "player_death", "round_start", "round_end", "round_freeze_end", "bomb_planted", "bomb_defused", "bomb_exploded" });
	std::printf("%s: %s, map %s, %d ticks, %d frames, %d messages, %d events%s%s\n", path, scan.ok ? "ok" : "FAILED",
		scan.header.mapName.c_str(), scan.header.playbackTicks, scan.frames, scan.messages, (int)scan.events.size(),
		scan.ok ? "" : ": ", scan.error.c_str());
	for (const Event& e : scan.events)
		std::printf("  %6d %s userid=%d attacker=%d %s\n", e.tick, e.name.c_str(), e.Int("userid"), e.Int("attacker"), e.Text("weapon").c_str());
	CHECK(scan.ok);
}

} // namespace

int main() {
	TestReadsWantedEvents();
	TestAllEventsWhenNoneNamed();
	TestCutShortDemoKeepsWhatWasRead();
	TestRejectsOtherFormats();
	TestReadsHeaderAlone();
	TestUnknownMessageStops();
	ScanLocalDemo();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("SourceDemoFileTests passed\n");
	return 0;
}
