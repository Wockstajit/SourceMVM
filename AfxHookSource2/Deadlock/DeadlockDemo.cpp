#include "DeadlockDemo.h"
#include "DeadlockAddresses.h"
#include "DeadlockEngine.h"
#include "../../Shared/AfxConsole.h"
#include <cmath>

namespace Deadlock {
void* Demo_Player() {
    __try {
        void* player = g_DemoPlayerPointer ? *g_DemoPlayerPointer : nullptr;
        return player && g_DemoPlayerVtable && *(size_t*)player == g_DemoPlayerVtable
            ? player : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

namespace {
template<typename T> T Call(int slot, T neutral) {
    __try {
        void* player = Demo_Player();
        if (!player || slot < 0) return neutral;
        return ((T(__fastcall*)(void*))(*(void***)player)[slot])(player);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return neutral; }
}
double GlobalFloat(size_t offset) {
    __try {
        unsigned char* globals = g_GlobalVars ? *g_GlobalVars : nullptr;
        if (!globals) return 0;
        float value = *(float*)(globals + offset);
        return std::isfinite(value) ? value : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
}
bool Demo_IsPlaying() { return Call<bool>(Engine_GetSlot(EngineSlot::DemoIsPlaying), false); }
bool Demo_IsPaused() { return Call<bool>(Engine_GetSlot(EngineSlot::DemoIsPaused), false); }
bool Demo_IsSeeking() { return Call<bool>(Engine_GetSlot(EngineSlot::DemoIsSeeking), false); }
int Demo_Tick() { return Demo_IsPlaying() ? Call<int>(Engine_GetSlot(EngineSlot::DemoGetTick), -1) : -1; }
int Demo_StartTick() { return Call<int>(Engine_GetSlot(EngineSlot::DemoGetStartTick), -1); }
double Demo_PlaybackScale() { return Call<float>(Engine_GetSlot(EngineSlot::DemoGetPlaybackScale), 0); }
double Demo_Time() { return Demo_IsPlaying() ? GlobalFloat(0x30) : 0; }
double Demo_IntervalPerTick() {
    double interval = GlobalFloat(0x54);
    return interval >= 0.001 && interval <= 0.1 ? interval : 0;
}
void Demo_Report() {
    advancedfx::Message("  demo player: global=%p player=%p vtable=%s seeking=%d start=%d scale=%.3f time=%.6f interval=%.6f\n",
        g_DemoPlayerPointer, Demo_Player(), Demo_Player() ? "OK" : "unavailable",
        Demo_IsSeeking(), Demo_StartTick(), Demo_PlaybackScale(), Demo_Time(), Demo_IntervalPerTick());
}
}
