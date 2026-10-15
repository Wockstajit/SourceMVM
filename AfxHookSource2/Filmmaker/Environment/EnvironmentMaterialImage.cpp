#define NOMINMAX
#include "EnvironmentMaterialMath.h"
#include "../Debug/MvmDebugLog.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
namespace Filmmaker::Environment {
namespace {
using Microsoft::WRL::ComPtr;
// Decodes the first frame of whatever `open` gives WIC (a file or an in-memory stream) to RGBA8.
template <class Open>
bool DecodeWithWic(Open open, MaterialImage &image, std::string &error) {
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
        error = "Could not initialize image decoder";
        return false;
    }
    struct Apartment {
        bool owns;
        ~Apartment() {
            if (owns)
                CoUninitialize();
        }
    } scope{SUCCEEDED(apartment)};
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<IWICStream> stream; // kept alive while the decoder reads from it
    MaterialImage pending;
    HRESULT hr =
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr))
        hr = open(factory.Get(), stream, decoder);
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr))
        hr = frame->GetSize(&pending.width, &pending.height);
    if (FAILED(hr) || !pending.width || !pending.height || pending.width > 4096 || pending.height > 4096) {
        error = "Texture must be a readable image no larger than 4096 x 4096";
        return false;
    }
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                   nullptr, 0, WICBitmapPaletteTypeCustom);
    pending.rgba.resize(size_t(pending.width) * pending.height * 4);
    if (SUCCEEDED(hr))
        hr =
            converter->CopyPixels(nullptr, pending.width * 4, UINT(pending.rgba.size()), pending.rgba.data());
    if (FAILED(hr)) {
        error = "Texture RGBA conversion failed";
        return false;
    }
    MvmDebugLog_LinefAlways("environment.material", "decoded=1 width=%u height=%u bytes=%zu", pending.width,
                            pending.height, pending.rgba.size());
    image = std::move(pending);
    error.clear();
    return true;
}
} // namespace
bool LoadMaterialImage(const std::wstring &path, MaterialImage &image, std::string &error) {
    return DecodeWithWic([&](IWICImagingFactory *factory, ComPtr<IWICStream> &, ComPtr<IWICBitmapDecoder> &decoder) {
        return factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                  &decoder);
    }, image, error);
}
bool LoadMaterialImageFromMemory(const unsigned char *data, size_t size, MaterialImage &image, std::string &error) {
    if (!data || !size || size > 64u * 1024 * 1024) {
        error = "Embedded texture missing or larger than 64 MB";
        return false;
    }
    return DecodeWithWic([&](IWICImagingFactory *factory, ComPtr<IWICStream> &stream, ComPtr<IWICBitmapDecoder> &decoder) {
        HRESULT hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr))
            hr = stream->InitializeFromMemory(const_cast<BYTE *>(data), DWORD(size));
        if (SUCCEEDED(hr))
            hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        return hr;
    }, image, error);
}
} // namespace Filmmaker::Environment
