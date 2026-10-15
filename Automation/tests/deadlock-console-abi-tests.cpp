#include "../../AfxHookSource2/Deadlock/DeadlockConsoleAbi.h"
#include <cstdio>
#include <cstring>

namespace {
int host;
bool called = false;
void* __fastcall Registrar(void* self, std::uint64_t* result,
	const Deadlock::ConsoleCommandCreation* creation, std::uint64_t flags) {
	if (self != &host || !result || !creation || result == (void*)creation || flags != 0
		|| std::strcmp(creation->name, "mirv_test") || creation->callback != &host
		|| creation->callbackFlags != 1 || creation->completion != nullptr)
		return nullptr;
	called = true;
	*result = 0x12340000002aULL;
	return result;
}
}

int main() {
	Deadlock::ConsoleCommandCreation creation;
	creation.name = "mirv_test";
	creation.callback = &host;
	const auto handle = Deadlock::ConsoleRegister(&Registrar, &host, creation);
	if (!called || handle != 0x12340000002aULL
		|| Deadlock::ConsoleRegister(nullptr, &host, creation) != 0xffff
		|| Deadlock::ConsoleRegister(&Registrar, nullptr, creation) != 0xffff) {
		std::puts("FAIL: Deadlock registration arguments/return buffer"); return 1;
	}
	std::puts("PASS: Deadlock native registration arguments and return buffer");
	return 0;
}
