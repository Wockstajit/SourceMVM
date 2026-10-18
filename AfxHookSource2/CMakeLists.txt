cmake_minimum_required (VERSION 3.24)

project ("AfxHookSource2")

################################################################################

set(AFXHOOKSOURCE2_SHADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_line_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_line_vs_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_line_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_line_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_drawtexture_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_drawtexture_vs_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_drawtexture_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_drawtexture_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_blit_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_blit_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_blit_triangle_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_blit_triangle_vs_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_fisheye_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_fisheye_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_contact_shadow_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_contact_shadow_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depthms_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depthms_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_render_pass_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_render_pass_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depth_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depth_vs_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depth_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depth_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_vs_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_ps_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_tex_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_tex_vs_5_0.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_tex_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_tex_ps_5_0.h"
)

set(AFXHOOKSOURCE2_SHADERS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_line_vs_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_line_ps_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_drawtexture_vs_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_drawtexture_ps_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_blit_ps_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_blit_triangle_vs_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_fisheye_ps_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_contact_shadow_ps_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_depthms_ps_5_0.fxc"
	"${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_render_pass_ps_5_0.fxc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_depth_vs_5_0.fxc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_depth_ps_5_0.fxc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_deadlock_overlay_vs_5_0.fxc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_deadlock_overlay_ps_5_0.fxc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_deadlock_overlay_tex_vs_5_0.fxc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_deadlock_overlay_tex_ps_5_0.fxc"
)

add_custom_command(
    WORKING_DIRECTORY ${shaderbuilder_BINARY_DIR}
    OUTPUT ${AFXHOOKSOURCE2_SHADERS}
    DEPENDS shaderbuilder ${AFXHOOKSOURCE2_SHADERS_SOURCES}
    COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile vs_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_line_vs_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_line_vs_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_line_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_line_ps_5_0"
    COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile vs_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_drawtexture_vs_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_drawtexture_vs_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_drawtexture_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_drawtexture_ps_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_blit_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_blit_ps_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile vs_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_blit_triangle_vs_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_blit_triangle_vs_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_fisheye_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_fisheye_ps_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_contact_shadow_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_contact_shadow_ps_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_depthms_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depthms_ps_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_render_pass_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_render_pass_ps_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile vs_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_depth_vs_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depth_vs_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_depth_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depth_ps_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile vs_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_deadlock_overlay_vs_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_vs_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_deadlock_overlay_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_ps_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile vs_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_deadlock_overlay_tex_vs_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_tex_vs_5_0"
	COMMAND "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile ps_5_0 --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/afx_deadlock_overlay_tex_ps_5_0.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_deadlock_overlay_tex_ps_5_0"
)

add_custom_target(AfxHookSource2Shaders
    DEPENDS ${AFXHOOKSOURCE2_SHADERS}
)

# AfxShaders.cpp loads the compiled programs at runtime from
# <HLAE>/resources/shaders.  Keep the install tree in lockstep with the DLL;
# otherwise a hook-only build can inject new C++ while silently executing stale
# shader bytecode left behind by an older full install.
set(AFXHOOKSOURCE2_SHADER_BINARIES
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_line_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_line_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_drawtexture_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_drawtexture_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_blit_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_blit_triangle_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_fisheye_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_contact_shadow_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depthms_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_render_pass_ps_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depth_vs_5_0.acs"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_depth_ps_5_0.acs"
)

install(
    FILES ${AFXHOOKSOURCE2_SHADER_BINARIES}
    DESTINATION "bin/resources/shaders"
    COMPONENT x64-source2
)
# Death Animations clip library (FX/tools/extract-death-anims.py): one file holding every source.
# Its donor animations are third-party, so a checkout may not carry it: the hook then reports
# "ragdoll animation file missing" and Death Animations has no clips.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../FX/death-animations/ragdoll_animations.mvmanim")
    install(FILES ../FX/death-animations/ragdoll_animations.mvmanim
        DESTINATION "bin/resources/ragdoll" COMPONENT x64-source2)
else()
    message(STATUS "FX/death-animations/ragdoll_animations.mvmanim not present: Death Animations ships without clips.")
endif()
install(FILES ../Automation/lib/Resolve-Cs2Dir.ps1 ../Automation/lib/AutomationCommon.ps1
    DESTINATION "bin/automation/lib" COMPONENT x64-source2)

# Scoreboard / event parse helper. Hook-only cmake --install used to leave
# bin/x64/FilmmakerDemoInfo/ empty, so every demo showed "No scoreboard data".
find_program(GO_EXECUTABLE go)
if(GO_EXECUTABLE)
	set(FILMMAKER_DEMOINFO_EXE "${CMAKE_CURRENT_BINARY_DIR}/FilmmakerDemoInfo/FilmmakerDemoInfo.exe")
	add_custom_command(
		OUTPUT "${FILMMAKER_DEMOINFO_EXE}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/FilmmakerDemoInfo"
		COMMAND "${GO_EXECUTABLE}" build -o "${FILMMAKER_DEMOINFO_EXE}" .
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../FilmmakerDemoInfoGo"
		DEPENDS
			"${CMAKE_CURRENT_SOURCE_DIR}/../FilmmakerDemoInfoGo/main.go"
			"${CMAKE_CURRENT_SOURCE_DIR}/../FilmmakerDemoInfoGo/go.mod"
		COMMENT "Building FilmmakerDemoInfo helper"
		VERBATIM)
	add_custom_target(FilmmakerDemoInfoHelper ALL DEPENDS "${FILMMAKER_DEMOINFO_EXE}")
	install(FILES "${FILMMAKER_DEMOINFO_EXE}" DESTINATION "bin/x64/FilmmakerDemoInfo" COMPONENT x64-source2)

	# Keyframed demo copies for one-frame backward seeks (Filmmaker/Demo/DemoKeyframes.h).
	set(FILMMAKER_KEYFRAMES_EXE "${CMAKE_CURRENT_BINARY_DIR}/FilmmakerDemoKeyframes/FilmmakerDemoKeyframes.exe")
	file(GLOB FILMMAKER_KEYFRAMES_SOURCES CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/../FilmmakerDemoKeyframesGo/*.go"
		"${CMAKE_CURRENT_SOURCE_DIR}/../FilmmakerDemoKeyframesGo/st2/*.go")
	add_custom_command(
		OUTPUT "${FILMMAKER_KEYFRAMES_EXE}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/FilmmakerDemoKeyframes"
		COMMAND "${GO_EXECUTABLE}" build -o "${FILMMAKER_KEYFRAMES_EXE}" .
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../FilmmakerDemoKeyframesGo"
		DEPENDS ${FILMMAKER_KEYFRAMES_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../FilmmakerDemoKeyframesGo/go.mod"
		COMMENT "Building FilmmakerDemoKeyframes helper"
		VERBATIM)
	add_custom_target(FilmmakerDemoKeyframesHelper ALL DEPENDS "${FILMMAKER_KEYFRAMES_EXE}")
	install(FILES "${FILMMAKER_KEYFRAMES_EXE}" DESTINATION "bin/x64/FilmmakerDemoKeyframes" COMPONENT x64-source2)
endif()


################################################################################

add_library(${PROJECT_NAME} MODULE)

# Ship the exact runtime inputs as validated bytecode, avoiding a cold compile
# on the first frame. Runtime compilation remains the fallback for stale bundles.
add_executable(EnvironmentShaderBuilder ../Automation/tools/prepare-environment-shaders.cpp)
target_compile_features(EnvironmentShaderBuilder PRIVATE cxx_std_17)
set(ENVIRONMENT_SHADER_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/environment-shaders")
add_custom_command(OUTPUT "${ENVIRONMENT_SHADER_BUNDLE}/ready.stamp"
    COMMAND $<TARGET_FILE:EnvironmentShaderBuilder> "${ENVIRONMENT_SHADER_BUNDLE}"
    DEPENDS EnvironmentShaderBuilder
    COMMENT "Preparing environment shader bundle"
    VERBATIM)
add_custom_target(EnvironmentShaderBundle DEPENDS "${ENVIRONMENT_SHADER_BUNDLE}/ready.stamp")
add_dependencies(${PROJECT_NAME} EnvironmentShaderBundle)
install(DIRECTORY "${ENVIRONMENT_SHADER_BUNDLE}/"
    DESTINATION "bin/x64/environment-shaders" COMPONENT x64-source2
    FILES_MATCHING PATTERN "*.cso")

add_dependencies(${PROJECT_NAME}
    OpenEXR
    AfxHookSource2Shaders
    AfxHookSource2Snippets
)

SET(AFXHOOKSOURCE2SNIPPETS_OUTPUTS ${AFXHOOKSOURCE2SNIPPETS_OUTPUTS} PARENT_SCOPE)

add_definitions(-DGAME_CS2)

#set_target_properties(
#    ${PROJECT_NAME}
#    PROPERTIES 
#        OUTPUT_NAME "${PROJECT_NAME}_IF_YOU_USE_THIS_TO_JOIN_GOTV_OR_VAC_SERVERS_YOU_WILL_GET_VAC_BANNED"
#)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# - generate PDB in Release too
# - enable Hot Reload in Debug
target_compile_options(${PROJECT_NAME} PRIVATE
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:${AFX_MSVC_RELEASE_DEBUG_INFO}>
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/ZI>
	)
target_link_options(${PROJECT_NAME} PRIVATE
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/DEBUG /OPT:REF /OPT:ICF>
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/INCREMENTAL>
	)

target_include_directories(${PROJECT_NAME} PRIVATE
    ./
    ${AFX_OPENEXR_INCLUDE_DIRECTORIES}
)

target_link_directories(${PROJECT_NAME} PRIVATE
    ${AFX_OPENEXR_LINK_DIRECTORIES}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${AFX_OPENEXR_LINK_LIBRARIES}
    afx_hook_source2
	dxguid.lib
	dxgi.lib
    windowscodecs.lib
	ole32.lib # filmmaker folder picker (IFileOpenDialog / CoCreateInstance)
	shell32.lib # "show demo in Explorer" (SHOpenFolderAndSelectItems / ILCreateFromPathW)
	oleaut32.lib # ...and the VARIANT/BSTR half of re-pointing an open Explorer window
    bcrypt.lib # corrosion fails to detect this
    $<$<CONFIG:Debug>:msvcrtd.lib> # corrision messes this up in latest rust version
)

target_sources(${PROJECT_NAME} PRIVATE
    ../Deps/release/Detours/src/detours.cpp
    ../Deps/release/Detours/src/detours.h
    ../Deps/release/Detours/src/detver.h
    ../Deps/release/Detours/src/disasm.cpp
    ../Deps/release/Detours/src/disolarm.cpp
    ../Deps/release/Detours/src/disolarm64.cpp
    ../Deps/release/Detours/src/disolia64.cpp
    ../Deps/release/Detours/src/disolx64.cpp
    ../Deps/release/Detours/src/disolx86.cpp
    ../Deps/release/Detours/src/image.cpp
    ../Deps/release/Detours/src/modules.cpp

    ../Deps/release/prop/cs2/sdk_src/public/appframework/IAppSystem.h
    ../Deps/release/prop/cs2/sdk_src/public/interfaces/interfaces.h
    ../Deps/release/prop/cs2/sdk_src/public/tier0/memalloc.h
    ../Deps/release/prop/cs2/sdk_src/public/tier0/platform.cpp
    ../Deps/release/prop/cs2/sdk_src/public/tier0/platform.h
    ../Deps/release/prop/cs2/sdk_src/public/tier1/convar.cpp
    ../Deps/release/prop/cs2/sdk_src/public/tier1/convar.h
    ../Deps/release/prop/cs2/sdk_src/public/tier1/iconvar.h
    ../Deps/release/prop/cs2/sdk_src/public/tier1/KeyValues.h
    ../Deps/release/prop/cs2/sdk_src/public/tier1/strtools.h
    ../Deps/release/prop/cs2/sdk_src/public/tier1/utlmap.h
    ../Deps/release/prop/cs2/sdk_src/public/tier1/utlmemory.cpp
    ../Deps/release/prop/cs2/sdk_src/public/tier1/utlmemory.h
    ../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.cpp
    ../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h
    ../Deps/release/prop/cs2/sdk_src/public/tier1/utlvector.h
    ../Deps/release/prop/cs2/sdk_src/public/tier1/bufferstring.cpp
    ../Deps/release/prop/cs2/sdk_src/public/tier1/bufferstring.h
    ../Deps/release/prop/cs2/sdk_src/public/vstdlib/IKeyValuesSystem.h
    ../Deps/release/prop/cs2/sdk_src/public/Color.h
    ../Deps/release/prop/cs2/sdk_src/public/cdll_int.h
    ../Deps/release/prop/cs2/sdk_src/public/filesystem.h
    ../Deps/release/prop/cs2/sdk_src/public/const.h
    ../Deps/release/prop/cs2/sdk_src/public/entityhandle.h
    ../Deps/release/prop/cs2/sdk_src/public/icvar.h
    ../Deps/release/prop/cs2/sdk_src/public/igameevents.h
    ../Deps/release/prop/cs2/sdk_src/public/igameuiservice.h
    ../Deps/release/prop/cs2/sdk_src/tier1/KeyValues.cpp
    ../Deps/release/prop/AfxHookSource/SourceSdkShared.cpp
    ../Deps/release/prop/AfxHookSource/SourceSdkShared.h
    ../Deps/release/prop/AfxHookSource/SourceInterfaces.h
    
    ../Shared/vcpp/AfxAddr.cpp
    ../Shared/vcpp/AfxAddr.h

    ../Shared/AfxCommandLine.cpp
    ../Shared/AfxCommandLine.h
    ../Shared/AfxConsole.cpp
    ../Shared/AfxConsole.h
    ../Shared/AfxDetours.cpp
    ../Shared/AfxDetours.h
    ../Shared/AfxMath.cpp
    ../Shared/AfxMath.h
    ../Shared/AfxOutStreams.cpp
    ../Shared/AfxOutStreams.h    
    ../Shared/CamIO.cpp
    ../Shared/CamIO.h
    ../Shared/CamPath.cpp
    ../Shared/CamPath.h
    ../Shared/CamPathRs.cpp
    ../Shared/CommandSystem.cpp
    ../Shared/CommandSystem.h
    ../Shared/ConsolePrinter.h
    ../Shared/EasySampler.cpp
    ../Shared/EasySampler.h
    ../Shared/FFITools.h
    ../Shared/FileTools.cpp
    ../Shared/FileTools.h
    ../Shared/FovScaling.cpp
    ../Shared/FovScaling.h
    ../Shared/GrowingBufferPoolThreadSafe.h
    ../Shared/ImageTransformer.cpp
    ../Shared/ImageTransformer.h    
    ../Shared/OutVideoStreamCreators.h
    ../Shared/OpenExrOutput.cpp
    ../Shared/OpenExrOutput.h    
    ../Shared/RawOutput.cpp
    ../Shared/RawOutput.h    
    ../Shared/RecordingSettings.cpp
    ../Shared/RecordingSettings.h    
    ../Shared/StringTools.cpp
    ../Shared/StringTools.h
    ../Shared/ThreadPool.h
    ../Shared/binutils.cpp
    ../Shared/binutils.h
    ../Shared/MirvCamIO.cpp
    ../Shared/MirvCamIO.h
    ../Shared/MirvCampath.cpp
    ../Shared/MirvCampath.h
	../Shared/MirvDeathMsgFilter.cpp
	../Shared/MirvDeathMsgFilter.h
    ../Shared/MirvInput.cpp
    ../Shared/MirvInput.h
    ../Shared/MirvSkip.cpp
    ../Shared/MirvSkip.h
    ../Shared/RefCounted.h
    ../Shared/RefCountedThreadSafe.h
    ../Shared/TRefCounted.h

    addresses.cpp
    addresses.h

    AfxHookSource2Rs.cpp
    AfxHookSource2Rs.h

    AfxShaders.cpp
    AfxShaders.h

	CamIO.cpp
	CamIO.h

    CampathDrawer.cpp
    CampathDrawer.h

    ClientEntitySystem.cpp
    ClientEntitySystem.h

    ConsoleRs.cpp
    CVarRs.cpp

	DeathMsg.cpp
	DeathMsg.h

    GameEvents.cpp
    GameEvents.h

	Globals.cpp
	Globals.h

    hlaeFolder.cpp
    hlaeFolder.h
	
	ViewModel.cpp
	ViewModel.h

    main.cpp

	MirvColors.cpp
	MirvColors.h

	MirvCommands.cpp
	MirvCommands.h

	MirvFix.cpp
	MirvFix.h

	MirvTime.cpp
	MirvTime.h

    ReplaceName.cpp
    ReplaceName.h

    RenderCommands.cpp
    RenderCommands.h

    RenderServiceHooks.cpp
    RenderServiceHooks.h

    RenderSystemDX11Hooks.cpp
    RenderSystemDX11Hooks.h

    ViewportScaler.cpp
    ViewportScaler.h
    FisheyePass.cpp
    FisheyePass.h
    ContactShadowPass.cpp
    RenderPassCompositor.cpp
    ContactShadowPass.h
    Filmmaker/Environment/EnvironmentMath.h
    Filmmaker/Environment/EnvironmentScene.h
    Filmmaker/Props/ClientEntitySpawn.cpp
    Filmmaker/Props/Props.cpp
    Filmmaker/Props/PropPreview.cpp
    Filmmaker/Environment/EnvironmentNativeWater.cpp
    Filmmaker/Environment/EnvironmentNativeRain.cpp
    Filmmaker/Environment/EnvironmentNativeRain.h
    Filmmaker/Relight/RelightScreenGi.cpp Filmmaker/Relight/RelightProbes.cpp Filmmaker/Relight/RelightProbeGi.cpp Filmmaker/Relight/RelightReflectionFilter.cpp Filmmaker/Relight/RelightStainedGlass.cpp Filmmaker/Relight/RelightLightEditor.cpp Filmmaker/Relight/RelightLightmap.cpp
    Filmmaker/Relight/Relight.cpp Filmmaker/Relight/RelightNativeCluster.cpp
    Filmmaker/Relight/Relight.h
    Filmmaker/Relight/RelightMath.h
    Filmmaker/Environment/EnvironmentScene.cpp
    Filmmaker/Environment/EnvironmentVfxLighting.cpp
    Filmmaker/Environment/EnvironmentWorld.cpp
    Filmmaker/Environment/EnvironmentBakedLighting.cpp
    Filmmaker/Environment/EnvironmentBakedLighting.h
    Filmmaker/Environment/EnvironmentClimateMath.h
    Filmmaker/Environment/EnvironmentClimateShaders.h
    Filmmaker/Environment/EnvironmentShaderCompiler.h
    Filmmaker/Environment/EnvironmentWeatherMath.h
    Filmmaker/Environment/EnvironmentSurfaceMath.h
    Filmmaker/Environment/EnvironmentMaterialMath.h
    Filmmaker/Environment/EnvironmentMaterialImage.cpp
    Filmmaker/Environment/EnvironmentMaterialShaders.h
    Filmmaker/Environment/EnvironmentStudioMath.h
    Filmmaker/Environment/EnvironmentStudioShaders.h
    Filmmaker/Environment/EnvironmentCoatingMath.h
    Filmmaker/Environment/EnvironmentCoatingShaders.h
    Filmmaker/Environment/EnvironmentShadowShaders.h
    Filmmaker/Environment/EnvironmentShadowMath.h
    Filmmaker/Environment/EnvironmentShadowMaps.h
    Filmmaker/Environment/EnvironmentOcclusion.h
    Filmmaker/Environment/EnvironmentOcclusion.cpp
    Filmmaker/Environment/MapCollisionGrid.h
    Filmmaker/Environment/MapCollisionGrid.cpp
    Filmmaker/Panorama/ConfigHudClimateJs.h
    Filmmaker/Environment/EnvironmentWorld.h
    Filmmaker/Environment/EnvironmentDocument.cpp
    Filmmaker/Environment/EnvironmentObj.cpp
    Filmmaker/Environment/EnvironmentGltf.cpp
    Filmmaker/Environment/EnvironmentWorldDocument.cpp
    Filmmaker/Environment/EnvironmentRender.h
    Filmmaker/Environment/EnvironmentRender.cpp
    Filmmaker/Environment/EnvironmentShadowMaps.cpp
    Filmmaker/Environment/EnvironmentFoliage.cpp
    Filmmaker/Environment/EnvironmentViewmodel.cpp
    Filmmaker/Environment/EnvironmentAnimatedCasters.cpp Filmmaker/Environment/EnvironmentParticleShadows.cpp
    Filmmaker/Environment/EnvironmentGi.cpp
    Filmmaker/Environment/EnvironmentGi.h
    Filmmaker/Environment/EnvironmentGiMath.h
    Filmmaker/Environment/EnvironmentGiTraceShaders.h
    Filmmaker/Environment/EnvironmentGiBuildShaders.h
    Filmmaker/Environment/EnvironmentShaders.h
    Filmmaker/Panorama/ConfigHudEnvironmentTabJs.h
    Filmmaker/Panorama/ConfigHudEnvironmentStateJs.h
    Filmmaker/Panorama/ConfigHudEnvironmentNavJs.h

    ReShadeAdvancedfx.cpp
    ReShadeAdvancedfx.h

	SchemaSystem.cpp
	SchemaSystem.h

	SceneSystem.cpp
	SceneSystem.h

    StreamSettings.h

    WrpConsole.cpp
    WrpConsole.h

    # --- Filmmaker demo-browser feature (self-contained module) ---
    Filmmaker/Filmmaker.cpp
    Filmmaker/Filmmaker.h
    Filmmaker/FilmmakerCommand.cpp
    Filmmaker/FilmmakerCommandUtil.cpp
    Filmmaker/FilmmakerCommandUtil.h
    Filmmaker/MarkerCommand.cpp
    Filmmaker/CameraTimelineCommand.cpp
    Filmmaker/CamPathShakeCommand.cpp
    Filmmaker/CameraEditorCommand.cpp
    Filmmaker/FollowCommand.cpp
    Filmmaker/GraphEditorCommand.cpp

    Filmmaker/Config/DemoFileStore.cpp
    Filmmaker/Config/DemoFileStore.h
    Filmmaker/Config/DemoFolderStore.cpp
    Filmmaker/Config/DemoFolderStore.h
    Filmmaker/Config/DemoOrganizerStore.cpp
    Filmmaker/Config/DemoOrganizerStore.h
    Filmmaker/Config/WorkshopOrganizerStore.cpp
    Filmmaker/Config/WorkshopOrganizerStore.h
    Filmmaker/Config/EffectsPersistence.cpp
    Filmmaker/Config/EffectsPersistence.h
    Filmmaker/Config/FilmmakerSettingsStore.cpp
    Filmmaker/Config/FilmmakerSettingsStore.h
    Filmmaker/Config/HiddenDemoStore.cpp
    Filmmaker/Config/HiddenDemoStore.h

    Filmmaker/Demo/DemoEntry.h
    Filmmaker/Demo/DemoHeaderReader.cpp
    Filmmaker/Demo/DemoHeaderReader.h
    Filmmaker/Demo/DemoInfoReader.cpp
    Filmmaker/Demo/DemoInfoReader.h
    Filmmaker/Demo/DemoInfoHelper.cpp
    Filmmaker/Demo/DemoInfoHelper.h
    Filmmaker/Demo/DemoKeyframes.cpp
    Filmmaker/Demo/DemoRepair.cpp
    Filmmaker/Demo/DemoRepair.h
    Filmmaker/Demo/DemoProjectPath.cpp
    Filmmaker/Demo/DemoProjectPath.h
    Filmmaker/Demo/DemoProjectStore.cpp
    Filmmaker/Demo/DemoProjectStore.h
    Filmmaker/Demo/DemoProjectStoreMath.h
    Filmmaker/Demo/DemoProjectFileMath.h
    Filmmaker/Demo/DemoLibrary.cpp
    Filmmaker/Demo/DemoLibrary.h
    Filmmaker/Demo/DemoOrganizer.cpp
    Filmmaker/Demo/DemoOrganizer.h
    Filmmaker/Demo/DemoScanner.cpp
    Filmmaker/Demo/DemoScanner.h
    Filmmaker/Demo/PlayingDemoPath.cpp
    Filmmaker/Demo/PlayingDemoPath.h

    Filmmaker/PlayMenu/PlayMenuHud.cpp
    Filmmaker/PlayMenu/PlayMenuHud.h
    Filmmaker/PlayMenu/PlayMenuCommand.cpp
    Filmmaker/PlayMenu/PlayMenuStripJs.h
    Filmmaker/PlayMenu/WorkshopFilterJs.h
    Filmmaker/PlayMenu/WorkshopKeywordEditorJs.h

    Filmmaker/Community/CommunityHud.cpp
    Filmmaker/Community/CommunityHud.h
    Filmmaker/Community/CommunityJs.h
    Filmmaker/Community/BugReport.cpp
    Filmmaker/Community/BugReport.h
    Filmmaker/Community/BugReportMath.h
    Filmmaker/Community/BugReportSend.cpp
    Filmmaker/Community/BugReportSend.h
    Filmmaker/Community/CrashReport.cpp
    Filmmaker/Community/CrashReport.h
    Filmmaker/Community/CrashReportMath.h
    Filmmaker/Community/HcTrackerFeed.cpp
    Filmmaker/Community/HcTrackerFeed.h
    Filmmaker/Community/HcTrackerFeedMath.h
    Filmmaker/Platform/HttpRequest.cpp
    Filmmaker/Platform/HttpRequest.h
    Filmmaker/Platform/ShellOpen.cpp
    Filmmaker/Platform/ShellOpen.h
    Filmmaker/Platform/SystemUsage.cpp
    Filmmaker/Platform/SystemUsage.h

    Filmmaker/Workshop/WorkshopOrganizer.cpp
    Filmmaker/Workshop/WorkshopOrganizer.h

    Filmmaker/Panorama/DemoOrganizerJs.h
    Filmmaker/Panorama/FilmmakerGuiJs.h
    Filmmaker/Panorama/FilmmakerMenu.cpp
    Filmmaker/Panorama/FilmmakerMenu.h
    Filmmaker/Panorama/PanoramaBridge.cpp
    Filmmaker/Panorama/PanoramaBridge.h
    Filmmaker/Panorama/VideoSettingsResolution.cpp
    Filmmaker/Panorama/VideoSettingsResolution.h
    Filmmaker/Panorama/VideoSettingsResolutionJs.h
    Filmmaker/Panorama/MovieHud.cpp
    Filmmaker/Panorama/MovieHud.h
    Filmmaker/Panorama/MovieHudJs.h
    Filmmaker/Panorama/MarkerHud.cpp
    Filmmaker/Panorama/MarkerHud.h
    Filmmaker/Panorama/MarkerHudJs.h
    Filmmaker/Panorama/FxDebugHud.cpp
    Filmmaker/Panorama/FxDebugHud.h
    Filmmaker/Panorama/FxDebugHudJs.h
    Filmmaker/Panorama/CameraTimelineHud.cpp
    Filmmaker/Panorama/CameraTimelineHud.h
    Filmmaker/Panorama/UiLiveReload.cpp
    Filmmaker/Panorama/UiLiveReload.h
    Filmmaker/Panorama/WorkspacePreview.cpp
    Filmmaker/Panorama/WorkspacePreview.h
    Filmmaker/Panorama/CameraTimelineJs.h
    Filmmaker/Panorama/CameraTimelineRangeJs.h
    Filmmaker/Panorama/SpeedScrubberJs.h
    Filmmaker/Movie/DemoTimescale.cpp
    Filmmaker/Movie/DemoScrub.cpp
    Filmmaker/Movie/ScrubFrameCache.cpp
    Filmmaker/Movie/ScrubFrameCache.h
    Filmmaker/Movie/ScrubFrameCacheMath.h
    Filmmaker/Movie/DemoTimescale.h
    Filmmaker/Panorama/CameraEditorHud.cpp
    Filmmaker/Panorama/CameraEditorHud.h
    Filmmaker/Panorama/AimCrosshairHud.cpp
    Filmmaker/Panorama/AimCrosshairHud.h
    Filmmaker/Panorama/AimCrosshairHudJs.h
    Filmmaker/Panorama/EasterEggOverlayHud.cpp
    Filmmaker/Panorama/EasterEggOverlayHud.h
    Filmmaker/Panorama/EasterEggOverlayJs.h
    Filmmaker/Panorama/EasterEggSparklesJs.h
    Filmmaker/Panorama/EasterEggHintJs.h
    Filmmaker/Panorama/EasterEggVegasJs.h
    Filmmaker/Panorama/EasterEggClippyJs.h
    Filmmaker/Panorama/OnboardingHud.cpp
    Filmmaker/Panorama/OnboardingHud.h
    Filmmaker/Panorama/OnboardingJs.h
    Filmmaker/Panorama/OnboardingClippyJs.h
    Filmmaker/Panorama/OnboardingScriptJs.h
    Filmmaker/Panorama/OnboardingRoutesJs.h
    Filmmaker/Panorama/OnboardingGuideJs.h
    Filmmaker/Panorama/OnboardingLessonJs.h
    Filmmaker/Onboarding/Onboarding.cpp
    Filmmaker/Onboarding/Onboarding.h
    Filmmaker/Onboarding/PracticeSession.cpp
    Filmmaker/Onboarding/PracticeSession.h
    Filmmaker/Config/PracticeSessionFlag.h
    Filmmaker/EasterEggs/EasterEggs.cpp
    Filmmaker/EasterEggs/EasterEggs.h
    Filmmaker/EasterEggs/EasterEggHintMath.h
    Filmmaker/EasterEggs/EasterEggWorld.h
    Filmmaker/EasterEggs/EasterEggLighting.cpp
    Filmmaker/EasterEggs/EasterEggLightingMath.h
    Filmmaker/EasterEggs/EasterEggBodies.cpp
    Filmmaker/EasterEggs/EasterEggChickens.cpp
    Filmmaker/EasterEggs/EasterEggChickenMath.h
    Filmmaker/EasterEggs/EasterEggBay.cpp
    Filmmaker/EasterEggs/EasterEggAudioMath.h
    Filmmaker/EasterEggs/EasterEggSnake.cpp
    Filmmaker/EasterEggs/EasterEggSnake.h
    Filmmaker/EasterEggs/SnakeMath.h
    Filmmaker/EasterEggs/EasterEggGunMute.cpp
    Filmmaker/EasterEggs/EasterEggGunMute.h
    Filmmaker/EasterEggs/EasterEggUwu.cpp
    Filmmaker/EasterEggs/EasterEggWesMath.h
    Filmmaker/EasterEggs/EasterEggUwu.h
    Filmmaker/EasterEggs/Osu/OsuGame.cpp
    Filmmaker/EasterEggs/Osu/OsuAudio.cpp
    Filmmaker/EasterEggs/Osu/OsuAudio.h
    Filmmaker/EasterEggs/Osu/OsuLibrary.cpp
    Filmmaker/EasterEggs/Osu/OsuLibrary.h
    Filmmaker/EasterEggs/Osu/OsuPlatform.cpp
    Filmmaker/EasterEggs/Osu/OsuPlatform.h
    Filmmaker/EasterEggs/Osu/OsuHitsounds.cpp
    Filmmaker/EasterEggs/Osu/OsuSkin.cpp
    Filmmaker/EasterEggs/Osu/OsuSkin.h
    Filmmaker/EasterEggs/Osu/OsuHitsounds.h
    Filmmaker/EasterEggs/Osu/OsuGame.h
    Filmmaker/EasterEggs/Osu/OsuMath.h
    Filmmaker/EasterEggs/Osu/OsuDifficulty.h
    Filmmaker/EasterEggs/Osu/OsuRealm.h
    Filmmaker/Panorama/EasterEggOsuJs.h
    Filmmaker/Panorama/EasterEggSnakeJs.h
    Filmmaker/Panorama/EasterEggOsuPlayJs.h
    Filmmaker/Panorama/EasterEggOsuSelectJs.h
    Filmmaker/Panorama/EasterEggOsuMenusJs.h
    Filmmaker/Panorama/EasterEggOsuScreensJs.h
    Filmmaker/Panorama/EasterEggWorldJs.h
    Filmmaker/Panorama/InspectorWidth.cpp
    Filmmaker/Panorama/ConfigHud.cpp
    Filmmaker/Panorama/ConfigHud.h
    Filmmaker/Panorama/ConfigHudJs.h
    Filmmaker/Panorama/EditorSettingsPopupJs.h
    Filmmaker/Panorama/ConfigHudWidgetsJs.h
    Filmmaker/Panorama/ConfigHudCameraTabJs.h
    Filmmaker/Panorama/ConfigHudEffectsTabJs.h
    Filmmaker/Panorama/ConfigHudRagdollsTabJs.h
    Filmmaker/Panorama/ConfigHudSearchJs.h
    Filmmaker/MvmTestCommand.cpp
    Filmmaker/MvmTest.h
    Filmmaker/Cosmetics/CosmeticUiQueries.cpp
    Filmmaker/Cosmetics/CosmeticUiQueries.h
    Filmmaker/FilmmakerDebugCommand.cpp
    Filmmaker/FilmmakerDebugCommand.h
    Filmmaker/FilmmakerHealth.cpp
    Filmmaker/FilmmakerHealth.h
    Filmmaker/Panorama/PanoramaFindPanel.cpp
    Filmmaker/Panorama/PanoramaPerfProbe.cpp
    Filmmaker/Panorama/PanoramaFindPanel.h
    Filmmaker/Panorama/CameraEditorJs.h
    Filmmaker/Panorama/CameraEditorWidgetsJs.h
    Filmmaker/Panorama/CameraEditorCustomizeJs.h
    Filmmaker/Panorama/CameraEditorCustomizePreviewJs.h
    Filmmaker/Panorama/DeathAnimPreview.cpp
    Filmmaker/Panorama/DeathAnimPreview.h
    Filmmaker/Panorama/PreviewSceneProbe.cpp
    Filmmaker/Panorama/PreviewSceneProbe.h
    Filmmaker/Panorama/PreviewAnimClock.cpp
    Filmmaker/Panorama/PreviewAnimClock.h
    Filmmaker/Panorama/PreviewAnimTick.cpp
    Filmmaker/Panorama/PreviewAnimTick.h
    Filmmaker/Panorama/PreviewRenderProbe.cpp
    Filmmaker/Panorama/PreviewRenderProbe.h
    Filmmaker/Panorama/PreviewRenderFlag.cpp
    Filmmaker/Panorama/PreviewRenderFlag.h
    Filmmaker/Panorama/CameraEditorFollowInspectorJs.h
    Filmmaker/Panorama/CameraEditorPathInspectorJs.h
    Filmmaker/Panorama/CameraEditorDebugJs.h
    Filmmaker/Panorama/CameraEditorRenderJs.h
    Filmmaker/Panorama/GraphEditorExperimentHud.cpp
    Filmmaker/Panorama/GraphEditorExperimentHud.h
    Filmmaker/Panorama/GraphEditorJs.h
    Filmmaker/Panorama/DemoBarButtons.cpp
    Filmmaker/Panorama/DemoBarButtons.h
    Filmmaker/Panorama/DemoBarButtonsJs.h

    Filmmaker/Movie/CameraBridge.h
    Filmmaker/Movie/CamMarkers.cpp
    Filmmaker/Movie/CamMarkers.h
    Filmmaker/Movie/TimelineDock.cpp
    Filmmaker/Movie/TimelinePlayhead.cpp
    Filmmaker/Movie/TimelinePlayhead.h
    Filmmaker/Movie/CamPathShake.cpp
    Filmmaker/Movie/CamPathShake.h
    Filmmaker/Movie/RageShake.cpp
    Filmmaker/Movie/RageShake.h
    Filmmaker/Movie/RageShakeStyles.cpp
    Filmmaker/Movie/CamPathEval.cpp
    Filmmaker/Movie/CamPathEval.h
    Filmmaker/Movie/CamPathTimingMath.h
    Filmmaker/Movie/CamPlayback.cpp
    Filmmaker/Movie/CamPlayback.h
    Filmmaker/Movie/CameraPath.cpp
    Filmmaker/Movie/CameraPathPicking.cpp
    Filmmaker/Movie/CameraPath.h
    Filmmaker/Movie/SceneKeyframeCatalog.cpp
    Filmmaker/Movie/SceneKeys.cpp
    Filmmaker/Movie/SceneKeys.h
    Filmmaker/Movie/SceneRig.cpp
    Filmmaker/Movie/SceneRig.h
    Filmmaker/Movie/SceneRigMath.h
    Filmmaker/Movie/SceneRigTransient.h
    Filmmaker/Movie/SceneKeyframeMath.h
    Filmmaker/Movie/GraphExpModel.cpp
    Filmmaker/Movie/GraphExpModel.h
    Filmmaker/Movie/FollowCamera.cpp
    Filmmaker/Movie/FollowCamera.h
    Filmmaker/Movie/FollowCameraMath.cpp
    Filmmaker/Movie/FollowCameraMath.h
    Filmmaker/Movie/FollowTargetProviders.cpp
    Filmmaker/Movie/FollowTargetProviders.h
    Filmmaker/Movie/FollowTargetDiscovery.cpp
    Filmmaker/Movie/FollowTargetDiscovery.h
    Filmmaker/Movie/FollowEventIndex.cpp
    Filmmaker/Movie/FollowEventIndex.h
    Filmmaker/Movie/FollowCameraStateJson.cpp
    Filmmaker/Movie/DemoEndHold.cpp
    Filmmaker/Movie/DemoEndHold.h
    Filmmaker/Movie/PlaybackLoop.cpp
    Filmmaker/Movie/PlaybackLoop.h
    Filmmaker/Movie/SmoothView.cpp
    Filmmaker/Movie/SmoothView.h
    Filmmaker/Movie/DemoAimResampler.cpp
    Filmmaker/Movie/DemoAimResampler.h
    Filmmaker/Movie/DemoAimResamplerMath.h
    Filmmaker/Movie/SmoothViewTest.cpp
    Filmmaker/Movie/SmoothViewTest.h
    Filmmaker/Movie/ViewFx.cpp
    Filmmaker/Movie/ViewFx.h
    Filmmaker/Movie/ViewFxTypes.h
    Filmmaker/Movie/ViewFxWeaponSway.cpp
    Filmmaker/Movie/ViewFxWeaponSway.h
    Filmmaker/Movie/ViewFxWeaponFloat.cpp
    Filmmaker/Movie/ViewFxWeaponFloat.h
    Filmmaker/Movie/ViewFxDeadzoneAim.cpp
    Filmmaker/Movie/ViewFxDeadzoneAim.h
    Filmmaker/Movie/CameraOwnership.h
    Filmmaker/Movie/ViewFxVm.cpp
    Filmmaker/Movie/ViewFxVm.h
    Filmmaker/Movie/ViewFxVmCalcHook.cpp
    Filmmaker/Movie/ViewFxVmCommand.cpp
    Filmmaker/Movie/ViewFxVmInternal.h
    Filmmaker/Movie/ViewFxVmSceneTransform.cpp
    # Camera-shake layer stack (handheld / recoil), ported from GMod addons --
    # see Docs/features/filmmaker_camera_shake.md. HandheldCamCurves.cpp is GENERATED by
    # FX/tools/convert-shakify-curves.py; do not hand-edit it.
    Filmmaker/Movie/CameraShakeTypes.h
    Filmmaker/Movie/HudBlackout.cpp
    Filmmaker/Movie/HudBlackout.h
    Filmmaker/Movie/CameraShakeStack.cpp
    Filmmaker/Movie/CameraShakeStack.h
    Filmmaker/Movie/GunWeaponMath.h
    Filmmaker/Movie/HandheldCam.cpp
    Filmmaker/Movie/HandheldCam.h
    Filmmaker/Movie/HandheldCamCurves.cpp
    Filmmaker/Movie/WeaponRecoil.cpp
    Filmmaker/Movie/WeaponRecoil.h
    Filmmaker/Movie/WeaponBobProbe.cpp
    Filmmaker/Movie/WeaponBobProbe.h
    Filmmaker/Movie/WeaponPivot.cpp
    Filmmaker/Movie/WeaponPivot.h
    Filmmaker/Movie/WeaponBob.cpp
    Filmmaker/Movie/WeaponBob.h
    Filmmaker/Movie/WeaponBobMath.h
    Filmmaker/Movie/WeaponRecoilPresets.h
    Filmmaker/Movie/WeaponRecoilStyles.h
    Filmmaker/Lens/LensFx.cpp
    Filmmaker/Lens/LensFx.h
    Filmmaker/Lens/LensFxMath.h
    Filmmaker/Lens/LensFxShaders.h
    Filmmaker/Movie/RecoilPivotMarker.cpp
    Filmmaker/Movie/RecoilPivotMarker.h
    Filmmaker/Movie/RecoilPivotMarkerMath.h
    Filmmaker/Movie/ParticleFx.cpp
    Filmmaker/Movie/ParticleFx.h
    Filmmaker/Movie/ParticleFxInternal.h
    Filmmaker/Movie/ParticleFxRules.cpp
    Filmmaker/Movie/PerWeaponFx.cpp
    Filmmaker/Movie/ParticleFxHook.cpp
    Filmmaker/Movie/ParticleFxMoney.cpp
    Filmmaker/Movie/ParticleFxFlashbang.cpp
    Filmmaker/Movie/ParticleFxMollyLights.cpp
    Filmmaker/Movie/ParticleFxSettings.cpp
    Filmmaker/Movie/ParticleFxDiagnostics.cpp
    Filmmaker/Movie/ParticleFxPrefetch.cpp
    Filmmaker/Environment/SurfaceRayGridMath.h
    Filmmaker/Movie/RagdollPhysics.cpp
    Filmmaker/Movie/RagdollPhysics.h
    Filmmaker/Movie/DeathAnimations.cpp
    Filmmaker/Movie/DeathAnimations.h
    Filmmaker/Movie/DeathAnimationsMath.h
    Filmmaker/Movie/NervousSystem.cpp
    Filmmaker/Movie/NervousSystem.h
    Filmmaker/Movie/NervousSystemMath.h
    Filmmaker/Movie/RagdollBodies.cpp
    Filmmaker/Movie/RagdollBodies.h
    Filmmaker/Movie/RagdollWeaponGround.cpp
    Filmmaker/Movie/RagdollWeaponGround.h
    Filmmaker/Movie/RagdollWeaponPhysics.cpp
    Filmmaker/Movie/RagdollWeaponPhysics.h
    Filmmaker/Movie/RagdollWeapon.cpp
    Filmmaker/Movie/RagdollWeapon.h
    Filmmaker/Movie/RagdollWeaponRates.h
    Filmmaker/Movie/RagdollWeaponMath.h
    Filmmaker/Movie/RagdollWeaponArmPhysics.cpp
    Filmmaker/Movie/RagdollWeaponArmPhysics.h
    Filmmaker/Movie/RagdollWeaponArmPhysicsMath.h
    Filmmaker/Movie/MethodActing.cpp
    Filmmaker/Movie/MethodActing.h
    Filmmaker/Movie/MethodActingFlex.cpp
    Filmmaker/Movie/MethodActingFlex.h
    Filmmaker/Movie/MethodActingGaze.cpp
    Filmmaker/Movie/MethodActingGaze.h
    Filmmaker/Movie/ModelPoseAccess.h
    Filmmaker/Movie/MethodActingMath.h
    Filmmaker/Movie/FxAlign.cpp
    Filmmaker/Movie/FxAlign.h
    Filmmaker/Movie/MovieMode.cpp
    Filmmaker/Movie/MovieMode.h
    Filmmaker/Movie/UiClickMath.h

    Filmmaker/World/WorldSchema.cpp
    Filmmaker/World/WorldSchema.h
    Filmmaker/World/WorldEntities.cpp
    Filmmaker/World/WorldEntities.h
    Filmmaker/World/WorldHealth.cpp
    Filmmaker/World/WorldHealth.h
    Filmmaker/World/WorldCommand.cpp
    Filmmaker/World/WorldCommand.h
    Filmmaker/World/ShadowMapProbe.cpp
    Filmmaker/World/ShadowMapProbe.h
    Filmmaker/World/NightMode.cpp
    Filmmaker/World/AutoExposure.cpp
    Filmmaker/World/NightMode.h
    Filmmaker/World/SmokeFx.cpp
    Filmmaker/World/SmokeFx.h
    Filmmaker/World/SkyboxLibrary.cpp
    Filmmaker/World/SkyboxLibrary.h
    Filmmaker/World/CloudFx.cpp
    Filmmaker/World/CloudFx.h
    Filmmaker/World/SunFx.cpp
    Filmmaker/World/SunFx.h
    Filmmaker/World/LightFx.cpp
    Filmmaker/World/LightFx.h
    Filmmaker/World/BakedShadowFx.cpp
    Filmmaker/World/BakedShadowFx.h
    Filmmaker/World/DynamicShadowFx.cpp
    Filmmaker/World/DynamicShadowFx.h
    Filmmaker/World/ShadowStageProbe.cpp
    Filmmaker/World/ShadowStageProbe.h
    Filmmaker/World/ShadowFx.cpp
    Filmmaker/World/ShadowFx.h
    Filmmaker/World/CustomShadowFx.cpp
    Filmmaker/World/CustomShadowFx.h
    Filmmaker/World/CustomShadowAtlas.cpp
    Filmmaker/World/CustomShadowAtlas.h
    Filmmaker/World/LightTintMath.h
    Filmmaker/World/MaterialTintOwnership.h
    Filmmaker/World/WorldModulation.cpp
    Filmmaker/World/WorldModulation.h
    Filmmaker/World/SkyboxBuilder.cpp
    Filmmaker/World/SkyboxBuilder.h
	Filmmaker/World/SkyOrient.cpp
	Filmmaker/World/SkyOrient.h
	Filmmaker/World/SkyGpuRotation.cpp
	Filmmaker/World/SkyGpuRotation.h
    Filmmaker/World/SkyGpuProbe.cpp
    Filmmaker/World/SkyGpuProbe.h
    Filmmaker/Panorama/ConfigHudLightingTabJs.h
    Filmmaker/Panorama/ConfigHudAtmosphereTabJs.h
    Filmmaker/Panorama/ConfigHudWeatherSkyTabJs.h

    Filmmaker/Cosmetics/CosmeticProfile.cpp
    Filmmaker/Cosmetics/CosmeticProfile.h
    Filmmaker/Cosmetics/CosmeticCatalog.cpp
    Filmmaker/Cosmetics/CosmeticCatalog.h
    Filmmaker/Cosmetics/CosmeticEntityAccess.cpp
    Filmmaker/Cosmetics/CosmeticEntityAccess.h
    Filmmaker/Cosmetics/CosmeticRebuildFlags.h
    Filmmaker/Cosmetics/CosmeticOverrideSystem.cpp
    Filmmaker/Cosmetics/CosmeticThemeRestore.cpp
    Filmmaker/Cosmetics/CosmeticDemoSync.cpp
    Filmmaker/Cosmetics/CosmeticPaintKitBridge.cpp
    Filmmaker/Cosmetics/CosmeticModelSwapInternal.h
    Filmmaker/Cosmetics/CosmeticFnResolver.cpp
    Filmmaker/Cosmetics/CosmeticKnifeSwap.cpp
    Filmmaker/Cosmetics/CosmeticGloveSwap.cpp
    Filmmaker/Cosmetics/CosmeticOverrideSystem.h
    Filmmaker/Cosmetics/CosmeticModelSwap.cpp
    Filmmaker/Cosmetics/CosmeticModelSwap.h
    Filmmaker/Cosmetics/CosmeticDirectComposite.cpp
    Filmmaker/Cosmetics/CosmeticDirectComposite.h
    Filmmaker/Cosmetics/CosmeticAnimFix.cpp
    Filmmaker/Cosmetics/CosmeticAnimFix.h
    Filmmaker/Cosmetics/CosmeticCommands.cpp
    Filmmaker/Cosmetics/CosmeticDebug.cpp
    Filmmaker/Cosmetics/CosmeticDebugInternal.h
    Filmmaker/Cosmetics/CosmeticDebugEvents.cpp
    Filmmaker/Debug/MvmDebugLog.cpp
    Filmmaker/Debug/MvmDebugLog.h
    Filmmaker/Debug/GpuPassProfiler.cpp
    Filmmaker/Debug/GpuPassProfiler.h
    Filmmaker/Debug/GpuPassProfileMath.h
    Filmmaker/Cosmetics/CosmeticGloveLabels.cpp
    Filmmaker/Cosmetics/CosmeticGloveLabels.h
    Filmmaker/Cosmetics/CosmeticGloveSkinTable.cpp

    Filmmaker/Platform/DemoFilePicker.cpp
    Filmmaker/Platform/DemoFilePicker.h
    Filmmaker/Platform/FolderPicker.cpp
    Filmmaker/Platform/FolderPicker.h
    Filmmaker/Platform/JsonBuilder.cpp
    Filmmaker/Platform/JsonBuilder.h
    Filmmaker/Platform/JsonParser.cpp
    Filmmaker/Platform/JsonParser.h
    Filmmaker/Platform/AtomicFile.h
    Filmmaker/Platform/ProtobufWire.cpp
    Filmmaker/Platform/ProtobufWire.h
    Filmmaker/Platform/RawMouse.cpp
    Filmmaker/Platform/RawMouse.h
    Filmmaker/Platform/TextEncoding.cpp
    Filmmaker/Platform/TextEncoding.h

     Filmmaker/Render/RenderTypes.h
     Filmmaker/Render/RenderHdrCapture.cpp
     Filmmaker/Render/RenderHdrCapture.h
     Filmmaker/Platform/ExrWriter.cpp
     Filmmaker/Platform/ExrWriter.h
     Filmmaker/Render/RenderRange.cpp
    Filmmaker/Render/RenderRange.h
    Filmmaker/Render/RenderCommand.cpp
    Filmmaker/Render/RenderCommand.h
    Filmmaker/Render/RenderSession.cpp
    Filmmaker/Render/RenderSession.h
    Filmmaker/Render/RenderValidation.cpp
    Filmmaker/Render/RenderValidation.h
    Filmmaker/Render/RenderStateGuard.cpp
    Filmmaker/Render/RenderStateGuard.h
    Filmmaker/Render/RenderOutput.cpp
	Filmmaker/Render/RenderEncoding.cpp
    Filmmaker/Render/RenderOutput.h
    Filmmaker/Render/RenderResolution.cpp
    Filmmaker/Render/RenderResolution.h
    Filmmaker/Render/RenderTargetProbe.cpp
    Filmmaker/Render/RenderTargetProbe.h
    Filmmaker/Render/RenderViewportRecordProbe.cpp
    Filmmaker/Render/RenderViewportRecordProbe.h
    Filmmaker/Render/RenderSupersample.cpp
    Filmmaker/Render/RenderSupersample.h
    Filmmaker/Render/RenderEngineSize.cpp
    Filmmaker/Render/RenderEngineSize.h
    Filmmaker/Render/RenderJob.h
    Filmmaker/Render/RenderManifest.cpp
    Filmmaker/Render/RenderManifest.h
    Filmmaker/Render/RenderEncoderDetect.cpp
    Filmmaker/Render/RenderEncoderDetect.h
    Filmmaker/Render/RenderSessionDriver.cpp
    Filmmaker/Render/RenderSessionDriver.h
    Filmmaker/Render/RenderWorldOnly.cpp
    Filmmaker/Render/RenderWorldOnly.h
    Filmmaker/Render/RenderWorldPlayersNoGun.cpp
    Filmmaker/Render/RenderWorldPlayersNoGun.h
    Filmmaker/Render/RenderPlayersOnlyGreen.cpp
    Filmmaker/Render/RenderPlayersOnlyGreen.h

    Filmmaker/Window/AspectRatioMath.h
    Filmmaker/Window/WindowPrecisionGuard.h
    Filmmaker/Window/EngineVideoMode.cpp
    Filmmaker/Window/EngineVideoMode.h
    Filmmaker/Window/Sdl3Window.cpp
    Filmmaker/Window/Sdl3Window.h
    Filmmaker/Window/WindowSizeOverlay.cpp
    Filmmaker/Window/WindowSizeOverlay.h
    Filmmaker/Window/WindowSnapMath.h
    Filmmaker/Window/WindowResize.cpp
    Filmmaker/Window/WindowResize.h
    Filmmaker/Window/UnfocusedRate.cpp
    Filmmaker/Window/UnfocusedRate.h

    Filmmaker/Movie/CameraRuntimeInternal.h
    Filmmaker/Movie/CameraBridge.cpp
    Filmmaker/MouseCapture.cpp
    Filmmaker/MouseCapture.h

    WindowInputHooks.cpp
    WindowInputHooks.h
    ConsoleCapture.cpp
    ConsoleCapture.h
    EngineLogQuiet.cpp
    EngineLogQuiet.h
    UiInputGate.cpp
    UiInputGate.h
)

source_group("AfxHookSource2/Filmmaker" REGULAR_EXPRESSION "/AfxHookSource2/Filmmaker/")

# This is far form proper, but it will do for now:
source_group("Deps/release/Detours" REGULAR_EXPRESSION "/Deps/release/Detours/")
source_group("Deps/release/Detours/src" REGULAR_EXPRESSION "/Deps/release/Detours/src/")
source_group("Deps/release/prop" REGULAR_EXPRESSION "/Deps/release/prop/")
source_group("Deps/release/prop/AfxHookSource" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource")
source_group("Deps/release/prop/cs2" REGULAR_EXPRESSION "/Deps/release/prop/cs2/")
source_group("Deps/release/prop/cs2/sdk_src/public" REGULAR_EXPRESSION "/Deps/release/prop/cs2/sdk_src/public/")
source_group("Deps/release/prop/cs2/sdk_src/public/appframework" REGULAR_EXPRESSION "/Deps/release/prop/cs2/sdk_src/public/appframework/")
source_group("Deps/release/prop/cs2/sdk_src/public/interfaces" REGULAR_EXPRESSION "/Deps/release/prop/cs2/sdk_src/public/interfaces/")
source_group("Deps/release/prop/cs2/sdk_src/public/tier0" REGULAR_EXPRESSION "/Deps/release/prop/cs2/sdk_src/public/tier0/")
source_group("Deps/release/prop/cs2/sdk_src/public/tier1" REGULAR_EXPRESSION "/Deps/release/prop/cs2/sdk_src/public/tier1/")


source_group("shared" REGULAR_EXPRESSION "/Shared/")
source_group("Shared/hooks" REGULAR_EXPRESSION "/Shared/hooks/")
source_group("Shared/vcpp" REGULAR_EXPRESSION "/Shared/vcpp/")
source_group("AfxHookSource2" REGULAR_EXPRESSION "/AfxHookSource2/")

# Compile DXR bytecode at build time; the shipped backend needs no DXC installation.
get_filename_component(MVM_WINDOWS_KITS_ROOT "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
file(GLOB MVM_DXC_CANDIDATES "${MVM_WINDOWS_KITS_ROOT}/bin/*/x64/dxc.exe")
list(SORT MVM_DXC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
find_program(MVM_DXC dxc HINTS ${MVM_DXC_CANDIDATES})
if(NOT MVM_DXC AND MVM_DXC_CANDIDATES)
    list(GET MVM_DXC_CANDIDATES 0 MVM_DXC)
endif()
if(NOT MVM_DXC)
    message(FATAL_ERROR "The Windows SDK x64 DXC compiler is required for environment ray tracing")
endif()
set(MVM_RAY_HEADER "${CMAKE_CURRENT_BINARY_DIR}/EnvironmentRayQueryBytecode.h")
add_custom_command(OUTPUT "${MVM_RAY_HEADER}"
    COMMAND "${MVM_DXC}" -T cs_6_5 -E Main -O3 -Fh "${MVM_RAY_HEADER}" -Vn g_EnvironmentRayQuery
        "${CMAKE_CURRENT_SOURCE_DIR}/Filmmaker/Environment/EnvironmentRayQuery.hlsl"
    DEPENDS Filmmaker/Environment/EnvironmentRayQuery.hlsl VERBATIM)
add_library(EnvironmentRayBackend STATIC Filmmaker/Environment/EnvironmentRayBackend.cpp "${MVM_RAY_HEADER}")
target_include_directories(EnvironmentRayBackend PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(EnvironmentRayBackend PRIVATE cxx_std_17)
target_link_libraries(EnvironmentRayBackend PUBLIC d3d12 dxgi)
target_sources(AfxHookSource2 PRIVATE Filmmaker/Environment/EnvironmentRayProbe.cpp)
target_link_libraries(AfxHookSource2 PRIVATE EnvironmentRayBackend)

# Native material replay preserves authored weapon BRDF inputs for environment sun.
target_sources(AfxHookSource2 PRIVATE Filmmaker/Environment/EnvironmentNativeMaterial.cpp)
add_executable(RelightNativeClusterTests ../Automation/tests/relight-native-cluster-tests.cpp Filmmaker/Relight/RelightNativeCluster.cpp)
target_compile_features(RelightNativeClusterTests PRIVATE cxx_std_17)
target_link_libraries(RelightNativeClusterTests PRIVATE d3d11 d3dcompiler dxguid)
add_test(NAME RelightNativeClusterTests COMMAND RelightNativeClusterTests "${CMAKE_CURRENT_SOURCE_DIR}/../Automation/output/engine-clusters/fixtures")
set_tests_properties(RelightNativeClusterTests PROPERTIES SKIP_RETURN_CODE 77)

foreach(ao_target AfxHookSource2)
    target_sources(${ao_target} PRIVATE Filmmaker/Environment/EnvironmentAmbientOcclusion.cpp)
    target_sources(${ao_target} PRIVATE Filmmaker/Environment/EnvironmentSunVisibility.cpp)
    target_sources(${ao_target} PRIVATE Filmmaker/Environment/EnvironmentCasterBounds.cpp)
endforeach()

install(FILES ../Resources/LICENSES/DxilHash/LICENSE.txt DESTINATION "bin/resources/LICENSES/DxilHash" COMPONENT x64-source2)

target_sources(AfxHookSource2 PRIVATE Filmmaker/World/SmokeAppearance.cpp)

# Native map data extraction (Filmmaker/MapData): engine-free Source 2 package, resource and
# KeyValues3 readers plus the lighting cache writers. Shared by the hook, the MapDataTool CLI
# and the tests. Vendored decoders: lz4, zstd (decompression only), meshoptimizer, bcdec.
add_library(MapData STATIC
    Filmmaker/MapData/MapCacheCodec.cpp
    Filmmaker/MapData/MapCacheCodec.h
    Filmmaker/MapData/MapDataIo.cpp
    Filmmaker/MapData/MapDataIo.h
    Filmmaker/MapData/MapEntities.cpp
    Filmmaker/MapData/MapEntities.h
    Filmmaker/MapData/MapEntityKnownKeys.h
    Filmmaker/MapData/MapCasters.cpp
    Filmmaker/MapData/MapCasters.h
    Filmmaker/MapData/MapExtract.cpp
    Filmmaker/MapData/MapExtract.h
    Filmmaker/MapData/MapGeometry.h
    Filmmaker/MapData/MapLightmap.cpp
    Filmmaker/MapData/MapLightmap.h
    Filmmaker/MapData/MapMeshes.cpp
    Filmmaker/MapData/MapMeshes.h
    Filmmaker/MapData/MapTextures.cpp
    Filmmaker/MapData/MapTextures.h
    Filmmaker/Platform/JsonParser.cpp
    Filmmaker/MapData/MapPhysics.cpp
    Filmmaker/MapData/MapPhysics.h
    Filmmaker/MapData/MapSources.cpp
    Filmmaker/MapData/MapSources.h
    ../Deps/release/lz4/lz4.c
    ../Deps/release/zstd/lib/common/entropy_common.c
    ../Deps/release/zstd/lib/common/error_private.c
    ../Deps/release/zstd/lib/common/fse_decompress.c
    ../Deps/release/zstd/lib/common/xxhash.c
    ../Deps/release/zstd/lib/common/zstd_common.c
    ../Deps/release/zstd/lib/decompress/huf_decompress.c
    ../Deps/release/zstd/lib/decompress/zstd_ddict.c
    ../Deps/release/zstd/lib/decompress/zstd_decompress.c
    ../Deps/release/zstd/lib/decompress/zstd_decompress_block.c
    ../Deps/release/meshoptimizer/indexcodec.cpp
    ../Deps/release/meshoptimizer/vertexcodec.cpp
    ../Deps/release/meshoptimizer/vertexfilter.cpp
)
target_compile_features(MapData PUBLIC cxx_std_17)
target_compile_definitions(MapData PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_NO_TRACE=1 ZSTD_MULTITHREAD_SUPPORT_DEFAULT=0)
target_link_libraries(MapData PUBLIC bcrypt gdiplus cabinet)
target_link_libraries(AfxHookSource2 PRIVATE MapData)

add_executable(MapDataTool ../Automation/tools/map-data-tool.cpp)
target_link_libraries(MapDataTool PRIVATE MapData)

include("${CMAKE_CURRENT_SOURCE_DIR}/../CMake/Source2Tests.cmake")

target_sources(EnvironmentRenderTests PRIVATE Filmmaker/Relight/RelightNativeCluster.cpp)
