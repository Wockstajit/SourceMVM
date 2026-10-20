#pragma once

// FOLLOW / ATTACH CAMERA inspector: one card, three sub-pages. JS fragment concatenated (via
// #include, as an adjacent raw string literal) into the kCameraEditorJs body assembled in
// CameraEditorJs.h -- not a standalone script; it shares scope with the root IIFE. MUST follow
// CameraEditorCardsJs.h, whose card/tile builders it uses.
//
// LAYOUT (Docs/features/camera-editor-feature.md):
//
//   pinned bar         the target line, PRESS LIVE / FOLLOWING LIVE, and Live / Stop -- above
//                      the rail, so Live works from every card
//   SETUP / CAMERA & TARGET   mode, THIS CAMERA (Place / Reposition / Clear), then TARGET
//   SHOT  / FRAMING           offsets / rotation / FOV, Reset in the header
//   SHOT  / TRACKING          Aim (Lock-on only) / On death, Reset in the header
//
// NAME COLLISIONS ARE REAL HERE. Every *Js.h fragment lands in ONE function scope, so a `var` in
// this file is visible to the render workspace and the Customize modal. Everything introduced
// here is prefixed `fol` (or keeps a name this file already owned).

R"EDJS(
    // ===================== the pinned status bar ========================
    // Above the rail (CameraEditorJs.h builds edFollowBar): the target line, whether it is
    // following, and Live / Stop -- reachable from every Follow card, not only the first.
    var folBarCol = mk('Panel', edFollowBar); folBarCol.hittest = false;
    folBarCol.style.flowChildren = 'down'; folBarCol.style.width = 'fill-parent-flow(1.0)';
    folBarCol.style.verticalAlign = 'center';
    var followStatus = lbl(folBarCol, 'Place a camera and select a target.', S.value, 13);
    followStatus.hittest = false; followStatus.style.fontWeight = 'bold';
    followStatus.style.whiteSpace = 'nowrap'; followStatus.style.textOverflow = 'ellipsis'; followStatus.style.width = '100%';
    var folLiveState = lbl(folBarCol, '', S.accent, 11);
    folLiveState.hittest = false; folLiveState.style.fontWeight = 'bold'; folLiveState.style.letterSpacing = '1.2px';
    folLiveState.style.textTransform = 'uppercase';
    folLiveState.style.marginTop = '2px';
    var followPreview = btnPrimary(edFollowBar, 'Live', function () {
      cmd((st && st.follow && st.follow.enabled) ? 'mirv_filmmaker follow stop' : 'mirv_filmmaker follow preview');
    });
    followPreview.style.verticalAlign = 'center'; followPreview.style.marginRight = '0px';
    followPreview.style.paddingTop = '6px'; followPreview.style.paddingBottom = '6px';
    followPreview.style.paddingLeft = '16px'; followPreview.style.paddingRight = '16px';
    followPreview.__lbl.style.fontSize = '12px';

    // ===================== CAMERA & TARGET ==============================
    // The persistent rows and the old Target page, merged into ONE card (Docs/features/camera-editor-feature.md): mode,
    // what this camera does, then what it watches.
    var followSec = edCard(inspectorBody, 'CAMERA & TARGET', { edge: S.accent, rail: { tab: 'follow', key: 'ftarget', group: 'Setup' } });
    var folPersist = followSec;
    var folTargetPage = followSec;

    // Lock-on (a placed turret) vs Attach (ride-along), as a mode row.
    var camModeBtns = edModeRow(folPersist, 'Camera mode',
      [['lockon', 'Lock-on'], ['attach', 'Attached']], null,
      function (v) { cmd('mirv_filmmaker follow mode ' + v); },
      'Lock-on is a placed camera that turns to follow; Attach rides along with the target.');
    var folModeNote = edNote(folPersist, '');
    var folCamActions = edEvenActions(folPersist, 'This camera', [
      ['Place', function () { cmd('mirv_filmmaker follow place'); }, 'primary'],
      ['Reposition', function () { cmd('mirv_filmmaker follow reposition'); }],
      ['Clear', function () { cmd('mirv_filmmaker follow clear'); }, 'danger']
    ]);
    var followPlace = folCamActions.btns[0];
    var followReposition = folCamActions.btns[1];
    var followClear = folCamActions.btns[2];
    var followWarn = lbl(folPersist, '', '#f4c95dff', 12);
    followWarn.hittest = false; followWarn.style.whiteSpace = 'normal';
    followWarn.style.width = '100%'; followWarn.style.marginTop = '6px';
    followWarn.style.paddingLeft = UI.line.padH + 'px'; followWarn.style.paddingRight = UI.line.padH + 'px';

    edSubHead(folTargetPage, 'Target');

    // A dropdown row in the line grammar: the 130px label column, the bar, an optional side
    // button. These lists are long and live (targets, events, mounts), which is the one case that
    // keeps a real anchored popup.
    function folDropRow(parent, label, id, onPick, tip, sideCap, sideCmd) {
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'right'; r.style.width = '100%';
      r.style.marginTop = (UI.line.gap + 1) + 'px';
      r.style.paddingLeft = UI.line.padH + 'px';
      edEndTileLine(parent);
      var nl = lbl(r, label, S.label, 13);
      nl.style.width = UI.line.label + 'px'; nl.style.verticalAlign = 'center'; nl.style.whiteSpace = 'nowrap';
      edTip(nl, tip);
      var host = mk('Panel', r); host.hittest = false; host.hittestchildren = true;
      host.style.flowChildren = 'right'; host.style.width = 'fill-parent-flow(1.0)';
      host.style.verticalAlign = 'center'; host.style.marginLeft = UI.line.colGap + 'px';
      var drop = customDrop(host, id, onPick);
      var side = null;
      if (sideCap) {
        side = edButton(r, sideCap, sideCmd);
        side.style.marginLeft = '6px'; side.style.marginRight = '0px';
        side.style.verticalAlign = 'center';
      }
      return { row: r, drop: drop, side: side };
    }

    // Target type: ONE dropdown whose options depend on the camera mode.
    var TYPE_LOCKON = [['Player', 'player'], ['Grenade', 'grenade'], ['Weapon/C4', 'weapon']];
    var TYPE_ATTACH = [['Grenade', 'grenade'], ['Player Bone', 'player'], ['Weapon Attach', 'weapon']];
    var folTypeRow = folDropRow(folTargetPage, 'Type', 'FmTypeDrop',
      function (value) { cmd('mirv_filmmaker follow type ' + value); },
      'What the camera locks onto, or mounts to in Attach mode.');
    var typeDrop = folTypeRow.drop;

    var folPlayerRow = folDropRow(folTargetPage, 'Player', 'FmWeaponPlayerDrop',
      function (value) { if (value != null && value !== '') cmd('mirv_filmmaker follow weaponplayer ' + value); },
      'Whose weapon to follow. The target list below is filtered to this player.');
    var weaponPlayerDrop = folPlayerRow.drop, weaponPlayerRow = folPlayerRow.row;
    function renderWeaponPlayers(f) {
      var players = (f && f.players) || [], opts = [];
      for (var i = 0; i < players.length; i++) {
        var p = players[i];
        opts.push([p.name + (p.team && p.team !== '-' ? ' · ' + p.team : '') + (!p.alive ? ' · dead' : ''), p.index]);
      }
      weaponPlayerDrop.update(opts, f.weaponFilterPlayer, 'players:' + opts.length + ':' + f.weaponFilterPlayer,
        opts.length ? null : 'No players available');
    }

    var SRC_OPTS = [['Auto', 'auto'], ['Held', 'held'], ['Dropped', 'dropped']];
    var folSrcRow = folDropRow(folTargetPage, 'Source', 'FmSrcDrop',
      function (value) { cmd('mirv_filmmaker follow weaponsource ' + value); },
      'Which copy of the weapon to follow. Auto takes whichever exists.');
    var srcDrop = folSrcRow.drop, srcRow = folSrcRow.row;

    var folTargetRow = folDropRow(folTargetPage, 'Target', 'FmTargetDrop',
      function (c) { if (c) chooseCandidate(c); },
      'The entity the camera follows.',
      'Go To', function () { cmd('mirv_filmmaker follow goto'); });
    var targetDrop = folTargetRow.drop, goToTargetBtn = folTargetRow.side;

    var folNearest = edActionGroup(folTargetPage, 'Closest candidate', [
      ['Select Nearest', function () { cmd('mirv_filmmaker follow nearest'); }]
    ], 1, '');
    var nearestBtn = folNearest.btns[0], nearestNameLbl = folNearest.note;

    // A far pick asks before it is used: a mis-click on a target across the map otherwise looks
    // like the camera broke.
    var pendingCandidate = null;
    var folConfirm = edActionGroup(folTargetPage, 'Target is far away', [
      ['Use anyway', function () {
        if (pendingCandidate) cmd('mirv_filmmaker follow select ' + pendingCandidate.index);
        pendingCandidate = null; folConfirm.row.visible = false;
      }],
      ['Cancel', function () { pendingCandidate = null; folConfirm.row.visible = false; }]
    ], 2, '');
    folConfirm.row.visible = false;
    var confirmRow = folConfirm.row, confirmText = folConfirm.note;

    function chooseCandidate(c) {
      if (st && st.follow && st.follow.type === 1) {
        cmd('mirv_filmmaker follow selecthandle ' + c.handle);
        return;
      }
      if (c.distanceLevel >= 2) {
        pendingCandidate = c;
        confirmText.text = (c.distanceLevel >= 3 ? 'Very far target' : 'Far target')
          + ' (' + Math.round(c.distance) + 'u). Use it anyway?';
        confirmRow.visible = true;
      } else cmd('mirv_filmmaker follow select ' + c.index);
    }
    function candidateText(f, c) {
      if (f.type === 1) {
        var delta = c.tickDelta >= 0 ? ('+' + c.tickDelta) : String(c.tickDelta);
        return (c.grenadeType || c.name) + ' · ' + (c.ownerName || 'unknown thrower')
          + (c.team !== '-' ? ' · ' + c.team : '') + ' · tick ' + c.throwTick
          + ' (' + delta + ') · ' + (c.status || 'nearby');
      }
      // The engine class name trails weapons: several entities present the SAME friendly name --
      // every knife skin is "Knife" -- so the class is the only thing that says which entity a
      // pick will actually follow.
      var cls = c.className ? ' · ' + c.className : '';
      if (c.isWeapon || c.isBomb) {
        return c.name + ' · ' + (c.status || (c.dropped ? 'dropped' : 'held')) + cls;
      }
      return c.name + (c.team && c.team !== '-' ? ' · ' + c.team : '')
        + (f.type === 0 && !c.alive ? ' · dead' : '')
        + ' · ' + Math.round(c.distance) + 'u';
    }
    function renderCandidates(f) {
      var MAXCAND = 40;
      var list = (f && f.candidates) || [];
      if (!list.length) {
        targetDrop.update([], null, 'empty:' + (f ? f.type : -1),
          (f && f.type === 1) ? 'No throws nearby' : 'No targets — Select Nearest');
        return;
      }
      // Distance is intentionally LEFT OUT of the rebuild signature: it changes every frame as
      // entities move, and rebuilding rows would close an open popup. Only the SET of targets
      // (and which one is selected) triggers a rebuild; the collapsed bar still updates live.
      var opts = [], sigParts = [], displayText = null, n = Math.min(MAXCAND, list.length);
      for (var j = 0; j < n; j++) {
        var c2 = list[j];
        var sel = !!(f.targetValid && ((c2.index === f.targetIndex && f.targetIndex >= 0)
                                       || (c2.handle && c2.handle === f.targetHandle)));
        var meta = { selected: sel };
        if (c2.distanceLevel >= 2) meta.color = '#f4c95dff';
        var text = candidateText(f, c2);
        opts.push([text, c2, meta]);
        if (sel) displayText = text;
        sigParts.push(c2.index + ':' + c2.handle + ':' + c2.name + ':' + c2.status + ':'
                      + c2.throwTick + (sel ? ':S' : ''));
      }
      targetDrop.update(opts, null, sigParts.join('|') + ':' + f.type,
        displayText || ('Select target (' + n + ')'));
    }
)EDJS"
R"EDJS(
    var folAttachRow = folDropRow(folTargetPage, 'Mount', 'FmAttachDrop',
      function (value) { if (value) cmd('mirv_filmmaker follow bone ' + value); },
      'Bone or attachment the camera mounts to. Read off the selected entity, so the list '
      + 'changes with the target. Head follows the skull bone; Eyes follows the view. '
      + 'Weapon switches keep your saved mount and retry it when the gun returns.');
    var attachDrop = folAttachRow.drop, attachRow = folAttachRow.row;
    function renderAttachmentMenu(f) {
      var options = [];
      if (f && f.attachPoints && f.attachPoints.length) options = f.attachPoints;
      var list = (f && f.candidates) || [], selected = null;
      for (var i = 0; i < list.length; i++) {
        if ((list[i].index === f.targetIndex) || (f.targetHandle && list[i].handle === f.targetHandle)) {
          selected = list[i]; break;
        }
      }
      if (!options.length && selected && selected.attachPoints && selected.attachPoints.length) {
        options = selected.attachPoints;
      }
      if (!options.length && selected && selected.attachments && selected.attachments.length) {
        options = [];
        for (var si = 0; si < selected.attachments.length; si++) {
          options.push({ id: selected.attachments[si], label: selected.attachments[si],
                         kind: 'attachment', valid: true });
        }
      }
      var hasCurrent = false;
      for (var oi = 0; oi < options.length; oi++) if (options[oi].id === f.attachment) hasCurrent = true;
      if (f && f.attachment && !hasCurrent) {
        options.unshift({ id: f.attachment, label: f.attachment + ' (saved; unavailable here)', kind: '', valid: false });
      }
      var opts = [];
      for (var j = 0; j < options.length; j++) {
        var p = options[j], name = p.id || p.label || 'entity', label = p.label || name;
        if (p.kind && p.kind !== 'attachment') label += ' · ' + p.kind;
        if (!p.valid && p.kind) label += ' · unavailable';
        opts.push([label, name]);
      }
      if (!opts.length) opts = [['No valid attach points', '']];
      attachDrop.update(opts, (f && f.attachment) || '');
    }

    var folEventRow = folDropRow(folTargetPage, 'Event', 'FmEventDrop',
      function (value) { if (value != null && value !== '') cmd('mirv_filmmaker follow eventselect ' + value); },
      'Drop and pickup events scanned from the demo, so this covers the whole round rather than '
      + 'what is on screen.');
    var eventDrop = folEventRow.drop, eventRow = folEventRow.row;
    var folEventGroup = edActionGroup(folTargetPage, 'Events', [
      ['Go To Event', function () { cmd('mirv_filmmaker follow previewtick'); }]
    ], 1, 'Jumps the demo to that moment, so you can mount the camera on an item as it changes hands.');
    var previewTickBtn = folEventGroup.btns[0], eventsLabel = folEventGroup.row;
    function eventText(e) {
      var label = e.type.replace('weapon_drop', 'death drop').replace('bomb_dropped', 'C4 drop')
        .replace('bomb_pickup', 'C4 pickup').replace('bomb_planted', 'C4 plant').replace('item_pickup', 'pickup');
      return 'tick ' + e.tick + '  ' + label + (e.item ? ('  ' + e.item) : '')
        + (e.ownerName ? ('  ' + e.ownerName) : '');
    }
    function renderEvents(f) {
      var list = (f && f.events) || [];
      var sig = (f ? (f.eventStatus + ':' + f.selectedEvent + ':') : '') + list.length;
      for (var i = 0; i < list.length; i++) sig += '|' + list[i].index + ':' + list[i].tick;
      if (!list.length) {
        var scanText = 'No drop/pickup events near here.';
        if (f && f.eventStatus === 1) {
          scanText = 'Scanning demo for drop/pickup events...'
            + (f.eventScanElapsedMs ? (' ' + Math.round(f.eventScanElapsedMs / 1000) + 's') : '');
        } else if (f && f.eventStatus === 3) {
          scanText = f.eventError ? ('Event scan failed: ' + f.eventError)
                                  : 'Event scan unavailable for this demo.';
        }
        eventDrop.update([], null, 'empty:' + (f ? f.eventStatus : -1), scanText);
        return;
      }
      var opts = [], displayText = null;
      for (var j = 0; j < list.length; j++) {
        var e = list[j], text = eventText(e), on = (f.selectedEvent === e.index);
        opts.push([text, e.index, { selected: on, color: on ? S.accent : S.value }]);
        if (on) displayText = text;
      }
      eventDrop.update(opts, f.selectedEvent, sig, displayText || ('Select event (' + list.length + ')'));
    }

    // Attach diagnostics, shown only with `follow debug` on and a live attach.
    var attachDebugText = lbl(folTargetPage, '', '#9effa0ff', 11);
    attachDebugText.hittest = false;
    attachDebugText.style.fontFamily = 'Consolas, "Courier New", monospace';
    attachDebugText.style.whiteSpace = 'normal'; attachDebugText.style.width = '100%';
    attachDebugText.style.marginTop = '9px';
)EDJS"
R"EDJS(
    // ===================== FRAMING ======================================
    // The drag is driven from the C++ OS-cursor pipe: RELATIVE and damped, so a 512-unit offset
    // is placeable instead of ~5 units per pixel.
    // ONE card for the shot (2026-10 sidebar cleanup): FRAMING and TRACKING as its two sub-tabs,
    // one Reset for both in the header.
    var folFrameSec = edCard(inspectorBody, 'FRAMING & TRACKING', { edge: S.accent, rail: { tab: 'follow', key: 'fshot', group: 'Shot' } });
    folFrameSec.__headerReset(function () {
      cmd('mirv_filmmaker follow reset framing');
      cmd('mirv_filmmaker follow reset tracking');
    });
    var folFramingPage = folFrameSec.__page('Framing');
    var folFramingNote = edNote(folFramingPage, '');
    folFramingNote.style.marginTop = '0px'; folFramingNote.style.marginBottom = '2px';
    edSubHead(folFramingPage, 'Offset');
    var xformSliders = [];
    function folTile(parent, sink, name, lo, hi, decimals, unit, getVal, command, color, tip) {
      var handle = edValueTile(parent, {
        label: name, min: lo, max: hi, decimals: decimals, unit: unit || '', fill: color, tip: tip,
        onChange: function (v) { cmd('mirv_filmmaker follow ' + command + ' ' + v.toFixed(3)); }
      });
      handle.getVal = getVal;
      sink.push(handle);
      return handle;
    }
    folTile(folFramingPage, xformSliders, 'Off X', -256, 256, 1, 'u',
      function (f) { return f.offset ? f.offset[0] : 0; }, 'offsetx', S.axis[0],
      'Lock-on: world X aim offset. Attached: forward / back from the mount.');
    folTile(folFramingPage, xformSliders, 'Off Y', -256, 256, 1, 'u',
      function (f) { return f.offset ? f.offset[1] : 0; }, 'offsety', S.axis[1],
      'Lock-on: world Y aim offset. Attached: right / left from the mount.');
    folTile(folFramingPage, xformSliders, 'Off Z', -256, 256, 1, 'u',
      function (f) { return f.offset ? f.offset[2] : 0; }, 'offsetz', S.axis[2],
      'Lock-on: world height aim offset. Attached: up / down in the mount axes.');
    edSubHead(folFramingPage, 'Rotation');
    folTile(folFramingPage, xformSliders, 'Pitch', -180, 180, 0, '°',
      function (f) { return f.rotation ? f.rotation[0] : 0; }, 'rotpitch', S.axis[3],
      'Camera pitch relative to the target.');
    folTile(folFramingPage, xformSliders, 'Yaw', -180, 180, 0, '°',
      function (f) { return f.rotation ? f.rotation[1] : 0; }, 'rotyaw', S.axis[4],
      'Camera yaw relative to the target.');
    folTile(folFramingPage, xformSliders, 'Roll', -180, 180, 0, '°',
      function (f) { return f.rotation ? f.rotation[2] : 0; }, 'rotroll', S.axis[5],
      'Camera roll relative to the target.');
    edSubHead(folFramingPage, 'Lens');
    folTile(folFramingPage, xformSliders, 'FOV', 1, 170, 0, '',
      function (f) { return f.fov || 90; }, 'fov', S.axis[6],
      'Field of view for the follow camera.');

    // ===================== TRACKING =====================================
    // Pages Aim / On death (Docs/features/camera-editor-feature.md). Attached is a rigid mount, so only Lock-on exposes aim
    // smoothing and prediction.
    var folTrackSec = folFrameSec;   // the merged card answers to both names
    var folTrackPage = folFrameSec.__page('Tracking');
    var folTrackingNote = edNote(folTrackPage, '');
    folTrackingNote.style.marginTop = '2px';
    edSubHead(folTrackPage, 'Aim');
    var folDeathPage = folTrackPage;
    function folSettingsGroup(parent) {
      edEndTileLine(parent);
      var p = mk('Panel', parent); p.hittest = false; p.hittestchildren = true;
      p.style.flowChildren = 'down'; p.style.width = '100%';
      return p;
    }
    var folAimSettings = folSettingsGroup(folTrackPage);
    var followSliders = [];
    folTile(folAimSettings, followSliders, 'Aim smoothing', 0, 1, 2, 's',
      function (f) { return f.look; }, 'look', S.axis[4],
      'Aim smoothing half-time. Zero snaps to the target; higher values ease the turn.');
    folTile(folAimSettings, followSliders, 'Target smoothing', 0, 1, 2, 's',
      function (f) { return f.position; }, 'position', S.axis[0],
      'Smooths the point the placed camera watches. The camera position stays fixed.');
    folTile(folAimSettings, followSliders, 'Predict', 0, 0.5, 2, 's',
      function (f) { return f.prediction; }, 'prediction', S.axis[2],
      'Leads the target by this much, so fast movers stay framed.');
    folTile(folAimSettings, followSliders, 'Deadzone', 0, 10, 1, '°',
      function (f) { return f.deadzone; }, 'deadzone', S.axis[3],
      'Angle the target may move inside before the camera reacts.');
    folTile(folAimSettings, followSliders, 'Turn/s', 0, 1440, 0, '°',
      function (f) { return f.maxTurn; }, 'maxturn', S.axis[5],
      'Ceiling on how fast the camera may swing.');

    var optionBtns = {};
    function followToggle(parent, key, label, command, tip) {
      optionBtns[key] = edSwitchTile(parent, label, tip, function () {
        var f = st && st.follow;
        cmd('mirv_filmmaker follow ' + command + ' ' + ((f && f[key]) ? 0 : 1));
      });
      return optionBtns[key];
    }
    var folDeathSettings = folSettingsGroup(folDeathPage);
    edSubHead(folDeathSettings, 'On death');
    var folDeathMode = edModeRow(folDeathSettings, 'On death',
      [['keep', 'Keep following'], ['timed', 'Timed hold']], null,
      function (v) { cmd('mirv_filmmaker follow autodead ' + (v === 'timed' ? 1 : 0)); },
      'Keep following watches the ragdoll while it exists. Timed hold stops after the duration below.');
    var folHoldSettings = folSettingsGroup(folDeathSettings);
    folTile(folHoldSettings, followSliders, 'Death hold', 0, 10, 1, 's',
      function (f) { return f.deathHold; }, 'deathhold', S.axis[6],
      'Demo playback seconds following the ragdoll before stopping or retargeting. Pausing pauses this timer.');
    var folRetargetSettings = folSettingsGroup(folDeathSettings);
    followToggle(folRetargetSettings, 'switchWeapon', 'Retarget dropped weapon', 'switchweapon',
      'After the death hold, follow the dropped weapon.');
    followToggle(folRetargetSettings, 'switchBomb', 'Retarget dropped C4', 'switchbomb',
      'After the death hold, follow dropped C4 when available.');
    var folDeathNote = edNote(folDeathSettings, '');
    var folLostSettings = folSettingsGroup(folDeathPage);
    edSubHead(folLostSettings, 'When target disappears');
    followToggle(folLostSettings, 'holdLast', 'Hold last camera pose', 'hold',
      'Keep the last pose only after the target or ragdoll disappears. While the body exists, follow its movement.');
)EDJS"
R"EDJS(
    // ===================== the per-frame sync ===========================
    function syncFollowInspector() {
      var f = st.follow || {};
      var attachModeOn = (f.mode === 1);

      camModeBtns.sync(attachModeOn ? 'attach' : 'lockon');
      folModeNote.text = attachModeOn
        ? 'Attached: the camera moves and rotates with the selected mount.'
        : 'Lock-on: the camera stays where you place it and turns to watch the target.';
      folFramingNote.text = attachModeOn
        ? 'Attached framing. X = forward, Y = right, Z = up in the mount axes. Reposition captures your current view.'
        : 'Lock-on framing. Offsets move the aim point in world axes; rotation trims the aim. The camera stays placed.';
      folTrackingNote.text = attachModeOn
        ? 'Attached is a rigid mount: it follows the body directly, so there is no aim smoothing. The On death page still applies.'
        : 'Lock-on tracking. These apply only to Lock-on.';
      folAimSettings.visible = !attachModeOn;
      folDeathSettings.visible = (f.type === 0 || (f.type === 2 && f.weaponPlayerIndex >= 0));
      folRetargetSettings.visible = (f.type === 0);
      folDeathMode.sync(f.autoDead ? 'timed' : 'keep');
      var folWillRetarget = f.type === 0 && (f.switchWeapon || f.switchBomb);
      folHoldSettings.visible = !!(f.autoDead || folWillRetarget);
      folDeathNote.text = folWillRetarget
        ? 'Follow the ragdoll for the hold duration, then switch to the selected dropped item.'
        : (f.autoDead ? 'Follow the moving ragdoll, then stop when the hold ends. Zero stops immediately.'
                      : 'Follow the moving ragdoll for as long as it exists. No death timer is active.');

      // Options follow the mode; the selection reflects the current type.
      var typeVal = (f.type === 1) ? 'grenade' : (f.type === 2 ? 'weapon' : 'player');
      typeDrop.update(attachModeOn ? TYPE_ATTACH : TYPE_LOCKON, typeVal);

      // Place / Clear are lock-on only. Reposition is shared: lock-on moves the placed camera,
      // attach captures the current freecam pose as offset / rotation / FOV relative to the bone.
      followPlace.visible = !attachModeOn;
      followClear.visible = !attachModeOn;
      followPlace.__lbl.text = f.hasCamera ? 'Replace' : 'Place';
      // Live = the preview toggle. It is the card's one primary action, so it stays in the header.
      followPreview.__lbl.text = f.enabled ? 'Stop' : 'Live';
      folLiveState.text = f.enabled ? 'Following live' : 'Press Live to see it follow';
      folLiveState.style.color = f.enabled ? '#35cc59ff' : S.accent;
      followSec.__badge(f.enabled ? 'LIVE' : '', 'live');

      var haveTarget = (f.targetIndex >= 0) || (f.type === 2 && f.weaponPlayerIndex >= 0);
      // Target chosen but not enabled is the state users get stuck in: FollowCamera::RunFrame
      // early-returns on !enabled, so picking a target changes NOTHING on screen until Live is
      // pressed. Say so on the status line the user is already reading.
      followStatus.text = haveTarget
        ? ((f.targetName || ('Entity #' + f.targetIndex)) + ' — ' + (f.typeName || 'Player')
           + (attachModeOn ? ' (attach)' : ' (lock-on)'))
        : (attachModeOn ? 'Select a target to ride.' : 'Place a camera, then select a target.');
      followStatus.style.color = f.targetValid ? S.value : S.inactive;
      followWarn.text = f.repositioning
        ? (attachModeOn ? 'Reposition active: move in freecam, then left-click to save the attach pose.'
                        : 'Reposition active: move in freecam, then left-click to place.')
        : (f.message ? f.message
        : (!attachModeOn && f.distanceLevel >= 3 ? 'Very far / likely outside the useful shot area.'
        : (!attachModeOn && f.distanceLevel >= 2 ? 'Selected target is far from this camera.' : '')));
      followWarn.visible = !!followWarn.text;

      nearestBtn.__lbl.text = (f.type === 1) ? 'Nearest Throw' : 'Select Nearest';
      var nearestCand = (f.candidates && f.candidates.length) ? f.candidates[0] : null;
      nearestNameLbl.text = nearestCand
        ? ('closest: ' + nearestCand.name
           + (nearestCand.team && nearestCand.team !== '-' ? '  ·  ' + nearestCand.team : ''))
        : 'closest: —';
      nearestNameLbl.style.color = nearestCand ? S.value : S.inactive;
      renderCandidates(f);

      // Weapon source (Weapon type only).
      weaponPlayerRow.visible = (f.type === 2);
      srcRow.visible = (f.type === 2);
      if (f.type === 2) {
        renderWeaponPlayers(f);
        var srcVal = (f.weaponSource === 1) ? 'held' : (f.weaponSource === 2 ? 'dropped' : 'auto');
        srcDrop.update(SRC_OPTS, srcVal);
      }
      // Attach point (attach mode only).
      attachRow.visible = attachModeOn;
      if (attachModeOn) renderAttachmentMenu(f);

      var ad = f.attachDebug || {};
      attachDebugText.visible = !!(f.debug && attachModeOn && ad.active);
      attachDebugText.text = attachDebugText.visible
        ? ('entity ' + (ad.entityIndex == null ? '-' : ad.entityIndex)
           + '  attach ' + (ad.attachId || '-') + '  ' + (ad.source || '-') + '\n'
           + 'target ' + ((ad.rawTarget || []).join ? ad.rawTarget.join(', ') : '-') + '\n'
           + 'camera ' + ((ad.camera || []).join ? ad.camera.join(', ') : '-') + '\n'
           + 'dist ' + Number(ad.distance || 0).toFixed(1)
           + '  aim ' + Number(ad.aimError || 0).toFixed(1)
           + '  jitter ' + Number(ad.jitter || 0).toFixed(2)
           + '  dC ' + Number(ad.cameraDelta || 0).toFixed(2)
           + '  dT ' + Number(ad.targetDelta || 0).toFixed(2))
        : '';

      // Events + Go To Event (grenade throws / weapon-C4 drops).
      var eventsRelevant = (f.type === 1 || f.type === 2);
      eventsLabel.visible = eventsRelevant;
      eventRow.visible = (f.type === 2);
      previewTickBtn.__lbl.text = (f.type === 1)
        ? (f.grenadePending ? 'Waiting for Throw...' : 'Go To Throw')
        : 'Go To Event';
      btnSel(previewTickBtn, f.grenadePending);
      if (f.type === 2) renderEvents(f);
      else eventDrop.update([], null, 'hidden', 'No events');

      // Framing and tracking tiles. edValueTile.sync already refuses to write while the pointer
      // owns the gesture, so there is no mid-drag snap-back to guard against here.
      for (var xs = 0; xs < xformSliders.length; xs++) {
        xformSliders[xs].sync(num(xformSliders[xs].getVal(f)));
      }
      for (var fs = 0; fs < followSliders.length; fs++) {
        followSliders[fs].sync(num(followSliders[fs].getVal(f)));
      }
      for (var fk in optionBtns) if (optionBtns.hasOwnProperty(fk)) optionBtns[fk].set(!!f[fk]);

      var typeWord = { 0: 'Player', 1: 'Grenade', 2: 'Weapon / C4' }[f.type] || 'Player';
      if (attachModeOn) typeWord = { 0: 'Player bone', 1: 'Grenade', 2: 'Weapon attach' }[f.type] || 'Player bone';
      followSec.__note((attachModeOn ? 'Attached' : 'Lock-on') + ' · '
        + (haveTarget ? (f.targetName || typeWord) : 'no target')
        + (haveTarget && !attachModeOn && f.distance ? ' · ' + Math.round(num(f.distance)) + 'u' : ''));
      var off = f.offset || [0, 0, 0];
      folFrameSec.__note('Offset ' + num(off[0]).toFixed(0) + ' / ' + num(off[1]).toFixed(0) + ' / '
        + num(off[2]).toFixed(0) + ' · FOV ' + num(f.fov || 90).toFixed(0) + ' · '
        + (attachModeOn ? 'rigid mount' : ('smoothing ' + num(f.look).toFixed(2) + 's'))
        + ' · ' + (f.autoDead ? 'timed hold' : 'keep on death'));
    }
)EDJS"
