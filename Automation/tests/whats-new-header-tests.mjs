// The What's new button lives in the main menu's top bar, left of INVENTORY, and stays in the pause
// menu (WhatsNewJs.h). Static checks against the sources, so they run without CS2.
//
// During demo playback CS2 collapses the bar's centre group (INVENTORY ... NEWS carry
// .DisableDuringDemoPlayback, mainmenu.xml), so a button placed inside that group would vanish
// exactly in the pause menu. Like the HC TRACKER badge it is a Button at the end of #MainMenuInput.
import fs from 'node:fs';
import test from 'node:test';
import assert from 'node:assert/strict';

const read = (rel) => fs.readFileSync(new URL('../../' + rel, import.meta.url), 'utf8');
const whatsNew = read('AfxHookSource2/Filmmaker/Panorama/WhatsNewJs.h');
const gui = read('AfxHookSource2/Filmmaker/Panorama/FilmmakerGuiJs.h');

test('the button is created in #MainMenuInput, not inside the top bar or its centre group', () => {
  assert.match(whatsNew, /var layer = root3\.FindChildTraverse\('MainMenuInput'\) \|\| ctx;/);
  assert.match(whatsNew, /\$\.CreatePanel\('Button', layer, 'FmWhatsNewHeader'/);
  assert.doesNotMatch(whatsNew, /CreatePanel\([^)]*(MainMenuNavBarCenterContainer|JsMainMenuTopNavBar)/);
});

const layout = read('AfxHookSource2/Filmmaker/Panorama/HeaderLayoutJs.h');
const community = read('AfxHookSource2/Filmmaker/Community/CommunityJs.h');

// HeaderLayoutJs.h's slot function, evaluated as the page runs it.
function headerSlot() {
  const body = layout.slice(layout.indexOf('R"HLJS(') + 7, layout.indexOf(')HLJS"'));
  return new Function(body + '\nreturn mvmHeaderSlot;')();
}

test('one fixed spot each for the home and pause menus, from the shared bar layout', () => {
  for (const [name, text, fn] of [['WhatsNewJs.h', whatsNew, 'function wnHeaderPlace('], ['CommunityJs.h', community, 'function placeBadge(']]) {
    const place = text.slice(text.indexOf(fn));
    const body = place.slice(0, place.indexOf('\n    }\n') > 0 ? place.indexOf('\n    }\n') : 900);
    // Nothing that pausing changes feeds the spot: not INVENTORY (collapsed, and it keeps a stale
    // layout), not the bar's left container (pause hides its quit icon), no live panel positions.
    assert.doesNotMatch(body, /MainMenuNavBarInventory|left-container|GetPositionWithinWindow/, name);
    assert.match(body, /mvmHeaderSlot\((-1|1), w, mvmHeaderBarWidth\((root3|root)\)\)/, name);
  }
  assert.match(gui, /#include "HeaderLayoutJs\.h"/);
  assert.match(community, /#include "\.\.\/Panorama\/HeaderLayoutJs\.h"/);
  // HC TRACKER is left-anchored from the slot now, not pinned 200px from the right edge.
  assert.doesNotMatch(community, /marginRight = '200px'/);
});

test('both buttons are centred in their stretch: even room before and after', () => {
  const slot = headerSlot();
  // At 1920 wide with the widths measured in game (2026-10-24): WHAT'S NEW 204px, HC TRACKER 189px.
  const left = slot(-1, 204, 1920), right = slot(1, 189, 1920);
  assert.equal(Math.round(left.x), 341);
  assert.equal(Math.round(left.before), Math.round(left.after));
  assert.equal(Math.round(right.x), 1438);
  assert.equal(Math.round(right.before), Math.round(right.after));
  // A 4:3 bar keeps them inside their stretches.
  const narrow = slot(1, 189, 1440);
  assert.ok(narrow.before > 0 && narrow.after > 0);
});

test('one placement loop per button', () => {
  assert.match(whatsNew, /var loop = \+\+wnPlaceLoop;/);
  assert.match(whatsNew, /if \(loop !== wnPlaceLoop \|\| !b\.IsValid\(\)\) return;/);
});

test('the Watch page no longer carries a What\'s new button', () => {
  assert.doesNotMatch(gui, /FmWhatsNewBtn/);
  assert.doesNotMatch(whatsNew, /FmWhatsNewBtn/);
});
