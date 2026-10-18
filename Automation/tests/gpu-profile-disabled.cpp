// Rendering fixtures exercise the passes without opening a profiler frame.
#include "../../AfxHookSource2/Filmmaker/Debug/GpuPassProfiler.h"
namespace Filmmaker::GpuProfile {
bool Enabled(){return false;}
unsigned PassId(const char*){return 0;}
Scope::Scope(ID3D11DeviceContext*,unsigned){}
Scope::~Scope(){}
void NoteSampled(const char*,double,unsigned){}
void NoteSampledProgram(uint64_t,double,unsigned){}
}
