#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Relight/RelightIrradianceMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeMaterialMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeAtmosphereMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Relight;
void Check(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
ComPtr<ID3DBlob> Compile(const char* source,const char* profile){
    ComPtr<ID3DBlob> code,error;
    auto hr=Filmmaker::Environment::CompileEnvironmentShader(source,strlen(source),"DynamicIrradianceTest",nullptr,
        "Main",profile,D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
    if(error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(hr),"compile");return code;
}
ComPtr<ID3D11VertexShader> FixtureVertexShader(ID3D11Device* device,const std::vector<uint8_t>& bytes){
    using Reflect=HRESULT(WINAPI*)(LPCVOID,SIZE_T,REFIID,void**);
    const auto reflect=reinterpret_cast<Reflect>(GetProcAddress(Filmmaker::Environment::EnvironmentShaderCompiler(),"D3DReflect"));
    ComPtr<ID3D11ShaderReflection> reflection;
    Check(reflect&&SUCCEEDED(reflect(bytes.data(),bytes.size(),IID_ID3D11ShaderReflection,reinterpret_cast<void**>(reflection.GetAddressOf()))),"reflect native inputs");
    D3D11_SHADER_DESC desc{};Check(SUCCEEDED(reflection->GetDesc(&desc)),"native signature");
    std::ostringstream fields,values;
    for(unsigned i=0;i<desc.InputParameters;++i){
        D3D11_SIGNATURE_PARAMETER_DESC input{};Check(SUCCEEDED(reflection->GetInputParameterDesc(i,&input)),"native parameter");
        if(input.SystemValueType==D3D_NAME_POSITION)continue;
        // The rasterizer supplies these values, rather than a linked vertex output.
        if(input.SystemValueType==D3D_NAME_IS_FRONT_FACE||input.SystemValueType==D3D_NAME_SAMPLE_INDEX||input.SystemValueType==D3D_NAME_PRIMITIVE_ID)continue;
        if(!input.ReadWriteMask)continue;
        unsigned components=4;while(components>1&&!(input.Mask&(1u<<(components-1))))--components;
        std::string type=input.ComponentType==D3D_REGISTER_COMPONENT_UINT32?"uint":input.ComponentType==D3D_REGISTER_COMPONENT_SINT32?"int":"float";
        if(components>1)type+=std::to_string(components);
        fields<<(input.ComponentType==D3D_REGISTER_COMPONENT_FLOAT32?"":"nointerpolation ")<<type<<" a"<<i<<":"<<input.SemanticName<<input.SemanticIndex<<";";
        values<<"v.a"<<i<<"="<<type<<"(";
        for(unsigned component=0;component<components;++component)values<<(component?",":"")<<(component<2?"0":"1");
        values<<");";
    }
    const auto source=std::string("struct V{float4 p:SV_Position;")+fields.str()+"}; V Main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)}; V v;v.p=float4(p[id],.5,1);"+values.str()+"return v;}";
    const auto code=Compile(source.c_str(),"vs_5_0");ComPtr<ID3D11VertexShader> shader;
    Check(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader)),"native linked vertex shader");return shader;
}
int main(int argc,char** argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    bool hardware=false,original=false,warm=true,constantSamples=false,serialized=false,parallelDriver=false;std::string fixture;unsigned cold=0;
    for(int i=1;i<argc;++i){const std::string option=argv[i];if(option=="--hardware")hardware=true;
        else if(option=="--original")original=true;else if(option=="--no-warm")warm=false;else if(option=="--fixture"&&i+1<argc)fixture=argv[++i];
        else if(option=="--constant-samples")constantSamples=true;
        else if(option=="--serialized")serialized=true;
        else if(option=="--parallel-driver")parallelDriver=true;
        else if(option=="--cold"&&i+1<argc)cold=unsigned(std::stoul(argv[++i]));else Check(false,"unknown option");}
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,
        (serialized||(hardware&&!parallelDriver))?D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS:0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"D3D11 device");
    if(hardware)std::cout<<"Hardware device creation flags="<<device->GetCreationFlags()<<std::endl;
    const auto source=Compile(R"(
        cbuffer View:register(b4){float4 view[36];}
        Texture2DArray<float4> baked:register(t2);Texture2DArray<float4> bent:register(t3);SamplerState s:register(s0);
        float4 Main(float4 screen:SV_Position,float3 world:TEXCOORD0,float3 normal:TEXCOORD1,float2 uv:TEXCOORD2):SV_Target{
            float3 p=world+view[35].xyz;
            float3 light=baked.Sample(s,float3(uv,0)).rgb;
            float3 occlusion=bent.Sample(s,float3(uv,0)).rgb;
            return float4(light*occlusion.z*float3(.5,.25,.125)+p*.000001+normal*.000001+screen.x*.000000001,1);
        })","ps_5_0");
    std::vector<uint8_t> patched;IrradianceProgramInfo info;
    if(!DynamicIrradianceProgram(source->GetBufferPointer(),source->GetBufferSize(),patched,&info)){
        auto disassemble=reinterpret_cast<pD3DDisassemble>(GetProcAddress(Filmmaker::Environment::EnvironmentShaderCompiler(),"D3DDisassemble"));
        ComPtr<ID3DBlob> listing;if(disassemble&&SUCCEEDED(disassemble(source->GetBufferPointer(),source->GetBufferSize(),0,nullptr,&listing)))std::cerr<<static_cast<const char*>(listing->GetBufferPointer());
        Check(false,"replace irradiance query");
    }
    Check(info.queries==1,"one query");
    ComPtr<ID3D11PixelShader> ps;
    const auto hr=device->CreatePixelShader(patched.data(),patched.size(),nullptr,&ps);
    if(FAILED(hr))std::cerr<<"CreatePixelShader: "<<std::hex<<hr<<'\n';
    Check(SUCCEEDED(hr),"patched shader validates");
    const auto vsCode=Compile(R"(
        struct V{float4 p:SV_Position;float3 world:TEXCOORD0;float3 normal:TEXCOORD1;float2 uv:TEXCOORD2;};
        V Main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};
            V v;v.p=float4(p[id],.5,1);v.world=float3(0,0,0);v.normal=float3(0,0,1);v.uv=p[id]*.5+.5;return v;})","vs_5_0");
    ComPtr<ID3D11VertexShader> vs;Check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)),"vertex shader");
    auto buffer=[&](const void* p,UINT bytes){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{p,0,0};ComPtr<ID3D11Buffer> b;Check(SUCCEEDED(device->CreateBuffer(&d,&initial,&b)),"constants");return b;};
    float view[36*4]{};auto b4=buffer(view,sizeof(view));
    float pack[40]{1,1,1,0,.5f,.5f,.5f,0,.5f,.5f,.5f,0,.5f,.5f,.5f,0};auto b13=buffer(pack,sizeof(pack));
    float bands[24]{};for(unsigned i=0;i<6;++i){bands[i*4]=i==2?2.f:0;bands[i*4+1]=i==2?4.f:0;bands[i*4+2]=i==2?8.f:0;}
    D3D11_TEXTURE3D_DESC volume{};volume.Width=volume.Height=volume.MipLevels=1;volume.Depth=6;
    volume.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;volume.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{bands,16,16};ComPtr<ID3D11Texture3D> texture;ComPtr<ID3D11ShaderResourceView> srv;
    Check(SUCCEEDED(device->CreateTexture3D(&volume,&initial,&texture))&&SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&srv)),"live irradiance volume");
    // Production receiver queries gather exact cage/cache texels and interpolate
    // their weights themselves. Exercise that point-sampled binding throughout.
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sample;Check(SUCCEEDED(device->CreateSamplerState(&sampler,&sample)),"sampler");
    D3D11_TEXTURE2D_DESC target{};target.Width=target.Height=4;target.MipLevels=target.ArraySize=target.SampleDesc.Count=1;
    target.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;target.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color,read;ComPtr<ID3D11RenderTargetView> rtv;
    Check(SUCCEEDED(device->CreateTexture2D(&target,nullptr,&color))&&SUCCEEDED(device->CreateRenderTargetView(color.Get(),nullptr,&rtv)),"target");
    target.BindFlags=0;target.Usage=D3D11_USAGE_STAGING;target.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(SUCCEEDED(device->CreateTexture2D(&target,nullptr,&read)),"readback");
    auto cb=b4.Get();ctx->PSSetConstantBuffers(4,1,&cb);cb=b13.Get();ctx->PSSetConstantBuffers(13,1,&cb);
    auto resource=srv.Get();ctx->PSSetShaderResources(IrradianceTextureSlot,1,&resource);ctx->PSSetShaderResources(IrradianceWorldSlot,1,&resource);auto ss=sample.Get();ctx->PSSetSamplers(15,1,&ss);
    ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport{0,0,4,4,0,1};ctx->RSSetViewports(1,&viewport);auto rt=rtv.Get();ctx->OMSetRenderTargets(1,&rt,nullptr);
    auto draw=[&](unsigned row=1,unsigned vertices=3){ctx->Draw(vertices,0);ctx->CopyResource(read.Get(),color.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)),"map");std::array<float,4> result;
        std::memcpy(result.data(),static_cast<char*>(mapped.pData)+mapped.RowPitch*row+16,16);ctx->Unmap(read.Get(),0);return result;};
    auto lit=draw();std::cout<<"lit="<<lit[0]<<","<<lit[1]<<","<<lit[2]<<","<<lit[3]<<'\n';for(unsigned i=0;i<3;++i)Check(std::abs(lit[i]-1)<.0001f,"world-space lighting preserves material albedo, bypasses baked AO");
    // Multiple native queries read live irradiance even when the first query
    // is skipped by a branch. Distinct array slices keep both native samples.
    const auto repeatedCode=Compile(R"(
        cbuffer View:register(b4){float4 view[36];}
        Texture2DArray<float4> baked:register(t2);SamplerState s:register(s0);
        float4 Main(float4 screen:SV_Position,float3 world:TEXCOORD0,float3 normal:TEXCOORD1,float2 uv:TEXCOORD2):SV_Target{
            float3 a=0;if(view[0].x>0)a=baked.Sample(s,float3(uv,0)).bgr;
            float3 b=baked.Sample(s,float3(uv,1)).rgb;
            return float4((a+b)*float3(.5,.25,.125)+(world+view[35].xyz)*.000001+normal*.000001+screen.x*.000000001,1);
        })","ps_5_0");
    std::vector<uint8_t> repeated;IrradianceProgramInfo repeatedInfo;
    Check(DynamicIrradianceProgram(repeatedCode->GetBufferPointer(),repeatedCode->GetBufferSize(),repeated,&repeatedInfo)&&repeatedInfo.queries==2,"two branch-dependent native queries");
    Check(repeatedInfo.evaluations==1,"invariant queries share one irradiance evaluation");
    ComPtr<ID3D11PixelShader> repeatedPs;
    Check(SUCCEEDED(device->CreatePixelShader(repeated.data(),repeated.size(),nullptr,&repeatedPs)),"repeated shader validates");
    ctx->PSSetShader(repeatedPs.Get(),nullptr,0);
    const auto skippedFirst=draw();for(unsigned c=0;c<3;++c)Check(std::abs(skippedFirst[c]-1)<.0001f,"later query works when first branch is skipped");
    view[0]=1;ctx->UpdateSubresource(b4.Get(),0,nullptr,view,0,0);
    const float repeatedExpected[]{5,2,1.25f};
    const auto bothQueries=draw();for(unsigned c=0;c<3;++c)Check(std::abs(bothQueries[c]-repeatedExpected[c])<.0001f,"shared query preserves native swizzles and contributions");
    view[0]=0;ctx->UpdateSubresource(b4.Get(),0,nullptr,view,0,0);
    // Reconstructed programs (first-person, decals) derive position and normal in
    // temporaries. Every site reads the same snapshot, so one evaluation serves all.
    auto loops=[](const std::vector<uint8_t>& program){
        uint32_t chunks;std::memcpy(&chunks,program.data()+28,4);unsigned count=0;
        for(unsigned chunk=0;chunk<chunks;++chunk){uint32_t start,size;std::memcpy(&start,program.data()+32+chunk*4,4);std::memcpy(&size,program.data()+start+4,4);
            if(std::memcmp(program.data()+start,"SHEX",4)&&std::memcmp(program.data()+start,"SHDR",4))continue;
            IrradianceDxbc::Words code(size/4);std::memcpy(code.data(),program.data()+start+8,size);
            for(size_t i=2;i<code.size();){const unsigned op=code[i]&2047;if(op==48)++count;i+=op==53?code[i+1]:(code[i]>>24)&127;}}
        return count;};
    const auto reconstructedVs=Compile(R"(
        struct V{float4 p:SV_Position;float4 ray:TEXCOORD4;float4 basis:TEXCOORD5;float2 uv:TEXCOORD2;};
        V Main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};
            V v;v.p=float4(p[id],.5,1);v.ray=float4(0,0,0,1);v.basis=float4(0,0,1,0);v.uv=p[id]*.5+.5;return v;})","vs_5_0");
    ComPtr<ID3D11VertexShader> reconstructedVertex;
    Check(SUCCEEDED(device->CreateVertexShader(reconstructedVs->GetBufferPointer(),reconstructedVs->GetBufferSize(),nullptr,&reconstructedVertex)),"reconstructed vertex shader");
    std::vector<float> lightingRows(1990*4,0.f);auto b3=buffer(lightingRows.data(),UINT(lightingRows.size()*4));
    cb=b3.Get();ctx->PSSetConstantBuffers(3,1,&cb);ctx->VSSetShader(reconstructedVertex.Get(),nullptr,0);
    auto reconstructed=[&](const char* first,const char* name,unsigned expectLoops,const float* expected){
        const auto code=Compile((std::string(R"(
            cbuffer Lighting:register(b3){float4 lighting[1990];}
            cbuffer View:register(b4){float4 view[36];}
            Texture2DArray<float4> baked:register(t2);SamplerState s:register(s0);
            float4 Main(float4 screen:SV_Position,float4 ray:TEXCOORD4,float4 basis:TEXCOORD5,float2 uv:TEXCOORD2):SV_Target{
                float3 p=ray.xyz-view[35].xyz;
                float3 n=normalize(basis.xyz);
                float2 at=uv+(n.xy+p.xy+p.zz)*.000001;
                float3 a=0;)")+first+R"(
                float3 b=baked.Sample(s,float3(at,1)).bgr;
                float lit=0;[branch]if(b.y>.5)lit=saturate(dot(n,lighting[1945].xyz));
                return float4((a+b)*float3(.5,.25,.125)+lit*.000001+screen.x*.000000001,1);
            })").c_str(),"ps_5_0");
        std::vector<uint8_t> program;IrradianceProgramInfo details;
        if(!DynamicIrradianceProgram(code->GetBufferPointer(),code->GetBufferSize(),program,&details)){
            auto disassemble=reinterpret_cast<pD3DDisassemble>(GetProcAddress(Filmmaker::Environment::EnvironmentShaderCompiler(),"D3DDisassemble"));
            ComPtr<ID3DBlob> listing;if(disassemble&&SUCCEEDED(disassemble(code->GetBufferPointer(),code->GetBufferSize(),0,nullptr,&listing)))std::cerr<<static_cast<const char*>(listing->GetBufferPointer());
            Check(false,name);
        }
        Check(details.position==~0u&&details.queries==2&&details.evaluations==1,"reconstructed sites share one irradiance evaluation");
        Check(loops(program)==expectLoops,"reconstructed program carries the expected world-query copies");
        ComPtr<ID3D11PixelShader> shader;Check(SUCCEEDED(device->CreatePixelShader(program.data(),program.size(),nullptr,&shader)),"reconstructed shader validates");
        ctx->PSSetShader(shader.Get(),nullptr,0);
        const auto result=draw();for(unsigned c=0;c<3;++c)Check(std::abs(result[c]-expected[c])<.0001f,name);
    };
    // Straight-line sites: one query at the first site, plain copies afterwards.
    reconstructed("a=baked.Sample(s,float3(at,0)).rgb;","straight-line reconstructed sites keep native swizzles",1,repeatedExpected);
    // A branch-skipped first site leaves the evaluation to the later one.
    const float laterOnly[]{4,1,.25f};
    reconstructed("[branch]if(view[1].x>0)a=baked.Sample(s,float3(at,0)).rgb;","guarded reconstructed query survives a skipped first site",2,laterOnly);
    view[4]=1;ctx->UpdateSubresource(b4.Get(),0,nullptr,view,0,0);
    reconstructed("[branch]if(view[1].x>0)a=baked.Sample(s,float3(at,0)).rgb;","guarded reconstructed query is reused by the later site",2,repeatedExpected);
    view[4]=0;ctx->UpdateSubresource(b4.Get(),0,nullptr,view,0,0);
    ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);
    for(float& value:bands)value=0;ctx->UpdateSubresource(texture.Get(),0,nullptr,bands,16,16);
    auto dark=draw();for(unsigned i=0;i<3;++i)Check(std::abs(dark[i])<.0001f,"zero live illumination has no baked fill");
    // Exercise the visibility-aware cage, rather than just validating its bytecode.
    float distances[24]{};for(unsigned i=0;i<6;++i){distances[i*4]=8;distances[i*4+1]=64;distances[i*4+2]=1;}
    D3D11_SUBRESOURCE_DATA distanceData{distances,16,16};ComPtr<ID3D11Texture3D> distanceTexture;ComPtr<ID3D11ShaderResourceView> distanceView;
    Check(SUCCEEDED(device->CreateTexture3D(&volume,&distanceData,&distanceTexture))&&SUCCEEDED(device->CreateShaderResourceView(distanceTexture.Get(),nullptr,&distanceView)),"DDGI distance moments");
    auto distanceSrv=distanceView.Get();ctx->PSSetShaderResources(IrradianceDistanceSlot,1,&distanceSrv);
    pack[15]=1;ctx->UpdateSubresource(b13.Get(),0,nullptr,pack,0,0);
    for(unsigned i=0;i<6;++i){bands[i*4]=i==2?2.f:0;bands[i*4+1]=i==2?4.f:0;bands[i*4+2]=i==2?8.f:0;}
    ctx->UpdateSubresource(texture.Get(),0,nullptr,bands,16,16);
    auto visible=draw();for(unsigned i=0;i<3;++i)Check(std::abs(visible[i]-1)<.0001f,"visible DDGI probes preserve uniform irradiance and albedo");
    ctx->PSSetShader(repeatedPs.Get(),nullptr,0);
    const auto skippedDdgi=draw();for(unsigned c=0;c<3;++c)Check(std::abs(skippedDdgi[c]-1)<.0001f,"shared DDGI works when first native branch is skipped");
    view[0]=1;ctx->UpdateSubresource(b4.Get(),0,nullptr,view,0,0);
    const auto repeatedDdgi=draw();for(unsigned c=0;c<3;++c)Check(std::abs(repeatedDdgi[c]-repeatedExpected[c])<.0001f,"shared DDGI preserves branch-dependent channel contributions");
    view[0]=0;ctx->UpdateSubresource(b4.Get(),0,nullptr,view,0,0);ctx->PSSetShader(ps.Get(),nullptr,0);
    // Execute the injected WorldQuery with a dead row at the receiver's exact
    // probe plane. The screen compute path has the same fixture in its own test.
    D3D11_TEXTURE3D_DESC rowVolume=volume;rowVolume.Height=64;
    std::array<float,64*6*4> rowMoments{};
    for(unsigned band=0;band<6;++band)for(unsigned y=0;y<64;++y){
        auto* m=rowMoments.data()+(band*64+y)*4;
        m[0]=y==31?0.f:8.f;m[1]=y==31?0.f:64.f;m[2]=1;
    }
    D3D11_SUBRESOURCE_DATA rowData{rowMoments.data(),16,64*16};
    ComPtr<ID3D11Texture3D> rowTexture;ComPtr<ID3D11ShaderResourceView> rowView;
    Check(SUCCEEDED(device->CreateTexture3D(&rowVolume,&rowData,&rowTexture))&&
        SUCCEEDED(device->CreateShaderResourceView(rowTexture.Get(),nullptr,&rowView)),"dead-row distance fixture");
    auto rowSrv=rowView.Get();ctx->PSSetShaderResources(IrradianceDistanceSlot,1,&rowSrv);
    pack[9]=.5f/64;pack[13]=63.5f/64;
    for(float offset:{-1e-6f,0.f,1e-6f}){
        pack[5]=31.5f/64+offset;ctx->UpdateSubresource(b13.Get(),0,nullptr,pack,0,0);
        const auto supported=draw();
        for(unsigned c=0;c<3;++c)Check(std::abs(supported[c]-1)<.01f,"Material receiver stays lit on both sides of a dead probe-row plane");
    }
    D3D11_TEXTURE3D_DESC cageVolume=volume;cageVolume.Width=cageVolume.Height=64;cageVolume.Depth=64*6;
    std::vector<std::array<float,4>> cageMoments(size_t(64)*64*64*6);
    for(unsigned z=0;z<64*6;++z)for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
        cageMoments[(size_t(z)*64+y)*64+x]=(x==31||y==31||z%64==31)?
            std::array<float,4>{0,0,1,1.5f}:std::array<float,4>{8,64,1,0};
    D3D11_SUBRESOURCE_DATA cageData{cageMoments.data(),64*16,64*64*16};
    ComPtr<ID3D11Texture3D> cageTexture;ComPtr<ID3D11ShaderResourceView> cageView;
    Check(SUCCEEDED(device->CreateTexture3D(&cageVolume,&cageData,&cageTexture))&&
        SUCCEEDED(device->CreateShaderResourceView(cageTexture.Get(),nullptr,&cageView)),"intersecting dead-row distance fixture");
    auto cageSrv=cageView.Get();ctx->PSSetShaderResources(IrradianceDistanceSlot,1,&cageSrv);
    pack[4]=pack[5]=31.5f/64;pack[6]=31.15f/64;
    for(unsigned axis=0;axis<3;++axis){pack[8+axis]=.5f/64;pack[12+axis]=63.5f/64;}
    ctx->UpdateSubresource(b13.Get(),0,nullptr,pack,0,0);
    const auto intersection=draw();
    for(unsigned c=0;c<3;++c)Check(std::abs(intersection[c]-1)<.01f,"Material query retains the visible corner at three dead-row planes");
    // Varying probe colors detect nonzero corner swaps at cage boundaries.
    D3D11_TEXTURE3D_DESC varyingVolume=volume;varyingVolume.Width=64;
    std::array<std::array<float,4>,64*6> varyingBands{};
    for(unsigned b=0;b<6;++b)for(unsigned x=0;x<64;++x)
        varyingBands[b*64+x]={b==2?2.f*(x%3+1):0, b==2?4.f:0, b==2?8.f:0,1};
    D3D11_SUBRESOURCE_DATA varyingData{varyingBands.data(),64*16,64*16};
    ComPtr<ID3D11Texture3D> varyingTexture;ComPtr<ID3D11ShaderResourceView> varyingView;
    Check(SUCCEEDED(device->CreateTexture3D(&varyingVolume,&varyingData,&varyingTexture))&&
        SUCCEEDED(device->CreateShaderResourceView(varyingTexture.Get(),nullptr,&varyingView)),"nonuniform material probe fixture");
    auto varyingSrv=varyingView.Get();ctx->PSSetShaderResources(IrradianceWorldSlot,1,&varyingSrv);
    ctx->PSSetShaderResources(IrradianceDistanceSlot,1,&distanceSrv);
    for(float plane:{31.5f,32.f}){
        float samples[2]{};
        for(unsigned side=0;side<2;++side){
            pack[4]=(plane+(side?1e-4f:-1e-4f))/64;
            ctx->UpdateSubresource(b13.Get(),0,nullptr,pack,0,0);samples[side]=draw()[0];
        }
        Check(std::abs(samples[0]-samples[1])<.002f,"Nonuniform material irradiance is continuous across cage boundaries");
    }
    auto originalSrv=srv.Get();ctx->PSSetShaderResources(IrradianceWorldSlot,1,&originalSrv);
    for(unsigned axis=0;axis<3;++axis){pack[4+axis]=pack[8+axis]=pack[12+axis]=.5f;}
    pack[5]=pack[9]=pack[13]=.5f;ctx->UpdateSubresource(b13.Get(),0,nullptr,pack,0,0);
    ctx->PSSetShaderResources(IrradianceDistanceSlot,1,&distanceSrv);
    for(unsigned i=0;i<6;++i)distances[i*4+2]=1e-5f;
    ctx->UpdateSubresource(distanceTexture.Get(),0,nullptr,distances,16,16);
    const auto lowConfidence=draw();
    for(unsigned i=0;i<3;++i)Check(lowConfidence[i]>0&&lowConfidence[i]<.5f,"Low-confidence cages fade continuously instead of abruptly becoming black");
    for(unsigned i=0;i<6;++i)distances[i*4+2]=0;
    ctx->UpdateSubresource(distanceTexture.Get(),0,nullptr,distances,16,16);
    const auto noConfidence=draw();
    for(unsigned i=0;i<3;++i)Check(std::abs(noConfidence[i])<1e-5f,"Zero-confidence cages cannot introduce an ambient light floor");
    for(unsigned i=0;i<6;++i)distances[i*4+2]=1;
    for(unsigned i=0;i<6;++i){distances[i*4]=i==2?8.f:0;distances[i*4+1]=i==2?64.f:0;}
    ctx->UpdateSubresource(distanceTexture.Get(),0,nullptr,distances,16,16);
    const auto positiveZ=draw();for(unsigned i=0;i<3;++i)Check(std::abs(positiveZ[i]-1)<.001f,"visibility uses the positive Z band for a receiver above the probe");
    for(unsigned i=0;i<6;++i){distances[i*4]=i==5?8.f:0;distances[i*4+1]=i==5?64.f:0;distances[i*4+3]=3.5f;}
    ctx->UpdateSubresource(distanceTexture.Get(),0,nullptr,distances,16,16);
    const auto negativeZ=draw();for(unsigned i=0;i<3;++i)Check(std::abs(negativeZ[i]-1)<.001f,"visibility uses the negative Z band after relocation above the receiver");
    for(unsigned i=0;i<6;++i)distances[i*4+3]=0;
    for(unsigned i=0;i<6;++i){distances[i*4]=distances[i*4+1]=0;}
    ctx->UpdateSubresource(distanceTexture.Get(),0,nullptr,distances,16,16);
    auto occluded=draw();for(unsigned i=0;i<3;++i)Check(std::abs(occluded[i])<.001f,"blocked DDGI probes cannot light through a wall");
    for(unsigned i=0;i<6;++i){distances[i*4]=.02f;distances[i*4+1]=.0004f;distances[i*4+3]=0;}
    ctx->UpdateSubresource(distanceTexture.Get(),0,nullptr,distances,16,16);
    const auto originalOrigin=draw();
    for(unsigned i=0;i<3;++i)Check(std::abs(originalOrigin[i])<.001f,"An unreached receiver is rejected using original probe distance");
    for(unsigned i=0;i<6;++i)distances[i*4+3]=3.35f;
    ctx->UpdateSubresource(distanceTexture.Get(),0,nullptr,distances,16,16);
    const auto relocated=draw();
    for(unsigned i=0;i<3;++i)Check(std::abs(relocated[i]-1)<.001f,"Receiver distance uses the actual relocated trace origin");
    for(unsigned i=0;i<6;++i)distances[i*4+3]=3.5f;
    ctx->UpdateSubresource(distanceTexture.Get(),0,nullptr,distances,16,16);
    const auto beyondReceiver=draw();
    for(unsigned i=0;i<3;++i)Check(std::abs(beyondReceiver[i])<.001f,"Relocation beyond a receiver does not admit blocked irradiance");
    // Screen cache: reject incompatible foreground/background depths and fall
    // back to the original world query without turning missing cache texels black.
    for(float& v:bands)v=0;ctx->UpdateSubresource(texture.Get(),0,nullptr,bands,16,16);
    D3D11_TEXTURE3D_DESC screenDesc=volume;screenDesc.Width=screenDesc.Height=2;
    std::array<float,2*2*6*4> screenValues{};
    auto screenFill=[&](float depth){for(unsigned b=0;b<6;++b)for(unsigned xy=0;xy<4;++xy){
        auto* v=screenValues.data()+(b*4+xy)*4;v[0]=b==2?2.f:0;v[1]=b==2?4.f:0;v[2]=b==2?8.f:0;v[3]=depth;}};
    screenFill(1);D3D11_SUBRESOURCE_DATA screenData{screenValues.data(),2*16,4*16};
    ComPtr<ID3D11Texture3D> screenTexture;ComPtr<ID3D11ShaderResourceView> screenView;
    Check(SUCCEEDED(device->CreateTexture3D(&screenDesc,&screenData,&screenTexture))&&
        SUCCEEDED(device->CreateShaderResourceView(screenTexture.Get(),nullptr,&screenView)),"screen irradiance cache");
    auto screenSrv=screenView.Get();ctx->PSSetShaderResources(IrradianceTextureSlot,1,&screenSrv);
    pack[15]=2;for(unsigned r=0;r<4;++r)pack[16+r*4+r]=1;pack[36]=pack[37]=pack[38]=pack[39]=2;
    ctx->UpdateSubresource(b13.Get(),0,nullptr,pack,0,0);
    const auto screenLit=draw();for(unsigned c=0;c<3;++c)Check(std::abs(screenLit[c]-1)<.001f,"depth-matched screen cache supplies irradiance");
    // A perspective floor has hundreds of units of depth difference between
    // its cache neighbours. It is one continuous surface, not a disocclusion.
    const auto slopedCode=Compile(R"(
        struct V{float4 p:SV_Position;float3 world:TEXCOORD0;float3 normal:TEXCOORD1;float2 uv:TEXCOORD2;};
        V Main(uint id:SV_VertexID){float2 points[6]={float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
            float2 ndc=points[id];float w=1000/(1-.8*ndc.y);V v;
            v.world=float3(ndc*w,0);v.p=float4(ndc*w,.5*w,w);v.normal=float3(0,0,1);v.uv=ndc*.5+.5;return v;})","vs_5_0");
    ComPtr<ID3D11VertexShader> slopedVs;
    Check(SUCCEEDED(device->CreateVertexShader(slopedCode->GetBufferPointer(),slopedCode->GetBufferSize(),nullptr,&slopedVs)),"perspective floor shader");
    for(unsigned b=0;b<6;++b)for(unsigned xy=0;xy<4;++xy)screenValues[(b*4+xy)*4+3]=xy<2?1250.f:625.f;
    ctx->UpdateSubresource(screenTexture.Get(),0,nullptr,screenValues.data(),2*16,4*16);
    pack[29]=.8f;pack[31]=1000;ctx->UpdateSubresource(b13.Get(),0,nullptr,pack,0,0);ctx->VSSetShader(slopedVs.Get(),nullptr,0);
    const auto sloped=draw(2,6);for(unsigned c=0;c<3;++c)Check(std::abs(sloped[c]-1)<.001f,"sloped receiver keeps its screen irradiance instead of fading to coarse dark probes");
    for(unsigned b=0;b<6;++b)for(unsigned xy=0;xy<4;++xy)screenValues[(b*4+xy)*4+3]+=2000;
    ctx->UpdateSubresource(screenTexture.Get(),0,nullptr,screenValues.data(),2*16,4*16);
    const auto slopedOccluder=draw(2,6);for(unsigned c=0;c<3;++c)Check(std::abs(slopedOccluder[c])<.001f,"plane prediction still rejects separate background geometry");
    pack[29]=0;pack[31]=1;ctx->UpdateSubresource(b13.Get(),0,nullptr,pack,0,0);ctx->VSSetShader(vs.Get(),nullptr,0);
    screenFill(1);ctx->UpdateSubresource(screenTexture.Get(),0,nullptr,screenValues.data(),2*16,4*16);
    const auto zeroNormalCode=Compile(R"(
        struct V{float4 p:SV_Position;float3 world:TEXCOORD0;float3 normal:TEXCOORD1;float2 uv:TEXCOORD2;};
        V Main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};
            V v;v.p=float4(p[id],.5,1);v.world=0;v.normal=0;v.uv=p[id]*.5+.5;return v;})","vs_5_0");
    ComPtr<ID3D11VertexShader> zeroNormal;
    Check(SUCCEEDED(device->CreateVertexShader(zeroNormalCode->GetBufferPointer(),zeroNormalCode->GetBufferSize(),nullptr,&zeroNormal)),"zero-normal vertex shader");
    ctx->VSSetShader(zeroNormal.Get(),nullptr,0);const auto degenerate=draw();
    for(unsigned c=0;c<3;++c)Check(std::isfinite(degenerate[c]),"screen cache never emits NaN from a degenerate normal");
    ctx->VSSetShader(vs.Get(),nullptr,0);
    // Half the footprint is distant geometry; it must not darken the foreground.
    for(unsigned b=0;b<6;++b)for(unsigned xy=0;xy<2;++xy)screenValues[(b*4+xy)*4+3]=100;
    ctx->UpdateSubresource(screenTexture.Get(),0,nullptr,screenValues.data(),2*16,4*16);
    const auto edge=draw();for(unsigned c=0;c<3;++c)Check(std::abs(edge[c]-1)<.001f,"bilateral upsample rejects a distant neighbour without losing foreground light");
    screenFill(100);ctx->UpdateSubresource(screenTexture.Get(),0,nullptr,screenValues.data(),2*16,4*16);
    const auto rejected=draw();for(unsigned c=0;c<3;++c)Check(std::abs(rejected[c])<.001f,"incompatible screen depths fall back to dark world probes");
    screenFill(-1);ctx->UpdateSubresource(screenTexture.Get(),0,nullptr,screenValues.data(),2*16,4*16);
    const auto missing=draw();for(unsigned c=0;c<3;++c)Check(std::abs(missing[c])<.001f,"missing depth cannot reuse cached light");
    resource=srv.Get();ctx->PSSetShaderResources(IrradianceTextureSlot,1,&resource);
    pack[15]=0;ctx->UpdateSubresource(b13.Get(),0,nullptr,pack,0,0);
    for(size_t n:{size_t(0),size_t(31),source->GetBufferSize()-1})Check(!DynamicIrradianceProgram(source->GetBufferPointer(),n,patched),"reject truncated programs");
    const auto dir=std::filesystem::path(__FILE__).parent_path().parent_path()/"output/lighting-replacement/fixture-shaders";
    unsigned validated=0,worldPrograms=0;
    std::array<float,16384> emptyConstants{};auto nativeConstants=buffer(emptyConstants.data(),sizeof(emptyConstants));
    std::array<ID3D11Buffer*,13> nativeBuffers;nativeBuffers.fill(nativeConstants.Get());
    struct NativeTarget{ComPtr<ID3D11RenderTargetView> view;DXGI_FORMAT format;UINT samples;};std::vector<NativeTarget> nativeTargets;
    if(hardware)for(const auto format:{DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R8G8B8A8_UNORM})for(UINT samples:{1u,4u,8u}){
        UINT quality=0;Check(SUCCEEDED(device->CheckMultisampleQualityLevels(format,samples,&quality)),"target sample support");if(!quality)continue;
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=4;td.MipLevels=td.ArraySize=1;td.Format=format;td.SampleDesc.Count=samples;td.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> image;NativeTarget output{{},format,samples};
        Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&image))&&SUCCEEDED(device->CreateRenderTargetView(image.Get(),nullptr,&output.view)),"native output target");nativeTargets.push_back(std::move(output));
    }
    if(warm)for(const auto& output:nativeTargets){
        std::cout<<"Synthetic target "<<output.format<<" samples="<<output.samples<<std::endl;
        auto outputView=output.view.Get();ctx->OMSetRenderTargets(1,&outputView,nullptr);draw();
    }
    ctx->OMSetRenderTargets(1,&rt,nullptr);
    if(std::filesystem::exists(dir))for(const auto& item:std::filesystem::directory_iterator(dir))if(item.path().extension()==".dxbc"){
        if(!fixture.empty()&&item.path().stem()!=fixture)continue;
        std::ifstream file(item.path(),std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
        Filmmaker::Environment::NativeSunAbiInfo abi;Filmmaker::Environment::HasNativeSunMaterialAbi(bytes.data(),bytes.size(),&abi);
        // An explicitly selected capture fixture may be a weapon/character probe
        // program; the default bulk suite retains its world-program coverage.
        if(!(abi.lightmapArrayMask&2)&&(fixture.empty()||!abi.probeSlot))continue;++worldPrograms;
        {
            uint32_t chunkCount;std::memcpy(&chunkCount,bytes.data()+28,4);
            for(unsigned chunk=0;chunk<chunkCount;++chunk){
                uint32_t start,size;std::memcpy(&start,bytes.data()+32+chunk*4,4);std::memcpy(&size,bytes.data()+start+4,4);
                if(std::memcmp(bytes.data()+start,"SHEX",4)&&std::memcmp(bytes.data()+start,"SHDR",4))continue;
                IrradianceDxbc::Words words(size/4);std::memcpy(words.data(),bytes.data()+start+8,size);
                const auto kernels=Filmmaker::Environment::SmoothNativeLampKernels(words);
                unsigned depths=0,sources=0;
                for(size_t at=2;at<words.size();){
                    const unsigned op=words[at]&2047;const size_t length=op==53?words[at+1]:(words[at]>>24)&127;
                    if(op==45||op==167){
                        size_t cursor=at;uint32_t ext=words[at];while(ext>>31)ext=words[++cursor];++cursor;
                        std::vector<IrradianceDxbc::Operand> operands;
                        while(cursor<at+length){IrradianceDxbc::Operand o;Check(IrradianceDxbc::ReadOperand(words,cursor,at+length,o),"soft-shadow operands decode");operands.push_back(o);}
                        if(op==45&&operands.size()==3&&operands[2].index==6){
                            Check(((words[at+1]>>6)&31)==3,"Blocker search loads the declared 2D depth atlas");++depths;}
                        if(op==167&&operands.size()==4&&operands[2].type==4&&words[operands[2].start+1]==336)++sources;
                    }
                    at+=length;
                }
                if(kernels)Check(sources==kernels&&depths==5*kernels,"Each owned lamp kernel retains its source index and five blocker depths");
                if(!fixture.empty())std::cout<<"Local shadow kernels="<<kernels<<std::endl;
            }
        }
        std::vector<uint8_t> atmosphere;
        bool ownedCascades=false;uint32_t screenShadowSlot=0;
        // Match the production path: owned AO and cascades enable transformations
        // that the fog-only default intentionally leaves untouched.
        const bool ownsAtmosphere=Filmmaker::Environment::NativeAtmosphereProgram(bytes.data(),bytes.size(),atmosphere,true,&ownedCascades,&screenShadowSlot);
        if(!DynamicIrradianceProgram(ownsAtmosphere?atmosphere.data():bytes.data(),
                                    ownsAtmosphere?atmosphere.size():bytes.size(),patched,&info,abi.probeSlot)){
            auto disassemble=reinterpret_cast<pD3DDisassemble>(GetProcAddress(Filmmaker::Environment::EnvironmentShaderCompiler(),"D3DDisassemble"));
            ComPtr<ID3DBlob> listing;if(disassemble&&SUCCEEDED(disassemble(bytes.data(),bytes.size(),0,nullptr,&listing))){
                auto path=item.path();path.replace_extension(".irradiance-rejected.txt");std::ofstream report(path);report<<static_cast<const char*>(listing->GetBufferPointer());}
            std::cerr<<item.path()<<" rejected\n";continue;
        }
        if(cold){
            // Change only the query's degenerate-normal floor to exercise a fresh
            // driver-cache key. Unit surface normals retain identical lighting.
            const auto before=IrradianceDxbc::Float(IrradianceNormalEpsilon),after=IrradianceDxbc::Float(IrradianceNormalEpsilon+float(cold)*1e-12f);
            for(size_t offset=32;offset+4<=patched.size();offset+=4){uint32_t value;std::memcpy(&value,patched.data()+offset,4);
                if(value==before)std::memcpy(patched.data()+offset,&after,4);}
            Filmmaker::Environment::DxbcHash::ComputeHashRetail(patched.data()+20,unsigned(patched.size()-20),patched.data()+4);
        }
        if(constantSamples){
            uint32_t chunkCount;std::memcpy(&chunkCount,patched.data()+28,4);
            for(unsigned chunk=0;chunk<chunkCount;++chunk){
                uint32_t start,size;std::memcpy(&start,patched.data()+32+chunk*4,4);std::memcpy(&size,patched.data()+start+4,4);
                if(std::memcmp(patched.data()+start,"SHEX",4)&&std::memcmp(patched.data()+start,"SHDR",4))continue;
                IrradianceDxbc::Words code(size/4);std::memcpy(code.data(),patched.data()+start+8,size);
                for(size_t i=2;i<code.size();){
                    const unsigned opcode=code[i]&2047;const size_t length=opcode==53?code[i+1]:(code[i]>>24)&127;
                    if(opcode==72){
                        size_t cursor=i+1;uint32_t extended=code[i];while(extended&0x80000000u)extended=code[cursor++];
                        IrradianceDxbc::Operand destination,coordinates,resource;
                        Check(IrradianceDxbc::ReadOperand(code,cursor,i+length,destination)&&IrradianceDxbc::ReadOperand(code,cursor,i+length,coordinates)&&IrradianceDxbc::ReadOperand(code,cursor,i+length,resource),"sample operands");
                        if(resource.type==7&&(resource.index==IrradianceTextureSlot||resource.index==IrradianceWorldSlot)){
                            IrradianceDxbc::Words replacement;
                            IrradianceDxbc::Emit(replacement,54,{{code[destination.start],code[destination.start+1]},IrradianceDxbc::Literal(1,1,1,1)});
                            while(replacement.size()<length)replacement.push_back(0x0100003au);
                            Check(replacement.size()==length,"constant sample instruction footprint");std::copy(replacement.begin(),replacement.end(),code.begin()+i);
                        }
                    }
                    i+=length;
                }
                std::memcpy(patched.data()+start+8,code.data(),size);
            }
            Filmmaker::Environment::DxbcHash::ComputeHashRetail(patched.data()+20,unsigned(patched.size()-20),patched.data()+4);
        }
        if(hardware)std::cout<<"Hardware shader "<<item.path().filename()<<" queries="<<info.queries<<" evaluations="<<info.evaluations<<std::endl;
        if(!fixture.empty()){
            auto disassemble=reinterpret_cast<pD3DDisassemble>(GetProcAddress(Filmmaker::Environment::EnvironmentShaderCompiler(),"D3DDisassemble"));
            ComPtr<ID3DBlob> listing;if(disassemble&&SUCCEEDED(disassemble(patched.data(),patched.size(),0,nullptr,&listing))){
                auto path=item.path();path.replace_extension(".irradiance.txt");std::ofstream report(path);report<<static_cast<const char*>(listing->GetBufferPointer());}
        }
        const auto& program=original?bytes:patched;
        ComPtr<ID3D11PixelShader> shader;Check(SUCCEEDED(device->CreatePixelShader(program.data(),program.size(),nullptr,&shader)),"shipped dynamic irradiance shader validates");
        const auto fixtureVs=FixtureVertexShader(device.Get(),bytes);
        ctx->PSSetConstantBuffers(0,UINT(nativeBuffers.size()),nativeBuffers.data());ctx->VSSetShader(fixtureVs.Get(),nullptr,0);ctx->PSSetShader(shader.Get(),nullptr,0);
        // Force execution and readback: NVIDIA may defer final shader compilation until Draw.
        draw();Check(device->GetDeviceRemovedReason()==S_OK,"native material draw retains hardware device");++validated;
        if(hardware)std::cout<<"Draw completed "<<item.path().filename()<<std::endl;
        for(const auto& output:nativeTargets){
            std::cout<<"Target "<<output.format<<" samples="<<output.samples<<std::endl;
            auto outputView=output.view.Get();ctx->OMSetRenderTargets(1,&outputView,nullptr);draw();
            Check(device->GetDeviceRemovedReason()==S_OK,"native MSAA material draw retains hardware device");
        }
        ctx->OMSetRenderTargets(1,&rt,nullptr);
    }
    if(!fixture.empty())Check(worldPrograms==1,"explicit fixture must match one supported native program");
    std::cout<<"Validated "<<validated<<"/"<<worldPrograms<<" shipped native programs\n";
    Check(validated==worldPrograms,"all extracted world permutations transformed");
}
