#pragma once

// ATMOSPHERE card of the CONFIG panel's EFFECTS tab -- what is in the air, as CS2 draws it:
// grenade smoke colour/appearance and the whole-scene tint. JS fragment concatenated (via #include, as an
// adjacent raw string literal) into the kConfigHudJs body assembled in ConfigHudJs.h. Shares
// scope with the root IIFE and appends to `pageEffects` / `renderHooks`.
//
// (Added volumetric fog is a different thing entirely and lives on the ENVIRONMENT tab. Both
// tabs having something called fog is correct -- see the split note in ConfigHudEffectsTabJs.h.)
//
// SMOKE drives the EXISTING `mirv_colors smokes t|ct` backend (AfxHookSource2/MirvColors.cpp).
// State is mirrored back through MirvColors_GetSmokeColorName so the panel and the console can
// never disagree.
//
// NO "apply to existing smoke" BUTTON, and no note explaining when a colour takes effect.
// CS2 bakes a smoke's colour into its voxel volume at spawn, so deployed smoke has to be
// re-triggered for a change to show -- but that is an engine implementation detail, not a
// decision the operator should have to make. SmokeFx.cpp detects the colour change and
// re-triggers automatically, so picking a colour just works. Nothing about that leaks here.
//
// ONE card, four sub-pages: Smoke (colour) / Look / Volume / World tint. The card is already
// ATMOSPHERE, so the smoke pages drop the "Smoke" prefix -- four long labels overran the strip.

R"CFJS(
    var atmosSec = section(pageEffects, 'ATMOSPHERE', true, false, 'atmos.card', '#80bdffff');
    var atmosReset = atmosSec.__headerReset([
      'mirv_colors smokes t default',
      'mirv_colors smokes ct default',
      'mirv_filmmaker world smoke appearance reset',
      'mirv_filmmaker world modulate reset'
    ], 'fx.atmosphere');

    // ===================== SMOKE ========================================
    // ONE page with dividers (2026-10 sidebar cleanup): the smoke's colour, look and volume, then
    // the world tint, as groups that scroll, instead of a page with sub-tabs inside it.
    var smokeCard = atmosSec;
    var smokePage = atmosSec.__group('Smoke colour');
    var smokeAppearancePage = atmosSec.__group('Smoke look');
    var smokeVolumePage = atmosSec.__group('Smoke volume');

    // Deliberately short: a long swatch grid is harder to scan than a few strong choices plus a
    // real picker for anything else.
    var SMOKE_COLORS = [
      ['default', '#00000000'], ['white', '#ffffffff'], ['red', '#e0302cff'],
      ['green', '#3fbf4aff'],   ['blue', '#3f7fe0ff'],  ['yellow', '#e8c93aff'],
      ['magenta', '#d84fc4ff'], ['cyan', '#3fd0d8ff'], ['black', '#1a1a1aff']
    ];

    var smokeCustomRgb = { t: [255, 128, 0], ct: [0, 200, 255] };
    var smokeRows = {};

    function smokeTeam(parent, caption, teamKey) {
      var picker = null;
      var grid = swatchGrid(parent, SMOKE_COLORS, function (nameKey) {
        if (picker) picker.hide();
        cmd('mirv_colors smokes ' + teamKey + ' ' + nameKey);
      }, {
        label: caption,
        custom: true,
        onCustom: function () { if (picker) picker.show(); }
      });

      picker = colorPicker(parent, 'CUSTOM ' + caption, smokeCustomRgb[teamKey],
        function (c) {
          grid.paintCustom(c);
          cmd('mirv_colors smokes ' + teamKey + ' ' + c[0] + ' ' + c[1] + ' ' + c[2]);
        });

      smokeRows[teamKey] = {
        // The picker is a closure local; the footer's RESET needs a way to close it.
        hide: function () { if (picker) picker.hide(); },
        sync: function (activeName) {
          // A named colour highlights its swatch. Anything else -- an r g b triple from the
          // picker or the console -- highlights Custom, which is the honest answer and also keeps
          // the Custom chip showing the colour actually in use.
          var named = false;
          for (var k = 0; k < SMOKE_COLORS.length; k++) {
            if (SMOKE_COLORS[k][0] === activeName) { named = true; break; }
          }
          grid.select(named ? activeName : 'custom');
          // Skip while this team's picker is open: its rgbRef is smokeCustomRgb[teamKey], so
          // syncing the lagged live value back would overwrite the drag mid-gesture.
          if (!named && (!picker || !picker.visible())) {
            var rgb = parseRgbTriple(activeName);
            if (rgb) {
              smokeCustomRgb[teamKey][0] = rgb[0];
              smokeCustomRgb[teamKey][1] = rgb[1];
              smokeCustomRgb[teamKey][2] = rgb[2];
              grid.paintCustom(rgb);
            }
          }
        }
      };
    }

    // "255 128 0" -> [255,128,0]; anything else -> null.
    function parseRgbTriple(s) {
      if (typeof s !== 'string') return null;
      var parts = s.split(' ');
      if (parts.length !== 3) return null;
      var out = [];
      for (var i = 0; i < 3; i++) {
        var n = parseInt(parts[i], 10);
        if (!isFinite(n)) return null;
        out.push(n < 0 ? 0 : (n > 255 ? 255 : n));
      }
      return out;
    }

    smokeTeam(smokePage, 'Terrorist', 't');
    smokeTeam(smokePage, 'Counter-T', 'ct');

    var smokeGroup = actionGroup(smokePage, 'Both teams', [
      ['Reset both', function () {
        cmd('mirv_colors smokes t default');
        cmd('mirv_colors smokes ct default');
      }]
    ], 2);

    // Smoke look and Smoke volume are sub-tabs of the Smoke page (built above with smokePage).
    var smokeAppearance = togglePill(smokeAppearancePage, 'Custom appearance',
      'Adjusts the look of deployed smoke. Switch off to return to the native appearance.',
      function () { cmd('mirv_filmmaker world smoke appearance toggle'); });
    var smokeShapeStatus = envNote(smokeAppearancePage, '');
    var smokeDensity = sliderRow(smokeAppearancePage, {label:'Density', cmd:'mirv_filmmaker world smoke appearance density', min:0, max:20, decimals:2, unit:'', tip:'How opaque the smoke looks. Native value: 0.85.'});
    var smokeWispy = sliderRow(smokeAppearancePage, {label:'Wispiness', cmd:'mirv_filmmaker world smoke appearance wispy', min:0, max:1, decimals:2, unit:'', tip:'Shapes the noise into wisps. Native value: 0.'});
    subHead(smokeAppearancePage, 'TEXTURE');
    var smokeNoise = sliderRow(smokeAppearancePage, {label:'Noise amount', cmd:'mirv_filmmaker world smoke appearance noise', min:0, max:3, decimals:2, unit:'', tip:'Overall noise distortion. Native value: 0.8.'});
    var smokeScaleLow = sliderRow(smokeAppearancePage, {label:'Coarse detail', cmd:'mirv_filmmaker world smoke appearance scale_low', min:0.05, max:20, decimals:2, unit:'', tip:'Frequency of the larger noise shapes. Higher values make smaller details. Native value: 3.'});
    var smokeStrengthLow = sliderRow(smokeAppearancePage, {label:'Coarse amount', cmd:'mirv_filmmaker world smoke appearance strength_low', min:0, max:1, decimals:2, unit:'', tip:'Strength of the coarse noise layer. Native value: 0.5.'});
    var smokeScaleHigh = sliderRow(smokeAppearancePage, {label:'Fine detail', cmd:'mirv_filmmaker world smoke appearance scale_high', min:0.05, max:20, decimals:2, unit:'', tip:'Frequency of the fine noise shapes. Native value: 7.'});
    var smokeStrengthHigh = sliderRow(smokeAppearancePage, {label:'Fine amount', cmd:'mirv_filmmaker world smoke appearance strength_high', min:0, max:1, decimals:2, unit:'', tip:'Strength of the fine noise layer. Native value: 0.35.'});
    subHead(smokeAppearancePage, 'MOTION');
    var smokeSpeed = sliderRow(smokeAppearancePage, {label:'Animation speed', cmd:'mirv_filmmaker world smoke appearance speed', min:0, max:10, decimals:2, unit:'', tip:'Speed of the moving noise. Zero stops this motion. Native value: 1.5.'});
    var smokeBoil = sliderRow(smokeAppearancePage, {label:'Boiling', cmd:'mirv_filmmaker world smoke appearance boil', min:-1, max:1, decimals:2, unit:'', tip:'Strength and direction of the rolling distortion. Native value: -0.3.'});
    subHead(smokeAppearancePage, 'SAMPLING');
    var smokeRayStep = sliderRow(smokeAppearancePage, {label:'Ray step', cmd:'mirv_filmmaker world smoke appearance ray_step', min:0.5, max:20, decimals:2, unit:'', tip:'Distance between rendering samples. Smaller values cost more GPU time. Native value: 6. This does not change voxel size.'});
    actionGroup(smokeAppearancePage, 'Appearance', [['Reset appearance', function () {cmd('mirv_filmmaker world smoke appearance reset');}]], 1);

    var smokeVolumeOn = togglePill(smokeVolumePage, 'Custom smoke',
      'Changes the visible smoke live, including clouds already deployed.',
      function () { cmd('mirv_filmmaker world smoke appearance toggle'); });
    var smokeSpread = sliderRow(smokeVolumePage, {label:'Spread', cmd:'mirv_filmmaker world smoke appearance spread', min:0.25, max:2, decimals:2, unit:'x', tip:'Scales the visible cloud around its origin. 1 is native size. Applies to existing smoke.'});
    var smokeVoxelSize = sliderRow(smokeVolumePage, {label:'Voxel size', cmd:'mirv_filmmaker world smoke appearance voxel_size', min:2.5, max:320, decimals:1, unit:'u', tip:'Size of a rendered cell in world units. Larger cells make coarser smoke. Linked to voxels per side; native size is 20 units.'});
    var smokeResolution = sliderRow(smokeVolumePage, {label:'Voxels per side', cmd:'mirv_filmmaker world smoke appearance resolution', min:4, max:64, decimals:0, unit:'', tip:'Number of rendered cells along each side of the smoke volume. Native grid: 32 per side. Increasing this resamples the existing smoke.'});
    var smokeGridStatus = envNote(smokeVolumePage, '');
    envNote(smokeVolumePage, 'Voxel size and count are linked. Spread changes the cloud size. These controls resample the visible smoke.');
    actionGroup(smokeVolumePage, 'Smoke', [['Reset smoke', function () {cmd('mirv_filmmaker world smoke appearance reset');}]], 1);

    // ===================== WORLD TINT ===================================
    // World Tint belongs with atmosphere: it is an environment-wide grade applied to geometry,
    // props, players, foliage, and the HDR sky. Lighting stays reserved for exposure, the sun,
    // and shadow direction.
    var modPage = atmosSec.__group('World tint');

    var MOD_TINTS = [
      ['off',     '#00000000', '255 255 255'],
      ['magenta', '#ff3ce0ff', '255 60 224'],
      ['crimson', '#ff5a5aff', '255 90 90'],
      ['amber',   '#ffb45aff', '255 180 90'],
      ['lime',    '#9ae86bff', '154 232 107'],
      ['cyan',    '#5ad6ffff', '90 214 255'],
      ['azure',   '#6b9dffff', '107 157 255'],
      ['violet',  '#b06bffff', '176 107 255'],
      ['mono',    '#b4bcc4ff', '180 188 196']
    ];
    var modCustomRgb = [255, 60, 224];
    var modPicker = null;

    function applyWorldTint(rgb) {
      if (rgb === '255 255 255') { cmd('mirv_filmmaker world modulate reset'); return; }
      // Treat this as a whole-scene grade. Leaving crates/foliage authored-colour or keeping a
      // bright blue HDR sky is exactly what made strong tints read washed out.
      cmd('mirv_filmmaker world modulate world ' + rgb + ' quiet');
      cmd('mirv_filmmaker world modulate props ' + rgb + ' quiet');
      cmd('mirv_filmmaker world modulate sunclouds ' + rgb + ' quiet');
    }

    var modGrid = swatchGrid(modPage, MOD_TINTS, function (nameKey, rgb) {
      if (modPicker) modPicker.hide();
      applyWorldTint(rgb);
    }, {
      label: 'Tint',
      custom: true,
      onCustom: function () { if (modPicker) modPicker.show(); }
    });

    modPicker = colorPicker(modPage, 'CUSTOM WORLD TINT', modCustomRgb, function (c) {
      modGrid.paintCustom(c);
      applyWorldTint(c[0] + ' ' + c[1] + ' ' + c[2]);
    });

    var modStrength = sliderRow(modPage, {
      label: 'Tint strength', cmd: 'mirv_filmmaker world modulate strength',
      min: 0, max: 1, decimals: 2, unit: '',
      tip: 'Blends between the map’s own colours (0) and the full tint (1). Use this rather than '
         + 'picking a paler swatch — a pale swatch still replaces the colour, this keeps it.'
    });

    // Ties the world tint to the sun tint when an operator wants one coherent environmental grade.
    var modGroup = actionGroup(modPage, 'Grade', [
      ['Match sun', function () {
        var c = lightCustomRgb;
        for (var i = 0; i < LIGHT_TINTS.length; i++) {
          if (lightGrid.selected === LIGHT_TINTS[i][0]) { c = LIGHT_TINTS[i][2].split(' '); break; }
        }
        modGrid.paintCustom(c); modGrid.select('custom');
        applyWorldTint(c[0] + ' ' + c[1] + ' ' + c[2]);
      }],
      ['Reset tint', function () { cmd('mirv_filmmaker world modulate reset'); }]
    ], 2, 'Reaches map geometry, model props, foliage, players and the HDR sky.');

    // Local half of the footer's RESET: only the open pickers. Both swatch grids and the strength
    // value mirror back from C++, so the commands settle them.
    fxResetLocal.atmosphere = function () {
      smokeRows.t.hide();
      smokeRows.ct.hide();
      if (modPicker) modPicker.hide();
    };

    renderHooks.push(function (st) {
      var t = st.smokeT || 'default', ct = st.smokeCT || 'default';
      smokeRows.t.sync(t);
      smokeRows.ct.sync(ct);
      smokeAppearance.set(st.smokeAppearance === true);
      smokeVolumeOn.set(st.smokeAppearance === true);
      var spread = typeof st.smoke_spread === 'number' ? st.smoke_spread : 1;
      var resolution = typeof st.smoke_resolution === 'number' ? st.smoke_resolution : 32;
      smokeSpread.sync(spread);
      smokeVoxelSize.sync(typeof st.smoke_voxel_size === 'number' ? st.smoke_voxel_size : 640 * spread / resolution);
      smokeResolution.sync(resolution);
      smokeGridStatus.text = resolution + ' x ' + resolution + ' x ' + resolution + ' = ' + (resolution * resolution * resolution) + ' rendered cells';
      smokeDensity.sync(typeof st.smoke_density === 'number' ? st.smoke_density : 0.85);
      smokeWispy.sync(typeof st.smoke_wispy === 'number' ? st.smoke_wispy : 0);
      smokeNoise.sync(typeof st.smoke_noise === 'number' ? st.smoke_noise : 0.8);
      smokeScaleLow.sync(typeof st.smoke_scale_low === 'number' ? st.smoke_scale_low : 3);
      smokeScaleHigh.sync(typeof st.smoke_scale_high === 'number' ? st.smoke_scale_high : 7);
      smokeStrengthLow.sync(typeof st.smoke_strength_low === 'number' ? st.smoke_strength_low : 0.5);
      smokeStrengthHigh.sync(typeof st.smoke_strength_high === 'number' ? st.smoke_strength_high : 0.35);
      smokeSpeed.sync(typeof st.smoke_speed === 'number' ? st.smoke_speed : 1.5);
      smokeBoil.sync(typeof st.smoke_boil === 'number' ? st.smoke_boil : -0.3);
      smokeRayStep.sync(typeof st.smoke_ray_step === 'number' ? st.smoke_ray_step : 6);
      smokeShapeStatus.text = st.smokeAppearance === true
        ? (st.smokeAppearanceReady === true ? 'Custom smoke appearance' : 'Waiting for a supported smoke renderer')
        : 'Native smoke appearance';

      var on = (st.mod_world_on === true);
      var p = (typeof st.mod_world === 'string') ? st.mod_world.split(' ') : ['255', '255', '255'];
      var mr = +p[0], mg = +p[1], mb = +p[2];

      var named = 'off';
      if (on) {
        named = 'custom';
        for (var i = 1; i < MOD_TINTS.length; i++) {
          var q = MOD_TINTS[i][2].split(' ');
          if (+q[0] === mr && +q[1] === mg && +q[2] === mb) { named = MOD_TINTS[i][0]; break; }
        }
      }
      modGrid.select(named);
      if (named === 'custom' && (!modPicker || !modPicker.visible())) {
        modCustomRgb[0] = mr; modCustomRgb[1] = mg; modCustomRgb[2] = mb;
        modGrid.paintCustom(modCustomRgb);
      }

      var strength = typeof st.mod_strength === 'number' ? st.mod_strength : 1;
      modStrength.sync(strength);

      // An r-g-b triple from the picker would blow past the summary's width, so a custom colour
      // reads as "custom" here and the swatch inside the card carries the actual value.
      function smokeWord(n) { return (n.indexOf(' ') >= 0) ? 'custom' : n; }
      atmosSec.__note('Smoke '
        + ((t === 'default' && ct === 'default') ? 'map default' : (smokeWord(t) + ' / ' + smokeWord(ct)))
        + (st.smokeAppearance === true ? ' · custom shape' : '')
        + ' · tint ' + (on ? ((named === 'custom' ? 'custom' : named) + ' ' + Math.round(strength * 100) + '%') : 'off'));
    });
)CFJS"
