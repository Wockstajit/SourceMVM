#include <RenderCommands.h>

CRenderCommands::CRenderCommands() {
    m_EngineThreadCommands = new CRenderPassCommands();
}

CRenderCommands::~CRenderCommands() {
    {
        std::unique_lock<std::mutex> lock(m_CommandsQueueMutex);
        while(!m_CommandsQueue.empty()) {
            delete m_CommandsQueue.front();
            m_CommandsQueue.pop();
        }
    }
    {
        std::unique_lock<std::mutex> lock(m_ReusableMutex);
        while(!m_Reusable.empty()) {
            delete m_Reusable.front();
            m_Reusable.pop();
        }
    }    
}

CRenderCommands::CRenderPassCommands & CRenderCommands::EngineThread_GetCommands() {
    return *m_EngineThreadCommands;
}

void CRenderCommands::EngineThread_EndFrame() {

    // Submit current frame:

    CRenderPassCommands* pRenderPassCommands = m_EngineThreadCommands;
    m_EngineThreadCommands = nullptr;

    {
        std::unique_lock<std::mutex> lock(m_CommandsQueueMutex);
        m_CommandsQueue.emplace(pRenderPassCommands);
    }

    // Prepare next frame:

    {
        std::unique_lock<std::mutex> lock(m_ReusableMutex);
        if (!m_Reusable.empty()) {
            m_EngineThreadCommands = m_Reusable.front();
            m_Reusable.pop();
        }
    }

    if (m_EngineThreadCommands == nullptr) m_EngineThreadCommands = new CRenderPassCommands();
}

void CRenderCommands::RenderThread_BeginFrame() {
    m_RenderThread_FrameBegun = true;
    m_CurrentThreadId = GetCurrentThreadId();
    m_RenderThreadCommands = nullptr;

    RenderThread_EnsureCommands();
}

void CRenderCommands::RenderThread_EnsureCommands() {
    // Only meaningful between BeginFrame and EndFrame, and only when BeginFrame's own dequeue
    // came back empty (the engine thread had not submitted the pass yet at Present time).
    if (!m_RenderThread_FrameBegun || m_RenderThreadCommands) return;

    {
        std::unique_lock<std::mutex> lock(m_CommandsQueueMutex);
        if (m_CommandsQueue.empty()) return;
        m_RenderThreadCommands = m_CommandsQueue.front();
        m_CommandsQueue.pop();
    }

    // Anchor the in-stream pass ordinal to the object's engine-side tag. This is the ONLY
    // authoritative sync point; the per-pass Present callback advances it between objects.
    if (m_RenderThreadCommands->PassOrdinal >= 0)
        m_RenderThread_PassOrdinal.store(m_RenderThreadCommands->PassOrdinal, std::memory_order_release);

    auto& beginFrameReliable = m_RenderThreadCommands->BeginReliable;
    while (!beginFrameReliable.Empty()) {
        beginFrameReliable.Front()();
        beginFrameReliable.Pop();
    }
}

CRenderCommands::CRenderPassCommands * CRenderCommands::RenderThread_GetCommands() {
    // Same late-retry as the state hooks: a consumer asking for the pass commands IS proof the
    // pass is executing, so the object must be available by now if the engine submitted late.
    RenderThread_EnsureCommands();
    return m_RenderThreadCommands;
}

bool CRenderCommands::RenderThread_FrameBegun() {
    return m_RenderThread_FrameBegun;
}

void CRenderCommands::RenderThread_EndFrame() {
    if(m_RenderThreadCommands) {
		while (!m_RenderThreadCommands->EndReliable.Empty()) {
			m_RenderThreadCommands->EndReliable.Front()();
			m_RenderThreadCommands->EndReliable.Pop();
		}
        m_RenderThreadCommands->Finalize();
        {
            std::unique_lock<std::mutex> lock(m_ReusableMutex);
            m_Reusable.emplace(m_RenderThreadCommands);
        }
        m_RenderThreadCommands = nullptr;
    }

    m_RenderThread_FrameBegun = false;
}

bool CRenderCommands::CurrentThreadIsRenderThread() const {
    return m_RenderThread_FrameBegun &&  GetCurrentThreadId() == m_CurrentThreadId;
}
