// Step 1 of capturing the game's real state feed: wrap the workspace's render() so the next frame's
// state object is kept where a second probe can print it.
//
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-stash-state.js
//   (one frame later)
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-dump-ui-state.js
//
// Why two steps: RunScript has no return value and the state is a call ARGUMENT to render(), not
// something the panel keeps. Wrapping once and reading back a frame later is the whole trick, and
// it works on a running build with no rebuild. Idempotent -- loading it twice wraps once.
(function () {
  try {
    if (!$.MvmWorkspace || $.MvmWorkspace.__mvmStashed) { $.Msg('STASH\talready\n'); return; }
    var real = $.MvmWorkspace.render;
    $.MvmWorkspace.render = function (st) {
      try { $.__mvmLastState = st; } catch (e) {}
      return real(st);
    };
    $.MvmWorkspace.__mvmStashed = true;
    $.Msg('STASH\tarmed\n');
  } catch (e) {
    $.Msg('STASH\terror\t' + e + '\n');
  }
})();
