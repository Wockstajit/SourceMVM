cmake_minimum_required (VERSION 3.24)

project ("AfxHookSource")

###############################################################################
## BEGIN Shaders

add_custom_target(AfxHookSourceShaders
)

macro(add_shader name profile)
    add_custom_command(
        WORKING_DIRECTORY "${shaderbuilder_BINARY_DIR}"
        OUTPUT
            "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/${name}.acs"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/${name}.h"
        DEPENDS
            shaderbuilder
            "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/${name}.fxc"
        COMMAND
            "${shaderbuilder_BINARY_DIR}/ShaderBuilder.exe" --profile "${profile}" --in "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/${name}.fxc" --outPrefix "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/${name}"
    )
    target_sources(AfxHookSourceShaders
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/${name}.acs"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/${name}.h"
    )
endmacro()

add_shader("afx_line_vs20" "vs_2_0")
add_shader("afx_line_ps20" "ps_2_0")
add_shader("afx_drawtexture_ps20" "ps_2_0")

if(CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
    add_shader("afx_pgldraw_vs20" "vs_2_0")
    add_shader("afx_pgldraw_ps20" "ps_2_0")
    add_shader("afx_depth_ps20" "ps_2_0")
endif()

## END Shaders
###############################################################################

###############################################################################
## BEGIN protobuf

if(CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
    #find_package(protobuf CONFIG REQUIRED)
    include(${protobuf_SOURCE_DIR}/cmake/protobuf-generate.cmake)

    add_library(AfxHookSourceProtobufs OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/../Deps/release/prop/protobuf/csgo/netmessages.proto")

    target_link_libraries(AfxHookSourceProtobufs PUBLIC protobuf::libprotobuf)

    set(PROTO_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/csgo")

    target_include_directories(AfxHookSourceProtobufs PUBLIC "$<BUILD_INTERFACE:${PROTO_BINARY_DIR}>")

    protobuf_generate(
        TARGET AfxHookSourceProtobufs
        IMPORT_DIRS "${protobuf_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/../Deps/release/prop/protobuf/csgo"
        PROTOC_OUT_DIR "${PROTO_BINARY_DIR}")
endif()

## END protobuf
###############################################################################


add_library(AfxHookSource MODULE)

add_dependencies(AfxHookSource
    OpenEXR
    AfxHookSourceShaders
)

target_compile_definitions(AfxHookSource PRIVATE
    AFX_INTEROP
    AFX_MIRV_PGL
    _CRT_SECURE_NO_WARNINGS
    # Shared SOURCE:MVM filmmaker files compiled into this hook leave out their CS2-only parts.
    GAME_SOURCE1
    # Dear ImGui reads the Source 1 UI's configuration (Filmmaker/Ui/UiImConfig.h).
    IMGUI_USER_CONFIG="Filmmaker/Ui/UiImConfig.h"
)

# generate PDB in release too:
target_compile_options(AfxHookSource PRIVATE $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:RELEASE>>:/Zi>)
target_link_options(AfxHookSource PRIVATE $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:RELEASE>>:/DEBUG /DEBUG /OPT:REF /OPT:ICF>)

target_include_directories(AfxHookSource PRIVATE
    ./
    ../Deps/release/prop/AfxHookSource
    ../
    ../Deps/release/imgui
    ../Deps/release/prop
    "${AFX_OPENEXR_INCLUDE_DIRECTORIES}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_directories(AfxHookSource PRIVATE
    "${AFX_OPENEXR_LINK_DIRECTORIES}"
)

target_link_libraries(AfxHookSource PRIVATE
    "${AFX_OPENEXR_LINK_LIBRARIES}"
)

target_sources(AfxHookSource PRIVATE   

    csgo/hooks/engine/cmd.cpp
    csgo/hooks/engine/cmd.h

    tf2/ClientToolsTf2.cpp
    tf2/ClientToolsTf2.h

    addresses.cpp
    addresses.h

    AfxShaders.cpp
    AfxShaders.h
    AfxStreams.cpp
    AfxStreams.h
    CampathDrawer.cpp
    CampathDrawer.h
    ClientTools.cpp
    ClientTools.h
    d3d9Hooks.cpp
    d3d9Hooks.h
    D3D9ImageBuffer.h
    Filmmaker/CSS/CssProfile.cpp
    Filmmaker/SourceCameraBridge.cpp
    Filmmaker/SourceCameraPath.cpp
    Filmmaker/SourceCameraPath.h
    Filmmaker/SourceCapture.cpp
    Filmmaker/SourceCapture.h
    Filmmaker/SourceConCommand.cpp
    Filmmaker/SourceConCommand.h
    Filmmaker/SourceConsole.cpp
    Filmmaker/SourceConsole.h
    Filmmaker/DemoConvert/ConvertV34.cpp
    Filmmaker/DemoConvert/ConvertV34.h
    Filmmaker/DemoConvert/DemoBits.h
    Filmmaker/DemoConvert/DemoTables.cpp
    Filmmaker/DemoConvert/DemoTables.h
    Filmmaker/DemoConvert/MapModels.cpp
    Filmmaker/DemoConvert/MapModels.h
    Filmmaker/SourceDebugLog.cpp
    Filmmaker/SourceDemoBar.cpp
    Filmmaker/SourceDemoBar.h
    Filmmaker/SourceDemoBarModel.cpp
    Filmmaker/SourceDemoBarModel.h
    Filmmaker/SourceDemoFile.cpp
    Filmmaker/SourceDemoFile.h
    Filmmaker/SourceDemoIndex.cpp
    Filmmaker/SourceDemoIndex.h
    Filmmaker/SourceDemoMarkers.cpp
    Filmmaker/SourceDemoMarkers.h
    Filmmaker/SourceDirector.cpp
    Filmmaker/SourceDirector.h
    Filmmaker/SourceEngine.cpp
    Filmmaker/SourceEngine.h
    Filmmaker/SourceFollowCamera.cpp
    Filmmaker/SourceFollowCamera.h
    Filmmaker/SourceFilmmaker.cpp
    Filmmaker/SourceFilmmaker.h
    Filmmaker/SourceGameProfile.cpp
    Filmmaker/SourceGameProfile.h
    Filmmaker/SourceMainThread.cpp
    Filmmaker/SourceMainThread.h
    Filmmaker/SourceNetcon.cpp
    Filmmaker/SourceNetcon.h
    Filmmaker/SourceNetvars.h
    Filmmaker/SourceOldDemos.cpp
    Filmmaker/SourceOldDemos.h
    Filmmaker/SourceSdk2013.h
    Filmmaker/SourceSmoothView.cpp
    Filmmaker/SourceSmoothView.h
    Filmmaker/SourceTimelineDock.cpp
    Filmmaker/SourceTimelineDock.h
    Filmmaker/SourceTimelinePlayhead.cpp
    Filmmaker/SourceTimelinePlayhead.h
    Filmmaker/SourceUi.cpp
    Filmmaker/SourceUi.h
    Filmmaker/Ui/CameraEditorShell.cpp
    Filmmaker/Ui/CameraEditorShell.h
    Filmmaker/Ui/DemoBar.cpp
    Filmmaker/Ui/DemoBar.h
    Filmmaker/Ui/DemoBarMath.h
    Filmmaker/Ui/DirectorHud.cpp
    Filmmaker/Ui/DirectorHud.h
    Filmmaker/Ui/DirectorMath.h
    Filmmaker/Ui/DirectorMouse.h
    Filmmaker/Ui/HudCard.h
    Filmmaker/Ui/InspectorCards.cpp
    Filmmaker/Ui/InspectorCards.h
    Filmmaker/Ui/MarkerPopup.cpp
    Filmmaker/Ui/MarkerPopup.h
    Filmmaker/Ui/PathOverlay.cpp
    Filmmaker/Ui/PathInspector.cpp
    Filmmaker/Ui/PathInspector.h
    Filmmaker/Ui/PathOverlay.h
    Filmmaker/Ui/TimelineDock.cpp
    Filmmaker/Ui/TimelineDock.h
    Filmmaker/Ui/TimelineDockMath.h
    Filmmaker/Ui/UiImConfig.h
    Filmmaker/Ui/UiKeys.h
    Filmmaker/Ui/UiRenderDx9.cpp
    Filmmaker/Ui/UiRenderDx9.h
    Filmmaker/Ui/UiStyle.cpp
    Filmmaker/Ui/UiStyle.h
    Filmmaker/Ui/UiTheme.h
    Filmmaker/Ui/UiTypes.h
    Filmmaker/Ui/UiWidgets.cpp
    Filmmaker/Ui/UiWidgets.h
    Filmmaker/Ui/WorkspaceLayoutMath.h
    Filmmaker/Ui/WorkspaceMouse.h

    # Dear ImGui (MIT), the Source 1 UI's widget layer.
    ../Deps/release/imgui/imgui.cpp
    ../Deps/release/imgui/imgui.h
    ../Deps/release/imgui/imgui_draw.cpp
    ../Deps/release/imgui/imgui_tables.cpp
    ../Deps/release/imgui/imgui_widgets.cpp

    # Shared with the CS2 and Deadlock hooks, compiled from the CS2 tree.
    ../AfxHookSource2/Filmmaker/Debug/DebugLogDedupMath.h
    ../AfxHookSource2/Filmmaker/Debug/MvmDebugLog.cpp
    ../AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h
    ../AfxHookSource2/Filmmaker/Debug/MvmDebugLogHost.h
    ../AfxHookSource2/Filmmaker/Demo/DemoMatchInfoFile.h
    ../AfxHookSource2/Filmmaker/Demo/DemoProjectFileMath.h
    ../AfxHookSource2/Filmmaker/Demo/DemoProjectPath.cpp
    ../AfxHookSource2/Filmmaker/Demo/DemoProjectPath.h
    ../AfxHookSource2/Filmmaker/Demo/DemoProjectStore.cpp
    ../AfxHookSource2/Filmmaker/Demo/DemoProjectStore.h
    ../AfxHookSource2/Filmmaker/Demo/DemoProjectStoreMath.h
    ../AfxHookSource2/Filmmaker/FilmmakerCommandUtil.cpp
    ../AfxHookSource2/Filmmaker/FilmmakerCommandUtil.h
    ../AfxHookSource2/Filmmaker/FollowCommand.cpp
    ../AfxHookSource2/Filmmaker/Platform/JsonBuilder.cpp
    ../AfxHookSource2/Filmmaker/Platform/JsonBuilder.h
    ../AfxHookSource2/Filmmaker/Platform/JsonParser.cpp
    ../AfxHookSource2/Filmmaker/Platform/JsonParser.h
    ../AfxHookSource2/Filmmaker/Platform/TextEncoding.cpp
    ../AfxHookSource2/Filmmaker/Platform/TextEncoding.h
    ../AfxHookSource2/Filmmaker/Movie/CamMarkerPickMath.h
    ../AfxHookSource2/Filmmaker/Movie/CamMarkers.cpp
    ../AfxHookSource2/Filmmaker/Movie/CamMarkers.h
    ../AfxHookSource2/Filmmaker/Movie/CamPathEval.cpp
    ../AfxHookSource2/Filmmaker/Movie/CamPathEval.h
    ../AfxHookSource2/Filmmaker/Movie/CamPathTimingMath.h
    ../AfxHookSource2/Filmmaker/Movie/CamPlayback.cpp
    ../AfxHookSource2/Filmmaker/Movie/CamPlayback.h
    ../AfxHookSource2/Filmmaker/Movie/CameraBridge.h
    ../AfxHookSource2/Filmmaker/Movie/FollowCamera.cpp
    ../AfxHookSource2/Filmmaker/Movie/FollowCamera.h
    ../AfxHookSource2/Filmmaker/Movie/FollowCameraHost.h
    ../AfxHookSource2/Filmmaker/Movie/FollowCameraMath.cpp
    ../AfxHookSource2/Filmmaker/Movie/FollowCameraMath.h
    ../AfxHookSource2/Filmmaker/Movie/FollowCameraStateJson.cpp
    ../AfxHookSource2/Filmmaker/Movie/FollowEventIndex.cpp
    ../AfxHookSource2/Filmmaker/Movie/FollowEventIndex.h
    ../AfxHookSource2/Filmmaker/Movie/FollowTargetDiscovery.cpp
    ../AfxHookSource2/Filmmaker/Movie/FollowTargetDiscovery.h
    ../AfxHookSource2/Filmmaker/Movie/GraphCurveMath.h
    ../AfxHookSource2/Filmmaker/Movie/GraphExpModel.cpp
    ../AfxHookSource2/Filmmaker/Movie/GraphExpModel.h
    ../AfxHookSource2/Filmmaker/Movie/ScrubFrameCache.h
    ../AfxHookSource2/Filmmaker/Movie/SmoothView.cpp
    ../AfxHookSource2/Filmmaker/Movie/SmoothView.h
    ../AfxHookSource2/Filmmaker/Movie/SmoothViewAirMotionMath.h
    ../AfxHookSource2/Filmmaker/Movie/SmoothViewHost.h
    ../AfxHookSource2/Filmmaker/Movie/SmoothViewTiltMath.h
    Gui.cpp
    Gui.h
    hlaeFolder.cpp
    hlaeFolder.h
    main.cpp
    MaterialSystemHooks.cpp
    MaterialSystemHooks.h
    MirvCam.cpp
    MirvCam.h
    MirvCommands.cpp
    MirvTime.cpp
    MirvTime.h
    MirvQueueCmd.cpp
    MirvQueueCmd.h
    RenderView.cpp
    RenderView.h
    ReShadeAdvancedfx.cpp
    ReShadeAdvancedfx.h
    stdafx.h
    WrpConsole.cpp
    WrpConsole.h
    WrpGlobals.cpp
    WrpGlobals.h
    WrpVEngineClient.cpp
    WrpVEngineClient.h

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

    ../Deps/release/prop/AfxHookSource/bm/sdk_src/public/appframework/IAppSystem.h
    ../Deps/release/prop/AfxHookSource/bm/sdk_src/public/interfaces/interfaces.h
    ../Deps/release/prop/AfxHookSource/bm/sdk_src/public/tier0/memalloc.h
    ../Deps/release/prop/AfxHookSource/bm/sdk_src/public/tier1/convar.cpp
    ../Deps/release/prop/AfxHookSource/bm/sdk_src/public/tier1/convar.h
    ../Deps/release/prop/AfxHookSource/bm/sdk_src/public/tier1/iconvar.h
    ../Deps/release/prop/AfxHookSource/bm/sdk_src/public/cdll_int.h
    ../Deps/release/prop/AfxHookSource/bm/sdk_src/public/icvar.h

    ../Deps/release/prop/AfxHookSource/csgo/bitbuf/demofilebitbuf.cpp
    ../Deps/release/prop/AfxHookSource/csgo/bitbuf/demofilebitbuf.h
    ../Deps/release/prop/AfxHookSource/csgo/sdk_src/public/appframework/IAppSystem.h
    ../Deps/release/prop/AfxHookSource/csgo/sdk_src/public/interfaces/interfaces.h
    ../Deps/release/prop/AfxHookSource/csgo/sdk_src/public/tier0/memalloc.h
    ../Deps/release/prop/AfxHookSource/csgo/sdk_src/public/tier1/convar.cpp
    ../Deps/release/prop/AfxHookSource/csgo/sdk_src/public/tier1/convar.h
    ../Deps/release/prop/AfxHookSource/csgo/sdk_src/public/tier1/iconvar.h
    ../Deps/release/prop/AfxHookSource/csgo/sdk_src/public/icvar.h

    ../Deps/release/prop/AfxHookSource/css/sdk_src/public/engine/ishadowmgr.h
    ../Deps/release/prop/AfxHookSource/css/sdk_src/public/tier1/KeyValues.h
    ../Deps/release/prop/AfxHookSource/css/sdk_src/public/toolframework/itoolentity.h
    ../Deps/release/prop/AfxHookSource/css/sdk_src/public/toolframework/itoolframework.h
    ../Deps/release/prop/AfxHookSource/css/sdk_src/public/vstdlib/IKeyValuesSystem.h
    ../Deps/release/prop/AfxHookSource/css/sdk_src/public/basehandle.h
    ../Deps/release/prop/AfxHookSource/css/sdk_src/public/const.h
    ../Deps/release/prop/AfxHookSource/css/sdk_src/public/iclientrenderable.h
    ../Deps/release/prop/AfxHookSource/css/sdk_src/public/ihandleentity.h
    ../Deps/release/prop/AfxHookSource/css/sdk_src/tier1/KeyValues.cpp

    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/engine/ishadowmgr.h
    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/tier1/KeyValues.h
    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/toolframework/itoolentity.h
    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/toolframework/itoolframework.h
    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/tools/bonelist.cpp
    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/tools/bonelist.h
    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/vstdlib/ikeyvaluessystem.h
    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/basehandle.h
    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/const.h
    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/iclientrenderable.h
    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/ihandleentity.h
    ../Deps/release/prop/AfxHookSource/cssV34/sdk_src/tier1/KeyValues.cpp

    ../Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/engine/ishadowmgr.h
    ../Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/tier1/KeyValues.h
    ../Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/toolframework/itoolentity.h
    ../Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/toolframework/itoolframework.h
    ../Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/vstdlib/IKeyValuesSystem.h
    ../Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/basehandle.h
    ../Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/const.h
    ../Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/iclientrenderable.h
    ../Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/ihandleentity.h
    ../Deps/release/prop/AfxHookSource/garrysmod/sdk_src/tier1/KeyValues.cpp

    ../Deps/release/prop/AfxHookSource/insurgency2/public/cdll_int.h

    ../Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/appframework/IAppSystem.h
    ../Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/interfaces/interfaces.h
    ../Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/tier0/memalloc.h
    ../Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/tier1/convar.cpp
    ../Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/tier1/convar.h
    ../Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/tier1/iconvar.h
    ../Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/icvar.h

    ../Deps/release/prop/AfxHookSource/swarm/sdk_src/public/appframework/IAppSystem.h
    ../Deps/release/prop/AfxHookSource/swarm/sdk_src/public/interfaces/interfaces.h
    ../Deps/release/prop/AfxHookSource/swarm/sdk_src/public/tier0/memalloc.h
    ../Deps/release/prop/AfxHookSource/swarm/sdk_src/public/tier1/convar.cpp
    ../Deps/release/prop/AfxHookSource/swarm/sdk_src/public/tier1/convar.h
    ../Deps/release/prop/AfxHookSource/swarm/sdk_src/public/tier1/iconvar.h
    ../Deps/release/prop/AfxHookSource/swarm/sdk_src/public/icvar.h

    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/public/engine/ishadowmgr.h
    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/public/tier1/KeyValues.h
    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/public/toolframework/itoolentity.h
    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/public/toolframework/itoolframework.h
    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/public/tools/bonelist.cpp
    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/public/tools/bonelist.h
    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/public/vstdlib/IKeyValuesSystem.h
    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/public/basehandle.h
    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/public/const.h
    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/public/iclientrenderable.h
    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/public/ihandleentity.h
    ../Deps/release/prop/AfxHookSource/tf2/sdk_src/tier1/KeyValues.cpp

    ../Deps/release/prop/AfxHookSource/SourceInterfaces.cpp
    ../Deps/release/prop/AfxHookSource/SourceInterfaces.h
    ../Deps/release/prop/AfxHookSource/SourceSdkShared.cpp
    ../Deps/release/prop/AfxHookSource/SourceSdkShared.h

    ../Deps/release/rapidxml/rapidxml.hpp
    ../Deps/release/rapidxml/rapidxml_iterators.hpp
    ../Deps/release/rapidxml/rapidxml_print.hpp
    ../Deps/release/rapidxml/rapidxml_utils.hpp

    ../Shared/hooks/gameOverlayRenderer.cpp
    ../Shared/hooks/gameOverlayRenderer.h
    ../Shared/vcpp/AfxAddr.cpp
    ../Shared/vcpp/AfxAddr.h
    ../Shared/AfxCommandLine.cpp
    ../Shared/AfxCommandLine.h
    ../Shared/AfxConsole.cpp
    ../Shared/AfxConsole.h
    ../Shared/AfxDetours.cpp
    ../Shared/AfxDetours.h
    ../Shared/AfxGameRecord.cpp
    ../Shared/AfxGameRecord.h
    ../Shared/AfxMath.cpp
    ../Shared/AfxMath.h
    ../Shared/AfxOutStreams.cpp
    ../Shared/AfxOutStreams.h
    ../Shared/binutils.cpp
    ../Shared/binutils.h
    ../Shared/bvhexport.cpp
    ../Shared/bvhexport.h
    ../Shared/bvhimport.cpp
    ../Shared/bvhimport.h
    ../Shared/CamIO.cpp
    ../Shared/CamIO.h
    ../Shared/CamPath.cpp
    ../Shared/CamPath.h
    ../Shared/CommandSystem.cpp
    ../Shared/CommandSystem.h
    ../Shared/EasySampler.cpp
    ../Shared/EasySampler.h
    ../Shared/FileTools.cpp
    ../Shared/FileTools.h
    ../Shared/FovScaling.cpp
    ../Shared/FovScaling.h
    ../Shared/GrowingBufferPoolThreadSafe.h
    ../Shared/ImageTransformer.cpp
    ../Shared/ImageTransformer.h
    ../Shared/MirvCamIO.cpp
    ../Shared/MirvCamIO.h
    ../Shared/MirvCampath.cpp
    ../Shared/MirvCampath.h
    ../Shared/MirvInput.cpp
    ../Shared/MirvInput.h
    ../Shared/NetconServer.cpp
    ../Shared/NetconServer.h
	../Shared/MirvSkip.cpp
	../Shared/MirvSkip.h
    ../Shared/OpenExrOutput.cpp
    ../Shared/OpenExrOutput.h
    ../Shared/OutVideoStreamCreators.h
    ../Shared/RawOutput.cpp
    ../Shared/RawOutput.h
    ../Shared/RecordingSettings.cpp
    ../Shared/RecordingSettings.h
    ../Shared/RefCounted.h
    ../Shared/RefCountedThreadSafe.h
    ../Shared/StringTools.cpp
    ../Shared/StringTools.h
    ../Shared/ThreadPool.h
    ../Shared/TRefCounted.h
)

# This is far form proper, but it will do for now:
source_group("AfxHookSource" REGULAR_EXPRESSION "/AfxHookSource/")
source_group("AfxHookSource/Filmmaker" REGULAR_EXPRESSION "/AfxHookSource/Filmmaker/")
source_group("AfxHookSource/Filmmaker/CSS" REGULAR_EXPRESSION "/AfxHookSource/Filmmaker/CSS/")
source_group("AfxHookSource/Filmmaker/DemoConvert" REGULAR_EXPRESSION "/AfxHookSource/Filmmaker/DemoConvert/")
source_group("AfxHookSource/Filmmaker/Ui" REGULAR_EXPRESSION "/AfxHookSource/Filmmaker/Ui/")
source_group("Deps/release/imgui" REGULAR_EXPRESSION "/Deps/release/imgui/")
source_group("AfxHookSource2/Filmmaker" REGULAR_EXPRESSION "/AfxHookSource2/Filmmaker/")
source_group("AfxHookSource/tf2" REGULAR_EXPRESSION "/AfxHookSource/tf2/")
source_group("deps" REGULAR_EXPRESSION "/Deps/")
source_group("Deps/release" REGULAR_EXPRESSION "/Deps/release/")
source_group("Deps/release/Detours" REGULAR_EXPRESSION "/Deps/release/Detours/")
source_group("Deps/release/Detours/src" REGULAR_EXPRESSION "/Deps/release/Detours/src/")
source_group("Deps/release/prop" REGULAR_EXPRESSION "/Deps/release/prop/")
source_group("Deps/release/prop/AfxHookSource" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/")
source_group("Deps/release/prop/AfxHookSource/bm" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/bm/")
source_group("Deps/release/prop/AfxHookSource/bm/sdk_src" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/bm/sdk_src/")
source_group("Deps/release/prop/AfxHookSource/bm/sdk_src/public" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/bm/sdk_src/public/")
source_group("Deps/release/prop/AfxHookSource/bm/sdk_src/public/appframework" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/bm/sdk_src/public/appframework/")
source_group("Deps/release/prop/AfxHookSource/bm/sdk_src/public/interfaces" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/bm/sdk_src/public/interfaces/")
source_group("Deps/release/prop/AfxHookSource/bm/sdk_src/public/tier0" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/bm/sdk_src/public/tier0/")
source_group("Deps/release/prop/AfxHookSource/bm/sdk_src/public/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/bm/sdk_src/public/tier1/")
source_group("Deps/release/prop/AfxHookSource/csgo" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/csgo/")
source_group("Deps/release/prop/AfxHookSource/csgo/bitbuf" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/csgo/bitbuf/")
source_group("Deps/release/prop/AfxHookSource/csgo/sdk_src" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/csgo/sdk_src/")
source_group("Deps/release/prop/AfxHookSource/csgo/sdk_src/public" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/csgo/sdk_src/public/")
source_group("Deps/release/prop/AfxHookSource/csgo/sdk_src/public/appframework" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/csgo/sdk_src/public/appframework/")
source_group("Deps/release/prop/AfxHookSource/csgo/sdk_src/public/interfaces" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/csgo/sdk_src/public/interfaces/")
source_group("Deps/release/prop/AfxHookSource/csgo/sdk_src/public/tier0" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/csgo/sdk_src/public/tier0/")
source_group("Deps/release/prop/AfxHookSource/csgo/sdk_src/public/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/csgo/sdk_src/public/tier1/")
source_group("Deps/release/prop/AfxHookSource/css" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/css/")
source_group("Deps/release/prop/AfxHookSource/css/sdk_src" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/css/sdk_src/")
source_group("Deps/release/prop/AfxHookSource/css/sdk_src/public" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/css/sdk_src/public/")
source_group("Deps/release/prop/AfxHookSource/css/sdk_src/public/engine" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/css/sdk_src/public/engine/")
source_group("Deps/release/prop/AfxHookSource/css/sdk_src/public/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/css/sdk_src/public/tier1/")
source_group("Deps/release/prop/AfxHookSource/css/sdk_src/public/toolframework" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/css/sdk_src/public/toolframework/")
source_group("Deps/release/prop/AfxHookSource/css/sdk_src/public/vstdlib" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/css/sdk_src/public/vstdlib/")
source_group("Deps/release/prop/AfxHookSource/css/sdk_src/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/css/sdk_src/tier1/")
source_group("Deps/release/prop/AfxHookSource/cssV34" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/cssV34/")
source_group("Deps/release/prop/AfxHookSource/cssV34/sdk_src" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/cssV34/sdk_src/")
source_group("Deps/release/prop/AfxHookSource/cssV34/sdk_src/public" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/")
source_group("Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/engine" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/engine/")
source_group("Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/tier1/")
source_group("Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/toolframework" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/toolframework/")
source_group("Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/tools" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/tools/")
source_group("Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/vstdlib" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/cssV34/sdk_src/public/vstdlib/")
source_group("Deps/release/prop/AfxHookSource/cssV34/sdk_src/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/cssV34/sdk_src/tier1/")
source_group("Deps/release/prop/AfxHookSource/garrysmod" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/garrysmod/")
source_group("Deps/release/prop/AfxHookSource/garrysmod/sdk_src" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/garrysmod/sdk_src/")
source_group("Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/")
source_group("Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/engine" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/engine/")
source_group("Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/tier1/")
source_group("Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/toolframework" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/toolframework/")
source_group("Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/vstdlib" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/garrysmod/sdk_src/public/vstdlib/")
source_group("Deps/release/prop/AfxHookSource/garrysmod/sdk_src/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/garrysmod/sdk_src/tier1/")
source_group("Deps/release/prop/AfxHookSource/insurgency2" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/insurgency2/")
source_group("Deps/release/prop/AfxHookSource/insurgency2/sdk_src" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/insurgency2/sdk_src/")
source_group("Deps/release/prop/AfxHookSource/insurgency2/sdk_src/public" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/insurgency2/sdk_src/public/")
source_group("Deps/release/prop/AfxHookSource/l4d2" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/l4d2/")
source_group("Deps/release/prop/AfxHookSource/l4d2/sdk_src" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/l4d2/sdk_src/")
source_group("Deps/release/prop/AfxHookSource/l4d2/sdk_src/public" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/")
source_group("Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/appframework" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/appframework/")
source_group("Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/interfaces" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/interfaces/")
source_group("Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/tier0" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/tier0/")
source_group("Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/l4d2/sdk_src/public/tier1/")
source_group("Deps/release/prop/AfxHookSource/swarm" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/swarm/")
source_group("Deps/release/prop/AfxHookSource/swarm/sdk_src" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/swarm/sdk_src/")
source_group("Deps/release/prop/AfxHookSource/swarm/sdk_src/public" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/swarm/sdk_src/public/")
source_group("Deps/release/prop/AfxHookSource/swarm/sdk_src/public/appframework" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/swarm/sdk_src/public/appframework/")
source_group("Deps/release/prop/AfxHookSource/swarm/sdk_src/public/interfaces" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/swarm/sdk_src/public/interfaces/")
source_group("Deps/release/prop/AfxHookSource/swarm/sdk_src/public/tier0" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/swarm/sdk_src/public/tier0/")
source_group("Deps/release/prop/AfxHookSource/swarm/sdk_src/public/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/swarm/sdk_src/public/tier1/")
source_group("Deps/release/prop/AfxHookSource/tf2/" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/tf2/")
source_group("Deps/release/prop/AfxHookSource/tf2/sdk_src" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/tf2/sdk_src/")
source_group("Deps/release/prop/AfxHookSource/tf2/sdk_src/public" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/tf2/sdk_src/public/")
source_group("Deps/release/prop/AfxHookSource/tf2/sdk_src/public/engine" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/tf2/sdk_src/public/engine/")
source_group("Deps/release/prop/AfxHookSource/tf2/sdk_src/public/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/tf2/sdk_src/public/tier1/")
source_group("Deps/release/prop/AfxHookSource/tf2/sdk_src/public/toolframework" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/tf2/sdk_src/public/toolframework/")
source_group("Deps/release/prop/AfxHookSource/tf2/sdk_src/public/tools" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/tf2/sdk_src/public/tools/")
source_group("Deps/release/prop/AfxHookSource/tf2/sdk_src/public/vstdlib" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/tf2/sdk_src/public/vstdlib/")
source_group("Deps/release/prop/AfxHookSource/tf2/sdk_src/tier1" REGULAR_EXPRESSION "/Deps/release/prop/AfxHookSource/tf2/sdk_src/public/tier1/")
source_group("Deps/release/rapidxml" REGULAR_EXPRESSION "/Deps/release/rapidxml/")
source_group("shared" REGULAR_EXPRESSION "/Shared/")
source_group("Shared/hooks" REGULAR_EXPRESSION "/Shared/hooks/")
source_group("Shared/vcpp" REGULAR_EXPRESSION "/Shared/vcpp/")

if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
    add_custom_command(
	    WORKING_DIRECTORY ${AfxHookSource_BINARY_DIR}
	    OUTPUT "${AfxHookSource_BINARY_DIR}/afx_iface_passthrough_x64.obj"
	    COMMAND ${nasm_SOURCE_DIR}/nasm.exe -f win64 -o "${AfxHookSource_BINARY_DIR}/afx_iface_passthrough_x64.obj" "${AfxHookSource_SOURCE_DIR}/afx_iface_passthrough_x64.asm"
	    DEPENDS "${nasm_SOURCE_DIR}/nasm.exe"
            "${AfxHookSource_SOURCE_DIR}/afx_iface_passthrough_x64.asm"
        VERBATIM
    )

    add_custom_target(AfxHookSourceIfacePassThrough
        DEPENDS "${AfxHookSource_BINARY_DIR}/afx_iface_passthrough_x64.obj"
    )

    add_dependencies(AfxHookSource
        AfxHookSourceIfacePassThrough
    )

    target_link_libraries(AfxHookSource PRIVATE
       "${AfxHookSource_BINARY_DIR}/afx_iface_passthrough_x64.obj"
    )

    # The hook loads these from <bin>/resources at runtime (AfxShaders.cpp, CampathDrawer.cpp), so
    # the x64-source1 stage carries them next to the DLL, the way AfxHookSource2 stages its own.
    install(
        FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_line_vs20.acs"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_line_ps20.acs"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Shaders/build/afx_drawtexture_ps20.acs"
        DESTINATION "bin/resources/shaders"
        COMPONENT x64-source1
    )
    install(
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/../Resources/resources/hexfont.tga"
        DESTINATION "bin/resources"
        COMPONENT x64-source1
    )
    install(
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/../Deps/release/imgui/LICENSE.txt"
        DESTINATION "bin/resources/LICENSES/DearImGui"
        COMPONENT x64-source1
    )
endif()

if(CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
    target_include_directories(AfxHookSource PRIVATE
        "${protobuf_SOURCE_DIR}/src"
        AfxHookSourceProtobufs
    )

    target_link_directories(AfxHookSource PRIVATE
        AfxHookSourceProtobufs
    )

    target_link_libraries(AfxHookSource PRIVATE
        AfxHookSourceProtobufs
    )

    target_sources(AfxHookSource PRIVATE
        csgo/hooks/client/csgo_spectatortools.cpp
        csgo/AfxInterfaces.h
        csgo/AfxMaterials.cpp
        csgo/AfxMaterials.h
        csgo/ClientToolsCsgo.cpp
        csgo/ClientToolsCsgo.h
        csgo/Panorama.cpp
        csgo/Panorama.h
        
        css/ClientToolsCss.cpp
        css/ClientToolsCss.h
        
        cssV34/ClientToolsCssV34.cpp
        cssV34/ClientToolsCssV34.h

        garrysmod/ClientToolsGarrysmod.cpp
        garrysmod/ClientToolsGarrysmod.h
        
        momentum/ClientToolsMom.cpp
        momentum/ClientToolsMom.h

        AfxInterop.cpp
        AfxInterop.h
        aiming.cpp
        aiming.h
        asmClassTools.h
        csgo_Audio.cpp
        csgo_Audio.h
        csgo_c_baseanimatingoverlay.cpp
        csgo_c_baseanimatingoverlay.h
        csgo_CBasePlayer.cpp
        csgo_CBasePlayer.h
        csgo_CCSGameMovement.cpp
        csgo_CCSGameMovement.h
        csgo_CHudDeathNotice.cpp
        csgo_CHudDeathNotice.h
        csgo_CRendering3dView.cpp
        csgo_CRendering3dView.h
        csgo_CSkyBoxView.cpp
        csgo_CSkyBoxView.h
        csgo_CViewRender.cpp
        csgo_CViewRender.h
        csgo_GameEvents.cpp
        csgo_GameEvents.h
        csgo_models_replace.cpp
        csgo_models_replace.h
        csgo_net_chan.cpp
        csgo_net_chan.h
        csgo_S_StartSound.cpp
        csgo_S_StartSound.h
        csgo_SndMixTimeScalePatch.cpp
        csgo_SndMixTimeScalePatch.h
        csgo_Stdshader_dx9_Hooks.cpp
        csgo_Stdshader_dx9_Hooks.h
        csgo_view.cpp
        csgo_view.h
        csgo_vphysics.cpp
        csgo_vphysics.h
        MatRenderContextHook.cpp
        MatRenderContextHook.h
        mirv_voice.cpp
        mirv_voice.h
        MirvCalcs.cpp
        MirvCalcs.h
        MirvPgl.cpp
        MirvPgl.h
        MirvWav.cpp
        MirvWav.h

        ../Shared/AfxColorLut.cpp
        ../Shared/AfxColorLut.h

    	../Shared/MirvDeathMsgFilter.cpp
	    ../Shared/MirvDeathMsgFilter.h
    )

    source_group("AfxHookSource/csgo" REGULAR_EXPRESSION "/AfxHookSource/csgo/")
    source_group("AfxHookSource/csgo/hooks" REGULAR_EXPRESSION "/AfxHookSource/csgo/hooks/")
    source_group("AfxHookSource/csgo/hooks/client" REGULAR_EXPRESSION "/AfxHookSource/csgo/hooks/client/")
    source_group("AfxHookSource/csgo/hooks/engine" REGULAR_EXPRESSION "/AfxHookSource/csgo/hooks/engine/")
    source_group("AfxHookSource/css" REGULAR_EXPRESSION "/AfxHookSource/css/")
    source_group("AfxHookSource/cssV34" REGULAR_EXPRESSION "/AfxHookSource/cssV34/")
    source_group("AfxHookSource/garrysmod" REGULAR_EXPRESSION "/AfxHookSource/garrysmod/")
    source_group("AfxHookSource/momentum" REGULAR_EXPRESSION "/AfxHookSource/momentum/")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../CMake/Source1Tests.cmake")
