#pragma once

// ENVIRONMENT tab of the CONFIG panel: the pass cards, and the plumbing every environment
// fragment shares. JS fragment concatenated (via #include, as an adjacent raw string literal)
// into the kConfigHudJs body assembled in ConfigHudJs.h -- not a standalone script. Shares scope
// with the root IIFE and appends to `pageEnvironment` / `renderHooks`.
//
// THIS TAB IS THE ADDED RENDERER (the `rami` branch work): procedural sky, volumetric clouds,
// surface relighting, voxel bounce, volumetric fog, precipitation, surface coatings and the
// finishing grade. None of it drives a CS2 setting; it is composited on top. Anything that drives
// a value the game ALREADY HAS is the EFFECTS tab instead -- see the split note at the top of
// ConfigHudEffectsTabJs.h. "Fog" existing on both tabs is correct and deliberate: one is the map's
// own fog, the other is added volumetric fog.
//
// Every card here is ONE RENDER PASS, so they all carry the environment teal (#5fd0c0) rather
// than the panel's blue -- the two classes must never read as the same thing (Docs/panorama/panorama_ui_guide.md).
//
// The cards are created HERE, in the original design's order, because Panorama flows
// siblings in creation order and the fragments that fill them run afterwards:
//
//   ConfigHudClimateJs.h           sky / lighting / fog / coatings / finishing / looks & quality
//   ConfigHudEnvironmentTabJs.h    precipitation, scene fog+tint, objects, the object inspector
//   ConfigHudEnvironmentStateJs.h  presets, diagnostics, the tab footer
//
// The old six-button sub-nav (Lighting / Sky & clouds / Atmosphere / Weather / Props / Presets)
// is gone: it was a second tab bar under the panel's own, and the cards say what they are.

R"CFJS(
    var ENV_ACCENT = '#5fd0c0ff';
    // Declared here because the EFFECTS fragments (included after this one) hang their local
    // reset closures on it.
    var fxResetLocal = {};
    function envCommand(t) { cmd('mirv_filmmaker environment ' + t); }
    function envState() { return st && st.environment; }
    function envClimate() { var s = envState(); return (s && s.climate) ? s.climate : null; }

    // Explanations live on hover now, so an environment "help" line is a tooltip on the control it
    // explains. envNote stays for the genuinely STATEFUL lines -- a status, a count, a reason
    // something is unavailable -- and for the mock's `nt()` rows.
    function envNote(parent, text) {
      var l = lbl(parent, text, S.hint, 12);
      l.hittest = false; l.style.whiteSpace = 'normal';
      l.style.width = '100%'; l.style.marginTop = '6px';
      l.style.paddingLeft = UI.line.padH + 'px'; l.style.paddingRight = UI.line.padH + 'px';
      endTileLine(parent);
      return l;
    }

    function envPrepBusy(st) {
      var render = st && st.envRender ? String(st.envRender) : '';
      return !!(st && (st.envBuildBusy || st.envOcclusionPreparing
        || render.indexOf('Preparing') === 0 || render.indexOf('Waiting') === 0));
    }
    function envPrepPaint(st) {
      var busy = envPrepBusy(st);
      var text = (st && (st.envBuildStatus || st.envOcclusionStatus || renderStatus(st) || ''))
        || 'Preparing map and environment cache';
      envPrepTick = busy ? (envPrepTick || 0) + 1 : 0;
      var dots = 1 + (Math.floor(envPrepTick / 8) % 3);
      var spin = new Array(dots + 1).join('\u25CF');
      if (envPrepBanner) {
        envPrepBanner.visible = busy;
        if (busy) {
          if (envPrepSpin) envPrepSpin.text = spin;
          if (envPrepText) envPrepText.text = text;
        }
      }
    }
    function renderStatus(st) {
      return st && st.envRender ? String(st.envRender) : '';
    }

    // In-tab only: the viewport-wide gold overlay made every other Config tab look frozen.
    var envPrepTick = 0;
    var envPrepBanner = mk('Panel', pageEnvironment);
    envPrepBanner.hittest = false; envPrepBanner.visible = false;
    envPrepBanner.style.width = '100%'; envPrepBanner.style.flowChildren = 'right';
    envPrepBanner.style.marginBottom = '10px';
    envPrepBanner.style.paddingTop = '10px'; envPrepBanner.style.paddingBottom = '10px';
    envPrepBanner.style.paddingLeft = UI.tile.padH + 'px'; envPrepBanner.style.paddingRight = UI.tile.padH + 'px';
    envPrepBanner.style.borderRadius = UI.tile.radius + 'px';
    envPrepBanner.style.backgroundColor = '#5a3a08ff';
    envPrepBanner.style.border = '1px solid #ffcc4d';
    var envPrepSpin = lbl(envPrepBanner, '', '#ffcc4dff', 16);
    envPrepSpin.style.width = '28px'; envPrepSpin.style.verticalAlign = 'center';
    envPrepSpin.style.fontWeight = 'bold';
    var envPrepText = lbl(envPrepBanner, '', '#fff8e6ff', 14);
    envPrepText.style.width = 'fill-parent-flow(1.0)'; envPrepText.style.whiteSpace = 'normal';
    envPrepText.style.verticalAlign = 'center'; envPrepText.style.fontWeight = 'bold';

    // Cards follow the work: choose a look, shape the environment, then place objects.
    var envLooksSec   = section(pageEnvironment, 'LOOKS',         true, true,  'env.looks', ENV_ACCENT);
    var envGradeSec   = envLooksSec;
    var envSkySec     = section(pageEnvironment, 'SKY',           true, false, 'env.sky',   ENV_ACCENT);
    var envLightSec   = section(pageEnvironment, 'LIGHTING',      true, false, 'env.light', ENV_ACCENT);
    var envFogSec     = section(pageEnvironment, 'FOG',           true, false, 'env.fog',   ENV_ACCENT);
    var envWeatherSec = section(pageEnvironment, 'WEATHER',       true, false, 'env.wx',    ENV_ACCENT);
    var envSceneSec   = section(pageEnvironment, 'SCENE OBJECTS', true, false, 'env.scene', ENV_ACCENT);

)CFJS"
R"CFJS(
    // ---- the climate registry --------------------------------------------------------------
    // Every climate control registers here so ONE render hook can sync them all.
    var climateControls = [], envPassPills = [], envPassResetters = [];

    // Scene-wide values that are not climate keys (`environment set <key>`): the distance fog on
    // VOLUMETRIC FOG and the exposure/tint on FINISHING. One registry, synced in the state fragment.
    var envSceneControls = [];
    var SCENE_DEFAULT = { fogR: 0.45, fogG: 0.5, fogB: 0.6, exposure: 1, tintR: 1, tintG: 1, tintB: 1 };
    function envSceneSlider(parent, label, key, min, max, tip) {
      var control = sliderRow(parent, {
        label: label, min: min, max: max, decimals: 2, unit: '', tip: tip,
        onChange: function (v) { envCommand('set ' + key + ' ' + v); }
      });
      envSceneControls.push([control, key]);
      return control;
    }
    function envSceneResetCmds(keys) {
      var out = [];
      for (var i = 0; i < keys.length; i++)
        out.push('mirv_filmmaker environment set ' + keys[i] + ' ' + SCENE_DEFAULT[keys[i]]);
      return out;
    }

    // EVERY PASS IS INDEPENDENT. There is deliberately no gate any more: turning the fog on used
    // to do nothing until you had ALSO found the master switch on the sky card and turned that
    // on, which is exactly backwards -- the one control you did not touch was the one blocking
    // the one you did. Now nothing on this tab is ever greyed out, and the renderer's own master
    // (`climate enabled`) is a consequence of what you switch on rather than a prerequisite for
    // it: any edit turns it on, and switching the last pass off turns it back off.
    //
    // climateGate stays as a no-op so the page builders read the same either way, and so the
    // decision lives in ONE place if a gate is ever genuinely needed again.
    function climateGate(parent) { return parent; }

    // The keys that mean "this pass is doing something", with the value that means it is not.
    // `enabled` is not among them: it is the switch being derived, not a pass.
    var PASS_NEUTRAL = [
      ['sky', 0], ['clouds', 0], ['storm', 0],
      // contactShadows and lightShafts are deliberately absent: both ship defaulted to 1, so
      // counting them would mean "a pass is on" on a scene nobody has touched, and the renderer
      // could never be released. They are settings OF a pass, not passes.
      ['relighting', 0], ['worldGi', 0], ['bakedSuppressed', 0],
      ['fogDensity', 0],
      ['snow', 0], ['wet', 0], ['puddles', 0],
      ['bloom', 0], ['vignette', 0], ['grain', 0], ['chromatic', 0],
      ['saturation', 1], ['contrast', 1]
    ];
    // `just` is what this click has ALREADY WRITTEN, keyed by climate key. It has to be passed
    // in: the state JSON mirrors back a frame later, so reading `c` alone right after a write
    // still reports the old value and the renderer would never be released.
    function envAnyPassOn(just) {
      var c = envClimate();
      if (!c) return false;
      for (var i = 0; i < PASS_NEUTRAL.length; i++) {
        var key = PASS_NEUTRAL[i][0];
        var v = (just && just.hasOwnProperty(key)) ? just[key] : (c[key] || 0);
        if (Math.abs(v - PASS_NEUTRAL[i][1]) > 1e-4) return true;
      }
      if (just && just.hasOwnProperty('weatherType')) return !!just.weatherType;
      var s = envState();
      if (!s) return false;
      if (s.objects && s.objects.some(function(o){return o.enabled !== false;})) return true;
      var exposure = just && just.hasOwnProperty('exposure') ? just.exposure : s.exposure;
      if (Math.abs((exposure === undefined ? 1 : exposure) - 1) > 1e-4) return true;
      var tint = s.tint || [1,1,1];
      for (var ti = 0; ti < 3; ti++) {
        var tk = ['tintR','tintG','tintB'][ti];
        var tv = just && just.hasOwnProperty(tk) ? just[tk] : tint[ti];
        if (Math.abs(tv - 1) > 1e-4) return true;
      }
      return !!(s.weather && s.weather.type);
    }
    // Enable only in response to an edit, never simply by opening the panel.
    function envEnsureEnabled() {
      var c = envClimate();
      if (c && !c.enabled) envCommand('climate enabled 1');
    }
    // The other half: switching the last pass off should not leave the renderer running.
    function envReleaseIfIdle(just) {
      var c = envClimate();
      if (!envAnyPassOn(just)) {
        envCommand('climate enabled 0');
        envCommand('off');
      }
    }

    // The mock's DEFS table, key for key: [label, min, max, decimals, unit, offAtZero, tooltip].
    // One table beats fifty call sites that each restate a range, and it is the same shape the
    // original design used, preserving that table's layout.
    var CLD = {
      stormInterval: ['Strike interval', 2, 60, 1, 's', false, 'Average spacing; each seeded strike has a varied onset and several pulses.'],
      stormStrength: ['Lightning intensity', 0, 8, 2, '', false, 'Brightness of lightning channels, cloud glow and the shadowed emitter.'],
      stormRadius: ['Strike radius', 100, 8000, 0, 'u', false, 'Strike placement around the fixed storm center.'],
      stormSeed: ['Lightning seed', 0, 65535, 0, '', false, 'Repeatable strike positions and timing.'],
      stormX: ['Storm center X', -32768, 32768, 0, 'u', false, 'Fixed world-space center.'],
      stormY: ['Storm center Y', -32768, 32768, 0, 'u', false, 'Fixed world-space center.'],
      stormZ: ['Strike floor', -8000, 32000, 0, 'u', false, 'Bottom of the lightning channels. Set to the ground height for the shot.'],
      coverage:    ['Cloud coverage', 0, 1, 2, '', false, 'How much of the sky the volumetric cloud layer fills.'],
      density:     ['Cloud density', 0, 2, 2, '', false, 'How thick the cloud medium is, which sets how hard the sun is blocked.'],
      cloudBase:   ['Base height', -2000, 32000, 0, 'u', false, 'World height the cloud layer starts at.'],
      cloudDepth:  ['Layer depth', 100, 8000, 0, 'u', false, 'How tall the cloud layer is above its base.'],
      cloudScale:  ['Detail scale', 0.0001, 0.01, 4, '', false, 'Size of the cloud noise. Smaller values give bigger formations.'],
      windSpeed:   ['Wind speed', -600, 600, 0, 'u/s', false, 'How fast the cloud layer drifts.'],
      haze:        ['Horizon haze', 0, 2, 2, '', false, 'Atmospheric thickening toward the horizon.'],
      stars:       ['Stars at night', 0, 1, 2, '', false, 'Star brightness once the sun is below the horizon.'],
      sunAzimuth:  ['Azimuth', 0, 360, 0, '°', false, 'Compass direction of the procedural sun. Drives sky, clouds, fog and relighting.'],
      sunElevation:['Elevation', -30, 90, 0, '°', false, 'Height of the sun above the horizon. Negative is night.'],
      sunPower:    ['Sun brightness', 0, 8, 2, '', false, 'Brightness of the sun on surfaces and in the sky.'],
      sunColorR:   ['Sun red', 0, 2, 2, '', false, 'Red in the sunlight. 1 is the default warm-to-white ramp; lower cools the light, higher warms it.'],
      sunColorG:   ['Sun green', 0, 2, 2, '', false, 'Green in the sunlight. Tints the surfaces, the sun disk and the sky glow together.'],
      sunColorB:   ['Sun blue', 0, 2, 2, '', false, 'Blue in the sunlight. Raise it for an overcast or moonlit cast, lower it for golden hour.'],
      sunRelight:  ['Surface sunlight', 0, 3, 2, '', false, 'How strongly the procedural sun lights map surfaces.'],
      relight:     ['Flatten contrast', 0, 1, 2, '', false, 'Keeps baked shading as AO and replaces the map’s directional light with Environment’s sun.'],
      ambient:     ['Ambient light', 0, 2, 2, '', false, 'Light arriving from the sky. Map lamps have their own brightness.'],
      mapLightPower:['Map lights', 0, 8, 2, '', false, 'Brightness of the map’s authored lamps, including interior lights.'],
      shadowBias:  ['Surface offset', 0.1, 16, 1, '', false, 'Shadow-map offset that stops surfaces shadowing themselves.'],
      lampAngle:    ['Light source angle', 0, 5, 2, '°', false, 'Apparent size of map lights for shadow filtering. 0 gives sharp shadows; larger values soften their edges without changing brightness.'],
      shadowSoftness:['Shadow softness', 0, 4, 2, '×', false, 'Scales sun and map light shadow softness together. 1 uses the source sizes above; 0 makes both sharp.'],
      sunAngle:     ['Sun size', 0, 5, 2, '°', false, 'Apparent size of the sun in degrees. Shadows stay sharp where an object touches a surface and soften the further they fall from it, like real sunlight. 0.53° is the real sun; raise it for broad, soft shadows.'],
      bounce:      ['Bounce intensity', 0, 4, 2, '', false, 'Amount of reflected light from the sun, sky and map lights. 0 removes bounce while keeping sky occlusion; 1 is the default.'],
      bounceSteps: ['Bounce steps/frame', 1, 3, 0, '', false, 'How many indirect-light propagation steps run each frame. 1 is the default; 2 or 3 can converge faster but cost more GPU time.'],
      bounceSize:  ['Volume size', 512, 8192, 0, 'u', false, '128 cells per axis, so a smaller volume gives finer detail.'],
      bounceX:     ['Center X', -32768, 32768, 0, 'u', false, 'World X of the bounce volume centre.'],
      bounceY:     ['Center Y', -32768, 32768, 0, 'u', false, 'World Y of the bounce volume centre.'],
      bounceZ:     ['Center Z', -32768, 32768, 0, 'u', false, 'World Z of the bounce volume centre.'],
      bounceAlbedo:['Map reflectance', 0.05, 0.95, 2, '', false, 'How much light the map’s own surfaces are assumed to bounce.'],
      sceneReflections: ['Scene reflections', 0, 1, 2, '', false, 'Rough reflections off the bounce volume. Needs the volume enabled.'],
      fogStart: ['Start distance', 0, 12000, 0, 'u', false, 'Clear space between the camera and the fog.'],
      fogDensity:   ['Density', 0, 6, 2, '', false, 'Density of the volumetric fog the sun and lights scatter through.'],
      fogBase:     ['Base height', -4000, 32000, 0, 'u', false, 'World height the fog sits at.'],
      fogHeight:   ['Height', 20, 3000, 0, 'u', false, 'How quickly the fog thins with height.'],
      fogNoise:    ['Variation', 0, 1, 2, '', false, 'Breaks the fog up so it is not a flat sheet.'],
      fogRange:    ['Ray distance', 128, 12000, 0, 'u', false, 'How far the fog is marched. Longer costs more.'],
      scatter:     ['Forward scattering', 0, 0.85, 2, '', false, 'How much light bends toward the camera, which makes shafts read.'],
      localFogRadius: ['Local radius', 0, 8000, 0, 'u', true, 'Confines the fog to a sphere. 0 keeps it global.'],
      bloom:       ['Bloom', 0, 2, 2, '', true, 'Glow around bright areas.'],
      bloomThreshold: ['Bloom threshold', 0.1, 8, 2, '', false, 'How bright a pixel must be before it blooms.'],
      saturation:  ['Saturation', 0, 2, 2, '', false, 'Colour intensity of the finished frame.'],
      contrast:    ['Contrast', 0.25, 2, 2, '', false, 'Tonal separation of the finished frame.'],
      vignette:    ['Vignette', 0, 1, 2, '', true, 'Darkening toward the frame edges.'],
      grain:       ['Film grain', 0, 0.1, 3, '', true, 'Fine noise over the frame.'],
      chromatic:   ['Chromatic shift', 0, 4, 2, '', true, 'Colour fringing toward the frame edges.'],
      snow:        ['Snow coverage', 0, 1, 2, '', true, 'Snow on upward-facing surfaces, including actors.'],
      puddles:     ['Puddle coverage', 0, 1, 2, '', true, 'Standing water on flat ground. Local radius uses the height band.'],
      wet:         ['Wetness', 0, 1, 2, '', true, 'Darkens and glosses surfaces as if rained on.'],
      puddleHeight:['Puddle height', -4000, 8000, 0, 'u', false, 'World height the puddle band sits at when the coating is local.'],
      puddleDepth: ['Height tolerance', 1, 200, 0, 'u', false, 'How far either side of that height still counts as puddle when local.'],
      ripple:      ['Ripples', 0, 2, 2, '', true, 'Surface movement on the standing water.'],
      reflectionRoughness: ['Reflection roughness', 0, 1, 2, '', false, 'How sharp the wet-surface reflections are.'],
      maskScale:   ['Patch scale', 0.002, 0.1, 3, '', false, 'Size of the coverage patches.'],
      surfaceRadius: ['Surface radius', 0, 8000, 0, 'u', true, 'Confines the coatings to a sphere. 0 keeps them global.'],
      particleDetail: ['Snowflake detail', 0, 1, 2, '', false, 'Shape detail on individual flakes.'],
      turbulence:  ['Turbulence', 0, 8, 2, '', true, 'How much the precipitation tumbles as it falls.'],
      particleOpacity: ['Opacity', 0, 1, 2, '', false, 'How solid each particle reads.'],
      softness:    ['Soft intersections', 0.1, 64, 1, '', false, 'Fades particles as they meet geometry so they do not cut hard.'],
      streak:      ['Rain streak length', 1, 50, 0, '', false, 'How long each rain streak is drawn.'],
      particleLight: ['Light response', 0, 2, 2, '', false, 'How much added lights illuminate the precipitation.'],
      bakedFill:   ['Unbaked fill', 0, 4, 2, '', false, 'Flat light fed through the map’s own irradiance input, so albedo and occlusion still apply. 0 leaves unlit faces black.']
    };
)CFJS"
R"CFJS(
    // A climate value tile. `cs(page, 'coverage')` is the whole call: the range, the unit and the
    // hover text come from CLD above.
    function cs(parent, key) {
      var d = CLD[key];
      var control = sliderRow(parent, {
        label: d[0], min: d[1], max: d[2], decimals: d[3], unit: d[4], offAtZero: d[5], tip: d[6],
        onChange: function (v) {
          envEnsureEnabled();
          if (LIGHT_KEYS.indexOf(key) >= 0) envCommand('climate relighting 1');
          if (['sunAzimuth', 'sunElevation', 'sunPower', 'haze', 'stars'].indexOf(key) >= 0) envCommand('climate sky 1');
          envCommand('climate ' + key + ' ' + v);
        }
      });
      climateControls.push([control, key]);
      return control;
    }
    // Every Lighting control. Editing one keeps dynamic lighting on (relighting 1), so turning a
    // lighting feature off or resetting the card never releases the renderer and drops the map
    // back to its baked lighting.
    var LIGHT_KEYS = ['mapLightPower', 'ambient', 'sunAngle', 'lampAngle', 'shadowSoftness', 'contactShadows', 'ao', 'skyOcclusion',
      'bounce', 'bounceSteps'];
    // A Lighting feature switch: on/off only, and never a reason to switch the renderer off.
    function lw(parent, label, key, tip) {
      var t = togglePill(parent, label, tip, function () {
        var c = envClimate();
        var on = !(c && c[key]);
        envEnsureEnabled();
        envCommand('climate relighting 1');
        envCommand('climate ' + key + ' ' + (on ? '1' : '0'));
      });
      climateControls.push([t, key, 'toggle']);
      return t;
    }
    // A climate switch tile, in the same 76px box as a value tile.
    function cw(parent, label, key, tip) {
      var t = togglePill(parent, label, tip, function () {
        var c = envClimate();
        var on = !(c && c[key]);
        if (on) envEnsureEnabled();
        envCommand('climate ' + key + ' ' + (on ? '1' : '0'));
        if (!on) { var justOff = {}; justOff[key] = 0; envReleaseIfIdle(justOff); }
      });
      climateControls.push([t, key, 'toggle']);
      return t;
    }

    // ---- the per-card pass switch ----------------------------------------------------------
    // Every card on this tab is one pass, and the original design gave every pass its own switch. The
    // backend has exactly one real on/off (`climate enabled`, the renderer as a whole); the other
    // passes are driven by their own values instead. So a card's switch BYPASSES its pass: off
    // writes each of the card's driving values to its neutral setting and remembers what was
    // there, on writes the remembered values back. That is a real, useful control -- one click to
    // see the frame without this pass -- rather than a switch that only pretends to be one.
    //
    // keys: [[climateKey, offValue, restoreValue], ...]. A pass reads ON when ANY of its keys is
    // away from neutral, which is also exactly what the card summary reports.
    // `confirmOn(proceed)`, when given, runs instead of switching ON and calls proceed() to do it.
    // Switching the last pass off releases the renderer. Pending writes override the delayed
    // state mirror, while unrelated passes and placed objects keep their resources.
    function envPassPill(card, keys, tip, confirmOn, opts) {
      var memo = null;
      envPassResetters.push(function (resetKeys) {
        for (var i = 0; i < keys.length; i++)
          if (!resetKeys || resetKeys.indexOf(keys[i][0]) >= 0) { memo = null; return; }
      });
      function isOn() {
        var c = envClimate();
        if (!c) return false;
        // Presets may retain non-neutral values while bypassed. A first click must enable
        // the renderer and the requested pass rather than switching a latent value off.
        if (!c.enabled) return false;
        for (var i = 0; i < keys.length; i++) {
          // A shadow preference alone does not enable the added-lighting pass, and a cloud layer
          // alone is not the sky -- clouds default to on, so a freshly reset card would otherwise
          // report this switch as on with no sky rendering.
          // ...and neither a cloud layer nor the bounce volume is the pass itself: both ride
          // along with their card's switch and must not report it as on by themselves.
          if (keys[i][0] === 'contactShadows' || keys[i][0] === 'clouds' ||
              keys[i][0] === 'worldGi') continue;
          if (opts && opts.rides && opts.rides.indexOf(keys[i][0]) >= 0) continue;
          if (Math.abs((c[keys[i][0]] || 0) - keys[i][1]) > 1e-4) return true;
        }
        return false;
      }
      var toggle = function () {
        var c = envClimate();
        if (!c) return;
        var i;
        if (isOn()) {
          memo = [];
          for (i = 0; i < keys.length; i++) memo.push(c[keys[i][0]]);
          // pass-off, not `climate <key> <neutral>`: the backend keeps each value in `held`, so the
          // card switched on after a demo change or a restart gets it back. memo only lasts this session.
          var justOff = {}, pairs = [];
          for (i = 0; i < keys.length; i++) {
            pairs.push(keys[i][0] + ' ' + keys[i][1]);
            justOff[keys[i][0]] = keys[i][1];
          }
          envCommand('pass-off ' + pairs.join(' '));
          envReleaseIfIdle(justOff);
        } else {
          var turnOn = function () {
            envEnsureEnabled();
            // After a demo or game restart every pass comes back off with its value held by the
            // backend (HoldRestoredPasses); switching the card on brings the saved value back.
            var held = (envState() || {}).held || {};
            for (var j = 0; j < keys.length; j++) {
              var v = memo ? memo[j] : (held.hasOwnProperty(keys[j][0]) ? held[keys[j][0]] : keys[j][2]);
              // Restoring a remembered neutral would leave the switch stuck off.
              if (Math.abs(v - keys[j][1]) < 1e-4) v = keys[j][2];
              envCommand('climate ' + keys[j][0] + ' ' + v);
            }
          };
          if (confirmOn) confirmOn(turnOn); else turnOn();
        }
      };
      var h = card.__headerPill(toggle, tip);
      card.__togglePass = toggle; // backend verification uses the actual header/rail callback
      envPassPills.push([h, isOn]);
      return h;
    }

    // Card-scoped RESET. The backend resets by PAGE (sky / lighting / atmosphere / weather), and
    // its pages do not line up with the original design's cards -- fog and the finishing grade are one
    // backend page, and `reset lighting` also deletes every added light. So a card's Reset writes
    // its OWN keys back to their shipped defaults and touches nothing else.
    var CL_DEFAULT = {
      storm: 0, stormInterval: 9, stormStrength: 2, stormRadius: 1600, stormX: 0, stormY: 0, stormZ: 0, stormSeed: 13, stormHold: 0,
      sky: 0, clouds: 0, coverage: 0.55, density: 0.65, cloudBase: 1800, cloudDepth: 1200,
      cloudScale: 0.0008, windSpeed: 25, sunAzimuth: 135, sunElevation: 35, sunPower: 1,
      sunColorR: 1, sunColorG: 1, sunColorB: 1,
      haze: 0.4, stars: 0.3,
      relighting: 0, relight: 0, ambient: 1, mapLightPower: 1, sunRelight: 0.6, bakedSuppressed: 0, bakedFill: 0.6,
      contactShadows: 1, shadowBias: 2, sunAngle: 0.53, lampAngle: 1, shadowSoftness: 1, worldGi: 0, bounce: 1, bounceSteps: 1, bounceSize: 2048,
      bounceX: 0, bounceY: 0, bounceZ: 0, bounceAlbedo: 0.55, sceneReflections: 0, ao: 1, skyOcclusion: 1,
      fogDensity: 0, fogStart: 0, fogHeight: 320, fogBase: 0, fogNoise: 0.4, fogRange: 4000, scatter: 0.45,
      lightShafts: 0, localFogRadius: 0,
      snow: 0, wet: 0, puddles: 0, puddleHeight: 128, puddleDepth: 24, surfaceRadius: 0, puddleNative: 0,
      maskScale: 0.02, ripple: 0.35, reflectionRoughness: 0.35,
      particleDetail: 1, turbulence: 1, particleOpacity: 0.65, softness: 8, streak: 12,
      particleLight: 0.4,
      bloom: 0, bloomThreshold: 0.8, saturation: 1, contrast: 1, vignette: 0, grain: 0,
      chromatic: 0
    };
    // A card's Reset, as the shared runner's three phases. PASS_NEUTRAL gives the OFF value
    // for the keys that ARE passes; everything else goes to 0 first, which is enough to make the
    // renderer see a change before the default lands on top of it.
    var PASS_OFF = {};
    for (var pni = 0; pni < PASS_NEUTRAL.length; pni++) PASS_OFF[PASS_NEUTRAL[pni][0]] = PASS_NEUTRAL[pni][1];
    function envResetCmds(keys) {
      var off = [], def = [];
      for (var i = 0; i < keys.length; i++) {
        var k = keys[i];
        // Only neutralize actual passes. Zero is invalid for heights, ranges and scales.
        if (PASS_OFF.hasOwnProperty(k)) off.push('mirv_filmmaker environment climate ' + k + ' ' + PASS_OFF[k]);
        def.push('mirv_filmmaker environment climate ' + k + ' ' + CL_DEFAULT[k]);
      }
      // Sun default writes couple sky/lighting on. Finish with the off writes.
      return { off: off, cmds: def.concat(off), after: function () { envAfterReset(keys); } };
    }
    // Reset never enables the renderer. The backend publishes edits every frame; no relatch
    // is required. Account for the pending writes before deciding whether the last pass is off.
    function envAfterReset(keys, extra) {
      for (var r = 0; r < envPassResetters.length; r++) envPassResetters[r](keys);
      var just = {};
      for (var i = 0; i < keys.length; i++) just[keys[i]] = CL_DEFAULT[keys[i]];
      if (extra) for (var k in extra) just[k] = extra[k];
      // Restore the pre-reset lighting switch after coupled sun writes.
      var c = envClimate(), scene = envState();
      if (keys.indexOf('sunAzimuth') >= 0 && keys.indexOf('relighting') < 0) {
        just.relighting = c ? (c.relighting || 0) : 0;
        envCommand('climate relighting ' + just.relighting);
      }
      if (scene && !scene.enabled) {
        envCommand('climate enabled 0'); envCommand('off');
      } else envReleaseIfIdle(just);
    }

    // ---- the card switches -----------------------------------------------------------------
    // One per pass, and no card's switch is a prerequisite for another's. Reset comes BEFORE the
    // switch on every card, because the original design's header runs action -> Reset -> switch ->
    // chevron and Panorama flows siblings in creation order.
    // THE SKY CARD OWNS THE SKY AND ITS CLOUDS. The header switch IS the procedural sky (there is
    // no second switch inside the card); the cloud layer has its own switch on the Clouds page.
    var envSkyReset = envSkySec.__headerReset(envResetCmds(
      ['sky', 'haze', 'stars', 'sunAzimuth', 'sunElevation', 'sunPower',
       'sunColorR', 'sunColorG', 'sunColorB',
       'clouds', 'coverage', 'density', 'cloudBase', 'cloudDepth', 'cloudScale', 'windSpeed']), 'env.sky');
    // Switching on brings the cloud layer with it, because a bare procedural sky reads as a flat
    // gradient; switching off bypasses the clouds too and puts them back with the sky. The Clouds
    // page switch still overrides that either way, and its value is remembered across a bypass.
    // The sun it draws lights the world too: switching on also brings dynamic lighting (cast
    // shadows, ambient occlusion, sky occlusion), which rides along like the clouds and never
    // reports this switch as on by itself.
    var envSkyPill = envPassPill(envSkySec, [['sky', 0, 1], ['clouds', 0, 1], ['relighting', 0, 1]],
      'Turns the procedural sky on or off, with clouds and the dynamic lighting its sun casts '
      + '(shadows, ambient occlusion, sky occlusion). The cloud layer has its own switch on the '
      + 'Clouds page and the Lighting card keeps its own switch.',
      // The first switch-on of each game session warns first ($ outlives a panel rebuild, not a
      // restart). The wording is the author's, verbatim.
      function (proceed) {
        if ($.MvmSkyWarned) { proceed(); return; }
        cfgConfirm('Sky', 'This mode is very experimental and buggy and laggy as fuck. I might fix it.',
          'Turn on anyway', function () { $.MvmSkyWarned = true; proceed(); });
      }, { rides: ['relighting'] });

    // Release every lighting switch while restoring its tuning.
    var envLightResetSpec = envResetCmds(
      ['relighting', 'relight', 'worldGi', 'bakedSuppressed', 'sceneReflections',
       'mapLightPower', 'ambient', 'sunRelight', 'bakedFill', 'contactShadows',
       'shadowBias', 'sunAngle', 'lampAngle', 'shadowSoftness', 'bounce', 'bounceSteps',
       'bounceSize', 'bounceX', 'bounceY', 'bounceZ', 'bounceAlbedo',
       'ao', 'skyOcclusion']);
    envLightResetSpec.cmds = envLightResetSpec.cmds.concat(
      ['mirv_relight lighteditor off', 'mirv_relight lights reset',
       'mirv_filmmaker world exposure adapt off quiet', 'mirv_filmmaker world exposure speed 2 quiet',
       'mirv_filmmaker world exposure compensation 0 quiet']);
    var envLightReset = envLightSec.__headerReset(envLightResetSpec, 'env.light');

    // One fog state controls near lighting and the far continuation.
    var envFogResetSpec = envResetCmds(
      ['fogDensity', 'fogStart', 'fogBase', 'fogHeight', 'fogNoise', 'fogRange', 'scatter', 'lightShafts',
       'localFogRadius']);
    envFogResetSpec.cmds = envFogResetSpec.cmds.concat(envSceneResetCmds(['fogR', 'fogG', 'fogB']));
    var envFogReset = envFogSec.__headerReset(envFogResetSpec, 'env.fog');
    var envFogPill = envPassPill(envFogSec, [['fogDensity', 0, 0.35]],
      'Bypasses the volumetric fog by dropping its density to zero, and puts it back where it was.');

    // Exposure and tint are scene values, not climate keys, so the reset sends them explicitly --
    // they used to be skipped and the tints stayed where they were.
    var envGradeResetSpec = envResetCmds(
      ['bloom', 'bloomThreshold', 'vignette', 'saturation', 'contrast', 'grain', 'chromatic']);
    envGradeResetSpec.cmds = envGradeResetSpec.cmds.concat(
      envSceneResetCmds(['exposure', 'tintR', 'tintG', 'tintB']));
    envGradeResetSpec.after = function () {
      envAfterReset(['bloom','bloomThreshold','vignette','saturation','contrast','grain','chromatic'],
                    {exposure:1,tintR:1,tintG:1,tintB:1});
    };
    var envGradeReset = envGradeSec.__headerReset(envGradeResetSpec, 'env.looks');


    renderHooks.push(function (st) {
      var c = envClimate();
      if (!c) return;
      for (var p = 0; p < envPassPills.length; p++) envPassPills[p][0].set(envPassPills[p][1]());
      for (var j = 0; j < climateControls.length; j++) {
        var rec = climateControls[j], v = c[rec[1]];
        if (rec[2] === 'toggle') rec[0].set(!!v);
        else if (rec[2] === 'seg') rec[0].sync(String(Math.round(v || 0)));
        else if (typeof v === 'number') rec[0].sync(v);
      }
    });
)CFJS"
