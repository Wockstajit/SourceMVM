#include "DemoTimescale.h"

#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstdio>
#include <cstdlib>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// The scrubber maps 0.05x..20x logarithmically. Anything outside a generous band around that is
// not a speed anyone asked for, and 0 or negative would be a log of a non-positive number.
constexpr float kMinScale = 0.001f;
constexpr float kMaxScale = 1000.0f;

float g_scale = 1.0f;

} // namespace

float DemoTimescale_Get() { return g_scale; }

void DemoTimescale_Set(float scale) {
	if (!(scale >= kMinScale) || scale > kMaxScale) return;
	g_scale = scale;
	if (!g_pEngineToClient) return;
	char cmd[64];
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "demo_timescale %.4f", scale);
	g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
}

void DemoTimescale_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc >= 3) {
		const float v = (float)atof(args->ArgV(2));
		if (!(v >= kMinScale) || v > kMaxScale) {
			advancedfx::Warning("%s timescale: expected %g .. %g.\n", cmd, kMinScale, kMaxScale);
			return;
		}
		DemoTimescale_Set(v);
		return; // silent: the scrubbers call this every frame of a drag
	}
	advancedfx::Message("%s timescale <%g..%g> - demo playback speed (currently %g).\n"
		"  Set it through this rather than `demo_timescale` so the demo bar, the camera timeline\n"
		"  and the graph editor all show the same value.\n",
		cmd, kMinScale, kMaxScale, g_scale);
}

} // namespace Filmmaker
