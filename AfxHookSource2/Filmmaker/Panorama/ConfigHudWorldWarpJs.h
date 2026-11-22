#pragma once

// WORLD EFFECTS card in CONFIG > EFFECTS (WorldWarp.h):
// Little Planet (360 stereographic), Inception, Rolling Horizon, World Wave, World Fold.
// JS fragment concatenated into ConfigHudJs.h.

R"CFJS(
    var wwSec = section(pageEffects, T('WORLD EFFECTS'), true, false, 'fx.worldwarp', '#7fd4ffff');
    var wwReset = wwSec.__headerReset(['mirv_filmmaker worldwarp reset'], 'fx.worldwarp');
    var wwPill = wwSec.__headerPill(function () {
      var w = (st && st.worldWarp) || {};
      cmd(w.on ? 'mirv_filmmaker worldwarp off' : 'mirv_filmmaker worldwarp mode littleplanet preset');
    }, T('Bends the 3D world: Little Planet, Inception, Rolling Horizon, World Wave, and World Fold.'));

    var WW_MODES = [
      ['off', T('Off')],
      ['littleplanet', T('Little Planet')],
      ['inception', T('Inception')],
      ['horizon', T('Rolling Horizon')],
      ['wave', T('World Wave')],
      ['fold', T('World Fold')]
    ];
    var wwModeGrid = optionGrid(wwSec, T('World preset'), WW_MODES, 3,
      T('Little Planet puts the street in the center with buildings fanning out and sky around the rim. Inception and Rolling Horizon curve the far street up or down. World Wave rolls a bend across the map. World Fold lifts one side overhead around a movable hinge axis.'),
      function (k) { cmd('mirv_filmmaker worldwarp mode ' + k + ' preset'); }, 12);

    function wwSet(k, v) { cmd('mirv_filmmaker worldwarp set ' + k + ' ' + v); }
    function wwSlider(parent, label, key, min, max, dec, unit, tip) {
      return sliderRow(parent, {
        label: label, min: min, max: max, decimals: dec || 0, unit: unit || '', tip: tip,
        onChange: function (v) { wwSet(key, v); }
      });
    }

    // ---- Little Planet controls ----
    var wwPlanetBox = mk('Panel', wwSec); wwPlanetBox.style.width = '100%'; wwPlanetBox.style.flowChildren = 'down';
    subHead(wwPlanetBox, T('Little Planet'));
    var wwPlanetScale = wwSlider(wwPlanetBox, T('Planet zoom'), 'planetScale', 0.35, 2.5, 2, 'x',
      T('Controls how much of the street and surrounding buildings fill the planet disc.'));
    var wwPlanetSpin = wwSlider(wwPlanetBox, T('Spin'), 'planetSpin', -180, 180, 0, '°',
      T('Rotates the planet around the center street.'));
    var wwPlanetTilt = wwSlider(wwPlanetBox, T('Pole tilt'), 'planetTilt', -45, 45, 0, '°',
      T('Shifts the center pole slightly away from straight down.'));
    var wwPlanetConformal = sliderRow(wwPlanetBox, {
      label: T('Edge detail'), min: 0, max: 100, decimals: 0, unit: '%',
      tip: T('Conformal stereographic balance. High values keep rooftops and building faces near the rim recognizable.'),
      onChange: function (v) { wwSet('planetConformal', (v / 100).toFixed(3)); }
    });
    var wwPlanetRim = sliderRow(wwPlanetBox, {
      label: T('Sky rim softness'), min: 0, max: 100, decimals: 0, unit: '%',
      tip: T('Softens the sky wrapping around the outer rim of the planet.'),
      onChange: function (v) { wwSet('planetRimSoftness', (v / 100).toFixed(3)); }
    });

    // ---- Inception / Rolling Horizon controls ----
    var wwHorizonBox = mk('Panel', wwSec); wwHorizonBox.style.width = '100%'; wwHorizonBox.style.flowChildren = 'down';
    subHead(wwHorizonBox, T('Rolling Horizon'));
    var wwHorizonBend = wwSlider(wwHorizonBox, T('Horizon bend'), 'horizonBend', -2.0, 2.0, 2, '',
      T('Positive bends the far street upward (Inception); negative curves it downward over a horizon.'));
    var wwHorizonFlat = wwSlider(wwHorizonBox, T('Flat zone'), 'horizonFlatRadius', 0, 2000, 0, 'u',
      T('Distance around the subject kept flat before the bend begins.'));
    var wwHorizonCurve = wwSlider(wwHorizonBox, T('Curve steepness'), 'horizonCurve', 1.2, 3.0, 2, '',
      T('How sharply the bend rises with distance.'));

    // ---- World Wave controls ----
    var wwWaveBox = mk('Panel', wwSec); wwWaveBox.style.width = '100%'; wwWaveBox.style.flowChildren = 'down';
    subHead(wwWaveBox, T('World Wave'));
    var wwWaveHeight = wwSlider(wwWaveBox, T('Wave height'), 'waveHeight', 0, 500, 0, 'u',
      T('Vertical height of the rolling bend across the map.'));
    var wwWaveLen = wwSlider(wwWaveBox, T('Wavelength'), 'waveLength', 80, 3000, 0, 'u',
      T('World distance between wave crests.'));
    var wwWaveSpeed = wwSlider(wwWaveBox, T('Wave speed'), 'waveSpeed', -15, 15, 1, '',
      T('How fast the wave rolls across the map (synced to demo time).'));
    var wwWaveDir = wwSlider(wwWaveBox, T('Direction'), 'waveDirection', 0, 360, 0, '°',
      T('Compass direction the wave travels along.'));
    var wwWaveRadial = togglePill(wwWaveBox, T('Radial ripple'),
      T('On: ripples expand outward in rings from the origin. Off: travels along Direction.'),
      function () {
        var w = (st && st.worldWarp) || {};
        wwSet('waveRadial', w.waveRadial ? 0 : 1);
      });
    var wwWaveFlat = wwSlider(wwWaveBox, T('Stable near camera'), 'waveFlatRadius', 0, 1000, 0, 'u',
      T('Keeps the ground right next to the camera calm before the wave builds.'));

    // ---- World Fold controls ----
    var wwFoldBox = mk('Panel', wwSec); wwFoldBox.style.width = '100%'; wwFoldBox.style.flowChildren = 'down';
    subHead(wwFoldBox, T('World Fold'));
    var wwFoldAngle = wwSlider(wwFoldBox, T('Fold angle'), 'foldAngle', -180, 180, 0, '°',
      T('Lifts one side of the scene around the hinge axis. Around 90° brings a row of buildings overhead.'));
    var wwFoldYaw = wwSlider(wwFoldBox, T('Axis direction'), 'foldAxisYaw', 0, 360, 0, '°',
      T('Compass orientation of the fold hinge line.'));
    var wwFoldOffset = wwSlider(wwFoldBox, T('Axis distance'), 'foldOffset', -2000, 2000, 0, 'u',
      T('Slides the fold hinge forward or back across the street.'));
    var wwFoldSoft = wwSlider(wwFoldBox, T('Hinge softness'), 'foldSoftness', 0, 800, 0, 'u',
      T('How smoothly the ground curves into the lifted wall.'));
    var wwFoldFlip = togglePill(wwFoldBox, T('Flip lifted side'),
      T('Lifts the opposite side of the hinge axis.'),
      function () {
        var w = (st && st.worldWarp) || {};
        wwSet('foldFlipSide', w.foldFlipSide ? 0 : 1);
      });

    // ---- Shared Scene Origin (Rolling Horizon / World Wave / World Fold) ----
    var wwOriginBox = mk('Panel', wwSec); wwOriginBox.style.width = '100%'; wwOriginBox.style.flowChildren = 'down';
    subHead(wwOriginBox, T('Scene origin'));
    actionGroup(wwOriginBox, T('Origin'), [
      [T('Place at camera'), function () { cmd('mirv_filmmaker worldwarp origin camera'); }],
      [T('Follow camera'), function () { cmd('mirv_filmmaker worldwarp origin clear'); }]
    ], 2, T('Pin the bend/wave/fold origin in the scene at the current camera position, or keep it centered on the moving camera.'));
    var wwOriginNote = lbl(wwOriginBox, '', S.hint, 11);
    wwOriginNote.hittest = false; wwOriginNote.style.marginTop = '4px';
    wwOriginNote.style.paddingLeft = UI.line.padH + 'px';

    renderHooks.push(function (st) {
      var w = st && st.worldWarp; if (!w) return;
      var m = w.mode || 'off';
      wwPill.set(!!w.on);
      wwReset.setDisabled(!w.on);
      wwModeGrid.sync(m);
      wwPlanetBox.visible = (m === 'littleplanet');
      wwHorizonBox.visible = (m === 'inception' || m === 'horizon');
      wwWaveBox.visible = (m === 'wave');
      wwFoldBox.visible = (m === 'fold');
      wwOriginBox.visible = (m === 'inception' || m === 'horizon' || m === 'wave' || m === 'fold');
      wwPlanetScale.sync(w.planetScale);
      wwPlanetSpin.sync(w.planetSpin);
      wwPlanetTilt.sync(w.planetTilt);
      wwPlanetConformal.sync(Math.round((w.planetConformal || 0) * 100));
      wwPlanetRim.sync(Math.round((w.planetRimSoftness || 0) * 100));
      wwHorizonBend.sync(w.horizonBend);
      wwHorizonFlat.sync(w.horizonFlatRadius);
      wwHorizonCurve.sync(w.horizonCurve);
      wwWaveHeight.sync(w.waveHeight);
      wwWaveLen.sync(w.waveLength);
      wwWaveSpeed.sync(w.waveSpeed);
      wwWaveDir.sync(w.waveDirection);
      wwWaveRadial.set(!!w.waveRadial);
      wwWaveFlat.sync(w.waveFlatRadius);
      wwFoldAngle.sync(w.foldAngle);
      wwFoldYaw.sync(w.foldAxisYaw);
      wwFoldOffset.sync(w.foldOffset);
      wwFoldSoft.sync(w.foldSoftness);
      wwFoldFlip.set(!!w.foldFlipSide);
      var o = w.origin || [0, 0, 0];
      wwOriginNote.text = w.useSceneOrigin
        ? T('Pinned in scene at ({0}, {1}, {2})', Math.round(o[0]), Math.round(o[1]), Math.round(o[2]))
        : T('Following camera position.');
      var labels = { off: T('Off'), littleplanet: T('Little Planet'), inception: T('Inception'),
                     horizon: T('Rolling Horizon'), wave: T('World Wave'), fold: T('World Fold') };
      wwSec.__note(labels[m] || T('Off'));
    });
)CFJS"
