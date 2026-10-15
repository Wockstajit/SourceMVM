// Read-only live Panorama enumeration. Run through `mirv_filmmaker livejs load <path>`.
// Select `mirv_filmmaker livejs context menu` or `hud` first; inspect both during transitions.
// Output uses one JSON object per [audit.eggs] line. It does not activate effects or move input.
(function () {
  var ctx = $.GetContextPanel();
  function info(p) {
    if (!p) return null;
    return { id: p.id, type: p.paneltype, visible: p.visible,
      width: p.actuallayoutwidth, height: p.actuallayoutheight,
      opacity: p.style.opacity, classes: p.GetClasses ? p.GetClasses() : [] };
  }
  function report(kind, value) {
    $.Msg('[audit.eggs] ' + JSON.stringify({ kind: kind, value: value }) + '\n');
  }
  report('context', info(ctx));
  var stack = [{ panel: ctx, depth: 0 }], roots = [], visited = 0, truncated = false;
  while (stack.length && visited < 8192) {
    var item = stack.pop(), panel = item.panel;
    visited++;
    if (panel.id === 'MvmEggRoot') roots.push(panel);
    var children = panel.Children();
    if (item.depth >= 64) { if (children.length) truncated = true; continue; }
    for (var i = 0; i < children.length; ++i)
      stack.push({ panel: children[i], depth: item.depth + 1 });
  }
  report('inventory', { roots: roots.length, visited: visited, truncated: truncated || stack.length > 0 });
  for (var r = 0; r < roots.length; ++r) {
    var root = roots[r];
    report('root', info(root));
    report('state', root.GetAttributeString('state', '{}'));
    var p = root;
    for (var n = 0; p && n < 12; ++n, p = p.GetParent()) report('ancestor', info(p));
    ['MvmEggVegas', 'MvmEggVegasMovie', 'MvmEggClippy'].forEach(function (id) {
      report(id, info(root.FindChildTraverse(id)));
    });
    var hints = root.FindChildTraverse('MvmEggHints');
    if (hints) report('hints', { boxes: hints.Children().length,
      contents: hints.Children().slice(0, 8).map(function (box) {
        return { panel: info(box), labels: box.Children().slice(0, 16).map(function (p) {
          return { type: p.paneltype, text: p.paneltype === 'Label' ? p.text : '',
            opacity: p.style.opacity, transform: p.style.transform };
        }) };
      }) });
  }
})();
