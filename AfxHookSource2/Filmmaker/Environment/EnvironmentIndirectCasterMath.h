#pragma once
#include <cstdint>
#include <cstring>
namespace Filmmaker::Environment {
// D3D11 indirect records start with vertex/index count and instance count.
// Vendor multi-draw can separate records with additional padding.
inline uint32_t IndirectCasterSpan(uint32_t bytes,uint32_t offset,uint32_t stride,uint32_t count,bool indexed) {
    const uint32_t record=indexed?20:16;
    if(!count||offset%4||stride%4||stride<record)return 0;
    const uint64_t span=uint64_t(count-1)*stride+record;
    if(span>1024*1024||uint64_t(offset)+span>bytes)return 0;
    return uint32_t(span);
}
inline bool IndirectCasterVertices(const void* data,uint32_t span,uint32_t stride,uint32_t count,
                                   bool indexed,uint32_t capacity,uint32_t& vertices) {
    vertices=0;
    if(!data||!IndirectCasterSpan(span,0,stride,count,indexed))return false;
    uint64_t total=0;
    for(uint32_t i=0;i<count;++i){uint32_t words[2]{};
        std::memcpy(words,static_cast<const unsigned char*>(data)+uint64_t(i)*stride,sizeof(words));
        if(!words[1])continue; // GPU culling writes zero instances.
        if(words[0]%3)return false;
        total+=uint64_t(words[0])*words[1];
        if(total>capacity)return false;
    }
    vertices=uint32_t(total);return true;
}
}
