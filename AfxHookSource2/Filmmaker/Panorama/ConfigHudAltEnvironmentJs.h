#pragma once

// ENVIRONMENT > World > MAP (Docs/features/alternative-environment.md): the open demo plays on
// another map -- a Workshop night or rain remake, another installed map -- with every player,
// weapon and effect from the demo on top. Backend: Filmmaker/AltEnvironment/AltEnvironment.cpp
// (`mirv_filmmaker altenv`, state in st.altenv).
//
// Two parts. The CARD in the inspector holds the choice, the alignment numbers, AUTO ALIGN and the
// play button. The MAP VIEW takes over the preview area while it is open: the list of maps on the
// left, and both maps seen from above on the right -- the replacement map in grey (lighter is
// higher), the demo's own map as an orange layer of its walls and the paths its players walked,
// plus where the players are now. Dragging the orange layer moves the demo on the new map,
// right-dragging turns it, the wheel zooms. Panorama gets no mouse-move event in this HUD, so the
// drags run off the OS-cursor pipe (st.mx / st.my / st.lmb / st.rmb), as the sliders' do.
//
// The pictures are PNGs the hook draws and serves on 127.0.0.1 (Platform/LocalImageServer.h);
// st.altenv.demoPic / mapPic give each one's URL and the world rectangle it covers.

R"CFJS(
    // ===================== MAP (alternative environment) =====================
    // Not a render pass, so it carries the panel's blue rather than the environment teal.
    var aeSec = section(pageEnvironment, 'MAP', true, false, 'env.map', S.accent);
    function aeCmd(t) { cmd('mirv_filmmaker altenv ' + t); }
    var ae = { st: {}, draft: { x: 0, y: 0, z: 0, yaw: 0 }, dragging: false, lastSend: 0 };
    function aeFixed(v, d) { v = +v || 0; return Math.abs(v) < 0.0005 ? '0' : v.toFixed(d); }
    function aeWrapLabel(l) { l.style.whiteSpace = 'normal'; l.style.width = '100%'; return l; }
    // The render hooks run every frame while the pointer moves; a label is only written when its text
    // changes, so it is not laid out again for nothing.
    function aeText(l, t) { if (l.text !== t) l.text = t; }
    function aeStyle(p, key, v) { var c = p.__st || (p.__st = {}); if (c[key] !== v) { c[key] = v; p.style[key] = v; } }
    // A row of equal buttons across the card. Never right-wrap: btn() centres itself vertically,
    // and in a wrapping row Panorama centres each button in the whole row's height, so a wrapped
    // second line was drawn over the first.
    function aeBtnRow(parent, items) {
      var r = mk('Panel', parent); r.hittest = false; r.hittestchildren = true;
      r.style.flowChildren = 'right'; r.style.width = '100%'; r.style.marginTop = '6px';
      r.style.paddingLeft = UI.line.padH + 'px'; r.style.paddingRight = UI.line.padH + 'px';
      var out = [];
      for (var i = 0; i < items.length; i++) {
        var b = btn(r, items[i][0], items[i][1], items[i][2] || S.label);
        b.style.width = 'fill-parent-flow(1.0)';
        b.style.marginRight = (i === items.length - 1 ? 0 : 6) + 'px';
        b.__lbl.style.horizontalAlign = 'center';
        out.push(b);
      }
      endTileLine(parent);
      return out;
    }
    // The card's Reset: the alignment on the map it plays on goes back to no offset. Going back to
    // the demo's own map is BACK TO ..., never a reset.
    var aeResetSpec = { cmds: ['mirv_filmmaker altenv reset'] };
    var aeHeadReset = aeSec.__headerReset(aeResetSpec, 'env.map');

    var aeIntro = aeWrapLabel(lbl(aeSec, '', S.hint, 12));
    aeIntro.hittest = false; aeIntro.style.marginTop = '2px';
    aeIntro.style.paddingLeft = UI.line.padH + 'px'; aeIntro.style.paddingRight = UI.line.padH + 'px';
    aeIntro.text = 'Play this demo on another map, such as a night or rain version from the Workshop. '
      + 'Players, weapons, grenades and effects stay where the demo recorded them; the new map is moved under them.';
    endTileLine(aeSec);

    // ---- which map ----
    var aeMapG = aeSec.__group('Plays on');
    var aeMapRow = row(aeMapG); aeMapRow.style.flowChildren = 'right';
    aeMapRow.style.paddingLeft = UI.line.padH + 'px'; aeMapRow.style.paddingRight = UI.line.padH + 'px';
    var aeMapCol = mk('Panel', aeMapRow); aeMapCol.hittest = false;
    aeMapCol.style.flowChildren = 'down'; aeMapCol.style.width = 'fill-parent-flow(1.0)';
    aeMapCol.style.verticalAlign = 'center';
    var aeMapName = aeWrapLabel(lbl(aeMapCol, '', S.value, 15)); aeMapName.style.fontWeight = 'bold';
    var aeMapSub = aeWrapLabel(lbl(aeMapCol, '', S.muted, 12)); aeMapSub.style.marginTop = '2px';
    var aeChoose = btn(aeMapRow, 'CHOOSE MAP', function () { aeCmd('view on'); aeCmd('maps'); }, S.label);
    aeChoose.style.marginRight = '0px'; aeChoose.style.marginLeft = '10px';
    attachTip(aeChoose, 'Opens the map view: the maps installed and from the Workshop on the left, both maps seen from above on the right.');

    // ---- alignment ----
    // The heading lives inside the group, so the whole group hides until a map is picked.
    var aeAlignG = aeSec.__group('');
    subHead(aeAlignG, 'Line them up');
    var aeFit = aeWrapLabel(lbl(aeAlignG, '', S.label, 13));
    aeFit.hittest = false; aeFit.style.marginTop = '4px';
    aeFit.style.paddingLeft = UI.line.padH + 'px'; aeFit.style.paddingRight = UI.line.padH + 'px';
    endTileLine(aeAlignG);
    function aeSendDraft() {
      var d = ae.draft;
      aeCmd('align ' + aeFixed(d.x, 1) + ' ' + aeFixed(d.y, 1) + ' ' + aeFixed(d.z, 1) + ' ' + aeFixed(d.yaw, 2) + ' quiet');
    }
    function aeSlider(label, key, min, max, decimals, unit, tip) {
      return sliderRow(aeAlignG, { label: label, min: min, max: max, decimals: decimals, unit: unit, tip: tip,
        onChange: function (v) { ae.draft[key] = v; ae.dragging = true; ae.lastSend = Date.now(); aeSendDraft(); } });
    }
    var aeX = aeSlider('Move east', 'x', -8192, 8192, 0, 'u', 'Moves the demo along the new map\'s x axis, in game units.');
    var aeY = aeSlider('Move north', 'y', -8192, 8192, 0, 'u', 'Moves the demo along the new map\'s y axis, in game units.');
    var aeZ = aeSlider('Move up', 'z', -2048, 2048, 0, 'u', 'Raises or lowers the demo on the new map. AUTO ALIGN sets it from the two floors.');
    var aeYaw = aeSlider('Turn', 'yaw', -180, 180, 1, '°', 'Turns the demo on the new map, in degrees. Positive turns it counter-clockwise seen from above.');
    var aeToolsA = aeBtnRow(aeAlignG, [
      ['AUTO ALIGN', function () { aeCmd('auto'); }, S.accent],
      ['SHOW MAP VIEW', function () { aeCmd(ae.st.view ? 'view off' : 'view on'); }]]);
    var aeAuto = aeToolsA[0], aeViewBtn = aeToolsA[1];
    attachTip(aeAuto, 'Finds where the demo\'s map fits on the new one, from both maps\' floors and the paths the players walked. A remake of the same map lines up with no offset.');
    aeBtnRow(aeAlignG, [
      ['TURN −90°', function () { aeCmd('turn -90'); }],
      ['TURN +90°', function () { aeCmd('turn 90'); }]]);
    // The demo map's own doors, physics props and breakables (AltEnvironmentProps.h). Not part of
    // the copy: it applies at once.
    var aeProps = togglePill(aeAlignG, 'Demo map props',
      'Shows the doors, physics props and breakables the demo recorded on its own map.',
      function () { aeCmd('props toggle'); },
      'The demo recorded its own map\'s doors, physics props and breakables, so on another map they float where they were. '
      + 'Off hides them. Anything whose model is in the demo map\'s own files can only draw as ERROR, so it stays hidden either way.');

    // ---- play ----
    var aePlayG = aeSec.__group('');
    var aeStatus = aeWrapLabel(lbl(aePlayG, '', S.hint, 12));
    aeStatus.hittest = false; aeStatus.style.marginTop = '6px';
    aeStatus.style.paddingLeft = UI.line.padH + 'px'; aeStatus.style.paddingRight = UI.line.padH + 'px';
    endTileLine(aePlayG);
    var aePlay = wideBtn(aePlayG, 'PLAY ON THIS MAP', function () { aeCmd('apply'); }, S.accent);
    aePlay.__lbl.style.whiteSpace = 'normal'; aePlay.__lbl.style.textAlign = 'center';
    attachTip(aePlay, 'Reopens the demo on the chosen map at the same moment. The map loads like any level, so nothing stutters while the demo plays.');
    var aeBack = wideBtn(aePlayG, 'BACK TO ITS OWN MAP', function () { aeCmd('off'); }, S.label);
    aeBack.__lbl.style.whiteSpace = 'normal'; aeBack.__lbl.style.textAlign = 'center';
)CFJS"
R"CFJS(
    // ===================== the MAP VIEW over the preview area =====================
    var aeView = $.CreatePanel('Panel', root, 'MvmAltEnvView'); aeView.hittest = true; aeView.hittestchildren = true;
    aeView.visible = false; aeView.style.zIndex = '85';
    aeView.style.backgroundColor = '#0b0e12f2'; aeView.style.border = '1px solid #ffffff1f';
    aeView.style.borderRadius = '4px'; aeView.style.overflow = 'clip';
    aeView.SetPanelEvent('onactivate', function () {}); // a press on the view never reaches the game
    var AE_LIST_W = 300, AE_HEAD_H = 46, AE_ROW_H = 44;

    var aeHead = mk('Panel', aeView); aeHead.hittest = false; aeHead.hittestchildren = true;
    aeHead.style.width = '100%'; aeHead.style.height = AE_HEAD_H + 'px'; aeHead.style.flowChildren = 'right';
    aeHead.style.paddingLeft = '14px'; aeHead.style.paddingRight = '8px';
    aeHead.style.borderBottom = '1px solid #ffffff14';
    var aeTitle = lbl(aeHead, 'MAP VIEW', S.value, 15); aeTitle.hittest = false;
    aeTitle.style.fontWeight = 'bold'; aeTitle.style.letterSpacing = '1.2px'; aeTitle.style.verticalAlign = 'center';
    aeTitle.style.whiteSpace = 'nowrap';
    var aeSubtitle = lbl(aeHead, '', S.muted, 12); aeSubtitle.hittest = false;
    aeSubtitle.style.verticalAlign = 'center'; aeSubtitle.style.marginLeft = '12px';
    aeSubtitle.style.width = 'fill-parent-flow(1.0)'; aeSubtitle.style.whiteSpace = 'normal';
    var aeClose = btn(aeHead, 'CLOSE', function () { aeCmd('view off'); }, S.label);
    aeClose.style.marginRight = '0px';

    // ---- the list of maps (left) ----
    var aeList = mk('Panel', aeView); aeList.hittest = false; aeList.hittestchildren = true;
    aeList.style.width = AE_LIST_W + 'px'; aeList.style.flowChildren = 'down';
    aeList.style.position = '0px ' + AE_HEAD_H + 'px 0px';
    aeList.style.padding = '8px'; aeList.style.borderRight = '1px solid #ffffff14';
    var aeTabs = mk('Panel', aeList); aeTabs.hittest = false; aeTabs.hittestchildren = true;
    aeTabs.style.flowChildren = 'right'; aeTabs.style.width = '100%'; aeTabs.style.marginBottom = '6px';
    var aeKind = 'workshop', aePage = 0, aePerPage = 8;
    var aeTabWs = btn(aeTabs, 'WORKSHOP', function () { aeKind = 'workshop'; aePage = 0; aeListSig = ''; }, S.label);
    var aeTabIn = btn(aeTabs, 'INSTALLED', function () { aeKind = 'installed'; aePage = 0; aeListSig = ''; }, S.label);
    aeTabIn.style.marginRight = '0px';
    var aeOwn = wideBtn(aeList, '', function () { aeCmd('pick none'); }, S.label);
    aeOwn.__lbl.style.whiteSpace = 'normal'; aeOwn.style.marginBottom = '6px';
    var aeRows = mk('Panel', aeList); aeRows.hittest = false; aeRows.hittestchildren = true;
    aeRows.style.flowChildren = 'down'; aeRows.style.width = '100%';
    var aePager = mk('Panel', aeList); aePager.hittest = false; aePager.hittestchildren = true;
    aePager.style.flowChildren = 'right'; aePager.style.width = '100%'; aePager.style.marginTop = '6px';
    var aePrev = btn(aePager, '◀', function () { if (aePage > 0) { aePage--; aeListSig = ''; } }, S.label);
    var aePageLbl = lbl(aePager, '', S.muted, 12); aePageLbl.hittest = false;
    aePageLbl.style.verticalAlign = 'center'; aePageLbl.style.width = 'fill-parent-flow(1.0)'; aePageLbl.style.textAlign = 'center';
    var aeNext = btn(aePager, '▶', function () { aePage++; aeListSig = ''; }, S.label);
    aeNext.style.marginRight = '0px';
    var aeListSig = '';

    function aeMapsOf(a) {
      var out = [], maps = a.maps || [];
      for (var i = 0; i < maps.length; i++) {
        if ((aeKind === 'workshop') === !!maps[i].a && maps[i].m !== a.demoMap) out.push(maps[i]);
      }
      return out;
    }
    function aeRenderList(a, listH) {
      aePerPage = Math.max(3, Math.floor((listH - 150) / (AE_ROW_H + 4)));
      var maps = aeMapsOf(a), pages = Math.max(1, Math.ceil(maps.length / aePerPage));
      if (aePage >= pages) aePage = pages - 1;
      var sig = aeKind + '|' + aePage + '|' + aePerPage + '|' + maps.length + '|' + (a.pick || '') + '|' + (a.pickAddon || '') + '|' + (a.mapsBusy ? 1 : 0);
      setBtnOn(aeTabWs, aeKind === 'workshop'); setBtnOn(aeTabIn, aeKind === 'installed');
      aeOwn.__lbl.text = (a.demoTitle || a.demoMap || 'Its own map') + ' (the demo\'s own map)';
      setBtnOn(aeOwn, !a.pick);
      if (sig === aeListSig) return;
      aeListSig = sig;
      aeRows.RemoveAndDeleteChildren();
      if (a.mapsBusy && !maps.length) {
        var wait = aeWrapLabel(lbl(aeRows, 'Looking for maps...', S.muted, 12)); wait.hittest = false; wait.style.margin = '6px';
      } else if (!maps.length) {
        var none = aeWrapLabel(lbl(aeRows, aeKind === 'workshop' ? 'No Workshop maps are installed. Subscribe to one in the Workshop and it shows up here.' : 'No other maps found.', S.muted, 12));
        none.hittest = false; none.style.margin = '6px';
      }
      for (var i = aePage * aePerPage; i < Math.min(maps.length, (aePage + 1) * aePerPage); i++) (function (m) {
        var r = mk('Panel', aeRows); r.hittest = true;
        r.style.width = '100%'; r.style.minHeight = AE_ROW_H + 'px'; r.style.flowChildren = 'down';
        r.style.paddingLeft = '8px'; r.style.paddingRight = '8px'; r.style.paddingTop = '5px'; r.style.paddingBottom = '5px';
        r.style.marginBottom = '4px'; r.style.borderRadius = '3px';
        // Two Workshop items can hold a map of the same name (Nightly and Rainy Cache): the row is the
        // map in its item, and so is the pick.
        var on = m.m === a.pick && (!m.a || !a.pickAddon || m.a === a.pickAddon);
        var t = aeWrapLabel(lbl(r, m.t, on ? S.accent : S.value, 13)); t.hittest = false; t.style.fontWeight = 'bold';
        var sub = m.m + (m.tiny ? ' · a placeholder, not a playable map' : '');
        var s2 = aeWrapLabel(lbl(r, sub, m.tiny ? '#ffb070ff' : S.muted, 11)); s2.hittest = false; s2.style.marginTop = '1px';
        function paint() { r.style.backgroundColor = on ? '#80bdff22' : (r.__hot ? '#ffffff12' : '#ffffff06'); }
        r.SetPanelEvent('onmouseover', function () { r.__hot = true; paint(); });
        r.SetPanelEvent('onmouseout', function () { r.__hot = false; paint(); });
        r.SetPanelEvent('onactivate', function () { aeCmd('pick ' + m.m + (m.a ? ' ' + m.a : '')); });
        paint();
      })(maps[i]);
      aePageLbl.text = maps.length ? ('Page ' + (aePage + 1) + ' of ' + pages + ' · ' + maps.length + ' maps') : '';
      setBtnDisabled(aePrev, aePage <= 0); setBtnDisabled(aeNext, aePage >= pages - 1);
    }
)CFJS"
R"CFJS(
    // ---- the two maps from above (right) ----
    var aeCanvas = $.CreatePanel('Panel', aeView, 'MvmAltEnvCanvas'); aeCanvas.hittest = false; aeCanvas.hittestchildren = false;
    aeCanvas.style.overflow = 'clip';
    var aeMapImg = $.CreatePanel('Image', aeCanvas, 'MvmAltEnvMap', { scaling: 'stretch' }); aeMapImg.hittest = false;
    var aeDemoLayer = $.CreatePanel('Panel', aeCanvas, 'MvmAltEnvDemo'); aeDemoLayer.hittest = false;
    aeDemoLayer.style.transformOrigin = '50% 50%';
    // Two pictures of the demo layer, so a recolour never blanks it: setting a new image on a panel
    // clears it until the download lands. The next picture loads into the hidden one and takes over
    // on ImageLoaded. One load at a time; a picture published meanwhile waits, and any between are
    // skipped.
    var aeDemoImgs = ['A', 'B'].map(function (k, i) {
      var im = $.CreatePanel('Image', aeDemoLayer, 'MvmAltEnvDemo' + k, { scaling: 'stretch' }); im.hittest = false;
      im.style.width = '100%'; im.style.height = '100%'; im.style.opacity = '0';
      $.RegisterEventHandler('ImageLoaded', im, function () { aeDemoLoaded(i, true); });
      $.RegisterEventHandler('ImageFailedLoad', im, function () { aeDemoLoaded(i, false); });
      return im;
    });
    var aeDemoFront = -1, aeDemoWant = '', aeDemoLoading = null, aeDemoFailed = '';
    var aeDemoStats = { asked: 0, shown: 0, failed: 0, late: 0, waitSum: 0, waitMax: 0 };
    function aeDemoImage(url) { aeDemoWant = url; aeDemoNext(); }
    function aeDemoNext() {
      // A load that never reports (not seen, but it would freeze the layer) is shown anyway.
      if (aeDemoLoading && Date.now() - aeDemoLoading.at > 1500) { aeDemoStats.late++; aeDemoLoaded(aeDemoLoading.i, true); return; }
      if (aeDemoLoading || !aeDemoWant || aeDemoWant === aeDemoFailed) return;
      if (aeDemoFront >= 0 && aeDemoImgs[aeDemoFront].__src === aeDemoWant) return;
      var i = aeDemoFront === 0 ? 1 : 0;
      aeDemoLoading = { i: i, at: Date.now() };
      aeDemoStats.asked++;
      aeDemoImgs[i].__src = aeDemoWant;
      aeDemoImgs[i].SetImage(aeDemoWant);
    }
    function aeDemoLoaded(i, ok) {
      if (!aeDemoLoading || aeDemoLoading.i !== i) return;
      var wait = Date.now() - aeDemoLoading.at;
      aeDemoLoading = null;
      if (ok) {
        aeDemoImgs[i].style.opacity = '1';
        if (aeDemoFront >= 0 && aeDemoFront !== i) aeDemoImgs[aeDemoFront].style.opacity = '0';
        aeDemoFront = i;
        aeDemoStats.shown++; aeDemoStats.waitSum += wait; aeDemoStats.waitMax = Math.max(aeDemoStats.waitMax, wait);
      } else {
        // The last drawing stays; the next one published is tried, this one is not asked for again.
        aeDemoStats.failed++;
        aeDemoFailed = aeDemoImgs[i].__src;
      }
      aeDemoNext();
    }
    // `mirv_filmmaker config eval $.Msg(JSON.stringify($.ConfigHudMapView.stats()))`: how the swaps go.
    $.ConfigHudMapView = { stats: function () { return aeDemoStats; } };
    var aeDots = mk('Panel', aeDemoLayer); aeDots.hittest = false; aeDots.style.width = '100%'; aeDots.style.height = '100%';
    var aeCanvasMsg = aeWrapLabel(lbl(aeCanvas, '', S.label, 14)); aeCanvasMsg.hittest = false;
    aeCanvasMsg.style.horizontalAlign = 'center'; aeCanvasMsg.style.verticalAlign = 'center';
    aeCanvasMsg.style.textAlign = 'center'; aeCanvasMsg.style.width = '70%';
    var aeLegend = aeWrapLabel(lbl(aeCanvas, '', S.label, 12)); aeLegend.hittest = false;
    aeLegend.style.verticalAlign = 'bottom'; aeLegend.style.marginBottom = '8px'; aeLegend.style.marginLeft = '10px';
    aeLegend.style.width = '80%'; aeLegend.style.padding = '6px 8px'; aeLegend.style.borderRadius = '3px';
    aeLegend.style.backgroundColor = '#000000a0';
    // Top left: where the walked paths land under the alignment being edited (C++ colours the demo
    // layer the same way, AltEnvironmentMath.h LandingPicture), then how to drag.
    var AE_LANDINGS = [['on', 'on the map', '#4ade80ff'], ['floating', 'floating', '#facc15ff'], ['under', 'under it', '#a855f7ff'], ['outside', 'outside it', '#f85252ff']];
    var aeTop = mk('Panel', aeCanvas); aeTop.hittest = false; aeTop.style.flowChildren = 'down';
    aeTop.style.marginTop = '8px'; aeTop.style.marginLeft = '10px';
    var aeLand = mk('Panel', aeTop); aeLand.hittest = false; aeLand.style.flowChildren = 'right';
    aeLand.style.padding = '5px 8px'; aeLand.style.borderRadius = '3px'; aeLand.style.backgroundColor = '#000000b8';
    aeLand.style.marginBottom = '4px';
    var aeLandParts = {};
    AE_LANDINGS.forEach(function (k, i) {
      var sw = mk('Panel', aeLand); sw.hittest = false; sw.style.width = '10px'; sw.style.height = '10px';
      sw.style.borderRadius = '2px'; sw.style.backgroundColor = k[2]; sw.style.verticalAlign = 'center';
      sw.style.marginLeft = (i ? 12 : 0) + 'px'; sw.style.marginRight = '5px';
      var t = lbl(aeLand, '', S.value, 12); t.hittest = false; t.style.verticalAlign = 'center'; t.style.whiteSpace = 'nowrap';
      aeLandParts[k[0]] = t;
    });
    var aeHint = lbl(aeTop, 'Drag to move · right-drag to turn (Shift: 15° steps) · wheel to zoom · Ctrl-drag to pan', S.hint, 12); aeHint.hittest = false;
    aeHint.style.padding = '4px 8px'; aeHint.style.borderRadius = '3px'; aeHint.style.backgroundColor = '#000000a0';

    // Map coordinates <-> canvas pixels. zoom 1 fits the replacement map (or the demo's own map
    // when none is picked); pan is in canvas pixels.
    var aeCam = { zoom: 1, panX: 0, panY: 0, k: 1, x0: 0, y1: 0, cw: 0, ch: 0, key: '' };
    function aeToCanvas(x, y) { return { x: (x - aeCam.x0) * aeCam.k + aeCam.panX, y: (aeCam.y1 - y) * aeCam.k + aeCam.panY }; }
    function aeRot(x, y, deg) { var r = deg * Math.PI / 180, c = Math.cos(r), s = Math.sin(r); return { x: c * x - s * y, y: s * x + c * y }; }
    function aeDemoToMap(d, x, y) { var r = aeRot(x, y, d.yaw); return { x: r.x + d.x, y: r.y + d.y }; }
    // The pivot keeps its place on the map while the demo turns (AltEnvironmentMath.h RotateAbout).
    function aeRotateAbout(d, px, py, deg) {
      var anchored = aeDemoToMap(d, px, py), yaw = d.yaw + deg;
      // One-step wrap to (-180, 180], as NormalizeYaw does (no 360-step loop: angle-wrap-loop-tests).
      yaw = isFinite(yaw) ? yaw % 360 : 0;
      if (yaw <= -180) yaw += 360;
      if (yaw > 180) yaw -= 360;
      var turned = aeRot(px, py, yaw);
      return { x: anchored.x - turned.x, y: anchored.y - turned.y, z: d.z, yaw: yaw };
    }
    // x0 / y1 are the map point at the canvas's top-left corner when it fits (zoom 1, no pan).
    function aeFrame(a, cw, ch) {
      var pic = a.mapPic || a.demoPic;
      if (!pic) return false;
      var key = (a.mapPic ? a.mapPic.url : '') + '|' + (a.demoPic ? a.demoPic.url : '') + '|' + cw + 'x' + ch;
      if (key !== aeCam.key) { aeCam.key = key; aeCam.zoom = 1; aeCam.panX = 0; aeCam.panY = 0; }
      var fit = Math.min(cw / pic.w, ch / pic.h) * 0.94;
      aeCam.x0 = pic.x0 - (cw / fit - pic.w) / 2;
      aeCam.y1 = pic.y0 + pic.h + (ch / fit - pic.h) / 2;
      aeCam.fit = fit; aeCam.k = fit * aeCam.zoom; aeCam.cw = cw; aeCam.ch = ch;
      return true;
    }
    // Zoom about a canvas point: the map point under it stays under it.
    function aeZoomAt(cx, cy, factor) {
      var at = { x: aeCam.x0 + (cx - aeCam.panX) / aeCam.k, y: aeCam.y1 - (cy - aeCam.panY) / aeCam.k };
      var z = Math.max(1, Math.min(16, aeCam.zoom * factor));
      if (z === aeCam.zoom) return;
      aeCam.zoom = z; aeCam.k = aeCam.fit * z;
      aeCam.panX = z === 1 ? 0 : cx - (at.x - aeCam.x0) * aeCam.k;
      aeCam.panY = z === 1 ? 0 : cy - (aeCam.y1 - at.y) * aeCam.k;
    }
    function aeCanvasRect() {
      try {
        if (!aeView.visible) return null;
        var p = aeCanvas.GetPositionWithinWindow();
        var w = aeCanvas.actuallayoutwidth || 0, h = aeCanvas.actuallayoutheight || 0;
        if (!p || !(w > 0) || !(h > 0) || w > 1e30) return null;
        // Cursor pixels per layout pixel, from the size the canvas was given and the size it has:
        // the same units the sliders' drags use (cursor deltas against actuallayoutwidth).
        var lw = aeCanvas.__w || w, lh = aeCanvas.__h || h;
        return { x: p.x, y: p.y, w: w, h: h, sx: w / lw, sy: h / lh };
      } catch (e) { return null; }
    }
    // The wheel over the map zooms it (ConfigHudJs.h api.wheel offers each notch here first).
    function aeWheel(n) {
      var r = aeCanvasRect();
      if (!r || cfgCursorX < r.x || cfgCursorY < r.y || cfgCursorX >= r.x + r.w || cfgCursorY >= r.y + r.h) return false;
      aeZoomAt((cfgCursorX - r.x) / r.sx, (cfgCursorY - r.y) / r.sy, n > 0 ? 1.25 : 0.8);
      return true;
    }
)CFJS"
R"CFJS(
    var aeDrag = null, aeLastL = false, aeLastR = false;
    function aePump(st, a) {
      var r = aeCanvasRect();
      var l = !!st.lmb && !!st.cursor, rb = !!st.rmb && !!st.cursor;
      var over = r && st.mx >= r.x && st.my >= r.y && st.mx < r.x + r.w && st.my < r.y + r.h;
      var canDrag = !!(a.demoPic && a.pick && a.mapPic);
      // Ctrl + drag pans a zoomed view.
      if (!aeDrag && over && l && !aeLastL && st.ctrl && aeCam.zoom > 1) {
        aeDrag = { pan: true, mx: st.mx, my: st.my, px: aeCam.panX, py: aeCam.panY };
      }
      if (!aeDrag && canDrag && over && ((l && !aeLastL) || (rb && !aeLastR))) {
        var piv = a.demoPic ? { x: a.demoPic.x0 + a.demoPic.w / 2, y: a.demoPic.y0 + a.demoPic.h / 2 } : { x: 0, y: 0 };
        var pc = aeToCanvas(aeDemoToMap(ae.draft, piv.x, piv.y).x, aeDemoToMap(ae.draft, piv.x, piv.y).y);
        aeDrag = { turn: !l, mx: st.mx, my: st.my, base: { x: ae.draft.x, y: ae.draft.y, z: ae.draft.z, yaw: ae.draft.yaw },
                   pivot: piv, pcx: r.x + pc.x * r.sx, pcy: r.y + pc.y * r.sy };
        aeDrag.a0 = Math.atan2(st.my - aeDrag.pcy, st.mx - aeDrag.pcx);
      }
      aeLastL = l; aeLastR = rb;
      if (!aeDrag) return;
      if (aeDrag.pan) {
        if (!l) { aeDrag = null; return; }
        aeCam.panX = aeDrag.px + (st.mx - aeDrag.mx) / r.sx;
        aeCam.panY = aeDrag.py + (st.my - aeDrag.my) / r.sy;
        return;
      }
      if (aeDrag.turn ? !rb : !l) { aeDrag = null; aeSendDraft(); ae.dragging = true; ae.lastSend = Date.now(); return; }
      if (aeDrag.turn) {
        var a1 = Math.atan2(st.my - aeDrag.pcy, st.mx - aeDrag.pcx);
        // Screen y runs down, map y up: a clockwise drag on screen turns the demo clockwise.
        var deg = -(a1 - aeDrag.a0) * 180 / Math.PI;
        if (st.shift) deg = Math.round(deg / 15) * 15;
        ae.draft = aeRotateAbout(aeDrag.base, aeDrag.pivot.x, aeDrag.pivot.y, deg);
      } else {
        var dx = (st.mx - aeDrag.mx) / r.sx / aeCam.k, dy = (st.my - aeDrag.my) / r.sy / aeCam.k;
        ae.draft = { x: aeDrag.base.x + dx, y: aeDrag.base.y - dy, z: aeDrag.base.z, yaw: aeDrag.base.yaw };
      }
      ae.dragging = true;
      var now = Date.now();
      if (now - ae.lastSend > 33) { ae.lastSend = now; aeSendDraft(); }
    }

    function aeLayoutView(st, a) {
      var rect = (typeof cfgPreviewRect !== 'undefined') ? cfgPreviewRect : null;
      // Only over the MAP card: another card or tab gets its preview back.
      var rail = (typeof cfgRails !== 'undefined') ? cfgRails.environment : null;
      var onCard = (typeof activeTab === 'undefined' || activeTab === 'environment') && (!rail || rail.sel === 'env.map');
      var show = !!(a.view && a.demo && onCard && rect && rect.w > 200 && rect.h > 160);
      if (aeView.visible !== show) {
        aeView.visible = show;
        // C++ (ConfigHud::UpdateTextCapture) treats the open view like a popup: its clicks, drags and
        // wheel are the view's, never CS2's camera or the scene-object picker's.
        root.SetAttributeString('mapviewopen', show ? '1' : '0');
      }
      if (!show) { aeDrag = null; return; }
      var x = Math.round(rect.x), y = Math.round(rect.y), w = Math.round(rect.w), h = Math.round(rect.h);
      var pos = x + 'px ' + y + 'px 0px';
      if (aeView.__pos !== pos) { aeView.__pos = pos; aeView.style.position = pos; }
      if (aeView.__w !== w) { aeView.__w = w; aeView.style.width = w + 'px'; }
      if (aeView.__h !== h) { aeView.__h = h; aeView.style.height = h + 'px'; aeList.style.height = (h - AE_HEAD_H) + 'px'; }
      var cw = w - AE_LIST_W, ch = h - AE_HEAD_H;
      var cpos = AE_LIST_W + 'px ' + AE_HEAD_H + 'px 0px';
      if (aeCanvas.__pos !== cpos) { aeCanvas.__pos = cpos; aeCanvas.style.position = cpos; }
      if (aeCanvas.__w !== cw) { aeCanvas.__w = cw; aeCanvas.style.width = cw + 'px'; }
      if (aeCanvas.__h !== ch) { aeCanvas.__h = ch; aeCanvas.style.height = ch + 'px'; }
      aeText(aeTitle, 'MAP VIEW');
      aeText(aeSubtitle, a.pick ? ((a.demoTitle || a.demoMap) + ' on ' + (a.pickTitle || a.pick)) : 'Choose the map to play this demo on.');
      aeRenderList(a, ch);
      aePump(st, a);

      var have = aeFrame(a, cw, ch);
      var msg = '';
      if (!have) msg = a.picStatus || 'Reading the maps...';
      else if (a.pick && !a.mapPic) msg = a.picStatus || ('Reading ' + (a.pickTitle || a.pick) + '...');
      if (aeCanvasMsg.text !== msg) aeCanvasMsg.text = msg;
      aeCanvasMsg.visible = msg !== '';
      aeMapImg.visible = have && !!a.mapPic;
      if (a.mapPic && have) {
        if (aeMapImg.__src !== a.mapPic.url) { aeMapImg.__src = a.mapPic.url; aeMapImg.SetImage(a.mapPic.url); }
        var m0 = aeToCanvas(a.mapPic.x0, a.mapPic.y0 + a.mapPic.h);
        aeStyle(aeMapImg, 'position', m0.x.toFixed(1) + 'px ' + m0.y.toFixed(1) + 'px 0px');
        aeStyle(aeMapImg, 'width', (a.mapPic.w * aeCam.k).toFixed(1) + 'px'); aeStyle(aeMapImg, 'height', (a.mapPic.h * aeCam.k).toFixed(1) + 'px');
      }
      aeDemoLayer.visible = have && !!a.demoPic;
      if (a.demoPic && have) {
        var d = a.pick ? ae.draft : { x: 0, y: 0, z: 0, yaw: 0 };
        if (aeDemoWant !== a.demoPic.url) aeDemoImage(a.demoPic.url);
        else aeDemoNext();
        // Placed by its centre and turned about it: the centre of the demo's picture lands where the
        // alignment puts it, and a turn about the centre is the same picture whichever way it is pivoted.
        var lw = a.demoPic.w * aeCam.k, lh = a.demoPic.h * aeCam.k;
        var mid = aeDemoToMap(d, a.demoPic.x0 + a.demoPic.w / 2, a.demoPic.y0 + a.demoPic.h / 2), cm = aeToCanvas(mid.x, mid.y);
        aeStyle(aeDemoLayer, 'position', (cm.x - lw / 2).toFixed(1) + 'px ' + (cm.y - lh / 2).toFixed(1) + 'px 0px');
        aeStyle(aeDemoLayer, 'width', lw.toFixed(1) + 'px');
        aeStyle(aeDemoLayer, 'height', lh.toFixed(1) + 'px');
        // Screen y runs down: a counter-clockwise turn on the map is a negative CSS rotation.
        aeStyle(aeDemoLayer, 'transform', 'rotateZ(' + (-d.yaw).toFixed(2) + 'deg)');
        // The players now, in the demo layer's own pixels so they turn with it.
        var players = a.players || [], sig = players.length + ':' + aeCam.k.toFixed(4);
        for (var pi = 0; pi < players.length; pi++) sig += ',' + players[pi][0] + ',' + players[pi][1];
        if (sig !== aeDots.__sig) {
          aeDots.__sig = sig;
          aeDots.RemoveAndDeleteChildren();
          for (var pj = 0; pj < players.length; pj++) {
            var dot = mk('Panel', aeDots); dot.hittest = false;
            var u = (players[pj][0] - a.demoPic.x0) * aeCam.k, v = (a.demoPic.y0 + a.demoPic.h - players[pj][1]) * aeCam.k;
            dot.style.width = '10px'; dot.style.height = '10px'; dot.style.borderRadius = '5px';
            dot.style.position = (u - 5).toFixed(1) + 'px ' + (v - 5).toFixed(1) + 'px 0px';
            dot.style.backgroundColor = players[pj][2] === 3 ? '#6fb3ffff' : '#ff8a3dff'; // yellow means floating
            dot.style.border = '2px solid #000000cc';
          }
        }
      }
      var demoName = a.demoTitle || a.demoMap || 'the demo\'s map';
      var land = a.pick ? a.landing : null;
      aeText(aeLegend, land
        ? ('Grey: ' + (a.pickTitle || a.pick) + ', lighter is higher. Coloured: ' + demoName + '\'s floor and where its players walked, by where they land. Light lines: its walls. Dots: the players now (orange T, blue CT).')
        : a.pick
          ? ('Grey: ' + (a.pickTitle || a.pick) + ', lighter is higher. Orange: ' + demoName + '\'s walls and where its players walked. Dots: the players now (orange T, blue CT).')
          : ('Orange: ' + demoName + '\'s walls and where its players walked. Dots: the players now. Choose a map on the left.'));
      aeLegend.visible = have;
      aeLand.visible = have && !!land;
      if (land) AE_LANDINGS.forEach(function (k) {
        var t = Math.round((+land[k[0]] || 0) * 100) + '% ' + k[1];
        if (aeLandParts[k[0]].text !== t) aeLandParts[k[0]].text = t;
      });
      aeHint.visible = have && !!a.pick && !!a.mapPic;
    }
)CFJS"
R"CFJS(
    renderHooks.push(function (st) {
      var a = (st && st.altenv) || {};
      ae.st = a;
      // While the pointer drags (here or on a slider) the local draft leads; C++'s echo catches up.
      if (!aeDrag && (!ae.dragging || Date.now() - ae.lastSend > 400)) {
        ae.dragging = false;
        if (a.draft) ae.draft = { x: +a.draft.x || 0, y: +a.draft.y || 0, z: +a.draft.z || 0, yaw: +a.draft.yaw || 0 };
      }
      var own = a.demoTitle || a.demoMap || 'its own map';
      var picked = a.pick ? (a.pickTitle || a.pick) : '';
      aeText(aeMapName, picked || (own + ' (its own map)'));
      aeText(aeMapSub, !a.demo ? 'Open a demo to choose a map for it.'
        : a.pick ? (a.pick + (a.pickAddon ? ' · Workshop item ' + a.pickAddon : ' · installed with CS2'))
        : 'The demo plays on the map it was recorded on.');
      setBtnDisabled(aeChoose, !a.demo);
      aeAlignG.visible = !!a.pick;
      var f = a.fit;
      aeText(aeFit, !a.pick ? '' : !f ? 'Open the map view to see how well the maps line up.'
        : ('Floor match ' + Math.round(f.floor * 100) + '%' + (f.samples ? ' · walked paths on the map ' + Math.round(f.players * 100) + '%' : '')));
      if (!ae.dragging && !aeDrag) {
        aeX.sync(ae.draft.x); aeY.sync(ae.draft.y); aeZ.sync(ae.draft.z); aeYaw.sync(ae.draft.yaw);
      }
      setBtnDisabled(aeAuto, !a.pick || !!a.auto);
      aeText(aeAuto.__lbl, a.auto ? ('MATCHING ' + Math.round((+a.autoProgress || 0) * 100) + '%') : 'AUTO ALIGN');
      aeText(aeViewBtn.__lbl, a.view ? 'HIDE MAP VIEW' : 'SHOW MAP VIEW');
      var playing = !!(a.enabled && a.copy && !a.dirty);
      var title = (picked || own).toUpperCase();
      aeText(aePlay.__lbl, a.building ? ('WRITING THE DEMO FOR ' + title + ' ' + Math.round((+a.progress || 0) * 100) + '%')
        : playing ? ('PLAYING ON ' + title) : ('PLAY ON ' + title));
      aePlay.visible = !!a.pick;
      setBtnDisabled(aePlay, !a.demo || !!a.building || playing);
      aeText(aeBack.__lbl, 'BACK TO ' + own.toUpperCase());
      aeBack.visible = !!a.enabled;
      var status = a.error || a.status || '';
      if (!status && a.pick && a.dirty && a.enabled) status = 'Changed since it was played. Press the button to play it this way.';
      if (!status && a.feetStatus && a.view) status = a.feetStatus;
      if (aeStatus.text !== status) aeStatus.text = status;
      aeStatus.style.color = a.error ? '#ff9a8aff' : S.hint;
      aeStatus.visible = status !== '';
      aeSec.__note(!a.demo ? 'Open a demo' : a.enabled ? ('On ' + (a.appliedTitle || a.applied)) : picked ? (picked + ' · not played yet') : 'Its own map');
      var d0 = ae.draft;
      aeHeadReset.setDisabled(!d0.x && !d0.y && !d0.z && !d0.yaw);
      aeProps.set(!!a.props);
      aeLayoutView(st || {}, a);
    });
)CFJS"
