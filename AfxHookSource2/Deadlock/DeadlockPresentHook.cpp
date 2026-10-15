#include "DeadlockPresentHook.h"

#include "DeadlockFrame.h"
#include "DeadlockViewportBlit.h"
#include "DeadlockOverlayDrawer.h"

#include "../../Shared/AfxConsole.h"
#include "../../Shared/AfxDetours.h"

#include <d3d11.h>
#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")

namespace Deadlock {

namespace {

bool g_installed = false;
void* g_swapChain = nullptr;

// IDXGISwapChain::Present is vtable slot 8: IUnknown takes 0-2, IDXGIObject 3-6,
// IDXGIDeviceSubObject 7, then Present. Fixed by the COM interface definition, not by a game
// build, which is the entire reason this hook needs no signature and no re-derivation.
const int kPresentSlot = 8;

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
Present_t g_originalPresent = nullptr;

HRESULT __stdcall New_Present(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
	// The device is only reachable once the game has a swapchain, and this is the first place we
	// see one. Done once: the blit's D3D objects outlive every frame, and recreating them per
	// Present would be the vertex-buffer leak all over again.
	if (swapChain != g_swapChain) {
		g_swapChain = swapChain;
		ID3D11Device* device = nullptr;
		if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device)) && device) {
			ViewportBlit_BeginDevice(device);
			OverlayDrawer_BeginDevice(device);
			device->Release();
		}
	}

	// Render-thread work only. No Panorama, no console command execution, nothing that assumes
	// the main thread.
	Frame_Render();

	return g_originalPresent ? g_originalPresent(swapChain, syncInterval, flags)
	                         : E_FAIL;
}

// Reads the Present entry out of a swap chain vtable we make ourselves.
//
// The alternative is to find the game's own swap chain, which means either hooking the DXGI
// factory before the game uses it or scanning rendersystemdx11.dll for the device object. Both are
// more fragile than this for no benefit: every IDXGISwapChain from the same runtime shares one
// vtable, so detouring the entry read from a throwaway chain also redirects the game's.
void* FindPresentEntry() {
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"AfxHookDeadlockProbe";
	if (!RegisterClassExW(&wc)) {
		// Already registered from an earlier attempt; that is fine.
		if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
	}

	HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
	                              0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
	if (!window) {
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return nullptr;
	}

	DXGI_SWAP_CHAIN_DESC desc = {};
	desc.BufferCount = 1;
	desc.BufferDesc.Width = 8;
	desc.BufferDesc.Height = 8;
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.OutputWindow = window;
	desc.SampleDesc.Count = 1;
	desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	IDXGISwapChain* chain = nullptr;
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;

	// BGRA support is deliberately not requested and no debug layer is enabled: this device exists
	// for one vtable read and is destroyed immediately.
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	HRESULT hr = D3D11CreateDeviceAndSwapChain(
		nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		levels, 2, D3D11_SDK_VERSION,
		&desc, &chain, &device, nullptr, &context);

	void* presentEntry = nullptr;
	if (SUCCEEDED(hr) && chain) {
		void** vtable = *(void***)chain;
		if (vtable) presentEntry = vtable[kPresentSlot];
	}

	if (context) context->Release();
	if (device) device->Release();
	if (chain) chain->Release();
	DestroyWindow(window);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);

	return presentEntry;
}

} // namespace

bool PresentHook_Install(HMODULE) {
	if (g_installed) return true;

	void* present = FindPresentEntry();
	if (!present) {
		advancedfx::Warning(
			"AfxHookDeadlock: could not read IDXGISwapChain::Present. The render pump will stay\n"
			"at zero, which 'mirv_filmmaker health' reports.\n");
		return false;
	}

	g_originalPresent = (Present_t)present;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_originalPresent, New_Present);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_originalPresent = nullptr;
		advancedfx::Warning("AfxHookDeadlock: could not detour Present.\n");
		return false;
	}

	g_installed = true;
	advancedfx::Message("AfxHookDeadlock: present hook installed.\n");
	return true;
}

bool PresentHook_Installed() { return g_installed; }
void* PresentHook_SwapChain() { return g_swapChain; }

} // namespace Deadlock
