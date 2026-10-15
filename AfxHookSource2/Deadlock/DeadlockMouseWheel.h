#pragma once
// Wheel observer on its own message thread. Never replaces SDL's raw-input registration.
// Only foreground Deadlock events are accumulated; the main-thread input owner consumes them.
namespace Deadlock {
void MouseWheel_Initialize();
void MouseWheel_PumpMessages();
// Whole signed notches, retaining high-resolution fractional carry between calls.
int MouseWheel_ConsumeDelta();
void MouseWheel_Report();
}
