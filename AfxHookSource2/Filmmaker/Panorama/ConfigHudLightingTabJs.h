#pragma once

// LIGHTING card of the CONFIG panel's EFFECTS tab. JS fragment concatenated (via #include, as an
// adjacent raw string literal) into the kConfigHudJs script body assembled in ConfigHudJs.h --
// not a standalone script. Shares scope with the root IIFE and appends to `pageEffects` /
// `renderHooks`.
//
// CS2's OWN lighting: the engine tonemapper's exposure, the engine sun's colour and direction,
// and the engine shadow maps. The added renderer's procedural sun is a different thing and lives
// on the ENVIRONMENT tab -- see the split note at the top of ConfigHudEffectsTabJs.h.
//
// ONE card, three sub-pages (Time of day / Sun / Shadow map). It used to be three separate cards,
// and one of them -- SHADOWS -- was built and then hidden with a GetParent().GetParent() reach,
// so its four tuning values existed only for the console. They are on the Shadow map page now.

R"CFJS(
    var litSec = section(pageEffects, 'LIGHTING', true, false, 'light.card', '#80bdffff');
    var lightResetBtn = litSec.__headerReset([
      'mirv_filmmaker world exposure auto quiet',
      'mirv_filmmaker world light reset',
      'mirv_filmmaker world sun reset',
      'mirv_filmmaker world shadows reset',
      'mirv_filmmaker world customshadow reset'
    ], 'fx.lighting');

    // ===================== TIME OF DAY ==================================
    // Each one pins the tonemapper to a set exposure. Fixed values, so they do not claim to
    // change the time of day -- they change how the frame is exposed.
    var todPage = litSec.__group('Time of day');
    var TOD = [
      ['dawn',   'Dawn',    0.55],
      ['day',    'Day',     1.00],
      ['golden', 'Golden',  0.85],
      ['dusk',   'Dusk',    0.35],
      ['night',  'Night',   0.10],
      ['moon',   'Moonlit', 0.22]
    ];
    var todItems = [];
    for (var todI = 0; todI < TOD.length; todI++) todItems.push([TOD[todI][0], TOD[todI][1]]);
    var todGrid = optionGrid(todPage, 'Time of day', todItems, 3,
      'Each one pins the tonemapper to a set exposure: dawn 0.55, day 1.00, golden 0.85, '
      + 'dusk 0.35, night 0.10, moonlit 0.22.',
      function (key) {
        for (var i = 0; i < TOD.length; i++)
          if (TOD[i][0] === key) cmd('mirv_filmmaker world exposure ' + TOD[i][2].toFixed(2) + ' quiet');
      }, 15);

    var expSlider = sliderRow(todPage, {
      label: 'Exposure',
      cmd: 'mirv_filmmaker world exposure',
      min: 0.02, max: 4.0, decimals: 2, unit: '',
      tip: 'Pins the tonemapper instead of dimming the screen, so it composites correctly -- '
         + 'muzzle flashes still bloom and it survives a render. 0.10 night, 0.35 dusk, 1.0 neutral.'
    });
    // A named tile, not a loose button (Docs/panorama/panorama_ui_guide.md): a one-action set and a four-action set
    // read as the same kind of object.
    var expAutoGroup = actionGroup(todPage, 'Metering', [
      ['Auto exposure', function () { cmd('mirv_filmmaker world exposure auto quiet'); }]
    ], 2, 'Hands exposure back to the engine, which re-meters as the shot moves.');
    var expAutoBtn = expAutoGroup.btns[0];

    // ===================== SUN ==========================================
    // Tints every light the scene system submits, through World/LightFx.cpp. The same recovered
    // object pointer provides the live sun direction controls below; all writes are bounded and
    // the authored direction is restored on reset.
    var sunPage = litSec.__group('Sun');
    var LIGHT_TINTS = [
      ['default', '#00000000', '255 255 255'],
      ['warm',    '#ffb277ff', '255 178 119'],
      ['gold',    '#ffd27aff', '255 210 122'],
      ['rose',    '#ff9aa8ff', '255 154 168'],
      ['cool',    '#9ec6ffff', '158 198 255'],
      ['teal',    '#7fd8d0ff', '127 216 208'],
      ['violet',  '#b79affff', '183 154 255'],
      ['ember',   '#ff8a5cff', '255 138 92'],
      ['ash',     '#9aa3adff', '154 163 173']
    ];
    var lightCustomRgb = [255, 180, 120];
    var lightPicker = null;

    var lightGrid = swatchGrid(sunPage, LIGHT_TINTS, function (nameKey, rgb) {
      if (lightPicker) lightPicker.hide();
      cmd('mirv_filmmaker world light tint ' + rgb + ' quiet');
    }, {
      label: 'Sun colour',
      custom: true,
      onCustom: function () { if (lightPicker) lightPicker.show(); }
    });

    lightPicker = colorPicker(sunPage, 'CUSTOM SUN TINT', lightCustomRgb, function (c) {
      lightGrid.paintCustom(c);
      cmd('mirv_filmmaker world light tint ' + c[0] + ' ' + c[1] + ' ' + c[2] + ' quiet');
    });

    var lightAmount = sliderRow(sunPage, {
      label: 'Sun tint amount', cmd: 'mirv_filmmaker world light amount',
      min: 0, max: 1, decimals: 2, unit: '', offAtZero: true,
      tip: 'Blends from the map’s authored light at 0.00 to the chosen tint at 1.00. '
         + 'The tint never increases the light’s HDR ceiling, so it cannot become an exposure glow.'
    });
    // Shadow direction is applied to the validated scene-light direction field, and the visible
    // sun disc follows this physical direction. Independent Skybox yaw/pitch live on WEATHER & SKY.
    var lightYaw = sliderRow(sunPage, {
      label: 'Rotate', cmd: 'mirv_filmmaker world light yaw',
      min: 0, max: 360, decimals: 1, unit: '°',
      tip: 'Spins the sun around the sky like a disc on a table: same height, only its compass direction changes. Some maps keep their authored shadow direction; use Environment sunlight for movable custom shadows.'
    });
    var lightPitch = sliderRow(sunPage, {
      label: 'Tilt', cmd: 'mirv_filmmaker world light pitch',
      min: -90, max: 90, decimals: 1, unit: '°',
      tip: 'Raises (+) or lowers (-) the sun toward or away from overhead, keeping its compass direction. The visible sun follows; native cast-shadow rotation depends on the map.'
    });
    // RANGE 0.25..2.00 on purpose (the backend still clamps at 8.0). This is not a radius
    // parameter -- SkyGpuRotation scales the halo TRIANGLES away from the billboard centre, so
    // past roughly 2x the geometry visibly stretches and the sun stops reading as a sun.
    var sunGlowSize = sliderRow(sunPage, {
      label: 'Bloom size', cmd: 'mirv_filmmaker world sun glow_size',
      min: 0.25, max: 2.0, decimals: 2, unit: 'x',
      tip: 'Scales the sun halo footprint around the same billboard centre as the disc. Separate '
         + 'from glow brightness. Large values stretch the halo geometry, so this stops at 2x.'
    });

    // The four light/shadow switches are ONE chip row under one heading (Docs/features/filmmaker_config_panel.md).
    subHead(sunPage, 'Light sources & shadows');
    function nativeLightingOwned() {
      var e = envState(), c = envClimate();
      return !!(e && e.enabled);
    }
    var engineLightsToggle = togglePill(sunPage, 'Engine lights',
      'Direct lights submitted by CS2. Does not change either shadow switch or baked material lighting. A shadow cannot be visible without a light to shade.',
      function () { if (!nativeLightingOwned()) cmd('mirv_filmmaker world light enabled toggle'); });
    var shadowsToggle = togglePill(sunPage, 'Engine shadows',
      'All-on/off shortcut for both shadow layers below. Does not disable engine lights or baked material lighting.',
      function () { if (!nativeLightingOwned()) cmd('mirv_filmmaker world shadows ' + ((st.bakedShadows !== false || st.dynamicShadows !== false) ? 'off' : 'on')); });
    var bakedShadowsToggle = togglePill(sunPage, 'Baked shadows',
      'CS2’s baked-shadow layer. Some animated plants also depend on this engine layer; it is not the same as baked material lighting.',
      function () { if (!nativeLightingOwned()) cmd('mirv_filmmaker world bakedshadows toggle'); });
    var liveShadowsToggle = togglePill(sunPage, 'Live engine shadows',
      'CS2’s live cascades, separately from baked shadows. Turning off baked shadows can still remove map foliage casters.',
      function () { if (!nativeLightingOwned()) cmd('mirv_filmmaker world dynamicshadows toggle'); });
    var lightingOwnerNote = envNote(sunPage, '');
    var shadowState = lbl(sunPage, '', S.danger, 12);
    shadowState.hittest = false;
    shadowState.style.whiteSpace = 'normal'; shadowState.style.width = '100%';
    shadowState.style.paddingLeft = UI.line.padH + 'px'; shadowState.style.paddingRight = UI.line.padH + 'px';
    shadowState.style.marginTop = '9px';
    shadowState.visible = false;

    var sunHidden = false;
    var lightUniform = true;
    var sunGroup = actionGroup(sunPage, 'Sun disc', [
      ['Color only', function () {
        lightUniform = !lightUniform;
        cmd('mirv_filmmaker world light mode ' + (lightUniform ? 'uniform' : 'filtered'));
      }],
      ['Hide sun', function () {
        sunHidden = !sunHidden;
        cmd('mirv_filmmaker world sun visible ' + (sunHidden ? '0' : '1'));
      }]
    ], 2);
    var lightMode = sunGroup.btns[0], sunDrawBtn = sunGroup.btns[1];
    // The one thing the controls cannot say for themselves: what the two modes MEAN.
    var lightState = lbl(sunPage, '', S.hint, 12);
    lightState.hittest = false;
    lightState.style.whiteSpace = 'normal'; lightState.style.width = '100%';
    lightState.style.paddingLeft = UI.line.padH + 'px'; lightState.style.paddingRight = UI.line.padH + 'px'; lightState.style.marginTop = '9px';
    // Speaks only when the machinery is missing.
    var lightRotationState = lbl(sunPage, '', S.danger, 12);
    lightRotationState.hittest = false;
    lightRotationState.style.whiteSpace = 'normal'; lightRotationState.style.width = '100%';
    lightRotationState.style.paddingLeft = UI.line.padH + 'px'; lightRotationState.style.paddingRight = UI.line.padH + 'px';
    lightRotationState.style.marginTop = '7px';
    lightRotationState.visible = false;
)CFJS"
R"CFJS(
    // (There is no SHADOW MAP page. The mod's own directional shadow-map composite -- its
    //  Strength / Softness / Depth bias / Filter taps are not on this panel. This tab is for what
    //  the game already has. `mirv_filmmaker world customshadow` still exists on
    //  the console for anyone testing that code path.)

    // Local half of the footer's RESET (ConfigHudEffectsResetJs.h): only the open picker. Every
    // other control on this card mirrors back from C++, so the reset commands settle them.
    fxResetLocal.lighting = function () {
      if (lightPicker) lightPicker.hide();
    };

    renderHooks.push(function (st) {
      var v = (typeof st.exposure === 'number') ? st.exposure : -1;
      var isAuto = !(v > 0);

      // The value has no "off" position, so it parks at the neutral 1.0 while auto is active
      // rather than jumping to its minimum, which would read as "pinned very dark".
      expSlider.sync(isAuto ? 1.0 : v);
      setBtnOn(expAutoBtn, isAuto, S.accent);
      var adapting = st.exposureAdapt === true;

      var matched = '', matchedName = '';
      if (!isAuto) {
        for (var i = 0; i < TOD.length; i++) {
          if (Math.abs(TOD[i][2] - v) < 0.005) { matched = TOD[i][0]; matchedName = TOD[i][1]; break; }
        }
      }
      todGrid.sync(matched);

      // ---- scene lights ----
      lightAmount.sync(typeof st.lightAmount === 'number' ? st.lightAmount : 1);

      var lr = (typeof st.lightR === 'number') ? st.lightR : 255;
      var lg = (typeof st.lightG === 'number') ? st.lightG : 255;
      var lb = (typeof st.lightB === 'number') ? st.lightB : 255;
      var namedTint = '';
      for (var t = 0; t < LIGHT_TINTS.length; t++) {
        var p = LIGHT_TINTS[t][2].split(' ');
        if (+p[0] === lr && +p[1] === lg && +p[2] === lb) { namedTint = LIGHT_TINTS[t][0]; break; }
      }
      if (namedTint) lightGrid.select(namedTint);
      else {
        lightGrid.select('custom');
        // Not while the picker is open: rgbRef IS this array, so writing the round-tripped value
        // back would fight the drag in progress and leave the buffer one step behind -- which is
        // then what re-opening Custom commits. Same guard colorField already uses.
        if (!lightPicker || !lightPicker.visible()) {
          lightCustomRgb[0] = lr; lightCustomRgb[1] = lg; lightCustomRgb[2] = lb;
          lightGrid.paintCustom(lightCustomRgb);
        }
      }

      if (typeof st.lightUniform === 'boolean') {
        lightUniform = st.lightUniform;
        lightMode.__lbl.text = lightUniform ? 'Color only' : 'Filter / dim';
      }
      lightState.text = (st.lightOk === false)
        ? 'Unavailable on this CS2 build — the scene-light hook did not resolve.'
        : (lightUniform
            ? 'Color only keeps the sun’s authored peak brightness. Filter / dim multiplies it, so dark tints dim the disc.'
            : 'Filter / dim multiplies the authored RGB, so dark tints dim the disc and glow. Color only keeps its peak brightness.');
      lightState.style.color = (st.lightOk === false) ? S.danger : S.hint;

      lightYaw.sync(typeof st.sunYaw === 'number' ? st.sunYaw : 0);
      lightPitch.sync(typeof st.sunPitch === 'number' ? st.sunPitch : 0);
      sunGlowSize.sync(typeof st.sunGlowSize === 'number' ? st.sunGlowSize : 1);
      lightRotationState.visible = (st.lightRotationOk === false);
      lightRotationState.text = 'Sun direction is unavailable on this CS2 build.';

      sunHidden = st.sunVisible === false;
      sunDrawBtn.__lbl.text = sunHidden ? 'Show sun' : 'Hide sun';
      setBtnOn(sunDrawBtn, sunHidden, S.accent);
      setBtnOn(lightMode, !lightUniform, S.accent);

      var ownsNative = nativeLightingOwned();
      engineLightsToggle.set(st.engineLights !== false);
      bakedShadowsToggle.set(st.bakedShadows !== false);
      liveShadowsToggle.set(st.dynamicShadows !== false);
      shadowsToggle.set(st.bakedShadows !== false || st.dynamicShadows !== false);
      [engineLightsToggle, shadowsToggle, bakedShadowsToggle, liveShadowsToggle].forEach(function (control) {
        control.row.style.opacity = ownsNative ? '0.45' : '1';
      });
      lightingOwnerNote.visible = ownsNative;
      lightingOwnerNote.text = 'Environment owns dynamic lighting. Adjust Map lights, Ambient light and Cast shadows there.';
      shadowState.visible = (st.shadowsOk === false);
      shadowState.text = 'Shadow control is unavailable on this CS2 build.';

      // The card's summary: what it is pinned to, and where the sun points.
      var amt = (typeof st.lightAmount === 'number') ? st.lightAmount : 1;
      var tintName = namedTint || 'custom';
      litSec.__note((isAuto ? (adapting ? 'Eye adaptation' : 'Auto exposure') : (matchedName || ('Exposure ' + v.toFixed(2))))
        + ' · sun ' + Math.round(typeof st.sunYaw === 'number' ? st.sunYaw : 0) + '°'
        + ((tintName === 'default' || amt <= 0.001) ? '' : (' · ' + tintName + ' ' + Math.round(amt * 100) + '%')));
    });
)CFJS"
