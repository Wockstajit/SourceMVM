// Is something SCALING the replay bar?
//
// The bar measures 1920 wide and paints a ~500px stub at the right, and no visibility toggle, width
// jiggle or re-generate moves it. A transform explains exactly that: Panorama reports the
// pre-transform layout rectangle, and paints the post-transform one. The preview scaler puts a
// translate3d + scale3d on the game's own HUD panels, and `hud_replay` is supposed to be EXEMPT --
// it is the dock, and it lives at the bottom of the SCREEN rather than inside the picture.
//
// So this prints the transform on the bar and on every ancestor, because a transform on an ancestor
// scales the bar just as surely as one on the bar itself.
//
// Run with: mirv_deadlock livejs load <abs path to this file>

(function () {
  function find(id) {
    var root = $.GetContextPanel();
    if (!root) return null;
    try { return root.FindChildTraverse(id); } catch (e) { return null; }
  }

  function report(label, p) {
    if (!p) { $.Msg('XFORM\t' + label + '\tMISSING\n'); return; }
    var t = '?', to = '?', id = '';
    try { id = p.id || ('<' + p.paneltype + '>'); } catch (e) {}
    try { t = String(p.style.transform); } catch (e) { t = '<threw>'; }
    try { to = String(p.style.transformOrigin); } catch (e) { to = '<threw>'; }
    $.Msg('XFORM\t' + label + '\t' + id + '\ttransform=' + t + '\torigin=' + to + '\n');
  }

  var p = find('hud_replay');
  var depth = 0;
  while (p && depth < 12) {
    report('chain[' + depth + ']', p);
    try { p = p.GetParent(); } catch (e) { p = null; }
    depth++;
  }

  var others = ['SpectatorOptionsContainer', 'PlaybackControls', 'MainPlaybackControls',
                'ReplayScrubber', 'MvmBarButtons'];
  for (var i = 0; i < others.length; ++i) report('panel', find(others[i]));
  $.Msg('XFORM\tdone\n');
})();
