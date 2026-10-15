#pragma once
namespace Filmmaker::Relight {
inline constexpr char ScreenGiShader[]=R"HLSL(
cbuffer Screen:register(b0){
    float4 scale,bias,lower,upper;
    float4 camera,size;
    row_major float4x4 worldToClip;
};
Texture2D<float4> positions:register(t0);
Texture3D<float4> irradiance:register(t1);
Texture3D<float4> distances:register(t2);
RWTexture3D<float4> output:register(u0);
float4 Position(int2 p){return positions.Load(int3(clamp(p,0,int2(size.xy)-1),0));}
float3 Normal(int2 pixel,float3 p){
    float4 l=Position(pixel-int2(1,0)),r=Position(pixel+int2(1,0));
    float4 u=Position(pixel-int2(0,1)),d=Position(pixel+int2(0,1));
    float3 dx=r.w>0&&(l.w<=0||length(r.xyz-p)<length(p-l.xyz))?r.xyz-p:p-l.xyz;
    float3 dy=d.w>0&&(u.w<=0||length(d.xyz-p)<length(p-u.xyz))?d.xyz-p:p-u.xyz;
    float3 n=cross(dx,dy);float n2=dot(n,n);
    if(n2<1e-12)return normalize(camera.xyz-p);
    n*=rsqrt(n2);return dot(n,camera.xyz-p)<0?-n:n;
}
[numthreads(8,8,1)]void Build(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=uint2(size.xy)))return;
    float4 position=Position(id.xy);
    if(position.w<=0){[unroll]for(uint b=0;b<6;++b)output[uint3(id.xy,b)]=float4(0,0,0,-1);return;}
    float3 n=Normal(id.xy,position.xyz);
    float3 q=clamp(position.xyz*scale.xyz+bias.xyz,lower.xyz,upper.xyz)*64+n*.35;
    int3 base=(int3)floor(q);
    // Quadratic B-spline support is continuous when the nearest probe changes.
    // Three rows per axis retain neighbouring light at a dead probe plane without
    // a hard fraction clamp that swaps nonzero-weight corners at cage boundaries.
    float3 fraction=q-.5-float3(base);
    float3 colors[6];[unroll]for(uint b=0;b<6;++b)colors[b]=0;
    float total=0,validTotal=0;
    [loop]for(uint corner=0;corner<27;++corner){
        int3 offset=int3(corner%3,(corner/3)%3,corner/9)-1;
        int3 cell=clamp(base+offset,0,63);float3 delta=q-(float3(cell)+.5);
        float code=distances.Load(int4(cell,0)).w;
        uint axis=(uint)floor(abs(code));float amount=frac(abs(code))*(code>=0?1:-1);
        delta-=float3(axis==1,axis==2,axis==3)*amount;
        float rawSquared=dot(delta,delta),d2=max(rawSquared,.0001);
        float3 angular=rawSquared>1e-8?delta*delta/rawSquared:float3(1,1,1)/3;
        float visibility=0,valid=0;
        [unroll]for(uint a=0;a<3;++a){
            uint band=a+(delta[a]>=0?0:3);
            float4 m=distances.Load(int4(cell.xy,cell.z+band*64,0));
            float beyond=max(sqrt(d2)-m.x,0),variance=max(m.y-m.x*m.x,.0001);
            float v=variance/(variance+beyond*beyond);
            visibility+=angular[a]*v*v*v*m.z;valid=max(valid,m.z);
        }
        float facing=max(.0001,(1-dot(delta,n)*rsqrt(d2))*.5);
        float3 edge=.5+float3(offset)*fraction;
        float3 w=float3(offset.x==0?.75-fraction.x*fraction.x:.5*edge.x*edge.x,offset.y==0?.75-fraction.y*fraction.y:.5*edge.y*edge.y,offset.z==0?.75-fraction.z*fraction.z:.5*edge.z*edge.z);float trilinear=w.x*w.y*w.z,weight=trilinear*visibility*(facing*facing+.2);
        // Invalid probes hand their kernel share to the rest of the cage (see WorldQuery).
        total+=weight;validTotal+=trilinear*valid;
        [unroll]for(uint b=0;b<6;++b)colors[b]+=weight*irradiance.Load(int4(cell.xy,cell.z+b*64,0)).rgb;
    }
    float depth=mul(worldToClip,float4(position.xyz,1)).w;
    float confidence=saturate(total/max(validTotal,1e-4)*1e4);
    [unroll]for(uint b=0;b<6;++b)output[uint3(id.xy,b)]=float4(colors[b]/max(total,1e-4)*confidence,depth);
}
)HLSL";
}
