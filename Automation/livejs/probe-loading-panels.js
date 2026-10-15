// Read-only snapshot of native loading panels. Run once in each context (hud/menu) after a
// demo switch; pair with health, mvm_debug and a window screenshot. It never dismisses panels.
// mirv_filmmaker livejs context hud
// mirv_filmmaker livejs load "<absolute path to this file>"
(function () {
  var seen = 0, matches = 0, limited = false;
  try {
    var top = $.GetContextPanel();
    for (var i = 0; i < 32; i++) {
      var parent = top.GetParent();
      if (!parent) break;
      top = parent;
    }
    function walk(p, depth, ancestorsVisible) {
      if (!p || depth > 48 || seen >= 6000) { limited = true; return; }
      seen++;
      var visible = ancestorsVisible && p.visible !== false;
      // .visible is the explicit flag; layout dimensions and ancestor flags are reported
      // separately because CSS can collapse a panel whose explicit flag is still true.
      if (/loading|download|loadscreen|level-load/i.test(p.id || '')) {
        matches++;
        $.Msg('LOADPANEL ' + JSON.stringify({id: p.id, type: p.paneltype,
          visible: p.visible, ancestorsVisible: ancestorsVisible,
          width: p.actuallayoutwidth, height: p.actuallayoutheight}) + '\n');
      }
      for (var j = 0; j < p.GetChildCount(); j++) walk(p.GetChild(j), depth + 1, visible);
    }
    walk(top, 0, true);
    $.Msg('LOADPANELS ' + JSON.stringify({root: top.id, visited: seen,
      matches: matches, truncated: limited}) + '\n');
  } catch (e) { $.Msg('LOADPANELS_ERROR ' + String(e) + '\n'); }
})();
