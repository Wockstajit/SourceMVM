#pragma once

// SCENE OBJECTS > (a light, mesh or camera) > RIG (Docs/features/scene-object-rigs.md). What the
// selected object rides -- a player at a body mount, a player's held weapon, a dropped weapon, a
// thrown grenade or another scene object --, where it sits in that frame, its pose keys on the
// TIMELINE dock, and for a camera, looking through it. State is st.rig (SceneRig_AppendStateJson);
// every action is a `mirv_filmmaker rig ...` command, so netcon automation drives the same paths.
// Lives in the Outliner card's olRigHost, under the light and mesh inspectors.

R"CFJS(
    function rigCommand(t) { cmd('mirv_filmmaker rig ' + t); }
    function rigState() { return (st && st.rig) || null; }
    function rigOf(id) { var r = rigState(); return r && r.objects ? (r.objects[String(id)] || null) : null; }
    function rigIsLightKind(k) { return k === 'point' || k === 'spot' || k === 'area'; }
    var RIG_KINDS = [['none', 'Nothing (stays in the world)'], ['player', 'A player'], ['held', "A player's weapon"],
                     ['dropped', 'A dropped weapon'], ['grenade', 'A thrown grenade'], ['object', 'Another scene object']];
    var RIG_KIND_LABEL = { player: 'a player', held: "a player's weapon", dropped: 'a dropped weapon', grenade: 'a grenade', object: 'a scene object' };

    var rigBox = mk('Panel', olRigHost);
    rigBox.style.width = '100%'; rigBox.style.flowChildren = 'down'; rigBox.style.marginTop = '4px';
    subHead(rigBox, 'Rig');
    var rigNote = envNote(rigBox, '');
    // A kind picked from "Attach to" before its "Which": the list fills from it, a pick attaches.
    var rigPendingKind = null, rigPendingFor = 0, rigWhoSig = '', rigMountSig = '';
    var rigKind = dropdownRow(rigBox, 'Attach to', RIG_KINDS, function (k) {
      var o = envSelected(); if (!o) return;
      if (k === 'none') { rigPendingKind = null; rigCommand('parent ' + o.id + ' none'); return; }
      rigPendingKind = k; rigPendingFor = o.id; rigWhoSig = '';
    });
    attachTip(rigKind.row, 'Attach it to something in the demo. It keeps its place on screen and follows from there.');
    var rigWho = dropdownRow(rigBox, 'Which', [], function (v) {
      var o = envSelected(); if (!o || v === '') return;
      var r = rigOf(o.id), k = rigPendingKind || (r && r.parent);
      if (!k || k === 'none') return;
      var mount = k === 'player' ? ' head' : k === 'held' ? ' muzzle' : '';
      rigCommand('parent ' + o.id + ' ' + k + ' ' + v + mount);
      rigPendingKind = null;
    }, { empty: 'Nothing of that kind at this tick. Scrub to where it is in the demo.' });
    var rigMount = dropdownRow(rigBox, 'Mount', [], function (m) {
      var o = envSelected(); if (o) rigCommand('mount ' + o.id + ' ' + m);
    });
    attachTip(rigMount.row, 'The point on it to ride: a bone on a player, an attachment such as the muzzle on a weapon.');
    var rigMissing = modeRow(rigBox, 'While it is gone', [['hide', 'Hide'], ['hold', 'Hold last pose']], '236px', function (k) {
      var o = envSelected(); if (o) rigCommand('missing ' + o.id + ' ' + k);
    }, 'Before a grenade is thrown or after it ends, or while a player is not in the demo.', 13);

    var rigPoseHead = subHead(rigBox, 'Offset'), rigPoseHeadLbl = rigPoseHead.GetChild(0);
    var rigPoseNote = envNote(rigBox, '');
    var rigSliders = [];
    function rigSlider(label, key, min, max, decimals, unit, tip) {
      var host = mk('Panel', rigBox); host.style.width = '100%'; host.style.flowChildren = 'down';
      var s = sliderRow(host, {
        label: label, min: min, max: max, decimals: decimals, unit: unit, tip: tip,
        onChange: function (v) { var o = envSelected(); if (o) rigCommand('set ' + o.id + ' ' + key + ' ' + v); }
      });
      rigSliders.push({ s: s, key: key, host: host, index: ['x', 'y', 'z', 'pitch', 'yaw', 'roll', 'fov'].indexOf(key) });
      return s;
    }
    rigSlider('Forward', 'x', -256, 256, 1, 'u', 'Along the mount: forward is where it faces.');
    rigSlider('Right', 'y', -256, 256, 1, 'u', 'To the mount’s right.');
    rigSlider('Up', 'z', -256, 256, 1, 'u', 'Above the mount.');
    rigSlider('Pitch', 'pitch', -180, 180, 0, '°');
    rigSlider('Yaw', 'yaw', -180, 180, 0, '°');
    rigSlider('Roll', 'roll', -180, 180, 0, '°');
    rigSlider('Field of view', 'fov', 1, 179, 0, '°', 'The camera’s field of view.');

    // The card's actions: full-width rows of equal buttons, one row per job (keys, the camera's
    // view, the object), instead of ragged right-aligned stacks under the sliders.
    function rigBtnRow(topMargin) {
      var r = mk('Panel', rigBox);
      r.style.flowChildren = 'right'; r.style.width = '100%'; r.style.marginTop = topMargin + 'px';
      endTileLine(rigBox);
      return r;
    }
    function rigBtn(row, text, onClick) {
      var b = actionBtn(row, text, onClick);
      b.style.width = 'fill-parent-flow(1.0)';
      row.__n = (row.__n || 0) + 1;
      b.style.marginLeft = row.__n > 1 ? '6px' : '0px';
      b.__lbl.style.horizontalAlign = 'center';
      return b;
    }
    var rigActs = rigBtnRow(12), rigActs2 = actionRowEnd(rigBox, 8);
    var rigPlaceBtn = rigBtn(rigActs, 'Place at camera', function () { var o = envSelected(); if (o) rigCommand('place ' + o.id); });
    attachTip(rigPlaceBtn, 'Moves it to where the camera is, facing where it looks. With Animate on, that is a key at the playhead.');
    var rigAnimBtn = rigBtn(rigActs, 'Animate', function () {
      var o = envSelected(), r = o && rigOf(o.id);
      if (!o || !r) return;
      if (!r.animated) { rigCommand('animate ' + o.id + ' on'); return; }
      if (r.keys > 1) cfgConfirm('Stop animating?', 'Its ' + r.keys + ' pose keys are removed. It keeps the pose it has at the playhead.', 'Remove keys',
        function () { rigCommand('animate ' + o.id + ' off'); });
      else rigCommand('animate ' + o.id + ' off');
    });
    attachTip(rigAnimBtn, 'Animate: Place at camera and pose edits key it at the playhead. Keys blend linearly; hold or ease them on the TIMELINE.');
    // A camera's own view: go there (the free cam at its pose), or render from it. Both land on its
    // tick first -- its first key, or a grenade's throw -- when the playhead is not already there.
    var rigViewActs = rigBtnRow(6);
    var rigGoBtn = rigBtn(rigViewActs, 'Go to camera', function () { var o = envSelected(); if (o) rigCommand('goto ' + o.id); });
    attachTip(rigGoBtn, 'Goes to its tick and puts the free cam where it looks from.');
    var rigLookBtn = rigBtn(rigViewActs, 'Look through', function () {
      var o = envSelected(); if (!o) return;
      var r = rigState();
      rigCommand('lookthrough ' + o.id + ' ' + (r && r.lookThrough === o.id ? 'off' : 'on'));
    });
    attachTip(rigLookBtn, 'Goes to its tick and renders from this camera. It outranks the camera path and the free cam until you stop.');
    var rigTimelineBtn = actionBtn(rigActs2, 'Open timeline', function () { cmd('mirv_filmmaker dock timeline'); });
    // A camera has no light or mesh inspector, so its card carries the object actions too.
    var rigCamActs = rigBtnRow(6);
    var rigEditBtn = rigBtn(rigCamActs, 'Edit position', function () {
      var o = envSelected(); if (!o) return;
      if (st && st.envEditing) envCommand('edit stop'); else envCommand('edit start ' + o.id);
    });
    rigBtn(rigCamActs, 'Duplicate', function () { var o = envSelected(); if (o) envCommand('duplicate ' + o.id); });
    olDanger(rigBtn(rigCamActs, 'Remove', function () { var o = envSelected(); if (o) envCommand('remove ' + o.id); }));
    var rigMsg = envNote(rigBox, '');

    function rigWhoItems(kind, self) {
      var r = rigState() || {};
      if (kind === 'player' || kind === 'held')
        return (r.players || []).map(function (p) {
          return [String(p.pawn), p.name + (p.team === 2 ? '  ·  T' : p.team === 3 ? '  ·  CT' : '') + (p.alive ? '' : '  (dead)')];
        });
      if (kind === 'dropped') return (r.dropped || []).map(function (d) { return [String(d.index), d.name + '  ·  #' + d.index]; });
      // In the air now (its entity index), or seen earlier this session (h<handle>; the object
      // then rides it when the playhead gets there). Same list the Follow Camera offers.
      if (kind === 'grenade') return (r.grenades || []).map(function (g) {
        return [g.active ? String(g.index) : 'h' + g.handle,
          g.type + (g.thrower ? ' by ' + g.thrower : '') + '  ·  thrown at ' + g.throwTick + (g.active ? '  ·  in the air' : '')];
      });
      if (kind === 'object') {
        var s = envState();
        return ((s && s.objects) || []).filter(function (o) { return o.id !== self; }).map(function (o) {
          return [String(o.id), (rigIsLightKind(o.kind) ? o.kind + ' light' : o.kind === 'camera' ? 'Camera' : (o.asset || o.kind)) + ' ' + o.id];
        });
      }
      return [];
    }

    renderHooks.push(function (st) {
      var o = envSelected();
      var it = olSel ? olFind(olSel) : null;
      var show = !!o && !!it && it.src === 'e' && olPicked().length <= 1;
      olRigHost.visible = show;
      if (!show) return;
      var r = rigOf(o.id) || { parent: 'none', attached: false, animated: false, keys: 0, local: [0, 0, 0, 0, 0, 0, 90], visible: true, valid: true };
      var camera = o.kind === 'camera';
      if (rigPendingFor !== o.id) rigPendingKind = null;
      var kind = rigPendingKind || r.parent || 'none';
      rigKind.sync(kind);

      // Which: the live candidates for the kind; attached shows what it rides even when that one is not listed.
      var items = rigWhoItems(kind, o.id), wsig = kind + '|' + JSON.stringify(items);
      if (wsig !== rigWhoSig) { rigWhoSig = wsig; rigWho.setItems(items); }
      rigWho.row.visible = kind !== 'none';
      if (rigPendingKind) rigWho.setText(items.length ? 'Choose ' + RIG_KIND_LABEL[kind] + ' to attach to' : 'None at this tick', true);
      else if (r.attached) {
        // The row it rides is ticked in the list; the field names it, even while it is gone.
        var current = '';
        if (kind === 'player' || kind === 'held') (rigState().players || []).forEach(function (p) { if (p.name === r.player) current = String(p.pawn); });
        if (kind === 'object') current = String(r.object);
        if (current) rigWho.sync(current);
        rigWho.setText(r.who || r.player || 'Not in the demo at this tick', !r.valid);
      }

      var mountable = r.attached && !rigPendingKind && (kind === 'player' || kind === 'held' || kind === 'dropped');
      rigMount.row.visible = mountable;
      if (mountable) {
        var rs = rigState(), mounts = r.mounts && r.mounts.length ? r.mounts : (kind === 'player' ? rs.playerMounts : rs.weaponMounts);
        var mi = (mounts || []).map(function (m) { return [m.id, m.label]; }), msig = JSON.stringify(mi);
        if (msig !== rigMountSig) { rigMountSig = msig; rigMount.setItems(mi); }
        rigMount.sync(r.mount);
      }
      rigMissing.row.visible = r.attached && !rigPendingKind;
      if (r.attached) rigMissing.sync(r.missing || 'hide');

      // The pose: the offset when it rides something; its turn (and a camera's view) otherwise.
      var rigged = r.attached || r.animated;
      rigPoseHead.visible = r.attached || camera;
      var head = r.attached ? 'Offset' : r.animated ? 'Pose at the playhead' : 'View';
      if (rigPoseHeadLbl.text !== head) rigPoseHeadLbl.text = head;
      rigPoseNote.visible = r.animated && !r.attached;
      rigPoseNote.text = 'Its position is the world’s: fly the camera there and Place at camera, or drag its arrows.';
      rigSliders.forEach(function (row) {
        // A light or mesh turns from its own card's Pitch / Yaw / Roll, at the top of the card; a
        // camera has no other card, so its turn lives here.
        var visible = row.key === 'fov' ? camera : row.index <= 2 ? r.attached : camera;
        row.host.visible = visible;
        if (visible) row.s.sync(row.key === 'fov' && !rigged ? (o.fov || 90) : (row.index <= 2 || rigged ? r.local[row.index] : (o.angles || [0, 0, 0])[row.index - 3]));
      });

      var lt = rigState() && rigState().lookThrough === o.id;
      rigAnimBtn.__lbl.text = r.animated ? 'Animate: on' : 'Animate: off';
      rigLookBtn.__lbl.text = lt ? 'Stop looking through' : 'Look through';
      rigViewActs.visible = camera;
      rigTimelineBtn.visible = r.animated;
      rigActs2.visible = r.animated;
      rigCamActs.visible = camera;
      rigEditBtn.__lbl.text = st.envEditing ? 'Done' : 'Edit position';

      var mount = r.mount ? r.mount.replace(/_/g, ' ') : '';
      var keys = r.animated ? '  ·  ' + r.keys + (r.keys === 1 ? ' pose key' : ' pose keys') + ', linear between keys' : '';
      rigNote.text = rigPendingKind ? 'Pick which one in the list below.'
        : r.attached ? (r.valid ? 'Rides ' + (r.who || RIG_KIND_LABEL[kind]) + (mount && kind !== 'grenade' && kind !== 'object' ? ' at the ' + mount : '') + keys
                                : (r.who || 'Its parent') + ' is not in the demo at this tick: ' + (r.visible ? 'held at its last pose.' : 'hidden until it is back.'))
        : r.animated ? 'Keyed in the world' + keys + '.'
        : 'Sits where you put it. Attach it to ride something in the demo, or Animate it to key its pose.';
      var msg = rigState() && rigState().message;
      rigMsg.visible = !!msg; rigMsg.text = msg || '';
    });
)CFJS"
