#include "../../AfxHookSource2/Filmmaker/Deadlock/DeadlockRenderMath.h"
#include <iostream>
int main() {
    using Filmmaker::DeadlockRenderMath::FrameCount;
    for(int rate : {24,30,50,60,120,144,240,1000}) {
        if (FrameCount(5700,5764,1.0/64,rate) != rate+1) return 1;
        if (FrameCount(5700,5700,1.0/64,rate) != 1) return 2;
    }
    if (FrameCount(5700,5716,1.0/64,120)!=31) return 3;
    if (FrameCount(0,1,1.0/64,24)!=1) return 4;
    if (FrameCount(0,64*3600,1.0/64,144)!=518401) return 5;
    if (FrameCount(10,9,1.0/64,60)!=0) return 6;
    // High-FPS capture (600 fps over 64 ticks → 601 inclusive samples).
    if (FrameCount(5700,5764,1.0/64,600)!=601) return 7;
    std::cout << "Inclusive capture endpoints pass at preset and custom FPS.\n";
}
