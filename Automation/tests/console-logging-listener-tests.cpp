#include "../../AfxHookSource2/ConsoleLoggingListener.h"
#include <cstring>
#include <cstdlib>
#include <thread>
struct Listener : Filmmaker::Cs2LoggingListener {
    const char* last=nullptr;
    void Log(const void*, const char* text) override {last=text;}
};
int main(){
    {
        Filmmaker::Cs2ConsoleLogScope outer;
        if(!outer)return 2;
        {Filmmaker::Cs2ConsoleLogScope recursive;if(recursive)return 3;}
        Filmmaker::Cs2ConsoleLogScope stillRecursive;if(stillRecursive)return 4;
        bool independent=false;std::thread other([&]{Filmmaker::Cs2ConsoleLogScope scope;independent=bool(scope);});other.join();
        if(!independent)return 5;
    }
    try{Filmmaker::Cs2ConsoleLogScope scope;if(!scope)return 6;throw 1;}catch(int){}
    Filmmaker::Cs2ConsoleLogScope afterUnwind;if(!afterUnwind)return 7;
    Listener listener;
    auto table=*reinterpret_cast<void***>(&listener);
    using Log=void(*)(void*,const void*,const char*);
    using Notify=void(*)(void*,int);
    reinterpret_cast<Log>(table[0])(&listener,nullptr,"before");
    if(!listener.last||std::strcmp(listener.last,"before"))return 1;
    // Reproduce tier0's ABI dispatch, including the startup-crashing slot 3.
    for(int slot=1;slot<=4;++slot)reinterpret_cast<Notify>(table[slot])(&listener,88);
    reinterpret_cast<Log>(table[0])(&listener,nullptr,"after");
    return !listener.last||std::strcmp(listener.last,"after");
}
