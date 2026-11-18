#pragma once
// Pixel-stage substitutions that outlive one draw.
//
// Live lighting replaces a lit draw's inputs (the light list, cluster masks, shadow atlas, probe
// volumes, three constant buffers, two samplers and the program itself) and used to put the engine's
// own back after every draw: about 75 device calls a draw, for several hundred draws a frame. CS2
// submits those draws in runs of one material program, and between two draws of a run it changes
// only material textures and per-object constants. So the replacements can stay bound.
//
// While a substitution is held, the device context has our bindings and this layer has the engine's:
//   - PSGet* report the engine's bindings, so engine and hook code reading state see what the engine set;
//   - a PSSet* on a substituted slot updates the engine's side and leaves ours bound;
//   - anything the lighting decision depends on (program, vertex shader, targets, blend, viewport,
//     the view and lighting constant buffers, extra shader stages, a command list, the present) ends
//     the substitution first, restoring every engine binding (Flush).
// A draw that arrives with the substitution still held therefore needs exactly the inputs that are
// bound, and takes them without a device call.
//
// During that draw itself the layer steps aside (Suspend/Resume): hook code that runs inside a lit
// draw's detour (the shadow caster capture, the smoke and sky scopes) has always seen our bindings
// there, saved and restored them around its own work, and decided from them. It must see the same
// with a held substitution as with one bound for that draw alone.
//
// Immediate context, render thread only. Deferred contexts pass through.
#include <d3d11.h>
#include <d3d11_1.h>

namespace Filmmaker::PixelStateLayer {

inline constexpr unsigned ViewSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
inline constexpr unsigned SamplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
inline constexpr unsigned BufferSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;

enum class End : unsigned {
    Shader, VertexShader, ExtraStage, Targets, Blend, Viewport, Constants, Constants1, CommandList,
    ClearState, Present, Ineligible, Reset, Count
};

// What a lit draw leaves bound, with what the engine had there. `engine*` pointers carry one
// reference each, which the layer takes over; `ours*` are borrowed and referenced by the layer.
struct Substitution {
    ID3D11PixelShader* engineShader = nullptr; // null: the engine's own program stays bound
    unsigned viewCount = 0;
    UINT viewSlot[48]{};
    ID3D11ShaderResourceView* engineView[48]{};
    ID3D11ShaderResourceView* oursView[48]{};
    unsigned samplerCount = 0;
    UINT samplerSlot[4]{};
    ID3D11SamplerState* engineSampler[4]{};
    ID3D11SamplerState* oursSampler[4]{};
    unsigned bufferCount = 0;
    UINT bufferSlot[6]{};
    ID3D11Buffer* engineBuffer[6]{};
    ID3D11Buffer* oursBuffer[6]{};
    // Constant buffers the lighting decision read without replacing them (per-view rows, the
    // engine's lighting constants). A different buffer there ends the substitution.
    unsigned dependencyCount = 0;
    UINT dependencySlot[4]{};
};

// Diagnostic (mirv_relight stickydraws): 0 restores the engine's bindings after every lit draw.
void SetEnabled(bool enabled);
bool Enabled();

// Installs the pixel-stage Get/Set detours. Call inside the context's Detour transaction.
void Attach(ID3D11DeviceContext* context);
// After that transaction committed. `setShader` is the original PSSetShader behind the caller's own
// detour of it. The layer holds nothing until this has run.
void Commit(void (STDMETHODCALLTYPE* setShader)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT));

// True while `context` holds a substitution and no reused draw is in progress: the next lit draw
// needs no setup.
bool Active(ID3D11DeviceContext* context);
// Around a draw that reuses the held substitution: reads and sets pass straight through, and only
// an explicit Flush ends the hold.
void Suspend();
void Resume();
// Flush unless a reused draw is in progress.
void FlushIfIdle(End reason);
// Counts a draw served by the held substitution.
void NoteReuse();
// Takes over the bindings a lit draw made. Reads the rest of what the decision depended on
// (vertex shader, targets, blend state, viewport) from the context.
void Adopt(ID3D11DeviceContext* context, const Substitution& substitution);
// Restores the engine's bindings. No-op when nothing is held.
void Flush(End reason);

// Calls from detours that already exist. OnSetShader returns true when the engine re-set the
// program it already believes is bound: the call must not be forwarded.
bool OnSetShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader, UINT classInstances);
void OnSetRenderTargets(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth);
void OnSetBlendState(ID3D11DeviceContext* context, ID3D11BlendState* blend, const FLOAT factor[4], UINT mask);
void OnSetViewports(ID3D11DeviceContext* context, UINT count, const D3D11_VIEWPORT* viewports);

struct Stats { unsigned long long adopted = 0, reused = 0, ended[unsigned(End::Count)]{}; };
// Totals since the last call; `reset` clears them.
Stats TakeStats(bool reset);
const char* EndName(End reason);

} // namespace Filmmaker::PixelStateLayer
