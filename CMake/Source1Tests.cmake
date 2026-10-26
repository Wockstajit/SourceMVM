# Native tests for the Source 1 hook's SOURCE:MVM code (AfxHookSource/Filmmaker and the Shared/
# pieces it brings in). Included from AfxHookSource/CMakeLists.txt, so paths are relative to it.
#
#   ctest --test-dir build/x64-release -C Release --output-on-failure -R NetconServerTests

add_executable(NetconServerTests
    ../Automation/tests/netcon-server-tests.cpp
    ../Shared/NetconServer.cpp
    ../Shared/NetconServer.h
    ../Shared/AfxConsole.cpp
)
target_compile_features(NetconServerTests PRIVATE cxx_std_17)
# Shared/*.cpp opens with #include "stdafx.h"; this hook's is the minimal one.
target_include_directories(NetconServerTests PRIVATE ./)
add_test(NAME NetconServerTests COMMAND NetconServerTests)
