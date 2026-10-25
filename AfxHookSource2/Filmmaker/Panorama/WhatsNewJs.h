#pragma once

// WHAT'S NEW -- the header button left of INVENTORY, and the changelog in CS2's own news popup.
//
// NOT a standalone script: a raw-string chunk spliced into FilmmakerGuiJs.h's IIFE (the same way
// DemoOrganizerJs.h is). It uses the host scope's `$`, `ctx`, `root3`, `host` and `api`.
//
// THE BUTTON sits in the top bar, left of INVENTORY, built like the HC TRACKER badge
// (Community/CommunityJs.h): a Button at the end of #MainMenuInput, above the drag layer that
// covers the bar, rather than a child of the bar. That is also what keeps it in the pause menu:
// during demo playback CS2 collapses the bar's whole centre group (INVENTORY ... NEWS carry
// .DisableDuringDemoPlayback), so a child of it would vanish exactly when paused. Its spot is
// fixed (wnHeaderPlace): the same in the home menu and the pause menu.
//
// C++ -> JS : host attribute "whatsnew" (ToolVersion_WhatsNewJson), then $.Filmmaker.renderWhatsNew().
//             {"label","dev","open","sections":[{"title","date","top","groups":[{"name","items"}]}]}
// JS -> C++ : "whatsnew seen" once the popup is up (it then stays shut until the next update),
//             "whatsnew changelog" for the full changelog on GitHub.
//
// THE POPUP IS CS2'S: popup_news.xml, the window CS2 opens for a new blog post (see
// new_news_entry_check.js). It brings the native frame, dim backdrop, title and date, link button,
// Close button, Escape and the open/close animation. Its body is an HTML view that loads the post's
// URL; we hand it about:blank, remove it, and draw the changelog in its place, so nothing is fetched
// and the notes come from the CHANGELOG.md that shipped with this build.
//
// It opens by itself once when `open` is set, and like CS2's news popup only on the main menu,
// never the pause menu or over a demo. It leads with the `top` sections (what changed since the
// version last seen); older releases fold under SHOW EARLIER VERSIONS.

R"FMJS(
    var whatsNew = null, wnPopup = null, wnAutoShown = false, wnEarlierOpen = false, wnMenuHooked = false;
    var WN = { title: '#ffffffff', body: '#ccccccff', muted: '#8c8c8cff', line: '#ffffff1a' };

    // The installed version as the header button and the Movie Making tooltip show it.
    function wnVersionText() {
      if (!whatsNew) return '';
      return whatsNew.dev ? 'dev build' : 'v' + whatsNew.label;
    }
    function wnLabel(parent, text, color, size) {
      var l = $.CreatePanel('Label', parent, '', { text: text });
      l.style.color = color; l.style.fontSize = size + 'px'; l.hittest = false;
      return l;
    }
    function wnParam(text) { return ('' + (text || '')).replace(/[&=]/g, ' ').trim(); }

    function wnSection(list, s, first) {
      if (!first) {
        var rule = $.CreatePanel('Panel', list, '', {});
        rule.style.width = '100%'; rule.style.height = '1px'; rule.style.backgroundColor = WN.line;
        rule.style.marginTop = '18px';
      }
      var row = $.CreatePanel('Panel', list, '', {});
      row.style.flowChildren = 'right'; row.style.width = '100%'; row.style.marginTop = first ? '6px' : '16px';
      var t = wnLabel(row, s.title, WN.title, 20); t.style.fontWeight = 'bold';
      t.style.width = 'fill-parent-flow(1.0)';
      if (s.date) { var d = wnLabel(row, s.date, WN.muted, 14); d.style.verticalAlign = 'bottom'; }
      (s.groups || []).forEach(function (g) {
        // [ GROUP ] in capitals, the way CS2's own release notes head their sections.
        if (g.name) {
          var c = wnLabel(list, '[ ' + g.name.toUpperCase() + ' ]', WN.title, 14);
          c.style.fontWeight = 'bold'; c.style.letterSpacing = '1px';
          c.style.marginTop = '12px'; c.style.marginBottom = '2px';
        }
        (g.items || []).forEach(function (text) {
          var item = $.CreatePanel('Panel', list, '', {});
          item.style.flowChildren = 'right'; item.style.width = '100%'; item.style.marginTop = '6px';
          // A drawn dot, not a bullet glyph: the menu font is not guaranteed to have one.
          var dot = $.CreatePanel('Panel', item, '', {}); dot.hittest = false;
          dot.style.width = '4px'; dot.style.height = '4px'; dot.style.borderRadius = '2px';
          dot.style.backgroundColor = WN.body; dot.style.marginTop = '8px';
          dot.style.marginLeft = '2px'; dot.style.marginRight = '10px';
          var l = wnLabel(item, text, WN.body, 15);
          l.style.width = 'fill-parent-flow(1.0)'; l.style.whiteSpace = 'normal';
        });
      });
    }
)FMJS"
R"FMJS(
    // Draws the changelog into the popup's body, in place of its web view.
    function wnFill() {
      if (!wnPopup || !wnPopup.IsValid() || !whatsNew) return false;
      var box = wnPopup.FindChildTraverse('HTMLContainer');
      if (!box) return false;
      ['BlogHTML', 'spinner'].forEach(function (id) { var p = box.FindChildTraverse(id); if (p) p.DeleteAsync(0); });
      var list = box.FindChildTraverse('FmWhatsNewList');
      if (!list) {
        list = $.CreatePanel('Panel', box, 'FmWhatsNewList', {});
        list.style.width = '100%'; list.style.height = '100%';
        list.style.flowChildren = 'down'; list.style.overflow = 'squish scroll';
        list.style.paddingLeft = '24px'; list.style.paddingRight = '24px';
        list.style.paddingTop = '6px'; list.style.paddingBottom = '18px';
      }
      list.RemoveAndDeleteChildren();
      var sections = whatsNew.sections || [], top = [], earlier = [];
      sections.forEach(function (s) { (s.top ? top : earlier).push(s); });
      if (!top.length && earlier.length) top.push(earlier.shift());
      top.forEach(function (s, i) { wnSection(list, s, i === 0); });
      if (!top.length) { var none = wnLabel(list, 'No changelog came with this build.', WN.muted, 15); none.style.marginTop = '12px'; }
      if (earlier.length) {
        var toggle = $.CreatePanel('Button', list, 'FmWhatsNewEarlier', {});
        toggle.style.marginTop = '22px'; toggle.style.paddingTop = '6px'; toggle.style.paddingBottom = '6px';
        var tl = wnLabel(toggle, (wnEarlierOpen ? 'HIDE' : 'SHOW') + ' EARLIER VERSIONS (' + earlier.length + ')', WN.muted, 14);
        tl.style.fontWeight = 'bold'; tl.style.letterSpacing = '1px';
        toggle.SetPanelEvent('onmouseover', function () { try { tl.style.color = WN.title; } catch (e) {} });
        toggle.SetPanelEvent('onmouseout', function () { try { tl.style.color = WN.muted; } catch (e) {} });
        toggle.SetPanelEvent('onactivate', function () { wnEarlierOpen = !wnEarlierOpen; wnFill(); });
        if (wnEarlierOpen) earlier.forEach(function (s) { wnSection(list, s, false); });
      }
      // The link button opened the post in the Steam overlay; here it opens the full changelog.
      var link = wnPopup.FindChildTraverse('id-news-url-button');
      if (link) link.SetPanelEvent('onactivate', function () { api.addCommand('whatsnew changelog'); });
      return true;
    }

    function whatsNewOpen() {
      if (!whatsNew) return;
      if (wnPopup && wnPopup.IsValid()) return;  // already up
      var first = null;
      (whatsNew.sections || []).forEach(function (s) { if (!first && s.top) first = s; });
      var title = whatsNew.dev ? 'What\'s new in SOURCE:MVM (dev build)' : 'What\'s new in SOURCE:MVM ' + whatsNew.label;
      wnEarlierOpen = false;
      try {
        wnPopup = UiToolkitAPI.ShowCustomLayoutPopupParameters('', 'file://{resources}/layout/popups/popup_news.xml',
          'date=' + wnParam(first && first.date).replace(/\s+/g, '') + '&title=' + wnParam(title) + '&link=about:blank');
      } catch (e) { wnPopup = null; $.Msg('[filmmaker] What\'s new popup unavailable: ' + e + '\n'); return; }
      // The layout loads with the popup; if its body is not there yet, fill it next frame.
      if (!wnFill()) $.Schedule(0, wnFill);
      api.addCommand('whatsnew seen');
      wnSeen = true; wnHeaderSync();
    }

    // Opens by itself on the main menu only, the same place CS2 shows a new blog post.
    function wnAutoOpen() {
      if (wnAutoShown || !whatsNew || !whatsNew.open) return;
      var paused = false, map = '';
      try { paused = ctx.BHasClass('MainMenuRootPanel--PauseMenuMode'); } catch (e) {}
      // At the main menu CS2 names the map "<empty>".
      try { map = GameStateAPI.GetMapBSPName() || ''; } catch (e) {}
      if (paused || (map && map.charAt(0) !== '<')) return;
      wnAutoShown = true;
      $.Schedule(0.8, whatsNewOpen);
    }

    function whatsNewRender() {
      var raw = host.GetAttributeString('whatsnew', '');
      var data = null; if (raw) { try { data = JSON.parse(raw); } catch (e) {} }
      if (!data) return;
      whatsNew = data;
      wnHeaderEnsure(); wnHeaderSync();
      if (wnPopup && wnPopup.IsValid()) wnFill();
      if (!wnMenuHooked) {
        wnMenuHooked = true;
        try { $.RegisterForUnhandledEvent('CSGOShowMainMenu', function () { $.Schedule(0.3, wnAutoOpen); }); } catch (e) {}
      }
      wnAutoOpen();
    }

    // One parseable line for automation:
    // [WHATSNEW] open=1 filled=1 rows=24 top=2 earlier=4 auto=0 btn=1 x=339 w=207 gaps=164,164 inv=710 dot=0 label=0.3.1
    // `x`/`w`: the button in layout px; `gaps`: the room left and right of it as placed; `inv`: where
    // CS2 last laid out INVENTORY's button (it keeps that while the centre group is collapsed when
    // paused, which is one more reason the spot is not derived from it).
    function whatsNewReport() {
      var sections = (whatsNew && whatsNew.sections) || [], top = 0;
      sections.forEach(function (s) { if (s.top) ++top; });
      var open = wnPopup && wnPopup.IsValid() ? 1 : 0;
      var btn = wnHeader && wnHeader.IsValid() ? wnHeader : null;
      var list = open ? wnPopup.FindChildTraverse('FmWhatsNewList') : null;
      var uis = btn ? (btn.actualuiscale_x || 1) : 1;
      var pos = btn && btn.GetPositionWithinWindow ? btn.GetPositionWithinWindow() : null;
      var inv = root3.FindChildTraverse('MainMenuNavBarInventory');
      var invPos = inv && (inv.actuallayoutwidth || 0) > 0 && inv.GetPositionWithinWindow ? inv.GetPositionWithinWindow() : null;
      $.Msg('[WHATSNEW] open=' + open + ' filled=' + (list ? 1 : 0) + ' rows=' + (list ? list.GetChildCount() : -1) +
        ' top=' + top + ' earlier=' + (sections.length - top) +
        ' auto=' + (whatsNew && whatsNew.open ? 1 : 0) + ' btn=' + (btn ? 1 : 0) +
        ' x=' + (pos ? Math.round(pos.x / uis) : -1) + ' w=' + (btn ? Math.round((btn.actuallayoutwidth || 0) / uis) : -1) +
        ' gaps=' + (wnGaps ? wnGaps.join(',') : '-') + ' inv=' + (invPos ? Math.round(invPos.x / uis) : -1) +
        ' dot=' + (wnDot && wnDot.IsValid() && wnDot.visible ? 1 : 0) +
        ' label=' + (whatsNew ? ('' + whatsNew.label).replace(/\s+/g, '_') : '-') + '\n');
    }
)FMJS"
R"FMJS(
    // ---- the header button ---------------------------------------------------------------------
    // HC TRACKER's look (CommunityJs.h): a dark pill with a thin border in CS2's condensed type, at the
    // bar's height. A gold dot marks notes not opened yet; the version rides next to the words.
    var wnHeader = null, wnHeaderWord = null, wnHeaderVersion = null, wnDot = null;
    var wnSeen = false, wnGaps = null, wnPlaceLoop = 0;
    var WNH = { bg: 'rgba(10,10,11,0.62)', border: 'rgba(255,255,255,0.22)', gold: '#f0b323', text: '#f0eeeb', muted: '#9aa3ad' };

    function wnHeaderEnsure() {
      if (wnHeader && wnHeader.IsValid()) return wnHeader;
      var old = root3.FindChildTraverse('FmWhatsNewHeader'); if (old) old.DeleteAsync(0);
      var layer = root3.FindChildTraverse('MainMenuInput') || ctx;
      var b = $.CreatePanel('Button', layer, 'FmWhatsNewHeader', {});
      var s = b.style;
      // 12 / 13px: the icon's ink sits 2px inside its box, so both ends read 15px from the border.
      s.flowChildren = 'right'; s.height = '46px'; s.paddingLeft = '12px'; s.paddingRight = '13px';
      s.horizontalAlign = 'left'; s.verticalAlign = 'top'; s.marginTop = '9px'; s.zIndex = '10';
      s.backgroundColor = WNH.bg; s.border = '1px solid ' + WNH.border; s.borderRadius = '3px';
      s.transitionProperty = 'background-color, border, box-shadow'; s.transitionDuration = '0.15s';
      var icon = $.CreatePanel('Image', b, '', { src: 's2r://panorama/images/icons/ui/info.vsvg' });
      icon.style.width = '22px'; icon.style.height = '22px'; icon.style.verticalAlign = 'center';
      icon.style.marginRight = '10px'; icon.style.washColor = WNH.gold; icon.hittest = false;
      wnHeaderWord = $.CreatePanel('Label', b, '', { text: 'WHAT\'S NEW' });
      wnHeaderWord.AddClass('stratum-medium-condensed'); wnHeaderWord.hittest = false;
      wnHeaderWord.style.fontSize = '26px'; wnHeaderWord.style.color = WNH.text; wnHeaderWord.style.letterSpacing = '1px';
      wnHeaderWord.style.verticalAlign = 'center'; wnHeaderWord.style.transform = 'translateY(2px)';
      wnHeaderVersion = $.CreatePanel('Label', b, '', { text: '' });
      wnHeaderVersion.AddClass('stratum-regular-condensed'); wnHeaderVersion.hittest = false;
      wnHeaderVersion.style.fontSize = '18px'; wnHeaderVersion.style.color = WNH.muted;
      wnHeaderVersion.style.marginLeft = '10px'; wnHeaderVersion.style.verticalAlign = 'center';
      wnHeaderVersion.style.transform = 'translateY(2px)';
      wnDot = $.CreatePanel('Panel', b, '', {}); wnDot.hittest = false; wnDot.visible = false;
      wnDot.style.width = '8px'; wnDot.style.height = '8px'; wnDot.style.borderRadius = '4px';
      wnDot.style.backgroundColor = WNH.gold; wnDot.style.marginLeft = '8px'; wnDot.style.verticalAlign = 'center';
      b.SetPanelEvent('onmouseover', function () {
        b.style.backgroundColor = 'rgba(240,179,35,0.14)'; b.style.border = '1px solid ' + WNH.gold;
        b.style.boxShadow = 'rgba(240,179,35,0.40) 0px 0px 12px 0px'; wnHeaderWord.style.color = WNH.gold;
        try { UiToolkitAPI.ShowTextTooltip('FmWhatsNewHeader', 'What changed in SOURCE:MVM ' + wnVersionText()); } catch (e) {}
      });
      b.SetPanelEvent('onmouseout', function () {
        b.style.backgroundColor = WNH.bg; b.style.border = '1px solid ' + WNH.border;
        b.style.boxShadow = 'rgba(0,0,0,0) 0px 0px 0px 0px'; wnHeaderWord.style.color = WNH.text;
        try { UiToolkitAPI.HideTextTooltip(); } catch (e) {}
      });
      b.SetPanelEvent('onactivate', function () {
        try { $.DispatchEvent('CSGOPlaySoundEffect', 'UIPanorama.generic_button_press', 'MOUSE'); } catch (e) {}
        whatsNewOpen();
      });
      wnHeader = b;
      // One placement loop per button: a rebuilt button ends the previous one.
      var loop = ++wnPlaceLoop;
      (function tick() {
        if (loop !== wnPlaceLoop || !b.IsValid()) return;
        wnHeaderPlace();
        $.Schedule(0.1, tick);
      })();
      return b;
    }

    function wnHeaderSync() {
      if (!wnHeader || !wnHeader.IsValid()) return;
      wnHeaderVersion.text = wnVersionText();
      wnDot.visible = !!(whatsNew && whatsNew.open && !wnSeen);
    }

    // ONE SPOT for the main menu and the pause menu: centred in the bar's stretch left of INVENTORY
    // (mvmHeaderSlot, HeaderLayoutJs.h), the mirror of HC TRACKER right of NEWS. At 1920 wide it
    // spans 341 to 545, 166px clear on both sides. On a narrow screen (4:3) the version is dropped
    // before the gaps would close up.
    var WN_MIN_GAP = 28;
    function wnHeaderPlace() {
      if (!wnHeader || !wnHeader.IsValid()) return;
      var uis = wnHeader.actualuiscale_x || 1;
      var w = (wnHeader.actuallayoutwidth || 0) / uis;
      if (w <= 0) return;   // not laid out yet
      var slot = mvmHeaderSlot(-1, w, mvmHeaderBarWidth(root3));
      if (slot.before < WN_MIN_GAP && wnHeaderVersion.visible) { wnHeaderVersion.visible = false; return; }
      wnGaps = [Math.round(slot.before), Math.round(slot.after)];
      wnHeader.style.marginLeft = Math.round(slot.x) + 'px';
    }
)FMJS"
