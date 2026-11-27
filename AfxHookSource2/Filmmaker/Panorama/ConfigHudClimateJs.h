#pragma once

// Shared Panorama controls for Looks, Sky, Lighting and the single added fog.
R"CFJS(
    // ONE page, grouped (2026-10 sidebar cleanup): Presets, Color and Lens are ten rows together.
    var envLooksPage = envLooksSec.__group(T('Presets'));
    presetActions(envLooksPage, T('Starting looks'), [
      [T('Storm'), function () { envCommand('look storm'); }],
      [T('Winter'), function () { envCommand('look winter'); }],
      [T('Sunset'), function () { envCommand('look sunset'); }],
      [T('Night'), function () { envCommand('look night'); }],
      [T('Sandstorm'), function () { envCommand('look sandstorm'); }]
    ]);
    var envGradeColour = envLooksSec.__group(T('Color'));
    envSceneSlider(envGradeColour, T('Exposure'), 'exposure', 0.05, 4, '');
    cs(envGradeColour, 'saturation'); cs(envGradeColour, 'contrast');
    colorField(envGradeColour, T('Scene tint'), function () { var s = envState(); return s && {rgb:s.tint}; }, function (rgb) {
      ['tintR','tintG','tintB'].forEach(function (key,i) { envCommand('set '+key+' '+rgb[i]); });
    });
    var envGradeLens = envLooksSec.__group(T('Lens'));
    cs(envGradeLens, 'bloom'); cs(envGradeLens, 'vignette');
    cs(envGradeLens, 'grain'); cs(envGradeLens, 'chromatic');

    // TIME OF DAY (2026-11): one slider moves the sun and the moon together, the way they move; the
    // sun's path and its noon height turn and tilt the day. The four angle sliders it replaces left
    // the moon wherever it was put whatever the sun did.
    var envSkyPage = envSkySec.__group(T('Sun & sky'));
    cs(envSkyPage, 'timeOfDay'); cs(envSkyPage, 'sunPath'); cs(envSkyPage, 'sunHeight'); cs(envSkyPage, 'sunPower');
    cs(envSkyPage, 'haze'); cs(envSkyPage, 'stars');
    colorField(envSkyPage, T('Sun color'), function () { var c=envClimate(); return c && {rgb:[c.sunColorR,c.sunColorG,c.sunColorB]}; }, function (rgb) {
      envCommand('climate enabled 1');
      ['sunColorR','sunColorG','sunColorB'].forEach(function (key,i) { envCommand('climate '+key+' '+rgb[i]); });
    }, 2, T('Multiplies the sun. White is the default warm-to-white ramp; the same colour drives surface lighting, the sun disk, the sky glow and the bounce.'));
    var envMoonPage = envSkySec.__group(T('Moon'));
    cs(envMoonPage, 'moon'); cs(envMoonPage, 'moonPhase'); cs(envMoonPage, 'moonSize');
    colorField(envMoonPage, T('Moon color'), function () { var c=envClimate(); return c && {rgb:[c.moonColorR,c.moonColorG,c.moonColorB]}; }, function (rgb) {
      envCommand('climate enabled 1'); envCommand('climate sky 1');
      ['moonColorR','moonColorG','moonColorB'].forEach(function (key,i) { envCommand('climate '+key+' '+rgb[i]); });
    }, 2, T('Once the sun is below the horizon the moon takes over as the light: the map is lit and shadowed from where the moon stands, in this colour.'));
    var envCloudPage = envSkySec.__group(T('Clouds'));
    cw(envCloudPage, T('Clouds'), 'clouds', T('Clouds can be used with either the map sky or the added sky.'));
    // Always shown (it used to show only with Clouds on and the added sky off, and could not be found).
    cw(envCloudPage, T('Hide map clouds'), 'hideMapClouds', T('Keeps the map’s own sky and hides only its own clouds. Turn Clouds on to put the cloud layer over it instead. With the added sky on, the map’s clouds are always hidden.'));
    cs(envCloudPage, 'coverage'); cs(envCloudPage, 'density'); cs(envCloudPage, 'cloudBase'); cs(envCloudPage, 'windSpeed');
    cs(envCloudPage, 'cloudRain');

    // Today's one long page split three ways (Docs/features/filmmaker_config_panel.md): what lights the scene, how its
    // shadows look, and which map lights are on.
    var envRelightPage = envLightSec.__group(T('Light'));
    var envShadowPage = envLightSec.__group(T('Shadows'));
    // THE ENVIRONMENT'S OWN AUTO EXPOSURE (climate autoExposure / exposureBias). It follows the relit
    // frame while Environment lighting is on and is separate from the in-game Auto exposure on
    // Effects > Lighting: neither switch reads or changes the other.
    var envExposurePage = envLightSec.__group(T('Exposure'));
    lw(envExposurePage, T('Auto exposure'), 'autoExposure',
      T('Meters the relit scene and settles the exposure, so Environment lighting is never left too bright or too dark for the map. Separate from Auto exposure on Effects > Lighting.'));
    cs(envExposurePage, 'exposureBias');
    var envAdaptNote = envNote(envExposurePage, '');
    cs(envRelightPage, 'mapLightPower'); cs(envRelightPage, 'ambient'); cs(envRelightPage, 'playerFill');
    lw(envRelightPage, T('Fill lights'), 'fillLights', T('Lights a map adds only to brighten a place, with no lamp of their own: lights that only add bounce, and panels or spots aimed up at arches and ceilings. Off removes them on every map.'));
    cs(envRelightPage, 'bounce');
    cs(envRelightPage, 'bounceSteps');
    lw(envShadowPage, T('Cast shadows'), 'contactShadows', T('Sunlight and map lamps cast real-time shadows from geometry and players.'));
    lw(envShadowPage, T('Ambient occlusion'), 'ao', T('Darkens corners, creases and contact points where less light can reach.'));
    lw(envShadowPage, T('Sky occlusion'), 'skyOcclusion', T('Keeps sky light out of rooms, alleys and covered areas that cannot see the sky.'));
    subHead(envShadowPage, T('Shadow sharpness'));
    cs(envShadowPage, 'sunAngle'); cs(envShadowPage, 'lampAngle');
    cs(envShadowPage, 'shadowSoftness');
    var climateShadowStatus = envNote(envShadowPage, '');

    // TWO PAGES (2026-11). The card had grown to thirty rows, most of them ground fog's, and scrolled
    // a page and a half. The height fog and the light both fogs share stay on the first page; ground
    // fog, a fluid with its own shape, motion and light, has the second.
    var envFogPage = envFogSec.__page(T('Fog'));
    var envGroundFogPage = envFogSec.__page(T('Ground fog'));
    subHead(envFogPage, T('Height fog'));
    cw(envFogPage, T('Half-Life: Alyx fog'), 'alyxFog', T('Switches height fog to Half-Life: Alyx volumetric fog with near clarity fade-in, counter-drifting 3D air currents, forward scatter and sky bounce. Weather presets also use this fog while enabled.'));
    cs(envFogPage, 'fogDensity'); cs(envFogPage, 'fogStart'); cs(envFogPage, 'fogRange');
    cs(envFogPage, 'fogHeight'); cs(envFogPage, 'fogNoise');
    colorField(envFogPage, T('Height fog color'), function () { var s=envState(); return s && {rgb:s.fogColor}; }, function (rgb) {
      ['fogR','fogG','fogB'].forEach(function (key,i) { envCommand('set '+key+' '+rgb[i]); });
    });
    subHead(envFogPage, T('Light'));
    cw(envFogPage, T('Light shafts'), 'lightShafts', T('Sunlight through gaps in the map draws rays in the height fog, and buildings shade it. Uses the map’s own sun unless the Sky card moves it. Ground fog has its own Shadows slider.'));
    cs(envFogPage, 'fogLightIntensity'); cs(envFogPage, 'scatter');
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
    presetActions(envGroundFogPage, T('Start from'), [
      [T('Street mist'), function () { applyGroundFogLook(GROUND_FOG_LOOKS.mist); }],
      [T('Thick sheet'), function () { applyGroundFogLook(GROUND_FOG_LOOKS.sheet); }]
    ]);
    // GROUND FOG'S OWN SWITCH (2026-11-18). On and off without dragging the density down: off holds
    // the density (pass-off, as the card switches do) and every setting below, on brings them back (1
    // the first time). The card's header switch is the height fog's alone.
    var envGroundFogMemo = null;
    var envGroundFogSwitch = togglePill(envGroundFogPage, T('Ground fog'), T('Turns the ground fog on and off. Off keeps its density and every setting below; on brings them back. The switch in the card header is the height fog.'), function () {
      var c = envClimate();
      if (!c) return;
      if (c.enabled && c.groundFog > 0) {
        envGroundFogMemo = c.groundFog;
        envCommand('pass-off groundFog 0');
        envReleaseIfIdle({ groundFog: 0 });
      } else {
        envEnsureEnabled();
        var held = (envState() || {}).held || {};
        envCommand('climate groundFog ' + (envGroundFogMemo || held.groundFog || 1));
      }
    });
    cs(envGroundFogPage, 'groundFog'); cs(envGroundFogPage, 'groundFogCoverage'); cs(envGroundFogPage, 'groundFogHeight');
    subHead(envGroundFogPage, T('Shape'));
    cs(envGroundFogPage, 'groundFogSoftness');
    cs(envGroundFogPage, 'groundFogWisps'); cs(envGroundFogPage, 'groundFogWispSize'); cs(envGroundFogPage, 'groundFogStreak');
    cs(envGroundFogPage, 'groundFogDetail'); cs(envGroundFogPage, 'groundFogBillowSize');
    subHead(envGroundFogPage, T('Light'));
    colorField(envGroundFogPage, T('Ground fog color'), function () { var c=envClimate(); return c && {rgb:[c.groundFogColorR,c.groundFogColorG,c.groundFogColorB]}; }, function (rgb) {
      ['groundFogColorR','groundFogColorG','groundFogColorB'].forEach(function (key,i) { envCommand('climate '+key+' '+rgb[i]); });
    });
    cs(envGroundFogPage, 'groundFogSun'); cs(envGroundFogPage, 'groundFogShade'); cs(envGroundFogPage, 'groundFogShadows');
    colorField(envGroundFogPage, T('Shade color'), function () { var c=envClimate(); return c && {rgb:[c.groundFogShadeR,c.groundFogShadeG,c.groundFogShadeB]}; }, function (rgb) {
      ['groundFogShadeR','groundFogShadeG','groundFogShadeB'].forEach(function (key,i) { envCommand('climate '+key+' '+rgb[i]); });
    });
    subHead(envGroundFogPage, T('Motion'));
    cs(envGroundFogPage, 'groundFogFlow'); cs(envGroundFogPage, 'groundFogPushSize');
    cs(envGroundFogPage, 'groundFogRecovery'); cs(envGroundFogPage, 'groundFogSwirl');
    cs(envGroundFogPage, 'groundFogWind'); cs(envGroundFogPage, 'groundFogWindAngle'); cs(envGroundFogPage, 'groundFogChurn');
    // A test view, not part of the look: it never appears in an EXR.
    var envFogSimView = togglePill(envGroundFogPage, T('Show sim view'), T('Shows the live fluid in a corner of the screen, from above. Dark is cleared fog, bright is heaped fog, blue is moving air, orange dots are players and green is the camera.'), function () {
      envCommand('groundfog-view toggle');
    });

    renderHooks.push(function (st) {
      var c = envClimate(), s = envState();
      if (!c || !s) return;
      envSkySec.__note((c.enabled && (c.sky || c.clouds))
        ? ((c.sky ? envClock(c.timeOfDay || 0) + (c.sunElevation >= 0 ? ' · ' + T('sun {0}°', Math.round(c.sunElevation)) : c.moonElevation > 0 && c.moon > 0 ? ' · ' + T('moon up') : ' · ' + T('night')) : T('Map sky')) + (c.clouds ? ' · ' + T('clouds') : '')) : T('Map sky'));
      // Environment lighting is the Lighting switch (relighting), not the Environment pass as a
      // whole: weather or fog alone keep the map's own lighting.
      envLightSec.__note(s.enabled && c.enabled && c.relighting ? (c.contactShadows ? T('Dynamic lighting · shadows on') : T('Dynamic lighting · shadows off')) : T('Map lighting'));
      var fogParts = [];
      if (c.enabled && c.fogDensity > 0) fogParts.push(c.alyxFog ? T('Alyx {0}', c.fogDensity.toFixed(2)) : T('Height {0}', c.fogDensity.toFixed(2)));
      if (c.enabled && c.groundFog > 0) fogParts.push(T('ground {0}', c.groundFog.toFixed(2)));
      if (fogParts.length && c.lightShafts) fogParts.push(T('shafts'));
      envFogSec.__note(fogParts.length ? fogParts.join(' · ') : T('Off'));
      envFogSimView.set(st.groundFogView === true);
      envGroundFogSwitch.set(!!(c.enabled && c.groundFog > 0));
      envLooksSec.__note(T('Exposure {0} · saturation {1}', s.exposure.toFixed(2), c.saturation.toFixed(2)));
      climateShadowStatus.text = st.envOcclusionReady ? '' : (st.envOcclusionStatus || T('Map lighting data unavailable'));
      climateShadowStatus.visible = !!climateShadowStatus.text;
      lightPickStatus.text = st.lightEditor ? T('Editing lights: aim at a light and press F, Esc to finish.')
        : (st.lightsOff ? TN(st.lightsOff, '{0} light switched off on this map.', '{0} lights switched off on this map.') : T('Every map light is on.'));
      // The Environment's own adaptation: live while its lighting has the map and nothing pins exposure.
      var envAdapting = !!(s.enabled && c.enabled && c.relighting && c.autoExposure), pinned = typeof st.exposure === 'number' && st.exposure > 0;
      envAdaptNote.text = envAdapting && pinned ? T('Exposure is pinned on Effects > Lighting, so this is paused. Press Auto exposure there to release it.')
        : (envAdapting ? T('Adapting · exposure {0}', typeof st.exposureAdapted === 'number' && st.exposureAdapted > 0 ? st.exposureAdapted.toFixed(2) : '-') : '');
      envAdaptNote.visible = !!envAdaptNote.text;
      envPrepPaint(st);
    });
)CFJS"
