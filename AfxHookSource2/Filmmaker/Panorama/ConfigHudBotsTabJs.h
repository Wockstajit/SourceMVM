#pragma once

// BOTS tab of the CONFIG panel (Docs/features/bot-takes.md). JS fragment concatenated into the
// kConfigHudJs body assembled in ConfigHudJs.h; it shares scope with the root IIFE and appends to
// `pageBots` / `renderHooks`. Backend: Filmmaker/Takes/Takes.cpp (`mirv_filmmaker takes`, state in
// st.takes). Only shown in a private match: a demo has no server to put bots on.

R"CFJS(
    // ===================== BOTS: record yourself, then bots perform it ==================
    function btCommand(t) { cmd('mirv_filmmaker takes ' + t); }
    // The agent names come from the same generated catalog the Customize window reads.
    var COSMETICS = null;
    function rarColor() { return '#ffffffff'; }
)CFJS"
#include "CameraEditorCosmeticsCatalog.inc"
R"CFJS(
    var BT_WEAPONS = [
      ['weapon_glock', 'Glock-18'], ['weapon_hkp2000', 'P2000'], ['weapon_usp_silencer', 'USP-S'],
      ['weapon_p250', 'P250'], ['weapon_elite', 'Dual Berettas'], ['weapon_fiveseven', 'Five-SeveN'],
      ['weapon_tec9', 'Tec-9'], ['weapon_cz75a', 'CZ75-Auto'], ['weapon_deagle', 'Desert Eagle'],
      ['weapon_revolver', 'R8 Revolver'], ['weapon_ak47', 'AK-47'], ['weapon_m4a1', 'M4A4'],
      ['weapon_m4a1_silencer', 'M4A1-S'], ['weapon_galilar', 'Galil AR'], ['weapon_famas', 'FAMAS'],
      ['weapon_aug', 'AUG'], ['weapon_sg556', 'SG 553'], ['weapon_awp', 'AWP'], ['weapon_ssg08', 'SSG 08'],
      ['weapon_scar20', 'SCAR-20'], ['weapon_g3sg1', 'G3SG1'], ['weapon_mp9', 'MP9'], ['weapon_mac10', 'MAC-10'],
      ['weapon_mp7', 'MP7'], ['weapon_mp5sd', 'MP5-SD'], ['weapon_ump45', 'UMP-45'], ['weapon_p90', 'P90'],
      ['weapon_bizon', 'PP-Bizon'], ['weapon_nova', 'Nova'], ['weapon_xm1014', 'XM1014'], ['weapon_mag7', 'MAG-7'],
      ['weapon_sawedoff', 'Sawed-Off'], ['weapon_m249', 'M249'], ['weapon_negev', 'Negev']
    ];
    function btWeaponName(cls) {
      for (var i = 0; i < BT_WEAPONS.length; i++) if (BT_WEAPONS[i][0] === cls) return BT_WEAPONS[i][1];
      return String(cls).replace('weapon_', '').replace('item_', '');
    }
    function btAgents() {
      var out = [['-', T('Default')]];
      var list = (COSMETICS && COSMETICS.agent) || [];
      for (var i = 0; i < list.length; i++) {
        var o = list[i][2] || {};
        if (o.model) out.push([o.model, list[i][0]]);
      }
      return out;
    }

    // ---- RECORD & PLAY ----
    var btSec = section(pageBots, T('RECORD AND PLAY'), true, true, 'bots.scene', '#ff5c5cff');
    var btStatus = lbl(btSec, '', S.value, 14); btStatus.hittest = false;
    btStatus.style.whiteSpace = 'normal'; btStatus.style.width = '100%'; btStatus.style.marginTop = '6px';
    btStatus.style.paddingLeft = UI.line.padH + 'px'; btStatus.style.paddingRight = UI.line.padH + 'px';
    // The three controls this whole tab is about, so they get the full width.
    function btBigButtons(parent, items) {
      endTileLine(parent);
      var line = mk('Panel', parent); line.hittest = false; line.hittestchildren = true;
      line.style.flowChildren = 'right'; line.style.width = '100%'; line.style.marginTop = '10px';
      var out = [];
      for (var i = 0; i < items.length; i++) (function (caption, onClick, first) {
        var b = mk('Panel', line);
        b.style.width = 'fill-parent-flow(1.0)'; b.style.height = '36px'; b.style.borderRadius = '3px';
        if (!first) b.style.marginLeft = '6px';
        var l = lbl(b, caption, S.label, 14);
        l.hittest = false; l.style.fontWeight = 'bold'; l.style.whiteSpace = 'nowrap';
        l.style.horizontalAlign = 'center'; l.style.verticalAlign = 'center';
        makeInteractive(b, l, onClick, { fg: S.label });
        out.push(b);
      })(items[i][0], items[i][1], i === 0);
      return out;
    }
    var btActions = btBigButtons(btSec, [
      [T('Record'), function () { cmd('mvm_test menu off'); btCommand('record'); }],
      [T('Stop'), function () { btCommand('stop'); }],
      [T('Play'), function () { btCommand('play'); }]
    ]);
    var btRecBtn = btActions[0], btStopBtn = btActions[1], btPlayBtn = btActions[2];
    attachTip(btRecBtn, T('Closes this panel and records everything you do from where you stand. Takes you already recorded play along. {0} does the same.', K('takes.record', 'F6')));
    attachTip(btStopBtn, T('Ends the recording and keeps it, or stops the bots where they are.'));
    attachTip(btPlayBtn, T('Plays every take once as a real bot, while you stay where you are. A bot you killed comes back at its start. {0} does the same.', K('takes.play', 'F7')));
    var btLoop = togglePill(btSec, T('Loop'), T('Plays again from the start after the last take ends. When this is off, bots stay where their takes ended.'),
      function () { btCommand('loop ' + (st && st.takes && st.takes.loop ? 'off' : 'on')); });
    var btFollow = modeRow(btSec, T('Stay on the path'), [['auto', T('Auto')], ['on', T('Always')], ['off', T('Never')]], '236px',
      function (k) { btCommand('follow ' + k); },
      T('A bot replays your input, so it matches on its own. A different gun runs at a different speed, so Auto keeps those takes on the recorded path. Always ignores anything placed in the way later.'), 13);
    var btDemoRow = actionGroup(btSec, '', [
      [T('Record demo'), function () { btCommand('demo'); }],
      [T('Save'), function () { btCommand('save scene'); }],
      [T('Load'), function () { btCommand('load scene'); }]
    ]);
    attachTip(btDemoRow.btns[0], T('Plays one pass from the start and records it to a demo file. Open it from the demo browser.'));
    attachTip(btDemoRow.btns[1], T('Saves every take on this map.'));
    attachTip(btDemoRow.btns[2], T('Loads the saved takes for this map.'));
    var btHelp = lbl(btSec, T('{0} starts and stops recording. {1} plays and stops.', K('takes.record', 'F6'), K('takes.play', 'F7')), S.hint, 12);
    btHelp.hittest = false; btHelp.style.marginTop = '8px';
    btHelp.style.whiteSpace = 'normal'; btHelp.style.width = '100%';
    btHelp.style.paddingLeft = UI.line.padH + 'px'; btHelp.style.paddingRight = UI.line.padH + 'px';

)CFJS"
R"CFJS(
    // ---- TAKES: one row per bot. Clicking a row opens that bot's own settings right under it. ----
    var btTakesSec = section(pageBots, T('TAKES'), true, true, 'bots.takes', '#6db3ffff');
    var btEmpty = lbl(btTakesSec, T('Nothing recorded yet. Press Record or {0}, do something, then press it again.', K('takes.record', 'F6')), S.hint, 13);
    btEmpty.hittest = false; btEmpty.style.whiteSpace = 'normal'; btEmpty.style.width = '100%'; btEmpty.style.marginTop = '6px';
    btEmpty.style.paddingLeft = UI.line.padH + 'px'; btEmpty.style.paddingRight = UI.line.padH + 'px';
    var btList = mk('Panel', btTakesSec); btList.hittest = false; btList.hittestchildren = true;
    btList.style.width = '100%'; btList.style.flowChildren = 'down';
    var btRows = [], btOpen = -1;
    function btTake() { var a = st && st.takes && st.takes.tracks; return (a && a[btOpen]) || null; }
    function btN() { return String(btOpen + 1); }
    function btPaintRow(r) {
      var open = r.__index === btOpen;
      r.style.backgroundColor = r.__hot ? '#ffffff14' : (open ? '#ffffff0a' : S.clear);
      r.__caret.style.transform = open ? 'rotateZ(0deg)' : 'rotateZ(-90deg)';
      r.__name.style.color = open ? S.accent : S.value;
    }
    function btApplyOpen() {
      btEdit.visible = btOpen >= 0;
      if (btOpen >= 0 && btRows[btOpen]) btList.MoveChildAfter(btEdit, btRows[btOpen]);
      for (var i = 0; i < btRows.length; i++) btPaintRow(btRows[i]);
    }
    function btRow(i) {
      if (btRows[i]) return btRows[i];
      var r = mk('Panel', btList); r.__index = i;
      r.style.width = '100%'; r.style.flowChildren = 'right'; r.style.borderRadius = '3px';
      r.style.marginTop = '4px'; r.style.padding = '6px ' + UI.line.padH + 'px';
      var caret = lbl(r, '▾', S.muted, 14); caret.hittest = false;
      caret.style.verticalAlign = 'center'; caret.style.marginRight = '8px'; caret.style.transformOrigin = '50% 50%';
      var col = mk('Panel', r); col.hittest = false;
      col.style.flowChildren = 'down'; col.style.width = 'fill-parent-flow(1.0)';
      var name = lbl(col, '', S.value, 14); name.hittest = false;
      name.style.whiteSpace = 'normal'; name.style.width = '100%';
      var sub = lbl(col, '', S.hint, 12); sub.hittest = false;
      sub.style.whiteSpace = 'normal'; sub.style.width = '100%'; sub.style.marginTop = '1px';
      r.__caret = caret; r.__name = name; r.__sub = sub;
      hoverable(r, btPaintRow);
      r.SetPanelEvent('onactivate', function () { btOpen = btOpen === i ? -1 : i; btApplyOpen(); });
      btRows[i] = r;
      return r;
    }
    var btEdit = mk('Panel', btList); btEdit.hittest = false; btEdit.hittestchildren = true;
    btEdit.style.width = '100%'; btEdit.style.flowChildren = 'down';
    btEdit.style.paddingBottom = '6px'; btEdit.style.borderBottom = '1px solid #ffffff14';
    btEdit.visible = false;
    var btOn = togglePill(btEdit, T('Plays'), T('Turn a take off to leave it out without deleting it.'),
      function () { var t = btTake(); if (t) btCommand('enable ' + btN() + ' ' + (t.enabled ? 'off' : 'on')); });
    var btHold = togglePill(btEdit, T('Never fires'), T('Plays everything else in the take, without shooting or throwing.'),
      function () { var t = btTake(); if (t) btCommand('holdfire ' + btN() + ' ' + (t.holdFire ? 'off' : 'on')); });
    var btTeam = modeRow(btEdit, T('Team'), [['t', 'T'], ['ct', 'CT']], '236px',
      function (k) { btCommand('team ' + btN() + ' ' + k); }, T('Which side this bot plays on.'), 13);
    var btDelay = sliderRow(btEdit, { label: T('Starts after'), min: 0, max: 30, decimals: 1, unit: 's',
      tip: T('Holds this take back, so it lines up with the others.'),
      onChange: function (v) { btCommand('delay ' + btN() + ' ' + v); } });
    var btAgent = dropdownRow(btEdit, T('Player model'), btAgents(), function (v) { btCommand('model ' + btN() + ' ' + v); });
    attachTip(btAgent.row, T('The character this bot appears as. The performance stays the same.'));
    // One row per gun used in the take: the row's name is the recorded gun, its value what the bot holds.
    var btSwapItems = [['-', T('Same gun')]];
    for (var bwi = 0; bwi < BT_WEAPONS.length; bwi++) btSwapItems.push([BT_WEAPONS[bwi][0], BT_WEAPONS[bwi][1]]);
    var btGuns = mk('Panel', btEdit); btGuns.hittest = false; btGuns.hittestchildren = true;
    btGuns.style.width = '100%'; btGuns.style.flowChildren = 'down';
    var btGunRows = [], btGunClasses = [];
    function btGunRow(i) {
      if (btGunRows[i]) return btGunRows[i];
      var row = dropdownRow(btGuns, T('Gun'), btSwapItems, function (v) {
        if (btGunClasses[i]) btCommand('weapon ' + btN() + ' ' + btGunClasses[i] + ' ' + v);
      });
      attachTip(row.row, T('The gun this bot holds whenever you held this one. It still fires at its own speed, from the next time you press Play.'));
      btGunRows[i] = row;
      return row;
    }
    var btReadout = lbl(btEdit, '', S.hint, 12); btReadout.hittest = false;
    btReadout.style.whiteSpace = 'normal'; btReadout.style.width = '100%'; btReadout.style.marginTop = '8px';
    btReadout.style.paddingLeft = UI.line.padH + 'px'; btReadout.style.paddingRight = UI.line.padH + 'px';
    var btDelete = actionGroup(btEdit, '', [[T('Delete this take'), function () { btCommand('delete ' + btN()); btOpen = -1; btApplyOpen(); }]]);

)CFJS"
R"CFJS(
    // ---- OBJECTS ----
    var btObjSec = section(pageBots, T('SOLID OBJECTS'), true, false, 'bots.objects', '#7fd18bff');
    var btObjNote = lbl(btObjSec, T('Place props from ENVIRONMENT > Scene objects, then turn on Solid for each one. You and the bots collide with solid props, so you can jump on them.'), S.hint, 13);
    btObjNote.hittest = false; btObjNote.style.whiteSpace = 'normal'; btObjNote.style.width = '100%'; btObjNote.style.marginTop = '6px';
    btObjNote.style.paddingLeft = UI.line.padH + 'px'; btObjNote.style.paddingRight = UI.line.padH + 'px';
    var btSolidNew = togglePill(btObjSec, T('New props are solid'), T('Anything you place from now on starts solid.'),
      function () { cmd('mirv_filmmaker props solidnew ' + (st && st.takes && st.takes.solidNew ? 'off' : 'on')); });
    var btObjActions = actionGroup(btObjSec, '', [
      [T('Place props'), function () { activeTab = 'environment'; applyTabs(); }],
      [T('Make all solid'), function () { cmd('mirv_filmmaker props solidall on'); }],
      [T('Make none solid'), function () { cmd('mirv_filmmaker props solidall off'); }]
    ]);

    // ---- GUN RANDOMNESS ----
    var btGunSec = section(pageBots, T('GUN RANDOMNESS'), true, false, 'bots.gun', '#ffc55cff');
    var btNoSpread = togglePill(btGunSec, T('No random spread'),
      T('Every bullet goes exactly where the gun points. Recoil itself is unchanged. Helps a recorded frag land the same way on every loop.'),
      function () { btCommand('nospread ' + (st && st.takes && st.takes.noSpread ? 'off' : 'on')); });
    var btRecoilRandom = sliderRow(btGunSec, { label: T('Recoil randomness'), min: 0, max: 100, decimals: 0, unit: '%',
      tip: T('How far each shot strays from the gun' + "'" + 's recoil pattern. 0% follows the pattern exactly. The strength of the recoil and every other recoil setting stay as they are.'),
      onChange: function (v) { btCommand('recoilrandom ' + v); } });

    function btFmtTime(sec) {
      var s = Math.max(0, Math.floor(sec)), m = Math.floor(s / 60); s = s % 60;
      return m + ':' + (s < 10 ? '0' : '') + s;
    }
    renderHooks.push(function (st) {
      var k = (st && st.takes) || null;
      var usable = !!(k && k.available);
      if (tabBtns.bots && tabBtns.bots.visible !== usable) tabBtns.bots.visible = usable;
      if (!usable && activeTab === 'bots') { activeTab = 'camera'; applyTabs(); }
      if (!k) return;
      var tracks = k.tracks || [];
      var line = k.message || '';
      if (k.recording) line = T('Recording {0}. Press Stop or {1} when you are done.', btFmtTime(k.recSeconds), K('takes.record', 'F6'));
      else if (k.phase === 'playing') line = k.demoRecording
        ? T('Playing {0} of {1}, recording a demo.', btFmtTime(k.sceneSeconds), btFmtTime(k.lengthSeconds))
        : T('Playing {0} of {1}.', btFmtTime(k.sceneSeconds), btFmtTime(k.lengthSeconds));
      else if (k.phase === 'adding bots') line = T('Adding bots...');
      else if (k.phase === 'spawning') line = T('Getting ready...');
      else if (!line) line = tracks.length ? T('Ready. Stand where you want to be, then press Play.') : T('Go to the spot, then press Record.');
      if (btStatus.text !== line) btStatus.text = line;
      setBtnOn(btRecBtn, !!k.recording, '#ff5c5cff');
      setBtnOn(btPlayBtn, k.phase === 'playing' && !k.recording);
      setBtnDisabled(btRecBtn, !!k.recording);
      setBtnDisabled(btStopBtn, !k.recording && (k.phase === 'idle' || k.phase === 'finished'));
      setBtnDisabled(btPlayBtn, !tracks.length || !!k.recording);
      btLoop.set(!!k.loop);
      btFollow.sync(k.follow || 'auto');
      btSolidNew.set(!!k.solidNew);
      btNoSpread.set(!!k.noSpread);
      btRecoilRandom.sync(typeof k.recoilRandom === 'number' ? k.recoilRandom : 100);
      btSec.__note(k.recording ? T('Recording') : (k.phase === 'playing' ? T('Playing') : (tracks.length ? T('Ready') : T('Nothing yet'))));
      btTakesSec.__note(tracks.length ? TN(tracks.length, '{0} take', '{0} takes') : T('None'));
      btObjSec.__note(T('{0} solid', k.solidCount || 0));
      btGunSec.__note(k.noSpread ? T('No spread') : (k.recoilRandom < 100 ? T('Recoil {0}%', k.recoilRandom) : T('Normal##recoil')));

      btEmpty.visible = !tracks.length;
      if (btOpen >= tracks.length) { btOpen = -1; btApplyOpen(); }
      var rowsBefore = btRows.length;
      for (var i = 0; i < Math.max(tracks.length, btRows.length); i++) {
        var r = btRow(i), tk = tracks[i];
        var showRow = !!tk;
        if (r.visible !== showRow) r.visible = showRow;
        if (!tk) continue;
        var plays = [];
        for (var gi = 0; gi < (tk.weapons || []).length; gi++) {
          var gun = btWeaponName(tk.weapons[gi]);
          for (var sw = 0; sw < (tk.swaps || []).length; sw++) {
            if (tk.swaps[sw][0] === tk.weapons[gi]) gun = T('{0} as {1}', gun, btWeaponName(tk.swaps[sw][1]));
          }
          plays.push(gun);
        }
        var summary = (tk.team === 2 ? 'T' : 'CT') + ', ' + T('{0} seconds', tk.seconds.toFixed(1)) + (plays.length ? ', ' + plays.join(', ') : '');
        if (!tk.enabled) summary += ', ' + T('off');
        else if (tk.holdFire) summary += ', ' + T('never fires');
        if (r.__name.text !== tk.name) r.__name.text = tk.name;
        if (r.__sub.text !== summary) r.__sub.text = summary;
        var dim = tk.enabled ? '1' : '0.55';
        if (r.__dim !== dim) { r.__dim = dim; r.style.opacity = dim; }
      }
      if (btRows.length !== rowsBefore) btApplyOpen();
      var t = tracks[btOpen];
      if (!t) return;
      var read = '';
      if (t.hasBot) read = T('Stays within {0} units of the recording.', t.maxDrift.toFixed(2)) + ' ' + TN(t.deaths, 'Died {0} time.', 'Died {0} times.');
      if (btReadout.text !== read) btReadout.text = read;
      btReadout.visible = !!read;
      btOn.set(!!t.enabled);
      btHold.set(!!t.holdFire);
      btTeam.sync(t.team === 2 ? 't' : 'ct');
      btDelay.sync(t.delay || 0);
      btAgent.sync(t.agent || '-');
      var weapons = t.weapons || [];
      btGunClasses = weapons;
      for (var w = 0; w < Math.max(weapons.length, btGunRows.length); w++) {
        var gr = btGunRow(w), cls = weapons[w];
        var showGun = !!cls;
        if (gr.row.visible !== showGun) gr.row.visible = showGun;
        if (!cls) continue;
        var cap = btWeaponName(cls);
        if (gr.label.text !== cap) gr.label.text = cap;
        var swapped = '-';
        for (var si = 0; si < (t.swaps || []).length; si++) if (t.swaps[si][0] === cls) swapped = t.swaps[si][1];
        gr.sync(swapped);
      }
    });
)CFJS"
