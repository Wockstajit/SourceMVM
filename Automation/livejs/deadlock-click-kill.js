// Click the first kill marker, the way a person does, and report where it takes us.
//
// The seek is asynchronous, so this only fires the click and prints the tick it asked for; read the
// tick back with a separate `mirv_deadlock health` afterwards. The point is that the landing tick is
// BEFORE the kill by the lead-in, not on it -- arriving on the exact tick of a kill means arriving
// after the thing worth watching.
//
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-click-kill.js
(function () {
  var TAB = String.fromCharCode(9);
  function out(s) { $.Msg('CLICK' + TAB + s + '\n'); }
  try {
    var ctx = $.GetContextPanel();
    var box = ctx.FindChildTraverse('MarkersContainer');
    if (!box) { out('no MarkersContainer'); return; }
    var n = box.GetChildCount();
    var target = null, targetId = '';
    for (var i = 0; i < n; ++i) {
      var c = box.GetChild(i);
      var id = '';
      try { id = c.id || ''; } catch (e) {}
      if (/^kill_event_/.test(id)) { target = c; targetId = id; break; }
    }
    if (!target) { out('no kill marker'); return; }
    var tick = parseInt(targetId.replace(/^kill_event_/, ''), 10);
    out('clicking' + TAB + targetId + ' (event tick ' + tick + ')');
    // The source argument is not optional: without it Panorama fires the event with no source and
    // the handler never runs -- a button that highlights and does nothing.
    $.DispatchEvent('Activated', target, 'mouse');
    out('dispatched' + TAB + 'expect the playhead a lead-in BEFORE ' + tick);
  } catch (err) {
    out('error' + TAB + err);
  }
})();
