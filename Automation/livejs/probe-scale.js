// Geometry probe: why does our UI read "small" relative to CS2's own?
//
//   mirv_filmmaker livejs load <this file>
//
// Prints one line per measured panel. `actuallayout*` is DEVICE px; a style length in px is
// DESIGN px and gets multiplied by actualuiscale to become device px. So the question this
// answers is: what uiscale is the HUD context running at, and are our panels sized in the same
// space CS2's own HUD panels are.

(function () {
  try {
    var ctx = $.GetContextPanel();
    var out = [];
    function nm(v) { return (v === undefined || v === null) ? '?' : (Math.round(v * 100) / 100); }
    function dump(tag, p) {
      if (!p) { out.push(tag + ': (not found)'); return; }
      var w = p.actuallayoutwidth || 0, h = p.actuallayoutheight || 0;
      var sx = p.actualuiscale_x || 0, sy = p.actualuiscale_y || 0;
      out.push(tag + ': device=' + nm(w) + 'x' + nm(h)
        + '  uiscale=' + nm(sx) + ',' + nm(sy)
        + '  design=' + nm(sx ? w / sx : 0) + 'x' + nm(sy ? h / sy : 0)
        + '  vis=' + (p.visible ? 1 : 0));
    }
    function find(id) { try { return ctx.FindChildTraverse(id); } catch (e) { return null; } }

    dump('ctx (CSGOHud)      ', ctx);

    // Our roots.
    dump('CamEditorRoot      ', find('CamEditorRoot'));
    dump('CamEditorRenderRoot', find('CamEditorRenderRoot'));
    dump('ConfigHudRoot      ', find('ConfigHudRoot'));
    dump('CamTimelineRoot    ', find('CamTimelineRoot'));
    dump('MirvSpeedRow       ', find('MirvSpeedRow'));

    // CS2's own demo bar, as the reference for "correctly sized" at this resolution.
    dump('SliderRow (native) ', find('SliderRow'));
    dump('RoundMarkers(nativ)', find('HighlightIcons'));

    // A couple of native HUD panels for scale comparison.
    dump('HudRadar   (native)', find('HudRadar'));
    dump('HudChat    (native)', find('HudChat'));

    // Walk CamEditorRoot's direct children: the inspector is the 430px column and has no id.
    var er = find('CamEditorRoot');
    if (er && er.GetChildCount) {
      var n = er.GetChildCount();
      out.push('CamEditorRoot children: ' + n);
      for (var i = 0; i < n; i++) {
        var c = er.GetChild(i);
        var w = c.actuallayoutwidth || 0, h = c.actuallayoutheight || 0;
        if (w > 200 && h > 200) dump('  child#' + i + ' (big)   ', c);
      }
    }

    $.Msg('[scaleprobe] ---- begin ----\n');
    for (var k = 0; k < out.length; k++) $.Msg('[scaleprobe] ' + out[k] + '\n');
    $.Msg('[scaleprobe] ---- end ----\n');
  } catch (err) {
    $.Msg('[scaleprobe] error: ' + err + '\n');
  }
})();
