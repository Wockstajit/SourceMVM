#pragma once

// Panorama JS for the CONFIG panel: a lightweight clone of the Camera Editor workspace
// (CameraEditorJs.h) built ONCE into the in-game HUD (CSGOHud) context by ConfigHud.cpp.
//
// Deliberately a SEPARATE, self-contained script: it renders only a right-side settings
// sidebar over the normal game view. It does not own a preview, timeline, bottom band, or
// viewport geometry; the dedicated Camera Editor remains the only full workspace.
//
// Accent color is the theme's existing BLUE (the Camera Editor's "freeze" token) instead of
// the editor's gold, so the two workspaces read as siblings without being confusable.
//
// LAYOUT: the inspector is paged into four TABS -- CAMERA / ENVIRONMENT / VFX / RAGDOLLS -- each built
// by its own fragment file, with collapsible cards inside. It used to be one unbroken scroll
// column; it stopped fitting once the camera-shake layers landed. Same paging recipe the
// Camera Editor uses (CameraEditorJs.h): a `visible` toggle over per-tab container panels.
//
// This file owns only the shell: theme, chrome, tab strip, and render dispatcher. Everything
// with a control in it lives in a fragment:
//
//   ConfigHudWidgetsJs.h      builders: mk/lbl/btn/row/section/sliderRow/togglePill/cycleRow
//   ConfigHudCameraTabJs.h    MODIFIERS + HANDHELD + STEP BOB + RECOIL
//   ConfigHudEffectsTabJs.h   VFX page (particle-effect toggles)
//   ConfigHudLightingTabJs.h    ENVIRONMENT > LIGHTING
//   ConfigHudAtmosphereTabJs.h  ENVIRONMENT > ATMOSPHERE
//   ConfigHudWeatherSkyTabJs.h  ENVIRONMENT > WEATHER & SKY
//   ConfigHudEffectsResetJs.h   EFFECTS footer, shared by all four: keep-between-demos + RESET
//   ConfigHudRagdollsTabJs.h  advanced ragdolls (empty) / death weapon / physics / method acting
//
// Each fragment appends its own closure to `renderHooks`, so render() below never has to know
// what any tab contains. Splitting the fragments out also keeps every raw string literal well
// clear of MSVC's ~16380-byte cap, which the old single chunk was within a kilobyte of.
//
//   C++ -> JS : attribute "state" (see ConfigHud::BuildStateJson), then $.ConfigHud.render().
//   JS  -> C++: buttons issue "mirv_filmmaker ..." console commands / cvar writes.

namespace Filmmaker {

inline const char* kConfigHudJs = R"CFJS(
(function () {
  try {
    var existing = $('#ConfigHudRoot'); if (existing) existing.DeleteAsync(0);
    // Top-level child of ctx (see EditorSettingsPopupJs.h), so a rebuild has to drop the old
    // one explicitly or two cards stack on the next open.
    var existingSettings = $('#ConfigEditorSettingsRoot'); if (existingSettings) existingSettings.DeleteAsync(0);
    var ctx = $.GetContextPanel();

    // Same theme tokens as CameraEditorJs.h, with the panel accent flipped to the theme's
    // blue (S.freeze there) so Config is visually distinct from the gold Camera Editor.
    var S = {
      accent: '#80bdffff', danger: '#c92a2acc',
      bg: '#0c0e12ff', bgSoft: '#14181eff',
      sectionBg: '#1a1f26ff', cardBorder: '#ffffff10',
      cardShadow: '#00000066 0px 3px 12px 0px',
      // TEXT TIERS (Docs/panorama/panorama_ui_guide.md): `value` is the thing you came to read, `label` the row names,
      // `hint` static supporting text, `muted` a card's live summary and an inactive tab, `dim`
      // DISABLED CONTROLS ONLY. #6b7480 is a border/rule colour and the disabled tier -- it is
      // never body text, because it fails contrast at these sizes.
      label: '#c8d3dfff', value: '#eef2f6ff', dim: '#6b7480ff',
      // hint #bcc6d2 = APCA Lc 71 on #0c0e12, muted #afb9c6 = Lc 64: both clear Lc 60 for the
      // 11-12px text they set (the original design's #afb9c6 / #8b95a3 measured 64 / 44).
      hint: '#bcc6d2ff', muted: '#afb9c6ff',
      // Card grounds: #131820 shut, #161b21 open, so an open card lifts off the column.
      cardBg: '#131820ff', cardBgOpen: '#161b21ff', cardBorderOpen: '#ffffff1f',
      // A SHUT card's hover ground. Exactly what the translucent row fill used to
      // composite to over cardBg (#ffffff0e over #131820), so the hover looks the same --
      // it just covers the whole card now instead of an inset rectangle.
      cardBgHover: '#20252cff',
      // Tiles (value + switch) and their hover/press edges.
      tileBg: '#1b2129ff', tileHover: '#1e242dff',
      tileBorder: '#ffffff14', tileBorderHover: '#ffffff40',
      trackBg: '#ffffff14',
      btnBg: '#ffffff0d', btnOn: '#80bdff2e',
      btnBorder: '#ffffff1f', btnBorderHover: '#ffffff3d',
      // Interaction tiers. Panorama inline styles have no :hover in this in-game HUD context and
      // there is no stylesheet of ours to hang classes on, so hover/press are driven from panel
      // events through paintBtn() in ConfigHudWidgetsJs.h. These are the fills it picks.
      btnHover: '#ffffff1f', btnDown: '#ffffff26', btnOnHover: '#80bdff45',
      tabHoverEdge: '#80bdff66',
      rowHover: '#212833ff',   // a one-line value row under the pointer
      // The effect rail (Docs/features/filmmaker_config_panel.md): selected row ground,
      // its hover, group headings, and the inset ground of drawers and the colour picker.
      railSel: '#1a2029ff', railHover: '#1b2027ff', group: '#afb9c6ff', drawer: '#11151bff',
      edgeOff: '#2a3037ff',
      clear: 'rgba(0,0,0,0)', // NEVER style.X = '' (aborts the script)
      font: 'Stratum2, "Arial Unicode MS"',
      // Monospaced-DIGIT cut of the same family, shipped by CS2 and used by its own stylesheets
      // for scoreboard/timer figures. Any readout whose NUMBER changes while you watch it uses
      // it, so a centred value does not shift as its digits change width.
      fontMonodigit: 'Stratum2 Bold Monodigit, Stratum2, "Arial Unicode MS"'
    };
    // Keep these in sync with CameraEditorJs.h so the two workspaces line up 1:1. The column is
    // 520 (Docs/panorama/panorama_ui_guide.md, and UI.col.w in PanoramaUiMetricsJs.h): the old 430 could not hold two
    // 213px value tiles side by side. Spelled out rather than read from UI, because this literal
    // runs BEFORE that fragment is spliced in and `UI` is still undefined here.
    // INSPECTOR_W is the operator's dragged column width, not a constant: the grip below writes
    // it and C++ hands it back in st.inspW (InspectorWidth.h) so it survives a rebuild and a
    // restart. Everything that lays out against the column reads it live; the handful of style
    // writes that baked it at build time are re-applied by cfgApplyInspectorW().
    var INSPECTOR_W = 640, BOTTOM_LIFT = 28;
    var TAB_BAR_H = 42, TAB_GAP = 4, NATIVE_BAR_H = 96;
    var _lastInvariantSig = ''; // rate-limit layout-invariant warnings

    function cmd(c) {
      try { GameInterfaceAPI.ConsoleCommand(c); }
      catch (e) { $.Msg('[confighud] cmd failed: ' + e + '\n'); }
    }
    function rdInt(name) {
      try { var v = parseInt(GameInterfaceAPI.GetSettingString(name)); return isFinite(v) ? v : 0; }
      catch (e) { return 0; }
    }
    function finitePositive(v) { return typeof v === 'number' && isFinite(v) && v > 1; }
)CFJS"
#include "PanoramaUiMetricsJs.h"
#include "InspectorLayoutJs.h"
#include "ConfigHudWidgetsJs.h"
#include "ConfigColorPickerJs.h"
#include "WorkspaceShellJs.h"
#include "WorkspaceChromeJs.h"
#include "TimelineDockRibbonJs.h"
#include "EditorSettingsPopupJs.h"
#include "InspectorResizeJs.h"
R"CFJS(
    // ---- root: full-screen, non-hittest. Only the right sidebar is interactive. ----
    var root = $.CreatePanel('Panel', ctx, 'ConfigHudRoot', {});
    root.hittest = false; root.style.width = '100%'; root.style.height = '100%';
    root.style.zIndex = '52'; // just under CamEditorRoot (53) -- mutually exclusive anyway

    // Demo-only workspace chrome. In a live mvm_test game these remain hidden and Config is the
    // existing full-size sidebar overlay. During demo playback they frame a true scaled preview.
    // Preview catcher FRAME: four panels ringing the preview rect (top/bottom/left/right).
    // Empty-space clicks land on the frame and are swallowed -- they must NOT switch spectator
    // targets. Clicks INSIDE the preview hit nothing of ours and pass through to the game,
    // where CS2's native LMB/RMB spectator switching works.
    // THE WORKSPACE SHELL -- catchers, PREVIEW tag, letterbox masks, frame and decor come from
    // the one shared builder (WorkspaceShellJs.h); this panel supplies its accent and wordmark.
    var shell = mvmWorkspaceShell(root, {
      accent: S.accent, title: 'CONFIG / SYSTEM', code: 'SOURCE // MVM FILMMAKER',
      onCatcher: function () { /* swallow */ }
    });
    var catcherTop = shell.catchers.top, catcherBottom = shell.catchers.bottom;
    var catcherLeft = shell.catchers.left, catcherRight = shell.catchers.right;
    var tag = shell.tag, tagLbl = shell.tagLbl;

    var backdrop = shell.backdrop;

    // THE SHARED DOCK RIBBON. Three real tabs, not a label: the dock's mode moved to
    // Movie/TimelineDock.h, so this panel can switch REGULAR TIMELINE / CAMERA / GRAPH exactly
    // like the Camera Editor does. One definition for both -- see TimelineDockRibbonJs.h.
    var dockRibbon = mvmTimelineDockRibbon(root, S.accent, function () { settingsPopup.toggle(); });
    var timelineBand = dockRibbon.root;
    timelineBand.visible = false;
    timelineBand.id = 'CfgTlBand';
    var settingsBtn = dockRibbon.gear;

    // ---- EDITOR SETTINGS -------------------------------------------------------------------
    // The SAME fly-out the Camera Editor's gear opens -- one definition, in
    // EditorSettingsPopupJs.h. It lives at the RIGHT END OF THIS BAND, opposite 'REGULAR
    // TIMELINE', because that is where the Camera Editor keeps its own gear: the two workspaces'
    // bottom bars have to read as the same bar. (It started on the inspector's tab strip, which
    // put demo-playback settings up in the CAMERA/EFFECTS/RAGDOLLS row where they do not belong.)
    // The band is demo-only, and so are these settings, so the gear correctly goes away with it.
    // The gear is the ribbon's own (`dockRibbon.gear`, assigned to settingsBtn above). A second
    // hand-built gear used to be appended to the band here; it survived the ribbon swap and sat
    // clipped beside the real one as a stray half-glyph.
    var settingsPopup = mvmEditorSettingsPopup(ctx, {
      id: 'ConfigEditorSettingsRoot',        // deleted by ConfigHud's teardown script, by id
      publish: root,
      // Just above this band and left of the inspector -- the band's own right edge is the
      // inspector's left edge, so the card rises directly off the gear that opened it.
      marginRight: INSPECTOR_W + 16,
      marginBottom: NATIVE_BAR_H + TAB_GAP + TAB_BAR_H + 16,
      accent: S.accent,                      // Config's accent already IS the theme blue
      debugTag: 'cfgsettings',
      onOpen: function () { settingsBtn.style.backgroundColor = S.accent.substring(0, 7) + '33'; },
      onClose: function () { settingsBtn.style.backgroundColor = '#ffffff14'; }
    });

    var barLeft = shell.masks.left, barRight = shell.masks.right;
    var barTop = shell.masks.top, barBottom = shell.masks.bottom;
    var frameL = shell.frames.left, frameR = shell.frames.right;
    var frameT = shell.frames.top, frameB = shell.frames.bottom;
    var workspaceDecor = shell.decor;

)CFJS"
// SPLIT HERE for MSVC's ~16380-byte cap on a single string literal, not for any semantic reason:
// this chunk had ~900 bytes of headroom left, which the next edit would have spent (the failure
// mode is error C2026, "string too big, trailing characters truncated"). Adjacent raw literals
// concatenate, so the seam is invisible to the script.
R"CFJS(
    function hideDemoWorkspace() {
      shell.hide();
      timelineBand.visible = false;
      root.SetAttributeString('previewrect', '');
    }

    function layoutDemoWorkspace(cursorOn) {
      var sx = root.actualuiscale_x || ctx.actualuiscale_x || 1;
      var sy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var rawW = root.actuallayoutwidth || 0, rawH = root.actuallayoutheight || 0;
      if (rawW < 16) { rawW = ctx.actuallayoutwidth || 0; rawH = ctx.actuallayoutheight || 0; }
      var rw = rawW / sx, rh = rawH / sy;
      if (!finitePositive(rw) || !finitePositive(rh)) { hideDemoWorkspace(); return; }

      // Which editor is docked decides how tall the bottom is. Measured, not assumed -- the
      // same shared measurement the Camera Editor uses (mvmDockMeasure), so the preview stops
      // above the CAMERA / GRAPH overlays exactly as it stops above the native bar.
      var dm = mvmDockMeasure(ctx, rh, sy, st && st.dockTab, { floor: BOTTOM_LIFT });
      var nativeH = dm.editorTop, nativeBarVisible = dm.nativeBottom;
      var bottomH = dm.bottomH;
      var bandY = dm.tabY;
      var areaW = rw - INSPECTOR_W, areaH = rh - bottomH;
      if (!finitePositive(areaW) || !finitePositive(areaH)) { hideDemoWorkspace(); return; }

      var aspect = rw / rh, pw = areaW, ph = pw / aspect;
      if (ph > areaH) { ph = areaH; pw = ph * aspect; }
      pw = Math.floor(pw); ph = Math.floor(ph);
      if (!finitePositive(pw) || !finitePositive(ph)) { hideDemoWorkspace(); return; }
      var ox = Math.max(0, Math.floor((areaW - pw) / 2));
      var oy = Math.max(0, Math.floor((areaH - ph) / 2));
      var right = ox + pw, bottom = oy + ph;

      // Catcher frame: ring the preview so empty-space clicks are swallowed (no spectator
      // switch). The preview itself stays bare so LMB/RMB there reach the game and cycle
      // spectator targets natively. The bottom panel stops at the native demo bar's top edge
      // (when it is visible) so the bar keeps its own clicks.
      var cbBottom = nativeBarVisible ? Math.max(0, rh - nativeH) : rh;
      shell.layout({ rw: rw, rh: rh, areaX0: 0, areaW: areaW, areaH: areaH,
                     ox: ox, oy: oy, pw: pw, ph: ph, catcherBottom: cbBottom, cursorOn: cursorOn });
      backdrop.visible = true; backdrop.style.position = '0px ' + Math.floor(rh - bottomH) + 'px 0px';
      backdrop.style.width = Math.floor(areaW) + 'px'; backdrop.style.height = Math.floor(bottomH - nativeH) + 'px';
      timelineBand.visible = true; timelineBand.style.position = '0px ' + Math.floor(bandY) + 'px 0px';
      // Which dock tab is live is SHARED state (Movie/TimelineDock.h), so it is read back
      // from C++ rather than remembered here -- otherwise this ribbon and the editor's would
      // disagree the moment either one was clicked.
      // Same meta the editor's ribbon shows, from the same field, so the one dock never says
      // two different things depending on which workspace is open.
      dockRibbon.sync(st.dockTab, st.dockTab === 'timeline' ? mvmTimelineMeta() : '');
      timelineBand.style.width = Math.floor(areaW) + 'px';


      // ---- INVARIANTS: prevent future layout regressions ----
      (function checkLayoutInvariants() {
        var bad = [];
        if (right <= ox + 1) bad.push('preview width degenerate: right=' + right + ' ox=' + ox);
        if (bottom <= oy + 1) bad.push('preview height degenerate: bottom=' + bottom + ' oy=' + oy);
        if (Math.abs(right - ox - pw) > 1) bad.push('pw mismatch: pw=' + pw + ' computed=' + (right - ox));
        if (Math.abs(bottom - oy - ph) > 1) bad.push('ph mismatch: ph=' + ph + ' computed=' + (bottom - oy));
        if (bottom > areaH + 1) bad.push('bottom exceeds areaH: bottom=' + bottom + ' areaH=' + areaH);
        if (right > areaW + 1) bad.push('right exceeds areaW: right=' + right + ' areaW=' + areaW);
        // Config never has a render workspace — side bars must always equal areaH.
        // Verify the side bar height matches areaH (the comment says areaH + 'px', but
        // verify the actual computed style value to catch accidental overrides).
        if (ox > 1 && barLeft.style.height !== areaH + 'px')
          bad.push('barLeft height mismatch: ' + barLeft.style.height + ' expected ' + areaH + 'px');
        if (areaW - right > 1 && barRight.style.height !== areaH + 'px')
          bad.push('barRight height mismatch: ' + barRight.style.height + ' expected ' + areaH + 'px');
        var fx0 = ox / rw, fy0 = oy / rh, fx1 = right / rw, fy1 = bottom / rh;
        if (fx0 < -0.001 || fx0 > 1.001) bad.push('x0 fraction out of range: ' + fx0.toFixed(4));
        if (fy0 < -0.001 || fy0 > 1.001) bad.push('y0 fraction out of range: ' + fy0.toFixed(4));
        if (fx1 < -0.001 || fx1 > 1.001) bad.push('x1 fraction out of range: ' + fx1.toFixed(4));
        if (fy1 < -0.001 || fy1 > 1.001) bad.push('y1 fraction out of range: ' + fy1.toFixed(4));
        if (fx1 - fx0 <= 0.001 || fy1 - fy0 <= 0.001)
          bad.push('preview fraction span too small: dx=' + (fx1 - fx0).toFixed(4) + ' dy=' + (fy1 - fy0).toFixed(4));
        if (bad.length) {
          var sig = bad.join('|');
          if (sig !== _lastInvariantSig) {
            _lastInvariantSig = sig;
            for (var bi = 0; bi < bad.length; bi++)
              $.Msg('[confighud] INVARIANT BROKEN: ' + bad[bi] + '\n');
          }
        } else { _lastInvariantSig = ''; }
      })();

      root.SetAttributeString('previewrect',
        (ox / rw).toFixed(5) + ' ' + (oy / rh).toFixed(5) + ' ' +
        (right / rw).toFixed(5) + ' ' + (bottom / rh).toFixed(5));
    }
)CFJS"
R"CFJS(
    // Bottom clearance for the floating RENDER / CUSTOMIZE PLAYER row (created near the end of
    // this file, parented to `root`, so it does NOT scroll with the column).
    //
    // The viewport is SHORTENED by this much -- it is deliberately not padding. padding-bottom
    // only extends the scrollable range; it never moves content up, so whenever a tab's content
    // already fits (measured: Effects = 1151px in a 1200px viewport) the last row still rendered
    // underneath the buttons. Shortening the viewport instead means nothing can ever draw under
    // the buttons, and a tab whose content really does run past the readable area scrolls by
    // exactly the amount needed to bring its last control clear (see cfgApplyScroll).
    var ACTION_CLEAR = UI.action.h + UI.action.inset * 2;
    var FADE_H = 46;
    // A little breathing room past the point where the last control has cleared the fade, so it
    // does not end up sitting exactly on the fade's edge. The fade's own height is NOT included
    // here -- cfgApplyScroll already subtracts it from the readable viewport.
    var CFG_OVERSCROLL = 18;

    // The column's ground: the Render workspace's background (WorkspaceShellJs.h mvmColumnGround),
    // tinted blue. Root-level, full height, built BEFORE the inspector so it paints under it; the
    // inspector and the footer band behind RENDER / CUSTOMIZE PLAYER are transparent over it.
    var columnGround = mvmColumnGround(root, S.accent, INSPECTOR_W);
    // Right inspector column, same chrome as the Camera Editor's.
    var inspector = mk('Panel', root); inspector.hittest = false;
    inspector.style.horizontalAlign = 'right'; inspector.style.verticalAlign = 'top';
    inspector.style.width = INSPECTOR_W + 'px'; inspector.style.height = '100%';
    inspector.style.backgroundColor = S.clear; inspector.style.borderLeft = '1px solid ' + S.clear;
    inspector.style.flowChildren = 'down';
    inspector.style.paddingTop = UI.col.pad + 'px'; inspector.style.paddingBottom = '10px';
    inspector.style.paddingLeft = UI.col.pad + 'px'; inspector.style.paddingRight = UI.col.pad + 'px';
    inspector.style.fontFamily = S.font;
    // CLIP, not native scroll. Panorama never receives the mouse wheel in this in-game HUD
    // context -- MovieMode::OnMouseWheel consumes it so it cannot reach the demo spectator
    // behind the panel -- so `overflow: scroll` renders a scrollbar that nothing can drive.
    // The column is scrolled manually instead: C++ forwards each captured notch to api.wheel()
    // and cfgApplyScroll() translates `scrollWrap`. Same approach, and the same reason, as the
    // Customize modal's control column (CameraEditorCustomizeJs.h custWheel).
    // CLIP, NEVER SQUISH. `squish` lets Panorama SHRINK flow children when the column's content
    // overflows it, and a real mouse-wheel event runs through Panorama's own layout as well as
    // ours: on that pass the scrolled subtree still counts its full height, the column overflows,
    // and the pinned chrome gets squished -- Labels have no explicit height, so they go to zero
    // and the header chips and tab captions vanish while their bordered boxes survive. That is
    // the "text disappears when I scroll" bug. Nothing here ever needs squishing: scrollView is
    // fill-parent-flow and takes exactly what is left, so plain clip is always correct.
    inspector.style.overflow = 'clip';

    // The panel chrome (title + Exit, the mouse/HUD row, the tab strip) is PINNED: it is created
    // as a direct child of the inspector below, outside the scroll region. Scrolling it away was
    // wrong -- you lost the tab strip and the Exit button the moment you moved down a page.
    // Only `scrollWrap` (the tab bodies) moves; both are declared here because the scroll
    // helpers below are defined before the panels exist (var hoisting), and are only ever
    // CALLED after the build has finished.
    var scrollView, scrollWrap;

    // Real pixel height = root height - the action row's footprint. Recomputed from render()
    // so a resolution / uiscale change re-fits it; the cached compare keeps it to one style
    // write per actual change instead of one per frame.
    var _lastViewH = -1;
    function syncInspectorHeight() {
      var vsy = root.actualuiscale_y || ctx.actualuiscale_y || 1;
      var vrh = (root.actuallayoutheight || 0) / vsy;
      if (!finitePositive(vrh)) return;
      var vh = Math.max(120, Math.round(vrh - ACTION_CLEAR));
      if (vh === _lastViewH) return;
      _lastViewH = vh;
      inspector.style.height = vh + 'px';
    }

    // ---- live-match mouse capture --------------------------------------------------------
    // THE LEVER THAT MAKES BUTTONS AND TABS CLICKABLE ON A LIVE MAP.
    //
    // Releasing player aim (the CCSGOInput::MouseInputEnabled detour in main.cpp) and suspending
    // MirvInput are both necessary but NOT sufficient: the game still has to be told that a
    // Panorama panel owns the mouse. Without it the cursor moves and our own OS-cursor pipe still
    // drives the sliders -- which is exactly why sliders worked while every real button and tab
    // was dead -- but Panorama itself never receives the click.
    //
    // This is the same API CS2's own UI uses, so no hooks and no vtable indices are involved:
    //   scoreboard.js : AddDenyInputFlagsToGame(contextMenu, 'ScoreboardPlayercard', 'CaptureMouse')
    //   mapdraft.js   : AddDenyInputFlagsToGame(panel, 'MapDraft', 'ShareMouse')
    // It is also what the user found by hand -- Tab (scoreboard) then right-click is the native
    // path into this same state, after which every control in our panel became clickable.
    //
    // 'ShareMouse' rather than 'CaptureMouse': this is a sidebar over live gameplay, like the map
    // draft, not a modal context menu. The game keeps its input, we just stop being locked out.
    var cfgDenyInput = null;
    var cfgCaptureReported = false; // one-shot: say once whether the API is reachable here
    function setLiveMouseCapture(want) {
      try {
        if (want && !cfgDenyInput) {
          cfgDenyInput = UiToolkitAPI.AddDenyInputFlagsToGame(inspector, 'MvmConfigLive', 'ShareMouse');
          if (!cfgCaptureReported) {
            cfgCaptureReported = true;
            $.Msg('[confighud] live mouse capture ACQUIRED handle=' + cfgDenyInput + '\n');
          }
        } else if (!want && cfgDenyInput) {
          UiToolkitAPI.ReleaseDenyInputFlagsToGame(cfgDenyInput);
          cfgDenyInput = null;
        }
      } catch (eCap) {
        // Never let a missing/renamed API take the whole render pass down with it -- the panel
        // still draws and the demo workspace, which does not need this at all, is unaffected.
        cfgDenyInput = null;
        if (!cfgCaptureReported) {
          cfgCaptureReported = true;
          $.Msg('[confighud] live mouse capture UNAVAILABLE: ' + eCap + '\n');
        }
      }
    }

    // ---- manual scrolling ---------------------------------------------------------------
    // Driven by api.wheel(), which C++ calls with the notches it captured (see ConfigHud.cpp
    // PushWheel/DispatchWheel). cfgScrollY is in CSS px, 0 = top.
    var cfgScrollY = 0, cfgAppliedScrollY = -1;
    function cfgViewH() {
      var vsy = inspector.actualuiscale_y || 1;
      return ((scrollView && scrollView.actuallayoutheight) || 0) / vsy;
    }
    // Where the scroll region begins, in root-space css px (for placing the scrollbar thumb).
    function cfgScrollTop() {
      try {
        var vsy = root.actualuiscale_y || 1;
        var p = scrollView.GetPositionWithinWindow();
        return p && isFinite(p.y) ? p.y / vsy : 0;
      } catch (e) { return 0; }
    }
    function cfgContentH() {
      var vsy = inspector.actualuiscale_y || 1;
      return ((scrollWrap && scrollWrap.actuallayoutheight) || 0) / vsy;
    }
    function cfgApplyScroll() {
      if (!scrollWrap || !scrollView) return;
      var viewH = cfgViewH(), contH = cfgContentH();
      // WHAT YOU CAN ACTUALLY READ is the viewport MINUS the bottom fade -- content under the
      // fade has already dissolved into the footer. So that, not the raw viewport, is what the
      // content has to clear, and it is measured against `usableH` below.
      //
      // maxY IS ZERO WHEN THE CONTENT ALREADY FITS. It used to be `contH - viewH + CFG_OVERSCROLL`
      // unconditionally, which left ~64px of travel on a tab whose content fits entirely -- and
      // the EFFECTS sub-tabs fit easily with their cards collapsed, WEATHER & SKY being the one
      // you meet first. One notch there scrolled the whole page up under the PINNED chrome, so
      // the EFFECTS sub-tab strip and the first card's buttons slid out of the clip region:
      // they vanished, stopped hit-testing, and only came back by scrolling all the way up.
      // A page that fits must not scroll at all.
      var usableH = Math.max(0, viewH - FADE_H);
      var maxY = contH > usableH ? (contH - usableH + CFG_OVERSCROLL) : 0;
      if (cfgScrollY > maxY) cfgScrollY = maxY;
      if (cfgScrollY < 0) cfgScrollY = 0;
      // marginTop moves the column. `position` and `transform` were both measured against it
      // and behave identically -- none of the three has anything to do with the "text
      // disappears when I scroll" bug, which was the pinned chrome sharing a compositing
      // context with this clipped column (see the zIndex note on `head`). Guarded on a real
      // change because render() runs every frame and a repeated style write still costs a
      // layout pass.
      var cfgScrollPx = Math.round(cfgScrollY);
      if (cfgScrollPx !== cfgAppliedScrollY) {
        cfgAppliedScrollY = cfgScrollPx;
        scrollWrap.style.marginTop = (-cfgScrollPx) + 'px';
      }
      // Scrollbar: the only affordance that the column HAS more content, now that the native
      // one is gone. Hidden entirely when everything fits.
      if (maxY <= 0.5 || viewH <= 0 || contH <= 0) { scrollThumb.visible = false; return; }
      var th = Math.max(30, viewH * (viewH / contH));
      var ty = (viewH - th) * (cfgScrollY / maxY);
      scrollThumb.visible = true;
      scrollThumb.style.height = Math.round(th) + 'px';
      // The thumb tracks the SCROLL REGION, which now starts below the pinned chrome.
      scrollThumb.style.marginTop = Math.round(cfgScrollTop() + ty) + 'px';
    }

)CFJS"
R"CFJS(
    // ===================== HEADER + PANEL CHROME ========================
    // ONE row: the panel's name, then its chips. The original design's header carries MOUSE and ESC EXIT;
    // Game UI is the mod's third piece of panel chrome and joins them rather than keeping a
    // second row of its own, so the column starts with one line instead of two.
    // PINNED CHROME GETS ITS OWN STACKING CONTEXT. Without an explicit zIndex this row is
    // composited together with the clipped scroll column beside it, and while a card straddles
    // the viewport's top edge the engine drops the row's children from the frame -- the boxes
    // keep their exact geometry and the text simply stops painting. See the header of
    // scratchpad patch_chromelayer / the note in cfgApplyScroll for the measurements. Purely a
    // paint-order change: this row sits above the scroll viewport in flow and never overlaps it.
    var head = mk('Panel', inspector); head.hittest = false; head.id = 'CfgHead';
    head.style.flowChildren = 'right'; head.style.width = '100%';
    head.style.zIndex = '80';
    var hTitle = lbl(head, 'CONFIG', S.value, 18); hTitle.hittest = false;
    hTitle.style.fontWeight = 'bold';
    hTitle.style.letterSpacing = '2.5px'; hTitle.style.width = 'fill-parent-flow(1.0)';
    hTitle.style.verticalAlign = 'center'; hTitle.style.whiteSpace = 'nowrap';
    // A header chip. ONE definition for every workspace -- see WorkspaceChromeJs.h for why
    // this stopped being per-panel and why a chip carries no accent fill.
    function chromeChip(text, onClick) {
      return mvmChromeChip(head, function (parent, t, f) { return btn(parent, t, f, S.label); },
        paintBtn, text, onClick);
    }
    // No MOUSE: UI/GAME chip: in a demo, where you click decides it (Movie/UiClickMath.h) -- the
    // preview is GAME, everything else is UI.
    // Game-UI visibility: how much of CS2's own HUD shows behind the panel. Hide All = clean
    // plate, In-Game = radar + HP/ammo with no observer panel, Show All = the full spectator HUD.
    var HUD_CYCLE = [['hidden', 'Hide All'], ['game', 'In-Game'], ['full', 'Show All']];
    var hudCycle = chromeChip('GAME UI: SHOW ALL', function () {
      var cur = (st && st.hudView) || 'full', idx = 0;
      for (var i = 0; i < HUD_CYCLE.length; i++) if (HUD_CYCLE[i][0] === cur) idx = i;
      cmd('mirv_filmmaker editor hud ' + HUD_CYCLE[(idx + 1) % HUD_CYCLE.length][0]);
    });
    var exitBtn = chromeChip('ESC EXIT', function () { cmd('mirv_filmmaker config close'); });
    exitBtn.id = 'CfgExit';

    // ===================== INSPECTOR TAB STRIP ==========================
    // TEXT TABS ON A RULE, left-aligned with a fixed gap and an accent underline on the active
    // one -- the same grammar the cards use for their sub-pages, one size up. Each tab fragment
    // below appends its cards to its own page panel; applyTabs() toggles `visible`.
    // CAMERA / EFFECTS / ENVIRONMENT / RAGDOLLS, in that order (Docs/panorama/panorama_ui_guide.md). EFFECTS is
    // everything that drives a value CS2 ALREADY HAS; ENVIRONMENT is the added renderer drawn on
    // top of it. They sit next to each other because the split between them is the thing the
    // operator has to hold in their head -- see the note at the top of ConfigHudEffectsTabJs.h.
    var TABS = [['camera', 'CAMERA'], ['effects', 'EFFECTS'], ['environment', 'ENVIRONMENT'], ['ragdolls', 'RAGDOLLS']];
    var activeTab = 'camera';
    var configUiHydrated = false;
    var configWasEnabled = false;   // drives the one-shot panel entrance
    var tabStrip = mk('Panel', inspector); tabStrip.hittest = false;
    tabStrip.style.zIndex = '80';   // same reason as `head` above
    tabStrip.style.flowChildren = 'right';
    tabStrip.style.width = '100%'; tabStrip.style.marginTop = '20px';
    tabStrip.style.borderBottom = '1px solid #ffffff12';
    var tabBtns = {};
    for (var ti = 0; ti < TABS.length; ti++) (function (key, caption, last) {
      var b = tabify(btn(tabStrip, caption, function () {
        // A tab click always lands on the tab itself, so it drops a running search first.
        if (cfgSearchActive) cfgSetQuery('');
        activeTab = key;
        applyTabs();
        cmd('mirv_filmmaker settings ui config.tab ' + key);
      }, S.label));
      // The TOP of the hierarchy, so one size up from tabify's default, and sized to its own
      // caption with a fixed gap rather than stretched: four stretched tabs put ENVIRONMENT's
      // 11 characters in the same slot as CAMERA's 6 and left the strip reading as a button row.
      b.style.width = 'fit-children';
      b.style.marginRight = last ? '0px' : UI.tabTop.gap + 'px';
      b.style.paddingLeft = '0px'; b.style.paddingRight = '0px';
      b.style.paddingBottom = UI.tabTop.padBottom + 'px';
      b.__lbl.style.fontSize = UI.tabTop.font + 'px';
      b.__lbl.style.letterSpacing = UI.tabTop.ls + 'px';
      tabBtns[key] = b;
    })(TABS[ti][0], TABS[ti][1], ti === TABS.length - 1);
    // GLOBAL SEARCH sits at the right end of the tab rule (Docs/features/filmmaker_config_panel.md). It searches every tab;
    // typing goes through the same WM_CHAR bridge the preset/scene name fields use
    // (configWindowChars), because HUD TextEntry panels never get real keyboard focus here.
    var tabSpacer = mk('Panel', tabStrip); tabSpacer.hittest = false;
    tabSpacer.style.width = 'fill-parent-flow(1.0)'; tabSpacer.style.height = '1px';
    var searchBox = mk('Panel', tabStrip); searchBox.hittest = true;
    searchBox.style.flowChildren = 'right'; searchBox.style.width = '180px'; searchBox.style.height = '28px';
    searchBox.style.marginBottom = '7px'; searchBox.style.verticalAlign = 'bottom';
    searchBox.style.paddingLeft = '9px'; searchBox.style.paddingRight = '9px';
    searchBox.style.borderRadius = '3px'; searchBox.style.backgroundColor = '#0c0e12ff';
    trans(searchBox, 'border-color', 0.08);
    var searchIcon = lbl(searchBox, '⌕', S.muted, 15); searchIcon.hittest = false;
    searchIcon.style.verticalAlign = 'center'; searchIcon.style.marginRight = '7px';
    var searchText = lbl(searchBox, 'Search all tabs', S.muted, 13); searchText.hittest = false;
    searchText.style.width = 'fill-parent-flow(1.0)'; searchText.style.verticalAlign = 'center';
    searchText.style.whiteSpace = 'nowrap'; searchText.style.textOverflow = 'ellipsis';
    var searchClear = lbl(searchBox, '×', S.muted, 15); searchClear.hittest = true; searchClear.visible = false;
    searchClear.style.verticalAlign = 'center'; searchClear.style.marginLeft = '4px';

    // BODY: the rail on the left, the scrolling pane on the right. Only the pane scrolls; the
    // rail never needs to (one row per card), so it stays put while a long card moves.
    var bodyRow = mk('Panel', inspector); bodyRow.hittest = false; bodyRow.hittestchildren = true;
    bodyRow.style.flowChildren = 'right'; bodyRow.style.width = '100%';
    bodyRow.style.height = 'fill-parent-flow(1.0)'; bodyRow.style.marginTop = '14px';
    var cfgRailHost = mk('Panel', bodyRow); cfgRailHost.hittest = false; cfgRailHost.hittestchildren = true;
    cfgRailHost.style.flowChildren = 'down';
    cfgRailHost.style.width = UI.rail.w + 'px'; cfgRailHost.style.height = '100%';
    cfgRailHost.style.paddingRight = UI.rail.padR + 'px';
    cfgRailHost.style.borderRight = '1px solid #ffffff12';
    cfgRailHost.style.overflow = 'clip';
    // The scroll region takes whatever width is left, so the title/mouse row/tab strip stay put
    // while only the selected card moves.
    scrollView = mk('Panel', bodyRow);
    scrollView.style.width = 'fill-parent-flow(1.0)';
    scrollView.style.height = '100%';
    scrollView.style.marginLeft = UI.rail.gap + 'px';
    scrollView.style.overflow = 'clip';   // clip only: its child is positioned, never squished
    scrollWrap = mk('Panel', scrollView);
    scrollWrap.style.flowChildren = 'down';
    scrollWrap.style.width = '100%';
    scrollWrap.style.height = 'fit-children';
    scrollWrap.style.transitionDuration = '0s';   // never animate the scrolled subtree

    var pageCamera = mk('Panel', scrollWrap);
    pageCamera.style.flowChildren = 'down'; pageCamera.style.width = '100%';
    var pageEffects = mk('Panel', scrollWrap);
    pageEffects.style.flowChildren = 'down'; pageEffects.style.width = '100%';
    var pageRagdolls = mk('Panel', scrollWrap);
    pageRagdolls.style.flowChildren = 'down'; pageRagdolls.style.width = '100%';
    var pageEnvironment = mk('Panel', scrollWrap);
    pageEnvironment.style.flowChildren = 'down'; pageEnvironment.style.width = '100%';
    // The search results replace the rail and the pane while a query is up.
    var pageSearch = mk('Panel', scrollWrap);
    pageSearch.style.flowChildren = 'down'; pageSearch.style.width = '100%';
    pageSearch.visible = false;
    pageCamera.__tabKey = 'camera'; pageEffects.__tabKey = 'effects';
    pageEnvironment.__tabKey = 'environment'; pageRagdolls.__tabKey = 'ragdolls';
    var pages = { camera: pageCamera, effects: pageEffects, environment: pageEnvironment, ragdolls: pageRagdolls };

    // ---- the pinned footer ------------------------------------------------------------------
    // "Keep between demos" and the tab's RESET are STUCK TO THE BOTTOM of the column
    // (Docs/panorama/panorama_ui_guide.md): they are the tab's standing statement about itself, not its last setting,
    // and a footer that scrolls away is a footer you have to go looking for.
    //
    // A SIBLING of scrollView, not a child of it. scrollView is height:fill-parent-flow, so this
    // band takes its own height first and the scroll viewport gets whatever is left -- which is
    // also what keeps the two from overlapping at any resolution.
    var pinnedFooter = mk('Panel', inspector); pinnedFooter.hittest = false;
    pinnedFooter.style.flowChildren = 'down'; pinnedFooter.style.width = '100%';
    pinnedFooter.style.height = 'fit-children';
    pinnedFooter.style.paddingTop = '12px'; pinnedFooter.style.paddingBottom = '2px';
    pinnedFooter.style.marginTop = '10px';
    // No rule above the footer: it sits straight on the column ground, like the Camera Editor's.
    // One holder per tab, shown with its page. cfgFooterHosts is what footerCard() looks a page
    // up in, so the tab fragments keep calling footerCard(pageX) and land here instead.
    var footerCamera = mk('Panel', pinnedFooter);
    var footerEffects = mk('Panel', pinnedFooter);
    var footerRagdolls = mk('Panel', pinnedFooter);
    var footerEnvironment = mk('Panel', pinnedFooter);
    var pageFooters = { camera: footerCamera, effects: footerEffects,
                        environment: footerEnvironment, ragdolls: footerRagdolls };
    for (var fk in pageFooters) {
      pageFooters[fk].style.flowChildren = 'down';
      pageFooters[fk].style.width = '100%';
      pageFooters[fk].hittest = false; pageFooters[fk].hittestchildren = true;
      cfgFooterHosts.push([pages[fk], pageFooters[fk]]);
    }

    var configTabSeen = '';
    var cfgSearchActive = false;   // set by ConfigHudSearchJs.h while a query is up
    var cfgViewEditSent = '';      // the last 'viewedit' published to C++
    function applyTabs() {
      for (var k in pages) {
        pages[k].visible = !cfgSearchActive && (k === activeTab);
        if (cfgRails[k]) cfgRails[k].host.visible = (k === activeTab);
        pageFooters[k].visible = (k === activeTab);
        styleTab(tabBtns[k], !cfgSearchActive && k === activeTab);
      }
      // A query replaces the rail AND the pane with the result list, at the pane's full width.
      cfgRailHost.visible = !cfgSearchActive;
      // The ENVIRONMENT tab edits in the preview: C++ (UiClick viewEdit) then leaves a left click
      // in the view to the scene-object picker and makes a held right click look around.
      var viewEdit = !cfgSearchActive && activeTab === 'environment' ? '1' : '0';
      if (cfgViewEditSent !== viewEdit) { cfgViewEditSent = viewEdit; root.SetAttributeString('viewedit', viewEdit); }
      scrollView.style.marginLeft = cfgSearchActive ? '0px' : (UI.rail.gap + 'px');
      pageSearch.visible = cfgSearchActive;
      // applyTabs() is called both on a click and from render(), so the entrance is keyed on a
      // real change -- otherwise the visible page would restart its animation every frame.
      if (configTabSeen !== activeTab) {
        configTabSeen = activeTab;
        // A new tab starts at its top; carrying the previous tab's offset into a shorter tab
        // would open it already scrolled past its first card.
        cfgScrollY = 0;
        // No entrance animation on a tab swap: the page lives inside the scrolled
        // subtree, and transforming it reintroduces the layer that made pinned text
        // flicker. The tab strip's own underline already reports the change.
      }
    }

    // Each tab fragment pushes a function(st) here; render() just runs them all, so it never
    // has to know what any tab contains.
    var renderHooks = [];
    // Editing shortcuts from C++ (ConfigHud_PushKey: "delete", "duplicate"), each handed to every
    // hook; a hook acts only when its own selection is the one on screen.
    var keyHooks = [];
)CFJS"
#include "ConfigHudCameraTabJs.h"
#include "ConfigHudEnvironmentNavJs.h"
#include "ConfigHudClimateJs.h"
#include "ConfigHudEffectsTabJs.h"
// Sub-tabs of EFFECTS. MUST follow ConfigHudEffectsTabJs.h -- that fragment creates the
// fxPage* containers these append into.
#include "ConfigHudLightingTabJs.h"
#include "ConfigHudAtmosphereTabJs.h"
#include "ConfigHudWeatherSkyTabJs.h"
// Shared EFFECTS footer (keep-between-demos toggle + per-sub-tab RESET). MUST follow all four
// sub-tab fragments: it parents to pageEffects, and Panorama flows siblings in creation order,
// so this is what puts it BELOW whichever sub-page is showing.
#include "ConfigHudEffectsResetJs.h"
#include "ConfigHudRagdollsTabJs.h"
// The per-player death animation picker the Advanced Ragdolls card opens. Parents to root, so it
// must follow the fragments that build the tabs underneath it.
#include "ConfigHudDeathAnimPickerJs.h"
#include "ConfigHudEnvironmentTabJs.h"
// Global search. MUST follow every tab fragment: the index fills as their widgets build.
#include "ConfigHudSearchJs.h"
R"CFJS(
    applyTabs();

)CFJS"
R"CFJS(
    // ---- bottom chrome under the action row -------------------------------------------
    // Both are parented to ROOT, not to the inspector: they must stay pinned while the column
    // scrolls. Explicit z-index orders the three layers deterministically:
    //   inspector (auto/0)  <  footer (100)  <  fade (110)  <  actionRow (120).
    //
    // The footer fills the strip freed by shortening the viewport -- without it the sidebar
    // would end above the buttons and the live game would show through behind them.
    // Scrollbar thumb. Parented to root (it must not scroll with the content it reports on) and
    // z-ordered under the fade so it disappears into the cut-off rather than crossing it.
    var scrollThumb = mk('Panel', root); scrollThumb.hittest = false; scrollThumb.visible = false;
    scrollThumb.style.horizontalAlign = 'right'; scrollThumb.style.verticalAlign = 'top';
    scrollThumb.style.marginRight = '3px'; scrollThumb.style.width = '4px';
    scrollThumb.style.borderRadius = '2px';
    scrollThumb.style.backgroundColor = '#ffffff33';
    scrollThumb.style.zIndex = '105';

    var footerBg = mk('Panel', root); footerBg.hittest = false; footerBg.visible = false;
    footerBg.style.horizontalAlign = 'right'; footerBg.style.verticalAlign = 'bottom';
    footerBg.style.width = INSPECTOR_W + 'px'; footerBg.style.height = ACTION_CLEAR + 'px';
    // Transparent over the column ground (see columnGround): a fill here was the flat strip
    // under RENDER / CUSTOMIZE PLAYER.
    footerBg.style.backgroundColor = S.clear;
    footerBg.style.borderLeft = '1px solid ' + S.clear;
    footerBg.style.zIndex = '100';

    // The cut-off fade: content dissolves into the footer instead of being sliced off by it.
    // The scroll area fades at BOTH edges (Docs/panorama/panorama_ui_guide.md), not just into the footer. Without the
    // top one a card scrolled up under the tab strip is sliced off by a hard edge, which reads as
    // a clipping bug rather than as content continuing above.
    // Parented to root, like the bottom fade, so it does not scroll with what it is fading.
    var TOP_FADE_H = 20;
    var topFade = mk('Panel', root); topFade.hittest = false; topFade.visible = false;
    topFade.style.horizontalAlign = 'right'; topFade.style.verticalAlign = 'top';
    topFade.style.width = INSPECTOR_W + 'px'; topFade.style.height = TOP_FADE_H + 'px';
    topFade.style.zIndex = '110';
    topFade.style.backgroundColor =
      'gradient( linear, 0% 0%, 0% 100%, from( #0f1114ff ), to( #0f111400 ) )';

    var bottomFade = mk('Panel', root); bottomFade.hittest = false; bottomFade.visible = false;
    bottomFade.style.horizontalAlign = 'right'; bottomFade.style.verticalAlign = 'bottom';
    bottomFade.style.marginBottom = ACTION_CLEAR + 'px';
    bottomFade.style.width = INSPECTOR_W + 'px'; bottomFade.style.height = FADE_H + 'px';
    bottomFade.style.zIndex = '110';
    bottomFade.style.backgroundColor =
      'gradient( linear, 0% 0%, 0% 100%, from( #0f111400 ), to( #0f1114ff ) )';

    // ---- the column's drag edge ------------------------------------------------------------
    // Everything that had INSPECTOR_W baked into a style at build time is re-written here, in
    // one place, so a resize cannot leave one of them behind. The pieces that COMPUTE from it
    // (layoutDemoWorkspace's areaW, the docked bar's width) read the variable live and need no
    // help; they just have to run again, which the next render does anyway.
    function cfgApplyInspectorW(w) {
      INSPECTOR_W = w;
      inspector.style.width = w + 'px';
      footerBg.style.width = w + 'px';
      topFade.style.width = w + 'px';
      bottomFade.style.width = w + 'px';
      if (settingsPopup && settingsPopup.setMarginRight) settingsPopup.setMarginRight(w + 16);
      // RENDER / CUSTOMIZE PLAYER float over the column at zIndex 120 and had UI.col.w baked in
      // at build time, so they kept the old edge while everything around them moved.
      actionRow.style.width = Math.floor(w - UI.action.inset * 2) + 'px';
      cfgGrip.place(w);
      // The docked timeline / camera / graph are DIFFERENT panels with their own state, and the
      // width is only persisted on release -- so tell them directly, or they lag the whole drag.
      mvmPublishInspW(w);
      // The scroll viewport is a function of the column, so re-run the clamp: a narrower column
      // is a taller content box and the current offset can now be past the end.
      try { cfgApplyScroll(); } catch (e) {}
    }
    var cfgGrip = mvmInspectorGrip({
      root: root,
      colour: S.accent,
      screenW: function () {
        var sx = ctx.actualuiscale_x || root.actualuiscale_x || 1;
        var raw = ctx.actuallayoutwidth || root.actuallayoutwidth || 0;
        return raw / sx;
      },
      apply: function (w) { cfgApplyInspectorW(w); },
      commit: function (w) { cmd('mirv_filmmaker settings ui inspectorW ' + w); }
    });
    cfgGrip.place(INSPECTOR_W);

)CFJS"
// Split here: MSVC caps a single raw string literal at ~16380 bytes (C2026). Adjacent
// literals are concatenated by the compiler, so this stays one script.
R"CFJS(
    // Persistent workspace actions. A real flow row keeps the buttons on one baseline with
    // deliberate spacing; each action switches to the Camera workspace before opening.
    // RENDER / CUSTOMIZE PLAYER, from the one shared builder (WorkspaceChromeJs.h). This
    // panel's geometry is the spec the other workspaces now follow.
    var dockActions = mvmActionRow(root, makeInteractive, [
      { text: 'RENDER', width: 150, onClick: function () { cmd('mirv_filmmaker editor render'); } },
      { text: '\u2726  CUSTOMIZE PLAYER', width: 'fill-parent-flow(1.0)',
        onClick: function () { cmd('mirv_filmmaker editor customize'); } }
    ]);
    var actionRow = dockActions.root;
    var renderAction = dockActions.buttons[0], customizeAction = dockActions.buttons[1];

    // ===================== RENDER =======================================
    var st = null;
    // Raw `state` string the render hooks last ran against, and a slow heartbeat so a hook can
    // never stay stale indefinitely. See the gate in render().
    var lastHookRaw = null;
    var hookHeartbeat = 0;
    var api = {};
    api.render = function () {
      columnGround.sync(INSPECTOR_W, inspector.visible);
      var raw = root.GetAttributeString('state', '');
      // Release the live mouse claim on every path that hides the panel, or the game stays
      // locked out of its own input after the panel is gone.
      if (!raw) { setLiveMouseCapture(false); if ($.ConfigHudDap && $.ConfigHudDap.isOpen()) $.ConfigHudDap.close(); root.visible = false; inspector.visible = false; hideDemoWorkspace(); return; }
      try { st = JSON.parse(raw); } catch (e) { return; }
      // Everything from hydration to the chrome labels runs in one guard, because an exception
      // ANYWHERE in it used to skip the per-tab render hooks below -- and those hooks are what
      // write the labels. That is the mechanism behind "the CONFIG text sometimes disappears":
      // one bad assignment aborts the rest of render(), so every sync() stops updating and the
      // panel silently goes stale. It has bitten here before: the winFieldBlur ReferenceError
      // (fixed 2026-08-28) threw every frame and took layoutDemoWorkspace down with it.
      //
      // Same contract as cfgCursorPump and the render hooks below: degrade to "chrome not
      // updated this frame", and NAME the error rather than failing silently.
      try {
        if (!configUiHydrated) {
          if (pages[st.uiConfigTab]) activeTab = st.uiConfigTab;
          // Each tab's selected rail card, then each card's sub-page. An absent or unknown key
          // falls back to the first card in rail order.
          var ui0 = st.ui || {};
          for (var rk in cfgRails) {
            var want = ui0['config.' + rk + '.sel'];
            if (!want || !cfgSelectCard(rk, want, false)) cfgSelectCard(rk, cfgRailFirst(rk), false);
          }
          for (var ck in cfgSections) {
            var pv = ui0[ck + '.page'];
            if (pv !== undefined && cfgSections[ck].__setPage) cfgSections[ck].__setPage(pv);
          }
          applyTabs();
          configUiHydrated = true;
        }
        root.visible = !!st.enabled;
        if (!st.enabled) {
          configWindowBlur();
          setLiveMouseCapture(false);
          inspector.visible = false;
          actionRow.visible = false;
          footerBg.visible = false;
          bottomFade.visible = false;
          topFade.visible = false;
          hideDemoWorkspace();
          configWasEnabled = false;   // so the next enable plays its entrance again
          return;
        }
        var configJustOpened = !configWasEnabled;
        configWasEnabled = true;
        // (The window Width/Height text fields this used to blur moved to the CS2 Video settings
        // Custom Resolution dialog; both winFocusField and winFieldBlur are gone. The dangling
        // reference threw a ReferenceError on EVERY render, so nothing below it ran -- including
        // layoutDemoWorkspace, which is what publishes `previewrect`. Result: the panel drew its
        // controls but the scaled preview was never requested, so the world rendered FULL SCREEN
        // with the inspector on top. Fixed 2026-08-28.)
        inspector.visible = true;
        actionRow.visible = true;
        // Fit the scroll viewport above the action row before anything is measured this frame.
        syncInspectorHeight();
        footerBg.visible = true;
        // Same rule as the Camera Editor: the fade exists to dissolve content that actually runs under
        // the footer. With nothing scrolled beneath it, it was just a dark band over the column ground.
        bottomFade.visible = cfgContentH() - cfgScrollY > cfgViewH() - FADE_H + 1;
        // The fade belongs to the SCROLL area, so it has to clear the pinned footer as well as
        // the action row. Measured rather than assumed: the footer's height changes with the tab
        // (the keep row hides itself outside a demo).
        var pfsy = inspector.actualuiscale_y || 1;
        var pfh = (pinnedFooter.actuallayoutheight || 0) / pfsy;
        if (!(pfh >= 0) || pfh > 1e30) pfh = 0;
        bottomFade.style.marginBottom = Math.round(ACTION_CLEAR + pfh) + 'px';
        // Only while there is something above to fade INTO.
        topFade.visible = cfgScrollY > 1;
        topFade.style.marginTop = (scrollView ? cfgScrollTop() : 0) + 'px';
        // The panel arriving is the one moment this workspace earns motion: it appears over a live
        // game, and a hard cut reads as a rendering glitch rather than as a panel opening.
        // The inspector is deliberately NOT transformed on open: it owns both clipped
        // content and pinned text, and animating it flickers both.
        if (configJustOpened) { /* no entrance transform -- see the note above */ }
        renderAction.__lbl.text = st.renderJobActive ? 'RENDER PROGRESS' : 'RENDER';
        // Through setBtnOn, never straight to style: these run every frame and would repaint
        // over the hover fill before it was ever seen (see paintBtn in ConfigHudWidgetsJs.h).
        setBtnOn(renderAction, !!st.renderJobActive, S.accent);
        customizeAction.style.opacity = st.povReady ? '1.0' : '0.55';
        customizeAction.hittest = !!st.povReady;

        var cur = !!st.cursor;
        inspector.hittest = cur;
        // Popups cannot be clicked in GAME-mouse mode, so do not leave one hanging. Guarded on
        // isOpen() -- a per-frame closer that fires unconditionally is its own bug (it stomped the
        // editor's dropdowns once already).
        if ((!cur || !st.demo) && settingsPopup.isOpen()) settingsPopup.close(cur ? 'noDemo' : 'noCursor');
        // Live map only, and only while the panel holds the UI cursor: G hands the mouse back to
        // the game, and that has to release the deny-input claim too or aiming stays broken.
        setLiveMouseCapture(!st.demo && cur);

)CFJS"
// Split for MSVC's ~16380-byte cap on a single raw string literal (C2026), not for any
// semantic reason. Adjacent raw literals concatenate, so the seam is invisible to the script.
R"CFJS(
        // THE COLUMN WIDTH HAS TO BE RESOLVED BEFORE THE WORKSPACE IS LAID OUT.
        //
        // layoutDemoWorkspace computes `areaW = rw - INSPECTOR_W` and publishes the result as
        // `previewrect`, which is what the world blit and the scaled game HUD both follow. This
        // block -- which pulls INSPECTOR_W up to the operator's stored width -- used to run about
        // forty lines BELOW that call, so the first frame of every open laid the workspace out
        // against the width this panel happened to be carrying and the correction only landed on
        // the next frame.
        //
        // That is the flicker on the Camera Editor -> Config swap. Traced in game
        // (`$.CamTimeline.trace(1)` alongside the confighud.previewrect log line), consecutive
        // frames with Config open for both:
        //
        //   frame 8013  previewrect 0.00000 0.12037 0.63819 0.75833
        //   frame 8014  previewrect 0.00000 0.09167 0.69444 0.78611
        //
        // -- one frame with the scaled world, its backdrop and the whole gameplay HUD drawn
        // ~80px narrower and lower, then a jump to the real size. Everything moves at once, which
        // is why it reads as the background flickering rather than as one panel resizing.
        // Resolving the width first makes the first frame the right one, so there is nothing to
        // jump from.
        //
        // The drag edge has to be pumped here too: it is what says whether the operator is
        // holding the edge right now, and a stored width must not fight a live drag. It still
        // runs before the value-tile pump below, which is the other thing that constraint needs.
        var gripBusy = false;
        try { gripBusy = cfgGrip.pump(st); } catch (eGrip) {}
        // Follow the stored width whenever the operator is not dragging -- this is what applies a
        // width set in another panel, or restored from the settings file on the first frame.
        if (!gripBusy && typeof st.inspW === 'number' && st.inspW > 0) {
          var wantW = Math.floor(st.inspW);
          var capW = 0;
          {
            var gsx = ctx.actualuiscale_x || root.actualuiscale_x || 1;
            var graw = ctx.actuallayoutwidth || root.actuallayoutwidth || 0;
            var gsw = graw / gsx;
            capW = (gsw > 1) ? Math.max(600, gsw * 0.62) : 1e9;   // same cap as InspectorWidth.h
          }
          if (wantW > capW) wantW = Math.floor(capW);
          if (wantW < 600) wantW = 600;
          if (wantW !== INSPECTOR_W) cfgApplyInspectorW(wantW);
        }

        if (st.demo) layoutDemoWorkspace(cur);
        else hideDemoWorkspace();

        var hv = st.hudView || 'full', hudName = 'Show All';
        for (var hb = 0; hb < HUD_CYCLE.length; hb++) if (HUD_CYCLE[hb][0] === hv) hudName = HUD_CYCLE[hb][1];
        hudCycle.__lbl.text = 'GAME UI: ' + hudName.toUpperCase();
      } catch (eChrome) {
        $.Msg('[confighud] chrome render failed: ' + eChrome + '\n');
      }

      // Per-tab mirrors. Every hook runs even for a hidden tab: the sync() calls are cheap and
      // keeping hidden controls current means switching tabs never shows a stale frame.
      // Relative slider dragging (Shift = fine). Runs BEFORE the per-tab mirrors: the pump can
      // write a new value this frame, and the sync() hooks below must see it rather than paint
      // over it with last frame's state.
      // (The drag edge and the stored column width are resolved higher up, immediately before
      // layoutDemoWorkspace -- which is the thing that consumes the width. See the note there.)
      try { cfgColorPump(st); cfgDropdownPump(st); cfgCursorPump(st); cfgTextPump(st); }
      catch (ep) { $.Msg('[confighud] slider pump failed: ' + ep + '\n'); }

      // THE HOOK SWEEP IS GATED ON THE STATE ACTUALLY CHANGING.
      //
      // Every hook is a pure function of `st` -- none of them reads the clock, a frame counter
      // or anything random (checked 2026-09-19), so when the state string is byte-identical to
      // last frame they would write exactly what is already on screen. The C++ side only
      // re-publishes the `state` attribute when it changes, so comparing the raw string is an
      // exact test for "nothing to do".
      //
      // Why it is worth gating. render() runs EVERY hook for EVERY tab each frame, not just the
      // visible one. Measured 2026-09-19 on a paused frame at 2560x1440: stubbing render() out
      // took the panel from 109 to 136 fps, so the sweep alone cost 1.74 ms of a 9.09 ms frame
      // -- the single largest thing the panel does. While you are actually dragging a control
      // the state changes every frame, so the sweep runs exactly as often as before; it is the
      // idle panel, which is most of the time it is open, that stops paying for itself.
      //
      // The heartbeat is insurance, not the mechanism: if some future hook does grow a
      // dependency the state string cannot see, it still refreshes a few times a second instead
      // of never.
      var hooksDirty = (raw !== lastHookRaw);
      if (!hooksDirty && ++hookHeartbeat >= 30) hooksDirty = true;
      if (hooksDirty) {
        lastHookRaw = raw;
        hookHeartbeat = 0;
        for (var rh2 = 0; rh2 < renderHooks.length; rh2++) {
          try { renderHooks[rh2](st); }
          catch (eh) { $.Msg('[confighud] render hook ' + rh2 + ' failed: ' + eh + '\n'); }
        }
      }

      // Last: the hooks above can change the column's height (a card opening, a longer
      // readout), so the clamp and the scrollbar are resolved against this frame's real size.
      cfgApplyScroll();
    };

    // Called from C++ (ConfigHud::DispatchWheel) with the notches captured this frame:
    // +1 = wheel up. One notch moves a bit more than a card header, which is the amount that
    // reads as "a scroll" without overshooting a card.
    api.wheel = function (notches) {
      var n = parseInt(notches, 10);
      if (!isFinite(n) || n === 0) return;
      // While the death animation picker is up, the wheel scrolls its clip list, not the column.
      if ($.ConfigHudDap && $.ConfigHudDap.isOpen()) { $.ConfigHudDap.wheel(n); return; }
      // The wheel moves the content out from under a stationary cursor, and Panorama fires no
      // mouseout for that -- so a tooltip opened before the scroll would be left hanging over
      // controls it no longer belongs to. Drop it with the first notch.
      tipHide();
      cfgDropdownClose();   // the popover is anchored to a field the wheel is about to move
      // A list that scrolls on its own (scrollWell) takes the notch while the cursor is over it;
      // at its top or bottom the notch goes on to the column.
      if (cfgWellWheel(n)) return;
      // The wheel ALWAYS scrolls the column. It used to change the value of whatever tile the
      // pointer happened to be over ("wheel = 2% per notch" in the original design), which meant scrolling
      // past a slider silently edited it -- you cannot move through a tab without crossing tiles.
      // Values are changed by dragging them, which is deliberate and reversible; scrolling is not.
      cfgScrollY -= n * 90;
      cfgApplyScroll();
    };
    api.chars = function (csv) { configWindowChars(csv); };
    api.key = function (name) {
      for (var k = 0; k < keyHooks.length; k++) {
        try { keyHooks[k](String(name)); }
        catch (ek) { $.Msg('[confighud] key hook ' + k + ' failed: ' + ek + '\n'); }
      }
    };
    api.wordmark = function (at) { shell.replay(at); return 'wordmark ' + (at === undefined ? 'replay' : at); };
    // Run the global search from the console: mirv_filmmaker config eval $.ConfigHud.search('tilt')
    api.search = function (q) { cfgSetQuery(evalArg(q)); return cfgQuery; };
    // Open or shut a dropdown by its label: $.ConfigHud.dropdown('Preset')
    api.dropdown = function (label) {
      var d = cfgDropdowns[evalArg(label)];
      if (!d) return 'unknown';
      d.toggle();
      return cfgDropOpen === d ? 'open' : 'closed';
    };
    // Open or shut a colour field's inline picker by its label: $.ConfigHud.color('Fog color')
    api.color = function (title) {
      var p = cfgColorFields[evalArg(title)];
      if (!p) return 'unknown';
      p.show();
      return p.visible() ? 'open' : 'closed';
    };
    // Drives the Weapon Recoil preset field from the console the way the Render workspace's
    // renderFocus does, so the focus -> type -> Enter path is testable without a mouse:
    //   mirv_filmmaker config eval $.ConfigHud.presetFocus()
    //   mirv_filmmaker config eval $.ConfigHud.chars('112,49,13')
    api.presetFocus = function () { wrPresetFocusOn(); return 'ok'; };
    api.presetBuf = function () { return wrPresetBuf; };
    // Run a RESET exactly as its button does -- same spec, same three phases. Registered so the
    // resets can be regression-tested without a mouse: there is no way to click a two-step
    // confirm button from automation, and these are the controls most worth proving.
    //   mirv_filmmaker config eval $.ConfigHud.reset('effects')
    // Names: camera, effects, ragdolls, environment, env.sky, env.light, env.fog, env.looks,
    // plus every camera card's own label.
    // Press a tab footer's RESET as a click would, confirm included ($.ConfigHud.press('environment')).
    api.press = function (which) { var f = cfgFooterPress[String(which).trim()]; if (f) f(); return !!f; };
    // Close any open menu, dropdown or confirm (automation; a click outside does the same).
    api.dismiss = function () { cfgConfirmClose(); cfgDropdownClose(); return 'dismissed'; };
    api.reset = function (which) {
      var name = evalArg(which);
      if (!name) {
        var all = [];
        for (var k in cfgResets) all.push(k);
        return all.join(' ');
      }
      if (!cfgResets[name]) return 'unknown: ' + name;
      return cfgRunReset(cfgResetSpec(cfgResets[name]())) ? ('reset ' + name) : ('empty: ' + name);
    };
    // Diagnostic dump for the "pinned text disappears while scrolling" bug. The dropout turned
    // out to be a persistent, perfectly reproducible state at scroll offsets 450-630 css px --
    // not a race and not the wheel -- and it survives a forced restyle, so it is neither a
    // clobbered colour nor a transient. This prints every pinned panel AND every overlay that
    // could be sitting on top of them, so a mispositioned overlay shows up as geometry rather
    // than having to be inferred from screenshots.
)CFJS"
R"CFJS(
    // Park the column at an exact offset, in css px. api.wheel only moves in whole 90px
    // notches, so this is the only way a regression capture can hit a specific scroll
    // position (it is how the pinned-chrome dropout was bisected to a 10px window).
    api.scrollTo = function (px) {
      var v = parseFloat(evalArg(px));
      if (!isFinite(v)) return Math.round(cfgScrollY);
      cfgScrollY = v;
      cfgApplyScroll();
      return Math.round(cfgScrollY);
    };
    // ---- automation hooks ----------------------------------------------------------------
    // Synthetic mouse input never reaches CS2 (see Docs/agent), so the only way to drive this
    // panel from a script is a REGISTERED helper. netcon truncates `ui_eval` at 256 bytes, which
    // is why these are short verbs rather than inline JS.
    //
    // gotoTab is not just a settings write: st.uiConfigTab is only read once, during hydration,
    // so `mirv_filmmaker settings ui config.tab X` moves the stored value and nothing on screen.
    // ARGUMENTS ARRIVE PADDED. CS2's console tokenizer splits on quotes and commas, and the
    // `... eval <js>` verbs rejoin ArgV with a single space -- so `gotoTab('camera')` reaches the
    // script as `gotoTab(' camera ')`. Harmless inside JS syntax, fatal for a string compare, so
    // every console-driven helper trims what it is given.
    function evalArg(v) { return ('' + (v === undefined || v === null ? '' : v)).replace(/^\s+|\s+$/g, ''); }
    api.gotoTab = function (key) {
      key = evalArg(key);
      if (!pages[key]) return 'unknown:' + key;
      activeTab = key;
      applyTabs();
      cmd('mirv_filmmaker settings ui config.tab ' + key);
      return key;
    };
    // Select one card by its persistKey ('env.sky', 'config.section.handheld', ...).
    api.card = function (persistKey, open) {
      persistKey = evalArg(persistKey);
      var body = cfgSections[persistKey];
      if (!body) return 'unknown:' + persistKey;
      // With the rail there is no "shut": opening a card SELECTS it (switching to its tab), and
      // a close request is a no-op so old automation that shuts every other card still works.
      if (!(open === '0' || open === 0 || open === false)) {
        var cc = body.__card && body.__card.__cfgCard;
        if (cc && cc.tab && pages[cc.tab] && cc.tab !== activeTab) { activeTab = cc.tab; applyTabs(); }
        body.__setOpen(true);
        // On the rail, opening a card means selecting its row.
        if (cc && cc.tab) cfgSelectCard(cc.tab, persistKey, false);
      }
      return persistKey + '=' + (body.__getOpen() ? 'open' : 'shut');
    };
    // A card's panels, for the onboarding tutorial's spotlight (Panorama/OnboardingGuideJs.h).
    // part: 'header' (the title row, default), 'rail' (its row in the left rail), 'test' (Weapon
    // Recoil's Test Shot button), 'pivot' (the hinge drawer of recoil / bob). Null when absent.
    api.cardPanel = function (persistKey, part) {
      var body = cfgSections[persistKey];
      var cc = body && body.__card && body.__card.__cfgCard;
      if (!cc) return null;
      if (part === 'rail') { var r = cfgRails[cc.tab]; return r && r.items[persistKey] ? r.items[persistKey].row : null; }
      if (part === 'test') return body.__cfgParts ? body.__cfgParts.test : null;
      if (part === 'pivot') {
        var owner = persistKey === 'config.section.weaponrecoil' ? 'recoil' : (persistKey === 'config.section.weaponbob' ? 'bob' : '');
        return owner && cfgPivotDrawers[owner] ? cfgPivotDrawers[owner].panel : null;
      }
      return cc.header || null;
    };
    // Select a card's sub-page by index, so a screenshot can reach every page of every card.
    api.cardPage = function (persistKey, index) {
      persistKey = evalArg(persistKey);
      var body = cfgSections[persistKey];
      if (!body || !body.__setPage) return 'unknown:' + persistKey;
      body.__setPage(index);
      return persistKey + '#' + index;
    };
    // Exercise the actual pass switch without OS input; callers wait for the native state mirror
    // before the next toggle. Confirmation dialogs still apply exactly as they do to a click.
    api.togglePass = function (persistKey) {
      persistKey = evalArg(persistKey);
      var body = cfgSections[persistKey];
      if (!body || !body.__togglePass) return 'unknown:' + persistKey;
      body.__togglePass();
      return persistKey;
    };
    // Editor Settings, drivable from `mirv_filmmaker config eval` the way the Camera Editor's
    // own gear is drivable through $.CamEditor.toggleSettings(). Synthetic mouse input does not
    // reach CS2, so without this the gear on the timeline band could only be tested by hand.
    api.editorSettings = function (want) {
      if (want === 'open') settingsPopup.open();
      else if (want === 'close') settingsPopup.close('eval');
      else settingsPopup.toggle();
      return settingsPopup.isOpen() ? 'open' : 'closed';
    };
    $.ConfigHud = api;
    $.Msg('[confighud] panel built.\n');
  } catch (e) {
    $.Msg('[confighud] build error: ' + e + '\n');
  }
})();
)CFJS";

} // namespace Filmmaker
