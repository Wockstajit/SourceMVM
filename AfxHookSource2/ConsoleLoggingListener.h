#pragma once
namespace Filmmaker {
// Engine metadata queries can synchronously emit another console message.
// Drop that nested record before it attempts to lock the same logging mutex.
class Cs2ConsoleLogScope {
    static bool& Entered(){static thread_local bool entered=false;return entered;}
    bool entered_;
public:
    Cs2ConsoleLogScope():entered_(!Entered()){if(entered_)Entered()=true;}
    ~Cs2ConsoleLogScope(){if(entered_)Entered()=false;}
    Cs2ConsoleLogScope(const Cs2ConsoleLogScope&)=delete;
    Cs2ConsoleLogScope& operator=(const Cs2ConsoleLogScope&)=delete;
    explicit operator bool() const{return entered_;}
};
// CS2 tier0 dispatches verbosity and flags notifications through slots 3 and 4.
// A Log-only listener lets those calls run off its vtable during map startup.
// Keep the two intervening notification slots, and do not add a virtual destructor:
// Log must remain slot zero. Notification payloads are ignored by console capture.
class Cs2LoggingListener {
public:
    virtual void Log(const void* context, const char* message) = 0;
    virtual void ReservedNotification1(int) {}
    virtual void ReservedNotification2(int) {}
    virtual void OnChannelVerbosityChanged(int) {}
    virtual void OnChannelFlagsChanged(int) {}
};
}
