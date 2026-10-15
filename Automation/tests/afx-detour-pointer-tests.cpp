#include "../../Shared/AfxDetours.h"
#include <cstdio>

namespace {
int Original() { return 7; }
int Replacement() { return 11; }
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
}

int main() {
    auto slot = static_cast<PVOID*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!slot) return 2;
    *slot = reinterpret_cast<PVOID>(Original);
    DWORD previous = 0;
    if (!VirtualProtect(slot, 4096, PAGE_READONLY, &previous)) return 2;

    PVOID original = nullptr;
    Check(AfxDetourPtr(slot, reinterpret_cast<PVOID>(Replacement), &original) != FALSE,
          "successful hook must report success");
    Check(original == reinterpret_cast<PVOID>(Original), "original function is retained");
    Check(reinterpret_cast<int(*)()>(*slot)() == 11, "replacement is callable through the slot");
    Check(reinterpret_cast<int(*)()>(original)() == 7, "original remains callable");
    MEMORY_BASIC_INFORMATION info{};
    Check(VirtualQuery(slot, &info, sizeof(info)) != 0 && info.Protect == PAGE_READONLY,
          "hook must restore read-only page protection");

    Check(AfxDetourPtr(slot, original, nullptr) != FALSE, "restoring without an output pointer succeeds");
    Check(reinterpret_cast<int(*)()>(*slot)() == 7, "original function is restored");
    Check(VirtualQuery(slot, &info, sizeof(info)) != 0 && info.Protect == PAGE_READONLY,
          "restore also preserves page protection");
    Check(AfxDetourPtr(nullptr, original, &original) == FALSE, "invalid target fails");
    Check(original == reinterpret_cast<PVOID>(Original), "failed hook leaves output unchanged");
    VirtualFree(slot, 0, MEM_RELEASE);
    return failures ? 1 : 0;
}
