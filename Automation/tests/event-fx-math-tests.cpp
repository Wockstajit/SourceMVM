#include "../../AfxHookSource2/Filmmaker/EventFx/EventFxMath.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <set>

using namespace Filmmaker::EventFx;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

// Every catalog entry has a unique key, a label, and only real particle paths.
void TestCatalog() {
	std::set<std::string> keys;
	for (const EffectDef& e : kEffects) {
		CHECK(keys.insert(e.key).second);
		CHECK(e.label && *e.label);
		CHECK(e.life > 0.0f);
		CHECK(e.systems[0] != nullptr);
		for (const char* s : e.systems) {
			if (!s) continue;
			const size_t n = std::strlen(s);
			CHECK(std::strncmp(s, "particles/", 10) == 0);
			CHECK(n > 5 && std::strcmp(s + n - 5, ".vpcf") == 0);
		}
	}
	CHECK(FindEffect("he") && FindEffect("blood") && FindEffect("c4"));
	CHECK(FindEffect("nope") == nullptr);
}

// The dropdown's templates spawn real effects, each on its own event, and every one is ready to play
// once picked (the scene starts with none, so nothing plays until a template is chosen).
void TestTemplates() {
	const auto presets = Templates();
	CHECK(presets.size() >= 6);
	std::set<int> ids;
	for (const EventPreset& p : presets) {
		CHECK(ids.insert(p.id).second);
		CHECK(p.enabled);
		int effects = 0;
		for (const std::string& e : p.effects) if (!e.empty()) { CHECK(FindEffect(e) != nullptr); ++effects; }
		CHECK(effects >= 1);
	}
	CHECK(presets[0].trigger == TriggerKind::HeGrenadePop);
	CHECK(presets[0].where == Where::AtEvent);
	CHECK(!SystemsOf(presets).empty());
}

// One firing: every effect of every copy, at the point plus the lift, after the delay, kept for its life.
void TestPlanSpawns() {
	EventPreset p = MakePreset(1, "t", TriggerKind::BombExplode, "c4", "blood", "");
	p.lift = 10.0f; p.delay = 0.5f;
	const float at[3] = { 100, 200, 300 };
	auto spawns = PlanSpawns(p, at, 50.0, 7);
	CHECK(spawns.size() == 2);
	if (spawns.size() == 2) {
		CHECK(spawns[0].system == FindEffect("c4")->systems[0]);
		CHECK(spawns[0].pos[0] == 100 && spawns[0].pos[1] == 200 && spawns[0].pos[2] == 310);
		CHECK(std::fabs(spawns[0].start - 50.5) < 1e-9);
		CHECK(spawns[0].life == FindEffect("c4")->life);
		CHECK(spawns[1].life == FindEffect("blood")->life);
	}
	// Copies scatter on a ring and start one after another; a set life overrides the default.
	p.copies = 4; p.spread = 100.0f; p.life = 2.0f; p.effects[1] = "";
	spawns = PlanSpawns(p, at, 0.0, 7);
	CHECK(spawns.size() == 4);
	for (size_t i = 1; i < spawns.size(); ++i) {
		const float dx = spawns[i].pos[0] - at[0], dy = spawns[i].pos[1] - at[1];
		const float r = std::sqrt(dx * dx + dy * dy);
		CHECK(r > 50.0f && r <= 100.01f);
		CHECK(spawns[i].start > spawns[i - 1].start);
		CHECK(spawns[i].life == 2.0);
	}
	// Never more than the cap, however many copies and effects.
	p.copies = 99; p.effects[0] = "c4"; p.effects[1] = "he"; p.effects[2] = "blood";
	CHECK(PlanSpawns(p, at, 0.0, 1).size() == (size_t)std::min(kMaxSpawnsPerFire, kMaxCopies * 3));
	// An unknown or empty effect spawns nothing.
	EventPreset none = MakePreset(2, "n", TriggerKind::Kill, "", "missing", nullptr);
	CHECK(PlanSpawns(none, at, 0.0, 1).empty());
}

void TestShake() {
	CHECK(EvaluateEnvelope(-0.1, kShakeAttack, 0.7f) == 0.0f);
	CHECK(std::fabs(EvaluateEnvelope(kShakeAttack, kShakeAttack, 0.7f) - 1.0f) < 1e-4f);
	CHECK(EvaluateEnvelope(2.0, kShakeAttack, 0.7f) == 0.0f);
	// Auto: a light shake is slow and short, a hard one quick and long; a choice overrides it.
	CHECK(ShakeHz(0.1f, 0.0f) < ShakeHz(0.9f, 0.0f));
	CHECK(ShakeSeconds(0.1f, 0.0f) < ShakeSeconds(0.9f, 0.0f));
	CHECK(ShakeHz(0.9f, 4.0f) == 4.0f && ShakeSeconds(0.1f, 2.5f) == 2.5f);
	CHECK(ShakeHz(1.0f, 0.0f) <= 14.0f && ShakeHz(0.0f, 0.0f) >= 4.0f);
	CHECK(ShakeFalloff(0.0f) == 1.0f);
	CHECK(ShakeFalloff(1250.0f) > 0.45f && ShakeFalloff(1250.0f) < 0.55f);
	CHECK(ShakeFalloff(99999.0f) == 0.2f);
	ActiveShake s; s.start = 10.0; s.strength = 1.0f; s.seed = 3;
	ShakeFrame f = EvaluateShakes({ s }, 10.1, 0.0);
	CHECK(f.active);
	CHECK(std::fabs(f.pitch) + std::fabs(f.yaw) + std::fabs(f.roll) > 0.1f);
	CHECK(!EvaluateShakes({ s }, 12.0, 0.0).active);
	// A long shake is still going where a short one has stopped.
	ActiveShake longer = s; longer.seconds = 2.5f;
	CHECK(!EvaluateShakes({ s }, 11.0, 0.0).active);
	CHECK(EvaluateShakes({ longer }, 11.0, 0.0).active);
	s.strength = 0.0f;
	CHECK(!EvaluateShakes({ s }, 10.1, 0.0).active);
}

void TestTriggerTokens() {
	for (int i = 0; i < (int)TriggerKind::Count; ++i) {
		TriggerKind back = TriggerKind::Count;
		CHECK(TriggerFromToken(TriggerToken((TriggerKind)i), back) && back == (TriggerKind)i);
	}
}

} // namespace

int main() {
	TestCatalog();
	TestTemplates();
	TestPlanSpawns();
	TestShake();
	TestTriggerTokens();
	if (g_failed) {
		std::cerr << g_failed << " EventFxMathTests failed\n";
		return 1;
	}
	return 0;
}
