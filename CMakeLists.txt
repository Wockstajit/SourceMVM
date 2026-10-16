cmake_minimum_required (VERSION 3.24)

project ("advancedfx")

# Register CTest tests declared in subdirectories (AfxHookSource2's
# FollowCameraMathTests); without this, `ctest -R FollowCameraMathTests`
# silently finds zero tests.
enable_testing()

# MSVC compiles one .cpp at a time per project unless /MP is set; without it the
# whole build is effectively single-threaded. AfxCppCli (C++/CLI) is unaffected:
# it builds through its own vcxproj, not CMake compile flags. Ninja already runs one
# compiler per source file in parallel, so it does not get /MP.
if(MSVC AND CMAKE_GENERATOR MATCHES "^Visual Studio")
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/MP>)
endif()

# The Visual Studio presets name the platform (-A x64 or -A Win32). Ninja presets have no
# platform, so take it from the compiler that the developer prompt selected.
if(CMAKE_GENERATOR_PLATFORM)
    set(AFX_TARGET_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(AFX_TARGET_PLATFORM "x64")
else()
    set(AFX_TARGET_PLATFORM "Win32")
endif()
message(STATUS "AFX_TARGET_PLATFORM=${AFX_TARGET_PLATFORM}")

# Release hooks always carry full debug info and link a PDB with /DEBUG. /Zi has the compiler
# write that info into one shared PDB per target, which a compiler cache such as sccache cannot
# store. The Ninja CI preset uses /Z7 instead: the same info goes into each object file, and the
# linker still writes the complete PDB from it.
set(AFX_MSVC_RELEASE_DEBUG_INFO "/Zi" CACHE STRING "Compiler debug-info flag for Release hook builds: /Zi or /Z7")
set_property(CACHE AFX_MSVC_RELEASE_DEBUG_INFO PROPERTY STRINGS /Zi /Z7)
if(NOT AFX_MSVC_RELEASE_DEBUG_INFO MATCHES "^/Z[i7]$")
    message(FATAL_ERROR "AFX_MSVC_RELEASE_DEBUG_INFO must be /Zi or /Z7, not '${AFX_MSVC_RELEASE_DEBUG_INFO}'")
endif()

# Define CMAKE_CXX_STANDARD otherwise e.g. libprotobuf will have linking problems against ABSL when compiling protobuf:
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED On)
set(CMAKE_CXX_EXTENSIONS Off)

string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_LOWER)
if(CMAKE_BUILD_TYPE_LOWER STREQUAL "debug")
	set(CMAKE_BUILD_TYPE_DEBUG 1)
endif()

set(VS_CONFIGURATION $<IF:$<CONFIG:Debug>,Debug,Release>)

#
# Rust / Corrosion related
#

if (NOT DEFINED AFX_RUST_CARGO_LOCKED)
    set(AFX_RUST_CARGO_LOCKED TRUE)
endif()

message(STATUS "AFX_RUST_CARGO_LOCKED=${AFX_RUST_CARGO_LOCKED}")

#
# Get Microsoft Visual Studio related paths:
#

execute_process(
    COMMAND "$ENV{ProgramFiles\(x86\)}\\Microsoft Visual Studio\\Installer\\vswhere.exe" "-latest" "-version" "[17.0,18.0)" "-property" "installationPath"
    OUTPUT_VARIABLE VS_INSTALLPATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REPLACE "\\" "/" VS_INSTALLPATH ${VS_INSTALLPATH})

execute_process(
    COMMAND "$ENV{ProgramFiles\(x86\)}\\Microsoft Visual Studio\\Installer\\vswhere.exe" "-latest" "-version" "[17.0,18.0)" "-requires" "Microsoft.Component.MSBuild" "-find" "MSBuild\\**\\**\\Bin\\MSBuild.exe"
    OUTPUT_VARIABLE VS_MSBUILD
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REPLACE "\\" "/" VS_MSBUILD ${VS_MSBUILD})

################################################################################

# set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}")

if(${AFX_RUST_CARGO_LOCKED})
    set(AFX_RUST_CARGO_LOCKED "LOCKED")
else()
    set(AFX_RUST_CARGO_LOCKED "")
endif()

#
# Include dependencies:
#

include(FetchContent)
include(ExternalProject)

# SOURCE:MVM default: CS2 / Source 2 only. Legacy full HLAE builds set this OFF.
if(NOT DEFINED AFX_SOURCE2_ONLY)
    set(AFX_SOURCE2_ONLY OFF)
endif()
message(STATUS "AFX_SOURCE2_ONLY=${AFX_SOURCE2_ONLY}")

# The Deadlock hook. OFF by default and gated on its own option, so a default
# `cmake --preset x64-source2-release` produces exactly the CS2 build it produced before this
# target existed -- the Deadlock work cannot regress CS2 because it is not compiled unless asked
# for. Turned on by the x64-deadlock-* presets.
if(NOT DEFINED AFX_DEADLOCK)
    set(AFX_DEADLOCK OFF)
endif()
message(STATUS "AFX_DEADLOCK=${AFX_DEADLOCK}")

if(AFX_TARGET_PLATFORM STREQUAL "x64")

#
# Setup "corrosion" for including Rust code in our projects:
#

FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG 1499b14e4906a2890f5cee1547c8848db261753d # v0.6.1
)
FetchContent_MakeAvailable(Corrosion)

#
# Import our main Rust workspace:
#

if(${CMAKE_BUILD_TYPE_DEBUG})
    set(AFX_RUST_CARGO_PROFILE_HOOK)
    set(AFX_RUST_CARGO_PROFILE_TOOL)
else()
    set(AFX_RUST_CARGO_PROFILE_HOOK PROFILE "release-hook")
    set(AFX_RUST_CARGO_PROFILE_TOOL PROFILE "release-tool")
endif()

corrosion_import_crate(
    MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
    ${AFX_RUST_CARGO_LOCKED}
    ${AFX_RUST_CARGO_PROFILE_HOOK}
    CRATES
        afx_hook_source2
)

#corrosion_import_crate(
#    MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
#    ${AFX_RUST_CARGO_LOCKED}
#    ${AFX_RUST_CARGO_PROFILE_TOOL}
#    IMPORTED_CRATES AFX_RUST_TOOL_CRATES
#    CRATES
#        file_size_calc
#)

if(${CMAKE_BUILD_TYPE_DEBUG})
    #TODO: Not quite fixed yet: https://corrosion-rs.github.io/corrosion/common_issues.html
    corrosion_set_env_vars(afx_hook_source2 "CFLAGS=-MDd" "CXXFLAGS=-MDd")
endif()

endif()

#
# Project setup
#

if(AFX_TARGET_PLATFORM STREQUAL "Win32")
    if(AFX_SOURCE2_ONLY)
        message(FATAL_ERROR "AFX_SOURCE2_ONLY requires the x64 generator platform.")
    endif()

    add_subdirectory("Deps/release/protobuf")
    add_subdirectory("Deps/release/openexr")
    add_subdirectory("Deps/release/injector_bin")

    add_subdirectory("AfxCppCli")
    add_subdirectory("ShaderBuilder")
    add_subdirectory("AfxHookGoldSrc")
    add_subdirectory("AfxHookSource")
    add_subdirectory("HLAE")

    add_custom_target(advancedfx ALL
	    DEPENDS hlae
    )
elseif(AFX_TARGET_PLATFORM STREQUAL "x64")

    add_subdirectory("Deps/release/openexr")
    add_subdirectory("ShaderBuilder")
    add_subdirectory("AfxHookSource2")
    add_subdirectory("Misc/mirv-script/src/snippets")

    if(AFX_DEADLOCK)
        add_subdirectory("AfxHookSource2/Deadlock")
    endif()

    if(AFX_SOURCE2_ONLY)
        # Injector package (x64 binaries) without the Win32 HLAE GUI stack.
        # The Electron launcher is intentionally built by its own app script.
        add_subdirectory("Deps/release/injector_bin")

        if(AFX_DEADLOCK)
            add_custom_target(advancedfx ALL
                DEPENDS AfxHookSource2 AfxHookDeadlock injector
            )
        else()
            add_custom_target(advancedfx ALL
                DEPENDS AfxHookSource2 injector
            )
        endif()
    else()
        # Legacy full x64 hook family (Source 1 + Source 2). Needs NASM for AfxHookSource.
        FetchContent_Declare(
            nasm
            URL https://github.com/advancedfx/nasm/releases/download/nasm-2.15.05/nasm-2.15.05-win64.zip https://www.nasm.us/pub/nasm/releasebuilds/2.15.05/win64/nasm-2.15.05-win64.zip
            URL_HASH SHA512=0d0469f3d475f3d192b9b5b7ee74662cbcb9e02efa0d51839c12cbf6f5de5cd58b32e1d12c9ee4381a63e99a4956e03df7decaf1369e598e0edff19a12629073
        )
        FetchContent_MakeAvailable(nasm)

        add_subdirectory("AfxHookSource")

        add_custom_target(advancedfx ALL
            DEPENDS AfxHookSource AfxHookSource2
        )
    endif()
endif()

#
# Install
#

set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
if(${CMAKE_BUILD_TYPE_DEBUG})
	set(CMAKE_INSTALL_DEBUG_LIBRARIES TRUE)
endif()
include(InstallRequiredSystemLibraries)
if(${CMAKE_BUILD_TYPE_DEBUG})
	set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} "${MSVC_CRT_DIR}/vccorlib${v}d.dll" "${MSVC_REDIST_DIR}/${CMAKE_MSVC_ARCH}/Microsoft.${MSVC_REDIST_NAME}.CRT/vccorlib${v}.dll")
else()
	set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} "${MSVC_CRT_DIR}/vccorlib${v}.dll" )
endif()

if(AFX_TARGET_PLATFORM STREQUAL "Win32")
    install(
        PROGRAMS
            ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}

            "${afxcppcli_BINARY_DIR}/AfxCppCli.dll"
            "$<TARGET_FILE:AfxHookGoldSrc>"
            "$<TARGET_FILE:AfxHookSource>"
            "${hlae_BINARY_DIR}/HLAE.exe"
            "${hlae_BINARY_DIR}/NGettext.dll"
            "${injector_BINARY_DIR}/injector.exe"

            ${AFX_OPENEXR_INSTALL_BINARIES}

        DESTINATION "bin"
    )
    install(
        FILES
            "${injector_BINARY_DIR}/AfxHook.dat"
        DESTINATION "bin"
    )

    install(
        FILES
            "${afxcppcli_BINARY_DIR}/AfxCppCli.pdb"
            "$<TARGET_PDB_FILE:AfxHookGoldSrc>"
            "$<TARGET_PDB_FILE:AfxHookSource>"
            "${hlae_BINARY_DIR}/HLAE.pdb"
            "${injector_BINARY_DIR}/injector.pdb"
        DESTINATION "pdb"
    )

    install(
        PROGRAMS
            "${injector_BINARY_DIR}/x64/injector.exe"
        DESTINATION "bin/x64"
    )
    install(
        FILES
            "${injector_BINARY_DIR}/x64/AfxHook.dat"
        DESTINATION "bin/x64"
    )
    install(
        FILES
            "${injector_BINARY_DIR}/x64/injector.pdb"
        DESTINATION "pdb/x64"
    )
elseif(AFX_TARGET_PLATFORM STREQUAL "x64")
    install(
        PROGRAMS
            ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
            ${AFX_OPENEXR_INSTALL_BINARIES}
        DESTINATION "bin/x64"
        COMPONENT x64-runtime
    )

    if(NOT AFX_SOURCE2_ONLY)
        install(
            PROGRAMS
                "$<TARGET_FILE:AfxHookSource>"
            DESTINATION "bin/x64"
            COMPONENT x64-source1
        )

        install(
            FILES
                "$<TARGET_PDB_FILE:AfxHookSource>"
            DESTINATION "pdb/x64"
            COMPONENT x64-source1
        )
    endif()

    install(
        PROGRAMS
            "$<TARGET_FILE:AfxHookSource2>"
        DESTINATION "bin/x64"
        COMPONENT x64-source2
    )

    install(
        FILES
            "$<TARGET_PDB_FILE:AfxHookSource2>"
        DESTINATION "pdb/x64"
        COMPONENT x64-source2
    )

    install(
		DIRECTORY
            "${AfxHookSource2Snippets_BINARY_DIR}/dist/"
        DESTINATION "bin/resources/AfxHookSource2/snippets"
        COMPONENT x64-source2
    )

    if(AFX_SOURCE2_ONLY)
        install(
            PROGRAMS
                "${injector_BINARY_DIR}/x64/injector.exe"
            DESTINATION "bin/x64"
            COMPONENT x64-runtime
        )
        install(
            FILES
                "${injector_BINARY_DIR}/x64/AfxHook.dat"
            DESTINATION "bin/x64"
            COMPONENT x64-runtime
        )
        install(
            FILES
                "${injector_BINARY_DIR}/x64/injector.pdb"
            DESTINATION "pdb/x64"
            COMPONENT x64-runtime
        )
    endif()

#    corrosion_install(
#        TARGETS
#            ${AFX_RUST_TOOL_CRATES}
#        RUNTIME
#            DESTINATION "bin/x64"
#    )
endif()
