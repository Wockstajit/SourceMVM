#pragma once

// Weather, scene outliner and focused object inspector. Shared with the browser mock.

R"CFJS(
    function envSelected() {
      var s = envState();
      if (!s) return null;
      for (var i = 0; i < s.objects.length; i++) if (s.objects[i].id === st.envSelected) return s.objects[i];
      return null;
    }

    // ===================== PRECIPITATION ================================
    // The `weather` backend, not a climate value: a particle field with its own type, volume and
    // collision. The Detail page below IS climate, because those keys shade the same particles.
    var WXD = {
      count:  ['Intensity', 0, 100, 0, '%', true, 'Amount of rain, snow or other particles.'],
      speed:  ['Fall speed', 0, 1500, 0, 'u/s', false, 'How fast they fall.'],
      size:   ['Particle size', 0.1, 12, 1, '', false, 'Size of each particle.'],
      windX:  ['Wind X', -500, 500, 0, 'u/s', false, 'Sideways drift of the precipitation.'],
      windY:  ['Wind Y', -500, 500, 0, 'u/s', false, 'Forward drift of the precipitation.'],
      radius: ['Field radius', 64, 4096, 0, 'u', false, 'How wide the precipitation volume is.'],
      height: ['Field height', 64, 4096, 0, 'u', false, 'How tall the precipitation volume is.']
    };
    var WX_DEFAULT = { count: 1800, speed: 600, size: 1, windX: 40, windY: 0, radius: 900, height: 900 };
    var envWeatherControls = [];
    function ws(parent, key) {
      var d = WXD[key];
      var control = sliderRow(parent, {
        label: d[0], min: d[1], max: d[2], decimals: d[3], unit: d[4], offAtZero: d[5], tip: d[6],
        onChange: function (v) { envEnsureEnabled(); envCommand('weather set ' + key + ' ' + (key === 'count' ? Math.round(v * 60) : v)); }
      });
      envWeatherControls.push([control, key]);
      return control;
    }

    var envWeatherResetCmds = ['mirv_filmmaker environment weather clear'];
    for (var wxk in WX_DEFAULT) {
      envWeatherResetCmds.push('mirv_filmmaker environment weather set ' + wxk + ' ' + WX_DEFAULT[wxk]);
    }
    envWeatherResetCmds.push('mirv_filmmaker environment weather set follow 1');
    envWeatherResetCmds.push('mirv_filmmaker environment weather set shelter 1');
    // envResetCmds returns a SPEC ({ off, cmds, after }), not a flat array -- a reset goes off
    // before it goes back to defaults. Merge the two rather than concatenating, or the spec
    // object lands in the command list and gets sent as "[object Object]".
    // Lightning lives on this card, so its Reset owns the storm keys too.
    // Surface coatings are part of this card too (rain and wetness are one effect), so its Reset
    // owns their keys as well.
    var ENV_WX_KEYS = ['particleDetail', 'turbulence', 'particleOpacity', 'softness', 'streak', 'particleLight',
       'storm', 'stormInterval', 'stormStrength', 'stormRadius', 'stormX', 'stormY', 'stormZ',
       'stormSeed', 'stormHold',
       'snow', 'wet', 'puddles', 'ripple', 'puddleHeight', 'puddleDepth', 'maskScale', 'surfaceRadius',
       'reflectionRoughness', 'puddleNative'];
    var envWeatherClimate = envResetCmds(ENV_WX_KEYS);
    var envWeatherReset = envWeatherSec.__headerReset({
      // Weather's own "off" is the Clear type, which the first command already does.
      off: envWeatherClimate.off,
      cmds: envWeatherResetCmds.concat(envWeatherClimate.cmds),
      after: function () {
        envAfterReset(ENV_WX_KEYS, { weatherType: 0 });
        envWeatherMemo = null;
        envWeatherArmed = false;
      }
    }, 'env.wx');

    // What a weather type does to the ground when it starts on dry ground.
    var WX_SURFACE = {
      rain: { snow: 0, wet: 0.6, puddles: 0.25 },
      snow: { snow: 0.6, wet: 0, puddles: 0 },
      ash:  { snow: 0, wet: 0, puddles: 0 }
    };
    function envSurfacesDry() {
      var c = envClimate();
      return !c || !(c.snow > 0 || c.wet > 0 || c.puddles > 0 || c.storm > 0);
    }
    // Picking a weather starts its surfaces too, unless surfaces are already set up by hand.
    function envChooseWeather(k) {
      var dry = envSurfacesDry();
      envEnsureEnabled();
      envCommand('weather ' + k);
      var look = WX_SURFACE[k];
      if (look && dry)
        for (var key in look) envCommand('climate ' + key + ' ' + look[key]);
    }

    // The card's pass switch covers the particles AND the surfaces. Off remembers the running type
    // and the coverages; on puts them back. The FIRST time on, it starts CLEAR with everything off
    // (no rain, dry ground, no lightning): it used to start rain on wet ground, which is why turning
    // Weather on "turned on rain on some maps" -- the first time on each one. `envWeatherArmed`
    // keeps the card on while nothing is chosen yet, so its controls stay live.
    var envWeatherMemo = null;
    var envWeatherArmed = false;
    function envWeatherOn() {
      var s = envState(), c = envClimate();
      if (!s) return false;
      return envWeatherArmed || !!s.weather.type ||
        !!(c && c.enabled && (c.snow > 0 || c.wet > 0 || c.puddles > 0 || c.storm > 0));
    }
    var envWeatherPill = envWeatherSec.__headerPill(function () {
      var s = envState(), c = envClimate();
      if (!s || !c) return;
      if (envWeatherOn()) {
        envWeatherMemo = {
          type: ['clear', 'rain', 'snow', 'ash', 'embers', 'motes', 'smoke'][s.weather.type] || 'clear',
          snow: c.snow || 0, wet: c.wet || 0, puddles: c.puddles || 0, storm: c.storm || 0
        };
        envWeatherArmed = false;
        envCommand('weather clear');
        envCommand('climate snow 0'); envCommand('climate wet 0'); envCommand('climate puddles 0');
        envCommand('climate storm 0');
        envReleaseIfIdle({ weatherType: 0, snow: 0, wet: 0, puddles: 0, storm: 0 });
      } else {
        var m = envWeatherMemo;
        // No switch-off this session: a restart held the saved weather instead (HoldRestoredPasses).
        var held = s.held || {};
        if (!m && (held.weatherType || held.snow || held.wet || held.puddles || held.storm)) {
          envEnsureEnabled();
          // By number, not by name: `weather rain` re-applies the rain preset's count and speed.
          if (held.weatherType) envCommand('weather set type ' + held.weatherType);
          envCommand('climate snow ' + (held.snow || 0)); envCommand('climate wet ' + (held.wet || 0));
          envCommand('climate puddles ' + (held.puddles || 0)); envCommand('climate storm ' + (held.storm || 0));
          return;
        }
        if (!m || (m.type === 'clear' && !m.snow && !m.wet && !m.puddles && !m.storm)) {
          envWeatherArmed = true; // on, clear: pick a weather or a ground coating from here
          envEnsureEnabled();
          return;
        }
        envEnsureEnabled();
        if (m.type !== 'clear') envCommand('weather ' + m.type);
        envCommand('climate snow ' + m.snow); envCommand('climate wet ' + m.wet);
        envCommand('climate puddles ' + m.puddles); envCommand('climate storm ' + m.storm);
      }
    }, 'Switches the weather and its wet or snowy ground off together, and puts both back.');

    // ONE page, grouped (2026-10 sidebar cleanup): Weather, Ground and Lightning are twelve rows.
    var envWxType = envWeatherSec.__group('Weather');
    var envWeatherModes = modeRow(envWxType, 'Weather',
      [['clear', 'Clear'], ['rain', 'Rain'], ['snow', 'Snow'], ['ash', 'Ash']], '236px',
      envChooseWeather,
      'The precipitation the field emits. Rain and Snow also start wet or snowy ground if the '
      + 'Ground page is untouched. Clear stops the particles only.', 13);
    ws(envWxType, 'count'); ws(envWxType, 'speed');
    var envWindDirection = 0;
    var envWindSpeedControl = sliderRow(envWxType, {
      label: 'Wind speed', min: 0, max: 500, decimals: 0, unit: 'u/s',
      tip: 'How strongly the wind pushes the weather.',
      onChange: function (v) {
        envEnsureEnabled();
        envCommand('weather wind ' + v + ' ' + envWindDirection);
        // The same wind sways placed trees and grass (Foliage wind below follows): on Clear there
        // is no precipitation for it to push, and the slider looked like it did nothing.
        cmd('mirv_filmmaker props wind ' + Math.round(Math.min(200, v * 0.4)));
      }
    });
    var envWindDirectionControl = sliderRow(envWxType, {
      label: 'Wind direction', min: 0, max: 360, decimals: 0, unit: '°',
      tip: 'Compass direction of the wind.',
      onChange: function (v) {
        var w = envState().weather; envWindDirection = v;
        envEnsureEnabled();
        envCommand('weather wind ' + Math.sqrt(w.windX * w.windX + w.windY * w.windY) + ' ' + v);
      }
    });
    // Foliage wind: CS2 sways placed trees and grass with the level's wind, and most maps have none,
    // so this adds one (Filmmaker/Props, `props wind`). The trees move while the demo plays.
    var envFoliageWind = sliderRow(envWxType, {
      label: 'Foliage wind', min: 0, max: 200, decimals: 0, unit: '',
      tip: 'How hard the wind sways placed trees, plants and grass. 0 leaves them to their own gentle sway. They move while the demo plays.',
      onChange: function (v) { cmd('mirv_filmmaker props wind ' + v); }
    });
    renderHooks.push(function (st) { if (st && st.props) envFoliageWind.sync(st.props.wind || 0); });
    var envCoatPage = envWeatherSec.__group('Ground');
    cs(envCoatPage, 'wet'); cs(envCoatPage, 'puddles'); cs(envCoatPage, 'snow');
    var envStormPage = envWeatherSec.__group('Lightning');
    cw(envStormPage, 'Lightning', 'storm', 'Lightning channels, cloud flashes and illuminated surfaces.');
    cs(envStormPage, 'stormStrength'); cs(envStormPage, 'stormInterval');
)CFJS"
#include "ConfigHudHeatmapJs.h"
#include "ConfigHudOutlinerJs.h"
#include "ConfigHudPropsJs.h"
R"CFJS(
    // The light and mesh inspectors live in the Outliner's selected-object card (olLightHost,
    // olMeshHost). The old Lights page and its LIT BY cards are gone: the tree's footer line says
    // what draws the lights, and the add-light row sits above the card's tabs.
    var envObjectLists = {}, envInspectors = {}, envObjectControls = {};

    // Explanations live on hover (Docs/panorama/panorama_ui_guide.md), so every tile in the inspector carries one.
    // Keyed by the object field rather than repeated at ~35 call sites.
    var OBJ_TIPS = {
      pitch: 'Rotation about the object’s side axis.',
      yaw: 'Rotation about the object’s vertical axis.',
      roll: 'Rotation about the object’s forward axis.',
      sx: 'Half-width of the prop along X, before rotation.',
      sy: 'Half-depth of the prop along Y, before rotation.',
      sz: 'Half-height of the prop along Z, before rotation.',
      roughness: 'How diffuse the surface is. 0 is a mirror, 1 is fully matte.',
      metallic: 'Blends between a dielectric and a metal response.',
      coat: 'Strength of a clear lacquer layer over the base material.',
      coatRoughness: 'How sharp the clear coat’s own reflections are.',
      anisotropy: 'Stretches the highlight, as on brushed metal. Negative flips the direction.',
      emission: 'How much light the prop gives off. Emissive props feed the bounce volume.',
      normalStrength: 'How strongly the normal map perturbs the surface.',
      uvScale: 'How many times the texture repeats across the prop.',
      intensity: 'Brightness of the light.',
      range: 'How far the light reaches before it falls off to nothing.',
      cone: 'Spot and area lights: the full angle of the beam.',
      width: 'Area lights: width of the emitting rectangle.',
      height: 'Area lights: height of the emitting rectangle.',
      sourceRadius: 'Softens the light’s shadow. Large values can soften fine blockers away.',
      patternScale: 'Size of the projected pattern on the surfaces it falls on.',
      rate: 'How fast the cue repeats, or how fast caustics travel.',
      amount: 'How far the cue swings from the light’s base intensity.',
      phase: 'Offsets the cue in time, so two lights do not pulse together.',
      r: 'Red channel of the object’s colour.',
      g: 'Green channel of the object’s colour.',
      b: 'Blue channel of the object’s colour.'
    };

    // ===================== SCENE OBJECTS > SELECTED =====================
    // One inspector per class of object, in the Outliner's card; only the one matching the
    // selection is visible. Every control is wrapped in its own host panel because the render hook
    // shows and hides them one at a time by object kind -- a point light has no direction, only an
    // area light has a width. One slider per row, the card's actions last.
    ['props', 'lights'].forEach(function (group) {
      var inspector = mk('Panel', group === 'lights' ? olLightHost : olMeshHost);
      inspector.style.width = '100%'; inspector.style.flowChildren = 'down'; inspector.style.marginTop = '6px';
      envInspectors[group] = inspector; envObjectControls[group] = [];
      inspector.__controlRows = [];
      var controlParent = inspector;
      function control(label, key, min, max, decimals, unit, tip) {
        var host = mk('Panel', controlParent);
        host.style.width = '100%'; host.style.flowChildren = 'down';
        var c = sliderRow(host, {
          label: label, min: min, max: max, decimals: decimals || 0, unit: unit || '',
          tip: tip || OBJ_TIPS[key],
          onChange: function (v) {
            var o = envSelected();
            if (!o) return;
            if (group === 'props'
                && ['roughness', 'metallic', 'coat', 'coatRoughness', 'anisotropy', 'emission',
                    'normalStrength', 'uvScale'].indexOf(key) >= 0 && !o.materialEnabled) return;
            envCommand('object ' + o.id + ' ' + key + ' ' + v);
          }
        });
        envObjectControls[group].push([c, key, host]);
        return c;
      }
      control('Pitch', 'pitch', -180, 180, 0, '°');
      control('Yaw', 'yaw', -180, 180, 0, '°');
      control('Roll', 'roll', -180, 180, 0, '°');
)CFJS"
R"CFJS(
      if (group === 'props') {
        control('Half width', 'sx', 0.1, 512, 1, 'u');
        control('Half depth', 'sy', 0.1, 512, 1, 'u');
        control('Half height', 'sz', 0.1, 512, 1, 'u');

        var cameraVisibility = mk('Panel', inspector);
        cameraVisibility.style.width = '100%'; cameraVisibility.style.flowChildren = 'down';
        inspector.__cameraVisibilitySection = cameraVisibility;
        inspector.__cameraVisible = modeRow(cameraVisibility, 'Seen by the camera',
          [['1', 'Visible'], ['0', 'Blocker only']], '236px', function (k) {
            var o = envSelected(); if (o) envCommand('object ' + o.id + ' cameraVisible ' + k);
          }, 'A blocker-only prop still blocks and bounces added light without being drawn.', 13);

        // A GLB's animations (st.envClips[id]): which one plays, and how fast. Shown only for a
        // mesh that has clips; the buttons are rebuilt when the selected mesh's clips change.
        var animSection = mk('Panel', inspector);
        animSection.style.width = '100%'; animSection.style.flowChildren = 'down';
        inspector.__animSection = animSection;
        subHead(animSection, 'ANIMATION');
        inspector.__animNote = envNote(animSection, '');
        inspector.__animButtons = mk('Panel', animSection);
        inspector.__animButtons.style.width = '100%'; inspector.__animButtons.style.flowChildren = 'down';
        inspector.__animSig = '';
        inspector.__animSpeed = sliderRow(animSection, {
          label: 'Speed', min: -4, max: 4, decimals: 2, unit: '\u00d7',
          tip: 'How fast the clip plays: 1 is as authored, negative plays it backwards. It follows the demo clock, so renders and scrubbing pose it the same way.',
          onChange: function (v) { var o = envSelected(); if (o) envCommand('object ' + o.id + ' clipSpeed ' + v); }
        });

        controlParent = mk('Panel', inspector);
        controlParent.style.width = '100%'; controlParent.style.flowChildren = 'down';
        inspector.__materialSection = controlParent;
        subHead(controlParent, 'MATERIAL');
        inspector.__materialEnabled = modeRow(controlParent, 'Shading',
          [['0', 'Simple'], ['1', 'Material']], '236px', function (k) {
            var o = envSelected(); if (o) envCommand('object ' + o.id + ' materialEnabled ' + k);
          }, 'Material shading unlocks roughness, metallic, emission and textures.', 13);
        control('Roughness', 'roughness', 0.05, 1, 2);
        control('Metallic', 'metallic', 0, 1, 2);
        control('Emission', 'emission', 0, 16, 2);
        inspector.__textureChannel = 'albedo';
        inspector.__textureNote = envNote(controlParent, '');
        inspector.__textureSlots = modeRow(controlParent, 'Texture slot',
          [['albedo', 'Color'], ['normal', 'Normal'], ['roughness', 'Roughness']], '236px',
          function (k) { inspector.__textureChannel = k; inspector.__textureSlots.sync(k); },
          'Color is sRGB; normal and roughness are linear. Normal maps use the +Y tangent convention.', 12);
        inspector.__textureSlots.sync('albedo');
        actionGroup(controlParent, 'Selected slot', [
          ['Clear texture', function () {
            var o = envSelected();
            if (o) envCommand('texture ' + o.id + ' ' + inspector.__textureChannel + ' -');
          }]
        ], 3);
        inspector.__textureList = mk('Panel', controlParent);
        inspector.__textureList.style.width = '100%';
        inspector.__textureList.style.flowChildren = 'down';
        inspector.__textureSig = '';
        controlParent = inspector;
      } else {
        control('Brightness', 'intensity', 0, 8, 2);
        control('Range', 'range', 16, 4096, 0, 'u');
        control('Beam angle', 'cone', 5, 170, 0, '°');
        control('Emitter width', 'width', 1, 1024, 0, 'u');
        control('Emitter height', 'height', 1, 1024, 0, 'u');
        control('Source radius', 'sourceRadius', 0, 64, 1, 'u',
          'Filters world shadows. Large values can soften fine blockers away.');

        inspector.__shadows = modeRow(inspector, 'Dynamic shadows', [['0', 'Off'], ['1', 'On']], '236px',
          function (k) { var o = envSelected(); if (o) envCommand('object ' + o.id + ' shadows ' + k); },
          'Blocks this light with map geometry and added props. Up to four added lights cast at once.', 13);
      }
      controlParent = mk('Panel', inspector);
      controlParent.style.width = '100%'; controlParent.style.flowChildren = 'down'; controlParent.style.marginTop = '4px';
      colorField(controlParent, group === 'lights' ? 'Light color' : 'Object color', function () { var o=envSelected(); return o && {id:o.id,rgb:o.color}; }, function (rgb) {
        var o=envSelected(); if (!o) return;
        ['r','g','b'].forEach(function (key,i) { envCommand('object '+o.id+' '+key+' '+rgb[i]); });
      });
      // The actions: move it (with its arrows in the view), copy it, or remove it.
      var acts = actionRowEnd(inspector, 10);
      acts.style.flowChildren = 'right-wrap';
      inspector.__place = actionBtn(acts, 'Edit position', function () {
        var o = envSelected(); if (!o) return;
        if (st && st.envEditing) envCommand('edit stop'); else envCommand('edit start ' + o.id);
      });
      actionBtn(acts, 'Duplicate', function () { var o = envSelected(); if (o) envCommand('duplicate ' + o.id); });
      olDanger(actionBtn(acts, 'Remove', function () { var o = envSelected(); if (o) envCommand('remove ' + o.id); }));
    });

)CFJS"
#include "ConfigHudRigJs.h"
#include "ConfigHudEnvironmentStateJs.h"
#include "ConfigHudEnvironmentGizmoJs.h"
#include "ConfigHudPropsGizmoJs.h"
