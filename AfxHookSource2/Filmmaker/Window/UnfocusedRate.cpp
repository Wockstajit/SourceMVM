#include "UnfocusedRate.h"

#include "../Debug/MvmDebugLog.h"

#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"

namespace Filmmaker {

namespace {

// BOTH knobs are required, which is why setting only the sleep did not fix the jitter: the sleep
// is how long the engine idles per unfocused frame, but `r_always_render_all_windows` is what
// makes it keep RENDERING at all when it does not own focus. The inherited recorder sets exactly
// this pair around a recording (see CAfxStreams::RecordStart); SOURCE:MVM is an offline movie
// tool whose normal workflow has another window focused, so it wants them permanently.
struct Knob {
	const char* name;
	bool boolean;   // false = int32
	int value;
};

const Knob kKnobs[] = {
	{ "engine_no_focus_sleep", false, 0 },
	{ "r_always_render_all_windows", true, 1 },
};

const size_t kKnobCount = sizeof(kKnobs) / sizeof(kKnobs[0]);

} // namespace

void UnfocusedRate_MainThreadTick() {
	static bool s_done = false;
	if (s_done)
		return;
	using namespace SOURCESDK::CS2;
	if (!g_pCVar)
		return;

	// Apply each knob INDEPENDENTLY. An earlier all-or-nothing version applied neither whenever
	// one name failed to resolve, and (measured 2026-08-10) that meant it silently applied nothing
	// at all -- the window.nofocus channel stayed empty through a whole session while the game
	// still stuttered when unfocused. A cvar this build cannot find is reported once and skipped;
	// the rest still take effect.
	static int s_tries = 0;
	static bool s_applied[kKnobCount] = {};
	++s_tries;

	bool allApplied = true;
	for (size_t i = 0; i < kKnobCount; ++i) {
		if (s_applied[i])
			continue;
		Cvar_s* handle = g_pCVar->GetCvar(g_pCVar->FindConVar(kKnobs[i].name, false).Get());
		if (!handle) {
			allApplied = false;
			continue;
		}
		const int previous = kKnobs[i].boolean ? (handle->m_Value.m_bValue ? 1 : 0)
			: handle->m_Value.m_i32Value;
		if (kKnobs[i].boolean)
			handle->m_Value.m_bValue = kKnobs[i].value != 0;
		else
			handle->m_Value.m_i32Value = kKnobs[i].value;
		s_applied[i] = true;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("window.nofocus", "%s %d -> %d", kKnobs[i].name, previous,
				kKnobs[i].value);
	}

	// Give late-registering cvars a few hundred frames before giving up, then stop retrying and
	// say which ones never resolved rather than looping silently forever.
	if (allApplied || s_tries > 600) {
		for (size_t i = 0; i < kKnobCount; ++i) {
			if (!s_applied[i])
				advancedfx::Warning(
					"mirv_filmmaker: '%s' not found; unfocused-window throttling may persist.\n",
					kKnobs[i].name);
		}
		s_done = true;
	}
}

} // namespace Filmmaker
