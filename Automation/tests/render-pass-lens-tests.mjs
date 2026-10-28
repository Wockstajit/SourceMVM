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
  const captures = boundary.indexOf('pRenderPassCommands->OnBeforeUi(pTexture)');
  const drop = boundary.indexOf('AfxRenderPass::DropCleanColor()');
  assert.ok(keep > 0 && keep < lens, 'the clean copy is kept before the lens draws');
  assert.ok(lens < captures && captures < drop, 'the captures run before the clean copy is dropped');
});

test('the lens sits out renders no Beauty plate reads, and extra renders keep the Main focus', () => {
  assert.match(hooks, /pLensCommands && pLensCommands->SkipLens\) && !AfxLivePassPreviewShown\(\)/);
  assert.match(hooks, /commands\.SkipLens = !recordsAsDrawn;/);
  assert.match(hooks, /commands\.KeepCleanColor = recordsAsDrawn && composes;/);
  assert.match(hooks, /commands\.HoldLensFocus = true;/);
  assert.match(read('AfxHookSource2/StreamSettings.h'), /bool IsComposedPass\(\) const/);
  const lens = read('AfxHookSource2/Filmmaker/Lens/LensFx.cpp');
  assert.match(lens, /const bool advanceFocus = autoFocus && !\(holdFocus && g\.focusValid\);/);
});

// The live view drew the game HUD over a Depth + Gun only plate, and the HUD's bottom shading laid a
// dark gradient over the white gun that the render never had.
test('the live view of a plate without the HUD hides the HUD', () => {
  // CS2's live pass view (RenderHost_ApplyLivePreview).
  const command = read('AfxHookSource2/Filmmaker/Render/RenderHostSource2.cpp');
  assert.match(command,
    /if \(live\.type != PassType::Beauty \|\| e != VisibleElements::Everything\)\s*CameraEditorHudRef\(\)\.SetHudViewOverride\(CameraEditorHud::HudView::HideAll\);\s*else\s*CameraEditorHudRef\(\)\.ClearHudViewOverride\(\);/);
});
