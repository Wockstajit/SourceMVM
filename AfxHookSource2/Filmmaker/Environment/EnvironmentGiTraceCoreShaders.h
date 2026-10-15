// Shared cone traversal; callers supply volume bindings and grid constants.
R"HLSL(
float4 TraceRadiance(float3 origin,float3 direction,float aperture,float maxDistance) {
    float4 result=0;
    if(giInfo.x<.5)return result;
    float distance=giInfo.y*1.5;
    [loop]for(int step=0;step<32;++step) {
        float diameter=max(giInfo.y,distance*aperture);
        float3 uv=(origin+direction*distance-giLowSize.xyz)/giLowSize.w;
        if(any(uv<=0)||any(uv>=1)||distance>maxDistance||result.a>.98)break;
        float mip=clamp(log2(diameter/giInfo.y),0,6);
        float4 value=giRadiance.SampleLevel(giSampler,uv,mip);
        if(any(value.rgb>1e-5)){float4 face=giDirection.SampleLevel(giSampler,uv,mip);float3 normal=face.xyz;
            value.rgb*=lerp(saturate(dot(normal*rsqrt(max(dot(normal,normal),1e-8)),-direction)*4),1,saturate(face.w/max(value.a,1e-5)));}
        result+=(1-result.a)*value;
        distance+=max(giInfo.y,diameter*.65);
    }
    return result;
}
)HLSL"
