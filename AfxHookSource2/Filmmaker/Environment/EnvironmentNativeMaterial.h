#pragma once
#include "EnvironmentScene.h"
#include <d3d11.h>
namespace Filmmaker::Environment {
void NoteNativeMaterialPixelShader(ID3D11PixelShader*,const void*,size_t);
// The lighting variants of CS2's material programs are built on a background worker, off CS2's
// loader threads (EnvironmentNativeMaterial.cpp). For the heartbeat: programs handled, programs
// still waiting, and the worker's total time.
struct NativeMaterialDerivation { unsigned long long done=0; size_t queued=0; double workMs=0; };
NativeMaterialDerivation NativeMaterialDerivationStatus();
// Writes the bytecode of every clustered-light program seen so far to <directory>/<hash>.dxbc.
unsigned DumpNativePrograms(const char* directory);
// Shade the native sky geometry directly, preserving its depth and MSAA coverage.
struct NativeMaterialSkyScope {
    NativeMaterialSkyScope(ID3D11DeviceContext*,bool enabled=true);
    ~NativeMaterialSkyScope();
    NativeMaterialSkyScope(const NativeMaterialSkyScope&)=delete;
    NativeMaterialSkyScope& operator=(const NativeMaterialSkyScope&)=delete;
private:
    ID3D11DeviceContext* context=nullptr;
    ID3D11PixelShader* original=nullptr;
    ID3D11Buffer* previousConstants=nullptr;
};
// Suppress only the supported world/viewmodel draw's native sun. Skybox and other
// passes retain their original lighting, and the engine's constant buffer is read-only.
struct NativeMaterialSunScope {
    NativeMaterialSunScope(ID3D11DeviceContext*,bool enabled=true);
    ~NativeMaterialSunScope();
    NativeMaterialSunScope(const NativeMaterialSunScope&)=delete;
    NativeMaterialSunScope& operator=(const NativeMaterialSunScope&)=delete;
private:
    ID3D11DeviceContext* context=nullptr;
    ID3D11Buffer* original=nullptr;
};
// Baked lighting off, on the engine's OWN draw. Substitutes the map's baked lighting inputs for
// the duration of one draw and restores them after, so the world renders unbaked into CS2's own
// frame with no replay, no coverage pass and no composite involvement.
//
// A map bakes into THREE carriers and all have to go, or the frame shows one lighting model
// meeting another at every prop edge:
//   lightmap arrays (t1 direct, t2 irradiance) -- world surfaces
//   probe irradiance volume                    -- props, characters, foliage, cables, decals
//   reflection cubemaps                        -- every glossy surface, on both of the above
// Measured on Mirage: substituting only the lightmap left 8 of 23 drawn programs baked, among
// them csgo_vertexlitgeneric and csgo_projected_decals, which is what made a boarded window read
// as brighter than the wall it sits on. A cubemap is a 360 capture of the map taken under its
// original lights, so a reflection reinstates that lighting even where nothing else does.
//
// Deliberately independent of the owned-lighting replay below, which needs the clustered-light
// ABI a program may not have. This needs only what that program declares, recorded for EVERY
// pixel shader by NoteNativeMaterialPixelShader, so it also reaches the overlay, decal and
// blended materials the replay rejects outright.
struct NativeMaterialUnbakeScope {
    NativeMaterialUnbakeScope(ID3D11DeviceContext*,bool enabled=true);
    ~NativeMaterialUnbakeScope();
    NativeMaterialUnbakeScope(const NativeMaterialUnbakeScope&)=delete;
    NativeMaterialUnbakeScope& operator=(const NativeMaterialUnbakeScope&)=delete;
private:
    static const unsigned kMaxSlots=8; // t1-t3, the probe volume, and the reflection cubes
    ID3D11DeviceContext* context=nullptr;
    UINT slots[kMaxSlots]{};
    unsigned count=0;
    ID3D11ShaderResourceView* previous[kMaxSlots]{};
};
// Whole-map relighting, on the engine's OWN draw. Every program that carries the clustered-light
// interface (derived from its bytecode, so any material family) reads our light records, cluster
// masks, shadow atlas and ambient in place of the map's, with every baked input neutralised, and
// the engine shades and blends as it always does. No replay, no private target, no composite.
struct NativeMaterialLiveScope {
    NativeMaterialLiveScope(ID3D11DeviceContext*,bool enabled=true);
    ~NativeMaterialLiveScope();
    NativeMaterialLiveScope(const NativeMaterialLiveScope&)=delete;
    NativeMaterialLiveScope& operator=(const NativeMaterialLiveScope&)=delete;
private:
    ID3D11DeviceContext* context=nullptr;
    ID3D11Buffer* original=nullptr;
    ID3D11PixelShader* originalShader=nullptr;
    bool irradianceActive=false;
    ID3D11Buffer* previousIrradianceConstants=nullptr;
    ID3D11ShaderResourceView* previousIrradiance=nullptr;
    ID3D11ShaderResourceView* previousDistance=nullptr;
    ID3D11ShaderResourceView* previousWorldIrradiance=nullptr;
    ID3D11SamplerState* previousIrradianceSampler=nullptr;
    bool smokeActive=false;
    bool overlayScaled=false;
    ID3D11BlendState* previousOverlayBlend=nullptr;
    float previousOverlayFactor[4]{};
    UINT previousOverlayMask=0xffffffff;
    ID3D11ShaderResourceView* previousSmokeViews[3]{};
    ID3D11SamplerState* previousSmokeSamplers[2]{};
    ID3D11Buffer* previousSmokeConstants[2]{};
};
// Immediate render thread only. Replay supported world and first-person draws between
// Begin/End; material shaders, textures and pose stay native. Engine resources
// are read only; the light buffer and color target are private copies.
bool BeginNativeMaterialDraw(ID3D11DeviceContext*);
// Main thread readable: live lighting's per-frame CPU cost (light preparation, ambient occlusion,
// all of its draw setup), ~20-frame averages.
void NativeMaterialLiveCost(float& prepareMs,float& ambientOcclusionMs,float& totalMs);
bool BeginNativeMaterialBaseline(ID3D11DeviceContext*);
bool BeginNativeMaterialWithoutFixtures(ID3D11DeviceContext*);
bool BeginNativeMaterialCoverage(ID3D11DeviceContext*);
void EndNativeMaterialDraw(ID3D11DeviceContext*);
using NativeMaterialHdrPass = bool(*)(ID3D11DeviceContext*,ID3D11RenderTargetView*,ID3D11DepthStencilView*,ID3D11Texture2D*,const Frame&);
void SetNativeMaterialHdrPass(NativeMaterialHdrPass);
using LightingProofShadowPass = bool(*)(ID3D11DeviceContext*,const Frame&,Matrix*,ID3D11ShaderResourceView**);
using MapLightShadowPass = unsigned(*)(ID3D11DeviceContext*,const Frame&,const Vec3*,const float*,
                                       unsigned,const Vec3*,const float*,unsigned,Vec3,Matrix*,
                                       unsigned*,ID3D11ShaderResourceView**);
void SetMapLightShadowPass(MapLightShadowPass);
void SetLightingProofShadowPass(LightingProofShadowPass);
// Runtime sky-visibility maps for the ambient term (EnvironmentShadowMaps::SkyVisibilityMaps).
using SkyVisibilitySource = bool(*)(ID3D11ShaderResourceView**,Matrix*);
void SetSkyVisibilitySource(SkyVisibilitySource);
// Called before the first first-person (viewport depth 0..0.1) material draw of a frame.
using ViewmodelShadowPass = bool(*)(ID3D11DeviceContext*);
void SetViewmodelShadowPass(ViewmodelShadowPass);
using GlassBeamShadowPass = unsigned(*)(ID3D11DeviceContext*,const Matrix*,unsigned);
void SetGlassBeamShadowPass(GlassBeamShadowPass);
// Binds the authored (world space, un-rebased) map light records for the
// diagnostic views and returns how many there are. Zero unbinds.
unsigned BindRelightRecords(ID3D11DeviceContext*);
unsigned RelightClusterMode();
bool NativeMaterialComposited(const Frame&);
bool NativeSkyApplied(const Frame&);
// Authored sun/cloud cards in the reserved sky depth interval belong to the
// replaced sky. Ordinary world/viewmodel particles remain native.
bool SkipNativeSkyEffect(ID3D11DeviceContext*,unsigned vertices,unsigned instances);
void NextNativeMaterialFrame();
void ResetNativeMaterial();
// Publish the immutable pass snapshot before any engine material draws.
void PrepareNativeMaterialFrame(const Frame&);
// Bind this frame's native material result without advancing lighting state.
// A mismatched frame/target or unsupported shader returns false.
bool BindNativeMaterial(ID3D11DeviceContext*,ID3D11Texture2D*,ID3D11Texture2D*,const Frame&,
                        ID3D11Texture2D* nativeHdrSource=nullptr);
}
