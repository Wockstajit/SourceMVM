#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeCoatingMath.h"
#include <iostream>
using namespace Filmmaker::Environment;
int main(){
    OcclusionMesh world;
    world.positions={{-128,-128,0},{128,-128,0},{128,128,0},{-128,128,0},
                     {0,0,0},{0,0,128},{128,0,128}};
    world.indices={0,1,2,0,2,3,4,5,6};
    Climate c;
    if(!NativeCoatingTriangles(world,c).empty())return 1;
    c[CL_puddles]=1;
    auto area=[](const std::vector<Vec3>& v){float total=0;for(size_t i=0;i+2<v.size();i+=3)total+=std::abs(Cross(v[i+1]-v[i],v[i+2]-v[i]).z)*.5f;return total;};
    auto all=NativeCoatingTriangles(world,c);
    if(all.empty()||all.size()%3)return 2;
    for(auto p:all)if(std::abs(p.z-kNativeWaterLift)>.0001f)return 3;
    c[CL_surfaceRadius]=64;c[CL_puddleHeight]=0;c[CL_puddleDepth]=2;
    auto local=NativeCoatingTriangles(world,c);
    if(local.empty()||area(local)>=area(all))return 4;
    for(auto p:local)if(p.x*p.x+p.y*p.y>64*64+.01f)return 5;
    c[CL_puddleHeight]=100;
    if(!NativeCoatingTriangles(world,c).empty())return 6;
    c[CL_surfaceRadius]=0;c[CL_puddles]=.5f;
    auto partial=NativeCoatingTriangles(world,c),repeat=NativeCoatingTriangles(world,c);
    if(partial.empty()||area(partial)>=area(all)||partial.size()!=repeat.size())return 7;
    for(size_t i=0;i<partial.size();++i)if(Dot(partial[i]-repeat[i],partial[i]-repeat[i])!=0)return 8;
    c[CL_wet]=1;c[CL_puddles]=0;
    if(!NativeCoatingTriangles(world,c).empty())return 10;
    c[CL_puddles]=.2f;auto low=NativeCoatingTriangles(world,c);
    c[CL_puddles]=.8f;auto high=NativeCoatingTriangles(world,c);
    if(area(high)<=area(low)*2)return 11;
    c[CL_wet]=0;if(area(NativeCoatingTriangles(world,c))!=area(high))return 12;
    OcclusionMesh ceiling=world;ceiling.indices={2,1,0,3,2,0};
    if(!NativeCoatingTriangles(ceiling,c).empty())return 13;
    OcclusionMesh slope=world;slope.indices={0,1,2,0,2,3};slope.positions[1].z=80;slope.positions[2].z=80;
    if(!NativeCoatingTriangles(slope,c).empty())return 14;
    // Full coverage of a large play floor stays inside the geometry budget; kilometre backdrops are skipped.
    OcclusionMesh vast;vast.positions={{-1400,-1400,0},{1400,-1400,0},{1400,1400,0},{-1400,1400,0}};vast.indices={0,1,2,0,2,3};
    {Climate full;full[CL_puddles]=1;std::vector<Vec3> big;
     try{big=NativeCoatingTriangles(vast,full);}catch(...){return 15;}
     if(big.empty()||big.size()>1200000)return 16;
     if(area(big)<2800.f*2800*.2f)return 17;
     vast.positions={{-8000,-8000,0},{8000,-8000,0},{8000,8000,0},{-8000,8000,0}};if(!NativeCoatingTriangles(vast,full).empty())return 18;}
    c[CL_snow]=1;
    if(!NativeCoatingTriangles(world,c).empty())return 9;
    std::cout<<"Native coating geometry: walls, area, level, coverage and deterministic placement passed\n";
}
