#pragma once

// The CAMERA EDITOR column: two sub-tabs, PATH CAMERA and FOLLOW CAMERA, built out of the card and
// row grammar in DeadlockControlsJs.h.
//
// WHAT CHANGED AND WHY. This was three flat cards and a marker list. See Docs/deadlock/README.md
// and Docs/deadlock/ui-style.md; the original design kept the CS2 card set and PATH / FOLLOW split,
// changing the vocabulary where Deadlock differs -- markers rather than cameras, heroes rather
// than weapons. So the structure follows that design, one card per named concern, each with its own
// group tabs, and every range, unit and tooltip comes from DEFS below rather than from the call site.
//
// DEADLOCK'S FOLLOW CAMERA IS NOT AN ORBIT AND IS NEVER FIRST-PERSON. The replay is third-person, so
// the camera is PLACED and then either locks on to a hero or attaches to a point on its model, per
// CS2's own Movie/FollowCamera.cpp (the parity spec -- see DeadlockFollowSolveMath.h). LOCK-ON's
// camera position is fixed at wherever it was placed and never recomputed; the Framing tiles (Off
// X/Y/Z, Pitch/Yaw/Roll) move the AIM POINT and trim the look, not the camera. ATTACH is the one
// that moves: the same six tiles are a LOCAL offset and rotation on the mount it rides. Same tiles,
// different meaning per mode, exactly like CS2's own Framing page -- see folFramingNote below for
// where that distinction is stated on screen.
//
// THE TARGET LIST COMES FROM THE GAME. `heroes` in the state feed is whoever is actually in this
// replay. Until the Deadlock port can enumerate entities the list arrives empty, and the dropdown
// says so rather than offering names that resolve to nothing.

#define DEADLOCK_CAMERA_BODY_JS R"CFJS(
(function () {
  if ($.MvmCameraBody) return;
  var M = $.MvmStyle;
  var X = $.MvmControls;
  if (!M || !X) return;
  var S = M.S;

  function cmd(t) { M.cmd(t); }

  // Every range, decimal count, unit, axis tint and tooltip in this column, in one table. The
  // implementation is checked against it rather than against each call site.
  var DEFS = {
    fov:        { label: 'FOV',        min: 20,   max: 130,  dec: 1, unit: '',  axis: 'fov',
                  tip: 'Field of view on the selected marker.' },
    roll:       { label: 'Roll',       min: -45,  max: 45,   dec: 1, unit: '°', axis: 'roll',
                  tip: 'Camera roll on the selected marker.' },
    // Lock-on and attach share these six tiles (CS2's own Framing page does the same, one set of
    // Off X/Y/Z + Pitch/Yaw/Roll tiles for both of its modes) -- the tip states BOTH meanings, and
    // the note line under the tiles (folFramingNote below) states whichever one is live right now.
    folOffX:    { label: 'Off X',      min: -1000, max: 1000, dec: 0, unit: 'u', axis: 'x',
                  tip: 'Lock-on: world X offset on the aim point. Attach: forward/back from the mount.' },
    folOffY:    { label: 'Off Y',      min: -1000, max: 1000, dec: 0, unit: 'u', axis: 'y',
                  tip: 'Lock-on: world Y offset on the aim point. Attach: right/left from the mount.' },
    folOffZ:    { label: 'Off Z',      min: -1000, max: 1000, dec: 0, unit: 'u', axis: 'z',
                  tip: 'Lock-on: world height offset on the aim point. Attach: up/down at the mount.' },
    folRotPitch:{ label: 'Pitch',      min: -180, max: 180,  dec: 0, unit: '°', axis: 'pitch',
                  tip: 'Lock-on: trims the aim up/down. Attach: rotates the ride pitch.' },
    folRotYaw:  { label: 'Yaw',        min: -180, max: 180,  dec: 0, unit: '°', axis: 'yaw',
                  tip: 'Lock-on: trims the aim left/right. Attach: rotates the ride yaw.' },
    folRotRoll: { label: 'Roll',       min: -180, max: 180,  dec: 0, unit: '°', axis: 'roll',
                  tip: 'Lock-on: trims the aim roll. Attach: rotates the ride roll.' },
    fovFollow:  { label: 'FOV',        min: 20,   max: 130,  dec: 0, unit: '',  axis: 'fov',
                  tip: 'Field of view for the follow camera.' },
    look:       { label: 'Look',       min: 0,    max: 1,    dec: 2, unit: '',  axis: 'yaw',
                  tip: 'How hard the camera turns onto the hero. Lower is looser.' },
    position:   { label: 'Pos',        min: 0,    max: 1,    dec: 2, unit: '',  axis: 'x',
                  tip: 'How hard the camera chases the hero’s position.' },
    prediction: { label: 'Predict',    min: 0,    max: 0.5,  dec: 2, unit: 's', axis: 'z',
                  tip: 'Leads the hero, so a dashing target stays framed.' },
    deadzone:   { label: 'Deadzone',   min: 0,    max: 10,   dec: 1, unit: '°', axis: 'pitch',
                  tip: 'Angle the hero may move inside before the camera reacts.' },
    maxTurn:    { label: 'Turn/s',     min: 0,    max: 1440, dec: 0, unit: '°', axis: 'roll',
                  tip: 'Ceiling on how fast the camera may swing.' },
    deathHold:  { label: 'Death hold', min: 0,    max: 10,   dec: 1, unit: 's', axis: 'fov',
                  tip: 'Stay attached this long through a death, then release.' }
  };

  var ATTACHMENTS = [
    ['origin', 'Origin', 'entity'], ['chest', 'Chest', 'bone'], ['head', 'Head', 'bone'],
    ['hand_r', 'Right hand', 'attachment'], ['weapon', 'Weapon muzzle', 'attachment']
  ];

  // The full-screen "clear everything" nuke, CS2-shaped: CameraEditorJs.h ~159-190
  // (confirmOverlay/confirmBox/confirmTitle/confirmBody/confirmYes/confirmNo), reached from its own
  // PATH inspector's "Clear All" action (CameraEditorPathInspectorJs.h line 75). One shared instance
  // -- only one clear-everything prompt can ever be on screen at a time -- built lazily against the
  // true top-level root, never a card or column body (see X.confirmModal's own doc comment for why).
  var clearModal = null;
  function confirmClearAll() {
    if (!clearModal && X.confirmModal) {
      var host = ($.MvmWorkspace && $.MvmWorkspace.root) ? $.MvmWorkspace.root() : null;
      if (host) clearModal = X.confirmModal(host, { width: 520 });
    }
    if (!clearModal) { cmd('mirv_filmmaker marker clear'); return; }
    clearModal.show({
      title: 'CLEAR ALL CAMERA PATHS?',
      body: 'This deletes every path camera/keyframe for this demo.',
      noLabel: 'No',
      yesLabel: 'Yes, clear all',
      onYes: function () { cmd('mirv_filmmaker marker clear'); }
    });
  }

  // ---- FOLLOW card persist rows, duplicated per sub-page ---------------------------------------
  //
  // CameraEditorFollowInspectorJs.h keeps "This camera" (Place/Reposition/Clear) and the Camera mode
  // row in `folPersist`, ABOVE the Target/Framing/Tracking page strip, so neither moves when the page
  // changes. X.card's own `persist` array reproduces that placement -- but it concats the SAME row
  // defs into every group's row list, and every group's rows land in the ONE flat `w` map keyed by
  // `id` (DeadlockControlsJs.h's addRows: `if (dd.id) widgets[dd.id] = w;`). A row with one id built
  // three times (once per group) leaves `w.<id>` pointing at only the LAST group's copy, so the other
  // two pages' copies would never receive a `.set()`/`.show()` call and would freeze at their built
  // state -- invisible on whichever page loads first. So persist is not used for camMode or the
  // Place/Clear pair below: each page gets its OWN row, with a page-suffixed id, and render() below
  // updates all three explicitly. Three DOM rows doing the same job costs a little construction, not
  // correctness.
  function camModeRow(suffix) {
    var d = X.mode('camMode' + suffix, 'Camera mode', [['lockon', 'Lock-on'], ['attach', 'Attach']], {
      tip: 'Lock-on is a placed camera that turns to follow the hero; Attach rides a point on the model.',
      segW: 200,
      onPick: function (k) { cmd('mirv_filmmaker follow mode ' + k); }
    });
    return d;
  }
  // CS2 keeps Place / Reposition / Clear as ONE three-button row under ONE "This camera" card
  // (CameraEditorFollowInspectorJs.h), and hides only the Place and Clear buttons in Attach mode
  // (~428-429: `followPlace.visible = !attachModeOn; followClear.visible = !attachModeOn;`) while
  // Reposition stays put -- Attach still needs it, to capture the mount-relative pose. X.act's
  // set() now takes a per-button visibility array (DeadlockControlsJs.h) for exactly this, so the
  // three live in one row/one card like CS2's instead of two cards split by which ones can hide.
  function camThisCameraRow(suffix) {
    var d = X.act('This camera', [
      { cap: 'Place', cmd: 'mirv_filmmaker follow place' },
      { cap: 'Reposition', cmd: 'mirv_filmmaker follow reposition' },
      { cap: 'Clear', cmd: 'mirv_filmmaker follow clear' }
    ], 3, { note: 'One camera. Placing again replaces it. Place it, then pick what it watches.' });
    d.id = 'camThisCamera' + suffix;
    return d;
  }
  function followPersistRows(suffix) {
    return [camThisCameraRow(suffix), camModeRow(suffix)];
  }

  var wrap = null, col = null;
  var cards = {};          // key -> card widget
  var sub = 'path';        // which sub-tab
  var last = null;         // last state, for the summaries
  var heroKey = '', attachKey = 'chest';

  // A value tile's def, wired straight to the command that owns the number.
  function tile(id, widthPx, run) {
    var d = DEFS[id];
    return X.scrub(id, d.label, {
      min: d.min, max: d.max, dec: d.dec, unit: d.unit, axis: d.axis, tip: d.tip,
      width: widthPx, onChange: run
    });
  }

  function heroOptions() {
    var list = (last && last.heroes) || [];
    var out = [];
    for (var i = 0; i < list.length; ++i) {
      out.push([String(list[i].id), list[i].name || ('Hero ' + list[i].id), list[i].team || '']);
    }
    return out;
  }

  function heroLabel() {
    var list = (last && last.heroes) || [];
    for (var i = 0; i < list.length; ++i) {
      if (String(list[i].id) === heroKey) return list[i].name || ('Hero ' + list[i].id);
    }
    return list.length ? 'Select hero' : 'No heroes in this replay';
  }

  function attachLabel() {
    for (var i = 0; i < ATTACHMENTS.length; ++i) {
      if (ATTACHMENTS[i][0] === attachKey) return ATTACHMENTS[i][1];
    }
    return 'Select attachment';
  }

)CFJS" R"CFJS(
  // ---- PATH CAMERA ----------------------------------------------------------------------------

  function buildPathCards(host) {
    cards.sel = X.card(host, {
      key: 'sel', title: 'SELECTED CAMERA', edge: S.gold, action: '+ Add',
      onAction: function () { cmd('mirv_filmmaker marker add'); },
      groups: [{ head: 'Camera', rows: [
        X.nav({ txt: 'No cameras placed',
                onPrev: function () { cmd('mirv_filmmaker marker sel prev'); },
                onNext: function () { cmd('mirv_filmmaker marker sel next'); },
                tag: 'selNav' }),
        X.act('This camera', [
          { cap: 'Delete', cmd: 'mirv_filmmaker marker delete' },
          // CS2's Tick -> Here is a RETIME (CameraPath::MoveKey): the key moves to the current
          // tick, its pose stays. `marker update` (pose overwrite) remains on the console.
          { cap: 'Tick → here', cmd: 'mirv_filmmaker marker tickhere' },
          // CS2's own "Clear All" (CameraEditorPathInspectorJs.h line 75, 'danger') opens the same
          // full-screen confirm this fires -- deleting the whole shot deserves the heavy prompt.
          { cap: 'Clear all', run: confirmClearAll }
        ], 3),
        X.act('Placement', [
          { cap: 'Reposition — fly, then click to place', cmd: 'mirv_filmmaker marker reposition' }
        ], 1),
        tile('fov', 213, function (v) { cmd('mirv_filmmaker marker fov ' + v.toFixed(1)); }),
        tile('roll', 213, function (v) { cmd('mirv_filmmaker marker roll ' + v.toFixed(1)); }),
        X.note('FOV and roll belong to the selected camera and are keyed with it.')
      ] }]
    }, S.gold);
    // The nav row is the only widget in this card addressed by name, so it is tagged rather than
    // given an `id` -- an id would make it look like a value the state feed writes.
    cards.sel.w.selNav = cards.sel.w.selNav || null;

    cards.path = X.card(host, {
      key: 'path', title: 'PATH', edge: S.gold,
      groups: [
        { head: 'Curve', rows: [
          X.mode('interp', 'Curve', [['linear', 'Linear'], ['smooth', '∿ Smooth']], {
            tip: 'How the camera moves between keys.', segW: 200,
            onPick: function (k) { cmd('mirv_filmmaker path interp ' + (k === 'smooth' ? 'cubic' : 'linear')); }
          }),
          X.mode('ease', 'Ease', [['none', 'None'], ['in', 'In'], ['out', 'Out'], ['inout', 'In/Out']], {
            tip: 'Ease on the SELECTED key — the same value the timeline’s Ease button and its right-click menu write.',
            segW: 260,
            onPick: function (k) { cmd('mirv_filmmaker marker ease ' + k); }
          }),
          X.note('Curve is the whole path; ease belongs to one key. Both mirror onto the timeline and the graph.')
        ] },
        { head: 'Speed', rows: [
          X.mode('speedMode', 'Mode', [['manual', 'Manual'], ['perseg', 'Per-Seg'], ['constant', 'Constant']], {
            tip: 'Manual follows the keys; the other two drive the multiplier.', segW: 260,
            onPick: function (k) { cmd('mirv_filmmaker path speedmode ' + k); }
          }),
          // One stepper serving both Per-Segment and Constant, exactly like CS2's own Amount row
          // (CameraEditorPathInspectorJs.h stepMul): which value it edits depends on the current
          // mode, so a second identical row would only be a second thing to keep in sync.
          X.stepper('speedAmount', 'Amount', [0.2, 0.5, 0.8, 1.0], {
            tip: 'Steps through 0.20 / 0.50 / 0.80 / 1.00.',
            onChange: function (v) {
              var perSeg = last && last.cam && (last.cam.speedMode | 0) === 2;
              cmd(perSeg
                ? ('mirv_filmmaker marker speedmul ' + v.toFixed(2))
                : ('mirv_filmmaker path speed ' + v.toFixed(2)));
            }
          }),
          // CS2's Timing pair (CameraEditorPathInspectorJs.h): Live plays the demo along the path,
          // Freeze holds the tick. The in-world path and markers draw automatically, like CS2's --
          // the old "Draw the path" / "Draw markers" switches were dead flags and had no CS2
          // counterpart; `path draw|drawmarkers 0` on the console is the override.
          X.mode('timing', 'Timing', [['live', 'Live'], ['freeze', 'Freeze']], {
            tip: 'Live plays the demo along the path; Freeze holds the tick.', segW: 180,
            onPick: function (k) { cmd('mirv_filmmaker path freeze ' + (k === 'freeze' ? 1 : 0)); }
          }),
          // CS2's pathHint: one line naming what the chosen speed mode does, rewritten per mode.
          X.note('Manual: the path follows the demo timeline.', 'speedHint')
        ] }
        // No "Shot" page: CS2's PATH card is Curve and Speed only. The shot autosaves on every edit
        // (DeadlockShot.cpp AutoSave), and Clear all lives on This camera; `path save|load` remain
        // on the console.
      ]
    }, S.gold);

    // Pinned to the bottom and never collapsible: a shortcut list you have to open is a shortcut
    // list you have already stopped reading.
    M.make(host, 'Panel', '', { width: '100%', height: 'fill-parent-flow(1.0)' });
    cards.keys = X.card(host, {
      key: 'keys', title: 'SHORTCUTS', edge: X.C.chevron, always: true,
      groups: [{ head: 'Keys', rows: [
        X.key('K', 'Place a camera at the free cam'),
        X.key('Space', 'Play / pause'),
        X.key('← / →', 'Skip ±10s'),
        X.key('J', 'Mouse: UI or game')
      ] }]
    }, S.gold);
  }

  // ---- FOLLOW CAMERA --------------------------------------------------------------------------

  function buildFollowCards(host) {
    cards.follow = X.card(host, {
      // ONE toggle in the header, not two. CS2's own header action is a single button whose label
      // and command both flip on the current state (CameraEditorFollowInspectorJs.h ~30-32, 432:
      // `cmd(f.enabled ? 'follow stop' : 'follow preview')`, `__lbl.text = f.enabled ? 'Stop' : 'Live'`).
      // This card used to carry BOTH a static "Live" header action (onAction always sent `follow
      // live`, so it could turn the camera ON but never OFF once live) AND a separate pill sending
      // `follow toggle` -- two controls doing overlapping things, and the action button was actually
      // broken as a toggle. Deadlock's `follow` console verb has `toggle`/`live` but no `stop`
      // (DeadlockCommands.cpp ~627-628), so the pill's `toggle` is the only symmetric verb; it is kept
      // as the one header control and the redundant, semantically-broken action button is dropped.
      // The card header's `action` slot has no way to relabel itself per frame (X.card exposes no
      // setter for it), so the closest available "one control, current-state-aware" affordance within
      // this API is the pill alone -- on/off colour plus the sub-line text below stand in for CS2's
      // Live/Stop caption swap.
      key: 'follow', title: 'FOLLOW / ATTACH CAMERA', edge: S.green, pill: 'followOn',
      onPill: function () { cmd('mirv_filmmaker follow toggle'); },
      groups: [
        { head: 'Target', rows: followPersistRows('Target').concat([
          X.drop('hero', 'Hero', {
            onOpen: function (anchor) {
              X.openMenu(anchor, {
                placeholder: 'Select hero',
                options: heroOptions(),
                note: 'Read from the replay’s hero list, so the names are whoever is actually in this match.'
              }, heroKey, function (k) { cmd('mirv_filmmaker follow hero ' + k); },
                 function () { if (cards.follow.w.hero) cards.follow.w.hero.setOpen(false); });
              if (cards.follow.w.hero) cards.follow.w.hero.setOpen(true);
            }
          }),
          X.drop('attach', 'Attachment', {
            onOpen: function (anchor) {
              X.openMenu(anchor, {
                placeholder: 'Select attachment',
                options: ATTACHMENTS,
                note: 'A fixed height/reach per point today, not a read of the hero’s own animated bones yet.'
              }, attachKey, function (k) { cmd('mirv_filmmaker follow attach ' + k); },
                 function () { if (cards.follow.w.attach) cards.follow.w.attach.setOpen(false); });
              if (cards.follow.w.attach) cards.follow.w.attach.setOpen(true);
            }
          }),
          X.act('Target', [
            { cap: 'Nearest', cmd: 'mirv_filmmaker follow nearest' },
            { cap: 'Go to', cmd: 'mirv_filmmaker follow goto' },
            { cap: 'Clear', cmd: 'mirv_filmmaker follow target clear' }
          ], 3)
        ]) },
        // CS2 puts the mode-dependent note FIRST, right where the page opens, before any of its
        // tiles (CameraEditorFollowInspectorJs.h ~299, ~338: `edNote(folFramingPage, '')` /
        // `edNote(folTrackPage, '')` are the first thing built on each page). It used to sit at the
        // bottom here, after every tile, which is backwards -- the note explains what the numbers
        // below it mean, so a reader hits the numbers first with no framing for them.
        { head: 'Framing', rows: followPersistRows('Framing').concat([
          X.note('', 'folFramingNote'),
          tile('folOffX', 213, function (v) { cmd('mirv_filmmaker follow offx ' + v.toFixed(0)); }),
          tile('folOffY', 213, function (v) { cmd('mirv_filmmaker follow offy ' + v.toFixed(0)); }),
          tile('folOffZ', 213, function (v) { cmd('mirv_filmmaker follow offz ' + v.toFixed(0)); }),
          tile('folRotPitch', 213, function (v) { cmd('mirv_filmmaker follow rotpitch ' + v.toFixed(0)); }),
          tile('folRotYaw', 213, function (v) { cmd('mirv_filmmaker follow rotyaw ' + v.toFixed(0)); }),
          tile('folRotRoll', 213, function (v) { cmd('mirv_filmmaker follow rotroll ' + v.toFixed(0)); }),
          tile('fovFollow', 213, function (v) { cmd('mirv_filmmaker follow fov ' + v.toFixed(0)); }),
          X.act('Framing', [{ cap: 'Reset framing', cmd: 'mirv_filmmaker follow resetframing' }], 3)
        ]) },
        // CS2's Tracking page is three visually distinct sections under their own captions --
        // aim settings, then "ON PLAYER DEATH" (mode + hold + retarget), then "WHEN TARGET
        // DISAPPEARS" (hold-last) -- not one flat list (CameraEditorFollowInspectorJs.h ~345-393:
        // folAimSettings, folDeathSettings with edSubHead('ON PLAYER DEATH'), folLostSettings with
        // edSubHead('WHEN TARGET DISAPPEARS')). X.cap gives the same caption style Config/Render
        // already use for their own section headers, so this reuses it rather than inventing one.
        { head: 'Tracking', rows: followPersistRows('Tracking').concat([
          X.note('', 'folTrackingNote'),
          // CS2 hides only this aim-smoothing group in Attach (~folAimSettings.visible = !attachModeOn,
          // CameraEditorFollowInspectorJs.h line 411); the death/retarget/hold-last controls below are
          // gated by its `f.type` (grenade/weapon/player), a concept Deadlock has no equivalent of, so
          // they stay visible in both modes here -- render() hides only look/position/prediction/
          // deadzone/maxTurn.
          tile('look', 213, function (v) { cmd('mirv_filmmaker follow look ' + v.toFixed(2)); }),
          tile('position', 213, function (v) { cmd('mirv_filmmaker follow pos ' + v.toFixed(2)); }),
          tile('prediction', 213, function (v) { cmd('mirv_filmmaker follow predict ' + v.toFixed(2)); }),
          tile('deadzone', 213, function (v) { cmd('mirv_filmmaker follow deadzone ' + v.toFixed(1)); }),
          tile('maxTurn', 213, function (v) { cmd('mirv_filmmaker follow maxturn ' + v.toFixed(0)); }),
          X.cap('ON PLAYER DEATH'),
          // The same boolean CS2 calls autoDisableOnDeath, shown the way CS2 shows it
          // (CameraEditorFollowInspectorJs.h): a two-way pick, not a switch. "Keep following" is
          // releaseOnDeath=0 (never let go), "Timed hold" is releaseOnDeath=1 (let go once the
          // hold below runs out).
          X.mode('autoDead', 'On death', [['keep', 'Keep following'], ['hold', 'Timed hold']], {
            tip: 'Keep following never lets go of a dead hero; Timed hold lets go once the hold below runs out.',
            segW: 236,
            onPick: function (k) { cmd('mirv_filmmaker follow releaseondeath ' + (k === 'hold' ? 1 : 0)); }
          }),
          X.note('Follow the fallen hero, then stop when the hold ends. Zero stops immediately.'),
          tile('deathHold', 213, function (v) { cmd('mirv_filmmaker follow deathhold ' + v.toFixed(1)); }),
          // HONEST, NOT WIRED YET. The flag round-trips (FollowCamera_SetFlag stores g_s.retarget,
          // DeadlockSettings.cpp persists and reloads it), but DeadlockFollowSolveMath.h's own header
          // comment says so plainly: Deadlock has no dropped-weapon/dropped-bomb retarget target, so
          // `wantsRetarget` is hard-coded false and g_s.retarget is never READ by anything that decides
          // whether to reacquire a hero after they respawn. Toggling this switch changes nothing you
          // can see today. Removing the control would also drop its saved value from every shot that
          // already has one on disk, so the switch stays -- the tip says what it actually does instead.
          // Grouped under ON PLAYER DEATH, matching where CS2 puts its own retarget toggles
          // (folRetargetSettings is a child of folDeathSettings, not folLostSettings).
          X.sw('retarget', 'Retarget on respawn', {
            tip: 'Saved with the shot, but not yet wired to any retargeting behaviour -- toggling it has no visible effect yet.',
            onToggle: function (v) { cmd('mirv_filmmaker follow retarget ' + (v ? 1 : 0)); }
          }),
          X.cap('WHEN TARGET DISAPPEARS'),
          X.sw('holdLast', 'Hold last position', {
            tip: 'Stays where the hero was instead of releasing.',
            onToggle: function (v) { cmd('mirv_filmmaker follow holdlast ' + (v ? 1 : 0)); }
          }),
          X.act('Tracking', [{ cap: 'Reset tracking', cmd: 'mirv_filmmaker follow resettracking' }], 3)
        ]) }
      ]
    }, S.green);
  }
)CFJS" R"CFJS(
  // ---- build ----------------------------------------------------------------------------------

  var pathHost = null, followHost = null;

  function build(parent) {
    wrap = M.make(parent, 'Panel', 'MvmCameraBody', { width: '100%', height: '100%', flowChildren: 'down' });
    if (!wrap) return;
    wrap.visible = false;

    col = X.column(wrap, {
      id: 'MvmCameraColumn',
      name: 'CAMERA EDITOR',
      accent: S.gold,
      subTabs: [['path', 'Path camera'], ['follow', 'Follow camera']],
      onSubTab: function (k) { setSub(k); },
      onCollapseAll: function () {
        for (var k in cards) if (cards[k]) cards[k].setOpen(false);
        refreshCollapse();
      }
    });
    if (!col) return;
    col.setVisible(true);
    // CS2's action row at the foot of the inspector (CameraEditorJs.h mvmActionRow: RENDER, then
    // CUSTOMIZE PLAYER filling the rest). Deadlock has no player customisation, so RENDER takes the
    // row. Pinned below the scroller, so the last card scrolls clear of it rather than under it.
    col.footer.set('', [
      { cap: 'Render', colour: S.gold, run: function () { cmd('mirv_filmmaker workspace render'); } }
    ]);

    // Full height so the SHORTCUTS card can be pinned to the bottom with a flexible spacer.
    // Panorama has no `margin-top: auto`; a filling sibling is the equivalent.
    pathHost = M.make(col.cards, 'Panel', '', { width: '100%', height: '100%', flowChildren: 'down' });
    followHost = M.make(col.cards, 'Panel', '', { width: '100%', flowChildren: 'down' });
    buildPathCards(pathHost);
    buildFollowCards(followHost);
    setSub('path');
  }

  function refreshCollapse() {
    var open = 0;
    for (var k in cards) {
      if (!cards[k] || cards[k].always) continue;
      if (visibleCard(k) && cards[k].isOpen()) open++;
    }
    // "Collapse all" only earns its place once a second card is open; before that it collapses
    // the one card you just opened, which is the opposite of a convenience.
    col.setCollapseVisible(open > 1);
  }

  function visibleCard(k) {
    return sub === 'path' ? (k === 'sel' || k === 'path' || k === 'keys') : (k === 'follow');
  }

  function setSub(k) {
    sub = k;
    X.show(pathHost, k === 'path');
    X.show(followHost, k === 'follow');
    col.setSubTab(k);
    // Opening the sub-tab's own first card is the difference between landing on a column of
    // headers and landing on the thing you switched tabs to see.
    if (k === 'path') { cards.sel && cards.sel.setOpen(true); }
    else { cards.follow && cards.follow.setOpen(true); }
    refreshCollapse();
  }

  function setVisible(v) {
    if (wrap) { try { wrap.visible = !!v; } catch (e) {} }
    if (!v) X.closeMenu();
  }

  // ---- render ---------------------------------------------------------------------------------

  var EASE_NAMES = ['none', 'in', 'out', 'inout'];
  var SPEED_NAMES = ['manual', 'constant', 'perseg'];   // mirrors PathSettings::speedMode

  function render(st) {
    if (!wrap || !col) return;
    last = st;

    col.setMouseMode(!!st.uiMouse);

    var cam = st.cam || {};
    var list = st.markerList || [];
    var sel = (st.selected === undefined) ? -1 : st.selected;
    var hasSel = sel >= 0 && sel < list.length;

    // --- selected camera
    if (cards.sel) {
      var navTxt = list.length
        ? ('Camera ' + (sel + 1) + ' of ' + list.length + '  ·  tick ' + (hasSel ? list[sel].tick : '-'))
        : 'No cameras placed';
      if (cards.sel.w.selNav) cards.sel.w.selNav.set(navTxt);
      if (cards.sel.w.fov) cards.sel.w.fov.set(typeof cam.fov === 'number' ? cam.fov : 90);
      if (cards.sel.w.roll) cards.sel.w.roll.set(typeof cam.roll === 'number' ? cam.roll : 0);
      cards.sel.setSub(hasSel
        ? ('Camera ' + (sel + 1) + ' of ' + list.length + ' · FOV ' +
           (typeof cam.fov === 'number' ? cam.fov.toFixed(0) : '-') + ' · roll ' +
           (typeof cam.roll === 'number' ? cam.roll.toFixed(0) : '-') + '°')
        : (list.length ? (list.length + ' cameras · none selected') : 'No cameras placed'));
    }

    // --- path
    if (cards.path) {
      var interp = st.pathSmooth ? 'smooth' : 'linear';
      var ease = EASE_NAMES[(cam.ease || 0)] || 'none';
      var speed = SPEED_NAMES[(cam.speedMode || 0)] || 'manual';
      // The last marker has no outgoing segment, so cam.speedMul falls back to 1.0 server-side
      // (Shot_AppendCameraStateJson) exactly like CS2's stepMul() does for st.sel == null.
      var perSegMode = speed === 'perseg', constMode = speed === 'constant';
      var isLastKey = hasSel && sel === list.length - 1;
      var amount = perSegMode
        ? (typeof cam.speedMul === 'number' ? cam.speedMul : 1)
        : (typeof cam.constSpeed === 'number' ? cam.constSpeed : 1);
      // CS2's Amount readout and enable rule (CameraEditorPathInspectorJs.h ~288-297): Per-Segment
      // edits the selected key's outgoing segment (never the last key's), Constant the whole path,
      // and Manual has no multiplier at all.
      var amountUsable = (perSegMode && hasSel && !isLastKey) || constMode;
      var amountText = perSegMode ? (hasSel ? ('Segment  x' + amount.toFixed(2)) : 'select a key')
        : (constMode ? ('Constant  x' + amount.toFixed(2)) : 'not used in Manual');
      if (cards.path.w.interp) cards.path.w.interp.set(interp);
      if (cards.path.w.ease) cards.path.w.ease.set(ease);
      if (cards.path.w.speedMode) cards.path.w.speedMode.set(speed);
      if (cards.path.w.speedAmount) cards.path.w.speedAmount.set(amount, amountText, amountUsable);
      if (cards.path.w.speedHint) cards.path.w.speedHint.set(perSegMode
        ? 'Per-Segment: each key carries its own speed into the next one.'
        : (constMode ? 'Constant: one speed for the whole path.' : 'Manual: the path follows the demo timeline.'));
      if (cards.path.w.timing) cards.path.w.timing.set(cam.freeze ? 'freeze' : 'live');
      // CS2's collapsed-card summary, word for word (CameraEditorPathInspectorJs.h pathSec.__note).
      cards.path.setSub((interp === 'smooth' ? 'Smooth' : 'Linear') + ' · '
        + (hasSel ? ({ none: 'no ease', 'in': 'ease in', out: 'ease out', inout: 'ease in/out' }[ease] || 'no ease')
                  : 'no key selected') + ' · '
        + (perSegMode ? ('per-segment ' + amount.toFixed(2) + 'x')
           : constMode ? ('constant ' + amount.toFixed(2) + 'x')
           : 'manual'));
    }

    if (cards.keys) cards.keys.setSub('K place · Space play · J mouse');

    // --- follow
    var f = st.follow || {};
    if (cards.follow) {
      heroKey = (f.hero === undefined || f.hero === null) ? '' : String(f.hero);
      attachKey = f.attach || 'chest';
      var w = cards.follow.w;
      // Three physical rows (one per sub-page, see followPersistRows above), all reflecting the
      // same mode -- each must be set explicitly since none of them share a single tracked widget.
      var fMode = f.mode || 'lockon';
      if (w.camModeTarget) w.camModeTarget.set(fMode);
      if (w.camModeFraming) w.camModeFraming.set(fMode);
      if (w.camModeTracking) w.camModeTracking.set(fMode);
      var attachModeOnFollow = fMode === 'attach';
      // Place/Clear (indices 0, 2) hide as a pair in Attach; Reposition (index 1), same row, never
      // does -- CS2 parity, see camThisCameraRow's own comment above.
      var thisCamVis = [!attachModeOnFollow, true, !attachModeOnFollow];
      if (w.camThisCameraTarget) w.camThisCameraTarget.set(thisCamVis);
      if (w.camThisCameraFraming) w.camThisCameraFraming.set(thisCamVis);
      if (w.camThisCameraTracking) w.camThisCameraTracking.set(thisCamVis);
      if (w.hero) w.hero.set(heroLabel());
      if (w.attach) {
        w.attach.set(attachLabel());
        // The attachment picker is meaningless in lock-on: there is nothing to ride.
        X.show(w.attach.root, (f.mode || 'lockon') === 'attach');
      }
      if (w.folOffX) w.folOffX.set(num(f.offx, 0));
      if (w.folOffY) w.folOffY.set(num(f.offy, 0));
      if (w.folOffZ) w.folOffZ.set(num(f.offz, 0));
      if (w.folRotPitch) w.folRotPitch.set(num(f.rotpitch, 0));
      if (w.folRotYaw) w.folRotYaw.set(num(f.rotyaw, 0));
      if (w.folRotRoll) w.folRotRoll.set(num(f.rotroll, 0));
      if (w.fovFollow) w.fovFollow.set(num(f.fov, 68));
      if (w.folFramingNote) w.folFramingNote.set(attachModeOnFollow
        ? 'Attach framing. X = forward, Y = right, Z = up from the mount. Pitch/Yaw/Roll rotate the ride. The camera moves with the hero.'
        : 'Lock-on framing. Off X/Y/Z move the AIM POINT in world axes; Pitch/Yaw/Roll trim the look. The camera itself stays exactly where it was placed.');
      // CS2 hides its whole aim-smoothing group in Attach (folAimSettings.visible = !attachModeOn,
      // CameraEditorFollowInspectorJs.h line 411) since a rigid mount has nothing to smooth toward;
      // death/retarget/hold-last stay visible in both modes here (see the comment above these tiles).
      if (w.look) { w.look.set(num(f.look, 0.35)); X.show(w.look.root, !attachModeOnFollow); }
      if (w.position) { w.position.set(num(f.pos, 0.4)); X.show(w.position.root, !attachModeOnFollow); }
      if (w.prediction) { w.prediction.set(num(f.predict, 0.08)); X.show(w.prediction.root, !attachModeOnFollow); }
      if (w.deadzone) { w.deadzone.set(num(f.deadzone, 1.5)); X.show(w.deadzone.root, !attachModeOnFollow); }
      if (w.maxTurn) { w.maxTurn.set(num(f.maxturn, 420)); X.show(w.maxTurn.root, !attachModeOnFollow); }
      if (w.folTrackingNote) w.folTrackingNote.set(attachModeOnFollow
        ? 'Attached tracking. The mount follows the hero directly, so aim smoothing does not apply. Death and lost-target settings still apply.'
        : 'Lock-on tracking. Aim smoothing, death and lost-target settings all apply while placed and watching a hero.');
      if (w.deathHold) w.deathHold.set(num(f.deathhold, 2));
      if (w.autoDead) w.autoDead.set(f.releaseOnDeath ? 'hold' : 'keep');
      if (w.holdLast) w.holdLast.set(!!f.holdLast);
      if (w.retarget) w.retarget.set(!!f.retarget);
      cards.follow.setPill(!!f.on);
      cards.follow.setEdge(f.on ? S.green : X.C.edgeOff);
      cards.follow.setSub(f.on
        ? ((fMode === 'lockon' ? 'Lock-on' : 'Attach') + ' · ' + heroLabel() +
           ' · FOV ' + num(f.fov, 68).toFixed(0))
        : 'Off');
    }

    refreshCollapse();
  }

  function num(v, fallback) { return (typeof v === 'number') ? v : fallback; }

  // `gotoSub` is the automation seam: the verification scripts and `mirv_filmmaker ui_eval` need a
  // way to put the column on a given sub-tab without a click, and a UI that can only be reached
  // by pointing at it cannot be checked by anything but a person.
  $.MvmCameraBody = {
    build: build, setVisible: setVisible, render: render,
    gotoSub: function (k) { setSub(k === 'follow' ? 'follow' : 'path'); }
  };
})();
)CFJS"
