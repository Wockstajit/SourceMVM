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

add_executable(SourceNetvarsTests
    ../Automation/tests/source-netvars-tests.cpp
    Filmmaker/SourceNetvars.h
    Filmmaker/SourceSdk2013.h
)
target_compile_features(SourceNetvarsTests PRIVATE cxx_std_17)
add_test(NAME SourceNetvarsTests COMMAND SourceNetvarsTests)

add_executable(SourceWorkspaceLayoutMathTests
    ../Automation/tests/source-workspace-layout-math-tests.cpp
    Filmmaker/Ui/WorkspaceLayoutMath.h
)
target_compile_features(SourceWorkspaceLayoutMathTests PRIVATE cxx_std_17)
add_test(NAME SourceWorkspaceLayoutMathTests COMMAND SourceWorkspaceLayoutMathTests)

add_executable(SourceWorkspaceMouseTests
    ../Automation/tests/source-workspace-mouse-tests.cpp
    Filmmaker/Ui/WorkspaceMouse.h
)
target_compile_features(SourceWorkspaceMouseTests PRIVATE cxx_std_17)
add_test(NAME SourceWorkspaceMouseTests COMMAND SourceWorkspaceMouseTests)

add_executable(SourceDemoFileTests
    ../Automation/tests/source-demo-file-tests.cpp
    Filmmaker/SourceDemoFile.cpp
    Filmmaker/SourceDemoFile.h
)
target_compile_features(SourceDemoFileTests PRIVATE cxx_std_17)
add_test(NAME SourceDemoFileTests COMMAND SourceDemoFileTests)
