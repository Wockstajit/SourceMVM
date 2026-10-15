// Shared by LDR/HDR and MSAA variants. Geometry and pulse timing are evaluated on the CPU.
R"HLSL(
float3 StormCloudLight(float3 p) {
    float3 delta=p-stormSource.xyz;
    return float3(.66,.78,1)*stormSource.w*4*exp(-dot(delta,delta)/max(1,CL_cloudDepth*CL_cloudDepth*1.2));
}
struct StormVertex {float4 position:SV_Position;float2 uv:TEXCOORD0;float3 world:TEXCOORD1;float2 shape:TEXCOORD2;};
StormVertex StormVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID){
    int id=(int)instance+1,parent=(int)stormLinks[id].x;
    float3 a=stormPoints[parent].xyz,b=stormPoints[id].xyz;
    float2 corners[6]={float2(-1,0),float2(-1,1),float2(1,1),float2(-1,0),float2(1,1),float2(1,0)};
    float2 uv=corners[vertex];float3 p=lerp(a,b,uv.y);
    float3 side=cross(b-a,camera.xyz-p);float side2=dot(side,side);
    side=side2>1e-8?side*rsqrt(side2):cameraRight.xyz;
    float physical=lerp(stormPoints[parent].w,stormPoints[id].w,uv.y);
    float footprint=2*length(p-camera.xyz)/max(dimensions.y*length(viewProjection[1].xyz),1);
    // The core never drops below about a pixel; the quad reaches 12 core widths for the glow.
    float width=max(physical,footprint*1.1);
    StormVertex o;o.world=p+side*uv.x*width*12;o.position=mul(viewProjection,float4(o.world,1));
    o.uv=uv;o.shape=float2(physical/width,stormLinks[id].y);
    if(stormPoints[id].w<=0||stormSource.w<=0)o.position=float4(2,2,2,1);
    return o;
}
float4 StormPS(StormVertex i,uint sample:SV_SampleIndex):SV_Target{
    if(Hidden(i.position.z,ReadCombinedSample(i.position.xy,sample)))discard;
    float distance=length(i.world-camera.xyz);
    float extinction=exp(-max(0,i.world.z-CL_cloudBase)*CL_clouds*CL_density*.002
        -min(distance,CL_fogRange)*CL_fogDensity*.0002);
    // u is measured in core widths: a hot white core, then a wide ionised glow that is what
    // makes a bolt read against a bright daytime sky.
    float u=i.uv.x*12;
    float core=exp(-u*u*1.2)*saturate(i.shape.x*1.5+.35);
    float halo=exp(-abs(u)*.42)*.38;
    float visible=(1-SmokeHides(i.position.xy,i.world))*extinction*i.shape.y;
    float alpha=saturate((core+halo)*visible);
    float3 tint=lerp(float3(.62,.74,1),float3(1,1,1),saturate(core/(core+halo+1e-4)));
    return float4(tint*(1+stormSource.w)*4*tintExposure.w,alpha);
}
)HLSL"
