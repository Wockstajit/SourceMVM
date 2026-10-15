#pragma once
#include "EnvironmentMath.h"
#include <array>
#include <string>
namespace Filmmaker::Environment {
// The light record slot, shadow comparison sampler and baked lightmap inputs are
// now derived per program by HasNativeSunMaterialAbi in EnvironmentNativeMaterialMath.h.
// The thirteen hand-audited hashes that used to be listed here are asserted against
// that derivation by CheckDerivedAbiAgainstShippedShaders in
// Automation/tests/environment-native-material-tests.cpp.
// Transient experiment, deliberately excluded from scene/preset serialization.
struct LightingProof {
    bool enabled=false, fixture=true, shadows=true;
    uint64_t generation=0;
    std::string map, identity;
    Vec3 position{}, color{1,1,1};
    float lumens=0, authoredScale=1, intensity=1, range=900, indirect=0;
};
inline float ProofLinear(float s) {return s<=.04045f?s/12.92f:std::pow((s+.055f)/1.055f,2.4f);}
// Native shipped program cd2f50b4703ab117: offsets independently verified in DXBC.
// Full-sphere omni test: one box frustum bounds evaluation; no shadow projection yet.
inline std::array<float,120> ProofOmniRecord(const LightingProof& p){
    std::array<float,120> b{}; const float r=(std::max)(1.f,p.range);
    b[0]=b[5]=1/r;b[10]=.5f/r;b[15]=1;
    b[3]=-p.position.x/r;b[7]=-p.position.y/r;b[11]=.5f-.5f*p.position.z/r;
    b[32]=p.position.x;b[33]=p.position.y;b[34]=p.position.z;b[35]=1;
    b[36]=1;b[37]=-1/r;
    // VRF full-sphere omni conversion: lumens / (4pi / (4pi*10)).
    float power=p.fixture?p.lumens*p.authoredScale*p.intensity*10.f:0;
    b[40]=ProofLinear(p.color.x)*power;b[41]=ProofLinear(p.color.y)*power;b[42]=ProofLinear(p.color.z)*power;
    b[47]=1;b[48]=1;b[56]=b[57]=.999999f;b[64]=.04f;
    // Zero illumination OBB rows mean no additional box rejection. Cookie/shadow disabled.
    return b;
}
}
