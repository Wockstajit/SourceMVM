#include "DemoFilePicker.h"

#include <Windows.h>
#include <shobjidl.h>
#include <objbase.h>

namespace Filmmaker {

bool PickDemoFilesBlocking(std::vector<std::wstring>& out, const wchar_t* title, const wchar_t* filterCaption) {
	out.clear();

	const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	const bool needUninit = SUCCEEDED(coInit);

	bool result = false;
	IFileOpenDialog* dialog = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&dialog));
	if (SUCCEEDED(hr) && dialog) {
		DWORD options = 0;
		dialog->GetOptions(&options);
		dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
		dialog->SetTitle(title);

		// A second "All files" entry on purpose: demos that arrived with an odd extension
		// still read fine, and a filter the user cannot escape is worse than one they can.
		const COMDLG_FILTERSPEC filters[] = {
			{ filterCaption, L"*.dem" },
			{ L"All files", L"*.*" },
		};
		dialog->SetFileTypes((UINT)(sizeof(filters) / sizeof(filters[0])), filters);
		dialog->SetFileTypeIndex(1);
		dialog->SetDefaultExtension(L"dem");

		hr = dialog->Show(nullptr);
		if (SUCCEEDED(hr)) {
			IShellItemArray* items = nullptr;
			if (SUCCEEDED(dialog->GetResults(&items)) && items) {
				DWORD count = 0;
				items->GetCount(&count);
				for (DWORD i = 0; i < count; ++i) {
					IShellItem* item = nullptr;
					if (FAILED(items->GetItemAt(i, &item)) || !item)
						continue;
					PWSTR path = nullptr;
					if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
						out.push_back(path);
						CoTaskMemFree(path);
					}
					item->Release();
				}
				items->Release();
			}
			result = !out.empty();
		}
		dialog->Release();
	}

	if (needUninit)
		CoUninitialize();
	return result;
}

} // namespace Filmmaker
