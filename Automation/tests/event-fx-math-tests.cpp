#include "../../AfxHookSource2/Filmmaker/EventFx/EventFxMath.h"

#include <cmath>
#include <iostream>

using namespace Filmmaker::EventFx;

namespace {

int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

void TestEnvelopeShape() {
	CHECK(EvaluateEnvelope(-0.1, 0.05f, 0.5f) == 0.0f);
	CHECK(std::fabs(EvaluateEnvelope(0.05, 0.05f, 0.5f) - 1.0f) < 1e-4f);
	const float mid = EvaluateEnvelope(0.30, 0.05f, 0.5f);
	CHECK(mid > 0.2f && mid < 0.3f); // quadratic falloff: (0.5)^2 = 0.25
	CHECK(EvaluateEnvelope(0.60, 0.05f, 0.5f) == 0.0f);
}

void TestFlashbangPresetMultiActionCue() {
	auto presets = DefaultPresets();
	CHECK(presets.size() >= 5);
	const EventPreset& flash = presets[0];
	CHECK(flash.trigger == TriggerKind::FlashbangPop);
	CHECK(flash.lightBurst);

	ActiveCue cue{ flash, 10.0, 42 };
	EvaluatedFrame peak = EvaluateActiveCues({ cue }, 10.0 + flash.attackSec);
	CHECK(peak.active);
	CHECK(peak.exposureEv > 2.0f);
	CHECK(peak.fovDelta > 3.0f);
	CHECK(peak.chromatic > 0.3f);

	EvaluatedFrame done = EvaluateActiveCues({ cue }, 12.0);
	CHECK(!done.active);
	CHECK(done.exposureEv == 0.0f);
}

void TestBakeExposureKeys() {
	auto presets = DefaultPresets();
	auto keys = BuildBakedExposureKeys(presets[0], 1000.0, 64.0);
	CHECK(keys.size() == 4);
	CHECK(keys.front().tick == 1000.0 && std::fabs(keys.front().exposureScale - 1.0) < 1e-6);
	CHECK(keys[1].exposureScale > 2.0);
	CHECK(std::fabs(keys.back().exposureScale - 1.0) < 1e-6);
}

} // namespace

int main() {
	TestEnvelopeShape();
	TestFlashbangPresetMultiActionCue();
	TestBakeExposureKeys();
	if (g_failed) {
		std::cerr << g_failed << " EventFxMathTests failed\n";
		return 1;
	}
	return 0;
}
