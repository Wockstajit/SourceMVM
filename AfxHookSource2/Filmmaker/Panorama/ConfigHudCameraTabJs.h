#pragma once

// CAMERA tab of the CONFIG panel.
// Cards: SMOOTH VIEW, HANDHELD CAMERA, WEAPON RECOIL
// WEAPON RECOIL's Camera Shake tab is its own camera share split three ways (aim rotation /
// positional punch / barrel roll), which is the screen kick that belongs to the weapon's recoil.
// The Fisheye lens button lives on Smooth View's Effects page.

R"CFJS(
    // ===================== SMOOTH VIEW ===================================
    // The master switch and Reset live in the card HEADER (Docs/panorama/panorama_ui_guide.md): that is what lets a shut
    // card still say what it is, be switched off, and be reset without opening it. Both return the
    // same handles their in-body predecessors did, so the render hook below is unchanged.
    var svSec = section(pageCamera, 'SMOOTH VIEW', true, false, 'config.section.smoothview', '#80bdffff');
    var svReset = svSec.__headerReset(
      ['mirv_filmmaker smoothview reset', 'mirv_filmmaker fisheye off'], 'cam.smoothview');
    var svToggle = svSec.__headerPill(
      function () { cmd('mirv_filmmaker smoothview toggle'); },
      'Adds breathing, strafe tilt, and movement effects.');

    // The old Effects page held only three switches, so they are a CHIP ROW at the top of the
    // card now (Docs/features/filmmaker_config_panel.md): visible whichever page is open, and one fewer tab to look under.
    var svFalling = togglePill(svSec, 'Fall pitch',
      'Pitches the camera during a fall.',
      function () { cmd('mirv_filmmaker smoothview falling toggle'); });
    var svJumping = togglePill(svSec, 'Jump kick',
      'Kicks the camera on take-off and landing.',
      function () { cmd('mirv_filmmaker smoothview jumping toggle'); });
    // Fisheye is its own D3D pass with its own command; it lives here because it is a camera LOOK.
    var fisheyeBtn = togglePill(svSec, 'Fisheye Lens',
      'Barrel-distortion wide-angle lens pass.',
      function () { cmd('mirv_filmmaker fisheye toggle'); });

    var svPageBreathing = svSec.__page('Breathing');
    var svPageTilt = svSec.__page('Tilt');

    // ---- BREATHING ----
    var svHealthSway = togglePill(svPageBreathing, 'Health Sway',
      'Breathing gets heavier as health drops.',
      function () { cmd('mirv_filmmaker smoothview healthSway toggle'); });
    var svBreathX = sliderRow(svPageBreathing, { label: 'Freq X', tip: 'Speed of the side-to-side half of the idle breathing figure-8.', cmd: 'mirv_filmmaker smoothview breathX', min: 0.1, max: 2, decimals: 2, unit: 'x' });
    var svBreathY = sliderRow(svPageBreathing, { label: 'Freq Y', tip: 'Speed of the up-and-down half of the idle breathing figure-8.', cmd: 'mirv_filmmaker smoothview breathY', min: 0.1, max: 2, decimals: 2, unit: 'x' });
    var svStandAmp = sliderRow(svPageBreathing, { label: 'Stand Amp', tip: 'Size of the breathing sway while standing still, in degrees.', cmd: 'mirv_filmmaker smoothview standAmp', min: 0, max: 2, decimals: 2, unit: ' deg' });
    var svWalkAmp = sliderRow(svPageBreathing, { label: 'Walk Amp', tip: 'Size of the breathing sway while walking, in degrees.', cmd: 'mirv_filmmaker smoothview walkAmp', min: 0, max: 2, decimals: 2, unit: ' deg' });
    var svRunAmp = sliderRow(svPageBreathing, { label: 'Run Amp', tip: 'Size of the breathing sway while running, in degrees.', cmd: 'mirv_filmmaker smoothview runAmp', min: 0, max: 2, decimals: 2, unit: ' deg' });

    // ---- TILT ----
    var svTiltEnabled = togglePill(svPageTilt, 'Strafe Tilt',
      'Leans the camera into strafing movement.',
      function () { cmd('mirv_filmmaker smoothview tiltEnabled toggle'); });
    var svTiltInv = togglePill(svPageTilt, 'Reverse tilt',
      'Leans away from the movement instead of into it.',
      function () { cmd('mirv_filmmaker smoothview tiltInv toggle'); });
    var svTilt = sliderRow(svPageTilt, { label: 'Tilt Angle', tip: 'How far the view leans into a strafe. 0 keeps the horizon level.', cmd: 'mirv_filmmaker smoothview tilt', min: 0, max: 60, unit: '' });
    var svTiltSpeed = sliderRow(svPageTilt, { label: 'Tilt Speed', tip: 'How quickly the lean reaches full angle and returns. Higher is snappier.', cmd: 'mirv_filmmaker smoothview tiltSpeed',
      min: 0.5, max: 30, decimals: 1, unit: 'x' });

    renderHooks.push(function (st) {
      svToggle.set(!!st.svOn);
      svBreathX.sync(st.svBreathX); svBreathY.sync(st.svBreathY);
      svStandAmp.sync(st.svStandAmp); svWalkAmp.sync(st.svWalkAmp); svRunAmp.sync(st.svRunAmp);
      var hs = !!st.svHealthSway;
      svHealthSway.set(hs);
      var te = !!st.svTiltEnabled;
      svTiltEnabled.set(te);
      svTilt.sync(st.svTilt); svTiltSpeed.sync(st.svTiltSpeed);
      var ti = !!st.svTiltInv;
      svTiltInv.set(ti);
      var fe = !!st.svFalling;
      svFalling.set(fe);
      var je = !!st.svJumping;
      svJumping.set(je);
      fisheyeBtn.set(!!st.fisheye);
      svReset.setDisabled(!!st.smoothViewDefault && !st.fisheye);
    });

)CFJS"
R"CFJS(
    // ===================== HANDHELD ======================================
    var hhSec = section(pageCamera, 'HANDHELD CAMERA', true, false, 'config.section.handheld', '#80bdffff');
    var hhReset = hhSec.__headerReset(['mirv_filmmaker handheld reset quiet'], 'cam.handheld');
    var hhToggle = hhSec.__headerPill(
      function () { cmd('mirv_filmmaker handheld toggle'); },
      'Adds natural handheld camera movement.');
    var hhVmToggle = togglePill(hhSec, 'Decouple Viewmodel',
      'Keeps the weapon independent from handheld movement.',
      function () { cmd('mirv_filmmaker handheld viewmodel toggle'); });
    var hhCenterLockToggle = togglePill(hhSec, 'Center Lock',
      'Keeps recorded aim centered while the camera still moves.',
      function () { cmd('mirv_filmmaker handheld centerlock toggle'); });
    var hhCenterLock = sliderRow(hhSec, { label: 'Lock Strength', tip: 'How strongly the recorded aim is pulled back to screen centre, so added motion never hides the action.',
      cmd: 'mirv_filmmaker handheld centerlockpct', min: 0, max: 100 });

    var hhPageMovement = hhSec.__page('Movement');
    var hhPageCurves = hhSec.__page('Curves');
    var hhPageDetail = hhSec.__page('Detail');

    // ---- MOVEMENT ----
    var hhDrift = sliderRow(hhPageMovement, { label: 'Drift', tip: 'Slow sway, jitter and tremble, as if someone is holding the camera.', cmd: 'mirv_filmmaker handheld drift', offAtZero: true });
    var hhDriftDir = sliderRow(hhPageMovement, { label: 'Drift Direction',
      tip: 'Which way the slow drift leans. Its cycle is longer than most shots, so a clip usually sees only one lean — this picks which.',
      cmd: 'mirv_filmmaker handheld driftdir', min: 0, max: 360, unit: '°' });
    var hhTilt = sliderRow(hhPageMovement, { label: 'Ambient Tilt', tip: 'A slow lean that wanders while the shot runs.', cmd: 'mirv_filmmaker handheld tilt', offAtZero: true });
    var hhVariance = sliderRow(hhPageMovement, { label: 'Variance', tip: 'Varies each pass of a motion curve so a looping take stops reading as a loop.', cmd: 'mirv_filmmaker handheld variance', min: 0, max: 100 });
    // ---- CURVES ----
    var hhCurve = sliderRow(hhPageCurves, { label: 'Motion Curves', tip: 'Recorded human camera motion, switching itself between idle, walk and run.', cmd: 'mirv_filmmaker handheld curve', offAtZero: true });
    var hhSpeed = sliderRow(hhPageCurves, { label: 'Curve Speed', tip: 'Playback speed of those recorded curves. Higher makes the operator seem more restless.', cmd: 'mirv_filmmaker handheld speed', min: 25, max: 300 });
    // The third entry is the option's hover explanation, measured from each recorded curve
    // (HandheldCamCurves.cpp): how far it rotates, how fast it shakes, whether it moves position.
    var HH_CURVES = [
      ['INVESTIGATION','Investigation','Slow, wide wandering of someone searching a room: big drifting turns with a little body sway.'],
      ['THE_CLOSEUP','The Closeup','A barely-there hold for tight shots: very small, slow wobble and no position drift.'],
      ['THE_WEDDING','The Wedding','A calm, steady operator: gentle sway, the subtlest of the recorded holds after The Zeek.'],
      ['WALK_TO_THE_STORE','Walk to the Store','A relaxed walking rhythm: moderate side-to-side sway with a light step bounce.'],
      ['HANDYCAM_RUN','HandyCam Run','Running with a camcorder: fast, jerky shake with large turns, in a short loop.'],
      ['OUT_CAR_WINDOW','Out Car Window','Riding in a moving car: mostly up-and-down road bumps with little side-to-side.'],
      ['BIKE_ON_GRAVEL_2D','Bike On Gravel','Violent, rapid vibration, the strongest curve here: heavy pitch rattle and roll.'],
      ['SPACESHIP_SHAKE_2D','Spaceship Shake','A constant mid-speed rumble with a rolling tilt, like an engine under the floor.'],
      ['THE_ZEEK_2D','The Zeek','Almost still: a faint, slow float for shots that should feel alive but not handheld.']
    ];
    subHead(hhPageCurves, 'Curve per movement state');
    var hhIdle = cycleRow(hhPageCurves, 'Standing', 'mirv_filmmaker handheld preset idle', HH_CURVES,
      'Which recorded operator curve plays while the subject stands still.');
    var hhWalk = cycleRow(hhPageCurves, 'Walking', 'mirv_filmmaker handheld preset walk', HH_CURVES,
      'Which recorded operator curve plays while the subject walks.');
    var hhRun = cycleRow(hhPageCurves, 'Running', 'mirv_filmmaker handheld preset run', HH_CURVES,
      'Which recorded operator curve plays while the subject runs.');
    // ---- DETAIL ----
    var hhRegrip = sliderRow(hhPageDetail, { label: 'Re-grip Fumbles', tip: 'Occasional jolts, as if the operator adjusts their grip.', cmd: 'mirv_filmmaker handheld regrip', offAtZero: true });
    var hhPeriod = sliderRow(hhPageDetail, { label: 'Fumble Gap', tip: 'Average seconds between those grip adjustments.', cmd: 'mirv_filmmaker handheld period', min: 2, max: 60, unit: 's' });
    renderHooks.push(function (st) {
      hhToggle.set(!!st.handheldOn); hhVmToggle.set(!!st.hhVmDecoupled);
      hhCenterLockToggle.set(!!st.hhCenterLockOn); hhCenterLock.sync(st.hhCenterLock);
      // Dimmed, not hidden, while Center Lock is off: it still shows what it would do.
      hhCenterLock.row.style.opacity = st.hhCenterLockOn ? '1' : '0.4';
      hhDrift.sync(st.hhDrift); hhDriftDir.sync(st.hhDriftDir);
      hhCurve.sync(st.hhCurve); hhTilt.sync(st.hhTilt);
      hhSpeed.sync(st.hhSpeed); hhVariance.sync(st.hhVariance); hhRegrip.sync(st.hhRegrip); hhPeriod.sync(st.hhPeriod);
      hhIdle.sync(st.hhIdle); hhWalk.sync(st.hhWalk); hhRun.sync(st.hhRun);
      hhReset.setDisabled(!!st.handheldDefault);
    });
)CFJS"
R"CFJS(
    // ===================== WEAPON RECOIL ================================
    // Procedural viewmodel recoil (WeaponRecoil.h) -- a SEPARATE feature from Recoil Shake
    // above: that one shakes the SCREEN, this one kicks the GUN itself (MW-style spring
    // punch/rise/roll layered over CS2's own firing animation).
    // Amber edge: this card owns the amber pivot marker, and the edge is what ties the two.
    var wrSec = section(pageCamera, 'WEAPON RECOIL', true, false, 'config.section.weaponrecoil', '#e8b339ff');
    var wrTestBtn = wrSec.__headerAction('Test Shot', function () { cmd('mirv_filmmaker weaponrecoil test'); });
    wrSec.__cfgParts = { test: wrTestBtn };
    attachTip(wrTestBtn, 'Fires one kick using the scope selected below -- the gun in shot when that scope covers it, otherwise a weapon from that category. So a test always exercises the settings you are editing. Shortcut: press T while Config is open.');
    // Same split as Weapon Bob: Recoil's own reset covers its tuning and its `pivot` scalar, but
    // not the Forward/Right/Up hinge or the marker orb that sit in this card.
    var wrReset = wrSec.__headerReset(
      ['mirv_filmmaker weaponrecoil reset quiet',
       'mirv_filmmaker pivot recoil reset',
       'mirv_filmmaker pivotorb recoil off quiet'], 'cam.weaponrecoil');
    var wrToggle = wrSec.__headerPill(
      function () { cmd('mirv_filmmaker weaponrecoil toggle'); },
      'Punchy procedural gun recoil: the weapon kicks back, the muzzle climbs, then it springs back and settles.');

    // APPLIES TO and STYLE sit ABOVE the page strip, side by side (Docs/features/filmmaker_config_panel.md): they say WHAT
    // the pages are editing, so they stay visible whichever page is open.
    var wrShared = wrSec;
    var wrTop = row(wrShared); wrTop.style.marginTop = '0px';
    function wrTopCol(caption, last) {
      var c = mk('Panel', wrTop); c.hittest = false; c.hittestchildren = true;
      c.style.flowChildren = 'down'; c.style.width = 'fill-parent-flow(1.0)';
      if (!last) c.style.marginRight = '10px';
      var h = lbl(c, caption, S.muted, 11); h.hittest = false;
      h.style.fontWeight = 'bold'; h.style.letterSpacing = '1.5px'; h.style.marginBottom = '5px';
      h.style.textTransform = 'uppercase';
      return c;
    }
    var wrScopeCol = wrTopCol('Applies to', false);
    var wrStyleCol = wrTopCol('Style', true);
    // Captions match the words the card summary already uses; the keys stay style1/style2.
    var wrStyleSeg = segmented(wrStyleCol, [['style1', 'Springy'], ['style2', 'Snappy']], '100%',
      function (k) { wrStyleSeg.sync(k); cmd('mirv_filmmaker weaponrecoil style ' + k + ' quiet'); });
    attachTip(wrStyleSeg.panel, 'Springy: a heavier punch-led kick that rides up and settles back. '
      + 'Snappy: a sharper kick that throws the gun wider and rolls it more.');

    // ---- RECOIL APPLIES TO ----
    // The sliders always edit whichever scope is selected here. Narrower beats broader when a
    // shot matches several (type > slot > all); see WeaponRecoilScope.h.
    var WR_SLOTS = [['primary', 'Primary'], ['secondary', 'Secondary']];
    var WR_TYPES = [['pistol', 'Pistols'], ['smg', 'SMGs'], ['rifle', 'Rifles'],
      ['shotgun', 'Shotguns'], ['sniper', 'Snipers'], ['lmg', 'LMGs']];
    var wrScopeKind = 'all', wrScopeKey = '';
    // The gun in shot, as the engine last reported it, so picking a scope kind can land on the
    // weapon you are actually watching.
    var wrLive = '', wrLiveType = '', wrLiveSlot = '';
    function wrDefaultKey(kind) {
      var list = (kind === 'slot') ? WR_SLOTS : WR_TYPES;
      var live = (kind === 'slot') ? wrLiveSlot : wrLiveType;
      for (var i = 0; i < list.length; i++) if (list[i][0] === live) return live;
      return list[0][0];
    }
    // The selection we last pushed, held until the engine echoes it back, so a render tick that
    // lands before the queued command takes effect cannot snap the picker back.
    var wrScopeSent = null, wrScopeSentAge = 0;
    function wrSendScope() {
      if (wrScopeKind === 'all') {
        wrScopeSent = { kind: 'all', key: '' }; wrScopeSentAge = 0;
        cmd('mirv_filmmaker weaponrecoil scope all quiet');
        return;
      }
      if (!wrScopeKey) return;
      wrScopeSent = { kind: wrScopeKind, key: wrScopeKey }; wrScopeSentAge = 0;
      cmd('mirv_filmmaker weaponrecoil scope ' + wrScopeKind + ' ' + wrScopeKey + ' quiet');
    }
    var wrScopeSeg = segmented(wrScopeCol, [['all', 'All'], ['slot', 'Slot'], ['type', 'Type']], '100%', function (kind) {
      wrScopeKind = kind;
      wrScopeKey = (kind === 'all') ? '' : wrDefaultKey(kind);
      wrScopeSeg.sync(kind);
      wrKeyRender();
      wrSendScope();
    });

    // The key picker is rebuilt on every kind change rather than kept as hidden panels. One
    // wrapping row of cells, so the Type list never stacks into a tall grid.
    var wrKeyHost = mk('Panel', wrShared); wrKeyHost.hittest = false; wrKeyHost.hittestchildren = true;
    wrKeyHost.style.flowChildren = 'right-wrap'; wrKeyHost.style.width = '100%';
    var wrKeyBtns = {};
    function wrKeySync() {
      for (var k in wrKeyBtns) setBtnOn(wrKeyBtns[k], k === wrScopeKey, S.accent);
    }
    function wrKeyRender() {
      wrKeyHost.RemoveAndDeleteChildren();
      wrKeyBtns = {};
      if (wrScopeKind === 'all') { wrKeyHost.style.marginTop = '0px'; return; }
      wrKeyHost.style.marginTop = '6px';
      var list = (wrScopeKind === 'slot') ? WR_SLOTS : WR_TYPES;
      for (var i = 0; i < list.length; i++) (function (key, caption) {
        var b = mk('Panel', wrKeyHost);
        b.style.marginRight = '4px'; b.style.marginBottom = '4px';
        b.style.paddingTop = '5px'; b.style.paddingBottom = '5px';
        b.style.paddingLeft = '10px'; b.style.paddingRight = '10px';
        b.style.borderRadius = '3px';
        var l = lbl(b, caption, S.label, 12);
        l.hittest = false; l.style.fontWeight = 'bold'; l.style.whiteSpace = 'nowrap';
        b.__grid = true;
        makeInteractive(b, l, function () {
          wrScopeKey = key;
          wrKeySync();
          wrSendScope();
        }, { fg: S.label });
        wrKeyBtns[key] = b;
      })(list[i][0], list[i][1]);
      wrKeySync();
    }

    // Says whether the sliders below are this scope's OWN values or inherited ones.
    var wrScopeNote = lbl(wrShared, '', S.hint, 12);
    wrScopeNote.style.marginTop = '6px'; wrScopeNote.style.width = '100%';
    wrScopeNote.style.paddingLeft = UI.line.padH + 'px';
    var wrScopeClear = wideBtn(wrShared, 'Clear Override', function () {
      cmd('mirv_filmmaker weaponrecoil scopeclear');
    }, S.label);
    wrScopeSeg.sync('all');
    wrKeyRender();
)CFJS"
R"CFJS(
    // ---- NAMED PRESETS: one dropdown + Save ---------------------------------------------
    // A preset is a tuning snapshot plus the selected scope. Picking one loads it; × deletes it.
    // Save turns the field into a name entry fed by ConfigHud's WM_CHAR bridge (HUD Panorama
    // TextEntry panels do not receive real keyboard focus); Enter saves, Esc cancels.
    var wrPresetFocus = false, wrPresetBuf = '', wrPresetLoaded = '', wrPresetIsDefault = true;
    var wrPresetDrop = dropdownRow(wrShared, 'Preset', [], function (name) {
      wrPresetLoaded = name;
      cmd('mirv_filmmaker weaponrecoil preset load "' + name + '"');
    }, {
      empty: 'No saved presets yet. Press Save to name the current tuning.',
      onDelete: function (name) { cmd('mirv_filmmaker weaponrecoil preset delete "' + name + '"'); },
      button: ['Save', function () {
        if (wrPresetFocus && wrPresetBuf) wrPresetSave(); else wrPresetFocusOn();
      }]
    });
    var wrPresetField = wrPresetDrop.field;
    function wrPresetPaint() {
      if (wrPresetFocus) {
        wrPresetDrop.setText(wrPresetBuf ? (wrPresetBuf + '|') : 'Type a name, press Enter to save', !wrPresetBuf);
        wrPresetField.style.border = '1px solid ' + S.accent;
        return;
      }
      wrPresetField.style.border = '1px solid #ffffff1f';
      wrPresetDrop.setText(wrPresetLoaded || (wrPresetIsDefault ? 'Default tuning' : 'Custom tuning'), false);
    }
    function configWindowBlur() {
      wrPresetFocus = false;
      root.SetAttributeString('textcapture', '0');
      wrPresetPaint();
    }
    // The field that has the keyboard (every typed field sets it when clicked). A press anywhere
    // else hands the keyboard back, so G and the other keys work again without Enter or Esc.
    // Called every frame from the slider/dropdown pumps: render hooks only run when the state
    // string changes, so a hook can miss the frame the button went down.
    var cfgTextOwner = null, cfgTextPress = false;
    function cfgTextPump(s) {
      if (!s || typeof s.mx !== 'number') return;
      var down = !!s.lmb;
      if (down && !cfgTextPress && root.GetAttributeString
          && root.GetAttributeString('textcapture', '0') === '1') {
        var p = cfgTextOwner, inside = false;
        if (p && (!p.IsValid || p.IsValid()) && p.GetPositionWithinWindow) {
          var q = p.GetPositionWithinWindow();
          inside = !!q && s.mx >= q.x && s.mx < q.x + p.actuallayoutwidth
            && s.my >= q.y && s.my < q.y + p.actuallayoutheight;
        }
        if (!inside) configWindowBlur();
      }
      cfgTextPress = down;
    }
    function wrPresetFocusOn() {
      // Blur first: other name fields decorate configWindowBlur, so this clears any other focused
      // field before claiming the keyboard.
      configWindowBlur();
      wrPresetDrop.close();
      wrPresetFocus = true; wrPresetBuf = '';
      cfgTextOwner = wrPresetField;
      root.SetAttributeString('textcapture', '1');
      wrPresetPaint();
    }
    function wrPresetSave() {
      var name = wrPresetBuf;
      wrPresetBuf = ''; configWindowBlur();
      if (name) { wrPresetLoaded = name; cmd('mirv_filmmaker weaponrecoil preset save "' + name + '"'); }
      wrPresetPaint();
    }
    function configWindowChars(csv) {
      if (!wrPresetFocus) return;
      var parts = String(csv || '').split(',');
      for (var i = 0; i < parts.length; i++) {
        var code = parseInt(parts[i], 10);
        if (code === 13) {
          wrPresetSave();
        } else if (code === 27) {
          wrPresetBuf = ''; configWindowBlur();
        } else if (code === 8) {
          wrPresetBuf = wrPresetBuf.slice(0, -1);
        } else if (wrPresetBuf.length < 40 &&
          ((code >= 48 && code <= 57) || (code >= 65 && code <= 90) ||
           (code >= 97 && code <= 122) || code === 32 || code === 45 || code === 46 || code === 95)) {
          wrPresetBuf += String.fromCharCode(code);
        }
      }
      wrPresetPaint();
    }
    var wrPresetSig = '';
    function wrRenderPresets(items) {
      var sig = JSON.stringify(items || []);
      if (sig === wrPresetSig) return;
      wrPresetSig = sig;
      var list = [];
      items = items || [];
      for (var i = 0; i < items.length; i++) {
        var preset = items[i];
        var scopeLabel = preset.scope === 'all' ? 'All' :
          (preset.scope === 'slot' ? 'Slot: ' : 'Type: ') + (preset.key || '');
        list.push([preset.name, preset.name + '  ·  ' + scopeLabel]);
      }
      wrPresetDrop.setItems(list);
    }
    wrPresetPaint();

    // Two pages, so neither is a wall of sliders: WEAPON is the gun's own kick, CAMERA SHAKE is
    // how much of that kick reaches the view.
    var wrPageWeapon = wrSec.__page('Weapon');
    var wrPageShake = wrSec.__page('Camera Shake');

    // ---- CAMERA SHAKE ----
    // SHARES OF the kick set on the Weapon page, not extra kick: what reaches the screen is
    // (1 - share) * kick on the gun, so raising one makes the weapon look steadier.
    var wrShakeNote = lbl(wrPageShake, 'Shares of the kick, not extra kick — higher moves the view '
      + 'and steadies the gun.', S.hint, 12);
    wrShakeNote.style.width = '100%'; wrShakeNote.style.marginTop = '4px'; wrShakeNote.style.marginBottom = '2px';
    wrShakeNote.style.paddingLeft = UI.line.padH + 'px'; wrShakeNote.style.paddingRight = UI.line.padH + 'px';
    var wrCamRot = sliderRow(wrPageShake, { label: 'Rotation', tip: 'How much of the muzzle rise and sideways yaw the camera takes. MW sits around 15-30.', cmd: 'mirv_filmmaker weaponrecoil camerarot',
      min: 0, max: 100, unit: '%' });
    var wrCamPos = sliderRow(wrPageShake, { label: 'Punch', tip: 'How much of the backward kick moves the camera with it. Higher means the view takes the punch and the weapon moves less on screen, not that the punch gets bigger.', cmd: 'mirv_filmmaker weaponrecoil camerapos',
      min: 0, max: 100, unit: '%' });
    var wrCamRoll = sliderRow(wrPageShake, { label: 'Barrel Roll', tip: 'How much of the roll around the barrel axis reaches the camera. The first one to lower if the screen feels like it is tipping.', cmd: 'mirv_filmmaker weaponrecoil cameraroll',
      min: 0, max: 100, unit: '%' });

    var wrAmount = sliderRow(wrPageWeapon, { label: 'Amount', tip: 'Master strength of the kick on every shot.', cmd: 'mirv_filmmaker weaponrecoil amount', offAtZero: true });
    var wrPunch = sliderRow(wrPageWeapon, { label: 'Punch', tip: 'How far the gun kicks straight backward per shot. Higher values pull it farther back; it returns to rest without bouncing forward.', cmd: 'mirv_filmmaker weaponrecoil punch', offAtZero: true });
    var wrRot = sliderRow(wrPageWeapon, { label: 'Rotation', tip: 'Muzzle climb, sideways yaw, and roll around the barrel per shot.', cmd: 'mirv_filmmaker weaponrecoil rotation', offAtZero: true });
    var wrSnap = sliderRow(wrPageWeapon, { label: 'Snappiness', tip: 'How hard the spring yanks the gun back to rest. Higher is a sharper, faster kick.', cmd: 'mirv_filmmaker weaponrecoil snap', min: 25, max: 400 });
    var wrRecovery = sliderRow(wrPageWeapon, { label: 'Recovery', tip: 'How firmly the gun springs back. Lower overshoots and wobbles more before settling.', cmd: 'mirv_filmmaker weaponrecoil recovery', min: 25, max: 400 });
)CFJS"
R"CFJS(
    // ---- PIVOT DRAWER: where the weapon hinges, per weapon ----
    // Built TWICE -- once for Weapon Recoil, once for Weapon Bob -- because the two effects own
    // separate hinges and separate markers (amber for recoil, cyan for bob).
    //
    // A collapsible drawer at the bottom of the card (Docs/features/filmmaker_config_panel.md), shut by default. Its header
    // summarises the class in shot and the hinge; the EYE replaces both the old "Show Pivot
    // Marker" switch and the "Hide Pivot" button: lit = pinned marker on, and switching it off
    // also kills the marker's after-edit flash. ↺ is Reset Pivots.
    //
    // THE GUN IN SHOT PICKS THE SCOPE, and the other classes are locked: you tune a hinge by
    // watching the weapon swing around it, and there is nothing on screen to judge another
    // class's hinge against. There is deliberately no "All" cell (see WeaponRecoil history).
    var PV_SCOPES = [['rifle','Rifle'],['smg','SMG'],['pistol','Pistol'],['shotgun','Shotgun'],['sniper','Sniper'],['knife','Knife'],['utility','Utility']];
    var cfgPivotDrawers = {};
    function pivotSection(host, owner, k, orbKey, orbColour) {
      var colour = orbColour === 'amber' ? '#e8b339ff' : '#5fd0c0ff';
      var c7 = colour.substring(0, 7);
      endTileLine(host);
      var d = mk('Panel', host); d.hittest = false; d.hittestchildren = true;
      d.style.flowChildren = 'down'; d.style.width = '100%'; d.style.marginTop = '9px';
      d.style.backgroundColor = S.drawer; d.style.border = '1px solid #ffffff14';
      d.style.borderRadius = '3px';
      var head = mk('Panel', d); head.hittest = true;
      head.style.flowChildren = 'right'; head.style.width = '100%'; head.style.height = '34px';
      head.style.paddingLeft = '10px'; head.style.paddingRight = '6px';
      var dot = mk('Panel', head); dot.hittest = false;
      dot.style.width = '8px'; dot.style.height = '8px'; dot.style.borderRadius = '1px';
      dot.style.backgroundColor = colour; dot.style.verticalAlign = 'center'; dot.style.marginRight = '8px';
      var cap = lbl(head, 'Pivot', S.label, 11); cap.hittest = false;
      cap.style.textTransform = 'uppercase';
      cap.style.fontWeight = 'bold'; cap.style.letterSpacing = '1.5px';
      cap.style.verticalAlign = 'center'; cap.style.marginRight = '8px';
      var sum = lbl(head, '', S.muted, 12); sum.hittest = false;
      sum.style.fontFamily = S.fontMonodigit;   // updates every frame while a hinge is dragged
      sum.style.width = 'fill-parent-flow(1.0)'; sum.style.whiteSpace = 'nowrap';
      sum.style.textOverflow = 'ellipsis'; sum.style.verticalAlign = 'center';
      function iconBtn(glyph, size, onClick, tipText) {
        var b = mk('Panel', head); b.hittest = true;
        b.style.width = '28px'; b.style.height = '24px'; b.style.borderRadius = '3px';
        b.style.verticalAlign = 'center'; b.style.marginLeft = '4px';
        var g = lbl(b, glyph, S.muted, size); g.hittest = false;
        g.style.horizontalAlign = 'center'; g.style.verticalAlign = 'center';
        b.__g = g;
        b.SetPanelEvent('onactivate', onClick);
        if (tipText) attachTip(b, tipText);
        return b;
      }
      var orbOn = false, orbShowing = false;
      var eye = iconBtn('', 14, function () {
        cmd('mirv_filmmaker pivotorb ' + owner + ' toggle quiet');
        if (orbOn) cmd('mirv_filmmaker pivotorb ' + owner + ' hide quiet');
      }, 'Shows a ' + orbColour + ' marker in the world at the point this effect rotates the weapon around. '
         + 'Switching it off also hides the marker that flashes after each edit.');
      // The marker icon is DRAWN -- a ring and a dot, both centred by alignment -- not a font
      // glyph: the fallback font put the glyph's dot visibly off-centre in its box.
      eye.__g.visible = false;
      var eyeRing = mk('Panel', eye); eyeRing.hittest = false;
      eyeRing.style.width = '14px'; eyeRing.style.height = '14px'; eyeRing.style.borderRadius = '50%';
      eyeRing.style.horizontalAlign = 'center'; eyeRing.style.verticalAlign = 'center';
      var eyeDot = mk('Panel', eye); eyeDot.hittest = false;
      eyeDot.style.width = '6px'; eyeDot.style.height = '6px'; eyeDot.style.borderRadius = '50%';
      eyeDot.style.horizontalAlign = 'center'; eyeDot.style.verticalAlign = 'center';
      function paintEye() {
        var hot = eye.__hot;
        var ink = orbOn ? colour : (hot ? S.value : S.muted);
        eye.style.backgroundColor = orbOn ? (c7 + '2e') : (hot ? '#ffffff14' : S.clear);
        eye.style.border = '1px solid ' + (orbOn ? (c7 + '99') : '#ffffff1f');
        eyeRing.style.border = '2px solid ' + ink;
        eyeDot.style.backgroundColor = ink;
      }
      eye.__hoverPaint = paintEye;
      var resetIco = iconBtn('↺', 15, function () {
        cmd('mirv_filmmaker pivot ' + owner + ' reset');
        cmd('mirv_filmmaker pivotorb ' + owner + ' off quiet');
      }, 'Reset Pivots: puts every weapon class back on its shipped hinge and switches the marker off.');
      resetIco.__hoverPaint = function () {
        resetIco.style.backgroundColor = resetIco.__hot ? '#ffffff14' : S.clear;
        resetIco.__g.style.color = resetIco.__hot ? S.value : S.muted;
      };
      var chev = lbl(head, '⌄', S.label, 15); chev.hittest = false;
      chev.style.verticalAlign = 'center'; chev.style.marginLeft = '4px';
      chev.style.width = '24px'; chev.style.textAlign = 'center';
      chev.style.transformOrigin = '50% 50%';
      chev.style.transitionProperty = 'transform'; chev.style.transitionDuration = '0.15s';
      var bodyP = mk('Panel', d); bodyP.hittest = false; bodyP.hittestchildren = true;
      bodyP.style.flowChildren = 'down'; bodyP.style.width = '100%';
      bodyP.style.paddingLeft = '8px'; bodyP.style.paddingRight = '8px'; bodyP.style.paddingBottom = '8px';
      bodyP.__pivotOwner = owner;
      var open = false;
      function setOpen(v, persist) {
        open = !!v; bodyP.visible = open;
        chev.style.transform = open ? 'rotateZ(180deg)' : 'rotateZ(0deg)';
        if (persist) cmd('mirv_filmmaker settings ui config.pivot.' + owner + '.open ' + (open ? '1' : '0'));
      }
      hoverable(head, function () { head.style.backgroundColor = head.__hot ? '#ffffff08' : S.clear; });
      head.SetPanelEvent('onactivate', function () { setOpen(!open, true); });
      setOpen(false, false);
      cfgPivotDrawers[owner] = { setOpen: setOpen, panel: d };
)CFJS"
R"CFJS(
      var liveClass = '', scope = '';
      var seg = segmented(bodyP, PV_SCOPES, '100%', function (key) {
        if (key !== liveClass) return;   // locked -- only the class in shot is editable
        scope = key; paint();
      }, 11);
      // Per-axis writes via onChange: the command needs the CURRENT scope and owner.
      function set(axis) {
        return function (v) {
          cmd('mirv_filmmaker pivot ' + owner + ' set ' + scope + ' ' + axis + ' ' + v.toFixed(1));
        };
      }
      var pvOpts = { min: -30, max: 30, decimals: 1, unit: 'u', noFill: true, centerTick: true,
                     knobColor: colour, search: 'pivot' };
      function pvSlider(label, tip, axis) {
        var o = { label: label, tip: tip, onChange: set(axis) };
        for (var kk in pvOpts) o[kk] = pvOpts[kk];
        var h = sliderRow(bodyP, o);
        h.row.style.height = '28px';
        return h;
      }
      var fwd = pvSlider('Forward', 'How far in front of the eye the weapon rotates from. Larger swings the muzzle more through a turn. Negative puts the hinge behind the eye, which suits a weapon held at the shoulder.', 'fwd');
      var right = pvSlider('Right', 'Sideways position of the hinge. Positive is toward the right hand.', 'right');
      var up = pvSlider('Up', 'Vertical position of the hinge. Negative is below the eye, where a held weapon sits.', 'up');
      var last = null;
      function paint() {
        var st = last; if (!st) return;
        var live = st[k + 'Class'] || '';
        var liveName = '';
        for (var i = 0; i < PV_SCOPES.length; i++) if (PV_SCOPES[i][0] === live) liveName = PV_SCOPES[i][1];
        liveClass = liveName ? live : '';
        if (liveClass) scope = liveClass;
        seg.sync(scope);
        for (var j = 0; j < PV_SCOPES.length; j++) {
          var cell = seg.btns[PV_SCOPES[j][0]];
          if (cell) cell.style.opacity = (PV_SCOPES[j][0] === liveClass) ? '1.0' : '0.35';
        }
        var t = st[k + 'Table'] || {};
        var rowv = t[scope];
        if (rowv && rowv.p) { fwd.sync(rowv.p[0]); right.sync(rowv.p[1]); up.sync(rowv.p[2]); }
        var wep = st[k + 'Weapon'];
        var gun = wep ? String(wep).replace('weapon_', '') : '';
        var nums = (rowv && rowv.p)
          ? (' · ' + rowv.p[0].toFixed(1) + 'u / ' + rowv.p[1].toFixed(1) + 'u / ' + rowv.p[2].toFixed(1) + 'u') : '';
        var text = liveClass ? (liveName + (gun ? ' (' + gun + ')' : '') + nums)
                             : 'No weapon in shot';
        if (sum.text !== text) sum.text = text;
      }
      renderHooks.push(function (st) {
        last = st;
        orbOn = !!st[orbKey]; orbShowing = !!st[orbKey + 'Showing'];
        paintEye();
        resetIco.style.opacity = st[k + 'Default'] ? '0.45' : '1.0';
        if (st.ui && !d.__hydrated) {
          d.__hydrated = true;
          if (st.ui['config.pivot.' + owner + '.open'] === '1') setOpen(true, false);
        }
        paint();
      });
    }
    pivotSection(wrSec, 'recoil', 'pvr', 'wrPivotOrb', 'amber');
    // Test Shot lives outside the tab pages: you fire a test while tuning ANY of them, and having
    // to switch back to one page to press it is exactly the friction that makes a feel pass slow.
    // (Test Shot is the card's PRIMARY action and now sits in its header, where it is reachable
    // from any of the card's pages -- see the wrSec.__headerAction call above.)

    renderHooks.push(function (st) {
      wrToggle.set(!!st.wrOn);
      wrCamRot.sync(st.wrCamRot); wrCamPos.sync(st.wrCamPos); wrCamRoll.sync(st.wrCamRoll);
      wrAmount.sync(st.wrAmount); wrPunch.sync(st.wrPunch); wrRot.sync(st.wrRot);
      wrSnap.sync(st.wrSnap); wrRecovery.sync(st.wrRecovery);
      var style = st.wrStyle || 'style1';
      wrStyleSeg.sync(style);
      wrReset.setDisabled(!!st.weaponRecoilDefault);
      if (wrPresetIsDefault !== !!st.weaponRecoilDefault) {
        wrPresetIsDefault = !!st.weaponRecoilDefault;
        if (wrPresetIsDefault) wrPresetLoaded = '';
        wrPresetPaint();
      }

      // Follow the engine rather than the local vars: a console command can change the scope
      // too, and the card would otherwise keep highlighting whatever was last clicked here.
      var kind = st.wrScope || 'all';
      var key = st.wrScopeKey || '';
      // Wait for our own selection to come back before following the engine again. Capped at ~30
      // ticks so a command that never lands cannot strand the picker out of sync forever.
      if (wrScopeSent) {
        if (wrScopeSent.kind === kind && wrScopeSent.key === key) wrScopeSent = null;
        else if (++wrScopeSentAge > 30) wrScopeSent = null;
      }
      if (!wrScopeSent) {
        if (kind !== wrScopeKind) {
          wrScopeKind = kind; wrScopeKey = key; wrScopeSeg.sync(kind); wrKeyRender();
        } else if (key !== wrScopeKey) {
          wrScopeKey = key; wrKeySync();
        }
      }

      var own = !!st.wrScopeOwn;
      var count = (st.wrOverrides && st.wrOverrides.length) || 0;
      wrLive = st.wrLive || '';
      wrLiveType = st.wrLiveType || '';
      wrLiveSlot = st.wrLiveSlot || '';

      // WHAT THESE SLIDERS WILL ACTUALLY REACH.
      //
      // An override on a category nothing in shot belongs to behaves exactly like a broken one:
      // the sliders move, the card says the scope owns its values, and no shot ever uses them.
      // So the note names the gun in play and says outright when the selected scope does not
      // cover it, instead of leaving that to be inferred from a kick that never changes.
      var inUse = wrLive ? (' Gun in shot: ' + wrLive.toUpperCase() + '.') : '';
      var missing = (kind !== 'all') && wrLive && (st.wrScopeCovers === false);
      if (missing) {
        wrScopeNote.text = 'This scope does not cover the gun in shot (' + wrLive.toUpperCase()
          + '). Changes here will not show until a matching weapon fires.';
      } else if (kind === 'all') {
        wrScopeNote.text = (count
          ? ('All weapons. ' + count + ' override' + (count === 1 ? '' : 's') + ' set.')
          : 'All weapons.') + inUse;
      } else {
        wrScopeNote.text = (own
          ? 'Editing settings that belong to this scope.'
          : 'Inheriting. Move a slider to give this scope its own settings.') + inUse;
      }
      wrScopeNote.style.color = missing ? S.accent : S.hint;
      wrScopeClear.visible = (kind !== 'all') && own;
      wrRenderPresets(st.wrPresets);
    });
)CFJS"
R"CFJS(
    // ===================== WEAPON BOB ===================================
    // The gun's WALK CYCLE (WeaponBob.h). Sits directly under WEAPON RECOIL because the two are
    // the pair that make the weapon feel carried: recoil is what a shot does to it, bob is what
    // walking does. They are separate modules that share only the transport.
    //
    // This moves only the weapon, so it never sways the whole camera.
    // Cyan edge: this card owns the cyan pivot marker (the recoil card owns the amber one).
    var wbSec = section(pageCamera, 'WEAPON BOB', true, false, 'config.section.weaponbob', '#5fd0c0ff');
    // The hinge sliders and the marker orb are controls on THIS card, but they live in their own
    // modules -- `weaponbob reset` only ever touched Weapon Bob's own nine values, so a moved
    // pivot and a lit orb survived the card's Reset.
    var wbReset = wbSec.__headerReset(
      ['mirv_filmmaker weaponbob reset',
       'mirv_filmmaker pivot bob reset',
       'mirv_filmmaker pivotorb bob off quiet'], 'cam.weaponbob');
    var wbToggle = wbSec.__headerPill(
      function () { cmd('mirv_filmmaker weaponbob toggle'); },
      'The gun sways as the player moves, lags and rotates behind the aim, and breathes at rest. Moves the weapon, not the view.');

    // Nine values split into two pages (Docs/features/filmmaker_config_panel.md): WALK is the gait, LOOK is the aim.
    var wbPageWalk = wbSec.__page('Walk');
    var wbPageLook = wbSec.__page('Look');
    var wbAmount = sliderRow(wbPageWalk, { label: 'Amount', tip: 'Master intensity for everything in this card. CS2 adds a little movement of its own underneath.', cmd: 'mirv_filmmaker weaponbob amount',
      min: 0, max: 150, unit: '%', offAtZero: true });
    var wbSway = sliderRow(wbPageWalk, { label: 'Sway', tip: 'How far the gun travels: the side-to-side, up-down and in-out motion of the walk cycle.', cmd: 'mirv_filmmaker weaponbob sway',
      min: 0, max: 150, unit: '%', offAtZero: true });
    var wbTilt = sliderRow(wbPageWalk, { label: 'Tilt', tip: 'How much the gun rocks and rolls as it moves. This is most of the character; raise it for a heavier, looser weapon.', cmd: 'mirv_filmmaker weaponbob tilt',
      min: 0, max: 150, unit: '%', offAtZero: true });
    var wbRate = sliderRow(wbPageWalk, { label: 'Cadence', tip: 'Speed of the walk cycle without changing how far the gun moves. Lower reads as a heavier weapon.', cmd: 'mirv_filmmaker weaponbob rate',
      min: 25, max: 400, unit: '%' });
    var wbStep = sliderRow(wbPageWalk, { label: 'Step Sync', tip: 'Pulls the cycle onto real footsteps so the gun dips as each foot lands, instead of drifting against them.', cmd: 'mirv_filmmaker weaponbob stepsync',
      min: 0, max: 100, unit: '%', offAtZero: true });
    var wbLook = sliderRow(wbPageLook, { label: 'Look Sway', tip: 'The gun lags and rotates behind the aim as the player looks around, hinged on the weapon rather than the camera.', cmd: 'mirv_filmmaker weaponbob looksway',
      min: 0, max: 150, unit: '%', offAtZero: true });
    var wbInertia = sliderRow(wbPageLook, { label: 'Inertia', tip: 'A slower, heavier overshoot layered on the look sway. Raise it for a weapon that keeps swinging after a turn stops.', cmd: 'mirv_filmmaker weaponbob inertia',
      min: 0, max: 150, unit: '%', offAtZero: true });
    var wbIdle = sliderRow(wbPageLook, { label: 'Idle Breath', tip: 'Slow drift while standing still so the gun is never pinned in place.', cmd: 'mirv_filmmaker weaponbob idle',
      min: 0, max: 150, unit: '%', offAtZero: true });
    var wbCam = sliderRow(wbPageLook, { label: 'Camera Share', tip: 'How much of the weapon motion the camera also picks up. Keep this low: high values make the whole view move with the gun.', cmd: 'mirv_filmmaker weaponbob camera',
      min: 0, max: 100, unit: '%', offAtZero: true });

    // Weapon Bob's own hinge -- a second, independent copy of the drawer WEAPON RECOIL carries.
    pivotSection(wbSec, 'bob', 'pvb', 'wbPivotOrb', 'cyan');

    renderHooks.push(function (st) {
      wbToggle.set(!!st.wbOn);
      wbAmount.sync(st.wbAmount); wbSway.sync(st.wbSway); wbTilt.sync(st.wbTilt);
      wbLook.sync(st.wbLook); wbInertia.sync(st.wbInertia); wbIdle.sync(st.wbIdle);
      wbRate.sync(st.wbRate); wbStep.sync(st.wbStep); wbCam.sync(st.wbCam);
      wbReset.setDisabled(!!st.weaponBobDefault);
    });
)CFJS"
#include "ConfigHudLensTabJs.h"
R"CFJS(
    // ===================== CAMERA FOOTER ================================
    // Same footer the EFFECTS tab ends with, minus the demo carry-over (that setting is about
    // world state, which is what EFFECTS owns). Parented to pageCamera AFTER every card, and
    // Panorama flows siblings in creation order, so this is what puts it last on the page.
    //
    // The command list leans on `mirv_filmmaker settings reset <group>`, which replays the
    // snapshot taken before the saved file was read -- i.e. the values these modules were
    // constructed with. The backend owns the complete tab reset, including lens and pivots.
    var camFooter = footerCard(pageCamera);
    var camKeep = keepToggle(camFooter, 'camera', 'keepCamera',
      'Smooth View, Handheld, Weapon Bob, recoil, lens effects, Field of view and Viewmodel');
    var camReset = resetFooter(camFooter, function () {
      return { label: 'CAMERA', cmds: ['mirv_filmmaker effects reset camera'] };
    }, 'camera');

    // ---- shut-card readouts ----
    // Every card on this tab is an effect that is either doing something or not, and most of the
    // time the answer is "not". A closed card now says which, and by how much, so the tab reads as
    // a status list instead of three identical shut drawers.
    renderHooks.push(function (st) {
      camKeep.sync(st);
      camReset.paint();
      // The summary says WHAT THIS CARD IS DOING, in the values you would otherwise open it to
      // read (Docs/panorama/panorama_ui_guide.md). "On" alone was true and useless.
      svSec.__note(st.svOn
        ? ('Breathing · tilt ' + (st.svTiltEnabled ? Math.round(st.svTilt || 0) : 'off')
           + ' · effects ' + ((st.svFalling || st.svJumping || st.fisheye) ? 'on' : 'off'))
        : 'Off');
      hhSec.__note(st.handheldOn
        ? ('Drift ' + Math.round(st.hhDrift || 0) + '% · curves ' + Math.round(st.hhCurve || 0)
           + '% · lock ' + Math.round(st.hhCenterLock || 0) + '%')
        : 'Off');
      wrSec.__note(st.wrOn
        ? (Math.round(st.wrAmount || 0) + '% kick · ' + ((st.wrStyle === 'style2') ? 'snappy' : 'springy')
           + ' · camera ' + Math.round(st.wrCamRot || 0) + '%')
        : 'Off');
      wbSec.__note(st.wbOn
        ? (Math.round(st.wbAmount || 0) + '% · sway ' + Math.round(st.wbSway || 0)
           + '% · camera share ' + Math.round(st.wbCam || 0) + '%')
        : 'Off');
    });
)CFJS"
R"CFJS(
    // ===================== VIEWMODEL =====================================
    // The gun itself, not the motion applied to it -- WEAPON RECOIL and WEAPON BOB move whatever
    // this card decides the viewmodel IS.
    //
    // The whole card is one choice. `mirv_viewmodel`'s detour runs AFTER CS2's own viewmodel calc,
    // so leaving it alone means every player keeps the viewmodel the game already built for them:
    // their FOV, their offsets, their handedness, as the demo recorded it. That is the default.
    // Turning per-player off replaces all of it with ONE viewmodel every player shares, which is
    // what a shot needs when it has to match across a round instead of following whoever is
    // being spectated.
    //
    // Each field is owned separately, so a released axis still falls through to the engine even
    // while the override is on. That is exactly what `set`'s `*` / `-` grammar was built for, so
    // these sliders drive `mirv_viewmodel` directly rather than growing a second command surface.
    var vmSec = section(pageCamera, 'VIEWMODEL', true, false, 'config.section.viewmodel', '#c9a0ffff');
    var vmReset = vmSec.__headerReset(['mirv_viewmodel reset', 'mirv_fov default'], 'cam.viewmodel');

    // The SHOT's field of view (`mirv_fov`). It lives here because "field of view" is what a
    // director opens VIEWMODEL looking for, and a second card for one slider was one card too
    // many. It is independent of the per-player choice below: it changes the framing, not the
    // weapon, and the game's own FOV returns when the slider is put back to its default.
    var vmCamFov = sliderRow(vmSec, {
      label: 'Field of view', min: 5, max: 170, decimals: 0, unit: '°',
      tip: 'The camera’s field of view -- the whole shot, not just the weapon. Whole degrees only; '
         + '90 is the game’s default. Below ~40 reads as a long lens, above ~120 as a fisheye. '
         + 'Reset hands the framing back to the game.',
      onChange: function (v) { cmd('mirv_fov ' + v.toFixed(0)); }
    });

    // Per-player vs Shared is one choice between two states, so it is a two-cell mode row
    // (Docs/features/filmmaker_config_panel.md) rather than an on/off. Same command either way: `mirv_viewmodel enabled 1`
    // replaces every player's viewmodel with the shared one below, `0` hands it back.
    var vmPerPlayer = modeRow(vmSec, 'Viewmodel', [['per', 'Per-player'], ['shared', 'Shared']], null,
      function (k) {
        vmPerPlayer.sync(k);
        cmd('mirv_viewmodel enabled ' + (k === 'shared' ? '1' : '0'));
      },
      'Per-player: every player keeps the viewmodel the game built for them, exactly as the demo '
      + 'recorded it. Shared: one viewmodel for every player, and the controls below take over.');

    // CINEMATIC ranges, deliberately past CS2's own (FOV 54-68, offsets +/-2.5). The detour
    // writes these AFTER the engine's function returns, and that function is where its clamp
    // lives -- the 68.0 constant HookViewmodel's comment calls out -- so the values go in
    // unclamped. The old limits were this slider's, not the engine's.
    var vmFov = sliderRow(vmSec, { label: 'Weapon FOV', min: 5, max: 140, decimals: 0, unit: '°',
      tip: 'Viewmodel field of view -- the WEAPON’s projection only; Field of view above is the shot itself. '
         + 'Lower pulls the weapon in and makes it read larger; 68 is CS2’s default and its own ceiling.',
      onChange: function (v) { cmd('mirv_viewmodel set * * * ' + v.toFixed(0) + ' *'); } });
    var vmX = sliderRow(vmSec, { label: 'Offset X', min: -25, max: 25, decimals: 2, unit: '',
      tip: 'Forward and back along the weapon’s own axis. Negative pulls it toward the camera. CS2’s own limit is 2.5.',
      onChange: function (v) { cmd('mirv_viewmodel set ' + v.toFixed(2) + ' * * * *'); } });
    var vmY = sliderRow(vmSec, { label: 'Offset Y', min: -25, max: 25, decimals: 2, unit: '',
      tip: 'Side to side. Negative moves the weapon toward the centre of the screen. CS2’s own limit is 2.',
      onChange: function (v) { cmd('mirv_viewmodel set * ' + v.toFixed(2) + ' * * *'); } });
    var vmZ = sliderRow(vmSec, { label: 'Offset Z', min: -25, max: 25, decimals: 2, unit: '',
      tip: 'Up and down. Negative drops the weapon lower in frame. CS2’s own limit is 2.',
      onChange: function (v) { cmd('mirv_viewmodel set * * ' + v.toFixed(2) + ' * *'); } });
    var vmHand = togglePill(vmSec, 'Left-handed',
      'Mirrors the weapon to the other hand. Part of the shared viewmodel, so it only applies while per-player is off.',
      function () { cmd('mirv_viewmodel set * * * * ' + (st.vmLeftHanded ? '0' : '1')); });
    var vmNote = lbl(vmSec, '', S.hint, 12);
    vmNote.hittest = false;
    vmNote.style.whiteSpace = 'normal'; vmNote.style.width = '100%'; vmNote.style.marginTop = '9px';
    vmNote.style.paddingLeft = UI.line.padH + 'px';

    renderHooks.push(function (st) {
      var perPlayer = st.vmPerPlayer !== false;
      vmPerPlayer.sync(perPlayer ? 'per' : 'shared');
      vmHand.set(!!st.vmLeftHanded);
      vmCamFov.sync(st.camFovOn && typeof st.camFov === 'number' ? st.camFov : 90);
      vmFov.sync(typeof st.vmFov === 'number' ? st.vmFov : 68);
      vmX.sync(typeof st.vmX === 'number' ? st.vmX : 0);
      vmY.sync(typeof st.vmY === 'number' ? st.vmY : 0);
      vmZ.sync(typeof st.vmZ === 'number' ? st.vmZ : 0);
      // Dim rather than hide: the values stay readable, and it is obvious WHY they do nothing.
      [vmFov, vmX, vmY, vmZ].forEach(function (control) {
        if (control.slot) control.slot.style.opacity = perPlayer ? '0.4' : '1';
      });
      vmHand.row.style.opacity = perPlayer ? '0.4' : '1';
      vmReset.setDisabled(!!st.viewmodelDefault && !st.camFovOn);
      vmNote.text = perPlayer
        ? 'Each player keeps their own viewmodel, as recorded.'
        : 'One viewmodel for every player.';
      var fovNote = st.camFovOn ? (Math.round(st.camFov || 90) + '° · ') : '';
      vmSec.__note(fovNote + (perPlayer
        ? 'Per-player'
        : ('Shared · weapon FOV ' + Math.round(st.vmFov || 68)
           + ' · ' + (st.vmLeftHanded ? 'left' : 'right') + '-handed')));
    });
)CFJS"
