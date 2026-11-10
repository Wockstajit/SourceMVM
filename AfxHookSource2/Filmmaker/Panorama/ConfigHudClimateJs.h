#pragma once

// Shared Panorama controls for Looks, Sky, Lighting and the single added fog.
R"CFJS(
    // ONE page, grouped (2026-10 sidebar cleanup): Presets, Color and Lens are ten rows together.
    var envLooksPage = envLooksSec.__group(T('Presets'));
    actionGroup(envLooksPage, T('Starting looks'), [
      [T('Storm'), function () { envCommand('look storm'); }],
      [T('Winter'), function () { envCommand('look winter'); }],
      [T('Sunset'), function () { envCommand('look sunset'); }],
      [T('Night'), function () { envCommand('look night'); }]
    ], 4, T('Sets the sky, fog and weather together. Save the result with the preset menu in this card’s header.'));
    var envGradeColour = envLooksSec.__group(T('Color'));
    envSceneSlider(envGradeColour, T('Exposure'), 'exposure', 0.05, 4, T('Brightness of the finished frame.'));
    cs(envGradeColour, 'saturation'); cs(envGradeColour, 'contrast');
    colorField(envGradeColour, T('Scene tint'), function () { var s = envState(); return s && {rgb:s.tint}; }, function (rgb) {
      ['tintR','tintG','tintB'].forEach(function (key,i) { envCommand('set '+key+' '+rgb[i]); });
    });
    var envGradeLens = envLooksSec.__group(T('Lens'));
    cs(envGradeLens, 'bloom'); cs(envGradeLens, 'vignette');
    cs(envGradeLens, 'grain'); cs(envGradeLens, 'chromatic');

    var envSkyPage = envSkySec.__group(T('Sun & sky'));
    cs(envSkyPage, 'sunAzimuth'); cs(envSkyPage, 'sunElevation'); cs(envSkyPage, 'sunPower');
    cs(envSkyPage, 'haze'); cs(envSkyPage, 'stars');
    colorField(envSkyPage, T('Sun color'), function () { var c=envClimate(); return c && {rgb:[c.sunColorR,c.sunColorG,c.sunColorB]}; }, function (rgb) {
      envCommand('climate enabled 1');
      ['sunColorR','sunColorG','sunColorB'].forEach(function (key,i) { envCommand('climate '+key+' '+rgb[i]); });
    }, 2);
    envNote(envSkyPage, T('Multiplies the sun. 1, 1, 1 is the default warm-to-white ramp; the same colour drives surface lighting, the sun disk, the sky glow and the bounce.'));
    var envCloudPage = envSkySec.__group(T('Clouds'));
    cw(envCloudPage, T('Clouds'), 'clouds', T('Clouds can be used with either the map sky or the added sky.'));
    cs(envCloudPage, 'coverage'); cs(envCloudPage, 'cloudBase'); cs(envCloudPage, 'windSpeed');

    // Today's one long page split three ways (Docs/features/filmmaker_config_panel.md): what lights the scene, how its
    // shadows look, and which map lights are on.
    var envRelightPage = envLightSec.__group(T('Light'));
    var envShadowPage = envLightSec.__group(T('Shadows'));
    var envMapLightPage = envLightSec.__group(T('Map lights'));
    // Eye adaptation (Unreal-style auto exposure): meters the frame and eases the tonemapper toward
    // it, so walking from a dark doorway into sunlight brightens, then settles.
    var envExposurePage = envLightSec.__group(T('Exposure'));
    var envAdaptToggle = togglePill(envExposurePage, T('Auto exposure'),
      T('Meters the scene and adjusts exposure over time, like a camera or an eye. A pinned exposure (Effects > Lighting time of day or Exposure slider) overrides it.'),
      function () {
        cmd('mirv_filmmaker world exposure adapt ' + (st.exposureAdapt ? 'off' : 'on') + ' quiet');
        if (!st.exposureAdapt) cmd('mirv_filmmaker world exposure auto quiet');
      });
    var envAdaptSpeed = sliderRow(envExposurePage, {
      label: T('Adapt speed'), cmd: 'mirv_filmmaker world exposure speed',
      min: 0.1, max: 20, decimals: 1, unit: ' st/s',
      tip: T('How fast exposure catches up, in stops per second. 1-3 feels like a film camera; 10+ is near-instant.')
    });
    var envAdaptComp = sliderRow(envExposurePage, {
      label: T('Compensation'), cmd: 'mirv_filmmaker world exposure compensation',
      min: -4, max: 4, decimals: 1, unit: ' st', centerTick: true,
      tip: T('Brightens (+) or darkens (-) the adapted exposure, in stops.')
    });
    var envAdaptNote = envNote(envExposurePage, '');
    cs(envRelightPage, 'mapLightPower'); cs(envRelightPage, 'ambient');
    cs(envRelightPage, 'bounce');
    cs(envRelightPage, 'bounceSteps');
    lw(envShadowPage, T('Cast shadows'), 'contactShadows', T('Sunlight and map lamps cast real-time shadows from geometry and players.'));
    lw(envShadowPage, T('Ambient occlusion'), 'ao', T('Darkens corners, creases and contact points where less light can reach.'));
    lw(envShadowPage, T('Sky occlusion'), 'skyOcclusion', T('Keeps sky light out of rooms, alleys and covered areas that cannot see the sky.'));
    subHead(envShadowPage, T('Shadow sharpness'));
    cs(envShadowPage, 'sunAngle'); cs(envShadowPage, 'lampAngle');
    cs(envShadowPage, 'shadowSoftness');
    actionGroup(envMapLightPage, T('Pick lights'), [
      [T('Edit lights'), function () { cmd('mirv_relight lighteditor on'); cmd('mirv_filmmaker config close'); }],
      [T('Turn all back on'), function () { cmd('mirv_relight lights reset'); }]
    ], 2, T('Edit lights shows a marker on every map light. Fly to a light, aim at it and press F to switch it off or back on; Esc finishes. Your choices are saved for this map.'));
    var lightPickStatus = envNote(envMapLightPage, '');
    var climateShadowStatus = envNote(envShadowPage, '');
    envNote(envRelightPage, T('Sun brightness is in Sky. Map lights illuminate rooms independently of ambient light.'));

    var envFogPage = envFogSec.__page(T('Fog'));
    subHead(envFogPage, T('Height fog'));
    cs(envFogPage, 'fogDensity'); cs(envFogPage, 'fogStart');
    cs(envFogPage, 'fogHeight'); cs(envFogPage, 'fogNoise');
    colorField(envFogPage, T('Height fog color'), function () { var s=envState(); return s && {rgb:s.fogColor}; }, function (rgb) {
      ['fogR','fogG','fogB'].forEach(function (key,i) { envCommand('set '+key+' '+rgb[i]); });
    });
    subHead(envFogPage, T('Ground fog'));
    // A look is only a starting point: every value it sets is a slider below, free to change from there.
    var GROUND_FOG_LOOKS = {
      mist: { climate: { fogDensity: 0.02, fogStart: 0, fogHeight: 500, fogNoise: 0.5, lightShafts: 1,
                         groundFog: 0.2, groundFogCoverage: 1, groundFogHeight: 70, groundFogDetail: 0.8,
                         groundFogBillowSize: 500, groundFogSoftness: 0.7, groundFogWisps: 1, groundFogWispSize: 45,
                         groundFogStreak: 0.8, groundFogChurn: 1.5, groundFogWind: 45, groundFogSun: 1, groundFogShade: 1.1,
                         groundFogColorR: 0.98, groundFogColorG: 0.98, groundFogColorB: 0.97,
                         groundFogShadeR: 0.95, groundFogShadeG: 0.97, groundFogShadeB: 1 },
              scene: { fogR: 0.86, fogG: 0.89, fogB: 0.93 } },
      sheet: { climate: { fogDensity: 0, lightShafts: 1,
                          groundFog: 1, groundFogCoverage: 1, groundFogHeight: 56, groundFogDetail: 0.6,
                          groundFogBillowSize: 220, groundFogSoftness: 0, groundFogWisps: 0.2, groundFogWispSize: 36,
                          groundFogStreak: 0, groundFogChurn: 1, groundFogWind: 12, groundFogSun: 1, groundFogShade: 1,
                          groundFogColorR: 0.93, groundFogColorG: 0.91, groundFogColorB: 0.87,
                          groundFogShadeR: 1, groundFogShadeG: 1, groundFogShadeB: 1 },
               scene: {} }
    };
    function applyGroundFogLook(look) {
      envEnsureEnabled();
      var key;
      for (key in look.climate) envCommand('climate ' + key + ' ' + look.climate[key]);
      for (key in look.scene) envCommand('set ' + key + ' ' + look.scene[key]);
    }
    actionGroup(envFogPage, T('Start from'), [
      [T('Street mist'), function () { applyGroundFogLook(GROUND_FOG_LOOKS.mist); }],
      [T('Thick sheet'), function () { applyGroundFogLook(GROUND_FOG_LOOKS.sheet); }]
    ], 2, T('Sets the sliders below in one go, as a place to start. Street mist is thin torn fog with a milky haze over the map, white in the sun and blue in shade. Thick sheet is one solid layer on the floor.'));
    cs(envFogPage, 'groundFog'); cs(envFogPage, 'groundFogCoverage'); cs(envFogPage, 'groundFogHeight');
    subHead(envFogPage, T('Ground fog shape'));
    cs(envFogPage, 'groundFogSoftness');
    cs(envFogPage, 'groundFogWisps'); cs(envFogPage, 'groundFogWispSize'); cs(envFogPage, 'groundFogStreak');
    cs(envFogPage, 'groundFogDetail'); cs(envFogPage, 'groundFogBillowSize');
    subHead(envFogPage, T('Ground fog motion'));
    cs(envFogPage, 'groundFogFlow'); cs(envFogPage, 'groundFogPushSize');
    cs(envFogPage, 'groundFogRecovery'); cs(envFogPage, 'groundFogSwirl');
    cs(envFogPage, 'groundFogWind'); cs(envFogPage, 'groundFogWindAngle'); cs(envFogPage, 'groundFogChurn');
    subHead(envFogPage, T('Ground fog light'));
    colorField(envFogPage, T('Ground fog color'), function () { var c=envClimate(); return c && {rgb:[c.groundFogColorR,c.groundFogColorG,c.groundFogColorB]}; }, function (rgb) {
      ['groundFogColorR','groundFogColorG','groundFogColorB'].forEach(function (key,i) { envCommand('climate '+key+' '+rgb[i]); });
    });
    cs(envFogPage, 'groundFogSun'); cs(envFogPage, 'groundFogShade');
    colorField(envFogPage, T('Shade color'), function () { var c=envClimate(); return c && {rgb:[c.groundFogShadeR,c.groundFogShadeG,c.groundFogShadeB]}; }, function (rgb) {
      ['groundFogShadeR','groundFogShadeG','groundFogShadeB'].forEach(function (key,i) { envCommand('climate '+key+' '+rgb[i]); });
    });
    // A test view, not part of the look: it never appears in an EXR.
    var envFogSimView = togglePill(envFogPage, T('Show sim view'), T('Shows the live fluid in a corner of the screen, from above. Dark is cleared fog, bright is heaped fog, blue is moving air, orange dots are players and green is the camera.'), function () {
      envCommand('groundfog-view toggle');
    });
    subHead(envFogPage, T('Light'));
    cw(envFogPage, T('Light shafts'), 'lightShafts', T('Sunlight through gaps in the map draws rays in both fogs, and buildings shade them. Uses the map’s own sun unless the Sky card moves it.'));

    renderHooks.push(function (st) {
      var c = envClimate(), s = envState();
      if (!c || !s) return;
      envSkySec.__note((c.enabled && (c.sky || c.clouds))
        ? ((c.sky ? T('Sun {0}°', Math.round(c.sunElevation)) : T('Map sky')) + (c.clouds ? ' · ' + T('clouds') : '')) : T('Map sky'));
      // Environment lighting is the Lighting switch (relighting), not the Environment pass as a
      // whole: weather or fog alone keep the map's own lighting.
      envLightSec.__note(s.enabled && c.enabled && c.relighting ? (c.contactShadows ? T('Dynamic lighting · shadows on') : T('Dynamic lighting · shadows off')) : T('Map lighting'));
      var fogParts = [];
      if (c.enabled && c.fogDensity > 0) fogParts.push(T('Height {0}', c.fogDensity.toFixed(2)));
      if (c.enabled && c.groundFog > 0) fogParts.push(T('ground {0}', c.groundFog.toFixed(2)));
      if (fogParts.length && c.lightShafts) fogParts.push(T('shafts'));
      envFogSec.__note(fogParts.length ? fogParts.join(' · ') : T('Off'));
      envFogSimView.set(st.groundFogView === true);
      envLooksSec.__note(T('Exposure {0} · saturation {1}', s.exposure.toFixed(2), c.saturation.toFixed(2)));
      climateShadowStatus.text = st.envOcclusionReady ? '' : (st.envOcclusionStatus || T('Map lighting data unavailable'));
      climateShadowStatus.visible = !!climateShadowStatus.text;
      lightPickStatus.text = st.lightEditor ? T('Editing lights: aim at a light and press F, Esc to finish.')
        : (st.lightsOff ? TN(st.lightsOff, '{0} light switched off on this map.', '{0} lights switched off on this map.') : T('Every map light is on.'));
      var adapting = st.exposureAdapt === true, pinned = typeof st.exposure === 'number' && st.exposure > 0;
      envAdaptToggle.set(adapting);
      envAdaptSpeed.sync(typeof st.exposureSpeed === 'number' ? st.exposureSpeed : 2);
      envAdaptComp.sync(typeof st.exposureCompensation === 'number' ? st.exposureCompensation : 0);
      envAdaptSpeed.row.style.opacity = adapting ? '1' : '0.45';
      envAdaptComp.row.style.opacity = adapting ? '1' : '0.45';
      envAdaptNote.text = adapting && pinned ? T('Exposure is pinned on Effects > Lighting, so adaptation is paused. Press Auto exposure there to resume.')
        : (adapting ? T('Adapting · exposure {0}', typeof st.exposureAdapted === 'number' ? st.exposureAdapted.toFixed(2) : '-') : '');
      envAdaptNote.visible = !!envAdaptNote.text;
      envPrepPaint(st);
    });
)CFJS"
