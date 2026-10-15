#pragma once

// SOURCE:MVM debug-command surface: the mvm_debug console command (session-wide
// compressed diagnostic log) lives in FilmmakerDebugCommand.cpp, and MvmCommandTrace is
// the RAII tracer mirv_filmmaker's dispatcher instantiates so that, while mvm_debug is
// running, every mirv_filmmaker invocation is logged and followed by a one-line dump of
// the touched subsystem's state (follow / camera / fx / graph editor / cosmetics).

#include <string>

namespace advancedfx {
class ICommandArgs;
}

namespace Filmmaker {

class MvmCommandTrace {
public:
	explicit MvmCommandTrace(advancedfx::ICommandArgs* args);
	~MvmCommandTrace();

private:
	std::string m_sub;
};

} // namespace Filmmaker
