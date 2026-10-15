// Is Deadlock's own replay transport COLLAPSED, or merely drawing nothing?
//
// The panel tree says PlayButton is visible at a sensible rectangle and the game's own stylesheet
// gives it `background-color: #576886aa`, yet nothing appears -- while OUR buttons, appended to the
// same row, do. `visible` cannot tell those apart: it reports the inline flag, and a class-driven
// `visibility: collapse` does not touch it.
//
// So this paints the game's own controls an unmissable colour. If they appear, the subtree is live
// and the problem is the game's own styling or its textures; if they stay invisible, something up
// the chain is collapsing them and the rectangle in the dump is a lie.
//
// Run with: mirv_deadlock livejs load <abs path to this file>

(function () {
  function find(id) {
    var root = $.GetContextPanel();
    if (!root) return null;
    try { return root.FindChildTraverse(id); } catch (e) { return null; }
  }

  var probes = [
    ['PlaybackControls', '#ff00ffcc'],
    ['PlayButton', '#00ff00ff'],
    ['ReplayScrubber', '#ffaa00cc'],
    ['MainPlaybackControls', '#0000ffaa']
  ];

  for (var i = 0; i < probes.length; ++i) {
    var p = find(probes[i][0]);
    if (!p) { $.Msg('PAINT\t' + probes[i][0] + '\tMISSING\n'); continue; }
    try {
      p.style.backgroundColor = probes[i][1];
      p.style.visibility = 'visible';
      p.visible = true;
      $.Msg('PAINT\t' + probes[i][0] + '\tpainted ' + probes[i][1] + '\n');
    } catch (e) {
      $.Msg('PAINT\t' + probes[i][0] + '\tTHREW\n');
    }
  }

  // And the classes that could be collapsing it, asked by name because Panorama has no class list.
  var suspects = ['hud_replay', 'SpectatorOptionsContainer', 'PlaybackControls', 'MainPlaybackControls'];
  var classes = ['Hidden', 'Collapsed', 'Inactive', 'Disabled', 'NoReplay',
                 'ShowExpandedPlaybackControls', 'Minimized'];
  for (var j = 0; j < suspects.length; ++j) {
    var q = find(suspects[j]);
    if (!q) continue;
    var on = [];
    for (var k = 0; k < classes.length; ++k) {
      try { if (q.BHasClass(classes[k])) on.push(classes[k]); } catch (e) {}
    }
    $.Msg('CLASSES\t' + suspects[j] + '\t' + (on.length ? on.join(',') : '(none of the suspects)') + '\n');
  }
  $.Msg('PAINT\tdone\n');
})();
