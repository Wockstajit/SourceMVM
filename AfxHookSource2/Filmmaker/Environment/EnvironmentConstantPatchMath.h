#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Filmmaker::Environment {
// Sparse CPU writes to a GPU-owned constant buffer. Never upload the gaps: they
// contain native material values. A later GPU copy cancels only covered writes;
// CPU writes after that copy take precedence again when the batch is uploaded.
class ConstantPatchBatch {
public:
    struct Range {unsigned begin,end;};
    ConstantPatchBatch(){ranges.reserve(32);}
    void Reset(){ranges.clear();}
    void Overwrite(unsigned offset,unsigned size){
        const unsigned end=offset+size;
        for(size_t i=0;i<ranges.size();){
            auto& r=ranges[i];
            if(r.end<=offset||r.begin>=end){++i;continue;}
            if(r.begin<offset&&r.end>end){
                const unsigned tail=r.end;r.end=offset;
                ranges.insert(ranges.begin()+i+1,{end,tail});break;
            }
            if(r.begin<offset){r.end=offset;++i;}
            else if(r.end>end){r.begin=end;++i;}
            else ranges.erase(ranges.begin()+i);
        }
    }
    void Write(unsigned offset,unsigned size,const void* value){
        assert(offset<=bytes.size()&&size<=bytes.size()-offset);
        if(!size)return;
        std::memcpy(bytes.data()+offset,value,size);
        Range merged{offset,offset+size};
        auto first=std::lower_bound(ranges.begin(),ranges.end(),offset,
            [](const Range& r,unsigned begin){return r.end<begin;});
        auto last=first;
        while(last!=ranges.end()&&last->begin<=merged.end){
            merged.begin=(std::min)(merged.begin,last->begin);
            merged.end=(std::max)(merged.end,last->end);++last;
        }
        first=ranges.erase(first,last);ranges.insert(first,merged);
    }
    // GPU copies execute after the batched CPU upload. Exclude later CPU writes
    // from those copies so the same last-writer rule holds in the shared path.
    template<class Copy> void ForEachUnwritten(unsigned offset,unsigned size,Copy copy) const {
        const unsigned end=offset+size;unsigned cursor=offset;
        for(const auto& r:ranges){
            if(r.end<=cursor)continue;
            if(r.begin>=end)break;
            if(r.begin>cursor)copy(cursor,r.begin-cursor);
            cursor=(std::max)(cursor,r.end);
            if(cursor>=end)return;
        }
        if(cursor<end)copy(cursor,end-cursor);
    }
    template<class Upload> void Flush(Upload upload) const {
        for(const auto& r:ranges)upload(r.begin,r.end-r.begin,bytes.data()+r.begin);
    }
private:
    std::array<uint8_t,65536> bytes; // Only written spans are read; no per-draw clear.
    std::vector<Range> ranges;
};
}
