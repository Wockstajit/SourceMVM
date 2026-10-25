#include "RenderTakeHistory.h"

#include "RenderCommand.h"
#include "RenderOutput.h"
#include "RenderTakeImportMath.h"

#include "../Filmmaker.h" // PlayingDemoPath
#include "../Debug/MvmDebugLog.h"
#include "../Demo/DemoProjectPath.h"
#include "../Demo/PlayingDemoPath.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/TextEncoding.h"

#include "../../../Shared/AfxConsole.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <vector>

namespace Filmmaker {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxManifestBytes = 512 * 1024;
constexpr int kMaxListed = 6;

struct Take {
	std::string folderUtf8;
	std::string name;
	std::wstring demoKey; // the take's demo as a project path, for matching the playing one
	RenderTakeSettings settings;
};

// The same spelling for one file however the engine or the manifest wrote it.
std::wstring DemoKey(const std::wstring& path) {
	std::wstring k = path;
	for (auto& c : k) c = (c == L'/') ? L'\\' : (wchar_t)std::towlower(c);
	return k;
}

std::vector<Take> Scan(std::wstring folder) {
	std::vector<Take> takes;
	std::error_code ec;
	if (folder.empty() || !fs::is_directory(folder, ec)) return takes;
	for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
		if (!it->is_directory(ec)) continue;
		const fs::path manifest = it->path() / L"manifest.json";
		const auto size = fs::file_size(manifest, ec);
		if (ec || size == 0 || size > kMaxManifestBytes) { ec.clear(); continue; }
		std::ifstream in(manifest, std::ios::binary);
		std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		Take t;
		if (!ParseRenderTakeManifest(text, t.settings)) continue;
		t.folderUtf8 = WideToUtf8(it->path().wstring());
		t.name = WideToUtf8(it->path().filename().wstring());
		t.demoKey = DemoKey(CanonicalDemoPath(ResolveDemoProjectPath(Utf8ToWide(t.settings.demo))));
		takes.push_back(std::move(t));
	}
	// Newest first: writtenAt is ISO 8601 local time, so it sorts as text.
	std::sort(takes.begin(), takes.end(),
		[](const Take& a, const Take& b) { return a.settings.writtenAt > b.settings.writtenAt; });
	return takes;
}

std::vector<Take> g_takes;
std::future<std::vector<Take>> g_scan;
bool g_rescan = false;      // a refresh arrived while a scan was running
std::wstring g_scannedFolder;
bool g_haveScanned = false;

void StartScan() {
	g_scannedFolder = RenderOutputSettingsRef().folder;
	g_haveScanned = true;
	g_scan = std::async(std::launch::async, Scan, g_scannedFolder);
}

// The playing demo's takes, newest first, as published.
std::vector<const Take*> Listed() {
	std::vector<const Take*> out;
	const std::wstring playing = DemoKey(PlayingDemoPath());
	if (playing.empty()) return out;
	for (const Take& t : g_takes) {
		if (t.demoKey != playing) continue;
		out.push_back(&t);
		if ((int)out.size() >= kMaxListed) break;
	}
	return out;
}

std::string Quote(const std::string& s) { return "\"" + JsonBuilder::Escape(s) + "\""; }

} // namespace

void RenderTakeHistory_Refresh() {
	if (g_scan.valid()) { g_rescan = true; return; }
	StartScan();
}

void RenderTakeHistory_PumpMainThread() {
	if (g_scan.valid() && g_scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		g_takes = g_scan.get();
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("render.takes", "scanned '%s': %d take(s)",
				WideToUtf8(g_scannedFolder).c_str(), (int)g_takes.size());
		if (g_rescan) { g_rescan = false; StartScan(); }
	}
	// A new output folder has its own takes.
	if (g_haveScanned && !g_scan.valid() && RenderOutputSettingsRef().folder != g_scannedFolder)
		StartScan();
}

void RenderTakeHistory_AppendJson(std::ostringstream& o) {
	const std::vector<const Take*> list = Listed();
	o << ",\"takes\":[";
	for (size_t i = 0; i < list.size(); ++i) {
		const RenderTakeSettings& s = list[i]->settings;
		o << (i ? "," : "") << "{\"name\":" << Quote(list[i]->name)
			<< ",\"when\":" << Quote(s.writtenAt)
			<< ",\"status\":" << Quote(s.status)
			<< ",\"start\":" << s.startTick
			<< ",\"end\":" << s.endTickExclusive
			<< ",\"fps\":" << s.fps
			<< ",\"frames\":" << s.framesWritten
			<< ",\"format\":" << Quote(s.format)
			<< ",\"camera\":" << Quote(s.camera)
			<< ",\"passes\":[";
		for (size_t p = 0; p < s.passes.size(); ++p)
			o << (p ? "," : "") << "[" << Quote(s.passes[p].first) << "," << Quote(s.passes[p].second) << "]";
		o << "]}";
	}
	o << "]";
}

bool RenderTakeHistory_Import(int index, std::string& outSummary, std::string& outError) {
	const std::vector<const Take*> list = Listed();
	if (index < 0 || index >= (int)list.size()) {
		outError = "no such take in this demo's list (run `mirv_filmmaker render takes`)";
		return false;
	}
	const Take& take = *list[index];
	// The same commands a person would type, through the same handlers, so an import can never
	// set a combination the workspace could not.
	for (const std::vector<std::string>& c : RenderTakeImportCommands(take.settings)) {
		advancedfx::CFakeCommandArgs args("mirv_filmmaker");
		args.AddArg("render");
		for (const std::string& token : c) args.AddArg(token.c_str());
		Render_RunCommand(args.ArgC(), &args, "mirv_filmmaker");
	}
	outSummary = take.name + ": ticks " + std::to_string(take.settings.startTick) + ".."
		+ std::to_string(take.settings.endTickExclusive - 1) + ", " + std::to_string(take.settings.passes.size())
		+ " pass(es)";
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.takes", "imported %s", outSummary.c_str());
	return true;
}

void RenderTakeHistory_PrintList(const char* cmd) {
	const std::vector<const Take*> list = Listed();
	advancedfx::Message("%s: %d earlier take(s) of this demo in '%s'%s\n", cmd, (int)list.size(),
		WideToUtf8(g_scannedFolder).c_str(), g_scan.valid() ? " (scanning)" : "");
	for (size_t i = 0; i < list.size(); ++i) {
		const RenderTakeSettings& s = list[i]->settings;
		advancedfx::Message("  %d  %s  %s  ticks %d..%d  %d fps %s  %d pass(es)\n", (int)i, s.writtenAt.c_str(),
			s.status.c_str(), s.startTick, s.endTickExclusive - 1, s.fps, s.format.c_str(), (int)s.passes.size());
	}
}

} // namespace Filmmaker
