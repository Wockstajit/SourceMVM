// Behavior tests for Render/RenderTakeImportMath.h: a finished take's manifest.json read back as the
// settings, and the console commands, that rebuild its job.
#include "../../AfxHookSource2/Filmmaker/Render/RenderTakeImportMath.h"

#include <iostream>
#include <string>

using namespace Filmmaker;

namespace {

int g_failed = 0;

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; ++g_failed; } } while (0)

// A manifest as RenderManifest.cpp writes it (trimmed of the fields an import does not read).
const char* kTake = R"({"tool":"SOURCE:MVM render","writtenAt":"2026-10-24T11:17:16","status":"completed",
 "demo":"F:\\demos\\match.dem","startTick":34800,"endTickExclusive":34832,"demoTickRate":64,"outputFps":30,
 "cameraSource":"thirdPerson","cameraPathExtension":"holdFirst","format":"tga","hudMode":"none",
 "cameraDataSidecar":false,"blood":true,
 "streams":[{"name":"beauty","type":"beauty","elements":"all","folder":"beauty\\everything"},
            {"name":"key_gun","type":"key","elements":"gun","folder":"key\\gun"},
            {"name":"depth_gun","type":"depth","elements":"gun","folder":"depth\\gun"}],
 "rendersPerFrame":2,"keyColor":[0,1,0.25],"depthNearUnits":0,"depthFarUnits":16384,"framesWritten":15})";

bool HasCommand(const std::vector<std::vector<std::string>>& c, const std::vector<std::string>& want, size_t* at = nullptr) {
	for (size_t i = 0; i < c.size(); ++i)
		if (c[i] == want) { if (at) *at = i; return true; }
	return false;
}

void ReadsATake() {
	RenderTakeSettings t;
	CHECK(ParseRenderTakeManifest(kTake, t));
	CHECK(t.startTick == 34800 && t.endTickExclusive == 34832);
	CHECK(t.fps == 30 && t.format == "tga" && t.status == "completed" && t.framesWritten == 15);
	// The manifest's own names become console tokens.
	CHECK(t.camera == "thirdperson");
	CHECK(t.extension == "holdfirst");
	CHECK(t.hud == "none" && !t.cameraData && t.blood);
	// The key colour comes back in the console's 0..255.
	CHECK(t.key[0] == 0 && t.key[1] == 255 && t.key[2] == 64);
	CHECK(t.depthNear == 0.0 && t.depthFar == 16384.0);
	CHECK(t.passes.size() == 3 && t.passes[1].first == "key" && t.passes[1].second == "gun");
}

void RebuildsTheJobInOrder() {
	RenderTakeSettings t;
	CHECK(ParseRenderTakeManifest(kTake, t));
	const auto c = RenderTakeImportCommands(t);
	size_t camera = 0, range = 0, clear = 0, beauty = 0, gun = 0;
	CHECK(HasCommand(c, { "camera", "thirdperson" }, &camera));
	CHECK(HasCommand(c, { "range", "custom", "34800", "34832" }, &range));
	// Picking the camera can replace the range, so the range is set after it.
	CHECK(camera < range);
	CHECK(HasCommand(c, { "extension", "holdfirst" }));
	CHECK(HasCommand(c, { "output", "fps", "30" }));
	CHECK(HasCommand(c, { "output", "format", "tga" }));
	CHECK(HasCommand(c, { "pass", "clear" }, &clear));
	CHECK(HasCommand(c, { "pass", "add", "beauty", "all" }, &beauty));
	CHECK(HasCommand(c, { "pass", "add", "key", "gun" }, &gun));
	CHECK(clear < beauty && beauty < gun);
	CHECK(HasCommand(c, { "pass", "hud", "none" }));
	CHECK(HasCommand(c, { "pass", "cameradata", "0" }));
	CHECK(HasCommand(c, { "pass", "blood", "1" }));
	CHECK(HasCommand(c, { "pass", "key", "0", "255", "64" }));
	CHECK(HasCommand(c, { "pass", "depth", "0", "16384" }));
}

void RefusesWhatIsNotATake() {
	RenderTakeSettings t;
	t.startTick = 7;
	CHECK(!ParseRenderTakeManifest("not json", t));
	CHECK(!ParseRenderTakeManifest(R"({"tool":"something else","startTick":1,"endTickExclusive":2,
		"streams":[{"type":"beauty","elements":"all"}]})", t));
	// No range, or no pass: nothing a job could be rebuilt from.
	CHECK(!ParseRenderTakeManifest(R"({"tool":"SOURCE:MVM render","startTick":100,"endTickExclusive":100,
		"streams":[{"type":"beauty","elements":"all"}]})", t));
	CHECK(!ParseRenderTakeManifest(R"({"tool":"SOURCE:MVM render","startTick":100,"endTickExclusive":200,"streams":[]})", t));
	// A refused manifest leaves the output untouched.
	CHECK(t.startTick == 7);
}

void UnknownNamesAreLeftOut() {
	RenderTakeSettings t;
	CHECK(ParseRenderTakeManifest(R"({"tool":"SOURCE:MVM render","startTick":10,"endTickExclusive":20,
		"cameraSource":"drone","cameraPathExtension":"bounce","hudMode":"some",
		"streams":[{"type":"beauty","elements":"all"}]})", t));
	CHECK(t.camera.empty() && t.extension.empty() && t.hud.empty());
	const auto c = RenderTakeImportCommands(t);
	for (const auto& cmd : c) CHECK(cmd[0] != "camera" && cmd[0] != "extension" && !(cmd[0] == "pass" && cmd[1] == "hud"));
	// Without an output fps or format the current ones stay.
	for (const auto& cmd : c) CHECK(cmd[0] != "output");
}

} // namespace

int main() {
	ReadsATake();
	RebuildsTheJobInOrder();
	RefusesWhatIsNotATake();
	UnknownNamesAreLeftOut();
	if (g_failed) {
		std::cerr << g_failed << " check(s) failed\n";
		return 1;
	}
	std::cout << "render take import math tests passed\n";
	return 0;
}
