#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include "../Render/RenderPrivateTextureScope.h"
namespace Filmmaker::Environment {
// Native depth can be sampled after unbinding the output merger when its
// resource supports a compatible SRV. Otherwise retain an exact sample-for-
// sample copy. Never cache depth contents by resource identity or frame number.
class DepthReadTarget {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source,copy;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    D3D11_TEXTURE2D_DESC copyDesc{};
    bool direct=false;
public:
    bool Prepare(ID3D11Device* device,ID3D11Texture2D* input) {
        if(!device||!input)return false;
        if(source.Get()==input&&view)return true;
        D3D11_TEXTURE2D_DESC d{};input->GetDesc(&d);
        if(!d.Width||!d.Height||d.ArraySize!=1||d.MipLevels!=1||
           (d.SampleDesc.Count!=1&&d.SampleDesc.Count!=2&&d.SampleDesc.Count!=4&&d.SampleDesc.Count!=8))return false;
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
        switch(d.Format){
        case DXGI_FORMAT_D32_FLOAT:case DXGI_FORMAT_R32_TYPELESS:d.Format=DXGI_FORMAT_R32_TYPELESS;format=DXGI_FORMAT_R32_FLOAT;break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:case DXGI_FORMAT_R24G8_TYPELESS:d.Format=DXGI_FORMAT_R24G8_TYPELESS;format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;break;
        case DXGI_FORMAT_D16_UNORM:case DXGI_FORMAT_R16_TYPELESS:d.Format=DXGI_FORMAT_R16_TYPELESS;format=DXGI_FORMAT_R16_UNORM;break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:case DXGI_FORMAT_R32G8X24_TYPELESS:d.Format=DXGI_FORMAT_R32G8X24_TYPELESS;format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;break;
        default:return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sr{};sr.Format=format;
        sr.ViewDimension=d.SampleDesc.Count>1?D3D11_SRV_DIMENSION_TEXTURE2DMS:D3D11_SRV_DIMENSION_TEXTURE2D;sr.Texture2D.MipLevels=1;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> next;
        if((d.BindFlags&D3D11_BIND_SHADER_RESOURCE)&&SUCCEEDED(device->CreateShaderResourceView(input,&sr,&next))){
            source=input;view=next;copy.Reset();copyDesc={};direct=true;return true;
        }
        d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        if(!copy||copyDesc.Width!=d.Width||copyDesc.Height!=d.Height||copyDesc.Format!=d.Format||
           copyDesc.SampleDesc.Count!=d.SampleDesc.Count||copyDesc.SampleDesc.Quality!=d.SampleDesc.Quality){
            RenderPrivateTextureScope privateTextures;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
            if(FAILED(device->CreateTexture2D(&d,nullptr,&target)))return false;
            copy=target;copyDesc=d;
        }
        if(FAILED(device->CreateShaderResourceView(copy.Get(),&sr,&next)))return false;
        source=input;view=next;direct=false;return true;
    }
    ID3D11ShaderResourceView* View()const{return view.Get();}
    bool Direct()const{return direct;}
    void Copy(ID3D11DeviceContext* ctx)const{if(!direct)ctx->CopyResource(copy.Get(),source.Get());}
};
class DepthReadScope {
    ID3D11DeviceContext* context=nullptr;
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* depth=nullptr;
    ID3D11UnorderedAccessView* uavs[D3D11_1_UAV_SLOT_COUNT]{};
    UINT count=0,uavCount=0;
public:
    DepthReadScope(ID3D11DeviceContext* ctx,const DepthReadTarget& input) {
        if(!input.Direct()){input.Copy(ctx);return;}
        context=ctx;ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,targets,&depth);
        for(UINT i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i)if(targets[i])count=i+1;
        Microsoft::WRL::ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        uavCount=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?D3D11_1_UAV_SLOT_COUNT:D3D11_PS_CS_UAV_REGISTER_COUNT;
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,uavCount,uavs);
        ID3D11UnorderedAccessView* none[D3D11_1_UAV_SLOT_COUNT]{};
        // This private binding change also avoids the engine-target observations
        // attached to the ordinary OMSetRenderTargets draw path.
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,uavCount,none,nullptr);
    }
    ~DepthReadScope(){
        if(!context)return;
        UINT keep[D3D11_1_UAV_SLOT_COUNT];for(auto& value:keep)value=~0u;
        context->OMSetRenderTargetsAndUnorderedAccessViews(count,targets,depth,count,uavCount-count,uavs+count,keep);
        for(auto* target:targets)if(target)target->Release();if(depth)depth->Release();
        for(auto* uav:uavs)if(uav)uav->Release();
    }
    DepthReadScope(const DepthReadScope&)=delete;
    DepthReadScope& operator=(const DepthReadScope&)=delete;
};
}
