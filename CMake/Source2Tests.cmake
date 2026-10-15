add_executable(EnvironmentShaderCacheTests ../Automation/tests/environment-shader-cache-tests.cpp)
target_compile_features(EnvironmentShaderCacheTests PRIVATE cxx_std_17)
add_test(NAME EnvironmentShaderCacheTests COMMAND EnvironmentShaderCacheTests)

add_executable(FollowCameraMathTests
    ../Automation/tests/follow-camera-math-tests.cpp
    Filmmaker/Movie/FollowCameraMath.cpp
    Filmmaker/Movie/FollowCameraMath.h
)
target_compile_features(FollowCameraMathTests PRIVATE cxx_std_17)
add_test(NAME FollowCameraMathTests COMMAND FollowCameraMathTests)


add_executable(SmoothViewAirMotionMathTests
    ../Automation/tests/smooth-view-air-motion-math-tests.cpp
    Filmmaker/Movie/SmoothViewAirMotionMath.h
)
target_compile_features(SmoothViewAirMotionMathTests PRIVATE cxx_std_17)
add_test(NAME SmoothViewAirMotionMathTests COMMAND SmoothViewAirMotionMathTests)

add_executable(RecoilPivotMarkerMathTests
    ../Automation/tests/recoil-pivot-marker-math-tests.cpp
    Filmmaker/Movie/RecoilPivotMarkerMath.h
)
target_compile_features(RecoilPivotMarkerMathTests PRIVATE cxx_std_17)
add_test(NAME RecoilPivotMarkerMathTests COMMAND RecoilPivotMarkerMathTests)

add_executable(ViewFxWeaponSwayMathTests
    ../Automation/tests/viewfx-weapon-sway-math-tests.cpp
    Filmmaker/Movie/ViewFxWeaponSwayMath.h
)
target_compile_features(ViewFxWeaponSwayMathTests PRIVATE cxx_std_17)
add_test(NAME ViewFxWeaponSwayMathTests COMMAND ViewFxWeaponSwayMathTests)

add_executable(CameraShakePhaseClockMathTests
    ../Automation/tests/camera-shake-phase-clock-math-tests.cpp
    Filmmaker/Movie/CameraShakePhaseClockMath.h
)
target_compile_features(CameraShakePhaseClockMathTests PRIVATE cxx_std_17)
add_test(NAME CameraShakePhaseClockMathTests COMMAND CameraShakePhaseClockMathTests)

add_executable(CameraShakeTrackerMathTests
    ../Automation/tests/camera-shake-tracker-math-tests.cpp
    Filmmaker/Movie/CameraShakeTrackerMath.h
)
target_compile_features(CameraShakeTrackerMathTests PRIVATE cxx_std_17)
add_test(NAME CameraShakeTrackerMathTests COMMAND CameraShakeTrackerMathTests)

add_executable(SmoothViewTiltMathTests
    ../Automation/tests/smoothview-tilt-math-tests.cpp
    Filmmaker/Movie/SmoothViewTiltMath.h
)
target_compile_features(SmoothViewTiltMathTests PRIVATE cxx_std_17)
add_test(NAME SmoothViewTiltMathTests COMMAND SmoothViewTiltMathTests)

add_executable(RagdollWeaponMathTests
    ../Automation/tests/ragdoll-weapon-math-tests.cpp
    Filmmaker/Movie/RagdollWeaponMath.h
)
target_compile_features(RagdollWeaponMathTests PRIVATE cxx_std_17)
add_test(NAME RagdollWeaponMathTests COMMAND RagdollWeaponMathTests)

add_executable(DemoAimResamplerMathTests
    ../Automation/tests/demo-aim-resampler-math-tests.cpp
    Filmmaker/Movie/DemoAimResamplerMath.h
)
target_compile_features(DemoAimResamplerMathTests PRIVATE cxx_std_17)
add_test(NAME DemoAimResamplerMathTests COMMAND DemoAimResamplerMathTests)


add_executable(CameraCenterLockMathTests
    ../Automation/tests/camera-center-lock-math-tests.cpp
    Filmmaker/Movie/CameraCenterLockMath.h
)
target_compile_features(CameraCenterLockMathTests PRIVATE cxx_std_17)
add_test(NAME CameraCenterLockMathTests COMMAND CameraCenterLockMathTests)

add_executable(RageShakeMathTests
    ../Automation/tests/rage-shake-math-tests.cpp
    Filmmaker/Movie/RageShake.cpp
    Filmmaker/Movie/RageShakeStyles.cpp
    Filmmaker/Movie/RageShake.h
)
target_compile_features(RageShakeMathTests PRIVATE cxx_std_17)
add_test(NAME RageShakeMathTests COMMAND RageShakeMathTests)

add_executable(CubicMinKeysMathTests
    ../Automation/tests/cubic-min-keys-math-tests.cpp
    ../Shared/AfxMath.cpp
    ../Shared/AfxMath.h
)
# AfxMath.cpp includes "stdafx.h", which lives next to this list file.
target_include_directories(CubicMinKeysMathTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(CubicMinKeysMathTests PRIVATE cxx_std_17)
add_test(NAME CubicMinKeysMathTests COMMAND CubicMinKeysMathTests)

add_executable(WeaponRecoilScopeTests
    ../Automation/tests/weapon-recoil-scope-tests.cpp
    Filmmaker/Movie/WeaponRecoilScope.h
)
target_compile_features(WeaponRecoilScopeTests PRIVATE cxx_std_17)
add_test(NAME WeaponRecoilScopeTests COMMAND WeaponRecoilScopeTests)

add_executable(WeaponRecoilPresetTests
    ../Automation/tests/weapon-recoil-preset-tests.cpp
    Filmmaker/Movie/WeaponRecoilPresets.h
    Filmmaker/Movie/WeaponRecoilScope.h
    Filmmaker/Movie/WeaponRecoilStyles.h
)
target_compile_features(WeaponRecoilPresetTests PRIVATE cxx_std_17)
add_test(NAME WeaponRecoilPresetTests COMMAND WeaponRecoilPresetTests)

add_executable(WeaponRecoilMathTests
    ../Automation/tests/weapon-recoil-math-tests.cpp
    Filmmaker/Movie/WeaponRecoilMath.h
)
target_compile_features(WeaponRecoilMathTests PRIVATE cxx_std_17)
add_test(NAME WeaponRecoilMathTests COMMAND WeaponRecoilMathTests)

add_executable(AspectRatioMathTests
    ../Automation/tests/aspect-ratio-math-tests.cpp
    Filmmaker/Window/AspectRatioMath.h
)
target_compile_features(AspectRatioMathTests PRIVATE cxx_std_17)
add_test(NAME AspectRatioMathTests COMMAND AspectRatioMathTests)

add_executable(WindowSnapMathTests
    ../Automation/tests/window-snap-math-tests.cpp
    Filmmaker/Window/WindowSnapMath.h
    Filmmaker/Window/AspectRatioMath.h
)
target_compile_features(WindowSnapMathTests PRIVATE cxx_std_17)
add_test(NAME WindowSnapMathTests COMMAND WindowSnapMathTests)

add_executable(MethodActingMathTests
    ../Automation/tests/method-acting-math-tests.cpp
    Filmmaker/Movie/MethodActingMath.h
)
target_compile_features(MethodActingMathTests PRIVATE cxx_std_17)
add_test(NAME MethodActingMathTests COMMAND MethodActingMathTests)

add_executable(WindowPrecisionGuardTests
    ../Automation/tests/window-precision-guard-tests.cpp
    Filmmaker/Window/WindowPrecisionGuard.h
)
target_compile_features(WindowPrecisionGuardTests PRIVATE cxx_std_17)
add_test(NAME WindowPrecisionGuardTests COMMAND WindowPrecisionGuardTests)

add_executable(LightTintMathTests
    ../Automation/tests/light-tint-math-tests.cpp
    Filmmaker/World/LightTintMath.h
)
target_compile_features(LightTintMathTests PRIVATE cxx_std_17)
add_test(NAME LightTintMathTests COMMAND LightTintMathTests)

add_executable(MaterialTintOwnershipTests
    ../Automation/tests/material-tint-ownership-tests.cpp
    Filmmaker/World/MaterialTintOwnership.h
)
target_compile_features(MaterialTintOwnershipTests PRIVATE cxx_std_17)
add_test(NAME MaterialTintOwnershipTests COMMAND MaterialTintOwnershipTests)

add_executable(WorldTintMathTests
    ../Automation/tests/world-tint-math-tests.cpp
    Filmmaker/World/WorldTintMath.h
)
target_compile_features(WorldTintMathTests PRIVATE cxx_std_17)
add_test(NAME WorldTintMathTests COMMAND WorldTintMathTests)

add_executable(WeaponBobMathTests
    ../Automation/tests/weapon-bob-math-tests.cpp
    Filmmaker/Movie/WeaponBobMath.h
)
target_compile_features(WeaponBobMathTests PRIVATE cxx_std_17)
add_test(NAME WeaponBobMathTests COMMAND WeaponBobMathTests)

add_executable(SunRotationMathTests
    ../Automation/tests/sun-rotation-math-tests.cpp
    Filmmaker/World/SunRotationMath.h
)
target_compile_features(SunRotationMathTests PRIVATE cxx_std_17)
add_test(NAME SunRotationMathTests COMMAND SunRotationMathTests)

add_executable(DemoScannerTests
    ../Automation/tests/demo-scanner-tests.cpp
    Filmmaker/Demo/DemoScanner.cpp
    Filmmaker/Demo/DemoScanner.h
)
target_compile_features(DemoScannerTests PRIVATE cxx_std_17)
add_test(NAME DemoScannerTests COMMAND DemoScannerTests)

add_executable(DemoProjectPathTests
    ../Automation/tests/demo-project-path-tests.cpp
    Filmmaker/Demo/DemoProjectPath.cpp
    Filmmaker/Demo/DemoProjectPath.h
)
target_compile_features(DemoProjectPathTests PRIVATE cxx_std_17)
add_test(NAME DemoProjectPathTests COMMAND DemoProjectPathTests)

add_executable(DemoProjectStoreMathTests
    ../Automation/tests/demo-project-store-math-tests.cpp
)
target_compile_features(DemoProjectStoreMathTests PRIVATE cxx_std_17)
add_test(NAME DemoProjectStoreMathTests COMMAND DemoProjectStoreMathTests)

add_executable(DemoProjectFileMathTests
    ../Automation/tests/demo-project-file-math-tests.cpp
    Filmmaker/Demo/DemoProjectFileMath.h
    Filmmaker/Demo/DemoProjectStoreMath.h
)
target_compile_features(DemoProjectFileMathTests PRIVATE cxx_std_17)
add_test(NAME DemoProjectFileMathTests COMMAND DemoProjectFileMathTests)

add_executable(RenderRangeTests
    ../Automation/tests/render-range-tests.cpp
    Filmmaker/Render/RenderRange.cpp
    Filmmaker/Render/RenderRange.h
    Filmmaker/Render/RenderTypes.h
    Filmmaker/Render/RenderSession.cpp
    Filmmaker/Render/RenderSession.h
    Filmmaker/Render/RenderValidation.cpp
    Filmmaker/Render/RenderValidation.h
)
target_compile_features(RenderRangeTests PRIVATE cxx_std_17)
add_test(NAME RenderRangeTests COMMAND RenderRangeTests)

add_executable(RenderCaptureMathTests
    ../Automation/tests/render-capture-math-tests.cpp
)
target_compile_features(RenderCaptureMathTests PRIVATE cxx_std_17)
add_test(NAME RenderCaptureMathTests COMMAND RenderCaptureMathTests)

add_executable(RenderPassPlanTests
    ../Automation/tests/render-pass-plan-tests.cpp
)
target_compile_features(RenderPassPlanTests PRIVATE cxx_std_17)
add_test(NAME RenderPassPlanTests COMMAND RenderPassPlanTests)

add_executable(PropsMathTests
    ../Automation/tests/props-math-tests.cpp
)
target_compile_features(PropsMathTests PRIVATE cxx_std_17)
add_test(NAME PropsMathTests COMMAND PropsMathTests)

add_executable(PropEditMathTests
    ../Automation/tests/prop-edit-math-tests.cpp
)
target_compile_features(PropEditMathTests PRIVATE cxx_std_17)
add_test(NAME PropEditMathTests COMMAND PropEditMathTests)
add_executable(CamPathTimingMathTests
    ../Automation/tests/campath-timing-math-tests.cpp
)
target_compile_features(CamPathTimingMathTests PRIVATE cxx_std_17)
add_test(NAME CamPathTimingMathTests COMMAND CamPathTimingMathTests)

add_executable(BugReportMathTests
    ../Automation/tests/bug-report-math-tests.cpp
)
target_compile_features(BugReportMathTests PRIVATE cxx_std_17)
add_test(NAME BugReportMathTests COMMAND BugReportMathTests)

add_executable(CrashReportMathTests
    ../Automation/tests/crash-report-math-tests.cpp
)
target_compile_features(CrashReportMathTests PRIVATE cxx_std_17)
add_test(NAME CrashReportMathTests COMMAND CrashReportMathTests)

add_executable(HcTrackerFeedMathTests
    ../Automation/tests/hc-tracker-feed-math-tests.cpp
    Filmmaker/Platform/JsonParser.cpp
)
target_compile_features(HcTrackerFeedMathTests PRIVATE cxx_std_17)
add_test(NAME HcTrackerFeedMathTests COMMAND HcTrackerFeedMathTests)

add_executable(DemoScrubMathTests
    ../Automation/tests/demo-scrub-math-tests.cpp
)
target_compile_features(DemoScrubMathTests PRIVATE cxx_std_17)
add_test(NAME DemoScrubMathTests COMMAND DemoScrubMathTests)

add_executable(OsuRealmTests
    ../Automation/tests/osu-realm-tests.cpp
)
target_compile_features(OsuRealmTests PRIVATE cxx_std_17)
add_test(NAME OsuRealmTests COMMAND OsuRealmTests)

add_executable(EasterEggWesMathTests
    ../Automation/tests/easter-egg-wes-math-tests.cpp
)
target_compile_features(EasterEggWesMathTests PRIVATE cxx_std_17)
add_test(NAME EasterEggWesMathTests COMMAND EasterEggWesMathTests)

add_executable(OsuDifficultyTests
    ../Automation/tests/osu-difficulty-tests.cpp
)
target_compile_features(OsuDifficultyTests PRIVATE cxx_std_17)
add_test(NAME OsuDifficultyTests COMMAND OsuDifficultyTests)

add_executable(OsuMathTests
    ../Automation/tests/osu-math-tests.cpp
)
target_compile_features(OsuMathTests PRIVATE cxx_std_17)
add_test(NAME OsuMathTests COMMAND OsuMathTests)

add_executable(EasterEggChickenMathTests
    ../Automation/tests/easter-egg-chicken-math-tests.cpp
)
target_compile_features(EasterEggChickenMathTests PRIVATE cxx_std_17)
add_test(NAME EasterEggChickenMathTests COMMAND EasterEggChickenMathTests)

add_executable(EasterEggHintMathTests
    ../Automation/tests/easter-egg-hint-math-tests.cpp
)
target_compile_features(EasterEggHintMathTests PRIVATE cxx_std_17)
add_test(NAME EasterEggHintMathTests COMMAND EasterEggHintMathTests)

add_executable(EasterEggLightingMathTests
    ../Automation/tests/easter-egg-lighting-math-tests.cpp
    Filmmaker/Platform/JsonParser.cpp
)
target_compile_features(EasterEggLightingMathTests PRIVATE cxx_std_17)
add_test(NAME EasterEggLightingMathTests COMMAND EasterEggLightingMathTests)

add_executable(JsonParserTests
    ../Automation/tests/json-parser-tests.cpp
    Filmmaker/Platform/JsonParser.cpp
)
target_compile_features(JsonParserTests PRIVATE cxx_std_17)
add_test(NAME JsonParserTests COMMAND JsonParserTests)

add_executable(EasterEggMoonMathTests
    ../Automation/tests/easter-egg-moon-math-tests.cpp
)
target_compile_features(EasterEggMoonMathTests PRIVATE cxx_std_17)
add_test(NAME EasterEggMoonMathTests COMMAND EasterEggMoonMathTests)

add_executable(AtomicFileTests ../Automation/tests/atomic-file-tests.cpp)
target_compile_features(AtomicFileTests PRIVATE cxx_std_17)
add_test(NAME AtomicFileTests COMMAND AtomicFileTests)

add_executable(CosmeticThemeMathTests ../Automation/tests/cosmetic-theme-math-tests.cpp)
target_compile_features(CosmeticThemeMathTests PRIVATE cxx_std_17)
add_test(NAME CosmeticThemeMathTests COMMAND CosmeticThemeMathTests)

add_executable(EasterEggAudioMathTests
    ../Automation/tests/easter-egg-audio-math-tests.cpp
)
target_compile_features(EasterEggAudioMathTests PRIVATE cxx_std_17)
add_test(NAME EasterEggAudioMathTests COMMAND EasterEggAudioMathTests)

add_executable(SnakeMathTests
    ../Automation/tests/snake-math-tests.cpp
)
target_compile_features(SnakeMathTests PRIVATE cxx_std_17)
add_test(NAME SnakeMathTests COMMAND SnakeMathTests)

add_executable(UiClickMathTests
    ../Automation/tests/ui-click-math-tests.cpp
)
target_compile_features(UiClickMathTests PRIVATE cxx_std_17)
add_test(NAME UiClickMathTests COMMAND UiClickMathTests)

add_executable(SceneKeyframeMathTests
    ../Automation/tests/scene-keyframe-math-tests.cpp
)
target_compile_features(SceneKeyframeMathTests PRIVATE cxx_std_17)
add_test(NAME SceneKeyframeMathTests COMMAND SceneKeyframeMathTests)

add_executable(RelightProbeMathTests ../Automation/tests/relight-probe-math-tests.cpp)
target_compile_features(RelightProbeMathTests PRIVATE cxx_std_17)
add_test(NAME RelightProbeMathTests COMMAND RelightProbeMathTests)

add_executable(RelightMathTests ../Automation/tests/relight-math-tests.cpp)
target_compile_features(RelightMathTests PRIVATE cxx_std_17)
add_test(NAME RelightMathTests COMMAND RelightMathTests)
add_executable(EnvironmentVfxLightingTests ../Automation/tests/environment-vfx-lighting-tests.cpp)
target_compile_features(EnvironmentVfxLightingTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentVfxLightingTests PRIVATE d3d11)
add_test(NAME EnvironmentVfxLightingTests COMMAND EnvironmentVfxLightingTests)
add_executable(EnvironmentSunVisibilityMathTests ../Automation/tests/environment-sun-visibility-math-tests.cpp)
target_compile_features(EnvironmentSunVisibilityMathTests PRIVATE cxx_std_17)
add_test(NAME EnvironmentSunVisibilityMathTests COMMAND EnvironmentSunVisibilityMathTests)

add_executable(RelightIrradianceTests ../Automation/tests/relight-irradiance-tests.cpp)
target_compile_features(RelightIrradianceTests PRIVATE cxx_std_17)
target_link_libraries(RelightIrradianceTests PRIVATE d3d11 dxguid)
add_test(NAME RelightIrradianceTests COMMAND RelightIrradianceTests)

add_executable(RelightClusterMathTests ../Automation/tests/relight-cluster-math-tests.cpp)
add_executable(EnvironmentConstantPatchTests ../Automation/tests/environment-constant-patch-tests.cpp)
target_compile_features(EnvironmentConstantPatchTests PRIVATE cxx_std_17)
add_test(NAME EnvironmentConstantPatchTests COMMAND EnvironmentConstantPatchTests)
add_executable(EnvironmentNativeAoTests ../Automation/tests/environment-native-ao-tests.cpp)
target_compile_features(EnvironmentNativeAoTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentNativeAoTests PRIVATE d3d11 d3dcompiler)
add_test(NAME EnvironmentNativeAoTests COMMAND EnvironmentNativeAoTests)

add_executable(EnvironmentVolumeCacheTests ../Automation/tests/environment-volume-cache-tests.cpp)
target_compile_features(EnvironmentVolumeCacheTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentVolumeCacheTests PRIVATE d3d11 dxguid)
add_test(NAME EnvironmentVolumeCacheTests COMMAND EnvironmentVolumeCacheTests)
target_compile_features(RelightClusterMathTests PRIVATE cxx_std_17)
add_test(NAME RelightClusterMathTests COMMAND RelightClusterMathTests)

add_executable(RelightClusterGpuTests ../Automation/tests/relight-cluster-gpu-tests.cpp)
target_compile_features(RelightClusterGpuTests PRIVATE cxx_std_17)
target_link_libraries(RelightClusterGpuTests PRIVATE d3d11 dxguid)
add_test(NAME RelightClusterGpuTests COMMAND RelightClusterGpuTests)

add_executable(RelightProbeGpuTests ../Automation/tests/relight-probe-gpu-tests.cpp)
target_sources(RelightProbeGpuTests PRIVATE Filmmaker/Relight/RelightScreenGi.cpp Filmmaker/Relight/RelightProbes.cpp Filmmaker/Relight/RelightProbeGi.cpp Filmmaker/Relight/RelightStainedGlass.cpp Filmmaker/Relight/RelightLightmap.cpp)
target_compile_features(RelightProbeGpuTests PRIVATE cxx_std_17)
target_link_libraries(RelightProbeGpuTests PRIVATE d3d11 dxguid)
add_test(NAME RelightProbeGpuTests COMMAND RelightProbeGpuTests)

add_executable(EnvironmentMathTests
    ../Automation/tests/environment-math-tests.cpp
    Filmmaker/Environment/EnvironmentDocument.cpp
    Filmmaker/Environment/EnvironmentObj.cpp
    Filmmaker/Environment/EnvironmentWorldDocument.cpp
    Filmmaker/Platform/JsonParser.cpp
    Filmmaker/Platform/JsonBuilder.cpp
)
target_compile_features(EnvironmentMathTests PRIVATE cxx_std_17)
add_test(NAME EnvironmentMathTests COMMAND EnvironmentMathTests)
add_executable(AutoExposureMathTests
    ../Automation/tests/auto-exposure-math-tests.cpp
)
target_compile_features(AutoExposureMathTests PRIVATE cxx_std_17)
add_test(NAME AutoExposureMathTests COMMAND AutoExposureMathTests)

add_executable(EnvironmentGltfMathTests
    ../Automation/tests/environment-gltf-math-tests.cpp
    Filmmaker/Platform/JsonParser.cpp
)
target_compile_features(EnvironmentGltfMathTests PRIVATE cxx_std_17)
add_test(NAME EnvironmentGltfMathTests COMMAND EnvironmentGltfMathTests)

add_executable(EnvironmentRenderTests
    ../Automation/tests/environment-render-tests.cpp
    Filmmaker/Environment/EnvironmentRender.cpp
    Filmmaker/Environment/EnvironmentShadowMaps.cpp
    Filmmaker/Environment/EnvironmentFoliage.cpp
    Filmmaker/Environment/EnvironmentViewmodel.cpp
    Filmmaker/Environment/EnvironmentAnimatedCasters.cpp Filmmaker/Environment/EnvironmentParticleShadows.cpp
    Filmmaker/Environment/EnvironmentGi.cpp
    Filmmaker/Environment/EnvironmentDocument.cpp
    Filmmaker/Environment/EnvironmentObj.cpp
    Filmmaker/Environment/EnvironmentWorldDocument.cpp
    Filmmaker/Platform/JsonParser.cpp
    Filmmaker/Platform/JsonBuilder.cpp
)
target_compile_features(EnvironmentRenderTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentRenderTests PRIVATE d3d11 dxguid)
add_test(NAME EnvironmentRenderTests COMMAND EnvironmentRenderTests)
add_executable(EnvironmentFoliageTests ../Automation/tests/environment-foliage-tests.cpp Filmmaker/Environment/EnvironmentFoliage.cpp)
target_compile_features(EnvironmentFoliageTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentFoliageTests PRIVATE d3d11)
add_test(NAME EnvironmentFoliageTests COMMAND EnvironmentFoliageTests)
add_executable(EnvironmentReceiverTests ../Automation/tests/environment-receiver-tests.cpp Filmmaker/Environment/EnvironmentViewmodel.cpp)
target_compile_features(EnvironmentReceiverTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentReceiverTests PRIVATE d3d11)
add_test(NAME EnvironmentReceiverTests COMMAND EnvironmentReceiverTests)
add_executable(EnvironmentAnimatedCasterTests ../Automation/tests/environment-animated-caster-tests.cpp Filmmaker/Environment/EnvironmentAnimatedCasters.cpp Filmmaker/Environment/EnvironmentParticleShadows.cpp Filmmaker/Environment/EnvironmentCasterBounds.cpp)
target_compile_features(EnvironmentAnimatedCasterTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentAnimatedCasterTests PRIVATE d3d11 dxguid)
add_test(NAME EnvironmentAnimatedCasterTests COMMAND EnvironmentAnimatedCasterTests)

add_executable(EnvironmentRayTests ../Automation/tests/environment-ray-tests.cpp)
target_link_libraries(EnvironmentRayTests PRIVATE EnvironmentRayBackend)
add_test(NAME EnvironmentRayTests COMMAND EnvironmentRayTests)
set_tests_properties(EnvironmentRayTests PROPERTIES SKIP_RETURN_CODE 77)

target_sources(EnvironmentRenderTests PRIVATE Filmmaker/Environment/EnvironmentRayProbe.cpp)
target_link_libraries(EnvironmentRenderTests PRIVATE EnvironmentRayBackend)

add_executable(EnvironmentRaySurfaceTests ../Automation/tests/environment-ray-surface-tests.cpp)
target_compile_features(EnvironmentRaySurfaceTests PRIVATE cxx_std_17)
add_test(NAME EnvironmentRaySurfaceTests COMMAND EnvironmentRaySurfaceTests)

add_executable(SurfaceRayGridMathTests ../Automation/tests/surface-ray-grid-math-tests.cpp
    Filmmaker/Environment/SurfaceRayGridMath.h)
target_compile_features(SurfaceRayGridMathTests PRIVATE cxx_std_17)
add_test(NAME SurfaceRayGridMathTests COMMAND SurfaceRayGridMathTests)

add_executable(EnvironmentNativeCoatingTests ../Automation/tests/environment-native-coating-tests.cpp)
target_compile_features(EnvironmentNativeCoatingTests PRIVATE cxx_std_17)
add_test(NAME EnvironmentNativeCoatingTests COMMAND EnvironmentNativeCoatingTests)
add_executable(EnvironmentGizmoMathTests ../Automation/tests/environment-gizmo-math-tests.cpp)
target_compile_features(EnvironmentGizmoMathTests PRIVATE cxx_std_17)
add_test(NAME EnvironmentGizmoMathTests COMMAND EnvironmentGizmoMathTests)

target_sources(EnvironmentRenderTests PRIVATE Filmmaker/Environment/EnvironmentNativeMaterial.cpp Filmmaker/Relight/RelightScreenGi.cpp Filmmaker/Relight/RelightProbes.cpp Filmmaker/Relight/RelightProbeGi.cpp Filmmaker/Relight/RelightStainedGlass.cpp Filmmaker/Relight/RelightLightEditor.cpp Filmmaker/Relight/RelightLightmap.cpp)
add_executable(EnvironmentNativeMaterialTests ../Automation/tests/environment-native-material-tests.cpp
    Filmmaker/Relight/RelightScreenGi.cpp Filmmaker/Relight/RelightProbes.cpp Filmmaker/Relight/RelightProbeGi.cpp Filmmaker/Relight/RelightStainedGlass.cpp Filmmaker/Relight/RelightLightEditor.cpp Filmmaker/Relight/RelightLightmap.cpp
    Filmmaker/Environment/EnvironmentNativeMaterial.cpp Filmmaker/Relight/RelightNativeCluster.cpp)
target_compile_features(EnvironmentNativeMaterialTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentNativeMaterialTests PRIVATE d3d11 dxguid)
add_test(NAME EnvironmentNativeMaterialTests COMMAND EnvironmentNativeMaterialTests)

foreach(ao_target EnvironmentRenderTests EnvironmentNativeMaterialTests)
    target_sources(${ao_target} PRIVATE Filmmaker/Environment/EnvironmentAmbientOcclusion.cpp)
    target_sources(${ao_target} PRIVATE Filmmaker/Environment/EnvironmentSunVisibility.cpp)
    target_sources(${ao_target} PRIVATE Filmmaker/Environment/EnvironmentCasterBounds.cpp)
endforeach()
add_executable(EnvironmentCasterBoundsTests ../Automation/tests/environment-caster-bounds-tests.cpp
    Filmmaker/Environment/EnvironmentCasterBounds.cpp)
target_compile_features(EnvironmentCasterBoundsTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentCasterBoundsTests PRIVATE d3d11 dxguid)
add_test(NAME EnvironmentCasterBoundsTests COMMAND EnvironmentCasterBoundsTests)
add_executable(EnvironmentEarlyCasterTests ../Automation/tests/environment-early-caster-tests.cpp
    Filmmaker/Environment/EnvironmentAnimatedCasters.cpp Filmmaker/Environment/EnvironmentParticleShadows.cpp Filmmaker/Environment/EnvironmentCasterBounds.cpp)
target_compile_features(EnvironmentEarlyCasterTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentEarlyCasterTests PRIVATE d3d11 dxguid)
add_test(NAME EnvironmentEarlyCasterTests COMMAND EnvironmentEarlyCasterTests)
add_executable(EnvironmentAmbientOcclusionTests ../Automation/tests/environment-ambient-occlusion-tests.cpp
    Filmmaker/Environment/EnvironmentAmbientOcclusion.cpp)
target_compile_features(EnvironmentAmbientOcclusionTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentAmbientOcclusionTests PRIVATE d3d11 dxguid)
add_test(NAME EnvironmentAmbientOcclusionTests COMMAND EnvironmentAmbientOcclusionTests)

add_executable(ConsoleLoggingListenerTests ../Automation/tests/console-logging-listener-tests.cpp)
add_test(NAME ConsoleLoggingListenerTests COMMAND ConsoleLoggingListenerTests)

add_executable(EnvironmentPersistenceTests ../Automation/tests/environment-persistence-tests.cpp)
target_compile_features(EnvironmentPersistenceTests PRIVATE cxx_std_17)
add_test(NAME EnvironmentPersistenceTests COMMAND EnvironmentPersistenceTests)

add_executable(EnvironmentCacheTests ../Automation/tests/environment-cache-tests.cpp)
target_compile_features(EnvironmentCacheTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentCacheTests PRIVATE bcrypt)
add_test(NAME EnvironmentCacheTests COMMAND EnvironmentCacheTests)

add_executable(SceneKeyframeTrackTests
    ../Automation/tests/scene-keyframe-track-tests.cpp
    Filmmaker/Movie/SceneKeyframeCatalog.cpp
    Filmmaker/Environment/EnvironmentWorldDocument.cpp
    Filmmaker/Platform/JsonBuilder.cpp
    Filmmaker/Platform/JsonParser.cpp)
target_compile_features(SceneKeyframeTrackTests PRIVATE cxx_std_17)
add_test(NAME SceneKeyframeTrackTests COMMAND SceneKeyframeTrackTests)

add_executable(DeathAnimationsMathTests
    ../Automation/tests/death-animations-math-tests.cpp
    Filmmaker/Movie/DeathAnimationsMath.h)
target_compile_features(DeathAnimationsMathTests PRIVATE cxx_std_17)
add_test(NAME DeathAnimationsMathTests COMMAND DeathAnimationsMathTests)

add_executable(NervousSystemMathTests
    ../Automation/tests/nervous-system-math-tests.cpp
    Filmmaker/Movie/NervousSystemMath.h
    Filmmaker/Movie/DeathAnimationsMath.h)
target_compile_features(NervousSystemMathTests PRIVATE cxx_std_17)
add_test(NAME NervousSystemMathTests COMMAND NervousSystemMathTests)

add_executable(LensFxMathTests
    ../Automation/tests/lens-fx-math-tests.cpp
    Filmmaker/Lens/LensFxMath.h
    Filmmaker/Lens/LensFxShaders.h)
target_compile_features(LensFxMathTests PRIVATE cxx_std_17)
add_test(NAME LensFxMathTests COMMAND LensFxMathTests)

add_executable(LensFlareGpuTests ../Automation/tests/lens-flare-gpu-tests.cpp)
target_compile_features(LensFlareGpuTests PRIVATE cxx_std_17)
target_link_libraries(LensFlareGpuTests PRIVATE d3d11 d3dcompiler)
add_test(NAME LensFlareGpuTests COMMAND LensFlareGpuTests)

add_executable(EnvironmentParticleShadowTests ../Automation/tests/environment-particle-shadow-tests.cpp Filmmaker/Environment/EnvironmentParticleShadows.cpp)
target_compile_features(EnvironmentParticleShadowTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentParticleShadowTests PRIVATE d3d11 dxguid)
add_test(NAME EnvironmentParticleShadowTests COMMAND EnvironmentParticleShadowTests)

add_executable(SmokeAppearanceTests ../Automation/tests/smoke-appearance-tests.cpp
    Filmmaker/World/SmokeAppearance.cpp)
target_compile_features(SmokeAppearanceTests PRIVATE cxx_std_17)
target_link_libraries(SmokeAppearanceTests PRIVATE d3d11 d3dcompiler dxguid)
add_test(NAME SmokeAppearanceTests COMMAND SmokeAppearanceTests)

add_executable(MapDataTests ../Automation/tests/map-data-tests.cpp)
target_link_libraries(MapDataTests PRIVATE MapData)
add_test(NAME MapDataTests COMMAND MapDataTests)

add_executable(DebugLogDedupMathTests ../Automation/tests/debug-log-dedup-math-tests.cpp)
target_compile_features(DebugLogDedupMathTests PRIVATE cxx_std_17)
add_test(NAME DebugLogDedupMathTests COMMAND DebugLogDedupMathTests)

# Exercise the real Windows pointer-hook helper, including page protection restoration.
add_executable(AfxDetourPointerTests ../Automation/tests/afx-detour-pointer-tests.cpp
    ../Shared/AfxDetours.cpp
    ../Deps/release/Detours/src/modules.cpp
    ../Deps/release/Detours/src/detours.cpp
    ../Deps/release/Detours/src/disasm.cpp
    ../Deps/release/Detours/src/disolarm.cpp
    ../Deps/release/Detours/src/disolarm64.cpp
    ../Deps/release/Detours/src/disolia64.cpp
    ../Deps/release/Detours/src/disolx64.cpp
    ../Deps/release/Detours/src/disolx86.cpp)
target_include_directories(AfxDetourPointerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(AfxDetourPointerTests PRIVATE cxx_std_17)
add_test(NAME AfxDetourPointerTests COMMAND AfxDetourPointerTests)

add_executable(EnvironmentDepthReadTests ../Automation/tests/environment-depth-read-tests.cpp)
target_compile_features(EnvironmentDepthReadTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentDepthReadTests PRIVATE d3d11 d3dcompiler)
add_test(NAME EnvironmentDepthReadTests COMMAND EnvironmentDepthReadTests)

add_executable(EnvironmentCasterAlphaTests ../Automation/tests/environment-caster-alpha-tests.cpp)
target_compile_features(EnvironmentCasterAlphaTests PRIVATE cxx_std_17)
add_test(NAME EnvironmentCasterAlphaTests COMMAND EnvironmentCasterAlphaTests)

add_executable(EnvironmentNativeLightingTests ../Automation/tests/environment-native-lighting-tests.cpp)
target_compile_features(EnvironmentNativeLightingTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentNativeLightingTests PRIVATE d3d11 d3dcompiler)
add_test(NAME EnvironmentNativeLightingTests COMMAND EnvironmentNativeLightingTests)

add_executable(EnvironmentAtmosphereEdgeTests ../Automation/tests/environment-atmosphere-edge-tests.cpp)
target_compile_features(EnvironmentAtmosphereEdgeTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentAtmosphereEdgeTests PRIVATE d3d11 dxguid)
add_test(NAME EnvironmentAtmosphereEdgeTests COMMAND EnvironmentAtmosphereEdgeTests --hardware)

add_executable(EnvironmentCasterBufferTests ../Automation/tests/environment-caster-buffer-tests.cpp)
target_compile_features(EnvironmentCasterBufferTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentCasterBufferTests PRIVATE d3d11 dxguid)
add_test(NAME EnvironmentCasterBufferTests COMMAND EnvironmentCasterBufferTests)

add_executable(EnvironmentNativeClusterTests ../Automation/tests/environment-native-cluster-tests.cpp)
target_compile_features(EnvironmentNativeClusterTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentNativeClusterTests PRIVATE d3d11 d3dcompiler dxguid)
add_test(NAME EnvironmentNativeClusterTests COMMAND EnvironmentNativeClusterTests)

add_executable(EnvironmentLightingViewTests ../Automation/tests/environment-lighting-view-tests.cpp)
target_compile_features(EnvironmentLightingViewTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentLightingViewTests PRIVATE d3d11 d3dcompiler dxguid)
add_test(NAME EnvironmentLightingViewTests COMMAND EnvironmentLightingViewTests)

add_executable(EnvironmentNativeLampTests ../Automation/tests/environment-native-lamp-tests.cpp)
target_compile_features(EnvironmentNativeLampTests PRIVATE cxx_std_17)
target_link_libraries(EnvironmentNativeLampTests PRIVATE d3d11 d3dcompiler dxguid)
add_test(NAME EnvironmentNativeLampTests COMMAND EnvironmentNativeLampTests)

add_executable(FreeCamStepMathTests ../Automation/tests/free-cam-step-math-tests.cpp)
target_compile_features(FreeCamStepMathTests PRIVATE cxx_std_17)
add_test(NAME FreeCamStepMathTests COMMAND FreeCamStepMathTests)
