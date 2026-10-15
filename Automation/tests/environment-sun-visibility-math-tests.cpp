#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentSunVisibilityMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeSunCsmMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentClimateMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightMath.h"
#include "../../AfxHookSource2/Filmmaker/Relight/RelightEnvironmentMath.h"
#include <cstdlib>
#include <string>
#include <iostream>
using namespace Filmmaker::Environment;
static void Check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
static bool Near(float a, float b, float epsilon = .001f) { return std::abs(a - b) < epsilon; }
int main() {
    // A close receiver .02 units behind a blocker must not be unshadowed by
    // the native fallback's shared .0002 bias (two units at this depth scale).
    Matrix native{1.f/128,0,0,0,0,1.f/128,0,0,0,0,.0001f,.5f,0,0,0,1};
    const auto sampled=NativeSunReceiverMatrix(native,.0002f,2048);
    const float blocker=.5f,behind=.5f+.02f*.0001f;
    Check(behind-.0002f<blocker,"Regression reproduces a detached native contact");
    Check(behind+sampled[11]-native[11]-.0002f>blocker,"Bound native sun bias retains the wall blocker");
    Check(sampled[11]-.0002f<blocker,"An exposed receiver remains lit");
    for(unsigned i=0;i<16;++i)if(i!=11)Check(sampled[i]==native[i],"Bias compensation preserves cascade footprint and scale");
    // A weapon's per-sample native filter lacks receiver-plane correction. A
    // small pose/depth mismatch on its own blade must stay lit, while a real
    // blocker farther away still shadows it. World trim contacts stay unchanged.
    const auto weapon=NativeSunReceiverMatrix(native,.0002f,2048,1.f);
    const float bladeMismatch=.5f+.04f*.0001f;
    Check(bladeMismatch+sampled[11]-native[11]-.0002f>blocker,"The world tolerance reproduces weapon self-shadow acne");
    Check(bladeMismatch+weapon[11]-native[11]-.0002f<blocker,"Weapon tolerance rejects small self-depth error");
    Check(.5f+.3f*.0001f+weapon[11]-native[11]-.0002f>blocker,"A separate weapon blocker retains its shadow");
    // The uploaded disk must retain the original deterministic quadrature,
    // including the denser blocker search near contact and area-uniform filter.
    const auto search=SunDiskTaps<SunSearchTaps,true>();
    const auto filter=SunDiskTaps<SunFilterTaps,false>();
    for(unsigned i=0;i<SunSearchTaps;++i){
        const float t=(i+.5f)/SunSearchTaps;
        Check(Near(std::hypot(search[i].x,search[i].y),t*t,1e-6f),"Search disk retains quadratic radial spacing");
    }
    for(unsigned i=0;i<SunFilterTaps;++i){
        const float t=(i+.5f)/SunFilterTaps;
        Check(Near(std::hypot(filter[i].x,filter[i].y),std::sqrt(t),1e-6f),"Filter disk retains uniform area coverage");
        Check(Near(filter[i].x,std::sqrt(t)*std::cos(i*2.39996323f),1e-6f),"Filter direction retains the fixed world-space pattern");
    }
    // The penumbra is the distance to the caster times the sun's angular diameter (small angle):
    // 0.53 degrees is about 1/108 of the distance, so a caster 400 units up gives ~3.7 units of blur.
    Check(Near(SunPenumbraWidth(400, SunRealDiameterDegrees), 400 * 2 * std::tan(.2650f * 3.14159265f / 180.f), .01f),
          "Penumbra width is distance * 2 * tan(half angle)");
    Check(SunPenumbraWidth(400, SunRealDiameterDegrees) > 3.5f && SunPenumbraWidth(400, SunRealDiameterDegrees) < 3.9f,
          "The real sun blurs a 400 unit caster by under four units");
    // Contact is sharp, and softness grows linearly with distance and with sun size.
    Check(SunPenumbraWidth(0, 3) == 0 && SunPenumbraWidth(-5, 3) == 0, "A contact point has no penumbra");
    Check(Near(SunPenumbraWidth(200, 3) * 2, SunPenumbraWidth(400, 3), .01f), "Penumbra grows linearly with caster distance");
    Check(SunPenumbraWidth(400, 3) > 5 * SunPenumbraWidth(400, SunRealDiameterDegrees), "A larger sun gives a wider penumbra");
    Check(SunPenumbraWidth(400, 0) == 0, "A point sun casts hard shadows at any distance");
    // The filter never collapses below a bilinear footprint, so a contact does not alias.
    Check(Near(SunFilterRadiusTexels(0, 3, .125f), .75f), "Contact filter radius keeps the minimum footprint");
    Check(Near(SunFilterRadiusTexels(400, 3, .125f), .5f * SunPenumbraWidth(400, 3) / .125f, .01f),
          "Far filter radius is half the penumbra in texels");
    Check(SunFilterRadiusTexels(100, 3, 0) == .75f, "A degenerate texel size falls back to the minimum");
    // Blocker search reaches exactly as far as the widest penumbra can.
    Check(Near(SunSearchRadius(1000, 3), 1000 * SunTanHalfAngle(3), .001f), "Search radius is the largest possible half penumbra");
    Check(SunBlockerInsideCone(.5f, 100, 3) && !SunBlockerInsideCone(10, 100, 3),
          "Only blockers inside the cone the sun subtends can shade the receiver");
    Check(!SunBlockerInsideCone(.01f, 0, 3), "A blocker at the receiver's own depth hides nothing");
    // Edge model: half lit on the geometric edge, fully lit/umbra a half penumbra to either side.
    Check(Near(SunEdgeVisibility(0, 4), .5f) && SunEdgeVisibility(2, 4) == 1 && SunEdgeVisibility(-2, 4) == 0, "Edge ramp is symmetric");
    Check(SunEdgeVisibility(.1f, 0) == 1 && SunEdgeVisibility(-.1f, 0) == 0, "A zero penumbra is a hard edge");
    // The slider range matches the climate table and defaults to the real sun.
    Check(SunTanHalfAngle(99) == SunTanHalfAngle(SunDiameterMaxDegrees), "Sun size is clamped to the slider range");
    Check(Near(SunTanHalfAngle(NAN), SunTanHalfAngle(SunRealDiameterDegrees), 1e-6f), "A non-finite size reads as the real sun");
    const auto& spec = ClimateSpecs()[CL_sunAngle];
    Check(std::string(spec.key) == "sunAngle" && Near(spec.initial, SunRealDiameterDegrees, 1e-4f) && spec.lo == 0 && spec.hi == SunDiameterMaxDegrees,
          "The climate key is sunAngle, 0..5 degrees, default the real sun");
    Filmmaker::Relight::Scene scene;
    Check(Near(scene.sunAngle, SunRealDiameterDegrees, 1e-6f), "The relight scene defaults to the real sun");
    Climate climate;climate[CL_enabled] = 1;climate[CL_sunAngle] = 3;
    WorldLook world;Filmmaker::Relight::ApplyEnvironmentLighting(scene, climate, world);
    Check(Near(scene.sunAngle, 3), "The Lighting page's Sun size reaches the relight scene");
    std::cout << "Sun visibility math checks passed\n";
}
