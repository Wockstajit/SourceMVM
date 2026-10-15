#pragma once
#include <string>
namespace Filmmaker {
struct RenderSettings;
// A job owns one encoder. Start/finish/poll on main; Write on render, serialized internally.
bool DeadlockEncoder_Start(const RenderSettings& settings, const std::wstring& take, int width, int height);
bool DeadlockEncoder_Write(const void* pixels, unsigned pitch, unsigned width, unsigned height, bool bgra);
void DeadlockEncoder_Finish();
// 0 running, 1 success, -1 failed. Also reaps the process when finished.
int DeadlockEncoder_Poll();
const char* DeadlockEncoder_Error();
std::wstring DeadlockEncoder_ResolveExe();
}
