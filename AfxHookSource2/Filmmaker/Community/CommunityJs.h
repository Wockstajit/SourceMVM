#pragma once

// Panorama JS for Community/CommunityHud (main-menu context, so it also shows over the in-game
// pause menu). Fragment 1: the namespace, the C++ bridge host, the HC Tracker badge and the Report
// Bugs button. Fragment 2: the report dialog (write, review, then send). Fragment 3: the crash
// prompt. Fragment 4: live HCs. They share $.MvmCommunity.
//
// Look: CS2's own type (the stratum-* classes from csgostyles, which the main menu loads) at the
// nav bar's scale, so the additions read as part of the menu rather than pasted on top of it.
// The badge is centred in the top bar's empty stretch right of NEWS, the mirror of WHAT'S NEW left
// of INVENTORY (Panorama/HeaderLayoutJs.h, spliced in below). Its eye is HC Tracker's logo, loaded
// by URL (Panorama shows web images, as CS2's news page does); without a network the wordmark
// still reads.

namespace Filmmaker {

inline const char* kCommunityJs = R"CMJS(
(function () {
  ['MvmCommunityHost', 'MvmHcTracker', 'MvmReportBugsBtn', 'MvmReportDialog', 'MvmCrashPrompt'].forEach(function (id) {
    var old = $.GetContextPanel().FindChildTraverse(id); if (old) old.DeleteAsync(0);
  });
  function topRoot(p) { while (p && p.GetParent && p.GetParent()) p = p.GetParent(); return p; }
  var root = topRoot($.GetContextPanel());
  var C = $.MvmCommunity = {};
  C.HC_YELLOW = '#f5b719'; C.GOLD = '#f0b323';
  // HC Tracker's eye, compiled into our FX pack (FX/ui -> FX/compiled/.../panorama/images/sourcemvm).
  // The web copy is the fallback for a build whose FX pack predates it.
  C.LOGO = 's2r://panorama/images/sourcemvm/hctracker_eye_png.vtex';
  C.LOGO_WEB = 'https://raw.githubusercontent.com/Wockstajit/SourceMVM/main/Assets/hctracker/hctracker-eye.png';

  var host = $.CreatePanel('Panel', $.GetContextPanel(), 'MvmCommunityHost', {});
  host.visible = false; host.style.width = '0px'; host.style.height = '0px';
  C.cmd = function (line) {
    var q = host.GetAttributeString('cmd', '');
    host.SetAttributeString('cmd', q ? q + '\n' + line : line);
  };
  C.state = {};
  C.onState = function () {
    try { C.state = JSON.parse(host.GetAttributeString('state', '{}')); } catch (e) { C.state = {}; }
    if (C.renderDialog) C.renderDialog();
    if (C.renderCrash) C.renderCrash();
    if (C.renderHc) C.renderHc();
  };
  // A Label in CS2's own type. cls: a stratum-* class from csgostyles.
  C.text = function (parent, text, cls, size, color) {
    var l = $.CreatePanel('Label', parent, '', { text: text });
    l.AddClass(cls); l.style.fontSize = size + 'px'; l.style.color = color; l.hittest = false;
    return l;
  };
  C.events = function (p, over, out) { p.SetPanelEvent('onmouseover', over); p.SetPanelEvent('onmouseout', out); };
  C.tip = function (p, text, on) {
    try { if (on) UiToolkitAPI.ShowTextTooltip(p.id, text); else UiToolkitAPI.HideTextTooltip(); } catch (e) {}
  };
  C.sound = function () { try { $.DispatchEvent('CSGOPlaySoundEffect', 'UIPanorama.generic_button_press', 'MOUSE'); } catch (e) {} };
)CMJS"
// ---- the top bar's fixed layout, shared with WHAT'S NEW (Panorama/FilmmakerGuiJs.h) ----
#include "../Panorama/HeaderLayoutJs.h"
R"CMJS(

  // ---- HC Tracker badge (top bar, right of NEWS) --------------------------------------------------
  function findRightContainer() {
    var bar = root.FindChildTraverse('JsMainMenuTopNavBar');
    if (!bar) return null;
    for (var i = 0; i < bar.GetChildCount(); ++i) {
      var c = bar.GetChild(i);
      if (c && c.BHasClass && c.BHasClass('mainmenu-top-navbar__right-container')) return c;
    }
    return bar;
  }
  var right = findRightContainer();
  var layer = root.FindChildTraverse('MainMenuInput') || $.GetContextPanel();
  if (right) {
    // NOT inside the top bar: a later z-index-4 layer of #MainMenuInput (#main-menu-drag-panel's)
    // covers the bar's empty stretch and took the mouse, so the badge never saw a hover or click.
    // It lives at the end of #MainMenuInput instead, above every menu layer, placed where the
    // bar's right-hand container would put it (vertically centred in the 64px bar).
    var badge = $.CreatePanel('Button', layer, 'MvmHcTracker', {});
    var s = badge.style;
    // Centred in the bar's empty stretch right of NEWS (mvmHeaderSlot): the same room before and
    // after it, and the same spot in the home menu and the pause menu. At 1920 wide it spans about
    // 1437 to 1627. The top bar draws ABOVE the friends bar, so when the bar slides open the badge
    // fades out instead (watchLayers below), the way the native icons under the bar disappear.
    s.flowChildren = 'right'; s.height = '46px'; s.padding = '0px 18px 0px 12px';
    s.horizontalAlign = 'left'; s.verticalAlign = 'top'; s.marginTop = '9px'; s.zIndex = '10';
    // A first guess at its stock 189px width, so it never shows at the bar's left edge first.
    s.marginLeft = Math.round(mvmHeaderSlot(1, 189, mvmHeaderBarWidth(root)).x) + 'px';
    s.backgroundColor = 'rgba(10,10,11,0.62)'; s.border = '1px solid rgba(245,183,25,0.55)';
    s.borderRadius = '3px';
    s.transitionProperty = 'background-color, box-shadow, border, opacity'; s.transitionDuration = '0.15s';
    var eye = $.CreatePanel('Image', badge, 'MvmHcTrackerEye', { scaling: 'stretch-to-fit-preserve-aspect' });
    eye.style.width = '50px'; eye.style.height = '27px'; eye.style.verticalAlign = 'center';
    eye.style.marginRight = '10px'; eye.hittest = false;
    var triedWeb = false;
    $.RegisterEventHandler('ImageFailedLoad', eye, function () {
      if (!triedWeb) { triedWeb = true; eye.SetImage(C.LOGO_WEB); } else eye.visible = false;
    });
    eye.SetImage(C.LOGO);
    var word = C.text(badge, 'HC TRACKER', 'stratum-medium-condensed', 26, '#f0eeeb');
    word.style.verticalAlign = 'center'; word.style.letterSpacing = '1px'; word.style.transform = 'translateY(2px)';
    C.events(badge, function () {
      badge.style.backgroundColor = 'rgba(245,183,25,0.16)'; badge.style.border = '1px solid ' + C.HC_YELLOW;
      badge.style.boxShadow = 'rgba(245,183,25,0.45) 0px 0px 12px 0px'; word.style.color = C.HC_YELLOW;
      if (C.hcHover) C.hcHover(true); else C.tip(badge, 'Open HC Tracker in your browser', true);
    }, function () {
      badge.style.backgroundColor = 'rgba(10,10,11,0.62)'; badge.style.border = '1px solid rgba(245,183,25,0.55)';
      badge.style.boxShadow = 'rgba(0,0,0,0) 0px 0px 0px 0px'; word.style.color = '#f0eeeb';
      if (C.hcHover) C.hcHover(false); else C.tip(badge, '', false);
    });
    badge.SetPanelEvent('onactivate', function () { C.sound(); C.cmd('hctracker'); });
    C.badge = badge;
    // Its real width once laid out (the eye image and the wordmark), then kept there.
    (function placeBadge() {
      if (!badge.IsValid()) return;
      var w = (badge.actuallayoutwidth || 0) / (badge.actualuiscale_x || 1);
      if (w > 0) {
        C.badgeSlot = mvmHeaderSlot(1, w, mvmHeaderBarWidth(root));
        badge.style.marginLeft = Math.round(C.badgeSlot.x) + 'px';
      }
      $.Schedule(0.1, placeBadge);
    })();
  }

  // ---- Report a bug (bottom right, left of the friends bar's round buttons) -----------------------
  var input = root.FindChildTraverse('MainMenuInput') || $.GetContextPanel();
  var bug = $.CreatePanel('Button', input, 'MvmReportBugsBtn', {});
  var b = bug.style;
  b.flowChildren = 'right'; b.height = '54px'; b.padding = '0px 28px 0px 22px'; b.borderRadius = '27px';
  b.horizontalAlign = 'right'; b.verticalAlign = 'bottom'; b.marginRight = '170px'; b.marginBottom = '20px';
  b.backgroundColor = 'rgba(0,0,0,0.5)'; b.border = '1px solid rgba(255,255,255,0.16)';
  b.transitionProperty = 'background-color, border'; b.transitionDuration = '0.15s';
  var bugIcon = $.CreatePanel('Image', bug, '', { src: 's2r://panorama/images/icons/ui/warning.vsvg' });
  bugIcon.style.width = '24px'; bugIcon.style.height = '24px'; bugIcon.style.verticalAlign = 'center';
  bugIcon.style.marginRight = '12px'; bugIcon.style.washColor = '#ff9a7a'; bugIcon.hittest = false;
  var bugText = C.text(bug, 'REPORT A BUG', 'stratum-medium-condensed', 30, '#eef2f6');
  bugText.style.verticalAlign = 'center'; bugText.style.letterSpacing = '1px'; bugText.style.transform = 'translateY(2px)';
  C.events(bug, function () {
    bug.style.backgroundColor = 'rgba(255,255,255,0.12)'; bug.style.border = '1px solid rgba(255,154,122,0.8)';
    C.tip(bug, 'Write a bug report for the SOURCE:MVM developer', true);
  }, function () {
    bug.style.backgroundColor = 'rgba(0,0,0,0.5)'; bug.style.border = '1px solid rgba(255,255,255,0.16)';
    C.tip(bug, '', false);
  });
  bug.SetPanelEvent('onactivate', function () { C.sound(); if (C.openDialog) C.openDialog(); });
  var friends = root.FindChildTraverse('MainMenuFriendsAndParty');
  bug.style.zIndex = '10'; // above the drag layer, like the badge (it fades for the friends bar)
  C.friends = friends;
  // Home screen and pause menu only: a page (Demos, Inventory, ...) fills the screen and has its own
  // buttons in that corner. CS2 marks an open page with .mainmenu-content--open (mainmenu.js).
  // The friends bar is open while #JsMainMenuSidebar lacks .mainmenu-sidebar--minimized
  // (MainMenu.ExpandSidebar / MinimizeSidebar); the badge, drawn above it, fades with its slide.
  var menu = $.GetContextPanel(), sidebar = root.FindChildTraverse('JsMainMenuSidebar');
  C.sidebarOpen = false;
  function sidebarOpenNow() {
    // The menu's own panel tree first (the context root), then ours.
    if (!sidebar || !sidebar.IsValid()) sidebar = menu.FindChildTraverse('JsMainMenuSidebar') || root.FindChildTraverse('JsMainMenuSidebar');
    return !!(sidebar && sidebar.IsValid() && !sidebar.BHasClass('mainmenu-sidebar--minimized'));
  }
  // CS2 announces the slide itself (mainmenu.js MinimizeSidebar/ExpandSidebar dispatch it).
  $.RegisterForUnhandledEvent('SidebarIsCollapsed', function (collapsed) { applySide(!collapsed); });
  (function watchLayers() {
    if (!bug.IsValid()) return;
    var open = menu.BHasClass('mainmenu-content--open');
    if (bug.visible === open) bug.visible = !open;
    applySide(sidebarOpenNow());
    $.Schedule(0.1, watchLayers);
  })();
  function applySide(sideOpen) {
    if (!bug.IsValid()) return;
    if (sideOpen !== C.sidebarOpen) {
      C.sidebarOpen = sideOpen;
      // Both fade with the bar's 0.25s slide. Draw order cannot put them under it: the friends
      // bar is drawn below the menu's own layers (tested: moving the button before
      // #MainMenuFriendsAndParty did not), so they step aside instead, as if covered.
      var fade = [C.badge, bug];
      for (var f = 0; f < fade.length; ++f) {
        var p = fade[f];
        if (!p || !p.IsValid()) continue;
        p.style.transitionProperty = 'opacity, background-color, box-shadow, border';
        p.style.transitionDuration = '0.25s';
        p.style.opacity = sideOpen ? '0' : '1';
        p.hittest = !sideOpen;
      }
      if (C.hcHover && sideOpen) C.hcHover(false, true);
    }
  }
  C.root = root; C.input = input;
  C.report = function () {
    var slot = C.badgeSlot;
    return 'badge=' + !!$.GetContextPanel().FindChildTraverse('MvmHcTracker') + ' bug=' + !!bug + ' right=' + !!right +
      ' badgeX=' + (slot ? Math.round(slot.x) : -1) + ' gaps=' + (slot ? Math.round(slot.before) + ',' + Math.round(slot.after) : '-') +
      ' sidebar=' + !!(sidebar && sidebar.IsValid()) + ' sideOpen=' + C.sidebarOpen +
      ' minimized=' + !!(sidebar && sidebar.IsValid() && sidebar.BHasClass('mainmenu-sidebar--minimized'));
  };
})();
)CMJS";

inline const char* kCommunityReportJs = R"CMJS(
(function () {
  var C = $.MvmCommunity; if (!C) return;
  var dlg = null, form = null, review = null, done = null, entry = null, contact = null, saveBtn = null;
  var reviewBody = null, doneTitle = null, doneBody = null, issuesBtn = null, showBtn = null;
  var openBtn = null, keepBtn = null, sendBtn = null;
  // pendingSeq: the report seq before "Review" was pressed (-1: none). reviewSeq: the written report
  // the player is looking at. sendAsked/sawSending: Send was pressed and the send has started.
  var pendingSeq = -1, reviewSeq = -1, sendAsked = false, sawSending = false;
  // What a report holds, said before anything is written and again before it is sent.
  var HOLDS = 'what you wrote, your Discord name if you gave one, your CS2 and SOURCE:MVM builds, Windows version, ' +
    'map and demo names, and recent game logs. It has no Steam name or ID.';

  function caption(parent, text) {
    var l = C.text(parent, text, 'stratum-medium-condensed', 17, '#8b95a1');
    l.style.letterSpacing = '1px'; l.style.marginTop = '18px'; l.style.marginBottom = '6px';
    return l;
  }
  // The TextEntry itself is the box: inside the main menu CS2 already styles a TextEntry with its
  // own frame and padding, and a wrapper of ours doubled it (the one-line field was crushed).
  function field(parent, id, height, attrs) {
    var t = $.CreatePanel('TextEntry', parent, id, attrs);
    t.AddClass('stratum-regular'); t.style.width = '100%'; t.style.height = height + 'px';
    t.style.fontSize = '18px'; t.style.color = '#ffffff';
    return t;
  }
  function button(parent, id, text, primary, onClick) {
    var p = $.CreatePanel('Button', parent, id, {});
    var base = primary ? C.GOLD : 'rgba(255,255,255,0.07)', hot = primary ? '#ffc94a' : 'rgba(255,255,255,0.16)';
    p.style.padding = '9px 22px 7px 22px'; p.style.marginLeft = '10px'; p.style.borderRadius = '2px';
    p.style.backgroundColor = base;
    var t = C.text(p, text, 'stratum-medium-condensed', 21, primary ? '#181206' : '#e8ecf2');
    t.style.letterSpacing = '1px';
    p.SetPanelEvent('onmouseover', function () { p.style.backgroundColor = hot; });
    p.SetPanelEvent('onmouseout', function () { p.style.backgroundColor = base; });
    p.SetPanelEvent('onactivate', function () { C.sound(); onClick(); });
    p.__label = t; return p;
  }
  function row(parent) {
    var r = $.CreatePanel('Panel', parent, '', {});
    r.style.flowChildren = 'right'; r.style.horizontalAlign = 'right'; r.style.marginTop = '20px';
    return r;
  }
  function body(parent) {
    var t = C.text(parent, '', 'stratum-regular', 17, '#c9d1dc');
    t.style.width = '100%'; t.style.marginTop = '8px';
    return t;
  }
  C.button = button; C.row = row; C.body = body;

  function build() {
    // Inside the main menu (not the top root), so its stylesheet -- and the stratum-* type -- apply.
    dlg = $.CreatePanel('Panel', C.input, 'MvmReportDialog', {});
    dlg.style.width = '100%'; dlg.style.height = '100%'; dlg.style.zIndex = '1000';
    dlg.style.backgroundColor = 'rgba(0,0,0,0.72)';
    dlg.SetPanelEvent('onactivate', function () {});
    var card = $.CreatePanel('Panel', dlg, '', {});
    card.style.width = '660px'; card.style.flowChildren = 'down'; card.style.padding = '26px 32px 28px 32px';
    card.style.horizontalAlign = 'center'; card.style.verticalAlign = 'center';
    card.style.backgroundColor = 'gradient(linear, 0% 0%, 0% 100%, from(#262b33), to(#14171c))';
    card.style.borderTop = '3px solid ' + C.GOLD; card.style.boxShadow = 'rgba(0,0,0,0.7) 0px 12px 48px 0px';
    var title = C.text(card, 'REPORT A BUG', 'stratum-medium-condensed', 36, '#ffffff');
    title.style.letterSpacing = '1px';

    form = $.CreatePanel('Panel', card, '', {});
    form.style.flowChildren = 'down'; form.style.width = '100%';
    var intro = C.text(form, 'Tell us what went wrong and what you were doing. You see the report before anything is sent.', 'stratum-regular', 17, '#a9b3bf');
    intro.style.width = '100%'; intro.style.marginTop = '4px';
    caption(form, 'WHAT HAPPENED?');
    entry = field(form, 'MvmReportText', 150, { multiline: 'true', maxchars: '4000', placeholder: 'Example: the fire effect disappears after I scrub the demo back' });
    caption(form, 'YOUR DISCORD (OPTIONAL, SO WE CAN REPLY)');
    contact = field(form, 'MvmReportContact', 40, { maxchars: '64', placeholder: 'your Discord username' });
    var holds = C.text(form, 'The report holds ' + HOLDS, 'stratum-regular', 14, '#6f7a86');
    holds.style.width = '100%'; holds.style.marginTop = '14px';
    var actions = row(form);
    button(actions, 'MvmReportCancel', 'CANCEL', false, function () { C.closeDialog(); });
    saveBtn = button(actions, 'MvmReportSave', 'REVIEW REPORT', true, function () {
      if (pendingSeq >= 0) return;
      pendingSeq = (C.state.report && C.state.report.seq) || 0;
      C.cmd('bugreport ' + encodeURIComponent(entry.text || '-') + ' ' + encodeURIComponent(contact.text || '-'));
      saveBtn.__label.text = 'SAVING...';
    });

    // Step 2: the report is on this PC. Nothing leaves it until Send.
    review = $.CreatePanel('Panel', card, '', {});
    review.style.flowChildren = 'down'; review.style.width = '100%';
    C.text(review, 'CHECK IT BEFORE IT IS SENT', 'stratum-medium-condensed', 24, C.GOLD).style.marginTop = '10px';
    reviewBody = body(review);
    var reviewActions = row(review);
    openBtn = button(reviewActions, 'MvmReportOpen', 'SHOW FILE', false, function () { C.cmd('showreport'); });
    keepBtn = button(reviewActions, 'MvmReportKeep', "DON'T SEND", false, function () { C.closeDialog(); });
    sendBtn = button(reviewActions, 'MvmReportSend', 'SEND', true, function () {
      if (sendAsked) return;
      sendAsked = true; sawSending = false;
      C.cmd('sendreport ' + reviewSeq);
    });

    done = $.CreatePanel('Panel', card, '', {});
    done.style.flowChildren = 'down'; done.style.width = '100%';
    doneTitle = C.text(done, '', 'stratum-medium-condensed', 24, '#7fe0a0');
    doneTitle.style.letterSpacing = '1px'; doneTitle.style.marginTop = '10px';
    doneBody = body(done);
    var actions2 = row(done);
    issuesBtn = button(actions2, 'MvmReportIssues', 'OPEN GITHUB ISSUES', false, function () { C.cmd('issues'); });
    showBtn = button(actions2, 'MvmReportShow', 'SHOW FILE', false, function () { C.cmd('showreport'); });
    button(actions2, 'MvmReportDone', 'DONE', true, function () { C.closeDialog(); });
  }

  function step(name) { form.visible = name === 'form'; review.visible = name === 'review'; done.visible = name === 'done'; }

  C.renderDialog = function () {
    if (!dlg || !dlg.IsValid() || !dlg.visible) return;
    var st = C.state || {}, r = st.report || {};
    var off = st.send === 'off';
    var written = pendingSeq >= 0 && (r.seq || 0) > pendingSeq;
    if (!written) { step('form'); saveBtn.__label.text = pendingSeq >= 0 ? 'SAVING...' : (off ? 'SAVE REPORT' : 'REVIEW REPORT'); return; }
    reviewSeq = r.seq;
    if (!r.ok) {
      step('done'); doneTitle.text = 'COULD NOT SAVE THE REPORT'; doneTitle.style.color = '#ff8a6b';
      doneBody.text = r.error || 'Unknown error'; issuesBtn.visible = false; showBtn.visible = false; return;
    }
    var where = 'Saved on your PC as ' + r.file + ' (' + r.kb + ' KB).';
    if (off) {
      step('done'); doneTitle.text = 'REPORT SAVED'; doneTitle.style.color = '#7fe0a0';
      doneBody.text = where + ' This build cannot send reports. It is selected in the Explorer window that opened; you can attach it to an issue on GitHub.';
      issuesBtn.visible = true; showBtn.visible = true; return;
    }
    if (sendAsked && st.send === 'sending') sawSending = true;
    var finished = sendAsked && (sawSending || st.send === 'refused') && st.send !== 'sending';
    if (!sendAsked || !finished) {
      step('review');
      reviewBody.text = sendAsked ? 'Sending...' : where + ' It holds ' + HOLDS +
        ' Press SHOW FILE to read exactly what would be sent. SEND posts it to the SOURCE:MVM developer\'s Discord channel; DON\'T SEND keeps it on your PC only.';
      return;
    }
    step('done'); showBtn.visible = true;
    if (st.send === 'sent') {
      doneTitle.text = 'REPORT SENT'; doneTitle.style.color = '#7fe0a0';
      doneBody.text = 'Thanks! It went to the SOURCE:MVM developer, and its file was removed from your PC.';
      issuesBtn.visible = false; showBtn.visible = false;
    } else {
      doneTitle.text = 'COULD NOT SEND IT'; doneTitle.style.color = '#ff8a6b';
      doneBody.text = (st.sendDetail ? 'Reason: ' + st.sendDetail + '. ' : '') + where + ' You can attach it to an issue on GitHub instead.';
      issuesBtn.visible = true;
    }
  };
  C.openDialog = function () {
    if (!dlg || !dlg.IsValid()) build();
    pendingSeq = -1; reviewSeq = -1; sendAsked = false; sawSending = false; entry.text = '';
    dlg.visible = true; C.renderDialog();
    try { entry.SetFocus(); } catch (e) {}
  };
  C.closeDialog = function () { if (dlg && dlg.IsValid()) dlg.visible = false; pendingSeq = -1; sendAsked = false; };
  // Live check without clicks (console eval cannot carry strings): fill the form and press Review.
  // It stops at the review step; nothing is sent.
  C.selfTest = function () {
    C.openDialog(); entry.text = 'Self-test report from the SOURCE:MVM team. Ignore.'; contact.text = 'test';
    $.DispatchEvent('Activated', saveBtn, 'mouse');
  };
  // Live checks (console eval cannot carry strings): press a review button. 0 show file, 1 don't send,
  // 2 send -- the same Activated event a click raises.
  C.reportPress = function (n) {
    var b = [openBtn, keepBtn, sendBtn][n];
    if (b && b.IsValid()) $.DispatchEvent('Activated', b, 'mouse');
  };
  C.dialogStep = function () {
    if (!dlg || !dlg.IsValid() || !dlg.visible) return 'closed';
    return form.visible ? 'form' : review.visible ? 'review' : 'done';
  };
})();
)CMJS";

// Fragment 3: the crash prompt. When an earlier session crashed, the main menu says so and asks
// whether to send its report (Community/CrashReport). Nothing is sent without SEND. DON'T SEND keeps
// the report on this PC and it is never offered again.
inline const char* kCommunityCrashJs = R"CMJS(
(function () {
  var C = $.MvmCommunity; if (!C || !C.button) return;
  var prompt = null, title = null, text = null, showBtn = null, sendBtn = null, keepBtn = null, okBtn = null;
  var asked = false, hidden = false;

  function build() {
    prompt = $.CreatePanel('Panel', C.input, 'MvmCrashPrompt', {});
    prompt.style.width = '100%'; prompt.style.height = '100%'; prompt.style.zIndex = '999';
    prompt.style.backgroundColor = 'rgba(0,0,0,0.6)';
    prompt.SetPanelEvent('onactivate', function () {});
    var card = $.CreatePanel('Panel', prompt, '', {});
    card.style.width = '620px'; card.style.flowChildren = 'down'; card.style.padding = '24px 30px 26px 30px';
    card.style.horizontalAlign = 'center'; card.style.verticalAlign = 'center';
    card.style.backgroundColor = 'gradient(linear, 0% 0%, 0% 100%, from(#262b33), to(#14171c))';
    card.style.borderTop = '3px solid #e0473c'; card.style.boxShadow = 'rgba(0,0,0,0.7) 0px 12px 48px 0px';
    title = C.text(card, '', 'stratum-medium-condensed', 32, '#ffffff');
    title.style.letterSpacing = '1px';
    text = C.body(card);
    var actions = C.row(card);
    showBtn = C.button(actions, 'MvmCrashShow', 'SHOW FILE', false, function () { C.cmd('crashshow'); });
    keepBtn = C.button(actions, 'MvmCrashKeep', "DON'T SEND", false, function () { C.cmd('crashdecline'); hidden = true; render(); });
    sendBtn = C.button(actions, 'MvmCrashSend', 'SEND', true, function () { if (!asked) { asked = true; C.cmd('crashsend'); } });
    okBtn = C.button(actions, 'MvmCrashOk', 'OK', true, function () { hidden = true; render(); });
  }

  function render() {
    var st = C.state || {}, c = st.crash || {};
    var dialogOpen = C.dialogStep && C.dialogStep() !== 'closed';
    var finished = asked && (c.send === 'sent' || c.send === 'failed');
    var want = !hidden && !dialogOpen && ((c.count || 0) > 0 || finished);
    if (!want) { if (prompt && prompt.IsValid()) prompt.visible = false; return; }
    if (!prompt || !prompt.IsValid()) build();
    prompt.visible = true;
    var off = st.send === 'off';
    var showSend = !off && !finished;
    sendBtn.visible = showSend; keepBtn.visible = showSend; okBtn.visible = !showSend;
    if (finished && c.send === 'sent') {
      title.text = 'CRASH REPORT SENT';
      text.text = 'Thanks! It went to the SOURCE:MVM developer, and its files were removed from your PC.';
      return;
    }
    if (finished) {
      title.text = 'COULD NOT SEND IT';
      text.text = (c.detail ? 'Reason: ' + c.detail + '. ' : '') + 'The report stays on your PC; SOURCE:MVM asks again next time.';
      return;
    }
    title.text = 'CS2 CRASHED LAST TIME';
    text.text = (c.summary ? c.summary + '\n\n' : '') + 'SOURCE:MVM saved a crash report on your PC as ' + c.file +
      ((c.count || 0) > 1 ? ' (and ' + (c.count - 1) + ' more)' : '') + '. It holds the crash details, your CS2 and SOURCE:MVM builds, ' +
      'Windows version, map and demo names, and recent game logs. It has no Steam name or ID.' +
      (c.dump ? ' A small crash dump goes with it: each thread\'s stack at the moment of the crash, so the ' +
        'developer can see the exact function that failed. It can hold fragments of game memory, but no files.' : '') +
      (off ? ' This build cannot send reports.' : asked ? '\n\nSending...' :
        ' Send it to the SOURCE:MVM developer? Press SHOW FILE to read exactly what would be sent.');
  }
  C.renderCrash = render;
  // Live checks: press a prompt button. 0 show file, 1 don't send, 2 send, 3 ok.
  C.crashPress = function (n) {
    var b = [showBtn, keepBtn, sendBtn, okBtn][n];
    if (b && b.IsValid() && b.visible) $.DispatchEvent('Activated', b, 'mouse');
  };
  C.crashPromptState = function () { return (prompt && prompt.IsValid() && prompt.visible) ? title.text : 'hidden'; };
  render();
})();
)CMJS";

// Fragment 4: live HCs from HC Tracker (Community/HcTrackerFeed): a "2 LIVE" / "NEXT 3H" pill on
// the badge, and a dropdown under it on hover listing what is live and what is coming up. A row
// opens that HC's page; the footer opens the site. The dropdown sits at the top of #MainMenuInput's
// layers, like the badge, and closes when the friends bar opens.
inline const char* kCommunityHcJs = R"CMJS(
(function () {
  var C = $.MvmCommunity; if (!C || !C.badge) return;
  var badge = C.badge, drop = null, body = null;
  var DROP_W = 380;

  var pill = $.CreatePanel('Panel', badge, 'MvmHcLivePill', {});
  pill.style.flowChildren = 'right'; pill.style.height = '24px'; pill.style.padding = '0px 10px 0px 9px';
  pill.style.marginLeft = '12px'; pill.style.verticalAlign = 'center'; pill.style.borderRadius = '12px';
  pill.visible = false; pill.hittest = false;
  var pillDot = $.CreatePanel('Panel', pill, '', {});
  pillDot.style.width = '8px'; pillDot.style.height = '8px'; pillDot.style.borderRadius = '4px';
  pillDot.style.backgroundColor = '#ffffff'; pillDot.style.marginRight = '7px'; pillDot.style.verticalAlign = 'center';
  pillDot.hittest = false;
  var pillText = C.text(pill, '', 'stratum-medium-condensed', 18, '#ffffff');
  pillText.style.letterSpacing = '1px'; pillText.style.verticalAlign = 'center'; pillText.style.transform = 'translateY(1px)';

  // "3h", "2d", "45m" until an ISO time; '' when unset or unreadable.
  function until(iso) {
    if (!iso) return '';
    var t = Date.parse(iso); if (isNaN(t)) return '';
    var m = Math.round((t - Date.now()) / 60000);
    if (m <= 0) return 'now';
    if (m < 60) return m + 'm';
    if (m < 48 * 60) return Math.floor(m / 60) + 'h';
    return Math.floor(m / 1440) + 'd';
  }
  // Row/footer highlight only. Whether the dropdown is open is decided by polling hover (below).
  function keep(p) {
    p.SetPanelEvent('onmouseover', function () { if (p.__hot) p.__hot(true); });
    p.SetPanelEvent('onmouseout', function () { if (p.__hot) p.__hot(false); });
  }
  function caption(text) {
    var l = C.text(body, text, 'stratum-medium-condensed', 15, '#8b95a1');
    l.style.letterSpacing = '1px'; l.style.marginTop = '10px'; l.style.marginBottom = '4px';
  }
  function row(c, live) {
    var r = $.CreatePanel('Button', body, '', {});
    r.style.width = '100%'; r.style.flowChildren = 'down'; r.style.padding = '7px 10px'; r.style.borderRadius = '2px';
    if (live) r.style.borderLeft = '3px solid #e5484d';
    var t = C.text(r, c.title || 'Untitled HC', 'stratum-medium-condensed', 22, '#ffffff');
    t.style.width = '100%'; t.style.textOverflow = 'ellipsis'; t.style.whiteSpace = 'nowrap';
    var bits = [];
    if (c.host) bits.push('by ' + c.host);
    if (live) {
      bits.push(c.entries + (c.entries === 1 ? ' entry' : ' entries'));
      var closes = until(c.closesAt); if (closes) bits.push(closes === 'now' ? 'closing' : 'closes in ' + closes);
    } else {
      var starts = until(c.startsAt); if (starts) bits.push(starts === 'now' ? 'starting' : 'starts in ' + starts);
    }
    C.text(r, bits.join('  ·  '), 'stratum-regular', 14, '#a9b3bf');
    r.__hot = function (on) { r.style.backgroundColor = on ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0)'; };
    keep(r);
    r.SetPanelEvent('onactivate', function () { C.sound(); C.cmd('hcopen ' + c.code); });
  }
  function build() {
    drop = $.CreatePanel('Panel', C.input, 'MvmHcDropdown', {});
    drop.style.zIndex = '11'; // above every menu layer, so it takes hover and clicks
    var d = drop.style;
    d.width = DROP_W + 'px'; d.flowChildren = 'down'; d.padding = '12px 14px 14px 14px';
    d.backgroundColor = 'gradient(linear, 0% 0%, 0% 100%, from(#262b33), to(#14171c))';
    d.borderTop = '3px solid ' + C.HC_YELLOW; d.boxShadow = 'rgba(0,0,0,0.6) 0px 8px 30px 0px';
    drop.visible = false;
    keep(drop);
    body = $.CreatePanel('Panel', drop, '', {});
    body.style.flowChildren = 'down'; body.style.width = '100%';
    var foot = $.CreatePanel('Button', drop, '', {});
    foot.style.width = '100%'; foot.style.marginTop = '12px'; foot.style.padding = '9px 0px 7px 0px';
    foot.style.backgroundColor = 'rgba(245,183,25,0.14)'; foot.style.border = '1px solid rgba(245,183,25,0.5)';
    foot.style.borderRadius = '2px';
    var ft = C.text(foot, 'OPEN HC TRACKER', 'stratum-medium-condensed', 19, C.HC_YELLOW);
    ft.style.horizontalAlign = 'center'; ft.style.letterSpacing = '1px';
    foot.__hot = function (on) { foot.style.backgroundColor = on ? 'rgba(245,183,25,0.28)' : 'rgba(245,183,25,0.14)'; };
    keep(foot);
    foot.SetPanelEvent('onactivate', function () { C.sound(); C.cmd('hctracker'); });
  }
  function fill() {
    body.RemoveAndDeleteChildren();
    var hc = (C.state || {}).hc || {};
    if (!hc.ok) {
      C.text(body, 'Checking HC Tracker...', 'stratum-regular', 16, '#a9b3bf').style.marginTop = '4px';
      return;
    }
    var open = hc.open || [], soon = hc.scheduled || [];
    if (open.length) {
      caption('LIVE NOW  (' + hc.live + ')');
      for (var i = 0; i < open.length; ++i) row(open[i], true);
    }
    if (soon.length) {
      caption('COMING UP  (' + hc.upcoming + ')');
      for (var k = 0; k < soon.length; ++k) row(soon[k], false);
    }
    if (!open.length && !soon.length)
      C.text(body, 'No HCs running right now. Check back soon.', 'stratum-regular', 16, '#a9b3bf').style.marginTop = '4px';
  }
  // Centred under the badge. Layout px = window px / the menu's UI scale.
  function place() {
    var scale = C.input.actualuiscale_x || 1;
    var b = badge.GetPositionWithinWindow(), i = C.input.GetPositionWithinWindow();
    if (!b || !i) return;
    var x = (b.x - i.x + badge.actuallayoutwidth / 2) / scale - DROP_W / 2;
    var y = (b.y - i.y + badge.actuallayoutheight) / scale + 4;
    drop.style.position = x.toFixed(1) + 'px ' + y.toFixed(1) + 'px 0px';
  }
  function show() {
    if (!drop || !drop.IsValid()) build();
    fill(); place(); drop.visible = true;
  }
  function hide() { if (drop && drop.IsValid()) drop.visible = false; }
  // Hover opens it, polled the way CS2 polls its own friends bar (BHasHoverStyle, mainmenu.js):
  // mouse-over/out events fire unevenly across child panels and the gap under the badge. It stays
  // open while the badge OR the dropdown is hovered, with a short grace to cross the gap.
  var lastHover = 0, GRACE_MS = 350, pinned = false;
  (function watchHover() {
    if (!badge.IsValid()) return;
    var hovered = !C.sidebarOpen && (pinned || badge.BHasHoverStyle() || !!(drop && drop.IsValid() && drop.visible && drop.BHasHoverStyle()));
    var now = Date.now();
    if (hovered) { lastHover = now; if (!drop || !drop.IsValid() || !drop.visible) show(); }
    else if (drop && drop.IsValid() && drop.visible && (C.sidebarOpen || now - lastHover > GRACE_MS)) hide();
    $.Schedule(0.05, watchHover);
  })();
  C.hcHover = function (on, force) { if (force) hide(); };
  // Live checks without a mouse (synthetic input never reaches Panorama): hold it open, report hover.
  C.hcPin = function (on) { pinned = !!on; };
  C.hcHoverState = function () {
    return 'badgeHover=' + badge.BHasHoverStyle() + ' dropOpen=' + !!(drop && drop.IsValid() && drop.visible);
  };
  C.renderHc = function () {
    var hc = (C.state || {}).hc || {};
    if (!hc.ok || (!hc.live && !hc.upcoming)) pill.visible = false;
    else if (hc.live) {
      pill.visible = true; pillDot.visible = true; pill.style.backgroundColor = '#e5484d';
      pillText.text = hc.live + ' LIVE';
    } else {
      var next = (hc.scheduled && hc.scheduled[0]) ? until(hc.scheduled[0].startsAt) : '';
      pill.visible = true; pillDot.visible = false; pill.style.backgroundColor = 'rgba(255,255,255,0.16)';
      pillText.text = next && next !== 'now' ? 'NEXT ' + next.toUpperCase() : hc.upcoming + ' UPCOMING';
    }
    if (drop && drop.IsValid() && drop.visible) fill();
  };
  C.renderHc();
})();
)CMJS";

} // namespace Filmmaker
