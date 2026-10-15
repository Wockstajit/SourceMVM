cmake_minimum_required(VERSION 3.24)

# SOURCE:MVM Source 2 / CS2-only staged build coordinator.
# Separate from CMake/MultiBuild.cmake (legacy full HLAE package).

get_filename_component(AFX_SCRIPT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/AfxBuildScript.cmake")

if(NOT DEFINED AFX_SOURCE2_CONFIG)
    set(AFX_SOURCE2_CONFIG Release)
endif()
string(TOLOWER "${AFX_SOURCE2_CONFIG}" AFX_SOURCE2_CONFIG_LOWER)

if(NOT DEFINED AFX_SOURCE2_CONFIGURE)
    set(AFX_SOURCE2_CONFIGURE AUTO)
endif()

if(NOT DEFINED AFX_SOURCE2_PREFIX)
    set(AFX_SOURCE2_PREFIX "${AFX_SCRIPT_SOURCE_DIR}/build/source2-${AFX_SOURCE2_CONFIG_LOWER}")
endif()
get_filename_component(AFX_SOURCE2_PREFIX "${AFX_SOURCE2_PREFIX}" ABSOLUTE BASE_DIR "${AFX_SCRIPT_SOURCE_DIR}")

set(AFX_SOURCE2_PRESET "x64-source2-${AFX_SOURCE2_CONFIG_LOWER}")
set(AFX_SOURCE2_BUILD_DIR "${AFX_SCRIPT_SOURCE_DIR}/build/${AFX_SOURCE2_PRESET}")

message(STATUS "SOURCE:MVM Source2 config: ${AFX_SOURCE2_CONFIG}")
message(STATUS "SOURCE:MVM Source2 prefix: ${AFX_SOURCE2_PREFIX}")
message(STATUS "SOURCE:MVM Source2 configure mode: ${AFX_SOURCE2_CONFIGURE}")
message(STATUS "SOURCE:MVM Source2 preset: ${AFX_SOURCE2_PRESET}")

afx_configure_preset("${AFX_SOURCE2_PRESET}" "${AFX_SOURCE2_CONFIGURE}")

# Build only the CS2 stack targets (ALL target is source2-only in this preset).
afx_script_run("${CMAKE_COMMAND}" --build --preset "${AFX_SOURCE2_PRESET}" --parallel)

# Install only the Source 2 runtime + hook + injector staged by the x64 source2 tree.
# SourceMovieMakerLauncher is an independent Electron app and has its own build script.
afx_script_run("${CMAKE_COMMAND}" --install "${AFX_SOURCE2_BUILD_DIR}" --config "${AFX_SOURCE2_CONFIG}" --prefix "${AFX_SOURCE2_PREFIX}" --component x64-runtime)
afx_script_run("${CMAKE_COMMAND}" --install "${AFX_SOURCE2_BUILD_DIR}" --config "${AFX_SOURCE2_CONFIG}" --prefix "${AFX_SOURCE2_PREFIX}" --component x64-source2)

# Lean CS2 resource stage: shaders already installed by AfxHookSource2; copy shared assets.
afx_script_run("${CMAKE_COMMAND}" -E make_directory "${AFX_SOURCE2_PREFIX}/bin/resources")
afx_script_run("${CMAKE_COMMAND}" -E copy_if_different
    "${AFX_SCRIPT_SOURCE_DIR}/Resources/resources/hexfont.tga"
    "${AFX_SOURCE2_PREFIX}/bin/resources/hexfont.tga")
if(EXISTS "${AFX_SCRIPT_SOURCE_DIR}/Resources/AfxHookSource2_changelog.xml")
    afx_script_run("${CMAKE_COMMAND}" -E copy_if_different
        "${AFX_SCRIPT_SOURCE_DIR}/Resources/AfxHookSource2_changelog.xml"
        "${AFX_SOURCE2_PREFIX}/bin/AfxHookSource2_changelog.xml")
endif()
if(EXISTS "${AFX_SCRIPT_SOURCE_DIR}/Resources/ffmpeg")
    afx_script_run("${CMAKE_COMMAND}" -E copy_directory
        "${AFX_SCRIPT_SOURCE_DIR}/Resources/ffmpeg"
        "${AFX_SOURCE2_PREFIX}/bin/ffmpeg")
endif()

message(STATUS "SOURCE:MVM Source2 runtime ready: ${AFX_SOURCE2_PREFIX}")
message(STATUS "Build the standalone launcher separately with SourceMovieMakerLauncher/build-app.bat")
