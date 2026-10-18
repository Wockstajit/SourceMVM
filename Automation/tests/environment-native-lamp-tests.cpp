#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeShadowMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShaderCompiler.h"
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeAtmosphereMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeMaterialMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeAoMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeLightingMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeClusterMath.h"
#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentShadowPyramid.h"
using Microsoft::WRL::ComPtr;
using namespace Filmmaker::Environment;
using namespace Filmmaker::Relight::IrradianceDxbc;
void Check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
// Lane a source operand feeds to destination lane `lane`.
unsigned Lane(const Words& operand,unsigned lane){return (operand[0]>>(4+2*lane))&3;}
void TestSourceForMask(){
    // CS2's lightmapped world kernel: mad r16.zw, r16.zzzw, rect.zzzw, rect.xxxy. Copied into an
    // .xy move, the face NDC must arrive as (z, w), not (z, z) -- the square under placed lamps.
    const Words ndc=Temp(16,2|(2<<2)|(2<<4)|(3<<6));
    const auto zw=SourceForMask(ndc,12);
    Check(Lane(zw,0)==2&&Lane(zw,1)==3,"A .zw kernel's face NDC is read as (z, w)");
    Check(zw[1]==16,"Re-swizzling keeps the register");
    // The .xy kernel (mad r8.xy, r6.xyxx, ...) already reads in place.
    const auto xy=SourceForMask(Temp(6,0|(1<<2)),3);
    Check(Lane(xy,0)==0&&Lane(xy,1)==1,"A .xy kernel's face NDC is unchanged");
    // A single written lane replicates; a scalar-select operand is left alone.
    const auto w=SourceForMask(Temp(4,0xe4),8);
    Check(Lane(w,0)==3&&Lane(w,1)==3,"A .w write reads w");
    Words scalar{0x00100002u|(2u<<2)|(1u<<4),7};
    Check(SourceForMask(scalar,12)==scalar,"Select-1 operands are not re-swizzled");
}
int main(int argc,char** argv){
    TestSourceForMask();
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;bool hardware=argc>1&&std::string(argv[1])=="--hardware";
    auto hr=D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx);
    if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING)hr=D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx);
    Check(SUCCEEDED(hr),"Device");ComPtr<ID3D11InfoQueue> debug;device.As(&debug);
    auto compile=[&](const std::string& text,const char* profile){ComPtr<ID3DBlob> code,error;
        auto result=CompileEnvironmentShader(text.data(),text.size(),"NativeLampGpuTest",nullptr,"Main",profile,D3DCOMPILE_ENABLE_STRICTNESS,&code,&error);
        if(error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());Check(SUCCEEDED(result),"Compile");return code;};
    auto base=compile("cbuffer P:register(b0){float4 p;} Texture2D<float> t:register(t6);SamplerComparisonState s:register(s6);float4 Main(float4 v:SV_Position,uint sample:SV_SampleIndex):SV_Target{return t.SampleCmpLevelZero(s,p.xy+v.xy*p.w,p.z+sample*.000001);}","ps_5_0");
    auto make=[&](bool classify,float width,float height,NativeLampPyramid pyramid){
        const auto* raw=static_cast<const uint8_t*>(base->GetBufferPointer());const size_t size=base->GetBufferSize();auto word=[&](size_t at){UINT v;memcpy(&v,raw+at,4);return v;};
        const UINT chunks=word(28);size_t program=0;Words code;
        for(unsigned i=0;i<chunks;++i){auto at=word(32+i*4);if(word(at)==0x58454853||word(at)==0x52444853){program=at;code.resize(word(at+4)/4);memcpy(code.data(),raw+at+8,code.size()*4);}}
        Check(program!=0,"Shader chunk");Words output{code[0],0};unsigned temps=0;
        for(size_t at=2;at<code.size();){const unsigned op=code[at]&2047,n=(code[at]>>24)&127;Check(n&&at+n<=code.size(),"Instructions");
            if(op==104){temps=code[at+1];output.insert(output.end(),{0x02000068u,temps+NativeLampDiskTemps});}
            else if(op==88&&code[at+2]==6){output.insert(output.end(),code.begin()+at,code.begin()+at+n);
                if(pyramid.levels)output.insert(output.end(),{0x04001858u,0x00107000u,pyramid.slot,0x00004444u});}
            else if(op==89&&code[at+2]==0){output.insert(output.end(),code.begin()+at,code.begin()+at+n);output.back()=3;}
            else if(op==71){
                size_t cursor=at;UINT ext=code[cursor];while(ext>>31)ext=code[++cursor];++cursor;std::vector<Operand> args;
                while(cursor<at+n){Operand o;Check(ReadOperand(code,cursor,at+n,o),"Sample operands");args.push_back(o);}Check(args.size()==5&&temps,"Sample");
                auto words=[&](const Operand& o){return Words(code.begin()+o.start,code.begin()+o.end);};
                auto compare=[&](const Words& d,const Words& uv,const Words& ref){
                    Words w(code.begin()+at,code.begin()+args[0].start);for(const auto& a:{d,uv,words(args[2]),words(args[3]),ref})w.insert(w.end(),a.begin(),a.end());
                    w[0]=(w[0]&~0x7f000000u)|(UINT(w.size())<<24);output.insert(output.end(),w.begin(),w.end());};
                auto gather=[&](const Words& d,const Words& uv,const Words& ref){auto sampler=words(args[3]);sampler[0]=(sampler[0]&~0xfffu)|0xau;Emit(output,0x8000007eu,{{0x800000c2u,0x00155543u},d,uv,Words{0x00107e46u,6},sampler,ref});};
                Emit(output,54,{Dest(temps+3,3),Cb(0,1)});Emit(output,54,{Dest(temps+9,1),Cb(0,1,170)});
                Emit(output,54,{Dest(temps+8,8),Cb(0,1,255)});Emit(output,54,{Dest(temps+4,8),Cb(0,2,0)});
                Emit(output,54,{Dest(temps+5,8),Literal(1,1,1,1)});
                NativeLampDisk(output,temps,words(args[1]),words(args[4]),words(args[0]),compare,gather,classify,width,height,pyramid);

            }else {if(op==62)Emit(output,54,{Words{0x00102022u,0},Temp(temps+5,255)});output.insert(output.end(),code.begin()+at,code.begin()+at+n);}
            at+=n;
        }
        output[1]=UINT(output.size());std::vector<uint8_t> bytes(raw,raw+32+chunks*4);auto put=[&](size_t at,UINT v){memcpy(bytes.data()+at,&v,4);};
        for(unsigned i=0;i<chunks;++i){size_t at=word(32+i*4),to=bytes.size();put(32+i*4,UINT(to));
            if(at==program){bytes.insert(bytes.end(),raw+at,raw+at+8);bytes.resize(bytes.size()+output.size()*4);memcpy(bytes.data()+to+8,output.data(),output.size()*4);put(to+4,UINT(output.size()*4));}
            else bytes.insert(bytes.end(),raw+at,raw+at+8+word(at+4));}
        put(24,UINT(bytes.size()));DxbcHash::ComputeHashRetail(bytes.data()+20,UINT(bytes.size()-20),bytes.data()+4);
        ComPtr<ID3D11PixelShader> shader;Check(SUCCEEDED(device->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&shader)),"Generated filter validates");return shader;
    };
    auto vertexCode=compile("float4 Main(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vertex;
    Check(SUCCEEDED(device->CreateVertexShader(vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),nullptr,&vertex)),"Vertex");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=48;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> params;Check(SUCCEEDED(device->CreateBuffer(&bd,nullptr,&params)),"Constants");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;sd.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;sd.BorderColor[0]=sd.BorderColor[1]=sd.BorderColor[2]=sd.BorderColor[3]=1;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Check(SUCCEEDED(device->CreateSamplerState(&sd,&sampler)),"Comparison sampler");
    unsigned comparisons=0,fastSamples=0,mixedSamples=0,summaryLit=0,summaryShadowed=0;
    // Small atlases exercise the one-footprint gather and the summary's edges (odd sizes, reads past
    // a mip); the large one exercises disks of many texels against the summary, including blocks
    // left of its origin and a NaN depth inside the disk.
    struct Config {unsigned width,height,origin,tests;bool large;};
    for(const auto config:{Config{17,11,0,240,false},Config{128,32,0,240,false},Config{256,64,32,720,true}}){
        const unsigned dims[2]{config.width,config.height};
        unsigned levels=1; // the summary's full mip chain
        for(unsigned size=(std::max)(ShadowPyramidWidth(dims[0]-config.origin,0),ShadowPyramidWidth(dims[1],0));size>1&&levels<NativeLampPyramidLevels;size>>=1)++levels;
        const NativeLampPyramid pyramid{NativeLampPyramidSlot,config.origin,levels};
        auto original=make(false,float(dims[0]),float(dims[1]),{}),optimized=make(true,float(dims[0]),float(dims[1]),pyramid);
        std::vector<float> depths(dims[0]*dims[1]);D3D11_TEXTURE2D_DESC td{};td.Width=dims[0];td.Height=dims[1];td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> srv;Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&texture))&&SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&srv)),"Depth atlas");
        ShadowPyramidBuilder summary;std::string error;
        if(!summary.Create(device.Get(),dims[0],dims[1],config.origin,levels,error))std::cerr<<error<<'\n';Check(summary.Created(),"Atlas summary");
        for(unsigned samples:{1u,4u,8u}){
            td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.Width=8;td.Height=4;td.SampleDesc.Count=samples;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> target,flat,staging;ComPtr<ID3D11RenderTargetView> rtv,flatRtv;ComPtr<ID3D11ShaderResourceView> image;
            Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&target))&&SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv))&&SUCCEEDED(device->CreateShaderResourceView(target.Get(),nullptr,&image)),"MSAA output");
            td.Width*=samples;td.SampleDesc.Count=1;Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&flat))&&SUCCEEDED(device->CreateRenderTargetView(flat.Get(),nullptr,&flatRtv)),"Every sample output");td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&staging)),"Readback");
            auto extractCode=compile(samples>1?"Texture2DMS<float4> t:register(t1);float4 Main(float4 p:SV_Position):SV_Target{return t.Load(int2((uint)p.x/"+std::to_string(samples)+",p.y),(uint)p.x%"+std::to_string(samples)+");}":"Texture2D<float4> t:register(t1);float4 Main(float4 p:SV_Position):SV_Target{return t.Load(int3(p.xy,0));}","ps_5_0");ComPtr<ID3D11PixelShader> extract;Check(SUCCEEDED(device->CreatePixelShader(extractCode->GetBufferPointer(),extractCode->GetBufferSize(),nullptr,&extract)),"Extract");
            for(unsigned test=0;test<config.tests;++test){
                const float fractions[]{.003f,.25f,.49f,.5f,.51f,.75f,.997f};const float radii[]{0,.001f,.01f,.05f,.2f,.4f,.8f,1.2f};const float refs[]{0,.2f,.3f,.5f,.7f,.8f,1,1.2f};
                const float largeRadii[]{1.5f,2.5f,4,7,12,20,31,.6f};const float largeRefs[]{.2f,.29f,.3f,.5f,.51f,.7f,.8f,1,1.2f,0};const unsigned blockers[]{3,10,25,40};
                const float cx=float(dims[0]/2)+fractions[test%7],cy=float(dims[1]/2)+fractions[(test/7)%7];
                for(unsigned y=0;y<dims[1];++y)for(unsigned x=0;x<dims[0];++x){float& d=depths[y*dims[0]+x];
                    if(!config.large){d=test%4==0?.25f:test%4==1?.75f:test%4==2?((x+y)%2?.7f:.3f):.2f+.6f*x/dims[0];continue;}
                    const unsigned b=blockers[(test/6)%4];
                    switch(test%6){
                    case 0:d=.5f;break;
                    case 1:d=std::abs(float(x)-cx)<=b&&std::abs(float(y)-cy)<=b?.3f:.8f;break;
                    case 2:d=.2f+.6f*x/dims[0];break;
                    case 3:d=(x+y)%2?.7f:.3f;break;
                    case 4:d=x==unsigned(cx)&&y==unsigned(cy)?std::numeric_limits<float>::quiet_NaN():.5f;break;
                    default:d=x<8?.1f:.5f;break;}
                }
                ctx->UpdateSubresource(texture.Get(),0,nullptr,depths.data(),dims[0]*4,0);
                summary.Mark(0,0,dims[0],dims[1]);Check(summary.Record(ctx.Get(),srv.Get(),error),"Atlas summary update");
                const float rx=config.large?largeRadii[test%8]:radii[test%8],ry=config.large?largeRadii[(test/8)%8]:radii[(test/8)%8];
                std::array<float,12> values{cx/dims[0],cy/dims[1],config.large?largeRefs[(test/3)%10]:refs[(test/3)%8],.00001f,
                    rx/dims[0],ry/dims[1],test%5==0?.01f:test%5==1?-.001f:test%5==2&&config.large?.05f:0,test%3==0?.2f:test%3==1?.5f:1,
                    test%13==0?0.f:test%2?-12345.f:-12346.f,0,0,0};
                if(test%17==0)values[0]=-.01f; // atlas-border fallback
                if(config.large&&test%11==0)values[0]=(config.origin*.6f+fractions[test%7])/dims[0]; // left of the summary
                auto draw=[&](ID3D11PixelShader* ps){ctx->UpdateSubresource(params.Get(),0,nullptr,values.data(),0,0);auto b=params.Get();ctx->PSSetConstantBuffers(0,1,&b);auto s=srv.Get();ctx->PSSetShaderResources(6,1,&s);auto sam=sampler.Get();ctx->PSSetSamplers(6,1,&sam);
                    s=summary.View();ctx->PSSetShaderResources(NativeLampPyramidSlot,1,&s);
                    const float clear[]{-1,-1,-1,-1};ctx->ClearRenderTargetView(rtv.Get(),clear);auto r=rtv.Get();ctx->OMSetRenderTargets(1,&r,nullptr);D3D11_VIEWPORT v{0,0,8,4,0,1};ctx->RSSetViewports(1,&v);ctx->VSSetShader(vertex.Get(),nullptr,0);ctx->PSSetShader(ps,nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->Draw(3,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);
                    s=image.Get();ctx->PSSetShaderResources(1,1,&s);r=flatRtv.Get();ctx->OMSetRenderTargets(1,&r,nullptr);v.Width=8.f*samples;ctx->RSSetViewports(1,&v);ctx->PSSetShader(extract.Get(),nullptr,0);ctx->Draw(3,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);s=nullptr;ctx->PSSetShaderResources(1,1,&s);ctx->CopyResource(staging.Get(),flat.Get());
                    D3D11_MAPPED_SUBRESOURCE m{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"Read samples");std::vector<UINT> result(8*4*samples*4);for(unsigned y=0;y<4;++y)memcpy(result.data()+y*8*samples*4,static_cast<const char*>(m.pData)+y*m.RowPitch,8*samples*16);ctx->Unmap(staging.Get(),0);return result;};
                auto expected=draw(original.Get()),actual=draw(optimized.Get());
                for(unsigned i=0;i<actual.size();i+=4){if(expected[i]!=actual[i])std::cerr<<"size="<<dims[0]<<" samples="<<samples<<" case="<<test<<" pixel="<<i/4<<" ref="<<values[2]<<" radius="<<values[4]<<","<<values[5]<<" old="<<expected[i]<<" new="<<actual[i]<<char(10);Check(expected[i]==actual[i]&&expected[i+2]==actual[i+2]&&expected[i+3]==actual[i+3],"Every sample matches unchanged 32-comparison filter");
                    if(actual[i+1]==0){++fastSamples;if(config.large&&rx>=1.5f){if(actual[i])++summaryLit;else ++summaryShadowed;}}else ++mixedSamples;}
                ++comparisons;
            }
        }
    }
    Check(fastSamples>0&&mixedSamples>0,"Both shortcut and full disk paths exercised");
    Check(summaryLit>0&&summaryShadowed>0,"The atlas summary proves both lit and shadowed disks");
    unsigned native=0,shared=0,sparse=0;
    if(argc>2)for(const auto& entry:std::filesystem::directory_iterator(argv[2]))if(entry.path().extension()==L".dxbc"){
        std::ifstream f(entry.path(),std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{}),atmosphere,dynamic,ao,lighting,sp;
        NativeSunAbiInfo abi{};const bool sun=HasNativeSunMaterialAbi(bytes.data(),bytes.size(),&abi);bool cascades=false;uint32_t screen=0;
        if(!NativeAtmosphereProgram(bytes.data(),bytes.size(),atmosphere,true,sun?&cascades:nullptr,&screen))continue;
        auto candidate=atmosphere;
        if(Filmmaker::Relight::DynamicIrradianceProgram(candidate.data(),candidate.size(),dynamic,nullptr,abi.probeSlot))candidate=dynamic;
        if(NativeSharedAoProgram(candidate.data(),candidate.size(),ao))candidate=ao;
        ComPtr<ID3D11PixelShader> ps;Check(SUCCEEDED(device->CreatePixelShader(candidate.data(),candidate.size(),nullptr,&ps)),"Captured native classifier validates");++native;
        NativeSharedLightingInfo layout;
        if(NativeSharedLightingProgram(candidate.data(),candidate.size(),lighting,layout)){Check(SUCCEEDED(device->CreatePixelShader(lighting.data(),lighting.size(),nullptr,&ps)),"Captured shared lighting validates");++shared;}
        if(NativeSparseClusterProgram(candidate.data(),candidate.size(),sp)&&NativeSharedLightingProgram(sp.data(),sp.size(),lighting,layout)){Check(SUCCEEDED(device->CreatePixelShader(lighting.data(),lighting.size(),nullptr,&ps)),"Captured sparse lighting validates");++sparse;}
    }
    if(argc>2)std::cout<<native<<" native / "<<shared<<" shared / "<<sparse<<" sparse captured variants validated\n";
    if(debug)for(UINT64 i=0;i<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;debug->GetMessage(i,nullptr,&size);std::vector<char> data(size);auto m=reinterpret_cast<D3D11_MESSAGE*>(data.data());debug->GetMessage(i,m,&size);Check(m->Severity>D3D11_MESSAGE_SEVERITY_ERROR,m->pDescription);}
    std::cout<<comparisons<<" bitwise every-sample filter comparisons, "<<fastSamples<<" fast ("<<summaryLit<<" lit / "<<summaryShadowed<<" shadowed from the summary) / "<<mixedSamples<<" full samples, no D3D errors\n";
}