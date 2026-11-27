// Depth of field and the lens flare are Beauty effects: a render job's Key, Depth and Normal passes
// are built from the scene before the lens.
//
// The lens ran on every render of a job, so the Gun only key (Main against NoGun) picked up world
// edges both renders had blurred differently, the Players only key came out blurred, and the flare
// painted over the chroma background (tester reports, 2026-10-24). Static checks against the
// sources, so they run without CS2.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (rel) => fs.readFileSync(new URL('../../' + rel, import.meta.url), 'utf8');
const hooks = read('AfxHookSource2/RenderSystemDX11Hooks.cpp');

test('compositor passes and the Gun only reference read the colour from before the lens', () => {
  assert.match(hooks, /AfxRenderPass::StoreReference\(pDeviceContext, AfxRenderPass::CleanColor\(pTexture\)/);
  assert.match(hooks, /AfxRenderPass::Compose\(pDeviceContext, AfxRenderPass::CleanColor\(pTexture\)/);
  const boundary = hooks.slice(hooks.indexOf('class CAfxRenderCallbackBeforeUi'));
  const keep = boundary.indexOf('AfxRenderPass::KeepCleanColor(pDeviceContext');
  const lens = boundary.indexOf('Filmmaker::LensFx_Render(');
  const rain = boundary.indexOf('Filmmaker::LensRain_Render(');
  const captures = boundary.indexOf('pRenderPassCommands->OnBeforeUi(pTexture)');
  const drop = boundary.indexOf('AfxRenderPass::DropCleanColor()');
  assert.ok(keep > 0 && keep < lens, 'the clean copy is kept before the lens draws');
  assert.ok(lens < rain && rain < captures, 'lens rain draws after depth of field and before capture');
  assert.ok(lens < captures && captures < drop, 'the captures run before the clean copy is dropped');
});

test('the lens sits out renders no Beauty plate reads, and extra renders keep the Main focus', () => {
  assert.match(hooks, /pLensCommands && pLensCommands->SkipLens\) && !AfxLivePassPreviewShown\(\)/);
  assert.match(hooks, /commands\.SkipLens = !recordsAsDrawn;/);
  assert.match(hooks, /commands\.KeepCleanColor = recordsAsDrawn && composes;/);
  assert.match(hooks, /commands\.HoldLensFocus = true;/);
  assert.match(hooks, /rainHasWater = beautyEffects && !keyPass && Filmmaker::LensRain_Active\(\)/);
  assert.match(hooks, /rainRuns = rainHasWater && \(rainOnly \|\| picture\)/);
  assert.match(hooks, /\(lensRuns \|\| rainRuns \|\| reshadeRuns \|\| nodesRun\) && pLensCommands && pLensCommands->KeepCleanColor/);
  // Effect nodes change only Beauty: never a key pass, and the composed plates read the clean copy.
  assert.match(hooks, /const bool nodesRun = beautyEffects && !keyPass && Filmmaker::EffectNodes::Active\(\);/);
  assert.match(hooks, /if \(nodesRun\) \{/);
  assert.match(read('AfxHookSource2/StreamSettings.h'), /bool IsComposedPass\(\) const/);
  const lens = read('AfxHookSource2/Filmmaker/Lens/LensFx.cpp');
  assert.match(lens, /const bool advanceFocus = autoFocus && !\(holdFocus && g\.focusValid\);/);
});

test('lens preview is live only and rain only skips scene effects', () => {
  assert.match(hooks, /requestedPreview = beautyEffects && !keyPass && !AfxStreams_IsRcording\(\)\s*\? Filmmaker::LensFx_GetPreview\(\) : LensPreview::Picture/);
  assert.match(hooks, /requestedPreview == LensPreview::Rain && !rainHasWater\s*\? LensPreview::Picture/);
  assert.match(hooks, /lensRuns = beautyEffects && !rainOnly/);
  assert.match(hooks, /reshadeRuns = beautyEffects && picture/);
  const rain = read('AfxHookSource2/Filmmaker/Lens/LensRain.cpp');
  assert.match(rain, /c->GenerateMips\(g.copySrv.Get\(\)\);\s*if \(preview\) c->ClearRenderTargetView\(target, black\);/);
  assert.match(rain, /srvs\[\] = \{ g.copySrv.Get\(\) \}/);
  const lens = read('AfxHookSource2/Filmmaker/Lens/LensFx.cpp');
  assert.match(lens, /if \(preview != LensPreview::Dof\) L\[DofDebug\] = 0;/);
  assert.match(lens, /if \(preview != LensPreview::Flare\) L\[FlareDebug\] = 0;/);
});

test('lens rain respects the native scope surround without altering clean pass color', () => {
  const rain = read('AfxHookSource2/Filmmaker/Lens/LensRain.cpp');
  assert.match(hooks, /strcmp\(name, "legacy_sniper_scope\.vfx_ps"\)[\s\S]{0,110}g_NativeScopeDrawnThisPresent = true/);
  assert.match(hooks, /LensRain_Render\(pDeviceContext, pRenderTargetViews\[0\], rainOnly, g_NativeScopeDrawnThisPresent\)/);
  assert.match(rain, /if \(scoped\) \{[\s\S]{0,500}scopeSource[\s\S]{0,350}discard;\s*\}/);
  assert.ok(hooks.indexOf('AfxRenderPass::KeepCleanColor(pDeviceContext, pSceneTexture)') < hooks.indexOf('Filmmaker::LensRain_Render('));
});

// The live view drew the game HUD over a Depth + Gun only plate, and the HUD's bottom shading laid a
// dark gradient over the white gun that the render never had.
test('the live view of a plate without the HUD hides the HUD', () => {
  const command = read('AfxHookSource2/Filmmaker/Render/RenderCommand.cpp');
  assert.match(command,
    /if \(live\.type != PassType::Beauty \|\| e != VisibleElements::Everything\)\s*CameraEditorHudRef\(\)\.SetHudViewOverride\(CameraEditorHud::HudView::HideAll\);\s*else\s*CameraEditorHudRef\(\)\.ClearHudViewOverride\(\);/);
});
