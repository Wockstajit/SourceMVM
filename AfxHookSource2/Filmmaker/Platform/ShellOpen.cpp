#include "ShellOpen.h"

#include <Windows.h>
#include <shellapi.h> // ShellExecuteW
#include <shlobj.h>   // SHOpenFolderAndSelectItems / ILCreateFromPathW
#include <exdisp.h>   // IShellWindows / IWebBrowser2, to re-point an Explorer window already open

#include <atomic>
#include <cwctype>
#include <filesystem>
#include <string>
#include <thread>

namespace Filmmaker {

// SHOWING A FILE IN EXPLORER, PROPERLY.
//
// Three attempts got this wrong, each for its own reason, and all three came from shelling out to
// `explorer.exe /select,...` instead of asking the shell:
//
//   * ShellExecuteW reported success (>32) and opened NOTHING. Its dispatch depends on the
//     calling thread's COM state, and inside an injected DLL that is whatever the game left it
//     as -- not something this code gets to assume.
//   * CreateProcessW did open a window, but always a NEW one, and always behind the game.
//   * Raising it by window title is guesswork: Explorer titles a folder window "<name> - File
//     Explorer" unless the user turned full paths on, and matching loosely would yank an
//     unrelated window forward.
//
// The shell has an API for exactly this job. SHOpenFolderAndSelectItems is what Explorer's own
// "show in folder" uses: it REUSES a window already showing that folder instead of stacking up
// duplicates, selects the file inside it, and activates it. For a window open on a DIFFERENT
// folder, IShellWindows + IWebBrowser2::Navigate2 re-points it at the demo's folder first, so an
// Explorer you already had open follows you to the next demo instead of sitting there stale.
//
// COM is initialised ON THIS THREAD, by us, which is the part that makes it reliable: the whole
// sequence runs on a detached worker owning its own apartment, so nothing depends on the game's
// threading state and nothing blocks the main thread waiting on another process.
namespace {

// Point an ALREADY-OPEN Explorer window at `folder`. Returns true when one was navigated; the
// caller then lets SHOpenFolderAndSelectItems find that window and select the file inside it.
bool NavigateExistingExplorer(const std::wstring& folder) {
	IShellWindows* windows = nullptr;
	if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))) || !windows)
		return false;
	bool navigated = false;
	long count = 0;
	if (SUCCEEDED(windows->get_Count(&count))) {
		for (long i = 0; i < count && !navigated; ++i) {
			VARIANT idx; VariantInit(&idx); idx.vt = VT_I4; idx.lVal = i;
			IDispatch* disp = nullptr;
			const HRESULT itemHr = windows->Item(idx, &disp);
			VariantClear(&idx);
			if (FAILED(itemHr) || !disp) continue;
			IWebBrowser2* browser = nullptr;
			if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&browser))) && browser) {
				// Explorer folder windows and browser windows share this collection; a folder
				// window is the one whose top-level class is CabinetWClass.
				SHANDLE_PTR raw = 0;
				wchar_t cls[64] = {};
				if (SUCCEEDED(browser->get_HWND(&raw)) && raw)
					GetClassNameW(reinterpret_cast<HWND>(raw), cls, 63);
				if (0 == _wcsicmp(cls, L"CabinetWClass")) {
					VARIANT url; VariantInit(&url);
					url.vt = VT_BSTR;
					url.bstrVal = SysAllocString(folder.c_str());
					if (url.bstrVal)
						navigated = SUCCEEDED(browser->Navigate2(&url, nullptr, nullptr, nullptr, nullptr));
					VariantClear(&url);   // frees the BSTR
				}
				browser->Release();
			}
			disp->Release();
		}
	}
	windows->Release();
	return navigated;
}

// Last-resort raise. SHOpenFolderAndSelectItems normally activates the window itself once
// AllowSetForegroundWindow has granted the right, but a window that was already open and merely
// navigated does not always come forward -- and SetForegroundWindow alone is refused across
// processes. AttachThreadInput is the documented way out: borrow the target thread's input queue
// for the duration of the call, so Windows treats the request as coming from that thread.
void ForceRaiseExplorer() {
	const HWND cab = FindWindowW(L"CabinetWClass", nullptr);
	if (!cab) return;
	if (IsIconic(cab)) ShowWindow(cab, SW_RESTORE);
	const HWND fg = GetForegroundWindow();
	const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
	const DWORD own = GetCurrentThreadId();
	const DWORD tgt = GetWindowThreadProcessId(cab, nullptr);
	if (fgThread && fgThread != own) AttachThreadInput(own, fgThread, TRUE);
	if (tgt && tgt != own) AttachThreadInput(own, tgt, TRUE);
	BringWindowToTop(cab);
	SetForegroundWindow(cab);
	SetActiveWindow(cab);
	if (tgt && tgt != own) AttachThreadInput(own, tgt, FALSE);
	if (fgThread && fgThread != own) AttachThreadInput(own, fgThread, FALSE);
}

} // namespace

void ShowInExplorerAsync(const std::wstring& filePath) {
	static std::atomic_bool s_busy{ false };
	bool expected = false;
	if (!s_busy.compare_exchange_strong(expected, true))
		return; // one in flight is enough; a double click must not stack threads
	std::thread([filePath] {
		// OUR apartment, not the game's. This is the part the ShellExecute attempt was missing.
		const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		const bool ownInit = SUCCEEDED(initHr);
		{
			const std::wstring folder = std::filesystem::path(filePath).parent_path().wstring();
			// Re-point a window that is open on something else, so "show in Explorer" updates the
			// Explorer you already have rather than ignoring it.
			NavigateExistingExplorer(folder);
			PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(filePath.c_str());
			if (pidl) {
				// 0 items plus the FILE's pidl is the documented form for "open the parent folder
				// and select this one": it reuses a window already showing that folder.
				SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
				ILFree(pidl);
			}
			Sleep(250);           // let the window exist/settle before trying to raise it
			ForceRaiseExplorer();
		}
		if (ownInit) CoUninitialize();
		s_busy.store(false);
	}).detach();
}

// A URL goes through ShellExecuteW, which is exactly what failed for Explorer above when it ran
// on the game's thread. On a worker with its own apartment it opens the default browser.
void OpenUrlAsync(const std::wstring& url) {
	std::wstring lower = url;
	for (wchar_t& c : lower) c = wchar_t(std::towlower(c));
	if (lower.rfind(L"https://", 0) != 0 && lower.rfind(L"http://", 0) != 0)
		return; // never hand the shell a path or a protocol it would execute
	// The click gave us the foreground; pass that right on so the browser comes up in front.
	AllowSetForegroundWindow(ASFW_ANY);
	std::thread([url] {
		const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		if (SUCCEEDED(initHr)) CoUninitialize();
	}).detach();
}

bool CopyTextToClipboard(const std::wstring& text) {
	if (!OpenClipboard(nullptr)) return false;
	bool ok = false;
	EmptyClipboard();
	const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
		if (void* dst = GlobalLock(mem)) {
			memcpy(dst, text.c_str(), bytes);
			GlobalUnlock(mem);
			ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
		}
		if (!ok) GlobalFree(mem); // on success the clipboard owns it
	}
	CloseClipboard();
	return ok;
}

} // namespace Filmmaker
