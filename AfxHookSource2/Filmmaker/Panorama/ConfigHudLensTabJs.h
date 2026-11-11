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
    // buffer, so the blur follows real distance. Two pages:
    //   FOCUS  what is sharp and how strongly the rest blurs: a look preset, how focus is decided
    //          (manual / auto / near-far zones), then for Manual and Auto the aperture and the lens
    //          (Follow FOV, focal length, foreground blur) and the sharp band, or the four zone
    //          distances, the largest blur.
    //   BOKEH  what a point of light turns into: the BOKEH GAMMA first (it is what makes the shape
    //          show at all), then the iris shape and its few adjustments.
    // Controls that mean nothing for the current choice are hidden, not left to do nothing.
    var dofSec = section(pageCamera, T('DEPTH OF FIELD'), true, false, 'config.section.dof', '#c79bffff');
    var dofReset = dofSec.__headerReset(['mirv_filmmaker lens reset dof quiet'], 'cam.dof');
    var dofPill = dofSec.__headerPill(
      function () { cmd('mirv_filmmaker lens dof toggle quiet'); },
      T('Blurs what is in front of and behind the focus distance, using the game depth buffer. The Bokeh page shapes the blur.'));

    // ---- page: FOCUS ----
    var dofFocusPage = dofSec.__page(T('Focus'));
    var DOF_PRESETS = [['cinematic',T('Cinematic')],['anamorphic',T('Anamorphic')],['portrait',T('Portrait')],
                       ['miniature',T('Miniature')],['hearts',T('Hearts')],['stars',T('Stars')]];
    optionGrid(dofFocusPage, T('Look'), DOF_PRESETS, 3,
      T('A complete depth-of-field look: focus, blur and bokeh shape together. Applying one resets the DEPTH OF FIELD values first, so nothing from the previous look is left behind. It never switches the effect on or off.'),
      function (key) { cmd('mirv_filmmaker lens preset dof ' + key + ' quiet'); }, 13);
    // One row for the three ways to choose what is sharp (they are two parameters underneath:
    // dof.mode for lens vs zones, dof.auto for autofocus).
    var dofMethod = modeRow(dofFocusPage, T('Focus'), [['0', T('Manual')], ['1', T('Auto')], ['2', T('Zones')]], '260px',
      function (k) {
        lensSet('dof.mode', k === '2' ? 1 : 0);
        lensSet('dof.auto', k === '1' ? 1 : 0);
      },
      T('Manual: you set one sharp distance. Auto: the camera focuses on whatever is in the middle of the screen and eases there at the focus speed. Zones: you say where sharp starts and ends.'), 13);
    var dofFocus = lensSlider(dofFocusPage, T('Focus distance'), 'dof.focus', 10, 3000, 0, 'u',
      T('How far from the camera is perfectly sharp, in world units (1 unit is about 2.5 cm).'));
    var dofFocusSpeed = lensSlider(dofFocusPage, T('Focus speed'), 'dof.focusSpeed', 0.25, 30, 2, '',
      T('How fast autofocus moves to a new distance. Low drifts over like a slow focus pull; the far right (30) jumps instantly.'));
    var dofFstop = lensSlider(dofFocusPage, T('Aperture'), 'dof.fstop', 0.7, 16, 1, '',
      T('The f-stop. Lower is a wider opening and a shallower focus: f/1.4 is creamy, f/8 keeps most of the scene sharp.'));
    // The lens behind Manual and Auto: its focal length is the camera's own (from the field of view
    // the frame is drawn with, so zooming in lengthens it) or a set one, and the foreground blur says
    // how much of the blur in front of the focus is kept. Zones hide all three, like the aperture.
    var dofFollowFov = lensToggle(dofFocusPage, T('Follow FOV'), 'dof.followFov',
      T('Takes the focal length from the camera field of view, like a zoom lens: zoom in (a lower FOV) and the background blurs much more. The normal CS2 FOV of 90 is a very wide lens of about 14 mm, so expect little blur there.'));
    var dofFocal = lensSlider(dofFocusPage, T('Focal length'), 'dof.focalLength', 10, 300, 0, 'mm',
      T('The lens, in millimetres on a full-frame camera. A longer lens blurs the background much more at the same aperture: 35 mm is wide, 50 mm normal, 85 to 135 mm a portrait look, 200 mm and up a telephoto.'));
    var dofForeground = lensSlider(dofFocusPage, T('Foreground blur'), 'dof.foreground', 0, 100, 0, '%',
      T('How much of the blur in front of the focus distance is kept. 100% is a real lens. At Off everything in front of the focus stays sharp and only the background blurs.'), { offAtZero: true });
    var dofRange = lensSlider(dofFocusPage, T('Sharp band'), 'dof.range', 0, 1000, 0, 'u',
      T('How far in front of and behind the focus distance everything stays perfectly sharp, in world units. 0 is one sharp distance; raise it to keep a whole player or doorway sharp while the rest still blurs.'), { offAtZero: true });
    var dofNearStart = lensSlider(dofFocusPage, T('Near: fully blurred'), 'dof.nearStart', 0, 2000, 0, 'u',
      T('At this distance and closer the foreground is blurred as much as it ever gets.'));
    var dofNearEnd = lensSlider(dofFocusPage, T('Near: sharp from'), 'dof.nearEnd', 0, 2000, 0, 'u',
      T('The foreground blur fades out and everything is sharp from here.'));
    var dofFarStart = lensSlider(dofFocusPage, T('Far: sharp until'), 'dof.farStart', 0, 6000, 0, 'u',
      T('Sharp up to this distance, then the background starts to blur.'));
    var dofFarEnd = lensSlider(dofFocusPage, T('Far: fully blurred'), 'dof.farEnd', 0, 12000, 0, 'u',
      T('At this distance and beyond the background is blurred as much as it ever gets.'));
    var dofMaxBlur = lensSlider(dofFocusPage, T('Largest blur'), 'dof.maxBlur', 1, 90, 0, 'px',
      T('The radius of the biggest blur disc, in pixels on a 1080p frame (it scales with the resolution). Caps both ways of focusing.'));
    var dofWeaponToggle = lensToggle(dofFocusPage, T('Blur the weapon'), 'dof.weapon',
      T('The first-person weapon is drawn outside the world depth range. Off keeps it sharp; on blurs it as if it were held about 30 units from the lens.'));
    var dofDebug = modeRow(dofFocusPage, T('Show'), [['0', T('Picture')], ['1', T('Blur map')], ['2', T('Focus')], ['3', T('Zebra')]], '260px',
      function (k) { lensSet('dof.debug', k); },
      T('Blur map paints foreground blur orange and background blur blue by how strong it is. Focus paints what is sharp magenta -- the fastest way to see where your focus really is. Zebra draws stripes over what is sharp on top of the finished picture.'), 13);
)CFJS"
R"CFJS(
    // ---- page: BOKEH ----
    // What one point of light becomes. The gamma comes first: the frame is already tonemapped, so a
    // lamp is only as bright as white and spreads into a faint ghost. The gamma lifts small bright
    // lights (in stops, before they are blurred) so the disc keeps its edge and the shape shows --
    // the same control as the gamma on a bokeh effect in a compositing app.
    var dofBokehPage = dofSec.__page(T('Bokeh'));
    var dofGamma = lensSlider(dofBokehPage, T('Bokeh gamma'), 'dof.gamma', 0, 12, 1, '',
      T('Brings out the shape of the blur. Small bright lights are brightened before they are blurred, so each one becomes a clear, solid disc in the shape below. Raise it until the shape shows; 0 is a plain soft blur. Only lights on a darker surround are lifted, never walls or sky.'), { offAtZero: true });
    var IRIS_SHAPES = [['0',T('Circle')],['1',T('Polygon')],['2',T('Star')],['3',T('Heart')],['4',T('Ring')],['5',T('Custom')]];
    var dofShapeGrid = optionGrid(dofBokehPage, T('Shape'), IRIS_SHAPES, 3,
      T('The shape every out-of-focus point of light takes. Polygon is a real iris with blades, Ring is a mirror lens with a hole, Custom uses one of the shapes below or your own image.'),
      function (key) { lensSet('dof.shape', key); }, 13);
    var dofBlades = lensSlider(dofBokehPage, T('Blades'), 'dof.blades', 3, 12, 0, '',
      T('How many aperture blades (Polygon) or points (Star).'));
    var dofRoundness = lensSlider(dofBokehPage, T('Blade curve'), 'dof.roundness', -1, 1, 2, '',
      T('Real iris blades are slightly curved. 0 is a hard polygon, 1 is a circle. Below 0 the blades bend inward and the polygon becomes a curved star with the same number of points.'), { offAtZero: true });
    var dofRotation = lensSlider(dofBokehPage, T('Rotation'), 'dof.rotation', -180, 180, 0, '°',
      T('Turns the shape clockwise around its centre.'));
    var dofSqueeze = lensSlider(dofBokehPage, T('Anamorphic squeeze'), 'dof.squeeze', 1, 3, 2, 'x',
      T('Stretches every disc into a tall oval, the way an anamorphic cinema lens does. 1.00 is round.'));
    var dofEdge = lensSlider(dofBokehPage, T('Rim'), 'dof.edge', -1, 3, 2, '',
      T('How the light is spread across each disc. Above 0 the edge is brighter, like the ring in a lens with spherical aberration. Below 0 the light gathers in the middle and fades toward the edge, a soft creamy disc. 0 is even.'), { offAtZero: true });
    var MASKS = [['diamond',T('Diamond')],['cross',T('Cross')],['crescent',T('Crescent')],['clover',T('Clover')],['smile',T('Smiley')]];
    var dofMaskGrid = optionGrid(dofBokehPage, T('Custom shape'), MASKS, 5,
      T('The shapes Custom can use. Your own black-and-white image works too: run  mirv_filmmaker lens iris file <path>  in the console (white passes light).'),
      function (key) { cmd('mirv_filmmaker lens iris mask ' + key + ' quiet'); }, 13);
    var dofQuality = modeRow(dofBokehPage, T('Quality'), [['0', T('Low')], ['1', T('Medium')], ['2', T('High')]], '260px',
      function (k) { lensSet('dof.quality', k); },
      T('Samples per pixel (48 / 96 / 192). Higher makes big blurs smoother and shapes crisper, and costs more GPU time.'), 13);

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
      dofFocusSpeed.slot.visible = auto;
      dofFstop.slot.visible = !zone;
      dofRange.slot.visible = !zone;
      dofWeaponToggle.row.visible = !zone;
      dofNearStart.slot.visible = zone; dofNearEnd.slot.visible = zone;
      dofFarStart.slot.visible = zone; dofFarEnd.slot.visible = zone;
      // The lens rows: Follow FOV hides the set focal length and says which one the camera gives
      // (focalNow / hfov come from the last frame drawn, so only while the effect runs).
      var follow = L['dof.followFov'] > 0.5, dofOn = L['dof.on'] > 0.5;
      var focalNow = (follow && dofOn && L.focalNow > 0) ? L.focalNow : L['dof.focalLength'];
      dofFollowFov.row.visible = !zone;
      dofFocal.slot.visible = !zone && !follow;
      dofForeground.slot.visible = !zone;
      dofFollowFov.state((follow && dofOn && L.focalNow > 0)
        ? T('{0} mm at {1}°', L.focalNow.toFixed(0), (L.hfov || 0).toFixed(0)) : '');
      var shape = L['dof.shape'] | 0;
      if (shape < 0 || shape >= IRIS_SHAPES.length) shape = 0;
      dofShapeGrid.sync(String(shape));
      dofBlades.slot.visible = (shape === 1 || shape === 2);
      dofRoundness.slot.visible = (shape === 1);
      dofRotation.slot.visible = (shape === 1 || shape === 2 || shape === 3 || shape === 5);
      dofMaskGrid.row.visible = (shape === 5);
      dofMaskGrid.sync(L.irisMask || '');
      dofSec.__note(dofOn
        ? ((zone ? T('Zones') : ((auto ? T('Auto') + ' · ' : '') + (focalNow || 0).toFixed(0) + 'mm f/' + (L['dof.fstop'] || 0).toFixed(1)))
           + ' · ' + IRIS_SHAPES[shape][1].toLowerCase()
           + (L['dof.gamma'] > 0 ? ' · ' + T('gamma {0}', L['dof.gamma'].toFixed(1)) : ''))
        : T('Off'));
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
    var flareSec = section(pageCamera, T('LENS FLARE'), true, false, 'config.section.flare', '#ffb36bff');
    var flareReset = flareSec.__headerReset(['mirv_filmmaker lens reset flare quiet'], 'cam.flare');
    var flarePill = flareSec.__headerPill(
      function () { cmd('mirv_filmmaker lens flare toggle quiet'); },
      T('Adds lens flare from the brightest small lights in the picture. The Look page sets what it looks like, the Detail page fine-tunes it.'));

    // ---- page: LOOK ----
    var flareLookPage = flareSec.__page(T('Look'));
    var FLARE_PRESETS = [['cinematic',T('Cinematic')],['anamorphic',T('Anamorphic')],['classic',T('Classic')],['star',T('Star')],['dreamy',T('Dreamy')]];
    optionGrid(flareLookPage, T('Look'), FLARE_PRESETS, 3,
      T('A complete flare: which parts show and how strong, plus the colours. Applying one resets the LENS FLARE values first. It never switches the effect on or off.'),
      function (key) { cmd('mirv_filmmaker lens preset flare ' + key + ' quiet'); }, 13);
    lensSlider(flareLookPage, T('Intensity'), 'flare.intensity', 0, 2, 2, 'x',
      T('How strong the whole flare is. It brightens smoothly and can never push a normal frame to white.'), { offAtZero: true });
    lensSlider(flareLookPage, T('Source threshold'), 'flare.threshold', 0.75, 1, 2, '',
      T('How bright a light must be to throw a flare. About 0.85 catches lamps and the sun, 0.95 keeps only pure white hot spots, 1.00 turns every source off. Big bright areas such as the sky or a wall never flare, whatever this is set to.'));
    lensSlider(flareLookPage, T('Ghosts'), 'flare.ghosts', 0, 8, 0, '',
      T('How many reflections trail from each light. They sit on the straight line from the light through the middle of the frame, and each takes the shape of the aperture.'), { offAtZero: true });
    lensSlider(flareLookPage, T('Halo'), 'flare.halo', 0, 2, 2, 'x',
      T('A faint ring around the middle of the frame that passes through each light.'), { offAtZero: true });
    lensSlider(flareLookPage, T('Streak'), 'flare.streak', 0, 3, 2, 'x',
      T('The long horizontal line a cinema anamorphic lens draws through every light.'), { offAtZero: true });
    lensSlider(flareLookPage, T('Spikes'), 'flare.star', 0, 3, 2, 'x',
      T('Thin spikes running out of every light, the way a stopped-down lens diffracts it.'), { offAtZero: true });
    lensSlider(flareLookPage, T('Glow'), 'flare.glow', 0, 3, 2, 'x',
      T('A soft veil of glare around each light, like light bouncing inside the lens.'), { offAtZero: true });

    // ---- page: DETAIL ----
    var flareDetailPage = flareSec.__page(T('Detail'));
    lensSlider(flareDetailPage, T('Streak length'), 'flare.streakLen', 0.05, 1, 2, '',
      T('How far the streak reaches across the frame.'));
    lensSlider(flareDetailPage, T('Aperture blades'), 'flare.blades', 3, 12, 0, '',
      T('Sets the shape of every ghost and how many spikes a light gets. Six blades make hexagon ghosts and six spikes; an odd number makes twice as many spikes.'));
    lensSlider(flareDetailPage, T('Sun'), 'flare.sun', 0, 2, 2, 'x',
      T('How strongly the sun itself flares. It is found from the sky, so a sun on a bright sky still flares; walls and smoke in front of it block it. 0 leaves the sun to the ordinary light test.'), { offAtZero: true });
    lensSlider(flareDetailPage, T('Lens dirt'), 'flare.dirt', 0, 1, 2, '',
      T('Smudges on the front element that light up where flare falls on them. Load your own dirt image with  mirv_filmmaker lens dirt <path>  (the shape is read as brightness).'), { offAtZero: true });
    var FLARE_TINTS = [
      ['0', '#ffd9b8ff', T('warm')],  ['1', '#ffffffff', T('white')], ['2', '#ffc857ff', T('gold')],   ['3', '#b8d4ffff', T('cool')],
      ['4', '#7fe0d4ff', T('teal')],  ['5', '#ffa6c4ff', T('rose')],  ['6', '#c4a6ffff', T('violet')], ['7', '#ff8a5cff', T('ember')]
    ];
    subHead(flareDetailPage, T('FLARE COLOUR'));
    var flareTintGrid = swatchGrid(flareDetailPage, FLARE_TINTS, function (idx) { lensSet('flare.tint', idx); },
      { label: T('Flare colour'), perRow: 8, chipW: 40, marginTop: '4px' });
    var STREAK_TINTS = [
      ['0', '#5c94ffff', T('blue')],  ['1', '#5cf0ffff', T('cyan')],  ['2', '#ffffffff', T('white')],
      ['3', '#ffb14dff', T('amber')], ['4', '#ff5c5cff', T('red')],   ['5', '#ff5cd8ff', T('magenta')]
    ];
    subHead(flareDetailPage, T('STREAK COLOUR'));
    var flareStreakGrid = swatchGrid(flareDetailPage, STREAK_TINTS, function (idx) { lensSet('flare.streakTint', idx); },
      { label: T('Streak colour'), perRow: 8, chipW: 40, marginTop: '4px' });
    var flareDebug = lensToggle(flareDetailPage, T('Flare only'), 'flare.debug',
      T('Shows the flare on black, so you can judge and tune it without the scene behind it.'));

    renderHooks.push(function (st) {
      var L = st.lens; if (!L) return;
      flarePill.set(L['flare.on'] > 0.5);
      flareReset.setDisabled(!!L.flareDefault);
      for (var i = 0; i < lensSliders.flare.length; i++) lensSliders.flare[i].row.sync(L[lensSliders.flare[i].key]);
      for (var j = 0; j < lensToggles.flare.length; j++) lensToggles.flare[j].t.set(L[lensToggles.flare[j].key] > 0.5);
      flareTintGrid.select(String(L['flare.tint'] | 0));
      flareStreakGrid.select(String(L['flare.streakTint'] | 0));
      flareSec.__note(L['flare.on'] > 0.5
        ? (TN(L['flare.ghosts'] | 0, '{0} ghost', '{0} ghosts') + ' · ' + (L['flare.streak'] > 0 ? T('streak on') : T('streak off'))
           + ' · ' + (L['flare.star'] > 0 ? T('spikes on') : T('spikes off')) + ' · ' + T('from {0}', L['flare.threshold'].toFixed(2)))
        : T('Off'));
    });
)CFJS"
