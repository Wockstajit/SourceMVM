#pragma once

// Render-pass compositor: turns one physical render into the frame a pass stream records
// (Filmmaker multipass capture, see Filmmaker/Render/RenderPassPlan.h for the pass model).
//
// Render thread only, at the BeforeUi boundary of the render in question, while the engine's scene
// colour and depth are still bound. Every call finishes its GPU work through a deferred context
// executed on the immediate context BEFORE it returns, so the recorder's copy of the returned
// texture, queued right after, always sees the finished frame.

#include <d3d11.h>

namespace AfxRenderPass {

struct ComposeParams {
	int mode = 0;            // 0 beauty, 1 depth, 2 normal, 3 key (Filmmaker::PassType); 4 raw depth debug
	int mask = 0;            // 0 none, 1 colour key, 2 gun diff (Filmmaker::PassMask)
	float keyIn[4] = {0.0f, 1.0f, 0.0f, 0.2f};   // internal chroma key rgb + match tolerance
	float keyOut[3] = {0.0f, 1.0f, 0.0f};        // background colour written by the pass
	float nearUnits = 8.0f;  // depth: white
	float farUnits = 4096.0f; // depth: black
	float skyCutoff = 20000.0f; // normals: nothing past this is a surface
	float diffLo = 0.03f;    // gun diff: below = background
	float diffHi = 0.08f;    // gun diff: above = subject
	// The frame's camera, for unprojecting depth (see afx_render_pass_ps_5_0.fxc).
	float invViewProj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // inverse world-to-clip, row-major
	float camera[3] = {0, 0, 0};
	float forward[3] = {1, 0, 0}, right[3] = {0, -1, 0}, up[3] = {0, 0, 1};
	bool reversedDepth = false;
	float depthMin = 0.0f, depthMax = 1.0f; // world viewport depth range
	// Players Only: mask by depth against this frame's World Only render (StoreWorldReference)
	// instead of by colour alone. Set by a render job, whose plan renders World Only first.
	bool worldRef = false;
};

void BeginDevice(ID3D11Device* device);
void EndDevice();

// The scene colour of the current render from before the lens post pass (depth of field, flare):
// Key, Depth and Normal passes and the Gun Only reference are built from it, so the lens reaches
// Beauty plates only. Kept at the BeforeUi boundary just before the lens draws
// (CRenderPassCommands::KeepCleanColor) and dropped once that render's captures ran.
bool KeepCleanColor(ID3D11DeviceContext* immediate, ID3D11Texture2D* color);
// The kept copy while this render holds one, otherwise `color` itself (borrowed).
ID3D11Texture2D* CleanColor(ID3D11Texture2D* color);
void DropCleanColor();

// Keeps the Main render's colour and depth for Gun Only passes rendered later in the same frame.
bool StoreReference(ID3D11DeviceContext* immediate, ID3D11Texture2D* color, ID3D11DepthStencilView* depth);

// Keeps the World Only render's depth for the Players Only passes rendered later in the same frame.
bool StoreWorldReference(ID3D11DeviceContext* immediate, ID3D11DepthStencilView* depth);

// Returns the composed RGBA8 frame (borrowed; valid until the next Compose), or nullptr.
ID3D11Texture2D* Compose(ID3D11DeviceContext* immediate, ID3D11Texture2D* color,
	ID3D11DepthStencilView* depth, const ComposeParams& params);

// The same pass written back over `target` (the scene colour `color` came from), for the Render
// workspace's live view of a Depth or Normal pass. The source is copied first, so reading and
// writing the one texture is safe. False (target untouched) when the pass cannot be built.
bool ComposeInto(ID3D11DeviceContext* immediate, ID3D11Texture2D* color,
	ID3D11DepthStencilView* depth, const ComposeParams& params, ID3D11RenderTargetView* target);

} // namespace AfxRenderPass
