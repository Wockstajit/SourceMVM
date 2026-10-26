// The old-demo converter (AfxHookSource/Filmmaker/DemoConvert): bit encodings, the network table
// layouts, brush model matching, and a whole conversion of a small v34 demo written bit by bit here,
// read back with the current-format reader. No game file is needed.

#include "../../AfxHookSource/Filmmaker/DemoConvert/ConvertV34.h"
#include "../../AfxHookSource/Filmmaker/DemoConvert/DemoBits.h"
#include "../../AfxHookSource/Filmmaker/DemoConvert/DemoTables.h"
#include "../../AfxHookSource/Filmmaker/DemoConvert/MapModels.h"
#include "../../AfxHookSource/Filmmaker/SourceDemoFile.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace SourceFilmmaker;
using namespace SourceFilmmaker::DemoConvert;

static int g_failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED line %d: %s\n", __LINE__, #x); ++g_failed; } } while (0)

namespace {

// Episode One's bf_write::WriteUBitVar: n zeros, a one, then value - (2^n - 1) in n bits.
void UBitVarEp1(BitWriter& w, uint32_t value) {
	int n = 0;
	while (value >= (1u << (n + 1)) - 1) ++n;
	for (int i = 0; i < n; ++i) w.Bit(false);
	w.Bit(true);
	w.Bits(value - ((1u << n) - 1), n);
}

void TestBits() {
	for (uint32_t v : { 0u, 1u, 2u, 3u, 6u, 7u, 100u, 4095u, 70000u }) {
		BitWriter w;
		UBitVarEp1(w, v);
		w.UBitVar2013(v);
		w.VarInt(v);
		BitReader r(w.Data().data(), 0, w.Size());
		CHECK(r.UBitVarEp1() == v);
		CHECK(r.UBitVar2013() == v);
		CHECK(r.VarInt() == v);
		CHECK(!r.Overrun() && r.Left() == 0);
	}
	BitWriter w;
	w.Bits(5, 3);
	w.String("abc");
	w.Float(1.5f);
	w.Signed(-3, 5);
	BitWriter copy;
	BitReader source(w.Data().data(), 0, w.Size());
	copy.Copy(source, 3, w.Size()); // unaligned copy
	BitReader r(copy.Data().data(), 0, copy.Size());
	CHECK(r.String() == "abc" && r.Float() == 1.5f && r.Signed(5) == -3);
	BitReader past(w.Data().data(), 0, 4);
	past.Bits(8);
	CHECK(past.Overrun());
}

SendProp Prop(PropType type, const char* name, uint32_t flags, int bits = 0, float low = 0, float high = 0) {
	SendProp p;
	p.type = type;
	p.name = name;
	p.flags = flags;
	p.bits = bits;
	p.low = low;
	p.high = high;
	return p;
}

DataTables TestTables() {
	DataTables t;
	SendTable test;
	test.name = "DT_Test";
	test.props.push_back(Prop(PropType::Int, "m_nModelIndex", kPropUnsigned, 11));
	test.props.push_back(Prop(PropType::Vector, "m_vecOrigin", kPropCoord));
	test.props.push_back(Prop(PropType::Vector, "m_vecMins", kPropNoScale));
	test.props.push_back(Prop(PropType::Vector, "m_vecMaxs", kPropNoScale));
	SendProp element = Prop(PropType::Int, "m_hAttach", kPropUnsigned | kPropInsideArray, 21);
	test.props.push_back(element);
	SendProp array = Prop(PropType::Array, "m_hAttach", 0);
	array.elements = 4;
	test.props.push_back(array);
	t.tables.push_back(test);
	ServerClass c;
	c.id = 0;
	c.name = "CTest";
	c.table = "DT_Test";
	t.classes.push_back(c);
	return t;
}

void TestTablesRoundTrip() {
	DataTables t = TestTables();
	BitWriter w;
	WriteDataTables(t, w);
	BitReader r(w.Data().data(), 0, w.Size());
	DataTables back;
	std::string error;
	CHECK(ReadDataTables(r, 24, back, error));
	CHECK(back.tables.size() == 1 && back.tables[0].props.size() == 6 && back.classes.size() == 1);
	if (back.tables.size() == 1 && back.tables[0].props.size() == 6) {
		CHECK(back.tables[0].props[0].bits == 11 && back.tables[0].props[5].type == PropType::Array && back.tables[0].props[5].elements == 4);
	}
	const std::vector<FlatProp> flat = t.Flatten("DT_Test");
	CHECK(flat.size() == 5 && flat[4].element && flat[4].element->name == "m_hAttach");

	// The array becomes a SendPropArray3 table whose element props come before the parent's.
	CHECK(ArrayToTable(t, "DT_Test", "m_hAttach"));
	CHECK(t.Find("m_hAttach") && t.Find("m_hAttach")->props.size() == 4 && t.Find("m_hAttach")->props[3].name == "003");
	const std::vector<FlatProp> converted = t.Flatten("DT_Test");
	CHECK(converted.size() == 8 && converted[0].prop->name == "000" && converted[4].prop->name == "m_nModelIndex");
	CHECK(!ArrayToTable(t, "DT_Test", "m_nModelIndex"));
}

MapModel Model(float x0, float y0, float z0, float x1, float y1, float z1, float ox = 0, float oy = 0, float oz = 0) {
	MapModel m;
	m.mins[0] = x0; m.mins[1] = y0; m.mins[2] = z0;
	m.maxs[0] = x1; m.maxs[1] = y1; m.maxs[2] = z1;
	m.origin[0] = ox; m.origin[1] = oy; m.origin[2] = oz;
	return m;
}

BrushUse Use(int n, MapModel m, float ox, float oy, float oz) {
	BrushUse u;
	u.number = n;
	u.seen = true;
	for (int i = 0; i < 3; ++i) {
		u.mins[i] = m.mins[i] - 1; // demos send collision bounds a unit wider
		u.maxs[i] = m.maxs[i] + 1;
	}
	u.origin[0] = ox; u.origin[1] = oy; u.origin[2] = oz;
	return u;
}

void TestMatching() {
	MapModels map;
	map.models.push_back(Model(-4096, -4096, -4096, 4096, 4096, 4096));            // world
	map.models.push_back(Model(960, -262, 292, 962, -228, 340));                   // *1 a glass pane, world space
	map.models.push_back(Model(-24, -6, -3, 24, 2, 113, 1240, -266, 259));         // *2 a door, moved here
	map.models.push_back(Model(-1, -4, -1, 1, 4, 1, 448, -1463, -235));            // *3 a fan part
	map.models.push_back(Model(5000, 5000, 5000, 5010, 5010, 5010));               // *4 nothing old uses

	std::vector<BrushUse> uses(6);
	uses[1] = Use(1, map.models[1], 0, 0, 0);                  // same place: kept
	uses[3] = Use(3, map.models[2], 1240, -266, 259);         // the door, recorded as *3: moved
	uses[4] = Use(4, map.models[3], 0, 22, 22);               // the fan part, origin relative to a parent
	BrushUse gone;
	gone.number = 5;
	gone.seen = true;
	gone.mins[0] = -3137; gone.mins[1] = -1553; gone.mins[2] = 175;   // a world-space volume Valve removed
	gone.maxs[0] = 2913; gone.maxs[1] = 4105; gone.maxs[2] = 449;
	uses[5] = gone;
	const InlineRemap r = MatchInlineModels(uses, 7, map);
	CHECK(r.target[1] == 1);
	CHECK(r.target[3] == 2);
	CHECK(r.target[4] == 3);   // same shape, built around its origin
	CHECK(r.target[5] == -1);
	CHECK(r.target[2] == 4);   // only precached: takes the free number
	CHECK(r.target[6] == -1);  // only precached, none left
	CHECK(r.kept == 1 && r.moved == 2 && r.missing == 1 && r.unused == 2);

	// A door first seen open: same size, far from where the map has it.
	std::vector<BrushUse> open(2);
	open[1] = Use(1, map.models[2], 1240, -266, 259 + 128);
	MapModels doorMap;
	doorMap.models.push_back(map.models[0]);
	doorMap.models.push_back(map.models[2]);
	CHECK(MatchInlineModels(open, 2, doorMap).target[1] == 1);
}

// ---- a whole v34 demo -------------------------------------------------------------------------

void PutInt(std::vector<unsigned char>& out, int32_t v) {
	unsigned char b[4];
	std::memcpy(b, &v, 4);
	out.insert(out.end(), b, b + 4);
}

void PutFrame(std::vector<unsigned char>& out, int command, int tick, const BitWriter* payload) {
	out.push_back((unsigned char)command);
	PutInt(out, tick);
	if (command == 1 || command == 2) out.insert(out.end(), 84, 0);
	if (payload) {
		PutInt(out, (int32_t)payload->Bytes());
		out.insert(out.end(), payload->Data().begin(), payload->Data().end());
	}
}

// One string table entry in the v34 layout: next index, a whole string, optional variable data.
void Entry(BitWriter& w, const char* name, const BitWriter* data) {
	w.Bit(true);
	w.Bit(true);
	w.Bit(false);
	w.String(name);
	w.Bit(data != nullptr);
	if (data) {
		BitWriter whole = *data;
		whole.PadToByte();
		w.Bits((uint32_t)whole.Bytes(), 12);
		w.Append(whole);
	}
}

void CreateTable(BitWriter& w, const char* name, int maxEntries, const BitWriter& entries, int count) {
	w.Bits(12, 5);
	w.String(name);
	w.Bits((uint32_t)maxEntries, 16);
	w.Bits((uint32_t)count, Log2((unsigned)maxEntries) + 1);
	w.Bits((uint32_t)entries.Size(), 20);
	w.Bit(false);
	w.Append(entries);
}

// The prop list of the test entity: model index, then mins and maxs (origin stays at zero).
void PropList(BitWriter& w, int model) {
	w.Bit(true); UBitVarEp1(w, 0); w.Bits((uint32_t)model, 11);
	w.Bit(true); UBitVarEp1(w, 1);                              // m_vecMins
	w.Float(-8); w.Float(-8); w.Float(-8);
	w.Bit(true); UBitVarEp1(w, 0);                              // m_vecMaxs
	w.Float(8); w.Float(8); w.Float(8);
	w.Bit(false);
}

std::vector<unsigned char> V34Demo() {
	std::vector<unsigned char> demo(1072, 0);
	std::memcpy(demo.data(), "HL2DEMO\0", 8);
	int32_t v = 3;
	std::memcpy(&demo[8], &v, 4);
	v = 7;
	std::memcpy(&demo[12], &v, 4);
	std::strcpy((char*)&demo[16 + 2 * 260], "de_test");
	std::strcpy((char*)&demo[16 + 3 * 260], "cstrike");
	v = 100;
	std::memcpy(&demo[1060], &v, 4);

	BitWriter signon;
	signon.Bits(8, 5);                               // svc_ServerInfo, v34 layout
	signon.Bits(7, 16); signon.Bits(1, 32); signon.Bit(true); signon.Bit(false); signon.Bits(0, 32);
	signon.Bits(1, 16); signon.Bits(0x1234, 32); signon.Bits(0, 8); signon.Bits(10, 8); signon.Float(0.01f);
	signon.Bits('w', 8); signon.String("cstrike"); signon.String("de_test"); signon.String("sky"); signon.String("host");
	BitWriter events;                                // svc_GameEventList
	events.Bits(23, 9); events.String("player_death");
	events.Bits(4, 3); events.String("userid");
	events.Bits(4, 3); events.String("attacker");
	events.Bits(0, 3);
	signon.Bits(30, 5); signon.Bits(1, 9); signon.Bits((uint32_t)events.Size(), 20); signon.Append(events);
	PutFrame(demo, 1, 0, &signon);

	BitWriter stringTables;                          // a signon frame of its own, so the test reads it back easily
	BitWriter models;
	Entry(models, "", nullptr);
	Entry(models, "*1", nullptr);
	Entry(models, "*2", nullptr);
	CreateTable(stringTables, "modelprecache", 1024, models, 3);
	BitWriter baseline, baselines;
	PropList(baseline, 1);
	Entry(baselines, "0", &baseline);
	CreateTable(stringTables, "instancebaseline", 1024, baselines, 1);
	PutFrame(demo, 1, 0, &stringTables);

	DataTables t = TestTables();                     // the Episode One layout
	BitWriter old;
	for (const SendTable& table : t.tables) {
		old.Bit(true); old.Bit(table.needsDecoder); old.String(table.name);
		old.Bits((uint32_t)table.props.size(), 9);
		for (const SendProp& p : table.props) {
			const int type = p.type == PropType::Int ? 0 : p.type == PropType::Float ? 1 : p.type == PropType::Vector ? 2 : 4;
			old.Bits((uint32_t)type, 5); old.String(p.name); old.Bits(p.flags, 13);
			if (p.type == PropType::Array) old.Bits((uint32_t)p.elements, 10);
			else { old.Float(p.low); old.Float(p.high); old.Bits((uint32_t)p.bits, 6); }
		}
	}
	old.Bit(false);
	old.Bits(1, 16); old.Bits(0, 16); old.String("CTest"); old.String("DT_Test");
	PutFrame(demo, 6, 0, &old);
	PutFrame(demo, 3, 0, nullptr);

	BitWriter packet;
	packet.Bits(3, 5); packet.Signed(10, 32);         // net_Tick, v34: the tick only
	BitWriter entities;                                // entity 1 enters, class 0, model "*2"
	UBitVarEp1(entities, 1);
	entities.Bit(false); entities.Bit(true);
	entities.Bits(0, 1); entities.Bits(7, 10);
	PropList(entities, 2);
	packet.Bits(26, 5); packet.Bits(16, 11); packet.Bit(false); packet.Bit(false); packet.Bits(1, 11);
	packet.Bits((uint32_t)entities.Size(), 20); packet.Bit(false); packet.Append(entities);
	BitWriter death;
	death.Bits(23, 9); death.Bits(5, 16); death.Bits(6, 16);
	packet.Bits(25, 5); packet.Bits((uint32_t)death.Size(), 11); packet.Append(death);
	PutFrame(demo, 2, 10, &packet);
	PutFrame(demo, 7, 11, nullptr);
	return demo;
}

// The converted model precache, read from the output in the 2013 layout.
std::vector<std::string> OutputModels(const std::vector<unsigned char>& out) {
	std::vector<std::string> names;
	size_t at = 1072;
	while (at + 5 <= out.size()) {
		const int command = out[at];
		at += 5;
		if (command == 3 || command == 7) continue;
		if (command == 1 || command == 2) at += 84;
		int32_t length;
		std::memcpy(&length, &out[at], 4);
		at += 4;
		if (command == 1) {
			BitReader b(&out[at], (size_t)length);
			while (b.Left() >= 6) {
				const int type = (int)b.Bits(6);
				if (type != 12) break; // only the string table frame is read
				const std::string name = b.String();
				const int maxEntries = (int)b.Bits(16);
				const int count = (int)b.Bits(Log2((unsigned)maxEntries) + 1);
				const uint32_t bits = b.VarInt();
				if (b.Bit()) { b.Bits(12); b.Bits(4); }
				b.Bit();
				BitReader body = b.Sub(bits);
				if (name != "modelprecache") continue;
				for (int i = 0; i < count; ++i) {
					if (!body.Bit()) body.Bits(Log2((unsigned)maxEntries));
					std::string s;
					if (body.Bit()) { body.Bit(); s = body.String(); }
					if (body.Bit()) body.Skip((size_t)body.Bits(14) * 8);
					names.push_back(s);
				}
			}
		}
		at += (size_t)length;
	}
	return names;
}

void TestConversion() {
	const std::vector<unsigned char> demo = V34Demo();
	CHECK(IsV34Demo(demo.data(), demo.size()));
	CHECK(DemoMapName(demo.data(), demo.size()) == "de_test");

	// The installed map moved the brush the entity uses (*2) to *1.
	MapModels map;
	map.models.push_back(Model(-4096, -4096, -4096, 4096, 4096, 4096));
	map.models.push_back(Model(-7, -7, -7, 7, 7, 7));
	map.models.push_back(Model(100, 100, 100, 900, 900, 900));
	ConvertOptions options;
	options.map = &map;
	std::vector<unsigned char> out;
	const ConvertResult result = ConvertV34Demo(demo.data(), demo.size(), out, options);
	CHECK(result.ok);
	if (!result.ok) {
		std::printf("  error: %s\n", result.error.c_str());
		return;
	}
	CHECK(result.stats.mapMatched && result.stats.brushMoved == 1 && result.stats.brushMissing == 0);
	CHECK(result.stats.entityUpdates == 1 && result.stats.baselines == 1 && result.stats.classesOut == 1);
	int32_t protocol;
	std::memcpy(&protocol, &out[12], 4);
	CHECK(protocol == 24 && !IsV34Demo(out.data(), out.size()));

	const std::vector<std::string> models = OutputModels(out);
	CHECK(models.size() == 3);
	if (models.size() == 3) CHECK(models[1] == "*2" && models[2] == "*1");

	// The current-format reader walks every message of the result and finds the kill.
	const DemoFile::Scan scan = DemoFile::ScanDemo(out.data(), out.size(), { "player_death" });
	CHECK(scan.ok);
	if (!scan.ok) std::printf("  scan error: %s\n", scan.error.c_str());
	CHECK(scan.events.size() == 1 && scan.events[0].tick == 10 && scan.events[0].Int("userid") == 5);

	// Without the map the models stay as recorded.
	std::vector<unsigned char> plain;
	CHECK(ConvertV34Demo(demo.data(), demo.size(), plain).ok);
	const std::vector<std::string> recorded = OutputModels(plain);
	CHECK(recorded.size() == 3 && recorded[2] == "*2");

	// A demo cut inside its last packet still converts, closed at the cut.
	std::vector<unsigned char> cut(demo.begin(), demo.end() - 9);
	std::vector<unsigned char> cutOut;
	const ConvertResult partial = ConvertV34Demo(cut.data(), cut.size(), cutOut);
	CHECK(partial.ok && !partial.stats.stoppedEarly.empty());
	std::vector<unsigned char> notOld(cut.begin(), cut.end());
	notOld[12] = 24;
	CHECK(!ConvertV34Demo(notOld.data(), notOld.size(), cutOut).ok);
}

} // namespace

int main() {
	TestBits();
	TestTablesRoundTrip();
	TestMatching();
	TestConversion();
	if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
	std::printf("SourceDemoConvertTests passed\n");
	return 0;
}
