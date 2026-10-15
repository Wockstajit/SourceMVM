#pragma once
#include <cstdint>
namespace Filmmaker::Environment {
// Allocation granularity is independent of the caller's existing capture limit.
// Return zero for overflow or a request beyond that limit; never truncate geometry.
inline uint32_t CasterBufferCapacity(uint64_t required,uint32_t limit) {
    if(!required||required>limit)return 0;
    uint32_t capacity=65536;
    while(capacity<required){
        if(capacity>limit/2){capacity=limit;break;}
        capacity*=2;
    }
    return capacity>limit?limit:capacity;
}
}
