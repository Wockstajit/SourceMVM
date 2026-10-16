#include "PracticeSession.h"

#include "../Config/PracticeSessionFlag.h"
#include "../Config/FilmmakerSettingsStore.h"
#include "../Movie/CameraPath.h"
#include "../Movie/CamPathShake.h"
#include "../Movie/SceneKeys.h"
#include "../Movie/PlaybackLoop.h"
#include "../Movie/ParticleFx.h"
#include "../Environment/EnvironmentScene.h"
#include "../Props/Props.h"
#include "../Relight/RelightLightEditor.h"
#include "../Render/RenderCommand.h"
#include "../Render/RenderOutput.h"
#include "../Render/RenderRange.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// The render workspace is not saved anywhere, so it is kept here while the session runs.
struct RenderStash {
	RenderRange range;
	RenderCameraSource camera = RenderCameraSource::CurrentViewer;
	CameraPathExtension extension = CameraPathExtension::Fail;
	RenderOutputSettings output;
	RenderPasses passes;
	int tab = 0;
};
RenderStash g_render;

// CONFIG's own resets, the lists its tab footers run (EffectsPersistence.cpp, the ENVIRONMENT
// footer): camera feel, every effect, ragdolls, the environment, its lights and props. With the
// flag up they only change memory. They go through the console, so they land a frame later.
const char* const kConfigResets[] = {
	"mirv_filmmaker effects reset camera",
	"mirv_filmmaker effects reset effects",
	"mirv_filmmaker effects reset ragdolls",
	"mirv_filmmaker environment reset all",
	"mirv_relight lighteditor off",
	"mirv_relight lights reset",
	"mirv_relight off",
	"mirv_filmmaker props clear",
	"mirv_filmmaker props wind 0",
};

// Every per-demo module drops what it holds without saving it and reads the playing demo again
// next frame: empty during the session, the user's own data after it.
void ReloadPerDemo() {
	CameraPathRef().ReloadProject();
	CamPathShake_ReloadProject();
	SceneKeys_ReloadProject();
	PlaybackLoop_ReloadProject();
	Relight::ReloadLightChoices();
}

} // namespace

void PracticeSession_Begin() {
	if (PracticeSession_Active()) return;
	// What the user had reaches disk first: End reads it back from there.
	SceneKeys_Flush();
	Environment::BeginPractice();
	FilmmakerSettings_BeginPractice(); // writes pending settings, then applies the defaults
	PracticeSessionFlag().store(true, std::memory_order_release);

	ReloadPerDemo();
	ParticleFxRef().ResetSettingsToDefaults();

	g_render.range = Render_GetRange();
	g_render.camera = Render_GetCameraSource();
	g_render.extension = Render_GetExtension();
	g_render.output = RenderOutputSettingsRef();
	g_render.passes = RenderPassesRef();
	g_render.tab = Render_GetModalTab();
	RenderOutputSettings output;
	output.folder = g_render.output.folder;
	RenderOutputSettingsRef() = output;
	RenderPassesRef() = RenderPasses{};
	Render_SetRange(RenderRange{});
	Render_SetCameraSource(RenderCameraSource::CurrentViewer);
	Render_SetExtension(CameraPathExtension::Fail);
	Render_SetModalTab(0);

	if (g_pEngineToClient)
		for (const char* command : kConfigResets) g_pEngineToClient->ExecuteClientCmd(0, command, true);
	MvmDebugLog_LinefAlways("practice", "begin: defaults in memory, nothing is saved until the end");
}

void PracticeSession_End() {
	if (!PracticeSession_Active()) return;
	// The session's per-demo edits are dropped while the store still refuses them, so the reload
	// that follows cannot flush them into the demo's project.
	ReloadPerDemo();
	PracticeSessionFlag().store(false, std::memory_order_release);

	FilmmakerSettings_EndPractice();
	ParticleFxRef().ReloadSettings();
	Environment::EndPractice();
	Props_ReloadAfterPractice();

	Render_SetRange(g_render.range);
	Render_SetCameraSource(g_render.camera);
	Render_SetExtension(g_render.extension);
	RenderOutputSettingsRef() = g_render.output;
	RenderPassesRef() = g_render.passes;
	Render_SetModalTab(g_render.tab);
	MvmDebugLog_LinefAlways("practice", "end: settings, effects, keys, environment and props read back from disk");
}

} // namespace Filmmaker
