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
        || T('Preparing map and environment cache');
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
    var envLooksSec   = section(pageEnvironment, T('LOOKS'),         true, true,  'env.looks', ENV_ACCENT);
    var envGradeSec   = envLooksSec;
    var envSkySec     = section(pageEnvironment, T('SKY'),           true, false, 'env.sky',   ENV_ACCENT);
    var envLightSec   = section(pageEnvironment, T('LIGHTING'),      true, false, 'env.light', ENV_ACCENT);
    var envFogSec     = section(pageEnvironment, T('FOG'),           true, false, 'env.fog',   ENV_ACCENT);
    var envWeatherSec = section(pageEnvironment, T('WEATHER'),       true, false, 'env.wx',    ENV_ACCENT);
    var envSceneSec   = section(pageEnvironment, T('SCENE OBJECTS'), true, false, 'env.scene', ENV_ACCENT);

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
      ['fogDensity', 0], ['groundFog', 0],
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

)CFJS"
R"CFJS(
    // The mock's DEFS table, key for key: [label, min, max, decimals, unit, offAtZero, tooltip].
    // One table beats fifty call sites that each restate a range, and it is the same shape the
    // original design used, preserving that table's layout.
    var CLD = {
      stormInterval: [T('Strike interval'), 2, 60, 1, 's', false, T('Average spacing; each seeded strike has a varied onset and several pulses.')],
      stormStrength: [T('Lightning intensity'), 0, 8, 2, '', false, T('Brightness of lightning channels, cloud glow and the shadowed emitter.')],
      stormRadius: [T('Strike radius'), 100, 8000, 0, 'u', false, T('Strike placement around the fixed storm center.')],
      stormSeed: [T('Lightning seed'), 0, 65535, 0, '', false, T('Repeatable strike positions and timing.')],
      stormX: [T('Storm center X'), -32768, 32768, 0, 'u', false, T('Fixed world-space center.')],
      stormY: [T('Storm center Y'), -32768, 32768, 0, 'u', false, T('Fixed world-space center.')],
      stormZ: [T('Strike floor'), -8000, 32000, 0, 'u', false, T('Bottom of the lightning channels. Set to the ground height for the shot.')],
      coverage:    [T('Cloud coverage'), 0, 1, 2, '', false, T('How much of the sky the volumetric cloud layer fills.')],
      density:     [T('Cloud density'), 0, 2, 2, '', false, T('How thick the cloud medium is, which sets how hard the sun is blocked.')],
      cloudBase:   [T('Base height'), -2000, 32000, 0, 'u', false, T('World height the cloud layer starts at.')],
      cloudDepth:  [T('Layer depth'), 100, 8000, 0, 'u', false, T('How tall the cloud layer is above its base.')],
      cloudScale:  [T('Detail scale'), 0.0001, 0.01, 4, '', false, T('Size of the cloud noise. Smaller values give bigger formations.')],
      windSpeed:   [T('Wind speed'), -600, 600, 0, 'u/s', false, T('How fast the cloud layer drifts.')],
      haze:        [T('Horizon haze'), 0, 2, 2, '', false, T('Atmospheric thickening toward the horizon.')],
      stars:       [T('Stars at night'), 0, 1, 2, '', false, T('Star brightness once the sun is below the horizon.')],
      sunAzimuth:  [T('Azimuth'), 0, 360, 0, '°', false, T('Compass direction of the procedural sun. Drives sky, clouds, fog and relighting.')],
      sunElevation:[T('Elevation'), -30, 90, 0, '°', false, T('Height of the sun above the horizon. Negative is night.')],
      sunPower:    [T('Sun brightness'), 0, 8, 2, '', false, T('Brightness of the sun on surfaces and in the sky.')],
      sunColorR:   [T('Sun red'), 0, 2, 2, '', false, T('Red in the sunlight. 1 is the default warm-to-white ramp; lower cools the light, higher warms it.')],
      sunColorG:   [T('Sun green'), 0, 2, 2, '', false, T('Green in the sunlight. Tints the surfaces, the sun disk and the sky glow together.')],
      sunColorB:   [T('Sun blue'), 0, 2, 2, '', false, T('Blue in the sunlight. Raise it for an overcast or moonlit cast, lower it for golden hour.')],
      sunRelight:  [T('Surface sunlight'), 0, 3, 2, '', false, T('How strongly the procedural sun lights map surfaces.')],
      relight:     [T('Flatten contrast'), 0, 1, 2, '', false, T('Keeps baked shading as AO and replaces the map’s directional light with Environment’s sun.')],
      ambient:     [T('Ambient light'), 0, 2, 2, '', false, T('Light arriving from the sky. Map lamps have their own brightness.')],
      mapLightPower:[T('Map lights'), 0, 8, 2, '', false, T('Brightness of the map’s authored lamps, including interior lights.')],
      shadowBias:  [T('Surface offset'), 0.1, 16, 1, '', false, T('Shadow-map offset that stops surfaces shadowing themselves.')],
      lampAngle:    [T('Light source angle'), 0, 5, 2, '°', false, T('Apparent size of map lights for shadow filtering. 0 gives sharp shadows; larger values soften their edges without changing brightness.')],
      shadowSoftness:[T('Shadow softness'), 0, 4, 2, '×', false, T('Scales sun and map light shadow softness together. 1 uses the source sizes above; 0 makes both sharp.')],
      sunAngle:     [T('Sun size'), 0, 5, 2, '°', false, T('Apparent size of the sun in degrees. Shadows stay sharp where an object touches a surface and soften the further they fall from it, like real sunlight. 0.53° is the real sun; raise it for broad, soft shadows.')],
      bounce:      [T('Bounce intensity'), 0, 4, 2, '', false, T('Amount of reflected light from the sun, sky and map lights. 0 removes bounce while keeping sky occlusion; 1 is the default.')],
      bounceSteps: [T('Bounce steps/frame'), 1, 3, 0, '', false, T('How many indirect-light propagation steps run each frame. 1 is the default; 2 or 3 can converge faster but cost more GPU time.')],
      bounceSize:  [T('Volume size'), 512, 8192, 0, 'u', false, T('128 cells per axis, so a smaller volume gives finer detail.')],
      bounceX:     [T('Center X'), -32768, 32768, 0, 'u', false, T('World X of the bounce volume centre.')],
      bounceY:     [T('Center Y'), -32768, 32768, 0, 'u', false, T('World Y of the bounce volume centre.')],
      bounceZ:     [T('Center Z'), -32768, 32768, 0, 'u', false, T('World Z of the bounce volume centre.')],
      bounceAlbedo:[T('Map reflectance'), 0.05, 0.95, 2, '', false, T('How much light the map’s own surfaces are assumed to bounce.')],
      sceneReflections: [T('Scene reflections'), 0, 1, 2, '', false, T('Rough reflections off the bounce volume. Needs the volume enabled.')],
      fogStart: [T('Start distance'), 0, 12000, 0, 'u', false, T('Clear space between the camera and the fog.')],
      fogDensity:   [T('Density'), 0, 6, 2, '', false, T('Density of the volumetric fog the sun and lights scatter through.')],
      fogBase:     [T('Base height'), -4000, 32000, 0, 'u', false, T('World height the fog sits at.')],
      fogHeight:   [T('Height'), 20, 3000, 0, 'u', false, T('How quickly the fog thins with height.')],
      fogNoise:    [T('Variation'), 0, 1, 2, '', false, T('Breaks the fog up so it is not a flat sheet.')],
      fogRange:    [T('Ray distance'), 128, 12000, 0, 'u', false, T('How far the fog is marched. Longer costs more.')],
      scatter:     [T('Forward scattering'), 0, 0.85, 2, '', false, T('How much light bends toward the camera, which makes shafts read.')],
      localFogRadius: [T('Local radius'), 0, 8000, 0, 'u', true, T('Confines the fog to a sphere. 0 keeps it global.')],
      groundFog:   [T('Density'), 0, 4, 2, '', true, T('A low layer lying on the floor. Players moving through it push it aside and leave trails that drift and fill back in. Around 1 it hides the floor; 0.2 is a thin veil.')],
      groundFogCoverage: [T('Coverage'), 0.05, 1, 2, '', false, T('How much of the floor the fog covers. Lower it for separate patches with clear ground between them. Moving air is easiest to see there.')],
      groundFogHeight: [T('Depth'), 8, 400, 0, 'u', false, T('How high the ground fog stands over the floor. It follows slopes, stairs and upper floors.')],
      groundFogDetail: [T('Billows'), 0, 1, 2, '', false, T('Rolling lumps in the top of the layer.')],
      groundFogBillowSize: [T('Billow size'), 60, 1000, 0, 'u', false, T('How wide those lumps are. Big ones read as slow banks of fog.')],
      groundFogSoftness: [T('Softness'), 0, 1, 2, '', false, T('How gradually the fog fades upward. 0 is a defined sheet with a top surface. 1 thins all the way up from the floor, like mist.')],
      groundFogWisps: [T('Wisps'), 0, 1, 2, '', false, T('Tears the fog into thin streaks you can see through. The moving air bends them, so wakes drag them along.')],
      groundFogWispSize: [T('Wisp size'), 8, 240, 0, 'u', false, T('How wide those streaks are.')],
      groundFogStreak: [T('Streak'), 0, 1, 2, '', false, T('How far the streaks stretch along the drift direction.')],
      groundFogChurn: [T('Churn'), 0, 5, 2, '', false, T('How fast the billows and wisps roll and change. It does not move the fog players pushed.')],
      groundFogSun: [T('Sun on fog'), 0, 3, 2, '', false, T('How bright the sun makes the fog. High values read as a white glow against fog in shade.')],
      groundFogShade: [T('Shade brightness'), 0, 3, 2, '', false, T('How bright the fog is where the sun does not reach it.')],
      groundFogFlow: [T('Push'), 0, 3, 2, '', false, T('How strongly players moving through the fog shove it aside. 0 leaves it undisturbed.')],
      groundFogPushSize: [T('Push size'), 0.2, 2, 2, '', false, T('How wide the mark a player leaves in the fog is. Lower keeps it close around their legs; higher parts a wide circle.')],
      groundFogRecovery: [T('Refill time'), 0.5, 60, 1, 's', false, T('Seconds a cleared trail takes to fill back in.')],
      groundFogSwirl: [T('Swirl'), 0, 2, 2, '', false, T('How much the wakes behind players curl into eddies.')],
      groundFogWind: [T('Drift speed'), 0, 200, 0, 'u/s', false, T('How fast the fog drifts and carries the trails downwind.')],
      groundFogWindAngle: [T('Drift direction'), 0, 360, 0, '°', false, T('Compass direction the fog drifts toward.')],
      bloom:       [T('Bloom'), 0, 2, 2, '', true, T('Glow around bright areas.')],
      bloomThreshold: [T('Bloom threshold'), 0.1, 8, 2, '', false, T('How bright a pixel must be before it blooms.')],
      saturation:  [T('Saturation'), 0, 2, 2, '', false, T('Colour intensity of the finished frame.')],
      contrast:    [T('Contrast'), 0.25, 2, 2, '', false, T('Tonal separation of the finished frame.')],
      vignette:    [T('Vignette'), 0, 1, 2, '', true, T('Darkening toward the frame edges.')],
      grain:       [T('Film grain'), 0, 0.1, 3, '', true, T('Fine noise over the frame.')],
      chromatic:   [T('Chromatic shift'), 0, 4, 2, '', true, T('Colour fringing toward the frame edges.')],
      snow:        [T('Snow coverage'), 0, 1, 2, '', true, T('Snow on upward-facing surfaces, including actors.')],
      puddles:     [T('Puddle coverage'), 0, 1, 2, '', true, T('Standing water on flat ground. Local radius uses the height band.')],
      wet:         [T('Wetness'), 0, 1, 2, '', true, T('Darkens and glosses surfaces as if rained on.')],
      puddleHeight:[T('Puddle height'), -4000, 8000, 0, 'u', false, T('World height the puddle band sits at when the coating is local.')],
      puddleDepth: [T('Height tolerance'), 1, 200, 0, 'u', false, T('How far either side of that height still counts as puddle when local.')],
      ripple:      [T('Ripples'), 0, 2, 2, '', true, T('Surface movement on the standing water.')],
      reflectionRoughness: [T('Reflection roughness'), 0, 1, 2, '', false, T('How sharp the wet-surface reflections are.')],
      maskScale:   [T('Patch scale'), 0.002, 0.1, 3, '', false, T('Size of the coverage patches.')],
      surfaceRadius: [T('Surface radius'), 0, 8000, 0, 'u', true, T('Confines the coatings to a sphere. 0 keeps them global.')],
      particleDetail: [T('Snowflake detail'), 0, 1, 2, '', false, T('Shape detail on individual flakes.')],
      turbulence:  [T('Turbulence'), 0, 8, 2, '', true, T('How much the precipitation tumbles as it falls.')],
      particleOpacity: [T('Opacity'), 0, 1, 2, '', false, T('How solid each particle reads.')],
      softness:    [T('Soft intersections'), 0.1, 64, 1, '', false, T('Fades particles as they meet geometry so they do not cut hard.')],
      streak:      [T('Rain streak length'), 1, 50, 0, '', false, T('How long each rain streak is drawn.')],
      particleLight: [T('Light response'), 0, 2, 2, '', false, T('How much added lights illuminate the precipitation.')],
      bakedFill:   [T('Unbaked fill'), 0, 4, 2, '', false, T('Flat light fed through the map’s own irradiance input, so albedo and occlusion still apply. 0 leaves unlit faces black.')]
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
)CFJS"
R"CFJS(
    // Switching the last pass off releases the renderer. Pending writes override the delayed
    // state mirror, while unrelated passes and placed objects keep their resources.
    function envPassPill(card, keys, tip, confirmOn, opts) {
      var memo = null;
      envPassResetters.push(function (resetKeys) {
        for (var i = 0; i < keys.length; i++)
          if (!resetKeys || resetKeys.indexOf(keys[i][0]) >= 0) { memo = null; return; }
      });
      // Whether a key being non-neutral means this card is on (see isOn for the ones that do not).
      function passCounts(key) {
        if (key === 'contactShadows' || key === 'clouds' || key === 'worldGi') return false;
        return !(opts && opts.rides && opts.rides.indexOf(key) >= 0);
      }
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
          if (!passCounts(keys[i][0])) continue;
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
            // What each key had: this session's memo, else the backend's held value. A key the
            // backend does not hold was neutral when the card went off.
            var vals = [], anyOn = false, j;
            for (j = 0; j < keys.length; j++) {
              var v = memo ? memo[j] : (held.hasOwnProperty(keys[j][0]) ? held[keys[j][0]] : keys[j][1]);
              vals.push(v);
              if (passCounts(keys[j][0]) && Math.abs(v - keys[j][1]) > 1e-4) anyOn = true;
            }
            // Nothing to bring back (a fresh card, or every pass was neutral): the defaults, or the
            // switch would stay off. Otherwise each key goes back exactly where it was -- a pass
            // that was off stays off, it does not come on at its default beside the others.
            for (j = 0; j < keys.length; j++)
              envCommand('climate ' + keys[j][0] + ' ' + (anyOn ? vals[j] : keys[j][2]));
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
      groundFogColorR: 0.93, groundFogColorG: 0.91, groundFogColorB: 0.87,
      groundFog: 0, groundFogCoverage: 0.85, groundFogHeight: 56, groundFogDetail: 0.6, groundFogFlow: 1,
      groundFogRecovery: 8, groundFogSwirl: 0.8, groundFogWind: 12, groundFogWindAngle: 30,
      groundFogSoftness: 0.35, groundFogWisps: 0.35, groundFogWispSize: 36, groundFogStreak: 0.2,
      groundFogBillowSize: 220, groundFogChurn: 1, groundFogSun: 1, groundFogShade: 1,
      groundFogShadeR: 1, groundFogShadeG: 1, groundFogShadeB: 1, groundFogPushSize: 0.55,
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
)CFJS"
R"CFJS(
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
      T('Turns the procedural sky on or off, with clouds and the dynamic lighting its sun casts '
      + '(shadows, ambient occlusion, sky occlusion). The cloud layer has its own switch on the '
      + 'Clouds page and the Lighting card keeps its own switch.'),
      // The first switch-on of each game session warns first ($ outlives a panel rebuild, not a
      // restart). The wording is the author's, verbatim.
      function (proceed) {
        if ($.MvmSkyWarned) { proceed(); return; }
        cfgConfirm(T('Sky'), T('This mode is very experimental and buggy and laggy as fuck. I might fix it.'),
          T('Turn on anyway'), function () { $.MvmSkyWarned = true; proceed(); });
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

    // One fog card: the height fog, the ground fog that lies on the floor, and the light both share.
    var envFogResetSpec = envResetCmds(
      ['fogDensity', 'fogStart', 'fogBase', 'fogHeight', 'fogNoise', 'fogRange', 'scatter', 'lightShafts',
       'localFogRadius', 'groundFog', 'groundFogCoverage', 'groundFogHeight', 'groundFogDetail', 'groundFogFlow',
       'groundFogRecovery', 'groundFogSwirl', 'groundFogWind', 'groundFogWindAngle',
       'groundFogColorR', 'groundFogColorG', 'groundFogColorB', 'groundFogSoftness', 'groundFogWisps', 'groundFogWispSize',
       'groundFogStreak', 'groundFogBillowSize', 'groundFogChurn', 'groundFogSun', 'groundFogShade',
       'groundFogShadeR', 'groundFogShadeG', 'groundFogShadeB', 'groundFogPushSize']);
    envFogResetSpec.cmds = envFogResetSpec.cmds.concat(envSceneResetCmds(['fogR', 'fogG', 'fogB']));
    var envFogReset = envFogSec.__headerReset(envFogResetSpec, 'env.fog');
    // Switching the card on brings both fogs and their light shafts; shafts alone are not the pass.
    var envFogPill = envPassPill(envFogSec, [['fogDensity', 0, 0.05], ['groundFog', 0, 1], ['lightShafts', 0, 1]],
      T('Bypasses the height and ground fog by dropping their density to zero, and puts them back where they were.'),
      null, { rides: ['lightShafts'] });

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
