#pragma once

// Shared Panorama controls for Looks, Sky, Lighting and the single added fog.
R"CFJS(
    var envLooksPage = envLooksSec.__page('Presets');
    actionGroup(envLooksPage, 'Starting looks', [
      ['Storm', function () { envCommand('look storm'); }],
      ['Winter', function () { envCommand('look winter'); }],
      ['Sunset', function () { envCommand('look sunset'); }],
      ['Night', function () { envCommand('look night'); }]
    ], 2, 'Sets the sky, fog and weather together. Save the result with the preset button on Scene Objects.');
    var envGradeColour = envLooksSec.__page('Color');
    envSceneSlider(envGradeColour, 'Exposure', 'exposure', 0.05, 4, 'Brightness of the finished frame.');
    cs(envGradeColour, 'saturation'); cs(envGradeColour, 'contrast');
    colorField(envGradeColour, 'Scene tint', function () { var s = envState(); return s && {rgb:s.tint}; }, function (rgb) {
      ['tintR','tintG','tintB'].forEach(function (key,i) { envCommand('set '+key+' '+rgb[i]); });
    });
    var envGradeLens = envLooksSec.__page('Lens');
    cs(envGradeLens, 'bloom'); cs(envGradeLens, 'vignette');
    cs(envGradeLens, 'grain'); cs(envGradeLens, 'chromatic');

    var envSkyPage = envSkySec.__page('Sun & sky');
    cs(envSkyPage, 'sunAzimuth'); cs(envSkyPage, 'sunElevation'); cs(envSkyPage, 'sunPower');
    cs(envSkyPage, 'haze'); cs(envSkyPage, 'stars');
    colorField(envSkyPage, 'Sun color', function () { var c=envClimate(); return c && {rgb:[c.sunColorR,c.sunColorG,c.sunColorB]}; }, function (rgb) {
      envCommand('climate enabled 1');
      ['sunColorR','sunColorG','sunColorB'].forEach(function (key,i) { envCommand('climate '+key+' '+rgb[i]); });
    }, 2);
    envNote(envSkyPage, 'Multiplies the sun. 1, 1, 1 is the default warm-to-white ramp; the same colour drives surface lighting, the sun disk, the sky glow and the bounce.');
    var envCloudPage = envSkySec.__page('Clouds');
    cw(envCloudPage, 'Clouds', 'clouds', 'Clouds can be used with either the map sky or the added sky.');
    cs(envCloudPage, 'coverage'); cs(envCloudPage, 'cloudBase'); cs(envCloudPage, 'windSpeed');

    // Today's one long page split three ways (Docs/features/filmmaker_config_panel.md): what lights the scene, how its
    // shadows look, and which map lights are on.
    var envRelightPage = envLightSec.__page('Light');
    var envShadowPage = envLightSec.__page('Shadows');
    var envMapLightPage = envLightSec.__page('Map lights');
    // Eye adaptation (Unreal-style auto exposure): meters the frame and eases the tonemapper toward
    // it, so walking from a dark doorway into sunlight brightens, then settles.
    var envExposurePage = envLightSec.__page('Exposure');
    var envAdaptToggle = togglePill(envExposurePage, 'Auto exposure',
      'Meters the scene and adjusts exposure over time, like a camera or an eye. A pinned exposure (Effects > Lighting time of day or Exposure slider) overrides it.',
      function () {
        cmd('mirv_filmmaker world exposure adapt ' + (st.exposureAdapt ? 'off' : 'on') + ' quiet');
        if (!st.exposureAdapt) cmd('mirv_filmmaker world exposure auto quiet');
      });
    var envAdaptSpeed = sliderRow(envExposurePage, {
      label: 'Adapt speed', cmd: 'mirv_filmmaker world exposure speed',
      min: 0.1, max: 20, decimals: 1, unit: ' st/s',
      tip: 'How fast exposure catches up, in stops per second. 1-3 feels like a film camera; 10+ is near-instant.'
    });
    var envAdaptComp = sliderRow(envExposurePage, {
      label: 'Compensation', cmd: 'mirv_filmmaker world exposure compensation',
      min: -4, max: 4, decimals: 1, unit: ' st', centerTick: true,
      tip: 'Brightens (+) or darkens (-) the adapted exposure, in stops.'
    });
    var envAdaptNote = envNote(envExposurePage, '');
    cs(envRelightPage, 'mapLightPower'); cs(envRelightPage, 'ambient');
    cs(envRelightPage, 'bounce');
    cs(envRelightPage, 'bounceSteps');
    lw(envShadowPage, 'Cast shadows', 'contactShadows', 'Sunlight and map lamps cast real-time shadows from geometry and players.');
    lw(envShadowPage, 'Ambient occlusion', 'ao', 'Darkens corners, creases and contact points where less light can reach.');
    lw(envShadowPage, 'Sky occlusion', 'skyOcclusion', 'Keeps sky light out of rooms, alleys and covered areas that cannot see the sky.');
    subHead(envShadowPage, 'Shadow sharpness');
    cs(envShadowPage, 'sunAngle'); cs(envShadowPage, 'lampAngle');
    cs(envShadowPage, 'shadowSoftness');
    actionGroup(envMapLightPage, 'Pick lights', [
      ['Edit lights', function () { cmd('mirv_relight lighteditor on'); cmd('mirv_filmmaker config close'); }],
      ['Turn all back on', function () { cmd('mirv_relight lights reset'); }]
    ], 2, 'Edit lights shows a marker on every map light. Fly to a light, aim at it and press F to switch it off or back on; Esc finishes. Your choices are saved for this map.');
    var lightPickStatus = envNote(envMapLightPage, '');
    var climateShadowStatus = envNote(envShadowPage, '');
    envNote(envRelightPage, 'Sun brightness is in Sky. Map lights illuminate rooms independently of ambient light.');

    var envFogPage = envFogSec.__page('Fog');
    cs(envFogPage, 'fogDensity'); cs(envFogPage, 'fogStart');
    cs(envFogPage, 'fogHeight'); cs(envFogPage, 'fogNoise');
    colorField(envFogPage, 'Fog color', function () { var s=envState(); return s && {rgb:s.fogColor}; }, function (rgb) {
      ['fogR','fogG','fogB'].forEach(function (key,i) { envCommand('set '+key+' '+rgb[i]); });
    });

    renderHooks.push(function (st) {
      var c = envClimate(), s = envState();
      if (!c || !s) return;
      envSkySec.__note((c.enabled && (c.sky || c.clouds))
        ? ((c.sky ? 'Sun ' + Math.round(c.sunElevation) + '°' : 'Map sky') + (c.clouds ? ' · clouds' : '')) : 'Map sky');
      // Environment lighting is the Lighting switch (relighting), not the Environment pass as a
      // whole: weather or fog alone keep the map's own lighting.
      envLightSec.__note(s.enabled && c.enabled && c.relighting ? 'Dynamic lighting · ' + (c.contactShadows ? 'shadows on' : 'shadows off') : 'Map lighting');
      envFogSec.__note(c.enabled && c.fogDensity > 0
        ? 'Density ' + c.fogDensity.toFixed(2) + ' · starts at ' + Math.round(c.fogStart || 0) + 'u' : 'Off');
      envLooksSec.__note('Exposure ' + s.exposure.toFixed(2) + ' · saturation ' + c.saturation.toFixed(2));
      climateShadowStatus.text = st.envOcclusionReady ? '' : (st.envOcclusionStatus || 'Map lighting data unavailable');
      climateShadowStatus.visible = !!climateShadowStatus.text;
      lightPickStatus.text = st.lightEditor ? 'Editing lights: aim at a light and press F, Esc to finish.'
        : (st.lightsOff ? st.lightsOff + (st.lightsOff === 1 ? ' light' : ' lights') + ' switched off on this map.' : 'Every map light is on.');
      var adapting = st.exposureAdapt === true, pinned = typeof st.exposure === 'number' && st.exposure > 0;
      envAdaptToggle.set(adapting);
      envAdaptSpeed.sync(typeof st.exposureSpeed === 'number' ? st.exposureSpeed : 2);
      envAdaptComp.sync(typeof st.exposureCompensation === 'number' ? st.exposureCompensation : 0);
      envAdaptSpeed.row.style.opacity = adapting ? '1' : '0.45';
      envAdaptComp.row.style.opacity = adapting ? '1' : '0.45';
      envAdaptNote.text = adapting && pinned ? 'Exposure is pinned on Effects > Lighting, so adaptation is paused. Press Auto exposure there to resume.'
        : (adapting ? 'Adapting · exposure ' + (typeof st.exposureAdapted === 'number' ? st.exposureAdapted.toFixed(2) : '-') : '');
      envAdaptNote.visible = !!envAdaptNote.text;
      envPrepPaint(st);
    });
)CFJS"
