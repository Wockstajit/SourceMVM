#include "CustomShadowAtlas.h"

#include "DynamicShadowFx.h"
#include "../Debug/MvmDebugLog.h"

#include <atomic>
#include <mutex>

namespace Filmmaker {
namespace {

std::atomic<bool> g_active{ false };
std::atomic<bool> g_fillBorrow{ false };
std::atomic<unsigned long long> g_captures{ 0 };

std::mutex g_mutex;
ID3D11Device* g_device = nullptr;
ID3D11Texture2D* g_atlasCopy = nullptr;
ID3D11ShaderResourceView* g_atlasSrv = nullptr;
ID3D11DepthStencilView* g_atlasClearDsv = nullptr;
ID3D11Texture2D* g_trackedAtlas = nullptr; // non-owning
UINT g_width = 0;
UINT g_height = 0;
bool g_boundToAtlas = false;

bool IsCsmAtlasTexture(ID3D11Texture2D* tex, D3D11_TEXTURE2D_DESC* outDesc = nullptr) {
	if (!tex) return false;
	D3D11_TEXTURE2D_DESC desc = {};
	tex->GetDesc(&desc);
	if (outDesc) *outDesc = desc;
	// Proven Mirage live CSM atlas: 4352x5248 R16_TYPELESS, single sample.
	// Also accept nearby atlas sizes so a quality preset change does not kill capture.
	if (desc.SampleDesc.Count != 1) return false;
	if (desc.Format != DXGI_FORMAT_R16_TYPELESS
		&& desc.Format != DXGI_FORMAT_R16_UNORM
		&& desc.Format != DXGI_FORMAT_D16_UNORM)
		return false;
	if (desc.Width < 2048 || desc.Height < 2048) return false;
	if (desc.Width > 8192 || desc.Height > 8192) return false;
	// Prefer the known layout; still accept other large R16 depth atlases.
	return true;
}

void ReleaseCopy_NoLock() {
	if (g_atlasClearDsv) { g_atlasClearDsv->Release(); g_atlasClearDsv = nullptr; }
	if (g_atlasSrv) { g_atlasSrv->Release(); g_atlasSrv = nullptr; }
	if (g_atlasCopy) { g_atlasCopy->Release(); g_atlasCopy = nullptr; }
	g_width = g_height = 0;
}

bool EnsureCopy_NoLock(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& srcDesc) {
	if (!g_device || !src) return false;
	if (g_atlasCopy && g_width == srcDesc.Width && g_height == srcDesc.Height)
		return true;
	ReleaseCopy_NoLock();

	D3D11_TEXTURE2D_DESC td = srcDesc;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.MiscFlags = 0;
	td.CPUAccessFlags = 0;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
	td.Format = DXGI_FORMAT_R16_TYPELESS;
	if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_atlasCopy)) || !g_atlasCopy)
		return false;

	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = DXGI_FORMAT_R16_UNORM;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	if (FAILED(g_device->CreateShaderResourceView(g_atlasCopy, &sd, &g_atlasSrv)) || !g_atlasSrv) {
		ReleaseCopy_NoLock();
		return false;
	}

	D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
	dd.Format = DXGI_FORMAT_D16_UNORM;
	dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	if (FAILED(g_device->CreateDepthStencilView(g_atlasCopy, &dd, &g_atlasClearDsv))) {
		// Clear uses the engine atlas DSV when available; copy DSV is optional.
		g_atlasClearDsv = nullptr;
	}

	g_width = srcDesc.Width;
	g_height = srcDesc.Height;
	return true;
}

void CaptureAndSuppress(
	ID3D11DeviceContext* context,
	ID3D11Texture2D* atlasTex,
	ID3D11DepthStencilView* atlasDsv,
	const D3D11_TEXTURE2D_DESC& desc) {
	if (!context || !atlasTex || !g_active.load(std::memory_order_relaxed)) return;

	std::lock_guard<std::mutex> lock(g_mutex);
	if (!EnsureCopy_NoLock(atlasTex, desc)) return;

	context->CopyResource(g_atlasCopy, atlasTex);

	// Suppress engine shadow sampling for this frame: clear the live atlas after we copied it.
	if (atlasDsv) {
		context->ClearDepthStencilView(atlasDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
	}

	g_trackedAtlas = atlasTex;
	const unsigned long long n = g_captures.fetch_add(1, std::memory_order_relaxed) + 1;
	if (MvmDebugLog_Active() && (n <= 3ull || (n % 120ull) == 0ull)) {
		MvmDebugLog_LinefAlways("world.customshadow.atlas",
			"capture n=%llu tex=%p %ux%u fmt=%u cleared=1",
			n, (void*)atlasTex, desc.Width, desc.Height, (unsigned)desc.Format);
	}
}

} // namespace

void CustomShadowAtlas_SetActive(bool active) {
	g_active.store(active, std::memory_order_release);
	if (!active) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_boundToAtlas = false;
	}
}

bool CustomShadowAtlas_Active() {
	return g_active.load(std::memory_order_acquire);
}

void CustomShadowAtlas_SetFillBorrow(bool borrow) {
	g_fillBorrow.store(borrow, std::memory_order_release);
	DynamicShadowFx_SetFillBorrow(borrow);
}

bool CustomShadowAtlas_FillBorrow() {
	return g_fillBorrow.load(std::memory_order_acquire);
}

void CustomShadowAtlas_BeginDevice(ID3D11Device* device) {
	std::lock_guard<std::mutex> lock(g_mutex);
	ReleaseCopy_NoLock();
	if (g_device) { g_device->Release(); g_device = nullptr; }
	g_trackedAtlas = nullptr;
	g_boundToAtlas = false;
	if (device) {
		device->AddRef();
		g_device = device;
	}
}

void CustomShadowAtlas_EndDevice() {
	std::lock_guard<std::mutex> lock(g_mutex);
	ReleaseCopy_NoLock();
	if (g_device) { g_device->Release(); g_device = nullptr; }
	g_trackedAtlas = nullptr;
	g_boundToAtlas = false;
}

void CustomShadowAtlas_NoteRenderTargets(
	ID3D11DeviceContext* context,
	UINT numViews,
	ID3D11RenderTargetView* const* renderTargetViews,
	ID3D11DepthStencilView* depthStencilView) {
	(void)numViews;
	(void)renderTargetViews;
	if (!g_active.load(std::memory_order_relaxed) || !context || !g_device)
		return;
	if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
		return;

	ID3D11Texture2D* dsvTex = nullptr;
	D3D11_TEXTURE2D_DESC desc = {};
	bool isAtlas = false;
	if (depthStencilView) {
		ID3D11Resource* res = nullptr;
		depthStencilView->GetResource(&res);
		if (res) {
			res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&dsvTex);
			res->Release();
		}
		if (dsvTex) isAtlas = IsCsmAtlasTexture(dsvTex, &desc);
	}

	bool wasBound = false;
	ID3D11Texture2D* prev = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		wasBound = g_boundToAtlas;
		prev = g_trackedAtlas;
		if (isAtlas) {
			g_boundToAtlas = true;
			g_trackedAtlas = dsvTex;
		} else if (g_boundToAtlas) {
			g_boundToAtlas = false;
		}
	}

	// Transition: were drawing into the atlas, now binding something else → capture+clear.
	if (wasBound && !isAtlas && prev) {
		D3D11_TEXTURE2D_DESC prevDesc = {};
		prev->GetDesc(&prevDesc);
		// Re-acquire a DSV on the previous atlas for clear. The outgoing bind already
		// released the previous OM DSV; create a transient DSV for Clear.
		ID3D11DepthStencilView* clearDsv = nullptr;
		D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
		dd.Format = DXGI_FORMAT_D16_UNORM;
		dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		if (SUCCEEDED(g_device->CreateDepthStencilView(prev, &dd, &clearDsv)) && clearDsv) {
			CaptureAndSuppress(context, prev, clearDsv, prevDesc);
			clearDsv->Release();
		} else {
			CaptureAndSuppress(context, prev, nullptr, prevDesc);
		}
	}

	if (dsvTex) dsvTex->Release();
}

ID3D11ShaderResourceView* CustomShadowAtlas_Srv() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_atlasSrv;
}

bool CustomShadowAtlas_HaveCapture() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_captures.load(std::memory_order_relaxed) > 0 && g_atlasSrv != nullptr;
}

UINT CustomShadowAtlas_Width() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_width;
}

UINT CustomShadowAtlas_Height() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_height;
}

unsigned long long CustomShadowAtlas_CaptureCount() {
	return g_captures.load(std::memory_order_relaxed);
}

} // namespace Filmmaker
