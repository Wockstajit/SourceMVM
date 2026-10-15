#pragma once
namespace Filmmaker::Environment {
inline constexpr char kGiShader[]=R"HLSL(
cbuffer GiBuild:register(b0) {
    float4 gridLowSize;
    float4 gridInfo; // resolution, cell size, light count, sun enabled
    float4 sunDirection, sunRadiance;
    float4 lightPositionRange[16],lightColorIntensity[16],lightDirectionCone[16];
    float4 lightRightWidth[16],lightUpHeight[16],lightStudio[16],lightPatternTime[16];
};
cbuffer GiObject:register(b1) { row_major float4x4 model; float4 albedo; };
RWTexture3D<uint> voxelOutput:register(u0);
RWTexture3D<uint> emissionOutput:register(u3);
Texture3D<uint> emissionInput:register(t4);
Texture3D<uint> voxelInput:register(t0);
RWTexture3D<float4> radianceOutput:register(u1);
RWTexture3D<float4> directionOutput:register(u2);
)HLSL"
#include "EnvironmentStudioShaders.h"
#include "EnvironmentShadowShaders.h"
R"HLSL(
)HLSL"
#include "EnvironmentGiVoxelShaders.h"
R"HLSL(
float VoxelVisibility(float3 start,float3 end) {
    float3 delta=end-start;float extent=length(delta);float3 ray=delta/max(extent,1e-5);
    // Half-cell steps cover diagonal blockers. Outside the prepared volume the result is unknown.
    [loop]for(int i=2;i<448;++i){
        float distance=i*gridInfo.y*.5;
        if(distance>=extent)return 1;
        int3 cell=(int3)floor((start+ray*distance-gridLowSize.xyz)/gridInfo.y);
        if(any(cell<0)||any(cell>=(int)gridInfo.x))return 1;
        if(voxelInput.Load(int4(cell,0))!=0)return 0;
    }
    return 0;
}
[numthreads(4,4,4)]
void InjectCS(uint3 cell:SV_DispatchThreadID) {
    uint v=voxelInput.Load(int4(cell,0));
    if(v==0){radianceOutput[cell]=0;directionOutput[cell]=0;return;}
    float3 n=float3(v&63,(v>>6)&63,(v>>12)&63)/63*2-1;n=normalize(n);
    float3 color=float3((v>>18)&15,(v>>22)&15,(v>>26)&15)/15;
    float3 p=gridLowSize.xyz+(cell+.5)*gridInfo.y;
    float3 energy=0,bestNormal=0;float strongest=0;
    // Collision meshes can have inconsistent winding. Evaluate the side facing each emitter.
    if(gridInfo.w>.5){float3 offset=n*(dot(n,sunDirection.xyz)>=0?1:-1)*gridInfo.y;
        float3 contribution=sunRadiance.rgb*abs(dot(n,sunDirection.xyz))*WorldSunShadow(p+offset+sunDirection.xyz*gridInfo.y);
        energy+=contribution;strongest=dot(contribution,float3(.2126,.7152,.0722));bestNormal=n*(dot(n,sunDirection.xyz)>=0?1:-1);}
    [loop]for(int l=0;l<(int)gridInfo.z;++l){
        if(lightColorIntensity[l].w<=0)continue;
        [loop]for(int sample=0;sample<StudioSamples(l);++sample){
            float3 dir;float attenuation=StudioAttenuation(l,sample,p,dir);
            if(attenuation>.001){float3 offset=n*(dot(n,dir)>=0?1:-1)*gridInfo.y;
                float visibility=lightShadowInfo[l].x>=0?WorldLightShadowSample(l,p+offset,sample):
                    VoxelVisibility(p+offset,StudioPosition(l,sample));
                float3 contribution=lightColorIntensity[l].rgb*lightColorIntensity[l].w*attenuation*abs(dot(n,dir))*visibility;
                energy+=contribution;float strength=dot(contribution,float3(.2126,.7152,.0722));
                if(strength>strongest){strongest=strength;bestNormal=n*(dot(n,dir)>=0?1:-1);}
            }
        }
    }
    uint encodedEmission=emissionInput.Load(int4(cell,0));
    float3 emitted=float3(encodedEmission&255,(encodedEmission>>8)&255,(encodedEmission>>16)&255)/255*((encodedEmission>>24)/255.*16);
    if(dot(emitted,emitted)>0)bestNormal=0; // emission radiates from both sides of the coarse voxel
    radianceOutput[cell]=float4(color*energy+emitted,1);
    directionOutput[cell]=float4(bestNormal,encodedEmission>0?1:0);
}
)HLSL";
}
