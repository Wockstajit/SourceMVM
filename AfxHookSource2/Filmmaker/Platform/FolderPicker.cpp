#include "FolderPicker.h"

#include <Windows.h>
#include <shobjidl.h>
#include <objbase.h>

namespace Filmmaker {

bool PickFolderBlocking(std::wstring& out, const wchar_t* title) {
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
		dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
		dialog->SetTitle(title);

		hr = dialog->Show(nullptr);
		if (SUCCEEDED(hr)) {
			IShellItem* item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item)) && item) {
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
					out = path;
					result = true;
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		dialog->Release();
	}

	if (needUninit)
		CoUninitialize();
	return result;
}

} // namespace Filmmaker
