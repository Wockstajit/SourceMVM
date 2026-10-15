#include "stdafx.h"

#include "EngineLogQuiet.h"

#include "../Shared/AfxConsole.h"

#include <Windows.h>

namespace {

// Source 2 LoggingVerbosity_t as shown by `log_verbosity` help: off, essential, default,
// detailed, max.
constexpr int kVerbosityOff = 0;

typedef int(__fastcall* LoggingSystem_FindChannel_t)(const char* name);
typedef void(__fastcall* LoggingSystem_SetChannelVerbosityByName_t)(const char* name, int verbosity);
typedef int(__fastcall* LoggingSystem_GetChannelVerbosity_t)(int channelId);

// Channels silenced, and why. Keep this list short and documented -- every entry costs
// diagnosability on that channel.
const char* const kQuietChannels[] = {
	// Per-frame warning spam when the window is sized to a non-native resolution (scratch
	// render-target pool mismatch): "Failed to allocate scratch rendertarget for
	// game.waterefects..." plus the paired RenderSystem line below.
	"SceneSystem",
	// "Attempting to render without calling BindRenderTargets!", same trigger.
	"RenderSystem",
};

bool g_done = false;

} // namespace

void Filmmaker::EngineLogQuiet_RunMainThreadFrame() {
	if (g_done) return;

	HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
	if (!tier0) return;

	auto findChannel = (LoggingSystem_FindChannel_t)GetProcAddress(tier0, "LoggingSystem_FindChannel");
	auto setByName = (LoggingSystem_SetChannelVerbosityByName_t)GetProcAddress(tier0, "LoggingSystem_SetChannelVerbosityByName");
	auto getVerbosity = (LoggingSystem_GetChannelVerbosity_t)GetProcAddress(tier0, "LoggingSystem_GetChannelVerbosity");
	if (!findChannel || !setByName) { g_done = true; return; }

	// Wait until every target channel has been registered by its owning module; setting the
	// verbosity of a channel that registers afterwards would be lost.
	for (const char* name : kQuietChannels) {
		if (findChannel(name) < 0) return; // not yet -- retry next pump
	}

	for (const char* name : kQuietChannels) {
		const int before = getVerbosity ? getVerbosity(findChannel(name)) : -1;
		setByName(name, kVerbosityOff);
		advancedfx::DevMessage(1, "EngineLogQuiet: channel %s verbosity %d -> off\n", name, before);
	}
	g_done = true;
}
