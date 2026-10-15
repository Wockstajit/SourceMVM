#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentConstantPatchMath.h"
#include <iostream>
#include <random>
#include <cstdlib>
using Filmmaker::Environment::ConstantPatchBatch;
void Check(bool value,const char* why){if(!value){std::cerr<<why<<'\n';std::exit(1);}}
int main(){
    ConstantPatchBatch batch;
    std::array<uint8_t,1024> sequential{},batched{},value{};
    std::mt19937 random(4517);
    for(unsigned trial=0;trial<200;++trial){
        for(auto& b:sequential)b=uint8_t(random());batched=sequential;batch.Reset();
        for(unsigned operation=0;operation<80;++operation){
            const unsigned offset=random()%900,size=1+random()%124;
            for(auto& b:value)b=uint8_t(random());
            std::memcpy(sequential.data()+offset,value.data(),size);
            if(random()%3==0){ // GPU copies execute immediately, ahead of queued CPU writes.
                batch.Overwrite(offset,size);std::memcpy(batched.data()+offset,value.data(),size);
            }else batch.Write(offset,size,value.data());
        }
        unsigned lastEnd=0;
        batch.Flush([&](unsigned offset,unsigned size,const void* data){
            Check(offset>=lastEnd,"upload ranges are sorted and disjoint");lastEnd=offset+size;
            std::memcpy(batched.data()+offset,data,size);
        });
        Check(batched==sequential,"batched CPU/GPU overlaps preserve sequential bytes and native gaps");
    }
    batch.Reset();unsigned row[4]{1,2,3,4};
    batch.Write(16,16,row);batch.Write(32,16,row);batch.Write(16,16,row);
    unsigned uploads=0;batch.Flush([&](unsigned offset,unsigned size,const void*){
        ++uploads;Check(offset==16&&size==32,"adjacent and repeated rows share one upload");
    });Check(uploads==1,"coalesce repeated patches");
    batch.Overwrite(24,8);uploads=0;
    batch.Flush([&](unsigned,unsigned,const void*){++uploads;});Check(uploads==2,"GPU hole splits a CPU range");
    std::cout<<"Sparse constant patches preserve CPU/GPU write ordering\n";
}
