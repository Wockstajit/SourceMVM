#pragma once
#include "EnvironmentClimateMath.h"
#include <cstdint>
namespace Filmmaker::Environment {
// Absolute-time events: no accumulated RNG, frame delta or camera-relative placement.
inline float StormRandom(uint32_t n) {
    n ^= n >> 16; n *= 0x7feb352du; n ^= n >> 15; n *= 0x846ca68bu; n ^= n >> 16;
    return float(n & 0xffffffu) / 16777216.f;
}
inline constexpr int kStormTrunkPoints = 65;
inline constexpr int kStormPointCount = kStormTrunkPoints + 3 * 16 + 2 * 8;
struct StormSample {
    Vec3 source{};
    std::array<Vec3, kStormPointCount> points{};
    std::array<float, kStormPointCount> width{}, gain{};
    std::array<int, kStormPointCount> parent{};
    float flash = 0, range = 0;
    double age = 0;
    uint32_t event = 0;
};
inline double StormEventStart(const Climate &c, uint32_t event) {
    return double(event) * (std::max)(1.f,c[CL_stormInterval]) + .5 +
           StormRandom(event + uint32_t(c[CL_stormSeed])) * (std::max)(1.f,c[CL_stormInterval]) * .35;
}
// Midpoint displacement with decreasing displacement at each octave. Endpoints are fixed;
// branches share an actual parent vertex, so neither gaps nor disconnected forks can appear.
template<size_t N> inline std::array<Vec3,N> StormChannel(Vec3 a,Vec3 b,uint32_t seed,float spread) {
    std::array<Vec3,N> p{};p[0]=a;p[N-1]=b;
    Vec3 axis=Normalize(b-a),u=Normalize(Cross(axis,std::abs(axis.z)>.9f?Vec3{0,1,0}:Vec3{0,0,1}));
    Vec3 v=Cross(axis,u);
    for(int step=int(N)-1;step>1;step/=2) {
        for(int j=0;j<int(N)-1;j+=step) {
            int m=j+step/2;uint32_t key=seed+uint32_t(m*131+step*719);
            p[m]=(p[j]+p[j+step])*.5f+
                (u*(StormRandom(key)*2-1)+v*(StormRandom(key+1)*2-1))*spread;
        }
        spread*=.53f;
    }
    return p;
}
inline StormSample EvaluateStorm(const Climate &c, double time) {
    StormSample s;
    if(c[CL_storm]<.5f||c[CL_enabled]<.5f||!std::isfinite(time)||time<0)return s;
    const double interval=(std::max)(1.f,c[CL_stormInterval]);
    s.event=c[CL_stormHold]>.5f?0:uint32_t(std::fmod(std::floor(time/interval),4294967296.0));
    // Keep the previous event's trailing pulse across an interval boundary.
    if(c[CL_stormHold]<.5f&&s.event>0&&time<StormEventStart(c,s.event))--s.event;
    uint32_t seed=s.event*37u+uint32_t(c[CL_stormSeed]);
    s.age=c[CL_stormHold]>.5f?.025:time-StormEventStart(c,s.event);
    auto pulse=[&](double start,double duration,float gain){
        double t=s.age-start;
        return t>=0&&t<duration?gain*float(std::exp(-t*6/duration)):0.f;
    };
    double second=.08+StormRandom(seed+81)*.13,third=second+.13+StormRandom(seed+82)*.09;
    s.flash=c[CL_stormStrength]*(pulse(0,.16,1)+pulse(second,.12,.4f+.4f*StormRandom(seed+83))+
        pulse(third,.24,.15f+.25f*StormRandom(seed+84)));
    float angle=StormRandom(seed+1)*6.2831853f;
    float radius=c[CL_stormRadius]*(.65f+.35f*StormRandom(seed+2));
    Vec3 foot{c[CL_stormX]+std::cos(angle)*radius,c[CL_stormY]+std::sin(angle)*radius,c[CL_stormZ]};
    float top=(std::max)(foot.z+300,c[CL_cloudBase]+c[CL_cloudDepth]*.6f),height=top-foot.z;
    Vec3 head=foot+Vec3{(StormRandom(seed+3)-.5f)*height*.3f,(StormRandom(seed+4)-.5f)*height*.3f,height};
    auto trunk=StormChannel<kStormTrunkPoints>(head,foot,seed+10,height*.15f);
    s.source=trunk[12];s.range=(std::max)(3000.f,radius*2+height);
    for(int i=0;i<kStormTrunkPoints;++i){
        s.points[i]=trunk[i];s.parent[i]=(std::max)(0,i-1);
        s.width[i]=(2.8f-1.6f*i/(kStormTrunkPoints-1))*(.8f+.4f*StormRandom(seed+200+i));
        s.gain[i]=.8f+.2f*StormRandom(seed+400+i);
    }
    int next=kStormTrunkPoints;
    for(int branch=0;branch<5;++branch){
        int segments=branch<3?16:8;
        int parent=branch<3?12+branch*12+int(StormRandom(seed+500+branch)*6):kStormTrunkPoints+(branch-3)*16+6;
        Vec3 start=s.points[parent];float a=angle+branch*2.399963f+StormRandom(seed+520+branch);
        float reach=height*(branch<3?.22f:.09f)*(.7f+.6f*StormRandom(seed+530+branch));
        Vec3 end=start+Vec3{std::cos(a)*reach,std::sin(a)*reach,-reach*(.35f+.6f*StormRandom(seed+540+branch))};
        auto path=StormChannel<17>(start,end,seed+600+branch*100,reach*.22f);
        float branchWidth=s.width[parent]*.62f;
        // Secondary forks are occasional but deterministic; zero width deactivates them.
        bool enabled=branch<3||StormRandom(seed+900+branch)>.35f;
        for(int i=1;i<=segments;++i,++next){
            s.points[next]=path[i*(16/segments)];s.parent[next]=i==1?parent:next-1;
            s.width[next]=enabled?branchWidth*(1-.9f*float(i)/segments):0;
            s.gain[next]=(.35f+.25f*StormRandom(seed+1000+next))*(1-.7f*float(i)/segments);
        }
    }
    return s;
}
} // namespace Filmmaker::Environment
