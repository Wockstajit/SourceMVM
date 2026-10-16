#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentGi.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentStorm.h"
#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentRender.h"
#include <wrl/client.h>
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentSurfaceMath.h"
#include <iostream>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <d3dcompiler.h>
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowMaps.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentAnimatedCasters.h"
using Microsoft::WRL::ComPtr;
void RenderShaderCost_OwnWork(bool){}
using namespace Filmmaker::Environment;
namespace Filmmaker {
// A standalone device has no engine effect draws to classify.
bool SkyGpuProbe_IsPlayersOnlyGreenSkyDraw(ID3D11DeviceContext*,unsigned,unsigned){return false;}
bool MvmDebugLog_Active() { return true; }
void MvmDebugLog_Linef(const char *, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    puts("");
    va_end(a);
}
void MvmDebugLog_LinefAlways(const char *, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    puts("");
    va_end(a);
}
} // namespace Filmmaker
namespace Filmmaker::Environment {
unsigned drawn = 0;
// The native CS2 rain lives in the hook (engine particle manager); off in a standalone device test.
bool NativeRainActive() { return false; }
void SetRenderStatus(const char *status, bool frame) {
    std::cout << status << '\n';
    if (frame)
        ++drawn;
}
} // namespace Filmmaker::Environment
void Check(bool value, const char *message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // Exercise real bytecode reuse, corruption recovery and changed-source invalidation.
    wchar_t temporaryRoot[MAX_PATH];
    Check(GetTempPathW(MAX_PATH,temporaryRoot)>0,"Read cache test paths");
    auto testRoot=std::filesystem::path(temporaryRoot)/(L"mvm-shader-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    EnvironmentShaderCachePaths testPaths{testRoot/L"HLAE"/L"source_mvm"/L"shader-cache",{}};
    const char *testSource="float4 Main():SV_Target{return float4(.2,.3,.4,1);}";
    bool hit=false;ComPtr<ID3DBlob> first,cached,recovered,changed,errors;
    auto compileTest=[&](const char *source,ComPtr<ID3DBlob>& result){return CompileEnvironmentShader(source,strlen(source),"CacheTest",nullptr,"Main","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,&result,&errors,&hit,&testPaths);};
    Check(SUCCEEDED(compileTest(testSource,first))&&!hit,"Initial cache miss compiles bytecode");
    Check(SUCCEEDED(compileTest(testSource,cached))&&hit,"Second compile reuses bytecode");
    Check(first->GetBufferSize()==cached->GetBufferSize()&&!memcmp(first->GetBufferPointer(),cached->GetBufferPointer(),first->GetBufferSize()),"Cached bytecode is exact");
    auto cacheFolder=testRoot/L"HLAE"/L"source_mvm"/L"shader-cache";
    for(const auto& entry:std::filesystem::directory_iterator(cacheFolder)){std::fstream file(entry.path(),std::ios::in|std::ios::out|std::ios::binary);file.seekg(-1,std::ios::end);char byte=0;file.get(byte);file.seekp(-1,std::ios::end);file.put(char(static_cast<unsigned char>(byte)^0xff));}
    Check(SUCCEEDED(compileTest(testSource,recovered))&&!hit,"Corrupt cache recompiles");
    Check(first->GetBufferSize()==recovered->GetBufferSize()&&!memcmp(first->GetBufferPointer(),recovered->GetBufferPointer(),first->GetBufferSize()),"Corruption recovery restores bytecode");
    Check(SUCCEEDED(compileTest("float4 Main():SV_Target{return float4(.8,.1,.2,1);}",changed))&&!hit,"Changed source invalidates cache");
    for(const auto& entry:std::filesystem::directory_iterator(cacheFolder))std::filesystem::remove(entry.path());
    std::filesystem::remove(cacheFolder);std::filesystem::remove(cacheFolder.parent_path());
    std::filesystem::remove(cacheFolder.parent_path().parent_path());std::filesystem::remove(testRoot);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level;
    auto driver = argc > 1 && std::string(argv[1]) == "--hardware" ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
    std::cout << "Creating " << (driver == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware") << " device\n";
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, driver, nullptr, 0, nullptr, 0,
                                      D3D11_SDK_VERSION, &device, &level, &context)),
          "Create D3D11 test device");
    const auto preparationStart = GetTickCount64();
    BeginDevice(device.Get(), true);
    Check(GetTickCount64() - preparationStart < 1000, "Background preparation never blocks device creation");
    // Device recreation while compiling must reuse the job, never wait for or destroy it.
    EndDevice();
    BeginDevice(device.Get(), true);
    while (!PumpDevicePreparation() && GetTickCount64() - preparationStart < 600000)
        Sleep(10);
    Check(PumpDevicePreparation(), "Background preparation publishes usable D3D shaders");
    const auto resizeStart = GetTickCount64();
    BeginDevice(device.Get(), true);
    Check(PumpDevicePreparation() && GetTickCount64() - resizeStart < 1000,
          "Same-device resize retains the prepared renderer");
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 128;
    d.Height = 128;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color, depth, staging, resolved;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> dsv;
    Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &color)) &&
              SUCCEEDED(device->CreateRenderTargetView(color.Get(), nullptr, &target)),
          "Create color target");
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &staging)), "Create readback");
    d.Usage = D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags = 0;
    Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &resolved)), "Create resolve target");
    d.Format = DXGI_FORMAT_D32_FLOAT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags = 0;
    Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &depth)) &&
              SUCCEEDED(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv)),
          "Create scene depth");
    Frame frame;
    frame.active = true;
    frame.scene.enabled = true;
    frame.scene.ambient = 1;
    frame.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 4, 0, 0, -1, 0};
    auto read = [&]() {
        std::vector<unsigned char> bytes(128 * 128 * 4);
        D3D11_TEXTURE2D_DESC cd{};
        color->GetDesc(&cd);
        if (cd.SampleDesc.Count > 1) {
            context->ResolveSubresource(resolved.Get(), 0, color.Get(), 0, cd.Format);
            context->CopyResource(staging.Get(), resolved.Get());
        } else
            context->CopyResource(staging.Get(), color.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)), "Read frame");
        for (int y = 0; y < 128; ++y)
            memcpy(bytes.data() + y * 128 * 4, (char *)map.pData + y * map.RowPitch, 128 * 4);
        context->Unmap(staging.Get(), 0);
        return bytes;
    };
    float worldDepthMin = 0, worldDepthMax = 1;
    float background=.2f;
    bool greenSkyMarker=false;
    auto render = [&](float z) {
        float bg[] = {greenSkyMarker?0.f:background, greenSkyMarker?1.f:background,
                      greenSkyMarker?0.f:background, 1};
        context->ClearRenderTargetView(target.Get(), bg);
        context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, z, 0);
        auto t = target.Get();
        auto boundDepth = d.SampleDesc.Count == 1 ? dsv.Get() : nullptr;
        context->OMSetRenderTargets(1, &t, boundDepth);
        Render(context.Get(), target.Get(), dsv.Get(), frame, worldDepthMin, worldDepthMax);
        ComPtr<ID3D11RenderTargetView> restored;
        ComPtr<ID3D11DepthStencilView> restoredDepth;
        context->OMGetRenderTargets(1, &restored, &restoredDepth);
        Check(restored.Get() == target.Get() && restoredDepth.Get() == boundDepth,
              "Engine context restored after scene render");
        return read();
    };
    auto baseline = render(.004f);
    Check(drawn == 1, "Shaders and full pipeline execute");
    Check(baseline[(64 * 128 + 64) * 4] == 51, "Neutral scene preserves pixels");
    {
        // CS2 can leave either view of the same display surface bound after a
        // weapon draw. Procedural radiance must use one display transfer, while
        // the already encoded scene and the engine's original binding survive.
        auto savedColor = color;
        auto savedTarget = target;
        auto savedFrame = frame;
        D3D11_TEXTURE2D_DESC displayDesc{};
        color->GetDesc(&displayDesc);
        displayDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        Check(SUCCEEDED(device->CreateTexture2D(&displayDesc, nullptr, &color)),
              "Create native typeless display surface");
        ComPtr<ID3D11RenderTargetView> linearView, srgbView;
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
        viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        Check(SUCCEEDED(device->CreateRenderTargetView(color.Get(), &viewDesc, &linearView)),
              "Create native linear display view");
        viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        Check(SUCCEEDED(device->CreateRenderTargetView(color.Get(), &viewDesc, &srgbView)),
              "Create native sRGB display view");
        frame.scene.climate[CL_enabled] = 1;
        frame.scene.climate[CL_sky] = 1;
        auto displayRender = [&](ID3D11RenderTargetView *view, float z) {
            float encodedScene[] = {.2f, .3f, .4f, 1};
            context->ClearRenderTargetView(linearView.Get(), encodedScene);
            context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, z, 0);
            context->OMSetRenderTargets(1, &view, dsv.Get());
            Check(Render(context.Get(), view, dsv.Get(), frame, worldDepthMin, worldDepthMax),
                  "Render display transfer fixture");
            ComPtr<ID3D11RenderTargetView> restored;
            context->OMGetRenderTargets(1, &restored, nullptr);
            Check(restored.Get() == view, "Display transfer preserves the engine's original view");
            return read();
        };
        auto srgbSky = displayRender(srgbView.Get(), 0);
        Check(displayRender(linearView.Get(), 0) == srgbSky,
              "Weapon-dependent RTV format cannot change procedural sky color");
        auto srgbScene = displayRender(srgbView.Get(), .004f);
        Check(displayRender(linearView.Get(), .004f) == srgbScene,
              "Both display views preserve identical native scene pixels");
        Check(srgbScene[(64 * 128 + 64) * 4] == 51,
              "Native display color is decoded and encoded exactly once");
        frame.scene.climate[CL_clouds] = 1;
        frame.scene.climate[CL_coverage] = 1;
        frame.angles = {-45, 0, 0};
        frame.time = 30;
        auto srgbClouds = displayRender(srgbView.Get(), 0);
        Check(displayRender(linearView.Get(), 0) == srgbClouds,
              "Weapon-dependent RTV format cannot change volumetric cloud color");
        color = savedColor;
        target = savedTarget;
        frame = savedFrame;
    }
    int climateCenter=(64*128+64)*4;
    frame.scene.climate[CL_enabled]=1;frame.scene.climate[CL_sky]=1;
    auto procedural=render(0);
    Check(procedural[climateCenter+2]>procedural[climateCenter],"Procedural sky colors the actual sky pixels");
    Check(render(.004f)==baseline,"Procedural sky leaves opaque geometry intact");
    frame.angles={-45,0,0};frame.scene.climate[CL_clouds]=1;frame.scene.climate[CL_coverage]=1;
    frame.time=30;
    auto clouds=render(0);
    frame.scene.climate[CL_clouds]=0;auto noClouds=render(0);
    Check(clouds!=noClouds,"Raymarched cloud volume changes the sky");
    frame.scene.climate[CL_clouds]=1;frame.time=40;render(0);frame.time=30;
    Check(render(0)==clouds,"Clouds are deterministic after seeking");
    frame.scene.climate[CL_clouds]=0;auto wallNoClouds=render(.08f);
    frame.scene.climate[CL_clouds]=1;
    Check(render(.08f)==wallNoClouds,"Nearby geometry occludes cloud slab");
    {
        float savedMin=worldDepthMin,savedMax=worldDepthMax;
        worldDepthMin=.05f;worldDepthMax=1.f;
        frame.scene.climate[CL_enabled]=1;frame.scene.climate[CL_sky]=1;
        frame.scene.climate[CL_clouds]=1;frame.scene.climate[CL_coverage]=1;
        frame.angles={-45,0,0};frame.time=30;
        auto leftoverClouds=render(.02f);
        frame.scene.climate[CL_clouds]=0;auto leftoverClear=render(.02f);
        Check(leftoverClouds==leftoverClear,"3D skybox leftover depth keeps city pixels");
        frame.scene.climate[CL_relighting]=1;frame.scene.climate[CL_relight]=1;
        frame.scene.climate[CL_ambient]=.05f;frame.scene.climate[CL_sunRelight]=0;
        auto nightSkybox=render(.02f);
        Check(nightSkybox==leftoverClear,"Environment surface lighting preserves native 3D skybox shading");
        frame.angles={0,0,0};
        frame.scene.climate[CL_sunRelight]=1;frame.scene.climate[CL_contactShadows]=0;
        frame.scene.climate[CL_sunElevation]=20;frame.scene.climate[CL_sunAzimuth]=180;
        auto skyboxSunFront=render(.02f);
        frame.scene.climate[CL_sunAzimuth]=0;auto skyboxSunBack=render(.02f);
        Check(skyboxSunFront==skyboxSunBack,
              "Surface sun direction does not reconstruct skybox materials from final color");
        frame.scene.climate[CL_sunAzimuth]=180;frame.scene.climate[CL_sunElevation]=85;
        auto skyboxSunHigh=render(.02f);
        Check(skyboxSunFront==skyboxSunHigh,
              "Surface sun elevation preserves skybox detail");
        frame.angles={-45,0,0};frame.scene.climate[CL_sunElevation]=35;
        frame.scene.climate[CL_sunAzimuth]=135;
        frame.scene.climate[CL_relighting]=0;frame.scene.climate[CL_relight]=0;
        frame.scene.climate[CL_sunRelight]=.6f;
        frame.scene.climate[CL_clouds]=1;auto skyClouds=render(0);
        frame.scene.climate[CL_clouds]=0;auto skyClear=render(0);
        Check(skyClouds!=skyClear,"Empty sky still receives cloud volume");
        worldDepthMin=savedMin;worldDepthMax=savedMax;
    }
    frame.scene.climate=Climate{};frame.angles={0,0,0};
    frame.scene.climate[CL_snow]=1;
    Check(render(.004f)==baseline,"Snow avoids vertical walls");
    {
        Frame saved=frame;frame=Frame{};frame.active=true;frame.scene.enabled=true;frame.projection=saved.projection;
        Object emitter;emitter.id=1;emitter.kind="area";emitter.position={20,0,0};emitter.range=500;emitter.cone=150;emitter.intensity=1;emitter.color={1,1,1};emitter.studio.width=80;emitter.studio.height=80;
        frame.scene.objects={emitter};auto front=render(.04f);
        frame.scene.objects[0].angles.y=180;auto back=render(.04f);
        Check(front[climateCenter]>back[climateCenter]+20,"Area light is one-sided and illuminates the receiver");
        frame.scene.objects[0].angles.y=0;frame.scene.objects[0].studio.pattern=1;auto window=render(.04f);
        Check(window[climateCenter]+10<front[climateCenter],"Window projector creates an unlit frame at the center");
        frame.scene.objects[0].studio.pattern=0;frame.scene.objects[0].studio.cue=3;frame.time=.5;auto cueOff=render(.04f);
        frame.time=0;auto cueOn=render(.04f);Check(cueOn[climateCenter]>cueOff[climateCenter]+20,"Demo-time light cues affect actual shader illumination");
        frame.time=20.5;Check(render(.04f)==cueOff,"Light cue repeats independently of frame history");
        frame=saved;
    }
    frame.angles={90,0,0};frame.camera={0,0,1000};
    auto snowFloor=render(.004f);
    Check(snowFloor[climateCenter]>150,"Snow covers upward floor");
    {
        // CS2 SMOKE HIDES EVERYTHING WE ADD. The hook hands over CS2's full-resolution OIT absorbance
        // (R32); coverage is 1-exp(-b0). No smoke leaves the frame untouched, dense smoke leaves only
        // the untouched scene, and thin smoke blends between the two.
        auto absorbance=[&](float b0){
            std::vector<float> texels(128*128,b0);
            D3D11_TEXTURE2D_DESC sd{};sd.Width=128;sd.Height=128;sd.MipLevels=1;sd.ArraySize=1;sd.SampleDesc.Count=1;
            sd.Format=DXGI_FORMAT_R32_FLOAT;sd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{texels.data(),128*sizeof(float),0};
            ComPtr<ID3D11Texture2D> tex;ComPtr<ID3D11ShaderResourceView> view;
            Check(SUCCEEDED(device->CreateTexture2D(&sd,&init,&tex))&&SUCCEEDED(device->CreateShaderResourceView(tex.Get(),nullptr,&view)),"Create smoke absorbance");
            SetSmokeAbsorbance(view.Get(),128,128);
            auto r=render(.004f);
            SetSmokeAbsorbance(nullptr,0,0);
            return r;
        };
        const Climate snowy=frame.scene.climate;
        frame.scene.climate=Climate{};auto bare=render(.004f);frame.scene.climate=snowy;
        Check(bare!=snowFloor,"Smoke fixture: the snow is visible to begin with");
        Check(absorbance(0)==snowFloor,"Zero absorbance hides nothing");
        Check(absorbance(40)==bare,"Dense smoke hides the snow coating entirely");
        auto thin=absorbance(.7f);
        Check(thin[climateCenter]<snowFloor[climateCenter]&&thin[climateCenter]>bare[climateCenter],"Thin smoke fades the coating part way");
        Check(render(.004f)==snowFloor,"Clearing the smoke handoff restores the coating");
        Climate relitSmoke=snowy;
        relitSmoke[CL_enabled]=1;relitSmoke[CL_relighting]=1;relitSmoke[CL_relight]=.8f;relitSmoke[CL_sunRelight]=1.5f;
        frame.scene.climate=relitSmoke;
        auto hiddenRelit=absorbance(40);
        frame.scene.climate=snowy;
        Check(hiddenRelit!=absorbance(40),"Flattened lighting reaches smoke-covered pixels");
        frame.scene.climate=snowy;
    }
    frame.scene.climate[CL_enabled]=1;frame.scene.climate[CL_relighting]=1;
    auto litSnow=render(.004f);Check(litSnow[climateCenter]>snowFloor[climateCenter]+10,"Snow responds to enabled sunlight without requiring contrast flattening");
    frame.scene.climate=Climate{};frame.scene.climate[CL_surfaceRadius]=8000;auto floor=render(.004f); // explicit placed height-band fixture
    frame.scene.climate[CL_wet]=1;auto wet=render(.004f);
    Check(wet[climateCenter]<floor[climateCenter],"Wetness darkens a visible floor");
    frame.scene.climate[CL_puddles]=1;frame.scene.climate[CL_puddleHeight]=0;
    auto puddled=render(.004f);
    Check(puddled!=wet,"Shader film lays a standing-water film on flat floors");
    {
        const auto saved=frame.scene.climate;
        frame.scene.climate[CL_enabled]=frame.scene.climate[CL_relighting]=1;
        frame.scene.climate[CL_sunElevation]=-5;frame.scene.climate[CL_sky]=0;
        frame.scene.climate[CL_sunPower]=8;auto belowHorizon=render(.004f);
        frame.scene.climate[CL_sunPower]=0;
        Check(belowHorizon==render(.004f),"Below-horizon sun cannot light wet film or puddle reflections");
        frame.scene.climate=saved;
    }
    frame.scene.climate[CL_puddleHeight]=400;
    Check(render(.004f)==wet,"Local puddle height dries a floor that is outside the band");
    frame.camera={0,0,0};frame.angles={0,0,0};frame.scene.climate=Climate{};
    frame.scene.climate[CL_enabled]=1;frame.scene.climate[CL_sky]=0;
    frame.scene.climate[CL_fogDensity]=.5f;
    auto volume=render(.004f);
    Check(volume!=baseline,"Volumetric scattering reaches image");
    Object volumeLight;volumeLight.id=90;volumeLight.kind="point";volumeLight.position={200,0,0};volumeLight.color={1,0,0};volumeLight.range=800;volumeLight.intensity=5;
    frame.scene.objects.push_back(volumeLight);auto glowing=render(.004f);
    Check(glowing[climateCenter]>volume[climateCenter],"Added light scatters through volumetric fog");
    frame.scene.objects.clear();frame.scene.climate=Climate{};
    frame.scene.climate[CL_vignette]=1;auto finished=render(.004f);
    Check(finished[0]<baseline[0] && finished[climateCenter]==baseline[climateCenter],"Finishing affects corners without altering center");
    frame.scene.climate=Climate{};
    Check(render(.004f)==baseline,"Reset climate restores original pixels exactly");
    Object box;
    box.id = 1;
    box.position = {160, 0, 0};
    box.color = {1, 0, 0};
    box.mesh = Primitive("box");
    frame.scene.objects.push_back(box);
    auto prop = render(.004f);
    int center = (64 * 128 + 64) * 4;
    Check(prop[center] > 60 && prop[center + 1] < 5, "World-space prop renders at projected position");
    {
        Frame saved=frame;frame.scene.climate[CL_ambient]=.3f;auto image=std::make_shared<MaterialImage>();image->width=image->height=2;image->rgba={255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255};
        auto &material=frame.scene.objects[0].material;material.enabled=true;material.albedoImage=image;frame.scene.objects[0].color={1,1,1};
        auto textured=render(.004f);Check(textured[center]>textured[center+1]*3,"sRGB albedo texture changes actual material shading");
        material.albedoImage.reset();Object key;key.id=9;key.kind="point";key.position={60,30,20};key.range=300;key.color={1,1,1};key.intensity=.5f;frame.scene.objects.push_back(key);
        auto diffuse=render(.004f);frame.scene.objects[0].material.metallic=1;auto metal=render(.004f);Check(diffuse!=metal,"Metalness changes material reflection and diffuse response");
        frame.scene.objects[0].material.coat=1;auto coat=render(.004f);Check(coat!=metal,"Clear coat adds a separate specular layer");
        frame.scene.objects[0].material.anisotropy=.8f;auto brushed=render(.004f);frame.scene.objects[0].material.anisotropy=-.8f;Check(render(.004f)!=brushed,"Anisotropy rotates the elongated highlight response");
        frame.scene.objects[0].material=SurfaceMaterial{};frame.scene.objects[0].material.enabled=true;
        auto normal=std::make_shared<MaterialImage>();normal->width=normal->height=1;normal->rgba={240,128,240,255};frame.scene.objects[0].material.normalImage=normal;
        auto bumped=render(.004f);frame.scene.objects[0].material.normalStrength=0;Check(render(.004f)!=bumped,"Tangent normal texture affects lighting and can be neutralized");
        frame.scene.objects[0].cameraVisible=false;auto hidden=render(.004f);frame.scene.objects[0].enabled=false;Check(render(.004f)==hidden,"Camera-hidden prop does not enter the visible color or depth pass");
        frame=saved;
    }
    worldDepthMin = .05f;
    worldDepthMax = .75f;
    Check(render(.05f + .7f * .004f) == prop, "Viewport depth partition preserves prop visibility");
    worldDepthMin = 0;
    worldDepthMax = 1;
    auto occluded = render(.08f);
    Check(occluded == baseline, "Scene depth occludes prop behind wall");
    frame.scene.objects.clear();
    Object light;
    light.id = 2;
    light.kind = "point";
    light.position = {900, 0, 0};
    light.range = 400;
    light.color = {0, 0, 1};
    light.intensity = 3;
    frame.scene.objects.push_back(light);
    auto lit = render(.004f);
    Check(lit[center + 2] > baseline[center + 2] + 20 && lit[center] == baseline[center],
          "Added light reaches visible surface");
    frame.scene.objects[0].kind = "spot";
    frame.scene.objects[0].angles = {0, 180, 0};
    auto away = render(.004f);
    Check(away[center + 2] == baseline[center + 2], "Spotlight facing away does not illuminate wall");
    frame.scene.objects.clear();
    frame.scene.climate[CL_enabled] = 1; frame.scene.climate[CL_fogDensity] = 2;
    frame.scene.fogColor = {1, 0, 0};
    auto fog = render(.004f);
    Check(fog[center] > baseline[center] + 40 && fog[center] > fog[center + 1] * 3, "Unified lit fog applies its color");
    frame.scene.climate[CL_fogStart] = 1200;
    auto clearNear = render(.004f);
    for (int channel=0; channel<3; ++channel)
        Check(clearNear[center+channel] == baseline[center+channel], "Start distance leaves nearer geometry clear");
    frame.scene.climate[CL_fogStart] = 0;
    worldDepthMin = .05f;
    worldDepthMax = .75f;
    auto rangedFog = render(.05f + .7f * .004f);
    for (int channel = 0; channel < 3; ++channel)
        Check(std::abs(int(rangedFog[center + channel]) - int(fog[center + channel])) <= 1,
              "Viewport depth partition reconstructs the same fog distance");
    worldDepthMin = 0;
    worldDepthMax = 1;
    frame.scene.climate[CL_fogDensity] = 0;
    frame.scene.weather.shelter = false; // Explicit legacy particle baseline without map geometry.
    frame.scene.weather.type = 2;
    frame.scene.weather.count = 6000;
    frame.scene.weather.size = 4;
    frame.time = 1123.25;
    auto snow = render(.004f);
    frame.time = 900;
    render(.004f);
    frame.time = 1123.25;
    auto repeated = render(.004f);
    Check(snow == repeated, "Weather reconstructs identical pixels after a seek");
    Check(snow != baseline, "Weather changes visible pixels");
    {
        Frame saved=frame;frame.scene.weather.follow=false;frame.scene.weather.center={100,0,0};frame.scene.weather.radius=64;frame.scene.weather.height=64;frame.scene.weather.windX=frame.scene.weather.windY=0;frame.scene.weather.speed=200;
        frame.scene.weather.type=0;auto dry=render(.004f);frame.scene.weather.type=2;auto exposed=render(.004f);Check(exposed!=dry,"Unsheltered precipitation is visible");
        auto roof=std::make_shared<OcclusionMesh>();roof->positions={{-512,-512,64},{512,-512,64},{512,512,64},{-512,512,64}};roof->indices={0,1,2,0,2,3};roof->low={-512,-512,64};roof->high={512,512,64};
        frame.occlusion=roof;frame.scene.weather.shelter=true;auto sheltered=render(.004f);Check(sheltered==dry,"Offscreen roof blocks precipitation without any lighting feature enabled");
        frame.angles.y=180;render(.004f);frame.angles.y=0;Check(render(.004f)==sheltered,"Roof occlusion survives camera turns");
        auto ground=std::make_shared<OcclusionMesh>(*roof);for(auto &p:ground->positions)p.z=-64;ground->low.z=ground->high.z=-64;frame.occlusion=ground;
        Check(render(.004f)==exposed,"Removing the roof exposes precipitation above the ground");
        // The precipitation lattice spans beyond weather.radius. Cover the mapped
        // field seen by this fixture, including its margin, rather than only 256 units.
        Object canopy;canopy.id=95;canopy.position={100,0,64};canopy.scale={1024,1024,2};canopy.cameraVisible=false;canopy.mesh=Primitive("box");frame.scene.objects={canopy};Check(render(.004f)==dry,"Camera-hidden set dressing shelters precipitation");
        frame.scene.objects.clear();frame.occlusion.reset();Check(render(.004f)==exposed,"Missing map data keeps precipitation visible with an advisory");
        frame=saved;
    }
    std::ofstream ppm("environment-render-test.ppm", std::ios::binary);
    ppm << "P6\n128 128\n255\n";
    for (size_t i = 0; i < snow.size(); i += 4)
        ppm.write((char *)snow.data() + i, 3);
    // Exercise the multisampled depth shader using the same single-sample movie output.
    d.SampleDesc.Count = 4;
    depth.Reset();
    dsv.Reset();
    Check(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &depth)) &&
              SUCCEEDED(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv)),
          "Create MSAA scene depth");
    frame.scene.weather.type = 0;
    frame.scene.objects.push_back(box);
    auto msaa = render(.004f);
    Check(msaa[center] > 60 && msaa[center + 1] < 5, "MSAA world depth supported");
    D3D11_TEXTURE2D_DESC cd{};
    color->GetDesc(&cd);
    cd.SampleDesc.Count = 4;
    color.Reset();
    target.Reset();
    Check(SUCCEEDED(device->CreateTexture2D(&cd, nullptr, &color)) &&
              SUCCEEDED(device->CreateRenderTargetView(color.Get(), nullptr, &target)),
          "Create MSAA movie target");
    auto msaaColor = render(.004f);
    Check(msaaColor[center] > 60 && msaaColor[center + 1] < 5, "MSAA movie target renders props");
    frame.hasWorldToClip = true;
    frame.worldToClip = ViewProjection(frame.camera, frame.angles, frame.projection);
    frame.angles = {65, 80, 20};
    auto finalCamera = render(.004f);
    Check(finalCamera == msaaColor, "Completed engine matrix takes precedence over earlier camera pose");
    frame.hasWorldToClip = false;
    frame.angles = {0, 0, 0};
    Check(render(.08f) == baseline, "MSAA movie target preserves occlusion");
    // A foliage silhouette has foreground and sky in different samples of the SAME pixel.
    // Uniform MSAA depth misses resolve-before-fog bugs, so synthesize mixed coverage.
    frame.scene.objects.clear();
    frame.scene.climate[CL_enabled] = 1; frame.scene.climate[CL_fogDensity] = 2;
    frame.scene.fogColor = {1, 0, 0};
    auto nearFog = render(.004f), skyFog = render(0);
    const char *coverageShader = R"(
        float4 VS(uint id:SV_VertexID):SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); }
        void PS(uint sample:SV_SampleIndex,out float4 color:SV_Target,out float depth:SV_Depth) {
            color=float4(.2,.2,.2,1); depth=sample==0?.004:0;
        }
    )";
    HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Check(compiler != nullptr, "Load shader compiler for mixed coverage fixture");
    auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile"));
    Check(compile != nullptr, "Find shader compiler");
    ComPtr<ID3DBlob> vsCode, psCode;
    Check(SUCCEEDED(compile(coverageShader, strlen(coverageShader), nullptr, nullptr, nullptr, "VS", "vs_5_0",
                            0, 0, &vsCode, nullptr)) &&
              SUCCEEDED(compile(coverageShader, strlen(coverageShader), nullptr, nullptr, nullptr, "PS",
                                "ps_5_0", 0, 0, &psCode, nullptr)),
          "Compile mixed coverage fixture");
    FreeLibrary(compiler);
    ComPtr<ID3D11VertexShader> coverageVS;
    ComPtr<ID3D11PixelShader> coveragePS;
    Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr,
                                               &coverageVS)) &&
              SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                                                  nullptr, &coveragePS)),
          "Create coverage shaders");
    ComPtr<ID3D11RasterizerState> coverageRaster;
    D3D11_RASTERIZER_DESC coverageRasterDesc{};
    coverageRasterDesc.FillMode=D3D11_FILL_SOLID;coverageRasterDesc.CullMode=D3D11_CULL_NONE;
    coverageRasterDesc.DepthClipEnable=TRUE;coverageRasterDesc.MultisampleEnable=TRUE;
    Check(SUCCEEDED(device->CreateRasterizerState(&coverageRasterDesc,&coverageRaster)),"Create multisample fixture rasterizer");
    context->ClearState();
    context->RSSetState(coverageRaster.Get());
    auto coverageTarget = target.Get();
    context->OMSetRenderTargets(1, &coverageTarget, dsv.Get());
    D3D11_VIEWPORT coverageViewport{0, 0, 128, 128, 0, 1};
    context->RSSetViewports(1, &coverageViewport);
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(coverageVS.Get(), nullptr, 0);
    context->PSSetShader(coveragePS.Get(), nullptr, 0);
    context->Draw(3, 0);
    Render(context.Get(), target.Get(), dsv.Get(), frame);
    auto mixedFog = read();
    for (int channel = 0; channel < 3; ++channel) {
        int expected = (nearFog[center + channel] + 3 * skyFog[center + channel]) / 4;
        std::cerr << "fog channel=" << channel << " near=" << int(nearFog[center+channel]) << " sky=" << int(skyFog[center+channel]) << " mixed=" << int(mixedFog[center+channel]) << " expected=" << expected << std::endl;
        Check(std::abs(int(mixedFog[center + channel]) - expected) <= 2,
              "Fog grades each MSAA coverage sample before resolving");
    }
    {
        Frame saved=frame; frame=Frame{}; frame.active=true;frame.scene.enabled=true;frame.projection=saved.projection;
        frame.scene.climate[CL_enabled]=1; frame.scene.climate[CL_sky]=0;
        // Clouds now default on, and the layer drifts with windSpeed, so a fresh Frame would not
        // reproduce identical pixels at a later time. This case is specifically the lightning
        // channels WITHOUT the cloud pass, so switch them off deliberately.
        frame.scene.climate[CL_clouds]=0;
        frame.scene.climate[CL_storm]=1;frame.scene.climate[CL_stormHold]=1;
        auto strike=EvaluateStorm(frame.scene.climate,0);
        frame.camera={strike.source.x-800,strike.source.y,strike.source.z-300};frame.angles={0,0,0};
        auto bolt=render(0);frame.scene.climate[CL_storm]=0;auto dark=render(0);
        Check(bolt!=dark,"World-space lightning channels render without the cloud pass");
        frame.scene.climate[CL_storm]=1;frame.time=250;Check(render(0)==bolt,"Held lightning reconstructs identical pixels");
        auto blocked=render(.8f);frame.scene.climate[CL_storm]=0;
        Check(render(.8f)==blocked,"Opaque foreground occludes lightning channels");
        frame=saved;
    }
    {
        Frame saved=frame;frame=Frame{};frame.active=true;frame.scene.enabled=true;frame.projection=saved.projection;
        frame.scene.weather.type=1;frame.scene.weather.count=6000;frame.scene.weather.radius=400;frame.scene.weather.shelter=false;
        frame.time=12;
        auto rainy=render(0);frame.scene.weather.type=0;auto dry=render(0);
        Check(rainy!=dry,"World lattice rain renders over open sky");
        frame.scene.weather.type=1;frame.time=90;render(0);frame.time=12;
        Check(render(0)==rainy,"Rain returns to identical drops after seeking and pausing");
        frame.angles={-89.9f,0,0};Check(render(0)!=dry,"Rain has no upward-view cylinder singularity");
        frame.angles={0,0,0};
        std::vector<float> smoke(128*128,40);
        D3D11_TEXTURE2D_DESC sd{};sd.Width=128;sd.Height=128;sd.MipLevels=1;sd.ArraySize=1;sd.SampleDesc.Count=1;
        sd.Format=DXGI_FORMAT_R32_FLOAT;sd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{smoke.data(),128*sizeof(float),0};
        ComPtr<ID3D11Texture2D> tex;ComPtr<ID3D11ShaderResourceView> view;
        Check(SUCCEEDED(device->CreateTexture2D(&sd,&init,&tex))&&SUCCEEDED(device->CreateShaderResourceView(tex.Get(),nullptr,&view)),"Create interior smoke fixture");
        frame.smokes.push_back({{0,0,0},200});SetSmokeAbsorbance(view.Get(),128,128);
        Check(render(0)==dry,"Camera inside dense smoke cannot see rain streaks");
        SetSmokeAbsorbance(nullptr,0,0);frame.smokes.clear();
        Check(render(0)==rainy,"Clearing smoke restores the exact rainy frame without stale regions");
        frame.scene.climate[CL_enabled]=frame.scene.climate[CL_relighting]=1;
        frame.scene.climate[CL_sunElevation]=-5;frame.scene.climate[CL_sunPower]=8;
        auto nightRain=render(0);frame.scene.climate[CL_sunPower]=0;
        Check(nightRain==render(0),"Rain receives no below-horizon solar energy");
        frame.scene.climate=Climate{};
        background=0;auto night=render(0);frame.scene.weather.type=0;
        Check(night==render(0),"Rain does not emit light in an unlit scene");
        background=.2f;frame=saved;
    }
    // The actual regression: mixed foreground/sky coverage with a cloud/fog layer and strong relighting.
    frame.scene.climate[CL_fogDensity] = 0;frame.scene.climate[CL_enabled]=1;frame.scene.climate[CL_relighting]=1;
    frame.scene.climate[CL_sunRelight]=2;frame.scene.climate[CL_relight]=.8f;
    frame.scene.climate[CL_clouds]=1;frame.scene.climate[CL_coverage]=.9f;frame.scene.climate[CL_fogDensity]=.3f;
    frame.scene.climate[CL_lightShafts]=0;
    auto nearClimate=render(.004f),skyClimate=render(0);
    context->OMSetRenderTargets(1,&coverageTarget,dsv.Get());context->RSSetViewports(1,&coverageViewport);
    context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(coverageVS.Get(),nullptr,0);context->PSSetShader(coveragePS.Get(),nullptr,0);context->Draw(3,0);
    Render(context.Get(),target.Get(),dsv.Get(),frame);auto mixedClimate=read();
    for(int channel=0;channel<3;++channel){int expected=(nearClimate[center+channel]+3*skyClimate[center+channel])/4;
        std::cout<<"mixed channel "<<channel<<" near="<<int(nearClimate[center+channel])<<" sky="<<int(skyClimate[center+channel])<<" expected="<<expected<<" got="<<int(mixedClimate[center+channel])<<"\n";
        Check(std::abs(int(mixedClimate[center+channel])-expected)<=3,"Clouds and relighting preserve mixed MSAA sample coverage");}
    frame.scene.climate=Climate{};
    {
        Frame saved=frame;
        frame.nativeLightingApplied=true;
        frame.nativeSkyApplied=true;
        frame.scene.climate[CL_enabled]=1;frame.scene.climate[CL_clouds]=0;
        frame.scene.climate[CL_sky]=1;frame.scene.climate[CL_sunRelight]=3;
        frame.scene.climate[CL_relight]=1;frame.scene.climate[CL_ambient]=2;
        // Post-processing broadcasts the already resolved scene color into its
        // MSAA output. Different depth samples must not relight that color again.
        context->ClearState();context->RSSetState(coverageRaster.Get());
        context->OMSetRenderTargets(1,&coverageTarget,dsv.Get());context->RSSetViewports(1,&coverageViewport);
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(coverageVS.Get(),nullptr,0);context->PSSetShader(coveragePS.Get(),nullptr,0);
        context->Draw(3,0);auto resolvedNative=read();
        Render(context.Get(),target.Get(),dsv.Get(),frame);
        Check(read()==resolvedNative,"Completed native lighting and sky preserve resolved edge color with mixed MSAA depth");
        frame=saved;
    }
    // Snow must inherit broad map shading and not relight every raw geometric facet.
    {
        frame.scene.climate=Climate{};frame.scene.climate[CL_snow]=1;frame.camera={0,0,1000};frame.angles={90,0,0};
        const char* coatingFixture=R"(
            void PS(float4 screen:SV_Position,uint sample:SV_SampleIndex,out float4 color:SV_Target,out float depth:SV_Depth){
                float x=screen.x/128*2-1;depth=4/(1000+abs(x)*300);
                float shade=screen.x<32?.02:screen.x>96?.45:.2;color=float4(shade,shade,shade,1);
            })";
        auto lib=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto comp=reinterpret_cast<pD3DCompile>(GetProcAddress(lib,"D3DCompile"));ComPtr<ID3DBlob> code,error;ComPtr<ID3D11PixelShader> ps;
        Check(SUCCEEDED(comp(coatingFixture,strlen(coatingFixture),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&code,&error)),"Compile faceted coating fixture");
        Check(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps)),"Create coating fixture");FreeLibrary(lib);
        auto coatingRender=[&](){context->ClearState();context->OMSetRenderTargets(1,&coverageTarget,dsv.Get());context->RSSetViewports(1,&coverageViewport);context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(coverageVS.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->Draw(3,0);Render(context.Get(),target.Get(),dsv.Get(),frame);return read();};
        auto snowCoating=coatingRender();int seamLeft=(64*128+62)*4,seamRight=(64*128+65)*4;
        Check(std::abs(int(snowCoating[seamLeft])-int(snowCoating[seamRight]))<=2,"Snow does not reveal a false lighting seam across adjacent facets");
        Check(snowCoating[(64*128+112)*4]>snowCoating[(64*128+16)*4]+30,"Snow preserves broad map shading instead of painting over shadows");
        auto nearSnow=render(.004f),skySnow=render(0);
        context->ClearState();context->OMSetRenderTargets(1,&coverageTarget,dsv.Get());context->RSSetViewports(1,&coverageViewport);context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(coverageVS.Get(),nullptr,0);context->PSSetShader(coveragePS.Get(),nullptr,0);context->Draw(3,0);Render(context.Get(),target.Get(),dsv.Get(),frame);auto mixedSnow=read();
        for(int channel=0;channel<3;++channel){int expected=(nearSnow[center+channel]+3*skySnow[center+channel])/4;std::cout<<"snow mixed channel="<<channel<<" near="<<int(nearSnow[center+channel])<<" sky="<<int(skySnow[center+channel])<<" expected="<<expected<<" got="<<int(mixedSnow[center+channel])<<"\n";Check(std::abs(int(mixedSnow[center+channel])-expected)<=2,"Coating smoothing preserves mixed foreground and sky MSAA coverage");}
        frame.scene.climate=Climate{};frame.scene.climate[CL_wet]=1;frame.scene.climate[CL_puddles]=1;frame.scene.climate[CL_puddleHeight]=0;frame.scene.climate[CL_puddleDepth]=48;frame.scene.climate[CL_ripple]=0;
        auto pooled=coatingRender();Check(std::abs(int(pooled[seamLeft])-int(pooled[seamRight]))<=4,"Puddle reflection stays continuous across ground facets");
        frame.camera={0,0,0};frame.angles={0,0,0};frame.scene.climate=Climate{};
    }
    // Analytic wall/floor fixture: a red wall at x=300 and floor at z=-80, viewed along +X.
    const char* transportFixture=R"(
        void PS(float4 screen:SV_Position,uint sample:SV_SampleIndex,out float4 color:SV_Target,out float depth:SV_Depth){
            float2 ndc=screen.xy/128*float2(2,-2)+float2(-1,1);
            float floorT=ndc.y<0?-80/ndc.y:100000;
            float distance=min(300,floorT);depth=4/distance+sample*1e-10;
            color=floorT<300?float4(.2,.2,.2,1):float4(.8,.05,.02,1);
        }
    )";
    compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    compile=reinterpret_cast<pD3DCompile>(GetProcAddress(compiler,"D3DCompile"));
    ComPtr<ID3DBlob> fixtureCode;ComPtr<ID3D11PixelShader> fixturePS;
    Check(SUCCEEDED(compile(transportFixture,strlen(transportFixture),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&fixtureCode,nullptr)),"Compile transport scene");
    Check(SUCCEEDED(device->CreatePixelShader(fixtureCode->GetBufferPointer(),fixtureCode->GetBufferSize(),nullptr,&fixturePS)),"Create transport fixture");FreeLibrary(compiler);
    auto transportRender=[&](){
        context->ClearState();context->OMSetRenderTargets(1,&coverageTarget,dsv.Get());context->RSSetViewports(1,&coverageViewport);
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(coverageVS.Get(),nullptr,0);context->PSSetShader(fixturePS.Get(),nullptr,0);context->Draw(3,0);
        Render(context.Get(),target.Get(),dsv.Get(),frame);return read();
    };
    frame.scene.objects.clear();frame.scene.climate=Climate{};
    context->ClearState();
    frame.scene.climate[CL_fogDensity] = 0;
    frame.scene.objects.push_back(box);
    frame.projection[10] = -1;
    frame.projection[11] = -4;
    auto conventional = render(.996f);
    Check(conventional[center] > 60 && conventional[center + 1] < 5, "Conventional depth supported");
    worldDepthMax = .7f;
    Check(render(.7f * .996f) == conventional, "CS2 conventional depth partition supports MSAA props");
    worldDepthMax = 1;
    frame.active = false;
    auto disabled = render(.004f);
    Check(disabled == baseline, "Disabled scene restores baseline without engine writes");
    // Float targets must retain radiance above display white, using the separate HDR resource cache.
    D3D11_TEXTURE2D_DESC hd=cd;hd.SampleDesc={1,0};hd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    ComPtr<ID3D11Texture2D> hdrColor,hdrRead;
    ComPtr<ID3D11RenderTargetView> hdrTarget;
    Check(SUCCEEDED(device->CreateTexture2D(&hd,nullptr,&hdrColor)) && SUCCEEDED(device->CreateRenderTargetView(hdrColor.Get(),nullptr,&hdrTarget)),"Create HDR fixture");
    hd.BindFlags=0;hd.Usage=D3D11_USAGE_STAGING;hd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(SUCCEEDED(device->CreateTexture2D(&hd,nullptr,&hdrRead)),"Create HDR readback");
    float radiance[]={2.5f,.5f,-.1f,1};context->ClearRenderTargetView(hdrTarget.Get(),radiance);
    context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,.996f,0);
    frame.scene=Scene{};frame.active=true;frame.hdr=true;frame.exporting=true;
    Render(context.Get(),hdrTarget.Get(),dsv.Get(),frame);
    context->CopyResource(hdrRead.Get(),hdrColor.Get());D3D11_MAPPED_SUBRESOURCE hdrMap{};
    Check(SUCCEEDED(context->Map(hdrRead.Get(),0,D3D11_MAP_READ,0,&hdrMap)),"Map HDR fixture");
    Check(std::abs(static_cast<float*>(hdrMap.pData)[0]-2.5f)<.0001f,"HDR environment preserves values above 1.0");
    Check(std::abs(static_cast<float*>(hdrMap.pData)[2]+.1f)<.0001f,"Neutral HDR preserves signed radiance");
    context->Unmap(hdrRead.Get(),0);
    // Exercise the actual compressed native-post boundary at and far above display white.
    // Surface additions and grading share this boundary; neither may write raw HDR into it.
    frame.nativeCompressedHdr=true;
    const float encodedInput[]{.5f,.25f,.125f,1};
    float previousPeak=0;
    for(float gain:{1.f,10.f,100.f,1000000.f}) {
        frame.scene.exposure=gain;
        context->ClearRenderTargetView(hdrTarget.Get(),encodedInput);
        Render(context.Get(),hdrTarget.Get(),dsv.Get(),frame);
        context->CopyResource(hdrRead.Get(),hdrColor.Get());
        Check(SUCCEEDED(context->Map(hdrRead.Get(),0,D3D11_MAP_READ,0,&hdrMap)),"Read compressed lighting output");
        const float* pixel=static_cast<const float*>(hdrMap.pData);
        Check(std::isfinite(pixel[0])&&pixel[0]>=previousPeak&&pixel[0]<1,"Bright lighting stays finite, monotonic and below native decode singularity");
        Check(std::abs(pixel[1]/pixel[0]-.5f)<.0001f&&std::abs(pixel[2]/pixel[0]-.25f)<.0001f,"HDR encoding preserves highlight hue");
        if(gain<=100)Check(std::abs(pixel[0]/(1-pixel[0])-gain)<.02f,"Native decoder recovers the intended linear lighting");
        previousPeak=pixel[0];context->Unmap(hdrRead.Get(),0);
    }
    frame.nativeCompressedHdr=false;frame.scene.exposure=1;
    frame.hdr=false;frame.exporting=false;frame.active=false;
    Check(render(.996f)==baseline,"HDR export leaves display resources and state intact");
    // Sample real light-space shadow maps at a fixed receiver. Camera visibility plays no part.
    {
        auto world=std::make_shared<OcclusionMesh>();world->low={0,-200,-80};world->high={500,200,150};
        world->positions={{0,-200,-80},{500,-200,-80},{500,200,-80},{0,200,-80},
            {300,-200,-80},{300,200,-80},{300,200,150},{300,-200,150}};
        world->indices={0,1,2,0,2,3,4,5,6,4,6,7};
        Frame shadowFrame;shadowFrame.active=true;shadowFrame.scene.enabled=true;shadowFrame.occlusion=world;
        shadowFrame.scene.climate[CL_enabled]=1;shadowFrame.scene.climate[CL_sunAzimuth]=0;shadowFrame.scene.climate[CL_sunElevation]=0;
        Object light;light.id=1;light.kind="point";light.position={400,0,0};light.range=800;light.shadows=true;shadowFrame.scene.objects={light};
        // Exercise the actual private atlas producer, independently of the legacy
        // receiver shader. Compare its stored depth with a receiver behind a wall.
        {
            shadowFrame.proof.enabled=true;shadowFrame.proof.position=light.position;shadowFrame.proof.range=light.range;
            auto privateVisibility=[&](){
                Matrix matrices[7];ID3D11ShaderResourceView* atlas=nullptr;
                ComPtr<ID3D11PixelShader> before,after;context->PSGetShader(&before,nullptr,nullptr);
                Check(PrepareLightingProofShadows(context.Get(),shadowFrame,matrices,&atlas)&&atlas,"private fixture and sun atlas ready");
                context->PSGetShader(&after,nullptr,nullptr);Check(before==after,"private shadow command list restores caller shader");
                const Vec3 receiver{100,0,-70};const int face=PointShadowFace(receiver-shadowFrame.proof.position);
                const auto p=Transform(matrices[face],receiver);
                const UINT x=UINT((face%3)*1024+(p.x*.5f+.5f)*1024),y=UINT((face/3)*1024+(-p.y*.5f+.5f)*1024);
                D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
                d.Format=DXGI_FORMAT_R32_TYPELESS;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> staging;Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"private atlas readback texture");
                ComPtr<ID3D11Resource> resource;atlas->GetResource(&resource);const D3D11_BOX region{x,y,0,x+1,y+1,1};
                context->CopySubresourceRegion(staging.Get(),0,0,0,0,resource.Get(),0,&region);
                D3D11_MAPPED_SUBRESOURCE mapped{};Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"private atlas readback");
                const float stored=*static_cast<float*>(mapped.pData);context->Unmap(staging.Get(),0);
                return p.z-.0002f<=stored;
            };
            Check(!privateVisibility(),"private atlas wall occludes authored fixture");
            shadowFrame.proof.position.x=200;++shadowFrame.proof.generation;
            Check(privateVisibility(),"moving private fixture across wall restores illumination");
            shadowFrame.proof.enabled=false;
        }
        const char *probeSource="cbuffer LightPositions:register(b0){float4 lightPositionRange[16];float4 emitterSamples[4];}\nfloat3 StudioPosition(int l,int sample){return emitterSamples[0].w>0?emitterSamples[sample].xyz:lightPositionRange[l].xyz;}\nint StudioSamples(int l){return emitterSamples[0].w>0?4:1;}\n"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowShaders.h"
            "float4 Main(float4 position:SV_Position):SV_Target{bool vm=lightPositionRange[15].w>0;return float4(WorldLightShadow(0,float3(100,0,-70),vm),vm?WorldSunShadowSoft(float3(100,0,-70),float3(1,0,0),true):WorldSunShadow(float3(100,0,-70)),WorldLightShadow(0,float3(100,(position.x-64)*16,-70),vm),WorldSunShadowSoft(float3(100,(position.x-64)*16,-70),float3(1,0,0),vm));}";
        auto library=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto compiler=reinterpret_cast<pD3DCompile>(GetProcAddress(library,"D3DCompile"));
        ComPtr<ID3DBlob> code,errors;ComPtr<ID3D11PixelShader> probe;
        Check(SUCCEEDED(compiler(probeSource,strlen(probeSource),nullptr,nullptr,nullptr,"Main","ps_5_0",0,0,&code,&errors)),"Compile world shadow probe");
        Check(SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&probe)),"Create world shadow probe");FreeLibrary(library);
        D3D11_BUFFER_DESC cbd{};cbd.ByteWidth=20*16;cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;cbd.Usage=D3D11_USAGE_DEFAULT;
        ComPtr<ID3D11Buffer> lightBuffer;Check(SUCCEEDED(device->CreateBuffer(&cbd,nullptr,&lightBuffer)),"Create probe light constants");
        auto sampleShadows=[&](bool viewmodel=false){
            context->ClearState();PrepareShadowMaps(context.Get(),shadowFrame,viewmodel);context->ClearState();BindShadowMaps(context.Get());
            float positions[80]{};auto p=shadowFrame.scene.objects[0].position;positions[0]=p.x;positions[1]=p.y;positions[2]=p.z;
            positions[63]=viewmodel?1.f:0.f;
            if(shadowFrame.scene.objects[0].kind=="area") {const auto &o=shadowFrame.scene.objects[0];Vec3 dir,r,u;CameraBasis(o.angles,dir,r,u);for(int j=0;j<4;++j){auto sample=AreaLightSample(p,r,u,o.studio.width,o.studio.height,j);positions[64+j*4]=sample.x;positions[65+j*4]=sample.y;positions[66+j*4]=sample.z;positions[67+j*4]=1;}}
            context->UpdateSubresource(lightBuffer.Get(),0,nullptr,positions,0,0);auto cb=lightBuffer.Get();context->PSSetConstantBuffers(0,1,&cb);
            context->OMSetRenderTargets(1,&coverageTarget,nullptr);context->RSSetViewports(1,&coverageViewport);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(coverageVS.Get(),nullptr,0);context->PSSetShader(probe.Get(),nullptr,0);context->Draw(3,0);
            return read();
        };
        shadowFrame.scene.climate[CL_fogDensity]=.35f;shadowFrame.scene.climate[CL_lightShafts]=1;
        auto fogOnly=sampleShadows();
        Check(fogOnly[center]<10&&fogOnly[center+1]>245,"Fog alone keeps fixture shadows and renders no sun shadow map");
        shadowFrame.scene.climate[CL_fogDensity]=0;shadowFrame.scene.climate[CL_clouds]=1;
        auto blocked=sampleShadows();Check(blocked[center]<10&&blocked[center+1]<10,"Map geometry blocks point light and sun");
        shadowFrame.scene.climate[CL_sunElevation]=-5;
        auto nightShadows=sampleShadows();
        Check(nightShadows[center]<10&&nightShadows[center+1]>245,"At night local fixtures keep shadows while sun shadows stop");
        shadowFrame.scene.climate[CL_sunElevation]=0;
        shadowFrame.scene.climate[CL_relighting]=1;
        auto detailed=sampleShadows();
        Check(detailed[center+3]<10&&detailed[3]>245,"Near sun detail includes world blockers and clear receivers");
        shadowFrame.angles={0,180,0};
        Check(sampleShadows()==detailed,"Turning away does not remove near-map shadow casters");
        shadowFrame.camera={9000,8000,-3000};
        Check(sampleShadows()[center+3]<10,"Receivers outside near region fall back to full-map shadows");
        shadowFrame.scene.climate[CL_relighting]=0;
        shadowFrame.camera={2000,-3000,1000};shadowFrame.angles={0,180,0};shadowFrame.projection=Identity();
        Check(sampleShadows()==blocked,"Looking away does not change world-space light or sun shadows");
        shadowFrame.scene.objects[0].position={150,0,0};auto moved=sampleShadows();Check(moved[center]>245&&moved[center+1]<10,"Moving a point light updates its shadows independently of sun");
        shadowFrame.scene.objects[0].kind="spot";shadowFrame.scene.objects[0].angles={0,180,0};shadowFrame.scene.objects[0].cone=90;
        shadowFrame.scene.objects[0].position={400,0,0};Check(sampleShadows()[center]<10,"Spotlight casts world geometry shadows");
        auto sharpStudio=sampleShadows();shadowFrame.scene.objects[0].studio.sourceRadius=32;auto softStudio=sampleShadows();
        auto partial=[](const std::vector<unsigned char>& pixels){int count=0;for(size_t i=2;i<pixels.size();i+=4)if(pixels[i]>3&&pixels[i]<252)++count;return count;};
        Check(partial(softStudio)>partial(sharpStudio),"Source radius creates a wider partial-visibility penumbra in real shadow maps");
        shadowFrame.scene.objects[0].studio.sourceRadius=0;
        shadowFrame.scene.objects[0].kind="area";shadowFrame.scene.objects[0].position.x=350;shadowFrame.scene.objects[0].studio.width=16;shadowFrame.scene.objects[0].studio.height=16;
        Check(sampleShadows()[center]<10,"Area emitter samples respect an offscreen wall");
        shadowFrame.scene.objects[0].studio.width=1024;auto wideArea=sampleShadows();
        Check(wideArea[center]>30,"Widening an area emitter lets light reach around a blocker");
        shadowFrame.camera={9000,8000,-3000};Check(sampleShadows()==wideArea,"Area shadow quadrature is camera independent");
        shadowFrame.scene.objects[0].kind="spot";
        shadowFrame.scene.objects[0].shadows=false;Check(sampleShadows()[center]>245,"Per-light shadow disable restores unoccluded light");
        shadowFrame.scene.objects[0].shadows=true;shadowFrame.scene.objects[0].kind="point";shadowFrame.scene.objects[0].position={150,0,0};
        Object caster;caster.id=2;caster.kind="box";caster.position={125,0,-35};caster.scale={5,20,40};caster.mesh=Primitive("box");shadowFrame.scene.objects.push_back(caster);
        Check(sampleShadows()[center]<10,"Editor prop casts a dynamic shadow");
        shadowFrame.scene.objects[1].cameraVisible=false;Check(sampleShadows()[center]<10,"Camera-hidden prop remains a light blocker");
        shadowFrame.scene.objects[1].position.y=150;Check(sampleShadows()[center]>245,"Moving the editor prop updates the shadow");

        {
            Frame saved=shadowFrame;
            auto leafWorld=std::make_shared<OcclusionMesh>(*world);leafWorld->indices={0,1,2,0,2,3};
            auto leaves=std::make_shared<VisualCasters>();CutoutCaster leaf;
            leaf.vertices={{{300,-128,-150},0,0},{{300,128,-150},1,0},{{300,128,150},1,1},{{300,-128,150},0,1}};
            leaf.indices={0,1,2,0,2,3};leaf.width=4;leaf.height=1;leaf.alpha={255,255,0,0};leaf.cutoff=.466f;
            leaves->batches.push_back(leaf);leaves->low={300,-128,-150};leaves->high={300,128,150};leafWorld->visual=leaves;
            shadowFrame.occlusion=leafWorld;shadowFrame.scene.objects.resize(1);shadowFrame.scene.objects[0].position={400,0,0};
            shadowFrame.scene.climate[CL_relighting]=1;shadowFrame.camera={100,0,0};
            auto foliage=sampleShadows();int opaque=(64*128+60)*4,transparent=(64*128+68)*4;
            Check(foliage[opaque+3]<10&&foliage[transparent+3]>245,"Near sun preserves the leaf cutout, not an opaque card");
            Check(foliage[opaque+2]<10&&foliage[transparent+2]>245,"Point-light shadows preserve foliage transparency");
            // Exercise static-base + animated-overlay composition before native inputs
            // arrive. The fallback rest pose must retain cutouts and rigid world depth.
            auto animatedWorld=std::make_shared<OcclusionMesh>(*leafWorld);
            auto animatedLeaves=std::make_shared<VisualCasters>(*leaves);
            animatedLeaves->batches[0].animation.sway[0]=.2f;
            animatedWorld->visual=animatedLeaves;shadowFrame.occlusion=animatedWorld;
            auto composed=sampleShadows();
            Check(composed[opaque+3]<10&&composed[transparent+3]>245,"Cached rigid depth composes with animated cutouts at rest");
            auto receiverLeaves=sampleShadows(true);
            Check(receiverLeaves[opaque+3]<10&&receiverLeaves[transparent+3]>245&&
                  receiverLeaves[opaque+2]<10&&receiverLeaves[transparent+2]>245,
                  "First-person sun and local layers retain animated alpha-aware foliage");
            Check(sampleShadows()==composed,"Unchanged native generation reuses the composed shadow");
            shadowFrame.camera={9000,8000,-3000};auto farFoliage=sampleShadows();
            Check(farFoliage[opaque+3]<10&&farFoliage[transparent+3]>245,"Offscreen foliage also reaches the far sun map");
            shadowFrame.scene.climate[CL_sunAzimuth]=180;
            Check(sampleShadows()[opaque+3]>245,"Moving the sun changes foliage cast shadows");
            shadowFrame=saved;
        }

        // Native animated caster regression: compose a captured triangle over a
        // rigid floor/wall, then prove movement and disappearance remove only
        // the animated depth. This drives the production stream-output capture
        // and the same sampleShadows oracle used by the static caster checks.
        {
            Frame saved=shadowFrame;
            auto nativeWorld=std::make_shared<OcclusionMesh>();
            nativeWorld->low={0,-256,-80};nativeWorld->high={500,256,120};
            nativeWorld->positions={
                {0,-256,-80},{500,-256,-80},{500,256,-80},{0,256,-80},
                {300,64,-80},{300,256,-80},{300,256,120},{300,64,120}};
            nativeWorld->indices={0,1,2,0,2,3,4,5,6,4,6,7};
            shadowFrame=Frame{};shadowFrame.active=true;shadowFrame.scene.enabled=true;shadowFrame.occlusion=nativeWorld;
            shadowFrame.scene.climate[CL_enabled]=1;
            shadowFrame.scene.climate[CL_sunAzimuth]=0;shadowFrame.scene.climate[CL_sunElevation]=0;
            shadowFrame.scene.climate[CL_relighting]=1;shadowFrame.camera={200,0,-42};shadowFrame.angles={0,180,0};
            Object nativeLight;nativeLight.id=1;nativeLight.kind="point";nativeLight.position={400,0,0};nativeLight.range=800;nativeLight.shadows=true;
            shadowFrame.scene.objects={nativeLight};

            const char *nativeSource=R"(
                cbuffer PerViewConstantBuffer_t:register(b3){
                    column_major float4x4 g_matProjectionToWorld:packoffset(c4);
                    float4 g_vWorldToCameraOffset:packoffset(c24);
                    float4 pose:packoffset(c25);
                    float g_flWeaponOcclusionAmount:packoffset(c26);
                }
                struct NativeOutput {
                    float3 a:TEXCOORD13;float3 b:TEXCOORD0;float3 c:TEXCOORD1;
                    float4 d:TEXCOORD2;float4 e:TEXCOORD4;float3 f:TEXCOORD5;
                    float4 g:TEXCOORD6;uint h:TEXCOORD12;float4 position:SV_Position;
                };
                NativeOutput Main(uint id:SV_VertexID) {
                    float3 local[3]={float3(0,-24,-80),float3(0,24,-80),float3(0,0,40)};
                    NativeOutput o=(NativeOutput)0;
                    o.position=float4(local[id]+pose.xyz+float3(0,0,g_flWeaponOcclusionAmount),1);
                    return o;
                })";
            HMODULE nativeCompiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            Check(nativeCompiler!=nullptr,"Load shader compiler for animated caster fixture");
            auto nativeCompile=reinterpret_cast<pD3DCompile>(GetProcAddress(nativeCompiler,"D3DCompile"));
            Check(nativeCompile!=nullptr,"Find shader compiler for animated caster fixture");
            ComPtr<ID3DBlob> nativeCode,nativeErrors;
            Check(SUCCEEDED(nativeCompile(nativeSource,strlen(nativeSource),nullptr,nullptr,nullptr,"Main","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&nativeCode,&nativeErrors)),"Compile animated caster fixture");
            FreeLibrary(nativeCompiler);
            ComPtr<ID3D11VertexShader> nativeVS;
            Check(SUCCEEDED(device->CreateVertexShader(nativeCode->GetBufferPointer(),nativeCode->GetBufferSize(),nullptr,&nativeVS)),"Create animated caster fixture");
            const char *nativeName="csgo_character.vfx_vs";
            Check(SUCCEEDED(nativeVS->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(nativeName)),nativeName)),"Name animated caster fixture");
            NoteAnimatedCasterShader(nativeVS.Get(),nativeCode->GetBufferPointer(),nativeCode->GetBufferSize());
            float nativeConstants[108]{};
            nativeConstants[16]=nativeConstants[21]=nativeConstants[26]=nativeConstants[31]=1;
            ComPtr<ID3D11Buffer> nativeCB;
            D3D11_BUFFER_DESC nativeBufferDesc{};nativeBufferDesc.ByteWidth=sizeof(nativeConstants);nativeBufferDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            Check(SUCCEEDED(device->CreateBuffer(&nativeBufferDesc,nullptr,&nativeCB)),"Create animated caster constants");
            D3D11_TEXTURE2D_DESC nativeDesc{};nativeDesc.Width=nativeDesc.Height=512;nativeDesc.MipLevels=nativeDesc.ArraySize=nativeDesc.SampleDesc.Count=1;
            nativeDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;nativeDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> nativeColor,nativeDepth;ComPtr<ID3D11RenderTargetView> nativeTarget;ComPtr<ID3D11DepthStencilView> nativeDsv;
            Check(SUCCEEDED(device->CreateTexture2D(&nativeDesc,nullptr,&nativeColor))&&SUCCEEDED(device->CreateRenderTargetView(nativeColor.Get(),nullptr,&nativeTarget)),"Create animated caster color target");
            nativeDesc.Format=DXGI_FORMAT_D32_FLOAT;nativeDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
            Check(SUCCEEDED(device->CreateTexture2D(&nativeDesc,nullptr,&nativeDepth))&&SUCCEEDED(device->CreateDepthStencilView(nativeDepth.Get(),nullptr,&nativeDsv)),"Create animated caster depth target");
            D3D11_VIEWPORT nativeViewport{0,0,512,512,0,.95f};
            auto captureNative=[&](Vec3 position){
                nativeConstants[100]=position.x;nativeConstants[101]=position.y;nativeConstants[102]=position.z;
                context->ClearState();auto nativeRt=nativeTarget.Get();context->OMSetRenderTargets(1,&nativeRt,nativeDsv.Get());
                context->ClearDepthStencilView(nativeDsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->RSSetViewports(1,&nativeViewport);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(nativeVS.Get(),nullptr,0);
                auto cb=nativeCB.Get();context->VSSetConstantBuffers(3,1,&cb);context->UpdateSubresource(nativeCB.Get(),0,nullptr,nativeConstants,0,0);
                Check(BeginAnimatedCasterDraw(context.Get(),3,1),"Capture submitted animated caster triangle");context->Draw(3,0);EndAnimatedCasterDraw(context.Get());
            };
            const int rigid=(64*128+79)*4+2; // y=240 in the b-channel probe.
            const int actorReceiver=(64*128+65)*4+2; // y=16: outside the local proxy, inside the fixture.
            // Establish the static wall's depth before introducing any native
            // pose. The animated composition must preserve this exact sample
            // while its own caster moves or disappears.
            auto staticDepth=sampleShadows()[rigid];
            auto nativeSample=[&](){
                auto pixels=sampleShadows();
                Check(pixels[rigid]==staticDepth,"Animated caster composition preserves rigid wall depth");
                return pixels[center];
            };
            PrepareShadowMaps(context.Get(),shadowFrame);captureNative({250,0,0});
            Check(nativeSample()<10,"Submitted animated triangle casts a point-light shadow");
            auto receiver=sampleShadows(true);
            Check(receiver[center]>245&&receiver[center+3]>245,
                  "First-person receiver excludes synthetic body and third-person character shadows");
            Check(receiver[actorReceiver]>245,
                  "Third-person character limbs are excluded outside the local body proxy");
            Check(receiver[rigid]==staticDepth,"First-person receiver still sees the rigid wall shadow");
            Check(nativeSample()<10,"Creating the first-person layer preserves the actor's world shadow");
            Check(sampleShadows(true)==receiver,"Repeated first-person layer request reuses identical composed depth");
            NextAnimatedCasterFrame();
            const char *weaponName="csgo_weapon.vfx_vs";
            Check(SUCCEEDED(nativeVS->SetPrivateData(WKPDID_D3DDebugObjectName,UINT(strlen(weaponName)),weaponName)),
                  "Classify animated caster fixture as a weapon");
            captureNative({250,0,0});
            Check(nativeSample()<10,"Submitted weapon triangle retains its world shadow");
            auto weaponReceiver=sampleShadows(true);
            Check(weaponReceiver[center]>245&&weaponReceiver[center+3]>245&&weaponReceiver[actorReceiver]>245,
                  "Third-person weapon shadow is excluded from the first-person receiver");
            Check(nativeSample()<10,"First-person weapon isolation does not alter the world weapon shadow");
            NextAnimatedCasterFrame();
            captureNative({250,180,0});
            Check(nativeSample()>245,"Moving animated triangle removes its old shadow");
            NextAnimatedCasterFrame();
            Check(nativeSample()>245,"Empty animated frame clears the previous caster");
            auto emptyReceiver=sampleShadows(true);
            Check(emptyReceiver[center]>245&&emptyReceiver[actorReceiver]>245,
                  "Hidden or disappearing native actor leaves no synthetic first-person shadow");
            NextAnimatedCasterFrame();
            Check(nativeSample()>245,"Second static-only frame keeps the caster absent");
            shadowFrame.scene.objects[0].shadows=false;
            Check(sampleShadows()[center]>245,"Disabling shadow lighting removes animated caster output");
            Check(sampleShadows(true)[center]>245,"Disabling a local light does not reuse its first-person depth");
            NextAnimatedCasterFrame();
            shadowFrame.scene.objects[0].shadows=true;
            PrepareShadowMaps(context.Get(),shadowFrame);NextAnimatedCasterFrame();captureNative({250,0,0});
            Check(nativeSample()<10,"Re-enabling shadow lighting accepts a fresh animated caster");
            NextAnimatedCasterFrame();
            shadowFrame=saved;
        }

        shadowFrame.scene.objects.resize(1);shadowFrame.scene.objects[0].position={180,0,20};shadowFrame.scene.objects[0].color={1,.01f,.01f};shadowFrame.scene.objects[0].intensity=8;
        shadowFrame.scene.climate[CL_enabled]=0;shadowFrame.scene.climate[CL_worldGi]=1;shadowFrame.scene.climate[CL_bounceSize]=512;shadowFrame.scene.climate[CL_bounceX]=200;
        const char* giProbeSource=
            "#define CL_worldGi 1\n#define CL_bounce 1\n#define CL_sceneReflections 1\n#define CL_reflectionRoughness .3\n"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentGiTraceShaders.h"
            "float4 Main(float4 p:SV_Position):SV_Target {return float4(WorldBounce(float3(180,0,-78),float3(0,0,1)).r,WorldReflection(float3(100,0,0),float3(0,0,1),float3(1,0,0),float3(0,0,1)).r,0,1);}";
        library=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);compiler=reinterpret_cast<pD3DCompile>(GetProcAddress(library,"D3DCompile"));
        ComPtr<ID3DBlob> giCode,giErrors;ComPtr<ID3D11PixelShader> giProbe;
        Check(SUCCEEDED(compiler(giProbeSource,strlen(giProbeSource),nullptr,nullptr,nullptr,"Main","ps_5_0",0,0,&giCode,&giErrors)),"Compile GI receiver probe");
        Check(SUCCEEDED(device->CreatePixelShader(giCode->GetBufferPointer(),giCode->GetBufferSize(),nullptr,&giProbe)),"Create GI receiver probe");FreeLibrary(library);
        auto sampleGi=[&](){
            context->ClearState();PrepareShadowMaps(context.Get(),shadowFrame);context->ClearState();PrepareGi(context.Get(),shadowFrame);context->ClearState();BindGi(context.Get());
            context->OMSetRenderTargets(1,&coverageTarget,nullptr);context->RSSetViewports(1,&coverageViewport);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(coverageVS.Get(),nullptr,0);context->PSSetShader(giProbe.Get(),nullptr,0);context->Draw(3,0);return read();
        };
        auto bounced=sampleGi();std::cout<<"GI bounce="<<int(bounced[center])<<" reflection="<<int(bounced[center+1])<<"\n";
        Check(bounced[center]>5&&bounced[center+1]>20,"Offscreen lit world geometry supplies bounced light and reflection");
        shadowFrame.camera={-4000,9000,2000};shadowFrame.angles={90,275,0};Check(sampleGi()==bounced,"GI and reflected scene radiance are camera independent");
        shadowFrame.scene.objects[0].studio.cue=3;shadowFrame.time=.5;
        auto cueDark=sampleGi();Check(cueDark[center]<3,"Animated light off phase clears bounced radiance");
        shadowFrame.time=0;Check(sampleGi()==bounced,"Animated light bright phase restores bounced radiance");
        shadowFrame.scene.objects[0].studio.cue=0;
        shadowFrame.scene.objects[0].shadows=false;auto voxelLit=sampleGi();
        Check(voxelLit[center]>3,"Lights without shadow-map allocation contribute dynamically through voxel visibility");
        shadowFrame.scene.objects[0].color={0,0,1};
        Check(sampleGi()[center]<3,"Changing emitter color removes the previous red bounce immediately");
        shadowFrame.scene.objects[0].color={1,.01f,.01f};
        Check(sampleGi()==voxelLit,"Restoring emitter color restores identical indirect radiance");
        shadowFrame.scene.objects[0].position.x=400;
        Check(sampleGi()[center+1]<voxelLit[center+1]/3,"Voxel visibility prevents a light behind the wall from radiating toward the receiver");
        shadowFrame.scene.objects[0].position.x=180;
        shadowFrame.scene.objects[0].shadows=true;
        shadowFrame.scene.objects[0].intensity=0;auto darkGi=sampleGi();Check(darkGi[center]<3&&darkGi[center+1]<3,"Turning off the light removes its indirect light and reflection");
        shadowFrame.scene.objects[0].intensity=8;Check(sampleGi()==bounced,"Restoring the light reconstructs identical world radiance");
        shadowFrame.scene.objects[0].position.x=400;auto behindWallGi=sampleGi();
        Check(behindWallGi[center+1]<bounced[center+1]/3,"Back-facing lit wall does not radiate through to reflection receiver");
        shadowFrame.scene.objects[0].position.x=180;Check(sampleGi()==bounced,"Moving the light back restores cached radiance");
        {
            shadowFrame.scene.objects[0].intensity=0;Object emitter;emitter.id=77;emitter.position={250,0,0};emitter.scale={8,80,100};emitter.color={1,0,0};emitter.mesh=Primitive("box");emitter.material.enabled=true;emitter.material.emission=4;emitter.cameraVisible=false;shadowFrame.scene.objects.push_back(emitter);
            auto emitted=sampleGi();std::cout<<"Emission probe bounce="<<int(emitted[center])<<" reflection="<<int(emitted[center+1])<<"\n";Check(emitted[center]>5&&emitted[center+1]>20,"Emissive prop supplies indirect light and reflection without any direct light");
            shadowFrame.scene.objects[1].material.emission=0;auto stopped=sampleGi();Check(stopped[center]<3&&stopped[center+1]<3,"Disabling emission clears radiance without stale voxel data");
            shadowFrame.scene.objects.resize(1);shadowFrame.scene.objects[0].intensity=8;Check(sampleGi()==bounced,"Removing emissive geometry restores prior light transport");
        }
        frame=Frame{};frame.active=true;frame.scene=shadowFrame.scene;frame.occlusion=world;
        frame.projection={1,0,0,0,0,1,0,0,0,0,0,4,0,0,-1,0};frame.scene.climate[CL_worldGi]=0;
        auto noBounceSurface=transportRender();frame.scene.climate[CL_worldGi]=1;frame.scene.climate[CL_bounce]=4;
        auto bounceSurface=transportRender();long surfaceDelta=0;for(size_t i=0;i<bounceSurface.size();i+=4)surfaceDelta+=int(bounceSurface[i])-int(noBounceSurface[i]);
        Check(surfaceDelta>100,"World bounce is composed onto actual multisampled surfaces");
        shadowFrame.scene.climate[CL_worldGi]=0;Check(sampleGi()[center]<3,"Disabling world GI removes indirect contribution");
        shadowFrame.scene.climate[CL_worldGi]=1;shadowFrame.occlusion.reset();Check(sampleGi()[center]<3,"Map removal does not leave stale GI");
        shadowFrame.scene.objects[0].shadows=true;shadowFrame.occlusion.reset();Check(sampleShadows()[center]>245,"Missing map cache never reuses previous map shadows");
    }
    EndDevice();
    std::cout << "Environment D3D11 rendering checks passed\n";
}
