#pragma once

// Panorama JS for the camera TIMELINE scrub bar. Built ONCE into the in-game
// HUD (CSGOHud) context by CameraTimelineHud.cpp.
//
// While the panel is OPEN it REPLACES the native CS2 demo bar: the native
// CSGOHudDemoController contents (slider + transport + the G-mouse / next-player /
// next-camera hotkey labels) are hidden and ours occupies that bottom space; the
// native bar is restored when we close.
//
// UI-mouse mode (state.cursor): while the camera timeline is open it
// is forced on so the panel is always clickable. In regular native-demo-bar mode it
// can be toggled from third-person/freecam by G or the injected MOUSE button.
//
//   * TIMELINE: a native-styled scrubber (HorizontalSlider) + keyframe diamonds +
//     transport + speed buttons.
//
//   C++ -> JS : attribute "state" (light, every frame), then $.CamTimeline.render().
//   JS  -> C++: buttons / sliders issue "mirv_filmmaker camtl ..." console commands.

namespace Filmmaker {

inline const char* kCameraTimelineJs = R"TLJS(
(function () {
  try {
    var existing = $('#CamTimelineRoot'); if (existing) existing.DeleteAsync(0);
    var existingRange = $('#CamTimelineRangeRoot'); if (existingRange) existingRange.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    var S = {
      accent: '#f0b323ff', cameraAccent: '#f0b323ff', renderAccent: '#aeb4bcff', freeze: '#80bdffff',
      bg: 'rgba(12,14,18,0.96)', bgSolid: 'rgba(12,14,18,1)', panelBorder: '#ffffff1f',
      track: '#ffffff1a', grid: '#ffffff0e', gridMid: '#ffffff1c',
      line: '#f0b323dd', lineSel: '#ffffffff', playhead: '#ff5a5aee',
      label: '#c8d3dfff', value: '#eef2f6ff', btnBg: '#ffffff14', btnOn: '#f0b32333',
      // Supporting text, one tier below `label`: still ~7:1 on this near-black bar, so it reads as
      // quiet rather than as disabled.
      hint: '#9aa5b3ff',
      // Interaction tiers (hover / press). See btnVisual below -- Panorama inline styles have
      // no :hover here, so these are applied from panel events.
      btnHover: '#ffffff26', btnDown: '#ffffff3d', btnOnHover: '#f0b32352',
      // Regular-timeline render-range overlay: rangeFill/rangeEdge mark the active render span.
      // It used to be #4e9fff, which is the SAME BLUE as S.freeze above (2.5 deg of hue, 0.008 of
      // lightness apart) -- so on one bar, blue meant both "this is the render span" and "timing
      // is frozen". Moved to a teal nothing else in this palette uses: 76 deg from freeze, 93 from
      // the gold of S.accent / S.line, and 32 from the graph editor's green channel swatch (the
      // graph docks beside this bar, so that one matters too).
      // rangeEdge doubles as a 10px drag label, and its own 'ee' alpha costs ~6 Lc against the
      // 96%-opaque bar, so it is solved WITH the alpha applied, not as an opaque hex.
      rangeFill: '#59d1b514', rangeEdge: '#59d1b5ee',
      // roundFill/roundEdge were meant to tint the round under the playhead in a hue distinct from
      // the render range. Nothing references them (checked) -- the round markers come from the
      // native bar. Left in place rather than deleted, but do not trust them as rendered colours.
      roundFill: '#f0b3231c', roundEdge: '#f0b323ee',
      font: 'Stratum2, "Arial Unicode MS"'
    };
    var W = 1250, W_DEFAULT = 1250, LABELW = 132;
    // THE INSPECTOR COLUMN'S WIDTH, as the operator has dragged it. Published by C++
    // (InspectorWidth.h) into every panel that needs it, so this is a READ of the same number
    // CONFIG and the CAMERA EDITOR lay their column out to -- not a third copy of it.
    //
    // It used to be a local `520` with a "keep in sync with CameraEditorJs" comment, which is a
    // promise nothing checks; this file's copy is the one that decides how wide the docked bar
    // is, so a column that moved without it left the bar running under the inspector.
    // EDITOR_INSPECTOR_W survives only as the value to use before the first state arrives.
    var EDITOR_INSPECTOR_W = 640;
    var dockInspW = EDITOR_INSPECTOR_W;
    // A width pushed straight in by whichever panel is being DRAGGED right now (see api.setInspW).
    // The stored width only moves when the drag is released -- it is persisted through the console
    // and a command per frame would stall the game -- so during the gesture this is the only
    // source that is current. 0 = nobody is dragging; follow the published state.
    var dockInspWOverride = 0;
    function editorInspectorW(screenW) {
      // Same screen cap C++ applies (0.62) and the same floor, so the bar and the column agree
      // on where the edge is even on a screen too narrow for the stored width.
      var w = dockInspW > 0 ? dockInspW : EDITOR_INSPECTOR_W;
      if (screenW > 1) w = Math.min(w, Math.max(600, screenW * 0.62));
      return Math.floor(Math.max(600, w));
    }
    var EDITOR_BOTTOM_H = 176;    // keep in sync with CameraEditorJs BOTTOM_H
    var EDITOR_BOTTOM_LIFT = 0;   // FLUSH with the screen bottom; keep in sync with CameraEditorJs
                                  // BOTTOM_LIFT and GraphEditorJs BOTTOM_LIFT (see the note there)
    var HUD_INSET_X = 14, HUD_INSET_TOP = 12; // keep scaled CS2 HUD clear of the preview frame
    var EASE = ['none','in','out','inout'];
    var EASE_LBL = ['None','Ease In','Ease Out','Ease In/Out'];


    // ---- dock motion -----------------------------------------------------
    // This overlay is a bottom dock, and swapping between Regular Timeline / Camera / Graph used
    // to be a hard cut: one root's visible went false and another's went true in the same frame,
    // which reads as the screen glitching rather than as a panel changing. It rises from below --
    // the direction it actually lives in -- on the same arrival curve the rest of the HUD uses.
    //
    // TRANSFORM ONLY, no opacity: this dock is opaque and sits over the live game, so fading it
    // would show the match straight through the panel for the length of the transition.
    //
    // Fired once per visible->hidden->visible edge; render() runs every frame, so an unguarded
    // entrance would restart forever and never settle.
    var moDockSeen = false, moDockHideFrames = 0, moDockCameFromGraph = false;
    // fromY is a SIGNED offset, because the direction has to agree with what the dock is doing:
    //   growing  (Camera -> Graph): the top edge travels UP, so the panel rises  -> fromY = +14
    //   shrinking (Graph -> Camera): the top edge travels DOWN, so it descends   -> fromY = -14
    // Always rising made the shrink read backwards -- the dock got shorter while its content
    // moved the other way, which is the "weird" part.
    function moDockIn(panel, fromY) {
      if (!panel || (panel.IsValid && !panel.IsValid())) return;
      var y = (typeof fromY === 'number') ? fromY : 14;
      try {
        panel.style.transitionProperty = 'transform';
        panel.style.transitionTimingFunction = 'cubic-bezier(0.16,1,0.3,1)';
        panel.style.transitionDuration = '0s';
        panel.style.transform = 'translateY(' + y + 'px)';
      } catch (e) { return; }
      $.Schedule(0, function () {
        try {
          if (!panel || (panel.IsValid && !panel.IsValid())) return;
          panel.style.transitionDuration = '0.2s';
          panel.style.transform = 'translateY(0px)';
        } catch (e2) {}
      });
    }
    // Swapping workspaces (Cam Editor <-> Config) drives this dock's inputs through a frame or two
    // of disagreement, so the raw "should it show" answer blinks false and back, which re-fired the
    // entrance and made rapid Cam Editor / Config clicking stutter.
    //
    // The debounce is on the ANIMATION ONLY -- never on visibility. Delaying the hide was a real
    // bug: the C++ side STOPS pumping this script's render() once the bottom mode changes, so the
    // last render to run returned "still visible", root.visible stayed true, and the camera
    // timeline was left stranded on top of the Regular Timeline with nothing ever coming back to
    // clear it. Visibility must therefore be decided from the RAW answer every frame; only the
    // decision to replay the entrance gets to wait.
    //
    // moDockOpenIsReal(raw) is called once per render. It returns true when raw has just gone
    // false->true after being genuinely hidden (not a one-frame blink).
    function moDockOpenIsReal(raw) {
      var wasHiddenLongEnough = (moDockHideFrames > 2);
      if (raw) { moDockHideFrames = 0; return wasHiddenLongEnough; }
      moDockHideFrames++;
      return false;
    }

    function disarmClear() {
      clearConfirm = false;
      if (clearBtn && clearBtn.__lbl) {
        clearBtn.__lbl.text = 'Clear';
        clearBtn.__lbl.style.color = S.value;
      }
    }
    function cmd(c) {
      // Any action disarms a pending "Clear - sure?" confirm.
      disarmClear();
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[camtl] cmd failed: ' + e + '\n'); }
    }

    // ---- small builders -------------------------------------------------
    function mk(type, parent, props) { return $.CreatePanel(type, parent, '', props || {}); }
    function lbl(parent, text, color, size) {
      var l = mk('Label', parent); l.text = text || '';
      if (color) l.style.color = color; if (size) l.style.fontSize = size + 'px';
      l.style.fontFamily = S.font; return l;
    }
    // Same interaction contract as the editor's widgets (CameraEditorWidgetsJs.h): hover and a
    // short press flash come from panel events, and the SELECTED state is owned by btnVisual so
    // the per-frame render() cannot repaint over the hover fill. There is no :hover in this
    // in-game Panorama context and no stylesheet of ours to attach classes to.
    function panelAlive(b) {
      try {
        if (!b || (b.IsValid && !b.IsValid())) return false;
        if (!b.__lbl || (b.__lbl.IsValid && !b.__lbl.IsValid())) return false;
        return true;
      } catch (e) { return false; }
    }
    function btnVisual(b) {
      if (!panelAlive(b)) return;
      var bg = b.__sel ? b.__bgSel : b.__bgBase;
      var fg = b.__sel ? b.__fgSel : b.__fgBase;
      if (b.__down) bg = S.btnDown;
      else if (b.__hot) bg = b.__sel ? S.btnOnHover : S.btnHover;
      b.style.backgroundColor = bg;
      b.__lbl.style.color = fg;
    }
    function btnSel(b, on) {
      on = !!on;
      if (!b || b.__bgBase == null || b.__sel === on) return b;
      b.__sel = on; btnVisual(b); return b;
    }
)TLJS"
#include "PanoramaUiMetricsJs.h"
#include "SpeedScrubberJs.h"
R"TLJS(
    function btn(parent, text, onClick, color) {
      var b = mk('Panel', parent); b.hittest = true;
      b.style.borderRadius = UI.btn.radius + 'px';
      b.style.paddingTop = UI.btnCompact.padV + 'px'; b.style.paddingBottom = UI.btnCompact.padV + 'px';
      b.style.paddingLeft = UI.btnCompact.padH + 'px'; b.style.paddingRight = UI.btnCompact.padH + 'px';
      b.style.marginRight = UI.btn.gap + 'px'; b.style.verticalAlign = 'center';
      var l = lbl(b, text, color || S.value, UI.btnCompact.font); l.style.fontWeight = 'bold';
      b.__lbl = l;
      b.__sel = false; b.__hot = false; b.__down = false;
      b.__bgBase = S.btnBg; b.__fgBase = color || S.value;
      b.__bgSel = S.btnOn; b.__fgSel = S.accent;
      b.SetPanelEvent('onmouseover', function () { b.__hot = true; btnVisual(b); });
      b.SetPanelEvent('onmouseout', function () { b.__hot = false; b.__down = false; btnVisual(b); });
      b.SetPanelEvent('onactivate', function () {
        b.__down = true; btnVisual(b);
        try { $.Schedule(0.11, function () { if (!panelAlive(b)) return; b.__down = false; btnVisual(b); }); }
        catch (e) { if (panelAlive(b)) { b.__down = false; btnVisual(b); } }
        if (b !== clearBtn) disarmClear();
        onClick();
      });
      btnVisual(b);
      return b;
    }
    // A keyframe diamond. GEOMETRY NOTE, because this clipped for a while: a `size` square
    // rotated 45deg spans size*sqrt(2) corner-to-corner (14px -> ~19.8px), so the CONTAINER must
    // be sized to the diagonal and cy centred in it -- sizing it to the square is what cut the
    // points off. Callers also clamp cx so the first/last keys keep their full shape instead of
    // bleeding past the strip. `sel` owns the resting colours; hover brightens without stealing
    // the selected state (same contract as btnVisual).
    //
    // THE CLICK TARGET IS NOT THE GEM. The visible square is 14px and rotated, and a click a few
    // pixels off its centre missed it entirely -- reported as "clicking the diamonds does nothing".
    // An invisible DIAM_HIT_W x DIAM_HIT_H box centred on the key takes the clicks instead; the gem
    // inside it is drawn only. The height stops short of the next lane in SCENE KEYS.
    var DIAM_HIT_W = 24, DIAM_HIT_H = 20;
    function diamond(parent, cx, cy, size, sel, onClick, onRight) {
      var hit = mk('Panel', parent);
      hit.style.position = (cx - DIAM_HIT_W / 2).toFixed(1) + 'px ' + (cy - DIAM_HIT_H / 2).toFixed(1) + 'px 0px';
      hit.style.width = DIAM_HIT_W + 'px'; hit.style.height = DIAM_HIT_H + 'px';
      hit.hittest = !!(onClick || onRight);
      var p = mk('Panel', hit);
      p.hittest = false;
      var half = size / 2;
      p.style.position = (DIAM_HIT_W / 2 - half).toFixed(1) + 'px ' + (DIAM_HIT_H / 2 - half).toFixed(1) + 'px 0px';
      p.style.width = size + 'px'; p.style.height = size + 'px';
      p.style.transformOrigin = '50% 50%';
      p.style.transform = 'rotateZ(45deg)';
      // 2px radius knocks the needle tips off the rotated square -- reads as a cut gem instead
      // of four spikes -- and the 1px edge separates it from both the dark bar and the groove:
      // idle = gold body on a dark edge, selected = white body on a gold edge ("held").
      p.style.borderRadius = '2px';
      var fill = sel ? '#ffffffff' : S.accent;
      p.style.backgroundColor = fill;
      p.style.border = '1px solid ' + (sel ? S.accent : '#00000088');
      if (hit.hittest) {
        hit.SetPanelEvent('onmouseover', function () { if (!sel) p.style.backgroundColor = '#ffd766ff'; });
        hit.SetPanelEvent('onmouseout', function () { p.style.backgroundColor = fill; });
      }
      if (onClick) hit.SetPanelEvent('onactivate', onClick);
      // Right-click = this keyframe's shake menu. `oncontextmenu` is the only right-button event
      // this in-game HUD context delivers (the Config panel's skybox tiles use the same one), and
      // it reaches us because the timeline forces UI-mouse mode -- which is what stops MovieMode
      // from swallowing the click for CS2's spectator switch (see MovieMode::OnMouseButton).
      if (onRight) hit.SetPanelEvent('oncontextmenu', onRight);
      return hit;
    }

    // Find a native HUD panel by id (walk to the topmost ancestor, then traverse) -- MEMOISED.
    //
    // The uncached version is what made this panel the most expensive thing on the main thread.
    // FindChildTraverse from the ROOT is a full recursive walk of every panel in the game's UI,
    // and render() takes that walk eight to ten times a frame: four in patchNativeBar, three more
    // per marker set in the dock/undock path, plus the range overlay and the HUD-view sweep. It
    // does that whether or not a workspace is open, so a plain demo paid for it too. Measured with
    // `mvm_debug perf on` at 1600x1200 on de_nuke, no workspace open, this pump ran ~1.0 ms per
    // frame; the JS body is nearly all of it (pinning the compiled-script cache changed nothing).
    //
    // The HUD tree is stable between rebuilds, so the answer is cacheable. IsValid() is the
    // invalidation: a panel CS2 destroys fails it and we re-resolve on the spot. A MISS is cached
    // too -- otherwise the ids that are simply absent (a demo bar that has not been created yet)
    // would each cost a full walk every frame, which is the exact case this is here to fix -- but
    // only for a short run of frames, so a panel that appears later is still picked up promptly.
    var nativeCache = {};      // id -> panel
    var nativeMissAge = {};    // id -> frames since we last looked for one that was not there
    var NATIVE_MISS_RETRY = 20;
    function findNativeUncached(id) {
      var top = ctx; while (top.GetParent && top.GetParent()) top = top.GetParent();
      var p = top.FindChildTraverse && top.FindChildTraverse(id);
      if (!p && ctx.FindChildTraverse) p = ctx.FindChildTraverse(id);
      return p;
    }
    function findNative(id) {
      var hit = nativeCache[id];
      if (hit) {
        try { if (!hit.IsValid || hit.IsValid()) return hit; } catch (e) {}
        delete nativeCache[id];
      }
      if (nativeMissAge[id] > 0) { nativeMissAge[id]--; return null; }
      var p = findNativeUncached(id);
      if (p) { nativeCache[id] = p; delete nativeMissAge[id]; }
      else nativeMissAge[id] = NATIVE_MISS_RETRY;
      return p;
    }
    // Hide/show the native demo bar (CSGOHudDemoController "Contents": SliderRow + ControlRow), and
    // -- for the editor's Regular Timeline mode -- DOCK it to fit left of the inspector. We touch
    // ONLY width + horizontalAlign (on Contents and its root); the undock path resets BOTH to their
    // defaults (100% / center), so the bar restores pixel-perfect when the editor closes. (The old
    // dock also changed height/verticalAlign/margin and never reset them -> the broken-on-exit bar.)
    var nativeContents = null, nativeRoot = null, nativeBgOn = false;   // false | 'docked' | 'undocked'
    // The undocked demo bar's backing (see nativeUndock) and our bar buttons' OPAQUE fills: the
    // old 8-20% white fills vanished over a bright scene and took the labels with them.
    var BAR_BG = 'rgba(12,14,18,0.9)';
    var BAR_BTN = { base: '#272b32ff', loopOn: '#1d4a2bff',
      editor: '#4a3a10ff', config: '#1c3552ff' };
    function ensureNative() {
      if (!nativeContents) {
        var sr = findNative('SliderRow');
        if (sr && sr.GetParent) { nativeContents = sr.GetParent(); nativeRoot = nativeContents.GetParent && nativeContents.GetParent(); }
      }
      return nativeContents;
    }
    function setNativeHidden(hide) {
      if (ensureNative()) nativeContents.visible = !hide;
    }

)TLJS"
// Split for MSVC's ~16380-byte cap on a single raw string literal (C2026), not for any
// semantic reason. Adjacent raw literals concatenate, so the seam is invisible to the script.
R"TLJS(
    // ---- THE TRANSPORT ROW HAS TO FIT THE DOCK ------------------------------------------------
    //
    // Docking narrows CS2's demo bar to sit left of the workspace inspector. The SliderRow half
    // follows correctly -- it is fractional, and rescaleMarkers() below keeps the round/kill marks
    // glued to it. The CONTROL row does not. Since the 2026-10-14 CS2 build it reads:
    //
    //   ControlRow  PlayButton | TimeLabel | TimeControls | RoundControls | MvmSpeedSpacer |
    //               SpeedControls (our dial) | ControlRowSpacer | PlayerControls |
    //               NextEventButton | CameraButton | (Highlights, Settings, EndPlayback: hidden) |
    //               MvmBarButtons
    //
    // arrangeRow() moves SpeedControls between two equal flexible spacers so the dial sits in the
    // middle of the bar (CS2 puts it right after Play). Everything else is FIXED px. Our LOOP /
    // MOUSE / CAM EDITOR / CONFIG buttons live in MvmBarButtons at the right end. A narrow dock
    // leaves the spacers at 0 and Panorama's default `overflow: squish` crushes the fixed clusters
    // rather than clipping -- the squish happens during LAYOUT, so a transform alone fixes nothing.
    //
    // Two steps, in this order:
    //
    //   1. give the ROW the width its content needs (every fixed cluster, i.e. the end of our group
    //      minus both spacers, plus FIT_TAIL of buffer). Without this the row keeps the dock width
    //      and CLIPS its own children at it;
    //   2. scale the row from its LEFT edge by k = dock width / needed width.
    //
    // CS2's own UpdateControlRowFit collapses its labels (camera mode name, tick readout, long
    // clock) one step at a time while ControlRowSpacer reads 0, which step 1 guarantees under a
    // tight dock; `need` shrinks with each collapse and the scale relaxes to match. It never
    // un-collapses while docked tight, because the spacer only regains the width the collapse freed.
    //
    // IDEMPOTENT: a transform is applied AFTER layout, so every width and offset read here is the
    // natural one, and re-measuring cannot wind the row down frame after frame (the compounding
    // trap rescaleMarkers documents below). The fixed extent does not depend on the row width we
    // write: only the spacers absorb it, and they are subtracted out.
    //
    // CLICKABLE: CS2's own itemtile.css scales a tile to 0.85 on :active, i.e. while it is being
    // held down, so Panorama hit-tests transformed panels.
    //
    // FIT_MIN is the floor. Past it the row would be too small to read, so we stop scaling and let
    // it overflow; reaching it needs an inspector past ~70% of the screen, and the column caps at 62%.
    // ---- WHAT THIS PANEL DECIDED, WHEN IT CHANGED --------------------------------------------
    //
    // A one-frame UI glitch is invisible to a screenshot burst: PrintWindow costs ~23ms a frame at
    // 1600x1200, so it samples roughly every third frame and a single bad frame usually falls
    // between two captures. This is the instrument that does not miss one. Every decision render()
    // makes about the bar is folded into one short line, and the line is only emitted when it
    // CHANGES -- so a workspace swap prints two or three lines, a steady frame prints nothing, and
    // a state that appears for exactly one frame prints twice (in and back out) instead of hiding.
    //
    // $.Msg goes to the game console, which `mvm_debug start` captures with a frame stamp. Off by
    // default; `mirv_filmmaker camtl eval $.CamTimeline.trace(1)` arms it.
    var traceOn = false, traceLast = '';
    function traceState(line) {
      if (!traceOn || line === traceLast) return;
      traceLast = line;
      $.Msg('[camtl.trace] ' + line + String.fromCharCode(10));
    }

    // Our bar buttons: compact, matching CS2's own 36px `.interactive` controls on this row.
    var BAR_BTN_W = 96, BAR_BTN_H = 36, BAR_BTN_GAP = 4;   // ONE definition, used by every button
    var BAR_ICON_W = 36;                                   // the icon-only LOOP button
    var FIT_TAIL = 44;                      // buffer between the last control and the dock edge
    var FIT_MIN = 0.62;
    // Offsets of panels the native script has parked read back as ~3.06e38. Anything past this is
    // not a position (saneFrac below guards the marker rescale against the same garbage).
    function fitSaneEdge(v) { return (typeof v === 'number' && isFinite(v) && v >= 0 && v < 100000) ? v : 0; }
    var fitRowW = -1, fitK = -1;
    // FILL. When the dock is WIDER than the row needs, the row is stretched to the dock and the
    // native spacer takes the difference, so CameraButton and our group stay hugging the right edge.
    function fitControlRow(availCss) {
      var row = findNative('ControlRow'), spacer = findNative('ControlRowSpacer');
      if (!row || !row.style || !spacer) return;
      var group = row.FindChildTraverse && row.FindChildTraverse('MvmBarButtons');
      if (!group || !group.visible || !(group.actuallayoutwidth > 0)) return;   // nothing of ours yet
      var sx = row.actualuiscale_x || ctx.actualuiscale_x || 1;

      // 1. The row, sized to every fixed cluster (the end of our group minus the two flexible
      //    spacers) plus the buffer -- or, on a dock wider than that, to the dock (FILL above).
      var mid = row.FindChildTraverse && row.FindChildTraverse('MvmSpeedSpacer');
      var flex = (spacer.actuallayoutwidth || 0) + ((mid && mid.actuallayoutwidth) || 0);
      var groupEnd = fitSaneEdge(group.actualxoffset + group.actuallayoutwidth);
      var need = (groupEnd - flex) / sx + FIT_TAIL;
      if (!(need > 120)) return;   // not laid out yet: sizing off this would collapse the row
      var rowW = (availCss > need) ? availCss : need;
      if (Math.abs(fitRowW - rowW) > 1) { fitRowW = rowW; row.style.width = Math.round(rowW) + 'px'; }

      // 2. ...and scale it into the space the dock actually has.
      if (!(availCss > 8)) return;
      var k = availCss / need;
      if (k > 1) k = 1;
      if (k < FIT_MIN) k = FIT_MIN;
      // Hysteresis: this runs every frame while docked, and rewriting a transform that has not
      // meaningfully moved costs a Panorama relayout for nothing.
      if (fitK >= 0 && Math.abs(k - fitK) < 0.004) return;
      fitK = k;
      // PUBLISH IT. The speed scrubber living in this row is cursor-driven: it compares the raw OS
      // cursor against its own GetPositionWithinWindow() and actuallayoutwidth. The first of those
      // is transform-aware and the second is not, so under a scaled row it would map a drag against
      // a strip wider than the one on screen and the handle would run away from the pointer. Both
      // scripts share one Panorama context (see the note in CameraEditorJs about $ being shared),
      // so this is the channel. 1 means "no fit applied".
      $.MirvBarFitK = k;
      if (k >= 0.999) {
        // NEVER '' -- Panorama throws on an empty style value and takes the whole render() with it.
        row.style.transform = 'none';
        return;
      }
      row.style.transformOrigin = '0% 50%';
      row.style.transform = 'scale3d(' + k.toFixed(3) + ', ' + k.toFixed(3) + ', 1)';
    }
    // Hand the row back exactly as CS2 laid it out: widths from its own stylesheet (null, not '',
    // which Panorama rejects) and no transform of ours.
    function unfitControlRow() {
      if (fitRowW < 0 && fitK < 0) return;
      fitRowW = -1; fitK = -1;
      $.MirvBarFitK = 1;
      var row = findNative('ControlRow');
      if (row && row.style) {
        row.style.transform = 'none';
        try { row.style.width = null; } catch (e) {}
      }
    }

    function nativeDock(barW) {
      if (!ensureNative()) return;
      var w = Math.max(360, Math.floor(barW || 0));
      nativeContents.style.width = w + 'px'; nativeContents.style.horizontalAlign = 'left';
      // Give the docked bar the same opaque editor background as the camera/graph panels.
      // The editor's backdrop is dropped in Regular Timeline mode (it would cover this lower-z
      // native bar), so this is the ONLY fill behind the bar -- it must be FULLY opaque (bgSolid),
      // not the 0.96 S.bg the camera/graph use (those look solid only because they're layered over
      // the opaque backdrop; a single 0.96 layer here would let the game blur show through).
      nativeContents.style.backgroundColor = S.bgSolid;
      // NOT on #Root: Root is fit-children and grows upward to hold CS2's player list / round menu
      // when one opens, so an opaque Root fill blanked the whole preview behind the popup.
      if (nativeRoot && nativeRoot.style) { nativeRoot.style.width = w + 'px'; nativeRoot.style.horizontalAlign = 'left'; }
      nativeBgOn = 'docked';
      rescaleMarkers('RoundMarkers', true, true);
      rescaleMarkers('HighlightMarkers', true, false);
      rescaleMarkers('HighlightIcons', false, false);
      markersDocked = true; // docked geometry is now written on the native marker children
      // The control row's children are fixed-px and do not follow the container -- fit them.
      // `w` (the dock width we just wrote onto Contents) is the budget, NOT ControlRow's own
      // width: step 2 of the fit rewrites that, so reading it back here would chase our own output.
      fitControlRow(w);
      dockPopups(true);
    }
    // CS2's player list and round menu (#PlayerList / #RoundMenu, opened from the player name and
    // the round button) are children of the bar's #Root and open ABOVE #Contents. Docked, the
    // controller sits below our workspace roots (z53+), so they opened behind the editor chrome,
    // and at full size beside a row scaled by fitK. While docked: lift the controller's HUD-level
    // panel above the editor/timeline roots (z-layer map, PanoramaBridge.h: 58 -- below the marker
    // menu and graph overlay, which hide this bar anyway), scale the popups by the row's factor from
    // their bottom-left corner (their native marginLeft already lands on the scaled buttons), and cap
    // their height so a ten-player list stays inside the preview area.
    var popupsDocked = false, popupHost = null, popupHostZ = null;
    var POPUP_IDS = ['PlayerList', 'RoundMenu'];
    function nativeHudChild() {
      var p = nativeRoot;
      while (p && p.GetParent && p.GetParent() && p.GetParent() !== ctx) p = p.GetParent();
      return (p && p.GetParent && p.GetParent() === ctx) ? p : null;
    }
    function dockPopups(on) {
      if (!on && !popupsDocked) return;
      if (on && !popupsDocked) {
        popupHost = nativeHudChild();
        if (popupHost) { popupHostZ = popupHost.style.zIndex || null; popupHost.style.zIndex = '58'; }
        popupsDocked = true;
      }
      var k = (on && fitK > 0) ? fitK : 1;
      for (var i = 0; i < POPUP_IDS.length; i++) {
        var p = findNative(POPUP_IDS[i]);
        if (!p || !p.style) continue;
        if (!on) {
          p.style.transform = 'none';
          try { p.style.maxHeight = null; } catch (e) {}
          p.__mvmK = undefined;
          continue;
        }
        if (p.__mvmK === k) continue;   // nothing moved: skip the relayout
        p.__mvmK = k;
        p.style.transformOrigin = '0% 100%';
        p.style.transform = k < 0.999 ? ('scale3d(' + k.toFixed(3) + ', ' + k.toFixed(3) + ', 1)') : 'none';
        p.style.maxHeight = '440px';
      }
      if (!on) {
        if (popupHost && popupHost.IsValid && popupHost.IsValid()) {
          try { popupHost.style.zIndex = popupHostZ; } catch (e2) { popupHost.style.zIndex = '0'; }
        }
        popupHost = null; popupHostZ = null;
        popupsDocked = false;
      }
    }
    function nativeUndock() {
      if (!ensureNative()) return;
      // Restore width/align FIRST so the bar always un-docks, even if a later assignment fails.
      nativeContents.style.width = '100%'; nativeContents.style.horizontalAlign = 'center';
      if (nativeRoot && nativeRoot.style) { nativeRoot.style.width = '100%'; nativeRoot.style.horizontalAlign = 'center'; }
      unfitControlRow(); // hand the transport row back at 1:1 with the full-width bar
      dockPopups(false); // popups back to CS2's size and the controller back to its own z
      // Undocked fill: a dark translucent backing over CS2's world blur. CS2's own
      // hud-blur-bg-color is too light to read the bar's text over a bright sky, and the old
      // "restore" wrote rgba(0,0,0,0) here, which ERASED that backdrop entirely after the first
      // dock (Panorama rejects '' and an inline value cannot be un-set back to the stylesheet's).
      // Written once per state change, not per frame.
      if (nativeBgOn !== 'undocked') {
        nativeContents.style.backgroundColor = BAR_BG;
        if (nativeRoot && nativeRoot.style) nativeRoot.style.backgroundColor = 'rgba(0,0,0,0)';
        nativeBgOn = 'undocked';
      }
      // Marker restore/capture state machine (see rescaleMarkers below). The docked geometry we
      // wrote onto the marker children does NOT go away when the container returns to 100% --
      // the native script never re-lays them out -- so we must actively write the basis back.
      // While our docked styles are still applied, keep REAPPLYING the basis scaled to the live
      // container width (which is animating back to the full native width); once the container
      // reads its basis width again the children are pixel-identical to their native geometry.
      // Only AFTER that (plus a couple frames of style->actual-layout lag) resume capturing the
      // basis from live reads. Capturing while our docked styles were still on the children was
      // the compounding-shrink bug: every open/close cycle baked the shrunken left/width in as
      // the new "basis", so the kill/round icons kept compressing and never lined up again.
      if (markersDocked) {
        rescaleMarkers('RoundMarkers', true, true);
        rescaleMarkers('HighlightMarkers', true, false);
        rescaleMarkers('HighlightIcons', false, false);
        if (markersAtBasisWidth()) { markersDocked = false; markerCaptureHold = 2; }
      } else if (markerCaptureHold > 0) {
        markerCaptureHold--;
      } else {
        // Fully restored: refresh the basis every frame while undocked -- cheap, and self-heals
        // if the native script recreates the markers (round data changed, spectated player
        // switched) while undocked.
        captureMarkerBasis('RoundMarkers');
        captureMarkerBasis('HighlightMarkers');
        captureMarkerBasis('HighlightIcons');
      }
    }
)TLJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent
// literals are concatenated by the compiler, so this stays one JS blob.
R"TLJS(
    // ---- Round/kill marker rescale -----------------------------------------------------------
    // CS2's own huddemocontroller.js positions the round-shade bars (#RoundMarkers), the
    // highlight-reel intervals (#HighlightMarkers) and the kill/death icons (#HighlightIcons) as
    // ABSOLUTE PIXEL children, computed ONCE against the bar's width at that moment: RoundMarkers
    // is guarded by an internal one-shot flag that never fires again, and the other two are only
    // recomputed when the spectated player changes. Docking the bar narrower for the editor's
    // Regular Timeline mode resizes Contents/Root (and RoundMarkers/HighlightMarkers/HighlightIcons
    // correctly cascade to 100% of that via native CSS -- verified live: container 1578px->1099px),
    // but the existing marker children keep their STALE full-width geometry (verified live: a
    // marker's actuallayoutwidth read 71px in BOTH states) -- they don't shrink with their parent,
    // so the round-shade boundaries / kill icons no longer line up with the (correctly rescaled)
    // slider thumb: the playhead ends up visually inside the wrong round's shading.
    // Fix: capture each marker's left offset + width while UNDOCKED (the bar's true native width
    // == the basis the native script computed against), then reapply those figures scaled to the
    // CURRENT container width every time we dock. Must read the BASIS ONCE and always scale FROM
    // it (not from the live value) -- rescaling from an already-rescaled live read would compound
    // the scale factor every frame.
    //
    // EVERYTHING IN THE BASIS IS A FRACTION, never a device pixel. That is the fix for "change
    // the resolution while the editor / Config / Render workspace is open and the round shading
    // and the kill icons never line up again": device px captured at 1440p are meaningless at
    // 1080p, so on a resolution change the stored basis described a bar that no longer existed --
    // and because a basis is only ever captured while UNDOCKED, a workspace being open meant it
    // could never be re-captured either, so the mismatch was permanent until the demo restarted.
    // Fractions are resolution-independent, and re-deriving from them is idempotent (which the
    // px form was NOT -- see the compounding-shrink note below).
    var markerBasis = {}; // id -> { wFrac: container/ctx width at capture, items: [{l,w} fractions] }
    var markersDocked = false;  // docked geometry is currently written on the native marker children
    var markerCaptureHold = 0;  // undocked frames to wait after a restore before trusting live reads
    // Container width as a fraction of the whole HUD, so the settle test below survives a
    // resolution change too.
    function markerWidthFrac(p) {
      var cw = ctx.actuallayoutwidth || 0;
      if (!(cw > 1)) return 0;
      return (p.actuallayoutwidth || 0) / cw;
    }
    // True once every marker container with a captured basis is laid out at its basis width
    // again (i.e. the undock has visually settled and a scale-1 rescale restored the children).
    function markersAtBasisWidth() {
      for (var id in markerBasis) {
        var b = markerBasis[id]; if (!b || !b.wFrac) continue;
        var p = findNative(id); if (!p) continue;
        // Child count changed = the native script rebuilt this set with fresh geometry while we
        // were docked; none of our styles are on the new children, so it can't block settling.
        if (p.GetChildCount && p.GetChildCount() !== b.items.length) continue;
        if (Math.abs(markerWidthFrac(p) - b.wFrac) > 0.005) return false;
      }
      return true;
    }
    // A basis entry is a FRACTION of the container width, so anything outside a small sane band is
    // not a marker position -- it is a child the engine has not laid out yet reporting a garbage
    // offset. Recording one poisons the basis permanently (it is only ever captured while
    // undocked), and the next rescale then writes e.g. `margin-left: 3.06e+38px`, which Panorama
    // rejects by THROWING -- out of rescaleMarkers, out of nativeUndock, out of api.render, and
    // "Skipping rest of script". Everything below that call in render() then stops running for the
    // rest of the session: the scrub readout, the keyframe diamonds, the speed scrubber's cursor
    // pump. One bad float silently disabled half the bar, which is why this is guarded in BOTH
    // directions rather than only where it is written.
    function saneFrac(v) { return typeof v === 'number' && isFinite(v) && v > -4 && v < 4; }
    function captureMarkerBasis(id) {
      var p = findNative(id);
      if (!p || !p.GetChildCount) return;
      var n = p.GetChildCount();
      if (n === 0) { delete markerBasis[id]; return; }
      var w = p.actuallayoutwidth || 0;
      if (!(w > 1)) return;               // not laid out yet: a divide here would poison the basis
      var items = [];
      for (var i = 0; i < n; i++) {
        var c = p.GetChild(i);
        var l = (c.actualxoffset || 0) / w, cw = (c.actuallayoutwidth || 0) / w;
        if (!saneFrac(l) || !saneFrac(cw)) return;   // capture ALL of them or none: a partial
        items.push({ l: l, w: cw });                 // basis would misplace the rest of the set
      }
      markerBasis[id] = { wFrac: markerWidthFrac(p), items: items };
    }
    // scaleWidth=false for HighlightIcons: those are fixed-size (22px) CSS icons -- rescale their
    // left offset only, don't stretch/squish the icon itself. usePosition matches whichever
    // property the NATIVE script used to place that element, so we don't change its layout
    // behavior: RoundMarkers children use style.position (no flow-children on the parent, so the
    // CSS margin-top:30px combines with our y=0); HighlightMarkers/HighlightIcons children use
    // style.marginLeft (switching those to style.position would drop HighlightIcons' CSS
    // vertical-align:center, since position sets an explicit y and takes the child out of flow).
    function rescaleMarkers(id, scaleWidth, usePosition) {
      // Belt and braces around the value guards below: this runs from nativeDock/nativeUndock,
      // which run from api.render, which has no outer guard -- so ANY throw in here stops the rest
      // of render() for good. Same reasoning as applyLayout's try/catch further down.
      try { rescaleMarkersUnsafe(id, scaleWidth, usePosition); }
      catch (e) { delete markerBasis[id]; $.Msg('[camtl] rescaleMarkers(' + id + '): ' + e + '\n'); }
    }
    function rescaleMarkersUnsafe(id, scaleWidth, usePosition) {
      var p = findNative(id);
      if (!p || !p.GetChildCount) return;
      var basis = markerBasis[id];
      var n = p.GetChildCount();
      if (!basis || n !== basis.items.length) {
        // No basis, or the native script just rebuilt this set (round data changed, spectated
        // player switched, the HUD was recreated by a resolution change). Whatever it wrote was
        // computed against the container as it is RIGHT NOW, so it is correct at this instant --
        // adopt it. Previously this returned and waited for an undocked capture that never comes
        // while a workspace is open, which is what left the icons stranded at the old geometry.
        // Adopting is safe to repeat because the basis is fractional: re-deriving at the same
        // width reproduces the identical numbers instead of compounding the scale factor (the
        // bug the old device-px basis had, where every open/close cycle shrank the markers).
        captureMarkerBasis(id);
        return;
      }
      var curW = p.actuallayoutwidth || 0;
      if (!(curW > 1)) return;
      var uiscale = p.actualuiscale_x || ctx.actualuiscale_x || 1;
      // NOTHING TO DO UNLESS THE CONTAINER MOVED. The values below are a pure function of the
      // basis (constant), the container width and the ui scale, so once they are written they stay
      // correct until one of those changes -- but this runs from nativeDock/nativeUndock, i.e.
      // EVERY FRAME a workspace is open. On a full match that was ~140 style writes per frame
      // across the round shades and the kill icons, each one invalidating Panorama layout, to
      // re-assert numbers that were already there. Re-derive only on a real change; `n` is in the
      // key so a native rebuild (round data, spectator switch) still forces a pass.
      var applyKey = n + ':' + Math.round(curW) + ':' + uiscale.toFixed(4);
      if (basis.appliedKey === applyKey) return;
      basis.appliedKey = applyKey;
      for (var i = 0; i < n; i++) {
        var c = p.GetChild(i), it = basis.items[i];
        var left = (it.l * curW) / uiscale, wide = (it.w * curW) / uiscale;
        // Second gate, in case a basis captured by an older build of this script survived into a
        // live session: a value Panorama rejects throws, and the throw takes the whole render with
        // it (see saneFrac). Drop the poisoned basis and re-adopt from the live geometry instead.
        if (!isFinite(left) || !isFinite(wide) || Math.abs(left) > 1e5 || Math.abs(wide) > 1e5) {
          delete markerBasis[id];
          return;
        }
        // (basis.appliedKey is set above, so a run that bails here drops the basis with it.)
        var leftPx = left.toFixed(1);
        if (usePosition) c.style.position = leftPx + 'px 0px 0px';
        else c.style.marginLeft = leftPx + 'px';
        if (scaleWidth) c.style.width = wide.toFixed(1) + 'px';
      }
    }
)TLJS"
R"TLJS(
    // The 5 s skip. CS2's -15s / +15s buttons step by a constant in its own script, so their click
    // is retargeted to `mirv_filmmaker skip` (the same path, step and camera-timeline clamp as the
    // arrow keys) and the `timestep_value` dialog variable their labels print is set to match.
    // Latched per panel: a rebuilt HUD gets fresh panels without the latch and is patched again.
    var SKIP_SECONDS = 5;   // keep equal to kSkipSeconds in Movie/MovieMode.cpp
    function patchTimeStep() {
      var back = findNative('TimeStepBackward'), fwd = findNative('TimeStepForward');
      if (back && !back.__mvmSkip) {
        back.__mvmSkip = 1;
        back.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker skip back'); });
      }
      if (fwd && !fwd.__mvmSkip) {
        fwd.__mvmSkip = 1;
        fwd.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker skip forward'); });
      }
      // The variable lives on the CSGOHudDemoController panel: Contents -> Root -> controller.
      var ctl = ensureNative() && nativeRoot && nativeRoot.GetParent && nativeRoot.GetParent();
      if (ctl && ctl.__mvmStep !== SKIP_SECONDS && ctl.SetDialogVariableInt) {
        ctl.__mvmStep = SKIP_SECONDS;
        ctl.SetDialogVariableInt('timestep_value', SKIP_SECONDS);
      }
    }
)TLJS"
// Split for MSVC's ~16380-byte cap on a single raw string literal (C2026).
R"TLJS(
    // Our buttons' home on the native row: one group appended to ControlRow, so it lands right of
    // CS2's ControlRowSpacer and CameraButton (the hidden Highlights/Settings/EndPlayback collapse).
    function barGroup() {
      var row = findNative('ControlRow');
      if (!row) return null;
      var g = row.FindChildTraverse && row.FindChildTraverse('MvmBarButtons');
      if (!g) {
        g = $.CreatePanel('Panel', row, 'MvmBarButtons', {});
        g.style.flowChildren = 'right';
        g.style.verticalAlign = 'center';
        g.style.height = BAR_BTN_H + 'px';
        g.style.marginLeft = '6px';
        g.style.marginRight = '16px';
      }
      arrangeRow(row);
      return g;
    }
    // Centre the speed dial: [transport left] MvmSpeedSpacer | SpeedControls | ControlRowSpacer
    // [player / next event / camera / our buttons right]. Two equal fill-parent-flow spacers put
    // the dial in the middle of the free space. Moves only what is out of place, so a settled row
    // costs no relayout; a rebuilt HUD (fresh panels) is simply re-arranged.
    var ROW_MIDDLE = ['MvmSpeedSpacer', 'SpeedControls', 'ControlRowSpacer', 'PlayerControls', 'NextEventButton'];
    function arrangeRow(row) {
      var mid = row.FindChildTraverse('MvmSpeedSpacer');
      if (!mid) {
        mid = $.CreatePanel('Panel', row, 'MvmSpeedSpacer', {});
        mid.hittest = false;
        mid.style.width = 'fill-parent-flow(1.0)';
      }
      var speed = findNative('SpeedControls');
      if (speed && !speed.__mvmMid) {
        speed.__mvmMid = 1;   // symmetric margins: CS2 gives it margin-left 15px for its old slot
        speed.style.marginLeft = '20px'; speed.style.marginRight = '20px';
      }
      compactReadouts();
      var prev = findNative('RoundControls');
      if (!prev || prev.GetParent() !== row) return;
      for (var i = 0; i < ROW_MIDDLE.length; i++) {
        var p = (i === 0) ? mid : findNative(ROW_MIDDLE[i]);
        if (!p || p.GetParent() !== row) return;
        if (rowIndex(row, p) !== rowIndex(row, prev) + 1) row.MoveChildAfter(p, prev);
        prev = p;
      }
    }
    // Give the speed dial the room: CS2's clock/tick readout and spectated-player name are sized
    // for a bar without it. Smaller text on the Time/Tick labels and a narrower player name
    // (CS2: 190px, 18px text). Latched per panel, so a rebuilt HUD is restyled once.
    var READOUT_IDS = ['TimeTitle', 'TimeValue', 'TimeValueShort', 'TickLabel', 'TickValue'];
    function compactReadouts() {
      for (var i = 0; i < READOUT_IDS.length; i++) {
        var l = findNative(READOUT_IDS[i]);
        if (l && !l.__mvmSmall) { l.__mvmSmall = 1; l.style.fontSize = '15px'; }
      }
      var name = findNative('PlayerName');
      if (name && !name.__mvmSmall) {
        name.__mvmSmall = 1;
        name.style.width = '140px'; name.style.fontSize = '15px';
      }
    }
    function rowIndex(row, p) {
      var n = row.GetChildCount();
      for (var i = 0; i < n; i++) if (row.GetChild(i) === p) return i;
      return -1;
    }
)TLJS"
// Split for MSVC's ~16380-byte cap on a single raw string literal (C2026).
R"TLJS(
    // Hover hint for our bar buttons. UiToolkitAPI's text tooltip lives in a layer the Camera
    // Editor's inspector (z53+) paints over, so the LOOP hint vanished under it while docked. This
    // one is a top-level root above every HUD layer (z-layer map: PanoramaBridge.h, 270).
    function barTip(text, anchor) {
      var r = ctx.FindChildTraverse('MvmBarTipRoot');
      if (!r) {
        r = $.CreatePanel('Panel', ctx, 'MvmBarTipRoot', {});
        r.hittest = false;
        r.style.width = '100%'; r.style.height = '100%'; r.style.zIndex = '270';
        var l = $.CreatePanel('Label', r, 'MvmBarTipLabel', {});
        l.hittest = false;
        l.style.fontSize = '14px'; l.style.color = '#e3e8eeff';
        l.style.backgroundColor = '#0b0f14f2'; l.style.border = '1px solid #ffffff22';
        l.style.borderRadius = '2px'; l.style.padding = '6px 10px';
      }
      var lbl = r.FindChildTraverse('MvmBarTipLabel');
      if (!text || !anchor || !anchor.GetPositionWithinWindow) { r.visible = false; return; }
      lbl.text = text;
      var a = anchor.GetPositionWithinWindow();
      var sx = ctx.actualuiscale_x || 1, sy = ctx.actualuiscale_y || sx;
      var cw = (ctx.actuallayoutwidth || 0) / sx;
      var x = a.x / sx, y = a.y / sy - 40;
      if (cw > 0 && x > cw - 170) x = cw - 170;   // keep the hint on screen at the right edge
      lbl.style.position = Math.round(x) + 'px ' + Math.round(y) + 'px 0px';
      r.visible = true;
    }
    // Patch the LIVE native demo bar (CSGOHudDemoController): hide the native controls a movie
    // timeline has no use for, retarget the time step, and inject our LOOP / MOUSE / CAM EDITOR /
    // CONFIG buttons into it (idempotent; re-adds if the bar is recreated). Runs every frame while
    // in a demo so the clutter never reappears. CS2's player stepper, next-kill and camera-cycle
    // buttons stay: they are useful while filming.
    //
    // The injected buttons are HIDDEN only when the editor replaces the native bar with the
    // camera-timeline or graph overlay (hosted + open / graphExp). In the editor's default NATIVE
    // bottom mode the native bar IS the bottom panel, so we keep the buttons present on it.
    // The camera CS2's camera button says it is showing: 0 first person, 1 chase, 2 free cam, -1
    // unknown. GOTV demos report no observer mode, so MovieMode reads this back ('cammode').
    var lastCamMode = null;
    function publishCameraMode() {
      var m = -1;
      try {
        var lbl = findNative('CameraModeLabel');
        var t = lbl ? String(lbl.text || '').toLowerCase() : '';
        if (t.indexOf('first') >= 0) m = 0;
        else if (t.indexOf('chase') >= 0 || t.indexOf('third') >= 0) m = 1;
        else if (t.indexOf('free') >= 0 || t.indexOf('roam') >= 0) m = 2;
      } catch (e) { m = -1; }
      if (m !== lastCamMode) { lastCamMode = m; try { root.SetAttributeString('cammode', String(m)); } catch (e2) {} }
    }
    function patchNativeBar() {
      patchTimeStep();
      // "End Playback" and the native gear (opens CS2's own X-Ray/True View panel, which our Camera
      // Editor's own ⚙ menu replaces) are suppressed PERMANENTLY from the moment a demo starts --
      // not just while the Camera Editor is open. This runs every frame regardless of editor state
      // (patchNativeBar is called unconditionally from CameraTimelineHud, which is always active
      // during demo playback), so unlike the editor-only CameraEditorJs.h version this doesn't
      // reappear when the editor is closed or before it's ever opened.
      var endBtn = findNative('EndPlayback');
      if (endBtn) endBtn.visible = false;
      var gearBtn = findNative('SettingsButton');
      if (gearBtn) gearBtn.visible = false;
      // The film-reel icon (HighlightsButton, film.vsvg) toggles CS2's highlight-reel filter on
      // the bar. It has no place in a movie-making timeline -- in any of the three bottom modes,
      // in Config, or during plain playback -- so it goes with EndPlayback and the native gear.
      var reelBtn = findNative('HighlightsButton');
      if (reelBtn) reelBtn.visible = false;
      // CS2's spectator overview map ([CTRL] Close / [Z] / [X] / [SPACE] Clear Drawing) pops up
      // over the shot. It is no use while filming, so it never shows during a demo. The panel
      // toggles itself, so this is re-asserted every frame like the buttons above.
      var overview = findNative('MapOverview');
      if (overview && overview.visible) overview.visible = false;
      publishCameraMode();
      var host = barGroup();
      if (!host) return;
      function childIndex(p) {
        if (!p || !host.GetChildCount) return -1;
        var n = host.GetChildCount();
        for (var ci = 0; ci < n; ci++) if (host.GetChild(ci) === p) return ci;
        return -1;
      }
      var hosted = !!(st && st.hosted);
      var cfgOpenBar = !!(st && st.configOpen);
      var oldEditor = host.FindChildTraverse && host.FindChildTraverse('CamEditorBtn');
      var oldConfig = host.FindChildTraverse && host.FindChildTraverse('ConfigBtn');
      var oldSwap = host.FindChildTraverse && host.FindChildTraverse('ModeSwapBtn');
      // Ordering guard (CAM EDITOR -> CONFIG): if the first is missing while the second exists,
      // or the pair is out of order, delete both and rebuild next frame.
      var badOrder = (!oldEditor && oldConfig)
        || (oldEditor && oldConfig && childIndex(oldEditor) > childIndex(oldConfig));
      if (badOrder) {
        try { if (oldEditor) oldEditor.DeleteAsync(0); } catch (orderErr2) {}
        try { if (oldConfig) oldConfig.DeleteAsync(0); } catch (orderErr3) {}
        return;
      }
      function nativeBtn(id, text, onactivate) {
        var b = host.FindChildTraverse && host.FindChildTraverse(id);
        if (!b) {
          b = $.CreatePanel('Panel', host, id, {});
          b.hittest = true;
          b.style.verticalAlign = 'center'; b.style.marginLeft = BAR_BTN_GAP + 'px';
          b.style.borderRadius = '2px';
          b.style.border = '1px solid #ffffff12';
          var l = $.CreatePanel('Label', b, '', {});
          b.__lbl = l;
          b.SetPanelEvent('onactivate', onactivate);
        } else if (!b.__lbl) {
          b.__lbl = (b.GetChildCount && b.GetChildCount() > 0) ? b.GetChild(0) : $.CreatePanel('Label', b, '', {});
        }
        // Fixed geometry, written ONCE: the bar buttons must never resize when a label changes
        // (ModeSwapBtn flips CAM EDITOR <-> CONFIG) and the text must stay centered -- fit-to-text
        // widths made the row jump around. patchNativeBar runs every frame, and re-asserting ten
        // style properties on four buttons per frame is forty layout invalidations for a geometry
        // that never moves; __geom latches it. The button is ours and nothing else writes it, so
        // a panel that still exists still has it.
        if (!b.__geom) {
          b.__geom = 1;
          b.style.height = BAR_BTN_H + 'px'; b.style.width = BAR_BTN_W + 'px';
          b.style.paddingLeft = '0px'; b.style.paddingRight = '0px';
          b.__lbl.style.fontWeight = 'bold'; b.__lbl.style.fontSize = '13px';
          b.__lbl.style.verticalAlign = 'center'; b.__lbl.style.horizontalAlign = 'center';
          b.__lbl.style.width = '100%'; b.__lbl.style.textAlign = 'center';
          // CS2's `#ControlRow Label { padding: 5px }` reaches into our buttons and pushes the text
          // off centre; the button box does the spacing here.
          b.__lbl.style.padding = '0px';
        }
        if (b.__lbl.text !== text) b.__lbl.text = text;
        return b;
      }
      // Loop controls belong only to the regular/native timeline, including its docked
      // editor and Config forms. Camera and graph timelines do not duplicate them.
      // Keep the shared loop state intact when switching between those views.
      var lpOn = !!(st && st.loopOn);
      var lpReady = !!(st && st.loopReady);
      // An icon-only square (CS2's own refresh_skinny glyph: two arrows chasing round a circle),
      // created FIRST so it sits leftmost in the group. Tinted green while a loop is active.
      var lpBtn = host.FindChildTraverse && host.FindChildTraverse('LoopBtn');
      if (!lpBtn) {
        lpBtn = $.CreatePanel('Panel', host, 'LoopBtn', {});
        lpBtn.hittest = true;
        lpBtn.style.width = BAR_ICON_W + 'px'; lpBtn.style.height = BAR_BTN_H + 'px';
        lpBtn.style.verticalAlign = 'center'; lpBtn.style.marginLeft = BAR_BTN_GAP + 'px';
        lpBtn.style.borderRadius = '2px';
        lpBtn.style.border = '1px solid #ffffff12';
        lpBtn.__icon = $.CreatePanel('Image', lpBtn, '', { src: 'file://{images}/icons/ui/refresh_skinny.svg' });
        lpBtn.__icon.hittest = false;
        // padding 0: CS2's `#ControlRow Image { padding: 5px }` would shrink the glyph to 10px.
        lpBtn.__icon.style.width = '22px'; lpBtn.__icon.style.height = '22px';
        lpBtn.__icon.style.padding = '0px';
        lpBtn.__icon.style.horizontalAlign = 'center'; lpBtn.__icon.style.verticalAlign = 'center';
        lpBtn.SetPanelEvent('onactivate', function () { cmd('mirv_filmmaker loop toggle quiet'); });
        var lpRef = lpBtn;
        lpBtn.SetPanelEvent('onmouseover', function () { barTip('Loop playback', lpRef); });
        lpBtn.SetPanelEvent('onmouseout', function () { barTip(''); });
      }
      var LOOP_ON_FG = '#7be08eff';
      var lpLit = lpOn && lpReady;
      if (lpBtn.__lit !== lpLit) {
        lpBtn.__lit = lpLit;
        lpBtn.style.backgroundColor = lpLit ? BAR_BTN.loopOn : BAR_BTN.base;
        if (lpBtn.__icon) lpBtn.__icon.style.washColor = lpLit ? LOOP_ON_FG : S.label;
      }
      var lpVis = !(st && (st.open || st.graphExp));
      if (lpBtn.visible && !lpVis) barTip('');   // no onmouseout fires for a panel that vanishes
      lpBtn.visible = lpVis;

      // Workspace open (Camera Editor hosted OR Config): the standalone CAM EDITOR / CONFIG
      // buttons collapse into ONE swap button that flips to the OTHER workspace --
      // Config shows "CAM EDITOR" (gold), the editor's Regular Timeline shows "CONFIG" (blue).
      // There is no MOUSE button: CS2's own cameras hand the cursor back after a look-around drag.
      if (hosted || cfgOpenBar) {
        try { if (oldEditor) oldEditor.visible = false; } catch (he) {}
        try { if (oldConfig) oldConfig.visible = false; } catch (hc) {}
        var sb = nativeBtn('ModeSwapBtn', cfgOpenBar ? 'CAM EDITOR' : 'CONFIG', function () {
          // Read the CURRENT state at click time (one shared handler for both directions).
          if (st && st.configOpen) cmd('mirv_filmmaker editor on');
          else cmd('mirv_filmmaker config on');
        });
        sb.style.backgroundColor = cfgOpenBar ? BAR_BTN.editor : BAR_BTN.config;
        sb.__lbl.style.color = cfgOpenBar ? S.accent : S.freeze;
        sb.visible = true;
        return;
      }
      try { if (oldSwap) oldSwap.visible = false; } catch (hsw) {}
      var cb = nativeBtn('CamEditorBtn', 'CAM EDITOR', function () { cmd('mirv_filmmaker editor toggle'); });
      cb.style.backgroundColor = BAR_BTN.editor;
      cb.__lbl.style.color = S.accent;
      cb.visible = true;
      // CONFIG: the lightweight UI/display settings panel (ConfigHud), BLUE so the two
      // workspaces read as distinct siblings.
      var gb = nativeBtn('ConfigBtn', 'CONFIG', function () { cmd('mirv_filmmaker config toggle'); });
      gb.style.backgroundColor = BAR_BTN.config;
      gb.__lbl.style.color = S.freeze;
      gb.visible = true;
    }

)TLJS"
// Split for MSVC's ~16380-byte cap on a single raw string literal (C2026).
R"TLJS(
    // Legacy cleanup hook: if an older build hid native HUD siblings, restore them. The
    // camera timeline HUD toggle is disabled; this must not hide anything anymore.
    // CamEditorConfirmRoot/CamEditorSettingsRoot: promoted to TOP-LEVEL siblings of CamEditorRoot
    // (see CameraEditorJs.h z-layer map) so their z-index beats the Graph/Timeline roots -- which
    // also makes them direct ctx children like CamEditorRoot, so they must be whitelisted here too
    // or this "Hide All" sweep force-hides them the instant they open (no visible symptom besides
    // the popup silently vanishing -- it never goes through closeSettings(), so nothing logs it).
    var OURS = { 'CamTimelineRoot': 1, 'CamTimelineRangeRoot': 1, 'MarkerHudRoot': 1, 'MovieHudRoot': 1, 'CamEditorRoot': 1, 'GraphExpRoot': 1, 'CamEditorConfirmRoot': 1, 'CamEditorSettingsRoot': 1, 'CamEditorRenderRoot': 1, 'ConfigHudRoot': 1, 'CamEditorCustomizeRoot': 1, 'CamEditorCustomizeConfirmRoot': 1, 'MvmBarTipRoot': 1,
      // The easter-egg overlay: never part of the gameplay HUD the view modes hide.
      'MvmEggRoot': 1 };
    var VIEWPORT_HUD_ROOTS = {
      'HudBottomGradient': 1,
      'HudInWorld': 1,
      'HudTopCenter': 1,
      'HudTopLeft': 1,
      'HudLowerLeft': 1,
      'HudBottomCenter': 1,
      'HudBottomRight': 1,
      'HudTopRight': 1,
      'HudCompass': 1,
      'HudChat': 1,
      'HudRadar': 1,
      'HudMoney': 1,
      'HudHealthArmor': 1,
      'jsHudHealthArmorAmmoMore': 1,
      'HudWeaponSelection': 1,
      'HudDeathNotice': 1,
      'ScoreAndTimeAndBomb': 1,
      'HudTeamCounter': 1,
      'HudWinPanel': 1,
      'HudSpectatorVignetting': 1,
      'HudRadio': 1,
      'HudInstructor': 1,
      'HudVote': 1,
      'HudRosettaSelector': 1,
      'MapOverview': 1,
      'HudPerfStatsBasics': 1,
      'Scoreboard': 1,
      'HudDeathPanel': 1
    };
    // Spectator observer chrome stripped for the "in-game" look: the player name/clan/weapon
    // side bar, the centred player avatar, and the spectator vignette. Radar + HP/ammo stay.
    var SPEC_PANELS = ['jsHudSpecplayer__Bg', 'HudSpecplayer__Avatar', 'HudSpectatorVignetting'];
    var hiddenNatives = null; // [{p, v, id}] captured while ANY panel is hidden; null while fully shown
    var hiddenNativeIds = {};
    var curHudView = 'full';
    function restoreGameHud() {
      if (hiddenNatives) {
        for (var j = 0; j < hiddenNatives.length; j++) {
          var h = hiddenNatives[j];
          try { if (h.p && !(h.p.IsValid && !h.p.IsValid())) h.p.visible = h.v; } catch (e2) {}
          // The native HUD can rebuild while the editor is open. Restore by id too so panels hidden
          // by an older JS closure do not stay hidden after leaving the camera editor.
          try {
            if (h.id) {
              var cur = ctx.FindChildTraverse && ctx.FindChildTraverse(h.id);
              if (cur) cur.visible = h.v;
            }
          } catch (e3) {}
        }
      }
      hiddenNatives = null;
      hiddenNativeIds = {};
    }
    function forceFullHudPanels() {
      // Defensive restore for the spectator HUD pieces that "In-Game" mode hides. If the script was
      // rebuilt while those panels were already hidden, hiddenNatives may not contain them.
      for (var i = 0; i < SPEC_PANELS.length; i++) {
        try {
          var p = ctx.FindChildTraverse && ctx.FindChildTraverse(SPEC_PANELS[i]);
          if (p) p.visible = true;
        } catch (e) {}
      }
    }
    function hidePanel(c, forceRestoreVisible) {
      if (!c) return;
      var id = '';
      try { id = c.id || ''; } catch (eid) {}
      if (!hiddenNatives) hiddenNatives = [];
      var key = id || ('__panel_' + hiddenNatives.length);
      if ((forceRestoreVisible || c.visible) && !hiddenNativeIds[key]) {
        hiddenNativeIds[key] = 1;
        hiddenNatives.push({ p: c, v: forceRestoreVisible ? true : !!c.visible, id: id });
      }
      try { c.visible = false; } catch (e) {}
    }
    // view: 'full' (show everything) | 'hidden' (hide the whole gameplay HUD) | 'game' (keep
    // radar + HP/ammo, drop the spectator observer panel). Called every frame so a panel the
    // game re-shows (round start, etc.) gets re-hidden; a mode change restores the baseline first.
    function applyHudView(view) {
      if (view !== curHudView) { restoreGameHud(); curHudView = view; }
      if (view === 'full') { forceFullHudPanels(); return; }
      if (!hiddenNatives) hiddenNatives = [];
      if (view === 'hidden') {
        var nk = ctx.GetChildCount ? ctx.GetChildCount() : 0;
        for (var i = 0; i < nk; i++) {
          var c = ctx.GetChild(i);
          if (!c || OURS[c.id]) continue;
          if ((c.FindChildTraverse && (c.FindChildTraverse('SliderRow') || c.FindChildTraverse('ControlRow')))) continue;
          if (c.visible) hidePanel(c, false);
        }
      } else { // 'game'
        for (var s = 0; s < SPEC_PANELS.length; s++)
          hidePanel(ctx.FindChildTraverse && ctx.FindChildTraverse(SPEC_PANELS[s]), true);
      }
    }
)TLJS"
R"TLJS(
    var viewportHudPanels = [];
    function isOurPanel(p) {
      return !!(p && OURS[p.id]);
    }
    function isViewportHudRoot(p) {
      return !!(p && VIEWPORT_HUD_ROOTS[p.id]);
    }
    function isNativeDemoBarPanel(p) {
      return !!(p && p.FindChildTraverse && (p.FindChildTraverse('SliderRow') || p.FindChildTraverse('ControlRow')));
    }
    function restoreHudViewport() {
      for (var i = 0; i < viewportHudPanels.length; i++) {
        var p = viewportHudPanels[i];
        try {
          if (!p || (p.IsValid && !p.IsValid())) continue;
          // Snap the transform back with NO animation, then re-arm the panel's own transition on
          // the next frame -- once the transform has already settled -- so toggling back to Hide All
          // doesn't trigger an animated "slide out", and native HUD anims still work after we let go.
          p.style.transition = 'none';
          // First snap the compositor out of the scaled preview. HudBottomGradient otherwise
          // keeps drawing its old preview-sized surface even after Panorama reports transform=null.
          p.style.transform = 'none';
          p.__camViewportAdjusted = false;
          p.__camViewportCss = '';
          (function (panel) {
            $.Schedule(0, function () {
              try {
                if (!panel || (panel.IsValid && !panel.IsValid())) return;
                // Re-taken since (a one-frame restore during a seek): clearing now would wipe the
                // fresh preview transform while __camViewportCss says it is applied, and the HUD
                // stayed full-size until the preview rect next moved ("VP scale breaks").
                if (panel.__camViewportAdjusted) return;
                // Then release every inline override to CS2. Its native demo-controller rule keeps
                // the lower gameplay HUD above the timeline; leaving transform='none' masks it.
                panel.style.transformOrigin = null;
                panel.style.transform = null;
                panel.style.transition = null;
              } catch (e3) {}
            });
          })(p);
        } catch (e) {}
      }
      viewportHudPanels = [];
      forceClearViewportTransforms();
    }
    // Defensive: the tracked viewportHudPanels list lives in THIS script closure, but the native
    // HUD (and so this whole timeline script) can rebuild while the editor is open. After such a
    // rebuild the live HUD roots still carry the editor's scale3d/translate3d + __camViewportAdjusted
    // from the PREVIOUS closure, yet the new closure's tracked list is empty -- so the loop above
    // restores nothing and the gameplay HUD stays shrunk/offset (not lined up with the now full-screen
    // world) after leaving the camera editor. Sweep the live HUD roots by their own flag and clear any
    // orphaned transform. Mirrors the visibility-side forceFullHudPanels() guard.
    function forceClearViewportTransforms() {
      var n = ctx.GetChildCount ? ctx.GetChildCount() : 0;
      for (var i = 0; i < n; i++) {
        var c = ctx.GetChild(i);
        if (!c || !isViewportHudRoot(c) || !c.__camViewportAdjusted) continue;
        try {
          c.style.transition = 'none';
          c.style.transform = 'none';
          c.__camViewportAdjusted = false;
          c.__camViewportCss = '';
          (function (panel) {
            $.Schedule(0, function () {
              try {
                if (!panel || (panel.IsValid && !panel.IsValid())) return;
                if (panel.__camViewportAdjusted) return; // re-taken since: see restoreHudViewport
                panel.style.transformOrigin = null;
                panel.style.transform = null;
                panel.style.transition = null;
              } catch (e2) {}
            });
          })(c);
        } catch (e) {}
      }
    }
    function rememberHudViewportPanel(p) {
      if (!p || p.__camViewportAdjusted) return;
      try {
        p.__camViewportAdjusted = true;
        viewportHudPanels.push(p);
      } catch (e) {}
    }
    // HOW WIDE IS THE SCREEN -- one answer for every site that needs it. Read the HUD CONTEXT
    // first: it is relaid out the moment the window is resized. Our own root (CamTimelineRoot) is
    // NOT -- it keeps reporting its previous size until something (a visibility toggle, a rebuild)
    // forces a relayout, which is what left the docked native bar at its old width after a resize
    // until a dock-tab switch happened to relayout root. Root is only the fallback for the first
    // frames after a build, when ctx can still read 0.
    function hudSizeCss() {
      var sx = ctx.actualuiscale_x || root.actualuiscale_x || 1;
      var sy = ctx.actualuiscale_y || root.actualuiscale_y || 1;
      var w = ctx.actuallayoutwidth || 0; if (w < 16) w = root.actuallayoutwidth || 0;
      var h = ctx.actuallayoutheight || 0; if (h < 16) h = root.actuallayoutheight || 0;
      return { w: w / sx, h: h / sy, sx: sx, sy: sy };
    }
    function applyGameHudViewport(active, rect) {
      if (!active || !rect || rect.length < 4) { restoreHudViewport(); return; }
      var hs = hudSizeCss();
      var rsx = hs.sx, rsy = hs.sy, rw = hs.w, rh = hs.h;
      if (!(rw > 1 && rh > 1)) { restoreHudViewport(); return; }
      var x0 = Number(rect[0]), y0 = Number(rect[1]), x1 = Number(rect[2]), y1 = Number(rect[3]);
      if (!(isFinite(x0) && isFinite(y0) && isFinite(x1) && isFinite(y1) && x1 > x0 && y1 > y0)) {
        restoreHudViewport(); return;
      }
      // Map the FULL screen 1:1 into the EXACT preview rect the world blit uses -- NO insets, so
      // the HUD lines up pixel-for-pixel with the scaled game view (the rect already matches the
      // game aspect, so sx == sy). x0*rw / y0*rh is the rect's top-left in virtual px.
      var sx = x1 - x0, sy = y1 - y0;
      var tx = x0 * rw, ty = y0 * rh;
      var n = ctx.GetChildCount ? ctx.GetChildCount() : 0;
      for (var i = 0; i < n; i++) {
        var c = ctx.GetChild(i);
        if (!c || isOurPanel(c) || isNativeDemoBarPanel(c) || !isViewportHudRoot(c)) continue;
        rememberHudViewportPanel(c);
        try {
          // transform-origin stays '0% 0%' (each panel's OWN top-left) -- the only form Panorama
          // reliably accepts; a 3-value/px origin throws and silently drops the whole transform.
          // To still scale uniformly about the SCREEN origin (so RIGHT/BOTTOM-anchored panels like
          // death notices / ammo land inside the rect, not under the inspector), we compensate in
          // the translate: a panel laid out at screen (px,py) needs translate (tx-(1-sx)*px, ...).
          // Full-screen panels have px=py=0 -> this reduces to the plain (tx,ty) mapping, and if an
          // offset is unavailable it falls back to that same safe behaviour.
          var px = (c.actualxoffset || 0) / rsx, py = (c.actualyoffset || 0) / rsy;
          var ptx = tx - (1 - sx) * px, pty = ty - (1 - sy) * py;
          // Panorama applies the listed translate before the scale for these HUD panels, so the
          // translate itself gets scaled. Feed it pre-divided values to produce screen-pixel tx/ty.
          var cssTx = ptx / sx, cssTy = pty / sy;
          var css = 'translate3d(' + cssTx.toFixed(1) + 'px, ' + cssTy.toFixed(1) + 'px, 0px) scale3d('
            + sx.toFixed(5) + ', ' + sy.toFixed(5) + ', 1)';
          // Three style writes across ~26 HUD roots, every frame a workspace is open, almost all
          // of them re-stating the value already there: the transform only moves when the preview
          // rect changes or the panel itself is laid out somewhere new (a death notice arriving,
          // the ammo counter changing width). Compare first. The remembered string lives on the
          // panel, so it is dropped with the panel when CS2 rebuilds the HUD -- and it is cleared
          // on release below, so a panel we hand back is always rewritten when we take it again.
          if (c.__camViewportCss === css) continue;
          c.__camViewportCss = css;
          c.style.transition = 'none'; // snap into the preview rect; no animated slide/scale
          c.style.transformOrigin = '0% 0%';
          c.style.transform = css;
        } catch (e2) {}
      }
    }

    // ---- root + hit catcher + panel ------------------------------------
    var root = $.CreatePanel('Panel', ctx, 'CamTimelineRoot', {});
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%'; root.style.zIndex = '55'; // z-layer map: PanoramaBridge.h
    var catcher = mk('Panel', root); catcher.hittest = true; catcher.visible = false;
    catcher.style.width = '100%'; catcher.style.height = '100%';
    catcher.SetPanelEvent('onactivate', function () { /* swallow stray clicks */ });

    var panel = $.CreatePanel('Panel', root, 'CamTimelineBar', {}); panel.hittest = true;
    var ps = panel.style;
    ps.horizontalAlign = 'center'; ps.verticalAlign = 'bottom'; ps.marginBottom = '0px';
    ps.width = (LABELW + W + 28) + 'px';
    ps.backgroundColor = S.bg; ps.borderRadius = '6px'; ps.border = '1px solid ' + S.panelBorder;
    ps.boxShadow = '#000000cc 0px 0px 12px 2px'; // native demo-bar style depth
    ps.flowChildren = 'down'; ps.paddingTop = '8px'; ps.paddingBottom = '10px';
    ps.paddingLeft = '14px'; ps.paddingRight = '14px'; ps.fontFamily = S.font;

    // ===================== HEADER =======================================
    var header = mk('Panel', panel); header.style.flowChildren = 'right'; header.style.width = '100%';
    header.style.marginBottom = '8px';
    var hTitle = lbl(header, 'CAMERA TIMELINE', S.value, 15); hTitle.style.fontWeight = 'bold';
    hTitle.style.letterSpacing = '2px'; hTitle.style.verticalAlign = 'center';
    var hInfo = lbl(header, '', S.hint, 11); hInfo.style.marginLeft = '14px';
    hInfo.style.width = 'fill-parent-flow(1.0)'; hInfo.style.verticalAlign = 'center';
    hInfo.style.letterSpacing = '0.5px';
    var mouseLbl = lbl(header, '', S.accent, 12); mouseLbl.style.verticalAlign = 'center';
    mouseLbl.style.marginRight = '10px'; mouseLbl.style.whiteSpace = 'nowrap';
    var clearConfirm = false;
    var clearBtn = btn(header, 'Clear', function () {
      if (!clearConfirm) { clearConfirm = true; clearBtn.__lbl.text = 'Clear — sure?'; clearBtn.__lbl.style.color = S.accent; return; }
      clearConfirm = false; clearBtn.__lbl.text = 'Clear'; clearBtn.__lbl.style.color = S.value;
      cmd('mirv_filmmaker camtl clear');
    }, S.value);
    // ("Curve Editor" view toggle removed: the graph editor is the curve editor now.)
    var closeBtn = btn(header, '✕', function () { cmd('mirv_filmmaker camtl close'); }, S.value);
    // In the Camera Editor, switch the bottom panel back to the (default) graph editor.
    var graphBtn = btn(header, '≡ Graph', function () { cmd('mirv_filmmaker editor curveeditor graph'); }, S.accent);
)TLJS"
R"TLJS(
    // ===================== SHARED TRANSPORT =============================
    var trow = mk('Panel', panel); trow.style.flowChildren = 'right'; trow.style.width = '100%';
    // The original design's body is `gap:8px` between its three rows; height is whatever the controls need.
    trow.style.height = 'fit-children'; trow.style.marginBottom = '8px';
    var playBtn = btn(trow, '▶', function () {
      var isPlaying = transportShownPlaying();
      transportOverride = !isPlaying;
      transportOverrideUntil = nowMs() + 2500;
      setTransportButton(transportOverride);
      cmd(isPlaying ? 'mirv_filmmaker camtl pause' : 'mirv_filmmaker camtl play');
    }, S.accent);
    // Original design: `width:38px; padding:7px 0; border-radius:3px`, accent fill.
    playBtn.style.width = '38px';
    playBtn.style.paddingLeft = '0px';
    playBtn.style.paddingRight = '0px';
    playBtn.style.paddingTop = '7px'; playBtn.style.paddingBottom = '7px';
    playBtn.style.borderRadius = '3px';
    playBtn.__lbl.style.width = '100%';
    playBtn.__lbl.style.textAlign = 'center';
    // Same width for every icon control in the row, so the transport reads as one instrument
    // rather than five loose buttons. TROW_GAP is the ONE gap used between groups below.
    var ICON_W = 33, TROW_GAP = 12;
    function iconBtn(glyph, onClick) {
      var b = btn(trow, glyph, onClick, S.value);
      b.style.width = ICON_W + 'px';
      b.style.paddingLeft = '0px'; b.style.paddingRight = '0px';
      b.__lbl.style.width = '100%'; b.__lbl.style.textAlign = 'center';
      return b;
    }
    // ONE JOINED PAIR, as the original design drew it: a 1px-divided group of two 44px halves inside a
    // single #ffffff1f border, 11px off the play button. The prev/next-KEY jumps that used to sit
    // here are not in the original design's transport -- the KEYFRAME row below owns keyframes now.
    var stepGroup = mk('Panel', trow);
    stepGroup.style.flowChildren = 'right';
    stepGroup.style.marginLeft = '11px';
    stepGroup.style.verticalAlign = 'center';
    stepGroup.style.borderRadius = '3px';
    stepGroup.style.border = '1px solid #ffffff1f';
    stepGroup.style.overflow = 'noclip';
    function stepBtn(glyph, onClick) {
      var b = btn(stepGroup, glyph, onClick, S.value);
      b.style.width = '44px';
      b.style.paddingLeft = '0px'; b.style.paddingRight = '0px';
      b.style.paddingTop = '10px'; b.style.paddingBottom = '10px';
      b.style.marginRight = '0px';
      b.style.borderRadius = '0px';
      b.__bgBase = '#ffffff14'; b.__brBase = 'rgba(0,0,0,0)';
      b.__lbl.style.width = '100%'; b.__lbl.style.textAlign = 'center';
      btnVisual(b);
      return b;
    }
    stepBtn('◀ 1', function () { if (st) cmd('mirv_filmmaker camtl scrub ' + (activeTick() - 1)); });
    var stepDiv = mk('Panel', stepGroup);
    stepDiv.hittest = false;
    stepDiv.style.width = '1px'; stepDiv.style.height = '100%';
    stepDiv.style.backgroundColor = '#ffffff1f';
    stepBtn('1 ▶', function () { if (st) cmd('mirv_filmmaker camtl scrub ' + (activeTick() + 1)); });
    // Linear | Smooth: the path's ONE interpolation mode, as a segmented pair on the transport --
    // this used to live only in the keyframe footer, which is hidden in the editor (hosted) mode,
    // so the camera timeline had no way to say what the graph's Smooth button says. Both surfaces
    // now drive the same CameraPath::Interp (and the graph's handles mirror it), so this pair and
    // the graph's Smooth/Linear can never disagree. The '∿' matches the graph's own button.
    var interpLin = btn(trow, 'Linear', function () { cmd('mirv_filmmaker camtl interp linear'); }, S.value);
    interpLin.style.marginLeft = '12px'; interpLin.style.marginRight = '2px';
    var interpSmooth = btn(trow, '∿ Smooth', function () { cmd('mirv_filmmaker camtl interp cubic'); }, S.value);
    var tReadout = lbl(trow, '', S.value, 13); tReadout.style.verticalAlign = 'center';
    // Original design: Stratum2Mono 15/700, takes the slack between the segmented pair and the dial.
    tReadout.style.marginLeft = '14px'; tReadout.style.width = 'fill-parent-flow(1.0)';
    tReadout.style.fontFamily = 'Stratum2Mono, monospace';
    tReadout.style.fontSize = '15px'; tReadout.style.fontWeight = 'bold';
    // One line, always. This slot holds the scrub hint, and a wrapping hint was what made the
    // whole row change height (see the hint text below).
    tReadout.style.whiteSpace = 'nowrap'; tReadout.style.textOverflow = 'ellipsis';
    tReadout.style.overflow = 'squish';
    tReadout.style.whiteSpace = 'nowrap'; tReadout.style.textOverflow = 'ellipsis';
    // Demo speed: the SAME scrubber the native demo bar carries, not a row of preset pills. Six
    // fixed buttons could not express 0.7x or 12x at all, and they took more width than the
    // continuous control does. Right-anchored so the transport reads left-to-right: play, step,
    // where you are, how fast.
    var speedScrub = buildSpeedScrubber(trow, { width: 250, height: 30, font: S.font });
    speedScrub.strip.style.marginLeft = TROW_GAP + 'px';

    // ===================== TIMELINE VIEW ================================
    var tl = mk('Panel', panel); tl.style.flowChildren = 'down'; tl.style.width = (LABELW + W) + 'px';

    // Timeline scrubber spans the full content width.
    var TLW = LABELW + W;
    var srow = mk('Panel', tl); srow.style.width = TLW + 'px'; srow.style.height = '42px';
    // 22px tall: a 14px diamond's rotated diagonal is ~19.8px plus its border, centred at cy=11.
    // The old 18px strip clipped every diamond's top point (see the note on diamond()).
    // SCENE KEYS mode grows this to two compact lanes (camera context + editable scene events).
    var diamWrap = mk('Panel', srow); diamWrap.hittest = false;
    diamWrap.style.width = TLW + 'px'; diamWrap.style.height = '22px'; diamWrap.style.position = '0px 0px 0px';
    var sceneLaneActive = false;
    var SROW_BASE = 42, SROW_SCENE = 64, DIAM_H_BASE = 22, DIAM_H_SCENE = 40;
    // Render-range In/Out handle overlay (same row, on top of diamonds, non-interactive).
    var rangeWrap = mk('Panel', srow); rangeWrap.hittest = false;
    rangeWrap.style.width = TLW + 'px'; rangeWrap.style.height = '42px'; rangeWrap.style.position = '0px 0px 0px';

)TLJS"
R"TLJS(
    // (No separate track panel: the native Slider draws its own groove, so a second
    // trackBg here produced a doubled line that ran past the keyframes.)
    // Native CS2 sliders need BOTH the class (styling) and the direction ATTRIBUTE
    // (drag axis): e.g. <Slider class="HorizontalSlider" direction="horizontal"/>.
    // Without direction the Slider defaults to vertical, so the thumb drags up/down
    // instead of left/right -- pass it in the CreatePanel construction props.
    // ---- the original design's track ---------------------------------------------------------------
    // Drawn BEFORE the Slider so the Slider sits on top and keeps the drag. All three are
    // non-hittest: a panel over the Slider that hit-tests would swallow the press.
    // A FULL-HEIGHT WRAPPER at 0,0, exactly like diamWrap/rangeWrap above, with the 8px rail
    // inset by margin inside it. Positioning the 8px panel itself with `position: 0 26 0` put it
    // outside the row's stack and only its top border survived -- the rail read as a hairline.
    var railWrap = mk('Panel', srow); railWrap.hittest = false;
    railWrap.style.width = TLW + 'px'; railWrap.style.height = '42px';
    railWrap.style.position = '0px 0px 0px';
    var railBg = mk('Panel', railWrap); railBg.hittest = false;
    railBg.style.width = TLW + 'px'; railBg.style.height = '8px';
    railBg.style.marginTop = '26px';
    railBg.style.borderRadius = '4px';
    railBg.style.backgroundColor = '#10141aff';
    railBg.style.border = '1px solid #ffffff1a';
    railBg.style.overflow = 'clip';
    var railFill = mk('Panel', railBg); railFill.hittest = false;
    railFill.style.height = '100%'; railFill.style.width = '0px';
    railFill.style.backgroundColor = S.accent;

    // The playhead runs the FULL height of the track, not just the rail -- it has to read against
    // the keyframe diamonds above it, which is where you actually aim.
    var headWrap = mk('Panel', srow); headWrap.hittest = false;
    headWrap.style.width = TLW + 'px'; headWrap.style.height = '42px';
    headWrap.style.position = '0px 0px 0px';
    var headLine = mk('Panel', headWrap); headLine.hittest = false;
    headLine.style.width = '2px'; headLine.style.height = '42px';
    headLine.style.backgroundColor = '#ff5a5aff';
    headLine.style.position = '0px 0px 0px';
    var headKnob = mk('Panel', headLine); headKnob.hittest = false;
    headKnob.style.width = '10px'; headKnob.style.height = '10px';
    headKnob.style.borderRadius = '2px';
    headKnob.style.backgroundColor = '#ff5a5aff';
    headKnob.style.position = '-4px 22px 0px';

    // KEEP `HorizontalSlider`. The class is not just skin: the slider's drag maps the pointer
    // onto its TRACK's laid-out geometry, and a classless slider has no track to map onto, so it
    // renders but cannot be dragged -- reported as "I can't move the red thing left and right".
    // The stock grey chrome is removed below by making the slider's own parts transparent, which
    // costs nothing behavioural. (It is also emphatically not hidden with `opacity: 0`: a fully
    // transparent Panorama panel stops taking the drag at all.)
    var scrub = $.CreatePanel('Slider', srow, 'CamScrub', { direction: 'horizontal' });
    scrub.AddClass('HorizontalSlider');
    // Its band is the LOWER part of the track (y 18..42), not the whole 42px. The top 22px holds
    // the keyframe diamonds, which are created BEFORE this and own their clicks;
    // a full-height Slider sits on top of them in hit order and swallows those presses.
    scrub.style.width = TLW + 'px'; scrub.style.height = '24px'; scrub.style.position = '0px 18px 0px';
    // The band alone was not enough: the Slider's hit area still reached the diamonds, so a click
    // on one was a scrub and a right-click never opened its shake menu (2026-10-15 log: diamond
    // clicks arrived as `camtl scrub`, never `camtl select`). Diamonds go ABOVE the Slider in hit
    // order; diamWrap itself does not hittest, so a click between diamonds still reaches the Slider.
    try { srow.MoveChildAfter(diamWrap, scrub); } catch (eOrder) {}
    diamWrap.style.zIndex = '2';
    // rgba(0,0,0,0), NOT S.clear -- this script's theme has no `clear` token, so that wrote
    // `undefined` into a style, which Panorama throws on. The throw aborted the whole panel
    // build, so $.CamTimeline never registered and the bar froze at its build-time
    // standalone card: no keyframe row, no speed dial, no diamonds, overlapping the
    // inspector. See the panorama-style-empty-string note.
    scrub.style.backgroundColor = 'rgba(0,0,0,0)';
    // A Slider paints its OWN parts even with no class on it: SliderTrack, SliderTrackProgress,
    // SliderThumb and SliderDefault (confirmed by probing a live one). Those are the grey blocks
    // that were sitting on the rail. Made TRANSPARENT rather than hidden -- the drag is the
    // Slider's, and hiding its internals risks the thing this panel exists to do. The rail, the
    // fill and the playhead drawn above are the only visible track.
    for (var sc = 0; sc < scrub.GetChildCount(); sc++) {
      try {
        var sp = scrub.GetChild(sc);
        sp.style.backgroundColor = 'rgba(0,0,0,0)';
        sp.style.border = '0px solid rgba(0,0,0,0)';
        sp.style.boxShadow = 'none';
      } catch (eSlider) {}
    }
)TLJS"
R"TLJS(
)TLJS"
R"TLJS(
    // ===================== SHARED KEYFRAME FOOTER =======================
    var keyFooter = mk('Panel', panel); keyFooter.style.flowChildren = 'right'; keyFooter.style.width = '100%';
    keyFooter.style.marginTop = '8px'; keyFooter.style.verticalAlign = 'middle';
    var K = {};
    // Original design: `KEYFRAME`, 12/700, letter-spacing 1.6, #c8d3df.
    K.kLabel = lbl(keyFooter, 'KEYFRAME', S.label, 12); K.kLabel.style.verticalAlign = 'center';
    K.kLabel.style.fontWeight = 'bold'; K.kLabel.style.letterSpacing = '1.6px';
    K.kLabel.style.marginRight = '10px'; K.kLabel.style.whiteSpace = 'nowrap';
    K.add = btn(keyFooter, '+ Add', function () { cmd('mirv_filmmaker camtl addkey'); }, S.accent);
    K.del = btn(keyFooter, '− Delete', function () { if (st && st.selected >= 0) cmd('mirv_filmmaker camtl delkey ' + st.selected); }, S.value);
    K.retime = btn(keyFooter, '⟲ Tick→Here', function () { if (st && st.selected >= 0) cmd('mirv_filmmaker camtl movekey ' + st.selected + ' ' + st.tick); }, S.value);
    K.ease = btn(keyFooter, 'Ease: None', function () {
      if (!st || st.selected < 0) return;
      var nx = ((st.selEase || 0) + 1) % 4; cmd('mirv_filmmaker camtl ease ' + st.selected + ' ' + EASE[nx]);
    }, S.value);
    K.spdMinus = btn(keyFooter, '−', function () { stepSpeed(-1); }, S.accent);
    K.spdLbl = lbl(keyFooter, 'Seg x1.00', S.value, 13); K.spdLbl.style.verticalAlign = 'center'; K.spdLbl.style.width = '92px'; K.spdLbl.style.textAlign = 'center';
    K.spdPlus = btn(keyFooter, '+', function () { stepSpeed(1); }, S.accent);
    // (The old footer "Curve:" button moved to the transport row as the Linear|Smooth pair --
    // the footer is hidden in hosted mode, which left the editor with no interp control at all.)
    // Original design: a spacer pushes speed-mode and timing to the right edge of the row.
    var kSpacer = mk('Panel', keyFooter); kSpacer.hittest = false;
    kSpacer.style.width = 'fill-parent-flow(1.0)'; kSpacer.style.height = '1px';
    // SEGMENTED, not a cycle button -- every option on screen (Docs/panorama/panorama_ui_guide.md, "No dead
    // dropdowns"). Same shape as the Linear|Smooth pair on the transport row above.
    function segRow(items, apply) {
      var out = { btns: {} };
      for (var i = 0; i < items.length; i++) (function (key, cap, first) {
        var b = btn(keyFooter, cap, function () { apply(key); }, S.value);
        b.style.marginLeft = first ? '6px' : '0px';
        b.style.marginRight = '2px';
        out.btns[key] = b;
      })(items[i][0], items[i][1], i === 0);
      out.sync = function (cur) {
        for (var k in out.btns) btnSel(out.btns[k], k === cur);
      };
      return out;
    }
    K.smSeg = segRow([['manual', 'Manual'], ['persegment', 'Per-Seg'], ['constant', 'Constant']],
      function (k) { cmd('mirv_filmmaker marker speedmode ' + k); });
    K.tmSeg = segRow([['live', 'Live'], ['freeze', 'Freeze']],
      function (k) { cmd('mirv_filmmaker marker timing ' + k); });
    // Aliases so the rest of the render hook's existing writes keep working.
    K.sm = K.smSeg.btns.manual;
    K.tm = K.tmSeg.btns.live;

    // =====================================================================
    var st = null;
    var graphExpActive = false; // experimental graph editor owns the curve zone -> scrub drives IT
    var lastTlSig = '', lastContentW = -1;
    var lastVirtW = -1, lastVirtH = -1; // HUD virtual size; a change means resolution/aspect moved
    // Dynamically-created Panorama sliders work in their default 0..1 range (setting
    // min/max/out-of-range value is unreliable and leaves the thumb stuck mid-track), so
    // we keep value normalized and map 0..1 <-> tick / channel-value ourselves.
    var scrubT0 = 0, scrubT1 = 1;
    var lastHeadPx = -1;   // render() runs every frame; only move the head when it moved
    var scrubSyncing = false;
    var transportOverride = null, transportOverrideUntil = 0;
    function nowMs() { return (new Date()).getTime(); }
    function transportShownPlaying() {
      return transportOverride !== null ? transportOverride : !!(st && st.playing);
    }
    function syncTransportButton() {
      if (st && transportOverride !== null && (st.playing === transportOverride || nowMs() > transportOverrideUntil)) {
        transportOverride = null;
      }
      setTransportButton(transportShownPlaying());
    }
    function setTransportButton(playing) {
      playBtn.__lbl.text = playing ? '▮▮' : '▶';
      playBtn.__lbl.style.fontSize = playing ? '15px' : '13px';
    }

    function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
    function sliderTick(v, a, b) { return Math.round(a + v * (b - a)); }

    function frac(tick, t0, t1) { var d = t1 - t0; return d > 0 ? (tick - t0) / d : 0; }
    function activeTick() {
      return st && st.scrubbing ? st.scrubTick : (st ? st.tick : 0);
    }
    function gotoKey(dir) {
      if (!st || !st.markers || st.markers.length === 0) return;
      var cur = st.tick, bestI = -1, bestT = null;
      for (var i = 0; i < st.markers.length; i++) {
        var t = st.markers[i].tick;
        if (dir < 0 && t < cur) { if (bestT === null || t > bestT) { bestT = t; bestI = i; } }
        if (dir > 0 && t > cur) { if (bestT === null || t < bestT) { bestT = t; bestI = i; } }
      }
      if (bestI >= 0) cmd('mirv_filmmaker camtl select ' + bestI);
    }
    function stepSpeed(d) {
      if (!st || st.selected < 0) return;
      var steps = [0.2, 0.5, 0.8, 1.0], cur = st.selSpeedMul, idx = 0, best = 1e9;
      for (var i = 0; i < steps.length; i++) { var dd = Math.abs(steps[i] - cur); if (dd < best) { best = dd; idx = i; } }
      idx += (d > 0 ? 1 : -1); if (idx < 0) idx = 0; if (idx >= steps.length) idx = steps.length - 1;
      cmd('mirv_filmmaker camtl speed ' + st.selected + ' ' + steps[idx].toFixed(2));
    }
    // CS2 sliders fire SliderValueChanged (while dragging) + SliderReleased (on let-go),
    // delivered via $.RegisterEventHandler with the value as the 2nd arg -- NOT the
    // 'onvaluechanged' panel event. While dragging we PREVIEW the camera (smooth, no
    // world seek); on release we seek the demo world to the final tick.
    $.RegisterEventHandler('SliderValueChanged', scrub, function (panel, v) {
      if (scrubSyncing || (st && st.playing)) return;
      // Fractional on purpose: a slow drag moves the world BETWEEN ticks (Movie/DemoScrubMath.h),
      // which a rounded tick would turn back into tick-by-tick snapping. Release still lands whole.
      var tf = scrubT0 + v * (scrubT1 - scrubT0);
      tReadout.text = 'tick ' + Math.round(tf);
      // While the experiment owns the curve zone, the scrubber previews ITS curves (not the stable
      // path's) so the two never fight for the camera -- same commands the experiment's own ruler uses.
      if (graphExpActive) cmd('mirv_filmmaker grapheditor playhead ' + tf.toFixed(2));
      else cmd('mirv_filmmaker camtl scrubpreview ' + tf.toFixed(2));
    });
    $.RegisterEventHandler('SliderReleased', scrub, function (panel, v) {
      var t = sliderTick(v, scrubT0, scrubT1);
      // A click on the bar while the path plays carries playback on from that tick.
      if (st && st.playing) { if (!graphExpActive) cmd('mirv_filmmaker camtl playfrom ' + t); return; }
      if (graphExpActive) { cmd('mirv_filmmaker scrub release ' + t); cmd('mirv_filmmaker grapheditor playhead release'); }
      else cmd('mirv_filmmaker camtl scrub ' + t);
    });
    function rebuildTimelineDiamonds() {
      diamWrap.RemoveAndDeleteChildren();
      if (!st) return;
      var t0 = st.tickMin, t1 = st.tickMax; if (t1 <= t0) t1 = t0 + 1;
      // Half the rotated diagonal (14 * sqrt2 / 2) plus the border: the clamp margin that keeps
      // the first/last keys' full shape inside the strip instead of half a diamond off each end.
      var DIAM = 14, EDGE = DIAM * 0.7071 + 1;
      var sceneMode = st.uiEditorInspector === 'scene';
      if (sceneMode !== sceneLaneActive) {
        sceneLaneActive = sceneMode;
        var sH = sceneMode ? SROW_SCENE : SROW_BASE;
        var dH = sceneMode ? DIAM_H_SCENE : DIAM_H_BASE;
        srow.style.height = sH + 'px';
        diamWrap.style.height = dH + 'px';
        rangeWrap.style.height = sH + 'px';
        railWrap.style.height = sH + 'px';
        headWrap.style.height = sH + 'px';
        headLine.style.height = sH + 'px';
        var scrubY = sceneMode ? 36 : 18;
        scrub.style.position = '0px ' + scrubY + 'px 0px';
        railBg.style.marginTop = (scrubY + 8) + 'px';
        headKnob.style.position = '-4px ' + (scrubY + 4) + 'px 0px';
      }
      var camCY = sceneMode ? 9 : 11;
      var sceneCY = 29;
      // Camera markers (always drawn when present). Clicking one goes to that camera and
      // right-clicking opens its shake menu on EVERY inspector tab: in SCENE KEYS they used to be
      // read-only, which read as "clicking the diamonds does nothing" whenever that tab was open.
      if (st.markers) for (var i = 0; i < st.markers.length; i++) (function (i) {
        var x = frac(st.markers[i].tick, t0, t1) * TLW;
        var cx = Math.min(Math.max(x, EDGE), TLW - EDGE);
        var sel = (i === st.selected);
        diamond(diamWrap, cx, camCY, DIAM, sel, function () {
          cmd('mirv_filmmaker camtl select ' + i);
        }, function () {
          openShakeMenu(i, cx);
        });
        if (shakeSettingFor(i)) {
          var d = mk('Panel', diamWrap);
          d.hittest = false;
          d.style.position = (cx - 2).toFixed(1) + 'px ' + (camCY - 2) + 'px 0px';
          d.style.width = '4px'; d.style.height = '4px';
          d.style.borderRadius = '2px';
          d.style.backgroundColor = '#10141aee';
        }
      })(i);
      if (!sceneMode) return;
      var events = (st.sceneKeys && st.sceneKeys.events) || [];
      var selEv = (st.sceneKeys && st.sceneKeys.selectedEvent != null) ? st.sceneKeys.selectedEvent : -1;
      for (var ei = 0; ei < events.length; ei++) (function (ei) {
        var ev = events[ei];
        var x2 = frac(ev.tick, t0, t1) * TLW;
        var cx2 = Math.min(Math.max(x2, EDGE), TLW - EDGE);
        diamond(diamWrap, cx2, sceneCY, DIAM, ei === selEv, function () {
          cmd('mirv_filmmaker scenekeys select ' + ei);
        }, null);
      })(ei);
    }

    var lastRenderRangeSig = '';
    function rebuildRenderRangeHandles() {
      rangeWrap.RemoveAndDeleteChildren();
      if (!st) return;
      var active = !!(st.renderRangeActive && st.renderInTick < st.renderOutTickExclusive);
      if (!active && (!st.renderInTick || !st.renderOutTickExclusive || st.renderInTick >= st.renderOutTickExclusive)) return;
      var t0 = st.tickMin, t1 = st.tickMax; if (t1 <= t0) t1 = t0 + 1;
      // This bar now spans the CAMERA PATH, not the demo, so a render range set somewhere else in
      // the demo simply has no position here. Drawing it anyway put the flags off-panel (or
      // pinned to an edge, implying a boundary that is not there). No overlap = nothing to draw;
      // the render range still has its own overlay on the Regular Timeline, where it belongs.
      if (st.renderOutTickExclusive <= t0 || st.renderInTick >= t1) return;

      // Highlight region between In and Out
      // A range that only PARTLY overlaps the path is clipped to the bar rather than allowed to
      // draw past it -- the band should read as "this much of the shot is in the render", and a
      // triangle sitting outside the track reads as a handle you could drag there.
      var inX = clamp01(frac(st.renderInTick || 0, t0, t1)) * TLW;
      var outX = clamp01(frac(st.renderOutTickExclusive || 0, t0, t1)) * TLW;
      var rLeft = Math.min(inX, outX), rWidth = Math.abs(outX - inX);
      if (rWidth > 1) {
        var hl = mk('Panel', rangeWrap);
        // y tracks the scrub groove (slider at y18 + its groove inset); keep in step if it moves.
        hl.style.position = rLeft.toFixed(1) + 'px 28px 0px';
        hl.style.width = rWidth.toFixed(1) + 'px'; hl.style.height = '13px';
        hl.style.backgroundColor = '#2b7cd322'; hl.hittest = false;
      }

      // In handle: downward triangle (▲ pointing down)
      var inTri = mk('Panel', rangeWrap);
      inTri.style.position = (inX - 6).toFixed(1) + 'px 24px 0px';
      inTri.style.width = '0px'; inTri.style.height = '0px';
      inTri.style.borderLeft = '6px solid rgba(0,0,0,0)';
      inTri.style.borderRight = '6px solid rgba(0,0,0,0)';
      inTri.style.borderTop = '10px solid #4e9fffff';
      inTri.style.backgroundColor = 'rgba(0,0,0,0)';
      inTri.style.borderRadius = '0px';
      inTri.hittest = false;

      // Out handle: downward triangle
      var outTri = mk('Panel', rangeWrap);
      outTri.style.position = (outX - 6).toFixed(1) + 'px 24px 0px';
      outTri.style.width = '0px'; outTri.style.height = '0px';
      outTri.style.borderLeft = '6px solid rgba(0,0,0,0)';
      outTri.style.borderRight = '6px solid rgba(0,0,0,0)';
      outTri.style.borderTop = '10px solid #4e9fffff';
      outTri.style.backgroundColor = 'rgba(0,0,0,0)';
      outTri.style.borderRadius = '0px';
      outTri.hittest = false;
    }
    // ---- render-range overlay on the REGULAR (native) timeline ---------
    // Owns its own root, its own colours and its own draggable In/Out flags; see the fragment.
)TLJS"
#include "CameraTimelineRangeJs.h"
R"TLJS(
    function updateKeys(freeze) {
      var has = (st && st.selected >= 0);
      // The original design's caption is the constant word KEYFRAME -- it names the row, it is not a
      // readout. Which key is selected is already on the timeline (the lit diamond) and in
      // the ribbon's meta, and a caption that changes width shifts every button beside it.
      K.kLabel.text = 'KEYFRAME';
      K.del.visible = K.retime.visible = K.ease.visible = has;
      K.ease.__lbl.text = 'Ease: ' + EASE_LBL[(st && st.selEase) || 0];
      K.spdLbl.text = 'Seg x' + (st && st.selSpeedMul != null ? st.selSpeedMul.toFixed(2) : '1.00');
      var perSeg = (st && st.speedMode === 'Per-Segment');
      K.spdMinus.visible = K.spdPlus.visible = K.spdLbl.visible = (perSeg && has && !st.selIsLast);
      // Segmented now: light the segment that IS the current value instead of relabelling a
      // single cycle button. st.speedMode arrives capitalised ("PerSegment"), so it is folded to
      // the key spelling the console takes.
      // Light the segment that IS the current value. st.speedMode arrives capitalised
      // ("PerSegment"), so it is folded to the key spelling the console takes.
      var smKey = String((st && st.speedMode) || 'manual').toLowerCase().replace(/[^a-z]/g, '');
      if (smKey === 'perseg') smKey = 'persegment';
      K.smSeg.sync(smKey);
      K.tmSeg.sync(freeze ? 'freeze' : 'live');
    }
)TLJS"
// Per-keyframe GTA V shake menu (right-click a diamond). Its own fragment; it owns its panels,
// its own dismiss catcher and its own commands, and touches nothing the timeline already draws.
#include "CameraTimelineShakeMenuJs.h"
R"TLJS(
    // Responsive width: in hosted (editor) mode the bottom bar fills the space left of the
    // inspector, which varies with resolution / uiscale -- so the timeline width can't be the
    // fixed build-time W. Recompute the inner content width each render and restyle every
    // width-dependent panel, then invalidate the diamond cache so it relayouts at the new W.
    // Wrapped in try/catch: render() has no outer guard and a throw here would abort
    // the whole render (which also drops the injected native-bar MOUSE / CAM EDITOR buttons).
    function applyLayout(contentW) {
      try {
        contentW = Math.floor(contentW);
        if (contentW < 360) contentW = 360;
        if (contentW === lastContentW) return;
        lastContentW = contentW;
        W = contentW - LABELW; TLW = contentW;
        tl.style.width = contentW + 'px';
	srow.style.width = contentW + 'px';
	diamWrap.style.width = contentW + 'px';
	rangeWrap.style.width = contentW + 'px';
	scrub.style.width = contentW + 'px';
	railWrap.style.width = contentW + 'px';
	railBg.style.width = contentW + 'px';
	headWrap.style.width = contentW + 'px';
	lastTlSig = ''; lastRenderRangeSig = ''; // force relayout at new W
      } catch (e) { $.Msg('[camtl] applyLayout error: ' + e + '\n'); }
    }

    var api = {};
    // Called by CameraTimelineHud::Teardown BEFORE this panel is deleted. Deleting the panel does
    // NOT revert the styles we wrote onto CS2's own demo bar, so without this the bar is orphaned
    // at its docked width with nothing left alive to restore it.
    api.restoreNative = function () {
      try { setNativeHidden(false); nativeUndock(); applyHudView('full'); restoreHudViewport(); } catch (e) {}
    };
    api.render = function () {
      var raw = root.GetAttributeString('state', '');
      // No state pushed (C++ side stopped driving us, e.g. the panel is being torn down): restore
      // EVERYTHING we changed on the native HUD. nativeUndock() was missing here, so a bar that was
      // docked (narrowed to sit left of a workspace inspector) stayed short until some later render
      // happened to take the undock path -- the "timeline is smaller than the whole length, then
      // fixes itself after going in and out" report (2026-08-28).
      if (!raw) { closeShakeMenu(); root.visible = false; setNativeHidden(false); nativeUndock(); applyHudView('full'); restoreHudViewport(); return; }
      try { st = JSON.parse(raw); } catch (e) { return; }
      // Track the operator's column width before anything lays out against it this frame.
      // A live drag outranks the published width, and hands back once the published value has
      // caught up with it -- clearing the override on the command instead would show one frame
      // of the OLD stored width, which reads as the dock snapping back before it settles.
      if (dockInspWOverride > 0) {
        dockInspW = dockInspWOverride;
        if (typeof st.inspW === 'number' && Math.abs(st.inspW - dockInspWOverride) < 1) dockInspWOverride = 0;
      } else if (typeof st.inspW === 'number' && st.inspW > 0) {
        dockInspW = st.inspW;
      }

      // ---- resolution / aspect change guard --------------------------------------------
      // Changing resolution (especially the ASPECT: 4:3's HUD is 1440x1080 virtual, 16:9's is
      // 1920x1080) moves every width this panel derives. Anything memoised against the old
      // geometry has to be dropped in one place, or the bar keeps the layout it had before the
      // switch. Cheap: two reads and a compare per frame.
      var vsx = ctx.actualuiscale_x || 1, vsy = ctx.actualuiscale_y || 1;
      var vW = Math.round((ctx.actuallayoutwidth || 0) / vsx);
      var vH = Math.round((ctx.actuallayoutheight || 0) / vsy);
      if (vW > 1 && vH > 1 && (vW !== lastVirtW || vH !== lastVirtH)) {
        var firstMeasure = (lastVirtW < 0);
        lastVirtW = vW; lastVirtH = vH;
        if (!firstMeasure) {
          lastContentW = -1;          // force applyLayout to restyle every width-dependent panel
          lastTlSig = '';             // force the keyframe diamonds to relayout at the new width
          lastRenderRangeSig = '';    // ditto the render-range handles
          markerCaptureHold = 0;      // trust live marker reads again as soon as we are undocked
          $.Msg('[camtl] virtual size -> ' + vW + 'x' + vH + ' (layout caches invalidated)\n');
        }
      }
      patchNativeBar(); // keep the native demo bar de-cluttered + our button present
      // Camera Editor Mode hosts this panel and wants a clean workspace: hide the whole
      // gameplay HUD (radar/health/ammo/scoreboard/native demo bar). Restored on exit.
      var hosted = !!st.hosted;
      var renderWorkspace = hosted && !!st.renderOpen;
      S.accent = renderWorkspace ? S.renderAccent : S.cameraAccent;
      S.line = renderWorkspace ? '#aeb4bcdd' : '#f0b323dd';
      S.btnOn = renderWorkspace ? '#aeb4bc26' : '#f0b32333';
      var previewHidden = !!st.previewHudHidden;
      // Editor's "Regular Timeline" bottom mode = hosted with neither the camera overlay (st.open)
      // nor the graph (st.graphExp) active. There the native CS2 demo bar IS the bottom editor.
      var nativeMode = hosted && !st.open && !st.graphExp;
      // While hosted, the editor's UI-visibility picker chooses how much gameplay HUD shows;
      // the lightweight Config panel (ConfigHud) shares the SAME picker state and wants it
      // applied while it is open too. When neither is up the game HUD is fully restored.
      var cfgOpen = !!st.configOpen;
      // F8 BLACKOUT: one flag clears the screen. Our own root, the native demo bar and the whole
      // gameplay HUD (crosshair, radar, health/ammo, spectator strip) all go at once, and the
      // scaled workspace preview is released by the C++ side so the game gets the full frame back
      // instead of sitting in a corner surrounded by black. Everything here is render-time only:
      // nothing is closed, so the second press restores exactly what was up before.
)TLJS"
// SPLIT for MSVC's ~16380-byte cap on a single string literal, not for any semantic reason.
// Adjacent raw literals concatenate, so the seam is invisible to the script.
R"TLJS(
      var blackout = !!st.blackout;
      if (blackout) {
        applyHudView('hidden');
        applyGameHudViewport(false, null);
        setNativeHidden(true);
        nativeUndock();
        closeShakeMenu();
        root.visible = false;
        return;
      }
      applyHudView((hosted || cfgOpen) ? (st.hudView || 'full') : 'full');
      // Move the native CS2 gameplay HUD into the same preview rect as the rendered game.
      // This includes radar, score, player strip, health/ammo, inventory and death notices,
      // while excluding our editor/timeline chrome and the docked native demo bar.
      applyGameHudViewport((hosted || cfgOpen) && !!st.hudScale && (st.hudView || 'full') !== 'hidden', st.previewRect);
      // Native demo bar visibility: SHOWN in Regular Timeline mode (and in normal play); hidden when
      // a camera/graph overlay is up, our standalone panel is open, or a clean preview frame is
      // needed. Done up here -- BEFORE the hosted-layout early-return below -- so it never flashes
      // wrong during an unsettled first layout frame.
      // Decided from the RAW answer, deliberately. An earlier attempt debounced this hide to damp
      // the swap flicker and it was wrong for the same reason the dock's was: if the pump stops
      // while the debounce is holding, the native bar stays hidden with nothing to restore it.
      // The flicker's actual cause was two workspaces being enabled at once, fixed in C++
      // (CameraEditor_Set now closes Config), not here.
      var nativeBarVisible = !(!!st.open) && !(!!st.graphExp) && !previewHidden;
      setNativeHidden(!nativeBarVisible);
      updateRangeOverlay(nativeBarVisible); // render-range / loop flags whenever the native bar is visible
      closeBtn.visible = !hosted; // editor mode exits via its own "✕ Exit" button
      graphBtn.visible = false;   // bottom-panel switching is now the editor's bottom tab bar only
      // When hosted, this panel IS the editor's bottom bar under the preview. It fills the
      // entire width left of the inspector instead of floating as a card inside that bar.
      // Standalone timeline mode keeps the compact native-style card.
      if (hosted) {
        // Measure from the ALREADY-laid-out HUD context panel when our fresh root still reports 0
        // (the first layout passes after a build / resolution switch) -- otherwise the hosted bar
        // collapses to the compact fallback width and squishes for ~half a second until root
        // settles. Only restyle once we actually have a sane full-screen width.
        // ctx-first via hudSizeCss(): root lags a window resize (see the helper).
        var rw = hudSizeCss().w;
        var barW = renderWorkspace
          ? Math.floor(rw)
          : ((rw > editorInspectorW(rw)) ? Math.floor(rw - editorInspectorW(rw)) : 0);
        if (barW <= 0) return; // no valid width yet: hold last-good layout, retry next frame
        panel.style.horizontalAlign = 'left';
        panel.style.marginLeft = '0px';
        panel.style.marginBottom = EDITOR_BOTTOM_LIFT + 'px'; // rests at the bottom; tab bar sits ABOVE it
        panel.style.width = barW + 'px';
        // fit-children (NOT a fixed height): CameraEditorJs reads this panel's actual height
        // (#CamTimelineBar) to position the tab bar above it + shrink the preview.
        panel.style.height = 'fit-children';
        panel.style.borderRadius = '0px';
        panel.style.border = '0px solid transparent';
        panel.style.boxShadow = 'none';
        // The original design's dock body: `background:#0a0d11; padding:10px 12px`, flush and flat. The
        // standalone card keeps its own ground and 14/8 padding (restored in the else branch).
        panel.style.backgroundColor = '#0a0d11ff';
        panel.style.paddingLeft = '12px'; panel.style.paddingRight = '12px';
        panel.style.paddingTop = '10px'; panel.style.paddingBottom = '10px';
        applyLayout(barW - 28); // inner content fills the bar minus L/R padding
        // Regular Timeline mode: dock the native bar to fit left of the inspector (same barW).
        // Other hosted modes hide it; keep its geometry reset so it's correct when shown again.
        if (nativeMode) nativeDock(barW); else nativeUndock();
      } else {
        panel.style.horizontalAlign = 'center';
        panel.style.marginLeft = '0px';
        panel.style.marginBottom = '0px';
        panel.style.width = (LABELW + W_DEFAULT + 28) + 'px';
        panel.style.height = 'fit-children';
        panel.style.borderRadius = '6px';
        panel.style.border = '1px solid ' + S.panelBorder;
        panel.style.boxShadow = '#000000cc 0px 0px 12px 2px';
        panel.style.backgroundColor = S.bg;
        panel.style.paddingLeft = '14px'; panel.style.paddingRight = '14px';
        panel.style.paddingTop = '8px'; panel.style.paddingBottom = '10px';
        applyLayout(LABELW + W_DEFAULT); // restore the standalone default width
        // Config panel open (not hosted): dock the native demo bar to fit left of the Config
        // inspector (same width as the editor's Regular Timeline mode); otherwise restore the
        // native demo bar to full width / centre (pixel-perfect on exit).
        if (cfgOpen) {
          // ctx-first via hudSizeCss(): reading root here is what froze the bar at its old
          // width after a window resize until a dock-tab switch relaid root out.
          var crw = hudSizeCss().w;
          var cBarW = (crw > editorInspectorW(crw)) ? Math.floor(crw - editorInspectorW(crw)) : 0;
          if (cBarW > 0) nativeDock(cBarW);
        } else {
          nativeUndock();
        }
      }

      traceState('host=' + (hosted ? 'editor' : (cfgOpen ? 'config' : 'none'))
        + ' tab=' + (nativeMode ? 'native' : (st.open ? 'camera' : (st.graphExp ? 'graph' : '?')))
        + ' bar=' + (nativeBarVisible ? 1 : 0)
        + ' dock=' + Math.round(fitRowW) + '/' + fitK.toFixed(2)
        + ' hudView=' + (((hosted || cfgOpen) ? (st.hudView || 'full') : 'full'))
        + ' hudScale=' + (st.hudScale ? 1 : 0)
        + ' rect=' + (st.previewRect ? JSON.stringify(st.previewRect) : '-'));

      var graphExp = !!st.graphExp;
      graphExpActive = graphExp;
      var dockShown = !!st.open && !previewHidden;   // RAW: never delay a hide, see moDockOpenIsReal
      var dockRealOpen = moDockOpenIsReal(dockShown);
      root.visible = dockShown;
      if (dockShown !== moDockSeen) {
        moDockSeen = dockShown;
        // Arriving FROM the graph means the dock is shrinking, so it descends; opening from the
        // Regular Timeline means it is growing, so it rises. moDockCameFromGraph is latched below
        // while the graph is up, because st.graphExp has already flipped false by the time this
        // branch runs.
        if (dockShown && dockRealOpen) moDockIn(root, moDockCameFromGraph ? -14 : 14);
      }
      moDockCameFromGraph = !!st.graphExp;
      // (native demo bar visibility already resolved above, before the hosted early-return)
      if (!st.open || previewHidden) { closeShakeMenu(); catcher.visible = false; return; }

      // UI-mouse mode. The editor forces it on while open; the catcher absorbs stray
      // clicks so spectator target switching cannot leak through behind the panel.
      var cur = !!st.cursor;
      var forcedCur = !!st.cursorForced;
      // When hosted by the editor, the editor draws its own click-catcher (at a lower
      // z-index than this panel) so its inspector stays clickable; suppress ours here so
      // this full-screen catcher (z55) can't sit on top of the editor's inspector.
      catcher.visible = cur && !hosted; catcher.hittest = cur && !hosted;
      panel.hittest = true;
      // THE DOCK'S THIRD ROW (Docs/features/camera-editor-feature.md): KEYFRAME / + Add / - Delete /
      // Tick->Here / Ease / segment speed / speed mode / timing in the DOCK -- this used to be
      // `!hosted`, i.e. hidden in the one place the original design drew it, so none of those controls
      // was reachable while the timeline was docked.
      keyFooter.visible = true;
      // ...and the original design had no title row: the ribbon's meta already says "N keyframes", the
      // workspace header carries the MOUSE chip, and Clear All lives on the SELECTED CAMERA card.
      // Docked, this row was three restatements competing with the rows that own them.
      header.visible = !hosted;
      mouseLbl.text = forcedCur ? 'Mouse: UI  ·  editor cursor forced' : (cur ? 'Mouse: UI' : 'Mouse: GAME');
      mouseLbl.style.color = cur ? S.accent : S.label;
      // ONE SPEED, THREE BARS. st.timescale is the engine's demo_timescale, pushed by C++
      // (Movie/DemoTimescale.h). This used to read ctx.GetDemoControllerState().fTimeScale, which
      // simply does not exist on this HUD context panel -- verified over netcon:
      // "GetDemoControllerState is not a function" -- so the guard swallowed it every frame and
      // this bar never followed a speed set anywhere else.
      if (typeof st.timescale === 'number') speedScrub.setSpeed(st.timescale);
      // The scrubber has no mouse-move event to work from (nothing in this HUD context does), so it
      // is driven from the raw cursor the C++ side publishes on our state, once per frame.
      speedScrub.pump(st.mx || 0, st.my || 0, !!st.lmb);

      var freeze = (st.timing === 'Freeze');
      tl.visible = true;
      hTitle.text = 'CAMERA TIMELINE';
      // Deliberately SHORT. This used to read "tick 41230 · 6 keys · sel #2 · seg 3 · Linear",
      // which repeated the tick the transport readout already shows a row below it, and spent the
      // header on two fields (segment, interp) the curve editor owns. What is left is the only
      // thing the header can say that nothing else does: how much is on this timeline, and which
      // one you have hold of.
      hInfo.text = st.count + (st.count === 1 ? ' key' : ' keys')
        + (st.selected >= 0 ? ('  ·  #' + (st.selected + 1) + ' selected') : '');
      if (!clearConfirm) { clearBtn.__lbl.text = 'Clear'; clearBtn.__lbl.style.color = S.value; }

      var t0 = st.tickMin, t1 = st.tickMax; if (t1 <= t0) t1 = t0 + 1;
      scrubT0 = t0; scrubT1 = t1;
      var shownTick = activeTick();
      var headFrac = clamp01((shownTick - t0) / (t1 - t0));
      if (!scrub.mousedown) {
        scrubSyncing = true;
        scrub.value = headFrac; // normalized; don't fight a drag
        scrubSyncing = false;
      }
      // The rail's fill and the playhead follow the SAME fraction the Slider does -- including
      // mid-drag, when scrub.value is the user's and shownTick is already the scrubbed tick. Two
      // sources here would make the head lag the bar it is drawn on.
      var headPx = Math.round(headFrac * TLW);
      if (headPx !== lastHeadPx) {
        lastHeadPx = headPx;
        railFill.style.width = headPx + 'px';
        headLine.style.position = headPx + 'px 0px 0px';
      }
      // Was: 'Place 2+ camera markers (K or + Add), then drag to scrub'. That is wider than its
      // slot, so it wrapped to a second line and made the whole transport row taller -- then the
      // row snapped back the moment a second marker existed. A row that changes height from its
      // own text jitters while you work, so this stays on one line.
      if (st.count < 2 && st.uiEditorInspector !== 'scene') tReadout.text = 'Add 2 camera keys (K)';
      else if (!scrub.mousedown) tReadout.text = 'tick ' + shownTick + '   time ' + (st.time != null ? st.time.toFixed(2) : '?') + 's';
      syncTransportButton();
      // Segmented Linear|Smooth reflects the path's EFFECTIVE mode, so a change made in the graph
      // shows here the same frame -- and a Smooth that the path cannot actually run says so
      // rather than sitting lit over linear motion.
      btnSel(interpLin, st.interp === 'Linear');
      btnSel(interpSmooth, st.interp !== 'Linear');
      interpSmooth.__lbl.text = '∿ Smooth';
      interpSmooth.__lbl.style.color = st.interpDegraded ? S.hint
        : (interpSmooth.__sel ? S.accent : S.value);
	// The shake set is part of the signature: the diamonds carry the "has a shake" dot, so adding
	// or clearing one has to relayout them, not just repaint the menu.
	var shakeSig = (st.shakes || []).map(function (z) { return z.i + '.' + z.s; }).join(',');
	var sceneEvSig = (st.sceneKeys && st.sceneKeys.events)
	  ? st.sceneKeys.events.map(function (e) { return e.tick + '.' + ((e.fields && e.fields.length) || 0); }).join(',')
	  : '';
	var sceneSel = (st.sceneKeys && st.sceneKeys.selectedEvent != null) ? st.sceneKeys.selectedEvent : -1;
	var sig = st.tickMin + ':' + st.tickMax + ':' + st.selected + ':'
	  + (st.markers ? st.markers.map(function (m) { return m.tick; }).join(',') : '')
	  + ':' + shakeSig + ':' + (st.uiEditorInspector || '') + ':' + sceneEvSig + ':' + sceneSel;
	if (sig !== lastTlSig) { lastTlSig = sig; rebuildTimelineDiamonds(); }
	var rSig = (st.renderInTick||0) + ':' + (st.renderOutTickExclusive||0) + ':' + (st.tickMin||0) + ':' + (st.tickMax||0);
	if (rSig !== lastRenderRangeSig) { lastRenderRangeSig = rSig; rebuildRenderRangeHandles(); }

      updateKeys(freeze);
      renderShakeMenu(); // keeps the open menu in step with what the engine actually stored
    };

    // Pushed once by CameraTimelineHud from the C++ RageShake style table, so the index this
    // menu sends back is always the index the engine means -- no duplicated name list to drift.
    api.setShakeStyles = function (menu, rseCount, labels) { setShakeStylesImpl(menu, rseCount, labels); };

)TLJS"
// Split: MSVC caps one raw literal at ~16380 bytes (C2026); adjacent literals concatenate.
R"TLJS(
    // C++ pushes wheel notches here while the keyframe menu owns the cursor. Returns nothing;
    // the router already decided this surface gets them.
    // Open the keyframe menu on key `i` with its style list already down. There is no way to
    // right-click from automation, and this interaction (menu open -> list open -> wheel) is
    // exactly the one that was broken, so it needs to be drivable:
    //   mirv_filmmaker camtl eval $.CamTimeline.keyMenu(0)
    api.keyMenu = function (i, listOpen) {
      var idx = parseInt(i, 10);
      if (!isFinite(idx)) idx = 0;
      if (!st || !st.markers || idx < 0 || idx >= st.markers.length) return 'no such key';
      openShakeMenu(idx, 200);
      if (listOpen === undefined || String(listOpen) !== '0') {
        shakeListOpen = true;
        renderShakeMenu();
      }
      return 'menu on key ' + (idx + 1);
    };
    // THE COLUMN IS BEING DRAGGED RIGHT NOW, at this width. Called straight from CONFIG's and
    // the CAMERA EDITOR's grip -- every panel here shares one Panorama script context, so this is
    // a direct call rather than a console command and a frame of C++ round trip. That is what
    // makes the docked timeline / camera / graph follow the edge DURING the drag instead of
    // jumping to the new width on release.
    api.setInspW = function (w) {
      var n = parseInt(w, 10);
      dockInspWOverride = (isFinite(n) && n > 0) ? n : 0;
    };
    api.wheel = function (notches) {
      var n = parseInt(notches, 10);
      if (!isFinite(n) || n === 0) return;
      shakeMenuWheel(n);
    };
    api.dbgRange = function () { return dbgRangeState(); };
    //   mirv_filmmaker camtl eval $.Msg($.CamTimeline.dbgSpeed()+String.fromCharCode(10));
    api.dbgSpeed = function () { return speedScrub.dbg(); };
    // mirv_filmmaker camtl eval $.CamTimeline.trace(1);   (0 to stop; see traceState above)
    api.trace = function (on) { traceOn = !!on; traceLast = ''; return traceOn ? 'trace on' : 'trace off'; };
    // WHAT IS IN THE TRANSPORT ROW AND HOW WIDE IT WANTS TO BE:
    //   mirv_filmmaker camtl eval $.Msg($.CamTimeline.dbgBar()+String.fromCharCode(10));
    // `need` is the natural extent fitControlRow() measures; `avail` is the docked width it has to
    // live in; `k` is the scale that made it fit. need > avail with k == 1 means the fit is off.
    api.dbgBar = function () {
      var row = findNative('ControlRow');
      if (!row) return '{\"row\":null}';
      var sx = row.actualuiscale_x || ctx.actualuiscale_x || 1;
      var kids = [];
      for (var i = 0; i < row.GetChildCount(); i++) {
        var c = row.GetChild(i);
        kids.push((c.id || '?') + ':' + Math.round((c.actuallayoutwidth || 0) / sx)
          + '@' + Math.round(fitSaneEdge(c.actualxoffset) / sx) + (c.visible ? '' : '!'));
      }
      return JSON.stringify({
        rowW: Math.round((row.actuallayoutwidth || 0) / sx),
        need: Math.round(fitRowW),
        avail: Math.round((nativeContents ? nativeContents.actuallayoutwidth : 0) / sx),
        k: Number(fitK.toFixed(3)), uis: Number(sx.toFixed(3)), kids: kids
      });
    };
    // Everything a resolution/aspect change has to move, in one read:
    //   mirv_filmmaker camtl eval $.Msg($.CamTimeline.dbgLayout()+String.fromCharCode(10));
    // Run it before and after the switch -- whichever number fails to move is the stale one.
    api.dbgLayout = function () {
      function box(p) {
        if (!p) return null;
        var s = p.actualuiscale_x || 1;
        return { dev: Math.round(p.actuallayoutwidth || 0), uis: Number(s.toFixed(4)),
                 virt: Math.round((p.actuallayoutwidth || 0) / s),
                 styleW: (p.style && p.style.width) ? p.style.width : '-' };
      }
      var out = {
        ctx: box(ctx), virt: [lastVirtW, lastVirtH],
        lastContentW: lastContentW, W: W, TLW: TLW,
        panelStyleW: panel.style.width,
        contents: box(nativeContents), nativeRoot: box(nativeRoot),
        markersDocked: markersDocked, markerCaptureHold: markerCaptureHold,
        basis: {}, live: {}
      };
      var ids = ['RoundMarkers', 'HighlightMarkers', 'HighlightIcons'];
      for (var i = 0; i < ids.length; i++) {
        var id = ids[i], p = findNative(id), b = markerBasis[id];
        out.live[id] = box(p);
        out.basis[id] = b ? { wFrac: Number(b.wFrac.toFixed(4)), n: b.items.length,
                              lastFrac: Number(b.items[b.items.length - 1].l.toFixed(4)) } : null;
        if (p && p.GetChildCount && p.GetChildCount() > 0) {
          var last = p.GetChild(p.GetChildCount() - 1);
          out.live[id].lastChildX = Math.round(last.actualxoffset || 0);
          out.live[id].lastChildFrac = Number(((last.actualxoffset || 0) / (p.actuallayoutwidth || 1)).toFixed(4));
        }
      }
      var strip = ctx.FindChildTraverse && ctx.FindChildTraverse('MirvSpeedRow');
      out.speedRow = box(strip);
      if (strip && strip.GetChildCount && strip.GetChildCount() > 0) out.speedStrip = box(strip.GetChild(0));
      return JSON.stringify(out);
    };
    $.CamTimeline = api;
    api.render();
    patchNativeBar(); // inject immediately at build, even before the first state push
    $.Msg('[camtl] timeline panel built.\n');
  } catch (err) {
    $.Msg('[camtl] gui error: ' + err + '\n');
  }
})();
)TLJS";

} // namespace Filmmaker
