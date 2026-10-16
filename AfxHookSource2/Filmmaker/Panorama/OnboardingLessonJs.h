#pragma once

// LESSON fragment of the onboarding panel (concatenated into kOnboardingJs, shares its scope): runs
// the steps OnboardingScriptJs.h describes.
//
//   * Entering a step (once, in its own context, a beat after it appears): its enter() runs, the
//     camera-key count and scene-key revision are noted, and its world markers are armed.
//   * Every frame: the target is resolved and spotlit, markers are drawn, a camera key dropped
//     near a marker ticks it off, and done(app) is checked. When the step is done Clippy hops and
//     the lesson moves on by itself.
//   * Demo seeks go through `onboarding seek`, which lands paused on the tick. Config cards are
//     queued, because $.ConfigHud exists only once CONFIG has built.

R"OBJS(
    var ctxTop = ctx;
    while (ctxTop.GetParent && ctxTop.GetParent()) ctxTop = ctxTop.GetParent();
    function raw(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[onboarding] raw command failed: ' + e + '\n'); }
    }
    var pendingCard = null, pendingCardUntil = 0;
    // Jump the practice demo to a lesson's moment and leave it paused there (Onboarding.cpp).
    var lastSeekTick = null;
    function seek(tick) { lastSeekTick = tick; cmd('seek ' + tick); }
    // Let the demo play up to a tick, then pause it: the time between two camera keys. The driver
    // does the resume and the pause (`onboarding playto`); the panel only waits for it.
    var playTarget = null, playUntil = 0;
    function playTo(tick) { playTarget = tick; playUntil = Date.now() + 8000; cmd('playto ' + tick); }
    // CS2's roaming free camera: WASD to fly, hold the mouse to look around.
    function freeCam() { raw('spec_mode 4'); }
    function cfgCard(key) { pendingCard = key; pendingCardUntil = Date.now() + 4000; }
    function lessonPending(now) {
      if (playTarget !== null) {
        var ap = st.app || {};
        // The driver stops the roll within two ticks of the target (OnboardingHud.cpp); waiting
        // for the exact tick held Camera 2 back until the 8 s timeout.
        if ((ap.paused && (ap.tick || 0) + 3 >= playTarget) || now > playUntil) playTarget = null;
      }
      if (pendingCard) {
        if (now > pendingCardUntil) pendingCard = null;
        else if ($.ConfigHud && $.ConfigHud.card) {
          try { $.ConfigHud.card(pendingCard, 1); } catch (e) {}
          pendingCard = null;
        }
      }
    }

    // ---- targets
    var idCache = {};
    function findById(spec, sz, now) {
      var c = idCache[spec];
      if (c && c.p && rectOf(c.p, sz)) return c.p;
      if (c && now < c.next) return null;
      var p = null, gt = spec.indexOf('>');
      if (gt > 0) {
        var parent = ctxTop.FindChildTraverse(spec.substring(0, gt));
        p = parent ? parent.FindChildTraverse(spec.substring(gt + 1)) : null;
      } else p = ctxTop.FindChildTraverse(spec);
      idCache[spec] = { p: p, next: now + 250 };
      return rectOf(p, sz) ? p : null;
    }
    function resolve(spec, sz, now) {
      if (typeof spec === 'function') { try { return spec(); } catch (e) { return null; } }
      if (spec.indexOf('tour:') === 0) {
        var list = ($.MvmTour || {})[spec.substring(5)] || [];
        for (var i = list.length - 1; i >= 0; i--) if (rectOf(list[i], sz)) return list[i];
        return null;
      }
      if (spec.indexOf('cfg:') === 0) {
        var parts = spec.split(':');
        try { return $.ConfigHud && $.ConfigHud.cardPanel ? $.ConfigHud.cardPanel(parts[1], parts[2]) : null; }
        catch (e) { return null; }
      }
      return findById(spec, sz, now);
    }
    function targetRect(target, sz, now) {
      if (!target) return null;
      var list = (typeof target === 'string' || typeof target === 'function') ? [target] : target;
      for (var i = 0; i < list.length; i++) {
        var r = rectOf(resolve(list[i], sz, now), sz);
        if (r) return r;
      }
      return null;
    }
    function guideVisible(spec) { return !!targetRect(spec, cssSize(), Date.now()); }
    // Controls he must never stand on: a click meant for him would land on them (START RENDER,
    // RENDER). The lit target is left out; it gets its own berth.
    var AVOID = ['tour:render.start', 'tour:editor.render'];
    function avoidRects(sz, spot, now) {
      var out = [];
      for (var i = 0; i < AVOID.length; i++) {
        var r = rectOf(resolve(AVOID[i], sz, now), sz);
        if (r && !(spot && Math.abs(r.x - spot.x) < 1 && Math.abs(r.y - spot.y) < 1)) out.push(r);
      }
      return out;
    }

    // ---- main menu: the Watch page's DOWNLOADED tab
    // Same lookup as FilmmakerGuiJs.h's downloadedButton(): the native RadioButton has no id we can
    // rely on, and the centre of the Watch navbar is [Your Matches, Downloaded, Tournaments].
    var menuRefs = {};
    function menuPanel(id) {
      var p = menuRefs[id];
      if (p && p.IsValid && p.IsValid()) return p;
      p = ctxTop.FindChildTraverse(id);
      menuRefs[id] = p;
      return p;
    }
    function downloadedTab() {
      var page = menuPanel('JsWatchContent');
      var nav = page && page.FindChildTraverse('watch-navbar');
      var center = nav && nav.GetChild(0);
      return center && center.GetChildCount() >= 2 ? center.GetChild(1) : null;
    }
    // The Downloaded list is on screen: our body is built and its native page is not hidden.
    function downloadedOpen() {
      var jd = menuPanel('JsDownloaded');
      if (jd && jd.BHasClass && jd.BHasClass('WatchMenu--Hide')) return false;
      return guideVisible('FmDownloadedBody');
    }
    // "Did the user click it": an extra listener on the panel's own Activated event, next to the
    // handler its owner set (SetPanelEvent would replace that). Returns a getter that is true once
    // a click lands while the step that asked is still on screen.
    var clickSeen = {};
    function watchClicks(id) {
      var p = menuPanel(id), key = run.key;
      if (p && !p.__mvmObWatched) {
        p.__mvmObWatched = true;
        try { $.RegisterEventHandler('Activated', p, function () { clickSeen[id] = run.key; }); } catch (e) {}
      }
      return function () { return clickSeen[id] === key; };
    }

)OBJS"
R"OBJS(
    // ---- what a step needs open
    // A step names the state it is taught in (`needs`); entering it puts the game there, whatever
    // the user had open before: CONFIG or CAM EDITOR, the Render workspace and its tab, the dock
    // tab, the inspector tab, the graph mode. Retried for a few seconds, because opening the editor
    // takes a frame before its tabs and $.CamEditor exist.
    function needsMet(n, a) {
      if (n.config && !a.config) return false;
      if (n.editor && !a.editor) return false;
      if (n.render && !a.render) return false;
      if (!n.render && a.render) return false; // a step that does not teach Render starts without it
      if (n.renderTab != null && (n.renderTab === 0 ? a.renderTab > 1 : a.renderTab !== n.renderTab)) return false;
      if (n.dock && a.dock !== n.dock) return false;
      if (n.insp && a.insp !== n.insp) return false;
      if (n.graphMode && (a.graphMode === 'scene') !== (n.graphMode === 'scene')) return false;
      if (n.freecam && a.demo && !a.freeCam) return false;
      if (n.pov && a.demo && !a.pov) return false;
      if (n.camKeys && a.demo && (a.keys || 0) < n.camKeys) return false;
      if (n.sel && a.demo && !(a.sel >= 0)) return false;
      if (n.card && !guideVisible('cfg:' + n.card + ':header')) return false;
      if (n.reveal && !guideVisible(n.reveal.shows)) return false;
      if (n.menu === 'watch' && !guideVisible(downloadedTab)) return false;
      if (n.menu === 'downloaded' && !downloadedOpen()) return false;
      return true;
    }
    function applyNeeds(n, a) {
      // The free camera: a lesson seek lands in whatever camera the demo had, so it is asked for
      // again until it sticks.
      if (n.freecam && a.demo && !a.freeCam) freeCam();
      // First person: the weapon lessons look through the player's eyes. Our free cam off, then
      // CS2's in-eye view of the practice demo's player.
      if (n.pov && a.demo && !a.pov) { raw('spec_player aitch'); cmd('pov'); }
      // Camera keys: a practice session starts every demo empty, so a step reached by Skip, Back
      // or a resume has none until the first fight's two are placed.
      if (n.camKeys && a.demo && (a.keys || 0) < n.camKeys) ensureKeys();
      if (n.sel && a.demo && !(a.sel >= 0)) raw('mirv_filmmaker camtl select 0');
      // The main menu: the Watch page, then its DOWNLOADED tab, clicked the way the mouse would.
      if (n.menu) {
        if (!guideVisible(downloadedTab)) activate(menuPanel('MainMenuNavBarWatch'));
        else if (n.menu === 'downloaded' && !downloadedOpen()) activate(downloadedTab());
        return;
      }
      if (n.config) {
        if (!a.config) { raw('mirv_filmmaker config on'); return; }
        // The card the step points into: CONFIG keeps whichever one the user left open.
        if (n.card && !guideVisible('cfg:' + n.card + ':header')) cfgCard(n.card);
      }
      if (n.editor && !a.editor) { raw('mirv_filmmaker editor on'); return; }
      if (n.editor && !a.editor) { raw('mirv_filmmaker editor on'); return; }
      if (n.render && !a.render) { raw('mirv_filmmaker editor render'); return; }
      if (!n.render && a.render) { raw('mirv_filmmaker render close'); return; }
      if (n.renderTab != null && (n.renderTab === 0 ? a.renderTab > 1 : a.renderTab !== n.renderTab))
        raw('mirv_filmmaker render tab ' + n.renderTab);
      if (n.dock && a.dock !== n.dock) raw('mirv_filmmaker dock ' + n.dock);
      if (n.insp && a.insp !== n.insp) { try { $.CamEditor.gotoInspector(n.insp); } catch (e) {} }
      if (n.graphMode && (a.graphMode === 'scene') !== (n.graphMode === 'scene'))
        raw('mirv_filmmaker settings ui editor.graph ' + n.graphMode);
      // A card inside a workspace that has to be opened by its own button (TIME OF DAY).
      if (n.reveal && !guideVisible(n.reveal.shows)) activate(resolve(n.reveal.panel, cssSize(), Date.now()));
    }

)OBJS"
R"OBJS(
    // ---- steps
    // A step moves on by itself only when the user does its job while it is up. A step that is
    // already done when it comes up (its `needs` opened the thing it points at, the setting was
    // made earlier, the user came back with Back) only shows where things are: it waits for Next.
    var run = { key: '' };
    var lockSent = '';
    function sendLock(spec) { if (spec !== lockSent) { lockSent = spec; cmd('lock ' + spec); } }
    // Linear mode for the step on screen: the hotkeys it names (Onboarding.cpp swallows the rest)
    // and which clicks get through. Off on the opening, the endings, bridges and while closed.
    function lessonInput(step, live, spot) {
      if (!live) { sendLock('off'); setClicks('free', false); return; }
      sendLock('on' + (step.keys && step.keys.length ? ' ' + step.keys.join(' ') : ''));
      setClicks(step.click || (step.target ? 'target' : 'none'), !!spot);
    }
    function ctxOk(step) { return !step.ctx || step.ctx === 'any' || step.ctx === st.ctx; }
    function stepNow() {
      var r = routeOf();
      return r && st.step >= 0 && st.step < r.steps.length ? r.steps[st.step] : null;
    }
    function sceneKeyed(a) { return (a.sceneRev || 0) !== run.sceneRev0; }
    function allPlaced(marks) {
      for (var i = 0; i < marks.length; i++) if (!marks[i].done) return false;
      return marks.length > 0;
    }
    // Returns the rect to spotlight this frame, or null.
    function lessonFrame(now, sz) {
      var step = stepNow(), a = st.app || {};
      var key = (st.route || '') + '|' + st.step + '|' + st.rev;
      if (key !== run.key) {
        run = { key: key, since: now, entered: false, marks: [], advanceAt: 0, lastKeys: 0, sceneRev0: 0,
          checked: false, preDone: false };
      }
      if (!step || st.closed) { hideMarkers(); lessonInput(null, false, null); return null; }
      var ok = ctxOk(step);
      if (ok && !run.entered && now - run.since > 300) {
        run.entered = true;
        run.lastKeys = a.keys || 0;
        run.sceneRev0 = a.sceneRev || 0;
        run.marks = [];
        var ms = step.markers || [];
        for (var i = 0; i < ms.length; i++)
          run.marks.push({ name: ms[i].name, pos: ms[i].pos, ang: ms[i].ang, tick: ms[i].tick, done: false });
        // A camera path still playing from an earlier step would hold the view with J locked.
        var allowsJ = step.keys && step.keys.indexOf('J') >= 0;
        if (a.pathPlaying && !allowsJ) raw('mirv_filmmaker marker previewstop');
        run.needsUntil = step.needs ? now + 8000 : 0;
        run.needsNext = 0;
        if (step.enter) {
          try { step.enter(); } catch (e) { $.Msg('[onboarding] step enter: ' + e + '\n'); }
        }
      }
      if (ok && run.entered && step.needs && !run.needsHeld && now < run.needsUntil && now >= run.needsNext) {
        if (needsMet(step.needs, a)) run.needsHeld = true;
        else { run.needsNext = now + 600; applyNeeds(step.needs, a); }
      }
      // A step about one camera key looks through it: once the step's needs are in place the
      // selected key (or the first) is selected again, which jumps the demo to its tick and the
      // view to its pose (CameraPath::SelectForEditor), whatever camera was up before.
      if (ok && run.entered && step.showKey && !run.keyShown && (!step.needs || run.needsHeld) && a.demo && (a.keys || 0) > 0) {
        run.keyShown = true;
        raw('mirv_filmmaker camtl select ' + (a.sel >= 0 ? a.sel : 0));
      }
      // A graph step frames the keys: playback pages the graph's window to the playhead, which can
      // leave every key off screen and the canvas empty (nothing to click or right-click).
      // Only on a paused demo: while it plays the graph pages its window back to the playhead.
      if (ok && run.entered && step.fitGraph && !run.fitted && (!step.needs || run.needsHeld)
          && (!a.demo || (a.paused && !a.pathPlaying))) {
        if (!run.fitAt) run.fitAt = now + 500; // the dock and its model settle for a few frames
        else if (now >= run.fitAt && $.GraphExp && $.GraphExp.fit) { run.fitted = true; try { $.GraphExp.fit(); } catch (e) {} }
      }
      // The free camera alone keeps being asked for: a seek that lands later puts the demo's
      // own camera back.
      if (ok && run.entered && step.needs && step.needs.freecam && now < run.needsUntil && a.demo && !a.freeCam
          && now >= (run.camNext || 0)) { run.camNext = now + 600; freeCam(); }
      // A camera step starts the free camera a short flight from its first marker (startPose),
      // once the free cam is up and the seek has landed, and only in its first seconds. CS2's own
      // spec_goto moves its roaming camera; a MirvInput pose override does not.
      if (ok && run.entered && step.startPose && !run.posed && now - run.since < 12000 && a.freeCam
          && a.paused && lastSeekTick !== null && Math.abs((a.tick || 0) - lastSeekTick) <= 4) {
        run.posed = true;
        raw('spec_goto ' + step.startPose.pos.join(' ') + ' ' + step.startPose.ang.join(' '));
      }
      if (ok && run.entered && run.marks.length) {
        // One marker at a time: the placed ones stay (green), the next one shows once the demo has
        // rolled to its tick. A key dropped while the camera sits at it ticks it off, and the demo
        // then plays on to the following marker's moment and pauses there.
        var showing = [], cur = -1;
        for (var mi = 0; mi < run.marks.length; mi++) {
          if (run.marks[mi].done) showing.push(run.marks[mi]);
          else { cur = mi; break; }
        }
        if (cur >= 0 && playTarget === null) showing.push(run.marks[cur]);
        var near = worldMarkers(showing, sz), keys = a.keys || 0;
        var nearMark = near >= 0 ? showing[near] : null;
        if (keys > run.lastKeys && nearMark && !nearMark.done) {
          nearMark.done = true;
          var nx = run.marks[cur + 1];
          if (nx && typeof nx.tick === 'number') playTo(nx.tick);
        }
        run.lastKeys = keys;
      } else hideMarkers();
      // The first look waits until the step's `needs` are in place (or stopped being retried), so
      // what they open counts as "already there", not as the user's doing.
      var settled = !step.needs || !ok || run.needsHeld || now >= run.needsUntil;
      if (!run.advanceAt && !run.preDone && settled && (ok ? run.entered : !!step.anyCtx) && now - run.since > 900) {
        var fin = false;
        try { fin = step.done ? !!step.done(a) : allPlaced(run.marks); } catch (e) {}
        // Only in the step's own context: WATCH is done by the very switch to the demo, and the
        // panel rebuilt there sees it done at once.
        if (fin && !run.checked && ok) {
          run.preDone = true;
          // Its own words for "already done", when it has them (doneBody).
          if (step.doneBody) {
            showLine({ title: step.title, body: step.doneBody, expr: step.expr, action: 'Next' });
            startSpeech();
          }
        }
        else if (fin) { run.advanceAt = now + 700; clippyHop(clippy); }
        run.checked = true;
      }
      if (run.preDone && !st.closed && !goB.p.visible) { goB.l.text = 'Next'; goB.p.visible = true; }
      if (run.advanceAt && now >= run.advanceAt) {
        run.advanceAt = 1e15; // sent once; the rev bump that comes back starts the next step
        cmd('go ' + st.route + ' ' + (st.step + 1));
      }
      var spot = ok && run.entered ? targetRect(step.target, sz, now) : null;
      clickArea = ok && run.entered && step.clickArea ? targetRect(step.clickArea, sz, now) : null;
      // Never trapped. A step whose lit control cannot be found 3 s after its needs settled
      // (something the needs could not open) lets every click through, so the user can get there
      // by hand, and says so in the console. Skip still does the step.
      if (spot) run.spotAt = now;
      var lost = ok && run.entered && step.target && !spot && settled
        && now - Math.max(run.spotAt || 0, run.since) > 3000;
      if (lost && !run.lostSaid) {
        run.lostSaid = true;
        $.Msg('[onboarding] step ' + st.route + ' ' + st.step + ': its control is not on screen; clicks are free\n');
      }
      lessonInput(lost ? { keys: step.keys, click: 'free' } : step, ok && run.entered, spot);
      return spot;
    }
)OBJS"
