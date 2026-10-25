#pragma once

// A finished take read back (its manifest.json, RenderManifest.cpp) as the settings a new job
// starts from: re-render a shot with one change, or add a pass for exactly the same In / Out.
// Engine-free, so the mapping is unit-tested (Automation/tests/render-take-import-math-tests.cpp).
//
// The manifest names some settings its own way (camera "thirdPerson", extension "holdFirst", the
// key colour as 0..1 floats). An import replays the same `mirv_filmmaker render ...` commands a
// person would type, so it speaks the console's tokens, and the order matters: picking the Camera
// Path source replaces a non-path range with the path's span, so the camera goes first and the
// range after it. Encoder settings are not in the manifest, so an import keeps the current ones.

#include "../Platform/JsonParser.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace Filmmaker {

struct RenderTakeSettings {
	std::string writtenAt;
	std::string status;
	std::string demo;
	int startTick = -1;
	int endTickExclusive = -1;
	int fps = 0;
	int framesWritten = 0;
	std::string format;
	std::string camera;    // console token, empty when unknown
	std::string extension; // console token, empty when unknown
	std::string hud;
	bool cameraData = true;
	bool blood = true;
	int key[3] = { 0, 255, 0 };
	double depthNear = 8.0;
	double depthFar = 4096.0;
	std::vector<std::pair<std::string, std::string>> passes; // (type, elements) tokens, render order
};

inline const char* RenderTakeCameraToken(const std::string& manifestName) {
	if (manifestName == "currentViewer") return "current";
	if (manifestName == "pov") return "pov";
	if (manifestName == "thirdPerson") return "thirdperson";
	if (manifestName == "freeCamera") return "free";
	if (manifestName == "cameraPath") return "path";
	if (manifestName == "followCamera") return "follow";
	return "";
}

inline const char* RenderTakeExtensionToken(const std::string& manifestName) {
	if (manifestName == "fail") return "fail";
	if (manifestName == "holdFirst") return "holdfirst";
	if (manifestName == "holdLast") return "holdlast";
	if (manifestName == "clamp") return "clamp";
	return "";
}

// False when the text is not a SOURCE:MVM take manifest with a usable range and at least one pass.
inline bool ParseRenderTakeManifest(const std::string& text, RenderTakeSettings& out) {
	JsonValue root;
	if (!JsonParse(text, root) || root.type != JsonValue::Type::Object) return false;
	const JsonValue* tool = root.Find("tool");
	if (!tool || tool->AsString() != "SOURCE:MVM render") return false;
	RenderTakeSettings t;
	auto str = [&](const char* k) { const JsonValue* v = root.Find(k); return v ? v->AsString() : std::string(); };
	auto num = [&](const char* k, double def) { const JsonValue* v = root.Find(k); return v ? v->AsNumber(def) : def; };
	auto flag = [&](const char* k, bool def) { const JsonValue* v = root.Find(k); return v ? v->AsBool(def) : def; };
	t.writtenAt = str("writtenAt");
	t.status = str("status");
	t.demo = str("demo");
	t.startTick = (int)num("startTick", -1);
	t.endTickExclusive = (int)num("endTickExclusive", -1);
	t.fps = (int)num("outputFps", 0);
	t.framesWritten = (int)num("framesWritten", 0);
	t.format = str("format");
	t.camera = RenderTakeCameraToken(str("cameraSource"));
	t.extension = RenderTakeExtensionToken(str("cameraPathExtension"));
	t.hud = str("hudMode");
	if (t.hud != "none" && t.hud != "game" && t.hud != "full") t.hud.clear();
	t.cameraData = flag("cameraDataSidecar", true);
	t.blood = flag("blood", true);
	if (const JsonValue* key = root.Find("keyColor"); key && key->type == JsonValue::Type::Array && key->arr.size() == 3) {
		for (int k = 0; k < 3; ++k) {
			const double c = key->arr[k].AsNumber(k == 1 ? 1.0 : 0.0);
			t.key[k] = (int)std::lround((c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c)) * 255.0);
		}
	}
	t.depthNear = num("depthNearUnits", t.depthNear);
	t.depthFar = num("depthFarUnits", t.depthFar);
	if (const JsonValue* streams = root.Find("streams"); streams && streams->type == JsonValue::Type::Array) {
		for (const JsonValue& s : streams->arr) {
			const JsonValue* type = s.Find("type");
			const JsonValue* elements = s.Find("elements");
			if (type && elements && !type->AsString().empty() && !elements->AsString().empty())
				t.passes.emplace_back(type->AsString(), elements->AsString());
		}
	}
	if (t.startTick < 0 || t.endTickExclusive <= t.startTick || t.passes.empty()) return false;
	out = std::move(t);
	return true;
}

// The `mirv_filmmaker render` commands (without that prefix) that rebuild the take's job.
inline std::vector<std::vector<std::string>> RenderTakeImportCommands(const RenderTakeSettings& t) {
	auto n = [](double v) {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%g", v);
		return std::string(buf);
	};
	std::vector<std::vector<std::string>> c;
	if (!t.camera.empty()) c.push_back({ "camera", t.camera });
	if (!t.extension.empty()) c.push_back({ "extension", t.extension });
	c.push_back({ "range", "custom", std::to_string(t.startTick), std::to_string(t.endTickExclusive) });
	if (t.fps > 0) c.push_back({ "output", "fps", std::to_string(t.fps) });
	if (!t.format.empty()) c.push_back({ "output", "format", t.format });
	c.push_back({ "pass", "clear" });
	for (const auto& p : t.passes) c.push_back({ "pass", "add", p.first, p.second });
	if (!t.hud.empty()) c.push_back({ "pass", "hud", t.hud });
	c.push_back({ "pass", "cameradata", t.cameraData ? "1" : "0" });
	c.push_back({ "pass", "blood", t.blood ? "1" : "0" });
	c.push_back({ "pass", "key", std::to_string(t.key[0]), std::to_string(t.key[1]), std::to_string(t.key[2]) });
	c.push_back({ "pass", "depth", n(t.depthNear), n(t.depthFar) });
	return c;
}

} // namespace Filmmaker
