#pragma once
#include "EnvironmentShaderCompiler.h"
#include "EnvironmentShaderReachMath.h"
#include "EnvironmentClimateMath.h"
#include "EnvironmentCoatingMath.h"
#include "EnvironmentStormMath.h"
#include "EnvironmentWeatherMath.h"
#include "EnvironmentShaders.h"

namespace Filmmaker::Environment {
struct EnvironmentShaderRequest {const char *entry,*profile;int ms,cm;};
inline constexpr UINT EnvironmentShaderFlags=D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3;
inline const std::string& EnvironmentShaderSource(){
    static const std::string source=ClimateShaderDefines()+"#define STORM_POINTS "+std::to_string(kStormPointCount)+
        "\n#define RAIN_MOTION_BLUR "+std::to_string(kRainMotionBlur)+"\n#define RAIN_BRIGHTNESS "+std::to_string(kRainBrightness)+
        "\n#define SNOW_BRIGHTNESS "+std::to_string(kSnowBrightness)+"\n#define SNOW_MAX_LIFT "+std::to_string(kSnowMaxLift)+
        "\n#define RAIN_MAX_OPACITY "+std::to_string(kRainMaxOpacity)+"\n#define RAIN_SHAPE_GAIN "+std::to_string(kRainShapeGain)+
        "\n#define SPLASH_GRID "+std::to_string(kSplashGrid)+
        "\n#define SPLASH_PARTS "+std::to_string(kSplashParts)+"\n#define SPLASH_NEAR "+std::to_string(int(kSplashNear))+
        "\n#define SPLASH_FAR "+std::to_string(int(kSplashFar))+"\n"+kShader;
    return source;
}
// One list owns both device creation and bytecode preparation. Missing a variant here used
// to make enabling Sky compile it synchronously on the render thread and stall presentation.
inline constexpr const char* EnvironmentPixelShaderEntries[] = {
    "AtmospherePS", "AtmospherePixelPS", "MeshPS", "MaterialMeshPS", "WeatherPS", "VolumePS",
    "CompositePS", "StormPS", "SplashPS", "WeatherVeilPS", "VolumeBlurPS", "SkyVolumePS"};
inline constexpr size_t EnvironmentPixelShaderCount =
    sizeof(EnvironmentPixelShaderEntries) / sizeof(EnvironmentPixelShaderEntries[0]);
// The build-time bundle and runtime fallback must prepare exactly the same programs.
inline std::vector<EnvironmentShaderRequest> EnvironmentShaderRequests(){
    std::vector<EnvironmentShaderRequest> requests{{"ScreenVS","vs_5_0",0,0},
        {"AtmosphereEdgePS","ps_5_0",1,1}};
    for(int cm=0;cm<2;++cm)requests.push_back({"BlitPS","ps_5_0",0,cm});
    for(const char* entry:{"StormVS","MeshVS"})requests.push_back({entry,"vs_5_0",0,0});
    for(int cm=0;cm<2;++cm)requests.push_back({"WeatherVS","vs_5_0",0,cm}); // lights particles from the scene color
    for(int ms=0;ms<2;++ms)requests.push_back({"SplashVS","vs_5_0",ms,0}); // reads scene depth, so it has a depth-MSAA variant
    for(int ms=0;ms<2;++ms)for(int cm=0;cm<2;++cm)
        for(const char* entry:EnvironmentPixelShaderEntries)
            requests.push_back({entry,"ps_5_0",ms,cm});
    return requests;
}
// The text one program compiles from and is keyed on: what its entry point reaches in the source
// (EnvironmentShaderReachMath.h). An edit to the fog march then rebuilds the programs that march,
// not all of them. The build-time bundle and the runtime fallback both compile through here, so
// their keys agree. Should the source ever be laid out in a way the reach parser does not
// understand, every program compiles the whole source, as before.
inline const std::string& EnvironmentShaderProgramSource(const char* entry){
    static const auto programs=[]{
        const auto& source=EnvironmentShaderSource();
        const auto parsed=ParseShaderReach(source);
        std::unordered_map<std::string,std::string> texts;
        for(const auto& request:EnvironmentShaderRequests())
            if(!texts.count(request.entry))
                if(auto text=ShaderReachText(parsed,request.entry);!text.empty())texts.emplace(request.entry,std::move(text));
        return texts;
    }();
    const auto found=programs.find(entry);
    return found!=programs.end()?found->second:EnvironmentShaderSource();
}
}
