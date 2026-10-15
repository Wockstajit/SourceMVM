#pragma once

// LENS cards of the CONFIG panel's CAMERA tab: DEPTH OF FIELD (pages Focus and Bokeh) and
// LENS FLARE (pages Look and Detail). JS fragment concatenated (as an adjacent raw string literal)
// into the script body assembled in ConfigHudJs.h, from inside ConfigHudCameraTabJs.h so the cards
// land BEFORE the tab footer (Panorama flows siblings in creation order).
//
// Every control here writes one lens parameter through `mirv_filmmaker lens set <key> <value>`; the
// keys, ranges and defaults live in one table in Lens/LensFxMath.h and the current values arrive as
// `st.lens` (Lens/LensFx.cpp, LensFx_AppendStateJson). Nothing in this file knows a range twice:
// the sliders below repeat the min/max only so the bar has something to draw against -- the
// backend clamps regardless.

R"CFJS(
    // ===================== LENS: shared helpers =========================
    var lensSliders = { dof: [], flare: [] }, lensToggles = { dof: [], flare: [] };
    function lensGroup(key) { return key.indexOf('dof.') === 0 ? 'dof' : 'flare'; }
    function lensSet(key, v) { cmd('mirv_filmmaker lens set ' + key + ' ' + v + ' quiet'); }
    function lensSlider(host, label, key, min, max, decimals, unit, tip, extra) {
      var o = { label: label, tip: tip, min: min, max: max, decimals: decimals, unit: unit || '',
                onChange: function (v) { lensSet(key, v); } };
      if (extra) for (var k in extra) o[k] = extra[k];
      var r = sliderRow(host, o);
      lensSliders[lensGroup(key)].push({ key: key, row: r });
      return r;
    }
    function lensToggle(host, label, key, tip) {
      var t = togglePill(host, label, tip, function () {
        var L = (st && st.lens) || {};
        lensSet(key, L[key] > 0.5 ? 0 : 1);
      });
      lensToggles[lensGroup(key)].push({ key: key, t: t });
      return t;
    }

    // ===================== DEPTH OF FIELD ===============================
    // A post pass over the finished world frame (never the HUD) that reads the engine's own depth
    // buffer, so the blur follows real distance. Two pages, at most eight controls on either:
    //   FOCUS  what is sharp: a look preset, how focus is decided (manual / auto / near-far zones),
    //          the aperture or the four zone distances, the largest blur.
    //   BOKEH  what a point of light turns into: the BOKEH GAMMA first (it is what makes the shape
    //          show at all), then the iris shape and its few adjustments.
    // Controls that mean nothing for the current choice are hidden, not left to do nothing.
    var dofSec = section(pageCamera, 'DEPTH OF FIELD', true, false, 'config.section.dof', '#c79bffff');
    var dofReset = dofSec.__headerReset(['mirv_filmmaker lens reset dof quiet'], 'cam.dof');
    var dofPill = dofSec.__headerPill(
      function () { cmd('mirv_filmmaker lens dof toggle quiet'); },
      'Blurs what is in front of and behind the focus distance, using the game depth buffer. The Bokeh page shapes the blur.');

    // ---- page: FOCUS ----
    var dofFocusPage = dofSec.__page('Focus');
    var DOF_PRESETS = [['cinematic','Cinematic'],['anamorphic','Anamorphic'],['portrait','Portrait'],
                       ['miniature','Miniature'],['hearts','Hearts'],['stars','Stars']];
    optionGrid(dofFocusPage, 'Look', DOF_PRESETS, 3,
      'A complete depth-of-field look: focus, blur and bokeh shape together. Applying one resets the DEPTH OF FIELD values first, so nothing from the previous look is left behind. It never switches the effect on or off.',
      function (key) { cmd('mirv_filmmaker lens preset dof ' + key + ' quiet'); }, 13);
    // One row for the three ways to choose what is sharp (they are two parameters underneath:
    // dof.mode for lens vs zones, dof.auto for autofocus).
    var dofMethod = modeRow(dofFocusPage, 'Focus', [['0', 'Manual'], ['1', 'Auto'], ['2', 'Zones']], '260px',
      function (k) {
        lensSet('dof.mode', k === '2' ? 1 : 0);
        lensSet('dof.auto', k === '1' ? 1 : 0);
      },
      'Manual: you set one sharp distance. Auto: the camera focuses on whatever is in the middle of the screen and eases there. Zones: you say where sharp starts and ends.', 13);
    var dofFocus = lensSlider(dofFocusPage, 'Focus distance', 'dof.focus', 10, 3000, 0, 'u',
      'How far from the camera is perfectly sharp, in world units (1 unit is about 2.5 cm).');
    var dofFstop = lensSlider(dofFocusPage, 'Aperture', 'dof.fstop', 0.7, 16, 1, '',
      'The f-stop. Lower is a wider opening and a shallower focus: f/1.4 is creamy, f/8 keeps most of the scene sharp.');
    var dofNearStart = lensSlider(dofFocusPage, 'Near: fully blurred', 'dof.nearStart', 0, 2000, 0, 'u',
      'At this distance and closer the foreground is blurred as much as it ever gets.');
    var dofNearEnd = lensSlider(dofFocusPage, 'Near: sharp from', 'dof.nearEnd', 0, 2000, 0, 'u',
      'The foreground blur fades out and everything is sharp from here.');
    var dofFarStart = lensSlider(dofFocusPage, 'Far: sharp until', 'dof.farStart', 0, 6000, 0, 'u',
      'Sharp up to this distance, then the background starts to blur.');
    var dofFarEnd = lensSlider(dofFocusPage, 'Far: fully blurred', 'dof.farEnd', 0, 12000, 0, 'u',
      'At this distance and beyond the background is blurred as much as it ever gets.');
    var dofMaxBlur = lensSlider(dofFocusPage, 'Largest blur', 'dof.maxBlur', 1, 90, 0, 'px',
      'The radius of the biggest blur disc, in pixels on a 1080p frame (it scales with the resolution). Caps both ways of focusing.');
    var dofWeaponToggle = lensToggle(dofFocusPage, 'Blur the weapon', 'dof.weapon',
      'The first-person weapon is drawn outside the world depth range. Off keeps it sharp; on blurs it as if it were held about 30 units from the lens.');
    var dofDebug = modeRow(dofFocusPage, 'Show', [['0', 'Picture'], ['1', 'Blur map'], ['2', 'Focus plane']], '260px',
      function (k) { lensSet('dof.debug', k); },
      'Blur map paints foreground blur orange and background blur blue by how strong it is. Focus plane paints what is sharp magenta -- the fastest way to see where your focus really is.', 13);
)CFJS"
R"CFJS(
    // ---- page: BOKEH ----
    // What one point of light becomes. The gamma comes first: the frame is already tonemapped, so a
    // lamp is only as bright as white and spreads into a faint ghost. The gamma lifts small bright
    // lights (in stops, before they are blurred) so the disc keeps its edge and the shape shows --
    // the same control as the gamma on a bokeh effect in a compositing app.
    var dofBokehPage = dofSec.__page('Bokeh');
    var dofGamma = lensSlider(dofBokehPage, 'Bokeh gamma', 'dof.gamma', 0, 12, 1, '',
      'Brings out the shape of the blur. Small bright lights are brightened before they are blurred, so each one becomes a clear, solid disc in the shape below. Raise it until the shape shows; 0 is a plain soft blur. Only lights on a darker surround are lifted, never walls or sky.', { offAtZero: true });
    var IRIS_SHAPES = [['0','Circle'],['1','Polygon'],['2','Star'],['3','Heart'],['4','Ring'],['5','Custom']];
    var dofShapeGrid = optionGrid(dofBokehPage, 'Shape', IRIS_SHAPES, 3,
      'The shape every out-of-focus point of light takes. Polygon is a real iris with blades, Ring is a mirror lens with a hole, Custom uses one of the shapes below or your own image.',
      function (key) { lensSet('dof.shape', key); }, 13);
    var dofBlades = lensSlider(dofBokehPage, 'Blades', 'dof.blades', 3, 12, 0, '',
      'How many aperture blades (Polygon) or points (Star).');
    var dofRoundness = lensSlider(dofBokehPage, 'Blade curve', 'dof.roundness', 0, 1, 2, '',
      'Real iris blades are slightly curved. 0 is a hard polygon, 1 is a circle.', { offAtZero: true });
    var dofRotation = lensSlider(dofBokehPage, 'Rotation', 'dof.rotation', -180, 180, 0, '°',
      'Turns the shape clockwise around its centre.');
    var dofSqueeze = lensSlider(dofBokehPage, 'Anamorphic squeeze', 'dof.squeeze', 1, 3, 2, 'x',
      'Stretches every disc into a tall oval, the way an anamorphic cinema lens does. 1.00 is round.');
    var dofEdge = lensSlider(dofBokehPage, 'Bright rim', 'dof.edge', 0, 3, 2, '',
      'Brightens the edge of each disc, like the ring in a lens with spherical aberration.', { offAtZero: true });
    var MASKS = [['diamond','Diamond'],['cross','Cross'],['crescent','Crescent'],['clover','Clover'],['smile','Smiley']];
    var dofMaskGrid = optionGrid(dofBokehPage, 'Custom shape', MASKS, 5,
      'The shapes Custom can use. Your own black-and-white image works too: run  mirv_filmmaker lens iris file <path>  in the console (white passes light).',
      function (key) { cmd('mirv_filmmaker lens iris mask ' + key + ' quiet'); }, 13);
    var dofQuality = modeRow(dofBokehPage, 'Quality', [['0', 'Low'], ['1', 'Medium'], ['2', 'High']], '260px',
      function (k) { lensSet('dof.quality', k); },
      'Samples per pixel (48 / 96 / 192). Higher makes big blurs smoother and shapes crisper, and costs more GPU time.', 13);

    renderHooks.push(function (st) {
      var L = st.lens; if (!L) return;
      dofPill.set(L['dof.on'] > 0.5);
      dofReset.setDisabled(!!L.dofDefault);
      for (var i = 0; i < lensSliders.dof.length; i++) lensSliders.dof[i].row.sync(L[lensSliders.dof[i].key]);
      for (var j = 0; j < lensToggles.dof.length; j++) lensToggles.dof[j].t.set(L[lensToggles.dof[j].key] > 0.5);
      var zone = L['dof.mode'] > 0.5, auto = L['dof.auto'] > 0.5;
      dofMethod.sync(zone ? '2' : (auto ? '1' : '0'));
      dofDebug.sync(String(L['dof.debug'] | 0));
      dofQuality.sync(String(L['dof.quality'] | 0));
      // What is sharp decides which controls mean anything, so the others are hidden. (In zones the
      // weapon switch gives way to keep the page at eight controls.)
      dofFocus.slot.visible = !zone && !auto;
      dofFstop.slot.visible = !zone;
      dofWeaponToggle.row.visible = !zone;
      dofNearStart.slot.visible = zone; dofNearEnd.slot.visible = zone;
      dofFarStart.slot.visible = zone; dofFarEnd.slot.visible = zone;
      var shape = L['dof.shape'] | 0;
      if (shape < 0 || shape >= IRIS_SHAPES.length) shape = 0;
      dofShapeGrid.sync(String(shape));
      dofBlades.slot.visible = (shape === 1 || shape === 2);
      dofRoundness.slot.visible = (shape === 1);
      dofRotation.slot.visible = (shape === 1 || shape === 2 || shape === 3 || shape === 5);
      dofMaskGrid.row.visible = (shape === 5);
      dofMaskGrid.sync(L.irisMask || '');
      dofSec.__note(L['dof.on'] > 0.5
        ? ((zone ? 'Zones' : ((auto ? 'Auto · ' : '') + 'f/' + (L['dof.fstop'] || 0).toFixed(1)))
           + ' · ' + IRIS_SHAPES[shape][1].toLowerCase()
           + (L['dof.gamma'] > 0 ? ' · gamma ' + L['dof.gamma'].toFixed(1) : ''))
        : 'Off');
    });
)CFJS"
R"CFJS(
    // ===================== LENS FLARE ===================================
    // Screen-space flare built from the hot spots of the finished frame: ghosts shaped like the aperture,
    // a ring, an anamorphic streak, diffraction spikes, a soft glow and lens dirt. It lands in every
    // recorded frame and never on the HUD. Only lights throw a flare (small, brighter than what surrounds
    // them); a bright wall, the sky or a lit doorway never does, and the flare can never push a frame to
    // white. Two pages, at most eight controls on either:
    //   LOOK    a look preset, the intensity and threshold, and the amount of each part of the flare.
    //   DETAIL  the streak length, the aperture blades, dirt, the two colours and a flare-only view.
    var flareSec = section(pageCamera, 'LENS FLARE', true, false, 'config.section.flare', '#ffb36bff');
    var flareReset = flareSec.__headerReset(['mirv_filmmaker lens reset flare quiet'], 'cam.flare');
    var flarePill = flareSec.__headerPill(
      function () { cmd('mirv_filmmaker lens flare toggle quiet'); },
      'Adds lens flare from the brightest small lights in the picture. The Look page sets what it looks like, the Detail page fine-tunes it.');

    // ---- page: LOOK ----
    var flareLookPage = flareSec.__page('Look');
    var FLARE_PRESETS = [['cinematic','Cinematic'],['anamorphic','Anamorphic'],['classic','Classic'],['star','Star'],['dreamy','Dreamy']];
    optionGrid(flareLookPage, 'Look', FLARE_PRESETS, 3,
      'A complete flare: which parts show and how strong, plus the colours. Applying one resets the LENS FLARE values first. It never switches the effect on or off.',
      function (key) { cmd('mirv_filmmaker lens preset flare ' + key + ' quiet'); }, 13);
    lensSlider(flareLookPage, 'Intensity', 'flare.intensity', 0, 2, 2, 'x',
      'How strong the whole flare is. It brightens smoothly and can never push a normal frame to white.', { offAtZero: true });
    lensSlider(flareLookPage, 'Source threshold', 'flare.threshold', 0.75, 1, 2, '',
      'How bright a light must be to throw a flare. About 0.85 catches lamps and the sun, 0.95 keeps only pure white hot spots, 1.00 turns every source off. Big bright areas such as the sky or a wall never flare, whatever this is set to.');
    lensSlider(flareLookPage, 'Ghosts', 'flare.ghosts', 0, 8, 0, '',
      'How many reflections trail from each light. They sit on the straight line from the light through the middle of the frame, and each takes the shape of the aperture.', { offAtZero: true });
    lensSlider(flareLookPage, 'Halo', 'flare.halo', 0, 2, 2, 'x',
      'A faint ring around the middle of the frame that passes through each light.', { offAtZero: true });
    lensSlider(flareLookPage, 'Streak', 'flare.streak', 0, 3, 2, 'x',
      'The long horizontal line a cinema anamorphic lens draws through every light.', { offAtZero: true });
    lensSlider(flareLookPage, 'Spikes', 'flare.star', 0, 3, 2, 'x',
      'Thin spikes running out of every light, the way a stopped-down lens diffracts it.', { offAtZero: true });
    lensSlider(flareLookPage, 'Glow', 'flare.glow', 0, 3, 2, 'x',
      'A soft veil of glare around each light, like light bouncing inside the lens.', { offAtZero: true });

    // ---- page: DETAIL ----
    var flareDetailPage = flareSec.__page('Detail');
    lensSlider(flareDetailPage, 'Streak length', 'flare.streakLen', 0.05, 1, 2, '',
      'How far the streak reaches across the frame.');
    lensSlider(flareDetailPage, 'Aperture blades', 'flare.blades', 3, 12, 0, '',
      'Sets the shape of every ghost and how many spikes a light gets. Six blades make hexagon ghosts and six spikes; an odd number makes twice as many spikes.');
    lensSlider(flareDetailPage, 'Lens dirt', 'flare.dirt', 0, 1, 2, '',
      'Smudges on the front element that light up where flare falls on them. Load your own dirt image with  mirv_filmmaker lens dirt <path>  (the shape is read as brightness).', { offAtZero: true });
    var FLARE_TINTS = [
      ['0', '#ffd9b8ff', 'warm'],  ['1', '#ffffffff', 'white'], ['2', '#ffc857ff', 'gold'],   ['3', '#b8d4ffff', 'cool'],
      ['4', '#7fe0d4ff', 'teal'],  ['5', '#ffa6c4ff', 'rose'],  ['6', '#c4a6ffff', 'violet'], ['7', '#ff8a5cff', 'ember']
    ];
    subHead(flareDetailPage, 'FLARE COLOUR');
    var flareTintGrid = swatchGrid(flareDetailPage, FLARE_TINTS, function (idx) { lensSet('flare.tint', idx); },
      { label: 'Flare colour', perRow: 8, chipW: 40, marginTop: '4px' });
    var STREAK_TINTS = [
      ['0', '#5c94ffff', 'blue'],  ['1', '#5cf0ffff', 'cyan'],  ['2', '#ffffffff', 'white'],
      ['3', '#ffb14dff', 'amber'], ['4', '#ff5c5cff', 'red'],   ['5', '#ff5cd8ff', 'magenta']
    ];
    subHead(flareDetailPage, 'STREAK COLOUR');
    var flareStreakGrid = swatchGrid(flareDetailPage, STREAK_TINTS, function (idx) { lensSet('flare.streakTint', idx); },
      { label: 'Streak colour', perRow: 8, chipW: 40, marginTop: '4px' });
    var flareDebug = lensToggle(flareDetailPage, 'Flare only', 'flare.debug',
      'Shows the flare on black, so you can judge and tune it without the scene behind it.');

    renderHooks.push(function (st) {
      var L = st.lens; if (!L) return;
      flarePill.set(L['flare.on'] > 0.5);
      flareReset.setDisabled(!!L.flareDefault);
      for (var i = 0; i < lensSliders.flare.length; i++) lensSliders.flare[i].row.sync(L[lensSliders.flare[i].key]);
      for (var j = 0; j < lensToggles.flare.length; j++) lensToggles.flare[j].t.set(L[lensToggles.flare[j].key] > 0.5);
      flareTintGrid.select(String(L['flare.tint'] | 0));
      flareStreakGrid.select(String(L['flare.streakTint'] | 0));
      flareSec.__note(L['flare.on'] > 0.5
        ? ((L['flare.ghosts'] | 0) + ' ghosts · streak ' + (L['flare.streak'] > 0 ? 'on' : 'off')
           + ' · spikes ' + (L['flare.star'] > 0 ? 'on' : 'off') + ' · from ' + L['flare.threshold'].toFixed(2))
        : 'Off');
    });
)CFJS"
