#pragma once

#include "../Panorama/PanoramaBridge.h"

#include <mutex>
#include <string>
#include <vector>

namespace Filmmaker {

class SceneEditorHud {
public:
	void RunFrame();
	void PushWheel(int notches);
	void PushChar(unsigned charCode);
	bool TextCaptureActive() const { return m_textCapture; }
	void RequestEval(const std::string &js) {
		std::lock_guard<std::mutex> lk(m_evalMutex);
		m_evalQueue.push_back(js);
	}

private:
	void *FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	void UpdateScaleRequest();
	void DispatchWheel();
	void DispatchChars();

	PanoramaBridge m_bridge;
	std::mutex m_evalMutex;
	std::vector<std::string> m_evalQueue;
	std::mutex m_wheelMutex;
	int m_pendingWheel = 0;
	std::mutex m_charMutex;
	std::vector<unsigned> m_pendingChars;

	void *m_hudPanel = nullptr;
	void *m_root = nullptr;
	short m_symState = -1;
	short m_symTextCapture = -1;
	short m_symPreviewRect = -1;
	bool m_built = false;
	bool m_wasEnabled = false;
	bool m_textCapture = false;
	std::string m_lastState;
};

SceneEditorHud &SceneEditorHudRef();

} // namespace Filmmaker
