#pragma once

#include <d3d11.h>

#include <functional>
#include <atomic>
#include <queue>
#include <mutex>

class IRenderPassCommands {
public:
};

class CRenderCommands {
public:
    CRenderCommands();
    ~CRenderCommands();

    typedef std::function<void()> Fn;
    typedef std::function<void(ID3D11DeviceContext * pDeviceContext)> FnContext;
    typedef std::function<void(ID3D11DeviceContext * pDeviceContext, ID3D11Texture2D * pTexture)> FnContextTexture;
    typedef std::function<void(ID3D11DeviceContext * pDeviceContext, ID3D11RenderTargetView * pTarget)> FnContextTarget;

    template<class T> class CQueue {
    public:
        ~CQueue() {
            Clear();
        }

        void Clear() {
           while(!Empty()) Pop();
        }

        void Push(T &&value) {
            m_Queue.push(value);
        }

        bool Empty() {
            return m_Queue.empty();
        }

        T & Front() {
            return m_Queue.front();
        }

        void Pop() {
            m_Queue.pop();
        }

    private:
        std::queue<T> m_Queue;
    };
    
    class CRenderPassCommands :public IRenderPassCommands {
    public:
        ~CRenderPassCommands() {
            Finalize();
        }

        ID3D11DeviceContext * Context = nullptr;

        void Finalize(){
            PassOrdinal = -1;
            BeginReliable.Clear();
			EndReliable.Clear();
            BeforeUi.Clear();
            BeforeUi2.Clear();
            BeforeClear.Clear(); // an aborted pass must not carry its clear override into a reused one
            BeforePresent.Clear();
            AfterPresent.Clear();

            OnAfterPresentOrContextLossReliable();

            while(!FinalizeReliable.Empty()) {
                FinalizeReliable.Front()();
                FinalizeReliable.Pop();
            }
        }

        // Which render pass of the capture frame this object belongs to: 0 = main pass,
        // 1..N = the N distinct extra-pass groups, -1 = not tagged (no capture running).
        // Set on the ENGINE thread when the pass begins; consumed on the render thread at
        // dequeue to re-sync the in-stream pass ordinal (see RenderThread_PassOrdinal).
        int PassOrdinal = -1;

        CQueue<Fn> BeginReliable;
        
        CQueue<Fn> BeforeClear;

		// Runs on the render thread after the pass has finished, even when normal capture
		// callbacks were not reached. Pass-local D3D hook policy must be reset here rather
		// than on the engine thread, which can already be preparing the following pass.
		CQueue<Fn> EndReliable;

        CQueue<FnContextTexture> BeforeUi;
        
        CQueue<FnContextTarget> BeforeUi2;

        CQueue<FnContextTexture> BeforePresent;

        CQueue<FnContext> AfterPresent;

        CQueue<FnContext> AfterPresentOrContextLossReliable;

        CQueue<Fn> FinalizeReliable;

        void OnBeforeClear() {
            while(!BeforeClear.Empty()) {
                BeforeClear.Front()();
                BeforeClear.Pop();
            }
        }

        void OnBeforeUi(ID3D11Texture2D * pTexture) {
            if(Context && pTexture) {
                while(!BeforeUi.Empty()) {
                    BeforeUi.Front()(Context, pTexture);
                    BeforeUi.Pop();
                }
            } else {
                BeforeUi.Clear();
            }
        }

        void OnBeforeUi2(ID3D11RenderTargetView * pTarget) {
            if(Context && pTarget) {
                while(!BeforeUi2.Empty()) {
                    BeforeUi2.Front()(Context, pTarget);
                    BeforeUi2.Pop();
                }
            } else {
                BeforeUi2.Clear();
            }
        }        
        
        void OnBeforePresent(ID3D11Texture2D * pTexture) {
            if(Context && pTexture) {
                while(!BeforePresent.Empty()) {
                    BeforePresent.Front()(Context, pTexture);
                    BeforePresent.Pop();
                }
            } else {
                BeforePresent.Clear();
            }
        }

        void OnAfterPresent() {
            if(Context) {
                while(!AfterPresent.Empty()) {
                    AfterPresent.Front()(Context);
                    AfterPresent.Pop();
                }
            } else {
                AfterPresent.Clear();
            }
        }

        void OnAfterPresentOrContextLossReliable() {
            if(Context) {
                while(!AfterPresentOrContextLossReliable.Empty()) {
                    AfterPresentOrContextLossReliable.Front()(Context);
                    AfterPresentOrContextLossReliable.Pop();
                }
            } else {
                AfterPresentOrContextLossReliable.Clear();
            }
            SetContext(nullptr);
        }

        void SetContext(ID3D11DeviceContext* pContext) {
            if(Context) Context->Release();
            Context = pContext;
            if(pContext) pContext->AddRef();
        }

        ID3D11DeviceContext* GetContext() const {
            return Context;
        }        
    };

    CRenderPassCommands & EngineThread_GetCommands();
   
    void EngineThread_EndFrame();

    CRenderPassCommands * RenderThread_GetCommands();

    // Render thread. Late retry of the pass-commands dequeue.
    //
    // WHY THIS EXISTS. RenderThread_BeginFrame runs inside the previous pass's Present callback.
    // If the ENGINE thread has not called EngineThread_EndFrame for the next pass at that exact
    // instant, the dequeue comes back empty and the whole next pass used to run with null
    // commands: its BeginReliable state (depth preservation, chroma clear colour) was silently
    // skipped and then executed one pass LATE by the following BeginFrame. Measured 2026-08-08:
    // that skew is why isolation passes saw the preserve flag down at their own depth clears
    // (nondeterministic x-ray) while MAIN-pass clears saw it up.
    //
    // The pass's own D3D commands are only emitted by the engine AFTER it submitted the pass
    // object, so by the time any hook of this pass runs, the object is guaranteed to be in the
    // queue -- a retry at first use closes the race deterministically. Idempotent; no-op when
    // the commands were already dequeued.
    void RenderThread_EnsureCommands();

    // In-stream pass ordinal for the CURRENT render-thread window: 0 = main pass, k >= 1 = the
    // k-th distinct extra-pass group of this capture frame, -1 = unknown (no capture).
    //
    // WHY THIS EXISTS. Per-pass D3D policy (depth preservation, chroma clear) cannot ride
    // engine-thread state: direct publication runs one pass EARLY (the engine builds pass N+1
    // while the render thread executes pass N) and the queued BeginReliable path runs LATE for
    // clears the scene system emits during FrameUpdate, before the pass object is even
    // submitted -- both measured 2026-08-09. The only race-free clock is the command stream
    // itself: the per-pass Present callback advances this ordinal IN-STREAM, and the dequeued
    // object's PassOrdinal re-syncs it. Policy is then a lookup into a job-static schedule.
    int  RenderThread_PassOrdinal() const { return m_RenderThread_PassOrdinal.load(std::memory_order_acquire); }
    void RenderThread_AdvancePassOrdinal() {
        // Unknown stays unknown: counting windows only means something once a tagged object
        // has anchored the count to a real pass index.
        int v = m_RenderThread_PassOrdinal.load(std::memory_order_acquire);
        if (v >= 0) m_RenderThread_PassOrdinal.store(v + 1, std::memory_order_release);
    }

    bool RenderThread_FrameBegun();

    void RenderThread_BeginFrame();

    void RenderThread_EndFrame();

    void RenderThread_SetContext(ID3D11DeviceContext* pContext) {
        if(m_RenderThreadCommands) m_RenderThreadCommands->SetContext(pContext);
    }

    ID3D11DeviceContext* RenderThread_GetContext() const {
        if(m_RenderThreadCommands) return m_RenderThreadCommands->GetContext();
        return nullptr;
    }

    bool CurrentThreadIsRenderThread() const;

private:  
    std::mutex m_CommandsQueueMutex;
    std::queue<CRenderPassCommands *> m_CommandsQueue;
    bool m_QueuedCommands = false;
    std::mutex m_ReusableMutex;
    std::queue<CRenderPassCommands *> m_Reusable;
    CRenderPassCommands * m_EngineThreadCommands = nullptr;
    CRenderPassCommands * m_RenderThreadCommands = nullptr;
    bool m_RenderThread_FrameBegun = false;
    std::atomic<int> m_RenderThread_PassOrdinal{-1};
    DWORD m_CurrentThreadId=0;
};
 
