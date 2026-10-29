#include "SourceLensSmoke.h"
namespace SourceFilmmaker {
namespace {
struct Capture {
    IDirect3DDevice9* device=nullptr;
    IDirect3DSurface9* output=nullptr; // borrowed for the scoped auxiliary view
    IDirect3DSurface9* saved[4]={};
    D3DVIEWPORT9 viewport{};
    bool active=false,smoke=false,drawing=false;
    unsigned draws=0;
    static constexpr std::array<D3DRENDERSTATETYPE,10> keys={D3DRS_ALPHABLENDENABLE,
        D3DRS_SEPARATEALPHABLENDENABLE,D3DRS_SRCBLENDALPHA,D3DRS_DESTBLENDALPHA,
        D3DRS_BLENDOPALPHA,D3DRS_COLORWRITEENABLE,D3DRS_ZWRITEENABLE,
        D3DRS_SRGBWRITEENABLE,D3DRS_SRCBLEND,D3DRS_DESTBLEND};
    std::array<DWORD,keys.size()> values{};
} g;
}
void LensSmoke_Begin(IDirect3DDevice9* device,IDirect3DSurface9* target) {
    g.device=device;g.output=target;g.active=bool(device && target);g.smoke=false;g.draws=0;
}
void LensSmoke_Material(bool smoke) { g.smoke=smoke; }
bool LensSmoke_BeforeDraw(IDirect3DDevice9* device) {
    if(!g.active || !g.smoke || g.drawing || g.device!=device) return false;
    D3DSURFACE_DESC desc{};
    if(FAILED(g.output->GetDesc(&desc)) || FAILED(device->GetViewport(&g.viewport))
        || g.viewport.Width!=desc.Width || g.viewport.Height!=desc.Height) return false;
    for(unsigned i=0;i<g.keys.size();++i) if(FAILED(device->GetRenderState(g.keys[i],&g.values[i]))) return false;
    for(unsigned i=0;i<4;++i) { g.saved[i]=nullptr;device->GetRenderTarget(i,&g.saved[i]); }
    for(unsigned i=1;i<4;++i) device->SetRenderTarget(i,nullptr);
    g.drawing=true;
    if(FAILED(device->SetRenderTarget(0,g.output))) { LensSmoke_AfterDraw();return false; }
    device->SetViewport(&g.viewport);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,TRUE);
    device->SetRenderState(D3DRS_SRCBLENDALPHA,D3DBLEND_ZERO);
    device->SetRenderState(D3DRS_DESTBLENDALPHA,D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ZERO);
    device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ONE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_ALPHA);
    device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
    ++g.draws;return true;
}
void LensSmoke_AfterDraw() {
    if(!g.drawing) return;
    for(unsigned i=1;i<4;++i) g.device->SetRenderTarget(i,nullptr);
    for(unsigned i=0;i<4;++i) { g.device->SetRenderTarget(i,g.saved[i]);if(g.saved[i]) {g.saved[i]->Release();g.saved[i]=nullptr;} }
    g.device->SetViewport(&g.viewport);
    for(unsigned i=0;i<g.keys.size();++i) g.device->SetRenderState(g.keys[i],g.values[i]);
    g.drawing=false;
}
unsigned LensSmoke_End() {
    LensSmoke_AfterDraw();const unsigned draws=g.draws;
    g.active=g.smoke=false;g.output=nullptr;g.device=nullptr;return draws;
}
}
