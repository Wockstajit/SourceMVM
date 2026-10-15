#pragma once
#include "ColorMathJs.h"

// The CONFIG colour picker (Docs/features/filmmaker_config_panel.md): it opens INLINE,
// under the control that asked for it, so it never floats over another layer, never needs
// placing against the screen edge, and pushes the rows below it down instead of covering them.
//
//   wheel      132px HSV disc: hue around, saturation outward. Panorama has no conic gradient,
//              so the disc is 72 rotated spokes, each a linear gradient from its pure hue at the
//              rim to white at the centre; a black disc over it at opacity (1 - V) is brightness.
//   brightness a bar from black to the full colour, with a readout
//   readouts   before/now strip, HEX and RGB, 12 preset swatches, Revert (back to "before"), Done
//
// Dragging uses the OS-cursor pipe the value rows use (st.mx / st.my / st.lmb): there is no
// mouse-move event in this HUD context. Only one picker is open at a time.
R"CLRJS(
    var cfgColorAnchor = null, cfgColorOwner = null;
    var cfgColorLastLmb = false, cfgColorDrag = '';
    var CFG_COLOR_WHEEL = 132, CFG_COLOR_SPOKES = 72;
    var CFG_COLOR_PRESETS = ['#ffffff', '#ffd9b8', '#ffc857', '#ff8a5c', '#e0302c', '#ffa6c4',
                             '#c4a6ff', '#b8d4ff', '#7fe0d4', '#b8c4d0', '#5b6470', '#1a1a1a'];

    function cfgColorOver(panel, x, y) {
      if (!panel || !cfgPanelOnScreen(panel)) return false;
      var p = panel.GetPositionWithinWindow();
      return x >= p.x && y >= p.y && x < p.x + panel.actuallayoutwidth && y < p.y + panel.actuallayoutheight;
    }
    function cfgCloseColor() {
      var o = cfgColorOwner;
      cfgColorOwner = null; cfgColorDrag = '';
      if (o && o.ui) o.ui.host.visible = false;
      if (o && o.onClose) o.onClose();
    }
    function cfgHex6(rgb) { return rgbHex(rgb).substring(0, 7).toUpperCase(); }

    function cfgColorBuild(owner, host) {
      var ui = { host: host };
      // Full width, no side margin: in this Panorama host a 100%-wide panel with a margin hangs
      // that margin past the pane's right edge.
      host.style.flowChildren = 'right'; host.style.width = '100%';
      host.style.marginTop = '6px';
      host.style.padding = '12px'; host.style.borderRadius = '3px';
      host.style.backgroundColor = S.drawer; host.style.border = '1px solid #ffffff14';
      var wheel = mk('Panel', host); wheel.hittest = true;
      wheel.style.width = CFG_COLOR_WHEEL + 'px'; wheel.style.height = CFG_COLOR_WHEEL + 'px';
      wheel.style.marginRight = '14px';
      var R = CFG_COLOR_WHEEL / 2;
      for (var i = 0; i < CFG_COLOR_SPOKES; i++) {
        var h = i / CFG_COLOR_SPOKES;
        var sp = mk('Panel', wheel); sp.hittest = false;
        sp.style.width = '8px'; sp.style.height = R + 'px';
        sp.style.horizontalAlign = 'center'; sp.style.verticalAlign = 'top';
        sp.style.transformOrigin = '50% 100%';
        sp.style.transform = 'rotateZ(' + (h * 360).toFixed(2) + 'deg)';
        sp.style.backgroundColor = 'gradient( linear, 0% 0%, 0% 100%, from( ' + rgbHex(mvmHsvToRgb(h, 1, 1))
          + ' ), to( #ffffffff ) )';
      }
      var dark = mk('Panel', wheel); dark.hittest = false;
      dark.style.width = '100%'; dark.style.height = '100%'; dark.style.borderRadius = '50%';
      dark.style.backgroundColor = '#000000ff'; dark.style.opacity = '0';
      var ring = mk('Panel', wheel); ring.hittest = false;
      ring.style.width = '100%'; ring.style.height = '100%'; ring.style.borderRadius = '50%';
      ring.style.border = '1px solid #ffffff26';
      var marker = mk('Panel', wheel); marker.hittest = false;
      marker.style.width = '14px'; marker.style.height = '14px'; marker.style.borderRadius = '50%';
      marker.style.border = '2px solid #ffffffff'; marker.style.boxShadow = '#000000aa 0px 0px 0px 1px';

      var col = mk('Panel', host); col.hittest = false; col.hittestchildren = true;
      col.style.flowChildren = 'down'; col.style.width = 'fill-parent-flow(1.0)';
      var bn = mk('Panel', col); bn.hittest = false;
      bn.style.flowChildren = 'right'; bn.style.width = '100%'; bn.style.height = '34px';
      bn.style.borderRadius = '3px'; bn.style.border = '1px solid #ffffff1f';
      var before = mk('Panel', bn); before.hittest = false;
      before.style.width = 'fill-parent-flow(1.0)'; before.style.height = '100%';
      var now = mk('Panel', bn); now.hittest = false;
      now.style.width = 'fill-parent-flow(2.0)'; now.style.height = '100%';
      var vHead = mk('Panel', col); vHead.hittest = false;
      vHead.style.flowChildren = 'right'; vHead.style.width = '100%'; vHead.style.marginTop = '9px';
      var vCap = lbl(vHead, 'Brightness', S.muted, 11); vCap.hittest = false;
      vCap.style.textTransform = 'uppercase';
      vCap.style.fontWeight = 'bold'; vCap.style.letterSpacing = '1.2px'; vCap.style.width = 'fill-parent-flow(1.0)';
      var vText = lbl(vHead, '', S.value, 12); vText.hittest = false;
      vText.style.fontWeight = 'bold'; vText.style.fontFamily = S.fontMonodigit;
      var vBar = mk('Panel', col); vBar.hittest = true;
      vBar.style.width = '100%'; vBar.style.height = '20px'; vBar.style.marginTop = '2px';
      var vTrack = mk('Panel', vBar); vTrack.hittest = false;
      vTrack.style.width = '100%'; vTrack.style.height = '14px'; vTrack.style.verticalAlign = 'center';
      vTrack.style.borderRadius = '2px'; vTrack.style.border = '1px solid #ffffff1f';
      var vLine = mk('Panel', vBar); vLine.hittest = false;
      vLine.style.flowChildren = 'right'; vLine.style.width = '100%'; vLine.style.height = '20px';
      vLine.style.overflow = 'noclip';
      var vSpacer = mk('Panel', vLine); vSpacer.hittest = false; vSpacer.style.height = '1px';
      var vKnob = mk('Panel', vLine); vKnob.hittest = false;
      vKnob.style.width = '6px'; vKnob.style.height = '20px'; vKnob.style.marginLeft = '-3px';
      vKnob.style.borderRadius = '2px'; vKnob.style.backgroundColor = '#eef2f6ff';
      vKnob.style.boxShadow = '#000000aa 0px 0px 0px 1px';
)CLRJS"
R"CLRJS(
      var read = mk('Panel', col); read.hittest = false;
      read.style.flowChildren = 'right'; read.style.width = '100%'; read.style.marginTop = '9px';
      function readout(cap) {
        var c = lbl(read, cap, S.muted, 12); c.hittest = false;
        c.style.fontWeight = 'bold'; c.style.fontFamily = S.fontMonodigit; c.style.marginRight = '5px';
        var v = lbl(read, '', S.value, 12); v.hittest = false;
        v.style.fontWeight = 'bold'; v.style.fontFamily = S.fontMonodigit; v.style.marginRight = '12px';
        return v;
      }
      var hexText = readout('HEX'), rgbText = readout('RGB');
      var presets = mk('Panel', col); presets.hittest = false; presets.hittestchildren = true;
      presets.style.flowChildren = 'right-wrap'; presets.style.width = '100%'; presets.style.marginTop = '9px';
      for (var pi = 0; pi < CFG_COLOR_PRESETS.length; pi++) (function (hex) {
        var sw = mk('Panel', presets); sw.hittest = true;
        sw.style.width = '20px'; sw.style.height = '16px'; sw.style.borderRadius = '2px';
        sw.style.marginRight = '5px'; sw.style.marginBottom = '5px';
        sw.style.backgroundColor = hex + 'ff'; sw.style.boxShadow = 'inset 0px 0px 0px 1px #ffffff26';
        sw.style.border = '1px solid ' + S.clear;
        sw.SetPanelEvent('onmouseover', function () { sw.style.border = '1px solid #ffffff88'; });
        sw.SetPanelEvent('onmouseout', function () { sw.style.border = '1px solid ' + S.clear; });
        sw.SetPanelEvent('onactivate', function () {
          var n = parseInt(hex.substring(1), 16);
          owner.hsv = mvmRgbToHsv([(n >> 16) & 255, (n >> 8) & 255, n & 255], owner.hsv[0]);
          cfgColorCommit(owner);
        });
      })(CFG_COLOR_PRESETS[pi]);
      var acts = mk('Panel', col); acts.hittest = false; acts.hittestchildren = true;
      acts.style.flowChildren = 'right'; acts.style.width = 'fit-children';
      acts.style.horizontalAlign = 'right'; acts.style.marginTop = '4px';
      // "Revert", not "Reset": every card header's Reset restores DEFAULTS; this goes back to
      // the colour the picker opened with.
      var reset = btn(acts, 'Revert', function () {
        owner.hsv = mvmRgbToHsv(owner.orig, owner.hsv[0]);
        cfgColorCommit(owner);
      }, S.label);
      reset.__bgBase = S.clear; paintBtn(reset);
      var done = mk('Panel', acts); done.hittest = true;
      done.style.borderRadius = '3px';
      done.style.paddingTop = '5px'; done.style.paddingBottom = '5px';
      done.style.paddingLeft = '13px'; done.style.paddingRight = '13px';
      var dl = lbl(done, 'Done', '#0c0e12ff', 12); dl.hittest = false; dl.style.fontWeight = 'bold';
      hoverable(done, function (p) {
        p.style.backgroundColor = p.__hot ? (S.accent.substring(0, 7) + 'e6') : S.accent;
      });
      done.SetPanelEvent('onactivate', function () { if (cfgColorOwner === owner) cfgCloseColor(); });
      ui.wheel = wheel; ui.dark = dark; ui.marker = marker; ui.before = before; ui.now = now;
      ui.vText = vText; ui.vBar = vBar; ui.vTrack = vTrack; ui.vSpacer = vSpacer;
      ui.hexText = hexText; ui.rgbText = rgbText;
      return ui;
    }

    function cfgColorPaint(owner) {
      var u = owner.ui; if (!u) return;
      var h = owner.hsv[0], s = owner.hsv[1], v = owner.hsv[2];
      var rgb = mvmHsvToRgb(h, s, v);
      var R = CFG_COLOR_WHEEL / 2, a = h * Math.PI * 2;
      var mx = R + Math.sin(a) * s * R - 7, my = R - Math.cos(a) * s * R - 7;
      u.marker.style.position = mx.toFixed(1) + 'px ' + my.toFixed(1) + 'px 0px';
      u.marker.style.backgroundColor = rgbHex(rgb);
      u.dark.style.opacity = (1 - v).toFixed(3);
      u.now.style.backgroundColor = rgbHex(rgb);
      u.before.style.backgroundColor = rgbHex(owner.orig);
      u.vTrack.style.backgroundColor = 'gradient( linear, 0% 0%, 100% 0%, from( #000000ff ), to( '
        + rgbHex(mvmHsvToRgb(h, s, 1)) + ' ) )';
      u.vSpacer.style.width = (clamp01(v) * 100).toFixed(2) + '%';
      u.vText.text = Math.round(v * 100 * owner.scale) + '%';
      u.hexText.text = cfgHex6(rgb);
      u.rgbText.text = rgb[0] + ' ' + rgb[1] + ' ' + rgb[2];
      if (owner.onPaint) owner.onPaint(rgb);
    }
    function cfgColorCommit(owner) {
      var rgb = mvmHsvToRgb(owner.hsv[0], owner.hsv[1], owner.hsv[2]);
      var dst = owner.rgb, changed = false;
      for (var i = 0; i < 3; i++) { if (dst[i] !== rgb[i]) changed = true; dst[i] = rgb[i]; }
      cfgColorPaint(owner);
      if (changed) owner.change(dst);
    }
    function cfgColorPump(st) {
      var lmb = !!(st && st.cursor && st.lmb), down = lmb && !cfgColorLastLmb;
      cfgColorLastLmb = lmb;
      if (!st || typeof st.mx !== 'number') return;
      var o = cfgColorOwner;
      if (!o || !o.ui) { cfgColorDrag = ''; return; }
      if (!cfgPanelOnScreen(o.ui.host)) { if (!lmb) cfgColorDrag = ''; return; }
      var u = o.ui, mx = st.mx, my = st.my;
      if (down) {
        if (cfgColorOver(u.wheel, mx, my)) cfgColorDrag = 'w';
        else if (cfgColorOver(u.vBar, mx, my)) cfgColorDrag = 'v';
      }
      if (lmb && cfgColorDrag) {
        var panel = cfgColorDrag === 'w' ? u.wheel : u.vBar;
        var r = panel.GetPositionWithinWindow(), width = panel.actuallayoutwidth, height = panel.actuallayoutheight;
        if (cfgColorDrag === 'w') {
          var dx = mx - r.x - width / 2, dy = my - r.y - height / 2;
          var hh = Math.atan2(dx, -dy) / (Math.PI * 2);
          o.hsv[0] = (hh % 1 + 1) % 1;
          o.hsv[1] = clamp01(Math.sqrt(dx * dx + dy * dy) / (width / 2));
        } else {
          o.hsv[2] = clamp01((mx - r.x) / width);
        }
        cfgColorCommit(o);
      }
      if (!lmb) cfgColorDrag = '';
    }

    // One picker per colour, built lazily: the host panel is created NOW (so it sits in the flow
    // right under the control that owns it) and filled the first time it opens.
    function colorPicker(parent, titleText, rgbRef, onChange, scale) {
      endTileLine(parent);
      var host = mk('Panel', parent); host.hittest = false; host.hittestchildren = true;
      host.visible = false;
      var owner = { rgb: rgbRef, change: onChange, scale: scale || 1, ui: null,
                    hsv: [0, 0, 1], orig: [255, 255, 255], onClose: null, onPaint: null };
      var api = {
        owner: owner,
        show: function () {
          if (cfgColorOwner === owner) { cfgCloseColor(); return; }
          cfgCloseColor();
          if (!owner.ui) owner.ui = cfgColorBuild(owner, host);
          owner.orig = [rgbRef[0], rgbRef[1], rgbRef[2]];
          owner.hsv = mvmRgbToHsv(rgbRef, owner.hsv[0]);
          cfgColorOwner = owner;
          host.visible = true;
          cfgColorPaint(owner);
          onChange(rgbRef);
          if (api.onOpen) api.onOpen();
        },
        hide: function () { if (cfgColorOwner === owner) cfgCloseColor(); },
        visible: function () { return cfgColorOwner === owner; },
        sync: function () {
          if (cfgColorOwner === owner && !cfgColorDrag) { owner.hsv = mvmRgbToHsv(rgbRef, owner.hsv[0]); cfgColorPaint(owner); }
        }
      };
      return api;
    }
)CLRJS"
R"CLRJS(
    // The closed field: the label column, then a full-width strip filled with the current colour,
    // its HEX code and a chevron. Clicking it opens the picker inline below.
    var cfgColorFields = {};   // by title, for the $.ConfigHud.color() automation helper
    function colorField(parent, title, read, write, scale) {
      var rgb = [255, 255, 255], max = scale || 1, identity, hasIdentity = false;
      var line = row(parent); line.style.paddingLeft = UI.line.padH + 'px';
      line.style.marginTop = (UI.line.gap + 1) + 'px';
      var caption = lbl(line, title, S.label, 13); caption.hittest = false;
      caption.style.width = UI.line.label + 'px'; caption.style.verticalAlign = 'center';
      caption.style.whiteSpace = 'nowrap'; caption.style.textOverflow = 'ellipsis';
      var field = mk('Panel', line); field.hittest = true;
      field.style.flowChildren = 'right'; field.style.width = 'fill-parent-flow(1.0)';
      field.style.height = '32px'; field.style.marginLeft = UI.line.colGap + 'px';
      field.style.paddingTop = '3px'; field.style.paddingBottom = '3px';
      field.style.paddingLeft = '3px'; field.style.paddingRight = '6px';
      field.style.borderRadius = '3px'; field.style.backgroundColor = '#0c0e12ff';
      trans(field, 'border-color', 0.08);
      var strip = mk('Panel', field); strip.hittest = false;
      strip.style.width = 'fill-parent-flow(1.0)'; strip.style.height = '100%';
      strip.style.borderRadius = '2px'; strip.style.boxShadow = 'inset 0px 0px 0px 1px #ffffff1f';
      var hex = lbl(field, '#FFFFFF', S.label, 12); hex.hittest = false;
      hex.style.fontWeight = 'bold'; hex.style.fontFamily = S.fontMonodigit;
      hex.style.verticalAlign = 'center'; hex.style.marginLeft = '8px';
      var chev = lbl(field, '▾', S.muted, 12); chev.hittest = false;
      chev.style.verticalAlign = 'center'; chev.style.marginLeft = '8px';
      chev.style.transformOrigin = '50% 50%';
      function paint() {
        var open = picker.visible();
        strip.style.backgroundColor = rgbHex(rgb);
        hex.text = cfgHex6(rgb);
        field.style.border = '1px solid ' + (open ? (S.accent.substring(0, 7) + '99') : (field.__hot ? '#ffffff40' : '#ffffff1f'));
        chev.style.transform = open ? 'rotateZ(180deg)' : 'rotateZ(0deg)';
      }
      var picker = colorPicker(parent, title, rgb, function (value) {
        paint(); write(value.map(function (v) { return +(v / 255 * max).toFixed(4); }));
      }, max);
      picker.owner.onClose = paint;
      picker.onOpen = paint;
      cfgColorFields[title] = picker;
      hoverable(field, paint);
      field.SetPanelEvent('onactivate', function () { picker.show(); });
      renderHooks.push(function () {
        var data = read(); if (!data) { picker.hide(); return; }
        if (!hasIdentity) { identity = data.id; hasIdentity = true; }
        else if (identity !== data.id) { picker.hide(); identity = data.id; }
        if (!picker.visible()) for (var i = 0; i < 3; i++) rgb[i] = Math.round(clamp01(data.rgb[i] / max) * 255);
        paint();
      });
      return picker;
    }
)CLRJS"
