// Live experiment: can Panorama's `ui-scale` be used as a single lever to make the whole
// filmmaker UI bigger, without breaking the layouts that are computed from root pixels?
//
//   mirv_filmmaker livejs load <this file>
//
// `ui-scale` appears in CS2's own transition-property lists, so the property exists. The open
// question is what it does to a panel whose size is a PERCENTAGE of its parent (our roots are
// 100%x100%): if the layout box is computed first and then scaled, a scaled 100% root renders
// larger than the screen and its edge-anchored children get clipped -- which would rule the lever
// out for the roots and leave it usable only on fixed-size children like the inspector column.
//
// This script applies a scale to the inspector column only, reports the before/after geometry, and
// leaves it applied so the result can be judged by eye. Re-run with SCALE = 1 to undo.

(function () {
  try {
    var SCALE = 1.18;   // <-- tune me and re-save

    var ctx = $.GetContextPanel();
    var er = ctx.FindChildTraverse('CamEditorRoot');
    if (!er) { $.Msg('[tryscale] CamEditorRoot not found -- open the camera editor first\n'); return; }

    function nm(v) { return Math.round((v || 0) * 100) / 100; }
    function line(tag, p) {
      $.Msg('[tryscale] ' + tag
        + ' device=' + nm(p.actuallayoutwidth) + 'x' + nm(p.actuallayoutheight)
        + ' uiscale=' + nm(p.actualuiscale_x)
        + ' vis=' + (p.visible ? 1 : 0) + '\n');
    }

    // The inspector is the tall 430-design-px column; find it by shape rather than by id.
    var target = null, n = er.GetChildCount ? er.GetChildCount() : 0;
    for (var i = 0; i < n; i++) {
      var c = er.GetChild(i);
      var w = c.actuallayoutwidth || 0, h = c.actuallayoutheight || 0;
      if (w > 400 && w < 600 && h > 900) { target = c; break; }
    }
    if (!target) { $.Msg('[tryscale] inspector column not found\n'); return; }

    $.Msg('[tryscale] ---- before ----\n');
    line('inspector', target);
    line('root     ', er);

    // Try the lever. Both spellings are attempted because the JS property name for a hyphenated
    // Panorama style is not guaranteed; whichever one sticks is the one to use in the real fix.
    var applied = 'none';
    try { target.style.uiScale = (SCALE * 100).toFixed(1) + '%'; applied = 'uiScale%'; } catch (e) {}
    if (applied === 'none') { try { target.style.uiScale = String(SCALE); applied = 'uiScaleNum'; } catch (e2) {} }

    $.Schedule(0.15, function () {
      try {
        $.Msg('[tryscale] ---- after (applied=' + applied + ' scale=' + SCALE + ') ----\n');
        line('inspector', target);
        line('root     ', er);
        $.Msg('[tryscale] note: if inspector device width grew by ~' + SCALE
          + 'x and it is NOT clipped, the lever works.\n');
      } catch (e) {}
    });
  } catch (err) {
    $.Msg('[tryscale] error: ' + err + '\n');
  }
})();
