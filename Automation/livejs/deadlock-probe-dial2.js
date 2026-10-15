// Follow-up probe. The first one reported every zone caption at fraction 0.0000, which is either a
// dial that never placed them or a probe that cannot see how they were placed -- Panorama's
// `actualxoffset` reports the LAYOUT offset, and `transform` is applied after layout, so a panel
// moved by transform reads as if it never moved. `position` does show up there. This prints the raw
// numbers and the inline styles side by side so the two cases can be told apart.
//
// It also reports whether our own onactivate is bound to a kill marker, and what the handler emits,
// because the click landed on the wrong tick and the two candidate causes (our handler not bound vs
// our handler computing wrongly) look identical from outside.
//
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-probe-dial2.js
(function () {
  var TAB = String.fromCharCode(9);
  function out(s) { $.Msg('D2' + TAB + s + '\n'); }
  try {
    var ctx = $.GetContextPanel();
    var sx = ctx.actualuiscale_x || 1;
    var d = $.MvmNativeHud && $.MvmNativeHud.dial ? $.MvmNativeHud.dial() : null;
    if (!d) { out('no dial'); return; }

    function raw(label, p) {
      var s = label;
      try { s += ' xoff=' + p.actualxoffset; } catch (e) { s += ' xoff=ERR'; }
      try { s += ' w=' + p.actuallayoutwidth + ' h=' + p.actuallayoutheight; } catch (e) {}
      try { s += ' vis=' + p.visible; } catch (e) {}
      try { s += ' pos=' + p.style.position; } catch (e) {}
      try { s += ' xf=' + p.style.transform; } catch (e) {}
      try { s += ' op=' + p.style.opacity; } catch (e) {}
      try { s += ' text="' + (p.text === undefined ? '' : p.text) + '"'; } catch (e) {}
      out(s);
    }
    raw('strip', d.strip);
    for (var i = 0; i < d.zoneLabels.length; ++i) raw('label' + i, d.zoneLabels[i]);
    for (var j = 0; j < d.zoneTicks.length; ++j) raw('divider' + j, d.zoneTicks[j]);
    raw('handle', d.handle);
    raw('centreTick', d.centreTick);
    raw('pill', d.pill);
    out('uiscale' + TAB + sx);

    // The marker's handler: is ours bound, and what does it compute?
    var box = ctx.FindChildTraverse('MarkersContainer');
    if (box) {
      var n = box.GetChildCount();
      for (var k = 0; k < n; ++k) {
        var c = box.GetChild(k);
        var id = '';
        try { id = c.id || ''; } catch (e) {}
        if (!/^kill_event_/.test(id)) continue;
        var s2 = 'marker' + TAB + id;
        try { s2 += ' ours=' + (c.__mvmSeek === true); } catch (e) { s2 += ' ours=ERR'; }
        try { s2 += ' hittest=' + c.hittest; } catch (e) {}
        try { s2 += ' pos=' + c.style.position; } catch (e) {}
        out(s2);
        break;
      }
    }
    // And what the slider currently reports, since the click's landing tick has to be read against
    // the same span the handler used.
    var sl = ctx.FindChildTraverse('TimeScrubSlider');
    if (sl) { try { out('slider' + TAB + 'value=' + sl.value + ' min=' + sl.min + ' max=' + sl.max); } catch (e) {} }
  } catch (err) {
    out('error' + TAB + err);
  }
})();
