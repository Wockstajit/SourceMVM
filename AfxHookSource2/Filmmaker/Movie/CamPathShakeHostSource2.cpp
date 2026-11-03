// CS2's answers to path shake's host (CamPathShakeHost.h): the filmmaker camera path, the render's
// path tick, the scene keys' tick and the CS2 project store.

#include "CamPathShakeHost.h"
#include "CameraPath.h"
#include "SceneKeys.h"
#include "../Filmmaker.h"            // PlayingDemoPath()
#include "../Render/RenderCommand.h" // Render_PathCameraTick
#include "../../MirvTime.h"          // interval_per_tick_get: the tick->seconds rate

namespace Filmmaker {

const std::vector<CamMarker>& PathShakeHost_Markers() { return CameraPathRef().Markers(); }

// During a Camera Path render the job drives the path itself and the editor is not playing, so
// CurrentPathTick() is false there: the render's own tick is taken instead.
bool PathShakeHost_PathTick(double& outTick) {
	return CameraPathRef().CurrentPathTick(outTick) || Render_PathCameraTick(outTick);
}

double PathShakeHost_ObjectTick() { return SceneKeys_AppliedTick(); }

std::wstring PathShakeHost_DemoPath() { return PlayingDemoPath(); }

ProjectGame PathShakeHost_Game() { return ProjectGame::Cs2; }

double PathShakeHost_IntervalPerTick() { return g_MirvTime.interval_per_tick_get(); }

} // namespace Filmmaker
