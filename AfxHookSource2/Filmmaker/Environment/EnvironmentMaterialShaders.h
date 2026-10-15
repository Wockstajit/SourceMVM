#pragma once
R"HLSL(
Texture2D<float4> materialAlbedo:register(t10);
Texture2D<float4> materialNormal:register(t11);
Texture2D<float4> materialRoughness:register(t12);
SamplerState materialSampler:register(s4);
float3 MaterialFresnel(float3 f0,float cosine){return f0+(1-f0)*pow(1-saturate(cosine),5);}
float MaterialD(float3 h,float3 n,float3 tangent,float3 bitangent,float roughness,float anisotropy){
    float alpha=max(.0025,roughness*roughness),aspect=sqrt(1-.9*abs(anisotropy));
    float ax=alpha/aspect,ay=alpha*aspect;if(anisotropy<0){float swap=ax;ax=ay;ay=swap;}
    float x=dot(h,tangent)/ax,y=dot(h,bitangent)/ay,z=dot(h,n);
    float d=x*x+y*y+z*z;return 1/max(1e-10,3.14159265*ax*ay*d*d);
}
float MaterialLambda(float3 v,float3 n,float3 t,float3 b,float roughness,float anisotropy){
    float alpha=max(.0025,roughness*roughness),aspect=sqrt(1-.9*abs(anisotropy));
    float ax=alpha/aspect,ay=alpha*aspect;if(anisotropy<0){float swap=ax;ax=ay;ay=swap;}
    float nv=max(.0001,abs(dot(n,v)));float x=dot(t,v)*ax,y=dot(b,v)*ay;
    return .5*(sqrt(1+(x*x+y*y)/(nv*nv))-1);
}
float3 MaterialBrdf(float3 base,float3 n,float3 t,float3 b,float3 v,float3 l,float roughness){
    float nl=saturate(dot(n,l)),nv=max(.0001,saturate(dot(n,v)));if(nl<=0)return 0;
    float3 h=normalize(v+l+float3(1e-8,0,0));float3 f=MaterialFresnel(lerp(.04,base,materialParams.y),dot(v,h));
    float d=MaterialD(h,n,t,b,roughness,materialCoat.z);
    float g=1/(1+MaterialLambda(v,n,t,b,roughness,materialCoat.z)+MaterialLambda(l,n,t,b,roughness,materialCoat.z));
    float3 result=base*(1-materialParams.y)*(1-f)*nl+f*(d*g*3.14159265/(4*nv));
    float coatF=MaterialFresnel(float3(.04,.04,.04),dot(v,h)).x;
    float coatD=MaterialD(h,n,t,b,materialCoat.y,0);
    float coatG=1/(1+MaterialLambda(v,n,t,b,materialCoat.y,0)+MaterialLambda(l,n,t,b,materialCoat.y,0));
    return result*(1-materialCoat.x*coatF)+materialCoat.x*coatF*coatD*coatG*3.14159265/(4*nv);
}
void MaterialFrame(float3 world,float3 geometric,float2 uv,out float3 n,out float3 t,out float3 b){
    float3 dx=ddx(world),dy=ddy(world);float2 tx=ddx(uv),ty=ddy(uv);
    float determinant=tx.x*ty.y-tx.y*ty.x;
    t=abs(determinant)>1e-10?(dx*ty.y-dy*tx.y)/determinant:cross(abs(geometric.z)<.9?float3(0,0,1):float3(0,1,0),geometric);
    t-=geometric*dot(t,geometric);t=normalize(t+float3(1e-9,0,0));b=normalize(cross(geometric,t));
    if(abs(determinant)>1e-10&&dot(b,(dy*tx.x-dx*ty.x)/determinant)<0)b=-b;
    n=geometric;
    if(materialTextures.y>.5){float3 detail=materialNormal.Sample(materialSampler,uv).xyz*2-1;detail.xy*=materialCoat.w;detail.y=-detail.y;
        float handed=dot(cross(geometric,t),b)<0?-1:1;n=normalize(t*detail.x+b*detail.y+geometric*max(.01,detail.z));t=normalize(t-n*dot(t,n));b=normalize(cross(n,t))*handed;}
}
float3 ShadeMaterial(float3 world,float3 geometric,float2 uv){
    uv*=materialTextures.w;float3 base=objectColor.rgb;
    if(materialTextures.x>.5)base*=materialAlbedo.Sample(materialSampler,uv).rgb;
    float roughness=materialParams.x;
    if(materialTextures.z>.5)roughness*=materialRoughness.Sample(materialSampler,uv).r;
    roughness=clamp(roughness,.05,1);
    float3 n,t,b;MaterialFrame(world,normalize(geometric),uv,n,t,b);float3 v=normalize(camera.xyz-world);
    float3 color=base*(1-materialParams.y)*(lighting.y*.2+CL_ambient+WorldBounce(world,n))+base*materialParams.z;
    float3 sun=SunDirection();float visible=CL_contactShadows>0?WorldSunShadowSoft(world+n*shadowInfo.y,n):1;
    color+=MaterialBrdf(base,n,t,b,v,sun,roughness)*SunColor()*CL_sunRelight*visible;
    [loop]for(int l=0;l<(int)lighting.x;++l)[loop]for(int sample=0;sample<StudioSamples(l);++sample){
        float3 dir;float attenuation=StudioAttenuation(l,sample,world,dir);
        if(attenuation>0)color+=MaterialBrdf(base,n,t,b,v,dir,roughness)*lightColorIntensity[l].rgb*lightColorIntensity[l].w*attenuation*WorldLightShadowSample(l,world+n*shadowInfo.y,sample);
    }
    float3 ray=reflect(-v,n);float3 reflected=WorldReflectionRoughness(world,n,ray,SkyColor(ray),roughness);
    color+=reflected*MaterialFresnel(lerp(.04,base,materialParams.y),dot(n,v))*(1-roughness*.75);
    return ApplyCoatings(color,world,n,geometric,dot(max(color,0),float3(.2126,.7152,.0722)));
}
)HLSL"
