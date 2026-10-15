#pragma once
#include "../Relight/RelightMath.h"

namespace Filmmaker::Environment {
struct VfxLightPulse {
    Relight::Light light;
    double start = 0, duration = 0;
    int entity = -1;
    int userid = -1;
    bool sustained = false;
    double end = -1;
    float endEnergy = 0;
};
// Demo time owns the lifetime: pausing freezes a flash; rewinding discards it.
inline float VfxPulseEnergy(double now, double start, double duration) {
    if (!std::isfinite(now) || !std::isfinite(start) || !std::isfinite(duration) || duration <= 0 || now < start || now >= start + duration)
        return 0;
    const float remaining = float((start + duration - now) / duration);
    const float t=std::min(1.f, remaining * 4.f);
    return t*t*(3-2*t);
}
inline constexpr double VfxExtinguishFade = .18;
inline constexpr double VfxMuzzleDuration = .06;
inline float VfxMuzzleEnergy(double now, double start, double duration) {
    if (!std::isfinite(now) || !std::isfinite(start) || !std::isfinite(duration) || duration <= 0)
        return 0;
    const double age = now - start;
    if (age < 0 || age >= duration) return 0;
    const float t = float(age / duration);
    // A shot is an impulse, not a lamp held at full power until the next round.
    // Smooth endpoints avoid a hard light switch while keeping peak illumination.
    return 1 - t*t*(3-2*t);
}
inline constexpr double VfxIgnitionFade = .18;
inline float VfxFireFlicker(double time,unsigned seed) {
    if(!std::isfinite(time))return 0;
    const double phase=seed*2.39996323;
    return float(.9+.1*std::sin(time*12.7+phase)+.06*std::sin(time*27.3+phase*1.7)+.04*std::sin(time*43.1+phase*.6));
}
// Smooth, independent fire motion for the native particle lights. Never blink off.
inline float VfxNativeFireFlicker(double time,unsigned seed) {
    if(!std::isfinite(time))return 0;
    // Coherent value noise: irregular fire surges, without frame-random jumps.
    auto noise=[seed](double t){
        const auto cell=std::floor(t);const double u=t-cell;
        auto value=[seed](double c){uint32_t h=uint32_t(int64_t(c))^seed;
            h^=h>>16;h*=0x7feb352dU;h^=h>>15;h*=0x846ca68bU;h^=h>>16;
            return double(h&0xffffU)/32767.5-1.;};
        const double a=value(cell),b=value(cell+1);
        return a+(b-a)*u*u*(3-2*u);
    };
    return float(.88+.30*noise(time*6.1)+.14*noise(time*12.7)+.10*noise(time*1.9));
}
// Most of a fire's energy moves together, with smaller local flame variations.
// Independent patches alone average away the flicker as their footprints overlap.
inline float VfxNativePatchFlicker(double time,unsigned handle,unsigned slot) {
    return .85f*VfxNativeFireFlicker(time,handle)+.15f*VfxNativeFireFlicker(time,handle^(0x9e3779b9U*(slot+1)));
}
inline float VfxIgnitionEnergy(double age) {
    const float t=float(std::clamp(age/VfxIgnitionFade,0.,1.));return t*t*(3-2*t);
}
inline float VfxViewmodelEnergy(const Relight::Light& light) {
    // The first-person mesh sits in a separate projection very close to the
    // emitter. Use a visible, broad flash response without overpowering materials.
    if(!light.transient)return 1;
    if(light.identity=="vfx:muzzle"&&light.viewmodelEmitter)return .18f;
    if(light.identity=="vfx:held")return .08f;
    return 1;
}
inline bool VfxOwnMuzzle(const Relight::Light& light) {
    return light.transient && light.viewmodelEmitter && light.identity=="vfx:muzzle";
}
inline float VfxLightEnergy(const VfxLightPulse& pulse,double now) {
    if (pulse.end < 0 && pulse.light.identity == "vfx:muzzle")
        return VfxMuzzleEnergy(now, pulse.start, pulse.duration);
    if(pulse.end<0)return pulse.sustained?(now>=pulse.start&&now<pulse.start+pulse.duration?VfxIgnitionEnergy(now-pulse.start):0.f):VfxPulseEnergy(now,pulse.start,pulse.duration);
    const float t=float((now-pulse.end)/VfxExtinguishFade);
    if(!std::isfinite(t)||t<0||t>=1)return 0;
    return pulse.endEnergy*(1-t*t*(3-2*t));
}
inline void EndVfxPulse(VfxLightPulse& pulse,double now) {
    if(pulse.end>=0)return;
    pulse.endEnergy=VfxLightEnergy(pulse,now);pulse.end=now;
}
inline bool VfxLightExpired(const VfxLightPulse& pulse,double now) {
    return now>=(pulse.end<0?pulse.start+pulse.duration:pulse.end+VfxExtinguishFade);
}
inline Relight::Light VfxEmitter(const char* identity, Vec3 position, Vec3 color, float lumens, float range, float radius) {
    Relight::Light light;
    light.identity = identity; light.kind = "light_omni"; light.position = position;
    light.color = color; light.lumens = lumens; light.range = range;
    light.sourceRadius = radius; light.shape = 0; light.transient = true;
    light.minimumRoughness = .5f; // broad flame emission, without needle-like specular peaks
    return light;
}
// Four representative emitting patches, each on an actual burning point.
// Farthest-point seeding covers spread and corners; medoids never place a light
// at a centroid inside a wall between separate parts of the inferno.
inline std::vector<Vec3> VfxFireAnchors(const std::vector<Vec3>& points) {
    if(points.size()<=4)return points;
    std::vector<Vec3> seeds{points.front()};
    for(unsigned k=1;k<4;++k){float farthest=-1;Vec3 best{};
        for(const auto& p:points){float nearest=INFINITY;for(const auto& q:seeds){const auto d=p-q;nearest=std::min(nearest,Dot(d,d));}
            if(nearest>farthest){farthest=nearest;best=p;}}
        if(farthest<1)break;seeds.push_back(best);
    }
    for(unsigned pass=0;pass<4;++pass){std::vector<std::vector<Vec3>> groups(seeds.size());
        for(const auto& p:points){unsigned closest=0;float nearest=INFINITY;
            for(unsigned i=0;i<seeds.size();++i){const auto d=p-seeds[i];const float distance=Dot(d,d);if(distance<nearest){nearest=distance;closest=i;}}
            groups[closest].push_back(p);
        }
        for(unsigned i=0;i<groups.size();++i){if(groups[i].empty())continue;Vec3 center{};for(const auto& p:groups[i])center=center+p;
            center=center*(1.f/groups[i].size());float nearest=INFINITY;
            for(const auto& p:groups[i]){const auto d=p-center;const float distance=Dot(d,d);if(distance<nearest){nearest=distance;seeds[i]=p;}}
        }
    }
    return seeds;
}
}
