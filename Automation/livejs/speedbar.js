// Demo-bar SPEED SCRUBBER -- live-reloadable development copy.
//
//   mirv_filmmaker livejs watch <this file>
//
// Save and it re-runs immediately. When it is right, paste the body into
// AfxHookSource2/Filmmaker/Panorama/DemoBarButtonsJs.h.
//
// ============================ WHY IT IS BUILT THIS WAY ============================
//
// INPUT: the raw OS cursor, not a Panorama Slider.
//   A native Slider eases its own thumb AND only reports on SliderValueChanged, whose cadence is
//   not the frame rate. A fast flick therefore arrived late and in coarse chunks -- the reported
//   "drag it left and right really fast and it just glitches / takes forever to catch up".
//   DemoBarButtons.cpp publishes the real cursor onto this row every frame as "mirvcursor"
//   (client device px, same space GetPositionWithinWindow reports in), and the drag is computed
//   from it directly. The value is now exact and frame-locked; there is nothing left to lag.
//
// MOTION: transform, not position.
//   CS2's own stylesheets animate `transform` 428x vs `position` 82x. transform is the GPU path.
//
// NO PER-FRAME BLUR FILTER.
//   `blur` IS animatable in Panorama (value: `gaussian( x, y, passes )`, not a length), but
//   rewriting it every frame on this 46-child container took CS2 down twice -- a clean exit with
//   no crash dump, i.e. what a lost D3D device looks like. The smear is faked with line width and
//   field opacity instead, which allocates nothing.
//
// ONE OWNER AT A TIME.
//   Re-injection deletes the old row but cannot stop the old $.Schedule loop, which then touches
//   deleted panels -- a native fault, not a catchable JS error, so the process exits with no crash
//   dump. A generation token on `$` makes every superseded loop retire on its next wake-up.
//
// WIDTH IS ADAPTIVE.
//   A fixed width overflowed CS2's control row once the demo bar is docked narrow (Regular
//   Timeline mode shrinks it to fit left of the inspector) and clipped the CONFIG button. The
//   strip takes a clamped share of the bar's live width instead.
//
// MOTION IS A RESPONSE, NOT AN AMBIENT LOOP.
//   The field only flows while it is being dragged, and coasts to a stop afterwards. A strip that
//   animates while untouched reads as a bug, not as life.
//
// EASING: split by element, and adaptive.
//   The field's scroll transform is rewritten every frame, so ANY easing on it is pure lag -> 0s.
//   The per-line transforms ease, which is what makes a density change morph instead of teleport.
//   But a constant line easing chases a continuously-moving target during a fast drag, which is
//   its own kind of lag -- so the line easing is dropped to ~0 while the pointer is moving fast
//   and restored the moment it settles. Motion blur covers the difference.

(function () {
  try {
    var ctx = $.GetContextPanel();
    var speed = ctx.FindChildTraverse('SpeedControls');
    if (!speed) { $.Msg('[speedbar] SpeedControls not found\n'); return; }

    // Claim ownership BEFORE deleting the old row, so the previous loop stops on its next wake.
    var myGen = (($.MirvSpeedGen || 0) + 1);
    $.MirvSpeedGen = myGen;

    var prior = ctx.FindChildTraverse('MirvSpeedRow');
    if (prior) prior.DeleteAsync(0);
    // CS2's 2026-10-14 bar puts its own stepped speed Slider beside the #TimeScale readout in
    // #SpeedControls. This dial replaces both, so both are hidden; RestoreNative shows them again.
    var ts = ctx.FindChildTraverse('TimeScale');
    if (ts) ts.visible = false;
    var nativeSpeed = ctx.FindChildTraverse('SpeedSliderContainer');
    if (nativeSpeed) nativeSpeed.visible = false;

    // The dial takes the room the bar's buttons leave free (see refitWidth), capped at W_MAX and
    // never more than W_SHARE of the bar.
    // W_MIN stays generous: a docked bar is scaled as a whole (CameraTimelineJs fitControlRow), so
    // shrinking the dial as well left it unreadably small in the Camera Editor and Config.
    var W_MIN = 300, W_MAX = 420, W_SHARE = 0.26;
    var W = W_MAX, H = 36;   // W is re-derived from the live bar width each frame
    var MIN_SPEED = 0.05, MAX_SPEED = 20, RATIO = MAX_SPEED / MIN_SPEED;
    var SNAP = 0.02;
    var POOL = 46;
    var SP_SLOW = 30, SP_FAST = 9;
    var COL_SLOW = [78, 150, 255], COL_MID = [236, 241, 247], COL_FAST = [240, 179, 35];

    // ---- click ZONES ---------------------------------------------------------------------
    // The strip is five equal click targets. FIVE, so the middle one is dead centre -- and the
    // centre of the strip is already exactly where 1x lives (speedFor snaps v=0.5 to 1), so
    // "click the middle, land on 1x" needs no special case. CLICK (press + release without
    // travelling) snaps to that zone's speed; DRAG still scrubs continuously, so the zones cost
    // the existing gesture nothing.
    //
    // WHY THESE FIVE SPEEDS AND NOT 0.25/0.5/1/2/4. The track is LOGARITHMIC over 0.05x..20x, so
    // 0.25/0.5/1/2/4 all land inside the middle 46% of the strip: their labels would overlap each
    // other and, worse, a click on the left-hand fifth would leave the handle sitting two zones
    // away from the label you just clicked. These five are picked so each speed's real handle
    // position falls essentially ON its zone's centre (0.116 / 0.269 / 0.5 / 0.731 / 0.884 vs
    // centres 0.1 / 0.3 / 0.5 / 0.7 / 0.9) -- and they are symmetric, because 0.1*10 = 0.25*4 = 1.
    // That is what makes the labels, the ticks and the handle all agree.
    var ZONES = [0.1, 0.25, 1, 4, 10];
    var ZONE_LBL = ['0.1x', '0.25x', '1x', '4x', '10x'];
    var CLICK_SLOP = 4;   // device px of travel that still reads as a click rather than a drag

    function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    function lerp(a, b, t) { return a + (b - a) * t; }
    function speedFor(v) {
      v = clamp01(v);
      if (Math.abs(v - 0.5) < SNAP) return 1;
      return MIN_SPEED * Math.pow(RATIO, v);
    }
    function sliderFor(sp) {
      if (!(sp > 0)) return 0.5;
      return clamp01(Math.log(sp / MIN_SPEED) / Math.log(RATIO));
    }
    function fmtSpeed(sp) {
      if (sp >= 10) return sp.toFixed(1) + 'x';
      if (sp >= 1) return (Math.abs(sp - Math.round(sp)) < 0.005 ? sp.toFixed(0) : sp.toFixed(2)) + 'x';
      if (sp >= 0.1) return sp.toFixed(2) + 'x';
      return sp.toFixed(3) + 'x';
    }
    function rgba(c, a) { return 'rgba(' + (c[0] | 0) + ',' + (c[1] | 0) + ',' + (c[2] | 0) + ',' + a + ')'; }
    function tintFor(v) {
      var a, b, t;
      if (v < 0.5) { a = COL_SLOW; b = COL_MID; t = v / 0.5; }
      else { a = COL_MID; b = COL_FAST; t = (v - 0.5) / 0.5; }
      return [lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)];
    }

    var row = $.CreatePanel('Panel', speed, 'MirvSpeedRow', {});
    row.style.flowChildren = 'right';
    row.style.verticalAlign = 'center';

    // Fixed footprint: everything inside is absolutely positioned or transformed, so no amount of
    // line-spawning or blurring can reflow the demo bar around it.
    var strip = $.CreatePanel('Panel', row, '', {});
    strip.style.width = W + 'px'; strip.style.height = H + 'px';
    strip.style.verticalAlign = 'center';
    strip.hittest = true;
    strip.SetPanelEvent('onactivate', function () { /* swallow: the drag is cursor-driven */ });
    strip.style.borderRadius = '4px';
    strip.style.border = '1px solid #ffffff1a';
    strip.style.backgroundColor =
      'gradient( linear, 0% 0%, 100% 0%, from( #05070ae8 ), color-stop( 0.5, #0b0f14b8 ), to( #05070ae8 ) )';

    // Clipper for the scrolling field. The handle's halo lives OUTSIDE it so the glow can bleed.
    var clip = $.CreatePanel('Panel', strip, '', {});
    clip.hittest = false;
    clip.style.width = '100%'; clip.style.height = '100%';
    clip.style.overflow = 'clip';

    var field = $.CreatePanel('Panel', clip, '', {});
    field.hittest = false;
    field.style.width = '100%'; field.style.height = '100%';
    field.style.transitionProperty = 'opacity';
    field.style.transitionDuration = '0.10s';
    field.style.transitionTimingFunction = 'ease-out';

    var lines = [];
    for (var i = 0; i < POOL; i++) {
      var p = $.CreatePanel('Panel', field, '', {});
      p.hittest = false;
      p.style.position = '0px 0px 0px';
      p.style.width = '2px';
      p.style.height = '100%';
      p.style.verticalAlign = 'center';
      p.style.transitionProperty = 'transform, width, height, background-color';
      p.style.transitionDuration = '0.13s';
      p.style.transitionTimingFunction = 'ease-out';
      lines.push(p);
    }

    var centreTick = $.CreatePanel('Panel', clip, '', {});
    centreTick.hittest = false;
    centreTick.style.width = '1px'; centreTick.style.height = '100%';
    centreTick.style.position = (W / 2) + 'px 0px 0px';
    centreTick.style.backgroundColor = '#ffffff24';

    var handle = $.CreatePanel('Panel', strip, '', {});
    handle.hittest = false;
    handle.style.width = '4px'; handle.style.height = '70%';
    handle.style.verticalAlign = 'center';
    handle.style.position = '0px 0px 0px';
    handle.style.borderRadius = '2px';
    handle.style.border = '1px solid #05070acc';   // keeps it from dissolving into the field
    handle.style.transitionProperty = 'width, height, background-color, box-shadow';
    handle.style.transitionDuration = '0.09s';
    handle.style.transitionTimingFunction = 'ease-out';

    // Readout. Centring is horizontalAlign/verticalAlign ONLY. A Label given width:100% or
    // height:100% anchors to its parent's top-left and the alignment properties stop applying --
    // that is why it kept sitting off-centre.
    var pill = $.CreatePanel('Panel', strip, '', {});
    pill.hittest = false;
    pill.style.width = '96px'; pill.style.height = '24px';
    pill.style.horizontalAlign = 'center'; pill.style.verticalAlign = 'center';
    // Fully opaque: at 0.88 the line field read straight through the readout and made the number
    // busy to parse at a glance.
    pill.style.backgroundColor = '#05070aff';
    pill.style.borderRadius = '12px';
    pill.style.border = '1px solid #ffffff22';
    // Fades back on hover so the five zone labels can be read through where it sits.
    pill.style.transitionProperty = 'opacity';
    pill.style.transitionDuration = '0.12s';
    pill.style.transitionTimingFunction = 'ease-out';

    var readout = $.CreatePanel('Label', pill, '', {});
    readout.hittest = false;
    readout.text = '1x';
    readout.style.horizontalAlign = 'center';
    readout.style.verticalAlign = 'center';
    readout.style.fontSize = '14px';
    readout.style.color = '#eef2f6ff';

    // Zone chrome: four divider ticks + five labels, INVISIBLE until the pointer is over the
    // strip. At rest this control is a readout and must stay one -- five permanent labels on a
    // 200px strip is noise -- so the affordance only appears when it is actionable. Created
    // AFTER the pill so they draw over it while it is faded back.
    // Geometry is written in layoutZones() from the LIVE W, because refitWidth() changes W at
    // runtime; a build-time position would be wrong the moment the demo bar is docked narrow.
    var zoneTicks = [], zoneLabels = [];
    for (var zt = 1; zt < ZONES.length; zt++) {
      var zdiv = $.CreatePanel('Panel', strip, '', {});
      zdiv.hittest = false;
      zdiv.style.position = '0px 0px 0px';
      zdiv.style.width = '1px'; zdiv.style.height = '100%';
      zdiv.style.backgroundColor = '#ffffff3d';
      zdiv.style.opacity = '0';
      zdiv.style.transitionProperty = 'opacity';
      zdiv.style.transitionDuration = '0.12s';
      zdiv.style.transitionTimingFunction = 'ease-out';
      zoneTicks.push(zdiv);
    }
    for (var zi = 0; zi < ZONES.length; zi++) {
      var zlab = $.CreatePanel('Label', strip, '', {});
      zlab.hittest = false;
      zlab.text = ZONE_LBL[zi];
      // Explicit width + textAlign (NOT width:100% / an align property): a Label sized to its
      // parent anchors to the parent's top-left and the alignment properties stop applying --
      // the same trap documented on the readout above.
      zlab.style.position = '0px 0px 0px';
      zlab.style.textAlign = 'center';
      zlab.style.fontSize = '11px';
      zlab.style.fontWeight = 'bold';
      zlab.style.color = '#c6cedaff';
      zlab.style.opacity = '0';
      zlab.style.transitionProperty = 'opacity, color';
      zlab.style.transitionDuration = '0.12s';
      zlab.style.transitionTimingFunction = 'ease-out';
      zoneLabels.push(zlab);
    }

    // ---- state ----
    var curV = 0.5, hot = false, dragging = false;
    var pressX = 0, pressMoved = false;   // click-vs-drag discrimination for the zones
    var hotZone = -1, lastPaintedZone = -2;   // -2 = never painted, so the first pass always writes
    var scroll = 0, vel = 0, flow = 0, smear = 0, lastSmear = -1;
    var lastSpacing = -1, lastTintKey = '', lineEaseFast = false;
    var lastLmb = false, lastSent = null;

    // Re-derive the strip width from the space the demo bar actually has. Returns true when the
    // width changed, so the caller can force a re-layout (spacing and the handle both key off W).

    var lastAvail = -1;
    // FREE ROOM IN THE ROW. The dial sits between two fill-parent-flow spacers (CameraTimelineJs
    // arrangeRow: #MvmSpeedSpacer and CS2's #ControlRowSpacer), so their combined width IS the room
    // it may grow into. When ControlRowSpacer hits 0 the native script starts collapsing its own
    // labels (UpdateControlRowFit), so the dial leaves SPACER_KEEP px alone. The dial and the
    // spacers trade width one-for-one, so re-measuring every frame converges; the 6px deadband
    // stops it hunting.
    var SPACER_KEEP = 48;
    function barFreeWidth(sx) {
      var f = ctx.FindChildTraverse('ControlRowSpacer');
      if (!f || !f.visible) return 0;
      var m = ctx.FindChildTraverse('MvmSpeedSpacer');
      var w = (f.actuallayoutwidth || 0) + ((m && m.actuallayoutwidth) || 0);
      return w / sx - SPACER_KEEP;
    }
    function refitWidth() {
      // Measure from the demo bar's CONTENTS, not from ControlRow. The camera timeline resizes
      // ControlRow to its content width while a workspace is docked (CameraTimelineJs, "THE
      // TRANSPORT ROW HAS TO FIT THE DOCK"), and this strip is part of that content -- so keying
      // off the row would feed our own width back into the number that decides it. Contents is the
      // bar, which is what "a clamped share of the bar's live width" meant all along.
      var slider = ctx.FindChildTraverse('SliderRow');
      var host = (slider && slider.GetParent && slider.GetParent())
        || ctx.FindChildTraverse('ControlRow') || speed;
      if (!host) return false;
      var sx = host.actualuiscale_x || 1;
      var avail = (host.actuallayoutwidth || 0) / sx;
      if (!(avail > 40)) return false;
      if (Math.abs(avail - lastAvail) < 4 && Math.abs(barFreeWidth(sx)) < 6) return false;
      lastAvail = avail;
      var want = Math.round(Math.max(W_MIN, Math.min(W_MAX, avail * W_SHARE, W + barFreeWidth(sx))));
      if (Math.abs(want - W) < 6) return false;
      W = want;
      strip.style.width = W + 'px';
      centreTick.style.position = (W / 2) + 'px 0px 0px';
      lastSpacing = -1;            // force layout() to redistribute at the new width
      return true;
    }

    function setLineEase(fast) {
      if (lineEaseFast === fast) return;   // restyling 46 panels every frame would cost more than it saves
      lineEaseFast = fast;
      var d = fast ? '0.02s' : '0.13s';
      for (var i = 0; i < POOL; i++) lines[i].style.transitionDuration = d;
    }

    // Widen every line by the current smear. Called only when the smear changes materially, so
    // this is a handful of style writes per gesture rather than per frame.
    function setLineSmear(sm) {
      var w = Math.max(1, lerp(5, 1.5, curV)) * (1 + 2.2 * sm);
      for (var i = 0; i < POOL; i++) {
        var p = lines[i];
        if (p.style.opacity === '0') continue;
        p.style.width = w.toFixed(2) + 'px';
      }
    }

    // Zone geometry. Re-derived only when W actually changes (refitWidth moves it at runtime),
    // so this is nine style writes per resize, not per frame.
    var lastZoneW = -1;
    function layoutZones() {
      if (W === lastZoneW) return;
      lastZoneW = W;
      var zw = W / ZONES.length;
      for (var i = 0; i < zoneTicks.length; i++)
        zoneTicks[i].style.position = ((i + 1) * zw).toFixed(1) + 'px 0px 0px';
      var ly = Math.round((H - 14) / 2);
      for (var j = 0; j < zoneLabels.length; j++) {
        zoneLabels[j].style.width = zw.toFixed(1) + 'px';
        zoneLabels[j].style.position = (j * zw).toFixed(1) + 'px ' + ly + 'px 0px';
      }
    }
    function zoneAt(frac) {
      var i = Math.floor(clamp01(frac) * ZONES.length);
      if (i < 0) i = 0;
      if (i >= ZONES.length) i = ZONES.length - 1;
      return i;
    }
    // Reveal / highlight. Only writes when the (hover, hovered-zone) pair actually changes --

    // restyling ten panels every frame would cost more than the whole control.
    function paintZones() {
      var z = (hot && !(dragging && pressMoved)) ? hotZone : -1;
      if (z === lastPaintedZone) return;
      lastPaintedZone = z;
      var on = (z >= 0);
      for (var i = 0; i < zoneTicks.length; i++) zoneTicks[i].style.opacity = on ? '0.55' : '0';
      for (var j = 0; j < zoneLabels.length; j++) {
        zoneLabels[j].style.opacity = on ? (j === z ? '1' : '0.45') : '0';
        zoneLabels[j].style.color = (on && j === z) ? rgba(tintFor(sliderFor(ZONES[j])), 1) : '#c6cedaff';
      }
      pill.style.opacity = on ? '0.25' : '1';
    }
    // A click (no travel) is a preset, not a scrub: jump straight to that zone's speed. force=true
    // on applySpeed because the relative guard would swallow a small step such as 4x -> 10x mid-flick.
    function snapToZone(frac) {
      curV = sliderFor(ZONES[zoneAt(frac)]);
      vel = 0; flow = 0;
      setLineEase(false);
      layout(curV);
      paintChrome(curV);
      applySpeed(curV, true);
    }

    function layout(v) {
      var spacing = lerp(SP_SLOW, SP_FAST, v);
      var w = Math.max(1, lerp(5, 1.5, v));
      var tint = tintFor(v);
      var key = (tint[0] | 0) + ',' + (tint[1] | 0) + ',' + (tint[2] | 0);
      if (Math.abs(spacing - lastSpacing) < 0.05 && key === lastTintKey && smear === 0) return;
      lastSpacing = spacing; lastTintKey = key;
      for (var i = 0; i < POOL; i++) {
        var p = lines[i], x = i * spacing;
        if (x > W + spacing * 2) { p.style.opacity = '0'; continue; }
        var d = Math.abs((x - W / 2) / (W / 2)); if (d > 1) d = 1;
        p.style.transform = 'translateX(' + x.toFixed(2) + 'px)';
        p.style.width = w.toFixed(2) + 'px';
        p.style.height = (92 - 46 * d * d).toFixed(1) + '%';
        p.style.opacity = '1';
        p.style.backgroundColor = rgba(tint, (0.26 + 0.5 * (1 - d)).toFixed(3));
      }
    }

    function paintChrome(v) {
      layoutZones();   // cheap no-op unless W moved; keeps ticks/labels glued to a resized strip
      var tint = tintFor(v);
      var hw = dragging ? 9 : (hot ? 7 : 4);
      handle.style.width = hw + 'px';
      handle.style.height = (dragging ? 100 : (hot ? 88 : 70)) + '%';
      handle.style.transform = 'translateX(' + (v * (W - hw)).toFixed(2) + 'px)';
      handle.style.backgroundColor = rgba(tint, dragging ? 1 : (hot ? 0.98 : 0.88));
      handle.style.boxShadow = dragging ? (rgba(tint, 0.5) + ' 0px 0px 10px 1px')
                             : (hot ? (rgba(tint, 0.3) + ' 0px 0px 7px 0px')
                                    : (rgba(tint, 0) + ' 0px 0px 0px 0px'));
      readout.text = fmtSpeed(speedFor(v));
      readout.style.color = rgba(tint, 1);
      centreTick.style.backgroundColor = (Math.abs(v - 0.5) < SNAP) ? '#ffffff99' : '#ffffff24';
    }

    function applySpeed(v, force) {
      var sp = speedFor(v);
      // Relative guard: an absolute epsilon is meaningless at 20x and far too coarse at 0.05x.
      if (!force && lastSent !== null && Math.abs(sp - lastSent) < lastSent * 0.008) return;
      lastSent = sp;
      // `mirv_filmmaker timescale`, NOT `demo_timescale` -- it forwards to the engine AND records
      // the value, which is the only way the camera timeline and the graph editor (which now carry
      // this same control) learn about it. See Movie/DemoTimescale.h.
      try { GameInterfaceAPI.ConsoleCommand('mirv_filmmaker timescale ' + sp.toFixed(4)); } catch (e) {}
    }

    // ---- frame tick: input, motion, blur ----
    var alive = true;
    function tick() {
      // A newer injection owns the control now: stop, and do NOT reschedule. Without this the
      // superseded loop keeps poking deleted panels, which takes the game down natively.
      if (!alive || $.MirvSpeedGen !== myGen) return;
      if (strip.IsValid && !strip.IsValid()) { alive = false; return; }
      try {
        if (refitWidth()) { layout(curV); paintChrome(curV); }

        // -- raw cursor + demo speed from C++ (cursor in client device px) --
        var raw = row.GetAttributeString('mirvcursor', '');
        if (raw) {
          var f = raw.split(' ');
          var mx = parseInt(f[0], 10), my = parseInt(f[1], 10), lmb = (f[2] === '1');
          // ONE SPEED, THREE BARS. Field 5 is the engine's demo_timescale. This scrubber is now
          // also mounted in the camera timeline and the graph editor, and all three must show the
          // same number -- this one used to read its speed once, at build, and then own its handle
          // forever, so setting 0.25x in the camera dock left the demo bar reading 1x and the next
          // click here silently snapped the speed back. (The obvious source,
          // ctx.GetDemoControllerState(), does not exist on this context panel.)
          //
          // Idle only, and only on a real difference: reading back our own drag would fight it,
          // and an exact compare would thrash on the 4 decimals applySpeed sends.
          var esp = parseFloat(f[4]);
          if (!dragging && isFinite(esp) && esp > 0 &&
              (lastSent === null || Math.abs(esp - lastSent) > lastSent * 0.01)) {
            lastSent = esp;
            curV = sliderFor(esp);
            layout(curV);
            paintChrome(curV);
          }
          var a = strip.GetPositionWithinWindow ? strip.GetPositionWithinWindow() : null;
          // The camera timeline scales the whole control row down to fit a docked demo bar
          // (CameraTimelineJs, "THE TRANSPORT ROW HAS TO FIT THE DOCK"). GetPositionWithinWindow
          // already answers in the scaled frame; actuallayoutwidth does NOT -- it is the layout
          // size, which a transform never touches. Mixing the two would size the drag target to
          // the strip we laid out rather than the one on screen, and every drag would be off by
          // 1/k. $.MirvBarFitK is that factor, published by the panel that applies it.
          var fit = $.MirvBarFitK;
          if (!(typeof fit === 'number' && fit > 0.05 && fit <= 1)) fit = 1;
          var sw = (strip.actuallayoutwidth || 0) * fit, sh = (strip.actuallayoutheight || 0) * fit;
          if (a && sw > 0) {
            var over = (mx >= a.x && mx <= a.x + sw && my >= a.y && my <= a.y + sh);
            if (over !== hot) { hot = over; paintChrome(curV); }
            hotZone = zoneAt((mx - a.x) / sw);
            // Press must LAND on the strip to start a gesture; once started it keeps tracking even
            // if the pointer leaves, so a fast flick past the edge does not drop the gesture.
            if (lmb && !lastLmb && over) { dragging = true; pressX = mx; pressMoved = false; }
            if (!lmb) {
              // Released without travelling = a CLICK on a zone, not a scrub. Resolve it from
              // where the press LANDED (pressX), not from where the pointer ended up, so a couple
              // of px of hand-shake can never move the answer to the neighbouring zone.
              if (dragging && !pressMoved) snapToZone((pressX - a.x) / sw);
              dragging = false;
            }
            if (dragging) {
              if (!pressMoved && Math.abs(mx - pressX) > CLICK_SLOP) pressMoved = true;
              if (pressMoved) {
                var nv = clamp01((mx - a.x) / sw);
                vel = nv - curV;
                if (Math.abs(vel) < 0.0004) vel = 0;   // held still mid-drag: stop smearing
                if (vel !== 0) {
                  curV = nv;
                  setLineEase(Math.abs(vel) > 0.012);   // stop easing while the pointer is really moving
                  layout(curV);
                  paintChrome(curV);
                  applySpeed(curV, false);
                }
              }
            } else if (lineEaseFast) {
              setLineEase(false);                     // settle: restore the morph easing
              layout(curV);
            }
            // After the gesture is resolved, so an in-progress scrub reads as a scrub: the zone
            // overlay is a chooser, and it must get out of the way (and hand the readout back)
            // the moment the user is actually dragging a value.
            paintZones();
            lastLmb = lmb;
          }
        }

        // -- flow: a RESPONSE to dragging, not a permanent animation --
        // Spin up from drag velocity, then coast to a full stop. Idle means idle: a strip that
        // keeps sliding while untouched reads as broken, not as alive.
        var spacing = lerp(SP_SLOW, SP_FAST, curV);
        flow += ((dragging ? vel * 900 : 0) - flow) * 0.18;
        if (Math.abs(flow) < 0.05) flow = 0;
        if (flow !== 0) {
          scroll -= flow * 0.016;
          if (scroll <= -spacing) scroll += spacing;
          else if (scroll > 0) scroll -= spacing;
          field.style.transform = 'translateX(' + scroll.toFixed(2) + 'px)';
        }

        // -- smear: width + alpha, never a blur filter (see the note at the top of this file) --
        // A gaussian rewritten per frame on this container is what took the game down; this
        // produces the same read for free. `smear` is 0..1 and only the FIELD's opacity plus a
        // one-shot width nudge respond to it, so there is nothing per-frame to allocate.
        var target = dragging ? Math.min(1, Math.abs(vel) * 26) : 0;
        smear += (target - smear) * 0.3;
        if (smear < 0.01) smear = 0;
        vel *= 0.80;
        if (Math.abs(smear - lastSmear) > 0.04) {
          lastSmear = smear;
          field.style.opacity = (1 - 0.35 * smear).toFixed(3);
          setLineSmear(smear);
        }
      } catch (e) { alive = false; return; }
      $.Schedule(0.016, tick);
    }

    // Built at 1x and corrected on the FIRST tick from the engine's value in the cursor string.
    // (This used to try ctx.GetDemoControllerState().fTimeScale here; that method does not exist
    // on this context panel, so the try/catch swallowed it and the seed was always 1 anyway.)
    var cur = 1;
    lastSent = null;
    curV = sliderFor(cur);
    layout(curV);
    paintChrome(curV);
    tick();

    $.Msg('[speedbar] scrubber v4 (cursor-driven) injected.\n');
  } catch (err) {
    $.Msg('[speedbar] error: ' + err + '\n');
  }
})();
