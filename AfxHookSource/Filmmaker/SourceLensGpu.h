#pragma once
#include <cstdint>
#include "SourceLensSmoke.h"
namespace SourceFilmmaker {
void LensGpu_Precache();
// Main-thread read: after initial preparation, an off lens queues no render work.
bool LensGpu_Precached(uint64_t pictureRevision);
bool LensGpu_Prepared();
bool LensGpu_DepthPrepared();
// HLAE's render-thread functors own every call below.
bool LensGpu_Begin(bool lens,bool fisheye,uint64_t pictureRevision);
void LensGpu_DepthBegin(const LensSmokeFrame& smoke);
void LensGpu_DepthEnd();
void LensGpu_Render(const float* worldToClip,const float* sunDirection,bool holdFocus,bool lens,bool fisheye);
void LensGpu_End(bool began);
void LensGpu_DeviceLost();
}
