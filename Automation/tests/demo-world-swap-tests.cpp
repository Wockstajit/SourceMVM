// DemoWorldSwap: the copy of a demo that loads another world. A small synthetic demo carries every
// place a demo names its world: the file header, ServerInfo in a compressed signon packet, the
// main world and skybox spawn groups in a packet (GOTV) and in the DEM_SpawnGroups index (client
// demos), plus messages and frames that must come through untouched.
#include "../../AfxHookSource2/Filmmaker/Demo/DemoWorldSwap.h"
#include "../../AfxHookSource2/Filmmaker/Platform/SnappyDecode.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Filmmaker;

namespace {
int g_failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); ++g_failed; } } while (0)

// ---- encoding helpers --------------------------------------------------------------------------
void Varint(std::string& s, uint64_t v) {
	do { uint8_t b = uint8_t(v & 0x7f); v >>= 7; if (v) b |= 0x80; s.push_back(char(b)); } while (v);
}
std::string Str(uint32_t n, const std::string& v) { std::string s; Varint(s, (n << 3) | 2); Varint(s, v.size()); return s + v; }
std::string Num(uint32_t n, uint64_t v) { std::string s; Varint(s, n << 3); Varint(s, v); return s; }
std::string Flt(uint32_t n, float f) { std::string s; Varint(s, (n << 3) | 5); char b[4]; std::memcpy(b, &f, 4); return s.append(b, 4); }
std::string Vec(float x, float y, float z) { return Flt(1, x) + Flt(2, y) + Flt(3, z); }

struct Bits {
	std::string out;
	size_t pos = 0;
	void Put(uint32_t v, int n) {
		for (int k = 0; k < n; ++k, ++pos) {
			if ((pos >> 3) >= out.size()) out.push_back(0);
			if ((v >> k) & 1) out[pos >> 3] = char(uint8_t(out[pos >> 3]) | (1u << (pos & 7)));
		}
	}
	void Message(uint32_t type, const std::string& body) {
		if (type < 16) Put(type, 6);
		else if (type < 256) Put((type & 15) | 16, 6), Put(type >> 4, 4);
		else Put((type & 15) | 32, 6), Put(type >> 4, 8);
		uint32_t v = uint32_t(body.size());
		do { uint32_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; Put(b, 8); } while (v);
		for (unsigned char c : body) Put(c, 8);
	}
};

// A snappy literal block: valid input for SnappyDecode without a compressor.
std::string SnappyLiteral(const std::string& raw) {
	std::string s;
	Varint(s, raw.size());
	const size_t n = raw.size() - 1;
	if (n < 60) s.push_back(char(n << 2));
	else { s.push_back(char(62 << 2)); s.push_back(char(n & 0xff)); s.push_back(char((n >> 8) & 0xff)); s.push_back(char((n >> 16) & 0xff)); }
	return s + raw;
}

std::string Frame(uint64_t cmd, uint64_t tick, const std::string& payload) {
	std::string s;
	Varint(s, cmd); Varint(s, tick); Varint(s, payload.size());
	return s + payload;
}

const std::string kManifest = std::string("\x01\x02\x03\xff manifest bytes", 18);

std::string MainWorld() {
	return Str(1, "de_inferno") + Str(3, "mapload") + Num(4, 1) + Str(6, Vec(0, 0, 0)) + Str(7, Vec(0, 0, 0)) +
		Str(8, kManifest) + Num(9, 152) + Num(16, 1) + Str(20, "default");
}
std::string Skybox() {
	return Str(1, "maps/prefabs/de_inferno/inferno_skybox") + Num(4, 2) + Str(6, Vec(0, 0, 0)) + Str(7, Vec(0, 0, 0)) +
		Num(15, 2) + Num(18, 1) + Str(20, "skyboxWorldGroup0");
}
std::string Prefab() {
	return Str(1, "prefabs/misc/team_select") + Num(4, 3) + Str(6, Vec(-773.5f, 515.2f, -24)) + Str(7, Vec(0, 174.1f, 0)) +
		Num(18, 1) + Str(20, "default");
}

struct Built {
	std::string bytes, unrelatedFrame;
};
Built BuildDemo() {
	Built b;
	std::string s(16, '\0');
	std::memcpy(&s[0], "PBDEMS2", 8);
	s += Frame(1, 0, Str(1, "PBDEMS2") + Num(2, 14165) + Str(5, "de_inferno") + Str(6, "csgo") + Str(10, ""));
	// Signon: a tick message first, so ServerInfo starts off a byte boundary.
	Bits signon;
	signon.Message(4, Num(1, 7) + Num(2, 9));
	signon.Message(51, Str(1, "de_inferno") + Num(3, 0)); // svc_ClearAllStringTables
	const std::string session = Num(1, 1) + Str(9, "host") + Str(11, "de_inferno") + Str(13, "1.2.3.4:0");
	signon.Message(40, Num(1, 14165) + Flt(13, 0.015625f) + Str(15, "de_inferno") + Str(16, "sky") + Str(18, "") + Str(19, session));
	s += Frame(8 | 64, 0xffffffff, SnappyLiteral(Str(3, signon.out)));
	// First game packet: the GOTV spawn groups, between unrelated messages.
	Bits packet;
	packet.Message(4, Num(1, 1));
	packet.Message(8, MainWorld());
	packet.Message(8, Skybox());
	packet.Message(8, Prefab());
	packet.Message(300, std::string(37, 'x')); // an 8-bit-extension type
	s += Frame(7, 1, Num(1, 0) + Num(2, 0) + Str(3, packet.out));
	// An ordinary packet: must be copied byte for byte, compressed as it was.
	Bits plain;
	plain.Message(55, std::string(300, 'e'));
	plain.Message(4, Num(1, 2));
	b.unrelatedFrame = Frame(7 | 64, 2, SnappyLiteral(Str(3, plain.out)));
	s += b.unrelatedFrame;
	s += Frame(0, 3, ""); // DEM_Stop
	const int32_t fileInfo = int32_t(s.size());
	s += Frame(2, 3, Flt(1, 12.5f) + Num(2, 800));
	const int32_t spawnGroups = int32_t(s.size());
	Bits index;
	index.Message(8, MainWorld());
	Bits indexSky;
	indexSky.Message(8, Skybox());
	s += Frame(15, 3, Str(3, index.out) + Str(3, indexSky.out));
	std::memcpy(&s[8], &fileInfo, 4);
	std::memcpy(&s[12], &spawnGroups, 4);
	b.bytes = s;
	return b;
}

// ---- reading the copy back ---------------------------------------------------------------------
bool Uvarint(const std::string& s, size_t& i, uint64_t& v) {
	v = 0;
	for (int sh = 0; sh < 64; sh += 7) {
		if (i >= s.size()) return false;
		const uint8_t c = uint8_t(s[i++]);
		v |= uint64_t(c & 0x7f) << sh;
		if (!(c & 0x80)) return true;
	}
	return false;
}
struct Pb {
	uint32_t n, w;
	uint64_t v;
	std::string bytes;
};
std::vector<Pb> Fields(const std::string& s) {
	std::vector<Pb> out;
	size_t i = 0;
	while (i < s.size()) {
		uint64_t tag;
		if (!Uvarint(s, i, tag)) break;
		Pb f{ uint32_t(tag >> 3), uint32_t(tag & 7), 0, {} };
		if (f.w == 0) Uvarint(s, i, f.v);
		else if (f.w == 5) { f.bytes = s.substr(i, 4); i += 4; }
		else if (f.w == 2) { uint64_t l; Uvarint(s, i, l); f.bytes = s.substr(i, size_t(l)); i += size_t(l); }
		else break;
		out.push_back(f);
	}
	return out;
}
std::string Get(const std::string& s, uint32_t n) {
	for (const Pb& f : Fields(s)) if (f.n == n) return f.bytes;
	return "<none>";
}
float GetFloat(const std::string& vec, uint32_t n) {
	const std::string b = Get(vec, n);
	float f = 0;
	if (b.size() == 4) std::memcpy(&f, b.data(), 4);
	return f;
}
struct Msg { uint32_t type; std::string body; };
std::vector<Msg> Messages(const std::string& s) {
	std::vector<Msg> out;
	size_t pos = 0;
	auto bits = [&](int n) { uint32_t v = 0; for (int k = 0; k < n; ++k, ++pos) v |= uint32_t((uint8_t(s[pos >> 3]) >> (pos & 7)) & 1) << k; return v; };
	while (s.size() * 8 - pos >= 14) {
		uint32_t t = bits(6);
		if ((t & 0x30) == 0x10) t = (t & 15) | (bits(4) << 4);
		else if ((t & 0x30) == 0x20) t = (t & 15) | (bits(8) << 4);
		else if ((t & 0x30) == 0x30) t = (t & 15) | (bits(28) << 4);
		uint32_t len = 0;
		for (int k = 0; k < 5; ++k) { const uint32_t c = bits(8); len |= (c & 0x7f) << (7 * k); if (!(c & 0x80)) break; }
		if (s.size() * 8 - pos < size_t(len) * 8) break;
		std::string body;
		for (uint32_t k = 0; k < len; ++k) body.push_back(char(bits(8)));
		out.push_back({ t, body });
	}
	return out;
}
struct OutFrame { size_t start; uint64_t cmd, tick; std::string payload, raw; };
std::vector<OutFrame> Frames(const std::string& s) {
	std::vector<OutFrame> out;
	size_t i = 16;
	while (i < s.size()) {
		OutFrame f;
		f.start = i;
		uint64_t size;
		if (!Uvarint(s, i, f.cmd) || !Uvarint(s, i, f.tick) || !Uvarint(s, i, size)) break;
		f.payload = s.substr(i, size_t(size));
		i += size_t(size);
		f.raw = s.substr(f.start, i - f.start);
		out.push_back(f);
	}
	return out;
}

DemoWorldSwap Spec() {
	DemoWorldSwap spec;
	spec.sourceMap = "de_inferno";
	spec.map = "de_inferno_night";
	spec.addon = "3124567099";
	spec.skybox = "maps/prefabs/de_inferno/inferno_night_skybox";
	spec.offset = AltEnvironment::WorldOffsetFor({ 256, 128, 16, 30 });
	return spec;
}

void TestSnappy() {
	std::vector<uint8_t> out;
	const std::string lit = SnappyLiteral("hello snappy");
	CHECK(SnappyDecode(reinterpret_cast<const uint8_t*>(lit.data()), lit.size(), out));
	CHECK(std::string(out.begin(), out.end()) == "hello snappy");
	// "abc" then a 9-byte copy at offset 3 that overlaps itself.
	const uint8_t rep[] = { 12, 2 << 2, 'a', 'b', 'c', uint8_t(((9 - 4) << 2) | 1), 3 };
	CHECK(SnappyDecode(rep, sizeof rep, out) && std::string(out.begin(), out.end()) == "abcabcabcabc");
	const uint8_t bad[] = { 12, 2 << 2, 'a', 'b', 'c', uint8_t(((9 - 4) << 2) | 1), 9 }; // offset past the start
	CHECK(!SnappyDecode(bad, sizeof bad, out));
	const uint8_t shortLit[] = { 5, 4 << 2, 'a' };
	CHECK(!SnappyDecode(shortLit, sizeof shortLit, out));
}

void TestSwap() {
	const Built demo = BuildDemo();
	std::istringstream in(demo.bytes, std::ios::binary);
	std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
	double lastProgress = 0;
	const DemoWorldSwapResult r = SwapDemoWorld(in, out, Spec(), nullptr, [&](double p) { lastProgress = p; });
	CHECK(r.ok);
	if (!r.ok) { std::printf("  error: %s\n", r.error.c_str()); return; }
	CHECK(r.headers == 1 && r.serverInfos == 1 && r.levelNames == 1 && r.worlds == 2 && r.skyboxes == 2);
	CHECK(r.headerMap == "de_inferno");
	CHECK(r.frames == 7 && r.bytesIn == demo.bytes.size());
	CHECK(lastProgress == 1.0);
	const std::string copy = out.str();
	CHECK(r.bytesOut == copy.size());
	CHECK(copy.compare(0, 8, std::string("PBDEMS2\0", 8)) == 0);

	const auto frames = Frames(copy);
	CHECK(frames.size() == 7);
	if (frames.size() != 7) return;
	// Header offsets follow their frames.
	int32_t fileInfo, spawnGroups;
	std::memcpy(&fileInfo, copy.data() + 8, 4);
	std::memcpy(&spawnGroups, copy.data() + 12, 4);
	CHECK(size_t(fileInfo) == frames[5].start && frames[5].cmd == 2);
	CHECK(size_t(spawnGroups) == frames[6].start && frames[6].cmd == 15);

	CHECK(frames[0].cmd == 1);
	CHECK(Get(frames[0].payload, 5) == "de_inferno_night" && Get(frames[0].payload, 10) == "3124567099");
	CHECK(Get(frames[0].payload, 6) == "csgo"); // untouched fields stay

	// The signon came in compressed and leaves uncompressed.
	CHECK(frames[1].cmd == 8);
	const auto signon = Messages(Get(frames[1].payload, 3));
	CHECK(signon.size() == 3 && signon[0].type == 4 && signon[1].type == 51 && signon[2].type == 40);
	if (signon.size() == 3) {
		CHECK(Get(signon[1].body, 1) == "de_inferno_night");
		CHECK(Get(signon[2].body, 15) == "de_inferno_night" && Get(signon[2].body, 18) == "3124567099");
		CHECK(Get(signon[2].body, 16) == "sky");
		const std::string config = Get(signon[2].body, 19);
		CHECK(Get(config, 11) == "de_inferno_night" && Get(config, 9) == "host" && Get(config, 13) == "1.2.3.4:0");
	}

	const AltEnvironment::WorldOffset o = Spec().offset;
	const auto packet = Messages(Get(frames[2].payload, 3));
	CHECK(packet.size() == 5);
	if (packet.size() == 5) {
		CHECK(packet[0].type == 4 && packet[4].type == 300 && packet[4].body == std::string(37, 'x'));
		CHECK(Get(packet[1].body, 1) == "de_inferno_night");
		CHECK(Get(packet[1].body, 8) == kManifest && Get(packet[1].body, 20) == "default");
		const std::string pos = Get(packet[1].body, 6), ang = Get(packet[1].body, 7);
		CHECK(std::fabs(GetFloat(pos, 1) - float(o.pos.x)) < 1e-3 && std::fabs(GetFloat(pos, 2) - float(o.pos.y)) < 1e-3);
		CHECK(std::fabs(GetFloat(pos, 3) - float(o.pos.z)) < 1e-3 && std::fabs(GetFloat(ang, 2) - float(o.yaw)) < 1e-3);
		CHECK(Get(packet[2].body, 1) == "maps/prefabs/de_inferno/inferno_night_skybox");
		CHECK(std::fabs(GetFloat(Get(packet[2].body, 7), 2) - float(o.yaw)) < 1e-3);
		CHECK(packet[3].body == Prefab()); // other spawn groups keep their own placement
	}
	CHECK(frames[3].raw == demo.unrelatedFrame);

	const auto index = Fields(frames[6].payload);
	CHECK(index.size() == 2);
	if (index.size() == 2) {
		const auto main = Messages(index[0].bytes), sky = Messages(index[1].bytes);
		CHECK(main.size() == 1 && Get(main[0].body, 1) == "de_inferno_night");
		CHECK(sky.size() == 1 && Get(sky[0].body, 1) == "maps/prefabs/de_inferno/inferno_night_skybox");
	}
}

void TestKeepSkybox() {
	DemoWorldSwap spec = Spec();
	spec.skybox.clear(); // the replacement has no 3D skybox: the demo's own stays, moved with the world
	const Built demo = BuildDemo();
	std::istringstream in(demo.bytes, std::ios::binary);
	std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
	const DemoWorldSwapResult r = SwapDemoWorld(in, out, spec);
	CHECK(r.ok && r.skyboxes == 2);
	const auto frames = Frames(out.str());
	if (frames.size() < 3) { CHECK(false); return; }
	const auto packet = Messages(Get(frames[2].payload, 3));
	CHECK(packet.size() == 5 && Get(packet[2].body, 1) == "maps/prefabs/de_inferno/inferno_skybox");
}

void TestRefusals() {
	std::istringstream notDemo(std::string("HL2DEMO\0........", 16), std::ios::binary);
	std::stringstream out(std::ios::in | std::ios::out | std::ios::binary);
	CHECK(!SwapDemoWorld(notDemo, out, Spec()).ok);
	DemoWorldSwap wrongMap = Spec();
	wrongMap.sourceMap = "de_mirage";
	const Built demo = BuildDemo();
	std::istringstream in(demo.bytes, std::ios::binary);
	std::stringstream out2(std::ios::in | std::ios::out | std::ios::binary);
	const DemoWorldSwapResult r = SwapDemoWorld(in, out2, wrongMap);
	CHECK(!r.ok && r.error.find("de_mirage") != std::string::npos);
	std::atomic<bool> cancel{ true };
	std::istringstream in3(demo.bytes, std::ios::binary);
	std::stringstream out3(std::ios::in | std::ios::out | std::ios::binary);
	CHECK(SwapDemoWorld(in3, out3, Spec(), &cancel).error == "cancelled");
	std::string truncated = demo.bytes.substr(0, demo.bytes.size() - 5);
	std::istringstream in4(truncated, std::ios::binary);
	std::stringstream out4(std::ios::in | std::ios::out | std::ios::binary);
	CHECK(!SwapDemoWorld(in4, out4, Spec()).ok);
}
// A copy slot is written again on the third and every later change: the file swap must replace the
// slot's earlier copy (review on #105).
void TestFileReplacesSlot() {
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "mvm-demo-world-swap-test";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	const fs::path demoPath = dir / "match.dem", slot = dir / "match.altenv1.dem";
	{
		const Built demo = BuildDemo();
		std::ofstream(demoPath, std::ios::binary).write(demo.bytes.data(), std::streamsize(demo.bytes.size()));
	}
	const DemoWorldSwapResult first = SwapDemoWorldFile(demoPath, slot, Spec());
	CHECK(first.ok);
	const DemoWorldSwapResult again = SwapDemoWorldFile(demoPath, slot, Spec());
	CHECK(again.ok);
	if (!again.ok) std::printf("  error: %s\n", again.error.c_str());
	CHECK(fs::file_size(slot, ec) == again.bytesOut);
	CHECK(!fs::exists(fs::path(slot) += L".part", ec));
	fs::remove_all(dir, ec);
}
} // namespace

int main() {
	TestSnappy();
	TestSwap();
	TestKeepSkybox();
	TestRefusals();
	TestFileReplacesSlot();
	if (g_failed) {
		std::printf("%d check(s) failed\n", g_failed);
		return 1;
	}
	std::printf("demo world swap: all checks passed\n");
	return 0;
}
