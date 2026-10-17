#pragma once

//
class CMaterialSystemFunctor abstract {
public:
    virtual ~CMaterialSystemFunctor() {};
    virtual void operator()() = 0;
};
//

#include "AfxShaders.h"
#include "../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../Shared/CamPath.h"

#include <d3d11.h>
#define _XM_NO_INTRINSICS_
#include <DirectXMath.h>

#include <queue>
#include <list>
#include <atomic>
#include <memory>
#include <mutex>

#include "SceneOverlayData.h"
#include "SceneMarkerMath.h"

// TODO: most line vertices can be cached if the colouring is done through the vertex shader.
class CCampathDrawer
{
public:

	CCampathDrawer();
	~CCampathDrawer();

	void Begin();
	void End();

	void Draw_set(bool value);
	bool Draw_get(void);

	// Capture suppression. The keyframe gizmos, the trajectory poly-line and the follow-camera
	// marker are drawn INTO the scene render target (OnRenderThread_Draw runs off the world
	// pass, before UI), so they land in EVERY captured stream -- beauty, POV, free cam, depth,
	// isolation, all of them. A recorded frame must never contain them. This flag suppresses
	// production of the draw functors without disturbing the user's own Draw_set() choice, so
	// the gizmos come straight back when the job ends. Set by the Filmmaker render session for
	// the whole job lifetime; the drawer additionally self-gates on AfxStreams_IsRcording() so
	// a plain `mirv_streams record` outside the Render workspace is covered too.
	void SuppressForCapture_set(bool value) { m_SuppressForCapture = value; }
	bool SuppressForCapture_get() const { return m_SuppressForCapture; }

	void SetDrawKeyframeAxis(bool value) { m_DrawKeyframeAxis = value; }
	bool GetDrawKeyframeAxis() { return m_DrawKeyframeAxis;  }

	void SetDrawKeyframeCam(bool value) { m_DrawKeyframeCam = value; }
	bool GetDrawKeyframeCam() { return m_DrawKeyframeCam; }

	float GetDrawKeyframeIndex() { return m_DrawKeyframIndex; }
	void SetDrawKeyframeIndex(float value) { m_DrawKeyframIndex = value; }

	// Camera-marker styling (set by Filmmaker::CameraPath). When enabled, keyframes
	// are painted in the global Freeze (blue) / Live (gold) colour with a glow, and
	// the aimed-at marker (highlight) is drawn white. Plain members read on the
	// drawing thread; a benign one-frame-stale race at worst (visual only).
	// World-space box drawn around the entity the Follow inspector currently has selected.
	// The origin is the entity origin; the box rises "height" from it and is "half" wide either
	// way, which suits a pawn (origin at the feet) directly and a small object once the caller
	// lowers the origin by half its height.
	struct FollowTargetBox {
		bool enabled = false;
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float half = 0.0f;
		float height = 0.0f;
	};

	void MarkerStyle_set(bool enabled, bool freeze, int highlight) {
		m_MarkerStyle = enabled; m_MarkerFreeze = freeze; m_MarkerHighlight = highlight;
	}

	// Standalone follow-camera marker. This is independent of g_CamPath so placing
	// a follow camera does not create or leak path-camera keyframes.
	void FollowCameraMarker_set(bool enabled, const CamPathValue& value) {
		m_FollowCameraMarker = enabled; m_FollowCameraValue = value;
	}

	// Highlight for the selected follow target. A picking aid, not part of the camera: the
	// candidate list can only offer names and distances, and several rows legitimately read the
	// same ("Knife", "HE Grenade"), so nothing on screen says which body a pick refers to.
	void FollowTargetBox_set(const FollowTargetBox& box) { m_FollowTargetBox = box; }

	// Where the camera path puts the camera at the playhead tick: a dot on its line.
	void PathHead_set(bool enabled, double x, double y, double z) {
		m_PathHead = enabled; m_PathHeadPos[0] = x; m_PathHeadPos[1] = y; m_PathHeadPos[2] = z;
	}

	// Weapon Recoil's rotation pivot, drawn as a world-space wireframe sphere. It marks a point
	// ON THE WEAPON a few units from the eye, so it has to be a real 3D object sharing the
	// scene's depth -- a Panorama overlay can only paint a flat disc over the frame, which reads
	// as a UI dot rather than a thing sitting in the gun. Same standalone-marker contract as
	// FollowCameraMarker above: plain members, benign one-frame-stale race at worst.
	// TWO markers, one per effect that hinges the viewmodel (Weapon Recoil, Weapon Bob). They are
	// separate pivots with separate colours so the operator can tell which orb is which; a single
	// slot could only ever show one of them and would silently misreport the other.
	static const int kPivotMarkerSlots = 2;
	struct PivotMarker {
		bool enabled = false;
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float radius = 0.0f;
		unsigned char r = 255, g = 190, b = 60; // per-owner colour, set by the publisher
	};
	void PivotMarker_set(int slot, const PivotMarker& marker) {
		if (slot >= 0 && slot < kPivotMarkerSlots) m_PivotMarker[slot] = marker;
	}

	// True when the pivot marker is the ONLY thing this frame draws. CS2 renders the VIEWMODEL
	// after the ordinary gizmo point, so a marker meant to sit IN the weapon is painted over --
	// it is draw ORDER, which no depth state can fix. In this mode the render hook is allowed to
	// run the draw AGAIN on later 3D passes, so the marker also lands on top of the gun. Safe
	// only here: with `mirv_campath draw` off every camera gizmo is cleared from the snapshot,
	// so there is nothing else that could be drawn twice or against the wrong depth buffer.
	bool WantsLateRedraw() const { return !m_Draw && AnyPivotMarker(); }

	// The Environment's placed cameras and lights and the paths their keys take (SceneOverlayData.h).
	// Its own feature, like the pivot marker: drawn whether or not the camera path is, and also
	// while the Follow Camera owns the view. Swapped in whole from the main thread.
	void SceneOverlay_set(std::shared_ptr<const Filmmaker::SceneOverlay> overlay) {
		std::lock_guard<std::mutex> lock(m_SceneOverlayMutex);
		m_SceneOverlay = (overlay && !overlay->Empty()) ? std::move(overlay) : nullptr;
	}
	std::shared_ptr<const Filmmaker::SceneOverlay> SceneOverlay_get() {
		std::lock_guard<std::mutex> lock(m_SceneOverlayMutex);
		return m_SceneOverlay;
	}
	bool AnyPivotMarker() const {
		for (int i = 0; i < kPivotMarkerSlots; ++i) if (m_PivotMarker[i].enabled) return true;
		return false;
	}

	void BeginDevice(ID3D11Device * device);
	void EndDevice();
	void Reset();

	void OnRenderThread_Draw(ID3D11DeviceContext * pImmediateContext, const D3D11_VIEWPORT * pViewPort, ID3D11RenderTargetView * pRenderTargetView2, ID3D11DepthStencilView *pDepthStencilView2);
	void OnRenderThread_Present();
	// Frees the one-frame redraw functor retained for the pivot marker (see the .cpp).
	void ReleaseRetainedFunctor();

	void OnEngineThread_SetupViewDone();
	void OnEngineThread_EndFrame();

private:
	struct Vertex
	{
		FLOAT x, y, z; // Position of current line point
		DWORD diffuse; // Diffuse color of current line point
		FLOAT t0u, t0v, t0w; // Extrusion direction from current line point (-1/1), reserved
		FLOAT t1u, t1v, t1w; // Unit vector pointing to previous line point
		FLOAT t2u, t2v, t2w; // Unit vector pointing to next line point
	};

	struct TempPoint
	{
		double t;
		Vector3 y;
		TempPoint * nextPt;
	};

	class CDynamicProperties {
	public:
		CDynamicProperties(CCampathDrawer * drawer);

		double GetCurTime() const {
			return m_CurTime;
		}

		bool GetInCampath() const {
			return m_InCampath;
		}

		bool GetCampathEnabled() const {
			return m_CampathEnabled;
		}

		int GetScreenWidth() const {
			return m_ScreenWidth;
		}

		int GetScreenHeight() const {
			return m_ScreenHeight;
		}

		const SOURCESDK::VMatrix & GetWorldToScreenMatrix() const {
			return m_WorldToScreenMatrix;
		}

		const Vector3 & GetPlaneOrigin() const {
			return m_PlaneOrigin;
		}

		const Vector3 & GetPlaneNormal() const {
			return m_PlaneNormal;
		}
	
		const Vector3 & GetPlaneRight() const {
			return m_PlaneRight;
		}

		const Vector3 & GetPlaneUp() const {
			return m_PlaneUp;
		}

		float GetDrawKeyframeIndex() const {
			return m_DrawKeyFrameIndex;
		}

		bool GetDrawKeyframeAxis() const {
			return m_DrawKeyframeAxis;
		}

		bool GetDrawKeyframeCam() const {
			return m_DrawKeyframeCam;
		}

		const CamPathValue & GetCurrentValue() const {
			return m_CurrentValue;
		}

		bool GetMarkerStyle() const { return m_MarkerStyle; }
		bool GetMarkerFreeze() const { return m_MarkerFreeze; }
		int GetMarkerHighlight() const { return m_MarkerHighlight; }
		bool GetFollowCameraMarker() const { return m_FollowCameraMarker; }
		const CamPathValue& GetFollowCameraValue() const { return m_FollowCameraValue; }
		const FollowTargetBox& GetFollowTargetBox() const { return m_FollowTargetBox; }
		bool GetPathHead() const { return m_PathHead; }
		const double* GetPathHeadPos() const { return m_PathHeadPos; }
		const PivotMarker& GetPivotMarker(int slot) const { return m_PivotMarker[slot]; }
		// `mirv_campath draw` / the Filmmaker path asked for the camera path itself. False when only
		// a separate feature (pivot marker, follow target box, scene overlay) pulled the frame in.
		bool GetDrawCampath() const { return m_DrawCampath; }
		const std::shared_ptr<const Filmmaker::SceneOverlay>& GetSceneOverlay() const { return m_SceneOverlay; }

	private:
		double m_CurTime;
		bool m_InCampath;
		bool m_CampathEnabled;
		int m_ScreenWidth;
		int m_ScreenHeight;
		SOURCESDK::VMatrix m_WorldToScreenMatrix;
		Vector3 m_PlaneOrigin;
		Vector3 m_PlaneNormal;
		Vector3 m_PlaneRight;
		Vector3 m_PlaneUp;

		float m_DrawKeyFrameIndex;
		bool m_DrawKeyframeAxis;
		bool m_DrawKeyframeCam;

		bool m_MarkerStyle;
		bool m_MarkerFreeze;
		int m_MarkerHighlight;
		bool m_FollowCameraMarker;
		CamPathValue m_FollowCameraValue;
		FollowTargetBox m_FollowTargetBox;
		bool m_PathHead = false;
		double m_PathHeadPos[3] = {};
		PivotMarker m_PivotMarker[kPivotMarkerSlots];
		bool m_DrawCampath;
		std::shared_ptr<const Filmmaker::SceneOverlay> m_SceneOverlay;

		CamPathValue m_CurrentValue;
	};

	class CLessDynamicProperties {
	public:
		void AddRef() {
			++m_RefCount;
		}

		void Release() {
			int value = --m_RefCount;
			if(0 == value) {
				delete this;
			}
		}

		class CKeyframe {
		public:
			CKeyframe(double time, const CamPathValue & value)
			: m_Time(time)
			, m_Value(value) {

			}

			double GetTime() const {
				return m_Time;
			}

			const CamPathValue & GetValue() const {
				return m_Value;
			}

		private:
			double m_Time;
			CamPathValue m_Value;
		};

		CLessDynamicProperties(CCampathDrawer * drawer);

		bool GetCampathCanEval() const {
			return m_CampathCanEval;
		}

		std::list<CKeyframe>::const_iterator GetKeyframesBegin() const {
			return m_Keyframes.cbegin();
		}

		std::list<CKeyframe>::const_iterator GetKeyframesEnd() const {
			return m_Keyframes.cend();
		}

		size_t GetKeyframesSize() const {
			return m_Keyframes.size();
		}

		std::list<CKeyframe>::const_iterator GetTrajectoryPointsBegin() const {
			return m_TrajectoryPoints.cbegin();
		}

		std::list<CKeyframe>::const_iterator GetTrajectoryPointsEnd() const {
			return m_TrajectoryPoints.cend();
		}

	private:
		std::atomic_int m_RefCount = 0;
		bool m_CampathCanEval;
		std::list<CKeyframe> m_Keyframes;
		std::list<CKeyframe> m_TrajectoryPoints;
	};

	class CCampathDrawerFunctor : public CMaterialSystemFunctor {
	public:
		CCampathDrawerFunctor(CCampathDrawer * drawer) {
			m_DynamicProperties = new CDynamicProperties(drawer);
			m_LessDynamicProperties = drawer->m_LessDynamicProperties;
			m_LessDynamicProperties->AddRef();
		}

		virtual ~CCampathDrawerFunctor() {
			m_LessDynamicProperties->Release();
			delete m_DynamicProperties;
		}

		virtual void operator()();

	private:
		CDynamicProperties * m_DynamicProperties;
		CLessDynamicProperties * m_LessDynamicProperties;
	};

	std::mutex m_FunctorMutex;
	std::list<CMaterialSystemFunctor*> m_Functors;

	bool m_DrawKeyframeAxis = false;
	bool m_DrawKeyframeCam = true;

	bool m_MarkerStyle = false;
	bool m_MarkerFreeze = false;
	int m_MarkerHighlight = -1;
	bool m_FollowCameraMarker = false;
	CamPathValue m_FollowCameraValue;
	bool m_PathHead = false;
	double m_PathHeadPos[3] = {};
	// The draw functor is normally consumed once per frame. While the pivot marker is the only
	// content we RETAIN it and re-run it on every later 3D pass, so the marker ends up on top of
	// the viewmodel whatever order CS2 draws a given weapon in -- re-arming alone was not enough,
	// because the engine only queues one or two functors and some weapons need more passes.
	// Freed at present, and on device teardown.
	CMaterialSystemFunctor * m_RetainedFunctor = nullptr;
	std::mutex m_SceneOverlayMutex;
	std::shared_ptr<const Filmmaker::SceneOverlay> m_SceneOverlay;
	FollowTargetBox m_FollowTargetBox;
	PivotMarker m_PivotMarker[kPivotMarkerSlots];

	ID3D11Device * m_Device = nullptr;
	ID3D11DeviceContext * m_DeviceContext = nullptr;
	ID3D11DeviceContext * m_ImmediateContext = nullptr;
	const D3D11_VIEWPORT * m_pViewPort = nullptr;
	ID3D11RenderTargetView * m_Rtv2 = nullptr;
	ID3D11DepthStencilView * m_Dsv2 = nullptr;

	ID3D11DepthStencilState * m_DepthStencilStateDigits = nullptr;
	ID3D11DepthStencilState * m_DepthStencilStateLines = nullptr;
	// Depth-IGNORING variant, used only by the pivot marker. The marker annotates a point a
	// few units from the eye, inside the weapon -- and the viewmodel is drawn over this pass,
	// so a depth-tested marker is hidden by the very gun it is describing and looks broken.
	ID3D11DepthStencilState * m_DepthStencilStateNoDepth = nullptr;
	ID3D11SamplerState * m_SamplerState = nullptr;
	ID3D11RasterizerState * m_RasterizerStateDigits = nullptr;
	ID3D11RasterizerState * m_RasterizerStateLines = nullptr;
	// The solid markers: no culling, their faces are picked on the CPU (SceneMarkerMath.h).
	ID3D11RasterizerState * m_RasterizerStateSolid = nullptr;
	ID3D11BlendState * m_BlendState = nullptr;
	ID3D11BlendState * m_BlendStateAdditive = nullptr; // additive blend for the marker glow
	ID3D11InputLayout * m_InputLayoutDigits = nullptr;
	ID3D11InputLayout * m_InputLayoutLines = nullptr;

	struct VS_CONSTANT_BUFFER {
		DirectX::XMFLOAT4X4 matrix;
		DirectX::XMFLOAT4 plane0;
		DirectX::XMFLOAT4 paneN;
		DirectX::XMFLOAT4 screenInfo;
	};
	ID3D11Buffer * m_ConstantBuffer = nullptr;

	struct VS_CONSTANT_BUFFER_WIDTH {
		DirectX::XMFLOAT4 width;
	};
	ID3D11Buffer * m_ConstantBufferWidth = nullptr;

	bool m_Draw = false;
	std::atomic_bool m_SuppressForCapture = false;
	DWORD m_OldCurrentColor;
	Vector3 m_OldPreviousPolyLinePoint;
	ID3D11PixelShader * m_PixelShader = nullptr;
	bool m_PolyLineStarted;
	ID3D11VertexShader * m_VertexShader = nullptr;
	ID3D11Buffer * m_VertexBuffer = nullptr;
	UINT m_VertexBufferVertexCount = 0; // c_VertexBufferVertexCount
	// The buffer holds solid triangles, not line quads: the first solid flushes pending lines and
	// the first line (or any flush) flushes pending solids.
	bool m_SolidBatch = false;
	Vertex * m_LockedVertexBuffer = nullptr;
	float m_DrawKeyframIndex = 18.0f;
	ID3D11Texture2D* m_DigitsTexture = nullptr;
	ID3D11ShaderResourceView * m_DigitsTextureRV = nullptr;
	ID3D11VertexShader * m_DrawTextureVertexShader = nullptr;
	ID3D11PixelShader * m_DrawTextureShader = nullptr;

	CLessDynamicProperties * m_LessDynamicProperties = nullptr;

	static void CampathChangedFn(void * pUserData);

	void CamPathChanged();

	void BuildPolyLinePoint(Vector3 previous, Vector3 current, DWORD currentColor, Vector3 next, Vertex * pOutVertexData);

	void BuildSingleLine(Vector3 from, Vector3 to, Vertex * pOutVertexData);
	void BuildSingleLine(DWORD colorFrom, DWORD colorTo, Vertex * pOutVertexData);

	void BuildSingleQuad(Vector3 p0, float t0x, float t0y, Vector3 p1, float t1x, float t1y, Vector3 p2, float t2x, float t2y, Vector3 p3, float t3x, float t3y, Vertex * pOutVertexData);

	void AutoSingleLine(Vector3 from, DWORD colorFrom, Vector3 to, DWORD colorTo);
	void AutoSingleLineFlush();

	void AutoPolyLineStart();
	void AutoPolyLinePoint(Vector3 previous, Vector3 current, DWORD colorCurrent, Vector3 next);
	void AutoPolyLineFlush();

	void AutoSingleQuad(Vector3 p0, float t0x, float t0y, Vector3 p1, float t1x, float t1y, Vector3 p2, float t2x, float t2y, Vector3 p3, float t3x, float t3y);
	void AutoSingleQuadFlush();

	bool LockVertexBuffer();
	void UnlockVertexBuffer();
	void UnloadVertexBuffer();

	/// <summary>For reducing the number of points.</summary>
	static void RamerDouglasPeucker(TempPoint * start, TempPoint * end, double epsilon);
	static double ShortestDistanceToSegment(TempPoint * pt, TempPoint * start, TempPoint * end);

	void DrawCamera(const CamPathValue & cpv, DWORD colour, int screenWidth, int screenHeight, double sizeScale = 1.0);

	// The in-world markers (SceneMarkerMath.h): solid, shaded shapes with a crisp edge, drawn with
	// the line shader -- a vertex with no extrusion lands on its own point.
	struct MarkerView { Filmmaker::Marker::Vec eye, forward, right, up, light; };
	struct MarkerEdge { Vector3 from, to; };
	void AutoSolidTri(Vector3 a, DWORD colourA, Vector3 b, DWORD colourB, Vector3 c, DWORD colourC);
	void AutoSolidFlush();
	void DrawMarkerEdges(const std::vector<MarkerEdge> & edges, DWORD colour, float pixelWidth);
	void DrawMarkerBox(const MarkerView & view, Filmmaker::Marker::Vec centre, const Filmmaker::Marker::Vec axes[3], const double half[3],
		Filmmaker::Marker::Rgb rgb, const Filmmaker::Marker::Rgb * frontFace, unsigned char alpha);
	void DrawMarkerSphere(const MarkerView & view, Filmmaker::Marker::Vec centre, double radius, Filmmaker::Marker::Rgb rgb);
	void DrawMarkerDisc(const MarkerView & view, Filmmaker::Marker::Vec centre, double radius, DWORD centreColour, DWORD rimColour);
	void DrawMarkerRing(const MarkerView & view, Filmmaker::Marker::Vec centre, double radius, DWORD colour, float pixelWidth);
	void DrawCameraMarker(const MarkerView & view, const CamPathValue & cpv, Filmmaker::Marker::Rgb rgb, bool selected,
		int screenWidth, int screenHeight, double sizeScale);
	void DrawLightMarker(const MarkerView & view, const Filmmaker::SceneOverlay::Glyph & glyph, double sizeScale);
	void DrawPathMarker(const MarkerView & view, const std::vector<Vector3> & points, const std::vector<Vector3> & keys,
		Filmmaker::Marker::Rgb rgb, unsigned char alpha, float pixelWidth, bool selected);

	void OnPostRenderAllTools_DrawingThread(CDynamicProperties * dynamicPorperties, CLessDynamicProperties * lessDynamicProperties);

	void SetLineWidth(float lineWidth);
};

extern CCampathDrawer g_CampathDrawer;
