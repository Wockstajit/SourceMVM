#include "stdafx.h"

#include "SourceRenderCapture.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h> // before AfxOutStreams.h, which warns through it
#include <Shared/ImageBufferThreadSafe.h>
#include <Shared/ImageFormat.h>
#include <Shared/OutVideoStreamCreators.h>

#include <d3d9.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace SourceFilmmaker {

namespace {

// One frame in BGR, top row first.
class FrameBuffer : public advancedfx::IImageBufferThreadSafe {
public:
	FrameBuffer(int width, int height)
		: m_format(advancedfx::ImageFormat::BGR, width, height), m_bytes(m_format.Bytes) {}
	void AddRef() override { m_refs.fetch_add(1); }
	void Release() override {
		if (m_refs.fetch_sub(1) == 1) delete this;
	}
	const advancedfx::CImageFormat* GetImageBufferFormat() const override { return &m_format; }
	const void* GetImageBufferData() const override { return m_bytes.data(); }
	unsigned char* Data() { return m_bytes.data(); }
	size_t Pitch() const { return m_format.Pitch; }

private:
	std::atomic<int> m_refs{ 1 };
	advancedfx::CImageFormat m_format;
	std::vector<unsigned char> m_bytes;
};

template <class T>
void Release(T*& p) {
	if (p) p->Release();
	p = nullptr;
}

std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<FrameBuffer*> g_queue;
bool g_ending = false;
std::thread g_worker;
advancedfx::COutVideoStreamCreator* g_creator = nullptr;
std::atomic<bool> g_running{ false };
std::atomic<bool> g_finished{ true };
std::atomic<bool> g_armed{ false };
std::atomic<int> g_captured{ 0 }, g_written{ 0 }, g_failures{ 0 };

// Render thread: the readback surfaces, kept while the size and format hold.
IDirect3DSurface9* g_resolve = nullptr; // a plain render target the back buffer is copied into
IDirect3DSurface9* g_system = nullptr;  // its system-memory copy, locked for the bytes
UINT g_w = 0, g_h = 0;
D3DFORMAT g_format = D3DFMT_UNKNOWN;

void Worker() {
	advancedfx::TIOutVideoStream<true>* stream = nullptr;
	for (;;) {
		FrameBuffer* frame = nullptr;
		{
			std::unique_lock<std::mutex> lock(g_mutex);
			g_wake.wait(lock, [] { return g_ending || !g_queue.empty(); });
			if (g_queue.empty()) break; // ending and nothing left
			frame = g_queue.front();
			g_queue.pop_front();
		}
		if (!stream && g_creator) {
			stream = g_creator->CreateOutVideoStream(*frame->GetImageBufferFormat());
			if (!stream && Filmmaker::MvmDebugLog_Active())
				Filmmaker::MvmDebugLog_LinefAlways("render.capture", "the recorder made no stream for %dx%d",
					frame->GetImageBufferFormat()->Width, frame->GetImageBufferFormat()->Height);
		}
		if (stream && stream->SupplyImageBuffer(nullptr, frame)) g_written.fetch_add(1);
		else g_failures.fetch_add(1);
		frame->Release();
	}
	// Closing the stream ends the encoder: ffmpeg finishes the file when its input closes.
	if (stream) stream->Release();
	if (g_creator) g_creator->Release();
	g_creator = nullptr;
	g_finished = true;
}

bool EnsureSurfaces(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc) {
	if (g_resolve && g_system && g_w == desc.Width && g_h == desc.Height && g_format == desc.Format) return true;
	Release(g_resolve);
	Release(g_system);
	g_w = g_h = 0;
	if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_resolve, nullptr))
		|| FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &g_system, nullptr))) {
		Release(g_resolve);
		Release(g_system);
		return false;
	}
	g_w = desc.Width;
	g_h = desc.Height;
	g_format = desc.Format;
	return true;
}

// The back buffer's picture in BGR, or nullptr.
FrameBuffer* ReadBack(IDirect3DDevice9* device) {
	IDirect3DSurface9* back = nullptr;
	if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return nullptr;
	FrameBuffer* frame = nullptr;
	D3DSURFACE_DESC desc;
	back->GetDesc(&desc);
	const bool fourBytes = desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8;
	// A multisampled back buffer resolves through StretchRect; a plain one is copied the same way.
	if (fourBytes && EnsureSurfaces(device, desc)
		&& SUCCEEDED(device->StretchRect(back, nullptr, g_resolve, nullptr, D3DTEXF_NONE))
		&& SUCCEEDED(device->GetRenderTargetData(g_resolve, g_system))) {
		D3DLOCKED_RECT locked;
		if (SUCCEEDED(g_system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
			frame = new FrameBuffer((int)desc.Width, (int)desc.Height);
			for (UINT y = 0; y < desc.Height; ++y) {
				const unsigned char* src = (const unsigned char*)locked.pBits + (size_t)y * locked.Pitch;
				unsigned char* dst = frame->Data() + (size_t)y * frame->Pitch();
				for (UINT x = 0; x < desc.Width; ++x) {
					dst[0] = src[0];
					dst[1] = src[1];
					dst[2] = src[2];
					src += 4;
					dst += 3;
				}
			}
			g_system->UnlockRect();
		}
	}
	back->Release();
	return frame;
}

} // namespace

bool RenderCapture_Begin(advancedfx::COutVideoStreamCreator* creator, std::string& error) {
	if (g_running) {
		error = "a capture is already running";
		return false;
	}
	if (!creator) {
		error = "the recorder has no output for this format";
		return false;
	}
	if (g_worker.joinable()) g_worker.join();
	creator->AddRef();
	g_creator = creator;
	g_captured = 0;
	g_written = 0;
	g_failures = 0;
	g_ending = false;
	g_finished = false;
	g_armed = false;
	g_running = true;
	g_worker = std::thread(Worker);
	return true;
}

void RenderCapture_Arm() {
	if (g_running) g_armed = true;
}

void RenderCapture_Present(IDirect3DDevice9* device) {
	if (!device || !g_armed.exchange(false)) return;
	FrameBuffer* frame = ReadBack(device);
	if (!frame) {
		g_failures.fetch_add(1);
		return;
	}
	g_captured.fetch_add(1);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_queue.push_back(frame);
	}
	g_wake.notify_one();
}

int RenderCapture_Captured() { return g_captured.load(); }
int RenderCapture_Written() { return g_written.load(); }
int RenderCapture_Failures() { return g_failures.load(); }

void RenderCapture_End() {
	if (!g_running) return;
	g_running = false;
	g_armed = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_ending = true;
	}
	g_wake.notify_one();
}

bool RenderCapture_Finished() {
	if (!g_finished) return false;
	if (g_worker.joinable()) g_worker.join();
	return true;
}

void RenderCapture_DeviceLost() {
	Release(g_resolve);
	Release(g_system);
	g_w = g_h = 0;
}

} // namespace SourceFilmmaker
