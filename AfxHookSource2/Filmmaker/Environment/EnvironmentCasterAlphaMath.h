#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
namespace Filmmaker::Environment {
inline uint64_t CasterAlphaFingerprint(unsigned width,unsigned height,const std::vector<unsigned char>& pixels) {
    uint64_t hash=14695981039346656037ull;
    for(unsigned v:{width,height})for(unsigned b=0;b<4;++b){hash^=(v>>(b*8))&255;hash*=1099511628211ull;}
    for(auto v:pixels){hash^=v;hash*=1099511628211ull;}
    return hash;
}
// Source images remain immutable and alive for this upload's duration. The
// fingerprint narrows candidates; dimensions and every byte determine identity.
// Cutoff, animation and sampler addressing belong to each draw, not its image.
class CasterAlphaImageIndex {
    struct Image {unsigned width,height;const std::vector<unsigned char>* pixels;};
    std::vector<Image> images;
    std::unordered_map<uint64_t,std::vector<unsigned>> buckets;
public:
    unsigned Intern(unsigned width,unsigned height,const std::vector<unsigned char>& pixels,uint64_t fingerprint) {
        auto& candidates=buckets[fingerprint];
        for(unsigned index:candidates){const auto& image=images[index];
            if(image.width==width&&image.height==height&&*image.pixels==pixels)return index;}
        const unsigned index=unsigned(images.size());images.push_back({width,height,&pixels});candidates.push_back(index);return index;
    }
    unsigned Size()const{return unsigned(images.size());}
};
}
