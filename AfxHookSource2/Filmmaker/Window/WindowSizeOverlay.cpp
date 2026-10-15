#include "WindowSizeOverlay.h"

#include <windows.h>

#include <string>

namespace Filmmaker {

namespace {

const wchar_t* const kClassName = L"SourceMvmWindowSizeOverlay";
constexpr UINT_PTR kHideTimerId = 1;

// Design sizes at 96 DPI; scaled by the anchor window's DPI at show time.
// Wide enough for the longest secondary line the readout produces, which is the un-snapped case
// with the hint: "7:5 . closest 1.43:1 (-2.1%) . SHIFT to snap".
constexpr int kBaseWidth = 380;
constexpr int kBaseHeight = 104;
constexpr int kBaseCorner = 14;
constexpr int kBasePrimaryFont = 34;
constexpr int kBaseSecondaryFont = 16;

struct Overlay {
	HWND hwnd = nullptr;
	ATOM classAtom = 0;
	bool visible = false;
	ULONGLONG hideAt = 0;
	int dpi = 96;
	HFONT primaryFont = nullptr;
	HFONT secondaryFont = nullptr;
	int fontDpi = 0;
	std::wstring primary;
	std::wstring secondary;
};

Overlay& Get() {
	static Overlay s_overlay;
	return s_overlay;
}

int Scaled(int value, int dpi) {
	return MulDiv(value, dpi, 96);
}

std::wstring Widen(const std::string& utf8) {
	if (utf8.empty())
		return std::wstring();
	const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
	if (needed <= 0)
		return std::wstring();
	std::wstring out((size_t)needed, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &out[0], needed);
	return out;
}

// GetDpiForWindow is Windows 10 1607+. Resolved dynamically so the DLL still loads on anything
// older; the fallback is the desktop DC, which is correct for a single-monitor 96 DPI setup and
// merely imprecise otherwise.
int DpiForWindow(HWND hwnd) {
	using Fn = UINT (WINAPI*)(HWND);
	static Fn s_getDpiForWindow = nullptr;
	static bool s_resolved = false;
	if (!s_resolved) {
		s_resolved = true;
		if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
			s_getDpiForWindow = (Fn)GetProcAddress(user32, "GetDpiForWindow");
	}
	if (s_getDpiForWindow && hwnd) {
		const UINT dpi = s_getDpiForWindow(hwnd);
		if (dpi >= 72 && dpi <= 480)
			return (int)dpi;
	}
	if (HDC screen = GetDC(nullptr)) {
		const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
		ReleaseDC(nullptr, screen);
		if (dpi >= 72 && dpi <= 480)
			return dpi;
	}
	return 96;
}

void EnsureFonts(Overlay& overlay) {
	if (overlay.fontDpi == overlay.dpi && overlay.primaryFont && overlay.secondaryFont)
		return;

	if (overlay.primaryFont) { DeleteObject(overlay.primaryFont); overlay.primaryFont = nullptr; }
	if (overlay.secondaryFont) { DeleteObject(overlay.secondaryFont); overlay.secondaryFont = nullptr; }

	auto make = [&](int basePx, int weight) -> HFONT {
		return CreateFontW(-Scaled(basePx, overlay.dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
	};
	overlay.primaryFont = make(kBasePrimaryFont, FW_SEMIBOLD);
	overlay.secondaryFont = make(kBaseSecondaryFont, FW_NORMAL);
	overlay.fontDpi = overlay.dpi;
}

void Paint(HWND hwnd) {
	Overlay& overlay = Get();

	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);
	if (!hdc)
		return;

	RECT client;
	GetClientRect(hwnd, &client);
	const int width = client.right - client.left;
	const int height = client.bottom - client.top;

	// Double buffered: the overlay repaints on every WM_SIZING of a drag, and a direct-to-screen
	// fill would flicker badly at that rate.
	HDC mem = CreateCompatibleDC(hdc);
	HBITMAP bitmap = CreateCompatibleBitmap(hdc, width, height);
	HGDIOBJ oldBitmap = SelectObject(mem, bitmap);

	HBRUSH background = CreateSolidBrush(RGB(16, 17, 20));
	FillRect(mem, &client, background);
	DeleteObject(background);

	HPEN border = CreatePen(PS_SOLID, 1, RGB(72, 76, 84));
	HGDIOBJ oldPen = SelectObject(mem, border);
	HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
	Rectangle(mem, 0, 0, width, height);
	SelectObject(mem, oldBrush);
	SelectObject(mem, oldPen);
	DeleteObject(border);

	EnsureFonts(overlay);
	SetBkMode(mem, TRANSPARENT);

	const int pad = Scaled(10, overlay.dpi);
	RECT primaryRect = { pad, pad, width - pad, height / 2 + Scaled(10, overlay.dpi) };
	HGDIOBJ oldFont = SelectObject(mem, overlay.primaryFont);
	SetTextColor(mem, RGB(240, 242, 245));
	DrawTextW(mem, overlay.primary.c_str(), (int)overlay.primary.size(), &primaryRect,
		DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

	RECT secondaryRect = { pad, primaryRect.bottom, width - pad, height - pad };
	SelectObject(mem, overlay.secondaryFont);
	SetTextColor(mem, RGB(158, 165, 176));
	DrawTextW(mem, overlay.secondary.c_str(), (int)overlay.secondary.size(), &secondaryRect,
		DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
	SelectObject(mem, oldFont);

	BitBlt(hdc, 0, 0, width, height, mem, 0, 0, SRCCOPY);

	SelectObject(mem, oldBitmap);
	DeleteObject(bitmap);
	DeleteDC(mem);
	EndPaint(hwnd, &ps);
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_PAINT:
		Paint(hwnd);
		return 0;
	case WM_ERASEBKGND:
		return 1; // fully painted in WM_PAINT
	case WM_TIMER:
		if (wParam == kHideTimerId) {
			KillTimer(hwnd, kHideTimerId);
			WindowSizeOverlay_Hide();
			return 0;
		}
		break;
	case WM_NCHITTEST:
		return HTTRANSPARENT; // never steal a click from the game window underneath
	case WM_DESTROY:
		Get().hwnd = nullptr;
		Get().visible = false;
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HINSTANCE SelfModule() {
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)&OverlayWndProc, &self);
	return (HINSTANCE)self;
}

bool EnsureWindow(Overlay& overlay) {
	if (overlay.hwnd)
		return true;

	HINSTANCE instance = SelfModule();
	if (!overlay.classAtom) {
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &OverlayWndProc;
		wc.hInstance = instance;
		wc.hCursor = nullptr;
		wc.hbrBackground = nullptr;
		wc.lpszClassName = kClassName;
		overlay.classAtom = RegisterClassExW(&wc);
		if (!overlay.classAtom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return false;
	}

	overlay.hwnd = CreateWindowExW(
		WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
		kClassName, L"", WS_POPUP,
		0, 0, Scaled(kBaseWidth, overlay.dpi), Scaled(kBaseHeight, overlay.dpi),
		nullptr, nullptr, instance, nullptr);
	if (!overlay.hwnd)
		return false;

	// Constant alpha rather than per-pixel: GDI text drawing zeroes the alpha channel, so a
	// per-pixel layered window would need the whole "premultiply and repair alpha" dance for no
	// visible gain on an opaque card.
	SetLayeredWindowAttributes(overlay.hwnd, 0, 235, LWA_ALPHA);
	return true;
}

void ApplyRoundedRegion(Overlay& overlay, int width, int height) {
	const int corner = Scaled(kBaseCorner, overlay.dpi);
	HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, corner, corner);
	if (region)
		SetWindowRgn(overlay.hwnd, region, FALSE); // window owns the region now
}

} // namespace

void WindowSizeOverlay_Show(void* anchorHwnd, int centerX, int centerY,
	const std::string& primary, const std::string& secondary) {
	Overlay& overlay = Get();
	HWND anchor = (HWND)anchorHwnd;

	overlay.dpi = DpiForWindow(anchor);
	if (!EnsureWindow(overlay))
		return;

	overlay.hideAt = 0;
	overlay.primary = Widen(primary);
	overlay.secondary = Widen(secondary);

	const int width = Scaled(kBaseWidth, overlay.dpi);
	const int height = Scaled(kBaseHeight, overlay.dpi);

	const int x = centerX - width / 2;
	const int y = centerY - height / 2;

	KillTimer(overlay.hwnd, kHideTimerId);
	SetWindowPos(overlay.hwnd, HWND_TOPMOST, x, y, width, height,
		SWP_NOACTIVATE | (overlay.visible ? 0u : SWP_SHOWWINDOW));
	ApplyRoundedRegion(overlay, width, height);
	overlay.visible = true;

	// Synchronous repaint: inside a modal resize loop there is no pump of ours to come back to.
	RedrawWindow(overlay.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

void WindowSizeOverlay_MoveTo(int centerX, int centerY) {
	Overlay& overlay = Get();
	if (!overlay.hwnd || !overlay.visible)
		return;

	const int width = Scaled(kBaseWidth, overlay.dpi);
	const int height = Scaled(kBaseHeight, overlay.dpi);

	// Position only: SWP_NOSIZE keeps the card as it is, and the hide timer is deliberately left
	// running -- moving the window is not a reason for the readout to live longer.
	SetWindowPos(overlay.hwnd, HWND_TOPMOST, centerX - width / 2, centerY - height / 2, 0, 0,
		SWP_NOACTIVATE | SWP_NOSIZE);
}

void WindowSizeOverlay_HideAfter(unsigned holdMs) {
	Overlay& overlay = Get();
	if (!overlay.hwnd || !overlay.visible)
		return;
	overlay.hideAt = GetTickCount64() + holdMs;
	SetTimer(overlay.hwnd, kHideTimerId, holdMs, nullptr);
}

void WindowSizeOverlay_Pump() {
	Overlay& overlay = Get();
	// SDL may only dispatch messages for its own window after the modal loop.
	// Keep the same deadline on the main-frame pump as well as the native timer.
	if (overlay.visible && overlay.hideAt && GetTickCount64() >= overlay.hideAt)
		WindowSizeOverlay_Hide();
}

void WindowSizeOverlay_Hide() {
	Overlay& overlay = Get();
	if (!overlay.hwnd)
		return;
	KillTimer(overlay.hwnd, kHideTimerId);
	overlay.hideAt = 0;
	ShowWindow(overlay.hwnd, SW_HIDE);
	overlay.visible = false;
}

bool WindowSizeOverlay_Visible() {
	return Get().visible;
}

void WindowSizeOverlay_Shutdown() {
	Overlay& overlay = Get();
	if (overlay.hwnd) {
		KillTimer(overlay.hwnd, kHideTimerId);
		DestroyWindow(overlay.hwnd);
		overlay.hwnd = nullptr;
	}
	overlay.visible = false;
	if (overlay.primaryFont) { DeleteObject(overlay.primaryFont); overlay.primaryFont = nullptr; }
	if (overlay.secondaryFont) { DeleteObject(overlay.secondaryFont); overlay.secondaryFont = nullptr; }
	overlay.fontDpi = 0;
	if (overlay.classAtom) {
		UnregisterClassW(kClassName, SelfModule());
		overlay.classAtom = 0;
	}
}

} // namespace Filmmaker
