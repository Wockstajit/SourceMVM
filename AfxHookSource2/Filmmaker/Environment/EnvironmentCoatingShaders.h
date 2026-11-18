#pragma once
R"HLSL(
// Filter shading normals only for the weather coating. The underlying lighting keeps its
// geometric normal, and same-sample depth/hard-corner rejection prevents silhouette halos.
// THE GROUND'S OWN FACING, seen at any angle. The shared reconstruction (SurfaceNormalAt) gives up
// when a pixel's neighbours lie far apart in the world and returns the direction to the eye. Ground
// seen at a grazing angle (down a slope, across a far yard) and treads seen from above (a pixel or
// two tall each) always have neighbours far apart, so snow was handed the eye's direction for a
// floor, found it too steep and left it bare, and came back as the eye moved.
// This one has no distance limit. Depth is affine across the screen on any plane, so for each axis
// it reads two pixels each way and takes the side whose depths run on in a straight line: that side
// is the same surface and the other one crosses an edge. (After J. Turanszki, "Improved normal
// reconstruction from depth", wickedengine.net 2019, and atyuwen, "Accurate Normal Reconstruction
// from Depth Buffer", atyuwen.github.io 2021.)
float3 CoatingBaseNormal(float2 pixel,float depth,uint sample,float3 center,float3 fallback){
    float3 tangent[2];
    [unroll]for(int a=0;a<2;++a){
        float2 s=a?float2(0,1):float2(1,0);
        float2 l1=pixel-s,l2=pixel-2*s,r1=pixel+s,r2=pixel+2*s;
        bool left=all(l2>.5)&&all(l2<dimensions.xy-.5),right=all(r2>.5)&&all(r2<dimensions.xy-.5);
        float dl1=left?ReadSampleDepth(l1,sample):0,dl2=left?ReadSampleDepth(l2,sample):0;
        float dr1=right?ReadSampleDepth(r1,sample):0,dr2=right?ReadSampleDepth(r2,sample):0;
        left=left&&!IsSky(dl1)&&!IsSky(dl2);right=right&&!IsSky(dr1)&&!IsSky(dr2);
        if(!left&&!right)return fallback;
        float el=left?abs(2*dl1-dl2-depth):1e30,er=right?abs(2*dr1-dr2-depth):1e30;
        tangent[a]=el<er?center-World(l1,dl1):World(r1,dr1)-center;
    }
    float3 n=cross(tangent[0],tangent[1]);
    if(dot(n,n)<1e-12)return fallback;
    n=normalize(n);
    return dot(n,camera.xyz-center)<0?-n:n;
}
float3 CoatingNormal(float2 pixel,float depth,uint sample,float3 centerNormal) {
    float3 center=ReceiverPosition(pixel,depth,sample);
    float worldPerPixel=2*length(center-camera.xyz)/max(1,dimensions.y*length(viewProjection[1].xyz));
    float radius=clamp(10/max(worldPerPixel,.001),1,10);
    float worldRadius=max(worldPerPixel*radius,1);
    // The hands and weapon keep the shared normal (no coating lies on them).
    float3 base=IsViewmodelSample(pixel,sample)?centerNormal:CoatingBaseNormal(pixel,depth,sample,center,centerNormal);
    return FilterSurfaceNormal(pixel,depth,sample,false,base,radius,worldRadius*3);
}
float CoatingDetailFade(float footprint,float frequency) { return 1-smoothstep(.25,1,footprint*frequency); }
float CoatingCoverage(float amount,float noise,float softness) {
    float threshold=1-(1+softness)*amount;
    return smoothstep(threshold,threshold+softness,noise)*saturate(amount*8);
}
)HLSL"
