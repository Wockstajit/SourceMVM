#define NOMINMAX
#include "EnvironmentShadowMapsDetail.h"
#include "EnvironmentWeatherCover.h"
namespace Filmmaker::Environment {
using Microsoft::WRL::ComPtr;
using namespace ShadowDetail;
namespace {
struct PlayerMaskTarget {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11DepthStencilState> forward, reversed;
    UINT width = 0, height = 0;
    bool valid = false;
} playerMask;
} // namespace
void EndShadowMaps() { EndWeatherCover();playerMask = PlayerMaskTarget{};mapBeamMatrices.clear();mapBeamFrame=Frame{};mapBeamFrameReady=false;mapBeamKey=0;mapBeamCount=0;ResetFoliageInputs();EndAnimatedCasters();state = State{};proofState=State{};proofContext.Reset();proofAtlas.Reset();proofAtlasView.Reset();proofSun.Reset();proofSunDepth.Reset();proofTransfer.Reset();mapAtlas.Reset();mapAtlasView.Reset();lampPyramid=LampPyramid{};mapSunTransfer.Reset();mapSunWorldBackup.Reset();mapSunViewmodelActive=false;
    mapSunTexture.Reset();mapSunDepth.Reset();mapStaticDepth.Reset();mapStaticTransfer.Reset();mapStaticDsv.Reset();mapStaticBase.Reset();ResetLitBounds();ResetTileRefresh();mapStaticScratch={};mapSlotCount=0;
    mapStaticKey=0;mapStaticDone=0;mapStaticMatrices.clear();mapFaceDynamic.clear();mapFaceRefresh.clear();mapRefreshFrame=0;mapFoliageFaces.clear();mapFoliageDraws.clear();
    skyArray.Reset();skyDepth.Reset();skyDsv.Reset();skyArrayView.Reset();skyKey=0;skyDone=0;
    skyboxState=State{};skyboxSunTexture.Reset();skyboxSunDepth.Reset();skyboxSunView.Reset();skyboxSunReady=false; }
void BeginShadowMaps(ID3D11Device *device) {
    EndShadowMaps();
    if (!device)
        return;
    state.device = device;
    static HMODULE compiler = EnvironmentShaderCompiler();
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    const char *shader = "cbuffer Draw:register(b3){row_major float4x4 transform;} float4 Main(float3 "
                         "p:POSITION):SV_Position{return mul(transform,float4(p,1));}";
    ComPtr<ID3DBlob> code, errors;
    if (!compile || FAILED(CompileEnvironmentShader(shader, strlen(shader), "EnvironmentShadowDepth", nullptr, "Main",
                                   "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, &code, &errors))) {
        MvmDebugLog_LinefAlways("environment.shadow", "depth shader failed: %s",
                                errors ? static_cast<const char *>(errors->GetBufferPointer())
                                       : "compiler unavailable");
        return;
    }
    D3D11_INPUT_ELEMENT_DESC element{
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    if (FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                          &state.vertex)) ||
        FAILED(device->CreateInputLayout(&element, 1, code->GetBufferPointer(), code->GetBufferSize(),
                                         &state.layout)) ||
        !Buffer(sizeof(Constants), D3D11_BIND_CONSTANT_BUFFER, state.frame) ||
        !Buffer(sizeof(Matrix), D3D11_BIND_CONSTANT_BUFFER, state.draw))
        return;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.SlopeScaledDepthBias = 1;
    if (FAILED(device->CreateDepthStencilState(&depth, &state.depth)) ||
        FAILED(device->CreateRasterizerState(&raster, &state.raster)))
        return;
    raster.DepthClipEnable = FALSE;
    // Sun receiver-plane filtering handles slopes without detaching contact shadows.
    raster.SlopeScaledDepthBias = 0;
    if (FAILED(device->CreateRasterizerState(&raster, &state.sunRaster)))
        return;
    const std::string cutout=std::string(FoliageDeformationShader)+R"(
        cbuffer Draw:register(b3){row_major float4x4 transform;float4 settings;float4 sway;float4 flutter;}
        cbuffer NativeClock:register(b5){float4 clockData[19];}
        cbuffer NativeWind:register(b6){float4 windData[19];}
        Texture2D<float> alphaMap:register(t0);SamplerState alphaSampler:register(s0);
        struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;nointerpolation float cutoff:TEXCOORD1;};
        V VS(float3 p:POSITION,float2 uv:TEXCOORD0,float3 color:COLOR0){
            if(settings.y>0)p=FoliagePosition(p,color,sway,flutter,clockData[18].w,windData[17].xyz,windData[18].x);
            V o;o.p=mul(transform,float4(p,1));o.uv=uv;o.cutoff=settings.x;return o;}
        void PS(V i){clip(alphaMap.SampleLevel(alphaSampler,i.uv,0)-i.cutoff);}
    )";
    ComPtr<ID3DBlob> cutoutVS,cutoutPS;
    if(FAILED(CompileEnvironmentShader(cutout.c_str(),cutout.size(),"EnvironmentCutoutShadow",nullptr,"VS","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&cutoutVS,&errors))||
       FAILED(CompileEnvironmentShader(cutout.c_str(),cutout.size(),"EnvironmentCutoutShadow",nullptr,"PS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&cutoutPS,&errors))){
        MvmDebugLog_LinefAlways("environment.shadow","cutout shader failed: %s",errors?static_cast<const char*>(errors->GetBufferPointer()):"unknown");
        return;
    }
    D3D11_INPUT_ELEMENT_DESC elements[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,20,D3D11_INPUT_PER_VERTEX_DATA,0}};
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(device->CreateVertexShader(cutoutVS->GetBufferPointer(),cutoutVS->GetBufferSize(),nullptr,&state.cutoutVertex))||
       FAILED(device->CreatePixelShader(cutoutPS->GetBufferPointer(),cutoutPS->GetBufferSize(),nullptr,&state.cutoutPixel))||
       FAILED(device->CreateInputLayout(elements,3,cutoutVS->GetBufferPointer(),cutoutVS->GetBufferSize(),&state.cutoutLayout))||
       FAILED(device->CreateSamplerState(&sampler,&state.cutoutSampler))||
       !Buffer(sizeof(CutoutDraw),D3D11_BIND_CONSTANT_BUFFER,state.cutoutDraw))return;
    state.ready = true;
    BeginAnimatedCasters(device);
    MvmDebugLog_LinefAlways("environment.shadow",
                            "ready=1 localResolution=%u sunResolution=%u maxShadowLights=%u", LocalResolution,
                            SunResolution, MaxShadowLights);
}
void PrepareShadowMaps(ID3D11DeviceContext *ctx, const Frame &f, bool viewmodel, bool requireWorld) {
    auto &c = state.constants;
    c = Constants{};
    state.receiverWanted=viewmodel;
    for (auto &l : c.light)
        l.x = -1;
    c.info = {0, f.scene.climate[CL_shadowBias], float(LocalResolution), float(SunResolution)};
    // The sky map and the cover volume serve falling rain, snow and ash, and snow lying on the ground
    // with nothing falling: it must not lie indoors either.
    // Only the composite reads them: the relight pass and the 3D skybox prepare their own shadow
    // state through here (requireWorld) and must not draw a sky map or restart the cover bake.
    // The ground fog reads them too: fog under a roof or inside a stack of crates is lit by the sky
    // only as far as open air reaches it (GroundFogSkyOpen).
    bool shelter=!requireWorld&&f.scene.weather.shelter&&((f.scene.weather.type>=1&&f.scene.weather.type<=3&&f.scene.weather.count>0)||
        f.scene.climate[CL_snow]>0||(f.scene.climate[CL_enabled]>0&&f.scene.climate[CL_groundFog]>0));
    c.shelterInfo={shelter?-1.f:0.f,float(SunResolution),1.f,0};
    if (!state.ready)
        return;
    auto upload = [&]() { ctx->UpdateSubresource(state.frame.Get(), 0, nullptr, &c, 0, 0); };
    auto climate = EffectiveClimate(f.scene.climate);
    bool props = false;
    for (const auto &o : f.scene.objects)
        if (o.enabled && !o.IsLight())
            props = true;
    const bool lighting = f.relight.enabled || f.proof.enabled;
    bool sun = ClimateSamplesSunShadow(climate, lighting, props || ShadowCascadeView());
    // The borrowed map-light and proof passes switch cast shadows off and never ask for the sun.
    if (MvmDebugLog_Active() && climate[CL_contactShadows] > 0) {
        static int lastSun = -1;
        if (lastSun != int(sun)) {
            lastSun = int(sun);
            MvmDebugLog_LinefAlways("environment.shadow",
                "sun=%d enabled=%.0f fog=%.2f shafts=%.0f sky=%.0f clouds=%.0f lighting=%d props=%d",
                int(sun), climate[CL_enabled], climate[CL_fogDensity], climate[CL_lightShafts], climate[CL_sky],
                climate[CL_clouds], int(lighting), int(props));
        }
    }
    int requested = 0;
    for (const auto &o : f.scene.objects)
        if (o.enabled && o.IsLight() && o.shadows)
            ++requested;
    if (!f.occlusion || (!requireWorld && !sun && !requested && !shelter)) {
        RequestFoliageInputs(false);
        RequestAnimatedCasters(f.proof.enabled);
        upload();
        return;
    }
    // Retain allocations between camera-mode switches, but never expose stale
    // contents. Every receiver layer is keyed to the same geometry/pose cache.
    // Native relighting owns the near cascades. This pass then samples only its
    // far sun layer for weather/coatings; three unused 4096-square layers cost
    // 192 MiB. Keep all four when this pass actually prepares the near cascades.
    if(viewmodel&&sun&&ReceiverTexture(true,climate[CL_relighting]>0?4u:1u))c.viewmodelShadowInfo.x=1;
    if(viewmodel&&requested&&ReceiverTexture(false))c.viewmodelShadowInfo.y=1;
    if (state.world != f.occlusion) {
        state.world.reset();
        state.worldVertices.Reset();
        state.worldIndices.Reset();
        state.worldRanges.clear();
        state.casters.clear();
        for(auto &cache:state.cached){cache.valid=false;cache.composed=false;}
        std::fill(std::begin(state.localKeys), std::end(state.localKeys), 0);
        state.sunKey = 0;state.shelterKey=0;state.nearSunKey=0;
        RigidShadowGeometry scratch;ShadowSpatialIndices worldIndices;
        const auto rigid=[&]()->std::optional<RigidShadowView>{
            // A whole map's geometry is large. Near the commit limit (8x MSAA at 1440p under
            // RenderDoc, 2026-10-13) an uncaught bad_alloc here closed CS2 outright.
            try{
                const auto source=RigidShadowSource(*f.occlusion,scratch);
                worldIndices=PartitionShadowIndices(source.indices,[&](uint32_t i){return source.positions[i];});
                return source;
            }catch(const std::bad_alloc&){return std::nullopt;}
        }();
        if(!rigid){
            MvmDebugLog_LinefAlways("environment.shadow","map geometry allocation failed: vertices=%zu indices=%zu",
                f.occlusion->positions.size(),f.occlusion->indices.size());
            upload();
            return;
        }
        if (!Buffer(UINT(rigid->positions.size() * sizeof(Vec3)), D3D11_BIND_VERTEX_BUFFER,
                    state.worldVertices, rigid->positions.data()) ||
            !Buffer(UINT(worldIndices.indices.size() * sizeof(uint32_t)), D3D11_BIND_INDEX_BUFFER,
                    state.worldIndices, worldIndices.indices.data())) {
            MvmDebugLog_LinefAlways("environment.shadow", "map upload failed");
            upload();
            return;
        }
        state.worldRanges=std::move(worldIndices.ranges);
        if(f.occlusion->visual){
            size_t triangles=0;
            CasterAlphaImageIndex alphaIndex;
            std::vector<ComPtr<ID3D11ShaderResourceView>> alphaViews;
            uint64_t alphaBytes=0,uniqueAlphaBytes=0;
            for(const auto &source:f.occlusion->visual->batches){
                if(f.occlusion->visual->renderGeometry&&source.cutoff<=0&&!source.animation.Active())continue;
                GpuCaster caster;caster.count=UINT(source.indices.size());caster.cutoff=source.cutoff;caster.animation=source.animation;
                // Moving foliage needs tighter cells than rigid walls: one broad leaf
                // bounds block can otherwise mark many distant fixture faces dirty.
                auto indices=PartitionShadowIndices(source.indices,[&](uint32_t i){return source.vertices[i].position;},
                                                    source.animation.Active()?128.f:512.f);
                caster.ranges=std::move(indices.ranges);
                D3D11_TEXTURE2D_DESC td{};td.Width=source.width;td.Height=source.height;
                td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8_UNORM;
                td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA initial{source.alpha.data(),source.width,0};
                ComPtr<ID3D11Texture2D> texture;
                D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                sd.AddressU=D3D11_TEXTURE_ADDRESS_MODE(source.addressU+1);
                sd.AddressV=D3D11_TEXTURE_ADDRESS_MODE(source.addressV+1);
                sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sd.MaxLOD=D3D11_FLOAT32_MAX;
                if(!Buffer(UINT(source.vertices.size()*sizeof(CasterVertex)),D3D11_BIND_VERTEX_BUFFER,caster.vertices,source.vertices.data())||
                   !Buffer(UINT(indices.indices.size()*4),D3D11_BIND_INDEX_BUFFER,caster.indices,indices.indices.data())||
                   FAILED(state.device->CreateSamplerState(&sd,&caster.sampler))){
                    state.casters.clear();upload();return;
                }
                const auto fingerprint=CasterAlphaFingerprint(source.width,source.height,source.alpha);
                const unsigned image=alphaIndex.Intern(source.width,source.height,source.alpha,fingerprint);
                alphaBytes+=source.alpha.size();
                if(image==alphaViews.size()){
                    if(FAILED(state.device->CreateTexture2D(&td,&initial,&texture))||
                       FAILED(state.device->CreateShaderResourceView(texture.Get(),nullptr,&caster.alpha))){
                        state.casters.clear();upload();return;
                    }
                    char name[96];sprintf_s(name,"SourceMVM caster alpha %ux%u %016llx",source.width,source.height,fingerprint);
                    texture->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(std::strlen(name)),name);
                    alphaViews.push_back(caster.alpha);uniqueAlphaBytes+=source.alpha.size();
                }else caster.alpha=alphaViews[image];
                triangles+=source.indices.size()/3;state.casters.push_back(std::move(caster));
            }
            MvmDebugLog_LinefAlways("environment.shadow.alpha.shared","batches=%zu textures=%u bytes=%llu savedBytes=%llu exactTexels=1",
                state.casters.size(),alphaIndex.Size(),uniqueAlphaBytes,alphaBytes-uniqueAlphaBytes);
            size_t chunks=0;for(const auto& caster:state.casters)chunks+=caster.ranges.size();
            MvmDebugLog_LinefAlways("environment.shadow","visualCasters batches=%zu triangles=%zu chunks=%zu collisionChunks=%zu pose=native-inputs alphaTest=1",state.casters.size(),triangles,chunks,state.worldRanges.size());
            MvmDebugLog_LinefAlways("environment.shadow.geometry","renderGeometry=%u rigidTriangles=%zu rigidChunks=%zu physicsHulls=%u",
                unsigned(f.occlusion->visual->renderGeometry),rigid->indices.size()/3,state.worldRanges.size(),unsigned(!f.occlusion->visual->renderGeometry));
        }
        state.world = f.occlusion;
    }
    uint64_t key = 1469598103934665603ull;
    auto ptr = state.world.get();
    key = Hash(key, &ptr, sizeof(ptr));
    bool animated=false;for(const auto &caster:state.casters)animated|=caster.animation.Active();
    if(animated&&ctx->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE)
        state.windValid=ReadFoliageWind(ctx,state.wind,state.windStrength);
    RequestFoliageInputs(animated);unsigned long long serial=0;
    state.animate=animated&&BindFoliageInputs(ctx,serial);
    float maximumPadding=0;
    for(auto& caster:state.casters){
        caster.padding=!caster.animation.Active()||!state.animate?0:state.windValid?
            FoliageDisplacementRadius(caster.animation,state.wind,state.windStrength):std::numeric_limits<float>::infinity();
        // Round outwards so slowly drifting native wind reuses conservative face
        // membership instead of re-running the entire map's culling every frame.
        if(std::isfinite(caster.padding)&&caster.padding>0)caster.padding=std::ceil(caster.padding/8)*8;
        maximumPadding=(std::max)(maximumPadding,caster.padding);
    }
    uint64_t pose=Hash(state.animate?serial:0,&state.animate,sizeof(state.animate));
    if(animated){
        static ULONGLONG last=0;auto now=GetTickCount64();
        if(now-last>=1000&&MvmDebugLog_Active()){
            last=now;MvmDebugLog_LinefAlways("environment.foliage","animated=%d nativeGeneration=%llu cameraIndependent=1",int(state.animate),serial);
        }
    }
    Vec3 low = state.world->low, high = state.world->high;
    if(state.world->visual){
        auto a=state.world->visual->low,b=state.world->visual->high;
        low={(std::min)(low.x,a.x),(std::min)(low.y,a.y),(std::min)(low.z,a.z)};
        high={(std::max)(high.x,b.x),(std::max)(high.y,b.y),(std::max)(high.z,b.z)};
        // The native material can move vertices beyond the rest-pose bounds.
        if(animated){const float extent=std::isfinite(maximumPadding)?maximumPadding:256.f;
            low=low-Vec3{extent,extent,extent};high=high+Vec3{extent,extent,extent};}
    }
    RequestAnimatedCasters(true);
    const bool posed=HasAnimatedCasterGeometry();
    // First-person receivers use environment geometry only. Native actor
    // animation must not introduce third-person self shadows on the viewmodel.
    state.receiverPose=pose;
    // Scene assets are environment casters too. Refresh the receiver layer while
    // they are captured, and once more after the last one goes (no frozen prop
    // shadow); a frame with players only keeps reusing the environment layer.
    const bool sceneCasters=HasAnimatedSceneCasters();
    if(sceneCasters||state.sceneCastersLast){const auto generation=AnimatedCasterGeneration();
        state.receiverPose=Hash(state.receiverPose,&generation,sizeof(generation));}
    state.sceneCastersLast=sceneCasters;
    // Also clear the previous pose on the first empty frame: no frozen shadows
    // when an actor disappears or an engine visibility decision stops its draws.
    if(posed||state.posedLast){animated=true;auto generation=AnimatedCasterGeneration();pose=Hash(pose,&generation,sizeof(generation));}
    state.posedLast=posed;
    std::set<const Mesh *> used;
    for (const auto &o : f.scene.objects)
        if (o.enabled && !o.IsLight() && o.mesh) {
            auto mesh = o.mesh.get();
            used.insert(mesh);
            key = Hash(key, &mesh, sizeof(mesh));
            auto model = ModelMatrix(o.position, o.angles, o.scale);
            key = Hash(key, model.data(), sizeof(model));
            if (!state.props.count(mesh)) {
                GpuMesh gpu;
                gpu.source = o.mesh;
                if (!Buffer(UINT(o.mesh->vertices.size() * sizeof(Vertex)), D3D11_BIND_VERTEX_BUFFER,
                            gpu.vertices, o.mesh->vertices.data())) {
                    upload();
                    return;
                }
                state.props.emplace(mesh, std::move(gpu));
            }
            for (const auto &v : o.mesh->vertices) {
                auto p = Transform(model, v.position);
                low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
            }
        }
    for (auto it = state.props.begin(); it != state.props.end();)
        if (!used.count(it->first))
            it = state.props.erase(it);
        else
            ++it;
    state.worldKey = key;
    unsigned draws = 0;
    int light = 0, slot = 0;
    for (const auto &o : f.scene.objects)
        if (o.enabled && o.IsLight() && light < 16) {
            if (o.shadows && slot < int(MaxShadowLights) && Textures(false)) {
                bool point = o.kind == "point";
                int first = slot * 6, count = point ? 6 : o.kind == "area" ? 4 : 1;
                Vec3 direction, r, u;
                CameraBasis(o.angles, direction, r, u);
                uint64_t hash = Hash(key, &o.position, sizeof(o.position));
                hash = Hash(hash, &direction, sizeof(direction));
                hash = Hash(hash, &o.range, sizeof(o.range));
                hash = Hash(hash, &o.cone, sizeof(o.cone));
                hash = Hash(hash, &o.shadowNearClip, sizeof(o.shadowNearClip));
                hash = Hash(hash, &point, sizeof(point));
                hash=Hash(hash,&o.studio.width,sizeof(float)); hash=Hash(hash,&o.studio.height,sizeof(float));
                hash=Hash(hash,&count,sizeof(count)); hash=Hash(hash,&r,sizeof(r)); hash=Hash(hash,&u,sizeof(u));
                c.light[light] = {float(first), point ? 1.f : o.kind=="area"?2.f:0.f, o.studio.sourceRadius, o.range};
                std::array<Matrix,6> faceMatrices{};std::array<uint64_t,6> faceHashes{};
                for (int face = 0; face < count; ++face) {
                    auto matrix =
                        LocalShadowMatrix(o.kind=="area"?AreaLightSample(o.position,r,u,o.studio.width,o.studio.height,face):o.position, point ? PointShadowDirection(face) : direction,
                                          point ? 90 : o.cone, o.range,o.shadowNearClip);
                    if(requireWorld&&!point){
                        for(const auto& source:f.relight.lights)
                            if(Relight::HasFixtureProjection(source)&&source.position.x==o.position.x&&source.position.y==o.position.y&&source.position.z==o.position.z){
                                matrix=Relight::FixtureProjection(source);break;}
                        hash=Hash(hash,matrix.data(),sizeof(Matrix));
                    }
                    // The map atlas samples these faces with a filter that reaches across the face
                    // edge: render the budgeted cube faces with the same guard band as the static
                    // tier (the exact matrix is restored when the atlas is packed).
                    if (requireWorld) Relight::GuardFace(matrix.data(), Relight::LocalFaceGuard);
                    faceMatrices[face]=matrix;faceHashes[face]=hash;
                }
                // Moving actors are drawn into every face; cull their triangle blocks per face first.
                if(animated&&ShadowCasterCullingWide.load(std::memory_order_relaxed))
                    PrepareAnimatedCasterCulling(ctx,faceMatrices.data(),unsigned(count));
                for (int face = 0; face < count; ++face) {
                    const auto& matrix=faceMatrices[face];const auto faceHash=faceHashes[face];
                    c.local[first + face] = matrix;
                    if (DrawCached(ctx,f,matrix,state.localDsv[first+face].Get(),LocalResolution,first+face,faceHash,pose,animated)) {
                        ++draws;
                    }
                }
                state.localKeys[slot] = hash;
                ++slot;
            }
            ++light;
        }
    // One ladder for every sun cascade, so the tiers stay geometrically spaced against whatever
    // range the user asked for instead of against four compile-time constants.
    const Vec3 mapSpan = high - low;
    const auto cascades = SunCascadeHalfExtents(climate[CL_shadowRange],
                                                std::sqrt(Dot(mapSpan, mapSpan)) * .5f);
    if (sun && Textures(true)) {
        // The outer map is the camera-independent fallback used by fog/GI and
        // receivers beyond the detail cascades. Cropping it around the camera
        // removes their shadows entirely when the camera leaves that region.
        c.sun = SunShadowMatrix(low, high, ClimateSunDirection(climate));
        auto hash = Hash(key, c.sun.data(), sizeof(Matrix));
        if (DrawCached(ctx,f,c.sun,state.sunDsv.Get(),SunResolution,24,hash,pose,animated)) {
            state.sunKey = hash;
            ++draws;
        }
        c.info.x = 1;
        c.sunFilter.x = Relight::NativeFilterTexels;
        if(MvmDebugLog_Active()){
            // The full-map cascade is the one that has to cover everything the finer rings do not,
            // so its texel footprint is the floor on shadow quality at distance. Report it with the
            // bounds it was fitted to, because a visual/skybox union is what makes it coarse.
            static ULONGLONG lastFar=0;const auto now=GetTickCount64();
            if(now-lastFar>3000){lastFar=now;
                const float rowLength=std::sqrt(c.sun[0]*c.sun[0]+c.sun[1]*c.sun[1]+c.sun[2]*c.sun[2]);
                const float texelWorld=rowLength>0?2/(rowLength*float(SunResolution)):0;
                MvmDebugLog_LinefAlways("environment.shadow.far",
                    "span=%.0f resolution=%u texelWorld=%.3f world=(%.0f %.0f %.0f)-(%.0f %.0f %.0f) fitted=(%.0f %.0f %.0f)-(%.0f %.0f %.0f)",
                    rowLength>0?2/rowLength:0,SunResolution,texelWorld,
                    state.world->low.x,state.world->low.y,state.world->low.z,
                    state.world->high.x,state.world->high.y,state.world->high.z,
                    low.x,low.y,low.z,high.x,high.y,high.z);}
        }
        if (climate[CL_relighting] > 0 && Textures(3)) {
            c.nearSun = NearSunShadowMatrix(c.sun, f.camera, cascades.middle, float(SunResolution));
            const auto nearHash = Hash(key, c.nearSun.data(), sizeof(Matrix));
            if (DrawCached(ctx,f,c.nearSun,state.nearSunDsv.Get(),SunResolution,26,nearHash,pose,animated)) {
                state.nearSunKey = nearHash;
                ++draws;
                if(!state.animate&&!posed)MvmDebugLog_Linef("environment.shadow", "nearUpdated=1 resolution=%u span=2048 texelWorld=0.5 camera=(%.2f %.2f %.2f) fullDepthCasters=1",
                                 SunResolution, f.camera.x, f.camera.y, f.camera.z);
            }
            c.nearSunInfo = {1, float(SunResolution), 0, 0};
            if(Textures(5)){
                c.detailSun=NearSunShadowMatrix(c.sun,f.camera,cascades.detail,float(SunResolution));
                const auto detailHash=Hash(key,c.detailSun.data(),sizeof(Matrix));
                if(DrawCached(ctx,f,c.detailSun,state.detailSunDsv.Get(),SunResolution,28,detailHash,pose,animated))++draws;
                c.detailSunInfo={1,float(SunResolution),0,0};
            }
            if(Textures(4)){
                c.closeSun=NearSunShadowMatrix(c.sun,f.camera,cascades.close,float(SunResolution));
                auto closeHash=Hash(key,c.closeSun.data(),sizeof(Matrix));
                if(DrawCached(ctx,f,c.closeSun,state.closeSunDsv.Get(),SunResolution,27,closeHash,pose,animated))++draws;
                c.closeSunInfo={1,float(SunResolution),0,0};
                if(MvmDebugLog_Active()){
                    static ULONGLONG last=0;const auto now=GetTickCount64();
                    if(now-last>3000){last=now;MvmDebugLog_LinefAlways("environment.shadow.detail",
                        "closeSpan=%.0f resolution=%u texelWorld=%.5f bridgeReady=%d bridgeSpan=%.0f bridgeTexel=%.3f middleSpan=%.0f middleTexel=%.3f outerSpan=%.0f outerTexel=%.3f fullDepthCasters=1",
                        2*cascades.close,SunResolution,2*cascades.close/SunResolution,
                        int(c.detailSunInfo.x),2*cascades.detail,2*cascades.detail/SunResolution,
                        2*cascades.middle,2*cascades.middle/SunResolution,
                        2*cascades.outer,cascades.OuterTexelWorld(float(SunResolution)));}
                }
            }
        }
    }
    if(shelter&&Textures(2)){
        // THE SKY MAP looks straight down, whatever the wind: snow on the ground is hours of fall from
        // every direction, and rain sheltered along its slant put stair-stepped wet on every wall. It
        // covers the playable map (the nav mesh's box with room around it), not the world box: distant
        // scenery would spend its texels. Outside it everything counts as open sky.
        // The nav mesh's footprint is worked out once per map; the height range is this frame's, so a
        // prop placed above the map is inside it.
        if(state.skyWorld!=state.world.get()){
            state.skyWorld=state.world.get();state.skyLow={-1e9f,-1e9f,0};state.skyHigh={1e9f,1e9f,0};
            const auto& nav=state.world->navPositions;
            if(!nav.empty()){
                Vec3 a=nav[0],b=a;
                for(const auto& p:nav){a={(std::min)(a.x,p.x),(std::min)(a.y,p.y),a.z};b={(std::max)(b.x,p.x),(std::max)(b.y,p.y),b.z};}
                state.skyLow={a.x-512,a.y-512,0};state.skyHigh={b.x+512,b.y+512,0};
            }
        }
        const Vec3 skyLow{(std::max)(low.x,state.skyLow.x),(std::max)(low.y,state.skyLow.y),low.z},
            skyHigh{(std::min)(high.x,state.skyHigh.x),(std::min)(high.y,state.skyHigh.y),high.z};
        const Vec3 direction{0,0,1};
        c.shelter=PrecipitationShadowMatrix(skyLow,skyHigh,direction);
        auto hash=Hash(key,c.shelter.data(),sizeof(Matrix));
        if(DrawCached(ctx,f,c.shelter,state.shelterDsv.Get(),SunResolution,25,hash,pose,animated)){state.shelterKey=hash;
            if(!state.animate&&!posed)MvmDebugLog_LinefAlways("environment.shelter","updated=1 resolution=%u low=(%.0f %.0f) high=(%.0f %.0f) mapTriangles=%zu props=%zu cameraIndependent=1",SunResolution,skyLow.x,skyLow.y,skyHigh.x,skyHigh.y,state.world->indices.size()/3,used.size());}
        float rowLength=std::sqrt(c.shelter[0]*c.shelter[0]+c.shelter[1]*c.shelter[1]+c.shelter[2]*c.shelter[2]);
        float texelWorld=rowLength>0?2/(rowLength*float(SunResolution)):2;
        c.shelterDirection={direction.x,direction.y,direction.z,texelWorld};
        // A 10-unit thickness margin (a kerb or a step is not a roof) and a soft edge about 4 units wide.
        c.shelterInfo.z=10;
        c.shelterInfo.w=std::clamp(4/texelWorld,1.f,3.f);
        c.shelterInfo.x=1;
        // The wind's angle: weather comes from upwind and above. Its lean is the horizontal part of
        // that direction, 0 falling straight down and 0.71 at 45 degrees. With nothing falling, snow
        // already on the ground keeps the wind it was set with.
        {const auto& weather=f.scene.weather;
         const Vec3 from=PrecipitationSourceDirection(weather.windX,weather.windY,weather.speed);
         c.coverWind={from.x,from.y,from.z,std::sqrt(from.x*from.x+from.y*from.y)};}
        const auto cover=PrepareWeatherCover(state.device.Get(),f.occlusion);
        state.coverSrv=cover.view;
        if(cover.view){
            c.coverLow={cover.low.x,cover.low.y,cover.low.z,1};
            c.coverScale={cover.perUnit.x,cover.perUnit.y,cover.perUnit.z,0};
            c.coverSize={float(cover.nx),float(cover.ny),float(cover.nz),0};
        }
    }else state.coverSrv.Reset();
    if(viewmodel&&c.closeSunInfo.x>0&&AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel)&&Textures(6)){
        ctx->ClearDepthStencilView(state.viewmodelSunDsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        Draw(ctx,f,c.closeSun,state.viewmodelSunDsv.Get(),SunResolution,true,AnimatedCasterSubset::Viewmodel,false);
        c.viewmodelShadowInfo.z=1;
    }
    upload();
    if(viewmodel&&MvmDebugLog_Active()){
        static ULONGLONG last=0;auto now=GetTickCount64();if(now-last>3000){last=now;
            MvmDebugLog_LinefAlways("environment.shadow.viewmodel",
                "sun=%u local=%u excludedCharacters=%u excludedWeapons=%u worldCasters=%u viewmodelSelf=%u nativeViewmodelVertices=%u foliagePose=%llu",
                UINT(c.viewmodelShadowInfo.x),UINT(c.viewmodelShadowInfo.y),
                AnimatedCasterVertexCount(AnimatedCasterSubset::Characters),
                AnimatedCasterVertexCount(AnimatedCasterSubset::Weapons),
                AnimatedCasterVertexCount(),UINT(c.viewmodelShadowInfo.z),AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel),serial);}}
    if (draws&&!state.animate&&!posed)
        MvmDebugLog_LinefAlways("environment.shadow",
                                "updatedFaces=%u localLights=%d requested=%d sun=%d mapTriangles=%zu "
                                "props=%zu cameraIndependent=1",
                                draws, slot, requested, int(c.info.x), state.world->indices.size() / 3,
                                used.size());
}
bool PreparePlayerMask(ID3D11DeviceContext *ctx, const Matrix &vp, UINT width, UINT height, bool reversed) {
    playerMask.valid = false;
    if (!ctx || !state.ready || !state.device || !width || !height)
        return false;
    RequestAnimatedCasters(true);
    const UINT actors = AnimatedCasterVertexCount(AnimatedCasterSubset::Actors);
    if (MvmDebugLog_Active()) {
        static ULONGLONG last = 0;
        const auto now = GetTickCount64();
        if (now - last > 2000) {
            last = now;
            MvmDebugLog_LinefAlways("environment.playermask", "characters=%u weapons=%u size=%ux%u reversed=%d",
                                    AnimatedCasterVertexCount(AnimatedCasterSubset::Characters),
                                    AnimatedCasterVertexCount(AnimatedCasterSubset::Weapons), width, height, int(reversed));
        }
    }
    if (!actors)
        return false;
    if (!playerMask.texture || playerMask.width != width || playerMask.height != height) {
        playerMask.texture.Reset(); playerMask.dsv.Reset(); playerMask.srv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width; td.Height = height; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        if (FAILED(state.device->CreateTexture2D(&td, nullptr, &playerMask.texture)) ||
            FAILED(state.device->CreateDepthStencilView(playerMask.texture.Get(), &dv, &playerMask.dsv)) ||
            FAILED(state.device->CreateShaderResourceView(playerMask.texture.Get(), &sv, &playerMask.srv))) {
            playerMask.texture.Reset(); playerMask.dsv.Reset(); playerMask.srv.Reset();
            MvmDebugLog_LinefAlways("environment.playermask", "target allocation failed %ux%u", width, height);
            return false;
        }
        playerMask.width = width; playerMask.height = height;
    }
    if (!playerMask.forward || !playerMask.reversed) {
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_LESS;
        D3D11_DEPTH_STENCIL_DESC dr = ds; dr.DepthFunc = D3D11_COMPARISON_GREATER;
        if (FAILED(state.device->CreateDepthStencilState(&ds, &playerMask.forward)) ||
            FAILED(state.device->CreateDepthStencilState(&dr, &playerMask.reversed)))
            return false;
    }
    ctx->ClearDepthStencilView(playerMask.dsv.Get(), D3D11_CLEAR_DEPTH, reversed ? 0.f : 1.f, 0);
    auto dsv = playerMask.dsv.Get();
    ctx->OMSetRenderTargets(0, nullptr, dsv);
    ctx->OMSetDepthStencilState((reversed ? playerMask.reversed : playerMask.forward).Get(), 0);
    ctx->OMSetBlendState(nullptr, nullptr, ~0u);
    D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    ctx->RSSetViewports(1, &viewport);
    ctx->RSSetState(state.raster.Get());
    ctx->IASetInputLayout(state.layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(state.vertex.Get(), nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    auto cb = state.draw.Get();
    ctx->VSSetConstantBuffers(3, 1, &cb);
    ctx->UpdateSubresource(cb, 0, nullptr, vp.data(), 0, 0);
    DrawAnimatedCasters(ctx, AnimatedCasterSubset::Actors);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    playerMask.valid = true;
    return true;
}
void BindPlayerMask(ID3D11DeviceContext *ctx) {
    auto srv = playerMask.valid ? playerMask.srv.Get() : nullptr;
    ctx->PSSetShaderResources(31, 1, &srv);
}
// Share immutable shader/mesh resources only; targets, constants and caches have
// independent ownership and never consume a user's light or shadow slot.
ID3D11ShaderResourceView* LampShadowPyramid(ID3D11ShaderResourceView* atlas){
    return g_lampSummary&&atlas&&atlas==mapAtlasView.Get()&&lampPyramid.ready&&!lampPyramid.failed?lampPyramid.builder.View():nullptr;
}
ID3D11ShaderResourceView* SunShadowPyramid(ID3D11ShaderResourceView* atlas){
    return g_sunSummary&&atlas&&atlas==mapAtlasView.Get()&&!mapSunViewmodelActive&&sunPyramid.ready&&!sunPyramid.failed?sunPyramid.builder.View():nullptr;
}
bool SunShadowSummary(){return g_sunSummary;}
void SetSunShadowSummary(bool enabled){
    // Tiles drawn while it was off were not summarized: summarize the whole square again.
    if(enabled&&!g_sunSummary)sunPyramid.builder.Mark(0,0,Relight::LocalOriginX,Relight::AtlasHeight);
    g_sunSummary=enabled;
}
bool LampShadowSummary(){return g_lampSummary;}
void SetLampShadowSummary(bool enabled){g_lampSummary=enabled;}
ID3D11ShaderResourceView* SkyboxSunShadow(Matrix* matrix){
    if(!skyboxSunReady)return nullptr;
    if(matrix)*matrix=skyboxSunMatrix;
    return skyboxSunView.Get();
}
namespace {
// The first-person weapon and arms are drawn in their own pose, not the third-person body's.
// Sampling the ordinary close cascade put that body's arms and gun (posed differently, in the
// same place) as shadows across the first-person gun, and the first-person gun cast nothing
// on the hands holding it. Before the first-person pass the close cascade is redrawn for it:
// the cached static world, other players (the viewer's own body excluded), and the
// first-person pose itself. The world's own draws are already finished by then; the next
// frame redraws the tile normally, so the viewer's own shadow on the ground is unaffected.
constexpr char kViewerShadowShader[]=R"HLSL(
cbuffer Draw:register(b3){row_major float4x4 transform;}
cbuffer Viewer:register(b4){float4 viewer;float4 extent;} // eye xyz, radius | above, below
struct Varying{float4 position:SV_Position;float3 world:TEXCOORD0;};
Varying VS(float3 p:POSITION){Varying o;o.position=mul(transform,float4(p,1));o.world=p;return o;}
void PS(Varying i){float3 d=i.world-viewer.xyz;
    if(dot(d.xy,d.xy)<viewer.w*viewer.w&&d.z<extent.x&&d.z>-extent.y)discard;}
)HLSL";
ComPtr<ID3D11VertexShader> viewerVertex;ComPtr<ID3D11PixelShader> viewerPixel;ComPtr<ID3D11Buffer> viewerConstants;
uint64_t viewerGeneration=~0ull;bool viewerFailed=false;
bool ViewerShaders(ID3D11Device* device){
    if(viewerVertex&&viewerPixel&&viewerConstants)return true;if(viewerFailed)return false;viewerFailed=true;
    ComPtr<ID3DBlob> vs,ps,errors;
    if(FAILED(CompileEnvironmentShader(kViewerShadowShader,sizeof(kViewerShadowShader)-1,"EnvironmentViewerShadow",nullptr,"VS","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&vs,&errors))||
       FAILED(CompileEnvironmentShader(kViewerShadowShader,sizeof(kViewerShadowShader)-1,"EnvironmentViewerShadow",nullptr,"PS","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&ps,&errors))){
        MvmDebugLog_LinefAlways("environment.shadow.viewmodel","shader failed: %s",errors?static_cast<const char*>(errors->GetBufferPointer()):"compiler unavailable");return false;}
    D3D11_BUFFER_DESC b{};b.ByteWidth=32;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&viewerVertex))||
       FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&viewerPixel))||
       FAILED(device->CreateBuffer(&b,nullptr,&viewerConstants)))return false;
    viewerFailed=false;return true;
}
}
unsigned ActorShadowFacesPerFrame(){return g_actorShadowFacesPerFrame;}
void SetActorShadowFacesPerFrame(unsigned faces){g_actorShadowFacesPerFrame=faces;}
void RequestSunCascadeDump(const char* path){g_sunDumpPath=path?path:"";}
unsigned PrepareGlassBeamShadows(ID3D11DeviceContext* immediate,const Matrix* matrices,unsigned count){
    count=(std::min)(count,Relight::GlassBeamTiles);
    if(!immediate||!matrices||!count||!mapBeamFrameReady||!mapAtlas||!mapStaticDsv||!mapStaticTransfer||!proofContext)return 0;
    uint64_t key=1469598103934665603ull;key=Hash(key,matrices,sizeof(Matrix)*count);key=Hash(key,&count,sizeof(count));
    const auto* world=mapBeamFrame.occlusion.get();key=Hash(key,&world,sizeof(world));
    if(key==mapBeamKey)return mapBeamCount;
    struct Scope {Scope(){std::swap(state,proofState);}~Scope(){std::swap(state,proofState);}} scope;
    if(!state.ready)return 0;
    auto ctx=proofContext.Get();ctx->ClearState();
    g_clipOrthographic=true;
    for(unsigned i=0;i<count;++i){
        Draw(ctx,mapBeamFrame,matrices[i],mapStaticDsv.Get(),Relight::StaticSize);
        ctx->OMSetRenderTargets(0,nullptr,nullptr);
        ctx->CopyResource(mapStaticTransfer.Get(),mapStaticDepth.Get());
        unsigned x=0,y=0,size=0;Relight::TileOrigin(Relight::GlassBeamTile+i,x,y,size);
        ctx->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,mapStaticTransfer.Get(),0,nullptr);
        MarkLampTile(x,y,size);
    }
    g_clipOrthographic=false;
    RecordLampPyramid(ctx);
    ComPtr<ID3D11CommandList> list;if(FAILED(ctx->FinishCommandList(FALSE,&list)))return 0;
    {GPU_PASS_SCOPE(immediate,"shadows: glass beams");immediate->ExecuteCommandList(list.Get(),TRUE);}
    mapBeamMatrices.assign(matrices,matrices+count);
    mapBeamKey=key;mapBeamCount=count;
    MvmDebugLog_LinefAlways("environment.shadow.glassbeams","rendered %u stained glass beam shadow maps",count);
    return count;
}
bool PrepareViewmodelSunShadow(ID3D11DeviceContext* immediate){
    if(!immediate||!mapCloseReady||!mapAtlas||!mapSunDepth||!mapSunTexture||!mapSunTransfer||!proofContext)return false;
    const uint64_t generation=AnimatedCasterGeneration();
    if(viewerGeneration==generation)return true;
    viewerGeneration=generation;
    if(!AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel))return false;
    struct Scope {Scope(){std::swap(state,proofState);}~Scope(){std::swap(state,proofState);}} scope;
    if(!state.ready||!state.vertex||!state.layout||!state.device||!ViewerShaders(state.device.Get()))return false;
    auto ctx=proofContext.Get();ctx->ClearState();
    unsigned x=0,y=0,size=0;Relight::TileOrigin(Relight::SunCloseTile,x,y,size);
    // Static environment depth: the close cascade's retained cache, or (no posed casters this
    // frame, so no cache) the tile as it is in the atlas.
    const auto& cache=state.cached[27];
    if(cache.texture&&cache.valid)ctx->CopyResource(mapSunTexture.Get(),cache.texture.Get());
    else{const D3D11_BOX box{x,y,0,x+size,y+size,1};ctx->CopySubresourceRegion(mapSunTexture.Get(),0,0,0,0,mapAtlas.Get(),0,&box);}
    ctx->OMSetRenderTargets(0,nullptr,mapSunDepth.Get());
    ctx->OMSetDepthStencilState(state.depth.Get(),0);ctx->OMSetBlendState(nullptr,nullptr,~0u);
    const D3D11_VIEWPORT viewport{0,0,float(Relight::SunCascadeSize),float(Relight::SunCascadeSize),0,1};ctx->RSSetViewports(1,&viewport);
    ctx->RSSetState(state.sunRaster.Get());
    ctx->IASetInputLayout(state.layout.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto cb=state.draw.Get();ctx->VSSetConstantBuffers(3,1,&cb);
    ctx->UpdateSubresource(cb,0,nullptr,mapCloseMatrix.data(),0,0);
    const float viewer[8]{mapCloseCamera.x,mapCloseCamera.y,mapCloseCamera.z,Relight::ViewerShadowRadius,Relight::ViewerShadowAbove,Relight::ViewerShadowBelow,0,0};
    ctx->UpdateSubresource(viewerConstants.Get(),0,nullptr,viewer,0,0);
    auto vc=viewerConstants.Get();ctx->PSSetConstantBuffers(4,1,&vc);
    ctx->VSSetShader(viewerVertex.Get(),nullptr,0);ctx->PSSetShader(viewerPixel.Get(),nullptr,0);
    DrawAnimatedCasters(ctx,AnimatedCasterSubset::Actors,&mapCloseMatrix);
    const auto anchored=Multiply(mapCloseMatrix,ModelMatrix(mapCloseCamera,{0,0,0},{1,1,1}));
    ctx->UpdateSubresource(cb,0,nullptr,anchored.data(),0,0);
    ctx->VSSetShader(state.vertex.Get(),nullptr,0);ctx->PSSetShader(nullptr,nullptr,0);
    DrawAnimatedCasters(ctx,AnimatedCasterSubset::Viewmodel);
    ctx->OMSetRenderTargets(0,nullptr,nullptr);
    ctx->CopyResource(mapSunTransfer.Get(),mapSunTexture.Get());
    // Keep the world's tile: every body and surface drawn after the first-person block must still
    // read it. Without this, players drawn after the viewmodel read a cascade with the actors near
    // the camera removed and snapped between sunlit and self-shadowed frame to frame (2026-10-13).
    const bool backup=mapSunWorldBackup!=nullptr;
    if(backup){const D3D11_BOX tile{x,y,0,x+size,y+size,1};ctx->CopySubresourceRegion(mapSunWorldBackup.Get(),0,0,0,0,mapAtlas.Get(),0,&tile);}
    ctx->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,mapSunTransfer.Get(),0,nullptr);
    ComPtr<ID3D11CommandList> list;if(FAILED(ctx->FinishCommandList(FALSE,&list)))return false;
    {GPU_PASS_SCOPE(immediate,"shadows: viewmodel sun");immediate->ExecuteCommandList(list.Get(),TRUE);}
    mapSunViewmodelActive=backup;
    // The tile now holds the first-person version: the map pass must rebuild the world's version
    // next frame instead of keeping the tile as current.
    state.cached[27].composed=false;
    if(MvmDebugLog_Active()){static ULONGLONG last=0;const auto now=GetTickCount64();if(now-last>=3000){last=now;
        MvmDebugLog_LinefAlways("environment.shadow.viewmodel","redrawn close=%ux%u actors=%u viewmodel=%u cache=%u camera=(%.0f %.0f %.0f)",size,size,
            AnimatedCasterVertexCount(AnimatedCasterSubset::Actors),AnimatedCasterVertexCount(AnimatedCasterSubset::Viewmodel),unsigned(cache.texture&&cache.valid),
            mapCloseCamera.x,mapCloseCamera.y,mapCloseCamera.z);}}
    return true;
}
void RestoreWorldSunTile(ID3D11DeviceContext* immediate){
    if(!mapSunViewmodelActive||!immediate||!mapAtlas||!mapSunWorldBackup)return;
    mapSunViewmodelActive=false;
    unsigned x=0,y=0,size=0;Relight::TileOrigin(Relight::SunCloseTile,x,y,size);
    immediate->CopySubresourceRegion(mapAtlas.Get(),0,x,y,0,mapSunWorldBackup.Get(),0,nullptr);
}
bool PrepareLightingProofShadows(ID3D11DeviceContext* immediate,const Frame& input,Matrix* matrices,ID3D11ShaderResourceView** atlas) {
    if(!immediate||!matrices||!atlas||!state.ready||!input.occlusion)return false;
    RenderPrivateTextureScope privateTextures;
    if(!RelightPrivateShadowState(input.occlusion))return false;
    struct Scope {Scope(){std::swap(state,proofState);}~Scope(){std::swap(state,proofState);}} scope;
    if(!state.ready){
        if(!Buffer(sizeof(Constants),D3D11_BIND_CONSTANT_BUFFER,state.frame)||!Buffer(sizeof(Matrix),D3D11_BIND_CONSTANT_BUFFER,state.draw)||!Buffer(sizeof(CutoutDraw),D3D11_BIND_CONSTANT_BUFFER,state.cutoutDraw))return false;
        state.ready=true;
    }
    if(!proofAtlasView||!proofSunDepth||!proofTransfer){
        D3D11_TEXTURE2D_DESC d{};d.Width=4096;d.Height=2048;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32_TYPELESS;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sr{};sr.Format=DXGI_FORMAT_R32_FLOAT;sr.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sr.Texture2D.MipLevels=1;
        if(FAILED(state.device->CreateTexture2D(&d,nullptr,&proofAtlas))||FAILED(state.device->CreateShaderResourceView(proofAtlas.Get(),&sr,&proofAtlasView)))return false;
        // D3D11 depth resources only permit whole-subresource copies with zero
        // destination offsets. Transfer into a same-size non-depth texture first;
        // packing that texture into the atlas then uses ordinary texture copies.
        d.Width=d.Height=1024;
        if(FAILED(state.device->CreateTexture2D(&d,nullptr,&proofTransfer)))return false;
        d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D32_FLOAT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        if(FAILED(state.device->CreateTexture2D(&d,nullptr,&proofSun))||FAILED(state.device->CreateDepthStencilView(proofSun.Get(),&ds,&proofSunDepth)))return false;
    }
    Frame frame=input;frame.proof.enabled=false;frame.scene.weather.type=0;
    frame.scene.climate[CL_contactShadows]=0;frame.scene.climate[CL_sunPower]=0;
    frame.scene.objects.erase(std::remove_if(frame.scene.objects.begin(),frame.scene.objects.end(),[](const Object& o){return o.IsLight();}),frame.scene.objects.end());
    Object lamp;lamp.kind="point";lamp.position=input.proof.position;lamp.range=input.proof.range;lamp.shadows=true;frame.scene.objects.push_back(lamp);
    auto ctx=proofContext.Get();ctx->ClearState();PrepareShadowMaps(ctx,frame,false);
    if(state.world!=input.occlusion||!state.local||state.constants.light[0].x!=0){ctx->ClearState();ComPtr<ID3D11CommandList> discarded;ctx->FinishCommandList(FALSE,&discarded);return false;}
    ctx->OMSetRenderTargets(0,nullptr,nullptr);
    for(UINT face=0;face<6;++face){
        matrices[face]=state.constants.local[face];
        ctx->CopySubresourceRegion(proofTransfer.Get(),0,0,0,0,state.local.Get(),face,nullptr);
        ctx->CopySubresourceRegion(proofAtlas.Get(),0,(face%3)*1024,(face/3)*1024,0,proofTransfer.Get(),0,nullptr);
    }
    auto low=input.occlusion->low,high=input.occlusion->high;
    if(input.occlusion->visual){const auto& v=*input.occlusion->visual;low={std::min(low.x,v.low.x),std::min(low.y,v.low.y),std::min(low.z,v.low.z)};high={std::max(high.x,v.high.x),std::max(high.y,v.high.y),std::max(high.z,v.high.z)};}
    matrices[6]=SunShadowMatrix(low,high,ClimateSunDirection(input.scene.climate));
    auto hash=Hash(state.localKeys[0],matrices[6].data(),sizeof(Matrix));
    const bool animated=state.animate||HasAnimatedCasterGeometry();
    DrawCached(ctx,frame,matrices[6],proofSunDepth.Get(),1024,24,hash,AnimatedCasterGeneration(),animated);
    ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->CopyResource(proofTransfer.Get(),proofSun.Get());
    ctx->CopySubresourceRegion(proofAtlas.Get(),0,3072,0,0,proofTransfer.Get(),0,nullptr);
    ComPtr<ID3D11CommandList> list;if(FAILED(ctx->FinishCommandList(FALSE,&list)))return false;
    {GPU_PASS_SCOPE(immediate,"shadows: lighting proof");immediate->ExecuteCommandList(list.Get(),TRUE);}
    static uint64_t inspected=~0ull;
    if(MvmDebugLog_Active()&&inspected!=input.proof.generation){
        inspected=input.proof.generation;
        D3D11_TEXTURE2D_DESC d{};d.Width=6;d.Height=d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32_TYPELESS;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> samples;
        if(SUCCEEDED(state.device->CreateTexture2D(&d,nullptr,&samples))){
            for(UINT face=0;face<6;++face){const UINT x=(face%3)*1024+512,y=(face/3)*1024+512;
                const D3D11_BOX box{x,y,0,x+1,y+1,1};immediate->CopySubresourceRegion(samples.Get(),0,face,0,0,proofAtlas.Get(),0,&box);}
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if(SUCCEEDED(immediate->Map(samples.Get(),0,D3D11_MAP_READ,0,&mapped))){
                const auto z=static_cast<const float*>(mapped.pData);
                MvmDebugLog_LinefAlways("environment.proof.shadow","generation=%llu origin=(%.2f %.2f %.2f) centerDepth=(%.7f %.7f %.7f %.7f %.7f %.7f) triangles=%zu actors=%zu",
                    inspected,input.proof.position.x,input.proof.position.y,input.proof.position.z,z[0],z[1],z[2],z[3],z[4],z[5],state.world->indices.size()/3,size_t(AnimatedCasterVertexCount()));
                immediate->Unmap(samples.Get(),0);
            }
        }
    }
    *atlas=proofAtlasView.Get();return true;
}
bool SkyVisibilityMaps(ID3D11ShaderResourceView** maps,Matrix* matrices) {
    skyIdleFrames=0;
    if(!maps||!matrices||!skyArrayView||skyDone<SkyVisibilityDirections)return false;
    *maps=skyArrayView.Get();
    for(unsigned i=0;i<SkyVisibilityDirections;++i)matrices[i]=skyMatrices[i];
    return true;
}
void BindShadowMaps(ID3D11DeviceContext *ctx, bool compute) {
    auto cb = state.ready ? state.frame.Get() : nullptr;
    if (compute)
        ctx->CSSetConstantBuffers(2, 1, &cb);
    else
        ctx->PSSetConstantBuffers(2, 1, &cb);
    auto self=state.viewmodelSunSrv.Get();
    if(compute)ctx->CSSetShaderResources(21,1,&self);else ctx->PSSetShaderResources(21,1,&self);
    ID3D11ShaderResourceView* receiver[]={state.receiverSunSrv.Get(),state.receiverLocalSrv.Get()};
    if(compute)ctx->CSSetShaderResources(19,2,receiver);else ctx->PSSetShaderResources(19,2,receiver);
    if(!compute){auto shelter=state.shelterSrv.Get();ctx->PSSetShaderResources(13,1,&shelter);
        auto cover=state.coverSrv.Get();ctx->PSSetShaderResources(30,1,&cover);}
    auto nearSun = state.nearSunSrv.Get();
    auto closeSun=state.closeSunSrv.Get();
    auto detailSun=state.detailSunSrv.Get();
    if(compute)ctx->CSSetShaderResources(18,1,&detailSun);else ctx->PSSetShaderResources(18,1,&detailSun);
    if(compute)ctx->CSSetShaderResources(16,1,&closeSun);else ctx->PSSetShaderResources(16,1,&closeSun);
    if (compute) ctx->CSSetShaderResources(15, 1, &nearSun);
    else ctx->PSSetShaderResources(15, 1, &nearSun);
    ID3D11ShaderResourceView *srv[] = {state.localSrv.Get(), state.sunSrv.Get()};
    if (compute)
        ctx->CSSetShaderResources(5, 2, srv);
    else
        ctx->PSSetShaderResources(5, 2, srv);
}
} // namespace Filmmaker::Environment

