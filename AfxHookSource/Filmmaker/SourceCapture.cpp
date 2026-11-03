#include "stdafx.h"

#include "SourceCapture.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>

#include <d3d9.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace SourceFilmmaker {

namespace {

std::mutex g_mutex;
std::string g_path; // empty when nothing is asked for

template <typename T>
void Release(T*& p) {
	if (p) p->Release();
	p = nullptr;
}

// 24-bit bottom-up BMP from 32-bit BGRX rows.
bool WriteBmp(const std::string& path, const unsigned char* bits, int pitch, int width, int height) {
	const int rowBytes = (width * 3 + 3) & ~3;
	BITMAPFILEHEADER file = {};
	BITMAPINFOHEADER info = {};
	file.bfType = 0x4D42;
	file.bfOffBits = sizeof(file) + sizeof(info);
	file.bfSize = file.bfOffBits + (DWORD)rowBytes * height;
	info.biSize = sizeof(info);
	info.biWidth = width;
	info.biHeight = height;
	info.biPlanes = 1;
	info.biBitCount = 24;
	info.biCompression = BI_RGB;
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
	fwrite(&file, sizeof(file), 1, f);
	fwrite(&info, sizeof(info), 1, f);
	std::vector<unsigned char> row((size_t)rowBytes, 0);
	for (int y = height - 1; y >= 0; --y) {
		const unsigned char* src = bits + (size_t)y * pitch;
		for (int x = 0; x < width; ++x) {
			row[(size_t)x * 3 + 0] = src[x * 4 + 0];
			row[(size_t)x * 3 + 1] = src[x * 4 + 1];
			row[(size_t)x * 3 + 2] = src[x * 4 + 2];
		}
		fwrite(row.data(), 1, row.size(), f);
	}
	const bool ok = ferror(f) == 0;
	fclose(f);
	return ok;
}

std::string Capture(IDirect3DDevice9* device, const std::string& path) {
	IDirect3DSurface9 *back = nullptr, *resolved = nullptr, *system = nullptr;
	std::string error;
	D3DSURFACE_DESC desc = {};
	if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || FAILED(back->GetDesc(&desc))) {
		error = "no back buffer";
	} else if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) {
		error = "back buffer format " + std::to_string((int)desc.Format) + " is not 32-bit RGB";
	} else {
		resolved = back;
		resolved->AddRef();
		if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) { // resolve the samples first
			Release(resolved);
			if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr))
				|| FAILED(device->StretchRect(back, nullptr, resolved, nullptr, D3DTEXF_NONE)))
				error = "cannot resolve the multisampled back buffer";
		}
		D3DLOCKED_RECT locked = {};
		if (error.empty() && (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &system, nullptr))
			|| FAILED(device->GetRenderTargetData(resolved, system)) || FAILED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY))))
			error = "cannot read the back buffer";
		if (error.empty()) {
			if (!WriteBmp(path, (const unsigned char*)locked.pBits, locked.Pitch, (int)desc.Width, (int)desc.Height))
				error = "cannot write " + path;
			system->UnlockRect();
		}
	}
	Release(system);
	Release(resolved);
	Release(back);
	return error.empty() ? "captured " + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + " to " + path : error;
}

} // namespace

void Capture_Request(const char* path) {
	if (!path || !*path) {
		advancedfx::Message("mirv_filmmaker capture <file.bmp>: writes the next presented frame, the filmmaker's UI included.\n");
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_path = path;
	advancedfx::Message("capture: the next frame goes to %s\n", path);
}

void Capture_RenderThread(IDirect3DDevice9* device) {
	std::string path;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_path.empty()) return;
		path.swap(g_path);
	}
	const std::string result = device ? Capture(device, path) : std::string("no device");
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("capture", "%s", result.c_str());
}

} // namespace SourceFilmmaker
