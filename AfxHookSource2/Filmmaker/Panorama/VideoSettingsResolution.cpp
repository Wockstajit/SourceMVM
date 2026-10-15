#include "VideoSettingsResolution.h"

#include "PanoramaBridge.h"
#include "VideoSettingsResolutionJs.h"
#include "../Config/FilmmakerSettingsStore.h"

#include <string>

namespace Filmmaker {
namespace {

PanoramaBridge g_bridge;
void* g_context = nullptr;
unsigned g_refreshFrames = 0;

std::string QuoteJs(const std::string& value) {
	std::string out = "\"";
	for (const char ch : value) {
		switch (ch) {
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		default: out += ch; break;
		}
	}
	out += '"';
	return out;
}

} // namespace

void VideoSettingsResolution_RunMainThreadFrame() {
	g_bridge.Init();
	void* context = g_bridge.ContextPanel();
	if (!context || !g_bridge.CanRunScript()) {
		g_context = nullptr;
		g_refreshFrames = 0;
		return;
	}

	const bool contextChanged = context != g_context;
	g_context = context;
	++g_refreshFrames;
	if (!contextChanged && (g_refreshFrames % 120) != 0)
		return;

	// The script is idempotent. Re-running it occasionally repairs a Panorama context reload that
	// preserved the native panel pointer but discarded the JavaScript global.
	if (!g_bridge.RunScript(kVideoSettingsResolutionJs))
		return;

	const std::string saved = FilmmakerSettings_UiString("video.customResolutions", "");
	g_bridge.RunScript("$.MvmResolutionSettings && $.MvmResolutionSettings.setPresets(" + QuoteJs(saved) + ");");
}

} // namespace Filmmaker
