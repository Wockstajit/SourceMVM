#pragma once
R"HLSL(
// Filter shading normals only for the weather coating. The underlying lighting keeps its
// geometric normal, and same-sample depth/hard-corner rejection prevents silhouette halos.
float3 CoatingNormal(float2 pixel,float depth,uint sample,float3 centerNormal) {
    float3 center=ReceiverPosition(pixel,depth,sample);
    float worldPerPixel=2*length(center-camera.xyz)/max(1,dimensions.y*length(viewProjection[1].xyz));
    float radius=clamp(10/max(worldPerPixel,.001),1,10);
    float worldRadius=max(worldPerPixel*radius,1);
    return FilterSurfaceNormal(pixel,depth,sample,false,centerNormal,radius,worldRadius*3);
}
float CoatingDetailFade(float footprint,float frequency) { return 1-smoothstep(.25,1,footprint*frequency); }
float CoatingCoverage(float amount,float noise,float softness) {
    float threshold=1-(1+softness)*amount;
    return smoothstep(threshold,threshold+softness,noise)*saturate(amount*8);
}
)HLSL"
