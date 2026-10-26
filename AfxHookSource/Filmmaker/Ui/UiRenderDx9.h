#pragma once

// Draws a finished UI frame with D3D9, on the game's render thread.
//
// The UI is built on the main thread, but with the material system's queued rendering CS:S presents
// from its own render thread. So the main thread copies ImGui's draw data into a DrawSnapshot and
// hands it over; this renderer is the only code that touches the device, and only from the thread
// that presents. While a workspace is open it also letterboxes the game: the finished frame is
// copied out of the back buffer and drawn into the workspace's preview rect, black around it, as
// CS2's preview viewport does.

#include "UiTypes.h"

#include <imgui.h>

#include <d3d9.h>

#include <memory>
#include <vector>

namespace SourceFilmmaker {
namespace Ui {

// The font atlas texture's id in ImDrawCmd (ImFontAtlas::SetTexID).
const ImTextureID kFontTextureId = (ImTextureID)1;

struct DrawSnapshot {
	struct Command {
		ImVec4 clip;
		ImTextureID texture;
		unsigned indexOffset;
		unsigned vertexOffset;
		unsigned elementCount;
	};
	std::vector<ImDrawVert> vertices;
	std::vector<ImDrawIdx> indices;
	std::vector<Command> commands;
	ImVec2 displayPos = ImVec2(0, 0);
	ImVec2 displaySize = ImVec2(0, 0);
	bool letterbox = false; // draw the game into preview, black around it
	Rect preview;           // client px
};

// Copies what ImGui::GetDrawData() holds after ImGui::Render. Main thread.
std::shared_ptr<DrawSnapshot> TakeSnapshot(const ImDrawData* data);

// The font atlas pixels, copied on the main thread whenever the atlas is rebuilt.
struct FontPixels {
	std::vector<unsigned char> rgba;
	int width = 0;
	int height = 0;
	unsigned version = 0;
};

class RendererDx9 {
public:
	~RendererDx9();

	// Draws snapshot into the back buffer and leaves the device state as it found it.
	// Render thread, between the game's last draw and Present.
	void Render(IDirect3DDevice9* device, const DrawSnapshot& snapshot, const FontPixels& font);

	// Before a device reset or shutdown: every D3DPOOL_DEFAULT resource goes.
	void ReleaseDeviceObjects();

private:
	bool EnsureBuffers(IDirect3DDevice9* device, int vertices, int indices);
	bool EnsureFont(IDirect3DDevice9* device, const FontPixels& font);
	bool EnsureFrame(IDirect3DDevice9* device, const D3DSURFACE_DESC& backBuffer);
	void DrawLetterbox(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, const D3DSURFACE_DESC& desc, const Rect& preview);
	void DrawCommands(IDirect3DDevice9* device, const DrawSnapshot& snapshot);

	IDirect3DVertexBuffer9* m_vb = nullptr;
	IDirect3DIndexBuffer9* m_ib = nullptr;
	int m_vbSize = 0;
	int m_ibSize = 0;
	IDirect3DTexture9* m_font = nullptr;
	unsigned m_fontVersion = 0;
	IDirect3DTexture9* m_frame = nullptr;
	UINT m_frameW = 0;
	UINT m_frameH = 0;
	D3DFORMAT m_frameFormat = D3DFMT_UNKNOWN;
};

} // namespace Ui
} // namespace SourceFilmmaker
