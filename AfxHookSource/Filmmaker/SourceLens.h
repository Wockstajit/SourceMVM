#pragma once

// CS:S's lens render boundary. Controls/masks and effect shaders are shared with CS2.
// The engine's HUD paint is entered on the main thread; HLAE queues graphics work at that point.
namespace SourceFilmmaker {
void Lens_Frame();
void Lens_DepthMaterialScope(void* context,bool enter);
void Lens_ViewBegin(bool holdFocus=false,int drawFlags=1);
void Lens_DeviceLost();
void Lens_ViewEnd();
void Lens_PrintHealth();
} // namespace SourceFilmmaker
