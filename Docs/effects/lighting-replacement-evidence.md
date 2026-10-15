# CS2 lighting replacement evidence

Investigation and evidence for the replacement lighting path. Inspected revisions, dates and
outcomes are listed below. Current behavior and open verification gaps are in
[whole-map relighting](whole-map-relighting.md). Proposed work is not necessarily implemented.

This records the implementation and its limits. It does not mean full lighting takeover
is complete. Reference checkouts and Unreal assets were read without modification.

## Rendering decisions

VRF source was inspected at commit `252ec35cf0ee58fc4454efd5fc6a1baf0c916b40`.
Its reconstructed renderer is a reference; actual CS2 shader layouts still require bytecode
and runtime validation.

| Decision | Evidence | Implementation / limit |
| --- | --- | --- |
| Keep overlay blending, depth bias, discard and sample coverage | [VRF RenderMaterial](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/252ec35cf0ee58fc4454efd5fc6a1baf0c916b40/Renderer/Renderer/Materials/RenderMaterial.cs): overlays disable depth writes; blend equations and A2C vary by material | [Native material replay](../../AfxHookSource2/Filmmaker/Environment/EnvironmentNativeMaterial.cpp) retains these states. Native coverage instrumentation keeps discard and alpha. |
| Separate validity from material alpha | Live RGB-only native outputs and 1×/4× synthetic shader-discard/A2C readbacks | Dedicated R8 coverage; no `alpha > .999` decision. |
| Subtract matching HDR passes before tone mapping | Captured CS2 `post_process` uses resolved HDR input t12; native tone handoff validated in D3D11 tests and live Mirage | Matching native baseline/replacement composition enters the original tone pass. Sky and finishing effects remain afterward. |
| Preserve local fixture evaluation when replacing the sun | Mirage authored sun color/brightness matches the observed directional submission; [VRF lighting](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/252ec35cf0ee58fc4454efd5fc6a1baf0c916b40/Renderer/Shaders/common/lighting.slang) distinguishes direct light inputs from irradiance and probes | Sun-only submission suppression, rather than wholesale map-light/shadow suppression. This identification still needs broader map validation. |
| Do not infer fixture identity from runtime entity indices | VRF-decoded Mirage contains 73 authored lights, while live entity searches do not expose that complete set | The [map data extractor](map-data-extraction.md) records resource-scoped authored IDs and baked indices. Runtime fixture controls are not implemented. |
| Respect lightmap version and brightness units | [WorldLoader](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/252ec35cf0ee58fc4454efd5fc6a1baf0c916b40/Renderer/Renderer/World/WorldLoader.cs), [WorldLightingInfo](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/252ec35cf0ee58fc4454efd5fc6a1baf0c916b40/Renderer/Renderer/World/WorldLightingInfo.cs): V1 indexed strengths differ from newer shadow textures; brightness/lumen paths vary by light family | Export both authored fields; do not treat every brightness value as linear intensity or lumens. |
| Validate every patched native constant range | Shipped complex shader DXBC operand inspection: cb3 declares 1990 float4 entries, runtime allocation has 2014; direction/radiance/cascade signatures are checked | Unknown ABI rejects replay. GPU copies patch exact ranges and keep unrelated bytes. |

## Unreal asset findings

The UEditingTools v4.7 folder contains packages recorded as UE 5.5.1, 5.5.4, 5.6.1 and 5.8.1.
The folder version is not an asset-version guarantee. The selected material/widget inspection
covered 45 assets using UAssetAPI revision `3228c1e86261aa08131f7ec0ff1a395f5d0b2a84`.

The decoded `MasterMaterial/MF/Utils/MF_NormalInvertion` graph has these actual connections:
input → `BreakOutFloat3Components`; its second output → multiply by -1; first output, negated
second output and third output → `MakeFloat3` → function output. This verifies Y inversion
in that utility. It does not establish that every importer or master material invokes it.

The character master contains 117 exports with one raw export; the weapon master contains 11
with one raw export. The UE 5.8.1 main widget has 397 exports with 17 raw exports. Despite an
explicit `VER_UE5_8` parser request, the tested API reports its schema as UE 5.7. Those exports
are marked incomplete. No claim about fully decoded importer Blueprint logic, map-light
conversion or a UE brightness conversion is supported yet. A matching Editor export is still
needed for those raw portions. The supplied content was predominantly character/weapon tools;
a complete map importer was not established by the inspection.

## Checks and current acceptance

- Native replay tests pass at 1× and 4× MSAA, including discard holes, A2C, RGB-only writes, translucent biased overlays,
  current-generation edits, original-buffer preservation, state restoration and the native
  HDR/tone handoff. The shipped complex shader accepts the instrumented coverage program.
- The full environment D3D11 renderer suite and debounce/persistence queue tests pass.
- Matched Mirage captures show improved staircase-junction lighting and retained palace/market
  fixture illumination. This is limited visual evidence, not all-material acceptance.
- The stock-versus-custom comparison uses `mirv_fov 110`, identical camera pose, stock CS2 with
  custom sky and lighting off, versus both on.
- A 299-frame Mirage recording covers rapid sun edits, repeated off/on, lighting reset,
  full reset and preset restoration. In the measured static wall region, mean RGB stays between
  146.33 and 161.21 on the 0 to 255 scale. Black and white fractions stay below 0.02%, and no isolated
  one-frame brightness reversal above five levels was found. This does not prove absence of
  every temporal artifact or validate rebuilding GI.
- Native debug captures show matching scene/render generation and target/depth provenance.
  Sampled HDR and finishing compositor costs are approximately 2-4 ms each in this view;
  the eight-draw material timing sample is not the cost of all material replay draws.
- An edit and quit sent in the same network command batch persisted before the 750 ms debounce,
  and the exact edited value restored after relaunch. Geometry reload rejects obsolete
  generation results and retains working geometry after failed replacement loads.

Generated evidence is under `Automation/output/lighting-replacement` and runtime `mvm_debug`
logs. Use [transition recording](../../Automation/verify/verify-lighting-transitions.ps1) after
joining an offline match and applying `exec dev` once. Do not reapply it for each screenshot.

## Cache and autosave follow-up

Geometry manifests use format 2 / converter 5. The loader verifies SHA-256 of the map VPK,
shared package directory and immutable collision/caster payloads before publishing them. This
invalidates referenced material/texture changes recorded in the shared directory. Payloads are
published before an atomically replaced manifest; interrupted preparation retains the existing
manifest and files. Manifest size/nesting and payload size are bounded. Caches that do not match this format rebuild.
Mirage cold preparation completed with 123,213 collision and 1,096,512 placed-render triangles;
a second helper invocation reported a verified cache hit. The in-game worker loaded the cache in
approximately 323 ms after preparation completed. This is geometry validation, not GI
cache acceptance. The packaged helper dependency is included in the Source 2 install.

Cache tests cover known SHA-256 output, corrupt/missing payloads, traversal, oversized/deep
manifests and interrupted publication. Autosave queue tests also cover failed writes across
map switches, recovering the latest pending snapshot without overwriting another map's edits.

Keeping the native sun globally and subtracting an additional matched material pass before adding
custom sunlight is not the shipped ownership solution. Global sun suppression remains an
unresolved limit for unsupported material variants.

## Sun horizon and wet reflections

Direct solar radiance reaches zero at the horizon across native material constants,
surface/weather shaders and voxel injection. Twilight sky color remains independent. A sun
below the horizon does not allocate a sun shadow map; local-light shadow allocation continues.
Wet-film highlights require a front-facing sun and respect surface sunlight strength. Wet-film
and water sky reflections shadow the procedural solar disk when sun shadows are enabled.
Rain retains sampled scene illumination instead of replacing fixture colors with flat ambient.

The math, native-material and renderer suites passed after these changes. GPU tests compare
sun power 8 against 0 at elevation -5 for native materials, wet surfaces and rain, and check that
local-light shadows remain while the sun shadow is disabled. Both runtime components were
installed and tested in offline Mirage, with `exec dev` once after joining and FOV 110.
The live below-horizon comparison changed sampled wall/floor regions by less than 0.01 RGB
code values on average; this permits small temporal differences and is not pixel identity.
Matched stock/custom daytime captures also completed. These views do not establish complete
through-ceiling reflection acceptance or coverage of every native shader.

The user's current priority is native material fidelity, editable per-map lights and correct
shadows. Ray-traced final quality is deferred. Mirage still retains baked daylight at night;
the horizon correction does not separate baked sunlight from baked fixture illumination.

## Fixture isolation and replacement boundary

The Mirage market test did not meet fixture acceptance. At zero retained lighting,
walls became dark while blurred emissive fixtures remained. Bright floor/prop patches also
show incomplete material coverage. Daytime captures do not establish independently
working fixtures after removing baked daylight.

The shipped lightmapped program hash `cd2f50b4703ab117` uses 480-byte fixture records at t20
and a clustered loop bound at cb3[19].z. Other variants use t17. Four bounded GPU readbacks in
the market view reported zero mask words; this is not evidence about every draw or map.
A private-buffer HDR differential replay
preserves existing clustered direct light when retained ambient changes. GPU tests cover
1x/4x MSAA, contribution separation and original-state preservation. This mechanism
cannot reconstruct lamps whose illumination only exists in baked lighting.

An offline, named `light_omni2` creation probe also failed visible illumination acceptance.
Creation and exposed Enable/SetColor/SetBrightness/CastDynamicShadows inputs do not prove a
working render submission or demo support. No claim of successful fixture restoration follows from this test.

The replacement must own authored map light records, culling, direct lighting, shadows and
replacement indirect/reflection inputs, feeding validated native material evaluation before
tone mapping. Albedo alone cannot retain normals, roughness, metalness, opacity, emission and
layer blending. Final-color dimming and bloom do not meet this requirement. Baked daylight in
reflection probes must also be replaced or explicitly identified as an outstanding limitation.

[VRF](https://github.com/ValveResourceFormat/ValveResourceFormat) supplies a separate renderer
and resource decoding. [SourceIO](https://github.com/REDxEYE/SourceIO) is a Blender importer.
They are useful references for reconstructing light/material inputs, not verified drop-in CS2
runtime bindings. The supplied YouTube URL could not be retrieved and was not used as evidence.

The next acceptance gate is one reconstructed market fixture with the sun off: enable, color,
intensity and range edits must affect nearby surfaces, retain normal/specular detail and cast
an occluded shadow. Verify opaque and decal receivers at 1x/4x MSAA before expanding to all
fixtures. Compare the same FOV-110 pose with custom sky/lighting both off versus both on.
Ray tracing remains deferred; this test has not passed yet.

## Remaining implementation

### Environment HDR and skybox handling

Adding linear native-material radiance to compressed post-process input produces black
surfaces and green speckles above the inverse tone mapper's singularity. The shared composite
therefore decodes before lighting and encodes once afterward, keeping compressed half-float highlights strictly below one. Linear HDR export
remains unchanged. The render regression covers increasing exposure through one million while
checking finite, monotonic, hue-preserving output and ordinary-range round trips.

The miniature skybox does not pass through Environment's final-color unbaking approximation.
Native sun suppression is scoped to validated playable-world/viewmodel material draws using a
private buffer, restored at the end of each draw hook. It does not affect the skybox or the
owned `mirv_relight` pipeline. The GPU material test checks unrelated constant preservation,
binding restoration, skybox exclusion and owned-pipeline exclusion.

Offline Mirage captures at Surface lighting 10%, 100% and 300% retained the distant city's
native texture, lighting and ground without the black/green failure. The saved Sun Power was
8, so high surface values still overexposed sunlit playable-world walls; this is not a claim
that extreme gains retain highlight detail or that owned relighting matches stock CS2.
Regular light/shadow edits survived switching Environment Lighting off. With owned relighting
active, Environment Surface lighting and baked-suppression edits did not take over its settings.
EnvironmentRenderTests and EnvironmentNativeMaterialTests passed. Local capture artifacts
are generated under `Automation/output/environment-hdr-fix/` and remain untracked.

### Outstanding replacement work

The [owned native-lighting experiment](lighting-owned-proof.md) demonstrates
private authored-light surface illumination and moving raster shadows on supported Mirage
floor materials. It includes a verified binding table and reproducible captures. It does not pass
the complete material, specular or whole-map dark-baseline gate above.

Persistent content-keyed GI/material/light caches and progress UI,
authored sun initialization, editable fixture overrides, version-15 scene serialization,
material-aware world voxelization, verified native replacement-indirect inputs and final-quality
ray accumulation are still outstanding. Current GI reuse is in memory. Its geometry identities hash content rather than runtime
pointers; completed GI results are not yet persisted. The ray-query backend is not a completed indirect-lighting renderer. Scene format
remains version 14. Complete generation publication across every shadow/GI resource, unknown
shader safe-native behavior and Dust2/Nuke/Train live acceptance remain to be verified.
