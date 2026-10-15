#pragma once
#include "EnvironmentShaderCompiler.h"
#include "EnvironmentClimateMath.h"
#include "EnvironmentStormMath.h"
#include "EnvironmentWeatherMath.h"
#include "EnvironmentShaders.h"

namespace Filmmaker::Environment {
struct EnvironmentShaderRequest {const char *entry,*profile;int ms,cm;};
inline constexpr UINT EnvironmentShaderFlags=D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3;
inline const std::string& EnvironmentShaderSource(){
    static const std::string source=ClimateShaderDefines()+"#define STORM_POINTS "+std::to_string(kStormPointCount)+
        "\n#define RAIN_MOTION_BLUR "+std::to_string(kRainMotionBlur)+"\n#define RAIN_BRIGHTNESS "+std::to_string(kRainBrightness)+
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
}
