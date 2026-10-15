// Why is Deadlock's own replay bar not PAINTING while our appended buttons are?
//
// The panel tree says the bar is there, full width, with every control laid out at a sensible
// rectangle -- and the screen shows nothing left of the inspector column. A geometry dump cannot
// tell those apart, because everything it reports is already correct. So this asks the three
// questions geometry cannot answer: what opacity is on the chain, which classes are on it, and
// what is drawn over it.
//
// Run with: mirv_deadlock livejs load <abs path to this file>

(function () {
  function find(id) {
    var root = $.GetContextPanel();
    if (!root) return null;
    try { return root.FindChildTraverse(id); } catch (e) { return null; }
  }

  function chain(p) {
    var out = [], guard = 0;
    while (p && guard++ < 24) {
      var id = '';
      try { id = p.id || ('<' + p.paneltype + '>'); } catch (e) { id = '?'; }
      var cls = '';
      try { cls = (p.GetAttributeString ? '' : ''); } catch (e) {}
      var vis = '?', op = '?', w = '?', h = '?', x = '?', y = '?';
      try { vis = p.visible ? '1' : '0'; } catch (e) {}
      try { op = String(p.style.opacity); } catch (e) {}
      try {
        w = String(Math.round(p.actuallayoutwidth));
        h = String(Math.round(p.actuallayoutheight));
        x = String(Math.round(p.actualxoffset));
        y = String(Math.round(p.actualyoffset));
      } catch (e) {}
      var hidden = '?';
      try { hidden = p.BHasClass('Hidden') ? 'Hidden' : '-'; } catch (e) {}
      $.Msg('BARCHAIN\t' + id + '\tvis=' + vis + '\topacity=' + op +
            '\trect=' + x + ',' + y + ' ' + w + 'x' + h + '\t' + hidden + '\n');
      try { p = p.GetParent(); } catch (e) { p = null; }
    }
  }

  var bar = find('hud_replay');
  if (!bar) { $.Msg('BARPROBE\tno hud_replay\n'); return; }

  $.Msg('BARPROBE\t--- chain from hud_replay up to the root ---\n');
  chain(bar);

  // The controls that are missing on screen, and one of ours that is not.
  var ids = ['PlaybackControls', 'MainPlaybackControls', 'PlayButton', 'ReplayScrubber',
             'TimeScrubSlider', 'SliderTrack', 'MvmBarButtons', 'MvmBarLoop'];
  $.Msg('BARPROBE\t--- individual controls ---\n');
  for (var i = 0; i < ids.length; ++i) {
    var p = find(ids[i]);
    if (!p) { $.Msg('BARONE\t' + ids[i] + '\tMISSING\n'); continue; }
    var vis = '?', op = '?', bg = '?', w = '?', h = '?';
    try { vis = p.visible ? '1' : '0'; } catch (e) {}
    try { op = String(p.style.opacity); } catch (e) {}
    try { bg = String(p.style.backgroundColor); } catch (e) {}
    try { w = String(Math.round(p.actuallayoutwidth)); h = String(Math.round(p.actuallayoutheight)); } catch (e) {}
    $.Msg('BARONE\t' + ids[i] + '\tvis=' + vis + '\topacity=' + op + '\tbg=' + bg +
          '\tsize=' + w + 'x' + h + '\n');
  }

  // Our own dock panels, which sit at a higher z than the game's HUD and are the only thing that
  // could be painting over the bar.
  $.Msg('BARPROBE\t--- ours, over the same band ---\n');
  var mine = ['MvmWorkspaceRoot', 'MvmDockBody', 'MvmRegularTimeline', 'MvmDockRibbon', 'MvmBandB'];
  for (var j = 0; j < mine.length; ++j) {
    var q = find(mine[j]);
    if (!q) { $.Msg('BARMINE\t' + mine[j] + '\tMISSING\n'); continue; }
    var v = '?', o = '?', b = '?', z = '?';
    try { v = q.visible ? '1' : '0'; } catch (e) {}
    try { o = String(q.style.opacity); } catch (e) {}
    try { b = String(q.style.backgroundColor); } catch (e) {}
    try { z = String(q.style.zIndex); } catch (e) {}
    $.Msg('BARMINE\t' + mine[j] + '\tvis=' + v + '\topacity=' + o + '\tbg=' + b + '\tz=' + z + '\n');
  }
  $.Msg('BARPROBE\tdone\n');
})();
