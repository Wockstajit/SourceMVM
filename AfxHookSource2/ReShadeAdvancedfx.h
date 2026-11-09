// The ReShade addon's render entry (ReShadeStateAddon/). ReShade loads and frees its addons itself,
// so nothing here keeps a pointer into the addon: every call looks it up again (same rule and reason
// as UiInputGate.cpp).
class CReShadeAdvancedfx {
public:
	// The addon is loaded right now, so effects can run on the scene and reach a render.
	bool IsConnected();

	// ReShade itself is in the process, with or without our addon beside it.
	bool IsReShadeLoaded();

	// True when the add-on ran the effects on this texture. On false ReShade still draws them at
	// Present, where the recorder does not see them.
	bool AdvancedfxRenderEffects(void* pRenderTargetView, void* pDepthTextureResource);
};

extern CReShadeAdvancedfx g_ReShadeAdvancedfx;

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Resource;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11InputLayout;
struct ID3D11Buffer;
struct ID3D11SamplerState;
struct ID3D11RasterizerState;
struct ID3D11DepthStencilState;
struct ID3D11BlendState;

// Runs ReShade's effects on the scene at the before-UI boundary. CS2's scene target is multisampled
// and ReShade refuses a multisampled target, so the frame goes through a resolved single-sample copy:
// resolve, effects on the copy, then the copy is drawn back over the scene. Every sample of the scene
// then holds the graded pixel, so the recorder's own resolve and the preview both read the graded frame.
class CReShadeScenePass {
public:
	~CReShadeScenePass();
	void BeginDevice(ID3D11Device* device);
	void EndDevice();
	// True when the effects ran on this frame.
	bool Apply(ID3D11DeviceContext* pImmediateContext, ID3D11RenderTargetView* pTarget, ID3D11Resource* pDepth);

private:
	bool Ready() const;
	bool EnsureCopy(unsigned width, unsigned height, int format, int viewFormat);
	void ReleaseCopy();

	ID3D11Device* m_Device = nullptr;
	ID3D11DeviceContext* m_DeferredContext = nullptr;
	ID3D11Texture2D* m_Copy = nullptr;
	ID3D11ShaderResourceView* m_CopySrv = nullptr;
	unsigned m_CopyWidth = 0, m_CopyHeight = 0;
	int m_CopyFormat = 0, m_CopyViewFormat = 0;
	ID3D11VertexShader* m_VertexShader = nullptr;
	ID3D11PixelShader* m_PixelShader = nullptr;
	ID3D11InputLayout* m_InputLayout = nullptr;
	ID3D11Buffer* m_VertexBuffer = nullptr;
	ID3D11Buffer* m_VsConstantBuffer = nullptr;
	ID3D11SamplerState* m_SamplerState = nullptr;
	ID3D11RasterizerState* m_RasterizerState = nullptr;
	ID3D11DepthStencilState* m_DepthStencilState = nullptr;
	ID3D11BlendState* m_BlendState = nullptr;
};

extern CReShadeScenePass g_ReShadeScenePass;
