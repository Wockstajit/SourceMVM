// Shared conservative dominant-axis voxel rasterization.
R"HLSL(
struct VoxelVertex { float3 world:POSITION; };
VoxelVertex VoxelVS(float3 p:POSITION) { VoxelVertex o;o.world=mul(model,float4(p,1)).xyz;return o; }
struct VoxelPixel { float4 position:SV_Position; float3 world:TEXCOORD0; nointerpolation float3 normal:TEXCOORD1; };
[maxvertexcount(3)]
void VoxelGS(triangle VoxelVertex input[3],inout TriangleStream<VoxelPixel> output) {
    float3 n=cross(input[1].world-input[0].world,input[2].world-input[0].world);
    if(dot(n,n)<1e-12)return;
    n=normalize(n);float3 a=abs(n);int axis=a.x>=a.y&&a.x>=a.z?0:a.y>=a.z?1:2;
    [unroll]for(int v=0;v<3;++v){
        float3 uv=(input[v].world-gridLowSize.xyz)/gridLowSize.w;
        float2 plane=axis==0?uv.yz:axis==1?uv.xz:uv.xy;
        VoxelPixel o;o.position=float4(plane*float2(2,-2)+float2(-1,1),.5,1);o.world=input[v].world;o.normal=n;output.Append(o);
    }
}
void VoxelPS(VoxelPixel i) {
    int3 cell=(int3)floor((i.world-gridLowSize.xyz)/gridInfo.y);
    if(any(cell<0)||any(cell>=(int)gridInfo.x))return;
    uint3 n=(uint3)round(saturate(i.normal*.5+.5)*63);
    uint3 c=(uint3)round(saturate(albedo.rgb)*15);
    uint packed=0x80000000u|n.x|(n.y<<6)|(n.z<<12)|(c.x<<18)|(c.y<<22)|(c.z<<26);
    InterlockedMax(voxelOutput[cell],packed);
    uint3 emissionColor=(uint3)round(saturate(albedo.rgb)*255);
    uint emission=(uint)round(saturate(albedo.w/16)*255);
    if(emission>0)InterlockedMax(emissionOutput[cell],emissionColor.x|(emissionColor.y<<8)|(emissionColor.z<<16)|(emission<<24));
}
)HLSL"
