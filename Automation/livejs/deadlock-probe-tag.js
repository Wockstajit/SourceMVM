// The preview tag ("● LIVE" / "● PREVIEW" at the top-left of the frame) is an EMPTY 18px pill in
// game: the geometry probe reports its dot and label with FLT_MAX offsets, i.e. never laid out, and
// the screenshot shows a blank box. The mockup lays them out. This prints what each child actually
// is, so the cause is read rather than guessed.
//
//   mirv_deadlock livejs load <abs>/Automation/livejs/deadlock-probe-tag.js
(function () {
  var TAB = String.fromCharCode(9);
  var ctx = $.GetContextPanel();
  var tag = ctx.FindChildTraverse('MvmPreviewTag');
  if (!tag) { $.Msg('TAG' + TAB + 'missing\n'); return; }
  function dump(label, p) {
    var s = label;
    try { s += ' visible=' + p.visible; } catch (e) { s += ' visible=ERR'; }
    try { s += ' w=' + p.actuallayoutwidth + ' h=' + p.actuallayoutheight; } catch (e) {}
    try { s += ' x=' + p.actualxoffset + ' y=' + p.actualyoffset; } catch (e) {}
    try { s += ' desired=' + p.desiredlayoutwidth + 'x' + p.desiredlayoutheight; } catch (e) {}
    try { s += ' content=' + p.contentwidth + 'x' + p.contentheight; } catch (e) {}
    try { s += ' text="' + (p.text === undefined ? '' : p.text) + '"'; } catch (e) {}
    try { s += ' style.width=' + p.style.width + ' style.height=' + p.style.height + ' fontSize=' + p.style.fontSize + ' opacity=' + p.style.opacity + ' visibility=' + p.style.visibility; } catch (e) {}
    try { s += ' flow=' + p.style.flowChildren; } catch (e) {}
    $.Msg('TAG' + TAB + s + '\n');
  }
  dump('tag', tag);
  var n = tag.GetChildCount();
  for (var i = 0; i < n; ++i) dump('child' + i, tag.GetChild(i));
  // Compare against a label that DOES lay out: the header's shot label, built the same way.
  var ref = ctx.FindChildTraverse('MvmHeaderShot');
  if (ref) dump('ref MvmHeaderShot', ref);
})();
