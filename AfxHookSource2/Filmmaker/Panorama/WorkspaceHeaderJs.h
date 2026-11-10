#pragma once

// THE HEADER BAR -- the MODULAR layout's way into every panel (Panorama/UiLayout.h,
// Docs/panorama/modular-layout.md).
//
// In the WORKSPACE layout a panel opens from the demo bar's CAM EDITOR / CONFIG buttons and takes
// over the screen. In the MODULAR layout the game stays full screen and this bar runs along the
// top of it: each button opens or closes one panel, lit in that panel's own accent while it is
// open, so the bar always says what is on screen.
//
//   CONFIG          mirv_filmmaker config toggle        its window (ModuleWindowJs.h)
//   CAMERA EDITOR   mirv_filmmaker editor toggle        its window
//   TIMELINE        mirv_filmmaker dock toggle          the TIMELINE dock along the bottom
//   RENDER          mirv_filmmaker editor render | render close
//   CUSTOMIZE       mirv_filmmaker editor customize     the Customize Player modal
//   gear            Editor Settings (EditorSettingsPopupJs.h, the same card the panels open)
//   WORKSPACE       mirv_filmmaker layout workspace     back to the docked layout
//
// THE LOOK is the dock ribbon's (TimelineDockRibbonJs.h): the same tab box, fill, border and
// 13px tracked caption, each tab carrying its panel's accent -- CONFIG blue, Camera Editor gold,
// Render steel -- so a lit tab here matches the window it opened.
//
//   C++ -> JS : attribute "state" (WorkspaceHeaderHud::BuildStateJson), then $.MvmHeader.render().
//   JS  -> C++: console commands; attribute "winrect" (the bar's rect in client px, for the click
//               router in MovieMode) and "popupopen" (Editor Settings is up).

namespace Filmmaker {

inline const char* kWorkspaceHeaderJs = R"WHJS(
(function () {
)WHJS"
#include "LocalizationJs.h"
R"WHJS(
  try {
    var existing = $('#MvmHeaderRoot'); if (existing) existing.DeleteAsync(0);
    var existingSettings = $('#MvmHeaderSettingsRoot'); if (existingSettings) existingSettings.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    // The panels' own tokens (CameraEditorJs.h / ConfigHudJs.h), so the bar and the windows match.
    var S = {
      config: '#80bdffff', editor: '#f0b323ff', render: '#aeb4bcff', timeline: '#eef2f6ff',
      value: '#eef2f6ff', label: '#c8d3dfff', hint: '#bcc6d2ff', muted: '#8b95a3ff',
      clear: 'rgba(0,0,0,0)',
      font: 'Stratum2, "Arial Unicode MS"'
    };
    function mk(type, parent, props) { var p = $.CreatePanel(type, parent, '', props || {}); return type === 'Label' ? mvmFit(p) : p; }
    function lbl(parent, text, color, size) {
      var l = mk('Label', parent); l.text = text || '';
      if (color) l.style.color = color; if (size) l.style.fontSize = size + 'px';
      l.style.fontFamily = S.font; return l;
    }
    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[header] cmd failed: ' + e + '\n'); }
    }
    function rdInt(name) {
      try { var v = parseInt(GameInterfaceAPI.GetSettingString(name)); return isFinite(v) ? v : 0; }
      catch (e) { return 0; }
    }
    var st = null;
)WHJS"
#include "ModuleWindowJs.h"
#include "EditorSettingsPopupJs.h"
R"WHJS(
    // ---- the bar -----------------------------------------------------------------------------
    // z62: over both windows (52..54), the camera timeline (55) and the marker menu (60), under the
    // TIMELINE dock (65), which never reaches the top of the screen. Z-layer map: PanoramaBridge.h.
    var root = $.CreatePanel('Panel', ctx, 'MvmHeaderRoot', {});
    root.hittest = false; root.visible = false;
    root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '62';

    var band = mk('Panel', root); band.hittest = true;
    band.style.width = '100%'; band.style.height = MVM_WIN.headerH + 'px';
    band.style.horizontalAlign = 'left'; band.style.verticalAlign = 'top';
    band.style.flowChildren = 'right';
    band.style.paddingLeft = '14px'; band.style.paddingRight = '12px';
    // The column ground's base under the bar, with CS2's GO-button line map washed in at a whisper
    // (WorkspaceShellJs.h mvmColumnGround): the bar is made of the same stuff as the windows.
    band.style.backgroundColor = 'rgba(15,17,20,0.97)';
    band.style.borderBottom = '1px solid #ffffff1f';
    band.style.boxShadow = '#000000a6 0px 4px 18px 0px';
    band.SetPanelEvent('onactivate', function () { /* swallow: a click on the bar is not the game's */ });
    var wash = mk('Panel', band); wash.hittest = false;
    wash.style.ignoreParentFlow = 'true';
    wash.style.width = '100%'; wash.style.height = '100%';
    wash.style.backgroundImage = 'url("file://{resources}/videos/linemap.webm")';
    wash.style.backgroundRepeat = 'repeat'; wash.style.backgroundPosition = '20% 60%';
    wash.style.backgroundSize = 'auto auto'; wash.style.backgroundImgOpacity = '0.03';
    wash.style.washColor = '#aeb4bcff'; wash.style.brightness = '0.48';

    // Wordmark: the workspace decor's code line, the same face and tracking.
    var mark = mk('Panel', band); mark.hittest = false;
    mark.style.flowChildren = 'right'; mark.style.verticalAlign = 'center'; mark.style.marginRight = '16px';
    var markRule = mk('Panel', mark); markRule.hittest = false;
    markRule.style.width = '3px'; markRule.style.height = '16px'; markRule.style.verticalAlign = 'center';
    markRule.style.backgroundColor = S.editor; markRule.style.marginRight = '9px';
    var markLbl = lbl(mark, 'SOURCE // MVM', S.value, 12); markLbl.hittest = false;
    markLbl.style.fontWeight = 'bold'; markLbl.style.letterSpacing = '3px';
    markLbl.style.verticalAlign = 'center'; markLbl.style.whiteSpace = 'nowrap';
    var divider = mk('Panel', band); divider.hittest = false;
    divider.style.width = '1px'; divider.style.height = '22px'; divider.style.verticalAlign = 'center';
    divider.style.backgroundColor = '#ffffff1f'; divider.style.marginRight = '12px';

    // One tab: the dock ribbon's box (TimelineDockRibbonJs.h), sized to its caption -- nothing on
    // this bar truncates (Docs/panorama/panorama_ui_guide.md).
    var tabs = {};
    function tab(key, caption, accent, onClick, tip) {
      var b = mk('Panel', band); b.hittest = true;
      b.style.verticalAlign = 'center';
      b.style.paddingTop = '7px'; b.style.paddingBottom = '7px';
      b.style.paddingLeft = '14px'; b.style.paddingRight = '14px';
      b.style.marginRight = '6px';
      b.style.borderRadius = '3px';
      b.style.transitionProperty = 'background-color, border-color';
      b.style.transitionDuration = '0.12s';
      b.style.transitionTimingFunction = 'ease-out';
      var t = lbl(b, caption, S.label, 13); t.hittest = false;
      t.style.fontWeight = 'bold'; t.style.letterSpacing = '1.4px';
      t.style.whiteSpace = 'nowrap'; t.style.textAlign = 'center'; t.style.horizontalAlign = 'center';
      var rec = { panel: b, lbl: t, accent: accent, on: false, hot: false, enabled: true, onClick: onClick };
      b.SetPanelEvent('onmouseover', function () { rec.hot = true; paintTab(rec); });
      b.SetPanelEvent('onmouseout', function () { rec.hot = false; paintTab(rec); });
      b.SetPanelEvent('onactivate', function () { if (rec.enabled) onClick(); });
      if (tip) {
        b.SetPanelEvent('onmouseover', function () { rec.hot = true; paintTab(rec); try { $.DispatchEvent('UIShowTextTooltip', b, tip); } catch (e) {} });
        b.SetPanelEvent('onmouseout', function () { rec.hot = false; paintTab(rec); try { $.DispatchEvent('UIHideTextTooltip', b); } catch (e) {} });
      }
      tabs[key] = rec;
      paintTab(rec);
      return rec;
    }
    // On: the accent at 20% with an accent edge and accent text, exactly the ribbon's selected tab.
    function paintTab(rec) {
      var a = rec.accent.substring(0, 7);
      rec.panel.style.backgroundColor = rec.on ? (a + (rec.hot ? '45' : '33')) : (rec.hot ? '#ffffff1f' : '#ffffff14');
      rec.panel.style.border = '1px solid ' + (rec.on ? rec.accent : (rec.hot ? '#ffffff3d' : '#ffffff1a'));
      rec.lbl.style.color = rec.on ? rec.accent : S.label;
      rec.panel.style.opacity = rec.enabled ? '1.0' : '0.45';
    }
    function setTab(key, on, enabled, caption) {
      var rec = tabs[key]; if (!rec) return;
      on = !!on; enabled = (enabled === undefined) ? true : !!enabled;
      if (caption !== undefined && rec.lbl.text !== caption) rec.lbl.text = caption;
      if (on === rec.on && enabled === rec.enabled) return;
      rec.on = on; rec.enabled = enabled; rec.panel.hittest = enabled;
      paintTab(rec);
    }

    tab('config', T('CONFIG'), S.config, function () { cmd('mirv_filmmaker config toggle'); });
    tab('editor', T('CAMERA EDITOR'), S.editor, function () { cmd('mirv_filmmaker editor toggle'); });
    tab('timeline', T('TIMELINE'), S.timeline, function () { cmd('mirv_filmmaker dock toggle'); });
    tab('render', T('RENDER'), S.render, function () {
      cmd((st && st.render) ? 'mirv_filmmaker render close' : 'mirv_filmmaker editor render');
    });
    tab('customize', '✦  ' + T('CUSTOMIZE'), S.editor, function () { cmd('mirv_filmmaker editor customize'); },
      T('Customize Player: skins, knife, gloves and agent'));

    var spacer = mk('Panel', band); spacer.hittest = false;
    spacer.style.width = 'fill-parent-flow(1.0)'; spacer.style.height = '1px';

    // Editor Settings: the same gear and card the panels' dock ribbon opens.
    var gear = mk('Panel', band); gear.hittest = true;
    gear.style.width = '34px'; gear.style.marginRight = '10px'; gear.style.verticalAlign = 'center';
    gear.style.paddingTop = '5px'; gear.style.paddingBottom = '5px';
    gear.style.borderRadius = '3px';
    gear.style.backgroundColor = '#ffffff14'; gear.style.border = '1px solid #ffffff1a';
    var gearLbl = lbl(gear, '⚙', S.value, 16); gearLbl.hittest = false;
    gearLbl.style.horizontalAlign = 'center'; gearLbl.style.textAlign = 'center';
    var settingsPopup = mvmEditorSettingsPopup(ctx, {
      id: 'MvmHeaderSettingsRoot',
      publish: root,
      // Drops from the gear at the top right, not up from a bottom ribbon.
      marginRight: 150, marginBottom: 16,
      accent: S.config,
      modular: function () { return !!(st && st.modular); },
      onOpen: function () { gear.style.backgroundColor = '#80bdff33'; },
      onClose: function () { gear.style.backgroundColor = '#ffffff14'; }
    });
    settingsPopup.card.style.verticalAlign = 'top';
    settingsPopup.card.style.marginTop = (MVM_WIN.headerH + 8) + 'px';
    gear.SetPanelEvent('onactivate', function () { settingsPopup.toggle(); });

    // The layout switch: the docked workspaces or these windows. A pair of segments, so it reads
    // as "which of two" rather than as one more panel to open.
    var seg = mk('Panel', band); seg.hittest = false;
    seg.style.flowChildren = 'right'; seg.style.verticalAlign = 'center';
    seg.style.border = '1px solid #ffffff1a'; seg.style.borderRadius = '3px';
    function segBtn(caption, on, onClick) {
      var b = mk('Panel', seg); b.hittest = !on;
      b.style.paddingTop = '6px'; b.style.paddingBottom = '6px';
      b.style.paddingLeft = '12px'; b.style.paddingRight = '12px';
      b.style.backgroundColor = on ? '#ffffff1f' : S.clear;
      var t = lbl(b, caption, on ? S.value : S.muted, 12); t.hittest = false;
      t.style.fontWeight = 'bold'; t.style.letterSpacing = '1.4px'; t.style.whiteSpace = 'nowrap';
      if (!on) {
        b.SetPanelEvent('onmouseover', function () { t.style.color = S.value; b.style.backgroundColor = '#ffffff0d'; });
        b.SetPanelEvent('onmouseout', function () { t.style.color = S.muted; b.style.backgroundColor = S.clear; });
        b.SetPanelEvent('onactivate', onClick);
      }
      return b;
    }
    segBtn(T('WORKSPACE'), false, function () { cmd('mirv_filmmaker layout workspace'); });
    segBtn(T('MODULAR'), true, function () {});

    // ---- render ------------------------------------------------------------------------------
    var api = {};
    var lastRect = null;
    api.render = function () {
      var raw = root.GetAttributeString('state', '');
      if (!raw) { root.visible = false; return; }
      try { st = JSON.parse(raw); } catch (e) { return; }
      var on = !!st.enabled;
      if (root.visible !== on) root.visible = on;
      if (!on) {
        if (settingsPopup.isOpen()) settingsPopup.close('hidden');
        if (lastRect !== '') { lastRect = ''; root.SetAttributeString('winrect', ''); }
        return;
      }
      setTab('config', st.config);
      setTab('editor', st.editor && !st.render);
      setTab('timeline', st.dock === 'timeline');
      setTab('render', st.render, true, st.renderJob ? T('RENDER PROGRESS') : T('RENDER'));
      setTab('customize', st.customize, !!st.customizeReady);
      // Where the bar is, for the click router: a click here is the UI's in either mouse mode.
      var r = band.GetPositionWithinWindow ? band.GetPositionWithinWindow() : null;
      var s = r ? (Math.round(r.x) + ' ' + Math.round(r.y) + ' ' + Math.round(band.actuallayoutwidth || 0)
        + ' ' + Math.round(band.actuallayoutheight || 0)) : '';
      if (s !== lastRect) { lastRect = s; root.SetAttributeString('winrect', s); }
    };
    // Automation (netcon cannot click): $.MvmHeader.press('config'), $.MvmHeader.settings().
    api.press = function (key) {
      key = ('' + key).replace(/^\s+|\s+$/g, '');
      var rec = tabs[key];
      if (!rec || !rec.enabled) return 'unknown:' + key;
      rec.onClick();
      return key;
    };
    api.settings = function () { settingsPopup.toggle(); return settingsPopup.isOpen() ? 'open' : 'closed'; };
    $.MvmHeader = api;
    $.Msg('[header] built.\n');
  } catch (e) {
    $.Msg('[header] build error: ' + e + '\n');
  }
})();
)WHJS";

} // namespace Filmmaker
