#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "WorldDecals.h"

#include "../Environment/EnvironmentMaterialMath.h"
#include "../Environment/MapCollisionGrid.h"
#include "../Environment/EnvironmentScene.h"
#include "../Movie/CameraBridge.h"
#include "../Platform/JsonBuilder.h"
#include "../Platform/TextEncoding.h"
#include "../Debug/MvmDebugLog.h"
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"

#include <windows.h>
#include <commdlg.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#pragma comment(lib, "mfuuid.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace Filmmaker::WorldDecals {
using Microsoft::WRL::ComPtr;
namespace {

// ---- Dynamic Windows Media Foundation loader (for MP4/MOV/WMV/AVI videos on walls) ----------
struct MfApi {
	HMODULE mfplat = nullptr;
	HMODULE mfreadwrite = nullptr;
	HRESULT (WINAPI* Startup)(ULONG, DWORD) = nullptr;
	HRESULT (WINAPI* CreateAttributes)(IMFAttributes**, UINT32) = nullptr;
	HRESULT (WINAPI* CreateMediaType)(IMFMediaType**) = nullptr;
	HRESULT (WINAPI* CreateSourceReaderFromURL)(LPCWSTR, IMFAttributes*, IMFSourceReader**) = nullptr;
	bool ready = false;
	bool tried = false;

	bool Ensure() {
		if (tried) return ready;
		tried = true;
		mfplat = LoadLibraryW(L"mfplat.dll");
		mfreadwrite = LoadLibraryW(L"mfreadwrite.dll");
		if (!mfplat || !mfreadwrite) return false;
		Startup = reinterpret_cast<decltype(Startup)>(GetProcAddress(mfplat, "MFStartup"));
		CreateAttributes = reinterpret_cast<decltype(CreateAttributes)>(GetProcAddress(mfplat, "MFCreateAttributes"));
		CreateMediaType = reinterpret_cast<decltype(CreateMediaType)>(GetProcAddress(mfplat, "MFCreateMediaType"));
		CreateSourceReaderFromURL = reinterpret_cast<decltype(CreateSourceReaderFromURL)>(
			GetProcAddress(mfreadwrite, "MFCreateSourceReaderFromURL"));
		if (!Startup || !CreateAttributes || !CreateMediaType || !CreateSourceReaderFromURL) return false;
		HRESULT hr = Startup(MF_VERSION, MFSTARTUP_LITE);
		ready = SUCCEEDED(hr);
		return ready;
	}
} g_mf;

struct VideoDecoder {
	std::wstring path;
	ComPtr<IMFSourceReader> reader;
	uint32_t width = 0, height = 0;
	double durationSec = 0.0;
	double lastDecodedSec = -1e9;
	bool ok = false;
	std::shared_ptr<TextureBitmap> currentFrame;

	bool Open(const std::wstring& filePath) {
		Close();
		path = filePath;
		if (!g_mf.Ensure()) return false;
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		ComPtr<IMFAttributes> attrs;
		if (FAILED(g_mf.CreateAttributes(&attrs, 1))) return false;
		attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
		if (FAILED(g_mf.CreateSourceReaderFromURL(filePath.c_str(), attrs.Get(), &reader)) || !reader)
			return false;

		ComPtr<IMFMediaType> type;
		if (FAILED(g_mf.CreateMediaType(&type))) { Close(); return false; }
		type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
		if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get()))) {
			Close(); return false;
		}
		ComPtr<IMFMediaType> outType;
		if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &outType)) && outType) {
			UINT64 sizeVal = 0;
			if (SUCCEEDED(outType->GetUINT64(MF_MT_FRAME_SIZE, &sizeVal))) {
				width = (uint32_t)(sizeVal >> 32);
				height = (uint32_t)(sizeVal & 0xFFFFFFFFu);
			}
		}
		PROPVARIANT var;
		PropVariantInit(&var);
		if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))) {
			if (var.vt == VT_UI8) durationSec = (double)var.uhVal.QuadPart / 10000000.0;
			else if (var.vt == VT_I8) durationSec = (double)var.hVal.QuadPart / 10000000.0;
		}
		PropVariantClear(&var);
		if (width == 0 || height == 0 || width > 4096 || height > 4096) { Close(); return false; }
		ok = true;
		DecodeAt(0.0, 1);
		return ok;
	}

	void Close() {
		reader.Reset();
		currentFrame.reset();
		width = height = 0;
		durationSec = 0.0;
		lastDecodedSec = -1e9;
		ok = false;
	}

	// Decodes the video frame nearest `targetSec` into RGBA8.
	bool DecodeAt(double targetSec, uint64_t nextRevision) {
		if (!ok || !reader) return false;
		if (targetSec < 0.0) targetSec = 0.0;
		if (std::fabs(targetSec - lastDecodedSec) < 0.014 && currentFrame)
			return false; // within ~1 frame at 60fps

		// Seek if rewinding or jumping more than 0.25s forward.
		if (targetSec < lastDecodedSec - 0.005 || targetSec > lastDecodedSec + 0.25) {
			PROPVARIANT pos;
			PropVariantInit(&pos);
			pos.vt = VT_I8;
			pos.hVal.QuadPart = (LONGLONG)(targetSec * 10000000.0);
			reader->SetCurrentPosition(GUID_NULL, pos);
			PropVariantClear(&pos);
		}

		ComPtr<IMFSample> sample;
		DWORD flags = 0;
		LONGLONG ts = 0;
		for (int step = 0; step < 8; ++step) {
			sample.Reset();
			HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
			if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
			if (!sample) continue;
			const double sampleSec = (double)ts / 10000000.0;
			if (sampleSec + 0.016 < targetSec && step < 7) continue;
			ComPtr<IMFMediaBuffer> buf;
			if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf) break;
			BYTE* raw = nullptr;
			DWORD maxLen = 0, curLen = 0;
			if (FAILED(buf->Lock(&raw, &maxLen, &curLen)) || !raw) break;
			const size_t expected = (size_t)width * height * 4;
			if (curLen >= expected) {
				auto bmp = std::make_shared<TextureBitmap>();
				bmp->width = width;
				bmp->height = height;
				bmp->revision = nextRevision;
				bmp->rgba.resize(expected);
				// MFVideoFormat_RGB32 is BGRX; convert to RGBA with alpha = 255.
				for (size_t p = 0; p < expected; p += 4) {
					bmp->rgba[p + 0] = raw[p + 2];
					bmp->rgba[p + 1] = raw[p + 1];
					bmp->rgba[p + 2] = raw[p + 0];
					bmp->rgba[p + 3] = 255;
				}
				currentFrame = std::move(bmp);
				lastDecodedSec = sampleSec;
			}
			buf->Unlock();
			return true;
		}
		return false;
	}
};

// ---- Crisp GDI Text Rasterizer -> RGBA8 -----------------------------------------------------
std::shared_ptr<TextureBitmap> RasterizeTextDecal(const Decal& d, uint64_t rev) {
	const int W = 1024, H = 512;
	BITMAPINFO bmi{};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = W;
	bmi.bmiHeader.biHeight = -H; // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	HDC screenDc = GetDC(nullptr);
	HDC memDc = CreateCompatibleDC(screenDc);
	void* bits = nullptr;
	HBITMAP dib = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!dib || !bits) {
		if (dib) DeleteObject(dib);
		DeleteDC(memDc);
		ReleaseDC(nullptr, screenDc);
		return nullptr;
	}
	HGDIOBJ oldBmp = SelectObject(memDc, dib);
	std::memset(bits, 0, (size_t)W * H * 4);

	std::wstring wtext = Utf8ToWide(d.text.empty() ? "TEXT" : d.text);
	if (wtext.empty()) wtext = L"TEXT";

	const int pxHeight = std::clamp(d.fontSize * 2, 24, 340);
	HFONT font = CreateFontW(
		-pxHeight, 0, 0, 0, d.bold ? FW_HEAVY : FW_NORMAL,
		FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
		CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
	HGDIOBJ oldFont = SelectObject(memDc, font);
	SetBkMode(memDc, TRANSPARENT);

	RECT rc{ 32, 16, W - 32, H - 16 };
	// Measure first to vertically center multi-line text.
	RECT calc = rc;
	DrawTextW(memDc, wtext.c_str(), -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
	const int textH = (int)(calc.bottom - calc.top);
	const int yOff = (std::max)(0, ((int)(rc.bottom - rc.top) - textH) / 2);
	rc.top += yOff;

	// Render white mask into luminance (green channel) for crisp alpha extraction, plus optional outline.
	if (d.outline) {
		// Draw outline pass in blue channel (B=255, G=0) so we can composite a clean dark outline with alpha!
		SetTextColor(memDc, RGB(0, 0, 255));
		const int off = std::max(3, pxHeight / 26);
		const int dirs[8][2] = { {-1,0},{1,0},{0,-1},{0,1},{-1,-1},{1,-1},{-1,1},{1,1} };
		for (int k = 0; k < 8; ++k) {
			RECT orc = rc;
			OffsetRect(&orc, dirs[k][0] * off, dirs[k][1] * off);
			DrawTextW(memDc, wtext.c_str(), -1, &orc, DT_CENTER | DT_WORDBREAK);
		}
	}
	// Draw main text fill in white (R=255, G=255, B=255)
	SetTextColor(memDc, RGB(255, 255, 255));
	DrawTextW(memDc, wtext.c_str(), -1, &rc, DT_CENTER | DT_WORDBREAK);

	auto bmp = std::make_shared<TextureBitmap>();
	bmp->width = W;
	bmp->height = H;
	bmp->revision = rev;
	bmp->rgba.resize((size_t)W * H * 4);
	const uint8_t* src = static_cast<const uint8_t*>(bits);
	for (size_t i = 0; i < (size_t)W * H * 4; i += 4) {
		// DIB is BGRA: src[0]=B (outline+fill), src[1]=G (fill), src[2]=R (fill)
		const uint8_t fill = src[i + 1];
		const uint8_t outline = src[i + 0];
		const uint8_t alpha = std::max(fill, outline);
		// Fill pixels are bright white (tinted by decal.color in shader); outline pixels are dark!
		const uint8_t shade = alpha > 0 ? (uint8_t)std::min(255, (int)fill * 255 / std::max((int)alpha, 1)) : 0;
		bmp->rgba[i + 0] = shade;
		bmp->rgba[i + 1] = shade;
		bmp->rgba[i + 2] = shade;
		bmp->rgba[i + 3] = alpha;
	}

	SelectObject(memDc, oldFont);
	DeleteObject(font);
	SelectObject(memDc, oldBmp);
	DeleteObject(dib);
	DeleteDC(memDc);
	ReleaseDC(nullptr, screenDc);
	return bmp;
}

std::shared_ptr<TextureBitmap> LoadImageDecal(const std::string& utf8Path, uint64_t rev) {
	if (utf8Path.empty()) return nullptr;
	std::wstring wp = Utf8ToWide(utf8Path);
	if (wp.empty()) return nullptr;
	Environment::MaterialImage img;
	std::string err;
	if (!Environment::LoadMaterialImage(wp, img, err)) {
		MvmDebugLog_LinefAlways("worlddecals", "image load failed path=\"%s\" err=\"%s\"", utf8Path.c_str(), err.c_str());
		return nullptr;
	}
	auto bmp = std::make_shared<TextureBitmap>();
	bmp->width = img.width;
	bmp->height = img.height;
	bmp->revision = rev;
	bmp->rgba = std::move(img.rgba);
	return bmp;
}

struct Entry {
	Decal decal;
	std::shared_ptr<const TextureBitmap> texture;
	std::unique_ptr<VideoDecoder> video;
	bool dirtyTexture = true;
};

std::mutex g_mutex;
std::vector<Entry> g_entries;
int g_nextId = 1;
int g_selectedId = -1;
uint64_t g_texRevision = 1;
std::atomic<bool> g_anyEnabled{ false };
std::atomic<bool> g_pickerPending{ false };
std::mutex g_pickerResultMutex;
int g_pickerTargetId = -1;
bool g_pickerIsVideo = false;
std::string g_pickerChosenPath;
bool g_pickerHasResult = false;

Entry* FindLocked(int id) {
	for (auto& e : g_entries)
		if (e.decal.id == id) return &e;
	return nullptr;
}

void UpdateEnabledFlagLocked() {
	bool any = false;
	for (const auto& e : g_entries)
		if (e.decal.enabled) { any = true; break; }
	g_anyEnabled.store(any, std::memory_order_release);
}

// Places a new decal ahead of the current camera.
void PlaceAheadOfCamera(Decal& d, bool onFloor) {
	double origin[3] = {}, angles[3] = {}, fov = 90.0;
	CameraBridge_GetCurrentCamera(origin, angles, fov);
	float ang[3] = { (float)angles[0], (float)angles[1], 0.0f };
	Vec3 fwd, right, up;
	BasisFromAngles(ang, fwd, right, up);
	// With the map's collision available, the decal goes on the surface it hits and is aligned to
	// that surface's normal, so a wall decal placed from an angle stays flat on its wall. Without
	// map data it falls back to the camera-relative placement below.
	Environment::SurfaceHit hit;
	if (onFloor) {
		const Environment::Vec3 probe{ (float)origin[0] + fwd.x * 160.0f, (float)origin[1] + fwd.y * 160.0f, (float)origin[2] };
		if (Environment::MapCollisionRaycast(probe, { 0.0f, 0.0f, -1.0f }, 4096.0f, hit) && hit.normal.z > 0.5f) {
			OrientDecalToSurface({ hit.position.x, hit.position.y, hit.position.z }, { hit.normal.x, hit.normal.y, hit.normal.z }, ang[1], d);
			d.depth = 64.0f;
			return;
		}
	} else {
		const Environment::Vec3 eye{ (float)origin[0], (float)origin[1], (float)origin[2] };
		if (Environment::MapCollisionRaycast(eye, { fwd.x, fwd.y, fwd.z }, 8192.0f, hit)) {
			OrientDecalToSurface({ hit.position.x, hit.position.y, hit.position.z }, { hit.normal.x, hit.normal.y, hit.normal.z }, ang[1], d);
			d.depth = 48.0f;
			return;
		}
	}
	if (onFloor) {
		// Floor projection: drop ahead of camera onto floor plane, looking straight down.
		d.position[0] = (float)origin[0] + fwd.x * 160.0f;
		d.position[1] = (float)origin[1] + fwd.y * 160.0f;
		d.position[2] = (float)origin[2] - 58.0f;
		d.angles[0] = 89.5f;
		d.angles[1] = ang[1];
		d.angles[2] = 0.0f;
		d.depth = 64.0f;
	} else {
		// Wall projection: project straight forward from camera onto the wall ahead.
		d.position[0] = (float)origin[0] + fwd.x * 150.0f;
		d.position[1] = (float)origin[1] + fwd.y * 150.0f;
		d.position[2] = (float)origin[2] + fwd.z * 150.0f;
		d.angles[0] = ang[0];
		d.angles[1] = ang[1];
		d.angles[2] = 0.0f;
		d.depth = 96.0f;
	}
}

void StartMediaPicker(int decalId, bool isVideo) {
	if (g_pickerPending.exchange(true)) return;
	std::thread([decalId, isVideo]() {
		wchar_t fileBuf[MAX_PATH * 4] = {};
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.lpstrFile = fileBuf;
		ofn.nMaxFile = (DWORD)(sizeof(fileBuf) / sizeof(fileBuf[0]));
		ofn.lpstrFilter = isVideo
			? L"Video Files (*.mp4;*.mov;*.wmv;*.avi;*.mkv)\0*.mp4;*.mov;*.wmv;*.avi;*.mkv\0All Files (*.*)\0*.*\0"
			: L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif\0All Files (*.*)\0*.*\0";
		ofn.lpstrTitle = isVideo ? L"Choose Wall Video" : L"Choose Decal Image / Logo";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetOpenFileNameW(&ofn) && fileBuf[0]) {
			std::string utf8 = WideToUtf8(fileBuf);
			std::lock_guard<std::mutex> lock(g_pickerResultMutex);
			g_pickerTargetId = decalId;
			g_pickerIsVideo = isVideo;
			g_pickerChosenPath = std::move(utf8);
			g_pickerHasResult = true;
		}
		g_pickerPending.store(false, std::memory_order_release);
	}).detach();
}

} // namespace

void OnLevelInit() {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_entries.clear();
	g_selectedId = -1;
	g_nextId = 1;
	g_anyEnabled.store(false, std::memory_order_release);
}

void Shutdown() {
	OnLevelInit();
}

bool AnyEnabled() {
	return g_anyEnabled.load(std::memory_order_acquire);
}

void RunMainThreadFrame() {
	// Consume any completed native file picker result.
	{
		std::lock_guard<std::mutex> plock(g_pickerResultMutex);
		if (g_pickerHasResult) {
			g_pickerHasResult = false;
			std::lock_guard<std::mutex> lock(g_mutex);
			if (Entry* e = FindLocked(g_pickerTargetId)) {
				e->decal.mediaPath = g_pickerChosenPath;
				e->decal.kind = g_pickerIsVideo ? DecalKind::Video : DecalKind::Image;
				e->dirtyTexture = true;
				if (e->video) e->video->Close();
			}
		}
	}

	double demoTime = 0.0;
	if (!g_MirvTime.GetCurrentDemoTime(demoTime)) demoTime = g_MirvTime.curtime_get();
	if (!std::isfinite(demoTime)) demoTime = 0.0;

	std::lock_guard<std::mutex> lock(g_mutex);
	for (auto& e : g_entries) {
		if (!e.decal.enabled) continue;

		// Resolve attachment to any Environment Scene Object (light, mesh, camera) so the decal
		// moves with rigged / animated / camera-riding objects.
		if (e.decal.attachObjectId > 0) {
			float parentPos[3] = {}, parentAng[3] = {};
			if (Environment::ObjectWorldPose(e.decal.attachObjectId, parentPos, parentAng)) {
				ComposeAttachedPose(parentPos, parentAng,
					e.decal.attachOffset, e.decal.attachAngles,
					e.decal.position, e.decal.angles);
			}
		}

		// Rebuild text / image / video textures as needed.
		if (e.decal.kind == DecalKind::Text && e.dirtyTexture) {
			e.dirtyTexture = false;
			const uint64_t rev = ++g_texRevision;
			e.decal.textureRevision = rev;
			e.texture = RasterizeTextDecal(e.decal, rev);
		} else if (e.decal.kind == DecalKind::Image && e.dirtyTexture) {
			e.dirtyTexture = false;
			const uint64_t rev = ++g_texRevision;
			e.decal.textureRevision = rev;
			e.texture = LoadImageDecal(e.decal.mediaPath, rev);
		} else if (e.decal.kind == DecalKind::Video) {
			if (e.dirtyTexture) {
				e.dirtyTexture = false;
				if (!e.video) e.video = std::make_unique<VideoDecoder>();
				std::wstring wp = e.decal.mediaPath.empty() ? std::wstring{} : Utf8ToWide(e.decal.mediaPath);
				if (!wp.empty())
					e.video->Open(wp);
			}
			if (e.video && e.video->ok) {
				double t = (demoTime - e.decal.videoStartTime) * e.decal.videoSpeed;
				if (t < 0.0) t = 0.0;
				if (e.decal.videoLoop && e.video->durationSec > 0.05)
					t = std::fmod(t, e.video->durationSec);
				if (e.video->DecodeAt(t, g_texRevision + 1)) {
					++g_texRevision;
					e.decal.textureRevision = g_texRevision;
					e.texture = e.video->currentFrame;
				}
			}
		}
	}
}

std::vector<RenderItem> RenderSnapshot() {
	std::lock_guard<std::mutex> lock(g_mutex);
	std::vector<RenderItem> out;
	out.reserve(g_entries.size());
	for (const auto& e : g_entries) {
		if (!e.decal.enabled) continue;
		out.push_back(RenderItem{ e.decal, e.texture });
	}
	return out;
}

void AppendStateJson(std::ostringstream& out) {
	std::lock_guard<std::mutex> lock(g_mutex);
	out << ",\"decals\":{\"selected\":" << g_selectedId
		<< ",\"pickerPending\":" << (g_pickerPending.load(std::memory_order_relaxed) ? "true" : "false")
		<< ",\"items\":[";
	for (size_t i = 0; i < g_entries.size(); ++i) {
		const Decal& d = g_entries[i].decal;
		if (i) out << ",";
		out << "{\"id\":" << d.id
			<< ",\"name\":\"" << JsonBuilder::Escape(d.name) << "\""
			<< ",\"kind\":\"" << KindToken(d.kind) << "\""
			<< ",\"enabled\":" << (d.enabled ? "true" : "false")
			<< ",\"render\":" << (d.render ? "true" : "false")
			<< ",\"pos\":[" << d.position[0] << "," << d.position[1] << "," << d.position[2] << "]"
			<< ",\"ang\":[" << d.angles[0] << "," << d.angles[1] << "," << d.angles[2] << "]"
			<< ",\"width\":" << d.width << ",\"height\":" << d.height << ",\"depth\":" << d.depth
			<< ",\"color\":[" << d.color[0] << "," << d.color[1] << "," << d.color[2] << "," << d.color[3] << "]"
			<< ",\"blend\":\"" << BlendToken(d.blend) << "\""
			<< ",\"normalFade\":" << d.normalFade
			<< ",\"skipPlayers\":" << (d.skipPlayers ? "true" : "false")
			<< ",\"text\":\"" << JsonBuilder::Escape(d.text) << "\""
			<< ",\"fontSize\":" << d.fontSize
			<< ",\"bold\":" << (d.bold ? "true" : "false")
			<< ",\"outline\":" << (d.outline ? "true" : "false")
			<< ",\"mediaPath\":\"" << JsonBuilder::Escape(d.mediaPath) << "\""
			<< ",\"videoSpeed\":" << d.videoSpeed
			<< ",\"videoStart\":" << d.videoStartTime
			<< ",\"videoLoop\":" << (d.videoLoop ? "true" : "false")
			<< ",\"arrowHead\":" << d.arrowHead
			<< ",\"arrowShaft\":" << d.arrowShaft
			<< ",\"arrowDashed\":" << (d.arrowDashed ? "true" : "false")
			<< ",\"arrowPulse\":" << d.arrowPulse
			<< ",\"routeCount\":" << d.routePoints.size()
			<< ",\"routeWidth\":" << d.routeWidth
			<< ",\"routeProgress\":" << d.routeProgress
			<< ",\"routeAnimate\":" << (d.routeAnimate ? "true" : "false")
			<< ",\"routeStart\":" << d.routeStartTime
			<< ",\"routeDuration\":" << d.routeDuration
			<< ",\"routeDashed\":" << (d.routeDashed ? "true" : "false")
			<< ",\"routeArrowCap\":" << (d.routeArrowCap ? "true" : "false")
			<< ",\"attachObjectId\":" << d.attachObjectId
			<< ",\"attachOffset\":[" << d.attachOffset[0] << "," << d.attachOffset[1] << "," << d.attachOffset[2] << "]"
			<< "}";
	}
	out << "]}";
}

void RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	if (argc < 3) {
		advancedfx::Message(
			"%s decals add <text|image|video|arrow|route> [floor|wall]\n"
			"%s decals select <id> | remove <id> | duplicate <id> | clear\n"
			"%s decals set <id> <field> <value...>\n"
			"%s decals place <id> <wall|floor>\n"
			"%s decals pickmedia <id> <image|video>\n"
			"%s decals route <id> addpoint|undo|clear|startnow\n"
			"%s decals attach <id> <objectId|off>\n",
			cmd, cmd, cmd, cmd, cmd, cmd, cmd);
		return;
	}
	const char* sub = args->ArgV(2);
	if (0 == _stricmp(sub, "add")) {
		DecalKind kind = DecalKind::Text;
		if (argc >= 4) KindFromToken(args->ArgV(3), kind);
		const bool onFloor = (argc >= 5 && 0 == _stricmp(args->ArgV(4), "floor"))
			|| kind == DecalKind::Arrow || kind == DecalKind::Route;
		std::lock_guard<std::mutex> lock(g_mutex);
		Entry e;
		e.decal.id = g_nextId++;
		e.decal.kind = kind;
		if (kind == DecalKind::Text) {
			e.decal.name = "Text " + std::to_string(e.decal.id);
			e.decal.width = 128.0f; e.decal.height = 64.0f;
		} else if (kind == DecalKind::Image) {
			e.decal.name = "Wall Image " + std::to_string(e.decal.id);
			e.decal.width = 128.0f; e.decal.height = 96.0f;
		} else if (kind == DecalKind::Video) {
			e.decal.name = "Wall Video " + std::to_string(e.decal.id);
			e.decal.width = 160.0f; e.decal.height = 90.0f;
		} else if (kind == DecalKind::Arrow) {
			e.decal.name = "Arrow " + std::to_string(e.decal.id);
			e.decal.width = 72.0f; e.decal.height = 128.0f;
			e.decal.color[0] = 1.0f; e.decal.color[1] = 0.78f; e.decal.color[2] = 0.18f;
		} else if (kind == DecalKind::Route) {
			e.decal.name = "Route " + std::to_string(e.decal.id);
			e.decal.color[0] = 0.25f; e.decal.color[1] = 0.88f; e.decal.color[2] = 0.72f;
			e.decal.routeAnimate = true;
			double demoTime = 0.0;
			if (!g_MirvTime.GetCurrentDemoTime(demoTime)) demoTime = g_MirvTime.curtime_get();
			e.decal.routeStartTime = (float)demoTime;
			// Seed two initial waypoints ahead of camera so the route is immediately visible.
			double origin[3] = {}, angles[3] = {}, fov = 90.0;
			CameraBridge_GetCurrentCamera(origin, angles, fov);
			float ang[3] = { 0.0f, (float)angles[1], 0.0f };
			Vec3 fwd, right, up;
			BasisFromAngles(ang, fwd, right, up);
			e.decal.routePoints.push_back({ (float)origin[0] + fwd.x * 64.0f, (float)origin[1] + fwd.y * 64.0f, (float)origin[2] - 60.0f });
			e.decal.routePoints.push_back({ (float)origin[0] + fwd.x * 260.0f, (float)origin[1] + fwd.y * 260.0f, (float)origin[2] - 60.0f });
		}
		PlaceAheadOfCamera(e.decal, onFloor);
		e.dirtyTexture = true;
		g_selectedId = e.decal.id;
		g_entries.push_back(std::move(e));
		UpdateEnabledFlagLocked();
		return;
	}
	if (0 == _stricmp(sub, "select") && argc >= 4) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_selectedId = std::atoi(args->ArgV(3));
		return;
	}
	if (0 == _stricmp(sub, "clear")) {
		OnLevelInit();
		return;
	}
	if (0 == _stricmp(sub, "remove") && argc >= 4) {
		const int id = std::atoi(args->ArgV(3));
		std::lock_guard<std::mutex> lock(g_mutex);
		g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(),
			[id](const Entry& e) { return e.decal.id == id; }), g_entries.end());
		if (g_selectedId == id) g_selectedId = g_entries.empty() ? -1 : g_entries.back().decal.id;
		UpdateEnabledFlagLocked();
		return;
	}
	if (0 == _stricmp(sub, "duplicate") && argc >= 4) {
		const int id = std::atoi(args->ArgV(3));
		std::lock_guard<std::mutex> lock(g_mutex);
		Entry* src = FindLocked(id);
		if (!src) return;
		Entry copy;
		copy.decal = src->decal;
		copy.decal.id = g_nextId++;
		copy.decal.name += " copy";
		copy.decal.position[0] += 16.0f;
		copy.dirtyTexture = true;
		g_selectedId = copy.decal.id;
		g_entries.push_back(std::move(copy));
		UpdateEnabledFlagLocked();
		return;
	}
	if (0 == _stricmp(sub, "place") && argc >= 5) {
		const int id = std::atoi(args->ArgV(3));
		const bool onFloor = 0 == _stricmp(args->ArgV(4), "floor");
		std::lock_guard<std::mutex> lock(g_mutex);
		if (Entry* e = FindLocked(id)) {
			e->decal.attachObjectId = -1;
			PlaceAheadOfCamera(e->decal, onFloor);
		}
		return;
	}
	if (0 == _stricmp(sub, "pickmedia") && argc >= 5) {
		const int id = std::atoi(args->ArgV(3));
		const bool isVideo = 0 == _stricmp(args->ArgV(4), "video");
		StartMediaPicker(id, isVideo);
		return;
	}
	if (0 == _stricmp(sub, "attach") && argc >= 5) {
		const int id = std::atoi(args->ArgV(3));
		const char* target = args->ArgV(4);
		std::lock_guard<std::mutex> lock(g_mutex);
		if (Entry* e = FindLocked(id)) {
			if (0 == _stricmp(target, "off") || 0 == _stricmp(target, "none") || std::atoi(target) <= 0) {
				e->decal.attachObjectId = -1;
			} else {
				e->decal.attachObjectId = std::atoi(target);
			}
		}
		return;
	}
	if (0 == _stricmp(sub, "route") && argc >= 5) {
		const int id = std::atoi(args->ArgV(3));
		const char* act = args->ArgV(4);
		std::lock_guard<std::mutex> lock(g_mutex);
		Entry* e = FindLocked(id);
		if (!e) return;
		if (0 == _stricmp(act, "addpoint")) {
			if ((int)e->decal.routePoints.size() < kMaxRoutePoints) {
				double origin[3] = {}, angles[3] = {}, fov = 90.0;
				CameraBridge_GetCurrentCamera(origin, angles, fov);
				float ang[3] = { (float)angles[0], (float)angles[1], 0.0f };
				Vec3 fwd, right, up;
				BasisFromAngles(ang, fwd, right, up);
				// Intersect view ray with floor height (origin.z - 60) or step 160u forward.
				Vec3 pt{ (float)origin[0] + fwd.x * 160.0f, (float)origin[1] + fwd.y * 160.0f, (float)origin[2] - 60.0f };
				if (fwd.z < -0.15f) {
					const float t = Clamp(-60.0f / fwd.z, 24.0f, 1200.0f);
					pt = { (float)origin[0] + fwd.x * t, (float)origin[1] + fwd.y * t, (float)origin[2] - 60.0f };
				}
				e->decal.routePoints.push_back(pt);
			}
		} else if (0 == _stricmp(act, "undo")) {
			if (!e->decal.routePoints.empty()) e->decal.routePoints.pop_back();
		} else if (0 == _stricmp(act, "clear")) {
			e->decal.routePoints.clear();
		} else if (0 == _stricmp(act, "startnow")) {
			double demoTime = 0.0;
			if (!g_MirvTime.GetCurrentDemoTime(demoTime)) demoTime = g_MirvTime.curtime_get();
			e->decal.routeStartTime = (float)demoTime;
			e->decal.routeAnimate = true;
		}
		return;
	}
	if (0 == _stricmp(sub, "set") && argc >= 6) {
		const int id = std::atoi(args->ArgV(3));
		const char* f = args->ArgV(4);
		const char* val = args->ArgV(5);
		const float fv = (float)std::atof(val);
		std::lock_guard<std::mutex> lock(g_mutex);
		Entry* e = FindLocked(id);
		if (!e) return;
		Decal& d = e->decal;
		if (0 == _stricmp(f, "enabled")) { d.enabled = fv > 0.5f || 0 == _stricmp(val, "on") || 0 == _stricmp(val, "true"); UpdateEnabledFlagLocked(); }
		else if (0 == _stricmp(f, "render")) d.render = fv > 0.5f;
		else if (0 == _stricmp(f, "x")) d.position[0] = fv;
		else if (0 == _stricmp(f, "y")) d.position[1] = fv;
		else if (0 == _stricmp(f, "z")) d.position[2] = fv;
		else if (0 == _stricmp(f, "pitch")) d.angles[0] = Clamp(fv, -180.0f, 180.0f);
		else if (0 == _stricmp(f, "yaw")) d.angles[1] = Clamp(fv, -180.0f, 180.0f);
		else if (0 == _stricmp(f, "roll")) d.angles[2] = Clamp(fv, -180.0f, 180.0f);
		else if (0 == _stricmp(f, "width")) d.width = Clamp(fv, 4.0f, 2048.0f);
		else if (0 == _stricmp(f, "height")) d.height = Clamp(fv, 4.0f, 2048.0f);
		else if (0 == _stricmp(f, "depth")) d.depth = Clamp(fv, 4.0f, 512.0f);
		else if (0 == _stricmp(f, "opacity")) d.color[3] = Clamp(fv, 0.0f, 1.0f);
		else if (0 == _stricmp(f, "color") && argc >= 8) {
			d.color[0] = Clamp((float)std::atof(args->ArgV(5)), 0.0f, 1.0f);
			d.color[1] = Clamp((float)std::atof(args->ArgV(6)), 0.0f, 1.0f);
			d.color[2] = Clamp((float)std::atof(args->ArgV(7)), 0.0f, 1.0f);
		}
		else if (0 == _stricmp(f, "blend")) BlendFromToken(val, d.blend);
		else if (0 == _stricmp(f, "normalFade")) d.normalFade = Clamp(fv, 0.0f, 0.99f);
		else if (0 == _stricmp(f, "skipPlayers")) d.skipPlayers = fv > 0.5f;
		else if (0 == _stricmp(f, "text")) {
			std::string joined;
			for (int a = 5; a < argc; ++a) {
				if (a > 5) joined += ' ';
				joined += args->ArgV(a);
			}
			d.text = joined;
			e->dirtyTexture = true;
		}
		else if (0 == _stricmp(f, "fontSize")) { d.fontSize = std::clamp((int)fv, 16, 180); e->dirtyTexture = true; }
		else if (0 == _stricmp(f, "bold")) { d.bold = fv > 0.5f; e->dirtyTexture = true; }
		else if (0 == _stricmp(f, "outline")) { d.outline = fv > 0.5f; e->dirtyTexture = true; }
		else if (0 == _stricmp(f, "media")) { d.mediaPath = val; e->dirtyTexture = true; if (e->video) e->video->Close(); }
		else if (0 == _stricmp(f, "videoSpeed")) d.videoSpeed = Clamp(fv, 0.05f, 4.0f);
		else if (0 == _stricmp(f, "videoStart")) d.videoStartTime = fv;
		else if (0 == _stricmp(f, "videoLoop")) d.videoLoop = fv > 0.5f;
		else if (0 == _stricmp(f, "arrowHead")) d.arrowHead = Clamp(fv, 0.1f, 0.8f);
		else if (0 == _stricmp(f, "arrowShaft")) d.arrowShaft = Clamp(fv, 0.05f, 0.6f);
		else if (0 == _stricmp(f, "arrowDashed")) d.arrowDashed = fv > 0.5f;
		else if (0 == _stricmp(f, "arrowPulse")) d.arrowPulse = Clamp(fv, 0.0f, 10.0f);
		else if (0 == _stricmp(f, "routeWidth")) d.routeWidth = Clamp(fv, 2.0f, 128.0f);
		else if (0 == _stricmp(f, "routeProgress")) d.routeProgress = Clamp(fv, 0.0f, 1.0f);
		else if (0 == _stricmp(f, "routeAnimate")) d.routeAnimate = fv > 0.5f;
		else if (0 == _stricmp(f, "routeDuration")) d.routeDuration = Clamp(fv, 0.1f, 60.0f);
		else if (0 == _stricmp(f, "routeDashed")) d.routeDashed = fv > 0.5f;
		else if (0 == _stricmp(f, "routeArrowCap")) d.routeArrowCap = fv > 0.5f;
		else if (0 == _stricmp(f, "attachX")) d.attachOffset[0] = fv;
		else if (0 == _stricmp(f, "attachY")) d.attachOffset[1] = fv;
		else if (0 == _stricmp(f, "attachZ")) d.attachOffset[2] = fv;
		return;
	}
	advancedfx::Warning("%s decals: unknown subcommand '%s'.\n", cmd, sub);
}

} // namespace Filmmaker::WorldDecals
