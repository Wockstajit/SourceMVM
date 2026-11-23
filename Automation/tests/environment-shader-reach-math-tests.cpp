// What one Environment entry point compiles from (EnvironmentShaderReachMath.h): the declarations it
// reaches, so an edit rebuilds only the programs that use what changed.
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderReachMath.h"
#include <cstdlib>
#include <iostream>
#include <string>
using namespace Filmmaker::Environment;
void Check(bool ok, const char *name) {
    if (!ok) { std::cerr << "FAILED: " << name << "\n"; std::exit(1); }
}
bool Has(const std::string &text, const char *part) { return text.find(part) != std::string::npos; }
std::string Reach(const std::string &source, const char *entry) {
    return ShaderReachText(ParseShaderReach(source), entry);
}

const std::string kSource = R"(#define STEPS 8
#define FOG_SCALE (STEPS*2)
cbuffer Frame : register(b0) { float4 camera; float4 fogInfo[FOG_SCALE]; };
Texture2D<float4> colorTexture : register(t0);
#if COLOR_MS
Texture2DMS<float> depthTexture : register(t1);
#else
Texture2D<float> depthTexture : register(t1);
#endif
struct Screen { float4 position : SV_Position; };
static const float kFogFloor = 2, kUnused = 3;
static const float kWeights[2] = { 0.25, 0.75 };
// A comment that mentions RainAt and Mesh.
float FogDensity(float3 p) { return exp(-p.z * kFogFloor); }
float4 FogMarch(float3 dir) {
    float sum = 0;
    [loop] for (int k = 0; k < STEPS; ++k) sum += FogDensity(dir * k);
    return sum;
}
float RainAt(float2 p) { return frac(p.x * 7 + kWeights[1]); }
float Shade(float x) { return x; }
float Shade(float3 x) { return x.y; }
float ReadDepth(float2 p) {
#if COLOR_MS
    return depthTexture.Load(int2(p), 0);
#else
    return depthTexture.Load(int3(p, 0));
#endif
}
Screen ScreenVS(uint id : SV_VertexID) { Screen o; o.position = float4(id, 0, 0, 1); return o; }
float4 FogPS(Screen i) : SV_Target { return FogMarch(normalize(camera.xyz)) * Shade(ReadDepth(i.position.xy)); }
[earlydepthstencil]
float4 RainPS(Screen i) : SV_Target { return RainAt(i.position.xy) * colorTexture.Load(int3(0, 0, 0)); }
)";

int main() {
    const auto parsed = ParseShaderReach(kSource);
    Check(parsed.understood, "the test source is understood");

    // An entry carries what it reaches through any chain of uses and nothing else.
    const auto fog = Reach(kSource, "FogPS");
    Check(Has(fog, "float4 FogPS") && Has(fog, "FogMarch") && Has(fog, "FogDensity") && Has(fog, "kFogFloor") &&
              Has(fog, "#define STEPS"),
          "fog entry keeps its chain: march, density, constant and macro");
    Check(Has(fog, "struct Screen"), "a struct used as a parameter type is kept");
    Check(Has(fog, "float Shade(float x)") && Has(fog, "float Shade(float3 x)"), "every overload of a used name is kept");
    Check(!Has(fog, "RainAt") && !Has(fog, "RainPS") && !Has(fog, "kWeights") && !Has(fog, "ScreenVS"),
          "the fog entry leaves out rain, its constants and other entries");
    Check(!Has(fog, "comment"), "comments are left out");
    const auto rain = Reach(kSource, "RainPS");
    Check(Has(rain, "[earlydepthstencil]\nfloat4 RainPS"), "attributes stay with their function");
    Check(Has(rain, "kWeights") && !Has(rain, "FogMarch") && !Has(rain, "kFogFloor"), "rain entry keeps only rain");

    // Uniform declarations and conditional structure are kept for every entry, as written.
    for (const char *entry : {"FogPS", "RainPS", "ScreenVS"}) {
        const auto text = Reach(kSource, entry);
        Check(Has(text, "cbuffer Frame : register(b0)") && Has(text, "Texture2D<float4> colorTexture") &&
                  Has(text, "#if COLOR_MS\nTexture2DMS<float> depthTexture : register(t1);\n#else\n"
                            "Texture2D<float> depthTexture : register(t1);\n#endif"),
              "uniforms and #if blocks are kept for every entry");
        Check(Has(text, "#define FOG_SCALE") && Has(text, "#define STEPS"), "macros the uniforms use are kept");
    }
    Check(Has(fog, "#if COLOR_MS\n    return depthTexture.Load(int2(p), 0);\n#else"),
          "an #if inside a function stays in the function");
    Check(Has(fog, "static const float kFogFloor = 2, kUnused = 3;"), "a declaration with several names stays whole");

    // The point of it: an edit outside an entry's reach leaves its text, and so its cache key, alone.
    auto edited = kSource;
    edited.replace(edited.find("p.x * 7"), 7, "p.x * 9");
    Check(Reach(edited, "FogPS") == fog, "editing rain leaves the fog program's text unchanged");
    Check(Reach(edited, "RainPS") != rain, "editing rain changes the rain program's text");
    edited = kSource;
    edited.replace(edited.find("exp(-p.z"), 8, "exp(-p.y");
    Check(Reach(edited, "FogPS") != fog && Reach(edited, "RainPS") == rain, "editing the fog changes only the fog");
    edited = kSource;
    edited.replace(edited.find("A comment"), 9, "A remark ");
    edited += "/* trailing\n block */\n";
    Check(Reach(edited, "FogPS") == fog && Reach(edited, "RainPS") == rain, "editing comments changes nothing");
    edited = kSource;
    edited.replace(edited.find("#define STEPS 8"), 15, "#define STEPS 9");
    Check(Reach(edited, "FogPS") != fog && Reach(edited, "RainPS") != rain,
          "a macro the shared uniforms use changes every program");

    // Macros: function bodies may define them, and #if conditions keep what they test.
    const std::string bodyMacro = "float A() {\n#define HALF 0.5\nreturn HALF;\n}\nfloat B() { return HALF * 2; }\n"
                                  "float4 Main() : SV_Target { return B(); }\n";
    const auto main = Reach(bodyMacro, "Main");
    Check(Has(main, "float A()") && Has(main, "float B()"), "a macro defined inside a body brings that body along");
    const std::string tested = "#define WIDE 1\n#if WIDE\nstatic const float k = 2;\n#endif\n"
                               "float4 Main() : SV_Target { return 0; }\n";
    Check(Has(Reach(tested, "Main"), "#define WIDE 1"), "a macro an #if tests is kept");

    // Anything the parser does not understand falls back to the whole source (an empty result).
    Check(Reach(kSource, "MissingPS").empty(), "an unknown entry falls back to the whole source");
    const std::string unevenIf = "float4 Main() : SV_Target {\n#if A\nif (x) {\n#else\nif (y) {\n#endif\n"
                                 "return 0; }\nreturn 1;\n}\n";
    Check(!ParseShaderReach(unevenIf).understood, "an #if branch that leaves a brace open is not understood");
    const std::string inParameters = "float4 Main(\n#if A\nfloat a\n#else\nfloat b\n#endif\n) : SV_Target { return 0; }\n";
    Check(Has(Reach(inParameters, "Main"), "#if A\nfloat a\n#else\nfloat b\n#endif"),
          "an #if inside a parameter list stays in its function");
    const std::string betweenParts = "float4 Main()\n#if A\n: SV_Target\n#endif\n{ return 0; }\n";
    Check(!ParseShaderReach(betweenParts).understood, "a directive between the parts of a declaration is not understood");
    Check(!ParseShaderReach("float4 Main() : SV_Target { return 0;\n").understood, "an open brace is not understood");
    Check(!ParseShaderReach("float4 Main() : SV_Target { return 0; }}\n").understood, "a stray brace is not understood");
    Check(!ParseShaderReach("float4 Main() : SV_Target { return 0; } /* open").understood,
          "an unterminated comment is not understood");

    std::cout << "Environment shader reach: chains, overloads, uniforms, #if structure, comments, edits and fallbacks passed\n";
    return 0;
}
