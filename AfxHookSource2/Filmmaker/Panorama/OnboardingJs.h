#pragma once

// Panorama JS for Clippy's onboarding panel (OnboardingHud.cpp). Built into whichever context is
// up (main menu, else the in-game HUD) the first time the tutorial opens. C++ writes the "state"
// attribute on #MvmOnboardingRoot and calls $.MvmOnboarding.render() every frame while he is up.
//
// Fragments (adjacent raw literals, one shared scope):
//   OnboardingClippyJs.h  the clapperboard: build + per-frame pose
//   OnboardingScriptJs.h  every lesson step: words, targets, demo moments, what counts as done
//   OnboardingRoutesJs.h  the paths through the steps, the topic menu and the endings
//   OnboardingGuideJs.h   the spotlight (dim + ring) and the world markers
//   OnboardingLessonJs.h  runs a step: enter, spotlight, markers, auto-advance
//
// Behaviour carried over from the reviewed browser mockup:
//   * text types out quickly with short pauses after punctuation; the clapper top opens and
//     shuts on the syllables, and the optional chatter sound fires on the same beats;
//   * clicking the text shows the whole line at once (and replays it once it has finished);
//   * he sits on the left; the buttons never move while a line types, because a hidden copy of
//     the full line reserves its height and the bubble is anchored at its bottom edge;
//   * resting the cursor just outside him for half a second moves him to the other spot on the
//     left (at most once every 2 s); holding the mouse on anything else fades him until 1.1 s
//     after release, and in game he also fades while the camera is being flown (no cursor);
//   * Skip does the step; Back goes one back; the close box asks first (keep going, stop here,
//     start over), and stopping keeps the place for the next mvm_onboarding.
//
// Every button issues `mirv_filmmaker onboarding ...`; the tutorial state lives in C++
// (Onboarding/Onboarding.cpp) so it survives the main menu -> demo context switch.

namespace Filmmaker {

inline const char* kOnboardingJs = R"OBJS(
(function () {
  try {
    var old = $('#MvmOnboardingRoot'); if (old) old.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var root = $.CreatePanel('Panel', ctx, 'MvmOnboardingRoot');
    root.hittest = false;
    root.style.width = '100%'; root.style.height = '100%';
    root.style.position = '0px 0px 0px';
    // Above every Source MVM workspace root (the highest is 260), below the easter eggs (900).
    root.style.zIndex = '880';
    root.style.overflow = 'noclip noclip';

    var C = { ink: '#eef2f6', text: '#e3e8ee', muted: '#a9b4c2', gold: '#f0b323', goldHover: '#ffc53d',
      bubble: '#1d232c', line: '#454e5b', dark: '#151004' };
    var CMD = 'mirv_filmmaker onboarding ';
    function clamp(v, a, b) { return v < a ? a : (v > b ? b : v); }
    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(CMD + c); }
      catch (e) { $.Msg('[onboarding] cmd failed: ' + e + '\n'); }
    }
    // In game a child with no alignment is CENTRED in its parent; pin everything top-left.
    function P(parent, id) {
      var p = $.CreatePanel('Panel', parent, id || '');
      p.hittest = false;
      p.style.horizontalAlign = 'left'; p.style.verticalAlign = 'top';
      return p;
    }
    function L(parent, text, color, size) {
      var l = $.CreatePanel('Label', parent, '');
      l.hittest = false;
      l.style.horizontalAlign = 'left'; l.style.verticalAlign = 'top';
      l.text = text;
      l.style.color = color;
      l.style.fontSize = (+size).toFixed(1) + 'px';
      return l;
    }
    function cssSize() {
      var sx = Number(root.actualuiscale_x), sy = Number(root.actualuiscale_y);
      if (!(sx > 0)) sx = 1;
      if (!(sy > 0)) sy = 1;
      var w = (root.actuallayoutwidth || 0) / sx, h = (root.actuallayoutheight || 0) / sy;
      if (!(w > 0 && w < 1e5)) w = 1920;
      if (!(h > 0 && h < 1e5)) h = 1080;
      return { w: w, h: h, sx: sx, sy: sy };
    }
    function sizeOf(p, sz) {
      return { w: (p.actuallayoutwidth || 0) / sz.sx, h: (p.actuallayoutheight || 0) / sz.sy };
    }
    // A hittest panel with a hover colour and a click.
    function button(p, onHover, onActivate) {
      p.hittest = true;
      p.SetPanelEvent('onmouseover', function () { onHover(true); });
      p.SetPanelEvent('onmouseout', function () { onHover(false); });
      p.SetPanelEvent('onactivate', onActivate);
      return p;
    }
    function link(parent, text, onActivate) {
      var p = P(parent, '');
      p.style.padding = '3px 0px';
      var l = L(p, text, C.muted, 13);
      button(p, function (on) { l.style.color = on ? C.ink : C.muted; }, onActivate);
      return { p: p, l: l };
    }
    function goldButton(parent, text, onActivate) {
      var p = P(parent, '');
      p.style.backgroundColor = C.gold;
      p.style.borderRadius = '6px';
      p.style.padding = '7px 13px';
      var l = L(p, text, C.dark, 13);
      l.style.fontWeight = 'bold';
      button(p, function (on) { p.style.backgroundColor = on ? C.goldHover : C.gold; }, onActivate);
      return { p: p, l: l };
    }

    // ---------------------------------------------------------------- the assistant
    var BUBBLE_W = 440, GAP = 10, LEFT = 32;
    var asst = P(root, 'MvmObAssistant');
    asst.style.flowChildren = 'right';
    asst.style.verticalAlign = 'bottom';
    asst.style.overflow = 'noclip';
    asst.style.zIndex = '10'; // above the spotlight's dim (1) and the world markers (2)

    var wrap = P(asst, '');
    wrap.style.width = BUBBLE_W + 'px';
    wrap.style.verticalAlign = 'bottom';
    wrap.style.overflow = 'noclip';
    var bub = P(wrap, 'MvmObBubble');
    bub.hittest = true;
    bub.style.width = '100%';
    bub.style.flowChildren = 'down';
    bub.style.backgroundColor = C.bubble;
    bub.style.border = '1px solid ' + C.line;
    bub.style.borderRadius = '13px';
    bub.style.padding = '14px 18px 9px 18px';
    bub.style.boxShadow = '#000000aa 0px 10px 26px 0px';
    // Tail pointing at Clippy: a rotated square whose two outer edges carry the border.
    var tail = P(wrap, '');
    tail.style.width = '14px'; tail.style.height = '14px';
    tail.style.horizontalAlign = 'right'; tail.style.verticalAlign = 'bottom';
    tail.style.marginRight = '-8px'; tail.style.marginBottom = '64px';
    tail.style.backgroundColor = C.bubble;
    tail.style.borderTop = '1px solid ' + C.line;
    tail.style.borderRight = '1px solid ' + C.line;
    tail.style.transform = 'rotateZ(45deg)';

    // Top row: speaker + chapter on the left, chatter + exit on the right. It is also the drag
    // handle (see place()), which the grip in the middle of the top edge advertises.
    var top = P(bub, '');
    top.style.width = '100%'; top.style.height = '22px';
    top.style.overflow = 'noclip';
    var grip = P(top, '');
    grip.style.width = '30px'; grip.style.height = '4px'; grip.style.borderRadius = '2px';
    grip.style.horizontalAlign = 'center'; grip.style.marginTop = '-9px';
    grip.style.backgroundColor = C.line;
    var tl = P(top, '');
    tl.style.flowChildren = 'right'; tl.style.verticalAlign = 'center';
    var dot = P(tl, '');
    dot.style.width = '5px'; dot.style.height = '5px'; dot.style.borderRadius = '3px';
    dot.style.backgroundColor = C.gold; dot.style.verticalAlign = 'center'; dot.style.marginRight = '7px';
    var who = L(tl, 'Clippy', C.ink, 14);
    who.style.fontWeight = 'bold'; who.style.verticalAlign = 'center';
    var chapter = L(tl, '', C.muted, 12);
    chapter.style.marginLeft = '7px'; chapter.style.verticalAlign = 'center';
    var tr = P(top, '');
    tr.style.flowChildren = 'right'; tr.style.horizontalAlign = 'right'; tr.style.verticalAlign = 'center';
    var chatterL = link(tr, 'Chatter off', function () { cmd('chatter ' + (st.voice ? 'off' : 'on')); });
    chatterL.l.style.fontSize = '12px';
    chatterL.p.style.verticalAlign = 'center';
    var xBtn = P(tr, '');
    xBtn.style.width = '22px'; xBtn.style.height = '22px'; xBtn.style.borderRadius = '4px';
    xBtn.style.marginLeft = '10px'; xBtn.style.verticalAlign = 'center';
    var xBars = [];
    for (var xi = 0; xi < 2; xi++) {
      var xb = P(xBtn, '');
      xb.style.width = '12px'; xb.style.height = '2px'; xb.style.borderRadius = '1px';
      xb.style.horizontalAlign = 'center'; xb.style.verticalAlign = 'center';
      xb.style.backgroundColor = C.muted;
      xb.style.transform = 'rotateZ(' + (xi ? -45 : 45) + 'deg)';
      xBars.push(xb);
    }
    button(xBtn, function (on) {
      xBtn.style.backgroundColor = on ? '#ffffff14' : 'none';
      for (var i = 0; i < 2; i++) xBars[i].style.backgroundColor = on ? C.ink : C.muted;
    }, function () { cmd('close'); });

    // The words. Clicking anywhere on them shows the whole line (or replays a finished one).
    var readout = P(bub, '');
    readout.style.width = '100%'; readout.style.flowChildren = 'down'; readout.style.marginTop = '9px';
    var title = L(readout, '', C.ink, 21);
    title.style.fontWeight = 'bold'; title.style.width = '100%';
    var speech = P(readout, '');
    speech.style.width = '100%'; speech.style.marginTop = '6px';
    // The hidden copy holds the full line, so the bubble is already its final height while the
    // visible copy types out on top of it.
    var layoutL = L(speech, '', C.text, 16);
    layoutL.style.width = '100%'; layoutL.style.opacity = '0';
    var visL = L(speech, '', C.text, 16);
    visL.style.width = '100%';
    var status = P(readout, '');
    status.style.flowChildren = 'right'; status.style.height = '18px'; status.style.marginTop = '6px';
    var beats = [];
    for (var bi = 0; bi < 3; bi++) {
      var bb = P(status, '');
      bb.style.width = '3px'; bb.style.height = '4px'; bb.style.borderRadius = '2px';
      bb.style.marginRight = '3px'; bb.style.verticalAlign = 'center';
      bb.style.backgroundColor = C.muted;
      beats.push(bb);
    }
    var statusL = L(status, 'Your turn.', C.muted, 12);
    statusL.style.marginLeft = '5px'; statusL.style.verticalAlign = 'center';
    readout.hittest = true;
    readout.SetPanelEvent('onactivate', function () { if (speaking) stopSpeech(); else startSpeech(); });

    // Opening choices.
    var options = P(bub, '');
    options.style.width = '100%'; options.style.flowChildren = 'down'; options.style.marginTop = '8px';
)OBJS"
#include "OnboardingScriptJs.h"
#include "OnboardingRoutesJs.h"
#include "OnboardingClippyJs.h"
#include "OnboardingGuideJs.h"
#include "OnboardingLessonJs.h"
R"OBJS(
    // Option rows: a pool reused by every step that offers choices (the opening paths, "want the
    // deeper rundown?", the topic menu). Each row carries a `go` spec, see goSpec().
    var optRows = [];
    for (var oi = 0; oi < 8; oi++) (function () {
      var row = P(options, '');
      row.style.width = '100%'; row.style.height = '35px'; row.style.marginBottom = '5px';
      row.style.borderRadius = '7px'; row.style.backgroundColor = '#ffffff08';
      row.style.flowChildren = 'right';
      var d = P(row, '');
      d.style.width = '5px'; d.style.height = '5px'; d.style.borderRadius = '3px';
      d.style.backgroundColor = '#e3bb5b'; d.style.verticalAlign = 'center'; d.style.margin = '0px 10px 0px 12px';
      var l = L(row, '', C.text, 14);
      l.style.verticalAlign = 'center';
      var o = { row: row, l: l, go: null };
      button(row, function (on) {
        row.style.backgroundColor = on ? '#ffffff16' : '#ffffff08';
        l.style.color = on ? '#ffffff' : C.text;
      }, function () { if (o.go) goSpec(o.go); });
      row.visible = false;
      optRows.push(o);
    })();
    function setOptions(list) {
      for (var i = 0; i < optRows.length; i++) {
        var item = list && list[i];
        optRows[i].row.visible = !!item;
        optRows[i].go = item ? item.go : null;
        if (item) optRows[i].l.text = item.label;
      }
      options.visible = !!(list && list.length);
    }

    // Footer: the same three slots on every step, so nothing jumps.
    var foot = P(bub, '');
    foot.style.width = '100%'; foot.style.height = '44px'; foot.style.marginTop = '6px';
    foot.style.borderTop = '1px solid ' + C.line;
    var fl = P(foot, '');
    fl.style.flowChildren = 'right'; fl.style.verticalAlign = 'center'; fl.style.marginTop = '4px';
    // Back steps one back (from a lesson's first step, back to the opening choices). There is no
    // Restart in the footer: it is one of the close box's choices.
    var backL = link(fl, 'Back', function () { onBack(); });
    backL.p.style.marginRight = '20px';
    var skipL = link(fl, '', function () { onSkip(); });
    var goB = goldButton(foot, '', function () { onNext(); });
    goB.p.style.horizontalAlign = 'right'; goB.p.style.verticalAlign = 'center'; goB.p.style.marginTop = '4px';

    var clippy = clippyBuild(asst);
    clippy.c.style.verticalAlign = 'bottom';
    clippy.c.style.marginLeft = GAP + 'px';
    clippy.c.SetPanelEvent('onactivate', function () { poke(); });

)OBJS"
R"OBJS(
    // ---------------------------------------------------------------- state + speech
    var st = {}, seenRev = -1, lineKey = '', line = OPENING, isFinale = false;
    var speaking = false, plan = null, t0 = 0, shown = 0, nextBeat = 0;
    var pokes = [], swoon = false, swoonEnd = 0;
    // CS2's own UI sounds (soundevents/game_sounds_ui.vsndevts), played on the clapper's beats:
    // mostly the rename dialog's teletype tick, with a softer rollover mixed in so a long line is
    // less samey.
    var CHATTER = ['UIPanorama.rename_teletype', 'UIPanorama.rename_teletype', 'UIPanorama.mainmenu_rollover'];
    var chatterLogged = false;
    var SPEED = 73, OPENING_DEG = 34, REST = -8;

    function routeOf() { return st.route && ROUTES[st.route] ? ROUTES[st.route] : null; }
    function currentLine() {
      var r = routeOf();
      if (!r) return OPENING;
      if (st.step >= r.steps.length) return r.topic ? TOPIC_END : FINALE;
      var s = r.steps[st.step];
      return ctxOk(s) ? s : bridgeLine(s);
    }
    // 'route step', or 'finale' for the end of the current route.
    // '@resume' / '@off' / '@restart' are the close box's answers.
    function goSpec(spec) {
      if (spec === '@resume') { cmd('resume'); return; }
      if (spec === '@off') { cmd('off'); return; }
      if (spec === '@restart') { cmd('restart'); return; }
      if (spec === 'finale') { var r = routeOf(); cmd('go ' + st.route + ' ' + (r ? r.steps.length : 0)); return; }
      cmd('go ' + spec);
    }
    function onNext() {
      var r = routeOf();
      if (!r) return;
      if (st.step >= r.steps.length) { if (r.topic) cmd('go more 0'); else cmd('restart'); return; }
      var step = r.steps[st.step];
      if (!ctxOk(step)) {
        // The bridge's button: load the practice demo, or skip the menu-only steps.
        if (step.ctx === 'hud') { raw('playdemo onboarding'); return; }
        var i = st.step + 1;
        while (i < r.steps.length && r.steps[i].ctx === 'menu') i++;
        cmd('go ' + st.route + ' ' + i);
        return;
      }
      cmd('go ' + st.route + ' ' + (st.step + 1));
    }
    // "Explore on my own" (the opening and the endings) puts him away completely; mvm_onboarding
    // brings him back. Mid-lesson the same slot skips the step, and skipping DOES the step: its
    // act() opens what it asked you to open, places what it asked you to place, so the next step
    // starts where it expects to.
    function onSkip() {
      var r = routeOf();
      if (!r || st.step >= r.steps.length) { cmd('off'); return; }
      var step = r.steps[st.step];
      if (ctxOk(step) && step.act) {
        try { step.act(); } catch (e) { $.Msg('[onboarding] skip act: ' + e + '\n'); }
      }
      cmd('go ' + st.route + ' ' + (st.step + 1));
    }
    // Back one step; from a lesson's first step, back to where it was chosen.
    function onBack() {
      var r = routeOf();
      if (!r) return;
      if (st.step > 0) { cmd('go ' + st.route + ' ' + (st.step - 1)); return; }
      if (r.topic) cmd('go more 0'); else cmd('restart');
    }

    // Per-character reveal times plus clapper beats: about SPEED characters a second, spaces
    // quicker, a pause after . ! ? and a shorter one after , ; :. A beat at most every 120 ms.
    function makePlan(text) {
      var chars = text.split(''), times = [], beatList = [], t = 0, last = -140;
      for (var i = 0; i < chars.length; i++) {
        var c = chars[i];
        t += (1000 / SPEED) * (/\s/.test(c) ? 0.4 : 0.9 + Math.random() * 0.2);
        times.push(t);
        if (/[a-z0-9]/i.test(c) && t - last >= 120) {
          beatList.push({ at: Math.max(0, t - 12), len: 105, deg: OPENING_DEG * (0.55 + Math.random() * 0.45) });
          last = t;
        }
        if (i < chars.length - 1 && /[.!?]/.test(c)) t += 135;
        else if (/[,;:]/.test(c)) t += 70;
      }
      var dur = t + 120, keys = [{ t: 0, d: REST }];
      for (var b = 0; b < beatList.length; b++) {
        var bt = beatList[b];
        keys.push({ t: bt.at + bt.len * 0.32, d: -bt.deg }, { t: bt.at + bt.len * 0.76, d: 0 });
      }
      keys.push({ t: dur, d: REST });
      return { chars: chars, times: times, beats: beatList, keys: keys, dur: dur };
    }
    function clapAt(keys, ms) {
      for (var i = 1; i < keys.length; i++) {
        if (ms <= keys[i].t) {
          var a = keys[i - 1], b = keys[i], f = b.t > a.t ? (ms - a.t) / (b.t - a.t) : 1;
          return a.d + (b.d - a.d) * f;
        }
      }
      return REST;
    }
    function startSpeech() {
      plan = makePlan(line.body);
      t0 = Date.now(); shown = 0; nextBeat = 0; speaking = true;
      visL.text = '';
      statusL.text = 'Clippy is talking\u2026';
    }
    function stopSpeech() {
      speaking = false; plan = null;
      visL.text = line.body;
      statusL.text = 'Your turn.';
    }
    function tickSpeech(now) {
      if (!speaking || !plan) return REST;
      var el = now - t0, n = shown;
      while (n < plan.chars.length && plan.times[n] <= el) n++;
      if (n !== shown) { shown = n; visL.text = plan.chars.slice(0, n).join(''); }
      // Chatter on the same beats as the clapper. Beats more than 80 ms late (a hitch) are
      // dropped rather than played in a burst.
      while (nextBeat < plan.beats.length && plan.beats[nextBeat].at <= el) {
        if (st.voice && el - plan.beats[nextBeat].at < 80) {
          var snd = CHATTER[nextBeat % CHATTER.length];
          try {
            $.DispatchEvent('CSGOPlaySoundEffect', snd, 'MOUSE');
            if (st.dbg && !chatterLogged) { chatterLogged = true; $.Msg('[onboarding] chatter played ' + snd + '\n'); }
          } catch (e) {
            if (!chatterLogged) { chatterLogged = true; $.Msg('[onboarding] chatter failed: ' + e + '\n'); }
          }
        }
        nextBeat++;
      }
      if (el >= plan.dur) { stopSpeech(); return REST; }
      return clapAt(plan.keys, el);
    }

    function showLine(d) {
      line = d;
      title.text = d.title;
      layoutL.text = d.body;
      visL.text = d.body;
      clippy.expr = d.expr || 'curious';
    }
    // One poke: a hop and the line again. Three within two seconds: he swoons (SWOON), and the
    // lesson line comes back a moment after he finishes.
    function poke() {
      var now = Date.now(), recent = [];
      for (var i = 0; i < pokes.length; i++) if (now - pokes[i] < 2000) recent.push(pokes[i]);
      recent.push(now);
      pokes = recent;
      if (pokes.length >= 3 && !swoon) {
        pokes = []; swoon = true; swoonEnd = 0;
        showLine(SWOON);
        clippyFall(clippy, false);
      } else clippyHop(clippy);
      startSpeech();
    }
    function fill() {
      var r = routeOf(), n = r ? r.steps.length : 0;
      swoon = false;
      if (st.closed) {
        // The close box asked: the bubble holds the question and its three answers, nothing else.
        showLine(EXIT_ASK);
        isFinale = false;
        setOptions(EXIT_ASK.options);
        chapter.text = r ? '\u00b7 ' + r.name + ' \u00b7 paused' : '\u00b7 Paused';
        backL.p.visible = false; skipL.p.visible = false; goB.p.visible = false;
        return;
      }
      skipL.p.visible = true;
      showLine(currentLine());
      var atEnd = !!r && st.step >= n;
      isFinale = atEnd && !r.topic;
      var step = stepNow(), live = step && ctxOk(step);
      if (!r) setOptions(CHOICES.map(function (c) { return { label: c.label, go: c.route + ' 0' }; }));
      else setOptions(live && step.options ? step.options : null);
      chapter.text = !r ? '\u00b7 Welcome'
        : ('\u00b7 ' + r.name + (atEnd ? ' \u00b7 done' : (n > 1 ? ' \u00b7 ' + (st.step + 1) + ' of ' + n : '')));
      skipL.l.text = !r ? "I'll explore on my own" : (atEnd ? 'Explore on my own' : 'Skip this step');
      backL.p.visible = !!r;
      goB.p.visible = !!(r && line.action && !(live && step.options));
      goB.l.text = line.action || '';
    }
)OBJS"
R"OBJS(
    // ---------------------------------------------------------------- placement, drag, fade
    // Until the user drags him he has two spots on the left: low (above whatever band sits at
    // the bottom) and high. He moves between them only after the cursor rests in the ring just
    // outside him for 500 ms, at most every 2 s.
    //
    // Dragging: press on the bubble's header row (not on Chatter or the close box) and move. The
    // bottom-left corner follows the hand, so the buttons land where they are put. On release the
    // spot goes to C++ as screen fractions (`onboarding pos x y`), so it survives the menu ->
    // demo switch; `mvm_onboarding pos reset` hands placement back to the two spots.
    var slot = 'low', curL = null, curB = null, dwellAt = 0, lastMoveAt = -1e9, held = false, workUntil = 0;
    var opacity = 1, lastFrameAt = Date.now(), wasHidden = false;
    var RING = 28, TOP_PAD = 110, HANDLE_H = 36;
    var drag = null, lastLmb = false, userPos = null, seenPos = null, gripLit = false;
    var asstParts = null; // where the bubble and Clippy are this frame (css px): clicks may land here

    function inside(r, x, y) { return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1; }
    function lowMargin(sz) { return Math.round((st.dockH || 0) * sz.h / 1080) + 28; }
    function frac(v) { return Number(v.toFixed(4)); }
    // Where to stand next to a spotlit target: his own spots on the left first, then beside,
    // above or below it. The first place fully on screen that keeps the target clear wins.
    // A wide berth (pad) so a click meant for the bubble never lands on the lit control, and the
    // other way round; a smaller one around the controls he must never cover (avoidRects). When
    // nothing is fully clear, the spot that covers the least, the lit control counting most.
    // t (the lit control) may be null; first, when given, is tried before everything else.
    function overlapArea(x0, y0, x1, y1, r, pad) {
      var ox = Math.max(0, Math.min(x1, r.x + r.w + pad) - Math.max(x0, r.x - pad));
      var oy = Math.max(0, Math.min(y1, r.y + r.h + pad) - Math.max(y0, r.y - pad));
      return ox * oy;
    }
    function chooseSpot(t, a, sz, avoid, first) {
      var pad = 48, low = sz.h - lowMargin(sz), w = a.w, h = a.h;
      var right = sz.w - w - LEFT, high = Math.min(low, TOP_PAD + h);
      var cands = first ? [first] : [];
      cands.push({ left: LEFT, bottom: low }, { left: LEFT, bottom: high });
      if (t) {
        var mid = clamp(t.y + t.h / 2 + h / 2, h + 10, low);
        var cx = clamp(t.x + t.w / 2 - w / 2, 10, sz.w - w - 10);
        cands.push({ left: t.x + t.w + pad, bottom: mid }, { left: t.x - pad - w, bottom: mid },
          { left: cx, bottom: t.y - pad }, { left: cx, bottom: t.y + t.h + pad + h });
      }
      cands.push({ left: right, bottom: low }, { left: right, bottom: high });
      var best = cands[0], bestCover = Infinity;
      for (var i = 0; i < cands.length; i++) {
        var c = cands[i], x0 = c.left, x1 = c.left + w, y0 = c.bottom - h, y1 = c.bottom;
        if (!(x0 >= 0 && x1 <= sz.w && y0 >= 0 && y1 <= sz.h)) continue;
        var cover = t ? 100 * overlapArea(x0, y0, x1, y1, t, pad) : 0;
        for (var j = 0; j < avoid.length; j++) cover += overlapArea(x0, y0, x1, y1, avoid[j], 16);
        if (cover === 0) return c;
        if (cover < bestCover) { bestCover = cover; best = c; }
      }
      return best;
    }

    // spot: the spotlit target's rect, or null.
    function place(now, sz, dt, spot) {
      var a = sizeOf(asst, sz), b = sizeOf(wrap, sz), k = sizeOf(clippy.c, sz), t = sizeOf(tr, sz);
      var cx = st.cx * sz.w, cy = st.cy * sz.h;

      // A placed spot from C++ wins whenever it changes (a drag elsewhere, `pos reset`).
      var posKey = st.px >= 0 && st.py >= 0 ? st.px + ',' + st.py : '';
      if (posKey !== seenPos && !drag) { seenPos = posKey; userPos = posKey ? { fx: st.px, fy: st.py } : null; }

      var left, bottom; // css px: the assistant's left edge and bottom edge
      if (userPos) {
        left = clamp(userPos.fx * sz.w, 0, Math.max(0, sz.w - a.w));
        bottom = clamp(userPos.fy * sz.h, Math.min(a.h, sz.h), sz.h);
      } else {
        var wantL = LEFT, wantB = sz.h - lowMargin(sz);
        var avoid = a.h > 0 ? avoidRects(sz, spot, now) : [];
        if (spot && a.h > 0) { var c = chooseSpot(spot, a, sz, avoid); wantL = c.left; wantB = c.bottom; }
        else if (a.h > 0) {
          if (slot === 'high') wantB = Math.min(wantB, TOP_PAD + a.h);
          if (avoid.length) { var c2 = chooseSpot(null, a, sz, avoid, { left: wantL, bottom: wantB }); wantL = c2.left; wantB = c2.bottom; }
        }
        if (curL === null) { curL = wantL; curB = wantB; }
        var k8 = Math.min(1, dt * 8);
        curL += (wantL - curL) * k8; curB += (wantB - curB) * k8;
        if (Math.abs(wantL - curL) < 0.5) curL = wantL;
        if (Math.abs(wantB - curB) < 0.5) curB = wantB;
        left = curL; bottom = curB;
      }

      var press = !!st.lmb && !lastLmb;
      lastLmb = !!st.lmb;
      var handle = { x0: left, y0: bottom - b.h, x1: left + b.w - t.w - 18, y1: bottom - b.h + HANDLE_H };
      var onHandle = st.cvis && !st.closed && a.h > 0 && inside(handle, cx, cy);
      if (press && onHandle) drag = { dx: cx - left, dy: cy - bottom };
      if (drag) {
        if (st.lmb) {
          left = clamp(cx - drag.dx, 0, Math.max(0, sz.w - a.w));
          bottom = clamp(cy - drag.dy, Math.min(a.h, sz.h), sz.h);
          userPos = { fx: frac(left / sz.w), fy: frac(bottom / sz.h) };
        } else {
          drag = null;
          seenPos = userPos.fx + ',' + userPos.fy;
          cmd('pos ' + userPos.fx + ' ' + userPos.fy);
        }
      }
      if ((onHandle || !!drag) !== gripLit) {
        gripLit = onHandle || !!drag;
        grip.style.backgroundColor = gripLit ? C.muted : C.line;
      }
      left = Math.round(left); bottom = Math.round(bottom);
      asst.style.marginLeft = left + 'px';
      var mb = Math.round(sz.h - bottom);
      asst.style.marginBottom = mb + 'px';

      var parts = [
        { x0: left, y0: bottom - b.h, x1: left + b.w, y1: bottom },
        { x0: left + b.w + GAP, y0: bottom - k.h, x1: left + b.w + GAP + k.w, y1: bottom }
      ];
      var onUs = st.cvis && (inside(parts[0], cx, cy) || inside(parts[1], cx, cy));
      asstParts = a.h > 0 ? parts : null;

      // Fade while the user works anything else: a held button elsewhere, or (in game) flying
      // the camera with no cursor on screen. Never while he is being dragged.
      if (st.lmb && !onUs && !drag) held = true;
      if (held && !st.lmb) { held = false; workUntil = now + 1100; }
      var working = !drag && (held || now < workUntil || (st.ctx === 'hud' && !st.cvis));
      var target = working ? 0.2 : 1;
      opacity += (target - opacity) * Math.min(1, dt * 12);
      if (Math.abs(target - opacity) < 0.01) opacity = target;
      asst.style.opacity = opacity.toFixed(3);
      // Faded, he lets clicks through to what is under him; never on a linear step, where the
      // only thing under him a click could reach is something the step did not ask for.
      asst.hittestchildren = !working || clickMode !== 'free';

      // Hover buffer, only while he is in one of his own spots.
      var box = { x0: left - RING, y0: bottom - a.h - RING, x1: left + a.w + RING, y1: bottom + RING };
      var inRing = !userPos && !spot && st.cvis && !working && !onUs && a.h > 0 && inside(box, cx, cy);
      if (!inRing) { dwellAt = 0; }
      else {
        if (!dwellAt) dwellAt = now;
        if (now - dwellAt >= 500 && now - lastMoveAt >= 2000) {
          slot = slot === 'low' ? 'high' : 'low';
          lastMoveAt = now; dwellAt = 0;
        }
      }
      // Pupils follow a visible cursor; otherwise he glances around.
      var ex = left + b.w + GAP + k.w * 0.52, ey = bottom - k.h * 0.38;
      return st.cvis ? { x: (cx - ex) / RIG, y: (cy - ey) / RIG } : null;
    }

    $.MvmOnboarding = {
      render: function () {
        try { st = JSON.parse(root.GetAttributeString('state', '{}')); } catch (e) { return; }
        var now = Date.now(), dt = Math.min(0.1, Math.max(0, (now - lastFrameAt) / 1000));
        lastFrameAt = now;
        root.visible = !!st.on && !st.hidden;
        if (!root.visible) { if (speaking) stopSpeech(); wasHidden = true; return; }

        chatterL.l.text = st.voice ? 'Chatter on' : 'Chatter off';
        chatterL.l.style.color = st.voice ? C.gold : C.muted;

        var stp = stepNow();
        var key = (st.route || '') + '|' + st.step + '|' + (stp ? ctxOk(stp) : '') + '|' + !!st.closed;
        if (key !== lineKey || st.rev !== seenRev) {
          var revChanged = st.rev !== seenRev;
          lineKey = key; seenRev = st.rev;
          fill();
          if (revChanged || wasHidden) {
            if (isFinale) clippyFall(clippy, true); else clippyHop(clippy);
            startSpeech();
          }
        }
        wasHidden = false;
        if (swoon && !speaking) {
          if (!swoonEnd) swoonEnd = now + 2500;
          else if (now >= swoonEnd) { swoon = false; showLine(currentLine()); }
        }

        lessonPending(now);
        var sz = cssSize();
        var spot = st.closed ? null : lessonFrame(now, sz);
        // Asking whether to leave: no markers, no key lock, nothing blocking clicks.
        if (st.closed) { hideMarkers(); lessonInput(null, false, null); }
        spotlight(spot, sz, dt, now);
        var look = place(now, sz, dt, spot);
        sendClickZones(sz, asstParts);
        var deg = tickSpeech(now);
        for (var i = 0; i < 3; i++) {
          var hgt = speaking ? 3 + 7 * (0.5 + 0.5 * Math.sin(now / 1000 * Math.PI * 2 / 0.9 + i * 1.7)) : 3;
          beats[i].style.height = hgt.toFixed(1) + 'px';
        }
        clippyFrame(clippy, now, deg, speaking, look);
      }
    };
  } catch (e) { $.Msg('[onboarding] build failed: ' + e + '\n'); }
})();
)OBJS";

} // namespace Filmmaker
