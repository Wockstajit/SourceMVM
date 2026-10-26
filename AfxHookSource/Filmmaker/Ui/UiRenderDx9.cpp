#include "UiRenderDx9.h"

#include <cstring>

namespace SourceFilmmaker {
namespace Ui {

namespace {

struct Vertex {
	float pos[3];
	D3DCOLOR col;
	float uv[2];
};
const DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

template <typename T>
void Release(T*& p) {
	if (p) {
		p->Release();
		p = nullptr;
	}
}

// Fixed-function alpha blending in an orthographic projection over width x height pixels, offset by
// half a pixel so texels land on pixels -- the state ImGui's own DX9 backend sets.
void SetupState(IDirect3DDevice9* device, float x, float y, float width, float height) {
	D3DVIEWPORT9 vp;
	vp.X = 0;
	vp.Y = 0;
	vp.Width = (DWORD)width;
	vp.Height = (DWORD)height;
	vp.MinZ = 0.0f;
	vp.MaxZ = 1.0f;
	device->SetViewport(&vp);
	device->SetPixelShader(nullptr);
	device->SetVertexShader(nullptr);
	device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
	device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
	device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	device->SetRenderState(D3DRS_ZENABLE, FALSE);
	device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
	device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
	device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
	device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
	device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	device->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
	device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
	device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	device->SetRenderState(D3DRS_CLIPPING, TRUE);
	device->SetRenderState(D3DRS_LIGHTING, FALSE);
	// The game may leave sRGB writes on; the UI's colours are already display values.
	device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	device->SetRenderState(D3DRS_COLORWRITEENABLE,
		D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
	device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
	device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

	const float L = x + 0.5f, R = x + width + 0.5f, T = y + 0.5f, B = y + height + 0.5f;
	D3DMATRIX identity = { { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } } };
	D3DMATRIX projection = { { {
		2.0f / (R - L), 0.0f, 0.0f, 0.0f,
		0.0f, 2.0f / (T - B), 0.0f, 0.0f,
		0.0f, 0.0f, 0.5f, 0.0f,
		(L + R) / (L - R), (T + B) / (B - T), 0.5f, 1.0f
	} } };
	device->SetTransform(D3DTS_WORLD, &identity);
	device->SetTransform(D3DTS_VIEW, &identity);
	device->SetTransform(D3DTS_PROJECTION, &projection);
}

} // namespace

std::shared_ptr<DrawSnapshot> TakeSnapshot(const ImDrawData* data) {
	auto snapshot = std::make_shared<DrawSnapshot>();
	if (!data || !data->Valid) return snapshot;
	snapshot->displayPos = data->DisplayPos;
	snapshot->displaySize = data->DisplaySize;
	snapshot->vertices.reserve(data->TotalVtxCount);
	snapshot->indices.reserve(data->TotalIdxCount);
	for (const ImDrawList* list : data->CmdLists) {
		const unsigned vertexBase = (unsigned)snapshot->vertices.size();
		const unsigned indexBase = (unsigned)snapshot->indices.size();
		snapshot->vertices.insert(snapshot->vertices.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
		snapshot->indices.insert(snapshot->indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
		for (const ImDrawCmd& cmd : list->CmdBuffer) {
			if (cmd.UserCallback || cmd.ElemCount == 0) continue;
			DrawSnapshot::Command c;
			c.clip = cmd.ClipRect;
			c.texture = cmd.GetTexID();
			c.indexOffset = indexBase + cmd.IdxOffset;
			c.vertexOffset = vertexBase + cmd.VtxOffset;
			c.elementCount = cmd.ElemCount;
			snapshot->commands.push_back(c);
		}
	}
	return snapshot;
}

RendererDx9::~RendererDx9() { ReleaseDeviceObjects(); }

void RendererDx9::ReleaseDeviceObjects() {
	Release(m_vb);
	Release(m_ib);
	Release(m_font);
	Release(m_frame);
	m_vbSize = 0;
	m_ibSize = 0;
	m_fontVersion = 0;
	m_frameW = m_frameH = 0;
	m_frameFormat = D3DFMT_UNKNOWN;
}

bool RendererDx9::EnsureBuffers(IDirect3DDevice9* device, int vertices, int indices) {
	if (!m_vb || m_vbSize < vertices) {
		Release(m_vb);
		m_vbSize = vertices + 5000;
		if (FAILED(device->CreateVertexBuffer(m_vbSize * sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
			kFvf, D3DPOOL_DEFAULT, &m_vb, nullptr))) {
			m_vbSize = 0;
			return false;
		}
	}
	if (!m_ib || m_ibSize < indices) {
		Release(m_ib);
		m_ibSize = indices + 10000;
		if (FAILED(device->CreateIndexBuffer(m_ibSize * sizeof(ImDrawIdx), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
			sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32, D3DPOOL_DEFAULT, &m_ib, nullptr))) {
			m_ibSize = 0;
			return false;
		}
	}
	return true;
}

bool RendererDx9::EnsureFont(IDirect3DDevice9* device, const FontPixels& font) {
	if (m_font && m_fontVersion == font.version) return true;
	Release(m_font);
	if (font.width <= 0 || font.height <= 0 || font.rgba.size() < (size_t)font.width * font.height * 4) return false;
	if (FAILED(device->CreateTexture(font.width, font.height, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
		&m_font, nullptr)))
		return false;
	D3DLOCKED_RECT locked;
	if (FAILED(m_font->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD))) {
		Release(m_font);
		return false;
	}
	for (int y = 0; y < font.height; ++y) {
		const unsigned char* src = &font.rgba[(size_t)y * font.width * 4];
		unsigned char* dst = (unsigned char*)locked.pBits + (size_t)y * locked.Pitch;
		for (int x = 0; x < font.width; ++x) {
			// RGBA bytes to D3DFMT_A8R8G8B8, which is B, G, R, A in memory.
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = src[x * 4 + 3];
		}
	}
	m_font->UnlockRect(0);
	m_fontVersion = font.version;
	return true;
}

bool RendererDx9::EnsureFrame(IDirect3DDevice9* device, const D3DSURFACE_DESC& backBuffer) {
	if (m_frame && m_frameW == backBuffer.Width && m_frameH == backBuffer.Height && m_frameFormat == backBuffer.Format)
		return true;
	Release(m_frame);
	if (FAILED(device->CreateTexture(backBuffer.Width, backBuffer.Height, 1, D3DUSAGE_RENDERTARGET, backBuffer.Format,
		D3DPOOL_DEFAULT, &m_frame, nullptr)))
		return false;
	m_frameW = backBuffer.Width;
	m_frameH = backBuffer.Height;
	m_frameFormat = backBuffer.Format;
	return true;
}

void RendererDx9::DrawLetterbox(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer, const D3DSURFACE_DESC& desc,
	const Rect& preview) {
	if (!EnsureFrame(device, desc)) return;
	IDirect3DSurface9* frameSurface = nullptr;
	if (FAILED(m_frame->GetSurfaceLevel(0, &frameSurface)) || !frameSurface) return;
	const HRESULT copied = device->StretchRect(backBuffer, nullptr, frameSurface, nullptr, D3DTEXF_NONE);
	frameSurface->Release();
	if (FAILED(copied)) return;

	device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
	SetupState(device, 0, 0, (float)desc.Width, (float)desc.Height);
	device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	device->SetTexture(0, m_frame);
	const D3DCOLOR white = D3DCOLOR_ARGB(255, 255, 255, 255);
	Vertex quad[4] = {
		{ { preview.x0, preview.y0, 0 }, white, { 0, 0 } },
		{ { preview.x1, preview.y0, 0 }, white, { 1, 0 } },
		{ { preview.x0, preview.y1, 0 }, white, { 0, 1 } },
		{ { preview.x1, preview.y1, 0 }, white, { 1, 1 } },
	};
	device->SetFVF(kFvf);
	device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
	device->SetTexture(0, nullptr);
}

void RendererDx9::DrawCommands(IDirect3DDevice9* device, const DrawSnapshot& snapshot) {
	Vertex* vtx = nullptr;
	ImDrawIdx* idx = nullptr;
	if (FAILED(m_vb->Lock(0, (UINT)(snapshot.vertices.size() * sizeof(Vertex)), (void**)&vtx, D3DLOCK_DISCARD))) return;
	if (FAILED(m_ib->Lock(0, (UINT)(snapshot.indices.size() * sizeof(ImDrawIdx)), (void**)&idx, D3DLOCK_DISCARD))) {
		m_vb->Unlock();
		return;
	}
	for (const ImDrawVert& v : snapshot.vertices) {
		vtx->pos[0] = v.pos.x;
		vtx->pos[1] = v.pos.y;
		vtx->pos[2] = 0.0f;
		vtx->col = v.col; // IMGUI_USE_BGRA_PACKED_COLOR: already 0xAARRGGBB
		vtx->uv[0] = v.uv.x;
		vtx->uv[1] = v.uv.y;
		++vtx;
	}
	std::memcpy(idx, snapshot.indices.data(), snapshot.indices.size() * sizeof(ImDrawIdx));
	m_vb->Unlock();
	m_ib->Unlock();

	SetupState(device, snapshot.displayPos.x, snapshot.displayPos.y, snapshot.displaySize.x, snapshot.displaySize.y);
	device->SetStreamSource(0, m_vb, 0, sizeof(Vertex));
	device->SetIndices(m_ib);
	device->SetFVF(kFvf);
	const UINT vertexCount = (UINT)snapshot.vertices.size();
	for (const DrawSnapshot::Command& c : snapshot.commands) {
		const RECT clip = { (LONG)(c.clip.x - snapshot.displayPos.x), (LONG)(c.clip.y - snapshot.displayPos.y),
			(LONG)(c.clip.z - snapshot.displayPos.x), (LONG)(c.clip.w - snapshot.displayPos.y) };
		if (clip.right <= clip.left || clip.bottom <= clip.top) continue;
		device->SetScissorRect(&clip);
		device->SetTexture(0, c.texture == kFontTextureId ? m_font : nullptr);
		device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, (INT)c.vertexOffset, 0, vertexCount - c.vertexOffset,
			c.indexOffset, c.elementCount / 3);
	}
	device->SetTexture(0, nullptr);
}

void RendererDx9::Render(IDirect3DDevice9* device, const DrawSnapshot* world, const DrawSnapshot* ui, const FontPixels& font) {
	if (!device) return;
	const bool drawWorld = world && !world->commands.empty();
	const bool drawUi = ui && (!ui->commands.empty() || ui->letterbox);
	if (!drawWorld && !drawUi) return;

	IDirect3DSurface9* backBuffer = nullptr;
	if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer) return;
	D3DSURFACE_DESC desc;
	backBuffer->GetDesc(&desc);

	IDirect3DStateBlock9* state = nullptr;
	if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state)) || !state) {
		backBuffer->Release();
		return;
	}
	IDirect3DSurface9* oldTarget = nullptr;
	IDirect3DSurface9* oldDepth = nullptr;
	device->GetRenderTarget(0, &oldTarget);
	device->GetDepthStencilSurface(&oldDepth);
	device->SetRenderTarget(0, backBuffer);
	device->SetDepthStencilSurface(nullptr);

	if (SUCCEEDED(device->BeginScene())) {
		const auto draw = [&](const DrawSnapshot& snapshot) {
			if (!snapshot.commands.empty() && EnsureFont(device, font)
				&& EnsureBuffers(device, (int)snapshot.vertices.size(), (int)snapshot.indices.size()))
				DrawCommands(device, snapshot);
		};
		if (drawWorld) draw(*world);
		if (drawUi) {
			if (ui->letterbox && ui->preview.Valid()) DrawLetterbox(device, backBuffer, desc, ui->preview);
			draw(*ui);
		}
		device->EndScene();
	}

	device->SetRenderTarget(0, oldTarget);
	device->SetDepthStencilSurface(oldDepth);
	Release(oldTarget);
	Release(oldDepth);
	state->Apply();
	state->Release();
	backBuffer->Release();
}

} // namespace Ui
} // namespace SourceFilmmaker
