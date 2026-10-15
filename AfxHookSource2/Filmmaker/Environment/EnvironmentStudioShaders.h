#pragma once
R"HLSL(
// Both the surface renderer and GI injection use the same emitter/profile/projector evaluation.
int StudioSamples(int l) { return lightStudio[l].x>.5?4:1; }
float3 StudioPosition(int l,int sample) {
    if(lightStudio[l].x<.5)return lightPositionRange[l].xyz;
    return lightPositionRange[l].xyz + lightRightWidth[l].xyz*lightRightWidth[l].w*((sample&1)? .288675135:-.288675135)
        +lightUpHeight[l].xyz*lightUpHeight[l].w*((sample&2)? .288675135:-.288675135);
}
float StudioPattern(int l,float3 p) {
    float3 delta=p-lightPositionRange[l].xyz;
    float forward=dot(delta,lightDirectionCone[l].xyz);
    if(lightStudio[l].x>.5&&forward<=0)return 0;
    int pattern=(int)lightStudio[l].y,profile=(int)lightStudio[l].z;
    if(pattern==0&&profile==0)return 1;
    if(forward<=.001)return 0;
    float tangent=sqrt(max(0,1-lightDirectionCone[l].w*lightDirectionCone[l].w))/max(.01,lightDirectionCone[l].w);
    float2 uv=float2(dot(delta,lightRightWidth[l].xyz),dot(delta,lightUpHeight[l].xyz))/max(1,forward*tangent);
    float2 q=uv*lightStudio[l].w;
    float mask=1;
    if(pattern==1){float2 edge=abs(frac(q*.5+.5)-.5);mask=smoothstep(.04,.075,min(edge.x,edge.y));}
    if(pattern==2)mask=smoothstep(.15,.23,abs(frac(q.y*.5+.5)-.5));
    if(pattern==3){float leaf=sin(q.x*2.3+sin(q.y*1.7))*cos(q.y*3.1+sin(q.x));mask=smoothstep(-.25,.35,leaf);}
    if(pattern==4){float caustic=abs(sin(q.x*2+lightPatternTime[l].x)+sin(q.y*2.7-lightPatternTime[l].x*.8));mask=.1+.9*pow(saturate(1-caustic),6);}
    float cosine=saturate(forward/max(length(delta),.001));
    if(profile==1)mask*=pow(cosine,4);
    if(profile==2)mask*=pow(saturate(1-abs(cosine-.75)*4),2);
    if(profile==3)mask*=pow(saturate(1-abs(uv.y)),4)*saturate(1-abs(uv.x)*.25);
    return mask;
}
float StudioAttenuation(int l,int sample,float3 p,out float3 dir) {
    float3 delta=StudioPosition(l,sample)-p;
    float distance=length(delta);dir=delta/max(distance,.001);
    float attenuation=saturate(1-distance/lightPositionRange[l].w);attenuation*=attenuation;
    float cone=lightDirectionCone[l].w;
    if(cone>-1)attenuation*=smoothstep(cone,min(.9999,cone+.12),dot(-dir,lightDirectionCone[l].xyz));
    if(lightStudio[l].x>.5)attenuation*=saturate(dot(-dir,lightDirectionCone[l].xyz));
    return attenuation*StudioPattern(l,p)/StudioSamples(l);
}
)HLSL"
