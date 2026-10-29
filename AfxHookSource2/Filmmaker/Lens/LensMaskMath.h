#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace Filmmaker::Lens {
struct MaskPixels {
    unsigned width=0,height=0;
    std::vector<unsigned char> pixels;
};
// Area-reduce RGBA to luminance times alpha, retaining thin images and the full
// source extent. Integer overlap weights avoid rounding a white box below white.
inline MaskPixels ReduceMaskRgba(unsigned width,unsigned height,const std::vector<unsigned char>& rgba) {
    MaskPixels result;
    const uint64_t area=uint64_t(width)*height;
    if(!area || area>rgba.size()/4 || area>(std::numeric_limits<uint64_t>::max)()/255) return result;
    const unsigned largest=(std::max)(width,height);
    result.width=largest<=512 ? width : (std::max)(1u,unsigned((uint64_t(width)*512+largest/2)/largest));
    result.height=largest<=512 ? height : (std::max)(1u,unsigned((uint64_t(height)*512+largest/2)/largest));
    result.pixels.resize(size_t(result.width)*result.height);
    for(unsigned y=0;y<result.height;++y) for(unsigned x=0;x<result.width;++x) {
        const uint64_t x0=uint64_t(x)*width,x1=uint64_t(x+1)*width;
        const uint64_t y0=uint64_t(y)*height,y1=uint64_t(y+1)*height;
        uint64_t sum=0;
        for(unsigned sy=unsigned(y0/result.height);sy<=(y1-1)/result.height;++sy) {
            const uint64_t wy=(std::min)(y1,uint64_t(sy+1)*result.height)-(std::max)(y0,uint64_t(sy)*result.height);
            for(unsigned sx=unsigned(x0/result.width);sx<=(x1-1)/result.width;++sx) {
                const uint64_t wx=(std::min)(x1,uint64_t(sx+1)*result.width)-(std::max)(x0,uint64_t(sx)*result.width);
                const auto* p=&rgba[(size_t(sy)*width+sx)*4];
                const unsigned cover=(((p[0]*54+p[1]*183+p[2]*19)>>8)*p[3]/255);
                sum+=cover*wx*wy;
            }
        }
        result.pixels[size_t(y)*result.width+x]=(unsigned char)(sum/area);
    }
    return result;
}
}
