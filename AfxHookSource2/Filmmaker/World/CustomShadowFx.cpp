#include "CustomShadowFx.h"

#include "CustomShadowAtlas.h"
#include "../Debug/MvmDebugLog.h"
#include "../../ContactShadowPass.h"
#include "../../../Shared/AfxConsole.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace Filmmaker {
namespace {

constexpr float kDefaultStrength = 0.65f;
constexpr float kDefaultStepPixels = 2.5f;
constexpr float kDefaultThickness = 0.0008f;
constexpr int kDefaultSteps = 16;

std::atomic<bool> g_enabled{ false };
std::atomic<float> g_strength{ kDefaultStrength };
std::atomic<float> g_stepPixels{ kDefaultStepPixels };
std::atomic<float> g_thickness{ kDefaultThickness };
std::atomic<int> g_steps{ kDefaultSteps };
std::atomic<unsigned long long> g_diagFrames{ 0 };

void Publish() {
	AfxContactShadowRequest request;
	request.active = g_enabled.load(std::memory_order_relaxed);
	request.strength = g_strength.load(std::memory_order_relaxed);
	request.stepPixels = g_stepPixels.load(std::memory_order_relaxed);
	request.thickness = g_thickness.load(std::memory_order_relaxed);
	request.steps = g_steps.load(std::memory_order_relaxed);
	AfxContactShadow::SetRequest(request);
}

} // namespace

bool CustomShadowFx_Enabled() {
	return g_enabled.load(std::memory_order_relaxed);
}

float CustomShadowFx_Strength() {
	return g_strength.load(std::memory_order_relaxed);
}

void CustomShadowFx_SetStrength(float strength) {
	if (strength < 0.0f) strength = 0.0f;
	if (strength > 1.0f) strength = 1.0f;
	g_strength.store(strength, std::memory_order_relaxed);
	Publish();
	MvmDebugLog_LinefAlways("world.customshadow", "param=strength value=%.3f", strength);
}

float CustomShadowFx_StepPixels() {
	return g_stepPixels.load(std::memory_order_relaxed);
}

void CustomShadowFx_SetStepPixels(float stepPixels) {
	if (stepPixels < 0.25f) stepPixels = 0.25f;
	if (stepPixels > 16.0f) stepPixels = 16.0f;
	g_stepPixels.store(stepPixels, std::memory_order_relaxed);
	Publish();
	MvmDebugLog_LinefAlways("world.customshadow", "param=stepPixels value=%.3f", stepPixels);
}

float CustomShadowFx_Thickness() {
	return g_thickness.load(std::memory_order_relaxed);
}

void CustomShadowFx_SetThickness(float thickness) {
	if (thickness < 0.00001f) thickness = 0.00001f;
	if (thickness > 0.05f) thickness = 0.05f;
	g_thickness.store(thickness, std::memory_order_relaxed);
	Publish();
	MvmDebugLog_LinefAlways("world.customshadow", "param=thickness value=%.6f", thickness);
}

int CustomShadowFx_Steps() {
	return g_steps.load(std::memory_order_relaxed);
}

void CustomShadowFx_SetSteps(int steps) {
	if (steps < 1) steps = 1;
	if (steps > 32) steps = 32;
	g_steps.store(steps, std::memory_order_relaxed);
	Publish();
	MvmDebugLog_LinefAlways("world.customshadow", "param=steps value=%d", steps);
}

void CustomShadowFx_ResetSettings() {
	CustomShadowFx_SetStrength(kDefaultStrength);
	CustomShadowFx_SetStepPixels(kDefaultStepPixels);
	CustomShadowFx_SetThickness(kDefaultThickness);
	CustomShadowFx_SetSteps(kDefaultSteps);
}

void CustomShadowFx_SetEnabled(bool enabled) {
	g_enabled.store(enabled, std::memory_order_relaxed);
	CustomShadowAtlas_SetActive(enabled);
	CustomShadowAtlas_SetFillBorrow(enabled);
	Publish();
	MvmDebugLog_LinefAlways("world.customshadow",
		"enabled=%d strength=%.2f soft=%.2f bias=%.6f taps=%d note=phase-b-shadowmap atlasCaptures=%llu",
		enabled ? 1 : 0, CustomShadowFx_Strength(), CustomShadowFx_StepPixels(),
		CustomShadowFx_Thickness(), CustomShadowFx_Steps(),
		CustomShadowAtlas_CaptureCount());
}

void CustomShadowFx_OnDiagFrame() {
	++g_diagFrames;
}

void CustomShadowFx_AppendStateJson(std::ostringstream& o) {
	o << ",\"shaderShadows\":" << (CustomShadowFx_Enabled() ? "true" : "false");
	o << ",\"customShadowStub\":" << (CustomShadowFx_Enabled() ? "true" : "false");
	o << ",\"shaderShadowStrength\":" << CustomShadowFx_Strength();
	o << ",\"shaderShadowStepPixels\":" << CustomShadowFx_StepPixels();
	o << ",\"shaderShadowThickness\":" << CustomShadowFx_Thickness();
	o << ",\"shaderShadowSteps\":" << CustomShadowFx_Steps();
	o << ",\"customShadowDiagFrames\":" << g_diagFrames.load();
	o << ",\"customShadowAtlasCaptures\":" << CustomShadowAtlas_CaptureCount();
	o << ",\"customShadowHaveAtlas\":" << (CustomShadowAtlas_HaveCapture() ? "true" : "false");
	o << ",\"customShadowNote\":\"phase-b-shadowmap\"";
}

void CustomShadowFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* op = argc >= 4 ? args->ArgV(3) : "";
	if (0 == _stricmp(op, "on") || 0 == _stricmp(op, "enable")) {
		CustomShadowFx_SetEnabled(true);
		advancedfx::Message(
			"shader shadows ON (Phase B shadow map, strength=%.2f).\n",
			CustomShadowFx_Strength());
		return;
	}
	if (0 == _stricmp(op, "reset")) {
		CustomShadowFx_ResetSettings();
		CustomShadowFx_SetEnabled(false);
		advancedfx::Message("shader shadows reset and off.\n");
		return;
	}
	if (0 == _stricmp(op, "off") || 0 == _stricmp(op, "disable")) {
		CustomShadowFx_SetEnabled(false);
		advancedfx::Message("shader shadows off.\n");
		return;
	}
	if (0 == _stricmp(op, "toggle")) {
		CustomShadowFx_SetEnabled(!CustomShadowFx_Enabled());
		advancedfx::Message("shader shadows %s.\n",
			CustomShadowFx_Enabled() ? "ON" : "off");
		return;
	}
	if (0 == _stricmp(op, "strength")) {
		if (argc >= 5) CustomShadowFx_SetStrength((float)atof(args->ArgV(4)));
		advancedfx::Message("shader shadow strength = %.2f\n", CustomShadowFx_Strength());
		return;
	}
	if (0 == _stricmp(op, "step") || 0 == _stricmp(op, "step_pixels")
		|| 0 == _stricmp(op, "reach")) {
		if (argc >= 5) CustomShadowFx_SetStepPixels((float)atof(args->ArgV(4)));
		advancedfx::Message("shader shadow reach = %.2f px/step\n", CustomShadowFx_StepPixels());
		return;
	}
	if (0 == _stricmp(op, "thickness") || 0 == _stricmp(op, "bias")) {
		if (argc >= 5) CustomShadowFx_SetThickness((float)atof(args->ArgV(4)));
		advancedfx::Message("shader shadow depth bias = %.6f\n", CustomShadowFx_Thickness());
		return;
	}
	if (0 == _stricmp(op, "steps") || 0 == _stricmp(op, "samples")) {
		if (argc >= 5) CustomShadowFx_SetSteps(atoi(args->ArgV(4)));
		advancedfx::Message("shader shadow samples = %d\n", CustomShadowFx_Steps());
		return;
	}
	if (0 == _stricmp(op, "status")) {
		advancedfx::Message(
			"customshadow: %s strength=%.2f soft=%.2fpx bias=%.6f taps=%d frames=%llu "
			"ready=%d atlas=%d captures=%llu\n",
			CustomShadowFx_Enabled() ? "ON" : "off",
			CustomShadowFx_Strength(),
			CustomShadowFx_StepPixels(),
			CustomShadowFx_Thickness(),
			CustomShadowFx_Steps(),
			g_diagFrames.load(),
			AfxContactShadow::Pass() && AfxContactShadow::Pass()->Ready() ? 1 : 0,
			CustomShadowAtlas_HaveCapture() ? 1 : 0,
			CustomShadowAtlas_CaptureCount());
		return;
	}
	advancedfx::Message(
		"mirv_filmmaker world customshadow - Phase B directional shadow maps.\n"
		"Usage: %s world customshadow on|off|toggle|reset|status\n"
		"       %s world customshadow strength|reach|bias|steps <value>\n"
		"Current: %s strength=%.2f soft=%.2fpx bias=%.6f taps=%d.\n",
		cmd,
		cmd,
		CustomShadowFx_Enabled() ? "ON" : "off",
		CustomShadowFx_Strength(),
		CustomShadowFx_StepPixels(),
		CustomShadowFx_Thickness(),
		CustomShadowFx_Steps());
}

} // namespace Filmmaker
