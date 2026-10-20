#pragma once

// ROUND HEAT MAP (CONFIG > ENVIRONMENT > Scene Objects): where the players walked during the round
// being watched, painted red on the floor around the camera, so set dressing can go where nobody
// walks. Docs/features/round-heatmap.md owns the contract. The maths is RoundHeatmapMath.h, the
// floor pass is RoundHeatmapRender.cpp, and the player paths come from FilmmakerDemoInfo -paths.
//
// Threads: RunMainThreadFrame and RunCommand on the main thread; the paths load and the grid build
// on their own workers; Render and NoteCamera on the render thread. The grid is handed over as an
// immutable shared_ptr.

#include <cstdint>
#include <memory>
#include <sstream>
#include <vector>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
namespace advancedfx { class ICommandArgs; }

namespace Filmmaker::Heatmap {

// Main thread, once a frame: loads the demo's paths, follows the demo to its round and rebuilds the
// grid when the round, the radius, the team filter or the camera's area changes. Inert while off.
void RunMainThreadFrame();
// `mirv_filmmaker heatmap on|off|toggle|radius <units>|teams all|t|ct|status`
void RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);
// ,"heatmap":{...} for the Config state.
void AppendStateJson(std::ostringstream& out);
// The switch is on. The Environment keeps capturing where the live players stand while it is, so
// the pass can leave their shoes unpainted.
bool Enabled();
// Changes whenever what the pass paints changes other than by time and camera, for the scrub frame
// cache.
uint64_t PictureRevision();
// Joins the loader and builder threads and frees the GPU objects.
void Shutdown();

// One built grid as half floats, ready to upload (RoundHeatmapMath.h Grid::texels).
struct GpuGrid {
	float x0 = 0, y0 = 0, cell = 0, baseZ = 0, radius = 0;
	int size = 0;
	std::vector<uint16_t> texels; // size * size * 4
};
// The grid to paint now, or null. serial changes with every new grid.
std::shared_ptr<const GpuGrid> PublishedGrid(uint64_t& serial);
// Render thread, every frame: where the camera is, for the main thread's grid window.
void NoteCamera(float x, float y, float z);

// Render thread, every frame, after the Environment composite and the lens effects and before the
// recorder and the UI. It paints nothing while recording (the heat map is a placement aid, not part
// of the shot) and frees its GPU objects while the switch is off. worldToClip and projection are
// row-major with column vectors (Environment::Matrix), null without a camera. actors are the live
// players' feet (x, y, z per player).
void Render(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target, ID3D11DepthStencilView* depth,
	const float* worldToClip, const float* projection, const float camera[3], float depthMin, float depthMax,
	const float* actors, int actorCount, bool recording);
// Starts compiling the floor pass's shaders on a worker (main thread, when the switch goes on).
void PrepareShaders();
// Joins that worker and frees the GPU objects (DLL shutdown).
void ShutdownRender();

} // namespace Filmmaker::Heatmap
