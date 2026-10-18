#define NOMINMAX
#include "../../AfxHookSource2/Filmmaker/Relight/RelightReflectionFilter.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace Filmmaker::Relight;
using Microsoft::WRL::ComPtr;
void Check(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
float Half(uint16_t value){const auto exponent=(value>>10)&31,mantissa=value&1023;
    return (value&0x8000?-1.f:1.f)*std::ldexp(exponent?1.f+mantissa/1024.f:mantissa/1024.f,exponent?int(exponent)-15:-14);}
int main(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,8,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"WARP device");
    constexpr unsigned size=32,mips=6;ReflectionFilter filter;Check(filter.Initialize(device.Get(),size),"Reflection filter initializes");
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=size;d.MipLevels=mips;d.ArraySize=6;d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    d.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE|D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> source;ComPtr<ID3D11ShaderResourceView> view;
    Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&source)),"Source cube");
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=d.Format;s.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    s.TextureCubeArray.MipLevels=mips;s.TextureCubeArray.NumCubes=1;
    Check(SUCCEEDED(device->CreateShaderResourceView(source.Get(),&s,&view)),"Source cube array view");
    auto read=[&](ID3D11ShaderResourceView* input,unsigned face,unsigned mip){
        ComPtr<ID3D11Resource> resource;input->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;resource.As(&texture);
        D3D11_TEXTURE2D_DESC t{};texture->GetDesc(&t);t.Width=t.Height=size>>mip;t.ArraySize=t.MipLevels=1;
        t.BindFlags=t.MiscFlags=0;t.Usage=D3D11_USAGE_STAGING;t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;Check(SUCCEEDED(device->CreateTexture2D(&t,nullptr,&staging)),"Readback texture");
        ctx->CopySubresourceRegion(staging.Get(),0,0,0,0,texture.Get(),face*mips+mip,nullptr);
        D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"Readback");
        std::vector<uint16_t> pixels(t.Width*t.Height*4);
        for(unsigned y=0;y<t.Height;++y)std::memcpy(pixels.data()+y*t.Width*4,static_cast<const char*>(map.pData)+y*map.RowPitch,t.Width*8);
        ctx->Unmap(staging.Get(),0);return pixels;
    };
    auto upload=[&](bool constant){
        std::vector<uint16_t> data(size*size*4);
        for(unsigned face=0;face<6;++face){
            for(unsigned i=0;i<size*size;++i){
                data[i*4]=constant?0x3800:face==0?0x3c00:0;
                data[i*4+1]=constant?0x3c00:0;data[i*4+2]=constant?0x4000:0;data[i*4+3]=0x3c00;
            }
            ctx->UpdateSubresource(source.Get(),face*mips,nullptr,data.data(),size*8,0);
        }
        ctx->GenerateMips(view.Get());Check(filter.Apply(ctx.Get(),source.Get(),view.Get()),"Reflection filter dispatch");
    };
    upload(true);
    for(unsigned face=0;face<6;++face)for(unsigned mip=0;mip<mips;++mip){
        const auto pixels=read(filter.View(),face,mip);
        for(unsigned i=0;i<pixels.size();i+=4)Check(std::abs(Half(pixels[i])-.5f)<.001f&&
            std::abs(Half(pixels[i+1])-1)<.001f&&std::abs(Half(pixels[i+2])-2)<.002f,
            "A uniform HDR environment preserves energy at every roughness and cube face");
    }
    upload(false);
    for(unsigned face=0;face<6;++face)Check(read(filter.View(),face,0)==read(view.Get(),face,0),"Mirror-level detail is copied exactly");
    const float rawSide=Half(read(view.Get(),2,mips-1)[0]);
    const float filteredSide=Half(read(filter.View(),2,mips-1)[0]);
    Check(rawSide==0&&filteredSide>.03f&&filteredSide<.5f,"Rough reflection lobes gather illumination across cube face boundaries");
    Check(Half(read(filter.View(),0,mips-1)[0])<.9f,"Rough reflections spread a sharp source rather than retaining the box-mip highlight");
    const auto first=read(filter.View(),2,2);upload(false);
    Check(first==read(filter.View(),2,2),"Filtering is deterministic across identical frames");
    std::cout<<"Roughness reflection filtering: HDR energy, exact mirror detail, cross-face footprint and deterministic frames PASS\n";
}
