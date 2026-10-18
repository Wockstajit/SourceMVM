# AfxHookDeadlock: the Deadlock hook DLL.
#
# A SEPARATE TARGET from AfxHookSource2, deliberately. The CS2 hook is not modified by any of this
# and cannot regress from it, and a Deadlock bug cannot reach CS2. That also means no shared build
# logic: the reused sources below are NAMED AGAIN here rather than factored into a common library,
# because factoring them out would mean editing the CS2 target to accommodate Deadlock, which is
# the coupling this arrangement exists to avoid.
#
# Sources are listed explicitly, no globs, matching the CS2 target's style.

project(AfxHookDeadlock)

# Entity integration (hero list, follow/attach, player boxes). ON by default; OFF builds the stub so
# a frame-rate A/B or a crash bisect can drop the whole module. At runtime -mvmNoEntities does the same.
option(AFX_DEADLOCK_ENTITIES "Build Deadlock entity integration" ON)
if(AFX_DEADLOCK_ENTITIES)
	set(AFX_DEADLOCK_ENTITIES_SOURCE DeadlockEntities.cpp)
else()
	set(AFX_DEADLOCK_ENTITIES_SOURCE DeadlockEntitiesDisabled.cpp)
endif()

add_library(AfxHookDeadlock MODULE)

set_target_properties(AfxHookDeadlock PROPERTIES PREFIX "" OUTPUT_NAME "AfxHookDeadlock")

target_compile_features(AfxHookDeadlock PRIVATE cxx_std_17)
# Deliberately NOT defining NOMINMAX or WIN32_LEAN_AND_MEAN. The reused Shared/ sources are
# written against the full windows.h -- they use the min/max macros, CommandLineToArgvW from
# shellapi, and u_char from the winsock headers it drags in. The CS2 target does not define them
# either; matching that is what lets those files compile unchanged into both.
target_compile_definitions(AfxHookDeadlock PRIVATE
	GAME_DEADLOCK
	_CRT_SECURE_NO_WARNINGS
)

# /EHa: the guarded engine and Panorama calls use SEH alongside C++ objects. Every slot index and
# vtable displacement in this port starts life as a hypothesis, so a wrong one has to return a
# neutral value rather than take the game down.
target_compile_options(AfxHookDeadlock PRIVATE
	/EHa
	# PDBs in Release too. Every address in this port is a hypothesis; a crash without symbols
	# would be a dead end rather than a diagnosis.
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:${AFX_MSVC_RELEASE_DEBUG_INFO}>
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/ZI>
)
target_link_options(AfxHookDeadlock PRIVATE
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/DEBUG /OPT:REF /OPT:ICF>
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/INCREMENTAL>
)

# `..` puts AfxHookSource2/ on the include path, which is what the reused Shared/*.cpp files
# expect: they open with #include "stdafx.h" and that header lives there.
target_include_directories(AfxHookDeadlock PRIVATE
	../
	./
)

target_link_libraries(AfxHookDeadlock PRIVATE
	d3d11
	dxguid
	dxgi
	ws2_32
)

target_sources(AfxHookDeadlock PRIVATE
	# ---- Deadlock game layer ----------------------------------------------------------------
	DeadlockAddresses.cpp
	DeadlockAddresses.h
	DeadlockCameraRuntime.cpp
	DeadlockCameraRuntime.h
	DeadlockCommands.cpp
	DeadlockDebug.cpp
	DeadlockInput.cpp
	DeadlockInput.h
	DeadlockHotkeys.cpp
	DeadlockHotkeys.h
	DeadlockHudBlackout.cpp
	DeadlockHudBlackout.h
	DeadlockMouseWheel.cpp
	DeadlockMouseWheel.h
	DeadlockMirvCommands.cpp
	DeadlockCameraCommands.cpp
	DeadlockCameraCommands.h
	DeadlockCameraCommandMath.h
	DeadlockIcvarSlots.h
	DeadlockDemo.cpp
	DeadlockDemo.h
	${AFX_DEADLOCK_ENTITIES_SOURCE}
	DeadlockEntities.h
	DeadlockEngine.cpp
	DeadlockEngine.h
	DeadlockFrame.cpp
	DeadlockGlue.cpp
	DeadlockFrame.h
	DeadlockMain.cpp
	DeadlockNetcon.cpp
	DeadlockNetcon.h
	DeadlockPanorama.cpp
	DeadlockPlaybackSpeed.cpp
	DeadlockPlaybackSpeed.h
	DeadlockPlaybackSpeedMath.h
	DeadlockPanorama.h
	DeadlockPresentHook.cpp
	DeadlockCursorClip.cpp
	DeadlockCursorClip.h
	DeadlockPreUiHook.cpp
	DeadlockPreUiHook.h
	DeadlockViewportBlit.cpp
	DeadlockViewportBlit.h
	DeadlockBlitShaders.h
	DeadlockOverlayDrawer.cpp
	DeadlockOverlayDrawer.h
	DeadlockOverlayFont.h
	DeadlockOverlayMath.h
	DeadlockOverlayShaders.h
	DeadlockPresentHook.h
	DeadlockSchema.cpp
	DeadlockSchema.h
	DeadlockUi.cpp
	DeadlockUi.h
	DeadlockUiCommandBridge.cpp
	DeadlockUiCommandBridge.h
	DeadlockViewSetup.cpp
	DeadlockViewSetup.h

	# ---- Deadlock filmmaker layer -----------------------------------------------------------
	../Filmmaker/Deadlock/DeadlockCameraBodyJs.h
	../Filmmaker/Deadlock/DeadlockConfigRenderBodyJs.h
	../Filmmaker/Deadlock/DeadlockDemoClockMath.h
	../Filmmaker/Deadlock/DeadlockGraph.cpp
	../Filmmaker/Deadlock/DeadlockGraph.h
	../Filmmaker/Deadlock/DeadlockGraphBodyJs.h
	../Filmmaker/Deadlock/DeadlockControlsJs.h
	../Filmmaker/Deadlock/DeadlockMovieHud.cpp
	../Filmmaker/Deadlock/DeadlockMovieHud.h
	../Filmmaker/Deadlock/DeadlockMovieHudJs.h
	../Filmmaker/Deadlock/DeadlockDrawJs.h
	../Filmmaker/Deadlock/DeadlockSpeedDialJs.h
	../Filmmaker/Deadlock/DeadlockRegularTimelineJs.h
	../Filmmaker/Deadlock/DeadlockDock.cpp
	../Filmmaker/Deadlock/DeadlockDock.h
	../Filmmaker/Deadlock/DeadlockDockRibbonJs.h
	../Filmmaker/Deadlock/DeadlockNativeHudJs.h
	../Filmmaker/Deadlock/DeadlockPlaybackLoop.cpp
	../Filmmaker/Deadlock/DeadlockPlaybackLoop.h
	../Filmmaker/Deadlock/DeadlockPathShake.cpp
	../Filmmaker/Deadlock/DeadlockPathShake.h
	../Filmmaker/Deadlock/DeadlockFollowCamera.cpp
	../Filmmaker/Deadlock/DeadlockFollowCamera.h
	../Filmmaker/Deadlock/DeadlockRenderEncoder.cpp
	../Filmmaker/Deadlock/DeadlockRender.cpp
	../Filmmaker/Render/RenderEncoding.cpp
	../Filmmaker/Deadlock/DeadlockRender.h
	../Filmmaker/Deadlock/DeadlockSettings.cpp
	../Filmmaker/Deadlock/DeadlockSettings.h
	../Filmmaker/Deadlock/DeadlockShot.cpp
	../Filmmaker/Deadlock/DeadlockShot.h
	../Filmmaker/Deadlock/DeadlockStyleJs.h
	../Filmmaker/Deadlock/DeadlockTimelineJs.h
	../Filmmaker/Deadlock/DeadlockWorkspaceJs.h
	../Filmmaker/Deadlock/DeadlockWorkspaceLayoutMath.h
	../Filmmaker/Deadlock/DeadlockWorkspacePreview.cpp
	../Filmmaker/Deadlock/DeadlockWorkspacePreview.h
	../Filmmaker/Deadlock/DeadlockWorkspaces.cpp
	../Filmmaker/Deadlock/DeadlockWorkspaces.h

	# ---- Reused from the CS2 tree, unchanged ------------------------------------------------
	# Measured to contain zero references to the CS2 SDK, the entity system, the schema system or
	# DeathMsg.h -- which is why they can be compiled into both targets rather than forked. The
	# Panorama pair reaches DeathMsg.h for declarations only; DeadlockPanorama.cpp defines them.
	../ConsoleCapture.cpp
	../Globals.cpp
	../WrpConsole.cpp
	../Filmmaker/Debug/MvmDebugLog.cpp
	../Filmmaker/MouseCapture.cpp
	../Filmmaker/Movie/CamMarkers.cpp
	../Filmmaker/Movie/CamPathEval.cpp
	../Filmmaker/Movie/FollowCameraMath.cpp
	../Filmmaker/Movie/CameraShakeTypes.h
	../Filmmaker/Movie/RageShake.cpp
	../Filmmaker/Movie/RageShake.h
	../Filmmaker/Movie/RageShakeStyles.cpp
	../Filmmaker/Movie/GraphExpModel.cpp
	../Filmmaker/Panorama/PanoramaBridge.cpp
	../Filmmaker/Panorama/PanoramaFindPanel.cpp
	# Shared UNCHANGED, which is the whole point of the accessor contract in DeadlockPanorama.cpp:
	# UiLiveReload reaches Panorama only through the four DeathMsg.h accessors, and Deadlock's
	# GetPanoramaMainMenuPanel returns null so its AUTO context falls straight through to the HUD.
	../Filmmaker/Panorama/UiLiveReload.cpp
	../Filmmaker/Platform/FolderPicker.cpp
	../Filmmaker/Platform/JsonBuilder.cpp
	../Filmmaker/Platform/JsonParser.cpp
	../Filmmaker/Platform/TextEncoding.cpp
	../Filmmaker/Demo/DemoProjectPath.cpp
	../Filmmaker/Demo/DemoProjectPath.h
	../Filmmaker/Demo/DemoProjectStore.cpp
	../Filmmaker/Demo/DemoProjectStore.h
	../Filmmaker/Demo/DemoProjectStoreMath.h
	../Filmmaker/Demo/DemoProjectFileMath.h

	# ---- Shared/ ----------------------------------------------------------------------------
	../../Shared/AfxCommandLine.cpp
	../../Shared/AfxConsole.cpp
	../../Shared/AfxDetours.cpp
	../../Shared/AfxMath.cpp
	../../Shared/CamPath.cpp
	../../Shared/CamIO.cpp
	../../Shared/MirvCamIO.cpp
	../../Shared/MirvCampath.cpp
	../../Shared/CommandSystem.cpp
	../../Shared/FileTools.cpp
	../../Shared/FovScaling.cpp
	../../Shared/MirvInput.cpp
	../../Shared/MirvSkip.cpp
	../../Shared/StringTools.cpp
	../../Shared/binutils.cpp

	# ---- Detours ----------------------------------------------------------------------------
	../../Deps/release/Detours/src/detours.cpp
	../../Deps/release/Detours/src/modules.cpp
	../../Deps/release/Detours/src/disasm.cpp
	../../Deps/release/Detours/src/image.cpp
	../../Deps/release/Detours/src/creatwth.cpp

	# ---- the Source 2 SDK shim ---------------------------------------------------------------
	# Reuse console storage/helpers, not CS2 virtual layouts. Matching version strings do not
	# establish ABI compatibility; DeadlockConsoleAbi/DeadlockIcvarSlots adapt native registration.
	../../Deps/release/prop/cs2/sdk_src/public/tier1/convar.cpp
	../../Deps/release/prop/cs2/sdk_src/public/tier1/utlmemory.cpp
	../../Deps/release/prop/cs2/sdk_src/public/tier1/utlstring.cpp
	../../Deps/release/prop/cs2/sdk_src/public/tier1/bufferstring.cpp
)

# MODULE targets install through LIBRARY DESTINATION, not RUNTIME -- the same way the CS2 hook is
# staged. The launcher picks the DLL up from bin/x64 of the staging prefix.
install(TARGETS AfxHookDeadlock
	LIBRARY DESTINATION "bin/x64"
	RUNTIME DESTINATION "bin/x64"
	COMPONENT x64-deadlock)
# ---- tests -----------------------------------------------------------------------------------
# The repo rule: anything with real arithmetic lives in a header-only, engine-free *Math.h with a
# matching test, because the hook itself cannot be tested and this can. The demo clock is the whole
# reason -- treating ordinary backwards time jitter as a seek is a mistake that stays invisible
# until every shot-driven effect is quietly resetting itself.
include(CTest)

add_executable(DeadlockDemoClockMathTests ../../Automation/tests/deadlock-demo-clock-math-tests.cpp)
target_compile_features(DeadlockDemoClockMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockDemoClockMathTests COMMAND DeadlockDemoClockMathTests)

add_executable(DeadlockConsoleAbiTests ../../Automation/tests/deadlock-console-abi-tests.cpp)
target_compile_features(DeadlockConsoleAbiTests PRIVATE cxx_std_17)
add_test(NAME DeadlockConsoleAbiTests COMMAND DeadlockConsoleAbiTests)

add_executable(DeadlockCameraCommandMathTests ../../Automation/tests/deadlock-camera-command-math-tests.cpp)
target_compile_features(DeadlockCameraCommandMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockCameraCommandMathTests COMMAND DeadlockCameraCommandMathTests)

install(FILES "$<TARGET_PDB_FILE:AfxHookDeadlock>"
	DESTINATION "bin/x64"
	COMPONENT x64-deadlock
	OPTIONAL)

add_executable(DeadlockViewMathTests ../../Automation/tests/deadlock-view-math-tests.cpp)
target_compile_features(DeadlockViewMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockViewMathTests COMMAND DeadlockViewMathTests)

# The workspace layout. This is the arithmetic that was wrong on screen -- a rail down the left and
# no preview at all -- so it is the arithmetic that gets pinned down offline. Each case in the test
# is a shape the panel has to survive: a first frame with no measurement, a closed dock, a column
# wider than the screen, an ultrawide monitor.
add_executable(DeadlockWorkspaceLayoutMathTests
	../../Automation/tests/deadlock-workspace-layout-math-tests.cpp)
target_compile_features(DeadlockWorkspaceLayoutMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockWorkspaceLayoutMathTests COMMAND DeadlockWorkspaceLayoutMathTests)

# The playback dial. Two continuous sliders and a pair of stepper buttons drive one number, and the
# case that needed pinning down offline is stepping from BETWEEN two presets: snapping to the
# nearest rung first (which is what the command did before the sliders existed) makes one of the
# two buttons do nothing at all.
add_executable(DeadlockPlaybackSpeedMathTests
	../../Automation/tests/deadlock-playback-speed-math-tests.cpp)
target_compile_features(DeadlockPlaybackSpeedMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockPlaybackSpeedMathTests COMMAND DeadlockPlaybackSpeedMathTests)

# Who owns the view and whether a pushed pose is still fresh (DeadlockViewOwnerMath.h).
add_executable(DeadlockViewOwnerMathTests ../../Automation/tests/deadlock-view-owner-math-tests.cpp)
target_compile_features(DeadlockViewOwnerMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockViewOwnerMathTests COMMAND DeadlockViewOwnerMathTests)

# CS2's camera-path rules as Deadlock applies them: the add guard, arm/play/stop, the seek settle,
# the parked hold and the retime clamp (DeadlockShotRulesMath.h).
add_executable(DeadlockShotRulesMathTests ../../Automation/tests/deadlock-shot-rules-math-tests.cpp)
target_compile_features(DeadlockShotRulesMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockShotRulesMathTests COMMAND DeadlockShotRulesMathTests)

# Failed shot saves retain their payload and retry state independently per replay.
add_executable(DeadlockShotSaveMathTests ../../Automation/tests/deadlock-shot-save-math-tests.cpp)
target_compile_features(DeadlockShotSaveMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockShotSaveMathTests COMMAND DeadlockShotSaveMathTests)

# Replay identity changes and graph curve/history isolation across project switches.
add_executable(DeadlockReplaySessionMathTests
	../../Automation/tests/deadlock-replay-session-math-tests.cpp
	../Filmmaker/Movie/GraphExpModel.cpp)
target_compile_features(DeadlockReplaySessionMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockReplaySessionMathTests COMMAND DeadlockReplaySessionMathTests)

# Go to player: the free-camera pose behind and above a hero, looking at them (DeadlockGoToMath.h).
add_executable(DeadlockGoToMathTests ../../Automation/tests/deadlock-goto-math-tests.cpp)
target_compile_features(DeadlockGoToMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockGoToMathTests COMMAND DeadlockGoToMathTests)

# World point -> overlay pixel for the in-world path, markers and hero boxes (DeadlockOverlayMath.h).
add_executable(DeadlockOverlayMathTests ../../Automation/tests/deadlock-overlay-math-tests.cpp)
target_compile_features(DeadlockOverlayMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockOverlayMathTests COMMAND DeadlockOverlayMathTests)

# Entity-handle decoding and the hero roster's display name (DeadlockEntityMath.h) -- P4b.
add_executable(DeadlockEntityMathTests ../../Automation/tests/deadlock-entity-math-tests.cpp)
target_compile_features(DeadlockEntityMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockEntityMathTests COMMAND DeadlockEntityMathTests)

# Follow-camera lock-on (placed position, aim-only) and attach (real-bone ride-along) math, taken
# from CS2's own Movie/FollowCamera.cpp spec (DeadlockFollowSolveMath.h) -- P5.
add_executable(DeadlockFollowSolveMathTests ../../Automation/tests/deadlock-follow-solve-math-tests.cpp)
target_compile_features(DeadlockFollowSolveMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockFollowSolveMathTests COMMAND DeadlockFollowSolveMathTests)

add_executable(DeadlockRenderMathTests ../../Automation/tests/deadlock-render-math-tests.cpp)
target_compile_features(DeadlockRenderMathTests PRIVATE cxx_std_17)
add_test(NAME DeadlockRenderMathTests COMMAND DeadlockRenderMathTests)
