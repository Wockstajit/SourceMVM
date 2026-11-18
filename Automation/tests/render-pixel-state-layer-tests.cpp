// The pixel state layer on a real device context (WARP): a held substitution keeps our bindings in
// the context, tracks what the engine binds over them, and gives every engine binding back when it
// ends. The device-context detours are not installed here; the test calls the layer the way they do.
#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Render/RenderPixelStateLayer.h"
#include "../../AfxHookSource2/Filmmaker/Render/RenderPixelStateLayerDetail.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace Filmmaker::PixelStateLayer;

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Hr(HRESULT value, const char* message) { Check(SUCCEEDED(value), message); }

void STDMETHODCALLTYPE SetShader(ID3D11DeviceContext* c, ID3D11PixelShader* s, ID3D11ClassInstance* const* i, UINT n) { c->PSSetShader(s, i, n); }
void STDMETHODCALLTYPE SetViews(ID3D11DeviceContext* c, UINT a, UINT n, ID3D11ShaderResourceView* const* v) { c->PSSetShaderResources(a, n, v); }
void STDMETHODCALLTYPE SetSamplers(ID3D11DeviceContext* c, UINT a, UINT n, ID3D11SamplerState* const* v) { c->PSSetSamplers(a, n, v); }
void STDMETHODCALLTYPE SetBuffers(ID3D11DeviceContext* c, UINT a, UINT n, ID3D11Buffer* const* v) { c->PSSetConstantBuffers(a, n, v); }
void STDMETHODCALLTYPE GetBuffers(ID3D11DeviceContext* c, UINT a, UINT n, ID3D11Buffer** v) { c->PSGetConstantBuffers(a, n, v); }

ULONG References(IUnknown* object) { object->AddRef(); return object->Release(); }
} // namespace

int main() {
    try {
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx), "Create device");

        auto view = [&](ComPtr<ID3D11ShaderResourceView>& out) {
            D3D11_TEXTURE2D_DESC d{}; d.Width = d.Height = 4; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> texture; Hr(device->CreateTexture2D(&d, nullptr, &texture), "Create texture");
            Hr(device->CreateShaderResourceView(texture.Get(), nullptr, &out), "Create view");
        };
        auto buffer = [&](ComPtr<ID3D11Buffer>& out) {
            D3D11_BUFFER_DESC d{}; d.ByteWidth = 16; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            Hr(device->CreateBuffer(&d, nullptr, &out), "Create buffer");
        };
        auto sampler = [&](ComPtr<ID3D11SamplerState>& out, D3D11_FILTER filter) {
            D3D11_SAMPLER_DESC d{}; d.Filter = filter; d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; d.MaxLOD = D3D11_FLOAT32_MAX;
            Hr(device->CreateSamplerState(&d, &out), "Create sampler");
        };
        ComPtr<ID3D11ShaderResourceView> engineA, engineB, engineC, engineNew, ours6, ours7, ours13;
        for (auto* v : {std::addressof(engineA), std::addressof(engineB), std::addressof(engineC), std::addressof(engineNew), std::addressof(ours6), std::addressof(ours7), std::addressof(ours13)}) view(*v);
        ComPtr<ID3D11Buffer> engineLighting, engineView, engineOther, oursLighting, engineProbe, oursProbe, newView;
        for (auto* b : {std::addressof(engineLighting), std::addressof(engineView), std::addressof(engineOther), std::addressof(oursLighting), std::addressof(engineProbe), std::addressof(oursProbe), std::addressof(newView)}) buffer(*b);
        ComPtr<ID3D11SamplerState> engineSampler, oursSampler;
        sampler(engineSampler, D3D11_FILTER_MIN_MAG_MIP_POINT); sampler(oursSampler, D3D11_FILTER_MIN_MAG_MIP_LINEAR);
        ComPtr<ID3D11PixelShader> engineShader, oursShader, otherShader;
        const char source[] = "float4 main() : SV_Target { return 0; }";
        ComPtr<ID3DBlob> code;
        Hr(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &code, nullptr), "Compile shader");
        for (auto* shader : {std::addressof(engineShader), std::addressof(oursShader), std::addressof(otherShader)})
            Hr(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader->GetAddressOf()), "Create shader");

        // Without the detours nothing may be held: Adopt hands the engine's bindings straight back.
        Check(!Enabled(), "The layer is off until its detours are attached");

        Detail::Originals originals;
        originals.attached = true; originals.setShader = SetShader; originals.setShaderResources = SetViews;
        originals.setSamplers = SetSamplers; originals.setConstantBuffers = SetBuffers; originals.getConstantBuffers = GetBuffers;
        Detail::SetOriginals(originals);
        Check(Enabled(), "The layer is on once attached");

        const ULONG engineAReferences = References(engineA.Get()), oursReferences = References(ours6.Get());
        auto bindEngine = [&] {
            ctx->PSSetShader(engineShader.Get(), nullptr, 0);
            ID3D11ShaderResourceView* views[]{engineA.Get(), engineB.Get()}; ctx->PSSetShaderResources(6, 2, views);
            ID3D11ShaderResourceView* v13 = engineC.Get(); ctx->PSSetShaderResources(13, 1, &v13);
            ID3D11Buffer* b = engineProbe.Get(); ctx->PSSetConstantBuffers(2, 1, &b);
            b = engineLighting.Get(); ctx->PSSetConstantBuffers(3, 1, &b);
            b = engineView.Get(); ctx->PSSetConstantBuffers(4, 1, &b);
            ID3D11SamplerState* s = engineSampler.Get(); ctx->PSSetSamplers(5, 1, &s);
        };
        // What the lit draw does: save the engine's binding, bind ours, then let the layer hold both.
        auto litDraw = [&] {
            Substitution s;
            ctx->PSGetShader(&s.engineShader, nullptr, nullptr); ctx->PSSetShader(oursShader.Get(), nullptr, 0);
            const UINT slots[]{6, 7, 13}; ID3D11ShaderResourceView* ours[]{ours6.Get(), ours7.Get(), ours13.Get()};
            for (unsigned i = 0; i < 3; ++i) {
                s.viewSlot[i] = slots[i]; s.oursView[i] = ours[i];
                ctx->PSGetShaderResources(slots[i], 1, &s.engineView[i]); ctx->PSSetShaderResources(slots[i], 1, &ours[i]);
            }
            s.viewCount = 3;
            s.samplerSlot[0] = 5; s.oursSampler[0] = oursSampler.Get(); s.samplerCount = 1;
            ctx->PSGetSamplers(5, 1, &s.engineSampler[0]); ID3D11SamplerState* sampler = oursSampler.Get(); ctx->PSSetSamplers(5, 1, &sampler);
            s.bufferSlot[0] = 2; s.oursBuffer[0] = oursProbe.Get(); s.bufferCount = 1;
            ctx->PSGetConstantBuffers(2, 1, &s.engineBuffer[0]); ID3D11Buffer* probe = oursProbe.Get(); ctx->PSSetConstantBuffers(2, 1, &probe);
            s.dependencySlot[0] = 3; s.dependencySlot[1] = 4; s.dependencyCount = 2;
            Adopt(ctx.Get(), s);
        };
        auto boundView = [&](UINT slot) { ComPtr<ID3D11ShaderResourceView> v; ctx->PSGetShaderResources(slot, 1, &v); return v; };
        auto boundBuffer = [&](UINT slot) { ComPtr<ID3D11Buffer> b; ctx->PSGetConstantBuffers(slot, 1, &b); return b; };
        auto boundShader = [&] { ComPtr<ID3D11PixelShader> s; ctx->PSGetShader(&s, nullptr, nullptr); return s; };
        auto boundSampler = [&](UINT slot) { ComPtr<ID3D11SamplerState> s; ctx->PSGetSamplers(slot, 1, &s); return s; };

        // 1. A held substitution: the context has ours, the layer reports the engine's.
        bindEngine(); litDraw();
        Check(Active(ctx.Get()), "The substitution is held after a lit draw");
        Check(boundView(6) == ours6 && boundView(7) == ours7 && boundView(13) == ours13, "Our views stay bound");
        Check(boundShader() == oursShader && boundBuffer(2) == oursProbe && boundSampler(5) == oursSampler, "Our program, constants and sampler stay bound");
        {
            ID3D11ShaderResourceView* views[2]{}; ctx->PSGetShaderResources(6, 2, views);
            Detail::FixViews(6, 2, views);
            Check(views[0] == engineA.Get() && views[1] == engineB.Get(), "Reads report the engine's views");
            for (auto* v : views) if (v) v->Release();
            ID3D11PixelShader* shader = nullptr; Detail::GetShader(&shader, nullptr, nullptr);
            Check(Detail::ShaderHeld() && shader == engineShader.Get(), "Reads report the engine's program"); shader->Release();
            ID3D11Buffer* buffers[3]{}; ctx->PSGetConstantBuffers(2, 3, buffers); Detail::FixBuffers(2, 3, buffers);
            Check(buffers[0] == engineProbe.Get() && buffers[1] == engineLighting.Get() && buffers[2] == engineView.Get(), "Reads report the engine's constants");
            for (auto* b : buffers) if (b) b->Release();
            ID3D11SamplerState* samplers[1]{}; ctx->PSGetSamplers(5, 1, samplers); Detail::FixSamplers(5, 1, samplers);
            Check(samplers[0] == engineSampler.Get(), "Reads report the engine's sampler"); samplers[0]->Release();
        }

        // 2. The engine binds a new view over a substituted slot: ours stays, the engine's side moves.
        {
            ID3D11ShaderResourceView* engineSet[3]{engineC.Get(), engineNew.Get(), engineB.Get()}; // slots 5, 6, 7
            ID3D11ShaderResourceView* scratch[ViewSlots];
            auto forwarded = Detail::SetViews(5, 3, engineSet, scratch);
            Check(forwarded == scratch && scratch[0] == engineC.Get() && scratch[1] == ours6.Get() && scratch[2] == ours7.Get(),
                "A set over substituted slots forwards ours there and the engine's elsewhere");
            ctx->PSSetShaderResources(5, 3, forwarded);
            Check(boundView(5) == engineC && boundView(6) == ours6, "The unsubstituted slot takes the engine's view");
            ID3D11ShaderResourceView* untouched[1]{engineA.Get()};
            Check(Detail::SetViews(20, 1, untouched, scratch) == untouched, "A set elsewhere passes through unchanged");
        }
        // 3. The engine re-sets its own program: nothing to forward. Its own constants again: kept.
        Check(OnSetShader(ctx.Get(), engineShader.Get(), 0) && Active(ctx.Get()), "The engine's own program again keeps the substitution");
        {
            ID3D11Buffer* same[2]{engineLighting.Get(), engineView.Get()}; ID3D11Buffer* scratch[BufferSlots]; bool flush = true;
            Check(Detail::SetBuffers(3, 2, same, scratch, flush) == same && !flush, "The same view and lighting constants keep the substitution");
            ID3D11Buffer* probe[1]{engineOther.Get()};
            auto forwarded = Detail::SetBuffers(2, 1, probe, scratch, flush);
            Check(!flush && forwarded == scratch && scratch[0] == oursProbe.Get(), "A set over our constants keeps ours bound");
        }
        // 4. Ending it gives the engine everything it bound, including what it bound meanwhile.
        Flush(End::Present);
        Check(!Active(ctx.Get()), "Nothing is held after a flush");
        Check(boundView(6) == engineNew && boundView(7) == engineB && boundView(13) == engineC, "The engine's views are back, the newer one included");
        Check(boundShader() == engineShader && boundBuffer(2) == engineOther && boundSampler(5) == engineSampler, "The engine's program, constants and sampler are back");

        // 5. Each thing the lighting decision depends on ends the substitution.
        bindEngine(); litDraw();
        OnSetShader(ctx.Get(), otherShader.Get(), 0);
        Check(!Active(ctx.Get()) && boundShader() == engineShader, "Another program ends it and restores the engine's first");
        bindEngine(); litDraw();
        {
            ID3D11Buffer* changed[1]{newView.Get()}; ID3D11Buffer* scratch[BufferSlots]; bool flush = false;
            Detail::SetBuffers(4, 1, changed, scratch, flush);
            Check(flush, "A different view constant buffer ends it");
            Flush(End::Constants);
        }
        bindEngine(); litDraw();
        D3D11_VIEWPORT viewport{0, 0, 64, 64, 0, 1};
        OnSetViewports(ctx.Get(), 1, &viewport);
        Check(!Active(ctx.Get()), "A different viewport ends it");
        ctx->RSSetViewports(1, &viewport);
        bindEngine(); litDraw();
        OnSetViewports(ctx.Get(), 1, &viewport);
        Check(Active(ctx.Get()), "The same viewport again keeps it");
        OnSetBlendState(ctx.Get(), nullptr, nullptr, 0xffffffff);
        Check(Active(ctx.Get()), "The same blend state again keeps it");
        OnSetBlendState(ctx.Get(), nullptr, nullptr, 0x1);
        Check(!Active(ctx.Get()), "A different sample mask ends it");
        bindEngine(); litDraw();
        OnSetRenderTargets(ctx.Get(), 0, nullptr, nullptr);
        Check(Active(ctx.Get()), "The same (empty) targets again keep it");
        Check(!Detail::SameVertexShader(reinterpret_cast<ID3D11VertexShader*>(engineShader.Get())), "Another vertex shader is noticed");
        Flush(End::Present);

        // 6. During a draw that reuses the hold, the layer steps aside: code inside the draw sees
        //    and changes the real bindings, and nothing it does ends the hold.
        bindEngine(); litDraw();
        Suspend();
        Check(!Active(ctx.Get()) && !Detail::Holds(ctx.Get()), "A reused draw in progress is not a hold to reuse or to read through");
        Check(!OnSetShader(ctx.Get(), otherShader.Get(), 0), "A program set inside a reused draw is forwarded");
        OnSetViewports(ctx.Get(), 0, nullptr);
        FlushIfIdle(End::Ineligible);
        Resume();
        Check(Active(ctx.Get()) && boundShader() == oursShader && boundView(6) == ours6, "The hold survives the draw that reused it");
        Suspend(); Flush(End::Reset); Resume();
        Check(!Active(ctx.Get()) && boundShader() == engineShader && boundView(6) == engineA, "An explicit flush ends a hold even inside a reused draw");

        // 7. No reference is kept past the end.
        ctx->ClearState();
        Check(References(engineA.Get()) == engineAReferences && References(ours6.Get()) == oursReferences, "The layer released every reference it held");

        const Stats stats = TakeStats(true);
        Check(stats.adopted == 7 && stats.ended[unsigned(End::Present)] == 2 && stats.ended[unsigned(End::Shader)] == 1 &&
              stats.ended[unsigned(End::Reset)] == 1, "Counts match the calls made");
        std::cout << "render pixel state layer: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << "\n";
        return 1;
    }
}
