#pragma once

// EFFECTS tab of the CONFIG panel, first card: VFX -- PARTICLE PACKS. JS fragment concatenated
// (via #include, as an adjacent raw string literal) into the kConfigHudJs script body assembled
// in ConfigHudJs.h -- not a standalone script. Shares scope with the root IIFE, and appends to
// `pageEffects` / `renderHooks`.
//
// THE EFFECTS / ENVIRONMENT SPLIT (Docs/panorama/panorama_ui_guide.md): everything on this tab drives something CS2
// ALREADY HAS -- particle packs here, then the engine sun, exposure, the skybox library, smoke
// colours, world tint and cloud density in the cards that follow. The added renderer (procedural
// sky, volumetric fog, precipitation, coatings, grade) is the ENVIRONMENT tab and carries its own
// teal accent, so the two classes never read as the same thing. "Fog" appearing on both tabs is
// correct: one is the map's own fog, the other is added volumetric fog.
//
// Particle-effect toggles (ParticleFx.h): each category is a segmented mode row driving
// 'mirv_filmmaker fx set <cat> <mode>'; state mirrors back through the fx_* fields each render,
// so console changes show up here too.
//
//   On     = Insurgency 2 (full detail)
//   Modern = the converted MW2019 ARC9 pack, where it ships assets (tracers, weapon fx, HE)
//   Off    = default CS2 pass-through
//
// HE grenade and the bomb blast are separate rows so they can mix (e.g. Insurgency bomb +
// Modern HE). ('More' was dropped 2026-07-02 -- byte-identical to On.) Smoke GRENADES are never
// affected: CS2 volumetric smoke is not a particle swap.
//
// LAYOUT: ONE card with four sub-pages (Gunfire / Damage / Explosives / World). It used to be
// five separate cards -- a master card plus one per category -- which made the tab a stack of
// shut drawers you had to open one at a time to answer "what is switched on".

R"CFJS(
    var fxSec = section(pageEffects, 'VFX — PARTICLE PACKS', true, true, 'fx.packs', '#80bdffff');
    var fxCardReset = fxSec.__headerReset([
      'mirv_filmmaker fx set impacts off quiet', 'mirv_filmmaker fx set tracers off quiet',
      'mirv_filmmaker fx set weaponfx off quiet', 'mirv_filmmaker fx set explosions off quiet',
      'mirv_filmmaker fx set bombfx off quiet', 'mirv_filmmaker fx set molotov off quiet',
      'mirv_filmmaker fx set flashbang off quiet',
      'mirv_filmmaker fx moneyshot off', 'mirv_filmmaker fx mollylights off',
      'mirv_filmmaker fx helights off', 'mirv_filmmaker fx flashlights off', 'mirv_filmmaker fx off quiet'
    ], 'fx.vfx');
    var fxMaster = fxSec.__headerPill(
      function () { cmd('mirv_filmmaker fx ' + (st && st.fxOn ? 'off' : 'on') + ' quiet'); },
      'Controls custom particle effects. Off keeps every category at the CS2 default.');

    // Segmented mode control, one per particle category. Per-row mode subsets mirror
    // ParticleFx's ModeSupported() -- offering a mode with no distinct assets just confuses
    // (it would snap back to On on the C++ side anyway). `tip` hangs the hover explanation on
    // the name label only, same rule as sliderRow/togglePill in ConfigHudWidgetsJs.h.
    //
    // THE MODE COLUMN IS A FIXED WIDTH, AND EVERY ROW USES IT: a 2-mode row (On/Off) and a
    // 3-mode row (On/Modern/Off) share one straight edge down the whole card, so the selected
    // cell always lands in a predictable place.
    var FXSEG_W = 224;
    // `cmdFor` is optional: give it a function(mode) -> console command for a row that is not
    // a plain `fx set <category>` (Money on Headshot has its own verb).
    function fxSeg(parent, label, catKey, modes, tip, cmdFor) {
      var caps = { on: 'On', more: 'More', less: 'Less', modern: 'Modern', off: 'Off' };
      var items = [];
      for (var i = 0; i < modes.length; i++) items.push([modes[i], caps[modes[i]]]);
      return modeRow(parent, label, items, FXSEG_W + 'px', function (mode) {
        cmd(cmdFor ? cmdFor(mode) : ('mirv_filmmaker fx set ' + catKey + ' ' + mode + ' quiet'));
      }, tip);
    }

    // ONE page with three sub-headings (Docs/features/filmmaker_config_panel.md): seven mode rows fit without paging.
    var fxPageGun = fxSec, fxPageDmg = fxSec, fxPageExpl = fxSec;
    subHead(fxSec, 'Gunfire').style.marginTop = '2px';
    var fxImpacts  = fxSeg(fxPageGun, 'Bullet impacts', 'impacts', ['on', 'off'],
      'Randomized reduced Povarhok and Insurgency sparks and debris. Off keeps the CS2 default.');
    var fxTracers  = fxSeg(fxPageGun, 'Bullet tracers', 'tracers', ['on', 'modern', 'off'],
      'The light streaks bullets draw in flight. Modern is the MW-style pack; Off keeps the CS2 default.');
    var fxWeaponfx = fxSeg(fxPageGun, 'Muzzle & shells', 'weaponfx', ['on', 'modern', 'off'],
      'Muzzle flash, smoke and ejected shells. Modern is the MW-style pack; Off keeps the CS2 default.');

    subHead(fxSec, 'Damage');
    // Money-on-headshot swaps the headshot-only particles, so it fires exactly on real headshot
    // hits. (Blood itself is never swapped: CS2's own blood is plenty.) Same segmented control as every other row -- it used
    // to be a lone flip-label button, which hid the state you were not in.
    var fxMoney = fxSeg(fxPageDmg, 'Money on headshot', 'moneyshot', ['on', 'off'],
      'Bursts money instead of the usual effect on headshot kills.',
      function (mode) { return 'mirv_filmmaker fx moneyshot ' + mode; });

    subHead(fxSec, 'Explosives');
    var fxExplosions = fxSeg(fxPageExpl, 'HE grenade', 'explosions', ['on', 'modern', 'off'],
      'The HE grenade blast. Modern is the MW-style pack; Off keeps the CS2 default.');
    var fxBombfx     = fxSeg(fxPageExpl, 'Bomb (C4)', 'bombfx', ['on', 'off'],
      'The bomb detonation. Off keeps the CS2 default.');
    var fxFlashbang  = fxSeg(fxPageExpl, 'Flash grenade', 'flashbang', ['on', 'off'],
      'The flashbang detonation. On is the Modern Warfare 2019 flash: a white burst with sparks, '
      + 'debris, a smoke shockwave and a lingering smoke cloud. Off keeps the CS2 default.');
    var fxFlashBlind = fxSeg(fxPageExpl, 'Flash blindness', 'flashblind', ['on', 'off'],
      'Off: flashbangs still go off (sound and particles) but never blind anyone, so the '
      + 'detonation can be seen and filmed. On is the CS2 default.',
      function (mode) { return 'mirv_filmmaker fx flashblind ' + mode; });
    var fxMolotov    = fxSeg(fxPageExpl, 'Nade trails', 'molotov', ['on', 'off'],
      'In-flight smoke trail on thrown grenades. Molotov and incendiary get the full trail; '
      + 'Smoke grenades get the smoke ribbon alone. Molotov ground fire always stays CS2 default.');

    // These cells are independent toggles; any combination may be highlighted.
    function fxDynamicLightRow(parent, proxy) {
      var items = [['mollylights', 'Molly'], ['flashlights', 'Flashbang'], ['helights', 'Grenade']];
      var mask = 0;
      var row = modeRow(parent, 'Dynamic Lights', items, FXSEG_W + 'px', function (key) {
        for (var i = 0; i < items.length; i++) if (items[i][0] === key) {
          mask ^= (1 << i);
          row.sync(mask);
          cmd('mirv_filmmaker fx ' + key + ((mask & (1 << i)) ? ' on' : ' off'));
          break;
        }
      }, 'Grenade dynamic lights: click each type to toggle it. Highlighted means On. '
        + 'Molly adds fire lighting; Flashbang and Grenade add brief detonation bursts. '
        + 'Any combination works with native map lighting.', 12, true);
      row.sync = function (value) {
        mask = value || 0;
        for (var i = 0; i < items.length; i++) setBtnOn(row.btns[items[i][0]], !!(mask & (1 << i)), S.accent);
        if (row.__search) row.__search.mirror(mask);
      };
      if (!proxy) row.__search = cfgSearchAdd('seg', 'Dynamic Lights', row.row,
        'Grenade Molly Molotov Flashbang', function (host) { return fxDynamicLightRow(host, true); });
      return row;
    }
    var fxDynamicLights = fxDynamicLightRow(fxPageExpl, false);

    var fxWorldNote = lbl(fxPageExpl, 'Off keeps the CS2 default for that category. Smoke grenades are never swapped.',
      S.hint, 12);
    fxWorldNote.hittest = false; fxWorldNote.style.whiteSpace = 'normal';
    fxWorldNote.style.width = '100%'; fxWorldNote.style.marginTop = '10px';
    fxWorldNote.style.paddingLeft = UI.line.padH + 'px'; fxWorldNote.style.paddingRight = UI.line.padH + 'px';
    // The one state the card cannot show as a value: the pack is enabled but still installing.
    var fxStatus = lbl(fxPageExpl, 'Effects are still loading.', '#e8b339ff', 13);
    fxStatus.hittest = false; fxStatus.style.fontWeight = 'bold';
    fxStatus.style.whiteSpace = 'normal'; fxStatus.style.marginTop = '8px'; fxStatus.visible = false;
    fxStatus.style.paddingLeft = UI.line.padH + 'px';

    renderHooks.push(function (st) {
      fxMaster.set(!!st.fxOn);
      fxImpacts.sync(st.fx_impacts || 'off');
      fxTracers.sync(st.fx_tracers || 'off');
      fxWeaponfx.sync(st.fx_weaponfx || 'off');
      fxExplosions.sync(st.fx_explosions || 'off');
      fxBombfx.sync(st.fx_bombfx || 'off');
      fxFlashbang.sync(st.fx_flashbang || 'off');
      fxFlashBlind.sync(st.fxFlashBlind === false ? 'off' : 'on');
      fxMolotov.sync(st.fx_molotov || 'off');
      fxDynamicLights.sync((st.fxMollyLights ? 1 : 0) | (st.fxFlashLights ? 2 : 0) | (st.fxHeLights ? 4 : 0));
      fxMoney.sync(st.fxMoneyshot ? 'on' : 'off');
      fxStatus.visible = !!st.fxOn && !st.fxReady;
      // "N of 11 custom" is the honest summary: naming the modes would need three words per
      // category, and the count is what you actually want at a glance -- whether this tab is
      // doing anything at all.
      var vals = [st.fx_impacts, st.fx_tracers, st.fx_weaponfx,
                  st.fxMoneyshot ? 'on' : 'off', st.fx_explosions, st.fx_bombfx,
                  st.fx_molotov, st.fx_flashbang, st.fxMollyLights ? 'on' : 'off',
                  st.fxHeLights ? 'on' : 'off', st.fxFlashLights ? 'on' : 'off'];
      var custom = 0;
      for (var i = 0; i < vals.length; i++) if (vals[i] && vals[i] !== 'off') custom++;
      fxSec.__note(st.fxOn ? (custom + ' of ' + vals.length + ' custom') : 'Off');
    });
)CFJS"
