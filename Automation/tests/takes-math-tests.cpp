// Bot takes, engine-free: the command rewrite that turns a player's input into a bot's, the scene
// clock, and the file format. Live behaviour needs a private match; see Docs/features/bot-takes.md.

#include "../../AfxHookSource2/Filmmaker/Takes/TakesMath.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Filmmaker::Takes;

static int g_failures = 0;
#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                               \
		}                                                               \
	} while (0)

static bool Near(float a, float b, float eps = 0.0001f) { return std::fabs(a - b) <= eps; }

// A command as the game's client would send it, with the fields a bot must not inherit.
static std::vector<uint8_t> SampleCmd() {
	std::vector<uint8_t> angles;
	WriteFloatField(angles, 1, -12.5f);
	WriteFloatField(angles, 2, 90.25f);
	WriteFloatField(angles, 3, 0.0f);
	std::vector<uint8_t> buttons;
	WriteTag(buttons, Field::kButtonState1, WireVarint);
	WriteVarint(buttons, kInAttack | (1ull << 3));
	std::vector<uint8_t> base;
	WriteInt32Field(base, Field::kLegacyCommandNumber, 777);
	WriteInt32Field(base, Field::kClientTick, 5000);
	WriteUint32Field(base, Field::kPredictionOffset, 256);
	WriteBytesField(base, Field::kButtons, buttons.data(), buttons.size());
	WriteBytesField(base, Field::kViewAngles, angles.data(), angles.size());
	WriteFloatField(base, Field::kForward, 1.0f);
	WriteFloatField(base, Field::kLeft, -0.5f);
	WriteInt32Field(base, Field::kWeaponSelect, 123456);
	WriteUint32Field(base, Field::kPawnHandle, 999);
	const uint8_t crc[3] = {1, 2, 3};
	WriteBytesField(base, Field::kMoveCrc, crc, 3);
	WriteUint32Field(base, Field::kConsumedAngleChanges, 4);
	// One subtick step (field 18): it must come through untouched.
	std::vector<uint8_t> step;
	WriteTag(step, 1, WireVarint);
	WriteVarint(step, 2);
	WriteFloatField(step, 3, 0.5f);
	WriteBytesField(base, 18, step.data(), step.size());

	std::vector<uint8_t> entry;
	WriteBytesField(entry, 2, angles.data(), angles.size());
	WriteInt32Field(entry, 4, 4999);
	WriteInt32Field(entry, Field::kHistoryTargetEnt, 7);
	const float shoot[3] = {1, 2, 3};
	WriteVectorMessage(entry, Field::kHistoryShootPos, shoot);
	WriteVectorMessage(entry, Field::kHistoryTargetHead, shoot);

	std::vector<uint8_t> cmd;
	WriteBytesField(cmd, Field::kBase, base.data(), base.size());
	WriteBytesField(cmd, Field::kInputHistory, entry.data(), entry.size());
	WriteInt32Field(cmd, 6, 0); // attack1_start_history_index
	WriteTag(cmd, Field::kPredictBodyFx, WireVarint);
	WriteVarint(cmd, 1);
	return cmd;
}

// The field of a message, or empty.
static std::vector<WireField> Find(const uint8_t *data, size_t size, uint32_t number) {
	std::vector<WireField> out;
	ForEachField(data, size, [&](const WireField &f) {
		if (f.number == number) out.push_back(f);
	});
	return out;
}

static void TestVarints() {
	for (uint64_t v : {uint64_t(0), uint64_t(1), uint64_t(127), uint64_t(128), uint64_t(300), uint64_t(0xffffffffu), ~uint64_t(0)}) {
		std::vector<uint8_t> bytes;
		WriteVarint(bytes, v);
		const uint8_t *p = bytes.data();
		uint64_t out = 0;
		CHECK(ReadVarint(p, bytes.data() + bytes.size(), out));
		CHECK(out == v);
		CHECK(p == bytes.data() + bytes.size());
	}
	// Cut short: no terminating byte.
	const uint8_t cut[2] = {0x80, 0x80};
	const uint8_t *p = cut;
	uint64_t out;
	CHECK(!ReadVarint(p, cut + 2, out));
}

static void TestSummary() {
	const std::vector<uint8_t> cmd = SampleCmd();
	const CmdSummary s = SummarizeCmd(cmd.data(), cmd.size());
	CHECK(s.valid);
	CHECK(s.clientTick == 5000);
	CHECK(s.weaponSelect == 123456);
	CHECK(Near(s.pitch, -12.5f));
	CHECK(Near(s.yaw, 90.25f));
	CHECK(Near(s.forward, 1.0f));
	CHECK(Near(s.left, -0.5f));
	CHECK(s.buttons == (kInAttack | (1ull << 3)));
	CHECK(s.subtickMoves == 1);
	CHECK(s.historyEntries == 1);
	const uint8_t junk[3] = {0x0a, 0xff, 0x01};
	CHECK(!SummarizeCmd(junk, 3).valid);
}

static void TestRewriteKeepsThePerformance() {
	const std::vector<uint8_t> cmd = SampleCmd();
	CmdRewrite r;
	r.clientTick = 42;
	r.pawnHandle = 0x1234;
	r.weaponSelect = 0x5678;
	const std::vector<uint8_t> out = RewriteCmdForBot(cmd.data(), cmd.size(), r);
	CHECK(!out.empty());
	const CmdSummary s = SummarizeCmd(out.data(), out.size());
	CHECK(s.valid);
	// The bot's tick and weapon, the player's input.
	CHECK(s.clientTick == 42);
	CHECK(s.weaponSelect == 0x5678);
	CHECK(Near(s.pitch, -12.5f));
	CHECK(Near(s.yaw, 90.25f));
	CHECK(Near(s.forward, 1.0f));
	CHECK(Near(s.left, -0.5f));
	CHECK(s.buttons == (kInAttack | (1ull << 3)));
	CHECK(s.subtickMoves == 1);
	CHECK(s.historyEntries == 1);

	const auto bases = Find(out.data(), out.size(), Field::kBase);
	CHECK(bases.size() == 1);
	const WireField &base = bases[0];
	CHECK(Find(base.data, base.size, Field::kClientTick).size() == 1);
	CHECK(Find(base.data, base.size, Field::kLegacyCommandNumber).size() == 1);
	CHECK(Find(base.data, base.size, Field::kWeaponSelect).size() == 1);
	CHECK(Find(base.data, base.size, Field::kPawnHandle)[0].varint == 0x1234);
	// Client bookkeeping the bot never had.
	CHECK(Find(base.data, base.size, Field::kMoveCrc).empty());
	CHECK(Find(base.data, base.size, Field::kConsumedAngleChanges).empty());
	CHECK(Find(base.data, base.size, Field::kPredictionOffset).empty());
	CHECK(Find(out.data(), out.size(), Field::kPredictBodyFx).empty());
}

static void TestRewriteHistory() {
	const std::vector<uint8_t> cmd = SampleCmd();
	CmdRewrite r;
	r.hasEye = true;
	r.eye[0] = 10;
	r.eye[1] = 20;
	r.eye[2] = 30;
	const std::vector<uint8_t> out = RewriteCmdForBot(cmd.data(), cmd.size(), r);
	const auto entries = Find(out.data(), out.size(), Field::kInputHistory);
	CHECK(entries.size() == 1);
	const WireField &entry = entries[0];
	CHECK(Find(entry.data, entry.size, Field::kHistoryTargetEnt).empty());
	CHECK(Find(entry.data, entry.size, Field::kHistoryTargetHead).empty());
	CHECK(Find(entry.data, entry.size, 4).size() == 1); // the render tick stays
	const auto shoot = Find(entry.data, entry.size, Field::kHistoryShootPos);
	CHECK(shoot.size() == 1); // replaced, not added to
	const auto x = Find(shoot[0].data, shoot[0].size, 1);
	const auto z = Find(shoot[0].data, shoot[0].size, 3);
	CHECK(Near(FixedToFloat(x[0].fixed32), 10.0f));
	CHECK(Near(FixedToFloat(z[0].fixed32), 30.0f));

	// Without an eye the recorded shoot position stands.
	CmdRewrite keep;
	const std::vector<uint8_t> kept = RewriteCmdForBot(cmd.data(), cmd.size(), keep);
	const auto keptEntry = Find(kept.data(), kept.size(), Field::kInputHistory);
	const auto keptShoot = Find(keptEntry[0].data, keptEntry[0].size, Field::kHistoryShootPos);
	CHECK(Near(FixedToFloat(Find(keptShoot[0].data, keptShoot[0].size, 1)[0].fixed32), 1.0f));
}

static void TestHoldFire() {
	const std::vector<uint8_t> cmd = SampleCmd();
	CmdRewrite r;
	r.stripAttack = true;
	const std::vector<uint8_t> out = RewriteCmdForBot(cmd.data(), cmd.size(), r);
	const CmdSummary s = SummarizeCmd(out.data(), out.size());
	CHECK((s.buttons & kInAttack) == 0);
	CHECK((s.buttons & (1ull << 3)) != 0); // walking forward still walks
}

static void TestNoSwitchMeansNoWeaponField() {
	const std::vector<uint8_t> cmd = SampleCmd();
	CmdRewrite r;
	const std::vector<uint8_t> out = RewriteCmdForBot(cmd.data(), cmd.size(), r);
	CHECK(SummarizeCmd(out.data(), out.size()).weaponSelect == 0);
}

static void TestBadInputGivesNothing() {
	const uint8_t junk[3] = {0x0a, 0xff, 0x01};
	CHECK(RewriteCmdForBot(junk, 3, CmdRewrite{}).empty());
}

static void TestIdleCmd() {
	const std::vector<uint8_t> idle = BuildIdleCmd(77, 0x99, 5.0f, -170.0f);
	const CmdSummary s = SummarizeCmd(idle.data(), idle.size());
	CHECK(s.valid);
	CHECK(s.clientTick == 77);
	CHECK(s.buttons == 0);
	CHECK(Near(s.pitch, 5.0f));
	CHECK(Near(s.yaw, -170.0f));
	CHECK(s.forward == 0.0f);
}

static Track MakeTrack(const char *name, int ticks, int delay = 0) {
	Track t;
	t.name = name;
	t.delayTicks = delay;
	t.ticks.resize(ticks);
	return t;
}

static void TestClock() {
	Scene scene;
	CHECK(SceneLengthTicks(scene) == 0);
	scene.tracks.push_back(MakeTrack("a", 100));
	scene.tracks.push_back(MakeTrack("b", 50, 80));
	CHECK(SceneLengthTicks(scene) == 130);
	scene.tracks[1].enabled = false;
	CHECK(SceneLengthTicks(scene) == 100);

	const Track &late = scene.tracks[1];
	CHECK(FrameForSceneTick(late, 0).kind == FrameKind::BeforeStart);
	CHECK(FrameForSceneTick(late, 79).kind == FrameKind::BeforeStart);
	TrackFrame f = FrameForSceneTick(late, 80);
	CHECK(f.kind == FrameKind::Playing && f.index == 0);
	f = FrameForSceneTick(late, 129);
	CHECK(f.kind == FrameKind::Playing && f.index == 49);
	f = FrameForSceneTick(late, 130);
	CHECK(f.kind == FrameKind::Finished && f.index == 49);
}

static void TestDrift() {
	const float a[3] = {0, 0, 0}, b[3] = {3, 4, 0};
	CHECK(Near(Distance3(a, b), 5.0f));
	CHECK(ShouldCorrectDrift(30.0f, 24.0f));
	CHECK(!ShouldCorrectDrift(10.0f, 24.0f));
	CHECK(!ShouldCorrectDrift(1000.0f, 0.0f)); // off means off
}

static void TestTeleportDetection() {
	const float still[3] = {0, 0, 0}, run[3] = {250, 0, 0}, fall[3] = {0, 0, -1000};
	const float a[3] = {0, 0, 0};
	// A normal step while running is not a teleport.
	const float step[3] = {3.9f, 0, 0};
	CHECK(!MovedWithoutInput(a, run, step, run, 64));
	// Walking up a stair: the 18-unit snap is ordinary movement.
	const float stair[3] = {3.9f, 0, 18};
	CHECK(!MovedWithoutInput(a, run, stair, run, 64));
	// Falling fast covers a lot of height in a tick.
	const float fell[3] = {0, 0, -16};
	CHECK(!MovedWithoutInput(a, fall, fell, fall, 64));
	// Standing still and arriving 42 units away is not walking, however short the hop.
	const float hop[3] = {30, 30, 0};
	CHECK(MovedWithoutInput(a, still, hop, still, 64));
	const float far[3] = {1000, 0, 0};
	CHECK(MovedWithoutInput(a, run, far, run, 64));
	const float up[3] = {0, 0, 100};
	CHECK(MovedWithoutInput(a, still, up, still, 64));
}

static void TestNames() {
	std::vector<Track> tracks;
	CHECK(NextTakeName(tracks) == "Take 1");
	tracks.push_back(MakeTrack("Take 2", 1));
	CHECK(NextTakeName(tracks) == "Take 3");
	tracks.clear();
	tracks.push_back(MakeTrack("Take 2", 1));
	tracks.push_back(MakeTrack("x", 1));
	CHECK(NextTakeName(tracks) == "Take 3");
	CHECK(SafeFileName("a/b:c") == "a_b_c");
	CHECK(SafeFileName("../..") == "_____" || !SafeFileName("../..").empty());
	CHECK(SafeFileName("") == "scene");
	CHECK(SafeFileName("trailing ") == "trailing");
}

static void TestPerformerSteamId() {
	CHECK(PerformerSteamId("Take 1") == PerformerSteamId("Take 1"));
	CHECK(PerformerSteamId("Take 1") != PerformerSteamId("Take 2"));
	// Reads as an individual Steam account, so nothing treats it as "no id".
	CHECK((PerformerSteamId("Take 1") >> 32) == 0x01100001ull);
	CHECK(PerformerSteamId("") != 0);
}

static void TestSwaps() {
	Track t;
	t.weaponSwaps.push_back({"weapon_ak47", "weapon_m4a1"});
	CHECK(SwappedWeapon(t, "weapon_ak47") == "weapon_m4a1");
	CHECK(SwappedWeapon(t, "weapon_knife") == "weapon_knife");
}

static void TestInternName() {
	std::vector<std::string> names;
	CHECK(InternName(names, "a") == 0);
	CHECK(InternName(names, "b") == 1);
	CHECK(InternName(names, "a") == 0);
	CHECK(names.size() == 2);
}

static Scene MakeScene() {
	Scene s;
	s.map = "de_dust2";
	Track t = MakeTrack("Entry", 2, 16);
	t.team = 2;
	t.holdFire = true;
	t.armor = 100;
	t.helmet = 1;
	t.names = {"weapon_ak47", "weapon_knife"};
	t.startWeapons = {0, 1};
	t.inventory.push_back({5, 0, 1});
	t.cmds = {1, 2, 3, 4, 5};
	t.ticks[0].cmdSize = 2;
	t.ticks[1].cmdOffset = 2;
	t.ticks[1].cmdSize = 3;
	t.ticks[1].pos[2] = 64.0f;
	t.ticks[1].activeWeapon = 1;
	t.agentModel = "characters/models/tm_phoenix/tm_phoenix.vmdl";
	t.weaponSwaps.push_back({"weapon_ak47", "weapon_m4a1"});
	s.tracks.push_back(t);
	SceneObject o;
	o.name = "mvm_obj_1";
	o.model = "models/props/box.vmdl";
	o.pos[0] = 100;
	o.ang[1] = 45;
	o.scale = 2;
	o.solid = false;
	s.objects.push_back(o);
	return s;
}

static void TestSceneRoundTrip() {
	const Scene in = MakeScene();
	const std::vector<uint8_t> bytes = SerializeScene(in);
	Scene out;
	CHECK(ParseScene(bytes.data(), bytes.size(), out));
	CHECK(out.map == "de_dust2");
	CHECK(out.tracks.size() == 1);
	const Track &t = out.tracks[0];
	CHECK(t.name == "Entry");
	CHECK(t.team == 2);
	CHECK(t.holdFire);
	CHECK(t.delayTicks == 16);
	CHECK(t.armor == 100 && t.helmet == 1);
	CHECK(t.names.size() == 2 && t.names[1] == "weapon_knife");
	CHECK(t.startWeapons.size() == 2);
	CHECK(t.inventory.size() == 1 && t.inventory[0].tick == 5);
	CHECK(t.ticks.size() == 2);
	CHECK(t.ticks[1].cmdOffset == 2 && t.ticks[1].cmdSize == 3);
	CHECK(Near(t.ticks[1].pos[2], 64.0f));
	CHECK(t.ticks[1].activeWeapon == 1);
	CHECK(t.cmds.size() == 5);
	CHECK(t.agentModel == in.tracks[0].agentModel);
	CHECK(t.weaponSwaps.size() == 1 && t.weaponSwaps[0].to == "weapon_m4a1");
	CHECK(out.objects.size() == 1);
	CHECK(out.objects[0].name == "mvm_obj_1" && !out.objects[0].solid);
	CHECK(Near(out.objects[0].ang[1], 45.0f));
}

static void TestSceneRejectsDamage() {
	const std::vector<uint8_t> good = SerializeScene(MakeScene());
	Scene kept;
	kept.map = "untouched";
	// Every cut-off file is refused, and a refused load changes nothing.
	for (size_t n = 0; n < good.size(); ++n) CHECK(!ParseScene(good.data(), n, kept));
	CHECK(kept.map == "untouched");
	std::vector<uint8_t> extra = good;
	extra.push_back(0);
	CHECK(!ParseScene(extra.data(), extra.size(), kept));
	std::vector<uint8_t> magic = good;
	magic[0] ^= 0xff;
	CHECK(!ParseScene(magic.data(), magic.size(), kept));
	std::vector<uint8_t> version = good;
	version[4] = 99;
	CHECK(!ParseScene(version.data(), version.size(), kept));
	// A tick pointing past the commands would have the bot read past the buffer.
	Scene bad = MakeScene();
	bad.tracks[0].ticks[1].cmdSize = 1000;
	const std::vector<uint8_t> badBytes = SerializeScene(bad);
	CHECK(!ParseScene(badBytes.data(), badBytes.size(), kept));
}

int main() {
	TestVarints();
	TestSummary();
	TestRewriteKeepsThePerformance();
	TestRewriteHistory();
	TestHoldFire();
	TestNoSwitchMeansNoWeaponField();
	TestBadInputGivesNothing();
	TestIdleCmd();
	TestClock();
	TestDrift();
	TestTeleportDetection();
	TestNames();
	TestSwaps();
	TestPerformerSteamId();
	TestInternName();
	TestSceneRoundTrip();
	TestSceneRejectsDamage();
	if (g_failures) {
		std::printf("takes-math-tests: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::printf("takes-math-tests: all passed\n");
	return EXIT_SUCCESS;
}
