#pragma once
namespace Filmmaker::Environment {
inline constexpr const char* ParticleShadowShader=R"(
cbuffer Shadow:register(b3){row_major float4x4 worldToShadow;}
cbuffer NativeMaterial:register(b4){float4 controls[19];uint4 flags;}
Texture2D<float4> sheet:register(t8);
SamplerState sheetSampler:register(s0);
SamplerState alternateSampler:register(s1);
float4 SampleSheet(float2 uv){return (uint(controls[3].z)&1u)?sheet.Sample(alternateSampler,uv):sheet.Sample(sheetSampler,uv);}
struct V {float4 position:SV_Position;float4 uv:TEXCOORD0;float2 fade:TEXCOORD1;};
V VS(float3 p:POSITION,float4 uv:TEXCOORD0,float2 fade:TEXCOORD1){
    V o;o.position=mul(worldToShadow,float4(p,1));o.uv=uv;o.fade=fade;return o;
}
void PS(V i){
    // Verified spritecard sheet ABI. Retain animated frame interpolation and
    // material/particle opacity, but not camera-depth soft-particle feathering.
    // Shadows must not fade according to the unrelated main-camera depth image.
    // Distortion/layered sheet modes require different UV reconstruction. Fail
    // closed instead of projecting an opaque rectangle with unverified UVs.
    if(((flags.y>>16)&15u)!=0u || (flags.z&0x40000u))discard;
    float4 texel=SampleSheet(i.uv.xy);
    if((flags.z&32768u)&&i.fade.y>-1&&(flags.z&1024u))
        texel=lerp(texel,SampleSheet(i.uv.zw),i.fade.y);
    if(flags.z&1u)texel.rgb-=float3(.005859375,.009765625,.005859375);
    uint channel=flags.x&15u,operation=(flags.x>>4)&15u;
    float a=texel.a;
    if(channel==0u)a=operation==4u?0:1;
    else if(channel==4u||channel==5u)a=1;
    else if(channel>=6u&&channel<=8u)a=dot(texel.rgb,float3(.299,.587,.114));
    else if(channel>=9u&&channel<=11u)a=operation==4u?0:1;
    else if(channel==12u)a=texel.r;
    else if(channel==13u)a=texel.g;
    else if(channel==14u)a=texel.b;
    if(operation==1u)a*=2;
    else if(operation==3u)a+=1;
    else if(operation==4u)a=1-a;
    else if(operation==5u)a=(a+1)*.5;
    float textureScale=controls[8].y;
    if((uint(controls[3].z)>>12)&1u)textureScale*=i.uv.w;
    a=max(1+textureScale*(a-1),0);
    if(controls[0].w<controls[0].z)a=smoothstep(controls[0].w,controls[0].z,a);
    a=a*controls[4].w+controls[8].x;
    if(flags.z&1024u)a=saturate(a);
    a*=i.fade.x;
    if(flags.z&0x80000u)a=saturate(a*128);
    // Some converted, scene-lit smoke keeps an additive output-alpha mode.
    // Its density still follows the sheet/particle alpha. Pure self-lit flash
    // and glow (no native lighting contribution) must never cast darkness.
    float extinction=abs(controls[1].w);
    if(extinction==0 && controls[0].y>0 && controls[0].x<1)extinction=1;
    a=saturate(a*extinction);
    float threshold=max(frac(52.9829189*frac(dot(floor(i.position.xy),float2(.06711056,.00583715)))),1.0/65536.0);
    clip(a-threshold);
}
)";
}
